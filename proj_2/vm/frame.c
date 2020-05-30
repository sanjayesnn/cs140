#include "vm/frame.h"

#include <list.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"

struct list frame_table;
struct frame_table_elem *clock_hand; /* Position of "hand" for clock alg. */
struct lock frame_table_lock;

struct frame_table_elem* ft_find_frame (void *kpage);
struct frame_table_elem* ft_find_frame_by_upage (void *upage);
void increment_clock_hand (void);
void ft_evict_page (void);

struct frame_table_elem
  {
    struct thread *holder;          /* Thread owning page in frame. */
    struct spt_elem *page_data;     /* SPT entry for page in frame. */
    void *kpage;                    /* Ptr to kernel vaddr for page. */
    struct list_elem elem;
  };

void
ft_init (void)
{
  list_init (&frame_table);
  lock_init (&frame_table_lock);
  clock_hand = NULL;
}

void
ft_destruct (void)
{
  for (struct list_elem *e = list_begin (&frame_table);
          e != list_end (&frame_table);
          e = list_next (e))
    {
      struct frame_table_elem *cur = list_entry (e,
                                                 struct frame_table_elem,
                                                 elem);
      list_remove (e);
      free (cur);
    }
}

void
increment_clock_hand (void)
{
  struct list_elem *e = &clock_hand->elem;
  if (e == list_back (&frame_table))
    clock_hand = list_entry (list_front (&frame_table),
                             struct frame_table_elem, elem);
  else 
    clock_hand = list_entry (list_next (e),
                             struct frame_table_elem, elem);
}

/*
 * Runs clock algorithm and evicts a page
 */
void
ft_evict_page (void) 
{
  ASSERT (clock_hand);
  lock_acquire (&frame_table_lock);
  while (true) 
  {
    /* Check if frame pointed to by clock hand is pinned. */
    if (clock_hand->page_data->is_pinned)
      {
        increment_clock_hand ();
        continue;
      }
    // Check clock_hand for reference and dirty bit
    void *upage = clock_hand->page_data->upage;
    struct thread *t = clock_hand->holder;
    uint32_t *pd = t->pagedir;
    if (pagedir_is_accessed (pd, upage)) 
      {
        pagedir_set_accessed (pd, upage, false);
      } 
    else 
      {
        // Evicting the page at clock hand
        void *kpage = clock_hand->kpage;
        struct spt_elem *spte = clock_hand->page_data;

        lock_acquire (&spte->spt_elem_lock);

        if (!pagedir_is_dirty (pd, upage))
          {
            /* Not modified, can just get rid of this page right now */
            spte->status = IN_FILESYS;
          }
        else 
          {
            /* Not accessed, is dirty */
            // TODO: make sure that the data is not being accessed right now
            block_sector_t new_swap_sector = swap_write (kpage);
            spte->swap_sector = new_swap_sector;
            spte->status = IN_SWAP;
          }

        lock_release (&spte->spt_elem_lock);
        palloc_free_page (kpage);
        pagedir_clear_page (pd, upage);

        // Removes the current frame table elem and increments clock hand
        struct frame_table_elem *clock_hand_cp = clock_hand;
        increment_clock_hand ();
        list_remove (&clock_hand_cp->elem);
        free (clock_hand_cp);
        lock_release (&frame_table_lock);
        return;
      }

    increment_clock_hand ();
  }
}

/*
 * Returns false if upage not found
 * TODO: check synchronization
 */
