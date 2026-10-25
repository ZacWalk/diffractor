// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The index summary and the vocabulary built from it: histograms, the location heat map
// and matrix, distinct words and property values, collection folder totals, and the auto-complete
// answers the address box asks for.

#include "pch.h"

#include "model_index.h"
#include "model_index_internal.h"
#include "model_locations.h"
#include "model_property.h"
#include "model.h"
#include "util_text.h"

static void add_words(df::dense_string_counts& distinct_words, strings_by_prop& distinct_text, const str::cached text,
                      const prop::key_ref key)
{
	count_ranges(distinct_words, text);
	distinct_text[key].emplace(text);
}

constexpr size_t max_tag_companion_tags_per_item = 128;

bool location_matrix_params::contains(const gps_coordinate coordinate) const
{
	return coordinate.is_valid() &&
		coordinate.latitude() >= min_latitude && coordinate.latitude() <= max_latitude &&
		coordinate.longitude() >= min_longitude && coordinate.longitude() <= max_longitude;
}

pointi location_matrix_params::cell(const gps_coordinate coordinate) const
{
	if (projection == location_matrix_projection::location_heat_map)
	{
		const auto location = df::location_heat_map::calc_map_loc(coordinate);
		const auto span = std::max(1, area_cell_span);
		return {location.x / span * span, location.y / span * span};
	}

	constexpr auto max_mercator_latitude = 85.05112878;
	const auto scale = std::pow(2.0, zoom) * 256.0 / std::max(1, cell_size);
	const auto latitude = std::clamp(coordinate.latitude(), -max_mercator_latitude, max_mercator_latitude);
	const auto latitude_radians = latitude * M_PI / 180.0;
	return {
		static_cast<int>(std::floor((coordinate.longitude() + 180.0) / 360.0 * scale)),
		static_cast<int>(std::floor((1.0 - std::asinh(std::tan(latitude_radians)) / M_PI) / 2.0 * scale))
	};
}

void location_matrix::add(df::file_path path, const gps_coordinate coordinate, const bool can_thumbnail,
                          const int rating)
{
	if (!params.contains(coordinate)) return;
	const auto index = params.cell(coordinate);
	const auto key = static_cast<uint64_t>(static_cast<uint32_t>(index.x)) << 32 | static_cast<uint32_t>(index.y);
	const uint8_t representative_rank = can_thumbnail ? (rating >= 4 ? 2 : 1) : 0;
	const auto found = _cell_lookup.find(key);
	if (found == _cell_lookup.end())
	{
		_cell_lookup.emplace(key, static_cast<uint32_t>(cells.size()));
		_representative_ranks.push_back(representative_rank);
		cells.push_back({
			index, std::move(path), {}, 1,
			coordinate.latitude(), coordinate.longitude(),
			coordinate.latitude(), coordinate.longitude(), coordinate.latitude(), coordinate.longitude()
		});
	}
	else
	{
		const auto cell_index = found->second;
		auto& cell = cells[cell_index];
		++cell.count;
		cell.latitude_sum += coordinate.latitude();
		cell.longitude_sum += coordinate.longitude();
		cell.min_latitude = std::min(cell.min_latitude, coordinate.latitude());
		cell.min_longitude = std::min(cell.min_longitude, coordinate.longitude());
		cell.max_latitude = std::max(cell.max_latitude, coordinate.latitude());
		cell.max_longitude = std::max(cell.max_longitude, coordinate.longitude());
		if (representative_rank > _representative_ranks[cell_index] ||
			(representative_rank == _representative_ranks[cell_index] && path.icmp(cell.representative_path) < 0))
		{
			_representative_ranks[cell_index] = representative_rank;
			cell.representative_path = std::move(path);
		}
	}
}

void location_matrix::finalize()
{
	for (auto& cell : cells)
	{
		cell.centroid = {cell.latitude_sum / cell.count, cell.longitude_sum / cell.count};
	}
	std::ranges::sort(cells, [](const cell& left, const cell& right)
	{
		return left.index.y == right.index.y ? left.index.x < right.index.x : left.index.y < right.index.y;
	});
	_cell_lookup.clear();
	_representative_ranks.clear();
}

