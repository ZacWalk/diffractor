// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The application frame. Startup and shutdown, settings load and save, the frame layout and
// the status bar it paints, draining view invalidation in dependency order, keyboard routing and the
// order Tab visits the window in, switching views, update checks and crash reporting, and in a Debug build
// the screenshot scenes and the window's recording. Command behavior, toolbars, workers and the address box
// live in app_commands.cpp, app_toolbar.cpp, app_workers.cpp and app_search.cpp.

#include "pch.h"

#ifdef _WIN32
#include "libdesktop/platform/win32.h"
#endif

#include "app_text.h"
#include "util_zip.h"
#include "crypto_sha.h"

#include "model_location.h"
#include "model_db.h"
#include "model_faces_search.h"
#include "model_index.h"
#include "model.h"

#include "ui_dialog.h"
#include "util_crash_files_db.h"
#include "libdesktop/core/log.h"
#include "files.h"
#include "av_format.h"
#include "view_items.h"
#include "view_edit.h"
#include "view_movie.h"
#include "view_media.h"
#include "ui_sid.h"
#include "ui_tracker.h"
#include "ui_c64.h"
#include "view_selector.h"
#include "view_import.h"
#include "view_locate.h"
#include "view_rename.h"
#include "view_batch.h"
#include "view_sync.h"
#include "view_tags.h"
#include "view_options.h"
#include "view_explore.h"
#include "view_faces.h"

#include "app_sidebar.h"
#include "app_commands.h"
#include "app_dialogs.h"
#include "app_command_line.h"
#include "app.h"

#include <cstdio>
#include <utility>

#include "app_command_status.h"
#include "app_mcp.h"
#include "util_spell.h"

std::atomic_int df::file_handles_detached = 0;
std::atomic_int df::jobs_running = 0;
std::atomic_int df::loading_media = 0;
std::atomic_int df::dragging_items = 0;
std::atomic_int df::handling_crash = 0;
std::atomic<core::file_path> df::last_loaded_path;
std::atomic_bool df::network_allowed = true;
const std::string_view s_app_version = "127.4";
const std::string_view g_app_build = "1316";

extern const unsigned char diffractor_resource_icon_font[];
extern const unsigned long diffractor_resource_icon_font_size;

void register_diffractor_icon_font()
{
	render::set_icon_font({diffractor_resource_icon_font, static_cast<size_t>(diffractor_resource_icon_font_size)});
}
static constexpr auto s_search = "search";

extern void start_worker(platform::task_queue& q, std::string_view name, bool background_work = true);

std::string df::format_version(const bool short_text)
{
	if (short_text)
	{
		return std::format("{}.{}", s_app_version, g_app_build);
	}

	return std::format("{}: {}.{}  |  {}", tt.version, s_app_version, g_app_build, core::utf8_cast(__DATE__));
};

bool is_app_installed()
{
	const auto running_folder = platform::running_app_path().folder();
	const auto install_folder = known_path(platform::known_folder::app_data);
	return running_folder == install_folder;
}

struct app_updates_and_location_params
{
	gps_coordinate gps = setting.default_location;
	std::string version;
	std::string test_version = setting.available_test_version;

	void apply(const app_frame_ptr& app, const view_state& s) const
	{
		core::assert_true(platform::is_ui_thread());

		setting.default_location = gps;
		setting.available_version = version;
		setting.available_test_version = test_version;
		// settings_t is plain UI-owned state, so the report's own side effect is applied here rather
		// than on the web worker that observed it.
		setting.first_run_ever = false;

		s.invalidate_view(view_invalid::app_layout);

		app->save_options(true);
	}
};

static gps_coordinate parse_coordinates(const std::string_view text, const gps_coordinate def_coords)
{
	const auto splits = core::split(text, true);

	if (splits.size() == 2)
	{
		const auto latitude = splits[0];
		const auto longitude = splits[1];

		if (!longitude.empty() && !latitude.empty())
		{
			return {core::to_double(latitude), core::to_double(longitude)};
		}
	}

	return def_coords;
}

// Runs on a shared worker thread (which already owns its own name and COM
// initialisation) and only queues the actual network request, so it must not
// rename the thread or re-initialise it.
static void check_for_updates_and_location([[maybe_unused]] const app_frame_ptr& app, view_state& s)
{
	log_func lf(__FUNCTION__);

	spell().configure_async(&s._async);

	// An agent session reaches the network only where it was started with -mcp-allow:network.
	if (platform::is_online() && df::network_allowed)
	{
#ifndef WINSTORE
		if (setting.first_run_today && setting.check_for_updates)
		{
			s.queue_async(async_queue::web, [app, &s]
			{
				const auto reported_features = features_used_since_last_report();

				platform::web_request req;
				req.path = "/ver";
				req.query = platform::web_params{
					{"v"s, std::string(s_app_version)},
					{"b"s, std::string(g_app_build)},
					{"f"s, setting.first_run_ever ? "1"s : "0"s},
					{"ft"s, core::to_hex(reported_features)},
					{"i"s, core::to_hex(setting.instantiations)},
					{"os"s, platform::OS()},
					// The packed platform identity. It ships alongside `os` for one release rather
					// than replacing it on the same commit, so the ingest can change without a gap
					// in the daily series. It is its own parameter and never merged into `ft`: a
					// feature bit means "at least once since the last report" and is cleared once
					// sent, a system value means "right now" and is recomputed every time, and
					// OR-accumulating an architecture would make anyone who once ran the 32-bit
					// build look like a permanent 32-bit user.
					{"s"s, core::to_hex(platform::pack_environment(platform::environment()))},
				};

				const auto con = platform::connect_to_host("diffractor.com");
				const auto response = send_request(con, req);

				if (response.status_code == 200)
				{
					core::json::json_doc json;
					json.Parse(response.body);

					app_updates_and_location_params params;
					params.version = core::json::safe_string(json, "current_version");
					params.test_version = core::json::safe_string(json, "test_version");
					params.gps = parse_coordinates(core::json::safe_string(json, "latlon"), params.gps);

					clear_reported_feature_use(reported_features);

					s.queue_ui([params, app, &s] { params.apply(app, s); });
				}
			});
		}
#endif
		spell().lazy_download(s._async);
	}
}

std::vector<std::pair<std::string_view, std::string>> calc_app_info(const index_state& index, const bool include_state)
{
	std::vector<std::pair<std::string_view, std::string>> result;
	auto arch = "32-bit";
	auto config = "release";

#ifdef _M_X64
	arch = "64-bit";
#endif

#ifdef _DEBUG
	config = "debug";
#endif //_DEBUG

	const auto seconds_running = core::utc_now().to_seconds() - ui::start_time.to_seconds();

	result.reserve(include_state ? 25_z : 19_z);
	result.emplace_back("Version:", df::format_version(true));
	result.emplace_back("Windows:", std::format("{} {} {}", platform::OS(), arch, config));
	result.emplace_back("Id:", core::to_hex(core::crc32c(platform::user_name()), false));
	result.emplace_back("Running:", std::format("{} seconds", seconds_running));
	platform::memory_usage_t mem;

	if (platform::memory_usage(mem))
	{
		// Private working set first: it is what Task Manager's Memory column reports, so a user
		// comparing the two sees the same number. Shared pages are mapped data files, which cost
		// this process nothing private.
		result.emplace_back("Memory:",
		                    std::format("{} (commit {}, shared {})", core::file_size(mem.private_working_set),
		                                core::file_size(mem.commit), core::file_size(mem.shared_working_set)));
		result.emplace_back("Peak memory:", platform::format_size(core::file_size(mem.peak_working_set)));
	}

	result.emplace_back("Static Memory:", platform::format_size(core::file_size(
											  core::static_memory_usage.load(std::memory_order_relaxed))));

	result.emplace_back("GPU:", render::gpu_desc);
	result.emplace_back("GPU Id:", render::gpu_id);
	result.emplace_back("D3D:", render::d3d_info);

	result.emplace_back("Indexed items:", core::to_string(index.stats.index_item_count));
	result.emplace_back("Indexed folders:", core::to_string(index.stats.index_folder_count));
	result.emplace_back("Duplicates:", std::format("g={} mcomp={}", index.stats.indexed_dup_folder_count,
	                                               index.stats.indexed_max_compare_count));
	result.emplace_back("Hashes:",
	                    std::format("crc={} phash={} declined={} uninvited={}", index.stats.indexed_crc_count,
	                                index.stats.indexed_phash_count, index.stats.indexed_phash_declined_count,
	                                index.stats.indexed_phash_uninvited_count));
	result.emplace_back("DB size:", platform::format_size(index.stats.database_size));
	result.emplace_back("Saved:",
	                    std::format("{} items | {} thumbs", index.stats.items_saved, index.stats.thumbs_saved));

	result.emplace_back("Index load:", std::format("{} ms", index.stats.index_load_ms));
	result.emplace_back("Predictions:", std::format("{} ms", index.stats.predictions_ms));
	result.emplace_back("Count Matches:", std::format("{} ms", index.stats.count_matches_ms));

	if (include_state)
	{
		result.emplace_back("Jobs running: ", core::to_string(df::jobs_running));
		result.emplace_back("Is indexing: ", core::to_string(index.indexing));
		result.emplace_back("Is searching: ", core::to_string(index.searching));
		result.emplace_back("Is command active: ", core::to_string(ui::command_active));
		result.emplace_back("Is closing: ", core::to_string(platform::is_closing));
		result.emplace_back("Rendering function: ",
		                    core::utf8_cast(render::rendering_func.load(std::memory_order_relaxed)));
	}

	return result;
}

void app_logo_element::tooltip(view_hover_element& hover, const pointi /*loc*/, const pointi element_offset) const
{
	hover.elements->add(std::make_shared<text_element>(_text, render::font_face::title, render::text_style::single_line,
	                                                   flex_item::line_break));
	hover.elements->add(std::make_shared<text_element>(df::format_version(false), render::font_face::dialog,
	                                                   render::text_style::single_line, flex_item::line_break));

	if (setting.show_debug_info)
	{
		const auto table = std::make_shared<ui::table_element>(flex_item::center);

		for (const auto& i : calc_app_info(_state.item_index, true))
		{
			table->add(i.first, i.second);
		}

		hover.elements->add(table);
	}

	if (_interactive)
	{
		hover.elements->add(std::make_shared<text_element>(tt.help_more_info));
		add_shortcut_hint(hover, _state.find_command(commands::view_help));
	}
	hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
}

view_element_ptr create_app_logo_element(view_state& s, const render::font_face font, const bool interactive,
                                         const bool show_plasma, const double logo_scale,
                                         const view_element_options& options)
{
	return std::make_shared<app_logo_element>(s, font, interactive, show_plasma, logo_scale, options);
}

void app_frame::fail(const std::string_view message, const std::string_view more_text)
{
	auto message_s = std::string(message);
	auto more_text_s = std::string(more_text);

	// A failure during startup has no window to parent a dialog and no message loop to run one:
	// wWinMain returns as soon as init() fails, so a queued dialog would never be shown and the
	// process would simply disappear. Say it synchronously instead. Restricted to the UI thread so
	// a worker reporting a failure after the window has gone still just queues and is dropped.
	if (!_app_frame && platform::is_ui_thread())
	{
		auto text = message_s;
		if (!more_text_s.empty()) text += "\n\n" + more_text_s;
		platform::show_startup_failure(text);
		return;
	}

	queue_ui([message_s, more_text_s, parent = _app_frame]
	{
		const auto dlg = make_dlg(parent);

		std::vector<view_element_ptr> controls;
		controls.emplace_back(
			set_margin(std::make_shared<ui::title_control>(icon_index::error, tt.platform_text.title_error)));
		controls.emplace_back(set_margin(std::make_shared<text_element>(tt.platform_text.error_cannot_continue)));
		if (!message_s.empty()) controls.emplace_back(set_margin(std::make_shared<text_element>(message_s)));
		if (!more_text_s.empty()) controls.emplace_back(set_margin(std::make_shared<text_element>(more_text_s)));
		controls.emplace_back(std::make_shared<ui::close_control>(dlg->_frame));

		dlg->show_modal(controls);

		if (parent)
		{
			parent->close();
		}
	});
}

void media_view::update_media_elements()
{
	core::assert_true(platform::is_ui_thread());
	const auto display = _state.display_state();
	_display = display;

	// The same presentation the items view shows. A picture has the screen, with the panel floating over
	// it; anything else is the items view's stack of panels, with the compact panel and its description.
	auto presentation = make_media_presentation(_state, display, _host, true);
	_media_element = presentation.media;
	_is_stack = display && !presentation.picture;
	_controls_element = _state.create_selection_controls(true);
	_description_element = _state.create_selection_description();
	_stack.clear();

	if (_is_stack)
	{
		_stack = std::move(presentation.above);
		if (_controls_element && !_controls_element->is_empty()) _stack.emplace_back(_controls_element);

		if (_description_element)
		{
			// Prose can run on, and the stack does not scroll: the panel clips what does not fit.
			_description_element->flex.max_size.cy = stack_description_max_height;
			_stack.emplace_back(_description_element);
		}
	}
	else
	{
		if (_controls_element)
		{
			_controls_element->set_style_bit(view_element_style::dark_background, true);
		}
		if (_description_element)
		{
			_description_element->set_style_bit(view_element_style::dark_background, true);
		}
	}
}

// Case-insensitive exact match for a switch name.
static bool is_switch(const std::string_view op, const std::string_view name)
{
	return core::icmp(op, name) == 0;
}

// Case-insensitive match for "<prefix><value>" where value is non-empty.
static bool switch_value(const std::string_view op, const std::string_view prefix, std::string_view& value)
{
	if (op.size() > prefix.size() && core::icmp(op.substr(0, prefix.size()), prefix) == 0)
	{
		value = op.substr(prefix.size());
		return true;
	}

	return false;
}

void command_line_t::parse(const std::string_view command_line_text)
{
	// core::split with detect_quotes strips the surrounding quotes, so a quoted
	// path containing spaces arrives here as a single part.
	for (const auto part : core::split(core::trim(command_line_text), true, core::is_white_space))
	{
		if (part.empty())
		{
			continue;
		}

		// A leading '/' introduces a switch only where it cannot begin an absolute path. Everywhere
		// else '/home/zac' would be read as a switch and never reach the path branch below.
		const auto starts_switch = part[0] == '-' || (core::windows_path_semantics && part[0] == '/');

		// After -tool:<name>, what is not a switch is one of its arguments: name=value.
		if (!tool.empty() && !starts_switch)
		{
			tool_arguments.emplace_back(part);
			continue;
		}

		if (starts_switch && part.size() > 1)
		{
			const auto op = part.substr(part[1] == '-' ? 2 : 1);
			std::string_view value;

			if (is_switch(op, "no-gpu")) no_gpu = true;
			else if (is_switch(op, "no-indexing")) no_indexing = true;
			else if (is_switch(op, "new-window")) new_window = true;
			else if (is_switch(op, "test")) console_test = true;
			else if (switch_value(op, "test:", value))
			{
				console_test = true;
				test_filter = value;
			}
			else if (switch_value(op, "test-temp:", value)) test_temp_folder = value;
			else if (is_switch(op, "validate-po")) validate_po = true;
			else if (is_switch(op, "mcp")) agent = true;
			else if (switch_value(op, "mcp-ui:", value)) agent_ui = value;
			else if (switch_value(op, "mcp-allow:", value)) agent_allow = value;
			else if (switch_value(op, "tool:", value)) tool = value;
			else if (switch_value(op, "profile:", value)) profile_folder = value;
			else if (switch_value(op, "collection:", value)) collection_folder = value;
		}
		else if (part.ends_with("**"))
		{
			// A trailing "**" asks for the folder with its subfolders, as it does in the address box.
			// AutoPlay opens an inserted card that way, and may name a drive without its separator.
			auto text = std::string(part);
			if (core::windows_path_semantics && text.size() == 4 && text[1] == ':')
				text.insert(2, 1, core::preferred_path_sep);

			if (core::is_path(text))
			{
				const core::item_selector selector(text);

				if (selector.is_recursive() && !selector.has_wildcard() && platform::exists(selector.folder()))
				{
					folder_path = selector;
					selection = {};
				}
			}
		}
		else if (core::is_path(part))
		{
			const auto folder = core::folder_path(part);

			if (platform::exists(folder))
			{
				folder_path = folder;
				selection = {};
			}
			else
			{
				const auto file = core::file_path(part);

				if (platform::exists(file))
				{
					folder_path = file.folder();
					selection = file;
				}
			}
		}
	}
}

std::string command_line_t::format_restart_cmd_line() const
{
	std::string result;
	if (no_gpu) result += " -no-gpu";
	if (no_indexing) result += " -no-indexing";
	if (new_window) result += " -new-window";
	return result;
}

bool command_line_t::can_forward_file() const
{
	return !new_window && !selection.is_empty() && !no_gpu && !no_indexing && !console_test && !validate_po && !agent &&
	       tool.empty() && profile_folder.empty() && collection_folder.empty();
}

void view_frame::update_status(const std::string_view title, const std::string_view text, const int padding)
{
	if (_status_title != title || _status_text != text || _status_padding != padding)
	{
		_status_title = title;
		_status_text = text;
		_status_padding = padding;
		_state.invalidate_view(view_invalid::view_redraw);
	}
}

void view_frame::clear_status()
{
	update_status({}, {});
}

void view_frame::draw_status(render::draw_context& dc) const
{
	if (!_status_title.empty() || !_status_text.empty())
	{
		constexpr auto title_font = render::font_face::title;
		constexpr auto text_font = render::font_face::dialog;

		const auto text_color = core::color(dc.colors.foreground, dc.colors.alpha);
		const auto bg_color = core::color(ui::style::colors.important_background, dc.colors.alpha);
		const sizei avail_extent{_view_bounds.width() / 2, _view_bounds.height() / 2};

		const auto title_extent = dc.measure_text(_status_title, title_font, render::text_style::multiline_center,
		                                          avail_extent.cx);
		const auto text_extent = dc.measure_text(_status_text, text_font, render::text_style::multiline_center,
		                                         avail_extent.cx);
		const auto text_spacing = !_status_title.empty() && !_status_text.empty() ? _status_padding : 0;

		const auto extent = sizei(std::max(title_extent.cx, text_extent.cx),
		                          title_extent.cy + text_extent.cy + text_spacing);
		const auto bounds = center_rect(extent, _view_bounds);

		auto title_bounds = bounds;
		title_bounds.bottom = title_bounds.top + title_extent.cy;
		auto text_bounds = bounds;
		text_bounds.top = text_bounds.bottom - text_extent.cy;

		dc.draw_rect(bounds.inflate(_status_padding), bg_color);
		dc.draw_text(_status_title, title_bounds, title_font, render::text_style::multiline_center, text_color, {});
		dc.draw_text(_status_text, text_bounds, text_font, render::text_style::multiline_center, text_color, {});
	}
}

shell_file_operation_ui::shell_file_operation_ui(view_frame& view, ui::frame_ptr main_frame) :
	_view(view), _main_frame(std::move(main_frame))
{
	_view.update_status(tt.processing_files, {}, 20);
	_view.redraw_now();
	_main_frame->enable(false);
}

shell_file_operation_ui::~shell_file_operation_ui()
{
	_main_frame->enable(true);
	_view.clear_status();
}

// Both panes stay usable, so a splitter drag can never collapse either one out of reach.
static int clamp_pane_width(const int width, const int available, const int min_pane)
{
	const auto lo = std::min(min_pane, available / 2);
	return std::clamp(width, lo, std::max(lo, available - min_pane));
}

// The strip between a view and its panel: dragged, it shares the width between them, and the width the
// panel took is kept for the view. Internal to this file: the Items view has a splitter_controller of its own,
// and two classes of one name in two files are one class to the linker, which keeps one vtable for both.
namespace
{
	class panel_splitter_controller final : public view_controller
	{
		std::shared_ptr<view_frame> _view;

	public:
		explicit panel_splitter_controller(const std::shared_ptr<view_frame>& view) :
			view_controller(view, view->_splitter.bounds), _view(view)
		{
		}

		~panel_splitter_controller() override
		{
			_view->_splitter.tracking = false;
			_view->frame()->invalidate(_view->_splitter.bounds);
		}

		platform::cursor cursor() const override { return platform::cursor::left_right; }

		void draw(render::draw_context& dc) override
		{
			const auto& s = _view->_splitter;
			draw_splitter_handle(dc, s.bounds, s.width, true, s.tracking);
		}

		void on_mouse_left_button_down(const pointi loc, const platform::key_state /*keys*/) override
		{
			_view->_splitter.tracking = true;
			_view->drag_splitter(loc);
		}

