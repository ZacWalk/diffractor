// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Sidebar navigation panel. Contains collection overview charts, folder list,
// favorite searches, tags, ratings, labels, and drive information displays. Also holds the
// application logo lockup shared by the top bar and the About box.
//
// The sidebar is a projection of the index, so what it costs to refresh depends entirely on which
// part of it the change actually touched. Everything used to arrive through one flag that meant
// "rebuild all of it", which put an index query per row and a volume enumeration behind events that
// had changed neither. The work is separated into four tiers, each with its own invalidation:
//
//   view_invalid::sidebar         structure - which rows exist. Rebuilds every element, so it is
//                                 earned only by a change to the collection roots, the saved
//                                 searches, the tag vocabulary or the sidebar's own settings.
//   sidebar_file_types_and_dates  projection - the type and history charts, read straight from the
//                                 index histograms. No queries, no element churn.
//   sidebar_counts                the per-row sums. One index query per row, so it never rides
//                                 along with a structural rebuild it did not need.
//   sidebar_drives                volume enumeration, which blocks on unreachable network mappings.
//
// Two rules follow from that split, and both are load-bearing:
//
// Counts are not attempted before the index reports is_init_complete. Every query would answer zero
// for a collection that is not empty, the answer is discarded at draw time, and the pass spends its
// time contending for the index locks with the thread still building the index. Rows show the
// loading affordance until the answer can be real; each index phase that completes asks again.
//
// Changing the chrome above the rows must not rebuild the rows. compose_elements() exists so that
// showing or hiding one element - the indexing progress control appears and disappears on every
// index run - costs a list splice rather than recreating every element, its text layout and its sum.

#pragma once
#include "ui_charts.h"
#include "ui_controls.h"
#include "ui_globe.h"
#include "ui_plasma.h"
#include "app_util.h"

static std::string format_total_text(const df::count_and_size total, const bool multi_line)
{
	std::string result;

	if (total.count > 0)
	{
		result = str::format_count(total.count);
	}

	if (multi_line)
	{
		if (!total.size.is_empty())
		{
			result += "\n";
			result += df::format_size(total.size);
		}
	}

	return result;
}

struct text_layout_state
{
	ui::text_layout_ptr tf;
	std::string text;
	ui::style::font_face font = ui::style::font_face::dialog;
	ui::style::text_style style = ui::style::text_style::multiline;
	std::string _built_text; // the value of `text` that tf was last built from

	void lazy_load(ui::measure_context& mc)
	{
		// Rebuild when tf is missing OR when `text` was reassigned directly (e.g. a
		// language change re-populates a reused item and updates .text without going
		// through the text-taking overload). Without this the stale glyph layout would
		// keep rendering the previous language until restart.
		if (!tf || _built_text != text)
		{
			tf = mc.create_text_layout(font);
			tf->update(text, style);
			_built_text = text;
		}
	}

	void lazy_load(ui::measure_context& mc, const std::string& text_in, const ui::style::text_style style_in,
	               const ui::style::font_face font_in = ui::style::font_face::dialog)
	{
		const auto need_create = tf == nullptr ||
			text != text_in ||
			font != font_in ||
			style != style_in;

		if (need_create)
		{
			text = text_in;
			style = style_in;
			font = font_in;
			tf = mc.create_text_layout(font);
			tf->update(text, style);
			_built_text = text;
		}
	}

	bool is_empty() const
	{
		return text.empty();
	}

	void dpi_changed()
	{
		tf.reset();
	}
};

struct sidebar_tooltip_thumbnail
{
	df::file_path path;
	df::item_element_ptr item;
	bool requested = false;

	void add(const view_hover_element& hover, const view_state& state, const df::file_path& representative_path)
	{
		if (representative_path.is_empty()) return;
		if (path != representative_path)
		{
			path = representative_path;
			item.reset();
			requested = false;
		}

		if (!item)
		{
			const auto indexed = state.item_index.find_item(path);
			if (indexed.ft) item = std::make_shared<df::item_element>(path, indexed);
		}
		if (!item) return;

		const auto surface = item->thumbnail_surface();
		if (is_valid(surface))
		{
			hover.elements->add(std::make_shared<surface_element>(
				surface, 160, flex_item::center | flex_item::new_line, item->layout_orientation()));
		}
		else if (is_valid(item->thumbnail()))
		{
			item->stage_thumbnail_surface(state._async, false, true);
		}
		else if (!requested)
		{
			requested = true;
			state.item_index.queue_load_thumbnail(item);
		}
	}
};

class sidebar_element final : public view_element, public std::enable_shared_from_this<sidebar_element>
{
public:
	view_state& _state;

	mutable text_layout_state icon_layout;
	mutable text_layout_state title_layout;
	mutable text_layout_state total_layout;

	icon_index icon = icon_index::none;
	icon_index tooltip_icon = icon_index::none;
	std::string tooltip_text;
	std::string key;
	df::search_t search;
	std::function<df::file_group_histogram(view_state& s, df::cancel_token token)> calc_sum;
	df::file_group_histogram summary;
	// Distinguishes "not counted yet" from "counted, and the answer is none" - both render blank.
	bool summary_known = false;
	ui::color32 clr = 0;
	int icon_repeat = 1;
	mutable sidebar_tooltip_thumbnail _tooltip_thumbnail;

	explicit sidebar_element(view_state& state) noexcept
		: view_element(view_element_style::has_tooltip | view_element_style::can_invoke), _state(state)
	{
	}

	// A counted-but-empty row stays blank rather than showing a zero, so the sidebar does not become a
	// column of noughts on a search that simply matches nothing.
	std::string summary_text() const
	{
		if (!summary_known) return std::string(tt.loading.sv());
		return format_total_text(summary.total_items(), false);
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		render_background(dc, element_offset);

		const auto draw_clr = clr
			                      ? ui::color(ui::average(clr, dc.colors.foreground), dc.colors.alpha)
			                      : ui::color(dc.colors.foreground, dc.colors.alpha);
		const auto has_text = !title_layout.is_empty();
		const auto has_image = icon != icon_index::none;

		auto x = logical_bounds.left;

		if (has_image)
		{
			const auto cx = dc.icon_cxy * icon_repeat;

			auto r = logical_bounds;
			r.left = x;
			r.right = x + cx;

			icon_layout.lazy_load(dc, icon_to_utf8(icon, icon_repeat), ui::style::text_style::single_line_center,
			                      ui::style::font_face::icons);
			dc.draw_text(icon_layout.tf, r, draw_clr, {});
			x += cx + dc.padding1;
		}

		auto text_bounds = logical_bounds;
		text_bounds.left = x;

		const auto total_text = summary_text();

		if (!str::is_empty(total_text))
		{
			total_layout.lazy_load(dc, total_text, ui::style::text_style::single_line_far);
			const auto sum_text_extent = total_layout.tf->measure_text(text_bounds.width());
			dc.draw_text(total_layout.tf, text_bounds, draw_clr, {});
			text_bounds.right = text_bounds.right - sum_text_extent.cx;
		}

		if (has_text)
		{
			title_layout.lazy_load(dc);
			dc.draw_text(title_layout.tf, text_bounds, draw_clr, {});
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		auto cy = mc.text_line_height(ui::style::font_face::dialog);
		auto cx = width_limit;
		const auto total_text = summary_text();

		if (icon != icon_index::none)
		{
			cx -= mc.icon_cxy + mc.padding1;
		}

		if (!str::is_empty(total_text))
		{
			total_layout.lazy_load(mc, total_text, ui::style::text_style::single_line_far);
			const auto sum_text_extent = total_layout.tf->measure_text(cx);
			cx -= sum_text_extent.cx;
		}

		if (!title_layout.is_empty())
		{
			title_layout.lazy_load(mc);
			const auto text_extent = title_layout.tf->measure_text(cx);
			cy = std::max(cy, text_extent.cy);
		}

		return {width_limit, cy};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void tooltip(view_hover_element& result, const pointi loc, const pointi element_offset) const override
	{
		if (title_layout.is_empty())
		{
			result.elements->add(make_icon_element(tooltip_icon, icon_repeat, flex_item::line_break));
		}
		else
		{
			result.elements->add(make_icon_element(tooltip_icon, icon_repeat, flex_item::no_break));
			result.elements->add(std::make_shared<text_element>(title_layout.text, ui::style::font_face::title,
			                                                    ui::style::text_style::multiline,
			                                                    flex_item::line_break));
		}

		_tooltip_thumbnail.add(result, _state, summary.representative_path);

		if (str::is_empty(tooltip_text))
		{
			result.elements->add(std::make_shared<text_element>(search.text(), ui::style::font_face::dialog,
			                                                    ui::style::text_style::multiline,
			                                                    flex_item::new_line));
		}
		else
		{
			result.elements->add(std::make_shared<text_element>(tooltip_text, ui::style::font_face::dialog,
			                                                    ui::style::text_style::multiline,
			                                                    flex_item::new_line));
		}

		result.elements->add(std::make_shared<summary_control>(summary, flex_item::new_line));
		result.active_bounds = result.window_bounds = bounds.offset(element_offset);
		result.horizontal = true;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			_state.open(event.host, search, {});
		}
		else if (event.type == view_element_event_type::dpi_changed)
		{
			icon_layout.dpi_changed();
			title_layout.dpi_changed();
			total_layout.dpi_changed();
		}
	}
};

class sidebar_drive_element final : public view_element, public std::enable_shared_from_this<sidebar_drive_element>
{
public:
	mutable text_layout_state icon_layout;
	mutable text_layout_state title_layout;
	mutable text_layout_state total_layout;

	view_state& _state;
	icon_index icon = icon_index::none;
	platform::drive_t _drive;
	const int _graph_height = 8;

	explicit sidebar_drive_element(view_state& state, const platform::drive_t& d) noexcept
		: view_element(view_element_style::has_tooltip | view_element_style::can_invoke), _state(state), _drive(d)
	{
		if (!str::is_empty(_drive.vol_name))
		{
			title_layout.text = std::format("{} ({})", _drive.name, _drive.vol_name);
		}
		else
		{
			title_layout.text = _drive.name;
		}

		icon = drive_icon(_drive.type);
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		render_background(dc, element_offset);

		const auto pad1 = df::round(1 * dc.scale_factor);
		const auto graph_height = df::round(_graph_height * dc.scale_factor);
		const auto has_image = icon != icon_index::none;
		const auto draw_clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		auto x = logical_bounds.left;

		if (has_image)
		{
			auto r = logical_bounds;
			r.right = r.left + dc.icon_cxy;
			r.bottom -= graph_height;
			icon_layout.lazy_load(dc, icon_to_utf8(icon), ui::style::text_style::single_line_center,
			                      ui::style::font_face::icons);
			dc.draw_text(icon_layout.tf, r, draw_clr, {});
			x += dc.icon_cxy + dc.padding1;
		}

		auto text_bounds = logical_bounds;
		text_bounds.left = x;
		text_bounds.bottom -= graph_height + dc.padding1;

		auto graph_bounds = logical_bounds;
		graph_bounds.top = text_bounds.bottom + dc.padding1;

		dc.draw_rect(graph_bounds, ui::color(ui::style::color::sidebar_background, draw_clr.a).scale(1.22f));

		// Capacity is zero for a drive with no media inserted; there is no usage to plot.
		if (!_drive.capacity.is_empty())
		{
			auto used_bounds = graph_bounds.inflate(-pad1);
			used_bounds.right = used_bounds.left + static_cast<int>(df::mul_div(
				static_cast<int64_t>(used_bounds.width()), static_cast<int64_t>(_drive.used.to_int64()),
				static_cast<int64_t>(_drive.capacity.to_int64())));

			dc.draw_rect(used_bounds,
			             ui::color(
				             ui::average(ui::style::color::sidebar_background, ui::style::color::important_background),
				             draw_clr.a));
		}

		const auto total_text = std::format("{}|{}", df::format_size(_drive.used),
		                                    df::format_size(_drive.capacity));

		if (!str::is_empty(total_text))
		{
			total_layout.lazy_load(dc, total_text, ui::style::text_style::single_line_far);
			const auto sum_text_extent = total_layout.tf->measure_text(text_bounds.width());
			dc.draw_text(total_layout.tf, text_bounds, draw_clr, {});
			text_bounds.right = text_bounds.right - sum_text_extent.cx;
		}

		title_layout.lazy_load(dc, _drive.name, ui::style::text_style::single_line);
		dc.draw_text(title_layout.tf, text_bounds, draw_clr, {});
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto cy = mc.text_line_height(ui::style::font_face::dialog);
		const auto graph_height = df::round(_graph_height * mc.scale_factor);
		return {width_limit, cy + graph_height + mc.padding1};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void tooltip(view_hover_element& result, const pointi loc, const pointi element_offset) const override
	{
		result.elements->add(make_icon_element(icon, flex_item::no_break));
		result.elements->add(std::make_shared<text_element>(_drive.name, ui::style::font_face::title,
		                                                    ui::style::text_style::multiline,
		                                                    flex_item::line_break));


		const auto table = std::make_shared<ui::table_element>(flex_item::center);

		if (!str::is_empty(_drive.vol_name))
		{
			table->add(tt.disk_label, _drive.vol_name);
		}

		table->add(tt.disk_capacity, _drive.capacity.str());
		table->add(tt_prep(tt.disk_free), _drive.free.str());
		table->add(tt.disk_used, _drive.used.str());
		table->add(tt.disk_system, _drive.file_system);

		result.elements->add(table);

		result.active_bounds = result.window_bounds = bounds.offset(element_offset);
		result.horizontal = true;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			_state.open(event.host, _drive.name);
		}
		else if (event.type == view_element_event_type::dpi_changed)
		{
			icon_layout.dpi_changed();
			title_layout.dpi_changed();
			total_layout.dpi_changed();
		}
	}
};

using search_item_ptr = std::shared_ptr<sidebar_element>;
using drive_item_ptr = std::shared_ptr<sidebar_drive_element>;
// Values only. The sidebar is rebuilt on a worker while the UI thread is still rendering the
// current elements, so the worker must never see a pointer to a live element. It carries the
// last known summary per key so a rebuilt row can show a count before its own sum is computed.
// A rebuild recreates every element, so the counts already earned are carried across it. The known
// flag travels with them: dropping it would flash "loading" over rows whose answer had not changed.
struct sidebar_summary
{
	df::file_group_histogram counts;
	bool known = false;
};

using search_items_by_key_t = df::hash_map<std::string, sidebar_summary, df::ihash, df::ieq>;

inline bool should_publish_sidebar_structural_request(const uint64_t current_request, const uint64_t packet_request)
{
	return current_request == packet_request;
}

class search_item_factory
{
public:
	struct sidebar_config_snapshot
	{
		settings_t::search_t search;
		settings_t::sidebar_t sidebar;
		std::string favorite_tags;
	};

