// This is adapted from a benchmark written by John Ellis and Pete Kovac
// of Post Communications.
// It was modified by Hans Boehm of Silicon Graphics.
// Translated to C++ 30 May 1997 by William D Clinger of Northeastern Univ.
// Translated to C 15 March 2000 by Hans Boehm, now at HP Labs.
//
//      This is no substitute for real applications.  No actual application
//      is likely to behave in exactly this way.  However, this benchmark was
//      designed to be more representative of real applications than other
//      Java GC benchmarks of which we are aware.
//      It attempts to model those properties of allocation requests that
//      are important to current GC techniques.
//      It is designed to be used either to obtain a single overall performance
//      number, or to give a more detailed estimate of how collector
//      performance varies with object lifetimes.  It prints the time
//      required to allocate and collect balanced binary trees of various
//      sizes.  Smaller trees result in shorter object lifetimes.  Each cycle
//      allocates roughly the same amount of memory.
//      Two data structures are kept around during the entire process, so
//      that the measured performance is representative of applications
//      that maintain some live in-memory data.  One of these is a tree
//      containing many pointers.  The other is a large array containing
//      double precision floating point numbers.  Both should be of comparable
//      size.
//
//      The results are only really meaningful together with a specification
//      of how much memory was used.  It is possible to trade memory for
//      better time performance.  This benchmark should be run in a 32 MB
//      heap, though we don't currently know how to enforce that uniformly.
//
//      Unlike the original Ellis and Kovac benchmark, we do not attempt
//      measure pause times.  This facility should eventually be added back
//      in.  There are several reasons for omitting it for now.  The original
//      implementation depended on assumptions about the thread scheduler
//      that don't hold uniformly.  The results really measure both the
//      scheduler and GC.  Pause time measurements tend to not fit well with
//      current benchmark suites.  As far as we know, none of the current
//      commercial Java implementations seriously attempt to minimize GC pause
//      times.

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "assert.h"
#include "gcbench-types.h"
#include "gc.h"
#include "inline.h"

#define MAX_THREAD_COUNT 256

static const int long_lived_tree_depth = 16; // about 4Mb
static const int array_size = 500000; // about 4Mb
static const int min_tree_depth = 4;
static const int max_tree_depth = 16;

struct Node {
  GC_HEADER;
  struct Node * left;
  struct Node * right;
  int i, j;
};

struct DoubleArray {
  GC_HEADER;
  size_t length;
  double values[0];
};

static inline size_t node_size(Node *obj) {
  return sizeof(Node);
}
static inline size_t double_array_size(DoubleArray *array) {
  return sizeof(*array) + array->length * sizeof(double);
}
static inline void
visit_node_fields(Node *node,
                  void (*visit)(void **loc, void *visit_data),
                  void *visit_data) {
  visit((void**)&node->left, visit_data);
  visit((void**)&node->right, visit_data);
}
static inline void
visit_double_array_fields(DoubleArray *obj,
                          void (*visit)(void **loc, void *visit_data),
                          void *visit_data) {
}

typedef HANDLE_TO(Node) NodeHandle;
typedef HANDLE_TO(DoubleArray) DoubleArrayHandle;

static Node* allocate_node(struct mutator *mut) {
  // memset to 0 by the collector.
  return allocate(mut, ALLOC_KIND_NODE, sizeof (Node));
}

static DoubleArray* allocate_double_array(struct mutator *mut,
                                                 size_t size) {
  // May be uninitialized.
  DoubleArray *ret =
    allocate_pointerless(mut, ALLOC_KIND_DOUBLE_ARRAY,
                         sizeof(DoubleArray) + sizeof (double) * size);
  ret->length = size;
  return ret;
}

static unsigned long current_time(void)
{
  struct timeval t = { 0 };
  gettimeofday(&t, NULL);
  return t.tv_sec * 1000 * 1000 + t.tv_usec;
}

static double elapsed_millis(unsigned long start) {
  return (current_time() - start) * 1e-3;
}

// Nodes used by a tree of a given size
static int tree_size(int i) {
  return ((1 << (i + 1)) - 1);
}

// Number of iterations to use for a given tree depth
static int compute_num_iters(int i) {
  return 2 * tree_size(max_tree_depth + 2) / tree_size(i);
}

