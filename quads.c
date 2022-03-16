#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "assert.h"
#include "quads-types.h"
#include "gc.h"

typedef struct Quad {
  GC_HEADER;
  struct Quad *kids[4];
} Quad;
static inline size_t quad_size(Quad *obj) {
  return sizeof(Quad);
}
static inline void
visit_quad_fields(Quad *quad,
                  void (*visit)(void **loc, void *visit_data),
                  void *visit_data) {
  for (size_t i = 0; i < 4; i++)
    visit((void**)&quad->kids[i], visit_data);
}
typedef HANDLE_TO(Quad) QuadHandle;

static Quad* allocate_quad(struct context *cx) {
  // memset to 0 by the collector.
  return allocate(cx, ALLOC_KIND_QUAD, sizeof (Quad));
}

/* Get the current time in microseconds */
static unsigned long current_time(void)
{
  struct timeval t;
  if (gettimeofday(&t, NULL) == -1)
    return 0;
  return t.tv_sec * 1000 * 1000 + t.tv_usec;
}

// Build tree bottom-up
static Quad* make_tree(struct context *cx, int depth) {
  if (depth<=0) {
    return allocate_quad(cx);
  } else {
    QuadHandle kids[4] = { { NULL }, };
    for (size_t i = 0; i < 4; i++) {
      HANDLE_SET(kids[i], make_tree(cx, depth-1));
      PUSH_HANDLE(cx, kids[i]);
    }

    Quad *result = allocate_quad(cx);
    for (size_t i = 0; i < 4; i++)
      init_field((void**)&result->kids[i], HANDLE_REF(kids[i]));

    for (size_t i = 0; i < 4; i++)
      POP_HANDLE(cx, kids[3 - i]);

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


int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s DEPTH MULTIPLIER\n", argv[0]);
    return 1;
  }

  size_t depth = parse_size(argv[1], "depth");
  double multiplier = atof(argv[2]);

  if (!(1.0 < multiplier && multiplier < 100)) {
    fprintf(stderr, "Failed to parse heap multiplier '%s'\n", argv[2]);
    return 1;
  }

  // Compute byte size not counting any header word, so as to compute the same
  // heap size whether a header word is there or not.
  size_t nquads = tree_size(depth);
  size_t tree_bytes = nquads * 4 * sizeof(Quad*);
  size_t heap_size = tree_bytes * multiplier;

  printf("Allocating heap of %.3fGB (%.2f multiplier of live data).\n",
         heap_size / 1e9, multiplier);
  struct context _cx;
  struct context *cx = &_cx;
  initialize_gc(cx, heap_size);

  QuadHandle quad = { NULL };

  PUSH_HANDLE(cx, quad);

  print_start_gc_stats(cx);

  printf("Making quad tree of depth %zu (%zu nodes).  Total size %.3fGB.\n",
         depth, nquads, (nquads * sizeof(Quad)) / 1e9);
  unsigned long start = current_time();
  HANDLE_SET(quad, make_tree(cx, depth));
  print_elapsed("construction", start);

  validate_tree(HANDLE_REF(quad), depth);

  for (size_t i = 0; i < 10; i++) {
    printf("Allocating 1 GB of garbage.\n");
    size_t garbage_depth = 3;
    start = current_time();
    for (size_t i = 1e9/(tree_size(garbage_depth)*4*sizeof(Quad*)); i; i--)
      make_tree(cx, garbage_depth);
    print_elapsed("allocating garbage", start);

#if 0
#ifdef LAZY_SWEEP
    start = current_time();
    do {} while (sweep(cx));
    print_elapsed("finishing lazy sweep", start);
#endif

    start = current_time();
    collect(cx);
    print_elapsed("collection", start);
#endif

    start = current_time();
    validate_tree(HANDLE_REF(quad), depth);
    print_elapsed("validate tree", start);
  }

  print_end_gc_stats(cx);

  POP_HANDLE(cx, quad);
  return 0;
}

