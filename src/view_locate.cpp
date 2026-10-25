// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Location assignment view. Provides a map-based interface for setting
// GPS coordinates on selected items, with search and reverse geocoding.
// The map is rendered directly in the main view area (left panel).

#include "pch.h"
#include "model.h"
#include "model_db.h"
#include "model_index.h"
#include "model_locations.h"
#include "files.h"
#include "ui_dialog.h"
#include "ui_elements.h"
#include "view_locate.h"
#include "view_list.h"
#include "app_command_status.h"


// -------------------------------------------------------------------
// Location autocomplete match for search results
// -------------------------------------------------------------------

class locate_auto_complete_match final : public ui::auto_complete_match,
                                         public std::enable_shared_from_this<locate_auto_complete_match>
{
public:
	location_match match;
	ui::complete_strategy_t& _parent;

	locate_auto_complete_match(ui::complete_strategy_t& parent, location_match m, const int w) :
		auto_complete_match(view_element_style::can_invoke), match(std::move(m)), _parent(parent)
	{
		weight = w;
	}

	std::string edit_text() const override
	{
		return match.location.str();
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);

		if (bg.a > 0.0f)
		{
			dc.draw_rect(logical_bounds, bg);
		}

		auto rr = logical_bounds.inflate(-dc.padding1, 0);

		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		const auto city_text = match.city.text.sv();
		const auto state_text = match.state.text.sv();
		const auto country_text = match.country.text.sv();

		if (!city_text.empty())
		{
			const auto city_extent = dc.measure_text(city_text, ui::style::font_face::dialog,
			                                         ui::style::text_style::single_line, rr.width());
			dc.draw_text(city_text, rr, ui::style::font_face::dialog,
			             ui::style::text_style::single_line, clr, {});
			rr.left += city_extent.cx + dc.padding2;
		}

		std::string sub;
		if (!state_text.empty()) sub += state_text;
		if (!country_text.empty())
		{
			if (!sub.empty()) sub += ", ";
			sub += country_text;
		}

		if (!sub.empty())
		{
			dc.draw_text(sub, rr, ui::style::font_face::dialog,
			             ui::style::text_style::single_line,
			             ui::color(dc.colors.foreground, dc.colors.alpha * 0.6f), {});
		}
	}

	sizei measure(ui::measure_context& mc, const int cx) const override
	{
		const auto line_height = mc.text_line_height(ui::style::font_face::dialog);
		return {cx, line_height + mc.padding1};
	}

	// Enable mouse hover/click on the result row: without this override the base
	// element returns no controller, so the dropdown only responds to the keyboard.
	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::click)
		{
			_parent.selected(shared_from_this(), ui::complete_strategy_t::select_type::click);
		}
		else if (event.type == view_element_event_type::double_click)
		{
			_parent.selected(shared_from_this(), ui::complete_strategy_t::select_type::double_click);
		}
	}
};


// -------------------------------------------------------------------
// Location autocomplete strategy using local location database
// -------------------------------------------------------------------

class locate_auto_complete_strategy final : public std::enable_shared_from_this<locate_auto_complete_strategy>,
                                            public ui::complete_strategy_t
{
public:
	view_state& _state;
	ui::control_frame_ptr _parent;
	ui::auto_complete_results _results;
	ui::auto_complete_match_ptr _selected;

	// Each keystroke supersedes the last. The gazetteer answers on its own queue, so an older query
	// can return after a newer one and put its matches back in the list - where the first is
	// selected, retargeting the operation to a place the user had already typed past.
	uint64_t _search_generation = 0;

	std::function<void(const location_t&)> _changed;

	locate_auto_complete_strategy(view_state& s, ui::control_frame_ptr parent,
	                              std::function<void(const location_t&)> changed)
		: _state(s), _parent(std::move(parent)), _changed(std::move(changed))
	{
		resize_to_show_results = false;
		max_predictions = 15u;
	}

	// A coordinate chosen on the map, or a new item to locate, supersedes any search still on the
	// location queue the same way a keystroke does. Its answer would otherwise select its first match
	// and move the coordinate back to a place the user had moved away from.
	void supersede_pending_search()
	{
		++_search_generation;
	}

	std::string no_results_message() override
	{
		return std::string(tt.type_to_search);
	}

	void initialise(std::function<void(const ui::auto_complete_results&)> complete) override
	{
		search({}, std::move(complete));
	}

	void search(const std::string& query, std::function<void(const ui::auto_complete_results&)> complete) override
	{
		const auto generation = ++_search_generation;

		if (str::is_empty(query))
		{
			_state.queue_ui([t = shared_from_this(), generation, complete = std::move(complete)]
			{
				if (t->_search_generation != generation) return;
				t->_results.clear();
				complete({});
			});
			return;
		}

		_state.queue_location(
			[t = shared_from_this(), query, generation, complete = std::move(complete)](
			const location_cache& locations)
			{
				const auto locally_found = locations.auto_complete(query, t->max_predictions, setting.default_location);

				ui::auto_complete_results found;

				// locations.md 3.4: the gazetteer already ranks matches (exact spelling, then
				// population, then distance). Re-sorting on distance alone put a nearby hamlet
				// above the city the user was obviously typing.
				for (const auto& local : locally_found)
				{
					found.emplace_back(
						std::make_shared<locate_auto_complete_match>(*t, local,
						                                             static_cast<int>(found.size())));
				}

				t->_state.queue_ui([t, generation, complete, found]
				{
					if (t->_search_generation != generation) return;

					t->_results = found;
					if (t->_results.size() > t->max_predictions) t->_results.resize(t->max_predictions);
					complete(t->_results);
				});
			});
	}

	void selected(const ui::auto_complete_match_ptr& i, const select_type st) override
	{
		if (i)
		{
			const auto ii = std::dynamic_pointer_cast<locate_auto_complete_match>(i);

			if (ii && _changed)
			{
				_changed(ii->match.location);
			}

			if (_selected)
			{
				_selected->set_style_bit(view_element_style::selected, false);
			}

			_selected = i;

			if (_selected)
			{
				_selected->set_style_bit(view_element_style::selected, true);
			}
		}
	}

	ui::auto_complete_match_ptr selected() const override
	{
		return _selected;
	}
};


