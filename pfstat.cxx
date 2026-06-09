/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

extern "C" {
#include "errc.h"
}

#include <fmt/base.h>

#include "common.hxx"

class pfstat : public pfbase {
public:
	int do_action(File &file) override;
};

static void usage(char *argv0)
{
	fmt::print(stderr, "usage: {} [-prV] files\n", argv0);
}

int pfstat::do_action(File &file)
{
	if (file.record_length == 0) {
		// Better off using i.e. stat
		if (this->silent) {
			fmt::println(stderr, "{}: Not a member",
				file.full_filename);
		}
		return -1;
	}
	fmt::println("{}\t{}\t{}\t{}\t{}\t{}",
		file.full_filename,
		file.file_size,
		file.source_type,
		file.record_length,
		file.ccsid,
		file.description);
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
		int ret = state.do_thing(argv[i], false);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

	return any_error ? 2 : (any_match ? 0 : 1);
}
