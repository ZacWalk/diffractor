// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Search query parsing and matching. Parses user search text into filters and
// matches individual index records against them; query execution over the index lives in
// model_index.cpp.

#include "pch.h"
#include "model_search.h"
#include "model_index.h"
#include "model_items.h"
#include "model_locations.h"
#include "model_tokenizer.h"

df_assert_movable(df::search_t);
df_assert_movable(df::search_term);

// A search result is returned by value for every candidate an ordinary search examines, and one is
// stored per listed item. The related distance sits in padding the type already had, so relations
// cost that path nothing; this fails if a later field pushes the type into another word.
static_assert(sizeof(df::search_result) <= sizeof(void*) * 2 + 8);

constexpr static auto sv_duplicates = "duplicates";
constexpr static auto sv_remote = "remote";
constexpr static auto sv_panorama = "panorama";

void df::search_t::clear_date_properties()
{
	std::erase_if(_terms, [](auto&& v) { return v.is_date(); });
	_raw.clear();
}

df::search_t df::search_t::parse_path(const std::string_view text)
{
	search_t result;
	const folder_path folder(text);

	if (folder.is_drive() || folder.exists())
	{
		result.add_selector(item_selector(folder));
	}
	else
	{
		const auto last_slash = find_last_slash(text);
		const auto has_file_name = last_slash != std::string_view::npos && last_slash + 1 < text.size();
		const auto parent = has_file_name ? text.substr(0, last_slash) : std::string_view{};
		const auto has_drive_parent = parent.size() == 2 && parent[1] == ':';

		if (!has_file_name || (!is_path(parent) && !has_drive_parent))
		{
			return result;
		}

		const file_path file(text);

		if (file.exists() && file.is_valid())
		{
			result.add_selector(item_selector(file.folder())).with(file.name());
		}
	}

	return result;
}

// Issue #139: a path that contains spaces is wrapped in double quotes so it reads (and re-parses)
// as a single search term. Only whitespace forces quotes - str::need_quotes also fires on the colon
// in every Windows drive path, and quoting those would change what the address box shows for input
// that never needed it. Already-quoted text is returned unchanged.
std::string df::quote_path_term(const std::string_view path)
{
	if (path.find(' ') == std::string_view::npos) return std::string(path);
	if (path.front() == '\"' || path.front() == '\'') return std::string(path);

	std::string result;
	result.reserve(path.size() + 2);
	result += '\"';
	result.append(path);
	result += '\"';
	return result;
}

static std::string auto_quote_search_input(const std::string_view text)
{
	const auto trimmed = str::trim(text);

	if (!df::is_path(trimmed)) return std::string(text);

	auto quoted = df::quote_path_term(trimmed);

	// Nothing was added, so hand back exactly what was typed rather than the trimmed form (#178).
	return quoted.size() == trimmed.size() ? std::string(text) : quoted;
}

df::search_t df::search_t::parse_from_input(const std::string_view text) const
{
	// Issue #178: preserve exactly what the user typed (including quotes and terms
	// the engine does not specially recognize) so the search bar does not silently
	// revert the input to the normalized form. The builder mutators clear _raw, so
	// it is stamped here after the search has been fully constructed.
	const auto raw = auto_quote_search_input(text);
	const auto has_selector = this->has_selector();

	if (is_path(text))
	{
		auto result = parse_path(text);

		if (!result.is_empty())
		{
			result.raw_text(raw);
			return result;
		}
	}

	if (has_selector && str::starts(text, "*."))
	{
		auto a = *this;
		const auto s = a.selectors().front();
		const item_selector sel(s.folder(), s.is_recursive(), text);
		a.clear_selectors().add_selector(sel);
		a.raw_text(raw);
		return a;
	}

	auto result = parse(text);
	const auto has_query_parts = result.has_selector() &&
		(result.selectors().size() > 1 || !result.terms().empty());
	result.raw_text(has_query_parts ? std::string(text) : raw);
	return result;
}

namespace
{
	bool is_distance_token(const std::string_view token)
	{
		auto i = size_t{0};
		while (i < token.size() && (std::isdigit(static_cast<unsigned char>(token[i])) || token[i] == '.')) ++i;
		if (i == 0 || i == token.size()) return false;

		const auto unit = token.substr(i);
		return str::icmp(unit, "km") == 0 || str::icmp(unit, "m") == 0 || str::icmp(unit, "mi") == 0 ||
			str::icmp(unit, "mile") == 0 || str::icmp(unit, "miles") == 0;
	}

	bool is_size_unit_token(const std::string_view token)
	{
		// Every unit format_size writes, since format_term writes it apart from its number.
		return str::icmp(token, "kb") == 0 || str::icmp(token, "mb") == 0 || str::icmp(token, "gb") == 0 ||
			str::icmp(token, "tb") == 0;
	}
}

std::vector<search_part> df::coalesce_parts(std::vector<search_part> parts)
{
	std::vector<search_part> results;
	results.reserve(parts.size());

	for (auto& part : parts)
	{
		if (!results.empty() && part.scope.empty() && part.modifier.is_defaults())
		{
			auto& previous = results.back();

			// "size: 1 MB" is the canonical form format_terms emits, so the unit has to survive being
			// tokenized away from its number; otherwise re-running an unchanged query reads it as bytes.
			if (prop::from_prefix(previous.scope) == prop::file_size && !part.literal &&
				is_size_unit_token(part.term) && str::is_probably_num(previous.term))
			{
				previous.term += part.term;
				continue;
			}

			auto& location = previous;

			// absorb only what is recognisably part of the place, so that a following word such as
			// "sunset" stays a separate text term; the first unrecognised token ends the location
			// an explicitly quoted part is the user asking for a distinct thing, so it is never absorbed
			if (find_location_scope(location.scope) && !part.literal &&
				(part.after_comma || is_distance_token(part.term) || is_country_code(part.term)))
			{
				location.term += ", ";
				location.term += part.term;
				continue;
			}
		}

		results.emplace_back(std::move(part));
	}

	return results;
}

df::search_t df::search_t::parse(const std::string_view text)
{
	const auto trimmed = str::trim(text);
	search_t result;
	result.raw_text(text);

	if (trimmed.size() >= 2 && is_path_sep(trimmed[0]) && is_path_sep(trimmed[1]))
	{
		const auto share_separator = trimmed.find_first_of("\\/", 2);
		if (share_separator == std::string_view::npos || share_separator == 2 || share_separator + 1 == trimmed.size())
		{
			return result;
		}
	}

	if (is_path(trimmed))
	{
		result = parse_path(trimmed);
	}

	if (result.is_empty())
	{
		if (is_probably_selector(trimmed))
		{
			result.add_selector(item_selector(trimmed));
		}
		else
		{
			search_tokenizer t;

			for (const auto& part : coalesce_parts(t.parse(trimmed)))
			{
				if (part.scope.empty() && item_selector::can_iterate(part.term))
				{
					result.add_selector(item_selector(part.term));
				}
				else
				{
					result.parse_part(part);
				}
			}
		}
	}

	return result;
}

// Wrapping a value so the tokenizer reads it back whole. The rule lives here alone because every
// formatted term has to agree with the tokenizer's single un-quoting rule.
static std::string quote_term_value(const std::string_view term_text)
{
	const char quote_char = term_text.find('\"') == std::string::npos ? '\"' : '\'';
	std::string result;
	result += quote_char;

	// the tokenizer has no escape, so a value containing the delimiter is doubled and un-doubled on read
	for (const auto c : term_text)
	{
		result += c;
		if (c == quote_char) result += c;
	}

	result += quote_char;
	return result;
}

static std::string term_quote(const std::string_view term_text)
{
	auto has_special_char = std::ranges::any_of(term_text, [](const char c)
	{
		return c == ' ' || c == '\t' || c == '\'' || c == '"' || c == '!' || c == '-' ||
			c == '<' || c == '>' || c == '=' || c == '&' ||
			c == '@' || (c != ':' && search_tokenizer::is_delimiter_char(c));
	});

	// The tokenizer reads a bare "and" or "or" as the operator, so a term that is one of those words
	// has to be written quoted to read back as the text it is.
	if (!has_special_char && (str::icmp(term_text, "and") == 0 || str::icmp(term_text, "or") == 0 ||
		str::icmp(term_text, tt.query_and) == 0 || str::icmp(term_text, tt.query_or) == 0))
	{
		has_special_char = true;
	}

	if (!has_special_char)
	{
		// colons are allowed for folders or duration/time
		const auto colon_pos = term_text.find(':');

		if (colon_pos != std::string::npos)
		{
			if (colon_pos != 1) // folder
			{
				for (auto i = 0u; i < colon_pos; i++)
				{
					const auto c = term_text[i];

					if (c < '0' || c > '9')
					{
						// if not all digits, like a time ie 12:00
						has_special_char = true;
						break;
					}
				}
			}
		}
	}

	return has_special_char ? quote_term_value(term_text) : std::string(term_text);
}

// A search term has to read back as itself, so numbers are trimmed rather than rounded away.
static std::string trim_trailing_zeros(std::string v)
{
	if (v.find('.') == std::string::npos) return v;
	while (!v.empty() && v.back() == '0') v.pop_back();
	if (!v.empty() && v.back() == '.') v.pop_back();
	return v;
}

static std::string format_search_coordinate(const double v)
{
	return trim_trailing_zeros(std::format("{:.6f}", v));
}

static std::string format_search_number(const double v)
{
	std::array<char, 64> buffer{};
	const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
	if (ec == std::errc{}) return {buffer.data(), end};
	return trim_trailing_zeros(std::format("{:.17g}", v));
}

static std::string format_search_number(const float v)
{
	std::array<char, 64> buffer{};
	const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
	if (ec == std::errc{}) return {buffer.data(), end};
	return trim_trailing_zeros(std::format("{:.9g}", v));
}

// locations.md 4.2: a radius reads in the same metres and kilometres a user would type.
static std::string format_search_distance(const double km)
{
	const auto m = km * 1000.0;
	if (km < 1.0) return trim_trailing_zeros(std::format("{:.4f}", m)) + "m";
	return trim_trailing_zeros(std::format("{:.4f}", km)) + "km";
}

// locations.md 3.1: quotes are a fallback, not the default spelling. The parser itself decides
// whether the bare form is unambiguous, so this rule can never drift from the grammar.
static std::string quote_location_value(const std::string_view prefix, const std::string_view value,
                                        const df::search_term& term)
{
	std::string candidate(prefix);
	candidate += value;

	const auto reparsed = df::search_t::parse(candidate);

	if (reparsed.terms().size() == 1)
	{
		auto expected = term;
		expected.modifiers = df::search_term_modifier{};

		if (reparsed.terms().front() == expected) return std::string(value);
	}

	return term_quote(value);
}

static std::string format_xy(const df::xy16 xy)
{
	if (xy.y) return std::format("{}/{}", xy.x, xy.y);
	return str::to_string(xy.x);
}


