/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

bool waiters_priority_compare (const struct list_elem *a,
                                 const struct list_elem *b,
                                 void *aux);

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  old_level = intr_disable ();

  sema->value++;
  bool should_yield = false;
  if (!list_empty (&sema->waiters)) 
    {
      struct list_elem *max_priority = list_min (&sema -> waiters,
                                                  thread_priority_compare,
                                                  NULL); 
      struct thread *t = list_entry (max_priority, struct thread, elem);
      list_remove (max_priority);
      thread_unblock (t);
      /* Determine whether we should immediately yield to unblocked thread */
      if (t->priority > thread_current ()->priority)
        should_yield = true;
    }
  intr_set_level (old_level);
  if (should_yield)
    thread_yield();
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  lock->lock_elem.prev = NULL;
  lock->lock_elem.next = NULL;

  sema_init (&lock->semaphore, 1);
}


const int MAX_NESTED_DONATIONS = 8;
/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  enum intr_level old_level = intr_disable ();

  /* Handle priority donation */
  if (!thread_mlfqs) 
    {
      struct semaphore *sema = &lock->semaphore;
      /* Donates priority to other thread(s) */
      if (sema->value == 0) 
        {
          struct thread *current_holder = lock->holder;
          int priority = thread_current ()->priority;
          for (int i=0; i<MAX_NESTED_DONATIONS; i++) 
            {
              if (current_holder->priority >= priority) break;
              /* Donates priority */
              set_priority(current_holder, priority);
              if (!current_holder->lock_waiting_for) 
                {
                  /* This thread isn't waiting for any locks */
                  break;
                }
              /* Attempts donating priority to next thread in the tree */
              current_holder = current_holder->lock_waiting_for->holder;
            }

          /* Updates lock_waiting_for */
          thread_current ()->lock_waiting_for = lock;
      }
        
    while (sema->value == 0) 
      {
        list_push_back (&sema->waiters, &thread_current ()->elem);
        thread_block ();
      }
    sema->value--;
    thread_current ()->lock_waiting_for = NULL;
    list_push_back (&thread_current ()->acquired_locks, &lock->lock_elem);
  }

  else 
    sema_down (&lock->semaphore);

  lock->holder = thread_current ();
  intr_set_level (old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));
  enum intr_level old_level = intr_disable ();

  lock->holder = NULL;
  /* Remove lock_elem from acquired_locks list */
  if (lock->lock_elem.prev != NULL && lock->lock_elem.next != NULL)
    list_remove (&lock->lock_elem); 

  struct thread *t = thread_current ();
  struct semaphore *sema = &lock->semaphore;
  int original_priority = t->original_priority;
  int max_priority = original_priority;
  bool should_yield = false;

  if (!list_empty (&sema->waiters)) 
    {
      /* Determine which thread that was waiting for this lock to unblock */
      struct list_elem *max_pri_elem = list_min (&sema->waiters,
                                                 thread_priority_compare,
                                                 NULL); 
      struct thread *to_unblock = list_entry (max_pri_elem, 
                                              struct thread, 
                                              elem);
      list_remove (max_pri_elem);

      /* Update current thread's priority */
      if (!thread_mlfqs) 
        {
          struct list_elem *e;
      // TODO
      for (e = list_begin (&t->acquired_locks); 
          e != list_end (&t->acquired_locks); 
          e = list_next (e)) 
      {
          struct lock *l = list_entry (e, struct lock, lock_elem);
          if (!list_empty (&l->semaphore.waiters)) {
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
          int new_priority = (max_priority > original_priority) 
                              ? max_priority : original_priority;
          t->priority = new_priority;
      }

    thread_unblock (to_unblock);
    /* Determine whether we should yield to unblocked thread */
    if (to_unblock->priority > t->priority)
      should_yield = true;
  }

  sema->value++;
  intr_set_level (old_level);

  if (should_yield) 
    thread_yield();
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* One semaphore in a list. Used by condition to renotify one waiter. */
struct semaphore_elem 
  {
    struct list_elem elem;              /* List element. */
    struct semaphore semaphore;         /* This semaphore. */
    struct thread *waiting;             /* Thread waiting on this semaphore */
  };

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
  waiter.waiting = thread_current ();
  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* Comparison function for which which cond waiters are higher priority. */
bool waiters_priority_compare (const struct list_elem *a,
                                 const struct list_elem *b,
                                 void *aux) {
    (void) aux;
    struct semaphore_elem *a_waiter = list_entry (a,
                                                  struct semaphore_elem, 
                                                  elem);
    struct semaphore_elem *b_waiter = list_entry (b,
                                                  struct semaphore_elem, 
                                                  elem);
    return a_waiter->waiting->priority > b_waiter->waiting->priority;
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   Signals in order of thread priority.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) 
    {
      struct list_elem* max_pri_elem = list_min (&cond->waiters,
                                                 waiters_priority_compare,
                                                 NULL);
      list_remove (max_pri_elem);
      sema_up (&list_entry (max_pri_elem, 
                            struct semaphore_elem,
                            elem
                            )->semaphore);

    } 
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
