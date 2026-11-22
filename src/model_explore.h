// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: What the Explore view shows of the collection: its items as a timeline of days, weeks,
// months and years, the trips in it - runs of days spent away from home - and the places it was taken
// at, clustered for the globe at the scale it is seen at. Built off the UI thread from a detached
// copy of the index and published whole. The searches the timeline's parts open are made here too,
// from the same predicates that counted them, so what a part says it holds and what clicking it lists
// are one question asked twice. A place on the globe opens as any map area does, as a place and a
// radius covering it. Also the naming and search of a map area, which the sidebar globe shares.

#pragma once

#include "model_location.h"
#include "model_search.h"

class location_cache;

namespace df
{
	struct index_file_item;

	// What a timeline bar counts an item as.
	enum class explore_kind : uint8_t
	{
		photo,
		video,
		other,
	};

	struct explore_counts
	{
		uint32_t photos = 0;
		uint32_t videos = 0;
		uint32_t other = 0;

		uint32_t total() const { return photos + videos + other; }
		bool is_empty() const { return total() == 0; }

		void add(const explore_kind kind, const uint32_t n = 1)
		{
			if (kind == explore_kind::photo) photos += n;
			else if (kind == explore_kind::video) videos += n;
			else other += n;
		}

		explore_counts& operator+=(const explore_counts& other_counts)
		{
			photos += other_counts.photos;
			videos += other_counts.videos;
			other += other_counts.other;
			return *this;
		}

		bool operator==(const explore_counts&) const = default;
	};

	// The picture that stands for a span of the timeline or a place on the globe: the best its items offer,
	// by the rule the sidebar calendar chooses its months' pictures by, so the two show one picture for a month.
	struct explore_representative
	{
		core::file_path path;
		uint8_t rank = 0;

		void consider(const core::file_path& candidate, uint8_t candidate_rank);
	};

	// One collection item, copied out of the index on a worker, so nothing built from it reaches back.
	struct explore_sample
	{
		// The local day it was taken, by the key a date search's `original` target compares, in days since
		// 1601 - a Monday, which is what makes a week a run of days starting at a multiple of seven.
		uint32_t day = 0;
		explore_kind kind = explore_kind::other;
		// How well it stands for a span: df::thumbnail_representative_rank.
		uint8_t rank = 0;
		gps_coordinate coordinate;
		// The place, state and country the file names, before the gazetteer fills what it leaves out.
		core::cached place = {};
		core::cached state = {};
		core::cached country = {};
		core::file_path path;
	};

	// Nothing when the item has no date the timeline can place.
	std::optional<explore_sample> explore_sample_of(const index_file_item& file, core::file_path path);

	// A place a trip or a year is labelled with, as the location search compares it: a country, or a state
	// together with its country, which keeps it from meaning a state of that name somewhere else.
	struct explore_place
	{
		std::string name;
		location_level level = location_level::country;
		core::cached state = {};
		core::cached country = {};

		bool same_place(const explore_place& other) const;
	};

	// A run of days spent in one place away from home: a country abroad, or another state of home's country.
	struct explore_trip
	{
		explore_place place;
		uint32_t first_day = 0;
		uint32_t last_day = 0;
		// How many items the trip's search lists: those taken in its days that are of its place.
		uint32_t count = 0;
		gps_coordinate centre;
		explore_representative representative;
	};

	// A place away from home and a year it was visited in: `Croatia 2026`.
	struct explore_year_place
	{
		int year = 0;
		explore_place place;
		uint32_t count = 0;
	};

	// A week, Monday to Sunday, cut to the year it falls in so no part of the timeline crosses a year.
	struct explore_week
	{
		uint32_t first_day = 0;
		uint32_t last_day = 0;
		explore_counts counts;
		explore_representative representative;
	};

	struct explore_month
	{
		int year = 0;
		int month = 0; // 1 to 12
		uint32_t first_day = 0;
		uint32_t last_day = 0;
		explore_counts counts;
		explore_representative representative;
	};

	struct explore_year
	{
		int year = 0;
		uint32_t first_day = 0;
		uint32_t last_day = 0;
		explore_counts counts;
	};

	// A located item, for the globe: where and when, and the places it counts toward.
	struct explore_located
	{
		uint32_t day = 0;
		gps_coordinate coordinate;
		uint8_t rank = 0;
		core::file_path path;
		// Indexes into explore_model::places of its country and of its state, or no_place.
		uint32_t country = 0;
		uint32_t state = 0;
	};

