// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The agent tools that work the window as a user does, from what it shows (app_mcp.h): ui_controls lists
// what can be pressed, typed into or pointed at now - the bars' buttons, boxes and slider, the view's links, parts and
// panel, the dialog in front and the open menu's rows - ui_act works one with input given to its window as the
// platform's own is, menu_respond chooses from the open menu, and coverage says which commands have run and which of
// the controls listed have been used, so a sweep can press every button and know what it has missed.

#include "pch.h"

#include "app_mcp.h"
#include "app_mcp_json.h"
#include "model_db.h"
#include "model_index.h"
#include "model.h"
#include "ui_view.h"
#include "ui_dialog.h"
#include "ui_controls.h"
#include "app.h"

namespace
{
	constexpr std::string_view wait_property =
		R"("wait_s":{"type":"number","minimum":0,"maximum":600,"description":"How long to wait for the answer before replying with a job to wait on; 50 by default."})";

	// What the window reports once it has caught up with what was done.
	constexpr double caught_up_quiet_s = 0.05;
	constexpr double caught_up_timeout_s = 5.0;

	bool window_caught_up(const app_frame& app)
	{
		return app._invalids.load() == view_invalid::none && !(app._view && app._view->reviewing());
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Coverage
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// What the session has exercised: how often each command has run and each menu opened, the controls listed, and
	// how often each has been used - by ui_act or menu_respond, or by any input that reached it. On the UI thread.
	struct usage_record
	{
		bool tracking = false;
		std::map<commands, int> runs;
		std::map<commands, int> menus;
		// Each id listed, with its scope and kind.
		std::map<std::string, std::pair<std::string, std::string>, std::less<>> listed;
		std::map<std::string, int, std::less<>> used;
		// The id the latest listing gave each control, and each button of a toolbar by its command.
		std::map<std::pair<const ui::control*, const ui::command*>, std::string> ids;
		// Controls used that no listing named, by what they are and say.
		std::map<std::string, int, std::less<>> used_unlisted;
	};

	usage_record& usage()
	{
		static usage_record record;
		return record;
	}

	std::optional<commands> command_of(const ui::command_ptr& c)
	{
		if (!c) return {};
		if (const auto* const id = std::any_cast<commands>(&c->opaque)) return *id;
		return {};
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// What the window offers
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// Something a user can work: a control, a toolbar's button, a link or another part of what the view draws, an area
	// only pointed into - a picture, a chart, a map - the view itself, or a row of the open menu. Its id is where it is,
	// what it is and what it says, numbered where several say the same: "status/button/Rotate clockwise".
	struct agent_target
	{
		std::string id;
		std::string scope;
		std::string kind;
		std::string label;
		bool enabled = true;
		std::optional<bool> checked;
		std::optional<std::string> value;
		std::string why_disabled;
		// The frame its input goes to, and where that frame draws it, in its client pixels.
		ui::frame_ptr frame;
		recti bounds;
		// Where its frame shows what scrolls, and how to scroll it into view; empty and null where it is always shown.
		recti visible;
		std::function<void(recti)> reveal;
		ui::command_ptr command;
		ui::control_ptr control;
		view_element_ptr element;
		// A part of the view's frame: its area and which of its element's parts it is.
		std::optional<keyboard_area> area;
		int part = 0;
		// A menu row: whether it opens a submenu, and the rows it is in from the first level down, itself last.
		bool submenu = false;
		std::vector<std::string> path;
	};

	// What text reads as in an id: its first line, without its mnemonic marker, its runs of space made one, and short.
	// An icon font's glyphs - the private use area - and control characters read as nothing.
	std::string short_label(const std::string_view text)
	{
		constexpr size_t longest = 60;
		const auto parsed = ui::parse_mnemonic(text.substr(0, text.find_first_of("\r\n"))).text;
		const std::string_view view(parsed);
		std::string result;
		auto space = false;

		for (auto i = view.begin(); i != view.end();)
		{
			const auto start = i;
			const auto c = core::pop_utf8_char(i, view.end());
			if (i == start) break;

			if (c == ' ' || c == '\t')
			{
				space = !result.empty();
				continue;
			}

			if (c < 0x20 || (c >= 0xE000 && c <= 0xF8FF)) continue;
			if (space) result += ' ';
			space = false;
			result.append(start, i);
		}

		if (result.size() > longest)
		{
			auto end = longest;
			while (end > 0 && (static_cast<uint8_t>(result[end]) & 0xC0) == 0x80) --end;
			result.resize(end);
			result += "...";
		}

		return result;
	}

	// What an element is, by its type's name, for one that says nothing of itself: "photo_control".
	std::string type_label(const view_element& e)
	{
		std::string name = typeid(e).name();

		for (const auto prefix : {"class "sv, "struct "sv})
		{
			if (name.starts_with(prefix)) name.erase(0, prefix.size());
		}

		if (const auto colons = name.rfind("::"); colons != std::string::npos) name.erase(0, colons + 2);
		// A mangled name leads with its length.
		while (!name.empty() && std::isdigit(static_cast<unsigned char>(name.front()))) name.erase(0, 1);
		return name;
	}

	// The first thing a tooltip's elements say.
	std::string first_text(const view_element_ptr& e)
	{
		if (!e) return {};
		if (const auto* const text = dynamic_cast<const ui::text_element_base*>(e.get()); text && !text->text().empty())
			return text->text();
		if (const auto* const title = dynamic_cast<const ui::title_control*>(e.get()); title && !title->text().empty())
			return title->text();
		if (const auto* const title2 = dynamic_cast<const ui::title_control2*>(e.get());
		    title2 && !title2->title().empty())
			return title2->title();

		std::string found;
		e->visit_children([&found](const view_element_ptr& child)
		{
			if (found.empty()) found = first_text(child);
		});
		return found;
	}

	// How a walk of a tree of elements finds its targets: the scope they are in, the frame drawing them, how that
	// frame scrolls them, and the text read last, which names the field after it as dialog_respond names it.
	struct agent_walk
	{
		app_frame* app = nullptr;
		std::string scope;
		ui::frame_ptr frame;
		recti visible;
		std::function<void(recti)> reveal;
		std::optional<keyboard_area> area;
		std::string read_before;
		std::vector<agent_target>* out = nullptr;
	};

	void add_target(agent_walk& w, agent_target t)
	{
		if (t.bounds.is_empty()) return;
		t.scope = w.scope;
		t.frame = w.frame;
		t.visible = w.visible;
		t.reveal = w.reveal;
		if (t.element) t.area = w.area;
		w.out->push_back(std::move(t));
	}

	// What a field is called: its own label, or else what is read just before it, where that is short enough to be one.
	std::string field_name(const std::string& own, const std::string& before)
	{
		constexpr size_t longest_label = 80;
		if (!own.empty()) return short_label(own);

		auto label = std::string(core::trim(before));
		if (const auto stop = label.find(". "); stop != std::string::npos) label.resize(stop);
		while (!label.empty() && (label.back() == ':' || label.back() == ' ' || label.back() == '.')) label.pop_back();
		return label.size() <= longest_label ? short_label(label) : std::string{};
	}

	// What a part says: a command link its command's name, a link its text, and anything else what its tooltip says
	// first there.
	std::string part_label(const agent_walk& w, const view_element_ptr& e, const recti bounds, const pointi offset,
	                       ui::command_ptr& command)
	{
		if (const auto link = std::dynamic_pointer_cast<command_link_element>(e))
		{
			command = link->command();
			return short_label(!command->toolbar_text.empty() ? command->toolbar_text : command->text);
		}

		if (const auto* const text = dynamic_cast<const ui::text_element_base*>(e.get()))
		{
			if (auto label = short_label(text->text()); !label.empty()) return label;
		}

		view_hover_element hover;
		hover.clear();
		e->tooltip(hover, bounds.center(), offset);

		if (hover.id != commands::none && w.app)
		{
			if (auto c = w.app->find_command(hover.id))
			{
				command = std::move(c);
				return short_label(command->text);
			}
		}

		return short_label(first_text(hover.elements));
	}

	void add_control(agent_walk& w, const ui::control_ptr& c, const bool enabled, const std::string& label)
	{
		if (!c || !c->is_visible()) return;

		agent_target t;
		t.control = c;
		t.bounds = c->frame_bounds();
		t.enabled = enabled && c->enabled;

		if (const auto toolbar = std::dynamic_pointer_cast<ui::toolbar>(c))
		{
			for (const auto& b : toolbar->buttons())
			{
				if (!b.visible || !b.command) continue;

				agent_target button = t;
				button.kind = "button";
				button.label = short_label(b.text);
				button.command = b.command;
				button.bounds = b.bounds;
				button.enabled = t.enabled && b.enabled;
				if (b.checked || b.command->checkable) button.checked = b.checked;
				if (!button.enabled) button.why_disabled = b.command->disabled_reason;
				add_target(w, std::move(button));
			}

			return;
		}

		if (const auto button = std::dynamic_pointer_cast<ui::button>(c))
		{
			t.kind = c->traits.push_button ? "button" : c->traits.radio_group >= 0 ? "radio" : "check";
			t.label = short_label(button->window_text());
			if (!c->traits.push_button)
			{
				t.checked = button->is_checked();
				w.read_before = button->window_text();
			}
		}
		else if (const auto edit = std::dynamic_pointer_cast<ui::edit>(c))
		{
			t.kind = "edit";
			t.label = field_name(label, w.read_before);
			t.value = edit->window_text();
			w.read_before.clear();
		}
		else if (const auto date = std::dynamic_pointer_cast<ui::date_time_control>(c))
		{
			t.kind = "date";
			t.label = field_name(label, w.read_before);
			t.value = date->date().is_valid() ? date->date().to_xmp_date() : std::string{};
			w.read_before.clear();
		}
		else if (const auto slider = std::dynamic_pointer_cast<ui::trackbar>(c))
		{
			t.kind = "slider";
			t.label = field_name(label, w.read_before);
			t.value = std::to_string(slider->get_pos());
			w.read_before.clear();
		}
		else
		{
			t.kind = "control";
			t.label = !label.empty() ? short_label(label) : type_label(*c);
		}

		add_target(w, std::move(t));
	}

	// Each control and each part that acts, as Tab finds them, and what is only pointed into as one area: disabled ones
	// too, said to be.
	void walk_element(const view_element_ptr& e, const pointi offset, const bool enabled, const std::string& label,
	                  agent_walk& w)
	{
		if (!e || !e->is_visible()) return;

		const auto here_enabled = enabled && e->is_enabled();
		auto here_label = label;

		if (const auto* const title = dynamic_cast<const ui::title_control*>(e.get()))
		{
			if (!title->text().empty()) w.read_before = title->text();
		}
		else if (const auto* const title2 = dynamic_cast<const ui::title_control2*>(e.get()))
		{
			if (!title2->title().empty()) w.read_before = title2->title();
		}
		else if (const auto* const labelled = dynamic_cast<const ui::labelled_element*>(e.get()))
		{
			if (!labelled->label().empty()) here_label = labelled->label();
		}
		else if (const auto* const row = dynamic_cast<const ui::labelled_row*>(e.get()))
		{
			if (!row->row_label().empty()) here_label = std::string(row->row_label());
		}

		if (e->kind == ui::element_kind::control)
		{
			add_control(w, std::static_pointer_cast<ui::control>(e), here_enabled, here_label);
			return;
		}

		if (e->kind != ui::element_kind::content) return;

		if (const auto* const source = dynamic_cast<const keyboard_parts_source*>(e.get()))
		{
			const auto parts = source->keyboard_parts();

			if (parts.empty())
			{
				agent_target t;
				t.kind = "area";
				t.label = type_label(*e);
				t.enabled = here_enabled;
				t.bounds = e->bounds.offset(offset);
				t.element = e;
				add_target(w, std::move(t));
				return;
			}

			for (size_t i = 0; i < parts.size(); ++i)
			{
				agent_target t;
				t.kind = "part";
				t.bounds = parts[i].offset(offset);
				t.label = part_label(w, e, t.bounds, offset, t.command);
				if (t.label.empty()) t.label = std::format("{} {}", type_label(*e), i + 1);
				t.enabled = here_enabled;
				t.element = e;
				t.part = static_cast<int>(i);
				add_target(w, std::move(t));
			}

			return;
		}

		auto holds = false;
		const auto children_offset = offset + e->children_offset();

		e->visit_children([&](const view_element_ptr& child)
		{
			holds = true;
			walk_element(child, children_offset, here_enabled, here_label, w);
		});

		if (holds) return;

		const auto* const text = dynamic_cast<const ui::text_element_base*>(e.get());

		if (e->can_invoke())
		{
			agent_target t;
			t.bounds = e->bounds.offset(offset);
			t.label = part_label(w, e, t.bounds, offset, t.command);
			t.kind = text || std::dynamic_pointer_cast<command_link_element>(e) ? "link" : "part";
			if (t.label.empty()) t.label = type_label(*e);
			t.enabled = here_enabled && (!t.command || t.command->enable);
			if (t.command && !t.command->enable) t.why_disabled = t.command->disabled_reason;
			t.element = e;
			add_target(w, std::move(t));
		}

		if (text && !text->text().empty()) w.read_before = text->text();
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

	std::string_view area_name(const keyboard_area area)
	{
		switch (area)
		{
		case keyboard_area::sidebar: return "sidebar";
		case keyboard_area::listing: return "listing";
		case keyboard_area::media: return "media";
		case keyboard_area::panel: return "panel";
		case keyboard_area::view: break;
		}

		return "view";
	}

	// The open menu's rows: an item, or each button of a row of them, named by its path from the first level down.
	void add_menu_targets(std::vector<agent_target>& out)
	{
		const auto* const session = ui::open_menu_session();
		if (!session || session->done()) return;

		const auto levels = session->levels();
		std::vector<std::vector<std::string>> prefixes(levels.size());

		for (size_t l = 0; l < levels.size(); ++l)
		{
			const auto& level = levels[l];

			if (l > 0 && level.opened_from >= 0 && static_cast<size_t>(level.opened_from) < levels[l - 1].rows.size())
			{
				prefixes[l] = prefixes[l - 1];
				prefixes[l].push_back(levels[l - 1].rows[level.opened_from].text);
			}

			const auto add = [&](const ui::menu_row_state& row, const std::string_view kind)
			{
				agent_target t;
				t.scope = "menu";
				t.kind = kind;
				t.frame = level.popup;
				t.bounds = row.bounds;
				t.command = row.command;
				t.enabled = row.enabled;
				if (row.checked || (row.command && row.command->checkable)) t.checked = row.checked;
				if (!row.enabled && row.command) t.why_disabled = row.command->disabled_reason;
				t.submenu = row.submenu;
				t.path = prefixes[l];
				t.path.push_back(row.text);
				t.label = short_label(join_text(t.path, " > "));
				if (!t.bounds.is_empty()) out.push_back(std::move(t));
			};

			for (const auto& row : level.rows)
			{
				if (row.type == ui::menu_row_state::kind::item) add(row, "item");
				else if (row.type == ui::menu_row_state::kind::buttons)
				{
					for (const auto& cell : row.cells) add(cell, "button");
				}
			}
		}
	}

	// Everything the window offers now, in the order Tab visits it, with the dialog in front and the open menu last; and
	// each one listed is remembered, for coverage.
	std::vector<agent_target> window_targets(app_frame& app)
	{
		std::vector<agent_target> out;
		if (!app._window_started) return out;

		const auto walk_bar = [&](const std::shared_ptr<bar_frame>& bar, const std::string_view scope)
		{
			if (!bar || !app._show_bars) return;

			agent_walk w{.app = &app, .scope = std::string(scope), .frame = bar->frame(), .out = &out};

			for (const auto& c : bar->_controls)
			{
				const auto* const name = c == app._search_edit        ? "address"
				                         : c == app._filter_edit      ? "filter"
				                         : c == app._item_size        ? "item size"
				                         : c == app._face_filter_edit ? "people filter"
				                         : c == app._face_size        ? "face size"
				                                                      : "";
				add_control(w, c, true, name);
			}
		};

		walk_bar(app._title_bar, "title");

		// The logo beside the title, which opens the help.
		if (app._show_bars && app._app_logo && !app._title_bounds.is_empty())
		{
			agent_target logo;
			logo.scope = "title";
			logo.kind = "link";
			logo.label = "logo";
			logo.frame = app._title_bar->frame();
			logo.bounds = app._title_bounds;
			logo.command = app.find_command(commands::view_help);
			out.push_back(std::move(logo));
		}

		if (const auto& view = app._view_frame)
		{
			for (auto& region : view->keyboard_regions())
			{
				agent_walk w{.app = &app,
				             .scope = std::string(area_name(region.area)),
				             .frame = view->frame(),
				             .visible = region.clip,
				             .reveal = region.reveal,
				             .area = region.area,
				             .out = &out};

				for (const auto& root : region.roots) walk_element(root, region.offset, true, {}, w);
			}

			if (!view->_view_bounds.is_empty())
			{
				agent_target content;
				content.scope = "view";
				content.kind = "content";
				content.label = agent_view_name(app._state.view_mode());
				content.frame = view->frame();
				content.bounds = view->_view_bounds;
				out.push_back(std::move(content));
			}

			if (view->_strip->is_visible() && app._selector_frame && !app._selector_frame->_view_bounds.is_empty())
			{
				agent_target strip;
				strip.scope = "strip";
				strip.kind = "content";
				strip.label = "strip";
				strip.frame = app._selector_frame->frame();
				strip.bounds = app._selector_frame->_view_bounds;
				out.push_back(std::move(strip));
			}
		}

		walk_bar(app._status_bar, "status");

		if (const auto d = front_dialog())
		{
			agent_walk w{.app = &app, .scope = "dialog", .frame = d->frame(), .out = &out};
			d->visit_elements([&w](const view_element_ptr& e, const pointi offset)
			{ walk_element(e, offset, true, {}, w); });
		}

		add_menu_targets(out);

		std::map<std::string, int, std::less<>> counts;

		for (auto& t : out)
		{
			auto base = std::format("{}/{}/{}", t.scope, t.kind, t.label.empty() ? "unlabelled"s : t.label);
			const auto n = ++counts[base];
			t.id = n == 1 ? std::move(base) : std::format("{}#{}", base, n);
		}

		auto& record = usage();
		record.ids.clear();

		for (const auto& t : out)
		{
			record.listed[t.id] = {t.scope, t.kind};
			const auto* const toolbar_command = std::dynamic_pointer_cast<ui::toolbar>(t.control) ? t.command.get()
			                                                                                      : nullptr;
			if (t.control) record.ids[{t.control.get(), toolbar_command}] = t.id;
		}

		return out;
	}

	const agent_target* find_target(const std::vector<agent_target>& targets, const std::string_view id)
	{
		const auto found = std::ranges::find_if(targets, [id](const agent_target& t) { return t.id == id; });
		return found == targets.end() ? nullptr : &*found;
	}

	// The ids most like one not found, for the error that says so.
	std::vector<std::string> similar_ids(const std::vector<agent_target>& targets, const std::string_view id)
	{
		constexpr size_t most = 8;
		const auto last = id.substr(id.rfind('/') == std::string_view::npos ? 0 : id.rfind('/') + 1);
		std::vector<std::string> result;

		for (const auto& t : targets)
		{
			if (result.size() >= most) break;
			if (core::ifind(t.id, last) != std::string_view::npos) result.push_back(t.id);
		}

		return result;
	}

	void write_target(json_writer& json, const agent_target& t)
	{
		json.object().text("id", t.id).text("scope", t.scope).text("kind", t.kind).text("label", t.label);
		json.boolean("enabled", t.enabled);
		if (!t.why_disabled.empty()) json.text("why_disabled", t.why_disabled);
		if (t.checked) json.boolean("checked", *t.checked);
		if (t.value) json.text("value", *t.value);
		if (const auto id = command_of(t.command))
		{
			json.text("command", command_name(*id));
			if (const auto need = agent_command_need(*id); !need.empty()) json.text("needs", need);
		}
		if (t.submenu) json.boolean("submenu", true);
		if (!t.path.empty())
		{
			json.key("path").array();
			for (const auto& row : t.path) json.text(row);
			json.end_array();
		}
		if (!t.visible.is_empty() && !t.visible.contains(t.bounds.center())) json.boolean("scrolled_away", true);
		json.key("bounds").array().integer(t.bounds.left).integer(t.bounds.top).integer(t.bounds.width());
		json.integer(t.bounds.height()).end_array();
		if (const auto used = usage().used.find(t.id); used != usage().used.end()) json.integer("used", used->second);
		json.end_object();
	}

	std::string target_text(const agent_target& t)
	{
		auto text = t.id;
		if (t.checked) text += *t.checked ? " [x]" : " [ ]";
		if (t.value) text += std::format(" = '{}'", short_label(*t.value));
		if (t.submenu) text += " >";
		if (const auto id = command_of(t.command); id && t.kind != "button" && t.kind != "item")
			text += std::format(" ({})", command_name(*id));
		if (!t.enabled)
			text += t.why_disabled.empty() ? " (disabled)"s : std::format(" (disabled: {})", t.why_disabled);
		return text;
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Working them
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// What is in front of the window: the dialog that waits on an answer, and the open menu with how deep it is open.
	struct modal_state
	{
		const void* dialog = nullptr;
		const void* menu = nullptr;
		size_t levels = 0;

		bool operator==(const modal_state&) const = default;
	};

	modal_state modal_now()
	{
		modal_state s;
		if (const auto d = front_dialog(); d && !d->_shows_work) s.dialog = d.get();

		if (const auto* const m = ui::open_menu_session(); m && !m->done())
		{
			s.menu = m;
			s.levels = m->popups().size();
		}

		return s;
	}

	// Whether a dialog or a menu - or a deeper level of one - has opened since.
	bool opened_since(const modal_state& before)
	{
		const auto now = modal_now();
		return now != before && (now.dialog || now.menu);
	}

	struct act_request
	{
		std::string tool;
		std::string id;
		std::string action = "press";
		std::string text;
		char32_t key = 0;
		platform::key_state keys;
		pointi drag;
		int notches = 0;
		// menu_respond's: the rows to press, from the first level down, or none with cancel.
		std::vector<std::string> path;
		bool cancel = false;
	};

	core::mcp::tool_result act_answer(app_frame& app, const act_request& r, const bool opened,
	                                  const std::string& failed)
	{
		const auto what = r.tool == "menu_respond"
		                      ? (r.cancel ? "Cancelled the menu"s : std::format("Chose {}", join_text(r.path, " > ")))
		                      : std::format("{} {}", r.action, r.id);
		json_writer json;
		json.object().text("tool", r.tool).text("id", r.id).text("action", r.cancel ? "cancel"sv : r.action);
		json.boolean("opened", opened);
		if (!failed.empty()) json.text("failed", failed);
		json.key("window").raw(agent_view_json(app, 10));
		json.key("dialogs").raw(describe_dialogs_json(app));
		json.key("menu").raw(describe_menu_json());
		json.end_object();

		auto text = !failed.empty() ? std::format("{}: {}. {}", what, failed, agent_view_text(app, 0))
		            : opened        ? std::format("{}: a dialog or menu opened, which waits on an answer.", what)
		                            : std::format("{}. {}", what, agent_view_text(app, 0));
		text += describe_dialogs_text(app);
		text += describe_menu_text();
		return structured_result(std::move(text), json.str(), !failed.empty());
	}

	bool send_click(const ui::frame_ptr& frame, const pointi at, const platform::pointer_button button,
	                const platform::key_state keys)
	{
		return ui::send_pointer(frame, ui::pointer_input::down, button, at, keys) &&
		       ui::send_pointer(frame, ui::pointer_input::up, button, at, keys);
	}

	// Presses the open menu's rows along a path: each submenu on the way opened as a press opens it, then the last row
	// chosen. A row named alone is looked for in the deepest level open first. Why not, where it cannot be.
	std::string press_menu_path(mcp_host& host, app_frame& app, const std::vector<std::string>& path)
	{
		const auto left = platform::pointer_button::left;

		for (size_t i = 0; i < path.size(); ++i)
		{
			const auto targets = window_targets(app);
			const agent_target* found = nullptr;

			for (auto t = targets.rbegin(); t != targets.rend() && !found; ++t)
			{
				if (t->scope != "menu" || t->path.empty() || core::icmp(t->path.back(), path[i]) != 0) continue;

				// A path from the first level is matched row by row; a row named alone, at any depth.
				const auto matches = path.size() == 1 ||
				                     (t->path.size() == i + 1 &&
				                      std::ranges::equal(t->path, std::span(path).first(i + 1),
				                                         [](const std::string& a, const std::string& b)
				{ return core::icmp(a, b) == 0; }));
				if (matches) found = &*t;
			}

			if (!found)
				return std::format("the menu shows no '{}'",
				                   join_text(std::vector(path.begin(), path.begin() + static_cast<ptrdiff_t>(i) + 1),
				                             " > "));
			if (!found->enabled)
				return std::format("'{}' is disabled{}", found->label,
				                   found->why_disabled.empty() ? "" : ": " + found->why_disabled);

			const auto last = i + 1 == path.size();

			if (last)
			{
				if (found->submenu) return std::format("'{}' opens a submenu: name a row of it", found->label);

				// What is chosen runs as its command would from command_run, and needs what that needs; a row with no
				// command of the window's own - one made for what the menu is on - can change anything.
				const auto id = command_of(found->command);
				const auto why = id ? agent_command_refusal(host, *id)
				                 : host.options().allow.write
				                     ? std::string{}
				                     : std::format("'{}' acts on what the menu is on, which needs -mcp-allow:write",
				                                   found->label);
				if (!why.empty()) return why;
				usage().used[found->id]++;
			}
			else if (!found->submenu)
			{
				return std::format("'{}' opens no submenu", found->label);
			}

			const auto at = found->bounds.center();
			ui::send_pointer(found->frame, ui::pointer_input::move, left, at);
			if (!send_click(found->frame, at, left, {})) return "the menu took no input";
		}

		return {};
	}

	// Gives the target the input the action is, as the platform gives its window real input: the pointer moved to it
	// first, as a hand moves it. Why not, where it took none.
	std::string perform(mcp_host& host, app_frame& app, const act_request& r)
	{
		if (r.cancel)
		{
			if (auto* const session = ui::open_menu_session()) session->cancel();
			return {};
		}

		if (!r.path.empty()) return press_menu_path(host, app, r.path);

		auto targets = window_targets(app);
		const auto* found = find_target(targets, r.id);
		if (!found) return "it is no longer shown";

		// One its frame has scrolled out of sight is scrolled into view first, as a user scrolls to it. The frame is laid
		// out then, as it is before any input a user could give next, so the controls in it have moved with the scroll.
		if (found->reveal && !found->visible.is_empty() && !found->visible.contains(found->bounds.center()))
		{
			found->reveal(found->bounds);
			if (found->frame) found->frame->layout();
			targets = window_targets(app);
			found = find_target(targets, r.id);
			if (!found) return "it went once scrolled into view";
		}

		const auto target = *found;
		const auto& frame = target.frame;
		const auto at = target.bounds.center();
		const auto left = platform::pointer_button::left;
		const auto keys = r.keys;
		const auto move = [&](const pointi p)
		{
			return ui::send_pointer(frame, ui::pointer_input::move, left, p, keys);
		};

		if (!move(at)) return "its window takes no input now: a dialog holds it";
		usage().used[target.id]++;

		if (r.action == "hover") return {};

		if (r.action == "press")
		{
			send_click(frame, at, left, keys);
		}
		else if (r.action == "double_click")
		{
			send_click(frame, at, left, keys);
			ui::send_pointer(frame, ui::pointer_input::double_click, left, at, keys);
			ui::send_pointer(frame, ui::pointer_input::up, left, at, keys);
		}
		else if (r.action == "right_click")
		{
			send_click(frame, at, platform::pointer_button::right, keys);
			ui::send_context_menu(frame, at);
		}
		else if (r.action == "drag")
		{
			constexpr auto steps = 8;
			ui::send_pointer(frame, ui::pointer_input::down, left, at, keys);

			for (auto i = 1; i <= steps; ++i) move({at.x + r.drag.x * i / steps, at.y + r.drag.y * i / steps});

			ui::send_pointer(frame, ui::pointer_input::up, left, at + r.drag, keys);
		}
		else if (r.action == "wheel")
		{
			constexpr auto wheel_detent = 120;
			ui::send_wheel(frame, at, r.notches * wheel_detent, keys);
		}
		else if (r.action == "type")
		{
			// As a user replaces what a box holds: a press into it, Ctrl+A, then the text, Enter starting each new line.
			send_click(frame, at, left, {});
			platform::key_state control;
			control.control = true;
			ui::send_key(frame, U'A', control);
			size_t start = 0;

			for (auto end = r.text.find('\n'); start <= r.text.size(); end = r.text.find('\n', start))
			{
				const auto line = std::string_view(r.text).substr(start, end == std::string::npos ? end : end - start);
				if (!line.empty()) ui::send_text(frame, line);
				if (end == std::string::npos) break;
				ui::send_key(frame, platform::keys::RETURN, {});
				start = end + 1;
			}
		}
		else if (r.action == "key")
		{
			// The keyboard goes to it as Tab takes it there, then the key is pressed.
			if (target.control) target.control->focus();
			else if (target.area && target.element && app._view_frame)
			{
				const keyboard_part part{target.element, target.part, target.bounds};
				app._view_frame->keyboard_to(*target.area, &part);
			}
			else if (target.kind == "content")
			{
				if (app._view_frame && frame == app._view_frame->frame()) app.focus_view();
				else if (frame) frame->focus();
			}

			ui::send_key(frame, r.key, r.keys);
		}

		// What it did is looked at again where the pointer is now, as the platform's own pointer would be.
		if (r.action != "key" && r.action != "right_click") move(r.action == "drag" ? at + r.drag : at);
		return {};
	}

	// Gives the input as the call's job, in a task of its own, so a dialog or menu it opens runs its loop there. The
	// call is answered as soon as one opens, or once the window has caught up with what the input did.
	void act(mcp_host& host, const core::mcp::call_ptr& call, act_request request)
	{
		auto& app = host.app();
		const auto job = host.start_job(request.tool);
		const auto before = modal_now();

		host.when([job, before] { return job->is_done() || opened_since(before); }, 0.0, mcp_host::longest_wait_seconds,
		          [&app, job, request](const bool met)
		{
			if (met && !job->is_done()) job->complete(act_answer(app, request, true, {}));
		});

		platform::run_ui([&host, &app, job, request]
		{
			const auto failed = perform(host, app, request);

			if (!failed.empty())
			{
				if (!job->is_done()) job->complete(act_answer(app, request, false, failed));
				return;
			}

			host.when([&app] { return window_caught_up(app); }, caught_up_quiet_s, caught_up_timeout_s,
			          [&host, &app, job, request](bool)
			{
				// Work the input left a dialog showing - a file being saved - is part of it, done once that has gone.
				host.when([] { return dialogs_showing_work().empty(); }, 0.0, mcp_host::longest_wait_seconds,
				          [&app, job, request](bool)
				{
					if (!job->is_done()) job->complete(act_answer(app, request, false, {}));
				});
			});
		});

		host.wait(call, job, mcp_host::wait_seconds(*call), false);
	}

	// ctrl+, shift+ and alt+ before a key: a letter, a digit, or a key's name, as a script's key step takes it.
	bool parse_key_chord(const std::string_view chord, char32_t& key, platform::key_state& keys)
	{
		return parse_tutorial_key(chord, key, keys);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// The tools
	///////////////////////////////////////////////////////////////////////////////////////////////////

	void add_ui_controls(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "ui_controls";
		t.title = "List what can be worked";
		t.description =
			"Everything in the window a user can work now, each with an id ui_act takes: the title and status bars' "
			"buttons, boxes and slider; the view's links and parts - the sidebar's, the listing's and the media "
			"pane's - its panel's controls, the view itself and the strip; the dialog in front; and the open menu's "
			"rows. Each says its scope, its kind - button, check, radio, edit, slider, date, link, part, area (a "
			"picture, chart or map, only pointed into), content or item (a menu's) - what it says, whether it can be "
			"used now and why not, its value or whether it is checked, the command it runs and what that needs, "
			"where its window draws it, and for a menu's row its path, which menu_respond takes. scope, kind and "
			"contains narrow the list.";
		t.input_schema =
			R"({"type":"object","properties":{"scope":{"type":"string","description":"title, status, sidebar, listing, media, panel, view, strip, dialog or menu"},"kind":{"type":"string"},"contains":{"type":"string","description":"Only those whose id or command has these letters"},"enabled_only":{"type":"boolean"},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":0,"maximum":5000,"description":"How many to name; 500 by default"}},"additionalProperties":false})";
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto scope = call->text("scope");
			const auto kind = call->text("kind");
			const auto contains = call->text("contains");
			const auto enabled_only = call->flag("enabled_only", false);
			const auto offset = static_cast<size_t>(call->number("offset").value_or(0));
			const auto limit = static_cast<size_t>(call->number("limit").value_or(500));

			app.update_button_state(false);
			const auto targets = window_targets(app);
			std::vector<const agent_target*> matching;

			for (const auto& target : targets)
			{
				if (!scope.empty() && core::icmp(target.scope, scope) != 0) continue;
				if (!kind.empty() && core::icmp(target.kind, kind) != 0) continue;
				if (enabled_only && !target.enabled) continue;

				if (!contains.empty())
				{
					const auto id = command_of(target.command);
					const auto in_command = id && core::ifind(command_name(*id), contains) != std::string_view::npos;
					if (core::ifind(target.id, contains) == std::string_view::npos && !in_command) continue;
				}

				matching.push_back(&target);
			}

			const auto first = std::min(offset, matching.size());
			const auto last = std::min(matching.size(), first + limit);
			json_writer json;
			json.object().key("controls").array();
			std::string text;

			for (auto i = first; i < last; ++i)
			{
				write_target(json, *matching[i]);
				text += "\n" + target_text(*matching[i]);
			}

			json.end_array().integer("count", static_cast<int64_t>(matching.size()));
			json.integer("offset", static_cast<int64_t>(first)).boolean("more", last < matching.size());
			json.boolean("menu_open", ui::open_menu_session() != nullptr);
			json.boolean("dialog_open", dialog_awaits_answer()).end_object();

			auto head = std::format("{} of {} shown{}", last - first, matching.size(),
			                        last < matching.size() ? std::format("; offset {} reads on", last) : ""s);
			call->finish(structured_result(head + text, json.str()));
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	void add_ui_act(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "ui_act";
		t.title = "Work a control";
		t.description =
			"Works what ui_controls lists, by its id, with the input a user gives it - sent to its window as the "
			"platform's own input is, so it takes the path a user's does, through whatever is in front: press (the "
			"default), double_click, right_click (which opens its context menu), hover, drag by dx and dy pixels, "
			"wheel by notches (positive away from the user, up), type text into a box - pressed into, its text "
			"selected and replaced, Enter for each new line - or key, a key pressed with the keyboard taken there "
			"first: ctrl+a, escape, f2. One scrolled out of sight is scrolled into view first. What it runs needs "
			"what its command needs. Answers once the window has caught up, or as soon as a dialog or menu opens: "
			"dialog_respond and menu_respond answer those, and ui_act works them too.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"id":{{"type":"string"}},"action":{{"type":"string","enum":["press","double_click","right_click","hover","drag","wheel","type","key"]}},"text":{{"type":"string"}},"key":{{"type":"string","description":"ctrl+, shift+ and alt+ before a letter, a digit, or enter, escape, space, tab, backspace, delete, insert, left, right, up, down, home, end, page-up, page-down, plus, minus, apps or f1 to f12"}},"dx":{{"type":"integer"}},"dy":{{"type":"integer"}},"notches":{{"type":"integer","minimum":-50,"maximum":50}},"ctrl":{{"type":"boolean"}},"shift":{{"type":"boolean"}},{}}},"required":["id"],"additionalProperties":false}})",
			wait_property);
		t.annotations.destructive = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto refuse = [&call](std::string why)
			{
				call->finish(core::mcp::tool_result::error(std::move(why)));
			};

			if (app._tutorial) return refuse("Steps are being acted out: wait for their job, or job_cancel it");

			act_request request;
			request.tool = "ui_act";
			request.id = call->text("id");
			request.action = call->text("action", "press");
			request.text = call->text("text");
			request.drag = {static_cast<int>(call->number("dx").value_or(0)),
			                static_cast<int>(call->number("dy").value_or(0))};
			request.notches = static_cast<int>(call->number("notches").value_or(-1));
			request.keys.control = call->flag("ctrl", false);
			request.keys.shift = call->flag("shift", false);

			if (request.action == "key" && !parse_key_chord(call->text("key"), request.key, request.keys))
				return refuse(std::format("'{}' is no key: ctrl+a, escape, f2 and the like", call->text("key")));
			if (request.action == "drag" && request.drag == pointi{})
				return refuse("Say how far to drag it: dx, dy or both, in pixels");

			app.update_button_state(false);
			const auto targets = window_targets(app);
			const auto* const target = find_target(targets, request.id);

			if (!target)
			{
				const auto like = similar_ids(targets, request.id);
				return refuse(std::format("Nothing shown now is {}{}", request.id,
				                          like.empty() ? ": ui_controls lists what is"s
				                                       : std::format(". Perhaps: {}", join_text(like, ", "))));
			}

			const auto pointing = request.action == "hover" || request.action == "wheel";

			if (!pointing && !target->enabled)
			{
				return refuse(std::format("{} is disabled{}", target->id,
				                          target->why_disabled.empty() ? "" : ": " + target->why_disabled));
			}

			if (request.action == "type" && target->kind != "edit")
				return refuse(std::format("{} is a {}, not a box to type into", target->id, target->kind));

			// What a press runs needs what its command needs.
			if (!pointing)
			{
				if (const auto id = command_of(target->command))
				{
					if (auto why = agent_command_refusal(host, *id); !why.empty()) return refuse(std::move(why));
				}
			}

			if (dialog_awaits_answer() && target->scope != "dialog" && target->scope != "menu")
				return refuse("A dialog holds the window, and input waits behind it: work the dialog first");

			act(host, call, std::move(request));
		};
		host.add(std::move(t), tool_tier::write, true);
	}

