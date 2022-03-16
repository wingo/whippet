TESTS=gcbench # MT_GCBench MT_GCBench2
COLLECTORS=bdw semi mark-sweep parallel-mark-sweep

CC=gcc
CFLAGS=-Wall -O2 -g -fno-strict-aliasing

ALL_TESTS=$(foreach COLLECTOR,$(COLLECTORS),$(addprefix $(COLLECTOR)-,$(TESTS)))

all: $(ALL_TESTS)

bdw-%: bdw.h conservative-roots.h %-types.h %.c
	$(CC) $(CFLAGS) -DNDEBUG -lpthread `pkg-config --libs --cflags bdw-gc` -I. -DGC_BDW -o $@ $*.c

semi-%: semi.h precise-roots.h %-types.h heap-objects.h %.c
	$(CC) $(CFLAGS) -I. -DNDEBUG -DGC_SEMI -o $@ $*.c

mark-sweep-%: mark-sweep.h precise-roots.h serial-marker.h assert.h debug.h %-types.h heap-objects.h %.c
	$(CC) $(CFLAGS) -I. -Wno-unused -DNDEBUG -DGC_MARK_SWEEP -o $@ $*.c

parallel-mark-sweep-%: mark-sweep.h precise-roots.h parallel-marker.h assert.h debug.h %-types.h heap-objects.h %.c
	$(CC) $(CFLAGS) -I. -Wno-unused -DNDEBUG -DGC_PARALLEL_MARK_SWEEP -lpthread -o $@ $*.c

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
