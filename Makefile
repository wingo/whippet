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

%.bdw.gc.o: src/bdw.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 `pkg-config --cflags bdw-gc` -include benchmarks/$*-embedder.h -c src/bdw.c
%.bdw.o: benchmarks/%.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include api/bdw-attrs.h -c benchmarks/$*.c
%.bdw: %.bdw.o %.bdw.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) `pkg-config --libs bdw-gc` $^

%.semi.gc.o: src/semi.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/semi.c
%.semi.o: benchmarks/%.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include api/semi-attrs.h -c benchmarks/$*.c
%.semi: %.semi.o %.semi.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.whippet: %.whippet.o %.whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.stack-conservative-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.stack-conservative-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.stack-conservative-whippet: %.stack-conservative-whippet.o %.stack-conservative-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.heap-conservative-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.heap-conservative-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.heap-conservative-whippet: %.heap-conservative-whippet.o %.heap-conservative-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.parallel-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.parallel-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.parallel-whippet: %.parallel-whippet.o %.parallel-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.stack-conservative-parallel-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.stack-conservative-parallel-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.stack-conservative-parallel-whippet: %.stack-conservative-parallel-whippet.o %.stack-conservative-parallel-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.heap-conservative-parallel-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.heap-conservative-parallel-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -DGC_FULLY_CONSERVATIVE=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.heap-conservative-parallel-whippet: %.heap-conservative-parallel-whippet.o %.heap-conservative-parallel-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.generational-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.generational-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.generational-whippet: %.generational-whippet.o %.generational-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.stack-conservative-generational-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.stack-conservative-generational-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.stack-conservative-generational-whippet: %.stack-conservative-generational-whippet.o %.stack-conservative-generational-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.heap-conservative-generational-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.heap-conservative-generational-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.heap-conservative-generational-whippet: %.heap-conservative-generational-whippet.o %.heap-conservative-generational-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.parallel-generational-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.parallel-generational-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.parallel-generational-whippet: %.parallel-generational-whippet.o %.parallel-generational-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.stack-conservative-parallel-generational-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.stack-conservative-parallel-generational-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.stack-conservative-parallel-generational-whippet: %.stack-conservative-parallel-generational-whippet.o %.stack-conservative-parallel-generational-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

%.heap-conservative-parallel-generational-whippet.gc.o: src/whippet.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include benchmarks/$*-embedder.h -c src/whippet.c
%.heap-conservative-parallel-generational-whippet.o: benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include api/whippet-attrs.h -c benchmarks/$*.c
%.heap-conservative-parallel-generational-whippet: %.heap-conservative-parallel-generational-whippet.o %.heap-conservative-parallel-generational-whippet.gc.o gc-stack.o gc-options.o gc-platform.o %.gc-ephemeron.o
	$(LINK) $^

.PRECIOUS: $(ALL_TESTS) $(OBJS)

clean:
	rm -f $(ALL_TESTS) $(OBJS) $(DEPS)
