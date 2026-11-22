// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The agent tools over faces (app_mcp.h): faces_state reads face search, the face book, the Faces view's
// people and the faces on the selected page, and the faces on a picture the window shows, each with where it is as
// ui_input's `point at` takes it, so a session can review faces as a user does; faces_select selects people
// and faces in the Faces view; and faces_data saves the face book to a CSV file or reads one into it.

#include "pch.h"

#include "app_mcp.h"
#include "app_mcp_json.h"
#include "model_db.h"
#include "model_faces_search.h"
#include "model_index.h"
#include "model.h"
#include "ui_view.h"
#include "ui_dialog.h"
#include "ui_controls.h"
#include "view_faces.h"
#include "app.h"

namespace
{
	constexpr double caught_up_quiet_s = 0.05;
	constexpr double caught_up_timeout_s = 5.0;
	// Face search publishes what it read in batches, so it must stay settled a moment to have finished.
	constexpr double settle_quiet_s = 0.5;

	bool window_caught_up(const app_frame& app)
	{
		return app._invalids.load() == view_invalid::none;
	}

	// Face search has settled, and the window - the Faces view where it shows - shows what it found.
	bool faces_caught_up(const app_frame& app)
	{
		if (!window_caught_up(app) || !app._state.item_index.faces().is_settled()) return false;
		return app._state.view_mode() != view_type::faces || !app._view_faces || app._view_faces->is_current();
	}

	// Where a point of the view frame is, as fractions of the window - ui_input's `point at` - or none outside it.
	std::optional<pointd> window_fraction(const app_frame& app, const pointi view_point)
	{
		if (!app._view_frame || app._extent.cx <= 0 || app._extent.cy <= 0) return {};
		const auto origin = app._app_frame->client_to_screen({0, 0});
		const auto at = app._view_frame->frame()->client_to_screen(view_point);
		const pointd fraction{(at.x - origin.x) / static_cast<double>(app._extent.cx),
		                      (at.y - origin.y) / static_cast<double>(app._extent.cy)};
		if (fraction.X < 0.0 || fraction.Y < 0.0 || fraction.X > 1.0 || fraction.Y > 1.0) return {};
		return fraction;
	}

	void write_at(json_writer& json, const app_frame& app, const recti bounds)
	{
		json.key("at");
		const auto at = bounds.is_empty() ? std::nullopt : window_fraction(app, bounds.center());
		if (!at)
		{
			json.null();
			return;
		}
		json.array().number(std::round(at->X * 10000.0) / 10000.0).number(std::round(at->Y * 10000.0) / 10000.0);
		json.end_array();
	}

	// Where a rect of the view frame is, as fractions of the window: its left, top, right and bottom.
	void write_box(json_writer& json, const app_frame& app, const recti bounds)
	{
		json.key("box");
		const auto top_left = bounds.is_empty() ? std::nullopt : window_fraction(app, pointi(bounds.left, bounds.top));
		const auto bottom_right = bounds.is_empty() ? std::nullopt
		                                            : window_fraction(app, pointi(bounds.right, bounds.bottom));
		if (!top_left || !bottom_right)
		{
			json.null();
			return;
		}
		json.array();
		for (const auto v : {top_left->X, top_left->Y, bottom_right->X, bottom_right->Y})
			json.number(std::round(v * 10000.0) / 10000.0);
		json.end_array();
	}

	std::string_view row_kind_name(const faces_view::row::kind kind)
	{
		switch (kind)
		{
		case faces_view::row::kind::person: return "person";
		case faces_view::row::kind::file_person: return "file_person";
		case faces_view::row::kind::hidden_faces: return "hidden";
		case faces_view::row::kind::surname: return "surname";
		case faces_view::row::kind::unknown: return "unknown";
		}
		return {};
	}

	std::string_view member_name(const faces::member_kind kind)
	{
		switch (kind)
		{
		case faces::member_kind::confirmed: return "confirmed";
		case faces::member_kind::named: return "named";
		case faces::member_kind::suggested: return "suggested";
		case faces::member_kind::unknown: return "unknown";
		}
		return {};
	}

	constexpr std::array face_commands{commands::face_follow_placement,
	                                   commands::face_undo_placement,
	                                   commands::face_rename_person,
	                                   commands::face_reject,
	                                   commands::face_name,
	                                   commands::face_confirm,
	                                   commands::face_hide,
	                                   commands::face_show_again,
	                                   commands::face_search,
	                                   commands::face_person_search,
	                                   commands::face_open_in_folder,
	                                   commands::face_export,
	                                   commands::face_import};

