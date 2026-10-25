// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: GPS coordinate and location structures. Defines gps_coordinate for geographic
// positions, location_t for place information, attribution_cell for memoizing reverse
// geocoding, and distance calculations.

#pragma once
#include "libdesktop/util.h"


class gps_coordinate
{
public:
	static constexpr int invalid_coordinate = 180;

	// Latitude has half the range of longitude, so it needs its own bound: 180 would accept a
	// latitude that cannot exist.
	static constexpr int max_valid_latitude = 90;

	double _latitude = invalid_coordinate;
	double _longitude = invalid_coordinate;


	gps_coordinate(const double latitude, const double longitude) noexcept : _latitude(latitude), _longitude(longitude)
	{
	}

	gps_coordinate() noexcept = default;

	gps_coordinate& operator=(const gps_coordinate& other) noexcept = default;
	gps_coordinate& operator=(gps_coordinate&& other) noexcept = default;
	gps_coordinate(const gps_coordinate& other) noexcept = default;
	gps_coordinate(gps_coordinate&& other) noexcept = default;

	bool matches(const gps_coordinate other) const
	{
		return df::equiv(_latitude, other._latitude) && df::equiv(_longitude, other._longitude);
	}

	friend bool operator==(const gps_coordinate lhs, const gps_coordinate rhs)
	{
		return df::equiv(lhs._latitude, rhs._latitude)
			&& df::equiv(lhs._longitude, rhs._longitude);
	}

	friend bool operator!=(const gps_coordinate lhs, const gps_coordinate rhs)
	{
		return !(lhs == rhs);
	}

	bool is_empty() const
	{
		return df::equiv(_latitude, invalid_coordinate) || df::equiv(_longitude, invalid_coordinate);
	}

	void clear()
	{
		_latitude = invalid_coordinate;
		_longitude = invalid_coordinate;
	}

	// Exact, not df::equiv: this is a std::map key, and an absolute-epsilon comparison is not
	// transitive, which makes an ordered container using it undefined behaviour.
	bool operator<(const gps_coordinate rhs) const
	{
		if (_latitude != rhs._latitude) return _latitude < rhs._latitude;
		return _longitude < rhs._longitude;
	}

	bool is_valid() const
	{
		return std::abs(_latitude) <= max_valid_latitude && std::abs(_longitude) < invalid_coordinate;
	}

	double latitude() const
	{
		return _latitude;
	}

	double longitude() const
	{
		return _longitude;
	}

	void latitude(const double value)
	{
		_latitude = value;
	}

	void longitude(const double value)
	{
		_longitude = value;
	}

	std::string str() const;

	static void decimal_to_dms(double coord, uint32_t& deg, uint32_t& min, uint32_t& sec);

	static std::string decimal_to_dms_str(const double coord, const bool is_ns)
	{
		uint32_t n[3];
		decimal_to_dms(coord, n);
		// %u, not %lu: uint32_t is unsigned int, and under LP64 unsigned long is twice its width.
		return str::print("%u,%u,%u%c", n[0], n[1], n[2],
		                  is_ns ? (coord < 0 ? 'S' : 'N') : coord < 0 ? 'W' : 'E');
	}

	static void decimal_to_dms(const double coord, uint32_t u[3])
	{
		decimal_to_dms(coord, u[0], u[1], u[2]);
	}

	static double dms_to_decimal(int deg, int min, int sec);
	static double dms_to_decimal(double deg, double min, double sec);

	double magnitude_between_locations(const gps_coordinate& from_loc) const
	{
		const auto d_lat = _latitude - from_loc._latitude;
		const auto d_lon = _longitude - from_loc._longitude;

		return sqrt(d_lat * d_lat + d_lon * d_lon);
	}

	static double deg2rad(const double deg)
	{
		return deg * M_PI / 180;
	}

	static double rad2deg(const double rad)
	{
		return rad * 180 / M_PI;
	}

