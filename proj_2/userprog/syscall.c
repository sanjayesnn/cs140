#include <string.h>

#include "filesys/filesys.h"
#include "userprog/exception.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/pte.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "vm/page.h"

/* Each syscall argument takes up 4 bytes on the stack. */
#define ARG_SIZE 4
#define INPUT_FD 0
#define CONSOLE_FD 1
#define MAX_PUT_SIZE 200

typedef int mapid_t;

static void syscall_handler (struct intr_frame *);
static bool is_valid_string_memory (const void *vaddr);
static bool is_valid_memory_range (const void *vaddr, size_t size, bool is_writable);
struct file_data *get_file_with_fd (int fd);
struct mmap_file *get_mmap_file_with_mapping (mapid_t mapping);
static void *get_nth_syscall_arg (void *esp, int n);
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
mapid_t mmap (int fd, void *addr);
void munmap (mapid_t mapping);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  void *esp = f->esp;
  if (!is_valid_memory_range (esp, ARG_SIZE, false))
    exit (-1);

  int syscall = * (int *) esp;
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
      case SYS_MMAP : ;
        fd = * (int *) get_nth_syscall_arg (f->esp, 1);
        void *addr = * (void **) get_nth_syscall_arg (f->esp, 2);
        f->eax = mmap (fd, addr);
        break;
      case SYS_MUNMAP : ;
        mapid_t mapping = * (mapid_t *) get_nth_syscall_arg (f->esp, 1);
        munmap (mapping);
        break;
      default : ;
        exit (-1);
    }
}

void
halt (void)
{
  shutdown_power_off ();
}

void
exit (int status)
{
  struct thread *cur = thread_current ();
  
  char command[strlen(cur->name) + 1];
  strlcpy (command, cur->name, strlen(cur->name) + 1);
  const char tok[2] = " ";
  char* ptr;
  strtok_r (command, tok, &ptr);
  printf ("%s: exit(%d)\n", command, status < 0 ? -1 : status);
  
  lock_acquire (&cur->self_process_lock);
  if (cur->self_process != NULL) 
    cur->self_process->exit_status = status;

  lock_release (&cur->self_process_lock);
  thread_exit ();
}

pid_t
exec (const char *cmd_line)
{
  if (!is_valid_string_memory (cmd_line))
    exit (-1);

  char filename[strlen(cmd_line) + 1];
  strlcpy (filename, cmd_line, strlen(cmd_line) + 1);
  const char tok[2] = " ";
  char* ptr;
  strtok_r (filename, tok, &ptr);

  acquire_fs_lock ();
  struct file *file = filesys_open (filename);
  release_fs_lock ();
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", filename);
      return -1;
    }

  return process_execute(cmd_line);
}

int
wait (pid_t pid)
{
    return process_wait (pid);
}

bool
create (const char *file, unsigned initial_size)
{
  if (!is_valid_string_memory (file))
    exit (-1);

  acquire_fs_lock ();
  bool result = filesys_create (file, initial_size);
  release_fs_lock ();
  return result;
}

bool
remove (const char *file)
{
  if (!is_valid_string_memory (file))
    exit (-1);

  acquire_fs_lock ();
  bool result = filesys_remove (file);
  release_fs_lock ();
  return result;
}

/* Gets the file_data struct for a given file descriptor. */
struct file_data *
get_file_with_fd (int fd) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  for (e = list_begin (&cur->open_files); e != list_end (&cur->open_files);
       e = list_next (e)) 
    {
      struct file_data *fdata = list_entry (e, struct file_data, elem);
      if (fdata->fd == fd) 
          return fdata;
    }

  return NULL;
}

