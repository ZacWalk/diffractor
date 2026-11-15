// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tutorial scripts (.dft), for tutorial videos made from the running application: a scene set before the
// recording starts, then beats - a line of narration each, and what is shown while it is spoken. The reader is here,
// with the player that acts a script out in app_frame - its pointer and keys given to the window as its own input,
// never the system's mouse and keyboard - while the recorder (app_record.h) films the window and names where each beat
// starts, so its narration can be laid under the film. The tutorial_run and ui_input tools (app_mcp_ui.cpp) play
// them; ui_input's steps are a script's steps.

#pragma once

#include "app_commands.h"

// A script is one step a line; # starts a comment line. Text is 'single-quoted', reading \n, \' and \\ as .dfv
// does, or "double-quoted" and taken as it stands. The steps before the first say set the scene, before the
// recording starts; each say starts a beat, whose steps run while its narration is spoken. A beat ends once its
// steps are done and its narration has been spoken.
//
//   say <id> '<narration>'      starts a beat. narration\<script>\<id>.timing beside the script says how long its
//                               voice lasts, and where its cues fall; without it, both are guessed from the text.
//                               {name} in the narration marks a cue and is not spoken.
//   cue <name>                  waits for the narration to reach {name}
//   wait <seconds>
//   settle                      waits for searching, indexing and loading to finish
//   point <anchor> [<seconds>]  the pointer glides to the anchor, over 0.8 seconds unless given
//   hide                        hides the pointer
//   click [ctrl] [shift]        clicks what the pointer is on
//   double-click                double-clicks it
//   right-click [ctrl] [shift]  right-clicks it, which opens its menu
//   drag <anchor> [<seconds>]   presses where the pointer is, glides to the anchor - over 0.8 seconds unless given -
//                               and lets go there
//   wheel up|down [<notches>]   turns the wheel where the pointer is, a notch unless given
//   type '<text>'               types into the address box
//   text '<text>'               types into whatever has the keyboard - a box, the view - as a keyboard does
//   key <key>                   presses a key: ctrl+, shift+ or alt+ before a letter, a digit or enter, escape,
//                               space, tab, backspace, delete, insert, left, right, up, down, home, end, page-up,
//                               page-down, plus, minus, apps, or f1 to f12
//   command <name>              runs a command: one tutorial_command names
//   open ['<folder or search>'] opens it, or with none the folder the window opened on
//   select '<file name>' [ctrl] selects an item shown, or with ctrl adds it to the selection
//   tooltips on|off             shows the tooltip where the pointer rests; off until a script turns it on
//   list                        writes what the window shows to the console, naming the items in view: an aid to
//                               writing a script, since the console is all a rehearsal shows of the window
//
// An anchor is where the pointer goes: address (the address box), sidebar, types, map, calendar (the sidebar's
// charts), month <yyyy>-<mm> (one of the calendar's, its year scrolled to), items, preview (the media pane), view
// (the whole view), button <command> (a toolbar's), tile '<file name>' [<x> <y>] (an item's, scrolled to: its
// middle, or x and y across it as fractions of it), or at <x> <y> (fractions of the window from its top left). The
// pointer's input goes where a user's goes, given to the window as its own: a click on the address box takes the
// keyboard there, one on a month opens it, one on a button runs its command or opens its menu, and one on anything
// else - a control, a link, the view - is a click there. Keys and text go to whatever has the keyboard.
//
// A script cannot work a dialog or a menu. A step that opens one - a command that explains why it cannot run, a
// right-click - stops the script, which says so and waits until the dialog or menu is closed: dialog_respond and
// menu_respond answer them.

enum class tutorial_step_kind
{
	cue,
	wait,
	settle,
	point,
	hide,
	click,
	double_click,
	right_click,
	drag,
	wheel,
	type,
	text,
	key,
	command,
	open,
	select,
	tooltips,
	list,
};

