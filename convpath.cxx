/*
 * Copyright (c) 2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

extern "C" {
#include <as400_protos.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "errc.h"
}

#include "common.hxx"
#include "ebcdic.hxx"
#include "ilefunc.hxx"

using namespace pase_cpp;

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

EF<8> QSYS0100_name("QSYS0100");

static ILEFunction<void, Qlg_Path_Name_T*, QSYS0100*, const char*, unsigned int, unsigned int, ERRC0100*> Qp0lCvtPathToQSYSObjName("QSYS/QP0LLIB2", "Qp0lCvtPathToQSYSObjName");

/**
 * Takes an ASCII IFS path to a traditional object (like /QSYS.LIB/QGPL.LIB/QCLSRC.FILE/X.MBR)
 * and breaks it down into three 29-character EBCDIC strings.
 */
extern "C" int filename_to_libobj(File *file)
{
	struct {
		Qlg_Path_Name_T	qlg;
		char path[1024];
	} input_qlg  = {};

	iconv_t a2e = get_pase_to_system_iconv();
	char *in = (char*)file->filename, *out = input_qlg.path;
	size_t inleft = strlen(file->filename), outleft = 1024;
	iconv(a2e, &in, &inleft, &out, &outleft);

	// /QSYS.LIB/... path names are coerced to 37
	input_qlg.qlg.CCSID = 37;
	input_qlg.qlg.Path_Length = strlen(input_qlg.path);
	input_qlg.qlg.Path_Name_Delimiter[0] = 0x61; // ebcdic slash

	QSYS0100 qsys = {};
	qsys.bytes_available = sizeof(qsys);
	ERRC0100 errc = {};
	errc.bytes_in = sizeof(errc);

	Qp0lCvtPathToQSYSObjName(&input_qlg.qlg, &qsys, QSYS0100_name.value, sizeof(qsys), 37, (ERRC0100*)&errc);
	if (errc.exception_id[0] != '\0') {
		/* likely CPFA0DB */
		perror_xpf("Qp0lCvtPathToQSYSObjName");
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