	static std::vector<drive_item_ptr> create_drive_items(view_state& s, const platform::drives& drives)
	{
		std::vector<drive_item_ptr> results;

		for (auto d : drives)
		{
			results.emplace_back(std::make_shared<sidebar_drive_element>(s, d));
		}

		return results;
	}

	std::vector<search_item_ptr> create_search_items(view_state& s, const search_items_by_key_t& existing,
	                                                 const sidebar_config_snapshot& config) const
	{
		std::vector<search_item_ptr> results;

		for (auto i = 0; i < config.search.count; i++)
		{
			auto&& title = config.search.title[i];
			auto&& path = config.search.path[i];

			if (!title.empty() && !path.empty())
			{
				auto search = df::search_t::parse(path);
				auto item = create_or_find_item(s, existing, "s:"s + path);

				item->tooltip_icon = item->icon = search.has_selector() ? icon_index::folder : icon_index::search;
				item->title_layout.text = tt.translate_text(title);
				item->search = search;
				item->calc_sum = [search](const view_state& s, const df::cancel_token& token)
				{
					return s.item_index.count_matches(search, token);
				};
				results.emplace_back(item);
			}
		}

		results.emplace_back(create_item(s, existing, "@duplicates"s, icon_index::compare, tt.duplicates,
		                                 tt.duplicates_tooltip, 0));

		return results;
	}

	void compact_summary(index_state::distinct_results& summary) const
	{
		std::ranges::sort(summary, [](auto&& left, auto&& right) { return str::icmp(left.first, right.first) < 0; });

		auto dst = summary.begin();
		const auto last = summary.end();

		if (dst != last)
		{
			auto i = dst;
			++i;

			while (i != last)
			{
				if (str::icmp(i->first, dst->first) == 0)
				{
					dst->second.add(i->second);
				}
				else
				{
					++dst;
					*dst = std::move(*i);
				}

				++i;
			}

			++dst;

			if (dst != last)
			{
				summary.erase(dst, last);
			}
		}
	}

	std::vector<search_item_ptr> create_tags(view_state& s, const search_items_by_key_t& existing,
	                                         const sidebar_config_snapshot& config) const
	{
		std::vector<search_item_ptr> results;

		index_state::distinct_results tags;

		if (!config.sidebar.show_favorite_tags_only)
		{
			tags = s.item_index.distinct_tags();
		}

		str::split2(config.favorite_tags, true, [&tags](std::string_view part)
		{
			tags.emplace_back(part, df::file_group_histogram{});
		});

		compact_summary(tags);

		for (const auto& t : tags)
		{
			auto tag = std::string(t.first);
			auto search = df::search_t().with(prop::tag, tag);

			if (!str::is_empty(tag) && !search.is_empty())
			{
				auto key = std::format("t:{}", tag);
				auto i = create_or_find_item(s, existing, key);

				i->tooltip_icon = icon_index::tag;
				i->title_layout.text = tag;
				i->search = search;
				i->calc_sum = [tag](const view_state& s, df::cancel_token token)
				{
					return s.item_index.tag_summary(tag);
				};
				i->summary = t.second;
				results.emplace_back(i);
			}
		}

		return results;
	}

	search_item_ptr create_rating_item(view_state& s, const search_items_by_key_t& existing, const std::string& key,
	                                   const int rating) const
	{
		auto i = create_or_find_item(s, existing, key);
		i->tooltip_icon = i->icon = icon_index::star_solid;
		i->icon_repeat = rating;
		i->search = df::search_t::parse(key);
		i->calc_sum = [rating](const view_state& s, df::cancel_token token)
		{
			return s.item_index.rating_summary(rating);
		};
		i->summary = s.item_index.rating_summary(rating);
		return i;
	}

	search_item_ptr create_item(view_state& s, const search_items_by_key_t& existing, const std::string& key,
	                            const icon_index icon, const text_t& name, const text_t& tooltip,
	                            const ui::color32 clr = 0) const
	{
		auto i = create_or_find_item(s, existing, key);
		i->tooltip_icon = i->icon = icon;
		i->clr = clr;
		i->title_layout.text = name;
		i->tooltip_text = tooltip;
		i->search = df::search_t::parse(key);
		i->calc_sum = [search = i->search](const view_state& s, const df::cancel_token& token)
		{
			return s.item_index.count_matches(search, token);
		};
		return i;
	}

	std::vector<search_item_ptr> create_ratings(view_state& s, const search_items_by_key_t& existing) const
	{
		std::vector<search_item_ptr> results;
		results.emplace_back(create_rating_item(s, existing, "rating:5"s, 5));
		results.emplace_back(create_rating_item(s, existing, "rating:4"s, 4));
		results.emplace_back(create_rating_item(s, existing, "rating:3"s, 3));
		results.emplace_back(create_rating_item(s, existing, "rating:2"s, 2));
		results.emplace_back(create_rating_item(s, existing, "rating:1"s, 1));

		results.emplace_back(create_item(s, existing, "rating:-1"s, icon_index::cancel, tt.command_rate_rejected, {},
		                                 color_rate_rejected));
		return results;
	}

	std::vector<search_item_ptr> create_labels(view_state& s, const search_items_by_key_t& existing) const
	{
		std::vector<search_item_ptr> results;

		auto labels = s.item_index.distinct_labels();
		labels.emplace_back("select", df::file_group_histogram{});
		labels.emplace_back("second", df::file_group_histogram{});
		labels.emplace_back("approved", df::file_group_histogram{});
		labels.emplace_back("review", df::file_group_histogram{});
		labels.emplace_back("to do", df::file_group_histogram{});

		compact_summary(labels);

		for (const auto& lab : labels)
		{
			auto label = std::string(lab.first);
			auto key = std::format("l:{}", label);
			auto search = df::search_t().with(prop::label, label);

			if (!str::is_empty(label) && !search.is_empty())
			{
				const auto* const def = find_rate_label_def(label);
				auto i = create_or_find_item(s, existing, key);
				i->tooltip_icon = i->icon = def ? def->icon : icon_index::flag;
				i->clr = def ? def->clr : 0;
				i->title_layout.text = prop::format_label(label);
				i->tooltip_text = {};
				i->search = search;
				i->calc_sum = [label](const view_state& s, df::cancel_token token)
				{
					return s.item_index.label_summary(label);
				};
				i->summary = lab.second;
				results.emplace_back(i);
			}
		}

		return results;
	}


