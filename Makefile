TESTS=quads mt-gcbench # MT_GCBench MT_GCBench2
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

CC=gcc
CFLAGS=-Wall -O2 -g -flto -fno-strict-aliasing -fvisibility=hidden -Wno-unused -DNDEBUG
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

bdw-%-gc.o: bdw.c %-embedder.h %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 `pkg-config --cflags bdw-gc` -include $*-embedder.h -o $@ -c bdw.c
bdw-%.o: bdw.c %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include bdw-attrs.h -o $@ -c $*.c
bdw-%: bdw-%.o bdw-%-gc.o gc-stack.o gc-platform.o
	$(CC) $(LDFLAGS) `pkg-config --libs bdw-gc` -o $@ $^

semi-%-gc.o: semi.c %-embedder.h large-object-space.h assert.h debug.h %.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c semi.c
semi-%.o: semi.c %.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include semi-attrs.h -o $@ -c $*.c

whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PRECISE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c

stack-conservative-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
stack-conservative-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c

heap-conservative-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include $*-embedder.h -o $@ -c whippet.c
heap-conservative-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include whippet-attrs.h -o $@ -c $*.c

parallel-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
parallel-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c

stack-conservative-parallel-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
stack-conservative-parallel-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c

heap-conservative-parallel-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include $*-embedder.h -o $@ -c whippet.c
heap-conservative-parallel-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -DGC_FULLY_CONSERVATIVE=1 -include whippet-attrs.h -o $@ -c $*.c

generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c

stack-conservative-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
stack-conservative-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c

heap-conservative-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include $*-embedder.h -o $@ -c whippet.c
heap-conservative-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include whippet-attrs.h -o $@ -c $*.c

parallel-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
parallel-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c

stack-conservative-parallel-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include $*-embedder.h -o $@ -c whippet.c
stack-conservative-parallel-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -include whippet-attrs.h -o $@ -c $*.c

heap-conservative-parallel-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include $*-embedder.h -o $@ -c whippet.c
heap-conservative-parallel-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1 -include whippet-attrs.h -o $@ -c $*.c

%: %.o %-gc.o gc-platform.o gc-stack.o
	$(CC) $(LDFLAGS) $($*_LDFLAGS) -o $@ $^

check: $(addprefix test-$(TARGET),$(TARGETS))

test-%: $(ALL_TESTS)
	@echo "Running unit tests..."
	@set -e; for test in $?; do \
	  echo "Testing: $$test"; \
	  ./$$test; \
	done
	@echo "Success."

.PHONY: check

.PRECIOUS: $(ALL_TESTS)

clean:
	rm -f $(ALL_TESTS)
