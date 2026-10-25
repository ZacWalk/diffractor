// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Gazetteer index and reverse geocoding implementation. Loads and indexes the place
// and country files, reads records back by offset, and implements bounded coordinate
// attribution, name and id lookup, auto-complete and bounds queries over the KD-tree.

#include "pch.h"
#include "model_locations.h"
#include "model_location.h"
#include "model_property.h"
#include "app_text.h"

country_t country_t::null;

df_assert_pod(location_t);
df_assert_movable(country_t);
df_assert_movable(location_match);

static constexpr auto countries_file_name = "location-countries.txt";
static constexpr auto states_file_name = "location-states.txt";
static constexpr auto places_file_name = "location-places.txt";

constexpr auto max_location_cols = 64;

// Issue #119: location display-language table. Each entry's array index is its stable bit
// position in a place record's language bitmap (the langmask column). The generator
// (tools/generate_locations.py, LANGUAGE_BITS) writes one localized name per set bit,
// ordered by bit index, immediately after the default name column. NEVER reorder or remove
// entries - bit positions are baked into location-places.txt. Codes are geonames isolanguage
// values (ISO-639-1), which match Diffractor's .po language codes for the shipped UI languages.
static constexpr std::string_view location_language_codes[] = {
	"en", "es", "de", "fr", "it", "pt", "nl", "ru", "uk", "pl", "cs", "tr", "ja", "ko", "zh", "ar",
	"he", "hi", "th", "vi", "id", "sv", "no", "da", "fi", "el", "hu", "ro", "bg", "sr", "hr", "ca",
};

static_assert(std::size(location_language_codes) <= 32, "language bitmap is 32-bit");

// Lookup on google
// https://maps.googleapis.com/maps/api/geocode/json?search=wc1x8jy&sensor=false

struct csv_entry
{
	std::string_view _s;

	bool is_empty() const
	{
		return _s.empty();
	}

	str::cached cached_string() const
	{
		if (is_empty()) return {};
		return str::trim_and_cache(_s);
	}

	uint32_t hash() const
	{
		return str::fnv1a_i(_s);
	}

	std::string_view to_range() const
	{
		return _s;
	}

	int to_int() const
	{
		return str::to_int(_s);
	}

	uint32_t to_code2() const
	{
		return ::to_code2(_s);
	}

	double to_double() const
	{
		return str::to_double(_s);
	}

	float to_float() const
	{
		return static_cast<float>(to_double());
	}

	uint32_t to_uint32() const
	{
		uint32_t v = 0;

		for (const auto c : _s)
		{
			if (c < '0' || c > '9') break;
			v = v * 10u + static_cast<uint32_t>(c - '0');
		}

		return v;
	}
};

int location_localized_name_offset(const uint32_t langmask, const int lang_bit)
{
	if (lang_bit < 0 || lang_bit >= 32) return 0;

	const auto bit = 1u << lang_bit;
	if ((langmask & bit) == 0) return 0;

	return 1 + std::popcount(langmask & (bit - 1));
}

void location_t::clear()
{
	id = 0;
	place = {};
	state = {};
	country = {};
	position.clear();
	population = 0.0;
	flags = 0;
}

std::string format_distance_km(const double km)
{
	// SI symbols, not words, so the string reads the same as the `loc:"London, 10km"` a user types.
	if (km < 1.0) return std::format("{} m", df::round(km * 1000.0));
	if (km < 10.0 && std::fabs(km - std::round(km)) > 0.05) return std::format("{:.1f} km", km);
	return std::format("{} km", df::round(km));
}

std::string qualified_name(const location_t& loc)
{
	const auto level = loc.qualification();

	str::cached parts[3] = {};
	auto count = 0;

	parts[count++] = loc.place;
	if (level == location_qualification::name_region_country) parts[count++] = loc.state;
	if (level != location_qualification::name) parts[count++] = loc.country;

	std::string result;

	for (auto i = 0; i < count; ++i)
	{
		if (str::is_empty(parts[i])) continue;

		// A city-state, or a place whose name equals its region, must not repeat itself.
		auto repeated = false;
		for (auto j = 0; j < i && !repeated; ++j) repeated = icmp(parts[j], parts[i]) == 0;
		if (repeated) continue;

		join(result, parts[i], ", ", false);
	}

	// A record with no place name -- a country or region hit -- still needs a label.
	if (result.empty())
	{
		join(result, loc.state, ", ", false);
		join(result, loc.country, ", ", false);
	}

	return result;
}

double location_bearing_degrees(const gps_coordinate& from, const gps_coordinate& to)
{
	const auto lat1 = gps_coordinate::deg2rad(from.latitude());
	const auto lat2 = gps_coordinate::deg2rad(to.latitude());
	const auto delta_lon = gps_coordinate::deg2rad(to.longitude() - from.longitude());

	const auto y = std::sin(delta_lon) * std::cos(lat2);
	const auto x = std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(delta_lon);

	return gps_coordinate::rad2deg(std::atan2(y, x));
}

std::string bearing_descriptor(const located_place& lp)
{
	// locations.md 2.7: only steps 3-5 get a bearing. An item that is `at` a place is already
	// answered by the place name, and a bearing there would only add noise.
	if (lp.attribution == location_attribution::none || lp.attribution == location_attribution::at) return {};

	const auto place = qualified_name(lp.nearest);
	if (place.empty() || lp.nearest_km <= 0.0) return {};

	// tt.compass_points holds the eight abbreviations, north first and clockwise, comma separated.
	// Translators space them out after the comma, so each one has to be trimmed.
	const auto index = std::clamp(static_cast<int>(lp.nearest_bearing), 0, 7);
	auto remaining = tt.compass_points.sv();
	std::string_view compass = remaining;

	for (auto i = 0; i <= index; ++i)
	{
		const auto comma = remaining.find(',');
		compass = remaining.substr(0, comma);
		if (comma == std::string_view::npos) break;
		remaining = remaining.substr(comma + 1);
	}

	compass = str::trim(compass);
	return str_format(tt.bearing_fmt.sv(), format_distance_km(lp.nearest_km), compass, place);
}

std::string location_t::str() const
{
	return qualified_name(*this);
}

std::string gps_coordinate::str() const
{
	return prop::format_gps(_latitude, _longitude);
}

void gps_coordinate::decimal_to_dms(const double coord, uint32_t& deg, uint32_t& min, uint32_t& sec)
{
	const int total_sec = abs(df::round(coord * 3600));
	deg = total_sec / 3600;
	min = total_sec / 60 % 60;
	sec = total_sec % 60;
}

double gps_coordinate::dms_to_decimal(const int deg, const int min, const int sec)
{
	return deg + min / 60.0 + sec / 3600.0;
}

double gps_coordinate::dms_to_decimal(const double deg, const double min, const double sec)
{
	return deg + min / 60.0 + sec / 3600.0;
}

split_location_result split_location(const std::string_view text)
{
	constexpr auto delims = "+-";
	split_location_result result;

	const auto c1 = text.find_first_of(delims);

	if (c1 != std::string_view::npos)
	{
		const auto c2 = text.find_first_of(delims, c1 + 1);

		if (c2 != std::string_view::npos)
		{
			const auto c3 = text.find_first_of(delims, c2 + 1);

			const auto lat = text.substr(c1, c2 - c1);
			const auto lng = text.substr(c2, c3 != std::string_view::npos ? c3 - c2 : std::string_view::npos);
			const auto alt = c3 != std::string_view::npos ? text.substr(c3) : std::string_view{};

			result.x = str::to_double(lat);
			result.y = str::to_double(lng);
			result.z = str::to_double(alt);
			result.success = true;
		}
	}

	return result;
}


location_cache::location_cache() : _locations_path(df::probe_data_file(places_file_name))
{
	static_assert(std::is_trivial_v<kd_coordinates_t>);
	static_assert(std::is_trivial_v<location_id_and_offset>);
	static_assert(std::is_trivial_v<ngram_t>);
	static_assert(std::is_trivial_v<location_ngram_and_offset>);

	static_assert(sizeof(ngram_t) == 4);
	static_assert(sizeof(location_ngram_and_offset) == 8);
	static_assert(sizeof(location_id_and_offset) == 8);
	static_assert(sizeof(kd_coordinates_t) == 24);
	static_assert(sizeof(kd_point) == 8);
	static_assert(sizeof(kd_payload) == 16);
}

