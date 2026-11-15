// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Edit view. Implements a photo's pixel adjustments and comparison, and the lossless trim, sound
// removal and turn of a video or a sound file, copied in its own format or as an MP4.

#pragma once

#include "av_format.h"
#include "model.h"
#include "ui_view.h"
#include "ui_controllers.h"
#include "ui_dialog.h"
#include "view_list.h"

class log_slider_control;
class edit_view;
class edit_histogram_control;
class edit_compare_controller;
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

document_detection_result detect_document(const core::const_surface_ptr& surface, sizei source_extent);
document_correction fit_document_correction(const std::array<pointd, 4>& corners, sizei extent,
                                            core::orientation orientation);

// What Auto colour sets the controls to: the tone from the picture's luma histogram, and the white balance
// that brings its near-neutral pixels to grey.
struct auto_color_settings
{
	double brightness = 0;
	double contrast = 0;
	double darks = 0;
	double lights = 0;
	double midtones = 0;
	double vibrance = 0;
	double temperature = 0;
	double tint = 0;
};

// Nothing when the picture has no opaque pixel to measure.
std::optional<auto_color_settings> analyze_auto_color(const core::const_surface_ptr& surface);
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
bool edit_preview_is_current(const core::const_surface_ptr& preview, sizei answered_request, sizei wanted);
// Loading a JPEG, PNG or WebP reads only its header, so the preview is the first full decode. One that
// produced nothing is named as a failed load would be: too large when the decode budget refused it.
file_load_result::failure edit_preview_failure(const core::const_surface_ptr& preview, const file_load_result& loaded,
                                               sizei request);
// The load state once the first preview has failed: no pixels to edit, and the reason the status names.
file_load_result edit_load_without_preview(const file_load_result& loaded, file_load_result::failure failure);
std::optional<quadd> edit_pending_crop_for_loaded_photo(std::optional<rectd> pending_crop,
                                                        const file_load_result& loaded, bool show_rotated);
bool edit_pixel_controls_visible(bool is_bitmap, bool can_edit_pixels);
// An adjustment as its entry box shows it, to two places. A quarter turn negates an unset perspective,
// so a value that rounds to zero is shown unsigned rather than as a -0.00 the user never set.
std::string format_edit_adjustment(double value);
// Where an edited photo is offered to be saved beside its source: "<name>-edit", numbered past names
// taken, in the source's format when Diffractor can write it and as JPEG otherwise - or as AVIF, where `as_avif` says
// a JPEG would lose what the edit holds - never the name of a file already there, such as the camera's own JPEG beside
// a RAW. A video or a sound file is copied in its own format where Edit writes it, and as MP4 where Edit does not.
core::file_path edit_save_suggestion(core::file_path source, const std::function<bool(core::file_path)>& exists,
                                     bool as_avif = false);
// Whether an edited copy of a photo Diffractor does not write is offered as an AVIF rather than a JPEG: a HEIF whose
// edit holds what a JPEG would lose - more than eight bits a channel, an HDR signal, or a gain map.
bool edit_offers_avif(core::file_path source, const file_load_result& loaded);
// What a photo saved as `destination` would lose of its range, said as the save asks whether to go on: its HDR, in a
// format that holds none, or its gain map, in one that leaves it out. Empty where the format keeps it, and for a format
// Diffractor does not encode, which is only turned in place or copied whole. `repacks`: a JPEG goes into a JPEG XL
// whole, gain map and all.
std::string edit_save_loss(const picture_signal& signal, core::file_path destination, bool repacks);
// What the Edit panel says of a photo's depth and range: that it is HDR, and edited so; that it is too large to edit
// in HDR; or that it is too large to edit at its full depth. Empty for a photo edited as it is.
std::string_view edit_photo_note(const picture_signal& signal);

// How many of a picture's pixels fall at each level of red, green and blue: the Edit panel's histogram of a photo as
// its colour is edited.
struct edit_histogram
{
	static constexpr int levels = 64;
	std::array<std::array<uint32_t, levels>, 3> counts{};
	uint32_t samples = 0;

