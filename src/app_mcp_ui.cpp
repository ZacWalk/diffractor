// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The agent tools over the window (app_mcp.h): what it shows, its commands, where it is and what is selected,
// input acted out as a tutorial script's steps, the dialogs that open and their answers, screenshots and recordings,
// and tutorials played and filmed. Commands and input that can change files need -mcp-allow:write; pictures need
// capture.

#include "pch.h"

#include "app_mcp.h"
#include "app_mcp_json.h"
#include "files.h"
#include "model_db.h"
#include "model_faces_search.h"
#include "model_index.h"
#include "model.h"
#include "model_display.h"
#include "model_search.h"
#include "ui_dialog.h"
#include "view_list.h"
#include "app_command_line.h"
#include "app.h"

namespace
{
	constexpr std::string_view wait_property =
		R"("wait_s":{"type":"number","minimum":0,"maximum":600,"description":"How long to wait for the answer before replying with a job to wait on; 50 by default."})";

	constexpr std::string_view list_property =
		R"("list":{"type":"integer","minimum":0,"maximum":500,"description":"How many items to name; 20 by default"})";

	// A selection, a command's effect and a layout reach what the window reports on its next frame: what a tool reads
	// back once nothing is waiting for one.
	constexpr double caught_up_quiet_s = 0.05;
	constexpr double caught_up_timeout_s = 5.0;

	// Including a task's review, which a change to its panel or the selection sets working out again.
	bool window_caught_up(const app_frame& app)
	{
		return app._invalids.load() == view_invalid::none && !(app._view && app._view->reviewing());
	}

	bool refused_while_modal(const mcp_host& host, const core::mcp::call_ptr& call)
	{
		if (!host.modal_ui_open()) return false;

		// Input waits behind work a dialog shows under way as it does behind a question, but there is nothing to answer.
		const auto work = dialogs_showing_work();
		call->finish(core::mcp::tool_result::error(
			!work.empty() && !host.answerable_ui_open()
				? std::format("Diffractor is busy - {} - and input waits behind it: wait_until_idle, then try again",
		                      join_text(work, ", "))
				: "A dialog or menu is open, and input waits behind it: ui_state shows it, dialog_respond answers a "
				  "dialog and menu_respond a menu"s));
		return true;
	}

