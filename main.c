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
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <json_c_version.h>
#include <linkhash.h>

#include "pfgrep.h"
#include "errc.h"

// These contain conversions from convs[N] to system PASE CCSID, memoized to
// avoid constantly reopening iconv for conversion. Gets closed on exit.
// Because we only convert to a single CCSID, we can keep the maping flat.
// The memory cost of this should be minimal on modern systems.
iconv_t convs[UINT16_MAX] = {0};

static int do_thing(pfgrep *state, const char *filename, bool from_recursion);

static void usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-cFHhiLlnpqrstwVvx] pattern files...\n", argv0);
	fprintf(stderr, "usage: %s [-cFHhiLlnpqrstwVvx] [-e pattern] [-f file] files...\n", argv0);
}

static void version(void)
{
	fprintf(stderr, "pfgrep " PFGREP_VERSION "\n");
	fprintf(stderr, "\tusing json-c %s\n", json_c_version());
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

static int match_multiline(pfgrep *state, File *file)
{
	int matches = 0, rc = 0;
	uint32_t offset = 0, flags = 0;
	// We can have multiple expressions. Find the first match.
	size_t pattern_count = json_object_array_length(state->patterns);
	int lineno = 0;
	char *line = state->conv_buffer, *next = NULL;
	// If same CCSID, use read buffer, otherwise if EBCDIC/diff ASCII, conv
	if (file->ccsid == Qp2paseCCSID()) {
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
			if (state->quiet && !state->print_count) {
				// Special case: Early return since we don't
				// to count or print more lines
				goto fail;
			} else if (!state->quiet) {
				if ((state->file_count > 1 && !state->never_print_filename) || state->always_print_filename) {
					printf("%s:", file->filename);
				}
				if (state->print_line_numbers) {
					printf("%d:", lineno);
				}
				fwrite(line, conv_size, 1, stdout);
				putchar('\n');
			}
		}

		line = next;
	}
fail:
	return matches;
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
	if (file->ccsid == Qp2paseCCSID()) {
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
		matches = match_multiline(state, file);
	} else {
		if (!read_records(state, file, conv)) {
			goto fail;
		}
		matches = match_multiline(state, file);
	}


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

static int do_thing(pfgrep *state, const char *filename, bool from_recursion)
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
	state.patterns = json_object_new_array();

	// The default hashing algorithm linkhash uses is fine, but since we
	// deal with 20 character strings with few allowed characters, it
	// should be safe to use the simpler "Perl-like" hash, which is a bit
	// faster than the default.
	json_global_set_string_hash(JSON_C_STR_HASH_PERLLIKE);

	// TODO: Decide to warn the user if JIT is disabled, or if JIT is on but
	// the expression couldn't be compiled. For now, silently ignore errors.
	uint32_t can_jit = 0;
	pcre2_config(PCRE2_CONFIG_JIT, &can_jit);
	state.can_jit = can_jit;

	int ch;
	while ((ch = getopt(argc, argv, "ce:Ff:HhLlinpqrstwVvx")) != -1) {
		switch (ch) {
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
			version();
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
	for (int i = 0; i < UINT16_MAX; i++) {
		iconv_t conv = convs[i];
		if (conv == NULL || conv == (iconv_t)(-1)) {
			continue;
		}
		iconv_close(conv);
	}
	pcre2_match_data_free(state.match_data);
	json_object_put(state.patterns);
	free_cached_record_sizes();
	free(state.read_buffer);
	free(state.conv_buffer);
#endif

	return any_error ? 2 : (any_match ? 0 : 1);
}
