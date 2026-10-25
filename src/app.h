// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Main application frame and view management. Contains the app_frame class which
// orchestrates the UI, handles commands, manages views, and coordinates background tasks.

#pragma once

#include "model_display.h"
#include "model_tile_cache.h"
#include "app_search.h"

class sidebar_host;
class app_logo_element;
class view_state;
class edit_view_controls;
class items_view;
class selector_view;
class edit_view;
class movie_view;
class media_view;
class rename_view;
class batch_tool_view;
class sync_view;
class tags_view;
class import_view;
class locate_view;
class view_controls_host;

using view_controls_host_ptr = std::shared_ptr<view_controls_host>;

constexpr std::array media_volumes = {999, 777, 555, 333, 0};
extern icon_index volumes_icons[5];

std::vector<std::pair<std::string_view, std::string>> calc_app_info(const index_state& index, bool include_state);
bool is_app_installed();

inline float command_progress_alpha(const bool active, const int64_t total, const uint64_t tick_count)
{
	if (!active || total != 0) return 1.0f;
	if (!ui::animations_enabled) return 0.65f;

	constexpr int64_t period_ms = 1200;
	constexpr double pi = 3.14159265358979323846;
	const auto phase = static_cast<double>(tick_count % period_ms) / period_ms;
	return 0.45f + 0.35f * static_cast<float>((std::sin(phase * pi * 2.0) + 1.0) / 2.0);
}

inline bool command_progress_needs_animation(const bool active, const int64_t total)
{
	return ui::animations_enabled && active && total == 0;
}

inline bool status_needs_animation(const bool progress_active, const int64_t progress_total, const bool show_debug_info)
{
	return command_progress_needs_animation(progress_active, progress_total) || show_debug_info;
}

// Defined in app.cpp so callers do not need the whole sidebar header for the logo lockup.
view_element_ptr create_app_logo_element(view_state& s, ui::style::font_face font, bool interactive,
                                         bool show_plasma, double logo_scale,
                                         const view_element_options& options);


class view_frame;

class shell_file_operation_ui final : df::no_copy
{
	view_frame& _view;
	ui::control_frame_ptr _main_frame;

public:
	shell_file_operation_ui(view_frame& view, ui::control_frame_ptr main_frame);
	~shell_file_operation_ui() override;
};

class view_frame final : public std::enable_shared_from_this<view_frame>, public view_host
{
public:
	using this_type = view_frame;

	view_state& _state;
	ui::frame_ptr _frame;
	ui::control_frame_ptr _owner;

	pointi _pan_start_loc;

	std::shared_ptr<view_base> _view;

	bool _control_down = false;

	// Wheel accumulation happens once, here, rather than in each consumer. Two remainders per axis
	// because the two answers are quantised differently: whole detents for the surfaces that move by
	// detent, whole logical units for the ones that scroll.
	struct wheel_accumulator
	{
		double steps = 0.0;
		double scroll = 0.0;
	};

	wheel_accumulator _wheel_vertical;
	wheel_accumulator _wheel_horizontal;
	double _wheel_pending_pinch = 0.0;

	view_frame(view_state& s) : _state(s)
	{
	}

	// A device reporting whole detents and one reporting fractions of them reach the view as the same
	// thing: both remainders are carried, so a touchpad accumulates instead of being divided to
	// nothing, and neither answer can be rounded away twice on the way through.
	bool dispatch_wheel(const pointi loc, const int delta, const ui::key_state keys, const ui::wheel_axis axis,
	                    wheel_accumulator& pending)
	{
		const ui::wheel_notch notch{
			axis,
			ui::accumulate_wheel_steps(pending.steps, delta),
			ui::accumulate_wheel_steps(pending.scroll, delta / 2.0, 1.0),
			keys
		};

		if (notch.steps == 0 && notch.delta == 0) return false;
		return _view->mouse_wheel(loc, notch);
	}

	void init(const ui::control_frame_ptr& owner)
	{
		ui::frame_style fs;
		fs.hardware_accelerated = true;
		fs.colors = {
			ui::style::color::view_background, ui::style::color::view_text, ui::style::color::view_selected_background
		};
		fs.can_focus = true;
		fs.can_drop = true;
		_frame = owner->create_frame(weak_from_this(), fs);
		_owner = owner;
	}