location_matrix index_state::build_location_matrix(const location_matrix_params& params,
                                                   const df::unique_paths& excluded) const
{
	location_matrix result{params};

	for (const auto& [folder, folder_item] : _items.all_folders())
	{
		if (!folder_item->is_in_collection.load()) continue;

		for (const auto& file : folder_item->files)
		{
			const auto metadata = file.metadata.load();
			if (!metadata || !metadata->coordinate.is_valid()) continue;

			auto path = df::file_path(folder, file.name);
			const auto visual_media = file.ft &&
				(file.ft->has_trait(file_traits::bitmap) || file.ft->has_trait(file_traits::video_metadata));
			const auto can_thumbnail = visual_media && file.ft->has_trait(file_traits::thumbnail);
			if (!excluded.contains(path))
			{
				result.add(std::move(path), metadata->coordinate, can_thumbnail, metadata->rating);
			}
		}
	}

	result.finalize();
	return result;
}


void index_state::update_summary(const uint64_t generation)
{
	const auto start_ms = df::now_ms();

	{
		platform::shared_lock lock(_summary_rw);
		if (generation != 0 && generation != _summary_generation) return;
	}

	prop_text_summary distinct_labels;
	prop_num_summary distinct_ratings;
	prop_text_summary distinct_tags;
	tag_companion_counts tag_companions;

	df::dense_string_counts distinct_words;
	strings_by_prop distinct_text;
	df::unique_folders distinct_other_folders;
	size_t tag_companion_work = 0;

	index_histograms histograms;

	auto& distinct_tag_texts = distinct_text[prop::tag];
	const auto folders = _items.all_folders();
	auto files_until_currency_check = 256u;

	for (const auto& ifn : folders)
	{
		if (generation != 0)
		{
			platform::shared_lock lock(_summary_rw);
			if (generation != _summary_generation) return;
		}

		const auto is_indexed = ifn.second->is_in_collection.load();

		if (is_indexed)
		{
			for (const auto& file : ifn.second->files)
			{
				if (df::is_closing) return;
				if (generation != 0 && --files_until_currency_check == 0)
				{
					files_until_currency_check = 256;
					platform::shared_lock lock(_summary_rw);
					if (generation != _summary_generation) return;
				}

				const auto path = df::file_path(ifn.first, file.name);
				histograms.record(_locations, file, path);
				const auto md = file.metadata.load();

				if (md)
				{
					std::vector<std::string> companion_tags;
					split2(md->tags, true,
					       [&distinct_words, &distinct_tags, &distinct_tag_texts, &companion_tags, &file, &path](
					       const std::string_view part)
					       {
						       const auto cached_tag = str::cache(part);
						       if (companion_tags.size() < max_tag_companion_tags_per_item &&
							       std::ranges::none_of(companion_tags, [part](const auto& existing)
							       {
								       return str::icmp(existing, part) == 0;
							       }))
						       {
							       companion_tags.emplace_back(part);
						       }
						       distinct_tag_texts.emplace(cached_tag);
						       distinct_words[str::cache(std::format("#{}", part))] += 1;
						       distinct_tags[cached_tag].record(file, path);
					       });

					for (const auto& tag : companion_tags)
					{
						if (df::is_closing) return;
						for (const auto& companion : companion_tags)
						{
							if ((++tag_companion_work & 255u) == 0)
							{
								if (df::is_closing) return;
								if (generation != 0)
								{
									platform::shared_lock lock(_summary_rw);
									if (generation != _summary_generation) return;
								}
							}
							if (str::icmp(tag, companion) != 0)
							{
								auto& companions = tag_companions[tag];
								const auto cached_companion = str::cache(companion);
								auto found = companions.find(cached_companion);
								if (found == companions.end())
								{
									found = companions.emplace(cached_companion, df::int_counter{}).first;
								}
								++found->second;
							}
						}
					}

					if (!prop::is_null(md->album)) add_words(distinct_words, distinct_text, md->album, prop::album);
					if (!prop::is_null(md->album_artist))
						add_words(distinct_words, distinct_text, md->album_artist,
						          prop::album_artist);
					if (!prop::is_null(md->artist)) add_words(distinct_words, distinct_text, md->artist, prop::artist);
					if (!prop::is_null(md->audio_codec))
						add_words(distinct_words, distinct_text, md->audio_codec,
						          prop::audio_codec);
					if (!prop::is_null(md->bitrate))
						add_words(distinct_words, distinct_text, md->bitrate,
						          prop::bitrate);
					if (!prop::is_null(md->camera_manufacturer))
						add_words(
							distinct_words, distinct_text, md->camera_manufacturer, prop::camera_manufacturer);
					if (!prop::is_null(md->camera_model))
						add_words(distinct_words, distinct_text, md->camera_model,
						          prop::camera_model);
					if (!prop::is_null(md->comment))
						add_words(distinct_words, distinct_text, md->comment,
						          prop::comment);
					if (!prop::is_null(md->composer))
						add_words(distinct_words, distinct_text, md->composer,
						          prop::composer);
					if (!prop::is_null(md->copyright_creator))
						add_words(
							distinct_words, distinct_text, md->copyright_creator, prop::copyright_creator);
					if (!prop::is_null(md->copyright_credit))
						add_words(distinct_words, distinct_text,
						          md->copyright_credit, prop::copyright_credit);
					if (!prop::is_null(md->copyright_notice))
						add_words(distinct_words, distinct_text,
						          md->copyright_notice, prop::copyright_notice);
					if (!prop::is_null(md->copyright_source))
						add_words(distinct_words, distinct_text,
						          md->copyright_source, prop::copyright_source);
					if (!prop::is_null(md->copyright_url))
						add_words(distinct_words, distinct_text, md->copyright_url,
						          prop::copyright_url);
					if (!prop::is_null(md->description))
						add_words(distinct_words, distinct_text, md->description,
						          prop::description);
					if (!prop::is_null(md->encoder))
						add_words(distinct_words, distinct_text, md->encoder,
						          prop::encoder);
					if (!prop::is_null(md->file_name))
						add_words(distinct_words, distinct_text, md->file_name,
						          prop::file_name);
					if (!prop::is_null(md->genre))
					{
						// Genre is a single ';'-separated field; index each value
						// separately so the sidebar and autocomplete list them individually.
						count_ranges(distinct_words, md->genre);
						split2(md->genre, false, [&distinct_text](const std::string_view part)
						{
							const auto g = str::trim(part);
							if (!g.empty()) distinct_text[prop::genre].emplace(str::cache(g));
						}, str::is_genre_separator);
					}
					if (!prop::is_null(md->lens)) add_words(distinct_words, distinct_text, md->lens, prop::lens);
					if (!prop::is_null(md->location_place))
						add_words(distinct_words, distinct_text, md->location_place,
						          prop::location_place);
					if (!prop::is_null(md->location_country))
						add_words(distinct_words, distinct_text,
						          md->location_country, prop::location_country);
					if (!prop::is_null(md->location_state))
						add_words(distinct_words, distinct_text, md->location_state,
						          prop::location_state);
					if (!prop::is_null(md->performer))
						add_words(distinct_words, distinct_text, md->performer,
						          prop::performer);
					if (!prop::is_null(md->pixel_format))
						add_words(distinct_words, distinct_text, md->pixel_format,
						          prop::pixel_format);
					if (!prop::is_null(md->publisher))
						add_words(distinct_words, distinct_text, md->publisher,
						          prop::publisher);
					if (!prop::is_null(md->show)) add_words(distinct_words, distinct_text, md->show, prop::show);
					if (!prop::is_null(md->synopsis))
						add_words(distinct_words, distinct_text, md->synopsis,
						          prop::synopsis);
					if (!prop::is_null(md->title)) add_words(distinct_words, distinct_text, md->title, prop::title);
					if (!prop::is_null(md->video_codec))
						add_words(distinct_words, distinct_text, md->video_codec,
						          prop::video_codec);
					if (!prop::is_null(md->raw_file_name))
						add_words(distinct_words, distinct_text, md->raw_file_name,
						          prop::raw_file_name);

					if (!prop::is_null(md->label)) distinct_labels[md->label].record(file, path);

					auto r = md->rating;

					if (r >= -1 && r < 6 && r != 0)
					{
						if (r == -1) r = 0;
						distinct_ratings[r].record(file, path);
					}
				}
			}
		}

		if (!is_indexed)
		{
			distinct_other_folders.emplace(ifn.first);
		}
	}

	auto summary = std::make_shared<index_metadata_summary>();

	// This walk visits every indexed folder, so it is authoritative. Seeding from the previous
	// snapshot and only inserting would keep words from items that have since been deleted or
	// renamed, and grow without bound for as long as the session lasts.
	summary->_distinct_text = std::move(distinct_text);
	summary->_distinct_words = std::move(distinct_words);

	summary->_distinct_labels = std::move(distinct_labels);
	summary->_distinct_ratings = distinct_ratings;
	summary->_distinct_tags = std::move(distinct_tags);
	summary->_tag_companions = std::move(tag_companions);
	if (generation != 0)
	{
		platform::shared_lock lock(_summary_rw);
		if (generation != _summary_generation) return;
	}
	if (!rebuild_sorted_words(*summary, generation)) return;

	index_metadata_summary_const_ptr published_summary = std::move(summary);
	index_histograms_const_ptr histogram_snapshot = std::make_shared<index_histograms>(std::move(histograms));
	const auto scanned_other_folders = std::make_shared<const df::unique_folders>(std::move(distinct_other_folders));

	for (;;)
	{
		std::shared_ptr<const df::unique_folders> current_other_folders;
		{
			platform::shared_lock lock(_summary_rw);
			if (generation != 0 && generation != _summary_generation) return;
			current_other_folders = _summary._distinct_other_folders;
		}

		auto merged_other_folders = std::make_shared<df::unique_folders>(*scanned_other_folders);
		merged_other_folders->insert(current_other_folders->begin(), current_other_folders->end());
		std::shared_ptr<const df::unique_folders> other_folders_snapshot = std::move(merged_other_folders);

		{
			index_metadata_summary_const_ptr old_summary;
			index_histograms_const_ptr old_histograms;
			std::shared_ptr<const df::unique_folders> old_other_folders;
			platform::exclusive_lock lock(_summary_rw);
			if (generation != 0 && generation != _summary_generation) return;
			if (_summary._distinct_other_folders != current_other_folders) continue;
			_summary._metadata.swap(published_summary);
			_summary._histograms.swap(histogram_snapshot);
			_summary._distinct_other_folders.swap(other_folders_snapshot);
			old_summary = std::move(published_summary);
			old_histograms = std::move(histogram_snapshot);
			old_other_folders = std::move(other_folders_snapshot);
			break;
		}
	}

	_async.invalidate_view(view_invalid::sidebar | view_invalid::tooltip);

	df::trace(std::format("Index update summary in {} ms", df::now_ms() - start_ms));
}