	// The faces on the picture the window shows, where it shows one whole, upright and flat: whom each is, and where,
	// as the pointer finds them to offer their buttons (docs/faces.md#93-faces-on-a-picture). A video's faces are each
	// on the frame they were found in, which says whether it is the frame shown, paused.
	void write_picture(json_writer& json, std::string& text, const app_frame& app)
	{
		const auto& display = app._state._display;
		const auto video = display && display->is_one() && display->_item1 && display->_selected_texture1 &&
		                   display->_item1->file_type()->group == &file_group::video;
		const auto placeable = display && (video ? !display->is_panorama_projected() : can_place_faces(*display));
		const auto store = placeable ? faces::active_store() : faces::face_store_const_ptr{};
		if (!store) return;

		const auto path = display->_item1->path();
		const auto image = rectd(display->_selected_texture1->display_bounds().offset(display->media_offset));
		const auto shown = faces::faces_on_picture(*store, faces::active_overview().get(),
		                                           app._state.item_index.faces().book(), path);
		json.key("picture").object().text("path", path.str()).key("faces").array();
		text += std::format("\nFaces on {}:{}", path.name(), shown.empty() ? " none"sv : ""sv);

		for (const auto& f : shown)
		{
			const auto position = store->entry(f.face).position + 1;
			const recti bounds(core::round(image.X + f.box.X * image.Width),
			                   core::round(image.Y + f.box.Y * image.Height),
			                   core::round(image.X + f.box.right() * image.Width),
			                   core::round(image.Y + f.box.bottom() * image.Height));
			json.object().integer("position", position).text("name", f.name).boolean("offered", f.offered);
			json.integer("pictures", f.pictures).integer("suggested", f.suggested);
			json.text("term", faces::format_face_term(path.name(), position));
			if (f.frame_ms)
			{
				const auto on_frame = !display->is_playing_media() &&
				                      std::abs(display->media_pos() * 1000.0 - *f.frame_ms) <= 1.0;
				json.number("time", *f.frame_ms / 1000.0).boolean("on_frame", on_frame);
				const auto& seen = store->run_of_face(f.face).seen;
				json.key("seen").array();
				if (static_cast<size_t>(position - 1) < seen.size())
					for (const auto& s : seen[position - 1])
						json.array().number(s.start / 1000.0).number(s.end / 1000.0).end_array();
				json.end_array();
			}
			write_at(json, app, bounds);
			// Its buttons are drawn centred under its bottom edge once the pointer is over it.
			write_box(json, app, bounds);
			json.end_object();
			text += std::format("\n  {}: {}{}{}", position, f.name.empty() ? "unnamed"sv : std::string_view(f.name),
			                    f.offered ? "?"sv : ""sv,
			                    f.frame_ms ? std::format(" at {:.3f}s", *f.frame_ms / 1000.0) : std::string{});
		}

		json.end_array().end_object();
	}