	inline constexpr uint32_t explore_no_place = std::numeric_limits<uint32_t>::max();

	struct explore_model
	{
		// The span the timeline covers: from the 1st of January of its first year to today.
		uint32_t first_day = 0;
		uint32_t today = 0;
		int first_year = 0;
		int last_year = 0;

		// Indexed by day - first_day.
		std::vector<explore_counts> days;

		// Newest first, as the timeline reads from today back.
		std::vector<explore_year> years;
		std::vector<explore_month> months;
		std::vector<explore_week> weeks;
		std::vector<explore_trip> trips;
		std::vector<explore_year_place> year_places;

		// Every located item, dated or not, oldest first. Undated ones hold day zero.
		std::vector<explore_located> located;
		// The level-scoped names located items' places index.
		std::vector<core::cached> places;

		explore_counts total;
		// Items the timeline leaves out: undated, or dated before the years it offers.
		uint32_t outside = 0;

		bool is_empty() const { return total.is_empty() && located.empty(); }

		const explore_counts& day_counts(const uint32_t day) const
		{
			static const explore_counts none;
			return day >= first_day && day <= today && day - first_day < days.size() ? days[day - first_day] : none;
		}

		explore_counts span_counts(uint32_t first, uint32_t last) const;

		// Located items from first to last, inclusive, as a range of `located`.
		std::pair<size_t, size_t> located_between(uint32_t first, uint32_t last) const;
	};

	using explore_model_ptr = std::shared_ptr<const explore_model>;

	struct explore_request
	{
		std::vector<explore_sample> samples;
		// The local day it is.
		uint32_t today = 0;
		// The first year the user typed in Options for the sidebar calendar, or zero to work it out.
		int start_year = 0;
	};

	// Fills the places files leave out from the gazetteer, when there is one, and so runs on the location
	// worker.
	explore_model build_explore_model(explore_request request, const location_cache* locations);

	// Trips need this many items at least, so a stray photo carried across a border is not a journey.
	inline constexpr uint32_t explore_trip_min_items = 3;
	// Days with nothing located may separate the days of one trip; any more and it is two.
	inline constexpr uint32_t explore_trip_max_gap_days = 4;
	// Home is where most days were spent in the years around: a share this large of them is home as well,
	// so a life lived either side of a border is not one trip after another.
	inline constexpr double explore_home_share = 0.25;
	inline constexpr size_t explore_max_places_per_year = 4;

	// Places on the globe, gathered into cells about this many pixels across at the scale the globe is
	// seen at, and the level of cell that is.
	inline constexpr double explore_cluster_px = 64.0;
	inline constexpr int explore_max_cluster_level = 24;
	int explore_cluster_level(double globe_radius_px);

	struct explore_cluster
	{
		gps_coordinate centre;
		uint32_t count = 0;
		double min_latitude = 0.0;
		double min_longitude = 0.0;
		double max_latitude = 0.0;
		double max_longitude = 0.0;
		explore_representative representative;
	};

	struct explore_clusters
	{
		int level = -1;
		std::vector<explore_cluster> clusters;
		// For each of the model's located items, the cluster it is in.
		std::vector<uint32_t> of_located;
	};

	explore_clusters cluster_explore_model(const explore_model& model, int level);

	// The searches the parts open. Each starts from the whole collection, as the sidebar's do.
	search_t explore_days_search(uint32_t first_day, uint32_t last_day);
	search_t explore_month_search(const explore_month& month);
	search_t explore_year_search(int year);
	search_t explore_trip_search(const explore_trip& trip);
	search_t explore_year_place_search(const explore_year_place& place);

	// A map area as a search: the place that names it and a radius covering every item it showed, or its
	// coordinate and that radius when nothing was near enough to name it.
	search_t map_area_search(const map_location_area& area);

	// The day number of a date, and its date.
	uint32_t explore_day(int year, int month, int day);
	core::day_t explore_date(uint32_t day);
}

// The place that names a map area: the largest inside its bounds, else the closest, and nothing when even
// that is too far from it to be a truthful answer. Reads the gazetteer, so it runs on the location worker.
location_t name_map_area(const location_cache& locations, const map_location_area& area);