		void on_mouse_move(const pointi loc) override
		{
			if (_view->_splitter.tracking) _view->drag_splitter(loc);
		}

		void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
		{
			if (!_view->_splitter.tracking) return;

			_view->drag_splitter(loc);
			_view->_splitter.tracking = false;
			_view->frame()->invalidate(_view->_splitter.bounds);
			_view->invalidate_view(view_invalid::options_save);
		}
	};
}

void view_frame::panel(view_controls_host_ptr panel, const bool fills)
{
	if (_panel == panel && _panel_fills == fills) return;

	_panel = std::move(panel);
	_panel_fills = fills;
	if (_panel) _panel->_frame = frame();
	_splitter.tracking = false;
}

bool view_frame::in_panel(const pointi loc) const
{
	return _panel && _panel->_bounds.contains(loc);
}

void view_frame::drag_splitter(const pointi loc)
{
	const auto& s = _splitter;
	const auto available = std::max(0, _extent.cx - s.width);
	if (available <= 0) return;

	// The pointer holds the middle of the strip, so the grab point does not jump on the first move.
	const auto panel_width = clamp_pane_width(_extent.cx - loc.x - s.width / 2, available, s.min_pane);
	const auto pos = std::max(1, core::mul_div(panel_width, settings_t::view_splitter_max, available));

	if (setting.set_view_splitter(_state.view_mode(), pos))
	{
		// Direct pointer input, so lay out and repaint inside this message. Deferring to idle would
		// paint the strip at its new position while the panes are still at the old one. The setting
		// is written once the drag ends rather than on every move.
		frame()->layout();
		frame()->redraw_now();
	}
}

// The view takes what the panel, the splitter and the strip leave: the panel the share of the width the
// view keeps for it - all of it when it is the whole frame - and the strip a band along the base.
void view_frame::on_window_layout(render::measure_context& mc, const sizei extent, const bool /*is_minimized*/)
{
	const auto resized = extent != _extent;
	_extent = extent;

	const auto scale_factor = mc.scale_factor;
	const auto has_panel = _panel != nullptr;
	const auto fills = has_panel && _panel_fills;
	const auto splitter_width = has_panel && !fills ? core::mul_div(mc.scroll_width, 3, 5) : 0;
	const auto min_pane = core::round(200 * scale_factor);
	const auto available = std::max(0, extent.cx - splitter_width);
	const auto stored = setting.view_splitter(_state.view_mode());
	// Faces has a group list as well as the inspector. Leave the largest share for the faces themselves.
	const auto default_width = _state.view_mode() == view_type::faces
	                               ? std::clamp(available / 4, core::round(280 * scale_factor),
	                                            core::round(360 * scale_factor))
	                               : std::max(extent.cx / 4, core::round(400 * scale_factor));
	const auto preferred = stored > 0 ? core::mul_div(available, stored, settings_t::view_splitter_max) : default_width;
	const auto panel_width = fills ? extent.cx : has_panel ? clamp_pane_width(preferred, available, min_pane) : 0;
	const auto content_right = std::max(0, extent.cx - panel_width - splitter_width);
	const auto strip_height = _strip_wanted ? std::min(core::round(150 * scale_factor), extent.cy / 3) : 0;

	_view_bounds = recti(0, 0, content_right, extent.cy - strip_height);
	_strip->bounds = recti(0, extent.cy - strip_height, content_right, extent.cy);
	_strip->is_visible(strip_height > 0 && content_right > 0);
	_splitter.bounds = splitter_width > 0 ? recti(content_right, 0, content_right + splitter_width, extent.cy)
	                                      : recti{};
	_splitter.width = splitter_width;
	_splitter.min_pane = min_pane;

	_view->layout(mc, _view_bounds.extent());
	if (_panel) _panel->layout_panel(mc, recti(content_right + splitter_width, 0, extent.cx, extent.cy));
	if (resized && _state.view_mode() == view_type::faces && _active_controller)
		_state.invalidate_view(view_invalid::controller | view_invalid::tooltip);
}

void view_frame::on_window_paint(render::draw_context& dc)
{
	dc.time_now = core::now();
	dc.colors.alpha = 1.0;
	dc.colors.overlay_alpha = 1.0;

	if (!_view_bounds.is_empty())
	{
		render::scoped_clip clip(dc, _view_bounds);
		_view->render(dc, _active_controller);
	}

	if (_panel) _panel->render_panel(dc);

	if (!_splitter.bounds.is_empty())
	{
		// In the panel's colour, so the strip reads as part of it until the user touches it.
		dc.draw_rect(_splitter.bounds, core::color(ui::style::colors.dialog_background, dc.colors.alpha));
	}

	if (_active_controller)
	{
		_active_controller->draw(dc);
	}

	draw_keyboard_focus(dc);
	draw_status(dc);

	if (setting.show_debug_info && _active_controller)
	{
		const auto c = core::color(1.0f, 0.0f, 0.0f, 1.0f);
		const auto pad = core::round(2 * dc.scale_factor);
		dc.draw_border(_controller_bounds, _controller_bounds.inflate(pad), c, c);
	}
}

void view_frame::visit_elements(const std::function<void(const view_element_ptr&, pointi)>& visit)
{
	visit(_strip, {});
	if (_panel) _panel->visit_elements(visit);
}

std::vector<keyboard_region> view_frame::keyboard_regions() const
{
	auto regions = _view->keyboard_regions();

	if (_panel && !_panel->_bounds.is_empty())
	{
		const auto panel = _panel;
		regions.push_back({keyboard_area::panel, panel->_controls, panel->elements_offset(), panel->_bounds,
		                   [panel](const recti r) { panel->scroll_into_view(r.offset(-panel->elements_offset())); }});
	}

	return regions;
}

std::optional<keyboard_region> view_frame::keyboard_region_of(const keyboard_area area) const
{
	for (auto& region : keyboard_regions())
	{
		if (region.area == area) return std::move(region);
	}

	return {};
}

std::vector<keyboard_part> view_frame::keyboard_parts(const keyboard_area area) const
{
	const auto region = keyboard_region_of(area);
	return region ? find_keyboard_parts(region->roots, region->offset) : std::vector<keyboard_part>{};
}

std::optional<keyboard_part> view_frame::focused_part()
{
	if (_keyboard.area == keyboard_area::view) return {};

	const auto parts = keyboard_parts(_keyboard.area);
	const auto element = _keyboard.element.lock();
	const auto found = std::ranges::find_if(parts, [&](const keyboard_part& part)
	{ return part.element == element && part.index == _keyboard.index; });

	if (found != parts.end())
	{
		// It outlived its press, so a later rebuild - counts arriving long after - is followed like any other.
		_keyboard.pressed = false;
		_keyboard.bounds = found->bounds;
		return *found;
	}

	// Rebuilt - the sidebar counted again, the pane followed the selection - so the part now nearest where it was
	// takes its place. A part its own press took away at once is not followed: what the press did has moved the
	// user on.
	if (_keyboard.pressed || parts.empty()) return {};

	const auto was = _keyboard.bounds.center();
	const auto nearest = std::ranges::min_element(parts, {}, [was](const keyboard_part& part)
	{
		const auto at = part.bounds.center();
		return std::abs(at.x - was.x) + std::abs(at.y - was.y);
	});

	_keyboard.element = nearest->element;
	_keyboard.index = nearest->index;
	_keyboard.bounds = nearest->bounds;
	return *nearest;
}

void view_frame::keyboard_to(const keyboard_area area, const keyboard_part* part)
{
	const auto was_on_view = _keyboard.area == keyboard_area::view;

	if (part && area != keyboard_area::view)
	{
		_keyboard = {area, part->element, part->index, part->bounds, false};
		if (const auto region = keyboard_region_of(area); region && region->reveal) region->reveal(part->bounds);
	}
	else
	{
		_keyboard = {};
	}

	frame()->invalidate();

	// The view's own content has just taken the keyboard, or let it go to a part.
	if (was_on_view != (_keyboard.area == keyboard_area::view))
	{
		const auto f = frame();
		focus_changed(f->has_focus(), f->focused_control());
	}
}

bool view_frame::keyboard_part_key_down(const char32_t key, const platform::key_state keys)
{
	if (_keyboard.area == keyboard_area::view || keys.alt) return false;

	const auto part = focused_part();

	if (!part)
	{
		keyboard_to(keyboard_area::view, nullptr);
		return false;
	}

	if (key == platform::keys::ESCAPE && _keyboard.area != keyboard_area::panel)
	{
		keyboard_to(keyboard_area::view, nullptr);
		return true;
	}

	if (key == platform::keys::RETURN || key == platform::keys::SPACE)
	{
		_keyboard.pressed = true;
		press_keyboard_part(shared_from_this(), *part);
		return true;
	}

	if (keys.control) return false;

	// Each link of the panel is a stop of its own, as each of its controls is; elsewhere an area's parts are one.
	auto group = keyboard_parts(_keyboard.area);
	if (_keyboard.area == keyboard_area::panel)
		std::erase_if(group, [&part](const keyboard_part& p) { return p.element != part->element; });

	const auto from = static_cast<int>(
		std::ranges::find_if(group, [&part](const keyboard_part& p) { return p.is(*part); }) - group.begin());
	const auto to = step_keyboard_part(group, from, key);
	if (to < 0) return false;

	_keyboard_cues = true;
	if (to != from) keyboard_to(_keyboard.area, &group[to]);
	return true;
}

// Where the keyboard is, where nothing else shows it: around the part it is on, and around the view's own content
// once the keyboard has moved there.
void view_frame::draw_keyboard_focus(render::draw_context& dc)
{
	const auto f = frame();
	if (!f->has_focus() || f->focused_control()) return;

	if (const auto part = focused_part())
	{
		const auto region = keyboard_region_of(_keyboard.area);
		const render::scoped_clip clip(dc, region ? region->clip : recti(_extent));
		draw_keyboard_ring(dc, part->bounds);
	}
	else if (_keyboard_cues && _keyboard.area == keyboard_area::view && !_view_bounds.is_empty())
	{
		const auto bounds = _view->keyboard_bounds();
		const render::scoped_clip clip(dc, _view_bounds);
		draw_keyboard_ring(dc, bounds.is_empty() ? _view_bounds : bounds, bounds.is_empty());
	}
}

void view_frame::on_mouse_wheel(const pointi loc, const int delta, const platform::key_state keys, bool& was_handled)
{
	if (in_panel(loc)) _panel->on_mouse_wheel(loc, delta, keys, was_handled);
	else if (in_view(loc))
		was_handled = dispatch_wheel(loc, delta, keys, platform::wheel_axis::vertical, _wheel_vertical);
	update_controller(loc);
}

void view_frame::focus_changed(const bool has_focus, const ui::control_ptr& child)
{
	core::trace(std::format("render_window::focus {}", has_focus));
	// A control that takes the keyboard takes it off any drawn part, as a press does.
	if (has_focus && child) _keyboard = {};
	_view->focus(has_focus && !child && _keyboard.area == keyboard_area::view);
	if (child && _panel) _panel->focus_changed(has_focus, child);
	if (focus_listener) focus_listener(has_focus, child);
	frame()->invalidate();
}

view_controller_ptr view_frame::controller_from_location(const pointi loc, hit_test_context& ctx)
{
	if (ctx.occluded(_splitter.bounds))
	{
		return std::make_shared<panel_splitter_controller>(shared_from_this());
	}

	if (in_panel(loc))
	{
		ctx.confine(_panel->_bounds);
		return _panel->controller_from_location(loc, ctx);
	}

	if (!in_view(loc)) return nullptr;
	ctx.confine(_view_bounds);
	return _view->controller_from_location(shared_from_this(), loc, ctx);
}

void bar_frame::init(const ui::frame_ptr& owner)
{
	ui::frame_style fs;
	fs.colors = {ui::style::colors.toolbar_background, ui::style::colors.view_text,
	             ui::style::colors.view_selected_background};
	_frame = owner->create_frame(weak_from_this(), fs);
}

void bar_frame::on_window_layout(render::measure_context& mc, const sizei extent, const bool is_minimized)
{
	if (is_minimized) return;
	_extent = extent;

	std::vector<ui::control_ptr> shown;
	if (_kind == kind::title) _app.layout_title_bar(mc, extent, shown);
	else _app.layout_status_bar(mc, extent, shown);
	_controls = std::move(shown);
	frame()->invalidate();
}

void bar_frame::on_window_paint(render::draw_context& dc)
{
	if (_kind == kind::title) _app.paint_title_bar(dc);
	else _app.draw_status_bar(dc);

	for (const auto& c : _controls)
	{
		if (!c->is_visible()) continue;
		const auto foreground = dc.colors.foreground;
		if (_kind == kind::status && c == _app._receipt_link) dc.colors.foreground = core::rgba(104, 183, 255);
		c->render(dc, {});
		dc.colors.foreground = foreground;
	}
}

void bar_frame::visit_elements(const std::function<void(const view_element_ptr&, pointi)>& visit)
{
	const auto controls = _controls;
	for (const auto& c : controls) visit(c, {});
}

// The title bar moves the window by any part of it that is neither the title nor a control.
bool bar_frame::is_caption_area(const pointi loc) const
{
	return _kind == kind::title && !_app._title_bounds.contains(loc);
}

void bar_frame::on_mouse_move(const pointi loc, const bool /*is_tracking*/)
{
	if (_kind == kind::title) _app.title_bar_mouse_move(loc);
	else _app.update_status_hover(loc);
}

void bar_frame::on_mouse_leave(const pointi /*loc*/)
{
	if (_kind == kind::title) _app.title_bar_mouse_leave();
	else _app.update_status_hover({-1, -1});
}

void bar_frame::on_mouse_left_button_down(const pointi /*loc*/, const platform::key_state /*keys*/)
{
	_title_pressed = _kind == kind::title;
}

// The title is clicked only by a press it saw: the release after a press that closed a menu is not a click.
void bar_frame::on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/)
{
	if (std::exchange(_title_pressed, false)) _app.title_bar_click(loc);
}

void bar_frame::focus_changed(const bool has_focus, const ui::control_ptr& child)
{
	_app.focus_changed(has_focus, child);
}

void bar_frame::command_hover(const ui::command_ptr& c, const recti window_bounds)
{
	_app.command_hover(c, window_bounds);
}

// The frame's own menu: the title and status bars offer it, and so does anywhere the view under the pointer
// names none of its own.
static const view_menu s_frame_menu = {
	.list =
		{
			commands::refresh,
			commands::tool_eject,
			commands::none,
			commands::options,
			commands::favorite_tags,
			commands::none,
			commands::view_show_sidebar,
			commands::view_favorite_tags,
			commands::none,
			commands::large_font,
		},
};

std::vector<ui::command_ptr> bar_frame::menu(const pointi loc)
{
	// As app_frame::menu: the keyboard's menu has been opened already.
	if (loc == pointi{-1, -1}) return {};
	return _app.context_menu(s_frame_menu);
}

app_frame::app_frame() :
	_item_index(*this, _locations),
	_player(std::make_shared<av_player>(*this, [&index = _item_index](const core::file_path path, const double position)
{ index.save_media_position(path, position); })),
	_state(*this, *this, _item_index, _player), _edit_view_state(_state), _db(_state.item_index)
{
	_title_bar = std::make_shared<bar_frame>(*this, bar_frame::kind::title);
	_view_frame = std::make_shared<view_frame>(_state);
	_status_bar = std::make_shared<bar_frame>(*this, bar_frame::kind::status);
	_title_region = std::make_shared<ui::frame_element>();
	_body_region = std::make_shared<ui::frame_element>();
	_status_region = std::make_shared<ui::frame_element>();
	_regions = std::make_shared<ui::elements>(
		std::vector<view_element_ptr>{_title_region, _body_region, _status_region});
	_app_logo = std::make_shared<app_logo_element>(_state);
	_selector_frame = std::make_shared<view_frame>(_state);
	_view_sync = std::make_shared<sync_view>(_state, _view_frame);
	_view_tags = std::make_shared<tags_view>(_state, _view_frame);
	_view_options = std::make_shared<options_view>(_state, _view_frame,
	                                               options_view_actions{[this](const std::string_view code)
	{ select_language(code); }, [this] { index_maintenance_invoke(_state, _app_frame); }, [this] {
		return std::static_pointer_cast<void>(std::make_shared<pause_media>(_state));
	}, [this] { return ask_to_save_options(_app_frame); }});
	_view_import = std::make_shared<import_view>(_state, _view_frame);
	_view_locate = std::make_shared<locate_view>(_state, _view_frame);
	_view_explore = std::make_shared<explore_view>(_state, _view_frame);
	_view_faces = std::make_shared<faces_view>(_state, _view_frame);
	_view_rename = std::make_shared<rename_view>(_state, _view_frame);
	_view_batch = std::make_shared<batch_tool_view>(_state, _view_frame);
	_view_items = std::make_shared<items_view>(_state, _view_frame);
	_view_selector = std::make_shared<selector_view>(_state, _selector_frame,
	                                                 [this](const df::item_element_ptr& item,
	                                                        const platform::key_state keys)
	{ select_from_selector(item, keys); });
	_view_edit = std::make_shared<edit_view>(_state, _view_frame, _edit_view_state);
	_view_movie = std::make_shared<movie_view>(_state, _view_frame, _movie_view_state);
	_selector_frame->view(_view_selector);
	_view_media = std::make_shared<media_view>(_state, _view_frame);

	// Both frames carry a view from construction. init() creates their windows before the first
	// view_changed assigns one, and only some of view_frame's handlers test _view, so a paint or
	// pointer message arriving in that window would otherwise dereference null. Items is what
	// view_mode(view_type::items) selects moments later, and view_changed replaces it either way.
	_view_frame->view(_view_items);
}

app_frame::~app_frame()
{
	_threads.clear();
	// Order matters: workers truncate their queues as they exit, and a truncated task holding the last
	// reference to UI-owned state hands it to _ui_queue rather than destroying it on the worker (see
	// ui_owned_ptr). Joining first and draining second is what makes those hand-backs land here, on the
	// UI thread, while the objects they reference are still alive.
	(void)_ui_queue.dequeue_all();
	_state.close();

	core::log(__FUNCTION__, "destruct");
}

void app_frame::frame_tick()
{
	core::assert_true(platform::is_ui_thread());
	core::bump(render::ui_perf.frame_prepares);

	// An agent session's calls are answered from here, with a window or without one.
	if (_mcp) _mcp->tick();

	const auto vf = _view_frame;

	// _view is only assigned by view_changed, which init() reaches after the frame exists.
	if (!platform::is_closing && vf && _view)
	{
		const auto time_now = core::now();
		const auto display_frequency = platform::display_frequency();
		const auto animation_delay_ms = 1000 / display_frequency;
		constexpr auto idle_delay_ms = 1000 / render::default_ticks_per_second;

		// Clamped so a stall, a breakpoint, or a first frame cannot snap every fade to its target.
		const auto prepare_us = core::now_us();
		const auto elapsed_s = _last_prepare_us == 0
		                           ? 1.0 / display_frequency
		                           : std::clamp((prepare_us - _last_prepare_us) / 1'000'000.0, 1.0 / 240.0, 1.0 / 15.0);
		_last_prepare_us = prepare_us;
		ui::animation_step_factor = static_cast<float>(1.0 - std::exp(-ui::alpha_fade_rate * elapsed_s));

		std::vector<void*> removals;
		auto is_animating = false;
		auto is_animating2 = false;

		// NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order): each animation steps on its own
		for (const auto& i : ui::animations)
		{
			if (i.second())
			{
				is_animating2 = true;
			}
			else
			{
				removals.emplace_back(i.first);
			}
		}

		for (const auto& i : removals)
		{
			ui::animations.erase(i);
		}

		core::record_peak(render::ui_perf.animations_peak, static_cast<uint32_t>(ui::animations.size()));
		if (is_animating2) core::bump(render::ui_perf.prepares_registered_anim);

		const auto display = _state.display_state();
		auto frame_delay = idle_delay_ms;

		if (display)
		{
			is_animating |= display->step();
			if (is_animating) core::bump(render::ui_perf.prepares_display_anim);

			// If a cloud-only item was hydrated by viewing it, rescan so its items-view
			// thumbnail and index state refresh.
			_state.rescan_hydrated_display_item();

			const auto prepare_result = display->update_for_present(time_now);

			if (prepare_result == render_valid::invalid || is_animating)
			{
				if (prepare_result == render_valid::invalid) core::bump(render::ui_perf.prepares_display_invalid);
				invalidate_status();
				vf->frame()->invalidate();
			}
			else if (prepare_result == render_valid::present)
			{
				// Only what changed in place is shown again: the software renderer replays that region alone.
				vf->frame()->redraw(display->present_bounds());
			}

			if (is_animating2 || is_animating || display->is_playing_media() || display->is_running_c64())
			{
				frame_delay = std::min(frame_delay, animation_delay_ms);
			}
		}

		const auto progress = _view->progress();

		if (_app_logo && _show_bars && !_title_bounds.is_empty())
		{
			// Only the logo changed, so only the logo is invalidated - the title bar repaints that
			// rectangle and nothing else does. Faded, it drifts on the idle timer's ticks, and only where
			// animation is wanted and the window can be seen.
			if (_app_logo->step_plasma(time_now, render::animations_enabled && !vf->is_occluded()))
			{
				_title_bar->frame()->invalidate(_app_logo->invalidate_bounds());
			}
		}

		if (_app_logo && _app_logo->plasma_is_active())
		{
			frame_delay = std::min(frame_delay, 1000 / plasma::frames_per_second);
		}

		// A playing movie needs the cadence video playback gets: left on the idle timer the preview
		// advanced five times a second, which reads as the picture stepping rather than playing. The view
		// repaints itself only when its picture can have changed - a frame of the movie, not of the display.
		if (_state.view_mode() == view_type::movie && _view_movie && _view_movie->is_playing())
		{
			_view_movie->tick();
			frame_delay = std::min(frame_delay, animation_delay_ms);
		}

		if (command_progress_needs_animation(progress.active, progress.total))
		{
			frame_delay = std::min(frame_delay, animation_delay_ms);
		}

		// A tutorial's steps and its pointer move at the display's rate; once it has finished it goes.
		if (_tutorial)
		{
			_tutorial->tick();
			if (_tutorial->is_finished()) _tutorial.reset();
			frame_delay = std::min(frame_delay, animation_delay_ms);
		}

		if (vf->is_occluded())
		{
			frame_delay = 200;
		}

		_frame_delay = frame_delay;
		platform::set_frame_delay(frame_delay);
	}
}

