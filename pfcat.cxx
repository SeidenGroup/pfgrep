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

class pfcat : public pfbase {
public:
	int do_action(File *file) override;
};

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-prtV] files\n", argv0);
}

int pfcat::do_action(File *file)
{
	if (file->record_length == 0) {
		if (file->ccsid == this->pase_ccsid) {
			printf("%s", this->read_buffer);
		} else {
			printf("%s", this->conv_buffer);
		}
	} else {
		printf("%s", this->conv_buffer);
	}
	return 0;
}

int main(int argc, char **argv)
{
	auto state = pfcat();

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
			state.print_version("pfcat");
			return 0;
		default:
			usage(argv[0]);
			return 3;
		}
	}

	state.file_count = argc - optind;
	bool any_match = false, any_error = false;
	for (int i = optind; i < argc; i++) {
		int ret = state.do_thing(argv[i], false);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

	return any_error ? 2 : (any_match ? 0 : 1);
}