	// from http://en.wikipedia.org/wiki/Haversine_formula
	double distance_in_kilometers(const gps_coordinate other) const
	{
		static constexpr auto earth_radius_km = 6371.0;

		const auto lat1r = deg2rad(_latitude);
		const auto lon1r = deg2rad(_longitude);
		const auto lat2r = deg2rad(other._latitude);
		const auto lon2r = deg2rad(other._longitude);
		const auto u = sin((lat2r - lat1r) / 2.0);
		const auto v = sin((lon2r - lon1r) / 2.0);

		// The haversine term is a sine squared and so cannot exceed one, but rounding in the two
		// cosines can carry it a hair past it for a near-antipodal pair - and asin of anything above
		// one is NaN, which then poisons every distance comparison it reaches.
		const auto h = std::min(1.0, u * u + cos(lat1r) * cos(lat2r) * v * v);
		const auto result = 2.0 * earth_radius_km * asin(sqrt(h));

		return result;
	}
};

// locations.md 3.3: the reverse-geocode memo key. Two photos taken in the same spot never share
// an exact coordinate, so a memo keyed on gps_coordinate never hits and every located item pays
// for a gazetteer lookup. Attribution answers at kilometre scale, so a cell of about 1 km
// collapses them without changing the answer.
struct attribution_cell
{
	static constexpr double degrees = 0.01;

	int32_t lat = 0;
	int32_t lon = 0;

	attribution_cell() noexcept = default;

	explicit attribution_cell(const gps_coordinate c) noexcept :
		lat(static_cast<int32_t>(std::floor(c.latitude() / degrees))),
		lon(static_cast<int32_t>(std::floor(c.longitude() / degrees)))
	{
	}

	friend bool operator<(const attribution_cell l, const attribution_cell r)
	{
		if (l.lat != r.lat) return l.lat < r.lat;
		return l.lon < r.lon;
	}
};

struct location_and_distance_t
{
	gps_coordinate position;
	double km = 1.0;
};

struct selected_location_t
{
	uint32_t id = 0;

	std::string search_text;
	double latitude = gps_coordinate::invalid_coordinate;
	double longitude = gps_coordinate::invalid_coordinate;
};

struct split_location_result
{
	bool success = false;

	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

split_location_result split_location(std::string_view text);

// locations.md 4.2: the distance slider's detents in kilometres. A user thinks "a bit wider",
// not in linear metres, so the ladder is roughly logarithmic.
inline constexpr double location_distance_detents_km[] = {
	0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 100.0
};
inline constexpr int location_distance_detent_count = std::size(location_distance_detents_km);

// Where the slider starts when the term names a place but sets no radius. It matches the
// attribution radius of an unknown population, so "nearby" means the same thing everywhere.
inline constexpr double location_default_search_km = 10.0;

constexpr double location_distance_at_detent(const int index)
{
	return location_distance_detents_km[std::clamp(index, 0, location_distance_detent_count - 1)];
}

// Nearest on a ratio scale, so the midpoint between 1 km and 2 km is 1.41 km rather than 1.5 km.
constexpr int location_nearest_distance_detent(const double km)
{
	for (auto i = 1; i < location_distance_detent_count; ++i)
	{
		const auto lower = location_distance_detents_km[i - 1];
		const auto upper = location_distance_detents_km[i];
		if (km <= upper) return km * km < lower * upper ? i - 1 : i;
	}

	return location_distance_detent_count - 1;
}

// locations.md 5.1 rounds a map area's radius up, so it never drops an item the area contained.
constexpr int location_distance_detent_at_least(const double km)
{
	for (auto i = 0; i < location_distance_detent_count; ++i)
	{
		if (km <= location_distance_detents_km[i]) return i;
	}

	return location_distance_detent_count - 1;
}

std::string format_distance_km(double km);

// locations.md 2.1: the qualification level held in bits 0-1 of a gazetteer record's flags
// column. It is the smallest name form that uniquely identifies the place to a user.
enum class location_qualification : uint8_t
{
	name = 0,
	name_country = 1,
	name_region_country = 2,
};

// locations.md 2.1: bit layout of the flags column. Bits 3-31 are reserved, are written as
// zero by tools/generate_locations.py, and are ignored on read.
constexpr uint32_t location_flag_level_mask = 0x3;
constexpr uint32_t location_flag_extent = 0x4;

struct location_t
{
	uint32_t id = 0;
	str::cached place = {};
	str::cached state = {};
	str::cached country = {};
	gps_coordinate position = {};
	double population = 0.0;
	uint32_t flags = 0;