static std::string format_term_value(const df::search_term& term)
{
	const auto* const t = term.key;
	const auto n = term.int_val;
	const auto d = term.float_val;
	const auto xy = term.xy_val;
	const auto n64 = term.int64_val;

	if (t->data_type == prop::data_type::string || !str::is_empty(term.text)) return term.text;
	if (t->data_type == prop::data_type::date) return prop::format_date(df::date_t::from_days(n));
	if (t == prop::f_number) return prop::format_f_num(d);
	if (t == prop::megapixels) return str::print("%1.1f", d);
	if (t == prop::dimensions) return prop::format_dimensions({xy.x, xy.y});
	if (t == prop::duration) return prop::format_duration(n);
	if (t == prop::exposure_time) return format_search_number(static_cast<float>(d)) + "s";
	if (t == prop::iso_speed) return prop::format_iso(n);
	if (t == prop::latitude) return prop::format_gps(d);
	if (t == prop::longitude) return prop::format_gps(d);
	if (t == prop::rating) return str::to_string(n);
	if (t == prop::audio_sample_rate) return str::to_string(n);
	if (t == prop::audio_channels) return prop::format_audio_channels(n);
	if (t == prop::audio_sample_type) return format_audio_sample_type(static_cast<prop::audio_sample_t>(n));
	if (t == prop::streams) return prop::format_streams(n);
	if (t == prop::track_num || t == prop::disk_num || t == prop::episode)
	{
		return format_xy(xy);
	}
	if (t == prop::orientation)
	{
		return str::to_string(n);
	}
	if (t == prop::year)
	{
		return str::to_string(n);
	}
	if (t == prop::focal_length) return format_search_number(static_cast<float>(d)) + "mm";
	if (t == prop::file_size) return df::format_size(df::file_size(n64));
	if (t->data_type == prop::data_type::int32)
	{
		return str::to_string(n);
	}
	if (t->data_type == prop::data_type::uint32)
	{
		return str::to_string(static_cast<uint32_t>(n));
	}

	df::assert_true(false);
	return {};
}

std::string df::format_term(const search_term& term)
{
	std::ostringstream result;

	if (term.modifiers.less_than) result << "<";
	if (term.modifiers.greater_than) result << ">";
	if (term.modifiers.equals) result << "=";
	if (term.modifiers.less_than || term.modifiers.greater_than || term.modifiers.equals) result << " ";

	if (term.type == search_term_type::has_type)
	{
		const auto scope = term.modifiers.positive ? tt.query_with : tt.query_without;
		result << scope.sv();
		result << ':';
		result << ' ';
		result << term.key->name.sv();
	}
	else if (term.type == search_term_type::has_location)
	{
		const auto scope = term.modifiers.positive ? tt.query_with : tt.query_without;
		result << scope.sv();
		result << ':';
		result << ' ';
		result << "location";
	}
	else
	{
		if (!term.modifiers.positive) result << '-';

		if (term.type == search_term_type::text)
		{
			result << term_quote(term.text);
		}
		else if (term.type == search_term_type::value)
		{
			if (prop::tag == term.key)
			{
				result << '#';
				result << term_quote(format_term_value(term));
			}
			else if (prop::file_size == term.key)
			{
				// non quoted type
				result << term.key->name.sv();
				result << ':';
				result << ' ';
				result << format_term_value(term);
			}
			else
			{
				result << term.key->name.sv();
				result << ':';
				result << ' ';
				result << term_quote(format_term_value(term));
			}
		}
		else if (term.type == search_term_type::media_type)
		{
			result << '@';
			result << term.fg_val->display_name(false);
		}
		else if (term.type == search_term_type::date)
		{
			if (term.date_val.target == date_parts_prop::created)
			{
				result << tt.query_created.sv();
				result << ":";
			}
			else if (term.date_val.target == date_parts_prop::original)
			{
				result << tt.query_original.sv();
				result << ":";
			}
			else if (term.date_val.target == date_parts_prop::modified)
			{
				result << tt.query_modified.sv();
				result << ":";
			}

			if (term.date_val.age)
			{
				if (term.date_val.target == date_parts_prop::any)
				{
					result << tt.query_age.sv();
					result << ":";
				}

				result << str::to_string(term.date_val.age);
			}
			else
			{
				const auto is_month_only = term.date_val.year == 0 && term.date_val.day == 0;
				bool is_cat = false;

				if (term.date_val.year != 0)
				{
					result << str::to_string(term.date_val.year);
					is_cat = true;
				}

				if (term.date_val.month != 0)
				{
					if (is_cat) result << "-";
					result << (is_month_only
						           ? str::month(term.date_val.month, true)
						           : str::short_month(term.date_val.month, true));
					is_cat = true;
				}

				if (term.date_val.day != 0)
				{
					if (is_cat) result << "-";
					result << str::to_string(term.date_val.day);
				}
			}
		}
		else if (term.type == search_term_type::location)
		{
			const auto prefix = std::string(location_scope_name(term.level)) + ':';
			result << prefix;

			if (term.coord_val.is_valid())
			{
				// A coordinate reads back as the same "lat, lon, radius" a user could type;
				// the older `+lat+lon+km` spelling still parses.
				std::string value = format_search_coordinate(term.coord_val.latitude());
				value += ',';
				value += format_search_coordinate(term.coord_val.longitude());

				if (term.float_val > 0.0)
				{
					value += ',';
					value += format_search_distance(term.float_val);
				}

				result << value;
			}
			else
			{
				if (term.float_val > 0.0)
				{
					const auto place_query = std::string(term.text) + ", " + format_search_distance(term.float_val);
					result << quote_location_value(prefix, place_query, term);
				}
				else
				{
					result << quote_location_value(prefix, term.text, term);
				}
			}
		}
		else if (term.type == search_term_type::area)
		{
			result << "area:" << term_quote(term.text);
		}
		else if (term.type == search_term_type::extension)
		{
			result << "ext:";
			result << term.text;
		}
		else if (term.type == search_term_type::volume)
		{
			result << "volume:";
			result << term_quote(term.text);
		}
		else if (term.type == search_term_type::duplicate)
		{
			result << "@";
			result << sv_duplicates;
		}
		else if (term.type == search_term_type::remote)
		{
			result << "@";
			result << sv_remote;
		}
		else if (term.type == search_term_type::panorama)
		{
			result << "@";
			result << sv_panorama;
		}
	}

	return result.str();
}

static void term_join(std::string& result, const df::search_term& term)
{
	if (!result.empty()) result += ' ';
	if (term.modifiers.logical_op == df::search_term_modifier_bool::m_or)
	{
		result += tt.query_or;
		result += " ";
	}
	for (auto i = 0; i < term.modifiers.begin_group; i++) result += '(';

	result += format_term(term);

	for (auto i = 0; i < term.modifiers.end_group; i++) result += ')';
}

static void term_join(std::string& result, const std::string_view term)
{
	if (!result.empty()) result += ' ';
	result += term_quote(term);
}

static void term_join(std::string& result, const std::string_view scope, const std::string_view term)
{
	if (!result.empty()) result += ' ';
	result += scope;
	result += ':';
	result += ' ';
	result += term_quote(term);
}

std::string df::search_t::format_terms() const
{
	const bool no_filter = _terms.empty() && !has_related();

	if (_selectors.size() == 1 && !has_media_type() && no_filter) return _selectors.front().str();

	std::string result;

	for (const auto& s : _selectors)
	{
		term_join(result, s.str());
	}

	if (has_related())
	{
		term_join(result, tt.query_related, _related.path.str());
	}

	for (const auto& t : _terms)
	{
		term_join(result, t);
	}

	return result;
}

