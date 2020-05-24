#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"

void swap_init (void);
void swap_read (void *vaddr, block_sector_t start_sector);
block_sector_t swap_write (void *vaddr);
void swap_free (block_sector_t start_sector);

#endif