void app_frame::invalidate_status() const
{
	if (setting.show_debug_info && _view_frame)
	{
		_view_frame->frame()->invalidate();
	}
}

void app_frame::update_overlay()
{
	if (_view_frame)
	{
		const bool show_overlays = _state.should_show_overlays();

		if (show_overlays != _view_frame->show_cursor())
		{
			_view_frame->show_cursor(show_overlays);
			_view_media->overlay_alpha_target = show_overlays ? 1.0f : 0.0f;
			const auto display = _state.display_state();
			const auto overlay_target = show_overlays ? 1.0f : 0.0f;

			ui::animations[this] = [v = _view_media, fv = _view_frame, display, overlay_target]
			{
				// Gated and timed as animate_alpha is: under the CPU renderer the overlay takes its
				// new state at once, and elsewhere the fade lasts as long at any refresh rate.
				const auto approach = [&fv](float& value, const float target)
				{
					const auto dd = target - value;
					if (fabs(dd) <= 0.001f) return false;
					value = render::animations_enabled ? value + dd * ui::animation_step_factor : target;
					fv->invalidate_view(view_invalid::view_redraw);
					return true;
				};

				auto invalidate = approach(v->overlay_alpha, v->overlay_alpha_target);
				if (display && approach(display->_zoom_overlay_alpha, overlay_target)) invalidate = true;
				return invalidate;
			};

			if (!show_overlays && _bubble)
			{
				_bubble->hide();
			}

			invalidate_view(view_invalid::animations);
		}
	}
}

void app_frame::tick()
{
	const auto display = _state.display_state();

	if (display && display->_session)
	{
		display->_session->mute(display->_mute_preview);
		display->_session->adjust_volume();
	}

	// A running Commodore 64 follows the volume as the player does, up to the device's own full scale.
	if (display && display->_c64)
	{
		display->_c64->volume(std::clamp(setting.media_volume, 0, 1000) / 1000.0);
	}

	// _view is assigned by the first view_changed, which init() reaches after the window - and its
	// timer - already exist, so a tick can land in between.
	if (_app_frame && _app_frame->is_visible() && _view)
	{
		const auto time_now = core::now();

		_state.tick(_view_frame, time_now);
		_view_items->retry_visible_thumbnails(time_now);
		// The sidebar is embedded in the items view and shares its frame, so nothing else ticks it.
		if (_view_items->sidebar_visible()) _view_items->sidebar()->tick();
		_view_frame->tick();
		_selector_frame->tick();

		if (_state.view_mode() == view_type::movie) _view_movie->tick();
		if (_state.view_mode() == view_type::explore) _view_explore->tick_model();

		const auto progress = _view->progress();
		const auto repaint = status_repaints(progress.active, progress.total, setting.show_debug_info);

		if (repaint.status_bar && _show_bars)
		{
			_status_bar->frame()->invalidate();
		}

		if (repaint.view && _view_frame)
		{
			_view_frame->frame()->invalidate();
		}

		_search_color_lerp.target = _state.item_index.searching > 0 ? 255 : 0;
		if (_search_edit && _search_color_lerp.step())
		{
			_search_edit->set_background(
				_search_color_lerp.lerp(ui::style::colors.edit_background, ui::style::colors.important_background));
		}

		const auto logical_bounds = _view_items->calc_logical_items_bounds();
		const auto eviction_guard = _last_texture_eviction_bounds
		                                ? _last_texture_eviction_bounds->inflate(0, logical_bounds.height() / 2)
		                                : recti{};
		const auto outside_eviction_guard = !_last_texture_eviction_bounds ||
		                                    logical_bounds.left < eviction_guard.left ||
		                                    logical_bounds.right > eviction_guard.right ||
		                                    logical_bounds.top < eviction_guard.top ||
		                                    logical_bounds.bottom > eviction_guard.bottom;

		if (outside_eviction_guard)
		{
			_last_texture_eviction_bounds = logical_bounds;
			free_graphics_resources(true, true);
			df::trim_thumbnail_blobs(_state.search_items().items(), logical_bounds, core::max_thumbnail_bytes);
		}

		update_overlay();
	}

	if (_search_predictions_frame)
	{
		if (!_search_has_focus && _search_predictions_frame->frame()->is_visible())
		{
			hide_search_predictions();
		}
	}

	if (_folder_change_time != 0.0)
	{
		constexpr auto folder_change_settle_seconds = 1.0;

		if ((core::now() - _folder_change_time) > folder_change_settle_seconds)
		{
			_folder_change_time = 0.0;
			// The notification only says something touched the folder. Refreshing re-opens the whole
			// search, so let the index decide whether anything actually differs first.
			_item_index.queue_validate_changed_folders(std::move(_folders_changed));
			_folders_changed.clear();
		}
	}

	if (_instance_claim_retry && _instance_ready && !_instance_receiver && setting.reuse_existing_window &&
	    core::now() >= _next_instance_claim)
	{
		constexpr auto instance_claim_seconds = 2.0;
		_next_instance_claim = core::now() + instance_claim_seconds;
		reuse_running_instance(false);
	}

	// docs/faces.md#64-the-face-database: a window that only reads the face book takes it over within a few seconds of
	// the window writing it closing, and a book whose change could not be written is written again.
	if (_state.item_index.faces().book_needs_polling() && core::now() >= _next_face_book_poll)
	{
		constexpr auto face_book_poll_seconds = 2.0;
		_next_face_book_poll = core::now() + face_book_poll_seconds;
		queue_database([](database& db) { db.poll_faces(); });
	}

	if (_invalids != view_invalid::none)
	{
		idle();
	}

	// A recording that has written its length, or failed, is finished here and said so.
	if (_recording && _recording->is_finished())
	{
		const auto summary = finish_recording();
		if (_recording_ended) _recording_ended(summary);
	}
}

// A run with a profile of its own - a tutorial or a test - reports nothing to diffractor.com: a fresh profile would
// count as a new install, and its crashes are the developer's to see. Nor does an agent session, unless it was
// started to reach the network, and one whose window is not on the desktop plays nothing aloud. -collection makes
// that folder the whole collection, and opens the window on it. Applied over the settings each time they are read,
// so nothing a profile saved on an earlier run outlasts what this one was started with.
void app_frame::apply_run_settings()
{
#ifndef WINSTORE
	if (!command_line.profile_folder.empty() || (command_line.agent && !df::network_allowed))
	{
		setting.check_for_updates = false;
		setting.send_crash_dumps = false;
	}
#endif

	if (_mcp && !_mcp->is_console() && _mcp->options().presence != agent_presence::visible)
	{
		setting.media_volume = 0;
	}

	if (!command_line.collection_folder.empty())
	{
		setting.collection = {};
		setting.collection.more_folders = command_line.collection_folder;
		if (command_line.folder_path.is_empty())
			command_line.folder_path = core::item_selector(command_line.collection_folder);

		// The collection is all such a run has been to: a new profile offers the user's own folders - their name,
		// and where they keep things - and a film of the run would show them.
		_state.recent_folders._items = {command_line.collection_folder};
		_state.recent_searches._items = {command_line.collection_folder};
	}
}

std::string app_frame::start_recording(const screen_recording_request& request)
{
	if (_recording) return "the window is already being recorded";
	if (!core::is_path(request.path.str()))
		return std::format("give the full path of the movie to write: {}", request.path);

	_recording = start_screen_recording(request);
	if (!_recording) return "this platform cannot record its window";

	core::log(__FUNCTION__, std::format("Recording {}", request.path));
	return {};
}

std::string app_frame::finish_recording()
{
	if (!_recording) return {};

	_recording->stop();
	auto summary = _recording->summary();
	_recording.reset();
	core::log(__FUNCTION__, summary);
	return summary;
}

static constexpr auto s_recent_folders = "recent_folders";
static constexpr auto s_recent_searches = "recent_searches";
static constexpr auto s_recent_apps = "recent_apps";
static constexpr auto s_recent_tags = "recent_tags";
static constexpr auto s_recent_locations = "recent_locations";
static constexpr auto s_group_order = "group_order";
static constexpr auto s_sort_order = "sort_order";
static constexpr auto s_media_filter = "media_filter";
static constexpr auto s_unsettled_starts = "unsettled_starts";
static constexpr auto s_options_version = "options_version";

// Bump when a release changes what a stored option means, so a value written by an older build can
// be translated once. 1: "date created" used to resolve the capture time, which is now its own
// order. Migrating on every start instead would make the order it migrates away from unreachable.
static constexpr uint32_t options_version = 1;

// Rolling a release back is ordinary, and both settings and the cache are shared with whatever
// version the user goes back to. The long-standing keys therefore keep a value every build
// understands, and the true order lives beside them under its own key. The rules are
// `group_order_older_builds_understand` and `resolve_stored_order` in model_items.h.
static constexpr auto s_group_order_ex = "group_order_ex";
static constexpr auto s_sort_order_ex = "sort_order_ex";

void app_frame::load_options(const platform::setting_file_ptr& store)
{
	if (store->root_created())
	{
		const core::cached default_folders[] = {
			known_path(platform::known_folder::pictures).text(),  known_path(platform::known_folder::video).text(),
			known_path(platform::known_folder::music).text(),     known_path(platform::known_folder::desktop).text(),
			known_path(platform::known_folder::downloads).text(),
		};

		for (const auto& f : default_folders)
		{
			_state.recent_folders.add(f);
			_state.recent_searches.add(f);
		}

		saved_current_search = known_path(platform::known_folder::pictures).text();
	}
	else
	{
		_state.recent_folders.read({}, s_recent_folders, store);
		_state.recent_searches.read({}, s_recent_searches, store);
		_state.recent_apps.read({}, s_recent_apps, store);
		_state.recent_tags.read({}, s_recent_tags, store);
		_state.recent_locations.read({}, s_recent_locations, store);

		auto group_order = static_cast<uint32_t>(group_by::file_type);
		auto sort_order = static_cast<uint32_t>(sort_by::name);
		store->read({}, s_group_order, group_order);
		store->read({}, s_sort_order, sort_order);

		auto group_order_ex = group_order;
		auto sort_order_ex = sort_order;
		const auto has_group_ex = store->read({}, s_group_order_ex, group_order_ex);
		const auto has_sort_ex = store->read({}, s_sort_order_ex, sort_order_ex);

		_starting_group_order = resolve_stored_order<group_by>(group_order, group_order_ex, has_group_ex,
		                                                       group_order_older_builds_understand);
		_starting_sort_order = resolve_stored_order<sort_by>(sort_order, sort_order_ex, has_sort_ex,
		                                                     sort_order_older_builds_understand);

		uint32_t stored_options_version = 0;
		store->read({}, s_options_version, stored_options_version);

		// Someone who chose "date created" wanted when the picture was taken, so keep giving them that
		// (#184). Once only: after this the two orders are separate choices and both must stick.
		if (stored_options_version < 1)
		{
			if (_starting_group_order == group_by::date_created) _starting_group_order = group_by::date_original;
			if (_starting_sort_order == sort_by::date_created) _starting_sort_order = sort_by::date_original;
		}

		std::string media_filter;
		store->read({}, s_media_filter, media_filter);
		const auto parsed_filter = media_filter_from_string(media_filter);
		_starting_media_filter.assign(parsed_filter.groups().begin(), parsed_filter.groups().end());

		setting.read();
		store->read({}, s_search, saved_current_search);
	}
}

void app_frame::save_options(const bool search_only)
{
	auto& store = _settings;
	store->write({}, s_search, _state.search().text());
	store->write({}, s_group_order, static_cast<uint32_t>(group_order_older_builds_understand(_state.group_order())));
	store->write({}, s_sort_order, static_cast<uint32_t>(sort_order_older_builds_understand(_state.sort_order())));
	store->write({}, s_group_order_ex, static_cast<uint32_t>(_state.group_order()));
	store->write({}, s_sort_order_ex, static_cast<uint32_t>(_state.sort_order()));
	store->write({}, s_options_version, options_version);

	store->write({}, s_media_filter, media_filter_to_string(_state.filter()));

	if (_app_frame)
	{
		_app_frame->save_window_position(store);
	}

	if (!search_only)
	{
		_state.recent_folders.write({}, s_recent_folders, store);
		_state.recent_searches.write({}, s_recent_searches, store);
		_state.recent_apps.write({}, s_recent_apps, store);
		_state.recent_tags.write({}, s_recent_tags, store);
		_state.recent_locations.write({}, s_recent_locations, store);
		setting.write();
		// An agent session takes no file from Explorer: the user's next launch must not land in its window.
		if (_instance_ready && !_mcp)
		{
			if (!setting.reuse_existing_window) _instance_receiver.reset();
			// Enabling another window must not forward that window's original startup file again.
			else if (!_instance_receiver) reuse_running_instance(false);
		}
	}
}

void app_frame::search_complete(const df::search_t& search, const bool path_changed)
{
	if (path_changed)
	{
		save_options(true);
		invalidate_view(view_invalid::address);

		std::vector<platform::watched_folder> folders;

		for (const auto& s : search.selectors())
		{
			if (platform::exists(s.folder()))
			{
				// A folder shown with its subfolders is watched with them, so a file moved out of one
				// leaves the listing as it does from the folder itself (#266).
				folders.push_back({s.folder(), s.is_recursive()});
			}
		}

		platform::monitor_folders(folders);
	}

	_view->items_changed(path_changed);
	_view_selector->items_changed(path_changed);

	const auto& sidebar = _view_items->sidebar();
	sidebar->update_current_search();
	sidebar->invalidate();

	// No group_layout here. view_state::append_items has just rebuilt the groups, re-selected and
	// raised group_layout_complete in the same UI task, so asking for group_layout as well made the
	// next drain regroup, re-sort and relayout the entire listing a second time for every search.
	invalidate_view(view_invalid::item_scan | view_invalid::media_elements | view_invalid::tooltip |
	                view_invalid::command_state | view_invalid::app_layout | view_invalid::presence);

	if (path_changed)
	{
		invalidate_view(view_invalid::focus_item_visible);
	}
}

// Every bar's toolbars share one style, so measuring all of them gives the command rows a common height.
static int bars_toolbar_height(render::measure_context& mc,
                               const std::map<std::vector<bar_entry>, app_frame::frame_bar>& bars, const int cx)
{
	auto result = 0;

	for (const auto& [entries, parts] : bars)
	{
		for (const auto& part : parts)
		{
			if (part.toolbar) result = std::max(result, part.toolbar->measure_toolbar(mc, cx).cy);
		}
	}

	return result;
}

int app_frame::title_bar_height(render::measure_context& mc)
{
	const auto cy_address = mc.text_line_height(render::font_face::dialog) + mc.padding2 * 2;
	const auto content_height = std::max({_navigate1->measure_toolbar(mc, _extent.cx).cy,
	                                      _navigate2->measure_toolbar(mc, _extent.cx).cy,
	                                      _navigate3->measure_toolbar(mc, _extent.cx).cy, cy_address,
	                                      bars_toolbar_height(mc, _bars, _extent.cx)}) +
	                            mc.padding2;
	return core::mul_div(content_height, 6, 5);
}

int app_frame::status_bar_height(render::measure_context& mc, const int width)
{
	const auto line_height = mc.text_line_height(render::font_face::dialog);
	const auto height = std::max(bars_toolbar_height(mc, _bars, _extent.cx), line_height + mc.padding2) + mc.padding2;
	_status_text_row_height = 0;
	if (!_view || _view != _view_faces) return height;

	// Leave the commands where they fit; on a narrow bar the text gets the full width below them.
	const auto declared = _view->bars();
	const auto bar_width = [&](const std::vector<bar_entry>& entries)
	{
		auto used = 0;
		for (const auto& slot : measure_bar(mc, bar(entries), line_height + mc.padding2))
			if (slot.extent.cx > 0) used += slot.extent.cx + mc.padding1;
		return used;
	};
	const auto text_width = width - mc.padding2 - mc.padding1 - mc.handle_cxy - bar_width(declared.bottom) -
	                        bar_width(declared.bottom_leading);
	const auto text_height = line_height + mc.padding2;
	if (text_width >= core::round(180 * mc.scale_factor)) return std::max(height, text_height);
	_status_text_row_height = text_height;
	return height + text_height;
}

// The window holds three regions, each with a surface of its own: the title bar, the view - with its panel
// and strip - and the status bar. Full screen and a zoomed picture give the view the whole window; otherwise
// a line of the window's background is left round the edge, which is its border.
void app_frame::layout(render::measure_context& mc)
{
	if (platform::is_closing || !_app_frame || !_view || !_navigate1 || !_search_edit || !_navigate2 || !_navigate3 ||
	    !_filter_edit || !_item_size || _extent.is_empty())
	{
		return;
	}

	update_button_state(true);

	// Zoom is the picture's only in the views that show it: a task opened while a photo was zoomed keeps
	// its bars, and with them its Save, Run and Cancel.
	const auto display = _state.display_state();
	const auto picture_zoomed = _state.is_items_or_media_view() && display && display->is_zoom_mode();
	_show_bars = !_state.is_full_screen && !picture_zoomed;

	const auto scale1 = core::round(1 * mc.scale_factor);
	const auto client = recti(_extent).inflate(_state.is_full_screen ? 0 : -scale1);
	const auto body_top = _show_bars ? client.top + title_bar_height(mc) : client.top;
	const auto body_bottom = _show_bars ? std::max(body_top, client.bottom - status_bar_height(mc, client.width()))
	                                    : client.bottom;

	_title_region->bounds = recti(client.left, client.top, client.right, body_top);
	_body_region->bounds = recti(client.left, body_top, client.right, body_bottom);
	_status_region->bounds = recti(client.left, body_bottom, client.right, client.bottom);
	_title_region->is_visible(_show_bars);
	_status_region->is_visible(_show_bars);

	// A running task's panel and strip take no input: what the run works on stays what they showed when it began.
	const auto task_running = _view->progress().active;
	if (const auto& panel = _view_frame->_panel) panel->enable(!task_running);
	_view_selector->enable(!task_running);
	_view_frame->_strip_wanted = selector_strip_for_view(_state.view_mode()) != selector_strip::none;

	// Placing a region lays it out only when its size changes, and what it shows may have changed anyway.
	_title_bar->frame()->layout();
	_view_frame->frame()->layout();
	_status_bar->frame()->layout();

	if (_search_predictions_frame && _search_predictions_frame->frame()->is_visible())
	{
		_search_predictions_frame->frame()->window_bounds(calc_search_popup_bounds(), true);
	}
}

void app_frame::visit_elements(const std::function<void(const view_element_ptr&, pointi)>& visit)
{
	visit(_regions, {});
}

