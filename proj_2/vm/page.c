#include "vm/page.h"
#include "threads/malloc.h"


unsigned hash_func (const struct hash_elem *e, void *aux);
bool hash_less (const struct hash_elem *a,
                const struct hash_elem *b,
                void *aux);
void hash_free_elem (struct hash_elem *e, void *aux);


unsigned
hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct spt_elem *spt_entry = hash_entry (e, struct spt_elem, elem);
  void *vaddr = spt_entry->vaddr;
  return hash_bytes (&vaddr, sizeof (vaddr));
}

bool
hash_less (const struct hash_elem *a,
           const struct hash_elem *b,
           void *aux UNUSED)
{
  struct spt_elem *a_entry = hash_entry (a, struct spt_elem, elem);
  struct spt_elem *b_entry = hash_entry (b, struct spt_elem, elem);
  return a_entry->vaddr < b_entry->vaddr;
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
spt_add_page (struct hash *spt, void *vaddr)
{
  struct spt_elem *entry = malloc (sizeof (struct spt_elem));
  entry->status = IN_MEMORY;
  lock_init (&entry->spt_elem_lock);
  entry->vaddr = vaddr;
  entry->location = vaddr;
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

struct spt_elem*
spt_get_page (struct hash *spt, void *vaddr)
{
  struct spt_elem mock_entry;
  mock_entry.vaddr = vaddr;
  return hash_entry (hash_find (spt, &mock_entry.elem),
                     struct spt_elem,
                     elem);
}

