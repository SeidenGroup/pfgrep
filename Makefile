VERSION := 0.5alpha1

# DESTDIR can be set as well
PREFIX := /QOpenSys/pkgs

TESTLIB := pfgreptest

# Can be overriden for your own PCRE2 or json-c build
PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8)
PCRE2_LDFLAGS := $(shell pkg-config --libs libpcre2-8)
JSONC_CFLAGS := $(shell pkg-config --cflags json-c)
JSONC_LDFLAGS := $(shell pkg-config --libs json-c)
ZIP_CFLAGS := $(shell pkg-config --cflags libzip)
ZIP_LDFLAGS := $(shell pkg-config --libs libzip)

# Build with warnings as errors and symbols for developers,
# build with optimizations for release builds.
ifdef DEBUG
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -Wno-error=unused-function -g -Og -DDEBUG
LDFLAGS := -g -O0
else
CFLAGS := -std=gnu11 -Wall -Wextra -O2
LDFLAGS := -O2
endif

# Use gcc 10 from Yum if available, otherwise try regular gcc on PATH
CC := $(shell if type gcc-10 > /dev/null 2> /dev/null; then echo gcc-10; else echo gcc; fi)
LD := $(CC)

.PHONY: all clean install dist check

all: pfgrep pfcat pfzip

pfgrep: pfgrep.o common.o errc.o ebcdic.o convpath.o rcdfmt.o
	$(LD) $(ZIP_LDFLAGS) $(PCRE2_LDFLAGS) $(JSONC_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

pfcat: pfcat.o common.o errc.o ebcdic.o convpath.o rcdfmt.o
	$(LD) $(ZIP_LDFLAGS) $(PCRE2_LDFLAGS) $(JSONC_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

pfzip: pfzip.o common.o errc.o ebcdic.o convpath.o rcdfmt.o mbrinfo.o
	$(LD) $(ZIP_LDFLAGS) $(PCRE2_LDFLAGS) $(JSONC_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

%.o: %.c
	$(CC) $(PCRE2_CFLAGS) $(JSONC_CFLAGS) $(ZIP_CFLAGS) $(CFLAGS) -DPFGREP_VERSION=\"$(VERSION)\" -c -o $@ $^

clean:
	rm -f *.o pfgrep pfcat core

check: pfgrep pfcat pfzip
	TESTLIB=$(TESTLIB) ./test/bats/bin/bats -T test/pfgrep.bats test/pfcat.bats test/pfzip.bats

install: pfgrep
	install -D -m 755 pfgrep $(DESTDIR)$(PREFIX)/bin/pfgrep
	install -D -m 755 pfcat $(DESTDIR)$(PREFIX)/bin/pfcat
	install -D -m 755 pfzip $(DESTDIR)$(PREFIX)/bin/pfzip

dist:
	# This assumes git
	# XXX: git archive doesn't support submodules, so for now, exclude tests with source tarballs
	git archive --prefix=pfgrep-$(VERSION)/ --format=tar.gz -o pfgrep-$(VERSION).tar.gz HEAD Makefile README.md COPYING *.c *.h
