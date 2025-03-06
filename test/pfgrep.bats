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
	system crtsrcpf "$TESTLIB/qtxtsrc"
	system addpfm "$TESTLIB/qtxtsrc" abc
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

	TESTSTMF_A=$(mktemp /tmp/pfgrep_test.XXXXXXX)
	export TESTSTMF_A
	Rfile -r "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" > "$TESTSTMF_A"
	setccsid 1208 "$TESTSTMF_A"

	TESTSTMF_E=$(mktemp /tmp/pfgrep_test.XXXXXXX)
	export TESTSTMF_E
	Rfile -r "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" \
		| /usr/bin/iconv -f UTF-8 -t IBM-037 > "$TESTSTMF_E"
	setccsid 37 "$TESTSTMF_E"
}

@test "basic fixed string" {
	run pfgrep -F 'AB' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
ABC
AB
AB
ABC
EOF
}

@test "regular expression" {
	run pfgrep '^A.C$' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
ABC
ABC
EOF
}

@test "multiple expressions" {
	run pfgrep -e '^A.C$' -e "BAR$"  "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
ABC
ABC
FOO BAR
FOOBAR
EOF
}

@test "multiple expressions from file" {
	run pfgrep -f -  "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" <<EOF
A.C$
BAR$
EOF
	
	assert_output - <<EOF
ABC
ABC
FOO BAR
FOOBAR
EOF
}

@test "word match" {
	run pfgrep -w 'BAR' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output "FOO BAR"
}

@test "line match" {
	run pfgrep -x 'FOOBAR' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output "FOOBAR"
}

@test "inverted match" {
	run pfgrep -v 'B' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"

	assert_output - <<EOF

A
DEF
EOF
}

@test "case insensitivity" {
	run pfgrep -i 'abc' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
ABC
ABC
EOF
}

@test "line count" {
	run pfgrep -c 'AB' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"

	assert_output "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:4"
}

@test "forcing file names and line numbers" {
	run pfgrep -H -n 'AB' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:1:ABC
/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:2:AB
/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:5:AB
/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:6:ABC
EOF
}

@test "context lines after" {
	run pfgrep -A 3 -n 'B[AC]' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"

	assert_output - <<EOF
1:ABC
2:AB
3:
4:A
6:ABC
7:DEF
8:FOO BAR
9:FOOBAR
10:FOOBAR FOO
EOF
}

@test "reading EBCDIC streamfile" {
	run pfgrep -Fn 'AB' "$TESTSTMF_E"

	assert_output - <<EOF
1:ABC
2:AB
5:AB
6:ABC
EOF
}

@test "reading UTF-8 streamfile" {
	run pfgrep -Fn 'AB' "$TESTSTMF_A"

	assert_output - <<EOF
1:ABC
2:AB
5:AB
6:ABC
EOF
}

teardown_file() {
	system dltlib "$TESTLIB"
}