// -------------------------------------------------------------------
// locate_view implementation
// -------------------------------------------------------------------

class locate_controls_host final : public view_controls_host
{
public:
	locate_controls_host(view_state& state) : view_controls_host(state)
	{
	}

	void layout_controls(ui::measure_context& mc) override
	{
		if (_controls.empty()) return;

		const auto layout_padding = df::round(mc.padding1 / mc.scale_factor);
		const auto avail_bounds = recti(_extent).inflate(-layout_padding);
		mc.col_widths = {};
		ui::control_layouts positions;
		flex_container_layout column;
		column.direction = flex_direction::column;
		column.wrap = flex_wrap::no_wrap;
		column.align_items = flex_align::start;
		column.padding = {layout_padding, layout_padding};
		_layout_height = layout_flex_elements(_controls, mc, positions, avail_bounds, column).cy;
		_layout_width = avail_bounds.width();
		_label_width = mc.col_widths;
		_scroller.layout({_layout_width, _extent.cy}, recti(_extent), {});
		_dlg->apply_layout(positions, {});
		_dlg->invalidate();
	}
};

void locate_view::clear_resolved_place()
{
	_location->id = 0;
	_place_label.clear();
	_bearing.clear();
}

void locate_view::supersede_place_search() const
{
	if (const auto search = _place_search.lock()) search->supersede_pending_search();
}

std::shared_ptr<ui::complete_strategy_t> locate_view::place_search_for_tests() const
{
	return _place_search.lock();
}

void locate_view::update_location(const location_t& loc)
{
	if (!loc.position.is_valid())
	{
		return;
	}

	_location->id = loc.id;
	_location->latitude = loc.position.latitude();
	_location->longitude = loc.position.longitude();

	// The user named this place themselves, so it is `at` by construction rather than by radius.
	_place_label = qualified_name(loc);
	_bearing.clear();

	// Invalidate any in-flight reverse-geocode requests so they don't overwrite
	// the place/state/country we just set from the user's explicit selection.
	++_geocode_request_id;

	set_map_location(loc.position);
	refresh();
}

void locate_view::activate(const sizei extent)
{
	map_view::activate(extent);
	rebuild_markers();
	select_default_location();
}

