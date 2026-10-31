// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Index and database tests. Verifies the inverted/trigram/postings index, folder scanning
// and roots parsing, the SQLite cache schema and storage, thumbnail publication, staging and
// trimming, item property and metadata caching, duplicate and presence reporting, and cloud
// placeholder hydration.

#include "pch.h"

#include <condition_variable>

#include <sqlite3.h>

#include "test_fixtures.h"
#include "model_db_pack.h"
#include "model_postings.h"
#include "util_crash_files_db.h"
#include "app_util.h"
#include "metadata_exif.h"
#include "metadata_iptc.h"
#include "metadata_xmp.h"
#include "ui_elements.h"
#include "ui_map_common.h"

extern std::function<void(df::folder_path folder, int attempt)> test_after_validate_folder_snapshot;
extern std::function<void(const df::index_file_item& anchor)> test_after_duplicate_anchor;

static void should_create_database_schema()
{
	sqlite3_initialize();

	const auto database_path = _temps.next_path(".db");
	sqlite3* database_handle = nullptr;
	const auto open_result = sqlite3_open(database_path.str().c_str(), &database_handle);
	const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw_db(database_handle, sqlite3_close);

	if (open_result != SQLITE_OK)
	{
		throw test_assert_exception(std::format("Failed to create schema test database: {}",
		                                        raw_db ? sqlite3_errmsg(raw_db.get()) : "out of memory"));
	}

	const auto resource = load_app_resource(app_resource::sql);
	const std::string schema(reinterpret_cast<const char*>(resource.data()), resource.size());
	if (sqlite3_exec(raw_db.get(), schema.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
	{
		throw test_assert_exception(
			std::format("Failed to execute database schema: {}", sqlite3_errmsg(raw_db.get())));
	}

	const auto query_text = [&raw_db](const std::string_view sql)
	{
		sqlite3_stmt* statement_handle = nullptr;
		const auto prepare_result = sqlite3_prepare_v2(raw_db.get(), sql.data(), static_cast<int>(sql.size()),
		                                               &statement_handle, nullptr);
		const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(statement_handle, sqlite3_finalize);

		if (prepare_result != SQLITE_OK || sqlite3_step(statement.get()) != SQLITE_ROW)
		{
			throw test_assert_exception(
				std::format("Failed schema query '{}': {}", sql, sqlite3_errmsg(raw_db.get())));
		}

		const auto* text = sqlite3_column_text(statement.get(), 0);
		return text == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(text));
	};

	const auto raw_journal_mode = query_text("PRAGMA journal_mode");
	assert_equal(true, !raw_journal_mode.empty(), "schema leaves journal mode to the database owner");
	assert_equal("1", query_text("PRAGMA synchronous"), "schema synchronous mode");
	assert_equal("item_imports,item_properties,item_thumbnails,web_service_cache",
	             query_text("SELECT group_concat(name, ',') FROM (SELECT name FROM sqlite_schema "
		             "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name)"),
	             "schema tables");
	assert_equal("folder,name,properties,hash,media_position,flag,crc,last_scanned,last_indexed",
	             query_text("SELECT group_concat(name, ',') FROM pragma_table_info('item_properties')"),
	             "item_properties columns");
	assert_equal("folder,name,bitmap,cover_art,last_scanned",
	             query_text("SELECT group_concat(name, ',') FROM pragma_table_info('item_thumbnails')"),
	             "item_thumbnails columns");
	assert_equal("key,created_date,value",
	             query_text("SELECT group_concat(name, ',') FROM pragma_table_info('web_service_cache')"),
	             "web_service_cache columns");
	assert_equal("name,modified,size,imported",
	             query_text("SELECT group_concat(name, ',') FROM pragma_table_info('item_imports')"),
	             "item_imports columns");
	assert_equal("folder,name",
	             query_text("SELECT group_concat(name, ',') FROM "
		             "(SELECT name FROM pragma_table_info('item_properties') WHERE pk > 0 ORDER BY pk)"),
	             "item_properties primary key");
	assert_equal("folder,name",
	             query_text("SELECT group_concat(name, ',') FROM "
		             "(SELECT name FROM pragma_table_info('item_thumbnails') WHERE pk > 0 ORDER BY pk)"),
	             "item_thumbnails primary key");
	assert_equal("key",
	             query_text("SELECT group_concat(name, ',') FROM "
		             "(SELECT name FROM pragma_table_info('web_service_cache') WHERE pk > 0 ORDER BY pk)"),
	             "web_service_cache primary key");
	assert_equal("name,modified,size",
	             query_text("SELECT group_concat(name, ',') FROM "
		             "(SELECT name FROM pragma_table_info('item_imports') WHERE pk > 0 ORDER BY pk)"),
	             "item_imports primary key");
	assert_equal("ok", query_text("PRAGMA integrity_check"), "schema integrity");
}

static void should_open_database_in_wal_mode()
{
	const auto index_path = _temps.next_path();
	const auto db_path = df::file_path(index_path.folder(), index_path.file_name_without_extension(), ".db");

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	{
		database db(index);
		db.open(index_path.folder(), index_path.file_name_without_extension());
		db.close();
	}

	sqlite3* handle = nullptr;
	const auto open_result = sqlite3_open(db_path.str().c_str(), &handle);
	const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);
	if (open_result != SQLITE_OK) throw test_assert_exception("Failed to open database journal test"s);

	sqlite3_stmt* stmt = nullptr;
	if (sqlite3_prepare_v2(raw.get(), "PRAGMA journal_mode", -1, &stmt, nullptr) != SQLITE_OK)
		throw test_assert_exception("Failed to read journal mode"s);
	const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);

	assert_equal(SQLITE_ROW, sqlite3_step(statement.get()), "journal mode row");
	const auto* text = sqlite3_column_text(statement.get(), 0);
	assert_equal("wal", std::string(reinterpret_cast<const char*>(text)), "database owner selects WAL");
}

static void should_keep_rollback_journal_schema_initialization()
{
	sqlite3_initialize();

	const auto database_path = _temps.next_path(".db");
	sqlite3* database_handle = nullptr;
	const auto open_result = sqlite3_open(database_path.str().c_str(), &database_handle);
	const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw_db(database_handle, sqlite3_close);
	if (open_result != SQLITE_OK) throw test_assert_exception("Failed to open rollback journal test database"s);

	assert_equal(SQLITE_OK, database_test_seams::set_journal_mode(raw_db.get(), "DELETE"sv),
	             "rollback journal can be selected");

	const auto resource = load_app_resource(app_resource::sql);
	const std::string schema(reinterpret_cast<const char*>(resource.data()), resource.size());
	if (sqlite3_exec(raw_db.get(), schema.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
	{
		throw test_assert_exception("Failed to execute schema after rollback selection"s);
	}

	sqlite3_stmt* stmt = nullptr;
	if (sqlite3_prepare_v2(raw_db.get(), "PRAGMA journal_mode", -1, &stmt, nullptr) != SQLITE_OK)
		throw test_assert_exception("Failed to read retained journal mode"s);
	const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);
	assert_equal(SQLITE_ROW, sqlite3_step(statement.get()), "journal mode row");
	const auto* text = sqlite3_column_text(statement.get(), 0);
	const auto retained_mode = std::string(reinterpret_cast<const char*>(text));
	assert_equal(true, !retained_mode.empty(), "schema creation keeps a journal mode selected by SQLite");

	sqlite3* memory_handle = nullptr;
	if (sqlite3_open(":memory:", &memory_handle) != SQLITE_OK)
		throw test_assert_exception("Failed to open memory database"s);
	const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> memory(memory_handle, sqlite3_close);
	assert_equal(true, database_test_seams::set_journal_mode(memory.get(), "WAL"sv) != SQLITE_OK,
	             "failure to establish the requested mode is reported");

	const auto assert_fallback_open = [](const df::file_path index_path, const std::string_view label)
	{
		null_async_strategy as;
		const location_cache locations;
		index_state index(as, locations);

		database_test_seams::fail_next_journal_probe(SQLITE_IOERR_SHMOPEN);
		database db(index);
		db.open(index_path.folder(), index_path.file_name_without_extension());
		assert_equal(true, db.is_open(), std::format("{} opens after shared-memory failure", label));
		db.close();

		const auto db_path = df::file_path(index_path.folder(), index_path.file_name_without_extension(), ".db");
		sqlite3* handle = nullptr;
		if (sqlite3_open(db_path.str().c_str(), &handle) != SQLITE_OK)
			throw test_assert_exception("Failed to inspect fallback database"s);
		const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);

		{
			sqlite3_stmt* mode_stmt = nullptr;
			if (sqlite3_prepare_v2(raw.get(), "PRAGMA journal_mode", -1, &mode_stmt, nullptr) != SQLITE_OK)
				throw test_assert_exception("Failed to inspect fallback journal mode"s);
			const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> mode(mode_stmt, sqlite3_finalize);
			assert_equal(SQLITE_ROW, sqlite3_step(mode.get()), "journal mode row");
			const auto* text = sqlite3_column_text(mode.get(), 0);
			assert_equal("delete", std::string(reinterpret_cast<const char*>(text)),
			             std::format("{} stays in rollback journal mode", label));
		}

		if (sqlite3_exec(raw.get(), "BEGIN; INSERT OR REPLACE INTO web_service_cache VALUES ('probe', 1, 'ok'); COMMIT;",
		                 nullptr, nullptr, nullptr) != SQLITE_OK)
		{
			throw test_assert_exception(std::format("{} could not write after fallback: {}", label,
			                                        sqlite3_errmsg(raw.get())));
		}
	};

	assert_fallback_open(_temps.next_path(), "fresh database"sv);

	const auto rollback_path = _temps.next_path();
	{
		const auto rollback_db_path = df::file_path(rollback_path.folder(), rollback_path.file_name_without_extension(), ".db");
		sqlite3* handle = nullptr;
		if (sqlite3_open(rollback_db_path.str().c_str(), &handle) != SQLITE_OK)
			throw test_assert_exception("Failed to seed rollback database"s);
		const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);
		assert_equal(SQLITE_OK, database_test_seams::set_journal_mode(raw.get(), "DELETE"sv),
		             "seed rollback journal");
		const auto seeded_schema = load_app_resource(app_resource::sql);
		const std::string seeded_sql(reinterpret_cast<const char*>(seeded_schema.data()), seeded_schema.size());
		if (sqlite3_exec(raw.get(), seeded_sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
			throw test_assert_exception("Failed to seed rollback schema"s);
	}
	assert_fallback_open(rollback_path, "rollback database"sv);

	const auto wal_path = _temps.next_path();
	{
		null_async_strategy as;
		const location_cache locations;
		index_state index(as, locations);
		database db(index);
		db.open(wal_path.folder(), wal_path.file_name_without_extension());
		db.close();
	}
	assert_fallback_open(wal_path, "existing WAL database"sv);
}

static void should_classify_schema_check_failures_before_replacement()
{
	assert_equal(false, database_test_seams::schema_failure_allows_replacement(SQLITE_BUSY),
	             "busy is environmental, not replaceable");
	assert_equal(false, database_test_seams::schema_failure_allows_replacement(SQLITE_LOCKED),
	             "locked is environmental, not replaceable");
	assert_equal(false, database_test_seams::schema_failure_allows_replacement(SQLITE_IOERR),
	             "I/O is environmental, not replaceable");
	assert_equal(true, database_test_seams::schema_failure_allows_replacement(SQLITE_ERROR),
	             "incompatible schema is replaceable");
	assert_equal(true, database_test_seams::schema_failure_allows_replacement(SQLITE_CORRUPT),
	             "corrupt bytes are replaceable");
	assert_equal(true, database_test_seams::schema_failure_allows_replacement(SQLITE_NOTADB),
	             "non-database bytes are replaceable");
}

static void should_store_thumbnails()
{
	const auto index_path = _temps.next_path();
	const auto file_path = test_files_folder.combine_file("Test.jpg");

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	const auto i = load_item(index, file_path, true);
	db.perform_writes();

	const auto thumb = db.load_thumbnail(i->path());
	assert_equal(i->thumbnail(), thumb.thumb, "local loaded thumb");
}

static void should_store_cover_art()
{
	const auto index_path = _temps.next_path();
	const auto file_path = test_files_folder.combine_file("indy.mp4");

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	const auto i = load_item(index, file_path, true);
	db.perform_writes();

	const auto thumb = db.load_thumbnail(i->path());
	assert_equal(i->cover_art(), thumb.cover_art, "local loaded cover art");
}

static void should_store_item_properties()
{
	const auto index_path = _temps.next_path();
	const auto file_path = test_files_folder.combine_file("Test.jpg");

	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	auto data = blob_from_file(file_path);
	const auto crc32c_expected = df::crc32c(data.data(), data.size());

	auto md = std::make_shared<prop::item_metadata>();
	md->album = "test"_c;
	md->orientation = ui::orientation::bottom_right;

	std::deque<item_db_write> writes;

	constexpr auto media_pos = 111.1;

	{
		item_db_write w;
		w.crc32c = crc32c_expected;
		w.md = md;
		w.media_position = media_pos;
		w.path = file_path;
		writes.emplace_back(std::move(w));
	}

	db.perform_writes(std::move(writes));
	db.load_index_values();

	auto item = index.find_item(file_path);
	auto item_md = item.metadata.load();

	assert_metadata(*md, *item_md, "index");
	assert_equal(crc32c_expected, item.crc32c, "index crc32");
	assert_equal(static_cast<int>(media_pos), static_cast<int>(item_md->media_position),
	             "index media position");
	assert_equal(md->orientation, item_md->orientation, "index orientation");

	const auto reloaded_crc = platform::file_crc32(file_path);
	assert_equal(reloaded_crc, item.crc32c, "platform::file_crc32 crc32");
}

// A rescan carries the position the index holds into the database row. A position saved only to the
// row was written back over by the next rescan of the file, so the next session resumed from wherever
// the index had been loaded.
static void should_hold_a_saved_playback_position_in_the_index()
{
	const auto root = _temps.next_folder("saved-playback-position");
	const auto path = root.combine_file("played.jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), path, false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);
	assert_equal(true, index.find_item(path).metadata.load() != nullptr, "the file was scanned");

	index.save_media_position(path, 42.0);

	const auto md = index.find_item(path).metadata.load();
	assert_equal(true, md != nullptr && static_cast<int>(md->media_position) == 42,
	             "the index holds the saved position, so a rescan carries it rather than the old one");
}

// A database written before db_metadata_version records place text that cannot be told apart from
// text the file itself carried, so opening it must drop the cached metadata and force a rescan.
// Everything a rescan does not replace is kept, or the upgrade would cost a full thumbnail rebuild.
static void should_invalidate_cached_metadata_written_by_an_older_build()
{
	const auto index_path = _temps.next_path();
	const auto db_path = df::file_path(index_path.folder(), index_path.file_name_without_extension(), ".db");
	const auto file_path = test_files_folder.combine_file("Test.jpg");

	null_async_strategy as;
	location_cache locations;

	{
		index_state index(as, locations);
		database db(index);
		db.open(index_path.folder(), index_path.file_name_without_extension());

		auto md = std::make_shared<prop::item_metadata>();
		md->album = "test"_c;
		md->location_place = "Somewhere"_c;

		std::deque<item_db_write> writes;
		item_db_write w;
		w.path = file_path;
		w.md = md;
		w.crc32c = 0x1234u;
		w.media_position = 111.1;
		w.metadata_scanned = df::date_t(2020, 1, 1, 0, 0, 0);
		writes.emplace_back(std::move(w));

		db.perform_writes(std::move(writes));
		db.close();
	}

	// Stamp the file as one an older build left behind.
	{
		sqlite3* handle = nullptr;
		const auto open_result = sqlite3_open(db_path.str().c_str(), &handle);
		const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);

		if (open_result != SQLITE_OK ||
			sqlite3_exec(raw.get(), "PRAGMA user_version = 0;", nullptr, nullptr, nullptr) != SQLITE_OK)
		{
			throw test_assert_exception("Failed to reset the database metadata version"s);
		}
	}

	index_state index(as, locations);
	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	const auto item = index.find_item(file_path);
	const auto item_md = item.metadata.load();

	assert_equal(0ll, item.metadata_scanned.load().to_int64(), "scan state cleared");
	assert_equal(true, item_md == nullptr || str::is_empty(item_md->location_place), "stale place cleared");
	assert_equal(0x1234u, item.crc32c, "crc retained");
	assert_equal(true, item_md != nullptr && static_cast<int>(item_md->media_position) == 111,
	             "media position retained");
}

// Moving to the date pack asks for a re-read, but a pre-pack row still answers in the meantime. So
// this upgrade clears only the scan stamp and keeps the cached properties, or every search, group
// and timeline would go blank until the background re-index caught up.
static void should_keep_answering_while_upgrading_to_the_date_pack()
{
	const auto index_path = _temps.next_path();
	const auto db_path = df::file_path(index_path.folder(), index_path.file_name_without_extension(), ".db");
	const auto file_path = test_files_folder.combine_file("Test.jpg");

	null_async_strategy as;
	location_cache locations;

	{
		index_state index(as, locations);
		database db(index);
		db.open(index_path.folder(), index_path.file_name_without_extension());

		auto md = std::make_shared<prop::item_metadata>();
		md->album = "test"_c;
		md->dates.add(prop::date_source::exif_original, df::date_t(2012, 9, 14, 19, 21, 14));

		std::deque<item_db_write> writes;
		item_db_write w;
		w.path = file_path;
		w.md = md;
		w.crc32c = 0x1234u;
		w.metadata_scanned = df::date_t(2020, 1, 1, 0, 0, 0);
		writes.emplace_back(std::move(w));

		db.perform_writes(std::move(writes));
		db.close();
	}

	// A database written by the build before the pack, rather than one from before version 1.
	{
		sqlite3* handle = nullptr;
		const auto open_result = sqlite3_open(db_path.str().c_str(), &handle);
		const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);

		if (open_result != SQLITE_OK ||
			sqlite3_exec(raw.get(), "PRAGMA user_version = 1;", nullptr, nullptr, nullptr) != SQLITE_OK)
		{
			throw test_assert_exception("Failed to set the database metadata version"s);
		}
	}

	index_state index(as, locations);
	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	const auto item = index.find_item(file_path);
	const auto item_md = item.metadata.load();

	assert_equal(0ll, item.metadata_scanned.load().to_int64(), "a re-scan is requested");
	assert_equal(true, item_md != nullptr, "the cached properties are kept");
	assert_equal(df::date_t(2012, 9, 14, 19, 21, 14), item_md->dates.original(),
	             "and still answer with the date they held");
	assert_equal("test", item_md->album.sv(), "the rest of the cached metadata is untouched");
}

// Versions 3 and 4 both changed only what an audio or video scan records, so either upgrade asks those
// items alone to be re-scanned, and every row keeps answering meanwhile.
static void should_keep_answering_while_upgrading_audio_metadata(const int stored_version)
{
	const auto index_path = _temps.next_path();
	const auto db_path = df::file_path(index_path.folder(), index_path.file_name_without_extension(), ".db");
	const auto photo_path = test_files_folder.combine_file("Test.jpg");
	const auto audio_path = test_files_folder.combine_file("Colorblind.mp3");

	null_async_strategy as;
	location_cache locations;

	{
		index_state index(as, locations);
		database db(index);
		db.open(index_path.folder(), index_path.file_name_without_extension());

		auto photo_md = std::make_shared<prop::item_metadata>();
		photo_md->album = "photo"_c;

		auto audio_md = std::make_shared<prop::item_metadata>();
		audio_md->album = "audio"_c;
		audio_md->audio_sample_rate = 65535;

		std::deque<item_db_write> writes;
		item_db_write photo;
		photo.path = photo_path;
		photo.md = photo_md;
		photo.metadata_scanned = df::date_t(2020, 1, 1, 0, 0, 0);
		writes.emplace_back(std::move(photo));
		item_db_write audio;
		audio.path = audio_path;
		audio.md = audio_md;
		audio.metadata_scanned = df::date_t(2020, 1, 1, 0, 0, 0);
		writes.emplace_back(std::move(audio));

		db.perform_writes(std::move(writes));
		db.close();
	}

	{
		sqlite3* handle = nullptr;
		const auto open_result = sqlite3_open(db_path.str().c_str(), &handle);
		const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);

		if (open_result != SQLITE_OK ||
			sqlite3_exec(raw.get(), std::format("PRAGMA user_version = {};", stored_version).c_str(), nullptr,
			             nullptr, nullptr) != SQLITE_OK)
		{
			throw test_assert_exception("Failed to set the audio metadata version"s);
		}
	}

	index_state index(as, locations);
	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	const auto photo_item = index.find_item(photo_path);
	const auto photo_md = photo_item.metadata.load();
	const auto audio_item = index.find_item(audio_path);
	const auto audio_md = audio_item.metadata.load();

	assert_equal(df::date_t(2020, 1, 1, 0, 0, 0).to_int64(), photo_item.metadata_scanned.load().to_int64(),
	             "photo scan state is not cleared by the audio upgrade");
	assert_equal(0ll, audio_item.metadata_scanned.load().to_int64(), "an audio re-scan is requested");
	assert_equal(true, photo_md != nullptr && audio_md != nullptr, "cached metadata is kept");
	assert_equal("photo", photo_md->album.sv(), "photo metadata still answers");
	assert_equal("audio", audio_md->album.sv(), "audio metadata still answers");
	assert_equal(65535u, audio_md->audio_sample_rate, "legacy saturated sample rate remains readable meanwhile");
}

