// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Search query parsing and matching. Parses search expressions into terms,
// handles property filters, date ranges, location queries, duplicate and related-item
// matching, and the scope-aware autocompletion classifier. The text predicates, the date keys and the way
// term answers combine are shared with the search index (model_search_index.h), so the two cannot disagree.

#pragma once
#include "model_property.h"
#include "model_location.h"
#include "model_related.h"
#include "files.h"

struct search_part;
class location_cache;

namespace df
{
	enum class copy_grade : uint32_t;

	enum class search_result_type
	{
		no_match,
		similar,
		match_prop,
		match_text,
		has_type,
		match_multiple,
		match_flag,
		match_file_group,
		match_date,
		match_folder,
		match_ext,
		match_location,
		match_volume,
		related_album,
		related_series,
		related_time,
		related_location,
		related_film,
	};

	// `similar` carries the duplicate axis because it already means "a possible copy" everywhere else.
	constexpr search_result_type related_result_type(const related_axis axis)
	{
		switch (axis)
		{
		case related_axis::film: return search_result_type::related_film;
		case related_axis::album: return search_result_type::related_album;
		case related_axis::series: return search_result_type::related_series;
		case related_axis::time: return search_result_type::related_time;
		case related_axis::location: return search_result_type::related_location;
		default: return search_result_type::similar;
		}
	}

	constexpr related_axis related_axis_of(const search_result_type type)
	{
		switch (type)
		{
		case search_result_type::related_film: return related_axis::film;
		case search_result_type::related_album: return related_axis::album;
		case search_result_type::related_series: return related_axis::series;
		case search_result_type::related_time: return related_axis::time;
		case search_result_type::related_location: return related_axis::location;
		default: return related_axis::duplicate;
		}
	}

	enum class search_term_type
	{
		empty,
		text,
		extension,
		value,
		has_type,
		media_type,
		date,
		location,
		has_location,
		// locations.md 3.5: a built-in location class, spelled with `@` and taking no argument.
		remote,
		area,
		duplicate,
		volume,
		// The file's own panorama declaration, spelled `@panorama` and taking no argument.
		panorama,
		// What a library of films and television says an item is, spelled `@movie` and `@episode`.
		film,
		episode,
		// A recording started and not finished, spelled `@inprogress`.
		in_progress,
	};

	// Specificity a location term is constrained to; `any` matches place, region or country.
	enum class location_level : uint8_t
	{
		any,
		place,
		state,
		country,
	};

	// locations.md 3.5: every way to scope a term to a location, and the level each constrains to.
	// `loc:` and `near:` take a centre - a place name or a coordinate - and an optional radius; the
	// others name a place, region or country. Completing, tokenizing, coalescing, parsing and
	// formatting a query all read this one table. The first entry for a level is the spelling a
	// formatted query writes.
	struct location_scope
	{
		std::string_view name; // without its colon
		location_level level = location_level::any;
		bool takes_centre = false;
	};

	inline constexpr location_scope location_scopes[] = {
		{"loc", location_level::any, true},
		{"near", location_level::any, true},
		{"place", location_level::place, false},
		{"city", location_level::place, false},
		{"state", location_level::state, false},
		{"country", location_level::country, false},
		{"countries", location_level::country, false},
	};

	// The location scope spelled `scope`, compared without regard to case; null when it is not one.
	inline const location_scope* find_location_scope(const std::string_view scope)
	{
		for (const auto& s : location_scopes)
		{
			if (core::icmp(scope, s.name) == 0) return &s;
		}

		return nullptr;
	}

	// The spelling a formatted query writes for a level.
	constexpr std::string_view location_scope_name(const location_level level)
	{
		for (const auto& s : location_scopes)
		{
			if (s.level == level) return s.name;
		}

		return location_scopes[0].name;
	}

	struct search_result
	{
		search_result_type type = search_result_type::no_match;
		// Only a related search sets this: how far this item is from the item the search started at,
		// in the units of its axis. It is what orders a relation group. Deliberately 32-bit and
		// declared here - it occupies padding that already existed, so a search result carries it for
		// free, and every axis distance (seconds, metres, track and episode gaps) fits comfortably.
		int32_t distance = 0;
		prop::key_ref key = prop::null;
		core::cached text = {};

		// Constructors rather than aggregate initialisation, so that placing `distance` in the
		// padding after `type` costs the existing `{type}`, `{type, key}` and `{type, key, text}`
		// call sites nothing.
		constexpr search_result() noexcept = default;

		constexpr search_result(const search_result_type t) noexcept : type(t)
		{
		}

		constexpr search_result(const search_result_type t, const prop::key_ref k) noexcept : type(t), key(k)
		{
		}

		search_result(const search_result_type t, const prop::key_ref k, const core::cached x) noexcept :
			type(t), key(k), text(x)
		{
		}

		bool is_match() const
		{
			return type != search_result_type::no_match;
		}

		bool is_match(const prop::key_ref find_key) const
		{
			if (type == search_result_type::match_location)
			{
				return find_key == prop::longitude ||
					find_key == prop::latitude ||
					find_key == prop::location_country ||
					find_key == prop::location_place ||
					find_key == prop::location_state;
			}

			return find_key == key;
		}

		bool is_match(const prop::key_ref find_key, const std::string_view find_text) const
		{
			return find_key == key && ifind(text, find_text) != std::string_view::npos;
		}
	};

	inline bool is_probably_selector(const std::string_view text)
	{
		return core::item_selector::can_iterate(text) &&
			text.find(" #") == std::string::npos &&
			text.find(" @") == std::string::npos &&
			text.find(" *") == std::string::npos &&
			text.find("** ") == std::string::npos &&
			text.find("**\\ ") == std::string::npos &&
			text.find("**/ ") == std::string::npos &&
			text.find("\\ ") == std::string::npos &&
			text.find("/ ") == std::string::npos;
	}