int
open (const char *file)
{
  if (!is_valid_string_memory (file))
    exit (-1);

  struct thread *cur = thread_current ();
  acquire_fs_lock ();
  struct file *f = filesys_open (file);
  release_fs_lock ();
  if (f == NULL)
    return -1;
  
  struct file_data *fdata = malloc (sizeof (struct file_data));
  fdata->file_ptr = f;

  /* Determine the file descriptor number. */
  if (list_empty (&cur->open_files))
    fdata->fd = 2;
  else
    {
      struct file_data *back = list_entry (list_back (&cur->open_files), 
                                          struct file_data, elem);
      fdata->fd = back->fd + 1;
    }

  list_push_back (&cur->open_files, &fdata->elem);
  return fdata->fd;
}

int
filesize (int fd)
{
  struct file_data *f = get_file_with_fd (fd);
  if (f == NULL) 
    return -1;

  acquire_fs_lock ();
  int result = file_length (f->file_ptr); 
  release_fs_lock ();
  return result;
}

int
read (int fd, void *buffer, unsigned size)
{
  if (!is_valid_memory_range (buffer, size, true))
    exit (-1);

  if (fd == INPUT_FD)
    {
      uint8_t *buf = (uint8_t *) buffer;
      int keys_read = 0;
      for (; (unsigned) keys_read < size; keys_read++)
        {
          uint8_t next_char = input_getc ();
          buf[keys_read] = next_char;
        }
      return keys_read;
    }
  else
    {
      struct file_data *f = get_file_with_fd (fd);
      if (f == NULL) 
        return -1;
      acquire_fs_lock ();
      int result = file_read (f->file_ptr, buffer, size); 
      release_fs_lock ();
      return result;
    }
}

int
write (int fd, const void *buffer, unsigned size)
{
  if (!is_valid_memory_range (buffer, size, false))
    exit (-1);

  if (fd == CONSOLE_FD)
    {
      for (unsigned start = 0; start < size; start += MAX_PUT_SIZE)
        {
          putbuf (buffer + start,
                  (MAX_PUT_SIZE < size - start) ? MAX_PUT_SIZE : size - start);
        }
      return size;
    }
  else
    {
      struct file_data *f = get_file_with_fd (fd);
      if (f == NULL) 
        return 0; 
      acquire_fs_lock ();
      int result = file_write (f->file_ptr, buffer, size);
      release_fs_lock ();
      return result;
    }
}

void
seek (int fd, unsigned position)
{
  struct file_data *f = get_file_with_fd (fd);
  if (f == NULL) 
    return;

  acquire_fs_lock ();
  file_seek (f->file_ptr, position);
  release_fs_lock ();
}

unsigned
tell (int fd)
{
  struct file_data *f = get_file_with_fd (fd);
  if (f == NULL) 
    return 0;

  acquire_fs_lock ();
  unsigned result = file_tell (f->file_ptr);
  release_fs_lock ();
  return result;
}

void
close (int fd)
{
  struct file_data *f = get_file_with_fd (fd);
  if (f == NULL)
    return;
  
  acquire_fs_lock ();
  file_close (f->file_ptr);
  release_fs_lock ();
  list_remove (&f->elem);
  free (f);
}

mapid_t
mmap (int fd, void *addr)
{
  // TODO: figure out how to validate memory

  if (addr == 0x0 || pg_ofs (addr) != 0)
    return -1;

  struct file_data *f = get_file_with_fd (fd);
  int file_len = file_length (f->file_ptr);
  if (f == NULL || file_len <= 0) 
    return -1;

  /* Ensure memory doesn't overlaps any existing set of mapped pages. */
  void *upage = addr;
  struct thread *cur = thread_current ();
  while ((char *) upage < (char *) addr + file_len)
    {
      if (upage == NULL || !is_user_vaddr (upage))
        return -1;
      struct spt_elem *spte = spt_get_page (&cur->spt, upage);
      /* Page already mapped. */
      if (spte != NULL)
        return -1;
      
      upage = (char *) upage + PGSIZE;
    }

  struct file *reopened_file = file_reopen (f->file_ptr);

  if (reopened_file == NULL)
    return -1;

  struct mmap_file *mf = malloc (sizeof (struct mmap_file));
  if (mf == NULL)
    return -1;

  mf->map_id = cur->next_mapping_id;
  cur->next_mapping_id++;
  mf->file = reopened_file;
  mf->upage = addr;

  /* Create entries in supplementary page table. */
  off_t ofs = 0;
  int num_pages = 0;
  uint32_t zero_bytes = 0;
  for (ofs = 0; ofs < file_len; ofs += PGSIZE)
    {
      bool writable = is_file_writable (f->file_ptr);
      zero_bytes = (ofs + PGSIZE >= file_len) ? PGSIZE + ofs - file_len : 0;
      
      spt_add_page (&cur->spt, (char *)addr + ofs, writable, true);
      struct spt_elem *new_page = spt_get_page (&cur->spt, upage);
      if (new_page == NULL)
        return -1;
      new_page->zero_bytes = PGSIZE - zero_bytes;
      new_page->file = reopened_file;
      new_page->ofs = ofs;

      num_pages++;
    }

  mf->num_pages = num_pages;
  list_push_back (&cur->mmap_list, &mf->elem);
  return mf->map_id;
}

