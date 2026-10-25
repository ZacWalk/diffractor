// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: File item representation. Defines item_element class for individual files,
// manages thumbnail caching, metadata display, and selection state.

#include "pch.h"

#include "model_items.h"

#include "ui_dialog.h"
#include "model.h"
#include "model_index.h"

namespace df
{
	std::function<void(file_path source, file_path destination)> rename_after_move_for_test;
	std::function<void(std::string_view message)> rename_recovery_report_for_test;
}
#include "ui_controllers.h"
#include "ui_controls.h"

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

df_assert_pod(df::group_key);
df_assert_pod(df::file_group_histogram);
df_assert_movable(df::item_set);

//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////

static df::group_key media_type_index(const df::item_element_ptr& i)
{
	df::group_key result;
	const auto ft = i->file_type();
	const auto group_key = ft->group->key;

	result.order1 = ft->group->id;
	result.type = group_key;
	result.text1 = str::cache(ft->display_name(true));
	result.group = ft->group;
	return result;
}

static df::group_key shuffle_index_key(const df::item_element_ptr& i)
{
	df::group_key result;
	result.type = group_key_type::grouped_value;
	return result;
}

static df::group_key extension_key(const df::item_element_ptr& i)
{
	df::group_key result;
	auto ext = i->extension();

	if (!ext.empty())
	{
		result.type = group_key_type::grouped_value;
		if (ext.front() == L'.') ext = ext.substr(1);
		result.text1 = str::cache(ext);
	}

	return result;
}

static df::group_key folder_key(const df::item_element_ptr& i)
{
	df::group_key result;
	result.type = group_key_type::grouped_value;

	if (i->is_folder())
	{
		result.text1 = i->folder().parent().text();
	}
	else
	{
		result.text1 = i->folder().text();
	}

	return result;
}

static df::group_key location_key(const df::item_element_ptr& i, const location_cache& locations,
                                  std::map<attribution_cell, located_place>& resolved_places)
{
	df::group_key result;
	const auto* const ft = i->file_type();
	if (ft->has_trait(file_traits::no_metadata_grouping)) return {ft->group->key};

	const auto md = i->metadata();

	if (md)
	{
		auto country = md->location_country;
		auto state = md->location_state;
		auto place = md->location_place;

		if (md->coordinate.is_valid() && (is_empty(country) || is_empty(state) || is_empty(place)))
		{
			// locations.md 2.5: bounded attribution. A place too far away to be a truthful
			// answer contributes nothing, so a remote item groups by country or not at all.
			const attribution_cell cell(md->coordinate);
			const auto found = resolved_places.find(cell);
			const auto resolved = found != resolved_places.end()
				                      ? found->second
				                      : locations.find_attributed(md->coordinate);

			if (found == resolved_places.end() && locations.is_index_loaded())
			{
				resolved_places.emplace(cell, resolved);
			}

			if (is_empty(country)) country = resolved.place.country;
			if (is_empty(state)) state = resolved.place.state;
			if (is_empty(place)) place = resolved.place.place;
		}

		if (!is_empty(country) || !is_empty(state) || !is_empty(place))
		{
			result.type = group_key_type::grouped_value;
			result.text1 = normalize_county_name(country);
			result.text2 = state;
			result.text3 = place;
		}
	}

	return result;
}

static df::group_key camera_key(const df::item_element_ptr& i)
{
	df::group_key result;
	const auto* const ft = i->file_type();
	if (ft->has_trait(file_traits::no_metadata_grouping)) return {ft->group->key};

	const auto md = i->metadata();

	if (md && !is_empty(md->camera_manufacturer))
	{
		result.type = group_key_type::grouped_value;
		result.text1 = md->camera_manufacturer;
		result.text2 = md->camera_model;
	}

	return result;
}

static df::group_key album_show_key(const df::item_element_ptr& i)
{
	df::group_key result;
	const auto* const ft = i->file_type();
	if (ft->has_trait(file_traits::no_metadata_grouping)) return {ft->group->key};

	const auto md = i->metadata();

	if (md)
	{
		if (!is_empty(md->show))
		{
			result.type = group_key_type::grouped_value;
			result.text1 = md->show;
			result.text1_prop_type = prop::show;

			if (md->season != 0)
			{
				result.order3 = md->season;
			}
		}
		else if (!is_empty(md->album_artist) || !is_empty(md->artist) || !is_empty(md->album))
		{
			result.type = group_key_type::grouped_value;

			if (!is_empty(md->album_artist))
			{
				result.text1 = md->album_artist;
				result.text1_prop_type = prop::album_artist;
			}
			else if (!is_empty(md->artist))
			{
				result.text1 = md->artist;
				result.text1_prop_type = prop::artist;
			}

			if (!is_empty(md->album))
			{
				result.text2 = md->album;
			}
		}
	}

	return result;
}

static std::string_view related_axis_text(const df::related_axis axis)
{
	switch (axis)
	{
	case df::related_axis::album: return tt.related_group_album;
	case df::related_axis::series: return tt.related_group_series;
	case df::related_axis::time: return tt.related_group_time;
	case df::related_axis::location: return tt.related_group_location;
	default: return tt.related_group_duplicates;
	}
}

static icon_index related_axis_icon(const df::related_axis axis)
{
	switch (axis)
	{
	case df::related_axis::album: return icon_index::audio;
	case df::related_axis::series: return icon_index::video;
	case df::related_axis::time: return icon_index::time;
	case df::related_axis::location: return icon_index::location;
	default: return icon_index::compare;
	}
}

static df::group_key related_key(const df::item_element_ptr& i)
{
	const auto axis = df::related_axis_of(i->search().type);

	df::group_key result;
	result.type = group_key_type::grouped_value;
	result.order1 = static_cast<int32_t>(axis);
	result.text1 = str::cache(related_axis_text(axis));
	result.icon = related_axis_icon(axis);
	return result;
}

static df::group_key size_key(const df::item_element_ptr& i)
{
	df::group_key result;
	const auto size_bucket = prop::size_bucket(i->file_size().to_int64());
	if (size_bucket == 0) return {group_key_type::grouped_no_value};
	result.order1 = df::round(std::sqrt(size_bucket));
	result.type = group_key_type::grouped_value;
	return result;
}

static constexpr auto rating_base_num = 6;
static constexpr auto rating_reject_num = 7;

static df::group_key rating_key(const df::item_element_ptr& i)
{
	df::group_key result;
	const auto* const ft = i->file_type();
	if (ft->has_trait(file_traits::no_metadata_grouping)) return {ft->group->key};

	const auto md = i->metadata();

	if (md)
	{
		if (md->rating != 0)
		{
			result.type = group_key_type::grouped_value;

			if (md->rating == -1)
			{
				result.order1 = rating_reject_num;
			}
			else
			{
				result.order1 = rating_base_num - md->rating;
			}
		}

		if (!is_empty(md->label))
		{
			result.text1 = md->label;
			result.type = group_key_type::grouped_value;
		}
	}

	return result;
}

static df::group_key date_key(const df::date_t when, const df::item_element_ptr& i)
{
	df::group_key result;
	if (i->is_folder()) return {group_key_type::folder};
	result.type = group_key_type::grouped_value;

	if (when.is_valid())
	{
		const auto day = when.to_days();
		const auto today = platform::now().to_days();

		if (day == today)
		{
			result.order1 = 0;
		}
		else if (day == today - 1)
		{
			result.order1 = 1;
		}
		else
		{
			const auto date = when.date();
			result.order1 = 3000 - date.year;
			result.order2 = 100 - date.month;
		}
	}

	return result;
}

static df::group_key resolution_key(const df::item_element_ptr& i)
{
	df::group_key result;
	const auto* const ft = i->file_type();
	if (ft->has_trait(file_traits::no_metadata_grouping)) return {ft->group->key};

	const auto md = i->metadata();

	if (md && md->width > 0 && md->height > 0)
	{
		const auto extent = sizei{md->width, md->height};
		const auto display_name = ft->group->display_name(true);

		if (ft->has_trait(file_traits::av))
		{
			const auto video_res = prop::format_video_resolution(extent);

			if (!video_res.empty())
			{
				result.type = group_key_type::grouped_value;
				result.order1 = ft->group->id;
				result.order2 = 1 + df::round(std::sqrt(extent.cx * extent.cy));
				result.text1 = str::cache(display_name);
				result.text2 = str::cache(video_res);
				return result;
			}

			result.type = group_key_type::grouped_value;
			result.order1 = ft->group->id;
			result.order2 = 1 + df::round(std::sqrt(extent.cx * extent.cy));
			result.text1 = str::cache(display_name);
			result.text2 = str::cache(std::format("{}x{}", extent.cx, extent.cy));
		}
		else
		{
			if (extent.cx <= 128 && extent.cy <= 128)
			{
				result.type = group_key_type::grouped_value;
				result.order1 = 1;
				result.order2 = 0;
				result.text1 = str::cache(display_name);
				result.text2 = "icon size"_c;
			}
			else
			{
				const auto mp = ui::calc_mega_pixels(extent.cx, extent.cy);

				if (mp < 0.9)
				{
					result.type = group_key_type::grouped_value;
					result.order1 = 1;
					result.order2 = 1;
					result.text1 = str::cache(display_name);
					result.text2 = "small size"_c;
				}
				else
				{
					const auto n = prop::exp_round(mp);
					result.type = group_key_type::grouped_value;
					result.order1 = 1;
					result.order2 = 1 + df::round(std::sqrt(n));
					result.text1 = str::cache(display_name);
					result.text2 = str::cache(std::format("{} megapixels", n));
				}
			}
		}
	}

	return result;
}

aspect_ratio_group calc_aspect_ratio_group(const sizei dimensions)
{
	struct ratio_bucket
	{
		double ratio;
		aspect_ratio_bucket bucket;
	};

	constexpr ratio_bucket buckets[] = {
		{1.0, aspect_ratio_bucket::square},
		{5.0 / 4.0, aspect_ratio_bucket::five_four},
		{4.0 / 3.0, aspect_ratio_bucket::four_three},
		{3.0 / 2.0, aspect_ratio_bucket::three_two},
		{16.0 / 10.0, aspect_ratio_bucket::sixteen_ten},
		{16.0 / 9.0, aspect_ratio_bucket::sixteen_nine},
		{21.0 / 9.0, aspect_ratio_bucket::twenty_one_nine},
	};

	if (dimensions.cx <= 0 || dimensions.cy <= 0) return {};

	const auto ratio = std::max(dimensions.cx, dimensions.cy) /
		static_cast<double>(std::min(dimensions.cx, dimensions.cy));
	for (const auto& bucket : buckets)
	{
		const auto difference = ratio > bucket.ratio ? ratio - bucket.ratio : bucket.ratio - ratio;
		if (difference / bucket.ratio <= 0.03) return {bucket.bucket, dimensions.cy > dimensions.cx};
	}

	return {aspect_ratio_bucket::other, dimensions.cy > dimensions.cx};
}

static df::group_key aspect_ratio_key(const df::item_element_ptr& i)
{
	const auto* const ft = i->file_type();
	if (ft->has_trait(file_traits::no_metadata_grouping)) return {ft->group->key};

	const auto md = i->metadata();
	if (!md || md->width <= 0 || md->height <= 0) return {};

	struct ratio_label
	{
		aspect_ratio_bucket bucket;
		int order;
		std::string_view landscape;
		std::string_view portrait;
	};

	static constexpr ratio_label labels[] = {
		{aspect_ratio_bucket::square, 1000, "1:1", "1:1"},
		{aspect_ratio_bucket::five_four, 1250, "5:4", "4:5"},
		{aspect_ratio_bucket::four_three, 1333, "4:3", "3:4"},
		{aspect_ratio_bucket::three_two, 1500, "3:2", "2:3"},
		{aspect_ratio_bucket::sixteen_ten, 1600, "16:10", "10:16"},
		{aspect_ratio_bucket::sixteen_nine, 1778, "16:9", "9:16"},
		{aspect_ratio_bucket::twenty_one_nine, 2333, "21:9", "9:21"},
	};

	const auto aspect_group = calc_aspect_ratio_group({md->width, md->height});

	df::group_key result;
	result.type = group_key_type::grouped_value;
	result.order1 = aspect_group.is_portrait ? 1 : 0;
	result.order2 = 10000;

	for (const auto& label : labels)
	{
		if (aspect_group.bucket == label.bucket)
		{
			result.order2 = label.order;
			result.text1 = str::cache(aspect_group.is_portrait ? label.portrait : label.landscape);
			return result;
		}
	}

	result.text1 = str::cache(aspect_group.is_portrait
		                          ? tt.aspect_ratio_other_portrait.sv()
		                          : tt.aspect_ratio_other_landscape.sv());
	return result;
}

class toggle_item_display_element final : public std::enable_shared_from_this<toggle_item_display_element>,
                                          public view_element
{
	df::weak_item_group_ptr _parent;
	const icon_index _icon = icon_index::details;

public:
	toggle_item_display_element(df::weak_item_group_ptr parent) noexcept : view_element(
		                                                                       flex_item::right_justified |
		                                                                       view_element_style::has_tooltip |
		                                                                       view_element_style::can_invoke),
	                                                                       _parent(std::move(parent))
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		render_background(dc, element_offset);
		const auto clr = ui::color(link_foreground_color(), dc.colors.alpha);
		xdraw_icon(dc, _icon, bounds.offset(element_offset), clr, {});
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			const auto p = _parent.lock();
			if (p) p->toggle_display();
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(std::make_shared<text_element>(tt.tooltip_toggle_details_selected));
		hover.window_bounds = hover.active_bounds = bounds.offset(element_offset);
	}
};