void location_cache::set_display_language(const std::string_view code)
{
	int bit = -1;

	for (auto i = 0; i < static_cast<int>(std::size(location_language_codes)); ++i)
	{
		if (str::icmp(location_language_codes[i], code) == 0)
		{
			bit = i;
			break;
		}
	}

	_display_lang_bit.store(bit, std::memory_order_relaxed);
}

// Iterates the lines of a mapped file without copying them and without making the whole file
// resident. A whole-file view would be simpler, but scanning it touches every page and leaves the
// file's full size in the working set; a window that is unmapped as it advances keeps the resident
// cost bounded and is measurably faster besides.
class mapped_line_reader
{
	static constexpr uint64_t default_window = 4ull * 1024 * 1024;

	// A record is under a hundred bytes, so a line that will not fit in a window this large is a
	// corrupt or binary file rather than a long record. Growing without a bound would map it whole.
	static constexpr uint64_t max_window = 64ull * 1024 * 1024;

	platform::mapped_file_ptr _map;
	df::cspan _window;
	uint64_t _window_start = 0; // absolute file offset of _window[0]
	uint64_t _next = 0; // absolute file offset of the next unread line
	uint64_t _size = 0;
	uint64_t _window_len = default_window;
	bool _failed = false;

	bool fill()
	{
		_window = _map->set_window(_next, _window_len);
		_window_start = _next;

		if (_window.empty())
		{
			_failed = true;
			return false;
		}

		return true;
	}

public:
	explicit mapped_line_reader(const df::file_path path)
	{
		_map = platform::map_file(path, platform::map_mode::windowed);

		if (_map)
		{
			_size = _map->file_size();

			// A UTF-8 BOM is data to every byte-oriented reader, so it has to be stepped over
			// before the first record rather than trimmed out of the first field later.
			if (_size >= 3)
			{
				const auto head = _map->set_window(0, 3);

				if (head.size == 3 && head.data[0] == 0xEF && head.data[1] == 0xBB && head.data[2] == 0xBF)
				{
					_next = 3;
				}
			}
		}
	}

	bool is_open() const
	{
		return _map != nullptr && !_failed;
	}

	uint64_t size() const
	{
		return _size;
	}

	// Yields the next line and the absolute file offset it starts at. The line borrows the
	// mapping, so it is only valid until the following call.
	bool next(std::string_view& line, uint64_t& offset)
	{
		while (!_failed && _next < _size)
		{
			if (_window.empty() || _next < _window_start || _next >= _window_start + _window.size)
			{
				if (!fill()) break;
			}

			const auto* const begin = _window.data + (_next - _window_start);
			const auto remaining = _window.size - static_cast<size_t>(_next - _window_start);
			const auto* const nl = static_cast<const uint8_t*>(memchr(begin, '\n', remaining));
			const auto at_eof = _window_start + _window.size >= _size;

			if (!nl && !at_eof)
			{
				// The line straddles the window. Re-window from the line start, growing first if a
				// whole window was not enough to hold one line, so this cannot spin.
				if (_next == _window_start)
				{
					if (_window_len >= max_window)
					{
						_failed = true;
						break;
					}

					_window_len *= 2;
				}

				if (!fill()) break;
				continue;
			}

			const auto* const end = nl ? nl : begin + remaining;
			offset = _next;
			// Tolerate CRLF even though the shipped files are LF, so a hand-edited file still reads.
			const auto len = static_cast<size_t>(end - begin);
			const auto trimmed = (len > 0 && begin[len - 1] == '\r') ? len - 1 : len;
			line = {std::bit_cast<const char*>(begin), trimmed};
			_next += len + (nl ? 1 : 0);
			return true;
		}

		return false;
	}
};

// Counts the records a file holds so the per-record vectors can be sized exactly. Scanning for
// newlines against a mapped window costs a few milliseconds, which is cheaper than either the
// committed memory a generous reserve wastes or the reallocation an ungenerous one forces.
static size_t count_records(const df::file_path path)
{
	const auto map = platform::map_file(path, platform::map_mode::windowed);
	if (!map) return 0;

	constexpr uint64_t window = 4ull * 1024 * 1024;
	const auto size = map->file_size();
	size_t count = 0;
	auto trailing_bytes = false;

	for (uint64_t at = 0; at < size;)
	{
		const auto span = map->set_window(at, window);
		if (span.empty()) break;

		const auto* p = span.data;
		const auto* const end = span.data + span.size;

		while (p < end)
		{
			const auto* const nl = static_cast<const uint8_t*>(memchr(p, '\n', end - p));
			if (!nl) break;
			++count;
			p = nl + 1;
		}

		trailing_bytes = p < end;
		at += span.size;
	}

	// A final record with no trailing newline still counts.
	return count + (trailing_bytes ? 1 : 0);
}

static platform::mutex normalize_mutex;
using county_normalize_map = std::unordered_map<str::cached, str::cached, df::ihash, df::ieq>;
_Guarded_by_(normalize_mutex) static county_normalize_map county_abbreviations;
_Guarded_by_(normalize_mutex) static county_normalize_map county_names;


str::cached normalize_county_abbreviation(const str::cached country)
{
	platform::exclusive_lock abbreviation_lock(normalize_mutex);
	const auto found = county_abbreviations.find(country);
	return found != county_abbreviations.end() ? found->second : country;
}

str::cached normalize_county_name(const str::cached country)
{
	platform::exclusive_lock abbreviation_lock(normalize_mutex);
	const auto found = county_names.find(country);
	return found != county_names.end() ? found->second : country;
}

// ISO 3166-1 alpha-2 codes, plus common alpha-3 codes and the UK/USA/UAE aliases. Codes that
// are also ordinary English words (CAN, PER, NOR, FIN, MAR, KEN) are deliberately omitted so
// that unquoted search text is not mistaken for a country.
static constexpr std::string_view country_codes[] = {
	"AD", "AE", "AF", "AG", "AI", "AL", "AM", "AO", "AQ", "AR", "AS", "AT", "AU", "AW", "AX", "AZ",
	"BA", "BB", "BD", "BE", "BF", "BG", "BH", "BI", "BJ", "BL", "BM", "BN", "BO", "BQ", "BR", "BS",
	"BT", "BV", "BW", "BY", "BZ",
	"CA", "CC", "CD", "CF", "CG", "CH", "CI", "CK", "CL", "CM", "CN", "CO", "CR", "CU", "CV", "CW",
	"CX", "CY", "CZ",
	"DE", "DJ", "DK", "DM", "DO", "DZ",
	"EC", "EE", "EG", "EH", "ER", "ES", "ET",
	"FI", "FJ", "FK", "FM", "FO", "FR",
	"GA", "GB", "GD", "GE", "GF", "GG", "GH", "GI", "GL", "GM", "GN", "GP", "GQ", "GR", "GS", "GT",
	"GU", "GW", "GY",
	"HK", "HM", "HN", "HR", "HT", "HU",
	"ID", "IE", "IL", "IM", "IN", "IO", "IQ", "IR", "IS", "IT",
	"JE", "JM", "JO", "JP",
	"KE", "KG", "KH", "KI", "KM", "KN", "KP", "KR", "KW", "KY", "KZ",
	"LA", "LB", "LC", "LI", "LK", "LR", "LS", "LT", "LU", "LV", "LY",
	"MA", "MC", "MD", "ME", "MF", "MG", "MH", "MK", "ML", "MM", "MN", "MO", "MP", "MQ", "MR", "MS",
	"MT", "MU", "MV", "MW", "MX", "MY", "MZ",
	"NA", "NC", "NE", "NF", "NG", "NI", "NL", "NO", "NP", "NR", "NU", "NZ",
	"OM",
	"PA", "PE", "PF", "PG", "PH", "PK", "PL", "PM", "PN", "PR", "PS", "PT", "PW", "PY",
	"QA",
	"RE", "RO", "RS", "RU", "RW",
	"SA", "SB", "SC", "SD", "SE", "SG", "SH", "SI", "SJ", "SK", "SL", "SM", "SN", "SO", "SR", "SS",
	"ST", "SV", "SX", "SY", "SZ",
	"TC", "TD", "TF", "TG", "TH", "TJ", "TK", "TL", "TM", "TN", "TO", "TR", "TT", "TV", "TW", "TZ",
	"UA", "UG", "UM", "US", "UY", "UZ",
	"VA", "VC", "VE", "VG", "VI", "VN", "VU",
	"WF", "WS",
	"YE", "YT",
	"ZA", "ZM", "ZW",
	"UK", "USA", "UAE",
	"ARG", "AUS", "AUT", "BEL", "BGD", "BRA", "CHE", "CHL", "CHN", "COL", "CUB", "CYP", "CZE", "DEU",
	"DNK", "DZA", "EGY", "ESP", "ETH", "FRA", "GBR", "GHA", "GRC", "HKG", "HRV", "HUN", "IDN", "IND",
	"IRL", "IRN", "IRQ", "ISL", "ISR", "ITA", "JOR", "JPN", "KOR", "KWT", "LBN", "LKA", "LUX", "MEX",
	"MLT", "MYS", "NGA", "NLD", "NPL", "NZL", "PAK", "PHL", "POL", "PRT", "QAT", "ROU", "RUS", "SAU",
	"SGP", "SRB", "SWE", "THA", "TUN", "TUR", "TWN", "TZA", "UGA", "UKR", "VEN", "VNM", "ZAF",
};