	static search_item_ptr create_or_find_item(view_state& s, const search_items_by_key_t& existing,
	                                           const std::string& key)
	{
		auto result = std::make_shared<sidebar_element>(s);
		result->key = key;

		const auto found = existing.find(key);
		if (found != existing.end())
		{
			result->summary = found->second.counts;
			result->summary_known = found->second.known;
		}

		return result;
	}
};

static int calc_indexing_perc(const index_progress_snapshot& stats)
{
	const auto total = stats.index_item_count;
	const auto processed = stats.index_item_count - stats.index_item_remaining;
	const auto result = df::mul_div(std::clamp(processed, 0, total), 100, total);
	return std::clamp(result, 1, 100);
}

class sidebar_indexing_element final : public view_element,
                                       public std::enable_shared_from_this<sidebar_indexing_element>
{
	ui::style::font_face _font = ui::style::font_face::title;
	view_state& _s;

public:
	sidebar_indexing_element(view_state& s, const view_element_options& style_in) noexcept :
		view_element(
			style_in | view_element_style::can_invoke | view_element_style::has_tooltip |
			view_element_style::important), _s(s)
	{
		update_background_color();
	}

	std::string format_text() const
	{
		return std::format("{} {}%", tt.indexing, calc_indexing_perc(_s.item_index.indexing_progress()));
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		render_background(dc, element_offset);
		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);
		dc.draw_text(format_text(), logical_bounds, _font, ui::style::text_style::multiline_center, clr, {});
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto extent = mc.measure_text(format_text(), _font, ui::style::text_style::multiline_center, width_limit);
		return {width_limit, extent.cy};
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(std::make_shared<text_element>(tt.indexing_message, ui::style::font_face::dialog,
		                                                   ui::style::text_style::multiline,
		                                                   flex_item::line_break));
		hover.elements->add(std::make_shared<action_element>(tt.command_collection_options));

		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
		hover.horizontal = true;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			event.host->invoke(commands::options_collection);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

constexpr int sidebar_visualization_size = 220;

// Vertical breathing room around each of the three lit visuals, so the gaps between them are equal
// by construction rather than by three separate guesses.
constexpr int visual_margin = 7;

class sidebar_file_type_element final : public view_element,
                                        public std::enable_shared_from_this<sidebar_file_type_element>
{
public:
	bool _center_hover = false;
	mutable bool _pie_invalid = true;
	std::string _text;
	view_state& _state;

	struct pie_chart_entry
	{
		int id = 0;
		uint64_t count = 0;
		df::file_size size;
		file_group_ref group = file_group::other;
		bool focus = false;
	};

	static constexpr int chart_segment_count = pie_chart_segment_count;
	std::array<pie_chart_entry, chart_segment_count> _file_type_entries;
	uint32_t _generation = 0;

	sidebar_file_type_element(view_state& state) noexcept : view_element(
		                                                        view_element_style::has_tooltip |
		                                                        view_element_style::can_invoke |
		                                                        flex_item::center), _state(state)
	{
	}

	void populate(const df::file_group_histogram& summary)
	{
		struct group_count
		{
			int64_t weight;
			uint64_t count;
			df::file_size size;
			file_group_ref group;
		};

		std::vector<group_count> counts;
		counts.reserve(file_group::max_count);

		for (auto i = 0; i < file_group::max_count; ++i)
		{
			const auto c = summary.counts[i];
			if (c.count == 0) continue;

			// Cube root: a collection is mostly photographs, and a pie drawn to a linear share
			// would leave every other media type too thin to read or to aim at.
			const auto weight = std::max<int64_t>(1, df::round(std::cbrt(static_cast<double>(c.count))));
			counts.emplace_back(weight, c.count, c.size, file_group_from_index(i));
		}

		// Largest first. Segment zero sits at the back of the tilted disc, where a wedge is seen
		// whole; the front is where the extruded rim eats into it.
		std::ranges::sort(counts, [](auto&& left, auto&& right) { return left.weight > right.weight; });

		for (auto&& e : _file_type_entries)
		{
			e = {};
		}

		const auto group_count_shown = std::min(static_cast<int>(counts.size()), chart_segment_count);

		if (group_count_shown > 0)
		{
			auto total_weight = 0ll;
			for (auto i = 0; i < group_count_shown; ++i) total_weight += counts[i].weight;

			// Largest remainder, so the shares always sum to the whole disc and no group present in
			// the collection is rounded out of the chart it is supposed to appear in.
			std::vector<int> shares(group_count_shown, 1);
			std::vector<double> remainders(group_count_shown, 0.0);
			auto assigned = group_count_shown;

			for (auto i = 0; i < group_count_shown && assigned < chart_segment_count; ++i)
			{
				const auto exact = static_cast<double>(chart_segment_count - group_count_shown) *
					counts[i].weight / total_weight;
				const auto whole = static_cast<int>(exact);
				shares[i] += whole;
				remainders[i] = exact - whole;
				assigned += whole;
			}

			while (assigned < chart_segment_count)
			{
				auto best = 0;
				for (auto i = 1; i < group_count_shown; ++i) if (remainders[i] > remainders[best]) best = i;
				shares[best] += 1;
				remainders[best] = -1.0;
				assigned += 1;
			}

			auto segment = 0;

			for (auto i = 0; i < group_count_shown; ++i)
			{
				for (auto k = 0; k < shares[i] && segment < chart_segment_count; ++k, ++segment)
				{
					auto&& e = _file_type_entries[segment];
					e.id = segment;
					e.group = counts[i].group;
					e.count = counts[i].count;
					e.size = counts[i].size;
				}
			}

			while (segment < chart_segment_count)
			{
				_file_type_entries[segment] = _file_type_entries[segment - 1];
				_file_type_entries[segment].id = segment;
				segment += 1;
			}
		}

		const auto total_items = summary.total_items();
		_text = format_total_text(total_items, true);
		_generation += 1;
		_pie_invalid = true;
	}

	file_group_ref focused_group() const
	{
		for (const auto& e : _file_type_entries)
		{
			if (e.focus) return e.group;
		}

		return nullptr;
	}

	// The whole media type lifts, not the sixty-fourth of the disc the pointer happens to be over.
	// Clicking searches the media type and the bubble describes the media type, so highlighting one
	// sliver of it named a target the user was not being offered.
	bool hover_file_type(const int segment)
	{
		const auto group = segment >= 0 && segment < chart_segment_count
			                   ? _file_type_entries[segment].group
			                   : nullptr;

		auto changed = false;

		for (auto&& e : _file_type_entries)
		{
			const auto focus = group != nullptr && e.group == group && e.count > 0;

			if (e.focus != focus)
			{
				changed = true;
				e.focus = focus;
			}
		}

		return changed;
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		if (_center_hover)
		{
			const auto file_types = _state.item_index.file_types();
			const auto index_roots = _state.item_index.includes_with_totals();
			const auto total = file_types.total_items();
			const auto num = platform::format_number(str::to_string(total.count));
			const auto num_folder = platform::format_number(str::to_string(_state.item_index.stats.index_folder_count));
			const auto size = df::format_size(total.size);
			const auto text = str_format(tt.total_title.sv(), num, size, num_folder);

			hover.elements->add(std::make_shared<text_element>(tt.collection_title, ui::style::font_face::title,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::line_break |
			                                                   flex_item::center));
			hover.elements->add(std::make_shared<text_element>(text, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::line_break |
			                                                   flex_item::center));

			hover.elements->add(std::make_shared<divider_element>());
			hover.elements->add(
				std::make_shared<summary_control>(
					file_types, flex_item::line_break | flex_item::center));
			hover.elements->add(std::make_shared<divider_element>());

			const auto table = std::make_shared<ui::table_element>(flex_item::center);
			table->no_shrink_col[1] = true;
			table->no_shrink_col[2] = true;

			for (const auto& f : index_roots)
			{
				const auto folder_text = std::make_shared<text_element>(f.folder.text());
				folder_text->foreground_color(ui::emphasize(ui::style::color::view_text));
				const auto count_text = std::make_shared<text_element>(str::format_count(f.count),
				                                                       ui::style::text_style::single_line_far);
				const auto size_text = std::make_shared<text_element>(df::format_size(f.size),
				                                                      ui::style::text_style::single_line_far);
				table->add(folder_text, count_text, size_text);
			}

			hover.elements->add(table);
			hover.elements->add(std::make_shared<divider_element>());
			hover.elements->add(std::make_shared<action_element>(tt.command_collection_options));

			hover.preferred_size = view_hover_element::default_preferred_size + 64;
		}
		else
		{
			// One bubble for the media type, not one per segment: a hovered type now owns every
			// segment it was allotted, and the bubble describes the type rather than the sliver.
			for (const auto& e : _file_type_entries)
			{
				if (e.focus)
				{
					const auto ft = e.group;
					const auto icon = ft->icon;
					const auto num = platform::format_number(str::to_string(e.count));
					const auto size = df::format_size(e.size);

					hover.elements->add(make_icon_element(icon, flex_item::no_break));
					hover.elements->add(std::make_shared<text_element>(ft->display_name(e.count > 1),
					                                                   ui::style::font_face::title,
					                                                   ui::style::text_style::multiline,
					                                                   flex_item::line_break));

					const auto text = str_format(tt.collection_contains.sv(), num, ft->display_name(e.count > 1), size);
					hover.elements->add(std::make_shared<text_element>(text, ui::style::font_face::dialog,
					                                                   ui::style::text_style::multiline,
					                                                   flex_item::line_break));
					break;
				}
			}

			if (!hover.elements->is_empty())
			{
				hover.preferred_size = df::mul_div(view_hover_element::default_preferred_size, 2, 3);
			}
		}

		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
		hover.horizontal = true;
	}

	void hover(interaction_context& ic) override
	{
		const auto logical_bounds = bounds.offset(ic.element_offset);
		auto center_hover = false;
		auto segment = -1;

		// What was drawn answers what is under the pointer. A tilted, extruded disc has no angle
		// about its own centre that agrees with its silhouette, so recomputing one would offer the
		// user a wedge next to the one they can see themselves pointing at.
		if (logical_bounds.contains(ic.loc) && _chart.extent() == logical_bounds.extent())
		{
			const auto id = _chart.id_at({ic.loc.x - logical_bounds.left, ic.loc.y - logical_bounds.top});

			if (id == pie_chart_hole_id) center_hover = true;
			else if (id >= pie_chart_wedge_id_base) segment = id - pie_chart_wedge_id_base;
		}

		auto changed = hover_file_type(segment);

		if (_center_hover != center_hover)
		{
			_center_hover = center_hover;
			changed = true;
		}

		if (changed)
		{
			ic.invalidate_view = true;
			_state.invalidate_view(view_invalid::tooltip);
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			// The key cannot see a device change, so the texture is dropped and the pie re-rasterised.
			_tex.reset();
			_rendered = {};
			_pie_invalid = true;
			return;
		}

		if (event.type == view_element_event_type::invoke)
		{
			if (_center_hover)
			{
				_state.invoke(commands::options_collection);
			}
			else
			{
				for (const auto& e : _file_type_entries)
				{
					if (e.focus)
					{
						_state.open(event.host, df::search_t().add_media_type(e.group), {});
						break;
					}
				}
			}
		}
	}

	mutable ui::texture_ptr _tex;
	mutable chart_surface _chart;

	// Every input the pixels depend on. A key that omits one serves the wedge the pointer left, or
	// one theme's well floor under another.
	struct render_key
	{
		sizei extent;
		ui::color32 hole_color = 0;
		uint32_t generation = 0;
		file_group_ref raised = nullptr;
		bool center_hover = false;

		bool operator==(const render_key& other) const
		{
			return extent == other.extent && hole_color == other.hole_color &&
				generation == other.generation && raised == other.raised &&
				center_hover == other.center_hover;
		}
	};

	mutable render_key _rendered;

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto extent = logical_bounds.extent();

		if (extent.cx < 8 || extent.cy < 8) return;

		if (!_tex)
		{
			_tex = dc.create_texture();
			if (!_tex) return;
		}

		// The well floor is opaque so the total reads against a surface of its own, and so the
		// centre has pixels the pointer can claim for collection options.
		auto hole_color = ui::darken(ui::style::color::sidebar_background, 0.35f);

		if (_center_hover)
		{
			const auto tracking = is_style_bit_set(view_element_style::tracking);
			const auto selected = is_style_bit_set(view_element_style::selected);
			hole_color = view_handle_color(selected, true, tracking, dc.frame_has_focus, true).rgba();
		}

		if (_state.item_index.detecting > 0)
		{
			hole_color = ui::style::color::important_background;
		}

		const render_key key{extent, hole_color, _generation, focused_group(), _center_hover};

		if (_pie_invalid || !(_rendered == key))
		{
			if (_chart.prepare(extent))
			{
				pie_chart_scene scene;

				for (auto i = 0; i < chart_segment_count; ++i)
				{
					const auto& e = _file_type_entries[i];
					const auto color = e.count > 0
						                   ? e.group->color
						                   : ui::lighten(ui::style::color::sidebar_background, 0.25f);

					scene.wedges[i] = {
						ui::abgr(e.focus ? ui::lighten(color, 0.14f) : color),
						static_cast<uint16_t>(pie_chart_wedge_id_base + i),
						e.focus
					};
				}

				scene.hole_color = ui::abgr(hole_color);
				scene.hole_id = pie_chart_hole_id;

				render_pie_chart(_chart, scene);
				_tex->update(_chart.pixels());
				_rendered = key;
				_pie_invalid = false;
			}
		}

		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		if (_chart.is_ready())
		{
			dc.draw_texture(_tex, recti(logical_bounds.top_left(), _chart.extent()), dc.colors.alpha);
		}

		dc.draw_text(_text, logical_bounds, ui::style::font_face::dialog, ui::style::text_style::multiline_center, clr,
		             {});
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		// Wider than it is tall, because the disc is tilted. The two spare pixels are the margin the
		// rasteriser keeps around the silhouette.
		const auto size = std::min(width_limit, df::round(sidebar_visualization_size * mc.scale_factor));
		return {size, df::round(size * pie_chart_aspect) + 2};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		if (bounds != bounds_in)
		{
			_pie_invalid = true;
			bounds = bounds_in;
		}
	}
};

// The history chart is one control in two bands: eight years of month blocks, and under them a
// navigator holding the whole plausible span of the collection, one bar per year. Eight years is
// what the eye can take in at once; the navigator is how the user reaches the rest, and how they
// see where in time their photographs actually are before they go looking.
struct sidebar_history_element final : view_element, std::enable_shared_from_this<sidebar_history_element>
{
	static constexpr int col_count = 12;
	static constexpr int row_count = df::history_window_years;
	static constexpr int invalid_hover = -1;
	static constexpr int base_row_height = 14;

	// The block's depth vector and the clear space between months, in unscaled pixels, plus how far a
	// hovered block floats. Depth is an upper bound: measure() shrinks it to what the cell can spare.
	static constexpr int base_depth_x = 4;
	static constexpr int base_gap = 2;
	static constexpr int base_lift = 2;

	// Which way the scene recedes: +1 is away up and to the right, -1 away up and to the left. The
	// row skew and the block's own depth are both driven from here, because a grid receding one way
	// under blocks receding the other is not a projection of anything.
	static constexpr int base_recede_x = 1;

	// Share of the width the rows give up so they can slide across it; they keep the rest, and
	// base_skew_pad holds both ends off the edge of the panel.
	static constexpr int skew_percent = 15;
	static constexpr int base_skew_pad = 2;

	static constexpr int base_navigator_height = 26;
	static constexpr int base_navigator_gap = 7;

	// Twelve months across a wide sidebar gave cells three times wider than they were tall, which
	// stops reading as a calendar. Capped to the width the pie and globe already use, so the three
	// visuals line up as well.
	static constexpr int max_content_width = sidebar_visualization_size;

	// The two identity spaces in the one buffer this element rasterises into.
	static constexpr uint16_t month_id_base = 1;
	static constexpr uint16_t navigator_id_base = month_id_base + row_count * col_count;

	view_state& _state;
	std::array<df::date_counts, col_count * df::max_history_years> _counts{};
	std::array<df::file_path, col_count * df::max_history_years> _representative_paths{};
	std::array<uint64_t, df::max_history_years> _year_totals{};
	mutable sidebar_tooltip_thumbnail _tooltip_thumbnail;

	// What the navigator offers, and the newest year of the eight the calendar is showing.
	df::history_range _range;
	int _window_end = 0;
	bool _window_is_user_set = false;

	uint64_t _year_max = 0;
	int _hover_month = invalid_hover;
	int _hover_year = invalid_hover;
	int _current_year = 0;
	int _current_month = 0;
	uint32_t _generation = 0;

	// The month the address box is showing, or zero. Marked in the calendar so the panel and the
	// listing agree about where in time the user is.
	int _shown_year = 0;
	int _shown_month = 0;

	mutable int row_height = base_row_height;
	mutable int _top_pad = 0;
	mutable int _skew_pad = base_skew_pad;
	mutable int _nav_height = 0;
	mutable int _nav_gap = 0;
	mutable calendar_chart_style _month_style{};

	// The navigator carries one bar per year of the whole range, so its bars are a fraction of a
	// month block's width and cannot spare the same depth. Taken from the range being drawn rather
	// than the one measure() last saw, because new index data changes the range without changing the
	// height that made measure() run.
	calendar_chart_style nav_style(const int element_width) const
	{
		const auto cell = std::max(1, (element_width - 2 * _skew_pad) / std::max(1, _range.year_count()));
		const auto depth = base_recede_x *
			std::clamp(cell / 4, 1, std::max(1, std::abs(_month_style.depth_x) / 2));
		return {depth, std::max(1, df::round(std::abs(depth) * 1.3)), 1, 1};
	}

	// The row's own box inside the element: what the skew leaves it, and how far it slides across
	// the width between the back row and the front one. Derived rather than stored, so a layout that
	// hands the element a different width than measure() was offered still places its cells.
	struct row_plan
	{
		int width = 0;
		int travel = 0;
	};

	row_plan plan_rows(const int element_width) const
	{
		const auto available = std::max(col_count, element_width - 2 * _skew_pad);
		const auto width = std::max(col_count, df::mul_div(available, 100 - skew_percent, 100));
		return {width, available - width};
	}

	mutable ui::texture_ptr _tex;
	mutable chart_surface _chart;
	mutable bool _chart_invalid = true;

	struct render_key
	{
		sizei extent;
		ui::color32 background = 0;
		ui::color32 foreground = 0;
		uint32_t generation = 0;
		int hover_month = invalid_hover;
		int hover_year = invalid_hover;
		int shown_year = 0;
		int shown_month = 0;
		int window_end = 0;
		int row_height = 0;
		int depth = 0;

		bool operator==(const render_key& other) const
		{
			return extent == other.extent && background == other.background &&
				foreground == other.foreground && generation == other.generation &&
				hover_month == other.hover_month && hover_year == other.hover_year &&
				shown_year == other.shown_year && shown_month == other.shown_month &&
				window_end == other.window_end && row_height == other.row_height &&
				depth == other.depth;
		}
	};

	mutable render_key _rendered;

	sidebar_history_element(view_state& state) noexcept : view_element(
		                                                      view_element_style::has_tooltip |
		                                                      view_element_style::can_invoke |
		                                                      flex_item::center), _state(state)
	{
		populate({}); // Set some defaults
	}

