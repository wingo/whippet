// This is version 2 of the multithreaded GC Bench.
// Heap expansion is handled differently from version 1, in an attempt
// to make scalability measurements more meaningful.  The version with
// N threads now immediately expands the heap to N*32MB.
//
// To run this with BDWGC versions 6 and later with thread local allocation,
// define GC and LOCAL.  Without thread-local allocation, define just GC.
// To run it with the University of Tokyo scalable GC,
// define SGC.  To run it with malloc and explicit deallocation, define
// none of these.  (This should also work for Hoard.)
//
// Note that defining GC or SGC removes the explicit deallocation passes,
// which seems fair.
// 
// This is adapted from a benchmark written by John Ellis and Pete Kovac
// of Post Communications.
// It was modified by Hans Boehm of Silicon Graphics.
// Translated to C++ 30 May 1997 by William D Clinger of Northeastern Univ.
// Translated to C 15 March 2000 by Hans Boehm, now at HP Labs.
// Adapted to run NTHREADS client threads concurrently.  Each
// thread executes the original benchmark.  12 June 2000  by Hans Boehm.
// Changed heap expansion rule, and made the number of threads run-time
// configurable.  25 Oct 2000 by Hans Boehm.
//
//      This is no substitute for real applications.  No actual application
//      is likely to behave in exactly this way.  However, this benchmark was
//      designed to be more representative of real applications than other
//      Java GC benchmarks of which we were aware at the time.
//      It still doesn't seem too bad for something this small.
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
//      of how much memory was used.  This versions of the benchmark tries
//      to preallocate a sufficiently large heap that expansion should not be
//      needed.
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
//
//      Since this benchmark has recently been more widely used, some
//      anomalous behavious has been uncovered.  The user should be aware
//      of this:
//      1) Nearly all objects are of the same size.  This benchmark is
//         not useful for analyzing fragmentation behavior.  It is unclear
//         whether this is an issue for well-designed allocators.
//      2) Unless HOLES is defined, it tends to drop consecutively allocated
//         memory at the same time.  Many real applications do exhibit this
//         phenomenon, but probably not to this extent.  (Defining HOLES tends
//         to move the benchmark to the opposite extreme.)
//      3) It appears harder to predict object lifetimes than for most real
//         Java programs (see T. Harris, "Dynamic adptive pre-tenuring",
//         ISMM '00).

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <pthread.h>

#ifdef GC
#  ifndef LINUX_THREADS
#     define LINUX_THREADS
#  endif
#  ifndef _REENTRANT
#     define _REENTRANT
#  endif
#  ifdef LOCAL
#    define GC_REDIRECT_TO_LOCAL
#    include "gc_local_alloc.h"
#  endif
#  include "gc.h"
#endif
#ifdef SGC
#  include "sgc.h"
#  define GC
#  define pthread_create GC_pthread_create
#  define pthread_join GC_pthread_join
#endif

#define MAX_NTHREADS 1024

int nthreads = 0;

#ifdef PROFIL
  extern void init_profiling();
  extern dump_profile();
#endif

//  These macros were a quick hack for the Macintosh.
//
//  #define currentTime() clock()
//  #define elapsedTime(x) ((1000*(x))/CLOCKS_PER_SEC)

#define currentTime() stats_rtclock()
#define elapsedTime(x) (x)

/* Get the current time in milliseconds */

unsigned
stats_rtclock( void )
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

typedef struct Node0_struct {
        struct Node0_struct * left;
        struct Node0_struct * right;
        int i, j;
} Node0;

#ifdef HOLES
#   define HOLE() GC_NEW(Node0);
#else
#   define HOLE()
#endif

typedef Node0 *Node;

void init_Node(Node me, Node l, Node r) {
    me -> left = l;
    me -> right = r;
}

#ifndef GC
  void destroy_Node(Node me) {
    if (me -> left) {
	destroy_Node(me -> left);
    }
    if (me -> right) {
	destroy_Node(me -> right);
    }
    free(me);
  }
#endif

// Nodes used by a tree of a given size
static int TreeSize(int i) {
        return ((1 << (i + 1)) - 1);
}

// Number of iterations to use for a given tree depth
static int NumIters(int i) {
        return 2 * TreeSize(kStretchTreeDepth) / TreeSize(i);
}

// Build tree top down, assigning to older objects.
static void Populate(int iDepth, Node thisNode) {
        if (iDepth<=0) {
                return;
        } else {
                iDepth--;
#		ifdef GC
                  thisNode->left  = GC_NEW(Node0); HOLE();
                  thisNode->right = GC_NEW(Node0); HOLE();
#		else
                  thisNode->left  = calloc(1, sizeof(Node0));
                  thisNode->right = calloc(1, sizeof(Node0));
#		endif
                Populate (iDepth, thisNode->left);
                Populate (iDepth, thisNode->right);
        }
}

// Build tree bottom-up
static Node MakeTree(int iDepth) {
	Node result;
        if (iDepth<=0) {
#	    ifndef GC
		result = calloc(1, sizeof(Node0));
#	    else
		result = GC_NEW(Node0); HOLE();
#	    endif
	    /* result is implicitly initialized in both cases. */
	    return result;
        } else {
	    Node left = MakeTree(iDepth-1);
	    Node right = MakeTree(iDepth-1);
#	    ifndef GC
		result = malloc(sizeof(Node0));
#	    else
		result = GC_NEW(Node0); HOLE();
#	    endif
	    init_Node(result, left, right);
	    return result;
        }
}

static void PrintDiagnostics() {
#if 0
        long lFreeMemory = Runtime.getRuntime().freeMemory();
        long lTotalMemory = Runtime.getRuntime().totalMemory();

        System.out.print(" Total memory available="
                         + lTotalMemory + " bytes");
        System.out.println("  Free memory=" + lFreeMemory + " bytes");
#endif
}