bool is_country_code(const std::string_view token)
{
	if (token.size() < 2 || token.size() > 3) return false;

	const auto code = to_code2(token);
	return std::ranges::any_of(country_codes, [code](const std::string_view known) { return to_code2(known) == code; });
}

void location_cache::load_countries()
{
	static std::vector<std::pair<uint32_t, gps_coordinate>> centroids{
		{'AW', {12.52088038, -69.98267711}},
		{'AF', {33.83523073, 66.00473366}},
		{'AO', {-12.29336054, 17.53736768}},
		{'AI', {18.2239595, -63.06498927}},
		{'AL', {41.14244989, 20.04983396}},
		{'AX', {60.21488688, 19.95328768}},
		{'AD', {42.54229102, 1.56054378}},
		{'AE', {23.90528188, 54.3001671}},
		{'AR', {-35.3813488, -65.17980692}},
		{'AM', {40.28952569, 44.92993276}},
		{'AS', {-14.30445997, -170.7180258}},
		{'AQ', {-80.50857913, 19.92108951}},
		{'TF', {-49.24895485, 69.22666758}},
		{'AG', {17.2774996, -61.79469343}},
		{'AU', {-25.73288704, 134.4910001}},
		{'AT', {47.58549439, 14.1264761}},
		{'AZ', {40.28827235, 47.54599879}},
		{'BI', {-3.35939666, 29.87512156}},
		{'BE', {50.63981576, 4.64065114}},
		{'BJ', {9.6417597, 2.32785254}},
		{'BF', {12.26953846, -1.75456601}},
		{'BD', {23.86731158, 90.23812743}},
		{'BG', {42.76890318, 25.21552909}},
		{'BH', {26.04205135, 50.54196932}},
		{'CA', {61.36206324, -98.30777028}},
		{'BS', {24.29036702, -76.62843038}},
		{'BA', {44.17450125, 17.76876733}},
		{'BL', {17.89880451, -62.84067779}},
		{'BY', {53.53131377, 28.03209307}},
		{'BZ', {17.20027509, -88.71010486}},
		{'BM', {32.31367802, -64.7545589}},
		{'BO', {-16.70814787, -64.68538645}},
		{'BR', {-10.78777702, -53.09783113}},
		{'BB', {13.18145428, -59.559797}},
		{'BN', {4.51968958, 114.7220304}},
		{'BT', {27.41106589, 90.40188155}},
		{'BW', {-22.18403213, 23.79853368}},
		{'CF', {6.56823297, 20.46826831}},
		{'CH', {46.79785878, 8.20867471}},
		{'CL', {-37.73070989, -71.38256213}},
		{'CN', {36.56176546, 103.8190735}},
		{'CI', {7.6284262, -5.5692157}},
		{'CM', {5.69109849, 12.73964156}},
		{'CD', {-2.87746289, 23.64396107}},
		{'CG', {-0.83787463, 15.21965762}},
		{'CK', {-21.21927288, -159.7872422}},
		{'CO', {3.91383431, -73.08114582}},
		{'KM', {-11.87783444, 43.68253968}},
		{'CV', {15.95523324, -23.9598882}},
		{'CR', {9.97634464, -84.19208768}},
		{'CU', {21.62289528, -79.01605384}},
		{'CW', {12.19551675, -68.97119369}},
		{'KY', {19.42896497, -80.91213321}},
		{'CY', {34.91667211, 33.0060022}},
		{'CZ', {49.73341233, 15.31240163}},
		{'DE', {51.10698181, 10.38578051}},
		{'DJ', {11.74871806, 42.5606754}},
		{'DM', {15.4394702, -61.357726}},
		{'DK', {55.98125296, 10.02800992}},
		{'DO', {18.89433082, -70.50568896}},
		{'DZ', {28.15893849, 2.61732301}},
		{'EC', {-1.42381612, -78.75201922}},
		{'EG', {26.49593311, 29.86190099}},
		{'ER', {15.36186618, 38.84617011}},
		{'ES', {40.24448698, -3.64755047}},
		{'EE', {58.67192972, 25.54248537}},
		{'ET', {8.62278679, 39.60080098}},
		{'FI', {64.49884603, 26.2746656}},
		{'FJ', {-17.42858032, 165.4519543}},
		{'FK', {-51.74483954, -59.35238956}},
		{'FR', {42.17344011, -2.76172945}},
		{'FO', {62.05385403, -6.88095423}},
		{'FM', {7.45246814, 153.2394379}},
		{'GA', {-0.58660025, 11.7886287}},
		{'GB', {54.12387156, -2.86563164}},
		{'GE', {42.16855755, 43.50780252}},
		{'GG', {49.46809761, -2.57239064}},
		{'GH', {7.95345644, -1.21676566}},
		{'GN', {10.43621593, -10.94066612}},
		{'GM', {13.44965244, -15.39601295}},
		{'GW', {12.04744948, -14.94972445}},
		{'GQ', {1.70555135, 10.34137924}},
		{'GR', {39.07469623, 22.95555794}},
		{'GD', {12.11725044, -61.68220189}},
		{'GL', {74.71051289, -41.34191127}},
		{'GT', {15.69403664, -90.36482009}},
		{'GU', {13.44165626, 144.7679102}},
		{'GY', {4.79378034, -58.98202459}},
		{'HK', {22.39827737, 114.1138045}},
		{'HM', {-53.08724656, 73.5205171}},
		{'HN', {14.82688165, -86.6151661}},
		{'HR', {45.08047631, 16.40412899}},
		{'HT', {18.93502563, -72.68527509}},
		{'HU', {47.16277506, 19.39559116}},
		{'ID', {-2.21505456, 117.2401137}},
		{'IM', {54.22418911, -4.53873952}},
		{'IN', {22.88578212, 79.6119761}},
		{'IO', {-7.33059751, 72.44541229}},
		{'IE', {53.1754487, -8.13793569}},
		{'IR', {32.57503292, 54.27407004}},
		{'IQ', {33.03970582, 43.74353149}},
		{'IS', {64.99575386, -18.57396167}},
		{'IL', {31.46110101, 35.00444693}},
		{'IT', {42.79662641, 12.07001339}},
		{'JM', {18.15694878, -77.31482593}},
		{'JE', {49.21837377, -2.12689938}},
		{'JO', {31.24579091, 36.77136104}},
		{'JP', {37.59230135, 138.0308956}},
		{'KZ', {48.15688067, 67.29149357}},
		{'KE', {0.59988022, 37.79593973}},
		{'KG', {41.46221943, 74.54165513}},
		{'KH', {12.72004786, 104.9069433}},
		{'KI', {0.86001503, -45.61110513}},
		{'KN', {17.2645995, -62.68755265}},
		{'KR', {36.38523983, 127.8391609}},
		{'KW', {29.33431262, 47.58700459}},
		{'LA', {18.50217433, 103.7377241}},
		{'LB', {33.92306631, 35.88016072}},
		{'LR', {6.45278492, -9.32207573}},
		{'LY', {27.03094495, 18.00866169}},
		{'LC', {13.89479481, -60.96969923}},
		{'LI', {47.13665835, 9.53574312}},
		{'LK', {7.61266509, 80.70108238}},
		{'LS', {-29.58003188, 28.22723131}},
		{'LT', {55.32610984, 23.88719355}},
		{'LU', {49.76725361, 6.07182201}},
		{'LV', {56.85085163, 24.91235983}},
		{'MO', {22.22311688, 113.5093212}},
		{'MF', {18.08888611, -63.05972851}},
		{'MA', {29.83762955, -8.45615795}},
		{'MC', {43.75274627, 7.40627677}},
		{'MD', {47.19498804, 28.45673372}},
		{'MG', {-19.37189587, 46.70473674}},
		{'MV', {3.7287092, 73.45713004}},
		{'MX', {23.94753724, -102.5234517}},
		{'MH', {7.00376358, 170.3397612}},
		{'MK', {41.59530893, 21.68211346}},
		{'ML', {17.34581581, -3.54269065}},
		{'MT', {35.92149632, 14.40523316}},
		{'MM', {21.18566599, 96.48843321}},
		{'ME', {42.78890259, 19.23883939}},
		{'MN', {46.82681544, 103.0529977}},
		{'MP', {15.82927563, 145.6196965}},
		{'MZ', {-17.27381643, 35.53367543}},
		{'MR', {20.25736706, -10.34779815}},
		{'MS', {16.73941406, -62.18518546}},
		{'MU', {-20.27768704, 57.57120551}},
		{'MW', {-13.21808088, 34.28935599}},
		{'MY', {3.78986846, 109.6976228}},
		{'NA', {-22.13032568, 17.20963567}},
		{'NC', {-21.29991806, 165.6849237}},
		{'NE', {17.41912493, 9.38545882}},
		{'NF', {-29.0514609, 167.9492168}},
		{'NG', {9.59411452, 8.08943895}},
		{'NI', {12.84709429, -85.0305297}},
		{'NU', {-19.04945708, -169.8699468}},
		{'NL', {52.1007899, 5.28144793}},
		{'NO', {68.75015572, 15.34834656}},
		{'NP', {28.24891365, 83.9158264}},
		{'NR', {-0.51912639, 166.9325682}},
		{'NZ', {-41.81113557, 171.4849235}},
		{'OM', {20.60515333, 56.09166155}},
		{'PK', {29.9497515, 69.33957937}},
		{'PA', {8.51750797, -80.11915156}},
		{'PN', {-24.36500535, -128.317042}},
		{'PE', {-9.15280381, -74.38242685}},
		{'PH', {11.77536778, 122.8839325}},
		{'PW', {7.28742784, 134.4080797}},
		{'PG', {-6.46416646, 145.2074475}},
		{'PL', {52.12759564, 19.39012835}},
		{'PR', {18.22813055, -66.47307604}},
		{'KP', {40.15350311, 127.1924797}},
		{'PT', {39.59550671, -8.50104361}},
		{'PY', {-23.22823913, -58.40013703}},
		{'PS', {31.91613893, 35.19628705}},
		{'PF', {-14.72227409, -144.9049439}},
		{'QA', {25.30601188, 51.18479632}},
		{'RO', {45.85243127, 24.97293039}},
		{'RU', {61.98052209, 96.68656112}},
		{'RW', {-1.99033832, 29.91988515}},
		{'EH', {24.22956739, -12.21982755}},
		{'SA', {24.12245841, 44.53686271}},
		{'SD', {15.99035669, 29.94046812}},
		{'SS', {7.30877945, 30.24790002}},
		{'SN', {14.36624173, -14.4734924}},
		{'SG', {1.35876087, 103.8172559}},
		{'GS', {-54.46488248, -36.43318388}},
		{'SH', {-12.40355951, -9.54779416}},
		{'SB', {-8.92178022, 159.6328767}},
		{'SL', {8.56329593, -11.79271247}},
		{'SV', {13.73943744, -88.87164469}},
		{'SM', {43.94186747, 12.45922334}},
		{'SO', {4.75062876, 45.70714487}},
		{'PM', {46.91918789, -56.30319779}},
		{'RS', {44.2215032, 20.78958334}},
		{'ST', {0.44391445, 6.72429658}},
		{'SR', {4.13055413, -55.9123457}},
		{'SK', {48.70547528, 19.47905218}},
		{'SI', {46.11554772, 14.80444238}},
		{'SE', {62.77966519, 16.74558049}},
		{'SZ', {-26.55843045, 31.4819369}},
		{'SX', {18.05081728, -63.05713363}},
		{'SC', {-4.66099094, 55.47603279}},
		{'SY', {35.02547389, 38.50788204}},
		{'TC', {21.83047572, -71.97387881}},
		{'TD', {15.33333758, 18.64492513}},
		{'TG', {8.52531356, 0.96232845}},
		{'TH', {15.11815794, 101.0028813}},
		{'TJ', {38.5304539, 71.01362631}},
		{'TM', {39.11554137, 59.37100021}},
		{'TL', {-8.82889162, 125.8443898}},
		{'TO', {-20.42843174, -174.8098734}},
		{'TT', {10.45733408, -61.26567923}},
		{'TN', {34.11956246, 9.55288359}},
		{'TR', {39.0616029, 35.16895346}},
		{'TW', {23.7539928, 120.9542728}},
		{'TZ', {-6.27565408, 34.81309981}},
		{'UG', {1.27469299, 32.36907971}},
		{'UA', {48.99656673, 31.38326469}},
		{'UY', {-32.79951534, -56.01807053}},
		//{ 'US',	{ 	45.6795472, -112.4616737 } },
		{'US', {39.8333333, -98.585522}},
		{'UZ', {41.75554225, 63.14001528}},
		{'VA', {41.90174985, 12.43387177}},
		{'VC', {13.22472269, -61.20129695}},
		{'VE', {7.12422421, -66.18184123}},
		{'VG', {18.52585755, -64.47146992}},
		{'VI', {17.95500624, -64.80301538}},
		{'VN', {16.6460167, 106.299147}},
		{'VU', {-16.22640909, 167.6864464}},
		{'WF', {-13.88737039, -177.3483483}},
		{'WS', {-13.75324346, -172.1648506}},
		{'YE', {15.90928005, 47.58676189}},
		{'ZA', {-29.00034095, 25.08390093}},
		{'ZM', {-13.45824152, 27.77475946}},
		{'ZW', {-19.00420419, 29.8514412}}
	};

	county_normalize_map abbreviations;
	county_normalize_map names;
	csv_entry entries[max_location_cols];
	mapped_line_reader file(df::probe_data_file(countries_file_name));

	// Records are assigned by code, so a reload would otherwise keep countries the new file no
	// longer has. Cleared before the open test, not inside it, because the normalization maps
	// below are swapped in unconditionally: a failed load has to leave all three empty together
	// rather than a stale country table with no names to match it by.
	_countries.clear();

	if (file.is_open())
	{
		std::string_view line;
		uint64_t offset = 0;

		while (file.next(line, offset))
		{
			const auto entry_count = scan_entries(line, entries);

			if (entry_count > 1)
			{
				const auto code = entries[0].to_code2();
				const auto code_text = entries[0].cached_string();
				const auto name = entries[1].cached_string();

				// Issue #119: langmask + localized country names (ordered by bit) precede the
				// untagged search alt names. Localized names feed both display and the
				// abbreviation/name normalization maps so a country can be matched by any name.
				const auto langmask = entries[2].to_uint32();
				const auto localized_count = std::popcount(langmask);

				std::vector<str::cached> localized;
				localized.reserve(localized_count);

				for (auto i = 0; i < localized_count && (3 + i) < entry_count; i++)
				{
					auto&& csv_entry = entries[3 + i];
					const auto localized_name = csv_entry.cached_string();
					localized.emplace_back(localized_name);

					if (!csv_entry.is_empty())
					{
						abbreviations[localized_name] = code_text;
						names[localized_name] = name;
					}
				}

				std::vector<str::cached> alt_names;

				for (auto i = 3 + localized_count; i < entry_count; i++)
				{
					auto&& csv_entry = entries[i];

					if (!csv_entry.is_empty())
					{
						const auto alt_name = csv_entry.cached_string();

						alt_names.emplace_back(alt_name);
						abbreviations[alt_name] = code_text;
						names[alt_name] = name;
					}
				}

				abbreviations[name] = code_text;
				names[name] = name;
				names[code_text] = name;

				_countries[code] = country_t(code_text, name, std::move(alt_names), langmask, std::move(localized));
			}
		}

		for (const auto& c : centroids)
		{
			const auto found = _countries.find(c.first);

			if (found != _countries.end())
			{
				found->second._centroid = c.second;
			}
		}
	}

	platform::exclusive_lock abbreviation_lock(normalize_mutex);
	std::swap(county_abbreviations, abbreviations);
	std::swap(county_names, names);
}


