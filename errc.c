/*
 * Copyright (c) 2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <sys/errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <os400msg.h>

#include "ebcdic.h"
#include "errc.h"

static bool
get_xpf_exception (char msg[8])
{
	/* truncated version of the RCVM0100 struct w/ proper packing */
	struct __attribute ((packed)) {
		int  Bytes_Returned;
		int  Bytes_Available;
		int  Message_Severity;
		char Message_Id[7];
		char Message_Type[2];
	} msg_info = {0};
	msg_info.Bytes_Available = sizeof(msg_info);
	int outkey = 0x40404040; // four EBCDIC spaces for message key
	ERRC0100 errc = {0};
	errc.bytes_avail = sizeof(ERRC0100);

	int ret = QMHRCVPM(&msg_info, sizeof(msg_info), "RCVM0100", "*", 0, "*EXCP", &outkey, 0, "*SAME", &errc);
	if (ret != 0) {
		perror("QMHRCVPM");
		return false;
	}

	if (msg_info.Message_Id[0] == '\0') {
		return false; // No message
	}

	ebcdic2utf(msg_info.Message_Id, 7, msg);
	msg[7] = '\0';
	return true;
}

void perror_xpf(const char *s)
{
	char msg[8] = {0};
	if (errno == EUNKNOWN && get_xpf_exception(msg)) {
		fprintf(stderr, "%s: %s\n", s, msg);
	} else if (errno == EUNKNOWN) {
		fprintf(stderr, "%s: Unknown error without an exception occured\n", s);
	} else {
		perror(s);
	}
}
