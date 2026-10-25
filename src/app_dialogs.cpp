// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The dialogs commands open that are features in their own right - advanced search, the
// update offer, the keyboard reference, About, settings, collection settings and maintenance, sidebar
// customisation and sharing. app_commands.cpp registers the commands that open them.

#include "pch.h"

#include "model.h"
#include "model_index.h"
#include "model_db.h"
#include "model_tokenizer.h"
#include "util_zip.h"
#include "ui_controls.h"
#include "ui_dialog.h"
#include "ui_controllers.h"
#include "ui_map.h"
#include "app_text.h"
#include "app_command_status.h"
#include "app_util.h"
#include "app.h"
#include "app_dialogs.h"

static constexpr auto docs_url = "https://www.diffractor.com/docs";
static constexpr auto releases_url = "https://www.diffractor.com/releases";
static constexpr auto support_url = "https://github.com/ZacWalk/diffractor/issues";
static constexpr auto donate_url = "https://www.paypal.com/donate/?hosted_button_id=HX5NRS9JGKLRL";

bool should_refresh_advanced_search_hover_thumbnail(const bool has_surface, const bool has_thumbnail,
                                                    const bool is_staging, int& hover_retries)
{
	if (has_surface || !has_thumbnail) return false;
	if (is_staging) return true;
	if (hover_retries <= 0) return false;
	--hover_retries;
	return true;
}

bool ui::browse_for_term(view_state& vs, const control_frame_ptr& parent, std::string& result)
{
	const auto dlg = make_dlg(parent);
	auto dlg_parent = dlg->_frame;

	pause_media pause(vs);

	std::string scope;
	std::string text;

	auto default_texts = vs.item_index.auto_complete_text(prop::tag);
	auto edit_control = std::make_shared<ui::edit_control>(dlg_parent, tt.value, text, default_texts);

	auto create_commands = [&vs, edit_control](const std::shared_ptr<select_control>& sel)
	{
		std::vector<command_ptr> commands;

		for (const auto& s : prop::search_scopes())
		{
			auto c = std::make_shared<command>();
			c->text = s.scope;
			c->invoke = [&vs, sel, s, edit_control]
			{
				sel->update_text(s.scope);
				edit_control->auto_completes(vs.item_index.auto_complete_text(s.type));
			};

			commands.emplace_back(c);
		}

		return commands;
	};

	const std::vector<view_element_ptr> controls = {
		set_margin(std::make_shared<title_control>(icon_index::search, tt.search_select_term)),
		std::make_shared<divider_element>(),
		set_margin(std::make_shared<select_control>(dlg_parent, tt.scope, scope, create_commands)),
		set_margin(edit_control),
		std::make_shared<divider_element>(),
		std::make_shared<ok_cancel_control>(dlg->_frame)
	};

	if (dlg->show_modal(controls) == close_result::ok)
	{
		if (str::is_empty(str::trim(scope)) || str::icmp(str::trim(scope), "any") == 0)
		{
			result = text;
		}
		else
		{
			result = std::format("{}:{}", scope, str::quote_if_white_space(text));
		}

		return true;
	}

	return false;
}


// UI-thread state behind the advanced search location column. Marker sets are built off
// the UI thread and published here, so the generation counter drops results that arrive
// after the map has moved on and the map is only reached through a weak pointer that is
// locked back on the UI thread.
struct advanced_search_location_state
{
	enum class marker_place_state : uint8_t
	{
		unknown,
		pending,
		resolved
	};

	std::weak_ptr<map_control> map;
	std::vector<df::file_path> marker_paths;
	std::vector<marker_place_state> marker_place_states;
	std::vector<uint32_t> marker_place_view_generations;
	std::vector<std::string> marker_place_names;
	uint32_t generation = 0;

	// The map is framed once, on the first thing worth looking at. Re-framing later would
	// undo the pan and zoom the user performed to reach the hot spot they were aiming for.
	bool framed = false;

	// Names arrive from the gazetteer after the pick, so a later pick must win.
	uint32_t pick_generation = 0;
	std::function<void(std::string name, gps_coordinate centre, double radius_km)> apply_place;

	df::file_path hover_path;
	df::item_element_ptr hover_item;
	df::unique_paths thumbnail_requests;

	// Bounded number of bubble rebuilds spent waiting for a hover thumbnail, so a marker
	// whose thumbnail never decodes does not rebuild its bubble forever.
	int hover_retries = 0;
};

