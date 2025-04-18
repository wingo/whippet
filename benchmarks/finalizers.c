#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/time.h>

#include "assert.h"
#include "gc-api.h"
#include "gc-basic-stats.h"
#include "gc-finalizer.h"
#include "simple-roots-api.h"
#include "finalizers-types.h"
#include "simple-allocator.h"

typedef HANDLE_TO(SmallObject) SmallObjectHandle;
typedef HANDLE_TO(struct gc_finalizer) FinalizerHandle;
typedef HANDLE_TO(Pair) PairHandle;

static SmallObject* allocate_small_object(struct gc_mutator *mut) {
  return gc_allocate_with_kind(mut, ALLOC_KIND_SMALL_OBJECT, sizeof(SmallObject));
}

static Pair* allocate_pair(struct gc_mutator *mut) {
  return gc_allocate_with_kind(mut, ALLOC_KIND_PAIR, sizeof(Pair));
}

static struct gc_finalizer* allocate_finalizer(struct gc_mutator *mut) {
  struct gc_finalizer *ret = gc_allocate_finalizer(mut);
  *tag_word(gc_ref_from_heap_object(ret)) = tag_live(ALLOC_KIND_FINALIZER);
  return ret;
}

/* Get the current time in microseconds */
static unsigned long current_time(void)
{
  struct timeval t;
  if (gettimeofday(&t, NULL) == -1)
    return 0;
  return t.tv_sec * 1000 * 1000 + t.tv_usec;
}

struct thread {
  struct gc_mutator *mut;
  struct gc_mutator_roots roots;
};

static void print_elapsed(const char *what, unsigned long start) {
  unsigned long end = current_time();
  unsigned long msec = (end - start) / 1000;
  unsigned long usec = (end - start) % 1000;
  printf("Completed %s in %lu.%.3lu msec\n", what, msec, usec);
}

struct call_with_gc_data {
  void* (*f)(struct thread *);
  struct gc_heap *heap;
};
static void* call_with_gc_inner(struct gc_stack_addr addr, void *arg) {
  struct call_with_gc_data *data = arg;
  struct gc_mutator *mut = gc_init_for_thread(addr, data->heap);
  struct thread t = { mut, };
  gc_mutator_set_roots(mut, &t.roots);
  void *ret = data->f(&t);
  gc_finish_for_thread(mut);
  return ret;
}
static void* call_with_gc(void* (*f)(struct thread *),
                          struct gc_heap *heap) {
  struct call_with_gc_data data = { f, heap };
  return gc_call_with_stack_addr(call_with_gc_inner, &data);
}

#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                          \
    }                                                                   \
  } while (0)

#define CHECK_EQ(x, y) CHECK((x) == (y))
#define CHECK_NE(x, y) CHECK((x) != (y))
#define CHECK_NULL(x) CHECK_EQ(x, NULL)
#define CHECK_NOT_NULL(x) CHECK_NE(x, NULL)

static double heap_size;
static double heap_multiplier;
static size_t nthreads;

static void cause_gc(struct gc_mutator *mut) {
  // Doing a full collection lets us reason precisely about liveness.
  gc_collect(mut, GC_COLLECTION_MAJOR);
}

static inline void set_car(struct gc_mutator *mut, Pair *obj, void *val) {
  void **field = &obj->car;
  if (val)
    gc_write_barrier(mut, gc_ref_from_heap_object(obj), sizeof(Pair),
                     gc_edge(field),
                     gc_ref_from_heap_object(val));
  *field = val;
}

static inline void set_cdr(struct gc_mutator *mut, Pair *obj, void *val) {
  void **field = &obj->cdr;
  if (val)
    gc_write_barrier(mut, gc_ref_from_heap_object(obj), sizeof(Pair),
                     gc_edge(field),
                     gc_ref_from_heap_object(val));
  field = val;
}

static Pair* make_finalizer_chain(struct thread *t, size_t length) {
  PairHandle head = { NULL };
  PairHandle tail = { NULL };
  PUSH_HANDLE(t, head);
  PUSH_HANDLE(t, tail);

  for (size_t i = 0; i < length; i++) {
    HANDLE_SET(tail, HANDLE_REF(head));
    HANDLE_SET(head, allocate_pair(t->mut));
    set_car(t->mut, HANDLE_REF(head), allocate_small_object(t->mut));
    set_cdr(t->mut, HANDLE_REF(head), HANDLE_REF(tail));
    struct gc_finalizer *finalizer = allocate_finalizer(t->mut);
    gc_finalizer_attach(t->mut, finalizer, 0,
                        gc_ref_from_heap_object(HANDLE_REF(head)),
                        gc_ref_from_heap_object(HANDLE_REF(head)->car));
  }

  Pair *ret = HANDLE_REF(head);
  POP_HANDLE(t);
  POP_HANDLE(t);
  return ret;
}