	// Scope-aware search auto-completion helpers (Issue #157). These classify the active
	// (last whitespace-delimited) token of a search query so the value part of a scoped
	// term can be completed from the matching vocabulary: "#"/"tag:" -> tag names,
	// "@" -> media groups, "with:"/"without:" -> property scopes, and the location
	// scopes -> gazetteer places (locations.md 3.4).
	enum class search_scope_kind
	{
		none,
		tag,
		group,
		with,
		without,
		location,
	};

	struct search_scope_completion
	{
		search_scope_kind kind = search_scope_kind::none;
		std::string lead; // text before the active token, including trailing separator
		std::string value; // fragment typed after the scope prefix (may be empty)
		bool tag_as_scope = false; // true when written as "tag:" rather than "#"

		// Specificity the typed location scope constrains its completions to.
		location_level level = location_level::any;

		// Vocabulary query to feed the word index. Tags are stored as "#tag" and media
		// groups as "@group"; with/without complete from property scopes and locations
		// complete from the gazetteer, so both return {}.
		std::string vocab_query() const
		{
			switch (kind)
			{
			case search_scope_kind::tag: return "#" + value;
			case search_scope_kind::group: return "@" + value;
			default: return {};
			}
		}

		// Turn a matched vocabulary word ("#dog" / "@video") or property scope name
		// ("exposure") into the text that will be committed to the search box. A location
		// candidate is already a complete canonical term, so it is committed verbatim.
		std::string format(const std::string_view candidate) const
		{
			switch (kind)
			{
			case search_scope_kind::tag:
				return tag_as_scope
					       ? std::format("tag:{}", core::starts(candidate, "#") ? candidate.substr(1) : candidate)
					       : std::string(candidate);
			case search_scope_kind::with:
				return std::format("with:{}", candidate);
			case search_scope_kind::without:
				return std::format("without:{}", candidate);
			case search_scope_kind::group:
			case search_scope_kind::location:
			default:
				return std::string(candidate);
			}
		}
	};

	inline search_scope_completion classify_search_scope(const std::string_view query)
	{
		search_scope_completion result;

		const auto last_space = query.find_last_of(" \t");
		result.lead = last_space == std::string_view::npos
			              ? std::string{}
			              : std::string(query.substr(0, last_space + 1));
		const auto token = last_space == std::string_view::npos ? query : query.substr(last_space + 1);

		if (token.empty()) return result;

		// locations.md 3.5: every location scope completes from the same vocabulary; only the
		// level it constrains to differs.
		for (const auto& scope : location_scopes)
		{
			const auto prefix = std::string(scope.name) + ':';

			if (core::starts(token, prefix))
			{
				result.kind = search_scope_kind::location;
				result.level = scope.level;
				result.value = std::string(token.substr(prefix.size()));
				return result;
			}
		}

		if (core::starts(token, "tag:"))
		{
			result.kind = search_scope_kind::tag;
			result.tag_as_scope = true;
			result.value = std::string(token.substr(4));
		}
		else if (token.front() == '#')
		{
			result.kind = search_scope_kind::tag;
			result.value = std::string(token.substr(1));
		}
		else if (token.front() == '@')
		{
			result.kind = search_scope_kind::group;
			result.value = std::string(token.substr(1));
		}
		else if (core::starts(token, "without:"))
		{
			result.kind = search_scope_kind::without;
			result.value = std::string(token.substr(8));
		}
		else if (core::starts(token, "with:"))
		{
			result.kind = search_scope_kind::with;
			result.value = std::string(token.substr(5));
		}

		return result;
	}

	enum class search_term_modifier_bool
	{
		m_and,
		m_or,
		none
	};

	struct search_term_modifier
	{
		bool positive = true;
		search_term_modifier_bool logical_op = search_term_modifier_bool::none;
		bool less_than = false;
		bool greater_than = false;
		bool equals = false;
		int begin_group = 0;
		int end_group = 0;

		search_term_modifier() noexcept = default;
		search_term_modifier& operator=(const search_term_modifier&) noexcept = default;
		search_term_modifier& operator=(search_term_modifier&&) noexcept = default;
		search_term_modifier(const search_term_modifier&) noexcept = default;
		search_term_modifier(search_term_modifier&&) noexcept = default;

		search_term_modifier(const bool pos) noexcept : positive(pos)
		{
		}

		search_term_modifier(const bool pos, bool /*fuz*/) noexcept : positive(pos)
		{
		}

		bool is_defaults() const
		{
			return positive &&
				logical_op == search_term_modifier_bool::none &&
				!less_than &&
				!greater_than &&
				!equals &&
				begin_group == 0 &&
				end_group == 0;
		}

		friend auto operator<=>(const search_term_modifier&, const search_term_modifier&) = default;

		void clear()
		{
			positive = true;
			logical_op = search_term_modifier_bool::none;
			less_than = false;
			greater_than = false;
			equals = false;
			begin_group = 0;
			end_group = 0;
		}
	};

	struct search_parent;

	// The three keys a date term can be compared against, one per group order. `original` is the
	// capture-first ladder the tile shows and Group by Date Original keys on; `created` is the
	// Created concept alone. Appended rather than placed beside `created` so the numbering a term
	// carries stays what it was.
	enum class date_parts_prop
	{
		any,
		created,
		modified,
		original
	};

	struct date_parts
	{
		int age = 0;
		int year = 0;
		int month = 0;
		int day = 0;

		date_parts_prop target = date_parts_prop::any;

		date_parts() noexcept = default;
		date_parts(const date_parts&) noexcept = default;
		date_parts& operator=(const date_parts&) noexcept = default;
		date_parts(date_parts&&) noexcept = default;
		date_parts& operator=(date_parts&&) noexcept = default;

		date_parts(const core::day_t& dd, const date_parts_prop isc = date_parts_prop::any) noexcept : year(dd.year),
			month(dd.month), day(dd.day), target(isc)
		{
		}

		friend auto operator<=>(const date_parts&, const date_parts&) = default;

