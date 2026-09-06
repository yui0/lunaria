/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "arm.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

/* The lock is handed out in the order it was asked for.
 *
 * Fairness is not a nicety here.  Every caller that waits for something the
 * emulator has yet to produce does it by giving the lock up and taking it
 * again — a guest thread spinning on a condition another thread will satisfy,
 * the scheduler between the threads of a pass.  With an unfair lock the thread
 * that just released it is the one already running, so it wins the race back
 * every time and the thread it is waiting for never gets in: the wait loop
 * spins forever while making perfect forward progress by its own reckoning.
 *
 * The queue is explicit rather than a ticket counter over a shared condvar.
 * A shared condvar has to be broadcast on every release — only one waiter can
 * hold the next ticket, so with N engines each hand-off woke N-1 threads that
 * immediately went back to sleep.  With one condvar per waiter the releaser
 * signals exactly the thread whose turn it is, and the lock is handed over
 * without that thread having to win anything.
 *
 * Recursion depth lives in a thread-local, so the two operations on the hot
 * path that do not change the owner — a recursive acquire and asking whether
 * this thread holds it — need no mutex at all. */
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static bool            g_locked;   /* someone owns it */
/* Read without the mutex by arm_lock_yield() on the hot path, so it is an
 * atomic rather than a plain counter: a relaxed load is the same instruction
 * and the race stops being one. */
static _Atomic unsigned g_waiters; /* queued in acquire */
static __thread unsigned t_depth;  /* this thread's recursion depth */

struct arm_lock_waiter {
   pthread_cond_t          cv;
   struct arm_lock_waiter *next;
   bool                    go;     /* the lock has been handed to me */
};
static struct arm_lock_waiter *g_head, *g_tail;

/* One waiter node per host thread, not one per wait.
 *
 * A thread can only be queued once — arm_lock_acquire() returns immediately
 * when it already holds the lock — so the node is a property of the thread and
 * not of the call.  It used to live on the stack and have its condition
 * variable created and destroyed on every contended acquire, which is a pair
 * of futex-backed allocations on the hottest path the emulator has: with the
 * lock held all the time, every SVC pays it. */
static __thread struct arm_lock_waiter t_waiter;
static __thread bool                   t_waiter_ready;

/* How much of the wall clock somebody holds the lock, and how long the threads
 * that wanted it had to wait.  The two answer different questions, and the
 * summed wait alone is misleading: it counts every waiter, so eight threads
 * queued for one millisecond read as eight.  Held time says whether the SVC
 * layer is what keeps the engines from running in parallel.  Written under
 * g_m; the reporting thread reads them without it, so they are atomics — a
 * relaxed load and store, and a counter that is never torn. */
static _Atomic unsigned long long g_held_ns, g_wait_ns, g_max_wait_ns;
static struct timespec    g_held_since;

/* Hold time charged to the label the owner set (see arm_lock_tag).  The table
 * is fixed and indexed directly: a label is an SVC number or one of the few
 * reserved ids below it, so there is nothing to allocate and nothing to lock. */
static _Atomic unsigned long long g_tag_ns[ARM_LOCK_TAG_MAX];
/* The longest single hold, and whose it was.  The summed table says which
 * label costs the most in total; it cannot say which one stopped everything
 * else for forty milliseconds in one go, and that is the number a guest with
 * a periodic thread actually feels. */
static _Atomic unsigned long long g_max_hold_ns;
static _Atomic unsigned          g_max_hold_tag;
static __thread unsigned t_tag;
static __thread unsigned t_tag_saved;

unsigned arm_lock_tag(unsigned tag)
{
   const unsigned prev = t_tag;
   t_tag = tag < ARM_LOCK_TAG_MAX ? tag : 0u;
   return prev;
}

unsigned long long arm_lock_tag_take_ns(unsigned tag)
{
   if (tag >= ARM_LOCK_TAG_MAX) return 0ull;
   return atomic_exchange_explicit(&g_tag_ns[tag], 0ull, memory_order_relaxed);
}

static unsigned long long arm_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (unsigned long long)ts.tv_sec * 1000000000ull +
          (unsigned long long)ts.tv_nsec;
}

unsigned long long arm_lock_held_ns(void)
{
   return atomic_load_explicit(&g_held_ns, memory_order_relaxed);
}
unsigned long long arm_lock_wait_ns(void)
{
   return atomic_load_explicit(&g_wait_ns, memory_order_relaxed);
}
unsigned long long arm_lock_max_wait_ns(void)
{
   return atomic_exchange_explicit(&g_max_wait_ns, 0ull, memory_order_relaxed);
}

unsigned long long arm_lock_max_hold_ns(unsigned *tag_out)
{
   if (tag_out)
      *tag_out = atomic_load_explicit(&g_max_hold_tag, memory_order_relaxed);
   return atomic_exchange_explicit(&g_max_hold_ns, 0ull, memory_order_relaxed);
}

