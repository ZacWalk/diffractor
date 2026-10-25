// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: ZIP archive handling. Creates and extracts ZIP files, lists archive
// contents, and provides zlib compression/decompression.

#include "pch.h"
#include "util_zip.h"

#include "files.h"
#include "libdesktop/util_file.h"

#define ZLIB_CONST 1

#include "zlib.h"
#include "mz.h"
#include "mz_os.h"
#include "mz_strm.h"
#include "compat/zip.h"
#include "compat/unzip.h"

// Archive entry names are attacker-controlled: reduce to a bare leaf name so an entry such as
// "..\..\startup\evil.exe" cannot escape the destination folder. Returns empty to reject.

namespace
{
	bool is_leap_year(const int year) noexcept
	{
		return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	}

	int days_in_month(const int year, const int month) noexcept
	{
		constexpr std::array<int, 12> lengths = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
		if (month < 1 || month > 12) return 0;
		if (month == 2 && is_leap_year(year)) return 29;
		return lengths[static_cast<size_t>(month - 1)];
	}

	bool to_zip_date(zip_fileinfo& zi, const df::date_t modified)
	{
		auto ft = modified.system_to_local().date();

		if (ft.month < 1 || ft.month > 12 ||
			ft.day < 1 || ft.day > days_in_month(ft.year, ft.month) ||
			ft.hour < 0 || ft.hour > 23 ||
			ft.minute < 0 || ft.minute > 59 ||
			ft.second < 0 || ft.second > 59)
		{
			return false;
		}

		if (ft.year < 1980)
		{
			ft = {1980, 1, 1, 0, 0, 0};
		}
		else if (ft.year > 2107)
		{
			ft = {2107, 12, 31, 23, 59, 58};
		}

		ft.second &= ~1;

		zi.tmz_date.tm_year = ft.year;
		zi.tmz_date.tm_mon = ft.month - 1;
		zi.tmz_date.tm_mday = ft.day;
		zi.tmz_date.tm_hour = ft.hour;
		zi.tmz_date.tm_min = ft.minute;
		zi.tmz_date.tm_sec = ft.second;
		return true;
	}
}

bool df::zip_entry_needs_zip64(const uint64_t offset, const uint64_t size)
{
	// minizip-ng's allowance for deflate output exceeding its input (MZ_ZIP_UNCOMPR_SIZE64_CUSHION).
	constexpr uint64_t expansion_margin = 2ull * 1024 * 1024;
	return offset >= UINT32_MAX || size >= UINT32_MAX - expansion_margin;
}

df::zip_file::~zip_file()
{
	if (_handle.has_value())
	{
		close();
	}
}

bool df::zip_file::create(const file_path zip_file_path)
{
	assert_true(!_handle.has_value());
	const auto handle = zipOpen64(zip_file_path.str().c_str(), 0);

	if (handle == nullptr) return false;

	_handle = handle;
	return true;
}

bool df::zip_file::close()
{
	if (_handle.has_value())
	{
		const auto result = zipClose(std::any_cast<zipFile>(_handle), nullptr) == ZIP_OK;
		_handle.reset();
		return result;
	}

	return true;
}

