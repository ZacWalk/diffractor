// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Photo editing view. Implements pixel adjustments and comparison.

#pragma once

#include "model.h"
#include "ui_view.h"
#include "ui_controllers.h"
#include "ui_dialog.h"
#include "view_list.h"

class log_slider_control;
class edit_view;
class task_toolbar_control;
class rating_bar_control;

struct document_detection_result
{
	std::array<pointd, 4> corners{};
	sizei extent;
	double confidence = 0;

	explicit operator bool() const { return extent.cx > 0 && extent.cy > 0; }
};

// The straighten, perspective and crop that present a detected page upright. Expressed in the same
// controls the user already has, so the image morphs and the crop stays a rectangle they can adjust.
struct document_correction
{
	double straighten = 0;
	double perspective_horizontal = 0;
	double perspective_vertical = 0;
	quadd crop;

	explicit operator bool() const { return !crop.is_empty(); }
};

document_detection_result detect_document(const ui::const_surface_ptr& surface, sizei source_extent);
document_correction fit_document_correction(const std::array<pointd, 4>& corners, sizei extent,
                                            ui::orientation orientation);
bool edit_load_has_source_pixels(const file_load_result& loaded);
std::string_view edit_load_status_text(const file_load_result& loaded, bool source_loading);
bool should_accept_edit_async_result(size_t current_display_generation, size_t result_display_generation);
bool should_accept_edit_preview_result(size_t current_display_generation, size_t result_display_generation,
                                       size_t current_preview_generation, size_t result_preview_generation,
                                       sizei current_request_dimensions, sizei result_dimensions);
bool should_accept_edit_analysis(size_t current_display_generation, size_t result_display_generation,
                                 size_t current_edit_generation, size_t result_edit_generation);
// Whether the preview in hand answers the size the view now wants. The decode treats its request as
// a ceiling - it can land a pixel short on one axis, or at the native size of a picture smaller than
// the view - so a preview is judged by the request it answered, never by its own extent.
bool edit_preview_is_current(const ui::const_surface_ptr& preview, sizei answered_request, sizei wanted);
// Loading a JPEG, PNG or WebP reads only its header, so the preview is the first full decode. One that
// produced nothing is named as a failed load would be: too large when the decode budget refused it.
file_load_result::failure edit_preview_failure(const ui::const_surface_ptr& preview, const file_load_result& loaded,
                                               sizei request);
// The load state once the first preview has failed: no pixels to edit, and the reason the status names.
file_load_result edit_load_without_preview(const file_load_result& loaded, file_load_result::failure failure);
std::optional<quadd> edit_pending_crop_for_loaded_photo(std::optional<rectd> pending_crop,
                                                        const file_load_result& loaded, bool show_rotated);
bool edit_pixel_controls_visible(bool is_bitmap, bool can_edit_pixels);

struct edit_preview_render_decision
{
	bool draw_texture = false;
	bool show_loading = false;
	bool crop_interactive = false;
};

edit_preview_render_decision decide_edit_preview_render(bool has_texture, bool has_current_preview,
                                                        bool has_loading_status);

struct edit_request_coalescing_decision
{
	bool start_now = false;
	bool remember_latest = false;
};

edit_request_coalescing_decision decide_edit_request_coalescing(bool request_in_flight);
bool should_start_edit_follow_up_request(bool has_latest_request);

class edit_view_controls final : public view_controls_host
{
public:
	edit_view_state& _edit_state;
	std::shared_ptr<edit_view> _view;

