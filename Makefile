TESTS = quads mt-gcbench ephemerons finalizers
COLLECTORS = \
	bdw \
	semi \
	pcc \
	\
	whippet \
	stack-conservative-whippet \
	heap-conservative-whippet \
	\
	parallel-whippet \
	stack-conservative-parallel-whippet \
	heap-conservative-parallel-whippet \
	\
	generational-whippet \
	stack-conservative-generational-whippet \
	heap-conservative-generational-whippet \
	\
	parallel-generational-whippet \
	stack-conservative-parallel-generational-whippet \
	heap-conservative-parallel-generational-whippet

DEFAULT_BUILD := opt

BUILD_CFLAGS_opt      = -O2 -g -DNDEBUG
BUILD_CFLAGS_optdebug = -Og -g -DGC_DEBUG=1
BUILD_CFLAGS_debug    = -O0 -g -DGC_DEBUG=1

BUILD_CFLAGS = $(BUILD_CFLAGS_$(or $(BUILD),$(DEFAULT_BUILD)))

CC       = gcc
CFLAGS   = -Wall -flto -fno-strict-aliasing -fvisibility=hidden -Wno-unused $(BUILD_CFLAGS)
CPPFLAGS = -Iapi
LDFLAGS  = -lpthread -flto=auto
DEPFLAGS = -MMD -MP -MF $(@:obj/%.o=.deps/%.d)
COMPILE  = $(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -o $@
LINK     = $(CC) $(LDFLAGS) -o $@
PLATFORM = gnu-linux

ALL_TESTS = $(foreach COLLECTOR,$(COLLECTORS),$(addsuffix .$(COLLECTOR),$(TESTS)))

all: $(ALL_TESTS:%=bin/%)
.deps obj bin: ; mkdir -p $@

include $(wildcard .deps/*)

obj/gc-platform.o: src/gc-platform-$(PLATFORM).c | .deps obj
	$(COMPILE) -c $<
obj/gc-stack.o: src/gc-stack.c | .deps obj
	$(COMPILE) -c $<
obj/gc-options.o: src/gc-options.c | .deps obj
	$(COMPILE) -c $<
obj/%.gc-ephemeron.o: src/gc-ephemeron.c | .deps obj
	$(COMPILE) -include benchmarks/$*-embedder.h -c $<
obj/%.gc-finalizer.o: src/gc-finalizer.c | .deps obj
	$(COMPILE) -include benchmarks/$*-embedder.h -c $<

GC_STEM_bdw   	   = bdw
GC_CFLAGS_bdw 	   = -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1
GC_IMPL_CFLAGS_bdw = `pkg-config --cflags bdw-gc`
GC_LIBS_bdw        = `pkg-config --libs bdw-gc`

GC_STEM_semi       = semi
GC_CFLAGS_semi     = -DGC_PRECISE_ROOTS=1

GC_STEM_pcc       = pcc
GC_CFLAGS_pcc     = -DGC_PRECISE_ROOTS=1 -DGC_PARALLEL=1

define whippet_variant
GC_STEM_$(1)       = whippet
GC_CFLAGS_$(1)     = $(2)
endef

define generational_whippet_variants
$(call whippet_variant,$(1)whippet,$(2))
$(call whippet_variant,$(1)generational_whippet,$(2) -DGC_GENERATIONAL=1)
endef

define parallel_whippet_variants
$(call generational_whippet_variants,$(1),$(2))
$(call generational_whippet_variants,$(1)parallel_,$(2) -DGC_PARALLEL=1)
endef

define trace_whippet_variants
$(call parallel_whippet_variants,,-DGC_PRECISE_ROOTS=1)
$(call parallel_whippet_variants,stack_conservative_,-DGC_CONSERVATIVE_ROOTS=1)
$(call parallel_whippet_variants,heap_conservative_,-DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1)
endef

$(eval $(call trace_whippet_variants))

# $(1) is the benchmark, $(2) is the collector configuration
make_gc_var    = $$($(1)$(subst -,_,$(2)))
gc_impl        = $(call make_gc_var,GC_STEM_,$(1)).c
gc_attrs       = $(call make_gc_var,GC_STEM_,$(1))-attrs.h
gc_cflags      = $(call make_gc_var,GC_CFLAGS_,$(1))
gc_impl_cflags = $(call make_gc_var,GC_IMPL_CFLAGS_,$(1))
gc_libs        = $(call make_gc_var,GC_LIBS_,$(1))
define benchmark_template
obj/$(1).$(2).gc.o: src/$(call gc_impl,$(2)) | .deps obj
	$$(COMPILE) $(call gc_cflags,$(2)) $(call gc_impl_cflags,$(2)) -include benchmarks/$(1)-embedder.h -c $$<
obj/$(1).$(2).o: benchmarks/$(1).c | .deps obj
	$$(COMPILE) $(call gc_cflags,$(2)) -include api/$(call gc_attrs,$(2)) -c $$<
bin/$(1).$(2): obj/$(1).$(2).gc.o obj/$(1).$(2).o obj/gc-stack.o obj/gc-options.o obj/gc-platform.o obj/$(1).gc-ephemeron.o obj/$(1).gc-finalizer.o | bin
	$$(LINK) $$^ $(call gc_libs,$(2))
endef

$(foreach BENCHMARK,$(TESTS),\
  $(foreach COLLECTOR,$(COLLECTORS),\
    $(eval $(call benchmark_template,$(BENCHMARK),$(COLLECTOR)))))

.PRECIOUS: $(ALL_TESTS) $(OBJS)

clean:
	rm -f $(ALL_TESTS)
	rm -rf .deps obj bin

# Clear some of the default rules.
.SUFFIXES:
.SECONDARY:
%.c:;
Makefile:;
