#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "devices/timer.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. Only used 
   if thread_mlfqs is false */
static struct list ready_list;

/* List of ready lists for each priority level. Only used if 
   thread_mlfqs is true. pri_queues[i] correspends to the
   ready queue for threads with priority i. */
static struct list pri_queues[PRI_MAX + 1];

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */


/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
#define PRI_UPDATE_INTERVAL 4
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

static fixed_point_t load_avg;

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
void update_priority (struct thread *t, void *aux);
int num_ready_threads (void);
void add_to_ready_queue (struct thread *t);
struct thread *highest_priority_thread (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&all_list);
  if (thread_mlfqs) 
    {
      for (int i = 0; i <= PRI_MAX; i++)
        {
          list_init(&pri_queues[i]);
        }
    } 
  else
    list_init (&ready_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  load_avg = fix_int (0);
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Computes system load average using moving average. */
void
calculate_load_avg (void)
{
  int ready_threads = num_ready_threads ();
  if (thread_current () != idle_thread) 
    ready_threads ++;

  fixed_point_t coeff1 = fix_div (fix_int (59), fix_int (60));
  fixed_point_t coeff2 = fix_div (fix_int (1), fix_int (60));

  load_avg = fix_mul (coeff1, load_avg); 
  fixed_point_t prod2 = fix_scale (coeff2, ready_threads);
  load_avg = fix_add (load_avg, prod2);
} 

/* Computes recent cpu usage for a thread using moving average. */
void
calculate_recent_cpu (struct thread *t, void *aux)
{
  (void) aux;
  fixed_point_t a = fix_scale (load_avg, 2);
  fixed_point_t b = fix_add (a, fix_int (1));
  fixed_point_t coeff = fix_div (a, b);

  fixed_point_t prod = fix_mul (coeff, t->recent_cpu);
  t->recent_cpu = fix_add (prod, fix_int (t->nice));
}

/* Recalculates and updates priority for struct thread *t. Does not
   switch running thread. */
void
update_priority (struct thread *t, void *aux)
{
  (void) aux;
  fixed_point_t recent_cpu_term = fix_unscale (t->recent_cpu, 4);
  fixed_point_t nice_term = fix_scale (fix_int (t->nice), 2);
  fixed_point_t tmp = fix_sub (fix_int(PRI_MAX), recent_cpu_term);
  fixed_point_t unrounded_priority = fix_sub (tmp, nice_term);

  int priority = fix_trunc (unrounded_priority);
  if (priority > PRI_MAX)
    priority = PRI_MAX;
  else if (priority < PRI_MIN)
    priority = PRI_MIN;

  t->priority = priority;
}


/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;

#ifdef USERPROG
  else if (t->pagedir != NULL) 
    {
      t->recent_cpu = fix_add (t->recent_cpu, fix_int (1));
      user_ticks++;
    }
#endif
  else 
    {
      t->recent_cpu = fix_add (t->recent_cpu, fix_int (1));
      kernel_ticks++;
    }

  if (thread_mlfqs && timer_ticks () % TIMER_FREQ == 0) 
    {
      calculate_load_avg ();
      thread_foreach (calculate_recent_cpu, NULL);
    }

  if (thread_mlfqs && timer_ticks () % PRI_UPDATE_INTERVAL == 0)
    thread_foreach(update_priority, NULL);

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Comparison function to order threads by priority. */
bool
thread_priority_compare (const struct list_elem *a,
                          const struct list_elem *b,
                          void *aux) 
{
    (void) aux;
    struct thread *a_entry = list_entry (a, struct thread, elem);
    struct thread *b_entry = list_entry(b, struct thread, elem);
    return a_entry->priority > b_entry->priority;
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);
  
  if (thread_current ()->priority < t->priority)
    thread_yield ();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);

  add_to_ready_queue (t);

  t->status = THREAD_READY;

  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  
  ASSERT (!intr_context ());

  enum intr_level old_level = intr_disable ();
  if (cur != idle_thread) 
    {
      add_to_ready_queue (cur);
    }
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Returns the maximum priority of all threads waiting 
   for a lock held by the input thread. */
int
max_waiter_priority (struct thread *t) 
{
  int max_priority = PRI_MIN;
  struct list_elem *e;
  /* Loop over locks held by thread t */
  for (e = list_begin (&t->acquired_locks); 
        e != list_end (&t->acquired_locks); 
        e = list_next (e)) 
    {
      struct lock *l = list_entry (e, struct lock, lock_elem);
      /* Find the max priority waiter */
      if (!list_empty (&l->semaphore.waiters)) 
        {
          struct list_elem *max_pri_elem = list_min (&l->semaphore.waiters,
                                                    thread_priority_compare,
                                                    NULL);
          struct thread *max_pri_thread = list_entry (max_pri_elem, 
                                                      struct thread, 
                                                      elem);
          if (max_pri_thread->priority > max_priority)
            max_priority = max_pri_thread->priority; 
        } 
    }

    return max_priority;
}

