#include <bitmap.h>
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "vm/swap.h"

const int NUM_BLOCKS_IN_PAGE = PGSIZE / BLOCK_SECTOR_SIZE;

struct block *swap_partition;
struct bitmap *swap_table;
struct lock swap_lock;

void
swap_init (void)
{
  swap_partition = block_get_role (BLOCK_SWAP);
  if (swap_partition == NULL)
    PANIC ("Swap partition not found.");
    
  swap_table = bitmap_create (block_size (swap_partition));
  if (swap_table == NULL)
    PANIC ("Cannot initialize swap partition.");

  lock_init (&swap_lock);
}

/**
 * Reads page from swap into vaddr.
 */
void
swap_read (void *vaddr, block_sector_t start_sector)
{
  for (int i = 0; i < NUM_BLOCKS_IN_PAGE; i++)
    {
        block_read (swap_partition, 
                    start_sector + i, 
                    (char *)vaddr + i * BLOCK_SECTOR_SIZE);
    }

  lock_acquire (&swap_lock);
  bitmap_set_multiple (swap_table, start_sector, NUM_BLOCKS_IN_PAGE, false);
  lock_release (&swap_lock);
}

/**
 * Writes a page from vaddr into the swap partition. 
 * Returns the start sector of the page's location in swap.
 */
block_sector_t
swap_write (void *vaddr)
{
  lock_acquire (&swap_lock);
  block_sector_t start_sector = bitmap_scan_and_flip (swap_table, 
                                                        0, 
                                                        NUM_BLOCKS_IN_PAGE, 
                                                        false);
  lock_release (&swap_lock);

  if (start_sector == BITMAP_ERROR)
    PANIC ("Swap partition is full");

  for (int i = 0; i < NUM_BLOCKS_IN_PAGE; i++)
    {
        block_write (swap_partition, 
                    start_sector + i, 
                    (char *)vaddr + i * BLOCK_SECTOR_SIZE);
    }

  return start_sector;
}

void
swap_free (block_sector_t start_sector)
{
  lock_acquire (&swap_lock);
  bitmap_set_multiple (swap_table, start_sector, NUM_BLOCKS_IN_PAGE, false);
  lock_release (&swap_lock);
}
