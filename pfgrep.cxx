/*
 * Copyright (c) 2024-2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

extern "C" {
#include <unistd.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include </QOpenSys/usr/include/iconv.h>
#include <pcre2.h>

#include "errc.h"
}

#include <fmt/base.h>

#include <deque>
#if defined(__cpp_lib_optional)
#include <optional>
#else
#include <experimental/optional>
#endif
#include <string>
#if defined(__cpp_lib_string_view)
#include <string_view>
#else
#include <experimental/string_view>
#endif
#include <utility>
#include <vector>

#include "common.hxx"

// XXX: Remove when we can assume C++17 minimum
// We should also use .has_value() instead for C++
#if defined(__cpp_lib_optional)
using std::optional;
using std::nullopt;
#else
using std::experimental::optional;
using std::experimental::nullopt;
#endif
#if defined(__cpp_lib_string_view)
using std::string_view;
#else
using std::experimental::string_view;
#endif

class Pattern {
public:
	Pattern(const std::string &expr, pcre2_code *re, bool can_jit) : expr(expr) {
		this->re = re;
		this->can_jit = can_jit;
	}

	// Lifetime is that of pattern_strings
	const std::string &expr;
	// Don't handle destruction in this, since we can get copied by
	// collections. Free at the end instead.
	// XXX: Probably use *_ptr...
	pcre2_code *re;
	bool can_jit;
};

class Match {
public:
	Match(const char *line, size_t length, int lineno, std::vector<string_view> substrings) {
		this->line = line;
		this->length = length;
		this->lineno = lineno;
		this->context = false;
		this->substrings = std::move(substrings);
	}

	Match(const char *line, size_t length, int lineno, bool context) {
		this->line = line;
		this->length = length;
		this->lineno = lineno;
		this->context = context;
		this->substrings = {};
	}

	// This is an offset into the file; alive as long as the match
	const char *line;
	size_t length;
	int lineno;
	bool context;
	// XXX: Right type for this?
	std::vector<string_view> substrings;
};

class PCRE2Error {
public:
	PCRE2Error(int rc) {
		this->rc = rc;
	}

	int rc;
};

class pfgrep : public pfbase {
public:
	~pfgrep();
	int do_action(File &file) override;
	void print_version(const char *tool_name);
	bool compile_pattern(const std::string &expr);
	bool add_patterns_from_file(const char *path);
	uint32_t get_compile_flags();
	uint32_t get_extra_compile_flags();

	/* Pattern */
	std::vector<std::string> pattern_strings;
	std::vector<Pattern> patterns;
	pcre2_general_context *general_context = nullptr;
	pcre2_compile_context *compile_context = nullptr;
	pcre2_match_data *match_data = nullptr;
	uint32_t biggest_capture_count = 0;
	bool can_jit = false;
	/* Options */
	bool print_matching_files = false;
	bool print_nonmatching_files = false;
	bool print_count = false;
	bool print_only_substrings = false;
	bool case_insensitive = false;
	bool always_print_filename = false;
	bool never_print_filename = false;
	bool print_line_numbers = false;
	bool invert = false;
	bool match_word = false;
	bool match_line = false;
	bool fixed = false;
	bool search_descriptions = false;
	int max_matches = 0;
	int after_lines = 0;
	unsigned int before_lines = 0;
	/* Current cross-file state */
	bool has_printed = false;

private:
	inline const char *maybe_colour(const char *colour);
	inline void print_separator();
	void print_filename(const std::string &filename, int count);
	inline void print_line_beginning(const File &file, const Match &match);
	bool print_line(const File &file, const Match &match);
	optional<Match> try_patterns(const char *line, size_t line_size, int line_no);
};

pfgrep::~pfgrep()
{
#ifdef DEBUG
	pcre2_match_data_free(this->match_data);
	for (const auto& pattern : patterns) {
		pcre2_code_free(pattern.re);
	}
	pcre2_compile_context_free(compile_context);
	pcre2_general_context_free(general_context);
#endif
}

void pfgrep::print_version(const char *tool_name)
{
	pfbase::print_version(tool_name);
	char pcre2_ver[256], pcre2_jit[256];
	uint32_t pcre2_can_jit = 0;
	pcre2_config(PCRE2_CONFIG_JIT, &pcre2_can_jit);
	pcre2_config(PCRE2_CONFIG_VERSION, pcre2_ver);
	fmt::print(stderr, "\tusing PCRE2 {}", pcre2_ver);
	if (pcre2_can_jit) {
		pcre2_config(PCRE2_CONFIG_JITTARGET, pcre2_jit);
		fmt::println(stderr, " (JIT target: {})", pcre2_jit);
	} else {
		fmt::println(stderr, " (no JIT)");
	}
}