// The title bar, in its own coordinates: the logo and title at the left; the address box between the history
// and folder buttons, centred in the window, where the view navigates; or the view's own commands, placed
// before the title so a long title yields to Close; and the window's buttons at the right.
void app_frame::layout_title_bar(render::measure_context& mc, const sizei extent, std::vector<ui::control_ptr>& shown)
{
	if (!_view || !_navigate1) return;

	const auto declared = _view->bars();
	const auto show_navigation = declared.navigation;
	const auto scale300 = core::round(300 * mc.scale_factor);
	const auto scale1000 = core::round(1000 * mc.scale_factor);
	const auto area = recti(extent);

	auto before_address = _navigate1;
	auto after_address = _navigate2;
	auto navigate1_extent = before_address->measure_toolbar(mc, extent.cx);
	auto navigate2_extent = after_address->measure_toolbar(mc, extent.cx);
	const auto navigate3_extent = _navigate3->measure_toolbar(mc, extent.cx);

	// Bound to an lvalue, not a temporary: each bar lives in _bars, but a reference returned from a call
	// that was handed a temporary reads as dangling to GCC.
	static const std::vector<bar_entry> no_entries;
	const auto& top_entries = declared.navigation ? no_entries : declared.top;
	const auto& top_bar = bar(top_entries);
	const auto cy_address = mc.text_line_height(render::font_face::dialog) + mc.padding2 * 2;
	const auto cy_control = mc.text_line_height(render::font_face::dialog) + mc.padding2;
	const auto top_slots = measure_bar(mc, top_bar, cy_control);
	const auto center_y = [&area](const int height)
	{
		return area.top + (area.height() - height) / 2;
	};

	const auto x_nav3 = area.right - mc.padding1 - navigate3_extent.cx;
	const auto y_nav3 = center_y(navigate3_extent.cy);
	const recti nav3_bounds(x_nav3, y_nav3, x_nav3 + navigate3_extent.cx, y_nav3 + navigate3_extent.cy);

	const auto toolbar_widths = navigate1_extent.cx + navigate2_extent.cx + navigate3_extent.cx;
	auto address_width = std::clamp((area.width() - toolbar_widths) / 2, scale300, scale1000);
	const auto address_center = (area.left + area.right) / 2;
	auto address_left = address_center - address_width / 2;
	if (show_navigation && (address_left <= navigate1_extent.cx + mc.padding1 ||
	                        address_left + address_width + navigate2_extent.cx + mc.padding1 >= x_nav3))
	{
		// Keep the main menu: it contains the destinations whose buttons no longer fit.
		after_address = bar({commands::menu_main}).front().toolbar;
		navigate2_extent = after_address->measure_toolbar(mc, extent.cx);
		const auto right = x_nav3 - navigate2_extent.cx - 2 * mc.padding1;
		const auto logo_room = mc.icon_cxy + 2 * mc.padding2;
		if (right - logo_room - navigate1_extent.cx < scale300)
		{
			before_address = bar({commands::browse_back}).front().toolbar;
			navigate1_extent = before_address->measure_toolbar(mc, extent.cx);
		}
		const auto left = logo_room + navigate1_extent.cx + mc.padding1;
		address_width = std::min(address_width, std::max(0, right - left));
		address_left = std::clamp(address_center - address_width / 2, left, std::max(left, right - address_width));
	}
	const auto address_right = address_center + address_width / 2;
	const auto nav1_x = address_left - mc.padding1 - navigate1_extent.cx;
	const auto nav1_y = center_y(navigate1_extent.cy);
	const recti nav1_bounds(nav1_x, nav1_y, nav1_x + navigate1_extent.cx, nav1_y + navigate1_extent.cy);
	const auto nav2_x = address_right + mc.padding1;
	const auto nav2_y = center_y(navigate2_extent.cy);
	const recti nav2_bounds(nav2_x, nav2_y, nav2_x + navigate2_extent.cx, nav2_y + navigate2_extent.cy);
	const auto address_y = center_y(cy_address);
	const recti search_bounds(address_left, address_y, address_right, address_y + cy_address);

	_app_logo->text(declared.navigation ? core::app_name() : _view->title());
	const auto bar_left = area.left + mc.padding2;
	const auto top_arrangement = arrange_bar(recti(bar_left, area.top, area.right - mc.padding2, area.bottom),
	                                         top_slots, mc.padding1);
	const auto title_limit = declared.navigation ? nav1_bounds.left - mc.padding1 : top_arrangement.remainder.right;
	const auto title_extent = _app_logo->measure(mc, std::max(0, title_limit - bar_left));
	const auto title_top = center_y(title_extent.cy);
	_title_bounds = recti(bar_left, title_top, bar_left + title_extent.cx, title_top + title_extent.cy);
	_app_logo->bounds = _title_bounds;

	const auto place = [&mc, &shown](const ui::control_ptr& c, const recti bounds, const bool visible)
	{
		if (!visible) return;
		c->is_visible(true);
		c->layout(mc, bounds);
		shown.emplace_back(c);
	};

	place(before_address, nav1_bounds, show_navigation && nav1_bounds.left > area.left && nav1_bounds.right < x_nav3);
	place(_search_edit, search_bounds, show_navigation && !search_bounds.is_empty() && search_bounds.right < x_nav3);
	place(after_address, nav2_bounds, show_navigation && nav2_bounds.right < x_nav3);

	for (auto i = 0u; i < top_bar.size(); ++i)
	{
		if (top_bar[i].toolbar)
			place(top_bar[i].toolbar, top_arrangement.slots[i], !top_arrangement.slots[i].is_empty());
	}

	// A task view's own bar carries its window buttons, beside its Close.
	place(_navigate3, nav3_bounds, show_navigation);

	// Focus left in a box the bar no longer shows would route every key somewhere the user cannot see,
	// so the keyboard goes back to the view.
	if (_search_has_focus && std::ranges::find(shown, _search_edit) == shown.end())
	{
		queue_ui([this] { focus_view(); });
	}
}

// The status bar, in its own coordinates: the view's own controls at the trailing end, kept clear of the
// corner the window is resized by, then whatever the view starts the bar with at the leading end; the
// view's status takes the room between them. The trailing controls are placed first, so a short bar gives up
// the leading ones before them, and a run of the view's own commands short of room gives way from its leading
// end into a More menu, so the command that ends it - a task's Run, Save or Render - stays.
void app_frame::layout_status_bar(render::measure_context& mc, const sizei extent, std::vector<ui::control_ptr>& shown)
{
	if (!_view || !_filter_edit) return;

	const auto declared = _view->bars();
	const auto view_processing = _view->progress().active;
	static const std::vector<bar_entry> no_entries;
	const auto& bottom_bar = bar(view_processing ? running_task_bar() : declared.bottom);
	const auto& leading_bar = bar(view_processing ? no_entries : declared.bottom_leading);
	const auto cy_control = mc.text_line_height(render::font_face::dialog) + mc.padding2;
	auto bottom_slots = measure_bar(mc, bottom_bar, cy_control);
	const auto leading_slots = measure_bar(mc, leading_bar, cy_control);

	for (auto i = 0u; i < bottom_bar.size(); ++i)
	{
		const auto& part = bottom_bar[i];
		if (part.toolbar && part.run.size() > 1)
			bottom_slots[i].min_width = trimmed_run(part.run, 1)->measure_toolbar(mc, extent.cx).cx;
	}

	const recti inner(mc.padding2, 0, extent.cx - mc.padding1 - mc.handle_cxy,
	                  std::max(0, extent.cy - _status_text_row_height));
	auto commands_area = inner;
	if (_view == _view_faces && !bottom_slots.empty())
	{
		// In Faces, make room for the filter and slider before putting the rest of the commands into More.
		auto controls_width = 0;
		for (const auto& slot : leading_slots) controls_width += slot.extent.cx + mc.padding1;
		commands_area.left += std::clamp(inner.width() - bottom_slots.back().min_width, 0, controls_width);
	}
	const auto bottom_arrangement = arrange_bar(commands_area, bottom_slots, mc.padding1);
	auto leading_area = bottom_arrangement.remainder;
	leading_area.left = inner.left;
	const auto leading_arrangement = arrange_bar(leading_area, leading_slots, mc.padding1, bar_alignment::leading);
	_status_text_bounds = _status_text_row_height > 0 ? recti(inner.left, inner.bottom, inner.right, extent.cy)
	                                                  : leading_arrangement.remainder;
	_totals_bounds = {};
	_bar_overflow.clear();

	// A run given less than its width shows the most of its last commands that fit after the More button, which
	// lists the rest.
	const auto fitted_run = [this, &mc, &extent](const frame_bar_part& part, recti& bounds)
	{
		if (!part.toolbar || part.run.size() < 2) return part.toolbar;
		if (bounds.width() >= part.toolbar->measure_toolbar(mc, extent.cx).cx) return part.toolbar;

		for (auto keep = part.run.size() - 1; keep > 0; --keep)
		{
			const auto& trimmed = trimmed_run(part.run, keep);
			const auto width = trimmed->measure_toolbar(mc, extent.cx).cx;
			if (keep > 1 && width > bounds.width()) continue;

			for (auto i = 0u; i < part.run.size() - keep; ++i)
			{
				if (auto command = find_command(part.run[i]); command && command->visible)
					_bar_overflow.emplace_back(std::move(command));
			}

			bounds.left = std::max(bounds.left, bounds.right - width);
			return trimmed;
		}

		return part.toolbar;
	};

	const auto place =
		[this, &mc, &shown, &extent, &fitted_run](const frame_bar& b, const bar_arrangement& a, const bool trims)
	{
		for (auto i = 0u; i < b.size(); ++i)
		{
			auto bounds = a.slots[i];
			if (bounds.is_empty()) continue;

			ui::control_ptr c;
			if (b[i].toolbar)
			{
				c = trims ? fitted_run(b[i], bounds) : b[i].toolbar;
				if (!trims && b[i].toolbar->measure_toolbar(mc, extent.cx).cx > bounds.width())
				{
					// A clipped leading toolbar used to leave half an icon against the filter box.
					std::vector<bar_entry> entries(b[i].run.begin(), b[i].run.end());
					for (size_t keep = entries.size(); keep-- > 0;)
					{
						entries.resize(keep);
						entries.emplace_back(commands::menu_bar_more);
						const auto compact = bar(entries).front().toolbar;
						const auto width = compact->measure_toolbar(mc, extent.cx).cx;
						if (width > bounds.width() && keep != 0) continue;
						for (size_t k = keep; k < b[i].run.size(); ++k)
							if (auto cmd = find_command(b[i].run[k]); cmd && cmd->visible) _bar_overflow.push_back(cmd);
						if (width > bounds.width()) c.reset();
						else
						{
							c = compact;
							bounds.right = bounds.left + width;
						}
						break;
					}
				}
			}
			else if (b[i].control == bar_control::item_filter) c = _filter_edit;
			else if (b[i].control == bar_control::item_size) c = _item_size;
			else if (b[i].control == bar_control::face_filter) c = _face_filter_edit;
			else if (b[i].control == bar_control::face_size) c = _face_size;
			else _totals_bounds = bounds;

			if (c)
			{
				c->is_visible(true);
				c->layout(mc, bounds);
				shown.emplace_back(c);
			}
		}
	};

	place(leading_bar, leading_arrangement, false);
	place(bottom_bar, bottom_arrangement, true);

	_receipt_prefix_bounds = {};
	_receipt_link.reset();
	_receipt_suffix_bounds = {};
	if (_view == _view_faces && !_status_text_bounds.is_empty())
	{
		const auto dest = _view_faces->can_undo_placement() ? _view_faces->placement_destination() : std::string{};
		const auto status = _view_faces->status();
		const auto line = status.substr(0, status.find('\n'));
		const auto at = dest.empty() ? line.npos : line.rfind(dest);
		const auto undo = _view_faces->can_undo_placement() ? commands::face_undo_placement : commands::none;
		if (at != line.npos || undo != commands::none)
		{
			auto row = _status_text_bounds;
			const auto height = std::min(row.height(), cy_control);
			row.top += (row.height() - height) / 2;
			row.bottom = row.top + height;
			_receipt_prefix = line.substr(0, at);
			_receipt_suffix = at == line.npos ? "" : std::string(line.substr(at + dest.size()));
			constexpr auto font = render::font_face::dialog;
			constexpr auto style = render::text_style::single_line;
			auto link = at != line.npos ? bar({commands::face_follow_placement}).front().toolbar : nullptr;
			_receipt_link = link;
			auto revert = undo != commands::none ? bar({undo}).front().toolbar : nullptr;
			const int undo_width = revert ? std::min(row.width(), revert->measure_toolbar(mc, extent.cx).cx) : 0;
			const int link_width = link ? std::min((row.width() - undo_width) * 2 / 3,
			                                       link->measure_toolbar(mc, extent.cx).cx)
			                            : 0;
			const int prefix_width = std::min(row.width() - undo_width - link_width,
			                                  mc.measure_text(_receipt_prefix, font, style, extent.cx).cx +
			                                      mc.padding1);
			_receipt_prefix_bounds = {row.left, row.top, row.left + prefix_width, row.bottom};
			row.left += prefix_width;
			const auto add_action = [&](const ui::toolbar_ptr& button, const int w)
			{
				if (!button || w <= 0) return;
				button->is_visible(true);
				button->layout(mc, {row.left, row.top, row.left + w, row.bottom});
				shown.push_back(button);
				row.left += w;
			};
			add_action(link, link_width);
			add_action(revert, undo_width);
			_receipt_suffix_bounds = row;
		}
	}

	if (_focused_filter && std::ranges::find(shown, _focused_filter) == shown.end())
	{
		queue_ui([this] { focus_view(); });
	}
}

void app_frame::paint_title_bar(render::draw_context& dc)
{
	if (_show_bars && !_title_bounds.is_empty())
	{
		_app_logo->render(dc, {});
	}
}

void parse_more_folders(core::index_roots& result, const std::string_view more_folders, const platform::drives& drives)
{
	core::hash_set<std::string, core::ihash, core::ieq> drive_label_includes;

	for (auto line : split_collection_folders(more_folders))
	{
		if (!core::is_empty(line))
		{
			if (core::is_exclude(line))
			{
				while (!line.empty() && (core::is_white_space(line.front()) || line.front() == '-'))
				{
					line = line.substr(1);
				}

				const auto exclude = core::trim(line);

				if (core::is_path(exclude))
				{
					// A syntactic full path is always recorded as an exclude, even
					// when it is not currently present (offline/removable drive or
					// a not-yet-created folder). Requiring existence here silently
					// dropped such excludes, making full-path exclusion unreliable.
					result.excludes.emplace(core::folder_path(exclude));
				}
				else
				{
					result.exclude_wildcards.emplace(core::cache(line));
				}
			}
			else
			{
				const auto path = core::folder_path(line);

				if (platform::exists(path))
				{
					result.folders.emplace(path);
				}
				else
				{
					// Could be a device/volume label or a server name - resolved below.
					drive_label_includes.emplace(line);
				}
			}
		}
	}

	// Resolve device/volume labels to the matching drive(s) by volume name.
	core::hash_set<std::string, core::ihash, core::ieq> resolved_labels;

	for (const auto& d : drives)
	{
		if (drive_label_includes.contains(d.vol_name))
		{
			result.folders.emplace(core::folder_path(d.name));
			resolved_labels.emplace(d.vol_name);
		}
	}

	// Remaining bare names that look like a server are added directly. This allows
	// network shares or removable devices whose mapped drive letter can change.
	for (const auto& label : drive_label_includes)
	{
		if (!resolved_labels.contains(label) && core::is_server(label))
		{
			result.folders.emplace(core::folder_path(label));
		}
	}

	for (const auto& exclude : result.excludes)
	{
		result.folders.erase(exclude);
	}
}

void parse_more_folders(core::index_roots& result, const std::string_view more_folders)
{
	parse_more_folders(result, more_folders, platform::scan_drives());
}

static bool pop_invalid_flag(std::atomic<view_invalid>& invalids, const view_invalid flag)
{
	auto expected = invalids.load();
	auto updated = static_cast<view_invalid>(static_cast<uint32_t>(expected) & ~static_cast<uint32_t>(flag));

	while (!invalids.compare_exchange_weak(expected, updated))
	{
		updated = static_cast<view_invalid>(static_cast<uint32_t>(expected) & ~static_cast<uint32_t>(flag));
	}

	return (static_cast<uint32_t>(expected) & static_cast<uint32_t>(flag)) != 0;
}

