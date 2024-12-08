/*
 * Copyright (c) 2024 Seiden Group
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
#include <string.h>
#include <sys/errno.h>
#include <sys/mode.h>
#include <sys/stat.h>
#include <unistd.h>

#include </QOpenSys/usr/include/iconv.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <linkhash.h>

#include "errc.h"

// These contain conversions from convs[N] to system PASE CCSID, memoized to
// avoid constantly reopening iconv for conversion. Gets closed on exit.
// Because we only convert to a single CCSID, we can keep the maping flat.
// The memory cost of this should be minimal on modern systems.
iconv_t convs[UINT16_MAX] = {0};

typedef struct pfgrep_state {
	/* Files */
	int file_count;
	/* Pattern */
	const char *expr;
	pcre2_code *re;
	/* Options */
	bool trim_ending_whitespace : 1;
	bool case_insensitive : 1;
	bool always_print_filename : 1;
	bool never_print_filename : 1;
	bool print_line_numbers : 1;
	bool invert : 1;
	// Note quiet does not imply silent et vice versa
	bool quiet : 1; // No output on successful match
	bool silent : 1; // No output on errors
	bool print_matching_files : 1;
	bool print_nonmatching_files : 1;
	bool print_count : 1;
	bool recurse : 1;
	bool match_word : 1;
	bool match_line : 1;
	bool fixed : 1;
} pfgrep;

typedef struct pfgrep_file {
	const char *filename; // IFS
	int fd;
	int record_length;
	uint16_t ccsid;
} File;

int filename_to_libobj(const char *input, char *lib_name, char *obj_name, char *mbr_name);
int get_pf_info(const char *lib_name, const char *obj_name);
static int do_thing(pfgrep *state, const char *filename);
void free_cached_record_sizes(void);

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-cFHhiLlnqrstwvx] EXPR files...\n", argv0);
}

static iconv_t get_iconv(uint16_t ccsid)
{
	iconv_t conv = convs[ccsid];
	if (conv == NULL || conv == (iconv_t)(-1)) {
		conv = iconv_open(ccsidtocs(Qp2paseCCSID()), ccsidtocs(ccsid));
		convs[ccsid] = conv;
	}
	return conv;
}

static uint32_t get_compile_flags(pfgrep *state)
{
	uint32_t flags = 0;
	if (state->case_insensitive) {
		flags |= PCRE2_CASELESS;
	}
	// XXX: We might consider using i.e. str(case)str instead, since it's
	// possibly faster than using PCRE, but it does work w/ the other PCRE
	// flags like matching words/lines, so...
	if (state->fixed) {
		flags |= PCRE2_LITERAL;
	}
	return flags;
}

static uint32_t get_extra_compile_flags(pfgrep *state)
{
	uint32_t flags = 0;
	if (state->match_word) {
		flags |= PCRE2_EXTRA_MATCH_WORD;
	}
	if (state->match_line) {
		flags |= PCRE2_EXTRA_MATCH_LINE;
	}
	return flags;
}