	// The fullest level of any channel short of black and white, where clipped pixels pile up: what the panel scales
	// the histogram to.
	uint32_t peak() const;
};

// Red, green and blue from 0 to 1 of about `most` of a packed or sixteen-bit picture's pixels, spread over all of it
// and leaving out any wholly transparent: what the histogram counts, graded again as the colour changes.
std::vector<std::array<float, 3>> edit_histogram_samples(const core::const_surface_ptr& surface, int most = 16384);
// The samples counted as `grade` - the colour the preview is drawn with - leaves them, or as they are without one.
edit_histogram count_edit_histogram(std::span<const std::array<float, 3>> samples,
                                    const render::texture_transform* grade = nullptr);
// The handles under the histogram, one each for the darks, the midtones and the lights: where one stands, 0 to 1 across
// the histogram, for its slider's value - the point of the tone curve that slider moves, a fifth, a half or four fifths
// along, moved as far as the curve moves it, a tenth at most - and the value a handle dragged to a place sets.
double edit_tone_handle_position(int handle, double value);
double edit_tone_handle_value(int handle, double position);
// What Save As offers a copy of a video or a sound file: its own format where Edit writes it, under each of its
// extensions, the source's first, and MP4 where `offers_mp4` says an MP4 copy can be made. Empty for a photo, which
// the dialog offers the formats Diffractor writes.
std::vector<platform::file_dialog_filter> edit_save_filters(core::file_path source, bool offers_mp4);
// A trim point as the panel shows it: minutes, seconds and a tenth, with hours once there are any.
std::string format_trim_time(double seconds);
// What a repairing copy says once it is saved: that the file was damaged, the parts it left out, and where it ends
// when it could be read no further - an ended_at that is negative says it read to the end.
std::string edit_repair_report(int skipped, double ended_at);

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
	std::shared_ptr<edit_histogram_control> _histogram;
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
	// A video's or a sound file's controls, shown in place of a photo's.
	std::shared_ptr<ui::title_control> _trim_title;
	std::shared_ptr<text_element> _trim_hint;
	view_element_ptr _trim_buttons;
	ui::button_ptr _trim_reset;
	std::shared_ptr<ui::check_control> _remove_sound_check;
	std::shared_ptr<ui::title_control> _turn_title;
	view_element_ptr _turn_buttons;
	ui::button_ptr _turn_left;
	ui::button_ptr _turn_right;
	ui::button_ptr _turn_reset;
	std::shared_ptr<text_element> _note;
	// What _note says, so a change that leaves it as it was lays nothing out again.
	std::string _note_text;
	std::shared_ptr<divider_element> _save_divider;
	std::shared_ptr<ui::title_control> _save_title;
	std::shared_ptr<ui::check_control> _backup_check;
	// How a copy of a video or a sound file is written.
	std::shared_ptr<ui::check_control> _index_first_check;
	std::shared_ptr<ui::check_control> _repair_check;
	std::shared_ptr<ui::slider_control> _jpeg_quality_slider;
	std::shared_ptr<ui::slider_control> _webp_quality_slider;
	std::shared_ptr<ui::check_control> _webp_lossless_check;
	std::shared_ptr<ui::slider_control> _jxl_quality_slider;
	std::shared_ptr<ui::check_control> _jxl_lossless_check;
	std::shared_ptr<ui::slider_control> _avif_quality_slider;
	std::shared_ptr<ui::check_control> _avif_lossless_check;

	edit_view_controls(view_state& s, edit_view_state& es) : view_controls_host(s), _edit_state(es)
	{
		_scroller._scroll_child_controls = true;
	}

	void layout_controls(render::measure_context& mc) override;
	void create_controls();
	bool is_tracking() const;
	// What the panel says of the draft: of a video's or a sound file's, what a copy in this format does differently or
	// cannot keep, and which resets have anything to undo; of a photo's, what its edit keeps of its depth and range.
	// Returns whether anything shown changed.
	bool update_notes();

	void options_changed() override;
};

