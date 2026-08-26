# Portable by default. -march=native is OPT-IN, because a binary built with it
# targets the build machine's exact CPU and will SIGILL on an older one or in a
# container on different hardware. Build with `make NATIVE=1` for local
# benchmarking only; never for anything you ship.
CC      ?= cc
NATIVE  ?= 0
# -MMD -MP emits a .d file per object listing the headers it used, so editing a
# header actually triggers a rebuild. Without it `make` silently leaves stale
# objects behind and you test a binary that does not match the source.
CFLAGS  ?= -O3 -flto -std=c11 -Wall -Wextra -Wno-unused-parameter -MMD -MP
ifeq ($(NATIVE),1)
CFLAGS  += -march=native
endif
LDFLAGS ?= -lm -lpthread -flto

CORE    := src/graph.c src/dispatch.c src/htable.c
COBJ    := $(CORE:.c=.o)

all: bench server tests

bench: $(COBJ) src/bench.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

server: $(COBJ) src/server.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests: $(COBJ) src/test.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(CORE:.c=.d) src/bench.d src/server.d src/test.d

# Exits non-zero if any assertion fails. Runs in about a second.
test: tests
	./tests

# Same contract, but over the socket: starts the daemon on a scratch port and
# drives it with bash's own /dev/tcp. No extra dependencies.
test-protocol: server
	./scripts/protocol_test.sh

test-all: test test-protocol

run: bench
	./bench

quick: bench
	./bench --quick

serve: server
	./server 9090

# Refresh the compiled-in real-city hospital rosters from the published open
# dataset. Deliberately NOT part of `all`: it reaches the network, and
# src/city_data.h is committed precisely so that a build never has to.
# Regenerate on purpose, then read the diff.
city-data:
	python3 scripts/gen_city_data.py --out src/city_data.h

clean:
	rm -f src/*.o src/*.d bench server tests

.PHONY: all test test-protocol test-all run quick serve city-data clean
