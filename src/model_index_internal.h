// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Helpers shared by the index and its scan workers.

#pragma once

#if __has_include("libdesktop/platform/storage.h")
#include "libdesktop/platform/storage.h"
#endif

class scan_storage_probe
{
#if __has_include("libdesktop/platform/storage.h")
	platform::storage_probe _storage;
#endif

public:
	bool seek_free([[maybe_unused]] const core::folder_path folder)
	{
#if __has_include("libdesktop/platform/storage.h")
		return _storage.seek_free(folder);
#else
		// With a platform library that cannot identify the medium, keep the scans serial.
		return false;
#endif
	}
};

// A folder's files are kept sorted by name, so a name is found by binary search.
inline df::index_item_infos::iterator find_file(df::index_item_infos& files, const std::string_view name)
{
	auto lb = std::lower_bound(files.begin(), files.end(), name);
	if (lb != files.end() && *lb == name) return lb;
	return files.end();
}

inline df::index_item_infos::const_iterator find_file(const df::index_item_infos& files, const std::string_view name)
{
	auto lb = std::lower_bound(files.begin(), files.end(), name);
	if (lb != files.end() && *lb == name) return lb;
	return files.end();
}

// Whether a file's indexed metadata - or, when asked, its thumbnail - is older than the file or its
// XMP sidecar, so it has to be scanned again.
bool needs_scan_impl(const df::index_folder_item_ptr& f, const df::index_file_item& file, bool thumbnail_needed,
                     bool scan_if_offline);
