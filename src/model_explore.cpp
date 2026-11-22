// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Builds what the Explore view shows from a detached copy of the collection - the timeline's
// days, weeks, months and years, the trips found in it and the places they were to, and the globe's
// clusters - and the searches each part opens, made from the predicates that counted it. Also names
// a map area and makes its search, for the sidebar globe and Explore's alike.

#include "pch.h"

#include "model_explore.h"
#include "model_index.h"
#include "model_items.h"
#include "model_locations.h"
#include "files.h"

void df::explore_representative::consider(const core::file_path& candidate, const uint8_t candidate_rank)
{
	if (should_replace_thumbnail_representative(candidate, candidate_rank, path, rank))
	{
		path = candidate;
		rank = candidate_rank;
	}
}

uint32_t df::explore_day(const int year, const int month, const int day)
{
	return core::date_t(year, month, day).to_days();
}

core::day_t df::explore_date(const uint32_t day)
{
	return core::date_t::from_days(day).date();
}

std::optional<df::explore_sample> df::explore_sample_of(const index_file_item& file, core::file_path path)
{
	if (!file.ft) return {};

	const auto md = file.metadata.load();

	// The key a date search's `original` target compares, so a day's count is what a search of that day lists.
	const auto taken = date_term_keys_of(date_parts_prop::original, file, md.get()).original;

	explore_sample result;
	result.day = taken.is_valid() ? taken.to_days() : 0;
	result.kind = file.ft->group == &file_group::photo   ? explore_kind::photo
	              : file.ft->group == &file_group::video ? explore_kind::video
	                                                     : explore_kind::other;
	result.rank = thumbnail_representative_rank(file, md.get());
	result.path = path;

	if (md)
	{
		result.coordinate = md->coordinate;
		result.place = md->location_place;
		result.state = md->location_state;
		result.country = md->location_country;
	}

	return result;
}

bool df::explore_place::same_place(const explore_place& other) const
{
	return level == other.level && core::icmp(state.sv(), other.state.sv()) == 0 &&
	       core::icmp(country.sv(), other.country.sv()) == 0;
}

df::explore_counts df::explore_model::span_counts(const uint32_t first, const uint32_t last) const
{
	explore_counts result;

	for (auto day = std::max(first, first_day); day <= std::min(last, today); ++day)
	{
		result += days[day - first_day];
	}

	return result;
}

std::pair<size_t, size_t> df::explore_model::located_between(const uint32_t first, const uint32_t last) const
{
	const auto begin = std::ranges::lower_bound(located, first, {}, &explore_located::day);
	const auto end = std::ranges::upper_bound(located, last, {}, &explore_located::day);
	return {static_cast<size_t>(begin - located.begin()), static_cast<size_t>(std::max(begin, end) - located.begin())};
}

namespace
{
	// The names places are compared by, as the location search compares them: without regard to case.
	class place_names
	{
		std::vector<core::cached>& _names;
		core::hash_map<std::string_view, uint32_t, core::ihash, core::ieq> _ids;

	public:
		explicit place_names(std::vector<core::cached>& names) : _names(names) {}

		uint32_t id(const core::cached name)
		{
			if (core::is_empty(name)) return df::explore_no_place;

			const auto found = _ids.find(name.sv());
			if (found != _ids.end()) return found->second;

			const auto id = static_cast<uint32_t>(_names.size());
			_names.push_back(name);
			_ids.emplace(name.sv(), id);
			return id;
		}
	};

	// Where a sample is, as a `state:` or `country:` term finds it: the file's own text, and the gazetteer's
	// answer for its coordinate where the text says nothing. One answer stands for a whole attribution cell,
	// as it does for the search.
	struct sample_place
	{
		uint32_t state = df::explore_no_place;
		uint32_t country = df::explore_no_place;
	};

	struct attributed_cell
	{
		core::cached state;
		core::cached country;
		std::string country_name;
	};

	struct day_place
	{
		uint32_t day = 0;
		uint32_t state = df::explore_no_place;
		uint32_t country = df::explore_no_place;
	};

