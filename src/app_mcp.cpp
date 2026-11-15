// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The agent session's host (app_mcp.h): its options, the jobs long work runs as and the calls that wait on
// them, the window it makes when a tool first needs one, the copies of itself it runs work in, the log lines it keeps,
// the session's own tools - help, status, waiting, diagnostics, the log, jobs and the window - and a single tool called
// from the command line (-tool:).

#include "pch.h"

#include "app_mcp.h"
#include "app_mcp_json.h"
#include "app_text.h"
#include "app_perf.h"
#include "model_db.h"
#include "model_index.h"
#include "model.h"
#include "ui_dialog.h"
#include "app_command_line.h"
#include "app.h"

#include <cstdio>

namespace
{
	// The latest lines the log was written, numbered so a caller can ask for those since a mark.
	struct log_ring
	{
		std::mutex mutex;
		std::deque<std::pair<uint64_t, std::string>> lines;
		uint64_t next = 1;
	};

	constexpr size_t kept_log_lines = 4000;

	log_ring& ring()
	{
		static log_ring r;
		return r;
	}

	void observe_log(const std::string_view line)
	{
		auto text = std::string(line);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();

		auto& r = ring();
		std::lock_guard lock(r.mutex);
		r.lines.emplace_back(r.next++, std::move(text));
		while (r.lines.size() > kept_log_lines) r.lines.pop_front();
	}

	// A job's output is kept to this much: enough for a test run's report, bounded for a render's.
	constexpr size_t kept_output_bytes = 256 * 1024;

	constexpr std::string_view instructions =
		"Diffractor organizes the photos, videos and audio on the user's own disks; this session offers its tools. "
		"Start with status: what the session may do, where its window is, and how the index stands. The library "
		"and the window's tools make the window the first time one is called - out of sight unless the session was "
		"started with -mcp-ui:visible - and load the index, which wait_until_idle waits for. Paths are full paths. "
		"Long work answers with a job when it has not finished within wait_s seconds: job_wait waits on, "
		"job_cancel stops it. A dialog that shows or a menu that opens stops input and interrupts the call: "
		"ui_state shows it, dialog_respond answers a dialog and menu_respond chooses from a menu. ui_controls lists "
		"everything the window offers - its buttons, boxes, links and parts, the dialog in front, the open menu's "
		"rows - which ui_act works by id with the input a user gives; coverage says which commands have run and "
		"which controls have been used. The tasks - Sync, Import, Locate, Tags, Rename, Convert, "
		"Metadata, Adjust Date - open with command_run (tool_sync, tool_import and the like); task_set fills in "
		"their panel, their Analyze command plans the work, task_state reads the plan, and their Run command, which "
		"needs -mcp-allow:write, does it. location_suggest finds, for items taken without a place, the item with one "
		"taken nearest in time, which Locate proposes. help explains the search language, the .dfv movie and .dft "
		"script formats, and what each -mcp-allow tier allows.";

	std::string help_text(const std::string_view topic)
	{
		if (topic == "search")
		{
			std::string scopes;

			for (const auto& s : prop::search_scopes())
			{
				if (s.scope == "any") continue;
				scopes += (scopes.empty() ? "" : ", ") + s.scope;
			}

			return std::format(
				"The search language is the address box's. Words, and \"quoted phrases\", match names and metadata; "
				"scope:value narrows a word to one property, as tag:holiday or camera:canon; @photo, @video, @audio "
				"and @commodore ask for a kind of file; @raw, @hdr, @animated, @transparent, @panorama, @wide, "
				"@portrait, @landscape, @square and @slowmo for how a picture or video was made; and @duplicates for "
				"the files the collection holds more than one copy "
				"of; a number or a date after < or > compares, as rating:>3 or created:>2019, and a date alone, as "
				"2019-07-13, asks for that day; ! or - before a term leaves out what it matches; and, or and "
				"parentheses combine terms; with:<property> and without:<property> ask whether an item has one, as "
				"without:location. A full folder path searches that folder, and ending it with ** searches its "
				"subfolders too. search runs a query without changing the window, in order of path, of when each "
				"item was taken, or of its set of copies; navigate shows it there.\n\nScopes: {}",
				scopes);
		}

		if (topic == "dfv")
		{
			return "A Movie project is a .dfv file: UTF-8 text, one item per line, in the order they play. "
				   "# starts a comment line.\n"
				   "  t <length> ['<text>']            a screen of text on black for <length> seconds\n"
				   "  c <part> <file> ['<caption>'] [!<sound file>]   a clip of the file\n"
				   "  s <part> <file> ['<caption>'] [!<sound file>]   the same, marked to be stabilized\n"
				   "A part is <from>-<to> in seconds (83.5), minutes and seconds (1:23.5) or hours (1:01:23.5); "
				   "either end may be left out, and - alone is the whole file. A photo shows for the part's length: "
				   "-6 is six seconds. #<n> after a part plays tune n of a SID file. A file is a full path or one "
				   "relative to the project's folder. 'single quotes' read \\n as a new line; \"double quotes\" take "
				   "a path as it stands. !sound.wav plays that file in place of the clip's own sound. Check a "
				   "project with movie_check, which names each line it cannot read, and make it with movie_render. "
				   "docs/movie.md has the whole format.";
		}

		if (topic == "dft")
		{
			return "A tutorial script is a .dft file, one step a line; # starts a comment. Steps before the first say "
				   "set the scene; say <id> '<narration>' starts a beat, whose steps run while it is spoken. "
				   "ui_input's steps are a script's steps, with no say.\n"
				   "  cue <name> | wait <seconds> | settle | hide | list | tooltips on|off\n"
				   "  point <anchor> [<seconds>]   the drawn pointer glides there\n"
				   "  click [ctrl] [shift] | double-click | right-click   on what the pointer is on\n"
				   "  drag <anchor> [<seconds>]    pressed where the pointer is, let go at the anchor\n"
				   "  wheel up|down [<notches>]    turned where the pointer is\n"
				   "  type '<text>'                into the address box\n"
				   "  text '<text>'                into whatever has the keyboard\n"
				   "  key <key>                    ctrl+, shift+, alt+ before a letter, digit or enter, escape, space, "
				   "tab, backspace, delete, insert, left, right, up, down, home, end, page-up, page-down, plus, minus, "
				   "apps, f1 to f12\n"
				   "  command <name>               a command, by the name command_list gives\n"
				   "  open ['<folder or search>'] | select '<file name>' [ctrl]\n"
				   "Anchors: address, sidebar, types, map, calendar, month <yyyy>-<mm>, items, preview, view, "
				   "button <command>, tile '<file name>' [<x> <y>], at <x> <y> - x and y are fractions from the top "
				   "left. The pointer and keys are given to the window as its own input, reaching what a user's "
				   "would. A script cannot work a dialog or a menu: dialog_respond and menu_respond answer them.";
		}

		if (topic == "tiers")
		{
			return "A session reads by default. The client's configuration starts it with -mcp-allow: to allow more, "
				   "comma-separated:\n"
				   "  write    change files: movie_render, ui_input, ui_act, tutorial_run, and commands, dialog "
				   "buttons and menu rows that change files\n"
				   "  capture  pictures of the screen and of the user's media: screenshot, record_*, tutorial_run\n"
				   "  network  work that reaches the network: update checks, crash reports, map tiles\n"
				   "  dev      the developer's tools: run_tests, validate_translations, generate_docs, "
				   "duplicate_report\n"
				   "  launch   opening another program, a folder or a web page on the user's desktop: the help, a "
				   "file's folder, a map, Open with. Without it, each is skipped and logged.\n"
				   "-mcp-ui:offscreen or visible makes the window at once, out of sight or on the desktop; a "
				   "recording needs it visible. -profile:<folder> keeps settings and the index apart from the user's, "
				   "and -collection:<folder> makes one folder the whole collection. status says what this session "
				   "was started with.";
		}

		return std::string(instructions) + "\n\nTopics: search, dfv, dft, tiers.";
	}

