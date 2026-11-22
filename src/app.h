// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Main application frame and view management. Contains the app_frame class which
// orchestrates the UI, handles commands, manages views, and coordinates background tasks.

#pragma once

#include "model_display.h"
#include "model_tile_cache.h"
#include "app_commands.h"
#include "app_record.h"
#include "app_tutorial.h"
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
class explore_view;
class faces_view;
class options_view;
class view_controls_host;
class mcp_host;

using view_controls_host_ptr = std::shared_ptr<view_controls_host>;

constexpr std::array media_volumes = {999, 777, 555, 333, 0};
extern icon_index volumes_icons[5];

std::vector<std::pair<std::string_view, std::string>> calc_app_info(const index_state& index, bool include_state);
bool is_app_installed();

inline float command_progress_alpha(const bool active, const int64_t total, const uint64_t tick_count)
{
	if (!active || total != 0) return 1.0f;
	if (!render::animations_enabled) return 0.65f;

	constexpr int64_t period_ms = 1200;
	constexpr double pi = 3.14159265358979323846;
	const auto phase = static_cast<double>(tick_count % period_ms) / period_ms;
	return 0.45f + 0.35f * static_cast<float>((std::sin(phase * pi * 2.0) + 1.0) / 2.0);
}

inline bool command_progress_needs_animation(const bool active, const int64_t total)
{
	return render::animations_enabled && active && total == 0;
}

// What each tick repaints on the status's behalf: the status bar while an open-ended run pulses in it,
// and the view while debug overlays are drawn over it.
struct status_repaint
{
	bool status_bar = false;
	bool view = false;
};

inline status_repaint status_repaints(const bool progress_active, const int64_t progress_total,
                                      const bool show_debug_info)
{
	return {command_progress_needs_animation(progress_active, progress_total), show_debug_info};
}

// One part of a bar as arrange_bar sees it.
struct bar_slot
{
	sizei extent;
	// The narrowest the part stays useful at; zero when it cannot narrow at all.
	int min_width = 0;
	// The first thing given up when the bar is short of room.
	bool optional = false;
};

struct bar_arrangement
{
	// One per slot, in the same order; empty for a slot given up.
	std::vector<recti> slots;
	// What is left once the slots and the gap beside them are placed: the title in the title bar, the
	// status in the status bar. At the leading end for a trailing arrangement, and the other way round.
	recti remainder;
};

enum class bar_alignment
{
	leading,
	trailing,
};

// Aligns the slots to one end of `bounds`, each centred vertically, the right end unless asked
// otherwise. When they do not fit, optional slots are given up first, then narrowing slots shrink
// towards their minimum, and only then are whole slots dropped from the end furthest from the one
// they are aligned to: for a trailing bar, where its commands sit, the trailing end is the last to
// go, and the last slot standing is clipped to the bar rather than dropped. A slot with no width
// takes no room, not even a gap.
bar_arrangement arrange_bar(recti bounds, const std::vector<bar_slot>& slots, int gap,
                            bar_alignment alignment = bar_alignment::trailing);

// Whether a focused control is worked by key, which is then the control's before any meaning the view gives it:
// Enter for a button or a toolbar, Space for any control, and the arrows, Home, End, Page Up and Page Down a
// toolbar, slider or radio group moves by. Enter on any other control still submits a task, as Enter in a dialog
// presses its default button, and a control that takes typing has had every key it edits with already.
bool control_takes_key(const ui::control& c, char32_t key, platform::key_state keys);

// Defined in app.cpp so callers do not need the whole sidebar header for the logo lockup.
view_element_ptr create_app_logo_element(view_state& s, render::font_face font, bool interactive, bool show_plasma,
                                         double logo_scale, const view_element_options& options);

class view_frame;

class shell_file_operation_ui final : core::no_copy
{
	view_frame& _view;
	ui::frame_ptr _main_frame;

public:
	shell_file_operation_ui(view_frame& view, ui::frame_ptr main_frame);
	~shell_file_operation_ui() override;
};

// A region of the window that shows a view: the main view, with the panel of its controls beside it and
// the strip of thumbnails under it where the view has them, or the strip itself. The panel is drawn here,
// in this frame's surface; the strip is a frame of its own, which this one places.
class view_frame final : public std::enable_shared_from_this<view_frame>, public view_host
{
public:
	using this_type = view_frame;

