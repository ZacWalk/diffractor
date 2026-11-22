// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The agent session (-mcp) and the single tool call (-tool:): the tools Diffractor offers an agent over MCP,
// what a session may do (-mcp-allow) and where its window is (-mcp-ui), the jobs long work runs as, and how a tool
// reaches the application - the window it makes when a tool first needs one, the UI thread, and copies of itself for
// work that must not share its process. The protocol is libdesktop's (core/mcp.h); each tool adapts a function the
// views or the command line already use. docs/agents.md owns the session.

#pragma once

#include "libdesktop/core/mcp.h"

class app_frame;

// Where a session's window is: made when a tool first needs it and kept where nobody sees it, made at once where
// nobody sees it, or made at once on the desktop.
enum class agent_presence
{
	none,
	offscreen,
	visible,
};

// What a session may do beyond reading. Each is the user's to grant, in the client's configuration.
struct agent_allow
{
	// Change files: render, rename, edit metadata, and any input or command that can.
	bool write = false;
	// Return or save pictures: of the screen, and of the user's media.
	bool capture = false;
	// Reach the network on the session's behalf: update checks, crash reports, map tiles, dictionaries.
	bool network = false;
	// The developer's tools: the test suite, translations, documentation, the duplicate report.
	bool dev = false;
	// Open another program, a folder or a web page on the user's desktop: the help, a file's folder, a map, Open with.
	bool launch = false;
};

struct agent_options
{
	agent_presence presence = agent_presence::none;
	agent_allow allow;
	// What -mcp-ui and -mcp-allow said that was not understood, which status reports.
	std::vector<std::string> problems;
};

agent_options parse_agent_options(std::string_view ui, std::string_view allow);

enum class tool_tier
{
	read,
	write,
	capture,
	dev,
};

// Long work - a render, a test run, a tutorial, a command that can open a dialog - runs as a job, which the call
// that started it waits for. Every member is safe from any thread.
class agent_job
{
public:
	agent_job(std::string id, std::string tool);

	const std::string& id() const { return _id; }
	const std::string& tool() const { return _tool; }

	// How far it has got, from 0 to 1, or below 0 for work of unknown size; and what it is doing.
	void progress(double fraction, std::string_view message);
	// A line of what it reports, the latest of which a call still waiting is shown.
	void output(std::string_view line);
	// Its answer. Only the first counts.
	void complete(core::mcp::tool_result result);
	// How to stop it; it completes itself once stopped. Run on the UI thread.
	void on_cancel(std::function<void()> cancel);
	void cancel();

	bool is_done() const;
	core::mcp::tool_result result() const;
	double fraction() const;
	std::string message() const;
	// The last lines it reported, joined.
	std::string recent_output(size_t lines) const;
	// The whole of what it reported, up to a bound.
	std::string all_output() const;
	double seconds() const;

private:
	std::string _id;
	std::string _tool;
	double _started = 0.0;
	mutable std::mutex _mutex;
	std::function<void()> _cancel;
	double _fraction = -1.0;
	std::string _message;
	std::deque<std::string> _lines;
	size_t _output_bytes = 0;
	bool _done = false;
	bool _cancelled = false;
	core::mcp::tool_result _result;
};

using agent_job_ptr = std::shared_ptr<agent_job>;

class mcp_host : public std::enable_shared_from_this<mcp_host>
{
public:
	// How long a call waits for its job before answering with how far it has got, unless it asks for longer.
	static constexpr double default_wait_seconds = 50.0;
	static constexpr double longest_wait_seconds = 600.0;

	mcp_host(app_frame& app, agent_options options, bool console);

	const core::mcp::server_ptr& server() const { return _server; }
	const agent_options& options() const { return _options; }
	// A -tool: call, run once in the console, rather than an agent session.
	bool is_console() const { return _console; }
	app_frame& app() const { return _app; }

	// Offers the tool where the session's tiers allow it, and otherwise says what would; a tool that needs the
	// window makes it first.
	void add(core::mcp::tool tool, tool_tier tier, bool needs_window);
	// The tools withheld, each with what would let it in.
	const std::vector<std::pair<std::string, std::string>>& withheld() const { return _withheld; }

	// On the UI thread: the window and the index, made when a tool first needs them. False, with why, where they
	// cannot be.
	bool ensure_window(std::string& why);
	// Whether a dialog or a menu holds the window, which is what input waits behind.
	bool modal_ui_open() const;
	// Whether a dialog shows or a menu is open: what a client can see and answer. Work that holds the window while it
	// runs, with nothing showing or with a dialog that only shows it under way, is not one.
	bool answerable_ui_open() const;

	agent_job_ptr start_job(std::string_view tool);
	agent_job_ptr find_job(std::string_view id) const;
	std::vector<agent_job_ptr> jobs() const;
	// Answers the call with the job's result once it is done; sooner where the wait passes - with how far it has got,
	// and the job to wait on - or, where interruptible, where a dialog shows or a menu opens.
	void wait(const core::mcp::call_ptr& call, const agent_job_ptr& job, double wait_s, bool interruptible);
	// The wait a call asked for, in seconds: wait_s, bounded, or the default.
	static double wait_seconds(const core::mcp::call& call);
	// Runs then on the UI thread once ready has held for quiet_seconds, with true, or once timeout_seconds pass, with
	// false.
	void when(std::function<bool()> ready, double quiet_seconds, double timeout_seconds,
	          std::function<void(bool ready)> then);
	// Answers the call once settled has held for quiet_seconds, or once timeout_seconds pass, with what answer makes of
	// whether it settled.
	void wait_until(const core::mcp::call_ptr& call, std::string_view tool, std::function<bool()> settled,
	                double quiet_seconds, double timeout_seconds,
	                std::function<core::mcp::tool_result(bool settled)> answer);

