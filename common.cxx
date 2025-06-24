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

#include </QOpenSys/usr/include/iconv.h>

#include "errc.h"
}

#include "common.hxx"

void pfbase::print_version(const char *tool_name)
{
	fprintf(stderr, "%s " PFGREP_VERSION "\n", tool_name);
	fprintf(stderr, "Copyright (c) Seiden Group 2024-2025\n");
	fprintf(stderr, "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n");
	fprintf(stderr, "Written by Calvin Buckley and others, see <https://github.com/SeidenGroup/pfgrep/graphs/contributors>\n");
}

pfbase::pfbase()
{
	this->pase_ccsid = Qp2paseCCSID();
}

bool pfbase::read_records(File *file, iconv_t conv)
{
	size_t read_buf_size = file->file_size + 1;
	if (read_buf_size > this->read_buffer_size) {
		this->read_buffer = (char*)realloc(this->read_buffer, read_buf_size);
		this->read_buffer_size = read_buf_size;
	}
	int bytes_to_read = file->file_size;
	// Read the whole file in
	while ((bytes_to_read = read(file->fd, this->read_buffer, bytes_to_read)) != 0) {
		if (bytes_to_read == -1) {
			if (!this->silent) {
				char msg[256 + PATH_MAX];
				snprintf(msg, sizeof(msg), "read(%s, %d)", file->filename, bytes_to_read);
				perror_xpf(msg);
			}
			return false;
		}
	}
	this->read_buffer[file->file_size] = '\0';

	size_t record_count = file->record_count;
	if (record_count <= 0) {
		record_count = file->file_size / file->record_length;
	}
	// record length * 6 for worst case UTF-8 conv + newline
	size_t conv_buf_size = (file->file_size * UTF8_SCALE_FACTOR) + record_count + 1;
	if (conv_buf_size > this->conv_buffer_size) {
		this->conv_buffer = (char*)realloc(this->conv_buffer, conv_buf_size);
		this->conv_buffer_size = conv_buf_size;
	}
	char *out = this->conv_buffer;
	size_t outleft = conv_buf_size;
	for (size_t record_num = 0; record_num < record_count; record_num++) {
		char *record = this->read_buffer + (record_num * file->record_length);
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
		if (!this->dont_trim_ending_whitespace) {
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

bool pfbase::read_streamfile(File *file, iconv_t conv)
{
	size_t read_buf_size = file->file_size + 1;
	if (read_buf_size > this->read_buffer_size) {
		this->read_buffer = (char*)realloc(this->read_buffer, read_buf_size);
		this->read_buffer_size = read_buf_size;
	}
	int bytes_to_read = file->file_size;
	size_t record_count = file->file_size;
	// Assume max length plus newline character for each line
	size_t conv_buf_size = read_buf_size + record_count;
	if (conv_buf_size > this->conv_buffer_size) {
		this->conv_buffer = (char*)realloc(this->conv_buffer, conv_buf_size);
		this->conv_buffer_size = conv_buf_size;
	}
	// Read the whole file in
	while ((bytes_to_read = read(file->fd, this->read_buffer, bytes_to_read)) != 0) {
		if (bytes_to_read == -1) {
			if (!this->silent) {
				char msg[256 + PATH_MAX];
				snprintf(msg, sizeof(msg), "read(%s, %d)", file->filename, bytes_to_read);
				perror_xpf(msg);
			}
			return false;
		}
	}
	this->read_buffer[file->file_size] = '\0';

	// Skip the copy, we'll just work against the read buffer directly.
	// Save an unnecessary iconv and conversion.
	if (file->ccsid == this->pase_ccsid) {
		this->conv_buffer[0] = '\0';
		return true;
	}

	char *in = this->read_buffer;
	size_t inleft = file->file_size;
	char *out = this->conv_buffer;
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
int pfbase::do_directory(const char *directory)
{
	char msg[PATH_MAX + 256];
	int files_matched = 0;
	DIR *dir = opendir(directory);
	if (dir == NULL) {
		if (!this->silent) {
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
		this->file_count++;

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
		int ret = do_thing(full_path, true);
		if (ret > 0) {
			files_matched += ret;
		}
		errno = 0; // Don't let i.e. iconv errors influence the next call
	}
	if (errno != 0) {
		if (!this->silent) {
			snprintf(msg, sizeof(msg), "reading dirent in %s", directory);
			perror_xpf(msg);
		}
	}
	closedir(dir);
	return files_matched;
}

bool pfbase::set_record_length(File *file)
{
	// Determine the record length, the API to do this needs traditional paths.
	// Note that it will resolve symlinks for us, so i.e. /QIBM/include works
	int ret = filename_to_libobj(file);
	if (ret == -1) {
		if (!this->silent) {
			fprintf(stderr, "filename_to_libobj(%s): Failed to convert IFS path to object name\n",
				file->filename);
		}
		return false;
	}
	int file_record_size = get_pf_info(file);
	if (file_record_size == 0 && errno == ENODEV) {
		// Ignore files we can't support w/ POSIX I/O for now
		return false;
	} else if (file_record_size == 0) {
		if (!this->silent) {
			fprintf(stderr, "get_pf_info(%s): Couldn't get record length\n",
				file->filename);
		}
		return false;
	} else if (file_record_size < 0 && this->search_non_source_files) {
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

int pfbase::do_file(File *file)
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
		if (!this->silent) {
			snprintf(msg, sizeof(msg), "open(%s)", filename);
			perror_xpf(msg);
		}
		return -1;
	}

	// Get member info for an accurate record count
	if (file->record_length != 0) {
		if (!get_member_info(file) && !this->silent) {
			snprintf(msg, sizeof(msg), "get_member_info(%s)", file->filename);
			perror(msg);
		}
	}

	// Open a conversion for this CCSID
	conv = get_iconv(file->ccsid);
	if (conv == (iconv_t)(-1)) {
		if (!this->silent) {
			snprintf(msg, sizeof(msg), "iconv_open(%d, %d)", Qp2paseCCSID(), file->ccsid);
			perror(msg);
		}
		goto fail;
	}

	if (!this->dont_read_file) {
		// Streamfiles are record length 0, and must be read differently
		if (file->record_length == 0) {
			if (!read_streamfile(file, conv)) {
				goto fail;
			}
		} else {
			if (!read_records(file, conv)) {
				goto fail;
			}
		}
	}
	matches = do_action(file);

	if (matches == 0 && this->print_nonmatching_files) {
		printf("%s\n", filename);
	} else if (matches > 0 && this->print_matching_files) {
		printf("%s\n", filename);
	}
	if (this->print_count) {
		printf("%s:%d\n", filename, matches);
	}

fail:
	// shift this should be reset after each file in case of MBCS/DBCS
	if (conv != (iconv_t)(-1)) {
		reset_iconv(conv);
	}

	if (file->fd != -1) {
		close(file->fd);
	}
	return matches;
}

int pfbase::do_thing(const char *filename, bool from_recursion)
{
	char msg[PATH_MAX + 256];
	int matches = 0;
	struct stat64_ILE s = {};
	File f = {};

	f.filename = filename;
	// IBM messed up the statx declaration, it doesn't write
	int ret = statx((char*)filename, (struct stat*)&s, sizeof(s), STX_XPFSS_PASE);
	if (ret == -1) {
		if (!this->silent) {
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
		if (this->recurse) {
			int subdir_files_matched = do_directory(filename);
			if (subdir_files_matched >= 0) {
				matches += subdir_files_matched;
			}
		} else {
			if (!this->silent) {
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
		if (!set_record_length(&f)) {
			return from_recursion ? 0 : -1; // messages emited in function
		}
		matches = do_file(&f);
	} else if (strcmp(s.st_objtype, "*STMF     ") == 0) {
		f.ccsid = s.st_ccsid; // or st_codepage?
		f.record_length = 0;
		matches = do_file(&f);
	}
	// XXX: Message for non-PF/members?
	return matches;
}
