/*
 * Copyright (c) 2021-2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EUNKNOWN
#define EUNKNOWN 3474
#endif

/* These probably don't match their names in XPF headers */
typedef struct {
	int bytes_in;
	int bytes_avail;
	char exception_id[7];
	char reserved;
} ERRC0100;

typedef struct {
	int key;
	int bytes_in;
	int bytes_avail;
	char exception_id[7];
	char reserved;
	int ccsid;
	int offset;
	int length;
	char data[];
} ERRC0200;

void perror_xpf(const char *s);