void index_histograms::record(const location_cache&, const df::index_file_item& file, const df::file_path& path)
{
	const auto year = _year;
	constexpr auto map_width = static_cast<int>(df::location_heat_map::map_width);

	const auto md = file.metadata.load();
	auto created = file.file_created.system_to_local();

	if (md)
	{
		// One resolver: the timeline must bucket an item under the day it groups under.
		const auto resolved = md->created();
		if (resolved.is_valid()) created = resolved;

		const auto coord = md->coordinate;

		if (coord.is_valid())
		{
			const auto map_loc = df::location_heat_map::calc_map_loc(coord);
			const auto map_index = map_loc.y * map_width + map_loc.x;
			_locations.coordinates[map_index] += 1;
			_location_latitude_sums[map_index] += coord.latitude();
			_location_longitude_sums[map_index] += coord.longitude();
			_location_min_latitudes[map_index] = std::min(_location_min_latitudes[map_index], coord.latitude());
			_location_min_longitudes[map_index] = std::min(_location_min_longitudes[map_index], coord.longitude());
			_location_max_latitudes[map_index] = std::max(_location_max_latitudes[map_index], coord.latitude());
			_location_max_longitudes[map_index] = std::max(_location_max_longitudes[map_index], coord.longitude());

			const auto place_name = md->location_place;
			const auto state_name = md->location_state;
			const auto country_name = md->location_country;
			if (!str::is_empty(place_name) || !str::is_empty(state_name) || !str::is_empty(country_name))
			{
				const auto group_id = crypto::hash_gen(country_name.sv()).append(state_name.sv()).append(
					place_name.sv()).result();
				const auto found = _location_groups.find(group_id);

				if (found != _location_groups.end())
				{
					found->second.count += 1;
					found->second.latitude_sum += coord.latitude();
					found->second.longitude_sum += coord.longitude();
					found->second.loc = df::location_heat_map::calc_map_loc(found->second.centroid());
				}
				else
				{
					const auto name = !str::is_empty(place_name) ? place_name : country_name;
					_location_groups[group_id] = {
						name, state_name, country_name, 1, map_loc, coord.latitude(), coord.longitude()
					};
				}
			}
		}
	}

	auto& file_type = _file_types.counts[file.ft->group->id];
	file_type.count += 1;
	file_type.size += file.size;

	const auto created_date_parts = created.date();
	const auto created_year_offset = year - created_date_parts.year;

	if (created_year_offset >= 0 && created_year_offset < df::max_history_years)
	{
		const auto date_index = created_year_offset * 12 + created_date_parts.month - 1;
		_dates.dates[date_index].created += 1;
		_dates.record_representative(date_index, file, path);
	}

	const auto modified_date_parts = file.file_modified.load().system_to_local().date();
	const auto modified_date_parts_year_offset = year - modified_date_parts.year;

	if (modified_date_parts_year_offset >= 0 && modified_date_parts_year_offset < df::max_history_years)
	{
		// The cell draws the created count and clicks through on created, and there is one
		// representative per cell - letting the modified pass claim it hands the hover a picture the
		// click-through listing does not contain.
		const auto date_index = modified_date_parts_year_offset * 12 + modified_date_parts.month - 1;
		_dates.dates[date_index].modified += 1;
	}
}

