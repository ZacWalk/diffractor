// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Gazetteer index and reverse geocoding. Declares location_cache, which loads the
// place database into a KD-tree plus name and ngram indexes, reads full records back from the
// source file by offset, and answers bounded attribution, name search and auto-complete.

#pragma once

#include "libdesktop/util_kdtree.h"
#include "model_location.h"

class database;
class db_statement;

static constexpr auto max_country_alt_names = 16;

constexpr uint32_t to_code2(const std::string_view s)
{
	uint32_t result = 0;
	for (const auto c : s) result = result << 8 | str::to_upper(c);
	return result;
}

struct country_loc
{
	uint32_t code = {};
	str::cached name = {};
	gps_coordinate centroid = {};
};

str::cached normalize_county_abbreviation(str::cached country);
str::cached normalize_county_name(str::cached country);

// True when the token is an ISO 3166-1 country code (or a well-known alias such as UK).
// Used to recognise a country qualifier in unquoted location search input, where the
// gazetteer is not available because it is owned by the location worker.
bool is_country_code(std::string_view token);

// Conservative longitude span for a spherical radius query at `latitude`. Exposed so tests can
// pin the antimeridian/pole contract without reaching into the gazetteer index.
double location_longitude_span_degrees(double latitude, double max_km);

// Issue #119: returns the record column offset for a display-language bit relative to the
// default name column (0 = default name, 1 = first localized name, ...). A place record
// stores one localized name per set bit in langmask, ordered by ascending bit index; the
// name for bit N therefore sits after popcount(langmask below N) earlier localized names.
// Returns 0 (use the default name) when the bit is unset or the language is not selected.
int location_localized_name_offset(uint32_t langmask, int lang_bit);

class country_t
{
	char _code[3]{};
	str::cached _name = {};
	std::vector<str::cached> _alt_names;
	df::hash_map<uint32_t, str::cached> _states;
	gps_coordinate _centroid;

	// Issue #119: localized country names ordered by language bit index, one per set bit in
	// _langmask (mirrors the place-name scheme). Empty when no translations were available.
	std::vector<str::cached> _localized;
	uint32_t _langmask = 0;

public:
	static country_t null;

	country_t() = default;
	~country_t() noexcept = default;
	country_t(const country_t&) = default;
	country_t& operator=(const country_t&) = default;
	country_t(country_t&&) noexcept = default;
	country_t& operator=(country_t&&) noexcept = default;

	country_t(const std::string_view code, const str::cached name,
	          std::vector<str::cached> alt_names, const uint32_t langmask = 0,
	          std::vector<str::cached> localized = {}) noexcept
		: _name(name), _alt_names(std::move(alt_names)), _localized(std::move(localized)), _langmask(langmask)
	{
		_code[0] = code[0];
		_code[1] = code[1];
		_code[2] = 0;
	}

	void state(const std::string_view code, const str::cached name)
	{
		_states[to_code2(code)] = name;
	}

	std::string_view code() const
	{
		return {_code, 2};
	}

	uint32_t code2() const
	{
		return to_code2(_code);
	}

	const str::cached name() const
	{
		return _name;
	}

	// Issue #119: country name in the given display-language bit, or the default name when the
	// language has no translation (or none is selected, i.e. lang_bit < 0).
	str::cached localized_name(const int lang_bit) const
	{
		const auto offset = location_localized_name_offset(_langmask, lang_bit);
		if (offset == 0 || offset > static_cast<int>(_localized.size())) return _name;
		const auto localized = _localized[offset - 1];
		return str::is_empty(localized) ? _name : localized;
	}

	const std::vector<str::cached>& alt_names() const
	{
		return _alt_names;
	}

	const std::vector<str::cached>& localized_names() const
	{
		return _localized;
	}

	str::cached state(const uint32_t code) const
	{
		const auto found = _states.find(code);
		if (found != _states.cend()) return found->second;
		return {};
	}

	const gps_coordinate& centroid() const
	{
		return _centroid;
	}

	bool operator<(const country_t& other) const
	{
		return icmp(_name, other._name) < 0;
	}

	friend bool operator==(const country_t& lhs, const country_t& rhs)
	{
		return icmp(lhs._name, rhs._name) == 0;
	}

