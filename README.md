# pfgrep: grep for source physical file members (and friends)

**pfgrep** is a fast (with lots possible for future optimization) way to search
with regular expressions (using PCRE2) in physical file members. It's faster
than QShell grep or using a PASE iconv/Rfile with grep in a shell script.

pfgrep also includes other utilities useful for bridging the gap between
physical files/EBCDIC streamfiles and the rest of the world. These include:

* **pfzip**: Put PFs/streamfiles into an archive as normal UTF-8/ASCII text
  files in a Zip file, complete with member descriptions as comments. Useful
  combined with pfgrep to take out a bunch of relevant files for analysis.

And some small utilities, mostly useful as examples or for diagnosing issues
with other tools:

* **pfcat**: Read several PFs/streamfiles at once. Not as featureful as Rfile.
* **pfstat**: Print PF member information in tab-separated value (TSV) form.

## Installation

We provide binary builds for stable releases - look in the [releases][releases]
for `.rpm` packages.

Packages are also provided in the Seiden Group repository for our customers.

To install the RPM, copy the RPM somewhere to the IFS and run:

```shell
yum install /path/to/pfgrep.rpm
```

You may need to use `update` instead of `install` for future updates.

If you have our repository, run:

```shell
yum install pfgrep
```

If you don't want to install the RPM, want to try features in development, or
want to work on pfgrep, follow the instructions below to build from source.

## Building from source

Clone the repository somewhere:

```shell
git clone https://github.com/SeidenGroup/pfgrep
cd pfgrep
```

Install necessary dependencies:

```shell
yum install json-c-devel pcre2-devel libzip-devel pkg-config make-gnu gcc gcc-cplusplus
# Needed on IBM i 7.4 or newer, older GCC can't handle newer versions' headers
yum install gcc-10 gcc-cplusplus-10
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

### pfgrep

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
pfgrep -r '^#(define|pragma).*Qp0l.*Attr' /QSYS.LIB/QSYSINC.LIB/H.FILE
```

Note that expansions with globs are performed by the shell, and not pfgrep.

### pfzip

Put the library QSYSINC into a zip file called includes.zip:

```shell
pfzip -r includes.zip /QSYS.LIB/QSYSINC.LIB
```

(The `-r` flag like for pfgrep will make it recurse into directories.)

The member descriptions, record length, original CCSID before translation, and
the source type applied to the file extension (if you don't want the new file
extension, pass `-E`) will be in the archive. You can view it in an archiver
utility like so:

```
Archive:  includes.zip
  Length      Date    Time    Name
---------  ---------- -----   ----
     9089  2013-10-06 12:59   QSYS.LIB/QSYSINC.LIB/ARPA.FILE/INET.C
(original PF record length 80 CCSID 37)
    38324  2013-10-06 13:00   QSYS.LIB/QSYSINC.LIB/ARPA.FILE/NAMESER.C
(original PF record length 80 CCSID 37)
     6790  2013-10-06 12:59   QSYS.LIB/QSYSINC.LIB/ARPA.FILE/REXEC.C
REMOTE EXECUTION APIS                              (original PF record length 80 CCSID 37)
[...]
```

You can also compose pfzip with pfgrep together. For example, you can put all
files that match a regular expression like so:

```shell
pfzip client.zip $(pfgrep -l "(sales|exports) /QSYS.LIB/PRODLIB.LIB/Q*SRC.LIB")
```

To break this down, the `$()` takes the output of one command and turns it into
command line arguments for another command. The `-l` flag to pfgrep will make it
only print the files that match instead of the matching text in the files.

### pfcat

Print multiple files:

```shell
pfcat /QSYS.LIB/QSYSINC.LIB/H.FILE/*.MBR
```

Print everything recursively:

```shell
pfcat -r /QSYS.LIB/QSYSINC.LIB/H.FILE
```

### pfstat

Print multiple files:

```shell
pfstat /QSYS.LIB/QSYSINC.LIB/H.FILE/*.MBR
```

Print everything recursively:

```shell
pfstat -r /QSYS.LIB/QSYSINC.LIB/H.FILE
```

## Usage

### pfgrep

The first argument is a (PCRE) regular expression and all subsequent arguments
are filenames (IFS style) of physical file members, symbolic links to members,
or if recursion is enabled, directories/files/members that contain these.

