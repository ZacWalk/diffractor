// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The face database's SQLite file: its tables, read whole into the face book at start and written one change
// at a time, each in a transaction. Owned by docs/faces.md#64-the-face-database.

#include "pch.h"

#include <sqlite3.h>

#include "model_db_faces.h"

namespace
{
	constexpr int schema_version = 4;
	constexpr std::array<std::string_view, 3>
		tables{"CREATE TABLE face_people (id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND 4294967295), "
	           "name TEXT NOT NULL CHECK(length(name) > 0))",
	           "CREATE TABLE face_marks (id INTEGER PRIMARY KEY, folder TEXT NOT NULL, name TEXT NOT NULL, "
	           "x REAL NOT NULL, y REAL NOT NULL, w REAL NOT NULL, h REAL NOT NULL, position INTEGER NOT NULL, "
	           "person INTEGER REFERENCES face_people(id), hidden INTEGER NOT NULL CHECK(hidden IN (0,1)))",
	           "CREATE TABLE face_rejections (mark INTEGER NOT NULL REFERENCES face_marks(id) ON DELETE CASCADE, "
	           "person INTEGER NOT NULL REFERENCES face_people(id), PRIMARY KEY(mark,person)) WITHOUT ROWID"};
	constexpr std::string_view alias_table =
		"CREATE TABLE face_aliases (name TEXT PRIMARY KEY CHECK(length(name) > 0), "
		"person INTEGER NOT NULL REFERENCES face_people(id) ON DELETE CASCADE) WITHOUT ROWID";
	constexpr std::array<std::string_view, 3> signature_tables{
		"CREATE TABLE face_signatures (id INTEGER PRIMARY KEY, key BLOB NOT NULL UNIQUE, "
		"person INTEGER REFERENCES face_people(id), hidden INTEGER NOT NULL CHECK(hidden IN (0,1)))",
		"CREATE TABLE face_signature_rejections (signature INTEGER NOT NULL REFERENCES face_signatures(id) ON DELETE CASCADE, "
		"person INTEGER NOT NULL REFERENCES face_people(id), PRIMARY KEY(signature,person)) WITHOUT ROWID",
		"CREATE TABLE face_mark_signatures (mark INTEGER PRIMARY KEY REFERENCES face_marks(id) ON DELETE CASCADE, "
		"signature INTEGER NOT NULL REFERENCES face_signatures(id) DEFERRABLE INITIALLY DEFERRED)"};

	// Two little-endian 32-bit words followed by the quantized bytes. No native struct layout in the database.
	using signature_blob = std::array<uint8_t, 8 + faces::vector_size>;

	signature_blob pack_key(const faces::face_signature& key)
	{
		signature_blob bytes;
		const auto scale = std::bit_cast<uint32_t>(key.hash.scale);
		for (size_t i = 0; i < 4; ++i)
		{
			bytes[i] = static_cast<uint8_t>(key.model >> (8 * i));
			bytes[4 + i] = static_cast<uint8_t>(scale >> (8 * i));
		}
		std::memcpy(bytes.data() + 8, key.hash.values.data(), key.hash.values.size());
		return bytes;
	}

	std::optional<faces::face_signature> unpack_key(const std::span<const uint8_t> bytes)
	{
		if (bytes.size() != signature_blob{}.size()) return {};
		faces::face_signature key;
		uint32_t scale = 0;
		for (size_t i = 0; i < 4; ++i)
		{
			key.model |= uint32_t{bytes[i]} << (8 * i);
			scale |= uint32_t{bytes[4 + i]} << (8 * i);
		}
		key.hash.scale = std::bit_cast<float>(scale);
		std::memcpy(key.hash.values.data(), bytes.data() + 8, key.hash.values.size());
		return key.valid() ? std::optional(key) : std::nullopt;
	}

	constexpr auto path_collation = core::case_insensitive_path_identity ? "NOCASE" : "BINARY";

	void log_error(sqlite3* db, const std::string_view what)
	{
		core::log(__FUNCTION__, std::format("Face database: {} [{}]", core::utf8_cast(sqlite3_errmsg(db)), what));
	}