// Rolling a release back is ordinary, and the cache file is shared with whatever build the user
// goes back to. The rows stay readable, but the version stamp must come back down: left above this
// build's own, going forward again would find a version already satisfied and skip the upgrade it
// owed, trusting rows written in an older shape in the meantime.
static void should_reclaim_a_cache_written_by_a_newer_build()
{
	const auto index_path = _temps.next_path();
	const auto db_path = df::file_path(index_path.folder(), index_path.file_name_without_extension(), ".db");
	const auto file_path = test_files_folder.combine_file("Test.jpg");

	null_async_strategy as;
	location_cache locations;

	{
		index_state index(as, locations);
		database db(index);
		db.open(index_path.folder(), index_path.file_name_without_extension());

		auto md = std::make_shared<prop::item_metadata>();
		md->album = "test"_c;
		md->dates.add(prop::date_source::exif_original, df::date_t(2012, 9, 14, 19, 21, 14));

		std::deque<item_db_write> writes;
		item_db_write w;
		w.path = file_path;
		w.md = md;
		w.metadata_scanned = df::date_t(2020, 1, 1, 0, 0, 0);
		writes.emplace_back(std::move(w));

		db.perform_writes(std::move(writes));
		db.close();
	}

	// Stamped by a release that does not exist yet.
	{
		sqlite3* handle = nullptr;
		const auto open_result = sqlite3_open(db_path.str().c_str(), &handle);
		const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);

		if (open_result != SQLITE_OK ||
			sqlite3_exec(raw.get(), "PRAGMA user_version = 99;", nullptr, nullptr, nullptr) != SQLITE_OK)
		{
			throw test_assert_exception("Failed to set the database metadata version"s);
		}
	}

	{
		index_state index(as, locations);
		database db(index);
		db.open(index_path.folder(), index_path.file_name_without_extension());

		const auto item = index.find_item(file_path);
		const auto item_md = item.metadata.load();

		assert_equal(true, item_md != nullptr, "the rows a newer build wrote are kept");
		assert_equal(df::date_t(2012, 9, 14, 19, 21, 14), item_md->dates.original(), "and still answer");
		assert_equal(df::date_t(2020, 1, 1, 0, 0, 0).to_int64(), item.metadata_scanned.load().to_int64(),
		             "and are not needlessly re-scanned");
		db.close();
	}

	sqlite3* handle = nullptr;
	const auto open_result = sqlite3_open(db_path.str().c_str(), &handle);
	const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);
	auto stamped = -1;

	if (open_result == SQLITE_OK)
	{
		sqlite3_stmt* stmt = nullptr;

		if (sqlite3_prepare_v2(raw.get(), "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK)
		{
			if (sqlite3_step(stmt) == SQLITE_ROW) stamped = sqlite3_column_int(stmt, 0);
			sqlite3_finalize(stmt);
		}
	}

	assert_equal(5, stamped, "the stamp comes back down to what this build writes");
}

// A cache file this build cannot read must be replaced, not refused. Everything it holds can be
// rebuilt by re-indexing, while failing to open it closes the app before the user can reach the
// reset that would repair it.
static void should_replace_an_unreadable_database()
{
	const auto index_path = _temps.next_path();
	const auto db_path = df::file_path(index_path.folder(), index_path.file_name_without_extension(), ".db");
	const auto file_path = test_files_folder.combine_file("Test.jpg");

	{
		std::ofstream corrupt(platform::to_stream_path(db_path), std::ios::binary | std::ios::trunc);
		corrupt << "SQLite format 3\0not a database at all";
	}

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	assert_equal(true, db.is_open(), "replaced database open");
	assert_equal(false, db.has_errors(), "faults from the replaced file cleared");

	const auto md = std::make_shared<prop::item_metadata>();
	md->album = "replaced"_c;

	std::deque<item_db_write> writes;
	item_db_write w;
	w.path = file_path;
	w.md = md;
	w.crc32c = 0x4321u;
	writes.emplace_back(std::move(w));

	db.perform_writes(std::move(writes));
	db.load_index_values();

	assert_equal(0x4321u, index.find_item(file_path).crc32c, "replaced database usable");
}

// Without a database the app still has to run. Every operation must complete rather than strand
// its caller, and the write queue must keep draining or the index would hoard encoded thumbnails
// for a whole session against a database that can never accept them.
static void should_run_without_a_database()
{
	const auto missing_folder = _temps.next_path().folder().combine("missing-cache-folder");
	const auto file_path = test_files_folder.combine_file("Test.jpg");

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(missing_folder, "diffractor-cache");

	assert_equal(false, db.is_open(), "database not open");

	item_db_write w;
	w.path = file_path;
	w.md = std::make_shared<prop::item_metadata>();
	index.db_writes().enqueue(std::move(w));

	db.perform_writes();
	assert_equal(true, index.db_writes().dequeue_all().empty(), "write queue drained");

	assert_equal(true, db.load_item_imports().empty(), "no import history");
	db.writes_item_imports({});
	db.web_service_cache("key", "value");
	assert_equal(true, db.web_service_cache("key").empty(), "no web cache");
	db.clean({file_path});
	assert_equal(false, ui::is_valid(db.load_thumbnail(file_path).thumb), "no thumbnail");
}

// The scan loops hand their rows to the database in groups. One row at a time found the write queue
// empty every time, so it woke the database thread - and opened a transaction - once per file.
static void should_hand_scan_results_to_the_database_in_groups()
{
	// Stands in for the database worker, which drains the whole write queue on every pass. That eager
	// drain is what makes a per-row producer wake it again for the very next row.
	class draining_async_strategy final : public null_async_strategy
	{
	public:
		index_state* index = nullptr;
		int drains = 0;
		size_t rows = 0;

		void queue_database(std::function<void(database&)> f) override
		{
			++drains;
			rows += index->db_writes().dequeue_all().size();
		}
	};

	draining_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	as.index = &index;

	df::index_roots paths;
	paths.folders.emplace(test_files_folder);
	paths.excludes.emplace(test_files_folder.combine("excluded1"));
	paths.exclude_wildcards.emplace("exclud*2"_c);

	index.index_roots(paths);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	as.rows += index.db_writes().dequeue_all().size();

	// Without the fixtures actually being scanned the comparison below would pass vacuously.
	assert_equal(expected_cached_item_count, index.stats.media_item_count, "cached item count");
	assert_equal(true, as.rows >= 40, std::format("rows written: {}", as.rows));

	// One drain per row is the defect. Grouping cannot need more than one per 64-row group.
	assert_equal(true, as.drains <= static_cast<int>(as.rows / 8),
	             std::format("database drains {} for {} rows", as.drains, as.rows));
}

static void should_pack_item_properties()
{
	const auto file_path = test_files_folder.combine_file("Test.jpg");
	const auto md = extract_properties(file_path);
	md->album = "test"_c;
	md->orientation = ui::orientation::bottom_right;

	metadata_packer packer;
	packer.pack(md);

	const auto unpacked = std::make_shared<prop::item_metadata>();

	metadata_unpacker unpacker(packer.cdata());
	unpacker.unpack(unpacked);

	assert_metadata(*md, *unpacked, "index");
	assert_equal(md->orientation, unpacked->orientation, "index orientation");

	md->audio_sample_rate = 192000;
	md->audio_channels = 6;

	metadata_packer audio_packer;
	audio_packer.pack(md);

	const auto audio_unpacked = std::make_shared<prop::item_metadata>();
	metadata_unpacker audio_unpacker(audio_packer.cdata());
	audio_unpacker.unpack(audio_unpacked);

	assert_equal(6, static_cast<int>(audio_unpacked->audio_channels), "audio channels round trip");
	assert_equal(192000u, audio_unpacked->audio_sample_rate, "high audio sample rate round trips");

	metadata_packer legacy_audio;
	legacy_audio._data[1] = 1;
	legacy_audio.write(prop::audio_sample_rate.id, static_cast<uint16_t>(65535));

	const auto legacy_unpacked = std::make_shared<prop::item_metadata>();
	metadata_unpacker legacy_unpacker(legacy_audio.cdata());
	legacy_unpacker.unpack(legacy_unpacked);
	assert_equal(65535u, legacy_unpacked->audio_sample_rate, "legacy 16-bit sample rate remains readable");

	for (const auto rating : {static_cast<int16_t>(-32768), static_cast<int16_t>(-2), static_cast<int16_t>(6)})
	{
		metadata_packer bad_rating;
		bad_rating.write(prop::rating.id, rating);
		const auto bad_unpacked = std::make_shared<prop::item_metadata>();
		metadata_unpacker bad_unpacker(bad_rating.cdata());
		bad_unpacker.unpack(bad_unpacked);
		assert_equal(static_cast<int16_t>(0), bad_unpacked->rating, "out-of-range cached rating is ignored");
	}

	for (const auto rating : {static_cast<int16_t>(-1), static_cast<int16_t>(0), static_cast<int16_t>(1),
		     static_cast<int16_t>(2), static_cast<int16_t>(3), static_cast<int16_t>(4), static_cast<int16_t>(5)})
	{
		metadata_packer good_rating;
		good_rating.write(prop::rating.id, rating);
		const auto good_unpacked = std::make_shared<prop::item_metadata>();
		metadata_unpacker good_unpacker(good_rating.cdata());
		good_unpacker.unpack(good_unpacked);
		assert_equal(rating, good_unpacked->rating, "valid cached rating survives");
	}

	// The panorama flag is only useful if it survives the re-index that fills it, and it is written
	// after the properties an older build stops unpacking at, so its round trip is asserted here.
	md->panorama = prop::panorama_projection::equirectangular;

	metadata_packer pano_packer;
	pano_packer.pack(md);

	const auto pano_unpacked = std::make_shared<prop::item_metadata>();
	metadata_unpacker pano_unpacker(pano_packer.cdata());
	pano_unpacker.unpack(pano_unpacked);

	assert_equal(static_cast<int>(prop::panorama_projection::equirectangular),
	             static_cast<int>(pano_unpacked->panorama), "index panorama projection");
	assert_equal(true, pano_unpacked->is_panorama(), "and it reads back as a panorama");
	assert_equal(false, unpacked->is_panorama(), "while a file that declared none stays none");
}

static void should_write_pack_rows_a_rollback_build_can_read()
{
	const auto md = std::make_shared<prop::item_metadata>();
	md->album = "rollback"_c;
	md->rating = 4;
	md->width = 640;
	md->height = 480;
	md->audio_sample_rate = 192000;
	md->audio_channels = 2;

	metadata_packer packer;
	packer.pack(md);

	const auto data = packer.cdata();
	assert_equal(1, static_cast<int>(data.data[1]), "outer pack version remains rollback-readable");

	size_t pos = 2;
	auto album_seen = false;
	auto rating_seen = false;
	auto dimensions_seen = false;
	auto channels_seen = false;
	auto sample_rate_seen = false;
	auto full_sample_rate_stops_old_reader = false;

	const auto read_len = [&]()
	{
		size_t result = data.data[pos++];
		if (result == 0xff)
		{
			result = static_cast<size_t>(data.data[pos++]);
			result |= static_cast<size_t>(data.data[pos++]) << 8;
		}
		else if (result == 0xfe)
		{
			result = static_cast<size_t>(data.data[pos++]);
			result |= static_cast<size_t>(data.data[pos++]) << 8;
			result |= static_cast<size_t>(data.data[pos++]) << 16;
			result |= static_cast<size_t>(data.data[pos++]) << 24;
		}
		return result;
	};

	while (pos + 2 <= data.size)
	{
		auto id = static_cast<uint16_t>(data.data[pos++]);
		id |= static_cast<uint16_t>(data.data[pos++] << 8);
		const auto len = read_len();
		const auto* value = data.data + pos;

		if (id == prop::album.id)
		{
			album_seen = std::string_view(reinterpret_cast<const char*>(value), len) == "rollback"sv;
		}
		else if (id == prop::rating.id && len == sizeof(int16_t))
		{
			int16_t rating = 0;
			std::memcpy(&rating, value, sizeof(rating));
			rating_seen = rating == 4;
		}
		else if (id == prop::dimensions.id && len == sizeof(df::xy32))
		{
			df::xy32 dimensions;
			std::memcpy(&dimensions, value, sizeof(dimensions));
			dimensions_seen = dimensions.x == 640 && dimensions.y == 480;
		}
		else if (id == prop::audio_channels.id && len == sizeof(uint16_t))
		{
			uint16_t channels = 0;
			std::memcpy(&channels, value, sizeof(channels));
			channels_seen = channels == 2;
		}
		else if (id == prop::audio_sample_rate.id)
		{
			uint16_t sample_rate = 0;
			std::memcpy(&sample_rate, value, sizeof(sample_rate));
			sample_rate_seen = len == sizeof(uint16_t) && sample_rate == UINT16_MAX;
		}
		else if (id == prop::audio_sample_rate_full.id)
		{
			full_sample_rate_stops_old_reader = true;
			break;
		}

		pos += len;
	}

	assert_equal(true, album_seen, "rollback reader keeps text fields");
	assert_equal(true, rating_seen, "rollback reader keeps rating fields");
	assert_equal(true, dimensions_seen, "rollback reader keeps dimensions fields");
	assert_equal(true, channels_seen, "rollback reader keeps newly added channels");
	assert_equal(true, sample_rate_seen, "rollback reader sees the saturated legacy sample rate");
	assert_equal(true, full_sample_rate_stops_old_reader, "full sample rate is written only after rollback fields");

	md->audio_sample_rate = 48000;
	metadata_packer ordinary_packer;
	ordinary_packer.pack(md);
	const auto ordinary_data = ordinary_packer.cdata();
	pos = 2;
	auto ordinary_rate_seen = false;

	while (pos + 2 <= ordinary_data.size)
	{
		auto id = static_cast<uint16_t>(ordinary_data.data[pos++]);
		id |= static_cast<uint16_t>(ordinary_data.data[pos++] << 8);
		size_t len = ordinary_data.data[pos++];
		if (len == 0xff)
		{
			len = static_cast<size_t>(ordinary_data.data[pos++]);
			len |= static_cast<size_t>(ordinary_data.data[pos++]) << 8;
		}
		else if (len == 0xfe)
		{
			len = static_cast<size_t>(ordinary_data.data[pos++]);
			len |= static_cast<size_t>(ordinary_data.data[pos++]) << 8;
			len |= static_cast<size_t>(ordinary_data.data[pos++]) << 16;
			len |= static_cast<size_t>(ordinary_data.data[pos++]) << 24;
		}
		const auto* value = ordinary_data.data + pos;
		if (id == prop::audio_sample_rate.id)
		{
			uint16_t sample_rate = 0;
			std::memcpy(&sample_rate, value, sizeof(sample_rate));
			ordinary_rate_seen = len == sizeof(uint16_t) && sample_rate == 48000;
			break;
		}
		pos += len;
	}

	assert_equal(true, ordinary_rate_seen, "rollback reader sees ordinary sample rates exactly");
}

// The cache file carries one name across every version, and a build older than the date pack sees a
// user_version above its own and therefore trusts the rows without re-reading them. So a row this
// build writes has to carry the two date properties that build knows, or reinstalling an earlier
// Diffractor dates every file from the filesystem with nothing that would ever repair it.
static void should_write_dates_an_older_build_can_read()
{
	const auto md = std::make_shared<prop::item_metadata>();
	md->dates.add(prop::date_source::exif_original, df::date_t(2019, 5, 4, 9, 0, 0));
	md->dates.add_utc(prop::date_source::container_created, df::date_t(2026, 8, 19, 7, 0, 0));
	// Present so the ordering assertion below is not vacuous: these are the first records v1.26.4
	// cannot name, and it stops at them.
	md->altitude = 120;
	md->gps_speed = 3;

	metadata_packer packer;
	packer.pack(md);

	auto original_at = -1;
	auto created_at = -1;
	auto pack_at = -1;
	auto oldest_reader_stops_at = -1;
	auto record = 0;

	// Walked by id and length, which is all an older build does: it names what it knows and steps
	// over the rest. v1.26.4 is stricter still - it stops dead at the first id it cannot name - so
	// where the two date records sit matters as much as that they are written.
	metadata_unpacker reader(packer.cdata());

	while (!reader.at_end())
	{
		const prop::key_ref t = reader.read_type();

		if (t == prop::created_exif) original_at = record;
		else if (t == prop::created_utc) created_at = record;
		else if (t == prop::dates_packed) pack_at = record;

		if (oldest_reader_stops_at < 0 && (t == prop::altitude || t == prop::gps_speed))
		{
			oldest_reader_stops_at = record;
		}

		reader.skip_val();
		++record;
	}

	assert_equal(true, pack_at >= 0, "the pack is written");
	assert_equal(true, original_at >= 0, "and the Original date an older build reads");
	assert_equal(true, created_at >= 0, "and the Created instant an older build reads");
	assert_equal(true, oldest_reader_stops_at >= 0, "the record the oldest reader stops at is present");
	assert_equal(true, original_at < oldest_reader_stops_at, "the Original date precedes it");
	assert_equal(true, created_at < oldest_reader_stops_at, "and so does the Created instant");

	// This build must not read its own compatibility copy back as a source, or every re-loaded row
	// would claim a legacy reading it never had - and the copies precede the pack, so with four
	// groups they could evict a real one.
	const auto unpacked = std::make_shared<prop::item_metadata>();
	metadata_unpacker unpacker(packer.cdata());
	unpacker.unpack(unpacked);

	assert_equal(df::date_t(2019, 5, 4, 9, 0, 0), unpacked->dates.original(), "the pack answers Original");
	assert_equal(false, unpacked->dates.has_source(prop::date_source::legacy_original),
	             "and the copy written for an older build is not read back as a source");
	assert_equal(false, unpacked->dates.has_source(prop::date_source::legacy_created),
	             "for either date");

	// A row written before the pack existed has no pack, and those same records are then the only
	// dates there are.
	metadata_packer legacy_only;
	legacy_only.write(prop::created_exif.id, df::date_t(2011, 2, 3));

	const auto from_legacy = std::make_shared<prop::item_metadata>();
	metadata_unpacker legacy_reader(legacy_only.cdata());
	legacy_reader.unpack(from_legacy);

	assert_equal(df::date_t(2011, 2, 3), from_legacy->dates.original(), "a pre-pack row still answers");
	assert_equal(true, from_legacy->dates.has_source(prop::date_source::legacy_original),
	             "at legacy authority, so the re-scan replaces it");
}

// A later release may append a field to a group record or to the pack's trailer. The stored body
// states how long each is, so this build steps over what it does not know and keeps the dates.
// Without that, a format change would cost every user a full re-index.
static void should_read_a_date_pack_written_by_a_later_release()
{
	constexpr uint8_t extra = 4;
	constexpr uint8_t stride = 18 + extra;
	constexpr uint8_t trailer = 8 + extra;

	df::blob body;
	body.push_back(9); // a pack version this build has never heard of
	body.push_back(1); // one group
	body.push_back(stride);
	body.push_back(trailer);

	const auto push = [&body](const auto v)
	{
		const auto* const src = std::bit_cast<const uint8_t*>(&v);
		body.insert(body.end(), src, src + sizeof(v));
	};

	push(static_cast<uint64_t>(prop::date_source::exif_original));
	push(df::date_t(2019, 5, 4, 9, 0, 0).to_int64());
	push(static_cast<int16_t>(prop::date_pack::no_offset));
	body.insert(body.end(), extra, 0x5a); // a per-group field this build cannot name

	push(static_cast<uint64_t>(prop::date_source::rip_date));
	body.insert(body.end(), extra, 0x5a); // a trailer field this build cannot name

	metadata_packer packer;
	packer.write_prop_id(prop::dates_packed.id);
	packer.write_len(body.size());
	packer._data.insert(packer._data.end(), body.begin(), body.end());
	packer.write(prop::rating.id, static_cast<int16_t>(3));

	const auto unpacked = std::make_shared<prop::item_metadata>();
	metadata_unpacker unpacker(packer.cdata());
	unpacker.unpack(unpacked);

	assert_equal(df::date_t(2019, 5, 4, 9, 0, 0), unpacked->dates.original(),
	             "a longer group record still yields its date");
	assert_equal(true, unpacked->dates.has_source(prop::date_source::rip_date),
	             "and a longer trailer still yields the overflow mask");
	assert_equal(static_cast<int16_t>(3), unpacked->rating,
	             "and the record after it is not knocked out of alignment");
}

// A pack can be structurally perfect and still restore nothing: a later release may name a date
// source this build has no member for, and every group in the body may carry only that. Recording
// "a pack was read" rather than "a date was restored" would then discard the plain date records
// written beside the pack for exactly this case, and the row would answer with no date at all -
// the same defect as a refused pack, one level further down.
static void should_fall_back_when_a_pack_names_no_source_this_build_knows()
{
	constexpr uint64_t unknown_source = 1ull << 40;

	df::blob body;
	body.push_back(2); // the pack version this build writes
	body.push_back(1); // one group
	body.push_back(18); // this build's own group stride
	body.push_back(8); // and its own trailer

	const auto push = [&body](const auto v)
	{
		const auto* const src = std::bit_cast<const uint8_t*>(&v);
		body.insert(body.end(), src, src + sizeof(v));
	};

	push(unknown_source);
	push(df::date_t(2031, 7, 1, 12, 0, 0).to_int64());
	push(static_cast<int16_t>(prop::date_pack::no_offset));
	push(static_cast<uint64_t>(0)); // no overflow

	metadata_packer packer;
	// Production order: the copies an older build reads are written before the pack.
	packer.write(prop::created_exif.id, df::date_t(2011, 2, 3));
	packer.write_prop_id(prop::dates_packed.id);
	packer.write_len(body.size());
	packer._data.insert(packer._data.end(), body.begin(), body.end());

	const auto unpacked = std::make_shared<prop::item_metadata>();
	metadata_unpacker unpacker(packer.cdata());
	unpacker.unpack(unpacked);

	assert_equal(df::date_t(2011, 2, 3), unpacked->dates.original(),
	             "the row answers from the record written beside the pack");
	assert_equal(true, unpacked->dates.has_source(prop::date_source::legacy_original),
	             "at legacy authority, so a re-scan still replaces it");
}

static void should_store_webservice_results()
{
	const auto index_path = _temps.next_path();

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	constexpr auto key = "key   xxxxxxxxxx";
	const auto value = long_text;
	db.web_service_cache(key, value);
	const auto result = db.web_service_cache(key);

	assert_equal(result, value, "web_service_cache");
}

static void should_bound_webservice_cache()
{
	const auto index_path = _temps.next_path();

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	for (int i = 0; i <= 1000; ++i)
	{
		db.web_service_cache(std::format("key{}", i), "value");
	}

	assert_equal(true, db.web_service_cache("key0").empty(), "oldest web cache entry evicted");
	assert_equal("value", db.web_service_cache("key1000"), "newest web cache entry retained");
}

static void should_index(shared_test_context& stc)
{
	stc.lazy_load_index();

	assert_equal(expected_cached_item_count, stc.test_index.stats.media_item_count, "cached item count");

	const auto expected_md = expected_test_jpg();
	expected_md->file_name = "Test.jpg"_c;
	assert_metadata(*expected_md, *metadata_from_cache(stc.test_index, test_files_folder.combine_file("Test.jpg")),
	                "Test.jpg");

	const auto actual = metadata_from_cache(stc.test_index, test_files_folder.combine_file("Gherkin.CR2"));
	assert_equal("Canon", actual->camera_manufacturer, "camera_manufacturer");
	assert_equal("United Kingdom", actual->location_country, "location_country");
	assert_equal("© Mark Ridgwell", actual->copyright_notice, "copyright_notice");
	assert_equal("\"Mark Ridgwell\"", actual->copyright_creator, "copyright_creator");
}

static void should_parse_roots(shared_test_context& stc)
{
	df::index_roots roots1;
	parse_more_folders(roots1, test_files_folder.text());

	assert_equal(0_z, roots1.files.size(), "parsed files");
	assert_equal(0_z, roots1.excludes.size(), "parsed excludes");
	assert_equal(1_z, roots1.folders.size(), "parsed folder");
	assert_equal(test_files_folder.text(), roots1.folders.begin()->text(), "parsed folder");

	df::index_roots roots2;
	const auto exclude_files_folder = test_files_folder.combine("excluded1");
	parse_more_folders(roots2, std::format(" - {}\n{}", exclude_files_folder.text(), test_files_folder.text()));

	assert_equal(0_z, roots2.files.size(), "parsed files");
	assert_equal(1_z, roots2.excludes.size(), "parsed excludes");
	assert_equal(1_z, roots2.folders.size(), "parsed folder");
	assert_equal(test_files_folder.text(), roots2.folders.begin()->text(), "parsed folder");
	assert_equal(exclude_files_folder.text(), roots2.excludes.begin()->text(), "parsed exclude");

	df::index_roots roots3;
	parse_more_folders(roots3, std::format("- secret\n{}\n -exclude*", test_files_folder.text()));

	assert_equal(0_z, roots3.files.size(), "parsed files");
	assert_equal(0_z, roots3.excludes.size(), "parsed excludes");
	assert_equal(1_z, roots3.folders.size(), "parsed folder");
	assert_equal(2_z, roots3.exclude_wildcards.size(), "parsed exclude wildcards");

	const std::vector<str::cached> exclude_wildcards(roots3.exclude_wildcards.begin(), roots3.exclude_wildcards.end());

	assert_equal(test_files_folder.text(), roots3.folders.begin()->text(), "parsed folder");
	assert_equal("exclude*", exclude_wildcards[0], "parsed exclude");
	assert_equal("secret", exclude_wildcards[1], "parsed exclude");
}

static void should_parse_drive_label_roots(shared_test_context& stc)
{
	// A device label (volume name) entered in the collection list should resolve
	// to the matching drive by its volume label - not by its drive letter.
	//
	// What a mounted volume is called differs by platform; that it is found by its label does not.
	const auto drive_path = df::windows_path_semantics ? "X:\\" : "/mnt/diffractor-test";

	platform::drives drives;

	platform::drive_t d;
	d.name = drive_path;
	d.vol_name = "DiffractorTestLabel";
	drives.emplace_back(d);

	// A label matching the drive's volume name resolves to that drive's path
	// (rather than being stored as the bare label text).
	df::index_roots roots;
	parse_more_folders(roots, "DiffractorTestLabel", drives);
	assert_equal(1_z, roots.folders.size(), "matching label count");
	assert_equal(drive_path, roots.folders.begin()->text(), "device label resolved to drive path");
}

static void should_apply_collection_exclusions_before_membership_shortcuts()
{
	const auto root = _temps.next_folder("excluded-membership");
	const auto excluded = root.combine("secret");
	const auto sibling = root.combine("public");
	platform::create_folder(excluded);
	platform::create_folder(sibling);
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), excluded.combine_file("hidden.jpg"), false, false);
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), sibling.combine_file("shown.jpg"), false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::index_roots roots;
	roots.folders.emplace(root);
	roots.excludes.emplace(excluded);
	index.index_roots(roots);

	assert_equal(false, index.is_in_collection(excluded), "excluded immediate child is not a member");
	assert_equal(false, index.is_in_collection(excluded.combine("leaf")), "excluded descendants are not members");
	assert_equal(true, index.is_in_collection(sibling), "included siblings still use the root shortcut");

	df::index_roots nested_roots;
	nested_roots.folders.emplace(root);
	nested_roots.folders.emplace(excluded.combine("photos"));
	nested_roots.excludes.emplace(excluded);
	index.index_roots(nested_roots);
	assert_equal(true, index.is_in_collection(excluded.combine("photos")),
	             "a nested declared root is not hidden by an ancestor exclude");

	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	assert_equal(1, count_search_results(index, "@photo"), "recursive indexing does not re-enrol an excluded child");
}

