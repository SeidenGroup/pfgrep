# DESTDIR can be set as well
PREFIX := /QOpenSys/pkgs

# Can be overriden for your own PCRE2 build
PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8)
PCRE2_LDFLAGS := $(shell pkg-config --libs libpcre2-8)

# Build with warnings as errors and symbols for developers,
# build with optimizations for release builds.
ifdef DEBUG
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -g -Og
LDFLAGS := -g -O0
else
CFLAGS := -std=gnu11 -Wall -Wextra -O2
LDFLAGS := -O2
endif

VERSION := 0.1

# Use gcc 10 from Yum if available, otherwise try regular gcc on PATH
CC := $(shell if type gcc-10 > /dev/null 2> /dev/null; then echo gcc-10; else echo gcc; fi)
LD := $(CC)

.PHONY: clean install dist

pfgrep: main.o errc.o ebcdic.o convpath.o rcdfmt.o reallocarray.o
	$(LD) $(PCRE2_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

%.o: %.c
	$(CC) $(PCRE2_CFLAGS) $(CFLAGS) -c -o $@ $^

clean:
	rm -f *.o pfgrep core

install: pfgrep
	install -D -m 755 pfgrep $(DESTDIR)$(PREFIX)/bin/pfgrep

dist:
	# This assumes git
	git archive --prefix=pfgrep-$(VERSION)/ --format=tar.gz -o pfgrep-$(VERSION).tar.gz HEAD Makefile README.md COPYING *.c *.h