std::vector<map_location_area> index_histograms::map_locations(const int requested_cell_span) const
{
	constexpr auto map_width = static_cast<int>(df::location_heat_map::map_width);
	const auto cell_span = std::clamp(std::bit_ceil(static_cast<unsigned>(std::max(requested_cell_span, 1))), 1u, 64u);

	struct area_build
	{
		uint32_t count = 0;
		double latitude_sum = 0.0;
		double longitude_sum = 0.0;
		double min_latitude = std::numeric_limits<double>::max();
		double min_longitude = std::numeric_limits<double>::max();
		double max_latitude = std::numeric_limits<double>::lowest();
		double max_longitude = std::numeric_limits<double>::lowest();
	};

	df::hash_map<uint32_t, area_build> builds;
	for (auto cell_index = 0u; cell_index < _locations.coordinates.size(); ++cell_index)
	{
		const auto count = _locations.coordinates[cell_index];
		if (count == 0) continue;
		const auto x = static_cast<int>(cell_index % map_width);
		const auto y = static_cast<int>(cell_index / map_width);
		const auto area_x = x / static_cast<int>(cell_span) * static_cast<int>(cell_span);
		const auto area_y = y / static_cast<int>(cell_span) * static_cast<int>(cell_span);
		const auto area_key = static_cast<uint32_t>(area_y * map_width + area_x);
		auto& area = builds[area_key];
		area.count += count;
		area.latitude_sum += _location_latitude_sums[cell_index];
		area.longitude_sum += _location_longitude_sums[cell_index];
		area.min_latitude = std::min(area.min_latitude, _location_min_latitudes[cell_index]);
		area.min_longitude = std::min(area.min_longitude, _location_min_longitudes[cell_index]);
		area.max_latitude = std::max(area.max_latitude, _location_max_latitudes[cell_index]);
		area.max_longitude = std::max(area.max_longitude, _location_max_longitudes[cell_index]);
	}

	std::vector<map_location_area> result;
	result.reserve(builds.size());
	for (const auto& [area_key, build] : builds)
	{
		const auto area_x = static_cast<int>(area_key % map_width);
		const auto area_y = static_cast<int>(area_key / map_width);
		result.push_back({
			.count = build.count,
			.cell = {area_x, area_y},
			.cell_span = static_cast<int>(cell_span),
			.position = {build.latitude_sum / build.count, build.longitude_sum / build.count},
			.min_latitude = build.min_latitude,
			.min_longitude = build.min_longitude,
			.max_latitude = build.max_latitude,
			.max_longitude = build.max_longitude
		});
	}

	std::ranges::sort(result, [](const auto& left, const auto& right)
	{
		return left.cell.y == right.cell.y ? left.cell.x < right.cell.x : left.cell.y < right.cell.y;
	});

	return result;
}

