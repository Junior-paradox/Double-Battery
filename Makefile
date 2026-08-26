CC      ?= gcc
CFLAGS  ?= -O3 -march=native -flto -std=c11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lm -lpthread -flto
SRC     := src/graph.c src/dispatch.c src/hindex.c src/bench.c
OBJ     := $(SRC:.c=.o)

all: bench

bench: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: bench
	./bench

clean:
	rm -f $(OBJ) bench

.PHONY: all run clean