	// Answers the call once ready has held for quiet_s, or once timeout_s pass, with what answer makes of it; and
	// sooner, where interruptible, if a dialog or menu opens - the job then goes on, for job_wait.
	void answer_when(mcp_host& host, const core::mcp::call_ptr& call, const std::string_view tool,
	                 std::function<bool()> ready, const double quiet_s, const double timeout_s,
	                 const bool interruptible, std::function<core::mcp::tool_result(bool settled)> answer)
	{
		const auto job = host.start_job(tool);
		host.when(std::move(ready), quiet_s, timeout_s, [job, answer = std::move(answer)](const bool met)
		{
			if (!job->is_done()) job->complete(answer(met));
		});
		host.wait(call, job, timeout_s + 1.0, interruptible);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Dialogs
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// A control's text as it reads: its mnemonic marker taken out.
	std::string plain_label(const std::string_view text)
	{
		std::string result;

		for (size_t i = 0; i < text.size(); ++i)
		{
			if (text[i] == '&')
			{
				if (i + 1 < text.size() && text[i + 1] == '&')
				{
					result += '&';
					++i;
				}

				continue;
			}

			result += text[i];
		}

		return result;
	}

	// What a dialog shows, and the controls an answer works.
	struct dialog_parts
	{
		struct button_part
		{
			ui::button_ptr control;
			std::string text;
			bool is_default = false;
			bool enabled = true;
		};

		struct check_part
		{
			ui::button_ptr control;
			std::string text;
			bool checked = false;
			bool radio = false;
			bool enabled = true;
		};

		// A field is typed into, or holds a date.
		struct field_part
		{
			ui::edit_ptr edit;
			ui::date_time_control_ptr date;
			std::string label;
			std::string text;
			bool enabled = true;
		};

		std::shared_ptr<dialog> owner;
		// A prompt the system put up in front, rather than one of Diffractor's dialogs: answered through the platform,
		// by its buttons alone.
		uintptr_t system_prompt = 0;
		std::vector<std::string> titles;
		std::vector<std::string> texts;
		std::vector<button_part> buttons;
		std::vector<check_part> checks;
		std::vector<field_part> fields;
		// A list a search fills - Copy to folder's folders, the people naming a face offers - each row by its text.
		struct row_part
		{
			std::shared_ptr<ui::results_list> list;
			ui::auto_complete_match_ptr match;
			std::string text;
			bool selected = false;
		};
		std::vector<row_part> rows;
		// What was read last on the way to the next field.
		std::string read_before;
	};

	std::string date_text(const core::date_t date)
	{
		return date.is_valid() ? date.to_xmp_date() : std::string{};
	}

	std::optional<core::date_t> parse_date(const std::string_view text)
	{
		core::date_t date;
		if (date.parse(core::trim(text)) && date.is_valid()) return date;
		return std::nullopt;
	}

	// What a field is called: its own label, or else what is read just before it - the heading above it, or the check
	// box it belongs to - where that is short enough to be one. A heading that goes on to explain is called by its
	// first sentence.
	std::string field_label(const std::string& own, const std::string& before)
	{
		constexpr size_t longest_label = 80;
		if (!own.empty()) return own;

		auto label = std::string(core::trim(before));
		if (const auto stop = label.find(". "); stop != std::string::npos) label.resize(stop);
		while (!label.empty() && (label.back() == ':' || label.back() == ' ' || label.back() == '.')) label.pop_back();
		return label.size() <= longest_label ? label : std::string{};
	}

	void collect(const view_element_ptr& e, const std::string& label, const bool enabled, dialog_parts& parts)
	{
		if (!e || !e->is_visible()) return;

		const auto here_enabled = enabled && e->is_enabled();
		auto here_label = label;

		if (const auto* const title = dynamic_cast<const ui::title_control*>(e.get()))
		{
			if (!title->text().empty())
			{
				parts.titles.push_back(title->text());
				parts.read_before = title->text();
			}
		}
		else if (const auto* const title2 = dynamic_cast<const ui::title_control2*>(e.get()))
		{
			if (!title2->title().empty())
			{
				parts.titles.push_back(title2->title());
				parts.read_before = title2->title();
			}

			if (!title2->details().empty()) parts.texts.push_back(title2->details());
		}
		else if (const auto* const busy = dynamic_cast<const ui::busy_control*>(e.get()))
		{
			if (!busy->text().empty()) parts.texts.push_back(busy->text());
		}
		else if (const auto* const paragraph = dynamic_cast<const ui::text_element_base*>(e.get()))
		{
			if (!paragraph->text().empty())
			{
				parts.texts.push_back(paragraph->text());
				parts.read_before = paragraph->text();
			}
		}
		else if (const auto* const labelled = dynamic_cast<const ui::labelled_element*>(e.get()))
		{
			if (!labelled->label().empty()) here_label = plain_label(labelled->label());
		}
		else if (const auto* const row = dynamic_cast<const ui::labelled_row*>(e.get()))
		{
			if (!row->row_label().empty()) here_label = plain_label(row->row_label());
		}
		else if (const auto button = std::dynamic_pointer_cast<ui::button>(e))
		{
			const auto usable = here_enabled && button->enabled;
			auto text = plain_label(button->window_text());

			if (button->traits.push_button)
			{
				parts.buttons.push_back({button, std::move(text), button->traits.default_button, usable});
			}
			else
			{
				parts.read_before = text;
				parts.checks.push_back(
					{button, std::move(text), button->is_checked(), button->traits.radio_group >= 0, usable});
			}
		}
		else if (const auto edit = std::dynamic_pointer_cast<ui::edit>(e))
		{
			parts.fields.push_back({edit, nullptr, field_label(here_label, parts.read_before), edit->window_text(),
			                        here_enabled && edit->enabled});
			parts.read_before.clear();
		}
		else if (const auto date = std::dynamic_pointer_cast<ui::date_time_control>(e))
		{
			parts.fields.push_back({nullptr, date, field_label(here_label, parts.read_before), date_text(date->date()),
			                        here_enabled && date->enabled});
			parts.read_before.clear();
		}
		else if (const auto list = std::dynamic_pointer_cast<ui::results_list>(e))
		{
			for (const auto& r : list->_results)
			{
				if (!r) continue;
				const auto selected = list->_completes && list->_completes->selected() == r;
				parts.rows.push_back({list, r, r->edit_text(), selected});
			}
		}

		e->visit_children([&](const view_element_ptr& child) { collect(child, here_label, here_enabled, parts); });
	}

	// Oldest first: the last is the one in front.
	std::vector<dialog_parts> open_dialog_parts()
	{
		std::vector<dialog_parts> result;

		for (const auto& weak : open_dialogs())
		{
			const auto d = weak.lock();
			if (!d || !d->frame()->is_visible()) continue;

			dialog_parts parts;
			parts.owner = d;
			for (const auto& c : d->_controls) collect(c, {}, true, parts);
			result.push_back(std::move(parts));
		}

		return result;
	}

	// Diffractor's dialogs, then what the system has put up in front of them all - a shell confirmation or error, a
	// message box - read as a screen reader reads it, since nobody is there to answer it but the agent.
	std::vector<dialog_parts> dialogs_and_prompts()
	{
		auto result = open_dialog_parts();

		for (auto& prompt : platform::system_prompts())
		{
			dialog_parts parts;
			parts.system_prompt = prompt.id;
			parts.titles.push_back(std::move(prompt.title));
			parts.texts = std::move(prompt.texts);
			for (auto& b : prompt.buttons) parts.buttons.push_back({nullptr, std::move(b), false, true});
			result.push_back(std::move(parts));
		}

		return result;
	}

	std::shared_ptr<dialog> front_dialog()
	{
		const auto& dialogs = open_dialogs();

		for (auto i = dialogs.rbegin(); i != dialogs.rend(); ++i)
		{
			if (auto d = i->lock(); d && d->frame()->is_visible()) return d;
		}

		return nullptr;
	}

	// The check boxes and fields of a dialog or a task's panel, into an open object.
	void write_checks_and_fields(json_writer& json, const dialog_parts& parts)
	{
		json.key("checks").array();

		for (const auto& c : parts.checks)
		{
			json.object().text("text", c.text).boolean("checked", c.checked).boolean("radio", c.radio);
			json.boolean("enabled", c.enabled).end_object();
		}

		json.end_array();
		json.key("fields").array();

		for (const auto& f : parts.fields)
		{
			json.object().text("label", f.label).text("text", f.text);
			if (f.date) json.text("kind", "date");
			json.boolean("enabled", f.enabled).end_object();
		}

		json.end_array();
	}

	std::string checks_and_fields_text(const dialog_parts& parts)
	{
		std::string text;

		for (const auto& f : parts.fields)
		{
			text += std::format("\n  {}field {}: '{}'{}", f.date ? "date "s : ""s,
			                    f.label.empty() ? "(unlabelled)"s : f.label, f.text, f.enabled ? "" : " (disabled)");
		}

		for (const auto& c : parts.checks)
		{
			const auto* const mark = c.radio ? (c.checked ? "(o)" : "( )") : (c.checked ? "[x]" : "[ ]");
			text += std::format("\n  {} {}{}", mark, c.text, c.enabled ? "" : " (disabled)");
		}

		return text;
	}

	void write_dialog(json_writer& json, const dialog_parts& parts)
	{
		json.object();
		json.text("title", parts.titles.empty() ? std::string{} : parts.titles.front());
		if (parts.system_prompt != 0) json.boolean("system", true);
		json.key("text").array();
		for (size_t i = 1; i < parts.titles.size(); ++i) json.text(parts.titles[i]);
		for (const auto& t : parts.texts) json.text(t);
		json.end_array();
		json.key("buttons").array();

		for (const auto& b : parts.buttons)
		{
			json.object().text("text", b.text).boolean("default", b.is_default).boolean("enabled", b.enabled);
			json.end_object();
		}

		json.end_array();
		write_checks_and_fields(json, parts);
		if (!parts.rows.empty())
		{
			json.key("rows").array();
			for (const auto& r : parts.rows)
				json.object().text("text", r.text).boolean("selected", r.selected).end_object();
			json.end_array();
		}
		json.end_object();
	}

	void write_dialogs(json_writer& json, const std::vector<dialog_parts>& dialogs)
	{
		json.key("dialogs").array();
		for (const auto& d : dialogs) write_dialog(json, d);
		json.end_array();
	}

	std::string dialogs_text(const std::vector<dialog_parts>& dialogs)
	{
		std::string text;

		for (const auto& d : dialogs)
		{
			text += std::format("\n{}: {}", d.system_prompt != 0 ? "Windows' dialog"sv : "Dialog"sv,
			                    d.titles.empty() ? "(untitled)"s : d.titles.front());
			for (size_t i = 1; i < d.titles.size(); ++i) text += "\n  " + d.titles[i];
			for (const auto& t : d.texts) text += "\n  " + t;
			text += checks_and_fields_text(d);

			if (!d.rows.empty())
			{
				text += "\n  rows:";
				for (const auto& r : d.rows) text += std::format(" [{}]{}", r.text, r.selected ? " (chosen)" : "");
			}

			if (!d.buttons.empty())
			{
				text += "\n  buttons:";
				for (const auto& b : d.buttons) text += std::format(" [{}]{}", b.text, b.enabled ? "" : " (disabled)");
			}
		}

		return text;
	}

	// The fields and check boxes an answer names, found and checked before anything is touched.
	struct form_answer
	{
		std::vector<std::pair<const dialog_parts::field_part*, std::string>> fills;
		std::vector<std::pair<const dialog_parts::check_part*, bool>> ticks;
	};

	// The object an argument holds, where it names anything.
	const rapidjson::Value* named_values(const rapidjson::Value& arguments, const char* const name)
	{
		if (!arguments.HasMember(name)) return nullptr;
		const auto& value = arguments[name];
		return value.IsObject() && value.MemberCount() > 0 ? &value : nullptr;
	}

	// How ready a field an answer names must be: there and taking input, as one about to be typed into; there, enabled
	// or not, where a check box set by the same answer can still enable it; or not there yet, where such a check box
	// can still show it.
	enum class field_need
	{
		enabled,
		present,
		may_appear,
	};

	void find_fields(const dialog_parts& parts, const rapidjson::Value& fields, const field_need need,
	                 form_answer& answer, std::vector<std::string>& problems)
	{
		for (const auto& m : fields.GetObject())
		{
			const std::string_view label(m.name.GetString(), m.name.GetStringLength());
			std::string value(m.value.GetString(), m.value.GetStringLength());
			const auto found = std::ranges::find_if(parts.fields,
			                                        [label](const auto& f) { return core::icmp(f.label, label) == 0; });

			if (found == parts.fields.end())
			{
				if (need != field_need::may_appear) problems.push_back(std::format("there is no field '{}'", label));
			}
			else if (need == field_need::enabled && !found->enabled)
				problems.push_back(std::format("the field '{}' is disabled", label));
			else if (found->date && !parse_date(value))
			{
				problems.push_back(
					std::format("'{}' holds a date, given as yyyy-mm-dd or yyyy-mm-dd hh:mm:ss, not '{}'", label,
				                value));
			}
			else answer.fills.emplace_back(&*found, std::move(value));
		}
	}

	void find_checks(const dialog_parts& parts, const rapidjson::Value& checks, form_answer& answer,
	                 std::vector<std::string>& problems)
	{
		for (const auto& m : checks.GetObject())
		{
			const std::string_view text(m.name.GetString(), m.name.GetStringLength());
			const auto found = std::ranges::find_if(parts.checks,
			                                        [text](const auto& c) { return core::icmp(c.text, text) == 0; });

			if (found == parts.checks.end()) problems.push_back(std::format("there is no check box '{}'", text));
			else if (!found->enabled) problems.push_back(std::format("'{}' is disabled", text));
			else if (found->radio && !m.value.GetBool())
			{
				problems.push_back(std::format("'{}' is a radio button: choose another to turn it off", text));
			}
			else answer.ticks.emplace_back(&*found, m.value.GetBool());
		}
	}

	void tick_checks(const form_answer& answer)
	{
		for (const auto& [check, value] : answer.ticks)
		{
			if (check->checked != value) check->control->click();
		}
	}

	// As typing does, or choosing a date: the field's owner hears of each change.
	void fill_fields(const form_answer& answer)
	{
		for (const auto& [field, value] : answer.fills)
		{
			if (field->date)
			{
				field->date->date(*parse_date(value));
				continue;
			}

			field->edit->focus();
			field->edit->select_all();
			field->edit->on_key(platform::keys::DEL, {});
			field->edit->on_text(value);
		}
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// What the window shows
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// A view with a panel of its own beside it, which task_state reads and task_set fills in.
	bool is_task_view(const view_type v)
	{
		switch (v)
		{
		case view_type::edit:
		case view_type::movie:
		case view_type::rename:
		case view_type::batch:
		case view_type::import:
		case view_type::sync:
		case view_type::locate:
		case view_type::tags:
		case view_type::options: return true;
		default: return false;
		}
	}

	std::vector<std::string> shown_paths(const app_frame& app, const size_t most)
	{
		std::vector<std::string> result;

		for (const auto& group : app._state.groups())
		{
			for (const auto& item : group->items())
			{
				if (result.size() >= most) return result;
				result.push_back(item->path().str());
			}
		}

		return result;
	}

	// The view, the search and the items shown, naming the first `listed` of them and of the selection, into an open
	// object.
	void write_view(json_writer& json, const app_frame& app, const size_t listed)
	{
		const auto& state = app._state;
		json.text("view", agent_view_name(state.view_mode())).boolean("full_screen", state.is_full_screen);
		json.boolean("task", is_task_view(state.view_mode()));
		json.text("search", state.search().text()).boolean("opening", state.is_opening());
		json.integer("found", static_cast<int64_t>(state.search_items().size()));
		json.integer("shown", static_cast<int64_t>(state.display_items().size()));
		json.integer("groups", static_cast<int64_t>(state.groups().size()));
		json.integer("selected", static_cast<int64_t>(state.selected_count()));
		json.key("items").array();
		for (const auto& path : shown_paths(app, listed)) json.text(path);
		json.end_array();
		json.key("selection").array();
		size_t named = 0;

		for (const auto& item : state.selected_items().items())
		{
			if (named++ >= listed) break;
			json.text(item->path().str());
		}

		json.end_array();

		// The media shown: where it is and whether it plays, and docs/faces.md#8-searching: the parts of a video the faces
		// the search found it by are seen in, which its scrubber marks.
		const auto& display = state._display;
		if (display && display->is_one() && display->_item1 && display->_item1->file_type()->has_trait(file_traits::av))
		{
			json.key("media").object().number("position", std::round(display->media_pos() * 1000.0) / 1000.0);
			json.boolean("playing", display->is_playing_media()).key("found").array();
			for (const auto& s : df::face_spans_found(state.search(), display->_item1->path()))
				json.array().number(s.start / 1000.0).number(s.end / 1000.0).end_array();
			json.end_array().end_object();
		}
	}

	std::string view_text(const app_frame& app, const size_t listed)
	{
		const auto& state = app._state;
		auto text = std::format("{} view{}, '{}': {} items shown, {} selected{}", agent_view_name(state.view_mode()),
		                        state.is_full_screen ? " (full screen)" : "", state.search().text(),
		                        state.display_items().size(), state.selected_count(),
		                        state.is_opening() ? " - still loading" : "");
		if (is_task_view(state.view_mode())) text += " - a task: task_state reads its panel and review";
		for (const auto& path : shown_paths(app, listed)) text += "\n  " + path;
		return text;
	}

	void add_ui_state(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "ui_state";
		t.title = "What the window shows";
		t.description = "What the window shows now: the view, the folder or search, how many items it finds and "
						"shows, the first of them and of the selection, the media shown - where it is, whether it "
						"plays, and the parts of a video where faces the search found it by are seen - whether it is "
						"busy, and any dialog in front - its text, buttons, check boxes and fields, for dialog_respond "
						"- menu open - its rows, for menu_respond - recording or steps being acted out.";
		t.input_schema = std::format(R"({{"type":"object","properties":{{{}}},"additionalProperties":false}})",
		                             list_property);
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto listed = static_cast<size_t>(call->number("list").value_or(20));
			const auto dialogs = dialogs_and_prompts();
			const auto busy = agent_window_busy(app);
			const auto bounds = app._app_frame->window_bounds();

			json_writer json;
			json.object();
			write_view(json, app, listed);
			json.key("window").object().integer("width", bounds.width()).integer("height", bounds.height());
			json.boolean("maximized", app._app_frame->is_maximized()).end_object();
			json.key("busy").array();
			for (const auto& b : busy) json.text(b);
			json.end_array();
			json.boolean("menu_open", ui::menu_active > 0);
			json.key("menu").raw(describe_menu_json());
			write_dialogs(json, dialogs);
			json.boolean("acting", app._tutorial != nullptr);
			if (app._recording)
				json.key("recording").object().number("seconds", app._recording->elapsed()).end_object();
			json.end_object();

			auto text = view_text(app, listed);
			if (!busy.empty()) text += "\nBusy: " + join_text(busy, ", ");
			if (ui::menu_active > 0)
			{
				const auto menu = describe_menu_text();
				text += menu.empty() ? "\nA menu is open."s : menu;
			}
			text += dialogs_text(dialogs);
			call->finish(structured_result(std::move(text), json.str()));
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Commands
	///////////////////////////////////////////////////////////////////////////////////////////////////

	enum class command_need
	{
		read,
		write,
		network,
		launch,
		refused,
	};

	struct command_rule
	{
		command_need need = command_need::read;
		std::string_view why;
	};

	// What a command needs of the session beyond reading: one that changes files, the clipboard or the system needs
	// write; one that opens another program, a folder or a web page on the user's desktop needs launch; one that ends
	// the session is not run. One that opens a menu needs nothing: what is chosen from it needs what its own command
	// needs.
	command_rule rule_of(const commands id)
	{
		constexpr std::string_view changes = "changes files, the clipboard or the system";
		constexpr std::string_view opens = "opens another program, a folder or a web page on the user's desktop";

		switch (id)
		{
		case commands::exit: return {command_need::refused, "ends the session: close the session's input instead"};
		case commands::tool_scan: return {command_need::refused, "works a scanner through the system's own windows"};
#ifndef WINSTORE
		case commands::info_new_version: return {command_need::refused, "installs another version of Diffractor"};
		case commands::info_check_for_updates:
			return {command_need::network, "asks diffractor.com for a newer version"};
#endif

		case commands::browse_open_containingfolder:
		case commands::browse_open_googlemap:
		case commands::browse_open_in_file_browser:
		case commands::options_help:
		case commands::tool_open_with:
		case commands::view_help: return {command_need::launch, opens};

		case commands::edit_copy:
		case commands::edit_copy_item_path:
		case commands::edit_cut:
		case commands::edit_item_save:
		case commands::edit_item_save_and_next:
		case commands::edit_item_save_and_prev:
		case commands::edit_item_save_as:
		case commands::edit_paste:
		case commands::import_run:
		case commands::label_approved:
		case commands::label_none:
		case commands::label_review:
		case commands::label_second:
		case commands::label_select:
		case commands::label_to_do:
		case commands::locate_run:
		case commands::print:
		case commands::rate_1:
		case commands::rate_2:
		case commands::rate_3:
		case commands::rate_4:
		case commands::rate_5:
		case commands::rate_none:
		case commands::rate_rejected:
		case commands::sync_run:
		case commands::tags_run:
		case commands::tool_burn:
		case commands::tool_copy_to_folder:
		case commands::tool_delete:
		case commands::tool_desktop_background:
		case commands::tool_edit_description:
		case commands::tool_eject:
		case commands::tool_file_properties:
		case commands::tool_move_to_folder:
		case commands::tool_movie_render:
		case commands::tool_movie_save:
		case commands::tool_new_folder:
		case commands::tool_rotate_anticlockwise:
		case commands::tool_rotate_clockwise:
		case commands::tool_rotate_reset:
		case commands::tool_run:
		case commands::tool_save_current_video_frame:
		case commands::tool_share: return {command_need::write, changes};

		// docs/faces.md#6-the-face-book: what the user tells Diffractor about faces is theirs, as their files are.
		case commands::face_confirm:
		case commands::face_hide:
		case commands::face_import:
		case commands::face_name:
		case commands::face_name_picture:
		case commands::face_reject:
		case commands::face_rename_person:
		case commands::face_undo_placement:
		case commands::face_show_again: return {command_need::write, "changes the user's face book"};
		case commands::face_export:
		case commands::face_save_names: return {command_need::write, changes};

		default: return {};
		}
	}

	std::string_view need_name(const command_need need)
	{
		switch (need)
		{
		case command_need::write: return "write";
		case command_need::network: return "network";
		case command_need::launch: return "launch";
		case command_need::refused: return "refused";
		case command_need::read: break;
		}

		return {};
	}

	// Why the session does not run the command; empty where it does.
	std::string command_refusal(const mcp_host& host, const commands id)
	{
		const auto rule = rule_of(id);
		const auto& allow = host.options().allow;
		const auto name = command_name(id);

		if (rule.need == command_need::refused) return std::format("{} is not run: it {}", name, rule.why);
		if (rule.need == command_need::write && !allow.write)
		{
			return std::format("{} {}, which needs -mcp-allow:write", name, rule.why);
		}
		if (rule.need == command_need::network && !allow.network)
		{
			return std::format("{} {}, which needs -mcp-allow:network", name, rule.why);
		}
		if (rule.need == command_need::launch && !allow.launch)
		{
			return std::format("{} {}, which needs -mcp-allow:launch", name, rule.why);
		}

		return {};
	}

	void add_command_list(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "command_list";
		t.title = "List the commands";
		t.description = "Every command the window offers - what its menus, toolbars and keys run - by the name "
						"command_run and ui_input's command step take: what each is called, whether it can run now and "
						"if not why, whether it is checked, its keys, and what it needs of the session beyond reading. "
						"contains narrows the list to names or text with those letters.";
		t.input_schema =
			R"({"type":"object","properties":{"contains":{"type":"string"},"enabled_only":{"type":"boolean"}},"additionalProperties":false})";
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto contains = call->text("contains");
			const auto enabled_only = call->flag("enabled_only", false);
			app.update_button_state(false);

			json_writer json;
			json.object().key("commands").array();
			std::string text;
			auto count = 0;

			for (const auto id : all_commands())
			{
				const auto command = app.find_command(id);
				if (!command) continue;

				const auto name = command_name(id);
				const auto label = plain_label(command->text);

				if (!contains.empty() && core::ifind(name, contains) == std::string_view::npos &&
				    core::ifind(label, contains) == std::string_view::npos)
				{
					continue;
				}

				if (enabled_only && !command->enable) continue;

				const auto rule = rule_of(id);
				json.object().text("name", name).text("text", label);
				if (!command->tooltip_text.empty()) json.text("tooltip", command->tooltip_text);
				json.boolean("enabled", command->enable);
				if (!command->enable && !command->disabled_reason.empty())
				{
					json.text("why_disabled", command->disabled_reason);
				}
				if (command->checkable || command->checked) json.boolean("checked", command->checked);
				if (!command->keyboard_accelerator_text.empty()) json.text("keys", command->keyboard_accelerator_text);
				if (rule.need != command_need::read) json.text("needs", need_name(rule.need));
				json.end_object();

				text += std::format("\n{} - {}{}{}", name, label, command->enable ? "" : " (disabled)",
				                    rule.need == command_need::read ? ""s : std::format(" [{}]", need_name(rule.need)));
				++count;
			}

			json.end_array().integer("count", count).end_object();
			call->finish(structured_result(std::format("{} commands:{}", count, text), json.str()));
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	void add_command_run(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "command_run";
		t.title = "Run a command";
		t.description = "Runs a command as its menu item, button or key would, by the name command_list gives - "
						"view_fullscreen, select_all, tool_rename. One that cannot run now says why. Commands that "
						"change files, the clipboard or the system need -mcp-allow:write. One that opens a menu - "
						"menu_main and the like - opens it as a press on its button does, and answers with the menu, "
						"which menu_respond chooses from; one that opens a dialog answers with the dialog, which "
						"dialog_respond answers.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"command":{{"type":"string"}},{}}},"required":["command"],"additionalProperties":false}})",
			wait_property);
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto name = call->text("command");
			const auto id = command_from_name(name);

			if (!id)
			{
				call->finish(core::mcp::tool_result::error(
					std::format("There is no command {}: command_list names them", name)));
				return;
			}

			if (const auto why = command_refusal(host, *id); !why.empty())
			{
				call->finish(core::mcp::tool_result::error(why));
				return;
			}

			if (refused_while_modal(host, call)) return;

			app.update_button_state(false);
			const auto command = app.find_command(*id);

			if (!command || !command->enable)
			{
				const auto why = command && !command->disabled_reason.empty() ? command->disabled_reason
				                                                              : "it is not offered here"s;
				auto message = std::format("{} cannot run now: {}", name, why);

				// A task's command waits on the index the window loads; refused meanwhile, its reason can mislead.
				if (const auto busy = agent_window_busy(app); !busy.empty())
				{
					message += std::format(". Diffractor is still busy - {} - which may be why: wait_until_idle, then "
					                       "try again",
					                       join_text(busy, ", "));
				}

				call->finish(core::mcp::tool_result::error(std::move(message)));
				return;
			}

			const auto job = host.start_job("command_run");

			// A task of its own: a command that opens a dialog or a menu runs its loop in it, and this call is answered
			// with the dialog or menu meanwhile. What the command did is read once the window has caught up with it.
			platform::run_ui([&host, &app, job, id = *id, name, command]
			{
				if (!command->invoke && command->menu) open_command_menu(app, command);
				else app.invoke(id);
				host.when([&app] { return window_caught_up(app); }, caught_up_quiet_s, caught_up_timeout_s,
				          [&host, &app, job, name](bool)
				{
					// Work the command left a dialog showing - an edit being saved - is part of it, done once that has
					// gone.
					host.when([] { return dialogs_showing_work().empty(); }, 0.0, mcp_host::longest_wait_seconds,
					          [&app, job, name](bool)
					{
						json_writer json;
						json.object().text("command", name);
						write_view(json, app, 10);
						json.end_object();
						job->complete(
							structured_result(std::format("Ran {}. {}", name, view_text(app, 0)), json.str()));
					});
				});
			});

			host.wait(call, job, mcp_host::wait_seconds(*call), true);
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Where the window is, and what is selected
	///////////////////////////////////////////////////////////////////////////////////////////////////

	void add_navigate(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "navigate";
		t.title = "Open a folder or a search";
		t.description = "Shows a folder or a search in the window, as typing it in the address box does - a folder's "
						"full path, one ending in ** for its subfolders too, or a search (help with topic search) - "
						"and answers once it has loaded, with how many items it shows and the first of them.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"to":{{"type":"string","description":"A folder's full path, or a search"}},{},{}}},"required":["to"],"additionalProperties":false}})",
			list_property, wait_property);
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto to = call->text("to");
			const auto listed = static_cast<size_t>(call->number("list").value_or(20));

			if (core::trim(to).empty())
			{
				call->finish(core::mcp::tool_result::error("Say where: a folder's full path, or a search"));
				return;
			}

			if (refused_while_modal(host, call)) return;

			app._state.open(app._view_frame, std::string_view(to));

			answer_when(host, call, "navigate",
			            [&app] { return !app._state.is_opening() && app._item_index.searching == 0; }, 0.3,
			            mcp_host::wait_seconds(*call), true, [&app, listed](const bool settled)
			{
				json_writer json;
				json.object();
				write_view(json, app, listed);
				json.boolean("settled", settled).end_object();
				return structured_result(view_text(app, listed), json.str());
			});
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	void add_select(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "select";
		t.title = "Select items";
		t.description = "Selects items the window shows, by file name or full path - in place of the selection, or "
						"added to it - or all of them, none, or the inverse. What is selected is what commands act on.";
		t.input_schema =
			R"({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"},"description":"File names, as the window shows them, or full paths, which tell apart files of one name in different folders"},"add":{"type":"boolean","description":"Add to the selection rather than replace it"},"all":{"type":"boolean"},"none":{"type":"boolean"},"invert":{"type":"boolean"}},"additionalProperties":false})";
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			auto& state = app._state;
			const auto& view = app._view_frame;
			std::vector<std::string> missing;