std::optional<map_location_area> index_histograms::find_map_location(const std::string_view name,
                                                                     const location_cache& locations,
                                                                     const gps_coordinate default_location) const
{
	std::vector<location_t> named_locations;
	for (const auto& match : locations.auto_complete(name, 32, default_location))
	{
		if (str::icmp(match.location.place, name) == 0)
		{
			named_locations.emplace_back(match.location);
		}
	}

	for (const auto cell_span : {1, 2, 4, 8, 16, 32, 64})
	{
		const auto areas = map_locations(cell_span);
		for (const auto& named_location : named_locations)
		{
			const auto map_cell = df::location_heat_map::calc_map_loc(named_location.position);
			const auto found = std::ranges::find_if(areas, [map_cell](const map_location_area& area)
			{
				return area.contains(map_cell);
			});
			if (found == areas.end()) continue;

			auto selected = locations.find_largest(found->min_latitude, found->min_longitude,
			                                       found->max_latitude, found->max_longitude);
			if (selected.id == 0)
			{
				selected = locations.find_closest(found->position.latitude(), found->position.longitude());
			}
			if (str::icmp(selected.place, name) == 0)
			{
				auto result = *found;
				result.name = selected.place;
				result.state = selected.state;
				result.country = selected.country;
				result.population = selected.population;
				return result;
			}
		}
	}

	return {};
}