void location_cache::load_states()
{
	csv_entry entries[max_location_cols];
	mapped_line_reader file(df::probe_data_file(states_file_name));

	if (file.is_open())
	{
		std::string_view line;
		uint64_t offset = 0;

		while (file.next(line, offset))
		{
			const auto entry_count = scan_entries(line, entries);
			df::assert_true(entry_count == 2);

			if (entry_count == 2)
			{
				const auto id = entries[0]._s;
				const auto found_sep = id.find('.');

				if (found_sep != std::string_view::npos)
				{
					const auto country_code = to_code2(id.substr(0, found_sep));
					const auto found_country = _countries.find(country_code);

					if (found_country != _countries.end())
					{
						const auto state_code = id.substr(found_sep + 1);
						found_country->second.state(state_code, entries[1].cached_string());
					}
				}
			}
		}
	}
}

namespace Cols
{
	// locations.md 2.1: the flags column is fixed width and sits immediately BEFORE the name
	// column, because the name is followed by a variable-length run of localized names. Mirrors
	// the layout written by tools/generate_locations.py (CityRecord.__str__).
	enum GeoNamesCols
	{
		id = 0,
		latitude,
		longitude,
		stateCode,
		countryCode,
		population,
		langmask,
		flags,
		name
	};

	// The name column of a stale file that predates the flags column.
	constexpr int legacy_name = flags;
};

