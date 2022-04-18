TESTS=quads mt-gcbench # MT_GCBench MT_GCBench2
COLLECTORS=bdw semi mark-sweep parallel-mark-sweep

CC=gcc
CFLAGS=-Wall -O2 -g -fno-strict-aliasing -Wno-unused -DNDEBUG
INCLUDES=-I.
LDFLAGS=-lpthread
COMPILE=$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS)

ALL_TESTS=$(foreach COLLECTOR,$(COLLECTORS),$(addprefix $(COLLECTOR)-,$(TESTS)))

all: $(ALL_TESTS)

bdw-%: bdw.h conservative-roots.h %-types.h %.c
	$(COMPILE) `pkg-config --libs --cflags bdw-gc` -DGC_BDW -o $@ $*.c

semi-%: semi.h precise-roots.h %-types.h heap-objects.h %.c
	$(COMPILE) -DGC_SEMI -o $@ $*.c

mark-sweep-%: mark-sweep.h precise-roots.h serial-tracer.h assert.h debug.h %-types.h heap-objects.h %.c
	$(COMPILE) -DGC_MARK_SWEEP -o $@ $*.c

parallel-mark-sweep-%: mark-sweep.h precise-roots.h parallel-tracer.h assert.h debug.h %-types.h heap-objects.h %.c
	$(COMPILE) -DGC_PARALLEL_MARK_SWEEP -o $@ $*.c

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
