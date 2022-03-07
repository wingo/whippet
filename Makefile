TESTS=GCBench # MT_GCBench MT_GCBench2
COLLECTORS=bdw semi mark-sweep

CC=gcc
CFLAGS=-Wall -O2 -g

ALL_TESTS=$(foreach COLLECTOR,$(COLLECTORS),$(addprefix $(COLLECTOR)-,$(TESTS)))

all: $(ALL_TESTS)

bdw-%: bdw.h conservative-roots.h %.c
	$(CC) $(CFLAGS) -lpthread `pkg-config --libs --cflags bdw-gc` -I. -DGC_BDW -o $@ $*.c

semi-%: semi.h precise-roots.h %.c
	$(CC) $(CFLAGS) -I. -DGC_SEMI -o $@ $*.c

mark-sweep-%: mark-sweep.h precise-roots.h %.c
	$(CC) $(CFLAGS) -I. -DGC_MARK_SWEEP -o $@ $*.c

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