			if (refused_while_modal(host, call)) return;

			if (call->flag("all", false))
			{
				state.select_all(view);
			}
			else if (call->flag("none", false))
			{
				state.select_nothing(view);
			}
			else if (call->flag("invert", false))
			{
				state.select_inverse(view);
			}
			else
			{
				const auto names = call->texts("names");

				if (names.empty())
				{
					call->finish(
						core::mcp::tool_result::error("Name the items to select, or ask for all, none or invert"));
					return;
				}

				auto replace = !call->flag("add", false);
				std::set<std::string> seen;

				for (const auto& name : names)
				{
					if (!seen.insert(name).second) continue;

					// A full path names one item where a name alone can name several in a search across folders.
					const auto item = core::is_path(name) ? state.find_displayed_item_by_path(core::file_path(name))
					                                      : state.find_displayed_item_by_name(name);

					if (!item)
					{
						missing.push_back(name);
					}
					// Asked of the item, not of selected_items(), which catches up only as the view next updates:
					// selecting the first name has just deselected the rest the list still holds, and each after
					// the first is added by toggling it.
					else if (replace || !item->is_selected())
					{
						state.select(view, item, !replace, false, false);
						replace = false;
					}
				}
			}

			answer_when(host, call, "select", [&app] { return window_caught_up(app); }, caught_up_quiet_s,
			            caught_up_timeout_s, false, [&app, missing](bool)
			{
				const auto& state = app._state;
				json_writer json;
				json.object();
				write_view(json, app, 50);
				json.key("not_shown").array();
				for (const auto& name : missing) json.text(name);
				json.end_array().end_object();

				auto text = std::format("{} selected", state.selected_count());
				if (!missing.empty()) text += std::format("; not shown in the window: {}", join_text(missing, ", "));
				return structured_result(std::move(text), json.str(), state.selected_count() == 0 && !missing.empty());
			});
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Input, tutorials and dialogs
	///////////////////////////////////////////////////////////////////////////////////////////////////

