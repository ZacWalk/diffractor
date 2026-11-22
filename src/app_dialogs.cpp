// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The dialogs commands open that are features in their own right - the update offer, the
// keyboard reference, About, index maintenance and sharing. app_commands.cpp registers
// the commands that open them; the settings themselves are the Options view's (view_options.cpp).

#include "pch.h"

#include "model.h"
#include "model_index.h"
#include "model_db.h"
#include "model_tokenizer.h"
#include "util_zip.h"
#include "ui_controls.h"
#include "ui_dialog.h"
#include "ui_controllers.h"
#include "app_text.h"
#include "app_command_status.h"
#include "app_util.h"
#include "app.h"
#include "app_dialogs.h"

static constexpr auto docs_url = "https://www.diffractor.com/docs";
static constexpr auto releases_url = "https://www.diffractor.com/releases";
static constexpr auto support_url = "https://github.com/ZacWalk/diffractor/issues";
static constexpr auto donate_url = "https://www.paypal.com/donate/?hosted_button_id=HX5NRS9JGKLRL";

#ifndef WINSTORE
void show_update_dialog(view_state& s, const ui::frame_ptr& parent)
{
	const auto title = tt.update_title;

	auto dlg = make_dlg(parent);
	std::vector<view_element_ptr> controls;

	pause_media pause(s);

	// Phase 1: always check online for a newer version, showing a busy indicator.
	controls.emplace_back(set_margin(std::make_shared<ui::title_control>(icon_index::lightbulb, title)));
	controls.emplace_back(std::make_shared<ui::busy_control>(dlg->_frame, icon_index::lightbulb, tt.update_checking));
	controls.emplace_back(std::make_shared<ui::close_control>(dlg->_frame, true, tt.button_close));

	auto found_version = std::make_shared<std::string>();
	auto check_failed = std::make_shared<bool>(false);
	// The tasks hold the dialog weakly: a dialog released last on the web thread would destroy its
	// window there, and one closed early would linger until the request ended.
	const std::weak_ptr weak_dlg = dlg;

	s.queue_async(async_queue::web, [&s, weak_dlg, found_version, check_failed]
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
			core::json::json_doc json;
			json.Parse(response.body);
			version = core::json::safe_string(json, "current_version");
		}

		const auto failed = version.empty();

		s.queue_ui([weak_dlg, found_version, check_failed, version, failed]
		{
			*found_version = version;
			*check_failed = failed;
			if (const auto d = weak_dlg.lock()) d->close(false);
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
	if (core::version(s_app_version) >= core::version(setting.available_version))
	{
		dlg->show_message(icon_index::lightbulb, title, str_format(tt.update_up_to_date_fmt.sv(), s_app_version));
		return;
	}

	controls.clear();
	controls.emplace_back(set_margin(std::make_shared<ui::title_control>(icon_index::lightbulb, title)));
	controls.emplace_back(set_margin(
		std::make_shared<text_element>(str_format(tt.update_help_fmt.sv(), setting.available_version, s_app_version))));

	controls.emplace_back(std::make_shared<ui::button_control>(dlg->_frame, icon_index::import, tt.update_install_now,
	                                                           tt.update_help, [f = dlg->_frame] { f->close(); }));

	controls.emplace_back(std::make_shared<ui::button_control>(dlg->_frame, icon_index::time, tt.update_not_now,
	                                                           tt.update_not_now_help, [&s, f = dlg->_frame]
	{
		setting.min_show_update_day = core::utc_now().to_days() + 7;
		s.invalidate_view(view_invalid::view_layout | view_invalid::app_layout);
		f->close(true);
	}));

	controls.emplace_back(std::make_shared<ui::button_control>(dlg->_frame, icon_index::question, tt.update_more_info,
	                                                           tt.update_more_info_help, [f = dlg->_frame]
	{
		platform::open("https://www.diffractor.com/blog");
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
			core::file_path path;
		};

		auto state = std::make_shared<download_state>();
		auto download_complete = [&s, weak_dlg, state](const core::file_path download_path)
		{
			s.queue_ui([weak_dlg, state, download_path]
			{
				if (state->active)
				{
					state->completed = true;
					state->path = download_path;
					if (const auto d = weak_dlg.lock()) d->close(state->path.is_empty());
				}
			});
		};

		s.queue_async(async_queue::web, [download_complete]
		{ platform::download_and_verify("diffractor.com"sv, "diffractor-setup.exe"sv, download_complete); });

		if (dlg->show_modal(controls) == ui::close_result::ok)
		{
			state->active = false;

			// Launch the downloaded installer. It is interactive and will close any
			// running instance of Diffractor before installing over the current folder.
			const auto module_folder = known_path(platform::known_folder::running_app_folder);
			const auto install_result = platform::install(state->path, module_folder, false, false);

			if (install_result.failed())
			{
				dlg->show_message(icon_index::error, core::app_name(), install_result.format_error(tt.update_failed));
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

	std::ranges::sort(items, [](auto&& left, auto&& right) { return core::icmp(left->text, right->text) < 0; });

	keyboard_ref_section section;
	section.title = title;
	section.rows.reserve(items.size());

	for (const auto& c : items)
	{
		section.rows.emplace_back(c->keyboard_accelerator_text, c->text);
	}

	sections.emplace_back(std::move(section));
}

// The keyboard reference is read top to bottom, so the sections follow the order a user meets
// them rather than the column packing the old layout needed.
static std::vector<keyboard_ref_section> build_keyboard_reference(const commands_map& commands)
{
	std::vector<keyboard_ref_section> sections;

	keyboard_ref_section basics;
	basics.title = tt.keyboard_basics_title;
	basics.rows.emplace_back(platform::format_keyboard_accelerator(
								 {platform::keyboard_accelerator_t{platform::keys::RETURN}}),
	                         std::string(tt.keyboard_enter_desc.sv()));
	basics.rows.emplace_back(platform::format_keyboard_accelerator(
								 {platform::keyboard_accelerator_t{platform::keys::SPACE}}),
	                         std::string(tt.keyboard_space_desc.sv()));
	basics.rows.emplace_back(platform::format_keyboard_accelerator(
								 {platform::keyboard_accelerator_t{platform::keys::ESCAPE}}),
	                         std::string(tt.keyboard_escape_desc.sv()));
	basics.rows.emplace_back(platform::format_keyboard_accelerator({close_view_shortcut()}),
	                         std::string(tt.keyboard_close_desc.sv()));
	basics.rows.emplace_back(platform::format_keyboard_accelerator(
								 {platform::keyboard_accelerator_t{platform::keys::LEFT},
	                              platform::keyboard_accelerator_t{platform::keys::RIGHT}}),
	                         std::string(tt.keyboard_left_right_desc.sv()));
	basics.rows.emplace_back(platform::format_keyboard_accelerator(
								 {platform::keyboard_accelerator_t{platform::keys::UP},
	                              platform::keyboard_accelerator_t{platform::keys::DOWN}}),
	                         std::string(tt.keyboard_up_down_desc.sv()));
	basics.rows.emplace_back(platform::format_keyboard_accelerator(
								 {platform::keyboard_accelerator_t{platform::keys::HOME},
	                              platform::keyboard_accelerator_t{platform::keys::END}}),
	                         std::string(tt.keyboard_home_end_desc.sv()));
	basics.rows.emplace_back(platform::format_keyboard_accelerator(
								 {platform::keyboard_accelerator_t{platform::keys::OEM_PLUS},
	                              platform::keyboard_accelerator_t{platform::keys::OEM_MINUS}}),
	                         std::string(tt.keyboard_zoom_keys_desc.sv()));
	basics.rows.emplace_back(platform::format_keyboard_accelerator(
								 {platform::keyboard_accelerator_t{platform::keys::LEFT,
	                                                               platform::keyboard_accelerator_t::control},
	                              platform::keyboard_accelerator_t{platform::keys::RIGHT,
	                                                               platform::keyboard_accelerator_t::control}}),
	                         std::string(tt.keyboard_ctrl_left_right_desc.sv()));
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

void show_keyboard_reference(view_state& s, const ui::frame_ptr& parent, const commands_map& commands)
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
	controls.emplace_back(std::make_shared<ui::button_row>(
		std::vector<ui::control_ptr>{ui::make_button(tt.copy_to_clipboard, [&sections]
	{ platform::set_clipboard(format_keyboard_reference(sections)); }),
	                                 ui::make_button(tt.button_close, [h = dlg->_frame] { h->close(false); }, true)}));

	pause_media pause(s);
	// Title above and actions below stay put; only the key list scrolls.
	dlg->_pinned_header = 2;
	dlg->_pinned_footer = 2;
	dlg->show_modal(controls, {88}, {88});
}

void send_info(const view_state& s, const ui::frame_ptr& parent)
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
		// An agent session reaches the network only where it was started to.
		if (!df::network_allowed)
		{
			results->abort(tt.diagnostics_send_failed);
			return;
		}

		const auto log_file_path = platform::log_path;
		const auto previous_log_path = platform::previous_log_path;
		const auto crash_zip_path = platform::temp_file();
		const auto log_file_copy = platform::temp_file();

		if (platform::exists(log_file_path))
		{
			platform::copy_file(log_file_path, log_file_copy, true, true);
		}

		df::zip_file zip;
		auto has_zip = false;

		if (zip.create(crash_zip_path))
		{
			if (platform::exists(log_file_copy)) zip.add(log_file_copy, "diffractor.log");
			if (platform::exists(previous_log_path)) zip.add(previous_log_path);
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

		if (platform::exists(log_file_copy)) platform::delete_file(log_file_copy);
		if (platform::exists(crash_zip_path)) platform::delete_file(crash_zip_path);

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

void about_invoke(view_state& s, const ui::frame_ptr& parent, commands_map& commands)
{
	const auto dlg = make_dlg(parent);
	auto dlg_parent = dlg->_frame;

	std::vector<view_element_ptr> controls;
	controls.emplace_back(create_app_logo_element(s, render::font_face::mega, false, false, 1.6, flex_item::center));
	controls.emplace_back(std::make_shared<text_element>(df::format_version(false), render::font_face::dialog,
	                                                     render::text_style::single_line_center, flex_item::center));
	controls.emplace_back(std::make_shared<divider_element>());

	auto cols = std::make_shared<ui::col_control>();

	const auto learn = std::make_shared<ui::group_control>();
	learn->add(std::make_shared<ui::title_control>(icon_index::question, tt.documentation));
	learn->add(std::make_shared<text_element>(tt.about_info));
	learn->add(std::make_shared<link_element>(tt.learn_more_diffractor_com, [] { platform::open(docs_url); }));
	learn->add(std::make_shared<link_element>(tt.releases, [] { platform::open(releases_url); }));
	learn->add(std::make_shared<ui::title_control>(icon_index::keyboard, tt.keyboard));
	learn->add(std::make_shared<link_element>(tt.list_of_accelerators, [&s, dlg_parent, &c = commands]
	{ show_keyboard_reference(s, dlg_parent, c); }));

	const auto support = std::make_shared<ui::group_control>();
	support->add(std::make_shared<ui::title_control>(icon_index::buy, tt.donate));
	support->add(std::make_shared<text_element>(tt.donate_help));
	support->add(std::make_shared<link_element>(tt.donate_link, [] { platform::open(donate_url); }));
	support->add(std::make_shared<ui::title_control>(icon_index::support, tt.support));
	support->add(std::make_shared<link_element>(tt.help_more_info, [] { platform::open(support_url); }));
	support->add(ui::make_button(tt.help_send_info, [&s, dlg_parent] { send_info(s, dlg_parent); }));

	cols->add(set_margin(learn, 8, 8));
	cols->add(set_margin(support, 8, 8));
	controls.emplace_back(cols);
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<ui::close_control>(dlg->_frame));

	pause_media pause(s);
	dlg->show_modal(controls, {55});
};

std::string format_index_text(const view_state& s)
{
	const auto file_types = s.item_index.file_types();
	const auto total = file_types.total_items();
	const auto num = platform::format_number(core::to_string(total.count));
	const auto database_size = platform::format_size(s.item_index.stats.database_size);
	const auto text = str_format(tt.index_size_fmt.sv(), database_size, num);
	return text;
}

class path_text_element final : public std::enable_shared_from_this<path_text_element>, public view_element
{
	std::string _text;
	core::file_path _path;
	render::font_face _font = render::font_face::dialog;
	render::text_style _text_style = render::text_style::multiline;

public:
	path_text_element(const core::file_path path) noexcept :
		view_element(view_element_style::has_tooltip | view_element_style::can_invoke), _text(path.str()), _path(path)
	{
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		dc.draw_text(_text, logical_bounds, _font, _text_style, core::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
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

	void tooltip(view_hover_element& result, const pointi /*loc*/, const pointi element_offset) const override
	{
		result.elements->add(make_icon_element(icon_index::data, flex_item::no_break));
		result.elements->add(std::make_shared<text_element>(_text, render::font_face::title,
		                                                    render::text_style::multiline, flex_item::line_break));
		result.active_bounds = result.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

void index_maintenance_invoke(const view_state& s, const ui::frame_ptr& parent)
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
			error = core::utf8_cast(e.what());
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
		set_margin(
			std::make_shared<ui::title_control2>(dlg->_frame, icon_index::settings, title, tt.defragment_and_compact)),
		std::make_shared<divider_element>(),
		set_margin(std::make_shared<text_element>(format_index_text(s))),
		set_margin(std::make_shared<path_text_element>(s.item_index.stats.database_path)),
		set_margin(std::make_shared<text_element>(tt.index_maintenance_help)),
		set_margin(std::make_shared<ui::check_control>(dlg->_frame, tt.reset_database, is_reset)),
	};

	if (check_result->has_errors)
	{
		controls.emplace_back(
			set_margin(set_padding(std::make_shared<text_element>(tt.index_maintenance_reset_recommended,
		                                                          render::text_style::multiline),
		                           8, 8)));
	}
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<ui::ok_cancel_control>(dlg->_frame));

	if (dlg->show_modal(controls) == ui::close_result::ok)
	{
		dlg->show_status(icon_index::star, is_reset ? tt.resetting : tt.defragmenting);
		const auto maintenance_result = std::make_shared<database_result>();
		const auto finished = std::make_shared<core::thread_event>(true, false);

		s._async.queue_database([&s, maintenance_result, finished, is_reset](database& db)
		{
			std::string error;

			try
			{
				db.maintenance(is_reset);
			}
			catch (const std::exception& e)
			{
				error = core::utf8_cast(e.what());
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

// The share sheet needs a title. One file is named; several are counted.
static std::string share_sheet_title(const std::vector<core::file_path>& paths)
{
	return paths.size() == 1 ? paths.front().name().str()
	                         : format_plural_text(tt.title_item_count_fmt, static_cast<int64_t>(paths.size()));
}

// MAPI shows the mail client's own modal UI, so the draft is created on the UI thread despite
// blocking it. The progress dialog stays up meanwhile, saying what it is waiting for.
static void email_outgoing_files(view_state& s, const item_results_ptr& results,
                                 const std::vector<core::file_path>& file_paths, const outgoing_options& outgoing)
{
	s.queue_async(async_queue::work, [&s, results, file_paths, outgoing]
	{
		const auto staging = outgoing_staging_folder();
		const auto prepared = prepare_outgoing_files(file_paths, outgoing, staging, *results);

		// A cancel that lands after the last file is still a decision not to send.
		if (!prepared.success || results->is_canceled())
		{
			results->complete(results->is_canceled()   ? std::string_view{}
			                  : prepared.error.empty() ? tt.email_failed.sv()
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

void share_invoke(view_state& s, const ui::frame_ptr& parent, const view_host_base_ptr& view)
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
		// commits, as the Options view does.
		settings_t::outgoing_choices_t& saved_choices = use_sheet ? setting.share : setting.email;
		auto choices = saved_choices;
		std::string validation_error;

		for (;;)
		{
			std::vector<view_element_ptr> controls;
			auto title_control = std::make_shared<ui::title_control2>(dlg->_frame, icon, title,
			                                                          format_plural_text(use_sheet ? tt.share_info_fmt
			                                                                                       : tt.email_info_fmt,
			                                                                             items),
			                                                          std::vector<core::const_surface_ptr>{},
			                                                          items.size());
			title_control->selection_async(items.thumbs(), items.size(), s._async);
			controls.emplace_back(set_margin(title_control));
			controls.emplace_back(std::make_shared<divider_element>());
			controls.emplace_back(
				set_margin(std::make_shared<text_element>(use_sheet ? tt.share_small_help : tt.email_small_help)));
			controls.emplace_back(
				std::make_shared<ui::check_control>(dlg->_frame, use_sheet ? tt.share_zip : tt.email_zip, choices.zip));
			controls.emplace_back(
				std::make_shared<ui::check_control>(dlg->_frame, tt.email_convert_to_jpeg, choices.convert));

			auto limit = std::make_shared<ui::check_control>(dlg->_frame, tt.email_limit_dimensions, choices.limit);
			limit->child(std::make_shared<ui::num_control>(dlg->_frame, std::string_view{}, choices.max_side));
			controls.emplace_back(limit);

			if (!validation_error.empty())
			{
				auto warning = std::make_shared<text_element>(validation_error,
				                                              flex_item::stretch | view_element_style::important);
				warning->margin = {10, 10};
				warning->padding = {10, 10};
				warning->update_background_color();
				controls.emplace_back(std::move(warning));
			}

			controls.emplace_back(std::make_shared<divider_element>());
			controls.emplace_back(
				std::make_shared<ui::ok_cancel_control>(dlg->_frame, use_sheet ? tt.button_share : tt.button_send));

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
			email_outgoing_files(s,
			                     std::make_shared<command_status>(s._async, dlg, icon, title, file_paths.size(),
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

				rr.complete(prepared->success || results->is_canceled() ? std::string_view{}
				            : prepared->error.empty()                   ? tt.share_failed.sv()
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
