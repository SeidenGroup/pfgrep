/*
 * Copyright (c) 2021-2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

size_t ebcdic2utf (const char *ebcdic, int ebcdic_len, char *utf);
size_t utf2ebcdic (const char *utf, int ebcdic_len, char *ebcdic);
int ztoi (char *zoned, int len);