	// What a day is, for finding trips: where the most of its located items were taken.
	struct day_label
	{
		df::location_level level = df::location_level::any; // any: home, or nothing known
		uint32_t state = df::explore_no_place;
		uint32_t country = df::explore_no_place;

		bool is_away() const { return level != df::location_level::any; }

		bool operator==(const day_label&) const = default;
	};

	template <typename Key> Key most_counted(const core::hash_map<Key, uint32_t>& counts, const Key none)
	{
		auto best = none;
		uint32_t best_count = 0;

		for (const auto& [key, count] : counts)
		{
			if (count > best_count || (count == best_count && key < best))
			{
				best = key;
				best_count = count;
			}
		}

		return best;
	}

	// Each year's home: the places most days were spent in the years around it, so a move abroad moves home
	// with it and a long trip does not.
	core::hash_set<uint32_t> home_of(const std::map<int, core::hash_map<uint32_t, uint32_t>>& days_by_year,
	                                 const int year)
	{
		core::hash_map<uint32_t, uint32_t> window;
		uint32_t total = 0;

		for (auto y = year - 1; y <= year + 1; ++y)
		{
			const auto found = days_by_year.find(y);
			if (found == days_by_year.end()) continue;

			for (const auto& [id, days] : found->second)
			{
				window[id] += days;
				total += days;
			}
		}

		core::hash_set<uint32_t> result;
		if (total == 0) return result;

		result.insert(most_counted(window, df::explore_no_place));

		for (const auto& [id, days] : window)
		{
			if (days >= total * df::explore_home_share) result.insert(id);
		}

		return result;
	}

	// The nearest year either side that has a home, for a year with nothing located in it.
	core::hash_set<uint32_t> home_near(const std::map<int, core::hash_map<uint32_t, uint32_t>>& days_by_year,
	                                   const int year)
	{
		for (auto distance = 0; distance < df::max_history_years; ++distance)
		{
			for (const auto y : {year - distance, year + distance})
			{
				auto home = home_of(days_by_year, y);
				if (!home.empty()) return home;
			}
		}

		return {};
	}

	void add_unit_vector(double& x, double& y, double& z, const gps_coordinate coordinate)
	{
		const auto latitude = gps_coordinate::deg2rad(coordinate.latitude());
		const auto longitude = gps_coordinate::deg2rad(coordinate.longitude());
		x += std::cos(latitude) * std::sin(longitude);
		y += std::sin(latitude);
		z += std::cos(latitude) * std::cos(longitude);
	}

	// The mean of points on the sphere, which has no seam at the antimeridian as a mean of degrees does.
	gps_coordinate unit_vector_centre(const double x, const double y, const double z, const gps_coordinate fallback)
	{
		const auto length = std::sqrt(x * x + y * y + z * z);
		if (length < 1e-9) return fallback;

		auto longitude = gps_coordinate::rad2deg(std::atan2(x, z));
		if (longitude >= 180.0) longitude = std::nextafter(180.0, 0.0);
		if (longitude <= -180.0) longitude = std::nextafter(-180.0, 0.0);
		return {gps_coordinate::rad2deg(std::asin(std::clamp(y / length, -1.0, 1.0))), longitude};
	}
}

