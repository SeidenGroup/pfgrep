VERSION := 0.3.2

# DESTDIR can be set as well
PREFIX := /QOpenSys/pkgs

# Can be overriden for your own PCRE2 or json-c build
PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8)
PCRE2_LDFLAGS := $(shell pkg-config --libs libpcre2-8)
JSONC_CFLAGS := $(shell pkg-config --cflags json-c)
JSONC_LDFLAGS := $(shell pkg-config --libs json-c)

# Build with warnings as errors and symbols for developers,
# build with optimizations for release builds.
ifdef DEBUG
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -g -Og -DDEBUG
LDFLAGS := -g -O0
else
CFLAGS := -std=gnu11 -Wall -Wextra -O2
LDFLAGS := -O2
endif

# Use gcc 10 from Yum if available, otherwise try regular gcc on PATH
CC := $(shell if type gcc-10 > /dev/null 2> /dev/null; then echo gcc-10; else echo gcc; fi)
LD := $(CC)

.PHONY: clean install dist

pfgrep: main.o errc.o ebcdic.o convpath.o rcdfmt.o
	$(LD) $(PCRE2_LDFLAGS) $(JSONC_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

%.o: %.c
	$(CC) $(PCRE2_CFLAGS) $(JSONC_CFLAGS) $(CFLAGS) -DPFGREP_VERSION=\"$(VERSION)\" -c -o $@ $^

clean:
	rm -f *.o pfgrep core

install: pfgrep
	install -D -m 755 pfgrep $(DESTDIR)$(PREFIX)/bin/pfgrep

dist:
	# This assumes git
	git archive --prefix=pfgrep-$(VERSION)/ --format=tar.gz -o pfgrep-$(VERSION).tar.gz HEAD Makefile README.md COPYING *.c *.h
