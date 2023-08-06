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
INCLUDES=-Iapi
LDFLAGS=-lpthread -flto
COMPILE=$(CC) $(CFLAGS) $(INCLUDES)
PLATFORM=gnu-linux

ALL_TESTS=$(foreach COLLECTOR,$(COLLECTORS),$(addprefix $(COLLECTOR)-,$(TESTS)))

all: $(ALL_TESTS)

gc-platform.o: src/gc-platform.h src/gc-platform-$(PLATFORM).c api/gc-visibility.h
	$(COMPILE) -o $@ -c src/gc-platform-$(PLATFORM).c

gc-stack.o: src/gc-stack.c
	$(COMPILE) -o $@ -c $<

gc-options.o: src/gc-options.c api/gc-options.h src/gc-options-internal.h
	$(COMPILE) -o $@ -c $<

gc-ephemeron-%.o: src/gc-ephemeron.c api/gc-ephemeron.h src/gc-ephemeron-internal.h benchmarks/%-embedder.h
	$(COMPILE) -include benchmarks/$*-embedder.h -o $@ -c $<

bdw-%-gc.o: src/bdw.c benchmarks/%-embedder.h benchmarks/%.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 `pkg-config --cflags bdw-gc` -include benchmarks/$*-embedder.h -o $@ -c src/bdw.c
bdw-%.o: src/bdw.c benchmarks/%.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include api/bdw-attrs.h -o $@ -c benchmarks/$*.c
bdw-%: bdw-%.o bdw-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) `pkg-config --libs bdw-gc` -o $@ $^

semi-%-gc.o: src/semi.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/semi.c
semi-%.o: src/semi.c benchmarks/%.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include api/semi-attrs.h -o $@ -c benchmarks/$*.c
semi-%: semi-%.o semi-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
whippet-%: whippet-%.o whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

stack-conservative-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
stack-conservative-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
stack-conservative-whippet-%: stack-conservative-whippet-%.o stack-conservative-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

heap-conservative-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
heap-conservative-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
heap-conservative-whippet-%: heap-conservative-whippet-%.o heap-conservative-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

parallel-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
parallel-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
parallel-whippet-%: parallel-whippet-%.o parallel-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

stack-conservative-parallel-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
stack-conservative-parallel-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
stack-conservative-parallel-whippet-%: stack-conservative-parallel-whippet-%.o stack-conservative-parallel-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

heap-conservative-parallel-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
heap-conservative-parallel-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -DGC_FULLY_CONSERVATIVE=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
heap-conservative-parallel-whippet-%: heap-conservative-parallel-whippet-%.o heap-conservative-parallel-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

generational-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
generational-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
generational-whippet-%: generational-whippet-%.o generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

stack-conservative-generational-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
stack-conservative-generational-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
stack-conservative-generational-whippet-%: stack-conservative-generational-whippet-%.o stack-conservative-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

heap-conservative-generational-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
heap-conservative-generational-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
heap-conservative-generational-whippet-%: heap-conservative-generational-whippet-%.o heap-conservative-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

parallel-generational-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
parallel-generational-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
parallel-generational-whippet-%: parallel-generational-whippet-%.o parallel-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

stack-conservative-parallel-generational-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
stack-conservative-parallel-generational-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
stack-conservative-parallel-generational-whippet-%: stack-conservative-parallel-generational-whippet-%.o stack-conservative-parallel-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

heap-conservative-parallel-generational-whippet-%-gc.o: src/whippet.c benchmarks/%-embedder.h
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include benchmarks/$*-embedder.h -o $@ -c src/whippet.c
heap-conservative-parallel-generational-whippet-%.o: src/whippet.c benchmarks/%.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include api/whippet-attrs.h -o $@ -c benchmarks/$*.c
heap-conservative-parallel-generational-whippet-%: heap-conservative-parallel-generational-whippet-%.o heap-conservative-parallel-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

.PRECIOUS: $(ALL_TESTS)

clean:
	rm -f $(ALL_TESTS)