	void add_menu_respond(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "menu_respond";
		t.title = "Choose from a menu";
		t.description =
			"Chooses from the menu open now, as ui_state shows it: a row by its text - Name - or by its path from the "
			"first level down - Sort by > Name - each submenu on the way opened as a press opens it, the row pressed "
			"where the menu shows it; or cancels the menu, as Escape does. A row named alone is looked for in the "
			"deepest level open first. What is chosen needs what its command needs - a row made for what the menu is "
			"on can change anything, so needs -mcp-allow:write - and cancelling needs nothing. command_run opens a "
			"command's menu, and ui_act's right_click a context menu. Answers once the menu has closed, with what the "
			"window shows then, or with the dialog or menu the choice opened.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"choose":{{"type":"string","description":"A row's text, or a path of them joined by >"}},"cancel":{{"type":"boolean"}},{}}},"additionalProperties":false}})",
			wait_property);
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			const auto refuse = [&call](std::string why)
			{
				call->finish(core::mcp::tool_result::error(std::move(why)));
			};

			const auto* const session = ui::open_menu_session();

			if (!session || session->done())
			{
				return refuse("No menu is open: command_run opens a command's menu - menu_main and the like - and "
				              "ui_act's right_click a context menu");
			}

			act_request request;
			request.tool = "menu_respond";
			request.cancel = call->flag("cancel", false);
			const auto choose = call->text("choose");