	std::string_view presence_name(const agent_presence p)
	{
		switch (p)
		{
		case agent_presence::offscreen: return "offscreen";
		case agent_presence::visible: return "visible";
		case agent_presence::none: break;
		}
		return "none";
	}

	std::string_view view_name(const view_type v)
	{
		switch (v)
		{
		case view_type::items: return "items";
		case view_type::media: return "media";
		case view_type::edit: return "edit";
		case view_type::movie: return "movie";
		case view_type::rename: return "rename";
		case view_type::batch: return "batch";
		case view_type::import: return "import";
		case view_type::sync: return "sync";
		case view_type::locate: return "locate";
		case view_type::tags: return "tags";
		case view_type::options: return "options";
		case view_type::none: break;
		}
		return "none";
	}

	// A task that is analyzing or running.
	bool task_running(const app_frame& app)
	{
		return app._view && app._view->progress().active;
	}

	// A task whose review is still being worked out, which its run waits for.
	bool task_reviewing(const app_frame& app)
	{
		return app._view && app._view->reviewing();
	}
}

std::string_view agent_view_name(const view_type v)
{
	return view_name(v);
}

bool agent_window_settled(const app_frame& app)
{
	if (!app._window_started) return true;
	const auto& index = app._item_index;
	return index.is_init_complete() && index.scanning_items == 0 && index.searching == 0 && index.indexing == 0 &&
	       index.thumbnailing_items == 0 && df::loading_media == 0 && app._invalids.load() == view_invalid::none &&
	       !app._tutorial && !task_running(app) && !task_reviewing(app) && dialogs_showing_work().empty();
}

std::vector<std::string> agent_window_busy(const app_frame& app)
{
	std::vector<std::string> busy;
	if (!app._window_started) return busy;
	const auto& index = app._item_index;
	if (!index.is_init_complete()) busy.emplace_back("loading the index");
	if (index.scanning_items != 0) busy.emplace_back("scanning");
	if (index.searching != 0) busy.emplace_back("searching");
	if (index.indexing != 0) busy.emplace_back("indexing");
	if (index.thumbnailing_items != 0) busy.emplace_back("making thumbnails");
	if (df::loading_media != 0) busy.emplace_back("loading media");
	if (app._invalids.load() != view_invalid::none) busy.emplace_back("updating the window");
	if (app._tutorial) busy.emplace_back("acting out steps");

	if (task_running(app))
	{
		const auto name = app._view->operation_name();
		busy.emplace_back(name.empty() ? "running the task"s : std::format("running {}", name));
	}

	if (task_reviewing(app)) busy.emplace_back("working out the review");
	for (auto& work : dialogs_showing_work()) busy.push_back(std::move(work));

	return busy;
}

agent_options parse_agent_options(const std::string_view ui, const std::string_view allow)
{
	agent_options result;

	if (ui.empty() || ui == "none") result.presence = agent_presence::none;
	else if (ui == "offscreen") result.presence = agent_presence::offscreen;
	else if (ui == "visible") result.presence = agent_presence::visible;
	else result.problems.emplace_back(std::format("-mcp-ui:{} is not none, offscreen or visible", ui));

	for (const auto part : core::split(allow, false, [](const char c) { return c == ','; }))
	{
		const auto tier = core::trim(part);
		if (tier.empty()) continue;
		if (tier == "write") result.allow.write = true;
		else if (tier == "capture") result.allow.capture = true;
		else if (tier == "network") result.allow.network = true;
		else if (tier == "dev") result.allow.dev = true;
		else if (tier == "launch") result.allow.launch = true;
		else
			result.problems.emplace_back(
				std::format("-mcp-allow:{} is not write, capture, network, dev or launch", tier));
	}

	return result;
}

