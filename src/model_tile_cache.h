// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: SQLite-backed store for downloaded map tiles. Keeps every tile in one
// map-tiles-cache.db rather than one file per tile, and prunes it by age and size.
// The connection is owned by the map-tile database thread; no other thread may touch it.

#pragma once

struct sqlite3;
struct sqlite3_stmt;
class tile_cache_db;

namespace tile_cache_test_seams
{
	bool failure_allows_replacement(int sqlite_result);
	void fail_next_commit();
	bool wrapper_transaction_active(const tile_cache_db& db);
	bool sqlite_transaction_active(const tile_cache_db& db);
	uint32_t writes_since_prune(const tile_cache_db& db);
}

// Packs a slippy-map tile address into the rowid the tile store is keyed by. Zoom never exceeds 18,
// so x and y stay below 2^20 and the whole address fits in a positive 64-bit integer - which makes
// the lookup a rowid seek and keeps the PNG in the table leaf.
constexpr int64_t map_tile_db_key(const int z, const int x, const int y) noexcept
{
	return (static_cast<int64_t>(z) << 40) | (static_cast<int64_t>(x) << 20) | static_cast<int64_t>(y);
}

// Where the tile database lives: the app cache-data folder, the same place the index database
// (diffractor-cache.db) is opened from, so both rebuildable caches sit together.
df::file_path resolve_tile_cache_db_path();

class tile_cache_db final : public df::no_copy
{
	df::file_path _db_path;
	uint32_t _db_thread_id = 0;
	sqlite3* _db = nullptr;

	sqlite3_stmt* _load = nullptr;
	sqlite3_stmt* _store = nullptr;
	sqlite3_stmt* _touch = nullptr;
	int _last_open_result = 0;

	// Reads are the common case and must not each cost a commit, so the accessed stamps they earn
	// are collected here and written with the batch the worker thread is already draining.
	std::vector<int64_t> _touched;

	bool _in_transaction = false;
	uint32_t _writes_since_prune = 0;

	bool connect_and_prepare();
	bool last_failure_allows_replacement() const;
	void delete_database_files() const;
	void begin();
	bool commit_transaction();
	int exec(std::string_view sql) const;
	int64_t pragma_value(std::string_view pragma) const;

public:
	// The OpenStreetMap tile usage policy requires clients to keep what they download; nothing
	// fetched inside this window is ever evicted, whatever the cache is costing.
	static constexpr uint32_t min_retention_days = 7;
	static constexpr uint32_t max_unused_days = 30;
	static constexpr int64_t max_bytes = 256ll * 1024 * 1024;
	static constexpr uint32_t prune_write_interval = 256;

	tile_cache_db() = default;
	~tile_cache_db();

	bool is_db_thread() const;
	bool is_open() const { return _db != nullptr; }
	const df::file_path& path() const { return _db_path; }

	// Opens, or replaces and reopens a file this build cannot use. A cache that cannot be opened at
	// all leaves every call below a no-op, so the map falls back to fetching each session.
	void open(df::file_path path);
	void close();

	// Empty when absent. A hit stamps the tile as used, which is what the size prune orders by.
	df::blob load(int64_t key);
	void store(int64_t key, df::cspan data);
	void store(int64_t key, df::cspan data, df::date_t when);

	// Commits the batch the worker just drained and runs a prune when writes have earned one.
	void flush();

	void prune();
	void prune(uint32_t unused_days, int64_t max_size_bytes);

	int64_t count() const;
	int64_t used_bytes() const;

	friend bool tile_cache_test_seams::wrapper_transaction_active(const tile_cache_db& db);
	friend bool tile_cache_test_seams::sqlite_transaction_active(const tile_cache_db& db);
	friend uint32_t tile_cache_test_seams::writes_since_prune(const tile_cache_db& db);
};