	core::mcp::tool_result player_answer(const app_frame& app, const tutorial_outcome& outcome, const agent_job& job)
	{
		json_writer json;
		json.object().boolean("ok", outcome.ok).text("summary", outcome.summary).integer("missed", outcome.missed);
		if (!outcome.movie.is_empty()) json.text("movie", outcome.movie.str());
		if (!outcome.markers.is_empty()) json.text("markers", outcome.markers.str());
		if (outcome.film_seconds > 0.0) json.number("film_seconds", outcome.film_seconds);
		json.key("beats").array();
		for (const auto& [id, at] : outcome.beats) json.object().text("id", id).number("at_s", at).end_object();
		json.end_array();
		write_view(json, app, 0);
		json.end_object();

		auto text = std::format("{}\n{}", outcome.summary, view_text(app, 0));
		if (const auto said = job.all_output(); !said.empty()) text += "\n\n" + said;
		return structured_result(std::move(text), json.str(), !outcome.ok);
	}

	// Acts the steps out as the call's job, a step at a time from the frame tick. Steps alone cannot see past a dialog,
	// so a call acting them out is answered once one shows; a tutorial's player waits for a dialog to close itself,
	// and says so.
	void play(mcp_host& host, const core::mcp::call_ptr& call, const std::string_view tool, tutorial_options options)
	{
		auto& app = host.app();
		const auto job = host.start_job(tool);
		const auto interruptible = options.steps_only;
		// A step reaches the commands command_run does, and is held to the same tiers.
		options.refusal = [&host](const commands id)
		{
			return agent_command_refusal(host, id);
		};
		options.said = [job](const std::string_view line)
		{
			job->output(line);
			// As progress too, which a client that asked for it hears as each beat starts.
			job->progress(-1.0, line);
		};
		options.finished = [&app, job](const tutorial_outcome& outcome)
		{
			job->complete(player_answer(app, outcome, *job));
		};
		app._tutorial = std::make_unique<tutorial_player>(app, std::move(options));
		job->on_cancel([&app]
		{
			if (app._tutorial) app._tutorial->cancel();
		});
		host.wait(call, job, mcp_host::wait_seconds(*call), interruptible);
	}

