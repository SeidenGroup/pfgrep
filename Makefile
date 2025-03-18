VERSION := 0.5

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
AR := ar

.PHONY: all clean install dist check

all: pfgrep pfcat pfstat pfzip

libpf.a: common.o conv.o errc.o convpath.o rcdfmt.o mbrinfo.o
	$(AR) -X64 cru $@ $^

pfgrep: pfgrep.o libpf.a
	$(LD) $(ZIP_LDFLAGS) $(PCRE2_LDFLAGS) $(JSONC_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

pfcat: pfcat.o libpf.a
	$(LD) $(ZIP_LDFLAGS) $(PCRE2_LDFLAGS) $(JSONC_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

pfstat: pfstat.o libpf.a
	$(LD) $(ZIP_LDFLAGS) $(PCRE2_LDFLAGS) $(JSONC_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

pfzip: pfzip.o libpf.a
	$(LD) $(ZIP_LDFLAGS) $(PCRE2_LDFLAGS) $(JSONC_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

%.o: %.c
	$(CC) $(PCRE2_CFLAGS) $(JSONC_CFLAGS) $(ZIP_CFLAGS) $(CFLAGS) -DPFGREP_VERSION=\"$(VERSION)\" -c -o $@ $^

clean:
	rm -f *.o *.a pfgrep pfcat pfstat pfcat core

check: pfgrep pfcat pfzip
	TESTLIB=$(TESTLIB) ./test/bats/bin/bats -T test/pfgrep.bats test/pfcat.bats test/pfzip.bats

install: all
	install -D -m 755 pfgrep $(DESTDIR)$(PREFIX)/bin/pfgrep
	install -D -m 755 pfcat $(DESTDIR)$(PREFIX)/bin/pfcat
	install -D -m 755 pfstat $(DESTDIR)$(PREFIX)/bin/pfstat
	install -D -m 755 pfzip $(DESTDIR)$(PREFIX)/bin/pfzip
	install -D -m 644 pfgrep.1 $(DESTDIR)$(PREFIX)/share/man/man1/pfgrep.1
	install -D -m 644 pfcat.1 $(DESTDIR)$(PREFIX)/share/man/man1/pfcat.1
	install -D -m 644 pfstat.1 $(DESTDIR)$(PREFIX)/share/man/man1/pfstat.1
	install -D -m 644 pfzip.1 $(DESTDIR)$(PREFIX)/share/man/man1/pfzip.1

dist:
	# This assumes git
	# XXX: git archive doesn't support submodules, so for now, exclude tests with source tarballs
	git archive --prefix=pfgrep-$(VERSION)/ --format=tar.gz -o pfgrep-$(VERSION).tar.gz HEAD Makefile README.md COPYING *.c *.h *.1
