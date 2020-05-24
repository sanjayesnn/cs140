  
#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"

void ft_init (void);
void ft_destruct (void);

void *vm_get_frame (enum palloc_flags);
void vm_free_frame (void *page);

#endif
