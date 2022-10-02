TESTS=quads mt-gcbench # MT_GCBench MT_GCBench2
COLLECTORS=bdw semi whippet conservative-whippet parallel-whippet conservative-parallel-whippet generational-whippet conservative-generational-whippet parallel-generational-whippet conservative-parallel-generational-whippet

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

bdw-%-gc.o: semi.c %-embedder.h %.c
	$(COMPILE) `pkg-config --cflags bdw-gc` -include $*-embedder.h -o $@ -c bdw.c
bdw-%.o: semi.c %.c
	$(COMPILE) -include bdw-attrs.h -o $@ -c $*.c
bdw-%: bdw-%.o bdw-%-gc.o gc-stack.o gc-platform.o
	$(CC) $(LDFLAGS) `pkg-config --libs bdw-gc` -o $@ $^

semi-%-gc.o: semi.c %-embedder.h large-object-space.h assert.h debug.h %.c
	$(COMPILE) -DGC_PRECISE=1 -include $*-embedder.h -o $@ -c semi.c
semi-%.o: semi.c %.c
	$(COMPILE) -DGC_PRECISE=1 -include semi-attrs.h -o $@ -c $*.c

whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PRECISE=1 -include $*-embedder.h -o $@ -c whippet.c
whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PRECISE=1 -include whippet-attrs.h -o $@ -c $*.c

conservative-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PRECISE=0 -include $*-embedder.h -o $@ -c whippet.c
conservative-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PRECISE=0 -include whippet-attrs.h -o $@ -c $*.c

parallel-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE=1 -include $*-embedder.h -o $@ -c whippet.c
parallel-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE=1 -include whippet-attrs.h -o $@ -c $*.c

conservative-parallel-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE=0 -include $*-embedder.h -o $@ -c whippet.c
conservative-parallel-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_PRECISE=0 -include whippet-attrs.h -o $@ -c $*.c

generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE=1 -include $*-embedder.h -o $@ -c whippet.c
generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE=1 -include whippet-attrs.h -o $@ -c $*.c

conservative-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h serial-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE=0 -include $*-embedder.h -o $@ -c whippet.c
conservative-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_GENERATIONAL=1 -DGC_PRECISE=0 -include whippet-attrs.h -o $@ -c $*.c

parallel-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE=1 -include $*-embedder.h -o $@ -c whippet.c
parallel-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE=1 -include whippet-attrs.h -o $@ -c $*.c

conservative-parallel-generational-whippet-%-gc.o: whippet.c %-embedder.h large-object-space.h parallel-tracer.h assert.h debug.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE=0 -include $*-embedder.h -o $@ -c whippet.c
conservative-parallel-generational-whippet-%.o: whippet.c %.c
	$(COMPILE) -DGC_PARALLEL=1 -DGC_GENERATIONAL=1 -DGC_PRECISE=0 -include whippet-attrs.h -o $@ -c $*.c

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