static void should_restore_cached_offline_collection_descendant_membership()
{
	const auto root = _temps.folder().combine("missing-parent").combine("offline-cache-root");
	const auto leaf = root.combine("fileless").combine("leaf");
	const auto excluded = root.combine("secret").combine("leaf");
	const auto wildcard_excluded = root.combine("@eaDir").combine("leaf");

	db_items_t member_items;
	db_item_t member;
	member.path = str::cache("cached.jpg");
	member.metadata_scanned = df::date_t(2026, 1, 1);
	member.metadata = std::make_shared<prop::item_metadata>();
	member.metadata->file_name = member.path;
	member_items.emplace_back(std::move(member));

	db_items_t excluded_items;
	db_item_t hidden;
	hidden.path = str::cache("hidden.jpg");
	hidden.metadata_scanned = df::date_t(2026, 1, 1);
	hidden.metadata = std::make_shared<prop::item_metadata>();
	hidden.metadata->file_name = hidden.path;
	excluded_items.emplace_back(std::move(hidden));

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	db_items_t no_root_items;
	index.merge_folder(root, no_root_items);
	index.merge_folder(leaf, member_items);
	index.merge_folder(excluded, excluded_items);
	index.merge_folder(wildcard_excluded, excluded_items);
	index.cache_load_complete();

	df::index_roots roots;
	roots.folders.emplace(root);
	roots.excludes.emplace(root.combine("secret"));
	roots.exclude_wildcards.emplace("@eaDir"_c);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	assert_equal(1, count_search_results(index, "@photo"),
	             "cached descendants below fileless offline ancestors stay in the collection");
	assert_equal(true, index.is_init_complete(), "retaining offline descendants does not make discovery incomplete");
}

static void should_not_restore_deleted_cached_collection_folders()
{
	const auto root = _temps.next_folder("deleted-cache-root");
	const auto deleted = root.combine("deleted");
	const auto kept = root.combine("kept");
	platform::create_folder(kept);
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), kept.combine_file("kept.jpg"), false, false);

	db_items_t cached_items;
	db_item_t cached;
	cached.path = str::cache("stale.jpg");
	cached.metadata_scanned = df::date_t(2026, 1, 1);
	cached.metadata = std::make_shared<prop::item_metadata>();
	cached.metadata->file_name = cached.path;
	cached_items.emplace_back(std::move(cached));

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	index.merge_folder(deleted, cached_items);
	index.cache_load_complete();

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	assert_equal(1, count_search_results(index, "@photo"),
	             "cached descendants are restored only below folders whose enumeration failed");
}

// The startup query validates the folder on screen before the database cache reaches it, so the cache
// merges into a node that already holds each file's modified time. A row scanned before the file last
// changed holds hashes of bytes that are gone: adopting them reported an edited picture as identical to
// an untouched copy, and the row kept them for every launch after.
static void should_not_adopt_cached_hashes_older_than_the_file()
{
	const auto root = _temps.next_folder("merge-after-validation");
	const auto changed = root.combine_file("changed.jpg");
	const auto unchanged = root.combine_file("unchanged.jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), changed, false, false);
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), unchanged, false, false);

	const auto db_path = _temps.next_path();
	null_async_strategy as;
	location_cache locations;

	{
		index_state index(as, locations);
		database db(index);
		db.open(db_path.folder(), db_path.file_name_without_extension());

		// One row scanned long before the file's modified time and one scanned after it.
		std::deque<item_db_write> rows;
		const auto add_row = [&rows](const df::file_path path, const df::date_t scanned, const uint32_t crc)
		{
			item_db_write row;
			row.path = path;
			row.md = std::make_shared<prop::item_metadata>();
			row.crc32c = crc;
			row.metadata_scanned = scanned;
			rows.emplace_back(std::move(row));
		};
		add_row(changed, df::date_t(2000, 1, 1, 0, 0, 0), 0x1111u);
		add_row(unchanged, df::date_t(2100, 1, 1, 0, 0, 0), 0x2222u);
		db.perform_writes(std::move(rows));

		// The query for the folder on screen gets there first; the cache load arrives after it.
		index.validate_folder(root, true, platform::now());
		db.load_index_values();

		assert_equal(0u, index.find_item(changed).crc32c.load(),
		             "a row scanned before the file changed lends it no checksum");
		assert_equal(0x2222u, index.find_item(unchanged).crc32c.load(),
		             "a row scanned since the file changed keeps its checksum");

		db.perform_writes();
		db.close();
	}

	index_state reloaded(as, locations);
	database db(reloaded);
	db.open(db_path.folder(), db_path.file_name_without_extension());

	assert_equal(0u, reloaded.find_item(changed).crc32c.load(), "the stale checksum is cleared from the row");
	assert_equal(0x2222u, reloaded.find_item(unchanged).crc32c.load(), "the current checksum is kept");
}

static void should_drop_deleted_declared_root_cache()
{
	const auto parent = _temps.next_folder("deleted-root-parent");
	const auto root = parent.combine("deleted-root");
	const auto leaf = root.combine("leaf");

	db_items_t cached_items;
	db_item_t cached;
	cached.path = str::cache("stale.jpg");
	cached.metadata_scanned = df::date_t(2026, 1, 1);
	cached.metadata = std::make_shared<prop::item_metadata>();
	cached.metadata->file_name = cached.path;
	cached_items.emplace_back(std::move(cached));

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	index.merge_folder(root, db_items_t{});
	index.merge_folder(leaf, cached_items);
	index.cache_load_complete();

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	assert_equal(0, count_search_results(index, "@photo"),
	             "a deleted online root does not keep cached descendants searchable");

	const auto db_path = _temps.next_path();
	{
		index_state writer_index(as, locations);
		database db(writer_index);
		db.open(db_path.folder(), db_path.file_name_without_extension());
		const auto live_root = _temps.next_folder("deleted-root-db-parent").combine("deleted-root-db");
		const auto live_leaf = live_root.combine("leaf");
		platform::create_folder(live_leaf);
		platform::copy_file(test_files_folder.combine_file("Test.jpg"), live_leaf.combine_file("stale.jpg"), false,
		                    false);
		df::index_roots live_roots;
		live_roots.folders.emplace(live_root);
		writer_index.index_roots(live_roots);
		writer_index.index_folders(test_token);
		writer_index.scan_uncached(test_token);
		db.perform_writes();
		assert_equal(1, count_search_results(writer_index, "@photo"), "database cleanup setup indexed one row");
		db.close();

		const auto db_file = df::file_path(db_path.folder(), db_path.file_name_without_extension(), ".db");
		sqlite3* handle = nullptr;
		if (sqlite3_open(db_file.str().c_str(), &handle) != SQLITE_OK)
			throw test_assert_exception("Failed to age deleted root database row"s);
		{
			const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> raw(handle, sqlite3_close);
			if (sqlite3_exec(raw.get(), "UPDATE item_properties SET last_indexed = 0", nullptr, nullptr, nullptr) !=
				SQLITE_OK)
			{
				throw test_assert_exception("Failed to age deleted root database row"s);
			}
		}

		db.open(db_path.folder(), db_path.file_name_without_extension());

		platform::delete_items({}, {live_root}, false);
		writer_index.index_folders(test_token);
		writer_index.scan_uncached(test_token);
		db.clean(writer_index.all_indexed_items());
	}

	{
		index_state reloaded_index(as, locations);
		database db(reloaded_index);
		db.open(db_path.folder(), db_path.file_name_without_extension());
		db.load_index_values();
		assert_equal(0_z, reloaded_index.all_indexed_items().size(),
		             "cleaned deleted-root rows do not reload next launch");
	}
}

static void should_report_incomplete_collection_discovery()
{
	const auto root = _temps.next_folder("incomplete-discovery");
	platform::create_folder(root.combine("child"));

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	index.cache_load_complete();

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);

	std::atomic_int scan_version = 1;
	const df::cancel_token canceled(scan_version);
	const df::cancel_token current(scan_version);
	index.index_folders(canceled);
	index.scan_uncached(test_token);

	assert_equal(true, index.is_init_complete(), "workers may start after an incomplete discovery");

	const auto outside = std::make_shared<df::item_element>(root.combine_file("absent.jpg"), make_index_file_info({}));
	index.queue_update_presence(df::item_set({outside}));
	assert_equal(static_cast<int>(item_presence::unknown), static_cast<int>(outside->presence()),
	             "incomplete discovery does not publish an absence");

	platform::copy_file(test_files_folder.combine_file("Test.jpg"), root.combine_file("photo.jpg"), false, false);
	index_state paused(as, locations);
	paused.cache_load_complete();
	paused.index_roots(roots);
	paused.index_folders(test_token, false);
	assert_equal(true, paused.is_init_complete(), "skipped collection discovery releases on-demand workers");
	assert_equal(0_z, paused.all_indexed_items().size(), "disabled discovery does not enumerate collection files");
	paused.queue_update_presence(df::item_set({outside}));
	assert_equal(static_cast<int>(item_presence::unknown), static_cast<int>(outside->presence()),
	             "skipped discovery does not claim fresh absence evidence");
	paused.index_folders(test_token);
	assert_equal(1_z, paused.all_indexed_items().size(), "normal discovery still enumerates the collection");
}

static void should_invalidate_metadata_when_sidecar_identity_changes()
{
	const auto temp_folder = _temps.next_folder("sidecar-identity");
	const auto file_path = _temps.next_path_in(temp_folder, ".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, false);
	const auto xmp_path = file_path.extension(".xmp");
	const std::string xmp_packet =
		"<?xpacket begin=\"\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
		"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description/></rdf:RDF>"
		"</x:xmpmeta><?xpacket end=\"w\"?>";

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	index.validate_folder(temp_folder, true, platform::now());

	df::blob_save_to_file(df::blob(xmp_packet.begin(), xmp_packet.end()), xmp_path);
	index.validate_folder(temp_folder, true, platform::now());
	auto with_sidecar = index.find_item(file_path);
	auto with_sidecar_md = with_sidecar.metadata.load();
	assert_equal(true, with_sidecar_md != nullptr, "sidecar association metadata exists");
	assert_equal(xmp_path.name(), with_sidecar_md->xmp, "xmp sidecar is selected");
	assert_equal(false, with_sidecar.metadata_scanned.load().is_valid(),
	             "adding a sidecar invalidates effective metadata");

	platform::delete_items({xmp_path}, {}, false);
	index.validate_folder(temp_folder, true, platform::now());
	auto without_sidecar = index.find_item(file_path);
	auto without_sidecar_md = without_sidecar.metadata.load();
	assert_equal(true, without_sidecar_md != nullptr, "sidecar removal keeps metadata packet");
	assert_equal(""sv, without_sidecar_md->sidecars.sv(), "last sidecar association is cleared");
	assert_equal(""sv, without_sidecar_md->xmp.sv(), "last xmp association is cleared");
	assert_equal(false, without_sidecar.metadata_scanned.load().is_valid(),
	             "removing a sidecar invalidates effective metadata");
}

extern df::cancel_token make_scan_uncached_token_for_index_workers();
extern df::cancel_token make_index_update_token_for_worker_tests();

static void should_not_cancel_replacement_collection_walk_when_scan_starts()
{
	const auto cancelled_walk = make_index_update_token_for_worker_tests();
	const auto replacement_walk = make_index_update_token_for_worker_tests();
	assert_equal(true, cancelled_walk.is_cancelled(), "replacement walk superseded the prior walk");

	const auto scan_token = make_scan_uncached_token_for_index_workers();
	(void)scan_token;

	assert_equal(false, replacement_walk.is_cancelled(),
	             "starting scan_uncached must not cancel the replacement collection walk");
}

// Verifies the item-level reload predicate independently of the scanner and database. Thumbnail
// loading is deferred until the DB lookup completes; after that, a missing/unstamped thumbnail is
// eligible, a thumbnail stamped at the file modification time is current, and a later file change
// makes that same thumbnail stale.
static void should_not_reload_thumb_when_valid()
{
	const auto load_path = test_files_folder.combine_file("Test.jpg");

	const df::date_t date(1972, 5, 25);
	const df::date_t date2(1972, 5, 26);

	files ff;
	const auto loaded = ff.load(load_path, false);

	const auto i_local = std::make_shared<df::item_element>(load_path, make_index_file_info(date));
	assert_equal(false, i_local->should_load_thumbnail(), "should not load by default");

	i_local->begin_db_thumbnail_query();
	assert_equal(true, i_local->should_load_thumbnail(), "should load after db load");

	i_local->thumbnail(loaded.i, nullptr);
	assert_equal(true, i_local->should_load_thumbnail(), "should load without timestamp");

	i_local->thumbnail(loaded.i, nullptr, date);
	assert_equal(false, i_local->should_load_thumbnail(), "should not load a current timestamped thumbnail");

	i_local->update(load_path, make_index_file_info(date2));
	assert_equal(true, i_local->should_load_thumbnail(), "should if date changes");

	i_local->update(load_path, make_index_file_info(date.add_day(-1)));
	assert_equal(true, i_local->should_load_thumbnail(), "should if date changes backward");
}

static void should_reuse_persisted_hover_thumbnail_until_video_changes()
{
	const auto index_path = _temps.next_path();
	const auto image_path = test_files_folder.combine_file("Test.jpg");
	const df::file_path video_path(test_files_folder, "hover-preview.mp4");
	const df::date_t modified(2026, 7, 31);

	files ff;
	const auto loaded = ff.load(image_path, false);
	assert_equal(true, is_valid(loaded.i), "hover thumbnail test image");

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	index.save_thumbnail(video_path, loaded.i, {}, modified);
	db.perform_writes();

	const auto reloaded_item = std::make_shared<df::item_element>(video_path, make_index_file_info(modified));
	df::item_set items;
	reloaded_item->add_to(items);
	reloaded_item->begin_db_thumbnail_query();
	db.load_thumbnails(index, database::make_thumbnail_requests(items));

	assert_equal(true, reloaded_item->has_thumb(), "hover thumbnail survives database round trip");
	assert_equal(false, reloaded_item->should_load_thumbnail(), "unchanged video reuses hovered thumbnail");

	reloaded_item->update(video_path, make_index_file_info(modified.add_day(1)));
	assert_equal(true, reloaded_item->should_load_thumbnail(), "modified video invalidates hovered thumbnail");

	reloaded_item->update(video_path, make_index_file_info(modified.add_day(-1)));
	assert_equal(true, reloaded_item->should_load_thumbnail(), "older modified date invalidates hovered thumbnail");
}

// Exercises the complete two-stage thumbnail lifecycle. The metadata scan may cache a provisional
// embedded thumbnail without a current thumbnail timestamp. The visible-item scan then generates
// the full thumbnail, stores it with its scan timestamp, and a fresh item must reuse that DB row
// without opening the source again. The final stale control proves the test detects a required
// regeneration when the source modification time advances.
static void should_reload_thumb_after_scan()
{
	files ff;
	const auto index_path = _temps.next_path();
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());
	build_index(index, db);

	auto path_test = df::file_path(test_files_folder, "Test.jpg");
	const auto path_sony = df::file_path(test_files_folder, "Sony.JPG");

	const auto test_item = load_item(index, path_test, false);
	const auto sony_item = load_item(index, path_sony, false);

	assert_equal(false, test_item->should_load_thumbnail(), "should_load_thumbnail for test.jpg");
	assert_equal(false, sony_item->should_load_thumbnail(), "should_load_thumbnail for sony.jpg");

	df::item_set items;
	items._items = {test_item, sony_item};

	// Stage 1: a metadata refresh persists no thumbnail at all, so the item is still waiting on the
	// visible-item pass for one.
	const auto metadata_refreshed_initially = index.scan_items(items, false, false, false, false, test_token, true);
	db.perform_writes();
	assert_equal(true, metadata_refreshed_initially, "forced metadata refresh should require regrouping after scan");

	assert_equal(false, test_item->should_load_thumbnail(), "should_load_thumbnail for test.jpg");
	assert_equal(false, sony_item->should_load_thumbnail(), "should_load_thumbnail for sony.jpg");

	items.for_all([](const auto& item) { item->begin_db_thumbnail_query(); });
	items.for_all([](const auto& item) { item->begin_db_thumbnail_query(); });
	db.load_thumbnails(index, database::make_thumbnail_requests(items));

	assert_equal(true, test_item->should_load_thumbnail(),
	             "should_load_thumbnail for test.jpg after db load_thumbnails");
	assert_equal(true, sony_item->should_load_thumbnail(),
	             "should_load_thumbnail for sony.jpg after db load_thumbnails");

	// Stage 2: loading thumbnails from the full files is what generates and stores the images, with
	// scan timestamps that are current relative to the source modification times.
	const auto metadata_refreshed_for_thumbnails = index.scan_items(items, true, false, false, false, test_token);
	db.perform_writes();
	assert_equal(false, metadata_refreshed_for_thumbnails,
	             "current metadata should not force regrouping after thumbnail generation");

	assert_equal(false, test_item->should_load_thumbnail(),
	             "should_load_thumbnail for test.jpg after index load_thumbnails");
	assert_equal(false, sony_item->should_load_thumbnail(),
	             "should_load_thumbnail for sony.jpg after index load_thumbnails");

	const auto reloaded_item = std::make_shared<df::item_element>(path_test,
	                                                              make_index_file_info(test_item->file_modified()));
	df::item_set reloaded_items;
	reloaded_item->add_to(reloaded_items);
	reloaded_items.for_all([](const auto& item) { item->begin_db_thumbnail_query(); });
	db.load_thumbnails(index, database::make_thumbnail_requests(reloaded_items));

	// Simulate a later application session: DB hydration alone must make the generated thumbnail
	// current, and the real conditional scan path must perform no thumbnail write.
	assert_equal(true, reloaded_item->has_thumb(), "generated thumbnail should survive database round trip");
	assert_equal(false, reloaded_item->should_load_thumbnail(),
	             "generated database thumbnail should not reload from full file");

	const auto thumbs_saved_before_valid_scan = index.stats.thumbs_saved;
	index.scan_items(reloaded_items, true, false, true, false, test_token);
	db.perform_writes();
	assert_equal(thumbs_saved_before_valid_scan, index.stats.thumbs_saved,
	             "valid database thumbnail should skip full-file thumbnail generation");

	// Sensitivity control: advancing the source timestamp must make the cache stale and produce
	// exactly one replacement thumbnail through the same conditional scan path.
	reloaded_item->update(path_test, make_index_file_info(reloaded_item->thumbnail_timestamp().add_day(1)));
	assert_equal(true, reloaded_item->should_load_thumbnail(), "modified file should make database thumbnail stale");
	const auto metadata_refreshed_after_modify =
		index.scan_items(reloaded_items, true, false, true, false, test_token);
	db.perform_writes();
	assert_equal(false, metadata_refreshed_after_modify,
	             "thumbnail staleness alone should not force regrouping after regeneration");
	assert_equal(thumbs_saved_before_valid_scan + 1, index.stats.thumbs_saved,
	             "stale database thumbnail should regenerate from the full file");
}