	bool refused_while_acting(const app_frame& app, const core::mcp::call_ptr& call)
	{
		if (!app._tutorial) return false;
		call->finish(
			core::mcp::tool_result::error("Steps are being acted out already: wait for their job, or job_cancel it"));
		return true;
	}

	void add_ui_input(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "ui_input";
		t.title = "Act out input";
		t.description =
			"Acts out steps in the window as a user would - pointing, clicking, dragging, the wheel, typing, keys "
			"and commands - one step a line, in the language of a tutorial script's scene (help with topic dft): "
			"point tile 'IMG_1.jpg', double-click, right-click, drag at 0.5 0.5, wheel down 3, key escape, type "
			"'beach', text 'a caption', command view_fullscreen. The pointer and keys are given to the window as "
			"its own input, so they reach what a user's would - a control, a toolbar's button, a link, the view - "
			"and the drawn pointer shows where, in a recording too; the user's own pointer and keyboard are not "
			"used. Each call starts with the pointer hidden, so a click follows a point in the same call. Answers "
			"once the steps are done, naming any that found nothing to act on; a dialog or menu that opens ends "
			"them, and ui_state shows it.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"steps":{{"type":"string","description":"One step a line"}},{}}},"required":["steps"],"additionalProperties":false}})",
			wait_property);
		t.annotations.destructive = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			if (refused_while_modal(host, call) || refused_while_acting(host.app(), call)) return;

