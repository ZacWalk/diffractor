// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: locations.md 6 -- derives visit nodes from a result set. A visit is a run of items
// close together in space whose gaps in time are all short, which is the shape of a trip. The
// input is a vector of detached samples taken on the UI thread and the output is a vector of
// detached nodes, so the whole computation runs on a worker and owns nothing the UI owns.

#pragma once

#include "model_location.h"
#include "model_search.h"
#include "libdesktop/util_date.h"

class location_cache;

namespace df
{
	// One result item, copied out of UI-owned state so a worker never reads an item_element.
	struct visit_sample
	{
		uint32_t days = 0;
		gps_coordinate coordinate;
		str::cached place = {};
		str::cached state = {};
		str::cached country = {};
	};

	enum class visit_kind : uint8_t
	{
		visit,
		era,
	};

	struct visit_node
	{
		std::string name;
		date_t first;
		date_t last;
		uint32_t count = 0;
		uint32_t active_days = 0;
		double radius_km = 0.0;
		gps_coordinate centre;
		bool named = false;
		visit_kind kind = visit_kind::visit;
		double score = 0.0;
		int cluster = 0;
	};

	// locations.md 7.2: one place inside the result set, keyed exactly as the location grouping
	// keys it. A chip states a count and then runs a search, so the two have to be the same
	// question asked twice -- which rules out a radius, because a circle drawn around one place
	// contains whatever else happens to lie inside it.
	struct visit_place_tally
	{
		std::string name;

		// The level-scoped terms that reproduce exactly the items counted here.
		str::cached place;
		str::cached state;
		str::cached country;
		uint32_t count = 0;
	};

	struct visit_timeline
	{
		std::vector<visit_node> nodes;
		std::vector<visit_place_tally> places;

		uint32_t sample_count = 0;
		uint32_t located_count = 0;
		uint32_t undated_count = 0;
		uint32_t unlocated_count = 0;

		// locations.md 6.2 step 8: false means show nothing at all, which is the honest answer for
		// a result set whose items do not form trips.
		bool publish = false;
	};

	struct visit_work_counters
	{
		uint64_t place_predicate_unions = 0;
	};

	struct visit_request
	{
		visit_request() = default;
		explicit visit_request(cancel_token value) : token(std::move(value)) {}

		std::vector<visit_sample> samples;
		cancel_token token;
		visit_work_counters* counters = nullptr;

		// locations.md 6.2 step 6: an era is revealed only when the query names its place.
		std::string intent_place;
	};

	// locations.md 6.2: cluster, segment, score, then select. Runs on the location worker.
	visit_timeline compute_visits(visit_request request, const location_cache& locations);

	// locations.md 6.3: the coarsest label that still says which visit this is -- `Jun 2019`,
	// `2019`, `2000-2010`.
	std::string visit_node_dates(const visit_node& node);

	// locations.md 6.3/6.5: the query a node runs. Location and date terms are replaced rather
	// than added to, so clicking a second node moves the view instead of intersecting two trips,
	// and the result reproduces exactly the items the node counted.
	search_t visit_node_search(const search_t& current, const visit_node& node);

	// locations.md 7.2: the query a breakdown place runs -- the level-scoped place terms it was
	// counted by, with the current date scope left alone.
	search_t visit_place_search(const search_t& current, const visit_place_tally& place);

	// The place named in full, for the bubble the short chip label cannot carry.
	std::string visit_place_detail(const visit_place_tally& place);

	// locations.md 6.3: a latched node is the one the current query already reproduces.
	bool is_visit_node_selected(const search_t& current, const visit_node& node);
}

// locations.md 6.2 step 2: clusters merge when their centres are this close and the bucket grid
// is finer, so a place that straddles a bucket boundary still forms one cluster.
inline constexpr double visit_cluster_merge_km = 25.0;
inline constexpr double visit_bucket_degrees = 0.25;

// locations.md 6.2 step 3: G = clamp(3 x median gap, 14 days, 180 days).
inline constexpr int visit_gap_min_days = 14;
inline constexpr int visit_gap_max_days = 180;

// locations.md 6.5: ten nodes maximum. The strip is a starting point, not a summary of a life.
inline constexpr int visit_max_nodes = 10;
inline constexpr uint32_t visit_min_node_items = 5;
inline constexpr double visit_min_node_share = 0.01;
inline constexpr double visit_publish_coverage = 0.40;

// locations.md 6.2 step 5: a decade of photos from where the user lives is not a trip.
inline constexpr int visit_era_min_span_days = 548;
inline constexpr double visit_era_month_occupancy = 0.25;
inline constexpr double visit_era_collection_share = 0.15;

// locations.md 6.2 step 4: tuned constants, recorded next to the implementation rather than
// exposed as settings. Separation dominates because a run standing alone in time is what a user
// remembers as a trip; rarity dominates because somewhere they rarely go is the interesting answer.
inline constexpr double visit_weight_items = 1.0;
inline constexpr double visit_weight_span = 0.5;
inline constexpr double visit_weight_density = 0.3;
inline constexpr double visit_weight_separation = 0.8;
inline constexpr double visit_weight_rarity = 1.2;

// A globally scattered collection could otherwise produce a cluster per item. The cap bounds the
// merge pass, which is quadratic in cluster count.
inline constexpr size_t visit_max_clusters = 512;
inline constexpr size_t visit_max_places = 8;