// The cost assertion behind the write-suppression design: a metadata-only edit must read the file
// exactly once, through the write's own coherent handle, and must leave the cached index record
// current. Asserting on the cached record matters more than asserting on the item: the background
// safety net refreshes the folder from the filesystem, so a stale record is masked whenever that
// refresh happens to run, and costs a re-read and a re-decode whenever it does not.
static void should_not_reread_after_metadata_write()
{
	// A private folder: the shared suite temp folder would drag every other test's files into the
	// scan and make this test's cost depend on test order.
	const auto temp_folder = _temps.next_folder("write-suppression");
	const auto file_path = _temps.next_path_in(temp_folder, ".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto item = load_item(index, file_path, true);
	assert_equal(false, item->should_load_thumbnail(), "thumbnail is current before the write");
	assert_equal(false, index.needs_scan(item), "record is current before the write");

	const auto modified_before = item->file_modified();
	item->retain_thumbnail_across_next_write();

	const auto request = index_state::make_scan_request(item, true, false);
	const auto xmp = detect_xmp_sidecar(file_path);

	metadata_edits edits;
	edits.rating = 3;

	files ff;
	const auto scans_before = df::file_perf.scans.load();
	const auto result = ff.update(file_path, edits, {}, {}, false, xmp,
	                              index_state::make_rescan_spec(request, xmp, false, false));

	assert_equal(true, result.success(), "the write succeeds");
	assert_equal(true, result.scanned, "the write scans back through its own handle");
	assert_equal(true, result.coherent, "the write-back scan is coherent");
	assert_equal(scans_before + 1, df::file_perf.scans.load(), "the write reads the file exactly once");

	const auto force = index.apply_write_scan(request, result);
	assert_equal(false, force, "a coherent write does not force a rescan");

	const auto written_modified = df::date_t(result.modified);
	assert_equal(true, written_modified == index.find_item(file_path).file_modified.load(),
	             "the cached index record carries the written modified time");
	assert_equal(true, modified_before < item->file_modified(), "the item carries the written modified time");
	assert_equal(false, item->should_load_thumbnail(), "the written item does not re-request its thumbnail");
	assert_equal(false, index.needs_scan(item), "the cached record does not report needing a scan");
	assert_equal(scans_before + 1, df::file_perf.scans.load(), "publishing the write costs no extra read");
}

static void should_clear_hashes_after_coherent_content_change()
{
	const auto index_path = _temps.next_path();
	const auto temp_folder = _temps.next_folder("coherent-hashes");
	const auto file_path = _temps.next_path_in(temp_folder, ".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	const auto item = load_item(index, file_path, false);
	const auto old_modified = item->file_modified();
	const index_file_revision old_revision{old_modified, item->file_size().to_int64()};
	constexpr uint32_t old_crc = 0x12345678u;
	constexpr crypto::phash_rotations old_phash{0x1111ull, 0x2222ull, 0x3333ull, 0x4444ull};
	index.save_crc(file_path, old_revision, old_crc);
	index.save_phash(file_path, old_revision, old_phash);
	db.perform_writes();

	const auto request = index_state::make_scan_request(item, false, false);
	file_scan_result sr;
	sr.success = true;

	index.apply_scan_now(request, sr, true, old_modified.add_day(1));
	db.perform_writes();

	const auto changed = index.find_item(file_path);
	assert_equal(0u, changed.crc32c.load(), "same-size coherent write clears stale CRC");
	assert_equal(false, changed.phash.load() != nullptr, "same-size coherent write clears stale phash");

	index_state reloaded(as, locations);
	database db2(reloaded);
	db2.open(index_path.folder(), index_path.file_name_without_extension());
	db2.load_index_values();
	const auto persisted = reloaded.find_item(file_path);
	assert_equal(0u, persisted.crc32c.load(), "cleared CRC persists");
	assert_equal(false, persisted.phash.load() != nullptr, "cleared phash persists");

	file_scan_result fresh;
	fresh.success = true;
	fresh.crc32c = 0x87654321u;
	index.apply_scan_now(request, fresh, true, old_modified.add_day(2));
	db.perform_writes();
	assert_equal(fresh.crc32c, index.find_item(file_path).crc32c.load(), "fresh CRC evidence is restored");
}

// Two batches can hold one path at once, so a claim is counted rather than a set membership. The
// first release must not open the file up while the second batch's write is still queued.
static void should_count_overlapping_write_claims()
{
	const auto temp_folder = _temps.next_folder("write-claims");
	const auto file_path = _temps.next_path_in(temp_folder, ".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto item = load_item(index, file_path, true);
	df::item_set items;
	items.add(item);

	const std::vector claimed{file_path};
	index.claim_for_write(claimed);
	index.claim_for_write(claimed);

	index.release_write_claim(claimed);

	const auto scans_after_first_release = df::file_perf.scans.load();
	index.queue_scan_modified_items(items, true);
	assert_equal(scans_after_first_release, df::file_perf.scans.load(),
	             "a still-claimed path must stay deferred after an overlapping batch releases");

	index.release_write_claim(claimed);
	assert_equal(true, scans_after_first_release < df::file_perf.scans.load(),
	             "the deferred scan runs once the last claim is released");
}

// Only selector folders are live-watched, so a search that names no folder - related items,
// duplicates, a tag, a date - has nothing watching it. Deleting from one of those views has to tell
// the index itself, or the view keeps listing a file that is gone.
// The index is a cache, so an in-app change has to both correct it and ask for the search to be run
// again. Asking unconditionally would re-open the search on every folder walked, so the request is
// tied to a folder actually differing.
static void should_request_a_re_query_only_when_a_folder_changed(shared_test_context& stc)
{
	const df::file_path source(test_files_folder, "Test.jpg");
	const auto temp_folder = _temps.next_folder("requery");
	const auto removed = _temps.next_path_in(temp_folder, ".jpg");
	platform::copy_file(source, removed, false, false);

	deferred_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::index_roots roots;
	roots.folders.emplace(temp_folder);
	index.index_roots(roots);
	index.index_folders(test_token);

	df::unique_folders touched;
	touched.emplace(temp_folder);

	// Nothing has changed on disk, so nothing needs re-running.
	index.queue_validate_changed_folders(touched);
	assert_equal(true, as.run_next(async_queue::scan_folder), "validate ran");
	assert_equal(false, as.was_invalidated(view_invalid::refresh_items), "an unchanged folder asks for nothing");

	assert_equal(true, platform::delete_items({removed}, {}, false).success(), "delete");

	index.queue_validate_changed_folders(std::move(touched));
	assert_equal(true, as.run_next(async_queue::scan_folder), "validate ran");
	assert_equal(true, as.was_invalidated(view_invalid::refresh_items), "a changed folder asks for the re-query");
}

static void should_drop_deleted_items_from_a_search_with_no_folder(shared_test_context& stc)
{
	const df::file_path source(test_files_folder, "Test.jpg");
	const auto temp_folder = _temps.next_folder("deleted");
	const auto kept = _temps.next_path_in(temp_folder, ".jpg");
	const auto removed = _temps.next_path_in(temp_folder, ".jpg");

	platform::copy_file(source, kept, false, false);
	platform::copy_file(source, removed, false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	auto cache_path = _temps.next_path();
	database db(index);
	db.open(cache_path.folder(), cache_path.file_name_without_extension());

	df::index_roots roots;
	roots.folders.emplace(temp_folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	// No selector: this is the shape every relation, duplicate and tag search has.
	const auto search = df::search_t::parse("@photo");
	assert_equal(2, count_search_results(index, search), "both copies are listed");

	assert_equal(true, platform::delete_items({removed}, {}, false).success(), "delete");

	df::unique_folders touched;
	touched.emplace(removed.folder());
	index.queue_validate_changed_folders(std::move(touched));

	assert_equal(1, count_search_results(index, search), "the deleted copy is gone from the list");
}

// The index is flat and keyed by folder path, so erasing a removed folder left everything beneath
// it indexed: the files of a deleted subtree stayed searchable and counted for the whole session.
static void should_drop_descendants_of_a_deleted_folder(shared_test_context& stc)
{
	const df::file_path source(test_files_folder, "Test.jpg");
	const auto root = _temps.next_folder("deleted-tree");
	const auto branch = root.combine("branch");
	const auto leaf = branch.combine("leaf");
	platform::create_folder(leaf);

	const auto in_root = root.combine_file("root.jpg");
	const auto in_branch = branch.combine_file("branch.jpg");
	const auto in_leaf = leaf.combine_file("leaf.jpg");

	for (const auto& path : {in_root, in_branch, in_leaf})
	{
		assert_equal(true, platform::copy_file(source, path, false, false).success(), "copy fixture");
	}

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	auto cache_path = _temps.next_path();
	database db(index);
	db.open(cache_path.folder(), cache_path.file_name_without_extension());

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	const auto search = df::search_t::parse("@photo");
	assert_equal(3, count_search_results(index, search), "every copy in the tree is listed");

	assert_equal(true, platform::delete_items({}, {branch}, false).success(), "delete the branch");
	assert_equal(false, platform::exists(branch), "the branch is gone from disk");

	df::unique_folders touched;
	touched.emplace(root);
	index.queue_validate_changed_folders(std::move(touched));

	assert_equal(1, count_search_results(index, search),
	             "only the file outside the deleted branch survives");
}

// The wildcard names the files being looked for, not the folders they are under. Gating the descent
// on it left a recursive "*.jpg" searching only folders that were themselves called *.jpg, so every
// nested match was missed.
static void should_search_a_recursive_wildcard_through_subfolders(shared_test_context& stc)
{
	const df::file_path source(test_files_folder, "Test.jpg");
	const auto root = _temps.next_folder("recursive-wildcard");
	const auto nested = root.combine("pictures");
	platform::create_folder(nested);

	assert_equal(true, platform::copy_file(source, root.combine_file("top.jpg"), false, false).success(), "copy top");
	assert_equal(true, platform::copy_file(source, nested.combine_file("nested.jpg"), false, false).success(),
	             "copy nested");
	assert_equal(true, platform::copy_file(source, nested.combine_file("nested.png"), false, false).success(),
	             "copy other extension");

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);

	const auto recursive = df::search_t().add_selector(df::item_selector(root, true, "*.jpg"));
	assert_equal(2, count_search_results(index, recursive), "a recursive wildcard reaches nested folders");

	const auto shallow = df::search_t().add_selector(df::item_selector(root, false, "*.jpg"));
	assert_equal(1, count_search_results(index, shallow), "a shallow wildcard stays in its folder");
}

#ifndef _WIN32
static void should_search_a_recursive_wildcard_without_following_directory_symlinks(shared_test_context& stc)
{
	const df::file_path source_file(test_files_folder, "Test.jpg");
	const auto root = _temps.next_folder("recursive-wildcard-symlink");
	const auto real = root.combine("real");
	const auto link = root.combine("link");
	platform::create_folder(real);

	assert_equal(true, platform::copy_file(source_file, real.combine_file("nested.jpg"), false, false).success(),
	             "copy nested");
	std::filesystem::create_directory_symlink(platform::to_stream_path(real), platform::to_stream_path(link));

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);

	const auto recursive = df::search_t().add_selector(df::item_selector(root, true, "*.jpg"));
	assert_equal(1, count_search_results(index, recursive), "a recursive wildcard does not descend a directory symlink");
}
#endif

static void should_detect_duplicates(shared_test_context& stc)
{
	files ff;
	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	auto cache_path = _temps.next_path();
	database db(index);
	db.open(cache_path.folder(), cache_path.file_name_without_extension());
	build_index(index, db);

	const auto path1 = df::file_path(test_files_folder, "Test.jpg");
	const auto path2 = df::file_path(test_files_folder, "Test90.jpg");
	const auto path3 = df::file_path(test_files_folder, "Test180.jpg");
	const auto path4 = df::file_path(test_files_folder, "Test270.jpg");
	const auto path5 = df::file_path(test_files_folder, "Small.jpg");
	const auto path_sony = df::file_path(test_files_folder, "Sony.JPG");

	const auto test_item1 = std::make_shared<df::item_element>(path1, index.find_item(path1));
	const auto test_item2 = std::make_shared<df::item_element>(path2, index.find_item(path2));
	const auto test_item3 = std::make_shared<df::item_element>(path3, index.find_item(path3));
	const auto test_item4 = std::make_shared<df::item_element>(path4, index.find_item(path4));
	const auto test_item5 = std::make_shared<df::item_element>(path5, index.find_item(path5));
	const auto sony_item = std::make_shared<df::item_element>(path_sony, index.find_item(path_sony));

	df::item_set items({test_item1, test_item2, test_item3, test_item4, test_item5, sony_item});
	items.for_all([](const auto& item) { item->begin_db_thumbnail_query(); });
	db.load_thumbnails(index, database::make_thumbnail_requests(items));

	index.scan_item(test_item1, true, false);
	index.scan_item(test_item2, true, false);
	index.scan_item(test_item3, true, false);
	index.scan_item(test_item4, true, false);
	index.scan_item(test_item5, true, false);
	index.scan_item(sony_item, true, false);

	index.update_predictions();

	// Small.jpg is Test.jpg resized, and Test90/180/270 are the same picture turned. None share a
	// name, size or CRC with the original, so the perceptual stage is the only thing that can see
	// them. A quarter turn is a common grading step, so a turned copy is claimed as a copy.
	const auto test_group = index.find_item(test_item1->path()).duplicates.load();
	const auto small_group = index.find_item(test_item5->path()).duplicates.load();

	assert_equal(5u, test_group.count, "a resized copy and three turns are duplicates");
	assert_equal(5u, small_group.count, "and so is the original");
	assert_equal(true, test_group.group != 0 && test_group.group == small_group.group, "one duplicate group");

	// The grade is the claim. A re-encode is only ever "possible", and saying so is what separates it
	// from a byte-identical copy on a surface the user deletes from.
	assert_equal(static_cast<int>(df::copy_grade::same_picture), static_cast<int>(test_group.grade),
	             "a resized copy is graded as the same picture");
	assert_equal(static_cast<int>(df::copy_grade::same_picture), static_cast<int>(small_group.grade),
	             "and so is the original");

	// A turned copy joins the set; an unrelated photo taken at the same second still does not.
	assert_equal(5u, index.find_item(test_item2->path()).duplicates.load().count, "a quarter turn is a copy");
	assert_equal(5u, index.find_item(test_item3->path()).duplicates.load().count, "a half turn is a copy");
	assert_equal(1u, index.find_item(sony_item->path()).duplicates.load().count, "an unrelated photo is not");

	// Parity: `@duplicates` and a related search read the one duplicate group, so a picture found by
	// the perceptual stage is reported by both rather than only by the feature that computed it.
	assert_equal(5, count_search_results(index, "@duplicates"), "@duplicates lists the set");

	df::related_info r;
	r.load(test_item1);

	std::string related_summary;

	index.query_items(df::search_t().related(r), [&related_summary, &test_item5](
		                  const index_state::query_item_results& items, bool)
	                  {
		                  for (const auto& i : items)
		                  {
			                  if (i.path != test_item5->path()) continue;
			                  related_summary = df::related_axis_of(i.match.type) == df::related_axis::duplicate
				                                    ? "duplicate"
				                                    : "other";
		                  }
	                  }, test_token);

	assert_equal("duplicate", related_summary,
	             "related reports the resized copy as a possible copy, not a coincidence of time");
}

// Presence, duplicate search and related items are one relation asked at three scales, so none of
// them may see a copy the others deny. The perceptual grade is the one that was visible to duplicate
// search and related items while presence was blind to it.
static void should_report_a_re_encoded_copy_to_presence()
{
	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	const auto cache_path = _temps.next_path();
	database db(index);
	db.open(cache_path.folder(), cache_path.file_name_without_extension());

	// A collection holding the full-size picture only, so an outside copy of the resized one cannot
	// match any member by name, size or checksum. The picture is the only evidence left.
	const auto collection_folder = _temps.next_path().folder().combine("re-encode-collection");
	const auto outside_folder = _temps.next_path().folder().combine("re-encode-outside");
	platform::create_folder(collection_folder);
	platform::create_folder(outside_folder);

	const auto member_path = collection_folder.combine_file("original.jpg");
	const auto outside_path = outside_folder.combine_file("resized-elsewhere.jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), member_path, false, false);
	platform::copy_file(test_files_folder.combine_file("Small.jpg"), outside_path, false, false);

	df::index_roots roots;
	roots.folders.emplace(collection_folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);
	index.update_predictions();

	const auto outside_item = std::make_shared<df::item_element>(outside_path, index.find_item(outside_path));
	index.scan_item(outside_item, true, false);
	index.queue_update_presence(df::item_set({outside_item}));

	const auto presence = outside_item->presence();

	assert_equal(true,
	             presence == item_presence::similar_in || presence == item_presence::newer_in ||
	             presence == item_presence::older_in,
	             "presence reports a possible copy of a re-encode rather than an absence");
	assert_equal(static_cast<int>(df::copy_grade::same_picture),
	             static_cast<int>(outside_item->duplicates().grade),
	             "presence grades it the same way duplicate search would");
}

// Presence and duplicate search are one relation asked at two scales, so a quarter turn has to read
// the same from both. This is the surface that was blind to the perceptual grade before.
static void should_report_a_rotated_copy_to_presence()
{
	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	const auto cache_path = _temps.next_path();
	database db(index);
	db.open(cache_path.folder(), cache_path.file_name_without_extension());

	const auto collection_folder = _temps.next_path().folder().combine("rotate-collection");
	const auto outside_folder = _temps.next_path().folder().combine("rotate-outside");
	platform::create_folder(collection_folder);
	platform::create_folder(outside_folder);

	// The collection holds the upright picture; the outside file is the same picture turned, so it
	// shares no name, size or checksum with the member and its stored extent is transposed. The
	// fixtures are 1024x683 and 672x1024: lossless JPEG rotation trims to the MCU grid, so a turned
	// copy is not an exact transpose, and the shape narrowing has to survive that.
	const auto member_path = collection_folder.combine_file("upright.jpg");
	const auto outside_path = outside_folder.combine_file("turned-elsewhere.jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), member_path, false, false);
	platform::copy_file(test_files_folder.combine_file("Test90.jpg"), outside_path, false, false);

	df::index_roots roots;
	roots.folders.emplace(collection_folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);
	index.update_predictions();

	const auto outside_item = std::make_shared<df::item_element>(outside_path, index.find_item(outside_path));
	index.scan_item(outside_item, true, false);
	index.queue_update_presence(df::item_set({outside_item}));

	const auto presence = outside_item->presence();

	assert_equal(true,
	             presence == item_presence::similar_in || presence == item_presence::newer_in ||
	             presence == item_presence::older_in,
	             "presence reports a possible copy of a rotation rather than an absence");
	assert_equal(static_cast<int>(df::copy_grade::same_picture),
	             static_cast<int>(outside_item->duplicates().grade),
	             "presence grades a turned copy the same way duplicate search would");
}

static void should_require_equal_size_for_duplicate_crc()
{
	df::index_file_item first;
	first.ft = files::file_type_from_name("first.jpg");
	first.name = str::cache("first.jpg");
	first.size = df::file_size(100);
	first.crc32c = 1234;

	df::index_file_item second;
	second.ft = files::file_type_from_name("second.jpg");
	second.name = str::cache("second.jpg");
	second.size = df::file_size(200);
	second.crc32c = 1234;

	assert_equal(false, is_dup_match(&first, &second), "same CRC with different sizes");
	second.size = first.size;
	assert_equal(true, is_dup_match(&first, &second), "same CRC and size");
}

static void should_bound_weak_duplicate_buckets()
{
	const auto root = _temps.next_folder("weak-duplicates");

	for (auto i = 0; i < 160; ++i)
	{
		write_test_file(root.combine_file(std::format("weak-{}.txt", i)), "same-size");
	}

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.update_predictions();

	assert_equal(true, index.stats.indexed_max_compare_count <= 1,
	             "weak size or timestamp buckets do not drive all-pairs comparison");

	const auto copy_root = _temps.next_folder("exact-duplicates");
	const auto first = copy_root.combine_file("first.jpg");
	const auto second = copy_root.combine_file("second.jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), first, false, false);
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), second, false, false);

	index_state exact_index(as, locations);
	df::index_roots exact_roots;
	exact_roots.folders.emplace(copy_root);
	exact_index.index_roots(exact_roots);
	exact_index.index_folders(test_token);
	exact_index.scan_uncached(test_token);
	exact_index.update_predictions();

	const auto first_dups = exact_index.find_item(first).duplicates.load();
	const auto second_dups = exact_index.find_item(second).duplicates.load();

	assert_equal(2u, first_dups.count, "exact duplicates are still grouped");
	assert_equal(first_dups.group, second_dups.group, "exact duplicates share a group");
	assert_equal(static_cast<int>(df::copy_grade::identical), static_cast<int>(first_dups.grade),
	             "exact duplicates keep the strongest grade");

	const auto cached_root = _temps.folder().combine("case-duplicates");
	const auto cached_leaf = cached_root.combine("offline");
	db_items_t cached_items;
	for (const auto name : {"IMG_1.JPG"sv, "img_1.jpg"sv})
	{
		db_item_t cached;
		cached.path = str::cache(name);
		cached_items.emplace_back(std::move(cached));
	}

	index_state cached_index(as, locations);
	db_items_t no_root_items;
	cached_index.merge_folder(cached_root, no_root_items);
	cached_index.merge_folder(cached_leaf, cached_items);
	cached_index.cache_load_complete();

	df::index_roots cached_roots;
	cached_roots.folders.emplace(cached_root);
	cached_index.index_roots(cached_roots);
	cached_index.index_folders(test_token);
	cached_index.update_predictions();

	assert_equal(0u, cached_index.find_item(cached_leaf.combine_file("IMG_1.JPG")).duplicates.load().count,
	             "same-name matching needs a valid date when rows are cached offline");

	const auto outside = std::make_shared<df::item_element>(cached_root.parent().combine_file("IMG_1.JPG"),
	                                                        make_index_file_info({}));
	cached_index.queue_update_presence(df::item_set({outside}));
	assert_equal(static_cast<int>(item_presence::unknown), static_cast<int>(outside->presence()),
	             "presence also refuses same-name pairs with no valid date");

	const auto dated_root = _temps.folder().combine("dated-name-duplicates-parent").combine("dated-name-duplicates");
	const auto dated_left = dated_root.combine("left");
	const auto dated_right = dated_root.combine("right");
	const auto dated = df::date_t(2026, 2, 3, 4, 5, 6);
	db_items_t dated_left_items;
	db_items_t dated_right_items;
	for (auto* items : {&dated_left_items, &dated_right_items})
	{
		db_item_t cached;
		cached.path = str::cache("IMG_1.JPG");
		cached.metadata = std::make_shared<prop::item_metadata>();
		cached.metadata->dates.add(prop::date_source::exif_original, dated);
		cached.metadata->file_name = cached.path;
		items->emplace_back(std::move(cached));
	}

	index_state dated_index(as, locations);
	dated_index.merge_folder(dated_root, db_items_t{});
	dated_index.merge_folder(dated_left, dated_left_items);
	dated_index.merge_folder(dated_right, dated_right_items);
	dated_index.cache_load_complete();
	df::index_roots dated_roots;
	dated_roots.folders.emplace(dated_root);
	dated_index.index_roots(dated_roots);
	dated_index.index_folders(test_token);
	dated_index.update_predictions();

	const auto dated_dups = dated_index.find_item(dated_left.combine_file("IMG_1.JPG")).duplicates.load();
	assert_equal(2u, dated_dups.count, "same-name rows with a valid equal date still group");

	auto dated_outside_info = make_index_file_info(dated);
	dated_outside_info.safe_ps()->dates.add(prop::date_source::exif_original, dated);
	const auto dated_outside = std::make_shared<df::item_element>(
		dated_root.parent().combine_file("img_1.jpg"), dated_outside_info);
	dated_index.queue_update_presence(df::item_set({dated_outside}));
	const auto dated_presence = dated_outside->presence();
	assert_equal(true,
	             dated_presence == item_presence::similar_in ||
	             dated_presence == item_presence::newer_in ||
	             dated_presence == item_presence::older_in,
	             "same-name rows with a valid equal date still match presence");

	const auto offline_video_root = _temps.folder().combine("offline-video-parent").combine("offline-video");
	const auto offline_video_left = offline_video_root.combine("trip-a");
	const auto offline_video_right = offline_video_root.combine("trip-b");
	db_items_t offline_video_left_items;
	db_items_t offline_video_right_items;
	for (auto* items : {&offline_video_left_items, &offline_video_right_items})
	{
		db_item_t cached;
		cached.path = str::cache("GOPR0001.MP4");
		items->emplace_back(std::move(cached));
	}

	index_state offline_video_index(as, locations);
	offline_video_index.merge_folder(offline_video_root, db_items_t{});
	offline_video_index.merge_folder(offline_video_left, offline_video_left_items);
	offline_video_index.merge_folder(offline_video_right, offline_video_right_items);
	offline_video_index.cache_load_complete();
	df::index_roots offline_video_roots;
	offline_video_roots.folders.emplace(offline_video_root);
	offline_video_index.index_roots(offline_video_roots);
	offline_video_index.index_folders(test_token);
	offline_video_index.update_predictions();

	assert_equal(0u,
	             offline_video_index.find_item(offline_video_left.combine_file("GOPR0001.MP4")).duplicates.load().
	                                 group,
	             "offline same-name videos with unknown size do not group");

	const auto online_video_root = _temps.next_folder("online-video-size");
	const auto online_video_left = online_video_root.combine("trip-a");
	const auto online_video_right = online_video_root.combine("trip-b");
	platform::create_folder(online_video_left);
	platform::create_folder(online_video_right);
	write_test_file(online_video_left.combine_file("GOPR0001.MP4"), "video-bytes");
	write_test_file(online_video_right.combine_file("GOPR0001.MP4"), "video-bytes");

	index_state online_video_index(as, locations);
	df::index_roots online_video_roots;
	online_video_roots.folders.emplace(online_video_root);
	online_video_index.index_roots(online_video_roots);
	online_video_index.index_folders(test_token);
	online_video_index.update_predictions();
	assert_equal(2u,
	             online_video_index.find_item(online_video_left.combine_file("GOPR0001.MP4")).duplicates.load().count,
	             "same-name videos with known equal size still group");
}

// Issue #137 - the duplicate badge showed "1" on files that have no duplicate at all, because the
// count includes the file itself. A badge reporting a number that is true of every file tells the
// reader nothing, and the reader has to learn that before they can ignore it.
static void should_badge_only_a_duplicated_item()
{
	df::item_display_info info;
	info.presence = item_presence::not_in;

	info.duplicates = 0;
	assert_equal(false, df::can_show_duplicates(info), "an item no pass has grouped carries no badge");

	info.duplicates = 1;
	assert_equal(false, df::can_show_duplicates(info), "an item that is its own only copy carries no badge");

	info.duplicates = 2;
	assert_equal(true, df::can_show_duplicates(info), "two copies is the first count worth reporting");

	// Presence remains the gate: a count drawn while the check is still running would read as an
	// answer it has not reached.
	info.presence = item_presence::unknown;
	assert_equal(false, df::can_show_duplicates(info), "nothing is claimed before presence answers");
}

static void should_update_collection_presence(shared_test_context& stc)
{
	stc.lazy_load_index();

	const auto source_path = test_files_folder.combine_file("Test.jpg");
	const auto source_info = stc.test_index.find_item(source_path);
	const auto external_root = _temps.next_path().folder().combine("presence-test");

	auto make_external = [&](const std::string_view folder_name, df::index_file_item info,
	                         const std::string_view file_name = "Test.jpg")
	{
		return std::make_shared<df::item_element>(
			df::file_path(external_root.combine(folder_name), file_name), info);
	};

	const auto in_collection = std::make_shared<df::item_element>(source_path, source_info);
	const auto possible_copy = make_external("same", source_info);

	auto newer_info = source_info;
	newer_info.crc32c = 0;
	newer_info.file_modified = df::date_t(2099, 1, 1);
	const auto possible_older_copy = make_external("newer-outside", newer_info);

	auto older_info = source_info;
	older_info.crc32c = 0;
	older_info.file_modified = df::date_t(1990, 1, 1);
	const auto possible_newer_copy = make_external("older-outside", older_info);

	const auto absent = load_item(stc.test_index,
	                              test_files_folder.combine("excluded1").combine_file("document.png"), false);
	auto incomplete_info = source_info;
	incomplete_info.crc32c = 0;
	const auto incomplete = make_external("incomplete", incomplete_info, "not-present.jpg");

	const auto folder_info = std::make_shared<df::index_folder_item>();
	const auto folder = std::make_shared<df::item_element>(external_root.combine("folder"), folder_info);
	folder->presence(item_presence::similar_in);
	folder->duplicates(df::duplicate_info{.group = 42, .count = 2});

	stc.test_index.queue_update_presence(df::item_set({
		in_collection, possible_copy, possible_older_copy, possible_newer_copy, absent, incomplete, folder
	}));

	assert_equal(static_cast<int>(item_presence::this_in), static_cast<int>(in_collection->presence()),
	             "collection member presence");
	assert_equal(source_info.duplicates.load().count, in_collection->duplicates().count,
	             "collection member duplicate summary");
	assert_equal(static_cast<int>(item_presence::similar_in), static_cast<int>(possible_copy->presence()),
	             "possible copy presence");
	assert_equal(static_cast<int>(item_presence::older_in), static_cast<int>(possible_older_copy->presence()),
	             "possible older collection copy presence");
	assert_equal(static_cast<int>(item_presence::newer_in), static_cast<int>(possible_newer_copy->presence()),
	             "possible newer collection copy presence");
	assert_equal(static_cast<int>(item_presence::not_in), static_cast<int>(absent->presence()),
	             "no possible collection copy presence");
	assert_equal(static_cast<int>(item_presence::unknown), static_cast<int>(incomplete->presence()),
	             "incomplete presence remains provisional");
	assert_equal(static_cast<int>(item_presence::unknown), static_cast<int>(folder->presence()),
	             "folder has no file presence");
	assert_equal(0u, folder->duplicates().count, "folder has no duplicate summary");

	std::atomic_int scan_version = 0;
	const df::cancel_token canceled_scan(scan_version);
	const df::cancel_token current_scan(scan_version);
	stc.test_index.scan_uncached(canceled_scan);
	stc.test_index.queue_update_presence(df::item_set({absent}));
	assert_equal(static_cast<int>(item_presence::unknown), static_cast<int>(absent->presence()),
	             "absence remains provisional after an incomplete collection scan");

	stc.test_index.scan_uncached(current_scan);
	stc.test_index.queue_update_presence(df::item_set({absent}));
	assert_equal(static_cast<int>(item_presence::not_in), static_cast<int>(absent->presence()),
	             "absence is published after the collection scan completes");
}

static void should_discard_stale_presence_result()
{
	deferred_async_strategy async;
	location_cache locations;
	index_state index(async, locations);

	df::index_file_item initial;
	initial.name = str::cache("presence.jpg");
	initial.ft = files::file_type_from_name(initial.name);
	initial.size = df::file_size(100);
	initial.file_modified = df::date_t(2020, 1, 1);
	const auto item = std::make_shared<df::item_element>(df::file_path("c:\\presence.jpg"), initial);
	item->presence(item_presence::similar_in);

	index.queue_update_presence(df::item_set({item}));
	auto changed = initial;
	changed.size = df::file_size(200);
	changed.file_modified = df::date_t(2021, 1, 1);
	item->update(item->path(), changed);

	assert_equal(true, async.run_next(async_queue::index_presence_single), "presence work was queued");
	async.drain_ui();

	assert_equal(static_cast<int>(item_presence::similar_in), static_cast<int>(item->presence()),
	             "stale presence result was discarded");
}

static void should_discard_stale_scan_item_update()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);
	index.validate_folder(file_path.folder(), true, platform::now());

	const auto indexed = index.find_item(file_path);
	const auto item = std::make_shared<df::item_element>(file_path, indexed);
	index.scan_item(item, false, false);

	const auto replacement_path = file_path.extension(".changed.jpg");
	item->update(replacement_path, indexed);
	async.drain_ui();

	assert_equal(true, item->path() == replacement_path, "stale scan item update was discarded");
}

static void should_discard_stale_crc_result()
{
	deferred_async_strategy async;
	location_cache locations;
	index_state index(async, locations);

	df::index_file_item initial;
	initial.name = str::cache("crc.jpg");
	initial.ft = files::file_type_from_name(initial.name);
	initial.size = df::file_size(100);
	initial.file_modified = df::date_t(2026, 1, 1, 10, 0, 0);
	const auto path = df::file_path("c:\\crc.jpg");
	const auto item = std::make_shared<df::item_element>(path, initial);

	index.publish_crc(item, path, initial.size, initial.file_modified.load(), df::item_online_status::disk, 0, 123);
	auto changed = initial;
	changed.size = df::file_size(200);
	item->update(path, changed);
	async.drain_ui();

	assert_equal(0u, item->crc32c(), "stale CRC result was discarded");

	// A replacement of the same length is still a different file, and only its modified time says
	// so. Size alone let the previous file's checksum land on the new content.
	const auto same_size = std::make_shared<df::item_element>(path, initial);
	index.publish_crc(same_size, path, initial.size, initial.file_modified.load(), df::item_online_status::disk, 0,
	                  123);
	auto retouched = initial;
	retouched.file_modified = df::date_t(2026, 1, 1, 11, 0, 0);
	same_size->update(path, retouched);
	async.drain_ui();

	assert_equal(0u, same_size->crc32c(), "a same-size replacement does not take the old checksum");

	// The unchanged file still gets its checksum, or the guard would simply stop CRCs working.
	const auto unchanged = std::make_shared<df::item_element>(path, initial);
	index.publish_crc(unchanged, path, initial.size, initial.file_modified.load(), df::item_online_status::disk, 0,
	                  123);
	async.drain_ui();

	assert_equal(123u, unchanged->crc32c(), "an unchanged file keeps its checksum");
}

static void should_continue_predictions_after_terminal_phash_publication()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);

	df::index_roots roots;
	roots.folders.emplace(file_path.folder());
	index.index_roots(roots);
	index.index_folders(test_token);

	const auto file = index.find_item(file_path);
	const phash_result result{file_path, revision_of(file), {crypto::phash_declined, 0, 0, 0}};
	index.save_phashes({result}, false);
	index.save_phashes({result}, true);

	assert_equal(2_z, async.pending_worker_count(async_queue::work), "phash publication is queued in groups");
	assert_equal(0_z, async.pending_worker_count(async_queue::index_predictions_single),
	             "prediction does not run before the terminal hash is published");
	assert_equal(true, async.run_next(async_queue::work), "phash publication ran");
	assert_equal(0_z, async.pending_worker_count(async_queue::index_predictions_single),
	             "an intermediate publication does not continue while more hashes are in flight");
	assert_equal(true, async.run_next(async_queue::work), "final phash publication ran");
	assert_equal(1_z, async.pending_worker_count(async_queue::index_predictions_single),
	             "the final terminal publication, including decline, queues one continuation");
}