void advanced_search_invoke(view_state& state, const ui::control_frame_ptr& parent,
                                   const view_host_base_ptr& view)
{
	auto dlg = make_dlg(parent);
	auto dlg_parent = dlg->_frame;

	const auto search = state.search();

	static std::string selected_folder;
	static std::string all_terms;
	static std::string none_terms;

	static bool search_collection = true;
	static bool search_folder = false;
	static bool search_sub_folders = true;
	static bool search_location = false;

	static bool search_photos = false;
	static bool search_videos = false;
	static bool search_audio = false;

	static bool search_date_from = false;
	static bool search_date_until = false;
	static bool search_date_original = false;
	static bool search_date_created = false;
	static bool search_date_modified = false;
	static df::date_t from_val;
	static df::date_t until_val;

	static location_and_distance_t location{{}, location_default_search_km};
	static std::string location_name;
	static gps_coordinate selected_location;

	if (!search.selectors().empty())
	{
		// The dialog opens on the scope the user is already looking at. Filling the folder in
		// while leaving "the collection" selected would show a path that plays no part in the
		// search.
		selected_folder = search.selectors().front().folder().text();
		search_folder = true;
		search_collection = false;
	}

	// An empty date picker cannot be read or compared, so a range starts on today and the
	// user narrows from there.
	if (!from_val.is_valid()) from_val = platform::now();
	if (!until_val.is_valid()) until_val = platform::now();

	auto search_collection_radio = std::make_shared<ui::check_control>(dlg->_frame, tt.search_collection,
	                                                                   search_collection, true);

	auto search_folder_ratio = std::make_shared<ui::check_control>(dlg->_frame, tt.search_folder, search_folder, true);
	auto webp_group = std::make_shared<ui::group_control>();
	webp_group->add(std::make_shared<ui::folder_picker_control>(dlg_parent, selected_folder));
	webp_group->add(std::make_shared<ui::check_control>(dlg_parent, tt.search_sub_folders, search_sub_folders));
	search_folder_ratio->child(webp_group);

	auto file_type_control = std::make_shared<ui::col_control>(std::vector<view_element_ptr>{
		std::make_shared<ui::check_control>(dlg_parent, tt.search_photos, search_photos),
		std::make_shared<ui::check_control>(dlg_parent, tt.search_videos, search_videos),
		std::make_shared<ui::check_control>(dlg_parent, tt.search_audio, search_audio)
	});

	auto date_type_control = std::make_shared<ui::col_control>(std::vector<view_element_ptr>{
		std::make_shared<ui::check_control>(dlg_parent, tt_prep(tt.prop_name_original.sv()), search_date_original),
		std::make_shared<ui::check_control>(dlg_parent, tt.prop_name_created, search_date_created),
		std::make_shared<ui::check_control>(dlg_parent, tt.prop_name_modified, search_date_modified)
	});

	auto from_check = std::make_shared<ui::check_control>(dlg->_frame, tt.search_date_from, search_date_from, false,
	                                                      true);
	from_check->child(std::make_shared<ui::date_control>(dlg_parent, from_val, false));
	auto until_check = std::make_shared<ui::check_control>(dlg->_frame, tt.search_date_until, search_date_until, false,
	                                                       true);
	until_check->child(std::make_shared<ui::date_control>(dlg_parent, until_val, false));

	file_type_control->compact = true;
	date_type_control->compact = true;

	//
	// Location. The map fills the right of the dialog and is always live, showing the
	// collection's photo hot spots. The user pans and zooms until a hot spot they want is
	// visible and clicks it; the line under "Located within" says what that pick means.
	//

	const auto ls = std::make_shared<advanced_search_location_state>();

	const auto location_check = std::make_shared<ui::check_control>(dlg_parent, tt.search_located_within,
	                                                                search_location);

	const auto describe_location = []()
	{
		if (!location.position.is_valid()) return std::string{};

		const auto where = location_name.empty()
			                   ? std::format("{:.5f}, {:.5f}", location.position.latitude(),
			                                 location.position.longitude())
			                   : location_name;

		return str_format(tt.search_within_fmt.sv(), format_distance_km(location.km), where);
	};

	const auto location_summary = std::make_shared<text_element>(describe_location());

	auto map = std::make_shared<map_control>(state._async, std::function<void(gps_coordinate)>{});
	map->init(dlg->_frame);
	map->set_show_crosshair(false);
	ls->map = map;

	// Seed the map so it is never blank, and mark the remembered pick if there is one.
	map->set_location_marker(location.position.is_valid() ? location.position : setting.default_location);

	if (selected_location.is_valid() || location.position.is_valid())
	{
		map->set_selected(selected_location.is_valid() ? selected_location : location.position, 0);
	}

	if (location.position.is_valid())
	{
		// A remembered pick is what the dialog is about, so the map opens showing that area
		// and the distance it covers rather than the whole collection.
		const auto lat_span = location.km / 111.0;
		const auto lon_span = location.km / (111.0 * std::max(
			0.05, std::cos(gps_coordinate::deg2rad(location.position.latitude()))));

		map_box box;
		box.add(gps_coordinate(std::max(-85.0, location.position.latitude() - lat_span),
		                       std::max(-180.0, location.position.longitude() - lon_span)));
		box.add(gps_coordinate(std::min(85.0, location.position.latitude() + lat_span),
		                       std::min(180.0, location.position.longitude() + lon_span)));
		map->frame_on(box);
		ls->framed = true;
	}

	// Applies whatever the pick resolved to, and is the only writer of the location statics.
	ls->apply_place = [location_check, location_summary, describe_location, dlg, ls,
			search_collection_radio, search_folder_ratio](
		const std::string& name, const gps_coordinate centre, const double radius_km)
		{
			df::assert_true(ui::is_ui_thread());

			location_name = name;
			location.position = centre;
			location.km = location_distance_at_detent(location_distance_detent_at_least(radius_km));
			location_summary->text(describe_location());
			location_check->checked(true);

			// The hot spots are the whole collection's, so picking one under a folder scope would
			// promise items the search then refuses to look for.
			search_folder_ratio->checked(false);
			search_collection_radio->checked(true);

			dlg->_frame->invalidate();
			dlg->layout();
		};

	// Clicking a hot spot is the whole gesture: it names the area and turns the option on.
	// The coordinate answers straight away; the gazetteer then upgrades it to a place name so
	// the search reads as `loc:Tokyo, 100km` rather than a pair of numbers.
	map->marker_picked = [&state, ls](const gps_coordinate coord, const double radius_km, int)
	{
		df::assert_true(ui::is_ui_thread());

		selected_location = coord;
		ls->apply_place({}, coord, radius_km);

		const auto generation = ++ls->pick_generation;
		const std::weak_ptr<advanced_search_location_state> weak_ls = ls;

		state.queue_location([&state, weak_ls, generation, coord, radius_km](const location_cache& locations)
		{
			const auto found = locations.find_largest_attributed(coord);
			auto name = found.position.is_valid() ? qualified_name(found) : std::string{};
			auto centre = found.position;

			// The place centre is not the bubble, so the radius has to cover the offset too. A
			// name that would widen the search by more than one detent is not worth the trade.
			auto km = radius_km;

			if (!name.empty())
			{
				km = centre.distance_in_kilometers(coord) + radius_km;

				constexpr auto max_km = location_distance_at_detent(location_distance_detent_count - 1);

				if (location_distance_detent_at_least(km) > location_distance_detent_at_least(radius_km) + 1)
				{
					name.clear();
					centre = coord;
					km = radius_km;
				}
				else
				{
					km = std::min(km, max_km);
				}
			}
			else
			{
				centre = coord;
			}

			state.queue_ui([weak_ls, generation, name = std::move(name), centre, km]
			{
				const auto ls = weak_ls.lock();
				if (!ls || ls->pick_generation != generation || !ls->apply_place) return;
				ls->apply_place(name, centre, km);
			});
		});
	};

	// Photo hot spots. build_location_matrix reads the index, so it runs on the query
	// queue and is published back to the map on the UI thread.
	auto rebuild_markers = [&state, ls](const int zoom)
	{
		const auto generation = ++ls->generation;
		const std::weak_ptr<advanced_search_location_state> weak_ls = ls;

		state.queue_async(async_queue::query, [&state, weak_ls, generation, zoom]
		{
			location_matrix_params params;
			params.zoom = zoom;
			auto matrix = state.item_index.build_location_matrix(params);

			std::vector<map_engine::marker> markers;
			std::vector<df::file_path> paths;
			markers.reserve(matrix.cells.size());
			paths.reserve(matrix.cells.size());

			// Where the collection actually holds photos, so the map can open on something
			// clickable instead of the default coordinate at street level.
			map_box box;

			for (auto& cell : matrix.cells)
			{
				markers.push_back({cell.centroid, cell.count});
				paths.emplace_back(cell.representative_path);
				box.add(gps_coordinate(cell.min_latitude, cell.min_longitude));
				box.add(gps_coordinate(cell.max_latitude, cell.max_longitude));
			}

			auto marker_snapshot = map_engine::prepare_marker_snapshot(markers, zoom);

			state._async.queue_ui(
				[weak_ls, generation, box,
					marker_snapshot = std::move(marker_snapshot), paths = std::move(paths)]() mutable
				{
					const auto s = weak_ls.lock();
					if (!s || s->generation != generation) return;

					const auto m = s->map.lock();
					if (!m) return;

					s->marker_paths = std::move(paths);
					s->marker_place_states.assign(s->marker_paths.size(),
					                              advanced_search_location_state::marker_place_state::unknown);
					s->marker_place_view_generations.assign(s->marker_paths.size(), 0);
					s->marker_place_names.assign(s->marker_paths.size(), {});
					s->hover_path = {};
					s->hover_item.reset();
					m->set_marker_snapshot(std::move(marker_snapshot));

					if (!s->framed && box.valid)
					{
						// Framing changes the zoom, which asks for a rebuild at that zoom; the
						// generation check drops this now-stale set when that answer arrives.
						s->framed = true;
						m->frame_on(box);
					}
				});
		});
	};

	map->zoom_changed = [rebuild_markers](const int zoom) { rebuild_markers(zoom); };

	map->marker_hover = [&state, ls](view_hover_element& hover, const int marker_index, const int count,
	                                 const pointi anchor, bool& needs_refresh)
	{
		if (marker_index < 0 || marker_index >= static_cast<int>(ls->marker_paths.size())) return;

		const auto map = ls->map.lock();
		if (!map) return;

		const auto view_generation = map->view_generation();

		auto& place_state = ls->marker_place_states[marker_index];
		if (ls->marker_place_view_generations[marker_index] != view_generation)
		{
			ls->marker_place_view_generations[marker_index] = view_generation;
			ls->marker_place_names[marker_index].clear();
			place_state = advanced_search_location_state::marker_place_state::unknown;
		}

		if (place_state == advanced_search_location_state::marker_place_state::unknown)
		{
			place_state = advanced_search_location_state::marker_place_state::pending;
			needs_refresh = true;

			const auto generation = ls->generation;
			const auto marker_coordinate = map->gps_at_screen(anchor);
			const auto visible_coordinates =
				std::make_shared<const std::vector<gps_coordinate>>(map->visible_cluster_coordinates());
			const std::weak_ptr<advanced_search_location_state> weak_ls = ls;

			state.queue_location([&async = state._async, weak_ls, generation, view_generation, marker_index,
					marker_coordinate, visible_coordinates](
				const location_cache& locations)
				{
					std::string unique_name;
					const auto found = locations.find_largest_attributed(marker_coordinate);

					if (found.id != 0)
					{
						const auto radius_km = location_attribution_radius_km(found.population);
						auto unique = true;

						for (const auto coordinate : *visible_coordinates)
						{
							if (!unique) break;
							if (coordinate == marker_coordinate ||
								coordinate.distance_in_kilometers(found.position) > radius_km)
								continue;

							unique = locations.find_largest_attributed(coordinate).id != found.id;
						}

						if (unique) unique_name = qualified_name(found);
					}

					async.queue_ui([weak_ls, generation, view_generation, marker_index,
						unique_name = std::move(unique_name)]
					{
						const auto s = weak_ls.lock();
						if (!s || s->generation != generation ||
							marker_index >= static_cast<int>(s->marker_place_states.size()))
							return;

						const auto map = s->map.lock();
						if (!map || map->view_generation() != view_generation) return;

						s->marker_place_states[marker_index] =
							advanced_search_location_state::marker_place_state::resolved;
						s->marker_place_names[marker_index] = unique_name;
						if (const auto m = s->map.lock()) m->hover_needs_refresh = true;
					});
				});
		}
		else if (place_state == advanced_search_location_state::marker_place_state::pending)
		{
			needs_refresh = true;
		}

		const auto path = ls->marker_paths[marker_index];

		if (ls->hover_path != path)
		{
			ls->hover_path = path;
			ls->hover_item.reset();
			ls->hover_retries = 20;
		}

		if (!ls->hover_item)
		{
			const auto indexed = state.item_index.find_item(path);
			if (indexed.ft) ls->hover_item = std::make_shared<df::item_element>(path, indexed);
		}

		const auto item = ls->hover_item;
		if (!item) return;

		const auto elements = std::make_shared<view_elements>();
		const auto surface = item->thumbnail_surface();

		if (is_valid(surface))
		{
			elements->add(std::make_shared<surface_element>(surface, 160, flex_item::center,
			                                                item->layout_orientation()));
		}
		else if (is_valid(item->thumbnail()))
		{
			needs_refresh = should_refresh_advanced_search_hover_thumbnail(
				false, true, item->is_staging_thumbnail_surface(), ls->hover_retries);
			if (needs_refresh && !item->is_staging_thumbnail_surface())
			{
				item->stage_thumbnail_surface(state._async, false, true);
			}
		}
		else if (ls->hover_retries > 0)
		{
			// The thumbnail decodes asynchronously; ask once and rebuild the bubble on the
			// next tick so the preview appears without the user moving the mouse.
			ls->hover_retries -= 1;
			needs_refresh = true;

			if (ls->thumbnail_requests.emplace(item->path()).second)
			{
				state.item_index.queue_load_thumbnail(item);
			}
		}

		const auto& place_name = ls->marker_place_names[marker_index];
		const auto caption = count <= 1
			                     ? std::string(item->name().sv())
			                     : place_name.empty()
			                     ? format_plural_text(tt.map_items_here_fmt, count)
			                     : str_format(tt.map_items_close_to_fmt.sv(),
			                                  platform::format_number(str::to_string(count)), place_name);

		elements->add(std::make_shared<text_element>(caption, flex_item::center | flex_item::new_line));

		hover.elements = elements;
		hover.window_bounds = recti(anchor.x - 8, anchor.y - 8, anchor.x + 8, anchor.y + 8);
		hover.active_bounds = recti(anchor.x - 12, anchor.y - 12, anchor.x + 12, anchor.y + 12);
		hover.preferred_size = 180;
		hover.horizontal = false;
	};

	const auto criteria_col = std::make_shared<ui::group_control>();
	criteria_col->add(set_margin(search_collection_radio));
	criteria_col->add(set_margin(search_folder_ratio));
	criteria_col->add(std::make_shared<divider_element>());
	criteria_col->add(set_margin(std::make_shared<ui::term_picker_control>(state, dlg_parent, tt.search_all_terms,
	                                                                       all_terms)));
	criteria_col->add(set_margin(std::make_shared<ui::term_picker_control>(state, dlg_parent, tt.search_none_terms,
	                                                                       none_terms)));
	criteria_col->add(std::make_shared<divider_element>());
	// "Created" and "Modified" mean nothing on their own, so the block says what the boxes
	// are about before the user reads them.
	criteria_col->add(set_margin(std::make_shared<text_element>(tt.dates_title, ui::style::font_face::title)));
	criteria_col->add(set_margin(date_type_control));
	criteria_col->add(set_margin(from_check));
	criteria_col->add(set_margin(until_check));
	criteria_col->add(std::make_shared<divider_element>());
	criteria_col->add(set_margin(std::make_shared<text_element>(tt.media_metadata_title, ui::style::font_face::title)));
	criteria_col->add(set_margin(file_type_control));
	criteria_col->add(std::make_shared<divider_element>());
	criteria_col->add(set_margin(location_check));
	criteria_col->add(set_margin(location_summary));

	const auto cols = std::make_shared<ui::col_control>();
	cols->add(criteria_col);
	cols->add(set_margin(map), {66});

	std::vector<view_element_ptr> controls = {
		set_margin(std::make_shared<ui::title_control>(icon_index::search, tt.command_advanced_search)),
		std::make_shared<divider_element>(),
		cols,
		std::make_shared<divider_element>(),
		std::make_shared<ui::ok_cancel_control>(dlg->_frame)
	};

	// The first pass only has to say where the collection is, so it runs at the coarsest zoom
	// the map can show. Framing on the answer then asks for an accurate rebuild at the zoom
	// the user actually ends up looking at.
	rebuild_markers(map_engine::min_zoom);

	pause_media pause(state);

	const auto result = dlg->show_modal(controls, {122});

	// Stop late marker results from reaching a map that is about to be destroyed, and drop
	// the callbacks that reference the dialog.
	ls->generation += 1;
	ls->pick_generation += 1;
	ls->apply_place = {};
	ls->map.reset();
	map->marker_hover = {};
	map->zoom_changed = {};
	map->marker_picked = {};

	if (result == ui::close_result::ok)
	{
		df::search_t new_search;

		if (search_folder && !str::is_empty(selected_folder))
		{
			new_search.add_selector(df::item_selector(df::folder_path(selected_folder), search_sub_folders));
		}

		if (search_location && location.position.is_valid())
		{
			if (location_name.empty())
			{
				new_search.with(df::search_term(df::search_term_type::location, location.position, location.km,
				                                df::search_term_modifier{}));
			}
			else
			{
				// A named centre keeps the search readable and retypable in the address box.
				auto term = df::search_term(df::search_term_type::location, location_name,
				                            df::search_term_modifier{});
				term.float_val = location.km;
				new_search.with(term);
			}
		}

		if (search_photos)
		{
			new_search.with(df::search_term(file_group::photo, df::search_term_modifier{}));
		}

		if (search_videos)
		{
			new_search.with(df::search_term(file_group::video, df::search_term_modifier{}));
		}

		if (search_audio)
		{
			new_search.with(df::search_term(file_group::audio, df::search_term_modifier{}));
		}

		// One box narrows the range to that date; none or several leave it against all of them, which
		// is what the range means with nothing said about which date it applies to.
		auto date_target = df::date_parts_prop::any;
		const auto date_boxes_checked = (search_date_original ? 1 : 0) + (search_date_created ? 1 : 0) +
			(search_date_modified ? 1 : 0);

		if (date_boxes_checked == 1)
		{
			if (search_date_original) date_target = df::date_parts_prop::original;
			else if (search_date_created) date_target = df::date_parts_prop::created;
			else date_target = df::date_parts_prop::modified;
		}

		if (search_date_from)
		{
			df::date_parts parts(from_val.date(), date_target);
			df::search_term_modifier mod;
			mod.greater_than = true;
			mod.equals = true;
			new_search.with(df::search_term(df::search_term_type::date, parts, mod));
		}

		if (search_date_until)
		{
			df::date_parts parts(until_val.date(), date_target);
			df::search_term_modifier mod;
			mod.less_than = true;
			mod.equals = true;
			new_search.with(df::search_term(df::search_term_type::date, parts, mod));
		}

		search_tokenizer t;

		for (const auto& part : df::coalesce_parts(t.parse(all_terms)))
		{
			new_search.parse_part(part);
		}

		for (auto part : df::coalesce_parts(t.parse(none_terms)))
		{
			part.modifier.positive = false;
			new_search.parse_part(part);
		}

		if (new_search.is_empty())
		{
			// Nothing was chosen, so there is no search to run. Closing on OK without changing
			// what is on screen is indistinguishable from Cancel, so say why.
			dlg->show_message(icon_index::search, tt.command_advanced_search, tt.search_no_criteria);
		}
		else
		{
			state.open(view, new_search, {});
		}
	}
}

