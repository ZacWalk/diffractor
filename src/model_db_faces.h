// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The face database - the face book's own SQLite file, kept apart from the index's cache because what it
// holds is the user's and nothing rebuilds it: resetting the index or deleting what face search found leaves it alone.
// Used on the database's thread. Owned by docs/faces.md#64-the-face-database.

#pragma once

#include "model_faces_book.h"

struct sqlite3;

class face_database final : core::no_copy
{
	sqlite3* _db = nullptr;
	core::file_path _path;
	bool _read_only = false;
	// What PRAGMA data_version answered when this connection last read the file, which moves when another connection
	// commits a change.
	mutable int64_t _data_version = -1;

	int64_t data_version() const;

public:
	static constexpr std::string_view file_name = "diffractor-people.db";

	// Only one Diffractor process writes the file: docs/faces.md#64-the-face-database. Another reads it.
	enum class access
	{
		write,
		read,
	};

	face_database() = default;
	~face_database();

	// Initialize an empty file, or open a supported schema. Refuse incompatible files without modifying them. Read
	// alone, an empty or older file is not initialized or brought up to date, and refused.
	bool open(core::file_path path, access mode = access::write);
	void close();
	bool is_open() const { return _db != nullptr; }
	bool is_read_only() const { return _read_only; }
	core::file_path path() const { return _path; }

	// Everything the file holds, into the book.
	bool load(faces::face_book& book) const;
	// Whether another connection has committed a change since this one last loaded the file.
	bool changed_elsewhere() const;
	// One change, in one transaction. What a change deletes is overwritten, and a change that leaves the book empty -
	// the user asking to forget it - is followed by compacting the file, so nothing forgotten stays readable in it.
	bool write(const faces::book_write& write) const;
};
