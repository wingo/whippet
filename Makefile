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
INCLUDES=-I.
LDFLAGS=-lpthread -flto
COMPILE=$(CC) $(CFLAGS) $(INCLUDES)
PLATFORM=gnu-linux

ALL_TESTS=$(foreach COLLECTOR,$(COLLECTORS),$(addprefix $(COLLECTOR)-,$(TESTS)))

all: $(ALL_TESTS)

gc-platform.o: gc-platform.h gc-platform-$(PLATFORM).c gc-visibility.h
	$(COMPILE) -o $@ -c gc-platform-$(PLATFORM).c

gc-stack.o: gc-stack.c
	$(COMPILE) -o $@ -c $<

gc-options.o: gc-options.c gc-options.h gc-options-internal.h
	$(COMPILE) -o $@ -c $<

gc-ephemeron-%.o: gc-ephemeron.c gc-ephemeron.h gc-ephemeron-internal.h %-embedder.h
	$(COMPILE) -include $*-embedder.h -o $@ -c $<

bdw-%-gc.o: bdw.c %-embedder.h %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 `pkg-config --cflags bdw-gc` -include $*-embedder.h -o $@ -c bdw.c
bdw-%.o: bdw.c %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include bdw-attrs.h -o $@ -c $*.c
bdw-%: bdw-%.o bdw-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) `pkg-config --libs bdw-gc` -o $@ $^

semi-%-gc.o: semi.c %-embedder.h large-object-space.h assert.h debug.h %.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c semi.c
semi-%.o: semi.c %.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include semi-attrs.h -o $@ -c $*.c
semi-%: semi-%.o semi-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c
whippet-%: whippet-%.o whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

stack-conservative-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
stack-conservative-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c
stack-conservative-whippet-%: stack-conservative-whippet-%.o stack-conservative-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

heap-conservative-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include $*-embedder.h -o $@ -c whippet.c
heap-conservative-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include whippet-attrs.h -o $@ -c $*.c
heap-conservative-whippet-%: heap-conservative-whippet-%.o heap-conservative-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

parallel-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
parallel-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c
parallel-whippet-%: parallel-whippet-%.o parallel-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

stack-conservative-parallel-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
stack-conservative-parallel-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c
stack-conservative-parallel-whippet-%: stack-conservative-parallel-whippet-%.o stack-conservative-parallel-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

heap-conservative-parallel-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include $*-embedder.h -o $@ -c whippet.c
heap-conservative-parallel-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -DGC_FULLY_CONSERVATIVE=1 -include whippet-attrs.h -o $@ -c $*.c
heap-conservative-parallel-whippet-%: heap-conservative-parallel-whippet-%.o heap-conservative-parallel-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c
generational-whippet-%: generational-whippet-%.o generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

stack-conservative-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
stack-conservative-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c
stack-conservative-generational-whippet-%: stack-conservative-generational-whippet-%.o stack-conservative-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

heap-conservative-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include $*-embedder.h -o $@ -c whippet.c
heap-conservative-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include whippet-attrs.h -o $@ -c $*.c
heap-conservative-generational-whippet-%: heap-conservative-generational-whippet-%.o heap-conservative-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

parallel-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
parallel-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c
parallel-generational-whippet-%: parallel-generational-whippet-%.o parallel-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

stack-conservative-parallel-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
stack-conservative-parallel-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c
stack-conservative-parallel-generational-whippet-%: stack-conservative-parallel-generational-whippet-%.o stack-conservative-parallel-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

heap-conservative-parallel-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include $*-embedder.h -o $@ -c whippet.c
heap-conservative-parallel-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include whippet-attrs.h -o $@ -c $*.c
heap-conservative-parallel-generational-whippet-%: heap-conservative-parallel-generational-whippet-%.o heap-conservative-parallel-generational-whippet-%-gc.o gc-stack.o gc-options.o gc-platform.o gc-ephemeron-%.o
	$(CC) $(LDFLAGS) -o $@ $^

.PRECIOUS: $(ALL_TESTS)

clean:
	rm -f $(ALL_TESTS)