static void sort_items(df::item_elements& items, const group_by group_mode, const sort_by sort_order,
                       const bool group_by_dups)
{
	constexpr auto size_sorter = [](const df::item_element_ptr& l, const df::item_element_ptr& r) -> bool
	{
		const auto ll = l->file_size();
		const auto rr = r->file_size();
		return ll == rr ? icmp(l->name(), r->name()) < 0 : ll > rr;
	};

	constexpr auto modified_sorter = [](const df::item_element_ptr& l, const df::item_element_ptr& r)
	{
		if (l->file_modified() == r->file_modified()) return icmp(l->name(), r->name()) < 0;
		return l->file_modified() > r->file_modified();
	};

	constexpr auto created_sorter = [](const df::item_element_ptr& l, const df::item_element_ptr& r)
	{
		if (l->media_created() == r->media_created()) return icmp(l->name(), r->name()) < 0;
		return l->media_created() > r->media_created();
	};

	constexpr auto file_created_sorter = [](const df::item_element_ptr& l, const df::item_element_ptr& r)
	{
		const auto ll = l->file_or_metadata_created();
		const auto rr = r->file_or_metadata_created();
		if (ll == rr) return icmp(l->name(), r->name()) < 0;
		return ll > rr;
	};

	constexpr auto pixel_sorter = [](const df::item_element_ptr& l, const df::item_element_ptr& r) -> bool
	{
		const auto lmd = l->metadata();
		const auto rmd = r->metadata();
		const auto ll = lmd ? ui::calc_mega_pixels(lmd->width, lmd->height) : 0;
		const auto rr = rmd ? ui::calc_mega_pixels(rmd->width, rmd->height) : 0;
		return ll == rr ? icmp(l->name(), r->name()) < 0 : ll > rr;
	};

	constexpr auto group_sorter = [](const df::item_element_ptr& l, const df::item_element_ptr& r)
	{
		const auto ll = l->duplicates().group;
		const auto rr = r->duplicates().group;
		if (ll != rr) return ll < rr;
		return icmp(l->name(), r->name()) < 0;
	};

	constexpr auto name_sorter = [](const df::item_element_ptr& l, const df::item_element_ptr& r)
	{
		return str::icmp_natural(l->name(), r->name()) < 0;
	};

	// Inside a relation group the order is closeness to the item the search started at.
	constexpr auto related_sorter = [](const df::item_element_ptr& l, const df::item_element_ptr& r)
	{
		const auto ll = l->search().distance;
		const auto rr = r->search().distance;
		if (ll != rr) return ll < rr;
		return str::icmp_natural(l->name(), r->name()) < 0;
	};

	const auto sorts_by_date = group_mode != group_by::related &&
		(sort_order == sort_by::date_created || sort_order == sort_by::date_modified ||
			sort_order == sort_by::date_original ||
			(sort_order == sort_by::def && (group_mode == group_by::date_created || group_mode ==
				group_by::date_modified || group_mode == group_by::date_original)));
	const auto reverse_after = !setting.sort_dates_descending && sorts_by_date;

	// Regrouping re-sorts every group even when nothing that affects order moved, and these comparators
	// run case-insensitive natural string compares.
	const auto apply = [&items, reverse_after](auto&& pred)
	{
		if (reverse_after)
		{
			const auto descending = [&pred](const df::item_element_ptr& l, const df::item_element_ptr& r)
			{
				return pred(r, l);
			};

			if (!std::ranges::is_sorted(items, descending))
			{
				std::ranges::sort(items, pred);
				std::ranges::reverse(items);
			}
		}
		else if (!std::ranges::is_sorted(items, pred))
		{
			std::ranges::sort(items, pred);
		}
	};

	if (group_mode == group_by::shuffle)
	{
		apply([](const df::item_element_ptr& l, const df::item_element_ptr& r)
		{
			return l->random() < r->random();
		});
	}
	else if (group_mode == group_by::related)
	{
		apply(related_sorter);
	}
	else if (sort_order == sort_by::size)
	{
		apply(size_sorter);
	}
	else if (sort_order == sort_by::date_modified)
	{
		apply(modified_sorter);
	}
	else if (sort_order == sort_by::date_created)
	{
		apply(file_created_sorter);
	}
	else if (sort_order == sort_by::date_original)
	{
		apply(created_sorter);
	}
	else if (sort_order == sort_by::name)
	{
		apply(name_sorter);
	}
	else if (group_mode == group_by::size)
	{
		apply(size_sorter);
	}
	else if (group_mode == group_by::date_modified)
	{
		apply(modified_sorter);
	}
	else if (group_mode == group_by::date_created)
	{
		apply(file_created_sorter);
	}
	else if (group_mode == group_by::date_original)
	{
		apply(created_sorter);
	}
	else if (group_mode == group_by::resolution)
	{
		apply(pixel_sorter);
	}
	else if (group_mode == group_by::aspect_ratio)
	{
		apply(name_sorter);
	}
	else if (group_mode == group_by::folder)
	{
		apply(name_sorter);
	}
	else if (group_by_dups)
	{
		apply(group_sorter);
	}
	else
	{
		apply(name_sorter);
	}
}


void df::item_group::sort(const group_by group_mode, const sort_by sort_order, const bool group_by_dups)
{
	sort_items(_items, group_mode, sort_order, group_by_dups);

	auto last_dup_group = 0u;
	auto alt_background = false;

	for (const auto& i : _items)
	{
		const auto dup_group = i->duplicates().group;
		if (group_by_dups && last_dup_group != dup_group) alt_background = !alt_background;
		i->alt_background = alt_background;
		last_dup_group = dup_group;
	}

	_show_folder = group_by_dups || _state.search().has_related();
}

//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////

df::file_group_histogram df::item_set::summary() const
{
	file_group_histogram result;

	for (const auto& i : _items)
	{
		if (i->is_folder())
		{
			result.record(i->info());
		}
		else
		{
			result.record(i->file_type(), i->file_size());
		}
	}

	return result;
}

df::history_range df::history_auto_range(const date_histogram& dates, const int current_year)
{
	const auto window = history_range{current_year - history_window_years + 1, current_year};

	std::array<uint64_t, max_history_years> yearly{};
	uint64_t total = 0;

	for (auto y = 0; y < max_history_years; ++y)
	{
		uint64_t sum = 0;
		for (auto m = 0; m < 12; ++m) sum += dates.dates[static_cast<size_t>(y) * 12 + m].created;
		yearly[y] = sum;
		total += sum;
	}

	if (total == 0) return window;

	// How much of the collection sits at or beyond each year, counting back from today. Both trims
	// below are questions about a tail, and this answers them without rescanning.
	std::array<uint64_t, max_history_years + 1> beyond{};
	for (auto y = max_history_years - 1; y >= 0; --y) beyond[y] = beyond[y + 1] + yearly[y];

	// The shortest run back from today that still holds nearly everything. A scanner that stamped
	// 1900 on four negatives does not get to set the scale for twenty thousand photographs.
	const auto keep = total * history_coverage_percent / 100;
	uint64_t running = 0;
	auto oldest = 0;

	for (auto y = 0; y < max_history_years; ++y)
	{
		if (yearly[y] == 0) continue;
		running += yearly[y];
		oldest = y;
		if (running >= keep) break;
	}

	// Real history thins out; it does not fall silent for six years and then resume. An island
	// behind a long gap, holding only a sliver of the collection, is a camera whose clock reset.
	auto gap = 0;

	for (auto y = 1; y <= oldest; ++y)
	{
		if (yearly[y] == 0)
		{
			++gap;
			continue;
		}

		if (gap >= history_gap_years && beyond[y] * 100 < total * history_island_percent)
		{
			oldest = y - gap - 1;
			break;
		}

		gap = 0;
	}

	// Never narrower than one window, or the navigator has nothing to navigate.
	return {std::min(current_year - oldest, window.first_year), current_year};
}


//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////

df::item_set_info df::item_set::info() const
{
	item_set_info result;

	for (const auto& i : _items)
	{
		const auto md = i->metadata();

		if (md)
		{
			if (i->sidecars_count() > 0) result.sidecars += 1;

			if (i->is_media())
			{
				if (md->tags.is_empty()) result.untagged += 1;
				if (str::is_empty(md->location_country)) result.unlocated += 1;
				if (md->rating == 0) result.unrated += 1;
				if (str::is_empty(md->copyright_credit)) result.uncredited += 1;
			}
		}

		if (i->duplicates().count > 1)
		{
			result.duplicates += 1;
		}

		if (i->rating() == -1) result.rejected += 1;
	}

	return result;
}


std::string_view item_presence_text(const item_presence v, const bool long_text)
{
	if (v == item_presence::this_in)
	{
		return long_text ? tt.presence_this_in_long : tt.presence_this_in;
	}
	if (v == item_presence::similar_in)
	{
		return long_text ? tt.presence_similar_in_long : tt.presence_similar_in;
	}
	if (v == item_presence::not_in)
	{
		return long_text ? tt.presence_not_in_long : tt.presence_not_in;
	}
	if (v == item_presence::newer_in)
	{
		return long_text ? tt.presence_newer_in_long : tt.presence_newer_in;
	}
	if (v == item_presence::older_in)
	{
		return long_text ? tt.presence_older_in_long : tt.presence_older_in;
	}
	return tt.presence_loading;
}

static df::group_key presence_key(const item_presence& pr)
{
	df::group_key result;
	result.order1 = static_cast<int>(pr);
	result.type = group_key_type::grouped_value;

	switch (pr)
	{
	case item_presence::this_in:
		result.text1 = str::cache(tt.presence_this_in);
		result.icon = icon_index::check;
		break;
	case item_presence::similar_in:
		result.text1 = str::cache(tt.presence_similar_in);
		result.icon = icon_index::compare;
		break;
	case item_presence::not_in:
		result.text1 = str::cache(tt.presence_not_in);
		result.icon = icon_index::del;
		break;
	case item_presence::newer_in:
		result.text1 = str::cache(tt.presence_newer_in);
		result.icon = icon_index::add2;
		break;
	case item_presence::older_in:
		result.text1 = str::cache(tt.presence_older_in);
		result.icon = icon_index::remove2;
		break;
	default:
		result.text1 = str::cache(tt.presence_loading);
		result.icon = icon_index::unknown;
		break;
	}

	return result;
}

static int calc_item_line_height()
{
	return setting.item_scale_dimension();
}

