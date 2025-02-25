setup() {
	load 'test_helper/bats-support/load'
	load 'test_helper/bats-assert/load'

	# get the containing directory of this file
	# use $BATS_TEST_FILENAME instead of ${BASH_SOURCE[0]} or $0,
	# as those will point to the bats executable's location or the preprocessed file respectively
	DIR="$( cd "$( dirname "$BATS_TEST_FILENAME" )" >/dev/null 2>&1 && pwd )"
	PATH="$DIR/../:$PATH"
}

setup_file() {
	# Install test fixtures
	system crtlib "$TESTLIB"
	system crtsrcpf "$TESTLIB/qtxtsrc" "CCSID(37) RCDLEN(92)"
	system addpfm "$TESTLIB/qtxtsrc" abc "TEXT('Sample text file') SRCTYPE(RPGLE)"
	Rfile -w "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" <<EOF
ABC
AB

A
AB
ABC
DEF
FOO BAR
FOOBAR
FOOBAR FOO
EOF
	# XXX: It seems we can't set the modification date on a member.
	# Instead, get the current modification date. (GNU date -r ext)
	MEMBER_MOD_DATE=$(date -r "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" "+%Y-%m-%d %H:%M")
	export MEMBER_MOD_DATE

	TESTSTMF_A=$(mktemp /tmp/pfzip_test.XXXXXXX)
	export TESTSTMF_A
	Rfile -r "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" > "$TESTSTMF_A"
	setccsid 1208 "$TESTSTMF_A"
	touch -d "2025-01-01 12:34:56" "$TESTSTMF_A"

	TESTSTMF_E=$(mktemp /tmp/pfzip_test.XXXXXXX)
	export TESTSTMF_E
	Rfile -r "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" \
		| /usr/bin/iconv -f UTF-8 -t IBM-037 > "$TESTSTMF_E"
	setccsid 37 "$TESTSTMF_E"
	touch -d "2025-01-01 12:34:56" "$TESTSTMF_E"

	# the archive will have leading slash stripped
	TESTSTMF_E_NLS=${TESTSTMF_E#/}
	export TESTSTMF_E_NLS
	TESTSTMF_A_NLS=${TESTSTMF_A#/}
	export TESTSTMF_A_NLS

	TESTZIP=$(mktemp /tmp/pfzip_test.XXXXXXX).zip
	export TESTZIP
}

@test "creating archive" {
	pfzip "$TESTZIP" "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" "$TESTSTMF_E" "$TESTSTMF_A"
	run unzip -l "$TESTZIP"

	assert_output - <<EOF
Archive:  $TESTZIP
  Length      Date    Time    Name
---------  ---------- -----   ----
       47  $MEMBER_MOD_DATE   QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.RPGLE
Sample text file                                   (original PF record length 80 CCSID 37)
       47  2025-01-01 12:34   $TESTSTMF_E_NLS
(original streamfile CCSID 37)
       47  2025-01-01 12:34   $TESTSTMF_A_NLS
(original streamfile CCSID 1208)
---------                     -------
      141                     3 files

EOF
}

@test "overwrite archive" {
	pfzip -W "$TESTZIP" "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	run unzip -l "$TESTZIP"

	assert_output - <<EOF
Archive:  $TESTZIP
  Length      Date    Time    Name
---------  ---------- -----   ----
       47  $MEMBER_MOD_DATE   QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.RPGLE
Sample text file                                   (original PF record length 80 CCSID 37)
---------                     -------
       47                     1 file

EOF
}

@test "disabling path translation" {
	pfzip -WE "$TESTZIP" "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" "$TESTSTMF_E" "$TESTSTMF_A"
	run unzip -l "$TESTZIP"

	assert_output - <<EOF
Archive:  $TESTZIP
  Length      Date    Time    Name
---------  ---------- -----   ----
       47  $MEMBER_MOD_DATE   QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR
Sample text file                                   (original PF record length 80 CCSID 37)
       47  2025-01-01 12:34   $TESTSTMF_E_NLS
(original streamfile CCSID 37)
       47  2025-01-01 12:34   $TESTSTMF_A_NLS
(original streamfile CCSID 1208)
---------                     -------
      141                     3 files

EOF
}

teardown_file() {
	system dltlib "$TESTLIB"
}
