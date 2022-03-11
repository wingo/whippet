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

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "assert.h"
#include "gcbench-types.h"
#include "gc.h"

static const int kStretchTreeDepth    = 18;      // about 16Mb
static const int kLongLivedTreeDepth  = 16;  // about 4Mb
static const int kArraySize  = 500000;  // about 4Mb
static const int kMinTreeDepth = 4;
static const int kMaxTreeDepth = 16;

typedef struct Node {
  GC_HEADER;
  struct Node * left;
  struct Node * right;
  int i, j;
} Node;

typedef struct DoubleArray {
  GC_HEADER;
  size_t length;
  double values[0];
} DoubleArray;

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

static Node* allocate_node(struct context *cx) {
  // memset to 0 by the collector.
  return allocate(cx, ALLOC_KIND_NODE, sizeof (Node));
}

static struct DoubleArray* allocate_double_array(struct context *cx,
                                                 size_t size) {
  // note, we might allow the collector to leave this data uninitialized.
  DoubleArray *ret = allocate(cx, ALLOC_KIND_DOUBLE_ARRAY,
                              sizeof(DoubleArray) + sizeof (double) * size);
  ret->length = size;
  return ret;
}

/* Get the current time in milliseconds */
static unsigned currentTime(void)
{
  struct timeval t;
  struct timezone tz;

  if (gettimeofday( &t, &tz ) == -1)
    return 0;
  return (t.tv_sec * 1000 + t.tv_usec / 1000);
}

void init_Node(Node *me, Node *l, Node *r) {
  init_field((void**)&me->left, l);
  init_field((void**)&me->right, r);
}

// Nodes used by a tree of a given size
static int TreeSize(int i) {
  return ((1 << (i + 1)) - 1);
}

// Number of iterations to use for a given tree depth
static int NumIters(int i) {
  return 2 * TreeSize(kStretchTreeDepth) / TreeSize(i);
}

// Build tree top down, assigning to older objects.
static void Populate(struct context *cx, int iDepth, Node *node) {
  if (iDepth<=0) {
    return;
  } else {
    iDepth--;
    
    NodeHandle self = { node };
    PUSH_HANDLE(cx, self);
    NodeHandle l = { allocate_node(cx) };
    PUSH_HANDLE(cx, l);
    NodeHandle r = { allocate_node(cx) };
    PUSH_HANDLE(cx, r);
    set_field((void**)&HANDLE_REF(self)->left, HANDLE_REF(l));
    set_field((void**)&HANDLE_REF(self)->right, HANDLE_REF(r));
    Populate (cx, iDepth, HANDLE_REF(self)->left);
    Populate (cx, iDepth, HANDLE_REF(self)->right);
    POP_HANDLE(cx, r);
    POP_HANDLE(cx, l);
    POP_HANDLE(cx, self);
  }
}

// Build tree bottom-up
static Node* MakeTree(struct context *cx, int iDepth) {
  if (iDepth<=0) {
    return allocate_node(cx);
  } else {
    NodeHandle left = { MakeTree(cx, iDepth-1) };
    PUSH_HANDLE(cx, left);
    NodeHandle right = { MakeTree(cx, iDepth-1) };
    PUSH_HANDLE(cx, right);
    Node *result = allocate_node(cx);
    init_Node(result, HANDLE_REF(left), HANDLE_REF(right));
    POP_HANDLE(cx, right);
    POP_HANDLE(cx, left);
    return result;
  }
}

static void ValidateTree(Node *tree, int depth) {
#ifndef NDEBUG
  ASSERT_EQ(tree->i, 0);
  ASSERT_EQ(tree->j, 0);
  if (depth == 0) {
    ASSERT(!tree->left);
    ASSERT(!tree->right);
  } else {
    ASSERT(tree->left);
    ASSERT(tree->right);
    ValidateTree(tree->left, depth - 1);
    ValidateTree(tree->right, depth - 1);
  }
#endif
}

