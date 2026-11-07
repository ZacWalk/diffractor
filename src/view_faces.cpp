// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The face view's off-thread half - collecting the groups, reading their sample thumbnails
// and cropping them - plus its hit testing and its controls panel. The rows and their drawing live
// in view_faces.h. Owned by docs/faces.md.

#include "pch.h"
#include "view_faces.h"

#include "model_index.h"
#include "ui_dialog.h"

// A row searches for its group. That is the whole of what a row does: a group has no name, so there
// is nothing else to offer and no second target to aim at.
class face_row_controller final : public view_controller
{
	std::shared_ptr<faces_view> _view;
	faces::palette_entry _entry;
	bool _tracking = false;

public:
	face_row_controller(const view_host_ptr& host, std::shared_ptr<faces_view> view, const recti bounds,
	                    faces::palette_entry entry)
		: view_controller(host, bounds), _view(std::move(view)), _entry(std::move(entry))
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::link;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		const auto invoke = _tracking && _bounds.contains(loc);
		_tracking = false;
		if (!invoke) return;
		_view->search_for(_host, _entry);
	}

	void popup_from_location(view_hover_element& hover) override
	{
		hover.active_bounds = hover.window_bounds = _bounds;
		hover.elements = std::make_shared<view_elements>();
		hover.elements->add(std::make_shared<text_element>(tt.face_search_this));
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		return true;
	}
};

view_controller_ptr faces_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                         hit_test_context& ctx)
{
	if (_scroller.can_scroll() && ctx.occluded(_scroller.scroll_bounds()))
	{
		return std::make_shared<scroll_controller>(host, _scroller, _scroller.scroll_bounds());
	}

	const auto offset = _scroller.scroll_offset();

	for (const auto& r : _rows)
	{
		const auto bounds = r.bounds.offset(0, -offset.y);
		if (!bounds.contains(loc)) continue;
		if (ctx.occluded(bounds))
			return std::make_shared<face_row_controller>(host, shared_from_this(), bounds, r.entry);
	}

	return nullptr;
}

void faces_view::activate(const sizei extent)
{
	_active = true;
	_extent = extent;
	_enabled = setting.faces.enabled;
	if (!_state.item_index.face_scan_complete()) _state.item_index.queue_update_face_groups();
	refresh();
}

// The same four hops the sidebar uses: the projection walks every face, the thumbnails come off the
// database thread, and the decode and crop happen on a worker. None of it may run where paint does.
void faces_view::refresh()
{
	if (!_active) return;
	if (!_state.item_index.face_search_enabled())
	{
		_rows.clear();
		_status = tt.faces_unavailable;
		return;
	}
	if (_in_flight)
	{
		_again = true;
		return;
	}
	_in_flight = true;
	_again = false;
	_thumbnail_window = thumbnail_window();
	const auto [first, last] = _thumbnail_window;
	const auto request = ++_request;
	const auto generation = _state.item_index.face_generation();
	face_thumbnail_cache held;
	for (size_t index = 0; index < _rows.size(); ++index)
	{
		auto& row = _rows[index];
		if (index >= first && index < last && ui::is_valid(row.thumbnail))
			held[row.entry.sample] = {row.entry, row.thumbnail};
		else
		{
			row.thumbnail.reset();
			row.tex.reset();
		}
	}
	auto* const index = &_state.item_index;
	auto* const async = &_state._async;
	const auto view = ui_owned(*async, shared_from_this());
	const auto failed = [async, view]
	{
		async->queue_ui([view] { view->finish_refresh(); });
	};
	async->queue_async(async_queue::sidebar,
		[index, async, view, failed, first, last, request, generation, held = std::move(held)]() mutable
		{
			try
			{
				auto entries = index->face_palette_entries();
				async->queue_database([async, view, failed, first, last, request, generation,
					entries = std::move(entries), held = std::move(held)](const database& db) mutable
				{
					try
					{
						std::vector<ui::const_image_ptr> encoded(entries.size());
						std::vector<ui::const_surface_ptr> surfaces(entries.size());
						for (size_t position = first; position < std::min(last, entries.size()); ++position)
						{
							const auto& entry = entries[position];
							const auto cached = held.find(entry.sample);
							if (cached != held.end())
							{
								const auto& old = cached->second.entry;
								if (old.source == entry.source && old.crop_space == entry.crop_space &&
									old.crop.X == entry.crop.X && old.crop.Y == entry.crop.Y &&
									old.crop.Width == entry.crop.Width && old.crop.Height == entry.crop.Height)
									surfaces[position] = cached->second.pixels;
							}
							if (!surfaces[position])
								encoded[position] = db.load_thumbnail(df::file_path(entry.sample.path.sv())).thumb;
						}
						async->queue_async(async_queue::sidebar, [async, view, failed, first, last, request, generation,
							entries = std::move(entries), encoded = std::move(encoded), surfaces = std::move(surfaces)]() mutable
						{
							try
							{
								files decoder;
								for (size_t position = first; position < std::min(last, entries.size()); ++position)
									if (!surfaces[position]) surfaces[position] = crop_face_thumbnail(decoder, encoded[position], entries[position]);
								async->queue_ui([view, request, generation, entries = std::move(entries), surfaces = std::move(surfaces)]() mutable
								{
									view->publish(request, generation, std::move(entries), std::move(surfaces));
								});
							}
							catch (const std::exception& error)
							{
								df::log(__FUNCTION__, error.what());
								failed();
							}
						});
					}
					catch (const std::exception& error)
					{
						df::log(__FUNCTION__, error.what());
						failed();
					}
				});
			}
			catch (const std::exception& error)
			{
				df::log(__FUNCTION__, error.what());
				failed();
			}
		});
}