void app_frame::complete_pending_events()
{
	// Reentrancy guard: a UI-thread wait (ui_wait_for_signal) pumps messages and re-runs the idle
	// action, so this drain can be re-entered while an outer drain is still on the stack. Shallow
	// nesting is legitimate and preserved (e.g. a modal wait started from a queued task still wants
	// its live updates applied), but each level runs queued callbacks that may wait again, so cap the
	// depth to stop unbounded stack growth. Beyond the cap the queued work is left for the next idle
	// pass (queued work re-signals the idle event), which drains it once the stack unwinds.
	constexpr int max_reentrancy_depth = 8;
	core::scope_locked_inc depth(_completing_pending_events);

	if (_completing_pending_events > max_reentrancy_depth)
	{
		_pending_events_deferred = true;
		return;
	}

	try
	{
		if (!platform::is_closing && _app_frame)
		{
			// Only the outermost pass is timed; a nested drain runs inside its parent's measurement.
			std::optional<core::perf_timer> ui_busy;
			if (_completing_pending_events == 1) ui_busy.emplace(render::ui_perf.idle_us, &render::ui_perf.idle_max_us);

			const auto tasks = _ui_queue.dequeue_all();
			core::bump(render::ui_perf.idle_drains);
			core::bump(render::ui_perf.idle_tasks, tasks.size());
			core::record_peak(render::ui_perf.idle_batch_peak, static_cast<uint32_t>(tasks.size()));

			for (const auto& t : tasks)
			{
				try
				{
					const auto wait_us = t.wait_us();
					core::bump(render::ui_perf.idle_wait_us, wait_us);
					core::record_peak(render::ui_perf.idle_wait_max_us,
					                  static_cast<uint32_t>(std::min<uint64_t>(wait_us, UINT32_MAX)));
					t();
				}
				catch (const std::exception& e)
				{
					// Per task, as start_worker does: the batch has already been dequeued, so letting one
					// failure reach the outer handler would silently discard every task queued behind it.
					core::log(__FUNCTION__, e.what());
				}
			}

			// A task may have closed the window - a Debug build's screenshot scene or recording closes it from
			// one - and on_window_destroy has then let go of the views the invalidations below reach.
			if (platform::is_closing) return;

			if (pop_invalid_flag(_invalids, view_invalid::font_size))
			{
				update_font_size();

				// The font faces keep their identity but their pixel size changed. Cached text
				// layouts (e.g. sidebar element labels) are only rebuilt on a text/face change or
				// a DPI change, so reuse the DPI-change reset here to drop the stale layouts. The
				// sidebar/view relayout queued by the same options invalidation then rebuilds them
				// at the new size.
				_view_items->sidebar()->dpi_changed();
				const view_element_event font_changed_event{view_element_event_type::dpi_changed, nullptr};
				element_broadcast(font_changed_event);
			}

			if (pop_invalid_flag(_invalids, view_invalid::status))
			{
				// The status is painted in the frame's status bar, and an open-ended run with no rows yet
				// also says it across the empty list.
				if (_show_bars)
				{
					if (_view == _view_faces) _status_bar->frame()->layout();
					_status_bar->frame()->invalidate();
				}
				_view_frame->frame()->invalidate();

				// The task view on screen reports under one key, so switching to a view that is not
				// running ends what the previous one showed.
				const auto progress = _view->progress();
				if (progress.active) work_progress(&_view, progress.position, progress.total);
				else work_finished(&_view);
			}

			if (pop_invalid_flag(_invalids, view_invalid::animations))
			{
				frame_tick();
			}

			if (pop_invalid_flag(_invalids, view_invalid::options_save))
			{
				update_command_text();
				// That resets labels the command state derives - Run's names the task - so the state is
				// derived again later in this pass, before anything paints.
				invalidate_view(view_invalid::command_state);
				save_options();

				_view_frame->frame()->options_changed();
				_selector_frame->frame()->options_changed();
				_view_edit->options_changed();
				_view_movie->options_changed();
				_navigate1->options_changed();
				_search_edit->options_changed();
				_navigate2->options_changed();
				_navigate3->options_changed();
				_filter_edit->options_changed();
				_item_size->options_changed();
				_face_filter_edit->options_changed();
				_face_size->options_changed();

				for (const auto& [entries, parts] : _bars)
				{
					for (const auto& part : parts)
					{
						if (part.toolbar) part.toolbar->options_changed();
					}
				}

				_state.update_search_is_favorite_or_collection_root();

				if (_search_predictions_frame)
				{
					_search_predictions_frame->frame()->options_changed();
				}
			}

			// A collection's listing of copies is the index's answer, so a file that has lost its last copy leaves it
			// rather than staying listed as a copy someone might delete (#265). Only the collection's: an item from a
			// folder outside it is listed with no count and handed one by presence, so re-listing it would undo
			// the count presence gave it and ask again, without end.
			if (pop_invalid_flag(_invalids, view_invalid::duplicate_counts) && _state.search().is_duplicates() &&
			    !_state.search().has_selector())
			{
				invalidate_view(view_invalid::refresh_items);
			}

			if (pop_invalid_flag(_invalids, view_invalid::refresh_items))
			{
				if (df::file_handles_detached == 0 && ui::command_active == 0)
				{
					_view->refresh_from_source();
				}
				else
				{
					// re-invalidate for later
					invalidate_view(view_invalid::refresh_items);
				}
			}

			if (pop_invalid_flag(_invalids, view_invalid::selector_filter))
			{
				// Layout clamps the scroll against the new content width, so nothing is needed here
				// beyond rebuilding the strip.
				_view_selector->refresh();
			}

			if (pop_invalid_flag(_invalids, view_invalid::selection_list))
			{
				const auto selection_changed = _state.update_selection();

				// The strip marks the selection on its scroll bar, an item scrolled out of its sight included, so it
				// is drawn again whichever view changed the selection.
				if (selection_changed) _selector_frame->frame()->invalidate();

				if (selection_changed && _state.view_mode() == view_type::tags)
				{
					_view_tags->refresh();
					view_changed(view_type::tags);
				}
				else if (selection_changed && _state.view_mode() == view_type::batch &&
				         _view_batch->mode() == batch_tool_mode::metadata)
				{
					_view_batch->refresh();
					view_changed(view_type::batch);
				}
			}

			if (pop_invalid_flag(_invalids, view_invalid::image_compare))
			{
				_state.update_pixel_difference();
			}

			if (pop_invalid_flag(_invalids, view_invalid::screen_saver))
			{
				const auto display = _state.display_state();
				const auto is_playing = display && (display->is_playing() || display->is_running_c64());
				platform::enable_screen_saver(!_state.is_full_screen && !is_playing);
			}

			if (pop_invalid_flag(_invalids, view_invalid::sidebar))
			{
				const auto& sidebar = _view_items->sidebar();
				sidebar->populate();
				sidebar->layout();
				_view_explore->collection_changed();
				auto search = _state.search();
				// Finding an area can take items away from what is listed - one searched without - and so waits for
				// a view that gives way without asking, rather than asking each time the sidebar is drawn again.
				if (_state.item_index.thumbnailing_items.load() == 0 && _state.resolve_area_search(search) &&
				    (view_gives_way() || view_state::finding_areas_only_adds(_state.search())))
				{
					_state.history.replace_current_search(_state.search(), search);
					_state.open(_view_frame, search, {});
				}
			}

			if (pop_invalid_flag(_invalids, view_invalid::sidebar_file_types_and_dates))
			{
				const auto& sidebar = _view_items->sidebar();
				sidebar->populate_file_types_and_dates();
				sidebar->layout();
				_view_explore->collection_changed();
			}

			if (pop_invalid_flag(_invalids, view_invalid::sidebar_drives))
			{
				_view_items->sidebar()->populate_drives();
			}

			if (pop_invalid_flag(_invalids, view_invalid::sidebar_counts))
			{
				_view_items->sidebar()->queue_update_predictions();
			}

			if (pop_invalid_flag(_invalids, view_invalid::index_summary))
			{
				_state.invalidate_day_counts();
				_state.item_index.queue_update_predictions();
				_state.item_index.queue_update_summary();
				// Pictures the index has found or seen change since are read for faces.
				_state.item_index.faces().start_scan();

				// The names the collection holds may be new: the Faces view and the suggestions made against them
				// are made again while it is shown, and as it is shown otherwise.
				_state.item_index.faces().names_may_have_changed();
				if (_state.view_mode() == view_type::faces)
				{
					_state.item_index.faces().queue_overview();
					_view_faces->model_changed();
				}
			}

			if (pop_invalid_flag(_invalids, view_invalid::faces))
			{
				_view_faces->faces_changed();
				_view_items->sidebar()->faces_changed();

				// docs/design.md#system-states-and-consistency: what person:, face: and @face list, and the sidebar's
				// counts of people, follow each grouping face search publishes - once a minute while it scans, and
				// after each change the user makes - rather than each picture it reads.
				const auto overview = _state.item_index.faces().current_overview();
				if (_faces_listed.owner_before(overview) || overview.owner_before(_faces_listed))
				{
					_faces_listed = overview;
					if (_state.is_items_or_media_view() && _state.search().reads_faces())
						invalidate_view(view_invalid::refresh_items);
					invalidate_view(view_invalid::sidebar_counts);
				}
			}

			if (pop_invalid_flag(_invalids, view_invalid::media_elements))
			{
				_view->update_media_elements();
				invalidate_view(view_invalid::view_layout | view_invalid::view_redraw | view_invalid::controller);
			}

			if (pop_invalid_flag(_invalids, view_invalid::command_state))
			{
				update_button_state(false);
			}

			if (pop_invalid_flag(_invalids, view_invalid::item_scan))
			{
				_item_index.queue_scan_listed_items(_state.display_items());
			}

			if (pop_invalid_flag(_invalids, view_invalid::presence))
			{
				// Nothing to match against until the cache has merged and the folders are indexed: the
				// pass would resolve every item to unknown and then decline to conclude anything, having
				// contended for the index locks with the thread still building it. Each index phase that
				// completes raises this flag again.
				if (_item_index.is_init_complete())
				{
					_item_index.queue_update_presence(_state.display_items());
				}
			}

			const auto update_groups = pop_invalid_flag(_invalids, view_invalid::group_layout);
			const auto groups_updated = pop_invalid_flag(_invalids, view_invalid::group_layout_complete);
			if (update_groups || groups_updated)
			{
				if (update_groups)
				{
					_state.update_item_groups();
					_state.update_selection();

					// docs/locations.md#visits: the timeline is derived from the result set, so it is
					// re-derived exactly when the result set changes and never during paint.
					_state.refresh_visits();
				}
				_view->items_changed(false);
				_view_selector->items_changed(false);
				invalidate_view(view_invalid::view_layout | view_invalid::controller);
			}

			if (pop_invalid_flag(_invalids, view_invalid::address))
			{
				_state.update_search_is_favorite_or_collection_root();
				if (_search_edit && !_search_edit->has_focus())
				{
					_search_edit->window_text(_state.search().text());
				}
				invalidate_view(view_invalid::command_state);
			}

			if (pop_invalid_flag(_invalids, view_invalid::app_layout))
			{
				_app_frame->layout();
			}

			if (pop_invalid_flag(_invalids, view_invalid::view_layout))
			{
				if (_view_frame)
				{
					_view_frame->layout();
					_selector_frame->layout();
				}

				// Every change of thumbnail size relays the view out, whichever control made it.
				update_item_size_slider();
			}

			{
				// A rebuild is a superset of an update, so pop both and run only the stronger one.
				const auto rebuild = pop_invalid_flag(_invalids, view_invalid::index_rebuild);
				const auto update = pop_invalid_flag(_invalids, view_invalid::index);

				if (rebuild) rebuild_index();
				else if (update) update_index();
			}

			if (pop_invalid_flag(_invalids, view_invalid::focus_item_visible))
			{
				if (_state.focus_item())
				{
					make_visible(_state.focus_item());
				}
			}

			if (pop_invalid_flag(_invalids, view_invalid::view_redraw))
			{
				_view_frame->frame()->invalidate();
			}

			if (pop_invalid_flag(_invalids, view_invalid::controller))
			{
				// needs to be last to allow layout
				_view_frame->invalidate_controller();

				// The strip scrolls and rebuilds independently, so its controller goes stale on the same
				// events; without this a click after a scroll still targets the item that was under the
				// pointer before it.
				_selector_frame->invalidate_controller();
			}

			if (pop_invalid_flag(_invalids, view_invalid::tooltip))
			{
				update_tooltip();
			}
		}
	}
	catch (std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	if (_completing_pending_events == 1 && _pending_events_deferred.exchange(false))
	{
		platform::queue_idle();
	}
}

void app_frame::idle()
{
	update_overlay();
	complete_pending_events();
	if (_app_frame && _app_frame->is_visible() && !platform::is_closing)
	{
		const auto position = _app_frame->client_to_screen(_app_frame->cursor_location());
		update_tooltip_visibility(core::tick_count64(), position, _app_frame->scale_factor());
	}
	else
	{
		_tooltip_dwell.reset();
		if (_bubble && _bubble->is_visible()) _bubble->hide();
	}
	mark_startup_settled();
}

// Idle with the window up means the message loop drained a full cycle including the first paint,
// which is the earliest point at which this launch can be said to have reached the user.
void app_frame::mark_startup_settled()
{
	if (_startup_settled || !_app_frame || !_app_frame->is_visible()) return;

	_startup_settled = true;

	// Cleared by any launch that reached the user, not only by the one that recorded an attempt: a
	// count left raised by a concurrent launch would eventually reset a working install's settings,
	// which is the failure this backstop must not cause. The scope is released either way.
	_settings->write({}, s_unsettled_starts, static_cast<uint32_t>(0));

	if (_owns_startup_scope)
	{
		platform::release_startup_scope();
		_owns_startup_scope = false;
	}

	if (_safe_start) report_safe_start();
}

void app_frame::report_safe_start()
{
	// On the next turn, so the idle pass that detected it finishes first.
	queue_ui([this]
	{ make_dlg(_app_frame)->show_message(icon_index::error, tt.safe_start_title, tt.safe_start_message); });
}

bool control_takes_key(const ui::control& c, const char32_t key, const platform::key_state keys)
{
	if (c.traits.typing || keys.control || keys.alt) return false;
	if (key == platform::keys::RETURN) return c.traits.push_button || dynamic_cast<const ui::toolbar*>(&c) != nullptr;
	return key == platform::keys::SPACE || key == platform::keys::LEFT || key == platform::keys::RIGHT ||
	       key == platform::keys::UP || key == platform::keys::DOWN || key == platform::keys::HOME ||
	       key == platform::keys::END || key == platform::keys::PRIOR || key == platform::keys::NEXT;
}

commands app_frame::close_shortcut_target() const
{
	if (!_state.is_items_or_media_view()) return commands::view_close;
	if (_state.is_full_screen) return commands::view_fullscreen;
	const auto display = _state.display_state();
	if (display && (display->is_zoom_mode() || display->is_playing_slideshow())) return commands::none;
	return commands::exit;
}

bool app_frame::key_down(const char32_t key, const platform::key_state keys)
{
	// Ctrl is itself a key press, so this is where the modifier that arms drawing a region first
	// becomes known - and the pointer may not move again before the user acts on it.
	_view_frame->sync_modifier_cursor();

	if (ui::command_active == 0)
	{
		// The filter box is an edit, so it reaches here first only for the keys an edit hands up; the
		// rest it handles itself.
		if (_focused_filter && filter_key_down(key, keys)) return true;

		// Step the prediction list without committing. The address bar is drawn by the items view,
		// so the arrow and tab keys have to be claimed before the view sees them as text input.
		const auto step_predictions = [this](const int delta)
		{
			if (_search_predictions_frame)
			{
				_search_predictions_frame->step_selection(delta);
			}

			return true;
		};

		if (_search_has_focus)
		{
			if (key == platform::keys::RETURN)
			{
				search_enter();
				return true;
			}
			if (key == platform::keys::ESCAPE)
			{
				cancel_search_edit();
				return true;
			}
			if (key == platform::keys::UP) return step_predictions(-1);
			if (key == platform::keys::DOWN) return step_predictions(1);
			// Tab takes a completion the arrows have previewed into the draft; with none previewed it moves on, as
			// from any field, rather than putting whichever completion the list starts on into the box.
			if (key == platform::keys::TAB && !keys.control && !keys.shift && _search_session.is_previewing() &&
			    search_accept_selected())
				return true;
		}

		// Apps and Shift+F10 open the menu a right-click on what has the keyboard opens.
		if ((key == platform::keys::APPS || (key == platform::keys::F10 && keys.shift)) && !keys.control && !keys.alt)
		{
			open_keyboard_menu();
			return true;
		}

		if (const auto control = keyboard_control())
		{
			if (control_takes_key(*control, key, keys)) return false;

			// Escape takes the keyboard back to the view from the title and status bars. In a task's panel it keeps
			// its meaning for the task.
			if (key == platform::keys::ESCAPE && !keys.alt && !_view_controls_have_focus)
			{
				focus_view();
				return true;
			}
		}

		// The drawn part the keyboard is on, or the strip, has its keys before the view does.
		if (_view_frame->frame()->has_focus() && _view_frame->keyboard_part_key_down(key, keys)) return true;

		if (_selector_frame->frame()->has_focus())
		{
			if (key == platform::keys::ESCAPE && !keys.alt)
			{
				focus_view();
				return true;
			}

			if (_view_selector->key_down(key, keys)) return true;
		}

		// Enter submits the task view the user is in, so a task can be completed without leaving the
		// keyboard. Routed through the run command so a run the toolbar refuses stays refused, and
		// so a single-line entry field does not just beep.
		if (key == platform::keys::RETURN && !keys.control && !keys.shift && !keys.alt)
		{
			const auto run_command = task_view_run_command();

			if (run_command != commands::none)
			{
				const auto found = _commands.find(run_command);
				if (found != _commands.end() && found->second->enable)
				{
					invoke(run_command);
				}

				return true;
			}
		}

		// Text being edited in the view has every key, but for a Tab it does not use, which moves on as it does from
		// any field.
		const auto close_shortcut = key == close_view_shortcut().key && keys.control && !keys.shift && !keys.alt;
		if (!close_shortcut && _view_has_focus && _view->focus_mode() == ui::focus_mode::text_edit)
		{
			const auto taken = _view->key_down(key, keys);
			if (taken || key != platform::keys::TAB) return taken;
		}

		// A running Commodore 64 has the keyboard while the view does: the arrows and Space are its joystick,
		// and Escape leaves it before Escape's other meanings are reached.
		if (_view_has_focus && _state.c64_key_down(key, keys)) return true;

		if (close_shortcut && _state.is_items_or_media_view())
		{
			const auto target = close_shortcut_target();
			if (target == commands::none) _state.escape(_view_frame);
			else if (target == commands::exit && _mcp) core::log("key_down", agent_command_refusal(*_mcp, target));
			else invoke(target);
			return true;
		}

		// Tab moves the keyboard on round the window, through every frame of it, and Shift+Tab back.
		if (key == platform::keys::TAB && !keys.control && !keys.alt)
		{
			move_keyboard(!keys.shift);
			return true;
		}

		if (key == platform::keys::ESCAPE)
		{
			if (_selector_frame->escape_controller()) return true;
			if (_view_frame->escape_controller()) return true;

			// view_state::escape owns the unwind ladder (slideshow, zoom, full screen, ...)
			// so there is one order rather than two competing ones.
			if (_view->escape()) return true;

			const auto view_mode = _state.view_mode();
			if (view_mode != view_type::items && view_mode != view_type::media)
			{
				_view->exit();
				return true;
			}

			if (_state.escape(_view_frame)) return true;
		}

		if (_view_has_focus && _view->key_down(key, keys)) return true;

		if (!_view_controls_have_focus && _state.is_items_or_media_view())
		{
			if (key == platform::keys::SPACE && _view_frame->key_down_controller(key, keys)) return true;

			const auto display = _state.display_state();
			if (_view_has_focus && display && display->is_zoom_mode() && !keys.control && !keys.shift && !keys.alt)
			{
				constexpr double arrow_step = 48.0;
				if (key == platform::keys::LEFT) display->pan_zoom_by({-arrow_step, 0.0});
				else if (key == platform::keys::RIGHT) display->pan_zoom_by({arrow_step, 0.0});
				else if (key == platform::keys::UP) display->pan_zoom_by({0.0, -arrow_step});
				else if (key == platform::keys::DOWN) display->pan_zoom_by({0.0, arrow_step});
				else if (key == platform::keys::PRIOR) _state.select_next_media(_view_frame, false);
				else if (key == platform::keys::NEXT) _state.select_next_media(_view_frame, true);
				else if (key == platform::keys::HOME) display->pan_zoom_to_horizontal_edge(false);
				else if (key == platform::keys::END) display->pan_zoom_to_horizontal_edge(true);
				else goto no_zoom_pan_key;
				return true;
			}

		no_zoom_pan_key:
			if (key == platform::keys::BROWSER_BACK)
			{
				_state.browse_back(_view_frame);
				return true;
			}
			if (key == platform::keys::BROWSER_FORWARD)
			{
				_state.browse_forward(_view_frame);
				return true;
			}
			if (key == platform::keys::BROWSER_REFRESH)
			{
				reload();
				return true;
			}
			if (key == platform::keys::BROWSER_HOME)
			{
				invoke(commands::view_fullscreen);
				return true;
			}
			if (key == platform::keys::BROWSER_STOP || key == platform::keys::MEDIA_STOP)
			{
				const auto current_display = _state.display_state();
				if (current_display && current_display->is_playing_media()) _state.pause();
				return true;
			}
			if (key == platform::keys::MEDIA_PLAY_PAUSE)
			{
				_state.play(_view_frame);
				return true;
			}
			if (key == platform::keys::MEDIA_PREV_TRACK)
			{
				_state.select_next(_view_frame, false, keys.control, keys.shift);
				return true;
			}
			if (key == platform::keys::MEDIA_NEXT_TRACK)
			{
				_state.select_next(_view_frame, true, keys.control, keys.shift);
				return true;
			}
			if (key == platform::keys::RETURN && !keys.control && !keys.shift)
			{
				if (_state.enter(_view_frame)) return true;
			}

			if (_view_has_focus)
			{
				if (!keys.alt)
				{
					// In Items the keys move through the items as through any list: alone they select what they
					// reach, Shift extends from the anchor and Ctrl with Shift adds the run, Ctrl alone moves the
					// focus, and Ctrl+Space puts the focused item in the selection or takes it out. The media view,
					// which shows no list, steps through the items with Ctrl putting each one reached in or out.
					const auto in_items = _state.view_mode() == view_type::items;
					const auto focus_only = in_items && keys.control && !keys.shift;

					if (key == platform::keys::LEFT)
					{
						if (focus_only) _state.focus_next(false);
						else _state.select_next(_view_frame, false, keys.control, keys.shift);
						return true;
					}
					if (key == platform::keys::RIGHT)
					{
						if (focus_only) _state.focus_next(true);
						else _state.select_next(_view_frame, true, keys.control, keys.shift);
						return true;
					}
					if (key == platform::keys::HOME)
					{
						if (focus_only) _state.focus_end(false);
						else _state.select_end(_view_frame, false, keys.control, keys.shift);
						return true;
					}
					if (key == platform::keys::END)
					{
						if (focus_only) _state.focus_end(true);
						else _state.select_end(_view_frame, true, keys.control, keys.shift);
						return true;
					}

					if (in_items)
					{
						if (key == platform::keys::SPACE && keys.control && !keys.shift)
						{
							_state.toggle_focus(_view_frame);
							return true;
						}
						if (key == platform::keys::UP)
						{
							_state.stop_slideshow();
							_view_items->line_up(keys.control, keys.shift);
							_state.make_visible(_state.focus_item());
							return true;
						}
						if (key == platform::keys::DOWN)
						{
							_state.stop_slideshow();
							_view_items->line_down(keys.control, keys.shift);
							_state.make_visible(_state.focus_item());
							return true;
						}
					}
				}
			}
		}

		const auto normalized_key = platform::keys::normalize_numpad(key);
		const auto display = _state.display_state();
		// As for the bars: a task view opened over a zoomed photo keeps its own shortcuts.
		const auto zoom_mode = _state.is_items_or_media_view() && display && display->is_zoom_mode();

		if (zoom_mode && !keys.control && !keys.alt)
		{
			if (!keys.shift && normalized_key == '0')
			{
				display->toggle_zoom_fit();
				return true;
			}
			if (normalized_key == platform::keys::OEM_PLUS)
			{
				display->adjust_zoom_scale(1);
				return true;
			}
			if (!keys.shift && normalized_key == platform::keys::OEM_MINUS)
			{
				display->adjust_zoom_scale(-1);
				return true;
			}
		}

		for (const auto& c : _commands)
		{
			const auto zoom_command = c.second->group == command_group::rate_flag || c.first == commands::view_zoom ||
			                          c.first == commands::view_zoom_toggle_fit || c.first == commands::view_zoom_100 ||
			                          c.first == commands::view_zoom_in || c.first == commands::view_zoom_out ||
			                          c.first == commands::view_zoom_pane_flip || c.first == commands::view_fullscreen;
			if (zoom_mode && !zoom_command) continue;

			for (const auto& ac : c.second->kba)
			{
				if (ac.key == normalized_key)
				{
					const auto key_state_match = ((ac.key_state & platform::keyboard_accelerator_t::control) != 0) ==
					                                 keys.control &&
					                             ((ac.key_state & platform::keyboard_accelerator_t::shift) != 0) ==
					                                 keys.shift &&
					                             ((ac.key_state & platform::keyboard_accelerator_t::alt) != 0) ==
					                                 keys.alt;

					if (key_state_match && c.second->enable)
					{
						invoke(c.second);
						return true;
					}
				}
			}
		}

		if (zoom_mode) return true;
	}

	return false;
}

bool app_frame::text_input(const std::string_view text)
{
	return _view_has_focus && _view && _view->text_input(text);
}

ui::focus_mode app_frame::focus_mode() const
{
	if (_search_has_focus || _focused_filter) return ui::focus_mode::text_edit;
	if (_view_has_focus && _view) return _view->focus_mode();
	return ui::focus_mode::none;
}

void app_frame::toggle_full_screen()
{
	_state.is_full_screen = !_state.is_full_screen;

	if (_state.is_full_screen)
	{
		if (!_state.has_selection() && _state.has_display_items())
		{
			// Media view needs something to show. Remember what full screen had to pick so it is
			// not left behind afterwards as a target the user never chose.
			_state.select_next(_view_frame, true, false, false);
			_full_screen_auto_selected = _state.focus_item();
		}

		_state.view_mode(view_type::media);
		_view_frame->tick();
	}
	else
	{
		// Leaving full screen returns to browsing only when full screen was the media view. A task
		// view entered from full screen keeps its own mode and the selection it is working on.
		const auto was_media = _state.view_mode() == view_type::media;

		if (_full_screen_auto_selected)
		{
			const auto& selected = _state.selected_items();

			if (was_media && selected.size() == 1 && selected.items().front() == _full_screen_auto_selected)
			{
				_state.select_nothing(_view_frame);
			}

			_full_screen_auto_selected.reset();
		}

		if (was_media)
		{
			_state.view_mode(view_type::items);
		}
	}

	// Full screen swaps to the media view and hides both bars, so release text focus first.
	if (_search_has_focus || _focused_filter)
	{
		focus_view();
	}

	// A tutorial's window stays the size it is recorded at, and looks as full screen does on a screen that size.
	if (_tutorial)
	{
		invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::screen_saver |
		                view_invalid::command_state);
		return;
	}

	_app_frame->full_screen(_state.is_full_screen);
	invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::screen_saver |
	                view_invalid::command_state);
}

