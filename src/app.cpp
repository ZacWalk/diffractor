// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The application frame. Startup and shutdown, settings load and save, the frame layout,
// draining view invalidation in dependency order, keyboard routing, switching views, update checks
// and crash reporting. Command behavior, toolbars, workers and the address box live in
// app_commands.cpp, app_toolbar.cpp, app_workers.cpp and app_search.cpp.

#include "pch.h"

#include "app_text.h"
#include "util_zip.h"

#include "model_location.h"
#include "model_db.h"
#include "model_index.h"
#include "model.h"

#include "ui_dialog.h"
#include "util_crash_files_db.h"
#include "libdesktop/util_log.h"
#include "metadata_xmp.h"
#include "files.h"
#include "av_format.h"
#include "view_items.h"
#include "view_edit.h"
#include "view_movie.h"
#include "view_media.h"
#include "view_selector.h"
#include "view_import.h"
#include "view_locate.h"
#include "view_rename.h"
#include "view_batch.h"
#include "view_sync.h"
#include "view_tags.h"

#include "app_sidebar.h"
#include "app_commands.h"
#include "app_command_line.h"
#include "app.h"

#include <utility>

#include "app_command_status.h"
#include "util_spell.h"


const std::string_view s_app_name = "Diffractor";
const std::string_view s_app_version = "127.3";
const std::string_view g_app_build = "1313";

const ui::chrome_icon_set ui::chrome_icons = {
	.check = icon_index::check,
	.checkbox_off = icon_index::checkbox_off,
	.checkbox_on = icon_index::checkbox_on,
	.radio_off = icon_index::radio_off,
	.radio_on = icon_index::radio_on,
	.close = icon_index::close,
	.maximize = icon_index::maximize,
	.minimize = icon_index::minimize,
	.restore = icon_index::restore,
};
static constexpr auto s_search = "search";

extern void start_worker(platform::task_queue& q, std::string_view name);

std::string df::format_version(const bool short_text)
{
	if (short_text)
	{
		return std::format("{}.{}", s_app_version, g_app_build);
	}

	return std::format("{}: {}.{}  |  {}", tt.version, s_app_version, g_app_build, str::utf8_cast(__DATE__));
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
		df::assert_true(ui::is_ui_thread());

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
	const auto splits = str::split(text, true);

	if (splits.size() == 2)
	{
		const auto latitude = splits[0];
		const auto longitude = splits[1];

		if (!longitude.empty() && !latitude.empty())
		{
			return {str::to_double(latitude), str::to_double(longitude)};
		}
	}

	return def_coords;
}

// Runs on a shared worker thread (which already owns its own name and COM
// initialisation) and only queues the actual network request, so it must not
// rename the thread or re-initialise it.
static void check_for_updates_and_location(const app_frame_ptr& app, view_state& s)
{
	log_func lf(__FUNCTION__);

	spell().configure_async(&s._async);

	if (platform::is_online())
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
					{"ft"s, str::to_hex(reported_features)},
					{"i"s, str::to_hex(setting.instantiations)},
					{"os"s, platform::OS()},
					// The packed platform identity. It ships alongside `os` for one release rather
					// than replacing it on the same commit, so the ingest can change without a gap
					// in the daily series. It is its own parameter and never merged into `ft`: a
					// feature bit means "at least once since the last report" and is cleared once
					// sent, a system value means "right now" and is recomputed every time, and
					// OR-accumulating an architecture would make anyone who once ran the 32-bit
					// build look like a permanent 32-bit user.
					{"s"s, str::to_hex(df::pack_environment(platform::environment()))},
				};

				const auto con = platform::connect_to_host("diffractor.com");
				const auto response = send_request(con, req);

				if (response.status_code == 200)
				{
					df::util::json::json_doc json;
					json.Parse(response.body);

					app_updates_and_location_params params;
					params.version = df::util::json::safe_string(json, "current_version");
					params.test_version = df::util::json::safe_string(json, "test_version");
					params.gps = parse_coordinates(df::util::json::safe_string(json, "latlon"), params.gps);

					clear_reported_feature_use(reported_features);

					s.queue_ui([params, app, &s]
					{
						params.apply(app, s);
					});
				}
			});
		}
#endif
		spell().lazy_download(s._async);
	}
}

std::vector<std::pair<std::string_view, std::string>> calc_app_info(const index_state& index,
                                                                    const bool include_state)
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

	const auto seconds_running = platform::now().to_seconds() - df::start_time.to_seconds();

	result.reserve(include_state ? 25_z : 19_z);
	result.emplace_back("Version:", df::format_version(true));
	result.emplace_back("Windows:", std::format("{} {} {}", platform::OS(), arch, config));
	result.emplace_back("Id:", str::to_hex(df::crc32c(platform::user_name()), false));
	result.emplace_back("Running:", std::format("{} seconds", seconds_running));
	platform::memory_usage_t mem;

	if (platform::memory_usage(mem))
	{
		// Private working set first: it is what Task Manager's Memory column reports, so a user
		// comparing the two sees the same number. Shared pages are mapped data files, which cost
		// this process nothing private.
		result.emplace_back("Memory:",
		                    std::format("{} (commit {}, shared {})", df::file_size(mem.private_working_set),
		                                df::file_size(mem.commit), df::file_size(mem.shared_working_set)));
		result.emplace_back("Peak memory:", df::file_size(mem.peak_working_set).str());
	}

	result.emplace_back("Static Memory:",
	                    df::file_size(platform::static_memory_usage.load(std::memory_order_relaxed)).str());

	result.emplace_back("GPU:", df::gpu_desc);
	result.emplace_back("GPU Id:", df::gpu_id);
	result.emplace_back("D3D:", df::d3d_info);

	result.emplace_back("Indexed items:", str::to_string(index.stats.index_item_count));
	result.emplace_back("Indexed folders:", str::to_string(index.stats.index_folder_count));
	result.emplace_back("Duplicates:",
	                    std::format("g={} mcomp={}", index.stats.indexed_dup_folder_count,
	                                index.stats.indexed_max_compare_count));
	result.emplace_back("Hashes:",
	                    std::format("crc={} phash={} declined={} uninvited={}", index.stats.indexed_crc_count,
	                                index.stats.indexed_phash_count, index.stats.indexed_phash_declined_count,
	                                index.stats.indexed_phash_uninvited_count));
	result.emplace_back("DB size:", index.stats.database_size.str());
	result.emplace_back("Saved:", std::format("{} items | {} thumbs", index.stats.items_saved,
	                                          index.stats.thumbs_saved));


	result.emplace_back("Index load:", std::format("{} ms", index.stats.index_load_ms));
	result.emplace_back("Predictions:", std::format("{} ms", index.stats.predictions_ms));
	result.emplace_back("Count Matches:", std::format("{} ms", index.stats.count_matches_ms));

	if (include_state)
	{
		result.emplace_back("Jobs running: ", str::to_string(df::jobs_running));
		result.emplace_back("Is indexing: ", str::to_string(index.indexing));
		result.emplace_back("Is searching: ", str::to_string(index.searching));
		result.emplace_back("Is command active: ", str::to_string(df::command_active));
		result.emplace_back("Is closing: ", str::to_string(df::is_closing));
		result.emplace_back("Rendering function: ", str::utf8_cast(df::rendering_func.load(std::memory_order_relaxed)));
	}

	return result;
}

void app_logo_element::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	hover.elements->add(std::make_shared<text_element>(_text, ui::style::font_face::title,
	                                                   ui::style::text_style::single_line,
	                                                   flex_item::line_break));
	hover.elements->add(std::make_shared<text_element>(df::format_version(false), ui::style::font_face::dialog,
	                                                   ui::style::text_style::single_line,
	                                                   flex_item::line_break));

	if (setting.show_debug_info)
	{
		const auto table = std::make_shared<ui::table_element>(flex_item::center);

		for (const auto& i : calc_app_info(_state.item_index, true))
		{
			table->add(i.first, i.second);
		}

		hover.elements->add(table);
	}

	hover.elements->add(std::make_shared<action_element>(tt.help_more_info));
	hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
}

view_element_ptr create_app_logo_element(view_state& s, const ui::style::font_face font, const bool interactive,
                                         const bool show_plasma, const double logo_scale,
                                         const view_element_options& options)
{
	return std::make_shared<app_logo_element>(s, font, interactive, show_plasma, logo_scale, options);
}

void app_frame::app_fail(const std::string_view message, const std::string_view more_text)
{
	auto message_s = std::string(message);
	auto more_text_s = std::string(more_text);

	// A failure during startup has no window to parent a dialog and no message loop to run one:
	// wWinMain returns as soon as init() fails, so a queued dialog would never be shown and the
	// process would simply disappear. Say it synchronously instead. Restricted to the UI thread so
	// a worker reporting a failure after the window has gone still just queues and is dropped.
	if (!_app_frame && ui::is_ui_thread())
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
		controls.emplace_back(set_margin(std::make_shared<ui::title_control>(icon_index::error, tt.platform_text.title_error)));
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
	df::assert_true(ui::is_ui_thread());
	const auto display = _state.display_state();
	_display = display;
	view_element_ptr media_element;

	if (display)
	{
		if (display->is_one())
		{
			const auto& item = display->_item1;
			const auto* const file_type = item->file_type();

			// Arrowing through fullscreen lands on whatever is next, including a file the decoder
			// could not open. A player over nothing is what makes that look like a broken video
			// rather than a file that was never media, so it falls through to no media element and
			// the panel states what the file is.
			const auto media_unavailable = display->_av_open_failed;

			if (file_type->has_trait(file_traits::bitmap))
			{
				media_element = std::make_shared<photo_control>(_state, display, _host);
			}
			else if (file_type->has_trait(file_traits::visualize_audio) && !media_unavailable)
			{
				media_element = std::make_shared<audio_control>(_state, display, _host, true);
			}
			else if (file_type->has_trait(file_traits::av) && !media_unavailable)
			{
				media_element = std::make_shared<video_control>(_state, display, _host);
			}
			else if (file_type->has_trait(file_traits::archive))
			{
				media_element = std::make_shared<file_list_control>(display, flex_item::center);
			}
			else
			{
			}
		}
		else if (display->is_two())
		{
			media_element = std::make_shared<side_by_side_control>(_state, display, _host);
		}
		else if (display->is_multi())
		{
			media_element = std::make_shared<images_control2>(_state, display);
		}
	}

	std::swap(_media_element, media_element);
	_controls_element = _state.create_selection_controls(true);
	_description_element = _state.create_selection_description();

	if (_controls_element)
	{
		_controls_element->set_style_bit(view_element_style::dark_background, true);
	}
	if (_description_element)
	{
		_description_element->set_style_bit(view_element_style::dark_background, true);
	}
}