void arm_lock_acquire(void)
{
   if (t_depth) { ++t_depth; return; }
   pthread_mutex_lock(&g_m);
   if (!g_locked && !g_head) {
      g_locked = true;
      clock_gettime(CLOCK_MONOTONIC, &g_held_since);
   } else {
      /* The releaser only touches the node while holding g_m, and this thread
       * does not return from the wait until it has been dequeued under the
       * same mutex, so the node is free again by the time we leave. */
      struct arm_lock_waiter *w = &t_waiter;
      if (!t_waiter_ready) {
         pthread_cond_init(&w->cv, NULL);
         t_waiter_ready = true;
      }
      w->next = NULL;
      w->go   = false;
      if (g_tail) g_tail->next = w; else g_head = w;
      g_tail = w;
      atomic_fetch_add_explicit(&g_waiters, 1u, memory_order_relaxed);
      const unsigned long long t0 = arm_now_ns();
      while (!w->go)
         pthread_cond_wait(&w->cv, &g_m);
      const unsigned long long waited = arm_now_ns() - t0;
      atomic_fetch_add_explicit(&g_wait_ns, waited, memory_order_relaxed);
      unsigned long long worst =
         atomic_load_explicit(&g_max_wait_ns, memory_order_relaxed);
      /* Only g_m holders get here, so a plain compare-and-store is enough. */
      if (waited > worst)
         atomic_store_explicit(&g_max_wait_ns, waited, memory_order_relaxed);
      atomic_fetch_sub_explicit(&g_waiters, 1u, memory_order_relaxed);
      /* g_locked stayed true: the releaser handed ownership straight over. */
   }
   pthread_mutex_unlock(&g_m);
   /* The label belongs to this acquisition, not to the thread.  It used to
    * persist, so every acquire that never set one -- a scheduler pass, a guest
    * callback, any internal helper that just takes the lock -- was charged to
    * whichever SVC this thread happened to run last.  That is how one SVC came
    * to "hold" a quarter of the wall clock while the timers inside it summed
    * to a fraction of a second.  Start unlabelled and let the caller name it. */
   t_tag = 0u;
   t_depth = 1;
}

void arm_lock_release(void)
{
   if (!t_depth) return;      /* not ours to release */
   if (--t_depth) return;     /* still held by an outer acquire */
   pthread_mutex_lock(&g_m);
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   /* tv_nsec borrows across the second boundary on roughly half of all
    * releases (whenever now.tv_nsec < g_held_since.tv_nsec) -- computing
    * that subtraction in unsigned before adding the whole-seconds term
    * wrapped it to just under 2^64 ns instead of going negative, so "mine"
    * came out around 18446744073709.5 ms on those releases. Since that beat
    * every real hold, g_max_hold_ns latched onto the wrapped value instead
    * of the actual longest hold almost as often as not -- the
    * "[slice] longest single hold" line this feeds was reporting garbage,
    * not a rare fluke. Do the subtraction in signed arithmetic, where a
    * borrow is just a negative intermediate, and only convert to unsigned
    * once the true (non-negative, the clock is monotonic) result is known. */
   const long long mine_s =
      (long long)(now.tv_sec - g_held_since.tv_sec) * 1000000000ll +
      ((long long)now.tv_nsec - (long long)g_held_since.tv_nsec);
   const unsigned long long mine = (unsigned long long)mine_s;
   atomic_fetch_add_explicit(&g_held_ns, mine, memory_order_relaxed);
   atomic_fetch_add_explicit(&g_tag_ns[t_tag], mine, memory_order_relaxed);
   {
      unsigned long long prev =
         atomic_load_explicit(&g_max_hold_ns, memory_order_relaxed);
      while (mine > prev &&
             !atomic_compare_exchange_weak_explicit(
                 &g_max_hold_ns, &prev, mine,
                 memory_order_relaxed, memory_order_relaxed))
         ;
      if (mine >= prev)
         atomic_store_explicit(&g_max_hold_tag, t_tag, memory_order_relaxed);
   }
   struct arm_lock_waiter *w = g_head;
   if (w) {
      g_head = w->next;
      if (!g_head) g_tail = NULL;
      w->go = true;
      g_held_since = now;          /* handed straight over: still held */
      pthread_cond_signal(&w->cv);
   } else {
      g_locked = false;
   }
   pthread_mutex_unlock(&g_m);
}

bool arm_lock_held(void)
{
   return t_depth != 0u;
}

unsigned arm_lock_unlock_all(void)
{
   unsigned d = t_depth;
   if (d) {
      t_tag_saved = t_tag;   /* the same acquisition resumes in relock() */
      t_depth = 1u;
      arm_lock_release();
   }
   return d;
}

void arm_lock_relock(unsigned depth)
{
   if (!depth) return;
   arm_lock_acquire();
   t_tag   = t_tag_saved;
   t_depth = depth;
}

void arm_lock_yield(void)
{
   /* A stale read only gates an optimisation: a stale zero costs one more turn
    * of the caller's loop, a stale non-zero one pointless hand-off.  The
    * hand-off itself needs no waiting — acquire() queues this thread behind
    * the threads already asking. */
   if (!atomic_load_explicit(&g_waiters, memory_order_relaxed))
      return;
   unsigned d = arm_lock_unlock_all();
   if (!d)
      return;
   arm_lock_relock(d);
}

unsigned arm_lock_waiters(void)
{
   return atomic_load_explicit(&g_waiters, memory_order_relaxed);
}

static bool g_parallel;
bool arm_parallel_engines(void) { return g_parallel; }
void arm_set_parallel_engines(bool on) { g_parallel = on; }
