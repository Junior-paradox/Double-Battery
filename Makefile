CC      ?= gcc
CFLAGS  ?= -O3 -march=native -flto -std=c11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lm -lpthread -flto
CORE    := src/graph.c src/dispatch.c src/htable.c
COBJ    := $(CORE:.c=.o)

all: bench server

bench: $(COBJ) src/bench.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

server: $(COBJ) src/server.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: bench
	./bench

serve: server
	./server 9090

clean:
	rm -f src/*.o bench server

.PHONY: all run clean
