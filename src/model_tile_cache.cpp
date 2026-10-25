// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: SQLite-backed store for downloaded map tiles. Owns map-tiles-cache.db, its schema,
// batched writes and the bounded prune that keeps the file from growing without limit.

#include "pch.h"

#include <sqlite3.h>

#include "model_tile_cache.h"

namespace
{
	// Faults are logged and swallowed: a tile cache that misbehaves must cost a redraw, never the map.
	// The index database keeps its own fault count, and nothing here is allowed to reach it.
	void tile_db_error(sqlite3* db, const std::string_view sql)
	{
		df::log(__FUNCTION__, std::format("Tile cache error: {} [{}]", str::utf8_cast(sqlite3_errmsg(db)), sql));
	}

	// Wraps a statement for the length of one call. It either compiles its own SQL or borrows a
	// prepared handle, and in both cases leaves nothing bound behind it.
	class tile_stmt
	{
		sqlite3* _db = nullptr;
		sqlite3_stmt* _handle = nullptr;
		bool _owned = false;

	public:
		tile_stmt(sqlite3* db, const std::string_view sql) : _db(db), _owned(true)
		{
			if (sqlite3_prepare_v2(_db, sql.data(), static_cast<int>(sql.size()), &_handle, nullptr) != SQLITE_OK)
			{
				tile_db_error(_db, sql);
				_handle = nullptr;
			}
		}

		tile_stmt(sqlite3* db, sqlite3_stmt* handle) noexcept : _db(db), _handle(handle)
		{
		}

		tile_stmt(const tile_stmt&) = delete;
		tile_stmt& operator=(const tile_stmt&) = delete;

		~tile_stmt()
		{
			if (_handle == nullptr) return;

			if (_owned)
			{
				sqlite3_finalize(_handle);
			}
			else
			{
				sqlite3_reset(_handle);
				sqlite3_clear_bindings(_handle);
			}
		}

		bool is_valid() const { return _handle != nullptr; }

		void bind(const int i, const int64_t n) const
		{
			if (_handle != nullptr && sqlite3_bind_int64(_handle, i, n) != SQLITE_OK)
			{
				tile_db_error(_db, "sqlite3_bind_int64");
			}
		}

		void bind(const int i, const df::cspan cs) const
		{
			if (_handle != nullptr &&
				sqlite3_bind_blob(_handle, i, cs.data, static_cast<int>(cs.size), SQLITE_STATIC) != SQLITE_OK)
			{
				tile_db_error(_db, "sqlite3_bind_blob");
			}
		}

		bool read() const
		{
			if (_handle == nullptr) return false;

			const auto result = sqlite3_step(_handle);

			if (result == SQLITE_ROW) return true;
			if (result != SQLITE_DONE) tile_db_error(_db, "sqlite3_step");

			return false;
		}

		void exec() const
		{
			while (read())
			{
			}
		}

		df::blob blob(const int i) const
		{
			if (_handle == nullptr) return {};

			const auto* const data = static_cast<const uint8_t*>(sqlite3_column_blob(_handle, i));
			const auto len = sqlite3_column_bytes(_handle, i);

			return data == nullptr ? df::blob{} : df::blob(data, data + len);
		}

		int64_t int64(const int i) const
		{
			return _handle == nullptr ? 0 : sqlite3_column_int64(_handle, i);
		}
	};

	constexpr std::string_view tile_schema_sql =
		"CREATE TABLE IF NOT EXISTS tiles ("
		"id INTEGER PRIMARY KEY,"
		"fetched INTEGER NOT NULL,"
		"accessed INTEGER NOT NULL,"
		"bytes BLOB NOT NULL);"
		"CREATE INDEX IF NOT EXISTS idx_tiles_accessed ON tiles (accessed);";

	// One pass evicts at most this many rows so a prune cannot hold the tile thread for seconds.
	constexpr int prune_batch_rows = 512;
	constexpr int prune_max_batches = 8;
	constexpr int vacuum_batch_pages = 512;
	std::atomic_bool fail_next_commit_flag = false;

