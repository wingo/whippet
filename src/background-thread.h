#ifndef BACKGROUND_THREAD_H
#define BACKGROUND_THREAD_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "assert.h"
#include "debug.h"

enum {
  GC_BACKGROUND_TASK_START = 0,
  GC_BACKGROUND_TASK_MIDDLE = 100,
  GC_BACKGROUND_TASK_END = 200
};

struct gc_background_task {
  int id;
  int priority;
  void (*run)(void *data);
  void *data;
};

enum gc_background_thread_state {
  GC_BACKGROUND_THREAD_STARTING,
  GC_BACKGROUND_THREAD_RUNNING,
  GC_BACKGROUND_THREAD_STOPPING
};

struct gc_background_thread {
  size_t count;
  size_t capacity;
  struct gc_background_task *tasks;
  int next_id;
  enum gc_background_thread_state state;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t cond;
};

static void*
gc_background_thread(void *data) {
  struct gc_background_thread *thread = data;
  pthread_mutex_lock(&thread->lock);
  while (thread->state == GC_BACKGROUND_THREAD_STARTING)
    pthread_cond_wait(&thread->cond, &thread->lock);
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts)) {
    perror("background thread: failed to get time!");
    return NULL;
  }
  while (thread->state == GC_BACKGROUND_THREAD_RUNNING) {
    ts.tv_sec += 1;
    pthread_cond_timedwait(&thread->cond, &thread->lock, &ts);
    if (thread->state == GC_BACKGROUND_THREAD_RUNNING)
      for (size_t i = 0; i < thread->count; i++)
        thread->tasks[i].run(thread->tasks[i].data);
  }
  pthread_mutex_unlock(&thread->lock);
  return NULL;
}

static struct gc_background_thread*
gc_make_background_thread(void) {
  struct gc_background_thread *thread;
  thread = malloc(sizeof(*thread));
  if (!thread)
    GC_CRASH();
  memset(thread, 0, sizeof(*thread));
  thread->tasks = NULL;
  thread->count = 0;
  thread->capacity = 0;
  thread->state = GC_BACKGROUND_THREAD_STARTING;
  pthread_mutex_init(&thread->lock, NULL);
  pthread_cond_init(&thread->cond, NULL);
  if (pthread_create(&thread->thread, NULL, gc_background_thread, thread)) {
    perror("spawning background thread failed");
    GC_CRASH();
  }
  return thread;
}

static void
gc_background_thread_start(struct gc_background_thread *thread) {
  pthread_mutex_lock(&thread->lock);
  GC_ASSERT_EQ(thread->state, GC_BACKGROUND_THREAD_STARTING);
  thread->state = GC_BACKGROUND_THREAD_RUNNING;
  pthread_mutex_unlock(&thread->lock);
  pthread_cond_signal(&thread->cond);
}

static int
gc_background_thread_add_task(struct gc_background_thread *thread,
                              int priority, void (*run)(void *data),
                              void *data) {
  pthread_mutex_lock(&thread->lock);
  if (thread->count == thread->capacity) {
    size_t new_capacity = thread->capacity * 2 + 1;
    struct gc_background_task *new_tasks =
      realloc(thread->tasks, sizeof(struct gc_background_task) * new_capacity);
    if (!new_tasks) {
      perror("ran out of space for background tasks!");
      GC_CRASH();
    }
    thread->capacity = new_capacity;
    thread->tasks = new_tasks;
  }
  size_t insert = 0;
  for (; insert < thread->count; insert++) {
    if (priority < thread->tasks[insert].priority)
      break;
  }
  size_t bytes_to_move =
    (thread->count - insert) * sizeof(struct gc_background_task);
  memmove(&thread->tasks[insert + 1], &thread->tasks[insert], bytes_to_move);
  int id = thread->next_id++;
  thread->tasks[insert].id = id;
  thread->tasks[insert].priority = priority;
  thread->tasks[insert].run = run;
  thread->tasks[insert].data = data;
  thread->count++;
  pthread_mutex_unlock(&thread->lock);
  return id;
}

static void
gc_background_thread_remove_task(struct gc_background_thread *thread,
                                 int id) {
  pthread_mutex_lock(&thread->lock);
  size_t remove = 0;
  for (; remove < thread->count; remove++) {
    if (thread->tasks[remove].id == id)
      break;
  }
  if (remove == thread->count)
    GC_CRASH();
  size_t bytes_to_move =
    (thread->count - (remove + 1)) * sizeof(struct gc_background_task);
  memmove(&thread->tasks[remove], &thread->tasks[remove + 1], bytes_to_move);
  pthread_mutex_unlock(&thread->lock);
}

static void
gc_destroy_background_thread(struct gc_background_thread *thread) {
  pthread_mutex_lock(&thread->lock);
  GC_ASSERT(thread->state == GC_BACKGROUND_THREAD_RUNNING);
  thread->state = GC_BACKGROUND_THREAD_STOPPING;
  pthread_mutex_unlock(&thread->lock);
  pthread_cond_signal(&thread->cond);
  pthread_join(thread->thread, NULL);
  free(thread->tasks);
  free(thread);
}

#endif // BACKGROUND_THREAD_H
