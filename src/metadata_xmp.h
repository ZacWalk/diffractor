// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: XMP metadata parsing and writing over libfiles' XMP data model. Handles extensible
// metadata including ratings, labels, keywords, and editing history.

#pragma once

#include "model_property.h"

class metadata_edits;

struct xmp_update_result
{
	bool success = false;
	df::file_path xmp_path;
};

namespace metadata_xmp
{
	struct property_presence
	{
		bool tags = false;
		bool rating = false;
	};

	void initialise();

	void parse(prop::item_metadata& pd, df::cspan xmp);
	void parse(prop::item_metadata& pd, df::file_path path);
	property_presence properties(df::cspan xmp);

	// Which patch of the sphere a panorama declares it holds. Read from the file being displayed
	// rather than from the index: only the item on screen needs it, and reading it here costs no
	// stored field and no re-index. An undeclared or contradictory crop comes back invalid, which
	// the caller resolves against the pixels it actually has.
	prop::panorama_geometry panorama(df::file_path path);

	// dst_xmp_path is the exact sidecar file to write. The caller stages it next to the media
	// file actually being written so a failure can never touch the live sidecar.
	xmp_update_result update(df::file_path file_path, df::file_path src_path, const metadata_edits& edits,
	                         std::string_view src_xmp_name, df::file_path dst_xmp_path);
	// Stages a metadata-only edit at stage_path, a new file, with the edit already in it, when the file
	// keeps its packet in a format libfiles writes: a copy patched where the edit fits the space the file
	// holds, otherwise one pass over the source. Nothing, with nothing written, for any other file; the
	// caller copies that and calls update().
	std::optional<xmp_update_result> stage_update(df::file_path stage_path, df::file_path src_path,
	                                              const metadata_edits& edits, std::string_view src_xmp_name);
	// Patches a metadata-only edit into the file where it lies, when it fits the space the file holds:
	// nothing the file holds moves, and libfiles orders the writes so that an interruption leaves a
	// file that reads whole. False, with nothing written, when it does not fit; the caller stages it.
	bool patch(df::file_path path, const metadata_edits& edits, std::string_view src_xmp_name);
	void update(std::string& buffer, const metadata_edits& edits);
	metadata_kv_list to_info(df::cspan xmp);
	// `parsed` is false only when the toolkit could not read the packet; a valid empty packet is parsed.
	metadata_kv_list to_info(df::cspan xmp, bool& parsed);

	void set_test_raw_source_xmp(df::file_path path, df::cspan xmp);
	void set_test_update_failure_sequence(std::vector<int> actions);
};