struct mmap_file *
get_mmap_file_with_mapping (mapid_t mapping)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for (e = list_begin (&cur->mmap_list); e != list_end (&cur->mmap_list);
       e = list_next (e)) 
    {
      struct mmap_file *mf = list_entry (e, struct mmap_file, elem);
      if (mf->map_id == mapping) 
          return mf;
    }
  
  return NULL;
}

/* TODO: Synchronize data structure access. */
void
munmap (mapid_t mapping)
{
  struct mmap_file *mf = get_mmap_file_with_mapping (mapping);
  if (mf == NULL)
    return;
  
  list_remove (&mf->elem);

  struct thread *cur = thread_current();
  size_t num_pages = mf->num_pages;
  for (size_t page_num = 0; page_num < num_pages; page_num++)
    {
      void *addr = (char *)mf->upage + page_num * PGSIZE;
      struct spt_elem *spte = spt_get_page (&cur->spt, addr);
      if (spte != NULL)
        vm_free_page (spte);;
    }
  
  free (mf);
}

/* Determines whether the supplied pointer references a valid string. */
static bool
is_valid_string_memory (const void *vaddr) 
{
  if (vaddr == NULL)
    return false;

  /* Start at the beginning of the page. */
  void *upage = pg_round_down (vaddr);
  char *cur = (char *) vaddr;

  /* Check every page in the given range. */
  while (true)
    {
      if (upage == NULL || !is_user_vaddr (upage))
        return false;

      uint32_t *pte = lookup_page (thread_current ()->pagedir, upage, false);
      if (pte == NULL || (*pte & PTE_P) == 0)
        return false;
      
      while (cur < (char *) upage + PGSIZE) 
        {
          if (*cur == '\0')
            return true;
          
          cur++;
        }
      upage = (char *) upage + PGSIZE;
    }
  
  return true;
}

/* Determines whether the supplied pointer references valid user memory. */
static bool
is_valid_memory_range (const void *vaddr, size_t size, bool is_writable) 
{
  if (vaddr == NULL)
    return false;

  /* Start at the beginning of the page. */
  void *upage = pg_round_down (vaddr);
  /* Check every page in the given range. */
  while ((char *) upage < (char *) vaddr + size)
    {
      if (upage == NULL || !is_user_vaddr (upage))
        return false;

      uint32_t *pte = lookup_page (thread_current ()->pagedir, upage, false);
      if (pte == NULL || (*pte & PTE_P) == 0)
        return false;
      /* Page is writable. */
      if (is_writable && (*pte & PTE_W) == 0)
        return false;
      
      upage = (char *) upage + PGSIZE;
    }
  
  return true;
}

/* Returns a pointer to the n-th argument of a syscall. Args are 1-indexed */
static void*
get_nth_syscall_arg (void *esp, int n)
{
  if (!is_valid_memory_range ((char *) esp + ARG_SIZE * n, ARG_SIZE, false))
    exit (-1);

  return (char *) esp + ARG_SIZE * n;
}