	std::shared_ptr<text_element> _info;
	std::shared_ptr<ui::title_control> _straighten_title;
	std::shared_ptr<log_slider_control> _straighten_slider;
	std::shared_ptr<log_slider_control> _perspective_horizontal_slider;
	std::shared_ptr<log_slider_control> _perspective_vertical_slider;
	std::shared_ptr<task_toolbar_control> _rotate_toolbar;
	std::shared_ptr<divider_element> _color_divider;
	std::shared_ptr<ui::title_control> _color_title;
	std::shared_ptr<log_slider_control> _vibrance_slider;
	std::shared_ptr<log_slider_control> _darks_slider;
	std::shared_ptr<log_slider_control> _midtones_slider;
	std::shared_ptr<log_slider_control> _lights_slider;
	std::shared_ptr<log_slider_control> _contrast_slider;
	std::shared_ptr<log_slider_control> _brightness_slider;
	std::shared_ptr<log_slider_control> _saturation_slider;
	std::shared_ptr<log_slider_control> _temperature_slider;
	std::shared_ptr<log_slider_control> _tint_slider;
	std::shared_ptr<task_toolbar_control> _color_toolbar;
	std::shared_ptr<divider_element> _save_divider;
	std::shared_ptr<ui::title_control> _save_title;
	std::shared_ptr<ui::check_control> _backup_check;
	std::shared_ptr<ui::slider_control> _jpeg_quality_slider;
	std::shared_ptr<ui::slider_control> _webp_quality_slider;
	std::shared_ptr<ui::check_control> _webp_lossless_check;

	edit_view_controls(view_state& s, edit_view_state& es) : view_controls_host(s), _edit_state(es)
	{
		_scroller._scroll_child_controls = true;
	}

	void layout_controls(ui::measure_context& mc) override;
	void create_controls();
	bool is_tracking() const;

	void options_changed() override;
};

class edit_view final : public view_base, public std::enable_shared_from_this<edit_view>
{
	using this_type = edit_view;

	struct source_load_request
	{
		df::file_path path;
		size_t display_generation = 0;
		std::optional<rectd> pending_crop;
	};

	struct preview_decode_request
	{
		file_load_result loaded;
		sizei dimensions;
		size_t display_generation = 0;
		size_t preview_generation = 0;
	};

	view_state& _state;
	view_host_ptr _host;

	edit_view_state& _edit_state;
	std::shared_ptr<edit_view_controls> _edit_controls;

	sizei _extent;
	affined _image_transform;

	rectd _crop_bounds;
	rectd _crop_handle_tl;
	rectd _crop_handle_tr;
	rectd _crop_handle_bl;
	rectd _crop_handle_br;

	std::string _title;
	df::file_path _path;
	std::string_view _xmp_name;
	file_type_ref _mt = nullptr;
	file_load_result _loaded;
	ui::const_surface_ptr _preview_source;
	// The request _preview_source answered; meaningful only while it is set.
	sizei _preview_source_request;
	ui::const_surface_ptr _dialog_preview_source;
	// Shed on device loss from the const broadcast; render rebuilds whenever it is absent, so
	// clearing it is the whole recovery.
	mutable ui::texture_ptr _texture;
	display_state_ptr _media_display;
	view_element_ptr _media_element;
	view_element_ptr _play_element;
	view_element_ptr _scrubber_element;
	size_t _display_generation = 0;
	size_t _preview_generation = 0;
	sizei _preview_request_dimensions;
	// A size whose decode produced nothing while an earlier size stays on screen; not asked for again.
	sizei _preview_failed_request;
	bool _source_load_in_flight = false;
	std::optional<source_load_request> _pending_source_load_request;
	bool _preview_decode_in_flight = false;
	std::optional<preview_decode_request> _pending_preview_decode_request;
	// Bumped by changed(), which every edit routes through. A background analysis snapshots it and
	// retires if anything has been adjusted since, so its answer cannot land on newer work.
	size_t _edit_generation = 0;
	bool _source_loading = false;
	bool _invalid = true;

	friend class selection_move_controller<this_type>;
	friend class handle_move_controller<this_type>;

public:
	edit_view(view_state& s, view_host_ptr host, edit_view_state& evs);
	recti calc_media_bounds() const
	{
		return {0, 0, _extent.cx, _extent.cy};
	}

	view_controls_host_ptr controls(const ui::control_frame_ptr& owner);
	static ui::const_surface_ptr build_preview_surface(const ui::const_surface_ptr& source, sizei loaded_dimensions,
	                                                   const image_edits& edits);
	ui::const_surface_ptr preview_surface() const;