enum class tutorial_anchor_kind
{
	address,
	sidebar,
	types,
	map,
	calendar,
	month,
	items,
	preview,
	view,
	button,
	tile,
	at,
};

struct tutorial_anchor
{
	tutorial_anchor_kind kind = tutorial_anchor_kind::view;
	// A tile's file name.
	std::string name;
	commands command = commands::none;
	// A month's year, and its month from 1.
	int year = 0;
	int month = 0;
	// at: from the window's top left, and tile: from the tile's, as fractions of its width and height.
	double x = 0.0;
	double y = 0.0;
};

struct tutorial_step
{
	tutorial_step_kind kind = tutorial_step_kind::wait;
	int line = 0;
	// wait's length; point's and drag's glide.
	double seconds = 0.0;
	// cue's name; type's, text's, open's and select's text.
	std::string text;
	tutorial_anchor anchor;
	commands command = commands::none;
	char32_t key = 0;
	// The modifiers of a click, a key or a select.
	platform::key_state keys;
	// tooltips on.
	bool on = false;
	// wheel's turn: notches away from the user - up - or towards, down, below zero.
	int notches = 0;
};

struct tutorial_beat
{
	std::string id;
	// As written, cues and all.
	std::string narration;
	int line = 0;
	std::vector<tutorial_step> steps;
};

struct tutorial_script
{
	std::vector<tutorial_step> scene;
	std::vector<tutorial_beat> beats;
	// "line <n>: <problem>", for each line that could not be read. A script with any is not played.
	std::vector<std::string> problems;
};

tutorial_script parse_tutorial_script(std::string_view text);

// The commands a script names, and the name each has.
std::optional<commands> tutorial_command(std::string_view name);
// A key as a script's key step names it - ctrl+, shift+ and alt+ before a letter, a digit or a key's name - and its
// modifiers, added to keys. False for one that is no key.
bool parse_tutorial_key(std::string_view chord, char32_t& key, platform::key_state& keys);

// How long a beat's voice lasts, and when it reaches each of its cues, in seconds from its start.
struct tutorial_narration
{
	double seconds = 0.0;
	std::vector<std::pair<std::string, double>> cues;
	// Measured from the voice; otherwise guessed from the text.
	bool measured = false;

	std::optional<double> cue(std::string_view name) const;
};

// The narration as it is spoken: its cues taken out.
std::string tutorial_spoken_text(std::string_view narration);
// Guessed from the text, at an even pace: before there is a voice, a rehearsal runs to this.
tutorial_narration estimate_tutorial_narration(std::string_view narration);
// A .timing file, as tutorial.py writes it: "length<TAB><seconds>", then "cue<TAB><name><TAB><seconds>" for each
// cue, one a line. Nothing for one that does not give a length.
std::optional<tutorial_narration> parse_tutorial_timing(std::string_view text);

class app_frame;

// What a played script came to.
struct tutorial_outcome
{
	bool ok = false;
	std::string summary;
	// Steps that found nothing to act on: an anchor not shown, a click on nothing.
	int missed = 0;
	// When each beat starts in the film, in seconds, and how long the film runs.
	std::vector<std::pair<std::string, double>> beats;
	double film_seconds = 0.0;
	// The film and its markers, where one was made.
	core::file_path movie;
	core::file_path markers;
};

struct tutorial_options
{
	// The script's file - its narration's timing is read from beside it - or, where it is given, its text.
	core::file_path script;
	std::string text;
	// Where the film goes; empty rehearses, recording nothing.
	core::file_path record;
	// Steps alone, as ui_input sends them: no beats, no recording, and no settling but what the steps ask for. A
	// dialog or menu opening ends them, since what they would act on is behind it.
	bool steps_only = false;
	// Why a command the steps would run may not be, or empty where it may: an agent session refuses one needing a tier
	// it was not granted, as command_run does. A script that would run one is not played. Unset, any may be.
	std::function<std::string(commands)> refusal;
	// Each line the player says as it goes, and its outcome once it has finished, on the UI thread.
	std::function<void(std::string_view)> said;
	std::function<void(const tutorial_outcome&)> finished;
};