std::shared_ptr<group_title_control> df::build_group_title(view_state& s, const view_host_base_ptr& view,
                                                           const item_group_ptr& g)
{
	assert_true(ui::is_ui_thread());

	const auto order = s.effective_group_order();
	const auto generation = s.group_title_generation();

	// A size title reads the current file sizes of the group, so it cannot be reused.
	const auto can_cache = order != group_by::size;

	if (can_cache && g->_title && g->_title_generation == generation)
	{
		return g->_title;
	}

	auto result = std::make_shared<group_title_control>();
	constexpr auto font = ui::style::font_face::title;

	g->_scroll_tooltip_text.clear();

	// The title is cached on the group, and the group outlives the view, so a strong capture here
	// would pin the view host and everything it owns.
	const auto add_title_link = [g, &s, weak_view = std::weak_ptr(view), result, font](
		const std::string_view title, const search_t& search)
	{
		if (search.is_empty())
		{
			const auto link = std::make_shared<text_element>(title, font, ui::style::text_style::multiline,
			                                                 view_element_style::none);
			link->margin(4);
			link->padding(4);
			result->elements.emplace_back(link);
		}
		else
		{
			const auto command = [&s, weak_view, search]
			{
				if (const auto host = weak_view.lock()) s.open(host, search, {});
			};

			auto tooltip = [search](const view_hover_element& popup)
			{
				popup.elements->add(make_icon_element(search.first_type()->icon, flex_item::no_break));
				popup.elements->add(std::make_shared<text_element>(search.text()));
			};

			const auto link = std::make_shared<link_element>(title, command, tooltip, font,
			                                                 ui::style::text_style::multiline,
			                                                 view_element_style::none);
			link->margin(4);
			link->padding(4);
			result->elements.emplace_back(link);
		}

		if constexpr (font == ui::style::font_face::title)
		{
			g->_scroll_tooltip_text.emplace_back(title);
		}
	};

	const auto current_search = s.search();
	const auto& key = g->_key;

	if (key.type == group_key_type::grouped_no_value)
	{
		if (order == group_by::extension)
		{
			add_title_link(tt.group_title_no_extension, search_t(current_search).without_extension());
		}
		else if (order == group_by::location)
		{
			add_title_link(tt.group_title_no_location, search_t(current_search).without(prop::location_country));
		}
		else if (order == group_by::rating_label)
		{
			add_title_link(tt.group_title_no_rating, search_t(current_search).without(prop::rating));
		}
		else if (order == group_by::resolution)
		{
			add_title_link(tt.group_title_no_resolution, search_t(current_search));
		}
		else if (order == group_by::aspect_ratio)
		{
			add_title_link(tt.group_title_no_aspect_ratio, search_t(current_search));
		}
		else if (order == group_by::camera)
		{
			add_title_link(tt.group_title_no_camera, search_t(current_search).without(prop::camera_model));
		}
		else if (order == group_by::album_show)
		{
			add_title_link(tt.group_title_no_album_or_show, {});
		}
		else if (order == group_by::folder)
		{
			add_title_link(tt.folder_title, {});
		}
		else
		{
			add_title_link(tt.group_title_no_value, search_t(current_search).without(prop::album));
		}
	}
	else if (key.type == group_key_type::grouped_value)
	{
		if (order == group_by::shuffle)
		{
			add_title_link(tt.group_title_shuffle, {});
		}
		else if (order == group_by::size)
		{
			file_size min_size;
			file_size max_size;

			if (!g->_items.empty())
			{
				min_size = g->_items[0]->file_size();
				max_size = g->_items[0]->file_size();

				for (const auto& i : g->_items)
				{
					const auto fs = i->file_size();
					if (min_size > fs) min_size = fs;
					if (max_size < fs) max_size = fs;
				}

				if (min_size == max_size)
				{
					add_title_link(df::format_size(min_size), {});
				}
				else
				{
					const auto text = str_format(tt.group_title_size_range_fmt.sv(), df::format_size(min_size),
					                             df::format_size(max_size));
					add_title_link(text, {});
				}
			}
		}
		else if (order == group_by::extension)
		{
			if (!is_empty(key.text1))
			{
				add_title_link(key.text1, search_t(current_search).with_extension(key.text1));
			}
			else
			{
				add_title_link(tt.group_title_no_extension, search_t(current_search).without_extension());
			}
		}
		else if (order == group_by::location)
		{
			// locations.md 3.8, baseline defect 7: a header must reproduce its own group. The
			// breadcrumb links carry their parents, so clicking `London` under `United Kingdom`
			// returns that group and not the other London as well.
			if (!is_empty(key.text1))
			{
				add_title_link(key.text1, search_t(current_search).location(key.text1, location_level::country));
			}

			if (!is_empty(key.text2))
			{
				auto search = search_t(current_search).location(key.text2, location_level::state);
				if (!is_empty(key.text1)) search.location(key.text1, location_level::country);
				add_title_link(key.text2, search);
			}

			if (!is_empty(key.text3))
			{
				auto search = search_t(current_search).location(key.text3, location_level::place);

				if (!is_empty(key.text2)) search.location(key.text2, location_level::state);
				else if (!is_empty(key.text1)) search.location(key.text1, location_level::country);

				add_title_link(key.text3, search);
			}
		}
		else if (order == group_by::rating_label)
		{
			if (rating_reject_num == key.order1)
			{
				add_title_link(tt.command_rate_rejected, search_t(current_search).with(prop::rating, -1));
			}
			else if (key.order1 != 0)
			{
				const int32_t rating = rating_base_num - key.order1;

				if (rating != 0)
				{
					const auto search = search_t(current_search).with(prop::rating, rating);
					const auto command = [&s, view, search] { s.open(view, search, {}); };

					auto tooltip = [rating](const view_hover_element& popup)
					{
						popup.elements->add(
							make_icon_element(icon_index::star_solid, rating, flex_item::line_break));
					};

					const auto link = make_icon_link_element(icon_index::star_solid, rating, command, tooltip);
					link->margin(4);
					link->padding(4);
					result->elements.emplace_back(link);

					g->_scroll_tooltip_rating = rating;
				}
			}

			if (!is_empty(key.text1))
			{
				add_title_link(tt.translate_text(key.text1.sz()),
				               search_t(current_search).with(prop::label, key.text1));
			}
		}
		else if (order == group_by::date_created || order == group_by::date_modified ||
			order == group_by::date_original)
		{
			auto target = date_parts_prop::any;
			if (order == group_by::date_modified) target = date_parts_prop::modified;
			if (order == group_by::date_created) target = date_parts_prop::created;
			if (order == group_by::date_original) target = date_parts_prop::original;

			if (key.order1 == 0)
			{
				const auto today = platform::now().date();
				add_title_link(tt.group_title_today,
				               search_t(current_search).day(today.day, today.month, today.year, target));
			}
			else if (key.order1 == 1)
			{
				const auto yesterday = platform::now().previous_day().date();
				add_title_link(tt.group_title_yesterday,
				               search_t(current_search).day(yesterday.day, yesterday.month, yesterday.year, target));
			}
			else
			{
				const auto month = 100 - key.order2;
				const auto year = 3000 - key.order1;

				add_title_link(std::format("{} {}", str::month(month, true), year),
				               search_t(current_search).day(0, month, year, target));
			}
		}
		else if (order == group_by::resolution)
		{
			if (!is_empty(key.text1) && !is_empty(key.text2))
			{
				add_title_link(std::format("{}:{}", key.text1, key.text2), {});
			}
		}
		else if (order == group_by::aspect_ratio)
		{
			add_title_link(key.text1, {});
		}
		else if (order == group_by::camera)
		{
			if (!is_empty(key.text1))
			{
				add_title_link(key.text1, search_t(current_search).with(prop::camera_manufacturer, key.text1));
			}

			if (!is_empty(key.text2))
			{
				add_title_link(key.text2, search_t(current_search).with(prop::camera_model, key.text2));
			}
		}
		else if (order == group_by::album_show)
		{
			if (!is_empty(key.text1))
			{
				add_title_link(key.text1, search_t(current_search).with(key.text1_prop_type, key.text1));
			}

			if (key.order3 != 0)
			{
				add_title_link(std::format("{} {}", prop::season.text(), key.order3),
				               search_t(current_search).with(prop::season, key.order3));
			}

			if (!is_empty(key.text2))
			{
				add_title_link(key.text2, search_t(current_search).with(prop::album, key.text2));
			}
		}
		else if (order == group_by::file_type)
		{
			if (!is_empty(key.text1))
			{
				add_title_link(key.text1, search_t(current_search).add_media_type(key.group));
			}
		}
		else if (order == group_by::presence)
		{
			if (!is_empty(key.text1))
			{
				add_title_link(key.text1, {});
			}
		}
		else if (order == group_by::related)
		{
			// A relation is not a filter that can be re-run on its own, so the heading is plain text.
			add_title_link(key.text1, {});
		}
		else if (order == group_by::folder)
		{
			if (!is_empty(key.text1))
			{
				add_title_link(key.text1, search_t().add_selector(key.text1));
			}
		}
	}
	else if (key.type == group_key_type::other)
	{
		add_title_link(file_group::other.display_name(true), {});
	}
	else if (key.type == group_key_type::folder)
	{
		add_title_link(file_group::folder.display_name(true), {});
	}
	else if (key.type == group_key_type::archive)
	{
		add_title_link(file_group::archive.display_name(true), {});
	}
	else if (key.type == group_key_type::retro)
	{
		add_title_link(file_group::commodore.display_name(true), {});
	}
	else if (key.type == group_key_type::audio)
	{
		add_title_link(file_group::audio.display_name(true), {});
	}
	else if (key.type == group_key_type::video)
	{
		add_title_link(file_group::video.display_name(true), {});
	}
	else if (key.type == group_key_type::photo)
	{
		add_title_link(file_group::photo.display_name(true), {});
	}
	else
	{
		add_title_link(tt.group_title_items, {});
	}

	result->elements.emplace_back(std::make_shared<padding_element>());

	if (key.type == group_key_type::folder)
	{
		result->elements.emplace_back(make_icon_link_element(icon_index::new_folder, commands::tool_new_folder,
		                                                     flex_item::right_justified));
		result->elements.emplace_back(make_icon_link_element(icon_index::recursive, commands::browse_recursive,
		                                                     flex_item::right_justified));
	}


	result->elements.emplace_back(std::make_shared<toggle_item_display_element>(g));
	result->padding(8);
	result->margin(4, 8);
	result->set_style_bit(view_element_style::background, true);

	g->_title = can_cache ? result : nullptr;
	g->_title_generation = generation;

	return result;
}

void df::item_group::update_scroll_info(const group_by order)
{
	scroll_text.clear();
	icon = icon_index::none;

	if (_key.type == group_key_type::grouped_value)
	{
		if (order == group_by::shuffle)
		{
			icon = icon_index::shuffle;
		}
		else if (order == group_by::size)
		{
			if (!_items.empty())
			{
				file_size total;

				for (const auto& ii : _items)
				{
					total += ii->file_size();
				}

				scroll_text = prop::format_magnitude(total / _items.size());
			}
		}
		else if (order == group_by::extension)
		{
			scroll_text = to_upper(_key.text1);
		}
		else if (order == group_by::location)
		{
			scroll_text = normalize_county_abbreviation(_key.text1);
		}
		else if (order == group_by::rating_label)
		{
			if (rating_reject_num == _key.order1)
			{
				icon = icon_index::del; // reject
			}
			else if (_key.order1 != 0)
			{
				const int32_t rating = rating_base_num - _key.order1;

				if (rating != 0)
				{
					scroll_text = str::to_string(rating);
				}
			}
		}
		else if (order == group_by::date_created || order == group_by::date_modified ||
			order == group_by::date_original)
		{
			if (_key.order1 == 0)
			{
				icon = icon_index::today;
			}
			else if (_key.order1 == 1)
			{
				icon = icon_index::yesterday;
			}
			else
			{
				scroll_text = str::to_string(3000 - _key.order1); // year
			}
		}
		else if (order == group_by::resolution)
		{
			scroll_text = _key.text2;
		}
		else if (order == group_by::aspect_ratio)
		{
			scroll_text = _key.text1;
		}
		else if (order == group_by::camera)
		{
			scroll_text = _key.text1;
		}
		else if (order == group_by::album_show)
		{
			scroll_text = _key.text1;
		}
		else if (order == group_by::related)
		{
			icon = _key.icon;
		}
		else if (order == group_by::file_type)
		{
			icon = _key.group->icon;
		}
		else if (order == group_by::presence)
		{
			icon = _key.icon;
		}
		else if (order == group_by::folder)
		{
			icon = icon_index::folder;
		}
	}
	else if (_key.type == group_key_type::other)
	{
		icon = icon_index::document;
	}
	else if (_key.type == group_key_type::folder)
	{
		icon = icon_index::folder;
	}
	else if (_key.type == group_key_type::archive)
	{
		icon = icon_index::archive;
	}
	else if (_key.type == group_key_type::retro)
	{
		icon = icon_index::retro;
	}
}

void view_state::update_item_groups()
{
	std::map<df::group_key, df::item_group_ptr> existing_groups;
	for (const auto& g : _item_groups)
	{
		existing_groups[g->_key] = g;
	}

	df::item_set new_display_items;
	const auto is_duplicates = _search.is_duplicates();
	const auto group_order = effective_group_order();
	std::map<df::group_key, df::item_elements> groups;

	const auto& locations = item_index.locations();

	if (_resolved_places_language != locations.display_language_bit())
	{
		_resolved_places_language = locations.display_language_bit();
		_resolved_places.clear();
		_resolving_places.clear();
		++_resolved_places_generation;
	}

	for (const auto& i : _search_items._items)
	{
		if (_filter.match(i))
		{
			new_display_items.add(i);

			if (i->is_folder())
			{
				switch (group_order)
				{
				case group_by::size:
					groups[size_key(i)].emplace_back(i);
					break;

				case group_by::folder:
					groups[folder_key(i)].emplace_back(i);
					break;

				case group_by::date_created:
					groups[date_key(i->file_or_metadata_created(), i)].emplace_back(i);
					break;

				case group_by::date_original:
					groups[date_key(i->media_created(), i)].emplace_back(i);
					break;

				case group_by::date_modified:
					groups[date_key(i->file_modified().system_to_local(), i)].emplace_back(i);
					break;

				case group_by::shuffle:
					groups[shuffle_index_key(i)].emplace_back(i);
					break;

				default:
					groups[media_type_index(i)].emplace_back(i);
					break;
				}
			}
			else
			{
				switch (group_order)
				{
				case group_by::size:
					groups[size_key(i)].emplace_back(i);
					break;

				case group_by::extension:
					groups[extension_key(i)].emplace_back(i);
					break;

				case group_by::folder:
					groups[folder_key(i)].emplace_back(i);
					break;

				case group_by::location:
					groups[location_key(i, locations, _resolved_places)].emplace_back(i);
					break;

				case group_by::rating_label:
					groups[rating_key(i)].emplace_back(i);
					break;

				case group_by::date_created:
					groups[date_key(i->file_or_metadata_created(), i)].emplace_back(i);
					break;

				case group_by::date_original:
					groups[date_key(i->media_created(), i)].emplace_back(i);
					break;

				case group_by::date_modified:
					groups[date_key(i->file_modified().system_to_local(), i)].emplace_back(i);
					break;

				case group_by::resolution:
					groups[resolution_key(i)].emplace_back(i);
					break;

				case group_by::aspect_ratio:
					groups[aspect_ratio_key(i)].emplace_back(i);
					break;

				case group_by::camera:
					groups[camera_key(i)].emplace_back(i);
					break;

				case group_by::album_show:
					groups[album_show_key(i)].emplace_back(i);
					break;

				case group_by::shuffle:
					groups[shuffle_index_key(i)].emplace_back(i);
					break;

				case group_by::presence:
					groups[presence_key(i->presence())].emplace_back(i);
					break;

				case group_by::related:
					groups[related_key(i)].emplace_back(i);
					break;

				default:
					groups[media_type_index(i)].emplace_back(i);
					break;
				}
			}
		}
		else if (i->is_selected())
		{
			i->select(false, {}, i);
		}
	}

	df::item_groups new_item_groups;
	new_item_groups.reserve(groups.size());

	// Sorted rather than hashed: this is rebuilt for every group on every regroup, and a node per
	// displayed item was the allocation traffic that cost. The vector keeps its capacity across groups.
	std::vector<const df::item_element*> current_members;

	for (auto&& i : groups)
	{
		df::item_group_ptr b;
		auto found_group = existing_groups.find(i.first);

		if (found_group != existing_groups.end())
		{
			b = found_group->second;

			// Unchanged membership lets the group keep the order it is already sorted in. An item
			// belongs to one group, so equal counts plus every incoming item already present proves it.
			auto membership_unchanged = i.second.size() == b->items().size();

			if (membership_unchanged)
			{
				current_members.clear();
				current_members.reserve(b->items().size());
				for (const auto& item : b->items()) current_members.emplace_back(item.get());
				std::ranges::sort(current_members);

				for (const auto& item : i.second)
				{
					if (!std::ranges::binary_search(current_members, item.get()))
					{
						membership_unchanged = false;
						break;
					}
				}
			}

			if (!membership_unchanged)
			{
				b->items(std::move(i.second));
			}
		}
		else
		{
			const auto key = i.first.type;
			const auto item_group_display = setting.is_detail_display(key)
				                                ? df::item_group_display::detail
				                                : df::item_group_display::icons;

			b = std::make_shared<df::item_group>(*this, std::move(i.second), item_group_display, i.first);
			b->padding(0);
			b->margin(8, 0);
		}

		b->update_scroll_info(group_order);
		b->sort(group_order, _sort_order, is_duplicates);

		new_item_groups.emplace_back(b);
	}

	groups.clear();

	if (!setting.sort_dates_descending && is_date_group_order(group_order))
	{
		std::ranges::reverse(new_item_groups);
	}

	const auto new_summary_shown = new_display_items.summary();

	if (_summary_shown != new_summary_shown)
	{
		_summary_shown = new_summary_shown;
		invalidate_view(view_invalid::app_layout);
	}

	_display_items = std::move(new_display_items);
	_item_groups = std::move(new_item_groups);

	if (_hover)
	{
		// An item that survives the rebuild must not keep a hover style bit that no controller owns.
		_hover->set_style_bit(view_element_style::hover, false);
		_hover.reset();
	}

	// A focus or anchor the filter dropped keeps its last layout bounds, so item_from_location would
	// keep answering with an item select() then rejects as not displayed, silently ignoring every
	// click over that area.
	if (_focus && !_display_items.contains(_focus))
	{
		_focus.reset();
	}

	if (_selection_anchor && !_display_items.contains(_selection_anchor))
	{
		_selection_anchor.reset();
	}
}