	void activate(sizei extent) override;
	void deactivate() override;
	void refresh() override;
	void update_media_elements() override;

	void layout(ui::measure_context& mc, sizei extent) override;
	bool is_photo() const;
	bool can_edit_pixels() const;
	void queue_source_load(source_load_request request);
	void start_source_load(source_load_request request);
	void complete_source_load(const source_load_request& request, file_load_result loaded);
	void queue_preview_decode();
	void start_preview_decode(preview_decode_request request);
	void complete_preview_decode(const preview_decode_request& request, ui::const_surface_ptr source,
	                             file_load_result::failure failure);
	void clear_crop_interaction_bounds();
	void draw_loading_status(ui::draw_context& dc, std::string_view status) const;
	void changed();
	void cancel() const;
	void exit() override;
	bool escape() override;
	void save_current();
	void save(df::file_path src_path, df::file_path dst_path, std::string_view xmp_name,
	          const ui::control_frame_ptr& owner, std::function<void(bool)> complete) const;
	bool has_changes() const;
	void select_item(const df::item_element_ptr& item);
	void render(ui::draw_context& dc, view_controller_ptr controller) override;
	bool can_exit() override;
	void display_changed() override;

	void broadcast_event(const view_element_event& event) const override
	{
		if (event.type == view_element_event_type::free_graphics_resources) _texture.reset();
		if (_media_element) _media_element->dispatch_event(event);
		if (_play_element) _play_element->dispatch_event(event);
		if (_scrubber_element) _scrubber_element->dispatch_event(event);
	}

	static void draw_handle(ui::draw_context& dc, recti handle_bounds2, float alpha);

	bool check_path(df::file_path& path, const ui::control_frame_ptr& owner) const;
	df::item_element_ptr next_editable_item(bool forward) const;
	void save_and_next(bool forward);
	void save_as();

	void rotate_anticlockwise();
	void rotate_clockwise();
	void rotate_reset();
	void color_reset();
	void report_no_result(std::string_view title) const;
	void queue_auto_adjust(int max_dimension, std::string title,
	                       std::function<std::function<void(edit_view_state&)>(const ui::const_surface_ptr&)> analyze);
	void auto_color();
	void auto_straighten();
	void auto_document();
	void toggle_preview();

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, hit_test_context& ctx) override;

	void device_selection2(const rectd& sel_bounds_in, const int active_point)
	{
		const auto dims = _loaded.dimensions();
		auto sel = quadd(sel_bounds_in).transform(_image_transform.invert());
		sel = sel.crop(rectd(0, 0, dims.cx, dims.cy), active_point);
		selection(sel);
	}

	void device_selection(const rectd& sel_bounds_in, const bool crop, const bool limit)
	{
		const auto dims = _loaded.dimensions();
		const auto limit_bounds = rectd(0, 0, dims.cx, dims.cy);
		auto sel = quadd(sel_bounds_in).transform(_image_transform.invert());
		if (crop) sel = sel.crop(limit_bounds);
		if (limit) sel = sel.limit(limit_bounds);
		selection(sel);
	}

	quadd selection() const
	{
		return _edit_state._edits.effective_crop_bounds(_loaded.dimensions());
	}

	void selection(const quadd& s)
	{
		_edit_state.selection(s);
		_state.invalidate_view(view_invalid::view_redraw);
		changed();
	}

	rectd device_selection() const
	{
		const auto dims = _loaded.dimensions();
		const auto crop = _edit_state.selection();
		const auto draw_crop = crop.crop(rectd(0, 0, dims.cx, dims.cy));
		return draw_crop.transform(_image_transform).bounding_rect();
	}

	std::string_view title() override
	{
		const auto i = _state._edit_item;

		if (i)
		{
			_title = std::format("{}: {}", s_app_name, tt.editing_title);
		}
		else
		{
			_title = s_app_name;
		}

		return _title;
	}

	void options_changed() const
	{
		if (_edit_controls)
		{
			_edit_controls->options_changed();
		}
	}
};
