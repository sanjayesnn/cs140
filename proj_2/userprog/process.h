#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/synch.h"
#include <list.h>
#include <stdbool.h>

typedef int tid_t;
typedef int pid_t;

struct process
  {
    pid_t pid;
    bool loaded;
    int exit_status;
    struct semaphore exit_sema;
    struct thread *self_thread;
    struct list_elem elem; 
  };

bool install_page (void *upage, void *kpage, bool writable);

tid_t process_execute (const char *cmdline);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
void process_free_children (void);

#endif /* userprog/process.h */
