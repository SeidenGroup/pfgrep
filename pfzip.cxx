/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

extern "C" {
#include <zip.h>

#include "errc.h"
}

#include <fmt/format.h>

#include <cstring>
#include <string>

#include "common.hxx"

class pfzip : public pfbase {
public:
	int do_action(File &file) override;
	void print_version(const char *tool_name);

	/* Archive */
	zip_t *archive;
	/* Archive options */
	bool overwrite : 1;
	bool dont_replace_extension : 1;
private:
	std::string normalize_path(const File &file);
};

void pfzip::print_version(const char *tool_name)
{
	pfbase::print_version(tool_name);
	fmt::print(stderr, "\tusing libzip {}\n", zip_libzip_version());
}

static void usage(char *argv0)
{
	fmt::print(stderr, "usage: {} [-EprstWV] output_file.zip files\n", argv0);
}

/**
 * Change the filename to be better suited to an archive. This can remove
 * the leading / from a path to make it not absolute, and replace the file
 * extension of a PF member to match its source type when possible.
 */
std::string pfzip::normalize_path(const File &file)
{
	const char *input = file.filename;
	// Make sure the path in the zip won't be absolute.
	if (input[0] == '/') {
		input++;
	}
	std::string new_path(file.filename + 1);
	// If requested, we can replace the generic .MBR suffix on a member
	// with a file extension derived from the member's source type.
	// XXX: make case insensitive, C++20
	if (this->dont_replace_extension || new_path.find("QSYS.LIB/") == std::string::npos) {
		return new_path;
	}
	std::string source_type(file.source_type);
	// Trim whitespace in source_type
	source_type.resize(source_type.find(' '));
	if (source_type.empty()) {
		return new_path;
	}
	std::string::size_type ext_pos = new_path.find(".MBR");
	if (ext_pos != std::string::npos) {
		new_path.replace(ext_pos + 1, 3, source_type);
	}
	return new_path;
}

int pfzip::do_action(File &file)
{
	zip_int64_t index = -1;
	int nonfatal_ret;
	const char *buf = this->conv_buffer;
	if (file.record_length == 0 && file.ccsid == this->pase_ccsid) {
		buf = this->read_buffer;
	}
	size_t len = strlen(buf);
	// we must keep a copy around until zip_close, and we reread the buffer
	// therefore make a copy (NBD) and tell libzip to free (last parm)
	char *buf_copy = strdup(buf);
	zip_source_t *s = zip_source_buffer(this->archive, buf_copy, len, 1);
	if (s == NULL && !this->silent) {
		fmt::print(stderr, "zip_source_buffer({}): {}\n",
			file.filename,
			zip_strerror(this->archive));
		return -1;
	}

	auto path = normalize_path(file);
	index = zip_file_add(this->archive, path.c_str(), s, 0);
	if (index == -1 && !this->silent) {
		fmt::println(stderr, "zip_file_add({}): {}",
			file.filename,
			zip_strerror(this->archive));
		zip_source_free(s);
		return -1;
	}

	// Put the member description as a comment.
	// The other metdata is there too; may not be best place for it
	std::string comment;
	if (file.record_length == 0) {
		comment = fmt::format("(original streamfile CCSID {})", file.ccsid);
	} else if (*file.description) {
		comment = fmt::format("{} (original PF record length {} CCSID {})",
			file.description,
			file.record_length,
			file.ccsid);
	} else {
		comment = fmt::format("(original PF record length {} CCSID {})",
			file.record_length,
			file.ccsid);
	}
	// not critical if these fail, but do warn
	nonfatal_ret = zip_file_set_comment(this->archive, index, comment.c_str(), comment.size(), 0);
	if (nonfatal_ret && !this->silent) {
		fmt::println(stderr, "zip_file_set_comment: Can't set comment for {}",
			file.filename);
	}
	nonfatal_ret = zip_file_set_mtime(this->archive, index, file.mtime, 0);
	if (nonfatal_ret && !this->silent) {
		fmt::println(stderr, "zip_file_set_comment: Can't set modification time ({}) for {}",
			file.mtime,
			file.filename);
	}
	return 1;
}

int main(int argc, char **argv)
{
	auto state = pfzip();

	int ch;
	while ((ch = getopt(argc, argv, "EprstWV")) != -1) {
		switch (ch) {
		case 'E':
			state.dont_replace_extension = true;
			break;
		case 'p':
			state.search_non_source_files = true;
			break;
		case 'r':
			state.recurse = true;
			break;
		case 's':
			state.silent = true;
			break;
		case 't':
			state.dont_trim_ending_whitespace = true;
			break;
		case 'W':
			state.overwrite = true;
			break;
		case 'V':
			state.print_version("pfzip");
			return 0;
		default:
			usage(argv[0]);
			return 3;
		}
	}

	if (optind + 1 >= argc) {
		usage(argv[0]);
		return 3;
	}
	const char *output_file = argv[optind++];
	state.file_count = argc - optind;
	if (state.file_count == 0 && !state.silent) {
		fmt::println(stderr, "{}: need files for archive", argv[0]);
		return 5;
	}

	int zerrno;
	int open_flags = ZIP_CREATE;
	if (state.overwrite) {
		open_flags |= ZIP_TRUNCATE;
	}
	state.archive = zip_open(output_file, open_flags, &zerrno);
	if (state.archive == NULL && !state.silent) {
		zip_error_t error;
		zip_error_init_with_code(&error, zerrno);
		fmt::println(stderr, "zip_open: {}",
        		zip_error_strerror(&error));
		zip_error_fini(&error);
		return 6;
	}

	bool any_match = false, any_error = false;
	for (int i = optind; i < argc; i++) {
		int ret = state.do_thing(argv[i], false);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

	if (zip_close(state.archive) == -1 && !state.silent) {
		fmt::println(stderr, "zip_close: {}", zip_strerror(state.archive));
		return 4;
	}

	return any_error ? 2 : (any_match ? 0 : 1);
}
