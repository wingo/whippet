#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/time.h>

#include "assert.h"
#include "gc-api.h"
#include "gc-basic-stats.h"
#include "gc-ephemeron.h"
#include "simple-roots-api.h"
#include "ephemerons-types.h"
#include "simple-allocator.h"

typedef HANDLE_TO(SmallObject) SmallObjectHandle;
typedef HANDLE_TO(struct gc_ephemeron) EphemeronHandle;
typedef HANDLE_TO(Box) BoxHandle;

static SmallObject* allocate_small_object(struct gc_mutator *mut) {
  return gc_allocate_with_kind(mut, ALLOC_KIND_SMALL_OBJECT, sizeof(SmallObject));
}

static Box* allocate_box(struct gc_mutator *mut) {
  return gc_allocate_with_kind(mut, ALLOC_KIND_BOX, sizeof(Box));
}

static struct gc_ephemeron* allocate_ephemeron(struct gc_mutator *mut) {
  struct gc_ephemeron *ret = gc_allocate_ephemeron(mut);
  *tag_word(gc_ref_from_heap_object(ret)) = tag_live(ALLOC_KIND_EPHEMERON);
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

static size_t ephemeron_chain_length(struct gc_ephemeron **loc,
                                     SmallObject *key) {
  struct gc_ephemeron *head = gc_ephemeron_chain_head(loc);
  size_t len = 0;
  while (head) {
    CHECK_EQ(key, (SmallObject*)gc_ref_value(gc_ephemeron_key(head)));
    Box *value = gc_ref_heap_object(gc_ephemeron_value(head));
    CHECK_NOT_NULL(value);
    key = value->obj;
    CHECK_NOT_NULL(key);
    head = gc_ephemeron_chain_next(head);
    len++;
  }
  return len;
}

static double heap_size;
static double heap_multiplier;
static size_t nthreads;

static void cause_gc(struct gc_mutator *mut) {
  // Doing a full collection lets us reason precisely about liveness.
  gc_collect(mut, GC_COLLECTION_MAJOR);
}

static void make_ephemeron_chain(struct thread *t, EphemeronHandle *head,
                                 SmallObjectHandle *head_key, size_t length) {
  BoxHandle tail_box = { NULL };
  PUSH_HANDLE(t, tail_box);

  CHECK_NULL(HANDLE_REF(*head_key));
  HANDLE_SET(*head_key, allocate_small_object(t->mut));

  for (size_t i = 0; i < length; i++) {
    HANDLE_SET(tail_box, allocate_box(t->mut));
    HANDLE_REF(tail_box)->obj = HANDLE_REF(*head_key);
    HANDLE_SET(*head_key, allocate_small_object(t->mut));
    struct gc_ephemeron *ephemeron = allocate_ephemeron(t->mut);
    gc_ephemeron_init(t->mut, ephemeron,
                      gc_ref_from_heap_object(HANDLE_REF(*head_key)),
                      gc_ref_from_heap_object(HANDLE_REF(tail_box)));
    gc_ephemeron_chain_push(HANDLE_LOC(*head), ephemeron);
  }

  POP_HANDLE(t);
}

static void* run_one_test(struct thread *t) {
  size_t unit_size = gc_ephemeron_size() + sizeof(Box);
  size_t list_length = heap_size / nthreads / heap_multiplier / unit_size;

  printf("Allocating ephemeron list %zu nodes long.  Total size %.3fGB.\n",
         list_length, list_length * unit_size / 1e9);

  unsigned long thread_start = current_time();

  SmallObjectHandle head_key = { NULL };
  EphemeronHandle head = { NULL };

  PUSH_HANDLE(t, head_key);
  PUSH_HANDLE(t, head);

  make_ephemeron_chain(t, &head, &head_key, list_length);

  size_t measured_length = ephemeron_chain_length(HANDLE_LOC(head),
                                                  HANDLE_REF(head_key));
  CHECK_EQ(measured_length, list_length);

  cause_gc(t->mut);
  measured_length = ephemeron_chain_length(HANDLE_LOC(head),
                                           HANDLE_REF(head_key));
  CHECK_EQ(measured_length, list_length);

  if (!GC_CONSERVATIVE_ROOTS) {
    HANDLE_SET(head_key, NULL);
    cause_gc(t->mut);
    measured_length = ephemeron_chain_length(HANDLE_LOC(head),
                                             HANDLE_REF(head_key));
    CHECK_EQ(measured_length, 0);
  }

  // swap head_key for a key halfway in, cause gc
  // check length is expected half-length; warn, or error if precise
  // clear and return

  print_elapsed("thread", thread_start);

  POP_HANDLE(t);
  POP_HANDLE(t);

  return NULL;
}

static void* run_one_test_in_thread(void *arg) {
  struct gc_heap *heap = arg;
  return call_with_gc(run_one_test, heap);
}

struct join_data { int status; pthread_t thread; };
static void *join_thread(struct gc_mutator *unused, void *data) {
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
  run_one_test(&main_thread);
  for (size_t i = 1; i < nthreads; i++) {
    struct join_data data = { 0, threads[i] };
    gc_deactivate_for_call(mut, join_thread, &data);
    if (data.status) {
      errno = data.status;
      perror("Failed to join thread");
      return 1;
    }
  }
  
  gc_basic_stats_finish(&stats);
  fputs("\n", stdout);
  gc_basic_stats_print(&stats, stdout);

  return 0;
}