	// What the address box is showing, so the calendar can mark it. A search naming a month the
	// window does not cover moves the window: a mark nobody can see is not a mark.
	bool set_current_search(const df::search_t& search)
	{
		const auto parts = search.find_date_parts();
		const auto named = parts.year != 0 && parts.month != 0;
		const auto year = named ? parts.year : 0;
		const auto month = named ? parts.month : 0;
		auto changed = false;

		if (_shown_year != year || _shown_month != month)
		{
			_shown_year = year;
			_shown_month = month;
			changed = true;
		}

		const auto outside = year > _window_end || year <= _window_end - row_count;

		if (named && _range.contains(year) && outside)
		{
			_window_end = std::clamp(year, _range.first_year + df::history_window_years - 1,
			                         _range.last_year);
			_window_is_user_set = true;
			changed = true;
		}

		if (changed) _chart_invalid = true;
		return changed;
	}

	int year_of_row(const int row) const { return _window_end - row; }

	int offset_of_year(const int year) const { return _current_year - year; }

	bool window_fits() const
	{
		return _window_end <= _range.last_year &&
			_window_end - df::history_window_years + 1 >= _range.first_year;
	}

	void populate(const df::date_histogram& summary)
	{
		const auto now = platform::now().date();
		_current_year = now.year;
		_current_month = now.month;
		_counts = summary.dates;
		_representative_paths = summary.representative_paths;

		for (auto y = 0; y < df::max_history_years; ++y)
		{
			uint64_t sum = 0;
			for (auto m = 0; m < col_count; ++m) sum += summary.dates[y * col_count + m].created;
			_year_totals[y] = sum;
		}

		// A start year the user typed is an instruction, not a guess to be second-guessed. Only when
		// they have not said does the range get worked out from what was indexed.
		const auto start_year = setting.sidebar.history_start_year;

		_range = start_year > 0
			         ? df::history_range_from_start_year(start_year, _current_year)
			         : df::history_auto_range(summary, _current_year);

		// A window the user picked is theirs to keep, for as long as the range still holds it.
		if (!_window_is_user_set || !window_fits())
		{
			_window_end = _range.last_year;
			_window_is_user_set = false;
		}

		_year_max = 0;

		for (auto y = 0; y < df::max_history_years; ++y)
		{
			if (_range.contains(_current_year - y)) _year_max = std::max(_year_max, _year_totals[y]);
		}

		_generation += 1;
		_chart_invalid = true;
	}

	bool is_future(const int year, const int month) const
	{
		return year > _current_year || (year == _current_year && month >= _current_month);
	}

	void build_month_cells(std::vector<calendar_chart_cell>& cells, const recti local, const sizei extent,
	                       const ui::color32 foreground) const
	{
		// The window sets its own scale. Absolute comparison across the whole collection is what the
		// navigator underneath is for; inside eight years, what matters is how the months compare.
		uint64_t window_max = 0;

		for (auto row = 0; row < row_count; ++row)
		{
			const auto offset = offset_of_year(year_of_row(row));
			if (offset < 0 || offset >= df::max_history_years) continue;
			for (auto m = 0; m < col_count; ++m)
			{
				window_max = std::max(window_max, static_cast<uint64_t>(_counts[offset * col_count + m].created));
			}
		}

		// A block may clear its own row and lean over the one above, which is what stops a busy
		// month reading like every other busy month. What it may not do is bury the month above:
		// each column remembers where the previous row's block topped out, and the next one is cut
		// short of it. Every month keeps a band of its own to be pointed at.
		std::array<int, col_count> previous_top{};
		std::ranges::fill(previous_top, std::numeric_limits<int>::min());

		const auto ceiling = std::max(1, 2 * row_height - _month_style.depth_y - 2);
		const auto min_exposed = std::max(3, _month_style.depth_y + 2);

		for (auto row = 0; row < row_count; ++row)
		{
			const auto year = year_of_row(row);
			const auto offset = offset_of_year(year);

			for (auto i = 0; i < col_count; ++i)
			{
				// Nearer months are handed over last, and which side is nearer follows the
				// direction the scene recedes in.
				const auto m = _month_style.depth_x >= 0 ? i : col_count - 1 - i;
				const auto cell = month_bounds(local, row, m);
				if (cell.bottom > extent.cy || cell.width() < 3) continue;

				const auto known = offset >= 0 && offset < df::max_history_years;
				const auto index = known ? offset * col_count + m : 0;

				// Months the collection has not reached yet are sockets with nothing in them, and
				// nothing to aim at: the edge of the collection's time is a fact worth showing
				// rather than a row of blocks that happen to be flat.
				const auto future = !known || is_future(year, m);
				const auto count = future ? 0u : static_cast<uint64_t>(_counts[index].created);
				const auto share = chart_log_height(count, window_max);
				const auto hovered = !future && _hover_month == (row << 8) + m;
				const auto shown = !future && year == _shown_year && m + 1 == _shown_month;

				const auto floor_y = cell.bottom - 1;
				auto height = share <= 0.0 ? 0 : std::max(1, df::round(share * ceiling));

				if (previous_top[m] == std::numeric_limits<int>::min())
				{
					// The first row leans into the pad measure() reserved for it, and no further
					// than the top of the element.
					height = std::min(height, floor_y - _month_style.depth_y);
				}
				else
				{
					height = std::min(height, floor_y - _month_style.depth_y - previous_top[m] - min_exposed);
				}

				height = std::clamp(height, 0, ceiling);
				previous_top[m] = floor_y - height - _month_style.depth_y;

				auto color = ui::style::color::important_background;

				if (!hovered)
				{
					// Height carries the count and the tint repeats it, but only as far as a mid
					// tone: the globe beside it is a photograph of the world, and a grid of
					// near-white blocks took the panel over. The month being listed is the
					// exception - it is warm, so the panel and the listing agree at a glance.
					const auto tint = future ? 16 : count > 0 ? 55 + df::round(share * 90.0) : 32;
					color = shown
						        ? ui::lerp(ui::style::color::sidebar_background,
						                   ui::style::color::important_background, 200)
						        : ui::lerp(ui::style::color::sidebar_background, foreground, tint);
				}

				cells.emplace_back(cell, height, ui::abgr(color),
				                   future
					                   ? chart_surface::no_id
					                   : static_cast<uint16_t>(month_id_base + row * col_count + m),
				                   hovered || shown);
			}
		}
	}

