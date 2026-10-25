// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

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

		bool create(file_path path);
		bool close();

		bool add(file_path path, std::string_view name, date_t modified) const;
		bool add(file_path path, std::string_view name) const;
		bool add(file_path path);

		static std::vector<archive_item> list(file_path zip_file_path);
	};

	// Whether an entry starting at this archive offset and holding this many bytes needs ZIP64 headers.
	bool zip_entry_needs_zip64(uint64_t offset, uint64_t size);
}