agent_job::agent_job(std::string id, std::string tool) :
	_id(std::move(id)), _tool(std::move(tool)), _started(core::now())
{
}

void agent_job::progress(const double fraction, const std::string_view message)
{
	std::lock_guard lock(_mutex);
	_fraction = fraction;
	if (!message.empty()) _message = message;
}

void agent_job::output(const std::string_view line)
{
	std::lock_guard lock(_mutex);
	if (_output_bytes > kept_output_bytes) return;
	_lines.emplace_back(line);
	_output_bytes += line.size() + 1;
	if (_output_bytes > kept_output_bytes) _lines.emplace_back("(the rest of the output is not kept)");
}

void agent_job::complete(core::mcp::tool_result result)
{
	std::lock_guard lock(_mutex);
	if (_done) return;
	_done = true;
	_result = std::move(result);
	_cancel = {};
}

void agent_job::on_cancel(std::function<void()> cancel)
{
	std::lock_guard lock(_mutex);
	if (!_done) _cancel = std::move(cancel);
}

void agent_job::cancel()
{
	std::function<void()> cancel;
	{
		std::lock_guard lock(_mutex);
		if (_done || _cancelled) return;
		_cancelled = true;
		cancel = _cancel;
	}

	if (cancel) cancel();
	else complete(core::mcp::tool_result::error(std::format("{} was cancelled", _tool)));
}

bool agent_job::is_done() const
{
	std::lock_guard lock(_mutex);
	return _done;
}

core::mcp::tool_result agent_job::result() const
{
	std::lock_guard lock(_mutex);
	return _result;
}

double agent_job::fraction() const
{
	std::lock_guard lock(_mutex);
	return _fraction;
}

std::string agent_job::message() const
{
	std::lock_guard lock(_mutex);
	return _message;
}

std::string agent_job::recent_output(const size_t lines) const
{
	std::lock_guard lock(_mutex);
	std::string result;
	const auto first = _lines.size() > lines ? _lines.size() - lines : 0;

	for (auto i = first; i < _lines.size(); ++i)
	{
		result += _lines[i];
		result += '\n';
	}

	return result;
}

std::string agent_job::all_output() const
{
	return recent_output(SIZE_MAX);
}

double agent_job::seconds() const
{
	return core::now() - _started;
}

mcp_host::mcp_host(app_frame& app, agent_options options, const bool console) :
	_app(app), _options(std::move(options)), _console(console)
{
	_server = std::make_shared<core::mcp::server>(core::mcp::server_info{
		.name = "Diffractor",
		.version = df::format_version(true),
		.instructions = std::string(instructions),
	});

	if (!_console) core::set_log_observer(&observe_log);
	// Another program, a folder or a web page would open on the user's desktop, beyond the session.
	platform::allow_agent_to_open(_options.allow.launch);

	add_session_tools(*this);
	add_engine_tools(*this);
	add_library_tools(*this);
	add_ui_tools(*this);
}

void mcp_host::add(core::mcp::tool tool, const tool_tier tier, const bool needs_window)
{
	const auto& allow = _options.allow;
	const auto allowed = tier == tool_tier::read || (tier == tool_tier::write && allow.write) ||
	                     (tier == tool_tier::capture && allow.capture) || (tier == tool_tier::dev && allow.dev);

	if (!allowed)
	{
		const auto* const needs = tier == tool_tier::write ? "write" : tier == tool_tier::capture ? "capture" : "dev";
		_withheld.emplace_back(tool.name, std::format("-mcp-allow:{}", needs));
		return;
	}

	_schemas[tool.name] = tool.input_schema;
	auto handler = std::move(tool.handler);

	tool.handler = [this, handler, needs_window, name = tool.name](const core::mcp::call_ptr& call)
	{
		if (needs_window)
		{
			if (_console)
			{
				call->finish(core::mcp::tool_result::error(
					std::format("{} works in a session, with the window: start Diffractor with -mcp", name)));
				return;
			}

			std::string why;

			if (!ensure_window(why))
			{
				call->finish(core::mcp::tool_result::error(why));
				return;
			}
		}

		handler(call);
	};

	_server->add_tool(std::move(tool));
}

std::string mcp_host::input_schema(const std::string_view tool) const
{
	const auto found = _schemas.find(std::string(tool));
	return found == _schemas.end() ? std::string{} : found->second;
}

bool mcp_host::ensure_window(std::string& why)
{
	if (!_app._window_started)
	{
		if (_console)
		{
			why = "This needs a session, with the window: start Diffractor with -mcp";
			return false;
		}

		core::log(__FUNCTION__, "A tool needs the window: making it");

		if (!_app.start_window(why))
		{
			if (why.empty()) why = "The window could not be made";
			return false;
		}

		// The platform shows the windows init made; this one comes later, and out of sight, so it is shown here.
		_app._app_frame->show(true);
		track_command_use(_app);
	}

	return true;
}

bool mcp_host::modal_ui_open() const
{
	return (_app._app_frame && !_app._app_frame->is_enabled()) || ui::menu_active > 0 || !open_dialogs().empty();
}

bool mcp_host::answerable_ui_open() const
{
	return ui::menu_active > 0 || dialog_awaits_answer();
}