			if (request.cancel == !choose.empty()) return refuse("Choose a row, or cancel: one of them");

			for (auto rest = std::string_view(choose); !rest.empty();)
			{
				const auto split = rest.find('>');
				const auto row = core::trim(rest.substr(0, split));
				if (!row.empty()) request.path.emplace_back(row);
				rest = split == std::string_view::npos ? std::string_view{} : rest.substr(split + 1);
			}

			if (!request.cancel && request.path.empty()) return refuse("Name the row to choose");
			request.id = request.cancel ? "menu" : join_text(request.path, " > ");
			act(host, call, std::move(request));
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	void add_coverage(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "coverage";
		t.title = "What has been exercised";
		t.description =
			"What the session has exercised: how often each command has run and each command's menu opened - from its "
			"button, a menu, its keys, command_run or ui_act - and the commands that have not, each with whether it "
			"can run now, why not, and what it needs; and of everything ui_controls has listed, what has been used - "
			"by ui_act, menu_respond or any input that reached it - and what has not. reset starts counting again.";
		t.input_schema =
			R"({"type":"object","properties":{"reset":{"type":"boolean"},"list":{"type":"integer","minimum":0,"maximum":5000,"description":"How many of what has not been exercised to name; 200 by default"}},"additionalProperties":false})";
		t.annotations.read_only = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			auto& record = usage();
			const auto listed_most = static_cast<size_t>(call->number("list").value_or(200));

			if (call->flag("reset", false))
			{
				record.runs.clear();
				record.menus.clear();
				record.listed.clear();
				record.used.clear();
				record.used_unlisted.clear();
				call->finish(structured_result("Coverage starts again from nothing.", R"({"reset":true})"));
				return;
			}

			app.update_button_state(false);
			json_writer json;
			json.object().key("commands").object();
			auto total = 0;
			auto exercised = 0;
			std::vector<std::string> not_run_text;
			json.key("run").array();

			for (const auto id : all_commands())
			{
				const auto command = app.find_command(id);
				if (!command) continue;
				++total;

				const auto runs = record.runs.contains(id) ? record.runs.at(id) : 0;
				const auto menus = record.menus.contains(id) ? record.menus.at(id) : 0;
				if (runs == 0 && menus == 0) continue;
				++exercised;
				json.object().text("name", command_name(id)).integer("runs", runs).integer("menus", menus).end_object();
			}

			json.end_array().key("not_run").array();

			for (const auto id : all_commands())
			{
				const auto command = app.find_command(id);
				if (!command || record.runs.contains(id) || record.menus.contains(id)) continue;

				if (not_run_text.size() < listed_most)
				{
					const auto need = agent_command_need(id);
					json.object().text("name", command_name(id)).boolean("enabled", command->enable);
					if (!command->enable && !command->disabled_reason.empty())
						json.text("why_disabled", command->disabled_reason);
					if (!need.empty()) json.text("needs", need);
					json.end_object();
					not_run_text.push_back(
						std::format("{}{}{}", command_name(id), need.empty() ? ""s : std::format(" [{}]", need),
					                command->enable
					                    ? ""s
					                    : std::format(" (disabled{})", command->disabled_reason.empty()
					                                                       ? ""s
					                                                       : ": " + command->disabled_reason)));
				}
			}

			json.end_array().integer("total", total).integer("exercised", exercised).end_object();

			std::vector<std::string> not_used;
			json.key("controls").object().key("not_used").array();

			for (const auto& [id, where] : record.listed)
			{
				if (record.used.contains(id)) continue;
				if (not_used.size() < listed_most)
				{
					json.text(id);
					not_used.push_back(id);
				}
			}

			const auto used = std::ranges::count_if(record.listed, [&](const auto& entry)
			{ return record.used.contains(entry.first); });
			json.end_array().integer("listed", static_cast<int64_t>(record.listed.size()));
			json.integer("used", static_cast<int64_t>(used)).key("used_unlisted").array();
			for (const auto& [what, count] : record.used_unlisted)
				json.object().text("what", what).integer("uses", count).end_object();
			json.end_array().end_object().end_object();

			auto text = std::format("Commands: {} of {} exercised. Controls: {} of {} listed used.", exercised, total,
			                        used, record.listed.size());
			if (!not_run_text.empty()) text += "\nNot run: " + join_text(not_run_text, ", ");
			if (!not_used.empty()) text += "\nNot used: " + join_text(not_used, ", ");
			call->finish(structured_result(std::move(text), json.str()));
		};
		host.add(std::move(t), tool_tier::read, true);
	}
}