static_assert(Cols::name == location_cache_default_name_col,
              "location_cache::_place_name_col default must match the current layout");

// locations.md 2.1: a record from a current file has an all-digit flags column where a stale
// file has the place name. Detected once per load; a stale file is then read with the legacy
// offsets so it degrades to over-qualified names rather than wrong names.
static bool is_flags_column(const std::string_view s)
{
	if (s.empty()) return false;
	for (const auto c : s)
	{
		if (c < '0' || c > '9') return false;
	}
	return true;
}

int location_cache::scan_entries(const std::string_view line, csv_entry* entries)
{
	std::fill_n(entries, max_location_cols, csv_entry{});

	auto col_count = 0;
	auto* col = entries;
	const auto* const col_end = entries + max_location_cols;
	auto i = 0u;
	auto begin = 0u;

	while (i < line.size())
	{
		const auto c = line[i];

		if (c == '\t' && col < col_end)
		{
			col->_s = line.substr(begin, i - begin);
			col++;
			begin = i + 1;
			col_count += 1;
		}

		++i;
	}

	if (col < col_end)
	{
		col->_s = line.substr(begin, i - begin);
		col_count += 1;
	}

	return col_count;
}

std::string_view location_cache::record_at(const uint32_t offset) const
{
	if (!_places_map) return {};

	const auto span = _places_map->data();
	if (offset >= span.size) return {};

	const auto* const begin = span.data + offset;
	const auto remaining = span.size - offset;
	const auto* const nl = static_cast<const uint8_t*>(memchr(begin, '\n', remaining));
	auto len = nl ? static_cast<size_t>(nl - begin) : remaining;
	if (len > 0 && begin[len - 1] == '\r') --len;

	return {std::bit_cast<const char*>(begin), len};
}

location_t location_cache::build_location(const uint32_t offset) const
{
	const auto line = record_at(offset);
	if (line.empty()) return {};

	csv_entry entries[max_location_cols];

	if (scan_entries(line, entries) <= 0) return {};

	return build_location(entries);
}

location_t location_cache::build_location(const csv_entry* entries) const
{
	const auto country_code = entries[Cols::countryCode].to_code2();
	const auto& country = find_country_locked(country_code);
	const auto state = country.state(entries[Cols::stateCode].to_code2());
	const auto population = entries[Cols::population].to_double();
	const auto lang_bit = _display_lang_bit.load(std::memory_order_relaxed);
	const auto name_col = _place_name_col;

	// locations.md 2.1: bits 0-1 are the qualification level, bit 2 marks an extent feature.
	// A stale file has no flags column, so the record reads as level 0; location_t treats an
	// absent level conservatively where it matters.
	const auto flags = name_col == Cols::name ? entries[Cols::flags].to_uint32() : 0u;

	// Issue #119: prefer the place name in the selected UI language. langmask has one bit set
	// per localized name that follows the default name column, ordered by bit index; the
	// helper resolves the column offset. Offset 0 means fall back to the default name.
	auto place = entries[name_col].cached_string();
	const auto offset = location_localized_name_offset(entries[Cols::langmask].to_uint32(), lang_bit);

	if (offset != 0)
	{
		const auto localized = entries[name_col + offset].cached_string();
		if (!str::is_empty(localized)) place = localized;
	}

	const gps_coordinate position(entries[Cols::latitude].to_double(), entries[Cols::longitude].to_double());
	return location_t(entries[Cols::id].to_int(), place, state, country.localized_name(lang_bit), position,
	                  population, flags);
}