// The perceptual walk holds no index lock, so a rescan of an edited picture can clear a record's hash
// between choosing the anchor and comparing against it. Loading the anchor's hash a second time read
// that null and crashed; the walk compares against the hash it chose the anchor for.
static void should_compare_against_the_anchor_hash_it_chose()
{
	const auto root = _temps.next_folder("phash-anchor-cleared");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), root.combine_file("original.jpg"), false, false);
	platform::copy_file(test_files_folder.combine_file("Small.jpg"), root.combine_file("resized.jpg"), false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	auto anchors = 0;
	const df::scope_exit clear_hook([] { test_after_duplicate_anchor = {}; });
	test_after_duplicate_anchor = [&anchors](const df::index_file_item& anchor)
	{
		// Once, as one rescan would: the anchor is cleared after it was chosen and before it is used.
		if (anchors++ == 0) anchor.phash = nullptr;
	};

	index.update_predictions();

	assert_equal(true, anchors > 0, "the perceptual stage chose an anchor");
	assert_equal(2u, index.find_item(root.combine_file("resized.jpg")).duplicates.load().count,
	             "the pair is compared against the hash its anchor was chosen with");
}

static void should_mark_oversized_capture_time_as_crowded()
{
	const auto root = _temps.next_folder("crowded-capture-time");
	for (auto i = 0; i < 9; ++i)
	{
		platform::copy_file(test_files_folder.combine_file("Test.jpg"),
		                    root.combine_file(std::format("burst-{}.jpg", i)), false, false);
	}

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);
	index.update_predictions();

	const auto duplicates = index.find_item(root.combine_file("burst-0.jpg")).duplicates.load();
	assert_equal(1u, duplicates.same_picture_crowded,
	             "a capture time that exceeds the comparison bound stays marked as crowded");
}

static void should_not_report_presence_against_crowd_declined_members()
{
	const auto root = _temps.next_folder("crowded-presence");
	for (auto i = 0; i < 4; ++i)
	{
		platform::copy_file(test_files_folder.combine_file("Test.jpg"),
		                    root.combine_file(std::format("burst-{}.jpg", i)), false, false);
	}

	const auto outside = _temps.next_folder("crowded-presence-outside").combine_file("turned.jpg");
	platform::copy_file(test_files_folder.combine_file("Test90.jpg"), outside, false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);
	index.update_predictions();

	assert_equal(1u, index.find_item(root.combine_file("burst-0.jpg")).duplicates.load().same_picture_crowded,
	             "the collection member was crowd-declined");

	const auto outside_item = std::make_shared<df::item_element>(outside, index.find_item(outside));
	index.scan_item(outside_item, true, false);
	index.queue_update_presence(df::item_set({outside_item}));

	const auto presence = outside_item->presence();
	assert_equal(true, presence == item_presence::unknown || presence == item_presence::not_in,
	             "presence refuses collection members from a crowd-declined burst");
}

static void should_detect_rotation(shared_test_context& stc)
{
	files ff;
	const auto index_path = _temps.next_path();
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	df::index_roots paths;
	paths.folders.emplace(test_files_folder);

	index.index_roots(paths);
	index.index_folders(test_token);

	const auto path_test = df::file_path(test_files_folder, "exif-rotated.jpg");
	const auto test_item = std::make_shared<df::item_element>(path_test, index.find_item(path_test));

	assert_equal(ui::orientation::top_left, test_item->layout_orientation());

	df::item_set items;
	items._items = {test_item};

	index.scan_items(items, false, false, false, false, test_token);
	db.perform_writes();

	assert_equal(ui::orientation::right_top, test_item->layout_orientation());

	// Indexing stores no thumbnail, so the item only asks for one once it is visible - which is what
	// clears db_query_pending. Without this the metadata scan above is the last thing that runs and
	// the thumbnail assertion below is testing nothing.
	items.for_all([](const auto& item) { item->begin_db_thumbnail_query(); });
	db.load_thumbnails(index, database::make_thumbnail_requests(items));

	assert_equal(ui::orientation::right_top, test_item->layout_orientation());

	index.scan_items(items, true, false, false, false, test_token);
	db.perform_writes();

	assert_equal(ui::orientation::right_top, test_item->layout_orientation());
	assert_equal(true, is_valid(test_item->thumbnail()), "the visible-item scan produced a thumbnail");
	assert_equal(ui::orientation::right_top, test_item->thumbnail()->orientation());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Index concurrency (thread-synchronisation regression)
// The indexing thread rebuilds the folder index (index_folders) - replacing the summary folder
// sets and flipping the per-folder is_in_collection/is_excluded flags under the summary/map locks -
// while UI and worker threads read the same state through the const query methods. Before the sync
// review, auto_complete_folders read _summary without the lock and the folder flags were plain
// bools, so this pattern was a data race. This soak test drives that exact reader/writer overlap;
// it must finish without crashing, deadlocking, or corrupting the index.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_index_concurrently()
{
	const auto index_path = _temps.next_path();

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	df::index_roots paths;
	paths.folders.emplace(test_files_folder);
	index.index_roots(paths);
	index.index_folders(test_token);

	std::atomic<int> reader_iterations = 0;
	std::atomic<int> active_reader_iterations = 0;
	std::mutex phase_mutex;
	std::condition_variable phase_changed;
	bool writer_active = false;
	bool stop = false;
	std::vector<std::thread> readers;

	// Readers hammer the const query methods that share state with index_folders.
	int readers_ready = 0;
	for (auto i = 0; i < 3; ++i)
	{
		readers.emplace_back([&]
		{
			{
				std::lock_guard lock(phase_mutex);
				++readers_ready;
			}
			phase_changed.notify_all();

			std::unique_lock lock(phase_mutex);
			phase_changed.wait(lock, [&] { return writer_active || stop; });

			while (!stop)
			{
				const auto active = writer_active;
				lock.unlock();
				index.auto_complete_folders("test", 32);
				(void)index.is_in_collection(test_files_folder);
				(void)index.distinct_folders();
				(void)index.duplicate_list(0);
				reader_iterations.fetch_add(1, std::memory_order_relaxed);
				if (active)
				{
					active_reader_iterations.fetch_add(1, std::memory_order_relaxed);
					phase_changed.notify_all();
				}
				lock.lock();
			}
		});
	}

	auto readers_joined = false;
	const df::scope_exit join_readers([&]
	{
		{
			std::lock_guard lock(phase_mutex);
			stop = true;
			writer_active = true;
		}
		phase_changed.notify_all();
		if (!readers_joined)
		{
			for (auto& t : readers) t.join();
			readers_joined = true;
		}
	});

	{
		std::unique_lock lock(phase_mutex);
		phase_changed.wait(lock, [&] { return readers_ready == static_cast<int>(readers.size()); });
		writer_active = true;
	}
	phase_changed.notify_all();

	// A single writer, matching the real design (one indexing thread), repeatedly rebuilds
	// the folder index while readers are in their acknowledged active phase.
	for (auto i = 0; i < 8 && !df::is_closing; ++i)
	{
		index.index_roots(paths);
		index.index_folders(test_token);
		if (i == 0)
		{
			std::unique_lock lock(phase_mutex);
			phase_changed.wait(lock, [&]
			{
				return active_reader_iterations.load(std::memory_order_relaxed) >= static_cast<int>(readers.size());
			});
		}
	}

	{
		std::lock_guard lock(phase_mutex);
		stop = true;
		writer_active = false;
	}
	phase_changed.notify_all();
	for (auto& t : readers) t.join();
	readers_joined = true;

	assert_equal(true, reader_iterations.load() > 0, "concurrent readers ran");
	assert_equal(true, active_reader_iterations.load() >= static_cast<int>(readers.size()),
	             "each reader acknowledged the writer-active phase");
	assert_equal(true, index.is_in_collection(test_files_folder), "collection intact after concurrent indexing");
}

// validate_folder reads the folder node, enumerates the file system without holding a lock, then
// publishes what it built. Another thread that rebuilt or scanned the same folder inside that
// window leaves newer state in the index - this pass copied the file items, atomics and all, when
// it started - so an unconditional store puts the pre-enumeration copy back over it, losing every
// scan result recorded since and sending those files through the scanner again.
static void should_not_publish_a_stale_folder_rebuild()
{
	index_items items;

	const auto folder = test_files_folder.combine("stale-rebuild");
	const auto name = str::cache("stale-rebuild"sv);

	const auto original = std::make_shared<df::index_folder_item>();
	original->name = name;
	items.replace(folder, original);

	// What a rebuild that read `original` before it enumerated would publish.
	const auto stale = std::make_shared<df::index_folder_item>();
	stale->name = name;

	// Another thread finishes its own rebuild of the same folder first.
	const auto newer = std::make_shared<df::index_folder_item>();
	newer->name = name;
	items.replace(folder, newer);

	const auto original_snapshot = index_folder_snapshot{original, original->revision_snapshot()};
	assert_equal(true, items.replace_if(folder, original_snapshot.folder, original_snapshot.content_revision, stale) ==
	             newer,
	             "a rebuild built from a superseded node does not publish");
	assert_equal(true, items.find(folder) == newer, "the newer node is what the folder still holds");

	const auto newer_snapshot = items.find_snapshot(folder);
	assert_equal(true, items.replace_if(folder, newer_snapshot.folder, newer_snapshot.content_revision, stale) == stale,
	             "a rebuild built from the current node publishes");
	assert_equal(true, items.find(folder) == stale, "and becomes what the folder holds");
}

static void should_not_publish_a_folder_rebuild_over_in_place_content()
{
	index_items items;

	const auto folder = test_files_folder.combine("stale-content-rebuild");
	const auto name = str::cache("stale-content-rebuild"sv);
	const auto file_name = str::cache("changed.jpg"sv);
	const auto added_name = str::cache("added.jpg"sv);

	df::index_item_infos files;
	files.resize(1);
	files[0].name = file_name;
	files[0].ft = files::file_type_from_name(file_name);

	const auto original = std::make_shared<df::index_folder_item>(std::move(files));
	original->name = name;
	items.replace(folder, original);

	const auto snapshot = items.find_snapshot(folder);
	df::index_item_infos rebuilt_files;
	rebuilt_files.resize(2);
	rebuilt_files[0] = snapshot.folder->files[0];
	rebuilt_files[0].metadata_scanned = df::date_t::null;
	rebuilt_files[1].name = added_name;
	rebuilt_files[1].ft = files::file_type_from_name(added_name);
	const auto rebuilt = std::make_shared<df::index_folder_item>(std::move(rebuilt_files));
	rebuilt->name = name;

	const auto wrote = items.update_file(df::file_path(folder, file_name), [file_name](const df::index_folder_item_ptr& f,
	                                                                                  const df::index_file_item& file)
	{
		auto scanned = std::make_shared<prop::item_metadata>();
		scanned->file_name = file_name;
		scanned->tags = str::cache("newer");
		file.metadata_scanned = platform::now();
		file.metadata.store(scanned);
		const auto previous = file.search_presence.load();
		file.calc_search_presence();
		f->update_search_presence(file, previous);
		return true;
	});

	assert_equal(true, wrote, "the in-place writer updated the current node");
	assert_equal(true, items.replace_if(folder, snapshot.folder, snapshot.content_revision, rebuilt) == original,
	             "the stale rebuild is refused");

	const auto retry_snapshot = items.find_snapshot(folder);
	df::index_item_infos retry_files;
	retry_files.resize(2);
	retry_files[0] = retry_snapshot.folder->files[0];
	retry_files[1].name = added_name;
	retry_files[1].ft = files::file_type_from_name(added_name);
	const auto retry = std::make_shared<df::index_folder_item>(std::move(retry_files));
	retry->name = name;

	assert_equal(true, items.replace_if(folder, retry_snapshot.folder, retry_snapshot.content_revision, retry) == retry,
	             "a rebuild retried from current content publishes file-system changes");
	const auto published = items.find(folder);
	assert_equal(true, published == retry, "the retried rebuild becomes current");
	assert_equal(2, static_cast<int>(published->files.size()), "the validation's new file survives the content race");
	assert_equal("newer"sv, published->files[0].metadata.load()->tags.sv(), "newer metadata survives");
	assert_equal(true, published->files[0].metadata_scanned.load().is_valid(), "newer scan stamp survives");
	assert_equal(true, (published->files[0].search_presence.load().types & search_presence_mask::tag) != 0,
	             "newer search state survives");
}

static void should_keep_parent_child_current_after_replacement_ordering()
{
	const auto parent_path = test_files_folder.combine("parent-replace-order");
	const auto child_path = parent_path.combine("child");
	const auto child_name = str::cache("child"sv);

	for (auto round = 0; round < 200; ++round)
	{
		index_items items;
		const auto original = std::make_shared<df::index_folder_item>();
		original->name = child_name;
		df::index_folder_infos children;
		children.emplace_back(original);
		const auto parent = std::make_shared<df::index_folder_item>(df::index_item_infos{}, std::move(children));
		parent->name = parent_path.name();
		items.replace(parent_path, parent);
		items.replace(child_path, original);

		const auto first = std::make_shared<df::index_folder_item>();
		first->name = child_name;
		const auto second = std::make_shared<df::index_folder_item>();
		second->name = child_name;

		std::mutex mutex;
		std::condition_variable changed;
		auto ready = 0;
		auto start = false;

		const auto replace_child = [&](const df::index_folder_item_ptr& replacement)
		{
			std::unique_lock lock(mutex);
			++ready;
			changed.notify_all();
			changed.wait(lock, [&] { return start; });
			lock.unlock();
			items.replace(child_path, replacement);
		};

		std::thread t1(replace_child, first);
		std::thread t2(replace_child, second);

		{
			std::unique_lock lock(mutex);
			changed.wait(lock, [&] { return ready == 2; });
			start = true;
		}
		changed.notify_all();
		t1.join();
		t2.join();

		const auto current = items.find(child_path);
		const auto parent_children = parent->folders_snapshot();
		assert_equal(1, static_cast<int>(parent_children->size()), "parent still has one child");
		assert_equal(true, (*parent_children)[0] == current, "parent child snapshot matches the map entry");
	}
}

static std::string make_numbered_tags(const std::string_view prefix, const int count)
{
	std::string result;
	for (auto i = 0; i < count; ++i)
	{
		if (!result.empty()) result += ' ';
		result += std::format("{}{}", prefix, i);
	}
	return result;
}

static db_item_t make_cached_tag_item(const std::string_view name, const std::string& tags)
{
	db_item_t item;
	item.path = str::cache(name);
	item.metadata = std::make_shared<prop::item_metadata>();
	item.metadata->file_name = item.path;
	item.metadata->tags = str::cache(tags);
	item.metadata_scanned = platform::now();
	return item;
}

static void publish_test_tag_scan(index_state& index, const df::file_path path, const std::string_view tag)
{
	auto item = std::make_shared<df::item_element>(path, index.find_item(path));
	const auto request = index_state::make_scan_request(item, false, false);
	file_scan_result sr;
	sr.success = true;
	sr.keywords.emplace_back(str::cache(tag));
	index.apply_scan_now(request, sr, true, platform::now());
}

static void should_retry_folder_validation_from_current_content()
{
	const auto root = _temps.next_folder("validation-current-content");
	const auto existing_path = root.combine_file("existing.jpg");
	const auto added_path = root.combine_file("added.jpg");
	write_test_file(existing_path, "existing");
	write_test_file(added_path, "added");

	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);

	db_items_t cached;
	cached.emplace_back(make_cached_tag_item("existing.jpg", "oldtag"));
	index.merge_folder(root, cached);

	const df::scope_exit clear_hook([] { test_after_validate_folder_snapshot = {}; });
	test_after_validate_folder_snapshot = [&](const df::folder_path folder, const int attempt)
	{
		if (folder == root && attempt == 0)
		{
			publish_test_tag_scan(index, existing_path, "hooktag");
		}
	};

	const auto result = index.validate_folder(root, true, platform::now());

	assert_equal(false, result.deferred, "one mismatch is retried synchronously");
	assert_equal(2, static_cast<int>(result.folder->files.size()), "validation's new file is published");
	const auto existing = std::lower_bound(result.folder->files.begin(), result.folder->files.end(), existing_path.name());
	assert_equal(true, existing != result.folder->files.end(), "existing file remains indexed");
	assert_equal(true, existing->metadata_scanned.load().is_valid(), "in-place scan stamp survives retry");
	assert_equal("hooktag"sv, existing->metadata.load()->tags.sv(), "in-place metadata survives retry");
	assert_equal(true, (result.folder->search_presence_summary.load().types & search_presence_mask::tag) != 0,
	             "folder summary includes the in-place tag");
	result.folder->is_in_collection = true;
	assert_equal(1, count_search_results(index, "hooktag"), "real query path finds the in-place tag");
}

static void should_defer_folder_validation_after_repeated_content_changes()
{
	const auto root = _temps.next_folder("validation-deferred");
	const auto existing_path = root.combine_file("existing.jpg");
	const auto added_path = root.combine_file("added.jpg");
	write_test_file(existing_path, "existing");
	write_test_file(added_path, "added");

	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);

	db_items_t cached;
	cached.emplace_back(make_cached_tag_item("existing.jpg", "oldtag"));
	index.merge_folder(root, cached);

	const df::scope_exit clear_hook([] { test_after_validate_folder_snapshot = {}; });
	test_after_validate_folder_snapshot = [&](const df::folder_path folder, const int attempt)
	{
		if (folder == root)
		{
			publish_test_tag_scan(index, existing_path, std::format("hooktag{}", attempt));
		}
	};

	const auto deferred = index.validate_folder(root, true, platform::now());
	assert_equal(true, deferred.deferred, "repeated mismatches are reported as deferred");
	assert_equal(true, async.pending_worker_count(async_queue::scan_folder) > 0,
	             "deferred validation queues a plain revalidation");

	test_after_validate_folder_snapshot = {};
	assert_equal(true, async.run_next(async_queue::scan_folder), "queued revalidation runs");

	const auto current = index.validate_folder(root, false, platform::now());
	assert_equal(false, current.deferred, "deferred revalidation settled");
	assert_equal(2, static_cast<int>(current.folder->files.size()), "queued revalidation publishes the new file");
	assert_equal(true, async.was_invalidated(view_invalid::refresh_items), "queued revalidation refreshes items");
}

static void should_keep_validation_resets_across_content_retry()
{
	const auto root = _temps.next_folder("validation-reset-retry");
	const auto media_path = root.combine_file("media.jpg");
	const auto sidecar_path = root.combine_file("media.xmp");
	const auto other_path = root.combine_file("other.jpg");
	write_test_file(media_path, "media");
	write_test_file(other_path, "other");

	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);

	index.validate_folder(root, true, platform::now());
	publish_test_tag_scan(index, media_path, "mediaold");
	write_test_file(sidecar_path, "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>");

	const df::scope_exit clear_hook([] { test_after_validate_folder_snapshot = {}; });
	test_after_validate_folder_snapshot = [&](const df::folder_path folder, const int attempt)
	{
		if (folder == root && attempt == 0)
		{
			publish_test_tag_scan(index, other_path, "othertag");
		}
	};

	const auto result = index.validate_folder(root, true, platform::now());
	const auto media = std::lower_bound(result.folder->files.begin(), result.folder->files.end(), media_path.name());
	const auto other = std::lower_bound(result.folder->files.begin(), result.folder->files.end(), other_path.name());

	assert_equal(true, media != result.folder->files.end(), "media file remains indexed");
	assert_equal(true, other != result.folder->files.end(), "other file remains indexed");
	assert_equal(false, media->metadata_scanned.load().is_valid(), "sidecar association reset survives retry");
	assert_equal("othertag"sv, other->metadata.load()->tags.sv(), "other in-place write survives retry");
}

static void should_retry_parent_validation_after_child_publication()
{
	const auto parent_path = _temps.next_folder("parent-validation-retry");
	const auto child_path = parent_path.combine("child");
	platform::create_folder(child_path);

	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);

	db_items_t cached_child;
	cached_child.emplace_back(make_cached_tag_item("old.jpg", "oldchild"));
	index.merge_folder(parent_path, db_items_t{});
	index.merge_folder(child_path, cached_child);

	const df::scope_exit clear_hook([] { test_after_validate_folder_snapshot = {}; });
	test_after_validate_folder_snapshot = [&](const df::folder_path folder, const int attempt)
	{
		if (folder == parent_path && attempt == 0)
		{
			db_items_t new_child;
			new_child.emplace_back(make_cached_tag_item("new.jpg", "newchild"));
			index.merge_folder(child_path, new_child);
		}
	};

	const auto parent = index.validate_folder(parent_path, true, platform::now());
	const auto current_child = index.validate_folder(child_path, false, platform::now()).folder;
	const auto children = parent.folder->folders_snapshot();

	assert_equal(1, static_cast<int>(children->size()), "parent has one child");
	assert_equal(true, (*children)[0] == current_child, "parent child pointer is the current map node");
}

// A database-loaded folder with a subfolder on disk the cached node does not list yet, whose first
// validation is held up by a writer publishing into it on every attempt.
struct deferred_folder_fixture
{
	df::folder_path root;
	df::file_path existing_path;
	df::folder_path sub;
	df::file_path sub_path;
	int validations = 0;
	bool contended = true;

	explicit deferred_folder_fixture(const std::string_view name) :
		root(_temps.next_folder(name)),
		existing_path(root.combine_file("existing.jpg")),
		sub(root.combine("sub")),
		sub_path(sub.combine_file("inside.jpg"))
	{
		write_test_file(existing_path, "existing");
		platform::create_folder(sub);
		write_test_file(sub_path, "inside");
	}

	void load(index_state& index) const
	{
		db_items_t cached;
		cached.emplace_back(make_cached_tag_item("existing.jpg", "oldtag"));
		index.merge_folder(root, cached);
	}

