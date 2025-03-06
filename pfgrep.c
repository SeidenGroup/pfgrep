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
#include <string.h>
#include <sys/errno.h>
#include <sys/mode.h>
#include <sys/stat.h>
#include <unistd.h>

#include </QOpenSys/usr/include/iconv.h>
#include <linkhash.h>

#include "common.h"
#include "errc.h"

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-A num] [-cFHhiLlnpqrstwVvx] pattern files...\n", argv0);
	fprintf(stderr, "usage: %s [-A num] [-cFHhiLlnpqrstwVvx] [-e pattern] [-f file] files...\n", argv0);
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

static bool print_line(pfgrep *state, File *file, const char *line, size_t line_size, int lineno)
{
	if (state->quiet && !state->print_count) {
		// Special case: Early return since we don't
		// to count or print more lines
		return false;
	} else if (!state->quiet) {
		if ((state->file_count > 1 && !state->never_print_filename) || state->always_print_filename) {
			printf("%s:", file->filename);
		}
		if (state->print_line_numbers) {
			printf("%d:", lineno);
		}
		fwrite(line, line_size, 1, stdout);
		putchar('\n');
	}
	return true;
}

int do_action(pfgrep *state, File *file)
{
	int matches = 0, rc = 0;
	uint32_t offset = 0, flags = 0;
	// We can have multiple expressions. Find the first match.
	size_t pattern_count = json_object_array_length(state->patterns);
	int lineno = 0;
	int current_after_lines = 0;
	char *line = state->conv_buffer, *next = NULL;
	// If same CCSID, use read buffer, otherwise if EBCDIC/diff ASCII, conv
	if (file->ccsid == state->pase_ccsid) {
		line = state->read_buffer;
	}
	while (line && *line) {
		bool matched = false;
		lineno++;
		// Handle CRLF newlines (could be better)
		size_t conv_size = 0;
		next = strpbrk(line, "\r\n");
		if (next) {
			conv_size = (size_t)(next - line);
			if (next[0] == '\r') {
				next++;
			}
			next++;
		} else {
			conv_size = strlen(line);
		}

		for (size_t i = 0; i < pattern_count; i++) {
			json_object *jso = json_object_array_get_idx(state->patterns, i);
			pcre2_code *re = (pcre2_code*)json_object_get_userdata(jso);

			// As long as we checked that the pattern successfully was JIT
			// compiled, it should be safe to use pcre2_jit_match instead.
			if (state->can_jit) {
				rc = pcre2_jit_match(re, (PCRE2_SPTR)line, conv_size, offset, flags, state->match_data, NULL);
			} else {
				rc = pcre2_match(re, (PCRE2_SPTR)line, conv_size, offset, flags, state->match_data, NULL);
			}

			if (rc > 0) {
				matched = true;
				break;
			} else if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) {
				// handle error after the loop
				break;
			}
		}

		if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) {
			if (!state->silent) {
				PCRE2_UCHAR buffer[256];
				pcre2_get_error_message(rc, buffer, sizeof(buffer));
				fprintf(stderr, "failed match error: %s (%d)\n", buffer, rc);
			}
		} else if ((matched && !state->invert) || (!matched && state->invert)) {
			matches++;
			current_after_lines = state->after_lines;
			if (!print_line(state, file, line, conv_size, lineno)) {
				goto fail;
			}
		} else if (current_after_lines-- > 0) {
			print_line(state, file, line, conv_size, lineno);
		}

		line = next;
	}
fail:
	return matches;
}

static void cleanup_pattern(json_object *jso, void *userdata)
{
	(void)jso;
	pcre2_code *re = (pcre2_code*)userdata;
	pcre2_code_free(re);
}