void location_cache::load_index()
{
	platform::exclusive_lock lock(_rw);

	// A reload tears down what readers were gated on, so retract readiness for its duration.
	_index_loaded.store(false, std::memory_order_release);
	_places_map.reset();

	// The load appends, so a second call has to start from empty or it would index every record
	// twice and leave lookups resolving to whichever duplicate sorted first.
	_coords.clear();
	_locations_by_id.clear();
	_locations_by_ngram.clear();

	load_countries();
	load_states();

	csv_entry entries[max_location_cols];

	// Created before the scan so the section exists for the whole load. A mapped file cannot be
	// truncated or replaced underneath us, so the offsets the scan records still address the
	// records they were taken from. An unread view costs nothing: only touched pages go resident.
	auto places_map = platform::map_file(_locations_path);
	mapped_line_reader file(_locations_path);

	// Without the read-back view every offset the scan records would be unreadable, so an index
	// built now would answer every lookup with a blank place. No index is the honest answer.
	if (file.is_open() && places_map)
	{
		// Reserving for a guessed record count either wastes committed memory or forces the
		// vectors to reallocate mid-load, and both show up in the figure users read as the app's
		// memory use. Counting newlines first is a few milliseconds against a mapped file and
		// makes the two per-record vectors exact, so neither grows nor needs shrinking after.
		const auto record_count = count_records(_locations_path);

		_coords.reserve(record_count);
		_locations_by_id.reserve(record_count);

		// Measured at 3.78 names per record across the shipped gazetteer. Rounding up buys one
		// allocation that is a few percent large in exchange for never reallocating 8 MB. The cap
		// stops a corrupt record count turning the hint into a huge speculative allocation.
		constexpr size_t max_ngram_reserve = 8u * 1024 * 1024;
		_locations_by_ngram.reserve(std::min<size_t>(record_count, max_ngram_reserve / 4) * 4);

		std::string_view line;
		uint64_t pos = 0;
		auto first_record = true;
		auto abandoned = false;

		while (file.next(line, pos))
		{
			if (df::is_closing)
			{
				// Returning here would publish a half-built index that never gets sorted or
				// tree-built, so every later lookup would be wrong rather than simply empty.
				abandoned = true;
				break;
			}

			const auto entry_count = scan_entries(line, entries);

			if (first_record)
			{
				first_record = false;

				// locations.md 2.1: decide the layout once from the first record. A file without
				// the flags column is a stale file, not a supported variant - read it with the
				// legacy offsets so names stay correct, and say so once.
				_place_name_col = entry_count > Cols::name && is_flags_column(entries[Cols::flags].to_range())
					                  ? static_cast<int>(Cols::name)
					                  : static_cast<int>(Cols::legacy_name);

				if (_place_name_col != Cols::name)
				{
					df::log(__FUNCTION__,
					        std::format(
						        "{} predates the qualification-level column; place names will be over-qualified",
						        places_file_name));
				}
			}

			const auto id = static_cast<uint32_t>(entries[Cols::id].to_int());
			const auto country = entries[Cols::countryCode].to_code2();
			const auto offset = static_cast<uint32_t>(pos);
			const auto x = entries[Cols::latitude].to_float();
			const auto y = entries[Cols::longitude].to_float();
			const auto population = entries[Cols::population].to_float();

			// A short, unidentified or off-globe record would still be indexed and would still be
			// returned by find_closest, so an attribution could name a place that has no record to
			// read back. Skip it instead; the rest of the file is still good. Offsets are stored
			// as 32 bits, so a record past 4 GB has no addressable offset and is skipped too.
			if (entry_count <= _place_name_col || id == 0 ||
				pos > std::numeric_limits<uint32_t>::max() ||
				!std::isfinite(x) || !std::isfinite(y) ||
				std::abs(x) > 90.0f || std::abs(y) > 180.0f)
			{
				continue;
			}

			_coords.emplace_back(x, y, offset, country, id, population);
			_locations_by_id.emplace_back(id, offset);

			const auto name_entry_count = entry_count - _place_name_col;
			const auto* const name_entries = entries + _place_name_col;

			for (auto i = 0; i < name_entry_count; i++)
			{
				auto r = name_entries[i].to_range();

				if (!r.empty())
				{
					_locations_by_ngram.emplace_back(r, offset);
				}
			}
		}

		// A reader that failed mid-file has delivered part of the gazetteer, and a partial index
		// answers lookups with wrong neighbours rather than with nothing. Treat it as abandoned.
		if (abandoned || !file.is_open())
		{
			_locations_by_id.clear();
			_locations_by_ngram.clear();
			_coords.clear();
		}
		else
		{
			_places_map = std::move(places_map);
		}
	}

	std::sort(_locations_by_id.begin(), _locations_by_id.end());
	std::sort(_locations_by_ngram.begin(), _locations_by_ngram.end());

	_tree.build(_coords);
	++_load_generation;
	_index_loaded.store(!_tree.is_empty(), std::memory_order_release);
}

struct location_match_possible
{
	// The record's file offset rather than its text: the mapping outlives the match list, so a
	// candidate can be re-read after sorting without copying every line that was considered.
	uint32_t offset{};
	location_match_part city;
	location_match_part state;
	location_match_part country;
	double distance_away{};
	double population{};

	// 0 the typed name is the whole place name, 1 it is a prefix of it, 2 it appears anywhere.
	int name_rank = 2;

	// locations.md 2.2 + 3.4: the record a bare name should resolve to is the one the user means,
	// which is the exact spelling first and then the largest place. Ordering by proximity to the
	// default location alone answered "London" with whichever hamlet happened to be nearest.
	bool operator<(const location_match_possible& other) const
	{
		if (name_rank != other.name_rank) return name_rank < other.name_rank;
		if (population != other.population) return population > other.population;
		return distance_away < other.distance_away;
	}
};

static bool find_match(const str::cached text, const std::string_view query, str::cached& text_result,
                       str::part_t& highlight_result)
{
	const auto found = ifind(text, query);

	if (found != std::string_view::npos)
	{
		text_result = text;
		highlight_result = {found, query.length()};
		return true;
	}

	return false;
}

static bool find_match(const std::string_view text, const std::string_view query, str::cached& text_result,
                       str::part_t& highlight_result)
{
	const auto found = str::ifind(text, query);

	if (found != std::string_view::npos)
	{
		text_result = str::cache(text);
		highlight_result = {found, query.length()};
		return true;
	}

	return false;
}

static bool find_match(const csv_entry* entry, const int entry_count, const std::string_view query,
                       str::cached& text_result, str::part_t& highlight_result)
{
	for (auto i = 0; i < entry_count; i++)
	{
		if (find_match(entry[i].to_range(), query, text_result, highlight_result))
		{
			return true;
		}
	}

	return false;
}

static bool find_match(const country_t& country, const std::string_view query, str::cached& text_result,
                       str::part_t& highlight_result)
{
	if (find_match(country.name(), query, text_result, highlight_result))
	{
		return true;
	}

	if (find_match(country.code(), query, text_result, highlight_result))
	{
		return true;
	}

	for (const auto& an : country.alt_names())
	{
		if (find_match(an, query, text_result, highlight_result))
			return true;
	}

	// Issue #119: also match localized country names so a country can be found in any language.
	for (const auto& ln : country.localized_names())
	{
		if (find_match(ln, query, text_result, highlight_result))
			return true;
	}

	return false;
}

