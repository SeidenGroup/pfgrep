/*
 * Copyright (c) 2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <as400_protos.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>

#include <json_object.h>

#include "ebcdic.h"
#include "errc.h"

#include <fcntl.h>
#include "common.h"

static ILEpointer QUSRMBRD_pgm __attribute__ ((aligned (16)));
static int QUSRMBRD_initialized = false;

const char MBRD0200[] = {0xd4, 0xc2, 0xd9, 0xc4, 0xf0, 0xf2, 0xf0, 0xf0};

static bool
init_pgm (void)
{
	if (0 != _RSLOBJ2(&QUSRMBRD_pgm, RSLOBJ_TS_PGM, "QUSRMBRD", "QSYS")) {
		perror("resolving QSYS/QUSRMBRD");
		return false;
	}
	QUSRMBRD_initialized = true;
	return true;
}

static void
QUSRMBRD (char *output, const int *outlen, const char *format, const char *filename, const char *member, const char *override, ERRC0100 *error)
{
	if (!QUSRMBRD_initialized) {
		if (!init_pgm()) {
			abort();
		}
	}
	/* Assume caller passes in EBCDIC */
	void *pgm_argv[] = {
		(void*)output,
		(void*)outlen,
		(void*)format,
		(void*)filename,
		(void*)member,
		(void*)override,
		(void*)error,
		NULL
	};
	if (0 != _PGMCALL(&QUSRMBRD_pgm, pgm_argv, PGMCALL_EXCP_NOSIGNAL)) {
		perror_xpf("QUSRMBRD");
	}
}

// assume EBCDIC
bool get_member_info(File *file)
{
	char output[8192];
	memset(output, 0, 8192);
	int outlen = sizeof(output);
	const char override = 0xF0; /* EBCDIC '0' */

	ERRC0100 errc = {0};
	errc.bytes_avail = sizeof(ERRC0100);

	QUSRMBRD(output, &outlen, MBRD0200, file->libobj, file->member, &override, &errc);
	if (errc.exception_id[0] != '\0') {
		// XXX: Translate common messages like CPF5715 into ENOENT, etc.
		errno = ENOSYS;
		return false;
	}

	// XXX: Convert to using struct
	char *source_type = output + 0x30;
	ebcdic2utf(source_type, 10, file->source_type);

	int32_t desc_ccsid = *(uint32_t*)(output + 0xF0);
	char *out = file->description;
	// It's not documented as such, but 65535 seems to imply an empty desc
	if (desc_ccsid != 65535) {
		iconv_t desc_conv = get_iconv(desc_ccsid);
		char *desc = output + 0x54;
		size_t inleft = 50, outleft = inleft * UTF8_SCALE_FACTOR;
		memset(out, 0, outleft);
		iconv(desc_conv, &desc, &inleft, &out, &outleft);
	}
	*out = '\0';

	return true;
}