#ifndef WINSTORE
void show_update_dialog(view_state& s, const ui::control_frame_ptr& parent)
{
	const auto title = tt.update_title;

	auto dlg = make_dlg(parent);
	std::vector<view_element_ptr> controls;

	pause_media pause(s);

	// Phase 1: always check online for a newer version, showing a busy indicator.
	controls.emplace_back(
		set_margin(std::make_shared<ui::title_control>(icon_index::lightbulb, title)));
	controls.emplace_back(std::make_shared<ui::busy_control>(dlg->_frame, icon_index::lightbulb, tt.update_checking));
	controls.emplace_back(std::make_shared<ui::close_control>(dlg->_frame, true, tt.button_close));

	auto found_version = std::make_shared<std::string>();
	auto check_failed = std::make_shared<bool>(false);

	s.queue_async(async_queue::web, [&s, dlg, found_version, check_failed]
	{
		platform::web_request req;
		req.path = "/ver";
		req.query = platform::web_params{
			{"v"s, std::string(s_app_version)},
			{"b"s, std::string(g_app_build)},
			{"os"s, platform::OS()},
		};

		const auto con = platform::connect_to_host("diffractor.com");
		const auto response = platform::send_request(con, req);

		std::string version;

		if (response.status_code == 200)
		{
			df::util::json::json_doc json;
			json.Parse(response.body);
			version = df::util::json::safe_string(json, "current_version");
		}

		const auto failed = version.empty();

		s.queue_ui([dlg, found_version, check_failed, version, failed]
		{
			*found_version = version;
			*check_failed = failed;
			dlg->close(false);
		});
	});

	if (dlg->show_modal(controls) != ui::close_result::ok)
	{
		return;
	}

	if (*check_failed)
	{
		// Without an answer from the server the stored version says nothing about what is
		// available, so reporting "you are using the latest version" would be a guess.
		dlg->show_message(icon_index::error, title, tt.update_check_failed);
		return;
	}

	if (!found_version->empty())
	{
		setting.available_version = *found_version;
		s.invalidate_view(view_invalid::view_layout | view_invalid::app_layout);
	}

	// Phase 2. Being up to date is news, not a decision, so it is reported as a message rather
	// than as a list of choices; only the upgrade path offers buttons.
	if (df::version(s_app_version) >= df::version(setting.available_version))
	{
		dlg->show_message(icon_index::lightbulb, title, str_format(tt.update_up_to_date_fmt.sv(), s_app_version));
		return;
	}

	controls.clear();
	controls.emplace_back(set_margin(std::make_shared<ui::title_control>(icon_index::lightbulb, title)));
	controls.emplace_back(set_margin(std::make_shared<text_element>(
		str_format(tt.update_help_fmt.sv(), setting.available_version, s_app_version))));

	controls.emplace_back(std::make_shared<ui::button_control>(dlg->_frame, icon_index::import, tt.update_install_now,
	                                                           tt.update_help, [f = dlg->_frame] { f->close(); }));

	controls.emplace_back(std::make_shared<ui::button_control>(dlg->_frame, icon_index::time, tt.update_not_now,
	                                                           tt.update_not_now_help, [&s, f = dlg->_frame]
	                                                           {
		                                                           setting.min_show_update_day = platform::now().
			                                                           to_days() + 7;
		                                                           s.invalidate_view(
			                                                           view_invalid::view_layout |
			                                                           view_invalid::app_layout);
		                                                           f->close(true);
	                                                           }));

	controls.emplace_back(std::make_shared<ui::button_control>(dlg->_frame, icon_index::question, tt.update_more_info,
	                                                           tt.update_more_info_help, [f = dlg->_frame]
	                                                           {
		                                                           platform::open(
			                                                           "https://www.diffractor.com/blog");
		                                                           f->close(true);
	                                                           }));

	if (dlg->show_modal(controls) == ui::close_result::ok)
	{
		controls.clear();
		controls.emplace_back(set_margin(std::make_shared<ui::title_control>(icon_index::import, title)));
		controls.emplace_back(
			std::make_shared<ui::busy_control>(dlg->_frame, icon_index::lightbulb, tt.update_please_wait));
		controls.emplace_back(std::make_shared<ui::close_control>(dlg->_frame, true, tt.button_close));

		struct download_state
		{
			std::atomic_bool active = true;
			// Set on the UI thread when the download reported a result, so a dialog that closed itself
			// can be told apart from one the user dismissed.
			bool completed = false;
			df::file_path path;
		};

		auto state = std::make_shared<download_state>();
		auto download_complete = [&s, dlg, state](const df::file_path download_path)
		{
			s.queue_ui([dlg, state, download_path]
			{
				if (state->active)
				{
					state->completed = true;
					state->path = download_path;
					dlg->close(state->path.is_empty());
				}
			});
		};

		s.queue_async(async_queue::web, [download_complete]
		{
			platform::download_and_verify("diffractor.com"sv, "diffractor-setup.exe"sv, download_complete);
		});

		if (dlg->show_modal(controls) == ui::close_result::ok)
		{
			state->active = false;

			// Launch the downloaded installer. It is interactive and will close any
			// running instance of Diffractor before installing over the current folder.
			const auto module_folder = known_path(platform::known_folder::running_app_folder);
			const auto install_result = platform::install(state->path, module_folder, false, false);

			if (install_result.failed())
			{
				dlg->show_message(icon_index::error, s_app_name, install_result.format_error(tt.update_failed));
			}
		}
		else
		{
			state->active = false;

			if (state->completed)
			{
				// The download answered but produced nothing. Without this the dialog just vanishes
				// and the user is left believing the update installed.
				dlg->show_message(icon_index::error, title, tt.update_failed);
			}
		}
	}
}