	// Contends only the first validation of the root unless told otherwise, so a later one publishes.
	void hook(index_state& index)
	{
		test_after_validate_folder_snapshot = [this, &index](const df::folder_path folder, const int attempt)
		{
			if (folder != root) return;
			if (attempt == 0) ++validations;
			if (contended && validations == 1)
			{
				publish_test_tag_scan(index, existing_path, std::format("hooktag{}", attempt));
			}
		};
	}
};

static void should_rescan_folder_after_deferred_folder_scan()
{
	deferred_folder_fixture f("deferred-folder-scan");
	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);
	f.load(index);

	const df::scope_exit clear_hook([] { test_after_validate_folder_snapshot = {}; });
	test_after_validate_folder_snapshot = [&](const df::folder_path folder, const int attempt)
	{
		if (f.contended && folder == f.root)
		{
			publish_test_tag_scan(index, f.existing_path, std::format("hooktag{}", attempt));
		}
	};

	assert_equal(false, index.scan_folder(f.root, true, platform::now()), "a contended folder scan is deferred");
	assert_equal(false, index.scan_folder(f.root, true, platform::now()), "and so is a second one");
	assert_equal(1, static_cast<int>(async.pending_worker_count(async_queue::scan_folder)),
	             "repeated deferrals of one folder share one follow-up");

	f.contended = false;
	while (async.run_next(async_queue::scan_folder))
	{
	}

	assert_equal(true, index.find_item(f.sub_path).ft != nullptr, "the follow-up rescans into the subfolder");
	assert_equal(true, async.was_invalidated(view_invalid::refresh_items), "the follow-up refreshes the view");
}

static void should_revisit_deferred_folder_during_index_walk()
{
	deferred_folder_fixture f("deferred-index-walk");
	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);
	f.load(index);

	const df::scope_exit clear_hook([] { test_after_validate_folder_snapshot = {}; });
	f.hook(index);

	df::index_roots roots;
	roots.folders.emplace(f.root);
	index.index_roots(roots);
	index.index_folders(test_token);

	assert_equal(2, f.validations, "the deferred collection folder is walked again");
	assert_equal(true, index.find_item(f.sub_path).ft != nullptr, "the revisit walks the folder's subfolders");
	// The node's own mark, since is_in_collection answers any child of a root from the roots alone.
	const auto sub_node = index.validate_folder(f.sub, false, platform::now()).folder;
	assert_equal(true, sub_node && sub_node->is_in_collection.load(), "and they join the collection");
}

static void should_list_deferred_folder_in_import_analysis()
{
	deferred_folder_fixture f("deferred-import-analysis");
	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);
	f.load(index);

	const df::scope_exit clear_hook([] { test_after_validate_folder_snapshot = {}; });
	f.hook(index);

	df::index_roots roots;
	roots.folders.emplace(f.root);
	const auto listed = index.scan_items(roots, true, true, test_token);
	const auto lists = [&listed](const df::file_path path)
	{
		return std::ranges::any_of(listed, [path](const folder_scan_item& i)
		{
			return df::file_path(i.folder, i.item.name) == path;
		});
	};

	assert_equal(2, f.validations, "the deferred source folder is analysed again");
	assert_equal(true, lists(f.existing_path), "the deferred source folder's files are listed");
	assert_equal(true, lists(f.sub_path), "the deferred source folder's subfolders are walked");
}

static void should_scan_forced_item_when_folder_validation_defers()
{
	const auto root = _temps.next_folder("deferred-forced-scan");
	const auto photo_path = root.combine_file("photo.jpg");
	const auto other_path = root.combine_file("other.jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), photo_path, false, false);
	write_test_file(other_path, "other");

	deferred_async_strategy async;
	const location_cache locations;
	index_state index(async, locations);
	index.validate_folder(root, true, platform::now());
	// A file the node does not list yet, so every attempt has a change to publish. The writer's stamp
	// alone is not one: it comes from the same coarse clock as the file's modified time and can equal it.
	write_test_file(root.combine_file("added.jpg"), "added");

	const df::scope_exit clear_hook([] { test_after_validate_folder_snapshot = {}; });
	test_after_validate_folder_snapshot = [&](const df::folder_path folder, const int attempt)
	{
		if (folder == root) publish_test_tag_scan(index, other_path, std::format("othertag{}", attempt));
	};

	df::item_set items;
	items._items = {std::make_shared<df::item_element>(photo_path, index.find_item(photo_path))};
	const auto refreshed = index.scan_items(items, false, true, false, false, test_token, true);
	test_after_validate_folder_snapshot = {};

	assert_equal(true, async.pending_worker_count(async_queue::scan_folder) > 0, "the folder's validation deferred");
	assert_equal(true, refreshed, "the forced rescan still scans the item");
}

static void should_bound_tag_companion_recommendations()
{
	const auto root = _temps.next_folder("tag-companions");

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);

	db_items_t items;
	items.emplace_back(make_cached_tag_item("many.jpg", make_numbered_tags("pathological-tag-", 512)));
	items.emplace_back(make_cached_tag_item("normal.jpg", "normal-alpha normal-beta normal-beta normal-gamma"));
	for (auto i = 0; i < 11050; ++i)
	{
		items.emplace_back(make_cached_tag_item(std::format("ordinary-{:05}.jpg", i),
		                                        std::format("ordinaryanchor ordinarytag{}", i)));
	}
	index.merge_folder(root, items);
	index.update_summary();

	const auto companion_entry_count = index.tag_companion_entry_count();
	assert_equal(true, companion_entry_count > 20000u,
	             "ordinary companion suggestions are not globally truncated");
	assert_equal(true, companion_entry_count <= 128u * 127u + 11050u * 2u + 6u,
	             "companion storage is bounded by the per-item recommendation budget");
	assert_equal(1, static_cast<int>(index.tag_summary("pathological-tag-511").total_items().count),
	             "authoritative tag counts keep tags beyond the companion budget");
	assert_equal(1, count_search_results(index, "pathological-tag-511"),
	             "tag search keeps tags beyond the companion budget");

	const auto suggestions = index.auto_complete_tag_companions({"normal-alpha"}, "normal-b", 8);
	assert_equal(1, static_cast<int>(suggestions.size()), "normal companion suggestions stay deterministic");
	assert_equal("#normal-beta"s, suggestions[0].text, "normal companion text");
	assert_equal(1, suggestions[0].occurrences, "repeated tags do not multiply recommendation work");

}

static void should_publish_coherent_indexing_progress()
{
	const auto root = _temps.next_folder("index-progress");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), root.combine_file("one.jpg"), false, false);
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), root.combine_file("two.jpg"), false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);

	auto progress = index.indexing_progress();
	assert_equal(0, progress.index_item_count, "progress starts as one coherent empty snapshot");
	assert_equal(0, progress.index_item_remaining, "progress starts with no remaining items");

	index.scan_uncached(test_token);

	progress = index.indexing_progress();
	assert_equal(2, progress.index_item_count, "progress publishes the scan total");
	assert_equal(0, progress.index_item_remaining, "progress publishes completion with the same snapshot");

	std::atomic_bool cancel_flag = true;
	df::cancel_token cancelled(cancel_flag);
	index.scan_uncached(cancelled);
	progress = index.indexing_progress();
	assert_equal(2, progress.index_item_count, "cancelled restart still publishes a coherent total");
	assert_equal(0, progress.index_item_remaining, "cancelled restart publishes a bounded remaining count");
}

// Verifies the pure prefix-range lookup that powers fast typeahead prediction over the
// case-insensitively sorted vocabulary snapshot.
static void should_find_word_prefix_range()
{
	// Sorted by str::icmp: '#' (0x23) < '@' (0x40) < letters (case-folded).
	const std::vector<std::string_view> terms = {
		"#dog", "@video", "Amsterdam", "index", "indigo", "industry", "windmill", "window"
	};

	const auto collect = [&terms](const std::string_view q)
	{
		const auto [lo, hi] = word_prefix_range(terms, q);
		return std::vector<std::string_view>(lo, hi);
	};

	assert_equal(3_z, collect("ind").size(), "three words start with 'ind'");
	assert_equal("index"s, std::string(collect("ind").front()), "first 'ind' match");
	assert_equal("industry"s, std::string(collect("ind").back()), "last 'ind' match");
	assert_equal(3_z, collect("IND").size(), "prefix match is case-insensitive");
	assert_equal(0_z, collect("dust").size(), "an interior substring is not a prefix");
	assert_equal(1_z, collect("@vid").size(), "scoped '@vid' prefix");
	assert_equal("@video"s, std::string(collect("@vid").front()), "'@video' matched");
	assert_equal(1_z, collect("#do").size(), "scoped '#do' prefix");
	assert_equal(0_z, collect("zzz").size(), "no match returns an empty range");
	assert_equal(1_z, collect("windmill").size(), "exact word matches itself");
	assert_equal(terms.size(), collect("").size(), "empty query matches all terms");
}

// Verifies the compressed posting lists (delta + VByte) and the boolean set operations that
// will compose term queries in the search inverted index.
static void should_encode_postings()
{
	// Round-trips across single-byte, multi-byte and max-width varints and large gaps.
	const std::vector<uint32_t> ids = {0, 1, 5, 127, 128, 300, 16383, 16384, 1000000, 4000000000u};
	const auto pl = df::posting_list::from_sorted(ids);
	assert_equal(true, pl.to_vector() == ids, "postings round-trip");
	assert_equal(static_cast<int>(ids.size()), static_cast<int>(pl.count()), "posting count");

	std::vector<uint32_t> via_each;
	pl.for_each([&via_each](const uint32_t id) { via_each.push_back(id); });
	assert_equal(true, via_each == ids, "for_each matches to_vector");

	// Duplicates and out-of-order ids are ignored (a term twice in one item = one posting).
	df::posting_list dedup;
	dedup.add(10);
	dedup.add(10);
	dedup.add(4);
	dedup.add(11);
	assert_equal(true, (dedup.to_vector() == std::vector<uint32_t>{10, 11}), "dedup / out-of-order ignored");

	// Empty list.
	const df::posting_list empty;
	assert_equal(true, empty.empty(), "empty posting list");
	assert_equal(true, empty.to_vector().empty(), "empty decodes to empty");

	// Compression: a dense contiguous run is ~1 byte per id (all deltas == 1).
	std::vector<uint32_t> dense(1000);
	for (uint32_t i = 0; i < dense.size(); ++i) dense[i] = i;
	const auto dense_pl = df::posting_list::from_sorted(dense);
	assert_equal(true, dense_pl.byte_size() <= dense.size() + 4, "dense run compresses to ~1 byte/id");

	// Boolean set operations (AND / OR / AND-NOT) that compose term queries.
	const std::vector<uint32_t> a = {1, 3, 5, 7, 9};
	const std::vector<uint32_t> b = {2, 3, 4, 7, 8};
	assert_equal(true, (df::postings_intersect(a, b) == std::vector<uint32_t>{3, 7}), "intersect (AND)");
	assert_equal(true, (df::postings_union(a, b) == std::vector<uint32_t>{1, 2, 3, 4, 5, 7, 8, 9}), "union (OR)");
	assert_equal(true, (df::postings_difference(a, b) == std::vector<uint32_t>{1, 5, 9}), "difference (AND-NOT)");
}

// Builds a tiny inverted index and cross-checks its term lookups and boolean composition
// against a brute-force scan of the same corpus - the correctness strategy for replacing the
// per-item search scan with a reverse index.
static void should_query_inverted_index()
{
	const std::vector<std::pair<uint32_t, std::vector<std::string_view>>> docs = {
		{0, {"beach", "sunset", "hawaii"}},
		{1, {"beach", "surf"}},
		{2, {"mountain", "sunset"}},
		{3, {"beach", "sunset", "surf"}},
		{4, {"portrait"}},
	};

	df::inverted_index index;
	for (const auto& [id, terms] : docs) index.add_document(id, terms);

	const auto brute = [&docs](const std::string_view term)
	{
		std::vector<uint32_t> out;
		for (const auto& [id, terms] : docs)
			if (std::ranges::any_of(terms, [term](const std::string_view t) { return str::icmp(t, term) == 0; }))
				out.push_back(id);
		return out;
	};

	for (const auto* const term : {"beach", "sunset", "surf", "portrait", "mountain", "missing"})
	{
		assert_equal(true, index.find(term) == brute(term), std::format("term '{}' matches brute force", term));
	}

	assert_equal(true, index.find("BEACH") == brute("beach"), "case-insensitive term lookup");

	// Boolean composition maps to AND / AND-NOT / OR query semantics.
	assert_equal(
		true, (df::postings_intersect(index.find("beach"), index.find("sunset")) == std::vector<uint32_t>{0, 3}),
		"beach AND sunset");
	assert_equal(true, (df::postings_difference(index.find("beach"), index.find("surf")) == std::vector<uint32_t>{0}),
	             "beach AND NOT surf");
	assert_equal(true,
	             (df::postings_union(index.find("sunset"), index.find("mountain")) == std::vector<uint32_t>{0, 2, 3}),
	             "sunset OR mountain");
}

// Cross-checks the trigram substring accelerator: its candidates must be a superset of the
// true (case-insensitive) substring matches, and verifying those candidates must reproduce the
// brute-force result exactly - the safe "candidate + verify" path for indexed substring search.
static void should_query_trigram_index()
{
	const std::vector<std::string_view> corpus = {
		"beach sunset", // 0
		"bulldog puppy", // 1
		"my dog photo", // 2
		"hotdog stand", // 3
		"mountain lake", // 4
		"DOGMA", // 5 - different case
		// 6 - UTF-8 for the code points U+65E5 U+672C U+8A9E. Written as escapes so the bytes do not
		// depend on the source encoding. Above U+3FFF, so these are the only grams here whose top key
		// bytes are non-zero - the radix passes that an all-Latin corpus lets freeze() skip.
		"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e photo",
	};

	df::trigram_index index;
	for (uint32_t i = 0; i < corpus.size(); ++i) index.add(i, corpus[i]);
	index.freeze();

	const auto brute = [&corpus](const std::string_view q)
	{
		std::vector<uint32_t> out;
		for (uint32_t i = 0; i < corpus.size(); ++i)
			if (str::contains(corpus[i], q)) out.push_back(i);
		return out;
	};

	for (const auto* const q : {"dog", "sunset", "mountain", "og p", "xyz", "DOG", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"})
	{
		const auto cand = index.candidates(q);
		assert_equal(true, cand.has_value(), std::format("'{}' produces trigram candidates", q));

		const auto truth = brute(q);

		// No false negatives: every true match is among the candidates.
		for (const auto id : truth)
			assert_equal(true, std::ranges::find(*cand, id) != cand->end(),
			             std::format("'{}' candidate superset", q));

		// Verifying candidates reproduces the brute-force substring result exactly.
		std::vector<uint32_t> verified;
		for (const auto id : *cand)
			if (str::contains(corpus[id], q)) verified.push_back(id);
		std::ranges::sort(verified);
		assert_equal(true, verified == truth, std::format("'{}' verified candidates == brute force", q));
	}

	// Queries shorter than a trigram cannot use the index.
	assert_equal(false, index.candidates("do").has_value(), "short query requires a scan");
}

static void should_materialize_detached_query_item()
{
	null_async_strategy async;
	const location_cache locations;
	const index_state index(async, locations);

	df::index_file_item file;
	file.name = "detached.jpg"_c;
	file.ft = files::file_type_from_name(file.name);
	file.size = df::file_size(1234);
	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->title = "Detached snapshot"_c;
	file.metadata.store(metadata);

	const auto path = df::file_path("c:\\detached.jpg");
	index_state::query_item_results query_items;
	query_items.push_back({index_state::query_item_kind::file, path, file, {}, {}, {}});

	const auto items = index.materialize_query_items(std::move(query_items), {});
	assert_equal(1_z, items.size(), "detached query result materialized without an index lookup");
	assert_equal("Detached snapshot", items.items().front()->title(), "detached metadata snapshot retained");
	assert_equal(1234, static_cast<int>(items.items().front()->file_size().to_int64()),
	             "detached file facts retained");
}

static void should_batch_thumbnail_publication()
{
	deferred_async_strategy async;
	location_cache locations;
	index_state index(async, locations);

	df::index_file_item first_file;
	first_file.name = "first.jpg"_c;
	first_file.ft = files::file_type_from_name(first_file.name);
	const auto first = std::make_shared<df::item_element>(df::file_path("c:\\first.jpg"), first_file);

	df::index_file_item second_file;
	second_file.name = "second.jpg"_c;
	second_file.ft = files::file_type_from_name(second_file.name);
	const auto second = std::make_shared<df::item_element>(df::file_path("c:\\second.jpg"), second_file);

	const auto timestamp = df::date_t(2026, 7, 28);
	index_state::thumbnail_results results;
	results.emplace_back(first, first->path(), nullptr, nullptr, timestamp);
	results.emplace_back(second, second->path(), nullptr, nullptr, timestamp);
	index.publish_thumbnails(std::move(results), false);

	assert_equal(1_z, async.pending_ui_count(), "thumbnail result batch should queue one UI callback");
	async.drain_ui();
	assert_equal(timestamp, first->thumbnail_timestamp(), "first batched thumbnail result applied");
	assert_equal(timestamp, second->thumbnail_timestamp(), "second batched thumbnail result applied");
	assert_equal(true, async.was_invalidated(view_invalid::view_redraw), "thumbnail publication requests redraw");
	assert_equal(false, async.was_invalidated(view_invalid::view_layout),
	             "unchanged thumbnail geometry does not request full layout");
}

static void should_bound_folder_thumbnail_candidate_visits()
{
	const auto budget = database_test_seams::folder_thumbnail_visit_budget();
	const auto root = df::folder_path("c:\\root");
	std::vector<df::folder_path> wide_visits;
	auto wide_enumerations = 0;

	database_test_seams::visit_folder_thumbnail_candidates(
		root,
		[&](const df::folder_path& folder)
		{
			wide_visits.emplace_back(folder);
			return true;
		},
		[&](const df::folder_path& folder)
		{
			++wide_enumerations;
			std::vector<df::folder_path> result;
			for (auto i = 0; i < static_cast<int>(budget * 2); ++i)
			{
				result.emplace_back(folder.combine(std::format("child-{}", i)));
			}
			return result;
		});

	assert_equal(budget, wide_visits.size(), "wide folder traversal stops at the visit budget");
	assert_equal(1, wide_enumerations, "children after the budget are not enumerated");

	std::vector<df::folder_path> deep_visits;
	database_test_seams::visit_folder_thumbnail_candidates(
		root,
		[&](const df::folder_path& folder)
		{
			deep_visits.emplace_back(folder);
			return true;
		},
		[&](const df::folder_path& folder)
		{
			return std::vector<df::folder_path>{folder.combine("next"sv)};
		});

	assert_equal(budget, deep_visits.size(), "deep folder traversal stops at the same visit budget");
}

static void should_discard_stale_thumbnail_publications()
{
	const auto image_path = test_files_folder.combine_file("Test.jpg");
	files ff;
	const auto loaded = ff.load(image_path, false);
	assert_equal(true, is_valid(loaded.i), "thumbnail fixture loaded");

	deferred_async_strategy async;
	location_cache locations;
	index_state index(async, locations);

	df::index_file_item file;
	file.name = "stale-thumb.jpg"_c;
	file.ft = files::file_type_from_name(file.name);
	const auto item = std::make_shared<df::item_element>(df::file_path("c:\\stale-thumb.jpg"), file);

	const auto old_generation = item->begin_thumbnail_load();
	const auto new_generation = item->begin_thumbnail_load();
	index.publish_thumbnail(item, item->path(), loaded.i, {}, df::date_t(2026, 8, 1), old_generation, true, true);
	index.publish_thumbnail_failure(item, item->path(), old_generation);
	async.drain_ui();

	assert_equal(false, item->has_thumb(), "obsolete local thumbnail result is rejected");
	assert_equal(false, item->failed_loading_thumbnail(), "obsolete local failure is rejected");
	assert_equal(true, item->is_loading_thumbnail(), "obsolete completion does not clear the newer claim");

	index.publish_thumbnail(item, item->path(), loaded.i, {}, df::date_t(2026, 8, 2), new_generation, true, true);
	async.drain_ui();

	assert_equal(true, item->has_thumb(), "current local thumbnail result is accepted");
	assert_equal(false, item->is_loading_thumbnail(), "current local result releases its loading claim");

	const auto db_generation = item->begin_db_thumbnail_query();
	item->thumbnail(loaded.i, {}, df::date_t(2026, 8, 3));

	index_state::thumbnail_results results;
	results.emplace_back(item, item->path(), loaded.i, nullptr, df::date_t(2026, 8, 4), db_generation);
	index.publish_thumbnails(std::move(results), false);
	async.drain_ui();

	assert_equal(df::date_t(2026, 8, 3), item->thumbnail_timestamp(),
	             "obsolete database thumbnail does not overwrite newer pixels");
}

static void should_build_thumbnail_requests_with_currency()
{
	df::index_file_item file_info;
	file_info.name = "request.jpg"_c;
	file_info.ft = files::file_type_from_name(file_info.name);
	const auto file = std::make_shared<df::item_element>(df::file_path("c:\\folder\\request.jpg"), file_info);
	const auto file_generation = file->begin_db_thumbnail_query();

	// Mirrors the Items view database-thumbnail request construction. Before this regression was
	// fixed, the added generation field shifted the positional aggregate arguments so folders were
	// sent down the file branch and files lost their request currency.
	const auto file_request = database::make_thumbnail_request(file, file_generation);
	assert_equal(file_generation, file_request.generation, "file db thumbnail request keeps currency");
	assert_equal(false, file_request.is_folder, "file db thumbnail request keeps the file/folder kind");

	auto folder_info = std::make_shared<df::index_folder_item>();
	folder_info->name = "folder"_c;
	const auto folder = std::make_shared<df::item_element>(df::folder_path("c:\\folder"), folder_info);
	const auto folder_generation = folder->begin_db_thumbnail_query();
	const auto folder_request = database::make_thumbnail_request(folder, folder_generation);
	assert_equal(folder_generation, folder_request.generation, "folder db thumbnail request keeps currency");
	assert_equal(true, folder_request.is_folder, "folder db thumbnail request keeps the file/folder kind");
}

static void should_release_thumbnail_loading_after_non_owner_install()
{
	const auto image_path = test_files_folder.combine_file("Test.jpg");
	files ff;
	const auto loaded = ff.load(image_path, false);
	assert_equal(true, is_valid(loaded.i), "thumbnail fixture loaded");

	deferred_async_strategy async;
	location_cache locations;
	index_state index(async, locations);

	df::index_file_item file;
	file.name = "loading-owner.jpg"_c;
	file.ft = files::file_type_from_name(file.name);
	const auto item = std::make_shared<df::item_element>(df::file_path("c:\\loading-owner.jpg"), file);

	const auto owner_generation = item->begin_thumbnail_load();
	item->thumbnail(loaded.i, {}, df::date_t(2026, 9, 1));
	index.publish_thumbnail(item, item->path(), loaded.i, {}, df::date_t(2026, 9, 2), owner_generation, true, true);
	async.drain_ui();

	assert_equal(false, item->is_loading_thumbnail(),
	             "the owner completion releases loading even after a non-owner thumbnail install");
	assert_equal(df::date_t(2026, 9, 1), item->thumbnail_timestamp(),
	             "obsolete owner pixels are not installed after a newer non-owner thumbnail");
}

static void should_keep_folder_presence_summary_add_only()
{
	df::index_file_item file;
	file.name = "presence.jpg"_c;
	file.ft = files::file_type_from_name(file.name);
	search_presence_mask previous;
	previous.types = search_presence_mask::tag;
	file.search_presence = search_presence_mask{};

	auto folder = std::make_shared<df::index_folder_item>();
	folder->search_presence_summary = previous;
	folder->update_search_presence(file, previous);

	assert_equal(true, folder->search_presence_summary.load().contains_required(previous),
	             "folder search-presence summary keeps previously added bits");
}

static void should_skip_unneeded_thumbnail_staging()
{
	deferred_async_strategy async;
	df::index_file_item file;
	file.name = "staging.jpg"_c;
	file.ft = files::file_type_from_name(file.name);
	const auto item = std::make_shared<df::item_element>(df::file_path("c:\\staging.jpg"), file);

	item->stage_thumbnail_surface(async);
	assert_equal(0_z, async.pending_worker_count(async_queue::render),
	             "missing thumbnail should not queue render work");
}

static void should_reuse_cached_thumbnail_surface()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_async_strategy scan_async;
	const location_cache locations;
	index_state index(scan_async, locations);
	const auto item = load_item(index, file_path, true);
	assert_equal(true, item->has_cached_surface(), "scan fixture populated the thumbnail surface cache");

	deferred_async_strategy deferred;
	item->stage_thumbnail_surface(deferred);
	assert_equal(0_z, deferred.pending_worker_count(async_queue::render),
	             "cached thumbnail surface should not be staged again");
}

static void should_not_rescan_unchanged_sidecars_after_reload()
{
	const auto db_path = _temps.next_path();
	const auto temp_folder = _temps.next_folder("sidecar-reload");
	const auto file_path = _temps.next_path_in(temp_folder, ".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, false);
	const auto xmp_path = file_path.extension(".xmp");
	const std::string xmp_packet =
		"<?xpacket begin=\"\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
		"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description/></rdf:RDF>"
		"</x:xmpmeta><?xpacket end=\"w\"?>";
	df::blob_save_to_file(df::blob(xmp_packet.begin(), xmp_packet.end()), xmp_path);

	null_async_strategy as;
	const location_cache locations;

	{
		index_state index(as, locations);
		database db(index);
		db.open(db_path.folder(), db_path.file_name_without_extension());
		df::index_roots roots;
		roots.folders.emplace(temp_folder);
		index.index_roots(roots);
		index.index_folders(test_token);
		index.scan_uncached(test_token);
		db.perform_writes();
	}

	index_state reloaded(as, locations);
	database db(reloaded);
	db.open(db_path.folder(), db_path.file_name_without_extension());
	db.load_index_values();
	df::index_roots roots;
	roots.folders.emplace(temp_folder);
	reloaded.index_roots(roots);
	reloaded.index_folders(test_token);

	const auto scans_before = df::file_perf.scans.load();
	reloaded.scan_uncached(test_token);
	assert_equal(scans_before, df::file_perf.scans.load(), "unchanged sidecars do not rescan after db reload");
}

// Simulates a OneDrive Files On-Demand online-only file using platform::test_offline_predicate,
// which forces a real local file to be reported as an offline placeholder during folder
// enumeration. Verifies: (1) offline placeholders are indexed for metadata via the shell path
// with no content hash, (2) metadata_scanned persists so they are NOT re-indexed on restart,
// and (3) once hydrated (online) they are re-scanned to get a content hash and thumbnail.
static void should_index_offline_placeholder()
{
	const auto index_path = _temps.next_path();
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	// --- Phase 1: the file is an online-only cloud placeholder ---
	platform::test_offline_predicate = [file_path](const df::file_path& p) { return p == file_path; };
	const df::scope_exit clear_offline([] { platform::test_offline_predicate = nullptr; });

	df::index_roots roots;
	roots.folders.emplace(file_path.folder());
	index.index_roots(roots);
	index.index_folders(test_token);

	const auto offline_item = load_item(index, file_path, true);

	const auto offline_status = offline_item->online_status();
	const auto offline_md = offline_item->metadata();
	const auto offline_crc = offline_item->crc32c();
	const auto offline_tag_search = count_search_results(index, "key1");
	const auto offline_tag_mask =
		(index.find_item(file_path).search_presence.load().types & search_presence_mask::tag) != 0;
	// The shell property store also surfaces keywords and GPS for a placeholder (no hydration), which
	// the offline scan reads for free to help index non-downloaded files.
	const std::string offline_tags(offline_md ? offline_md->tags.sv() : std::string_view{});
	const auto offline_has_gps = offline_md && offline_md->coordinate.is_valid();
	[[maybe_unused]] const auto offline_gps_ok = offline_has_gps &&
		std::abs(offline_md->coordinate.latitude() - 50.08806) < 0.01 &&
		std::abs(offline_md->coordinate.longitude() - 14.42083) < 0.01;
	// Guards against re-indexing every startup: a scanned placeholder must not report that it
	// still needs a metadata scan, nor keep requesting a (shell) thumbnail every session.
	const auto offline_needs_scan = index.needs_scan(offline_item);
	const auto offline_wants_thumb = offline_item->should_load_thumbnail();

	db.perform_writes();

	// Reload into a fresh index from the same database to prove persistence, i.e. that the
	// item would NOT be re-indexed on the next application start.
	uint32_t reloaded_crc = 0xffffffffu;
	bool reloaded_scanned_valid = false;
	{
		index_state index2(as, locations);
		database db2(index2);
		db2.open(index_path.folder(), index_path.file_name_without_extension());
		db2.load_index_values();
		const auto reloaded = index2.find_item(file_path);
		reloaded_crc = reloaded.crc32c;
		reloaded_scanned_valid = reloaded.metadata_scanned.load().is_valid();
	}

	// --- Phase 2: the file has been hydrated (downloaded) and is now online ---
	platform::test_offline_predicate = nullptr;

	const auto online_item = load_item(index, file_path, true);

	const auto online_status = online_item->online_status();
	const auto online_crc = online_item->crc32c();
	const auto online_thumb_valid = ui::is_valid(online_item->thumbnail());
	const auto online_md = online_item->metadata();
	const std::string online_tags(online_md ? online_md->tags.sv() : std::string_view{});

	assert_equal(static_cast<int>(df::item_online_status::offline), static_cast<int>(offline_status),
	             "item reported offline while a placeholder");
	assert_equal(true, offline_md != nullptr, "offline item has shell metadata");
#ifdef _WIN32
	// A placeholder's bytes are not on disk, so this content can only come from the shell property
	// store answering on the file's behalf. Elsewhere an offline scan has nothing to read.
	assert_equal("key1 key2 key3", offline_tags, "shell keywords extracted for offline placeholder");
	assert_equal(true, offline_tag_mask, "offline shell metadata refreshes the indexed tag prefilter");
	assert_equal(1, offline_tag_search, "offline shell metadata is immediately searchable");
	assert_equal(true, offline_gps_ok, "shell GPS coordinate extracted for offline placeholder");
#endif
	assert_equal(0u, offline_crc, "offline item has no content hash");
	assert_equal(false, offline_needs_scan, "scanned placeholder does not need re-scan (no re-index on restart)");
	assert_equal(false, offline_wants_thumb, "placeholder does not repeatedly request a thumbnail");

	assert_equal(true, reloaded_scanned_valid, "metadata_scanned persisted (no re-index on restart)");
	assert_equal(0u, reloaded_crc, "no content hash persisted for offline item");

	assert_equal("key1 key2 key3", online_tags, "embedded tags extracted into item metadata after hydration");

	assert_equal(static_cast<int>(df::item_online_status::disk), static_cast<int>(online_status),
	             "item reported online after hydration");
	assert_equal(true, online_crc != 0, "content hash computed after hydration");
	assert_equal(true, online_thumb_valid, "thumbnail loaded after hydration");
}

// A folder brought in from the database has no filesystem stamp on it at all - the cached row
// carries neither a modified time nor a size - so the first validate_folder of every launch cannot
// answer "have the bytes changed?" by comparing stamps. It has to ask the scan timestamp instead,
// which is persisted. Getting this wrong in either direction is silent: read the absent stamp as a
// difference and every cached checksum is wiped on every launch, so duplicate detection loses its
// byte-identical rung; read it as a match and an edited file keeps the hash of its old bytes and is
// reported identical to its untouched copy.
static void should_keep_a_cached_checksum_the_bytes_still_describe()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	const auto file_modified = platform::file_attributes(file_path).modified;
	assert_equal(true, df::date_t(file_modified).is_valid(), "the fixture has a modified time to reason about");

	null_async_strategy as;
	location_cache locations;

	constexpr uint32_t cached_crc = 0x1234u;
	constexpr crypto::phash_rotations cached_phash{0x1111ull, 0x2222ull, 0x3333ull, 0x4444ull};

	struct surviving
	{
		uint32_t crc = 0;
		bool has_phash = false;
		// What the next launch would load, once the validation's own writes are flushed.
		uint32_t stored_crc = 0;
		bool has_stored_phash = false;
	};

	// Writes one cached row for the file, reopens it in a fresh index (which is what builds a node
	// with no stamp), runs the validation index_folders makes, and answers with the surviving hashes -
	// in the node, and in the row a later launch reads back.
	const auto after_validation = [&](const df::date_t scanned) -> surviving
	{
		const auto db_name = _temps.next_path();

		{
			index_state index(as, locations);
			database db(index);
			db.open(db_name.folder(), db_name.file_name_without_extension());

			std::deque<item_db_write> writes;
			item_db_write w;
			w.path = file_path;
			w.md = std::make_shared<prop::item_metadata>();
			w.crc32c = cached_crc;
			w.phash = cached_phash;
			w.metadata_scanned = scanned;
			writes.emplace_back(std::move(w));

			db.perform_writes(std::move(writes));
			db.close();
		}

		surviving result;

		{
			index_state index(as, locations);
			database db(index);
			db.open(db_name.folder(), db_name.file_name_without_extension());

			assert_equal(cached_crc, index.find_item(file_path).crc32c.load(), "the cached hash was loaded");
			assert_equal(true, index.find_item(file_path).phash.load() != nullptr,
			             "the cached picture hash was loaded");

			index.validate_folder(file_path.folder(), true, platform::now());

			const auto item = index.find_item(file_path);
			result.crc = item.crc32c.load();
			result.has_phash = item.phash.load() != nullptr;

			db.perform_writes();
			db.close();
		}

		index_state reopened(as, locations);
		database db(reopened);
		db.open(db_name.folder(), db_name.file_name_without_extension());

		const auto stored = reopened.find_item(file_path);
		result.stored_crc = stored.crc32c.load();
		result.has_stored_phash = stored.phash.load() != nullptr;
		return result;
	};

	// Scanned after the bytes were last written: nothing has happened to the file since we hashed it.
	const auto scanned_after = df::date_t(file_modified).add_day(1);
	const auto kept = after_validation(scanned_after);
	assert_equal(cached_crc, kept.crc,
	             "a hash of bytes nothing has touched survives the first validation after a launch");
	assert_equal(true, kept.has_phash, "and so does the picture hash");
	assert_equal(cached_crc, kept.stored_crc, "and both stay stored for the next launch");
	assert_equal(true, kept.has_stored_phash, "the picture hash included");

	// Scanned before the bytes were last written: the file was edited while we were not looking, so
	// the stored hashes describe bytes that are gone. The picture hash is persisted, so one left
	// behind here reports the edited file and its untouched copy as the same picture every session.
	const auto scanned_before = df::date_t(file_modified).add_day(-1);
	const auto cleared = after_validation(scanned_before);
	assert_equal(0u, cleared.crc, "a hash of bytes that have since been rewritten is cleared");
	assert_equal(false, cleared.has_phash, "and so is the picture hash");
	assert_equal(0u, cleared.stored_crc, "from the stored row too, or the next launch loads it back");
	assert_equal(false, cleared.has_stored_phash, "the picture hash included");
}

// Verifies the item-level half of hydration recovery: an item whose thumbnail could not be
// loaded while it was a cloud placeholder is allowed to load it again once the file transitions
// from offline to online (item_element::update).
static void should_clear_failed_thumbnail_on_hydration()
{
	const auto file_path = test_files_folder.combine_file("Test.jpg");

	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);

	index.validate_folder(file_path.folder(), true, platform::now());

	auto info = index.find_item(file_path);
	info.ft = files::file_type_from_name(file_path.name());

	const auto item = std::make_shared<df::item_element>(file_path, info);

	// Offline placeholder whose thumbnail could not be loaded from the shell cache.
	auto offline_info = info;
	offline_info.flags |= df::index_item_flags::is_offline;
	item->update(file_path, offline_info);
	item->failed_loading_thumbnail(true);

	assert_equal(static_cast<int>(df::item_online_status::offline), static_cast<int>(item->online_status()),
	             "offline before hydration");
	assert_equal(true, item->failed_loading_thumbnail(), "thumbnail marked failed while offline");

	// Hydrated: the same item is now reported online.
	auto online_info = info;
	online_info.flags &= ~df::index_item_flags::is_offline;
	item->update(file_path, online_info);

	assert_equal(static_cast<int>(df::item_online_status::disk), static_cast<int>(item->online_status()),
	             "online after hydration");
	assert_equal(false, item->failed_loading_thumbnail(), "thumbnail failure cleared after hydration");
}

