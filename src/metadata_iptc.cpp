// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: IPTC metadata. libfiles reads the datasets; this maps captions, keywords, bylines,
// copyright, places and dates onto an item's metadata and lists the datasets for display.

#include "pch.h"
#include "metadata_iptc.h"
#include "model_tags.h"
#include "model_property.h"

#include <libfiles/iptc.h>

// IPTC is parsed after Exif, so an empty dataset must not erase a value the Exif block supplied.
static void assign_if_set(str::cached& dst, const str::cached val)
{
	if (!str::is_empty(val)) dst = val;
}

// IPTC dates are `CCYYMMDD` and times are `HHMMSS±HHMM`, both fixed width and neither carrying the
// separators parse_exif_date expects, so the pair is punctuated into one reading here. A date with
// no time is midnight, which is what the standard means by a date alone; a time with no date says
// nothing at all and is dropped.
static void add_iptc_date(prop::item_metadata& pd, const std::string_view date, const std::string_view time,
                          const prop::date_source source)
{
	if (date.size() != 8 || !str::is_num(date)) return;

	auto text = std::format("{}:{}:{}", date.substr(0, 4), date.substr(4, 2), date.substr(6, 2));
	auto offset = prop::date_pack::no_offset;

	if (time.size() >= 6 && str::is_num(time.substr(0, 6)))
	{
		std::format_to(std::back_inserter(text), " {}:{}:{}",
		               time.substr(0, 2), time.substr(2, 2), time.substr(4, 2));

		// `+0200` rather than the `+02:00` the shared parser takes.
		if (time.size() == 11 && (time[6] == '+' || time[6] == '-') && str::is_num(time.substr(7, 4)))
		{
			offset = prop::parse_utc_offset(
				std::format("{}{}:{}", time[6], time.substr(7, 2), time.substr(9, 2)));
		}
	}
	else
	{
		text += " 00:00:00";
	}

	df::date_t d;
	if (d.parse_exif_date(text) && d.is_valid()) pd.dates.add(source, d, offset);
}

void metadata_iptc::parse(prop::item_metadata& pd, const df::cspan cs)
{
	const auto fields = libfiles::read_iptc({cs.data, cs.size});

	tag_set tags;
	tag_set artists;

	for (const auto& keyword : fields.keywords) tags.add_one(str::cache(keyword));
	for (const auto& byline : fields.bylines) artists.add_one(str::cache(byline));

	assign_if_set(pd.description, str::cache(fields.caption));
	assign_if_set(pd.title, str::cache(fields.object_name));
	assign_if_set(pd.location_place, str::cache(fields.city));
	assign_if_set(pd.location_state, str::cache(fields.state));
	assign_if_set(pd.location_country, str::cache(fields.country));
	assign_if_set(pd.copyright_credit, str::cache(fields.credit));
	assign_if_set(pd.copyright_source, str::cache(fields.source));
	assign_if_set(pd.copyright_notice, str::cache(fields.copyright));

	add_iptc_date(pd, fields.date_created, fields.time_created, prop::date_source::iptc_created);
	add_iptc_date(pd, fields.digital_creation_date, fields.digital_creation_time, prop::date_source::iptc_digital_created);

	if (!tags.is_empty())
	{
		tags.add(tag_set(pd.tags));
		tags.make_unique();
		pd.tags = str::cache(tags.to_string());
	}

	if (!artists.is_empty())
	{
		artists.add(tag_set(pd.artist));
		artists.make_unique();
		pd.artist = str::cache(artists.to_string());
	}
}

metadata_kv_list metadata_iptc::to_info(const df::cspan cs)
{
	metadata_kv_list result;

	for (auto& entry : libfiles::list_iptc({cs.data, cs.size}))
	{
		result.emplace_back(std::move(entry.title), std::move(entry.value));
	}

	return result;
}
