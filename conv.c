/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <as400_protos.h>
#include <stdbool.h>

#include "common.h"

// These contain conversions from convs[N] to system PASE CCSID, memoized to
// avoid constantly reopening iconv for conversion. Gets closed on exit.
// Because we only convert to a single CCSID, we can keep the maping flat.
// The memory cost of this should be minimal on modern systems.
static iconv_t convs[UINT16_MAX];

// Used for converting from PASE CCSID to 37, which is used for paths, as
// well as various locale-invariant things we usually convert from than to.
static iconv_t pase_to_system_iconv = NULL;

iconv_t get_pase_to_system_iconv(void)
{
	if (pase_to_system_iconv == NULL) {
		pase_to_system_iconv = iconv_open(ccsidtocs(37), ccsidtocs(Qp2paseCCSID()));
	}
	return pase_to_system_iconv;
}

iconv_t get_iconv(uint16_t ccsid)
{
	iconv_t conv = convs[ccsid];
	if (conv == NULL || conv == (iconv_t)(-1)) {
		conv = iconv_open(ccsidtocs(Qp2paseCCSID()), ccsidtocs(ccsid));
		convs[ccsid] = conv;
	}
	return conv;
}

void free_cached_iconv(void)
{
	for (int i = 0; i < UINT16_MAX; i++) {
		iconv_t conv = convs[i];
		if (conv == NULL || conv == (iconv_t)(-1)) {
			continue;
		}
		iconv_close(conv);
	}
	iconv_close(pase_to_system_iconv);
}

/**
 * Resets iconv shift state for MBCS encodings.
 */
void reset_iconv(iconv_t conv)
{
	iconv(conv, NULL, NULL, NULL, NULL);
}