location_matches location_cache::auto_complete(const std::string_view query, const uint32_t max_results,
                                               const gps_coordinate default_location) const
{
	platform::shared_lock lock(_rw);

	location_matches result;
	std::vector<uint32_t> ngram_matches;
	std::vector<location_match_possible> possible_matches;

	country_loc closest_country;
	find_closest_locked(default_location.latitude(), default_location.longitude(), &closest_country);
	const auto query_lower = str::to_lower(query);
	auto query_parts = str::split(query_lower, true);

	for (auto&& part : query_parts)
	{
		part = str::trim(part);
	}

	if (!query_parts.empty())
	{
		const auto short_query = query_parts.size() == 1 && query_parts[0].size() < 3;
		if (query_parts.size() == 1 && query_parts[0].size() == 2)
		{
			const auto found_country = _countries.find(to_code2(query_parts[0]));
			if (found_country != _countries.end())
			{
				location_match country_match;
				country_match.location.country = found_country->second.localized_name(_display_lang_bit.load());
				country_match.country.text = country_match.location.country;
				country_match.country.highlights.emplace_back(0, query_parts[0].size());
				result.emplace_back(std::move(country_match));
			}
		}

		const auto ngram = ngram_t(query_parts[0]);
		auto found_ngram = std::lower_bound(_locations_by_ngram.begin(), _locations_by_ngram.end(),
		                                    location_ngram_and_offset{ngram, 0});

		while (found_ngram != _locations_by_ngram.end() && found_ngram->ngram.is_possible_match(ngram))
		{
			ngram_matches.emplace_back(found_ngram->offset);
			++found_ngram;
		}

		std::ranges::sort(ngram_matches);
		ngram_matches.erase(std::ranges::unique(ngram_matches).begin(), ngram_matches.end());

		if (_places_map)
		{
			for (const auto& line_offset : ngram_matches)
			{
				csv_entry entries[max_location_cols];
				const auto entry_count = scan_entries(record_at(line_offset), entries);
				const auto& country = find_country_locked(entries[Cols::countryCode].to_code2());
				const auto state = country.state(entries[Cols::stateCode].to_code2());
				const auto is_same_country = closest_country.code == country.code2();
				const auto name_col_count = entry_count - _place_name_col;

				auto match_count = 0u;
				location_match_possible possible;

				for (const auto& part : query_parts)
				{
					if (!short_query || is_same_country)
					{
						str::cached text_result;
						str::part_t highlight_result;
						bool has_match = false;

						if (is_empty(possible.city.text))
						{
							if (find_match(entries + _place_name_col, name_col_count, part, text_result,
							               highlight_result))
							{
								possible.city.text = text_result;
								possible.city.highlights.emplace_back(highlight_result);
								match_count += 1;
								has_match = true;
							}
						}
						else
						{
							if (find_match(possible.city.text, part, text_result, highlight_result))
							{
								possible.city.highlights.emplace_back(highlight_result);
								match_count += 1;
								has_match = true;
							}
						}

						if (!has_match)
						{
							if (is_empty(possible.country.text))
							{
								if (find_match(country, part, text_result, highlight_result))
								{
									possible.country.text = text_result;
									possible.country.highlights.emplace_back(highlight_result);
									match_count += 1;
									has_match = true;
								}
							}
							else
							{
								if (find_match(possible.country.text, part, text_result, highlight_result))
								{
									possible.country.highlights.emplace_back(highlight_result);
									match_count += 1;
									has_match = true;
								}
							}
						}

						if (!has_match)
						{
							if (is_empty(possible.state.text))
							{
								if (find_match(state, part, text_result, highlight_result))
								{
									possible.state.text = text_result;
									possible.state.highlights.emplace_back(highlight_result);
									match_count += 1;
									has_match = true;
								}
							}
							else
							{
								if (find_match(possible.state.text, part, text_result, highlight_result))
								{
									possible.state.highlights.emplace_back(highlight_result);
									match_count += 1;
									has_match = true;
								}
							}
						}

						if (!has_match)
						{
							break;
						}
					}
				}

				if (match_count == query_parts.size())
				{
					gps_coordinate position(entries[Cols::latitude].to_double(), entries[Cols::longitude].to_double());
					possible.distance_away = default_location.magnitude_between_locations(position);
					possible.population = entries[Cols::population].to_double();

					if (const auto matched_name = possible.city.text.sv(); !matched_name.empty())
					{
						const auto& primary = query_parts.front();
						if (str::icmp(matched_name, primary) == 0) possible.name_rank = 0;
						else if (str::starts(matched_name, primary)) possible.name_rank = 1;
					}

					possible.offset = line_offset;
					possible_matches.emplace_back(possible);
				}
			}

			std::sort(possible_matches.begin(), possible_matches.end());

			for (const auto& possible : possible_matches)
			{
				if (result.size() < max_results)
				{
					csv_entry entries[max_location_cols];
					const auto col_count = scan_entries(record_at(possible.offset), entries);

					location_match lm;

					if (col_count > 0)
					{
						lm.location = build_location(entries);
					}

					lm.city = possible.city;
					lm.state = possible.state;
					lm.country = possible.country;

					if (is_empty(lm.city.text)) lm.city.text = lm.location.place;
					if (is_empty(lm.state.text)) lm.state.text = lm.location.state;
					if (is_empty(lm.country.text)) lm.country.text = lm.location.country;

					lm.distance_away = possible.distance_away;
					result.emplace_back(lm);
				}
			}
		}
	}

	return result;
}

location_t location_cache::find_by_name(const std::string_view query) const
{
	platform::shared_lock lock(_rw);

	location_t result;
	if (query.empty()) return result;

	// locations.md 3.1: a place query is a name, optionally qualified by region and country.
	auto query_parts = str::split(query, true, [](const char c) { return c == ','; });

	for (auto&& part : query_parts)
	{
		part = str::trim(part);
	}

	std::erase_if(query_parts, [](const std::string_view p) { return p.empty(); });
	if (query_parts.empty()) return result;

	const auto name = query_parts[0];

	std::vector<uint32_t> ngram_matches;
	const auto ngram = ngram_t(name);
	auto found_ngram = std::lower_bound(_locations_by_ngram.begin(), _locations_by_ngram.end(),
	                                    location_ngram_and_offset{ngram, 0});

	while (found_ngram != _locations_by_ngram.end() && found_ngram->ngram.is_possible_match(ngram))
	{
		ngram_matches.emplace_back(found_ngram->offset);
		++found_ngram;
	}

	std::ranges::sort(ngram_matches);
	ngram_matches.erase(std::ranges::unique(ngram_matches).begin(), ngram_matches.end());

	if (!_places_map) return result;

	// The canonical record is the exact-name match with the largest population, so a bare
	// name never resolves to an unrelated substring completion (locations.md defect 1).
	auto best_population = -1.0;

	for (const auto& line_offset : ngram_matches)
	{
		csv_entry entries[max_location_cols];
		const auto entry_count = scan_entries(record_at(line_offset), entries);
		if (entry_count <= _place_name_col) continue;

		auto name_matched = false;

		for (auto i = _place_name_col; i < entry_count && !name_matched; ++i)
		{
			name_matched = str::icmp(str::trim(entries[i].to_range()), name) == 0;
		}

		if (!name_matched) continue;

		const auto& country = find_country_locked(entries[Cols::countryCode].to_code2());
		const auto state = country.state(entries[Cols::stateCode].to_code2());
		auto qualifiers_matched = true;

		for (size_t i = 1; i < query_parts.size() && qualifiers_matched; ++i)
		{
			const auto qualifier = query_parts[i];
			qualifiers_matched = str::icmp(state.sv(), qualifier) == 0 ||
				str::icmp(country.localized_name(_display_lang_bit.load()).sv(), qualifier) == 0 ||
				str::icmp(country.name().sv(), qualifier) == 0;
		}

		if (!qualifiers_matched) continue;

		const auto population = entries[Cols::population].to_double();

		if (population > best_population)
		{
			best_population = population;

			// entries already describes this line, so re-scanning it into a second 1 KB array
			// only copied work that was just done.
			result = build_location(entries);
		}
	}

	return result;
}

location_t location_cache::find_by_id(const uint32_t id) const
{
	platform::shared_lock lock(_rw);
	location_t result;
	const auto found = std::lower_bound(_locations_by_id.begin(), _locations_by_id.end(),
	                                    location_id_and_offset{id, 0});

	if (found != _locations_by_id.end() && found->id == id)
	{
		result = build_location(found->offset);
	}

	return result;
}

// locations.md 4.1: a degree of longitude is only a degree of latitude's worth of ground at the
// equator, so a nearest-place search over raw degrees ranks somewhere too far east or west as
// nearer than somewhere just up the road. collect_within_km already corrects for this; the same
// factor, and the same floor near the poles, applies to the nearest-point search.
static float lon_scale_at(const double latitude)
{
	return static_cast<float>(std::max(0.05, std::cos(gps_coordinate::deg2rad(latitude))));
}

static double normalize_longitude(double longitude)
{
	while (longitude < -180.0) longitude += 360.0;
	while (longitude > 180.0) longitude -= 360.0;
	return longitude;
}

double location_longitude_span_degrees(const double latitude, const double max_km)
{
	const auto lat_span = max_km / 111.0;
	const auto min_lat = std::max(-90.0, latitude - lat_span);
	const auto max_lat = std::min(90.0, latitude + lat_span);
	const auto reaches_pole = min_lat <= -90.0 || max_lat >= 90.0;
	const auto angular_radius = max_km / 6371.0;
	const auto cos_latitude = std::abs(std::cos(gps_coordinate::deg2rad(latitude)));

	if (reaches_pole || cos_latitude <= 1e-12) return 180.0;

	const auto ratio = std::sin(angular_radius) / cos_latitude;
	return ratio < 1.0 ? gps_coordinate::rad2deg(std::asin(ratio)) : 180.0;
}

kd_coordinates_t location_cache::find_closest_geo_locked(const double latitude, const double longitude) const
{
	const gps_coordinate at(latitude, longitude);
	const auto y_scale = lon_scale_at(latitude);
	kd_coordinates_t result{};
	auto best_km = std::numeric_limits<double>::max();

	for (const auto wrapped_longitude : {longitude, longitude - 360.0, longitude + 360.0})
	{
		const auto candidate = _tree.find_closest(_coords, static_cast<float>(latitude),
		                                          static_cast<float>(wrapped_longitude), y_scale);
		if (candidate.id == 0) continue;

		const auto km = at.distance_in_kilometers(gps_coordinate(candidate.x, candidate.y));
		if (km < best_km)
		{
			best_km = km;
			result = candidate;
		}
	}

	return result;
}