			tutorial_options options;
			options.text = call->text("steps");
			options.steps_only = true;
			play(host, call, "ui_input", std::move(options));
		};
		host.add(std::move(t), tool_tier::write, true);
	}

	void add_tutorial_run(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "tutorial_run";
		t.title = "Play a tutorial script";
		t.description = "Plays a tutorial script (.dft) in the window - its scene, then each beat acted out while its "
						"narration would be spoken, timed by the narration's .timing files where they are - given as "
						"the script's full path, or its text. With record, films the window to that MP4 and writes "
						"where each beat starts beside it (<movie>.markers.txt), for the narration to be laid under; "
						"without, it rehearses. Recording needs the window on the desktop (-mcp-ui:visible). help with "
						"topic dft has the language.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"script":{{"type":"string","description":"The .dft script's full path"}},"text":{{"type":"string","description":"The script itself, in place of a file"}},"record":{{"type":"string","description":"The full path of the MP4 to film it to"}},{}}},"additionalProperties":false}})",
			wait_property);
		t.annotations.destructive = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto script = call->text("script");
			const auto text = call->text("text");
			const auto record = call->text("record");

			const auto refuse = [&call](std::string why)
			{
				call->finish(core::mcp::tool_result::error(std::move(why)));
			};

			if (!host.options().allow.write)
			{
				refuse("tutorial_run acts on the window as a user would, which can change files: it needs "
				       "-mcp-allow:write as well as capture");
				return;
			}

			if (script.empty() == text.empty()) return refuse("Give the script's full path, or its text: one of them");
			if (!script.empty() && !core::is_path(script))
				return refuse(std::format("Give the script's full path, not {}", script));

			if (!record.empty())
			{
				if (!core::is_path(record)) return refuse(std::format("Give the movie's full path, not {}", record));

				if (host.options().presence != agent_presence::visible)
				{
					return refuse("A recording films the window as the screen shows it, so the window must be on the "
					              "desktop: start the session with -mcp-ui:visible, or leave out record to rehearse");
				}
			}

			if (app._recording) return refuse("The window is being recorded already: record_stop ends that first");
			if (refused_while_modal(host, call) || refused_while_acting(app, call)) return;

			tutorial_options options;
			if (!script.empty()) options.script = core::file_path(script);
			options.text = text;
			if (!record.empty()) options.record = core::file_path(record);
			play(host, call, "tutorial_run", std::move(options));
		};
		host.add(std::move(t), tool_tier::capture, true);
	}

	void add_dialog_respond(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "dialog_respond";
		t.title = "Answer a dialog";
		t.description =
			"Answers the dialog in front, as ui_state shows it: fills fields by their label - a date as "
			"yyyy-mm-dd hh:mm:ss - and sets check boxes and radio buttons by their text, then presses a "
			"button by its text - or cancels the dialog, as Escape does. choose picks a row of the list a "
			"search fills - Copy to folder's folders, the people naming a face offers - by its text, as a "
			"click picks it, and then presses the button named, or else the default one. One Windows puts up "
			"itself, marked system, is answered by its buttons or cancelled. A session that may not write can "
			"only cancel: the rest can change files. Answers once the dialog has closed or another has "
			"opened, with what is in front then.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"button":{{"type":"string","description":"The text of the button to press"}},"choose":{{"type":"string","description":"The text of a row of the dialog's list"}},"cancel":{{"type":"boolean"}},"checks":{{"type":"object","additionalProperties":{{"type":"boolean"}},"description":"Check boxes and radio buttons, by their text"}},"fields":{{"type":"object","additionalProperties":{{"type":"string"}},"description":"Fields, by their label"}},{}}},"additionalProperties":false}})",
			wait_property);
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			auto dialogs = dialogs_and_prompts();
			const auto refuse = [&call](std::string why)
			{
				call->finish(core::mcp::tool_result::error(std::move(why)));
			};

			if (dialogs.empty())
			{
				return refuse(ui::menu_active > 0 ? "A menu is open, not a dialog: menu_respond chooses from it"
				                                  : "No dialog is open");
			}

			auto& target = dialogs.back();
			const auto cancel = call->flag("cancel", false);
			const auto button = call->text("button");
			const auto choose = call->text("choose");
			const auto* const checks = named_values(call->arguments(), "checks");
			const auto* const fields = named_values(call->arguments(), "fields");

			if (cancel && (!button.empty() || !choose.empty()))
				return refuse("Press a button or choose a row, or cancel: not both");
			if (!cancel && button.empty() && choose.empty() && !checks && !fields)
			{
				return refuse("Say what to do: fields or checks to set, a row to choose, a button to press, or cancel");
			}

			if ((!button.empty() || !choose.empty() || checks || fields) && !host.options().allow.write)
			{
				return refuse("This session can only cancel a dialog: pressing a button, choosing a row, setting a "
				              "check box or filling a field can change files, which needs -mcp-allow:write");
			}

			// Windows' own: pressed by its button's text, or cancelled, through the platform; it has nothing else this
			// session can set. The answer waits for it to go, or for what it asks to have been answered.
			if (const auto prompt = target.system_prompt; prompt != 0)
			{
				if (checks || fields || !choose.empty())
				{
					return refuse("Nothing was changed: a dialog Windows shows is answered by its buttons alone." +
					              dialogs_text({target}));
				}

				if (!button.empty() && std::ranges::none_of(target.buttons, [&button](const auto& b)
				{ return core::icmp(b.text, button) == 0; }))
				{
					return refuse(
						std::format("Nothing was changed: there is no button '{}'.{}", button, dialogs_text({target})));
				}

				platform::run_ui([prompt, button] { platform::answer_system_prompt(prompt, button); });
				answer_when(host, call, "dialog_respond", [prompt] { return !platform::system_prompt_shown(prompt); },
				            0.2, mcp_host::wait_seconds(*call), false, [&app](const bool settled)
				{
					const auto now = dialogs_and_prompts();
					json_writer json;
					json.object().boolean("closed", settled);
					write_dialogs(json, now);
					write_view(json, app, 0);
					json.end_object();

					auto text = now.empty() ? "The dialog has closed. " + view_text(app, 0)
					                        : "In front now:" + dialogs_text(now);
					if (!settled) text = "The dialog is still open. " + text;
					return structured_result(std::move(text), json.str());
				});
				return;
			}

			// Everything is found before anything is touched, so a wrong name changes nothing.
			std::vector<std::string> problems;
			form_answer answer;
			const dialog_parts::button_part* press = nullptr;

			if (fields) find_fields(target, *fields, field_need::enabled, answer, problems);
			if (checks) find_checks(target, *checks, answer, problems);

			if (!button.empty())
			{
				const auto found = std::ranges::find_if(target.buttons, [&button](const auto& b)
				{ return core::icmp(b.text, button) == 0; });

				if (found == target.buttons.end()) problems.push_back(std::format("there is no button '{}'", button));
				else if (!found->enabled) problems.push_back(std::format("'{}' is disabled", button));
				else press = &*found;
			}

			// A row is chosen as a click chooses it; the dialog is then answered by its default button, where no other
			// is named.
			const dialog_parts::row_part* chosen = nullptr;
			if (!choose.empty())
			{
				const auto found = std::ranges::find_if(target.rows, [&choose](const auto& r)
				{ return core::icmp(r.text, choose) == 0; });

				if (found == target.rows.end()) problems.push_back(std::format("there is no row '{}'", choose));
				else chosen = &*found;

				if (chosen && button.empty())
				{
					const auto answer_button = std::ranges::find_if(target.buttons, [](const auto& b)
					{ return b.is_default && b.enabled; });
					if (answer_button != target.buttons.end()) press = &*answer_button;
				}
			}

			if (!problems.empty())
			{
				return refuse(
					std::format("Nothing was changed: {}.{}", join_text(problems, "; "), dialogs_text({target})));
			}

			fill_fields(answer);
			tick_checks(answer);

			if (!press && !cancel && !chosen)
			{
				const auto now = dialogs_and_prompts();
				json_writer json;
				json.object();
				write_dialogs(json, now);
				json.end_object();
				call->finish(structured_result("Set." + dialogs_text(now), json.str()));
				return;
			}

			const std::weak_ptr<dialog> answered = target.owner;

			// A task of its own: what a button does may open another dialog, whose loop runs there.
			if (chosen)
			{
				platform::run_ui(
					[list = chosen->list, match = chosen->match, answered, control = press ? press->control : nullptr]
				{
					list->selected(match, ui::complete_strategy_t::select_type::click);
					// A click on a row can answer the dialog itself, as Open with's does.
					if (const auto d = answered.lock(); control && d && front_dialog() == d) control->click();
				});
			}
			else if (press)
			{
				platform::run_ui([control = press->control] { control->click(); });
			}
			else
			{
				platform::run_ui([answered]
				{
					const auto d = answered.lock();
					if (!d) return;

					// A dialog that reports work under way stops the work as its Cancel button does.
					if (const auto stop = d->_close_cb) stop();
					else d->close(true);
				});
			}

			// Answered once the dialog has gone, or another is in front of it - Windows' own among them, as a delete's
			// confirmation can be.
			answer_when(host, call, "dialog_respond",
			            [answered]
			{
				const auto d = answered.lock();
				return !d || front_dialog() != d || platform::system_prompt_shown();
			}, 0.2, mcp_host::wait_seconds(*call), false, [&app](const bool settled)
			{
				const auto now = dialogs_and_prompts();
				json_writer json;
				json.object().boolean("closed", settled);
				write_dialogs(json, now);
				write_view(json, app, 0);
				json.end_object();

				auto text = now.empty() ? "The dialog has closed. " + view_text(app, 0)
				                        : "In front now:" + dialogs_text(now);
				if (!settled) text = "The dialog is still open. " + text;
				return structured_result(std::move(text), json.str());
			});
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Task views
	///////////////////////////////////////////////////////////////////////////////////////////////////

	constexpr std::string_view task_commands = "tool_sync, tool_import, tool_locate, tool_tag, tool_rename, "
											   "tool_convert, tool_edit_metadata, tool_adjust_date, tool_edit, "
											   "tool_movie or options";

	constexpr std::string_view rows_properties =
		R"("contains":{"type":"string","description":"Only the review's rows with a cell holding this"},"column":{"type":"string","description":"The column contains looks in, by its heading; every column by default"},"offset":{"type":"integer","minimum":0,"description":"How many rows to pass over first, to read on"},"limit":{"type":"integer","minimum":0,"maximum":1000,"description":"How many rows to name; 20 by default"})";

	dialog_parts panel_parts(const app_frame& app)
	{
		dialog_parts parts;

		if (const auto panel = app._view_frame ? app._view_frame->_panel : nullptr)
		{
			for (const auto& c : panel->_controls) collect(c, {}, true, parts);
		}

		return parts;
	}

	// Which of a review's rows to name: those with a cell holding `contains` - in `column` where it names one - a page
	// of them.
	struct rows_wanted
	{
		std::string contains;
		std::string column;
		size_t offset = 0;
		size_t limit = 20;
	};

	rows_wanted rows_wanted_of(const core::mcp::call& call)
	{
		return {call.text("contains"), call.text("column"), static_cast<size_t>(call.number("offset").value_or(0)),
		        static_cast<size_t>(call.number("limit").value_or(20))};
	}

	std::string join_cells(const std::vector<std::string_view>& cells)
	{
		std::string result;

		for (const auto cell : cells)
		{
			if (!result.empty()) result += " | ";
			result += cell;
		}

		return result;
	}

	// The review of a task that is reviewed before it runs - or, after a run, what it did to each row - into an open
	// object, and as text.
	void write_review(json_writer& json, std::string& text, review_list& list, const rows_wanted& wanted)
	{
		const auto titles = list.column_titles();
		std::vector<int> columns;

		// A column without a heading holds only an arrow between the two that have one.
		for (auto c = 0; c < list.column_count(); ++c)
		{
			if (!titles[c].sv().empty()) columns.push_back(c);
		}

		std::vector<std::string_view> headings;
		for (const auto c : columns) headings.push_back(titles[c].sv());

		// The columns contains looks in: the one named, or all of them.
		auto searched = columns;

		if (!wanted.column.empty())
		{
			std::erase_if(searched, [&](const int c) { return core::icmp(titles[c].sv(), wanted.column) != 0; });
		}

		std::vector<const review_list::row_element*> matching;

		for (const auto& row : list._rows)
		{
			const auto matches = wanted.contains.empty() ? !searched.empty()
			                                             : std::ranges::any_of(searched, [&](const int c)
			{ return core::ifind(row->_text[c], wanted.contains) != std::string_view::npos; });

			if (matches) matching.push_back(row.get());
		}

		const auto first = std::min(wanted.offset, matching.size());
		const auto last = std::min(matching.size(), first + wanted.limit);

		json.key("review").object().key("columns").array();
		for (const auto heading : headings) json.text(heading);
		json.end_array();
		json.boolean("results", list.showing_results()).integer("rows", static_cast<int64_t>(list._rows.size()));
		json.integer("matching", static_cast<int64_t>(matching.size())).integer("offset", static_cast<int64_t>(first));
		json.boolean("more", last < matching.size()).key("items").array();

		for (auto i = first; i < last; ++i)
		{
			json.array();
			for (const auto c : columns) json.text(matching[i]->_text[c]);
			json.end_array();
		}

		json.end_array().end_object();

		if (list._rows.empty())
		{
			const auto empty = list.empty_message();
			text += "\nReview: nothing yet";
			if (!empty.sv().empty()) text += std::format(" - {}", empty.sv());
			return;
		}

		text += std::format("\n{}: {} rows", list.showing_results() ? "Results" : "Review", list._rows.size());

		if (searched.empty())
		{
			text += std::format("; there is no column '{}' - its columns are {}", wanted.column, join_cells(headings));
		}
		else if (!wanted.contains.empty() || !wanted.column.empty())
		{
			text += std::format(", {} with '{}'{}", matching.size(), wanted.contains,
			                    wanted.column.empty() ? ""s : std::format(" in {}", wanted.column));
		}

		if (first < last) text += std::format("; {} to {} ({}):", first + 1, last, join_cells(headings));

		for (auto i = first; i < last; ++i)
		{
			std::vector<std::string_view> cells;
			for (const auto c : columns) cells.emplace_back(matching[i]->_text[c]);
			text += "\n  " + join_cells(cells);
		}

		if (wanted.limit > 0 && last < matching.size())
		{
			text += std::format("\n  ... {} more: offset {} reads on", matching.size() - last, last);
		}
	}

	// The task view the window shows: its commands, status and progress, its panel's text, check boxes and fields,
	// and its review where it has one.
	core::mcp::tool_result task_answer(app_frame& app, const rows_wanted& wanted)
	{
		const auto mode = app._state.view_mode();
		const auto view = app._view;
		json_writer json;
		json.object().text("view", agent_view_name(mode));

		if (!is_task_view(mode) || !view)
		{
			json.boolean("task", false).end_object();
			return structured_result(std::format("The window shows the {} view, which is no task: command_run opens "
			                                     "one with {}.",
			                                     agent_view_name(mode), task_commands),
			                         json.str());
		}

		const auto progress = view->progress();
		const auto title = std::string(view->title());
		const auto status = std::string(view->status());
		json.boolean("task", true).text("title", title).text("status", status).boolean("running", progress.active);

		if (progress.active)
		{
			json.key("progress").object().integer("done", progress.position).integer("total", progress.total);
			json.end_object();
		}

		auto text = std::format("{} ({} view)", title, agent_view_name(mode));
		if (progress.active)
		{
			text += progress.total > 0 ? std::format(" - running, {} of {} done", progress.position, progress.total)
			                           : " - running"s;
		}
		if (!status.empty()) text += "\nStatus: " + status;

		app.update_button_state(false);
		const auto bars = view->bars();
		std::string command_text;
		json.key("commands").array();

		for (const auto* const entries : {&bars.bottom_leading, &bars.bottom})
		{
			for (const auto& entry : *entries)
			{
				const auto* const id = std::get_if<commands>(&entry);
				const auto command = id ? app.find_command(*id) : nullptr;
				if (!command || !command->visible) continue;

				const auto name = command_name(*id);
				const auto rule = rule_of(*id);
				json.object().text("name", name).text("text", plain_label(command->text));
				json.boolean("enabled", command->enable);
				if (!command->enable && !command->disabled_reason.empty())
				{
					json.text("why_disabled", command->disabled_reason);
				}
				if (rule.need != command_need::read) json.text("needs", need_name(rule.need));
				json.end_object();

				command_text += std::format(" {}{}{}", name,
				                            rule.need == command_need::read
				                                ? ""s
				                                : std::format(" [{}]", need_name(rule.need)),
				                            command->enable ? "" : " (disabled)");
			}
		}

		json.end_array();
		if (!command_text.empty()) text += "\nCommands:" + command_text;

		const auto parts = panel_parts(app);
		json.key("panel").object().key("text").array();
		for (const auto& t : parts.titles) json.text(t);
		for (const auto& t : parts.texts) json.text(t);
		json.end_array();
		write_checks_and_fields(json, parts);
		json.end_object();

		text += "\nPanel:";
		for (const auto& t : parts.titles) text += "\n  " + t;
		for (const auto& t : parts.texts) text += "\n  " + t;
		text += checks_and_fields_text(parts);

		if (const auto list = std::dynamic_pointer_cast<review_list>(view)) write_review(json, text, *list, wanted);

		json.end_object();
		return structured_result(std::move(text), json.str());
	}

	void add_task_state(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "task_state";
		t.title = "What a task view shows";
		t.description = std::format(
			"The task view the window shows - Sync, Import, Locate, Tags, Rename, Convert, Metadata, Adjust Date, "
			"Edit, Movie or Options, which command_run opens with {}: its commands and whether each can run now, its "
			"status line and progress, its panel's text, check boxes and fields, which task_set fills in, and for a "
			"task reviewed before it runs, the review - each row as its columns show it, a page at a time and "
			"narrowed by contains - or after a run, what the run did to each row.",
			task_commands);
		t.input_schema = std::format(R"({{"type":"object","properties":{{{}}},"additionalProperties":false}})",
		                             rows_properties);
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			call->finish(task_answer(host.app(), rows_wanted_of(*call)));
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	// Answers task_set's job with the task as it stands once the window has caught up with what was set. The keyboard
	// goes back to the view first, which closes any suggestions a field offered as it was typed into.
	void answer_task_set(mcp_host& host, app_frame& app, const agent_job_ptr& job)
	{
		if (app._view_frame) app._view_frame->keyboard_to(keyboard_area::view, nullptr);

		host.when([&app] { return window_caught_up(app); }, caught_up_quiet_s, caught_up_timeout_s, [&app, job](bool)
		{
			if (!job->is_done()) job->complete(task_answer(app, {.limit = 10}));
		});
	}

	// Fills the fields task_set names as the panel shows them now - when every check box that could show or enable one
	// has been set - then answers.
	void fill_task_fields(mcp_host& host, app_frame& app, const rapidjson::Value& fields, const bool ticked,
	                      const agent_job_ptr& job)
	{
		const auto now = panel_parts(app);
		std::vector<std::string> problems;
		form_answer filled;
		find_fields(now, fields, field_need::enabled, filled, problems);

		if (!problems.empty())
		{
			job->complete(core::mcp::tool_result::error(
				std::format("{}: {}.\nThe panel:{}",
			                ticked ? "The check boxes were set, but no field was filled" : "Nothing was changed",
			                join_text(problems, "; "), checks_and_fields_text(now))));
			return;
		}

		fill_fields(filled);
		answer_task_set(host, app, job);
	}

	void add_task_set(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "task_set";
		t.title = "Fill in a task's panel";
		t.description =
			"Fills in the panel of the task view shown, as task_state shows it: sets check boxes and radio buttons "
			"by their text first, which can show or enable the fields under them, then fields by their label - a "
			"date as yyyy-mm-dd hh:mm:ss. Changes no files: the task's own commands run it, through command_run - "
			"analyzing needs nothing more, running needs -mcp-allow:write. Answers with the task as it then stands.";
		t.input_schema =
			R"({"type":"object","properties":{"checks":{"type":"object","additionalProperties":{"type":"boolean"},"description":"Check boxes and radio buttons, by their text"},"fields":{"type":"object","additionalProperties":{"type":"string"},"description":"Fields, by their label"}},"additionalProperties":false})";
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto refuse = [&call](std::string why)
			{
				call->finish(core::mcp::tool_result::error(std::move(why)));
			};

			if (!is_task_view(app._state.view_mode()))
			{
				return refuse(
					std::format("The window shows the {} view, which is no task: command_run opens one with {}",
				                agent_view_name(app._state.view_mode()), task_commands));
			}

			if (refused_while_modal(host, call)) return;

			if (app._view && app._view->progress().active)
			{
				return refuse("The task is running, and its panel takes no input until it ends: wait_until_idle waits "
				              "for it");
			}

			const auto* const checks = named_values(call->arguments(), "checks");
			const auto* const fields = named_values(call->arguments(), "fields");
			if (!checks && !fields) return refuse("Say what to set: check boxes, fields or both");

			const auto parts = panel_parts(app);
			std::vector<std::string> problems;
			form_answer answer;
			if (checks) find_checks(parts, *checks, answer, problems);

			// A check box this call changes can show or enable the fields under it, so until it is set a field need
			// only be there - and where one changes, not even that.
			const auto changes_checks = std::ranges::any_of(answer.ticks, [](const auto& tick)
			{ return tick.first->checked != tick.second; });
			if (fields)
			{
				find_fields(parts, *fields, changes_checks ? field_need::may_appear : field_need::present, answer,
				            problems);
			}

			if (!problems.empty())
			{
				return refuse(std::format("Nothing was changed: {}.\nThe panel:{}", join_text(problems, "; "),
				                          checks_and_fields_text(parts)));
			}

			tick_checks(answer);
			const auto job = host.start_job("task_set");

			if (!fields)
			{
				answer_task_set(host, app, job);
			}
			else if (!changes_checks)
			{
				fill_task_fields(host, app, *fields, false, job);
			}
			else
			{
				// What a check box shows or enables takes input once the panel has been laid out again and its frame
				// has found its controls where they now are. The fields are filled after that, as a user types into a
				// field once it can be seen. The call is held for the arguments fields points into.
				app.invalidate_view(view_invalid::view_layout);
				host.when([&app] { return window_caught_up(app); }, caught_up_quiet_s, caught_up_timeout_s,
				          [&host, &app, call, fields, job](bool)
				{
					if (job->is_done()) return;

					if (host.modal_ui_open() || !is_task_view(app._state.view_mode()))
					{
						job->complete(core::mcp::tool_result::error(
							"The check boxes were set, but the panel was gone before any field was filled: ui_state "
							"shows what is in front"));
						return;
					}

					fill_task_fields(host, app, *fields, true, job);
				});
			}

			// Long enough for the window to catch up with the check boxes, then with the fields.
			host.wait(call, job, 2.0 * caught_up_timeout_s + 1.0, false);
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Pictures
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// Capture makes new files; one saved over a file already there changes the user's files, which needs write.
	std::string replace_refusal(const mcp_host& host, const std::initializer_list<core::file_path> paths)
	{
		if (host.options().allow.write) return {};

		for (const auto& path : paths)
		{
			if (platform::exists(path))
				return std::format("{} is there already, and replacing it needs -mcp-allow:write", path.str());
		}

		return {};
	}

	core::mcp::tool_result take_screenshot(app_frame& app, const std::string& target, const std::string& path,
	                                       const bool include_image, const int max_size, const bool settled)
	{
		auto frame = app._app_frame;

		if (target == "dialog")
		{
			const auto d = front_dialog();
			if (!d) return core::mcp::tool_result::error("No dialog is open");
			frame = d->frame();
		}

		const auto captured = platform::capture_window_surface(frame->handle());
		if (!captured) return core::mcp::tool_result::error("The window could not be captured: log_read says why");

		files ff;
		const auto surface = max_size > 0 ? ff.fit_within(captured, {max_size, max_size}) : captured;
		const auto dimensions = surface->dimensions();

		core::mcp::tool_result result;
		std::string text;

		if (!path.empty())
		{
			const auto file_path = core::file_path(path);
			const auto image = ff.surface_to_image(surface, libfiles::metadata_blocks{}, file_encode_params{},
			                                       extension_to_format(file_path.extension()));
			const auto file = image ? platform::open_file(file_path, platform::file_open_mode::create)
			                        : platform::file_ptr{};
			const auto saved = file && file->write(image->data().data(), image->data().size()) == image->data().size();
			if (!saved) return core::mcp::tool_result::error(std::format("The picture could not be saved to {}", path));
			text = std::format("Saved {}, {}x{}", path, dimensions.cx, dimensions.cy);
		}

		if (include_image)
		{
			const auto png = ff.surface_to_image(surface, libfiles::metadata_blocks{}, file_encode_params{},
			                                     core::image_format::PNG);
			if (!png) return core::mcp::tool_result::error("The picture could not be encoded");

			core::mcp::content image;
			image.kind = core::mcp::content::kind_t::image;
			image.data.assign(reinterpret_cast<const char*>(png->data().data()), png->data().size());
			image.mime_type = "image/png";
			result.content.push_back(std::move(image));
			if (text.empty()) text = std::format("{}x{}", dimensions.cx, dimensions.cy);
		}

		if (!settled) text += std::format("; taken while still busy: {}", join_text(agent_window_busy(app), ", "));
		result.content.insert(result.content.begin(), core::mcp::content{.text = std::move(text)});

		json_writer json;
		json.object().integer("width", dimensions.cx).integer("height", dimensions.cy).boolean("settled", settled);
		if (!path.empty()) json.text("path", path);
		json.end_object();
		result.structured = json.str();
		return result;
	}

	void add_screenshot(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "screenshot";
		t.title = "Take a screenshot";
		t.description =
			"A picture of the window - or of the dialog in front - as it is drawn, on the desktop or out "
			"of sight. Waits up to settle_s seconds for the window to settle first. Saved to path where "
			"one is given, as PNG, JPEG, WebP, JPEG XL or AVIF by its extension, and returned as a PNG unless "
			"it was saved and image is false. max_size bounds its longer side in pixels: 1600 for a picture "
			"returned, none for one only saved.";
		t.input_schema =
			R"({"type":"object","properties":{"target":{"type":"string","enum":["window","dialog"]},"path":{"type":"string","description":"The full path to save it to"},"image":{"type":"boolean"},"max_size":{"type":"integer","minimum":64,"maximum":16384},"settle_s":{"type":"number","minimum":0,"maximum":120,"description":"How long to wait for the window to settle; 10 by default"}},"additionalProperties":false})";
		t.annotations.read_only = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto target = call->text("target", "window");
			const auto path = call->text("path");

			if (!path.empty())
			{
				if (!core::is_path(path))
				{
					call->finish(
						core::mcp::tool_result::error(std::format("Give the full path to save to, not {}", path)));
					return;
				}

				if (!files::can_save(core::file_path(path)))
				{
					call->finish(core::mcp::tool_result::error("Save as .png, .jpg, .webp, .jxl or .avif"));
					return;
				}

				if (const auto why = replace_refusal(host, {core::file_path(path)}); !why.empty())
				{
					call->finish(core::mcp::tool_result::error(why));
					return;
				}
			}

			const auto include_image = call->flag("image", path.empty());
			const auto max_size = static_cast<int>(call->number("max_size").value_or(include_image ? 1600 : 0));
			const auto settle_s = call->number("settle_s").value_or(10.0);

			answer_when(host, call, "screenshot", [&app, settle_s]
			{ return settle_s <= 0.0 || agent_window_settled(app); }, settle_s > 0.0 ? 0.3 : 0.0, settle_s, false,
			            [&app, target, path, include_image, max_size](const bool settled)
			{ return take_screenshot(app, target, path, include_image, max_size, settled); });
		};
		host.add(std::move(t), tool_tier::capture, true);
	}

	// What a recording that ended by itself wrote, for record_stop to say.
	struct recording_notes
	{
		std::string ended;
	};

	void add_recording(mcp_host& host)
	{
		const auto notes = std::make_shared<recording_notes>();

		core::mcp::tool start;
		start.name = "record_start";
		start.title = "Start recording the window";
		start.description = "Films the window to an MP4 at 30 frames a second, as the screen shows it - menus, bubbles "
							"and the pointer with it - until record_stop, or for seconds. record_mark names moments "
							"in it. Needs the window on the desktop: -mcp-ui:visible. While it records, the window is "
							"held above others unless present is false.";
		start.input_schema =
			R"({"type":"object","properties":{"path":{"type":"string","description":"The full path of the MP4"},"seconds":{"type":"number","minimum":0},"pointer":{"type":"boolean","description":"Draw the system pointer; true by default"},"present":{"type":"boolean"}},"required":["path"],"additionalProperties":false})";
		start.handler = [&host, notes](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto path = call->text("path");
			const auto refuse = [&call](std::string why)
			{
				call->finish(core::mcp::tool_result::error(std::move(why)));
			};

			if (host.options().presence != agent_presence::visible)
			{
				return refuse("A recording films the window as the screen shows it, so the window must be on the "
				              "desktop: start the session with -mcp-ui:visible");
			}

			if (app._tutorial) return refuse("Steps are being acted out, and a tutorial records itself");
			if (!core::is_path(path)) return refuse(std::format("Give the movie's full path, not {}", path));
			// A recording writes where its moments are named beside the movie as well.
			const core::file_path movie(path);
			if (auto why = replace_refusal(host, {movie, screen_recording_markers_path(movie)}); !why.empty())
				return refuse(std::move(why));

			screen_recording_request request;
			request.window = app._app_frame->handle();
			request.path = core::file_path(path);
			request.seconds = call->number("seconds").value_or(0.0);
			request.draw_pointer = call->flag("pointer", true);
			request.present_window = call->flag("present", true);

			notes->ended.clear();
			app._recording_ended = [notes](const std::string_view summary)
			{
				notes->ended = summary;
			};
			if (const auto why = app.start_recording(request); !why.empty())
				return refuse(std::format("Not recording: {}", why));

			call->finish(core::mcp::tool_result::text(
				std::format("Recording {}: record_mark names a moment in it, record_stop ends it", path)));
		};
		host.add(std::move(start), tool_tier::capture, true);

		core::mcp::tool mark;
		mark.name = "record_mark";
		mark.title = "Name a moment in the recording";
		mark.description = "Names this moment of the recording. The names are written beside the movie, one "
						   "\"<seconds><TAB><name>\" a line, in <movie>.markers.txt, once it is finished.";
		mark.input_schema =
			R"({"type":"object","properties":{"name":{"type":"string","minLength":1}},"required":["name"],"additionalProperties":false})";
		mark.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();

			if (!app._recording)
			{
				call->finish(core::mcp::tool_result::error("Nothing is being recorded"));
				return;
			}

			const auto name = call->text("name");
			app._recording->mark(name);
			call->finish(core::mcp::tool_result::text(std::format("{} at {:.2f} s", name, app._recording->elapsed())));
		};
		host.add(std::move(mark), tool_tier::capture, true);

		core::mcp::tool stop;
		stop.name = "record_stop";
		stop.title = "Stop recording";
		stop.description = "Ends the recording and finishes its file: says what was written - its length and size - "
						   "or why nothing was.";
		stop.handler = [&host, notes](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			std::string summary;

			if (app._recording) summary = app.finish_recording();
			else if (!notes->ended.empty()) summary = "It had ended already: " + notes->ended;

			if (summary.empty())
			{
				call->finish(core::mcp::tool_result::error("Nothing is being recorded"));
				return;
			}

			notes->ended.clear();
			call->finish(core::mcp::tool_result::text(summary));
		};
		host.add(std::move(stop), tool_tier::capture, true);
	}