df::explore_model df::build_explore_model(explore_request request, const location_cache* const locations)
{
	explore_model model;
	auto& samples = request.samples;

	// Oldest first, and for the same day in path order, so every pass below meets them the same way.
	std::ranges::sort(samples, [](const explore_sample& l, const explore_sample& r)
	{ return l.day != r.day ? l.day < r.day : l.path < r.path; });

	auto today = request.today;
	if (!samples.empty()) today = std::max(today, samples.back().day);

	const auto today_date = explore_date(today);
	const auto current_year = today_date.year;

	// The years the sidebar calendar offers, so a clock that once reset to 1900 does not stretch the
	// timeline across a century of nothing; and of those only from the first that holds something.
	history_range offered;

	if (request.start_year > 0)
	{
		offered = history_range_from_start_year(request.start_year, current_year);
	}
	else
	{
		const auto histogram = std::make_unique<date_histogram>();

		for (const auto& s : samples)
		{
			if (s.day == 0) continue;
			const auto taken = explore_date(s.day);
			const auto offset = current_year - taken.year;
			if (offset >= 0 && offset < max_history_years) histogram->dates[offset * 12 + taken.month - 1].created += 1;
		}

		offered = history_auto_range(*histogram, current_year);
	}

	auto first_year = current_year;

	for (const auto& s : samples)
	{
		if (s.day != 0 && explore_date(s.day).year >= offered.first_year)
		{
			first_year = std::min(current_year, explore_date(s.day).year);
			break;
		}
	}

	model.first_year = first_year;
	model.last_year = current_year;
	model.first_day = explore_day(first_year, 1, 1);
	model.today = today;
	model.days.resize(today - model.first_day + 1);

	const auto in_range = [&model](const uint32_t day)
	{
		return day >= model.first_day && day <= model.today;
	};

	for (const auto& s : samples)
	{
		if (!in_range(s.day))
		{
			++model.outside;
			continue;
		}

		model.days[s.day - model.first_day].add(s.kind);
		model.total.add(s.kind);
	}

	// Years, months and weeks, newest first. Each day belongs to exactly one of each, which is what lets one
	// pass over the samples pick their pictures.
	std::vector<uint32_t> month_of_day(model.days.size());
	std::vector<uint32_t> week_of_day(model.days.size());

	for (auto year = current_year; year >= first_year; --year)
	{
		const auto year_first = explore_day(year, 1, 1);
		const auto year_last = std::min(today, explore_day(year, 12, 31));
		model.years.push_back({year, year_first, year_last, model.span_counts(year_first, year_last)});

		const auto last_month = year == current_year ? today_date.month : 12;

		for (auto month = last_month; month >= 1; --month)
		{
			const auto month_first = explore_day(year, month, 1);
			const auto month_last = std::min(today, month == 12 ? year_last : explore_day(year, month + 1, 1) - 1);
			const auto index = static_cast<uint32_t>(model.months.size());

			for (auto day = month_first; day <= month_last; ++day) month_of_day[day - model.first_day] = index;
			model.months.push_back(
				{year, month, month_first, month_last, model.span_counts(month_first, month_last), {}});
		}

		// Day numbers count from a Monday, so a week starts at a multiple of seven.
		for (auto week_last = year_last; week_last >= year_first;)
		{
			const auto week_first = std::max(year_first, week_last - week_last % 7);
			const auto index = static_cast<uint32_t>(model.weeks.size());

			for (auto day = week_first; day <= week_last; ++day) week_of_day[day - model.first_day] = index;
			model.weeks.push_back({week_first, week_last, model.span_counts(week_first, week_last), {}});

			if (week_first == 0) break;
			week_last = week_first - 1;
		}
	}

	for (const auto& s : samples)
	{
		if (!in_range(s.day)) continue;
		model.months[month_of_day[s.day - model.first_day]].representative.consider(s.path, s.rank);
		model.weeks[week_of_day[s.day - model.first_day]].representative.consider(s.path, s.rank);
	}

	// Where each sample is.
	place_names names(model.places);
	std::vector<sample_place> places(samples.size());
	std::map<attribution_cell, attributed_cell> attributed;
	core::hash_map<uint32_t, std::string> country_names;

	for (size_t i = 0; i < samples.size(); ++i)
	{
		const auto& s = samples[i];
		auto state = s.state;
		auto country = s.country;
		std::string country_name(s.country.sv());

		if (locations && s.coordinate.is_valid() &&
		    (core::is_empty(s.place) || core::is_empty(state) || core::is_empty(country)))
		{
			const attribution_cell cell(s.coordinate);
			auto found = attributed.find(cell);

			if (found == attributed.end())
			{
				country_loc canonical;
				const auto resolved = locations->find_attributed(s.coordinate, &canonical);

				attributed_cell answer;
				answer.state = resolved.place.state;
				answer.country = core::is_empty(canonical.name) ? resolved.place.country : canonical.name;
				answer.country_name = core::is_empty(resolved.place.country) ? std::string(answer.country.sv())
				                                                             : std::string(resolved.place.country.sv());
				found = attributed.emplace(cell, std::move(answer)).first;
			}

			if (core::is_empty(state)) state = found->second.state;

			if (core::is_empty(country))
			{
				country = found->second.country;
				country_name = found->second.country_name;
			}
		}

		places[i] = {names.id(state), names.id(country)};
		if (places[i].country != explore_no_place) country_names.try_emplace(places[i].country, country_name);
	}

	// Each day's place: the country most of its items were taken in, and the state of that country most were.
	std::vector<day_label> labels;
	std::map<int, core::hash_map<uint32_t, uint32_t>> country_days;
	std::map<int, core::hash_map<uint32_t, uint32_t>> state_days;
	std::vector<uint32_t> labelled_days;

	for (size_t i = 0; i < samples.size();)
	{
		const auto day = samples[i].day;
		core::hash_map<uint32_t, uint32_t> countries;
		auto j = i;

		for (; j < samples.size() && samples[j].day == day; ++j)
		{
			if (places[j].country != explore_no_place) ++countries[places[j].country];
		}

		if (in_range(day) && !countries.empty())
		{
			day_label label;
			label.country = most_counted(countries, explore_no_place);

			core::hash_map<uint32_t, uint32_t> states;
			for (auto k = i; k < j; ++k)
			{
				if (places[k].country == label.country && places[k].state != explore_no_place)
					++states[places[k].state];
			}

			label.state = most_counted(states, explore_no_place);

			const auto year = explore_date(day).year;
			++country_days[year][label.country];
			if (label.state != explore_no_place) ++state_days[year][label.state];

			labels.push_back(label);
			labelled_days.push_back(day);
		}

		i = j;
	}

	// Away from home: a country that is not home's, or a state of home's country that is not home's state.
	std::map<int, std::pair<core::hash_set<uint32_t>, core::hash_set<uint32_t>>> homes;

	for (size_t i = 0; i < labels.size(); ++i)
	{
		const auto year = explore_date(labelled_days[i]).year;
		auto found = homes.find(year);

		if (found == homes.end())
		{
			found = homes.emplace(year, std::make_pair(home_near(country_days, year), home_near(state_days, year)))
			            .first;
		}

		const auto& [home_countries, home_states] = found->second;
		auto& label = labels[i];

		if (!home_countries.contains(label.country))
		{
			label.level = location_level::country;
			label.state = explore_no_place;
		}
		else if (label.state != explore_no_place && !home_states.contains(label.state))
		{
			label.level = location_level::state;
		}
		else
		{
			label = {};
		}
	}

	const auto matches = [&places](const size_t sample, const day_label& label)
	{
		return places[sample].country == label.country &&
		       (label.level != location_level::state || places[sample].state == label.state);
	};

	const auto place_of = [&model, &country_names](const day_label& label)
	{
		explore_place place;
		place.level = label.level;
		place.country = model.places[label.country];

		if (label.level == location_level::state)
		{
			place.state = model.places[label.state];
			place.name = std::string(place.state.sv());
		}
		else
		{
			const auto found = country_names.find(label.country);
			place.name = found != country_names.end() && !found->second.empty() ? found->second
			                                                                    : std::string(place.country.sv());
		}

		return place;
	};

	// Runs of days with one away label, broken by a day at home, by another place, or by too long without
	// anything located. A trip counts what its search lists: every item in its days that is of its place.
	const auto close_trip = [&](const day_label& label, const uint32_t first, const uint32_t last)
	{
		explore_trip trip;
		trip.place = place_of(label);
		trip.first_day = first;
		trip.last_day = last;

		double x = 0.0, y = 0.0, z = 0.0;
		const auto begin = std::ranges::lower_bound(samples, first, {}, &explore_sample::day) - samples.begin();

		for (auto i = static_cast<size_t>(begin); i < samples.size() && samples[i].day <= last; ++i)
		{
			if (!matches(i, label)) continue;

			++trip.count;
			trip.representative.consider(samples[i].path, samples[i].rank);
			if (samples[i].coordinate.is_valid()) add_unit_vector(x, y, z, samples[i].coordinate);
		}

		trip.centre = unit_vector_centre(x, y, z, {});
		if (trip.count >= explore_trip_min_items) model.trips.push_back(std::move(trip));
	};

	day_label current;
	uint32_t trip_first = 0;
	uint32_t trip_last = 0;

	for (size_t i = 0; i < labels.size(); ++i)
	{
		const auto& label = labels[i];
		const auto day = labelled_days[i];
		const auto continues = current.is_away() && label == current &&
		                       day - trip_last <= explore_trip_max_gap_days + 1;

		if (continues)
		{
			trip_last = day;
			continue;
		}

		if (current.is_away()) close_trip(current, trip_first, trip_last);

		current = label;
		trip_first = trip_last = day;
	}

	if (current.is_away()) close_trip(current, trip_first, trip_last);

	std::ranges::reverse(model.trips);

	// Each year's places away from home, counted as `country:` or `state:` with the year lists them.
	std::map<int, std::vector<explore_year_place>> by_year;

	for (const auto& trip : model.trips)
	{
		for (auto year = explore_date(trip.first_day).year; year <= explore_date(trip.last_day).year; ++year)
		{
			auto& year_places = by_year[year];
			if (std::ranges::any_of(year_places,
			                        [&](const explore_year_place& p) { return p.place.same_place(trip.place); }))
				continue;

			explore_year_place year_place;
			year_place.year = year;
			year_place.place = trip.place;
			year_places.push_back(std::move(year_place));
		}
	}

	for (auto& [year, year_places] : by_year)
	{
		const auto year_first = explore_day(year, 1, 1);
		const auto year_last = explore_day(year, 12, 31);
		const auto begin = std::ranges::lower_bound(samples, year_first, {}, &explore_sample::day) - samples.begin();

		for (auto i = static_cast<size_t>(begin); i < samples.size() && samples[i].day <= year_last; ++i)
		{
			for (auto& year_place : year_places)
			{
				const auto& p = year_place.place;
				const auto state_matches = p.level != location_level::state ||
				                           (places[i].state != explore_no_place &&
				                            core::icmp(model.places[places[i].state].sv(), p.state.sv()) == 0);

				if (places[i].country != explore_no_place && state_matches &&
				    core::icmp(model.places[places[i].country].sv(), p.country.sv()) == 0)
				{
					++year_place.count;
				}
			}
		}

		std::ranges::stable_sort(year_places, std::ranges::greater{}, &explore_year_place::count);
		if (year_places.size() > explore_max_places_per_year) year_places.resize(explore_max_places_per_year);
	}

	for (auto it = by_year.rbegin(); it != by_year.rend(); ++it)
	{
		for (auto& year_place : it->second) model.year_places.push_back(std::move(year_place));
	}

	// Every located item, for the globe.
	for (size_t i = 0; i < samples.size(); ++i)
	{
		const auto& s = samples[i];
		if (!s.coordinate.is_valid()) continue;
		model.located.push_back({s.day, s.coordinate, s.rank, s.path, places[i].country, places[i].state});
	}

	return model;
}