static int iter_records(pfgrep *state, const char *filename, int fd, iconv_t conv, int record_length)
{
	char *read_buf = malloc(record_length + 1);
	size_t conv_buf_size = (record_length * 6) + 1;
	char *conv_buf = malloc(conv_buf_size);
	int bytes_read;
	// XXX: This could use the sequence and date numbers in the PF
	int matches = 0;
	int line = 0;
	while ((bytes_read = read(fd, read_buf, record_length)) != 0) {
		if (bytes_read == -1) {
			if (!state->silent) {
				char msg[256 + PATH_MAX];
				snprintf(msg, sizeof(msg), "read(%s, %d)", filename, record_length);
				perror_xpf(msg);
			}
			matches = -1;
			goto fail;
		}
		read_buf[record_length] = '\0';
		// XXX: Check if bytes_read < record_length
		char *in = read_buf, *out = conv_buf;
		size_t inleft = record_length, outleft = conv_buf_size;
		int rc = iconv(conv, &in, &inleft, &out, &outleft);
		if (rc != 0) {
			perror("iconv");
			matches = -1;
			goto fail;
		}
		line++;
		// Trim buffer to end of iconv plus trim spaces,
		// as SRCPFs are fixed length and space padded,
		// so $ works like expected
		ssize_t conv_size = conv_buf_size - outleft;
		conv_buf[conv_size] = '\0';
		if (state->trim_ending_whitespace) {
			while (conv_size >= 0 && conv_buf[conv_size] == ' ') {
				conv_size--;
			}
			conv_buf[conv_size + 1] = '\0';
			if (conv_size == 0) {
				continue;
			}
		}
		// Now actually match...
		uint32_t offset = 0, flags = 0;
		pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(state->re, NULL);
		if (match_data == NULL) {
			if (!state->silent) {
				fprintf(stderr, "failed match error: Couldn't allocate memory for match data\n");
			}
			matches = -1;
			goto fail;
		}
		rc = pcre2_match(state->re, (PCRE2_SPTR)conv_buf, conv_size, offset, flags, match_data, NULL);
		if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) {
			if (!state->silent) {
				PCRE2_UCHAR buffer[256];
				pcre2_get_error_message(rc, buffer, sizeof(buffer));
				fprintf(stderr, "failed match error: %s (%d)\n", buffer, rc);
			}
		} else if ((rc != PCRE2_ERROR_NOMATCH && !state->invert) || (rc == PCRE2_ERROR_NOMATCH && state->invert)) {
			matches++;
			if (state->quiet && !state->print_count) {
				// Special case: Early return since we don't
				// to count or print more lines
				pcre2_match_data_free(match_data);
				goto fail;
			} else if (!state->quiet) {
				if ((state->file_count > 1 && !state->never_print_filename) || state->always_print_filename) {
					printf("%s:", filename);
				}
				if (state->print_line_numbers) {
					printf("%d:", line);
				}
				printf("%s\n", conv_buf);
			}
		}
		pcre2_match_data_free(match_data);
	}
fail:
	free(conv_buf);
	free(read_buf);
	return matches;
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
		do_thing(state, full_path);
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

static int do_file(pfgrep *state, File *file)
{
	char msg[PATH_MAX + 256];
	int matches = -1;
	iconv_t conv = (iconv_t)(-1);
	const char *filename = file->filename;
	int fd = -1;

	// Determine the record length, the API to do this needs traditional paths.
	// Note that it will resolve symlinks for us, so i.e. /QIBM/include works
	char lib_name[29], obj_name[29], mbr_name[29];
	int ret = filename_to_libobj(filename, lib_name, obj_name, mbr_name);
	if (ret == -1) {
		if (!state->silent) {
			fprintf(stderr, "filename_to_libobj(%s): Failed to convert IFS path to object name\n", filename);
		}
		goto fail;
	}
	int file_record_size = get_pf_info(lib_name, obj_name);
	if (file_record_size == 0 && errno == ENODEV) {
		if (!state->silent) {
			fprintf(stderr, "get_pf_info(%s): Not a physical file member\n", filename);
		}
		goto fail;
	} else if (file_record_size == 0) {
		if (!state->silent) {
			fprintf(stderr, "get_pf_info(%s): Couldn't get record length\n", filename);
		}
		goto fail;
	} else if (file_record_size < 0) {
		// Non-source PF, signedness is used as source PF bit
		file_record_size = -file_record_size;
	} else {
		// Source PF, length includes other metadata not pulled when
		// reading source PFs via POSIX APIs
		file_record_size -= 12;
	}

	// Only open after we know it's a valid thing to open.
	fd = open(file->filename, O_RDONLY);
	// We let do_file fill in the filename and CCSID. Technically a TOCTOU
	// problem, but open(2) error reporting with IBM i objects is goofy.
	if (fd == -1) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "open(%s)", filename);
			perror_xpf(msg);
		}
		return -1;
	}

	// Open a conversion for this CCSID (XXX: Should cache)
	conv = get_iconv(file->ccsid);
	if (conv == (iconv_t)(-1)) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "iconv_open(%d, %d)", Qp2paseCCSID(), file->ccsid);
			perror(msg);
		}
		goto fail;
	}

	matches = iter_records(state, filename, fd, conv, file_record_size);
	if (matches == 0 && state->print_nonmatching_files) {
		printf("%s\n", filename);
	} else if (matches > 0 && state->print_matching_files) {
		printf("%s\n", filename);
	}
	if (state->print_count) {
		printf("%s:%d\n", filename, matches);
	}
