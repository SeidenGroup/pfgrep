/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <as400_protos.h>
#include <as400_types.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/mode.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "errc.h"

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-prtV] files\n", argv0);
}

int do_action(pfgrep *state, File *file)
{
	if (file->record_length == 0) {
		if (file->ccsid == state->pase_ccsid) {
			printf("%s", state->read_buffer);
		} else {
			printf("%s", state->conv_buffer);
		}
	} else {
		printf("%s", state->conv_buffer);
	}
	return 0;
}

int main(int argc, char **argv)
{
	pfgrep state = {0};
	common_init(&state);

	int ch;
	while ((ch = getopt(argc, argv, "prtV")) != -1) {
		switch (ch) {
		case 'p':
			state.search_non_source_files = true;
			break;
		case 'r':
			state.recurse = true;
			break;
		case 't':
			state.dont_trim_ending_whitespace = true;
			break;
		case 'V':
			print_version("pfcat");
			return 0;
		default:
			usage(argv[0]);
			return 3;
		}
	}

	state.file_count = argc - optind;
	bool any_match = false, any_error = false;
	for (int i = optind; i < argc; i++) {
		int ret = do_thing(&state, argv[i], false);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

#ifdef DEBUG
	// This deinitialization may be unnecessary, do it for future use of
	// sanitizers/*grind when available on i
	free_cached_iconv();
	free_cached_record_sizes();
	free(state.read_buffer);
	free(state.conv_buffer);
#endif

	return any_error ? 2 : (any_match ? 0 : 1);
}