	void build_navigator_cells(std::vector<calendar_chart_cell>& cells, const recti local,
	                           const calendar_chart_style& style, const ui::color32 foreground) const
	{
		const auto years = _range.year_count();
		const auto ceiling = std::max(1, _nav_height - style.depth_y - 2);
		const auto oldest_shown = _window_end - df::history_window_years + 1;

		for (auto i = 0; i < years; ++i)
		{
			const auto index = _month_style.depth_x >= 0 ? i : years - 1 - i;
			const auto year = _range.first_year + index;
			const auto offset = offset_of_year(year);
			const auto cell = navigator_bounds(local, index);
			if (cell.width() < 1) continue;

			const auto total = offset >= 0 && offset < df::max_history_years ? _year_totals[offset] : 0;
			const auto selected = year >= oldest_shown && year <= _window_end;
			const auto hovered = _hover_year == year;
			const auto share = chart_log_height(total, _year_max);
			const auto height = total == 0 ? 0 : std::max(1, df::round(share * ceiling));

			// The selected span is tinted rather than boxed: a bracket drawn round eight bars is
			// one more line to read, where a run of warm bars simply is the thing shown above.
			const auto color = hovered
				                   ? ui::style::color::important_background
				                   : selected
				                   ? ui::lerp(ui::style::color::sidebar_background,
				                              ui::style::color::important_background, 165)
				                   : ui::lerp(ui::style::color::sidebar_background, foreground, 50);

			cells.emplace_back(cell, height, ui::abgr(color),
			                   static_cast<uint16_t>(navigator_id_base + index), hovered);
		}
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto extent = logical_bounds.extent();

		if (extent.cx < 8 || extent.cy < 4) return;

		if (!_tex)
		{
			_tex = dc.create_texture();
			if (!_tex) return;
		}

		const render_key key{
			extent, ui::style::color::sidebar_background, dc.colors.foreground,
			_generation, _hover_month, _hover_year, _shown_year, _shown_month, _window_end, row_height,
			_month_style.depth_x
		};

		if (_chart_invalid || !(_rendered == key))
		{
			if (_chart.prepare(extent))
			{
				const recti local(0, 0, extent.cx, extent.cy);
				const auto navigator_style = nav_style(extent.cx);
				std::vector<calendar_chart_cell> months;
				std::vector<calendar_chart_cell> navigator;
				months.reserve(row_count * col_count);
				navigator.reserve(_range.year_count());

				build_month_cells(months, local, extent, dc.colors.foreground);
				build_navigator_cells(navigator, local, navigator_style, dc.colors.foreground);

				// Two passes because the bands cannot share a depth: a navigator bar is a fraction
				// of a month block's width, and the block's depth would swallow it whole.
				render_calendar_chart(_chart, months, _month_style);
				render_calendar_chart(_chart, navigator, navigator_style, false);

				_tex->update(_chart.pixels());
				_rendered = key;
				_chart_invalid = false;
			}
		}

		if (_chart.is_ready())
		{
			dc.draw_texture(_tex, recti(logical_bounds.top_left(), _chart.extent()), dc.colors.alpha);
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto width = std::min(width_limit, df::round(max_content_width * mc.scale_factor));

		row_height = df::round(mc.scale_factor * base_row_height);
		_skew_pad = df::round(mc.scale_factor * base_skew_pad);
		_nav_gap = df::round(mc.scale_factor * base_navigator_gap);

		// Depth is taken out of the block's own width, so a fixed depth would eat a narrow sidebar
		// alive. Scale it to the cell instead and keep the face the wider part.
		const auto plan = plan_rows(width);
		const auto cell_width = std::max(4, plan.width / col_count);
		const auto depth = base_recede_x * std::clamp(cell_width / 4, 1, df::round(mc.scale_factor * base_depth_x));

		_month_style = {
			depth,
			// Tipped up steeper than the depth is wide. Seen nearly side on a block reads as a
			// rectangle with a smear along one edge; it is the visible cap that makes it a solid.
			std::max(2, df::round(std::abs(depth) * 1.3)),
			std::max(1, df::round(mc.scale_factor * base_gap)),
			std::max(1, df::round(mc.scale_factor * base_lift))
		};

		_nav_height = df::round(mc.scale_factor * base_navigator_height);

		// The room a first-row block needs to lean into. Without it the newest year - usually the
		// busiest - would be the one year that could not reach the full height, and the scale it
		// set for every row below it would be a scale it could not itself be drawn at.
		_top_pad = std::max(0, row_height - 1);

		return {width, _top_pad + row_count * row_height + _nav_gap + _nav_height + 1};
	}

	recti month_bounds(const recti logical_bounds, const int row, const int month) const
	{
		const auto plan = plan_rows(logical_bounds.width());

		// Fake isometric: the row itself slides across as the years come forward, so the plan the
		// blocks stand on recedes the same way they do. One diagonal across the whole window, now
		// that the window is eight years rather than however many the collection spans.
		const auto step = df::mul_div(row_count - 1 - row, plan.travel, row_count - 1);
		const auto offset = _month_style.depth_x >= 0 ? step : plan.travel - step;
		const auto left = logical_bounds.left + _skew_pad + offset;

		return {
			left + df::mul_div(month, plan.width, col_count),
			logical_bounds.top + _top_pad + row * row_height,
			left + df::mul_div(month + 1, plan.width, col_count),
			logical_bounds.top + _top_pad + (row + 1) * row_height
		};
	}

	recti navigator_bounds(const recti logical_bounds, const int index) const
	{
		const auto years = std::max(1, _range.year_count());
		const auto width = std::max(years, logical_bounds.width() - 2 * _skew_pad);
		const auto left = logical_bounds.left + _skew_pad;
		const auto top = logical_bounds.top + _top_pad + row_count * row_height + _nav_gap;

		return {
			left + df::mul_div(index, width, years),
			top,
			left + df::mul_div(index + 1, width, years),
			top + _nav_height
		};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	bool hover_month_is_valid() const
	{
		return _hover_month != invalid_hover;
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		if (_hover_year != invalid_hover)
		{
			const auto offset = offset_of_year(_hover_year);
			const auto total = offset >= 0 && offset < df::max_history_years ? _year_totals[offset] : 0;

			hover.elements->add(make_icon_element(icon_index::time, flex_item::no_break));
			hover.elements->add(std::make_shared<text_element>(str::to_string(_hover_year),
			                                                   ui::style::font_face::title,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::line_break));
			hover.elements->add(std::make_shared<text_element>(
				format_plural_text(tt.title_item_count_fmt, static_cast<uint32_t>(total)),
				ui::style::font_face::dialog, ui::style::text_style::multiline, flex_item::line_break));
			hover.elements->add(std::make_shared<action_element>(tt.history_navigator_action));
		}
		else if (hover_month_is_valid())
		{
			const auto m = _hover_month & 0x0F;
			const auto row = _hover_month >> 8;
			const auto date_index = offset_of_year(year_of_row(row)) * col_count + m;
			const auto date_count = _counts[date_index];
			const auto month = str::month(m + 1, true);
			const auto year = str::to_string(year_of_row(row));

			hover.elements->add(make_icon_element(icon_index::time, flex_item::no_break));
			hover.elements->add(std::make_shared<text_element>(month, ui::style::font_face::title,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::no_break));
			hover.elements->add(std::make_shared<text_element>(year, ui::style::font_face::title,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::line_break));
			_tooltip_thumbnail.add(hover, _state, _representative_paths[date_index]);
			hover.elements->add(std::make_shared<text_element>(tt.collection_contains2, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::new_line));

			const auto table = std::make_shared<ui::table_element>(flex_item::center);
			const auto num1 = std::make_shared<text_element>(
				platform::format_number(str::to_string(date_count.created)),
				ui::style::text_style::single_line_far);
			const auto num2 = std::make_shared<text_element>(
				platform::format_number(str::to_string(date_count.modified)),
				ui::style::text_style::single_line_far);
			table->add(num1, std::make_shared<text_element>(str_format(tt.items_created_fmt.sv(), month, year)));
			table->add(num2, std::make_shared<text_element>(str_format(tt.items_modified_fmt.sv(), month, year)));
			hover.elements->add(table);

			hover.elements->add(std::make_shared<action_element>(tt.open_created_modified));
		}

		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
		hover.horizontal = true;
	}

	void hover(interaction_context& ic) override
	{
		const auto logical_bounds = bounds.offset(ic.element_offset);
		auto month = invalid_hover;
		auto year = invalid_hover;

		// A block that has risen no longer fills the cell it was laid out in, so the drawn
		// silhouette is what decides. Months the collection has not reached record no identity and
		// so cannot be aimed at.
		if (logical_bounds.contains(ic.loc) && _chart.extent() == logical_bounds.extent())
		{
			const auto id = _chart.id_at({ic.loc.x - logical_bounds.left, ic.loc.y - logical_bounds.top});

			if (id >= navigator_id_base)
			{
				year = _range.first_year + (id - navigator_id_base);
			}
			else if (id >= month_id_base)
			{
				const auto index = id - month_id_base;
				month = (index / col_count << 8) + index % col_count;
			}
		}

		if (month != _hover_month || year != _hover_year)
		{
			_hover_month = month;
			_hover_year = year;
			ic.invalidate_view = true;
			_state.invalidate_view(view_invalid::tooltip);
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			// The key cannot see a device change, so the texture is dropped and the calendar re-rasterised.
			_tex.reset();
			_rendered = {};
			_chart_invalid = true;
			return;
		}

		if (event.type != view_element_event_type::invoke) return;

		if (_hover_year != invalid_hover)
		{
			// The year clicked becomes the newest of the eight shown, which is the way the calendar
			// is read. Clamped so the window cannot slide off the end of what the navigator offers.
			_window_end = std::clamp(_hover_year, _range.first_year + df::history_window_years - 1,
			                         _range.last_year);
			_window_is_user_set = true;
			_chart_invalid = true;
			_state.invalidate_view(view_invalid::sidebar_file_types_and_dates | view_invalid::tooltip);
		}
		else if (hover_month_is_valid())
		{
			const auto m = _hover_month & 0x0F;
			const auto row = _hover_month >> 8;

			// The calendar buckets on the same capture-first ladder the tile shows, so the click has to
			// ask for that key rather than the Created concept.
			const auto ks = ui::current_key_state();
			const auto type = ks.control ? df::date_parts_prop::modified : df::date_parts_prop::original;
			const auto search = df::search_t().day(0, m + 1, year_of_row(row), type);

			_state.open(event.host, search, {});
		}
	}
};

class sidebar_map_element final : public view_element, public std::enable_shared_from_this<sidebar_map_element>
{
	view_state& _state;

	ui::const_surface_ptr _surface;
	ui::const_surface_ptr _surface_original;
	globe_renderer _renderer;
	mutable ui::texture_ptr _tex;
	mutable ui::surface_ptr _globe_surface;
	mutable ui::texture_ptr _marker_tex;
	mutable int _marker_size = 0;
	mutable ui::color32 _marker_color = 0;

	mutable bool _tex_invalid = true;
	mutable sizei _rendered_extent;
	df::location_heat_map _summary;

	// The coordinate facing the viewer. It starts on the collection and stays wherever the user
	// last dragged it; a drag is theirs to keep, so a later publish never yanks it back.
	gps_coordinate _view;
	gps_coordinate _drag_start_view;
	bool _view_is_user_set = false;
	bool _drag_start_view_is_user_set = false;

	std::vector<map_location_area> _locations;
	df::hash_map<uint32_t, map_location_area> _resolved_areas;

	struct representative_item
	{
		df::file_path path;
		df::item_element_ptr item;
	};

	mutable df::hash_map<uint32_t, representative_item> _representatives;
	mutable df::unique_paths _thumbnail_requests;
	int _hover_location = -1;

	// Issue #119: the display language bit that _resolved_areas / _locations names were resolved
	// under. When the UI language changes the cached place names must be dropped and re-resolved,
	// otherwise the map keeps showing the previous language's names until the collection changes.
	int _resolved_lang_bit = -1;

	// Naming an area reads the gazetteer, so it runs on the location worker. These track the
	// requests in flight and make any answer that arrives after a language change discardable.
	df::hash_set<uint32_t> _resolving_areas;
	uint32_t _resolved_generation = 0;

	// The sphere fills the element, which is square and the same size as the pie chart above it.
	globe_projection projection(const pointi element_offset) const
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto radius = std::min(logical_bounds.width(), logical_bounds.height()) / 2 - 1;
		return {_view, logical_bounds.center(), static_cast<double>(std::max(radius, 1))};
	}

	// Where an area is drawn, or nothing when it is on the far side. The user turns the globe to
	// reach those; nothing off the near hemisphere is drawn or aimed at.
	std::optional<pointi> location_to_view(const map_location_area& location, const pointi element_offset) const
	{
		return projection(element_offset).project(location.position);
	}

	// How close the pointer has to be to claim a marker. design.md targeting: the pointer has to be
	// ON one, so the globally nearest area is never picked out of empty ocean.
	int grab_radius() const
	{
		return std::max(bounds.height() / 8, 4);
	}

	// The hovered area is marked with a translucent disc rather than a filled box: a square drawn
	// on a sphere reads as a sticker stuck to it, and an opaque one hides the place it names.
	// Built as a surface so the hardware and software backends draw the same marker.
	void update_marker(ui::draw_context& dc, const int diameter) const
	{
		const auto clr = ui::bgr(ui::style::color::important_background);

		// Keyed on the colour as well as the size: the accent colour is a theme value, and a key
		// that omits an input serves one theme's marker under another.
		if (_marker_tex && _marker_size == diameter && _marker_color == clr) return;

		if (!_marker_tex)
		{
			_marker_tex = dc.create_texture();
			if (!_marker_tex) return;
		}

		const auto surface = std::make_shared<ui::surface>();

		// A failed alloc still reports the requested extent, so the marker has to be abandoned before
		// the loop below writes through the null it handed back. Leaving the key unset retries.
		if (!surface->alloc(diameter, diameter, ui::texture_format::ARGB)) return;

		const auto centre = (diameter - 1) / 2.0;
		const auto radius = diameter / 2.0;

		for (auto y = 0; y < diameter; ++y)
		{
			auto* const line = std::bit_cast<ui::color32*>(surface->pixels_line(y));

			for (auto x = 0; x < diameter; ++x)
			{
				const auto dx = x - centre;
				const auto dy = y - centre;
				const auto distance = std::sqrt(dx * dx + dy * dy);
				const auto edge = std::clamp(radius - distance + 0.5, 0.0, 1.0);

				// A quiet wash inside and a brighter rim, so the marker rings the place rather
				// than covering it.
				const auto ring = (distance / radius - 0.82) / 0.13;
				const auto alpha = std::clamp((0.20 + 0.68 * std::exp(-0.5 * ring * ring)) * edge, 0.0, 1.0);
				const auto scale = static_cast<uint32_t>(alpha * 256.0);

				const auto rb = ((clr & 0x00FF00FFu) * scale >> 8) & 0x00FF00FFu;
				const auto g = ((clr & 0x0000FF00u) * scale >> 8) & 0x0000FF00u;
				line[x] = (static_cast<uint32_t>(df::round(alpha * 255.0)) << 24) | rb | g;
			}
		}

		_marker_tex->update(surface);
		_marker_size = diameter;
		_marker_color = clr;
	}

	static std::string display_name(const map_location_area& location)
	{
		return location.name;
	}

	static uint32_t area_key(const map_location_area& area)
	{
		return static_cast<uint32_t>(area.cell.x) |
			(static_cast<uint32_t>(area.cell.y) << 8) |
			(static_cast<uint32_t>(std::countr_zero(static_cast<unsigned>(area.cell_span))) << 16);
	}

	static uint32_t area_key(const pointi cell, const int cell_span)
	{
		return static_cast<uint32_t>(cell.x) |
			(static_cast<uint32_t>(cell.y) << 8) |
			(static_cast<uint32_t>(std::countr_zero(static_cast<unsigned>(cell_span))) << 16);
	}

	// Issue #119: drop cached place names when the UI display language changes so the map
	// re-resolves them in the new language instead of showing the previous one.
	void invalidate_stale_language()
	{
		const auto lang_bit = _state.item_index.locations().display_language_bit();
		if (lang_bit == _resolved_lang_bit) return;
		_resolved_lang_bit = lang_bit;
		_resolved_areas.clear();
		_resolving_areas.clear();
		++_resolved_generation;
		for (auto& location : _locations) location.name.clear();
	}

	// The gazetteer lookups this needs read a file, so they never run here. The answer is memoized;
	// a miss queues the work and the published result invalidates the tooltip that asked.
	void resolve_area(map_location_area& area)
	{
		df::assert_true(ui::is_ui_thread());
		invalidate_stale_language();

		if (!area.name.empty()) return;

		const auto key = area_key(area);
		if (const auto found = _resolved_areas.find(key);
			found != _resolved_areas.end() && found->second.has_same_photo_bounds(area))
		{
			const auto count = area.count;
			area = found->second;
			area.count = count;
			return;
		}

		if (!_resolving_areas.emplace(key).second) return;

		_state.queue_location([weak = std::weak_ptr(shared_from_this()), &state = _state, key,
				generation = _resolved_generation, request = area](const location_cache& locations)
			{
				auto selected = locations.find_largest(request.min_latitude, request.min_longitude,
				                                       request.max_latitude, request.max_longitude);
				if (selected.id == 0)
				{
					selected = locations.find_closest(request.position.latitude(),
					                                  request.position.longitude());
				}

				// locations.md 5.1 + 2.5: a place may only name the area from within its attribution
				// radius, so a distant city is never borrowed as the answer for an empty region.
				if (selected.id != 0 && selected.position.is_valid() && request.position.is_valid() &&
					selected.position.distance_in_kilometers(request.position) >
					location_attribution_radius_km(selected.population) * 3.0)
				{
					selected = {};
				}

				auto resolved = request;
				resolved.name = selected.id == 0
					                ? std::string(tt.location_remote.sv())
					                : qualified_name(selected);
				resolved.state = selected.state;
				resolved.country = selected.country;
				resolved.population = selected.population;
				resolved.place_position = selected.id == 0 ? gps_coordinate{} : selected.position;

				state.queue_ui([weak, key, generation, resolved = std::move(resolved)]
				{
					const auto self = weak.lock();
					if (!self) return;

					// A language change cleared the memo, which makes anything still in flight an
					// answer to a question nobody is asking now.
					if (generation != self->_resolved_generation) return;

					self->_resolving_areas.erase(key);
					self->_resolved_areas[key] = resolved;

					for (auto& location : self->_locations)
					{
						if (area_key(location) == key && location.has_same_photo_bounds(resolved))
						{
							const auto count = location.count;
							location = resolved;
							location.count = count;
						}
					}

					self->_state.map_locations(self->_locations);
					self->_state.invalidate_view(view_invalid::tooltip);
				});
			});
	}

	// The map ships as flat-toned land over transparent water, so drawing it as it comes left the
	// oceans the colour of whatever was behind them - black, which sits badly beside the rest of
	// the palette. Composite it once over deep water, using the source's alpha as coverage so
	// coastlines stay smooth.
	static ui::const_surface_ptr composite_water(const ui::const_surface_ptr& map)
	{
		if (!is_valid(map)) return map;

		const auto dims = map->dimensions();
		const auto result = std::make_shared<ui::surface>();

		// A failed alloc still reports the requested extent, so the composite is abandoned rather than
		// written through the null it handed back. The uncomposited map draws, over a black ocean.
		if (!result->alloc(dims, ui::texture_format::ARGB)) return map;

		// Land is one flat tone, so its colour is whatever the first opaque pixel carries - and a
		// fully opaque pixel reads the same whether the decode premultiplied alpha or not.
		auto land = ui::bgr(ui::rgb(128, 128, 128));
		auto found_land = false;

		for (auto y = 0; y < dims.cy && !found_land; ++y)
		{
			const auto* const line = std::bit_cast<const ui::color32*>(map->pixels_line(y));

			for (auto x = 0; x < dims.cx; ++x)
			{
				if (ui::get_a(line[x]) == 255)
				{
					land = line[x] & 0x00FFFFFFu;
					found_land = true;
					break;
				}
			}
		}

		const auto water = ui::bgr(ui::rgb(19, 38, 64));

		for (auto y = 0; y < dims.cy; ++y)
		{
			const auto* const source = std::bit_cast<const ui::color32*>(map->pixels_line(y));
			auto* const destination = std::bit_cast<ui::color32*>(result->pixels_line(y));

			for (auto x = 0; x < dims.cx; ++x)
			{
				destination[x] = (ui::lerp(water, land, static_cast<int>(ui::get_a(source[x]))) & 0x00FFFFFFu) |
					0xFF000000u;
			}
		}

		return result;
	}

public:
	sidebar_map_element(view_state& state, ui::const_surface_ptr s) :
		view_element(view_element_style::has_tooltip | view_element_style::can_invoke | flex_item::center),
		_state(state), _surface_original(composite_water(s))
	{
		_surface = _surface_original;
		_renderer.set_source(_surface);
	}

	int cell_span() const
	{
		// Half the world faces the viewer at any moment, whatever the view is, so the areas the
		// globe folds cells into no longer depend on a crop.
		return map_location_cell_span(df::location_heat_map::map_width / 2, bounds.width());
	}

	// Where the collection deserves to be seen from: the count-weighted mean of its places. An
	// Australian collection opens on Australia and a US one on the US, without either being named.
	void frame_on_collection()
	{
		if (_view_is_user_set) return;

		globe_framer framer;
		for (const auto& location : _locations) framer.add(location.position, location.count);

		const auto framed = framer.view();
		if (!framed.is_valid() || framed == _view) return;

		_view = framed;
		_tex_invalid = true;
	}

	// The drag owns the view from the moment it turns, so an index publish mid-gesture cannot
	// reframe the globe under the pointer. A click that never moved is not a drag and leaves the
	// framing free to follow the collection.
	void begin_drag()
	{
		_drag_start_view = _view;
		_drag_start_view_is_user_set = _view_is_user_set;
	}

	// Escape unwinds the turn, so it has to unwind the pin the turn set as well. A cancelled drag is
	// not a choice of view, and a latched flag would stop the globe following the collection for the
	// rest of the session.
	void cancel_drag(const pointi element_offset)
	{
		drag_to({0, 0}, element_offset);
		_view_is_user_set = _drag_start_view_is_user_set;
	}

	bool drag_to(const pointi drag, const pointi element_offset)
	{
		const auto turned = globe_view_from_drag(_drag_start_view, drag, projection(element_offset).radius());
		if (turned == _view) return false;

		_view = turned;
		_view_is_user_set = true;
		_tex_invalid = true;
		_hover_location = -1;
		return true;
	}

	bool populate(const df::location_heat_map& summary, std::vector<map_location_area> locations)
	{
		invalidate_stale_language();

		for (auto& location : locations)
		{
			if (const auto found = _resolved_areas.find(area_key(location));
				found != _resolved_areas.end() && found->second.has_same_photo_bounds(location))
			{
				const auto count = location.count;
				location = found->second;
				location.count = count;
			}
		}
		_state.map_locations(locations);
		const auto locations_changed = _locations != locations;
		const auto summary_changed = _summary.coordinates != summary.coordinates;
		if (locations_changed || summary_changed)
		{
			if (locations_changed)
			{
				_locations = std::move(locations);
				_representatives.clear();
				_thumbnail_requests.clear();
				if (!_locations.empty())
				{
					location_matrix_params params;
					params.projection = location_matrix_projection::location_heat_map;
					params.area_cell_span = _locations.front().cell_span;
					auto matrix = _state.item_index.build_location_matrix(params);
					for (auto& cell : matrix.cells)
					{
						_representatives.emplace(area_key(cell.index, params.area_cell_span),
						                         representative_item{std::move(cell.representative_path), {}});
					}
				}
				_hover_location = -1;
				frame_on_collection();
			}

			if (summary_changed)
			{
				_summary = summary;
				const auto dims = _surface_original->dimensions();
				df::assert_true(dims.cx * df::location_heat_map::map_height ==
					dims.cy * df::location_heat_map::map_width);

				const auto max_coord = *std::max_element(summary.coordinates.begin(), summary.coordinates.end());

				if (max_coord == 0)
				{
					_surface = _surface_original;
				}
				else
				{
					// 32768 floats is 128 KB, which is an eighth of the default stack and this runs inside a
					// UI-thread populate chain.
					std::vector<float> heat_strength(
						df::location_heat_map::map_width * df::location_heat_map::map_height, 0.0f);
					const auto denominator = std::log1p(std::max(max_coord * 16u, 1u));

					for (auto heat_y = 1u; heat_y < df::location_heat_map::map_height - 1; ++heat_y)
					{
						for (auto heat_x = 1u; heat_x < df::location_heat_map::map_width - 1; ++heat_x)
						{
							const auto ym1 = (heat_y - 1) * df::location_heat_map::map_width;
							const auto y0 = heat_y * df::location_heat_map::map_width;
							const auto yp1 = (heat_y + 1) * df::location_heat_map::map_width;
							const auto value =
								summary.coordinates[ym1 + heat_x - 1] +
								summary.coordinates[ym1 + heat_x] * 2 +
								summary.coordinates[ym1 + heat_x + 1] +
								summary.coordinates[y0 + heat_x - 1] * 2 +
								summary.coordinates[y0 + heat_x] * 4 +
								summary.coordinates[y0 + heat_x + 1] * 2 +
								summary.coordinates[yp1 + heat_x - 1] +
								summary.coordinates[yp1 + heat_x] * 2 +
								summary.coordinates[yp1 + heat_x + 1];
							heat_strength[y0 + heat_x] = value == 0
								                             ? 0.0f
								                             : std::clamp(
									                             static_cast<float>(std::log1p(value) / denominator),
									                             0.0f, 1.0f);
						}
					}

					auto surface = std::make_shared<ui::surface>();
					const auto pixels = surface->alloc(dims, ui::texture_format::ARGB);

					if (!pixels)
					{
						// alloc records the requested extent before it asks for the memory, so a failed
						// surface still reports a full size - only the returned pointer says it is empty.
						_surface = _surface_original;
						_renderer.set_source(_surface);
						_tex_invalid = true;
						return true;
					}

					const auto heat_color = ui::bgr(ui::style::color::important_background) | 0xFF000000;
					memset(pixels, 0, surface->stride() * dims.cy);

					for (auto y = 0; y < dims.cy; ++y)
					{
						const auto map_line = std::bit_cast<const uint32_t*>(_surface_original->pixels_line(y));
						const auto surf_line = std::bit_cast<uint32_t*>(surface->pixels_line(y));
						const auto heat_y = static_cast<size_t>(y) * df::location_heat_map::map_height / dims.cy;
						for (auto x = 0; x < dims.cx; ++x)
						{
							const auto heat_x = static_cast<size_t>(x) * df::location_heat_map::map_width / dims.cx;
							const auto strength = heat_strength[heat_y * df::location_heat_map::map_width + heat_x];
							surf_line[x] = strength == 0.0f
								               ? map_line[x]
								               : ui::lerp(map_line[x], heat_color, strength);
						}
					}

					_surface = std::move(surface);
				}

				_renderer.set_source(_surface);
				_tex_invalid = true;
			}
			return true;
		}

		return false;
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		if (!_renderer.is_ready()) return;

		const auto logical_bounds = bounds.offset(element_offset);
		const auto extent = logical_bounds.extent();
		if (extent.cx < 4 || extent.cy < 4) return;

		if (!_tex)
		{
			const auto t = dc.create_texture();

			if (t)
			{
				_tex = t;
				_tex_invalid = true;
			}
		}

		if (!_tex) return;

		if (_tex_invalid || _rendered_extent != extent)
		{
			// The sphere is resampled only when the view or the size actually moved; a repaint
			// that changed neither redraws the texture it already has. The buffer is kept because a
			// drag re-renders on every frame and would otherwise allocate on every one of them.
			if (!_globe_surface || _globe_surface->dimensions() != extent)
			{
				_globe_surface = std::make_shared<ui::surface>();

				if (!_globe_surface->alloc(extent, ui::texture_format::ARGB))
				{
					// A failed alloc still reports the requested extent, so the buffer has to be dropped
					// rather than left to be recognised as the right size on the next frame.
					_globe_surface.reset();
					return;
				}
			}

			const globe_projection local(_view, pointi(extent.cx / 2, extent.cy / 2),
			                             projection(element_offset).radius());

			if (_renderer.render(*_globe_surface, local))
			{
				_tex->update(_globe_surface);
				_rendered_extent = extent;
				_tex_invalid = false;
			}
		}

		dc.draw_texture(_tex, logical_bounds);

		if (_hover_location >= 0 && _hover_location < static_cast<int>(_locations.size()))
		{
			if (const auto at = location_to_view(_locations[_hover_location], element_offset))
			{
				const auto diameter = grab_radius();
				update_marker(dc, diameter);

				if (_marker_tex)
				{
					dc.draw_texture(_marker_tex, center_rect(sizei(diameter, diameter), at.value()),
					                dc.colors.alpha);
				}
			}
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		// Square, and as wide as the pie above it: the pie is tilted and so shorter than it is wide,
		// but a sphere is not, and the two still have to line up on their width.
		const auto size = std::min(width_limit, df::round(sidebar_visualization_size * mc.scale_factor));
		return {size, size};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

	void hover(interaction_context& ic) override
	{
		if (_renderer.is_ready())
		{
			const auto logical_bounds = bounds.offset(ic.element_offset);
			const auto hovering = logical_bounds.contains(ic.loc);
			int hover_location = -1;

			if (hovering)
			{
				auto closest_distance = std::numeric_limits<int64_t>::max();

				// design.md targeting: the pointer has to be ON a marker. Picking the globally
				// nearest area made empty ocean hover -- and click through to -- whichever
				// cluster happened to be least far away, which is not a target the user chose.
				const auto grab = static_cast<int64_t>(grab_radius());
				const auto max_distance = grab * grab;
				const auto sphere = projection(ic.element_offset);

				for (auto i = 0u; i < _locations.size(); i++)
				{
					const auto at = sphere.project(_locations[i].position);
					if (!at) continue;

					const auto dx = static_cast<int64_t>(at->x - ic.loc.x);
					const auto dy = static_cast<int64_t>(at->y - ic.loc.y);
					const auto distance = dx * dx + dy * dy;

					if (distance <= max_distance && distance < closest_distance)
					{
						closest_distance = distance;
						hover_location = static_cast<int>(i);
					}
				}
			}

			if (_hover_location != hover_location)
			{
				_hover_location = hover_location;
				if (_hover_location != -1) resolve_area(_locations[_hover_location]);
				ic.invalidate_view = true;
				_state.invalidate_view(view_invalid::tooltip);
			}
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		if (_hover_location != -1)
		{
			const auto& location = _locations[_hover_location];
			const auto name = display_name(location);

			// The gazetteer answers on a worker, so the first bubble for an area often has no
			// name yet. Saying how many items are here is true either way; formatting the
			// "close to {}" and "within {} of {}" sentences around a hole is not.
			const auto named = !name.empty();

			hover.elements->add(make_icon_element(icon_index::location, flex_item::no_break));
			hover.elements->add(std::make_shared<text_element>(
				named ? name : format_plural_text(tt.map_items_here_fmt, location.count),
				ui::style::font_face::title,
				ui::style::text_style::multiline,
				flex_item::line_break));

			if (const auto found = _representatives.find(area_key(location)); found != _representatives.end())
			{
				auto& representative = found->second;
				if (!representative.item)
				{
					const auto indexed = _state.item_index.find_item(representative.path);
					if (indexed.ft)
					{
						representative.item = std::make_shared<df::item_element>(representative.path, indexed);
					}
				}

				if (representative.item)
				{
					const auto surface = representative.item->thumbnail_surface();
					if (is_valid(surface))
					{
						hover.elements->add(std::make_shared<surface_element>(
							surface, 160, flex_item::center | flex_item::new_line,
							representative.item->layout_orientation()));
					}
					else if (is_valid(representative.item->thumbnail()))
					{
						representative.item->stage_thumbnail_surface(_state._async, false, true);
					}
					else if (_thumbnail_requests.emplace(representative.path).second)
					{
						_state.item_index.queue_load_thumbnail(representative.item);
					}
				}
			}

			if (named)
			{
				hover.elements->add(std::make_shared<text_element>(
					str_format(tt.click_items_from_fmt.sv(), location.count, name), ui::style::font_face::dialog,
					ui::style::text_style::multiline, flex_item::new_line));

				// locations.md 5.4: the action line states the search it will run, including the
				// radius, so the count on the map and the count after the click are never a surprise.
				hover.elements->add(std::make_shared<action_element>(
					str_format(tt.search_within_fmt.sv(), format_distance_km(location.search_radius_km()),
					           name)));
			}
		}

		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
		hover.horizontal = true;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			// The globe holds a full-size resample buffer as well as two textures, which is more
			// than the flat map ever did, so it answers the broadcast the app uses to shed them.
			_tex.reset();
			_marker_tex.reset();
			_globe_surface.reset();
			_rendered_extent = {};
			_tex_invalid = true;
			return;
		}

		if (event.type == view_element_event_type::invoke && _hover_location != -1)
		{
			const auto& loc = _locations[_hover_location];
			auto search = df::search_t();

			// locations.md 5.1: an area opens a place plus a radius, never an internal `area:`
			// term. "Within 25 km of London" is a concept the user holds; "the cells GeoNames
			// happened to attribute to London" is not.
			const auto km = loc.search_radius_km();

			if (loc.place_position.is_valid() && !loc.name.empty())
			{
				search.location(loc.name, df::location_level::any).set_place_distance(km);
			}
			else if (loc.position.is_valid())
			{
				// Nothing was near enough to name, so the coordinates say exactly what was clicked.
				search.location(loc.position, km);
			}

			// Navigating from the map changes the query only. Grouping and sorting are
			// user-owned presentation and are never reassigned as a side effect (design.md).
			_state.open(event.host, search, {});
		}
	}
};

// Drags turn the globe; anything shorter than a few pixels is still a click on whatever marker is
// under the pointer. The sidebar scrolls by its scrollbar, so no direction has to be handed back.
inline bool should_invoke_globe_release(const bool was_cancelled, const bool was_turning, const bool release_in_bounds)
{
	return !was_cancelled && !was_turning && release_in_bounds;
}

class globe_rotate_controller final : public view_controller
{
	const std::shared_ptr<sidebar_map_element> _element;
	const pointi _element_offset;
	bool _tracking = false;
	bool _turned = false;
	bool _cancelled = false;

public:
	globe_rotate_controller(const view_host_ptr& host, std::shared_ptr<sidebar_map_element> e,
	                        const pointi element_offset, const recti bounds) :
		view_controller(host, bounds), _element(std::move(e)), _element_offset(element_offset)
	{
		_element->set_style_bit(view_element_style::hover, true, _host, _element);
	}

	~globe_rotate_controller() override
	{
		interaction_context ic{{-1, -1}, _element_offset, false};
		_element->hover(ic);
		_element->set_style_bit(view_element_style::hover, false, _host, _element);
		invalidate();
	}

	ui::style::cursor cursor() const override
	{
		return _tracking ? ui::style::cursor::hand_up : ui::style::cursor::hand_down;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		view_controller::on_mouse_left_button_down(loc, keys);
		_tracking = true;
		_turned = false;
		_cancelled = false;
		_element->begin_drag();
		update_hover(loc);
	}

	void on_mouse_move(const pointi loc) override
	{
		_last_loc = loc;

		if (_tracking)
		{
			const auto drag = loc - _start_loc;
			if (std::abs(drag.x) > 2 || std::abs(drag.y) > 2) _turned = true;
			if (_element->drag_to(drag, _element_offset)) invalidate();
			return;
		}

		update_hover(loc);
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;
		const auto was_cancelled = _cancelled;
		const auto was_turning = _turned;
		_tracking = false;
		_cancelled = false;
		update_hover(loc);

		// locations.md 5.5: a drag is never read as a click.
		if (should_invoke_globe_release(was_cancelled, was_turning, _bounds.contains(loc)))
		{
			const view_element_event click{view_element_event_type::click, _host};
			const view_element_event invoke{view_element_event_type::invoke, _host};
			_element->dispatch_event(click);
			_element->dispatch_event(invoke);
		}
	}

	// A turn in progress is unwound to where the drag started, matching every other drag controller.
	// Without this Escape falls through to the view and unwinds the whole view mid-turn.
	bool escape() override
	{
		if (!_tracking) return false;

		_tracking = false;
		_turned = false;
		_cancelled = true;
		_element->cancel_drag(_element_offset);
		invalidate();
		return true;
	}

	void popup_from_location(view_hover_element& hover) override
	{
		_element->tooltip(hover, _last_loc, _element_offset);
	}

private:
	void update_hover(const pointi loc)
	{
		interaction_context ic{loc, _element_offset, _tracking};
		_element->hover(ic);
		if (ic.invalidate_view) invalidate();
	}

	void invalidate() const
	{
		_host->frame()->invalidate(_element->invalidate_bounds(_element_offset));
	}
};

inline view_controller_ptr sidebar_map_element::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                        const pointi element_offset,
                                                                        hit_test_context& ctx)
{
	if (!is_visible() || !bounds.contains(loc - element_offset)) return nullptr;

	return std::make_shared<globe_rotate_controller>(host, shared_from_this(), element_offset,
	                                                 bounds.offset(element_offset));
}

class app_logo_element final : public std::enable_shared_from_this<app_logo_element>, public view_element
{
	view_state& _state;
	std::string_view _text = s_app_name;
	ui::style::font_face _font = ui::style::font_face::title;
	ui::style::text_style _text_style = ui::style::text_style::single_line;
	bool _interactive = true;
	bool _show_plasma = true;
	double _logo_scale = 1.0;

	mutable plasma logo_plasma;
	mutable ui::texture_ptr _plasma_tex;
	mutable ui::texture_ptr _logo_tex;
	mutable int _logo_size = 0;

public:
	app_logo_element(view_state& state, const ui::style::font_face font = ui::style::font_face::title,
	                 const bool interactive = true, const bool show_plasma = true, const double logo_scale = 1.0,
	                 const view_element_options& options = {}) noexcept :
		view_element(options | (interactive
			                        ? view_element_style::has_tooltip | view_element_style::can_invoke
			                        : view_element_style::none)),
		_state(state), _font(font), _interactive(interactive), _show_plasma(show_plasma), _logo_scale(logo_scale)
	{
	}

	void text(const std::string_view t)
	{
		_text = t;
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		render_background(dc, element_offset);

		auto logo_bounds = logical_bounds;
		logo_bounds.right = logo_bounds.left + logo_bounds.height();

		if (_show_plasma)
		{
			if (!_plasma_tex)
			{
				_plasma_tex = dc.create_texture();
			}

			if (_plasma_tex)
			{
				logo_plasma.render(_plasma_tex, logo_bounds.extent());
				dc.draw_texture(_plasma_tex, logo_bounds, logo_bounds.extent());
			}
		}

		// Drawn at the exact size it is shown at, so it stays sharp at every scale factor. Over the
		// plasma the mark is inset so the backdrop reads as a border around it.
		const auto logo_box = std::min(logo_bounds.width(), logo_bounds.height());
		const auto logo_size = _show_plasma ? df::round(logo_box * 0.8) : logo_box;

		if (logo_size > 0 && (!_logo_tex || _logo_size != logo_size))
		{
			const auto t = _logo_tex ? _logo_tex : dc.create_texture();

			if (t)
			{
				const auto logo_surface = std::make_shared<ui::surface>();

				if (logo_surface->alloc(logo_size, logo_size, ui::texture_format::ARGB))
				{
					fill_app_logo(*logo_surface);
					_logo_tex = t;
					_logo_tex->update(logo_surface);
					_logo_size = logo_size;
				}
			}
		}

		if (_logo_tex)
		{
			dc.draw_texture(_logo_tex, center_rect(_logo_tex->dimensions(), logo_bounds),
			                _logo_tex->dimensions(), dc.colors.alpha);
		}

		if (_show_plasma)
		{
			const auto plasma_border_clr = ui::color(0.25f, 0.25f, 0.25f, 1.0f);
			const auto pad = df::round(1 * dc.scale_factor);
			dc.draw_border(logo_bounds, logo_bounds.inflate(pad), plasma_border_clr, plasma_border_clr);
		}

		auto text_bounds = logical_bounds;
		text_bounds.left = logo_bounds.right + dc.padding2;
		dc.draw_text(_text, text_bounds, _font, _text_style, ui::color(dc.colors.foreground, dc.colors.alpha), {});
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto logo_box = df::round((mc.text_line_height(_font) + mc.padding2) * _logo_scale);
		const auto text_width_limit = std::max(0, width_limit - logo_box - mc.padding2);
		const auto text_extent = mc.measure_text(_text, _font, _text_style, text_width_limit);
		return {std::min(width_limit, logo_box + mc.padding2 + text_extent.cx), std::max(logo_box, text_extent.cy)};
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke && _interactive)
		{
			_state.invoke(commands::view_help);
		}
		else if (event.type == view_element_event_type::free_graphics_resources)
		{
			// app_frame calls this directly on the copy it owns. The About dialog's second instance
			// lives in a dialog's control list, which no broadcast walks, so this arm is what a route
			// to that instance would need rather than proof that one currently exists.
			free_graphics_resources();
		}
	}

	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		if (!_interactive) return {};
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	bool step_plasma(const double time_now) const
	{
		return logo_plasma.step(time_now);
	}

	bool plasma_is_active() const
	{
		return logo_plasma.is_active();
	}

	bool hover(const bool value)
	{
		logo_plasma._hover = value;
		if (is_style_bit_set(view_element_style::hover) == value) return false;

		const auto previous = _bg_color;
		set_style_bit(view_element_style::hover, value);
		_bg_color = previous;

		if (value)
		{
			_bg_target = view_handle_color(false, true, false, true, true,
			                               ui::color(ui::style::color::toolbar_background));

			if (ui::is_alpha_zero(_bg_color.a))
			{
				_bg_color.r = _bg_target.r;
				_bg_color.g = _bg_target.g;
				_bg_color.b = _bg_target.b;
			}
		}
		else
		{
			_bg_target = _bg_color;
			_bg_target.a = 0.0f;
		}

		return true;
	}

	bool step_background()
	{
		return ui::step_color_alpha(_bg_color, _bg_target);
	}

	bool step_background_if_animations_disabled()
	{
		return ui::animations_enabled ? false : step_background();
	}

	void free_graphics_resources() const
	{
		_plasma_tex.reset();
		_logo_tex.reset();
		_logo_size = 0;
	}
};


class sidebar_host final : public std::enable_shared_from_this<sidebar_host>, public view_host
{
public:
	view_state& _state;
	view_scroller _scroller;
	ui::frame_ptr _frame;
	ui::control_frame_ptr _owner;
	recti _embedded_bounds;
	bool _has_focus = false;
	bool _show_indexing_control = false;
	bool _is_detecting = false;
	int _last_indexing_perc = -1;