// locations.md 6.2: the snapshot is taken here, on the UI thread, because item metadata is
// UI-owned. Everything handed to the worker is a detached value, and the answer is published back
// whole rather than filled in field by field.
void view_state::refresh_visits()
{
	df::assert_true(ui::is_ui_thread());

	df::cancel_token token(_visits_generation);
	const auto generation = _visits_generation.load(std::memory_order_relaxed);

	df::visit_request request(std::move(token));
	request.samples.reserve(_display_items.size());

	for (const auto& i : _display_items._items)
	{
		// An item whose metadata has not been read yet is still one of the results, so it is
		// sampled as unlocated rather than dropped: the counts must not shrink and grow again as
		// scanning catches up.
		const auto md = i->metadata();

		df::visit_sample s;
		s.days = i->media_created().to_days();

		if (md)
		{
			s.coordinate = md->coordinate;
			s.place = md->location_place;
			s.state = md->location_state;
			s.country = md->location_country;
		}

		request.samples.emplace_back(s);
	}

	// locations.md 6.2 step 6: an era is offered only when the query already names its place.
	if (const auto* const place_term = _search.single_place_term())
	{
		request.intent_place = place_term->text;
	}

	queue_location([this, generation, request = std::move(request)](const location_cache& locations) mutable
	{
		auto timeline = df::compute_visits(std::move(request), locations);

		queue_ui([this, generation, timeline = std::move(timeline)]() mutable
		{
			if (generation != _visits_generation.load(std::memory_order_relaxed)) return;

			_visits = std::move(timeline);

			// group_layout_complete rebuilds the control rows without regrouping, so publishing a
			// timeline can never queue another derivation and loop.
			invalidate_view(view_invalid::group_layout_complete);
		});
	});
}

sizei df::item_group::measure(ui::measure_context& mc, const int width_limit) const
{
	_layout_bounds.resize(_items.size());

	const auto scale_factor = mc.scale_factor;
	// measure and save calculated layout information
	const double base_line_height = calc_item_line_height() * scale_factor;

	const double cy = base_line_height + mc.padding1;
	double y = 0;
	double y_max = 0;

	if (!_items.empty())
	{
		const auto item_count = static_cast<int>(_items.size());

		if (_display == item_group_display::detail)
		{
			const auto line_height = mc.text_line_height(ui::style::font_face::dialog);

			for (auto i = 0; i < item_count; ++i)
			{
				const auto& item = _items[i];
				const auto element_padding = item->porch() * scale_factor;
				const auto has_title = item->has_title();
				const auto line_count = 1 + (_show_folder ? 1 : 0) + (has_title ? 1 : 0);

				const auto height = element_padding.cy * 2 + line_height * line_count;
				_layout_bounds[i] = recti(element_padding.cx, round(y + element_padding.cy),
				                          width_limit - element_padding.cx, round(y + height - element_padding.cy));
				y_max = y + height;
				y += height;
			}
		}
		else
		{
			constexpr auto max_cols = 100;
			const auto gap = 2.0 * scale_factor;
			// The most of a tile that may still be cropped away to close a residual gap.
			constexpr auto crop_tolerance = 0.05;
			double dst_widths[max_cols];
			double nominal_widths[max_cols];
			double aspects[max_cols];

			const auto cy_min = cy * 0.77;
			auto prev_cy_line = 0.0;
			const auto default_cols = std::max(1.0, width_limit / (base_line_height + mc.padding1 * 2.0));
			const double x_limit = width_limit;
			auto n = 0;

			while (n < item_count)
			{
				double x = 0;
				int col_count = 0;
				const int line_start = n;

				// calc line
				while (n < item_count && col_count < max_cols)
				{
					const auto& item = _items[n];
					double cx = (width_limit - mc.padding1 * 3) / default_cols;
					auto aspect = 0.0;

					auto dims = item->layout_dims();
					const auto orientation = item->layout_orientation();

					if (!dims.is_empty())
					{
						if (setting.show_rotated && flips_xy(orientation))
						{
							std::swap(dims.cx, dims.cy);
						}

						// Cover art and placeholders have a borrowed or guessed aspect, so they hold a
						// nominal width instead of following the row height.
						if (item->layout_aspect_known())
						{
							aspect = static_cast<double>(dims.cx) / dims.cy;
							cx = std::max(20.0, aspect * cy);
						}
						else
						{
							cx = std::max(20.0, dims.cx * std::min(cy, static_cast<double>(dims.cy)) / dims.cy);
							if (cx > dims.cx) cx = dims.cx;
						}
					}

					nominal_widths[col_count] = cx;
					aspects[col_count] = aspect;

					if (col_count < 1 || x + cx / 1.5 <= x_limit)
					{
						col_count += 1;
						n += 1;
						x += cx;
					}
					else
					{
						break;
					}
				}

				// post processing on line
				if (col_count > 0)
				{
					const auto is_end_break = n == item_count;
					const auto avail_cx = width_limit - mc.padding1;
					const auto gaps_cx = gap * (col_count - 1);
					auto elastic_aspect = 0.0;
					auto fixed_cx = 0.0;

					for (int nn = 0; nn < col_count; nn++)
					{
						if (aspects[nn] > 0.0) elastic_aspect += aspects[nn];
						else fixed_cx += nominal_widths[nn];
					}

					// One height for the whole row, so a tile can be its own shape: at cx = aspect * cy_line
					// the row fills the width without any tile being cropped to reach it.
					const auto elastic_cx = std::max(0.0, avail_cx - gaps_cx - fixed_cx);
					const auto cy_fit = elastic_aspect > 0.0 ? elastic_cx / elastic_aspect : cy;

					// A trailing row is never stretched past the row above it, so the end of a group does
					// not read as a different thumbnail size.
					const auto cy_max = is_end_break && prev_cy_line > 0.0 ? prev_cy_line : cy * 1.33;
					auto cy_line = std::clamp(cy_fit, cy_min, std::max(cy_min, cy_max));

					if (cy_line > cy_fit && elastic_cx > 0.0)
					{
						// The height floor is the only thing that can leave a row over-full, and reaching
						// it may not cost more than the crop tolerance.
						cy_line = std::max(20.0 * scale_factor,
						                   std::min(cy_line, cy_fit / (1.0 - crop_tolerance)));
					}

					double total_width = 0.0;

					for (int nn = 0; nn < col_count; nn++)
					{
						const auto cx = aspects[nn] > 0.0
							                ? std::max(20.0 * scale_factor, aspects[nn] * cy_line)
							                : nominal_widths[nn];
						dst_widths[nn] = cx;
						total_width += cx;
					}

					auto x_gap = gap;

					if (total_width > avail_cx - gaps_cx)
					{
						// Take the remainder off the widths, which crops the sides of a tile rather than
						// the top and bottom of its subject.
						const auto shrink = (avail_cx - gaps_cx) / total_width;
						for (int nn = 0; nn < col_count; nn++) dst_widths[nn] *= shrink;
						total_width = avail_cx - gaps_cx;
					}
					else if (!is_end_break && col_count > 1)
					{
						// a single-column line has no gaps to distribute, and dividing by
						// col_count - 1 would make x_gap NaN
						x_gap = std::clamp((avail_cx - total_width) / (col_count - 1), gap, gap * 5.5);
					}

					double xx = 0;

					for (int nn = 0; nn < col_count; nn++)
					{
						const auto cx = dst_widths[nn];
						_layout_bounds[line_start + nn] =
							recti(round(xx), round(y), round(xx + cx), round(y + cy_line));
						xx += cx + x_gap;
					}

					y += cy_line + gap;
					y_max = std::max(y_max, y + mc.padding1);
					prev_cy_line = cy_line;
				}
			}
		}
	}

	return {width_limit, round(y_max)};
}

static bool can_show_flag(const df::item_display_info& info)
{
	return !is_empty(info.label) || info.rating == -1;
}

static void draw_flag(ui::draw_context& dc, const df::item_display_info& info, const recti logical_bounds,
                      const float a)
{
	draw_rate_label_badge(dc, info.label.sv(), info.rating, logical_bounds, a);
}

void draw_rate_label_badge(ui::draw_context& dc, const std::string_view label, const int rating,
                           const recti logical_bounds, const float a)
{
	auto icon = icon_index::flag;
	ui::color32 label_clr = 0;

	if (!label.empty())
	{
		const auto* const def = find_rate_label_def(label);

		if (def)
		{
			icon = def->icon;
			label_clr = def->clr;
		}
		else
		{
			label_clr = ui::average(dc.colors.foreground, dc.colors.background);
		}
	}

	// Reject shares the badge with the label, and the stronger state wins.
	if (rating < 0)
	{
		icon = rate_label_reject.icon;
		label_clr = rate_label_reject.clr;
	}

	const bool has_clr = label_clr != 0;
	const auto alpha = has_clr ? a : a / 4.0f;
	ui::color bg;

	if (has_clr)
	{
		bg = ui::color(label_clr, a);
	}

	xdraw_icon(dc, icon, logical_bounds, ui::color(dc.colors.foreground, alpha), bg);
}

void draw_pin_badge(ui::draw_context& dc, const recti logical_bounds, const float alpha)
{
	xdraw_icon(dc, icon_index::pinned, logical_bounds, ui::color(dc.colors.foreground, alpha),
	           ui::color(ui::style::color::important_background, alpha));
}

void df::item_group::layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions)
{
	bounds = bounds_in;

	if (_display == item_group_display::detail)
	{
		_row_draw_info.clear_for_layout();
		for (const auto& item : _items) item->row_layout_valid = false;
	}

	if (_layout_bounds.size() == _items.size())
	{
		for (auto i = 0u; i < _items.size(); ++i)
		{
			_items[i]->layout(mc, _layout_bounds[i].offset(bounds.left, bounds.top), positions);
		}
	}

	if (_display == item_group_display::detail)
	{
		update_row_layout(mc);
	}
}

void df::item_group::display(const item_group_display d)
{
	if (_display != d)
	{
		_display = d;
		_state.invalidate_view(view_invalid::view_layout);
	}
}

void df::item_group::toggle_display()
{
	display(_display == item_group_display::icons ? item_group_display::detail : item_group_display::icons);

	setting.set_detail_display(_key.type, _display == item_group_display::detail);
}

void df::item_group::update_row_layout(const ui::measure_context& mc) const
{
	_row_draw_info.icon.width = _row_draw_info.icon.extent;
	_row_draw_info.disk.width = _row_draw_info.disk.val_max > 1 ? _row_draw_info.disk.extent : 0;
	_row_draw_info.track.width = _row_draw_info.track.extent;
	_row_draw_info.title.width = _row_draw_info.title.extent;
	_row_draw_info.flag.width = _row_draw_info.flag.extent;
	_row_draw_info.presence.width = _row_draw_info.presence.extent;
	_row_draw_info.sidecars.width = _row_draw_info.sidecars.extent;
	_row_draw_info.items.width = _row_draw_info.items.extent;
	_row_draw_info.info.width = _row_draw_info.info.extent;
	_row_draw_info.duration.width = _row_draw_info.duration.extent;
	_row_draw_info.file_size.width = _row_draw_info.file_size.extent;
	_row_draw_info.bitrate.width = _row_draw_info.bitrate.extent;
	_row_draw_info.pixel_format.width = _row_draw_info.pixel_format.extent;
	_row_draw_info.dimensions.width = _row_draw_info.dimensions.extent;
	_row_draw_info.audio_sample_rate.width = _row_draw_info.audio_sample_rate.extent;
	_row_draw_info.modified.width = _row_draw_info.modified.extent;
	_row_draw_info.created.width = _row_draw_info.created.extent;

	const auto text_padding = round(item_draw_info::_text_padding * mc.scale_factor);
	const auto width_avail = bounds.width() - mc.padding1 * 5;
	const auto half_width_avail = width_avail / 2;
	const auto expendable_title_width = _row_draw_info.title.width > half_width_avail
		                                    ? _row_draw_info.title.width - half_width_avail
		                                    : 0;

	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.title.width -= expendable_title_width;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.info.width = _row_draw_info.disk.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.file_size.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.duration.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.track.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.audio_sample_rate.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.bitrate.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.dimensions.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.pixel_format.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.title.width = width_avail;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.items.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.modified.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.created.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.sidecars.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.presence.width = 0;
	if (width_avail < _row_draw_info.total(text_padding)) _row_draw_info.flag.width = 0;

	if (_row_draw_info.total(text_padding) < width_avail)
	{
		_row_draw_info.title.width += width_avail - _row_draw_info.total(text_padding);
	}
}

