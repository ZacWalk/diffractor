// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: locations.md 6.2 -- the visit derivation. Filter, tally places, cluster, name, segment,
// score, classify, select, publish. Every step is bounded by the result count and none of it
// touches UI state.

#include "pch.h"
#include "model_visits.h"
#include "model_locations.h"
#include "app_text.h"

namespace
{
	// A running spherical mean. Summing latitude and longitude directly is wrong across the
	// antimeridian - +179 and -179 average to 0, the middle of the map rather than the middle of the
	// pair - and wrong again near the poles. Unit vectors have no seam, so they sum and merge the
	// same way everywhere and read back as a direction.
	struct coordinate_sum
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;

		void add(const gps_coordinate c)
		{
			const auto lat = gps_coordinate::deg2rad(c.latitude());
			const auto lon = gps_coordinate::deg2rad(c.longitude());
			const auto cos_lat = std::cos(lat);
			x += cos_lat * std::cos(lon);
			y += cos_lat * std::sin(lon);
			z += std::sin(lat);
		}

		void add(const coordinate_sum& other)
		{
			x += other.x;
			y += other.y;
			z += other.z;
		}

		// Degenerate only when the members cancel out exactly - antipodal pairs have no meaningful
		// centre - so the caller's fallback is the answer rather than a direction read from zero.
		gps_coordinate mean(const gps_coordinate fallback) const
		{
			const auto hypotenuse = std::sqrt(x * x + y * y);

			if (hypotenuse < 1e-12 && std::abs(z) < 1e-12) return fallback;

			const auto lat = std::atan2(z, hypotenuse);
			const auto lon = (hypotenuse < 1e-12) ? 0.0 : std::atan2(y, x);

			// gps_coordinate reserves 180 as its "no coordinate" sentinel, so a mean that lands
			// exactly on the antimeridian has to be nudged inside the representable range rather
			// than answered as an absent coordinate.
			auto lon_deg = gps_coordinate::rad2deg(lon);
			if (lon_deg >= 180.0) lon_deg = std::nextafter(180.0, 0.0);
			else if (lon_deg <= -180.0) lon_deg = std::nextafter(-180.0, 0.0);

			return gps_coordinate(gps_coordinate::rad2deg(lat), lon_deg);
		}
	};

	struct cluster_t
	{
		coordinate_sum coordinates;
		uint32_t coord_count = 0;
		std::vector<size_t> members;

		gps_coordinate centre;
		std::string name;
		bool named = false;

		// Where a query for `name` resolves to. A covering radius is measured from here rather than
		// from the centroid, because this is the point the search measures from.
		gps_coordinate anchor;
	};

	struct resolved_name_t
	{
		std::string name;
		gps_coordinate anchor;
	};

	// locations.md 2.3/3.1: stored text qualifies nothing, so a bare `Richmond` would resolve to
	// the largest Richmond on earth rather than the one the items are in. The gazetteer record for
	// the stored parts supplies both the qualified label and the point a radius measures from.
	// The identity a place name carries. Place alone merges namesakes - a Springfield in Illinois
	// and one in Massachusetts are not the same visit - so the qualifying parts belong in the key,
	// and folding the case is what stops one spelling splitting into two.
	std::string place_identity_key(const str::cached place, const str::cached state, const str::cached country)
	{
		auto key = std::format("{}\x1f{}\x1f{}", place.sv(), state.sv(), country.sv());
		return str::to_lower(key);
	}

	std::string place_subset_key(const df::visit_place_tally& t, const uint32_t mask)
	{
		return place_identity_key((mask & 1u) != 0 ? t.place : str::cached{},
		                          (mask & 2u) != 0 ? t.state : str::cached{},
		                          (mask & 4u) != 0 ? t.country : str::cached{});
	}

	uint32_t place_mask(const df::visit_place_tally& t)
	{
		return (is_empty(t.place) ? 0u : 1u) |
			(is_empty(t.state) ? 0u : 2u) |
			(is_empty(t.country) ? 0u : 4u);
	}

	resolved_name_t resolve_stored_name(const df::visit_sample& s, const location_cache& locations,
	                                    std::map<std::string, resolved_name_t>& memo)
	{
		std::string query;
		const auto append = [&query](const str::cached part)
		{
			if (is_empty(part)) return;
			if (!query.empty()) query.append(", ");
			query.append(part.sv());
		};

		append(s.place);
		append(s.state);
		append(s.country);

		// find_by_name matches case-insensitively, so the memo has to as well or the same place
		// spelt differently misses and re-reads the gazetteer.
		const auto memo_key = str::to_lower(query);

		const auto found = memo.find(memo_key);
		if (found != memo.end()) return found->second;

		resolved_name_t result;
		result.name = qualified_name(location_t{0, s.place, s.state, s.country, {}, 0.0, 0});

		if (locations.is_index_loaded())
		{
			const auto record = locations.find_by_name(query);

			if (record.has_gps())
			{
				result.name = qualified_name(record);
				result.anchor = record.position;
			}
		}

		memo[memo_key] = result;
		return result;
	}

	class place_components
	{
		std::vector<size_t> _parent;

	public:
		explicit place_components(const size_t n) : _parent(n)
		{
			for (auto i = 0u; i < _parent.size(); ++i) _parent[i] = i;
		}

		size_t find(const size_t i)
		{
			if (_parent[i] != i) _parent[i] = find(_parent[i]);
			return _parent[i];
		}

		bool unite(const size_t l, const size_t r)
		{
			const auto lr = find(l);
			const auto rr = find(r);
			if (lr == rr) return false;
			_parent[rr] = lr;
			return true;
		}
	};

	// locations.md 7.2: the strip is a partition, and a chip's count and its click are one promise.
	// Exact identities are collapsed through the finite place/state/country predicate subsets. If
	// a partial chip would return sharper identities, the whole connected set merges to the common
	// predicate its search can honestly advertise.
	bool collapse_subsumed_places(std::vector<df::visit_place_tally>& tallies, const df::cancel_token& token,
	                              df::visit_work_counters* counters)
	{
		if (tallies.size() < 2) return true;

		place_components components(tallies.size());
		std::map<std::string, size_t> exact_keys;
		std::map<std::string, std::vector<size_t>> subset_keys;

		for (auto i = 0u; i < tallies.size(); ++i)
		{
			if ((i & 0xffu) == 0 && token.is_cancelled()) return false;
			const auto mask = place_mask(tallies[i]);
			exact_keys[place_subset_key(tallies[i], mask)] = i;

			for (auto subset = 1u; subset < 8u; ++subset)
			{
				if ((subset & mask) == subset) subset_keys[place_subset_key(tallies[i], subset)].emplace_back(i);
			}
		}

		auto unite_predicate_matches = [&](const std::string& key, const size_t with)
		{
			auto changed = false;
			const auto found = subset_keys.find(key);
			if (found == subset_keys.end()) return false;

			for (const auto i : found->second)
			{
				if (counters) ++counters->place_predicate_unions;
				changed |= components.unite(with, i);
			}

			return changed;
		};

		for (auto i = 0u; i < tallies.size(); ++i)
		{
			if ((i & 0xffu) == 0 && token.is_cancelled()) return false;
			const auto mask = place_mask(tallies[i]);
			unite_predicate_matches(place_subset_key(tallies[i], mask), i);
		}

		auto changed = true;
		while (changed)
		{
			changed = false;

			std::map<size_t, std::vector<size_t>> groups;
			for (auto i = 0u; i < tallies.size(); ++i) groups[components.find(i)].emplace_back(i);
			std::set<std::string> processed_predicates;

			for (const auto& [root, group] : groups)
			{
				if (token.is_cancelled()) return false;
				df::visit_place_tally predicate;
				auto mask = 7u;

				const auto resolve_field = [&](const auto ptr, const uint32_t bit)
				{
					auto value = tallies[group.front()].*ptr;

					for (const auto i : group)
					{
						const auto current = tallies[i].*ptr;
						if (is_empty(value) || is_empty(current) || icmp(value, current) != 0)
						{
							value = {};
							mask &= ~bit;
							break;
						}
					}

					return value;
				};

				predicate.place = resolve_field(&df::visit_place_tally::place, 1u);
				predicate.state = resolve_field(&df::visit_place_tally::state, 2u);
				predicate.country = resolve_field(&df::visit_place_tally::country, 4u);

				if (mask != 0)
				{
					const auto key = place_subset_key(predicate, mask);
					if (processed_predicates.emplace(key).second)
					{
						changed |= unite_predicate_matches(key, root);
					}
				}
			}
		}

		std::map<size_t, std::vector<size_t>> groups;
		for (auto i = 0u; i < tallies.size(); ++i) groups[components.find(i)].emplace_back(i);

		std::vector<df::visit_place_tally> collapsed;
		collapsed.reserve(groups.size());

		for (const auto& [root, group] : groups)
		{
			df::visit_place_tally merged;
			for (const auto i : group) merged.count += tallies[i].count;
			auto mask = 7u;

			const auto merge_field = [&](const auto ptr, const uint32_t bit)
			{
				auto value = tallies[group.front()].*ptr;

				for (const auto i : group)
				{
					const auto current = tallies[i].*ptr;
					if (is_empty(value) || is_empty(current) || icmp(value, current) != 0)
					{
						value = {};
						mask &= ~bit;
						break;
					}
				}

				return value;
			};

			merged.place = merge_field(&df::visit_place_tally::place, 1u);
			merged.state = merge_field(&df::visit_place_tally::state, 2u);
			merged.country = merge_field(&df::visit_place_tally::country, 4u);

			if (mask == 0)
			{
				struct candidate
				{
					size_t index = 0;
					std::string key;
					std::vector<size_t> covered;
					uint32_t count = 0;
				};

				std::vector<candidate> candidates;

				for (const auto i : group)
				{
					const auto exact_key = place_subset_key(tallies[i], place_mask(tallies[i]));
					const auto found = subset_keys.find(exact_key);

					if (found != subset_keys.end())
					{
						candidate c;
						c.index = i;
						c.key = exact_key;
						c.covered = found->second;

						for (const auto covered : c.covered)
						{
							c.count += tallies[covered].count;
						}

						candidates.emplace_back(std::move(c));
					}
				}

				std::ranges::sort(candidates, [](const candidate& l, const candidate& r)
				{
					return l.count == r.count ? l.key < r.key : l.count > r.count;
				});

				std::vector<bool> covered(tallies.size(), false);

				for (const auto& candidate : candidates)
				{
					auto overlaps = false;

					for (const auto i : candidate.covered)
					{
						overlaps = covered[i];
						if (overlaps) break;
					}

					if (overlaps) continue;

					for (const auto i : candidate.covered) covered[i] = true;

					auto tally = tallies[candidate.index];
					tally.count = candidate.count;
					collapsed.emplace_back(std::move(tally));
				}

				continue;
			}

			collapsed.emplace_back(std::move(merged));
		}

		tallies = std::move(collapsed);
		return true;
	}

	std::string place_label(const df::visit_place_tally& t, const location_qualification level)
	{
		return qualified_name(location_t{
			0, t.place, t.state, t.country, {}, 0.0, static_cast<uint32_t>(level)
		});
	}

	// locations.md 2.1 applied to the strip: the smallest name form that still tells the chips
	// apart. Two Londons that read `London` twice are one affordance the user cannot choose from.
	void qualify_place_names(std::vector<df::visit_place_tally>& tallies)
	{
		constexpr location_qualification levels[] = {
			location_qualification::name,
			location_qualification::name_country,
			location_qualification::name_region_country,
		};

		for (auto& tally : tallies)
		{
			auto chosen = place_label(tally, levels[std::size(levels) - 1]);

			for (const auto level : levels)
			{
				auto label = place_label(tally, level);
				auto collides = false;

				for (const auto& other : tallies)
				{
					if (&other == &tally) continue;
					collides = str::icmp(label, place_label(other, level)) == 0;
					if (collides) break;
				}

				if (!collides)
				{
					chosen = std::move(label);
					break;
				}
			}

			tally.name = std::move(chosen);
		}
	}

	// The bucket grid is finer than the merge distance on purpose: buckets only exist to make the
	// merge pass cheap, and the merge is what decides what a cluster is.
	struct bucket_key
	{
		int lat = 0;
		int lon = 0;

		friend bool operator<(const bucket_key l, const bucket_key r)
		{
			if (l.lat != r.lat) return l.lat < r.lat;
			return l.lon < r.lon;
		}
	};

	constexpr uint32_t average_days_per_month = 30;

	int median_gap_days(const std::vector<uint32_t>& sorted_days)
	{
		if (sorted_days.size() < 2) return 0;

		std::vector<uint32_t> gaps;
		gaps.reserve(sorted_days.size() - 1);

		for (size_t i = 1; i < sorted_days.size(); ++i)
		{
			gaps.emplace_back(sorted_days[i] - sorted_days[i - 1]);
		}

		std::ranges::nth_element(gaps, gaps.begin() + gaps.size() / 2);
		return static_cast<int>(gaps[gaps.size() / 2]);
	}

	struct candidate_t
	{
		int cluster = 0;
		size_t begin = 0;
		size_t end = 0;
		uint32_t first_days = 0;
		uint32_t last_days = 0;
		uint32_t count = 0;
		uint32_t active_days = 0;
		uint32_t active_months = 0;
		uint32_t gap_before = 0;
		uint32_t gap_after = 0;
		int gap_threshold = 0;
		double score = 0.0;
		bool is_era = false;
	};
}