agent_job_ptr mcp_host::start_job(const std::string_view tool)
{
	std::lock_guard lock(_jobs_mutex);
	auto job = std::make_shared<agent_job>(std::format("job-{}", _next_job++), std::string(tool));
	_jobs.emplace(job->id(), job);

	// Finished jobs are kept for a while, for a job_wait that comes late; the oldest go first.
	constexpr size_t kept_jobs = 64;

	while (_jobs.size() > kept_jobs)
	{
		const auto oldest = std::ranges::find_if(_jobs, [](const auto& entry) { return entry.second->is_done(); });
		if (oldest == _jobs.end()) break;
		_jobs.erase(oldest);
	}

	return job;
}

agent_job_ptr mcp_host::find_job(const std::string_view id) const
{
	std::lock_guard lock(_jobs_mutex);
	const auto found = _jobs.find(id);
	return found == _jobs.end() ? nullptr : found->second;
}

std::vector<agent_job_ptr> mcp_host::jobs() const
{
	std::lock_guard lock(_jobs_mutex);
	std::vector<agent_job_ptr> result;
	for (const auto& [id, job] : _jobs) result.push_back(job);
	return result;
}

double mcp_host::wait_seconds(const core::mcp::call& call)
{
	const auto asked = call.number("wait_s");
	if (!asked) return default_wait_seconds;
	return std::clamp(*asked, 0.0, longest_wait_seconds);
}

void mcp_host::wait(const core::mcp::call_ptr& call, const agent_job_ptr& job, const double wait_s,
                    const bool interruptible)
{
	if (job->is_done())
	{
		call->finish(job->result());
		return;
	}

	waiter w;
	w.call = call;
	w.job = job;
	w.deadline = core::now() + wait_s;
	w.interruptible = interruptible;
	_waiters.push_back(std::move(w));

	// A single call from the command line has no frame ticks of its own: it is answered here.
	if (_console)
	{
		while (!call->is_finished())
		{
			platform::pump_ui_tasks(20);
			tick();
		}
	}
}

void mcp_host::when(std::function<bool()> ready, const double quiet_seconds, const double timeout_seconds,
                    std::function<void(bool ready)> then)
{
	_conditions.push_back({std::move(ready), std::move(then), quiet_seconds, -1.0, core::now() + timeout_seconds});
}

void mcp_host::wait_until(const core::mcp::call_ptr& call, const std::string_view tool, std::function<bool()> settled,
                          const double quiet_seconds, const double timeout_seconds,
                          std::function<core::mcp::tool_result(bool settled)> answer)
{
	const auto job = start_job(tool);
	when(std::move(settled), quiet_seconds, timeout_seconds,
	     [job, answer = std::move(answer)](const bool met) { job->complete(answer(met)); });
	wait(call, job, timeout_seconds + 1.0, false);
}

void mcp_host::release(waiter& w, const std::string_view why)
{
	const auto& job = w.job;
	const auto dialog = why == "dialog";
	const auto fraction = job->fraction();
	const auto recent = job->recent_output(12);

	json_writer json;
	json.object();
	json.text("job", job->id());
	json.text("state", dialog ? "waiting_for_dialog" : "running");
	if (fraction >= 0.0) json.number("progress", fraction);
	json.text("message", job->message());
	json.number("seconds", job->seconds());
	if (dialog)
	{
		json.key("dialogs").raw(describe_dialogs_json(_app));
		json.key("menu").raw(describe_menu_json());
	}
	json.end_object();

	auto text = dialog ? std::format("{} is waiting on a dialog or menu that opened. Answer a dialog with "
	                                 "dialog_respond or a menu with menu_respond, then job_wait for {}.{}{}",
	                                 job->tool(), job->id(), describe_dialogs_text(_app), describe_menu_text())
	                   : std::format("{} is still running after {:.0f} s. job_wait waits for {}; job_cancel stops it.",
	                                 job->tool(), job->seconds(), job->id());
	if (!recent.empty()) text += "\n\nLatest output:\n" + recent;

	w.call->finish(structured_result(std::move(text), json.str()));
}

void mcp_host::tick()
{
	const auto now = core::now();

	// A condition is met once it has held for its quiet time, or given up on at its limit. What follows runs once the
	// list is settled, since it may add to it.
	std::vector<std::pair<std::function<void(bool)>, bool>> due;

	for (auto i = _conditions.begin(); i != _conditions.end();)
	{
		auto& c = *i;

		if (c.ready())
		{
			if (c.since < 0.0) c.since = now;
		}
		else
		{
			c.since = -1.0;
		}

		const auto met = c.since >= 0.0 && now - c.since >= c.quiet_seconds;

		if (met || now >= c.deadline)
		{
			due.emplace_back(std::move(c.then), met);
			i = _conditions.erase(i);
			continue;
		}

		++i;
	}

	for (const auto& [then, met] : due) then(met);

	// A waiting call is answered with what it waits behind only once there is something to answer: a dialog that shows,
	// or a menu. Work holding the window for a moment, a rating written say, is waited out.
	const auto modal = answerable_ui_open();

	for (auto i = _waiters.begin(); i != _waiters.end();)
	{
		auto& w = *i;

		if (w.call->is_cancelled() || w.call->is_finished())
		{
			if (w.call->is_cancelled()) w.job->cancel();
			i = _waiters.erase(i);
			continue;
		}

		if (w.job->is_done())
		{
			w.call->finish(w.job->result());
			i = _waiters.erase(i);
			continue;
		}

		const auto fraction = w.job->fraction();
		const auto message = w.job->message();

		if (fraction != w.last_fraction || message != w.last_message)
		{
			w.last_fraction = fraction;
			w.last_message = message;
			// Progress must rise with each notification: a fraction where there is one, otherwise the seconds spent.
			if (fraction >= 0.0) w.call->progress(fraction * 100.0, 100.0, message);
			else w.call->progress(w.job->seconds(), 0.0, message);
		}

		if (w.interruptible && modal)
		{
			release(w, "dialog");
			i = _waiters.erase(i);
			continue;
		}

		if (now >= w.deadline)
		{
			release(w, "deadline");
			i = _waiters.erase(i);
			continue;
		}

		++i;
	}
}

