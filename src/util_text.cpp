// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Counts the words in names and text for the index's summaries of a collection, leaving out
// numbers, cameras' numbered file names and filler words.

#include "pch.h"
#include "util_text.h"

static const df::hash_set<std::string_view, df::ihash, df::ieq> unwanted_english_word = {
	"and",
};

// Applied to the bytes of a UTF-8 string, so the classification must be the narrow one: passing a
// negative char to the wide predicates is a WEOF collision off Windows.
static bool is_range_separator(const char c)
{
	const auto uc = static_cast<unsigned char>(c);
	return std::ispunct(uc) || std::isspace(uc);
}

static bool is_num(const std::string_view text)
{
	return !text.empty() && std::find_if(text.begin(), text.end(), [](const char c)
	{
		return !std::isdigit(static_cast<unsigned char>(c));
	}) == text.end();
}

static bool is_num_with_prefix(const std::string_view text, const std::string_view prefix)
{
	return text.rfind(prefix, 0) == 0 && is_num(text.substr(prefix.size()));
}

static bool is_wanted_word(const std::string_view text)
{
	if (text.size() <= 2) return false;
	if (is_num(text)) return false;
	if (is_num_with_prefix(text, "DSC")) return false;
	if (is_num_with_prefix(text, "DSCF")) return false;
	if (is_num_with_prefix(text, "Image")) return false;
	if (is_num_with_prefix(text, "IMG_")) return false;
	if (is_num_with_prefix(text, "IMGP")) return false;
	if (is_num_with_prefix(text, "MVI_")) return false;
	if (is_num_with_prefix(text, "PIC_")) return false;
	if (is_num_with_prefix(text, "SN")) return false;
	return !unwanted_english_word.contains(text);
}

void str::count_ranges(df::dense_string_counts& totals, const std::string_view text)
{
	if (!text.empty())
	{
		auto i = 0u;
		auto split_start = std::string_view::npos;
		auto split_len = 0;

		while (i < text.size())
		{
			const auto c = text[i];

			if (!is_range_separator(c))
			{
				if (split_start == std::string_view::npos)
				{
					split_start = i;
				}

				split_len += 1;
			}
			else
			{
				if (split_start != std::string_view::npos && split_len > 0)
				{
					const auto part = trim(text.substr(split_start, split_len));

					if (!part.empty() && is_wanted_word(part))
					{
						++totals[part];
					}
				}

				split_start = std::string_view::npos;
				split_len = 0;
			}

			++i;
		}

		if (split_start != std::string_view::npos && split_len > 0)
		{
			const auto last = trim(text.substr(split_start, split_len));

			if (!last.empty() && is_wanted_word(last))
			{
				++totals[last];
			}
		}
	}
}
