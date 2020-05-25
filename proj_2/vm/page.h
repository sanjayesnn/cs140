#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <stdbool.h>
#include "filesys/file.h"

#include "threads/synch.h"
#include "devices/block.h"

enum page_status
  {
    IN_MEMORY,
    IN_FILESYS,
    IN_SWAP
  };

struct spt_elem
  {
    enum page_status status;
    void *upage;
    block_sector_t swap_sector;
    struct lock spt_elem_lock;
    struct hash_elem elem;
    bool writable;

    off_t ofs;
    uint32_t zero_bytes;
    struct file *file;
  };

void spt_init (struct hash* spt);
void spt_add_page (struct hash* spt, void *upage, bool writable);
struct spt_elem* spt_get_page (struct hash* spt, void *upage);

void spt_remove_page (struct hash *spt, struct spt_elem *entry);
void spt_free (struct hash *spt);


#endif
