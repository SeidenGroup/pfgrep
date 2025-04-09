/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define PCRE2_CODE_UNIT_WIDTH 8
#include </QOpenSys/usr/include/iconv.h>
#include <pcre2.h>
#include <json_object.h>
#include <json_c_version.h>
#include <zip.h>

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
	/* Archive */
	zip_t *archive;
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
	/* Archive options */
	bool overwrite : 1;
	bool dont_replace_extension : 1;
	/* Stat options */
	bool dont_read_file : 1;
	int max_matches;
	int after_lines;
} pfgrep;

typedef struct pfgrep_file {
	const char *filename; // IFS
	int64_t file_size;
	time_t mtime;
	int fd;
	int32_t record_count;
	int16_t record_length;
	uint16_t ccsid;
	// EBCDIC space-padded + null terminated names for PFs
	char libobj[21]; // object then library, QDBRTVFD needs
	char member[11];
	// Filled in from get_mbr_info
	char source_type[(10 * UTF8_SCALE_FACTOR) + 1];
	char description[(50 * UTF8_SCALE_FACTOR) + 1];
} File;

/* this is per-tool */
int do_action(pfgrep *state, File *file);

/* common.c */
void print_version(const char *tool_name);
void common_init(pfgrep *state);
int do_thing(pfgrep *state, const char *filename, bool from_recursion);

/* conv.c */
iconv_t get_pase_to_system_iconv(void);
iconv_t get_iconv(uint16_t ccsid);
void free_cached_iconv(void);
void reset_iconv(iconv_t conv);

/* convpath.c */
int filename_to_libobj(File *file);

/* rcdfmt.c */
int get_pf_info(File *file);
void free_cached_record_sizes(void);

/* mbrinfo.c */
bool get_member_info(File *file);