static bool add_pattern(pfgrep *state, const char *expr)
{
	int errornumber;
	PCRE2_SIZE erroroffset;
	pcre2_compile_context *compile_ctx = pcre2_compile_context_create(NULL);
	pcre2_set_compile_extra_options(compile_ctx, get_extra_compile_flags(state));
	pcre2_code *re = pcre2_compile((PCRE2_SPTR)expr,
			PCRE2_ZERO_TERMINATED,
			get_compile_flags(state),
			&errornumber,
			&erroroffset,
			compile_ctx);
	if (re == NULL) {
		PCRE2_UCHAR buffer[256];
		pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
		fprintf(stderr, "Failed to compile regular expression \"%s\" at offset %d: %s\n",
				expr,
				(int)erroroffset,
				buffer);
		return false;
	}

	// Allocate the largest required match_data later.
	uint32_t capture_count = 0;
	pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);
	if (capture_count > state->biggest_capture_count) {
		state->biggest_capture_count = capture_count;
	}

	// We should probably be seeing if the JIT status is usable per-expr...
	if (state->can_jit) {
		size_t jit_size = 0;
		int jit_ret = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
		if (jit_ret == 0) {
			jit_ret = pcre2_pattern_info(re, PCRE2_INFO_JITSIZE, &jit_size);
			if (jit_ret != 0 || jit_size == 0) {
				state->can_jit = false;
			}
		}
	}

	json_object *obj = json_object_new_string(expr);
	json_object_set_userdata(obj, (void*)re, cleanup_pattern);
	json_object_array_add(state->patterns, obj);
	return true;
}

static bool add_patterns_from_file(pfgrep *state, const char *path)
{
	FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
	if (f == NULL) {
		perror("can't open pattern file");
		return false;
	}
	char *line = NULL;
	size_t line_limit = 0;
	ssize_t line_len;
	bool ret = true;
	while ((line_len = getline(&line, &line_limit, f)) > 0) {
		// getline adds the ending newline, we don't want it
		if (line_len > 0 && line[line_len - 1] == '\n') {
			line[line_len - 1] = '\0';
		}
		if (line_len > 0 && !add_pattern(state, line)) {
			ret = false;
			break;
		}
		// getline allocated, we duped in array
		free(line);
		line = NULL;
		line_limit = 0;
	}
	if (f != stdin) {
		fclose(f);
	}
	return ret;
}

int main(int argc, char **argv)
{
	pfgrep state = {0};
	common_init(&state);
	state.patterns = json_object_new_array();

	// TODO: Decide to warn the user if JIT is disabled, or if JIT is on but
	// the expression couldn't be compiled. For now, silently ignore errors.
	uint32_t can_jit = 0;
	pcre2_config(PCRE2_CONFIG_JIT, &can_jit);
	state.can_jit = can_jit;

	int ch;
	while ((ch = getopt(argc, argv, "A:ce:Ff:HhLlinpqrstwVvx")) != -1) {
		switch (ch) {
		case 'A':
			state.after_lines = atoi(optarg);
			break;
		case 'c':
			state.print_count = true;
			state.quiet = true; // Implied
			break;
		case 'e':
			if (!add_pattern(&state, optarg)) {
				return 4;
			}
			break;
		case 'F':
			state.fixed = true;
			break;
		case 'f':
			if (!add_patterns_from_file(&state, optarg)) {
				return 4;
			}
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
		case 'p':
			state.search_non_source_files = true;
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
			state.dont_trim_ending_whitespace = true;
			break;
		case 'w':
			state.match_word = true;
			break;
		case 'V':
			print_version("pfgrep");
			return 0;
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

	// If -e nor -f were used, expect expr as first arg
	bool need_pattern_arg = json_object_array_length(state.patterns) == 0;

	// We take physical files, no stdin, so we need expr + files
	if (need_pattern_arg && optind + 1 >= argc) {
		usage(argv[0]);
		return 3;
	}

	if (need_pattern_arg) {
		char *expr = strdup(argv[optind++]);
		if (!add_pattern(&state, expr)) {
			return 4;
		}
	}

	// One big match data that can handle all possible;
	// uses capture count + 1 like pcre2_match_data_create_from_pattern
	state.match_data = pcre2_match_data_create(state.biggest_capture_count + 1, NULL);
	if (state.match_data == NULL) {
		if (!state.silent) {
			fprintf(stderr, "failed match error: Couldn't allocate memory for match data\n");
		}
		return 6;
	}

	state.file_count = argc - optind;
	bool any_match = false, any_error = false;
	for (int i = optind; i < argc; i++) {
		int ret = do_thing(&state, argv[i], false);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

#ifdef DEBUG
	// This deinitialization may be unnecessary, do it for future use of
	// sanitizers/*grind when available on i
	free_cached_iconv();
	pcre2_match_data_free(state.match_data);
	json_object_put(state.patterns);
	free_cached_record_sizes();
	free(state.read_buffer);
	free(state.conv_buffer);
#endif

	return any_error ? 2 : (any_match ? 0 : 1);
}