country_loc location_cache::find_country(const double x, const double y) const
{
	platform::shared_lock lock(_rw);
	const auto closest = find_closest_geo_locked(x, y);
	const auto found = _countries.find(closest.country);
	// NOTE: returns the canonical (English) name deliberately. This feeds the map/heat-map
	// country grouping whose label doubles as a search term (sidebar .with(name)); the search
	// must match the canonical country name stored in the index, so it is NOT localized here.
	return found != _countries.end()
		       ? country_loc{found->second.code2(), found->second.name(), found->second.centroid()}
		       : country_loc{};
}

location_t location_cache::find_closest(const double x, const double y) const
{
	return find_closest(x, y, nullptr);
}

location_t location_cache::find_closest(const double x, const double y, country_loc* country) const
{
	platform::shared_lock lock(_rw);
	return find_closest_locked(x, y, country);
}

location_t location_cache::find_closest_locked(const double x, const double y, country_loc* country) const
{
	const auto closest = find_closest_geo_locked(x, y);

	if (country)
	{
		const auto found = _countries.find(closest.country);
		*country = found != _countries.end()
			           ? country_loc{found->second.code2(), found->second.name(), found->second.centroid()}
			           : country_loc{};
	}

	return build_location(closest.offset);
}

void location_cache::collect_within_km(const double x, const double y, const double max_km,
                                       std::vector<kd_coordinates_t>& candidates) const
{
	const auto lat_span = max_km / 111.0;
	const auto min_lat = std::max(-90.0, x - lat_span);
	const auto max_lat = std::min(90.0, x + lat_span);
	const auto lon_span = location_longitude_span_degrees(x, max_km);

	const auto min_lon = y - lon_span;
	const auto max_lon = y + lon_span;

	const auto collect = [&](const double lon_low, const double lon_high)
	{
		_tree.find_in_bounds(_coords,
		                     static_cast<float>(min_lat), static_cast<float>(lon_low),
		                     static_cast<float>(max_lat), static_cast<float>(lon_high),
		                     candidates);
	};

	if (min_lon < -180.0)
	{
		collect(-180.0, normalize_longitude(max_lon));
		collect(normalize_longitude(min_lon), 180.0);
	}
	else if (max_lon > 180.0)
	{
		collect(normalize_longitude(min_lon), 180.0);
		collect(-180.0, normalize_longitude(max_lon));
	}
	else
	{
		collect(min_lon, max_lon);
	}
}

location_t location_cache::find_largest_attributed(const double x, const double y) const
{
	const gps_coordinate at(x, y);
	if (!at.is_valid()) return {};

	platform::shared_lock lock(_rw);
	if (_tree.is_empty()) return {};

	std::vector<kd_coordinates_t> candidates;
	collect_within_km(x, y, location_attribution_radius_km(1000000.0), candidates);

	const kd_coordinates_t* largest = nullptr;
	auto largest_km = std::numeric_limits<double>::max();

	for (const auto& candidate : candidates)
	{
		const auto km = at.distance_in_kilometers(gps_coordinate(candidate.x, candidate.y));
		if (km > location_attribution_radius_km(candidate.population)) continue;

		if (!largest || candidate.population > largest->population ||
			(candidate.population == largest->population && km < largest_km))
		{
			largest = &candidate;
			largest_km = km;
		}
	}

	if (!largest) return {};

	return build_location(largest->offset);
}

// locations.md 2.5: the attribution ladder. Step 1 (stored text) belongs to the caller; step 4
// (water bodies) arrives with location-waters.txt in 2.6 and currently falls through to remote.
located_place location_cache::find_attributed(const double x, const double y, country_loc* country) const
{
	if (country) *country = {};

	const gps_coordinate at(x, y);
	if (!at.is_valid()) return {};

	platform::shared_lock lock(_rw);
	if (_tree.is_empty()) return {};

	// Weighed the same way as find_closest: raw degrees rank somewhere far east or west as nearer
	// than somewhere up the road, and this is the lookup that names the place an item is shown at.
	const auto closest = find_closest_geo_locked(x, y);
	const auto closest_km = at.distance_in_kilometers(gps_coordinate(closest.x, closest.y));

	auto winner = closest;
	auto winner_km = closest_km;
	auto attribution = location_attribution::remote;

	if (closest_km <= location_attribution_radius_km(closest.population))
	{
		// The common case: the nearest place is close enough to stand for the item.
		attribution = location_attribution::at;
	}
	else
	{
		// The nearest place is not the best answer -- a city 40 km away beats a hamlet 12 km
		// away -- so widen to every place that could possibly reach this coordinate.
		std::vector<kd_coordinates_t> candidates;
		collect_within_km(x, y, location_max_attribution_km, candidates);

		auto best_at_km = std::numeric_limits<double>::max();
		auto best_near_km = std::numeric_limits<double>::max();
		const kd_coordinates_t* best_at = nullptr;
		const kd_coordinates_t* best_near = nullptr;

		for (const auto& candidate : candidates)
		{
			const auto km = at.distance_in_kilometers(gps_coordinate(candidate.x, candidate.y));
			const auto radius = location_attribution_radius_km(candidate.population);

			if (km <= radius && km < best_at_km)
			{
				best_at_km = km;
				best_at = &candidate;
			}
			else if (km <= radius * 3.0 && km < best_near_km)
			{
				best_near_km = km;
				best_near = &candidate;
			}
		}

		if (best_at)
		{
			winner = *best_at;
			winner_km = best_at_km;
			attribution = location_attribution::at;
		}
		else if (best_near)
		{
			winner = *best_near;
			winner_km = best_near_km;
			attribution = location_attribution::near;
		}
	}

	located_place result;
	result.attribution = attribution;
	result.distance_km = attribution == location_attribution::remote ? closest_km : winner_km;
	result.nearest_km = closest_km;
	result.nearest_bearing = location_bearing_from_degrees(
		location_bearing_degrees(gps_coordinate(closest.x, closest.y), at));

	// Nothing may be named when remote, but a country is a large enough target to still be
	// true when the nearest place is within the widest reach any place is ever granted.
	const auto country_code = attribution == location_attribution::remote
		                          ? (closest_km <= location_max_attribution_km ? closest.country : 0u)
		                          : winner.country;

	if (country_code != 0)
	{
		if (const auto found = _countries.find(country_code); found != _countries.end())
		{
			if (country) *country = {found->second.code2(), found->second.name(), found->second.centroid()};

			if (attribution == location_attribution::remote)
			{
				result.place.country = found->second.localized_name(_display_lang_bit.load());
			}
		}
	}

	if (_places_map)
	{
		// locations.md 2.7 needs the nearest record even when it was too far to attribute, so a
		// remote item can still say what it was 410 km north-west of.
		result.nearest = build_location(closest.offset);

		if (attribution != location_attribution::remote)
		{
			result.place = winner.offset == closest.offset ? result.nearest : build_location(winner.offset);
		}
	}

	return result;
}

location_t location_cache::find_largest(const double min_latitude, const double min_longitude,
                                        const double max_latitude, const double max_longitude) const
{
	platform::shared_lock lock(_rw);
	std::vector<kd_coordinates_t> candidates;
	_tree.find_in_bounds(_coords, static_cast<float>(min_latitude), static_cast<float>(min_longitude),
	                     static_cast<float>(max_latitude), static_cast<float>(max_longitude), candidates);

	const kd_coordinates_t* largest = nullptr;
	for (const auto& candidate : candidates)
	{
		if (!largest || candidate.population > largest->population ||
			(df::equiv(candidate.population, largest->population) && candidate.id < largest->id))
		{
			largest = &candidate;
		}
	}

	if (!largest) return {};

	return build_location(largest->offset);
}