	void tick() override
	{
		if (_active_controller)
		{
			_active_controller->tick();
		}

		sync_modifier_cursor();
	}

	// zoom.md: cursor state follows state rather than waiting for the next pointer move. Ctrl is what
	// arms drawing a region, and a held pointer produces no event that would otherwise re-ask.
	// key_down calls this on the press so the change is immediate; the tick covers the release, for
	// which there is no key event at all.
	void sync_modifier_cursor()
	{
		const auto control_down = ui::current_key_state().control;
		if (_control_down == control_down) return;
		_control_down = control_down;
		if (_hover && _active_controller) update_cursor();
	}

	// Never null after construction: most handlers below dereference _view without testing it, and a
	// frame with no view has nothing useful to become. A null assignment keeps the previous view
	// rather than clearing it, so view_changed's default branch cannot empty a live frame.
	void view(std::shared_ptr<view_base> v)
	{
		if (v) _view = std::move(v);
	}

	bool is_occluded() const
	{
		return frame()->is_occluded();
	}

	void invalidate_controller()
	{
		_controller_invalid = true;
		update_controller(frame()->cursor_location());
	}

	void on_window_destroy() override
	{
	}

	void on_window_layout(ui::measure_context& mc, const sizei extent, const bool is_minimized) override
	{
		_extent = extent;
		_view->layout(mc, extent);
	}

	void on_window_paint(ui::draw_context& dc) override
	{
		dc.time_now = df::now();
		dc.colors.alpha = 1.0;
		dc.colors.overlay_alpha = 1.0;

		_view->render(dc, _active_controller);

		if (_active_controller)
		{
			_active_controller->draw(dc);
		}

		const auto display = _state.display_state();
		if (!display || !display->is_zoom_mode()) draw_view_status(dc);
		draw_status(dc);

		if (setting.show_debug_info && _active_controller)
		{
			const auto c = ui::color(1.0f, 0.0f, 0.0f, 1.0f);
			const auto pad = df::round(2 * dc.scale_factor);
			dc.draw_border(_controller_bounds, _controller_bounds.inflate(pad), c, c);
		}
	}

	void on_mouse_wheel(const pointi loc, const int delta, const ui::key_state keys, bool& was_handled) override
	{
		was_handled = dispatch_wheel(loc, delta, keys, ui::wheel_axis::vertical, _wheel_vertical);
		update_controller(loc);
	}

	void on_mouse_hwheel(const pointi loc, const int delta, const ui::key_state keys, bool& was_handled) override
	{
		was_handled = dispatch_wheel(loc, delta, keys, ui::wheel_axis::horizontal, _wheel_horizontal);
		update_controller(loc);
	}

	void on_pinch(const pointi loc, const int delta, const bool begins, bool& was_handled) override
	{
		if (begins) _wheel_pending_pinch = 0.0;
		const auto steps = ui::accumulate_wheel_steps(_wheel_pending_pinch, delta);
		if (steps != 0) was_handled = _view->pinch(loc, steps);
		update_controller(loc);
	}