// Build tree top down, assigning to older objects.
static void populate(struct mutator *mut, int depth, Node *node) {
  if (depth <= 0)
    return;

  NodeHandle self = { node };
  PUSH_HANDLE(mut, self);
  NodeHandle l = { allocate_node(mut) };
  PUSH_HANDLE(mut, l);
  NodeHandle r = { allocate_node(mut) };
  PUSH_HANDLE(mut, r);

  set_field((void**)&HANDLE_REF(self)->left, HANDLE_REF(l));
  set_field((void**)&HANDLE_REF(self)->right, HANDLE_REF(r));
  // i is 0 because the memory is zeroed.
  HANDLE_REF(self)->j = depth;

  populate(mut, depth-1, HANDLE_REF(self)->left);
  populate(mut, depth-1, HANDLE_REF(self)->right);

  POP_HANDLE(mut);
  POP_HANDLE(mut);
  POP_HANDLE(mut);
}

// Build tree bottom-up
static Node* make_tree(struct mutator *mut, int depth) {
  if (depth <= 0)
    return allocate_node(mut);

  NodeHandle left = { make_tree(mut, depth-1) };
  PUSH_HANDLE(mut, left);
  NodeHandle right = { make_tree(mut, depth-1) };
  PUSH_HANDLE(mut, right);

  Node *result = allocate_node(mut);
  init_field((void**)&result->left, HANDLE_REF(left));
  init_field((void**)&result->right, HANDLE_REF(right));
  // i is 0 because the memory is zeroed.
  result->j = depth;

  POP_HANDLE(mut);
  POP_HANDLE(mut);

  return result;
}

static void validate_tree(Node *tree, int depth) {
#ifndef NDEBUG
  ASSERT_EQ(tree->i, 0);
  ASSERT_EQ(tree->j, depth);
  if (depth == 0) {
    ASSERT(!tree->left);
    ASSERT(!tree->right);
  } else {
    ASSERT(tree->left);
    ASSERT(tree->right);
    validate_tree(tree->left, depth - 1);
    validate_tree(tree->right, depth - 1);
  }
#endif
}

static void time_construction(struct mutator *mut, int depth) {
  int num_iters = compute_num_iters(depth);
  NodeHandle temp_tree = { NULL };
  PUSH_HANDLE(mut, temp_tree);

  printf("Creating %d trees of depth %d\n", num_iters, depth);

  {
    unsigned long start = current_time();
    for (int i = 0; i < num_iters; ++i) {
      HANDLE_SET(temp_tree, allocate_node(mut));
      populate(mut, depth, HANDLE_REF(temp_tree));
      validate_tree(HANDLE_REF(temp_tree), depth);
      HANDLE_SET(temp_tree, NULL);
    }
    printf("\tTop down construction took %.3f msec\n",
           elapsed_millis(start));
  }

  {
    long start = current_time();
    for (int i = 0; i < num_iters; ++i) {
      HANDLE_SET(temp_tree, make_tree(mut, depth));
      validate_tree(HANDLE_REF(temp_tree), depth);
      HANDLE_SET(temp_tree, NULL);
    }
    printf("\tBottom up construction took %.3f msec\n",
           elapsed_millis(start));
  }

  POP_HANDLE(mut);
}

static void* call_with_stack_base(void* (*)(uintptr_t*, void*), void*) NEVER_INLINE;
static void* call_with_stack_base_inner(void* (*)(uintptr_t*, void*), uintptr_t*, void*) NEVER_INLINE;
static void* call_with_stack_base_inner(void* (*f)(uintptr_t *stack_base, void *arg),
                                        uintptr_t *stack_base, void *arg) {
  return f(stack_base, arg);
}
static void* call_with_stack_base(void* (*f)(uintptr_t *stack_base, void *arg),
                                  void *arg) {
  uintptr_t x;
  return call_with_stack_base_inner(f, &x, arg);
}

struct call_with_gc_data {
  void* (*f)(struct mutator *);
  struct heap *heap;
};
static void* call_with_gc_inner(uintptr_t *stack_base, void *arg) {
  struct call_with_gc_data *data = arg;
  struct mutator *mut = initialize_gc_for_thread(stack_base, data->heap);
  void *ret = data->f(mut);
  finish_gc_for_thread(mut);
  return ret;
}
static void* call_with_gc(void* (*f)(struct mutator *),
                          struct heap *heap) {
  struct call_with_gc_data data = { f, heap };
  return call_with_stack_base(call_with_gc_inner, &data);
}

