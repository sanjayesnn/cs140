#include "vm/frame.h"

#include <list.h>
#include <stdio.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "userprog/process.h" /* For install_page */
#include "vm/page.h"
#include "vm/swap.h"

struct list frame_table;
struct frame_table_elem *clock_hand; /* Position of "hand" for clock alg. */
struct lock frame_table_lock;

struct frame_table_elem* ft_find_frame (void *upage);
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
    printf("Incrementing clock hand\n");
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
  printf("Evicting a page\n");
  ASSERT (clock_hand);
  while (true) 
  {
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
        if (!pagedir_is_dirty (pd, upage))
          {
            PANIC ("Removing unmodified page failed"); // TODO: implement file mapping
          }
        else 
          {
            /* Not accessed, is dirty */
            void *kpage = clock_hand->kpage;

            struct spt_elem *spte = clock_hand->page_data;
            /* Writes data to swap */
            // TODO: make sure that the data is not being accessed right now
            block_sector_t new_swap_sector = swap_write (kpage);
            
            lock_acquire (&spte->spt_elem_lock);
            spte->swap_sector = new_swap_sector;
            spte->status = IN_SWAP;
            lock_release (&spte->spt_elem_lock);
            palloc_free_page (kpage);
          }
        // Removes the current frame table elem and increments clock hand
        struct frame_table_elem *clock_hand_cp = clock_hand;
        increment_clock_hand ();
        list_remove (&clock_hand_cp->elem);
        free (clock_hand_cp);
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
     printf ("No page to page in\n");
     return false;
  }
  if (page->status != IN_SWAP) PANIC ("Page not in swap\n"); // TODO

  /* Gets an empty frame */
  void *kpage = vm_get_frame (PAL_USER, upage, page->writable);

  /* Fetches the page from swap */
  block_sector_t start_sector = page->swap_sector;
  swap_read (kpage, start_sector);

  /* Adds a mapping from upage to kpage */
  install_page (upage, kpage, page->writable);

  /* Does bookkeeping */ 
  lock_acquire (&page->spt_elem_lock);
  page->status = IN_MEMORY;
  lock_release (&page->spt_elem_lock);
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
  
      spt_add_page (&cur->spt, upage, writable);
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
    }
  return kpage;
}

struct frame_table_elem*
ft_find_frame (void *upage)
{
  for (struct list_elem *e = list_begin (&frame_table);
          e != list_end (&frame_table);
          e = list_next (e))
    {
      struct frame_table_elem *cur = list_entry (e,
                                                 struct frame_table_elem,
                                                 elem);
      if (cur->page_data->upage == upage)
        return upage;
    }
  return NULL;
}

void 
vm_free_frame (void *kpage) 
{
  struct frame_table_elem *fte = ft_find_frame (kpage);
  lock_acquire (&frame_table_lock);
  list_remove (&fte->elem);
  lock_release (&frame_table_lock);
  struct thread* cur = thread_current ();
  spt_remove_page (&cur->spt, fte->page_data);
  free (fte);
  palloc_free_page (kpage);
}