void app_frame::open_default_folder()
{
	bool success = false;

	if (!command_line.selection.is_empty())
	{
		success = _state.open_external_file(_view_frame, command_line.selection);
	}
	else if (!command_line.folder_path.is_empty())
	{
		// An empty entry is not "no selection": append_items reads a non-empty set as an explicit
		// list of paths to select, so a folder-only command line must pass an empty set.
		core::unique_paths selection;
		if (!command_line.selection.is_empty()) selection.emplace(command_line.selection);
		success = _state.open(_view_frame, df::search_t().add_selector(command_line.folder_path), selection);
	}

	if (!success)
	{
		if (!saved_current_search.empty())
		{
			const auto search = df::search_t::parse(saved_current_search);

			if (!search.is_empty())
			{
				if (search.has_related())
				{
					// Opening related needs loading of related fields
					_state.open(_view_frame, search.related().path);
					success = true;
				}
				else
				{
					success = _state.open(_view_frame, search, {});
				}
			}
		}
	}

	if (!success)
	{
		_state.open(_view_frame,
		            df::search_t().add_selector(core::item_selector(known_path(platform::known_folder::pictures))), {});
	}
}

inline void app_frame::make_visible(const df::item_element_ptr& i)
{
	_view_items->make_visible(i);
}

bool app_frame::is_command_checked(const commands id)
{
	const auto command = find_command(id);
	return command && command->checked;
}

void app_frame::element_broadcast(const view_element_event& event)
{
	_view_items->broadcast_event(event);
	_view_selector->broadcast_event(event);
	_view_edit->broadcast_event(event);
	_view_movie->broadcast_event(event);
	_view_media->broadcast_event(event);
	_view_rename->broadcast_event(event);
	_view_sync->broadcast_event(event);
	_view_import->broadcast_event(event);
	_view_locate->broadcast_event(event);
	_view_tags->broadcast_event(event);
	_view_explore->broadcast_event(event);
	_view_faces->broadcast_event(event);
}

void app_frame::focus_changed(const bool has_focus, const ui::control_ptr& child)
{
	core::trace(std::format("app_frame::focus {}", has_focus));

	if (child == _search_edit)
	{
		focus_search(has_focus);
	}
	else if (child && (child == _filter_edit || child == _face_filter_edit))
	{
		focus_filter(child, has_focus);
	}

	// The view frame holds the keyboard for the view, for a drawn part around it and for its panel's controls alike.
	const auto panel_focus = _view_frame->frame()->focused_control();
	_view_has_focus = _view_frame->frame()->has_focus() && !panel_focus &&
	                  _view_frame->_keyboard.area == keyboard_area::view;
	_view_controls_have_focus = panel_focus != nullptr;

	invalidate_view(view_invalid::view_redraw | view_invalid::command_state);
}

void app_frame::item_focus_changed(const df::item_element_ptr& focus, const df::item_element_ptr& previous)
{
	core::trace("app_frame::focus_changed");

	if (_view_items->is_visible(previous))
	{
		_view_items->make_visible(focus);
	}

	_view_selector->make_visible(focus);
}

app_frame::selector_strip app_frame::selector_strip_for_view(const view_type m) const
{
	switch (m)
	{
	case view_type::edit: return selector_strip::edit;
	case view_type::locate:
	case view_type::tags: return selector_strip::metadata;
	case view_type::batch:
		return _view_batch->mode() == batch_tool_mode::metadata ? selector_strip::metadata : selector_strip::none;
	// Movie draws its own strip. That strip is the document -- ordered, duplicable, and removing a
	// tile removes it from the movie -- so sharing the selector would make one click mean "select"
	// in every other view and "seek" here.
	case view_type::movie: return selector_strip::none;
	default: return selector_strip::none;
	}
}

void app_frame::reset_selector_selection_anchor()
{
	_view_selector->reset_selection_anchor();
}

void app_frame::select_from_selector(const df::item_element_ptr& item, const platform::key_state keys)
{
	if (!item || !_view_selector->is_enabled()) return;

	switch (selector_strip_for_view(_state.view_mode()))
	{
	case selector_strip::edit:
		// Edit works on one item, so the strip navigates rather than selects.
		if (can_edit_in_edit_view(item)) _view_edit->select_item(item);
		break;

	case selector_strip::metadata:
		if (!can_edit_metadata_of(item)) break;

		if (keys.shift)
		{
			// The range is measured across the strip, so Shift can never reach an item the strip
			// filtered out and add it to what the task will write.
			_state.select(_selector_frame, _view_selector->selection_range(item), keys.control);
		}
		else
		{
			_view_selector->selection_anchor(item);

			if (keys.control && item->is_selected())
			{
				_state.unselect(_selector_frame, item);
			}
			else
			{
				_state.select(_selector_frame, item, keys.control, false, false);
			}
		}
		break;

	case selector_strip::none: break;
	}
}

void app_frame::display_changed()
{
	core::assert_true(platform::is_ui_thread());

	if (!platform::is_closing)
	{
		_view->display_changed();
		_view_selector->display_changed();

		// The picture on screen is read for faces next: docs/faces.md#32-the-scan.
		if (const auto d = _state.display_state(); d && d->_item1)
			_state.item_index.faces().prioritize(d->_item1->path());

		invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::command_state |
		                view_invalid::screen_saver | view_invalid::media_elements | view_invalid::controller |
		                view_invalid::tooltip);
	}
};

void app_frame::reload()
{
	_view->reload();

	// Refresh is the user's recovery from anything stale, so it re-reads what is otherwise only
	// refreshed by an event: volume free space and labels change without notifying us at all, and
	// the cached optical-drive answer is refreshed on the same grounds.
	_has_burner.reset();
	invalidate_view(view_invalid::view_layout | view_invalid::group_layout | view_invalid::index |
	                view_invalid::refresh_items | view_invalid::item_scan | view_invalid::sidebar_drives |
	                view_invalid::command_state);
}

void app_frame::view_changed(const view_type m)
{
	core::assert_true(platform::is_ui_thread());
	core::assert_true(m != view_type::media || _state.is_full_screen);

	// Opening Locate on items that already carry a location must not hide them. A selector strip
	// that cannot show the item being placed reads as "nothing here" rather than as a live filter.
	if (m == view_type::locate && setting.locate_only_without_location)
	{
		const auto& selected = _state.selected_items().items();
		const auto focus = _state.focus_item();

		if ((focus && focus->has_gps()) ||
		    std::any_of(selected.begin(), selected.end(), [](const df::item_element_ptr& i) { return i->has_gps(); }))
		{
			setting.locate_only_without_location = false;
			invalidate_view(view_invalid::options_save);
		}
	}

	switch (selector_strip_for_view(m))
	{
	case selector_strip::edit:
		_view_selector->filter(can_edit_in_edit_view);
		_view_selector->activate(_selector_frame->_extent);
		break;

	case selector_strip::metadata:
		if (m == view_type::locate) _view_selector->filter(locate_offers);
		else _view_selector->filter(can_edit_metadata_of);
		_view_selector->activate(_selector_frame->_extent);
		break;

	case selector_strip::none:
		// A hidden strip holds no items: item visibility and the thumbnail queue belong to the view
		// that is actually on screen.
		_view_selector->deactivate();
		_view_selector->filter({});
		break;
	}

	auto v = _view;
	view_controls_host_ptr vc;

	switch (m)
	{
	case view_type::items: v = _view_items; break;
	case view_type::media: v = _view_media; break;
	case view_type::edit:
		v = _view_edit;
		vc = _view_edit->controls(_view_frame->frame());
		break;
	case view_type::movie:
		v = _view_movie;
		vc = _view_movie->controls(_view_frame->frame());
		break;
	case view_type::rename:
		v = _view_rename;
		vc = _view_rename->controls(_view_frame->frame());
		break;
	case view_type::batch:
		v = _view_batch;
		vc = _view_batch->controls(_view_frame->frame());
		break;
	case view_type::import:
		v = _view_import;
		vc = _view_import->controls(_view_frame->frame());
		break;
	case view_type::sync:
		v = _view_sync;
		vc = _view_sync->controls(_view_frame->frame());
		break;
	case view_type::locate:
		v = _view_locate;
		vc = _view_locate->controls(_view_frame->frame());
		break;
	case view_type::tags:
		v = _view_tags;
		vc = _view_tags->controls(_view_frame->frame());
		break;
	case view_type::explore: v = _view_explore; break;
	case view_type::faces:
		v = _view_faces;
		vc = _view_faces->controls(_view_frame->frame());
		break;
	case view_type::options:
		v = _view_options;
		vc = _view_options->controls(_view_frame->frame());
		break;
	default: break;
	}

	if (vc != _view_frame->_panel)
	{
		// The panel is drawn in the view's frame, which the app layout lays out again with it. Asked for
		// here rather than with the view change below, because a panel is also rebuilt without the view
		// changing: a new selection in Tags or Batch, or a language change. A part of the panel it replaces
		// can have the keyboard no longer.
		if (_view_frame->_keyboard.area == keyboard_area::panel) _view_frame->keyboard_to(keyboard_area::view, nullptr);
		_view_frame->panel(vc, m == view_type::options);
		invalidate_view(view_invalid::app_layout);
	}

	if (v != _view)
	{
		if (_view)
		{
			_view->deactivate();
		}

		_view = v;
		_view_frame->view(v);
		_view->activate(_view_frame->_view_bounds.extent());
		// Offscreen only. The items the user was just looking at are the ones the media view is about to
		// step through, and their thumbnail surfaces are what stands in for each image while it loads.
		free_graphics_resources(true, true);
		focus_view();

		invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::group_layout |
		                view_invalid::command_state | view_invalid::media_elements | view_invalid::tooltip |
		                view_invalid::controller | view_invalid::address);
	}
}

void app_frame::language_changed(const std::string_view lang_code)
{
	_state.display_language_changed();
	view_changed(_state.view_mode());
	queue_location([this, lang_code = std::string(lang_code)](location_cache& lc)
	{
		lc.set_display_language(lang_code);
		invalidate_view(view_invalid::sidebar);
	});
	// Explicit, because visual_options no longer carries the sidebar: every sidebar label is translated,
	// so a language change is one of the few option changes that really does rebuild it.
	invalidate_view(view_invalid::visual_options | view_invalid::sidebar);
}

void app_frame::select_language(const std::string_view lang_code)
{
	setting.language = lang_code;

	if (core::icmp(lang_code, "en") == 0)
	{
		tt.clear();
	}
	else
	{
		const auto lang_folder = known_path(platform::known_folder::running_app_folder).combine("languages");
		const auto lang_path = lang_folder.combine_file_ext(lang_code, ".po");
		tt.load_lang(lang_path.name(), load_po(lang_path));
		register_file_group_spellings();
	}

	language_changed(lang_code);
}

void app_frame::invoke(const command_info_ptr& command)
{
	try
	{
		if (command->enable)
		{
			core::log(__FUNCTION__, command->text);

			core::scope_locked_inc sl(ui::command_active);
			command->invoke();
			invalidate_view(view_invalid::command_state);
		}
	}
	catch (std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}
}

void app_frame::track_menu(const ui::frame_ptr& parent, const recti bounds,
                           const std::vector<ui::command_ptr>& commands)
{
	if (!parent) return;

	// What is playing goes on under a menu, which only holds the sequence where it is (view_state::tick). A
	// command chosen from it pauses, or lets go of, the playing file as it needs to.
	complete_pending_events(); // process any menu updates
	parent->track_menu(bounds, commands);
}

void app_frame::invoke(const commands id)
{
	const auto command = find_command(id);

	if (command)
	{
		invoke(command);
	}
}

void app_frame::report_scope_unavailable(const df::search_t& path)
{
	// Shown on the next UI turn so the failed open() call can unwind first.
	queue_ui([this, path]
	{
		const auto dlg = make_dlg(_app_frame);

		// Whatever was playing belongs to the scope that just failed, so it stops while the
		// question is on screen, like every other modal.
		pause_media pause(_state);

		enum class choice
		{
			none,
			retry,
			parent
		};
		auto selected = std::make_shared<choice>(choice::none);

		// The parent of the scope that failed, not of the scope still on screen.
		const auto parent = find_parent_search(path);

		std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::error, tt.scope_unavailable_title,
		                                                    str_format(tt.scope_unavailable_fmt.sv(), path.text()))),
			std::make_shared<divider_element>(),
			set_margin(std::make_shared<link_element>(tt.scope_unavailable_retry,
		                                              [dlg, selected]
		{
			*selected = choice::retry;
			dlg->close(false);
		})),
		};

		if (!parent.parent.is_empty())
		{
			controls.emplace_back(set_margin(std::make_shared<link_element>(tt.scope_unavailable_parent, [dlg, selected]
			{
				*selected = choice::parent;
				dlg->close(false);
			})));
		}

		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<ui::close_control>(dlg->_frame));

		dlg->show_modal(controls, {44}, {44});

		if (*selected == choice::retry)
		{
			_state.open(_view_frame, path, {});
		}
		else if (*selected == choice::parent)
		{
			_state.open(_view_frame, parent.parent, make_unique_paths(parent.selection));
		}
	});
}

bool app_frame::can_open_search(const df::search_t& /*link*/)
{
	if (_view->can_exit()) return true;

	// Say why the new scope was refused rather than appearing to do nothing.
	const auto name = _view->operation_name();

	if (!name.empty())
	{
		make_dlg(_app_frame)
			->show_message(icon_index::question, tt.cancel_operation_title, str_format(tt.scope_busy_fmt.sv(), name));
	}

	return false;
}

void app_frame::folder_changed(const core::folder_path folder)
{
	// A watched tree also reports folders no listing walked into - hidden ones, a sync tool's working
	// folders - and validating one would index it and query the listing again for nothing shown. A folder
	// made since is found by validating its parent, whose change its creation reports.
	const auto& selectors = _state.search().selectors();
	const auto is_watched_folder = std::ranges::any_of(selectors,
	                                                   [folder](const auto& s) { return s.folder() == folder; });
	if (!is_watched_folder && !_item_index.knows_folder(folder)) return;

	// Sync clients and batch tools report many changes in quick succession, so collect the folders and
	// let tick compare them once the burst has settled.
	_folder_change_time = core::now();
	_folders_changed.emplace(folder);
}

void app_frame::dpi_changed()
{
	_view_items->sidebar()->dpi_changed();

	const view_element_event e{view_element_event_type::dpi_changed, nullptr};
	element_broadcast(e);

	invalidate_view(view_invalid::app_layout);
}

void app_frame::on_window_layout(render::measure_context& mc, const sizei extent, const bool is_minimized)
{
	if (!is_minimized)
	{
		_extent = extent;
		layout(mc);
	}
}

// A container: what it shows is its regions'.
void app_frame::on_window_paint(render::draw_context& /*dc*/)
{
}

// The frame paints the parts of the status bar that are not controls: the totals among them, and the
// view's status in the room they leave, which says what the view is doing. A run shows it in a pill: one
// that fills as the count rises, or that pulses while the run cannot yet say how much is left.
void app_frame::draw_status_bar(render::draw_context& dc)
{
	_status_text_clipped = false;
	if (!_show_bars || !_view) return;

	constexpr auto font = render::font_face::dialog;
	const auto text_color = core::color(dc.colors.foreground, dc.colors.alpha);

	if (!_totals_bounds.is_empty())
	{
		dc.draw_text(_totals_text, _totals_bounds, font, render::text_style::single_line_center, text_color, {});
	}

	const auto status = _view->status();
	const auto progress = _view->progress();
	const auto counted = progress.active && progress.total > 0;
	const auto position = std::clamp(progress.position, int64_t{}, std::max(progress.total, int64_t{}));

	std::string text(status);
	if (counted) text = std::format("{}{}{} / {}", status, status.empty() ? "" : "  ", position, progress.total);
	if (text.empty() || _status_text_bounds.is_empty()) return;

	auto text_bounds = _status_text_bounds;

	if (_view == _view_faces && !_receipt_prefix_bounds.is_empty())
	{
		dc.draw_text(_receipt_prefix, _receipt_prefix_bounds, font, render::text_style::single_line, text_color, {});
		dc.draw_text(_receipt_suffix, _receipt_suffix_bounds, font, render::text_style::single_line, text_color, {});
		return;
	}

	if (progress.active)
	{
		// Measured at the final count, so the pill does not twitch every time a digit is added.
		const auto widest = counted ? std::format("{}{}{} / {}", status, status.empty() ? "" : "  ", progress.total,
		                                          progress.total)
		                            : text;
		const auto extent = dc.measure_text(widest, font, render::text_style::single_line, _status_text_bounds.width());
		const auto height = std::min(_status_text_bounds.height(), extent.cy + dc.padding1 * 2);
		const auto top = _status_text_bounds.top + (_status_text_bounds.height() - height) / 2;
		const recti pill(_status_text_bounds.left, top,
		                 std::min(_status_text_bounds.right, _status_text_bounds.left + extent.cx + dc.padding2 * 2),
		                 top + height);

		if (counted)
		{
			dc.draw_rounded_rect(pill, core::color(ui::style::colors.important_background, dc.colors.alpha * 0.3f),
			                     dc.padding1);
			auto done = pill;
			done.right = pill.left + core::mul_div(pill.width(), static_cast<int>(std::min<int64_t>(position, INT_MAX)),
			                                       static_cast<int>(std::min<int64_t>(progress.total, INT_MAX)));
			if (!done.is_empty())
			{
				dc.draw_rounded_rect(done, core::color(ui::style::colors.important_background, dc.colors.alpha),
				                     dc.padding1);
			}
		}
		else
		{
			const auto alpha = command_progress_alpha(progress.active, progress.total, core::tick_count());
			dc.draw_rounded_rect(pill, core::color(ui::style::colors.important_background, dc.colors.alpha * alpha),
			                     dc.padding1);
		}

		text_bounds = pill.inflate(-dc.padding2, 0);
	}

	constexpr auto style = render::text_style::single_line;
	const auto extent = dc.measure_text(text, font, style, 100000);
	_status_text_clipped = extent.cx > text_bounds.width() || extent.cy > text_bounds.height();
	dc.draw_text(text, text_bounds, font, style, text_color, {});
}

