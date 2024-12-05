# pfgrep: grep for source physical file members

pfgrep is a fast (with lots on the table for future optimization) way to search
with regular expressions (using PCRE2) in physical file members. It's faster
than QShell grep or using a PASE iconv/Rfile with grep in a shell script.

## Building

Clone the repository somewhere:

```shell
git clone https://github.com/SeidenGroup/pfgrep
cd pfgrep
```

Install necessary dependencies:

```shell
yum install pcre2-devel pkg-config make-gnu gcc
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

## Usage

The first argument is a (PCRE) regular expression and all subsequent arguments
are filenames (IFS style) of physical file members. The full path to members
must be specified; it does not yet recurse into just a file.

The flags that can be passed are:

* `-c`: Counts the matched lines in each file. Implies `-q`.
* `-F`: Don't use a regular expression, match substrings literally.
* `-H`: Always preprends the matched filename, even if only one member was passed.
* `-h`: Never preprends the matched filename, even if only multiple members were passed.
* `-i`: Matches are case insensitive.
* `-L`: Shows filenames that didn't match. Implies `-q`.
* `-L`: Shows filenames that did match. Implies `-q`.
* `-n`: Shows the line number of a match. Note this is *not* the sequence number of a record; this is not yet supported.
* `-q`: Doesn't print matches. The return code of pfgrep is unchanged though, so this is useful for i.e. conditionals in a script.
* `-r`: Recurses into directories, be it IFS directories, libraries, or physical files.
* `-s`: Doesn't print error messages. The return code of pfgrep is unchanged.
* `-t`: Trims whitespace at the end of lines; by default, pfgrep doesn't, so there are spaces at the end to match record padding.
* `-w`: Match only whole words.
* `-v`: Inverts matches; lines that don't match will match and be printed et vice versa.
* `-x`: Match only a whole line.

## Ideas/Plans

* Context lines (requires refactor to keep previous lines)
* Enable PCRE JIT (may require rebuild or vendoring)
* Multithreaded searches using i.e. a queue
* Memoize/cache values (i.e. record length, iconv converters)
* Multiline semantics (requires similar refactor for context)
* Work with sequence numbers and dates for records
