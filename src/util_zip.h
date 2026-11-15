// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: ZIP archive handling using minizip. Provides archive creation
// and listing for backup and export features.

#pragma once

struct archive_item;

namespace df
{
	class zip_file final
	{
	protected:
		std::any _handle;

	public:
		zip_file() = default;
		~zip_file();

		bool create(core::file_path path);
		bool close();

		bool add(core::file_path path, std::string_view name, core::date_t modified) const;
		bool add(core::file_path path, std::string_view name) const;
		bool add(core::file_path path);

		static std::vector<archive_item> list(core::file_path zip_file_path);
	};

	// Whether an entry starting at this archive offset and holding this many bytes needs ZIP64 headers.
	bool zip_entry_needs_zip64(uint64_t offset, uint64_t size);
}
