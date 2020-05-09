#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/pte.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* Each syscall argument takes up 4 bytes on the stack. */
#define ARG_SIZE 4

typedef int pid_t;

static void syscall_handler (struct intr_frame *);
static bool is_valid_ptr (const void *vaddr);
static bool is_writable_ptr (const void *vaddr);
static void* get_nth_syscall_arg (void *esp, int n);
static void call_syscall (struct intr_frame *f, int syscall);

void halt (void);
void exit (int status);
pid_t exec (const char *cmd_line);
int wait (pid_t pid);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned size);
int write (int fd, const void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");
  int syscall = * (int *) f->esp;
  call_syscall (f, syscall);
}

static void
call_syscall (struct intr_frame *f, int syscall)
{
  switch (syscall)
    {
      case SYS_HALT :
        halt ();
        break;
      case SYS_EXIT : ;
        int status = * (int *) get_nth_syscall_arg (f->esp, 1);
        exit (status);
        break;
      case SYS_EXEC : ;
        char *cmd_line = * (char **) get_nth_syscall_arg (f->esp, 1);
        f->eax = exec (cmd_line);
        break;
      case SYS_WAIT : ;
        pid_t pid = * (pid_t *) get_nth_syscall_arg (f->esp, 1);
        f->eax = wait (pid);
        break;
      case SYS_CREATE : ;
        char *file = * (char **) get_nth_syscall_arg (f->esp, 1);
        unsigned init_size = * (unsigned *) get_nth_syscall_arg (f->esp, 2);
        f->eax = create (file, init_size);
        break;
      case SYS_REMOVE : ;
        file = * (char **) get_nth_syscall_arg (f->esp, 1);
        f->eax = remove (file);
        break;
      case SYS_OPEN : ;
        file = * (char **) get_nth_syscall_arg (f->esp, 1);
        f->eax = open (file);
        break;
      case SYS_FILESIZE : ;
        int fd = * (int *) get_nth_syscall_arg (f->esp, 1);
        f->eax = filesize (fd);
        break;
      case SYS_READ : ;
        fd = * (int *) get_nth_syscall_arg (f->esp, 1);
        void *buffer = * (void **) get_nth_syscall_arg (f->esp, 2);
        unsigned size = * (unsigned *) get_nth_syscall_arg (f->esp, 3);
        f->eax = read (fd, buffer, size);
        break;
      case SYS_WRITE : ;
        fd = * (int *) get_nth_syscall_arg (f->esp, 1);
        buffer = * (void **) get_nth_syscall_arg (f->esp, 2);
        size = * (unsigned *) get_nth_syscall_arg (f->esp, 3);
        f->eax = write (fd, buffer, size);
        break;
      case SYS_SEEK : ;
        fd = * (int *) get_nth_syscall_arg (f->esp, 1);
        unsigned position = * (unsigned *) get_nth_syscall_arg (f->esp, 2);
        seek (fd, position);
        break;
      case SYS_TELL : ;
        fd = * (int *) get_nth_syscall_arg (f->esp, 1);
        f->eax = tell (fd);
        break;
      case SYS_CLOSE : ;
        fd = * (int *) get_nth_syscall_arg (f->esp, 1);
        close (fd);
        break;
      default : ;
        /* TODO: add error handling for invalid syscall # */
    }
}

void
halt (void)
{
}

void
exit (int status)
{
  process_exit ();
  thread_exit ();
}

pid_t
exec (const char *cmd_line)
{
  return -1;
}

int
wait (pid_t pid)
{
  return -1;
}

bool
create (const char *file, unsigned initial_size)
{
 return false;
}

bool
remove (const char *file)
{
  return false;
}

int
open (const char *file)
{
  return -1;
}

int
filesize (int fd)
{
  return -1;
}

int
read (int fd, void *buffer, unsigned size)
{
  return -1;
}

int
write (int fd, const void *buffer, unsigned size)
{
  return -1;
}

void
seek (int fd, unsigned position)
{
}

unsigned
tell (int fd)
{
  return 0;
}

void
close (int fd)
{
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

/* Returns a pointer to the n-th argument of a syscall. Args are 1-indexed */
static void*
get_nth_syscall_arg (void *esp, int n)
{
  return esp + ARG_SIZE * n;
}