void df::item_group::update_detail_row_layout(ui::draw_context& dc, const item_element_ptr& i,
                                              const bool has_related) const
{
	const auto info = i->populate_info();

	_row_draw_info.icon.extent = std::max(_row_draw_info.icon.extent, dc.icon_cxy);

	if (info.disk != 0)
	{
		_row_draw_info.disk.update_extent(dc, str::to_string(info.disk));
	}

	if (info.track != 0)
	{
		_row_draw_info.track.update_extent(dc, str::to_string(info.track), info.track);
	}

	_row_draw_info.title.update_extent(dc, combine2(info.title, info.name));

	if (can_show_flag(info))
	{
		_row_draw_info.flag.extent = std::max(_row_draw_info.flag.extent, dc.icon_cxy);
	}

	if (!has_related && can_show_duplicates(info))
	{
		_row_draw_info.presence.update_extent(dc, str::format_count(info.duplicates));
	}

	if (info.sidecars > 0)
	{
		_row_draw_info.sidecars.update_extent(dc, str::format_count(info.sidecars));
	}

	if (!str::is_empty(info.info))
	{
		_row_draw_info.info.update_extent(dc, info.info);
	}

	if (info.items > 0)
	{
		_row_draw_info.items.update_extent(dc, str::format_count(info.items), info.items);
	}

	if (info.duration > 0)
	{
		_row_draw_info.duration.update_extent(dc, prop::format_duration(info.duration), info.duration);
	}

	if (!info.size.is_empty())
	{
		_row_draw_info.file_size.update_extent(dc, df::format_size(info.size),
		                                       static_cast<double>(info.size.to_int64()));
	}

	if (!is_empty(info.bitrate))
	{
		_row_draw_info.bitrate.update_extent(dc, info.bitrate);
	}

	if (!is_empty(info.pixel_format))
	{
		_row_draw_info.pixel_format.update_extent(dc, info.pixel_format);
	}

	if (!prop::is_null(info.dimensions))
	{
		_row_draw_info.dimensions.update_extent(dc, prop::format_pixels(info.dimensions, i->file_type()));
	}

	if (!prop::is_null(info.audio_sample_rate))
	{
		_row_draw_info.audio_sample_rate.update_extent(dc, prop::format_audio_sample_rate(info.audio_sample_rate));
	}

	if (_state.group_order() == group_by::date_modified)
	{
		_row_draw_info.modified.update_extent(dc, platform::format_date_time(info.modified));
	}

	if (_state.group_order() == group_by::date_created || _state.group_order() == group_by::date_original)
	{
		const auto shown = _state.group_order() == group_by::date_created ? info.file_created : info.created;
		_row_draw_info.created.update_extent(dc, platform::format_date_time(shown));
	}

	i->row_layout_valid = true;
}

void df::item_group::render(ui::draw_context& dc, const pointi element_offset) const
{
}

void df::item_group::scroll_tooltip(const item_element_ptr& item, const view_elements_ptr& elements,
                                    async_strategy& async) const
{
	constexpr auto max_thumb_dim = 80;

	if (item && is_valid(item->thumbnail_surface()))
	{
		_scroll_tooltip_staging_item.reset();
		elements->add(std::make_shared<surface_element>(item->thumbnail_surface(), max_thumb_dim, flex_item::center));
	}
	else if (item && is_valid(item->thumbnail()))
	{
		if (!_scroll_tooltip_staging_item || _scroll_tooltip_staging_item == item ||
			!_scroll_tooltip_staging_item->is_staging_thumbnail_surface())
		{
			_scroll_tooltip_staging_item = item;
			item->stage_thumbnail_surface(async, false, true);
		}
	}
	else
	{
		for (const auto& i : _items)
		{
			if (is_valid(i->thumbnail_surface()))
			{
				_scroll_tooltip_staging_item.reset();
				elements->add(std::make_shared<surface_element>(i->thumbnail_surface(), max_thumb_dim,
				                                                flex_item::center));
				break;
			}
			if (i->has_thumb())
			{
				if (!_scroll_tooltip_staging_item || _scroll_tooltip_staging_item == i ||
				!_scroll_tooltip_staging_item->is_staging_thumbnail_surface())
				{
				_scroll_tooltip_staging_item = i;
				i->stage_thumbnail_surface(async, false, true);
				}
				break;
			}
		}
	}

	if (_scroll_tooltip_rating != 0)
	{
		elements->add(make_icon_element(icon_index::star_solid, _scroll_tooltip_rating,
		                                flex_item::line_break));
	}

	for (const auto& t : _scroll_tooltip_text)
	{
		elements->add(std::make_shared<text_element>(t, ui::style::font_face::dialog, ui::style::text_style::multiline,
		                                             flex_item::center | flex_item::new_line));
	}
}

// Items are laid out strictly top-to-bottom, so bounds.bottom is non-decreasing across _items.
// Returns the index of the first item that can contain y; callers scan forward while bounds.top <= y.
static size_t first_item_at_y(const df::item_elements& items, const int y)
{
	const auto found = std::ranges::lower_bound(items, y, {}, [](const df::item_element_ptr& i)
	{
		return i->bounds.bottom;
	});

	return std::distance(items.begin(), found);
}

void df::item_group::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	// Every item is laid out inside the group, so a miss here rules out the whole group. This keeps
	// a hit test over a large collection proportional to the number of groups, not items.
	if (!bounds.offset(element_offset).contains(loc)) return;

	const auto y = loc.y - element_offset.y;

	for (auto n = first_item_at_y(_items, y); n < _items.size() && _items[n]->bounds.top <= y; ++n)
	{
		const auto& i = _items[n];

		if (i->bounds.offset(element_offset).contains(loc))
		{
			i->tooltip(hover, loc, element_offset);
		}
	}
}

view_controller_ptr df::item_group::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                             const pointi element_offset,
                                                             hit_test_context& ctx)
{
	if (!bounds.offset(element_offset).contains(loc)) return nullptr;

	const auto y = loc.y - element_offset.y;

	for (auto n = first_item_at_y(_items, y); n < _items.size() && _items[n]->bounds.top <= y; ++n)
	{
		const auto& i = _items[n];

		if (i->bounds.offset(element_offset).contains(loc))
		{
			auto result = i->controller_from_location(host, loc, element_offset, ctx);
			if (result) return result;
		}
	}

	return nullptr;
}

df::item_element_ptr df::item_group::drawable_from_layout_location(const pointi loc) const
{
	// The hovered and focused items are checked by view_state::item_from_location before we get here,
	// because they can extend beyond their layout bounds.
	for (auto n = first_item_at_y(_items, loc.y); n < _items.size() && _items[n]->bounds.top <= loc.y; ++n)
	{
		if (_items[n]->bounds.contains(loc))
		{
			return _items[n];
		}
	}

	return nullptr;
}


// Folds the duplicates bit into the published mask from the count as it stands now, with a
// compare-exchange so a mask republished concurrently is not overwritten. Both writers finish here:
// calc_search_presence republishes the whole mask from a count it read earlier, and a count that
// update_duplicates published in between would otherwise be lost under that store - leaving a
// duplicate out of a duplicates search, because the mask is a hard rejection filter.
static void fold_duplicates_bit(std::atomic<search_presence_mask>& presence,
                                const std::atomic<df::duplicate_info>& duplicates)
{
	auto existing = presence.load();
	search_presence_mask updated;

	do
	{
		updated = existing;

		if (duplicates.load().count > 1) updated.types |= search_presence_mask::duplicates;
		else updated.types &= ~search_presence_mask::duplicates;
	}
	while (!presence.compare_exchange_weak(existing, updated));
}

void df::index_file_item::update_duplicates(const index_folder_item_ptr& f, const duplicate_info dup_info) const
{
	const auto existing = duplicates.load();

	if (existing.count != dup_info.count || existing.group != dup_info.group ||
		existing.grade != dup_info.grade || existing.same_picture_crowded != dup_info.same_picture_crowded)
	{
		duplicates = dup_info;
		fold_duplicates_bit(search_presence, duplicates);
		f->update_search_presence(*this);
	}
}

void df::index_file_item::calc_search_presence() const
{
	search_presence_mask updated;
	updated.types |= ft->group->search_presence_bit();

	const auto md = metadata.load();

	if (md)
	{
		updated |= md->calc_search_presence();
	}

	if (duplicates.load().count > 1)
	{
		updated.types |= search_presence_mask::duplicates;
	}

	search_presence = updated;
	fold_duplicates_bit(search_presence, duplicates);
}

void df::item_element::render_bg(ui::draw_context& dc, const item_group&, const pointi element_offset) const
{
	render_background(dc, element_offset);
}