int df::explore_cluster_level(const double globe_radius_px)
{
	const auto level = std::log2(std::max(1.0, M_PI * globe_radius_px / explore_cluster_px));
	return std::clamp(static_cast<int>(std::lround(level)), 0, explore_max_cluster_level);
}

df::explore_clusters df::cluster_explore_model(const explore_model& model, const int level)
{
	explore_clusters result;
	result.level = std::clamp(level, 0, explore_max_cluster_level);
	result.of_located.resize(model.located.size());

	// Bands of latitude cell-high, each cut into as many cells as keep them square on the sphere, so a cell
	// covers about the same ground everywhere rather than shrinking toward the poles.
	const auto cell_degrees = 180.0 / static_cast<double>(int64_t{1} << result.level);
	const auto bands = int64_t{1} << result.level;

	const auto cell_of = [cell_degrees, bands](const double latitude, const double longitude)
	{
		const auto band = std::clamp(static_cast<int64_t>((std::clamp(latitude, -90.0, 90.0) + 90.0) / cell_degrees),
		                             int64_t{0}, bands - 1);
		const auto band_latitude = -90.0 + (static_cast<double>(band) + 0.5) * cell_degrees;
		const auto columns = std::max<int64_t>(1,
		                                       std::llround(360.0 * std::cos(gps_coordinate::deg2rad(band_latitude)) /
		                                                    cell_degrees));
		auto wrapped = std::fmod(longitude + 180.0, 360.0);
		if (wrapped < 0.0) wrapped += 360.0;
		const auto column = std::clamp(static_cast<int64_t>(wrapped / 360.0 * static_cast<double>(columns)), int64_t{0},
		                               columns - 1);
		return static_cast<uint64_t>(band) << 32 | static_cast<uint64_t>(column);
	};

	struct sums
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	std::vector<explore_cluster> cells;
	std::vector<sums> centres;
	core::hash_map<uint64_t, uint32_t> cell_index;
	std::vector<uint32_t> cell_of_located(model.located.size());

	for (size_t i = 0; i < model.located.size(); ++i)
	{
		const auto& located = model.located[i];
		const auto key = cell_of(located.coordinate.latitude(), located.coordinate.longitude());
		auto [found, added] = cell_index.try_emplace(key, static_cast<uint32_t>(cells.size()));

		if (added)
		{
			explore_cluster cluster;
			cluster.min_latitude = cluster.max_latitude = located.coordinate.latitude();
			cluster.min_longitude = cluster.max_longitude = located.coordinate.longitude();
			cells.push_back(cluster);
			centres.emplace_back();
		}

		const auto index = found->second;
		auto& cluster = cells[index];
		cluster.count += 1;
		cluster.min_latitude = std::min(cluster.min_latitude, located.coordinate.latitude());
		cluster.max_latitude = std::max(cluster.max_latitude, located.coordinate.latitude());
		cluster.min_longitude = std::min(cluster.min_longitude, located.coordinate.longitude());
		cluster.max_longitude = std::max(cluster.max_longitude, located.coordinate.longitude());
		cluster.representative.consider(located.path, located.rank);
		add_unit_vector(centres[index].x, centres[index].y, centres[index].z, located.coordinate);
		cell_of_located[i] = index;
	}

	for (size_t i = 0; i < cells.size(); ++i)
	{
		cells[i].centre = unit_vector_centre(centres[i].x, centres[i].y, centres[i].z,
		                                     gps_coordinate(cells[i].min_latitude, cells[i].min_longitude));
	}

	// A cell edge can fall between two places a marker apart, which would draw two markers over each other. The
	// largest cells take in any neighbour whose centre is nearer than most of a cell, so what is drawn apart is apart.
	std::vector<uint32_t> order(cells.size());
	std::iota(order.begin(), order.end(), 0u);
	std::ranges::stable_sort(order, std::ranges::greater{}, [&cells](const uint32_t i) { return cells[i].count; });

	constexpr auto no_cluster = std::numeric_limits<uint32_t>::max();
	const auto merge_km = gps_coordinate::deg2rad(cell_degrees) * 0.75 * 6371.0;
	std::vector<uint32_t> merged_into(cells.size(), no_cluster);

	for (const auto i : order)
	{
		if (merged_into[i] != no_cluster) continue;
		merged_into[i] = i;

		const auto centre = cells[i].centre;
		const auto cos_latitude = std::max(0.05, std::cos(gps_coordinate::deg2rad(centre.latitude())));

		for (auto dy = -1; dy <= 1; ++dy)
		{
			for (auto dx = -1; dx <= 1; ++dx)
			{
				const auto key = cell_of(centre.latitude() + dy * cell_degrees,
				                         centre.longitude() + dx * cell_degrees / cos_latitude);
				const auto found = cell_index.find(key);
				if (found == cell_index.end()) continue;

				const auto j = found->second;
				if (merged_into[j] != no_cluster || cells[j].centre.distance_in_kilometers(centre) > merge_km) continue;

				merged_into[j] = i;
				auto& into = cells[i];
				const auto& from = cells[j];
				into.count += from.count;
				into.min_latitude = std::min(into.min_latitude, from.min_latitude);
				into.max_latitude = std::max(into.max_latitude, from.max_latitude);
				into.min_longitude = std::min(into.min_longitude, from.min_longitude);
				into.max_longitude = std::max(into.max_longitude, from.max_longitude);
				into.representative.consider(from.representative.path, from.representative.rank);
				centres[i].x += centres[j].x;
				centres[i].y += centres[j].y;
				centres[i].z += centres[j].z;
			}
		}

		cells[i].centre = unit_vector_centre(centres[i].x, centres[i].y, centres[i].z, centre);
	}

	// What is left, in the order the cells were met, and each located item's place among it.
	std::vector<uint32_t> renumbered(cells.size(), no_cluster);

	for (uint32_t i = 0; i < cells.size(); ++i)
	{
		if (merged_into[i] != i) continue;
		renumbered[i] = static_cast<uint32_t>(result.clusters.size());
		result.clusters.push_back(cells[i]);
	}

	for (size_t i = 0; i < cell_of_located.size(); ++i)
	{
		result.of_located[i] = renumbered[merged_into[cell_of_located[i]]];
	}

	return result;
}

