/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

extern "C" {
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

#include "errc.h"
}

#include "common.hxx"

class pfstat : public pfbase {
};

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-prV] files\n", argv0);
}

int do_action(pfbase *state, File *file)
{
	if (file->record_length != 0) {
		get_member_info(file);
	} else {
		// Better off using i.e. stat
		if (state->silent) {
			fprintf(stderr, "%s: Not a member\n",
				file->filename);
		}
		return -1;
	}
	printf("%s\t%ld\t%s\t%d\t%d\t%s\n",
		file->filename,
		file->file_size,
		file->source_type,
		file->record_length,
		file->ccsid,
		file->description);
	return 0;
}

int main(int argc, char **argv)
{
	auto state = pfstat();
	state.dont_read_file = true;

	int ch;
	while ((ch = getopt(argc, argv, "prV")) != -1) {
		switch (ch) {
		case 'p':
			state.search_non_source_files = true;
			break;
		case 'r':
			state.recurse = true;
			break;
		case 'V':
			state.print_version("pfstat");
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
