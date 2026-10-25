// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for the file layer (files*) -- format detection, codec decode and encode, container listing, and the write path: staging, replacement, originals, collisions, flush failure and temp cleanup.

#include "pch.h"
#include "files.h"
#include "crypto.h"
#include "libfiles/commodore.h"
#include "libfiles/exif.h"
#include "libfiles/exif_listing.h"
#include "libfiles/scan.h"
#include "test.h"
#include "util_zip.h"
#include "metadata_exif.h"
#include "metadata_iptc.h"
#include "metadata_xmp.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "app_util.h"

#include "webp/decode.h"
#include "webp/encode.h"
#include "webp/mux.h"
#include <zlib.h>
#include <png.h>
#include <cstdio>
#include <jxl/encode.h>
#include <jxl/color_encoding.h>

#define LIBARCHIVE_STATIC
#include <archive.h>
#include <archive_entry.h>
#undef min
#undef max
#undef GetObject

extern "C" {
#include <jpeglib.h>
}

#define LIBHEIF_STATIC_BUILD 1
#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <libheif/heif_properties.h>

static void should_check_overwrite()
{
	const auto src_path = df::file_path(test_files_folder, "Test.jpg");
	const auto dest_folder = test_files_folder;

	df::item_set items;
	const auto item = std::make_shared<df::item_element>(src_path, df::index_file_item{});
	items.add(item);

	const auto overwrites = check_overwrite(dest_folder, items, {});
	assert_equal(true, !overwrites.empty(), "should detect existing file");

	const auto no_overwrites = check_overwrite(dest_folder, items, ".xyz");
	assert_equal(true, no_overwrites.empty(), "should not detect with different extension");
}

static void should_report_zip_create_failure()
{
	df::zip_file zip;
	const auto missing_folder = _temps.folder().combine("missing");
	const auto path = missing_folder.combine_file("items.zip");
	assert_equal(false, zip.create(path), "zip create failure reported");
	assert_equal(false, path.exists(), "failed zip was not created");
}

class recording_file final : public platform::file
{
	df::file_path _path;
	df::blob _prefix;
	uint64_t _size = 0;
	mutable uint64_t _pos = 0;

public:
	mutable uint64_t max_read = 0;
	mutable uint64_t read_calls = 0;

	recording_file(df::file_path path, df::blob prefix, const uint64_t size) :
		_path(std::move(path)), _prefix(std::move(prefix)), _size(size)
	{
	}

	uint64_t size() const override { return _size; }

	uint64_t read(uint8_t* buf, const uint64_t buf_size) const override
	{
		const auto available = _pos < _size ? std::min(buf_size, _size - _pos) : 0;
		memset(buf, 0, static_cast<size_t>(available));

		if (_pos < _prefix.size())
		{
			const auto copied = std::min<uint64_t>(available, _prefix.size() - _pos);
			memcpy(buf, _prefix.data() + static_cast<size_t>(_pos), static_cast<size_t>(copied));
		}

		_pos += available;
		max_read = std::max(max_read, available);
		++read_calls;
		return available;
	}

	uint64_t write(const uint8_t*, uint64_t) override { return 0; }
	bool flush() const override { return true; }

	uint64_t seek(const uint64_t pos, const whence w) const override
	{
		switch (w)
		{
		case whence::begin:
			_pos = pos;
			break;
		case whence::current:
			_pos += pos;
			break;
		case whence::end:
			_pos = _size + pos;
			break;
		}
		return _pos;
	}

	uint64_t pos() const override { return _pos; }
	bool trunc(uint64_t) const override { return false; }
	df::file_path path() const override { return _path; }
};

// The copy loop stops when read64k answers false, which it does for the end of the file and for a
// read that failed alike - so a truncated copy used to close as a successful entry. What is written
// is now held to what the file holds. A real short read cannot be staged here without a fault
// injection seam, so what this pins is the other half: that a file crossing several buffer loads
// still satisfies the check rather than being refused as short.
static void should_add_a_multi_chunk_file_to_a_zip()
{
	const auto source = _temps.next_path(".bin");

	// Three buffer loads and a remainder, so the loop runs more than once and ends part way.
	df::blob payload(64u * 1024u * 3u + 517u);
	for (auto i = 0_z; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 7 + (i >> 11));
	df::blob_save_to_file(payload, source);

	const auto archive = _temps.next_path(".zip");
	df::zip_file zip;
	assert_equal(true, zip.create(archive), "the archive is created");
	assert_equal(true, zip.add(source, "payload.bin"), "a file spanning several reads is added whole");
	assert_equal(true, zip.close(), "and the archive closes");

	const auto listed = df::zip_file::list(archive);
	assert_equal(1_z, listed.size(), "the archive holds the one entry");
	assert_equal(static_cast<uint64_t>(payload.size()), listed.front().uncompressed_size.to_int64(),
	             "and it holds every byte of the source");
}

static void should_round_trip_zip_entry_names()
{
	const auto source = _temps.next_path(".bin");
	const df::blob payload = {1, 2, 3};
	df::blob_save_to_file(payload, source);

	const auto check_name = [&](const std::string& name)
	{
		const auto archive = _temps.next_path(".zip");
		df::zip_file zip;
		assert_equal(true, zip.create(archive), "the archive is created");
		assert_equal(true, zip.add(source, name, df::date_t(2024, 1, 2, 3, 4, 6)), "the long-name entry is added");
		assert_equal(true, zip.close(), "the archive closes");

		const auto listed = df::zip_file::list(archive);
		assert_equal(1_z, listed.size(), "the archive holds one long-name entry");
		assert_equal(name.size(), listed.front().filename.size(), "the complete entry name length is listed");
		assert_equal(true, listed.front().filename == name, "the complete entry name is listed exactly");
	};

	check_name(std::string(255, 'a'));
	check_name(std::string(256, 'b'));
	check_name(std::string(257, 'c'));
	check_name(std::string(UINT16_MAX, 'd'));

	df::zip_file zip;
	assert_equal(true, zip.create(_temps.next_path(".zip")), "the archive for an oversized name is created");
	assert_equal(false, zip.add(source, std::string(static_cast<size_t>(UINT16_MAX) + 1u, 'e'),
	                            df::date_t(2024, 1, 2, 3, 4, 6)),
	             "an entry name beyond the ZIP 16-bit length is refused");
}

// ZIP64 headers are reserved for entries that need them: a small archive keeps the classic headers
// every reader accepts, while an entry past 4 GiB of archive, or holding nearly that much, still fits.
static void should_write_utf8_zip_entries()
{
	const auto source = _temps.next_path(".bin");
	write_test_file(source, "archive content");
	const auto path = _temps.next_path(".zip");
	const std::string name = "\xe6\x9d\xb1\xe4\xba\xac.bin";
	df::zip_file zip;
	assert_equal(true, zip.create(path), "ZIP created");
	assert_equal(true, zip.add(source, name), "Unicode entry written");
	assert_equal(true, zip.close(), "ZIP closed");
	const auto bytes = df::blob_from_file(path);
	assert_equal(true, bytes.size() >= 30, "local ZIP header present");
	const auto flags = bytes[6] | bytes[7] << 8;
	assert_equal(true, (flags & (1 << 11)) != 0, "entry declares UTF-8 rather than relying on the reader's code page");
	const auto version = bytes[4] | bytes[5] << 8;
	assert_equal(true, version < 45, "a small entry keeps classic headers");
	const auto listed = files::list_archive(path);
	assert_equal(1_z, listed.size(), "independent libarchive reader finds the entry");
	assert_equal(name, listed.front().filename, "independent reader preserves its Unicode name");

	constexpr uint64_t last_classic_offset = 0xFFFFFFFEull;
	constexpr uint64_t large_size = 0xFFFFFFFFull - 2ull * 1024 * 1024;
	assert_equal(false, df::zip_entry_needs_zip64(last_classic_offset, 15), "the last 32-bit offset is classic");
	assert_equal(true, df::zip_entry_needs_zip64(last_classic_offset + 1, 15), "an entry beyond it needs ZIP64");
	assert_equal(false, df::zip_entry_needs_zip64(0, large_size - 1), "a size with deflate headroom is classic");
	assert_equal(true, df::zip_entry_needs_zip64(0, large_size), "a size that could overflow needs ZIP64");
}

static void should_keep_archive_entries_after_warnings()
{
	const auto source = _temps.next_path(".bin");
	write_test_file(source, "archive content");
	const auto path = _temps.next_path(".zip");
	df::zip_file zip;
	assert_equal(true, zip.create(path), "warning fixture created");
	assert_equal(true, zip.add(source, "first.bin"), "first entry written");
	assert_equal(true, zip.add(source, "second.bin"), "second entry written");
	assert_equal(true, zip.close(), "warning fixture closed");
	auto bytes = df::blob_from_file(path);
	assert_equal(true, bytes.size() > 18, "local header contains CRC");
	bytes[14] = 1;
	bytes[15] = bytes[16] = bytes[17] = 0;
	assert_equal(true, df::blob_save_to_file(bytes, path), "mismatched local CRC stored");

	const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
	assert_equal(true, reader != nullptr, "archive reader allocated");
	archive_read_support_format_zip(reader.get());
	assert_equal(ARCHIVE_OK, archive_read_open_memory(reader.get(), bytes.data(), bytes.size()), "warning fixture opened");
	archive_entry* entry = nullptr;
	assert_equal(ARCHIVE_WARN, archive_read_next_header(reader.get(), &entry), "fixture produces a recoverable header warning");

	const auto listed = files::list_archive(path);
	assert_equal(2_z, listed.size(), "a usable warned entry and its successor both remain visible");
	assert_equal("second.bin", listed.back().filename, "listing continued beyond the warning");
}

static void should_convert_archive_unix_timestamps()
{
	std::array<uint8_t, 2560> bytes{};
	const auto header = [&bytes](const size_t offset, const std::string_view name, const char type, const unsigned long size)
	{
		auto* data = reinterpret_cast<char*>(bytes.data() + offset);
		std::copy(name.begin(), name.end(), data);
		std::memcpy(data + 100, "0000644", 7);
		assert_equal(11, std::snprintf(data + 124, 12, "%011lo", size), "TAR size field fits");
		assert_equal(11, std::snprintf(data + 136, 12, "%011lo", 1704164645ul), "TAR time field fits");
		std::memset(data + 148, ' ', 8);
		data[156] = type;
		std::memcpy(data + 257, "ustar", 5);
		std::memcpy(data + 263, "00", 2);
		const auto checksum = std::accumulate(bytes.begin() + offset, bytes.begin() + offset + 512, 0u);
		assert_equal(6, std::snprintf(data + 148, 7, "%06o", checksum), "TAR checksum field fits");
		data[155] = ' ';
	};
	constexpr std::string_view pax = "28 ctime=1704164645.1234567\n";
	static_assert(pax.size() == 28);
	header(0, "PaxHeader", 'x', static_cast<unsigned long>(pax.size()));
	std::copy(pax.begin(), pax.end(), bytes.begin() + 512);
	header(1024, "dated.bin", '0', 0);
	const auto path = _temps.next_path(".tar");
	assert_equal(true, df::blob_save_to_file({bytes.data(), bytes.size()}, path), "PAX fixture stored");
	const auto listed = files::list_archive(path);
	assert_equal(1_z, listed.size(), "dated archive entry found");
	assert_equal(df::date_t(2024, 1, 2, 3, 4, 5).to_int64() + 1234567,
	             listed.front().created.to_int64(), "Unix epoch, seconds and fractional precision are converted");
}

static void should_round_trip_zip_entry_dates()
{
	const auto source = _temps.next_path(".bin");
	const df::blob payload = {1, 2, 3};
	df::blob_save_to_file(payload, source);

	const std::array dates = {
		df::date_t(2024, 1, 31, 23, 58, 56),
		df::date_t(2024, 12, 31, 1, 2, 4),
		df::date_t(2024, 2, 29, 6, 8, 10),
		df::date_t(1975, 6, 15, 12, 0, 0),
		df::date_t(1601, 1, 1, 0, 0, 0),
	};

	const std::array expected = {
		df::date_t(2024, 1, 31, 23, 58, 56),
		df::date_t(2024, 12, 31, 1, 2, 4),
		df::date_t(2024, 2, 29, 6, 8, 10),
		df::date_t(1980, 1, 1, 0, 0, 0).local_to_system(),
		df::date_t(1980, 1, 1, 0, 0, 0).local_to_system(),
	};

	const auto archive = _temps.next_path(".zip");
	df::zip_file zip;
	assert_equal(true, zip.create(archive), "the date archive is created");

	for (auto i = 0_z; i < dates.size(); ++i)
	{
		assert_equal(true, zip.add(source, std::format("dated-{}.bin", i), dates[i]), "the dated entry is added");
	}

	assert_equal(true, zip.close(), "the date archive closes");

	const auto listed = df::zip_file::list(archive);
	assert_equal(dates.size(), listed.size(), "every dated entry is present");

	for (auto i = 0_z; i < dates.size(); ++i)
	{
		assert_equal(expected[i].date().year, listed[i].created.date().year, "zip date year");
		assert_equal(expected[i].date().month, listed[i].created.date().month, "zip date month");
		assert_equal(expected[i].date().day, listed[i].created.date().day, "zip date day");
		assert_equal(expected[i].date().hour, listed[i].created.date().hour, "zip date hour");
		assert_equal(expected[i].date().minute, listed[i].created.date().minute, "zip date minute");
		assert_equal(expected[i].date().second, listed[i].created.date().second, "zip date second");
	}
}

static void should_create_original_before_replace()
{
	const auto destination = _temps.next_path(".jpg");
	const auto replacement = _temps.next_path(".jpg");
	const df::blob original = {1, 2, 3};
	const df::blob updated = {4, 5, 6};
	df::blob_save_to_file(original, destination);
	df::blob_save_to_file(updated, replacement);

	const auto result = platform::replace_file(destination, replacement, true);
	const auto original_path = df::file_path(destination.folder(),
	                                         std::string(destination.file_name_without_extension()) + ".original",
	                                         destination.extension());
	assert_equal(true, result.success(), "replacement with backup succeeds");
	assert_equal(true, original_path.exists(), "original backup exists");
	assert_equal(true, df::blob_from_file(original_path) == original, "backup contains original bytes");
	assert_equal(true, result.coherent_handle != nullptr, "replacement returns coherent handle");
	df::blob actual(updated.size());
	result.coherent_handle->seek(0, platform::file::whence::begin);
	actual.resize(static_cast<size_t>(result.coherent_handle->read(actual.data(), actual.size())));
	assert_equal(true, actual == updated, "destination contains updated bytes");
	platform::delete_file(original_path);
}

static void should_report_move_or_copy_collision_paths()
{
	const auto root = _temps.folder().combine(std::format("move-copy-{}", platform::tick_count()));
	const auto source = root.combine("source");
	const auto target = root.combine("target");
	const auto source_folder = source.combine("album");
	const auto occupied_folder = target.combine("album");
	platform::create_folder(source_folder);
	platform::create_folder(occupied_folder);

	const auto source_file = source.combine_file("photo.txt");
	const auto occupied_file = target.combine_file("photo.txt");
	const df::blob contents = {1};
	df::blob_save_to_file(contents, source_file);
	df::blob_save_to_file(contents, occupied_file);
	df::blob_save_to_file(contents, source_folder.combine_file("inside.txt"));

	const auto result = platform::move_or_copy({source_file}, {source_folder}, target, false);
	assert_equal(true, result.success(), "copy with collisions succeeds");
	assert_equal(uint64_t{1}, static_cast<uint64_t>(result.created_files.files.size()), "one created file reported");
	assert_equal(uint64_t{1}, static_cast<uint64_t>(result.created_files.folders.size()), "one created folder reported");
	assert_equal(true, result.created_files.files.front() != occupied_file, "renamed file path reported");
	assert_equal(true, result.created_files.folders.front() != occupied_folder, "renamed folder path reported");
	assert_equal(true, result.created_files.files.front().exists(), "reported file exists");
	assert_equal(true, result.created_files.folders.front().exists(), "reported folder exists");
	platform::delete_items({}, {root}, false);
}

#ifndef _WIN32
static void should_report_linux_file_operation_errors()
{
	const platform::file_op_result collision{platform::file_op_result_code::ALREADY_EXISTS};
	const platform::file_op_result failure{platform::file_op_result_code::FAILED};
	assert_equal(true, collision.failed(), "an existing destination is a failure");
	assert_equal(false, collision.format_error().empty(), "a collision cannot look like an empty success message");
	assert_equal(false, failure.format_error().empty(), "an unspecified failure still has a diagnostic");
}

static void should_reject_linux_folder_file_collisions()
{
	const auto root = _temps.next_folder("linux-folder-collision");
	const auto file = root.combine_file("occupied");
	write_test_file(file, "not a directory");

	assert_equal(true, platform::create_folder(root.combine("occupied")).failed(),
	             "an existing file is not a successful mkdir");
	assert_equal("not a directory", read_test_file(file), "the colliding file is unchanged");

	const auto link = root.combine("dangling");
	std::filesystem::create_directory_symlink(platform::to_stream_path(root.combine("missing")),
	                                           platform::to_stream_path(link));
	assert_equal(true, platform::create_folder(link).failed(), "a dangling symlink is not a directory");
	assert_equal(true, platform::create_folder(root.combine("directory")).success(), "a new directory is created");
	assert_equal(true, platform::create_folder(root.combine("directory")).success(), "an existing directory is accepted");
}

static void should_preserve_linux_directory_move_on_unsupported_no_replace()
{
	const auto root = _temps.next_folder("linux-directory-move");
	const auto source = root.combine("source");
	const auto destination = root.combine("destination");
	assert_equal(true, platform::create_folder(source).success(), "source directory exists");
	write_test_file(source.combine_file("original.txt"), "original");

	platform::test_linux_move_file_failures({true, false});
	const df::scope_exit restore([] { platform::test_linux_clear_move_file_failures(); });
	const auto result = platform::move_file(source, destination);
	assert_equal(true, result.failed(), "unsupported atomic directory rename is a reported failure");
	assert_equal("original", read_test_file(source.combine_file("original.txt")), "source is retained");
	assert_equal(false, destination.exists(), "an unsafe fallback did not publish a destination");
}

static void should_refuse_linux_copy_identity_overwrites()
{
	const auto root = _temps.next_folder("linux-copy-identity");
	const auto source = root.combine_file("source.bin");
	const auto alias = root.combine_file("alias.bin");
	const df::blob bytes = {1, 2, 3, 4};
	df::blob_save_to_file(bytes, source);
	std::filesystem::create_hard_link(platform::to_stream_path(source), platform::to_stream_path(alias));

	const auto same_path = platform::copy_file(source, source, false, false);
	assert_equal(true, same_path.failed(), "copy to self fails");
	assert_equal(true, df::blob_from_file(source) == bytes, "copy to self preserves source");

	const auto hard_link = platform::copy_file(source, alias, false, false);
	assert_equal(true, hard_link.failed(), "copy to hard-link alias fails");
	assert_equal(true, df::blob_from_file(source) == bytes, "hard-link source bytes survive");
	assert_equal(true, df::blob_from_file(alias) == bytes, "hard-link destination bytes survive");
}

static void should_report_linux_copy_flush_failures()
{
	const auto root = _temps.next_folder("linux-copy-flush");
	const auto source = root.combine_file("source.bin");
	const auto destination = root.combine_file("destination.bin");
	const df::blob source_bytes = {9, 8, 7, 6, 5, 4};
	const df::blob original_destination = {1, 2, 3};
	df::blob_save_to_file(source_bytes, source);
	df::blob_save_to_file(original_destination, destination);

	platform::linux_copy_file_test_failures partial;
	partial.max_write_bytes = 2;
	partial.interrupt_first_write = true;
	platform::test_linux_copy_file_failures(partial);
	const auto partial_result = platform::copy_file(source, root.combine_file("partial.bin"), true, false);
	platform::test_linux_clear_copy_file_failures();
	assert_equal(true, partial_result.success(), "partial and interrupted writes complete");
	assert_equal(true, df::blob_from_file(root.combine_file("partial.bin")) == source_bytes,
	             "partial write copy has every byte");

	platform::linux_copy_file_test_failures write_failure;
	write_failure.fail_write_after_calls = 0;
	platform::test_linux_copy_file_failures(write_failure);
	const auto write_result = platform::copy_file(source, destination, false, false);
	platform::test_linux_clear_copy_file_failures();
	assert_equal(true, write_result.failed(), "write failure is reported");
	assert_equal(true, df::blob_from_file(destination) == original_destination,
	             "write failure preserves prior destination");

	platform::linux_copy_file_test_failures close_failure;
	close_failure.fail_destination_close = true;
	platform::test_linux_copy_file_failures(close_failure);
	const auto close_result = platform::copy_file(source, destination, false, false);
	platform::test_linux_clear_copy_file_failures();
	assert_equal(true, close_result.failed(), "late close failure is reported");
	assert_equal(true, df::blob_from_file(destination) == original_destination,
	             "late close failure preserves prior destination");
}

static void should_claim_linux_auto_rename_destinations_atomically()
{
	const auto root = _temps.next_folder("linux-auto-rename");
	const auto source_folder = root.combine("source");
	const auto target = root.combine("target");
	platform::create_folder(source_folder);
	platform::create_folder(target);

	const auto source = source_folder.combine_file("photo.txt");
	const df::blob source_bytes = {7};
	const df::blob competing_bytes = {3};
	df::blob_save_to_file(source_bytes, source);

	bool competed = false;
	platform::test_linux_before_claim_file_path = [&](const df::file_path candidate)
	{
		if (!competed)
		{
			competed = true;
			df::blob_save_to_file(competing_bytes, candidate);
		}
	};

	const auto result = platform::move_or_copy({source}, {}, target, false);
	platform::test_linux_before_claim_file_path = {};

	assert_equal(true, result.success(), "copy retries after a raced file candidate");
	assert_equal(1_z, result.created_files.files.size(), "one copied file is reported");
	assert_equal(true, result.created_files.files.front() != target.combine_file("photo.txt"),
	             "the raced name was not reported as copied");
	assert_equal(true, df::blob_from_file(target.combine_file("photo.txt")) == competing_bytes,
	             "competing file bytes survive");
	assert_equal(true, df::blob_from_file(result.created_files.files.front()) == source_bytes,
	             "reported file has source bytes");

	const auto replace_source = source_folder.combine_file("replace.txt");
	const auto replace_target = target.combine_file("replace.txt");
	df::blob_save_to_file(source_bytes, replace_source);
	df::blob_save_to_file(competing_bytes, replace_target);
	const auto replace = platform::move_or_copy({replace_source}, {}, target, false, true);
	assert_equal(true, replace.success(), "explicit replace still succeeds");
	assert_equal(true, df::blob_from_file(replace_target) == source_bytes, "replace writes source bytes");
}

static void should_reject_linux_recursive_copy_into_descendant()
{
	const auto root = _temps.next_folder("linux-descendant-copy");
	const auto source = root.combine("source");
	const auto child = source.combine("child");
	platform::create_folder(child);
	df::blob_save_to_file(df::blob{1}, source.combine_file("photo.txt"));

	const auto result = platform::move_or_copy({}, {source}, child, false);
	assert_equal(true, result.failed(), "copy into descendant fails");
	assert_equal(false, platform::exists(child.combine("source")), "descendant target is not created");
	assert_equal(true, platform::exists(source.combine_file("photo.txt")), "source remains available");
}

static void should_delete_linux_directory_symlinks()
{
	const auto root = _temps.next_folder("linux-delete-symlink");
	const auto source = root.combine("source");
	const auto target = root.combine("target");
	const auto link = source.combine("linked");
	platform::create_folder(source);
	platform::create_folder(target);
	df::blob_save_to_file(df::blob{1}, source.combine_file("file.txt"));
	df::blob_save_to_file(df::blob{2}, target.combine_file("target.txt"));
	std::filesystem::create_directory_symlink(platform::to_stream_path(target), platform::to_stream_path(link));

	const auto result = platform::delete_items({}, {source}, false);
	assert_equal(true, result.success(), std::format("delete succeeds ({})", result.format_error()));
	assert_equal(false, platform::exists(source), "source folder is removed");
	assert_equal(true, platform::exists(target.combine_file("target.txt")), "symlink target survives");
}

static void should_move_or_copy_linux_directory_symlinks()
{
	const auto root = _temps.next_folder("linux-copy-symlink");
	const auto source = root.combine("source");
	const auto target = root.combine("target");
	const auto link_target = root.combine("linked-target");
	platform::create_folder(source);
	platform::create_folder(target);
	platform::create_folder(link_target);
	std::filesystem::create_directory_symlink(platform::to_stream_path(link_target),
	                                          platform::to_stream_path(source.combine("linked")));

	const auto result = platform::move_or_copy({}, {source}, target, false);
	assert_equal(true, result.success(), std::format("copy succeeds ({})", result.format_error()));
	assert_equal(1_z, result.created_files.folders.size(), "one folder reported");
	const auto copied_link = result.created_files.folders.front().combine("linked");
	assert_equal(true, std::filesystem::is_symlink(platform::to_stream_path(copied_link)),
	             "directory symlink is recreated");
	assert_equal(platform::to_stream_path(link_target).string(),
	             std::filesystem::read_symlink(platform::to_stream_path(copied_link)).string(),
	             "link target spelling is preserved");
}

static void should_move_or_copy_into_its_own_folder_with_auto_rename()
{
	const auto root = _temps.next_folder("linux-copy-self-parent");
	const auto file = root.combine_file("photo.txt");
	const auto folder = root.combine("album");
	df::blob_save_to_file(df::blob{7}, file);
	platform::create_folder(folder);
	df::blob_save_to_file(df::blob{8}, folder.combine_file("inside.txt"));

	const auto file_result = platform::move_or_copy({file}, {}, root, false);
	assert_equal(true, file_result.success(), std::format("file copy succeeds ({})", file_result.format_error()));
	assert_equal(1_z, file_result.created_files.files.size(), "one copied file reported");
	assert_equal(true, file_result.created_files.files.front() != file, "copy uses a new file name");
	assert_equal(true, df::blob_from_file(file_result.created_files.files.front()) == df::blob{7},
	             "copied file has the source bytes");

	const auto folder_result = platform::move_or_copy({}, {folder}, root, false);
	assert_equal(true, folder_result.success(), std::format("folder copy succeeds ({})", folder_result.format_error()));
	assert_equal(1_z, folder_result.created_files.folders.size(), "one copied folder reported");
	assert_equal(true, folder_result.created_files.folders.front() != folder, "copy uses a new folder name");
	assert_equal(true, platform::exists(folder_result.created_files.folders.front().combine_file("inside.txt")),
	             "copied folder has the source contents");
}

static void should_move_or_copy_stop_on_inner_linux_folder_collision()
{
	const auto root = _temps.next_folder("linux-inner-collision");
	const auto source = root.combine("album");
	const auto target = root.combine("target");
	platform::create_folder(source);
	platform::create_folder(target);
	df::blob_save_to_file(df::blob{5}, source.combine_file("inside.txt"));

	bool competed = false;
	platform::test_linux_before_copy_folder_file_path = [&](const df::file_path candidate)
	{
		if (!competed)
		{
			competed = true;
			df::blob_save_to_file(df::blob{6}, candidate);
		}
	};

	const auto result = platform::move_or_copy({}, {source}, target, false);
	platform::test_linux_before_copy_folder_file_path = {};

	assert_equal(true, result.failed(), "inner collision fails the claimed tree");
	assert_equal(false, platform::exists(target.combine("album (2)")), "inner collision does not retry the top folder");
	assert_equal(true, df::blob_from_file(target.combine("album").combine_file("inside.txt")) == df::blob{6},
	             "competing inner file survives");
}

static void should_move_linux_no_replace_without_renameat_or_links()
{
	const auto root = _temps.next_folder("linux-move-fallback");
	const auto source = root.combine_file("source.txt");
	const auto destination = root.combine_file("destination.txt");
	const df::blob bytes = {4, 3, 2};
	df::blob_save_to_file(bytes, source);

	platform::test_linux_move_file_failures({true, true});
	const auto result = platform::move_file(source, destination, true);
	platform::test_linux_clear_move_file_failures();

	assert_equal(true, result.success(), std::format("fallback move succeeds ({})", result.format_error()));
	assert_equal(false, platform::exists(source), "source is removed after the exclusive copy");
	assert_equal(true, df::blob_from_file(destination) == bytes, "destination has source bytes");
}

static void should_refuse_or_preserve_linux_replace_permissions()
{
	const auto root = _temps.next_folder("linux-copy-permissions");
	const auto source = root.combine_file("source.txt");
	const auto read_only = root.combine_file("read-only.txt");
	const auto private_file = root.combine_file("private.txt");
	const df::blob source_bytes = {9};
	const df::blob original_bytes = {1};
	df::blob_save_to_file(source_bytes, source);
	df::blob_save_to_file(original_bytes, read_only);
	df::blob_save_to_file(original_bytes, private_file);

	namespace fs = std::filesystem;
	fs::permissions(platform::to_stream_path(read_only),
	                fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
	                fs::perm_options::replace);
	const auto refused = platform::copy_file(source, read_only, false, false);
	assert_equal(true, refused.failed(), "read-only destination refuses replace");
	assert_equal(true, df::blob_from_file(read_only) == original_bytes, "read-only destination bytes survive");

	fs::permissions(platform::to_stream_path(private_file), fs::perms::owner_read | fs::perms::owner_write,
	                fs::perm_options::replace);
	const auto replaced = platform::copy_file(source, private_file, false, false);
	assert_equal(true, replaced.success(), std::format("replace succeeds ({})", replaced.format_error()));
	const auto mode = fs::status(platform::to_stream_path(private_file)).permissions() & fs::perms::all;
	assert_equal(true, mode == (fs::perms::owner_read | fs::perms::owner_write),
	             "replace preserves destination permissions");
}

static void should_move_or_copy_reject_linux_destination_alias_inside_source()
{
	const auto root = _temps.next_folder("linux-copy-alias-descendant");
	const auto source = root.combine("source");
	const auto nested = source.combine("a").combine("b");
	const auto target = root.combine("target-link");
	platform::create_folder(nested);
	df::blob_save_to_file(df::blob{1}, source.combine_file("top.txt"));
	df::blob_save_to_file(df::blob{2}, source.combine("a").combine_file("mid.txt"));
	std::filesystem::create_directory_symlink(platform::to_stream_path(nested), platform::to_stream_path(target));

	const auto result = platform::move_or_copy({}, {source}, target, false);
	assert_equal(true, result.failed(), "alias into source is rejected");
	assert_equal(false, platform::exists(nested.combine("source")), "copy does not create output inside source");
	assert_equal(true, platform::exists(source.combine_file("top.txt")), "source remains available");
}
#endif

static void should_fail_replace_when_flush_fails()
{
	const auto result = platform::replacement_flush_result(false, "flush failed");
	assert_equal(true, result.failed(), "failed flush stops replacement");
	assert_equal("flush failed", result.error_message, "flush error preserved");

#ifdef _WIN32
	// Holding the replacement open is what makes the flush fail, and that is Windows' mandatory
	// locking. A POSIX open descriptor denies nothing, so there is no failure here to observe.
	const auto destination = _temps.next_path(".bin");
	const auto replacement = _temps.next_path(".bin");
	const df::blob original = {1, 2, 3};
	const df::blob updated = {4, 5, 6};
	df::blob_save_to_file(original, destination);
	df::blob_save_to_file(updated, replacement);
	auto locked = platform::open_file(replacement, platform::file_open_mode::read_write);
	assert_equal(true, locked != nullptr, "replacement locked");

	const auto real_result = platform::replace_file(destination, replacement);
	locked.reset();
	assert_equal(true, real_result.failed(), "real flush failure stops replacement");
	assert_equal(true, !real_result.error_message.empty(), "real flush error reported");
	assert_equal(true, df::blob_from_file(destination) == original, "destination unchanged after flush failure");
#endif
}

#ifdef _WIN32
// The failure this cleans up after is induced by holding the destination open, which denies the
// write only where locking is mandatory. POSIX has no equivalent, so the failure cannot be staged
// here and the cleanup it proves has no way to be observed.
static void should_cleanup_failed_update_temps()
{
	const auto src_path = df::file_path(test_files_folder, "Test.jpg");
	// A private folder: enumerating the shared suite temp folder would scan every file every other
	// test has left there.
	const auto scratch = _temps.next_folder("update-temps");
	const auto destination = _temps.next_path_in(scratch, ".jpg");
	platform::copy_file(src_path, destination, false, false);
	std::vector<str::cached> files_before;
	for (const auto& file : platform::iterate_file_items(scratch, false).files)
	{
		files_before.emplace_back(file.name);
	}

	auto locked = platform::open_file(destination, platform::file_open_mode::read_write);
	assert_equal(true, locked != nullptr, "destination locked");

	metadata_edits edits;
	edits.rating = 3;
	files ff;
	const auto result = ff.update(destination, edits, {}, {}, false, {});
	locked.reset();

	assert_equal(true, result.failed(), "locked update fails");
	const auto contents = platform::iterate_file_items(scratch, false);
	const auto leaked = std::ranges::any_of(contents.files, [&files_before](const platform::file_info& file)
	{
		return std::ranges::find(files_before, file.name) == files_before.end() &&
			str::starts(file.name, "diffractor_");
	});
	assert_equal(false, leaked, "failed update removes temporary files");
}
#endif

static void should_settle_transport_stream_extension_by_header()
{
	assert_equal(true, files::has_media_header_rule(".ts"), "the transport stream extension has a header rule");
	assert_equal(true, files::has_media_header_rule("m2ts"), "the rule ignores a leading dot");
	assert_equal(false, files::has_media_header_rule(".mp4"), "an unambiguous extension is left to the decoder");

	std::array<uint8_t, files::media_header_probe_bytes> header{};

	const auto write_packets = [&header](const size_t start, const size_t packet_size)
	{
		header.fill(0);
		for (auto i = size_t{0}; i < 4; ++i) header[start + i * packet_size] = 0x47;
	};

	write_packets(0, 188);
	assert_equal(true, files::media_header_matches(".ts", {header.data(), header.size()}),
	             "a broadcast packet run is accepted");

	write_packets(4, 192);
	assert_equal(true, files::media_header_matches(".m2ts", {header.data(), header.size()}),
	             "an M2TS timestamp prefix is accepted");

	write_packets(0, 204);
	assert_equal(true, files::media_header_matches(".ts", {header.data(), header.size()}),
	             "a Reed-Solomon packet run is accepted");

	// A capture that begins mid-packet still aligns further in, and ffmpeg would find it, so refusing
	// it here would hide real video.
	write_packets(97, 188);
	assert_equal(true, files::media_header_matches(".ts", {header.data(), header.size()}),
	             "a stream that starts mid-packet is accepted");

	// A TypeScript file that opens with 'G' (0x47) matches the sync byte but not the packet run.
	const std::string_view typescript = "Get the exported type before anything else is imported;\n";
	header.fill(0);
	std::memcpy(header.data(), typescript.data(), typescript.size());
	assert_equal(false, files::media_header_matches(".ts", {header.data(), typescript.size()}),
	             "TypeScript source is not mistaken for a transport stream");

	assert_equal(true, files::media_header_matches(".mp4", {header.data(), typescript.size()}),
	             "an extension with no rule always matches");
}

static df::blob make_test_xmp_packet(const std::string_view body)
{
	const auto xml = std::format(
		"<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
		"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
		"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
		"<rdf:Description rdf:about=\"\" "
		"xmlns:dc=\"http://purl.org/dc/elements/1.1/\">{}</rdf:Description>"
		"</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>",
		body);
	return {std::bit_cast<const uint8_t*>(xml.data()), std::bit_cast<const uint8_t*>(xml.data() + xml.size())};
}

static ui::surface_ptr make_gradient_surface(int cx, int cy);

static void should_fail_webp_save_when_metadata_chunk_insertion_fails()
{
	files ff;
	const auto surface = make_gradient_surface(16, 16);
	file_encode_params params;
	params.webp_lossless = true;

	const auto assert_failed_chunk = [&](const std::string_view fourcc, const metadata_parts& metadata,
	                                     const ui::const_surface_ptr& source)
	{
		files_test_hooks::fail_next_webp_chunk(fourcc);
		const auto saved = save_webp(source, metadata, params);
		assert_equal(false, is_valid(saved), std::format("{} insertion failure fails the save", fourcc));
	};

	metadata_parts icc;
	icc.icc = {1, 2, 3, 4};
	assert_failed_chunk("ICCP", icc, surface);

	metadata_parts exif;
	exif.exif = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 0x2a, 0};
	assert_failed_chunk("EXIF", exif, surface);

	metadata_parts orientation_metadata;
	const auto rotated = std::make_shared<ui::surface>();
	rotated->copy(*surface, surface->dimensions());
	rotated->orientation(ui::orientation::right_top);
	assert_failed_chunk("EXIF", orientation_metadata, rotated);

	metadata_parts xmp;
	xmp.xmp = make_test_xmp_packet(
		"<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">webp</rdf:li></rdf:Alt></dc:title>");
	assert_failed_chunk("XMP ", xmp, surface);
}

static void should_scan_d64()
{
	constexpr auto file_name = "Ace of Aces (Europe).D64";
	const auto load_path = test_files_folder.combine("retro").combine_file(file_name);
	const auto loaded = df::blob_from_file(load_path);
	const auto contents = libfiles::list_c64_media({loaded.data(), loaded.size()});

	assert_equal(4_z, contents.rows.size(), "d64", file_name);
	assert_equal("147 \"ACE OF ACES+    \" PRG", contents.rows[0].text, "d64", file_name);
}

static void should_scan_archive()
{
	constexpr auto file_name = "benchmarks.zip";
	const auto load_path = test_files_folder.combine_file(file_name);
	const auto contents = files::list_archive(load_path);

	assert_equal(2_z, contents.size(), "archive", file_name);
	assert_equal("PXL_20240404_074316577.jpg", contents[0].filename, "archive", file_name);
}

static void should_detect_tiff_by_version()
{
	// The byte-order mark alone is shared with plenty of non-TIFF files, so the
	// version word decides: 42 is classic TIFF and 43 is BigTIFF.
	constexpr uint8_t little_endian_42[] = {'I', 'I', 42, 0, 8, 0, 0, 0};
	constexpr uint8_t big_endian_42[] = {'M', 'M', 0, 42, 0, 0, 0, 8};
	constexpr uint8_t big_endian_43[] = {'M', 'M', 0, 43, 0, 8, 0, 0};
	constexpr uint8_t not_tiff[] = {'I', 'I', 'B', 'M', 0, 0, 0, 0};

	assert_equal(true, files::detect_format({little_endian_42, std::size(little_endian_42)}) ==
	             detected_format::TIFF, "little endian tiff");
	assert_equal(true, files::detect_format({big_endian_42, std::size(big_endian_42)}) ==
	             detected_format::TIFF, "big endian tiff");
	assert_equal(true, files::detect_format({big_endian_43, std::size(big_endian_43)}) ==
	             detected_format::TIFF, "big endian bigtiff");
	assert_equal(true, files::detect_format({not_tiff, std::size(not_tiff)}) ==
	             detected_format::Unknown, "not tiff");
}

// Each still format is one entry in files_formats.cpp. Its file type still has to take its place in
// the extension table, and libfiles has to name it from its signature - a format added to one and not
// the others is what this exists to catch.
static void should_register_every_still_format()
{
	const std::map<detected_format, std::vector<uint8_t>> signatures = {
		{detected_format::PSD, {'8', 'B', 'P', 'S'}},
		{detected_format::GIF, {'G', 'I', 'F', '8', '9', 'a'}},
		{detected_format::JXL, {0xFF, 0x0A, 0x00, 0x00}},
		{detected_format::WEBP, {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'}},
		{detected_format::JPEG, {0xFF, 0xD8, 0xFF, 0xE0}},
		{detected_format::BMP, {'B', 'M', 0, 0}},
		{detected_format::PNG, {0x89, 'P', 'N', 'G'}},
		{detected_format::TIFF, {'I', 'I', 42, 0}},
		{detected_format::HEIF, {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'}},
	};
	std::set<detected_format> seen;

	for (const auto& still : still_formats())
	{
		const auto name = std::string(str::split(still.extensions, true).front());
		assert_equal(true, seen.insert(still.format).second, std::format("{} has one entry", name));
		const auto signature = signatures.find(still.format);
		assert_equal(true, signature != signatures.end() &&
		             files::detect_format({signature->second.data(), signature->second.size()}) == still.format,
		             std::format("{} is detected", name));
		assert_equal(true, still.scan != nullptr, std::format("{} is scanned", name));
		assert_equal(true, still.encoded == ui::image_format::Unknown || still.decode != nullptr,
		             std::format("{} kept encoded can still be decoded", name));

		for (const auto ext : str::split(still.extensions, true))
		{
			const auto* const ft = files::file_type_from_name(std::format("x.{}", ext));
			assert_equal(true, ft->extension == still.extensions && ft->text == still.description &&
			             ft->traits == still.traits, std::format("{} registers as its still format", ext));
		}
	}

	assert_equal(true, find_still_format(detected_format::Unknown) == nullptr, "unknown is not a format");
}

static void should_scan_and_load_bitmap_psd()
{
	// A minimal uncompressed 1-bit-per-pixel bitmap-mode psd. Photoshop stores
	// bitmap mode inverted, so a set bit is black and a clear bit is white.
	constexpr uint8_t bitmap_psd[] = {
		'8', 'B', 'P', 'S', 0, 1, // signature and version
		0, 0, 0, 0, 0, 0, // reserved
		0, 1, // channels
		0, 0, 0, 2, // rows
		0, 0, 0, 16, // columns
		0, 1, // depth
		0, 0, // mode - bitmap
		0, 0, 0, 0, // colour mode data length
		0, 0, 0, 0, // image resource length
		0, 0, 0, 0, // layer and mask length
		0, 0, // compression - none
		0b1010'1010, 0b0000'1111, // row 0
		0b0000'0000, 0b1111'1111, // row 1
	};

	mem_read_stream scan_stream({bitmap_psd, std::size(bitmap_psd)});
	const auto scanned = scan_photo(scan_stream);

	assert_equal(true, scanned.success, "bitmap psd scanned");
	assert_equal(16u, scanned.width, "bitmap psd width");
	assert_equal(2u, scanned.height, "bitmap psd height");
	assert_equal("mono"_c, scanned.pixel_format, "bitmap psd pixel format");

	mem_read_stream load_stream({bitmap_psd, std::size(bitmap_psd)});
	const auto surface = load_psd(load_stream);

	assert_equal(true, is_valid(surface), "bitmap psd loaded");
	assert_equal(16, static_cast<int>(surface->width()), "bitmap psd surface width");
	assert_equal(2, static_cast<int>(surface->height()), "bitmap psd surface height");

	const auto* const row0 = std::bit_cast<const uint32_t*>(surface->pixels_line(0));
	const auto* const row1 = std::bit_cast<const uint32_t*>(surface->pixels_line(1));

	constexpr uint32_t black = 0x000000;
	constexpr uint32_t white = 0xFFFFFF;
	const auto rgb = [](const uint32_t pixel) { return pixel & 0xFFFFFF; };

	assert_equal(black, rgb(row0[0]), "bitmap psd 0,0 is black");
	assert_equal(white, rgb(row0[1]), "bitmap psd 1,0 is white");
	assert_equal(white, rgb(row0[8]), "bitmap psd 8,0 is white");
	assert_equal(black, rgb(row0[12]), "bitmap psd 12,0 is black");
	assert_equal(white, rgb(row1[0]), "bitmap psd 0,1 is white");
	assert_equal(black, rgb(row1[15]), "bitmap psd 15,1 is black");
}

static void append_be16(std::vector<uint8_t>& b, const uint16_t v)
{
	b.push_back(static_cast<uint8_t>(v >> 8));
	b.push_back(static_cast<uint8_t>(v));
}

static void append_be32(std::vector<uint8_t>& b, const uint32_t v)
{
	b.push_back(static_cast<uint8_t>(v >> 24));
	b.push_back(static_cast<uint8_t>(v >> 16));
	b.push_back(static_cast<uint8_t>(v >> 8));
	b.push_back(static_cast<uint8_t>(v));
}

static void append_photoshop_resource(std::vector<uint8_t>& out, uint16_t id, std::string_view name,
                                      df::cspan payload);

static df::blob make_lab_psd(const std::vector<uint8_t>& l, const std::vector<uint8_t>& a,
                             const std::vector<uint8_t>& b)
{
	std::vector<uint8_t> psd;
	psd.insert(psd.end(), {'8', 'B', 'P', 'S'});
	append_be16(psd, 1);
	psd.insert(psd.end(), 6, 0);
	append_be16(psd, 3);
	append_be32(psd, 1);
	append_be32(psd, static_cast<uint32_t>(l.size()));
	append_be16(psd, 8);
	append_be16(psd, 9);
	append_be32(psd, 0);
	append_be32(psd, 0);
	append_be32(psd, 0);
	append_be16(psd, 0);
	psd.insert(psd.end(), l.begin(), l.end());
	psd.insert(psd.end(), a.begin(), a.end());
	psd.insert(psd.end(), b.begin(), b.end());
	return {psd.begin(), psd.end()};
}

static void should_convert_psd_lab_samples_to_lab_coordinates()
{
	const auto psd = make_lab_psd({0, 64, 128, 191, 255}, {128, 128, 128, 128, 128}, {128, 128, 128, 128, 128});
	mem_read_stream stream(psd);
	const auto surface = load_psd(stream);

	assert_equal(true, ui::is_valid(surface), "Lab PSD loaded");

	const auto black = surface->get_pixel(0, 0);
	const auto gray = surface->get_pixel(1, 0);
	const auto mid25 = surface->get_pixel(1, 0);
	const auto mid50 = surface->get_pixel(2, 0);
	const auto mid75 = surface->get_pixel(3, 0);
	const auto white = surface->get_pixel(4, 0);

	assert_equal(true, ui::get_r(black) < 4 && ui::get_g(black) < 4 && ui::get_b(black) < 4,
	             "Lab black is black");
	assert_equal(true, std::abs(static_cast<int>(ui::get_r(mid50)) - static_cast<int>(ui::get_g(mid50))) <= 2 &&
	             std::abs(static_cast<int>(ui::get_g(mid50)) - static_cast<int>(ui::get_b(mid50))) <= 2,
	             "Lab neutral gray is neutral");
	assert_near(59.0, static_cast<double>(ui::get_r(mid25)), 1.0, "Lab L25 sRGB");
	assert_near(119.0, static_cast<double>(ui::get_r(mid50)), 1.0, "Lab L50 sRGB");
	assert_near(185.0, static_cast<double>(ui::get_r(mid75)), 1.0, "Lab L75 sRGB");
	assert_equal(true, ui::get_r(white) > 250 && ui::get_g(white) > 250 && ui::get_b(white) > 250,
	             "Lab white is white");
}

class counting_read_stream final : public read_stream
{
	df::blob _prefix;

public:
	size_t max_read = 0;

	counting_read_stream(df::blob prefix, const uint64_t size) : _prefix(std::move(prefix))
	{
		_file_size = size;
	}

	template <typename T>
	T peek(const uint64_t pos)
	{
		check_range(pos, sizeof(T));
		T result{};
		if (pos < _prefix.size())
		{
			const auto copied = std::min<uint64_t>(sizeof(T), _prefix.size() - pos);
			memcpy(&result, _prefix.data() + static_cast<size_t>(pos), static_cast<size_t>(copied));
		}
		return result;
	}

	uint8_t peek8(const uint64_t pos) override { return peek<uint8_t>(pos); }
	uint16_t peek16(const uint64_t pos) override { return peek<uint16_t>(pos); }
	uint32_t peek32(const uint64_t pos) override { return peek<uint32_t>(pos); }
	uint64_t peek64(const uint64_t pos) override { return peek<uint64_t>(pos); }
	pack128 peek128(const uint64_t pos) override { return peek<pack128>(pos); }
	df::blob read_all() override { return {}; }

	df::blob read(const uint64_t pos, const size_t len) override
	{
		df::blob result(len);
		read(pos, result.data(), len);
		return result;
	}

	void read(const uint64_t pos, uint8_t* buffer, const size_t len) override
	{
		check_range(pos, len);
		memset(buffer, 0, len);
		if (pos < _prefix.size())
		{
			const auto copied = std::min<uint64_t>(len, _prefix.size() - pos);
			memcpy(buffer, _prefix.data() + static_cast<size_t>(pos), static_cast<size_t>(copied));
		}
		max_read = std::max(max_read, len);
	}
};

static df::blob make_psd_prefix(const uint16_t channels, const uint32_t rows, const uint32_t columns,
                                const uint16_t depth, const uint16_t mode, const uint32_t color_len,
                                const uint32_t resource_len)
{
	std::vector<uint8_t> psd;
	psd.insert(psd.end(), {'8', 'B', 'P', 'S'});
	append_be16(psd, 1);
	psd.insert(psd.end(), 6, 0);
	append_be16(psd, channels);
	append_be32(psd, rows);
	append_be32(psd, columns);
	append_be16(psd, depth);
	append_be16(psd, mode);
	append_be32(psd, color_len);
	append_be32(psd, resource_len);
	return {psd.begin(), psd.end()};
}

static void should_bound_psd_resource_allocations()
{
	const auto prefix = make_psd_prefix(3, 1, 1, 8, 3, 0, 16u * 1024u * 1024u + 1u);
	counting_read_stream stream(prefix.clone(), prefix.size() + 16u * 1024u * 1024u + 1u);
	const auto scanned = scan_photo(stream);

	assert_equal(true, scanned.success, "oversized resource section is a bounded scan");
	assert_equal(true, scanned.metadata.iptc.empty(), "oversized resource is not published");
	assert_equal(true, stream.max_read < df::two_fifty_six_k, "oversized resource bytes are skipped, not allocated");
}

static void should_read_small_psd_metadata_after_large_unused_resource()
{
	const df::blob exif = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 0x2a, 0};
	std::vector<uint8_t> resources;
	append_photoshop_resource(resources, 0x0422, "", exif);
	const df::blob large_unused(16u * 1024u * 1024u, 0);
	append_photoshop_resource(resources, 0x03ed, "", large_unused);

	const auto prefix = make_psd_prefix(3, 1, 1, 8, 3, 0, static_cast<uint32_t>(resources.size()));
	auto data = prefix.clone();
	data.insert(data.end(), resources.begin(), resources.end());
	counting_read_stream stream(std::move(data), prefix.size() + resources.size());
	const auto scanned = scan_photo(stream);

	assert_equal(true, scanned.success, "large unused resource section scans");
	assert_equal(true, scanned.metadata.exif == exif, "small EXIF resource is still read");
	assert_equal(true, stream.max_read < df::two_fifty_six_k, "unused large resource is skipped");
}

static void should_load_valid_psd_indexed_palette()
{
	std::vector<uint8_t> psd;
	psd.insert(psd.end(), {'8', 'B', 'P', 'S'});
	append_be16(psd, 1);
	psd.insert(psd.end(), 6, 0);
	append_be16(psd, 1);
	append_be32(psd, 1);
	append_be32(psd, 1);
	append_be16(psd, 8);
	append_be16(psd, 2);
	append_be32(psd, 768);
	std::array<uint8_t, 768> palette = {};
	palette[7] = 10;
	palette[256 + 7] = 20;
	palette[512 + 7] = 30;
	psd.insert(psd.end(), palette.begin(), palette.end());
	append_be32(psd, 0); // resources length
	append_be32(psd, 0); // layer and mask length
	append_be16(psd, 0); // raw compression
	psd.push_back(7); // palette index

	const df::blob bytes{psd.begin(), psd.end()};
	mem_read_stream stream(bytes);
	const auto surface = load_psd(stream);
	assert_equal(true, ui::is_valid(surface), "indexed psd loaded");
	const auto c = surface->get_pixel(0, 0);
	assert_equal(30u, ui::get_r(c), "palette red");
	assert_equal(20u, ui::get_g(c), "palette green");
	assert_equal(10u, ui::get_b(c), "palette blue");
}

static void should_keep_dimensions_from_truncated_gif()
{
	// A valid GIF89a header followed by an application extension declaring an 11-byte
	// identifier the file does not contain. The block walk cannot complete, but the
	// dimensions were read from the header before it and must survive it - otherwise the
	// scan is recorded as a failure and the file is re-scanned on every index pass.
	constexpr uint8_t truncated_gif[] = {
		'G', 'I', 'F', '8', '9', 'a',
		0x40, 0x00, // width 64
		0x20, 0x00, // height 32
		0x00, 0x00, 0x00, // packed fields (no global colour table), background, aspect
		0x21, 0xFF, 0x0B, // application extension promising 11 bytes that are not there
	};

	mem_read_stream stream({truncated_gif, std::size(truncated_gif)});
	const auto scanned = scan_photo(stream);

	assert_equal(true, scanned.success, "truncated gif scanned");
	assert_equal(64u, scanned.width, "truncated gif width");
	assert_equal(32u, scanned.height, "truncated gif height");
}

static std::vector<uint8_t> make_tiff_with_dimensions(const uint32_t width, const uint32_t height)
{
	std::vector<uint8_t> buf;

	const auto put16 = [&buf](const uint16_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto put32 = [&buf](const uint32_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
		buf.push_back(static_cast<uint8_t>(v >> 16));
		buf.push_back(static_cast<uint8_t>(v >> 24));
	};
	const auto put_entry = [put16, put32](const uint16_t tag, const uint32_t value)
	{
		put16(tag);
		put16(4); // FMT_ULONG
		put32(1);
		put32(value);
	};

	put16(0x4949);
	put16(42);
	put32(8); // IFD0 offset

	put16(2); // entry count
	put_entry(0x0100, width); // ImageWidth
	put_entry(0x0101, height); // ImageLength
	put32(0); // no IFD1

	return buf;
}

static void should_reject_absurd_tiff_dimensions()
{
	const auto valid = make_tiff_with_dimensions(64, 32);
	mem_read_stream valid_stream({valid.data(), valid.size()});
	const auto scanned_valid = scan_photo(valid_stream);

	assert_equal(true, scanned_valid.success, "valid tiff scanned");
	assert_equal(64u, scanned_valid.width, "valid tiff width");
	assert_equal(32u, scanned_valid.height, "valid tiff height");

	// These are unvalidated 32-bit file fields. Cast into sizei this one turns negative, and
	// the decode budget it is later checked against then passes.
	const auto absurd = make_tiff_with_dimensions(0xFFFFFFFFu, 0xFFFFFFFFu);
	mem_read_stream absurd_stream({absurd.data(), absurd.size()});
	const auto scanned_absurd = scan_photo(absurd_stream);

	assert_equal(false, scanned_absurd.success, "absurd tiff rejected");
	assert_equal(0u, scanned_absurd.width, "absurd tiff width cleared");
	assert_equal(0u, scanned_absurd.height, "absurd tiff height cleared");
}

// Indexing must not pay for an embedded thumbnail it will not use: extracting one reads the
// thumbnail's bytes off the file and copies them (EXIF/TIFF) or fully decodes them (HEIF). The scan
// asks for one only when a thumbnail is wanted, and this pins both halves of that - nothing when it
// is not, and still a thumbnail when it is.
static void should_extract_embedded_thumbnails_only_on_demand()
{
	// Each is over the 256 KB in-memory limit, so the scan reaches them through the seek-and-read
	// stream rather than a span into an already-resident blob.
	for (const auto* const name : {"Nikon.JPG", "IMG_0096.JPG", "melnik.heic"})
	{
		const auto path = test_files_folder.combine_file(name);

		files ff;
		const auto indexed = ff_scan_file(ff, path);
		const auto wanted = ff_scan_and_load_thumb(ff, path);

		assert_equal(true, indexed.success, name, "metadata scan succeeded");
		assert_equal(false, is_valid(indexed.thumbnail_image) || is_valid(indexed.thumbnail_surface),
		             name, "metadata scan extracted no embedded thumbnail");

		// The metadata a scan reports must not depend on whether a thumbnail was asked for.
		assert_equal(wanted.width, indexed.width, name, "width");
		assert_equal(wanted.height, indexed.height, name, "height");
		assert_equal(static_cast<int>(wanted.orientation), static_cast<int>(indexed.orientation),
		             name, "orientation");
		assert_equal(wanted.created_utc, indexed.created_utc, name, "created");

		assert_equal(true, is_valid(wanted.thumbnail_image) || is_valid(wanted.thumbnail_surface),
		             name, "on-demand scan produced a thumbnail");
	}
}

// libheif's plugin priority would decode HEIC's HEVC with libde265. FFmpeg's decoder gives the same
// pixels in about two thirds of the time, so HEVC images are named to it - and only HEVC images,
// because a named decoder takes whatever codec the image holds. HEVC decoding is bit-exact by
// specification, so the two decoders must agree. They do on 64-bit; libde265's 32-bit build does not
// (on this thumbnail 32,462 of 589,824 samples, off by up to 132, while FFmpeg's 32-bit decode matches
// both 64-bit ones), which is a second reason HEVC goes to FFmpeg and why only 64-bit compares them.
static void should_decode_heic_hevc_with_ffmpeg()
{
	const auto* const hevc_decoder = heif_hevc_decoder_id();
	assert_equal("ffmpeg"sv, std::string_view(hevc_decoder ? hevc_decoder : ""), "libheif carries FFmpeg's HEVC decoder");

	const auto chosen = [](const std::string_view name)
	{
		file_read_stream stream;
		if (!stream.open(test_files_folder.combine_file(name))) return std::string{"unreadable"};
		const auto* const id = heif_primary_decoder_id(stream);
		return std::string(id ? id : "");
	};

	assert_equal("ffmpeg"s, chosen("melnik.heic"), "a grid of HEVC tiles is steered to FFmpeg");
	assert_equal(""s, chosen("hato.profile0.10bpc.yuv420.avif"), "an AV1 image keeps libheif's choice");

	const auto file = platform::open_file(test_files_folder.combine_file("melnik.heic"), platform::file_open_mode::read);
	assert_equal(true, file != nullptr, "HEIC fixture opened");
	if (!file) return;

	std::vector<uint8_t> bytes(static_cast<size_t>(file->size()));
	assert_equal(static_cast<uint64_t>(bytes.size()), file->read(bytes.data(), bytes.size()), "HEIC fixture read");

	// The embedded thumbnail is one HEVC picture, which is the unit either decoder works in.
	const auto decode_thumbnail = [&bytes](const char* const decoder_id)
	{
		std::vector<uint8_t> pixels;
		auto* const ctx = heif_context_alloc();
		auto* const options = heif_decoding_options_alloc();
		heif_image_handle* primary = nullptr;
		heif_image_handle* thumbnail = nullptr;
		heif_image* image = nullptr;
		heif_item_id thumbnail_id = 0;
		options->decoder_id = decoder_id;

		if (heif_context_read_from_memory_without_copy(ctx, bytes.data(), bytes.size(), nullptr).code == heif_error_Ok &&
			heif_context_get_primary_image_handle(ctx, &primary).code == heif_error_Ok &&
			heif_image_handle_get_list_of_thumbnail_IDs(primary, &thumbnail_id, 1) == 1 &&
			heif_image_handle_get_thumbnail(primary, thumbnail_id, &thumbnail).code == heif_error_Ok &&
			heif_decode_image(thumbnail, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options).code ==
			heif_error_Ok)
		{
			int stride = 0;
			const auto* const plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
			const auto height = heif_image_get_height(image, heif_channel_interleaved);
			if (plane && stride > 0 && height > 0) pixels.assign(plane, plane + static_cast<size_t>(stride) * height);
		}

		heif_image_release(image);
		heif_image_handle_release(thumbnail);
		heif_image_handle_release(primary);
		heif_decoding_options_free(options);
		heif_context_free(ctx);
		return pixels;
	};

	const auto by_ffmpeg = decode_thumbnail("ffmpeg");
	const auto by_libde265 = decode_thumbnail("libde265");

	assert_equal(true, !by_ffmpeg.empty() && !by_libde265.empty(), "both decoders decode the HEVC picture");

	if constexpr (sizeof(void*) == 8)
	{
		assert_equal(true, by_ffmpeg == by_libde265, "and give the same pixels");
	}
}

// Defined below with the other JPEG helpers; the orientation tests need a stream that carries no
// Exif of its own, which is exactly what encoding one here produces.
static ui::surface_ptr make_gradient_surface(int cx, int cy);

// Regression: the JPEG decoder must keep the APP1 marker, or the embedded EXIF orientation is lost.
static void should_read_jpeg_orientation()
{
	const auto load_path = test_files_folder.combine_file("exif-rotated.jpg");
	const auto data = df::blob_from_file(load_path);

	files ff;
	bool is_yuv = false;
	const auto rotated = files_test_hooks::decode_jpeg(ff, data, {}, false, decode_intent::display, is_yuv);
	assert_equal(true, is_valid(rotated), "decoded jpeg");
	assert_equal(ui::orientation::right_top, rotated->orientation(), "decoder recovers EXIF orientation");

	// files holds ONE decoder for every image it reads, so each decode has to state what its own
	// stream carries. Assigning the orientation only when an Exif marker was found left the
	// previous file's value in place for a stream with none - which is most embedded RAW
	// thumbnails - and drew them at that rotation.
	const auto plain = ff.surface_to_image(make_gradient_surface(32, 32), {}, {}, ui::image_format::JPEG);
	const auto upright = files_test_hooks::decode_jpeg(ff, plain->data(), {}, false, decode_intent::display, is_yuv);
	assert_equal(true, is_valid(upright), "a jpeg carrying no orientation decodes");
	assert_equal(ui::orientation::top_left, upright->orientation(),
	             "and is upright rather than keeping the last one's rotation");
}

// One EXIF orientation wherever Diffractor reads one: an item's metadata, every decoder and the scan
// take libfiles' rule - the last Orientation in IFD0, else the last in IFD1. A copy in the Exif
// directory is not the image's. Diffractor's own parser let that copy win while the scan ignored it,
// so an item could be shown turned one way and its thumbnail another.
static void should_read_one_exif_orientation_everywhere()
{
	const auto block = [](const std::optional<uint16_t> primary, const uint16_t in_exif, const uint16_t thumbnail)
	{
		df::blob b;
		const auto put16 = [&b](const uint16_t v)
		{
			b.push_back(static_cast<uint8_t>(v));
			b.push_back(static_cast<uint8_t>(v >> 8));
		};
		const auto put32 = [&put16](const uint32_t v)
		{
			put16(static_cast<uint16_t>(v));
			put16(static_cast<uint16_t>(v >> 16));
		};
		const auto put_entry = [&](const uint16_t tag, const uint16_t type, const uint32_t value)
		{
			put16(tag);
			put16(type);
			put32(1);
			put32(value);
		};

		constexpr uint16_t fmt_short = 3;
		constexpr uint16_t fmt_long = 4;
		const uint32_t ifd0_entries = primary ? 2 : 1;
		const uint32_t exif_ifd = 8 + 2 + 12 * ifd0_entries + 4;
		const uint32_t ifd1 = exif_ifd + 2 + 12 + 4;

		put16(0x4949); // little-endian TIFF header
		put16(0x002a);
		put32(8);

		put16(static_cast<uint16_t>(ifd0_entries));
		if (primary) put_entry(0x0112, fmt_short, *primary);
		put_entry(0x8769, fmt_long, exif_ifd); // ExifIFDPointer
		put32(ifd1);

		put16(1);
		put_entry(0x0112, fmt_short, in_exif); // a copy where the tag does not belong
		put32(0);

		put16(1);
		put_entry(0x0112, fmt_short, thumbnail);
		put32(0);
		return b;
	};

	files ff;
	const auto plain = ff.surface_to_image(make_gradient_surface(32, 32), {}, {}, ui::image_format::JPEG);
	assert_equal(true, plain && !plain->data().empty(), "encoded a jpeg to carry each block");

	const auto check = [&](const df::blob& exif, const ui::orientation expected, const std::string_view what)
	{
		prop::item_metadata md;
		metadata_exif::parse(md, {exif.data(), exif.size()});
		assert_equal(expected, md.orientation, std::format("{}: the item", what));
		assert_equal(expected, metadata_exif::orientation({exif.data(), exif.size()}),
		             std::format("{}: the HEIF, PNG and WebP decoders", what));

		// The block in an APP1 segment straight after the JPEG's SOI.
		const auto& jpeg = plain->data();
		const auto length = static_cast<uint16_t>(2 + 6 + exif.size());
		df::blob carried(jpeg.begin(), jpeg.begin() + 2);
		carried.push_back(0xFF);
		carried.push_back(0xE1);
		carried.push_back(static_cast<uint8_t>(length >> 8));
		carried.push_back(static_cast<uint8_t>(length));
		for (const auto c : std::string_view("Exif\0\0", 6)) carried.push_back(static_cast<uint8_t>(c));
		carried.insert(carried.end(), exif.begin(), exif.end());
		carried.insert(carried.end(), jpeg.begin() + 2, jpeg.end());

		bool is_yuv = false;
		const auto decoded = files_test_hooks::decode_jpeg(ff, carried, {}, false, decode_intent::display, is_yuv);
		assert_equal(true, is_valid(decoded), std::format("{}: the jpeg decodes", what));
		if (decoded) assert_equal(expected, decoded->orientation(), std::format("{}: the JPEG decoder", what));
	};

	check(block(uint16_t{1}, 6, 8), ui::orientation::top_left, "IFD0 says upright");
	check(block(uint16_t{3}, 6, 8), ui::orientation::bottom_right, "IFD0 says turned");
	check(block({}, 6, 8), ui::orientation::left_bottom, "only IFD1 says");
}

static df::blob make_iptc_caption(const std::string_view caption)
{
	df::blob result;
	result.push_back(0x1c);
	result.push_back(0x02);
	result.push_back(120);
	result.push_back(static_cast<uint8_t>(caption.size() >> 8));
	result.push_back(static_cast<uint8_t>(caption.size()));
	result.insert(result.end(), caption.begin(), caption.end());
	return result;
}

static void append_photoshop_resource(std::vector<uint8_t>& out, const uint16_t id, const std::string_view name,
                                      const df::cspan payload)
{
	out.insert(out.end(), {'8', 'B', 'I', 'M'});
	append_be16(out, id);
	out.push_back(static_cast<uint8_t>(name.size()));
	out.insert(out.end(), name.begin(), name.end());
	if ((name.size() & 1u) == 0) out.push_back(0);
	append_be32(out, static_cast<uint32_t>(payload.size));
	out.insert(out.end(), payload.begin(), payload.end());
	if (payload.size & 1u) out.push_back(0);
}

static df::blob jpeg_with_app13_resources(const ui::const_image_ptr& jpeg, const df::blob& resources)
{
	std::vector<uint8_t> out;
	const auto& source = jpeg->data();
	out.insert(out.end(), source.data(), source.data() + 2);
	out.push_back(0xff);
	out.push_back(0xed);
	const auto len = static_cast<uint16_t>(resources.size() + 2u);
	append_be16(out, len);
	out.insert(out.end(), resources.begin(), resources.end());
	out.insert(out.end(), source.data() + 2, source.data() + source.size());
	return {out.begin(), out.end()};
}

static void append_jpeg_marker(std::vector<uint8_t>& out, const uint8_t marker, const df::cspan payload)
{
	out.push_back(0xff);
	out.push_back(marker);
	const auto len = static_cast<uint16_t>(payload.size + 2u);
	append_be16(out, len);
	out.insert(out.end(), payload.begin(), payload.end());
}

static df::blob jpeg_with_metadata_markers(const ui::const_image_ptr& jpeg, const std::vector<df::blob>& payloads,
                                           const std::vector<uint8_t>& markers)
{
	std::vector<uint8_t> out;
	const auto& source = jpeg->data();
	out.insert(out.end(), source.data(), source.data() + 2);
	for (auto i = 0_z; i < payloads.size(); ++i) append_jpeg_marker(out, markers[i], payloads[i]);
	out.insert(out.end(), source.data() + 2, source.data() + source.size());
	return {out.begin(), out.end()};
}

static void should_parse_jpeg_photoshop_iptc_resources()
{
	files ff;
	const auto jpeg = ff.surface_to_image(make_gradient_surface(8, 8), {}, {}, ui::image_format::JPEG);
	assert_equal(true, is_valid(jpeg), "jpeg encoded");

	const auto iptc = make_iptc_caption("resource caption");
	std::vector<uint8_t> resources;
	constexpr std::string_view sig = "Photoshop 3.0\0"sv;
	resources.insert(resources.end(), sig.begin(), sig.end());
	const df::blob other = {1, 2, 3};
	append_photoshop_resource(resources, 0x03ed, "name", other);
	append_photoshop_resource(resources, 0x0404, "iptc", iptc);

	const auto wrapped = jpeg_with_app13_resources(jpeg, {resources.begin(), resources.end()});
	mem_read_stream stream(wrapped);
	const auto scanned = scan_photo(stream);
	prop::item_metadata md;
	metadata_iptc::parse(md, scanned.metadata.iptc);

	assert_equal("resource caption", md.description, "named IPTC resource parsed");

	// The resource the encoder writes the IPTC in reads back as it went in, its odd length padded.
	metadata_parts written;
	written.iptc = iptc.clone();
	const auto encoded = ff.surface_to_image(make_gradient_surface(8, 8), written, {}, ui::image_format::JPEG);
	mem_read_stream encoded_stream(encoded->data());
	const auto rescanned = scan_photo(encoded_stream);
	assert_equal(true, std::ranges::equal(rescanned.metadata.iptc, iptc),
	             "written marker length and padding preserve payload");
}

static void should_keep_app13_from_overriding_jpeg_app1_and_app2_metadata()
{
	files ff;
	const auto jpeg = ff.surface_to_image(make_gradient_surface(8, 8), {}, {}, ui::image_format::JPEG);
	assert_equal(true, is_valid(jpeg), "jpeg encoded");

	const df::blob app1_xmp = {'a', 'p', 'p', '1', '-', 'x', 'm', 'p'};
	const df::blob app13_xmp = {'a', 'p', 'p', '1', '3', '-', 'x', 'm', 'p'};
	const df::blob app1_exif = {'I', 'I', 0x2a, 0, 8, 0, 0, 0};
	const df::blob app13_exif = {'M', 'M', 0, 0x2a};
	const df::blob app2_icc = {'i', 'c', 'c', '-', 'a', 'p', 'p', '2'};
	const df::blob app13_icc = {'i', 'c', 'c', '-', 'a', 'p', 'p', '1', '3'};

	std::vector<uint8_t> app13;
	constexpr std::string_view sig = "Photoshop 3.0\0"sv;
	app13.insert(app13.end(), sig.begin(), sig.end());
	append_photoshop_resource(app13, 0x0424, "", app13_xmp);
	append_photoshop_resource(app13, 0x0422, "", app13_exif);
	append_photoshop_resource(app13, 0x040f, "", app13_icc);

	df::blob xmp_marker;
	xmp_marker.insert(xmp_marker.end(), xmp_signature.begin(), xmp_signature.end());
	xmp_marker.insert(xmp_marker.end(), app1_xmp.begin(), app1_xmp.end());
	df::blob exif_marker;
	exif_marker.insert(exif_marker.end(), exif_signature.begin(), exif_signature.end());
	exif_marker.insert(exif_marker.end(), app1_exif.begin(), app1_exif.end());
	df::blob icc_marker;
	icc_marker.insert(icc_marker.end(), icc_signature.begin(), icc_signature.end());
	icc_marker.push_back(1);
	icc_marker.push_back(1);
	icc_marker.insert(icc_marker.end(), app2_icc.begin(), app2_icc.end());

	std::vector<df::blob> payloads;
	payloads.emplace_back(std::move(xmp_marker));
	payloads.emplace_back(std::move(exif_marker));
	payloads.emplace_back(std::move(icc_marker));
	payloads.emplace_back(app13.begin(), app13.end());
	const auto wrapped = jpeg_with_metadata_markers(jpeg, payloads, {0xe1, 0xe1, 0xe2, 0xed});
	mem_read_stream stream(wrapped);
	const auto scanned = scan_photo(stream);
	assert_equal(true, scanned.metadata.xmp == app1_xmp, "APP1 XMP remains authoritative");
	assert_equal(true, scanned.metadata.exif == app1_exif, "APP1 EXIF remains authoritative");
	assert_equal(true, scanned.metadata.icc == app2_icc, "APP13 ICC is not appended to APP2 ICC");
}

static void should_preserve_non_iptc_jpeg_photoshop_resources()
{
	files ff;
	const auto jpeg = ff.surface_to_image(make_gradient_surface(8, 8), {}, {}, ui::image_format::JPEG);
	assert_equal(true, is_valid(jpeg), "jpeg encoded");

	const auto iptc = make_iptc_caption("resource caption");
	const df::blob other = {9, 8, 7, 6, 5};
	std::vector<uint8_t> app13;
	constexpr std::string_view sig = "Photoshop 3.0\0"sv;
	app13.insert(app13.end(), sig.begin(), sig.end());
	append_photoshop_resource(app13, 0x0404, "iptc", iptc);
	append_photoshop_resource(app13, 0x03ed, "keep", other);

	const auto wrapped = jpeg_with_app13_resources(jpeg, {app13.begin(), app13.end()});
	mem_read_stream stream(wrapped);
	const auto scanned = scan_photo(stream);
	const auto encoded = save_jpeg(make_gradient_surface(8, 8), scanned.metadata, {});
	assert_equal(true, is_valid(encoded), "jpeg re-encoded");

	mem_read_stream out_stream(encoded->data());
	const auto rescanned = scan_photo(out_stream);
	std::vector<uint8_t> expected;
	append_photoshop_resource(expected, 0x03ed, "keep", other);
	assert_equal(true, rescanned.metadata.photoshop_resources == df::blob{expected.begin(), expected.end()},
	             "non-IPTC Photoshop resource survives re-encode");
}

static void should_roundtrip_split_jpeg_photoshop_resources()
{
	files ff;
	const auto jpeg = ff.surface_to_image(make_gradient_surface(8, 8), {}, {}, ui::image_format::JPEG);
	assert_equal(true, is_valid(jpeg), "jpeg encoded");

	const df::blob first_payload(40000, 0x31);
	const df::blob second_payload(40000, 0x32);
	std::vector<uint8_t> first;
	std::vector<uint8_t> second;
	constexpr std::string_view sig = "Photoshop 3.0\0"sv;
	first.insert(first.end(), sig.begin(), sig.end());
	second.insert(second.end(), sig.begin(), sig.end());
	append_photoshop_resource(first, 0x03ed, "first", first_payload);
	append_photoshop_resource(second, 0x03ee, "second", second_payload);

	std::vector<df::blob> payloads;
	payloads.emplace_back(first.begin(), first.end());
	payloads.emplace_back(second.begin(), second.end());
	const auto wrapped = jpeg_with_metadata_markers(jpeg, payloads, {0xed, 0xed});
	mem_read_stream stream(wrapped);
	const auto scanned = scan_photo(stream);

	std::vector<uint8_t> expected;
	append_photoshop_resource(expected, 0x03ed, "first", first_payload);
	append_photoshop_resource(expected, 0x03ee, "second", second_payload);
	assert_equal(true, scanned.metadata.photoshop_resources == df::blob{expected.begin(), expected.end()},
	             "split APP13 resources are joined in order");

	const auto encoded = save_jpeg(make_gradient_surface(8, 8), scanned.metadata, {});
	assert_equal(true, is_valid(encoded), "split APP13 resources re-encode");
	mem_read_stream out_stream(encoded->data());
	const auto rescanned = scan_photo(out_stream);
	assert_equal(true, rescanned.metadata.photoshop_resources == df::blob{expected.begin(), expected.end()},
	             "split APP13 resources survive re-encode");
}

static df::blob split_app13_payload(const std::vector<uint8_t>& joined, const size_t split_at)
{
	constexpr std::string_view sig = "Photoshop 3.0\0"sv;
	std::vector<df::blob> payloads;
	for (auto pos = 0_z; pos < joined.size(); pos += split_at)
	{
		std::vector<uint8_t> payload;
		payload.insert(payload.end(), sig.begin(), sig.end());
		const auto count = std::min(split_at, joined.size() - pos);
		payload.insert(payload.end(), joined.begin() + static_cast<ptrdiff_t>(pos),
		               joined.begin() + static_cast<ptrdiff_t>(pos + count));
		payloads.emplace_back(payload.begin(), payload.end());
	}

	files ff;
	const auto jpeg = ff.surface_to_image(make_gradient_surface(8, 8), {}, {}, ui::image_format::JPEG);
	return jpeg_with_metadata_markers(jpeg, payloads, std::vector<uint8_t>(payloads.size(), 0xed));
}

static void should_join_straddled_jpeg_photoshop_resources()
{
	const df::blob first_payload(65490, 0x41);
	const df::blob second_payload(5000, 0x42);
	std::vector<uint8_t> joined;
	append_photoshop_resource(joined, 0x03ed, "first", first_payload);
	append_photoshop_resource(joined, 0x03ee, "second", second_payload);

	const auto wrapped = split_app13_payload(joined, 65519);
	mem_read_stream stream(wrapped);
	const auto scanned = scan_photo(stream);
	assert_equal(true, scanned.metadata.photoshop_resources == df::blob{joined.begin(), joined.end()},
	             "straddled APP13 record survives the byte-level join");
}

static void should_cap_joined_jpeg_photoshop_resources()
{
	const df::blob payload(1200, 0x43);
	std::vector<uint8_t> joined;
	append_photoshop_resource(joined, 0x03ed, "first", payload);
	append_photoshop_resource(joined, 0x03ee, "second", payload);

	files_test_hooks::set_jpeg_photoshop_resource_cap(1600);
	const df::scope_exit restore_cap([] { files_test_hooks::set_jpeg_photoshop_resource_cap(0); });

	const auto wrapped = split_app13_payload(joined, 800);
	mem_read_stream stream(wrapped);
	const auto scanned = scan_photo(stream);

	std::vector<uint8_t> expected;
	append_photoshop_resource(expected, 0x03ed, "first", payload);
	assert_equal(true, scanned.metadata.photoshop_resources == df::blob{expected.begin(), expected.end()},
	             "joined APP13 payload is capped at a complete resource");
}

// IFD1 describes the embedded thumbnail, which cameras commonly store already upright while the
// primary image is not. Taking its orientation over IFD0's displayed the photograph at its
// thumbnail's rotation; it may only stand in where the primary image gave none.
static void should_prefer_primary_orientation_over_the_thumbnail_ifd()
{
	// A little-endian TIFF whose IFD0 carries one orientation and points at an IFD1 carrying
	// another. Nothing else: the scanner reads orientation from both and needs no more.
	const auto build_exif = [](const uint16_t ifd0_orientation, const uint16_t ifd1_orientation)
	{
		std::vector<uint8_t> b;
		const auto put16 = [&b](const uint16_t v)
		{
			b.push_back(static_cast<uint8_t>(v));
			b.push_back(static_cast<uint8_t>(v >> 8));
		};
		const auto put32 = [&b](const uint32_t v)
		{
			for (auto i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
		};
		const auto orientation_ifd = [&](const uint16_t orientation)
		{
			put16(1); // one entry
			put16(0x0112); // Orientation
			put16(3); // FMT_USHORT
			put32(1);
			put16(orientation);
			put16(0); // the value field is four bytes wide
		};

		put16(0x4949);
		put16(0x002a);
		put32(8); // IFD0 follows the header

		constexpr uint32_t ifd1_offset = 8 + 2 + 12 + 4;
		orientation_ifd(ifd0_orientation);
		put32(ifd1_offset);
		orientation_ifd(ifd1_orientation);
		put32(0); // no further IFD
		return b;
	};

	files ff;
	const auto jpeg = ff.surface_to_image(make_gradient_surface(32, 32), {}, {}, ui::image_format::JPEG);

	const auto scan_with_exif = [&](const uint16_t ifd0_orientation, const uint16_t ifd1_orientation)
	{
		const auto tiff = build_exif(ifd0_orientation, ifd1_orientation);
		const auto& source = jpeg->data();

		std::vector<uint8_t> out;
		out.insert(out.end(), source.data(), source.data() + 2); // SOI

		const auto payload = static_cast<uint16_t>(2 + 6 + tiff.size());
		out.push_back(0xFF);
		out.push_back(0xE1);
		out.push_back(static_cast<uint8_t>(payload >> 8));
		out.push_back(static_cast<uint8_t>(payload));

		constexpr uint8_t signature[] = {'E', 'x', 'i', 'f', 0, 0};
		out.insert(out.end(), std::begin(signature), std::end(signature));
		out.insert(out.end(), tiff.begin(), tiff.end());
		out.insert(out.end(), source.data() + 2, source.data() + source.size());

		const auto path = _temps.next_path(".jpg");
		df::blob_save_to_file(df::blob(out.begin(), out.end()), path);
		return ff_scan_file(ff, path).orientation;
	};

	assert_equal(static_cast<int>(ui::orientation::right_top),
	             static_cast<int>(scan_with_exif(6, 1)),
	             "the primary image's rotation is what the photograph is shown at");
	assert_equal(static_cast<int>(ui::orientation::top_left),
	             static_cast<int>(scan_with_exif(1, 6)),
	             "and an upright primary is not rotated by its thumbnail");
}

static void should_use_thumbnail_orientation_only_when_primary_is_absent()
{
	const auto build_exif = []()
	{
		std::vector<uint8_t> b;
		const auto put16 = [&b](const uint16_t v)
		{
			b.push_back(static_cast<uint8_t>(v));
			b.push_back(static_cast<uint8_t>(v >> 8));
		};
		const auto put32 = [&b](const uint32_t v)
		{
			for (auto i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
		};

		put16(0x4949);
		put16(0x002a);
		put32(8); // IFD0 follows the header

		constexpr uint32_t ifd1_offset = 8 + 2 + 4;
		put16(0); // IFD0 has no primary orientation
		put32(ifd1_offset);

		put16(1); // IFD1 carries the thumbnail-orientation fallback
		put16(0x0112); // Orientation
		put16(3); // FMT_USHORT
		put32(1);
		put16(6); // right_top
		put16(0);
		put32(0); // no further IFD
		return b;
	};

	files ff;
	const auto plain = ff.surface_to_image(make_gradient_surface(32, 32), {}, {}, ui::image_format::JPEG);
	const auto tiff = build_exif();
	const auto& source = plain->data();

	std::vector<uint8_t> jpeg;
	jpeg.insert(jpeg.end(), source.data(), source.data() + 2); // SOI

	const auto payload = static_cast<uint16_t>(2 + 6 + tiff.size());
	jpeg.push_back(0xFF);
	jpeg.push_back(0xE1);
	jpeg.push_back(static_cast<uint8_t>(payload >> 8));
	jpeg.push_back(static_cast<uint8_t>(payload));

	constexpr uint8_t signature[] = {'E', 'x', 'i', 'f', 0, 0};
	jpeg.insert(jpeg.end(), std::begin(signature), std::end(signature));
	jpeg.insert(jpeg.end(), tiff.begin(), tiff.end());
	jpeg.insert(jpeg.end(), source.data() + 2, source.data() + source.size());

	mem_read_stream scan_stream({jpeg.data(), jpeg.size()});
	const auto scanned = scan_photo(scan_stream);

	bool is_yuv = false;
	const auto decoded = files_test_hooks::decode_jpeg(ff, {jpeg.data(), jpeg.size()}, {}, false,
	                                                   decode_intent::display, is_yuv);
	assert_equal(true, is_valid(decoded), "decoder reads IFD1-only jpeg");
	assert_equal(static_cast<int>(ui::orientation::right_top),
	             static_cast<int>(scanned.orientation),
	             "scan uses IFD1 orientation when IFD0 is absent");
	assert_equal(static_cast<int>(scanned.orientation),
	             static_cast<int>(decoded->orientation()),
	             "decoder and scanner agree on IFD1-only orientation");
}

static std::vector<uint8_t> make_tiff_with_orientations(const uint16_t ifd0_orientation,
                                                        const uint16_t ifd1_orientation)
{
	std::vector<uint8_t> b;
	const auto put16 = [&b](const uint16_t v)
	{
		b.push_back(static_cast<uint8_t>(v));
		b.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto put32 = [&b](const uint32_t v)
	{
		for (auto i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
	};
	const auto put_entry = [&](const uint16_t tag, const uint16_t fmt, const uint32_t count, const uint32_t value)
	{
		put16(tag);
		put16(fmt);
		put32(count);
		put32(value);
	};
	const auto put_orientation = [&](const uint16_t orientation)
	{
		put16(0x0112); // Orientation
		put16(3); // FMT_USHORT
		put32(1);
		put16(orientation);
		put16(0);
	};

	put16(0x4949);
	put16(42);
	put32(8);

	constexpr uint32_t ifd1_offset = 8 + 2 + 3 * 12 + 4;
	put16(3);
	put_entry(0x0100, 4, 1, 32); // ImageWidth
	put_entry(0x0101, 4, 1, 16); // ImageLength
	put_orientation(ifd0_orientation);
	put32(ifd1_offset);
	put16(1);
	put_orientation(ifd1_orientation);
	put32(0);
	return b;
}

static void should_prefer_primary_tiff_orientation_over_the_thumbnail_ifd()
{
	const auto scan_orientation = [](const uint16_t ifd0_orientation, const uint16_t ifd1_orientation)
	{
		const auto tiff = make_tiff_with_orientations(ifd0_orientation, ifd1_orientation);
		mem_read_stream stream({tiff.data(), tiff.size()});
		return scan_photo(stream).orientation;
	};

	assert_equal(static_cast<int>(ui::orientation::right_top),
	             static_cast<int>(scan_orientation(6, 1)),
	             "the primary TIFF orientation is kept");
	assert_equal(static_cast<int>(ui::orientation::top_left),
	             static_cast<int>(scan_orientation(1, 6)),
	             "the thumbnail TIFF orientation does not rotate an upright primary");
}

// The payload of the first DQT segment, which is the table the encoder quantized with.
static df::blob first_dqt(const df::cspan jpeg)
{
	for (size_t i = 2; i + 4 < jpeg.size;)
	{
		if (jpeg.data[i] != 0xFF) break;

		const auto marker = jpeg.data[i + 1];

		if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
		{
			i += 2;
			continue;
		}

		const size_t len = (static_cast<size_t>(jpeg.data[i + 2]) << 8) | jpeg.data[i + 3];
		if (marker == 0xDB) return {jpeg.data + i + 4, jpeg.data + i + 2 + len};
		if (marker == 0xDA) break; // start of scan - no table declarations follow
		i += 2 + len;
	}

	return {};
}

static ui::surface_ptr make_gradient_surface(const int cx, const int cy)
{
	auto surface = std::make_shared<ui::surface>();
	surface->alloc(cx, cy, ui::texture_format::RGB);

	for (auto y = 0; y < cy; ++y)
		for (auto x = 0; x < cx; ++x)
			surface->set_pixel(x, y, ui::rgba((x * 4) & 0xFF, (y * 4) & 0xFF, ((x ^ y) * 4) & 0xFF));

	return surface;
}

// Editing a JPEG must re-encode against the source's own quantization tables, so an untouched block
// quantizes back to itself instead of being re-quantized to whatever the quality slider says.
static void should_reuse_source_jpeg_tables()
{
	files ff;
	const auto surface = make_gradient_surface(64, 64);

	file_encode_params coarse;
	coarse.jpeg_save_quality = 40;
	const auto source = ff.surface_to_image(surface, {}, coarse, ui::image_format::JPEG);
	const auto source_dqt = first_dqt(source->data());
	assert_equal(false, source_dqt.empty(), "source declares a quantization table");

	file_encode_params matched;
	matched.jpeg_save_quality = 95;
	matched.jpeg_source = source->data();
	const auto re_encoded = ff.surface_to_image(surface, {}, matched, ui::image_format::JPEG);
	assert_equal(true, first_dqt(re_encoded->data()) == source_dqt, "re-encode adopts the source tables");

	file_encode_params unmatched;
	unmatched.jpeg_save_quality = 95;
	const auto control = ff.surface_to_image(surface, {}, unmatched, ui::image_format::JPEG);
	assert_equal(false, first_dqt(control->data()) == source_dqt, "quality still applies without a source");
}

// A source libjpeg fails on - one that is not a JPEG, or a lossless one, whose tables it will not copy -
// is no reason to fail the save of an edit: the save falls back to the quality. libfiles' own Release
// build once let libjpeg's error escape instead, the catch that made it the fallback dropped under
// /EHsc; libfiles now returns from those errors by longjmp, whatever its consumer's flags.
static void should_save_over_a_source_jpeg_libjpeg_fails_on()
{
	jpeg_compress_struct encoder{};
	jpeg_error_mgr error{};
	encoder.err = jpeg_std_error(&error);
	error.error_exit = [](j_common_ptr info)
	{
		char message[JMSG_LENGTH_MAX]{};
		info->err->format_message(info, message);
		throw test_assert_exception(message);
	};
	const df::scope_exit destroy([&encoder] { jpeg_destroy_compress(&encoder); });
	jpeg_create_compress(&encoder);
	unsigned char* encoded = nullptr;
	unsigned long encoded_size = 0;
	const df::scope_exit free_encoded([&encoded] { std::free(encoded); });
	jpeg_mem_dest(&encoder, &encoded, &encoded_size);
	encoder.image_width = 16;
	encoder.image_height = 16;
	encoder.input_components = 3;
	encoder.in_color_space = JCS_YCbCr;
	jpeg_set_defaults(&encoder);
	for (auto ci = 0; ci < encoder.num_components; ++ci)
		encoder.comp_info[ci].h_samp_factor = encoder.comp_info[ci].v_samp_factor = 1;
	jpeg_enable_lossless(&encoder, 1, 0);
	jpeg_start_compress(&encoder, TRUE);
	JSAMPLE samples[16 * 3] = {};
	for (auto i = 0; i < 16 * 3; ++i) samples[i] = static_cast<JSAMPLE>(i * 5);
	JSAMPROW row = samples;
	while (encoder.next_scanline < encoder.image_height) jpeg_write_scanlines(&encoder, &row, 1);
	jpeg_finish_compress(&encoder);

	files ff;
	const auto surface = make_gradient_surface(64, 64);
	file_encode_params params;
	params.jpeg_save_quality = 95;
	const auto expected = first_dqt(ff.surface_to_image(surface, {}, params, ui::image_format::JPEG)->data());

	const df::blob not_jpeg = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
	for (const auto source : {df::cspan(not_jpeg), df::cspan{encoded, encoded_size}})
	{
		params.jpeg_source = source;
		const auto saved = save_surface(ui::image_format::JPEG, surface, {}, params);
		assert_equal(true, first_dqt(saved->data()) == expected, "a save falls back to the quality's tables");
		const auto stored = ff.surface_to_image(surface, {}, params, ui::image_format::JPEG);
		assert_equal(true, first_dqt(stored->data()) == expected, "so does the encoder files holds");
	}
}

// What libfiles tells a sink about a JPEG before it reads a pixel, the decode then refused.
static libfiles::frame_info jpeg_frame(const df::cspan data)
{
	struct refusing_sink final : libfiles::image_sink
	{
		libfiles::frame_info frame;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& f) override
		{
			frame = f;
			frame.exif = {};
			return {};
		}
	};

	refusing_sink sink;
	libfiles::reader r(std::span<const uint8_t>(data.data, data.size));
	libfiles::decode(r, sink);
	return sink.frame;
}

// Lossless rotation must refuse rather than trim. Trimming silently drops up to a whole MCU of edge
// pixels the user saw in the preview; refusing sends the save down the re-encode path instead.
static void should_refuse_imperfect_lossless_rotate()
{
	files ff;

	// 4:2:0 chroma puts the MCU grid on 16 pixels, so 20 rows cannot rotate losslessly.
	const auto aligned = ff.surface_to_image(make_gradient_surface(32, 32), {}, {}, ui::image_format::JPEG);
	const auto unaligned = ff.surface_to_image(make_gradient_surface(32, 20), {}, {}, ui::image_format::JPEG);

	jpeg_encoder aligned_encoder;
	const jpeg_transformer aligned_transformer;
	assert_equal(false,
	             aligned_transformer.transform(aligned->data(), aligned_encoder, simple_transform::rot_90).empty(),
	             "aligned rotate stays lossless");

	jpeg_encoder unaligned_encoder;
	const jpeg_transformer unaligned_transformer;
	assert_equal(true,
	             unaligned_transformer.transform(unaligned->data(), unaligned_encoder, simple_transform::rot_90).empty(),
	             "unaligned rotate refuses rather than trimming");
}

// files holds ONE long-lived encoder for every image it writes, and handle_error_exit throws out of
// libjpeg. An encode abandoned mid-scan therefore leaves global_state at CSTATE_SCANNING, and
// without a reset on entry every later encode - every thumbnail, every Convert - would ERREXIT with
// "Improper call to JPEG library" for the rest of the session.
static void should_reuse_jpeg_encoder_after_abandoned_encode()
{
	jpeg_encoder encoder;
	const auto surface = make_gradient_surface(32, 32);
	const auto dimensions = surface->dimensions();

	// start() leaves the encoder mid-compress, which is the state a throw out of jpeg_write_scanlines
	// or a marker write hands back.
	encoder.start(dimensions.cx, dimensions.cy, ui::orientation::top_left, {}, {});

	const auto encoded = encoder.encode(dimensions.cx, dimensions.cy, surface->pixels(),
	                                    static_cast<uint32_t>(surface->stride()), ui::orientation::top_left, {}, {});

	assert_equal(false, encoded.empty(), "encoder still usable after an abandoned encode");
}

// Offset of the start-of-scan marker, or 0 when there is none.
static size_t sos_offset(const df::cspan jpeg)
{
	for (size_t i = 2; i + 4 < jpeg.size;)
	{
		if (jpeg.data[i] != 0xFF) break;

		const auto marker = jpeg.data[i + 1];

		if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
		{
			i += 2;
			continue;
		}

		if (marker == 0xDA) return i;

		i += 2 + ((static_cast<size_t>(jpeg.data[i + 2]) << 8) | jpeg.data[i + 3]);
	}

	return 0;
}

// A JPEG that ends inside its entropy data makes jpeg_read_coefficients suspend and hand back a
// null coefficient array, which transupp indexes straight into a crash. The rotate must refuse -
// and leave both codecs usable, because files holds one transformer and one encoder for every file.
static void should_survive_truncated_lossless_rotate()
{
	files ff;

	const auto whole = ff.surface_to_image(make_gradient_surface(256, 256), {}, {}, ui::image_format::JPEG);
	const auto& data = whole->data();
	const auto sos = sos_offset(data);

	assert_equal(true, sos > 0 && sos + 64 < data.size(), "test image has scan data to truncate");

	const std::vector<uint8_t> truncated(data.data(), data.data() + sos + 64);

	jpeg_encoder encoder;
	const jpeg_transformer transformer;

	assert_equal(true,
	             transformer.transform({truncated.data(), truncated.size()}, encoder, simple_transform::rot_90).empty(),
	             "truncated rotate refuses");

	assert_equal(false, transformer.transform(data, encoder, simple_transform::rot_90).empty(),
	             "transformer and encoder stay usable after the refusal");
}

// jpeg_read_coefficients holds every coefficient of the full-resolution image whatever scale the
// caller asked for, and transupp requests a second array the same size to rotate into. The decode
// budget divides by the scale factor, so it describes none of that - a rotate has to be measured
// against what its coefficients cost, or a hundred-megapixel photograph reaches libjpeg asking for
// gigabytes.
static void should_refuse_an_over_budget_lossless_rotate()
{
	files ff;
	const auto image = ff.surface_to_image(make_gradient_surface(256, 256), {}, {}, ui::image_format::JPEG);

	jpeg_encoder encoder;
	jpeg_transformer transformer;

	assert_equal(true, transformer.read_header(image->data()), "header read");
	assert_equal(static_cast<uint64_t>(0), jpeg_frame(image->data()).working_bytes,
	             "a baseline single-scan jpeg decodes a block at a time");

	// 256 x 256 at 4:2:0 is 32 x 32 luma blocks and 16 x 16 of each chroma, at 128 bytes a block.
	constexpr int64_t expected = (32 * 32 + 2 * 16 * 16) * 128;
	assert_equal(static_cast<uint64_t>(expected), static_cast<uint64_t>(transformer.coefficient_bytes()),
	             "a rotate holds the whole image however little it would decode");

	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });

	df::max_decode_bytes = expected * 2 - 1;
	auto over_budget = false;
	assert_equal(true, transformer.transform(image->data(), encoder, simple_transform::rot_90, &over_budget).empty(),
	             "a rotate one byte over its workspace refuses");
	assert_equal(true, over_budget, "and says the budget is why, which is not an imperfect rotate");

	df::max_decode_bytes = expected * 2;
	assert_equal(false, transformer.transform(image->data(), encoder, simple_transform::rot_90, &over_budget).empty(),
	             "the same rotate is taken when source and workspace both fit");
	assert_equal(false, over_budget, "and is not reported as over budget");
}

// A lossless rotate refused for its budget must not turn into a re-encode. The re-encode holds the
// decoded picture and a rotated copy - more than the rotate was refused for - and gives up quality
// besides, so a JPEG that could have turned losslessly came back lossy from a check meant to bound
// memory. The budget here fits the decode the re-encode would start with, but not the rotate.
static void should_not_re_encode_a_rotate_refused_for_its_budget()
{
	const auto load_path = test_files_folder.combine_file("Lossless0.jpg");
	const auto save_path = _temps.next_path();
	const auto source = df::blob_from_file(load_path);

	jpeg_transformer transformer;
	assert_equal(true, transformer.read_header({source.data(), source.size()}), "header read");
	const auto dimensions = transformer.dimensions();
	const auto rotate_bytes = transformer.coefficient_bytes() * 2;

	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });

	df::max_decode_bytes = rotate_bytes - 1;
	assert_equal(true, static_cast<int64_t>(dimensions.cx) * dimensions.cy * 4 <= df::max_decode_bytes,
	             "the decode a re-encode starts with fits the budget");

	image_edits edits;
	const quadd crop(dimensions);
	edits.crop_bounds(crop.transform(simple_transform::rot_90));

	files ff;
	const auto result = ff.update(load_path, save_path, {}, edits, {}, false, {});

	assert_equal(false, result.success(), "the rotate is refused rather than re-encoded");
	assert_equal(false, save_path.exists(), "and nothing is written in its place");
}

// libjpeg reduces while it decodes, which is what makes an enormous stitch affordable as a
// thumbnail - but a progressive stream must first buffer every coefficient of the full-resolution
// image, and no decode scale touches that. Charging only the reduced output let a stream whose
// coefficients alone run to gigabytes pass a budget that said kilobytes.
static void should_charge_a_progressive_jpeg_for_its_coefficients()
{
	files ff;

	// The fixture lives in excluded1 so that adding it does not change the indexed item counts.
	const auto progressive = ff.load(test_files_folder.combine("excluded1").combine_file("Progressive.jpg"), false);
	assert_equal(true, is_valid(progressive.i), "the progressive fixture loads");

	const auto baseline = ff.surface_to_image(make_gradient_surface(256, 256), {}, {}, ui::image_format::JPEG);
	assert_equal(true, is_valid(baseline), "and a baseline control of the same shape encodes");

	// Both are 256 x 256 at 4:2:0. An eighth-scale decode writes 32 x 32 pixels either way, but the
	// progressive one also holds 32 x 32 luma blocks and 16 x 16 of each chroma at 128 bytes a
	// block, whole and unscaled, before it can emit a single row.
	constexpr sizei thumbnail{32, 32};
	constexpr int64_t scaled_output_bytes = 32ll * 32 * 4;
	constexpr int64_t coefficient_bytes = (32 * 32 + 2 * 16 * 16) * 128;

	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });

	df::max_decode_bytes = scaled_output_bytes + coefficient_bytes - 1;
	assert_equal(false, is_valid(ff.image_to_surface(progressive.i, thumbnail)),
	             "a progressive thumbnail one byte short of its coefficients is refused");
	assert_equal(true, is_valid(ff.image_to_surface(baseline, thumbnail)),
	             "a baseline thumbnail of the same picture is not");

	df::max_decode_bytes = scaled_output_bytes + coefficient_bytes;
	assert_equal(true, is_valid(ff.image_to_surface(progressive.i, thumbnail)),
	             "and the progressive one is taken once its coefficients fit");
}

static void should_rotate_lossless()
{
	const auto save_path = _temps.next_path();
	const auto load_path = test_files_folder.combine_file("Lossless0.jpg");

	image_edits edits;
	const quadd crop(sizei(640, 480));
	edits.crop_bounds(crop.transform(simple_transform::rot_90));

	files ff;
	ff.update(load_path, save_path, {}, edits, {}, false, {});

	const auto expected = extract_properties(test_files_folder.combine_file("Lossless90.jpg"));
	const auto actual = extract_properties(save_path);

	assert_equal(expected->width, actual->width);
	assert_equal(expected->height, actual->height);
}

static void should_rotate_lossless_exif_dimensions()
{
	const auto surface = std::make_shared<ui::surface>();
	assert_equal(true, surface->alloc(32, 16, ui::texture_format::RGB) != nullptr, "rotation fixture allocated");
	surface->make_blank();
	metadata_parts metadata;
	metadata.exif = {
		'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0,
		1, 0, 0x69, 0x87, 4, 0, 1, 0, 0, 0, 26, 0, 0, 0, 0, 0, 0, 0,
		2, 0, 2, 0xa0, 4, 0, 1, 0, 0, 0, 32, 0, 0, 0,
		3, 0xa0, 4, 0, 1, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0
	};
	jpeg_encoder encoder;
	const auto source = encoder.encode(32, 16, surface->pixels(), static_cast<uint32_t>(surface->stride()),
	                                   ui::orientation::top_left, metadata, {});
	jpeg_transformer transformer;
	jpeg_encoder rotated_encoder;
	const auto rotated = transformer.transform(source, rotated_encoder, simple_transform::rot_90);
	assert_equal(false, rotated.empty(), "aligned JPEG rotated losslessly");
	const auto path = _temps.next_path(".jpg");
	assert_equal(true, df::blob_save_to_file(rotated, path), "rotated fixture saved");
	files codecs;
	const auto scanned = ff_scan_file(codecs, path);
	const auto listing = libfiles::list_exif({scanned.metadata.exif.data(), scanned.metadata.exif.size()});
	assert_equal(false, listing.directories.empty(), "rotated EXIF is readable independently of JPEG header dimensions");
	const auto dimension = [&listing](const uint16_t tag) -> uint32_t
	{
		for (const auto& directory : listing.directories)
		{
			if (directory.directory != libfiles::exif_directory::exif) continue;

			for (const auto& e : directory.entries)
			{
				if (e.tag != tag) continue;
				assert_equal(true, e.type == 4 && e.bytes.size() >= 4, "dimension tag is a LONG");
				const auto* const p = e.bytes.data();
				return listing.little_endian
					       ? p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24
					       : static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
			}
		}

		assert_equal(true, false, "dimension tag is present");
		return 0;
	};
	assert_equal(16u, dimension(0xa002), "EXIF width describes the rotated destination");
	assert_equal(32u, dimension(0xa003), "EXIF height describes the rotated destination");
}

// Every 8-bit YCbCr JPEG belongs on the GPU NV12 path; the decoder averages whatever chroma the
// source carries down to one pair per 2x2 block. Only formats it cannot pack take the RGB path.
static bool jpeg_uses_nv12(files& ff, const char* const name)
{
	const auto loaded = ff.load(test_files_folder.combine_file(name), false);
	assert_equal(true, is_valid(loaded.i), "loaded jpeg");

	bool is_yuv = false;
	const auto decoded = files_test_hooks::decode_jpeg(ff, loaded.i->data(), {}, true, decode_intent::display, is_yuv);
	assert_equal(true, is_valid(decoded), "decoded jpeg");
	return is_yuv;
}

static void should_render_ycbcr_jpeg_as_nv12()
{
	files ff;

	assert_equal(true, jpeg_uses_nv12(ff, "exif-rotated.jpg"), "4:2:0 renders as nv12");
	assert_equal(true, jpeg_uses_nv12(ff, "Small.jpg"), "4:2:2 renders as nv12");
	assert_equal(false, jpeg_uses_nv12(ff, "cmyk.jpg"), "cmyk avoids nv12");
}

// A 1:8 decode - what a very large image gets - returns one iMCU row per call, which for a source
// with no vertical chroma subsampling is a single luma row. The NV12 reader once averaged two chroma
// rows per output pair out of a buffer holding one, and indexed the pair from the call rather than
// from the image, so each output row took the wrong source row mixed with scratch libjpeg had never
// written.
//
// The fixture is eight-pixel bands alternating saturated and neutral, so at 1:8 every decoded row
// is one band and a correct decode reads the even, saturated ones. The assertion is on the distance
// between Cr and Cb, which is what each half of the defect destroys: reading the odd band collapses
// it to zero, and mixing in an unwritten row halves it. It is also what the unknown scratch value
// cancels out of - an absolute threshold depends on whatever the heap happened to hold, which is
// how the first version of this test passed against the bug it was written for.
static void should_decode_scaled_422_jpeg_as_nv12()
{
	files ff;

	// Re-encoding through a 4:2:2 source adopts its sampling factors, giving a 4:2:2 image large
	// enough for the smallest scale factor.
	const auto donor = ff.load(test_files_folder.combine_file("Small.jpg"), false);
	assert_equal(true, is_valid(donor.i), "loaded 4:2:2 source");

	// The encoder consumes the surface as BGRX, so this is a saturated primary rather than the blue
	// the argument order reads as. Which primary it is does not matter, only that it is far off
	// neutral in chroma while the alternating band sits exactly on it.
	constexpr auto extent = 1024;
	const auto saturated = ui::rgba(0, 0, 255);
	const auto neutral = ui::rgba(128, 128, 128);

	const auto banded = std::make_shared<ui::surface>();
	banded->alloc(extent, extent, ui::texture_format::RGB);

	for (auto y = 0; y < extent; ++y)
	{
		const auto band = ((y / 8) & 1) == 0 ? saturated : neutral;

		for (auto x = 0; x < extent; ++x)
		{
			banded->set_pixel(x, y, band);
		}
	}

	file_encode_params params;
	params.jpeg_source = donor.i->data();

	const auto encoded = ff.surface_to_image(banded, {}, params, ui::image_format::JPEG);
	assert_equal(true, is_valid(encoded), "encoded jpeg");

	// Without this the test is vacuous: 4:2:0 returns two luma rows per call, never reaches the
	// defect, and averages the two bands together as its own correct answer.
	const auto encoded_path = _temps.next_path(".jpg");
	df::blob_save_to_file(encoded->data(), encoded_path);
	assert_equal("yuv422", ff_scan_file(ff, encoded_path).pixel_format.sv(), "re-encoded as 4:2:2");

	// Shown at an eighth of its size, so the decode reduces by 8.
	bool is_yuv = false;
	const auto decoded = files_test_hooks::decode_jpeg(ff, encoded->data(), {extent / 8, extent / 8}, true,
	                                                   decode_intent::thumbnail, is_yuv);
	assert_equal(true, is_valid(decoded), "decoded at 1:8");
	assert_equal(true, is_yuv, "scaled 4:2:2 renders as nv12");
	assert_equal(true, decoded->dimensions() == sizei{extent / 8, extent / 8}, "at an eighth of the size");
	const auto& nv12 = *decoded;

	auto worst = 255;

	for (auto y = 0u; y < nv12.height() / 2; ++y)
	{
		const auto* const row = nv12.pixels() + nv12.stride() * (nv12.height() + y);

		for (auto x = 0u; x < nv12.width(); x += 2)
		{
			worst = std::min(worst, std::abs(static_cast<int>(row[x + 1]) - static_cast<int>(row[x])));
		}
	}

	assert_equal(true, worst >= 130, std::format("every chroma row reads its own band - lowest |Cr-Cb| {}", worst));
}

// Fixtures for the deep-precision and transfer-function paths are four flat horizontal bands, so a
// correct decode lands on known 8-bit values and a truncating or unconverted one visibly does not.
static void should_decode_bands(const char* const name, const std::initializer_list<int> expected, const int tolerance)
{
	// The fixtures live in excluded1 so that adding them does not change the indexed item counts.
	files ff;
	const auto loaded = ff.load(test_files_folder.combine("excluded1").combine_file(name), false);
	assert_equal(true, is_valid(loaded.i), std::format("loaded {}", name));

	const auto surface = loaded.to_surface();
	assert_equal(true, is_valid(surface), std::format("decoded {}", name));

	const auto cy = surface->height();
	auto band = 0;

	for (const auto want : expected)
	{
		const auto* const row = surface->pixels_line(cy * band / 4 + cy / 8);
		const auto message = std::format("{} band {}: wanted {}, got {},{},{},{}", name, band, want,
		                                 row[0], row[1], row[2], row[3]);

		for (auto c = 0; c < 3; c++)
		{
			assert_equal(true, std::abs(static_cast<int>(row[c]) - want) <= tolerance, message);
		}

		++band;
	}
}

// 12-bit lossy and 16-bit lossless JPEGs used to fail to load outright.
static void should_decode_deep_precision_jpeg(const char* const name)
{
	should_decode_bands(name, {0, 85, 170, 255}, 2);
}

static void should_decode_lossless_grayscale_jpeg(const int precision)
{
	jpeg_compress_struct encoder{};
	jpeg_error_mgr error{};
	encoder.err = jpeg_std_error(&error);
	error.error_exit = [](j_common_ptr info)
	{
		char message[JMSG_LENGTH_MAX]{};
		info->err->format_message(info, message);
		throw test_assert_exception(message);
	};
	const df::scope_exit destroy([&encoder] { jpeg_destroy_compress(&encoder); });
	jpeg_create_compress(&encoder);
	unsigned char* encoded = nullptr;
	unsigned long encoded_size = 0;
	const df::scope_exit free_encoded([&encoded] { std::free(encoded); });
	jpeg_mem_dest(&encoder, &encoded, &encoded_size);
	encoder.image_width = 3;
	encoder.image_height = 1;
	encoder.input_components = 1;
	encoder.in_color_space = JCS_GRAYSCALE;
	jpeg_set_defaults(&encoder);
	encoder.data_precision = precision;
	jpeg_enable_lossless(&encoder, 1, 0);
	jpeg_start_compress(&encoder, TRUE);

	const auto maximum = (1u << precision) - 1u;
	if (precision == 8)
	{
		JSAMPLE samples[] = {0, 85, 255};
		JSAMPROW row = samples;
		assert_equal(1u, jpeg_write_scanlines(&encoder, &row, 1), "grayscale fixture encoded");
	}
	else if (precision <= 12)
	{
		J12SAMPLE samples[] = {0, static_cast<J12SAMPLE>(maximum / 3), static_cast<J12SAMPLE>(maximum)};
		J12SAMPROW row = samples;
		assert_equal(1u, jpeg12_write_scanlines(&encoder, &row, 1), "wide grayscale fixture encoded");
	}
	else
	{
		J16SAMPLE samples[] = {0, static_cast<J16SAMPLE>(maximum / 3), static_cast<J16SAMPLE>(maximum)};
		J16SAMPROW row = samples;
		assert_equal(1u, jpeg16_write_scanlines(&encoder, &row, 1), "deep grayscale fixture encoded");
	}
	jpeg_finish_compress(&encoder);

	files ff;
	bool is_yuv = false;
	const auto surface = files_test_hooks::decode_jpeg(ff, {encoded, encoded_size}, {}, false, decode_intent::display,
	                                                   is_yuv);
	assert_equal(true, is_valid(surface), "the JPEG decoder decodes it without another codec's fallback");
	constexpr uint8_t expected[] = {0, 85, 255};
	for (auto x = 0; x < 3; ++x)
		for (auto channel = 0; channel < 3; ++channel)
			assert_equal(expected[x], surface->pixels_line(0)[x * 4 + channel], "grayscale expands and scales correctly");
}

static void should_decode_associated_alpha_jxl()
{
	const std::unique_ptr<JxlEncoder, decltype(&JxlEncoderDestroy)> encoder(JxlEncoderCreate(nullptr), JxlEncoderDestroy);
	assert_equal(true, encoder != nullptr, "JXL encoder created");
	JxlBasicInfo info{};
	JxlEncoderInitBasicInfo(&info);
	info.xsize = 2;
	info.ysize = 1;
	info.bits_per_sample = 8;
	info.uses_original_profile = JXL_TRUE;
	info.num_color_channels = 3;
	info.num_extra_channels = 1;
	info.alpha_bits = 8;
	info.alpha_premultiplied = JXL_TRUE;
	assert_equal(JXL_ENC_SUCCESS, JxlEncoderSetBasicInfo(encoder.get(), &info), "JXL basic info accepted");
	JxlColorEncoding color{};
	JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
	assert_equal(JXL_ENC_SUCCESS, JxlEncoderSetColorEncoding(encoder.get(), &color), "JXL color encoding accepted");
	auto* settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
	assert_equal(true, settings != nullptr, "JXL frame settings created");
	assert_equal(JXL_ENC_SUCCESS, JxlEncoderSetFrameLossless(settings, JXL_TRUE), "lossless JXL fixture");
	const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
	constexpr uint8_t pixels[] = {64, 32, 16, 128, 255, 128, 0, 255};
	assert_equal(JXL_ENC_SUCCESS, JxlEncoderAddImageFrame(settings, &format, pixels, sizeof(pixels)), "JXL pixels accepted");
	JxlEncoderCloseInput(encoder.get());
	std::array<uint8_t, 4096> encoded{};
	auto* next = encoded.data();
	auto available = encoded.size();
	assert_equal(JXL_ENC_SUCCESS, JxlEncoderProcessOutput(encoder.get(), &next, &available), "JXL fixture encoded");

	mem_read_stream stream(df::cspan{encoded.data(), encoded.size() - available});
	const auto surface = load_jxl(stream);
	assert_equal(true, is_valid(surface), "associated-alpha JXL decodes");
	constexpr int expected[] = {32, 64, 128};
	for (auto channel = 0; channel < 3; ++channel)
		assert_equal(true, std::abs(static_cast<int>(surface->pixels_line(0)[channel]) - expected[channel]) <= 1,
		             "JXL float-to-byte quantization stays within one level of straight alpha");
	assert_equal(128, surface->pixels_line(0)[3], "unpremultiplication preserves alpha");
	assert_equal(0xffff8000u, surface->get_pixel(1, 0), "opaque colors remain unchanged");
}

static void should_decode_associated_alpha_heif()
{
	const std::unique_ptr<heif_context, decltype(&heif_context_free)> context(heif_context_alloc(), heif_context_free);
	assert_equal(true, context != nullptr, "HEIF context created");
	heif_image* raw_image = nullptr;
	assert_equal(heif_error_Ok, heif_image_create(2, 1, heif_colorspace_RGB, heif_chroma_interleaved_RGB, &raw_image).code,
	             "HEIF image created");
	const std::unique_ptr<heif_image, decltype(&heif_image_release)> image(raw_image, heif_image_release);
	assert_equal(heif_error_Ok, heif_image_add_plane(image.get(), heif_channel_interleaved, 2, 1, 8).code,
	             "HEIF plane allocated");
	int stride = 0;
	auto* plane = heif_image_get_plane(image.get(), heif_channel_interleaved, &stride);
	assert_equal(true, plane != nullptr && stride >= 6, "HEIF plane available");
	std::memset(plane, 0, stride);
	constexpr uint8_t pixels[] = {64, 32, 16, 255, 128, 0};
	std::memcpy(plane, pixels, sizeof(pixels));
	heif_image_set_premultiplied_alpha(image.get(), true);

	heif_encoder* raw_encoder = nullptr;
	assert_equal(heif_error_Ok, heif_context_get_encoder_for_format(context.get(), heif_compression_uncompressed,
	                                                               &raw_encoder).code, "uncompressed HEIF encoder available");
	const std::unique_ptr<heif_encoder, decltype(&heif_encoder_release)> encoder(raw_encoder, heif_encoder_release);
	heif_image_handle* raw_handle = nullptr;
	assert_equal(heif_error_Ok, heif_context_encode_image(context.get(), image.get(), encoder.get(), nullptr, &raw_handle).code,
	             "associated-alpha HEIF encoded");
	const std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle(raw_handle, heif_image_handle_release);
	// The uncompressed encoder does not create external-alpha prem/auxl references.
	heif_image* raw_alpha = nullptr;
	assert_equal(heif_error_Ok, heif_image_create(2, 1, heif_colorspace_monochrome, heif_chroma_monochrome, &raw_alpha).code,
	             "HEIF auxiliary alpha image created");
	const std::unique_ptr<heif_image, decltype(&heif_image_release)> alpha(raw_alpha, heif_image_release);
	assert_equal(heif_error_Ok, heif_image_add_plane(alpha.get(), heif_channel_Y, 2, 1, 8).code, "HEIF alpha plane allocated");
	auto* alpha_plane = heif_image_get_plane(alpha.get(), heif_channel_Y, &stride);
	assert_equal(true, alpha_plane && stride >= 2, "HEIF alpha plane available");
	std::memset(alpha_plane, 0, stride);
	alpha_plane[0] = 128;
	alpha_plane[1] = 255;
	heif_image_handle* raw_alpha_handle = nullptr;
	assert_equal(heif_error_Ok, heif_context_encode_image(context.get(), alpha.get(), encoder.get(), nullptr,
	                                                    &raw_alpha_handle).code, "HEIF auxiliary alpha encoded");
	const std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> alpha_handle(
		raw_alpha_handle, heif_image_handle_release);
	const auto image_id = heif_image_handle_get_item_id(handle.get());
	const auto alpha_id = heif_image_handle_get_item_id(alpha_handle.get());
	constexpr char alpha_type[] = "\0\0\0\0urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
	assert_equal(heif_error_Ok, heif_item_add_raw_property(context.get(), alpha_id, heif_fourcc('a', 'u', 'x', 'C'),
	                                                     nullptr, reinterpret_cast<const uint8_t*>(alpha_type),
	                                                     sizeof(alpha_type), true, nullptr).code, "alpha role declared");
	assert_equal(heif_error_Ok, heif_context_add_item_reference(context.get(), heif_fourcc('a', 'u', 'x', 'l'),
	                                                          alpha_id, image_id).code, "alpha attached");
	assert_equal(heif_error_Ok, heif_context_add_item_reference(context.get(), heif_fourcc('p', 'r', 'e', 'm'),
	                                                          image_id, alpha_id).code, "associated alpha declared");
	const auto path = _temps.next_path(".heif");
	assert_equal(heif_error_Ok, heif_context_write_to_file(context.get(), path.str().c_str()).code, "HEIF fixture saved");
	const std::unique_ptr<heif_context, decltype(&heif_context_free)> check_context(heif_context_alloc(), heif_context_free);
	assert_equal(true, check_context != nullptr, "HEIF validation context created");
	const auto opened = heif_context_read_from_file(check_context.get(), path.str().c_str(), nullptr);
	assert_equal(heif_error_Ok, opened.code, opened.message);
	heif_image_handle* raw_check_handle = nullptr;
	const auto primary = heif_context_get_primary_image_handle(check_context.get(), &raw_check_handle);
	const std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> check_handle(
		raw_check_handle, heif_image_handle_release);
	assert_equal(heif_error_Ok, primary.code, primary.message);
	assert_equal(true, heif_image_handle_is_premultiplied_alpha(check_handle.get()) != 0, "fixture declares associated alpha");
	heif_image* raw_decoded = nullptr;
	const auto decoded = heif_decode_image(check_handle.get(), &raw_decoded, heif_colorspace_RGB,
	                                       heif_chroma_interleaved_RGBA, nullptr);
	const std::unique_ptr<heif_image, decltype(&heif_image_release)> decoded_image(raw_decoded, heif_image_release);
	assert_equal(heif_error_Ok, decoded.code, decoded.message);
	file_read_stream stream;
	assert_equal(true, stream.open(path), "HEIF fixture opened");
	const auto surface = load_heif(stream);
	assert_equal(true, is_valid(surface), "associated-alpha HEIF decodes");
	assert_equal(0x80804020u, surface->get_pixel(0, 0), "HEIF associated colors become straight-alpha BGRA");
	assert_equal(0xffff8000u, surface->get_pixel(1, 0), "HEIF opaque colors remain unchanged");
}

// Scaling 16-bit samples gives 1, 33, 65, 97 where the old truncation gave 0, 32, 64, 96, so this
// only tells the two apart if it demands the exact value.
static void should_scale_16bit_png()
{
	should_decode_bands("deep16.png", {1, 33, 65, 97}, 0);
}

// gamma.png declares gAMA 1.0, so its linear samples need an ~1/2.2 encode for an sRGB display.
static void should_apply_png_gamma()
{
	should_decode_bands("gamma.png", {0, 136, 186, 224}, 2);
}

static void append_png_be32(df::blob& out, const uint32_t value)
{
	out.push_back(static_cast<uint8_t>(value >> 24));
	out.push_back(static_cast<uint8_t>(value >> 16));
	out.push_back(static_cast<uint8_t>(value >> 8));
	out.push_back(static_cast<uint8_t>(value));
}

static void insert_png_chunk_before_iend(df::blob& png, const char (&type)[5], const df::cspan data)
{
	df::blob chunk;
	append_png_be32(chunk, static_cast<uint32_t>(data.size));
	const auto type_start = chunk.size();
	chunk.insert(chunk.end(), type, type + 4);
	chunk.insert(chunk.end(), data.data, data.data + data.size);

	auto crc = crc32(0, Z_NULL, 0);
	crc = crc32(crc, chunk.data() + type_start, static_cast<uInt>(4 + data.size));
	append_png_be32(chunk, crc);

	assert_equal(true, png.size() >= 12u, "png has an IEND chunk");
	png.insert(png.end() - 12, chunk.begin(), chunk.end());
}

// SRC-019 - pixel decode must finish metadata reading so eXIf after IDAT still orients the loaded
// surface.
static void should_read_png_orientation_after_idat()
{
	files ff;
	const auto image = ff.surface_to_image(make_gradient_surface(16, 8), {}, {}, ui::image_format::PNG);
	const auto& encoded = image->data();
	df::blob png(encoded.data(), encoded.data() + encoded.size());

	auto exif = make_orientation_exif(ui::orientation::right_top);
	exif.insert(exif.begin(), exif_signature.begin(), exif_signature.end());
	insert_png_chunk_before_iend(png, "eXIf", {exif.data(), exif.size()});

	const auto path = _temps.next_path(".png");
	df::blob_save_to_file(png, path);

	const auto loaded = load_png(df::blob_from_file(path));
	assert_equal(true, is_valid(loaded), "png loaded");
	assert_equal(static_cast<int>(ui::orientation::right_top), static_cast<int>(loaded->orientation()),
	             "trailing eXIf orientation");
}

static void should_decode_12bit_gray_jpeg()
{
	should_decode_deep_precision_jpeg("deep12gray.jpg");
}

static void should_decode_12bit_colour_jpeg()
{
	should_decode_deep_precision_jpeg("deep12.jpg");
}

static void should_decode_16bit_gray_jpeg()
{
	should_decode_deep_precision_jpeg("deep16gray.jpg");
}

static void should_decode_16bit_colour_jpeg()
{
	should_decode_deep_precision_jpeg("deep16.jpg");
}

static void should_decode_thin_jpeg_when_planar_is_preferred()
{
	files ff;

	const auto decode = [&](const int cx, const int cy, const sizei target_extent, const std::string_view name)
	{
		const auto encoded = ff.surface_to_image(make_gradient_surface(cx, cy), {}, {}, ui::image_format::JPEG);
		assert_equal(true, is_valid(encoded), std::format("{} encoded", name));

		const auto packed = ff.image_to_surface(encoded, target_extent, false, {}, decode_intent::display);
		assert_equal(true, ui::is_valid(packed), std::format("{} packed decode succeeds", name));

		const auto planar_preferred = ff.image_to_surface(encoded, target_extent, true, {}, decode_intent::display);
		assert_equal(true, ui::is_valid(planar_preferred), std::format("{} planar-preferred decode succeeds", name));
		assert_equal(true, planar_preferred->format() == ui::texture_format::RGB,
		             std::format("{} falls back to packed RGB", name));
		assert_equal(true, packed->dimensions() == planar_preferred->dimensions(),
		             std::format("{} keeps packed dimensions", name));
		assert_equal(packed->orientation(), planar_preferred->orientation(),
		             std::format("{} keeps orientation", name));
	};

	decode(1, 8, {}, "one-pixel width");
	decode(8, 1, {}, "one-pixel height");
	decode(8, 8, {1, 1}, "scaled to one pixel");
}

// The pixel format is what the properties panel and list rows show, and it is indexed for search.
// Chroma subsampling is a headline property of a JPEG, so it belongs in that name - and it has to
// use the same words HEIF, WebP and video already use or a search for one finds only some of them.
static void should_report_jpeg_chroma_subsampling()
{
	files ff;

	const auto reported = [&ff](const char* const name)
	{
		return ff_scan_file(ff, test_files_folder.combine_file(name)).pixel_format;
	};

	assert_equal("yuv420", reported("exif-rotated.jpg").sv(), "4:2:0 jpeg");
	assert_equal("yuv422", reported("Small.jpg").sv(), "4:2:2 jpeg");
	assert_equal("ycck", reported("cmyk.jpg").sv(), "adobe ycck jpeg");
}

struct webp_chunk
{
	std::string tag;
	df::blob data;
};

static std::vector<webp_chunk> read_webp_chunks(const df::file_path path)
{
	file_read_stream fs;
	assert_equal(true, fs.open(path), "open webp");

	const auto bytes = fs.read_all();
	assert_equal(true, bytes.size() > 12u, "webp larger than the RIFF header");

	std::vector<webp_chunk> result;

	for (auto pos = size_t{12}; pos + 8 <= bytes.size();)
	{
		uint32_t len = 0;
		memcpy(&len, bytes.data() + pos + 4, 4);

		const auto payload = pos + 8;
		if (len > bytes.size() - payload) break; // subtract, so a 32-bit size_t cannot wrap

		result.emplace_back(std::string(reinterpret_cast<const char*>(bytes.data() + pos), 4),
		                    df::blob(bytes.begin() + payload, bytes.begin() + payload + len));
		pos = payload + len + (len & 1);
	}

	return result;
}

// Regression guard for the WebP metadata rewrite. The XMP handler used to re-emit
// chunks grouped by category and then truncate the file, so a save could move the
// XMP packet on top of image or alpha data and destroy the picture. Saving metadata
// must leave every non-XMP chunk byte-identical and in its original position, and
// the image must still decode unchanged.
static void should_preserve_webp_chunks_on_metadata_save()
{
	files ff;
	const auto load_path = test_files_folder.combine_file("lake.webp");
	const auto save_path = _temps.next_path(".webp");

	const auto original = ff.load(load_path, false);
	assert_equal(false, original.is_empty(), "original webp loaded");

	metadata_edits first;
	first.rating = 3;
	ff.update(load_path, save_path, first, {}, {}, false, {});
	const auto after_first = read_webp_chunks(save_path);

	metadata_edits second;
	second.rating = 5;
	ff.update(save_path, second, {}, {}, false, {});
	const auto after_second = read_webp_chunks(save_path);

	assert_equal(after_first.size(), after_second.size(), "chunk count unchanged");
	assert_equal(true, after_first.size() >= 3u, "extended webp has image and metadata chunks");
	assert_equal(true, std::ranges::any_of(after_first, [](const webp_chunk& c) { return c.tag == "XMP "; }),
	             "xmp chunk written");

	for (auto i = size_t{0}; i < after_first.size() && i < after_second.size(); ++i)
	{
		assert_equal(after_first[i].tag, after_second[i].tag, std::format("chunk {} tag", i));

		if (after_first[i].tag != "XMP ")
		{
			assert_equal(true, after_first[i].data == after_second[i].data,
			             std::format("chunk {} {} bytes unchanged", i, after_first[i].tag));
		}
	}

	const auto reloaded = ff.load(save_path, false);
	assert_equal(false, reloaded.is_empty(), "webp still decodes");
	assert_equal(original.i->width(), reloaded.i->width(), "width preserved");
	assert_equal(original.i->height(), reloaded.i->height(), "height preserved");

	const auto scanned = ff_scan_file(ff, save_path);
	assert_equal(5, scanned.to_props()->rating, "rating written");
}

// Regression guard for the WebP save-quality path. The editor maps the user's
// save settings (setting.webp_lossless / setting.webp_quality) onto
// file_encode_params before writing; a swapped or ignored knob would silently
// bloat or degrade the images users save. Verify the encoder honours both:
// lossless produces a substantially larger file than a heavily compressed lossy
// encode of the same pixels, and a higher quality produces a larger file than
// the lowest quality - proving each parameter actually takes effect.
static void should_honor_webp_save_quality()
{
	files ff;
	const auto load_path = test_files_folder.combine_file("Test.jpg");
	const auto source = ff.load(load_path, false);
	assert_equal(false, source.is_empty(), "source loaded");

	const auto lossless_path = _temps.next_path(".webp");
	{
		file_encode_params params;
		params.webp_lossless = true;
		ff.update(load_path, lossless_path, {}, {}, params, false, {});
	}

	const auto low_quality_path = _temps.next_path(".webp");
	{
		file_encode_params params;
		params.webp_lossless = false;
		params.webp_quality = 1;
		ff.update(load_path, low_quality_path, {}, {}, params, false, {});
	}

	const auto high_quality_path = _temps.next_path(".webp");
	{
		file_encode_params params;
		params.webp_lossless = false;
		params.webp_quality = 95;
		ff.update(load_path, high_quality_path, {}, {}, params, false, {});
	}

	const auto lossless_size = platform::file_attributes(lossless_path).size;
	const auto low_quality_size = platform::file_attributes(low_quality_path).size;
	const auto high_quality_size = platform::file_attributes(high_quality_path).size;

	assert_equal(true, lossless_size > 0 && low_quality_size > 0 && high_quality_size > 0, "webp files written");

	// Lossless keeps every detail, so it must be much larger than a heavily
	// compressed lossy encode of the same pixels.
	assert_equal(true, lossless_size > low_quality_size, "webp lossless larger than low quality");

	// Higher quality retains more detail, so it must be larger than the lowest
	// quality - this proves webp_quality is applied (and not treated as a bool).
	assert_equal(true, high_quality_size > low_quality_size, "webp high quality larger than low quality");
}

// The WebP loader used to tag every surface ARGB, which forces the renderer down
// the alpha-blended path for images that are entirely opaque. Verify the decoded
// format follows the bitstream: opaque in, RGB out; alpha in, ARGB out.
static void should_tag_webp_surface_alpha()
{
	const auto opaque_path = test_files_folder.combine_file("lake.webp");
	const auto opaque_data = df::blob_from_file(opaque_path);
	assert_equal(false, opaque_data.empty(), "opaque webp read");

	const auto opaque_surface = load_webp(opaque_data);
	assert_equal(true, is_valid(opaque_surface), "opaque webp decoded");
	assert_equal(true, opaque_surface->format() == ui::texture_format::RGB, "opaque webp surface is RGB");

	const auto transparent = std::make_shared<ui::surface>();
	const auto* const pixels = transparent->alloc(16, 16, ui::texture_format::ARGB);
	assert_equal(true, pixels != nullptr, "alpha surface allocated");

	for (auto y = 0; y < 16; ++y)
	{
		auto* const line = std::bit_cast<ui::color32*>(transparent->pixels_line(y));

		for (auto x = 0; x < 16; ++x)
		{
			// BGRA in memory: alpha ramps across the row so the encode keeps an alpha plane.
			line[x] = (static_cast<ui::color32>(x * 16) << 24) | 0x00FF8040u;
		}
	}

	file_encode_params params;
	params.webp_lossless = true;
	const auto encoded = save_webp(transparent, {}, params);
	assert_equal(true, is_valid(encoded), "alpha webp encoded");

	const auto decoded = load_webp(encoded->data());
	assert_equal(true, is_valid(decoded), "alpha webp decoded");
	assert_equal(true, decoded->format() == ui::texture_format::ARGB, "alpha webp surface is ARGB");
}

static void should_decode_webp_at_the_requested_size()
{
	const auto data = df::blob_from_file(test_files_folder.combine_file("lake.webp"));

	const auto full = load_webp(data);
	assert_equal(true, is_valid(full), "the fixture decodes");

	const auto native = full->dimensions();
	const sizei target{native.cx / 4, native.cy / 4};

	// libwebp rescales inside the decoder, so the surface comes back at the size asked for rather
	// than at the file's size with a downscale still owed. Every caller of image_to_surface asks
	// for a bounded size, so this is the ordinary path, not a special one.
	const auto scaled = load_webp(data, false, target);
	assert_equal(true, is_valid(scaled), "a reduced decode succeeds");
	assert_equal(true, scaled->dimensions().cx <= target.cx, "the decode fits inside the requested width");
	assert_equal(true, scaled->dimensions().cy <= target.cy, "and inside the requested height");
	assert_equal(true, scaled->dimensions().cx > native.cx / 8, "and is not scaled past the target");
	assert_equal(true, scaled->format() == full->format(), "and keeps the format the file earned");

	// Aspect is preserved rather than stretched to the box, so a reduced decode frames the picture
	// the same way the full one does.
	const auto full_aspect = static_cast<double>(native.cx) / native.cy;
	const auto scaled_aspect = static_cast<double>(scaled->dimensions().cx) / scaled->dimensions().cy;
	assert_near(full_aspect, scaled_aspect, 0.02, "the reduced decode keeps the aspect ratio");

	// A target at or above the source is left alone, so nothing is ever enlarged into.
	const auto larger = load_webp(data, false, {native.cx * 2, native.cy * 2});
	assert_equal(native.cx, larger->dimensions().cx, "a larger target does not enlarge");

	// The reduced decode must produce the same picture as decoding in full and scaling afterwards,
	// which is the path it replaces. What it saves is the colour conversion and the scaler pass;
	// libwebp still walks the whole bitstream, so on a small file that saving is inside the noise and
	// is deliberately not asserted here.
	files ff;
	const auto scaled_after = ff.fit_within(load_webp(data, false), target);
	assert_equal(scaled_after->dimensions().cx, scaled->dimensions().cx,
	             "a reduced decode agrees with decode-then-scale on width");
	assert_equal(scaled_after->dimensions().cy, scaled->dimensions().cy,
	             "a reduced decode agrees with decode-then-scale on height");
}

static void should_decode_opaque_lossy_webp_as_nv12()
{
	const auto data = df::blob_from_file(test_files_folder.combine_file("lake.webp"));
	const auto rgb = load_webp(data, false);
	const auto nv12 = load_webp(data, true);

	assert_equal(true, is_valid(rgb) && rgb->format() == ui::texture_format::RGB, "webp RGB fallback decoded");
	assert_equal(true, is_valid(nv12) && nv12->format() == ui::texture_format::NV12, "webp NV12 decoded");
	assert_equal(true, nv12->size() * 2 < rgb->size(), "webp NV12 uses less than half the RGB surface memory");
	assert_equal(true, nv12->color_space() == ui::color_space::rec601_limited, "webp NV12 color space");

	const auto converted = std::make_shared<ui::surface>();
	av_scaler scaler;
	assert_equal(true, scaler.convert_yuv_surface(*nv12, converted), "webp NV12 converts for comparison");

	uint64_t total_difference = 0;
	const auto dimensions = rgb->dimensions();

	for (auto y = 0; y < dimensions.cy; ++y)
	{
		const auto* const expected = rgb->pixels_line(y);
		const auto* const actual = converted->pixels_line(y);

		for (auto x = 0; x < dimensions.cx * 4; x += 4)
		{
			for (auto channel = 0; channel < 3; ++channel)
			{
				total_difference += std::abs(static_cast<int>(expected[x + channel]) - actual[x + channel]);
			}
		}
	}

	const auto average_difference = static_cast<double>(total_difference) / (dimensions.cx * dimensions.cy * 3);
	assert_equal(true, average_difference < 3.0,
	             std::format("webp NV12 average RGB difference: {}", average_difference));

	// VP8 encodes YCbCr 4:2:0, so an NV12 surface is handed over as planes rather than converted to
	// packed pixels for libwebp to convert straight back. The picture has to survive that, which is
	// what catches a wrong plane order, a missed de-interleave or a skipped range conversion.
	{
		file_encode_params planar_params;
		planar_params.webp_quality = thumbnail_webp_quality;
		planar_params.webp_fast = true;

		const auto planar_encoded = save_webp(nv12, {}, planar_params);
		assert_equal(true, is_valid(planar_encoded), "nv12 encodes as webp through the planar path");

		files planar_ff;
		const auto round_trip = planar_ff.image_to_surface(planar_encoded, {}, false);
		assert_equal(true, ui::is_valid(round_trip), "the planar webp decodes again");
		assert_equal(true, round_trip->dimensions() == dimensions, "the planar webp keeps its size");

		uint64_t planar_difference = 0;

		for (auto y = 0; y < dimensions.cy; ++y)
		{
			const auto* const expected = rgb->pixels_line(y);
			const auto* const actual = round_trip->pixels_line(y);

			for (auto x = 0; x < dimensions.cx * 4; x += 4)
			{
				for (auto channel = 0; channel < 3; ++channel)
				{
					planar_difference += std::abs(static_cast<int>(expected[x + channel]) - actual[x + channel]);
				}
			}
		}

		const auto planar_average = static_cast<double>(planar_difference) / (dimensions.cx * dimensions.cy * 3);
		// Measured 2.07: one further lossy generation of an already lossy source. A wrong plane order
		// or a missed de-interleave is a colour swap, which lands an order of magnitude above this.
		assert_equal(true, planar_average < 6.0,
		             std::format("planar webp round trip average difference: {}", planar_average));
	}

	files ff;
	const auto image = std::make_shared<ui::image>(df::cspan(data), dimensions, ui::image_format::WEBP,
	                                              ui::orientation::top_left);
	const auto dispatched = ff.image_to_surface(image, {}, true);
	assert_equal(true, is_valid(dispatched) && dispatched->format() == ui::texture_format::NV12,
	             "webp image dispatch preserves NV12");

	const auto target_extent = sizei{32, 32};
	const auto scaled = ff.image_to_surface(image, target_extent, true);
	assert_equal(true, is_valid(scaled) && scaled->format() == ui::texture_format::RGB,
	             "webp NV12 downscale produces target RGB");
	assert_equal(true, ui::scale_dimensions(dimensions, target_extent) == scaled->dimensions(),
	             "webp downscale honors target extent");
}

// A JPEG thumbnail used to be decoded YCbCr -> packed by libjpeg, reduced, then converted packed ->
// YCbCr again by libwebp before VP8 saw it. Two conversions, the first at the DCT-scaled size, both
// discarded. Keeping the planes throughout is what removes them, and the reduction has to stay
// planar for that to hold - which is what this pins.
static void should_thumbnail_a_jpeg_without_leaving_yuv()
{
	files ff;
	const auto data = df::blob_from_file(test_files_folder.combine_file("Test.jpg"));
	assert_equal(true, !data.empty(), "loaded jpeg bytes");

	constexpr sizei ceiling{256, 256};
	const auto surface = ff.image_to_surface(data, ceiling, true, decode_intent::thumbnail);

	assert_equal(true, ui::is_valid(surface), "thumbnail decode produced a surface");
	assert_equal(true, surface->format() == ui::texture_format::NV12, "a 4:2:0 jpeg stays planar to the encoder");
	assert_equal(true, surface->color_space() == ui::color_space::rec601_full, "jpeg planes are full range");

	const auto extent = surface->dimensions();
	assert_equal(true, extent.cx <= ceiling.cx && extent.cy <= ceiling.cy, "the planar reduction honours the ceiling");
	assert_equal(true, ((extent.cx | extent.cy) & 1) == 0, "the planar reduction lands on an even extent");

	const auto thumb = ff.surface_to_thumbnail(surface);
	assert_equal(true, is_valid(thumb) && thumb->format() == ui::image_format::WEBP,
	             "the planar surface encodes as webp");

	// The range conversion is the one thing planes do not carry across: libwebp's YUV is limited
	// range and VP8 signals none, so a full-range source handed over untouched comes back with
	// crushed blacks. Compare against the packed decode of the same file, which never left full range.
	const auto packed = ff.image_to_surface(data, ceiling, false, decode_intent::thumbnail);
	const auto decoded = ff.image_to_surface(thumb, {}, false);

	assert_equal(true, ui::is_valid(packed) && ui::is_valid(decoded), "both comparison surfaces decoded");

	// The planar reduction rounds down to an even extent, so the two can differ by a pixel on an
	// axis. Compare the region they share.
	const auto common = sizei{
		std::min(packed->dimensions().cx, decoded->dimensions().cx),
		std::min(packed->dimensions().cy, decoded->dimensions().cy)
	};

	assert_equal(true, packed->dimensions().cx - common.cx <= 1 && packed->dimensions().cy - common.cy <= 1,
	             "the thumbnail is within a pixel of the packed decode on each axis");

	uint64_t difference = 0;

	for (auto y = 0; y < common.cy; ++y)
	{
		const auto* const expected = packed->pixels_line(y);
		const auto* const actual = decoded->pixels_line(y);

		for (auto x = 0; x < common.cx * 4; x += 4)
		{
			for (auto channel = 0; channel < 3; ++channel)
			{
				difference += std::abs(static_cast<int>(expected[x + channel]) - actual[x + channel]);
			}
		}
	}

	// Measured 3.82 with the range conversion and 8.05 without it, so this separates the two cleanly.
	const auto average = static_cast<double>(difference) / (common.cx * common.cy * 3);
	assert_equal(true, average < 5.5, std::format("planar thumbnail average difference: {}", average));
}

// The caller decides the pixel format, because only it knows whether these pixels are bound for a
// texture, for a luma plane or for a packed reader. The latch a driver fault clears -
// ui::yuv_textures_allowed, kept across runs in setting.use_yuv - reaches the decoders only through
// ui::yuv_textures_enabled at the display call site; a decoder that read it directly would also
// force packed on an analysis or thumbnail decode that never goes near a texture.
static void should_let_the_caller_choose_the_decoded_pixel_format()
{
	const auto saved = setting.use_yuv;
	const auto saved_allowed = ui::yuv_textures_allowed;
	const df::scope_exit restore([saved, saved_allowed]
	{
		setting.use_yuv = saved;
		ui::yuv_textures_allowed = saved_allowed;
	});

	files ff;
	const auto webp = df::blob_from_file(test_files_folder.combine_file("lake.webp"));
	const auto jpeg = ff.load(test_files_folder.combine_file("exif-rotated.jpg"), false);
	assert_equal(true, is_valid(jpeg.i), "loaded jpeg");

	const auto webp_yuv = load_webp(webp, true);
	const auto jpeg_yuv = jpeg.to_surface({}, true);
	assert_equal(true, is_valid(webp_yuv) && webp_yuv->format() == ui::texture_format::NV12,
	             "webp nv12 when the caller allows it");
	assert_equal(true, is_valid(jpeg_yuv) && jpeg_yuv->format() == ui::texture_format::NV12,
	             "jpeg nv12 when the caller allows it");

	const auto webp_packed = load_webp(webp, false);
	const auto jpeg_packed = jpeg.to_surface({}, false);
	assert_equal(true, is_valid(webp_packed) && webp_packed->format() == ui::texture_format::RGB,
	             "webp rgb when the caller refuses planar");
	assert_equal(true, is_valid(jpeg_packed) && jpeg_packed->format() == ui::texture_format::RGB,
	             "jpeg rgb when the caller refuses planar");

	// The latch belongs to the texture upload, not to the decode. A thumbnail or analysis decode on
	// a machine whose driver faulted must still be free to take the cheaper planar path.
	setting.use_yuv = false;
	ui::yuv_textures_allowed = false;
	const auto webp_latched = load_webp(webp, true);
	const auto jpeg_latched = jpeg.to_surface({}, true);
	assert_equal(true, is_valid(webp_latched) && webp_latched->format() == ui::texture_format::NV12,
	             "webp still honours the caller with the fault latch set");
	assert_equal(true, is_valid(jpeg_latched) && jpeg_latched->format() == ui::texture_format::NV12,
	             "jpeg still honours the caller with the fault latch set");
}

// A thumbnail scaled to fit a box is regularly odd on one axis. decode_jpeg crops those to even and
// still reaches the GPU as NV12; a webp decoder that instead falls back to RGB costs 4 bytes per
// pixel rather than 1.5, for the majority of a collection's thumbnails.
static void should_decode_odd_sized_webp_as_nv12()
{
	constexpr sizei odd_extent{321, 215};
	const auto surface = std::make_shared<ui::surface>();
	assert_equal(true, surface->alloc(odd_extent, ui::texture_format::RGB) != nullptr, "allocated odd surface");

	for (auto y = 0; y < odd_extent.cy; ++y)
	{
		for (auto x = 0; x < odd_extent.cx; ++x)
		{
			surface->set_pixel(x, y, ui::rgba(x & 0xff, y & 0xff, (x + y) & 0xff));
		}
	}

	file_encode_params params;
	params.webp_quality = thumbnail_webp_quality;
	params.webp_fast = true;

	const auto encoded = save_webp(surface, {}, params);
	assert_equal(true, is_valid(encoded), "encoded odd webp");

	const auto decoded = load_webp(encoded->data(), true);
	assert_equal(true, is_valid(decoded) && decoded->format() == ui::texture_format::NV12,
	             "odd webp decodes as nv12");
	assert_equal(320, decoded->dimensions().cx, "odd webp width cropped to even");
	assert_equal(214, decoded->dimensions().cy, "odd webp height cropped to even");
}

// The shell returns 32-bit BGRA even for photo thumbnails, which are opaque, so a stored format
// chosen from the surface tag sent every cloud thumbnail down the PNG branch at several times the
// bytes. An opaque thumbnail must carry no alpha plane, or it also loses the NV12 decode path.
static void should_keep_thumbnail_alpha_only_when_needed()
{
	files ff;
	const auto loaded = ff.load(test_files_folder.combine_file("Test.jpg"), false);
	const auto photo = loaded.to_surface(setting.thumbnail_max_dimension, false, {}, decode_intent::thumbnail);
	assert_equal(true, ui::is_valid(photo), "loaded photo surface");

	const auto extent = photo->dimensions();
	const auto opaque = std::make_shared<ui::surface>();
	const auto translucent = std::make_shared<ui::surface>();
	assert_equal(true, opaque->alloc(extent, ui::texture_format::ARGB) != nullptr, "allocated opaque surface");
	assert_equal(true, translucent->alloc(extent, ui::texture_format::ARGB) != nullptr,
	             "allocated translucent surface");

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto c = photo->get_pixel(x, y);
			const auto rgb = ui::rgba(ui::get_r(c), ui::get_g(c), ui::get_b(c));
			opaque->set_pixel(x, y, rgb);
			translucent->set_pixel(x, y, x < extent.cx / 2 ? rgb & 0x00ffffff : rgb);
		}
	}

	const auto opaque_thumb = ff.surface_to_thumbnail(opaque);
	const auto translucent_thumb = ff.surface_to_thumbnail(translucent);

	assert_equal(true, is_valid(opaque_thumb) && opaque_thumb->format() == ui::image_format::WEBP,
	             "opaque thumbnail is stored as webp");
	assert_equal(true, is_valid(translucent_thumb) && translucent_thumb->format() == ui::image_format::WEBP,
	             "translucent thumbnail is stored as webp");

	// An opaque thumbnail that still carries an alpha plane cannot take the NV12 path.
	const auto opaque_surface = ff.image_to_surface(opaque_thumb, {}, true);
	assert_equal(true, ui::is_valid(opaque_surface) && opaque_surface->format() == ui::texture_format::NV12,
	             "opaque thumbnail drops its alpha plane and decodes as nv12");

	const auto translucent_surface = ff.image_to_surface(translucent_thumb, {}, true);
	assert_equal(true, ui::is_valid(translucent_surface) &&
	             translucent_surface->format() == ui::texture_format::ARGB,
	             "translucent thumbnail keeps its alpha plane");
	assert_equal(true, ui::get_a(translucent_surface->get_pixel(extent.cx / 4, extent.cy / 2)) < 128,
	             "transparent half survives the thumbnail round trip");
	assert_equal(true, ui::get_a(translucent_surface->get_pixel(extent.cx * 3 / 4, extent.cy / 2)) > 200,
	             "opaque half survives the thumbnail round trip");

	const auto png = save_png(translucent, {});
	assert_equal(true, is_valid(png), "encoded png reference");
	assert_equal(true, translucent_thumb->data().size() * 2 < png->data().size(),
	             "webp thumbnail is far smaller than the png it replaces");
}

static void should_refuse_truncated_webp_decode()
{
	const auto data = df::blob_from_file(test_files_folder.combine_file("lake.webp"));
	auto truncated_size = 0_z;

	for (auto size = 12_z; size < data.size(); ++size)
	{
		WebPBitstreamFeatures features;
		if (WebPGetFeatures(data.data(), size, &features) == VP8_STATUS_OK)
		{
			truncated_size = size;
			break;
		}
	}

	assert_equal(true, truncated_size > 0, "truncated webp retains a readable header");
	assert_equal(true, !is_valid(load_webp({data.data(), truncated_size})),
	             "truncated webp does not return an allocated partial surface");
}

// A file larger than can be held in memory used to be read as its first 100 MiB. The codec decoded
// what it was given and filled the missing rows itself, so the frame ended in a black tail - and on
// the edit path that frame was re-encoded straight over the user's original.
static void should_refuse_a_file_too_large_to_hold()
{
	const auto path = _temps.next_path(".jpg");

	// A valid JPEG header followed by enough bytes to pass the ceiling. Nothing decodes it; the
	// refusal happens on the file's size, before a byte is read.
	{
		const auto source = df::blob_from_file(test_files_folder.combine_file("Test.jpg"));
		assert_equal(false, source.empty(), "the fixture loaded");

		std::ofstream f(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
		f.write(std::bit_cast<const char*>(source.data()), static_cast<std::streamsize>(source.size()));

		const std::vector<char> filler(1024 * 1024, 0);
		auto written = source.size();

		while (written <= static_cast<size_t>(df::max_blob_size))
		{
			f.write(filler.data(), static_cast<std::streamsize>(filler.size()));
			written += filler.size();
		}
	}

	assert_equal(true, platform::file_attributes(path).size > static_cast<uint64_t>(df::max_blob_size),
	             "the fixture is past the ceiling");

	files ff;
	const auto loaded = ff.load(path, false);

	assert_equal(false, loaded.success, "an oversized file does not load");
	assert_equal(true, loaded.reason == file_load_result::failure::too_large,
	             "and says so as a property of the file rather than as an unreadable one");
	assert_equal(false, is_valid(loaded.i), "no truncated image is handed back");

	// The read itself refuses too, so no caller can obtain a prefix by accident. A caller that
	// asks for one by name still gets exactly what it asked for.
	auto threw = false;

	try
	{
		df::blob_from_file(path);
	}
	catch (const app_exception&)
	{
		threw = true;
	}

	assert_equal(true, threw, "reading the whole file refuses rather than truncating");
	assert_equal(1024_z, df::blob_head_from_file(path, 1024).size(), "an explicit prefix is still served");

	platform::delete_file(path);
}

static void should_convert_raw_to_jpeg()
{
	const auto load_path = test_files_folder.combine("raw").combine_file("Screws.CR2");
	const auto save_path = _temps.next_path(".jpg");

	files ff;
	ff.update(load_path, save_path, {}, {}, {}, false, {});

	const auto sr_expected = ff_scan_file(ff, load_path, detect_xmp_sidecar(load_path));
	const auto sr_actual = ff_scan_file(ff, save_path);

	const auto expected = sr_expected.to_props();
	const auto actual = sr_actual.to_props();

	assert_equal(expected->tags, actual->tags, "tags");
	assert_equal(expected->title, actual->title, "title");
	assert_equal(expected->description, actual->description, "description");
	assert_equal(expected->width, actual->width, "width");
	assert_equal(expected->height, actual->height, "height");
}

// Diffractor reads these and cannot write any of them, so there is no round trip to lean on: the
// fixtures are written by tools/make_test_images.py and the decode is checked against what that
// script drew. A gradient with a red corner is what makes a transposed, mirrored or channel-swapped
// decode fail here rather than merely look wrong later.
static void should_decode_a_read_only_format(const std::string_view name)
{
	files ff;
	const auto path = test_formats_folder.combine_file(name);
	const auto loaded = ff.load(path, false);

	// These decode during load rather than on demand, so the surface is what comes back.
	const auto surface = loaded.s;
	assert_equal(true, is_valid(surface), std::format("{} decoded", name));

	if (!is_valid(surface)) return;

	assert_equal(32, surface->dimensions().cx, std::format("{} width", name));
	assert_equal(24, surface->dimensions().cy, std::format("{} height", name));

	// The greyscale fixture carries one channel, so only the geometry above is comparable.
	if (name.ends_with(".pgm")) return;

	// Read by memory position: a surface is BGRA in memory, while ui::color32 spells the same bytes
	// the other way round, so naming the channels here is what keeps the check legible.
	const auto* const first = surface->pixels_line(0);
	const auto* const last = surface->pixels_line(surface->dimensions().cy - 1) +
		static_cast<size_t>(surface->dimensions().cx - 1) * 4;

	assert_equal(true, first[2] > 200 && first[1] < 60 && first[0] < 60,
	             std::format("{} top left is the red corner", name));
	assert_equal(true, last[0] > 100 && last[0] < 160, std::format("{} carries the blue channel", name));
}

static void should_save(const std::string_view ext, const bool should_support_metadata)
{
	const auto save_path = _temps.next_path(ext);
	const auto load_path = test_files_folder.combine_file("Test.jpg");

	files ff;
	constexpr image_edits color;
	ff.update(load_path, save_path, {}, color, {}, false, {});

	const auto expected = extract_properties(load_path);
	const auto actual = extract_properties(save_path);

	assert_equal(expected->width, actual->width);
	assert_equal(expected->height, actual->height);

	if (should_support_metadata)
	{
		assert_metadata(*expected, *actual, save_path.name());
	}
}

static void should_not_rewrite_unchanged_file()
{
	const auto path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), path, false, false);
	const auto modified_before = platform::file_attributes(path).modified;

	files ff;
	const auto result = ff.update(path, {}, {}, {}, false, {});

	assert_equal(true, result.success(), "unchanged save succeeds");
	assert_equal(modified_before, platform::file_attributes(path).modified, "unchanged save preserves modified time");
}

// The three readers of what a write produced are all served from one scan taken behind the write.
// This is the display's share of it: the bytes come back as an image, so nothing reads the file
// again to draw what was just saved.
static void should_return_written_image()
{
	const auto path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), path, false, false);

	files ff;
	const auto before = ff.load(path, false);

	rescan_spec rescan;
	rescan.wanted = true;
	rescan.load_thumbnail = true;
	rescan.want_image = true;
	rescan.file_type = files::file_type_from_name(path);

	metadata_edits edits;
	edits.rating = 3;
	const auto result = ff.update(path, edits, {}, {}, false, {}, rescan);

	assert_equal(true, result.success(), std::format("rating saved ({})", result.format_error()));
	assert_equal(true, result.scanned, "write scanned what it wrote");
	assert_equal(true, result.loaded.success, "write returned the image it wrote");
	assert_equal(before.i->width(), result.loaded.i->width(), "written image width");
	assert_equal(before.i->height(), result.loaded.i->height(), "written image height");

	// The scan only wraps the written bytes when a caller asked for them, and asking for a thumbnail
	// is a separate request. Without this the assertions above pass on the thumbnail's half of the
	// gate and say nothing about want_image.
	rescan_spec image_only;
	image_only.wanted = true;
	image_only.load_thumbnail = false;
	image_only.want_image = true;
	image_only.file_type = files::file_type_from_name(path);

	metadata_edits more_edits;
	more_edits.rating = 4;
	const auto image_only_result = ff.update(path, more_edits, {}, {}, false, {}, image_only);

	assert_equal(true, image_only_result.success(),
	             std::format("rating saved ({})", image_only_result.format_error()));
	assert_equal(true, image_only_result.loaded.success, "want_image alone returns the written image");
	assert_equal(before.i->width(), image_only_result.loaded.i->width(), "image-only written width");
	assert_equal(before.i->height(), image_only_result.loaded.i->height(), "image-only written height");
}

// The AV display's share. A container can be gigabytes, so it takes the handle rather than the
// bytes; reading it back is what proves the handle refers to the swapped-in file and not the stage.
static void should_hand_over_written_handle()
{
	const auto path = _temps.next_path(".mp3");
	platform::copy_file(test_files_folder.combine_file("Colorblind.mp3"), path, false, false);

	files ff;

	rescan_spec rescan;
	rescan.want_handle = true;

	metadata_edits edits;
	edits.rating = 3;
	const auto result = ff.update(path, edits, {}, {}, false, {}, rescan);

	assert_equal(true, result.success(), std::format("rating saved ({})", result.format_error()));
	assert_equal(true, result.staged, "edit staged and swapped");
	assert_equal(true, result.display_handle != nullptr, "write handed over its open handle");

	const auto expected = df::blob_from_file(path);
	df::blob actual;
	actual.resize(expected.size());
	result.display_handle->seek(0, platform::file::whence::begin);
	actual.resize(static_cast<size_t>(result.display_handle->read(actual.data(), actual.size())));

	assert_equal(true, expected == actual, "handle reads back the written bytes");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// File handle lifetime
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_keep_file_handles_detached_until_last_operation()
{
	null_state_strategy state_strategy;
	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);
	view_state state(state_strategy, async, index, make_test_player());
	const auto initial_detach_count = df::file_handles_detached.load();

	auto first = std::make_shared<detach_file_handles>(state);
	auto second = std::make_shared<detach_file_handles>(state);
	assert_equal(initial_detach_count + 2, df::file_handles_detached.load(), "both operations detached");

	first.reset();
	assert_equal(initial_detach_count + 1, df::file_handles_detached.load(), "later operation remains detached");
	assert_equal(0ull, static_cast<uint64_t>(async.pending_worker_count(async_queue::scan_modified_items)),
	             "no intermediate rescan");

	second.reset();
	assert_equal(initial_detach_count, df::file_handles_detached.load(), "final operation restores handles");
	assert_equal(1ull, static_cast<uint64_t>(async.pending_worker_count(async_queue::scan_modified_items)),
	             "one final rescan");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Decoder robustness
///////////////////////////////////////////////////////////////////////////////////////////////////

static bool write_binary_file(const df::file_path path, const uint8_t* const data, const int size)
{
	auto f = open_file(path, platform::file_open_mode::create);

	if (!f)
	{
		return false;
	}

	const auto written = f->write(data, size);
	f.reset();

	return written == static_cast<uint64_t>(size) && path.exists() && platform::file_attributes(path).size ==
		static_cast<uint64_t>(size);
}

static file_scan_result scan_recorded_jpeg(files& ff, const df::blob& prefix, const uint64_t reported_size,
                                           const bool load_thumbnail, std::shared_ptr<recording_file>& recorded)
{
	const auto path = _temps.next_path(".jpg");
	recorded = std::make_shared<recording_file>(path, prefix.clone(), reported_size);
	return ff.scan_file(recorded, path, load_thumbnail, files::file_type_from_name(path), {},
	                    {64, 64});
}

static df::blob blob_slice(const df::blob& bytes, const size_t offset, const size_t len)
{
	return {bytes.begin() + static_cast<ptrdiff_t>(offset), bytes.begin() + static_cast<ptrdiff_t>(offset + len)};
}

static void should_apply_encoded_ceiling_to_bitmap_thumbnail_scans()
{
	const auto source = df::blob_from_file(test_files_folder.combine_file("Test.jpg"));
	assert_equal(true, source.size() > sizeof(pack128), "jpeg fixture loaded");

	files ff;

	std::shared_ptr<recording_file> below;
	const auto below_scan = scan_recorded_jpeg(ff, source, source.size(), true, below);
	assert_equal(true, below_scan.success, "below-ceiling thumbnail scan reads metadata");
	assert_equal(true, is_valid(below_scan.thumbnail_image), "below-ceiling thumbnail is available");
	assert_equal(static_cast<uint64_t>(source.size()), below->max_read, "below-ceiling read is the whole file");

	std::shared_ptr<recording_file> at_ceiling;
	const auto at_scan = scan_recorded_jpeg(ff, source, df::max_blob_size, true, at_ceiling);
	assert_equal(true, at_scan.success, "at-ceiling thumbnail scan reads metadata");
	assert_equal(static_cast<uint64_t>(df::max_blob_size), at_ceiling->max_read,
	             "at-ceiling thumbnail read is still admitted");

	std::shared_ptr<recording_file> above_thumbnail;
	const auto above_scan = scan_recorded_jpeg(ff, source, static_cast<uint64_t>(df::max_blob_size) + 1u, true,
	                                           above_thumbnail);
	assert_equal(true, above_scan.success, "above-ceiling thumbnail scan keeps header metadata");
	assert_equal(false, is_valid(above_scan.thumbnail_image), "above-ceiling scan does not publish a partial thumbnail");
	assert_equal(true, above_thumbnail->max_read < static_cast<uint64_t>(df::max_blob_size),
	             "above-ceiling scan never reads or allocates the whole file");

	std::shared_ptr<recording_file> above_metadata;
	const auto metadata_scan = scan_recorded_jpeg(ff, source, static_cast<uint64_t>(df::max_blob_size) + 1u, false,
	                                              above_metadata);
	assert_equal(true, metadata_scan.success, "above-ceiling metadata-only scan still succeeds");
	assert_equal(false, is_valid(metadata_scan.thumbnail_image), "metadata-only scan has no thumbnail");
	assert_equal(true, above_metadata->max_read < static_cast<uint64_t>(df::max_blob_size),
	             "metadata-only scan remains bounded");
}

static void should_reset_sliding_reader_when_reopened()
{
	const auto a = _temps.next_path(".bin");
	const auto b = _temps.next_path(".bin");
	const df::blob bytes_a = {0, 1, 2, 3, 4, 5, 6, 7};
	const df::blob bytes_b = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
	df::blob_save_to_file(bytes_a, a);
	df::blob_save_to_file(bytes_b, b);

	file_read_stream stream;
	assert_equal(true, stream.open(a), "open first file");
	assert_equal(true, blob_slice(bytes_a, 2, 4) == stream.read(2, 4), "first file read populates the window");

	assert_equal(true, stream.open(b), "reopen without close");
	assert_equal(true, blob_slice(bytes_b, 2, 4) == stream.read(2, 4), "reopen discards cached bytes");
	assert_equal(true, blob_slice(bytes_b, 6, 4) == stream.read(6, 4), "boundary-crossing read comes from reopened file");

	stream.close();
	assert_equal(true, stream.open(a), "reopen after close");
	assert_equal(true, blob_slice(bytes_a, 1, 5) == stream.read(1, 5), "close clears cached range and storage");

	assert_equal(false, stream.open(platform::file_ptr{}), "failed open is reported");
	assert_equal(true, stream.open(b), "open after failed open");
	assert_equal(true, blob_slice(bytes_b, 0, 3) == stream.read(0, 3), "failed open left no stale cached state");
}

static void should_not_crash(const std::string_view name)
{
	const auto path = test_files_folder.combine_file(name);
	auto blob = blob_from_file(path);
	const auto ext = name.substr(df::find_ext(name));

	// Without this the whole test degrades silently: a missing or empty fixture makes both
	// loops below no-ops, and "did not crash" would be reported for work that never ran.
	assert_equal(true, blob.size() > 16u, std::format("{} fixture is readable", name));

	const auto original = std::vector<uint8_t>(blob.begin(), blob.end());

	files ff;

	auto* const data = blob.data();
	const auto size = blob.size();

	auto corrupted = 0u;

	for (auto i = 0u; i < size && !df::is_closing; i++)
	{
		const auto v = data[i];
		data[i] = 0xff;

		prop::item_metadata md;
		mem_read_stream stream(blob);
		const auto info = scan_photo(stream);

		metadata_exif::parse(md, info.metadata.exif);
		metadata_iptc::parse(md, info.metadata.iptc);
		metadata_xmp::parse(md, info.metadata.xmp);

		data[i] = v;
		++corrupted;
	}

	constexpr auto truncation_steps = 32;
	auto truncated = 0u;

	for (auto i = 0u; i < truncation_steps && !df::is_closing; i++)
	{
		const auto save_path = _temps.next_path(ext);
		const auto byte_count = df::mul_div(static_cast<int>(size), i, truncation_steps);
		assert_equal(true, write_binary_file(save_path, data, byte_count),
		             std::format("{} truncation fixture {} was written", name, i));
		ff_scan_and_load_thumb(ff, save_path);
		++truncated;
	}

	if (df::is_closing)
	{
		assert_equal(true, corrupted > 0u, std::format("{} sweep started before shutdown", name));
		return;
	}

	assert_equal(static_cast<int>(size), static_cast<int>(corrupted),
	             std::format("{} every byte was corrupted and scanned", name));
	assert_equal(truncation_steps, static_cast<int>(truncated),
	             std::format("{} every truncation was scanned", name));
	assert_equal(true, std::equal(original.begin(), original.end(), blob.begin(), blob.end()),
	             std::format("{} bytes restored after the sweep", name));
}

// Every still libfiles reads, scanned two ways that must agree: from memory, where libfiles borrows the
// bytes, and through a stream, where it reads by block-aligned window. The fixtures and any folders named
// in DIFFRACTOR_SCAN_PARITY_CORPUS (separated by ';') are scanned whole - also from disk - then cut
// short at fixed points and with single bytes corrupted, for indexing and for inspection, thumbnails
// included. Before libfiles took over the scanning, the same sweep held it to Diffractor's own
// scanners: 19,640 comparisons over 138 files, with no difference but the WebP one libfiles' README records.
// HEIF/AVIF and JPEG XL were held the same way to libheif and libjxl before libfiles took them over:
// 40,116 comparisons over 258 files, with no difference.
namespace scan_parity
{
	std::string describe(const file_scan_result& r)
	{
		auto s = std::format("format {} success {} size {}x{} pixels '{}' orientation {}\n",
		                     static_cast<int>(r.format), r.success, r.width, r.height, r.pixel_format.sv(),
		                     static_cast<int>(r.orientation));

		const auto block = [&s](const std::string_view name, const df::blob& b)
		{
			s += std::format("{} {} bytes {:08x}\n", name, b.size(), df::crc32c(b.data(), b.size()));
		};

		block("xmp", r.metadata.xmp);
		block("exif", r.metadata.exif);
		block("iptc", r.metadata.iptc);
		block("icc", r.metadata.icc);
		block("photoshop", r.metadata.photoshop_resources);
		s += std::format("gps {} {:.9f} {:.9f}\n", r.gps.is_valid(), r.gps.latitude(), r.gps.longitude());

		if (r.thumbnail_image)
		{
			const auto& data = r.thumbnail_image->data();
			const auto d = r.thumbnail_image->dimensions();
			s += std::format("thumbnail image {} bytes {:08x} {}x{} orientation {}\n", data.size(),
			                 df::crc32c(data.data(), data.size()), d.cx, d.cy,
			                 static_cast<int>(r.thumbnail_image->orientation()));
		}

		if (r.thumbnail_surface)
		{
			const auto d = r.thumbnail_surface->dimensions();
			s += std::format("thumbnail surface {}x{} orientation {}\n", d.cx, d.cy,
			                 static_cast<int>(r.thumbnail_surface->orientation()));
		}

		for (const auto& row : r.structure_metadata)
		{
			s += std::format("row '{}' '{}' '{}' '{}' {} {} {} detail {}", row.key, row.value, row.shape, row.id,
			                 row.depth, row.container, row.open_by_default, row.detail.index());

			if (const auto* const text = std::get_if<metadata_text_detail>(&row.detail))
			{
				s += " " + text->text;
			}
			else if (const auto* const bytes = std::get_if<metadata_binary_detail>(&row.detail))
			{
				s += std::format(" {} {:08x} {}", bytes->bytes.size(),
				                 df::crc32c(bytes->bytes.data(), bytes->bytes.size()), bytes->is_image);
			}
			else if (const auto* const table = std::get_if<metadata_numeric_detail>(&row.detail))
			{
				s += std::format(" {} columns", table->columns);
				for (const auto v : table->values) s += std::format(" {}", v);
			}

			s += "\n";
		}

		return s;
	}

	std::string first_difference(const std::string& expected, const std::string& actual)
	{
		const auto expected_lines = str::split(expected, false, [](const char c) { return c == '\n'; });
		const auto actual_lines = str::split(actual, false, [](const char c) { return c == '\n'; });

		for (size_t i = 0; i < std::max(expected_lines.size(), actual_lines.size()); ++i)
		{
			const auto e = i < expected_lines.size() ? expected_lines[i] : std::string_view("(nothing)");
			const auto a = i < actual_lines.size() ? actual_lines[i] : std::string_view("(nothing)");
			if (e != a) return std::format("diffractor: {} | libfiles: {}", e, a);
		}

		return "same";
	}

	bool is_libfiles_format(const detected_format format)
	{
		switch (format)
		{
		case detected_format::JPEG:
		case detected_format::PNG:
		case detected_format::WEBP:
		case detected_format::GIF:
		case detected_format::PSD:
		case detected_format::BMP:
		case detected_format::TIFF:
		case detected_format::HEIF:
		case detected_format::JXL:
			return true;
		default:
			return false;
		}
	}

	void add_folder(const df::folder_path folder, std::vector<df::file_path>& files)
	{
		const auto contents = platform::iterate_file_items(folder, false);
		for (const auto& f : contents.files) files.push_back(f.folder.combine_file(f.name));
		for (const auto& sub : contents.folders) add_folder(folder.combine(sub.name), files);
	}

	std::string environment_variable(const char* const name)
	{
#ifdef _MSC_VER
		char* value = nullptr;
		size_t size = 0;
		if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
		std::string result(value);
		free(value);
		return result;
#else
		const auto* const value = std::getenv(name);
		return value ? value : "";
#endif
	}

	std::vector<size_t> cut_points(const size_t size)
	{
		std::vector<size_t> cuts = {
			16, 20, 24, 32, 48, 64, 100, 128, 200, 256, 300, 512, 700, 1000, 1024, 1500, 2048, 3000, 4096, 6000,
			8192, 12000, 16384, 32768, 65536, 100000, 131072, 262144, size / 2, size - 13, size - 12, size - 4,
			size - 1
		};

		std::erase_if(cuts, [size](const size_t cut) { return cut < 16 || cut >= size; });
		std::ranges::sort(cuts);
		cuts.erase(std::ranges::unique(cuts).begin(), cuts.end());
		return cuts;
	}

	// Dense over the first header bytes, spread over the first 64 KB where the metadata lives, and a
	// few anywhere. In 64 bits throughout, so that a 32-bit build corrupts the same bytes and the
	// sweeps' records hold for it too.
	size_t corruption_position(const uint64_t k, const size_t size)
	{
		const auto head = std::min<uint64_t>(size, 65536);
		if (k < 16) return 16 + static_cast<size_t>(k * 7 % (head - 16));
		if (k < 40) return 16 + static_cast<size_t>(k * 2654435761ull % (head - 16));
		return 16 + static_cast<size_t>(k * 2654435761ull % (size - 16));
	}
}

// A memory stream that does not lend its memory, so libfiles reads it through its window.
class streamed_memory final : public read_stream
{
	df::cspan _data;

public:
	explicit streamed_memory(const df::cspan data) : _data(data)
	{
		_file_size = data.size;
	}

	template <typename T>
	T peek(const uint64_t pos)
	{
		check_range(pos, sizeof(T));
		T result;
		memcpy(&result, _data.data + pos, sizeof(T));
		return result;
	}

	uint8_t peek8(const uint64_t pos) override { return peek<uint8_t>(pos); }
	uint16_t peek16(const uint64_t pos) override { return peek<uint16_t>(pos); }
	uint32_t peek32(const uint64_t pos) override { return peek<uint32_t>(pos); }
	uint64_t peek64(const uint64_t pos) override { return peek<uint64_t>(pos); }
	pack128 peek128(const uint64_t pos) override { return peek<pack128>(pos); }

	void read(const uint64_t pos, uint8_t* buffer, const size_t len) override
	{
		check_range(pos, len);
		memcpy(buffer, _data.data + pos, len);
	}

	df::blob read(const uint64_t pos, const size_t len) override
	{
		check_range(pos, len);
		return {_data.data + pos, _data.data + pos + len};
	}

	df::blob read_all() override
	{
		return {_data.begin(), _data.end()};
	}
};

static void should_scan_stills_alike_from_memory_and_streams()
{
	std::vector<df::file_path> corpus;
	scan_parity::add_folder(test_files_folder, corpus);
	scan_parity::add_folder(test_formats_folder, corpus);

	// Held here: split returns views into it, and a temporary would be gone before the loop runs.
	const auto extra = scan_parity::environment_variable("DIFFRACTOR_SCAN_PARITY_CORPUS");

	for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
	{
		scan_parity::add_folder(df::folder_path(part), corpus);
	}

	auto files_scanned = 0;
	auto comparisons = 0;
	std::vector<std::string> differences;

	const auto compare = [&](const std::string& label, read_stream& resident, read_stream& streamed,
	                         const scan_intent intent, const bool thumbnail)
	{
		const auto expected = scan_parity::describe(scan_photo(resident, intent, thumbnail));
		const auto actual = scan_parity::describe(scan_photo(streamed, intent, thumbnail));
		++comparisons;

		if (expected != actual)
		{
			differences.push_back(std::format("{} ({}{}): {}", label,
			                                  intent == scan_intent::inspect ? "inspect" : "index",
			                                  thumbnail ? ", thumbnail" : "",
			                                  scan_parity::first_difference(expected, actual)));
		}
	};

	const auto compare_bytes = [&](const std::string& label, const df::cspan data, const bool thumbnails)
	{
		if (data.size < sizeof(pack128) || !scan_parity::is_libfiles_format(files::detect_format(data))) return;

		for (const auto intent : {scan_intent::index, scan_intent::inspect})
		{
			for (const auto thumbnail : {false, true})
			{
				if (thumbnail && !thumbnails) continue;
				mem_read_stream resident(data);
				streamed_memory streamed(data);
				compare(label, resident, streamed, intent, thumbnail);
			}
		}
	};

	for (const auto& path : corpus)
	{
		auto bytes = df::blob_from_file(path);
		if (bytes.size() < sizeof(pack128) || !scan_parity::is_libfiles_format(files::detect_format(bytes))) continue;

		++files_scanned;
		const auto name = path.str();

		compare_bytes(name, bytes, true);

		for (const auto intent : {scan_intent::index, scan_intent::inspect})
		{
			mem_read_stream resident(bytes);
			file_read_stream from_disk;

			if (from_disk.open(path))
			{
				compare(name + " from disk", resident, from_disk, intent, true);
			}
		}

		for (const auto cut : scan_parity::cut_points(bytes.size()))
		{
			compare_bytes(std::format("{} cut at {}", name, cut), {bytes.data(), cut}, false);
		}

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));

			if (bytes[at] != original)
			{
				compare_bytes(std::format("{} byte {} set to {:02x}", name, at, bytes[at]), bytes, false);
			}

			bytes[at] = original;
		}
	}

	df::log(__FUNCTION__, std::format("{} files, {} comparisons, {} differences", files_scanned, comparisons,
	                                  differences.size()));

	for (size_t i = 0; i < std::min<size_t>(differences.size(), 2000); ++i) df::log(__FUNCTION__, differences[i]);

	std::string report;
	std::map<std::string, int> per_file;

	for (const auto& difference : differences)
	{
		const auto file = difference.substr(0, difference.find(' '));
		if (++per_file[file] <= 2 && per_file.size() <= 30) report += "\n  " + difference;
	}

	for (const auto& [file, count] : per_file) report += std::format("\n  {} differences in {}", count, file);

	assert_equal(true, files_scanned > 30, "the fixtures hold stills to compare");
	assert_equal(0, static_cast<int>(differences.size()),
	             std::format("a scan through a stream reads as one from memory{}", report));
}

// Diffractor's PSD loader moved to libfiles. Before the switch both decoded every PSD in the fixtures
// and in DIFFRACTOR_SCAN_PARITY_CORPUS, and a synthetic one in every colour mode, raw and PackBits -
// whole, cut short and with single bytes corrupted - and agreed on every byte of every row, on what
// they told the decode budget and on which files they threw for: 2,072 comparisons over 31 files, the
// XMP SDK's BlueSquare.psd among them, with no difference. That comparison left a record of the old
// loader's outcomes for the fixtures and the synthetic files but Lab's, whose conversion takes pow,
// which may round differently elsewhere; it was the same on Windows and Linux. The sweep now holds
// libfiles to it.
namespace psd_sweep
{
	struct outcome
	{
		bool threw = false;
		sizei dimensions;
		std::vector<uint8_t> rows;
		sizei source_dimensions;
		bool over_budget = false;

		bool operator==(const outcome&) const = default;
	};

	template <typename Load>
	outcome run(Load&& load, const df::cspan bytes)
	{
		outcome o;
		load_diagnostic diagnostic;

		try
		{
			mem_read_stream stream(bytes);
			const auto surface = load(stream, &diagnostic);

			if (surface && !surface->empty())
			{
				o.dimensions = surface->dimensions();
				const auto row_bytes = static_cast<size_t>(o.dimensions.cx) * 4;
				o.rows.reserve(row_bytes * o.dimensions.cy);

				for (auto y = 0; y < o.dimensions.cy; ++y)
				{
					const auto* const row = surface->pixels_line(y);
					o.rows.insert(o.rows.end(), row, row + row_bytes);
				}
			}
		}
		catch (const std::exception&)
		{
			o.threw = true;
		}

		o.source_dimensions = diagnostic.source_dimensions;
		o.over_budget = diagnostic.over_budget;
		return o;
	}

	std::string describe(const outcome& o)
	{
		return std::format("threw {} {}x{} {} bytes {:08x} source {}x{} over budget {}", o.threw, o.dimensions.cx,
		                   o.dimensions.cy, o.rows.size(), df::crc32c(o.rows.data(), o.rows.size()),
		                   o.source_dimensions.cx, o.source_dimensions.cy, o.over_budget);
	}

	// A row as PackBits: runs of three or more alike repeated, the rest literal.
	void packbits(const uint8_t* const row, const size_t n, df::blob& out)
	{
		size_t x = 0;

		while (x < n)
		{
			auto same = 1_z;
			while (x + same < n && same < 128 && row[x + same] == row[x]) ++same;

			if (same >= 3)
			{
				out.push_back(static_cast<uint8_t>(1 - static_cast<int>(same)));
				out.push_back(row[x]);
				x += same;
				continue;
			}

			auto literal = 0_z;
			while (x + literal < n && literal < 128)
			{
				if (x + literal + 2 < n && row[x + literal] == row[x + literal + 1] &&
					row[x + literal] == row[x + literal + 2])
					break;
				++literal;
			}

			out.push_back(static_cast<uint8_t>(literal - 1));
			out.insert(out.end(), row + x, row + x + literal);
			x += literal;
		}
	}

	df::blob make_psd(const uint16_t mode, const uint16_t depth, const uint16_t channels, const uint32_t width,
	                  const uint32_t height, const bool rle, const uint32_t table_length, uint32_t& seed)
	{
		df::blob b;
		const auto u16 = [&b](const uint32_t v) { b.push_back(static_cast<uint8_t>(v >> 8)); b.push_back(static_cast<uint8_t>(v)); };
		const auto u32 = [&u16](const uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); };
		const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<uint8_t>(seed >> 24); };

		u32(0x38425053);
		u16(1);
		b.insert(b.end(), 6, 0);
		u16(channels);
		u32(height);
		u32(width);
		u16(depth);
		u16(mode);
		u32(table_length);
		for (uint32_t i = 0; i < table_length; ++i) b.push_back(next());
		u32(0);
		u32(0);
		u16(rle ? 1 : 0);

		const auto row_bytes = depth == 1 ? (width + 7) / 8 : width;
		std::vector<df::blob> runs;

		for (uint32_t c = 0; c < channels; ++c)
		{
			for (uint32_t y = 0; y < height; ++y)
			{
				// Flat stretches among the noise, so PackBits repeats as well as copies.
				std::vector<uint8_t> row(row_bytes);
				for (auto& v : row) v = (next() & 3) == 0 ? 0x55 : next();

				if (rle)
				{
					df::blob run;
					packbits(row.data(), row.size(), run);
					runs.push_back(std::move(run));
				}
				else
				{
					b.insert(b.end(), row.begin(), row.end());
				}
			}
		}

		for (const auto& run : runs) u16(static_cast<uint32_t>(run.size()));
		for (const auto& run : runs) b.insert(b.end(), run.begin(), run.end());
		return b;
	}
}

static void should_decode_psds_as_load_psd_did()
{
	// Each file, and whether its outcomes count toward the record kept for after the switch: the
	// fixtures' and the synthetic ones' but Lab's, whose conversion takes pow, which may round
	// differently on another platform.
	struct entry
	{
		std::string name;
		df::blob bytes;
		bool recorded = false;
	};

	std::vector<entry> corpus;
	std::vector<df::file_path> files;
	scan_parity::add_folder(test_files_folder, files);
	const auto fixtures = files.size();

	const auto extra = scan_parity::environment_variable("DIFFRACTOR_SCAN_PARITY_CORPUS");
	for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
	{
		scan_parity::add_folder(df::folder_path(part), files);
	}

	for (size_t i = 0; i < files.size(); ++i)
	{
		auto bytes = df::blob_from_file(files[i]);
		if (bytes.size() >= 26 && files::detect_format(bytes) == detected_format::PSD)
			corpus.push_back({std::string(files[i].name()), std::move(bytes), i < fixtures});
	}

	const auto real_files = corpus.size();

	struct synthetic
	{
		uint16_t mode, depth, channels;
		uint32_t table_length;
	};

	uint32_t seed = 12345;

	for (const auto& s : {synthetic{0, 1, 1, 0}, synthetic{1, 8, 1, 0}, synthetic{2, 8, 1, 768}, synthetic{2, 8, 1, 300},
	                      synthetic{3, 8, 3, 0}, synthetic{3, 8, 4, 0}, synthetic{3, 8, 5, 0}, synthetic{4, 8, 4, 0},
	                      synthetic{4, 8, 5, 0}, synthetic{7, 8, 2, 0}, synthetic{8, 8, 1, 40}, synthetic{9, 8, 3, 0},
	                      synthetic{9, 8, 4, 0}, synthetic{5, 8, 3, 0}})
	{
		for (const auto rle : {false, true})
		{
			corpus.push_back({std::format("synthetic mode {} depth {} channels {} table {} {}", s.mode, s.depth,
			                              s.channels, s.table_length, rle ? "packbits" : "raw"),
			                  psd_sweep::make_psd(s.mode, s.depth, s.channels, 37, 23, rle, s.table_length, seed),
			                  s.mode != 9});
		}
	}

	// Over the decode budget: both refuse it from the header alone. Only its first 64 bytes are kept,
	// so it is made one short row tall - the pixels come out in order, so the bytes kept are the ones
	// the whole image begins with - and its 30000 x 30000 written over the header's size, which a
	// 32-bit build could not have allocated.
	auto over_budget = psd_sweep::make_psd(3, 8, 3, 64, 1, false, 0, seed);
	for (const size_t field : {14u, 18u})
	{
		over_budget[field + 0] = 0;
		over_budget[field + 1] = 0;
		over_budget[field + 2] = 30000 >> 8;
		over_budget[field + 3] = 30000 & 0xFF;
	}
	over_budget.resize(64);
	corpus.push_back({"synthetic over budget", std::move(over_budget), true});

	auto decodes = 0;
	auto decoded = 0;
	auto recorded_outcomes = 0;
	std::string record;

	const auto decode = [&](const df::cspan bytes, const bool recorded)
	{
		++decodes;
		const auto outcome = psd_sweep::run([](read_stream& s, load_diagnostic* d) { return load_psd(s, d); }, bytes);
		if (!outcome.rows.empty()) ++decoded;

		if (recorded)
		{
			record += psd_sweep::describe(outcome) + "\n";
			++recorded_outcomes;
		}
	};

	for (auto& [name, bytes, recorded] : corpus)
	{
		decode(bytes, recorded);

		for (const auto cut : scan_parity::cut_points(bytes.size())) decode({bytes.data(), cut}, recorded);

		if (bytes.size() <= 16) continue;

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));
			if (bytes[at] != original) decode(bytes, recorded);
			bytes[at] = original;
		}
	}

	const auto checksum = df::crc32c(record.data(), record.size());
	df::log(__FUNCTION__, std::format("{} files ({} real), {} decodes, {} decoded; record {:08x} over {} outcomes",
	                                  corpus.size(), real_files, decodes, decoded, checksum, recorded_outcomes));

	assert_equal(true, real_files >= 1, "the fixtures hold a PSD");
	assert_equal(true, decoded > 500, "most of what is decoded decodes");
	assert_equal(1729, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0x8f0d9d10u), std::format("{:08x}", checksum),
	             "every outcome is the one load_psd had before it moved to libfiles");
}

// Diffractor's JPEG decoder moved to libfiles. Before the switch both decoded every JPEG in the
// fixtures and in DIFFRACTOR_SCAN_PARITY_CORPUS - whole, cut short and with single bytes corrupted; at
// full size and reduced; to BGRX and to NV12; for display and for a thumbnail - and agreed on every
// pixel byte, the format, the size, the colour space and the orientation: 11,754 comparisons over 62
// files, C:\Windows\Web and the XMP SDK's samples among them, with no difference. That comparison left
// a record of the old decoder's outcomes for the fixtures, the same on Windows and Linux. The sweep now
// holds libfiles to it.
//
// The record depends on the word size. libjpeg-turbo's entropy decoder fills its bit buffer a size_t at
// a time, so how much of a cut file's last data it decodes before the end of image the source supplies
// differs between a 32-bit build and a 64-bit one: 33 outcomes, all of files cut short, and the same
// with its SIMD turned off. The comparison ran on x86 too, with no difference between the decoders
// there either, and took the 32-bit record.
namespace jpeg_sweep
{
	struct outcome
	{
		bool decoded = false;
		bool yuv = false;
		sizei dimensions;
		int format = 0;
		int orientation = 0;
		int color_space = 0;
		std::vector<uint8_t> pixels;
	};

	outcome run(files& ff, const df::cspan data, const sizei target, const bool yuv, const decode_intent intent)
	{
		outcome o;
		const auto surface = files_test_hooks::decode_jpeg(ff, data, target, yuv, intent, o.yuv);
		o.decoded = surface && !surface->empty();

		if (o.decoded)
		{
			o.dimensions = surface->dimensions();
			o.format = static_cast<int>(surface->format());
			o.orientation = static_cast<int>(surface->orientation());
			o.color_space = static_cast<int>(surface->color_space());

			// The pixels only: a row's padding is the surface's, and the decoder does not write it.
			const auto nv12 = surface->format() == ui::texture_format::NV12;
			const auto row_bytes = static_cast<size_t>(o.dimensions.cx) * (nv12 ? 1 : 4);
			const auto rows = static_cast<size_t>(o.dimensions.cy) * (nv12 ? 3 : 2) / 2;
			o.pixels.reserve(row_bytes * rows);

			for (size_t y = 0; y < rows; ++y)
			{
				const auto* const row = surface->pixels() + y * surface->stride();
				o.pixels.insert(o.pixels.end(), row, row + row_bytes);
			}
		}

		return o;
	}

	std::string describe(const outcome& o)
	{
		return std::format("decoded {} yuv {} {}x{} format {} orientation {} colour {} {} bytes {:08x}", o.decoded,
		                   o.yuv, o.dimensions.cx, o.dimensions.cy, o.format, o.orientation, o.color_space,
		                   o.pixels.size(), df::crc32c(o.pixels.data(), o.pixels.size()));
	}

	struct request
	{
		sizei target;
		bool yuv = false;
		decode_intent intent = decode_intent::display;
	};
}

static void should_decode_jpegs_as_jpeg_decoder_x_did()
{
	// Each file, and whether its outcomes count toward the record: the fixtures' do, and a corpus
	// folder's are decoded for what they exercise but not recorded.
	struct entry
	{
		std::string name;
		df::blob bytes;
		bool recorded = false;
	};

	std::vector<entry> corpus;
	std::vector<df::file_path> files_found;
	scan_parity::add_folder(test_files_folder, files_found);
	scan_parity::add_folder(test_formats_folder, files_found);
	const auto fixtures = files_found.size();

	const auto extra = scan_parity::environment_variable("DIFFRACTOR_SCAN_PARITY_CORPUS");
	for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
	{
		scan_parity::add_folder(df::folder_path(part), files_found);
	}

	for (size_t i = 0; i < files_found.size(); ++i)
	{
		auto bytes = df::blob_from_file(files_found[i]);
		if (bytes.size() >= 64 && files::detect_format(bytes) == detected_format::JPEG)
			corpus.push_back({std::string(files_found[i].name()), std::move(bytes), i < fixtures});
	}

	using jpeg_sweep::request;
	const std::vector<request> whole_requests = {
		{{0, 0}, false, decode_intent::display}, {{0, 0}, true, decode_intent::display},
		{{256, 256}, false, decode_intent::thumbnail}, {{256, 256}, true, decode_intent::thumbnail},
		{{700, 500}, false, decode_intent::display}, {{320, 240}, true, decode_intent::display},
	};
	const std::vector<request> part_requests = {
		{{256, 256}, false, decode_intent::thumbnail}, {{256, 256}, true, decode_intent::thumbnail},
	};

	files ff;
	auto decodes = 0;
	auto decoded = 0;
	auto recorded_files = 0;
	auto recorded_outcomes = 0;
	std::string record;

	const auto decode = [&](const df::cspan bytes, const bool recorded, const std::vector<request>& requests)
	{
		for (const auto& r : requests)
		{
			++decodes;
			const auto outcome = jpeg_sweep::run(ff, bytes, r.target, r.yuv, r.intent);
			if (outcome.decoded) ++decoded;

			if (recorded)
			{
				record += jpeg_sweep::describe(outcome) + "\n";
				++recorded_outcomes;
			}
		}
	};

	for (auto& [name, bytes, recorded] : corpus)
	{
		if (recorded) ++recorded_files;
		decode(bytes, recorded, whole_requests);

		// A small file is cheap to decode in full, so its parts are too.
		auto requests = part_requests;
		if (bytes.size() < 300 * 1024) requests.push_back({{0, 0}, false, decode_intent::display});

		for (const auto cut : scan_parity::cut_points(bytes.size())) decode({bytes.data(), cut}, recorded, requests);

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));
			if (bytes[at] != original) decode(bytes, recorded, requests);
			bytes[at] = original;
		}
	}

	const auto checksum = df::crc32c(record.data(), record.size());
	df::log(__FUNCTION__, std::format("{} files ({} recorded), {} decodes, {} decoded; record {:08x} over {} outcomes",
	                                  corpus.size(), recorded_files, decodes, decoded, checksum, recorded_outcomes));

	assert_equal(true, recorded_files >= 10, "the fixtures hold JPEGs");
	constexpr auto expected = sizeof(size_t) == 8 ? 0xc15126d4u : 0x7344b182u;
	assert_equal(4642, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", expected), std::format("{:08x}", checksum),
	             "every outcome is the one jpeg_decoder_x had before it moved to libfiles");
}

// Diffractor's EXIF parser moved to libfiles. Before the switch both read every EXIF block the fixtures
// and DIFFRACTOR_SCAN_PARITY_CORPUS carry - whole, cut short, with each of its first 512 bytes set to
// every value from 0 to 13, to 0xFF, to its complement and to either neighbour, and 48 more spread
// through it - and agreed on everything an item takes from EXIF, floats to the bit: 248,814 readings of
// 35 blocks, with no difference on Windows x64 and x86 or on Linux. That comparison left a record of the
// old parser's readings of the fixtures, taken with each date as the text libfiles reads, not the date
// libdesktop makes of it: what a C runtime makes of damaged date text, and where a GPS stamp falls in
// local time, differ between machines. The sweep now holds libfiles to it - with one change since:
// Windows writes its XP tags as UTF-16LE bytes, which Diffractor's parser read as "?", and libfiles
// reads as Windows wrote them. That moved the readings of IMG_0096.JPG alone, the one fixture that
// carries them (XPTitle "Dog", XPKeywords "dog;London"), and the record from 6bed22cc to eb1315fe.
namespace exif_sweep
{
	std::string bits(const float value)
	{
		return std::format("{:08x}", std::bit_cast<uint32_t>(value));
	}

	std::string bits(const double value)
	{
		return std::format("{:016x}", std::bit_cast<uint64_t>(value));
	}

	std::string describe(const prop::item_metadata& md, const libfiles::exif_metadata& exif)
	{
		auto s = std::format("orientation {} make '{}' model '{}' lens '{}' encoder '{}' artist '{}' copyright '{}' "
		                     "description '{}' comment '{}' title '{}' tags '{}' f {} exposure {} focal {} focal35 {} "
		                     "iso {} rating {} altitude {} speed {} coordinate {} {} {}",
		                     static_cast<int>(md.orientation), md.camera_manufacturer.sv(), md.camera_model.sv(),
		                     md.lens.sv(), md.encoder.sv(), md.artist.sv(), md.copyright_notice.sv(),
		                     md.description.sv(), md.comment.sv(), md.title.sv(), md.tags.sv(), bits(md.f_number),
		                     bits(md.exposure_time), bits(md.focal_length), md.focal_length_35mm_equivalent,
		                     md.iso_speed, md.rating, bits(md.altitude), bits(md.gps_speed),
		                     md.coordinate.is_valid(), bits(md.coordinate.latitude()),
		                     bits(md.coordinate.longitude()));

		const auto date = [&s](const std::string_view name, const libfiles::exif_date& d)
		{
			s += std::format(" {} '{}' '{}' '{}'", name, d.text, d.offset, d.sub_second);
		};

		date("modified", exif.modified);
		date("original", exif.original);
		date("digitized", exif.digitized);
		s += std::format(" gps '{}'", exif.gps_date);

		if (exif.gps_time)
		{
			s += std::format(" {} {} {}", bits((*exif.gps_time)[0]), bits((*exif.gps_time)[1]),
			                 bits((*exif.gps_time)[2]));
		}

		return s;
	}
}

static void should_read_exif_as_diffractors_parser_did()
{
	struct block
	{
		std::string name;
		df::blob bytes;
		bool recorded = false;
	};

	std::vector<block> blocks;
	std::vector<df::file_path> found;
	scan_parity::add_folder(test_files_folder, found);
	scan_parity::add_folder(test_formats_folder, found);
	const auto fixtures = found.size();

	const auto extra = scan_parity::environment_variable("DIFFRACTOR_SCAN_PARITY_CORPUS");
	for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
	{
		scan_parity::add_folder(df::folder_path(part), found);
	}

	files ff;

	for (size_t i = 0; i < found.size(); ++i)
	{
		auto info = ff_scan_file(ff, found[i]);
		if (info.metadata.exif.size() > 16)
			blocks.push_back({std::string(found[i].name()), std::move(info.metadata.exif), i < fixtures});
	}

	auto readings = 0;
	auto recorded_blocks = 0;
	auto recorded_outcomes = 0;
	std::string record;

	const auto read = [&](const df::cspan bytes, const bool recorded)
	{
		++readings;
		prop::item_metadata md;
		metadata_exif::parse(md, bytes);

		if (recorded)
		{
			record += exif_sweep::describe(md, libfiles::read_exif({bytes.data, bytes.size})) + "\n";
			++recorded_outcomes;
		}
	};

	for (auto& b : blocks)
	{
		if (b.recorded) ++recorded_blocks;
		read(b.bytes, b.recorded);

		for (const auto cut : scan_parity::cut_points(b.bytes.size())) read({b.bytes.data(), cut}, b.recorded);

		const auto corrupt = [&](const size_t at, const uint8_t value)
		{
			const auto original = b.bytes[at];
			if (value == original) return;
			b.bytes[at] = value;
			read(b.bytes, b.recorded);
			b.bytes[at] = original;
		};

		for (size_t at = 0; at < std::min<size_t>(b.bytes.size(), 512); ++at)
		{
			const auto original = b.bytes[at];
			std::vector<uint8_t> values = {0xFF, static_cast<uint8_t>(original ^ 0xFF),
			                               static_cast<uint8_t>(original + 1), static_cast<uint8_t>(original - 1)};
			for (uint8_t v = 0; v <= 13; ++v) values.push_back(v);
			std::ranges::sort(values);
			values.erase(std::ranges::unique(values).begin(), values.end());

			for (const auto v : values) corrupt(at, v);
		}

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, b.bytes.size());
			corrupt(at, static_cast<uint8_t>(b.bytes[at] ^ 0x5A));
		}
	}

	const auto checksum = df::crc32c(record.data(), record.size());
	df::log(__FUNCTION__, std::format("{} blocks ({} recorded), {} readings; record {:08x} over {} outcomes",
	                                  blocks.size(), recorded_blocks, readings, checksum, recorded_outcomes));

	assert_equal(true, recorded_blocks >= 10, "the fixtures carry EXIF");
	assert_equal(154036, recorded_outcomes, "the sweep is the one the record was taken over");
	// The reading Diffractor's parser made, eb1315fe, but for the ISO speed in 227 of the readings: libfiles
	// 04e30fb reads it, and the 35 mm focal length, as the first value where it lies, at its type's width,
	// where the parser read the entry's field as a SHORT. All 227 are of blocks whose ISO entry the sweep
	// damaged - a SHORT made a LONG, or a count that puts the values beyond the field - and none of an
	// undamaged block.
	assert_equal(std::format("{:08x}", 0x48e303bbu), std::format("{:08x}", checksum),
	             "every reading is the one Diffractor's parser made before it moved to libfiles");
}

// metadata_exif::make_exif writes through libfiles. Before the switch, libexif's writer and libfiles'
// wrote the same bytes for every one of these 6,001 items - every field set and unset, values that
// round, overflow or vanish, text that lies inside its entry or after it, odd and even, UTF-8 and 4,000
// bytes long, and dates - and this is the record libexif's writer left of them.
static void should_write_exif_as_libexif_did()
{
	const float f_numbers[] = {0.0f, 1.4f, 2.8f, 0.95f, 22.0f, 5.6f, 1e-6f, 5e5f, 3.333333f};
	const float exposures[] = {0.0f, 1.0f / 8000.0f, 1.0f / 60.0f, 0.3f, 1.0f, 2.5f, 30.0f, 1e-12f, 0.999f};
	const uint16_t isos[] = {0, 100, 3200, 65535};
	const float focals[] = {0.0f, 4.25f, 50.0f, 600.0f, 18.5f, 1e-7f};
	const uint16_t focal35s[] = {0, 26, 105};
	const std::string texts[] = {"", "A", "LG", "Sony", "Canon", "NIKON CORPORATION",
	                             "\xC3\x9cn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 \xE2\x9C\x93", std::string(301, 'x'), "odd",
	                             "  padded  ", std::string(4000, 'y')};
	const ui::orientation orientations[] = {ui::orientation::none, ui::orientation::top_left,
	                                        ui::orientation::right_top, ui::orientation::left_bottom};
	const std::string_view date_texts[] = {"", "2024:05:06 07:08:09", "1999:12:31 23:59:59", "2038:01:19 03:14:08"};

	uint32_t state = 0x2545F491u;
	const auto pick = [&state](const size_t n)
	{
		state = state * 1664525u + 1013904223u;
		return static_cast<size_t>((state >> 8) % n);
	};

	const auto date = [](const std::string_view text)
	{
		df::date_t d;
		if (!text.empty()) d.parse_exif_date(text);
		return d;
	};

	const auto random_item = [&]()
	{
		auto md = std::make_shared<prop::item_metadata>();
		md->orientation = orientations[pick(std::size(orientations))];
		md->f_number = f_numbers[pick(std::size(f_numbers))];
		md->exposure_time = exposures[pick(std::size(exposures))];
		md->iso_speed = isos[pick(std::size(isos))];
		md->focal_length = focals[pick(std::size(focals))];
		md->focal_length_35mm_equivalent = focal35s[pick(std::size(focal35s))];
		md->camera_manufacturer = str::cache(texts[pick(std::size(texts))]);
		md->camera_model = str::cache(texts[pick(std::size(texts))]);
		md->artist = str::cache(texts[pick(std::size(texts))]);
		md->lens = str::cache(texts[pick(std::size(texts))]);
		md->copyright_notice = str::cache(texts[pick(std::size(texts))]);
		md->description = str::cache(texts[pick(std::size(texts))]);
		md->comment = str::cache(texts[pick(std::size(texts))]);
		if (const auto d = date(date_texts[pick(std::size(date_texts))]); d.is_valid())
			md->dates.add(prop::date_source::exif_original, d);
		if (const auto d = date(date_texts[pick(std::size(date_texts))]); d.is_valid())
			md->dates.add(prop::date_source::exif_datetime, d);
		if (const auto d = date(date_texts[pick(std::size(date_texts))]); d.is_valid())
			md->dates.add(prop::date_source::exif_digitized, d);
		return md;
	};

	auto items = 0;
	std::string record;

	const auto write = [&](const prop::item_metadata_ptr& md)
	{
		++items;
		const auto block = metadata_exif::make_exif(md);
		record.append(std::bit_cast<const char*>(block.data()), block.size());
	};

	write(std::make_shared<prop::item_metadata>());
	for (auto i = 0; i < 6000; ++i) write(random_item());

	const auto checksum = df::crc32c(record.data(), record.size());
	df::log(__FUNCTION__, std::format("{} items; record {:08x} over {} bytes", items, checksum, record.size()));

	assert_equal(6001, items, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0xc29fc1f1u), std::format("{:08x}", checksum),
	             "every block is the one libexif wrote");
}

// A lossless turn sets a picture's orientation upright and its dimensions to the turned ones where the
// EXIF block holds them, and leaves every other byte as it was. libexif rewrote the whole block: over
// these fixtures it emptied five Canon maker notes to six bytes and re-encoded five others.
static void should_turn_exif_without_rewriting_it()
{
	std::vector<df::file_path> found;
	scan_parity::add_folder(test_files_folder, found);
	scan_parity::add_folder(test_formats_folder, found);

	const std::set<std::string> set_by_the_turn = {
		"exif.0.0112", "exif.1.0112", "exif.2.a002", "exif.2.a003", "exif.4.1001", "exif.4.1002"
	};

	const auto rows = [](const df::blob& b)
	{
		std::map<std::string, std::string> m;
		for (const auto& r : metadata_exif::to_info({b.data(), b.size()})) m[r.id] = r.key + "|" + r.value + "|" + r.shape;
		return m;
	};

	files ff;
	auto blocks = 0;
	auto with_maker_note = 0;

	for (const auto& path : found)
	{
		auto info = ff_scan_file(ff, path);
		if (info.metadata.exif.size() <= 16) continue;

		df::blob block;
		if (!is_exif_signature(info.metadata.exif)) block.assign(exif_signature.begin(), exif_signature.end());
		block.insert(block.end(), info.metadata.exif.begin(), info.metadata.exif.end());
		++blocks;

		const auto name = std::string(path.name());
		const auto turned = metadata_exif::fix_dims({block.data(), block.size()}, 640, 480);
		assert_equal(block.size(), turned.size(), std::format("{}: the block keeps its size", name));

		size_t changed = 0;
		for (size_t i = 0; i < std::min(block.size(), turned.size()); ++i)
			if (block[i] != turned[i]) ++changed;
		assert_equal(true, changed <= 20, std::format("{}: only the turn's numbers change", name));

		const auto tiff = [](const df::blob& b)
		{
			return std::span<const uint8_t>(b.data() + exif_signature.size(), b.size() - exif_signature.size());
		};

		if (libfiles::read_exif(tiff(block)).orientation)
			assert_equal(1, static_cast<int>(*libfiles::read_exif(tiff(turned)).orientation),
			             std::format("{}: upright", name));

		// The listing reads as it did, but for the numbers the turn set and the facts derived from them.
		const auto was = rows(block);
		const auto is = rows(turned);

		for (const auto& [id, text] : was)
		{
			if (set_by_the_turn.contains(id) || id.starts_with("exif.derived")) continue;
			const auto it = is.find(id);
			assert_equal(text, it == is.end() ? std::string("(absent)") : it->second, std::format("{}: {}", name, id));
		}

		if (const auto it = is.find("exif.2.a002"); it != is.end() &&
			(it->second.find("|Long x1,") != std::string::npos || it->second.find("|Short x1,") != std::string::npos))
			assert_equal(true, it->second.find("|640|") != std::string::npos, std::format("{}: the turned width", name));

		if (was.contains("exif.2.927c")) ++with_maker_note;
	}

	assert_equal(true, blocks >= 20, "the fixtures carry EXIF");
	assert_equal(true, with_maker_note >= 10, "and maker notes");
}

namespace exif_listing_sweep
{
	// A listing as text, every row whole: its place, its text, and its detail's bytes.
	std::string describe(const metadata_kv_list& rows)
	{
		std::string s;

		for (const auto& r : rows)
		{
			s += std::format("{}|{}|{}|{}|{}|{}{}{}", r.id, r.key, r.value, r.shape, r.depth, r.container ? 'c' : '-',
			                 r.open_by_default ? 'o' : '-', r.prose ? 'p' : '-');

			if (const auto* const binary = std::get_if<metadata_binary_detail>(&r.detail))
			{
				s += std::format("|{}{:08x}:{}", binary->is_image ? "image " : "",
				                 df::crc32c(binary->bytes.data(), binary->bytes.size()), binary->bytes.size());
			}
			else if (!std::holds_alternative<std::monostate>(r.detail))
			{
				s += "|other detail";
			}

			s += '\n';
		}

		return s;
	}

	// Every variant of a block the listing sweep reads: whole, cut short, each of its first 256 bytes
	// set to 0, 0xFF, its complement and its successor, and 160 more bytes spread through it flipped.
	void for_each_variant(df::blob& b, const std::function<void(df::cspan)>& read)
	{
		read({b.data(), b.size()});

		for (const auto cut : scan_parity::cut_points(b.size())) read({b.data(), cut});

		const auto corrupt = [&](const size_t at, const uint8_t value)
		{
			const auto original = b[at];
			if (value == original) return;
			b[at] = value;
			read({b.data(), b.size()});
			b[at] = original;
		};

		for (size_t at = 0; at < std::min<size_t>(b.size(), 256); ++at)
		{
			const auto original = b[at];
			std::vector<uint8_t> values = {0, 0xFF, static_cast<uint8_t>(original ^ 0xFF),
			                               static_cast<uint8_t>(original + 1)};
			std::ranges::sort(values);
			values.erase(std::ranges::unique(values).begin(), values.end());
			for (const auto v : values) corrupt(at, v);
		}

		for (uint64_t k = 0; k < 160; ++k)
		{
			const auto at = scan_parity::corruption_position(k, b.size());
			corrupt(at, static_cast<uint8_t>(b[at] ^ 0x5A));
		}
	}
}

// The verbose pane's EXIF listing comes from libfiles, whose names, words and maker note decoders are
// libexif's, ported. Before the switch the two listed alike, every row whole, in each of these 19,354
// listings of the fixtures' EXIF blocks - whole, cut short and corrupted - and in 89,483 with a corpus
// of photographs from seven makers' cameras. This is the record libexif's listing left of the fixtures,
// the same on Windows x64 and x86 and on Linux.
static void should_list_exif_as_libexif_did()
{
	std::vector<df::file_path> found;
	scan_parity::add_folder(test_files_folder, found);
	scan_parity::add_folder(test_formats_folder, found);

	files ff;
	auto blocks = 0;
	auto listings = 0;
	size_t rows = 0;
	uint32_t checksum = 0;

	for (const auto& path : found)
	{
		auto info = ff_scan_file(ff, path);
		if (info.metadata.exif.size() <= 16) continue;
		++blocks;

		exif_listing_sweep::for_each_variant(info.metadata.exif, [&](const df::cspan bytes)
		{
			const auto listing = metadata_exif::to_info(bytes);
			const auto text = exif_listing_sweep::describe(listing);
			const uint32_t fold[] = {checksum, df::crc32c(text.data(), text.size())};
			checksum = df::crc32c(fold, sizeof(fold));
			rows += listing.size();
			++listings;
		});
	}

	df::log(__FUNCTION__, std::format("{} blocks, {} listings, {} rows; record {:08x}", blocks, listings, rows, checksum));

	assert_equal(true, blocks >= 20, "the fixtures carry EXIF");
	assert_equal(19354, listings, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0xaa1b7169u), std::format("{:08x}", checksum),
	             "every row reads as libexif listed it");
}

// Diffractor's Commodore 64 listing moved to libfiles. Before the switch both listed every file in the
// fixtures and in DIFFRACTOR_SCAN_PARITY_CORPUS - lib8bit's test images among them - and 540 generated
// disks of every size, tapes, cartridges and programs, some with directories long enough for the
// listing's cap - whole, cut short and with single bytes corrupted - and agreed on every row's text and
// screen codes and on every pixel drawn: 53,196 comparisons over 631 files, with no difference. That
// comparison left a record of list_disk's outcomes for the fixtures and 54 generated files, the same on
// Windows x64 and x86 and on Linux. The sweep now holds libfiles to it, through the surface the disk
// control draws.
namespace commodore_sweep
{
	// The colours the disk control draws with.
	constexpr auto foreground = ui::rgb(95, 83, 254);
	constexpr auto background = ui::rgb(33, 27, 174);

	struct outcome
	{
		std::vector<std::string> text;
		std::vector<std::vector<uint8_t>> codes;
		sizei dimensions;
		std::vector<uint8_t> pixels;

		bool operator==(const outcome&) const = default;
	};

	outcome run(const df::cspan bytes)
	{
		outcome o;
		const auto listing = libfiles::list_c64_media({bytes.data, bytes.size});

		for (const auto& row : listing.rows)
		{
			o.text.push_back(row.text);
			o.codes.push_back(row.screen_codes);
		}

		if (const auto surface = files::c64_listing_surface(listing, foreground, background))
		{
			o.dimensions = surface->dimensions();
			const auto row_bytes = static_cast<size_t>(o.dimensions.cx) * 4;

			for (auto y = 0; y < o.dimensions.cy; ++y)
			{
				const auto* const row = surface->pixels_line(y);
				o.pixels.insert(o.pixels.end(), row, row + row_bytes);
			}
		}

		return o;
	}

	std::string describe(const outcome& o)
	{
		std::string text;
		std::vector<uint8_t> codes;

		for (size_t i = 0; i < o.text.size(); ++i)
		{
			text += o.text[i];
			text += '\n';
			codes.insert(codes.end(), o.codes[i].begin(), o.codes[i].end());
			codes.push_back(0xFF);
		}

		return std::format("{} rows {}x{} text {:08x} codes {:08x} pixels {:08x}", o.text.size(), o.dimensions.cx,
		                   o.dimensions.cy, df::crc32c(text.data(), text.size()), df::crc32c(codes.data(), codes.size()),
		                   df::crc32c(o.pixels.data(), o.pixels.size()));
	}

	// Bytes from a seed, the same from every compiler: no expression draws twice, since C++ leaves the
	// order of its operands open.
	struct random
	{
		uint32_t seed = 0;

		uint8_t byte()
		{
			seed = seed * 1664525u + 1013904223u;
			return static_cast<uint8_t>(seed >> 24);
		}

		uint32_t below(const uint32_t n)
		{
			uint32_t v = byte();
			v = v << 8 | byte();
			v = v << 8 | byte();
			return v % n;
		}
	};

	void append(df::blob& b, const std::string_view s)
	{
		b.insert(b.end(), s.begin(), s.end());
	}

	void append_random(df::blob& b, random& rng, const size_t n)
	{
		for (size_t i = 0; i < n; ++i) b.push_back(rng.byte());
	}

	void append_le16(df::blob& b, const uint32_t v)
	{
		b.push_back(static_cast<uint8_t>(v));
		b.push_back(static_cast<uint8_t>(v >> 8));
	}

	// A name of n bytes: any bytes at all, or capitals padded as the format pads them.
	void append_name(df::blob& b, random& rng, const size_t n, const uint8_t pad)
	{
		if (rng.below(3) == 0)
		{
			append_random(b, rng, n);
			return;
		}

		const auto length = rng.below(static_cast<uint32_t>(n) + 1);
		for (size_t i = 0; i < n; ++i) b.push_back(i < length ? static_cast<uint8_t>('A' + rng.below(26)) : pad);
	}

	// A disk any of whose sectors could be a directory sector: links mostly to its own tracks, sometimes
	// ending the chain or leaving the disk, and entries of every type with names of any bytes.
	df::blob make_disk(const size_t size, const uint32_t tracks, random& rng)
	{
		static constexpr uint8_t types[] = {0, 0, 0x80, 0x81, 0x82, 0x82, 0x82, 0x83, 0x84, 0xC2, 0x99, 0x01};

		df::blob b;
		b.reserve(size);
		append_random(b, rng, size);

		for (size_t sector = 0; sector + 256 <= size; sector += 256)
		{
			const auto pick = rng.byte();
			if (pick < 24) b[sector] = 0;
			else if (pick < 40) b[sector] = rng.byte();
			else b[sector] = static_cast<uint8_t>(1 + rng.below(tracks + 2));
			b[sector + 1] = static_cast<uint8_t>(rng.below(pick & 1 ? 21 : 40));

			for (size_t slot = 0; slot < 8; ++slot)
			{
				const auto t = rng.byte();
				b[sector + slot * 32 + 2] = t < 192 ? types[t % std::size(types)] : rng.byte();
			}
		}

		// One disk in four has a directory that runs on, sector after sector in the order the drive
		// zones them, with a file in every entry: long enough for the listing's cap.
		if (rng.below(4) == 0)
		{
			std::vector<std::pair<uint32_t, uint32_t>> order;

			for (uint32_t t = 1; t <= tracks; ++t)
			{
				const auto zone = tracks == 70 && t > 35 ? t - 35 : t;
				const auto count = tracks == 80 ? 40u : zone <= 17 ? 21u : zone <= 24 ? 19u : zone <= 30 ? 18u : 17u;
				for (uint32_t s = 0; s < count; ++s) order.emplace_back(t, s);
			}

			const auto first = tracks == 80 ? 39u * 40u + 3u : 357u + 1u;
			const auto length = 60 + rng.below(40);

			for (uint32_t i = first; i < first + length && i + 1 < order.size(); ++i)
			{
				const auto sector = static_cast<size_t>(i) * 256;
				b[sector] = static_cast<uint8_t>(order[i + 1].first);
				b[sector + 1] = static_cast<uint8_t>(order[i + 1].second);
				for (size_t slot = 0; slot < 8; ++slot) b[sector + slot * 32 + 2] = 0x82;
			}
		}

		return b;
	}

	df::blob make_t64(random& rng)
	{
		static constexpr std::string_view descriptions[] = {
			"C64S tape image file", "C64 tape image file", "C64S tape file", "C64 tape", "C64 cassette image"
		};
		static constexpr uint8_t kinds[] = {0, 1, 1, 1, 1, 3};
		static constexpr uint8_t types[] = {0, 0x80, 0x81, 0x82, 0x82, 0x82, 0x83, 0x84, 0xC2};

		df::blob b;
		append(b, descriptions[rng.below(static_cast<uint32_t>(std::size(descriptions)))]);
		while (b.size() < 32) b.push_back(rng.below(4) == 0 ? rng.byte() : 0);
		append_random(b, rng, 2); // the version
		append_le16(b, rng.below(8) == 0 ? rng.below(65536) : rng.below(48)); // room
		append_le16(b, rng.below(8) == 0 ? rng.below(65536) : rng.below(48)); // used
		append_random(b, rng, 2);
		append_name(b, rng, 24, 0x20);

		const auto entries = rng.below(40);

		for (uint32_t i = 0; i < entries; ++i)
		{
			const auto kind = rng.below(8);
			b.push_back(kind < std::size(kinds) ? kinds[kind] : rng.byte());
			const auto type = rng.below(10);
			b.push_back(type < std::size(types) ? types[type] : rng.byte());
			const auto start = rng.below(2) == 0 ? 0x0801u : rng.below(65536);
			append_le16(b, start);
			append_le16(b, rng.below(2) == 0 ? start + rng.below(50000) : rng.below(65536));
			append_random(b, rng, 10);
			append_name(b, rng, 16, 0x20);
		}

		if (rng.below(4) == 0) b.resize(rng.below(static_cast<uint32_t>(b.size()) + 1));
		return b;
	}

	df::blob make_crt(random& rng)
	{
		df::blob b;
		append(b, "C64 CARTRIDGE   ");
		append_random(b, rng, 16);
		append_name(b, rng, 32, 0);
		append_random(b, rng, rng.below(20000));
		return b;
	}

	df::blob make_p00(random& rng)
	{
		df::blob b;
		append(b, "C64File");
		b.push_back(rng.below(4) == 0 ? rng.byte() : 0);
		append_name(b, rng, 16, 0);
		append_random(b, rng, 2);
		append_random(b, rng, rng.below(4) == 0 ? rng.below(70000) : rng.below(3000));
		return b;
	}

	df::blob make_prg(random& rng)
	{
		static constexpr uint32_t lengths[] = {0, 1, 2, 3, 254, 255, 256, 65536, 65537, 65538, 65539};
		const auto pick = rng.below(16);
		df::blob b;
		append_random(b, rng, pick < std::size(lengths) ? lengths[pick] : rng.below(70000));
		return b;
	}

	// Disks of every size each times, and tapes, cartridges and programs more times.
	std::vector<std::pair<std::string, df::blob>> synthetic(const int each, random& rng)
	{
		struct disk
		{
			size_t size;
			uint32_t tracks;
		};

		static constexpr disk disks[] = {
			{174848, 35}, {175531, 35}, {196608, 40}, {197376, 40}, {349696, 70}, {351062, 70}, {819200, 80}
		};

		std::vector<std::pair<std::string, df::blob>> result;

		for (const auto& d : disks)
		{
			for (auto i = 0; i < each; ++i) result.emplace_back(std::format("disk {} {}", d.size, i), make_disk(d.size, d.tracks, rng));
		}

		for (auto i = 0; i < each * 8; ++i) result.emplace_back(std::format("t64 {}", i), make_t64(rng));
		for (auto i = 0; i < each * 4; ++i) result.emplace_back(std::format("crt {}", i), make_crt(rng));
		for (auto i = 0; i < each * 4; ++i) result.emplace_back(std::format("p00 {}", i), make_p00(rng));
		for (auto i = 0; i < each * 4; ++i) result.emplace_back(std::format("prg {}", i), make_prg(rng));
		return result;
	}

	// Where a disk of this size keeps its first directory sector, or 0 for a size no disk has.
	size_t directory_offset(const size_t size)
	{
		switch (size)
		{
		case 174848:
		case 175531:
		case 196608:
		case 197376:
		case 349696:
		case 351062:
			return 0x16600;
		case 819200:
			return 0x61B00;
		default:
			return 0;
		}
	}

	// A file whole, cut short and with single bytes corrupted, as the other sweeps vary them, and for a
	// disk its first directory sector corrupted too, where those rarely land: its link, and each entry's
	// type, size and the ends of its name.
	template <typename Visit>
	void for_each_variant(df::blob& bytes, Visit&& visit)
	{
		visit(df::cspan(bytes));
		for (const auto cut : scan_parity::cut_points(bytes.size())) visit(df::cspan{bytes.data(), cut});
		if (bytes.size() <= 16) return;

		const auto corrupt = [&](const size_t at, const uint8_t value)
		{
			const auto original = bytes[at];
			bytes[at] = value;
			if (value != original) visit(df::cspan(bytes));
			bytes[at] = original;
		};

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			corrupt(at, static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF)));
		}

		const auto directory = directory_offset(bytes.size());
		if (directory == 0) return;

		for (const int track : {0, 1, 17, 18, 19, 35, 36, 40, 41, 53, 70, 71, 80, 81, 255})
			corrupt(directory, static_cast<uint8_t>(track));
		for (const int sector : {0, 1, 3, 19, 20, 21, 39, 40, 255})
			corrupt(directory + 1, static_cast<uint8_t>(sector));

		for (size_t slot = 0; slot < 8; ++slot)
		{
			for (const size_t field : {2u, 5u, 20u, 30u, 31u})
			{
				const auto at = directory + slot * 32 + field;
				corrupt(at, static_cast<uint8_t>(bytes[at] ^ 0xFF));
			}
		}
	}

	struct entry
	{
		std::string name;
		df::blob bytes;
		bool recorded = false;
	};

	// The fixtures, recorded, and DIFFRACTOR_SCAN_PARITY_CORPUS's folders, listed but not recorded.
	std::vector<entry> corpus()
	{
		std::vector<df::file_path> found;
		scan_parity::add_folder(test_files_folder, found);
		scan_parity::add_folder(test_formats_folder, found);
		const auto fixtures = found.size();

		const auto extra = scan_parity::environment_variable("DIFFRACTOR_SCAN_PARITY_CORPUS");
		for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
		{
			scan_parity::add_folder(df::folder_path(part), found);
		}

		std::vector<entry> result;
		for (size_t i = 0; i < found.size(); ++i)
			result.push_back({std::string(found[i].name()), df::blob_from_file(found[i]), i < fixtures});
		return result;
	}
}

static void should_list_commodore_media_as_list_disk_did()
{
	auto corpus = commodore_sweep::corpus();
	const auto has_disk = std::ranges::any_of(corpus, [](const commodore_sweep::entry& e) { return e.name == "Ace of Aces (Europe).D64"; });

	commodore_sweep::random rng{0x00C64C64};
	for (auto& [name, bytes] : commodore_sweep::synthetic(2, rng)) corpus.push_back({std::move(name), std::move(bytes), true});

	auto listings = 0;
	auto listed = 0;
	auto recorded_outcomes = 0;
	uint32_t checksum = 0;

	for (auto& e : corpus)
	{
		commodore_sweep::for_each_variant(e.bytes, [&](const df::cspan bytes)
		{
			const auto outcome = commodore_sweep::run(bytes);
			++listings;
			if (!outcome.text.empty()) ++listed;
			if (!e.recorded) return;

			const auto text = commodore_sweep::describe(outcome);
			const uint32_t fold[] = {checksum, df::crc32c(text.data(), text.size())};
			checksum = df::crc32c(fold, sizeof(fold));
			++recorded_outcomes;
		});
	}

	df::log(__FUNCTION__, std::format("{} files, {} listings, {} listed; record {:08x} over {} outcomes", corpus.size(),
	                                  listings, listed, checksum, recorded_outcomes));

	assert_equal(true, has_disk, "the fixtures hold a disk");
	assert_equal(true, listed > 1000, "much of what is listed lists");
	assert_equal(10762, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0xedcf99f2u), std::format("{:08x}", checksum),
	             "every listing is the one list_disk made before it moved to libfiles");
}

// Diffractor's PNG loader moved to libfiles. Before the switch both decoded every PNG in the fixtures
// and in DIFFRACTOR_SCAN_PARITY_CORPUS - libpng's PngSuite and contrib/testpngs among them - and 120
// generated PNGs of every colour type and depth, interlaced or not, under each gamma, with tRNS and with
// an eXIf before or after the image data; whole, cut short, with single bytes corrupted, and with image
// data corrupted under a repaired CRC. They agreed on whether the load threw, on a null or an empty
// surface, and on its size, format, orientation and every pixel byte: 27,573 comparisons over 324
// files, with no difference. That comparison left a record of load_png's outcomes for the fixtures and
// 120 generated files, the same on Windows x64 and x86 and on Linux. The sweep now holds libfiles to it.
namespace png_sweep
{
	struct outcome
	{
		bool threw = false;
		bool null = false;
		sizei dimensions;
		int format = -1;
		int orientation = -1;
		std::vector<uint8_t> rows;

		bool operator==(const outcome&) const = default;
	};

	template <typename Load>
	outcome run(Load&& load, const df::cspan bytes)
	{
		outcome o;

		try
		{
			const auto surface = load(bytes);
			o.null = !surface;

			if (surface)
			{
				o.dimensions = surface->dimensions();
				o.format = static_cast<int>(surface->format());
				o.orientation = static_cast<int>(surface->orientation());

				if (!surface->empty())
				{
					const auto row_bytes = static_cast<size_t>(o.dimensions.cx) * 4;
					o.rows.reserve(row_bytes * o.dimensions.cy);

					for (auto y = 0; y < o.dimensions.cy; ++y)
					{
						const auto* const row = surface->pixels_line(y);
						o.rows.insert(o.rows.end(), row, row + row_bytes);
					}
				}
			}
		}
		catch (const std::exception&)
		{
			o.threw = true;
		}

		return o;
	}

	std::string describe(const outcome& o)
	{
		return std::format("threw {} null {} {}x{} format {} orientation {} {} bytes {:08x}", o.threw, o.null,
		                   o.dimensions.cx, o.dimensions.cy, o.format, o.orientation, o.rows.size(),
		                   df::crc32c(o.rows.data(), o.rows.size()));
	}

	[[noreturn]] void PNGCBAPI write_failed(png_structp, const png_const_charp message)
	{
		throw std::runtime_error(message);
	}

	void PNGCBAPI write_quietly(png_structp, png_const_charp)
	{
	}

	void PNGCBAPI append(const png_structp png, const png_bytep data, const size_t length)
	{
		auto* const out = static_cast<df::blob*>(png_get_io_ptr(png));
		out->insert(out->end(), data, data + length);
	}

	void PNGCBAPI flush_nothing(png_structp)
	{
	}

	uint32_t be32(const df::blob& b, const size_t at)
	{
		return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 |
			static_cast<uint32_t>(b[at + 2]) << 8 | b[at + 3];
	}

	void put_be32(df::blob& b, const size_t at, const uint32_t v)
	{
		b[at] = static_cast<uint8_t>(v >> 24);
		b[at + 1] = static_cast<uint8_t>(v >> 16);
		b[at + 2] = static_cast<uint8_t>(v >> 8);
		b[at + 3] = static_cast<uint8_t>(v);
	}

	// Where the first chunk of a type begins, or 0. Walked in 64 bits, so a damaged length cannot wrap.
	size_t find_chunk(const df::blob& file, const std::string_view type)
	{
		for (uint64_t at = 8; at + 12 <= file.size();)
		{
			const auto i = static_cast<size_t>(at);
			if (std::string_view(std::bit_cast<const char*>(file.data() + i + 4), 4) == type) return i;
			at += 12 + static_cast<uint64_t>(be32(file, i));
		}

		return 0;
	}

	// The chunk's CRC, over its type and data, as zlib computes it.
	uint32_t chunk_crc(const df::blob& file, const size_t at, const uint32_t length)
	{
		return static_cast<uint32_t>(crc32(0, file.data() + at + 4, static_cast<uInt>(length + 4)));
	}

	df::blob splice(const df::blob& file, const size_t at, const std::string_view type, const df::blob& data)
	{
		df::blob chunk(12 + data.size());
		put_be32(chunk, 0, static_cast<uint32_t>(data.size()));
		std::copy(type.begin(), type.end(), chunk.begin() + 4);
		std::copy(data.begin(), data.end(), chunk.begin() + 8);
		put_be32(chunk, 8 + data.size(), chunk_crc(chunk, 0, static_cast<uint32_t>(data.size())));

		df::blob result(file.begin(), file.begin() + static_cast<ptrdiff_t>(at));
		result.insert(result.end(), chunk.begin(), chunk.end());
		result.insert(result.end(), file.begin() + static_cast<ptrdiff_t>(at), file.end());
		return result;
	}

	// A little-endian TIFF stream with one entry: the orientation.
	df::blob orientation_exif(const uint16_t value)
	{
		return {'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0,
		        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), 0, 0, 0, 0, 0, 0};
	}

	uint8_t next_byte(uint32_t& seed)
	{
		seed = seed * 1664525u + 1013904223u;
		return static_cast<uint8_t>(seed >> 24);
	}

	struct spec
	{
		int color_type = PNG_COLOR_TYPE_RGB;
		int bit_depth = 8;
		bool interlaced = false;
		bool transparency = false;
		int gamma = 0; // none, a linear gAMA, sRGB, or a gAMA of 1/2.2
		uint16_t exif_before = 0;
		uint16_t exif_after = 0;
	};

	// A PNG of noise as libpng writes one, 13 by 7 so every interlace pass has pixels, with an eXIf
	// spliced in before the image data or after it, where libpng will not put one.
	df::blob make_png(const spec& s, uint32_t& seed)
	{
		constexpr png_uint_32 width = 13;
		constexpr png_uint_32 height = 7;

		const auto channels = s.color_type == PNG_COLOR_TYPE_GRAY || s.color_type == PNG_COLOR_TYPE_PALETTE
			                      ? 1u
			                      : s.color_type == PNG_COLOR_TYPE_GRAY_ALPHA
			                      ? 2u
			                      : s.color_type == PNG_COLOR_TYPE_RGB
			                      ? 3u
			                      : 4u;
		const auto row_bytes = (width * channels * static_cast<uint32_t>(s.bit_depth) + 7u) / 8u;

		std::vector<std::vector<uint8_t>> rows(height, std::vector<uint8_t>(row_bytes));
		for (auto& row : rows) for (auto& b : row) b = next_byte(seed);

		df::blob out;
		auto* png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, write_failed, write_quietly);
		auto* info = png_create_info_struct(png);
		png_set_write_fn(png, &out, append, flush_nothing);
		png_set_IHDR(png, info, width, height, s.bit_depth, s.color_type,
		             s.interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		             PNG_FILTER_TYPE_DEFAULT);

		std::vector<png_color> palette;
		std::vector<uint8_t> alpha;

		if (s.color_type == PNG_COLOR_TYPE_PALETTE)
		{
			palette.resize(size_t{1} << s.bit_depth);

			for (auto& c : palette)
			{
				c.red = next_byte(seed);
				c.green = next_byte(seed);
				c.blue = next_byte(seed);
			}

			png_set_PLTE(png, info, palette.data(), static_cast<int>(palette.size()));

			if (s.transparency)
			{
				alpha.resize(std::min<size_t>(palette.size(), 5));
				for (auto& a : alpha) a = next_byte(seed);
				png_set_tRNS(png, info, alpha.data(), static_cast<int>(alpha.size()), nullptr);
			}
		}
		else if (s.transparency && (s.color_type == PNG_COLOR_TYPE_GRAY || s.color_type == PNG_COLOR_TYPE_RGB))
		{
			// The first pixel's colour is the transparent one, so at least one pixel is transparent.
			const auto& first = rows[0];
			png_color_16 colour{};

			if (s.color_type == PNG_COLOR_TYPE_GRAY)
			{
				colour.gray = s.bit_depth == 16
					              ? static_cast<png_uint_16>(first[0] << 8 | first[1])
					              : static_cast<png_uint_16>(first[0] >> (8 - s.bit_depth));
			}
			else if (s.bit_depth == 16)
			{
				colour.red = static_cast<png_uint_16>(first[0] << 8 | first[1]);
				colour.green = static_cast<png_uint_16>(first[2] << 8 | first[3]);
				colour.blue = static_cast<png_uint_16>(first[4] << 8 | first[5]);
			}
			else
			{
				colour.red = first[0];
				colour.green = first[1];
				colour.blue = first[2];
			}

			png_set_tRNS(png, info, nullptr, 0, &colour);
		}

		if (s.gamma == 1) png_set_gAMA_fixed(png, info, PNG_GAMMA_LINEAR);
		if (s.gamma == 2) png_set_sRGB_gAMA_and_cHRM(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
		if (s.gamma == 3) png_set_gAMA_fixed(png, info, 45455);

		std::vector<png_bytep> pointers;
		for (auto& row : rows) pointers.push_back(row.data());

		png_write_info(png, info);
		png_write_image(png, pointers.data());
		png_write_end(png, info);
		png_destroy_write_struct(&png, &info);

		if (s.exif_before) out = splice(out, find_chunk(out, "IDAT"), "eXIf", orientation_exif(s.exif_before));
		if (s.exif_after) out = splice(out, find_chunk(out, "IEND"), "eXIf", orientation_exif(s.exif_after));
		return out;
	}

	// Every colour type at every depth it has, interlaced and not, under each gamma; with transparency
	// in every other file, and an orientation before or after the image data in two of every three.
	std::vector<std::pair<std::string, df::blob>> synthetic(uint32_t& seed)
	{
		struct kind
		{
			int color_type;
			std::vector<int> depths;
		};

		const kind kinds[] = {
			{PNG_COLOR_TYPE_GRAY, {1, 2, 4, 8, 16}}, {PNG_COLOR_TYPE_GRAY_ALPHA, {8, 16}},
			{PNG_COLOR_TYPE_RGB, {8, 16}}, {PNG_COLOR_TYPE_RGB_ALPHA, {8, 16}}, {PNG_COLOR_TYPE_PALETTE, {1, 2, 4, 8}}
		};

		std::vector<std::pair<std::string, df::blob>> result;
		auto n = 0;

		for (const auto& k : kinds)
		{
			for (const auto depth : k.depths)
			{
				for (const auto interlaced : {false, true})
				{
					for (const auto gamma : {0, 1, 2, 3})
					{
						spec s;
						s.color_type = k.color_type;
						s.bit_depth = depth;
						s.interlaced = interlaced;
						s.gamma = gamma;
						s.transparency = n % 2 == 0;
						s.exif_before = n % 3 == 1 ? static_cast<uint16_t>(1 + n % 8) : uint16_t{0};
						s.exif_after = n % 3 == 2 ? static_cast<uint16_t>(1 + (n + 3) % 8) : uint16_t{0};
						result.emplace_back(std::format("type {} depth {} interlaced {} gamma {} #{}", k.color_type,
						                                depth, interlaced, gamma, n), make_png(s, seed));
						++n;
					}
				}
			}
		}

		return result;
	}

	// A file whole, cut short and with single bytes corrupted, as the other sweeps vary them; and with
	// bytes of its first image data chunk corrupted and that chunk's CRC put right, so libpng inflates
	// and unfilters damaged data rather than stopping at the CRC.
	template <typename Visit>
	void for_each_variant(df::blob& bytes, Visit&& visit)
	{
		visit(df::cspan(bytes));
		for (const auto cut : scan_parity::cut_points(bytes.size())) visit(df::cspan{bytes.data(), cut});
		if (bytes.size() <= 16) return;

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));
			if (bytes[at] != original) visit(df::cspan(bytes));
			bytes[at] = original;
		}

		const auto idat = find_chunk(bytes, "IDAT");
		if (idat == 0) return;

		const auto length = be32(bytes, idat);
		if (length == 0 || idat + 12 + uint64_t{length} > bytes.size()) return;

		const size_t crc_at = idat + 8 + length;
		const auto crc = be32(bytes, crc_at);

		for (uint64_t k = 0; k < 24; ++k)
		{
			const auto at = idat + 8 + static_cast<size_t>(k * 2654435761ull % length);
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(original ^ (k % 2 ? 0xFF : 0x5A));
			put_be32(bytes, crc_at, chunk_crc(bytes, idat, length));
			visit(df::cspan(bytes));
			bytes[at] = original;
			put_be32(bytes, crc_at, crc);
		}
	}

	struct entry
	{
		std::string name;
		df::blob bytes;
		bool recorded = false;
	};

	// The fixtures' PNGs, recorded, and DIFFRACTOR_SCAN_PARITY_CORPUS's, decoded but not recorded.
	std::vector<entry> corpus()
	{
		std::vector<df::file_path> found;
		scan_parity::add_folder(test_files_folder, found);
		scan_parity::add_folder(test_formats_folder, found);
		const auto fixtures = found.size();

		const auto extra = scan_parity::environment_variable("DIFFRACTOR_SCAN_PARITY_CORPUS");
		for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
		{
			scan_parity::add_folder(df::folder_path(part), found);
		}

		std::vector<entry> result;

		for (size_t i = 0; i < found.size(); ++i)
		{
			auto bytes = df::blob_from_file(found[i]);
			if (bytes.size() >= 16 && files::detect_format(bytes) == detected_format::PNG)
				result.push_back({std::string(found[i].name()), std::move(bytes), i < fixtures});
		}

		return result;
	}
}

static void should_decode_pngs_as_load_png_did()
{
	auto corpus = png_sweep::corpus();
	const auto real_files = corpus.size();

	uint32_t seed = 4242;
	for (auto& [name, bytes] : png_sweep::synthetic(seed)) corpus.push_back({std::move(name), std::move(bytes), true});

	auto decodes = 0;
	auto decoded = 0;
	auto recorded_outcomes = 0;
	uint32_t checksum = 0;

	for (auto& e : corpus)
	{
		png_sweep::for_each_variant(e.bytes, [&](const df::cspan bytes)
		{
			const auto outcome = png_sweep::run([](const df::cspan b) { return load_png(b); }, bytes);
			++decodes;
			if (!outcome.rows.empty()) ++decoded;
			if (!e.recorded) return;

			const auto text = png_sweep::describe(outcome);
			const uint32_t fold[] = {checksum, df::crc32c(text.data(), text.size())};
			checksum = df::crc32c(fold, sizeof(fold));
			++recorded_outcomes;
		});
	}

	df::log(__FUNCTION__, std::format("{} files ({} real), {} decodes, {} decoded; record {:08x} over {} outcomes",
	                                  corpus.size(), real_files, decodes, decoded, checksum, recorded_outcomes));

	assert_equal(true, real_files >= 5, "the fixtures hold PNGs");
	assert_equal(true, decoded > 1000, "much of what is decoded decodes");
	assert_equal(10748, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0x07e85724u), std::format("{:08x}", checksum),
	             "every outcome is the one load_png had before it moved to libfiles");
}

// Diffractor's WebP loader moved to libfiles. Before the switch both decoded every WebP in the fixtures
// and in DIFFRACTOR_SCAN_PARITY_CORPUS - libwebp's samples and a photo library's among them - and 36
// generated ones: lossy and lossless, opaque and alpha, odd and even, with EXIF chunks with and without
// a JPEG signature, ICC and XMP chunks, animations, and headers that claim 16383 by 16383. Whole, cut
// short and with single bytes corrupted, each under nine requests - packed and NV12, unreduced,
// reduced, enlarged and with a zero edge - they agreed on whether the load threw, on a null surface,
// and on its size, format, orientation, colour space and every byte of every plane: 25,335
// comparisons over 44 files, with no difference. That comparison left a record of load_webp's outcomes
// for the fixtures and the generated files, which libwebp's encoder writes alike on Windows x64 and x86
// and on Linux, where the record is the same. The sweep now holds libfiles to it.
namespace webp_sweep
{
	struct outcome
	{
		bool threw = false;
		bool null = false;
		sizei dimensions;
		int format = -1;
		int orientation = -1;
		int color_space = -1;
		std::vector<uint8_t> rows;

		bool operator==(const outcome&) const = default;
	};

	struct request
	{
		bool yuv = false;
		sizei target;
	};

	// Packed and planar, unreduced, reduced, enlarged and with a target that has a zero edge.
	const request requests[] = {
		{false, {}}, {true, {}}, {false, {320, 240}}, {true, {320, 240}}, {false, {33, 17}}, {false, {1, 1}},
		{true, {5000, 5000}}, {false, {0, 100}}, {true, {64, 0}}
	};

	template <typename Load>
	outcome run(Load&& load, const df::cspan bytes, const request& r)
	{
		outcome o;

		try
		{
			const auto surface = load(bytes, r.yuv, r.target);
			o.null = !surface;

			if (surface)
			{
				o.dimensions = surface->dimensions();
				o.format = static_cast<int>(surface->format());
				o.orientation = static_cast<int>(surface->orientation());
				o.color_space = static_cast<int>(surface->color_space());

				if (!surface->empty())
				{
					const auto nv12 = surface->format() == ui::texture_format::NV12;
					const auto row_bytes = static_cast<size_t>(o.dimensions.cx) * (nv12 ? 1 : 4);
					const auto rows = o.dimensions.cy + (nv12 ? o.dimensions.cy / 2 : 0);

					for (auto y = 0; y < rows; ++y)
					{
						const auto* const row = surface->pixels() + static_cast<size_t>(y) * surface->stride();
						o.rows.insert(o.rows.end(), row, row + row_bytes);
					}
				}
			}
		}
		catch (const std::exception&)
		{
			o.threw = true;
		}

		return o;
	}

	std::string describe(const outcome& o)
	{
		return std::format("threw {} null {} {}x{} format {} orientation {} cs {} {} bytes {:08x}", o.threw, o.null,
		                   o.dimensions.cx, o.dimensions.cy, o.format, o.orientation, o.color_space, o.rows.size(),
		                   df::crc32c(o.rows.data(), o.rows.size()));
	}

	uint8_t next_byte(uint32_t& seed)
	{
		seed = seed * 1664525u + 1013904223u;
		return static_cast<uint8_t>(seed >> 24);
	}

	// A gradient under noise, with alpha that varies when asked.
	std::vector<uint8_t> make_bgra(const int width, const int height, const bool alpha, uint32_t& seed)
	{
		std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);

		for (auto y = 0; y < height; ++y)
		{
			for (auto x = 0; x < width; ++x)
			{
				auto* const p = bgra.data() + (static_cast<size_t>(y) * width + x) * 4;
				const auto noise = next_byte(seed) >> 3;
				p[0] = static_cast<uint8_t>(x * 255 / std::max(1, width - 1));
				p[1] = static_cast<uint8_t>(y * 255 / std::max(1, height - 1));
				p[2] = static_cast<uint8_t>(80 + noise);
				const auto a = next_byte(seed);
				p[3] = alpha ? a : 0xFF;
			}
		}

		return bgra;
	}

	df::blob encode(const int width, const int height, const bool alpha, const bool lossless, uint32_t& seed)
	{
		const auto bgra = make_bgra(width, height, alpha, seed);
		uint8_t* out = nullptr;
		const auto size = lossless
			                  ? WebPEncodeLosslessBGRA(bgra.data(), width, height, width * 4, &out)
			                  : WebPEncodeBGRA(bgra.data(), width, height, width * 4, 80.f, &out);
		df::blob file(out, out + size);
		WebPFree(out);
		return file;
	}

	// The file with metadata chunks added by libwebp's mux, which writes the VP8X header they need.
	df::blob with_chunks(const df::blob& file, const df::blob& exif, const bool icc, const bool xmp)
	{
		const WebPData image{file.data(), file.size()};
		auto* const mux = WebPMuxCreate(&image, 1);
		if (!mux) return file.clone();

		if (!exif.empty())
		{
			const WebPData chunk{exif.data(), exif.size()};
			WebPMuxSetChunk(mux, "EXIF", &chunk, 1);
		}

		static constexpr uint8_t profile[] = {0, 0, 0, 24, 'n', 'o', 'n', 'e', 0, 0, 0, 0, 'm', 'n', 't', 'r', 'R', 'G', 'B', ' ', 'X', 'Y', 'Z', ' '};
		static constexpr char packet[] = "<x:xmpmeta xmlns:x='adobe:ns:meta/'/>";

		if (icc)
		{
			const WebPData chunk{profile, sizeof(profile)};
			WebPMuxSetChunk(mux, "ICCP", &chunk, 1);
		}

		if (xmp)
		{
			const WebPData chunk{std::bit_cast<const uint8_t*>(&packet[0]), sizeof(packet) - 1};
			WebPMuxSetChunk(mux, "XMP ", &chunk, 1);
		}

		WebPData assembled{};
		WebPMuxAssemble(mux, &assembled);
		df::blob result(assembled.bytes, assembled.bytes + assembled.size);
		WebPDataClear(&assembled);
		WebPMuxDelete(mux);
		return result;
	}

	// A little-endian TIFF stream with one entry, the orientation, led by a JPEG signature when asked.
	df::blob orientation_exif(const uint16_t value, const bool signature)
	{
		df::blob b;
		if (signature) b = {'E', 'x', 'i', 'f', 0, 0};
		const uint8_t tiff[] = {'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0,
		                        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), 0, 0, 0, 0, 0, 0};
		b.insert(b.end(), std::begin(tiff), std::end(tiff));
		return b;
	}

	df::blob animation(const bool lossless, uint32_t& seed)
	{
		WebPAnimEncoderOptions options;
		WebPAnimEncoderOptionsInit(&options);
		auto* const encoder = WebPAnimEncoderNew(19, 13, &options);

		for (auto frame = 0; frame < 3; ++frame)
		{
			const auto bgra = make_bgra(19, 13, frame == 1, seed);
			WebPPicture picture;
			WebPPictureInit(&picture);
			picture.width = 19;
			picture.height = 13;
			picture.use_argb = 1;
			WebPPictureImportBGRA(&picture, bgra.data(), 19 * 4);
			WebPConfig config;
			WebPConfigInit(&config);
			config.lossless = lossless ? 1 : 0;
			WebPAnimEncoderAdd(encoder, &picture, frame * 100, &config);
			WebPPictureFree(&picture);
		}

		WebPAnimEncoderAdd(encoder, nullptr, 300, nullptr);
		WebPData assembled{};
		WebPAnimEncoderAssemble(encoder, &assembled);
		df::blob result(assembled.bytes, assembled.bytes + assembled.size);
		WebPDataClear(&assembled);
		WebPAnimEncoderDelete(encoder);
		return result;
	}

	// A lossless header that claims width by height, and a few bytes of nothing after it: enough for
	// libwebp to give the size, not to decode.
	df::blob header_only(const uint32_t width, const uint32_t height)
	{
		const auto bits = (width - 1) | (height - 1) << 14 | 1u << 28;
		const uint8_t payload[] = {0x2f, static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
		                           static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24), 0, 0, 0, 0, 0, 0, 0};
		df::blob b = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'L',
		              static_cast<uint8_t>(sizeof(payload)), 0, 0, 0};
		b.insert(b.end(), std::begin(payload), std::end(payload));
		const auto riff = static_cast<uint32_t>(b.size() - 8);
		b[4] = static_cast<uint8_t>(riff);
		b[5] = static_cast<uint8_t>(riff >> 8);
		return b;
	}

	std::vector<std::pair<std::string, df::blob>> synthetic(uint32_t& seed)
	{
		static constexpr sizei sizes[] = {{37, 23}, {38, 24}, {1, 1}, {2, 2}, {3, 1}, {1, 3}, {64, 48}, {17, 1}};
		std::vector<std::pair<std::string, df::blob>> result;
		auto n = 0;

		for (const auto& s : sizes)
		{
			for (const auto lossless : {false, true})
			{
				for (const auto alpha : {false, true})
				{
					auto file = encode(s.cx, s.cy, alpha, lossless, seed);

					// Two of every three with an orientation, every other of those led by a JPEG
					// signature; a profile or a packet besides, now and then.
					if (n % 3 != 0 || n % 5 == 0)
					{
						const auto exif = n % 3 != 0 ? orientation_exif(static_cast<uint16_t>(1 + n % 8), n % 2 == 0) : df::blob{};
						file = with_chunks(file, exif, n % 5 == 0, n % 4 == 1);
					}

					result.emplace_back(std::format("{}x{} {} {} #{}", s.cx, s.cy, lossless ? "lossless" : "lossy",
					                                alpha ? "alpha" : "opaque", n), std::move(file));
					++n;
				}
			}
		}

		result.emplace_back("animation lossy", animation(false, seed));
		result.emplace_back("animation lossless", animation(true, seed));
		result.emplace_back("header 16383x16383", header_only(16383, 16383));
		result.emplace_back("header 3000x2000", header_only(3000, 2000));
		return result;
	}

	struct entry
	{
		std::string name;
		df::blob bytes;
		bool recorded = false;
	};

	// The fixtures' WebPs, recorded, and DIFFRACTOR_SCAN_PARITY_CORPUS's, decoded but not recorded.
	std::vector<entry> corpus()
	{
		std::vector<df::file_path> found;
		scan_parity::add_folder(test_files_folder, found);
		scan_parity::add_folder(test_formats_folder, found);
		const auto fixtures = found.size();

		const auto extra = scan_parity::environment_variable("DIFFRACTOR_SCAN_PARITY_CORPUS");
		for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
		{
			scan_parity::add_folder(df::folder_path(part), found);
		}

		std::vector<entry> result;

		for (size_t i = 0; i < found.size(); ++i)
		{
			df::blob bytes;

			try
			{
				bytes = df::blob_from_file(found[i]);
			}
			catch (const std::exception&)
			{
				// A corpus folder may hold a file too large to read whole - a video - which is no WebP.
				if (i < fixtures) throw;
				continue;
			}

			if (bytes.size() >= 16 && files::detect_format(bytes) == detected_format::WEBP)
				result.push_back({std::string(found[i].name()), std::move(bytes), i < fixtures});
		}

		return result;
	}

	// A file whole, cut short and with single bytes corrupted, as the other sweeps vary them.
	template <typename Visit>
	void for_each_variant(df::blob& bytes, Visit&& visit)
	{
		visit(df::cspan(bytes));
		for (const auto cut : scan_parity::cut_points(bytes.size())) visit(df::cspan{bytes.data(), cut});
		if (bytes.size() <= 16) return;

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));
			if (bytes[at] != original) visit(df::cspan(bytes));
			bytes[at] = original;
		}
	}
}

static void should_decode_webps_as_load_webp_did()
{
	auto corpus = webp_sweep::corpus();
	const auto real_files = corpus.size();

	uint32_t seed = 31337;
	for (auto& [name, bytes] : webp_sweep::synthetic(seed)) corpus.push_back({std::move(name), std::move(bytes), true});

	auto decodes = 0;
	auto decoded = 0;
	auto recorded_outcomes = 0;
	uint32_t checksum = 0;

	for (auto& e : corpus)
	{
		webp_sweep::for_each_variant(e.bytes, [&](const df::cspan bytes)
		{
			for (const auto& request : webp_sweep::requests)
			{
				const auto outcome = webp_sweep::run([](const df::cspan b, const bool yuv, const sizei t) { return load_webp(b, yuv, t); },
				                                     bytes, request);
				++decodes;
				if (!outcome.rows.empty()) ++decoded;
				if (!e.recorded) continue;

				const auto text = webp_sweep::describe(outcome);
				const uint32_t fold[] = {checksum, df::crc32c(text.data(), text.size())};
				checksum = df::crc32c(fold, sizeof(fold));
				++recorded_outcomes;
			}
		});
	}

	df::log(__FUNCTION__, std::format("{} files ({} real), {} decodes, {} decoded; record {:08x} over {} outcomes",
	                                  corpus.size(), real_files, decodes, decoded, checksum, recorded_outcomes));

	assert_equal(true, real_files >= 1, "the fixtures hold a WebP");
	assert_equal(true, decoded > 1000, "much of what is decoded decodes");
	assert_equal(20331, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0xa210cde4u), std::format("{:08x}", checksum),
	             "every outcome is the one load_webp had before it moved to libfiles");
}

// Diffractor's HEIF loader moved to libfiles. Before the switch both decoded every HEIF and AVIF in the
// fixtures and in DIFFRACTOR_SCAN_PARITY_CORPUS - libheif's examples and fuzzing corpus among them - and
// 34 generated ones: opaque and alpha, 1 by 1 to 33 by 17, turned and mirrored by their items, with and
// without an EXIF orientation, with thumbnail items turned or not, and colour with a premultiplied alpha
// item. Whole, cut short and with single bytes corrupted, they agreed on whether the load threw, on a
// null surface, and on its size, format, orientation and every byte of every row; on the size the
// decode budget judged and whether it refused it; on the thumbnail item a scan decodes, and whether it
// is upright; and on the decoder melnik.heic is steered to: 9,079 comparisons over 144 files, with no
// difference. That comparison left a record of load_heif's outcomes for the fixtures and the generated
// files, which libheif's uncompressed codec writes alike on Windows x64 and x86 and on Linux, where the
// record is the same. The sweep now holds libfiles to it.
namespace heif_sweep
{
	struct surface_outcome
	{
		bool threw = false;
		bool null = false;
		sizei dimensions;
		int format = -1;
		int orientation = -1;
		std::vector<uint8_t> rows;

		bool operator==(const surface_outcome&) const = default;
	};

	void capture(const ui::const_surface_ptr& surface, surface_outcome& o)
	{
		o.null = !surface;
		if (!surface) return;

		o.dimensions = surface->dimensions();
		o.format = static_cast<int>(surface->format());
		o.orientation = static_cast<int>(surface->orientation());
		if (surface->empty()) return;

		const auto row_bytes = static_cast<size_t>(o.dimensions.cx) * 4;
		for (auto y = 0; y < o.dimensions.cy; ++y)
		{
			const auto* const row = surface->pixels_line(y);
			o.rows.insert(o.rows.end(), row, row + row_bytes);
		}
	}

	struct outcome
	{
		surface_outcome image;
		sizei source_dimensions;
		bool over_budget = false;
		// What a scan that wants a thumbnail makes of the thumbnail item, and whether it is upright.
		surface_outcome thumbnail;
		bool thumbnail_orientation_applied = false;

		bool operator==(const outcome&) const = default;
	};

	template <typename Load, typename Scan>
	outcome run(Load&& load, Scan&& scan, const df::cspan bytes)
	{
		outcome o;

		try
		{
			mem_read_stream stream(bytes);
			load_diagnostic diagnostic;
			capture(load(stream, &diagnostic), o.image);
			o.source_dimensions = diagnostic.source_dimensions;
			o.over_budget = diagnostic.over_budget;
		}
		catch (const std::exception&)
		{
			o.image.threw = true;
		}

		try
		{
			files ff;
			mem_read_stream stream(bytes);
			const auto scanned = scan(stream, scan_intent::index, true, &ff);
			capture(scanned.thumbnail_surface, o.thumbnail);
			// The flag says something only of a thumbnail that was decoded.
			o.thumbnail_orientation_applied = scanned.thumbnail_surface && scanned.thumbnail_orientation_applied;
		}
		catch (const std::exception&)
		{
			o.thumbnail.threw = true;
		}

		return o;
	}

	std::string describe(const surface_outcome& o)
	{
		return std::format("threw {} null {} {}x{} format {} orientation {} {} bytes {:08x}", o.threw, o.null,
		                   o.dimensions.cx, o.dimensions.cy, o.format, o.orientation, o.rows.size(),
		                   df::crc32c(o.rows.data(), o.rows.size()));
	}

	std::string describe(const outcome& o)
	{
		return std::format("image {} source {}x{} over budget {} thumbnail {} upright {}", describe(o.image),
		                   o.source_dimensions.cx, o.source_dimensions.cy, o.over_budget, describe(o.thumbnail),
		                   o.thumbnail_orientation_applied);
	}

	heif_error write_to(heif_context*, const void* data, const size_t size, void* userdata)
	{
		auto* const out = static_cast<df::blob*>(userdata);
		out->insert(out->end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
		return {heif_error_Ok, heif_suberror_Unspecified, ""};
	}

	df::blob written(heif_context* const ctx)
	{
		df::blob file;
		heif_writer writer{};
		writer.writer_api_version = 1;
		writer.write = write_to;
		heif_context_write(ctx, &writer, &file);
		return file;
	}

	struct spec
	{
		int width = 7;
		int height = 5;
		bool alpha = false;
		heif_orientation orientation = heif_orientation_normal;
		uint16_t exif_orientation = 0;
		bool thumbnail = false;
		heif_orientation thumbnail_orientation = heif_orientation_normal;
	};

	heif_image* make_image(const int width, const int height, const bool alpha, uint32_t& seed)
	{
		heif_image* image = nullptr;
		heif_image_create(width, height, heif_colorspace_RGB, alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
		                  &image);
		heif_image_add_plane(image, heif_channel_interleaved, width, height, 8);
		int stride = 0;
		auto* const plane = heif_image_get_plane(image, heif_channel_interleaved, &stride);
		const auto channels = alpha ? 4 : 3;

		for (auto y = 0; y < height; ++y)
		{
			for (auto x = 0; x < width * channels; ++x)
			{
				seed = seed * 1664525u + 1013904223u;
				plane[static_cast<size_t>(y) * stride + x] = static_cast<uint8_t>(seed >> 24);
			}
		}

		return image;
	}

	// A little-endian TIFF stream with one entry, the orientation.
	df::blob orientation_exif(const uint16_t value)
	{
		return {'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0,
		        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), 0, 0, 0, 0, 0, 0};
	}

	// An image as libheif writes it through its uncompressed codec: its own rotation or mirror, an EXIF
	// orientation, and a thumbnail item with a turn of its own, as asked.
	df::blob make_heif(const spec& s, uint32_t& seed)
	{
		auto* const image = make_image(s.width, s.height, s.alpha, seed);
		auto* const ctx = heif_context_alloc();
		heif_encoder* encoder = nullptr;
		heif_context_get_encoder_for_format(ctx, heif_compression_uncompressed, &encoder);
		auto* const options = heif_encoding_options_alloc();
		options->image_orientation = s.orientation;

		heif_image_handle* handle = nullptr;
		heif_context_encode_image(ctx, image, encoder, options, &handle);

		if (s.exif_orientation)
		{
			const auto exif = orientation_exif(s.exif_orientation);
			heif_context_add_exif_metadata(ctx, handle, exif.data(), static_cast<int>(exif.size()));
		}

		if (s.thumbnail)
		{
			options->image_orientation = s.thumbnail_orientation;
			heif_image_handle* thumbnail = nullptr;
			heif_context_encode_thumbnail(ctx, image, handle, encoder, options, std::max(s.width, s.height) / 2, &thumbnail);
			heif_image_handle_release(thumbnail);
		}

		auto file = written(ctx);
		heif_image_handle_release(handle);
		heif_encoding_options_free(options);
		heif_encoder_release(encoder);
		heif_context_free(ctx);
		heif_image_release(image);
		return file;
	}

	// Colour with an alpha item of its own that declares it premultiplied, built as "Should decode
	// associated alpha HEIF" builds it.
	df::blob make_associated_alpha(const int width, const int height, uint32_t& seed)
	{
		auto* const ctx = heif_context_alloc();
		heif_encoder* encoder = nullptr;
		heif_context_get_encoder_for_format(ctx, heif_compression_uncompressed, &encoder);

		auto* const image = make_image(width, height, false, seed);
		heif_image_set_premultiplied_alpha(image, true);
		heif_image_handle* handle = nullptr;
		heif_context_encode_image(ctx, image, encoder, nullptr, &handle);

		heif_image* alpha = nullptr;
		heif_image_create(width, height, heif_colorspace_monochrome, heif_chroma_monochrome, &alpha);
		heif_image_add_plane(alpha, heif_channel_Y, width, height, 8);
		int stride = 0;
		auto* const plane = heif_image_get_plane(alpha, heif_channel_Y, &stride);

		for (auto y = 0; y < height; ++y)
		{
			for (auto x = 0; x < width; ++x)
			{
				seed = seed * 1664525u + 1013904223u;
				const auto pick = seed >> 24;
				plane[static_cast<size_t>(y) * stride + x] = static_cast<uint8_t>(pick < 32 ? 0 : pick < 64 ? 255 : pick);
			}
		}

		heif_image_handle* alpha_handle = nullptr;
		heif_context_encode_image(ctx, alpha, encoder, nullptr, &alpha_handle);
		const auto image_id = heif_image_handle_get_item_id(handle);
		const auto alpha_id = heif_image_handle_get_item_id(alpha_handle);
		constexpr char alpha_type[] = "\0\0\0\0urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
		heif_item_add_raw_property(ctx, alpha_id, heif_fourcc('a', 'u', 'x', 'C'), nullptr,
		                           reinterpret_cast<const uint8_t*>(alpha_type), sizeof(alpha_type), true, nullptr);
		heif_context_add_item_reference(ctx, heif_fourcc('a', 'u', 'x', 'l'), alpha_id, image_id);
		heif_context_add_item_reference(ctx, heif_fourcc('p', 'r', 'e', 'm'), image_id, alpha_id);

		auto file = written(ctx);
		heif_image_handle_release(alpha_handle);
		heif_image_release(alpha);
		heif_image_handle_release(handle);
		heif_image_release(image);
		heif_encoder_release(encoder);
		heif_context_free(ctx);
		return file;
	}

	std::vector<std::pair<std::string, df::blob>> synthetic(uint32_t& seed)
	{
		std::vector<std::pair<std::string, df::blob>> result;
		static constexpr sizei sizes[] = {{7, 5}, {16, 8}, {1, 1}, {33, 17}};
		static constexpr heif_orientation turns[] = {
			heif_orientation_normal, heif_orientation_rotate_90_cw, heif_orientation_flip_horizontally,
			heif_orientation_rotate_270_cw
		};
		auto n = 0;

		for (const auto& size : sizes)
		{
			for (const auto alpha : {false, true})
			{
				for (const auto turn : turns)
				{
					spec s;
					s.width = size.cx;
					s.height = size.cy;
					s.alpha = alpha;
					s.orientation = turn;
					s.exif_orientation = n % 3 == 0 ? uint16_t{0} : static_cast<uint16_t>(1 + n % 8);
					s.thumbnail = size.cx > 1 && n % 2 == 0;
					s.thumbnail_orientation = n % 4 == 0 ? heif_orientation_rotate_180 : heif_orientation_normal;
					result.emplace_back(std::format("{}x{} alpha {} turn {} #{}", size.cx, size.cy, alpha,
					                                static_cast<int>(turn), n), make_heif(s, seed));
					++n;
				}
			}
		}

		result.emplace_back("associated alpha 2x1", make_associated_alpha(2, 1, seed));
		result.emplace_back("associated alpha 13x7", make_associated_alpha(13, 7, seed));
		return result;
	}

	struct entry
	{
		std::string name;
		df::blob bytes;
		bool recorded = false;
	};

	// The fixtures' HEIF and AVIF, recorded, and DIFFRACTOR_SCAN_PARITY_CORPUS's, decoded but not recorded.
	std::vector<entry> corpus()
	{
		std::vector<df::file_path> found;
		scan_parity::add_folder(test_files_folder, found);
		scan_parity::add_folder(test_formats_folder, found);
		const auto fixtures = found.size();

		const auto extra = scan_parity::environment_variable("DIFFRACTOR_SCAN_PARITY_CORPUS");
		for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
		{
			scan_parity::add_folder(df::folder_path(part), found);
		}

		std::vector<entry> result;

		for (size_t i = 0; i < found.size(); ++i)
		{
			df::blob bytes;

			try
			{
				bytes = df::blob_from_file(found[i]);
			}
			catch (const std::exception&)
			{
				// A corpus folder may hold a file too large to read whole - a video - which is no HEIF.
				if (i < fixtures) throw;
				continue;
			}

			if (bytes.size() >= 16 && files::detect_format(bytes) == detected_format::HEIF)
				result.push_back({std::string(found[i].name()), std::move(bytes), i < fixtures});
		}

		return result;
	}

	// A file whole, cut short and with single bytes corrupted, as the other sweeps vary them.
	template <typename Visit>
	void for_each_variant(df::blob& bytes, Visit&& visit)
	{
		visit(df::cspan(bytes));
		for (const auto cut : scan_parity::cut_points(bytes.size())) visit(df::cspan{bytes.data(), cut});
		if (bytes.size() <= 16) return;

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));
			if (bytes[at] != original) visit(df::cspan(bytes));
			bytes[at] = original;
		}
	}
}

static void should_decode_heifs_as_load_heif_did()
{
	auto corpus = heif_sweep::corpus();
	const auto real_files = corpus.size();

	uint32_t seed = 2026;
	for (auto& [name, bytes] : heif_sweep::synthetic(seed)) corpus.push_back({std::move(name), std::move(bytes), true});

	auto decodes = 0;
	auto decoded = 0;
	auto thumbnails = 0;
	auto recorded_outcomes = 0;
	uint32_t checksum = 0;

	for (auto& e : corpus)
	{
		heif_sweep::for_each_variant(e.bytes, [&](const df::cspan bytes)
		{
			const auto outcome = heif_sweep::run(load_heif, scan_heif_libfiles, bytes);
			++decodes;
			if (!outcome.image.rows.empty()) ++decoded;
			if (!outcome.thumbnail.rows.empty()) ++thumbnails;
			if (!e.recorded) return;

			const auto text = heif_sweep::describe(outcome);
			const uint32_t fold[] = {checksum, df::crc32c(text.data(), text.size())};
			checksum = df::crc32c(fold, sizeof(fold));
			++recorded_outcomes;
		});
	}

	df::log(__FUNCTION__, std::format("{} files ({} real), {} decodes, {} decoded, {} thumbnails; record {:08x} over {} outcomes",
	                                  corpus.size(), real_files, decodes, decoded, thumbnails, checksum, recorded_outcomes));

	assert_equal(true, real_files >= 2, "the fixtures hold HEIF and AVIF");
	assert_equal(true, decoded > 1000, "much of what is decoded decodes");
	assert_equal(2347, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0xda7bc3f5u), std::format("{:08x}", checksum),
	             "every outcome is the one load_heif had before it moved to libfiles");
}

// Diffractor's JPEG XL loader moved to libfiles. Before the switch both decoded every JPEG XL in the
// fixtures and in DIFFRACTOR_SCAN_PARITY_CORPUS - libjxl's conformance files among them - the 22
// lossless files test-generated/jxl keeps, and 26 written afresh: lossless and lossy, opaque, alpha and
// premultiplied alpha, grey, 16-bit and linear, 1 by 1 to 96 by 64, turned by their codestreams,
// animated, and in a container whose Exif box gives another orientation. Whole, cut short and with
// single bytes corrupted, they agreed on whether the load threw, on a null surface, on its size,
// format, orientation and every byte of every row, and on the size the decode budget judged and whether
// it refused it: 4,205 comparisons over 74 files, with no difference. That comparison left
// a record of load_jxl's outcomes for the fixture and test-generated/jxl, alike on Windows
// x64 and x86 and on Linux. Those files are kept as libjxl's effort-1 encoder wrote them, as it writes
// other bytes on other platforms; and the fixture's rows are left out, as it is lossy, and libjxl
// decodes a lossy image in floating point, on whatever SIMD Highway picks for the processor. The sweep
// now holds libfiles to the record.
namespace jxl_sweep
{
	struct outcome
	{
		bool threw = false;
		bool null = false;
		sizei dimensions;
		int format = -1;
		int orientation = -1;
		std::vector<uint8_t> rows;
		sizei source_dimensions;
		bool over_budget = false;
	};

	outcome run(const df::cspan bytes)
	{
		outcome o;

		try
		{
			mem_read_stream stream(bytes);
			load_diagnostic diagnostic;
			const auto surface = load_jxl(stream, &diagnostic);
			o.source_dimensions = diagnostic.source_dimensions;
			o.over_budget = diagnostic.over_budget;
			o.null = !surface;

			if (surface)
			{
				o.dimensions = surface->dimensions();
				o.format = static_cast<int>(surface->format());
				o.orientation = static_cast<int>(surface->orientation());
				const auto row_bytes = static_cast<size_t>(o.dimensions.cx) * 4;

				for (auto y = 0; y < o.dimensions.cy; ++y)
				{
					const auto* const row = surface->pixels_line(y);
					o.rows.insert(o.rows.end(), row, row + row_bytes);
				}
			}
		}
		catch (const std::exception&)
		{
			o.threw = true;
		}

		return o;
	}

	std::string describe(const outcome& o, const bool with_rows)
	{
		return std::format("threw {} null {} {}x{} format {} orientation {} {} bytes {:08x} source {}x{} over budget {}",
		                   o.threw, o.null, o.dimensions.cx, o.dimensions.cy, o.format, o.orientation, o.rows.size(),
		                   with_rows ? df::crc32c(o.rows.data(), o.rows.size()) : 0u, o.source_dimensions.cx,
		                   o.source_dimensions.cy, o.over_budget);
	}

	struct entry
	{
		std::string name;
		df::blob bytes;
		// Lossless, so its rows are the same on every machine.
		bool exact = false;
	};

	// The fixtures' JPEG XL, whose rows are left out - lossy - and the lossless files test-generated/jxl
	// keeps, whose rows are not.
	std::vector<entry> corpus()
	{
		std::vector<df::file_path> found;
		scan_parity::add_folder(test_files_folder, found);
		scan_parity::add_folder(test_formats_folder, found);
		const auto fixtures = found.size();
		scan_parity::add_folder(test_generated_folder.combine("jxl"), found);

		std::vector<entry> result;

		for (size_t i = 0; i < found.size(); ++i)
		{
			auto bytes = df::blob_from_file(found[i]);
			if (bytes.size() >= 16 && files::detect_format(bytes) == detected_format::JXL)
				result.push_back({std::string(found[i].name()), std::move(bytes), i >= fixtures});
		}

		return result;
	}

	// A file whole, cut short and with single bytes corrupted, as the other sweeps vary them.
	template <typename Visit>
	void for_each_variant(df::blob& bytes, Visit&& visit)
	{
		visit(df::cspan(bytes));
		for (const auto cut : scan_parity::cut_points(bytes.size())) visit(df::cspan{bytes.data(), cut});
		if (bytes.size() <= 16) return;

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));
			if (bytes[at] != original) visit(df::cspan(bytes));
			bytes[at] = original;
		}
	}
}

static void should_decode_jxls_as_load_jxl_did()
{
	auto files = 0;
	auto decoded = 0;
	auto recorded_outcomes = 0;
	uint32_t checksum = 0;

	for (auto& e : jxl_sweep::corpus())
	{
		++files;

		jxl_sweep::for_each_variant(e.bytes, [&](const df::cspan bytes)
		{
			const auto outcome = jxl_sweep::run(bytes);
			if (!outcome.rows.empty()) ++decoded;

			const auto text = jxl_sweep::describe(outcome, e.exact);
			const uint32_t fold[] = {checksum, df::crc32c(text.data(), text.size())};
			checksum = df::crc32c(fold, sizeof(fold));
			++recorded_outcomes;
		});
	}

	df::log(__FUNCTION__, std::format("{} files, {} decoded; record {:08x} over {} outcomes", files, decoded, checksum,
	                                  recorded_outcomes));

	assert_equal(23, files, "the fixture and the generated files are all there");
	assert_equal(true, decoded > 400, "much of what is decoded decodes");
	assert_equal(1526, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0x8b7c5828u), std::format("{:08x}", checksum),
	             "every outcome is the one load_jxl had before it moved to libfiles");
}

// Diffractor's raw loader moved to libfiles. Before the switch both loaded every raw file in the fixtures
// and in DIFFRACTOR_SCAN_PARITY_CORPUS - the DNG SDK's samples, JPEG XL, floating-point and gain map DNGs
// among them - whole, cut short and with single bytes corrupted, with and without the preview, and under
// a budget that refuses all but the smallest development. They agreed on whether the load threw, on its
// success, whether it is the preview and why it failed, on the size the budget refused, on whether it is
// a JPEG or empty, on the image kept encoded - its format, size, orientation and bytes - and on the
// surface - its format, size, orientation and every byte of every row: 3,124 comparisons over 16 files
// on each of Windows x64 and x86 and Linux, with no difference. That comparison left a record of
// load_raw's outcomes for the fixtures' two CR2 files, alike on all three, which the sweep now holds
// libfiles to.
namespace raw_sweep
{
	struct outcome
	{
		bool threw = false;
		bool success = false;
		bool is_preview = false;
		int reason = -1;
		sizei source_dimensions;
		bool is_jpeg = false;
		bool is_empty = true;
		// 0 none, 1 there but not valid, 2 valid.
		int image = 0;
		int image_format = -1;
		sizei image_dimensions;
		int image_orientation = -1;
		size_t image_bytes = 0;
		uint32_t image_crc = 0;
		int surface = 0;
		sizei surface_dimensions;
		int surface_format = -1;
		int surface_orientation = -1;
		uint32_t rows_crc = 0;
	};

	outcome run(const df::file_path path, const bool preview)
	{
		outcome o;

		try
		{
			const auto r = load_raw(path, preview);
			o.success = r.success;
			o.is_preview = r.is_preview;
			o.reason = static_cast<int>(r.reason);
			o.source_dimensions = r.source_dimensions;
			o.is_jpeg = r.is_jpeg();
			o.is_empty = r.is_empty();

			if (r.i)
			{
				o.image = is_valid(r.i) ? 2 : 1;
				o.image_format = static_cast<int>(r.i->format());
				o.image_dimensions = r.i->dimensions();
				o.image_orientation = static_cast<int>(r.i->orientation());
				o.image_bytes = r.i->data().size();
				o.image_crc = df::crc32c(r.i->data().data(), r.i->data().size());
			}

			if (r.s)
			{
				o.surface = is_valid(r.s) ? 2 : 1;
				o.surface_dimensions = r.s->dimensions();
				o.surface_format = static_cast<int>(r.s->format());
				o.surface_orientation = static_cast<int>(r.s->orientation());

				if (o.surface == 2)
				{
					const auto row_bytes = static_cast<size_t>(o.surface_dimensions.cx) * 4;
					for (auto y = 0; y < o.surface_dimensions.cy; ++y)
						o.rows_crc = df::crc32c(o.rows_crc, r.s->pixels_line(y), row_bytes);
				}
			}
		}
		catch (const std::exception&)
		{
			o.threw = true;
		}

		return o;
	}

	std::string describe(const outcome& o)
	{
		return std::format(
			"threw {} success {} preview {} reason {} source {}x{} jpeg {} empty {} image {} format {} {}x{} "
			"orientation {} {} bytes {:08x} surface {} format {} {}x{} orientation {} rows {:08x}",
			o.threw, o.success, o.is_preview, o.reason, o.source_dimensions.cx, o.source_dimensions.cy, o.is_jpeg,
			o.is_empty, o.image, o.image_format, o.image_dimensions.cx, o.image_dimensions.cy, o.image_orientation,
			o.image_bytes, o.image_crc, o.surface, o.surface_format, o.surface_dimensions.cx,
			o.surface_dimensions.cy, o.surface_orientation, o.rows_crc);
	}

	struct entry
	{
		df::file_path path;
		df::blob bytes;
	};

	// The fixtures' raw files.
	std::vector<entry> corpus()
	{
		std::vector<df::file_path> found;
		scan_parity::add_folder(test_files_folder, found);
		scan_parity::add_folder(test_formats_folder, found);

		std::vector<entry> result;

		for (const auto& path : found)
		{
			if (files::file_type_from_name(path)->has_trait(file_traits::raw))
				result.push_back({path, df::blob_from_file(path)});
		}

		return result;
	}

	// A file whole, cut short and with single bytes corrupted, as the other sweeps vary them, written to a
	// file of its own extension for LibRaw to open. A development costs most of a second, so a variant is
	// marked to be developed whole only where it says most: the file whole, cut short at its middle and by
	// its last byte, and every eighth corruption. A CR2 cut short by a few bytes more, inside its image
	// data, develops on Windows from what LibRaw makes of the bytes past the end, and fails to on Linux,
	// whichever loader opens it.
	template <typename Visit>
	void for_each_variant(entry& e, Visit&& visit)
	{
		const auto path = _temps.next_path(e.path.extension());
		auto& bytes = e.bytes;

		const auto write = [&](const df::cspan variant, const bool develop)
		{
			if (!df::blob_save_to_file(variant, path)) throw std::runtime_error("the variant is not written");
			visit(path, develop);
		};

		write(df::cspan(bytes), true);

		for (const auto cut : scan_parity::cut_points(bytes.size()))
		{
			write(df::cspan{bytes.data(), cut}, cut == bytes.size() / 2 || cut == bytes.size() - 1);
		}

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));
			if (bytes[at] != original) write(df::cspan(bytes), k % 8 == 0);
			bytes[at] = original;
		}
	}

	struct mode
	{
		bool preview = false;
		// A budget of 64 KB refuses all but the smallest development, as the header declares its size.
		bool tiny_budget = false;
	};

	// The preview taken, or the development refused for the budget, for every variant; under the budget
	// that applies, where asked.
	template <typename Run>
	void for_each_mode(const bool develop, Run&& run)
	{
		for (const auto m : {mode{true, true}, mode{false, true}, mode{true, false}, mode{false, false}})
		{
			if (!m.tiny_budget && !develop) continue;

			const auto restore_budget = df::max_decode_bytes;
			const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });
			if (m.tiny_budget) df::max_decode_bytes = 64 * 1024;
			run(m);
		}
	}
}

static void should_load_raws_as_load_raw_did()
{
	auto files = 0;
	auto loaded = 0;
	auto too_large = 0;
	auto recorded_outcomes = 0;
	uint32_t checksum = 0;

	for (auto& e : raw_sweep::corpus())
	{
		++files;

		raw_sweep::for_each_variant(e, [&](const df::file_path path, const bool develop)
		{
			raw_sweep::for_each_mode(develop, [&](const raw_sweep::mode m)
			{
				const auto outcome = raw_sweep::run(path, m.preview);
				if (outcome.success) ++loaded;
				if (outcome.reason == static_cast<int>(file_load_result::failure::too_large)) ++too_large;

				const auto text = std::format("{} {} {}", m.preview, m.tiny_budget, raw_sweep::describe(outcome));
				const uint32_t fold[] = {checksum, df::crc32c(text.data(), text.size())};
				checksum = df::crc32c(fold, sizeof(fold));
				++recorded_outcomes;
			});
		});
	}

	df::log(__FUNCTION__, std::format("{} files, {} loaded, {} too large; record {:08x} over {} outcomes", files, loaded,
	                                  too_large, checksum, recorded_outcomes));

	assert_equal(2, files, "the fixtures' raw files are all there");
	assert_equal(true, loaded > 100 && too_large > 100, "much of what is loaded loads, and much is refused for its size");
	assert_equal(352, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0x176e6d77u), std::format("{:08x}", checksum),
	             "every outcome is the one load_raw had before it moved to libfiles");
}

// A raw file turned by its camera keeps its preview unturned, as the sensor saw it, and the preview takes
// the raw data's turn; the fixtures are all upright, so one is given a quarter turn. Its first Orientation
// entry - a short, one of them, little-endian - is IFD0's, which LibRaw takes the flip from.
static void should_turn_a_raw_preview_as_its_raw_data()
{
	auto bytes = df::blob_from_file(test_files_folder.combine_file("Gherkin.CR2"));
	constexpr uint8_t orientation_entry[] = {0x12, 0x01, 3, 0, 1, 0, 0, 0};
	const auto found = std::ranges::search(bytes, orientation_entry);
	assert_equal(true, !found.empty() && found.begin() + 10 <= bytes.end(), "the fixture states an orientation");

	const auto value = found.begin() + 8;
	value[0] = static_cast<uint8_t>(ui::orientation::right_top);
	value[1] = 0;

	const auto path = _temps.next_path(".CR2");
	assert_equal(true, df::blob_save_to_file(bytes, path), "the turned fixture is written");

	const auto preview = load_raw(path, true);
	assert_equal(true, preview.success && preview.is_preview && is_valid(preview.i), "the preview loads");
	assert_equal(true, preview.i->width() > preview.i->height(), "the preview is stored unturned");
	assert_equal(static_cast<int>(ui::orientation::right_top), static_cast<int>(preview.orientation()),
	             "the preview is turned as the raw data is");

	const auto developed = load_raw(path, false);
	assert_equal(true, developed.success && !developed.is_preview && is_valid(developed.s), "the raw data develops");
	assert_equal(static_cast<int>(ui::orientation::right_top), static_cast<int>(developed.orientation()),
	             "the development is turned as the raw data is");

	// A scan's thumbnail is the same preview, decoded smaller, and turned the same way.
	files ff;
	const auto scanned = ff.scan_file(path, true, files::file_type_from_name(path), {}, {320, 320},
	                                  scan_intent::index, false);
	assert_equal(true, is_valid(scanned.thumbnail_surface), "the scan's thumbnail is decoded");
	assert_equal(true, scanned.thumbnail_surface->dimensions().cx > scanned.thumbnail_surface->dimensions().cy,
	             "the scan's thumbnail is stored unturned");
	assert_equal(static_cast<int>(ui::orientation::right_top),
	             static_cast<int>(scanned.thumbnail_surface->orientation()),
	             "the scan's thumbnail is turned as the raw data is");
	assert_equal(static_cast<int>(ui::orientation::right_top), static_cast<int>(scanned.orientation),
	             "the scan gives the raw data's turn");
}

// Diffractor's raw scan moved to libfiles. Before the switch both scanned every raw file in the fixtures
// and in DIFFRACTOR_SCAN_PARITY_CORPUS - the DNG SDK's samples among them - whole, cut short and with
// single bytes corrupted, for an index and for an inspector, with a thumbnail and without, at two sizes.
// They agreed on every field a raw scan sets, on the listing row by row, and on the thumbnail's size,
// format, orientation and every row: 5,076 comparisons over 16 files on each of Windows x64 and
// x86 and Linux, with no difference. That comparison left a record of scan_raw's outcomes for the
// fixtures' two CR2 files, alike on all three, which the sweep now holds libfiles to.
namespace raw_scan_sweep
{
	struct mode
	{
		bool inspect = false;
		bool thumbnail = false;
		sizei max;
	};

	constexpr mode modes[] = {
		{false, false, {}}, {true, false, {}}, {false, true, {320, 320}}, {true, true, {}},
	};

	// Everything a raw scan sets, and a few things it must not. LibRaw's timestamp is mktime() of the
	// camera's clock, which counts from 1970 in the zone the scan runs in, so a record leaves the listing's
	// Timestamp out; the date taken from it is the camera's clock again, in any zone.
	std::string describe(const file_scan_result& r)
	{
		std::string listing;
		for (const auto& row : r.libraw_metadata)
			listing += std::format("{}\t{}\t{}\t{}\t{}\n", row.key, row.key != "Timestamp" ? row.value : "", row.id,
			                       row.depth, row.container);

		std::string thumbnail = "none";

		if (r.thumbnail_surface)
		{
			const auto& s = *r.thumbnail_surface;
			uint32_t rows = 0;

			if (is_valid(r.thumbnail_surface))
			{
				const auto row_bytes = static_cast<size_t>(s.dimensions().cx) * 4;
				for (auto y = 0; y < s.dimensions().cy; ++y) rows = df::crc32c(rows, s.pixels_line(y), row_bytes);
			}

			thumbnail = std::format("{} {}x{} format {} orientation {} rows {:08x}", is_valid(r.thumbnail_surface),
			                        s.dimensions().cx, s.dimensions().cy, static_cast<int>(s.format()),
			                        static_cast<int>(s.orientation()), rows);
		}

		return std::format(
			"success {} format {} {}x{} orientation {} pixels '{}' make '{}' model '{}' comment '{}' artist '{}' "
			"iso {} exposure {} f {} focal {} created {} source {} gps {} {} xmp {} {:08x} exif {} iptc {} "
			"image {} listing {} {:08x} thumbnail {}",
			r.success, static_cast<int>(r.format), r.width, r.height, static_cast<int>(r.orientation),
			r.pixel_format.sv(), r.camera_manufacturer.sv(), r.camera_model.sv(), r.comment.sv(), r.artist.sv(),
			r.iso_speed, r.exposure_time, r.f_number, r.focal_length, r.created_local._i,
			static_cast<int>(r.created_local_source), r.gps._latitude, r.gps._longitude, r.metadata.xmp.size(),
			df::crc32c(r.metadata.xmp.data(), r.metadata.xmp.size()), r.metadata.exif.size(), r.metadata.iptc.size(),
			r.thumbnail_image != nullptr, r.libraw_metadata.size(), df::crc32c(listing.data(), listing.size()),
			thumbnail);
	}

	std::string run(const df::file_path path, const mode m)
	{
		try
		{
			files ff;
			const auto ft = files::file_type_from_name(path);
			const auto r = ff.scan_file(path, m.thumbnail, ft, {}, m.max, m.inspect ? scan_intent::inspect : scan_intent::index, false);
			return describe(r);
		}
		catch (const std::exception&)
		{
			return "threw";
		}
	}

	struct entry
	{
		df::file_path path;
		df::blob bytes;
	};

	// The fixtures' raw files.
	std::vector<entry> corpus()
	{
		std::vector<df::file_path> found;
		scan_parity::add_folder(test_files_folder, found);
		scan_parity::add_folder(test_formats_folder, found);

		std::vector<entry> result;

		for (const auto& path : found)
		{
			if (files::file_type_from_name(path)->has_trait(file_traits::raw))
				result.push_back({path, df::blob_from_file(path)});
		}

		return result;
	}

	// A file whole, cut short and with single bytes corrupted, as the other sweeps vary them, written to a
	// file of its own extension for LibRaw to open.
	template <typename Visit>
	void for_each_variant(entry& e, Visit&& visit)
	{
		const auto path = _temps.next_path(e.path.extension());
		auto& bytes = e.bytes;

		const auto write = [&](const df::cspan variant)
		{
			if (!df::blob_save_to_file(variant, path)) throw std::runtime_error("the variant is not written");
			visit(path, variant.size);
		};

		write(df::cspan(bytes));
		for (const auto cut : scan_parity::cut_points(bytes.size())) write(df::cspan{bytes.data(), cut});

		for (uint64_t k = 0; k < 48; ++k)
		{
			const auto at = scan_parity::corruption_position(k, bytes.size());
			const auto original = bytes[at];
			bytes[at] = static_cast<uint8_t>(k % 2 ? original ^ 0xFF : (k % 3 ? 0x00 : 0xFF));
			if (bytes[at] != original) write(df::cspan(bytes));
			bytes[at] = original;
		}
	}
}

static void should_scan_raws_as_scan_raw_did()
{
	auto files = 0;
	auto scanned = 0;
	auto recorded_outcomes = 0;
	uint32_t checksum = 0;

	for (auto& e : raw_scan_sweep::corpus())
	{
		++files;

		raw_scan_sweep::for_each_variant(e, [&](const df::file_path path, size_t)
		{
			for (const auto& m : raw_scan_sweep::modes)
			{
				const auto text = raw_scan_sweep::run(path, m);
				if (text.starts_with("success true")) ++scanned;

				const uint32_t fold[] = {checksum, df::crc32c(text.data(), text.size())};
				checksum = df::crc32c(fold, sizeof(fold));
				++recorded_outcomes;
			}
		});
	}

	df::log(__FUNCTION__, std::format("{} files, {} scanned; record {:08x} over {} outcomes", files, scanned, checksum,
	                                  recorded_outcomes));

	assert_equal(2, files, "the fixtures' raw files are all there");
	assert_equal(true, scanned > 100, "much of what is scanned scans");
	assert_equal(640, recorded_outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0x5296f41bu), std::format("{:08x}", checksum),
	             "every outcome is the one scan_raw had before it moved to libfiles");
}

// Diffractor's JPEG, PNG and WebP encoders and its lossless JPEG turn moved to libfiles. Before the
// switch this sweep took a record of what they wrote, the same on Windows x64 and x86 and on Linux:
// 1,343 outcomes, 775 of them encoded files. Pictures from 1x1 to 301x199, packed with and
// without alpha and as NV12 and P010 planes, at each EXIF orientation; with each kind of metadata they
// carry, at and a byte over the limits a JPEG sets; at several qualities and WebP settings and against
// the tables of source JPEGs; through save_surface, files' long-lived encoder and the thumbnail store;
// and the fixture JPEGs and some of its own - one carrying the EXIF of Sony.JPG, whose thumbnail has
// an orientation - turned every way, cut short and with bytes corrupted, within the decode budget and
// a byte over it. A source JPEG that makes libjpeg fail is left out, for fear that an optimized
// Windows build dropped the catch that falls back from it, every call in its try being extern "C"
// under /EHsc. libfiles' own Release build did; Diffractor's, which optimize the whole program, never
// did, and should_save_over_a_source_jpeg_libjpeg_fails_on holds every build to the fallback.
//
// Then the old encoders and libfiles' were compared in process: 1,500 random pictures, metadata and
// settings through every encoder, the long-lived ones in step; every JPEG in the fixtures,
// C:\Windows\Web and the XMP SDK's samples turned, cut short and corrupted; and the fixtures' pictures
// with the metadata their files carry. They agreed on every byte and every refusal - 8,047 comparisons
// on Windows x64 and x86 and 6,303 on Linux - and the sweep now holds libfiles to the record.
namespace encode_sweep
{
	// xorshift32, so the pictures are the same on every platform.
	struct random
	{
		uint32_t state;

		uint32_t next()
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			return state;
		}
	};

	// Detail at every scale - a gradient, edges a block apart and noise - so each encoder has work in
	// every block. Every byte is written, an RGB pixel's fourth and a row's padding too, so an encoder
	// that read what it should ignore would show. Unless opaque, an ARGB picture is clear on its left
	// third, opaque on its right and graded between.
	ui::surface_ptr make_surface(const int cx, const int cy, const ui::texture_format format,
	                             const ui::orientation orientation = ui::orientation::top_left,
	                             const ui::color_space cs = ui::color_space::rec601_full, const bool opaque = false)
	{
		auto s = std::make_shared<ui::surface>();
		auto* const pixels = s->alloc(cx, cy, format, orientation);
		s->color_space(cs);

		const auto stride = s->stride();
		const auto width = static_cast<size_t>(cx);
		const auto height = static_cast<size_t>(cy);
		const auto planar = format == ui::texture_format::NV12 || format == ui::texture_format::P010;
		const auto size = stride * height * (planar ? 3u : 2u) / 2u;
		random rng{static_cast<uint32_t>(cx * 7919 + cy * 104729 + static_cast<int>(format) * 31) | 1u};

		for (size_t i = 0; i < size; ++i)
		{
			const auto x = i % stride;
			const auto y = i / stride;
			const auto edge = ((x / 8 + y / 8) & 1u) ? 64u : 0u;
			pixels[i] = static_cast<uint8_t>(x * 3 + y * 5 + edge + (rng.next() & 31u));
		}

		if (format == ui::texture_format::ARGB)
		{
			const auto third = width / 3;

			for (size_t y = 0; y < height; ++y)
			{
				for (size_t x = 0; x < width; ++x)
				{
					pixels[y * stride + x * 4 + 3] = static_cast<uint8_t>(
						opaque || x >= third * 2 ? 255u : x < third ? 0u : y * 16 + x);
				}
			}
		}

		return s;
	}

	std::string describe(const ui::const_image_ptr& image)
	{
		if (!image) return "null";
		const auto& data = image->data();
		const auto d = image->dimensions();
		return std::format("format {} {}x{} orientation {} {} bytes {:08x}", static_cast<int>(image->format()), d.cx,
		                   d.cy, static_cast<int>(image->orientation()), data.size(),
		                   df::crc32c(data.data(), data.size()));
	}

	std::string describe(const df::blob& data)
	{
		return std::format("{} bytes {:08x}", data.size(), df::crc32c(data.data(), data.size()));
	}

	// What an encoder made, or why it threw.
	template <typename F>
	std::string attempt(F&& encode)
	{
		try
		{
			return describe(encode());
		}
		catch (const std::exception& e)
		{
			return std::format("threw {}", e.what());
		}
	}

	// The payload of a JPEG's EXIF APP1 segment, its signature included.
	df::blob exif_segment(const df::cspan jpeg)
	{
		for (size_t i = 2; i + 4 <= jpeg.size && jpeg.data[i] == 0xFF;)
		{
			const auto marker = jpeg.data[i + 1];
			const auto length = (static_cast<size_t>(jpeg.data[i + 2]) << 8) | jpeg.data[i + 3];
			if (marker == 0xDA || length < 2 || i + 2 + length > jpeg.size) break;

			const df::cspan payload{jpeg.data + i + 4, length - 2};
			if (marker == 0xE1 && is_exif_signature(payload)) return df::blob(payload.begin(), payload.end());
			i += 2 + length;
		}

		return {};
	}

	// An ICC profile libpng takes for an RGB image - a header naming RGB data and the XYZ connection
	// space, and an empty tag table - size bytes long, patterned past the header unless zeros.
	df::blob icc_profile(const size_t size, const bool zeros = false)
	{
		df::blob icc(size, 0);
		for (size_t i = 132; !zeros && i < size; ++i) icc[i] = static_cast<uint8_t>(i * 31 + (i >> 9));

		icc[0] = static_cast<uint8_t>(size >> 24);
		icc[1] = static_cast<uint8_t>(size >> 16);
		icc[2] = static_cast<uint8_t>(size >> 8);
		icc[3] = static_cast<uint8_t>(size);
		icc[8] = 0x02;
		icc[9] = 0x10;
		std::memcpy(icc.data() + 12, "mntr", 4);
		std::memcpy(icc.data() + 16, "RGB ", 4);
		std::memcpy(icc.data() + 20, "XYZ ", 4);
		std::memcpy(icc.data() + 36, "acsp", 4);
		// The D50 illuminant, as s15Fixed16 numbers.
		constexpr uint8_t d50[] = {0x00, 0x00, 0xF6, 0xD6, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xD3, 0x2D};
		std::memcpy(icc.data() + 68, d50, sizeof(d50));
		return icc;
	}

	// 8BIM records with payloads of these sizes, named when odd.
	df::blob photoshop_resources(const std::vector<size_t>& payload_sizes)
	{
		std::vector<uint8_t> out;
		uint16_t id = 0x0BB8;

		for (const auto size : payload_sizes)
		{
			std::vector<uint8_t> payload(size);
			for (size_t i = 0; i < size; ++i) payload[i] = static_cast<uint8_t>(i * 7 + size);
			append_photoshop_resource(out, id++, size % 2 ? "odd" : "", {payload.data(), payload.size()});
		}

		return df::blob(out.begin(), out.end());
	}

	constexpr std::string_view xmp_packet =
		R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/">)"
		R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description )"
		R"(xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmp:Rating="4">)"
		R"(<dc:title><rdf:Alt><rdf:li xml:lang="x-default">Encoded by the sweep</rdf:li></rdf:Alt></dc:title>)"
		R"(</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>)";

	// The packet, padded with spaces to size bytes as XMP is.
	df::blob xmp(const size_t size = xmp_packet.size())
	{
		df::blob result(xmp_packet.begin(), xmp_packet.end());
		result.resize(std::max(size, xmp_packet.size()), ' ');
		return result;
	}

	df::blob padded(const df::blob& b, const size_t size)
	{
		auto result = b.clone();
		result.resize(size, 0x5A);
		return result;
	}

	struct metadata_case
	{
		std::string name;
		std::function<metadata_parts()> make;
	};

	struct webp_case
	{
		std::string name;
		file_encode_params params;
	};

	file_encode_params webp_params(const bool lossless, const int quality, const bool lossy_alpha, const bool fast)
	{
		file_encode_params params;
		params.webp_lossless = lossless;
		params.webp_quality = quality;
		params.webp_lossy_alpha = lossy_alpha;
		params.webp_fast = fast;
		return params;
	}

	file_encode_params jpeg_params(const int quality, const df::cspan source = {})
	{
		file_encode_params params;
		params.jpeg_save_quality = quality;
		params.jpeg_source = source;
		return params;
	}
}

static void should_encode_as_diffractors_encoders_did()
{
	using encode_sweep::attempt;
	using encode_sweep::make_surface;
	using encode_sweep::metadata_case;
	using ui::texture_format;

	std::string record;
	auto outcomes = 0;
	auto encoded = 0;

	const auto note = [&](const std::string& what, const std::string& outcome)
	{
		record += what + ": " + outcome + "\n";
		++outcomes;
		if (outcome.find(" bytes ") != std::string::npos && !outcome.starts_with("threw") && !outcome.starts_with("0 bytes"))
			++encoded;
	};

	const auto camera_exif = encode_sweep::exif_segment(df::blob_from_file(df::file_path(test_files_folder, "IMG_9340.JPG")));
	assert_equal(true, camera_exif.size() > 1000, "the camera fixture carries EXIF");

	const auto bare_exif = make_orientation_exif(ui::orientation::right_top);
	df::blob signed_exif(exif_signature.begin(), exif_signature.end());
	signed_exif.insert(signed_exif.end(), bare_exif.begin(), bare_exif.end());

	const auto everything = [&]
	{
		metadata_parts m;
		m.exif = camera_exif.clone();
		m.xmp = encode_sweep::xmp();
		m.iptc = make_iptc_caption("Encoded by the sweep");
		m.photoshop_resources = encode_sweep::photoshop_resources({16, 13});
		m.icc = encode_sweep::icc_profile(132);
		return m;
	};

	const auto exif_only = [](df::blob exif)
	{
		metadata_parts m;
		m.exif = std::move(exif);
		return m;
	};

	// The ICC segment a JPEG carries holds 65519 bytes of a profile, and a JPEG has 255 of them.
	constexpr size_t icc_segment = 65519;

	const std::vector<metadata_case> metadata = {
		{"none", [] { return metadata_parts{}; }},
		{"bare exif", [&] { return exif_only(bare_exif.clone()); }},
		{"signed exif", [&] { return exif_only(signed_exif.clone()); }},
		{"camera exif", [&] { return exif_only(camera_exif.clone()); }},
		{"camera exif unsigned", [&] { return exif_only(df::blob(camera_exif.begin() + 6, camera_exif.end())); }},
		{"xmp", [] { metadata_parts m; m.xmp = encode_sweep::xmp(); return m; }},
		{"iptc", [] { metadata_parts m; m.iptc = make_iptc_caption("Encoded by the sweep"); return m; }},
		{"photoshop", [] { metadata_parts m; m.photoshop_resources = encode_sweep::photoshop_resources({16, 13}); return m; }},
		{"iptc and photoshop over three markers", [] {
			metadata_parts m;
			m.iptc = make_iptc_caption("Encoded by the sweep");
			m.photoshop_resources = encode_sweep::photoshop_resources({30000, 30001, 30000});
			return m;
		}},
		{"photoshop cut short", [] {
			metadata_parts m;
			const auto r = encode_sweep::photoshop_resources({16, 13});
			m.photoshop_resources = df::blob(r.begin(), r.end() - 3);
			return m;
		}},
		{"icc", [] { metadata_parts m; m.icc = encode_sweep::icc_profile(132); return m; }},
		{"icc over two segments", [] { metadata_parts m; m.icc = encode_sweep::icc_profile(100000); return m; }},
		{"icc libpng refuses", [] { metadata_parts m; m.icc = encode_sweep::padded(df::blob{1, 2, 3}, 40); return m; }},
		{"everything", everything},
		{"exif at the marker limit", [&] { return exif_only(encode_sweep::padded(bare_exif, 65527)); }},
		{"exif a byte over", [&] { return exif_only(encode_sweep::padded(bare_exif, 65528)); }},
		{"signed exif a byte over", [&] { return exif_only(encode_sweep::padded(signed_exif, 65534)); }},
		{"xmp at the marker limit", [] { metadata_parts m; m.xmp = encode_sweep::xmp(65504); return m; }},
		{"xmp a byte over", [] { metadata_parts m; m.xmp = encode_sweep::xmp(65505); return m; }},
		{"iptc at the marker limit", [] { metadata_parts m; m.iptc = encode_sweep::padded(make_iptc_caption("x"), 65506); return m; }},
		{"iptc a byte over", [] { metadata_parts m; m.iptc = encode_sweep::padded(make_iptc_caption("x"), 65507); return m; }},
		{"photoshop record at the marker limit", [] { metadata_parts m; m.photoshop_resources = encode_sweep::photoshop_resources({65506}); return m; }},
		{"photoshop record over the limit", [] { metadata_parts m; m.photoshop_resources = encode_sweep::photoshop_resources({65508, 16}); return m; }},
		{"icc in 255 segments", [] { metadata_parts m; m.icc = encode_sweep::icc_profile(255 * icc_segment, true); return m; }},
		{"icc needing 256", [] { metadata_parts m; m.icc = encode_sweep::icc_profile(255 * icc_segment + 1, true); return m; }},
	};

	const std::vector<encode_sweep::webp_case> webp_cases = {
		{"webp", encode_sweep::webp_params(false, 70, false, false)},
		{"webp fast", encode_sweep::webp_params(false, 70, false, true)},
		{"webp q30 lossy alpha", encode_sweep::webp_params(false, 30, true, false)},
		{"webp lossless", encode_sweep::webp_params(true, 70, false, false)},
		{"webp q100", encode_sweep::webp_params(false, 100, false, false)},
		{"webp thumbnail", encode_sweep::webp_params(false, thumbnail_webp_quality, true, true)},
	};

	files ff;

	// Every encoder with the same picture and metadata: save_surface's three, and files' long-lived JPEG
	// encoder.
	const auto encode_all = [&](const std::string& what, const ui::const_surface_ptr& surface,
	                            const metadata_case& m)
	{
		note(what + " jpeg", attempt([&] { return save_surface(ui::image_format::JPEG, surface, m.make(), {}); }));
		note(what + " files jpeg", attempt([&] { return ff.surface_to_image(surface, m.make(), {}, ui::image_format::JPEG); }));
		note(what + " png", attempt([&] { return save_surface(ui::image_format::PNG, surface, m.make(), {}); }));
		note(what + " webp", attempt([&] { return save_surface(ui::image_format::WEBP, surface, m.make(), {}); }));
	};

	// Each orientation, with and without an EXIF to carry it.
	for (auto o = 0; o <= 8; ++o)
	{
		const auto surface = make_surface(16, 16, texture_format::RGB, static_cast<ui::orientation>(o));
		encode_all(std::format("orientation {}", o), surface, metadata[0]);
		encode_all(std::format("orientation {} bare exif", o), surface, metadata[1]);
	}

	// Each kind of metadata, on a picture turned a quarter so an orientation EXIF is wanted.
	for (const auto& m : metadata)
	{
		encode_all(m.name, make_surface(16, 16, texture_format::RGB, ui::orientation::right_top), m);
	}

	// The pictures.
	struct picture
	{
		int cx;
		int cy;
		texture_format format;
		ui::color_space cs = ui::color_space::rec601_full;
		bool opaque = false;
	};

	const std::vector<picture> pictures = {
		{1, 1, texture_format::RGB}, {7, 5, texture_format::RGB}, {16, 16, texture_format::RGB},
		{33, 17, texture_format::RGB}, {64, 48, texture_format::RGB}, {301, 199, texture_format::RGB},
		{7, 5, texture_format::ARGB}, {33, 17, texture_format::ARGB}, {64, 48, texture_format::ARGB},
		{64, 48, texture_format::ARGB, ui::color_space::rec601_full, true},
		{64, 48, texture_format::NV12, ui::color_space::rec601_full},
		{64, 48, texture_format::NV12, ui::color_space::rec601_limited},
		{64, 48, texture_format::NV12, ui::color_space::rec709_limited},
		{34, 18, texture_format::NV12, ui::color_space::rec601_full},
		{33, 17, texture_format::NV12, ui::color_space::rec601_full},
		{64, 48, texture_format::P010, ui::color_space::rec2020_limited},
	};

	for (const auto& p : pictures)
	{
		const auto surface = make_surface(p.cx, p.cy, p.format, ui::orientation::top_left, p.cs, p.opaque);
		const auto what = std::format("{}x{} format {} colour {} opaque {}", p.cx, p.cy, static_cast<int>(p.format),
		                              static_cast<int>(p.cs), p.opaque);
		const auto packed = ui::is_packed(p.format);

		// save_jpeg and save_png read four-byte pixels, so only files' routes, which refuse a plane, are
		// given one.
		if (packed)
		{
			for (const auto quality : {85, 40, 100, 1})
			{
				note(std::format("{} jpeg q{}", what, quality), attempt([&]
				{
					return save_surface(ui::image_format::JPEG, surface, {}, encode_sweep::jpeg_params(quality));
				}));
			}

			note(what + " png", attempt([&] { return save_surface(ui::image_format::PNG, surface, {}, {}); }));
		}

		note(what + " files jpeg", attempt([&] { return ff.surface_to_image(surface, {}, {}, ui::image_format::JPEG); }));
		note(what + " files png", attempt([&] { return ff.surface_to_image(surface, {}, {}, ui::image_format::PNG); }));
		note(what + " files either", attempt([&] { return ff.surface_to_image(surface, {}, {}, ui::image_format::Unknown); }));

		for (const auto& w : webp_cases)
		{
			note(what + " " + w.name, attempt([&] { return save_surface(ui::image_format::WEBP, surface, {}, w.params); }));
		}

		// A plane libwebp will not take is converted for the thumbnail store by FFmpeg's scaler, which
		// is not what this records.
		const auto webp_takes = packed || (p.format == texture_format::NV12 && ((p.cx | p.cy) & 1) == 0 &&
			(p.cs == ui::color_space::rec601_full || p.cs == ui::color_space::rec601_limited));
		if (webp_takes) note(what + " thumbnail", attempt([&] { return ff.surface_to_thumbnail(surface); }));
	}

	// Source JPEGs whose tables an edit is encoded against - or, where they cannot be adopted, not.
	{
		const auto test_jpg = df::blob_from_file(df::file_path(test_files_folder, "Test.jpg"));
		const auto excluded = test_files_folder.combine("excluded1");
		const auto made_q40 = save_surface(ui::image_format::JPEG, make_surface(64, 48, texture_format::RGB), {},
		                                   encode_sweep::jpeg_params(40));
		const auto made_q95 = save_surface(ui::image_format::JPEG, make_surface(33, 17, texture_format::RGB), {},
		                                   encode_sweep::jpeg_params(95));

		std::vector<std::pair<std::string, df::blob>> sources;
		sources.emplace_back("no source", df::blob{});
		sources.emplace_back("made at q40", made_q40->data().clone());
		sources.emplace_back("made at q95", made_q95->data().clone());
		sources.emplace_back("Test.jpg", test_jpg.clone());
		sources.emplace_back("Progressive.jpg", df::blob_from_file(df::file_path(excluded, "Progressive.jpg")));
		sources.emplace_back("cmyk.jpg", df::blob_from_file(df::file_path(test_files_folder, "cmyk.jpg")));
		sources.emplace_back("deep12.jpg", df::blob_from_file(df::file_path(excluded, "deep12.jpg")));
		sources.emplace_back("deep12gray.jpg", df::blob_from_file(df::file_path(excluded, "deep12gray.jpg")));
		sources.emplace_back("soi only", df::blob{0xFF, 0xD8});
		sources.emplace_back("Test.jpg cut at 100", df::blob(test_jpg.begin(), test_jpg.begin() + 100));
		sources.emplace_back("Test.jpg cut at 20", df::blob(test_jpg.begin(), test_jpg.begin() + 20));

		for (const auto& [name, source] : sources)
		{
			for (const auto& p : {picture{64, 48, texture_format::RGB}, picture{33, 17, texture_format::ARGB}})
			{
				const auto surface = make_surface(p.cx, p.cy, p.format);

				for (const auto quality : {85, 30})
				{
					const auto params = encode_sweep::jpeg_params(quality, source);
					const auto what = std::format("{} {}x{} q{}", name, p.cx, p.cy, quality);
					note(what + " jpeg", attempt([&] { return save_surface(ui::image_format::JPEG, surface, {}, params); }));
					note(what + " files jpeg", attempt([&] { return ff.surface_to_image(surface, {}, params, ui::image_format::JPEG); }));
				}
			}
		}
	}

	// The turns. One encoder writes them and, between them, pictures of its own, as the encoder files
	// holds does; and the budget is fixed, so the record does not depend on the machine's memory.
	jpeg_encoder encoder;
	jpeg_transformer transformer;
	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });
	constexpr int64_t budget = 256ll * 1024 * 1024;
	df::max_decode_bytes = budget;

	const auto between = make_surface(16, 16, texture_format::RGB, ui::orientation::bottom_right);
	const auto encode_between = [&]
	{
		return encoder.encode(16, 16, between->pixels(), static_cast<uint32_t>(between->stride()),
		                      ui::orientation::bottom_right, {}, {});
	};

	// An encode started and abandoned, as a throw part way leaves one, and one refused for its metadata.
	{
		const auto surface = make_surface(32, 32, texture_format::RGB);
		const auto abandoned = attempt([&]
		{
			encoder.start(32, 32, ui::orientation::right_top, everything(), {});
			return df::blob{};
		});
		note("started and abandoned", abandoned);
		note("then encodes", attempt(encode_between));
		note("refused", attempt([&]
		{
			return encoder.encode(32, 32, surface->pixels(), static_cast<uint32_t>(surface->stride()),
			                      ui::orientation::top_left, exif_only(encode_sweep::padded(bare_exif, 65528)), {});
		}));
		note("then encodes", attempt(encode_between));
	}

	const auto turned = [&](const df::cspan jpeg, const simple_transform t) -> std::string
	{
		try
		{
			auto over_budget = false;
			const auto data = transformer.transform(jpeg, encoder, t, &over_budget);
			return std::format("{} over budget {}", encode_sweep::describe(data), over_budget);
		}
		catch (const std::exception& e)
		{
			return std::format("threw {}", e.what());
		}
	};

	const auto turn = [&](const std::string& name, const df::cspan jpeg, const bool thorough)
	{
		int64_t coefficients = 0;

		try
		{
			const auto ok = transformer.read_header(jpeg);
			const auto d = ok ? transformer.dimensions() : sizei{};
			coefficients = ok ? transformer.coefficient_bytes() : 0;
			note(name + " header", std::format("{} {}x{} coefficients {}", ok, d.cx, d.cy, coefficients));
		}
		catch (const std::exception& e)
		{
			note(name + " header", std::format("threw {}", e.what()));
		}

		for (auto t = 0; t < 8; ++t)
		{
			note(std::format("{} turn {}", name, t), turned(jpeg, static_cast<simple_transform>(t)));
		}

		// Transposing is never refused as imperfect, so it reaches the budget whatever the size.
		if (coefficients > 0)
		{
			df::max_decode_bytes = coefficients * 2 - 1;
			note(name + " transposed a byte over budget", turned(jpeg, simple_transform::transpose));
			df::max_decode_bytes = coefficients * 2;
			note(name + " transposed on budget", turned(jpeg, simple_transform::transpose));
			df::max_decode_bytes = budget;
		}

		note(name + " then encodes", attempt(encode_between));

		if (thorough)
		{
			for (const auto cut : scan_parity::cut_points(jpeg.size))
			{
				note(std::format("{} cut at {} transposed", name, cut), turned({jpeg.data, cut}, simple_transform::transpose));
			}

			df::blob copy(jpeg.begin(), jpeg.end());

			for (uint64_t k = 0; k < 16; ++k)
			{
				const auto at = scan_parity::corruption_position(k, copy.size());
				const auto original = copy[at];
				copy[at] = static_cast<uint8_t>(original ^ 0xFF);
				note(std::format("{} corrupt at {} transposed", name, at), turned(copy, simple_transform::transpose));
				copy[at] = original;
			}

			note(name + " then encodes", attempt(encode_between));
		}
	};

	{
		const auto made = [&](const int cx, const int cy, const texture_format format, const metadata_parts& m,
		                      const int quality)
		{
			const auto surface = make_surface(cx, cy, format);
			return encoder.encode(cx, cy, surface->pixels(), static_cast<uint32_t>(surface->stride()),
			                      ui::orientation::top_left, m, encode_sweep::jpeg_params(quality));
		};

		turn("made 32x32 camera exif", made(32, 32, texture_format::RGB, exif_only(camera_exif.clone()), 85), true);
		turn("made 32x20", made(32, 20, texture_format::RGB, {}, 85), true);
		turn("made 48x48 signed exif q100", made(48, 48, texture_format::RGB, exif_only(signed_exif.clone()), 100), false);
		turn("made 33x17 everything", made(33, 17, texture_format::RGB, everything(), 85), false);

		metadata_parts icc_and_xmp;
		icc_and_xmp.icc = encode_sweep::icc_profile(100000);
		icc_and_xmp.xmp = encode_sweep::xmp();
		turn("made 64x48 icc and xmp", made(64, 48, texture_format::ARGB, icc_and_xmp, 85), false);

		// Sony.JPG is the one fixture whose EXIF gives its thumbnail an orientation, and too large to turn
		// here, so a picture of the sweep's own carries its EXIF.
		const auto sony_exif = encode_sweep::exif_segment(df::blob_from_file(df::file_path(test_files_folder, "Sony.JPG")));
		assert_equal(true, sony_exif.size() > 1000, "the Sony fixture carries EXIF");
		turn("made 32x32 sony exif", made(32, 32, texture_format::RGB, exif_only(sony_exif.clone()), 85), false);
		turn("empty", {}, false);

		const df::blob not_jpeg{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
		turn("not a jpeg", not_jpeg, false);
	}

	// The fixtures small enough to turn every way quickly, by name so every platform takes them in the
	// same order.
	std::vector<df::file_path> found;
	scan_parity::add_folder(test_files_folder, found);
	std::vector<std::pair<std::string, df::blob>> fixtures;

	for (const auto& path : found)
	{
		auto bytes = df::blob_from_file(path);
		if (bytes.size() >= 64 && bytes.size() <= 300 * 1024 && files::detect_format(bytes) == detected_format::JPEG)
			fixtures.emplace_back(std::string(path.name()), std::move(bytes));
	}

	std::ranges::sort(fixtures, [](const auto& a, const auto& b)
	{
		return a.first != b.first ? a.first < b.first : a.second.size() < b.second.size();
	});

	for (const auto& [name, bytes] : fixtures) turn(name, bytes, bytes.size() < 100 * 1024);

	const auto checksum = df::crc32c(record.data(), record.size());
	df::log(__FUNCTION__, std::format("{} fixtures turned, {} outcomes, {} encoded; record {:08x}", fixtures.size(),
	                                  outcomes, encoded, checksum));

	assert_equal(true, fixtures.size() >= 15, "the fixtures hold JPEGs to turn");
	assert_equal(true, encoded > 700, "most of what is encoded encodes");
	assert_equal(1343, outcomes, "the sweep is the one the record was taken over");
	assert_equal(std::format("{:08x}", 0x7d620a31u), std::format("{:08x}", checksum),
	             "every outcome is the one Diffractor's encoders had before they moved to libfiles");
}

void register_files_tests(view_state& state, test_registry& tests)
{
	tests.add("Should write UTF8 ZIP entries"s, should_write_utf8_zip_entries);
	tests.add("Should keep archive entries after warnings"s, should_keep_archive_entries_after_warnings);
	tests.add("Should convert archive Unix timestamps"s, should_convert_archive_unix_timestamps);
	tests.add("Should scan stills alike from memory and streams"s, should_scan_stills_alike_from_memory_and_streams);
	tests.add("Should decode PSDs as load_psd did"s, should_decode_psds_as_load_psd_did);
	tests.add("Should decode JPEGs as jpeg_decoder_x did"s, should_decode_jpegs_as_jpeg_decoder_x_did);
	tests.add("Should read EXIF as Diffractor's parser did"s, should_read_exif_as_diffractors_parser_did);
	tests.add("Should write EXIF as libexif did"s, should_write_exif_as_libexif_did);
	tests.add("Should turn EXIF without rewriting it"s, should_turn_exif_without_rewriting_it);
	tests.add("Should list EXIF as libexif did"s, should_list_exif_as_libexif_did);
	tests.add("Should list Commodore media as list_disk did"s, should_list_commodore_media_as_list_disk_did);
	tests.add("Should decode PNGs as load_png did"s, should_decode_pngs_as_load_png_did);
	tests.add("Should decode WebPs as load_webp did"s, should_decode_webps_as_load_webp_did);
	tests.add("Should decode HEIFs as load_heif did"s, should_decode_heifs_as_load_heif_did);
	tests.add("Should decode JXLs as load_jxl did"s, should_decode_jxls_as_load_jxl_did);
	tests.add("Should load raws as load_raw did"s, should_load_raws_as_load_raw_did);
	tests.add("Should turn a raw preview as its raw data"s, should_turn_a_raw_preview_as_its_raw_data);
	tests.add("Should scan raws as scan_raw did"s, should_scan_raws_as_scan_raw_did);
	tests.add("Should encode as Diffractor's encoders did"s, should_encode_as_diffractors_encoders_did);
	for (const auto precision : {8, 9, 10, 11, 12, 13, 16})
		tests.add(std::format("Should decode lossless {} bit grayscale JPEG", precision),
		          [precision] { should_decode_lossless_grayscale_jpeg(precision); });
	tests.add("Should decode associated alpha JXL"s, should_decode_associated_alpha_jxl);
	tests.add("Should decode associated alpha HEIF"s, should_decode_associated_alpha_heif);
	tests.add("Should rotate lossless EXIF dimensions"s, should_rotate_lossless_exif_dimensions);
	//
	// Write path
	//
	tests.add("Should check overwrite"s, should_check_overwrite);
	tests.add("Should report zip create failure"s, should_report_zip_create_failure);
	tests.add("Should add a multi chunk file to a zip"s, should_add_a_multi_chunk_file_to_a_zip);
	// SRC-010 - long names must not be consumed as a truncated C string.
	tests.add("Should round trip zip entry names"s, should_round_trip_zip_entry_names);
	// SRC-011 - application months are 1-12 while minizip takes 0-11.
	tests.add("Should round trip zip entry dates"s, should_round_trip_zip_entry_dates);
	tests.add("Should create original before replace"s, should_create_original_before_replace);
	tests.add("Should report move or copy collision paths"s, should_report_move_or_copy_collision_paths);
#ifndef _WIN32
	// PLAT-007 - Linux replacement copy truncated source/destination aliases before reading.
	tests.add("Should reject Linux folder file collisions"s, should_reject_linux_folder_file_collisions);
	tests.add("Should report Linux file operation errors"s, should_report_linux_file_operation_errors);
	tests.add("Should preserve Linux directory move on unsupported no replace"s,
	          should_preserve_linux_directory_move_on_unsupported_no_replace);
	tests.add("Should refuse Linux copy identity overwrites"s, should_refuse_linux_copy_identity_overwrites);
	// PLAT-007 - Linux copy ignored short writes and delayed close failures.
	tests.add("Should report Linux copy flush failures"s, should_report_linux_copy_flush_failures);
	// PLAT-008 - Linux auto-rename used a pre-check then an overwrite-capable copy.
	tests.add("Should move or copy by claiming Linux auto rename destinations atomically"s,
	          should_claim_linux_auto_rename_destinations_atomically);
	// PLAT-009 - Linux recursive folder copy could copy into its own destination.
	tests.add("Should move or copy reject Linux recursive copy into descendant"s,
	          should_reject_linux_recursive_copy_into_descendant);
	// G21 review - directory symlinks must be listed so delete can unlink them.
	tests.add("Should delete Linux directory symlinks"s, should_delete_linux_directory_symlinks);
	// G21 review - folder copy must not silently drop directory symlinks.
	tests.add("Should move or copy Linux directory symlinks"s, should_move_or_copy_linux_directory_symlinks);
	// G21 review - copying into the source's own parent should auto-rename, not fail on identity.
	tests.add("Should move or copy into its own folder with auto rename"s,
	          should_move_or_copy_into_its_own_folder_with_auto_rename);
	// G21 review - an inner collision belongs to the claimed tree, not the outer name picker.
	tests.add("Should move or copy stop on inner Linux folder collision"s,
	          should_move_or_copy_stop_on_inner_linux_folder_collision);
	// G21 review - filesystems without renameat2/link support still need a safe no-replace move.
	tests.add("Should move Linux no replace without renameat or links"s,
	          should_move_linux_no_replace_without_renameat_or_links);
	// G21 review - replacement copy must respect destination writeability and mode.
	tests.add("Should refuse or preserve Linux replace permissions"s,
	          should_refuse_or_preserve_linux_replace_permissions);
	// G21 review - containment checks must follow aliases into the source subtree.
	tests.add("Should move or copy reject Linux destination alias inside source"s,
	          should_move_or_copy_reject_linux_destination_alias_inside_source);
#endif
	tests.add("Should fail replace when flush fails"s, should_fail_replace_when_flush_fails);
#ifdef _WIN32
	tests.add("Should cleanup failed update temps"s, should_cleanup_failed_update_temps);
#endif
	tests.add("Should not rewrite unchanged file"s, should_not_rewrite_unchanged_file);
	tests.add("Should return written image"s, should_return_written_image);
	tests.add("Should hand over written handle"s, should_hand_over_written_handle);
	tests.add("Should save .png"s, [] { should_save(".png", true); });
	tests.add("Should save .jpg"s, [] { should_save(".jpg", true); });
	tests.add("Should save .webp"s, [] { should_save(".webp", true); });

	constexpr std::string_view read_only_formats[] = {
		"gradient.bmp", "gradient.tga", "gradient.sgi", "gradient.pcx", "gradient.ppm", "gradient.pgm"
	};

	for (auto name : read_only_formats)
	{
		tests.add(std::format("Should decode {}", name), [name] { should_decode_a_read_only_format(name); });
	}

	//
	// Format detection
	//
	tests.add("Should settle a transport stream extension by header"s,
	          should_settle_transport_stream_extension_by_header);
	tests.add("Should detect tiff by version"s, should_detect_tiff_by_version);
	tests.add("Should register every still format"s, should_register_every_still_format);

	//
	// Containers
	//
	tests.add("Should scan d64"s, should_scan_d64);
	tests.add("Should scan archive"s, should_scan_archive);

	//
	// Codec decode
	//
	tests.add("Should scan and load bitmap psd"s, should_scan_and_load_bitmap_psd);
	// SRC-026 - PSD stores Lab as byte L plus biased a/b samples
	tests.add("Should convert psd lab samples to lab coordinates"s,
	          should_convert_psd_lab_samples_to_lab_coordinates);
	// SRC-028 - PSD resource sections are metadata, not unbounded allocation requests
	tests.add("Should bound psd resource allocations"s, should_bound_psd_resource_allocations);
	// SRC-028 - large unused resources do not hide bounded metadata resources
	tests.add("Should read small psd metadata after large unused resource"s,
	          should_read_small_psd_metadata_after_large_unused_resource);
	// SRC-028 - the only PSD colour table allocation needed by the loader is the valid palette
	tests.add("Should load valid psd indexed palette"s, should_load_valid_psd_indexed_palette);
	tests.add("Should keep dimensions from truncated gif"s, should_keep_dimensions_from_truncated_gif);
	tests.add("Should reject absurd tiff dimensions"s, should_reject_absurd_tiff_dimensions);
	tests.add("Should extract embedded thumbnails only on demand"s,
	          should_extract_embedded_thumbnails_only_on_demand);
	tests.add("Should decode heic hevc with ffmpeg"s, should_decode_heic_hevc_with_ffmpeg);

	//
	// JPEG
	//
	tests.add("Should read jpeg orientation"s, should_read_jpeg_orientation);
	tests.add("Should read one EXIF orientation everywhere"s, should_read_one_exif_orientation_everywhere);
	// SRC-034 - APP13 can hold named and reordered Photoshop resources
	tests.add("Should parse jpeg photoshop iptc resources"s,
	          should_parse_jpeg_photoshop_iptc_resources);
	// Review G04 - APP13 PSIR must not override APP1/APP2 metadata
	tests.add("Should keep app13 from overriding jpeg app1 and app2 metadata"s,
	          should_keep_app13_from_overriding_jpeg_app1_and_app2_metadata);
	// Review G04 - non-IPTC Photoshop resources survive a JPEG re-encode
	tests.add("Should preserve non iptc jpeg photoshop resources"s,
	          should_preserve_non_iptc_jpeg_photoshop_resources);
	// Review G04 - Photoshop resources may span consecutive APP13 segments
	tests.add("Should roundtrip split jpeg photoshop resources"s,
	          should_roundtrip_split_jpeg_photoshop_resources);
	// Review G04 - XMP Toolkit splits Photoshop resources without regard to resource boundaries
	tests.add("Should join straddled jpeg photoshop resources"s,
	          should_join_straddled_jpeg_photoshop_resources);
	// Review G04 - crafted APP13 runs are capped before they grow without bound
	tests.add("Should cap joined jpeg photoshop resources"s,
	          should_cap_joined_jpeg_photoshop_resources);
	tests.add("Should prefer primary orientation over the thumbnail ifd"s,
	          should_prefer_primary_orientation_over_the_thumbnail_ifd);
	tests.add("Should use thumbnail orientation only when primary is absent"s,
	          should_use_thumbnail_orientation_only_when_primary_is_absent);
	tests.add("Should prefer primary tiff orientation over the thumbnail ifd"s,
	          should_prefer_primary_tiff_orientation_over_the_thumbnail_ifd);
	tests.add("Should reuse source jpeg tables"s, should_reuse_source_jpeg_tables);
	tests.add("Should save over a source JPEG libjpeg fails on"s, should_save_over_a_source_jpeg_libjpeg_fails_on);
	tests.add("Should refuse imperfect lossless rotate"s, should_refuse_imperfect_lossless_rotate);
	tests.add("Should survive truncated lossless rotate"s, should_survive_truncated_lossless_rotate);
	tests.add("Should refuse an over budget lossless rotate"s, should_refuse_an_over_budget_lossless_rotate);
	tests.add("Should not re-encode a rotate refused for its budget"s,
	          should_not_re_encode_a_rotate_refused_for_its_budget);
	tests.add("Should charge a progressive jpeg for its coefficients"s,
	          should_charge_a_progressive_jpeg_for_its_coefficients);
	tests.add("Should reuse jpeg encoder after abandoned encode"s,
	          should_reuse_jpeg_encoder_after_abandoned_encode);
	tests.add("Should rotate lossless"s, should_rotate_lossless);
	tests.add("Should render ycbcr jpeg as nv12"s, should_render_ycbcr_jpeg_as_nv12);
	tests.add("Should decode scaled 422 jpeg as nv12"s, should_decode_scaled_422_jpeg_as_nv12);
	tests.add("Should report jpeg chroma subsampling"s, should_report_jpeg_chroma_subsampling);
	tests.add("Should decode 12bit gray jpeg"s, should_decode_12bit_gray_jpeg);
	tests.add("Should decode 12bit colour jpeg"s, should_decode_12bit_colour_jpeg);
	tests.add("Should decode 16bit gray jpeg"s, should_decode_16bit_gray_jpeg);
	tests.add("Should decode 16bit colour jpeg"s, should_decode_16bit_colour_jpeg);
	// SRC-029 - one-pixel axes cannot form NV12
	tests.add("Should decode thin jpeg when planar is preferred"s,
	          should_decode_thin_jpeg_when_planar_is_preferred);

	//
	// PNG
	//
	tests.add("Should scale 16bit png"s, should_scale_16bit_png);
	tests.add("Should apply png gamma"s, should_apply_png_gamma);
	// SRC-019 - PNG metadata may be stored after IDAT.
	tests.add("Should read png orientation after idat"s, should_read_png_orientation_after_idat);

	//
	// WebP
	//
	tests.add("Should honor webp save quality"s, should_honor_webp_save_quality);
	tests.add("Should tag webp surface alpha"s, should_tag_webp_surface_alpha);
	tests.add("Should decode opaque lossy webp as nv12"s, should_decode_opaque_lossy_webp_as_nv12);
	tests.add("Should decode webp at the requested size"s, should_decode_webp_at_the_requested_size);
	tests.add("Should let the caller choose the decoded pixel format"s,
	          should_let_the_caller_choose_the_decoded_pixel_format);
	tests.add("Should thumbnail a jpeg without leaving yuv"s, should_thumbnail_a_jpeg_without_leaving_yuv);
	tests.add("Should decode odd sized webp as nv12"s, should_decode_odd_sized_webp_as_nv12);
	tests.add("Should keep thumbnail alpha only when needed"s, should_keep_thumbnail_alpha_only_when_needed);
	tests.add("Should refuse truncated webp decode"s, should_refuse_truncated_webp_decode);
	tests.add("Should preserve webp chunks on metadata save"s, should_preserve_webp_chunks_on_metadata_save);
	// SRC-031 - requested WebP metadata chunks are all-or-fail
	tests.add("Should fail webp save when metadata chunk insertion fails"s,
	          should_fail_webp_save_when_metadata_chunk_insertion_fails);

	//
	// RAW
	//
	tests.add("Should convert raw to jpeg"s, should_convert_raw_to_jpeg);
	tests.add("Should refuse a file too large to hold"s, should_refuse_a_file_too_large_to_hold);
	// SRC-017 - encoded-byte ceiling applies to bitmap thumbnail scans
	tests.add("Should apply encoded ceiling to bitmap thumbnail scans"s,
	          should_apply_encoded_ceiling_to_bitmap_thumbnail_scans);

	//
	// File handle lifetime
	//
	tests.add("Should keep file handles detached until last operation"s,
	          should_keep_file_handles_detached_until_last_operation);
	// SRC-032 - sliding reader reopen clears the cached window
	tests.add("Should reset sliding reader when reopened"s, should_reset_sliding_reader_when_reopened);

	//
	// Decoder robustness
	//
#ifndef _DEBUG
	tests.add("Should not crash on JPEG"s, [] { should_not_crash("Small.jpg"); });
	tests.add("Should not crash on GIF"s, [] { should_not_crash("tuesday.gif"); });
	tests.add("Should not crash on TIFF"s, [] { should_not_crash("Small.tif"); });
	tests.add("Should not crash on PNG"s, [] { should_not_crash("Cube.png"); });
	tests.add("Should not crash on WEBP"s, [] { should_not_crash("lake.webp"); });
#endif
}