void mcp_host::run_child(
	const core::mcp::call_ptr& call, const std::string_view tool, std::vector<std::string> arguments,
	std::function<core::mcp::tool_result(int exit_code, const std::string& out, const agent_job& job)> answer)
{
	const auto job = start_job(tool);
	const auto out = std::make_shared<std::string>();
	const auto tests = std::make_shared<std::pair<int, int>>(0, 0);

	platform::child_process_callbacks callbacks;

	callbacks.on_stdout_line = [out, job, tests](const std::string_view line)
	{
		*out += line;
		*out += '\n';

		// A test run says how many tests it runs, then one line a test.
		if (line.starts_with("Running ") && line.ends_with(" tests..."))
		{
			tests->first = parse_int(line.substr(8));
		}
		else if ((line.starts_with("  PASS") || line.starts_with("  FAIL")) && tests->first > 0)
		{
			++tests->second;
			job->progress(static_cast<double>(tests->second) / tests->first,
			              std::format("{} of {} tests", tests->second, tests->first));
		}
	};

	callbacks.on_stderr_line = [job](const std::string_view line)
	{
		job->output(line);

		// A render says how far it has got as a percentage.
		if (const auto percent = line.find('%'); percent != std::string_view::npos && percent > 0)
		{
			auto digits = percent;
			while (digits > 0 && line[digits - 1] >= '0' && line[digits - 1] <= '9') --digits;
			if (digits < percent)
				job->progress(parse_int(line.substr(digits, percent - digits)) / 100.0, core::trim(line));
		}
	};

	callbacks.on_exit = [out, job, answer](const int exit_code)
	{
		job->complete(answer ? answer(exit_code, *out, *job) : core::mcp::tool_result::error("No answer"));
	};

	const auto exe = platform::running_app_path();
	auto child = platform::spawn_child_process(exe, arguments, {}, std::move(callbacks));

	if (!child)
	{
		job->complete(core::mcp::tool_result::error(std::format("{} could not start a copy of Diffractor", tool)));
		call->finish(job->result());
		return;
	}

	core::log(__FUNCTION__, std::format("{} runs as {}", tool, job->id()));

	const std::shared_ptr<platform::child_process> running(std::move(child));
	job->on_cancel([running, job]
	{
		running->terminate();
		job->complete(core::mcp::tool_result::error(std::format("{} was cancelled", job->tool())));
	});

	// The process lives as long as the job: its callbacks belong to it.
	std::erase_if(_children, [](const auto& c) { return c.first->is_done(); });
	_children.emplace_back(job, running);

	wait(call, job, wait_seconds(*call), false);
}

void mcp_host::started()
{
	if (_console) return;

	for (const auto& problem : _options.problems) core::log(__FUNCTION__, problem);
	core::log(__FUNCTION__,
	          std::format("Agent session: window {}, allowing {}{}{}{}{}", presence_name(_options.presence),
	                      _options.allow.write ? "write " : "", _options.allow.capture ? "capture " : "",
	                      _options.allow.network ? "network " : "", _options.allow.dev ? "dev " : "",
	                      _options.allow.launch ? "launch" : ""));
	if (_app._window_started) track_command_use(_app);
}

uint64_t mcp_host::log_mark() const
{
	auto& r = ring();
	std::lock_guard lock(r.mutex);
	return r.next;
}