static void* run_one_test(struct thread *t) {
  size_t unit_size = gc_finalizer_size() + sizeof(Pair);
  size_t list_length = heap_size / nthreads / heap_multiplier / unit_size;
  ssize_t outstanding = list_length;

  printf("Allocating list %zu nodes long.  Total size %.3fGB.\n",
         list_length, list_length * unit_size / 1e9);

  unsigned long thread_start = current_time();

  PairHandle chain = { NULL };
  PUSH_HANDLE(t, chain);

  HANDLE_SET(chain, make_finalizer_chain(t, list_length));
  cause_gc(t->mut);

  size_t finalized = 0;
  for (struct gc_finalizer *f = gc_pop_finalizable(t->mut);
       f;
       f = gc_pop_finalizable(t->mut)) {
    Pair* p = gc_ref_heap_object(gc_finalizer_object(f));
    SmallObject* o = gc_ref_heap_object(gc_finalizer_closure(f));
    CHECK_EQ(p->car, o);
    finalized++;
  }
  printf("thread %p: GC before clear finalized %zu nodes.\n", t, finalized);
  outstanding -= finalized;

  HANDLE_SET(chain, NULL);
  cause_gc(t->mut);

  finalized = 0;
  for (struct gc_finalizer *f = gc_pop_finalizable(t->mut);
       f;
       f = gc_pop_finalizable(t->mut)) {
    Pair* p = gc_ref_heap_object(gc_finalizer_object(f));
    SmallObject* o = gc_ref_heap_object(gc_finalizer_closure(f));
    CHECK_EQ(p->car, o);
    finalized++;
  }
  printf("thread %p: GC after clear finalized %zu nodes.\n", t, finalized);
  outstanding -= finalized;

  print_elapsed("thread", thread_start);

  POP_HANDLE(t);

  return (void*)outstanding;
}

static void* run_one_test_in_thread(void *arg) {
  struct gc_heap *heap = arg;
  return call_with_gc(run_one_test, heap);
}

struct join_data { int status; pthread_t thread; };
static void *join_thread(void *data) {
  struct join_data *join_data = data;
  void *ret;
  join_data->status = pthread_join(join_data->thread, &ret);
  return ret;
}

#define MAX_THREAD_COUNT 256

int main(int argc, char *argv[]) {
  if (argc < 4 || 5 < argc) {
    fprintf(stderr, "usage: %s HEAP_SIZE MULTIPLIER NTHREADS [GC-OPTIONS]\n", argv[0]);
    return 1;
  }

  heap_size = atof(argv[1]);
  heap_multiplier = atof(argv[2]);
  nthreads = atol(argv[3]);

  if (heap_size < 8192) {
    fprintf(stderr,
            "Heap size should probably be at least 8192, right? '%s'\n",
            argv[1]);
    return 1;
  }
  if (!(1.0 < heap_multiplier && heap_multiplier < 100)) {
    fprintf(stderr, "Failed to parse heap multiplier '%s'\n", argv[2]);
    return 1;
  }
  if (nthreads < 1 || nthreads > MAX_THREAD_COUNT) {
    fprintf(stderr, "Expected integer between 1 and %d for thread count, got '%s'\n",
            (int)MAX_THREAD_COUNT, argv[2]);
    return 1;
  }

  printf("Allocating heap of %.3fGB (%.2f multiplier of live data).\n",
         heap_size / 1e9, heap_multiplier);

  struct gc_options *options = gc_allocate_options();
  gc_options_set_int(options, GC_OPTION_HEAP_SIZE_POLICY, GC_HEAP_SIZE_FIXED);
  gc_options_set_size(options, GC_OPTION_HEAP_SIZE, heap_size);
  if (argc == 5) {
    if (!gc_options_parse_and_set_many(options, argv[4])) {
      fprintf(stderr, "Failed to set GC options: '%s'\n", argv[4]);
      return 1;
    }
  }

  struct gc_heap *heap;
  struct gc_mutator *mut;
  struct gc_basic_stats stats;
  if (!gc_init(options, gc_empty_stack_addr(), &heap, &mut,
               GC_BASIC_STATS, &stats)) {
    fprintf(stderr, "Failed to initialize GC with heap size %zu bytes\n",
            (size_t)heap_size);
    return 1;
  }
  struct thread main_thread = { mut, };
  gc_mutator_set_roots(mut, &main_thread.roots);

  pthread_t threads[MAX_THREAD_COUNT];
  // Run one of the threads in the main thread.
  for (size_t i = 1; i < nthreads; i++) {
    int status = pthread_create(&threads[i], NULL, run_one_test_in_thread, heap);
    if (status) {
      errno = status;
      perror("Failed to create thread");
      return 1;
    }
  }
  ssize_t outstanding = (size_t)run_one_test(&main_thread);
  for (size_t i = 1; i < nthreads; i++) {
    struct join_data data = { 0, threads[i] };
    void *ret = gc_call_without_gc(mut, join_thread, &data);
    if (data.status) {
      errno = data.status;
      perror("Failed to join thread");
      return 1;
    }
    ssize_t thread_outstanding = (ssize_t)ret;
    outstanding += thread_outstanding;
  }
  
  if (outstanding)
    printf("\n\nWARNING: %zd nodes outstanding!!!\n\n", outstanding);

  gc_basic_stats_finish(&stats);
  fputs("\n", stdout);
  gc_basic_stats_print(&stats, stdout);

  return 0;
}

