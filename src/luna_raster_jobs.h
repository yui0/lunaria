/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

/* Native pixel rows only: callbacks must not enter Java, allocate VM objects,
 * or change shared Canvas state. The owner waits before exposing the bitmap.
 * Each VM owns and joins its workers, including on repeated create/destroy. */
typedef void (*luna_raster_rows_fn)(void *, int, int);
struct luna_raster_jobs {
  pthread_mutex_t dispatch, mutex;
  pthread_cond_t ready, done;
  pthread_t threads[3];
  unsigned count, remaining, generation;
  bool stopping;
  atomic_int next;
  int end, grain;
  luna_raster_rows_fn rows;
  void *context;
};
static void luna_raster_work(struct luna_raster_jobs *jobs)
{
  for (;;) {
    int begin = atomic_fetch_add_explicit(&jobs->next, jobs->grain, memory_order_relaxed);
    if (begin >= jobs->end) break;
    int end = jobs->end - begin < jobs->grain ? jobs->end : begin + jobs->grain;
    jobs->rows(jobs->context, begin, end);
  }
}
static void *luna_raster_worker(void *arg)
{
  struct luna_raster_jobs *jobs = arg;
  unsigned generation = 0;
  pthread_mutex_lock(&jobs->mutex);
  for (;;) {
    while (!jobs->stopping && generation == jobs->generation)
      pthread_cond_wait(&jobs->ready, &jobs->mutex);
    if (jobs->stopping) break;
    generation = jobs->generation;
    pthread_mutex_unlock(&jobs->mutex);
    luna_raster_work(jobs);
    pthread_mutex_lock(&jobs->mutex);
    if (!--jobs->remaining) pthread_cond_signal(&jobs->done);
  }
  pthread_mutex_unlock(&jobs->mutex);
  return NULL;
}
static struct luna_raster_jobs *luna_raster_jobs_create(unsigned workers)
{
  if (!workers) return NULL;
  struct luna_raster_jobs *jobs = calloc(1, sizeof *jobs);
  if (!jobs) return NULL;
  if (pthread_mutex_init(&jobs->dispatch, NULL)) goto fail;
  if (pthread_mutex_init(&jobs->mutex, NULL)) goto dispatch;
  if (pthread_cond_init(&jobs->ready, NULL)) goto mutex;
  if (pthread_cond_init(&jobs->done, NULL)) goto ready;
  atomic_init(&jobs->next, 0);
  if (workers > 3) workers = 3;
  while (jobs->count < workers) {
    if (pthread_create(&jobs->threads[jobs->count], NULL, luna_raster_worker, jobs)) break;
    ++jobs->count;
  }
  return jobs;
ready:
  pthread_cond_destroy(&jobs->ready);
mutex:
  pthread_mutex_destroy(&jobs->mutex);
dispatch:
  pthread_mutex_destroy(&jobs->dispatch);
fail:
  free(jobs);
  return NULL;
}
static void luna_raster_jobs_run(struct luna_raster_jobs *jobs,
                                  luna_raster_rows_fn rows, void *context,
                                  int begin, int end)
{
  if (begin >= end) return;
  if (!jobs || !jobs->count || end - begin < 16) {
    rows(context, begin, end);
    return;
  }
  pthread_mutex_lock(&jobs->dispatch);
  pthread_mutex_lock(&jobs->mutex);
  jobs->rows = rows;
  jobs->context = context;
  jobs->end = end;
  int chunks = (int)(jobs->count + 1) * 2;
  jobs->grain = ((end - begin + chunks - 1) / chunks + 7) & ~7;
  atomic_store_explicit(&jobs->next, begin, memory_order_relaxed);
  jobs->remaining = jobs->count;
  ++jobs->generation;
  pthread_cond_broadcast(&jobs->ready);
  pthread_mutex_unlock(&jobs->mutex);
  luna_raster_work(jobs);
  pthread_mutex_lock(&jobs->mutex);
  while (jobs->remaining) pthread_cond_wait(&jobs->done, &jobs->mutex);
  pthread_mutex_unlock(&jobs->mutex);
  pthread_mutex_unlock(&jobs->dispatch);
}
static void luna_raster_jobs_destroy(struct luna_raster_jobs *jobs)
{
  if (!jobs) return;
  pthread_mutex_lock(&jobs->dispatch);
  pthread_mutex_lock(&jobs->mutex);
  jobs->stopping = true;
  pthread_cond_broadcast(&jobs->ready);
  pthread_mutex_unlock(&jobs->mutex);
  for (unsigned i = 0; i < jobs->count; ++i) pthread_join(jobs->threads[i], NULL);
  pthread_mutex_unlock(&jobs->dispatch);
  pthread_cond_destroy(&jobs->done);
  pthread_cond_destroy(&jobs->ready);
  pthread_mutex_destroy(&jobs->mutex);
  pthread_mutex_destroy(&jobs->dispatch);
  free(jobs);
}