class edit_view final : public view_base, public std::enable_shared_from_this<edit_view>
{
	using this_type = edit_view;

	struct source_load_request
	{
		core::file_path path;
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
	core::file_path _path;
	std::string_view _xmp_name;
	file_type_ref _mt = nullptr;
	file_load_result _loaded;
	core::const_surface_ptr _preview_source;
	// The request _preview_source answered; meaningful only while it is set.
	sizei _preview_source_request;
	core::const_surface_ptr _dialog_preview_source;
	// Shed on device loss from the const broadcast; render rebuilds whenever it is absent, so
	// clearing it is the whole recovery.
	mutable render::texture_ptr _texture;
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
	// What a lossless copy of the shown video or sound file can do, read on a worker when it is shown;
	// cannot edit until it arrives. Its own format's where Edit writes it back, and MP4's where Edit does not.
	av_edit_plan _av_plan;
	// What an MP4 copy can do, which Save As offers beside the file's own format: the plan above where that is MP4.
	av_edit_plan _av_mp4_plan;
	// The shown item is a video or a sound file Edit opens, and its plan has not arrived yet.
	bool _is_av = false;
	bool _av_plan_pending = false;
	// The status bar's account of a video's or a sound file's draft: the changes a save would make.
	std::string _status;
	// The histogram the panel draws: a sample of the preview's pixels, counted again for each change of colour.
	std::vector<std::array<float, 3>> _histogram_samples;
	edit_histogram _histogram_counts;
	std::optional<size_t> _histogram_counted_at;
	// The before-and-after handle on the photo's left edge: how far across the picture its line stands - 0 at rest,
	// where the edit shows whole - and whether it is held. Let go, it slides back. Where the picture and the handle
	// were last drawn, which the handle is found and dragged against.
	ui::animate_alpha _compare;
	bool _comparing = false;
	recti _compare_picture;
	recti _compare_handle;

	friend class selection_move_controller<this_type>;
	friend class handle_move_controller<this_type>;
	friend class edit_compare_controller;

public:
	edit_view(view_state& s, view_host_ptr host, edit_view_state& evs);
	recti calc_media_bounds() const { return {0, 0, _extent.cx, _extent.cy}; }

	view_controls_host_ptr controls(const ui::frame_ptr& owner);
	static core::const_surface_ptr build_preview_surface(const core::const_surface_ptr& source, sizei loaded_dimensions,
	                                                     const image_edits& edits);
	core::const_surface_ptr preview_surface() const;

	void activate(sizei extent) override;
	void deactivate() override;
	void refresh() override;
	void update_media_elements() override;