void locate_view::rebuild_markers()
{
	df::assert_true(ui::is_ui_thread());

	std::vector<map_engine::marker> markers;
	std::vector<marker_item> marker_items;
	_thumbnail_requests.clear();
	auto excluded = std::make_shared<df::unique_paths>();

	for (const auto& item : _state.selected_items().items())
	{
		excluded->insert(item->path());
	}

	_state.search_items().for_all([&](const df::item_element_ptr& item)
	{
		if (!excluded->contains(item->path()) && item->has_gps())
		{
			const auto metadata = item->metadata();

			if (metadata && metadata->coordinate.is_valid() && excluded->emplace(item->path()).second)
			{
				marker_items.push_back({item->path(), item});
				markers.push_back({metadata->coordinate, 1});
			}
		}
	});

	const auto generation = ++_marker_generation;
	const auto zoom = _engine.zoom_level();
	const std::weak_ptr<locate_view> weak_self = shared_from_this();
	_pending_marker_generation = generation;
	_pending_marker_items = std::move(marker_items);

	_state.queue_async(async_queue::query,
	                   [&state = _state, weak_self, generation, zoom, excluded,
		                   markers = std::move(markers)]() mutable
	                   {
		                   location_matrix_params params;
		                   params.zoom = zoom;
		                   auto matrix = state.item_index.build_location_matrix(params, *excluded);

		                   std::vector<df::file_path> paths;
		                   paths.reserve(matrix.cells.size());

		                   for (auto& cell : matrix.cells)
		                   {
			                   paths.emplace_back(std::move(cell.representative_path));
			                   markers.push_back({cell.centroid, cell.count});
		                   }

		                   auto marker_snapshot = map_engine::prepare_marker_snapshot(markers, zoom);

		                   state._async.queue_ui([weak_self, generation,
				                   marker_snapshot = std::move(marker_snapshot), paths = std::move(paths)]() mutable
			                   {
				                   const auto self = weak_self.lock();

				                   if (!self || self->_marker_generation != generation ||
					                   self->_pending_marker_generation != generation)
				                   {
					                   return;
				                   }

				                   for (auto& path : paths)
				                   {
					                   self->_pending_marker_items.push_back({std::move(path), {}});
				                   }

				                   self->_marker_items = std::move(self->_pending_marker_items);
				                   self->_pending_marker_generation = 0;
				                   self->_engine.set_marker_snapshot(std::move(marker_snapshot));
				                   self->_state.invalidate_view(view_invalid::view_redraw);
			                   });
	                   });
}

void locate_view::deactivate()
{
	_populate_controls = nullptr;
	_status.clear();
	// A search still answering belongs to the session being left.
	supersede_place_search();
	// Reset selected location so the next activation re-evaluates from the
	// newly selected items rather than reusing stale data.
	*_location = selected_location_t{};
	clear_resolved_place();
	// Invalidate any in-flight reverse-geocode results from this session.
	++_geocode_request_id;
	// Release the marker set and its item references.
	_engine.set_markers({});
	_marker_items.clear();
	_pending_marker_items.clear();
	_pending_marker_generation = 0;
	// Any collection scan still running belongs to a map that is no longer on screen.
	++_marker_generation;
	_thumbnail_requests.clear();
	map_view::deactivate();
}

void locate_view::display_changed()
{
	select_default_location();
}

void locate_view::on_map_zoomed(const int zoom)
{
	rebuild_markers();
}

void locate_view::select_default_location()
{
	const auto target = _state.focus_item() ? _state.focus_item() : _state.first_selected().item;
	gps_coordinate initial_loc;

	if (target && target->has_gps())
	{
		if (const auto md = target->metadata(); md && md->coordinate.is_valid()) initial_loc = md->coordinate;
	}

	if (!initial_loc.is_valid() && target)
	{
		const auto target_date = target->calc_media_created();
		auto closest_delta = INT64_MAX;

		if (target_date.is_valid())
		{
			_state.search_items().for_all([&](const df::item_element_ptr& item)
			{
				if (item != target && item->has_gps())
				{
					const auto item_date = item->calc_media_created();
					const auto md = item->metadata();
					if (item_date.is_valid() && md && md->coordinate.is_valid())
					{
						const auto delta = std::abs(item_date - target_date);
						if (delta < closest_delta)
						{
							closest_delta = delta;
							initial_loc = md->coordinate;
						}
					}
				}
			});
		}
	}

	if (!initial_loc.is_valid()) initial_loc = setting.default_location;
	if (!initial_loc.is_valid()) initial_loc = gps_coordinate(48.8566, 2.3522);

	supersede_place_search();
	clear_resolved_place();
	_location->latitude = initial_loc.latitude();
	_location->longitude = initial_loc.longitude();
	set_map_location(initial_loc);
	request_reverse_geocode(initial_loc);
	refresh();
}

void locate_view::on_map_panned(const gps_coordinate& new_center)
{
	if (!new_center.is_valid())
	{
		return;
	}

	// Skip work if the centre has not actually moved (zero-distance click).
	if (df::equiv(_location->latitude, new_center.latitude()) &&
		df::equiv(_location->longitude, new_center.longitude()))
	{
		return;
	}

	// Update coordinates immediately so the UI reflects the new position
	supersede_place_search();
	clear_resolved_place();
	_location->latitude = new_center.latitude();
	_location->longitude = new_center.longitude();
	refresh();

	request_reverse_geocode(new_center);
}