	// Runs a copy of this program with these arguments as the call's job, for work that must not share the session's
	// process: what it writes to stderr is the job's output and progress, and once it exits, answer makes the result of
	// its exit code and what it wrote to stdout.
	void run_child(
		const core::mcp::call_ptr& call, std::string_view tool, std::vector<std::string> arguments,
		std::function<core::mcp::tool_result(int exit_code, const std::string& out, const agent_job& job)> answer);
	// A tool's input schema, as -tool: types its arguments by; empty for a tool the session withholds.
	std::string input_schema(std::string_view tool) const;

	// Each frame tick, on the UI thread.
	void tick();
	// Once init has run: the session is ready to answer.
	void started();

	// A mark in the log, and the lines written since one.
	uint64_t log_mark() const;
	std::vector<std::string> log_since(uint64_t mark, size_t most) const;

private:
	struct waiter
	{
		core::mcp::call_ptr call;
		agent_job_ptr job;
		double deadline = 0.0;
		bool interruptible = false;
		std::string last_message;
		double last_fraction = -2.0;
	};

	struct condition
	{
		std::function<bool()> ready;
		std::function<void(bool)> then;
		double quiet_seconds = 0.0;
		double since = -1.0;
		double deadline = 0.0;
	};

	void release(waiter& w, std::string_view why);

	app_frame& _app;
	agent_options _options;
	bool _console = false;
	core::mcp::server_ptr _server;
	std::vector<std::pair<std::string, std::string>> _withheld;
	std::map<std::string, agent_job_ptr, std::less<>> _jobs;
	std::vector<waiter> _waiters;
	std::vector<condition> _conditions;
	std::map<std::string, std::string> _schemas;
	// Each copy of the program a job runs, kept until its job is done: its callbacks end with it.
	std::vector<std::pair<agent_job_ptr, std::shared_ptr<platform::child_process>>> _children;
	uint64_t _next_job = 1;
	mutable std::mutex _jobs_mutex;
};

// A result in words for the model, with the same as a JSON object for a client that reads it.
inline core::mcp::tool_result structured_result(std::string text, std::string json, const bool is_error = false)
{
	auto result = core::mcp::tool_result::text(std::move(text));
	result.structured = std::move(json);
	result.is_error = is_error;
	return result;
}

// Whether the window has caught up and nothing is loading, scanning, searching, indexing, acting out steps or showing
// work under way; and if not, what is still busy.
bool agent_window_settled(const app_frame& app);
std::vector<std::string> agent_window_busy(const app_frame& app);
std::string_view agent_view_name(view_type v);
// The modal dialogs showing, as a JSON array: each one's title, text, buttons, check boxes and fields; and as text.
std::string describe_dialogs_json(app_frame& app);
std::string describe_dialogs_text(app_frame& app);
// The open menu, as a JSON object - each level's rows: text, keys, and whether each is enabled, checked and opens a
// submenu - or null where none is; and as text, empty where none is.
std::string describe_menu_json();
std::string describe_menu_text();
// The view, the search and the items shown, naming the first `listed` of them and of the selection: as a JSON object,
// and as text.
std::string agent_view_json(const app_frame& app, size_t listed);
std::string agent_view_text(const app_frame& app, size_t listed);
// What working a command needs of the session beyond reading - "write", "network", "launch" or "refused" - empty for
// reading; and why the session does not run it, empty where it does.
std::string_view agent_command_need(commands id);
std::string agent_command_refusal(const mcp_host& host, commands id);
// Opens a command's menu as a press on its button does, beside the button where a bar shows it and otherwise in the
// middle of the view; the menu's loop runs inside the call. False where the command has no menu to open.
bool open_command_menu(app_frame& app, const ui::command_ptr& command);
// From now on, refuses each command the session may not run however it is reached, saying why in the log, and counts
// each command run and menu opened, and each control used, for coverage: once the window has made the commands.
void track_command_use(app_frame& app);
// What each dialog showing work under way says - "Saving photo.avif..." - which is waited out, not answered.
std::vector<std::string> dialogs_showing_work();
// Whether a dialog showing waits on an answer: one showing work does not.
bool dialog_awaits_answer();

// Each area's tools.
void add_session_tools(mcp_host& host);
void add_engine_tools(mcp_host& host);
void add_library_tools(mcp_host& host);
void add_ui_tools(mcp_host& host);
void add_control_tools(mcp_host& host);
void add_face_tools(mcp_host& host);

// -tool:<name> name=value...: calls the tool once, without a session, and prints its result: its structured content
// as JSON on stdout where it has some, otherwise its text, and what it reports on the way on stderr. The process's
// exit code: 0 for a result, 1 for an error, 2 for a call that could not be made.
int run_tool_once(const std::shared_ptr<mcp_host>& host, std::string_view name, const std::vector<std::string>& pairs);

// name=value pairs as a tool's JSON arguments, each value read as the input schema types it: a number, true or
// false, a list split at commas, or text. Why not, where a pair cannot be read.
std::string tool_arguments_json(std::string_view input_schema, const std::vector<std::string>& pairs,
                                std::string& error);