	void layout(render::measure_context& mc, sizei extent) override;
	bool is_photo() const;
	bool can_edit_pixels() const;
	// What the edit's load found of the photo's depth, range and gain map.
	const picture_signal& signal() const { return _loaded.signal; }
	// A video or a sound file whose plan has arrived and says a lossless copy can be made.
	bool edits_av() const { return _av_plan.can_edit; }
	bool is_av() const { return _is_av; }
	bool av_plan_pending() const { return _av_plan_pending; }
	const av_edit_plan& av_plan() const { return _av_plan; }
	const av_edit_plan& av_mp4_plan() const { return _av_mp4_plan; }
	// The plan a copy saved as `path` is made by: the file's own format's, or MP4's.
	const av_edit_plan& av_plan_for(core::file_path path) const;
	// Whether a copy of the video or sound file can be saved as `path`: in its own format, or as MP4.
	bool saves_av_as(core::file_path path) const;
	av_edits av_draft() const;
	void queue_av_plan();
	// Where Edit's trim starts and ends: the handles on the scrubber, and I and O at the frame shown.
	void trim(double start, double end);
	void trim_start_here();
	void trim_end_here();
	void trim_clear();
	// Every change to a video's or a sound file's draft: the preview, the panel, the status bar and the commands.
	// A trim alone changes nothing the view or the panel lays out; one not set by dragging its handle moves the handles
	// out from under a pointer standing still, so what is under the pointer is found again.
	enum class av_change
	{
		dragged_trim,
		trim,
		layout,
	};
	void av_changed(av_change change = av_change::layout);
	void clear_av_preview() const;
	void update_status();
	void queue_source_load(source_load_request request);
	void start_source_load(source_load_request request);
	void complete_source_load(const source_load_request& request, file_load_result loaded);
	void queue_preview_decode();
	void start_preview_decode(const preview_decode_request& request);
	void complete_preview_decode(const preview_decode_request& request, const core::const_surface_ptr& source,
	                             file_load_result::failure failure);
	void clear_crop_interaction_bounds();
	void draw_loading_status(render::draw_context& dc, std::string_view status) const;
	void changed();
	void cancel() const;
	void exit() override;
	bool escape() override;
	void save_current();
	void save(core::file_path src_path, core::file_path dst_path, std::string_view xmp_name, const ui::frame_ptr& owner,
	          std::function<void(bool)> complete);
	void save_av(core::file_path src_path, core::file_path dst_path, std::string_view xmp_name,
	             const ui::frame_ptr& owner, std::function<void(bool)> complete);
	// What the save dialogs show of the draft: a photo's preview, and nothing for a video or sound.
	std::vector<core::const_surface_ptr> dialog_previews() const;
	bool has_changes() const;
	void select_item(const df::item_element_ptr& item);
	void render(render::draw_context& dc, view_controller_ptr controller) override;
	bool can_exit() override;
	void display_changed() override;
	bool key_down(char32_t key, platform::key_state keys) override;
	std::string_view status() override { return _status; }

	void broadcast_event(const view_element_event& event) const override
	{
		if (event.type == view_element_event_type::free_graphics_resources) _texture.reset();
		if (_media_element) _media_element->dispatch_event(event);
		if (_play_element) _play_element->dispatch_event(event);
		if (_scrubber_element) _scrubber_element->dispatch_event(event);
	}

	static void draw_handle(render::draw_context& dc, recti handle_bounds2, float alpha);

	bool check_path(core::file_path& path, const ui::frame_ptr& owner) const;
	// Where a copy is saved, chosen in the save dialog: a video or a sound file in the formats Save As offers it, and a
	// photo in those Diffractor writes.
	bool select_path(core::file_path& path) const;
	// After a save made a new file - Save As, or a video saved as an MP4 beside its own - the new file is shown.
	void open_saved_copy(core::file_path path);
	df::item_element_ptr next_editable_item(bool forward) const;
	void save_and_next(bool forward);
	void save_as();

	void rotate_anticlockwise();
	void rotate_clockwise();
	void rotate_reset();
	void color_reset();
	void report_no_result(std::string_view title) const;
	void queue_auto_adjust(
		int max_dimension, std::string title,
		std::function<std::function<void(edit_view_state&)>(const core::const_surface_ptr&)> analyze);
	void auto_color();
	void auto_straighten();
	void auto_document();
	void toggle_preview();
	// The colour histogram of the photo as edited, which the panel draws.
	const edit_histogram& histogram();
	// The before-and-after handle held with the pointer at x, its line following it across the picture; and let go,
	// when it slides back to the picture's edge.
	void compare_at(int x);
	void compare_release();
	// How far across the picture the before-and-after line stands, 0 at rest; and where it was last drawn.
	float compare_position() const { return _compare.val(); }
	int compare_line_x() const
	{
		return _compare_picture.left + core::round(_compare.val() * _compare_picture.width());
	}
	// Steps what slides or fades - the before-and-after line going back, the straighten grid - until it settles.
	void animate();
	void draw_compare(render::draw_context& dc, recti picture, int line_x, float alpha);

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

	quadd selection() const { return _edit_state._edits.effective_crop_bounds(_loaded.dimensions()); }

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
			_title = std::format("{}: {}", core::app_name(), tt.editing_title);
		}
		else
		{
			_title = core::app_name();
		}

		return _title;
	}

	view_bars bars() const override;

	void options_changed() const
	{
		if (_edit_controls)
		{
			_edit_controls->options_changed();
		}
	}
};
