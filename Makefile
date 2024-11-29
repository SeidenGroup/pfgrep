CFLAGS := $(shell pkg-config --cflags libpcre2-8) -std=gnu11 -Wall -Wextra -g -O0
LDFLAGS := $(shell pkg-config --libs libpcre2-8) -g -O0
CC := gcc
LD := gcc

.PHONY: clean

pfgrep: main.o errc.o ebcdic.o convpath.o rcdfmt.o
	$(LD) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

clean:
	rm -f *.o pfgrep core