	bool exec(sqlite3* db, const std::string_view sql)
	{
		const auto result = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, nullptr);
		if (result != SQLITE_OK) log_error(db, sql);
		return result == SQLITE_OK;
	}

	class statement final : core::no_copy
	{
		sqlite3* _db;
		sqlite3_stmt* _handle = nullptr;

		int bind(const int i, const std::string_view text) const
		{
			return sqlite3_bind_text(_handle, i, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
		}

		int bind(const int i, const int64_t n) const { return sqlite3_bind_int64(_handle, i, n); }
		int bind(const int i, const double d) const { return sqlite3_bind_double(_handle, i, d); }
		int bind(const int i, const signature_blob& key) const
		{
			return sqlite3_bind_blob(_handle, i, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
		}

	public:
		statement(sqlite3* db, const std::string_view sql) : _db(db)
		{
			if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &_handle, nullptr) != SQLITE_OK)
			{
				log_error(db, sql);
				_handle = nullptr;
			}
		}

		~statement() override
		{
			if (_handle) sqlite3_finalize(_handle);
		}

		// Every invocation binds all parameters, including after a failed write.
		template <typename... Args> bool run(const Args... args) const
		{
			if (!_handle) return false;
			auto i = 0;
			if (!((bind(++i, args) == SQLITE_OK) && ...))
			{
				log_error(_db, "bind");
				return false;
			}
			const auto result = sqlite3_step(_handle);
			if (result != SQLITE_DONE) log_error(_db, "step");
			sqlite3_reset(_handle);
			return result == SQLITE_DONE;
		}

		template <typename Row> bool read(const Row& row) const
		{
			if (!_handle) return false;
			int result;
			while ((result = sqlite3_step(_handle)) == SQLITE_ROW) row(*this);
			if (result != SQLITE_DONE) log_error(_db, "read");
			return result == SQLITE_DONE;
		}

		int64_t integer(const int column) const { return sqlite3_column_int64(_handle, column); }
		double real(const int column) const { return sqlite3_column_double(_handle, column); }
		std::span<const uint8_t> blob(const int column) const
		{
			return {static_cast<const uint8_t*>(sqlite3_column_blob(_handle, column)),
			        static_cast<size_t>(sqlite3_column_bytes(_handle, column))};
		}

		std::string_view text(const int column) const
		{
			const auto* const value = sqlite3_column_text(_handle, column);
			if (!value) return {};
			return {std::bit_cast<const char*>(value), static_cast<size_t>(sqlite3_column_bytes(_handle, column))};
		}
	};
}

face_database::~face_database()
{
	close();
}

bool face_database::open(const core::file_path path, const access mode)
{
	close();
	_path = path;
	_read_only = mode == access::read;
	sqlite3_initialize();

	if (!_read_only && !platform::exists(path.folder())) platform::create_folder(path.folder());

	const auto flags = _read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	if (sqlite3_open_v2(path.str().c_str(), &_db, flags, nullptr) != SQLITE_OK)
	{
		core::log(__FUNCTION__, std::format("The face database could not be opened: {}", path));
		if (_db) sqlite3_close(_db);
		_db = nullptr;
		return false;
	}

	sqlite3_busy_timeout(_db, 1000);

	auto stored = 0;
	{
		const statement version(_db, "PRAGMA user_version");
		if (!version.read([&](const statement& row) { stored = static_cast<int>(row.integer(0)); }))
		{
			close();
			return false;
		}
	}

	bool supported = stored == 0 || stored == 2 || stored == 3 || stored == schema_version;
	// A reader brings nothing up to date: the window writing the file does.
	if (_read_only) supported = stored == schema_version;
	size_t count = 0;
	if (supported)
	{
		// Version zero also describes unrelated, populated SQLite files. Initialize an empty schema only.
		const statement existing(_db,
		                         "SELECT sql FROM sqlite_master WHERE name NOT GLOB 'sqlite_*' AND type != 'index'");
		if (!existing.read([&](const statement& row)
		{
			++count;
			const auto sql = row.text(0);
			if (std::ranges::find(tables, sql) == tables.end() && !(stored >= 3 && sql == alias_table) &&
			    !(stored == schema_version && std::ranges::find(signature_tables, sql) != signature_tables.end()))
				supported = false;
		}))
			supported = false;
		supported = supported && count == (stored == 0 ? 0
		                                               : tables.size() + (stored >= 3) +
		                                                     (stored == schema_version ? signature_tables.size() : 0));
	}
	if (!supported)
	{
		core::log(__FUNCTION__,
		          std::format("Incompatible face database schema (version {}); file left unchanged", stored));
		close();
		return false;
	}
	if (!exec(_db, "PRAGMA foreign_keys = ON"))
	{
		close();
		return false;
	}
	// docs/faces.md#11-privacy-and-recovery: what the user forgets is overwritten, not left in free pages.
	if (!_read_only && !exec(_db, "PRAGMA secure_delete = ON"))
	{
		close();
		return false;
	}
	if (stored != schema_version)
	{
		bool ok = exec(_db, "BEGIN IMMEDIATE");
		if (!stored)
		{
			for (const auto table : tables) ok = ok && exec(_db, table);
			ok = ok &&
			     exec(_db, "CREATE INDEX face_marks_exact ON face_marks(folder,name); "
			               "CREATE INDEX face_marks_place ON face_marks(folder COLLATE NOCASE,name COLLATE NOCASE)");
		}
		if (stored < 3) ok = ok && exec(_db, alias_table);
		for (const auto table : signature_tables) ok = ok && exec(_db, table);
		if (!ok || !exec(_db, std::format("PRAGMA user_version = {}", schema_version)) || !exec(_db, "COMMIT"))
		{
			close(); // An uncommitted initialization or migration rolls back.
			return false;
		}
	}
	return true;
}