	std::shared_ptr<sidebar_indexing_element> _indexing_elements;
	std::shared_ptr<sidebar_file_type_element> _type_chart;
	std::shared_ptr<sidebar_history_element> _history_chart;
	std::shared_ptr<sidebar_map_element> _map;
	std::vector<search_item_ptr> _items;
	std::vector<drive_item_ptr> _drives;
	// Enumerating volumes calls GetVolumeInformation and GetDiskFreeSpaceEx per drive, either of which
	// blocks for as long as an unreachable network mapping takes to time out. Held so only a real drive
	// event pays that, not every rebuild the index asks for.
	platform::drives _drive_info;
	uint64_t _structural_request_id = 0;
	// The rows the factories built, kept so the chrome above them can change without rebuilding them.
	std::vector<view_element_ptr> _item_elements;
	std::vector<view_element_ptr> _elements;

	sidebar_host(view_state& s) : _state(s)
	{
		df::assert_true(ui::is_ui_thread());

		_indexing_elements = std::make_shared<sidebar_indexing_element>(
			s, flex_item::stretch |
			flex_item::center | view_element_style::important);
		_indexing_elements->padding(8);

		_type_chart = std::make_shared<sidebar_file_type_element>(s);
		_history_chart = std::make_shared<sidebar_history_element>(s);

		files ff;
		const auto surface = ff.image_to_surface(load_app_resource(app_resource::map_png));
		_map = std::make_shared<sidebar_map_element>(s, surface);

		// The three lit visuals are one group, so they carry one margin between them. Left on the
		// list's own spacing they sat as tight against each other as two rows of text do.
		for (const auto& visual : {
			     std::static_pointer_cast<view_element>(_type_chart),
			     std::static_pointer_cast<view_element>(_map),
			     std::static_pointer_cast<view_element>(_history_chart)
		     })
		{
			visual->margin = {0, visual_margin};
		}

		_elements.emplace_back(_type_chart);
		_elements.emplace_back(_map);
		_elements.emplace_back(_history_chart);
	}