bool
vm_page_in (void *upage) 
{
  struct hash *spt = &thread_current ()->spt;
  struct spt_elem *page = spt_get_page (spt, upage);

  if (page == NULL) {
     // printf ("No page to page in at %x. Spt has %d elements.\n", upage, hash_size (spt));
     return false;
  }
  ASSERT (page->status != IN_MEMORY); // TODO: this could happen for synchronization reasons??

  /* Gets an empty frame */
  void *kpage = vm_get_frame (PAL_USER, upage, page->writable);

  if (page->status == IN_SWAP)
    {
      /* Fetches the page from swap */
      block_sector_t start_sector = page->swap_sector;
      vm_pin_frame (upage, false);
      swap_read (kpage, start_sector);
      vm_unpin_frame (upage);
    }
  else if (page->status == IN_FILESYS) 
    {
      size_t page_zero_bytes = page->zero_bytes;
      size_t page_read_bytes = PGSIZE - page_zero_bytes;
      struct file* file = page->file;
      if (file_read_at (
                  file, 
                  kpage, 
                  page_read_bytes, 
                  page->ofs) != (int) page_read_bytes)
        {
          // TODO: Figure out whether this should ever happen
          vm_free_frame (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes); 
    }


  /* Adds a mapping from upage to kpage */
  struct thread *t = thread_current ();
  pagedir_set_page (t->pagedir, upage, kpage, page->writable);

  /* Does bookkeeping */ 
  lock_acquire (&page->spt_elem_lock);
  page->status = IN_MEMORY;
  lock_release (&page->spt_elem_lock);

  // printf("Page in returning true, addr: %x\n", upage);
  return true;
}

bool
vm_add_stack_page ()
{
  struct thread *t = thread_current ();
  uint8_t *stack_end = t->stack_end;
  void *upage = stack_end - PGSIZE;
  void *kpage = vm_get_frame (PAL_USER | PAL_ZERO, upage, true);
  if (kpage == NULL) return false;
  t->stack_end = stack_end - PGSIZE;
  pagedir_set_page (t->pagedir, upage, kpage, true);
  return true; 
}


void *
vm_get_frame (enum palloc_flags flags, void *upage, bool writable) 
{
  void *kpage = palloc_get_page (flags);
  if (kpage == NULL)
    {
      ft_evict_page ();
      kpage = palloc_get_page (flags);
      if (kpage == NULL)
          PANIC ("No frame found even after evicting\n"); // TODO
    }
    
  struct thread *cur = thread_current ();
  
  if (spt_get_page (&cur->spt, upage) == NULL) 
    {
      /* Creates a new supplemental page table entry for this page */ 
      spt_add_page (&cur->spt, upage, writable, false);
      struct spt_elem *new_page = spt_get_page (&cur->spt, upage);
      if (new_page != NULL)
        new_page->file = NULL;
    }

  struct frame_table_elem *new_entry = 
        malloc (sizeof (struct frame_table_elem));
  new_entry->holder = cur;
  new_entry->page_data = spt_get_page (&cur->spt, upage);
  new_entry->kpage = kpage;
  lock_acquire (&frame_table_lock);
  list_push_back (&frame_table, &new_entry->elem);
  if (clock_hand == NULL)
    clock_hand = new_entry;
  lock_release (&frame_table_lock);
  // printf("VM get frame returning kpage: %x\n", kpage);
  return kpage;
}

struct frame_table_elem*
ft_find_frame (void *kpage)
{
  for (struct list_elem *e = list_begin (&frame_table);
          e != list_end (&frame_table);
          e = list_next (e))
    {
      struct frame_table_elem *cur = list_entry (e,
                                                 struct frame_table_elem,
                                                 elem);
      if (cur->kpage == kpage)
        return cur;
    }
  return NULL;
}

struct frame_table_elem*
ft_find_frame_by_upage (void *upage)
{
  for (struct list_elem *e = list_begin (&frame_table);
          e != list_end (&frame_table);
          e = list_next (e))
    {
      struct frame_table_elem *cur = list_entry (e,
                                                 struct frame_table_elem,
                                                 elem);
      if (cur->page_data->upage == upage)
        return cur;
    }
  return NULL;
}

void 
vm_free_frame (void *kpage) 
{
  lock_acquire (&frame_table_lock);
  struct frame_table_elem *fte = ft_find_frame (kpage);
  // printf("Frame table elem: %x\n", fte);
  if (fte == NULL)
    return;
  if (fte == clock_hand)
    increment_clock_hand();
  list_remove (&fte->elem);
  lock_release (&frame_table_lock);
  free (fte);
  palloc_free_page (kpage);
}

void
vm_pin_frame (void *upage, bool page_in)
{
  struct hash *spt = &thread_current ()->spt;
  struct spt_elem *page = spt_get_page (spt, upage);
  ASSERT (page != NULL);
  ASSERT (!page->is_pinned);
  lock_acquire (&page->spt_elem_lock);
  page->is_pinned = true;
  bool not_in_memory = (page->status != IN_MEMORY);
  lock_release (&page->spt_elem_lock);
  if (not_in_memory && page_in)
    vm_page_in (upage);
}

void
vm_unpin_frame (void *upage)
{
  struct frame_table_elem *fte = ft_find_frame_by_upage (upage);
  ASSERT (fte != NULL);
  ASSERT (fte->page_data->is_pinned);
  fte->page_data->is_pinned = false;
}

/*
 * First attempt at pinning: although it makes more sense to have pinned
 * bool in the frame struct, we end up with this weird edge case
 * race condition handled by while loop, which is kind of an ugly/
 * roundabout way to handle the race condition (in the event of thrashing
 * we could be stuck in the loop forever)
bool
vm_pin_frame (void *upage)
{
  struct hash *spt = &thread_current ()->spt;
  struct spt_elem *page = spt_get_page (spt, upage);
  lock_acquire (&frame_table_lock);
  while (page->status != IN_MEMORY)
    {
      lock_release (&frame_table_lock);
      vm_page_in (upage);
      lock_acquire (&frame_table_lock);
    }

  struct frame_table_elem *fte = ft_find_frame_by_upage (upage);
  ASSERT (fte != NULL);
  ASSERT (!fte->pinned);
  fte->pinned = true;
  lock_release (&frame_table_lock);
  return true;
}

bool
vm_unpin_frame (void *upage)
{
  lock_acquire (&frame_table_lock);
  struct frame_table_elem *fte = ft_find_frame (kpage);
  ASSERT (fte != NULL);
  ASSERT (fte->pinned);
  fte->pinned = false;
  lock_release (&frame_table_lock);
  return true;

}
*/