void face_database::close()
{
	if (_db)
	{
		sqlite3_close_v2(_db);
		_db = nullptr;
	}
	_data_version = -1;
}

int64_t face_database::data_version() const
{
	int64_t version = -1;
	if (_db)
	{
		const statement read(_db, "PRAGMA data_version");
		if (!read.read([&](const statement& row) { version = row.integer(0); })) version = -1;
	}
	return version;
}

bool face_database::changed_elsewhere() const
{
	const auto now = data_version();
	return now < 0 || now != _data_version;
}

bool face_database::load(faces::face_book& book) const
{
	if (!_db) return false;
	// Taken before reading, so a change committed while the file is read is seen as a change.
	_data_version = data_version();

	faces::roster people;
	faces::marks_by_picture marks;
	faces::signature_marks signatures;

	{
		const statement rows(_db, "SELECT id, name FROM face_people ORDER BY id");
		if (!rows.read([&](const statement& row)
		{ people.people.push_back({static_cast<uint32_t>(row.integer(0)), std::string(row.text(1))}); }))
			return false;
	}

	{
		const statement rows(_db, "SELECT name, person FROM face_aliases");
		if (!rows.read([&](const statement& row)
		{ people.aliases.emplace(row.text(0), static_cast<uint32_t>(row.integer(1))); }))
			return false;
	}

	{
		const statement rows(_db,
		                     "SELECT s.key, s.person, s.hidden, r.person FROM face_signatures s "
		                     "LEFT JOIN face_signature_rejections r ON r.signature = s.id ORDER BY s.id, r.person");
		bool valid = true;
		if (!rows.read(
				[&](const statement& row)
		{
			const auto key = unpack_key(row.blob(0));
			if (!key)
			{
				valid = false;
				return;
			}
			auto& mark = signatures[*key];
			mark.person = static_cast<uint32_t>(row.integer(1));
			mark.hidden = row.integer(2) != 0;
			if (const auto rejected = row.integer(3)) mark.rejected.push_back(static_cast<uint32_t>(rejected));
		}) ||
		    !valid)
			return false;
	}

	{
		const statement rows(_db, "SELECT m.id, m.folder, m.name, m.x, m.y, m.w, m.h, m.position, m.person, "
		                          "m.hidden, r.person, b.signature, s.key FROM face_marks m "
		                          "LEFT JOIN face_rejections r ON r.mark = m.id "
		                          "LEFT JOIN face_mark_signatures b ON b.mark = m.id "
		                          "LEFT JOIN face_signatures s ON s.id = b.signature "
		                          "ORDER BY m.id, r.person");
		int64_t last = 0;
		bool valid = true;
		faces::face_mark* mark = nullptr;
		if (!rows.read(
				[&](const statement& row)
		{
			const auto id = row.integer(0);
			if (!mark || last != id)
			{
				auto& picture = marks[core::file_path(core::folder_path(row.text(1)), row.text(2))];
				mark = &picture.emplace_back();
				mark->box = {row.real(3), row.real(4), row.real(5), row.real(6)};
				mark->position = static_cast<int>(row.integer(7));
				mark->person = static_cast<uint32_t>(row.integer(8));
				mark->hidden = row.integer(9) != 0;
				if (row.integer(11))
				{
					mark->signature = unpack_key(row.blob(12));
					if (!mark->signature) valid = false;
				}
				last = id;
			}
			if (const auto rejected = row.integer(10)) mark->rejected.push_back(static_cast<uint32_t>(rejected));
		}) ||
		    !valid)
			return false;
	}

	book.load(std::move(people), std::move(marks), std::move(signatures));
	return true;
}