void df::item_element::render(ui::draw_context& dc, const item_group& group, const pointi element_offset) const
{
	const auto& s = group._state;
	const auto display = group.display();
	const auto group_order = s.group_order();
	const auto sort_order = s.sort_order();
	const auto info = populate_info();
	const auto has_related = s.search().has_related();

	const auto text_padding = round(item_draw_info::_text_padding * dc.scale_factor);
	const auto device_bounds = bounds.offset(element_offset);

	const auto* const ft = file_type();
	const auto is_highlight = is_style_bit_set(view_element_style::highlight);
	const auto is_hover = is_style_bit_set(view_element_style::hover);
	const auto is_folder = ft == file_type::folder;
	const auto is_focus = s.focus_item().get() == this;
	const auto is_selected = this->is_selected();
	// design.md: pin is a state distinct from selection. Without a mark on the item itself the held
	// item is indistinguishable from the one just clicked, which is the hidden state the pin exists
	// to avoid.
	const auto is_pinned = s._pin_item.get() == this;

	_pin_badge_bounds = {};

	const auto is_error = this->is_error();
	const auto show_folder = group._show_folder;

	constexpr auto text_font = ui::style::font_face::dialog;
	const auto text_line_height = dc.text_line_height(text_font);
	const auto cxy_flag = std::max(dc.icon_cxy + text_padding, text_line_height + text_padding);

	const auto background_is_highlighted = is_focus || is_highlight || is_hover || is_selected || is_error;
	const auto bg_color = calc_background_color(dc);
	const auto pad = padding * dc.scale_factor;

	if (alt_background)
	{
		dc.draw_rounded_rect(device_bounds.inflate(pad.cx, pad.cy), ui::color(0, 0.1f), dc.padding1);
	}

	if (display == item_group_display::detail)
	{
		const auto alpha = dc.colors.alpha;
		const auto text_color = ui::color(dc.colors.foreground, alpha);
		const auto group_text_color = ui::color(ft->text_color(dc.colors.foreground), alpha).emphasize(
			background_is_highlighted);
		const auto bg_disk = ui::color(ui::style::color::view_selected_background, dc.colors.alpha * 0.77f);
		const auto bg_dups = ui::color(app_colors::duplicate_background, dc.colors.alpha * 0.77f);
		const auto bg_sidecars = ui::color(app_colors::sidecar_background, dc.colors.alpha * 0.77f);

		if (device_bounds.width() > 100)
		{
			const auto& widths = group.row_widths();

			recti row_bounds = device_bounds;

			auto image_rect = row_bounds;
			image_rect.right = image_rect.left + widths.icon.extent + text_padding;

			{
				if (is_pinned)
				{
					draw_pin_badge(dc, image_rect, alpha);
				}
				else
				{
					xdraw_icon(dc, info.icon, image_rect, text_color, {});
				}
			}

			// The row icon cell doubles as the release affordance while the item is held.
			_pin_badge_bounds = is_pinned ? image_rect.offset(-element_offset.x, -element_offset.y) : recti{};

			row_bounds.left = image_rect.right + text_padding;

			auto text_rect = row_bounds;
			auto folder_rect = row_bounds;

			if (show_folder)
			{
				const auto line_count = 2 + (info.title.is_empty() ? 0 : 1);
				const auto line_height = text_rect.height() / line_count;
				folder_rect.top = text_rect.bottom -= line_height;
			}

			auto text_style = ui::style::text_style::single_line;
			auto text_style_far = ui::style::text_style::single_line_far;
			auto text_style_center = ui::style::text_style::single_line_center;
			auto text_x = text_rect.left;
			auto text_y = pad.cy;

			if (widths.disk.width > 0)
			{
				if (info.disk != 0)
				{
					const auto bb = widths.disk.calc_bounds(text_rect, text_x, text_y, text_padding);
					widths.disk.draw(dc, str::to_string(info.disk), ui::color{}, bb, text_font, text_style_far,
					                 text_color);
				}
				text_x += widths.disk.width + text_padding;
			}

			if (widths.track.width > 0)
			{
				if (info.track != 0)
				{
					const auto bb = widths.track.calc_bounds(text_rect, text_x, text_y, text_padding);
					widths.track.draw(dc, str::to_string(info.track), ui::color{}, bb, text_font, text_style_far,
					                  text_color);
				}
				text_x += widths.track.width + text_padding;
			}

			if (widths.title.width > 0)
			{
				auto title_rect = text_rect;
				title_rect.left = text_x;
				title_rect.right = text_x + widths.title.width + text_padding;

				if (info.online_status != item_online_status::disk)
				{
					const auto bb = center_rect(sizei{cxy_flag, cxy_flag},
					                            recti(title_rect.right - cxy_flag, title_rect.top, title_rect.right,
					                                  title_rect.bottom));
					xdraw_icon(dc, icon_index::cloud, bb, text_color, bg_disk);
					title_rect.right -= cxy_flag;
				}

				if (!info.title.is_empty() && title_rect.height() >= 2 * text_line_height)
				{
					auto name_bounds = title_rect;
					auto title_bounds = title_rect;
					name_bounds.top = title_bounds.bottom = (title_rect.top + title_rect.bottom) / 2;
					name_bounds.left = title_bounds.left += text_padding / 2;
					dc.draw_text(info.title, title_bounds, text_font, text_style, text_color, {});
					dc.draw_text(info.name, name_bounds, text_font, text_style, group_text_color, {});
				}
				else
				{
					dc.draw_text(info.name, title_rect, text_font, text_style, group_text_color, {});
				}

				text_x += widths.title.width + text_padding;
			}

			if (widths.flag.width > 0)
			{
				if (can_show_flag(info))
				{
					const auto bb = widths.flag.calc_bounds(text_rect, text_x, text_y, text_padding);
					draw_flag(dc, info, bb, dc.colors.alpha);
				}
				text_x += widths.flag.width + text_padding;
			}


			if (widths.presence.width > 0)
			{
				// The column is as wide as the widest row in the group, so a row with no duplicate
				// still has room reserved. Only the rows the layout measured may draw, or a file that
				// is its own only copy gets the "1" badge issue #137 exists to remove.
				if (!has_related && can_show_duplicates(info))
				{
					const auto bb = widths.presence.calc_bounds(text_rect, text_x, text_y, text_padding);
					widths.presence.draw(dc, str::format_count(info.duplicates), bg_dups, bb, text_font,
					                     text_style_center, text_color);
				}
				text_x += widths.presence.width + text_padding;
			}

			if (widths.sidecars.width > 0)
			{
				if (info.sidecars > 0)
				{
					const auto bb = widths.sidecars.calc_bounds(text_rect, text_x, text_y, text_padding);
					widths.sidecars.draw(dc, str::format_count(info.sidecars), bg_sidecars, bb, text_font,
					                     text_style_center, text_color);
				}
				text_x += widths.sidecars.width + text_padding;
			}

			if (widths.items.width > 0)
			{
				if (info.items > 0)
				{
					const auto bb = widths.items.calc_bounds(text_rect, text_x, text_y, text_padding);
					widths.items.draw(dc, str::format_count(info.items), info.items, bb, text_font, text_style_far,
					                  text_color);
				}
				text_x += widths.items.width + text_padding;
			}

			if (widths.duration.width > 0)
			{
				if (info.duration > 0)
				{
					const auto bb = widths.duration.calc_bounds(text_rect, text_x, text_y, text_padding);
					const auto text = prop::format_duration(info.duration);
					widths.duration.draw(dc, text, info.duration, bb, text_font, text_style_far, text_color);
				}
				text_x += widths.duration.width + text_padding;
			}

			if (widths.file_size.width > 0)
			{
				if (!info.size.is_empty())
				{
					const auto bb = widths.file_size.calc_bounds(text_rect, text_x, text_y, text_padding);
					const auto text = df::format_size(info.size);
					widths.file_size.draw(dc, text, static_cast<double>(info.size.to_int64()), bb, text_font,
					                      text_style_far, text_color);
				}
				text_x += widths.file_size.width + text_padding;
			}

			if (widths.bitrate.width > 0)
			{
				if (!prop::is_null(info.bitrate))
				{
					const auto bb = widths.bitrate.calc_bounds(text_rect, text_x, text_y, text_padding);
					widths.bitrate.draw(dc, info.bitrate, ui::color{}, bb, text_font, text_style_far, text_color);
				}
				text_x += widths.bitrate.width + text_padding;
			}

			if (widths.info.width > 0)
			{
				if (!prop::is_null(info.info))
				{
					const auto bb = widths.info.calc_bounds(text_rect, text_x, text_y, text_padding);
					widths.info.draw(dc, info.info, ui::color{}, bb, text_font, text_style_far, text_color);
				}
				text_x += widths.info.width + text_padding;
			}

			if (widths.dimensions.width > 0)
			{
				if (!prop::is_null(info.dimensions))
				{
					const auto bb = widths.dimensions.calc_bounds(text_rect, text_x, text_y, text_padding);
					const auto text = prop::format_pixels(info.dimensions, file_type());
					widths.dimensions.draw(dc, text, ui::color{}, bb, text_font, text_style_far, text_color);
				}
				text_x += widths.dimensions.width + text_padding;
			}

			if (widths.pixel_format.width > 0)
			{
				if (!is_empty(info.pixel_format))
				{
					const auto bb = widths.pixel_format.calc_bounds(text_rect, text_x, text_y, text_padding);
					widths.pixel_format.draw(dc, info.pixel_format, ui::color{}, bb, text_font, text_style, text_color);
				}
				text_x += widths.pixel_format.width + text_padding;
			}

			if (widths.audio_sample_rate.width > 0)
			{
				if (!prop::is_null(info.audio_sample_rate))
				{
					const auto bb = widths.audio_sample_rate.calc_bounds(text_rect, text_x, text_y, text_padding);
					const auto text = prop::format_audio_sample_rate(info.audio_sample_rate);
					widths.audio_sample_rate.draw(dc, text, ui::color{}, bb, text_font, text_style_far, text_color);
				}
				text_x += widths.audio_sample_rate.width + text_padding;
			}

			if (widths.created.width > 0)
			{
				const auto shown = group_order == group_by::date_created ? info.file_created : info.created;

				if (!prop::is_null(shown))
				{
					const auto bb = widths.created.calc_bounds(text_rect, text_x, text_y, text_padding);
					const auto text = platform::format_date_time(shown);
					widths.created.draw(dc, text, ui::color{}, bb, text_font, text_style_far, text_color);
				}
				text_x += widths.created.width + text_padding;
			}

			if (widths.modified.width > 0)
			{
				if (!prop::is_null(info.modified))
				{
					const auto bb = widths.modified.calc_bounds(text_rect, text_x, text_y, text_padding);
					const auto text = platform::format_date_time(info.modified);
					widths.modified.draw(dc, text, ui::color{}, bb, text_font, text_style_far, text_color);
				}
				text_x += widths.modified.width + text_padding;
			}

			if (show_folder)
			{
				folder_rect.left += text_padding / 2;
				dc.draw_text(info.folder, folder_rect, text_font, text_style, text_color, {});
			}
		}
	}
	else if (display == item_group_display::icons)
	{
		// Encoded images can be published or replaced by a worker between frames. Hold one
		// snapshot for this draw; their CPU surfaces may still be staging, which is a normal
		// lifecycle state handled by the file-type fallback below.
		const auto thumbnail = _thumbnail;
		const auto cover_art = _cover_art;
		const auto thumb_is_valid = is_valid(thumbnail) || is_valid(cover_art);
		const auto show_text = is_hover || !thumb_is_valid || is_folder || is_focus;
		const auto expand_text = (is_hover || is_focus) && thumb_is_valid;
		const auto show_stars = show_text && group_order != group_by::rating_label;
		const auto show_info = (is_folder || is_hover) && !str::is_empty(info.info);
		const auto show_size = show_text && !is_folder && (group_order == group_by::size || sort_order ==
			sort_by::size);
		const auto show_created = show_text && !is_folder && (group_order == group_by::date_created ||
			group_order == group_by::date_original || sort_order == sort_by::date_original ||
			sort_order == sort_by::date_created);
		// The group order names the key when it is a date order; otherwise the sort order does.
		const auto created_shown_is_file = group_order == group_by::date_created ||
			(group_order != group_by::date_original && sort_order == sort_by::date_created);
		const auto show_modified = show_text && !is_folder && (group_order == group_by::date_modified || sort_order ==
			sort_by::date_modified);
		const auto show_selected = !(is_focus || is_hover) && (is_selected || is_highlight);
		constexpr auto text_style = ui::style::text_style::single_line_center;
		const auto stars_line_height = std::max(text_line_height, dc.icon_cxy) + dc.padding2;
		const auto extra_padding = round((is_focus || is_hover ? 4 : 0) * dc.scale_factor);
		const auto bg_padding = sizei(pad.cx + extra_padding, pad.cy + extra_padding);
		const auto bg_bounds = device_bounds.inflate(bg_padding.cx, bg_padding.cy);
		const auto text_alpha = thumb_is_valid && !show_text ? bg_color.a * dc.colors.alpha : dc.colors.alpha;
		const auto text_color = ui::color(dc.colors.foreground, text_alpha);
		const auto fallback_text_color = ui::color(dc.colors.foreground, dc.colors.alpha);
		const auto group_text_color = ui::color(ft->text_color(dc.colors.foreground), text_alpha).emphasize(
			background_is_highlighted);
		const auto bg_disk = ui::color(ui::style::color::view_selected_background, text_alpha * 0.77f);
		const auto bg_dups = ui::color(app_colors::duplicate_background, text_alpha * 0.77f);
		const auto bg_sidecars = ui::color(app_colors::sidecar_background, text_alpha * 0.77f);
		const auto thumb_padding = round(item_draw_info::_thumb_padding * dc.scale_factor);
		auto title_style = ui::style::text_style::single_line_center;
		auto title_line_height2 = dc.text_line_height(info.title_font);
		auto title_line_height1 = is_empty(info.title) ? 0 : title_line_height2;
		auto title_text_extra_width = 0;

		if (is_focus || is_hover)
		{
			title_style = ui::style::text_style::multiline_center;
			auto avail_width = is_hover ? mul_div(bg_bounds.width(), 7, 4) : bg_bounds.width();
			const auto title_extent1 = is_empty(info.title)
				                           ? sizei{}
				                           : dc.measure_text(info.title, info.title_font, title_style, avail_width);
			const auto title_extent2 = dc.measure_text(info.name, info.title_font, title_style, avail_width);
			const auto extra1 = title_extent1.cx > bg_bounds.width() ? title_extent1.cx - device_bounds.width() : 0;
			const auto extra2 = title_extent2.cx > bg_bounds.width() ? title_extent2.cx - device_bounds.width() : 0;
			title_text_extra_width = std::max(extra1, extra2);

			const auto render_padding = title_text_extra_width / 2 + (is_hover ? text_padding : 0);
			const auto render_width = device_bounds.width() + render_padding * 2;
			title_line_height1 = is_empty(info.title)
				                     ? 0
				                     : dc.measure_text(info.title, info.title_font, title_style, render_width).cy;
			title_line_height2 = dc.measure_text(info.name, info.title_font, title_style, render_width).cy;
		}

		int text_height = 0;

		if (show_text)
		{
			text_height += title_line_height1;
			text_height += title_line_height2;
		}

		if (show_info)
		{
			text_height += text_line_height;
		}

		if (show_size || show_created || show_modified)
		{
			text_height += text_line_height;
		}

		if (show_stars && info.rating != 0)
		{
			text_height += stars_line_height;
		}

		auto text_rect = device_bounds;
		text_rect.top = expand_text ? bg_bounds.bottom : bg_bounds.bottom - text_height;
		text_rect.bottom = text_rect.top + text_height;
		text_rect.left -= title_text_extra_width / 2 + (is_hover ? text_padding : 0);
		text_rect.right += title_text_extra_width / 2 + (is_hover ? text_padding : 0);
		const auto group_device_bounds = group.bounds.offset(element_offset);

		if (text_rect.width() >= group_device_bounds.width())
		{
			text_rect.left = group_device_bounds.left;
			text_rect.right = group_device_bounds.right;
		}
		else if (text_rect.right > group_device_bounds.right)
		{
			text_rect = text_rect.offset(group_device_bounds.right - text_rect.right, 0);
		}
		else if (text_rect.left < group_device_bounds.left)
		{
			text_rect = text_rect.offset(group_device_bounds.left - text_rect.left, 0);
		}

		// The expanded caption is wider than the tile and is shifted sideways to stay inside the group,
		// so it overhangs the neighbouring columns - for the last tile in a row by most of a tile width.
		// Only its vertical growth belongs to the hit test: view_state::item_from_location lets the
		// hovered or focused item win, so a box spanning the overhang swallows the neighbour's clicks.
		const auto text_bounds = show_text ? text_rect.offset({-element_offset.x, -element_offset.y}) : recti{};
		const auto grown_bounds = bounds.make_union(text_bounds);
		_interactive_bounds = {bounds.left, grown_bounds.top, bounds.right, grown_bounds.bottom};

		auto icon_rect = device_bounds.inflate(-thumb_padding, -thumb_padding);
		auto thumbnail_drawn = false;
		const auto draw_placeholder = [&](const float alpha)
		{
			if (alpha <= 0.0f) return;

			if (is_folder)
			{
				xdraw_icon(dc, info.icon, icon_rect, group_text_color.aa(alpha), {});
				return;
			}

			if (ft->has_trait(file_traits::thumbnail))
			{
				dc.draw_rect(icon_rect, ui::color(ui::style::color::group_background, alpha * 0.72f));
			}

			auto ext = _name.substr(find_ext(_name));
			if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);

			if (str::is_empty(ext))
			{
				xdraw_icon(dc, info.icon, icon_rect, fallback_text_color.aa(alpha), {});
			}
			else
			{
				dc.draw_text(str::to_lower(ext), icon_rect, ui::style::font_face::dialog,
				             ui::style::text_style::single_line_center, fallback_text_color.aa(alpha * 0.68f), {});
			}
		};

		if (icon_rect.height() > text_height)
		{
			icon_rect.bottom = icon_rect.bottom - text_height;
		}

		if (thumb_is_valid)
		{
			// The GPU texture belongs to the draw context and is therefore created here rather
			// than by the staging worker. Cover art is preferred for the normal tile, while hover
			// deliberately reveals the media thumbnail; invalidate the one-slot texture cache when
			// that source changes.
			const auto use_cover_art = is_valid(cover_art) && (!is_hover || !is_valid(thumbnail));
			const auto orientation = use_cover_art ? cover_art->orientation() : thumbnail->orientation();
			auto tex = _texture;
			if (tex && (_thumbnail_state && thumbnail_state::texture_is_cover_art) != use_cover_art)
			{
				_texture.reset();
				tex.reset();
			}

			if (!tex)
			{
				const auto t = dc.create_texture();
				const auto surface = use_cover_art ? _cover_art_surface : _thumbnail_surface;

				if (t && surface && t->update(surface) != ui::texture_update_result::failed)
				{
					bump(ui_perf.texture_uploads);
					set_thumbnail_state(thumbnail_state::texture_is_cover_art, use_cover_art);
					_texture = t;
					tex = t;
				}
			}

			if (tex)
			{
				if (_thumbnail_state && thumbnail_state::fade_pending)
				{
					set_thumbnail_state(thumbnail_state::fade_pending, false);
					start_thumbnail_animation(group._state);
				}

				thumbnail_drawn = true;
				const auto thumbnail_alpha = _thumbnail_alpha.val();
				const auto texture_dimensions = tex->dimensions();
				const auto pad3 = round(3 * dc.scale_factor);
				const auto flip = setting.show_rotated && flips_xy(orientation);
				const auto rect_draw = rectd(device_bounds.left, device_bounds.top, device_bounds.width(),
				                             device_bounds.height()).inflate(show_selected ? -pad3 : 0);

				const double ww = flip ? rect_draw.Height : rect_draw.Width;
				const double hh = flip ? rect_draw.Width : rect_draw.Height;

				const double sx = ww / static_cast<double>(texture_dimensions.cx);
				const double sy = hh / static_cast<double>(texture_dimensions.cy);

				const auto texture_scale = std::max(sx, sy);
				const auto cx_tex = ww / texture_scale;
				const auto cy_tex = hh / texture_scale;
				const auto x_tex = (texture_dimensions.cx - cx_tex) / 2;
				// The cell is the tile's own shape, so this normally crops nothing; the slight top bias
				// keeps faces in frame for the residual case. Orientation is applied to the destination
				// quad so sampling remains in the source texture's coordinates.
				const auto y_tex = (texture_dimensions.cy - cy_tex) / 3;

				const rectd rect_tex(x_tex, y_tex, cx_tex, cy_tex);

				const auto dst_quad = setting.show_rotated
					                      ? quadd(rect_draw).transform(to_simple_transform(orientation))
					                      : quadd(rect_draw);

				const auto sampler = calc_sampler(rect_draw.extent().round(), rect_tex.extent().round(), orientation);

				draw_placeholder(dc.colors.alpha * (1.0f - thumbnail_alpha));
				dc.draw_texture(tex, dst_quad, rect_tex.round(), dc.colors.alpha * thumbnail_alpha, sampler);

				draw_texture_info(dc, rect_draw.round(), tex, orientation, sampler,
				                  dc.colors.alpha * thumbnail_alpha);
			}
		}
		if (!thumbnail_drawn)
		{
			// Encoded bytes suppress the normal title treatment before their staged surface is
			// ready. Keep a full-alpha type marker visible during that short asynchronous gap.
			draw_placeholder(dc.colors.alpha);
		}

		auto y = text_rect.top;

		if (show_text)
		{
			auto text_bg_bounds = text_rect;
			text_bg_bounds.left = std::min(bg_bounds.left, text_rect.left);
			text_bg_bounds.right = std::max(bg_bounds.right, text_rect.right);

			dc.draw_rounded_rect(text_bg_bounds, ui::color(ui::style::color::view_background, text_alpha * 0.888f),
			                     dc.padding1);

			recti title_rect(text_rect.left, y, text_rect.right, y + title_line_height1 + title_line_height2);

			if (!is_empty(info.title))
			{
				auto bounds = title_rect;
				bounds.bottom = title_rect.top + title_line_height1;
				dc.draw_text(info.title, bounds, info.title_font, title_style, group_text_color, {});
				y += title_line_height1;
			}

			if (!is_empty(info.name))
			{
				auto bounds = title_rect;
				bounds.top = title_rect.top + title_line_height1;
				dc.draw_text(info.name, bounds, info.title_font, title_style, group_text_color, {});
				y += title_line_height2;
			}
		}

		if (show_info)
		{
			recti info_rect(text_rect.left, y, text_rect.right, y + text_line_height);
			dc.draw_text(info.info, info_rect, text_font, text_style, text_color, {});
			y += text_line_height;
		}

		if (show_size || show_created || show_modified)
		{
			recti order_text_rect(text_rect.left, y, text_rect.right, y + text_line_height);
			std::string text;

			if (show_size)
			{
				text = df::format_size(info.size);
			}
			else if (show_created)
			{
				text = prop::format_date(created_shown_is_file ? info.file_created : info.created);
			}
			else if (show_modified)
			{
				text = prop::format_date(info.modified);
			}

			dc.draw_text(text, order_text_rect, text_font, ui::style::text_style::single_line_center, text_color, {});
			y += text_line_height;
		}

		if (show_stars)
		{
			const auto rating = info.rating;

			if (rating >= 1 && rating <= 5)
			{
				auto x = (text_rect.left + text_rect.right - dc.icon_cxy * rating) / 2;
				y += 2;

				for (auto ii = 0; ii < rating; ii++)
				{
					recti r(x, y, x + dc.icon_cxy, y + stars_line_height);
					xdraw_icon(dc, icon_index::star_solid, r, text_color, {});
					x += dc.icon_cxy;
				}
			}
			else if (rating < 0)
			{
				recti label_bounds(text_rect.left, y, text_rect.right, y + stars_line_height);
				dc.draw_text(tt.command_rate_rejected, label_bounds, text_font,
				             ui::style::text_style::single_line_center, ui::color(color_rate_rejected, text_alpha), {});
			}

			y += stars_line_height;
		}

		// Badges are anchored to the tile rather than the text block. Hover/focus expands the
		// title below the tile, so move the badge row upward to keep it attached to the image.
		const auto y_flag = device_bounds.top - (expand_text ? cxy_flag : 0);
		auto x_flag = device_bounds.left;

		// Drawn at full alpha: the pin must stay readable over a thumbnail that has dimmed the rest
		// of the badge row.
		if (is_pinned)
		{
			const recti bb(x_flag, y_flag, x_flag + cxy_flag, y_flag + cxy_flag);
			draw_pin_badge(dc, bb, dc.colors.alpha);
			_pin_badge_bounds = bb.offset(-element_offset.x, -element_offset.y);
			// Expanded text lifts the badge row above the tile, so the hit test has to follow it.
			_interactive_bounds = _interactive_bounds.make_union(_pin_badge_bounds);
			x_flag += bb.width();
		}

		if (can_show_flag(info))
		{
			const recti bb(x_flag, y_flag, x_flag + cxy_flag, y_flag + cxy_flag);
			draw_flag(dc, info, bb, text_alpha);
			x_flag += bb.width();
		}

		if (show_text)
		{
			if (!has_related && can_show_duplicates(info))
			{
				const auto text = str::format_count(info.duplicates);
				const auto cx = std::max(cxy_flag, dc.measure_text(text, ui::style::font_face::dialog,
				                                                   ui::style::text_style::single_line_center, 100).cx);
				const recti bb(x_flag, y_flag, x_flag + cx, y_flag + cxy_flag);
				dc.draw_text(text, bb, ui::style::font_face::dialog, ui::style::text_style::single_line_center,
				             text_color, bg_dups);
				x_flag += cx;
			}

			if (info.sidecars > 0)
			{
				const auto text = str::format_count(info.sidecars);
				const auto cx = std::max(cxy_flag, dc.measure_text(text, ui::style::font_face::dialog,
				                                                   ui::style::text_style::single_line_center, 100).cx);
				const recti bb(x_flag, y_flag, x_flag + cx, y_flag + cxy_flag);

				dc.draw_text(text, bb, ui::style::font_face::dialog, ui::style::text_style::single_line_center,
				             text_color, bg_sidecars);
				x_flag += cx;
			}

			if (info.online_status != item_online_status::disk)
			{
				const recti bb(x_flag, y_flag, x_flag + cxy_flag, y_flag + cxy_flag);
				xdraw_icon(dc, icon_index::cloud, bb, text_color, bg_disk);
				x_flag += bb.width();
			}
		}
	}
}