	friend bool operator!=(const country_t& lhs, const country_t& rhs)
	{
		return !(lhs == rhs);
	}

	friend class location_cache;
};

struct csv_entry;

// locations.md 2.1: index of the default name column in a current location-places.txt
// (id, latitude, longitude, stateCode, countryCode, population, langmask, flags, name).
// model_locations.cpp static_asserts this against the Cols enum.
constexpr int location_cache_default_name_col = 8;

struct location_match_part
{
	str::cached text;
	std::vector<str::part_t> highlights;
};

struct location_match
{
	location_match_part city;
	location_match_part state;
	location_match_part country;
	double distance_away{};
	location_t location;
};

using location_matches = std::vector<location_match>;

class location_cache final : public df::no_copy
{
	mutable platform::mutex _rw;
	_Guarded_by_(_rw) kd_tree _tree;
	_Guarded_by_(_rw) df::hash_map<uint32_t, country_t> _countries;
	_Guarded_by_(_rw) const df::file_path _locations_path;

	struct location_id_and_offset
	{
		uint32_t id;
		uint32_t offset;

		bool operator<(const location_id_and_offset& other) const
		{
			return id < other.id;
		}
	};

	struct ngram_t
	{
		static constexpr int depth = 4;
		uint8_t text[depth];

		ngram_t() noexcept = default;
		~ngram_t() noexcept = default;
		ngram_t(const ngram_t&) = default;
		ngram_t& operator=(const ngram_t&) = default;
		ngram_t(ngram_t&&) noexcept = default;
		ngram_t& operator=(ngram_t&&) noexcept = default;

		explicit ngram_t(const std::string_view r) noexcept
		{
			text[0] = 0;

			auto* p_out = text;
			const auto* const p_out_limit = text + depth;

			auto p_in = r.begin();
			const auto p_in_limit = r.end();

			while (p_out < p_out_limit && p_in < p_in_limit)
			{
				*p_out++ = str::normalize_for_compare(str::pop_utf8_char(p_in, p_in_limit));
			}

			while (p_out < p_out_limit)
			{
				*p_out++ = 0;
			}
		}

		bool is_possible_match(const ngram_t& other) const
		{
			return other.text[0] == text[0] &&
				(other.text[1] == 0 || other.text[1] == text[1]) &&
				(other.text[2] == 0 || other.text[2] == text[2]) &&
				(other.text[3] == 0 || other.text[3] == text[3]);
		}

		int cmp(const ngram_t& other) const noexcept
		{
			return memcmp(text, other.text, 4);
		}
	};

	struct location_ngram_and_offset
	{
		ngram_t ngram;
		uint32_t offset;

		location_ngram_and_offset() noexcept = default;
		~location_ngram_and_offset() noexcept = default;
		location_ngram_and_offset(const location_ngram_and_offset&) = default;
		location_ngram_and_offset& operator=(const location_ngram_and_offset&) = default;
		location_ngram_and_offset(location_ngram_and_offset&&) noexcept = default;
		location_ngram_and_offset& operator=(location_ngram_and_offset&&) noexcept = default;

		location_ngram_and_offset(const std::string_view r, const uint32_t off) noexcept : ngram(r), offset(off)
		{
		}

		location_ngram_and_offset(const ngram_t& n, const uint32_t off) noexcept : ngram(n), offset(off)
		{
		}

		bool operator<(const location_ngram_and_offset& other) const
		{
			return ngram.cmp(other.ngram) < 0;
		}
	};

	_Guarded_by_(_rw) kd_points _coords;
	_Guarded_by_(_rw) std::vector<location_id_and_offset> _locations_by_id;
	_Guarded_by_(_rw) std::vector<location_ngram_and_offset> _locations_by_ngram;

	// Issue #119: bit index (into location_language_codes) of the UI display language,
	// or -1 for none. Selects the localized place name from a record's language bitmap.
	std::atomic<int> _display_lang_bit = -1;

	// locations.md 2.1: column index of the default name in location-places.txt. A current
	// file carries the fixed-width flags column before the name, so the name sits one column
	// later. A stale pre-flags file is detected once during load_index and read with the old
	// offsets, so it degrades to over-qualified names rather than wrong names.
	_Guarded_by_(_rw) int _place_name_col = location_cache_default_name_col;