static df::search_t& add_place_terms(df::search_t& search, const df::explore_place& place)
{
	if (place.level == df::location_level::state) search.location(place.state.sv(), df::location_level::state);
	search.location(place.country.sv(), df::location_level::country);
	return search;
}

df::search_t df::explore_days_search(const uint32_t first_day, const uint32_t last_day)
{
	return search_t().date_range(explore_date(first_day), explore_date(last_day), date_parts_prop::original);
}

df::search_t df::explore_month_search(const explore_month& month)
{
	return search_t().day(0, month.month, month.year, date_parts_prop::original);
}

df::search_t df::explore_year_search(const int year)
{
	return search_t().year(year, date_parts_prop::original);
}

df::search_t df::explore_trip_search(const explore_trip& trip)
{
	auto search = search_t();
	add_place_terms(search, trip.place);
	return search.date_range(explore_date(trip.first_day), explore_date(trip.last_day), date_parts_prop::original);
}

df::search_t df::explore_year_place_search(const explore_year_place& place)
{
	auto search = search_t();
	add_place_terms(search, place.place);
	return search.year(place.year, date_parts_prop::original);
}

df::search_t df::map_area_search(const map_location_area& area)
{
	auto search = search_t();

	// locations.md: an area opens a place plus a radius, never an internal `area:` term. "Within 25 km of
	// London" is a concept the user holds; "the cells GeoNames happened to attribute to London" is not.
	const auto km = area.search_radius_km();

	if (area.place_position.is_valid() && !area.name.empty())
	{
		search.location(area.name, location_level::any).set_place_distance(km);
	}
	else if (area.position.is_valid())
	{
		// Nothing was near enough to name, so the coordinates say exactly what was clicked.
		search.location(area.position, km);
	}

	return search;
}

location_t name_map_area(const location_cache& locations, const map_location_area& area)
{
	auto selected = locations.find_largest(area.min_latitude, area.min_longitude, area.max_latitude,
	                                       area.max_longitude);

	if (selected.id == 0)
	{
		selected = locations.find_closest(area.position.latitude(), area.position.longitude());
	}

	// A place may only name the area from within its attribution radius, so a distant city is never
	// borrowed as the answer for an empty region.
	if (selected.id != 0 && selected.position.is_valid() && area.position.is_valid() &&
	    selected.position.distance_in_kilometers(area.position) >
	        location_attribution_radius_km(selected.population) * 3.0)
	{
		return {};
	}

	return selected;
}