void df::item_element::stage_thumbnail_surface(async_strategy& async, const bool invalidate_on_complete,
                                               const bool tooltip_on_complete) const
{
	df::assert_true(ui::is_ui_thread());
	bump(thumbnail_perf.stage_requests);

	if ((_thumbnail_state && thumbnail_state::surface_cached) ||
		(!ui::is_valid(_thumbnail) && !ui::is_valid(_cover_art)))
	{
		bump(thumbnail_perf.stage_skipped);

		auto invalid = view_invalid::none;
		if (invalidate_on_complete || (_thumbnail_state && thumbnail_state::invalidate_on_stage))
		{
			set_thumbnail_state(thumbnail_state::invalidate_on_stage, false);
			invalid |= view_invalid::view_redraw;
		}
		if ((_thumbnail_state && thumbnail_state::surface_cached) &&
			(_thumbnail_state && thumbnail_state::tooltip_on_stage))
		{
			set_thumbnail_state(thumbnail_state::tooltip_on_stage, false);
			invalid |= view_invalid::tooltip | view_invalid::view_redraw;
		}
		if (invalid != view_invalid::none) async.invalidate_view(invalid);

		return;
	}

	// Latched, not captured: a request arriving while a stage is in flight would otherwise be
	// discarded along with its callback, and that branch runs about as often as a stage completes.
	if (invalidate_on_complete) set_thumbnail_state(thumbnail_state::invalidate_on_stage, true);
	if (tooltip_on_complete) set_thumbnail_state(thumbnail_state::tooltip_on_stage, true);

	if (_thumbnail_state && thumbnail_state::staging_surface)
	{
		bump(thumbnail_perf.stage_coalesced);
		set_thumbnail_state(thumbnail_state::staging_requested, true);
		return;
	}

	bump(thumbnail_perf.stage_decodes);
	set_thumbnail_state(thumbnail_state::staging_surface, true);
	const auto weak = weak_from_this();
	const auto generation = _thumbnail_surface_generation;
	const auto thumbnail = _thumbnail;
	const auto cover_art = _cover_art;
	// Read here rather than on the worker: the backend owns it and only changes it on this thread.
	const auto can_use_yuv = ui::yuv_textures_enabled;

	async.queue_async(async_queue::render,
	                  [weak, generation, thumbnail, cover_art, can_use_yuv, &async]() mutable
	                  {
		                  files ff;
		                  auto thumbnail_surface = ui::is_valid(thumbnail)
			                                           ? ff.image_to_surface(thumbnail, {}, can_use_yuv)
			                                           : nullptr;
		                  auto cover_art_surface = ui::is_valid(cover_art)
			                                           ? ff.image_to_surface(cover_art, {}, can_use_yuv)
			                                           : nullptr;

		                  async.queue_ui(
			                  [weak, generation, thumbnail_surface = std::move(thumbnail_surface),
				                  cover_art_surface = std::move(cover_art_surface), &async]() mutable
			                  {
				                  const auto item = weak.lock();
				                  if (!item) return;

				                  df::assert_true(ui::is_ui_thread());
				                  item->set_thumbnail_state(thumbnail_state::staging_surface, false);
				                  const auto was_current = generation == item->_thumbnail_surface_generation;
				                  if (was_current)
				                  {
					                  item->_texture.reset();
					                  item->_thumbnail_surface = std::move(thumbnail_surface);
					                  item->_cover_art_surface = std::move(cover_art_surface);
					                  item->set_thumbnail_state(thumbnail_state::surface_cached,
					                                            item->_thumbnail_surface || item->_cover_art_surface);

					                  // The selection panel measures the displayed item's cover art, so a
					                  // result carrying it changes the panel's layout, not only its pixels.
					                  if (item->_cover_art_surface && item->is_selected())
					                  {
						                  async.invalidate_view(
							                  view_invalid::view_layout | view_invalid::view_redraw);
					                  }
				                  }
				                  else
				                  {
					                  // The item moved on while the surface was decoding, so the decode was wasted work.
					                  bump(thumbnail_perf.stage_discarded);
				                  }

				                  if (item->_thumbnail_state && thumbnail_state::staging_requested)
				                  {
					                  // invalidate_on_stage stays latched so the follow-up stage settles it.
					                  item->set_thumbnail_state(thumbnail_state::staging_requested, false);
					                  item->stage_thumbnail_surface(async, false);
				                  }
				                  else if (item->_thumbnail_state &&
					                  (thumbnail_state::invalidate_on_stage | thumbnail_state::tooltip_on_stage))
				                  {
					                  const auto has_surface = was_current && item->has_cached_surface();
					                  auto invalid = view_invalid::none;
					                  if (item->_thumbnail_state && thumbnail_state::invalidate_on_stage)
					                  {
						                  invalid |= view_invalid::view_redraw;
					                  }
					                  if (has_surface && (item->_thumbnail_state && thumbnail_state::tooltip_on_stage))
					                  {
						                  invalid |= view_invalid::tooltip | view_invalid::view_redraw;
					                  }
					                  item->set_thumbnail_state(thumbnail_state::invalidate_on_stage, false);
					                  item->set_thumbnail_state(thumbnail_state::tooltip_on_stage, false);
					                  if (invalid != view_invalid::none) async.invalidate_view(invalid);
				                  }
			                  });
	                  });
}

void df::item_element::start_thumbnail_animation(view_state& state) const
{
	_thumbnail_alpha.reset(0.0f, 1.0f);
	const auto item = shared_from_this();

	ui::animations[const_cast<item_element*>(this)] = [item, &state]
	{
		if (!item->is_visible())
		{
			item->_thumbnail_alpha.reset(1.0f);
			return false;
		}

		const auto previous = item->_thumbnail_alpha.val();
		const auto animating = item->_thumbnail_alpha.step();

		if (item->_thumbnail_alpha.val() != previous)
		{
			state.invalidate_view(view_invalid::view_redraw);
		}

		return animating;
	};

	state.invalidate_view(view_invalid::animations | view_invalid::view_redraw);
}

sizei df::item_element::measure(ui::measure_context& mc, const int width_limit) const
{
	assert_true(false);
	return {0, 0};
}

void df::item_element::layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions)
{
	bounds = bounds_in;
	_interactive_bounds = bounds_in;
}