	// The view sees the press before the controller does, so it can release text focus the press
	// did not land in. A rendered edit that kept focus would swallow every keyboard command.
	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_view->mouse_down(loc);
		view_host::on_mouse_left_button_down(loc, keys);
	}

	bool key_down(const int c, const ui::key_state keys) override
	{
		return false;
	}

	bool is_caption_area(const pointi loc) const override
	{
		return _view->is_caption_area(loc);
	}

	void pan_start(const pointi start_loc) override
	{
		_pan_start_loc = start_loc;
		_view->pan_start(_pan_start_loc);
	}

	void pan(const pointi start_loc, const pointi current_loc) override
	{
		_view->pan(_pan_start_loc, current_loc);
	}

	void pan_end(const pointi start_loc, const pointi final_loc) override
	{
		_view->pan_end(_pan_start_loc, final_loc);
	}

	bool touch_double_tap(const pointi location) override
	{
		return _view->touch_double_tap(location);
	}

	void on_mouse_other_button_up(const ui::other_mouse_button& button, const pointi loc,
	                              const ui::key_state keys) override
	{
		switch (button)
		{
		case ui::other_mouse_button::xb1:
			_state.browse_back(shared_from_this());
			break;
		case ui::other_mouse_button::xb2:
			_state.browse_forward(shared_from_this());
			break;
		default:
			break;
		}
	}

	void focus_changed(const bool has_focus, const ui::control_base_ptr& child) override
	{
		df::trace(std::format("render_window::focus {}", has_focus));
		_view->focus(has_focus);
		frame()->invalidate();
	}

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

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
	{
		return _view->controller_from_location(shared_from_this(), loc, ctx);
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
		const auto f = frame();
		_state.track_menu(f, bounds.offset(f->window_bounds().top_left()), commands);
	}

	void layout() const
	{
		frame()->layout();
	}

	void invalidate_element(const view_element_ptr& e) override
	{
		frame()->invalidate();
	}

	std::string _status_title;
	std::string _status_text;
	int _status_padding = 8;

	void update_status(std::string_view title, std::string_view text, int padding = 8);
	void clear_status();
	void draw_view_status(ui::draw_context& dc) const;
	void draw_status(ui::draw_context& dc) const;
	void redraw_now() const { frame()->redraw_now(); }

	platform::drop_effect drag_over(const platform::clipboard_data& data, const ui::key_state keys,
	                                const pointi loc) override
	{
		auto result = platform::drop_effect::none;

		// The Movie timeline is a document, so a drop adds to the movie rather than opening a file
		// or copying one into a folder. It is answered before anything else because none of the
		// questions below - is this over the items, is there a save path - mean anything there.
		if (_state.view_mode() == view_type::movie)
		{
			if (data.has_drop_files())
			{
				update_status(tt.command_movie_add, data.files_description().first_name);
				_view->drag_paths_over(loc);
				result = platform::drop_effect::copy;
			}
			else
			{
				_view->drag_paths_over(std::nullopt);
			}

			return result;
		}

		if (df::dragging_items == 0)
		{
			if (_view->is_over_items(loc))
			{
				const auto has_save_path = _state.search().is_showing_folder();

				if (has_save_path)
				{
					if (data.has_drop_files())
					{
						const auto dest_path = _state.save_path();
						const auto desc = data.files_description();
						const auto is_copy = !keys.shift && (keys.control || desc.preferred_drop_effect ==
							platform::drop_effect::copy);

						update_status(is_copy ? tt.platform_text.menu_copy : tt.menu_move,
						              std::format("{}\n{}\n{}", desc.first_name, tt.copy_to_join, dest_path.text()));

						result = is_copy ? platform::drop_effect::copy : platform::drop_effect::move;
					}
					else if (data.has_bitmap())
					{
						update_status(tt.save_new_photo, {});
						result = platform::drop_effect::copy;
					}
				}
			}
			else if (data.has_drop_files())
			{
				update_status(tt.open_title, data.first_path().str());
				result = platform::drop_effect::link;
			}
		}

		return result;
	}

	platform::drop_effect drag_drop(platform::clipboard_data& data, const ui::key_state keys, const pointi loc) override
	{
		auto result = platform::drop_effect::none;

		if (_state.view_mode() == view_type::movie)
		{
			if (data.has_drop_files() && _view->drop_paths(data.drop_paths(), loc))
			{
				result = platform::drop_effect::copy;
			}

			drag_leave();
			return result;
		}

		if (df::dragging_items == 0)
		{
			if (_view->is_over_items(loc))
			{
				const auto has_save_path = _state.search().is_showing_folder();

				if (has_save_path)
				{
					if (data.has_drop_files())
					{
						const auto save_path = _state.save_path();
						const auto desc = data.files_description();
						const auto is_copy = !keys.shift && (keys.control || desc.preferred_drop_effect ==
							platform::drop_effect::copy);
						const auto drop_action = is_copy ? platform::drop_effect::copy : platform::drop_effect::move;

						detach_file_handles detach(_state);
						shell_file_operation_ui processing(*this, _owner);
						const auto drop_result = data.drop_files(save_path, drop_action);

						if (drop_result.success())
						{
							if (!drop_result.created_files.files.empty() || !drop_result.created_files.folders.empty())
								detach.keep_display_closed();
							_state.open(shared_from_this(), _state.search(),
							            make_unique_paths(drop_result.created_files));
							result = drop_action;
						}
					}
					else if (data.has_bitmap())
					{
						const auto save_result = data.save_bitmap(_state.save_path(), "dropped", false);

						if (save_result.success())
						{
							result = platform::drop_effect::copy;
							_state.open(shared_from_this(), _state.search(),
							            make_unique_paths(save_result.created_files));
						}
					}
				}
			}
			else if (data.has_drop_files())
			{
				const auto path = data.first_path();
				_state.open(shared_from_this(), path);
				result = platform::drop_effect::link;
			}
		}

		drag_leave();
		return result;
	}

	void drag_leave() override
	{
		clear_status();
		if (_view) _view->drag_paths_over(std::nullopt);
	}

	void invalidate_view(const view_invalid invalid) override
	{
		_state.invalidate_view(invalid);
	}
};