#endif

struct keyboard_ref_row
{
	std::string keys;
	std::string description;
};

struct keyboard_ref_section
{
	std::string title;
	std::vector<keyboard_ref_row> rows;
};

// The dialog and the clipboard text are both rendered from this one collection so that they
// cannot drift apart.
static void add_keyboard_section(std::vector<keyboard_ref_section>& sections, const commands_map& commands,
                                 const command_group group, const std::string_view title)
{
	std::vector<command_info_ptr> items;

	for (const auto& c : commands)
	{
		if (c.second->group == group && !c.second->kba.empty())
		{
			items.emplace_back(c.second);
		}
	}

	// A group with no accelerators would otherwise render as a heading over nothing.
	if (items.empty()) return;

	std::ranges::sort(items, [](auto&& left, auto&& right)
	{
		return str::icmp(left->text, right->text) < 0;
	});

	keyboard_ref_section section;
	section.title = title;
	section.rows.reserve(items.size());

	for (const auto& c : items)
	{
		section.rows.emplace_back(c->keyboard_accelerator_text, c->text);
	}

	sections.emplace_back(std::move(section));
}

std::string_view keys::format(const char32_t key)
{
	if (key == BACK) return tt.keyboard_back;
	if (key == BROWSER_BACK) return tt.keyboard_browser_back;
	if (key == BROWSER_FAVORITES) return tt.keyboard_browser_favorites;
	if (key == BROWSER_FORWARD) return tt.keyboard_browser_forward;
	if (key == BROWSER_HOME) return tt.keyboard_browser_home;
	if (key == BROWSER_REFRESH) return tt.keyboard_browser_refresh;
	if (key == BROWSER_SEARCH) return tt.keyboard_browser_search;
	if (key == BROWSER_STOP) return tt.keyboard_browser_stop;
	if (key == DEL) return tt.keyboard_del;
	if (key == DOWN) return tt.keyboard_down;
	if (key == END) return tt.keyboard_end;
	if (key == ESCAPE) return tt.keyboard_escape;
	if (key == F1) return tt.keyboard_f1;
	if (key == F10) return tt.keyboard_f10;
	if (key == F11) return tt.keyboard_f11;
	if (key == F2) return tt.keyboard_f2;
	if (key == F3) return tt.keyboard_f3;
	if (key == F4) return tt.keyboard_f4;
	if (key == F5) return tt.keyboard_f5;
	if (key == F6) return tt.keyboard_f6;
	if (key == F7) return tt.keyboard_f7;
	if (key == F8) return tt.keyboard_f8;
	if (key == F9) return tt.keyboard_f9;
	if (key == HOME) return tt.keyboard_home;
	if (key == INSERT) return tt.keyboard_insert;
	if (key == LEFT) return tt.keyboard_left;
	if (key == MEDIA_NEXT_TRACK) return tt.keyboard_media_next_track;
	if (key == MEDIA_PLAY_PAUSE) return tt.keyboard_media_play_pause;
	if (key == MEDIA_PREV_TRACK) return tt.keyboard_media_prev_track;
	if (key == MEDIA_STOP) return tt.keyboard_media_stop;
	if (key == NEXT) return tt.keyboard_next;
	if (key == OEM_4) return tt.keyboard_oem_4;
	if (key == OEM_6) return tt.keyboard_oem_6;
	if (key == OEM_MINUS) return tt.keyboard_oem_minus;
	if (key == OEM_PLUS) return tt.keyboard_oem_plus;
	if (key == PRIOR) return tt.keyboard_prior;
	if (key == RETURN) return tt.keyboard_enter;
	if (key == RIGHT) return tt.keyboard_right;
	if (key == SPACE) return tt.keyboard_space;
	if (key == TAB) return tt.keyboard_tab;
	if (key == UP) return tt.keyboard_up;
	if (key == VOLUME_DOWN) return tt.keyboard_volume_down;
	if (key == VOLUME_MUTE) return tt.keyboard_volume_mute;
	if (key == VOLUME_UP) return tt.keyboard_volume_up;
	return "?";
}

static bool is_not_virt_key(const int key)
{
	return (key >= '0' && key <= '9') ||
		(key >= 'A' && key <= 'Z');
}

std::string format_keyboard_accelerator(const std::vector<keyboard_accelerator_t>& keyboard_accelerators)
{
	constexpr auto control = keyboard_accelerator_t::control;
	constexpr auto shift = keyboard_accelerator_t::shift;
	constexpr auto alt = keyboard_accelerator_t::alt;

	std::string result;

	for (const auto& ac : keyboard_accelerators)
	{
		// Add Accelerator
		if (!result.empty())
		{
			result += std::format(" {} ", tt.keyboard_or);
		}

		if (ac.key_state & alt)
		{
			result += std::format("{}+", tt.keyboard_alt);
		}
		if (ac.key_state & control)
		{
			result += std::format("{}+", tt.keyboard_control);
		}
		if (ac.key_state & shift)
		{
			result += std::format("{}+", tt.keyboard_shift);
		}

		if (ac.key == keys::RETURN)
		{
			result += tt.keyboard_enter;
		}
		else if (ac.key == keys::BACK)
		{
			result += tt.keyboard_backspace;
		}
		else if (ac.key == keys::OEM_PLUS || ac.key == keys::OEM_MINUS)
		{
			// Quoted so the key is not read as the modifier separator.
			result += std::format("'{}'", keys::format(ac.key));
		}
		else if (is_not_virt_key(ac.key))
		{
			const char szTemp[2] = {static_cast<char>(ac.key), 0};
			result += szTemp;
		}
		else
		{
			result += keys::format(ac.key);
		}
	}

	return result;
}

