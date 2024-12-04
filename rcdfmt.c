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

#include "ebcdic.h"
#include "errc.h"

static ILEpointer QDBRTVFD_pgm __attribute__ ((aligned (16)));
static int QDBRTVFD_initialized = false;

const char FILD0100[] = {0xc6, 0xc9, 0xd3, 0xc4, 0xf0, 0xf1, 0xf0, 0xf0};
const char _FIRST[] = {0x5c, 0xc6, 0xc9, 0xd9, 0xe2, 0xe3, 0x40, 0x40, 0x40, 0x40};
const char _FILETYPE[] = {0x5c, 0xc6, 0xc9, 0xd3, 0xc5, 0xe3, 0xe8, 0xd7, 0xc5, 0x40};
const char _INT[] = {0x5c, 0xc9, 0xd5, 0xe3, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40};
const char _EXT[] = {0x5c, 0xc5, 0xe7, 0xe3, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40};

static bool
init_pgm (void)
{
	if (0 != _RSLOBJ2(&QDBRTVFD_pgm, RSLOBJ_TS_PGM, "QDBRTVFD", "QSYS")) {
		perror("resolving QSYS/QDBRTVFD");
		return false;
	}
	QDBRTVFD_initialized = true;
	return true;
}

static void
QDBRTVFD (char *output, const int *outlen, char *output_filename, const char *format, const char *filename, const char *record_format, const char *override, const char *system, const char *format_type, ERRC0100 *error)
{
	if (!QDBRTVFD_initialized) {
		if (!init_pgm()) {
			abort();
		}
	}
	/* Assume caller passes in EBCDIC */
	void *pgm_argv[] = {
		(void*)output,
		(void*)outlen,
		(void*)output_filename,
		(void*)format,
		(void*)filename,
		(void*)record_format,
		(void*)override,
		(void*)system,
		(void*)format_type,
		(void*)error,
		NULL
	};
	if (0 != _PGMCALL(&QDBRTVFD_pgm, pgm_argv, PGMCALL_EXCP_NOSIGNAL)) {
		perror_xpf("QDBRTVFD");
		abort();
	}
}

int get_record_size(const char *lib_name, const char *obj_name)
{
	char filename[20], output_filename[20]; /* 10 chars of lib, 10 chars of obj */
	memcpy(filename, obj_name, 10);
	memcpy(filename + 10, lib_name, 10);
	/* Ensure filename is space and not null padded */
	for (int i = 0; i < 20; i++) {
		if (filename[i] == '\0') {
			filename[i] = 0x40; /* EBCDIC ' ' */
		}
	}
	/* It might look at the output filename... */
	memcpy(output_filename, filename, 20);

	/* XXX: Convert to using a Qdb_Qdbfh structure... */
	char output[8192];
	memset(output, 0, 8192);
	int outlen = sizeof(output);
	const char override = 0xF0; /* EBCDIC '0' */

	ERRC0100 errc = {0};
	errc.bytes_avail = sizeof(ERRC0100);

	QDBRTVFD(output, &outlen, output_filename, FILD0100, filename, _FIRST, &override, _FILETYPE, _INT, &errc);
	if (errc.exception_id[0] != '\0') {
		return -1;
	}

	if (!(output[8] & 8)) { // if Qdbfhfsu is not *SRC
		return -2;
	}

	int Qdbfmxrl = *(int16_t*)(output + 304);

	return Qdbfmxrl;
}
