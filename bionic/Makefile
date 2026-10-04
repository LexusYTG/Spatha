# Spatha — daemon (host bionic)
CC      ?= clang
CFLAGS  := -O2 -Wall -Wextra -pthread -I..
LDFLAGS := -ldl

.PHONY: all clean
all: spathad

spathad: spathad.c ../proto.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f spathad *.o
