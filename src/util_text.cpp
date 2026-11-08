// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Counts the words in names and text for the index's summaries of a collection, leaving out
// numbers, cameras' numbered file names and filler words; and decodes the character references in XML text,
// which Movie Maker projects and media managers' .nfo files are read through.

#include "pch.h"
#include "util_text.h"

std::string str::decode_xml_entities(const std::string_view text)
{
	if (text.find('&') == std::string_view::npos) return std::string(text);

	std::string result;
	result.reserve(text.size());

	for (size_t i = 0; i < text.size();)
	{
		if (text[i] != '&')
		{
			result += text[i++];
			continue;
		}

		// An entity is a handful of characters, so its ';' is looked for only that far. Looking to the
		// end of the text from every '&' made a run of bare ampersands cost the square of its length.
		const auto semicolon = text.substr(i, 11).find(';');

		if (semicolon == std::string_view::npos)
		{
			result += text[i++];
			continue;
		}

		const auto end = i + semicolon;
		const auto entity = text.substr(i + 1, end - i - 1);

		if (entity == "amp") result += '&';
		else if (entity == "lt") result += '<';
		else if (entity == "gt") result += '>';
		else if (entity == "quot") result += '"';
		else if (entity == "apos") result += '\'';
		else if (entity.size() > 1 && entity[0] == '#')
		{
			const auto hex = entity[1] == 'x' || entity[1] == 'X';
			const auto digits = entity.substr(hex ? 2 : 1);
			uint32_t code = 0;

			const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), code,
			                                       hex ? 16 : 10);

			// A surrogate is half of a UTF-16 pair and no character of its own; encoded alone it would make
			// text - a path in a Movie project, a title in an .nfo - that nothing can convert.
			if (ec == std::errc{} && ptr == digits.data() + digits.size() && code > 0 && code < 0x110000 &&
				(code < 0xD800 || code > 0xDFFF))
			{
				core::char32_to_utf8(std::back_inserter(result), code);
			}
		}
		else
		{
			result.append(text.substr(i, end - i + 1));
		}

		i = end + 1;
	}

	return result;
}

static const core::hash_set<std::string_view, core::ihash, core::ieq> unwanted_english_word = {
	"and",
};

// Applied to the bytes of a UTF-8 string, so the classification must be the narrow one: passing a
// negative char to the wide predicates is a WEOF collision off Windows.
static bool is_range_separator(const char c)
{
	const auto uc = static_cast<unsigned char>(c);
	return std::ispunct(uc) || std::isspace(uc);
}

static bool is_num_token(const std::string_view text)
{
	return !text.empty() && std::find_if(text.begin(), text.end(), [](const char c)
	{
		return !std::isdigit(static_cast<unsigned char>(c));
	}) == text.end();
}

static bool is_num_token_with_prefix(const std::string_view text, const std::string_view prefix)
{
	return text.rfind(prefix, 0) == 0 && is_num_token(text.substr(prefix.size()));
}

static bool is_wanted_word(const std::string_view text)
{
	if (text.size() <= 2) return false;
	if (is_num_token(text)) return false;
	if (is_num_token_with_prefix(text, "DSC")) return false;
	if (is_num_token_with_prefix(text, "DSCF")) return false;
	if (is_num_token_with_prefix(text, "Image")) return false;
	if (is_num_token_with_prefix(text, "IMG_")) return false;
	if (is_num_token_with_prefix(text, "IMGP")) return false;
	if (is_num_token_with_prefix(text, "MVI_")) return false;
	if (is_num_token_with_prefix(text, "PIC_")) return false;
	if (is_num_token_with_prefix(text, "SN")) return false;
	return !unwanted_english_word.contains(text);
}

void str::count_ranges(core::dense_string_counts& totals, const std::string_view text)
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