void faces_view::finish_refresh()
{
	_in_flight = false;
	if (_active && _again) refresh();
}

void faces_view::publish(const uint64_t request, const uint64_t generation,
                         std::vector<faces::palette_entry> entries,
                         std::vector<ui::const_surface_ptr> surfaces)
{
	const df::scope_exit release([this] { finish_refresh(); });

	// A result from a run the user has already moved past says nothing about what is on screen now.
	if (!_active || !_state.item_index.face_search_enabled() || request != _request ||
		_state.item_index.face_generation() != generation) return;

	_rows.clear();
	_rows.reserve(entries.size());

	for (auto i = 0u; i < entries.size(); ++i)
	{
		row r;
		r.entry = std::move(entries[i]);
		if (i < surfaces.size()) r.thumbnail = std::move(surfaces[i]);
		_rows.emplace_back(std::move(r));
	}

	// A row whose file has never had a thumbnail cached has nothing to crop, and most of a collection
	// has not been asked for one. Requesting it is what fills the column in; the request set stops the
	// same file being asked for on every refresh.
	const auto [first, last] = thumbnail_window();
	for (size_t position = first; position < std::min(last, _rows.size()); ++position)
	{
		const auto& r = _rows[position];
		if (ui::is_valid(r.thumbnail)) continue;

		const df::file_path path(r.entry.sample.path.sv());
		if (!_thumbnail_requests.emplace(path).second) continue;

		const auto indexed = _state.item_index.find_item(path);
		if (!indexed.ft) continue;

		_state.item_index.queue_load_thumbnail(std::make_shared<df::item_element>(path, indexed),
		                                       view_invalid::faces_view);
	}

	_complete = _state.item_index.face_scan_complete();
	update_status();
	_state.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw | view_invalid::controller);
}

// The bar under the list is the only place a long-running pass can say it is still going while the
// list already has something in it.
void faces_view::update_status()
{
	_complete = _state.item_index.face_scan_complete();
	const auto remaining = _state.item_index.faces_remaining.load();

	if (remaining > 0)
	{
		_status = std::format("{} - {}", tt.faces_finding.sv(),
		                      format_plural_text(tt.face_search_pictures_fmt, static_cast<uint32_t>(remaining)));
		return;
	}

	_status = _complete ? std::string{} : std::string(tt.faces_partial.sv());
}

view_controls_host_ptr faces_view::controls(const ui::control_frame_ptr& owner)
{
	auto result = std::make_shared<view_controls_host>(_state);
	const auto frame = owner->create_dlg(result, false);

	// The panel is built before the view is activated, so the checkbox is bound to a value that has
	// to be current NOW or it shows the state the application started in.
	_enabled = setting.faces.enabled;

	std::vector<view_element_ptr> controls;

	controls.emplace_back(std::make_shared<ui::title_control2>(frame, icon_index::person, tt.face_search,
	                                                          tt.face_search_what));
	controls.emplace_back(std::make_shared<divider_element>());

	if (!_state.item_index.face_engine_available())
	{
		controls.emplace_back(std::make_shared<text_element>(tt.faces_unavailable));
	}
	else
	{
		auto group = std::make_shared<ui::group_control>();
		group->add(std::make_shared<text_element>(
			format_plural_text(tt.face_search_pictures_fmt,
			                   static_cast<uint32_t>(_state.item_index.stats.picture_item_count))));
		group->add(std::make_shared<text_element>(tt.face_search_cost));
		group->add(std::make_shared<text_element>(tt.face_search_scope));
		group->add(std::make_shared<text_element>(tt.face_search_privacy));
		group->add(std::make_shared<text_element>(tt.face_search_undo));
		controls.emplace_back(group);

		controls.emplace_back(std::make_shared<divider_element>());

		// The panel has no OK button, so a change applies where it is made. Turning face search off
		// is what discards the faces, so it goes through the same entry point the dialog used.
		controls.emplace_back(std::make_shared<ui::check_control>(
			frame, tt.face_search_enable, _enabled, false, false,
			[this](const bool checked)
			{
				_state.item_index.set_face_search_enabled(checked);
				_state.invalidate_view(view_invalid::options_save | view_invalid::sidebar_faces |
					view_invalid::command_state | view_invalid::app_layout);
				refresh();
			}));
	}

	for (const auto& c : controls)
	{
		c->margin.cx = 8;
		c->margin.cy = 4;
	}

	result->_controls = controls;
	result->_frame = result->_dlg = frame;
	return result;
}
