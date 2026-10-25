// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: What one opened display shows: the item or items on screen, their textures and decoders,
// the player session, zoom and panorama views, comparison, and the media controls drawn over them.
// Split from model.h so the many files that only need view_state do not compile the audio and
// video headers this depends on.

#pragma once

#include "model.h"
#include "av_format.h"
#include "av_player.h"

class display_state_t final : public std::enable_shared_from_this<display_state_t>
{
public:
	async_strategy& _async;
	common_display_state_t& _common;

	bool _comparing = false;
	df::comparison_zoom_state _comparison_zoom;

	std::shared_ptr<av_session> _session;
	// Bumped whenever the session is torn down or superseded. An open that completes after a bump has
	// lost the display and must close what it was handed, not publish it. UI thread only.
	uint32_t _av_generation = 0;

	df::blob _selected_item_data;
	std::vector<archive_item> _archive_items;
	av_media_info _player_media_info;
	// Set once the full-file metadata scan (or player open) has completed for this display item.
	// Reading the whole file finishes hydrating a cloud-only placeholder, so this is the point at
	// which a previously offline item can be safely re-indexed to pick up its real metadata.
	bool _full_metadata_loaded = false;

	// The decoder could not make sense of this file, so there is nothing to play or show. The panel
	// falls back to a hex dump of the bytes, which at least says what the file really is.
	bool _av_open_failed = false;

	texture_state_ptr _selected_texture1;
	texture_state_ptr _selected_texture2;

	df::item_element_ptr _item1;
	df::item_element_ptr _item2;

	// Painted track width, used only to quantise scrubber redraws to whole pixels.
	mutable int _scrubber_width = 0;

	int _hover_scrubber_pos = -1;
	mutable int _time_width = 0;
	int _next_photo_tick = 0;
	// Latches the end of the current clip until the queued pause/seek has actually taken effect.
	bool _media_end_handled = false;

	ui::pixel_difference_result _pixel_difference = ui::pixel_difference_result::unknown;
	int _last_scrubber_pos = -1;
	int _last_duration = -1;
	int _last_seconds = -1;

	std::string _duration;
	std::string _time;

	ui::const_surface_ptr _hover_surface;

	mutable ui::vertices_ptr _audio_verts;
	mutable pointi _audio_element_offset;
	mutable recti _audio_element_bounds;
	mutable float _audio_element_alpha = 1.0f;

	int _compare_hover_loc = 0;
	int _compare_video_pos = 0;
	bool _temporary_zoom = false;
	uint64_t _zoom_activity_generation = 0;
	double _zoom_activity_time = 0.0;
	float _zoom_overlay_alpha = 1.0f;
	recti compare_control_bounds;
	recti _compare_bounds;
	recti _compare_limits;
	recti _compare_video_control_bounds;
	recti _compare_video_scrubber_bounds;
	// Clickable A and B pane markers. Two entries while both images show; only the first while magnified.
	mutable std::array<recti, 2> _pane_marker_bounds;
	bool _can_compare = false;
	bool _is_compare_video = false;
	bool _comparison_eligible = false;
	bool _is_one = false;
	bool _is_two = false;
	bool _is_multi = false;
	bool _can_zoom = false;

	struct zoom_layout_state
	{
		sized source_extent;
		sized viewport_extent;
		pointd viewport_origin;
		pointd pending_source_anchor;
		pointd pending_client_anchor;
		bool pending_reanchor = false;
	};

	std::array<zoom_layout_state, 2> _zoom_layouts;
	// The declaration request this display has already queued. Held here rather than on the session
	// because the session outlives every display: a read released with its display publishes nothing,
	// and the next display is the thing that should ask again.
	df::file_path _panorama_read_path;
	sizei _panorama_read_source;
	df::date_t _panorama_read_modified;

	std::vector<ui::const_image_ptr> _images;
	std::vector<ui::const_surface_ptr> _surfaces;
	// UI-owned, index-aligned with _images/_surfaces so a collage cell can name the item it shows.
	df::item_elements _collage_source_items;
	mutable std::vector<ui::texture_ptr> _textures;
	std::vector<recti> _surface_bounds;
	size_t _selection_item_count = 0;
	size_t _collage_image_count = 0;
	size_t _selection_overflow_count = 0;
	static constexpr size_t max_surfaces = 24;
	pointi media_offset;

