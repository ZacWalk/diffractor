// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Helpers the index's own translation units share - model_index.cpp, the duplicate and
// presence pass, and the summary pass - and nothing outside them should reach for.

#pragma once

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