If the `-e` or `-f` flags are given, then the first argument for pattern is
omitted, as the patterns are passed via these flags. These flags can be used
together and multiple times.

For information on PCRE regex syntax, consult [PCRE docs][pcre2syntax].

The flags that can be passed are:

* `-A`: Prints the number of lines specified until the end of file or another match.
* `-c`: Counts the matched lines in each file. Implies `-q`.
* `-e`: Uses a pattern to match.
* `-F`: Don't use a regular expression, match substrings literally.
* `-f`: Reads patterns from a stream file, each on their own line. Use `-` for standard input.
* `-H`: Always preprends the matched filename, even if only one member was passed.
* `-h`: Never preprends the matched filename, even if only multiple members were passed.
* `-i`: Matches are case insensitive.
* `-L`: Shows filenames that didn't match. Implies `-q`.
* `-l`: Shows filenames that did match. Implies `-q`.
* `-m`: Match only the number of lines specified.
* `-n`: Shows the line number of a match. Note this is *not* the sequence number of a record; this is not yet supported.
* `-p`: Searches non-source physical files. Note that non-source PFs are [subject to limitations][qsyslib-limits] (pfgrep reads PFs in binary mode).
* `-q`: Doesn't print matches. The return code of pfgrep is unchanged though, so this is useful for i.e. conditionals in a script.
* `-r`: Recurses into directories, be it IFS directories, libraries, or physical files.
* `-s`: Doesn't print error messages. The return code of pfgrep is unchanged.
* `-t`: Don't trim whitespace at the end of lines; by default, pfgrep does. This preserves the padding to match record length. (Older pfgrep inverted the definition of this flag.)
* `-w`: Match only whole words.
* `-V`: Prints the version of pfgrep and the libraries it uses, as well as copyright information.
* `-v`: Inverts matches; lines that don't match will match and be printed et vice versa.
* `-x`: Match only a whole line.

### pfzip

pfzip takes the Zip file to archive into as the first argument and things to put
into said archive as the arguments after. The resulting Zip file can be extracted
on non-IBM i systems.

The flags that can be passed are:

* `-E`: Don't do path translation. Currently, this includes replacing the `.MBR`
extension of PF members with their source type (i.e. `.RPGLE`)
* `-p`: Searches non-source physical files. Note that non-source PFs are [subject to limitations][qsyslib-limits] (pfgrep reads PFs in binary mode).
* `-r`: Recurses into directories, be it IFS directories, libraries, or physical files.
* `-s`: Doesn't print error messages. The return code of pfzip is unchanged.
* `-t`: Don't trim whitespace at the end of lines; by default, pfgrep does. This preserves the padding to match record length. (Older pfgrep inverted the definition of this flag.)
* `-W`: Overwrite the contents of the Zip file. By default, it is appended to.
* `-V`: Prints the version of pfgrep and the libraries it uses, as well as copyright information.

### pfcat

pfcat takes the files to read and concentate as its arguments.

The flags that can be passed are:

* `-p`: Searches non-source physical files. Note that non-source PFs are [subject to limitations][qsyslib-limits] (pfgrep reads PFs in binary mode).
* `-r`: Recurses into directories, be it IFS directories, libraries, or physical files.
* `-t`: Don't trim whitespace at the end of lines; by default, pfgrep does. This preserves the padding to match record length. (Older pfgrep inverted the definition of this flag.)
* `-V`: Prints the version of pfgrep and the libraries it uses, as well as copyright information.

### pfstat

pfstat takes the files to read and print information for as its arguments.

The flags that can be passed are:

* `-p`: Searches non-source physical files. Note that non-source PFs are [subject to limitations][qsyslib-limits] (pfgrep reads PFs in binary mode).
* `-r`: Recurses into directories, be it IFS directories, libraries, or physical files.
* `-V`: Prints the version of pfgrep and the libraries it uses, as well as copyright information.

[pcre2syntax]: https://www.pcre.org/current/doc/html/pcre2syntax.html
[qsyslib-limits]: https://www.ibm.com/docs/en/i/7.5?topic=qsyslib-file-handling-restrictions-in-file-system
[releases]: https://github.com/SeidenGroup/pfgrep/releases