void locate_view::on_marker_hover(view_hover_element& hover, const int marker_index, const int count,
                                  const pointi anchor)
{
	if (marker_index < 0 || marker_index >= static_cast<int>(_marker_items.size()))
	{
		return;
	}

	auto& marker = _marker_items[marker_index];
	if (!marker.item)
	{
		const auto indexed = _state.item_index.find_item(marker.path);
		if (indexed.ft)
		{
			marker.item = std::make_shared<df::item_element>(marker.path, indexed);
		}
	}

	const auto& item = marker.item;
	if (!item) return;
	const auto elements = std::make_shared<view_elements>();

	// Thumbnail of the representative photo (mirrors the items-view scrollbar preview).
	// It may be absent if the item's thumbnail has not been decoded into memory, so a
	// caption is always added below to guarantee the bubble has content.
	const auto surface = item->thumbnail_surface();

	if (is_valid(surface))
	{
		elements->add(std::make_shared<surface_element>(surface, 160, flex_item::center,
		                                                item->layout_orientation()));
	}
	else if (is_valid(item->thumbnail()))
	{
		item->stage_thumbnail_surface(_state._async, false, true);
	}
	else if (_thumbnail_requests.emplace(item->path()).second)
	{
		_state.item_index.queue_load_thumbnail(item);
	}

	// Caption: item count for a cluster, otherwise the representative file name.
	const auto caption = count > 1
		                     ? format_plural_text(tt.map_items_here_fmt, count)
		                     : std::string(item->name().sv());

	elements->add(std::make_shared<text_element>(caption,
	                                             flex_item::center | flex_item::new_line));

	// Anchor the bubble on the marker; the tooltip system offsets it into screen space.
	hover.elements = elements;
	hover.window_bounds = recti(anchor.x - 8, anchor.y - 8, anchor.x + 8, anchor.y + 8);
	hover.active_bounds = recti(anchor.x - 12, anchor.y - 12, anchor.x + 12, anchor.y + 12);
	hover.preferred_size = 180;
	hover.horizontal = false;
}

// Here the centre is the answer, so a picked bubble is moved under the crosshair rather than
// marked in place. Contrast the advanced-search map, which picks without moving.
void locate_view::on_marker_clicked(const gps_coordinate& coordinate, const int count)
{
	if (!coordinate.is_valid()) return;

	supersede_place_search();
	clear_resolved_place();
	_location->latitude = coordinate.latitude();
	_location->longitude = coordinate.longitude();
	set_map_location(coordinate);
	request_reverse_geocode(coordinate);
	refresh();

	// The bubble was anchored to the old view; leaving it up would point at nothing.
	_host->invalidate_view(view_invalid::tooltip | view_invalid::view_redraw);
}

void locate_view::request_reverse_geocode(const gps_coordinate& gps)
{
	const auto request_id = ++_geocode_request_id;
	std::weak_ptr<locate_view> weak_self = shared_from_this();
	_state._async.queue_location(
		[&async = _state._async, gps, weak_self, request_id](const location_cache& locations)
		{
			// locations.md 2.5: bounded attribution. find_closest would name a city hundreds of
			// kilometres away with the same confidence as a correct answer.
			const auto attributed = locations.find_attributed(gps);

			// Composed here because the record and its qualification level are only meaningful
			// beside the gazetteer; the UI thread receives a finished answer.
			auto label = qualified_name(attributed.place);

			if (attributed.attribution == location_attribution::near)
			{
				label = str_format(tt.location_near_fmt.sv(), label);
			}
			else if (attributed.attribution == location_attribution::remote && label.empty())
			{
				label = std::string(tt.location_remote.sv());
			}

			async.queue_ui([attributed, label, bearing = bearing_descriptor(attributed), gps, weak_self, request_id]
			{
				if (const auto self = weak_self.lock())
				{
					// Discard stale results from earlier requests.
					if (request_id != self->_geocode_request_id)
					{
						return;
					}

					self->_place_label = label;
					self->_bearing = bearing;

					// Only an `at` answer names a place the item is actually in, so only that answer
					// is worth remembering as a recent location.
					self->_location->id = attributed.attribution == location_attribution::at ? attributed.place.id : 0;
					self->_location->latitude = gps.latitude();
					self->_location->longitude = gps.longitude();
					self->refresh();
				}
			});
		});
}