	view_state& _state;
	ui::frame_ptr _frame;
	ui::frame_ptr _owner;

	pointi _pan_start_loc;

	std::shared_ptr<view_base> _view;
	// Where the view is drawn: the frame, less the panel, the splitter and the strip.
	recti _view_bounds;

	// The panel of the view's controls, and whether it is the whole frame - Options has nothing beside it.
	view_controls_host_ptr _panel;
	bool _panel_fills = false;

	// The strip of thumbnails under the view, composed into its surface where wanted.
	std::shared_ptr<ui::frame_element> _strip = std::make_shared<ui::frame_element>();
	bool _strip_wanted = false;

	// Told as the keyboard comes to or leaves the view or a control of its panel.
	std::function<void(bool, const ui::control_ptr&)> focus_listener;

	// Whether a press gives the frame the keyboard. The strip's does not: a press there picks an item and leaves
	// the keyboard where it was, as a press on a toolbar does.
	bool _press_takes_keyboard = true;

	// The drawn part the keyboard is on, when it is on neither the view's own content nor a control of the panel:
	// the area that holds it, which part of which element, and where it was last found. A part that has gone is
	// followed to the part now nearest where it was, unless pressing it took it away, which leaves the keyboard on
	// the view.
	struct keyboard_focus
	{
		keyboard_area area = keyboard_area::view;
		std::weak_ptr<view_element> element;
		int index = 0;
		recti bounds;
		bool pressed = false;
	} _keyboard;

	// Whether the keyboard has moved since the pointer last pressed, so the view's own content shows the ring too.
	bool _keyboard_cues = false;

	// The strip between the view and the panel, dragged to share the width between them. Written only by
	// layout, so a drag can map a pointer back to the proportion stored for the view.
	struct splitter_t
	{
		recti bounds;
		int width = 0;
		int min_pane = 0;
		bool tracking = false;
	} _splitter;

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

	view_frame(view_state& s) : _state(s) {}

	// A device reporting whole detents and one reporting fractions of them reach the view as the same
	// thing: both remainders are carried, so a touchpad accumulates instead of being divided to
	// nothing, and neither answer can be rounded away twice on the way through.
	bool dispatch_wheel(const pointi loc, const int delta, const platform::key_state keys,
	                    const platform::wheel_axis axis, wheel_accumulator& pending)
	{
		const platform::wheel_notch notch{axis, platform::accumulate_wheel_steps(pending.steps, delta),
		                                  platform::accumulate_wheel_steps(pending.scroll, delta / 2.0, 1.0), keys};

		if (notch.steps == 0 && notch.delta == 0) return false;
		return _view->mouse_wheel(loc, notch);
	}