	ui::animate_alpha _loading_alpha_animation;

	explicit display_state_t(async_strategy& async, common_display_state_t& common)
		: _async(async), _common(common)
	{
	}

	void populate(const view_state& state);

	bool is_one() const
	{
		return _is_one;
	}

	bool is_two() const
	{
		return _is_two;
	}

	// Two selected files is cardinality; comparison is the separate claim that the pair is like with like.
	bool is_comparison() const
	{
		return _is_two && _comparison_eligible;
	}

	render_valid update_for_present(double time_now) const;

	double media_pos() const
	{
		return _session ? _session->last_frame_time() : 0.0;
	}

	double media_start() const
	{
		return _player_media_info.start;
	}

	double media_end() const
	{
		return _player_media_info.end;
	}

	bool comparing() const
	{
		return _comparing;
	}

	static constexpr size_t zoom_pane_index(const df::zoom_pane pane) noexcept
	{
		return pane == df::zoom_pane::primary ? 0 : 1;
	}

	df::zoom_pane active_zoom_pane() const noexcept
	{
		return _is_two ? _comparison_zoom.active() : df::zoom_pane::primary;
	}

	void active_zoom_pane(const df::zoom_pane pane)
	{
		if (_is_two && _comparison_zoom.active() != pane)
		{
			// The pane you switch to always adopts the scale and center you were just looking at.
			_comparison_zoom.active(pane);
			mark_zoom_activity();
			_async.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw |
				view_invalid::command_state | view_invalid::controller);
		}
	}

	void flip_zoom_pane()
	{
		if (!_is_two) return;
		active_zoom_pane(df::comparison_zoom_state::other(_comparison_zoom.active()));
	}

	void active_zoom_pane_at(const pointd location)
	{
		// Magnified there is no left or right to hit test, so the pointer never chooses the pane.
		if (!_is_two || is_zoom_mode()) return;
		for (const auto pane : {df::zoom_pane::primary, df::zoom_pane::secondary})
		{
			const auto& layout = _zoom_layouts[zoom_pane_index(pane)];
			const rectd pane_bounds(layout.viewport_origin, layout.viewport_extent);
			if (pane_bounds.contains(location))
			{
				active_zoom_pane(pane);
				return;
			}
		}
	}

	const df::zoom_view_state& current_zoom_state() const noexcept
	{
		return _is_two ? _comparison_zoom.active_state() : _common._zoom;
	}

	zoom_layout_state& current_zoom_layout() noexcept
	{
		return _zoom_layouts[zoom_pane_index(active_zoom_pane())];
	}

	const zoom_layout_state& current_zoom_layout() const noexcept
	{
		return _zoom_layouts[zoom_pane_index(active_zoom_pane())];
	}

	pointd zoom_anchor_at(const pointd location) const noexcept
	{
		return location - current_zoom_layout().viewport_origin;
	}

	template <class Mutator>
	void mutate_zoom(Mutator&& mutator)
	{
		if (_is_two) _comparison_zoom.mutate(std::forward<Mutator>(mutator));
		else mutator(_common._zoom);
	}

	bool zoom() const
	{
		return _can_zoom && current_zoom_state().is_magnified(zoom_fit_scale());
	}

	bool is_zoom_mode() const
	{
		return _can_zoom && (_temporary_zoom || !current_zoom_state().is_fit());
	}

	uint64_t zoom_activity_generation() const noexcept
	{
		return _zoom_activity_generation;
	}

	void mark_zoom_activity() noexcept
	{
		++_zoom_activity_generation;
		_zoom_activity_time = df::now();
	}

	bool zoom_interactive(const double time_now) const noexcept
	{
		return time_now - _zoom_activity_time < 0.15;
	}

	void zoom(const bool zoom)
	{
		const auto state_changes = zoom ? !this->zoom() : !current_zoom_state().is_fit();
		if (state_changes)
		{
			stop_slideshow();

			if (zoom && panorama_projects())
			{
				enter_panorama_projection();
				return;
			}

			mutate_zoom([zoom](df::zoom_view_state& state)
			{
				if (zoom) state.set_explicit(1.0);
				else state.fit();
			});
			mark_zoom_activity();
			_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::tooltip |
				view_invalid::controller);
		}
	}

	int zoom_scale_percent() const
	{
		return df::round(current_zoom_state().effective_scale(zoom_fit_scale()) * 100.0);
	}

	void adjust_zoom_scale(const int direction, const pointd anchor)
	{
		if (direction == 0) return;

		if (is_panorama_projected())
		{
			panorama_step_fov(direction);
			return;
		}

		if (direction > 0 && panorama_projects() && current_zoom_state().is_fit())
		{
			enter_panorama_projection();
			return;
		}

		if (direction > 0 && panorama_enters_at_100())
		{
			zoom_100(anchor);
			return;
		}

		auto& layout = current_zoom_layout();
		const auto source_anchor = current_zoom_state().source_point_at(
			layout.source_extent, layout.viewport_extent, zoom_fit_scale(), anchor);
		mutate_zoom([&](df::zoom_view_state& state)
		{
			state.step(direction, zoom_fit_scale(), layout.source_extent, layout.viewport_extent, anchor);
		});
		mark_zoom_activity();
		if (!current_zoom_state().is_fit())
		{
			layout.pending_source_anchor = source_anchor;
			layout.pending_client_anchor = layout.viewport_origin + anchor;
			layout.pending_reanchor = true;
		}
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	void adjust_zoom_scale(const int direction)
	{
		if (direction == 0) return;
		const auto& layout = current_zoom_layout();
		const pointd anchor{layout.viewport_extent.Width / 2.0, layout.viewport_extent.Height / 2.0};

		if (is_panorama_projected())
		{
			panorama_step_fov(direction);
			return;
		}

		if (direction > 0 && panorama_projects() && current_zoom_state().is_fit())
		{
			enter_panorama_projection();
			return;
		}

		if (direction > 0 && panorama_enters_at_100())
		{
			zoom_100(anchor);
			return;
		}

		mutate_zoom([&](df::zoom_view_state& state)
		{
			state.step(direction, zoom_fit_scale(), layout.source_extent, layout.viewport_extent, anchor);
		});
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	void zoom_100()
	{
		const auto& layout = current_zoom_layout();
		zoom_100({layout.viewport_extent.Width / 2.0, layout.viewport_extent.Height / 2.0});
	}

	void zoom_100(const pointd anchor)
	{
		// zoom.md: 100% means one source pixel per device pixel, and a sphere has no such pixel. Asking
		// for actual size is asking for what the file stores, so it leaves the projection - visibly,
		// because the zoom chrome's control says which of the two is in force.
		if (declares_equirectangular()) _common._panorama.flat = true;

		auto& layout = current_zoom_layout();
		const auto source_anchor = current_zoom_state().source_point_at(
			layout.source_extent, layout.viewport_extent, zoom_fit_scale(), anchor);
		const auto old_scale = current_zoom_state().effective_scale(zoom_fit_scale());
		mutate_zoom([&](df::zoom_view_state& state)
		{
			state.set_anchored(1.0, old_scale, layout.source_extent, layout.viewport_extent, anchor);
		});
		mark_zoom_activity();
		layout.pending_source_anchor = source_anchor;
		layout.pending_client_anchor = layout.viewport_origin + anchor;
		layout.pending_reanchor = true;
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	void zoom_fit_variant(const df::zoom_scale_mode mode)
	{
		const auto& layout = current_zoom_layout();
		mutate_zoom([&](df::zoom_view_state& state)
		{
			if (mode == df::zoom_scale_mode::fit_width)
				state.fit_width(layout.source_extent, layout.viewport_extent);
			else if (mode == df::zoom_scale_mode::fill)
				state.fill(layout.source_extent, layout.viewport_extent);
			else
				state.fit();
		});
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	void zoom_scale(const double scale)
	{
		const auto& layout = current_zoom_layout();
		const pointd anchor{layout.viewport_extent.Width / 2.0, layout.viewport_extent.Height / 2.0};
		const auto old_scale = current_zoom_state().effective_scale(zoom_fit_scale());
		mutate_zoom([&](df::zoom_view_state& state)
		{
			state.set_anchored(scale, old_scale, layout.source_extent, layout.viewport_extent, anchor);
		});
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	double zoom_fit_scale() const
	{
		const auto& layout = current_zoom_layout();
		return df::zoom_view_state::fit_scale(layout.source_extent, layout.viewport_extent, setting.scale_up);
	}

	double zoom_fit_scale(const df::zoom_pane pane) const
	{
		const auto& layout = _zoom_layouts[zoom_pane_index(pane)];
		return df::zoom_view_state::fit_scale(layout.source_extent, layout.viewport_extent, setting.scale_up);
	}

	void zoom_layout(const sized source, const sized viewport, const pointd viewport_origin,
	                 const df::zoom_pane pane = df::zoom_pane::primary)
	{
		auto& layout = _zoom_layouts[zoom_pane_index(pane)];
		layout.source_extent = source;
		layout.viewport_extent = viewport;
		layout.viewport_origin = viewport_origin;
		auto& state = _is_two ? _comparison_zoom.state(pane) : _common._zoom;
		state.update_source(source, zoom_fit_scale(pane));
		state.update_fit_variant(source, viewport, setting.scale_up);
		if (layout.pending_reanchor)
		{
			state.center_source_point_at(layout.pending_source_anchor, source, viewport, zoom_fit_scale(pane),
			                             layout.pending_client_anchor - viewport_origin);
			layout.pending_reanchor = false;
		}
	}

	const df::zoom_view_state& zoom_state() const noexcept
	{
		return current_zoom_state();
	}

	const df::zoom_view_state& zoom_state(const df::zoom_pane pane) const noexcept
	{
		return _is_two ? _comparison_zoom.state(pane) : _common._zoom;
	}

	void restore_zoom_state(const df::zoom_view_state& state)
	{
		mutate_zoom([&](df::zoom_view_state& current) { current = state; });
		_temporary_zoom = false;
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	void inspect_at_100(const pointd anchor)
	{
		// zoom.md: one mechanism, two durations. A declared sphere is a sphere under a held button too,
		// and the pointer aims it from the moment of the press.
		if (panorama_projects())
		{
			enter_panorama_projection(true);
			panorama_look_at(anchor);
			return;
		}

		auto& layout = current_zoom_layout();
		const auto source_anchor = current_zoom_state().source_point_at(
			layout.source_extent, layout.viewport_extent, zoom_fit_scale(), anchor);
		const auto old_scale = current_zoom_state().effective_scale(zoom_fit_scale());
		mutate_zoom([&](df::zoom_view_state& state)
		{
			state.set_anchored(1.0, old_scale, layout.source_extent, layout.viewport_extent, anchor);
		});
		mark_zoom_activity();
		layout.pending_source_anchor = source_anchor;
		layout.pending_client_anchor = layout.viewport_origin + anchor;
		layout.pending_reanchor = true;
		_temporary_zoom = true;
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	void commit_inspect()
	{
		if (_temporary_zoom)
		{
			_temporary_zoom = false;
			mark_zoom_activity();
			_async.invalidate_view(view_invalid::view_redraw | view_invalid::controller);
		}
	}

	bool is_temporary_zoom() const noexcept
	{
		return _temporary_zoom;
	}

	void zoom_center(const pointd center)
	{
		const auto scale = current_zoom_state().effective_scale(zoom_fit_scale());
		mutate_zoom([&](df::zoom_view_state& state) { state.set_explicit(scale, center); });
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
	}

	// Moving the centre only. No button was pressed, so the sweep must not decide the scale for the
	// user and lose the fit variant they chose. This runs on plain pointer movement, so it asks for a
	// redraw and not a layout: nothing set_center touches changes a measured extent, and re-measuring
	// the media column on every sample of a sweep is a cost paid for nothing.
	void look_around_center(const pointd center)
	{
		mutate_zoom([&](df::zoom_view_state& state) { state.set_center(center); });
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_redraw);
	}

	// zoom.md: the display's own question, wider than what the file declares. The shape fallback
	// lives on the metadata record beside the declaration.
	bool displays_as_panorama() const
	{
		if (!_is_one || !_item1) return false;
		const auto md = _item1->metadata();
		return md && md->displays_as_panorama();
	}

	// zoom.md: the file declares a sphere, so the viewer can put the user inside it. Only
	// equirectangular: cylindrical names no vertical mapping the file agrees on, and `unspecified`
	// names no projection at all, so both keep the flat treatment.
	bool declares_equirectangular() const
	{
		if (!_is_one || !_item1) return false;
		const auto md = _item1->metadata();
		// Dimensions are what the coverage is derived from, so an item the scan has not measured yet
		// cannot be stood inside - the sidecar merge sets the declaration without ever seeing a pixel.
		return md && md->panorama == prop::panorama_projection::equirectangular && !md->dimensions().is_empty();
	}

	// Whether the next magnified draw is a projection. The flat override is per item and visible in
	// the zoom chrome, so this is never a hidden mode.
	bool panorama_projects() const
	{
		if (!declares_equirectangular() || _common._panorama.flat) return false;

		// The sphere is described in the file's stored pixel grid, so a file that also asks to be
		// rotated for display would be sampled in a space its own metadata does not describe. Shown
		// flat rather than shown wrong; vanishingly rare, because a stitcher writes the panorama the
		// way up it means it.
		return !setting.show_rotated || !_selected_texture1 ||
			_selected_texture1->display_orientation() == ui::orientation::top_left;
	}

	// Whether the next magnified draw is a projection. Inspect zoom is included: zoom.md makes it and
	// zoom mode two durations of one mechanism sharing one renderer, so press-and-hold on a declared
	// sphere shows the sphere. The flat override is per item and visible in the zoom chrome, so this
	// is never a hidden mode.
	bool is_panorama_projected() const
	{
		return is_zoom_mode() && panorama_projects();
	}

	const panorama_session& panorama() const noexcept
	{
		return _common._panorama;
	}

	panorama_request panorama_draw_request() const
	{
		if (!is_panorama_projected()) return {};
		return {true, _common._panorama.geometry, _common._panorama.view};
	}

	// Keyed on the path so a second panorama opens at its own centre. Declaration currency also
	// includes source dimensions: replacing a file at the same path must reread its crop geometry,
	// while ordinary repopulation must not throw away where the user is looking or the flat/projected
	// choice they made.
	void panorama_item(const df::file_path path, const sizei source, const df::date_t modified)
	{
		auto& session = _common._panorama;

		if (session.path == path && session.source == source && session.modified == modified) return;

		const auto same_path = session.path == path;

		session.path = path;
		session.source = source;
		session.modified = modified;
		session.geometry = prop::panorama_geometry::assumed(source);
		session.resolved = false;
		session.reset_view_on_resolve = !same_path;
		if (!same_path)
		{
			session.flat = false;
			session.view.reset(session.geometry);
		}
	}

	// The declared coverage, once the file has answered. Applied only while the item and source size
	// it was read for are still on screen. Declaration currency is separate from the user's camera
	// and flat/projected choice, so a same-path declaration refresh does not reset either.
	void panorama_geometry(const df::file_path path, const prop::panorama_geometry& declared, const sizei source,
	                       const df::date_t modified)
	{
		auto& session = _common._panorama;

		if (session.path != path || session.source != source || session.modified != modified || session.resolved) return;

		const auto resolved = prop::panorama_geometry::resolve(declared, source);

		if (!resolved.is_valid()) return;

		session.geometry = resolved;
		session.resolved = true;
		if (session.reset_view_on_resolve)
		{
			session.view.reset(session.geometry);
			session.reset_view_on_resolve = false;
		}
		_async.invalidate_view(view_invalid::view_redraw);
	}

	// zoom.md L6: the projection is a state the user can change, so it has a control that shows
	// which one is on. Leaving the projection lands on the flat picture at 100%, which is the
	// answer someone asking for the pixels wanted.
	void toggle_panorama_projection()
	{
		if (!declares_equirectangular()) return;

		_common._panorama.flat = !_common._panorama.flat;

		if (_common._panorama.flat) zoom_100();
		else _common._panorama.view.reset(_common._panorama.geometry);

		mark_zoom_activity();
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	// A drag turns the world under the pointer. The whole gesture is recomputed from its origin
	// against the view it started from, for the same reason panning is: coalesced or dropped move
	// messages must not change where the drag ends up.
	void panorama_drag(const pointd client_delta, const panorama_view& start)
	{
		const auto& layout = current_zoom_layout();
		_common._panorama.view.drag(client_delta, layout.viewport_extent, start);
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_redraw);
	}

	// An interrupted look puts the camera back where the gesture found it, the same undo a cancelled
	// pan gets.
	void restore_panorama_view(const panorama_view& view)
	{
		_common._panorama.view = view;
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_redraw);
	}

	// The field-of-view ladder replaces the zoom ladder while projected. Stepping out from the widest
	// stop leaves magnification entirely, which is the same shape as zoom.md L2 stepping out to Fit.
	void panorama_step_fov(const int direction)
	{
		if (direction == 0) return;

		if (!_common._panorama.view.step_fov(direction))
		{
			zoom(false);
			return;
		}

		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_redraw | view_invalid::controller);
	}

	// Entering a projection is entering zoom mode: the scale it carries is what makes the mode
	// active and what the flat picture returns to, and it is never what the projected draw uses.
	void enter_panorama_projection(const bool temporary = false)
	{
		const auto fit = zoom_fit_scale();
		const auto scale = fit < 1.0 ? 1.0 : fit * 1.5;
		mutate_zoom([scale](df::zoom_view_state& state) { state.set_explicit(scale); });
		_common._panorama.view.reset(_common._panorama.geometry);
		_temporary_zoom = temporary;
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	// Where a held pointer is aiming. Positional, like the flat inspect traversal it replaces: the
	// viewport maps the whole file, so crossing it looks all the way round.
	void panorama_look_at(const pointd local)
	{
		const auto& layout = current_zoom_layout();
		_common._panorama.view.aim(local, layout.viewport_extent, _common._panorama.geometry);
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_redraw);
	}

	// zoom.md: look around a flat wide picture. No button is held, so press-and-hold inspect zoom is
	// untouched. A projected panorama is excluded: it is looked around by dragging, and following a
	// resting pointer as well would give one picture two answers to the same movement.
	bool is_looking_around() const
	{
		return is_zoom_mode() && !_temporary_zoom && displays_as_panorama() && !panorama_projects();
	}

	// zoom.md: a 12000x1000 stitch fitted to a browsing window is a bright line, so the ladder's first
	// stop above fit is still nothing to read. Entering zoom on a panorama goes straight to 100%,
	// where the strip is legible and the pointer can sweep it. Only when 100% is a step up: a small
	// declared panorama scaled up to fill the window is already past it, and the shortcut would answer
	// a zoom in by zooming out.
	bool panorama_enters_at_100() const
	{
		return current_zoom_state().is_fit() && displays_as_panorama() && zoom_fit_scale() < 1.0;
	}

	// The pointer maps onto the source-space centre, so crossing the viewport sweeps the whole
	// width. The vertical axis is pinned unless the picture is taller than the viewport at this
	// scale: without something to see up there, following the pointer is only jitter.
	void look_around_at(const pointd local)
	{
		const auto& layout = current_zoom_layout();
		const auto viewport = layout.viewport_extent;
		const auto source = layout.source_extent;
		if (viewport.Width <= 0.0 || viewport.Height <= 0.0 || source.Width <= 0.0 || source.Height <= 0.0) return;

		const auto scale = current_zoom_state().effective_scale(zoom_fit_scale());
		const auto center = df::zoom_view_state::look_around_center(local, source, viewport, scale);

		if (current_zoom_state().center() == center) return;

		look_around_center(center);
	}

	void pan_zoom(const pointd client_delta, const df::zoom_view_state& start)
	{
		const auto& layout = current_zoom_layout();
		mutate_zoom([&](df::zoom_view_state& state)
		{
			state = start;
			const auto scale = state.effective_scale(zoom_fit_scale());
			state.pan_source({-client_delta.X / scale, -client_delta.Y / scale}, layout.source_extent,
			                 layout.viewport_extent, zoom_fit_scale());
		});
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
	}

	void pan_zoom_by(const pointd client_delta)
	{
		const auto& layout = current_zoom_layout();
		mutate_zoom([&](df::zoom_view_state& state)
		{
			const auto scale = state.effective_scale(zoom_fit_scale());
			state.pan_source({client_delta.X / scale, client_delta.Y / scale}, layout.source_extent,
			                 layout.viewport_extent, zoom_fit_scale());
		});
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
	}

	void pan_zoom_to_horizontal_edge(const bool last)
	{
		auto& layout = current_zoom_layout();
		mutate_zoom([&](df::zoom_view_state& state)
		{
			state.pan_source({last ? layout.source_extent.Width : -layout.source_extent.Width, 0.0},
			                 layout.source_extent, layout.viewport_extent, zoom_fit_scale());
		});
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
	}

	void zoom_region(const rectd& region)
	{
		const auto& layout = current_zoom_layout();
		mutate_zoom([&](df::zoom_view_state& state)
		{
			state.zoom_region(region, layout.source_extent, layout.viewport_extent, zoom_fit_scale());
		});
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	bool can_zoom() const
	{
		return _can_zoom;
	}

	void toggle_zoom()
	{
		if (current_zoom_state().is_fit()) zoom(true);
		else zoom(false);
	}

	void toggle_zoom_fit()
	{
		mutate_zoom([](df::zoom_view_state& state) { state.toggle_fit(); });
		mark_zoom_activity();
		_async.invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw |
			view_invalid::controller);
	}

	bool player_has_video() const
	{
		return _player_media_info.has_video;
	}

	bool can_play_media() const
	{
		return (_player_media_info.has_video || _player_media_info.has_audio) && _session;
	}

	bool is_playing_media() const
	{
		return (_player_media_info.has_video || _player_media_info.has_audio) && _session && _session->is_playing();
	}

	bool is_playing() const
	{
		return _common._is_slideshow || is_playing_media();
	}

	bool is_slideshow() const
	{
		return _common._is_slideshow;
	}

	bool is_playing_slideshow() const
	{
		return _common._is_slideshow && (!_session || !_session->is_playing());
	}

	void stop_slideshow()
	{
		if (_common._is_slideshow)
		{
			_common._is_slideshow = false;
			_next_photo_tick = 0;
			// command_state too: the slideshow toggle and its related commands read this flag.
			// screen_saver because a photo slideshow is the only thing holding sleep off.
			_async.invalidate_view(view_invalid::view_redraw | view_invalid::command_state |
				view_invalid::screen_saver);
		}
	}

	bool display_item_has_trait(const file_traits t) const
	{
		return _is_one && _item1 && _item1->file_type()->has_trait(t);
	}

	int slideshow_pos() const
	{
		return df::mul_div(_next_photo_tick, 1000,
		                   std::max(1, setting.slideshow_delay) * ui::default_ticks_per_second);
	}

	void update_scrubber()
	{
		const auto& info = _player_media_info;

		if (info.has_video || info.has_audio)
		{
			auto invalid = false;
			const auto start = info.start;
			const auto end = info.end;
			const auto len = end - start;
			const auto media_pos = _session ? _session->last_frame_time() : 0.0;
			const auto pos = static_cast<int>((media_pos - start) * _scrubber_width / std::max(1.0, len));
			const auto endi = df::round(end);

			if (endi != _last_duration)
			{
				_duration = str::format_seconds(endi);
			}

			if (pos != _last_scrubber_pos)
			{
				_last_scrubber_pos = pos;
				invalid = true;
			}

			const auto position = df::round(media_pos);

			if (_last_seconds != position)
			{
				_last_seconds = position;
				_time = str::format_seconds(position);
				invalid = true;
			}

			if (invalid)
			{
				_async.invalidate_view(view_invalid::view_redraw);
			}
		}
	}

	void load_compare_preview(int elapsed_numerator, int elapsed_denominator);
	void load_seek_preview(int pos_numerator, int pos_denominator, std::function<void()> callback);

	void preview_loaded()
	{
		update_scrubber();
		_async.invalidate_view(view_invalid::view_redraw | view_invalid::tooltip);
	}


	bool is_multi() const
	{
		return _is_multi;
	}

	bool step()
	{
		bool invalid = false;
		invalid |= _loading_alpha_animation.step();
		invalid |= _selected_texture1 && _selected_texture1->step();
		invalid |= _selected_texture2 && _selected_texture2->step();
		return invalid;
	}

	void update_av_session(const std::shared_ptr<av_session>& ses);

	// Reads the head of the display item into _selected_item_data for the hex view.
	void load_selected_item_data();
	// False when a teardown or a newer open has taken the display since; the caller then owns closing
	// the session it was handed.
	bool publish_av_session(const std::shared_ptr<av_session>& ses, uint32_t generation);
	// A player session only reads the container, so a sidecar packet is fetched separately.
	void load_xmp_sidecar();
	void calc_pixel_difference();
};