view_controller_ptr df::item_element::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                               const pointi element_offset,
                                                               hit_test_context& ctx)
{
	return nullptr;
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////


size_t df::trim_thumbnail_blobs(const item_elements& items, const recti viewport, const size_t budget_bytes)
{
	assert_true(ui::is_ui_thread());

	// Distance from an empty viewport says nothing, and answering it anyway would rank every item as
	// maximally far and dump the whole set whenever Items is not the laid-out view.
	if (viewport.height() <= 0) return 0;

	// Distance is measured in viewport heights, so the policy is the same whatever the window size
	// and the item scale. Eight is past any plausible scroll-back; beyond it everything is equal.
	constexpr size_t band_count = 8;
	const auto screen = viewport.height();

	const auto band_of = [screen, viewport](const recti bounds)
	{
		const auto distance = bounds.bottom < viewport.top
			                      ? viewport.top - bounds.bottom
			                      : (bounds.top > viewport.bottom ? bounds.top - viewport.bottom : 0);
		return std::min(static_cast<size_t>(distance / screen), band_count - 1);
	};

	std::array<size_t, band_count> bytes_by_band{};
	size_t total = 0;

	for (const auto& i : items)
	{
		const auto bytes = i->thumbnail_blob_bytes();
		if (bytes == 0) continue;
		total += bytes;
		bytes_by_band[band_of(i->bounds)] += bytes;
	}

	if (total <= budget_bytes) return 0;

	// Band 0 is kept whatever it costs: a budget that blanks what is on screen is worse than the
	// memory it saves. Bands are then added outward while they still fit.
	auto kept = bytes_by_band[0];
	size_t keep_bands = 1;

	while (keep_bands < band_count && kept + bytes_by_band[keep_bands] <= budget_bytes)
	{
		kept += bytes_by_band[keep_bands];
		++keep_bands;
	}

	size_t freed = 0;

	for (const auto& i : items)
	{
		if (band_of(i->bounds) >= keep_bands) freed += i->clear_thumbnail_blob();
	}

	return freed;
}

std::vector<ui::const_image_ptr> df::item_set::thumbs(const size_t max, const item_element_ptr& skip_this) const
{
	std::vector<ui::const_image_ptr> results;

	for (const auto& i : _items)
	{
		if (results.size() >= max)
			break;

		if (i->has_thumb() && i != skip_this)
		{
			results.emplace_back(i->thumbnail());
		}
	}

	return results;
}

std::vector<ui::const_surface_ptr> df::item_set::thumbnail_surfaces(const size_t max,
                                                                    const item_element_ptr& skip_this) const
{
	std::vector<ui::const_surface_ptr> results;

	for (const auto& i : _items)
	{
		if (results.size() >= max) break;
		if (i == skip_this) continue;

		if (const auto& surface = i->thumbnail_surface(); is_valid(surface))
		{
			results.emplace_back(surface);
		}
		else if (const auto cover = i->cover_art_surface(); is_valid(cover))
		{
			results.emplace_back(cover);
		}
	}

	return results;
}

void df::item_set::stage_thumbnail_surfaces(async_strategy& async, const size_t max,
                                            const item_element_ptr& skip_this) const
{
	auto requested = 0_z;

	for (const auto& i : _items)
	{
		if (requested >= max) break;
		if (i == skip_this) continue;
		if (!i->has_thumb()) continue;

		i->stage_thumbnail_surface(async, true);
		++requested;
	}
}

df::process_result df::item_set::can_process(const process_items_type file_types, const bool mark_errors,
                                             const view_host_base_ptr& view) const
{
	process_result result;

	if (file_types != process_items_type::local_file_or_folder)
	{
		for (const auto& i : _items)
		{
			if (i->is_folder())
			{
				result.record_error(i, process_result_code::folder, mark_errors, view);
			}
		}
	}

	for (const auto& i : _items)
	{
		const auto* const ft = i->file_type();

		if (file_types == process_items_type::photos_only && !ft->has_trait(file_traits::bitmap))
		{
			result.record_error(i, process_result_code::not_photo, mark_errors, view);
		}

		if (file_types == process_items_type::can_save_pixels)
		{
			if (i->is_read_only())
			{
				result.record_error(i, process_result_code::read_only, mark_errors, view);
			}
			else if (!files::can_save(i->path()))
			{
				result.record_error(i, process_result_code::cannot_save_pixels, mark_errors, view);
			}
		}

		if (file_types == process_items_type::can_save_metadata)
		{
			if (ft->has_trait(file_traits::embedded_xmp) && i->is_read_only())
			{
				result.record_error(i, process_result_code::cannot_embed_xmp, mark_errors, view);
			}

			if (!ft->has_trait(file_traits::edit))
			{
				result.record_error(i, process_result_code::cannot_edit, mark_errors, view);
			}
		}
	}

	return result;
}

void df::item_element::add_to(item_set& results)
{
	results.add(shared_from_this());
}

void df::item_element::add_to(paths& results)
{
	if (is_folder())
	{
		results.folders.emplace_back(_path.folder());
	}
	else
	{
		results.files.emplace_back(_path);
	}
}

void df::item_element::add_to(unique_paths& paths)
{
	paths.emplace(_path);
}

df::item_display_info df::item_element::populate_info() const
{
	item_display_info result;

	if (is_folder())
	{
		result.icon = _path.folder().is_drive() ? icon_index::disk : icon_index::folder;
		result.name = _name;
		result.title_font = ui::style::font_face::title;

		result.title = _info->volume;
		result.items = static_cast<int>(_total_count);
		result.size = _size;
		result.modified = _modified.system_to_local();
		result.created = media_created();
	}
	else
	{
		const auto* const mt = file_type();

		result.icon = icon_index::document;
		result.folder = _path.folder().text();
		result.online_status = _online_status;
		result.presence = _presence;
		result.name = _name;

		if (!is_link())
		{
			result.size = _size;
			result.duplicates = _duplicates.count;

			const auto& md = _metadata;

			if (md)
			{
				result.title = md->title;
				result.track = md->track.x;
				result.disk = md->disk.x;
				result.duration = md->duration;
				result.rating = md->rating;
				result.label = md->label;
				result.sidecars = static_cast<int>(sidecars_count());
				result.bitrate = md->bitrate;
				result.pixel_format = md->pixel_format;
				result.dimensions = md->dimensions();
				result.audio_channels = md->audio_channels;
				result.audio_sample_rate = md->audio_sample_rate;
				result.audio_sample_type = md->audio_sample_type;
			}

			result.icon = mt->icon;
			result.modified = _modified.system_to_local();
			result.created = media_created();
			result.file_created = file_or_metadata_created();
		}
	}

	return result;
}

void df::item_element::open(view_state& s, const view_host_base_ptr& view) const
{
	if (is_folder())
	{
		s.open(view, search_t().add_selector(_path.folder()), {});
	}
	else if (!is_media())
	{
		platform::open(path());
	}
}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void df::item_element::refresh_layout_dims()
{
	assert_true(ui::is_ui_thread());

	if (_ft == file_type::folder) return;

	// Cover art is a different picture from the media, not a downscaled copy of it, and it is what the
	// tile draws - so it owns the tile's shape even for a video whose own dimensions the index knows.
	// Letting the video win made the art letterbox inside a frame-shaped tile.
	if (is_valid(_cover_art))
	{
		_layout_dims = _cover_art->dimensions();
		_layout_orientation = _cover_art->orientation();
		_layout_aspect_known = true;
		return;
	}

	if (_metadata && _metadata->width > 0 && _metadata->height > 0)
	{
		_layout_dims = _metadata->dimensions();
		_layout_orientation = _metadata->orientation;
		_layout_aspect_known = true;
		return;
	}

	// A thumbnail is a downscaled representation, so its pixel size is only a stand-in until the real
	// aspect arrives - and a stand-in must never be stretched to justify a row.
	if (is_valid(_thumbnail))
	{
		_layout_dims = _thumbnail->dimensions();
		_layout_orientation = _thumbnail->orientation();
		_layout_aspect_known = false;
	}
}

bool df::item_element::update(const file_path path, const index_file_item& info) noexcept
{
	assert_true(!is_folder());

	const auto md = info.metadata.load();

	// scan_items republishes every displayed item on every pass, whether or not the index record
	// changed. Marking each one dirty unconditionally forced a full row relayout per pass, so the
	// layout-relevant fields are compared and row_layout_valid is only cleared on a real change.
	const auto previous_name = _name;
	const auto previous_path = _path;
	const auto previous_ft = _ft;
	const auto previous_size = _size;
	const auto previous_modified = _modified;
	const auto previous_created = _created;
	const auto previous_media_created = _media_created;
	const auto previous_metadata = _metadata;
	const auto previous_duplicates = _duplicates;
	const auto previous_crc32c = _crc32c;
	const auto previous_online_status = _online_status;
	const auto previous_is_read_only = _is_read_only;
	const auto previous_layout_dims = _layout_dims;
	const auto previous_layout_orientation = _layout_orientation;

	_name = path.name();
	const auto new_online_status = info.calc_online_status();

	if (_online_status == item_online_status::offline && new_online_status == item_online_status::disk)
	{
		// A cloud-only placeholder was hydrated: allow its thumbnail to load now that the
		// full file content is available locally.
		set_thumbnail_state(thumbnail_state::load_failed | thumbnail_state::shell_pending |
		                    thumbnail_state::shell_retry_pending, false);
		_shell_retry_count = 0;
	}

	_online_status = new_online_status;
	_is_read_only = info.flags && index_item_flags::is_read_only;
	_path = path;
	_ft = info.ft;
	_size = info.size;
	_modified = info.file_modified;
	_created = info.file_created;

	// Only the update that actually carries the write's new modified time may consume the flag; an
	// unrelated republish landing first would otherwise disarm it and let the write reload anyway.
	if (_retain_thumbnail_on_modify && _modified != previous_modified)
	{
		_retain_thumbnail_on_modify = false;
		if (is_valid(_thumbnail) || is_valid(_cover_art)) _thumbnail_timestamp = _modified;
	}
	_duplicates = info.duplicates.load();
	_metadata = md;
	if (!_media_position_changed)
	{
		_media_position = md ? md->media_position : 0.0;
	}
	_crc32c = info.crc32c.load();
	_media_created = calc_media_created();

	if (!_ft && !is_empty(_name))
	{
		_ft = files::file_type_from_name(_name);
	}

	// Indexed dimensions are the intrinsic size of the media, so the tile keeps the same geometry
	// before, during and after its thumbnail loads - unless the item carries cover art, which is what
	// the tile actually draws.
	refresh_layout_dims();

	_is_folder = _ft == file_type::folder;

	const auto changed = previous_name != _name ||
		previous_path != _path ||
		previous_ft != _ft ||
		previous_size != _size ||
		previous_modified != _modified ||
		previous_created != _created ||
		previous_media_created != _media_created ||
		previous_metadata != _metadata ||
		previous_duplicates != _duplicates ||
		previous_crc32c != _crc32c ||
		previous_online_status != _online_status ||
		previous_is_read_only != _is_read_only ||
		previous_layout_dims != _layout_dims ||
		previous_layout_orientation != _layout_orientation;

	if (changed)
	{
		row_layout_valid = false;
	}

	return changed;
}

static void notify_rename_after_move(const df::file_path source, const df::file_path destination)
{
	if (df::rename_after_move_for_test) df::rename_after_move_for_test(source, destination);
}

static platform::file_op_result rename_recovery_failed(const df::file_path expected_path,
                                                       const df::file_path surviving_path,
                                                       platform::file_op_result result)
{
	result.code = platform::file_op_result_code::FAILED;
	result.error_message = std::format("Could not restore {}; surviving path is {}. {}",
	                                   expected_path.str(), surviving_path.str(), result.format_error());
	if (df::rename_recovery_report_for_test) df::rename_recovery_report_for_test(result.error_message);
	df::log(__FUNCTION__, result.error_message);
	return result;
}

static platform::file_op_result rename_file(const df::file_path source, const df::file_path destination)
{
	if (source.icmp(destination) != 0 || source.pack() == destination.pack())
	{
		auto result = platform::move_file(source, destination, true);
		if (result.success()) notify_rename_after_move(source, destination);
		return result;
	}

	const auto temporary = platform::temp_file(source.extension(), source.folder());
	auto result = platform::move_file(source, temporary, true);
	if (result.success())
	{
		notify_rename_after_move(source, temporary);
		result = platform::move_file(temporary, destination, true);
		if (result.success())
		{
			notify_rename_after_move(temporary, destination);
		}
		else
		{
			const auto rollback = platform::move_file(temporary, source, true);
			if (rollback.success()) notify_rename_after_move(temporary, source);
			else return rename_recovery_failed(source, temporary, rollback);
		}
	}
	return result;
}

platform::file_op_result df::item_element::rename(index_state& index, const std::string_view new_name)
{
	if (is_folder())
	{
		const auto path_src = folder();
		const auto path_dst = path_src.parent().combine(new_name);

		if (path_src.text() == path_dst.text())
		{
			// no-op
			platform::file_op_result result;
			result.code = platform::file_op_result_code::OK;
			return result;
		}

		if (!path_dst.is_save_valid())
		{
			platform::file_op_result result;
			result.error_message = format_invalid_name_message(new_name);
			return result;
		}

		auto result = platform::move_file(path_src, path_dst);

		if (result.success())
		{
			notify_rename_after_move(df::file_path(path_src.parent(), path_src.name()),
			                         df::file_path(path_dst.parent(), path_dst.name()));
			_path.folder(path_dst);
			_name = path_dst.name();
		}

		return result;
	}
	const auto path_src = path();
	const auto path_dst = file_path(path_src.folder(), new_name, path_src.extension());

	if (path_src.pack() == path_dst.pack())
	{
		// no-op
		platform::file_op_result result;
		result.code = platform::file_op_result_code::OK;
		return result;
	}

	if (!path_dst.is_save_valid())
	{
		platform::file_op_result result;
		result.error_message = format_invalid_name_message(new_name);
		return result;
	}

	std::vector<std::pair<file_path, file_path>> moved_sidecars;
	const auto sidecar_parts = split(sidecars(), true);

	for (const auto& file_name : sidecar_parts)
	{
		const auto folder_path = path_src.folder();
		const auto sidecar_path_src = folder_path.combine_file(file_name);
		const auto sidecar_path_dst = folder_path.combine_file(new_name).extension(sidecar_path_src.extension());
		auto result = rename_file(sidecar_path_src, sidecar_path_dst);

		if (result.failed())
		{
			platform::file_op_result first_rollback_failure{platform::file_op_result_code::OK};
			for (auto i = moved_sidecars.rbegin(); i != moved_sidecars.rend(); ++i)
			{
				const auto rollback = rename_file(i->second, i->first);
				if (rollback.failed())
				{
					const auto reported = rename_recovery_failed(i->first, i->second, rollback);
					if (first_rollback_failure.success()) first_rollback_failure = reported;
				}
			}
			if (first_rollback_failure.failed())
			{
				index.queue_scan_folders(df::unique_folders{path_src.folder(), path_dst.folder()});
				return first_rollback_failure;
			}
			return result;
		}
		moved_sidecars.emplace_back(sidecar_path_src, sidecar_path_dst);
	}

	auto result = rename_file(path_src, path_dst);
	if (result.success())
	{
		_path = file_path(_path.folder(), _name = path_dst.name());
	}
	else
	{
		platform::file_op_result first_rollback_failure{platform::file_op_result_code::OK};
		for (auto i = moved_sidecars.rbegin(); i != moved_sidecars.rend(); ++i)
		{
			const auto rollback = rename_file(i->second, i->first);
			if (rollback.failed())
			{
				const auto reported = rename_recovery_failed(i->first, i->second, rollback);
				if (first_rollback_failure.success()) first_rollback_failure = reported;
			}
		}
		if (first_rollback_failure.failed())
		{
			index.queue_scan_folders(df::unique_folders{path_src.folder(), path_dst.folder()});
			return first_rollback_failure;
		}
	}

	return result;
}