df::visit_timeline df::compute_visits(visit_request request, const location_cache& locations)
{
	visit_timeline result;
	const auto& samples = request.samples;
	result.sample_count = static_cast<uint32_t>(samples.size());

	// 1. Filter. An item without a date cannot sit on a timeline, and an item without any location
	// cannot sit anywhere. Both are counted rather than guessed at, because inventing a node is
	// worse than showing none. Located items keep their place in the clustering whether or not they
	// are dated, because the breakdown's query carries no date and so must count none either.
	std::vector<size_t> usable;
	usable.reserve(samples.size());
	std::map<std::string, resolved_name_t> name_memo;

	for (size_t i = 0; i < samples.size(); ++i)
	{
		if ((i & 0xffu) == 0 && request.token.is_cancelled()) return result;
		const auto& s = samples[i];

		// A state or province is a location. Leaving it out of this test counted a photograph
		// described only by its state as unlocated, so the summary understated how much of the
		// result is placed - and the place breakdown below, which keys on the same fields, still
		// listed it.
		const auto has_location = s.coordinate.is_valid() || !is_empty(s.place) || !is_empty(s.state) ||
			!is_empty(s.country);

		if (s.days == 0)
		{
			++result.undated_count;
		}
		else if (!has_location)
		{
			++result.unlocated_count;
		}
		else
		{
			++result.located_count;
		}

		if (has_location) usable.emplace_back(i);
	}

	// locations.md 7.2: the place breakdown, keyed exactly as the location grouping keys a group,
	// so every chip runs the query that group header would run and returns precisely what it
	// counted. It is a partition of the located items, and it is built before the timeline decides
	// anything, because a result set that yields no visit still has places in it.
	{
		std::map<std::string, size_t> keys;
		std::map<attribution_cell, located_place> attributed;
		std::vector<visit_place_tally> tallies;

		for (const auto i : usable)
		{
			if ((i & 0xffu) == 0 && request.token.is_cancelled()) return result;
			const auto& s = samples[i];
			auto place = s.place;
			auto state = s.state;
			auto country = s.country;

			// Stored text is the user's own answer; attribution only fills what it left empty.
			if (s.coordinate.is_valid() && (is_empty(place) || is_empty(state) || is_empty(country)))
			{
				const attribution_cell cell(s.coordinate);
				const auto found = attributed.find(cell);
				const auto resolved = found != attributed.end()
					                      ? found->second
					                      : locations.find_attributed(s.coordinate);

				if (found == attributed.end() && locations.is_index_loaded())
				{
					attributed.emplace(cell, resolved);
				}

				if (is_empty(place)) place = resolved.place.place;
				if (is_empty(state)) state = resolved.place.state;
				if (is_empty(country)) country = resolved.place.country;
			}

			if (is_empty(place) && is_empty(state) && is_empty(country)) continue;

			auto key = place_identity_key(place, state, country);

			const auto found = keys.find(key);

			if (found != keys.end())
			{
				++tallies[found->second].count;
				continue;
			}

			keys[key] = tallies.size();

			visit_place_tally tally;
			tally.place = place;
			tally.state = state;
			tally.country = country;
			tally.count = 1;
			tallies.emplace_back(std::move(tally));
		}

		if (!collapse_subsumed_places(tallies, request.token, request.counters)) return result;

		std::ranges::stable_sort(tallies, [](const visit_place_tally& l, const visit_place_tally& r)
		{
			return l.count > r.count;
		});

		if (tallies.size() > visit_max_places) tallies.resize(visit_max_places);

		// Named last, because a chip only has to be as specific as the strip it appears in.
		qualify_place_names(tallies);
		result.places = std::move(tallies);
	}

	if (usable.size() < visit_min_node_items) return result;

	// 2. Cluster. Coordinates bucket spatially and then merge; items that carry only place text
	// cluster by that text, so a scanned photo labelled `Paris` still joins a visit.
	std::vector<cluster_t> clusters;
	std::map<bucket_key, std::vector<size_t>> buckets;
	std::map<std::string, int> text_clusters;

	for (const auto i : usable)
	{
		const auto& s = samples[i];

		if (s.coordinate.is_valid())
		{
			bucket_key key;
			key.lat = round(s.coordinate.latitude() / visit_bucket_degrees);
			key.lon = round(s.coordinate.longitude() / visit_bucket_degrees);
			buckets[key].emplace_back(i);
		}
		else
		{
			// The same identity the place tallies use. Keying on the place name alone merged
			// namesakes into one visit - a Springfield in Illinois and one in Massachusetts are not
			// the same trip - and a name that appeared with and without its qualifiers split into
			// two. A country-only sample keys on the country, which is all the identity it has.
			auto key = place_identity_key(s.place, s.state, s.country);

			const auto found = text_clusters.find(key);

			if (found != text_clusters.end())
			{
				clusters[found->second].members.emplace_back(i);
			}
			else
			{
				text_clusters[key] = static_cast<int>(clusters.size());
				cluster_t c;
				const auto resolved = resolve_stored_name(s, locations, name_memo);
				c.name = resolved.name;
				c.named = !c.name.empty();
				c.members.emplace_back(i);
				clusters.emplace_back(std::move(c));
			}
		}
	}

	// Largest buckets seed clusters, so a merge always folds a small bucket into the place that
	// most of the items actually came from rather than the other way round.
	std::vector<const std::pair<const bucket_key, std::vector<size_t>>*> ordered;
	ordered.reserve(buckets.size());
	for (const auto& b : buckets) ordered.emplace_back(&b);
	std::ranges::sort(ordered, [](auto* l, auto* r) { return l->second.size() > r->second.size(); });

	const auto first_coord_cluster = clusters.size();

	for (const auto* const b : ordered)
	{
		coordinate_sum bucket_coordinates;

		for (const auto i : b->second)
		{
			bucket_coordinates.add(samples[i].coordinate);
		}

		const auto centre = bucket_coordinates.mean(samples[b->second.front()].coordinate);
		auto merged = false;

		for (size_t c = first_coord_cluster; c < clusters.size(); ++c)
		{
			if (clusters[c].centre.distance_in_kilometers(centre) <= visit_cluster_merge_km)
			{
				clusters[c].coordinates.add(bucket_coordinates);
				clusters[c].coord_count += static_cast<uint32_t>(b->second.size());
				clusters[c].centre = clusters[c].coordinates.mean(clusters[c].centre);
				clusters[c].members.insert(clusters[c].members.end(), b->second.begin(), b->second.end());
				merged = true;
				break;
			}
		}

		if (merged) continue;
		if (clusters.size() >= visit_max_clusters) continue;

		cluster_t c;
		c.coordinates = bucket_coordinates;
		c.coord_count = static_cast<uint32_t>(b->second.size());
		c.centre = centre;
		c.members = b->second;
		clusters.emplace_back(std::move(c));
	}

	// Name each cluster once. Attribution reads the gazetteer, so it runs per cluster and never
	// per item; a result set of a hundred thousand photos still asks a few hundred questions.
	for (size_t c = first_coord_cluster; c < clusters.size(); ++c)
	{
		auto& cluster = clusters[c];
		if (!cluster.centre.is_valid()) continue;

		// A stored place name is the truth about where the photo was taken; attribution is only a
		// fallback for coordinates nobody labelled.
		for (const auto i : cluster.members)
		{
			if (!is_empty(samples[i].place))
			{
				const auto resolved = resolve_stored_name(samples[i], locations, name_memo);
				cluster.name = resolved.name;
				cluster.named = !cluster.name.empty();
				cluster.anchor = resolved.anchor;
				break;
			}
		}

		if (!cluster.named && locations.is_index_loaded())
		{
			const auto resolved = locations.find_attributed(cluster.centre);

			if (resolved.is_located() && resolved.distance_km <= visit_cluster_merge_km)
			{
				cluster.name = qualified_name(resolved.place);
				cluster.named = !cluster.name.empty();
				cluster.anchor = resolved.place.position;
			}
		}

		if (!cluster.named)
		{
			cluster.name = std::string(tt.location_remote.sv());
		}
	}

	// 3. Segment, and 5. classify eras. An era is a property of the whole cluster rather than of
	// one run inside it: somewhere lived in produces hundreds of short runs, and calling each of
	// them a trip would bury every real trip the user went looking for.
	std::vector<candidate_t> candidates;
	std::vector<std::vector<uint32_t>> cluster_days(clusters.size());
	const auto located = static_cast<double>(result.located_count);

	for (size_t c = 0; c < clusters.size(); ++c)
	{
		// Only the dated members can be segmented; the undated ones are in the cluster for the
		// breakdown's benefit and would otherwise read as day zero.
		auto& days = cluster_days[c];
		days.reserve(clusters[c].members.size());
		for (const auto i : clusters[c].members) if (samples[i].days != 0) days.emplace_back(samples[i].days);
		std::ranges::sort(days);

		if (days.empty()) continue;

		const auto threshold = std::clamp(3 * median_gap_days(days), visit_gap_min_days, visit_gap_max_days);

		const auto cluster_span = static_cast<double>(days.back() - days.front());
		const auto cluster_share = static_cast<double>(days.size()) / std::max(1.0, located);
		auto cluster_months = 1u;

		for (size_t d = 1; d < days.size(); ++d)
		{
			// A calendar month is not needed here; the question is only how much of the span has
			// items in it, and an average month answers that without a date conversion.
			if (days[d] / average_days_per_month != days[d - 1] / average_days_per_month) ++cluster_months;
		}

		const auto is_era = cluster_span > visit_era_min_span_days
			&& cluster_months / std::max(1.0, cluster_span / average_days_per_month) > visit_era_month_occupancy
			&& cluster_share > visit_era_collection_share;

		if (is_era)
		{
			// 6. Reveal on intent. An era is offered only when the query already names its place,
			// and then it arrives as one node carrying its own bounds -- `London 2000-2010`.
			if (request.intent_place.empty() || str::icmp(clusters[c].name, request.intent_place) != 0) continue;

			candidate_t era;
			era.cluster = static_cast<int>(c);
			era.first_days = days.front();
			era.last_days = days.back();
			era.count = static_cast<uint32_t>(days.size());
			era.active_months = cluster_months;
			era.gap_threshold = threshold;
			era.active_days = 1;

			for (size_t d = 1; d < days.size(); ++d)
			{
				if (days[d] != days[d - 1]) ++era.active_days;
			}

			era.is_era = true;
			candidates.emplace_back(era);
			continue;
		}

		size_t run_begin = 0;
		const auto first_candidate = candidates.size();

		for (size_t i = 1; i <= days.size(); ++i)
		{
			const auto split = i == days.size() ||
				static_cast<int>(days[i] - days[i - 1]) > threshold;

			if (!split) continue;

			candidate_t cand;
			cand.cluster = static_cast<int>(c);
			cand.begin = run_begin;
			cand.end = i;
			cand.first_days = days[run_begin];
			cand.last_days = days[i - 1];
			cand.count = static_cast<uint32_t>(i - run_begin);
			cand.gap_threshold = threshold;
			cand.active_days = 1;
			cand.active_months = 1;

			for (size_t d = run_begin + 1; d < i; ++d)
			{
				if (days[d] != days[d - 1]) ++cand.active_days;
				if (days[d] / average_days_per_month != days[d - 1] / average_days_per_month) ++cand.active_months;
			}

			if (!candidates.empty() && candidates.size() > first_candidate)
			{
				auto& prev = candidates.back();
				const auto gap = cand.first_days - prev.last_days;
				prev.gap_after = gap;
				cand.gap_before = gap;
			}

			candidates.emplace_back(cand);
			run_begin = i;
		}
	}

	if (candidates.empty()) return result;

	// 4. Score.
	for (auto& cand : candidates)
	{
		const auto span_days = static_cast<double>(cand.last_days - cand.first_days);
		const auto cluster_count = static_cast<double>(cluster_days[cand.cluster].size());
		const auto threshold = static_cast<double>(std::max(1, cand.gap_threshold));

		const auto density = static_cast<double>(cand.count) / std::max(1u, cand.active_days);
		const auto neighbour = cand.gap_before == 0
			                       ? cand.gap_after
			                       : (cand.gap_after == 0
				                          ? cand.gap_before
				                          : std::min(cand.gap_before, cand.gap_after));
		// locations.md 6.2: normalized by 4G, not G. Segmentation already split on every gap
		// larger than G, so normalizing by G would clamp this to 1 for every candidate.
		const auto separation = neighbour == 0 ? 1.0 : std::min(1.0, neighbour / (threshold * 4.0));
		const auto rarity = 1.0 - std::min(1.0, cluster_count / std::max(1.0, located));

		cand.score = visit_weight_items * std::log(1.0 + cand.count)
			+ visit_weight_span * std::log(1.0 + span_days)
			+ visit_weight_density * density
			+ visit_weight_separation * separation
			+ visit_weight_rarity * rarity;
	}

	// 7. Select. Strongest first, then back into chronological order so the strip reads as time.
	std::ranges::stable_sort(candidates, [](const candidate_t& l, const candidate_t& r)
	{
		return l.score > r.score;
	});

	const auto min_items = std::max(visit_min_node_items,
	                                static_cast<uint32_t>(located * visit_min_node_share));

	std::vector<candidate_t> selected;
	selected.reserve(visit_max_nodes);

	for (const auto& cand : candidates)
	{
		if (selected.size() >= visit_max_nodes) break;
		if (cand.count < min_items) continue;
		selected.emplace_back(cand);
	}

	// Unless dropping the small ones would leave nothing to show, in which case the strongest
	// survive: a short trip is still the answer when it is the only answer.
	if (selected.size() < 3)
	{
		selected.clear();
		for (const auto& cand : candidates)
		{
			if (selected.size() >= visit_max_nodes) break;
			selected.emplace_back(cand);
		}
	}

	std::ranges::sort(selected, [](const candidate_t& l, const candidate_t& r)
	{
		if (l.first_days != r.first_days) return l.first_days < r.first_days;
		return l.last_days < r.last_days;
	});

	// Merge adjacent nodes of the same cluster that the selection pass left touching.
	for (size_t i = 1; i < selected.size();)
	{
		auto& prev = selected[i - 1];
		const auto& cur = selected[i];

		if (prev.cluster == cur.cluster &&
			static_cast<int>(cur.first_days - prev.last_days) < prev.gap_threshold)
		{
			// Both counts include their own first day and month, so a run that starts on the day
			// or in the month the previous one ended shares one. Adding the totals outright would
			// report more active days than the merged span contains, and the era test in 6.2
			// reads active_months as a fraction of the span.
			const auto shared_day = cur.first_days == prev.last_days ? 1u : 0u;
			const auto shared_month = cur.first_days / average_days_per_month ==
			                          prev.last_days / average_days_per_month
				                          ? 1u
				                          : 0u;

			prev.last_days = std::max(prev.last_days, cur.last_days);
			prev.count += cur.count;
			prev.active_days += cur.active_days - shared_day;
			prev.active_months += cur.active_months - shared_month;
			prev.score = std::max(prev.score, cur.score);
			selected.erase(selected.begin() + i);
		}
		else
		{
			++i;
		}
	}

	// 8. Publish.
	auto covered = 0u;
	for (const auto& cand : selected) covered += cand.count;

	result.nodes.reserve(selected.size());

	for (const auto& cand : selected)
	{
		const auto& cluster = clusters[cand.cluster];

		visit_node node;
		node.name = cluster.name;
		node.first = date_t::from_days(cand.first_days);
		node.last = date_t::from_days(cand.last_days);
		node.count = cand.count;
		node.active_days = cand.active_days;

		// The click searches from the point the name resolves to, so that is where the node sits and
		// what its radius is measured from.
		node.centre = cluster.anchor.is_valid() ? cluster.anchor : cluster.centre;
		node.named = cluster.named;
		node.score = cand.score;
		node.cluster = cand.cluster;
		node.kind = cand.is_era ? visit_kind::era : visit_kind::visit;

		// The radius the click will use has to cover every item the node counted, or the search
		// the node promises would return fewer items than the node displayed.
		auto furthest = 0.0;
		auto any_gps = false;

		for (const auto i : cluster.members)
		{
			const auto& s = samples[i];
			if (s.days < cand.first_days || s.days > cand.last_days) continue;
			if (!s.coordinate.is_valid() || !node.centre.is_valid()) continue;
			any_gps = true;
			furthest = std::max(furthest, node.centre.distance_in_kilometers(s.coordinate));
		}

		// A text-only cluster has no coordinates at all. Any radius at all would turn the click
		// into a radius search, and the matcher requires GPS for those, so the node would promise
		// a count and then return nothing. Zero means "match by name".
		node.radius_km = any_gps
			                 ? std::max(furthest,
			                            location_distance_at_detent(location_distance_detent_at_least(furthest)))
			                 : 0.0;
		result.nodes.emplace_back(std::move(node));
	}

	result.publish = result.nodes.size() >= 2 &&
		static_cast<double>(covered) >= located * visit_publish_coverage;

	return result;
}