		bool is_empty() const
		{
			return age == 0 &&
				year == 0 &&
				month == 0 &&
				day == 0;
		}
	};

	// NFC form of a term's text, computed once at construction so matching never re-normalises the
	// query per field per item. Empty when the text is ASCII, which is already NFC.
	inline std::string nfc_for_match(const std::string_view text)
	{
		return core::is_ascii(text) ? std::string{} : platform::normalize_nfc(text);
	}

	struct search_term
	{
		search_term_type type = search_term_type::empty;

		search_term_modifier modifiers;

		prop::key_ref key = prop::null;
		std::string text;
		std::string _nfc_text;
		bool _is_wildcard = false;
		int int_val = 0;
		uint64_t int64_val = 0;
		double float_val = 0.0;
		gps_coordinate coord_val;
		pointi location_cell = {};
		int location_cell_span = 0;
		location_level level = location_level::any;
		core::xy16 xy_val = {0, 0};
		file_group_ref fg_val = nullptr;
		date_parts date_val;

		search_term() noexcept = default;
		search_term(const search_term&) = default;
		search_term& operator=(const search_term&) = default;
		search_term(search_term&&) noexcept = default;
		search_term& operator=(search_term&&) noexcept = default;

		explicit search_term(const search_term_type tt,
		                     const search_term_modifier& mods) noexcept : type(tt), modifiers(mods)
		{
		}

		explicit search_term(const search_term_type tt, const std::string_view v,
		                     const search_term_modifier& mods) noexcept :
			type(tt),
			modifiers(mods),
			text(v),
			_nfc_text(nfc_for_match(text)),
			_is_wildcard(core::is_wildcard(text))
		{
		}

		explicit search_term(const search_term_type tt, const date_parts& v,
		                     const search_term_modifier& mods) noexcept : type(tt), modifiers(mods), date_val(v)
		{
		}

		explicit search_term(const search_term_type tt, const gps_coordinate coord, const double v,
		                     const search_term_modifier& mods) noexcept : type(tt), modifiers(mods), float_val(v),
		                                                                  coord_val(coord)
		{
		}

		explicit search_term(const map_location_area& area, const search_term_modifier& mods) noexcept :
			type(search_term_type::area), modifiers(mods), text(area.name), location_cell(area.cell),
			location_cell_span(area.cell_span)
		{
		}

		explicit search_term(const prop::key_ref k, const std::string_view v,
		                     const search_term_modifier& mods) noexcept :
			type(search_term_type::value),
			modifiers(mods),
			key(k),
			text(v),
			_nfc_text(nfc_for_match(text)),
			_is_wildcard(core::is_wildcard(text))
		{
		}

		explicit search_term(const prop::key_ref k, const int v, const search_term_modifier& mods) noexcept :
			type(search_term_type::value), modifiers(mods), key(k), int_val(v)
		{
		}

		explicit search_term(const prop::key_ref k, const uint32_t v, const search_term_modifier& mods) noexcept :
			type(search_term_type::value), modifiers(mods), key(k), int_val(v)
		{
		}

		explicit search_term(const prop::key_ref k, const uint64_t v, const search_term_modifier& mods) noexcept :
			type(search_term_type::value), modifiers(mods), key(k), int64_val(v)
		{
		}

		explicit search_term(const prop::key_ref k, const double v, const search_term_modifier& mods) noexcept :
			type(search_term_type::value), modifiers(mods), key(k), float_val(v)
		{
		}

		explicit search_term(const prop::key_ref k, const core::xy16 v, const search_term_modifier& mods) noexcept :
			type(search_term_type::value), modifiers(mods), key(k), xy_val(v)
		{
		}

		explicit search_term(const std::string_view v, const search_term_modifier& mods) noexcept :
			type(search_term_type::text),
			modifiers(mods),
			text(v),
			_nfc_text(nfc_for_match(text)),
			_is_wildcard(core::is_wildcard(text))
		{
		}

		explicit search_term(const prop::key_ref t, const search_term_modifier& mods) noexcept :
			type(search_term_type::has_type), modifiers(mods), key(t)
		{
		}

		explicit search_term(const file_group_ref ft, const search_term_modifier& mods) noexcept :
			type(search_term_type::media_type), modifiers(mods), fg_val(ft)
		{
		}

		bool is_empty() const
		{
			return type == search_term_type::empty;
		}

		bool no_modifiers() const
		{
			return modifiers.is_defaults();
		}

		bool is_date() const
		{
			return type == search_term_type::date ||
				(type == search_term_type::value && key->data_type == prop::data_type::date);
		}

		bool is_int() const
		{
			return type == search_term_type::value && key->data_type == prop::data_type::int32;
		}

		bool is_media_type() const
		{
			return type == search_term_type::media_type;
		}

		bool needs_metadata() const
		{
			return type == search_term_type::text ||
				type == search_term_type::value ||
				type == search_term_type::has_type ||
				type == search_term_type::date ||
				type == search_term_type::location ||
				type == search_term_type::has_location ||
				type == search_term_type::remote ||
				type == search_term_type::area ||
				type == search_term_type::duplicate ||
				type == search_term_type::panorama ||
				type == search_term_type::film ||
				type == search_term_type::episode ||
				type == search_term_type::in_progress;
		}

		friend bool operator==(const search_term& lhs, const search_term& rhs)
		{
			return lhs.type == rhs.type
				&& lhs.modifiers == rhs.modifiers
				&& lhs.key == rhs.key
				&& lhs.text == rhs.text
				&& lhs.int_val == rhs.int_val
				&& lhs.int64_val == rhs.int64_val
				&& equiv(lhs.float_val, rhs.float_val)
				&& lhs.coord_val == rhs.coord_val
				&& lhs.location_cell == rhs.location_cell
				&& lhs.location_cell_span == rhs.location_cell_span
				&& lhs.level == rhs.level
				&& lhs.xy_val == rhs.xy_val
				&& lhs.fg_val == rhs.fg_val
				&& lhs.date_val == rhs.date_val;
		}

