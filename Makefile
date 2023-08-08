TESTS=quads mt-gcbench ephemerons # MT_GCBench MT_GCBench2
COLLECTORS= \
	bdw \
	semi \
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

DEFAULT_BUILD:=opt

BUILD_CFLAGS_opt=-O2 -g -DNDEBUG
BUILD_CFLAGS_optdebug=-Og -g -DGC_DEBUG=1
BUILD_CFLAGS_debug=-O0 -g -DGC_DEBUG=1

BUILD_CFLAGS=$(BUILD_CFLAGS_$(or $(BUILD),$(DEFAULT_BUILD)))

CC=gcc
CFLAGS=-Wall -flto -fno-strict-aliasing -fvisibility=hidden -Wno-unused $(BUILD_CFLAGS)
CPPFLAGS=-Iapi
LDFLAGS=-lpthread -flto
DEPFLAGS=-MMD -MP -MF $(@:%.o=.deps/%.d)
OUTPUT_OPTION=$(DEPFLAGS) -o $@
COMPILE=$(CC) $(CFLAGS) $(CPPFLAGS) $(OUTPUT_OPTION)
LINK=$(CC) $(LDFLAGS) -o $@
PLATFORM=gnu-linux

ALL_TESTS=$(foreach COLLECTOR,$(COLLECTORS),$(addsuffix .$(COLLECTOR),$(TESTS)))

all: $(ALL_TESTS)

OBJS=gc-platform.o gc-stack.o gc-options.o
OBJS+=$(foreach TEST,$(TESTS),$(TEST).gc-ephemeron.o)
OBJS+=$(foreach TEST,$(ALL_TESTS),$(TEST).gc.o $(TEST).o)
DEPS=$(OBJS:%.o=.deps/%.d)
$(OBJS): | .deps
.deps: ; mkdir -p .deps
include $(wildcard $(DEPS))

gc-platform.o: src/gc-platform-$(PLATFORM).c
	$(COMPILE) -c $<
gc-stack.o: src/gc-stack.c
	$(COMPILE) -c $<
gc-options.o: src/gc-options.c
	$(COMPILE) -c $<
%.gc-ephemeron.o: src/gc-ephemeron.c
	$(COMPILE) -include benchmarks/$*-embedder.h -c $<

GC_STEM_bdw=bdw
GC_CFLAGS_bdw=-DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1
GC_IMPL_CFLAGS_bdw=`pkg-config --cflags bdw-gc`
GC_LIBS_bdw=`pkg-config --libs bdw-gc`

GC_STEM_semi=semi
GC_CFLAGS_semi=-DGC_PRECISE_ROOTS=1

define whippet_variant
GC_STEM_$(1)=whippet
GC_CFLAGS_$(1)=$(2)
endef

$(eval $(call whippet_variant,whippet,\
              -DGC_PRECISE_ROOTS=1))
$(eval $(call whippet_variant,stack_conservative_whippet,\
              -DGC_CONSERVATIVE_ROOTS=1))
$(eval $(call whippet_variant,heap_conservative_whippet,\
              -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1))

$(eval $(call whippet_variant,parallel_whippet,\
              -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1))
$(eval $(call whippet_variant,stack_conservative_parallel_whippet,\
              -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1))
$(eval $(call whippet_variant,heap_conservative_parallel_whippet,\
              -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1))

$(eval $(call whippet_variant,generational_whippet,\
              -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1))
$(eval $(call whippet_variant,stack_conservative_generational_whippet,\
              -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1))
$(eval $(call whippet_variant,heap_conservative_generational_whippet,\
              -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1))

$(eval $(call whippet_variant,parallel_generational_whippet,\
              -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1))
$(eval $(call whippet_variant,stack_conservative_parallel_generational_whippet,\
              -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1))
$(eval $(call whippet_variant,heap_conservative_parallel_generational_whippet,\
              -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1))

# $(1) is the benchmark, $(2) is the collector configuration
# gc_stem for bdw: bdw
make_gc_var=$$($(1)$(subst -,_,$(2)))
gc_impl=$(call make_gc_var,GC_STEM_,$(1)).c
gc_attrs=$(call make_gc_var,GC_STEM_,$(1))-attrs.h
gc_cflags=$(call make_gc_var,GC_CFLAGS_,$(1))
gc_impl_cflags=$(call make_gc_var,GC_IMPL_CFLAGS_,$(1))
gc_libs=$(call make_gc_var,GC_LIBS_,$(1))
define benchmark_template
$(1).$(2).gc.o: src/$(call gc_impl,$(2))
	$$(COMPILE) $(call gc_cflags,$(2)) $(call gc_impl_cflags,$(2)) -include benchmarks/$(1)-embedder.h -c $$<
$(1).$(2).o: benchmarks/$(1).c
	$$(COMPILE) $(call gc_cflags,$(2)) -include api/$(call gc_attrs,$(2)) -c $$<
$(1).$(2): $(1).$(2).gc.o $(1).$(2).o gc-stack.o gc-options.o gc-platform.o $(1).gc-ephemeron.o
	$$(LINK) $(call gc_libs,$(2)) $$^
endef

$(foreach BENCHMARK,$(TESTS),\
  $(foreach COLLECTOR,$(COLLECTORS),\
    $(eval $(call benchmark_template,$(BENCHMARK),$(COLLECTOR)))))

.PRECIOUS: $(ALL_TESTS) $(OBJS)

clean:
	rm -f $(ALL_TESTS) $(OBJS) $(DEPS)