// Acts a script out in the window, a step at a time from the frame tick: sets its scene, waits for the window to
// settle, starts its recording if it is to make one, plays each beat, and says how it went once it has finished.
class tutorial_player
{
public:
	tutorial_player(app_frame& app, tutorial_options options);
	~tutorial_player();
	tutorial_player(const tutorial_player&) = delete;
	tutorial_player& operator=(const tutorial_player&) = delete;

	void tick();
	bool shows_tooltips() const { return _tooltips; }
	bool is_finished() const { return _stage == stage::finished; }
	// Stops it where it is, as a client cancelling its call does.
	void cancel();

private:
	enum class stage
	{
		loading,
		waiting_for_window,
		setting_scene,
		starting_recording,
		lead_in,
		beats,
		lead_out,
		finished,
	};

	struct loaded
	{
		tutorial_script script;
		std::vector<tutorial_narration> narration;
		std::string failure;
	};

	void load();
	void loaded_script(loaded result);
	// The steps that would run a command the options refuse, each as "line <n>: <why>".
	std::vector<std::string> refused_steps(const tutorial_script& script) const;
	void enter(stage s);
	// The step running now, if one is.
	const tutorial_step* current_step() const;
	// Runs the steps that are due; true once every one has finished.
	bool run_steps(const std::vector<tutorial_step>& steps, double beat_time, const tutorial_narration* narration);
	// Starts a step: false where it has nothing more to do.
	void begin_step(const tutorial_step& step);
	// Whether a started step has finished.
	bool step_is_done(const tutorial_step& step, double beat_time, const tutorial_narration* narration);
	bool is_settled();
	std::optional<pointi> anchor_point(const tutorial_anchor& anchor);
	void move_pointer(std::optional<pointi> to, double seconds);
	pointi pointer_now() const;
	void hover();
	// Pointer input where the drawn pointer is, given to the window as its own; false where the window takes none.
	bool send_pointer_input(ui::pointer_input input, platform::pointer_button button, platform::key_state keys,
	                        pointi at);
	void click(bool twice, platform::key_state keys);
	void right_click(platform::key_state keys);
	void wheel(int notches);
	void key(char32_t key, platform::key_state keys);
	void finish(std::string_view outcome, bool ok);
	void say(std::string_view text) const;
	// What the window shows: the view, the search and how many items are selected, for the log of a run.
	std::string shown() const;
	double now() const;

	app_frame& _app;
	tutorial_options _options;
	tutorial_outcome _outcome;
	std::shared_ptr<bool> _alive = std::make_shared<bool>(true);

	stage _stage = stage::loading;
	double _stage_started = 0.0;
	tutorial_script _script;
	std::vector<tutorial_narration> _narration;

	size_t _beat = 0;
	double _beat_started = 0.0;
	size_t _step = 0;
	bool _step_begun = false;
	double _step_started = 0.0;
	double _step_seconds = 0.0;
	double _settled_since = -1.0;
	size_t _typed = 0;

	// The pointer, on the screen: shown, gliding from one point to another over a time.
	bool _pointer_shown = false;
	pointi _pointer_from;
	pointi _pointer_to;
	double _pointer_moved = 0.0;
	double _pointer_seconds = 0.0;
	std::optional<tutorial_anchor> _pointed;
	std::optional<pointi> _hovered;
	// A drag under way: the pointer holds what it pressed, which follows it as it glides.
	bool _dragging = false;
	pointi _dragged_to;

	bool _tooltips = false;
	// Whether the recording is the one it started, which it ends; one it did not start it only draws into.
	bool _owns_recording = false;
	double _started = 0.0;
	// When the film starts: the recording's first frame, or for a rehearsal the moment one would have been.
	double _film_started = 0.0;
	// In a tick, which a dialog or menu a step opens ticks again from inside; and waiting for one to close.
	bool _ticking = false;
	bool _waiting = false;
};
