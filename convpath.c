/*
 * Copyright (c) 2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <as400_protos.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "common.h"
#include "errc.h"

typedef struct __attribute__((packed)) Qlg_Path_Name {
	int  CCSID;
	char Country_ID[2];
	char Language_ID[3];
	char Reserved[3];
	unsigned int Path_Type;
	int  Path_Length;
	char Path_Name_Delimiter[2];
	char Reserved2[10];
} Qlg_Path_Name_T;

typedef struct QSYS0100 {
	int bytes_returned;
	int bytes_available;
	int ccsid_out;
	char lib_name[28];
	char lib_type[20];
	char obj_name[28];
	char obj_type[20];
	char mbr_name[28];
	char mbr_type[20];
	char asp_name[28];
} QSYS0100;

const char QSYS0100_name[] = { 0xD8, 0xE2, 0xE8, 0xE2, 0xF0, 0xF1, 0xF0, 0xF0, 0x00 };

static ILEpointer Qp0lCvtPathToQSYSObjName_sym __attribute__ ((aligned (16)));
static int QP0LLIB2_mark = -1;
static bool QP0LLIB2_initialized = false;

static bool
init_pgm (void)
{
	QP0LLIB2_mark = _ILELOAD("QSYS/QP0LLIB2", ILELOAD_LIBOBJ);
	if (QP0LLIB2_mark == -1) {
		perror("resolving QSYS/QP0LLIB2");
		return false;
	}
	if (_ILESYM(&Qp0lCvtPathToQSYSObjName_sym, QP0LLIB2_mark, "Qp0lCvtPathToQSYSObjName") == -1) {
		perror("resolving Qp0lCvtPathToQSYSObjName");
		return false;
	}
	QP0LLIB2_initialized = true;
	return true;
}

static void
Qp0lCvtPathToQSYSObjName (Qlg_Path_Name_T *path_name, QSYS0100 *qsys_info, const char *format_name, unsigned int bytes_provided, unsigned int desired_CCSID, ERRC0100 *error)
{
	if (!QP0LLIB2_initialized) {
		if (!init_pgm()) {
			abort();
		}
	}
	/* Assume caller passes in EBCDIC */
	struct {
		ILEarglist_base base __attribute__ ((aligned (16)));
		ILEpointer _path_name __attribute__ ((aligned (16)));
		ILEpointer _qsys_info __attribute__ ((aligned (16)));
		ILEpointer _format_name __attribute__ ((aligned (16)));
		unsigned int _bytes_provided;
		unsigned int _desired_CCSID;
		ILEpointer _error __attribute__ ((aligned (16)));
	} arglist __attribute__ ((aligned (16)));
	arglist._path_name.s.addr = (address64_t)path_name;
	arglist._qsys_info.s.addr = (address64_t)qsys_info;
	arglist._format_name.s.addr = (address64_t)format_name;
	arglist._bytes_provided = bytes_provided;
	arglist._desired_CCSID = desired_CCSID;
	arglist._error.s.addr = (address64_t)error;
	const arg_type_t argtypes[] = {
		ARG_MEMPTR,
		ARG_MEMPTR,
		ARG_MEMPTR,
		ARG_UINT32,
		ARG_UINT32,
		ARG_MEMPTR,
		ARG_END
	};
	if (-1 != _ILECALLX(&Qp0lCvtPathToQSYSObjName_sym, &arglist.base, argtypes, RESULT_VOID, ILECALL_NOINTERRUPT)) {
		// Interpret the result of the error param in the caller.
	}
}

// XXX: Leaks, probably should move to conv funcs...
static iconv_t a2e = NULL;

/**
 * Takes an ASCII IFS path to a traditional object (like /QSYS.LIB/QGPL.LIB/QCLSRC.FILE/X.MBR)
 * and breaks it down into three 29-character EBCDIC strings.
 */
int filename_to_libobj(File *file)
{
	struct {
		Qlg_Path_Name_T	qlg;
		char path[1024];
	} input_qlg  = {0};

	if (a2e == NULL) {
		a2e = iconv_open(ccsidtocs(37), ccsidtocs(Qp2paseCCSID()));
	}
	char *in = (char*)file->filename, *out = input_qlg.path;
	size_t inleft = strlen(file->filename), outleft = 1024;
	iconv(a2e, &in, &inleft, &out, &outleft);

	// /QSYS.LIB/... path names are coerced to 37
	input_qlg.qlg.CCSID = 37;
	input_qlg.qlg.Path_Length = strlen(input_qlg.path);
	input_qlg.qlg.Path_Name_Delimiter[0] = 0x61; // ebcdic slash

	QSYS0100 qsys = {0};
	qsys.bytes_available = sizeof(qsys);
	ERRC0100 errc = {0};
	errc.bytes_in = sizeof(errc);

	Qp0lCvtPathToQSYSObjName(&input_qlg.qlg, &qsys, QSYS0100_name, sizeof(qsys), 37, (ERRC0100*)&errc);
	if (errc.exception_id[0] != '\0') {
		/* likely CPFA0DB */
		return -1;
	}

	memcpy(file->libobj, qsys.obj_name, 10);
	memcpy(file->libobj + 10, qsys.lib_name, 10);
	// Ensure filename is space and not null padded
	for (int i = 0; i < 20; i++) {
		if (file->libobj[i] == '\0') {
			file->libobj[i] = 0x40; /* EBCDIC ' ' */
		}
	}
	// Null terminate for json-c
	file->libobj[20] = '\0';
	
	memcpy(file->member, qsys.mbr_name, 10);
	for (int i = 0; i < 10; i++) {
		if (file->member[i] == '\0') {
			file->member[i] = 0x40;
		}
	}
	file->member[10] = '\0';

	return 0;
}