std::string describe_menu_json()
{
	const auto* const session = ui::open_menu_session();
	if (!session || session->done()) return "null";

	json_writer json;
	json.object().key("levels").array();

	for (const auto& level : session->levels())
	{
		json.object().integer("opened_from", level.opened_from).key("rows").array();

		const auto write_row = [&json](const ui::menu_row_state& row)
		{
			json.object().text("text", row.text);
			if (!row.keys.empty()) json.text("keys", row.keys);
			json.boolean("enabled", row.enabled);
			if (row.checked) json.boolean("checked", true);
			if (row.submenu) json.boolean("submenu", true);
			if (const auto id = command_of(row.command)) json.text("command", command_name(*id));
			json.end_object();
		};

		for (const auto& row : level.rows)
		{
			switch (row.type)
			{
			case ui::menu_row_state::kind::item: write_row(row); break;
			case ui::menu_row_state::kind::separator: json.object().boolean("separator", true).end_object(); break;
			case ui::menu_row_state::kind::content: json.object().boolean("content", true).end_object(); break;
			case ui::menu_row_state::kind::buttons:
				json.object().key("buttons").array();
				for (const auto& cell : row.cells) write_row(cell);
				json.end_array().end_object();
				break;
			}
		}

		json.end_array().end_object();
	}

	json.end_array().end_object();
	return json.str();
}