fail:
	if (fd != -1) {
		close(fd);
	}
	return matches;
}

static int do_thing(pfgrep *state, const char *filename)
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
		// Avoid bothering the user (per GH-1)
		return 0;
	} else if (strcmp(s.st_objtype, "*MBR      ") == 0) {
		f.ccsid = s.st_ccsid; // or st_codepage?
		return do_file(state, &f);
	}
	// XXX: Message for non-PF/members?
	return matches;
}

int main(int argc, char **argv)
{
	pfgrep state = {0};

	// The default hashing algorithm linkhash uses is fine, but since we
	// deal with 20 character strings with few allowed characters, it
	// should be safe to use the simpler "Perl-like" hash, which is a bit
	// faster than the default.
	json_global_set_string_hash(JSON_C_STR_HASH_PERLLIKE);

	int ch;
	while ((ch = getopt(argc, argv, "cFHhLlinqrstwvx")) != -1) {
		switch (ch) {
		case 'c':
			state.print_count = true;
			state.quiet = true; // Implied
			break;
		case 'F':
			state.fixed = true;
			break;
		case 'H':
			state.always_print_filename = true;
			break;
		case 'h':
			state.never_print_filename = true;
			break;
		case 'L':
			state.print_matching_files = false;
			state.print_nonmatching_files = true;
			state.quiet = true; // Implied
			break;
		case 'l':
			state.print_matching_files = true;
			state.print_nonmatching_files = false;
			state.quiet = true; // Implied
			break;
		case 'i':
			state.case_insensitive = true;
			break;
		case 'n':
			state.print_line_numbers = true;
			break;
		case 'q':
			state.quiet = true;
			break;
		case 'r':
			state.recurse = true;
			break;
		case 's':
			state.silent = true;
			break;
		case 't':
			state.trim_ending_whitespace = true;
			break;
		case 'w':
			state.match_word = true;
			break;
		case 'v':
			state.invert = true;
			break;
		case 'x':
			state.match_line = true;
			break;
		default:
			usage(argv[0]);
			return 3;
		}
	}

	// We take physical files, no stdin, so we need expr + files
	if (optind + 1 >= argc) {
		usage(argv[0]);
		return 3;
	}

	state.expr = argv[optind++];
	int errornumber;
	PCRE2_SIZE erroroffset;
	pcre2_compile_context *compile_ctx = pcre2_compile_context_create(NULL);
	pcre2_set_compile_extra_options(compile_ctx, get_extra_compile_flags(&state));
	state.re = pcre2_compile((PCRE2_SPTR)state.expr,
			PCRE2_ZERO_TERMINATED,
			get_compile_flags(&state),
			&errornumber,
			&erroroffset,
			compile_ctx);
	if (state.re == NULL) {
		PCRE2_UCHAR buffer[256];
		pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
		fprintf(stderr, "Failed to compile regular expression at offset %d: %s\n", (int)erroroffset, buffer);
		return 4;
	}

	state.file_count = argc - optind;
	bool any_match = false, any_error = false;
	for (int i = optind; i < argc; i++) {
		int ret = do_thing(&state, argv[i]);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

	// This deinitialization may be unnecessary, do it for future use of
	// sanitizers/*grind when available on i
	pcre2_code_free(state.re);
	for (int i = 0; i < UINT16_MAX; i++) {
		iconv_t conv = convs[i];
		if (conv == NULL || conv == (iconv_t)(-1)) {
			continue;
		}
		iconv_close(conv);
	}
	free_cached_record_sizes();

	return any_error ? 2 : (any_match ? 0 : 1);
}