std::vector<str::cached> index_state::distinct_genres() const
{
	index_metadata_summary_const_ptr summary;
	{
		platform::shared_lock lock(_summary_rw);
		summary = _summary._metadata;
	}
	const auto found = summary->_distinct_text.find(prop::genre);
	return found != summary->_distinct_text.end()
		       ? std::vector<str::cached>{found->second.begin(), found->second.end()}
		       : std::vector<str::cached>{};
}

std::vector<std::string> index_state::auto_complete_text(const prop::key_ref key)
{
	index_metadata_summary_const_ptr summary;
	{
		platform::shared_lock lock(_summary_rw);
		summary = _summary._metadata;
	}
	const auto found = summary->_distinct_text.find(key);
	return found != summary->_distinct_text.end()
		       ? std::vector<std::string>{found->second.begin(), found->second.end()}
		       : std::vector<std::string>{};
}

static df::count_and_size sum_items(const df::index_folder_info_const_ptr& folder)
{
	df::count_and_size result;

	// folders_snapshot() returns a temporary shared_ptr; dereferencing it into the range-for only
	// extends the pointee's lifetime through the initializer, not the loop body, so the snapshot is
	// bound to a name here to keep the vector alive for the whole loop. A concurrent replace_child()
	// dropping the last other reference mid-loop would otherwise free it out from under the recursion -
	// the observed crash (null index_folder_item read in folders_snapshot -> _Lock_and_load).
	const auto folders_snapshot = folder->folders_snapshot();

	for (const auto& sub_folder : *folders_snapshot)
	{
		result += sum_items(sub_folder);
	}

	for (const auto& file : folder->files)
	{
		result.size += file.size;
		++result.count;
	}

	return result;
}

std::vector<index_state::folder_total> index_state::includes_with_totals() const
{
	df::unique_folders includes;

	{
		platform::shared_lock lock(_summary_rw);
		includes = _summary._roots.folders;
	}

	std::vector<folder_total> results;
	results.reserve(includes.size());

	for (const auto& f : includes)
	{
		df::file_size size;
		uint64_t count = 0;
		const auto existing_folder = _items.find(f);

		if (existing_folder)
		{
			const auto sum_result = sum_items(existing_folder);
			size = sum_result.size;
			count = sum_result.count;
		}

		results.emplace_back(f, count, size);
	}

	return results;
}

// Rebuilds the case-insensitively-sorted vocabulary lookup before its immutable publication.
bool index_state::rebuild_sorted_words(index_metadata_summary& summary, const uint64_t generation) const
{
	auto& sorted = summary._sorted_words;
	sorted.clear();
	sorted.reserve(summary._distinct_words.size());

	for (const auto& kv : summary._distinct_words) sorted.emplace_back(kv.first);

	std::ranges::sort(sorted, [](const std::string_view a, const std::string_view b) { return str::icmp(a, b) < 0; });
	if (generation != 0)
	{
		platform::shared_lock lock(_summary_rw);
		if (generation != _summary_generation) return false;
	}

	// Trigram index over the vocabulary (term-id == index into _sorted_words). This accelerates
	// the per-keystroke *substring* prediction path, which would otherwise scan every term.
	summary._word_trigrams = df::trigram_index{};

	for (uint32_t i = 0; i < sorted.size(); ++i)
	{
		if (generation != 0 && (i & 255u) == 0)
		{
			platform::shared_lock lock(_summary_rw);
			if (generation != _summary_generation) return false;
		}
		summary._word_trigrams.add(i, sorted[i]);
	}

	summary._word_trigrams.freeze();

	return true;
}

