// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: SQLite database layer for persistent storage. Manages item thumbnails, metadata cache,
// import history, and web service cache with efficient batch writes.

#pragma once

#include "model_items.h"

struct item_db_write;
struct sqlite3;

class metadata_packer;
class index_state;
class db_statement;

using item_writes_t = platform::queue<item_db_write>;

namespace database_test_seams
{
	bool schema_failure_allows_replacement(int sqlite_result);
	int set_journal_mode(sqlite3* db, std::string_view requested);
	int probe_journal_mode(sqlite3* db);
	void fail_next_journal_probe(int sqlite_result);
	size_t folder_thumbnail_visit_budget();
	void visit_folder_thumbnail_candidates(
		const df::folder_path& root,
		const std::function<bool(const df::folder_path&)>& visit,
		const std::function<std::vector<df::folder_path>(const df::folder_path&)>& enumerate);
}

struct item_import
{
	str::cached name = {};
	df::date_t modified = {};
	df::file_size size = {};
	df::date_t imported = {};

	int compare(const item_import& other) const
	{
		const auto cam_diff = icmp(name, other.name);
		if (cam_diff != 0) return cam_diff;
		if (modified.to_int64() < other.modified.to_int64()) return -1;
		if (modified.to_int64() > other.modified.to_int64()) return 1;
		if (size.to_int64() < other.size.to_int64()) return -1;
		if (size.to_int64() > other.size.to_int64()) return 1;
		return 0;
	}
};

struct item_import_hash
{
	size_t operator()(const item_import& i) const
	{
		return crypto::hash_gen(i.name).append(i.modified.to_int64()).append(i.size.to_int64()).result();
	}
};

struct item_import_eq
{
	bool operator()(const item_import& l, const item_import& r) const
	{
		return l.compare(r) == 0;
	}
};

using item_import_set = df::hash_set<item_import, item_import_hash, item_import_eq>;

class database final : public df::no_copy
{
	index_state& _state;
	df::file_path _db_path;
	uint32_t _db_thread_id = 0;
	sqlite3* _db = nullptr;

	std::unique_ptr<db_statement> find_web_request;
	std::unique_ptr<db_statement> find_folder_thumbnail;
	std::unique_ptr<db_statement> find_thumbnail;

	// Day of the last web-service-cache age prune, so that full-table pass runs once a day
	// rather than on every cache write.
	mutable uint32_t _web_cache_pruned_day = 0;

	// Day of the last PRAGMA optimize, so a session-long connection re-analyses as the index grows.
	uint32_t _optimized_day = 0;

	bool is_db_thread() const;
	bool connect();
	bool prepare_database(bool can_replace);
	platform::file_op_result delete_database_files() const;

public:
	struct thumbnail_request
	{
		std::weak_ptr<df::item_element> lifetime;
		df::file_path path;
		df::folder_path folder;
		uint64_t generation = 0;
		bool is_folder = false;
		bool has_thumbnail = false;

		thumbnail_request(std::weak_ptr<df::item_element> lifetime, df::file_path path, df::folder_path folder,
		                  const uint64_t generation, const bool is_folder, const bool has_thumbnail) noexcept;
	};

	using thumbnail_requests = std::vector<thumbnail_request>;
	static thumbnail_request make_thumbnail_request(const df::item_element_ptr& item, uint64_t generation);

	struct db_thumbnail
	{
		ui::const_image_ptr thumb;
		ui::const_image_ptr cover_art;
		df::date_t last_indexed;

		db_thumbnail() noexcept = default;
		db_thumbnail(const db_thumbnail&) = delete;
		db_thumbnail& operator=(const db_thumbnail&) = delete;
		db_thumbnail(db_thumbnail&&) noexcept = default;
		db_thumbnail& operator=(db_thumbnail&&) noexcept = default;
	};


	database(index_state& s);
	~database() override;

	bool is_open() const;
	void load_index_values() const;

	bool has_errors() const;

	std::string web_service_cache(std::string_view key) const;
	void web_service_cache(std::string_view key, std::string_view value) const;

	item_import_set load_item_imports() const;
	void writes_item_imports(const item_import_set& items) const;

	void close();

	void clean(const std::vector<df::file_path>& indexed_items) const;
	db_thumbnail load_thumbnail(df::file_path id) const;
	db_thumbnail load_folder_thumbnail(str::cached folder) const;
	static thumbnail_requests make_thumbnail_requests(const df::item_set& items);
	void load_thumbnails(const index_state& index, const thumbnail_requests& requests) const;
	void open();
	void open(df::folder_path folder, std::string_view file_name);
	void upgrade_cached_metadata();
	bool invalidate_cached_metadata() const;
	bool request_date_pack_rescan() const;
	bool request_audio_metadata_rescan() const;
	void perform_writes();
	void perform_writes(std::deque<item_db_write> writes) const;
	void maintenance(bool is_reset);


	friend class bucket;
	friend class db_blob_builder;
};
