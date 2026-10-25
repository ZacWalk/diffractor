// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: IPTC metadata parsing. Extracts news and editorial metadata including
// captions, keywords, and copyright information from image files.

#pragma once

struct metadata_iptc
{
	static void parse(prop::item_metadata& pd, df::cspan cs);
	static metadata_kv_list to_info(df::cspan cs);
};