	core::mcp::tool_result faces_answer(app_frame& app, const size_t listed, const size_t offset)
	{
		auto& faces = app._state.item_index.faces();
		const auto mode = app._state.view_mode();
		json_writer json;
		json.object().text("view", agent_view_name(mode));

		const auto progress = faces.progress();
		const auto settled = faces.is_settled();
		json.key("face_search").object().boolean("enabled", faces.is_enabled()).boolean("paused", faces.is_paused());
		json.boolean("photos", faces.reads_photos()).boolean("videos", faces.reads_videos());
		json.integer("pictures", progress.eligible).integer("read", progress.done).integer("left", progress.left());
		json.integer("faces", progress.faces)
			.integer("unreadable", progress.unreadable)
			.integer("waiting", progress.waiting);
		json.boolean("ready", faces.current_overview() != nullptr).boolean("settled", settled).end_object();

		auto text = std::format("Face search is {}{}{}: {} of {} read, {} faces found{}.",
		                        faces.is_enabled() ? "on" : "off",
		                        !faces.is_enabled()     ? ""sv
		                        : !faces.reads_videos() ? " for photos"sv
		                        : !faces.reads_photos() ? " for videos"sv
		                                                : " for photos and videos"sv,
		                        faces.is_paused() ? ", paused" : "", progress.done, progress.eligible, progress.faces,
		                        settled ? ""sv : "; still reading or preparing suggestions"sv);

		const auto& book = faces.book();
		const auto roster = book.people();
		const auto keeping = [state = faces.book_state()]
		{
			switch (state)
			{
			case faces::book_keeping::kept: return "kept"sv;
			case faces::book_keeping::unsaved: return "unsaved"sv;
			case faces::book_keeping::shared: return "shared"sv;
			case faces::book_keeping::memory: return "memory"sv;
			case faces::book_keeping::opening: break;
			}
			return "opening"sv;
		}();
		json.key("book").object().text("path", faces.book_path().str()).boolean("kept", faces.is_book_kept());
		json.text("keeping", keeping);
		json.integer("marks", static_cast<int64_t>(book.mark_count())).key("people").array();
		for (const auto& p : roster->people)
		{
			json.object().integer("id", p.id).text("name", p.name).end_object();
		}
		json.end_array().end_object();
		const auto kept_how = [state = faces.book_state()]
		{
			switch (state)
			{
			case faces::book_keeping::kept: return ""sv;
			case faces::book_keeping::unsaved: return ", its latest changes not saved yet"sv;
			case faces::book_keeping::shared: return ", read only: another window writes it"sv;
			case faces::book_keeping::memory: return ", in memory only"sv;
			case faces::book_keeping::opening: break;
			}
			return ", still opening"sv;
		}();
		text += std::format("\nThe face book keeps {} people and {} marks{}.", roster->people.size(), book.mark_count(),
		                    kept_how);

		const auto& view = app._view_faces;

		if (mode == view_type::faces && view)
		{
			const auto& rows = view->rows();
			const auto selected_rows = view->selected_rows();
			json.key("people").array();
			text += "\nPeople:";

			for (size_t i = 0; i < rows.size(); ++i)
			{
				const auto& r = rows[i];
				const auto selected = std::ranges::find(selected_rows, static_cast<int>(i)) != selected_rows.end();
				json.object()
					.integer("row", static_cast<int64_t>(i))
					.text("key", r.key)
					.text("kind", row_kind_name(r.what));
				json.text("title", r.title).text("detail", r.detail).boolean("selected", selected);
				json.integer("book_person", r.book_person);
				write_at(json, app, view->row_bounds(static_cast<int>(i)));
				json.end_object();
				text += std::format("\n  {}{} {} [{}] {}", selected ? "*" : " ", i, r.title, row_kind_name(r.what),
				                    r.detail);
			}

			json.end_array();
			json.text("target", view->faces_targeted() ? "faces" : "people");
			text += std::format("\nCommands act on the {} selected.", view->faces_targeted() ? "faces" : "people");

			const auto& cells = view->cells();
			const auto& model = view->model();
			const auto store = model ? model->store : faces::face_store_const_ptr{};
			const auto last = std::min(cells.size(), offset + listed);
			json.key("faces")
				.object()
				.text("title", view->faces_title())
				.integer("count", static_cast<int64_t>(cells.size()));

			// How each face shown came to be there, counted over every one, not just the page listed.
			std::map<std::string_view, int64_t> kinds;
			for (const auto& c : cells) ++kinds[member_name(c.member)];
			json.key("members").object();
			for (const auto& [kind, n] : kinds) json.integer(kind, n);
			json.end_object();

			// docs/faces.md#9-the-faces-view: the headings the faces are listed under, each before the cell it starts at,
			// with the buttons at their ends and where each is, for ui_input to press.
			std::string heading_text;
			json.key("headings").array();
			for (const auto& h : view->headings())
			{
				json.object().text("text", h.text).integer("before", static_cast<int64_t>(h.before));
				json.boolean("confirm_all", h.has(faces_view::heading_action::confirm_all));
				json.key("buttons").array();
				for (const auto& b : h.buttons)
				{
					json.object().text("label", view->heading_action_text(b.action));
					write_at(json, app, view->heading_button_bounds(b));
					json.end_object();
				}
				json.end_array().end_object();
				heading_text += std::format("\n  [{}] from cell {}", h.text, h.before);
				for (const auto& b : h.buttons)
					heading_text += std::format(" [{}]", view->heading_action_text(b.action));
			}
			json.end_array();

			json.integer("offset", static_cast<int64_t>(offset)).key("items").array();
			text += view->faces_title().empty()
			            ? "\nFaces: none shown"s
			            : std::format("\nFaces: {} - {} shown", view->faces_title(), cells.size());
			text += heading_text;

			for (auto i = offset; i < last; ++i)
			{
				const auto& c = cells[i];
				const auto selected = view->is_cell_selected(static_cast<int>(i));
				json.object().integer("cell", static_cast<int64_t>(i)).text("file", c.shown.path.str());

				if (c.shown.face && store && *c.shown.face < store->face_count())
				{
					const auto& entry = store->entry(*c.shown.face);
					const auto position = entry.position + 1;
					json.integer("position", position);
					json.text("term", faces::format_face_term(c.shown.path.name(), position));
					// docs/faces.md#33-videos: in a video, the frame it was found in, in seconds.
					if (c.shown.frame_ms) json.number("time", *c.shown.frame_ms / 1000.0);
					// docs/faces.md#44-quality: the measures a face is judged clear by, and the judgment.
					json.key("quality").object().integer("confidence", entry.confidence);
					json.integer("eye_distance", entry.eye_distance);
					json.number("frontal", std::round(entry.frontal / 255.0 * 100.0) / 100.0);
					json.boolean("clear", entry.has_hash && entry.is_good()).end_object();
				}

				json.text("member", member_name(c.member)).number("score", std::round(c.score * 1000.0) / 1000.0);
				json.boolean("selected", selected).boolean("hidden", c.hidden);
				write_at(json, app, view->cell_bounds(static_cast<int>(i)));
				// docs/faces.md#92-faces: a suggestion's shortcuts, and where each is, for ui_input to press.
				if (const auto shortcuts = view->suggestion_buttons(static_cast<int>(i)); !shortcuts.empty())
				{
					json.key("shortcuts").array();
					for (const auto& s : shortcuts)
					{
						json.object().text("label", view->suggestion_action_text(s.action));
						write_at(json, app, s.bounds);
						json.end_object();
					}
					json.end_array();
				}
				json.end_object();
				text += std::format("\n  {}{} {} {}", selected ? "*" : " ", i, c.caption, member_name(c.member));
			}

			json.end_array().end_object();
			if (last < cells.size())
				text += std::format("\n  ... {} more: offset {} reads on", cells.size() - last, last);

			app.update_button_state(false);
			json.key("commands").array();
			std::string enabled;
			for (const auto id : face_commands)
			{
				const auto command = app.find_command(id);
				if (!command) continue;
				json.object().text("name", command_name(id)).text("label", command->text);
				json.boolean("enabled", command->enable).end_object();
				if (command->enable) enabled += std::format(" {}", command_name(id));
			}
			json.end_array();
			text += "\nCommands that can run:" + (enabled.empty() ? " none"s : enabled);
		}
		else
		{
			write_picture(json, text, app);
		}

		json.end_object();
		return structured_result(std::move(text), json.str());
	}