void locate_view::refresh()
{
	const auto has_location = gps_coordinate(_location->latitude, _location->longitude).is_valid();

	if (has_location)
	{
		// The coordinate is always true; the place name is appended only when the gazetteer
		// had one to give, so an empty answer reads as a coordinate rather than as blanks.
		_status = std::format("{} {}",
		                      str::to_string(_location->latitude, 5),
		                      str::to_string(_location->longitude, 5));

		if (!_place_label.empty()) _status += std::format(" | {}", _place_label);
		if (!_bearing.empty()) _status += std::format(" | {}", _bearing);
	}
	else
	{
		_status = std::string(tt.location_not_selected);
	}

	_state.invalidate_view(view_invalid::view_layout | view_invalid::status | view_invalid::command_state);
	if (_populate_controls) _populate_controls();
}

bool locate_view::run(detach_file_handles& detach)
{
	if (!can_run())
		return false;

	const auto title = tt.command_locate;
	constexpr auto icon = icon_index::location;

	record_feature_use(features::locate);
	auto dlg = make_dlg(_host->owner());

	metadata_edits edits;
	edits.location_coordinate = gps_coordinate(_location->latitude, _location->longitude);

	// locations.md 2.9: only the coordinate is written. Place, state and country are derived from
	// it at read time, so writing them would freeze one gazetteer's answer into the user's file.

	// Only record locations that map to a known place id; "0" is meaningless
	// as a recent-location entry.
	if (_location->id != 0)
	{
		_state.recent_locations.add(str::to_string(_location->id));
	}

	const auto results = std::make_shared<command_status>(_state._async, dlg, icon, title,
	                                                      _state.selected_count());
	_state.modify_items(results, icon, title, _state.selected_items().items(), edits, _host);
	results->wait_for_complete();

	return !results->is_canceled() && !results->has_failures();
}

bool locate_view::run()
{
	detach_file_handles detach(_state);
	return run(detach);
}

void locate_view::run_and_next(const bool forward)
{
	// Advancing past a failed or canceled write would report success by moving on.
	detach_file_handles detach(_state);
	if (!run(detach)) return;

	if (const auto item = _state.next_item(forward, false))
	{
		detach.keep_display_closed();
		_state.select(_host, item, false, false, false);
	}
}


view_controls_host_ptr locate_view::controls(const ui::control_frame_ptr& owner)
{
	auto result = std::make_shared<locate_controls_host>(_state);
	auto frame = owner->create_dlg(result, false);

	auto sel_changed = [this](const location_t& loc)
	{
		update_location(loc);
	};

	auto strategy = std::make_shared<locate_auto_complete_strategy>(
		_state, frame, sel_changed);
	_place_search = strategy;
	const auto search = std::make_shared<ui::search_control>(frame, _location->search_text, strategy);
	search->flex.align_self = flex_align::stretch;

	const auto selection_thumbnails = std::make_shared<ui::selection_thumbnails_control>(frame);
	const auto target_summary = std::make_shared<text_element>(std::string{});
	const auto overwrite_summary = std::make_shared<text_element>(std::string{}, view_element_style::important);
	overwrite_summary->margin = {10, 10};
	overwrite_summary->padding = {10, 10};
	overwrite_summary->update_background_color();

	std::vector<view_element_ptr> controls;
	controls.emplace_back(create_view_info_element(tt.map_instructions));
	controls.emplace_back(selection_thumbnails);
	controls.emplace_back(target_summary);
	controls.emplace_back(overwrite_summary);
	controls.emplace_back(std::make_shared<ui::check_control>(
		frame, tt.show_without_location, setting.locate_only_without_location, false, false,
		[this](const bool)
		{
			_state.invalidate_view(view_invalid::selector_filter | view_invalid::options_save);
		}));

	std::weak_ptr<locate_controls_host> weak_host = result;
	_populate_controls = [this, selection_thumbnails, target_summary, overwrite_summary, weak_host]
	{
		const auto& items = _state.selected_items();
		df::item_elements gps_items;
		for (const auto& item : items.items())
		{
			if (item->has_gps()) gps_items.emplace_back(item);
		}

		selection_thumbnails->selection_async(items.thumbs(), items.size(), _state._async);
		target_summary->text(format_plural_text(tt.be_updated_fmt, items));
		overwrite_summary->text(gps_items.empty()
			                        ? std::string{}
			                        : format_plural_text(tt.gps_overwrite_count_fmt, df::item_set(gps_items)));
		overwrite_summary->is_visible(!gps_items.empty());
		if (const auto host = weak_host.lock()) host->scroll_controls();
	};


	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<text_element>(tt.type_to_search));
	controls.emplace_back(search);


	for (const auto& c : controls)
	{
		c->margin.cx = 8;
		c->margin.cy = 4;
	}

	result->_controls = controls;
	result->_frame = result->_dlg = frame;
	_populate_controls();

	return result;
}
