# Spatha — ICD stub (container glibc)
CC      ?= gcc
CFLAGS  := -O2 -Wall -fPIC -shared -pthread -I..
LDFLAGS := -ldl

.PHONY: all tests clean
all: libspatha-icd.so

libspatha-icd.so: libspatha-icd.c ../proto.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests: libspatha libspatha-icd.so
libspatha: libspatha.c ../proto.c
	$(CC) -O2 -Wall -o $@ $^ -I..

clean:
	rm -f libspatha-icd.so libspatha *.o