// The keyboard reference is read top to bottom, so the sections follow the order a user meets
// them rather than the column packing the old layout needed.
static std::vector<keyboard_ref_section> build_keyboard_reference(const commands_map& commands)
{
	std::vector<keyboard_ref_section> sections;

	keyboard_ref_section basics;
	basics.title = tt.keyboard_basics_title;
	basics.rows.emplace_back(format_keyboard_accelerator({keyboard_accelerator_t{keys::RETURN}}),
	                         std::string(tt.keyboard_enter_desc.sv()));
	basics.rows.emplace_back(format_keyboard_accelerator({keyboard_accelerator_t{keys::SPACE}}),
	                         std::string(tt.keyboard_space_desc.sv()));
	basics.rows.emplace_back(format_keyboard_accelerator({keyboard_accelerator_t{keys::ESCAPE}}),
	                         std::string(tt.keyboard_escape_desc.sv()));
	basics.rows.emplace_back(
		format_keyboard_accelerator({keyboard_accelerator_t{keys::LEFT}, keyboard_accelerator_t{keys::RIGHT}}),
		std::string(tt.keyboard_left_right_desc.sv()));
	basics.rows.emplace_back(
		format_keyboard_accelerator({keyboard_accelerator_t{keys::UP}, keyboard_accelerator_t{keys::DOWN}}),
		std::string(tt.keyboard_up_down_desc.sv()));
	basics.rows.emplace_back(
		format_keyboard_accelerator({keyboard_accelerator_t{keys::HOME}, keyboard_accelerator_t{keys::END}}),
		std::string(tt.keyboard_home_end_desc.sv()));
	basics.rows.emplace_back(
		format_keyboard_accelerator({keyboard_accelerator_t{keys::OEM_PLUS}, keyboard_accelerator_t{keys::OEM_MINUS}}),
		std::string(tt.keyboard_zoom_keys_desc.sv()));
	basics.rows.emplace_back(format_keyboard_accelerator({
		                         keyboard_accelerator_t{keys::LEFT, keyboard_accelerator_t::control},
		                         keyboard_accelerator_t{keys::RIGHT, keyboard_accelerator_t::control}
	                         }), std::string(tt.keyboard_ctrl_left_right_desc.sv()));
	sections.emplace_back(std::move(basics));

	const auto& c = commands;

	add_keyboard_section(sections, c, command_group::navigation, tt.keyboard_navigation_title);
	add_keyboard_section(sections, c, command_group::selection, tt.keyboard_selection_title);
	add_keyboard_section(sections, c, command_group::open, tt.keyboard_open_title);
	add_keyboard_section(sections, c, command_group::file_management, tt.keyboard_file_management_title);
	add_keyboard_section(sections, c, command_group::edit_item, tt.keyboard_edit_title);
	add_keyboard_section(sections, c, command_group::media_playback, tt.keyboard_playback_title);
	add_keyboard_section(sections, c, command_group::rate_flag, tt.keyboard_rate_label_title);
	add_keyboard_section(sections, c, command_group::group_by, tt.keyboard_group_title);
	add_keyboard_section(sections, c, command_group::sort_by, tt.command_view_sort);
	add_keyboard_section(sections, c, command_group::tools, tt.keyboard_tools_title);
	add_keyboard_section(sections, c, command_group::options, tt.options_title);
	add_keyboard_section(sections, c, command_group::help, tt.keyboard_help_title);

	return sections;
}

// Tabs between the key and its description so the reference pastes into a document or a
// spreadsheet with the same two columns the dialog shows.
static std::string format_keyboard_reference(const std::vector<keyboard_ref_section>& sections)
{
	std::string result;
	result += tt.keyboard_ref_title.sv();
	result += "\r\n";

	for (const auto& section : sections)
	{
		result += "\r\n";
		result += section.title;
		result += "\r\n";

		for (const auto& row : section.rows)
		{
			result += "\t";
			result += row.keys;
			result += "\t";
			result += row.description;
			result += "\r\n";
		}
	}

	return result;
}

// The dialog stacks its controls in a column, so copy and close need one element to share a row.
class keyboard_ref_buttons final : public view_element, public std::enable_shared_from_this<keyboard_ref_buttons>
{
	ui::button_ptr _copy;
	ui::button_ptr _close;

	mutable int _copy_width = 100;
	mutable int _close_width = 100;

public:
	keyboard_ref_buttons(const ui::control_frame_ptr& h, std::function<void()> copy)
	{
		_copy = h->create_button(tt.copy_to_clipboard, std::move(copy));
		_close = h->create_button(tt.button_close, [h] { h->close(false); }, true);
	}

	void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler) override
	{
		handler(_copy);
		handler(_close);
	}

	sizei measure(ui::measure_context& mc, const int cx) const override
	{
		const auto copy_extent = _copy->measure(cx);
		const auto close_extent = _close->measure(cx);

		_copy_width = std::max(cx / 5, copy_extent.cx + mc.padding2);
		_close_width = std::max(cx / 5, close_extent.cx + mc.padding2);

		return {cx, std::max(copy_extent.cy, close_extent.cy) + mc.padding2};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;

		const auto total_button_width = _copy_width + _close_width + mc.padding2;
		const auto button_rect = center_rect(sizei{total_button_width, bounds.height()}, bounds);

		auto rcopy = button_rect;
		auto rclose = button_rect;

		rcopy.right = rcopy.left + _copy_width;
		rclose.left = rclose.right - _close_width;

		positions.emplace_back(_copy, rcopy, is_visible());
		positions.emplace_back(_close, rclose, is_visible());
	}
};

void show_keyboard_reference(view_state& s, const ui::control_frame_ptr& parent, const commands_map& commands)
{
	const auto dlg = make_dlg(parent);
	const auto sections = build_keyboard_reference(commands);

	std::vector<view_element_ptr> controls;
	controls.emplace_back(set_margin(std::make_shared<ui::title_control>(icon_index::keyboard, tt.keyboard_ref_title)));
	controls.emplace_back(set_margin(std::make_shared<divider_element>()));

	for (const auto& section : sections)
	{
		// Shaded heading over indented rows, matching the grouping used for verbose metadata.
		auto heading = std::make_shared<group_title_control>(section.title);
		heading->padding(8);
		heading->margin(4, 8);
		heading->set_style_bit(view_element_style::background, true);
		controls.emplace_back(std::move(heading));

		const auto table = std::make_shared<ui::table_element>(flex_item::grow);
		table->no_shrink_col[0] = true;

		for (const auto& row : section.rows)
		{
			table->add(icon_index::bullet, row.keys, row.description);
		}

		controls.emplace_back(set_margin(table, 16, 4));
	}

	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<keyboard_ref_buttons>(dlg->_frame, [&sections]
	{
		platform::set_clipboard(format_keyboard_reference(sections));
	}));

	pause_media pause(s);
	// Title above and actions below stay put; only the key list scrolls.
	dlg->_pinned_header = 2;
	dlg->_pinned_footer = 2;
	dlg->show_modal(controls, {88}, {88});
}

void send_info(const view_state& s, const ui::control_frame_ptr& parent)
{
	const auto title = tt.support;
	const auto dlg = make_dlg(parent);

	// Gathered here because it reads app state, then handed to the worker as a detached
	// value. Everything after this - copying and zipping the logs, and the upload itself -
	// runs off the UI thread.
	std::ostringstream message;

	for (const auto& i : calc_app_info(s.item_index, true))
	{
		message << i.first << " " << i.second << '\n';
	}

	const auto results = std::make_shared<command_status>(s._async, dlg, icon_index::support, title, 1);

	s.queue_async(async_queue::web, [results, info = message.str()]
	{
		const auto log_file_path = df::log_path;
		const auto previous_log_path = df::previous_log_path;
		const auto crash_zip_path = platform::temp_file();
		const auto log_file_copy = platform::temp_file();

		if (log_file_path.exists())
		{
			platform::copy_file(log_file_path, log_file_copy, true, true);
		}

		df::zip_file zip;
		auto has_zip = false;

		if (zip.create(crash_zip_path))
		{
			if (log_file_copy.exists()) zip.add(log_file_copy, "diffractor.log");
			if (previous_log_path.exists()) zip.add(previous_log_path);
			has_zip = zip.close();
		}

		platform::web_request req;
		req.verb = platform::web_request_verb::POST;
		req.path = "/crash";
		req.form_data.emplace_back("message", info);
		req.form_data.emplace_back("version", platform::OS());
		req.form_data.emplace_back("diffractor", s_app_version);
		req.form_data.emplace_back("build", g_app_build);
		req.form_data.emplace_back("subject", "Diffractor LOG");
		req.form_data.emplace_back("submit", "Send Report");

		// As the crash report does: an archive that did not close is not attached, and the app
		// information still goes, which is the part a report cannot do without.
		if (has_zip)
		{
			req.file_form_data_name = "ff";
			req.file_name = "logs.zip";
			req.file_path = crash_zip_path;
		}

		const auto con = platform::connect_to_host("diffractor.com");
		const auto response = send_request(con, req);
		const auto sent = response.status_code >= 200 && response.status_code < 300;

		if (log_file_copy.exists()) platform::delete_file(log_file_copy);
		if (crash_zip_path.exists()) platform::delete_file(crash_zip_path);

		// A send that failed is reported. Silence used to be indistinguishable from success.
		if (sent)
		{
			results->complete(tt.diagnostics_sent);
		}
		else
		{
			results->abort(tt.diagnostics_send_failed);
		}
	});

	results->wait_for_complete();
}