// Reproduces the "summary not refreshed after download" report: the SAME displayed item element
// (not a freshly created one) must have its metadata refreshed after the file is hydrated and
// re-scanned, so the first summary section (camera/tags read from item->metadata()) updates
// without re-selecting the photo.
static void should_refresh_same_item_metadata_after_hydration()
{
	const auto index_path = _temps.next_path();
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	database db(index);
	db.open(index_path.folder(), index_path.file_name_without_extension());

	// Phase 1: the file is an online-only placeholder; scan it and keep the displayed item.
	platform::test_offline_predicate = [file_path](const df::file_path& p) { return p == file_path; };
	const df::scope_exit clear_offline([] { platform::test_offline_predicate = nullptr; });

	const auto item = load_item(index, file_path, true);
	const auto offline_md = item->metadata();
	const std::string offline_camera(offline_md ? offline_md->camera_model.sv() : std::string_view{});

	// Phase 2: the file is hydrated. Re-scan THE SAME item element (as queue_scan_modified_items
	// does for the currently displayed item).
	platform::test_offline_predicate = nullptr;

	df::item_set to_rescan;
	item->add_to(to_rescan);
	index.scan_items(to_rescan, true, true, false, false, {});

	const auto online_md = item->metadata();
	const std::string online_tags(online_md ? online_md->tags.sv() : std::string_view{});
	const std::string online_camera(online_md ? online_md->camera_model.sv() : std::string_view{});

	// Camera is the discriminator: the offline shell property scan never provides a camera model
	// (unlike tags, which the shell can supply via System.Keywords), so it must appear only after
	// the full online re-scan of the hydrated file.
	assert_equal(true, offline_camera.empty(), "offline shell scan provides no camera model before hydration");
	assert_equal("key1 key2 key3", online_tags,
	             "same displayed item metadata refreshed with tags after hydration re-scan");
	assert_equal("Canon EOS 7D", online_camera,
	             "same displayed item metadata refreshed with camera after hydration re-scan");
}

// Verifies the display-trigger timing fix (view_state::rescan_hydrated_display_item): a cloud
// placeholder being viewed must not be re-indexed until the FULL-file metadata scan has completed
// (display_state_t::_full_metadata_loaded). Triggering earlier -- e.g. off a partial preview texture
// load, which is enough to show the image but only partially hydrates the placeholder -- would
// re-scan the file while it is still offline and miss its tags (the bug that required an F5). Once
// the full metadata has loaded (the file is fully hydrated) the same view frame triggers the online
// re-index and the item gains its tags.
static void should_trigger_rescan_only_after_full_metadata_load()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	common_display_state_t common;
	view_state s(ss, as, index, make_test_player());

	// Phase 1: index the file as an online-only placeholder.
	platform::test_offline_predicate = [file_path](const df::file_path& p) { return p == file_path; };
	const df::scope_exit clear_offline([] { platform::test_offline_predicate = nullptr; });
	const auto item = load_item(index, file_path, true);

	// The file has since been hydrated (predicate cleared): its bytes are fully present on disk.
	platform::test_offline_predicate = nullptr;

	// Build a display for the item with a loaded texture, mimicking the big-view render state.
	const auto d = std::make_shared<display_state_t>(as, common);
	d->_item1 = item;
	d->_selected_texture1 = std::make_shared<texture_state>(as, item);
	d->_selected_texture1->load_image(item); // synchronous under null_async_strategy
	s._display = d;

	const auto texture_loaded = !d->_selected_texture1->loaded().is_empty();

	// Phase 2a: the full-file metadata scan has NOT finished. The rescan must NOT fire, so the item
	// stays offline (this is the bug that left the tag list missing until F5).
	d->_full_metadata_loaded = false;
	s.rescan_hydrated_display_item();
	const auto status_before = item->online_status();
	const auto md_before = item->metadata();
	// Camera is the "not yet re-indexed" signal: the offline shell scan can supply tags (via
	// System.Keywords) but never a camera model, so that only appears after the full online re-scan.
	const std::string camera_before(md_before ? md_before->camera_model.sv() : std::string_view{});

	// Phase 2b: the full-file metadata scan has completed (the placeholder is fully hydrated). The
	// same view frame now triggers the online re-index; the item flips online and gains its tags.
	d->_full_metadata_loaded = true;
	s.rescan_hydrated_display_item();
	const auto status_after = item->online_status();
	const auto md_after = item->metadata();
	const std::string tags_after(md_after ? md_after->tags.sv() : std::string_view{});
	const std::string camera_after(md_after ? md_after->camera_model.sv() : std::string_view{});

	// Phase 3: the trigger is one-shot -- calling again must not change anything.
	s.rescan_hydrated_display_item();
	const auto status_repeat = item->online_status();

	s._display.reset();

	assert_equal(true, texture_loaded, "texture image loaded for display");
	assert_equal(static_cast<int>(df::item_online_status::offline), static_cast<int>(status_before),
	             "no premature re-index before full metadata load");
	assert_equal(true, camera_before.empty(),
	             "no camera model before full metadata load (offline shell scan lacks it)");
	assert_equal(static_cast<int>(df::item_online_status::disk), static_cast<int>(status_after),
	             "item re-indexed online after full metadata load");
	assert_equal("key1 key2 key3", tags_after, "tags picked up once full metadata load triggers re-index");
	assert_equal("Canon EOS 7D", camera_after, "camera picked up once full metadata load triggers re-index");
	assert_equal(static_cast<int>(df::item_online_status::disk), static_cast<int>(status_repeat),
	             "trigger remains one-shot after completing");
}

// A load claims _photo_loaded before it runs. If the load then fails and the claim stands, the
// texture is retained under its path and reused when the user navigates back onto it -- and both the
// paths that would load it (display_state_t::populate and texture_state::refresh) skip a texture that
// reports itself loaded. The image would then never load and never report a problem, leaving the
// thumbnail standing in for it for as long as the texture stayed cached.
static void should_retry_after_failed_load()
{
	const auto good_path = _temps.next_path(".jpg");
	const auto missing_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), good_path, false, true);
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), missing_path, false, true);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto good_item = load_item(index, good_path, true);
	const auto missing_item = load_item(index, missing_path, true);

	// Indexed while present, then removed, so the load is guaranteed to fail.
	platform::delete_file(missing_path);

	const auto failed = std::make_shared<texture_state>(as, missing_item);
	failed->load_image(missing_item); // synchronous under null_async_strategy

	assert_equal(false, failed->_photo_loaded, "failed load does not leave the image claimed as loaded");
	assert_equal(true, failed->_load_retry_pending, "failed load arms the retry that arriving on the item uses");
	assert_equal(true, failed->_is_placeholder, "failed load still shows the thumbnail standing in");

	// Arriving on the item is what must recover. refresh() is the per-tick path the display runs, and
	// it loads only when the texture does not claim to be loaded -- so this fails if the claim stands.
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), missing_path, false, true);
	failed->refresh(missing_item);

	assert_equal(true, failed->_photo_loaded, "arriving on the item after a failed load loads it");
	assert_equal(false, failed->_is_placeholder, "recovered load replaces the thumbnail with the image");
	assert_equal(false, failed->loaded().is_empty(), "recovered load holds the decoded image");

	// The success path must keep working: the claim stands and the decoded image replaces the thumb.
	const auto loaded = std::make_shared<texture_state>(as, good_item);
	loaded->load_image(good_item);

	assert_equal(true, loaded->_photo_loaded, "successful load claims the image so arriving does not reload");
	assert_equal(false, loaded->_is_placeholder, "successful load replaces the thumbnail with the image");
	assert_equal(false, loaded->loaded().is_empty(), "successful load holds the decoded image");
	assert_equal(false, loaded->_load_retry_pending, "successful load arms no retry");
}

static void should_report_exhausted_image_loads()
{
	for (const auto seeded : {false, true})
	{
		const auto path = _temps.next_path(".jpg");
		platform::copy_file(test_files_folder.combine_file("Test.jpg"), path, false, true);
		null_async_strategy as;
		const location_cache locations;
		index_state index(as, locations);
		const auto item = load_item(index, path, true);
		const auto texture = std::make_shared<texture_state>(as, item);
		if (seeded) texture->seed_placeholder(item);
		assert_equal(seeded, texture->has_visual(), "the fixture controls whether a thumbnail is available");
		{
			std::ofstream out(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
			out << "not a JPEG";
			assert_equal(true, out.good(), "invalid image fixture written");
		}

		texture->load_image(item);
		assert_equal(true, texture->_load_retry_pending, "transient failures still retry");
		texture->refresh(item);
		texture->refresh(item);
		assert_equal(false, texture->_load_retry_pending, "retries stop after three failures");
		assert_equal(true, texture->_display_problem == texture_state::display_problem::failed,
		             "an exhausted load publishes its failure");
		assert_equal(false, texture->has_visual(), "a stale thumbnail cannot impersonate the failed image");
		texture->seed_placeholder(item);
		texture->refresh(item);
		assert_equal(false, texture->has_visual(), "refresh cannot cover the terminal error with a thumbnail");

		platform::copy_file(test_files_folder.combine_file("Test.jpg"), path, false, true);
		const auto request = index_state::make_scan_request(item, false, false);
		file_scan_result scanned;
		scanned.success = true;
		index.apply_scan_now(request, scanned, true, item->file_modified().add_day(1));
		texture->refresh(item);
		assert_equal(true, texture->_photo_loaded, "a changed source retries after terminal failure");
		assert_equal(true, texture->_display_problem == texture_state::display_problem::none,
		             "successful recovery clears the failure");
		assert_equal(false, texture->loaded().is_empty(), "recovery holds the real image");
	}
}

// With no item-to-item fade there is no outgoing image to cover phase 1's latency, so anything the
// display cannot draw at once is a visible flash of the shaped grey rectangle. The browser has
// already decoded a thumbnail surface for every visible item, and adopting it is what makes phase 1
// immediate rather than a render-worker round trip. Demotion off the display gives up every decoded
// representation, so returning to a cached item needs the same seed or it waits out a full decode.
static void should_seed_placeholder_from_staged_thumbnail()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto item = load_item(index, file_path, true);
	assert_equal(true, item->has_cached_surface(), "fixture staged the item's thumbnail surface");

	const auto tex = std::make_shared<texture_state>(as, item);
	assert_equal(false, tex->has_visual(), "a fresh texture state has nothing to draw before it is seeded");

	tex->seed_placeholder(item);
	assert_equal(true, tex->has_visual(), "selection draws the staged thumbnail without waiting on a decode");
	assert_equal(true, tex->is_provisional(), "the thumbnail standing in is still marked provisional");

	// Seeding must never step backwards onto something better.
	tex->load_image(item); // synchronous under null_async_strategy
	assert_equal(false, tex->_is_placeholder, "load replaced the thumbnail with the image");
	tex->seed_placeholder(item);
	assert_equal(false, tex->_is_placeholder, "seeding does not reinstate the thumbnail over a loaded image");

	tex->release_decoded_surfaces();
	assert_equal(false, tex->has_visual(), "demotion gives up every representation the entry decoded");
	assert_equal(false, tex->loaded().is_empty(), "demotion keeps the loaded image so no file is re-read");

	tex->seed_placeholder(item);
	assert_equal(true, tex->has_visual(), "returning to a demoted item draws the thumbnail at once");
}

// A thumbnail budget that evicted without re-arming the database query would leave the item blank for
// as long as the search stayed open: update_visible_items_list only asks the database for items whose
// query is still pending, and every other route to a thumbnail is a file scan.
static void should_trim_thumbnail_blobs_by_distance()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::item_elements items;

	for (auto i = 0; i < 4; ++i)
	{
		const auto path = _temps.next_path(".jpg");
		platform::copy_file(test_files_folder.combine_file("Test.jpg"), path, false, true);
		items.emplace_back(load_item(index, path, true));
	}

	// Two items on screen, one a viewport below, one far below.
	constexpr recti viewport{0, 0, 400, 300};
	items[0]->bounds = {0, 0, 200, 150};
	items[1]->bounds = {200, 0, 400, 150};
	items[2]->bounds = {0, 700, 200, 850};
	items[3]->bounds = {0, 3000, 200, 3150};

	size_t total = 0;

	for (const auto& i : items)
	{
		assert_equal(true, i->has_thumb(), "fixture loaded a thumbnail", "trim thumbnails");
		assert_equal(true, i->begin_db_thumbnail_query() != 0, "database query starts pending", "trim thumbnails");
		total += i->thumbnail_blob_bytes();
	}

	assert_equal(0, static_cast<int>(df::trim_thumbnail_blobs(items, viewport, total)),
	             "nothing released while inside the budget", "trim thumbnails");
	assert_equal(true, items[3]->has_thumb(), "the furthest item keeps its thumbnail inside the budget",
	             "trim thumbnails");

	// Items is not always the laid-out view, and ranking by distance from nothing would dump the set.
	assert_equal(0, static_cast<int>(df::trim_thumbnail_blobs(items, recti{}, 1)),
	             "an empty viewport releases nothing", "trim thumbnails");
	assert_equal(true, items[3]->has_thumb(), "an empty viewport keeps every thumbnail", "trim thumbnails");

	const auto dims_before = items[3]->layout_dims();

	assert_equal(true, df::trim_thumbnail_blobs(items, viewport, 1) > 0, "over budget releases something",
	             "trim thumbnails");
	assert_equal(true, items[0]->has_thumb(), "an item in the viewport keeps its thumbnail", "trim thumbnails");
	assert_equal(true, items[1]->has_thumb(), "an item in the viewport keeps its thumbnail", "trim thumbnails");
	assert_equal(false, items[2]->has_thumb(), "an item a viewport away gives its thumbnail up", "trim thumbnails");
	assert_equal(false, items[3]->has_thumb(), "the furthest item gives its thumbnail up", "trim thumbnails");

	assert_equal(true, items[3]->begin_db_thumbnail_query() != 0, "an evicted item re-asks the database",
	             "trim thumbnails");
	assert_equal(false, items[0]->begin_db_thumbnail_query() != 0, "a retained item does not re-ask",
	             "trim thumbnails");
	assert_equal(dims_before.cx, items[3]->layout_dims().cx, "eviction does not reflow the row", "trim thumbnails");
	assert_equal(dims_before.cy, items[3]->layout_dims().cy, "eviction does not reflow the row", "trim thumbnails");
}