std::vector<std::string> mcp_host::log_since(const uint64_t mark, const size_t most) const
{
	auto& r = ring();
	std::lock_guard lock(r.mutex);
	std::vector<std::string> result;

	for (const auto& [number, line] : r.lines)
		if (number >= mark) result.push_back(line);

	if (result.size() > most) result.erase(result.begin(), result.end() - static_cast<ptrdiff_t>(most));
	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The session's own tools
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	void add_status(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "status";
		t.title = "Session status";
		t.description = "What this session is: Diffractor's version, the profile and collection it uses, where its "
						"window is and what it shows, how the index stands, what the session may do and the tools it "
						"withholds, and its jobs. Start here.";
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto& options = host.options();
			const auto& index = app._item_index;

			json_writer json;
			json.object();
			json.text("version", df::format_version(true));
#ifdef _DEBUG
			json.text("build", "debug");
#else
			json.text("build", "release");
#endif
			json.text("platform", platform::OS());
			json.text("profile", command_line.profile_folder.empty() ? "the user's own" : command_line.profile_folder);
			json.text("cache_folder", app_cache_folder().text().sv());
			json.key("collection").array();
			for (const auto& folder : index_folders(setting.collection).folders) json.text(folder.text().sv());
			json.end_array();
			json.boolean("settings_saved", false);

			json.key("window").object();
			json.text("presence", presence_name(options.presence));
			json.boolean("made", app._window_started);

			if (app._window_started && app._app_frame)
			{
				const auto bounds = app._app_frame->window_bounds();
				json.integer("width", bounds.width());
				json.integer("height", bounds.height());
				json.text("view", view_name(app._state.view_mode()));
				json.text("search", app._state.search().text());
			}

			json.end_object();

			json.key("index").object();
			json.boolean("started", app._window_started);
			json.boolean("ready", app._window_started && index.is_init_complete());
			json.integer("items", static_cast<int64_t>(index.stats.index_item_count));
			json.integer("folders", static_cast<int64_t>(index.stats.index_folder_count));
			json.key("busy").array();
			for (const auto& b : agent_window_busy(app)) json.text(b);
			json.end_array();
			json.end_object();

			json.key("allow").object();
			json.boolean("write", options.allow.write);
			json.boolean("capture", options.allow.capture);
			json.boolean("network", options.allow.network);
			json.boolean("dev", options.allow.dev);
			json.boolean("launch", options.allow.launch);
			json.end_object();

			json.key("withheld").array();

			for (const auto& [name, needs] : host.withheld())
			{
				json.object().text("tool", name).text("needs", needs).end_object();
			}

			json.end_array();
			json.key("jobs").array();

			for (const auto& job : host.jobs())
			{
				json.object().text("job", job->id()).text("tool", job->tool());
				json.text("state", job->is_done() ? "done" : "running").end_object();
			}

			json.end_array();
			json.key("problems").array();
			for (const auto& problem : options.problems) json.text(problem);
			json.end_array();
			json.end_object();

			const auto summary = std::format("Diffractor {} - window {}{}; index {}; {} tools withheld",
			                                 df::format_version(true), presence_name(options.presence),
			                                 app._window_started ? " (made)" : " (not made yet)",
			                                 !app._window_started ? "not loaded until a tool needs it"
			                                 : index.is_init_complete()
			                                     ? std::format("ready, {} items", index.stats.index_item_count)
			                                     : "loading"s,
			                                 host.withheld().size());
			call->finish(structured_result(summary, json.str()));
		};
		host.add(std::move(t), tool_tier::read, false);
	}

	void add_help(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "help";
		t.title = "How to use Diffractor's tools";
		t.description = "Guidance: overview (the default), search (the query language), dfv (the Movie project "
						"format), dft (tutorial scripts, and ui_input's steps), tiers (what -mcp-allow allows).";
		t.input_schema =
			R"({"type":"object","properties":{"topic":{"type":"string","enum":["overview","search","dfv","dft","tiers"]}},"additionalProperties":false})";
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [](const core::mcp::call_ptr& call)
		{
			call->finish(core::mcp::tool_result::text(help_text(call->text("topic", "overview"))));
		};
		host.add(std::move(t), tool_tier::read, false);
	}

	void add_wait_until_idle(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "wait_until_idle";
		t.title = "Wait until Diffractor is idle";
		t.description =
			"Waits until the index has loaded and nothing is scanning, searching, indexing, making "
			"thumbnails, loading media, acting out steps, running a task - a Sync or Import analyzing "
			"or running, say - or showing work under way, an edit being saved, and the window has caught "
			"up - quiet for quiet_ms - or until timeout_s. Says what is still busy if it gives up. Before a "
			"screenshot, or after starting work whose effects you want to read.";
		t.input_schema =
			R"({"type":"object","properties":{"timeout_s":{"type":"number","minimum":0,"maximum":600},"quiet_ms":{"type":"integer","minimum":0,"maximum":10000}},"additionalProperties":false})";
		t.annotations.read_only = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto timeout = call->number("timeout_s").value_or(60.0);
			const auto quiet = call->number("quiet_ms").value_or(500.0) / 1000.0;
			const auto started = core::now();

			host.wait_until(call, "wait_until_idle", [&app] { return agent_window_settled(app); }, quiet, timeout,
			                [&app, started](const bool settled)
			{
				json_writer json;
				json.object().boolean("idle", settled).number("waited_s", core::now() - started);
				json.key("busy").array();
				const auto busy = agent_window_busy(app);
				for (const auto& b : busy) json.text(b);
				json.end_array().end_object();
				const auto text = settled ? std::format("Idle after {:.1f} s", core::now() - started)
				                          : std::format("Still busy after {:.1f} s: {}", core::now() - started,
				                                        join_text(busy, ", "));
				return structured_result(text, json.str());
			});
		};
		host.add(std::move(t), tool_tier::read, false);
	}

	void add_diagnostics(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "diagnostics";
		t.title = "Diagnostics";
		t.description = "What Diffractor knows of itself: the version, the system and graphics, memory, the "
						"index's counts and timings, what is running, and the session's performance counters - "
						"queries, the database, files, thumbnails and painting - since it started. Ask before and "
						"after an action to see what it cost.";
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			json_writer json;
			json.object();
			json.key("app").object();
			std::string text;

			for (const auto& [name, value] : calc_app_info(app._item_index, true))
			{
				auto key = core::trim(name);
				if (key.ends_with(':')) key.remove_suffix(1);
				json.text(core::trim(key), value);
				text += std::format("{} {}\n", name, value);
			}

			json.end_object();

			const auto& q = df::query_perf;
			const auto& f = df::file_perf;
			const auto& d = df::db_perf;
			const auto& th = df::thumbnail_perf;
			const auto& ui = render::ui_perf;

			json.key("counters").object();
			json.integer("queries", static_cast<int64_t>(q.queries.load()));
			json.integer("query_ms", static_cast<int64_t>(q.query_us.load() / 1000));
			json.integer("query_max_ms", static_cast<int64_t>(q.query_max_us.load() / 1000));
			json.integer("file_scans", static_cast<int64_t>(f.scans.load()));
			json.integer("file_loads", static_cast<int64_t>(f.loads.load()));
			json.integer("file_load_max_ms", static_cast<int64_t>(f.load_max_us.load() / 1000));
			json.integer("decodes", static_cast<int64_t>(f.decodes.load()));
			json.integer("metadata_errors", static_cast<int64_t>(f.metadata_errors.load()));
			json.integer("db_items_written", static_cast<int64_t>(d.items_written.load()));
			json.integer("db_thumbs_written", static_cast<int64_t>(d.thumbs_written.load()));
			json.integer("thumbnail_plans", static_cast<int64_t>(th.plans.load()));
			json.integer("thumbnail_load_failures", static_cast<int64_t>(th.load_failures.load()));
			json.integer("paints", static_cast<int64_t>(ui.paints.load()));
			json.integer("paint_ms", static_cast<int64_t>(ui.paint_us.load() / 1000));
			json.integer("paint_max_ms", static_cast<int64_t>(ui.paint_max_us.load() / 1000));
			json.integer("frame_prepares", static_cast<int64_t>(ui.frame_prepares.load()));
			json.integer("invalidates", static_cast<int64_t>(ui.invalidates.load()));
			json.end_object();
			json.end_object();

			call->finish(structured_result(text, json.str()));
		};
		host.add(std::move(t), tool_tier::read, false);
	}

	void add_log_read(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "log_read";
		t.title = "Read the log";
		t.description = "The latest lines Diffractor wrote to its log - most, 200 by default - optionally only those "
						"containing some text, or only those since a mark an earlier call returned. Returns a mark "
						"to read on from.";
		t.input_schema =
			R"({"type":"object","properties":{"lines":{"type":"integer","minimum":1,"maximum":4000},"contains":{"type":"string"},"since":{"type":"integer","minimum":0}},"additionalProperties":false})";
		t.annotations.read_only = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			const auto most = static_cast<size_t>(call->number("lines").value_or(200));
			const auto since = static_cast<uint64_t>(call->number("since").value_or(0));
			const auto contains = call->text("contains");
			const auto mark = host.log_mark();
			auto lines = host.log_since(since, kept_log_lines);

			if (!contains.empty())
			{
				std::erase_if(lines, [&contains](const std::string& l)
				{ return core::ifind(l, contains) == std::string::npos; });
			}

			if (lines.size() > most) lines.erase(lines.begin(), lines.end() - static_cast<ptrdiff_t>(most));

			json_writer json;
			json.object()
				.integer("mark", static_cast<int64_t>(mark))
				.integer("lines", static_cast<int64_t>(lines.size()));
			json.end_object();

			auto text = join_text(lines, "\n");
			if (text.empty()) text = "(no lines)";
			call->finish(structured_result(std::format("{}\n\nmark {}", text, mark), json.str()));
		};
		host.add(std::move(t), tool_tier::read, false);
	}

	void add_jobs(mcp_host& host)
	{
		core::mcp::tool w;
		w.name = "job_wait";
		w.title = "Wait for a job";
		w.description = "Waits up to wait_s seconds for a job a tool started, and answers with its result once it "
						"has finished, or with how far it has got.";
		w.input_schema =
			R"({"type":"object","properties":{"job":{"type":"string"},"wait_s":{"type":"number","minimum":0,"maximum":600}},"required":["job"],"additionalProperties":false})";
		w.annotations.read_only = true;
		w.handler = [&host](const core::mcp::call_ptr& call)
		{
			const auto job = host.find_job(call->text("job"));

			if (!job)
			{
				call->finish(core::mcp::tool_result::error(
					std::format("There is no job {}: status lists the session's jobs", call->text("job"))));
				return;
			}

			host.wait(call, job, mcp_host::wait_seconds(*call), true);
		};
		host.add(std::move(w), tool_tier::read, false);

		core::mcp::tool c;
		c.name = "job_cancel";
		c.title = "Cancel a job";
		c.description = "Stops a job a tool started. A render stops without leaving a movie behind.";
		c.input_schema =
			R"({"type":"object","properties":{"job":{"type":"string"}},"required":["job"],"additionalProperties":false})";
		c.handler = [&host](const core::mcp::call_ptr& call)
		{
			const auto job = host.find_job(call->text("job"));

			if (!job)
			{
				call->finish(core::mcp::tool_result::error(std::format("There is no job {}", call->text("job"))));
				return;
			}

			job->cancel();
			call->finish(core::mcp::tool_result::text(job->is_done() ? std::format("{} has stopped", job->id())
			                                                         : std::format("{} is stopping", job->id())));
		};
		host.add(std::move(c), tool_tier::read, false);
	}

	void add_window(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "window";
		t.title = "Make or size the window";
		t.description = "Makes the window, if no tool has yet, and sizes it: width and height are its client area "
						"in pixels, the size screenshots and recordings are made at. Where it is - out of sight or "
						"on the desktop - is the session's, set by -mcp-ui.";
		t.input_schema =
			R"({"type":"object","properties":{"width":{"type":"integer","minimum":320,"maximum":7680},"height":{"type":"integer","minimum":240,"maximum":4320},"maximized":{"type":"boolean"}},"additionalProperties":false})";
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			const auto& frame = host.app()._app_frame;
			const auto maximize = call->has("maximized") && call->flag("maximized", false);
			const auto width = call->number("width");
			const auto height = call->number("height");
			const auto resize = !maximize && (width || height || call->has("maximized"));

			if (maximize) frame->sys_command(platform::sys_command_type::MAXIMIZE);
			else if (resize && frame->is_maximized()) frame->sys_command(platform::sys_command_type::RESTORE);

			// Once the window has had the command, which is posted to it: a restore it had yet to have would undo
			// the size.
			platform::run_ui([&host, call, resize, width, height]
			{
				const auto& frame = host.app()._app_frame;

				if (resize && (width || height))
				{
					// The drawn caption makes the whole window client area, so the window's size is the client's.
					auto bounds = frame->window_bounds();
					bounds.right = bounds.left + static_cast<int>(width.value_or(bounds.width()));
					bounds.bottom = bounds.top + static_cast<int>(height.value_or(bounds.height()));
					frame->window_bounds(bounds, true);
				}

				const auto bounds = frame->window_bounds();
				const auto presence = presence_name(host.options().presence);
				json_writer json;
				json.object().text("presence", presence);
				json.integer("width", bounds.width()).integer("height", bounds.height());
				json.boolean("maximized", frame->is_maximized()).end_object();
				call->finish(
					structured_result(std::format("The window is {}x{}, {}", bounds.width(), bounds.height(), presence),
				                      json.str()));
			});
		};
		host.add(std::move(t), tool_tier::read, true);
	}
}

