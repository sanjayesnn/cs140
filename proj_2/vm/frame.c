#include "vm/frame.h"

#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/page.h"

struct list frame_table;
struct frame_table_elem *clock_hand; /* Position of "hand" for clock alg. */
struct lock frame_table_lock;

struct frame_table_entry* ft_find_frame (void *upage);


struct frame_table_elem
  {
    struct thread *holder;          /* Thread owning page in frame. */
    struct spt_elem *page_data;     /* SPT entry for page in frame. */
    void *upage;                    /* Ptr to user vaddr for page in frame. */
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
  for (list_elem *e = list_begin (&frame_table);
          e != list_end (&frame_table);
          e = list_next (&frame_table, e))
    {
      struct frame_table_elem *cur = list_entry (e,
                                                 struct frame_table_elem,
                                                 elem);
      list_remove (&frame_table, &cur);
      free (cur);
    }
}

void *
vm_get_frame (enum palloc_flags flags) 
{
  void *page = palloc_get_page (flags);
  if (page == NULL)
    {
      return NULL;
    } else
    {
      struct thread *cur = thread_current ();
      spt_add_page (&cur->spt, page);
      struct frame_table_elem *new_entry = 
          malloc (sizeof (struct frame_table_elem));
      new_entry->holder = cur;
      new_entry->page_data = spt_get_page (&cur->spt, page);
      new_entry->upage = NULL; //TODO - how to calculate this?
      new_entry->kpage = page;
      lock_acquire (&frame_table_lock);
      list_push_back (&frame_table, &new_entry->elem);
      if (clock_hand == NULL)
        clock_hand = new_entry;
      lock_release (&frame_table_lock);
    }
  return page;
}

struct frame_table_entry*
ft_find_frame (void *upage)
{
  for (list_elem *e = list_begin (&frame_table);
          e != list_end (&frame_table);
          e = list_next (&frame_table, e))
    {
      struct frame_table_elem *cur = list_entry (e,
                                                 struct frame_table_elem,
                                                 elem);
      if (cur->upage == upage)
        return upage;
    }
  return NULL;
}

void 
vm_free_frame (void *page) 
{
  struct frame_table_entry *fte = ft_find_frame (page);
  lock_acquire (&frame_table_lock);
  list_remove (&frame_table, &fte->elem);
  lock_release (&frame_table_lock);
  struct thread* cur = thread_current ();
  spt_remove_page (&cur->spt, fte->page_data);
  free (fte);
  palloc_free_page (page);
}