void app_frame::title_bar_mouse_move(const pointi loc)
{
	if (!_app_logo) return;

	const auto logo_hover = _title_bounds.contains(loc);

	if (_app_logo->hover(logo_hover))
	{
		if (render::animations_enabled)
		{
			ui::animations[_app_logo.get()] = [logo = _app_logo, frame = _title_bar->frame()]
			{
				const auto animating = logo->step_background();
				if (animating) frame->invalidate(logo->invalidate_bounds());
				return animating;
			};
			invalidate_view(view_invalid::animations);
		}
		else
		{
			if (_app_logo->step_background_if_animations_disabled())
			{
				_title_bar->frame()->invalidate(_app_logo->invalidate_bounds());
			}
		}
	}

	if (_app_logo_hover != logo_hover)
	{
		_app_logo_hover = logo_hover;
		invalidate_view(view_invalid::tooltip);
	}
}

void app_frame::title_bar_mouse_leave()
{
	if (!_app_logo_hover) return;

	_app_logo_hover = false;

	if (_app_logo->hover(false))
	{
		if (render::animations_enabled)
		{
			ui::animations[_app_logo.get()] = [logo = _app_logo, frame = _title_bar->frame()]
			{
				const auto animating = logo->step_background();
				if (animating) frame->invalidate(logo->invalidate_bounds());
				return animating;
			};
			invalidate_view(view_invalid::animations);
		}
		else
		{
			if (_app_logo->step_background_if_animations_disabled())
			{
				_title_bar->frame()->invalidate(_app_logo->invalidate_bounds());
			}
		}
	}

	invalidate_view(view_invalid::tooltip);
}

void app_frame::title_bar_click(const pointi loc)
{
	if (_app_logo && _title_bounds.contains(loc))
	{
		invoke(commands::view_help);
	}
}

void app_frame::activate(const bool is_active)
{
	if (_is_active != is_active)
	{
		_is_active = is_active;
		_view_items->sidebar()->_is_active = is_active;
	}

	invalidate_view(view_invalid::view_redraw);
}

void app_frame::web_service_cache(std::string key, std::function<void(const std::string&)> f)
{
	queue_database([this, key = std::move(key), f = std::move(f)](const database& db)
	{
		auto result = db.web_service_cache(key);

		queue_async(async_queue::cloud, [result, f] { f(result); });
	});
}

void app_frame::web_service_cache(std::string key, std::string value)
{
	queue_database([key = std::move(key), value = std::move(value)](const database& db)
	{ db.web_service_cache(key, value); });
}

void app_frame::work_progress(const void* source, const int64_t done, const int64_t total)
{
	core::assert_true(platform::is_ui_thread());
	_work_reports.update(source, done, total);
	platform::show_progress(done, total);
}

void app_frame::work_finished(const void* source)
{
	core::assert_true(platform::is_ui_thread());
	if (!_work_reports.finish(source)) return;

	if (const auto latest = _work_reports.latest())
	{
		platform::show_progress(latest->done, latest->total);
	}
	else
	{
		platform::end_progress();
	}
}

void app_frame::search_text_changed(const std::string_view text)
{
	if (_search_predictions_frame && _search_has_focus)
	{
		_search_session.typed(text);
		_search_predictions_frame->selected(nullptr, ui::complete_strategy_t::select_type::init);
		_search_predictions_frame->search(std::string(text));
	}
}

void app_frame::delete_items(const df::item_set& items)
{
	const auto title = tt.command_delete;
	auto dlg = make_dlg(_app_frame);
	// Held to the set it was given. A per-item Delete names one item that need not be selected, and
	// validating the selection instead answered about files the command was never going to touch.
	const auto can_process = _state.can_process_items_and_mark_errors(_view_frame, items,
	                                                                  df::process_items_type::local_file_or_folder);

	pause_media pause(_state);

	if (can_process.fail())
	{
		dlg->show_message(icon_index::error, title, can_process.to_string());
	}
	else
	{
		const auto will_recycle = platform::can_recycle(items.file_paths(), items.folder_paths());
		const auto summary = items.summary();
		const auto item_count = (summary.total_items() + summary.total_folders()).count;
		constexpr int64_t many_files_threshold = 50;

		const auto& info_fmt = will_recycle ? tt.delete_info_fmt : tt.delete_info_permanent_fmt;

		std::vector<view_element_ptr> controls;
		auto title_control = std::make_shared<ui::title_control2>(dlg->_frame, icon_index::cancel, title,
		                                                          format_plural_text(info_fmt, items),
		                                                          std::vector<core::const_surface_ptr>{}, items.size());
		title_control->selection_async(items.thumbs(), items.size(), _state._async);
		controls.emplace_back(set_margin(title_control));

		const auto add_warning = [&controls](const std::string_view text)
		{
			auto warning = std::make_shared<text_element>(text, flex_item::stretch | view_element_style::important);
			warning->margin = {10, 10};
			warning->padding = {10, 10};
			warning->update_background_color();
			controls.emplace_back(std::move(warning));
		};

		if (item_count > many_files_threshold)
		{
			add_warning(format_plural_text(tt.delete_many_warning_fmt, items));
		}

		if (!will_recycle)
		{
			add_warning(tt.delete_no_recycle_warning);
		}

		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<ui::ok_cancel_control>(dlg->_frame, tt.button_delete));

		// Skipping the application confirmation is a convenience for deletions that should recycle.
		// The shell still owns the mandatory consent if that requested recycle would become permanent.
		const auto needs_confirmation = setting.confirm_deletions || !will_recycle;

		if (!needs_confirmation || dlg->show_modal(controls) == ui::close_result::ok)
		{
			// Navigation follows the selection, so it only applies when the selection is what is
			// leaving. Deleting one item from its own row must not move the cursor off a selection
			// that is still there.
			const auto deleting_the_selection = !items.empty() && items == _state.selected_items();
			const auto next = deleting_the_selection ? _state.next_unselected_item() : df::item_element_ptr{};

			// Only selector folders are live-watched, so a search that names no folder - related
			// items, duplicates, a tag or a date - has nothing watching it and would keep listing
			// what was just deleted. This is noted before the operation and reported after it
			// whatever the result, because a cancelled or partly failed delete still removes files.
			core::unique_folders touched;
			for (const auto& path : items.file_paths()) touched.emplace(path.folder());
			for (const auto& path : items.folder_paths()) touched.emplace(path.parent());

			auto deleted = false;

			{
				detach_file_handles detach(_state);
				shell_file_operation_ui processing(*_view_frame, _app_frame);

				// Where Windows cannot recycle everything, the dialog above said so and was agreed to: Windows
				// recycles what it can and is not asked to confirm the rest again, so a network delete is one
				// question, not two.
				const auto mode = will_recycle ? platform::delete_mode::recycle
				                               : platform::delete_mode::recycle_where_possible;
				const auto res = platform::delete_items(items.file_paths(), items.folder_paths(), mode);

				if (res.success())
				{
					deleted = true;
					// The display moves on with the cursor below rather than reopening a file that has gone.
					if (_state.displays_one_of(items)) detach.keep_display_closed();
				}
				else if (res.code != platform::file_op_result_code::CANCELLED)
				{
					dlg->show_message(icon_index::error, title, res.format_error(tt.delete_error));
				}

				_state.invalidate_view(view_invalid::view_layout | view_invalid::group_layout |
				                       view_invalid::command_state | view_invalid::app_layout);
			}

			if (deleted) _state.remove_items(_view_frame, items, next);
			_state.item_index.queue_validate_changed_folders(std::move(touched));
		}
	}
}

void app_frame::focus_view()
{
	if (_view_frame)
	{
		_view_frame->keyboard_to(keyboard_area::view, nullptr);
		_view_frame->frame()->focus();
	}
}

std::vector<app_frame::keyboard_stop> app_frame::keyboard_stops() const
{
	std::vector<keyboard_stop> stops;

	const auto add_controls = [&stops](const std::vector<ui::control_ptr>& controls)
	{
		for (const auto& c : controls)
		{
			if (ui::is_tab_stop(*c, controls)) stops.push_back({c});
		}
	};

	const auto add_parts = [this, &stops](const keyboard_area area)
	{
		if (auto parts = _view_frame->keyboard_parts(area); !parts.empty())
			stops.push_back({nullptr, std::move(parts), area});
	};

	if (_show_bars) add_controls(_title_bar->_controls);

	add_parts(keyboard_area::sidebar);
	if (!_view_frame->_view_bounds.is_empty()) stops.emplace_back();
	add_parts(keyboard_area::listing);
	add_parts(keyboard_area::media);
	if (_view_frame->_strip->is_visible() && _view_selector->is_enabled() && _view_selector->has_items())
		stops.push_back({.strip = true});

	// The panel's controls and links in its order; the parts of one element - the stars of a rating - are one stop.
	if (const auto panel = _view_frame->keyboard_region_of(keyboard_area::panel))
	{
		const auto targets = find_keyboard_targets(panel->roots, panel->offset);
		std::vector<ui::control_ptr> controls;

		for (const auto& target : targets)
		{
			if (const auto* const c = std::get_if<ui::control_ptr>(&target)) controls.push_back(*c);
		}

		for (const auto& target : targets)
		{
			if (const auto* const c = std::get_if<ui::control_ptr>(&target))
			{
				if (ui::is_tab_stop(**c, controls)) stops.push_back({*c});
			}
			else if (const auto& part = std::get<keyboard_part>(target);
			         !stops.empty() && !stops.back().parts.empty() &&
			         stops.back().parts.front().element == part.element)
			{
				stops.back().parts.push_back(part);
			}
			else
			{
				stops.push_back({nullptr, {part}, keyboard_area::panel});
			}
		}
	}

	if (_show_bars) add_controls(_status_bar->_controls);
	return stops;
}

int app_frame::current_keyboard_stop(const std::vector<keyboard_stop>& stops)
{
	const auto index_of = [&stops](const auto& is_it)
	{
		const auto found = std::ranges::find_if(stops, is_it);
		return found == stops.end() ? -1 : static_cast<int>(found - stops.begin());
	};

	if (const auto control = keyboard_control())
	{
		const auto group = control->traits.radio_group;
		return index_of([&control, group](const keyboard_stop& stop) {
			return stop.control == control || (group >= 0 && stop.control && stop.control->traits.radio_group == group);
		});
	}

	if (_selector_frame->frame()->has_focus()) return index_of([](const keyboard_stop& stop) { return stop.strip; });
	if (!_view_frame->frame()->has_focus()) return -1;

	if (const auto part = _view_frame->focused_part())
	{
		return index_of([&part](const keyboard_stop& stop)
		{ return std::ranges::any_of(stop.parts, [&part](const keyboard_part& p) { return p.is(*part); }); });
	}

	return index_of([](const keyboard_stop& stop) { return !stop.control && stop.parts.empty() && !stop.strip; });
}

void app_frame::move_keyboard(const bool forward)
{
	const auto stops = keyboard_stops();
	if (stops.empty()) return;

	const auto count = static_cast<int>(stops.size());
	const auto current = current_keyboard_stop(stops);
	const auto next = current < 0 ? (forward ? 0 : count - 1) : (current + (forward ? 1 : count - 1)) % count;
	keyboard_to(stops[next]);
}

void app_frame::keyboard_to(const keyboard_stop& stop)
{
	_view_frame->_keyboard_cues = true;

	if (stop.control)
	{
		stop.control->focus();
		// As Tab within one frame enters it: an edit with its text selected, so typing replaces it.
		if (const auto edit = std::dynamic_pointer_cast<ui::edit>(stop.control)) edit->select_all();
	}
	else if (stop.strip)
	{
		_selector_frame->frame()->focus();
	}
	else
	{
		focus_view();

		if (!stop.parts.empty())
		{
			// A group is entered at its checked part - the sidebar's row for what the listing shows - or else its first.
			const auto checked = std::ranges::find_if(stop.parts, [](const keyboard_part& part)
			{ return part.element->is_style_bit_set(view_element_style::checked); });
			_view_frame->keyboard_to(stop.area, &*(checked != stop.parts.end() ? checked : stop.parts.begin()));
		}
	}
}

ui::control_ptr app_frame::keyboard_control() const
{
	for (const auto& frame : {_title_bar->frame(), _view_frame->frame(), _status_bar->frame()})
	{
		if (auto control = frame->focused_control()) return control;
	}

	return nullptr;
}

// What has the keyboard is where the menu is asked for: a control, the part the keyboard is on, or what the view
// acts on, at its middle where the view names nothing. The strip offers none, as a right-click on it does not.
void app_frame::open_keyboard_menu()
{
	recti bounds;

	if (const auto control = keyboard_control())
	{
		bounds = control->window_bounds();
	}
	else if (_view_frame->frame()->has_focus())
	{
		const auto part = _view_frame->focused_part();
		auto local = part ? part->bounds : _view->keyboard_bounds();
		if (local.is_empty()) local = recti(_view_frame->_view_bounds.center(), sizei{});
		bounds = local.offset(_view_frame->frame()->client_to_screen({}));
	}
	else
	{
		return;
	}

	if (const auto commands = menu(bounds.center()); !commands.empty()) track_menu(_app_frame, bounds, commands);
}

bool app_frame::can_exit()
{
	// Closing the application while a task is running is a decision for the user to make,
	// not something to silently refuse.
	if (!_view->confirm_exit()) return false;
	return !_view_movie || _view == _view_movie || _view_movie->confirm_exit();
}

bool app_frame::edit_has_changes() const
{
	return _view_edit && _view_edit->has_changes();
}

bool app_frame::pre_init()
{
	register_diffractor_icon_font();
	core::log("main", df::format_version(false));

	// Written before anything can fail, so a truncated or crash-terminated log still says which
	// machine shape produced it. Perf numbers are only readable against the core count.
	core::log("main",
	          std::format("windows {} {} {} | {} cores", platform::OS(), sizeof(void*) == 8 ? "64-bit" : "32-bit",
#ifdef _DEBUG
	                      "debug",
#else
	                      "release",
#endif
	                      std::thread::hardware_concurrency()));

	std::setlocale(LC_ALL, "en_US.UTF-8");

	// Built here so the crash handler, which flushes the open-file list, never has to construct it
	// (and read from disk) inside a faulting process.
	(void)crash_files();

	// The renderers scale and convert pictures with FFmpeg's swscale, as the decoder does.
	render::make_surface_scaler = []() -> std::unique_ptr<render::surface_scaler>
	{
		return std::make_unique<av_scaler>();
	};

	return true;
}

int run_console_tests(std::string_view test_filter);
int validate_po_files();

bool app_frame::parse_command_line(const std::string_view command_line_text)
{
	command_line.parse(command_line_text);

	// A collection of its own never shares the user's settings: saving them would replace the user's own
	// collection folders with it. Given no -profile, it keeps them beside itself.
	if (!command_line.collection_folder.empty() && command_line.profile_folder.empty())
	{
		auto folder = command_line.collection_folder;
		while (!folder.empty() && core::is_path_sep(folder.back())) folder.pop_back();
		command_line.profile_folder = folder + "-profile";
	}

	if (!command_line.profile_folder.empty()) use_profile_folder(core::folder_path(command_line.profile_folder));

	// A tool called once, or an agent session: each answers from the same tools (app_mcp.h).
	if (!command_line.tool.empty() || command_line.agent)
	{
		const auto console = !command_line.tool.empty();
		auto options = parse_agent_options(command_line.agent_ui, command_line.agent_allow);

		// What a single call may do is what it was asked to do: the tool it names.
		if (console) options.allow = {.write = true, .capture = true, .network = true, .dev = true, .launch = true};

		df::network_allowed = options.allow.network;
		_mcp = std::make_shared<mcp_host>(*this, std::move(options), console);
	}

	return command_line.console_test || command_line.validate_po || !command_line.tool.empty();
}

int app_frame::run_console_mode()
{
	// /test runs the suite; /validate-po checks the translations against the strings app_text registers;
	// -tool calls one tool and prints what it answers.
	if (command_line.console_test) return run_console_tests(command_line.test_filter);
	if (command_line.validate_po) return validate_po_files();
	return run_tool_once(_mcp, command_line.tool, command_line.tool_arguments);
}

std::shared_ptr<core::mcp::server> app_frame::agent_server()
{
	return _mcp && !_mcp->is_console() ? _mcp->server() : nullptr;
}

bool app_frame::wants_gpu() const
{
	// A window nobody sees is drawn by the software renderer: its pictures are the same, it never fades, and it
	// leaves the graphics crash guards the user's next start reads alone.
	if (_mcp && _mcp->options().presence != agent_presence::visible) return false;
	return setting.use_gpu && !command_line.no_gpu;
}

void app_frame::update_font_size() const
{
	// The Large text option scales the whole interface with it.
	constexpr int large_font_size = 21;

	render::set_base_font_size(setting.large_font ? large_font_size : render::default_font_size);
}

bool app_frame::init(const std::string_view command_line_text)
{
	if (_forwarded_launch) return false;
	log_func lf(__FUNCTION__);
	core::log(__FUNCTION__, std::format("is_app_installed {}", is_app_installed()));

	// These files are skipped by the indexer for the rest of the session, so they are stated once
	// rather than left as an unexplained absence of metadata and thumbnails.
	if (const auto skipped = crash_files().skipped_file_count(); skipped > 0)
	{
		core::log(__FUNCTION__, std::format("skipping {} file(s) that crashed a previous run of this release{}",
		                                    skipped, crash_files().is_full() ? " - the crash list is full" : ""));
	}

	command_line.parse(command_line_text);
	load_file_types();

	// An agent session makes its window when a tool first needs one, unless it was started with it.
	if (_mcp && _mcp->options().presence == agent_presence::none)
	{
		_mcp->started();
		return true;
	}

	std::string why;
	const auto started = start_window(why);
	if (_mcp) _mcp->started();
	return started;
}

bool app_frame::start_window(std::string& why)
{
	if (_window_started) return true;

	// Two processes writing one index would each undo the other's work, so an agent session refuses a profile another
	// Diffractor has open. An ordinary run holds its profile so that a session can tell, and runs whatever it finds.
	{
		const auto profile = crypto::to_sha256(core::to_lower(app_cache_folder().text().sv()));
		auto locked = platform::try_lock_instance(std::format("Diffractor.Profile.{}", profile));

		if (_mcp && !locked.lock)
		{
			why = locked.already_running
			          ? "Diffractor is already open with this profile. Close it, or start the session with "
			            "-profile:<folder> to keep one of its own."s
			          : std::format("The profile could not be claimed: {}", locked.error);
			return false;
		}

		_profile_lock = std::move(locked.lock);
	}

	_item_index.init_item_index();

	const auto& store = _settings;
	load_options(store);
	setting.instantiations++;

	// load_options re-runs setting.read(), which puts the persisted values back, so the safe
	// start decided in load_settings has to be re-applied over them. reset_presentation is
	// idempotent, so this is the same answer, not a second decision.
	if (_safe_start) setting.reset_presentation();
	// And so has a run's own -profile and -collection, and an agent session's, over what an earlier run saved.
	apply_run_settings();

	if (setting.language != "en")
	{
		const auto lang_folder = known_path(platform::known_folder::running_app_folder).combine("languages");
		const auto lang_path = lang_folder.combine_file_ext(setting.language, ".po");

		const auto po_entries = load_po(lang_path);
		tt.load_lang(lang_path.name(), po_entries);
		register_file_group_spellings();
	}

	update_font_size();
	// Issue #227: only seed the default favorite tags on first run. After the user has
	// configured favorites once (flag set below, persisted on save) an empty list is
	// respected instead of resurrecting the removed defaults on every launch.
	if (should_seed_default_favorite_tags(setting.favorite_tags_initialized, core::is_empty(setting.favorite_tags),
	                                      store->root_created()))
	{
		setting.favorite_tags = tt.default_favorite_tags;
	}
	setting.favorite_tags_initialized = true;

	initialise_commands();

	// The window draws nothing of its own: its regions do, each with a surface of its own, and the line of
	// its background they leave round the edge is its border.
	ui::frame_style window_style;
	window_style.container = true;
	window_style.colors = {ui::style::colors.view_background, ui::style::colors.view_text,
	                       ui::style::colors.view_selected_background};
	// An agent session's window paints where nobody sees it, unless the session was started to show it.
	window_style.offscreen = _mcp && _mcp->options().presence != agent_presence::visible;
	_app_frame = ui::create_app_frame(shared_from_this(), store, shared_from_this(), window_style);

	if (!_app_frame)
	{
		why = std::string(tt.error_create_window_failed.sv());
		if (!_mcp) fail(tt.error_create_window_failed, {});
		return false;
	}

	_title_bar->init(_app_frame);
	_view_frame->init(_app_frame);
	_status_bar->init(_app_frame);
	_selector_frame->init(_view_frame->frame(), true);
	_selector_frame->_press_takes_keyboard = false;
	_title_region->child = _title_bar->frame();
	_body_region->child = _view_frame->frame();
	_status_region->child = _status_bar->frame();
	_view_frame->_strip->child = _selector_frame->frame();
	_view_frame->focus_listener = [this](const bool has_focus, const ui::control_ptr& child)
	{
		focus_changed(has_focus, child);
	};
	create_toolbars();

	init_search();

	_bubble = _app_frame->create_bubble();
	_state.view_mode(view_type::items);
	for (const auto* const group : _starting_media_filter)
	{
		_state.filter().add_group(group);
	}
	_threads.start_if_running([&q = work_task_queue] { start_worker(q, "work"); });

	open_default_folder();
	invalidate_view(view_invalid::address | view_invalid::sidebar_drives);

	work_task_queue.enqueue([this, app = shared_from_this()]
	{
		start_workers();
		check_for_updates_and_location(app, _state);

		// Scanning the tool folders is file i/o; the result is published to the file type and
		// group tables on the UI thread, which owns them.
		auto tools = scan_tools();
		queue_ui([t = std::move(tools)]() mutable { apply_tools(std::move(t)); });

		// Render is offered only where the machine can encode, and finding out is platform work the
		// UI thread does not do. Asked once here; the answer is the platform's to cache.
		const auto can_write_movies = platform::can_write_movies();
		queue_ui([this, can_write_movies]
		{
			_can_write_movies = can_write_movies;
			invalidate_view(view_invalid::command_state);
		});
	});

	if (!setting.sound_device.empty())
	{
		_state.change_audio_device(setting.sound_device);
	}

	_state.group_order(_starting_group_order, _starting_sort_order);
	focus_view();

	_window_started = true;
	_instance_ready = true;
	if (!_pending_instance_file.is_empty()) open_pending_instance_file();
	return true;
}