bool face_database::write(const faces::book_write& write) const
{
	if (!_db || _read_only || write.empty()) return false;
	if (!exec(_db, "BEGIN IMMEDIATE;")) return false;

	// A busy COMMIT leaves the transaction open. Do not let a failed write poison the connection.
	const core::scope_exit rollback([db = _db]
	{
		if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
	});

	if (write.clear && !exec(_db, "DELETE FROM face_marks; DELETE FROM face_signatures; DELETE FROM face_people;"))
		return false;

	if (!write.people.empty())
	{
		const statement put(_db, "INSERT INTO face_people (id, name) VALUES (?, ?) "
		                         "ON CONFLICT (id) DO UPDATE SET name = excluded.name");
		for (const auto& p : write.people)
		{
			if (!put.run(static_cast<int64_t>(p.id), std::string_view(p.name))) return false;
		}
	}

	if (!write.signatures.empty())
	{
		const statement put(_db, "INSERT INTO face_signatures (key, person, hidden) VALUES (?, NULLIF(?, 0), ?) "
		                         "ON CONFLICT (key) DO UPDATE SET person = excluded.person, hidden = excluded.hidden");
		const statement remove(_db, "DELETE FROM face_signatures WHERE key = ?");
		const statement reset(_db, "DELETE FROM face_signature_rejections WHERE signature = "
		                           "(SELECT id FROM face_signatures WHERE key = ?)");
		const statement reject(_db, "INSERT INTO face_signature_rejections (signature, person) "
		                            "VALUES ((SELECT id FROM face_signatures WHERE key = ?), ?)");
		for (const auto& [key, mark] : write.signatures)
		{
			if (!key.valid()) return false;
			const auto bytes = pack_key(key);
			if (!mark)
			{
				if (!remove.run(bytes)) return false;
				continue;
			}
			if (!put.run(bytes, int64_t{mark->person}, int64_t{mark->hidden}) || !reset.run(bytes)) return false;
			for (const auto person : mark->rejected)
				if (!reject.run(bytes, int64_t{person})) return false;
		}
	}

	// A picture's marks and their rejection sets are replaced together, including case variants on Windows.
	if (!write.pictures.empty())
	{
		const auto remove_sql =
			std::format("DELETE FROM face_marks WHERE folder = ? COLLATE {} AND name = ? COLLATE {}", path_collation,
		                path_collation);
		const statement remove(_db, remove_sql);
		const statement put(_db, "INSERT INTO face_marks (folder, name, x, y, w, h, position, person, hidden) "
		                         "VALUES (?, ?, ?, ?, ?, ?, ?, NULLIF(?, 0), ?)");
		const statement reject(_db, "INSERT INTO face_rejections (mark, person) VALUES (?, ?)");
		const statement link(_db, "INSERT INTO face_mark_signatures (mark, signature) "
		                          "VALUES (?, (SELECT id FROM face_signatures WHERE key = ?))");

		for (const auto& [path, marks] : write.pictures)
		{
			const auto folder = path.folder().text();
			const auto name = path.name();
			if (!remove.run(folder, name)) return false;

			for (const auto& m : marks)
			{
				if (!put.run(folder, name, m.box.X, m.box.Y, m.box.Width, m.box.Height,
				             static_cast<int64_t>(m.position), static_cast<int64_t>(m.person), int64_t{m.hidden}))
					return false;
				const auto id = static_cast<int64_t>(sqlite3_last_insert_rowid(_db));
				if (m.signature && !link.run(id, pack_key(*m.signature))) return false;
				for (const auto person : m.rejected)
					if (!reject.run(id, static_cast<int64_t>(person))) return false;
			}
		}
	}

	if (!write.aliases.empty())
	{
		const statement put(_db, "INSERT INTO face_aliases (name, person) VALUES (?, ?) "
		                         "ON CONFLICT (name) DO UPDATE SET person = excluded.person");
		const statement remove(_db, "DELETE FROM face_aliases WHERE name = ?");
		for (const auto& [name, id] : write.aliases)
			if (!(id ? put.run(std::string_view(name), static_cast<int64_t>(id)) : remove.run(std::string_view(name))))
				return false;
	}

	// Remove retired people only after their marks, rejections and former names have moved.
	if (!write.people_removed.empty())
	{
		const statement remove(_db, "DELETE FROM face_people WHERE id = ?");
		for (const auto id : write.people_removed)
			if (!remove.run(static_cast<int64_t>(id))) return false;
	}
	if (!exec(_db, "COMMIT;")) return false;

	// Forgotten: secure_delete has overwritten what was deleted, and compacting gives the freed pages back, so the
	// file is as small as an empty book's. A reader holding the file open only postpones this to the next forget.
	if (write.clear && write.people.empty() && write.pictures.empty() && write.signatures.empty()) exec(_db, "VACUUM;");
	return true;
}
