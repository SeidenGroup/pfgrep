/*
 * Copyright (c) 2021-2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include </QOpenSys/usr/include/iconv.h>
#include <as400_protos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static iconv_t e2a, a2e;

static void
init_iconv (void)
{
	// We use the encoding of a file for the grep part, but /QSYS.LIB names
	// are coerced to 37, and exception IDs are the same in all encodings.
	// Thus, it's not worth it to use the job CCSID.
	e2a = iconv_open(ccsidtocs(Qp2paseCCSID()), ccsidtocs(37));
	a2e = iconv_open(ccsidtocs(37), ccsidtocs(Qp2paseCCSID()));
	if (e2a == (iconv_t)(-1) || a2e == (iconv_t)(-1)) {
		fprintf(stderr, "Failed to open iconv for utility functions\n");
		abort();
	}
}

/*
 * Because EBCDIC strings are usually fixed-length and padded by ASCII, try to
 * cope by copying it to a fixed null-terminated buffer.
 */
size_t
ebcdic2utf (const char *ebcdic, int ebcdic_len, char *utf)
{
	size_t inleft, outleft, ret;
	if (e2a == NULL) {
		init_iconv ();
	}
	inleft = outleft = ebcdic_len + 1;
	char *temp;
	temp = malloc (ebcdic_len + 1);
	strncpy (temp, ebcdic, ebcdic_len);
	temp [ebcdic_len] = '\0';
	ret = iconv (e2a, &temp, &inleft, &utf, &outleft);
	free (temp);
	return ret;
}

/* Convert a UTF-8 string to a fixed-length EBCDIC string. */
size_t utf2ebcdic (const char *utf, int ebcdic_len, char *ebcdic)
{
	size_t inleft, outleft, ret;
	if (a2e == NULL) {
		init_iconv ();
	}
	inleft = outleft = ebcdic_len + 1;
	char *temp;
	temp = malloc (ebcdic_len + 1);
	char *orig = temp;
	sprintf (temp, "%-10s", utf);
	ret = iconv (a2e, &temp, &inleft, &ebcdic, &outleft);
	free (orig);
	return ret;
}

/*
 * Zoned decimal to integer conversion
 */
int
ztoi (char *zoned, int len)
{
	char *utf = malloc(len + 1);
	ebcdic2utf (zoned, len, utf);
	return atoi (utf);
}