std::vector<index_state::auto_complete_word> index_state::auto_complete_words(
	const std::string_view query, const size_t max_results)
{
	df::assert_true(!ui::is_ui_thread());

	std::vector<auto_complete_word> result;
	index_metadata_summary_const_ptr summary;
	{
		platform::shared_lock lock(_summary_rw);
		summary = _summary._metadata;
	}

	// Prefix matches first: a binary search over the sorted snapshot (O(log N + k)) rather
	// than a full scan of the vocabulary. Prefix hits are the strongest predictions and are
	// returned ahead of interior substring matches.
	const auto [prefix_first, prefix_last] = word_prefix_range(summary->_sorted_words, query);

	for (auto it = prefix_first; it != prefix_last && result.size() < max_results; ++it)
	{
		const auto count = summary->_distinct_words.find(*it);
		result.emplace_back(std::string(*it), std::vector<str::part_t>{{0, str::matched_text_byte_length(*it, 0, query)}},
		                    count == summary->_distinct_words.end() ? 0 : count->second.i);
	}

	// For longer queries, top up with interior substring matches the prefix pass cannot find,
	// skipping words already added as prefix matches. A single-token query (no spaces) matches
	// as a contiguous substring, so the vocabulary trigram index can supply the candidates and
	// we verify each with str::ifind2 - identical results to a full scan, but far fewer checks.
	// Multi-word queries use ifind2's word-gap semantics, which trigrams cannot bound, so those
	// (and queries too short for a trigram) fall back to a full scan.
	if (query.size() > 2 && result.size() < max_results)
	{
		// str::normalize_for_compare folds every whitespace form to a word gap, not just a space,
		// so any of them means the query has to take the full-scan path.
		const auto candidates = query.find_first_of(" \t\n\v\f\r") == std::string_view::npos
			                        ? summary->_word_trigrams.candidates(query)
			                        : std::nullopt;

		if (candidates)
		{
			for (const auto id : *candidates)
			{
				if (result.size() >= max_results) break;
				if (id >= summary->_sorted_words.size()) continue;

				const auto word = summary->_sorted_words[id];
				if (str::starts(word, query)) continue; // already added by the prefix pass

				const auto found = str::ifind2(word, query, 0);
				if (found.found)
				{
					const auto count = summary->_distinct_words.find(word);
					result.emplace_back(std::string(word), found.parts,
					                    count == summary->_distinct_words.end() ? 0 : count->second.i);
				}
			}
		}
		else
		{
			for (const auto& word : summary->_distinct_words)
			{
				if (result.size() >= max_results) break;
				if (str::starts(word.first, query)) continue; // already added by the prefix pass

				const auto found = str::ifind2(word.first, query, 0);
				if (found.found) result.emplace_back(std::string(word.first), found.parts, word.second.i);
			}
		}
	}

	return result;
}

std::vector<index_state::auto_complete_word> index_state::auto_complete_tag_companions(
	const std::vector<std::string>& tags, const std::string_view prefix, const size_t max_results) const
{
	std::vector<auto_complete_word> result;
	df::hash_map<std::string, int, df::ihash, df::ieq> scores;
	index_metadata_summary_const_ptr summary;
	{
		platform::shared_lock lock(_summary_rw);
		summary = _summary._metadata;
	}

	for (const auto& tag : tags)
	{
		const auto found = summary->_tag_companions.find(tag);
		if (found == summary->_tag_companions.end()) continue;

		for (const auto& [companion, count] : found->second)
		{
			if ((prefix.empty() || str::starts(companion, prefix)) &&
				std::ranges::none_of(tags, [&companion](const auto& existing)
				{
					return str::icmp(existing, companion) == 0;
				}))
			{
				scores[std::string(companion)] += count.i;
			}
		}
	}

	std::vector<std::pair<std::string, int>> ranked(scores.begin(), scores.end());
	std::ranges::sort(ranked, [](const auto& left, const auto& right)
	{
		return left.second == right.second
			       ? str::icmp(left.first, right.first) < 0
			       : left.second > right.second;
	});

	for (const auto& [tag, count] : ranked)
	{
		if (result.size() >= max_results) break;
		result.emplace_back(std::format("#{}", tag),
		                    prefix.empty() ? std::vector<str::part_t>{} : std::vector<str::part_t>{{1, prefix.size()}},
		                    count);
	}

	return result;
}

size_t index_state::tag_companion_entry_count() const
{
	index_metadata_summary_const_ptr summary;
	{
		platform::shared_lock lock(_summary_rw);
		summary = _summary._metadata;
	}

	size_t result = 0;
	for (const auto& [tag, companions] : summary->_tag_companions)
	{
		result += companions.size();
	}
	return result;
}

// locations.md 3.4: a completion the collection has no photos anywhere near is noise. The heat
// map is coordinate-based, so unlike the location groups it also counts GPS-only items that
// carry no place text -- which is the majority of a camera roll. One cell plus its neighbours is
// about the coarsest area a single place name can plausibly stand for.
static uint32_t collection_items_near(const df::location_heat_map& heat_map, const gps_coordinate coord)
{
	if (!coord.is_valid()) return 0;

	constexpr auto map_width = static_cast<int>(df::location_heat_map::map_width);
	constexpr auto map_height = static_cast<int>(df::location_heat_map::map_height);

	const auto cell = df::location_heat_map::calc_map_loc(coord);
	uint32_t total = 0;

	for (auto dy = -1; dy <= 1; ++dy)
	{
		const auto y = cell.y + dy;
		if (y < 0 || y >= map_height) continue;

		for (auto dx = -1; dx <= 1; ++dx)
		{
			// Longitude wraps, latitude does not.
			const auto x = ((cell.x + dx) % map_width + map_width) % map_width;
			total += heat_map.coordinates[y * map_width + x];
		}
	}

	return total;
}

