TESTS=quads mt-gcbench # MT_GCBench MT_GCBench2
COLLECTORS=bdw semi whippet parallel-whippet generational-whippet parallel-generational-whippet

CC=gcc
CFLAGS=-Wall -O2 -g -fno-strict-aliasing -Wno-unused -DNDEBUG
INCLUDES=-I.
LDFLAGS=-lpthread
COMPILE=$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS)

ALL_TESTS=$(foreach COLLECTOR,$(COLLECTORS),$(addprefix $(COLLECTOR)-,$(TESTS)))

all: $(ALL_TESTS)

bdw-%: bdw.h conservative-roots.h %-types.h %.c
	$(COMPILE) `pkg-config --libs --cflags bdw-gc` -DGC_BDW -o $@ $*.c

semi-%: semi.h precise-roots.h large-object-space.h %-types.h heap-objects.h %.c
	$(COMPILE) -DGC_SEMI -o $@ $*.c

whippet-%: whippet.h precise-roots.h large-object-space.h serial-tracer.h assert.h debug.h %-types.h heap-objects.h %.c
	$(COMPILE) -DGC_WHIPPET -o $@ $*.c

parallel-whippet-%: whippet.h precise-roots.h large-object-space.h parallel-tracer.h assert.h debug.h %-types.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL_WHIPPET -o $@ $*.c

generational-whippet-%: whippet.h precise-roots.h large-object-space.h serial-tracer.h assert.h debug.h %-types.h heap-objects.h %.c
	$(COMPILE) -DGC_GENERATIONAL_WHIPPET -o $@ $*.c

parallel-generational-whippet-%: whippet.h precise-roots.h large-object-space.h parallel-tracer.h assert.h debug.h %-types.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL_GENERATIONAL_WHIPPET -o $@ $*.c

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
