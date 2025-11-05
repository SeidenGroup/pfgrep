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
#include <string.h>
#include <sys/errno.h>
#include <sys/mode.h>
#include <sys/stat.h>
#include <unistd.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include </QOpenSys/usr/include/iconv.h>
#include <pcre2.h>

#include "errc.h"
}

#include <deque>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "common.hxx"

class Pattern {
public:
	Pattern(const char *pattern, pcre2_code *re) {
		this->pattern = std::string(pattern);
		this->re = re;
	}

	~Pattern() {
		pcre2_code_free(re);
	}

	std::string pattern;
	pcre2_code *re;
};

class pfgrep : public pfbase {
public:
	pfgrep() = default;
	// disable copy constructor for unique_ptr
	pfgrep(const pfgrep&) = delete;
	pfgrep& operator=(const pfgrep&) = delete;

	~pfgrep();
	int do_action(File *file) override;
	void print_version(const char *tool_name);
	bool add_pattern(const char *expr);
	bool add_patterns_from_file(const char *path);

	/* Pattern */
	std::vector<std::unique_ptr<Pattern>> patterns;
	pcre2_match_data *match_data = nullptr;
	uint32_t biggest_capture_count = 0;
	bool can_jit = false;
	/* Options */
	bool case_insensitive = false;
	bool always_print_filename = false;
	bool never_print_filename = false;
	bool print_line_numbers = false;
	bool invert = false;
	bool match_word = false;
	bool match_line = false;
	bool fixed = false;
	int max_matches = 0;
	int after_lines = 0;
	unsigned int before_lines = 0;

private:
	uint32_t get_compile_flags();
	uint32_t get_extra_compile_flags();
	bool print_line(File *file, const char *line, size_t line_size, int lineno);
};

pfgrep::~pfgrep()
{
#ifdef DEBUG
	pcre2_match_data_free(this->match_data);
#endif
}

void pfgrep::print_version(const char *tool_name)
{
	pfbase::print_version(tool_name);
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
}

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-A num] [-B num] [-C num] [-m matches] [-cFHhiLlnpqrstwVvx] pattern files...\n", argv0);
	fprintf(stderr, "usage: %s [-A num] [-B num] [-C num] [-m matches] [-cFHhiLlnpqrstwVvx] [-e pattern] [-f file] files...\n", argv0);
}

uint32_t pfgrep::get_compile_flags()
{
	uint32_t flags = 0;
	if (this->case_insensitive) {
		flags |= PCRE2_CASELESS;
	}
	// XXX: We might consider using i.e. str(case)str instead, since it's
	// possibly faster than using PCRE, but it does work w/ the other PCRE
	// flags like matching words/lines, so...
	if (this->fixed) {
		flags |= PCRE2_LITERAL;
	}
	return flags;
}

uint32_t pfgrep::get_extra_compile_flags()
{
	uint32_t flags = 0;
	if (this->match_word) {
		flags |= PCRE2_EXTRA_MATCH_WORD;
	}
	if (this->match_line) {
		flags |= PCRE2_EXTRA_MATCH_LINE;
	}
	return flags;
}

bool pfgrep::print_line(File *file, const char *line, size_t line_size, int lineno)
{
	if (this->quiet && !this->print_count) {
		// Special case: Early return since we don't
		// to count or print more lines
		return false;
	} else if (!this->quiet) {
		if ((this->file_count > 1 && !this->never_print_filename) || this->always_print_filename) {
			printf("%s:", file->filename);
		}
		if (this->print_line_numbers) {
			printf("%d:", lineno);
		}
		fwrite(line, line_size, 1, stdout);
		putchar('\n');
	}
	return true;
}

