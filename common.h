/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <json_c_version.h>

// In the worst case, a single byte character can become six bytes in UTF-8.
#define UTF8_SCALE_FACTOR 6

typedef struct pfgrep_state {
	/* Cached system info */
	int pase_ccsid;
	/* Files */
	int file_count;
	/* Buffers */
	char *read_buffer;
	size_t read_buffer_size;
	char *conv_buffer;
	size_t conv_buffer_size;
	/* Pattern */
	json_object *patterns; // array; elements are string, userdata is pcre2_code*
	pcre2_match_data *match_data;
	uint32_t biggest_capture_count;
	bool can_jit : 1;
	/* Options */
	bool search_non_source_files : 1;
	bool dont_trim_ending_whitespace : 1;
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
	int64_t file_size;
	int fd;
	int record_length;
	uint16_t ccsid;
} File;

/* this is per-tool */
int do_action(pfgrep *state, File *file);

/* common.c */
void print_version(const char *tool_name);
int do_thing(pfgrep *state, const char *filename, bool from_recursion);
void free_cached_iconv(void);

/* convpath.c */
int filename_to_libobj(const char *input, char *lib_name, char *obj_name, char *mbr_name);

/* rcdfmt.c */
int get_pf_info(const char *lib_name, const char *obj_name);
void free_cached_record_sizes(void);
