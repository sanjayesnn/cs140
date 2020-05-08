#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/pte.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static void syscall_handler (struct intr_frame *);
static bool is_valid_ptr (const void *vaddr);
static bool is_writable_ptr (const void *vaddr);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");
  thread_exit ();
}

/* Determines whether the supplied pointer references valid user memory. */
static bool
is_valid_ptr (const void *vaddr) 
{
  if (vaddr == NULL || !is_user_vaddr (vaddr))
      return false;
  
  uint32_t *pte = lookup_page (thread_current ()->pagedir, vaddr, false);
  if ((*pte & PTE_P) == 0 || (*pte & PTE_U) == 0)
    return false;
  
  return true;
}

/* Determines whether the supplied pointer references writable user memory. */
static bool
is_writable_ptr (const void *vaddr) 
{
  if (!is_valid_ptr (vaddr))
    return false;

  uint32_t *pte = lookup_page (thread_current ()->pagedir, vaddr, false);
  if ((*pte & PTE_W) == 0)
    return false;
}