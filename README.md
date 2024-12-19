# pfgrep: grep for source physical file members

pfgrep is a fast (with lots on the table for future optimization) way to search
with regular expressions (using PCRE2) in physical file members. It's faster
than QShell grep or using a PASE iconv/Rfile with grep in a shell script.

## Installation

We provide binary builds - look in the releases for RPM packages. Packages are
also provided in the Seiden Group repository for our customers.

To install the RPM, copy the RPM somewhere to the IFS and run:

```shell
yum install /path/to/pfgrep.rpm
```

You may need to use `update` instead of `install` for future updates.

If you have our repository, run:

```shell
yum install pfgrep
```

If you don't want to install the RPM or want to work on pfgrep, follow the
instructions below to build from source.

## Building from source

Clone the repository somewhere:

```shell
git clone https://github.com/SeidenGroup/pfgrep
cd pfgrep
```

Install necessary dependencies:

```shell
yum install json-c-devel pcre2-devel pkg-config make-gnu gcc
# Needed on IBM i 7.4 or newer, older GCC can't handle newer versions' headers
yum install gcc-10
```

Build:

```shell
# For average users, builds with optimizations
make
# For developers, builds w/o optimizations and includes debug symbols
make DEBUG=1
```

You'll have a `pfgrep` binary. You can run it directly, or install the binary:

```shell
make install
```

## Examples

Search for the string "CRASH" in case insensitive mode (including i.e. "crash")
in a member:

```shell
pfgrep -i crash /QSYS.LIB/CALVIN.LIB/QCLSRC.FILE/LOOP.MBR
```

Search for VxRx references in the ILE C header files:

```shell
pfgrep '[Vv][0-9][Rr][0-9]' /QIBM/include/*.h
```

Search for the word "function" recursively in several libraries and files:

```shell
pfgrep -w -r 'function' /QSYS.LIB/PROD.LIB /QSYS.LIB/DEV.LIB /QSYS.LIB/ALICE.LIB/ROBERT.FILE
```

Search recursively with a regular expression:

```shell
pfgrep -r 'pfgrep -r '^#(define|pragma).*Qp0l.*Attr' /QSYS.LIB/QSYSINC.LIB/H.FILE
```

Note that expansions with globs are performed by the shell, and not pfgrep.

## Usage

The first argument is a (PCRE) regular expression and all subsequent arguments
are filenames (IFS style) of physical file members, symbolic links to members,
or if recursion is enabled, directories/files/members that contain these.

If the `-e` or `-f` flags are given, then the first argument for pattern is
omitted, as the patterns are passed via these flags. These flags can be used
together and multiple times.

For information on PCRE regex syntax, consult [PCRE docs][pcre2syntax].

The flags that can be passed are:

* `-c`: Counts the matched lines in each file. Implies `-q`.
* `-e`: Uses a pattern to match.
* `-F`: Don't use a regular expression, match substrings literally.
* `-f`: Reads patterns from a stream file, each on their own line. Use `-` for standard input.
* `-H`: Always preprends the matched filename, even if only one member was passed.
* `-h`: Never preprends the matched filename, even if only multiple members were passed.
* `-i`: Matches are case insensitive.
* `-L`: Shows filenames that didn't match. Implies `-q`.
* `-l`: Shows filenames that did match. Implies `-q`.
* `-n`: Shows the line number of a match. Note this is *not* the sequence number of a record; this is not yet supported.
* `-p`: Searches non-source physical files. Note that non-source PFs are [subject to limitations][qsyslib-limits] (pfgrep reads PFs in binary mode).
* `-q`: Doesn't print matches. The return code of pfgrep is unchanged though, so this is useful for i.e. conditionals in a script.
* `-r`: Recurses into directories, be it IFS directories, libraries, or physical files.
* `-s`: Doesn't print error messages. The return code of pfgrep is unchanged.
* `-t`: Trims whitespace at the end of lines; by default, pfgrep doesn't, so there are spaces at the end to match record padding.
* `-w`: Match only whole words.
* `-V`: Prints the version of pfgrep and the libraries it uses, as well as copyright information.
* `-v`: Inverts matches; lines that don't match will match and be printed et vice versa.
* `-x`: Match only a whole line.

[pcre2syntax]: https://www.pcre.org/current/doc/html/pcre2syntax.html
[qsyslib-limits]: https://www.ibm.com/docs/en/i/7.5?topic=qsyslib-file-handling-restrictions-in-file-system