struct lerp_animate
{
	int val = 0;
	int target = 0;

	bool step()
	{
		if (val != target)
		{
			const auto step = (target - val) / 2;
			val = step == 0 ? target : val + step;
			return true;
		}

		return false;
	}

	ui::color32 lerp(const ui::color32& c1, const ui::color32& c2) const
	{
		return val == 0 ? c1 : ui::lerp(c1, c2, val);
	}
};


class app_frame final :
	public state_strategy,
	public async_strategy,
	public ui::app,
	public ui::frame_host,
	public std::enable_shared_from_this<app_frame>
{
public:
	using this_type = app_frame;

	location_cache _locations;
	index_state _item_index;
	std::shared_ptr<av_player> _player;
	view_state _state;
	edit_view_state _edit_view_state;
	movie_view_state _movie_view_state;
	database _db;
	tile_cache_db _tile_db;
	const ui::plat_app_ptr _pa;
	platform::setting_file_ptr _settings;

	platform::queue<platform::task_queue::task_t> _ui_queue;
	platform::task_queue cloud_task_queue;
	platform::task_queue database_task_queue;
	platform::task_queue index_task_queue;
	platform::task_queue load_task_queue;
	platform::task_queue load_raw_task_queue;
	platform::task_queue location_task_queue;
	platform::task_queue sidebar_task_queue;
	platform::task_queue web_task_queue;
	platform::task_queue map_tile_task_queue;
	// One thread, because the SQLite build serialises nothing for us: this connection is only ever
	// touched from here.
	platform::task_queue tile_db_task_queue;
	platform::task_queue predictions_task_queue;
	platform::task_queue summary_task_queue;
	platform::task_queue presence_task_queue;
	platform::task_queue auto_complete_task_queue;
	platform::task_queue query_task_queue;
	platform::task_queue render_task_queue;
	// Separate from render so the image being viewed is never queued behind background thumbnail
	// staging, which reaches hundreds of tasks deep while stepping through a folder.
	platform::task_queue render_display_task_queue;
	platform::task_queue scan_folder_task_queue;
	platform::task_queue scan_modified_items_task_queue;
	platform::task_queue scan_displayed_items_task_queue;
	platform::task_queue crc_task_queue;
	platform::task_queue work_task_queue;
	platform::threads _threads;

	// UI-thread owned. The database-open completion and every collection walk ask to bring the index
	// workers up, and any of them can win, so the bring-up is claimed once.
	bool _index_workers_started = false;

	// These workers are created on first use rather than at startup, so a session that never opens the
	// map, never reaches the network and never types in search pays for none of them. Claimed once by
	// claim_worker_start, which runs on whichever thread first queues to that queue.
	std::atomic_bool _web_worker_started = false;
	std::atomic_bool _cloud_worker_started = false;
	std::atomic_bool _auto_complete_worker_started = false;
	std::atomic_bool _map_tile_workers_started = false;
	std::atomic_bool _tile_db_worker_started = false;
	std::atomic_bool _media_preview_worker_started = false;

	ui::control_frame_ptr _app_frame;
	std::shared_ptr<app_logo_element> _app_logo;

	// Shared native top-bar controls remain available independently of the primary renderer.
	ui::toolbar_ptr _navigate1;
	ui::edit_ptr _search_edit;
	ui::toolbar_ptr _navigate2;
	ui::toolbar_ptr _navigate3;

	ui::toolbar_ptr _media_edit_commands;
	ui::toolbar_ptr _tool_commands;
	ui::toolbar_ptr _import_commands;
	ui::toolbar_ptr _locate_commands;
	ui::toolbar_ptr _sync_commands;
	ui::toolbar_ptr _tags_commands;
	ui::toolbar_ptr _movie_commands;
	ui::toolbar_ptr _busy_commands;

	std::shared_ptr<ui::complete_strategy_t> _search_completes;
	ui::list_window_ptr _search_predictions_frame;

	bool _search_has_focus = false;
	search_edit_session _search_session;
	bool _search_setting_text = false;
	bool _app_logo_hover = false;

	// The item full screen selected on the user's behalf, so leaving full screen can undo it.
	df::item_element_ptr _full_screen_auto_selected;
	bool _view_has_focus = false;
	bool _view_controls_have_focus = false;

	view_controls_host_ptr _view_controls;
	std::shared_ptr<view_frame> _view_frame;
	std::shared_ptr<view_frame> _selector_frame;

	std::shared_ptr<rename_view> _view_rename;
	std::shared_ptr<batch_tool_view> _view_batch;
	std::shared_ptr<import_view> _view_import;
	std::shared_ptr<locate_view> _view_locate;
	std::shared_ptr<sync_view> _view_sync;
	std::shared_ptr<tags_view> _view_tags;

	std::shared_ptr<items_view> _view_items;
	std::shared_ptr<selector_view> _view_selector;
	std::shared_ptr<edit_view> _view_edit;
	std::shared_ptr<movie_view> _view_movie;
	std::shared_ptr<media_view> _view_media;
	std::shared_ptr<view_base> _view;

	ui::bubble_window_ptr _bubble;

	int _frame_delay = 0;
	// Previous prepare_frame timestamp, for the elapsed time alpha fades advance by.
	int64_t _last_prepare_us = 0;
	lerp_animate _search_color_lerp;
	group_by _starting_group_order = group_by::file_type;
	sort_by _starting_sort_order = sort_by::def;
	std::vector<file_group_ref> _starting_media_filter;

	sizei _extent;
	recti _view_bounds;
	recti _top_bar_bounds;
	recti _title_bounds;
	std::optional<recti> _last_texture_eviction_bounds;

	// Written only by layout so the drag can map a pointer position straight back to a stored
	// proportion, and read only on the UI thread by paint and the mouse handlers.
	struct view_controls_splitter_t
	{
		recti bounds;
		int client_left = 0;
		int client_right = 0;
		int width = 0;
		int min_pane = 0;
		bool hover = false;
		bool tracking = false;
	} _controls_splitter;

	void drag_controls_splitter(pointi loc);
	void update_controls_splitter_hover(bool hover);

	// Time of the most recent folder-watch notification that has not yet produced a response, and the
	// folders that signalled during that burst. UI thread only.
	double _folder_change_time = 0.0;
	df::unique_folders _folders_changed;
	std::string saved_current_search;
	bool _is_active = false;

	// Machine-scoped and only changed by a device arriving, which system_device_change reports, so it
	// is answered once rather than by instantiating a shell COM object on every command-state update.
	std::optional<bool> _has_burner;

	// Whether this machine can encode a movie, once the startup worker has asked. Asking starts Media
	// Foundation and enumerates its encoders, about half a second, which the first command-state update
	// used to spend on the UI thread before the window had painted. Unknown reads as no, so Render is
	// absent for the moment before the answer, as it is on a machine that cannot render at all.
	std::optional<bool> _can_write_movies;

	// Recovery from a crash that happened before the user could touch anything. _safe_start says
	// this launch reverted presentation because previous ones never settled; _startup_settled
	// latches the moment this one did, which clears the persisted count.
	bool _safe_start = false;
	bool _startup_settled = false;
	bool _owns_startup_scope = false;
	void mark_startup_settled();
	void report_safe_start();

#ifdef _DEBUG
	double _screenshot_ready_time = 0;
	int _screenshot_stage = 0;
#endif

	commands_map _commands;
	ui::command_ptr _hover_command;
	recti _hover_command_bounds;
	work_progress_reports _work_reports;

	std::atomic<view_invalid> _invalids = view_invalid::none;

	// Guards against re-entrant draining of pending UI work. complete_pending_events can be reached
	// again while it is already running, because a UI-thread wait (ui_wait_for_signal) pumps messages
	// and re-runs the idle action. A nested drain would run more queued callbacks on the same stack -
	// unbounded growth if any of them wait again - so nested calls return immediately and the queued
	// work is picked up on the next idle pass instead.
	std::atomic_int _completing_pending_events = 0;

	// Set when the cap above skipped a drain. queue_ui only wakes idle on the empty-to-nonempty
	// transition, so a skipped drain leaves an already-nonempty queue with no wake pending; the
	// outermost drain re-arms idle on its way out. Re-arming from the nested call instead would
	// spin, because the wait that re-entered us would wake straight back into the same cap.
	std::atomic_bool _pending_events_deferred = false;

	view_hover_element _hover;

	app_frame(ui::plat_app_ptr pa);
	~app_frame() override;

	void free_graphics_resources(bool items_only, bool offscreen_only) override;
	void track_menu(const ui::frame_ptr& parent, recti bounds, const std::vector<ui::command_ptr>& commands) override;
	void idle() override;
	void hide_search_predictions();
	bool key_down(char32_t key, ui::key_state keys) override;
	bool text_input(std::string_view text) override;
	ui::focus_mode focus_mode() const override;
	void create_toolbars();
	void crash(df::file_path dump_file_path) override;
	std::string restart_cmd_line() override;
	void save_recovery_state() override;
	void invalidate_view(view_invalid invalid) override;
	void invoke(commands id) override;
	void invoke(const command_info_ptr& c);
	void toggle_full_screen() override;
	bool can_open_search(const df::search_t& path) override;
	void report_scope_unavailable(const df::search_t& path) override;
	void folder_changed(df::folder_path folder) override;
	void dpi_changed() override;
	void on_window_layout(ui::measure_context& mc, sizei extent, bool is_minimized) override;
	void on_window_paint(ui::draw_context& dc) override;
	bool is_caption_area(pointi loc) const override;
	void activate(bool is_active) override;
	void app_fail(std::string_view message, std::string_view more_text) override;
	void invalidate_status() const;
	void update_overlay();
	void tick() override;
#ifdef _DEBUG
	void run_test_action(std::string_view action);
	void tick_screenshot();
#endif
	void prepare_frame() override;
	void update_tooltip();
	void item_focus_changed(const df::item_element_ptr& focus, const df::item_element_ptr& previous) override;

	// One test decides whether a view shows the selector strip, what it offers and what a click on it
	// does, so the three cannot disagree.
	enum class selector_strip
	{
		none,
		photo,
		metadata
	};

	selector_strip selector_strip_for_view(view_type m) const;
	void select_from_selector(const df::item_element_ptr& item, ui::key_state keys);
	void reset_selector_selection_anchor();
	void make_visible(const df::item_element_ptr& i) override;
	bool is_command_checked(commands cmd) override;
	void element_broadcast(const view_element_event& event) override;
	recti calc_search_popup_bounds() const;
	void layout(ui::measure_context& mc);
	void complete_pending_events();
	bool load_settings(const platform::setting_file_ptr& store) override;
	void load_options(const platform::setting_file_ptr& store);
	void display_changed() override;
	void open_default_folder();
	void view_changed(view_type m) override;
	void search_complete(const df::search_t& path, bool path_changed) override;
	void save_options(bool search_only = false);
	void reload();
	void queue_ui(std::function<void()> f) override;
	void queue_async(async_queue q, std::function<void()> f) override;
	void queue_async_after(async_queue q, uint32_t delay_ms, std::function<void()> f) override;
	void queue_location(std::function<void(location_cache&)>) override;
	void queue_database(std::function<void(database&)> f) override;
	void queue_tile_db(std::function<void(tile_cache_db&)> f) override;
	void web_service_cache(std::string key, std::function<void(const std::string&)> f) override;
	void web_service_cache(std::string key, std::string value) override;
	void queue_media_preview(std::function<void(media_preview_state&)> f, bool must_run) override;
	void work_progress(const void* source, int64_t done, int64_t total) override;
	void work_finished(const void* source) override;
	static icon_index repeat_toggle_icon();
	bool update_toolbar_text(commands cc, const std::string& text);
	void update_button_state(bool resize);
	bool edit_has_changes() const;
	void update_index();
	void rebuild_index();
	void queue_index_update(bool forget_cached_metadata);
	void toggle_volume();
	static icon_index sound_icon();
	void def_command(commands id, command_group group, icon_index icon, std::string_view text,
	                 std::string_view tooltip = {});
	void language_changed(std::string_view lang_code);
	void update_command_text();
	void initialise_commands();
	command_info_ptr find_or_create_command_info(commands id);
	void add_command_invoke(commands id, std::function<void()> invoke);
	ui::command_ptr find_command(commands id) const override;
	void tooltip(view_hover_element& hover, commands id) const;
	void search_text_changed(std::string_view text);
	void preview_search_prediction(const std::string& text);
	void set_search_edit_text(std::string_view text);
	void delete_items(const df::item_set& items) override;

	// The run command of the task view currently shown, or none outside the task views.
	commands task_view_run_command() const;

	void focus_view() override;
	bool can_exit() override;
	bool pre_init() override;
	bool parse_command_line(std::string_view command_line_text) override;
	int run_console_mode() override;
	bool wants_gpu() const override;
	void start_workers();
	void start_index_workers();
	bool claim_worker_start(std::atomic_bool& started);
	void ensure_worker(std::atomic_bool& started, platform::task_queue& q, std::string_view name);
	void update_font_size() const;
	bool init(std::string_view command_line) override;
	void init_search();
	void final_exit() override;
	void exit() override;
	void system_event(ui::os_event_type ost) override;
	void search_enter();
	void cancel_search_edit();
	bool search_accept_selected();

	void on_mouse_move(pointi loc, bool is_tracking) override;

	void on_mouse_left_button_down(pointi loc, ui::key_state keys) override;

	void on_mouse_left_button_up(pointi loc, ui::key_state keys) override;

	void on_mouse_leave(pointi loc) override;

	void on_mouse_wheel(const pointi loc, const int delta, const ui::key_state keys, bool& was_handled) override
	{
	}

	void on_mouse_left_button_double_click(const pointi loc, const ui::key_state keys) override
	{
	}

	void on_mouse_other_button_up(const ui::other_mouse_button& button, const pointi loc,
	                              const ui::key_state keys) override
	{
	}

	void pan_start(const pointi start_loc) override
	{
	}

	void pan(const pointi start_loc, const pointi current_loc) override
	{
	}

	void pan_end(const pointi start_loc, const pointi final_loc) override
	{
	}

	void focus_search(bool has_focus);
	void focus_changed(bool has_focus, const ui::control_base_ptr& child) override;
	void on_window_destroy() override;
	void command_hover(const ui::command_ptr& c, recti window_bounds) override;

	std::vector<ui::command_ptr> menu(pointi loc) override;
};

using app_frame_ptr = std::shared_ptr<app_frame>;


class pause_media
{
	view_state& _state;
	display_state_ptr _display;

	bool _is_playable = false;
	bool _is_playing = false;

public:
	pause_media(view_state& s) : _state(s)
	{
		_display = s.display_state();

		if (_display)
		{
			_is_playable = _display->can_play_media();
			_is_playing = _display->is_playing_media();

			if (_is_playing && _display->_session)
			{
				_state._player->pause(_display->_session);
			}

			_display->stop_slideshow();
		}
	}

	~pause_media()
	{
		if (_is_playable && _is_playing)
		{
			if (_display == _state.display_state() && _display->_item1 && _display->_session)
			{
				const auto path = _display->_item1->path();

				if (path.exists())
				{
					_state._player->play(_display->_session);
				}
			}
		}
	}

	pause_media(const pause_media& other) = delete;
	pause_media(pause_media&& other) noexcept = delete;
	pause_media& operator=(const pause_media& other) = delete;
	pause_media& operator=(pause_media&& other) noexcept = delete;
};