static void* run_one_test(struct mutator *mut) {
  NodeHandle long_lived_tree = { NULL };
  NodeHandle temp_tree = { NULL };
  DoubleArrayHandle array = { NULL };

  PUSH_HANDLE(mut, long_lived_tree);
  PUSH_HANDLE(mut, temp_tree);
  PUSH_HANDLE(mut, array);

  // Create a long lived object
  printf(" Creating a long-lived binary tree of depth %d\n",
         long_lived_tree_depth);
  HANDLE_SET(long_lived_tree, allocate_node(mut));
  populate(mut, long_lived_tree_depth, HANDLE_REF(long_lived_tree));

  // Create long-lived array, filling half of it
  printf(" Creating a long-lived array of %d doubles\n", array_size);
  HANDLE_SET(array, allocate_double_array(mut, array_size));
  for (int i = 0; i < array_size/2; ++i) {
    HANDLE_REF(array)->values[i] = 1.0/i;
  }

  for (int d = min_tree_depth; d <= max_tree_depth; d += 2) {
    time_construction(mut, d);
  }

  validate_tree(HANDLE_REF(long_lived_tree), long_lived_tree_depth);

  // Fake reference to LongLivedTree and array to keep them from being optimized
  // away.
  if (HANDLE_REF(long_lived_tree)->i != 0
      || HANDLE_REF(array)->values[1000] != 1.0/1000)
    fprintf(stderr, "Failed\n");

  POP_HANDLE(mut);
  POP_HANDLE(mut);
  POP_HANDLE(mut);
  return NULL;
}

static void* run_one_test_in_thread(void *arg) {
  struct heap *heap = arg;
  return call_with_gc(run_one_test, heap);
}

struct join_data { int status; pthread_t thread; };
static void *join_thread(void *data) {
  struct join_data *join_data = data;
  void *ret;
  join_data->status = pthread_join(join_data->thread, &ret);
  return ret;
}

int main(int argc, char *argv[]) {
  // Define size of Node without any GC header.
  size_t sizeof_node = 2 * sizeof(Node*) + 2 * sizeof(int);
  size_t sizeof_double_array = sizeof(size_t);
  size_t heap_max_live =
    tree_size(long_lived_tree_depth) * sizeof_node +
    tree_size(max_tree_depth) * sizeof_node +
    sizeof_double_array + sizeof(double) * array_size;
  if (argc != 3) {
    fprintf(stderr, "usage: %s MULTIPLIER NTHREADS\n", argv[0]);
    return 1;
  }

  double multiplier = atof(argv[1]);
  size_t nthreads = atol(argv[2]);

  if (!(1.0 < multiplier && multiplier < 100)) {
    fprintf(stderr, "Failed to parse heap multiplier '%s'\n", argv[2]);
    return 1;
  }
  if (nthreads < 1 || nthreads > MAX_THREAD_COUNT) {
    fprintf(stderr, "Expected integer between 1 and %d for thread count, got '%s'\n",
            (int)MAX_THREAD_COUNT, argv[2]);
    return 1;
  }

  size_t heap_size = heap_max_live * multiplier * nthreads;
  struct heap *heap;
  struct mutator *mut;
  if (!initialize_gc(heap_size, &heap, &mut)) {
    fprintf(stderr, "Failed to initialize GC with heap size %zu bytes\n",
            heap_size);
    return 1;
  }

  printf("Garbage Collector Test\n");
  printf(" Live storage will peak at %zd bytes.\n\n", heap_max_live);
  print_start_gc_stats(heap);

  unsigned long start = current_time();
        
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
  run_one_test(mut);
  for (size_t i = 1; i < nthreads; i++) {
    struct join_data data = { 0, threads[i] };
    call_without_gc(mut, join_thread, &data);
    if (data.status) {
      errno = data.status;
      perror("Failed to join thread");
      return 1;
    }
  }
  
  printf("Completed in %.3f msec\n", elapsed_millis(start));
  print_end_gc_stats(heap);
}