	location_t() noexcept = default;

	location_t& operator=(const location_t& other) noexcept = default;
	location_t& operator=(location_t&& other) noexcept = default;
	location_t(const location_t& other) noexcept = default;
	location_t(location_t&& other) noexcept = default;

	location_t(const uint32_t id_in, const str::cached city_in, const str::cached state_in,
	           const str::cached country_in, const gps_coordinate position_in, const double pop,
	           const uint32_t flags_in = 0) noexcept
		: id(id_in), place(city_in), state(state_in), country(country_in), position(position_in), population(pop),
		  flags(flags_in)
	{
	}

	// locations.md 2.1: the reserved encoding 3 is read as level 2, so an older loader facing a
	// newer file over-qualifies rather than under-qualifies.
	location_qualification qualification() const
	{
		const auto level = flags & location_flag_level_mask;
		return level >= 2u
			       ? location_qualification::name_region_country
			       : static_cast<location_qualification>(level);
	}

	// locations.md 2.1: an extent feature is matched by its bounding box and has no meaningful
	// centroid, so it is excluded from find_closest, find_largest and radius search.
	bool is_extent() const
	{
		return (flags & location_flag_extent) != 0;
	}

	friend bool operator==(const location_t& lhs, const location_t& rhs)
	{
		return lhs.id == rhs.id;
	}

	friend bool operator!=(const location_t& lhs, const location_t& rhs)
	{
		return !(lhs == rhs);
	}

	bool operator<(const location_t& other) const
	{
		return id < other.id;
	}

	bool is_empty() const
	{
		return id == 0 && str::is_empty(place) && str::is_empty(state) & str::is_empty(country) && position.
			is_empty();
	}

	bool has_gps() const
	{
		return !position.is_empty();
	}

	std::string str() const;

	void clear();
};

// locations.md 2.3: the single composer of a place label. Group headers, map bubbles, timeline
// nodes, autocomplete and canonical term formatting all go through it; nothing else composes one.
std::string qualified_name(const location_t& loc);

// locations.md 2.5: how a coordinate was attributed. Steps at/near/remote are all *located*;
// only `none` means the item has no usable coordinate.
enum class location_attribution : uint8_t
{
	none,
	at,
	near,
	remote,
};

// locations.md 2.7: eight points, not sixteen. A nearest-place bearing is not precise enough to
// justify NNW, and the extra points read as false confidence.
enum class location_bearing : uint8_t
{
	north,
	north_east,
	east,
	south_east,
	south,
	south_west,
	west,
	north_west,
};

constexpr location_bearing location_bearing_from_degrees(double degrees)
{
	while (degrees < 0.0) degrees += 360.0;
	while (degrees >= 360.0) degrees -= 360.0;
	return static_cast<location_bearing>(static_cast<int>((degrees + 22.5) / 45.0) % 8);
}

// Initial great-circle bearing from `from` towards `to`, in degrees clockwise from north.
double location_bearing_degrees(const gps_coordinate& from, const gps_coordinate& to);

struct located_place
{
	location_t place;
	location_attribution attribution = location_attribution::none;
	double distance_km = 0.0;

