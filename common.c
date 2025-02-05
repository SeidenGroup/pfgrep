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

#include </QOpenSys/usr/include/iconv.h>
#include <linkhash.h>

#include "common.h"
#include "errc.h"

void print_version(const char *tool_name)
{
	fprintf(stderr, "%s " PFGREP_VERSION "\n", tool_name);
	fprintf(stderr, "\tusing json-c %s\n", json_c_version());
	fprintf(stderr, "\tusing libzip %s\n", zip_libzip_version());
	char pcre2_ver[256], pcre2_jit[256];
	uint32_t pcre2_can_jit = 0;
	pcre2_config(PCRE2_CONFIG_JIT, &pcre2_can_jit);
	pcre2_config(PCRE2_CONFIG_VERSION, pcre2_ver);
	fprintf(stderr, "\tusing PCRE2 %s", pcre2_ver);
	if (pcre2_can_jit) {
		pcre2_config(PCRE2_CONFIG_JITTARGET, pcre2_jit);
		fprintf(stderr, " (JIT target: %s)\n", pcre2_jit);
	} else {
		fprintf(stderr, " (no JIT)\n");
	}
	fprintf(stderr, "\nCopyright (c) Seiden Group 2024-2025\n");
	fprintf(stderr, "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n");
	fprintf(stderr, "Written by Calvin Buckley and others, see <https://github.com/SeidenGroup/pfgrep/graphs/contributors>\n");
}

// These contain conversions from convs[N] to system PASE CCSID, memoized to
// avoid constantly reopening iconv for conversion. Gets closed on exit.
// Because we only convert to a single CCSID, we can keep the maping flat.
// The memory cost of this should be minimal on modern systems.
iconv_t convs[UINT16_MAX] = {0};

static iconv_t get_iconv(uint16_t ccsid)
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
}

static bool read_records(pfgrep *state, File *file, iconv_t conv)
{
	size_t read_buf_size = file->file_size + 1;
	if (read_buf_size > state->read_buffer_size) {
		state->read_buffer = realloc(state->read_buffer, read_buf_size);
		state->read_buffer_size = read_buf_size;
	}
	int bytes_to_read = file->file_size;
	// Read the whole file in
	while ((bytes_to_read = read(file->fd, state->read_buffer, bytes_to_read)) != 0) {
		if (bytes_to_read == -1) {
			if (!state->silent) {
				char msg[256 + PATH_MAX];
				snprintf(msg, sizeof(msg), "read(%s, %d)", file->filename, bytes_to_read);
				perror_xpf(msg);
			}
			return false;
		}
	}
	state->read_buffer[file->file_size] = '\0';

	size_t record_count = file->file_size / file->record_length;
	// record length * 6 for worst case UTF-8 conv + newline
	size_t conv_buf_size = (file->file_size * UTF8_SCALE_FACTOR) + record_count + 1;
	if (conv_buf_size > state->conv_buffer_size) {
		state->conv_buffer = realloc(state->conv_buffer, conv_buf_size);
		state->conv_buffer_size = conv_buf_size;
	}
	char *out = state->conv_buffer;
	size_t outleft = conv_buf_size;
	for (size_t record_num = 0; record_num < record_count; record_num++) {
		char *record = state->read_buffer + (record_num * file->record_length);
		// Converted record is on a 6x multiplier due to possible
		// worst case EBCDIC->UTF-8 conversion
		char *in = record;
		char *beginning = out;
		size_t inleft = file->record_length;
		int rc = iconv(conv, &in, &inleft, &out, &outleft);
		if (rc != 0) {
			perror("iconv");
			return false;
		}
		// Trim buffer to end of iconv plus trim spaces,
		// as SRCPFs are fixed length and space padded,
		// so $ works like expected
		if (!state->dont_trim_ending_whitespace) {
			while (out >= beginning && *(out - 1) == ' ') {
				*out-- = '\0';
				outleft++;
			}
		}
		*out++ = '\n';
		outleft--;
	}
	*out++ = '\0';
	outleft--;

	return true;
}