static void usage(char *argv0)
{
	fmt::println(stderr, "usage: {} [-A num] [-B num] [-C num] [-m matches] [-cFHhiLlnopqrstwVvx] pattern files...", argv0);
	fmt::println(stderr, "usage: {} [-A num] [-B num] [-C num] [-m matches] [-cFHhiLlnopqrstwVvx] [-e pattern] [-f file] files...", argv0);
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

inline const char *pfgrep::maybe_colour(const char *colour)
{
	if (this->colourize == ColourizeAlways) {
		return colour;
	}
	return "";
}

inline void pfgrep::print_separator()
{
	// Don't worry about reseting it, as colour output will always follow
	fmt::println("{}--", maybe_colour(PFGREP_COLON_COLOUR));
}

void pfgrep::print_filename(const std::string &filename, int count)
{
	fmt::print("{}{}", maybe_colour(PFGREP_FILNAM_COLOUR), filename);
	if (count > -1) {
		fmt::println("{}:{}{}", maybe_colour(PFGREP_COLON_COLOUR),
			maybe_colour(PFGREP_NORMAL_COLOUR), count);
	} else {
		fmt::println("{}", maybe_colour(PFGREP_NORMAL_COLOUR));
	}
}

inline void pfgrep::print_line_beginning(const File &file, const Match &match)
{
	const char *colon = match.context ? "-" : ":";
	if ((this->file_count > 1 && !this->never_print_filename) || this->always_print_filename) {
		fmt::print("{}{}{}{}", maybe_colour(PFGREP_FILNAM_COLOUR),
			file.full_filename, maybe_colour(PFGREP_COLON_COLOUR),
			colon);
	}
	if (this->print_line_numbers) {
		fmt::print("{}{}{}{}", maybe_colour(PFGREP_LINENO_COLOUR),
			match.lineno, maybe_colour(PFGREP_COLON_COLOUR),
			colon);
	}
}

bool pfgrep::print_line(const File &file, const Match &match)
{
	if (this->quiet && !this->print_count) {
		// Special case: Early return since we don't
		// to count or print more lines
		return false;
	} else if (!this->quiet && this->print_only_substrings && match.substrings.size()) {
		for (const auto& substring : match.substrings) {
			print_line_beginning(file, match);
			fmt::println("{}{}{}", maybe_colour(PFGREP_MATCH_COLOUR),
				substring, maybe_colour(PFGREP_NORMAL_COLOUR));
		}
	} else if (!this->quiet) {
		print_line_beginning(file, match);
		if (this->colourize == ColourizeAlways && match.substrings.size()) {
			size_t last_substring_end = 0;
			for (const auto& substring : match.substrings) {
				auto before = string_view(match.line + last_substring_end,
					(substring.data() - match.line) - last_substring_end);
				fmt::print("{}{}{}{}",
					maybe_colour(PFGREP_NORMAL_COLOUR), before,
					maybe_colour(PFGREP_MATCH_COLOUR), substring);
				last_substring_end = (substring.data() - match.line) + substring.size();
			}
			auto after = string_view(match.line + last_substring_end,
				match.length - last_substring_end);
			fmt::println("{}{}", maybe_colour(PFGREP_NORMAL_COLOUR), after);
		} else {
			fmt::println("{}{}", maybe_colour(PFGREP_NORMAL_COLOUR),
				string_view(match.line, match.length));
		}
	}
	this->has_printed = true;
	return true;
}

optional<Match> pfgrep::try_patterns(const char *line, size_t line_size, int line_no)
{
	uint32_t offset = 0, flags = 0;
	int rc = 0;
	// XXX: Enable scan_more for structured output too
	bool scan_more = this->colourize == ColourizeAlways, matched = false;
	std::vector<string_view> substrings;
	size_t last_substring_end = 0;
	// We can have multiple expressions. Find the first match.
	for (const auto& pattern : this->patterns) {
		pcre2_code *re = pattern.re;

again:
		// As long as we checked that the pattern successfully was JIT
		// compiled, it should be safe to use pcre2_jit_match instead.
		if (pattern.can_jit) {
			rc = pcre2_jit_match(re, (PCRE2_SPTR)line, line_size, offset, flags, this->match_data, nullptr);
		} else {
			rc = pcre2_match(re, (PCRE2_SPTR)line, line_size, offset, flags, this->match_data, nullptr);
		}

		if (rc > 0) {
			matched = true;
			size_t* ovector = pcre2_get_ovector_pointer(this->match_data);
			size_t substring_length = ovector[1] - ovector[0];
			// Cheap way to avoid overlap and having to do more
			// complicated substring coalescing
			if ((ovector[0] > last_substring_end) || substrings.size() == 0) {
				substrings.emplace_back(line + ovector[0], substring_length);
			} else if ((ovector[0] == last_substring_end) && substrings.size()) {
				// If the two substrings run into each other
				const char *old_string = substrings.back().data();
				size_t new_length = substrings.back().size() + substring_length;
				substrings.back() = string_view(old_string, new_length);
			}
			// Scan more in this string; be careful not to loop
			// XXX: Use pcre2_next_match when we get newer PCRE2
			if (ovector[0] == ovector[1]) {
				break; // i.e. if empty string is pattern
			}
			last_substring_end = ovector[1];
			offset = ovector[1];
			goto again;
		}

		if (rc > 0 && !scan_more) {
			break;
		} else if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) {
			throw PCRE2Error(rc);
		}
	}
	if (!matched) {
		return nullopt;
	}
	return Match(line, line_size, line_no, std::move(substrings));
}