// Case-insensitive exact match for a switch name.
static bool is_switch(const std::string_view op, const std::string_view name)
{
	return str::icmp(op, name) == 0;
}

// Case-insensitive match for "<prefix><value>" where value is non-empty.
static bool switch_value(const std::string_view op, const std::string_view prefix, std::string_view& value)
{
	if (op.size() > prefix.size() && str::icmp(op.substr(0, prefix.size()), prefix) == 0)
	{
		value = op.substr(prefix.size());
		return true;
	}

	return false;
}

void command_line_t::parse(const std::string_view command_line_text)
{
	// str::split with detect_quotes strips the surrounding quotes, so a quoted
	// path containing spaces arrives here as a single part.
	for (const auto part : str::split(str::trim(command_line_text), true, str::is_white_space))
	{
		if (part.empty())
		{
			continue;
		}

		// A leading '/' introduces a switch only where it cannot begin an absolute path. Everywhere
		// else '/home/zac' would be read as a switch and never reach the path branch below.
		const auto starts_switch = part[0] == '-' || (df::windows_path_semantics && part[0] == '/');

		if (starts_switch && part.size() > 1)
		{
			const auto op = part.substr(part[1] == '-' ? 2 : 1);
			std::string_view value;

			if (is_switch(op, "no-gpu")) no_gpu = true;
			else if (is_switch(op, "no-indexing")) no_indexing = true;
			else if (is_switch(op, "run-tests") || is_switch(op, "test")) console_test = true;
			else if (switch_value(op, "test:", value))
			{
				console_test = true;
				test_filter = value;
			}
			else if (switch_value(op, "test-temp:", value)) test_temp_folder = value;
			else if (is_switch(op, "gen-docs")) gen_docs = true;
			else if (switch_value(op, "gen-docs:", value))
			{
				gen_docs = true;
				docs_path = value;
			}
			else if (is_switch(op, "validate-po")) validate_po = true;
			else if (switch_value(op, "dup-report:", value))
			{
				dup_report = true;
				dup_report_folder = value;
			}
			else if (switch_value(op, "dup-report-out:", value)) dup_report_output = value;
			else if (switch_value(op, "movie-check:", value))
			{
				movie_batch.projects = value;
				movie_batch.mode = movie_batch_mode::check;
			}
			else if (switch_value(op, "movie-import:", value))
			{
				movie_batch.projects = value;
				movie_batch.mode = movie_batch_mode::import;
			}
			else if (switch_value(op, "movie-render:", value))
			{
				movie_batch.projects = value;
				movie_batch.mode = movie_batch_mode::render;
			}
			else if (switch_value(op, "movie-media:", value)) movie_batch.media = value;
			else if (switch_value(op, "movie-out:", value)) movie_batch.output = value;
			else if (is_switch(op, "movie-overwrite")) movie_batch.overwrite = true;
			else if (is_switch(op, "movie-skip-missing")) movie_batch.skip_missing = true;
#ifdef _DEBUG
			else if (is_switch(op, "test-reset-graphics")) test_action = "reset-graphics";
			else if (is_switch(op, "test-crash")) test_action = "crash";
			else if (is_switch(op, "test-send-crash-report")) test_action = "send-crash-report";
			else if (is_switch(op, "test-new-version")) test_action = "new-version";
			else if (switch_value(op, "screenshot:", value)) screenshot_scene = value;
			else if (switch_value(op, "screenshot-output:", value)) screenshot_output = value;
#endif
		}
		else if (part.ends_with("**"))
		{
			// A trailing "**" asks for the folder with its subfolders, as it does in the address box.
			// AutoPlay opens an inserted card that way, and may name a drive without its separator.
			auto text = std::string(part);
			if (df::windows_path_semantics && text.size() == 4 && text[1] == ':') text.insert(2, 1, df::preferred_path_sep);

			if (df::is_path(text))
			{
				const df::item_selector selector(text);

				if (selector.is_recursive() && !selector.has_wildcard() && platform::exists(selector.folder()))
				{
					folder_path = selector;
					selection = {};
				}
			}
		}
		else if (df::is_path(part))
		{
			const auto folder = df::folder_path(part);

			if (platform::exists(folder))
			{
				folder_path = folder;
				selection = {};
			}
			else
			{
				const auto file = df::file_path(part);

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
	return result;
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

void view_frame::draw_view_status(ui::draw_context& dc) const
{
	const std::string_view status = _view->status();
	const auto progress = _view->progress();
	if (status.empty()) return;

	if (progress.active && progress.total == 0)
	{
		const auto alpha = command_progress_alpha(progress.active, progress.total, platform::tick_count());
		const auto text_extent = dc.measure_text(status, ui::style::font_face::dialog,
		                                         ui::style::text_style::single_line, _extent.cx);
		const auto bounds = center_rect(text_extent, recti(_extent)).inflate(dc.padding2, dc.padding1);
		dc.draw_rounded_rect(bounds, ui::color(ui::style::color::important_background, dc.colors.alpha * alpha),
		                     dc.padding1);
		dc.draw_text(status, bounds, ui::style::font_face::dialog, ui::style::text_style::single_line_center,
		             ui::color(dc.colors.foreground, dc.colors.alpha), {});
		return;
	}

	const auto height = dc.text_line_height(ui::style::font_face::dialog) + dc.padding2 * 2;
	const recti bounds(0, std::max(0, _extent.cy - height), _extent.cx, _extent.cy);
	dc.draw_rect(bounds, ui::color(ui::style::color::group_background, dc.colors.alpha * 0.94f));

	const auto text = progress.active && progress.total > 0
		                  ? std::format("{}  {} / {}", status,
		                                std::clamp(progress.position, int64_t{}, progress.total), progress.total)
		                  : std::string(status);
	dc.draw_text(text, bounds.inflate(-dc.padding2, 0), ui::style::font_face::dialog,
	             ui::style::text_style::single_line_center, ui::color(dc.colors.foreground, dc.colors.alpha), {});
}

void view_frame::draw_status(ui::draw_context& dc) const
{
	if (!_status_title.empty() || !_status_text.empty())
	{
		constexpr auto title_font = ui::style::font_face::title;
		constexpr auto text_font = ui::style::font_face::dialog;

		const auto text_color = ui::color(dc.colors.foreground, dc.colors.alpha);
		const auto bg_color = ui::color(ui::style::color::important_background, dc.colors.alpha);
		const sizei avail_extent{_extent.cx / 2, _extent.cy / 2};

		const auto title_extent = dc.measure_text(_status_title, title_font, ui::style::text_style::multiline_center,
		                                          avail_extent.cx);
		const auto text_extent = dc.measure_text(_status_text, text_font, ui::style::text_style::multiline_center,
		                                         avail_extent.cx);
		const auto text_spacing = !_status_title.empty() && !_status_text.empty() ? _status_padding : 0;

		const auto extent = sizei(std::max(title_extent.cx, text_extent.cx),
		                          title_extent.cy + text_extent.cy + text_spacing);
		const auto bounds = center_rect(extent, recti(_extent));

		auto title_bounds = bounds;
		title_bounds.bottom = title_bounds.top + title_extent.cy;
		auto text_bounds = bounds;
		text_bounds.top = text_bounds.bottom - text_extent.cy;

		dc.draw_rect(bounds.inflate(_status_padding), bg_color);
		dc.draw_text(_status_title, title_bounds, title_font, ui::style::text_style::multiline_center, text_color, {});
		dc.draw_text(_status_text, text_bounds, text_font, ui::style::text_style::multiline_center, text_color, {});
	}
}

shell_file_operation_ui::shell_file_operation_ui(view_frame& view, ui::control_frame_ptr main_frame) :
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

app_frame::app_frame(ui::plat_app_ptr pa) :
	_item_index(*this, _locations),
	_player(std::make_shared<av_player>(*this, [&index = _item_index](const df::file_path path, const double position)
	{
		index.save_media_position(path, position);
	})),
	_state(*this, *this, _item_index, _player),
	_edit_view_state(_state),
	_db(_state.item_index),
	_pa(std::move(pa))
{
	_view_frame = std::make_shared<view_frame>(_state);
	_app_logo = std::make_shared<app_logo_element>(_state);
	_selector_frame = std::make_shared<view_frame>(_state);
	_view_sync = std::make_shared<sync_view>(_state, _view_frame);
	_view_tags = std::make_shared<tags_view>(_state, _view_frame);
	_view_import = std::make_shared<import_view>(_state, _view_frame);
	_view_locate = std::make_shared<locate_view>(_state, _view_frame);
	_view_rename = std::make_shared<rename_view>(_state, _view_frame);
	_view_batch = std::make_shared<batch_tool_view>(_state, _view_frame);
	_view_items = std::make_shared<items_view>(_state, _view_frame);
	_view_selector = std::make_shared<selector_view>(_state, _selector_frame,
	                                                 [this](const df::item_element_ptr& item, const ui::key_state keys)
	                                                 {
		                                                 select_from_selector(item, keys);
	                                                 });
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

	df::log(__FUNCTION__, "destruct");
}

void app_frame::prepare_frame()
{
	df::assert_true(ui::is_ui_thread());
	df::bump(df::ui_perf.frame_prepares);
	const auto vf = _view_frame;

	// _view is only assigned by view_changed, which init() reaches after the frame exists.
	if (!df::is_closing && vf && _view)
	{
		const auto time_now = df::now();
		const auto display_frequency = platform::display_frequency();
		const auto animation_delay_ms = 1000 / display_frequency;
		constexpr auto idle_delay_ms = 1000 / ui::default_ticks_per_second;

		// Clamped so a stall, a breakpoint, or a first frame cannot snap every fade to its target.
		const auto prepare_us = df::now_us();
		const auto elapsed_s = _last_prepare_us == 0
			                       ? 1.0 / display_frequency
			                       : std::clamp((prepare_us - _last_prepare_us) / 1'000'000.0, 1.0 / 240.0,
			                                    1.0 / 15.0);
		_last_prepare_us = prepare_us;
		ui::animation_step_factor = static_cast<float>(1.0 - std::exp(-ui::alpha_fade_rate * elapsed_s));

		std::vector<void*> removals;
		auto is_animating = false;
		auto is_animating2 = false;

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

		df::record_peak(df::ui_perf.animations_peak, static_cast<uint32_t>(ui::animations.size()));
		if (is_animating2) df::bump(df::ui_perf.prepares_registered_anim);

		const auto display = _state.display_state();
		auto frame_delay = idle_delay_ms;

		if (display)
		{
			is_animating |= display->step();
			if (is_animating) df::bump(df::ui_perf.prepares_display_anim);

			// If a cloud-only item was hydrated by viewing it, rescan so its items-view
			// thumbnail and index state refresh.
			_state.rescan_hydrated_display_item();

			const auto prepare_result = display->update_for_present(time_now);

			if (prepare_result == render_valid::invalid || is_animating)
			{
				if (prepare_result == render_valid::invalid) df::bump(df::ui_perf.prepares_display_invalid);
				invalidate_status();
				vf->frame()->invalidate();
			}
			else if (prepare_result == render_valid::present)
			{
				vf->frame()->redraw();
			}

			if (is_animating2 || is_animating || display->is_playing_media())
			{
				frame_delay = std::min(frame_delay, animation_delay_ms);
			}
		}

		const auto progress = _view->progress();

		if (_app_logo && _app_frame && !_title_bounds.is_empty())
		{
			// Only the logo changed, so only the logo is invalidated - the frame repaints that
			// rectangle rather than the whole client.
			if (_app_logo->step_plasma(time_now) && _app_logo->plasma_is_active())
			{
				_app_frame->invalidate(_app_logo->invalidate_bounds());
			}
		}

		if (_app_logo && _app_logo->plasma_is_active())
		{
			frame_delay = std::min(frame_delay, 1000 / plasma::frames_per_second);
		}

		// A playing movie composes a new frame per refresh, so it needs the cadence video playback
		// gets. Left on the idle timer the preview advanced five times a second, which reads as the
		// picture stepping rather than playing.
		if (_state.view_mode() == view_type::movie && _view_movie && _view_movie->is_playing())
		{
			_view_movie->tick();
			vf->frame()->invalidate();
			frame_delay = std::min(frame_delay, animation_delay_ms);
		}

		if (command_progress_needs_animation(progress.active, progress.total))
		{
			frame_delay = std::min(frame_delay, animation_delay_ms);
		}

		if (vf->is_occluded())
		{
			frame_delay = 200;
		}

		_frame_delay = frame_delay;
		_pa->frame_delay(frame_delay);
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
					value = ui::animations_enabled ? value + dd * ui::animation_step_factor : target;
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
		display->_session->adjust_volume();
	}

	// _view is assigned by the first view_changed, which init() reaches after the window - and its
	// timer - already exist, so a tick can land in between.
	if (_app_frame && _app_frame->is_visible() && _view)
	{
		const auto time_now = df::now();

		_state.tick(_view_frame, time_now);
		_view_items->retry_visible_thumbnails(time_now);
		// The sidebar is embedded in the items view and shares its frame, so nothing else ticks it.
		if (_view_items->sidebar_visible()) _view_items->sidebar()->tick();
		_view_frame->tick();
		_view_items->update_edit_caret();
		_selector_frame->tick();

		if (_state.view_mode() == view_type::movie) _view_movie->tick();

		const auto progress = _view->progress();
		const auto animate_status = status_needs_animation(progress.active, progress.total, setting.show_debug_info);

		if (animate_status && _view_frame)
		{
			_view_frame->frame()->invalidate();
		}

		_search_color_lerp.target = _state.item_index.searching > 0 ? 255 : 0;
		if (_search_edit && _search_color_lerp.step())
		{
			_search_edit->set_background(
				_search_color_lerp.lerp(ui::style::color::edit_background,
				                        ui::style::color::important_background));
		}

		const auto logical_bounds = _view_items->calc_logical_items_bounds();
		const auto eviction_guard = _last_texture_eviction_bounds
			                            ? _last_texture_eviction_bounds->inflate(0, logical_bounds.height() / 2)
			                            : recti{};
		const auto outside_eviction_guard = !_last_texture_eviction_bounds ||
			logical_bounds.left < eviction_guard.left || logical_bounds.right > eviction_guard.right ||
			logical_bounds.top < eviction_guard.top || logical_bounds.bottom > eviction_guard.bottom;

		if (outside_eviction_guard)
		{
			_last_texture_eviction_bounds = logical_bounds;
			free_graphics_resources(true, true);
			df::trim_thumbnail_blobs(_state.search_items().items(), logical_bounds, df::max_thumbnail_bytes);
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

		if ((df::now() - _folder_change_time) > folder_change_settle_seconds)
		{
			_folder_change_time = 0.0;
			// The notification only says something touched the folder. Refreshing re-opens the whole
			// search, so let the index decide whether anything actually differs first.
			_item_index.queue_validate_changed_folders(std::move(_folders_changed));
			_folders_changed.clear();
		}
	}

	if (_invalids != view_invalid::none)
	{
		idle();
	}

#ifdef _DEBUG
	tick_screenshot();
#endif
}

#ifdef _DEBUG
void app_frame::tick_screenshot()
{
	if (command_line.screenshot_scene.empty() || !_app_frame) return;

	if (_screenshot_stage == 0)
	{
		if (!_state.has_display_items()) return;

		_pa->sys_command(ui::sys_command_type::RESTORE);
		auto window_bounds = _app_frame->window_bounds();
		window_bounds.right = window_bounds.left + 1600;
		window_bounds.bottom = window_bounds.top + 1000;
		_app_frame->window_bounds(window_bounds, true);

		const auto scene = std::string_view(command_line.screenshot_scene);
		if (str::icmp(scene, "items") == 0)
		{
			if (!_state.has_selection()) _state.select_next(_view_frame, true, false, false);
			_state.view_mode(view_type::items);
		}
		else if (str::icmp(scene, "fullscreen") == 0 ||
			str::icmp(scene, "edit") == 0 || str::icmp(scene, "edit-preview") == 0)
		{
			if (!_state.has_selection()) _state.select_next(_view_frame, true, false, false);
			if (str::icmp(scene, "fullscreen") == 0)
			{
				invoke(commands::view_fullscreen);
			}
			else
			{
				invoke(commands::tool_edit);
				if (str::icmp(scene, "edit-preview") == 0) invoke(commands::edit_item_preview);
			}
		}
		else
		{
			_state.select_all(_view_frame);
			if (str::icmp(scene, "rename") == 0) invoke(commands::tool_rename);
			else if (str::icmp(scene, "adjust-date") == 0) invoke(commands::tool_adjust_date);
			else if (str::icmp(scene, "convert") == 0) invoke(commands::tool_convert);
			else if (str::icmp(scene, "metadata") == 0) invoke(commands::tool_edit_metadata);
			else if (str::icmp(scene, "import") == 0) invoke(commands::tool_import);
			else if (str::icmp(scene, "sync") == 0) invoke(commands::tool_sync);
			else if (str::icmp(scene, "locate") == 0) invoke(commands::tool_locate);
			else
			{
				df::log(__FUNCTION__, std::format("Unknown screenshot scene: {}", scene));
				_screenshot_stage = 2;
				queue_ui([frame = _app_frame] { frame->close(); });
				return;
			}
		}

		_screenshot_stage = 1;
		_screenshot_ready_time = df::now() + 10.0;
		invalidate_view(view_invalid::app_layout | view_invalid::view_layout | view_invalid::view_redraw);
		return;
	}

	if (_screenshot_stage == 1 && df::now() >= _screenshot_ready_time)
	{
		const auto output = command_line.screenshot_output.empty()
			                    ? known_path(platform::known_folder::running_app_folder).parent().combine_file(
				                    "screenshot.webp")
			                    : df::file_path(command_line.screenshot_output);
		files ff;
		const auto captured = platform::capture_window_surface(_app_frame->handle());
		const auto surface = captured ? ff.fit_within(captured, {800, 500}) : ui::surface_ptr{};
		const auto format = extension_to_format(output.extension());
		const auto image = surface
			                   ? ff.surface_to_image(surface, metadata_parts{}, file_encode_params{}, format)
			                   : ui::const_image_ptr{};
		const auto file = image ? platform::open_file(output, platform::file_open_mode::create) : platform::file_ptr{};
		const auto saved = file && file->write(image->data().data(), image->data().size()) == image->data().size();
		if (!saved)
		{
			df::log(__FUNCTION__, std::format("Screenshot failed: {}", output));
			_screenshot_stage = 2;
		}
		else
		{
			df::log(__FUNCTION__, std::format("Screenshot saved: {}", output));
			_screenshot_stage = 2;
			queue_ui([frame = _app_frame] { frame->close(); });
		}
	}
}
#endif

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
		const str::cached default_folders[] =
		{
			known_path(platform::known_folder::pictures).text(),
			known_path(platform::known_folder::video).text(),
			known_path(platform::known_folder::music).text(),
			known_path(platform::known_folder::desktop).text(),
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
	store->write({}, s_group_order,
	             static_cast<uint32_t>(group_order_older_builds_understand(_state.group_order())));
	store->write({}, s_sort_order,
	             static_cast<uint32_t>(sort_order_older_builds_understand(_state.sort_order())));
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
	}
}

void app_frame::search_complete(const df::search_t& search, const bool path_changed)
{
	if (path_changed)
	{
		save_options(true);
		invalidate_view(view_invalid::address);

		std::vector<df::folder_path> folders;

		for (const auto& s : search.selectors())
		{
			if (s.folder().exists())
			{
				folders.emplace_back(s.folder());
			}
		}

		_pa->monitor_folders(folders);
	}

	_view->items_changed(path_changed);
	_view_selector->items_changed(path_changed);

	const auto& sidebar = _view_items->sidebar();
	sidebar->update_current_search();
	sidebar->invalidate();

	// No group_layout here. view_state::append_items has just rebuilt the groups, re-selected and
	// raised group_layout_complete in the same UI task, so asking for group_layout as well made the
	// next drain regroup, re-sort and relayout the entire listing a second time for every search.
	invalidate_view(view_invalid::item_scan |
		view_invalid::media_elements |
		view_invalid::tooltip |
		view_invalid::command_state |
		view_invalid::app_layout |
		view_invalid::presence);

	if (path_changed)
	{
		invalidate_view(view_invalid::focus_item_visible);
	}
}

// Both panes stay usable, so a splitter drag can never collapse either one out of reach.
static int clamp_pane_width(const int width, const int available, const int min_pane)
{
	const auto lo = std::min(min_pane, available / 2);
	return std::clamp(width, lo, std::max(lo, available - min_pane));
}

void app_frame::layout(ui::measure_context& mc)
{
	if (!df::is_closing && _app_frame && _view && _navigate1 && _search_edit && _navigate2 && _navigate3 && !_extent.
		is_empty())
	{
		update_button_state(true);

		const auto view_mode = _state.view_mode();
		const auto view_processing = _view && _view->progress().active;
		const auto display = _state.display_state();
		const auto show_top_bar = !_state.is_full_screen && (!display || !display->is_zoom_mode());
		const auto show_items_controls = show_top_bar && view_mode == view_type::items;
		const auto can_show_view_controls = view_mode == view_type::edit ||
			view_mode == view_type::movie ||
			view_mode == view_type::rename ||
			view_mode == view_type::batch ||
			view_mode == view_type::sync ||
			view_mode == view_type::import ||
			view_mode == view_type::locate ||
			view_mode == view_type::tags;
		const auto show_selector = selector_strip_for_view(view_mode) != selector_strip::none;

		const auto scale_factor = mc.scale_factor;
		const auto scale1 = df::round(1 * scale_factor);
		const auto scale150 = df::round(150 * scale_factor);
		const auto scale300 = df::round(300 * scale_factor);
		const auto scale400 = df::round(400 * scale_factor);
		const auto scale1000 = df::round(1000 * scale_factor);

		const auto navigate1_extent = _navigate1->measure_toolbar(_extent.cx);
		const auto navigate2_extent = _navigate2->measure_toolbar(_extent.cx);
		const auto navigate3_extent = _navigate3->measure_toolbar(_extent.cx);
		const auto media_edit_commands_extent = _media_edit_commands->measure_toolbar(_extent.cx);
		const auto tool_commands_extent = _tool_commands->measure_toolbar(_extent.cx);
		const auto import_commands_extent = _import_commands->measure_toolbar(_extent.cx);
		const auto locate_commands_extent = _locate_commands->measure_toolbar(_extent.cx);
		const auto sync_commands_extent = _sync_commands->measure_toolbar(_extent.cx);
		const auto tags_commands_extent = _tags_commands->measure_toolbar(_extent.cx);
		const auto movie_commands_extent = _movie_commands->measure_toolbar(_extent.cx);
		const auto busy_commands_extent = _busy_commands->measure_toolbar(_extent.cx);

		const auto cy_address = mc.text_line_height(ui::style::font_face::dialog) + mc.padding2 * 2;
		const auto top_content_height = std::max({
			navigate1_extent.cy, navigate2_extent.cy, navigate3_extent.cy, cy_address,
			media_edit_commands_extent.cy, tool_commands_extent.cy, import_commands_extent.cy,
			sync_commands_extent.cy, locate_commands_extent.cy, tags_commands_extent.cy,
			movie_commands_extent.cy, busy_commands_extent.cy
		}) + mc.padding2;
		const auto top_height = df::mul_div(top_content_height, 6, 5);

		const auto client_bounds = recti(_extent).inflate(_state.is_full_screen ? 0 : -scale1);
		const auto body_top = show_top_bar ? client_bounds.top + top_height : client_bounds.top;
		_top_bar_bounds = show_top_bar
			                  ? recti(client_bounds.left, client_bounds.top, client_bounds.right, body_top)
			                  : recti{};
		const auto selector_height = show_selector ? std::min(scale150, client_bounds.height() / 3) : 0;
		const auto splitter_width = can_show_view_controls ? df::mul_div(mc.scroll_width, 3, 5) : 0;
		const auto splitter_min_pane = df::round(200 * scale_factor);
		const auto splitter_available = std::max(0, client_bounds.width() - splitter_width);
		const auto stored_splitter = setting.view_splitter(view_mode);
		const auto preferred_view_controls = stored_splitter > 0
			                                     ? df::mul_div(splitter_available, stored_splitter,
			                                                   settings_t::view_splitter_max)
			                                     : std::max(client_bounds.width() / 4, scale400);
		const auto cx_view_controls = can_show_view_controls
			                              ? clamp_pane_width(preferred_view_controls, splitter_available,
			                                                 splitter_min_pane)
			                              : 0;
		const auto content_right = can_show_view_controls
			                           ? client_bounds.right - cx_view_controls - splitter_width
			                           : client_bounds.right;
		const auto toolbar_top = client_bounds.top;
		const auto center_y = [toolbar_top, top_height](const int height)
		{
			return toolbar_top + (top_height - height) / 2;
		};

		const auto x_nav3 = client_bounds.right - mc.padding1 - navigate3_extent.cx;
		const auto y_nav3 = center_y(navigate3_extent.cy);
		const recti nav3_bounds(x_nav3, y_nav3, x_nav3 + navigate3_extent.cx, y_nav3 + navigate3_extent.cy);

		const auto toolbar_widths = navigate1_extent.cx + navigate2_extent.cx + navigate3_extent.cx;
		const auto address_width = std::clamp((client_bounds.width() - toolbar_widths) / 2, scale300, scale1000);
		const auto address_center = (client_bounds.left + client_bounds.right) / 2;
		const auto address_left = address_center - address_width / 2;
		const auto address_right = address_center + address_width / 2;
		const auto nav1_x = address_left - mc.padding1 - navigate1_extent.cx;
		const auto nav1_y = center_y(navigate1_extent.cy);
		const recti nav1_bounds(nav1_x, nav1_y, nav1_x + navigate1_extent.cx, nav1_y + navigate1_extent.cy);
		const auto nav2_x = address_right + mc.padding1;
		const auto nav2_y = center_y(navigate2_extent.cy);
		const recti nav2_bounds(nav2_x, nav2_y, nav2_x + navigate2_extent.cx, nav2_y + navigate2_extent.cy);
		const auto address_y = center_y(cy_address);
		const recti search_bounds(address_left, address_y, address_right, address_y + cy_address);

		_app_logo->text(view_mode == view_type::items ? s_app_name : _view->title());
		const auto title_left = client_bounds.left + mc.padding1;
		// Outside the items view the address bar is not shown, so nav1 is no bound at all and a long
		// view title would run into the command bar, which command_bounds then clips against the
		// window edge. The commands are the view's primary affordance, so the title yields to them.
		const auto active_commands_extent =
			view_processing
				? busy_commands_extent
				: view_mode == view_type::edit
				? media_edit_commands_extent
				: view_mode == view_type::rename || view_mode == view_type::batch
				? tool_commands_extent
				: view_mode == view_type::import
				? import_commands_extent
				: view_mode == view_type::locate
				? locate_commands_extent
				: view_mode == view_type::sync
				? sync_commands_extent
				: view_mode == view_type::tags
				? tags_commands_extent
				: view_mode == view_type::movie
				? movie_commands_extent
				: sizei{};
		const auto title_limit = show_items_controls
			                         ? nav1_bounds.left
			                         : client_bounds.right - mc.padding2 - active_commands_extent.cx;
		const auto title_available_right = std::max(title_left, title_limit - mc.padding1);
		const auto title_extent = _app_logo->measure(mc, title_available_right - title_left);
		const auto title_top = center_y(title_extent.cy);
		_title_bounds = show_top_bar
			                ? recti(title_left, title_top, title_left + title_extent.cx, title_top + title_extent.cy)
			                : recti{};
		_app_logo->bounds = _title_bounds;

		const auto command_bounds = [&](const sizei extent)
		{
			const auto available_left = _title_bounds.right + mc.padding1;
			const auto available_right = client_bounds.right - mc.padding2;
			const auto available_width = std::max(0, available_right - available_left);
			const auto width = std::min(extent.cx, available_width);
			const auto x = available_right - width;
			const auto y = center_y(extent.cy);
			return recti(x, y, x + width, y + extent.cy);
		};
		const auto media_edit_bounds = command_bounds(media_edit_commands_extent);
		const auto tool_commands_bounds = command_bounds(tool_commands_extent);
		const auto import_commands_bounds = command_bounds(import_commands_extent);
		const auto locate_commands_bounds = command_bounds(locate_commands_extent);
		const auto sync_commands_bounds = command_bounds(sync_commands_extent);
		const auto tags_commands_bounds = command_bounds(tags_commands_extent);
		const auto movie_commands_bounds = command_bounds(movie_commands_extent);
		const auto busy_commands_bounds = command_bounds(busy_commands_extent);

		const auto show_media_edit_commands = !view_processing && view_mode == view_type::edit;
		const auto show_tool_commands = !view_processing &&
			(view_mode == view_type::rename || view_mode == view_type::batch);
		const auto show_import_commands = !view_processing && view_mode == view_type::import;
		const auto show_locate_commands = !view_processing && view_mode == view_type::locate;
		const auto show_sync_commands = !view_processing && view_mode == view_type::sync;
		const auto show_tags_commands = !view_processing && view_mode == view_type::tags;
		const auto show_movie_commands = !view_processing && view_mode == view_type::movie;
		const auto show_busy_commands = view_processing && can_show_view_controls;

		const auto selector_top = client_bounds.bottom - selector_height;
		const recti view_bounds(client_bounds.left, body_top, content_right, selector_top);
		const recti selector_bounds(client_bounds.left, selector_top, content_right, client_bounds.bottom);
		const recti view_controls_bounds(content_right + splitter_width, body_top, client_bounds.right,
		                                 client_bounds.bottom);

		_controls_splitter.bounds = can_show_view_controls
			                            ? recti(content_right, body_top, content_right + splitter_width,
			                                    client_bounds.bottom)
			                            : recti{};
		_controls_splitter.client_left = client_bounds.left;
		_controls_splitter.client_right = client_bounds.right;
		_controls_splitter.width = splitter_width;
		_controls_splitter.min_pane = splitter_min_pane;

		if (!can_show_view_controls)
		{
			_controls_splitter.hover = false;
			_controls_splitter.tracking = false;
		}

		ui::control_layouts positions;
		positions.emplace_back(_navigate1, nav1_bounds,
		                       show_items_controls && nav1_bounds.left > client_bounds.left);
		positions.emplace_back(_search_edit, search_bounds,
		                       show_items_controls && search_bounds.right < nav3_bounds.left);
		positions.emplace_back(_navigate2, nav2_bounds,
		                       show_items_controls && nav2_bounds.right < nav3_bounds.left);
		positions.emplace_back(_navigate3, nav3_bounds, show_items_controls);
		positions.emplace_back(_view_frame->frame(), view_bounds, _view->_show_render_window);
		positions.emplace_back(_selector_frame->frame(), selector_bounds, show_selector);
		positions.emplace_back(_media_edit_commands, media_edit_bounds, show_media_edit_commands);
		positions.emplace_back(_tool_commands, tool_commands_bounds, show_tool_commands);
		positions.emplace_back(_import_commands, import_commands_bounds, show_import_commands);
		positions.emplace_back(_locate_commands, locate_commands_bounds, show_locate_commands);
		positions.emplace_back(_sync_commands, sync_commands_bounds, show_sync_commands);
		positions.emplace_back(_tags_commands, tags_commands_bounds, show_tags_commands);
		positions.emplace_back(_movie_commands, movie_commands_bounds, show_movie_commands);
		positions.emplace_back(_busy_commands, busy_commands_bounds, show_busy_commands);

		if (_view_controls && _view_controls->_dlg)
		{
			for (const auto& control : _view_controls->_controls)
			{
				ui::enable_element(control, !view_processing);
			}
			positions.emplace_back(_view_controls->_dlg, view_controls_bounds, can_show_view_controls);
		}

		if (_search_predictions_frame && _search_predictions_frame->frame()->is_visible())
		{
			_search_predictions_frame->frame()->window_bounds(calc_search_popup_bounds(), true);
		}

		_view_bounds = view_bounds;
		_app_frame->apply_layout(positions, {0, 0});
	}
}


void parse_more_folders(df::index_roots& result, const std::string_view more_folders, const platform::drives& drives)
{
	df::hash_set<std::string, df::ihash, df::ieq> drive_label_includes;

	for (auto line : split_collection_folders(more_folders))
	{
		if (!str::is_empty(line))
		{
			if (str::is_exclude(line))
			{
				while (!line.empty() && (str::is_white_space(line.front()) || line.front() == '-'))
				{
					line = line.substr(1);
				}

				const auto exclude = str::trim(line);

				if (df::is_path(exclude))
				{
					// A syntactic full path is always recorded as an exclude, even
					// when it is not currently present (offline/removable drive or
					// a not-yet-created folder). Requiring existence here silently
					// dropped such excludes, making full-path exclusion unreliable.
					result.excludes.emplace(df::folder_path(exclude));
				}
				else
				{
					result.exclude_wildcards.emplace(str::cache(line));
				}
			}
			else
			{
				const auto path = df::folder_path(line);

				if (path.exists())
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
	df::hash_set<std::string, df::ihash, df::ieq> resolved_labels;

	for (const auto& d : drives)
	{
		if (drive_label_includes.contains(d.vol_name))
		{
			result.folders.emplace(df::folder_path(d.name));
			resolved_labels.emplace(d.vol_name);
		}
	}

	// Remaining bare names that look like a server are added directly. This allows
	// network shares or removable devices whose mapped drive letter can change.
	for (const auto& label : drive_label_includes)
	{
		if (!resolved_labels.contains(label) && platform::is_server(label))
		{
			result.folders.emplace(df::folder_path(label));
		}
	}

	for (const auto& exclude : result.excludes)
	{
		result.folders.erase(exclude);
	}
}

void parse_more_folders(df::index_roots& result, const std::string_view more_folders)
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
	df::scope_locked_inc depth(_completing_pending_events);

	if (_completing_pending_events > max_reentrancy_depth)
	{
		_pending_events_deferred = true;
		return;
	}

	try
	{
		if (!df::is_closing && _app_frame)
		{
			// Only the outermost pass is timed; a nested drain runs inside its parent's measurement.
			std::optional<df::perf_timer> ui_busy;
			if (_completing_pending_events == 1) ui_busy.emplace(df::ui_perf.idle_us, &df::ui_perf.idle_max_us);

			const auto tasks = _ui_queue.dequeue_all();
			df::bump(df::ui_perf.idle_drains);
			df::bump(df::ui_perf.idle_tasks, tasks.size());
			df::record_peak(df::ui_perf.idle_batch_peak, static_cast<uint32_t>(tasks.size()));

			for (const auto& t : tasks)
			{
				try
				{
					const auto wait_us = t.wait_us();
					df::bump(df::ui_perf.idle_wait_us, wait_us);
					df::record_peak(df::ui_perf.idle_wait_max_us,
					                static_cast<uint32_t>(std::min<uint64_t>(wait_us, UINT32_MAX)));
					t();
				}
				catch (const std::exception& e)
				{
					// Per task, as start_worker does: the batch has already been dequeued, so letting one
					// failure reach the outer handler would silently discard every task queued behind it.
					df::log(__FUNCTION__, e.what());
				}
			}

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
				_view_frame->frame()->invalidate();

				// The task view on screen reports under one key, so switching to a view that is not
				// running ends what the previous one showed.
				const auto progress = _view->progress();
				if (progress.active) work_progress(&_view, progress.position, progress.total);
				else work_finished(&_view);
			}

			if (pop_invalid_flag(_invalids, view_invalid::animations))
			{
				prepare_frame();
			}

			if (pop_invalid_flag(_invalids, view_invalid::options_save))
			{
				update_command_text();
				save_options();

				_view_frame->frame()->options_changed();
				_selector_frame->frame()->options_changed();
				_view_edit->options_changed();
				_view_movie->options_changed();
				_navigate1->options_changed();
				_search_edit->options_changed();
				_navigate2->options_changed();
				_navigate3->options_changed();
				_media_edit_commands->options_changed();
				_import_commands->options_changed();
				_locate_commands->options_changed();
				_tool_commands->options_changed();
				_sync_commands->options_changed();
				_tags_commands->options_changed();
				_movie_commands->options_changed();

				_state.update_search_is_favorite_or_collection_root();

				if (_search_predictions_frame)
				{
					_search_predictions_frame->frame()->options_changed();
				}
			}

			if (pop_invalid_flag(_invalids, view_invalid::refresh_items))
			{
				if (df::file_handles_detached == 0 && df::command_active == 0)
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
				const auto is_playing = display && display->is_playing();
				_pa->enable_screen_saver(!_state.is_full_screen && !is_playing);
			}

			if (pop_invalid_flag(_invalids, view_invalid::sidebar))
			{
				const auto& sidebar = _view_items->sidebar();
				sidebar->populate();
				sidebar->layout();
				auto search = _state.search();
				if (_state.item_index.thumbnailing_items.load() == 0 && _state.resolve_area_search(search))
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

					// locations.md 6.2: the timeline is derived from the result set, so it is
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
		df::log(__FUNCTION__, e.what());
	}

	if (_completing_pending_events == 1 && _pending_events_deferred.exchange(false))
	{
		_pa->queue_idle();
	}
}

void app_frame::idle()
{
	update_overlay();
	complete_pending_events();
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
	{
		make_dlg(_app_frame)->show_message(icon_index::error, tt.safe_start_title, tt.safe_start_message);
	});
}

bool app_frame::key_down(const char32_t key, const ui::key_state keys)
{
	// Ctrl is itself a key press, so this is where the modifier that arms drawing a region first
	// becomes known - and the pointer may not move again before the user acts on it.
	_view_frame->sync_modifier_cursor();

	if (df::command_active == 0)
	{
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
			if (key == keys::RETURN)
			{
				search_enter();
				return true;
			}
			if (key == keys::ESCAPE)
			{
				cancel_search_edit();
				return true;
			}
			if (key == keys::UP) return step_predictions(-1);
			if (key == keys::DOWN) return step_predictions(1);
			if (key == keys::TAB && !keys.control && !keys.shift && search_accept_selected()) return true;
		}

		// Enter submits the task view the user is in, so a task can be completed without leaving the
		// keyboard. Routed through the run command so a run the toolbar refuses stays refused, and
		// so a single-line entry field does not just beep.
		if (key == keys::RETURN && !keys.control && !keys.shift && !keys.alt)
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

		if (_view_has_focus && _view->focus_mode() == ui::focus_mode::text_edit)
		{
			return _view->key_down(key, keys);
		}

		if (key == keys::ESCAPE)
		{
			if (_view_controls && _view_controls->escape_controller()) return true;
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
			if (key == keys::SPACE && _view_frame->key_down_controller(key, keys)) return true;

			const auto display = _state.display_state();
			if (_view_has_focus && display && display->is_zoom_mode() && !keys.control && !keys.shift && !keys.alt)
			{
				constexpr double arrow_step = 48.0;
				if (key == keys::LEFT) display->pan_zoom_by({-arrow_step, 0.0});
				else if (key == keys::RIGHT) display->pan_zoom_by({arrow_step, 0.0});
				else if (key == keys::UP) display->pan_zoom_by({0.0, -arrow_step});
				else if (key == keys::DOWN) display->pan_zoom_by({0.0, arrow_step});
				else if (key == keys::PRIOR) _state.select_next_media(_view_frame, false);
				else if (key == keys::NEXT) _state.select_next_media(_view_frame, true);
				else if (key == keys::HOME) display->pan_zoom_to_horizontal_edge(false);
				else if (key == keys::END) display->pan_zoom_to_horizontal_edge(true);
				else goto no_zoom_pan_key;
				return true;
			}

		no_zoom_pan_key:
			if (key == keys::APPS)
			{
				const auto command = find_command(commands::menu_main);
				const auto menu = command && command->menu ? command->menu() : std::vector<ui::command_ptr>{};

				if (!menu.empty())
				{
					auto button_bounds = _navigate2->button_bounds(command);
					if (button_bounds.is_empty()) button_bounds = _navigate2->window_bounds();
					track_menu(_app_frame, button_bounds, menu);
				}
				return true;
			}
			if (key == keys::BROWSER_BACK)
			{
				_state.browse_back(_view_frame);
				return true;
			}
			if (key == keys::BROWSER_FORWARD)
			{
				_state.browse_forward(_view_frame);
				return true;
			}
			if (key == keys::BROWSER_REFRESH)
			{
				reload();
				return true;
			}
			if (key == keys::BROWSER_HOME)
			{
				invoke(commands::view_fullscreen);
				return true;
			}
			if (key == keys::BROWSER_STOP || key == keys::MEDIA_STOP)
			{
				const auto display = _state.display_state();
				if (display && display->is_playing_media()) _state.stop();
				return true;
			}
			if (key == keys::MEDIA_PLAY_PAUSE)
			{
				_state.play(_view_frame);
				return true;
			}
			if (key == keys::MEDIA_PREV_TRACK)
			{
				_state.select_next(_view_frame, false, keys.control, keys.shift);
				return true;
			}
			if (key == keys::MEDIA_NEXT_TRACK)
			{
				_state.select_next(_view_frame, true, keys.control, keys.shift);
				return true;
			}
			if (key == keys::RETURN && !keys.control && !keys.shift)
			{
				if (_state.enter(_view_frame)) return true;
			}

			if (_view_has_focus)
			{
				if (!keys.alt)
				{
					if (key == keys::LEFT)
					{
						_state.select_next(_view_frame, false, keys.control, keys.shift);
						return true;
					}
					if (key == keys::RIGHT)
					{
						_state.select_next(_view_frame, true, keys.control, keys.shift);
						return true;
					}
					if (key == keys::HOME)
					{
						_state.select_end(_view_frame, false, keys.control, keys.shift);
						return true;
					}
					if (key == keys::END)
					{
						_state.select_end(_view_frame, true, keys.control, keys.shift);
						return true;
					}

					if (_state.view_mode() == view_type::items)
					{
						if (key == keys::UP)
						{
							_state.stop_slideshow();
							_view_items->line_up(keys.control, keys.shift);
							_state.make_visible(_state.focus_item());
							return true;
						}
						if (key == keys::DOWN)
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

		const auto normalized_key = keys::normalize_numpad(key);
		const auto display = _state.display_state();
		const auto zoom_mode = display && display->is_zoom_mode();

		if (zoom_mode && !keys.control && !keys.alt)
		{
			if (!keys.shift && normalized_key == '0')
			{
				display->toggle_zoom_fit();
				return true;
			}
			if (normalized_key == keys::OEM_PLUS)
			{
				display->adjust_zoom_scale(1);
				return true;
			}
			if (!keys.shift && normalized_key == keys::OEM_MINUS)
			{
				display->adjust_zoom_scale(-1);
				return true;
			}
		}

		for (const auto& c : _commands)
		{
			const auto zoom_command = c.second->group == command_group::rate_flag ||
				c.first == commands::view_zoom || c.first == commands::view_zoom_toggle_fit ||
				c.first == commands::view_zoom_100 || c.first == commands::view_zoom_in ||
				c.first == commands::view_zoom_out || c.first == commands::view_zoom_pane_flip ||
				c.first == commands::view_fullscreen;
			if (zoom_mode && !zoom_command) continue;

			for (const auto& ac : c.second->kba)
			{
				if (ac.key == normalized_key)
				{
					const auto key_state_match =
						((ac.key_state & keyboard_accelerator_t::control) != 0) == keys.control &&
						((ac.key_state & keyboard_accelerator_t::shift) != 0) == keys.shift &&
						((ac.key_state & keyboard_accelerator_t::alt) != 0) == keys.alt;

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
	if (_search_has_focus) return ui::focus_mode::text_edit;
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

	// Full screen swaps to the media view, so release text focus before hiding the windowed chrome.
	if (_search_has_focus)
	{
		_view_items->blur_rendered_filter();
		focus_view();
	}

	_pa->full_screen(_state.is_full_screen);
	invalidate_view(view_invalid::app_layout | view_invalid::view_layout |
		view_invalid::screen_saver | view_invalid::command_state);
}


void app_frame::open_default_folder()
{
	bool success = false;

	if (!command_line.folder_path.is_empty())
	{
		// An empty entry is not "no selection": append_items reads a non-empty set as an explicit
		// list of paths to select, so a folder-only command line must pass an empty set.
		df::unique_paths selection;
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
		            df::search_t().add_selector(df::item_selector(known_path(platform::known_folder::pictures))), {});
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
}

void app_frame::focus_changed(const bool has_focus, const ui::control_base_ptr& child)
{
	df::trace(std::format("app_frame::focus {}", has_focus));

	if (child == _search_edit)
	{
		focus_search(has_focus);
	}

	_view_has_focus = _view_frame->frame()->has_focus();
	_view_controls_have_focus = _view_controls && _view_controls->_dlg && _view_controls->_dlg->has_focus();

	invalidate_view(view_invalid::view_redraw | view_invalid::command_state);
}

void app_frame::item_focus_changed(const df::item_element_ptr& focus, const df::item_element_ptr& previous)
{
	df::trace("app_frame::focus_changed");

	if (_view_items->is_visible(previous))
	{
		_view_items->make_visible(focus);
	}

	_view_selector->make_visible(focus);
}

static bool can_select_for_metadata_edit(const df::item_element_ptr& item)
{
	if (!item || item->is_folder()) return false;

	const auto* const file_type = item->file_type();
	if (!file_type->has_trait(file_traits::edit)) return false;

	// Sidecar metadata can always be written; embedded XMP needs a writable file.
	return !file_type->has_trait(file_traits::embedded_xmp) || !item->is_read_only();
}

static bool can_select_for_photo_edit(const df::item_element_ptr& item)
{
	return item && item->file_type()->can_edit_photo();
}

app_frame::selector_strip app_frame::selector_strip_for_view(const view_type m) const
{
	switch (m)
	{
	case view_type::edit:
		return selector_strip::photo;
	case view_type::locate:
	case view_type::tags:
		return selector_strip::metadata;
	case view_type::batch:
		return _view_batch->mode() == batch_tool_mode::metadata ? selector_strip::metadata : selector_strip::none;
	// Movie draws its own strip. That strip is the document -- ordered, duplicable, and removing a
	// tile removes it from the movie -- so sharing the selector would make one click mean "select"
	// in every other view and "seek" here.
	case view_type::movie:
		return selector_strip::none;
	default:
		return selector_strip::none;
	}
}

void app_frame::reset_selector_selection_anchor()
{
	_view_selector->reset_selection_anchor();
}

void app_frame::select_from_selector(const df::item_element_ptr& item, const ui::key_state keys)
{
	if (!item) return;

	switch (selector_strip_for_view(_state.view_mode()))
	{
	case selector_strip::photo:
		// Photo edit works on one item, so the strip navigates rather than selects.
		if (can_select_for_photo_edit(item)) _view_edit->select_item(item);
		break;

	case selector_strip::metadata:
		if (!can_select_for_metadata_edit(item)) break;

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

	case selector_strip::none:
		break;
	}
}

void app_frame::display_changed()
{
	df::assert_true(ui::is_ui_thread());

	if (!df::is_closing)
	{
		_view->display_changed();
		_view_selector->display_changed();

		invalidate_view(view_invalid::app_layout |
			view_invalid::view_layout |
			view_invalid::command_state |
			view_invalid::screen_saver |
			view_invalid::media_elements |
			view_invalid::controller |
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
	invalidate_view(view_invalid::view_layout |
		view_invalid::group_layout |
		view_invalid::index |
		view_invalid::refresh_items |
		view_invalid::item_scan |
		view_invalid::sidebar_drives |
		view_invalid::command_state);
}

void app_frame::view_changed(const view_type m)
{
	df::assert_true(ui::is_ui_thread());
	df::assert_true(m != view_type::media || _state.is_full_screen);

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
	case selector_strip::photo:
		_view_selector->filter([](const df::item_element_ptr& item)
		{
			return can_select_for_photo_edit(item);
		});
		_view_selector->activate(_selector_frame->_extent);
		break;

	case selector_strip::metadata:
		_view_selector->filter([m](const df::item_element_ptr& item)
		{
			if (!can_select_for_metadata_edit(item)) return false;
			// Locate can hide items that already carry a location, so a long list can be walked
			// down to nothing left to place.
			if (m == view_type::locate && setting.locate_only_without_location) return !item->has_gps();
			return true;
		});
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
	case view_type::items: v = _view_items;
		break;
	case view_type::media: v = _view_media;
		break;
	case view_type::edit: v = _view_edit;
		vc = _view_edit->controls(_app_frame);
		break;
	case view_type::movie: v = _view_movie;
		vc = _view_movie->controls(_app_frame);
		break;
	case view_type::rename: v = _view_rename;
		vc = _view_rename->controls(_app_frame);
		break;
	case view_type::batch: v = _view_batch;
		vc = _view_batch->controls(_app_frame);
		break;
	case view_type::import: v = _view_import;
		vc = _view_import->controls(_app_frame);
		break;
	case view_type::sync: v = _view_sync;
		vc = _view_sync->controls(_app_frame);
		break;
	case view_type::locate: v = _view_locate;
		vc = _view_locate->controls(_app_frame);
		break;
	case view_type::tags: v = _view_tags;
		vc = _view_tags->controls(_app_frame);
		break;
		break;
	default:
		break;
	}

	if (vc != _view_controls)
	{
		if (_view_controls && _view_controls->_dlg)
		{
			_view_controls->_dlg->show(false);
		}

		// The incoming panel is left hidden for the app layout to reveal, because that shrinks the view
		// frame and shows the panel in one batch. Shown any earlier it is still covered by the
		// full-width view frame, and its native controls would paint while clipped to nothing: the
		// paint is validated and lost, leaving them blank until something dirties them again.
		// Requested here rather than with the view change below, because a panel is also rebuilt
		// without the view changing: a new selection in Tags or Batch, or a language change.
		_view_controls = vc;
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
		_view->activate(_view_bounds.extent());
		// Offscreen only. The items the user was just looking at are the ones the media view is about to
		// step through, and their thumbnail surfaces are what stands in for each image while it loads.
		free_graphics_resources(true, true);
		focus_view();

		invalidate_view(view_invalid::app_layout |
			view_invalid::view_layout |
			view_invalid::group_layout |
			view_invalid::command_state |
			view_invalid::media_elements |
			view_invalid::tooltip |
			view_invalid::controller |
			view_invalid::address);
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

void app_frame::invoke(const command_info_ptr& command)
{
	try
	{
		if (command->enable)
		{
			df::log(__FUNCTION__, command->text);

			df::scope_locked_inc sl(df::command_active);
			command->invoke();
			invalidate_view(view_invalid::command_state);
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}
}

void app_frame::track_menu(const ui::frame_ptr& parent, const recti bounds,
                           const std::vector<ui::command_ptr>& commands)
{
	if (!parent) return;

	pause_media pause(_state);
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

		enum class choice { none, retry, parent };
		auto selected = std::make_shared<choice>(choice::none);

		// The parent of the scope that failed, not of the scope still on screen.
		const auto parent = find_parent_search(path);

		std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::error,
			                                                tt.scope_unavailable_title,
			                                                str_format(tt.scope_unavailable_fmt.sv(), path.text()))),
			std::make_shared<divider_element>(),
			set_margin(std::make_shared<link_element>(tt.scope_unavailable_retry, [dlg, selected]
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

bool app_frame::can_open_search(const df::search_t& link)
{
	if (_view->can_exit()) return true;

	// Say why the new scope was refused rather than appearing to do nothing.
	const auto name = _view->operation_name();

	if (!name.empty())
	{
		make_dlg(_app_frame)->show_message(icon_index::question, tt.cancel_operation_title,
		                                   str_format(tt.scope_busy_fmt.sv(), name));
	}

	return false;
}

void app_frame::folder_changed(const df::folder_path folder)
{
	// Sync clients and batch tools report many changes in quick succession, so collect the folders and
	// let tick compare them once the burst has settled.
	_folder_change_time = df::now();
	_folders_changed.emplace(folder);
}

void app_frame::dpi_changed()
{
	_view_items->sidebar()->dpi_changed();

	const view_element_event e{view_element_event_type::dpi_changed, nullptr};
	element_broadcast(e);

	invalidate_view(view_invalid::app_layout);
}

void app_frame::on_window_layout(ui::measure_context& mc, const sizei extent, const bool is_minimized)
{
	if (!is_minimized)
	{
		_extent = extent;
		layout(mc);
	}
}

bool app_frame::is_caption_area(const pointi loc) const
{
	if (_title_bounds.contains(loc)) return false;
	if (_top_bar_bounds.contains(loc)) return true;
	if (!_controls_splitter.bounds.is_empty() && _controls_splitter.bounds.contains(loc)) return false;
	if (!_view || !_view_bounds.contains(loc)) return false;

	return _view->is_caption_area({loc.x - _view_bounds.left, loc.y - _view_bounds.top});
}

void app_frame::on_window_paint(ui::draw_context& dc)
{
	if (!_state.is_full_screen && !_title_bounds.is_empty())
	{
		_app_logo->render(dc, {});
	}

	if (!_controls_splitter.bounds.is_empty())
	{
		// The app frame clears to toolbar_background, so the strip is repainted to match the
		// controls panel and read as part of it until the user touches it.
		dc.draw_rect(_controls_splitter.bounds,
		             ui::color(ui::style::color::dialog_background, dc.colors.alpha));

		if (_controls_splitter.hover || _controls_splitter.tracking)
		{
			draw_splitter_handle(dc, _controls_splitter.bounds, _controls_splitter.width, true,
			                     _controls_splitter.tracking);
		}
	}

	const auto border_outside = recti(_extent);
	const auto scale1 = df::round(1 * dc.scale_factor);
	const auto clr = ui::style::color::view_background;
	const auto border_clr = ui::color(clr, dc.colors.alpha);
	dc.draw_border(border_outside.inflate(-scale1), border_outside, border_clr, border_clr);
}

void app_frame::update_controls_splitter_hover(const bool hover)
{
	if (_controls_splitter.hover == hover) return;
	_controls_splitter.hover = hover;
	_app_frame->set_cursor(hover ? ui::style::cursor::left_right : ui::style::cursor::normal);
	_app_frame->invalidate(_controls_splitter.bounds);
}

void app_frame::drag_controls_splitter(const pointi loc)
{
	const auto& s = _controls_splitter;
	const auto available = std::max(0, s.client_right - s.client_left - s.width);
	if (available <= 0) return;

	// The pointer holds the middle of the strip, so the grab point does not jump on the first move.
	const auto panel = clamp_pane_width(s.client_right - loc.x - s.width / 2, available, s.min_pane);
	const auto pos = std::max(1, df::mul_div(panel, settings_t::view_splitter_max, available));

	if (setting.set_view_splitter(_state.view_mode(), pos))
	{
		// Direct pointer input, so lay out and repaint inside this message. Deferring to idle would
		// paint the strip at its new position while the panes are still at the old one. The setting
		// is written once the drag ends rather than on every move.
		_app_frame->layout();
		_app_frame->redraw_now();
	}
}

void app_frame::on_mouse_move(const pointi loc, const bool is_tracking)
{
	if (_controls_splitter.tracking)
	{
		drag_controls_splitter(loc);
		return;
	}

	update_controls_splitter_hover(!_controls_splitter.bounds.is_empty() &&
		_controls_splitter.bounds.contains(loc));

	if (_app_logo)
	{
		const auto logo_hover = _title_bounds.contains(loc);
		if (_app_logo->hover(logo_hover))
		{
			if (ui::animations_enabled)
			{
				ui::animations[_app_logo.get()] = [logo = _app_logo, frame = _app_frame]
				{
					const auto animating = logo->step_background();
					if (animating) frame->invalidate(logo->invalidate_bounds());
					return animating;
				};
				invalidate_view(view_invalid::animations);
			}
			else
			{
				if (_app_logo->step_background_if_animations_disabled()) _app_frame->invalidate(_app_logo->invalidate_bounds());
			}
		}

		if (_app_logo_hover != logo_hover)
		{
			_app_logo_hover = logo_hover;
			invalidate_view(view_invalid::tooltip);
		}
	}
}

void app_frame::on_mouse_leave(const pointi loc)
{
	update_controls_splitter_hover(false);

	if (_app_logo_hover)
	{
		_app_logo_hover = false;
		if (_app_logo->hover(false))
		{
			if (ui::animations_enabled)
			{
				ui::animations[_app_logo.get()] = [logo = _app_logo, frame = _app_frame]
				{
					const auto animating = logo->step_background();
					if (animating) frame->invalidate(logo->invalidate_bounds());
					return animating;
				};
				invalidate_view(view_invalid::animations);
			}
			else
			{
				if (_app_logo->step_background_if_animations_disabled()) _app_frame->invalidate(_app_logo->invalidate_bounds());
			}
		}
		invalidate_view(view_invalid::tooltip);
	}
}

void app_frame::on_mouse_left_button_down(const pointi loc, const ui::key_state keys)
{
	if (!_controls_splitter.bounds.is_empty() && _controls_splitter.bounds.contains(loc))
	{
		_controls_splitter.tracking = true;
		drag_controls_splitter(loc);
	}
}

void app_frame::on_mouse_left_button_up(const pointi loc, const ui::key_state keys)
{
	if (_controls_splitter.tracking)
	{
		drag_controls_splitter(loc);
		_controls_splitter.tracking = false;
		update_controls_splitter_hover(_controls_splitter.bounds.contains(loc));
		_app_frame->invalidate(_controls_splitter.bounds);
		invalidate_view(view_invalid::options_save);
		return;
	}

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

		queue_async(async_queue::cloud, [result, f]
		{
			f(result);
		});
	});
}

void app_frame::web_service_cache(std::string key, std::string value)
{
	queue_database([key = std::move(key), value = std::move(value)](const database& db)
	{
		db.web_service_cache(key, value);
	});
}

void app_frame::work_progress(const void* source, const int64_t done, const int64_t total)
{
	df::assert_true(ui::is_ui_thread());
	_work_reports.update(source, done, total);
	_pa->show_progress(done, total);
}

void app_frame::work_finished(const void* source)
{
	df::assert_true(ui::is_ui_thread());
	if (!_work_reports.finish(source)) return;

	if (const auto latest = _work_reports.latest())
	{
		_pa->show_progress(latest->done, latest->total);
	}
	else
	{
		_pa->end_progress();
	}
}


void app_frame::search_text_changed(const std::string_view text)
{
	if (!_search_setting_text && _search_predictions_frame && _search_has_focus)
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
	const auto can_process = _state.can_process_items_and_mark_errors(
		_view_frame, items, df::process_items_type::local_file_or_folder);

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
		auto title_control = std::make_shared<ui::title_control2>(
			dlg->_frame, icon_index::cancel, title, format_plural_text(info_fmt, items), std::vector<ui::const_surface_ptr>{},
			items.size());
		title_control->selection_async(items.thumbs(), items.size(), _state._async);
		controls.emplace_back(set_margin(title_control));

		const auto add_warning = [&controls](const std::string_view text)
		{
			auto warning = std::make_shared<text_element>(
				text, flex_item::stretch | view_element_style::important);
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
			bool should_select_next = false;
			// Navigation follows the selection, so it only applies when the selection is what is
			// leaving. Deleting one item from its own row must not move the cursor off a selection
			// that is still there.
			const auto deleting_the_selection = !items.empty() && items == _state.selected_items();
			const auto next = deleting_the_selection ? _state.next_unselected_item() : df::item_element_ptr{};

			// Only selector folders are live-watched, so a search that names no folder - related
			// items, duplicates, a tag or a date - has nothing watching it and would keep listing
			// what was just deleted. This is noted before the operation and reported after it
			// whatever the result, because a cancelled or partly failed delete still removes files.
			df::unique_folders touched;
			for (const auto& path : items.file_paths()) touched.emplace(path.folder());
			for (const auto& path : items.folder_paths()) touched.emplace(path.parent());

			{
				detach_file_handles detach(_state);
				shell_file_operation_ui processing(*_view_frame, _app_frame);

				constexpr auto allow_undo = true;
				const auto res = platform::delete_items(items.file_paths(), items.folder_paths(), allow_undo);

				if (res.success())
				{
					should_select_next = true;
				}
				else if (res.code != platform::file_op_result_code::CANCELLED)
				{
					dlg->show_message(icon_index::error, title, res.format_error(tt.delete_error));
				}

				_state.invalidate_view(view_invalid::view_layout |
					view_invalid::group_layout |
					view_invalid::command_state |
					view_invalid::app_layout);
			}

			_state.item_index.queue_validate_changed_folders(std::move(touched));

			if (should_select_next)
			{
				df::unique_paths selection;
				if (next) next->add_to(selection);
				_state.open(_view_frame, _state.search(), selection);
			}
		}
	}
}

void app_frame::focus_view()
{
	if (_view_frame)
	{
		_view_frame->frame()->focus();
	}
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
	df::log("main", df::format_version(false));

	// Written before anything can fail, so a truncated or crash-terminated log still says which
	// machine shape produced it. Perf numbers are only readable against the core count.
	df::log("main", std::format("windows {} {} {} | {} cores", platform::OS(),
	                            sizeof(void*) == 8 ? "64-bit" : "32-bit",
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

	// The chrome's embedded PNGs decode with the application's own decoder on every backend.
	ui::decode_embedded_image = [](const df::cspan data)
	{
		files ff;
		return ff.image_to_surface(data);
	};

	// The renderers scale and convert pictures with FFmpeg's swscale, as the decoder does.
	ui::make_surface_scaler = []() -> std::unique_ptr<ui::surface_scaler> { return std::make_unique<av_scaler>(); };

	return true;
}

int run_console_tests(std::string_view test_filter);
int generate_wiki_docs(std::string_view output_folder);
int validate_po_files();

bool app_frame::parse_command_line(const std::string_view command_line_text)
{
	command_line.parse(command_line_text);

	return command_line.console_test || command_line.gen_docs || command_line.validate_po ||
		command_line.dup_report || !command_line.movie_batch.projects.empty();
}

int app_frame::run_console_mode()
{
	// /test runs the suite; /gen-docs regenerates the wiki pages; /validate-po checks the
	// translations against the strings app_text registers; /dup-report measures duplicate grouping
	// over a real library, reading only; /movie-check, /movie-import and /movie-render report how
	// Movie projects' sources resolve, and import or render them when asked.
	if (command_line.console_test) return run_console_tests(command_line.test_filter);
	if (command_line.gen_docs) return generate_wiki_docs(command_line.docs_path);
	if (command_line.validate_po) return validate_po_files();
	if (command_line.dup_report)
		return run_duplicate_report(command_line.dup_report_folder, command_line.dup_report_output);
	return run_movie_batch(command_line.movie_batch);
}

bool app_frame::wants_gpu() const
{
	return setting.use_gpu && !command_line.no_gpu;
}


void app_frame::update_font_size() const
{
	if (_pa)
	{
		_pa->set_font_base_size(setting.large_font ? large_font_size : normal_font_size);
	}
}

bool app_frame::init(const std::string_view command_line_text)
{
	log_func lf(__FUNCTION__);
	df::log(__FUNCTION__, std::format("is_app_installed {}", is_app_installed()));

	// These files are skipped by the indexer for the rest of the session, so they are stated once
	// rather than left as an unexplained absence of metadata and thumbnails.
	if (const auto skipped = crash_files().skipped_file_count(); skipped > 0)
	{
		df::log(__FUNCTION__, std::format("skipping {} file(s) that crashed a previous run of this release{}",
		                                  skipped, crash_files().is_full() ? " - the crash list is full" : ""));
	}


	command_line.parse(command_line_text);
	load_file_types();
	metadata_xmp::initialise();
	_item_index.init_item_index();

	const auto& store = _settings;
	load_options(store);
	setting.instantiations++;

	// load_options re-runs setting.read(), which puts the persisted values back, so the safe
	// start decided in load_settings has to be re-applied over them. reset_presentation is
	// idempotent, so this is the same answer, not a second decision.
	if (_safe_start) setting.reset_presentation();

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
	if (should_seed_default_favorite_tags(setting.favorite_tags_initialized, str::is_empty(setting.favorite_tags),
	                                      store->root_created()))
	{
		setting.favorite_tags = tt.default_favorite_tags;
	}
	setting.favorite_tags_initialized = true;

	initialise_commands();

	_app_frame = _pa->create_app_frame(store, shared_from_this());

	if (!_app_frame)
	{
		app_fail(tt.error_create_window_failed, {});
		return false;
	}

	_view_frame->init(_app_frame);
	_selector_frame->init(_app_frame);
	create_toolbars();

	init_search();

	_bubble = _app_frame->create_bubble();
	_state.view_mode(view_type::items);
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

#ifdef _DEBUG
	if (!command_line.test_action.empty())
	{
		queue_ui([this] { run_test_action(command_line.test_action); });
	}
#endif

	if (!setting.sound_device.empty())
	{
		_state.change_audio_device(setting.sound_device);
	}

	for (const auto* const group : _starting_media_filter)
	{
		_state.filter().add_group(group);
	}

	_state.group_order(_starting_group_order, _starting_sort_order);
	focus_view();

#ifdef _DEBUG
	_screenshot_ready_time = 0;
#endif

	return true;
}


void app_frame::on_window_destroy()
{
	log_func lf(__FUNCTION__);

	if (_app_logo) _app_logo->free_graphics_resources();
	_bubble.reset();
	_search_predictions_frame.reset();
	_view_controls.reset();
	_view_frame.reset();
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

// Context menu layouts. commands::none marks a separator. Anything with a submenu of its own
// appears once, at the top level, rather than also being spelled out beside it.
static constexpr commands s_menu_media[] = {
	commands::play,
	commands::slideshow,
	commands::view_fullscreen,
	commands::none,
	commands::browse_previous_item,
	commands::browse_next_item,
	commands::browse_previous_group,
	commands::browse_next_group,
	commands::none,
	commands::menu_zoom,
	commands::menu_open,
	commands::menu_tools,
	commands::menu_rate_or_label,
	commands::playback_menu,
	commands::menu_display_options,
	commands::none,
	commands::tool_delete,
	commands::tool_rename,
	commands::tool_copy_to_folder,
	commands::tool_move_to_folder,
	commands::edit_cut,
	commands::edit_copy,
	commands::edit_copy_item_path,
};

static constexpr commands s_menu_items[] = {
	commands::menu_navigate,
	commands::menu_open,
	commands::menu_tools,
	commands::menu_rate_or_label,
	commands::menu_select,
	commands::menu_group,
	commands::menu_display_options,
	commands::none,
	commands::tool_delete,
	commands::tool_rename,
	commands::tool_copy_to_folder,
	commands::tool_move_to_folder,
	commands::edit_cut,
	commands::edit_copy,
	commands::edit_copy_item_path,
	commands::edit_paste,
	commands::none,
	commands::refresh,
	commands::tool_new_folder,
};

static constexpr commands s_menu_movie[] = {
	commands::tool_movie_send_to_end,
	commands::tool_movie_remove,
	commands::tool_movie_relink,
	commands::none,
	commands::tool_movie_select_all,
	commands::tool_movie_add,
};

static constexpr commands s_menu_view[] = {
	commands::view_close,
};

static constexpr commands s_menu_frame[] = {
	commands::refresh,
	commands::tool_eject,
	commands::none,
	commands::options_collection,
	commands::options_sidebar,
	commands::favorite_tags,
	commands::none,
	commands::view_show_sidebar,
	commands::view_favorite_tags,
	commands::none,
	commands::large_font,
};

std::vector<ui::command_ptr> app_frame::menu(const pointi loc)
{
	update_button_state(false);

	const auto view_window_bounds = _view_frame->frame()->window_bounds();
	std::span<const commands> ids = s_menu_frame;

	if (view_window_bounds.contains(loc))
	{
		switch (_view->context_menu(loc - view_window_bounds.top_left()))
		{
		case menu_type::sidebar: ids = s_menu_frame;
			break;
		case menu_type::media: ids = s_menu_media;
			break;
		case menu_type::items: ids = s_menu_items;
			break;
		case menu_type::movie: ids = s_menu_movie;
			break;
		case menu_type::view:
		default: ids = s_menu_view;
			break;
		}
	}

	std::vector<ui::command_ptr> result;
	result.reserve(ids.size());

	for (const auto id : ids)
	{
		result.emplace_back(id == commands::none ? nullptr : find_command(id));
	}

	return result;
}

void app_frame::exit()
{
	log_func lf(__FUNCTION__);
	_threads.clear();
}

void app_frame::system_event(const ui::os_event_type ost)
{
	df::assert_true(ui::is_ui_thread());

	if (ost == ui::os_event_type::system_device_change)
	{
		_has_burner.reset();
		invalidate_view(
			view_invalid::app_layout | view_invalid::view_layout | view_invalid::sidebar_drives |
			view_invalid::index);
	}
	else if (ost == ui::os_event_type::options_changed)
	{
		invalidate_view(view_invalid::options);
	}
	else if (ost == ui::os_event_type::dpi_changed)
	{
		invalidate_view(view_invalid::visual_options);
	}
	else if (ost == ui::os_event_type::screen_locked)
	{
		_state.stop();
	}
	else if (ost == ui::os_event_type::system_suspending)
	{
		// The machine may never come back (hibernate failure, flat battery), so treat suspend as
		// the last chance to persist. Stopping also releases the sleep block we are about to lose.
		_state.stop();
		save_options();
	}
	else if (ost == ui::os_event_type::system_resumed)
	{
		invalidate_view(
			view_invalid::app_layout | view_invalid::view_layout | view_invalid::sidebar |
			view_invalid::index | view_invalid::screen_saver);
	}
	else if (ost == ui::os_event_type::session_ending)
	{
		// No prompting and no teardown: this runs before any shutdown question is asked, and again
		// once the session is really ending, and the process is terminated regardless of what it
		// does next.
		save_options();
	}
	else if (ost == ui::os_event_type::graphics_device_lost)
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

	df::log("main", "exit");
	df::close_log();
}

ui::app_ptr create_app(const ui::plat_app_ptr& pa)
{
	return std::make_shared<app_frame>(pa);
}

void app_frame::crash(const df::file_path dump_file_path)
{
	// Claimed atomically: a test-then-increment let a second thread faulting inside this handler
	// start its own report and post a duplicate.
	auto unclaimed = 0;

	if (df::handling_crash.compare_exchange_strong(unclaimed, 1))
	{
		const df::scope_exit release_claim([] { --df::handling_crash; });

		if (_app_frame)
		{
			_app_frame->show(false);
		}

		flush_open_files_to_crash_files_list();

#ifndef WINSTORE
		if (setting.send_crash_dumps)
		{
			df::log(__FUNCTION__, "*** CRASH ***");

			if (const auto last_loaded = df::last_loaded_path.load(std::memory_order_relaxed);
				!last_loaded.is_empty())
			{
				df::log(__FUNCTION__, std::format("Last file type opened: {}", last_loaded.extension()));
			}

			const auto* const render_func = df::rendering_func.load(std::memory_order_relaxed);

			if (!str::is_empty(render_func))
			{
				df::log(__FUNCTION__, std::format("Rendering function: {}", str::utf8_cast(render_func)));
			}

			if (dump_file_path.is_empty())
			{
				// Reported anyway: the logs carry the timeline, the configuration and the last file opened,
				// which is often enough to attribute the fault without a dump.
				df::log(__FUNCTION__, "no minidump was written - reporting logs only");
			}

			const auto log_file_path = df::close_log();
			const auto previous_log_path = df::previous_log_path;
			const auto crash_zip_path = platform::temp_file();

			const auto now = platform::now();
			const auto date = now.date();

			const auto name = std::format("Diffractor-{}-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp",
			                              s_app_version, g_app_build, date.year, date.month, date.day,
			                              date.hour, date.minute, date.second);

			df::zip_file zip;
			auto has_zip = false;

			if (zip.create(crash_zip_path))
			{
				if (!dump_file_path.is_empty()) zip.add(dump_file_path, name);
				if (log_file_path.exists()) zip.add(log_file_path);
				if (previous_log_path.exists()) zip.add(previous_log_path);
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

			if (crash_zip_path.exists()) platform::delete_file(crash_zip_path);
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
			df::log(__FUNCTION__,
			        "Previous run crashed during hardware video decode - disabling HW video decode (GPU rendering kept)");
		}
	}
	else if (gpu_crashed)
	{
		if (setting.use_gpu)
		{
			platform::suppress_crash_guard(platform::crash_guard::gpu_render, true);
			df::log(__FUNCTION__,
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
	_settings = store;

	// A crash before the first frame repeats on every relaunch, and nothing the user did caused
	// it, so nothing they can stop doing avoids it. The count of starts that never settled is
	// raised here - the earliest point that has a store - and cleared once this one settles.
	_owns_startup_scope = platform::claim_startup_scope();

	uint32_t unsettled_starts = 0;
	store->read({}, s_unsettled_starts, unsettled_starts);
	const auto history = decide_startup(_owns_startup_scope, unsettled_starts);
	_safe_start = history.safe_start;
	if (history.record) store->write({}, s_unsettled_starts, history.next_unsettled);

	setting.read();

	// Must happen here rather than in init(): the caller creates the graphics factories from
	// setting.use_gpu before init() runs, so a safe start decided later would leave the very
	// device creation it exists to avoid already done.
	if (_safe_start)
	{
		df::log(__FUNCTION__, std::format("{} starts did not settle - reverting to safe presentation",
		                                  unsettled_starts));
		setting.reset_presentation();
	}

	apply_gpu_crash_guard();
	connect_yuv_latch();

	return true;
}

std::string app_frame::restart_cmd_line()
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