std::string df::search_t::text() const
{
	return !str::is_empty(_raw) ? _raw : format_terms();
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

static int days_per_month(const int month, const int year = -1)
{
	if (month == 4 || month == 6 || month == 9 || month == 11)
	{
		return 30;
	}

	if (month == 2)
	{
		const auto leap_year = (year >= 0 && year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
		return leap_year ? 29 : 28;
	}

	return 31;
}

df::date_parts df::search_t::find_date_parts() const
{
	date_parts result;

	for (const auto& t : _terms)
	{
		if (t.type == search_term_type::date && t.no_modifiers())
		{
			if (t.date_val.age) result.age = t.date_val.age;
			if (t.date_val.day) result.day = t.date_val.day;
			if (t.date_val.month) result.month = t.date_val.month;
			if (t.date_val.year) result.year = t.date_val.year;
			result.target = t.date_val.target;
		}
	}

	return result;
}

void df::search_t::next_date(const bool forward)
{
	auto parts = find_date_parts();

	if (parts.day && parts.month)
	{
		if (forward)
		{
			// The year decides February's length, and passing only the month left it at 28 - so
			// stepping forward from the 28th of a leap February skipped the 29th entirely. A
			// year-less date still takes the 28-day reading, which is what it did before.
			const auto days_in_month = days_per_month(parts.month, parts.year ? parts.year : -1);
			parts.day += 1;

			if (parts.day > days_in_month)
			{
				parts.day = 1;
				parts.month += 1;

				if (parts.month > 12)
				{
					parts.month = 1;

					if (parts.year)
					{
						parts.year += 1;
					}
				}
			}
		}
		else
		{
			parts.day -= 1;

			if (parts.day < 1)
			{
				parts.month -= 1;

				if (parts.month < 1)
				{
					parts.month = 12;

					if (parts.year)
					{
						parts.year -= 1;
					}
				}

				parts.day = days_per_month(parts.month, parts.year ? parts.year : -1);
			}
		}

		clear_date_properties();
		day(parts.day, parts.month, parts.year, parts.target);
	}
	else if (parts.year && parts.month)
	{
		if (forward)
		{
			parts.month += 1;

			if (parts.month > 12)
			{
				parts.year += 1;
				parts.month = 1;
			}
		}
		else
		{
			parts.month -= 1;

			if (parts.month < 1)
			{
				parts.year -= 1;
				parts.month = 12;
			}
		}

		clear_date_properties();
		day(0, parts.month, parts.year, parts.target);
	}
	else if (parts.year)
	{
		parts.year += forward ? 1 : -1;
		clear_date_properties();
		year(parts.year, parts.target);
	}
	else if (parts.month)
	{
		if (forward)
		{
			parts.month += 1;

			if (parts.month > 12)
			{
				parts.month = 1;
			}
		}
		else
		{
			parts.month -= 1;

			if (parts.month < 1)
			{
				parts.month = 12;
			}
		}

		clear_date_properties();
		month(parts.month, parts.target);
	}
}

search_presence_mask df::search_t::calc_required_presence() const
{
	search_presence_mask result;

	// Presence masks are a rejection-only optimisation: every exact match must contain every
	// bit returned here. A negated term requires absence rather than presence, and an OR
	// only requires one branch, so neither can safely contribute required bits. Disable
	// the prefilter for the whole expression when OR is present; deriving common bits
	// across nested groups would require evaluating the Boolean expression tree.
	if (std::ranges::any_of(_terms, [](const search_term& term)
	{
		return term.modifiers.logical_op == search_term_modifier_bool::m_or;
	}))
	{
		return result;
	}

	for (const auto& v : _terms)
	{
		if (!v.modifiers.positive) continue;

		switch (v.type)
		{
		case search_term_type::duplicate:
			result.types |= search_presence_mask::duplicates;
			break;
		case search_term_type::remote:
			// Only a coordinate can be remote, so the prefilter may skip everything else.
			result.types |= search_presence_mask::location;
			break;
		case search_term_type::panorama:
			result.types |= search_presence_mask::panorama;
			break;
		case search_term_type::media_type:
			result.types |= v.fg_val->search_presence_bit();
			break;
		case search_term_type::value: break;
		case search_term_type::has_type: break;
		case search_term_type::date:
			result.types |= v.key->search_presence_bit;
			break;
		case search_term_type::empty: break;
		case search_term_type::text: break;
		default:
			break;
		}
	}

	return result;
}

df::date_parts month_and_day(const std::string_view s)
{
	df::date_parts result;
	const auto parts = str::split(
		s, true, [](const char c) { return c == '-' || c == '/' || c == '\\' || c == '.'; });

	if (parts.size() == 2)
	{
		const auto month = str::month(parts[0]);

		if (month != 0 && str::is_num(parts[1]))
		{
			const auto d = str::to_int(parts[1]);

			if (d >= 1 && d <= 31)
			{
				result.month = month;
				result.day = d;
			}
		}
	}

	return result;
}

df::date_parts year_and_month(const std::string_view s)
{
	df::date_parts result;
	const auto parts = str::split(
		s, true, [](const char c) { return c == '-' || c == '/' || c == '\\' || c == '.'; });

	if (parts.size() == 2)
	{
		const auto month = str::month(parts[1]);

		if (month != 0 && str::is_num(parts[0]))
		{
			const auto y = str::to_int(parts[0]);

			if (y > 1900 && y < 2999)
			{
				result.month = month;
				result.year = y;
			}
		}
	}

	return result;
}


namespace
{
	struct place_query
	{
		std::string name;
		double km = 0.0;
	};

	// locations.md 3.1: a trailing <number><unit> component is a radius, with or without a comma.
	// Recognised only when it parses completely, so a place named like a distance still resolves.
	place_query split_place_query(const std::string_view text)
	{
		place_query result;
		result.name.assign(str::trim(text));
		if (result.name.empty()) return result;

		const auto tail = std::string_view(result.name);
		auto unit_start = tail.size();
		while (unit_start > 0 && std::isalpha(static_cast<unsigned char>(tail[unit_start - 1]))) --unit_start;
		if (unit_start == tail.size()) return result;

		const auto unit = tail.substr(unit_start);
		double scale = 0.0;

		if (str::icmp(unit, "km") == 0) scale = 1.0;
		else if (str::icmp(unit, "m") == 0) scale = 0.001;
		else if (str::icmp(unit, "mi") == 0 || str::icmp(unit, "mile") == 0 || str::icmp(unit, "miles") == 0)
			scale = 1.609344;
		else return result;

		auto number_end = unit_start;
		while (number_end > 0 && std::isspace(static_cast<unsigned char>(tail[number_end - 1]))) --number_end;
		auto number_start = number_end;
		while (number_start > 0 &&
			(std::isdigit(static_cast<unsigned char>(tail[number_start - 1])) || tail[number_start - 1] == '.'))
		{
			--number_start;
		}

		if (number_start == number_end || number_start == 0) return result;
		const auto separator = tail[number_start - 1];
		if (separator != ',' && !std::isspace(static_cast<unsigned char>(separator))) return result;

		const auto value = str::to_double(tail.substr(number_start, number_end - number_start));
		if (value <= 0.0) return result;

		result.km = value * scale;
		result.name.erase(number_start);
		while (!result.name.empty() && std::isspace(static_cast<unsigned char>(result.name.back()))) result.name.
			pop_back();
		if (!result.name.empty() && result.name.back() == ',') result.name.pop_back();
		while (!result.name.empty() && std::isspace(static_cast<unsigned char>(result.name.back()))) result.name.
			pop_back();
		return result;
	}

	// locations.md 3.1: "lat, lon" is a coordinate, not a place name. Both parts must be
	// numbers in range, so a place whose name contains digits still resolves by name.
	bool split_coordinate(const std::string_view text, gps_coordinate& coord)
	{
		const auto comma = text.find(',');
		if (comma == std::string_view::npos) return false;

		const auto lat_text = str::trim(text.substr(0, comma));
		const auto lon_text = str::trim(text.substr(comma + 1));
		if (lat_text.empty() || lon_text.empty()) return false;

		const auto is_number = [](const std::string_view v)
		{
			auto digits = 0;
			auto decimal = false;

			for (size_t i = 0; i < v.size(); ++i)
			{
				const auto c = v[i];
				if (std::isdigit(static_cast<unsigned char>(c))) ++digits;
				else if (c == '.' && !decimal) decimal = true;
				else if ((c == '+' || c == '-') && i == 0) continue;
				else return false;
			}

			return digits > 0;
		};

		if (!is_number(lat_text) || !is_number(lon_text)) return false;

		const auto lat = str::to_double(lat_text);
		const auto lon = str::to_double(lon_text);
		if (std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) return false;

		coord = gps_coordinate(lat, lon);
		return coord.is_valid();
	}
}

void df::search_t::parse_part(const search_part& part)
{
	const auto* type = prop::from_prefix(part.scope);

	if (part.scope == "@" && !part.literal)
	{
		const auto* const ft = parse_file_group(part.term);

		if (ft)
		{
			_terms.emplace_back(search_term(ft, part.modifier));
			return;
		}
	}

	if (type == prop::null)
	{
		const auto is_with = str::icmp(part.scope, "with") == 0 || str::icmp(part.scope, tt.query_with) == 0;
		const auto is_without = str::icmp(part.scope, "without") == 0 || str::icmp(part.scope, tt.query_without) == 0;

		if (is_with || is_without)
		{
			// The scope spells the polarity and a leading `-` inverts it. Everything else the tokenizer
			// read - the or/and join and the grouping parentheses - belongs to the term either way, so
			// the modifier is carried rather than rebuilt from the polarity alone.
			auto mods = part.modifier;
			mods.positive = part.modifier.positive == is_with;

			if (str::icmp(part.term, "location") == 0)
			{
				_terms.emplace_back(search_term_type::has_location, mods);
				return;
			}

			const auto* const scope_type = prop::from_prefix(part.term);

			// an unresolvable scope would build a null-key has_type term, which inverts to "match everything"
			if (scope_type != prop::null)
			{
				_terms.emplace_back(search_term(scope_type, mods));
				return;
			}
		}
		if (str::icmp(part.scope, "related") == 0 || str::icmp(part.scope, tt.query_related) == 0)
		{
			_related.path = file_path(part.term);
			return;
		}
	}

	date_t date;
	auto n = str::to_int(part.term);
	auto d = str::to_double(part.term);
	auto is_num = str::is_num(part.term);
	auto is_date = date.parse(part.term) && date.is_valid();
	double d1, d2;
	int n1, n2, n3;
	search_term result;

	if (part.scope == "@")
	{
		// Rebuilt per call rather than held in a function static: the tt.* entries are views into
		// the loaded catalog, and switching language in-session rewrites the buffers behind them.
		const hash_map<std::string_view, search_term_type, ihash, ieq> pre_title_stop_words
		{
			{tt.query_duplicates, search_term_type::duplicate},
			{tt.query_duplicates_alt1, search_term_type::duplicate},
			{tt.query_duplicates_alt2, search_term_type::duplicate},
			{"dups", search_term_type::duplicate},
			{"duplicate", search_term_type::duplicate},
			{sv_duplicates, search_term_type::duplicate},
			{sv_remote, search_term_type::remote},
			{sv_panorama, search_term_type::panorama},
			{"pano", search_term_type::panorama},
		};

		const auto found_flag = pre_title_stop_words.find(part.term);

		if (found_flag != pre_title_stop_words.end())
		{
			result = search_term(found_flag->second, part.modifier);
		}
	}
	else if (const auto* const location = find_location_scope(part.scope); location && location->takes_centre)
	{
		// locations.md 3.5: the guessable spelling of a built-in class is accepted and
		// canonicalizes to the `@` form, so there is only ever one vocabulary to learn.
		if (str::icmp(part.term, sv_remote) == 0)
		{
			result = search_term(search_term_type::remote, part.modifier);
		}
		else
		{
			const auto pq = split_place_query(part.term);
			gps_coordinate coord;

			if (split_coordinate(pq.name, coord))
			{
				result = search_term(search_term_type::location, coord, pq.km, part.modifier);
			}
			else if (const auto loc = split_location(part.term); loc.success)
			{
				result = search_term(search_term_type::location, gps_coordinate(loc.x, loc.y), loc.z, part.modifier);
			}
			else
			{
				result = search_term(search_term_type::location, pq.name, part.modifier);
				result.float_val = pq.km;
			}
		}
	}
	else if (location)
	{
		// These resolve locations, not the raw stored field; `with:place` asks about the field.
		// Only place level takes a radius; a region or country has extent, not a centre.
		const auto is_place = location->level == location_level::place;
		const auto pq = is_place ? split_place_query(part.term) : place_query{std::string(part.term), 0.0};
		result = search_term(search_term_type::location, pq.name, part.modifier);
		result.float_val = pq.km;
		result.level = location->level;
	}
	else if (str::icmp(part.scope, "area") == 0)
	{
		result = search_term(search_term_type::area, part.term, part.modifier);
	}
	else if (str::icmp(part.scope, "ext") == 0 ||
		str::icmp(part.scope, "extension") == 0 ||
		str::icmp(part.scope, "type") == 0)
	{
		result = search_term(search_term_type::extension, part.term, part.modifier);
	}
	else if (str::icmp(part.scope, "volume") == 0 ||
		str::icmp(part.scope, "vol") == 0)
	{
		result = search_term(search_term_type::volume, part.term, part.modifier);
	}
	else if (str::icmp(part.scope, "age") == 0 || str::icmp(part.scope, tt.query_age) == 0)
	{
		date_parts dd;
		dd.age = n;
		result = search_term(search_term_type::date, dd, part.modifier);
	}
	// if no property type try to guess one
	else if (type == prop::null)
	{
		if (_snscanf_s(std::bit_cast<const char*>(part.term.data()), part.term.size(), "%d:%d", &n1, &n2) == 2)
		{
			result = search_term(prop::duration, round(n1 * 60.0 + n2), part.modifier);
		}
		else if (!month_and_day(part.term).is_empty())
		{
			auto dd = month_and_day(part.term);
			result = search_term(search_term_type::date, dd, part.modifier);
		}
		else if (!year_and_month(part.term).is_empty())
		{
			auto dd = year_and_month(part.term);
			result = search_term(search_term_type::date, dd, part.modifier);
		}
		else if (is_date)
		{
			result = search_term(search_term_type::date, date_parts(date.date()), part.modifier);
		}
		else if (is_num && n >= 1 && n <= 5)
		{
			result = search_term(prop::rating, n, part.modifier);
		}
		else if (is_num && n > 1800 && n < 2100)
		{
			date_parts dd;
			dd.year = n;
			result = search_term(search_term_type::date, dd, part.modifier);
		}
		else if (str::month(part.term) != 0)
		{
			date_parts dd;
			dd.month = str::month(part.term);
			result = search_term(search_term_type::date, dd, part.modifier);
		}
		else if (_snscanf_s(std::bit_cast<const char*>(part.term.data()), part.term.size(), "f/%lf", &d1) == 1
			&& d1 > 0.0)
		{
			// Bare "f/N.N" term: match either as text (e.g. lens description "f/3.5-5.6")
			// OR as aperture f-number value. Wrap in a group with OR semantics.
			auto text_mods = part.modifier;
			text_mods.begin_group += 1;
			_terms.emplace_back(search_term(part.term, text_mods));

			search_term_modifier ap_mods = part.modifier;

			// Negated, the two arms have to be joined the other way round: "not either reading" is
			// "neither reading", which is De Morgan's law. Left as OR, a negated pair read as
			// "not the text OR not the aperture" - true of very nearly every photograph, so the
			// exclusion the user asked for removed almost nothing.
			ap_mods.logical_op = part.modifier.positive
				                     ? search_term_modifier_bool::m_or
				                     : search_term_modifier_bool::m_and;
			ap_mods.end_group += 1;
			result = search_term(prop::f_number, d1, ap_mods);
		}
		else
		{
			_terms.emplace_back(search_term(part.term, part.modifier));
		}
	}
	else if (type == prop::tag)
	{
		result = search_term(type, part.term, part.modifier);
	}
	else if (type == prop::created_utc || type == prop::created_exif || type == prop::modified)
	{
		auto target = date_parts_prop::any;
		if (type == prop::modified) target = date_parts_prop::modified;
		if (type == prop::created_utc) target = date_parts_prop::created;
		if (type == prop::created_exif) target = date_parts_prop::original;

		if (is_num)
		{
			if (is_num && n >= 1800 && n <= 2100)
			{
				date_parts dd;
				dd.year = n;
				dd.target = target;
				result = search_term(search_term_type::date, dd, part.modifier);
			}
			else
			{
				date_parts dd;
				dd.age = n;
				dd.target = target;
				result = search_term(search_term_type::date, dd, part.modifier);
			}
		}
		else if (!month_and_day(part.term).is_empty())
		{
			auto dd = month_and_day(part.term);
			dd.target = target;
			result = search_term(search_term_type::date, dd, part.modifier);
		}
		else if (!year_and_month(part.term).is_empty())
		{
			auto dd = year_and_month(part.term);
			dd.target = target;
			result = search_term(search_term_type::date, dd, part.modifier);
		}
		else if (str::month(part.term) != 0)
		{
			date_parts dd;
			dd.month = str::month(part.term);
			dd.target = target;
			result = search_term(search_term_type::date, dd, part.modifier);
		}
		else if (is_date)
		{
			result = search_term(search_term_type::date, date_parts(date.date(), target), part.modifier);
		}
	}
	else if (type == prop::f_number)
	{
		if (_snscanf_s(std::bit_cast<const char*>(part.term.data()), part.term.size(), "f/%lf", &d1) == 1)
		{
			result = search_term(prop::f_number, d1, part.modifier);
		}
		else
		{
			result = search_term(prop::f_number, d, part.modifier);
		}
	}
	else if (type == prop::iso_speed)
	{
		if (_snscanf_s(std::bit_cast<const char*>(part.term.data()), part.term.size(), "ISO%d", &n1) == 1)
		{
			result = search_term(prop::iso_speed, n1, part.modifier);
		}
		else
		{
			result = search_term(prop::iso_speed, n, part.modifier);
		}
	}
	else if (type == prop::exposure_time)
	{
		if (_snscanf_s(std::bit_cast<const char*>(part.term.data()), part.term.size(), "%lf/%lfs", &d1, &d2) == 2)
		{
			result = search_term(prop::exposure_time, d1 / d2, part.modifier);
		}
		else if (_snscanf_s(std::bit_cast<const char*>(part.term.data()), part.term.size(), "%lfs", &d1) == 1)
		{
			result = search_term(prop::exposure_time, d1, part.modifier);
		}
		else
		{
			result = search_term(prop::exposure_time, d, part.modifier);
		}
	}
	else if (type == prop::audio_sample_rate)
	{
		auto rate = n;

		if (str::ends(part.term, "khz"))
		{
			rate = static_cast<int>(d * 1000);
		}

		result = search_term(prop::audio_sample_rate, rate, part.modifier);
	}
	else if (type == prop::audio_sample_type)
	{
		const static hash_map<std::string_view, prop::audio_sample_t, ihash, ieq> audio_sample_types = {
			{"none", prop::audio_sample_t::none},
			{"8bit", prop::audio_sample_t::unsigned_8bit},
			{"16bit", prop::audio_sample_t::signed_16bit},
			{"32bit", prop::audio_sample_t::signed_32bit},
			{"64bit", prop::audio_sample_t::signed_64bit},
			{"float", prop::audio_sample_t::signed_float},
			{"double", prop::audio_sample_t::signed_double},
			{"8bit planar", prop::audio_sample_t::unsigned_planar_8bit},
			{"16bit planar", prop::audio_sample_t::signed_planar_16bit},
			{"32bit planar", prop::audio_sample_t::signed_planar_32bit},
			{"64bit planar", prop::audio_sample_t::signed_planar_64bit},
			{"float planar", prop::audio_sample_t::planar_float},
			{"double planar", prop::audio_sample_t::planar_double}
		};

		const auto found = audio_sample_types.find(part.term);

		if (found != audio_sample_types.end())
		{
			result = search_term(prop::audio_sample_type, static_cast<int>(found->second), part.modifier);
		}
		else
		{
			result = search_term(prop::audio_sample_type, n, part.modifier);
		}
	}
	else if (type == prop::audio_channels)
	{
		const static hash_map<std::string_view, int, ihash, ieq> audio_channels = {
			{"mono", 1},
			{"stereo", 2},
			{"3.0 surround", 3},
			{"quad", 4},
			{"5.0 surround", 5},
			{"5.1 surround", 6},
			{"7.1 surround", 8}
		};

		const auto found = audio_channels.find(part.term);

		if (found != audio_channels.end())
		{
			result = search_term(prop::audio_channels, found->second, part.modifier);
		}
		else
		{
			result = search_term(prop::audio_channels, n, part.modifier);
		}
	}
	else if (type == prop::duration)
	{
		auto mods = part.modifier;
		auto duration = 0;

		// H:MM:SS first: "%d:%d" also matches the prefix of "1:02:03"
		if (_snscanf_s(std::bit_cast<const char*>(part.term.data()), part.term.size(), "%d:%d:%d", &n1, &n2,
		               &n3) == 3)
		{
			duration = n1 * 60 * 60 + n2 * 60 + n3;
		}
		else if (_snscanf_s(std::bit_cast<const char*>(part.term.data()), part.term.size(), "%d:%d", &n1, &n2) == 2)
		{
			duration = n1 * 60 + n2;
		}
		else
		{
			if (str::ends(part.term, "m"))
			{
				duration = n * 60;
			}
			else if (str::ends(part.term, "h"))
			{
				duration = n * 60 * 60;
			}
			else if (str::ends(part.term, "d"))
			{
				duration = n * 60 * 60 * 24;
			}
			else
			{
				duration = n;
			}
		}

		result = search_term(prop::duration, duration, mods);
	}
	else if (type == prop::file_size)
	{
		// Scaled from the double, not the int: a bare byte count above two billion overflowed the
		// int and matched small files instead. Clamped rather than wrapped at the top of the range.
		const auto scaled = [d](const double multiplier) -> uint64_t
		{
			const auto bytes = d * multiplier;
			if (!std::isfinite(bytes) || bytes <= 0.0) return 0;

			constexpr auto limit = static_cast<double>(std::numeric_limits<uint64_t>::max());
			// std::round, not df::round: that one answers an int32 and turned every size at or above
			// two gigabytes into a negative number, which then widened to an enormous unsigned one.
			return bytes >= limit ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(std::round(bytes));
		};

		constexpr auto kb = 1024.0;
		constexpr auto mb = kb * 1024.0;
		constexpr auto gb = mb * 1024.0;
		constexpr auto tb = gb * 1024.0;

		uint64_t size = 0;

		if (str::ends(part.term, "tb")) size = scaled(tb);
		else if (str::ends(part.term, "gb")) size = scaled(gb);
		else if (str::ends(part.term, "mb")) size = scaled(mb);
		else if (str::ends(part.term, "kb")) size = scaled(kb);
		else size = scaled(1.0);

		result = search_term(type, size, part.modifier);
	}
	else
	{
		switch (type->data_type)
		{
		case prop::data_type::int32:
			result = search_term(type, n, part.modifier);
			break;
		case prop::data_type::date:
			result = search_term(type, date.to_days(), part.modifier);
			break;
		case prop::data_type::float32:
			result = search_term(type, d, part.modifier);
			break;
		case prop::data_type::size:
			result = search_term(type, static_cast<uint64_t>(n), part.modifier);
			break;
		case prop::data_type::string:
			result = search_term(type, part.term, part.modifier);
			break;
		case prop::data_type::int_pair:
			{
				// "N" or "N/M" as format_xy writes a track or a disc, and "WxH" as
				// format_dimensions writes a size. Splitting on '/' alone read "1920x1080" as a bare
				// 1920, and compare_term treats a zero second component as "any" - so a saved
				// dimensions query came back matching every image of that width whatever its
				// height, which is broader than what the user asked for and than what was shown.
				const auto sep = part.term.find_first_of("/xX");
				const auto x = static_cast<int16_t>(str::to_int(part.term.substr(0, sep)));
				const auto y = sep == std::string_view::npos
					               ? int16_t{0}
					               : static_cast<int16_t>(str::to_int(part.term.substr(sep + 1)));
				result = search_term(type, xy16::make(x, y), part.modifier);
			}
			break;
		case prop::data_type::uint32:
			result = search_term(type, n, part.modifier);
			break;
		}
	}

	if (!result.is_empty())
	{
		_terms.emplace_back(result);
	}
}

void df::related_info::load(const item_element_ptr& i)
{
	const auto md = i->metadata();

	path = i->path();
	name = i->name();
	size = i->file_size();
	file_created = i->file_created();
	ft = i->file_type();
	crc32c = i->crc32c();
	const auto duplicate_info = i->duplicates();
	group = duplicate_info.group;
	duplicate_grade = duplicate_info.grade;
	duplicate_crowded = duplicate_info.same_picture_crowded;

	if (md)
	{
		gps = md->coordinate;
		metadata_created = md->created();
		dimensions = md->dimensions();
		album = md->album;
		album_artist = md->album_artist;
		show = md->show;
		season = md->season;
		episode = md->episode;
		disk = md->disk;
		track = md->track;
	}

	is_loaded = true;
}

bool df::search_t::needs_metadata() const
{
	// Every relation but the duplicate rules is read from indexed metadata, so a related search over
	// a folder selector has to have that metadata scanned before it can answer.
	if (has_related())
	{
		return true;
	}

	for (const auto& t : _terms)
	{
		if (t.needs_metadata())
		{
			return true;
		}
	}

	return false;
}

// locations.md 3.3: name resolution happens once per search, on the search worker,
// never once per item.
void df::search_matcher::resolve_location_centres()
{
	for (const auto& t : _search.terms())
	{
		if (t.type != search_term_type::location || t.coord_val.is_valid() || t.text.empty())
		{
			continue;
		}

		if (_resolved_centres.contains(t.text)) continue;

		const auto resolved = _locations->find_by_name(t.text);

		if (resolved.position.is_valid())
		{
			// A region or country name has extent rather than a centre, so it is resolved for the
			// radius rule but never granted a reach of its own.
			const auto reach = str::is_empty(resolved.place) || resolved.is_extent()
				                   ? 0.0
				                   : location_attribution_radius_km(resolved.population);
			_resolved_centres[t.text] = {resolved.position, reach};
		}
	}
}

// locations.md 2.5: bounded attribution, memoized per ~1 km cell so a folder shot on one trip
// costs a single gazetteer read rather than one per photo.
df::search_matcher::attributed_location df::search_matcher::attributed(const gps_coordinate coord) const
{
	const attribution_cell cell(coord);
	const auto found = _attributed.find(cell);

	if (found != _attributed.end())
	{
		return found->second;
	}

	country_loc country;
	const auto resolved = _locations->find_attributed(coord, &country);

	attributed_location result;
	result.place = resolved.place.place;
	result.state = resolved.place.state;
	result.country = str::is_empty(country.name) ? resolved.place.country : country.name;
	result.attribution = resolved.attribution;
	result.reach_km = resolved.attribution == location_attribution::at ||
	                  resolved.attribution == location_attribution::near
		                  ? location_attribution_radius_km(resolved.place.population)
		                  : 0.0;

	if (_locations->is_index_loaded())
	{
		_attributed.emplace(cell, result);
	}

	return result;
}

bool df::search_matcher::can_contain(const search_presence_mask& available_presence) const
{
	return available_presence.contains_required(_required_presence);
}

struct compare_result
{
	bool match = false;
	int com = 0;
	str::cached val_matched = {};
};

static bool modifier_match(const compare_result cmp, const df::search_term_modifier& modifiers)
{
	if (modifiers.equals && cmp.com == 0 && cmp.match)
	{
		return true;
	}

	if (modifiers.greater_than && cmp.com < 0 && cmp.match)
	{
		return true;
	}

	if (modifiers.less_than && cmp.com > 0 && cmp.match)
	{
		return true;
	}

	if (!modifiers.greater_than && !modifiers.less_than && cmp.match)
	{
		return cmp.com == 0;
	}

	return false;
}


static compare_result compare_term(const df::search_term& term, const int rr)
{
	const auto ll = term.int_val;
	const auto res = ll < rr ? -1 : ll > rr ? 1 : 0;
	return {true, res};
}


static compare_result compare_file_size(const df::search_term& term, const uint64_t r)
{
	const auto ll = df::round_size(term.int64_val).total();
	const auto rr = df::round_size(r).total();

	const auto res = ll < rr ? -1 : ll > rr ? 1 : 0;
	return {true, res};
}

static compare_result compare_term(const df::search_term& term, const df::date_t r)
{
	if (!r.is_valid()) return {};

	const auto ll = static_cast<int>(term.int_val);
	const auto rr = static_cast<int>(r.to_days());
	const auto res = ll < rr ? -1 : ll > rr ? 1 : 0;
	return {true, res};
}

// Returns text unchanged when it is ASCII (already Unicode NFC); otherwise
// NFC-normalises it into 'buf' and returns a view of that. This lets Korean (and
// other) text that differs only in normalization form (precomposed NFC vs
// decomposed NFD jamo) match during search. The ASCII path is allocation-free, so
// only non-ASCII comparisons pay the cost. NOTE: applied only to search text
// matching - never to file-path comparisons, which must stay byte-exact.
static std::string_view nfc_view(const std::string_view text, std::string& buf)
{
	if (str::is_ascii(text)) return text;
	buf = platform::normalize_nfc(text);
	return buf;
}

// The query side was normalised once when the term was built.
static std::string_view nfc_query(const df::search_term& term)
{
	return term._nfc_text.empty() ? std::string_view(term.text) : std::string_view(term._nfc_text);
}

static compare_result compare_term(const df::search_term& term, const str::cached r)
{
	std::string rb;
	const auto q = nfc_query(term);
	const auto rr = nfc_view(r, rb);

	if (term._is_wildcard)
	{
		const auto cmp = str::wildcard_icmp(rr, q);
		return {cmp, 0, r};
	}

	const auto cmp = str::icmp(q, rr);
	return {cmp == 0, 0, r};
}

static compare_result compare_term(const df::search_term& term, const std::string_view r)
{
	std::string rb;
	const auto q = nfc_query(term);
	const auto rr = nfc_view(r, rb);
	const auto match = term._is_wildcard ? str::wildcard_icmp(rr, q) : str::icmp(q, rr) == 0;
	return {match, 0, match ? str::cache(r) : str::cached{}};
}

static compare_result compare_aperture(const df::search_term& term, const double f_number)
{
	auto ll = 0.0;
	auto rr = 0.0;

	if (term.key == prop::f_number)
	{
		ll = prop::closest_fstop(term.float_val);
	}

	if (f_number != 0.0)
	{
		rr = prop::closest_fstop(f_number);
	}

	if (df::equiv(ll, rr)) return {true, 0};
	const auto res = ll < rr ? -1 : ll > rr ? 1 : 0;
	return {true, res};
}

static double mp_round(const double mp)
{
	if (mp < 2.0)
	{
		return std::round(mp * 10.0) / 10.0;
	}

	return std::round(mp);
}

static compare_result compare_megapixels(const df::search_term& term, const int w, const int h)
{
	const auto ll = mp_round(term.float_val);
	const auto rr = mp_round(ui::calc_mega_pixels(w, h));

	if (df::equiv(ll, rr)) return {true, 0};
	const auto res = ll < rr ? -1 : ll > rr ? 1 : 0;
	return {true, res};
}

static compare_result compare_term(const df::search_term& term, const double rr)
{
	const auto ll = term.float_val;
	const auto res = ll < rr ? -1 : ll > rr ? 1 : 0;
	return {true, res};
}

static compare_result compare_float32_term(const df::search_term& term, const float rr)
{
	const auto ll = static_cast<float>(term.float_val);
	const auto res = ll < rr ? -1 : ll > rr ? 1 : 0;
	return {true, res};
}

static compare_result compare_exposure_time(const df::search_term& term, const double r)
{
	const auto query = static_cast<float>(term.float_val);
	const double ll = query < 1.0f ? -1.0 / query : query;
	const double rr = r < 1.0 ? -1.0 / r : r;

	if (df::equiv(ll, rr, 0.00001)) return {true, 0};
	const auto res = ll > rr ? -1 : ll < rr ? 1 : 0;
	return {true, res};
}

static compare_result compare_term(const df::search_term& term, const df::xy8 r)
{
	const auto l = term.xy_val;

	int res = 0;
	if (l.x < r.x) res = -1;
	else if (l.x > r.x) res = 1;
	else if (l.y == 0) res = 0; // a bare "track:3" matches 3 of any total
	else if (l.y < r.y) res = -1;
	else if (l.y > r.y) res = 1;
	return {true, res};
}

static compare_result compare_term(const df::search_term& term, const df::xy16 r)
{
	const auto l = term.xy_val;

	int res = 0;
	if (l.x < r.x) res = -1;
	else if (l.x > r.x) res = 1;
	else if (l.y == 0) res = 0; // a bare "track:3" matches 3 of any total
	else if (l.y < r.y) res = -1;
	else if (l.y > r.y) res = 1;
	return {true, res};
}

inline bool contains_term(const std::string_view text, const df::search_term& term)
{
	std::string tb;
	const auto q = nfc_query(term);
	const auto t = nfc_view(text, tb);

	if (term._is_wildcard)
	{
		return str::wildcard_icmp(t, q);
	}

	return str::contains(t, q);
}

inline bool same_term(const std::string_view text, const df::search_term& term)
{
	std::string tb;
	const auto q = nfc_query(term);
	const auto t = nfc_view(text, tb);

	if (term._is_wildcard)
	{
		return str::wildcard_icmp(t, q);
	}

	return str::same(t, q);
}

df::search_result compare_text(const df::search_term& term, const df::index_file_item& file)
{
	if (contains_term(file.name, term)) return {df::search_result_type::match_prop, prop::file_name};
	if (same_term(df::format_size(file.size), term)) return {df::search_result_type::match_prop, prop::file_size};

	const auto md = file.metadata.load();

	if (md)
	{
		if (contains_term(md->album, term)) return {df::search_result_type::match_prop, prop::album};
		if (contains_term(md->audio_codec, term)) return {df::search_result_type::match_prop, prop::audio_codec};
		if (contains_term(md->bitrate, term)) return {df::search_result_type::match_prop, prop::bitrate};
		if (contains_term(md->camera_manufacturer, term))
			return {
				df::search_result_type::match_prop, prop::camera_manufacturer
			};
		if (contains_term(md->camera_model, term)) return {df::search_result_type::match_prop, prop::camera_model};
		if (contains_term(md->comment, term)) return {df::search_result_type::match_prop, prop::comment};
		if (contains_term(md->composer, term)) return {df::search_result_type::match_prop, prop::composer};
		if (contains_term(md->copyright_creator, term))
			return {
				df::search_result_type::match_prop, prop::copyright_creator
			};
		if (contains_term(md->copyright_credit, term))
			return {
				df::search_result_type::match_prop, prop::copyright_credit
			};
		if (contains_term(md->copyright_notice, term))
			return {
				df::search_result_type::match_prop, prop::copyright_notice
			};
		if (contains_term(md->copyright_source, term))
			return {
				df::search_result_type::match_prop, prop::copyright_source
			};
		if (contains_term(md->copyright_url, term)) return {df::search_result_type::match_prop, prop::copyright_url};
		if (contains_term(md->description, term)) return {df::search_result_type::match_prop, prop::description};
		if (contains_term(md->encoder, term)) return {df::search_result_type::match_prop, prop::encoder};
		if (contains_term(md->file_name, term)) return {df::search_result_type::match_prop, prop::file_name};
		if (contains_term(md->genre, term)) return {df::search_result_type::match_prop, prop::genre};
		if (contains_term(md->lens, term)) return {df::search_result_type::match_prop, prop::lens};
		if (contains_term(md->location_place, term)) return {df::search_result_type::match_prop, prop::location_place};
		if (contains_term(md->location_country, term))
			return {
				df::search_result_type::match_prop, prop::location_country
			};
		if (contains_term(md->location_state, term)) return {df::search_result_type::match_prop, prop::location_state};
		if (contains_term(md->performer, term)) return {df::search_result_type::match_prop, prop::performer};
		if (contains_term(md->pixel_format, term)) return {df::search_result_type::match_prop, prop::pixel_format};
		if (contains_term(md->publisher, term)) return {df::search_result_type::match_prop, prop::publisher};
		if (contains_term(md->show, term)) return {df::search_result_type::match_prop, prop::show};
		if (contains_term(md->synopsis, term)) return {df::search_result_type::match_prop, prop::synopsis};
		if (contains_term(md->title, term)) return {df::search_result_type::match_prop, prop::title};
		if (contains_term(md->label, term)) return {df::search_result_type::match_prop, prop::label};
		if (contains_term(md->video_codec, term)) return {df::search_result_type::match_prop, prop::video_codec};
		if (contains_term(md->raw_file_name, term)) return {df::search_result_type::match_prop, prop::raw_file_name};

		if (same_term(prop::format_dimensions(md->dimensions()), term))
			return {
				df::search_result_type::match_prop, prop::dimensions
			};
		if (same_term(prop::format_pixels(md->dimensions(), file.ft), term))
			return {
				df::search_result_type::match_prop, prop::dimensions
			};
		if (same_term(prop::format_exposure(md->exposure_time), term))
			return {
				df::search_result_type::match_prop, prop::exposure_time
			};
		if (same_term(prop::format_f_num(md->f_number), term))
			return {
				df::search_result_type::match_prop, prop::f_number
			};
		if (same_term(prop::format_focal_length(md->focal_length, md->focal_length_35mm_equivalent), term))
			return {
				df::search_result_type::match_prop, prop::focal_length
			};

		if (same_term(prop::format_duration(md->duration), term))
			return {
				df::search_result_type::match_prop, prop::duration
			};
		if (same_term(prop::format_iso(md->iso_speed), term))
			return {
				df::search_result_type::match_prop, prop::iso_speed
			};
		if (same_term(prop::format_audio_channels(md->audio_channels), term))
			return {
				df::search_result_type::match_prop, prop::audio_channels
			};
		if (same_term(prop::format_audio_sample_rate(md->audio_sample_rate), term))
			return {
				df::search_result_type::match_prop, prop::audio_sample_rate
			};
		if (same_term(format_audio_sample_type(static_cast<prop::audio_sample_t>(md->audio_sample_type)), term))
			return
				{df::search_result_type::match_prop, prop::audio_sample_type};
		if (same_term(str::to_string(md->year), term)) return {df::search_result_type::match_prop, prop::year};

		compare_result comp_result;
		prop::key_ref key = prop::null;

		auto cmp = [&comp_result, &term](const std::string_view part)
		{
			if (!comp_result.match)
			{
				comp_result = compare_term(term, part);
			}
		};

		if (!is_empty(md->tags)) split2(md->tags, true, cmp);

		if (comp_result.match)
		{
			key = prop::tag;
		}
		else
		{
			if (!is_empty(md->artist)) split2(md->artist, true, cmp, str::is_artist_separator);

			if (comp_result.match)
			{
				key = prop::artist;
			}
			else
			{
				if (!is_empty(md->album_artist)) split2(md->album_artist, true, cmp, str::is_artist_separator);

				if (comp_result.match)
				{
					key = prop::album_artist;
				}
			}
		}

		if (comp_result.match)
		{
			return {df::search_result_type::match_prop, key, comp_result.val_matched};
		}
	}

	return {df::search_result_type::no_match};
}

static compare_result compare_val(const df::search_term& term, const df::index_file_item& file)
{
	auto&& t = term.key;

	if (t == prop::modified && !prop::is_null(file.file_modified)) return compare_term(term, file.file_modified);
	if (t == prop::file_size && !file.size.is_empty()) return compare_file_size(term, file.size.to_int64());

	const auto md = file.metadata.load();

	if (md)
	{
		if (term.key == prop::tag || term.key == prop::artist || term.key == prop::album_artist ||
			term.key == prop::genre)
		{
			compare_result comp_result;
			auto cmp = [&comp_result, &term](const std::string_view part)
			{
				if (!comp_result.match)
				{
					comp_result = compare_term(term, str::trim(part));
				}
			};

			if (term.key == prop::tag) split2(md->tags, true, cmp);
			else if (term.key == prop::artist) split2(md->artist, true, cmp, str::is_artist_separator);
			else if (term.key == prop::album_artist) split2(md->album_artist, true, cmp, str::is_artist_separator);
			else if (term.key == prop::genre) split2(md->genre, true, cmp, str::is_genre_separator);

			return comp_result;
		}

		if (t == prop::title && !prop::is_null(md->title)) return compare_term(term, md->title);
		if (t == prop::description && !prop::is_null(md->description)) return compare_term(term, md->description);
		if (t == prop::comment && !prop::is_null(md->comment)) return compare_term(term, md->comment);
		if (t == prop::synopsis && !prop::is_null(md->synopsis)) return compare_term(term, md->synopsis);
		if (t == prop::composer && !prop::is_null(md->composer)) return compare_term(term, md->composer);
		if (t == prop::encoder && !prop::is_null(md->encoder)) return compare_term(term, md->encoder);
		if (t == prop::publisher && !prop::is_null(md->publisher)) return compare_term(term, md->publisher);
		if (t == prop::performer && !prop::is_null(md->performer)) return compare_term(term, md->performer);
		if (t == prop::copyright_credit && !prop::is_null(md->copyright_credit))
			return compare_term(
				term, md->copyright_credit);
		if (t == prop::copyright_notice && !prop::is_null(md->copyright_notice))
			return compare_term(
				term, md->copyright_notice);
		if (t == prop::copyright_creator && !prop::is_null(md->copyright_creator))
			return compare_term(
				term, md->copyright_creator);
		if (t == prop::copyright_source && !prop::is_null(md->copyright_source))
			return compare_term(
				term, md->copyright_source);
		if (t == prop::copyright_url && !prop::is_null(md->copyright_url)) return compare_term(term, md->copyright_url);
		if (t == prop::file_name && !prop::is_null(md->file_name)) return compare_term(term, md->file_name);
		if (t == prop::raw_file_name && !prop::is_null(md->raw_file_name)) return compare_term(term, md->raw_file_name);
		if (t == prop::pixel_format && !prop::is_null(md->pixel_format)) return compare_term(term, md->pixel_format);
		if (t == prop::bitrate && !prop::is_null(md->bitrate)) return compare_term(term, md->bitrate);
		if (t == prop::orientation) return compare_term(term, static_cast<int>(md->orientation));
		if (t == prop::dimensions) return compare_term(term, df::xy16::make(md->width, md->height));
		if (t == prop::megapixels) return compare_megapixels(term, md->width, md->height);
		if (t == prop::year && !prop::is_null(md->year)) return compare_term(term, md->year);
		if (t == prop::rating && !prop::is_null(md->rating)) return compare_term(term, md->rating);
		if (t == prop::season && !prop::is_null(md->season)) return compare_term(term, md->season);
		if (t == prop::episode && !prop::is_null(md->episode)) return compare_term(term, md->episode);
		if (t == prop::disk_num && !prop::is_null(md->disk)) return compare_term(term, md->disk);
		if (t == prop::track_num && !prop::is_null(md->track)) return compare_term(term, md->track);
		if (t == prop::duration && !prop::is_null(md->duration)) return compare_term(term, md->duration);
		if (t == prop::created_utc && !prop::is_null(md->dates.created()))
			return compare_term(term, md->dates.created());
		if (t == prop::created_exif && !prop::is_null(md->dates.original()))
			return compare_term(term, md->dates.original());
		if (t == prop::exposure_time && !prop::is_null(md->exposure_time))
			return compare_exposure_time(
				term, md->exposure_time);
		if (t == prop::f_number && !prop::is_null(md->f_number)) return compare_aperture(term, md->f_number);
		if (t == prop::focal_length && !prop::is_null(md->focal_length)) return compare_float32_term(term, md->focal_length);
		if (t == prop::focal_length_35mm_equivalent && !prop::is_null(md->focal_length_35mm_equivalent))
			return
				compare_term(term, md->focal_length_35mm_equivalent);
		if (t == prop::iso_speed && !prop::is_null(md->iso_speed)) return compare_term(term, md->iso_speed);
		if (t == prop::latitude && md->coordinate.is_valid())
			return compare_term(
				term, md->coordinate.latitude());
		if (t == prop::longitude && md->coordinate.is_valid())
			return compare_term(
				term, md->coordinate.longitude());
		if (t == prop::location_country && !prop::is_null(md->location_country))
			return compare_term(
				term, md->location_country);
		if (t == prop::location_state && !prop::is_null(md->location_state))
			return compare_term(
				term, md->location_state);
		if (t == prop::location_place && !prop::is_null(md->location_place))
			return compare_term(
				term, md->location_place);
		if (t == prop::camera_manufacturer && !prop::is_null(md->camera_manufacturer))
			return compare_term(
				term, md->camera_manufacturer);
		if (t == prop::camera_model && !prop::is_null(md->camera_model)) return compare_term(term, md->camera_model);
		if (t == prop::lens && !prop::is_null(md->lens)) return compare_term(term, md->lens);
		if (t == prop::video_codec && !prop::is_null(md->video_codec)) return compare_term(term, md->video_codec);
		if (t == prop::audio_sample_type && !prop::is_null(md->audio_sample_type))
			return compare_term(
				term, md->audio_sample_type);
		if (t == prop::audio_sample_rate && !prop::is_null(md->audio_sample_rate))
			return compare_term(
				term, static_cast<int>(md->audio_sample_rate));
		if (t == prop::audio_channels && !prop::is_null(md->audio_channels))
			return compare_term(
				term, md->audio_channels);
		if (t == prop::audio_codec && !prop::is_null(md->audio_codec)) return compare_term(term, md->audio_codec);
		if (t == prop::album_artist && !prop::is_null(md->album_artist)) return compare_term(term, md->album_artist);
		if (t == prop::artist && !prop::is_null(md->artist)) return compare_term(term, md->artist);
		if (t == prop::album && !prop::is_null(md->album)) return compare_term(term, md->album);
		if (t == prop::show && !prop::is_null(md->show)) return compare_term(term, md->show);
		if (t == prop::game && !prop::is_null(md->game)) return compare_term(term, md->game);
		if (t == prop::system && !prop::is_null(md->system)) return compare_term(term, md->system);
		if (t == prop::label && !prop::is_null(md->label)) return compare_term(term, md->label);
		if (t == prop::doc_id && !prop::is_null(md->doc_id)) return compare_term(term, md->doc_id);
	}

	return {};
}

bool has_type(const prop::key_ref t, const df::index_file_item& file)
{
	if (t == prop::modified) return !prop::is_null(file.file_modified);
	if (t == prop::file_size) return !file.size.is_empty();

	const auto md = file.metadata.load();

	if (md)
	{
		if (t == prop::title) return !prop::is_null(md->title);
		if (t == prop::description) return !prop::is_null(md->description);
		if (t == prop::comment) return !prop::is_null(md->comment);
		if (t == prop::synopsis) return !prop::is_null(md->synopsis);
		if (t == prop::composer) return !prop::is_null(md->composer);
		if (t == prop::encoder) return !prop::is_null(md->encoder);
		if (t == prop::publisher) return !prop::is_null(md->publisher);
		if (t == prop::performer) return !prop::is_null(md->performer);
		if (t == prop::genre) return !prop::is_null(md->genre);
		if (t == prop::copyright_credit) return !prop::is_null(md->copyright_credit);
		if (t == prop::copyright_notice) return !prop::is_null(md->copyright_notice);
		if (t == prop::copyright_creator) return !prop::is_null(md->copyright_creator);
		if (t == prop::copyright_source) return !prop::is_null(md->copyright_source);
		if (t == prop::copyright_url) return !prop::is_null(md->copyright_url);
		if (t == prop::file_name) return !prop::is_null(md->file_name);
		if (t == prop::raw_file_name) return !prop::is_null(md->raw_file_name);
		if (t == prop::pixel_format) return !prop::is_null(md->pixel_format);
		if (t == prop::bitrate) return !prop::is_null(md->bitrate);
		// top_left is the default and none means nothing was recorded, so neither is an orientation
		// anyone asked for. This is the same test the packer and the presence bit apply.
		if (t == prop::orientation)
			return md->orientation != ui::orientation::top_left && md->orientation != ui::orientation::none;
		if (t == prop::dimensions) return !prop::is_null(md->width);
		if (t == prop::megapixels) return !prop::is_null(md->width);
		if (t == prop::year) return !prop::is_null(md->year);
		if (t == prop::rating) return !prop::is_null(md->rating);
		if (t == prop::season) return !prop::is_null(md->season);
		if (t == prop::episode) return !prop::is_null(md->episode);
		if (t == prop::disk_num) return !prop::is_null(md->disk);
		if (t == prop::track_num) return !prop::is_null(md->track);
		if (t == prop::duration) return !prop::is_null(md->duration);
		if (t == prop::created_utc) return !prop::is_null(md->dates.created());
		if (t == prop::created_exif) return !prop::is_null(md->dates.original());
		if (t == prop::exposure_time) return !prop::is_null(md->exposure_time);
		if (t == prop::f_number) return !prop::is_null(md->f_number);
		if (t == prop::focal_length) return !prop::is_null(md->focal_length);
		if (t == prop::focal_length_35mm_equivalent) return !prop::is_null(md->focal_length_35mm_equivalent);
		if (t == prop::iso_speed) return !prop::is_null(md->iso_speed);
		if (t == prop::latitude) return md->coordinate.is_valid();
		if (t == prop::longitude) return md->coordinate.is_valid();
		if (t == prop::location_country) return !prop::is_null(md->location_country);
		if (t == prop::location_state) return !prop::is_null(md->location_state);
		if (t == prop::location_place) return !prop::is_null(md->location_place);
		if (t == prop::camera_manufacturer) return !prop::is_null(md->camera_manufacturer);
		if (t == prop::camera_model) return !prop::is_null(md->camera_model);
		if (t == prop::lens) return !prop::is_null(md->lens);
		if (t == prop::video_codec) return !prop::is_null(md->video_codec);
		if (t == prop::audio_sample_rate) return !prop::is_null(md->audio_sample_rate);
		if (t == prop::audio_sample_type) return !prop::is_null(md->audio_sample_type);
		if (t == prop::audio_channels) return md->audio_channels != 0;
		if (t == prop::audio_codec) return !prop::is_null(md->audio_codec);
		if (t == prop::altitude) return md->altitude != 0.0f;
		if (t == prop::gps_speed) return md->gps_speed != 0.0f;
		if (t == prop::doc_id) return !prop::is_null(md->doc_id);
		if (t == prop::album_artist) return !prop::is_null(md->album_artist);
		if (t == prop::artist) return !prop::is_null(md->artist);
		if (t == prop::album) return !prop::is_null(md->album);
		if (t == prop::show) return !prop::is_null(md->show);
		if (t == prop::game) return !prop::is_null(md->game);
		if (t == prop::system) return !prop::is_null(md->system);
		if (t == prop::tag) return !prop::is_null(md->tags);
		if (t == prop::label) return !prop::is_null(md->label);
	}

	return false;
}

static bool is_date_match(const df::date_parts& term, const df::date_t d, const df::search_term_modifier& modifiers,
                          const uint32_t now_days)
{
	if (!d.is_valid())
		return false;

	if (term.age != 0)
	{
		const auto then = d.to_days();
		const auto age = static_cast<int>(now_days) - static_cast<int>(then);
		return age <= term.age;
	}

	const auto parts = d.date();
	int tt = 0;
	int vv = 0;

	if (term.year != 0)
	{
		tt += term.year * 12 * 31;
		vv += parts.year * 12 * 31;
	}

	if (term.month != 0)
	{
		tt += term.month * 31;
		vv += parts.month * 31;
	}

	if (term.day != 0)
	{
		tt += term.day;
		vv += parts.day;
	}

	const auto cc = tt - vv;
	const auto can_match = tt != 0 && vv != 0;

	if (can_match)
	{
		if (modifiers.equals && cc == 0)
		{
			return true;
		}

		if (modifiers.greater_than && cc < 0)
		{
			return true;
		}

		if (modifiers.less_than && cc > 0)
		{
			return true;
		}

		if (!modifiers.greater_than && !modifiers.less_than)
		{
			return cc == 0;
		}
	}

	return false;
}

static bool is_date_match(const df::search_term& term, const df::index_file_item& file, const uint32_t now_days)
{
	const bool is_any = term.date_val.target == df::date_parts_prop::any;
	const auto target = term.date_val.target;

	if (target == df::date_parts_prop::original || target == df::date_parts_prop::created || is_any)
	{
		const auto md = file.metadata.load();

		// A header is only clickable if the matcher answers the key it was drawn from, so each key
		// is reproduced here whole - mirroring one rung of one of them is #184 again on whichever
		// order the missing rung belongs to. The stamp is converted the way the group key converts
		// it; a UTC stamp against a local key misses by a day.
		const auto file_stamp = file.file_created.system_to_local();
		auto original_key = df::date_t::null;

		if (target == df::date_parts_prop::original || is_any)
		{
			const auto resolved = md ? md->created() : df::date_t::null;
			original_key = resolved.is_valid() ? resolved : file_stamp;

			if (original_key.is_valid() &&
				is_date_match(term.date_val, original_key, term.modifiers, now_days))
			{
				return true;
			}
		}

		if (target == df::date_parts_prop::created || is_any)
		{
			const auto created = md ? md->dates.created() : df::date_t::null;
			const auto created_key = created.is_valid() ? created : file_stamp;

			// On the `any` path the two keys usually agree, and the first has already answered.
			if (created_key.is_valid() && created_key != original_key &&
				is_date_match(term.date_val, created_key, term.modifiers, now_days))
			{
				return true;
			}
		}
	}

	if (target == df::date_parts_prop::modified || is_any)
	{
		// Same rule as the created axis: Group by Modified keys on the local stamp, so the term has
		// to compare against the local stamp or the header it drew answers with nothing.
		if (is_date_match(term.date_val, file.file_modified.load().system_to_local(), term.modifiers, now_days))
		{
			return true;
		}
	}

	return false;
}


bool df::search_t::is_match(const prop::key& key, const date_t date) const
{
	auto date_term_count = 0;
	auto date_term_match = 0;

	for (const auto& term : _terms)
	{
		if (term.is_date())
		{
			static constexpr search_term_modifier mod;
			const auto now_days = platform::now().to_days();

			date_term_count += 1;

			if (term.date_val.target == date_parts_prop::original && key == prop::created_exif)
			{
				if (is_date_match(term.date_val, date, mod, now_days)) ++date_term_match;
			}
			else if (term.date_val.target == date_parts_prop::created && key == prop::created_utc)
			{
				if (is_date_match(term.date_val, date, mod, now_days)) ++date_term_match;
			}
			else if (term.date_val.target == date_parts_prop::modified &&
				key == prop::modified)
			{
				if (is_date_match(term.date_val, date, mod, now_days)) ++date_term_match;
			}
			else if (term.date_val.target == date_parts_prop::any)
			{
				if (is_date_match(term.date_val, date, mod, now_days)) ++date_term_match;
			}
		}
	}

	return date_term_match > 0 && date_term_match >= date_term_count;
}

bool df::search_t::is_match(const prop::key& key, const int val) const
{
	auto term_count = 0;
	auto term_match = 0;

	for (const auto& term : _terms)
	{
		if (term.is_int())
		{
			term_count += 1;

			if (term.key == key)
			{
				const auto cmp = compare_term(term, val);
				const bool match_value = modifier_match(cmp, term.modifiers);
				if (match_value == term.modifiers.positive) ++term_match;
			}
		}
	}

	return term_match > 0 && term_match >= term_count;
}


static bool eq_ext(std::string_view ext1, std::string_view ext2)
{
	if (!ext1.empty() && ext1[0] == '.') ext1 = ext1.substr(1);
	if (!ext2.empty() && ext2[0] == '.') ext2 = ext2.substr(1);
	return str::icmp(ext1, ext2) == 0;
}

// locations.md 2.3/3.5: a completion commits the gazetteer's qualified name, which drops the region
// for a place qualified to its country, so "London, United Kingdom" has to match an item the index
// knows as London / England / United Kingdom. Every part must name one of the three fields, which
// is what still keeps "London, Ontario" off a London, England item.
static bool matches_qualified_name(const std::string_view query, const str::cached place, const str::cached state,
                                   const str::cached country)
{
	if (query.empty()) return false;

	const auto names_a_field = [place, state, country](const std::string_view part)
	{
		return (!str::is_empty(place) && str::icmp(part, place.sv()) == 0) ||
			(!str::is_empty(state) && str::icmp(part, state.sv()) == 0) ||
			(!str::is_empty(country) && str::icmp(part, country.sv()) == 0);
	};

	size_t pos = 0;

	while (true)
	{
		const auto comma = query.find(',', pos);
		const auto end = comma == std::string_view::npos ? query.size() : comma;
		const auto part = str::trim(query.substr(pos, end - pos));

		if (part.empty() || !names_a_field(part)) return false;
		if (comma == std::string_view::npos) return true;
		pos = comma + 1;
	}
}

bool df::match_volume_label(const std::string_view folder_name, const hash_map<char, str::cached>& drive_labels,
                            const search_term& term)
{
	if (folder_name.size() >= 2 && folder_name[1] == ':')
	{
		const auto drive_letter = static_cast<char>(str::to_upper(static_cast<unsigned char>(folder_name[0])));
		const auto found = drive_labels.find(drive_letter);

		if (found != drive_labels.end())
		{
			return same_term(found->second.sv(), term);
		}
	}

	return false;
}

df::search_result df::search_matcher::match_term(const str::cached folder_name, const index_file_item& file,
                                                 const search_term& term) const
{
	search_result result;

	if (term.type == search_term_type::media_type)
	{
		const bool match_file_group = term.fg_val == file.ft->group;

		if (match_file_group == term.modifiers.positive)
		{
			result.type = search_result_type::match_file_group;
		}
	}
	else if (term.type == search_term_type::extension)
	{
		const auto name = file.name.sv();
		const auto extension_pos = find_ext(name);
		const auto ext = name.substr(extension_pos);
		const bool match_ext = eq_ext(term.text, ext);

		if (match_ext == term.modifiers.positive)
		{
			result.type = search_result_type::match_ext;
		}
	}
	else if (term.type == search_term_type::volume)
	{
		const bool match_vol = match_volume_label(folder_name.sv(), _drive_labels, term);

		if (match_vol == term.modifiers.positive)
		{
			result.type = search_result_type::match_volume;
		}
	}
	else if (term.type == search_term_type::area)
	{
		const auto md = file.metadata.load();
		const auto match_area = term.location_cell_span > 0 && md && md->has_gps() && map_location_area{
			.cell = term.location_cell, .cell_span = term.location_cell_span
		}.contains(location_heat_map::calc_map_loc(md->coordinate));

		if (match_area == term.modifiers.positive)
		{
			result.type = search_result_type::match_location;
		}
	}
	else if (term.type == search_term_type::has_location)
	{
		// locations.md 3.6: no location can be determined at all, which is distinct
		// from `without:place` asking whether the stored field is populated.
		const auto md = file.metadata.load();
		const bool has_location = md && (md->coordinate.is_valid() ||
			!str::is_empty(md->location_place) ||
			!str::is_empty(md->location_state) ||
			!str::is_empty(md->location_country));

		if (has_location == term.modifiers.positive)
		{
			result.type = search_result_type::match_location;
		}
	}
	else if (term.type == search_term_type::location)
	{
		const auto md = file.metadata.load();
		auto match_location = false;
		if (term.coord_val.is_valid())
		{
			// The radius is inclusive, so a chip may promise exactly the distance of its furthest
			// item without that item falling outside the search the chip runs.
			match_location = md && md->has_gps() &&
				term.coord_val.distance_in_kilometers(md->coordinate) <= term.float_val;
		}
		else if (term.float_val > 0.0)
		{
			// Radius match: an item without coordinates can never satisfy one, even when its
			// stored text names the place (locations.md 3.2).
			const auto centre = resolved_centre(term.text);
			match_location = centre.is_valid() && md && md->has_gps() &&
				centre.distance_in_kilometers(md->coordinate) <= term.float_val;
		}
		else if (md)
		{
			// Stored text wins; the gazetteer only fills gaps, and only when the item has coordinates.
			auto place = md->location_place;
			auto state = md->location_state;
			auto country_name = md->location_country;

			// locations.md 2.5: bounded attribution, so a mid-ocean item is never matched
			// by the name of a city it is nowhere near. Remote still yields its country.
			if (_locations && md->coordinate.is_valid() &&
				(str::is_empty(place) || str::is_empty(state) || str::is_empty(country_name)))
			{
				const auto resolved = attributed(md->coordinate);

				if (str::is_empty(place)) place = resolved.place;
				if (str::is_empty(state)) state = resolved.state;
				if (str::is_empty(country_name)) country_name = resolved.country;
			}

			const auto equals = [&term](const str::cached value)
			{
				return !str::is_empty(value) && str::icmp(term.text, value.sv()) == 0;
			};

			switch (term.level)
			{
			case location_level::place:
				match_location = equals(place);
				break;
			case location_level::state:
				match_location = equals(state);
				break;
			case location_level::country:
				match_location = equals(country_name);
				break;
			default:
				match_location = matches_qualified_name(term.text, place, state, country_name);
				break;
			}

			// locations.md 2.5/3.2: inside a city the nearest record is often an unpopulated street, so
			// the derived name alone would never answer `loc:London`. A more significant place also
			// answers for anything its lesser namer stands within -- and no further, so a town 30 km
			// away keeps its own identity.
			if (!match_location && _locations && md->has_gps() && term.level != location_level::state &&
				term.level != location_level::country)
			{
				const auto reach = resolved_reach_km(term.text);
				const auto centre = resolved_centre(term.text);
				const auto named_by = attributed(md->coordinate);

				match_location = reach > named_by.reach_km && named_by.reach_km > 0.0 && centre.is_valid() &&
					centre.distance_in_kilometers(md->coordinate) <= named_by.reach_km;
			}
		}


		if (match_location == term.modifiers.positive)
		{
			result.type = search_result_type::match_location;
		}
	}
	else if (term.type == search_term_type::duplicate)
	{
		const bool has_dups = file.duplicates.load().count > 1;
		if (has_dups == term.modifiers.positive)
		{
			result.type = search_result_type::similar;
		}
	}
	else if (term.type == search_term_type::remote)
	{
		// locations.md 2.5 step 5: nothing significant enough is close enough to name. A stored
		// place name is the user's own answer, so an item that carries one is never remote.
		const auto md = file.metadata.load();
		const bool is_remote = _locations && md && md->coordinate.is_valid() &&
			str::is_empty(md->location_place) &&
			str::is_empty(md->location_state) &&
			attributed(md->coordinate).attribution == location_attribution::remote;

		if (is_remote == term.modifiers.positive)
		{
			result.type = search_result_type::match_location;
		}
	}
	else if (term.type == search_term_type::panorama)
	{
		// The file's own declaration. An image merely shaped like a panorama does not match, so a
		// wide crop of a landscape is not swept in with the photo spheres.
		const auto md = file.metadata.load();
		const bool is_panorama = md && md->is_panorama();

		if (is_panorama == term.modifiers.positive)
		{
			result.type = search_result_type::match_flag;
		}
	}
	else if (term.type == search_term_type::text)
	{
		const auto match_folder = can_match_folder &&
			str::contains(term.text, "**") &&
			str::wildcard_icmp(folder_name.sv(), term.text);

		// special case - exclusions can match folder name
		if (!term.modifiers.positive)
		{
			if (match_folder || contains_term(folder_name, term)) return result;
		}

		if (term.modifiers.positive && match_folder)
		{
			result.type = search_result_type::match_folder;
			result.text = folder_name;
		}
		else
		{
			const auto match_text = compare_text(term, file);

			if (match_text.is_match() && term.modifiers.positive)
			{
				// positive match
				result = match_text;
			}
			else if (!match_text.is_match() && !term.modifiers.positive)
			{
				// negative match
				result.type = search_result_type::match_text;
			}
		}
	}
	else if (term.is_date())
	{
		const bool match_date = is_date_match(term, file, _now_days);

		if (match_date == term.modifiers.positive)
		{
			result.type = search_result_type::match_date;
		}
	}
	else if (term.type == search_term_type::has_type)
	{
		if (file.ft != file_type::folder)
		{
			const bool match_has_type = has_type(term.key, file);

			if (match_has_type == term.modifiers.positive)
			{
				result.type = search_result_type::has_type;
			}
		}
	}
	else if (term.type == search_term_type::value)
	{
		const auto cmp = compare_val(term, file);
		const bool match_value = modifier_match(cmp, term.modifiers);

		if (match_value == term.modifiers.positive)
		{
			result.type = search_result_type::match_prop;
			result.key = term.key;
			result.text = cmp.val_matched;
		}
	}

	return result;
}

// The axes are tried in priority order, so an item that qualifies several ways is reported once
// under its strongest relation and appears in exactly one group.
std::optional<df::related_match> df::search_matcher::evaluate_related(const file_path path,
                                                                     const index_file_item& file) const
{
	const auto& related = _search.related();

	// The item the search started at is part of its own answer, and must never displace a relation:
	// it sorts ahead of every match, so a full axis can never be the reason it disappears.
	if (path == related.path)
	{
		return related_match{related_axis::duplicate, -1};
	}

	const auto dup_rank = dup_match_rank(related, file);

	if (dup_rank >= 0)
	{
		return related_match{related_axis::duplicate, dup_rank};
	}

	const auto md = file.metadata.load();

	if (!md)
	{
		return {};
	}

	if (!str::is_empty(related.album) && icmp(md->album, related.album) == 0)
	{
		// Two artists can both have a "Greatest Hits", so a named artist on both sides has to agree.
		const auto artist_agrees = str::is_empty(related.album_artist) || str::is_empty(md->album_artist) ||
			icmp(md->album_artist, related.album_artist) == 0;

		if (artist_agrees)
		{
			const auto ordinal = static_cast<int64_t>(md->disk.x) * 1000 + md->track.x;
			return related_match{related_axis::album, std::abs(ordinal - related.track_ordinal())};
		}
	}

	if (!str::is_empty(related.show) && icmp(md->show, related.show) == 0)
	{
		const auto ordinal = static_cast<int64_t>(md->season) * 1000 + md->episode.x;
		return related_match{related_axis::series, std::abs(ordinal - related.episode_ordinal())};
	}

	// Capture time, not file time: a collection copied in one pass shares a file time, which would
	// make every item in it equally and meaninglessly close.
	const auto created = md->created();

	if (related.metadata_created.is_valid() && created.is_valid())
	{
		const auto delta = std::abs(created - related.metadata_created) /
			static_cast<int64_t>(date_t::intervals_per_second);

		if (delta <= related_time_window_seconds)
		{
			return related_match{related_axis::time, delta};
		}
	}

	if (related.gps.is_valid() && md->coordinate.is_valid())
	{
		const auto km = related.gps.distance_in_kilometers(md->coordinate);

		if (km <= related_location_window_km)
		{
			return related_match{related_axis::location, static_cast<int64_t>(km * 1000.0)};
		}
	}

	return {};
}

df::search_result df::search_matcher::match_item(const file_path path, const index_file_item& file) const
{
	if (has_related)
	{
		const auto related = evaluate_related(path, file);

		if (!related)
		{
			return {};
		}

		// Any other terms still narrow a related search, and the relation has to survive them:
		// match_all_terms answers with its own result type and would otherwise lose the axis.
		if (!_search._terms.empty() &&
			(!can_contain(file.search_presence) || !match_all_terms(path.folder().text(), file).is_match()))
		{
			return {};
		}

		search_result result;
		result.type = related_result_type(related->axis);
		result.distance = static_cast<int32_t>(related->distance);
		return result;
	}

	search_result result;

	if (_search.has_selector() && _search._terms.empty())
	{
		result.type = search_result_type::match_folder;
	}

	if (_search._terms.empty())
	{
		return result;
	}

	if (!can_contain(file.search_presence))
	{
		return {search_result_type::no_match};
	}

	return match_all_terms(path.folder().text(), file);
}

df::search_result df::search_matcher::match_all_terms(const str::cached folder_name, const index_file_item& file) const
{
	if (_search._terms.size() == 1)
	{
		// optimisation for single term
		return match_term(folder_name, file, _search._terms[0]);
	}

	struct level
	{
		bool logical_and = true;
		bool state = true;
		// A level starts with no value rather than with an operator's identity. `true` is the
		// identity for AND but the annihilator for OR, so an OR level seeded with it stays true
		// whatever it later receives - which is what made a nested group after OR match everything.
		bool has_value = false;
	};

	constexpr auto max_levels = 32;
	level level_results[max_levels];
	auto current_level = 0;

	const auto contribute = [](level& target, const bool value, const bool logical_and)
	{
		if (!target.has_value)
		{
			target.state = value;
			target.has_value = true;
		}
		else if (logical_and)
		{
			target.state &= value;
		}
		else
		{
			target.state |= value;
		}
	};

	const auto fold_level = [&contribute](level* levels, int& depth)
	{
		if (depth > 0)
		{
			contribute(levels[depth - 1], levels[depth].state, levels[depth].logical_and);
			depth -= 1;
		}
	};

	for (const auto& term : _search._terms)
	{
		const auto is_match = match_term(folder_name, file, term).is_match();

		auto opened = 0;

		for (auto i = 0; i < term.modifiers.begin_group; i++)
		{
			if (current_level < max_levels - 1)
			{
				current_level += 1;
				level_results[current_level].logical_and = term.modifiers.logical_op !=
					search_term_modifier_bool::m_or;
				level_results[current_level].state = true;
				level_results[current_level].has_value = false;
				opened += 1;
			}
		}

		// combine into whichever level the term now sits in, so a '(' beyond max_levels still
		// contributes its match rather than being discarded
		contribute(level_results[current_level], is_match,
		           opened > 0 || term.modifiers.logical_op != search_term_modifier_bool::m_or);

		for (auto i = 0; i < term.modifiers.end_group; i++)
		{
			fold_level(level_results, current_level);
		}
	}

	// an unclosed '(' must still resolve into level 0; otherwise level 0 keeps its initial
	// 'true' and the query matches every item
	while (current_level > 0)
	{
		fold_level(level_results, current_level);
	}

	return {level_results[0].state ? search_result_type::match_multiple : search_result_type::no_match};
}

df::search_result df::search_matcher::match_folder(const str::cached folder_name, const str::cached name) const
{
	if (has_related)
	{
		return {};
	}

	index_file_item file;
	file.name = name;
	file.ft = file_type::folder;

	return match_all_terms(folder_name, file);
}