void about_invoke(view_state& s, const ui::control_frame_ptr& parent, commands_map& commands)
{
	const auto dlg = make_dlg(parent);
	auto dlg_parent = dlg->_frame;

	std::vector<view_element_ptr> controls;
	controls.emplace_back(create_app_logo_element(s, ui::style::font_face::mega, false, false, 1.6,
	                                              flex_item::center));
	controls.emplace_back(std::make_shared<text_element>(df::format_version(false), ui::style::font_face::dialog,
	                                                     ui::style::text_style::single_line_center,
	                                                     flex_item::center));
	controls.emplace_back(std::make_shared<divider_element>());

	auto cols = std::make_shared<ui::col_control>();

	const auto learn = std::make_shared<ui::group_control>();
	learn->add(std::make_shared<ui::title_control>(icon_index::question, tt.documentation));
	learn->add(std::make_shared<text_element>(tt.about_info));
	learn->add(std::make_shared<link_element>(tt.learn_more_diffractor_com, [] { platform::open(docs_url); }));
	learn->add(std::make_shared<link_element>(tt.releases, [] { platform::open(releases_url); }));
	learn->add(std::make_shared<ui::title_control>(icon_index::keyboard, tt.keyboard));
	learn->add(std::make_shared<link_element>(tt.list_of_accelerators, [&s, dlg_parent, &c = commands]
	{
		show_keyboard_reference(s, dlg_parent, c);
	}));

	const auto support = std::make_shared<ui::group_control>();
	support->add(std::make_shared<ui::title_control>(icon_index::buy, tt.donate));
	support->add(std::make_shared<text_element>(tt.donate_help));
	support->add(std::make_shared<link_element>(tt.donate_link, [] { platform::open(donate_url); }));
	support->add(std::make_shared<ui::title_control>(icon_index::support, tt.support));
	support->add(std::make_shared<link_element>(tt.help_more_info, [] { platform::open(support_url); }));
	support->add(std::make_shared<link_element>(tt.help_send_info, [&s, dlg_parent] { send_info(s, dlg_parent); }));

	cols->add(set_margin(learn, 8, 8));
	cols->add(set_margin(support, 8, 8));
	controls.emplace_back(cols);
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<ui::close_control>(dlg->_frame));

	pause_media pause(s);
	dlg->show_modal(controls, {55});
};

void settings_invoke(view_state& s, const ui::control_frame_ptr& parent)
{
	const auto dlg = make_dlg(parent);

	// Edit a copy so Cancel discards and OK commits, like every other dialog.
	settings_t edited = setting;

	std::shared_ptr<text_element> custom_index_locations_label;

	std::vector<view_element_ptr> controls;

	const auto settings = std::make_shared<ui::group_control>();
	const auto settings2 = std::make_shared<ui::group_control>();
	const auto advanced = std::make_shared<ui::group_control>();

	settings->add(std::make_shared<ui::title_control>(tt.options_app_options));
	settings->add(std::make_shared<ui::check_control>(dlg->_frame, tt.options_show_rotated, edited.show_rotated));
	settings->add(std::make_shared<ui::check_control>(dlg->_frame, tt.options_show_hidden, edited.show_hidden));
	settings->add(std::make_shared<ui::check_control>(dlg->_frame, tt.options_confirm_del, edited.confirm_deletions));
	settings->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.options_confirm_rotate, edited.confirm_rotations));
	settings->add(std::make_shared<ui::check_control>(dlg->_frame, tt.options_show_shadow, edited.show_shadow));
	settings->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.options_last_played_pos, edited.last_played_pos));

	settings->add(std::make_shared<ui::title_control>(tt.option_slideshow_title));
	settings->add(std::make_shared<text_element>(tt.option_slideshow_delay));
	settings->add(std::make_shared<ui::slider_control>(dlg->_frame, std::string_view{}, edited.slideshow_delay,
	                                                   settings_t::min_slideshow_delay,
	                                                   settings_t::max_slideshow_delay));

	settings2->add(std::make_shared<ui::title_control>(tt.options_save_options));
	settings2->add(std::make_shared<ui::check_control>(dlg->_frame, tt.options_backup_copy, edited.create_originals));
	settings2->add(std::make_shared<text_element>(tt.options_jpeg_quality));
	settings2->add(std::make_shared<ui::slider_control>(dlg->_frame, std::string_view{}, edited.jpeg_save_quality, 0,
	                                                    100));
	settings2->add(std::make_shared<text_element>(tt.options_webp_quality));
	settings2->add(std::make_shared<ui::slider_control>(dlg->_frame, std::string_view{}, edited.webp_quality, 1, 100));
	settings2->add(std::make_shared<ui::check_control>(dlg->_frame, tt.lossless_compression, edited.webp_lossless));

#ifndef WINSTORE
	settings2->add(std::make_shared<ui::title_control>(tt.options_updates));
	settings2->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.options_check_for_update, edited.check_for_updates));
#endif

	advanced->add(std::make_shared<ui::title_control>(tt.options_advanced));
	advanced->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.options_show_help_tooltips, edited.show_help_tooltips));
	advanced->add(std::make_shared<ui::check_control>(dlg->_frame, tt.options_use_gpu, edited.use_gpu));
	advanced->add(std::make_shared<ui::check_control>(dlg->_frame, tt.options_use_gpu_video, edited.use_d3d11va));
#ifndef WINSTORE
	advanced->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.options_send_crash_reports, edited.send_crash_dumps));
#endif
	advanced->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.options_show_debug_info, edited.show_debug_info));

	auto cols = std::make_shared<ui::col_control>();
	cols->add(set_margin(settings));
	cols->add(set_margin(settings2));
	cols->add(set_margin(advanced));

	controls.emplace_back(set_margin(
		std::make_shared<ui::title_control2>(dlg->_frame, icon_index::settings, tt.command_options,
		                                     std::string_view{})));
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(cols);
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<ui::ok_cancel_control>(dlg->_frame));

	pause_media pause(s);

	if (dlg->show_modal(controls, {111}) == ui::close_result::ok)
	{
		setting = edited;
		s.invalidate_view(view_invalid::options);
	}
}


static std::string format_index_text(const view_state& s)
{
	const auto file_types = s.item_index.file_types();
	const auto total = file_types.total_items();
	const auto num = platform::format_number(str::to_string(total.count));
	const auto database_size = df::format_size(s.item_index.stats.database_size);
	const auto text = str_format(tt.index_size_fmt.sv(), database_size, num);
	return text;
}

