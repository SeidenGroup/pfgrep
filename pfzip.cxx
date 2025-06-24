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
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/mode.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zip.h>
}

#include "common.hxx"
#include "errc.h"

class pfzip : public pfbase {
public:
	int do_action(File *file) override;
	void print_version(const char *tool_name);

	/* Archive */
	zip_t *archive;
	/* Archive options */
	bool overwrite : 1;
	bool dont_replace_extension : 1;
};

void pfzip::print_version(const char *tool_name)
{
	pfbase::print_version(tool_name);
	fprintf(stderr, "\tusing libzip %s\n", zip_libzip_version());
}

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-EprstWV] output_file.zip files\n", argv0);
}

static char *ends_with_mbr(const char *str)
{
	char *ext = strrchr(str, '.');
	if (ext && strcasecmp("MBR", ext + 1) == 0) {
		return ext + 1;
	}
	return NULL;
}

static bool is_nul_or_space(const char c)
{
	return c == ' ' || c == '\0';
}

/**
 * Change the filename to be better suited to an archive. This can remove
 * the leading / from a path to make it not absolute, and replace the file
 * extension of a PF member to match its source type when possible.
 */
static void normalize_path(char *output, size_t output_size, File *file, bool replace_mbr_ext)
{
	// If requested, we can replace the generic .MBR suffix on a member
	// with a file extension derived from the member's source type.
	// XXX: Condense these checks, make case insensitive
	if (replace_mbr_ext && !is_nul_or_space(*file->source_type)
		&& strstr(file->filename, "/QSYS.LIB/") != NULL
		&& ends_with_mbr(file->filename) != NULL) {
		char new_path[PATH_MAX + 1];
		// +1 to eliminate leading / for abs path
		strncpy(new_path, file->filename + 1, sizeof(new_path));
		// Copy without whitespace
		char *extension = ends_with_mbr(new_path);
		const char *source_type = file->source_type;
		while (!is_nul_or_space(*source_type)) {
			*extension++ = *source_type++;
		}
		*extension = '\0';
		strncpy(output, new_path, output_size);
		return;
	}
	// Otherwise, just use the path as-is, removing absoluteness.
	const char *input = file->filename;
	if (input[0] == '/') {
		input++;
	}
	strncpy(output, input, output_size);
}

int pfzip::do_action(File *file)
{
	zip_int64_t index = -1;
	int ret = 1, nonfatal_ret;
	const char *buf = this->conv_buffer;
	if (file->record_length == 0 && file->ccsid == this->pase_ccsid) {
		buf = this->read_buffer;
	}
	size_t len = strlen(buf);
	// we must keep a copy around until zip_close, and we reread the buffer
	// therefore make a copy (NBD) and tell libzip to free (last parm)
	char *buf_copy = strdup(buf);
	zip_source_t *s = zip_source_buffer(this->archive, buf_copy, len, 1);
	if (s == NULL && !this->silent) {
		fprintf(stderr, "zip_source_buffer(%s): %s\n",
			file->filename,
			zip_strerror(this->archive));
		goto fail;
	}

	char path[PATH_MAX + 1];
	normalize_path(path, sizeof(path), file, !this->dont_replace_extension);
	index = zip_file_add(this->archive, path, s, 0);
	if (index == -1 && !this->silent) {
		fprintf(stderr, "zip_file_add(%s): %s\n",
			file->filename,
			zip_strerror(this->archive));
		zip_source_free(s);
		ret = -1;
		goto fail;
	}

	char comment[512];
	// Put the member description as a comment.
	// The other metdata is there too; may not be best place for it
	if (file->record_length == 0) {
		snprintf(comment, 512, "(original streamfile CCSID %d)", file->ccsid);
	} else if (*file->description) {
		snprintf(comment, 512, "%s (original PF record length %d CCSID %d)",
			file->description,
			file->record_length,
			file->ccsid);
	} else {
		snprintf(comment, 512, "(original PF record length %d CCSID %d)",
			file->record_length,
			file->ccsid);
	}
	// not critical if these fail, but do warn
	nonfatal_ret = zip_file_set_comment(this->archive, index, comment, strlen(comment), 0);
	if (nonfatal_ret && !this->silent) {
		fprintf(stderr, "zip_file_set_comment: Can't set comment for %s",
			file->filename);
	}
	nonfatal_ret = zip_file_set_mtime(this->archive, index, file->mtime, 0);
	if (nonfatal_ret && !this->silent) {
		fprintf(stderr, "zip_file_set_comment: Can't set modification time (%zd) for %s",
			file->mtime,
			file->filename);
	}
fail:
	return ret;
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
		fprintf(stderr, "%s: need files for archive\n", argv[0]);
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
		fprintf(stderr, "zip_open: %s\n",
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
		fprintf(stderr, "zip_close: %s\n", zip_strerror(state.archive));
		return 4;
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