	bool tile_failure_allows_replacement(const int result)
	{
		switch (result & 0xff)
		{
		case SQLITE_CORRUPT:
		case SQLITE_NOTADB:
		case SQLITE_FORMAT:
		case SQLITE_ERROR:
			return true;
		default:
			return false;
		}
	}
}

bool tile_cache_test_seams::failure_allows_replacement(const int sqlite_result)
{
	return tile_failure_allows_replacement(sqlite_result);
}

void tile_cache_test_seams::fail_next_commit()
{
	fail_next_commit_flag.store(true);
}

bool tile_cache_test_seams::wrapper_transaction_active(const tile_cache_db& db)
{
	return db._in_transaction;
}

bool tile_cache_test_seams::sqlite_transaction_active(const tile_cache_db& db)
{
	return db._db != nullptr && sqlite3_get_autocommit(db._db) == 0;
}

uint32_t tile_cache_test_seams::writes_since_prune(const tile_cache_db& db)
{
	return db._writes_since_prune;
}

df::file_path resolve_tile_cache_db_path()
{
	// Both caches are rebuildable, so they share the folder the index database uses.
	const auto base = known_path(platform::known_folder::app_cache_data);

	if (!base.exists())
	{
		platform::create_folder(base);
	}

	return base.combine_file("map-tiles-cache.db"sv);
}

tile_cache_db::~tile_cache_db()
{
	close();
}

bool tile_cache_db::is_db_thread() const
{
	return _db_thread_id == platform::current_thread_id();
}

int tile_cache_db::exec(const std::string_view sql) const
{
	const auto result = sqlite3_exec(_db, std::string(sql).c_str(), nullptr, nullptr, nullptr);

	if (result != SQLITE_OK)
	{
		tile_db_error(_db, sql);
	}

	return result;
}

bool tile_cache_db::last_failure_allows_replacement() const
{
	return tile_failure_allows_replacement(_last_open_result);
}

bool tile_cache_db::commit_transaction()
{
	if (!_in_transaction) return true;

	if (fail_next_commit_flag.exchange(false))
	{
		tile_db_error(_db, "COMMIT fault injection"sv);
		if (sqlite3_get_autocommit(_db) == 0)
		{
			exec("ROLLBACK"sv);
		}

		_in_transaction = false;
		return false;
	}

	const auto result = exec("COMMIT"sv);
	if (result == SQLITE_OK)
	{
		_in_transaction = false;
		return true;
	}

	if (sqlite3_get_autocommit(_db) == 0)
	{
		exec("ROLLBACK"sv);
	}

	_in_transaction = false;
	return false;
}

int64_t tile_cache_db::pragma_value(const std::string_view pragma) const
{
	if (_db == nullptr) return 0;

	const tile_stmt statement(_db, pragma);
	return statement.read() ? statement.int64(0) : 0;
}

