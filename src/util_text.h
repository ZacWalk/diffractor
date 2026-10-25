// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Counting the words in names and text for the index's summaries of a collection;
// util_text.cpp says which words count.

#pragma once

namespace str
{
	void count_ranges(df::dense_string_counts& counts, std::string_view text);
};
