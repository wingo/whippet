#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/time.h>

#include "assert.h"
#include "gc-api.h"
#include "gc-basic-stats.h"
#include "simple-roots-api.h"
#include "quads-types.h"
#include "simple-allocator.h"

typedef HANDLE_TO(Quad) QuadHandle;

static Quad* allocate_quad(struct gc_mutator *mut) {
  // memset to 0 by the collector.
  return gc_allocate_with_kind(mut, ALLOC_KIND_QUAD, sizeof (Quad));
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
  size_t counter;
};

// Build tree bottom-up
static Quad* make_tree(struct thread *t, int depth) {
  if (depth<=0) {
    return allocate_quad(t->mut);
  } else {
    QuadHandle kids[4] = { { NULL }, };
    for (size_t i = 0; i < 4; i++) {
      HANDLE_SET(kids[i], make_tree(t, depth-1));
      PUSH_HANDLE(t, kids[i]);
    }

    Quad *result = allocate_quad(t->mut);
    for (size_t i = 0; i < 4; i++)
      result->kids[i] = HANDLE_REF(kids[i]);

    for (size_t i = 0; i < 4; i++)
      POP_HANDLE(t);

    return result;
  }
}

static void validate_tree(Quad *tree, int depth) {
  for (size_t i = 0; i < 4; i++) {
    if (depth == 0) {
      if (tree->kids[i])
        abort();
    } else {
      if (!tree->kids[i])
        abort();
      validate_tree(tree->kids[i], depth - 1);
    }
  }
}

static void print_elapsed(const char *what, unsigned long start) {
  unsigned long end = current_time();
  unsigned long msec = (end - start) / 1000;
  unsigned long usec = (end - start) % 1000;
  printf("Completed %s in %lu.%.3lu msec\n", what, msec, usec);
}

static size_t parse_size(char *arg, const char *what) {
  long val = atol(arg);
  if (val <= 0) {
    fprintf(stderr, "Failed to parse %s '%s'\n", what, arg);
    exit(1);
  }
  return val;
}

static size_t tree_size(size_t depth) {
  size_t nquads = 0;
  size_t leaf_count = 1;
  for (size_t i = 0; i <= depth; i++) {
    if (nquads > ((size_t)-1) - leaf_count) {
      fprintf(stderr,
              "error: address space too small for quad tree of depth %zu\n",
              depth);
      exit(1);
    }
    nquads += leaf_count;
    leaf_count *= 4;
  }
  return nquads;
}

#define MAX_THREAD_COUNT 256

int main(int argc, char *argv[]) {
  if (argc < 3 || 4 < argc) {
    fprintf(stderr, "usage: %s DEPTH MULTIPLIER [GC-OPTIONS]\n", argv[0]);
    return 1;
  }

  size_t depth = parse_size(argv[1], "depth");
  double multiplier = atof(argv[2]);

  if (!(1.0 < multiplier && multiplier < 100)) {
    fprintf(stderr, "Failed to parse heap multiplier '%s'\n", argv[2]);
    return 1;
  }

  size_t nquads = tree_size(depth);
  size_t tree_bytes = nquads * sizeof(Quad);
  size_t heap_size = tree_bytes * multiplier;

  printf("Allocating heap of %.3fGB (%.2f multiplier of live data).\n",
         heap_size / 1e9, multiplier);

  struct gc_options *options = gc_allocate_options();
  gc_options_set_int(options, GC_OPTION_HEAP_SIZE_POLICY, GC_HEAP_SIZE_FIXED);
  gc_options_set_size(options, GC_OPTION_HEAP_SIZE, heap_size);
  if (argc == 4) {
    if (!gc_options_parse_and_set_many(options, argv[3])) {
      fprintf(stderr, "Failed to set GC options: '%s'\n", argv[3]);
      return 1;
    }
  }

  struct gc_heap *heap;
  struct gc_mutator *mut;
  struct gc_basic_stats stats;
  if (!gc_init(options, gc_empty_stack_addr(), &heap, &mut,
               GC_BASIC_STATS, &stats)) {
    fprintf(stderr, "Failed to initialize GC with heap size %zu bytes\n",
            heap_size);
    return 1;
  }
  struct thread t = { mut, };
  gc_mutator_set_roots(mut, &t.roots);

  QuadHandle quad = { NULL };

  PUSH_HANDLE(&t, quad);

  printf("Making quad tree of depth %zu (%zu nodes).  Total size %.3fGB.\n",
         depth, nquads, (nquads * sizeof(Quad)) / 1e9);
  unsigned long start = current_time();
  HANDLE_SET(quad, make_tree(&t, depth));
  print_elapsed("construction", start);

  validate_tree(HANDLE_REF(quad), depth);

  size_t garbage_step = heap_size / 7.5;
  printf("Allocating %.3f GB of garbage, 20 times, validating live tree each time.\n",
         garbage_step / 1e9);
  unsigned long garbage_start = current_time();
  for (size_t i = 0; i < 20; i++) {
    size_t garbage_depth = 3;
    start = current_time();
    for (size_t i = garbage_step/(tree_size(garbage_depth)*4*sizeof(Quad*)); i; i--)
      make_tree(&t, garbage_depth);
    print_elapsed("allocating garbage", start);

    start = current_time();
    validate_tree(HANDLE_REF(quad), depth);
  }
  print_elapsed("allocation loop", garbage_start);

  gc_basic_stats_finish(&stats);
  fputs("\n", stdout);
  gc_basic_stats_print(&stats, stdout);

  POP_HANDLE(&t);
  return 0;
}