/* Sets the current thread's priority to NEW_PRIORITY.
 * Does NOT override potential donated priorities */
void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs)
    return;

  /* Avoid race condition with modifying thread struct */
  enum intr_level old_level = intr_disable ();
  struct thread *cur_thread = thread_current ();
  
  bool should_yield = false;

  int orig_priority = cur_thread->original_priority;
  int cur_priority = cur_thread->priority;

  if (orig_priority == new_priority) 
    {
        /* No change */
        intr_set_level (old_level);
        return;
    }

  if (cur_priority < new_priority) 
    {
        /* Increasing priority */
        cur_thread->original_priority = new_priority;
        cur_thread->priority = new_priority;
    } 
  else if (orig_priority != cur_priority) 
    {
        /* Thread has a donated priority, trying to decrease priority*/
        cur_thread->original_priority = new_priority;
    } 
  else 
    {
        /* No donated priority, trying to decrease priority */
        cur_thread->original_priority = new_priority;
        int max_priority = max_waiter_priority (cur_thread);
        new_priority = (max_priority > new_priority)
                        ? max_priority : new_priority;
        cur_thread->priority = new_priority;
    }

  /* Yields if needed*/
  struct thread *max_thread = highest_priority_thread ();
  if (max_thread->priority > cur_thread->priority)
    should_yield = true;
  intr_set_level (old_level);
  if (should_yield)
    thread_yield();
}

/* Change the priority of any thread, not necessarily the currently running
 * thread. Will override donated priority if thread is not running. */
void
set_priority (struct thread *t, int new_priority)
{
  enum intr_level old_level = intr_disable ();
  if (t->status == THREAD_RUNNING) 
    {
      thread_set_priority (new_priority);
    } 
  else if (t->status == THREAD_BLOCKED) 
    {
      t->priority = new_priority;
    } 
  else if (t->status == THREAD_READY) 
    {
      list_remove(&t->elem); //check this
      t->priority = new_priority;
      add_to_ready_queue (t);
    }
  intr_set_level (old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int new_nice) 
{
  enum intr_level old_level = intr_disable ();

  bool should_yield = false;
  struct thread *cur = thread_current ();
  cur->nice = new_nice;
  update_priority (cur, NULL);

  struct thread *max_pri = highest_priority_thread ();
  if (cur->priority < max_pri->priority)
    should_yield = true;
  
  intr_set_level (old_level);
  if (should_yield)
    thread_yield();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return fix_round (fix_scale (load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return fix_round (fix_scale (thread_current ()->recent_cpu, 100));
}

/* Returns total number of all ready threads at all priority levels */
int
num_ready_threads (void)
{
  if (!thread_mlfqs)
    return list_size (&ready_list);

  int total_size = 0;
  for (int i = 0; i < PRI_MAX; i++) 
    {
      total_size += list_size (&pri_queues[i]);
    }
  return total_size;
}

/* Adds t to the proper ready list, in the correct order, based on
   whether we are in thread_mlfqs mode or not */
void
add_to_ready_queue (struct thread *t)
{
  if (thread_mlfqs)
    list_push_back (&pri_queues[t->priority], &t->elem);
  else
    list_insert_ordered (&ready_list,
                          &t->elem,
                          thread_priority_compare,
                          NULL);
}

/* Returns the current highest priority thread, without removing the thread */
struct thread *
highest_priority_thread (void)
{
  if (thread_mlfqs) 
    {
      for (int i = PRI_MAX; i >= 0; i--)
        {
          if (!list_empty (&pri_queues[i])) 
            {
              return list_entry (list_front (&pri_queues[i]),
                                  struct thread,
                                  elem);
            }
        }
      return idle_thread;
    } 
  else 
    {
      if (!list_empty (&ready_list)) 
        {
          return list_entry (list_min (&ready_list,
                              thread_priority_compare,
                               NULL),
                            struct thread,
                            elem);
        } 
      else
        return idle_thread;
    }
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->original_priority = priority;
  t->recent_cpu = fix_int (0);
  t->lock_waiting_for = NULL;
  t->magic = THREAD_MAGIC;
  
  if (thread_mlfqs) 
    {
      if (t == initial_thread) 
        t->nice = 0;
      else
        t->nice = thread_get_nice();
      
      update_priority (t, NULL);
    }

  list_init (&t->acquired_locks);

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. Removes thread from relevent ready queue. */
static struct thread *
next_thread_to_run (void) 
{
  struct thread *highest_pri = highest_priority_thread ();
  if (highest_pri == idle_thread)
    return highest_pri;
  
  list_remove (&highest_pri->elem);
  return highest_pri;  
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
  / PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
