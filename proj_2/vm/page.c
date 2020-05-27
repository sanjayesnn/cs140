#include "vm/page.h"

#include <stdio.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"


unsigned hash_func (const struct hash_elem *e, void *aux);
bool hash_less (const struct hash_elem *a,
                const struct hash_elem *b,
                void *aux);
void hash_free_elem (struct hash_elem *e, void *aux);


unsigned
hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct spt_elem *spt_entry = hash_entry (e, struct spt_elem, elem);
  void *upage = spt_entry->upage;
  return hash_bytes (&upage, sizeof (upage));
}

bool
hash_less (const struct hash_elem *a,
           const struct hash_elem *b,
           void *aux UNUSED)
{
  struct spt_elem *a_entry = hash_entry (a, struct spt_elem, elem);
  struct spt_elem *b_entry = hash_entry (b, struct spt_elem, elem);
  return a_entry->upage < b_entry->upage;
}

void
hash_free_elem (struct hash_elem *e, void *aux UNUSED)
{
  free (hash_entry (e, struct spt_elem, elem));
}

void
spt_init (struct hash *spt)
{
  hash_init (spt, hash_func, hash_less, NULL);
}

void
spt_add_page (struct hash *spt, void *upage, bool writable, bool lazy)
{
  struct spt_elem *entry = malloc (sizeof (struct spt_elem));
  entry->status = lazy ? IN_FILESYS : IN_MEMORY;
  lock_init (&entry->spt_elem_lock);
  entry->upage = upage;
  entry->writable = writable;
  hash_insert (spt, &entry->elem);
}

void
spt_remove_page (struct hash *spt, struct spt_elem *entry)
{
  hash_delete (spt, &entry->elem);
  free (entry);
}

void
spt_free (struct hash *spt)
{
  hash_destroy (spt, hash_free_elem);
}

/*
 * Returns the page element corresponding to upage
 * Returns NULL if nothing found
 */
struct spt_elem*
spt_get_page (struct hash *spt, void *upage)
{
  struct spt_elem mock_entry;
  mock_entry.upage = upage;
  struct hash_elem *res = hash_find (spt, &mock_entry.elem);
  if (res == NULL) return NULL;
  return hash_entry (res,
                     struct spt_elem,
                     elem);
}

/*
 * Frees memory associated with the given supplemental page table entry.
 */ 
void 
vm_free_page (struct spt_elem *spte)
{
  struct thread *cur = thread_current ();
  void *kpage = pagedir_get_page (cur->pagedir, spte->upage);

  /* Write file to disk if necessary. */
  if (spte->file != NULL && 
      spte->writable && 
      is_file_writable (spte->file) &&
      pagedir_is_dirty (cur->pagedir, spte->upage))
    {
      off_t write_size = PGSIZE - spte->zero_bytes;
      file_write_at (spte->file, kpage, write_size, spte->ofs);
    } 

  pagedir_clear_page (cur->pagedir, spte->upage);
  vm_free_frame (kpage);
}