		friend bool operator!=(const search_term& lhs, const search_term& rhs)
		{
			return !(lhs == rhs);
		}

		friend bool operator<(const search_term& lhs, const search_term& rhs)
		{
			if (lhs.type < rhs.type)
				return true;
			if (rhs.type < lhs.type)
				return false;
			if (lhs.modifiers < rhs.modifiers)
				return true;
			if (rhs.modifiers < lhs.modifiers)
				return false;
			if (lhs.key < rhs.key)
				return true;
			if (rhs.key < lhs.key)
				return false;
			if (lhs.text < rhs.text)
				return true;
			if (rhs.text < lhs.text)
				return false;
			if (lhs._is_wildcard < rhs._is_wildcard)
				return true;
			if (rhs._is_wildcard < lhs._is_wildcard)
				return false;
			if (lhs.int_val < rhs.int_val)
				return true;
			if (rhs.int_val < lhs.int_val)
				return false;
			if (lhs.int64_val < rhs.int64_val)
				return true;
			if (rhs.int64_val < lhs.int64_val)
				return false;
			if (lhs.float_val < rhs.float_val)
				return true;
			if (rhs.float_val < lhs.float_val)
				return false;
			if (lhs.coord_val < rhs.coord_val)
				return true;
			if (rhs.coord_val < lhs.coord_val)
				return false;
			if (lhs.level < rhs.level)
				return true;
			if (rhs.level < lhs.level)
				return false;
			if (lhs.xy_val < rhs.xy_val)
				return true;
			if (rhs.xy_val < lhs.xy_val)
				return false;
			if (lhs.fg_val < rhs.fg_val)
				return true;
			if (rhs.fg_val < lhs.fg_val)
				return false;
			return lhs.date_val < rhs.date_val;
		}

		friend bool operator<=(const search_term& lhs, const search_term& rhs)
		{
			return !(rhs < lhs);
		}

		friend bool operator>(const search_term& lhs, const search_term& rhs)
		{
			return rhs < lhs;
		}

		friend bool operator>=(const search_term& lhs, const search_term& rhs)
		{
			return !(lhs < rhs);
		}
	};

	std::string format_term(const search_term& term);

	// Issue #139: wraps a path that contains spaces so it reads as one search term. Shared by the
	// address box and by folder completion, which would otherwise disagree about the same path.
	std::string quote_path_term(std::string_view path);

	// Returns true when folder_name is on a drive whose volume label matches the
	// term text (case-insensitive, wildcards supported). drive_labels maps an
	// upper-case drive letter to its current volume label.
	bool match_volume_label(std::string_view folder_name, const core::hash_map<char, core::cached>& drive_labels,
	                        const search_term& term);

	// locations.md 3.1: merges the tokens that follow a location scope into its term when they
	// are recognisably part of the place - a country code, a distance, or comma-bound text.
	std::vector<search_part> coalesce_parts(std::vector<search_part> parts);


	struct related_info
	{
		core::file_path path = {};
		gps_coordinate gps = {};
		core::date_t metadata_created = {};
		core::date_t file_created = {};
		core::cached name = {};
		core::cached album = {};
		core::cached album_artist = {};
		core::cached show = {};
		core::cached title = {};
		uint16_t year = 0;
		prop::media_kind kind = prop::media_kind::none;
		uint32_t crc32c = 0;
		core::file_size size = {};
		file_type_ref ft = nullptr;
		uint32_t group = 0;
		uint32_t exact_group = 0;
		copy_grade duplicate_grade = static_cast<copy_grade>(0);
		bool duplicate_crowded = false;
		crypto::phash_rotations phash{};
		sizei dimensions;
		uint16_t season = 0;
		core::xy8 episode = {0, 0};
		core::xy8 disk = {0, 0};
		core::xy8 track = {0, 0};

		bool is_loaded = false;

		core::date_t created() const
		{
			if (metadata_created.is_valid()) return metadata_created;

			// file_created is the index's instant. It is compared against
			// index_file_item::created(), which converts the same stamp.
			return file_created.is_valid() ? platform::utc_to_local(file_created) : core::date_t{};
		}

		// Position within an album or a series, used to answer with the neighbouring tracks or
		// episodes rather than with whichever ones happen to be first.
		int64_t track_ordinal() const
		{
			return static_cast<int64_t>(disk.x) * 1000 + track.x;
		}

		int64_t episode_ordinal() const
		{
			return static_cast<int64_t>(season) * 100000 + episode.x;
		}

		related_info() noexcept = default;
		related_info(const related_info&) noexcept = default;
		related_info& operator=(const related_info&) noexcept = default;
		related_info(related_info&&) noexcept = default;
		related_info& operator=(related_info&&) noexcept = default;

		friend bool operator==(const related_info& lhs, const related_info& rhs)
		{
			return lhs.path == rhs.path;
		}

		friend bool operator!=(const related_info& lhs, const related_info& rhs)
		{
			return !(lhs == rhs);
		}

		void load(const item_element_ptr& i);
	};

	class search_t
	{
		std::vector<core::item_selector> _selectors;
		std::vector<search_term> _terms;
		related_info _related;
		std::string _raw;

	public:
		search_t() noexcept = default;
		~search_t() noexcept = default;

		search_t(const search_t&) = default;
		search_t& operator=(const search_t&) = default;
		search_t(search_t&&) noexcept = default;
		search_t& operator=(search_t&&) noexcept = default;

		bool is_empty() const
		{
			return _selectors.empty() &&
				_related.path.is_empty() &&
				_terms.empty();
		}

		bool has_volume_term() const
		{
			return std::ranges::find_if(_terms, [](auto&& v)
			{
				return v.type == search_term_type::volume;
			}) != _terms.end();
		}

