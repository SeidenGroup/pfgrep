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

#include "errc.h"

}

#include "common.hxx"
#include "ebcdic.hxx"
#include "pgmfunc.hxx"

using namespace pase_cpp;

#include <map>
#include <string>

EF<8> FILD0100("FILD0100");
EF<10> _FIRST("*FIRST");
EF<10> _FILETYPE("*FILETYPE");
EF<10> _INT("*INT");

static PGMFunction<char*, int, char*, const char*, const char*, const char*, const char, const char*, const char*, ERRC0100*> QDBRTVFD("QSYS", "QDBRTVFD");

static std::map<std::string, int> cached_record_sizes;

/**
 * Gets information about a physical file. Returns the record length as a
 * positive number if source PF, as a negative number if not, and 0 if error.
 * errno is set when 0 on error is returned.
 */
extern "C" int get_pf_info(File *file)
{
	// XXX: Is it better to use array<char, 20>?
	std::string filename(file->libobj, 20);
	// Look at our cached records first
	auto cached_record_size = cached_record_sizes.find(filename);
	if (cached_record_size != cached_record_sizes.end()) {
		return cached_record_size->second;
	}

	char output_filename[21];
	/* XXX: Convert to using a Qdb_Qdbfh structure... */
	char output[8192];
	memset(output, 0, 8192);

	ERRC0100 errc = {};
	errc.bytes_avail = sizeof(ERRC0100);

	QDBRTVFD(output, sizeof(output), output_filename, FILD0100.value, filename.data(), _FIRST.value, '1'_e, _FILETYPE.value, _INT.value, &errc);
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

	cached_record_sizes[filename] = ret;

	return ret;
}
