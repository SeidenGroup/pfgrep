/*
 * Copyright (c) 2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

extern "C" {
#include <as400_protos.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>

#include <json_object.h>

#include "errc.h"

#include <fcntl.h>
#include "common.h"
}

#include "pgmfunc.hxx"

const char MBRD0200[] = {0xd4, 0xc2, 0xd9, 0xc4, 0xf0, 0xf2, 0xf0, 0xf0};

static auto QUSRMBRD = PGMFunction<char*, int, const char*, const char*, const char*, const char, ERRC0100*>("QSYS", "QUSRMBRD"); 

// assume EBCDIC
extern "C" bool get_member_info(File *file)
{
	char output[8192];
	memset(output, 0, 8192);

	ERRC0100 errc = {};
	errc.bytes_avail = sizeof(ERRC0100);

	QUSRMBRD(output, sizeof(output), MBRD0200, file->libobj, file->member, 0xF0, &errc);
	if (errc.exception_id[0] != '\0') {
		// XXX: Translate common messages like CPF5715 into ENOENT, etc.
		errno = ENOSYS;
		return false;
	}

	file->record_count = *(uint32_t*)(output + 0x8C);

	// XXX: Convert to using struct
	iconv_t sys_conv = get_iconv(37);
	char *in = output + 0x30, *out = file->source_type;
	size_t inleft = 10, outleft = sizeof(file->source_type);
	iconv(sys_conv, &in, &inleft, &out, &outleft);

	int32_t desc_ccsid = *(uint32_t*)(output + 0xF0);
	// 65535 is no-convert, but we want to convert, and binary descriptions
	// should be rare. Often, it's set for no description, or things that
	// predate ~V2R1.
	if (desc_ccsid == 65535 || desc_ccsid == 0) {
		desc_ccsid = Qp2jobCCSID();
	}

	{
		iconv_t desc_conv = get_iconv(desc_ccsid);
		char *in = output + 0x54, *out = file->description;
		size_t inleft = 50, outleft = sizeof(file->description);
		iconv(desc_conv, &in, &inleft, &out, &outleft);
		// reset in case of shift state
		reset_iconv(desc_conv);
		*out = '\0';
	}

	return true;
}