static bool read_streamfile(pfgrep *state, File *file, iconv_t conv)
{
	size_t read_buf_size = file->file_size + 1;
	if (read_buf_size > state->read_buffer_size) {
		state->read_buffer = realloc(state->read_buffer, read_buf_size);
		state->read_buffer_size = read_buf_size;
	}
	int bytes_to_read = file->file_size;
	size_t record_count = file->file_size;
	// Assume max length plus newline character for each line
	size_t conv_buf_size = read_buf_size + record_count;
	if (conv_buf_size > state->conv_buffer_size) {
		state->conv_buffer = realloc(state->conv_buffer, conv_buf_size);
		state->conv_buffer_size = conv_buf_size;
	}
	// Read the whole file in
	while ((bytes_to_read = read(file->fd, state->read_buffer, bytes_to_read)) != 0) {
		if (bytes_to_read == -1) {
			if (!state->silent) {
				char msg[256 + PATH_MAX];
				snprintf(msg, sizeof(msg), "read(%s, %d)", file->filename, bytes_to_read);
				perror_xpf(msg);
			}
			return false;
		}
	}
	state->read_buffer[file->file_size] = '\0';

	// Skip the copy, we'll just work against the read buffer directly.
	// Save an unnecessary iconv and conversion.
	if (file->ccsid == state->pase_ccsid) {
		state->conv_buffer[0] = '\0';
		return true;
	}

	char *in = state->read_buffer;
	size_t inleft = file->file_size;
	char *out = state->conv_buffer;
	size_t outleft = conv_buf_size;
	int rc = iconv(conv, &in, &inleft, &out, &outleft);
	if (rc != 0) {
		perror("iconv");
		return false;
	}
	*out++ = '\0';
	outleft--;

	return true;
}

/**
 * Recurse through a directory or physical file.
 */
static int do_directory(pfgrep *state, const char *directory)
{
	char msg[PATH_MAX + 256];
	int files_matched = 0;
	DIR *dir = opendir(directory);
	if (dir == NULL) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "opendir(%s)", directory);
			perror_xpf(msg);
		}
		return -1;
	}
	struct dirent *dirent = NULL;
	while ((dirent = readdir(dir)) != NULL) {
		if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0) {
			continue;
		}

		// Raise the file count in case single dir/PF passed,
		// so filenames of subdirectories are printed
		state->file_count++;

		// XXX: Technically on i, it might be faster to chdir rather
		// than use a full path, since resolution is faster from CWD
		char full_path[PATH_MAX + 1];
		// Avoid doubling the / if user has a trailing one passed
		// XXX: Should normalize these before the QSYS name conversion
		if (directory[strlen(directory) - 1] == '/' ) {
			snprintf(full_path, sizeof(full_path), "%s%s", directory, dirent->d_name);
		} else {
			snprintf(full_path, sizeof(full_path), "%s/%s", directory, dirent->d_name);
		}
		int ret = do_thing(state, full_path, true);
		if (ret > 0) {
			files_matched += ret;
		}
		errno = 0; // Don't let i.e. iconv errors influence the next call
	}
	if (errno != 0) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "reading dirent in %s", directory);
			perror_xpf(msg);
		}
	}
	closedir(dir);
	return files_matched;
}

static bool set_record_length(pfgrep *state, File *file)
{
	// Determine the record length, the API to do this needs traditional paths.
	// Note that it will resolve symlinks for us, so i.e. /QIBM/include works
	char lib_name[29], obj_name[29], mbr_name[29];
	const char *filename = file->filename;
	int ret = filename_to_libobj(file->filename, lib_name, obj_name, mbr_name);
	if (ret == -1) {
		if (!state->silent) {
			fprintf(stderr, "filename_to_libobj(%s): Failed to convert IFS path to object name\n", filename);
		}
		return false;
	}
	int file_record_size = get_pf_info(lib_name, obj_name);
	if (file_record_size == 0 && errno == ENODEV) {
		// Ignore files we can't support w/ POSIX I/O for now
		return false;
	} else if (file_record_size == 0) {
		if (!state->silent) {
			fprintf(stderr, "get_pf_info(%s): Couldn't get record length\n", filename);
		}
		return false;
	} else if (file_record_size < 0 && state->search_non_source_files) {
		// Non-source PF, signedness is used as source PF bit
		file->record_length = -file_record_size;
		return true;
	} else {
		// Source PF, length includes other metadata not pulled when
		// reading source PFs via POSIX APIs
		file->record_length = file_record_size - 12;
		return true;
	}
	return false;
}