int pfgrep::do_action(File *file)
{
	int matches = 0, rc = 0;
	uint32_t offset = 0, flags = 0;
	int lineno = 0;
	int current_after_lines = 0;
	char *line = this->conv_buffer, *next = NULL;
	std::deque<std::tuple<const char*, size_t, int>> before_queue;
	// If same CCSID, use read buffer, otherwise if EBCDIC/diff ASCII, conv
	if (file->ccsid == this->pase_ccsid) {
		line = this->read_buffer;
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

		// We can have multiple expressions. Find the first match.
		for (const auto& pattern : this->patterns) {
			pcre2_code *re = pattern->re;

			// As long as we checked that the pattern successfully was JIT
			// compiled, it should be safe to use pcre2_jit_match instead.
			if (this->can_jit) {
				rc = pcre2_jit_match(re, (PCRE2_SPTR)line, conv_size, offset, flags, this->match_data, NULL);
			} else {
				rc = pcre2_match(re, (PCRE2_SPTR)line, conv_size, offset, flags, this->match_data, NULL);
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
			if (!this->silent) {
				PCRE2_UCHAR buffer[256];
				pcre2_get_error_message(rc, buffer, sizeof(buffer));
				fprintf(stderr, "failed match error: %s (%d)\n", buffer, rc);
			}
		} else if ((matched && !this->invert) || (!matched && this->invert)) {
			matches++;
			current_after_lines = this->after_lines;

			// Drain the queue of before items
			for (const auto& queued_line : before_queue) {
				print_line(file, std::get<0>(queued_line),
					std::get<1>(queued_line),
					std::get<2>(queued_line));
			}
			before_queue.clear();

			if (!print_line(file, line, conv_size, lineno)) {
				goto fail;
			}
		} else if (current_after_lines-- > 0) {
			print_line(file, line, conv_size, lineno);
		} else if (this->before_lines) {
			// Push into the queue; make sure we don't go over
			before_queue.push_back({line, conv_size, lineno});
			if (before_queue.size() > this->before_lines) {
				before_queue.pop_front();
			}
		}

		if (this->max_matches > 0 && matches >= this->max_matches) {
			break;
		}

		line = next;
	}
fail:
	return matches;
}

bool pfgrep::add_pattern(const char *expr)
{
	int errornumber;
	PCRE2_SIZE erroroffset;
	pcre2_compile_context *compile_ctx = pcre2_compile_context_create(NULL);
	pcre2_set_compile_extra_options(compile_ctx, get_extra_compile_flags());
	pcre2_code *re = pcre2_compile((PCRE2_SPTR)expr,
			PCRE2_ZERO_TERMINATED,
			get_compile_flags(),
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
	if (capture_count > this->biggest_capture_count) {
		this->biggest_capture_count = capture_count;
	}

	// We should probably be seeing if the JIT status is usable per-expr...
	if (this->can_jit) {
		size_t jit_size = 0;
		int jit_ret = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
		if (jit_ret == 0) {
			jit_ret = pcre2_pattern_info(re, PCRE2_INFO_JITSIZE, &jit_size);
			if (jit_ret != 0 || jit_size == 0) {
				this->can_jit = false;
			}
		}
	}

	this->patterns.push_back(std::make_unique<Pattern>(expr, re));
	return true;
}

bool pfgrep::add_patterns_from_file(const char *path)
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
		if (line_len > 0 && !add_pattern(line)) {
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
	pfgrep state;

	// TODO: Decide to warn the user if JIT is disabled, or if JIT is on but
	// the expression couldn't be compiled. For now, silently ignore errors.
	uint32_t can_jit = 0;
	pcre2_config(PCRE2_CONFIG_JIT, &can_jit);
	state.can_jit = can_jit;

	int ch;
	while ((ch = getopt(argc, argv, "A:B:C:ce:Ff:HhLlim:npqrstwVvx")) != -1) {
		switch (ch) {
		case 'A':
			state.after_lines = atoi(optarg);
			break;
		case 'B':
			state.before_lines = atoi(optarg);
			break;
		case 'C':
			state.after_lines = atoi(optarg);
			state.before_lines = state.after_lines;
			break;
		case 'c':
			state.print_count = true;
			state.quiet = true; // Implied
			break;
		case 'e':
			if (!state.add_pattern(optarg)) {
				return 4;
			}
			break;
		case 'F':
			state.fixed = true;
			break;
		case 'f':
			if (!state.add_patterns_from_file(optarg)) {
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
		case 'm':
			state.max_matches = atoi(optarg);
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
			state.print_version("pfgrep");
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
	bool need_pattern_arg = state.patterns.size() == 0;

	// We take physical files, no stdin, so we need expr + files
	if (need_pattern_arg && optind + 1 >= argc) {
		usage(argv[0]);
		return 3;
	}

	if (need_pattern_arg) {
		char *expr = strdup(argv[optind++]);
		if (!state.add_pattern(expr)) {
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
		int ret = state.do_thing(argv[i], false);
		if (ret > 0) {
			any_match = true;
		} else if (ret < 0) {
			any_error = true;
		}
	}

	return any_error ? 2 : (any_match ? 0 : 1);
}