class path_text_element final : public std::enable_shared_from_this<path_text_element>, public view_element
{
	std::string _text;
	df::file_path _path;
	ui::style::font_face _font = ui::style::font_face::dialog;
	ui::style::text_style _text_style = ui::style::text_style::multiline;

public:
	path_text_element(const df::file_path path) noexcept : view_element(
		                                                       view_element_style::has_tooltip |
		                                                       view_element_style::can_invoke), _text(path.str()),
	                                                       _path(path)
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		dc.draw_text(_text, logical_bounds, _font, _text_style, ui::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		sizei result;

		if (!_text.empty())
		{
			result = mc.measure_text(_text, _font, _text_style, width_limit);
		}

		return result;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			platform::show_in_file_browser(_path);
		}
	}

	void tooltip(view_hover_element& result, const pointi loc, const pointi element_offset) const override
	{
		result.elements->add(make_icon_element(icon_index::data, flex_item::no_break));
		result.elements->add(std::make_shared<text_element>(_text, ui::style::font_face::title,
		                                                    ui::style::text_style::multiline,
		                                                    flex_item::line_break));
		result.active_bounds = result.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

static void index_maintenance(const ui::control_frame_ptr& parent, const view_state& s)
{
	const auto dlg = make_dlg(parent);
	const auto title = tt.index_maintenance_title;
	bool is_reset = false;

	// Filled in by the UI hop that reports the database work, never by the database thread itself, and
	// `complete` says that hop has run. A busy wait can end before the work does, so without it the
	// UI read an answer the worker was still writing.
	struct database_result
	{
		bool has_errors = false;
		std::string error;
		bool complete = false;
	};

	// The dialog crosses to the database thread only as a lifetime token. Holding it there let a late
	// answer close whatever the dialog showed next - the confirmation, read as approval - and let that
	// thread drop the dialog's last reference.
	const std::weak_ptr<dialog> weak_dlg = dlg;

	const auto check_result = std::make_shared<database_result>();
	// The busy window says which operation is running; "Processing..." said nothing.
	dlg->show_status(icon_index::star, title);
	s._async.queue_database([&s, weak_dlg, check_result](const database& db)
	{
		auto has_errors = false;
		std::string error;

		try
		{
			has_errors = db.has_errors();
		}
		catch (const std::exception& e)
		{
			error = str::utf8_cast(e.what());
		}

		s.queue_ui([weak_dlg, check_result, has_errors, error = std::move(error)]
		{
			check_result->has_errors = has_errors;
			check_result->error = error;
			check_result->complete = true;
			if (const auto d = weak_dlg.lock()) d->close(false);
		});
	});
	dlg->wait_for_close();

	// The check only reads, so Escape during it stops maintenance here. Nothing has been asked yet,
	// and the check's late answer finds no dialog to act on.
	if (!check_result->complete) return;

	if (!check_result->error.empty())
	{
		dlg->show_message(icon_index::error, title, check_result->error);
		return;
	}

	std::vector<view_element_ptr> controls = {
		set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::settings, title,
		                                                tt.defragment_and_compact)),
		std::make_shared<divider_element>(),
		set_margin(std::make_shared<text_element>(format_index_text(s))),
		set_margin(std::make_shared<path_text_element>(s.item_index.stats.database_path)),
		set_margin(std::make_shared<text_element>(tt.index_maintenance_help)),
		set_margin(std::make_shared<ui::check_control>(dlg->_frame, tt.reset_database, is_reset)),
		std::make_shared<divider_element>(),
		std::make_shared<ui::ok_cancel_control>(dlg->_frame)
	};

	if (check_result->has_errors)
	{
		controls.emplace_back(set_margin(set_padding(
			std::make_shared<text_element>(tt.index_maintenance_reset_recommended, ui::style::text_style::multiline), 8,
			8)));
	}

	if (dlg->show_modal(controls) == ui::close_result::ok)
	{
		dlg->show_status(icon_index::star, is_reset ? tt.resetting : tt.defragmenting);
		const auto maintenance_result = std::make_shared<database_result>();
		const auto finished = std::make_shared<platform::thread_event>(true, false);

		s._async.queue_database([&s, maintenance_result, finished, is_reset](database& db)
		{
			std::string error;

			try
			{
				db.maintenance(is_reset);
			}
			catch (const std::exception& e)
			{
				error = str::utf8_cast(e.what());
			}

			s.queue_ui([maintenance_result, finished, error = std::move(error)]
			{
				maintenance_result->error = error;
				maintenance_result->complete = true;
				finished->set();
			});
		});

		// The database is being rewritten and cannot be stopped part way, so this waits for the work
		// rather than for the window: Escape leaves the busy window up until the work is done. A
		// dialog that could not create its window answers every wait at once, so waiting on it in a
		// loop would spin the UI thread on a completion only a pumping wait can deliver.
		platform::wait_for({*finished, platform::event_exit}, 0, false);

		// Only an application closing underneath the wait leaves the result unreported.
		if (!maintenance_result->complete) return;

		if (maintenance_result->error.empty())
		{
			// Reset leaves an empty database, so the collection has to be re-read from the files.
			// That runs in the background with the normal indexing progress, not behind this dialog.
			s.invalidate_view(is_reset ? view_invalid::index_rebuild : view_invalid::index);
		}
		else
		{
			dlg->show_message(icon_index::error, title, maintenance_result->error);
		}
	}
};


void index_settings_invoke(view_state& s, const ui::control_frame_ptr& parent,
                                  settings_t::index_t collection_settings)
{
	const auto dlg = make_dlg(parent);

	std::vector<view_element_ptr> controls;
	std::shared_ptr<ui::folder_picker_control> more_custom_index_paths;

	auto dlg_parent = dlg->_frame;
	const auto local_index = std::make_shared<ui::group_control>();
	const auto custom_index = std::make_shared<ui::group_control>();

	auto index_text = format_index_text(s);

	// Maintenance is immediate and irreversible, so it must not run while this dialog can
	// still be cancelled. The link closes the dialog as OK - committing the folder choices
	// the user can see - and maintenance starts once that has happened.
	const auto run_maintenance = std::make_shared<bool>(false);

	local_index->add(std::make_shared<text_element>(tt.collection_info));
	local_index->add(std::make_shared<link_element>(tt.more_collection_options_information,
	                                                [] { platform::open(docs_url); }));

	const auto local_folders = platform::local_folders();

	local_index->add(std::make_shared<ui::title_control>(tt.collection_options_local_folders_title));
	local_index->add(
		std::make_shared<ui::check_control>(dlg_parent, local_folders.pictures.text(), collection_settings.pictures));
	local_index->add(
		std::make_shared<ui::check_control>(dlg_parent, local_folders.video.text(), collection_settings.video));
	local_index->add(
		std::make_shared<ui::check_control>(dlg_parent, local_folders.music.text(), collection_settings.music));

	if (local_folders.onedrive_pictures.exists())
		local_index->add(
			std::make_shared<ui::check_control>(dlg_parent, local_folders.onedrive_pictures.text(),
			                                    collection_settings.onedrive_pictures));
	if (local_folders.onedrive_video.exists())
		local_index->add(
			std::make_shared<ui::check_control>(dlg_parent, local_folders.onedrive_video.text(),
			                                    collection_settings.onedrive_video));
	if (local_folders.onedrive_music.exists())
		local_index->add(
			std::make_shared<ui::check_control>(dlg_parent, local_folders.onedrive_music.text(),
			                                    collection_settings.onedrive_music));
	if (local_folders.dropbox_photos.exists())
		local_index->add(
			std::make_shared<ui::check_control>(dlg_parent, local_folders.dropbox_photos.text(),
			                                    collection_settings.drop_box));

	local_index->add(std::make_shared<ui::title_control>(tt.index_maintenance_title));
	local_index->add(std::make_shared<text_element>(tt.indexing_message));
	local_index->add(std::make_shared<text_element>(index_text));
	local_index->add(std::make_shared<link_element>(tt.defragment_and_compact, [run_maintenance, dlg_parent]
	{
		*run_maintenance = true;
		dlg_parent->close(false);
	}));

	custom_index->add(std::make_shared<ui::title_control>(tt.collection_options_custom_folders_title));
	custom_index->add(std::make_shared<text_element>(tt.collection_options_more_folders));

	auto more_folders_parts = str::split(collection_settings.more_folders, true,
	                                     [](const char c) { return c == '\n' || c == '\r'; });
	auto more_folders_text = str::combine(more_folders_parts, "\r\n", false);

	custom_index->add(
		more_custom_index_paths = std::make_shared<ui::folder_picker_control>(dlg_parent, more_folders_text, true));
	custom_index->add(std::make_shared<text_element>(tt.collection_options_custom_locations_help));
	custom_index->add(std::make_shared<text_element>(tt.collection_options_custom_folders_help));


	auto cols = std::make_shared<ui::col_control>();
	cols->add(set_margin(local_index));
	cols->add(set_margin(custom_index));

	controls.emplace_back(set_margin(
		std::make_shared<ui::title_control2>(dlg->_frame, icon_index::set, tt.command_collection_options,
		                                     tt.collection_options_info)));
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(cols);
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<ui::ok_cancel_control>(dlg->_frame));

	pause_media pause(s);

	if (ui::close_result::ok == dlg->show_modal(controls, {99}))
	{
		// apply changes
		more_folders_parts = str::split(more_folders_text, false,
		                                [](const char c) { return c == '\n' || c == '\r'; });
		collection_settings.more_folders = str::combine(more_folders_parts, "\n", true);
		setting.collection = collection_settings;
	}

	dlg->_frame->destroy();
	s.invalidate_view(view_invalid::index | view_invalid::options);

	if (*run_maintenance)
	{
		index_maintenance(parent, s);
	}
}