bool df::zip_file::add(const file_path path, const std::string_view name_in, const date_t modified) const
{
	file f;

	if (f.open_read(path, true))
	{
		const auto attributes = platform::file_attributes(path);
		const auto name = std::string(name_in);

		if (name.size() > UINT16_MAX)
		{
			df::log(__FUNCTION__, std::format("zip entry name is too long: {} bytes", name.size()));
			return false;
		}

		//const auto wpath = platform::to_file_system_path(path)

		zip_fileinfo zi = {};

		if (!to_zip_date(zi, modified))
		{
			df::log(__FUNCTION__, std::format("invalid zip entry date for {}", name));
			return false;
		}

		// Classic headers are read by every tool, so ZIP64 is reserved for an entry whose size or
		// position does not fit 32 bits. The compatibility API offers only forced or disabled.
		const auto archive = std::any_cast<zipFile>(_handle);
		const auto offset = mz_stream_tell(zipGetStream_MZ(archive));
		const auto zip64 = offset < 0 || zip_entry_needs_zip64(static_cast<uint64_t>(offset), attributes.size);
		auto err = zipOpenNewFileInZip4_64(archive, name.c_str(), &zi, nullptr, 0,
		                                  nullptr, 0, nullptr, Z_DEFLATED, Z_BEST_COMPRESSION, 0, -MAX_WBITS, 8,
		                                  Z_DEFAULT_STRATEGY, nullptr, 0, MZ_VERSION_MADEBY, MZ_ZIP_FLAG_UTF8,
		                                  zip64 ? 1 : 0);

		if (err != ZIP_OK)
		{
			df::log(__FUNCTION__, std::format("error in opening {} in zip file", name));
			return false;
		}

		// read64k answers false for the end of the file and for a read that failed alike, so the
		// loop alone cannot tell a complete copy from a truncated one. Counting what was written
		// and holding it to what the file holds is what makes a short read a failure rather than a
		// smaller entry that reports success and looks intact until it is opened.
		uint64_t written = 0;

		while (f.read64k())
		{
			// Read in and write the item
			// in multiple buffer loads

			// Write
			err = zipWriteInFileInZip(std::any_cast<zipFile>(_handle), f.buffer(),
			                          static_cast<uint32_t>(f.buffer_data_size()));

			if (err != ZIP_OK)
			{
				//We could not write the file in the ZIP-File for whatever reason.
				df::log(__FUNCTION__, std::format("error writing {} in zip file", name));
				zipCloseFileInZip(std::any_cast<zipFile>(_handle));
				return false;
			}

			written += f.buffer_data_size();
		}

		if (written != attributes.size)
		{
			df::log(__FUNCTION__, std::format("read {} of {} bytes of {}", written, attributes.size, name));
			zipCloseFileInZip(std::any_cast<zipFile>(_handle));
			return false;
		}

		err = zipCloseFileInZip(std::any_cast<zipFile>(_handle));

		if (err != ZIP_OK)
		{
			df::log(__FUNCTION__, std::format("error closing {} in zip file", name));
			return false;
		}

		return true;
	}

	return false;
}

bool df::zip_file::add(const file_path path, const std::string_view name_in) const
{
	return add(path, name_in, date_t(platform::file_attributes(path).modified));
}

bool df::zip_file::add(const file_path path)
{
	return add(path, path.name());
}

std::vector<archive_item> df::zip_file::list(const file_path zip_file_path)
{
	std::vector<archive_item> results;

	auto* const hz = unzOpen2_64(zip_file_path.str().c_str(), nullptr);

	if (hz)
	{
		unz_global_info64 info;
		unz_file_info64 file;

		if (UNZ_OK == unzGetGlobalInfo64(hz, &info) &&
			UNZ_OK == unzGoToFirstFile(hz))
		{
			do
			{
				if (UNZ_OK == unzGetCurrentFileInfo64(hz, &file, nullptr, 0, nullptr, 0, nullptr, 0))
				{
					if (file.size_filename > UINT16_MAX)
					{
						df::log(__FUNCTION__, std::format("zip entry name is too long: {} bytes", file.size_filename));
						results.clear();
						break;
					}

					std::string filename;
					filename.resize(static_cast<size_t>(file.size_filename));

					const auto name_result = filename.empty()
						                         ? UNZ_OK
						                         : unzGetCurrentFileInfo64(hz, &file, filename.data(),
						                                                   static_cast<uint16_t>(filename.size()),
						                                                   nullptr, 0, nullptr, 0);

					if (name_result != UNZ_OK)
					{
						df::log(__FUNCTION__, "could not read zip entry name");
						results.clear();
						break;
					}

					archive_item result_info;
					result_info.filename = str::utf8_cast(std::string_view(filename.data(), filename.size()));
					result_info.uncompressed_size = file.uncompressed_size;
					result_info.compressed_size = file.compressed_size;
					result_info.created = platform::dos_date_to_ts(static_cast<uint16_t>(file.dosDate >> 16),
					                                               static_cast<uint16_t>(file.dosDate));
					results.emplace_back(result_info);
				}
			}
			while (UNZ_OK == unzGoToNextFile(hz));
		}

		unzClose(hz);
	}

	return results;
}
