setup() {
	load 'test_helper/bats-support/load'
	load 'test_helper/bats-assert/load'

	# get the containing directory of this file
	# use $BATS_TEST_FILENAME instead of ${BASH_SOURCE[0]} or $0,
	# as those will point to the bats executable's location or the preprocessed file respectively
	DIR="$( cd "$( dirname "$BATS_TEST_FILENAME" )" >/dev/null 2>&1 && pwd )"
	PATH="$DIR/../:$PATH"

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
}

@test "basic fixed string" {
	run pfgrep -t -F 'AB' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
ABC
AB
AB
ABC
EOF
}

@test "regular expression" {
	run pfgrep -t '^A.C$' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
ABC
ABC
EOF
}

@test "multiple expressions" {
	run pfgrep -t -e '^A.C$' -e "BAR$"  "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
ABC
ABC
FOO BAR
FOOBAR
EOF
}

@test "multiple expressions from file" {
	run pfgrep -t -f -  "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR" <<EOF
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
	run pfgrep -t -w 'BAR' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output "FOO BAR"
}

@test "line match" {
	run pfgrep -t -x 'FOOBAR' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output "FOOBAR"
}

@test "case insensitivity" {
	run pfgrep -t -i 'abc' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
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
	run pfgrep -t -H -n 'AB' "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:1:ABC
/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:2:AB
/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:5:AB
/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR:6:ABC
EOF
}

teardown() {
	system dltlib "$TESTLIB"
}