	void load_countries();
	void load_states();

	static int scan_entries(std::string_view line, csv_entry* entries);

	// The whole gazetteer, mapped read-only for the life of a loaded index. Reading a record is
	// then pointer arithmetic against clean shared pages rather than a seek and a getline, which
	// matters because auto-complete reads one record per ngram hit. Only touched pages become
	// resident, so the mapping costs the process no private memory for records nobody looks at.
	_Guarded_by_(_rw) platform::mapped_file_ptr _places_map;

	// The record starting at offset, without its line terminator. Empty when the offset is not
	// inside the mapping, which is how a stale offset fails safely rather than reading past the
	// end. Caller holds _rw.
	std::string_view record_at(uint32_t offset) const;

	std::atomic<uint32_t> _load_generation = 0;

	// Readiness is published outside _rw so a UI-thread check does not queue behind the exclusive
	// lock load_index holds across the whole gazetteer read, sort and tree build.
	std::atomic<bool> _index_loaded = false;

	// Shared locks here are not recursive: re-acquiring one while a writer waits deadlocks. Every
	// internal caller already holds _rw, so they use these and only the public entry points lock.
	const country_t& find_country_locked(const uint32_t code) const
	{
		const auto found = _countries.find(code);
		return found != _countries.cend() ? found->second : country_t::null;
	}

	location_t find_closest_locked(double x, double y, country_loc* country) const;
	kd_coordinates_t find_closest_geo_locked(double latitude, double longitude) const;

	// Collects every place within max_km of (x, y). Latitude is clamped: a box reaching past a
	// pole covers no places the clamped box misses. Longitude wraps, so a box crossing the
	// antimeridian is asked as two - left as one it would be empty and an item near the date
	// line would get no candidates at all. Caller holds _rw.
	void collect_within_km(double x, double y, double max_km, std::vector<kd_coordinates_t>& candidates) const;

	location_t build_location(uint32_t offset) const;
	location_t build_location(const csv_entry* entries) const;

public:
	location_cache();
	~location_cache() override = default;

	void load_index();

	// Issue #119: select the UI display language for localized place names. Accepts a
	// .po language code (e.g. "de", "es"); unknown codes fall back to the default name.
	void set_display_language(std::string_view code);

	// Issue #119: bit index of the active display language (or -1 for the default names).
	// Consumers that cache resolved place names watch this to drop stale-language entries.
	int display_language_bit() const
	{
		return _display_lang_bit.load(std::memory_order_relaxed);
	}

	bool is_index_loaded() const
	{
		return _index_loaded.load(std::memory_order_acquire);
	}

	country_loc find_country(double x, double y) const;
	location_t find_closest(double x, double y) const;
	location_t find_closest(double x, double y, country_loc* country) const;
	location_t find_by_id(uint32_t id) const;

	// locations.md 3.1: resolve a place query to its canonical record. Exact name match,
	// optionally qualified by region or country, largest population wins.
	location_t find_by_name(std::string_view query) const;
	location_t find_largest(double min_latitude, double min_longitude,
	                        double max_latitude, double max_longitude) const;

	// A map cluster needs a recognizable landmark rather than the nearest small feature.
	// Returns the highest-population place whose normal `At` radius contains the coordinate.
	location_t find_largest_attributed(double x, double y) const;

	location_t find_largest_attributed(const gps_coordinate coord) const
	{
		return find_largest_attributed(coord.latitude(), coord.longitude());
	}

	// locations.md 2.5: bounded attribution. Unlike find_closest this refuses to name a place
	// that is too far away to be a truthful answer, and says Remote instead. The optional
	// country is canonical (English) for the same reason find_country's is: it doubles as a
	// search term, while located_place::place carries the localized display names.
	located_place find_attributed(double x, double y, country_loc* country = nullptr) const;

	located_place find_attributed(const gps_coordinate coord, country_loc* country = nullptr) const
	{
		return find_attributed(coord.latitude(), coord.longitude(), country);
	}

	location_matches auto_complete(std::string_view query, uint32_t max_results,
	                               gps_coordinate default_location) const;

	const country_t& find_country(const uint32_t code) const
	{
		platform::shared_lock lock(_rw);
		return find_country_locked(code);
	}
};