#ifdef _DEBUG
	void add_debug_action(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "debug_action";
		t.title = "Force a rare event";
		t.description = "Debug builds only: forces what is hard to reach by hand - reset-graphics (the graphics "
						"device lost and made again), new-version (the offer of an update shown), send-crash-report (a "
						"crash report made for a test file), crash (the process crashes, ending the session).";
		t.input_schema =
			R"({"type":"object","properties":{"action":{"type":"string","enum":["reset-graphics","new-version","send-crash-report","crash"]}},"required":["action"],"additionalProperties":false})";
		t.annotations.destructive = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto action = call->text("action");
			platform::run_ui([&app, action] { app.run_test_action(action); });
			call->finish(core::mcp::tool_result::text(std::format("{} is under way", action)));
		};
		host.add(std::move(t), tool_tier::dev, true);
	}
#endif
}

std::string describe_dialogs_json(app_frame& /*app*/)
{
	json_writer json;
	json.array();
	for (const auto& parts : dialogs_and_prompts()) write_dialog(json, parts);
	json.end_array();
	return json.str();
}

std::string describe_dialogs_text(app_frame& /*app*/)
{
	return dialogs_text(dialogs_and_prompts());
}

std::string_view agent_command_need(const commands id)
{
	return need_name(rule_of(id).need);
}