	// locations.md 2.7: the nearest place regardless of radius, so an item that is only Near or
	// Remote can still answer "where was that?". Never a group key and never a search term.
	location_t nearest;
	double nearest_km = 0.0;
	location_bearing nearest_bearing = location_bearing::north;

	bool is_located() const
	{
		return attribution != location_attribution::none;
	}
};

// locations.md 2.7: the one composer of a bearing descriptor -- `410 km NW of Lisbon, Portugal`.
// Empty when nothing was near enough to name or the item is already `at` the place it names.
std::string bearing_descriptor(const located_place& lp);

// locations.md 2.5: a place may label an item only within a radius scaled to its significance,
// because a large city is a reasonable answer from far away and a hamlet is not.
constexpr double location_attribution_radius_km(const double population)
{
	if (population >= 1000000.0) return 100.0;
	if (population >= 100000.0) return 50.0;
	if (population >= 10000.0) return 25.0;
	if (population >= 1000.0) return 15.0;
	return 10.0;
}

// A place is `Near` within three attribution radii, so this is the widest any place can reach.
constexpr double location_max_attribution_km = location_attribution_radius_km(1000000.0) * 3.0;

struct map_location_area
{
	std::string name;
	str::cached state = {};
	str::cached country = {};
	uint32_t count = 0;
	pointi cell = {};
	int cell_span = 1;
	gps_coordinate position = {};
	double min_latitude = 0.0;
	double min_longitude = 0.0;
	double max_latitude = 0.0;
	double max_longitude = 0.0;
	double population = 0.0;

	// locations.md 5.1: the resolved place's own position, which is where the click's radius is
	// measured from. Invalid when nothing was near enough to name the area.
	gps_coordinate place_position = {};

	// locations.md 5.1: the smallest radius that still covers every item the area displayed,
	// measured from the resolved place to the furthest corner of the photo bounds and rounded up
	// to a slider detent so the click and the slider speak the same units.
	double search_radius_km() const
	{
		const auto centre = place_position.is_valid() ? place_position : position;
		if (!centre.is_valid()) return location_distance_at_detent(0);

		const double corner_latitudes[] = {min_latitude, max_latitude};
		const double corner_longitudes[] = {min_longitude, max_longitude};
		auto furthest = 0.0;

		for (const auto lat : corner_latitudes)
		{
			for (const auto lon : corner_longitudes)
			{
				furthest = std::max(furthest, centre.distance_in_kilometers(gps_coordinate(lat, lon)));
			}
		}

		// Past the top of the ladder there is no detent that covers the area, and covering what was
		// displayed is the contract: the slider clamps at its widest, the search does not. Rounding
		// down to 100 km here dropped every item a zoomed-out area showed beyond that.
		constexpr auto widest_detent = location_distance_detents_km[location_distance_detent_count - 1];
		if (furthest > widest_detent) return furthest;

		return location_distance_at_detent(location_distance_detent_at_least(furthest));
	}

	bool contains(const pointi map_cell) const
	{
		return map_cell.x >= cell.x && map_cell.x < cell.x + cell_span &&
			map_cell.y >= cell.y && map_cell.y < cell.y + cell_span;
	}

	bool has_same_photo_bounds(const map_location_area& other) const
	{
		return position == other.position &&
			df::equiv(min_latitude, other.min_latitude) &&
			df::equiv(min_longitude, other.min_longitude) &&
			df::equiv(max_latitude, other.max_latitude) &&
			df::equiv(max_longitude, other.max_longitude);
	}

	bool operator==(const map_location_area& other) const = default;
};

inline int map_location_cell_span(const int visible_cells, const int rendered_width)
{
	if (visible_cells <= 0 || rendered_width <= 0) return 16;
	constexpr auto target_area_width = 12;
	const auto requested = std::max(1, visible_cells * target_area_width / rendered_width);
	return static_cast<int>(std::bit_floor(static_cast<unsigned>(requested)));
}
