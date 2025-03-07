#ifndef GC_ALLOCATION_KIND_H
#define GC_ALLOCATION_KIND_H

enum gc_allocation_kind {
  // An object whose type can be inspected at run-time based on its contents,
  // and whose fields be traced via the gc_trace_object procedure.
  GC_ALLOCATION_TAGGED,
  // Like GC_ALLOCATION_TAGGED, but not containing any fields that reference
  // GC-managed objects.  The GC may choose to handle these specially.
  GC_ALLOCATION_TAGGED_POINTERLESS,
  // A raw allocation whose type cannot be inspected at trace-time, and whose
  // fields should be traced conservatively.
  GC_ALLOCATION_UNTAGGED_CONSERVATIVE,
  // A raw allocation whose type cannot be inspected at trace-time, but
  // containing no fields that reference GC-managed objects.
  GC_ALLOCATION_UNTAGGED_POINTERLESS
};

#endif // GC_ALLOCATION_KIND_H
