#include "vm/frame.h"

#include "threads/palloc.h"
#include "threads/thread.h"
#include "vm/page.h"

void *
vm_get_frame (enum palloc_flags flags) 
{
  void *page = palloc_get_page (flags);
  if (page == NULL) {
      return NULL;
  }

  spt_add_page (&thread_current ()->spt, page);
  return page;
}


void 
vm_free_frame (void *page) 
{
  struct thread* cur = thread_current ();
  spt_remove_page (&cur->spt, spt_get_page (&cur->spt, page));
  palloc_free_page (page);
}
