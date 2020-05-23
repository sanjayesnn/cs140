#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "threads/synch.h"

enum page_status
  {
    IN_MEMORY,
    IN_FILESYS,
    IN_SWAP
  };

struct spt_elem
  {
    enum page_status status;
    void *vaddr;
    void *location;
    struct lock spt_elem_lock;
    struct hash_elem elem;
  };

void spt_init (struct hash* spt);
void spt_add_page (struct hash* spt, void *vaddr);
struct spt_elem* spt_get_page (struct hash* spt, void *vaddr);

void spt_remove_page (struct hash *spt, struct spt_elem *entry);
void spt_free (struct hash *spt);


#endif