std::string df::visit_node_dates(const visit_node& node)
{
	const auto first = node.first.date();
	const auto last = node.last.date();

	if (first.year != last.year)
	{
		return std::format("{}-{}", first.year, last.year);
	}

	if (first.month == last.month)
	{
		return std::format("{} {}", str::short_month(first.month, true), first.year);
	}

	return str::to_string(first.year);
}

static void apply_visit_location(df::search_t& search, const gps_coordinate centre, const std::string& name,
                                 const bool named, const double radius_km)
{
	search.clear_term_type(df::search_term_type::location);
	search.clear_term_type(df::search_term_type::area);

	if (named && !name.empty())
	{
		search.location(name, df::location_level::any).set_place_distance(radius_km);
	}
	else if (centre.is_valid())
	{
		// Nothing named the cluster, so the coordinates say exactly what was clicked.
		search.location(centre, radius_km);
	}
}

df::search_t df::visit_node_search(const search_t& current, const visit_node& node)
{
	auto search = current;
	apply_visit_location(search, node.centre, node.name, node.named, node.radius_km);

	search.clear_term_type(search_term_type::date);
	search.date_range(node.first.date(), node.last.date(), date_parts_prop::original);

	return search;
}

df::search_t df::visit_place_search(const search_t& current, const visit_place_tally& place)
{
	auto search = current;

	// The same terms a location group header emits, which is what makes the count on the chip and
	// the count after the click the same number.
	search.clear_term_type(search_term_type::location);
	search.clear_term_type(search_term_type::area);

	if (!is_empty(place.place)) search.location(place.place.sv(), location_level::place);
	if (!is_empty(place.state)) search.location(place.state.sv(), location_level::state);
	if (!is_empty(place.country)) search.location(place.country.sv(), location_level::country);

	return search;
}

std::string df::visit_place_detail(const visit_place_tally& place)
{
	return qualified_name(location_t{
		0, place.place, place.state, place.country, {}, 0.0,
		static_cast<uint32_t>(location_qualification::name_region_country)
	});
}

bool df::is_visit_node_selected(const search_t& current, const visit_node& node)
{
	auto dates = 0;
	auto place = false;

	for (const auto& t : current.terms())
	{
		if (t.type == search_term_type::date && t.date_val.target == date_parts_prop::original)
		{
			const auto bound = t.modifiers.greater_than ? node.first.date() : node.last.date();

			if (t.date_val.year == bound.year && t.date_val.month == bound.month && t.date_val.day == bound.day)
			{
				++dates;
			}
		}
		else if (t.type == search_term_type::location)
		{
			if (node.named) place = str::icmp(t.text, node.name) == 0;
			else place = t.coord_val.is_valid() && t.coord_val == node.centre;
		}
	}

	return place && dates == 2;
}
