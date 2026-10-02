# Host (Termux):      make host        (CC=clang; requiere vulkan-headers)
# Container (Ubuntu): make icd && make install-icd
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

host: spathad spatha-test
spathad: spathad.c proto.c proto.h wire.h
	$(CC) $(CFLAGS) -o $@ spathad.c proto.c -ldl
spatha-test: libspatha.c proto.c proto.h
	$(CC) $(CFLAGS) -o $@ libspatha.c proto.c

icd: libspatha-icd.so
libspatha-icd.so: libspatha-icd.c proto.c proto.h wire.h
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=hidden -pthread -o $@ libspatha-icd.c proto.c -ldl

LIBDIR ?= /usr/lib/aarch64-linux-gnu
install-icd: libspatha-icd.so
	install -m755 libspatha-icd.so $(LIBDIR)/
	install -Dm644 spatha_icd.json /usr/share/vulkan/icd.d/spatha_icd.json

clean:
	rm -f spathad spatha-test libspatha-icd.so
.PHONY: host icd install-icd clean
