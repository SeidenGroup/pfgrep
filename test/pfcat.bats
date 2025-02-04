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

@test "reading physical file member" {
	run pfcat "/QSYS.LIB/$TESTLIB.LIB/QTXTSRC.FILE/ABC.MBR"
	
	assert_output - <<EOF
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

@test "reading EBCDIC streamfile" {
	run pfcat "$TESTSTMF_E"

	assert_output - <<EOF
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

@test "reading UTF-8 streamfile" {
	run pfcat "$TESTSTMF_A"

	assert_output - <<EOF
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

teardown_file() {
	system dltlib "$TESTLIB"
}