	void init(const ui::control_frame_ptr& owner)
	{
		ui::frame_style fs;
		fs.can_focus = true;
		fs.colors = {
			ui::style::color::sidebar_background, ui::style::color::view_text,
			ui::style::color::view_selected_background
		};
		fs.timer_milliseconds = 1000 / 15;
		_frame = owner->create_frame(weak_from_this(), fs);
		_owner = owner;
	}

	void attach_embedded(const ui::frame_ptr& frame, const ui::control_frame_ptr& owner)
	{
		_frame = frame;
		_owner = owner;
	}

	// The sidebar borrows the items view's frame, so it is attached whether or not it is on screen:
	// counting, drive scans and index events run regardless of visibility. This answers the separate
	// question of whether a repaint or a relayout of that shared frame is worth asking for.
	static bool is_shown()
	{
		return setting.show_sidebar;
	}

	void layout_embedded(ui::measure_context& mc, const recti bounds)
	{
		_embedded_bounds = bounds;
		_extent = bounds.extent();
		layout(mc);
	}

	void render_embedded(ui::draw_context& dc)
	{
		dc.draw_rect(_embedded_bounds, ui::color(ui::style::color::sidebar_background, dc.colors.alpha));
		dc.clip_bounds(_embedded_bounds);
		on_window_paint(dc);
		dc.restore_clip();
	}

	bool can_scroll() const
	{
		return _scroller.can_scroll();
	}

	void populate_file_types_and_dates() const
	{
		const auto histograms = _state.item_index.histograms();
		_type_chart->populate(histograms->_file_types);
		_history_chart->populate(histograms->_dates);
		invalidate();
	}

	// Which chrome sits above the rows, given the same rows. Kept apart from update_content so showing
	// or hiding one element does not rebuild every other element, its text layout and its count.
	void compose_elements()
	{
		df::assert_true(ui::is_ui_thread());

		_elements.clear();
		if (_show_indexing_control) _elements.emplace_back(_indexing_elements);
		if (setting.sidebar.show_total_items) _elements.emplace_back(_type_chart);
		if (setting.sidebar.show_world_map) _elements.emplace_back(_map);
		if (setting.sidebar.show_history) _elements.emplace_back(_history_chart);

		_elements.insert(_elements.end(), _item_elements.begin(), _item_elements.end());
	}

	void update_content(std::vector<search_item_ptr> items, std::vector<drive_item_ptr> drives,
	                    std::vector<view_element_ptr> item_elements)
	{
		df::assert_true(ui::is_ui_thread());

		_items = std::move(items);
		_drives = std::move(drives);
		_item_elements = std::move(item_elements);

		compose_elements();

		update_current_search();
		queue_update_predictions();
		layout();
	}