void add_session_tools(mcp_host& host)
{
	add_help(host);
	add_status(host);
	add_wait_until_idle(host);
	add_diagnostics(host);
	add_log_read(host);
	add_jobs(host);
	add_window(host);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// -tool:
///////////////////////////////////////////////////////////////////////////////////////////////////

std::string tool_arguments_json(const std::string_view input_schema, const std::vector<std::string>& pairs,
                                std::string& error)
{
	rapidjson::Document schema;
	schema.Parse(input_schema.data(), input_schema.size());
	const auto* const properties = schema.IsObject() && schema.HasMember("properties") ? &schema["properties"]
	                                                                                   : nullptr;

	const auto type_of = [properties](const std::string& name) -> std::string
	{
		if (!properties || !properties->IsObject() || !properties->HasMember(name.c_str())) return {};
		const auto& p = (*properties)[name.c_str()];
		return p.IsObject() && p.HasMember("type") && p["type"].IsString() ? p["type"].GetString() : "";
	};

	json_writer json;
	json.object();

	for (const auto& pair : pairs)
	{
		const auto equals = pair.find('=');

		if (equals == std::string::npos || equals == 0)
		{
			error = std::format("{} is not name=value", pair);
			return {};
		}

		const auto name = pair.substr(0, equals);
		const auto value = std::string_view(pair).substr(equals + 1);
		const auto type = type_of(name);
		json.key(name);

		if (type == "integer" || type == "number")
		{
			double number = 0;
			const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);

			if (ec != std::errc{} || end != value.data() + value.size())
			{
				error = std::format("{} is a number, not {}", name, value);
				return {};
			}

			if (type == "integer") json.integer(static_cast<int64_t>(number));
			else json.number(number);
		}
		else if (type == "boolean")
		{
			const auto yes = value == "true" || value == "1" || value == "yes";
			const auto no = value == "false" || value == "0" || value == "no";

			if (!yes && !no)
			{
				error = std::format("{} is true or false, not {}", name, value);
				return {};
			}

			json.boolean(yes);
		}
		else if (type == "array")
		{
			json.array();
			for (const auto part : core::split(value, false, [](const char c) { return c == ','; })) json.text(part);
			json.end_array();
		}
		else if (type == "object")
		{
			rapidjson::Document object;

			if (object.Parse(value.data(), value.size()).HasParseError() || !object.IsObject())
			{
				error = std::format("{} is a JSON object, not {}", name, value);
				return {};
			}

			json.raw(value);
		}
		else
		{
			json.text(value);
		}
	}

	json.end_object();
	return json.str();
}