std::string agent_command_refusal(const mcp_host& host, const commands id)
{
	return command_refusal(host, id);
}

std::string agent_view_json(const app_frame& app, const size_t listed)
{
	json_writer json;
	json.object();
	write_view(json, app, listed);
	json.end_object();
	return json.str();
}

std::string agent_view_text(const app_frame& app, const size_t listed)
{
	return view_text(app, listed);
}

std::vector<std::string> dialogs_showing_work()
{
	std::vector<std::string> result;

	for (const auto& parts : open_dialog_parts())
	{
		if (parts.owner->_shows_work) result.push_back(parts.texts.empty() ? "working"s : join_text(parts.texts, " "));
	}

	return result;
}

bool dialog_awaits_answer()
{
	return platform::system_prompt_shown() || std::ranges::any_of(open_dialogs(), [](const std::weak_ptr<dialog>& weak)
	{
		const auto d = weak.lock();
		return d && d->frame()->is_visible() && !d->_shows_work;
	});
}

void add_ui_tools(mcp_host& host)
{
	add_ui_state(host);
	add_command_list(host);
	add_command_run(host);
	add_navigate(host);
	add_select(host);
	add_dialog_respond(host);
	add_task_state(host);
	add_task_set(host);
	add_ui_input(host);
	add_tutorial_run(host);
	add_screenshot(host);
	add_recording(host);
#ifdef _DEBUG
	add_debug_action(host);
#endif
	add_control_tools(host);
	add_face_tools(host);
}
