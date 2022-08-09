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
                  void (*visit)(struct gc_edge edge, void *visit_data),
                  void *visit_data) {
  for (size_t i = 0; i < 4; i++)
    visit(gc_edge(&quad->kids[i]), visit_data);
}
typedef HANDLE_TO(Quad) QuadHandle;

static Quad* allocate_quad(struct mutator *mut) {
  // memset to 0 by the collector.
  return allocate(mut, ALLOC_KIND_QUAD, sizeof (Quad));
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
static Quad* make_tree(struct mutator *mut, int depth) {
  if (depth<=0) {
    return allocate_quad(mut);
  } else {
    QuadHandle kids[4] = { { NULL }, };
    for (size_t i = 0; i < 4; i++) {
      HANDLE_SET(kids[i], make_tree(mut, depth-1));
      PUSH_HANDLE(mut, kids[i]);
    }

    Quad *result = allocate_quad(mut);
    for (size_t i = 0; i < 4; i++)
      init_field(result, (void**)&result->kids[i], HANDLE_REF(kids[i]));

    for (size_t i = 0; i < 4; i++)
      POP_HANDLE(mut);

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

  unsigned long gc_start = current_time();
  printf("Allocating heap of %.3fGB (%.2f multiplier of live data).\n",
         heap_size / 1e9, multiplier);
  struct heap *heap;
  struct mutator *mut;
  if (!initialize_gc(heap_size, &heap, &mut)) {
    fprintf(stderr, "Failed to initialize GC with heap size %zu bytes\n",
            heap_size);
    return 1;
  }

  QuadHandle quad = { NULL };

  PUSH_HANDLE(mut, quad);

  print_start_gc_stats(heap);

  printf("Making quad tree of depth %zu (%zu nodes).  Total size %.3fGB.\n",
         depth, nquads, (nquads * sizeof(Quad)) / 1e9);
  unsigned long start = current_time();
  HANDLE_SET(quad, make_tree(mut, depth));
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
      make_tree(mut, garbage_depth);
    print_elapsed("allocating garbage", start);

    start = current_time();
    validate_tree(HANDLE_REF(quad), depth);
  }
  print_elapsed("allocation loop", garbage_start);
  print_elapsed("quads test", gc_start);

  print_end_gc_stats(heap);

  POP_HANDLE(mut);
  return 0;
}