	// The scan blocks on unreachable volumes, so it stays off the UI thread and the rebuild waits for
	// the answer rather than the sidebar waiting for the scan on every unrelated invalidation.
	void populate_drives()
	{
		df::assert_true(ui::is_ui_thread());

		_state.queue_async(async_queue::sidebar,
		                   [t = ui_owned(_state._async, shared_from_this()), &s = _state]
		                   {
			                   auto drives = platform::scan_drives();

			                   s.queue_ui([t, drives = std::move(drives)]() mutable
			                   {
				                   t->_drive_info = std::move(drives);
				                   t->populate();
			                   });
		                   });
	}

	// The structural tier: every element is recreated, which discards the text layout each one owns,
	// and update_content then asks for a fresh sum per row. Raise view_invalid::sidebar only for a
	// change to which rows exist - the cheaper tiers above cover everything else.
	void populate()
	{
		df::assert_true(ui::is_ui_thread());

		const auto histograms = _state.item_index.histograms();
		_type_chart->populate(histograms->_file_types);
		_history_chart->populate(histograms->_dates);
		if (_map->populate(histograms->_locations, histograms->map_locations(_map->cell_span()))) invalidate();

		search_items_by_key_t existing;
		const auto request_id = ++_structural_request_id;
		const search_item_factory::sidebar_config_snapshot config{
			setting.search, setting.sidebar, setting.favorite_tags
		};

		for (const auto& i : _items)
		{
			existing[i->key] = {i->summary, i->summary_known};
		}

		_state.queue_async(async_queue::sidebar,
		                   [t = ui_owned(_state._async, shared_from_this()), &s = _state, existing,
			                   drive_info = _drive_info, config, request_id]
		                   {
			                   constexpr search_item_factory f;
			                   std::vector<search_item_ptr> items;
			                   std::vector<drive_item_ptr> drives;
			                   std::vector<view_element_ptr> item_elements;
			                   std::unordered_set<search_item_ptr> added_elements;

			                   auto add_elements = [&items, &item_elements, &added_elements](
				                   const std::vector<search_item_ptr>& items_to_add)
			                   {
				                   if (!items_to_add.empty() && !item_elements.empty())
					                   item_elements.emplace_back(
						                   std::make_shared<divider_element>());

				                   for (const auto& i : items_to_add)
				                   {
					                   if (!added_elements.contains(i))
					                   {
						                   added_elements.insert(i);
						                   items.push_back(i);
						                   item_elements.push_back(i);
					                   }
				                   }
			                   };

			                   if (config.sidebar.show_favorite_searches)
			                   {
				                   add_elements(f.create_search_items(s, existing, config));
			                   }

			                   if (config.sidebar.show_drives)
			                   {
				                   drives = f.create_drive_items(s, drive_info);
				                   if (!drives.empty() && !item_elements.empty())
					                   item_elements.emplace_back(
						                   std::make_shared<divider_element>());
				                   item_elements.insert(item_elements.end(), drives.begin(), drives.end());
			                   }

			                   if (config.sidebar.show_ratings)
			                   {
				                   add_elements(f.create_ratings(s, existing));
			                   }

			                   if (config.sidebar.show_labels)
			                   {
				                   add_elements(f.create_labels(s, existing));
			                   }

			                   if (config.sidebar.show_tags)
			                   {
				                   add_elements(f.create_tags(s, existing, config));
			                   }


			                   item_elements.emplace_back(std::make_shared<divider_element>());

			                   const auto tag_show_text = config.sidebar.show_favorite_tags_only
				                                              ? tt.command_all_tags
				                                              : tt.command_favorite_tags;

			                   auto tags_element = std::make_shared<link_element>(
				                   tag_show_text, commands::view_favorite_tags,
				                   ui::style::font_face::dialog,
				                   ui::style::text_style::multiline_center,
				                   flex_item::center);

			                   auto customise_element = std::make_shared<link_element>(
				                   tt.command_customise, commands::options_sidebar,
				                   ui::style::font_face::dialog,
				                   ui::style::text_style::multiline_center,
				                   flex_item::center);
			                   auto favorite_tags_element = std::make_shared<link_element>(tt.customise_tags_title,
				                   commands::favorite_tags,
				                   ui::style::font_face::dialog,
				                   ui::style::text_style::multiline_center,
				                   flex_item::center);

			                   item_elements.emplace_back(tags_element);
			                   item_elements.emplace_back(favorite_tags_element);
			                   item_elements.emplace_back(customise_element);

			                   s.queue_ui(
				                   [t, items = std::move(items), drives = std::move(drives), item_elements = std::move(
					                   item_elements), request_id]
				                   {
					                   if (!should_publish_sidebar_structural_request(t->_structural_request_id, request_id))
					                   {
						                   return;
					                   }
					                   t->update_content(std::move(items), std::move(drives), std::move(item_elements));
				                   });
		                   });
	}

	void on_window_layout(ui::measure_context& mc, const sizei extent, const bool is_minimized) override
	{
		if (!is_minimized)
		{
			_extent = extent;
			layout(mc);
		}
	}

	void on_window_paint(ui::draw_context& dc) override
	{
		df::assert_true(ui::is_ui_thread());

		const auto offset = _scroller.scroll_offset();
		const auto logical_clip_bounds = dc.clip_bounds().offset(offset);

		for (const auto& e : _elements)
		{
			if (e->bounds.intersects(logical_clip_bounds))
			{
				e->render(dc, -offset);
			}
		}

		if (_active_controller)
		{
			_active_controller->draw(dc);
		}

		_scroller.draw_scroll(dc);

		if (setting.show_debug_info && _active_controller)
		{
			const auto c = ui::color(1.0f, 0.0f, 0.0f, 1.0f);
			const auto pad = df::round(2 * dc.scale_factor);
			dc.draw_border(_controller_bounds, _controller_bounds.inflate(pad), c, c);
		}
	}

	void update_current_search() const
	{
		const auto current_search = _state.search();
		const auto current_search_text = current_search.text();

		for (const auto& i : _items)
		{
			const auto checked = i->search == current_search;

			if (i->is_style_bit_set(view_element_style::checked) != checked)
			{
				i->set_style_bit(view_element_style::checked, checked);
				invalidate();
			}
		}

		for (const auto& i : _drives)
		{
			const auto checked = i->_drive.name == current_search_text;

			if (i->is_style_bit_set(view_element_style::checked) != checked)
			{
				i->set_style_bit(view_element_style::checked, checked);
				invalidate();
			}
		}

		if (_history_chart->set_current_search(current_search)) invalidate();
	}

	void queue_update_predictions()
	{
		// Every sum is an index query, and before the index reports init complete every one of them
		// answers zero for a collection that is not empty. Those answers are discarded at draw time, so
		// the pass is spent contending for the index locks with the thread still building the index.
		// Rows keep their loading affordance until the index can answer; each index phase that
		// completes asks for the counts again.
		if (!_state.item_index.is_init_complete()) return;

		static std::atomic_int version;
		df::cancel_token token(version);

		// calc_sum only queries item_index, which is synchronized, so it is safe off the UI thread. The
		// elements themselves are not: they own text layouts, so the list is guarded rather than copied
		// loose into the worker, and the view is reached through a weak reference locked on the UI thread.
		auto elements = ui_owned(_state._async, std::make_shared<std::vector<search_item_ptr>>(_items));

		_state.queue_async(async_queue::sidebar, [weak = weak_from_this(), &s = _state, elements, token]
		{
			df::scope_locked_inc slc(df::jobs_running);

			int update_count = 0;

			for (const auto& i : *elements)
			{
				if (i->calc_sum)
				{
					const auto sum = i->calc_sum(s, token);

					if (!token.is_cancelled())
					{
						update_count += 1;

						//if (i->summary != sum)
						{
							s.queue_ui([weak, i, sum, token]
							{
								if (token.is_cancelled()) return;
								const auto t = weak.lock();
								if (!t) return;
								i->summary_known = true;
								i->summary = sum;
								t->invalidate();
							});
						}
					}
				}
			}

			df::trace(std::format("Sidebar update {} predictions", update_count));
		});
	}

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
	{
		df::assert_true(ui::is_ui_thread());

		if (_scroller.can_scroll() && ctx.occluded(_scroller.scroll_bounds()))
		{
			return std::make_shared<scroll_controller>(shared_from_this(), _scroller, _scroller.scroll_bounds());
		}

		const auto offset = -_scroller.scroll_offset();

		for (const auto& e : _elements)
		{
			auto controller = e->controller_from_location(shared_from_this(), loc, offset, ctx);
			if (controller) return controller;
		}

		return nullptr;
	}

	void on_mouse_wheel(const pointi loc, const int delta, const ui::key_state keys, bool& was_handled) override
	{
		_scroller.offset(shared_from_this(), 0, -(delta / 2));
		was_handled = _scroller.can_scroll();
		update_controller(loc);
	}

	void layout() const
	{
		if (is_shown())
		{
			frame()->layout();
			frame()->invalidate();
		}
	}

	void invalidate() const
	{
		if (is_shown())
		{
			frame()->invalidate();
		}
	}

	void invalidate_element(const view_element_ptr& e) override
	{
		invalidate();
	}

	void layout(ui::measure_context& mc)
	{
		df::assert_true(ui::is_ui_thread());

		constexpr auto x_padding = 4;
		const auto embedded = !_embedded_bounds.is_empty();
		const auto scroll_padding = embedded || _scroller.can_scroll() ? mc.scroll_width : x_padding;
		const auto layout_padding = sizei{0, df::round(mc.padding1 / mc.scale_factor)};
		auto avail_bounds = embedded ? _embedded_bounds : recti(_extent);
		avail_bounds.left += x_padding; // -(mc.baseline_snap / 2);
		avail_bounds.right -= scroll_padding; // -(mc.baseline_snap / 2);

		ui::control_layouts positions;
		flex_container_layout column;
		column.direction = flex_direction::column;
		column.wrap = flex_wrap::no_wrap;
		column.align_items = flex_align::start;
		column.padding = layout_padding;
		const auto content_extent = layout_flex_elements(_elements, mc, positions, avail_bounds, column);

		const auto host_bounds = embedded ? _embedded_bounds : recti(_extent);
		const recti scroll_bounds{
			host_bounds.right - scroll_padding,
			embedded ? host_bounds.top + host_bounds.height() / 2 : host_bounds.top,
			host_bounds.right,
			host_bounds.bottom
		};
		const recti client_bounds{
			host_bounds.left, host_bounds.top, host_bounds.right - scroll_padding,
			host_bounds.bottom
		};
		_scroller.layout({client_bounds.width(), content_extent.cy + mc.padding2}, client_bounds, scroll_bounds);
	}

	static int preferred_width(ui::measure_context& mc)
	{
		const auto extent = mc.measure_text("Documents", ui::style::font_face::dialog,
		                                    ui::style::text_style::single_line, 200);
		return df::mul_div(extent.cx, 5, 2);
	}

	void focus_changed(const bool has_focus, const ui::control_base_ptr& child) override
	{
		df::trace(std::format("Sidebar navigation_controls::focus {}", has_focus));

		_has_focus = has_focus;
		invalidate();
	}


	bool _is_active = true;

	void tick() override
	{
		if (is_shown() && frame()->is_visible())
		{
			const auto is_indexing = _state.item_index.indexing > 0;
			const auto is_detecting = _state.item_index.detecting > 0;

			if (_show_indexing_control != is_indexing)
			{
				// One element joins or leaves the head of the list; the rows below it are unchanged.
				_show_indexing_control = is_indexing;
				compose_elements();
				layout();
			}

			if (_is_detecting != is_detecting)
			{
				_is_detecting = is_detecting;
				frame()->invalidate(_indexing_elements->bounds);
			}

			if (_show_indexing_control)
			{
				const auto perc = calc_indexing_perc(_state.item_index.indexing_progress());

				if (_last_indexing_perc != perc)
				{
					_last_indexing_perc = perc;
					invalidate();
				}
			}
			else
			{
				_last_indexing_perc = -1;
			}
		}
	}

	void activate(bool is_active) override
	{
	}

	bool key_down(const int c, const ui::key_state keys) override
	{
		return false;
	};

	const ui::frame_ptr frame() const override
	{
		return _frame ? _frame : ui::no_frame();
	}

	const ui::control_frame_ptr owner() override
	{
		return _owner;
	}

	void controller_changed() override
	{
		_state.invalidate_view(view_invalid::tooltip);
	}

	void invoke(const commands cmd) override
	{
		_state.invoke(cmd);
	}

	bool is_command_checked(const commands cmd) override
	{
		return _state.is_command_checked(cmd);
	}

	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override
	{
		_state.track_menu(frame(), bounds, commands);
	}

	void invalidate_view(const view_invalid invalid) override
	{
		_state.invalidate_view(invalid);
	}

	void dpi_changed() override
	{
		df::assert_true(ui::is_ui_thread());

		broadcast_event({view_element_event_type::dpi_changed, shared_from_this()});

		if (is_shown())
		{
			frame()->layout();
			frame()->invalidate();
		}
	}

	// The globe, the pie and the calendar each cache a texture, so the sidebar has to answer the
	// app-wide broadcasts the views answer - a hidden sidebar still holds them. The three are owned for
	// the life of the sidebar and compose_elements only decides which are drawn, so the broadcast walks
	// the members: routing it through _elements would leave a chart the user switched off holding a
	// texture belonging to a device that no longer exists, with no way for it ever to be told.
	void broadcast_event(const view_element_event& event) const
	{
		df::assert_true(ui::is_ui_thread());

		if (_indexing_elements) _indexing_elements->dispatch_event(event);
		if (_type_chart) _type_chart->dispatch_event(event);
		if (_map) _map->dispatch_event(event);
		if (_history_chart) _history_chart->dispatch_event(event);

		for (const auto& e : _item_elements)
		{
			e->dispatch_event(event);
		}
	}
};