bool tile_cache_db::connect_and_prepare()
{
	_last_open_result = SQLITE_OK;

	if (sqlite3_open(std::bit_cast<const char*>(_db_path.str().c_str()), &_db) != SQLITE_OK)
	{
		_last_open_result = _db == nullptr ? SQLITE_NOMEM : sqlite3_errcode(_db);
		df::log(__FUNCTION__, std::format("Failed to open tile cache: {}",
		                                  _db ? str::utf8_cast(sqlite3_errmsg(_db)) : "out of memory"));
		return false;
	}

	sqlite3_busy_timeout(_db, 1000);

	// auto_vacuum only takes hold on a database that has no schema yet, and it is what lets a prune
	// hand pages back to the filesystem instead of leaving a file that only ever grows.
	if ((_last_open_result = exec("PRAGMA auto_vacuum=INCREMENTAL;"sv)) != SQLITE_OK) return false;

	exec("PRAGMA journal_mode=WAL;"sv);

	if ((_last_open_result = exec("PRAGMA cache_size=-2048; PRAGMA trusted_schema=OFF;"sv)) != SQLITE_OK) return false;
	if ((_last_open_result = exec(tile_schema_sql)) != SQLITE_OK) return false;
	if ((_last_open_result = exec("PRAGMA user_version=1;"sv)) != SQLITE_OK) return false;

	const auto prepare = [this](sqlite3_stmt** handle, const std::string_view sql)
	{
		if (sqlite3_prepare_v2(_db, sql.data(), static_cast<int>(sql.size()), handle, nullptr) != SQLITE_OK)
		{
			_last_open_result = sqlite3_errcode(_db);
			tile_db_error(_db, sql);
			*handle = nullptr;
			return false;
		}

		return true;
	};

	return prepare(&_load, "SELECT bytes FROM tiles WHERE id = ?1"sv) &&
		prepare(&_store, "INSERT INTO tiles (id, fetched, accessed, bytes) VALUES (?1, ?2, ?2, ?3) "
		        "ON CONFLICT(id) DO UPDATE SET fetched = ?2, accessed = ?2, bytes = ?3"sv) &&
		prepare(&_touch, "UPDATE tiles SET accessed = ?2 WHERE id = ?1"sv);
}

void tile_cache_db::delete_database_files() const
{
	if (!_db_path.exists()) return;

	if (platform::delete_file(_db_path).success())
	{
		// A leftover write-ahead log is replayed into whatever file appears next, which would restore
		// the very content that could not be read.
		for (const auto suffix : {"-wal"sv, "-shm"sv})
		{
			const std::string sibling_name = std::string(_db_path.name()) + std::string(suffix);
			const auto sibling = df::file_path(_db_path.folder(), std::string_view(sibling_name));

			if (sibling.exists())
			{
				platform::delete_file(sibling);
			}
		}
	}
}

void tile_cache_db::open(df::file_path path)
{
	sqlite3_initialize();

	_db_thread_id = platform::current_thread_id();
	_db_path = std::move(path);

	if (connect_and_prepare()) return;

	// Rebuildable tiles justify replacing bytes this build cannot use, but a transient prepare or
	// I/O failure is not evidence that the healthy cache should be deleted.
	if (!last_failure_allows_replacement())
	{
		close();
		df::log(__FUNCTION__, "Tile cache unavailable - map tiles will be fetched this session"sv);
		return;
	}

	close();
	delete_database_files();

	if (connect_and_prepare()) return;

	close();
	df::log(__FUNCTION__, "Tile cache unavailable - map tiles will be fetched each session"sv);
}

