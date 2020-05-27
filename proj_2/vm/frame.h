  
#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdbool.h>
#include "threads/palloc.h"

void ft_init (void);
void ft_destruct (void);

bool vm_page_in (void *upage);
bool vm_add_stack_page (void);
void *vm_get_frame (enum palloc_flags, void *upage, bool writable);
void vm_free_frame (void *kpage);

#endif