int run_tool_once(const std::shared_ptr<mcp_host>& host, const std::string_view name,
                  const std::vector<std::string>& pairs)
{
	// Through the C runtime's streams, which the console mode bound to whatever the caller redirected.
	const auto print_error = [](const std::string_view text)
	{
		std::fprintf(stderr, "%.*s\n", static_cast<int>(text.size()), text.data());
		std::fflush(stderr);
	};

	const auto print = [](const std::string_view text)
	{
		std::fprintf(stdout, "%.*s\n", static_cast<int>(text.size()), text.data());
		std::fflush(stdout);
	};

	if (!host)
	{
		print_error("-tool: there is no tool host");
		return 2;
	}

	const auto schema = host->input_schema(name);

	if (schema.empty())
	{
		print_error(std::format("-tool: there is no tool {}; the tools are: {}", name,
		                        join_text(host->server()->tool_names(), ", ")));
		return 2;
	}

	std::string error;
	const auto arguments = tool_arguments_json(schema, pairs, error);

	if (!error.empty())
	{
		print_error(std::format("-tool:{}: {}", name, error));
		return 2;
	}

	std::string answer;
	host->server()->connect([&answer](const std::string_view message) { answer = message; },
	                        [](std::function<void()> task) { task(); });
	host->server()->receive(
		std::format(R"({{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{{"name":{},"arguments":{}}}}})",
	                core::mcp::quote(name), arguments));

	rapidjson::Document response;
	response.Parse(answer.c_str());

	if (!response.IsObject() || !response.HasMember("result"))
	{
		const auto message = response.IsObject() && response.HasMember("error") &&
		                             response["error"].HasMember("message")
		                         ? std::string(response["error"]["message"].GetString())
		                         : answer;
		print_error(std::format("-tool:{}: {}", name, message));
		return 2;
	}

	const auto& result = response["result"];
	const auto is_error = result.HasMember("isError") && result["isError"].IsBool() && result["isError"].GetBool();

	if (result.HasMember("structuredContent"))
	{
		print(core::mcp::to_json(result["structuredContent"]));
	}
	else if (result.HasMember("content") && result["content"].IsArray())
	{
		for (const auto& c : result["content"].GetArray())
		{
			if (c.HasMember("text") && c["text"].IsString())
			{
				// What went wrong is not a result: stdout is kept for those.
				if (is_error) print_error(c["text"].GetString());
				else print(c["text"].GetString());
			}
		}
	}

	return is_error ? 1 : 0;
}