void customise_invoke(view_state& s, const ui::control_frame_ptr& parent)
{
	const auto dlg = make_dlg(parent);
	// Edit copies so Cancel discards and OK commits, like every other dialog.
	auto search = setting.search;
	auto sidebar_settings = setting.sidebar;

	auto dlg_parent = dlg->_frame;
	const auto searches = std::make_shared<ui::group_control>();
	const auto sidebar = std::make_shared<ui::group_control>();

	searches->add(std::make_shared<ui::title_control>(tt.customise_searches_title));
	searches->add(
		std::make_shared<ui::two_col_table_control>(dlg_parent, search.title, search.path,
		                                            search.count));

	sidebar->add(std::make_shared<ui::title_control>(tt.customise_sidebar_title));
	sidebar->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.customize_show_total, sidebar_settings.show_total_items));
	sidebar->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.customize_show_history, sidebar_settings.show_history));
	sidebar->add(std::make_shared<ui::check_control>(dlg->_frame, tt.customize_show_world_map,
	                                                 sidebar_settings.show_world_map));
	sidebar->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.customize_show_drives, sidebar_settings.show_drives));
	sidebar->add(std::make_shared<ui::check_control>(dlg->_frame, tt.customize_show_searches,
	                                                 sidebar_settings.show_favorite_searches));
	sidebar->add(
		std::make_shared<ui::check_control>(dlg->_frame, tt.customize_show_tags, sidebar_settings.show_tags));
	sidebar->add(std::make_shared<ui::check_control>(dlg->_frame, tt.option_favorite_tags,
	                                                 sidebar_settings.show_favorite_tags_only));
	sidebar->add(std::make_shared<ui::check_control>(dlg->_frame, tt.customize_ratings,
	                                                 sidebar_settings.show_ratings));
	sidebar->add(std::make_shared<ui::check_control>(dlg->_frame, tt.customize_labels,
	                                                 sidebar_settings.show_labels));
	sidebar->add(set_margin(std::make_shared<text_element>(tt.customize_history_start_year)));
	sidebar->add(std::make_shared<ui::num_control>(dlg->_frame, std::string_view{},
	                                               sidebar_settings.history_start_year, true));

	auto cols = std::make_shared<ui::col_control>();
	cols->add(set_margin(searches));
	cols->add(set_margin(sidebar));

	std::vector<view_element_ptr> controls;
	controls.emplace_back(set_margin(
		std::make_shared<ui::title_control2>(dlg->_frame, icon_index::settings, tt.command_customise,
		                                     tt.customise_sidebar_desc)));
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(cols);
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<ui::ok_cancel_control>(dlg->_frame));

	pause_media pause(s);

	if (dlg->show_modal(controls, {111}) == ui::close_result::ok)
	{
		setting.search = search;
		setting.sidebar = sidebar_settings;

		s.invalidate_view(view_invalid::sidebar | view_invalid::options_save | view_invalid::command_state |
			view_invalid::tooltip | view_invalid::app_layout);
	}
};

// The share sheet needs a title. One file is named; several are counted.
static std::string share_sheet_title(const std::vector<df::file_path>& paths)
{
	return paths.size() == 1
		       ? paths.front().name().str()
		       : format_plural_text(tt.title_item_count_fmt, static_cast<int64_t>(paths.size()));
}

// MAPI shows the mail client's own modal UI, so the draft is created on the UI thread despite
// blocking it. The progress dialog stays up meanwhile, saying what it is waiting for.
static void email_outgoing_files(view_state& s, const item_results_ptr& results,
                                 const std::vector<df::file_path>& file_paths, const outgoing_options& outgoing)
{
	s.queue_async(async_queue::work, [&s, results, file_paths, outgoing]
	{
		const auto staging = outgoing_staging_folder();
		const auto prepared = prepare_outgoing_files(file_paths, outgoing, staging, *results);

		// A cancel that lands after the last file is still a decision not to send.
		if (!prepared.success || results->is_canceled())
		{
			results->complete(results->is_canceled()
				                  ? std::string_view{}
				                  : prepared.error.empty()
				                  ? tt.email_failed.sv()
				                  : std::string_view{prepared.error});
			return;
		}

		platform::attachments_t attachments;

		for (const auto& path : prepared.paths)
		{
			attachments.emplace_back(path.name().str(), path);
		}

		results->message(tt.email_connecting_to_mapi);

		s.queue_ui([&s, attachments, results, staging, wrote_copies = outgoing.writes_copies()]
		{
			results->message(tt.email_sending);

			// The second hop exists so the message above paints before MAPI blocks.
			s.queue_ui([&s, attachments, results, staging, wrote_copies]
			{
				const auto send_result = platform::mapi_send({}, {}, {}, attachments);

				if (send_result == platform::mapi_send_result::sent)
				{
					results->complete();
				}
				else if (send_result == platform::mapi_send_result::canceled)
				{
					results->complete(tt.email_canceled);
				}
				else
				{
					results->complete(tt.email_failed);
				}

				// The draft holds its own copies of the attachments by now.
				if (wrote_copies)
				{
					s.queue_async(async_queue::work, [staging] { clear_outgoing_staging(staging); });
				}
			});
		});
	});

	results->wait_for_complete();
}

void share_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view)
{
	// Where the system has no share sheet, the command makes an email draft instead.
	const auto use_sheet = platform::can_share_files();
	const auto title = use_sheet ? tt.command_share.sv() : tt.command_share_email.sv();
	const auto icon = use_sheet ? icon_index::share : icon_index::mail;
	auto dlg = make_dlg(parent);

	pause_media pause(s);

	if (can_process_selection_or_explain(s, view, parent, title, df::process_items_type::local_file))
	{
		const auto& items = s.selected_items();

		// Share and Email each keep their own choices. Edit a copy so Cancel discards and OK
		// commits, matching every other options dialog.
		settings_t::outgoing_choices_t& saved_choices = use_sheet ? setting.share : setting.email;
		auto choices = saved_choices;
		std::string validation_error;

		for (;;)
		{
			std::vector<view_element_ptr> controls;
			auto title_control = std::make_shared<ui::title_control2>(
				dlg->_frame, icon, title,
				format_plural_text(use_sheet ? tt.share_info_fmt : tt.email_info_fmt, items),
				std::vector<ui::const_surface_ptr>{}, items.size());
			title_control->selection_async(items.thumbs(), items.size(), s._async);
			controls.emplace_back(set_margin(title_control));
			controls.emplace_back(std::make_shared<divider_element>());
			controls.emplace_back(set_margin(std::make_shared<text_element>(
				use_sheet ? tt.share_small_help : tt.email_small_help)));
			controls.emplace_back(std::make_shared<ui::check_control>(
				dlg->_frame, use_sheet ? tt.share_zip : tt.email_zip, choices.zip));
			controls.emplace_back(
				std::make_shared<ui::check_control>(dlg->_frame, tt.email_convert_to_jpeg, choices.convert));

			auto limit = std::make_shared<ui::check_control>(dlg->_frame, tt.email_limit_dimensions,
			                                                 choices.limit);
			limit->child(std::make_shared<ui::num_control>(dlg->_frame, std::string_view{}, choices.max_side));
			controls.emplace_back(limit);

			if (!validation_error.empty())
			{
				auto warning = std::make_shared<text_element>(
					validation_error, flex_item::stretch | view_element_style::important);
				warning->margin = {10, 10};
				warning->padding = {10, 10};
				warning->update_background_color();
				controls.emplace_back(std::move(warning));
			}

			controls.emplace_back(std::make_shared<divider_element>());
			controls.emplace_back(std::make_shared<ui::ok_cancel_control>(
				dlg->_frame, use_sheet ? tt.button_share : tt.button_send));

			if (ui::close_result::ok != dlg->show_modal(controls)) return;

			// A limit below one pixel would scale every photo to nothing. The dialog reopens with the
			// choices intact rather than discarding them.
			if (choices.limit && choices.max_side < 1)
			{
				validation_error = tt.dimension_must_be_positive;
				continue;
			}

			break;
		}

		saved_choices = choices;
		record_feature_use(use_sheet ? features::share : features::email);

		outgoing_options outgoing;
		outgoing.zip = choices.zip;
		outgoing.convert_to_jpeg = choices.convert;
		outgoing.max_side = choices.limit ? choices.max_side : 0;

		const auto file_paths = items.file_paths(false);

		if (!use_sheet)
		{
			email_outgoing_files(s, std::make_shared<command_status>(s._async, dlg, icon, title, file_paths.size(),
			                                                         tt.email_preparing),
			                     file_paths, outgoing);
			return;
		}

		const auto prepared = std::make_shared<outgoing_files>();

		if (outgoing.writes_copies())
		{
			const auto results = std::make_shared<command_status>(s._async, dlg, icon, title, file_paths.size(),
			                                                      tt.share_preparing);

			s.queue_async(async_queue::work, [results, file_paths, outgoing, prepared]
			{
				// Ends the run even if preparation throws, so the dialog waiting on it closes.
				result_scope rr(results);

				// Written before the completion is published; the UI thread reads it only once that
				// completion has been drained.
				*prepared = prepare_outgoing_files(file_paths, outgoing, outgoing_staging_folder(), *results);

				// A cancel that lands after the last file is still a decision not to share.
				if (results->is_canceled()) prepared->success = false;

				rr.complete(prepared->success || results->is_canceled()
					            ? std::string_view{}
					            : prepared->error.empty()
					            ? tt.share_failed.sv()
					            : std::string_view{prepared->error});
			});

			results->wait_for_complete();
		}
		else
		{
			prepared->paths = file_paths;
			prepared->success = true;
		}

		// Every dialog has closed by now, so the sheet opens over the app rather than over one of them.
		if (prepared->success && !platform::share_files(prepared->paths, share_sheet_title(prepared->paths)))
		{
			dlg->show_message(icon_index::error, title, tt.share_failed);
		}
	}
}
