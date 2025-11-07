VERSION := 0.5.1

# DESTDIR can be set as well
PREFIX := /QOpenSys/pkgs

TESTLIB := pfgreptest

# Can be overriden for your own PCRE2 or json-c build
PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8)
PCRE2_LDFLAGS := $(shell pkg-config --libs libpcre2-8)
ZIP_CFLAGS := $(shell pkg-config --cflags libzip)
ZIP_LDFLAGS := $(shell pkg-config --libs libzip)
PASECPP_CFLAGS := -Iinclude/pase-cpp

DEPS_CFLAGS := $(PCRE2_CFLAGS) $(ZIP_CFLAGS) $(PASECPP_CFLAGS)
DEPS_LDFLAGS := $(PCRE2_LDFLAGS) $(ZIP_LDFLAGS)

# Build with warnings as errors and symbols for developers,
# build with optimizations for release builds.
ifdef DEBUG
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -Wno-error=unused-function -g -Og -DDEBUG
CXXFLAGS := -std=c++14 -Wall -Wextra -Werror -Wno-error=unused-function -g -Og -DDEBUG
LDFLAGS := -g -O0
else
CFLAGS := -std=gnu11 -Wall -Wextra -O2
CXXFLAGS := -std=c++14 -Wall -Wextra -O2
LDFLAGS := -O2
endif

# Use gcc 10 from Yum if available, otherwise try regular gcc on PATH
CC := $(shell if type gcc-10 > /dev/null 2> /dev/null; then echo gcc-10; else echo gcc; fi)
CXX := $(shell if type g++-10 > /dev/null 2> /dev/null; then echo g++-10; else echo g++; fi)
LD := $(CXX)
AR := ar

.PHONY: all clean install dist check

all: pfgrep pfcat pfstat pfzip

libpf.a: common.o conv.o errc.o convpath.o rcdfmt.o mbrinfo.o
	$(AR) -X64 cru $@ $^

pfgrep: pfgrep.o libpf.a
	$(LD) $(DEPS_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

pfcat: pfcat.o libpf.a
	$(LD) $(DEPS_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

pfstat: pfstat.o libpf.a
	$(LD) $(DEPS_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

pfzip: pfzip.o libpf.a
	$(LD) $(DEPS_LDFLAGS) $(LDFLAGS) -o $@ $^ /QOpenSys/usr/lib/libiconv.a

%.o: %.c
	$(CC) $(DEPS_CFLAGS) $(CFLAGS) -DPFGREP_VERSION=\"$(VERSION)\" -c -o $@ $^

%.o: %.cxx
	$(CXX) $(DEPS_CFLAGS) $(CXXFLAGS) -DPFGREP_VERSION=\"$(VERSION)\" -c -o $@ $^

clean:
	rm -f *.o *.a pfgrep pfcat pfstat pfcat core *.tar *.tar.gz

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

# This assumes git; take the root and then for each submodule staple it to the root's submodule
# approach from https://gist.github.com/arteymix/03702e3eb05c2c161a86b49d4626d21f
dist:
	rm -f pfgrep-$(VERSION).tar.gz
	git archive --prefix=pfgrep-$(VERSION)/ --format=tar -o pfgrep-$(VERSION).tar HEAD Makefile README.md COPYING *.c *.cxx *.h *.hxx *.1
	git submodule foreach --recursive "git archive --prefix=pfgrep-$(VERSION)/"'$$path'"/ --output="'$$sha1'".tar HEAD && tar --concatenate --file=$(shell pwd)/pfgrep-$(VERSION).tar "'$$sha1'".tar && rm "'$$sha1'".tar"
	gzip pfgrep-$(VERSION).tar