std::string describe_menu_text()
{
	const auto* const session = ui::open_menu_session();
	if (!session || session->done()) return {};

	const auto levels = session->levels();
	std::string text = "\nMenu (menu_respond chooses from it):";

	for (size_t l = 0; l < levels.size(); ++l)
	{
		const auto& level = levels[l];
		if (l > 0 && level.opened_from >= 0 && static_cast<size_t>(level.opened_from) < levels[l - 1].rows.size())
			text += std::format("\n  {} >", levels[l - 1].rows[level.opened_from].text);

		const auto indent = std::string(2 * (l + 1), ' ');

		for (const auto& row : level.rows)
		{
			const auto describe = [](const ui::menu_row_state& r)
			{
				return std::format("{}{}{}{}{}", r.checked ? "[x] " : "", r.text,
				                   r.keys.empty() ? ""s : std::format(" ({})", r.keys), r.submenu ? " >" : "",
				                   r.enabled ? "" : " (disabled)");
			};

			switch (row.type)
			{
			case ui::menu_row_state::kind::item: text += "\n" + indent + describe(row); break;
			case ui::menu_row_state::kind::separator: text += "\n" + indent + "---"; break;
			case ui::menu_row_state::kind::content: break;
			case ui::menu_row_state::kind::buttons:
			{
				std::vector<std::string> cells;
				for (const auto& cell : row.cells) cells.push_back("[" + describe(cell) + "]");
				text += "\n" + indent + join_text(cells, " ");
			}
			break;
			}
		}
	}

	return text;
}