	size_t size_of(const core::mcp::call& call, const std::string_view name, const size_t fallback)
	{
		const auto n = call.number(name);
		return n && *n >= 0.0 ? static_cast<size_t>(*n) : fallback;
	}

	void add_faces_state(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "faces_state";
		t.title = "What face search and the Faces view show";
		t.description =
			"Face search - on or off, how far its reading has got, whether it has settled - and the face book: the "
			"named people and how many faces the user has marked. In Faces, lists the People rows, Unknown and Hidden, "
			"and the current page's faces, their file, identity evidence, selection and screen position. Also lists "
			"the available commands. With a picture shown instead, lists its faces and where each is. wait_s waits for "
			"the scan and suggestions to settle.";
		t.input_schema =
			R"({"type":"object","properties":{"faces":{"type":"integer","minimum":0,"maximum":500,"description":"How many faces to list; 50 by default"},"offset":{"type":"integer","minimum":0,"description":"The first face to list"},"wait_s":{"type":"number","minimum":0,"maximum":600,"description":"How long to wait for face search to settle; 0, the default, answers at once"}},"additionalProperties":false})";
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto listed = size_of(*call, "faces", 50);
			const auto offset = size_of(*call, "offset", 0);
			const auto wait_s = call->number("wait_s").value_or(0.0);

			if (wait_s <= 0.0)
			{
				call->finish(faces_answer(app, listed, offset));
				return;
			}

			host.wait_until(call, "faces_state", [&app] { return faces_caught_up(app); }, settle_quiet_s, wait_s,
			                [&app, listed, offset](bool) { return faces_answer(app, listed, offset); });
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	void add_faces_select(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "faces_select";
		t.title = "Select people and faces in the Faces view";
		t.description =
			"Selects People rows by index, key or title (including Unknown or Hidden), then faces on that page by "
			"cell index. What is selected is what the review commands act on. Answers with faces_state.";
		t.input_schema =
			R"({"type":"object","properties":{"rows":{"type":"array","items":{"type":"integer","minimum":0}},"people":{"type":"array","items":{"type":"string"},"description":"Keys or titles, as faces_state shows them"},"cells":{"type":"array","items":{"type":"integer","minimum":0}}},"additionalProperties":false})";
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto& view = app._view_faces;

			if (app._state.view_mode() != view_type::faces || !view)
			{
				call->finish(core::mcp::tool_result::error(
					std::format("The window shows the {} view: command_run faces opens the Faces view",
				                agent_view_name(app._state.view_mode()))));
				return;
			}

			const auto& args = call->arguments();
			std::vector<int> rows;
			std::vector<std::string> missing;

			if (const auto found = args.FindMember("rows"); found != args.MemberEnd() && found->value.IsArray())
			{
				for (const auto& r : found->value.GetArray())
					if (r.IsInt() && r.GetInt() < static_cast<int>(view->rows().size())) rows.push_back(r.GetInt());
			}

			for (const auto& name : call->texts("people"))
			{
				const auto& all = view->rows();
				const auto by_key = std::ranges::find(all, name, &faces_view::row::key);
				if (by_key != all.end())
				{
					rows.push_back(static_cast<int>(by_key - all.begin()));
					continue;
				}

				auto any = false;
				for (size_t i = 0; i < all.size(); ++i)
				{
					if (core::icmp(all[i].title, name) == 0)
					{
						rows.push_back(static_cast<int>(i));
						any = true;
					}
				}
				if (!any) missing.push_back(name);
			}

			if (!missing.empty())
			{
				call->finish(core::mcp::tool_result::error(
					std::format("No visible People row is called {}: faces_state lists them",
				                join_text(missing, ", "))));
				return;
			}

			if (!rows.empty()) view->select_rows(rows);

			if (const auto found = args.FindMember("cells"); found != args.MemberEnd() && found->value.IsArray())
			{
				std::vector<int> cells;
				for (const auto& c : found->value.GetArray())
					if (c.IsInt() && c.GetInt() < static_cast<int>(view->cells().size())) cells.push_back(c.GetInt());
				view->select_cells(cells);
			}

			host.wait_until(call, "faces_select", [&app] { return window_caught_up(app); }, caught_up_quiet_s,
			                caught_up_timeout_s, [&app](bool) { return faces_answer(app, 50, 0); });
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	void add_faces_data(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "faces_data";
		t.title = "Save the face book to a file, or read one into it";
		t.description =
			"Saves names and model-specific biometric face signatures with confirmations, exclusions and visibility "
			"(no photo paths or filenames) as one CSV file "
			"at `export`, or reads one at `import` into it, joining people by name; as the Faces view's Export face "
			"data and Import face data do, without the file pickers a session cannot show. Paths are full paths.";
		t.input_schema =
			R"({"type":"object","properties":{"export":{"type":"string"},"import":{"type":"string"}},"additionalProperties":false})";
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			const auto is_export = call->has("export");
			const auto text = call->text(is_export ? "export" : "import");

			if (is_export == call->has("import") || !core::is_path(text))
			{
				call->finish(core::mcp::tool_result::error("Give one full path, to export to or to import from"));
				return;
			}

			const auto job = host.start_job("faces_data");
			auto done = [job](const bool ok, const std::string& message)
			{
				if (!job->is_done())
					job->complete(ok ? core::mcp::tool_result::text(message) : core::mcp::tool_result::error(message));
			};

			auto& state = host.app()._state;
			if (is_export) export_face_book(state, core::file_path(text), std::move(done));
			else import_face_book(state, core::file_path(text), std::move(done));
			host.wait(call, job, 60.0, false);
		};
		host.add(std::move(t), tool_tier::write, true);
	}
}

void add_face_tools(mcp_host& host)
{
	add_faces_state(host);
	add_faces_select(host);
	add_faces_data(host);
}