	void init(const ui::frame_ptr& owner, const bool embedded = false)
	{
		ui::frame_style fs;
		fs.hardware_accelerated = true;
		fs.embedded = embedded;
		fs.colors = {ui::style::colors.view_background, ui::style::colors.view_text,
		             ui::style::colors.view_selected_background};
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
		const auto control_down = platform::current_key_state().control;
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

	bool is_occluded() const { return frame()->is_occluded(); }

	void invalidate_controller()
	{
		_controller_invalid = true;
		update_controller(frame()->cursor_location());
	}

	void on_window_destroy() override {}

	// Shows a panel of the view's controls beside it, or none.
	void panel(view_controls_host_ptr panel, bool fills);
	bool in_panel(pointi loc) const;
	bool in_view(const pointi loc) const { return _view_bounds.contains(loc); }
	void drag_splitter(pointi loc);

	// The groups of drawn parts in the frame - the view's, then its panel's - and those of one area, where they are
	// drawn now.
	std::vector<keyboard_region> keyboard_regions() const;
	std::optional<keyboard_region> keyboard_region_of(keyboard_area area) const;
	std::vector<keyboard_part> keyboard_parts(keyboard_area area) const;
	// The part the keyboard is on, where it is drawn now; none while the keyboard is on the view's own content.
	std::optional<keyboard_part> focused_part();
	// Puts the keyboard on a part, brought into view, or with none on the view's own content.
	void keyboard_to(keyboard_area area, const keyboard_part* part);
	// The keys of the part the keyboard is on: the arrows, Home and End move it among its group, Enter and Space
	// press it, and Escape takes the keyboard back to the view - except from the panel, a task's dialog, where
	// Escape keeps its meaning for the task.
	bool keyboard_part_key_down(char32_t key, platform::key_state keys);
	void draw_keyboard_focus(render::draw_context& dc);

	// A press on the view gives it the keyboard; one between the panel's controls, or on the splitter beside
	// them, leaves the keyboard where it was, as it did when the panel was a window of its own.
	bool takes_keyboard_at(const pointi loc) const override
	{
		return _press_takes_keyboard && !in_panel(loc) && !_splitter.bounds.contains(loc);
	}

	void on_window_layout(render::measure_context& mc, sizei extent, bool is_minimized) override;
	void on_window_paint(render::draw_context& dc) override;

	void visit_elements(const std::function<void(const view_element_ptr&, pointi)>& visit) override;
	void on_mouse_wheel(pointi loc, int delta, platform::key_state keys, bool& was_handled) override;

	void on_mouse_hwheel(const pointi loc, const int delta, const platform::key_state keys, bool& was_handled) override
	{
		if (in_view(loc))
			was_handled = dispatch_wheel(loc, delta, keys, platform::wheel_axis::horizontal, _wheel_horizontal);
		update_controller(loc);
	}

	void on_pinch(const pointi loc, const int delta, const bool begins, bool& was_handled) override
	{
		if (begins) _wheel_pending_pinch = 0.0;
		const auto steps = platform::accumulate_wheel_steps(_wheel_pending_pinch, delta);
		if (steps != 0 && in_view(loc)) was_handled = _view->pinch(loc, steps);
		update_controller(loc);
	}

	// The view sees the press before the controller does, so it can release text focus the press
	// did not land in. A rendered edit that kept focus would swallow every keyboard command. A press
	// also takes the keyboard off any drawn part, as it takes it off the panel's controls.
	void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
	{
		_keyboard_cues = false;
		if (_keyboard.area != keyboard_area::view) keyboard_to(keyboard_area::view, nullptr);
		if (in_view(loc)) _view->mouse_down(loc);
		view_host::on_mouse_left_button_down(loc, keys);
	}

	bool key_down(const char32_t /*c*/, const platform::key_state /*keys*/) override { return false; }

	bool is_caption_area(const pointi loc) const override { return in_view(loc) && _view->is_caption_area(loc); }

	void pan_start(const pointi start_loc) override
	{
		_pan_start_loc = start_loc;
		if (in_view(start_loc)) _view->pan_start(_pan_start_loc);
	}

	void pan(const pointi /*start_loc*/, const pointi current_loc) override
	{
		if (in_view(_pan_start_loc)) _view->pan(_pan_start_loc, current_loc);
	}

	void pan_end(const pointi /*start_loc*/, const pointi final_loc) override
	{
		if (in_view(_pan_start_loc)) _view->pan_end(_pan_start_loc, final_loc);
	}

	bool touch_double_tap(const pointi location) override
	{
		return in_view(location) && _view->touch_double_tap(location);
	}

	void on_mouse_other_button_up(const platform::other_mouse_button& button, const pointi /*loc*/,
	                              const platform::key_state /*keys*/) override
	{
		switch (button)
		{
		case platform::other_mouse_button::xb1: _state.browse_back(shared_from_this()); break;
		case platform::other_mouse_button::xb2: _state.browse_forward(shared_from_this()); break;
		default: break;
		}
	}

	// The view has the keyboard while the frame does and none of the panel's controls holds it.
	void focus_changed(bool has_focus, const ui::control_ptr& child) override;

	const ui::frame_ptr frame() const override { return _frame ? _frame : ui::no_frame(); }

	const ui::frame_ptr owner() override { return _owner; }

	void controller_changed() override { _state.invalidate_view(view_invalid::tooltip); }

	// The splitter, the panel or the view, whichever holds loc; each answers within its own region.
	view_controller_ptr controller_from_location(pointi loc, hit_test_context& ctx) override;

	void invoke(const commands cmd) override { _state.invoke(cmd); }

	bool is_command_checked(const commands cmd) override { return _state.is_command_checked(cmd); }

	// bounds are this frame's; the menu opens where they are on the screen. On Windows the frame is a child
	// window, whose window bounds are its parent's coordinates, not the screen's.
	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override
	{
		const auto f = frame();
		_state.track_menu(f, bounds.offset(f->client_to_screen({})), commands);
	}

	void layout() const { frame()->layout(); }

	void invalidate_element(const view_element_ptr& /*e*/) override { frame()->invalidate(); }

	std::string _status_title;
	std::string _status_text;
	int _status_padding = 8;

	void update_status(std::string_view title, std::string_view text, int padding = 8);
	void clear_status();
	void draw_status(render::draw_context& dc) const;
	void redraw_now() const { frame()->redraw_now(); }

	platform::drop_effect drag_over(const platform::clipboard_data& data, const platform::key_state keys,
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
						const auto is_copy = !keys.shift && (keys.control ||
						                                     desc.preferred_drop_effect == platform::drop_effect::copy);

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

	platform::drop_effect drag_drop(platform::clipboard_data& data, const platform::key_state keys,
	                                const pointi loc) override
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
						const auto is_copy = !keys.shift && (keys.control ||
						                                     desc.preferred_drop_effect == platform::drop_effect::copy);
						const auto drop_action = is_copy ? platform::drop_effect::copy : platform::drop_effect::move;

						// What a drop is given, read before it runs, as a move takes them away.
						const auto given = data.drop_paths();
						const auto given_is_folder = folders_among(given);

						detach_file_handles detach(_state);
						shell_file_operation_ui processing(*this, _owner);
						const auto drop_result = data.drop_files(save_path, drop_action);

						if (drop_result.success())
						{
							record_dropped_files(_state,
							                     is_copy ? icon_index::copy_to_folder : icon_index::move_to_folder,
							                     is_copy ? tt.command_copy.sv() : tt.command_move.sv(), given,
							                     given_is_folder, drop_result.created_files, !is_copy);
							if (!drop_result.created_files.files.empty() || !drop_result.created_files.folders.empty())
								detach.keep_display_closed();
							_state.open(shared_from_this(), _state.search(),
							            make_unique_paths(drop_result.created_files));
							result = drop_action;
						}
					}
					else if (data.has_bitmap())
					{
						// Saved as a pasted picture is: as PNG, which loses nothing of it.
						const auto save_result = data.save_bitmap(_state.save_path(), std::string(tt.dropped_file_name),
						                                          true);

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

	void invalidate_view(const view_invalid invalid) override { _state.invalidate_view(invalid); }
};

class app_frame;

// The window's title bar along its top, or its status bar along its base: a region of the window drawn on
// the CPU renderer apart from the view, so a bar that changes draws itself alone. The application lays it
// out, draws it and answers its pointer in the bar's own coordinates; the address box, the filter, the item
// size and the toolbars the views declare are its controls.
class bar_frame final : public ui::frame_host, public std::enable_shared_from_this<bar_frame>
{
public:
	enum class kind
	{
		title,
		status
	};

	app_frame& _app;
	const kind _kind;
	ui::frame_ptr _frame;
	sizei _extent;
	// What the bar shows now, in the order Tab visits it.
	std::vector<ui::control_ptr> _controls;
	// Whether the title saw the press its next release ends.
	bool _title_pressed = false;

	bar_frame(app_frame& app, const kind k) : _app(app), _kind(k) {}

	void init(const ui::frame_ptr& owner);

	const ui::frame_ptr frame() const { return _frame ? _frame : ui::no_frame(); }

	void on_window_layout(render::measure_context& mc, sizei extent, bool is_minimized) override;
	void on_window_paint(render::draw_context& dc) override;
	void visit_elements(const std::function<void(const view_element_ptr&, pointi)>& visit) override;
	bool is_caption_area(pointi loc) const override;
	void on_mouse_move(pointi loc, bool is_tracking) override;
	void on_mouse_leave(pointi loc) override;
	void on_mouse_left_button_down(pointi loc, platform::key_state keys) override;
	void on_mouse_left_button_up(pointi loc, platform::key_state keys) override;
	void focus_changed(bool has_focus, const ui::control_ptr& child) override;
	void command_hover(const ui::command_ptr& c, recti window_bounds) override;
	std::vector<ui::command_ptr> menu(pointi loc) override;
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

	core::color32 lerp(const core::color32& c1, const core::color32& c2) const
	{
		return val == 0 ? c1 : core::lerp(c1, c2, val);
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
	platform::task_queue search_index_task_queue;
	platform::task_queue auto_complete_task_queue;
	platform::task_queue query_task_queue;
	platform::task_queue render_task_queue;
	// Separate from render, whose placeholders, strips and collages each decode in a fraction of the time the image
	// being viewed takes, so neither waits behind the other.
	platform::task_queue render_display_task_queue;
	// Several threads, started on first use: they decode the thumbnails items hold into surfaces to draw, which a
	// folder opening asks for by the hundred, so the render queue's work never waits behind them.
	platform::task_queue thumbnail_stage_task_queue;
	platform::task_queue scan_folder_task_queue;
	platform::task_queue scan_modified_items_task_queue;
	platform::task_queue scan_displayed_items_task_queue;
	// Several threads, started on first use: they decode the stills the scan_displayed_items thread reads,
	// so that thread is never held up by a decode.
	platform::task_queue thumbnail_decode_task_queue;
	platform::task_queue crc_task_queue;
	platform::task_queue work_task_queue;
	// A running Commodore 64 holds this queue's thread for as long as it runs, so nothing else queues here.
	platform::task_queue emulator_task_queue;
	// An agent's long work: a render holds its thread for as long as it runs.
	platform::task_queue agent_task_queue;
	// Face search's scan holds the first's thread for as long as it reads; the second's few threads decode, detect
	// and embed what it read (docs/faces.md#32-the-scan); the third's makes the overview of people and suggestions.
	platform::task_queue faces_task_queue;
	platform::task_queue faces_work_task_queue;
	platform::task_queue faces_overview_task_queue;
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
	std::atomic_bool _search_index_worker_started = false;
	std::atomic_bool _map_tile_workers_started = false;
	std::atomic_bool _thumbnail_decode_workers_started = false;
	std::atomic_bool _thumbnail_stage_workers_started = false;
	std::atomic_bool _tile_db_worker_started = false;
	std::atomic_bool _media_preview_worker_started = false;
	std::atomic_bool _emulator_worker_started = false;
	std::atomic_bool _agent_worker_started = false;
	std::atomic_bool _faces_worker_started = false;
	std::atomic_bool _faces_work_workers_started = false;
	std::atomic_bool _faces_overview_worker_started = false;

	ui::frame_ptr _app_frame;
	std::shared_ptr<app_logo_element> _app_logo;

	// Shared native top-bar controls remain available independently of the primary renderer.
	ui::toolbar_ptr _navigate1;
	ui::edit_ptr _search_edit;
	ui::toolbar_ptr _navigate2;
	ui::toolbar_ptr _navigate3;

	// One part of a bar as the frame builds it from a view's declaration: a toolbar for each run of
	// commands, or, where `toolbar` is null, the frame's own control named by `control`.
	struct frame_bar_part
	{
		ui::toolbar_ptr toolbar;
		bar_control control = bar_control::item_filter;
		// The commands the toolbar shows, in order.
		std::vector<commands> run;
	};

	using frame_bar = std::vector<frame_bar_part>;

	// Keyed by the declared entries, so views that declare the same set - every task view's title bar
	// - share one set of controls. Built before the first layout and never rebuilt: the commands they
	// show update in place, and a view switch only shows and hides what already exists.
	std::map<std::vector<bar_entry>, frame_bar> _bars;

	// What the status bar had no room for: the leading commands of a run it shows in part, which the More
	// button that stands in for them lists.
	std::vector<ui::command_ptr> _bar_overflow;

	// The bar controls that are not toolbar buttons, one of each whichever views declare them.
	ui::edit_ptr _filter_edit;
	ui::trackbar_ptr _item_size;
	ui::edit_ptr _face_filter_edit;
	ui::trackbar_ptr _face_size;
	std::string _totals_text;
	// The filter as it stood when its box took focus, which Escape puts back.
	std::string _filter_edit_original;
	ui::control_ptr _focused_filter;
	bool _item_size_dragging = false;
	bool _face_size_dragging = false;

	std::shared_ptr<ui::complete_strategy_t> _search_completes;
	ui::list_window_ptr _search_predictions_frame;

	bool _search_has_focus = false;
	search_edit_session _search_session;
	bool _app_logo_hover = false;

	// The item full screen selected on the user's behalf, so leaving full screen can undo it.
	df::item_element_ptr _full_screen_auto_selected;
	bool _view_has_focus = false;
	bool _view_controls_have_focus = false;

	// The window is a container of three regions, each with a surface of its own: the title bar, the
	// view - with its panel and strip - and the status bar.
	std::shared_ptr<bar_frame> _title_bar;
	std::shared_ptr<view_frame> _view_frame;
	std::shared_ptr<bar_frame> _status_bar;
	std::shared_ptr<view_frame> _selector_frame;
	std::shared_ptr<ui::elements> _regions;
	std::shared_ptr<ui::frame_element> _title_region;
	std::shared_ptr<ui::frame_element> _body_region;
	std::shared_ptr<ui::frame_element> _status_region;

	std::shared_ptr<rename_view> _view_rename;
	std::shared_ptr<batch_tool_view> _view_batch;
	std::shared_ptr<import_view> _view_import;
	std::shared_ptr<locate_view> _view_locate;
	std::shared_ptr<sync_view> _view_sync;
	std::shared_ptr<tags_view> _view_tags;
	std::shared_ptr<options_view> _view_options;
	std::shared_ptr<explore_view> _view_explore;
	std::shared_ptr<faces_view> _view_faces;
	// The grouping face search published that a listing it answers and the sidebar's counts were last brought up to.
	std::weak_ptr<const faces::overview> _faces_listed;

	std::shared_ptr<items_view> _view_items;
	std::shared_ptr<selector_view> _view_selector;
	std::shared_ptr<edit_view> _view_edit;
	std::shared_ptr<movie_view> _view_movie;
	std::shared_ptr<media_view> _view_media;
	std::shared_ptr<view_base> _view;

	ui::bubble_window_ptr _bubble;
	pointer_dwell _tooltip_dwell;
	bool _tooltip_content_dirty = false;

	int _frame_delay = 0;
	// Previous frame_tick timestamp, for the elapsed time alpha fades advance by.
	int64_t _last_prepare_us = 0;
	lerp_animate _search_color_lerp;
	group_by _starting_group_order = group_by::file_type;
	sort_by _starting_sort_order = sort_by::def;
	std::vector<file_group_ref> _starting_media_filter;

	sizei _extent;
	// Whether the title and status bars show: not in full screen, nor while a picture is zoomed.
	bool _show_bars = true;
	// In the title bar's coordinates, the title.
	recti _title_bounds;
	// In the status bar's coordinates, the parts of it drawn rather than controls: the view's status,
	// and the item totals among the bar's controls.
	recti _status_text_bounds;
	recti _receipt_prefix_bounds;
	ui::toolbar_ptr _receipt_link;
	recti _receipt_suffix_bounds;
	std::string _receipt_prefix;
	std::string _receipt_suffix;
	int _status_text_row_height = 0;
	recti _totals_bounds;
	std::optional<recti> _last_texture_eviction_bounds;

	enum class status_part
	{
		none,
		status,
		totals
	};

	status_part _status_hover = status_part::none;
	// Whether the last paint had to cut the status short, which is when hovering it says the rest.
	bool _status_text_clipped = false;

	// Time of the most recent folder-watch notification that has not yet produced a response, and the
	// folders that signalled during that burst. UI thread only.
	double _folder_change_time = 0.0;
	core::unique_folders _folders_changed;
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

	platform::instance_receiver_ptr _instance_receiver;
	core::file_path _pending_instance_file;
	bool _forwarded_launch = false;
	bool _instance_ready = false;
	bool _instance_open_queued = false;
	// Another window held the receiver when this one asked for it: tick() asks again, so this window takes
	// over once that one has closed.
	bool _instance_claim_retry = false;
	double _next_instance_claim = 0.0;
	double _next_face_book_poll = 0.0;
	bool reuse_running_instance(bool forward_file = true);
	bool accept_instance_files(std::vector<core::file_path> files);
	// Whether the view on screen gives way to another without asking. One running a task refuses, and Edit or Options
	// holding changes asks first, so a file sent by another launch is declined - and opens in a window of its own -
	// rather than accepted and then dropped when the task refuses or the user chooses to stay.
	bool view_gives_way() const;
	void open_pending_instance_file();

	// The window's recording, which record_start or a tutorial starts; null while nothing records.
	std::unique_ptr<screen_recording> _recording;
	// Told what a recording that ends by itself - its length written, or a failure - has written.
	std::function<void(std::string_view summary)> _recording_ended;
	// The script being acted out - tutorial_run's, or ui_input's steps - which starts and ends its recording itself.
	std::unique_ptr<tutorial_player> _tutorial;

	// An agent session's host (app_mcp.h), or a -tool: call's; null for an ordinary run.
	std::shared_ptr<mcp_host> _mcp;
	// Whether the window and the index have been made: by init, or for an agent session when a tool first needs them.
	bool _window_started = false;
	// Held while this process has its profile open, so an agent session can tell it is not alone with it.
	platform::instance_lock_ptr _profile_lock;

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

	app_frame();
	~app_frame() override;

	void free_graphics_resources(bool items_only, bool offscreen_only) override;
	void track_menu(const ui::frame_ptr& parent, recti bounds, const std::vector<ui::command_ptr>& commands) override;
	void idle() override;
	void hide_search_predictions();
	bool key_down(char32_t key, platform::key_state keys) override;
	bool text_input(std::string_view text) override;
	ui::focus_mode focus_mode() const override;
	void create_toolbars();

	// The bar the frame shows for a view's declared entries, built on first use.
	const frame_bar& bar(const std::vector<bar_entry>& entries);
	// The toolbar a run of commands shows when the bar has room for only its last `keep`: the More button, whose
	// menu holds the rest, then those. Built once for each length and kept, as every bar is.
	const ui::toolbar_ptr& trimmed_run(const std::vector<commands>& run, size_t keep);
	// What a running task's status bar offers in place of its view's own commands.
	static const std::vector<bar_entry>& running_task_bar();
	std::vector<bar_slot> measure_bar(render::measure_context& mc, const frame_bar& b, int control_height) const;
	void draw_status_bar(render::draw_context& dc);
	void status_tooltip(view_hover_element& hover) const;
	void update_status_hover(pointi loc);
	bool filter_key_down(char32_t key, platform::key_state keys);
	void filter_text_changed(std::string_view text);
	void focus_filter(const ui::control_ptr& edit, bool has_focus);
	void focus_filter_edit();
	void cancel_filter_edit();

	// One place Tab stops at, in the window's order: a control, which takes the focus itself; a group of drawn parts
	// in the view's frame, entered at its checked part or else its first; the strip; or, with none of these, the
	// view's own content.
	struct keyboard_stop
	{
		ui::control_ptr control;
		std::vector<keyboard_part> parts;
		keyboard_area area = keyboard_area::view;
		bool strip = false;
	};

	// Where Tab stops, in order: the title bar's controls; the view's sidebar, its own content, the links of its
	// listing and its media pane, the strip, and each control and link of its panel; then the status bar's controls.
	std::vector<keyboard_stop> keyboard_stops() const;
	// The one of them that has the keyboard, or -1.
	int current_keyboard_stop(const std::vector<keyboard_stop>& stops);
	// Tab and Shift+Tab: the keyboard to the next stop or the one before, round the window.
	void move_keyboard(bool forward);
	void keyboard_to(const keyboard_stop& stop);
	// The control that has the keyboard, in whichever of the window's frames holds it.
	ui::control_ptr keyboard_control() const;
	// Apps and Shift+F10: the menu a right-click on what has the keyboard opens, beside it.
	void open_keyboard_menu();
	void item_size_changed(int pos, bool tracking);
	void update_item_size_slider() const;
	void crash(core::file_path dump_file_path) override;
	std::string restart_command_line() override;
	void save_recovery_state() override;
	void invalidate_view(view_invalid invalid) override;
	void invoke(commands id) override;
	void invoke(const command_info_ptr& c);
	void toggle_full_screen() override;
	bool can_open_search(const df::search_t& path) override;
	void report_scope_unavailable(const df::search_t& path) override;
	void folder_changed(core::folder_path folder) override;
	void dpi_changed() override;
	void on_window_layout(render::measure_context& mc, sizei extent, bool is_minimized) override;
	void on_window_paint(render::draw_context& dc) override;
	void visit_elements(const std::function<void(const view_element_ptr&, pointi)>& visit) override;

	// The bars, each in its own coordinates.
	void layout_title_bar(render::measure_context& mc, sizei extent, std::vector<ui::control_ptr>& shown);
	void layout_status_bar(render::measure_context& mc, sizei extent, std::vector<ui::control_ptr>& shown);
	void paint_title_bar(render::draw_context& dc);
	void title_bar_mouse_move(pointi loc);
	void title_bar_mouse_leave();
	void title_bar_click(pointi loc);
	int title_bar_height(render::measure_context& mc);
	int status_bar_height(render::measure_context& mc, int width);
	void activate(bool is_active) override;
	void fail(std::string_view message, std::string_view more_text) override;
	void invalidate_status() const;
	void update_overlay();
	void tick() override;
#ifdef _DEBUG
	void run_test_action(std::string_view action);
#endif
	// -profile's and -collection's settings, and an agent session's, put over what was read: after each read of the
	// settings.
	void apply_run_settings();
	// Starts recording the window, logging that it has. Why it cannot, where it cannot; empty once it records.
	std::string start_recording(const screen_recording_request& request);
	// Ends the recording and finishes its file: what was written, or why nothing was.
	std::string finish_recording();
	void frame_tick() override;
	void update_tooltip();
	void update_tooltip_visibility(uint64_t now_ms, pointi screen_position, double scale_factor);
	void item_focus_changed(const df::item_element_ptr& focus, const df::item_element_ptr& previous) override;

	// One test decides whether a view shows the selector strip, what it offers and what a click on it
	// does, so the three cannot disagree.
	enum class selector_strip
	{
		none,
		edit,
		metadata
	};

	selector_strip selector_strip_for_view(view_type m) const;
	void select_from_selector(const df::item_element_ptr& item, platform::key_state keys);
	void reset_selector_selection_anchor();
	void make_visible(const df::item_element_ptr& i) override;
	bool is_command_checked(commands cmd) override;
	void element_broadcast(const view_element_event& event) override;
	recti calc_search_popup_bounds() const;
	void layout(render::measure_context& mc);
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
	// Shows the app in a language: English, or a translation installed beside the app.
	void select_language(std::string_view lang_code);
	void update_command_text();
	void initialise_commands();
	command_info_ptr find_or_create_command_info(commands id);
	void add_command_invoke(commands id, std::function<void()> invoke);
	ui::command_ptr find_command(commands id) const override;
	void tooltip(view_hover_element& hover, commands id) const;
	commands close_shortcut_target() const;
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
	std::shared_ptr<core::mcp::server> agent_server() override;
	// Makes the window and starts the index and its workers: the rest of init, which an agent session puts off until a
	// tool first needs them. False, with why, where the window cannot be made.
	bool start_window(std::string& why);
	bool wants_gpu() const override;
	void start_workers();
	void start_index_workers();
	bool claim_worker_start(std::atomic_bool& started);
	void ensure_worker(std::atomic_bool& started, platform::task_queue& q, std::string_view name,
	                   bool background_work = true);
	void update_font_size() const;
	bool init(std::string_view command_line) override;
	void init_search();
	void final_exit() override;
	void exit() override;
	void system_event(platform::os_event_type ost) override;
	void search_enter();
	void cancel_search_edit();
	bool search_accept_selected();

	void on_mouse_wheel(const pointi /*loc*/, const int /*delta*/, const platform::key_state /*keys*/,
	                    bool& /*was_handled*/) override
	{
	}

	void on_mouse_left_button_double_click(const pointi /*loc*/, const platform::key_state /*keys*/) override {}

	void on_mouse_other_button_up(const platform::other_mouse_button& /*button*/, const pointi /*loc*/,
	                              const platform::key_state /*keys*/) override
	{
	}

	void pan_start(const pointi /*start_loc*/) override {}

	void pan(const pointi /*start_loc*/, const pointi /*current_loc*/) override {}

	void pan_end(const pointi /*start_loc*/, const pointi /*final_loc*/) override {}

	void focus_search(bool has_focus);
	void focus_changed(bool has_focus, const ui::control_ptr& child) override;
	void on_window_destroy() override;
	void command_hover(const ui::command_ptr& c, recti window_bounds) override;

	// loc is a point on the screen, as every frame's menu is asked for.
	std::vector<ui::command_ptr> menu(pointi loc) override;
	// What a right-click at a point on the screen offers, before it is built.
	view_menu menu_at(pointi screen);

	// A menu a view names, built: its row of buttons, then the rest.
	std::vector<ui::command_ptr> context_menu(const view_menu& menu) const;
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

				if (platform::exists(path))
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