static void TimeConstruction(struct context *cx, int depth) {
  int iNumIters = NumIters(depth);
  NodeHandle tempTree = { NULL };
  PUSH_HANDLE(cx, tempTree);

  printf("Creating %d trees of depth %d\n", iNumIters, depth);

  {
    long tStart = currentTime();
    for (int i = 0; i < iNumIters; ++i) {
      HANDLE_SET(tempTree, allocate_node(cx));
      Populate(cx, depth, HANDLE_REF(tempTree));
      ValidateTree(HANDLE_REF(tempTree), depth);
      HANDLE_SET(tempTree, NULL);
    }
    long tFinish = currentTime();
    printf("\tTop down construction took %ld msec\n",
           tFinish - tStart);
  }

  {
    long tStart = currentTime();
    for (int i = 0; i < iNumIters; ++i) {
      HANDLE_SET(tempTree, MakeTree(cx, depth));
      ValidateTree(HANDLE_REF(tempTree), depth);
      HANDLE_SET(tempTree, NULL);
    }
    long tFinish = currentTime();
    printf("\tBottom up construction took %ld msec\n",
           tFinish - tStart);
  }

  POP_HANDLE(cx, tempTree);
}

int main() {
  size_t kHeapMaxLive =
    2 * sizeof(struct Node) * TreeSize(kLongLivedTreeDepth) +
    sizeof(double) * kArraySize;
  double kHeapMultiplier = 3;
  size_t kHeapSize = kHeapMaxLive * kHeapMultiplier;

  if (getenv("HEAP_SIZE"))
    kHeapSize = atol(getenv("HEAP_SIZE"));
  if (!kHeapSize) {
    fprintf(stderr, "Failed to parse HEAP_SIZE='%s'\n", getenv("HEAP_SIZE"));
    return 1;
  }

  struct context _cx;
  struct context *cx = &_cx;
  initialize_gc(cx, kHeapSize);

  NodeHandle root = { NULL };
  NodeHandle longLivedTree = { NULL };
  NodeHandle tempTree = { NULL };
  DoubleArrayHandle array = { NULL };

  PUSH_HANDLE(cx, root);
  PUSH_HANDLE(cx, longLivedTree);
  PUSH_HANDLE(cx, tempTree);
  PUSH_HANDLE(cx, array);

  printf("Garbage Collector Test\n");
  printf(" Live storage will peak at %zd bytes.\n\n", kHeapMaxLive);
  printf(" Stretching memory with a binary tree of depth %d\n",
         kStretchTreeDepth);
  print_start_gc_stats(cx);
       
  long tStart = currentTime();
        
  // Stretch the memory space quickly
  HANDLE_SET(tempTree, MakeTree(cx, kStretchTreeDepth));
  ValidateTree(HANDLE_REF(tempTree), kStretchTreeDepth);
  HANDLE_SET(tempTree, NULL);

  // Create a long lived object
  printf(" Creating a long-lived binary tree of depth %d\n",
         kLongLivedTreeDepth);
  HANDLE_SET(longLivedTree, allocate_node(cx));
  Populate(cx, kLongLivedTreeDepth, HANDLE_REF(longLivedTree));

  // Create long-lived array, filling half of it
  printf(" Creating a long-lived array of %d doubles\n", kArraySize);
  HANDLE_SET(array, allocate_double_array(cx, kArraySize));
  for (int i = 0; i < kArraySize/2; ++i) {
    HANDLE_REF(array)->values[i] = 1.0/i;
  }

  for (int d = kMinTreeDepth; d <= kMaxTreeDepth; d += 2) {
    TimeConstruction(cx, d);
  }

  ValidateTree(HANDLE_REF(longLivedTree), kLongLivedTreeDepth);

  if (HANDLE_REF(longLivedTree) == 0
      || HANDLE_REF(array)->values[1000] != 1.0/1000)
    fprintf(stderr, "Failed\n");
  // fake reference to LongLivedTree
  // and array
  // to keep them from being optimized away

  long tFinish = currentTime();
  long tElapsed = tFinish - tStart;
  printf("Completed in %ld msec\n", tElapsed);
  print_end_gc_stats(cx);

  POP_HANDLE(cx, array);
  POP_HANDLE(cx, tempTree);
  POP_HANDLE(cx, longLivedTree);
  POP_HANDLE(cx, root);
}