void tile_cache_db::close()
{
	if (_db == nullptr) return;

	try
	{
		flush();
	}
	catch (const std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	for (auto** handle : {&_load, &_store, &_touch})
	{
		if (*handle != nullptr)
		{
			sqlite3_finalize(*handle);
			*handle = nullptr;
		}
	}

	if (sqlite3_close_v2(_db) != SQLITE_OK)
	{
		tile_db_error(_db, "sqlite3_close_v2"sv);
	}

	_db = nullptr;
	_touched.clear();
	_in_transaction = false;
}

void tile_cache_db::begin()
{
	if (_db != nullptr && !_in_transaction)
	{
		_in_transaction = exec("BEGIN TRANSACTION"sv) == SQLITE_OK;
	}
}

df::blob tile_cache_db::load(const int64_t key)
{
	if (_db == nullptr) return {};

	df::assert_true(is_db_thread());

	const tile_stmt statement(_db, _load);
	statement.bind(1, key);

	auto result = statement.read() ? statement.blob(0) : df::blob{};

	if (!result.empty())
	{
		_touched.push_back(key);
	}

	return result;
}

void tile_cache_db::store(const int64_t key, const df::cspan data)
{
	store(key, data, platform::now());
}

void tile_cache_db::store(const int64_t key, const df::cspan data, const df::date_t when)
{
	if (_db == nullptr || data.empty()) return;

	df::assert_true(is_db_thread());

	begin();

	const tile_stmt statement(_db, _store);
	statement.bind(1, key);
	statement.bind(2, static_cast<int64_t>(when.to_int64()));
	statement.bind(3, data);
	statement.exec();

	++_writes_since_prune;
}

void tile_cache_db::flush()
{
	if (_db == nullptr) return;

	df::assert_true(is_db_thread());

	if (!_touched.empty())
	{
		begin();

		const auto now = static_cast<int64_t>(platform::now().to_int64());

		for (const auto key : _touched)
		{
			const tile_stmt statement(_db, _touch);
			statement.bind(1, key);
			statement.bind(2, now);
			statement.exec();
		}

		_touched.clear();
	}

	if (!commit_transaction()) return;

	if (_writes_since_prune >= prune_write_interval)
	{
		prune();
	}
}

void tile_cache_db::prune()
{
	prune(max_unused_days, max_bytes);
}

void tile_cache_db::prune(const uint32_t unused_days, const int64_t max_size_bytes)
{
	if (_db == nullptr) return;

	df::assert_true(is_db_thread());

	if (!commit_transaction()) return;

	const auto now = static_cast<int64_t>(platform::now().to_int64());
	const auto days = [now](const uint32_t count)
	{
		const auto span = static_cast<int64_t>(count) * static_cast<int64_t>(df::date_t::intervals_per_day);
		return now > span ? now - span : int64_t{0};
	};

	// Everything fetched after this instant is inside the retention the tile policy asks for and is
	// off limits to both passes below, however large the cache has grown.
	const auto protected_after = days(min_retention_days);

	auto remaining_rows = prune_batch_rows * prune_max_batches;

	{
		const tile_stmt statement(
			_db,
			"DELETE FROM tiles WHERE id IN (SELECT id FROM tiles WHERE accessed < ?1 AND fetched < ?2 LIMIT ?3)"sv);
		statement.bind(1, days(unused_days));
		statement.bind(2, protected_after);
		statement.bind(3, remaining_rows);
		statement.exec();
		remaining_rows -= std::clamp(sqlite3_changes(_db), 0, remaining_rows);
	}

	for (auto pass = 0; pass < prune_max_batches && remaining_rows > 0 && used_bytes() > max_size_bytes; ++pass)
	{
		const auto rows = std::min(prune_batch_rows, remaining_rows);
		const tile_stmt statement(
			_db, "DELETE FROM tiles WHERE id IN (SELECT id FROM tiles WHERE fetched < ?1 ORDER BY accessed LIMIT ?2)"sv);
		statement.bind(1, protected_after);
		statement.bind(2, rows);
		statement.exec();

		// Nothing left to give: what remains is all inside the retention window.
		const auto changed = sqlite3_changes(_db);
		if (changed == 0) break;
		remaining_rows -= std::clamp(changed, 0, remaining_rows);
	}

	exec(std::format("PRAGMA incremental_vacuum({});", vacuum_batch_pages));
	_writes_since_prune = remaining_rows == 0 ? prune_write_interval : 0;
}

int64_t tile_cache_db::count() const
{
	if (_db == nullptr) return 0;

	const tile_stmt statement(_db, "SELECT count(*) FROM tiles"sv);
	return statement.read() ? statement.int64(0) : 0;
}

// Pages the content actually occupies. The free list is excluded because a prune only returns those
// pages at the incremental vacuum, and counting them would make each pass look like it achieved
// nothing.
int64_t tile_cache_db::used_bytes() const
{
	const auto page_size = pragma_value("PRAGMA page_size"sv);
	const auto pages = pragma_value("PRAGMA page_count"sv) - pragma_value("PRAGMA freelist_count"sv);

	return pages > 0 ? pages * page_size : 0;
}