static int do_file(pfgrep *state, File *file)
{
	char msg[PATH_MAX + 256];
	int matches = -1;
	iconv_t conv = (iconv_t)(-1);
	const char *filename = file->filename;

	// Only open after we know it's a valid thing to open.
	file->fd = open(file->filename, O_RDONLY);
	// We let do_file fill in the filename and CCSID. Technically a TOCTOU
	// problem, but open(2) error reporting with IBM i objects is goofy.
	if (file->fd == -1) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "open(%s)", filename);
			perror_xpf(msg);
		}
		return -1;
	}

	// Open a conversion for this CCSID
	conv = get_iconv(file->ccsid);
	if (conv == (iconv_t)(-1)) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "iconv_open(%d, %d)", Qp2paseCCSID(), file->ccsid);
			perror(msg);
		}
		goto fail;
	}

	// Streamfiles are record length 0, and must be read differently
	if (file->record_length == 0) {
		if (!read_streamfile(state, file, conv)) {
			goto fail;
		}
	} else {
		if (!read_records(state, file, conv)) {
			goto fail;
		}
	}
	matches = do_action(state, file);

	if (matches == 0 && state->print_nonmatching_files) {
		printf("%s\n", filename);
	} else if (matches > 0 && state->print_matching_files) {
		printf("%s\n", filename);
	}
	if (state->print_count) {
		printf("%s:%d\n", filename, matches);
	}

fail:
	// shift state should be reset after each file in case of MBCS/DBCS
	if (conv != (iconv_t)(-1)) {
		iconv(conv, NULL, NULL, NULL, NULL);
	}

	if (file->fd != -1) {
		close(file->fd);
	}
	return matches;
}

int do_thing(pfgrep *state, const char *filename, bool from_recursion)
{
	char msg[PATH_MAX + 256];
	int matches = 0;
	struct stat64_ILE s = {0};
	File f;

	f.filename = filename;
	// IBM messed up the statx declaration, it doesn't write
	int ret = statx((char*)filename, (struct stat*)&s, sizeof(s), STX_XPFSS_PASE);
	if (ret == -1) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "stat(%s)", filename);
			perror_xpf(msg);
		}
		return -1;
	}
	f.file_size = s.st_size;
	// XXX: This is 32-bit with ILE mtime
	f.mtime = s.st_mtime;
	// objtype is *FILE or *DIR, check for mode though to avoid i.e. SAVFs
	if (S_ISDIR(s.st_mode)) {
		if (state->recurse) {
			int subdir_files_matched = do_directory(state, filename);
			if (subdir_files_matched >= 0) {
				matches += subdir_files_matched;
			}
		} else {
			if (!state->silent) {
				fprintf(stderr, "stat(%s): Is a directory or physical file\n", filename);
			}
			return -1;
		}
	} else if (s.st_size == 0) {
		// This is either a logical file or such (we can't open these
		// yet), or a supported empty file that would have no matches.
		// Avoid bothering the user (per GH-3)
		return 0;
	} else if (strcmp(s.st_objtype, "*MBR      ") == 0) {
		f.ccsid = s.st_ccsid; // or st_codepage?
		if (!set_record_length(state, &f)) {
			return from_recursion ? 0 : -1; // messages emited in function
		}
		matches = do_file(state, &f);
	} else if (strcmp(s.st_objtype, "*STMF     ") == 0) {
		f.ccsid = s.st_ccsid; // or st_codepage?
		f.record_length = 0;
		matches = do_file(state, &f);
	}
	// XXX: Message for non-PF/members?
	return matches;
}