// locations.md 3.4 + 3.5: a place completion commits the canonical location term, never the bare
// name. A bare name would run a text search over stored fields, which is exactly the empty result
// the one-location-vocabulary rule exists to remove.
std::vector<index_state::auto_complete_word> index_state::auto_complete_locations(
	const std::string_view query, const size_t max_results, const df::location_level level) const
{
	std::vector<auto_complete_word> result;
	if (query.empty()) return result;

	const auto summary = histograms();
	df::hash_set<std::string, df::ihash, df::ieq> seen;

	for (const auto& match : _locations.auto_complete(query, static_cast<uint32_t>(max_results * 4),
	                                                  setting.default_location))
	{
		std::string name;

		switch (level)
		{
		case df::location_level::state: name = std::string(match.location.state.sv());
			break;
		case df::location_level::country: name = std::string(match.location.country.sv());
			break;
		default: name = qualified_name(match.location);
			break;
		}

		if (name.empty() || !seen.emplace(name).second) continue;

		// A level-qualified scope collapses many gazetteer rows onto one label, so the row has to
		// have matched THAT label: "Frankfurt" is a fine hit for `loc:franc` and a wrong one for
		// `country:franc`, where the answer is France.
		if ((level == df::location_level::state || level == df::location_level::country) &&
			str::ifind(name, query) == std::string_view::npos)
		{
			continue;
		}

		auto text = df::search_t().location(name, level).text();
		std::vector<str::part_t> highlights;

		if (const auto found = str::ifind(text, query); found != std::string_view::npos)
		{
			highlights.emplace_back(found, str::matched_text_byte_length(text, found, query));
		}

		result.emplace_back(std::move(text), std::move(highlights),
		                    static_cast<int>(collection_items_near(summary->_locations, match.location.position)));
	}

	// A place the collection actually holds photos near is the one the user meant; the gazetteer
	// order (exact spelling, then population) decides the rest.
	std::ranges::stable_sort(result, [](const auto& left, const auto& right)
	{
		return left.occurrences > right.occurrences;
	});

	if (result.size() > max_results) result.resize(max_results);
	return result;
}

std::vector<index_state::auto_complete_folder> index_state::auto_complete_folders(
	const std::string_view query, const size_t max_results) const
{
	df::assert_true(!ui::is_ui_thread());

	std::vector<auto_complete_folder> result;

	// Retain the immutable folder snapshots under a shared lock, then match unlocked.
	std::array<std::shared_ptr<const df::unique_folders>, 3> folder_sets;

	{
		platform::shared_lock lock(_summary_rw);
		folder_sets[0] = _summary._distinct_prime_folders;
		folder_sets[1] = _summary._distinct_folders;
		folder_sets[2] = _summary._distinct_other_folders;
	}

	for (const auto& folders : folder_sets)
	{
		if (query.size() > 2)
		{
			for (const auto& folder : *folders)
			{
				auto name_pos = folder.is_root() ? 0u : folder.find_last_slash() + 1;
				if (name_pos == std::string_view::npos) name_pos = 0;
				auto found = str::ifind2(folder.text().substr(name_pos), query, name_pos);

				if (!found.found && name_pos != 0)
				{
					found = ifind2(folder.text(), query, 0);
				}

				if (found.found)
				{
					result.emplace_back(folder, found.parts);
					if (result.size() > max_results) break;
				}
			}
		}
		else
		{
			for (const auto& folder : *folders)
			{
				const auto name_pos = folder.is_root() ? 0u : folder.find_last_slash() + 1;

				if (name_pos != std::string_view::npos && str::starts(folder.text().substr(name_pos), query))
				{
					result.emplace_back(
						folder, std::vector<str::part_t>{{name_pos, str::matched_text_byte_length(folder.text(), name_pos, query)}});
					if (result.size() > max_results) break;
				}
				else if (starts(folder.text(), query))
				{
					result.emplace_back(folder, std::vector<str::part_t>{{0, str::matched_text_byte_length(folder.text(), 0, query)}});
					if (result.size() > max_results) break;
				}
			}
		}
	}

	return result;
}