		// A named place has to be resolved to coordinates before matching, whether the query gave a
		// radius or leaves the place's own attribution reach to stand for it.
		bool has_named_location() const
		{
			return std::ranges::find_if(_terms, [](auto&& v)
			{
				return v.type == search_term_type::location && !v.coord_val.is_valid() && !v.text.empty();
			}) != _terms.end();
		}

		// A named place with a radius has to be resolved to coordinates before matching.

		const std::vector<search_term>& terms() const
		{
			return _terms;
		}

		// locations.md 4.1: the distance slider controls a search only when exactly one location
		// term names a place. A coordinate term or a second place has no single radius to show.
		const search_term* single_place_term() const
		{
			const search_term* found = nullptr;

			for (const auto& t : _terms)
			{
				if (t.type != search_term_type::location) continue;
				if (found || t.coord_val.is_valid() || t.text.empty()) return nullptr;
				found = &t;
			}

			return found;
		}

		search_t& set_place_distance(const double km)
		{
			for (auto& t : _terms)
			{
				if (t.type == search_term_type::location && !t.coord_val.is_valid() && !t.text.empty())
				{
					t.float_val = km;
				}
			}

			_raw.clear();
			return *this;
		}

		// locations.md 3.7: dropping a qualifier -- `London, Canada` becomes `London` -- so a query
		// that resolved to the wrong namesake is one click from asking about all of them.
		search_t& set_place_name(const std::string_view name)
		{
			for (auto& t : _terms)
			{
				if (t.type == search_term_type::location && !t.coord_val.is_valid() && !t.text.empty())
				{
					t.text = name;
				}
			}

			_raw.clear();
			return *this;
		}

		bool is_showing_folder() const
		{
			return _selectors.size() == 1 &&
				!_selectors[0].is_recursive();
		}

		bool has_selector() const
		{
			return !_selectors.empty();
		}

		// True when this is a plain folder browse: one or more folder selectors with
		// no search terms. Used to distinguish browsing an empty folder ("Empty Folder")
		// from a search that matched nothing ("Nothing found").
		bool is_folder() const
		{
			return has_selector() && !has_terms();
		}

		bool has_recursive_selector() const
		{
			for (const auto& s : _selectors)
			{
				if (s.is_recursive())
				{
					return true;
				}
			}

			return false;
		}

		void remove_recursive()
		{
			for (auto&& s : _selectors)
			{
				if (s.is_recursive())
				{
					s = core::item_selector(s.folder(), false, s.wildcard());
				}
			}

			_raw.clear();
		}

		void set_recursive(const bool recursive)
		{
			for (auto&& s : _selectors)
			{
				if (s.is_recursive() != recursive)
				{
					s = core::item_selector(s.folder(), recursive, s.wildcard());
				}
			}

			_raw.clear();
		}

		bool has_terms() const
		{
			return !_terms.empty();
		}

		bool can_match_folder() const
		{
			for (const auto& t : _terms)
			{
				if (t.type == search_term_type::text && core::contains(t.text, "**"))
				{
					return true;
				}
			}

			return false;
		}

		bool needs_metadata() const;

		bool has_related() const
		{
			return !_related.path.is_empty();
		}

