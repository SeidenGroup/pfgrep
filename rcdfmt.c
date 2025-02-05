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

#include "common.h"

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
	}
}

static json_object *cached_record_sizes = NULL;

void free_cached_record_sizes(void)
{
	json_object_put(cached_record_sizes);
}

/**
 * Gets information about a physical file. Returns the record length as a
 * positive number if source PF, as a negative number if not, and 0 if error.
 * errno is set when 0 on error is returned.
 */
int get_pf_info(File *file)
{
	const char *filename = file->libobj;
	// Look at our cached records first
	if (cached_record_sizes == NULL) {
		cached_record_sizes = json_object_new_object();
	}
	json_object *cached_record_size = json_object_object_get(cached_record_sizes, filename);
	if (cached_record_size != NULL) {
		return json_object_get_int(cached_record_size);
	}

	/* It might look at the output filename... */
	char output_filename[21];
	memcpy(output_filename, filename, 21);

	/* XXX: Convert to using a Qdb_Qdbfh structure... */
	char output[8192];
	memset(output, 0, 8192);
	int outlen = sizeof(output);
	const char override = 0xF0; /* EBCDIC '0' */

	ERRC0100 errc = {0};
	errc.bytes_avail = sizeof(ERRC0100);

	QDBRTVFD(output, &outlen, output_filename, FILD0100, filename, _FIRST, &override, _FILETYPE, _INT, &errc);
	if (errc.exception_id[0] != '\0') {
		// XXX: Translate common messages like CPF5715 into ENOENT, etc.
		errno = ENOSYS;
		return 0;
	}

	// We use POSIX I/O to read record files, and this has limits:
	// https://www.ibm.com/docs/en/i/7.5?topic=qsyslib-file-handling-restrictions-in-file-system

	// Is this a logical file?
	bool Qdbfhfpl = output[8] & 0x20;
	if (Qdbfhfpl) {
		errno = ENODEV;
		return 0;
	}

	// Is this a source PF?
	bool Qdbfhfsu = output[8] & 0x08;

	// How many fields do we have?
	int16_t Qdbfmxfnum = *(int16_t*)(output + 206);
	// Is this program or externally described?
	// We can only use program described with a single field, source PFs,
	// or externally described, since we open as binary.
	bool Qdbfpgmd = output[60] & 0x70;
	if (!Qdbfhfsu && (!Qdbfpgmd || Qdbfmxfnum < 2)) {
		errno = ENODEV;
		return 0;
	}

	// Record length
	int Qdbfmxrl = *(int16_t*)(output + 304);

	// Compress return value into a single integer that's easily stored
	int ret = Qdbfhfsu ? Qdbfmxrl : -Qdbfmxrl;

	// Safe to use JSON_C_OBJECT_ADD_KEY_IS_NEW since we check beforehand
	json_object_object_add_ex(cached_record_sizes, filename, json_object_new_int(ret), JSON_C_OBJECT_ADD_KEY_IS_NEW);

	return ret;
}
