#include "vm/frame.h"

#include "threads/palloc.h"

void *
vm_get_frame (enum palloc_flags flags) 
{
  void *page = palloc_get_page (flags);
  if (page == NULL) {
      return NULL;
  }
  return page;
}


void 
vm_free_frame (void *page) 
{
  palloc_free_page (page);
}
