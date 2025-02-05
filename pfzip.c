/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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

#include </QOpenSys/usr/include/iconv.h>
#include <linkhash.h>
#include <zip.h>

#include "common.h"
#include "errc.h"

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-prtV] output_file.zip files\n", argv0);
}

static bool normalize_path(char *output, size_t output_size, const char *input)
{
	if (input[0] == '/') {
		input++;
	}
	strncpy(output, input, output_size);
	return true;
}

int do_action(pfgrep *state, File *file)
{
	int ret = 1;
	const char *buf = state->conv_buffer;
	if (file->record_length == 0 && file->ccsid == state->pase_ccsid) {
		buf = state->read_buffer;
	}
	size_t len = strlen(buf);
	// we must keep a copy around until zip_close, and we reread the buffer
	// therefore make a copy (NBD) and tell libzip to free (last parm)
	char *buf_copy = strdup(buf);
	zip_source_t *s = zip_source_buffer(state->archive, buf_copy, len, 1);
	if (s == NULL) {
		fprintf(stderr, "zip_source_buffer(%s): %s\n",
			file->filename,
			zip_strerror(state->archive));
		ret = -1;
		goto fail;
	}
	char path[PATH_MAX + 1];
	normalize_path(path, sizeof(path), file->filename);
	zip_int64_t index = zip_file_add(state->archive, path, s, 0);
	if (index == -1) {
		fprintf(stderr, "zip_file_add(%s): %s\n",
			file->filename,
			zip_strerror(state->archive));
		zip_source_free(s);
		ret = -1;
		goto fail;
	}
	char comment[256];
	if (file->record_length == 0) {
		snprintf(comment, 256, "Original STMF CCSID %d", file->ccsid);
	} else {
		snprintf(comment, 256, "Original PF Record Length %d CCSID %d",
			file->record_length,
			file->ccsid);
	}
	// not critical
	zip_file_set_comment(state->archive, index, comment, strlen(comment), 0);
	zip_file_set_mtime(state->archive, index, file->mtime, 0);
fail:
	return ret;
}

int main(int argc, char **argv)
{
	pfgrep state = {0};
	state.pase_ccsid = Qp2paseCCSID();

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
			print_version("pfzip");
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
	if (state.file_count == 0) {
		fprintf(stderr, "%s: need files for archive\n", argv[0]);
		return 5;
	}

	int zerrno;
	state.archive = zip_open(output_file, ZIP_CREATE, &zerrno);
	if (state.archive == NULL) {
		zip_error_t error;
		zip_error_init_with_code(&error, zerrno);
		fprintf(stderr, "zip_open: %s\n",
        		zip_error_strerror(&error));
		zip_error_fini(&error);
		return 6;
	}

	bool any_match = false, any_error = false;
	for (int i = optind; i < argc; i++) {
		int ret = do_thing(&state, argv[i], false);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

	if (zip_close(state.archive) == -1) {
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