// The recent-texture cache retains by form, not by recency. An entry that is no longer displayed
// gives up everything it decoded, because re-decoding an encoded file at display size costs less than
// the surface costs to hold -- but a format that decoded straight to a surface has no encoded form to
// fall back on, and dropping its pixels would send it back to the file for a full native-size decode.
static void should_retain_undisplayed_images_by_form()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto jpeg_path = _temps.next_path(".jpg");
	const auto heif_path = _temps.next_path(".heic");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), jpeg_path, false, true);
	platform::copy_file(test_files_folder.combine("excluded1").combine_file("melnik-rotated.heic"), heif_path, false,
	                    true);

	const auto encoded = std::make_shared<texture_state>(as, load_item(index, jpeg_path, true));
	const auto surface_only = std::make_shared<texture_state>(as, load_item(index, heif_path, true));

	encoded->load_image(load_item(index, jpeg_path, false)); // synchronous under null_async_strategy
	surface_only->load_image(load_item(index, heif_path, false));

	assert_equal(false, encoded->loaded().is_empty(), "jpeg loaded", "retain by form");
	assert_equal(false, surface_only->loaded().is_empty(), "heif loaded", "retain by form");
	assert_equal(0, static_cast<int>(encoded->retained_decoded_bytes()),
	             "a jpeg holds no decoded pixels before it is drawn", "retain by form");
	assert_equal(true, surface_only->retained_decoded_bytes() > 0,
	             "a heif decodes straight to a surface, so its pixels are its only representation",
	             "retain by form");

	encoded->release_decoded_surfaces();
	surface_only->release_decoded_surfaces();

	assert_equal(0, static_cast<int>(encoded->retained_decoded_bytes()), "the jpeg keeps nothing decoded",
	             "retain by form");
	assert_equal(true, surface_only->retained_decoded_bytes() > 0,
	             "the heif keeps the surface it cannot cheaply rebuild", "retain by form");

	// Both must still be loaded, or the next draw returns to the file and the cache achieved nothing.
	assert_equal(false, encoded->loaded().is_empty(), "release keeps the jpeg loaded", "retain by form");
	assert_equal(false, surface_only->loaded().is_empty(), "release keeps the heif loaded", "retain by form");
	assert_equal(true, encoded->_photo_loaded, "release does not re-arm a file load", "retain by form");
	assert_equal(true, surface_only->_photo_loaded, "release does not re-arm a file load", "retain by form");
}

// Reverse permutation of hydration: a file that was indexed online (tags + content hash) is
// dehydrated back into a cloud-only placeholder (OneDrive "free up space"). Its modified time is
// unchanged, so it must NOT be re-scanned; the item flips back to offline but keeps the previously
// indexed metadata (tags) and content hash rather than losing them to the shell-only offline scan.
static void should_preserve_metadata_when_dehydrated()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	// Online: a full scan produces tags and a content hash.
	const auto item = load_item(index, file_path, true);
	const auto online_md = item->metadata();
	const std::string online_tags(online_md ? online_md->tags.sv() : std::string_view{});
	const auto online_status = item->online_status();
	const auto online_has_crc = item->crc32c() != 0;

	// Dehydrated: the file becomes a cloud placeholder again. Re-scan the same item element.
	platform::test_offline_predicate = [file_path](const df::file_path& p) { return p == file_path; };
	const df::scope_exit clear_offline([] { platform::test_offline_predicate = nullptr; });
	df::item_set to_rescan;
	item->add_to(to_rescan);
	index.scan_items(to_rescan, true, true, false, false, {});
	platform::test_offline_predicate = nullptr;

	const auto offline_md = item->metadata();
	const std::string offline_tags(offline_md ? offline_md->tags.sv() : std::string_view{});
	const auto offline_status = item->online_status();
	const auto offline_has_crc = item->crc32c() != 0;

	assert_equal(static_cast<int>(df::item_online_status::disk), static_cast<int>(online_status),
	             "online after full scan");
	assert_equal("key1 key2 key3", online_tags, "tags read while online");
	assert_equal(true, online_has_crc, "content hash computed while online");

	assert_equal(static_cast<int>(df::item_online_status::offline), static_cast<int>(offline_status),
	             "offline after dehydration");
#ifdef _WIN32
	// The re-scan above is a real scan, so the tags survive it by being read again from the shell
	// property store rather than by being left alone. Elsewhere that scan has nothing to read.
	assert_equal("key1 key2 key3", offline_tags, "tags preserved after dehydration (no re-scan)");
#endif
	assert_equal(true, offline_has_crc, "content hash preserved after dehydration");
}

// Verifies the on-demand shell-thumbnail path for cloud-only placeholders: the thumbnail is fetched
// only for items the user is actually viewing (never during indexing / for the hydrating scan, and
// only for items still on screen -- an off-screen item is abandoned, not stuck), and a hydrated
// (online) item does not use this path at all. Uses test_offline_predicate to mark local files as
// placeholders; under null_async_strategy queue_scan_offline_thumbnails runs synchronously.
static void should_fetch_shell_thumbnail_only_for_offline_visible()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);
	const auto hidden_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), hidden_path, false, true);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	// Online-only placeholders: indexed with metadata only, no thumbnail.
	platform::test_offline_predicate = [file_path, hidden_path](const df::file_path& p)
	{
		return p == file_path || p == hidden_path;
	};
	const df::scope_exit clear_offline([] { platform::test_offline_predicate = nullptr; });
	const auto item = load_item(index, file_path, true);
	const auto hidden = load_item(index, hidden_path, true);
	item->begin_db_thumbnail_query(); // the db-thumbnail query has run; nothing cached yet
	hidden->begin_db_thumbnail_query();

	const auto offline_wants_normal = item->should_load_thumbnail();
	const auto offline_wants_shell = item->should_load_shell_thumbnail();

	// Only the on-screen item is marked visible; the fetcher must skip the off-screen one.
	item->is_visible(true);
	hidden->is_visible(false);

	// Fetch on-demand, exactly as the items view does for visible offline items.
	df::item_set to_load;
	item->add_to(to_load);
	hidden->add_to(to_load);
	index.queue_scan_offline_thumbnails(to_load);

	const auto pending_after = item->shell_thumbnail_pending();
	const auto wants_shell_after = item->should_load_shell_thumbnail();
	// The off-screen item was skipped: its pending flag is cleared (not stuck) and it stays eligible
	// so it is fetched if it later scrolls into view.
	const auto hidden_pending_after = hidden->shell_thumbnail_pending();
	const auto hidden_still_eligible = hidden->should_load_shell_thumbnail();
	const auto hidden_has_thumb = ui::is_valid(hidden->thumbnail());

	// Once hydrated (online), the shell-thumbnail path is not used (the normal scan handles it).
	platform::test_offline_predicate = nullptr;
	const auto online_item = load_item(index, file_path, true);
	online_item->begin_db_thumbnail_query();
	const auto online_wants_shell = online_item->should_load_shell_thumbnail();

	assert_equal(false, offline_wants_normal, "offline item never uses the hydrating thumbnail scan");
	assert_equal(true, offline_wants_shell, "offline visible item requests a shell thumbnail");
	assert_equal(false, pending_after, "shell-thumbnail pending flag cleared after the fetch attempt");
	assert_equal(false, wants_shell_after, "visible offline item does not re-request after a single fetch attempt");
	assert_equal(false, hidden_pending_after, "off-screen item pending flag cleared (abandoned, not stuck)");
	assert_equal(true, hidden_still_eligible,
	             "off-screen item skipped by the fetcher stays eligible for when it scrolls in");
	assert_equal(false, hidden_has_thumb, "off-screen item is not fetched");
	assert_equal(false, online_wants_shell, "hydrated (online) item does not use the shell thumbnail path");
}

// A cloud provider that has not generated a thumbnail returns its generic icon, which the fetcher
// reports as "pending" and the items view retries on a timer. Some files (video is common) never get
// one, so the retry is bounded: after the last attempt the item stops asking and keeps its file-type
// placeholder, and hydration restores its budget. Re-arming a batch abandoned for a newer visible set
// must not spend an attempt.
static void should_bound_shell_thumbnail_retries()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	platform::test_offline_predicate = [file_path](const df::file_path& p) { return p == file_path; };
	const df::scope_exit clear_offline([] { platform::test_offline_predicate = nullptr; });
	const auto item = load_item(index, file_path, true);
	item->begin_db_thumbnail_query();
	item->is_visible(true);

	// Abandoned batches re-arm without consuming the budget.
	for (auto i = 0; i < static_cast<int>(df::max_shell_thumbnail_retries) * 2; ++i)
	{
		item->shell_thumbnail_retry_pending(true, false);
		item->shell_thumbnail_retry_pending(false);
	}

	const auto eligible_after_abandons = item->should_load_shell_thumbnail();

	auto armed = 0;
	auto responses = 0;
	while (item->should_load_shell_thumbnail() && responses < static_cast<int>(df::max_shell_thumbnail_retries) * 4)
	{
		++responses;
		item->shell_thumbnail_retry_pending(true); // provider returned its generic icon again
		if (item->shell_thumbnail_retry_pending()) ++armed;
		item->shell_thumbnail_retry_pending(false); // retry pass re-requests it
	}

	const auto eligible_after_cap = item->should_load_shell_thumbnail();
	const auto failed_after_cap = item->failed_loading_thumbnail();
	const auto has_thumb = ui::is_valid(item->thumbnail());

	assert_equal(true, eligible_after_abandons, "abandoned batches re-arm without spending an attempt");
	assert_equal(static_cast<int>(df::max_shell_thumbnail_retries), armed, "provider retries are bounded");
	assert_equal(false, eligible_after_cap, "item stops asking the provider after the last attempt");
	assert_equal(true, failed_after_cap, "item settles on its file-type placeholder");
	assert_equal(false, has_thumb, "no thumbnail was invented for the item");
}

static void should_discard_stale_thumbnail_surface()
{
	const auto file_path = _temps.next_path(".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_async_strategy scan_async;
	const location_cache locations;
	index_state index(scan_async, locations);
	const auto item = load_item(index, file_path, true);
	assert_equal(true, item->has_thumb(), "scan produced encoded thumbnail");
	item->thumbnail(item->thumbnail(), {});
	assert_equal(false, item->has_cached_surface(), "test reset the decoded surface cache");

	deferred_async_strategy deferred;
	item->stage_thumbnail_surface(deferred);
	deferred.drain_ui();

	item->thumbnail({}, {});
	assert_equal(true, deferred.run_next(async_queue::render), "render work was queued");
	deferred.drain_ui();

	assert_equal(false, item->has_cached_surface(), "stale surface was discarded");
}

static void should_rescan_collection_after_forgetting_cache()
{
	const auto folder = _temps.next_folder("rebuild");
	const auto file_path = _temps.next_path_in(folder, ".jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), file_path, false, true);

	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);

	df::index_roots roots;
	roots.folders.emplace(folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);

	const auto scanned = index.find_item(file_path);
	assert_equal(true, scanned.metadata_scanned.load().is_valid(), "first scan stamped the item");
	assert_equal(true, scanned.metadata.load() != nullptr, "first scan cached metadata");

	index.forget_cached_metadata();

	const auto forgotten = index.find_item(file_path);
	assert_equal(false, forgotten.metadata_scanned.load().is_valid(), "reset clears the scan timestamp");
	assert_equal(true, forgotten.metadata.load() != nullptr,
	             "reset keeps the metadata payload so index-only values survive");

	index.scan_uncached(test_token);

	const auto rescanned = index.find_item(file_path);
	assert_equal(true, rescanned.metadata_scanned.load().is_valid(), "the collection is re-read after a reset");
}

void register_index_tests(view_state& state, test_registry& tests)
{
	//
	// Index
	//
	tests.add("Should find word prefix range"s, should_find_word_prefix_range);
	tests.add("Should encode postings"s, should_encode_postings);
	tests.add("Should query inverted index"s, should_query_inverted_index);
	tests.add("Should query trigram index"s, should_query_trigram_index);
	tests.add("Should materialize detached query item"s, should_materialize_detached_query_item);
	tests.add("Should batch thumbnail publication"s, should_batch_thumbnail_publication);
	// SRC-039 - folder-thumbnail traversal must charge every admitted child against the budget.
	tests.add("Should bound folder thumbnail candidate visits"s, should_bound_folder_thumbnail_candidate_visits);
	// SRC-055 - stale local/database thumbnail publications must not overwrite current requests.
	tests.add("Should discard stale thumbnail publications"s, should_discard_stale_thumbnail_publications);
	// G07a review - database thumbnail requests carry generation without shifting file/folder flags.
	tests.add("Should build thumbnail requests with currency"s, should_build_thumbnail_requests_with_currency);
	// G07a review - a non-owner thumbnail install must not strand an owner loading claim.
	tests.add("Should release thumbnail loading after non-owner install"s,
	          should_release_thumbnail_loading_after_non_owner_install);
	tests.add("Should skip unneeded thumbnail staging"s, should_skip_unneeded_thumbnail_staging);
	tests.add("Should reuse cached thumbnail surface"s, should_reuse_cached_thumbnail_surface);
	tests.add("Should index"s, should_index);
	tests.add("Should create database schema"s, should_create_database_schema);
	// SRC-036 - rollback fallback must survive schema creation.
	tests.add("Should keep database rollback journal schema initialization"s,
	          should_keep_rollback_journal_schema_initialization);
	// SRC-037 - environmental schema-check failures must not replace healthy database bytes.
	tests.add("Should classify database schema check failures before replacement"s,
	          should_classify_schema_check_failures_before_replacement);
	tests.add("Should open database in WAL mode"s, should_open_database_in_wal_mode);
	tests.add("Should store thumbnails"s, should_store_thumbnails);
	tests.add("Should store cover art"s, should_store_cover_art);
	tests.add("Should store item properties"s, should_store_item_properties);
	// Concurrency review #11 - a rescan wrote the position loaded at startup over a saved one.
	tests.add("Should hold a saved playback position in the index"s,
	          should_hold_a_saved_playback_position_in_the_index);
	tests.add("Should invalidate cached metadata from an older build"s,
	          should_invalidate_cached_metadata_written_by_an_older_build);
	tests.add("Should keep answering while upgrading to the date pack"s,
	          should_keep_answering_while_upgrading_to_the_date_pack);
	tests.add("Should keep answering while upgrading audio metadata"s,
	          [] { should_keep_answering_while_upgrading_audio_metadata(3); });
	tests.add("Should keep answering while upgrading to libfiles media scans"s,
	          [] { should_keep_answering_while_upgrading_audio_metadata(4); });
	tests.add("Should reclaim a cache written by a newer build"s,
	          should_reclaim_a_cache_written_by_a_newer_build);
	tests.add("Should replace an unreadable database"s, should_replace_an_unreadable_database);
	tests.add("Should run without a database"s, should_run_without_a_database);
	tests.add("Should hand scan results to the database in groups"s,
	          should_hand_scan_results_to_the_database_in_groups);
	// SRC-038/MOD-013/SRC-054 - cache serialization keeps channels, high rates, and bounded ratings.
	tests.add("Should store pack properties"s, should_pack_item_properties);
	// MOD-013 - rollback builds must still read fields around widened sample-rate records.
	tests.add("Should write pack rows a rollback build can read"s,
	          should_write_pack_rows_a_rollback_build_can_read);
	tests.add("Should write dates an older build can read"s, should_write_dates_an_older_build_can_read);
	tests.add("Should read a date pack written by a later release"s,
	          should_read_a_date_pack_written_by_a_later_release);
	tests.add("Should fall back when a date pack names no source this build knows"s,
	          should_fall_back_when_a_pack_names_no_source_this_build_knows);
	tests.add("Should store webservice results"s, should_store_webservice_results);
	tests.add("Should bound webservice cache"s, should_bound_webservice_cache);
	tests.add("Should detect duplicates"s, should_detect_duplicates);
	tests.add("Should drop deleted items from a search with no folder"s,
	          should_drop_deleted_items_from_a_search_with_no_folder);
	tests.add("Should drop descendants of a deleted folder"s, should_drop_descendants_of_a_deleted_folder);
	tests.add("Should search a recursive wildcard through subfolders"s,
	          should_search_a_recursive_wildcard_through_subfolders);
#ifndef _WIN32
	// PLAT-009 - Linux recursive traversal followed directory symlinks.
	tests.add("Should search a recursive wildcard without following directory symlinks"s,
	          should_search_a_recursive_wildcard_without_following_directory_symlinks);
#endif
	tests.add("Should request a re-query only when a folder changed"s,
	          should_request_a_re_query_only_when_a_folder_changed);
	tests.add("Should require equal size for duplicate CRC"s, should_require_equal_size_for_duplicate_crc);
	tests.add("Should bound weak duplicate buckets"s, should_bound_weak_duplicate_buckets);
	// Issue #137 - the presence badge said "1" on files with no duplicate
	tests.add("Should badge only a duplicated item"s, should_badge_only_a_duplicated_item);
	tests.add("Should report a re-encoded copy to presence"s, should_report_a_re_encoded_copy_to_presence);
	tests.add("Should report a rotated copy to presence"s, should_report_a_rotated_copy_to_presence);
	tests.add("Should update collection presence"s, should_update_collection_presence);
	tests.add("Should discard stale presence result"s, should_discard_stale_presence_result);
	tests.add("Should discard stale scan item update"s, should_discard_stale_scan_item_update);
	tests.add("Should discard stale CRC result"s, should_discard_stale_crc_result);
	tests.add("Should continue predictions after terminal phash publication"s,
	          should_continue_predictions_after_terminal_phash_publication);
	// Concurrency review #2 - a rescan clearing the anchor's hash mid-walk crashed the comparison.
	tests.add("Should compare against the anchor hash it chose"s, should_compare_against_the_anchor_hash_it_chose);
	tests.add("Should mark oversized capture time as crowded"s,
	          should_mark_oversized_capture_time_as_crowded);
	tests.add("Should not report presence against crowd declined members"s,
	          should_not_report_presence_against_crowd_declined_members);
	tests.add("Should not reload thumb when valid"s, should_not_reload_thumb_when_valid);
	tests.add("Should reuse persisted hover thumbnail until video changes"s,
	          should_reuse_persisted_hover_thumbnail_until_video_changes);
	tests.add("Should reload thumb after scan"s, should_reload_thumb_after_scan);
	tests.add("Should not reread after metadata write"s, should_not_reread_after_metadata_write);
	// SRC-051 - coherent post-write scans must invalidate revision-dependent hashes.
	tests.add("Should clear hashes after coherent content change"s, should_clear_hashes_after_coherent_content_change);
	tests.add("Should count overlapping write claims"s, should_count_overlapping_write_claims);
	tests.add("Should detect rotation"s, should_detect_rotation);
	tests.add("Should parse roots"s, should_parse_roots);
	tests.add("Should parse drive label roots"s, should_parse_drive_label_roots);
	tests.add("Should apply collection exclusions before membership shortcuts"s,
	          should_apply_collection_exclusions_before_membership_shortcuts);
	tests.add("Should restore cached offline collection descendant membership"s,
	          should_restore_cached_offline_collection_descendant_membership);
	tests.add("Should not restore deleted cached collection folders"s,
	          should_not_restore_deleted_cached_collection_folders);
	// Concurrency review #9 - the startup query validated a folder before the cache merged into it.
	tests.add("Should not adopt cached hashes older than the file"s,
	          should_not_adopt_cached_hashes_older_than_the_file);
	tests.add("Should drop deleted declared root cache"s,
	          should_drop_deleted_declared_root_cache);
	tests.add("Should report incomplete collection discovery"s,
	          should_report_incomplete_collection_discovery);
	tests.add("Should not cancel replacement collection walk when scan starts"s,
	          should_not_cancel_replacement_collection_walk_when_scan_starts);
	// SRC-047 - changing the selected sidecar set invalidates effective metadata.
	tests.add("Should invalidate metadata when sidecar identity changes"s,
	          should_invalidate_metadata_when_sidecar_identity_changes);
	// G07a review - unchanged sidecar associations loaded from the database must stay cache-current.
	tests.add("Should not rescan unchanged sidecars after reload"s,
	          should_not_rescan_unchanged_sidecars_after_reload);
	tests.add("Should index concurrently"s, should_index_concurrently);
	tests.add("Should not publish a stale folder rebuild"s, should_not_publish_a_stale_folder_rebuild);
	// SRC-048 - in-place scan publications must make older folder rebuild snapshots stale.
	tests.add("Should keep index folder rebuild from overwriting in-place content"s,
	          should_not_publish_a_folder_rebuild_over_in_place_content);
	tests.add("Should retry index folder validation from current content"s,
	          should_retry_folder_validation_from_current_content);
	tests.add("Should defer index folder validation after repeated content changes"s,
	          should_defer_folder_validation_after_repeated_content_changes);
	tests.add("Should keep index validation resets across content retry"s,
	          should_keep_validation_resets_across_content_retry);
	// SRC-048 - child snapshots must publish in the same order as the folder map.
	tests.add("Should keep index parent child current after replacement ordering"s,
	          should_keep_parent_child_current_after_replacement_ordering);
	tests.add("Should retry index parent validation after child publication"s,
	          should_retry_parent_validation_after_child_publication);
	// SRC-048 - a validation deferred to concurrent writers neither drops the folder from a walk or
	// a scan nor queues more than one follow-up for it.
	tests.add("Should rescan index folder after deferred folder scan"s, should_rescan_folder_after_deferred_folder_scan);
	tests.add("Should revisit deferred folder during index walk"s, should_revisit_deferred_folder_during_index_walk);
	tests.add("Should list deferred index folder in import analysis"s, should_list_deferred_folder_in_import_analysis);
	tests.add("Should scan forced index item when folder validation defers"s,
	          should_scan_forced_item_when_folder_validation_defers);
	// SRC-052 - tag companion recommendations are bounded without truncating tag authority.
	tests.add("Should bound index tag companion recommendations"s, should_bound_tag_companion_recommendations);
	// SRC-056 - Paint reads one coherent progress snapshot instead of paired plain counters.
	tests.add("Should publish coherent indexing progress"s, should_publish_coherent_indexing_progress);
	// SRC-049 - offline shell metadata publication refreshes search prefilter masks.
	tests.add("Should index offline OneDrive placeholder"s, should_index_offline_placeholder);
	tests.add("Should clear failed thumbnail on hydration"s, should_clear_failed_thumbnail_on_hydration);
	tests.add("Should keep a cached checksum the bytes still describe"s,
	          should_keep_a_cached_checksum_the_bytes_still_describe);
	tests.add("Should refresh same item metadata after hydration"s, should_refresh_same_item_metadata_after_hydration);
	tests.add("Should trigger rescan only after full metadata load"s,
	          should_trigger_rescan_only_after_full_metadata_load);
	tests.add("Should retry after failed load"s, should_retry_after_failed_load);
	// Initial-load failures exhausted their retries without reaching the viewer's error state.
	tests.add("Should report exhausted image loads"s, should_report_exhausted_image_loads);
	tests.add("Should seed placeholder from staged thumbnail"s, should_seed_placeholder_from_staged_thumbnail);
	tests.add("Should trim thumbnail blobs by distance"s, should_trim_thumbnail_blobs_by_distance);
	tests.add("Should retain undisplayed images by form"s, should_retain_undisplayed_images_by_form);
	tests.add("Should preserve metadata when dehydrated"s, should_preserve_metadata_when_dehydrated);
	// G07a review - folder search-presence aggregates are add-only during incremental publication.
	tests.add("Should keep folder presence summary add only"s, should_keep_folder_presence_summary_add_only);
	tests.add("Should fetch shell thumbnail only for offline visible"s,
	          should_fetch_shell_thumbnail_only_for_offline_visible);
	tests.add("Should bound shell thumbnail retries"s, should_bound_shell_thumbnail_retries);
	tests.add("Should discard stale thumbnail surface"s, should_discard_stale_thumbnail_surface);
	tests.add("Should rescan collection after forgetting cache"s, should_rescan_collection_after_forgetting_cache);
}