void app_frame::on_window_destroy()
{
	log_func lf(__FUNCTION__);
	_instance_ready = false;
	_instance_receiver.reset();

	// Before the window goes: the recording reads it until it stops, and its file is finished on the way out.
	_tutorial.reset();
	finish_recording();

	if (_app_logo) _app_logo->free_graphics_resources();
	_bubble.reset();
	_search_predictions_frame.reset();
	_view_frame.reset();
	_title_bar.reset();
	_status_bar.reset();
	_item_index.reset();
	_state.reset();
	save_options();

	// Clean shutdown: clear the graphics crash guards so this run is not mistaken for a
	// crash on the next launch. (_state.reset above closes any decoders, releasing the
	// hardware-decode guard; clear both here to cover the GPU-render guard as well.)
	if (!platform::crash_guard_failed(platform::crash_guard::gpu_render))
	{
		platform::set_crash_guard(platform::crash_guard::gpu_render, false);
	}
	platform::set_crash_guard(platform::crash_guard::hw_video_decode, false);
}

void app_frame::command_hover(const ui::command_ptr& c, const recti window_bounds)
{
	if (_hover_command != c)
	{
		_hover_command = c;
		_hover_command_bounds = window_bounds;
		invalidate_view(view_invalid::tooltip);
	}
}

// A view names its menu's commands, as it does its bars', and the frame builds them: the row of buttons
// first, set apart from the list, in which commands::none is a separator.
std::vector<ui::command_ptr> app_frame::context_menu(const view_menu& menu) const
{
	std::vector<ui::command_ptr> result;
	result.reserve(menu.list.size() + menu.own.size() + 3);

	if (!menu.row.empty())
	{
		const auto buttons = std::make_shared<ui::command>();
		for (const auto id : menu.row) buttons->buttons.emplace_back(find_command(id));
		result.emplace_back(buttons);
		result.emplace_back(nullptr);
	}

	if (!menu.own.empty())
	{
		result.insert(result.end(), menu.own.begin(), menu.own.end());
		if (!menu.list.empty()) result.emplace_back(nullptr);
	}

	for (const auto id : menu.list)
	{
		result.emplace_back(id == commands::none ? nullptr : find_command(id));
	}

	return result;
}

// The menu of the view under a screen point: the frame's own outside the view or where the view names none,
// and none over the panel beside it or the strip below it, whose controls have their own. The view's surface
// is a child window on Windows, and a child window's bounds are its parent's coordinates rather than the
// screen's, so the point reaches the view through the surface's own place on the screen.
view_menu app_frame::menu_at(const pointi screen)
{
	const auto loc = screen - _view_frame->frame()->client_to_screen({});
	if (!recti(_view_frame->_extent).contains(loc)) return s_frame_menu;
	if (!_view_frame->in_view(loc)) return {};
	auto menu = _view->context_menu(loc);
	return menu.empty() ? s_frame_menu : menu;
}

std::vector<ui::command_ptr> app_frame::menu(const pointi loc)
{
	// The menu the platform asks for after Apps or Shift+F10 is already open beside what has the keyboard
	// (open_keyboard_menu), so the request it makes at no point is not answered with a second.
	if (loc == pointi{-1, -1}) return {};
	// The view may select what is under the pointer as it names its menu, so the commands' states follow that.
	const auto at = menu_at(loc);
	update_button_state(false);
	return context_menu(at);
}
void app_frame::exit()
{
	log_func lf(__FUNCTION__);
	_instance_ready = false;
	_instance_receiver.reset();
	_threads.clear();
}

void app_frame::system_event(const platform::os_event_type ost)
{
	core::assert_true(platform::is_ui_thread());

	if (ost == platform::os_event_type::system_device_change)
	{
		_has_burner.reset();
		invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::sidebar_drives |
		                view_invalid::index);
	}
	else if (ost == platform::os_event_type::options_changed)
	{
		invalidate_view(view_invalid::options);
	}
	else if (ost == platform::os_event_type::dpi_changed)
	{
		invalidate_view(view_invalid::visual_options);
	}
	else if (ost == platform::os_event_type::screen_locked)
	{
		_state.stop();
	}
	else if (ost == platform::os_event_type::system_suspending)
	{
		// The machine may never come back (hibernate failure, flat battery), so treat suspend as
		// the last chance to persist. Stopping also releases the sleep block we are about to lose.
		_state.stop();
		save_options();
	}
	else if (ost == platform::os_event_type::system_resumed)
	{
		invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::sidebar |
		                view_invalid::index | view_invalid::screen_saver);
	}
	else if (ost == platform::os_event_type::session_ending)
	{
		// No prompting and no teardown: this runs before any shutdown question is asked, and again
		// once the session is really ending, and the process is terminated regardless of what it
		// does next.
		save_options();
	}
	else if (ost == platform::os_event_type::graphics_device_lost)
	{
		// Every texture and surface belongs to the device that has just gone away. Release
		// them here - the platform layer then rebuilds each window's draw context on the CPU
		// software backend, and the resources are staged again on the next paint.
		free_graphics_resources(false, false);
		invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw);
	}
}

void app_frame::final_exit()
{
	spell().flush_pending_custom_words();
	spell().configure_async(nullptr);

	// One aggregate block for the whole session - per-event tracing would swamp the log.
	df::log_perf_summary();
	log_file_op_summary();

	core::log("main", "exit");
	platform::close_log();
}

static void register_diffractor_chrome_icons()
{
	ui::set_chrome_icons({
		.check = icon_index::check,
		.checkbox_off = icon_index::checkbox_off,
		.checkbox_on = icon_index::checkbox_on,
		.radio_off = icon_index::radio_off,
		.radio_on = icon_index::radio_on,
		.close = icon_index::close,
		.maximize = icon_index::maximize,
		.minimize = icon_index::minimize,
		.restore = icon_index::restore,
	});
}

#ifdef _WIN32
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	// app_frame builds its views here, before platform::run, and they belong to this thread.
	platform::set_ui_thread();
	core::set_app_name("Diffractor");
	register_diffractor_chrome_icons();
	auto app = std::make_shared<app_frame>();
	return platform::run(app, __argc, __argv);
}
#else
int main(int argc, char** argv)
{
	// app_frame builds its views here, before platform::run, and they belong to this thread.
	platform::set_ui_thread();
	core::set_app_name("Diffractor");
	register_diffractor_chrome_icons();
	auto app = std::make_shared<app_frame>();
	return platform::run(app, argc, argv);
}
#endif

void app_frame::crash(const core::file_path dump_file_path)
{
	// Claimed atomically: a test-then-increment let a second thread faulting inside this handler
	// start its own report and post a duplicate.
	auto unclaimed = 0;

	if (df::handling_crash.compare_exchange_strong(unclaimed, 1))
	{
		const core::scope_exit release_claim([] { --df::handling_crash; });

		if (_app_frame)
		{
			_app_frame->show(false);
		}

		flush_open_files_to_crash_files_list();

#ifndef WINSTORE
		// Asked here too: an agent session's Options page can tick crash reports back on.
		if (setting.send_crash_dumps && df::network_allowed)
		{
			core::log(__FUNCTION__, "*** CRASH ***");

			if (const auto last_loaded = df::last_loaded_path.load(std::memory_order_relaxed); !last_loaded.is_empty())
			{
				core::log(__FUNCTION__, std::format("Last file type opened: {}", last_loaded.extension()));
			}

			const auto* const render_func = render::rendering_func.load(std::memory_order_relaxed);

			if (!core::is_empty(render_func))
			{
				core::log(__FUNCTION__, std::format("Rendering function: {}", core::utf8_cast(render_func)));
			}

			if (dump_file_path.is_empty())
			{
				// Reported anyway: the logs carry the timeline, the configuration and the last file opened,
				// which is often enough to attribute the fault without a dump.
				core::log(__FUNCTION__, "no minidump was written - reporting logs only");
			}

			const auto log_file_path = platform::close_log();
			const auto previous_log_path = platform::previous_log_path;
			const auto crash_zip_path = platform::temp_file();

			const auto now = core::utc_now();
			const auto date = now.date();

			const auto name = std::format("Diffractor-{}-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp", s_app_version,
			                              g_app_build, date.year, date.month, date.day, date.hour, date.minute,
			                              date.second);

			df::zip_file zip;
			auto has_zip = false;

			if (zip.create(crash_zip_path))
			{
				if (!dump_file_path.is_empty()) zip.add(dump_file_path, name);
				if (platform::exists(log_file_path)) zip.add(log_file_path);
				if (platform::exists(previous_log_path)) zip.add(previous_log_path);
				// Only an archive that closed is one to send. Finishing the central directory can fail
				// - a full disk, a short write - and the partial file would reach the server as a
				// corrupt upload while the dump it was meant to carry is deleted below.
				has_zip = zip.close();
			}

			std::ostringstream message;

			for (const auto& i : calc_app_info(_state.item_index, true))
			{
				message << i.first << " " << i.second << '\n';
			}

			platform::web_request req;
			req.verb = platform::web_request_verb::POST;
			req.path = "/crash";
			req.form_data.emplace_back("message", message.str());
			req.form_data.emplace_back("version", platform::OS());
			req.form_data.emplace_back("diffractor", s_app_version);
			req.form_data.emplace_back("build", g_app_build);
			req.form_data.emplace_back("subject", "Diffractor CRASH report");
			req.form_data.emplace_back("submit", "Send Report");

			// temp_file() only reserves a name, so a failed zip would post a path that is not there.
			// The report itself still carries the app info, which is the part worth keeping.
			if (has_zip)
			{
				req.file_form_data_name = "ff";
				req.file_name = "crash.zip";
				req.file_path = crash_zip_path;
			}

			const auto con = platform::connect_to_host("diffractor.com");
			send_request(con, req);

			if (platform::exists(crash_zip_path)) platform::delete_file(crash_zip_path);
		}
#endif

		// Whether or not it was sent, and whether or not reporting is enabled: a repeating crash would
		// otherwise leave a fresh multi-megabyte dump in the temp folder on every launch.
		if (!dump_file_path.is_empty()) platform::delete_file(dump_file_path);
	}
}

// Inspect the graphics crash-guard flags left by the previous run and fall back if it
// crashed while a graphics subsystem was active. A HW-decode crash disables only hardware
// video decoding (narrowest attribution); a crash with the GPU active but no decode in
// progress disables GPU rendering. GPU device loss is handled as a one-session software
// recovery: the user's preference is preserved and retried after that session exits cleanly.
// Escalation is graceful: a decode crash drops decode first; GPU is dropped only if a later
// run still crashes with decode already off (so only gpu_render remains set).
static void apply_gpu_crash_guard()
{
	const auto decode_crashed = platform::read_crash_guard(platform::crash_guard::hw_video_decode);
	const auto gpu_crashed = platform::read_crash_guard(platform::crash_guard::gpu_render);

	if (!decode_crashed && !gpu_crashed)
	{
		return;
	}

	// Attribute to the narrowest subsystem: a hardware-decode crash disables only HW video
	// decode (never the GPU), even if HW decode happens to already be off; only a crash with
	// no decode in progress disables GPU rendering.
	if (decode_crashed)
	{
		if (setting.use_d3d11va)
		{
			setting.use_d3d11va = false;
			core::log(
				__FUNCTION__,
				"Previous run crashed during hardware video decode - disabling HW video decode (GPU rendering kept)");
		}
	}
	else if (gpu_crashed)
	{
		if (setting.use_gpu)
		{
			platform::suppress_crash_guard(platform::crash_guard::gpu_render, true);
			core::log(__FUNCTION__,
			          "Previous run lost the GPU device - using software rendering for this recovery session");
		}
		return;
	}

	// Hardware-decode failures retain the existing persistent fallback policy.
	setting.write();
	platform::set_crash_guard(platform::crash_guard::hw_video_decode, false);
	platform::set_crash_guard(platform::crash_guard::gpu_render, false);
}

bool app_frame::load_settings(const platform::setting_file_ptr& store)
{
	// Called early in startup (before the graphics factories and UI are created) so the
	// persisted preferences - notably use_gpu / use_d3d11va - are available in time to
	// initialise rendering. The store is the single shared instance provided by the platform;
	// it is retained for later load_options / save_options.
	(void)store;

	// The store of a -profile keeps a run's settings apart from the user's, and an agent session keeps what it changes
	// to itself.
	if (_mcp && !_mcp->is_console()) keep_settings_to_session();
	_settings = app_settings_store();

	setting.read();
	apply_run_settings();

	// An agent session is its own: it neither hands a file to a running window nor takes one from Explorer, and is no
	// start of the user's, so it records no startup history and leaves the graphics crash guards alone.
	if (_mcp)
	{
		connect_yuv_latch();
		return true;
	}

	// A forwarding launch never writes startup/crash history or creates the index and window.
	_forwarded_launch = reuse_running_instance();
	if (_forwarded_launch) return true;

	// A crash before the first frame repeats on every relaunch, and nothing the user did caused
	// it, so nothing they can stop doing avoids it. The count of starts that never settled is
	// raised here - the earliest point that has a store - and cleared once this one settles.
	_owns_startup_scope = platform::claim_startup_scope();

	uint32_t unsettled_starts = 0;
	_settings->read({}, s_unsettled_starts, unsettled_starts);
	const auto history = decide_startup(_owns_startup_scope, unsettled_starts);
	_safe_start = history.safe_start;
	if (history.record) _settings->write({}, s_unsettled_starts, history.next_unsettled);

	// Must happen here rather than in init(): the caller creates the graphics factories from
	// setting.use_gpu before init() runs, so a safe start decided later would leave the very
	// device creation it exists to avoid already done.
	if (_safe_start)
	{
		core::log(__FUNCTION__,
		          std::format("{} starts did not settle - reverting to safe presentation", unsettled_starts));
		setting.reset_presentation();
	}

	apply_gpu_crash_guard();
	connect_yuv_latch();

	return true;
}

bool app_frame::reuse_running_instance(const bool forward_file)
{
	if (!platform::can_receive_instance_requests() || !setting.reuse_existing_window) return false;
	const auto profile = crypto::to_sha256(core::to_lower(app_cache_folder().text().sv()));
	const auto name = std::format("Diffractor.{}.{}",
#ifdef _DEBUG
	                              "Debug",
#else
	                              "Release",
#endif
	                              profile);
	const auto forward = forward_file && command_line.can_forward_file();
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		auto received = platform::receive_instance_requests(name, [this](std::vector<core::file_path> files)
		{ return accept_instance_files(std::move(files)); });
		_instance_claim_retry = received.already_running;
		if (received.receiver)
		{
			_instance_receiver = std::move(received.receiver);
			return false;
		}
		if (!received.already_running)
		{
			core::log(__FUNCTION__, received.error);
			return false;
		}
		if (!forward) return false;
		const std::array files{command_line.selection};
		const auto sent = platform::send_instance_request(name, files);
		if (sent.status == platform::instance_request_status::accepted)
		{
			core::log(__FUNCTION__, "File accepted by the running instance");
			return true;
		}
		core::log(__FUNCTION__, std::format("Opening a separate window: instance request {} {}",
		                                    static_cast<int>(sent.status), sent.error));
		if (sent.status != platform::instance_request_status::not_running) break;
	}
	return false;
}

bool app_frame::accept_instance_files(std::vector<core::file_path> files)
{
	if (!setting.reuse_existing_window || files.empty() || platform::is_closing ||
	    (_instance_ready && (!_app_frame->is_enabled() || ui::command_active != 0 || !view_gives_way())))
		return false;
	_pending_instance_file = files.back();
	if (_instance_ready) open_pending_instance_file();
	return true;
}

bool app_frame::view_gives_way() const
{
	if (_view->progress().active) return false;
	if (_view == _view_edit) return !_view_edit->has_changes();
	if (_view == _view_options) return !_view_options->has_changes();
	return true;
}

void app_frame::open_pending_instance_file()
{
	if (_instance_open_queued) return;
	_instance_open_queued = true;
	queue_ui([this]
	{
		_instance_open_queued = false;
		if (!_instance_ready || platform::is_closing || _pending_instance_file.is_empty()) return;
		const auto file = std::exchange(_pending_instance_file, {});
		_app_frame->sys_command(platform::sys_command_type::ACTIVATE);
		if (_state.open_external_file(_view_frame, file)) focus_view();
	});
}

std::string app_frame::restart_command_line()
{
	return command_line.format_restart_cmd_line();
}

void app_frame::save_recovery_state()
{
	// First, so the files open when Windows terminated the process are skipped on the next start.
	log_open_files_to_crash_files_list();
	flush_open_files_to_crash_files_list();
	save_options();
}

void app_frame::invalidate_view(const view_invalid invalid)
{
	auto expected = _invalids.load();
	auto updated = expected | invalid;

	while (!_invalids.compare_exchange_weak(expected, updated))
	{
		updated = expected | invalid;
	}
}

void app_frame::free_graphics_resources(const bool items_only, const bool offscreen_only)
{
	if (!items_only)
	{
		const auto d = _state._display;

		if (d)
		{
			if (d->_selected_texture1) d->_selected_texture1->free_graphics_resources();
			if (d->_selected_texture2) d->_selected_texture2->free_graphics_resources();

			// The audio visualizer vertices belong to the device being torn down. They are only
			// ever rebuilt when null, so leaving the stale buffer here leaves the visualizer blank
			// for the rest of the session after a device loss or a hardware/software downgrade.
			d->_audio_verts.reset();
			// So do the SID monitor's, which it makes again when it next draws.
			d->_sid_motion.reset();

			if (d->_session)
			{
				_player->close(d->_session, {});
			}
		}

		// Nothing above kept these reachable, so they would otherwise hold full-size images and their
		// textures past the device, theme or DPI change that just dropped everything on screen.
		_state.release_retained_textures();

		const view_element_event e{view_element_event_type::free_graphics_resources, nullptr};
		element_broadcast(e);
	}

	const auto logical_bounds = _view_items->calc_logical_items_bounds();
	const auto expanded_logical_bounds = logical_bounds.inflate(0, logical_bounds.height());

	// The selection panel draws cover art from the displayed item's staged surface, and nothing
	// restages an item the listing has scrolled past, so the two items a panel can be showing keep
	// theirs. A full release still takes them: the texture belongs to the device that is going, and
	// the restage below puts the surfaces back.
	const auto displayed = _state._display;
	const auto displayed1 = displayed ? displayed->_item1 : nullptr;
	const auto displayed2 = displayed ? displayed->_item2 : nullptr;

	for (const auto& i : _state.search_items().items())
	{
		if (offscreen_only && (i == displayed1 || i == displayed2)) continue;

		if (!offscreen_only || !i->bounds.intersects(expanded_logical_bounds))
		{
			i->clear_cached_surface();
		}
	}

	if (!offscreen_only && _view == _view_items)
	{
		_view_items->stage_visible_thumbnails();
	}

	if (_app_logo) _app_logo->free_graphics_resources();
}