		search_t& with(const prop::key_ref k, std::string_view v)
		{
			_terms.emplace_back(k, v, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& with(const search_term& term)
		{
			_terms.emplace_back(term);
			_raw.clear();
			return *this;
		}

		search_t& with(const prop::key_ref k, const int v)
		{
			_terms.emplace_back(k, v, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& with(const prop::key_ref k, const double v)
		{
			_terms.emplace_back(k, v, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& with(const prop::key_ref k, core::xy16 v)
		{
			_terms.emplace_back(k, v, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& with(const prop::key_ref k, uint64_t v)
		{
			_terms.emplace_back(k, v, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& without(const prop::key_ref k, const std::string_view v)
		{
			_terms.emplace_back(k, v, search_term_modifier(false));
			_raw.clear();
			return *this;
		}

		search_t& without_extension()
		{
			_terms.emplace_back(search_term_type::extension, search_term_modifier(false));
			_raw.clear();
			return *this;
		}

		search_t& with_extension(std::string_view v)
		{
			_terms.emplace_back(search_term_type::extension, v, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		bool has_date() const
		{
			return std::ranges::find_if(_terms, [](auto&& v) { return v.is_date(); }) != _terms.end();
		}

		int year() const
		{
			for (const auto& t : _terms)
			{
				if (t.type == search_term_type::date && t.date_val.year != 0)
				{
					return t.date_val.year;
				}
			}

			return 0;
		}

		date_parts find_date_parts() const;
		void next_date(bool forward);

		search_t& with(const std::string_view a)
		{
			_terms.emplace_back(a, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& with(const std::vector<std::string>& a)
		{
			for (const auto& s : a) _terms.emplace_back(s, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& with(const prop::key_ref t)
		{
			_terms.emplace_back(t, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& with(const std::vector<prop::key_ref>& types)
		{
			for (const auto& t : types) _terms.emplace_back(t, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& with(const search_term_type flag)
		{
			_terms.emplace_back(flag, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& without(const search_term_type flag)
		{
			_terms.emplace_back(flag, search_term_modifier(false));
			_raw.clear();
			return *this;
		}

		search_t& without(const prop::key_ref t)
		{
			_terms.emplace_back(t, search_term_modifier(false));
			_raw.clear();
			return *this;
		}

		search_t& without(const std::vector<prop::key_ref>& types)
		{
			for (const auto& t : types) _terms.emplace_back(t, search_term_modifier(false));
			_raw.clear();
			return *this;
		}

		search_t& fuzzy(const prop::key_ref k, const std::string_view v)
		{
			_terms.emplace_back(k, v, search_term_modifier(true, true));
			_raw.clear();
			return *this;
		}

		search_t& fuzzy(const prop::key_ref k, const int v)
		{
			_terms.emplace_back(k, v, search_term_modifier(true, true));
			_raw.clear();
			return *this;
		}

		search_t& fuzzy(const prop::key_ref k, const double v)
		{
			_terms.emplace_back(k, v, search_term_modifier(true, true));
			_raw.clear();
			return *this;
		}

		search_t& remove(const prop::key_ref t)
		{
			std::erase_if(_terms, [t](auto&& v) { return v.key == t; });
			_raw.clear();
			return *this;
		}

		search_t& age(const int a, const date_parts_prop target = date_parts_prop::any)
		{
			date_parts d;
			d.target = target;
			d.age = a;
			_terms.emplace_back(search_term_type::date, d, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& year(const int y, const date_parts_prop target = date_parts_prop::any)
		{
			date_parts d;
			d.target = target;
			d.year = y;
			_terms.emplace_back(search_term_type::date, d, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& month(const int m, const date_parts_prop target = date_parts_prop::any)
		{
			date_parts d;
			d.target = target;
			d.month = m;
			_terms.emplace_back(search_term_type::date, d, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& day(const int dd, const int m, const int y, const date_parts_prop target = date_parts_prop::any)
		{
			date_parts d;
			d.target = target;
			d.year = y;
			d.month = m;
			d.day = dd;
			_terms.emplace_back(search_term_type::date, d, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& location(const gps_coordinate coord, const double km)
		{
			_terms.emplace_back(search_term_type::location, coord, km, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& location(const std::string_view name)
		{
			_terms.emplace_back(search_term_type::location, name, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& location(const std::string_view name, const location_level level)
		{
			_terms.emplace_back(search_term_type::location, name, search_term_modifier(true));
			_terms.back().level = level;
			_raw.clear();
			return *this;
		}

		// locations.md 6.3: a timeline node refines the query to its own bounds. Two bracketing
		// comparisons express the range, so the query the node runs returns exactly the items the
		// node counted rather than a coarser month or year around them.
		search_t& date_range(const core::day_t& from, const core::day_t& to,
		                     const date_parts_prop target = date_parts_prop::original)
		{
			search_term_modifier not_before;
			not_before.greater_than = true;
			not_before.equals = true;
			_terms.emplace_back(search_term_type::date, date_parts(from, target), not_before);

			search_term_modifier not_after;
			not_after.less_than = true;
			not_after.equals = true;
			_terms.emplace_back(search_term_type::date, date_parts(to, target), not_after);

			_raw.clear();
			return *this;
		}

		search_t& area(const map_location_area& area)
		{
			_terms.emplace_back(area, search_term_modifier(true));
			_raw.clear();
			return *this;
		}

		search_t& resolve_area(const map_location_area& area)
		{
			for (auto& term : _terms)
			{
				if (term.type == search_term_type::area && term.location_cell_span == 0 &&
					core::icmp(term.text, area.name) == 0)
				{
					term.location_cell = area.cell;
					term.location_cell_span = area.cell_span;
				}
			}
			return *this;
		}

		search_t& clear_selectors()
		{
			_selectors.clear();
			_raw.clear();
			return *this;
		}

		search_t& clear_terms()
		{
			_terms.clear();
			_raw.clear();
			return *this;
		}

		// Drops the most recently added narrowing, so broadening undoes one term at a time.
		search_t& remove_last_term()
		{
			if (!_terms.empty()) _terms.pop_back();
			_raw.clear();
			return *this;
		}

		// The least specific location level the query names, or `any` when it names none. Location
		// terms are written most specific first, so the broadest one is not the last one.
		location_level broadest_location_level() const
		{
			auto result = location_level::any;

			for (const auto& t : _terms)
			{
				if (t.type == search_term_type::location && t.level != location_level::any && t.level > result)
				{
					result = t.level;
				}
			}

			return result;
		}

		// Drops every location term more specific than `level`.
		search_t& remove_location_terms_below(const location_level level)
		{
			const auto removed = std::erase_if(_terms, [level](const search_term& t)
			{
				return t.type == search_term_type::location && t.level != location_level::any && t.level < level;
			});

			if (removed > 0) _raw.clear();
			return *this;
		}

		search_t& add_selector(const core::item_selector& f)
		{
			_selectors.emplace_back(f);
			_raw.clear();
			return *this;
		}

		search_t& add_selector(const std::string_view sv)
		{
			_selectors.emplace_back(core::item_selector(sv));
			_raw.clear();
			return *this;
		}

		search_t& related(const related_info& r)
		{
			_related = r;
			_raw.clear();
			return *this;
		}

		bool has_term_type(const search_term_type& tt) const
		{
			return std::ranges::find_if(_terms, [tt](const search_term& t) { return t.type == tt; }) != _terms.end();
		}

		void clear_term_type(const search_term_type& tt)
		{
			std::erase_if(_terms, [tt](auto&& t) { return t.type == tt; });
			_raw.clear();
		}

		void clear_media_type()
		{
			clear_term_type(search_term_type::media_type);
			_raw.clear();
		}

		search_t& add_media_type(const file_group_ref ft)
		{
			const auto found = std::ranges::find_if(_terms, [ft](auto&& t) { return t.is_media_type(); });

			if (found == _terms.end())
			{
				_terms.emplace_back(ft, search_term_modifier(true));
			}
			else
			{
				found->fg_val = ft;
				found->modifiers = search_term_modifier(true);
			}

			_raw.clear();
			return *this;
		}

		bool has_media_type() const { return has_term_type(search_term_type::media_type); }

		bool is_duplicates() const { return has_term_type(search_term_type::duplicate); }

		void clear_date_properties();

		const std::vector<core::item_selector>& selectors() const
		{
			return _selectors;
		}

		const related_info& related() const
		{
			return _related;
		}

		bool contains(const prop::key_ref k, const std::string_view v) const
		{
			return std::find_if(_terms.begin(), _terms.end(), [k, v](auto&& t) { return t.key == k && t.text == v; }) !=
				_terms.end();
		}

		bool is_match(const prop::key& key, core::date_t date) const;
		bool is_match(const prop::key& key, int val) const;

		search_presence_mask calc_required_presence() const;

		void parse_part(const search_part& part);

		search_t parse_from_input(std::string_view text) const;
		static search_t parse_path(std::string_view text);
		static search_t parse(std::string_view text);
		// Reads text as parse does without asking the file system whether a path exists: a path is taken
		// as written. For a worker that must never wait on a share, such as one counting predictions.
		static search_t parse_without_probing(std::string_view text);

		// Whether text can be read as this search without asking the file system. Parsing asks it
		// whether "C:\Pics 2019" is one folder or a folder and a year, and whether a path names a file;
		// this accepts any of those readings, so a search can be compared with saved text without
		// waiting on a share that is away. Compared with the place in view - which is there - it gives
		// the answer parsing would.
		bool can_be_read_from(std::string_view text) const;

		void raw_text(const std::string_view raw) { _raw = raw; };
		const std::string& raw_text() const { return _raw; };

		std::string text() const;
		std::string format_terms() const;

		bool has_type() const
		{
			return first_type() != prop::null;
		}

		prop::key_ref first_type() const
		{
			for (const auto& t : _terms)
			{
				if (t.type == search_term_type::value || t.type == search_term_type::has_type)
				{
					return t.key;
				}
			}
			return prop::null;
		}

		friend bool operator==(const search_t& lhs, const search_t& rhs)
		{
			return lhs._selectors == rhs._selectors
				&& lhs._terms == rhs._terms
				&& lhs._related == rhs._related
				&& lhs._raw == rhs._raw;
		}

		friend bool operator!=(const search_t& lhs, const search_t& rhs)
		{
			return !(lhs == rhs);
		}

		friend class search_matcher;
	};

	struct search_parent
	{
		search_t parent;
		std::string name;
		core::paths selection;
	};

	// How a search's terms combine: AND by default, OR where written, nested by parentheses. Generic in
	// what is combined, so the matcher folds one item's answers and the search index folds whole sets of
	// items through exactly the same steps - which is what makes the two agree item for item.
	template <typename Value, typename Leaf, typename And, typename Or, typename Identity>
	Value combine_search_terms(const std::vector<search_term>& terms, Leaf&& leaf, And&& and_into, Or&& or_into,
	                           Identity&& identity)
	{
		if (terms.size() == 1) return leaf(terms.front());

		struct level
		{
			bool logical_and = true;
			// A level starts with no value rather than with an operator's identity. `true` is the
			// identity for AND but the annihilator for OR, so an OR level seeded with it stays true
			// whatever it later receives - which is what made a nested group after OR match everything.
			bool has_value = false;
			Value state{};
		};

		constexpr auto max_levels = 32;
		std::array<level, max_levels> levels{};
		auto current = 0;

		const auto contribute = [&and_into, &or_into](level& target, Value value, const bool logical_and)
		{
			if (!target.has_value)
			{
				target.state = std::move(value);
				target.has_value = true;
			}
			else if (logical_and)
			{
				and_into(target.state, value);
			}
			else
			{
				or_into(target.state, value);
			}
		};

		const auto fold = [&levels, &current, &contribute, &identity]
		{
			if (current > 0)
			{
				auto& child = levels[current];
				contribute(levels[current - 1], child.has_value ? std::move(child.state) : identity(),
				           child.logical_and);
				current -= 1;
			}
		};

		for (const auto& term : terms)
		{
			auto value = leaf(term);
			auto opened = 0;

			for (auto i = 0; i < term.modifiers.begin_group; i++)
			{
				if (current < max_levels - 1)
				{
					current += 1;
					levels[current].logical_and = term.modifiers.logical_op != search_term_modifier_bool::m_or;
					levels[current].has_value = false;
					levels[current].state = Value{};
					opened += 1;
				}
			}

			// combine into whichever level the term now sits in, so a '(' beyond max_levels still
			// contributes its match rather than being discarded
			contribute(levels[current], std::move(value),
			           opened > 0 || term.modifiers.logical_op != search_term_modifier_bool::m_or);

			for (auto i = 0; i < term.modifiers.end_group; i++)
			{
				fold();
			}
		}

		// an unclosed '(' must still resolve into level 0; otherwise level 0 keeps its identity and the
		// query matches every item
		while (current > 0)
		{
			fold();
		}

		return levels[0].has_value ? std::move(levels[0].state) : identity();
	}

	// The fields a free-text term reads, in the order compare_text reports the one that matched. The
	// search index builds from the same tables, so what it finds cannot drift from what matches.
	struct text_contains_field
	{
		core::cached prop::item_metadata::* member;
		prop::key_ref key;
	};

	struct text_format_field
	{
		prop::key_ref key;
		std::string (*format)(const prop::item_metadata& md, file_type_ref ft);
		// Every input format reads, packed, so two items with one key always format alike.
		uint64_t (*inputs)(const prop::item_metadata& md, file_type_ref ft);
	};

	struct text_split_field
	{
		core::cached prop::item_metadata::* member;
		prop::key_ref key;
		bool (*separator)(char);
	};

	// Searched inside by contains_term, after the file name.
	std::span<const text_contains_field> text_contains_fields();
	// Formatted for display and compared whole by same_term, after the file size.
	std::span<const text_format_field> text_format_fields();
	// Split into entries, one of which compare_term must equal.
	std::span<const text_split_field> text_split_fields();

	// Whether a free-text term matches this one value: found in it, equal to it as formatted, or equal
	// to it as one entry of a split field. They read nothing but the value, so the search index asks
	// once per distinct value what the matcher asks once per item.
	bool text_term_contains(const search_term& term, std::string_view value);
	bool text_term_same(const search_term& term, std::string_view value);
	bool text_term_equals_part(const search_term& term, std::string_view part);

	// The form two texts share exactly when term matching calls them equal - NFC normalised, then each
	// code point lower-cased as core::icmp compares it - written four bytes a code point so that no two
	// different sequences can share one.
	std::string search_fold_key(std::string_view text);

	// The dates a date term compares an item by, as Group by and the history chart key it: the original
	// date (the metadata's capture date, else the file's creation stamp), the created date (the
	// metadata's, else the same stamp) and the modified stamp, the stamps in local time. Only the keys
	// `target` compares are filled; `any` fills all three.
	struct date_term_keys
	{
		core::date_t original;
		core::date_t created;
		core::date_t modified;
	};

	date_term_keys date_term_keys_of(date_parts_prop target, const index_file_item& file,
	                                 const prop::item_metadata* md);

	// Whether a date term finds an item, given the keys it compares the item by and `matches`, which
	// decides one key and answers false for one that is not a valid date. On the `any` path the original
	// and created keys usually agree, and when they do the first has already answered. The search index
	// asks this with its own keys, so it tests the same keys in the same order as the matcher.
	template <typename Key, typename Matches>
	bool date_term_found(const date_parts_prop target, const Key& original, const Key& created, const Key& modified,
	                     Matches&& matches)
	{
		const bool is_any = target == date_parts_prop::any;
		const bool by_original = target == date_parts_prop::original || is_any;

		if (by_original && matches(original)) return true;

		if ((target == date_parts_prop::created || is_any) && !(by_original && created == original) &&
			matches(created))
		{
			return true;
		}

		return (target == date_parts_prop::modified || is_any) && matches(modified);
	}

	class search_matcher
	{
		const search_t& _search;
		const search_presence_mask _required_presence;
		const uint32_t _now_days = 0;
		const location_cache* _locations = nullptr;
		core::hash_map<char, core::cached> _drive_labels;

		struct resolved_place
		{
			gps_coordinate position;
			// The reach locations.md 2.5 grants this record, or zero when the name resolved to a
			// region or country, which has extent rather than a centre.
			double attribution_km = 0.0;
		};

		// Resolved once per search, never per item (locations.md §3.3).
		core::hash_map<std::string, resolved_place, core::ihash, core::ieq> _resolved_centres;

		struct attributed_location
		{
			core::cached place;
			core::cached state;
			core::cached country;
			// The reach locations.md 2.5 grants the record that named this coordinate, and so how far
			// a more significant place may stand over it.
			double reach_km = 0.0;
			location_attribution attribution = location_attribution::none;
		};

		// Attribution answers at kilometre scale, so one lookup serves every item in a cell.
		mutable std::map<attribution_cell, attributed_location> _attributed;

		attributed_location attributed(gps_coordinate coord) const;

	public:
		search_matcher(const search_t& s, const uint32_t now_days = core::utc_now().to_days(),
		               const location_cache* locations = nullptr) :
			_search(s),
			_required_presence(s.calc_required_presence()),
			_now_days(now_days),
			_locations(locations),
			has_terms(s.has_terms()),
			need_metadata(s.needs_metadata()),
			can_match_folder(_search.can_match_folder()),
			has_related(s.has_related())
		{
			if (s.has_volume_term())
			{
				// Resolve current drive letters to volume labels once so a volume:
				// term can match items by the label of the drive they live on.
				for (const auto& d : platform::scan_drives())
				{
					if (!d.name.empty() && !core::is_empty(d.vol_name))
					{
						_drive_labels[static_cast<char>(core::to_upper(static_cast<unsigned char>(d.name[0])))] =
							core::cache(d.vol_name);
					}
				}
			}

			if (locations && s.has_named_location())
			{
				resolve_location_centres();
			}
		}

		void resolve_location_centres();

		gps_coordinate resolved_centre(const std::string& name) const
		{
			const auto found = _resolved_centres.find(name);
			return found != _resolved_centres.cend() ? found->second.position : gps_coordinate{};
		}

		// locations.md 2.5/3.2: how far the named place itself reaches, so `loc:London` answers with
		// what the gazetteer would call London rather than with whatever unnamed dot is nearest.
		double resolved_reach_km(const std::string& name) const
		{
			const auto found = _resolved_centres.find(name);
			return found != _resolved_centres.cend() ? found->second.attribution_km : 0.0;
		}

		const bool has_terms = false;
		const bool need_metadata = false;
		const bool can_match_folder = false;
		// Hoisted out of the per-item loop: match_item tested this by inspecting the related path on
		// every candidate, which every ordinary search paid for and none of them needed.
		const bool has_related = false;

		bool can_contain(const search_presence_mask& available_presence) const;

		// The presence bits every match must carry, or none when the search cannot promise any.
		const search_presence_mask& required_presence() const
		{
			return _required_presence;
		}

		search_result match_term(core::cached folder_name, const index_file_item& file, const search_term& term) const;
		// The same answers for an item whose metadata the caller already holds, null when it has none, so
		// a search reads each item's metadata once rather than once per term.
		search_result match_term(core::cached folder_name, const index_file_item& file, const prop::item_metadata* md,
		                         const search_term& term) const;
		search_result match_all_terms(core::cached folder_name, const index_file_item& file) const;
		search_result match_all_terms(core::cached folder_name, const index_file_item& file,
		                              const prop::item_metadata* md) const;
		// A positive free-text term holding `**` matches every item in a folder its path matches.
		bool text_term_matches_folder(const search_term& term, core::cached folder_name) const;
		// Whether a date term's date compares true against one date key, today being the matcher's today.
		bool date_key_matches(const search_term& term, core::date_t key) const;

		const search_t& search() const
		{
			return _search;
		}

		// The strongest relation this item has to the item the search started at, or nothing when it
		// has none. Axis priority resolves an item that qualifies several ways, so it appears once.
		std::optional<related_match> evaluate_related(core::file_path path, const index_file_item& file) const;

		search_result match_item(core::file_path path, const index_file_item& file) const;
		search_result match_folder(core::cached folder_name, core::cached name) const;
	};
}
