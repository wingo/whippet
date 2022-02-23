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

typedef struct Node {
  struct Node * left;
  struct Node * right;
  int i, j;
} Node;

#ifdef GC_BDW
#include "bdw.h"
#else
#error unknown gc
#endif

/* Get the current time in milliseconds */
static unsigned currentTime(void)
{
  struct timeval t;
  struct timezone tz;

  if (gettimeofday( &t, &tz ) == -1)
    return 0;
  return (t.tv_sec * 1000 + t.tv_usec / 1000);
}

static const int kStretchTreeDepth    = 18;      // about 16Mb
static const int kLongLivedTreeDepth  = 16;  // about 4Mb
static const int kArraySize  = 500000;  // about 4Mb
static const int kMinTreeDepth = 4;
static const int kMaxTreeDepth = 16;

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
static void Populate(int iDepth, Node *node) {
  if (iDepth<=0) {
    return;
  } else {
    iDepth--;
    
    NodeHandle self = { node };
    PUSH_HANDLE(self);
    NodeHandle l = { allocate_node() };
    PUSH_HANDLE(l);
    NodeHandle r = { allocate_node() };
    PUSH_HANDLE(r);
    set_field((void**)&HANDLE_REF(self)->left, HANDLE_REF(l));
    set_field((void**)&HANDLE_REF(self)->right, HANDLE_REF(r));
    Populate (iDepth, HANDLE_REF(self)->left);
    Populate (iDepth, HANDLE_REF(self)->right);
    POP_HANDLE(r);
    POP_HANDLE(l);
    POP_HANDLE(self);
  }
}

// Build tree bottom-up
static Node* MakeTree(int iDepth) {
  if (iDepth<=0) {
    return allocate_node();
  } else {
    NodeHandle left = { MakeTree(iDepth-1) };
    PUSH_HANDLE(left);
    NodeHandle right = { MakeTree(iDepth-1) };
    PUSH_HANDLE(right);
    Node *result = allocate_node();
    init_Node(result, HANDLE_REF(left), HANDLE_REF(right));
    POP_HANDLE(left);
    POP_HANDLE(right);
    return result;
  }
}

static void TimeConstruction(int depth) {
  int iNumIters = NumIters(depth);
  NodeHandle tempTree = { NULL };
  PUSH_HANDLE(tempTree);

  printf("Creating %d trees of depth %d\n", iNumIters, depth);

  {
    long tStart = currentTime();
    for (int i = 0; i < iNumIters; ++i) {
      HANDLE_SET(tempTree, allocate_node());
      Populate(depth, HANDLE_REF(tempTree));
      HANDLE_SET(tempTree, NULL);
    }
    long tFinish = currentTime();
    printf("\tTop down construction took %ld msec\n",
           tFinish - tStart);
  }

  {
    long tStart = currentTime();
    for (int i = 0; i < iNumIters; ++i) {
      HANDLE_SET(tempTree, MakeTree(depth));
      HANDLE_SET(tempTree, NULL);
    }
    long tFinish = currentTime();
    printf("\tBottom up construction took %ld msec\n",
           tFinish - tStart);
  }

  POP_HANDLE(tempTree);
}

int main() {
  NodeHandle root = { NULL };
  NodeHandle longLivedTree = { NULL };
  NodeHandle tempTree = { NULL };
  HANDLE_TO(double) array = { NULL };

  PUSH_HANDLE(root);
  PUSH_HANDLE(longLivedTree);
  PUSH_HANDLE(tempTree);
  PUSH_HANDLE(array);

  initialize_gc();

  printf("Garbage Collector Test\n");
  printf(" Live storage will peak at %zd bytes.\n\n",
         2 * sizeof(struct Node) * TreeSize(kLongLivedTreeDepth) +
         sizeof(double) * kArraySize);
  printf(" Stretching memory with a binary tree of depth %d\n",
         kStretchTreeDepth);
  print_start_gc_stats();
       
  long tStart = currentTime();
        
  // Stretch the memory space quickly
  HANDLE_SET(tempTree, MakeTree(kStretchTreeDepth));
  HANDLE_SET(tempTree, NULL);

  // Create a long lived object
  printf(" Creating a long-lived binary tree of depth %d\n",
         kLongLivedTreeDepth);
  HANDLE_SET(longLivedTree, allocate_node());
  Populate(kLongLivedTreeDepth, HANDLE_REF(longLivedTree));

  // Create long-lived array, filling half of it
  printf(" Creating a long-lived array of %d doubles\n", kArraySize);
  HANDLE_SET(array, allocate_double_array(kArraySize));
  for (int i = 0; i < kArraySize/2; ++i) {
    HANDLE_REF(array)[i] = 1.0/i;
  }

  for (int d = kMinTreeDepth; d <= kMaxTreeDepth; d += 2) {
    TimeConstruction(d);
  }

  if (HANDLE_REF(longLivedTree) == 0 || HANDLE_REF(array)[1000] != 1.0/1000)
    fprintf(stderr, "Failed\n");
  // fake reference to LongLivedTree
  // and array
  // to keep them from being optimized away

  long tFinish = currentTime();
  long tElapsed = tFinish - tStart;
  printf("Completed in %ld msec\n", tElapsed);
  print_end_gc_stats();

  POP_HANDLE(array);
  POP_HANDLE(tempTree);
  POP_HANDLE(longLivedTree);
  POP_HANDLE(root);
}