bool open_command_menu(app_frame& app, const ui::command_ptr& command)
{
	if (!command || !command->menu) return false;

	// As a press on the command's button opens it, where a bar shows one.
	for (const auto& target : window_targets(app))
	{
		if (target.command != command || target.kind != "button" || !target.enabled) continue;

		const auto left = platform::pointer_button::left;
		const auto at = target.bounds.center();
		ui::send_pointer(target.frame, ui::pointer_input::move, left, at);
		if (send_click(target.frame, at, left, {})) return true;
	}

	const auto& view = app._view_frame;
	if (!view) return false;
	const auto middle = view->frame()->client_to_screen(view->_view_bounds.center());
	app.track_menu(view->frame(), recti(middle, sizei{}), command->menu());
	return true;
}

void track_command_use(app_frame& app)
{
	auto& record = usage();
	if (record.tracking || app._commands.empty()) return;
	record.tracking = true;

	for (const auto& [id, command] : app._commands)
	{
		const auto counted = id;

		if (command->invoke)
		{
			// The session's tiers hold however a command is reached: a key, a toolbar's button, a menu row or a step.
			// Exit stays the watching user's way to close the window - the agent's own tools refuse it before this.
			command->invoke = [&app, counted, run = std::move(command->invoke)]
			{
				if (app._mcp && counted != commands::exit)
				{
					if (const auto why = agent_command_refusal(*app._mcp, counted); !why.empty())
					{
						core::log("track_command_use", why);
						return;
					}
				}

				++usage().runs[counted];
				run();
			};
		}

		if (command->menu)
		{
			command->menu = [counted, open = std::move(command->menu)]
			{
				++usage().menus[counted];
				return open();
			};
		}
	}

	ui::on_control_used([](const ui::control& c, const ui::command_ptr& command)
	{
		auto& used = usage();
		const auto toolbar = dynamic_cast<const ui::toolbar*>(&c) != nullptr;

		if (const auto found = used.ids.find({&c, toolbar ? command.get() : nullptr}); found != used.ids.end())
		{
			++used.used[found->second];
		}
		else
		{
			const auto name = command ? short_label(command->text) : std::string{};
			++used.used_unlisted[name.empty() ? type_label(c) : std::format("{} {}", type_label(c), name)];
		}
	});
}

void add_control_tools(mcp_host& host)
{
	add_ui_controls(host);
	add_ui_act(host);
	add_menu_respond(host);
	add_coverage(host);
}