int pfgrep::do_action(File &file)
{
	int matches = 0, rc = 0;
	int lineno = 0;
	int last_printed_line = -1;
	int current_after_lines = 0;
	char *line = this->conv_buffer, *next = nullptr;
	std::deque<Match> before_queue;
	// If same CCSID, use read buffer, otherwise if EBCDIC/diff ASCII, conv
	if (file.ccsid == this->pase_ccsid) {
		line = this->read_buffer;
	}

	// For search descriptions (special behaviour where we match,
	// but treat it as a non-line for i.e. context purposes)
	if (this->search_descriptions && file.record_length > 0) {
		size_t desc_size = strlen(file.description);
		// Trim since we invariably have this
		if (!this->dont_trim_ending_whitespace) {
			while (desc_size > 0 && file.description[desc_size - 1] == ' ') {
				desc_size--;
			}
		}
		optional<Match> match = try_patterns(file.description, desc_size, 0);
		if (match != nullopt) {
			print_line(file, *match);
			last_printed_line = 0;
		}
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

		optional<Match> match;
		try {
			match = try_patterns(line, conv_size, lineno);
		} catch (PCRE2Error pcre2error) {
			if (!this->silent) {
				PCRE2_UCHAR buffer[256];
				pcre2_get_error_message(rc, buffer, sizeof(buffer));
				fmt::print(stderr, "failed match error: {} ({})", (const char*)buffer, pcre2error.rc);
			}
			goto fail;
		}

		if (match != nullopt) {
			matched = true;
		}
		if ((matched && !this->invert) || (!matched && this->invert)) {
			matches++;
			current_after_lines = this->after_lines;

			const bool has_context_lines = this->after_lines || before_lines;
			const bool separator_for_file = this->has_printed && last_printed_line <= 0;
			const bool separator_for_group = last_printed_line >= 0 && (last_printed_line < lineno - 1);
			if (has_context_lines && (separator_for_file || separator_for_group)) {
				print_separator();
			}
			last_printed_line = lineno;
			// Drain the queue of before items
			for (const auto& queued_match : before_queue) {
				print_line(file, queued_match);
			}
			before_queue.clear();

			if (matched && !print_line(file, *match)) {
				goto fail;
			} else if (!matched && !print_line(file,
					Match(line, conv_size, lineno, false))) {
				goto fail;
			}
		} else if (current_after_lines-- > 0) {
			last_printed_line = lineno;
			print_line(file, {line, conv_size, lineno, true});
		} else if (this->before_lines) {
			// Push into the queue; make sure we don't go over
			before_queue.emplace_back(line, conv_size, lineno, true);
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
	if (matches == 0 && this->print_nonmatching_files) {
		print_filename(file.full_filename, -1);
	} else if (matches > 0 && this->print_matching_files) {
		print_filename(file.full_filename, -1);
	} else if (this->print_count) {
		print_filename(file.full_filename, matches);
	}
	return matches;
}

bool pfgrep::compile_pattern(const std::string &expr)
{
	int errornumber;
	PCRE2_SIZE erroroffset;
	pcre2_code *re = pcre2_compile((PCRE2_SPTR)expr.c_str(),
			PCRE2_ZERO_TERMINATED,
			get_compile_flags(),
			&errornumber,
			&erroroffset,
			this->compile_context);
	if (re == nullptr) {
		PCRE2_UCHAR buffer[256];
		pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
		fmt::println(stderr, "Failed to compile regular expression \"{}\" at offset {}: {}",
				expr,
				(int)erroroffset,
				(const char*)buffer);
		return false;
	}

	// Allocate the largest required match_data later.
	uint32_t capture_count = 0;
	pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);
	if (capture_count > this->biggest_capture_count) {
		this->biggest_capture_count = capture_count;
	}

	bool pattern_can_jit = false;
	if (this->can_jit) {
		size_t jit_size = 0;
		int jit_ret = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
		if (jit_ret == 0) {
			jit_ret = pcre2_pattern_info(re, PCRE2_INFO_JITSIZE, &jit_size);
			pattern_can_jit = jit_size > 0;
		}
	}

	this->patterns.emplace_back(expr, re, pattern_can_jit);
	return true;
}

bool pfgrep::add_patterns_from_file(const char *path)
{
	FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
	if (f == nullptr) {
		perror("can't open pattern file");
		return false;
	}
	char *line = nullptr;
	size_t line_limit = 0;
	ssize_t line_len;
	bool ret = true;
	while ((line_len = getline(&line, &line_limit, f)) > 0) {
		// getline adds the ending newline, we don't want it
		if (line_len > 0 && line[line_len - 1] == '\n') {
			line[line_len - 1] = '\0';
		}
		this->pattern_strings.emplace_back(line);
		// getline allocated, we duped in array
		free(line);
		line = nullptr;
		line_limit = 0;
	}
	if (f != stdin) {
		fclose(f);
	}
	return ret;
}

extern "C" void *pfgrep_wrapped_malloc(size_t n, void*)
{
	return malloc(n);
}

extern "C" void pfgrep_wrapped_free(void *ptr, void*)
{
#ifdef DEBUG
	free(ptr);
#else
	(void)ptr;
#endif
}

int main(int argc, char **argv)
{
	pfgrep state;

	// PCRE only really mallocs for compiling and the JIT; matches are very
	// memory efficient and don't alloc. But if we do change allocators in
	// the future, make that easily possible.
	state.general_context = pcre2_general_context_create(pfgrep_wrapped_malloc, pfgrep_wrapped_free, nullptr);

	// TODO: Decide to warn the user if JIT is disabled, or if JIT is on but
	// the expression couldn't be compiled. For now, silently ignore errors.
	uint32_t can_jit = 0;
	pcre2_config(PCRE2_CONFIG_JIT, &can_jit);
	state.can_jit = can_jit;

	int ch;
	while ((ch = getopt(argc, argv, "A:B:C:cde:Ff:HhLlim:nopqrstwVvx")) != -1) {
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
			state.print_matching_files = false;
			state.print_nonmatching_files = false;
			state.quiet = true; // Implied
			break;
		case 'd':
			state.search_descriptions = true;
			break;
		case 'e':
			state.pattern_strings.emplace_back(optarg);
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
			state.print_count = false;
			state.print_matching_files = false;
			state.print_nonmatching_files = true;
			state.quiet = true; // Implied
			break;
		case 'l':
			state.print_count = false;
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
		case 'o':
			state.print_only_substrings = true;
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

	// TODO: flag
	if (state.colourize == ColourizeAuto) {
		if (getenv("NO_COLOR") != nullptr) {
			state.colourize = ColourizeNever;
		} else if (isatty(1) && (getenv("TERM") != nullptr)) {
			state.colourize = ColourizeAlways;
		}
	}

	// If -e nor -f were used, expect expr as first arg
	bool need_pattern_arg = state.pattern_strings.size() == 0;
	// We take physical files, no stdin, so we need expr + files
	if (need_pattern_arg && optind + 1 >= argc) {
		usage(argv[0]);
		return 3;
	}
	if (need_pattern_arg) {
		const char *expr = argv[optind++];
		state.pattern_strings.emplace_back(expr);
	}
	// We have to get the list of patterns first; as flags can be passed
	// after in the case of -e and -f.
	state.compile_context = pcre2_compile_context_create(state.general_context);
	pcre2_set_compile_extra_options(state.compile_context, state.get_extra_compile_flags());
	for (const auto& pattern_string : state.pattern_strings) {
		if (!state.compile_pattern(pattern_string)) {
			return 4;
		}
	}

	// One big match data that can handle all possible;
	// uses capture count + 1 like pcre2_match_data_create_from_pattern
	state.match_data = pcre2_match_data_create(state.biggest_capture_count + 1, state.general_context);
	if (state.match_data == nullptr) {
		if (!state.silent) {
			fmt::println(stderr, "failed match error: Couldn't allocate memory for match data");
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
