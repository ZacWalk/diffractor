// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Counting the words in names and text for the index's summaries of a collection, and decoding the
// character references XML text carries; util_text.cpp says which words count.

#pragma once

namespace str
{
	void count_ranges(core::dense_string_counts& counts, std::string_view text);

	// XML text with its references decoded: the five named ones and numeric ones. Anything else is left as
	// written, and a reference naming no character is dropped.
	std::string decode_xml_entities(std::string_view text);
};