static void TimeConstruction(int depth) {
        long    tStart, tFinish;
        int     iNumIters = NumIters(depth);
        Node    tempTree;
	int 	i;

	printf("0x%x: Creating %d trees of depth %d\n", pthread_self(), iNumIters, depth);
        
        tStart = currentTime();
        for (i = 0; i < iNumIters; ++i) {
#		ifndef GC
                  tempTree = calloc(1, sizeof(Node0));
#		else
                  tempTree = GC_NEW(Node0);
#		endif
                Populate(depth, tempTree);
#		ifndef GC
                  destroy_Node(tempTree);
#		endif
                tempTree = 0;
        }
        tFinish = currentTime();
        printf("\t0x%x: Top down construction took %d msec\n",
               pthread_self(), elapsedTime(tFinish - tStart));
             
        tStart = currentTime();
        for (i = 0; i < iNumIters; ++i) {
                tempTree = MakeTree(depth);
#		ifndef GC
                  destroy_Node(tempTree);
#		endif
                tempTree = 0;
        }
        tFinish = currentTime();
        printf("\t0x%x: Bottom up construction took %d msec\n",
               pthread_self(), elapsedTime(tFinish - tStart));

}

void * run_one_test(void * arg) {
	int d, i;
        Node    longLivedTree;
	double 	*array;
	/* size_t initial_bytes = GC_get_total_bytes(); */

        // Create a long lived object
        printf(" Creating a long-lived binary tree of depth %d\n",
               kLongLivedTreeDepth);
#	ifndef GC
          longLivedTree = calloc(1, sizeof(Node0));
#	else 
          longLivedTree = GC_NEW(Node0);
#	endif
        Populate(kLongLivedTreeDepth, longLivedTree);

        // Create long-lived array, filling half of it
	printf(" Creating a long-lived array of %d doubles\n", kArraySize);
#	ifndef GC
          array = malloc(kArraySize * sizeof(double));
#	else
#	  ifndef NO_PTRFREE
            array = GC_MALLOC_ATOMIC(sizeof(double) * kArraySize);
#	  else
            array = GC_MALLOC(sizeof(double) * kArraySize);
#	  endif
#	endif
        for (i = 0; i < kArraySize/2; ++i) {
                array[i] = 1.0/i;
        }

        for (d = kMinTreeDepth; d <= kMaxTreeDepth; d += 2) {
                TimeConstruction(d);
        }
	/* printf("Allocated %ld bytes before start, %ld after\n",
		initial_bytes, GC_get_total_bytes() - initial_bytes); */
        if (longLivedTree->left -> right == 0 || array[1000] != 1.0/1000)
		fprintf(stderr, "Failed\n");
                                // fake reference to LongLivedTree
                                // and array
                                // to keep them from being optimized away

}

int main(int argc, char **argv) {
        Node    root;
        Node    tempTree[MAX_NTHREADS];
        long    tStart, tFinish;
        long    tElapsed;
  	int	i;
#	ifdef SGC
	  SGC_attr_t attr;
#	endif

	if (1 == argc) {
	    nthreads = 1;
	} else if (2 == argc) {
	    nthreads = atoi(argv[1]);
	    if (nthreads < 1 || nthreads > MAX_NTHREADS) {
		fprintf(stderr, "Invalid # of threads argument\n");
		exit(1);
	    }
	} else {
	    fprintf(stderr, "Usage: %s [# of threads]\n");
	    exit(1);
	}
#       if defined(SGC)
	  /* The University of Tokyo collector needs explicit	*/
	  /* initialization.					*/
	  SGC_attr_init(&attr);
	  SGC_init(nthreads, &attr);
#  	endif
#ifdef GC
 // GC_full_freq = 30;
 // GC_free_space_divisor = 16;
 // GC_enable_incremental();
#endif
	printf("Garbage Collector Test\n");
 	printf(" Live storage will peak at %d bytes or less .\n\n",
               2 * sizeof(Node0) * nthreads
	         * (TreeSize(kLongLivedTreeDepth) + TreeSize(kMaxTreeDepth))
               + sizeof(double) * kArraySize);
        PrintDiagnostics();
        
#	ifdef GC
	  /* GC_expand_hp fails with empty heap */
	  GC_malloc(1);
	  GC_expand_hp(32*1024*1024*nthreads);
#	endif

#	ifdef PROFIL
	    init_profiling();
#	endif
       
        tStart = currentTime();
        {
	  pthread_t thread[MAX_NTHREADS];
	  for (i = 1; i < nthreads; ++i) {
    	    int code;

	    if ((code = pthread_create(thread+i, 0, run_one_test, 0)) != 0) {
    	      fprintf(stderr, "Thread creation failed %u\n", code);
	      exit(1);
	    }
	  }
	  /* We use the main thread to run one test.  This allows	*/
	  /* profiling to work, for example.				*/
	  run_one_test(0);
	  for (i = 1; i < nthreads; ++i) {
    	    int code;
	    if ((code = pthread_join(thread[i], 0)) != 0) {
        	fprintf(stderr, "Thread join failed %u\n", code);
      	    }
 	  }
        }
        PrintDiagnostics();

        tFinish = currentTime();
        tElapsed = elapsedTime(tFinish-tStart);
        PrintDiagnostics();
        printf("Completed in %d msec\n", tElapsed);
#	ifdef GC
	  printf("Completed %d collections\n", GC_gc_no);
	  printf("Heap size is %d\n", GC_get_heap_size());
#       endif
#	ifdef PROFIL
	  dump_profile();
#	endif
        return 0;
}

