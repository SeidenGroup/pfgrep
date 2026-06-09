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
#include <sys/errno.h>
#include <sys/mode.h>
#include <sys/stat.h>
#include <unistd.h>

#include </QOpenSys/usr/include/iconv.h>

#include "errc.h"
}

#include <fmt/format.h>

#include <cstring>
#include <string>

#include "common.hxx"

void pfbase::print_version(const char *tool_name)
{
	fmt::println(stderr, "{} " PFGREP_VERSION, tool_name);
	fmt::println(stderr, "Copyright (c) Seiden Group 2024-2025");
	fmt::println(stderr, "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>");
	fmt::println(stderr, "Written by Calvin Buckley and others, see <https://github.com/SeidenGroup/pfgrep/graphs/contributors>");
}

pfbase::pfbase()
{
	this->pase_ccsid = Qp2paseCCSID();
}

pfbase::~pfbase()
{
#ifdef DEBUG
	// This deinitialization may be unnecessary, do it for future use of
	// sanitizers/*grind when available on i
	free_cached_iconv();
	free(this->read_buffer);
	free(this->conv_buffer);
#endif
}

bool pfbase::read_records(const File &file, iconv_t conv)
{
	size_t read_buf_size = file.file_size + 1;
	if (read_buf_size > this->read_buffer_size) {
		this->read_buffer = (char*)realloc(this->read_buffer, read_buf_size);
		this->read_buffer_size = read_buf_size;
	}
	int bytes_to_read = file.file_size;
	// Read the whole file in
	while ((bytes_to_read = read(file.fd, this->read_buffer, bytes_to_read)) != 0) {
		if (bytes_to_read == -1) {
			if (!this->silent) {
				std::string msg;
				msg = fmt::format("read({}, {})", file.short_filename, bytes_to_read);
				perror_xpf(msg.c_str());
			}
			return false;
		}
	}
	this->read_buffer[file.file_size] = '\0';

	size_t record_count = file.record_count;
	if (record_count <= 0) {
		record_count = file.file_size / file.record_length;
	}
	// record length * 6 for worst case UTF-8 conv + newline
	size_t conv_buf_size = (file.file_size * UTF8_SCALE_FACTOR) + record_count + 1;
	if (conv_buf_size > this->conv_buffer_size) {
		this->conv_buffer = (char*)realloc(this->conv_buffer, conv_buf_size);
		this->conv_buffer_size = conv_buf_size;
	}
	char *out = this->conv_buffer;
	size_t outleft = conv_buf_size;
	for (size_t record_num = 0; record_num < record_count; record_num++) {
		char *record = this->read_buffer + (record_num * file.record_length);
		// Converted record is on a 6x multiplier due to possible
		// worst case EBCDIC->UTF-8 conversion
		char *in = record;
		char *beginning = out;
		size_t inleft = file.record_length;
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

bool pfbase::read_streamfile(const File &file, iconv_t conv)
{
	size_t read_buf_size = file.file_size + 1;
	if (read_buf_size > this->read_buffer_size) {
		this->read_buffer = (char*)realloc(this->read_buffer, read_buf_size);
		this->read_buffer_size = read_buf_size;
	}
	int bytes_to_read = file.file_size;
	size_t record_count = file.file_size;
	// Assume max length plus newline character for each line
	size_t conv_buf_size = read_buf_size + record_count;
	if (conv_buf_size > this->conv_buffer_size) {
		this->conv_buffer = (char*)realloc(this->conv_buffer, conv_buf_size);
		this->conv_buffer_size = conv_buf_size;
	}
	// Read the whole file in
	while ((bytes_to_read = read(file.fd, this->read_buffer, bytes_to_read)) != 0) {
		if (bytes_to_read == -1) {
			if (!this->silent) {
				std::string msg;
				msg = fmt::format("read({}, {})", file.full_filename, bytes_to_read);
				perror_xpf(msg.c_str());
			}
			return false;
		}
	}
	this->read_buffer[file.file_size] = '\0';

	// Skip the copy, we'll just work against the read buffer directly.
	// Save an unnecessary iconv and conversion.
	if (file.ccsid == this->pase_ccsid) {
		this->conv_buffer[0] = '\0';
		return true;
	}

	char *in = this->read_buffer;
	size_t inleft = file.file_size;
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
	std::string msg;
	int files_matched = 0;
	DIR *dir = opendir(directory);
	if (dir == NULL) {
		if (!this->silent) {
			msg = fmt::format("opendir({})", directory);
			perror_xpf(msg.c_str());
		}
		return -1;
	}
	struct dirent *dirent = NULL;
	char old_dir[PATH_MAX + 1];
	// On i, resolution is faster from cwd, path traversal is expensive
	if (getcwd(old_dir, PATH_MAX + 1) == NULL) {
		if (!this->silent) {
			msg = fmt::format("getcwd({})", directory);
			perror_xpf(msg.c_str());
		}
		return -1;
	}
	if (chdir(directory) == -1) {
		if (!this->silent) {
			msg = fmt::format("getcwd({})", directory);
			perror_xpf(msg.c_str());
		}
		return -1;
	}
	while ((dirent = readdir(dir)) != NULL) {
		if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0) {
			continue;
		}

		// Raise the file count in case single dir/PF passed,
		// so filenames of subdirectories are printed
		this->file_count++;

		int ret = do_thing(dirent->d_name, directory, true);
		if (ret > 0) {
			files_matched += ret;
		}
		errno = 0; // Don't let i.e. iconv errors influence the next call
	}
	if (errno != 0) {
		if (!this->silent) {
			msg = fmt::format("reading dirent in {}", directory);
			perror_xpf(msg.c_str());
		}
	}
	closedir(dir);
	if (chdir(old_dir) == -1) {
		if (!this->silent) {
			msg = fmt::format("getcwd({})", directory);
			perror_xpf(msg.c_str());
		}
		return -1;
	}
	return files_matched;
}

bool pfbase::set_record_length(File &file)
{
	// Determine the record length, the API to do this needs traditional paths.
	// Note that it will resolve symlinks for us, so i.e. /QIBM/include works
	int ret = filename_to_libobj(file);
	if (ret == -1) {
		if (!this->silent) {
			fmt::println(stderr, "filename_to_libobj({}): Failed to convert IFS path to object name",
				file.full_filename);
		}
		return false;
	}
	int file_record_size = get_pf_info(file);
	if (file_record_size == 0 && errno == ENODEV) {
		// Ignore files we can't support w/ POSIX I/O for now
		return false;
	} else if (file_record_size == 0) {
		if (!this->silent) {
			fmt::println(stderr, "get_pf_info({}): Couldn't get record length",
				file.full_filename);
		}
		return false;
	} else if (file_record_size < 0 && this->search_non_source_files) {
		// Non-source PF, signedness is used as source PF bit
		file.record_length = -file_record_size;
		return true;
	} else if (file_record_size > 0) {
		// Source PF, length includes other metadata not pulled when
		// reading source PFs via POSIX APIs
		file.record_length = file_record_size - 12;
		return true;
	}
	return false;
}

int pfbase::do_file(File &file)
{
	std::string msg;
	int matches = -1;
	iconv_t conv = (iconv_t)(-1);

	// Only open after we know it's a valid thing to open.
	// Note that it's safe to use short_filename because it's bound to the
	// suffix of the full filename.
	file.fd = open(file.short_filename.data(), O_RDONLY);
	// We let do_file fill in the filename and CCSID. Technically a TOCTOU
	// problem, but open(2) error reporting with IBM i objects is goofy.
	if (file.fd == -1) {
		if (!this->silent) {
			msg = fmt::format("open({})", file.full_filename);
			perror_xpf(msg.c_str());
		}
		return -1;
	}

	// Get member info for an accurate record count
	if (file.record_length != 0) {
		if (!get_member_info(file) && !this->silent) {
			msg = fmt::format("get_member_info({})", file.full_filename);
			perror(msg.c_str());
		}
	}

	// Open a conversion for this CCSID
	conv = get_iconv(file.ccsid);
	if (conv == (iconv_t)(-1)) {
		if (!this->silent) {
			msg = fmt::format("iconv_open({}, {})", Qp2paseCCSID(), file.ccsid);
			perror(msg.c_str());
		}
		goto fail;
	}

	if (!this->dont_read_file) {
		// Streamfiles are record length 0, and must be read differently
		if (file.record_length == 0) {
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

fail:
	// shift this should be reset after each file in case of MBCS/DBCS
	if (conv != (iconv_t)(-1)) {
		reset_iconv(conv);
	}

	if (file.fd != -1) {
		close(file.fd);
	}
	return matches;
}

int pfbase::do_thing(const char *filename, bool from_recursion)
{
	return do_thing(filename, nullptr, from_recursion);
}

int pfbase::do_thing(const char *filename, const char *dirname, bool from_recursion)
{
	std::string msg;
	int matches = 0;
	struct stat64_ILE s = {};
	File f = {};

	if (dirname != nullptr) {
		size_t filename_pos = strlen(dirname);
		if (dirname[filename_pos - 1] == '/') {
			f.full_filename = fmt::format("{}{}", dirname, filename);
		} else {
			f.full_filename = fmt::format("{}/{}", dirname, filename);
			filename_pos++;
		}
		f.short_filename = string_view(f.full_filename.c_str() + filename_pos);
	} else {
		f.full_filename = filename;
		f.short_filename = f.full_filename;
	}
	// IBM messed up the statx declaration, it doesn't write
	int ret = statx((char*)filename, (struct stat*)&s, sizeof(s), STX_XPFSS_PASE);
	if (ret == -1) {
		if (!this->silent) {
			msg = fmt::format("stat({})", filename);
			perror_xpf(msg.c_str());
		}
		return -1;
	}
	f.file_size = s.st_size;
	// XXX: This is 32-bit with ILE mtime
	f.mtime = s.st_mtime;
	// objtype is *FILE or *DIR, check for mode though to avoid i.e. SAVFs
	if (S_ISDIR(s.st_mode)) {
		if (this->recurse) {
			// Avoid recursion in a loop; get the ID (dev+inode) of
			// each directory. Note that we use the (truncated?) ID
			// that's 32-bit for device and inode, so we can mask
			// them into a single value. Consider that a TODO...
			uint64_t devino = ((uint64_t)s.st_dev << 32) | s.st_ino;
			// XXX: C++20: .contains
			if (visited_directories.find(devino) != visited_directories.end()) {
				return 0;
			}
			visited_directories.emplace(devino);
			int subdir_files_matched = do_directory(f.full_filename.c_str());
			if (subdir_files_matched >= 0) {
				matches += subdir_files_matched;
			}
		} else {
			if (!this->silent) {
				fmt::println(stderr, "stat({}): Is a directory or physical file", filename);
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
		if (!set_record_length(f)) {
			return from_recursion ? 0 : -1; // messages emited in function
		}
		matches = do_file(f);
	} else if (strcmp(s.st_objtype, "*STMF     ") == 0) {
		f.ccsid = s.st_ccsid; // or st_codepage?
		f.record_length = 0;
		matches = do_file(f);
	}
	// XXX: Message for non-PF/members?
	return matches;
}
