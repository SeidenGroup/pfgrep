/*
 * Copyright (c) 2024 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <as400_protos.h>
#include <as400_types.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include </QOpenSys/usr/include/iconv.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "errc.h"

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
} pfgrep;

int filename_to_libobj(const char *input, char *lib_name, char *obj_name, char *mbr_name);
int get_record_size(const char *lib_name, const char *obj_name);

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-cHhiLlnqstv] EXPR files...\n", argv0);
}

uint32_t get_compile_flags(pfgrep *state)
{
	uint32_t flags = 0;
	if (state->case_insensitive) {
		flags |= PCRE2_CASELESS;
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
					printf("%s: ", filename);
				}
				if (state->print_line_numbers) {
					printf("%d: ", line);
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

static int do_file(pfgrep *state, const char *filename)
{
	char msg[PATH_MAX + 256];
	int matches = -1;
	iconv_t conv = (iconv_t)(-1);
	int fd = open(filename, O_RDONLY);
	// It may be better to stat the file w/ path before opening. It opens
	// the possiblity of TOCTOU issues, but it'd give us better error
	// reporting since open fails before we can fstat it.
	if (fd == -1 && errno == EOPNOTSUPP) {
		if (!state->silent) {
			fprintf(stderr, "opening %s: Not a normal physical file\n", filename);
		}
		return -1;
	} else if (fd == -1) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "opening %s", filename);
			perror_xpf(msg);
		}
		return -1;
	}

	// Determine the CCSID and type of the file
	struct stat64_ILE s = {0};
	int ret = fstatx(fd, (struct stat*)&s, sizeof(s), STX_XPFSS_PASE);
	if (ret == -1) {
		if (!state->silent) {
			snprintf(msg, sizeof(msg), "getting CCSID of %s", filename);
			perror_xpf(msg);
		}
		goto fail;
	}
	if (strcmp(s.st_objtype, "*MBR      ") != 0) {
		if (!state->silent) {
			fprintf(stderr, "opening %s: Not a normal physical file\n", filename);
		}
		goto fail;
	}
	int ccsid = s.st_ccsid; // or st_codepage?

	// Determine the record length, the API to do this needs traditional paths.
	// Note that it will resolve symlinks for us, so i.e. /QIBM/include works
	char lib_name[29], obj_name[29], mbr_name[29];
	filename_to_libobj(filename, lib_name, obj_name, mbr_name);
	int file_record_size = get_record_size(lib_name, obj_name);
	if (file_record_size == -2) {
		if (!state->silent) {
			fprintf(stderr, "opening %s: Not a source physical file\n", filename);
		}
		goto fail;
	} else if (file_record_size == -1) {
		if (!state->silent) {
			fprintf(stderr, "opening %s: Couldn't get record length\n", filename);
		}
		goto fail;
	}
	// Record size includes other metadata not pulled in on read
	file_record_size -= 12;

	// Open a conversion for this CCSID (XXX: Should cache)
	conv = iconv_open(ccsidtocs(Qp2paseCCSID()), ccsidtocs(ccsid));
	if (conv == (iconv_t)(-1)) {
		if (!state->silent) {
			perror("iconv");
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
	if (conv != (iconv_t)(-1)) {
		iconv_close(conv);
	}
	close(fd);
	return matches;
}

int main(int argc, char **argv)
{
	pfgrep state = {0};

	int ch;
	while ((ch = getopt(argc, argv, "cHhLlinqstv")) != -1) {
		switch (ch) {
		case 'c':
			state.print_count = true;
			state.quiet = true; // Implied
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
		case 's':
			state.silent = true;
			break;
		case 't':
			state.trim_ending_whitespace = true;
			break;
		case 'v':
			state.invert = true;
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
	uint32_t pcre_opts = get_compile_flags(&state);
	state.re = pcre2_compile((PCRE2_SPTR)state.expr, PCRE2_ZERO_TERMINATED, pcre_opts, &errornumber, &erroroffset, NULL);
	if (state.re == NULL) {
		PCRE2_UCHAR buffer[256];
		pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
		fprintf(stderr, "Failed to compile regular expression at offset %d: %s\n", (int)erroroffset, buffer);
		return 4;
	}

	state.file_count = argc - optind;
	bool any_match = false, any_error = false;
	for (int i = optind; i < argc; i++) {
		int ret = do_file(&state, argv[i]);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

	pcre2_code_free(state.re);
	return any_error ? 2 : (any_match ? 0 : 1);
}
