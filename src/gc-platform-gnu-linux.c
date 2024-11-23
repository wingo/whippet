// For pthread_getattr_np.
#define _GNU_SOURCE
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define GC_IMPL 1

#include "debug.h"
#include "gc-align.h"
#include "gc-assert.h"
#include "gc-inline.h"
#include "gc-platform.h"

void gc_platform_init(void) {
  // Nothing to do.
}

static uintptr_t fallback_current_thread_stack_base(void) GC_NEVER_INLINE;
static uintptr_t fallback_current_thread_stack_base(void) {
  // Sloppily assume that there are very few frames between us and the
  // thread entry or main function, and that therefore we haven't
  // consumed more than a page of stack; we can then just round up the
  // stack pointer to the page boundary.
  fprintf(stderr,
          "Using fallback strategy to capture stack base for thread %p.\n",
          (void*)pthread_self());
  int local;
  uintptr_t hot = (uintptr_t)&local;
  size_t page_size = getpagesize();
  return (hot + page_size) & ~(page_size - 1);
}

uintptr_t gc_platform_current_thread_stack_base(void) {
  pthread_t me = pthread_self();
  pthread_attr_t attr;
  int err = pthread_getattr_np(me, &attr);
  if (err) {
    errno = err;
    // This case can occur for the main thread when running in a
    // filesystem without /proc/stat.
    perror("Failed to capture stack base via pthread_getattr_np");
    return fallback_current_thread_stack_base();
  }

  void *stack_low_addr;
  size_t stack_size;
  err = pthread_attr_getstack(&attr, &stack_low_addr, &stack_size);
  pthread_attr_destroy(&attr);
  if (err) {
    // Should never occur.
    errno = err;
    perror("pthread_attr_getstack");
    return fallback_current_thread_stack_base();
  }

  return (uintptr_t)stack_low_addr + stack_size;
}

struct visit_data {
  void (*f)(uintptr_t start, uintptr_t end, struct gc_heap *heap, void *data);
  struct gc_heap *heap;
  void *data;
};

static int visit_roots(struct dl_phdr_info *info, size_t size, void *data) {
  struct visit_data *visit_data = data;
  uintptr_t object_addr = info->dlpi_addr;
  const char *object_name = info->dlpi_name;
  const ElfW(Phdr) *program_headers = info->dlpi_phdr;
  size_t program_headers_count = info->dlpi_phnum;

  // From the loader's perspective, an ELF image is broken up into
  // "segments", each of which is described by a "program header".
  // Treat all writable data segments as potential edges into the
  // GC-managed heap.
  //
  // Note that there are some RELRO segments which are initially
  // writable but then remapped read-only.  BDW-GC will exclude these,
  // but we just punt for the time being and treat them as roots
  for (size_t i = 0; i < program_headers_count; i++) {
    const ElfW(Phdr) *p = &program_headers[i];
    if (p->p_type == PT_LOAD && (p->p_flags & PF_W)) {
      uintptr_t start = p->p_vaddr + object_addr;
      uintptr_t end = start + p->p_memsz;
      DEBUG("found roots for '%s': [%p,%p)\n", object_name,
            (void*)start, (void*)end);
      visit_data->f(start, end, visit_data->heap, visit_data->data);
    }
  }

  return 0;
}

void gc_platform_visit_global_conservative_roots(void (*f)(uintptr_t start,
                                                           uintptr_t end,
                                                           struct gc_heap*,
                                                           void *data),
                                                 struct gc_heap *heap,
                                                 void *data) {
  struct visit_data visit_data = { f, heap, data };
  dl_iterate_phdr(visit_roots, &visit_data);
}

int gc_platform_processor_count(void) {
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof (set), &set) != 0)
    return 1;
  return CPU_COUNT(&set);
}

uint64_t gc_platform_monotonic_nanoseconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts))
    GC_CRASH();
  uint64_t s = ts.tv_sec;
  uint64_t ns = ts.tv_nsec;
  uint64_t ns_per_sec = 1000000000;
  return s * ns_per_sec + ns;
}

size_t gc_platform_page_size(void) {
  return getpagesize();
}

struct gc_reservation gc_platform_reserve_memory(size_t size,
                                                 size_t alignment) {
  GC_ASSERT_EQ(size, align_down(size, getpagesize()));
  GC_ASSERT_EQ(alignment & (alignment - 1), 0);
  GC_ASSERT_EQ(alignment, align_down(alignment, getpagesize()));

  size_t extent = size + alignment;
  void *mem = mmap(NULL, extent, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

  if (mem == MAP_FAILED) {
    perror("failed to reserve address space");
    GC_CRASH();
  }

  uintptr_t base = (uintptr_t) mem;
  uintptr_t end = base + extent;
  uintptr_t aligned_base = alignment ? align_up(base, alignment) : base;
  uintptr_t aligned_end = aligned_base + size;

  if (aligned_base - base)
    munmap((void*)base, aligned_base - base);
  if (end - aligned_end)
    munmap((void*)aligned_end, end - aligned_end);

  return (struct gc_reservation){aligned_base, size};
}

void*
gc_platform_acquire_memory_from_reservation(struct gc_reservation reservation,
                                            size_t offset, size_t size) {
  GC_ASSERT_EQ(size, align_down(size, getpagesize()));
  GC_ASSERT(size <= reservation.size);
  GC_ASSERT(offset <= reservation.size - size);

  void *mem = mmap((void*)(reservation.base + offset), size,
                   PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return NULL;
  }

  return mem;
}

void
gc_platform_release_reservation(struct gc_reservation reservation) {
  if (munmap((void*)reservation.base, reservation.size) != 0)
    perror("failed to unmap memory");
}

void*
gc_platform_acquire_memory(size_t size, size_t alignment) {
  struct gc_reservation reservation =
    gc_platform_reserve_memory(size, alignment);
  return gc_platform_acquire_memory_from_reservation(reservation, 0, size);
}

void gc_platform_release_memory(void *ptr, size_t size) {
  GC_ASSERT_EQ((uintptr_t)ptr, align_down((uintptr_t)ptr, getpagesize()));
  GC_ASSERT_EQ(size, align_down(size, getpagesize()));
  if (munmap(ptr, size) != 0)
    perror("failed to unmap memory");
}

int gc_platform_populate_memory(void *ptr, size_t size) {
  GC_ASSERT_EQ((uintptr_t)ptr, align_down((uintptr_t)ptr, getpagesize()));
  GC_ASSERT_EQ(size, align_down(size, getpagesize()));
  if (madvise(ptr, size, MADV_WILLNEED) == 0)
    return 1;
  perror("failed to populate memory");
  return 0;
}

int gc_platform_discard_memory(void *ptr, size_t size) {
  GC_ASSERT_EQ((uintptr_t)ptr, align_down((uintptr_t)ptr, getpagesize()));
  GC_ASSERT_EQ(size, align_down(size, getpagesize()));
  if (madvise(ptr, size, MADV_DONTNEED) == 0)
    return 1;
  perror("failed to discard memory");
  return 0;
}
