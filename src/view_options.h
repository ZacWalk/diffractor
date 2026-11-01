// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Options view: every setting the user can change, on one page that fills the window. The page
// edits a copy of the settings, which Save options and close commits and Cancel discards; Close, Escape and
// closing Diffractor ask first when the copy holds changes. Options opens it at its top; the sidebar's own
// Customise sidebar link opens it at the Sidebar section.

#pragma once

#include "view_list.h"

enum class options_section
{
	top,
	sidebar,
};

// What leaving the page with changes not saved answers: stay on it, save them, or close without them.
enum class options_leave_choice
{
	stay,
	save,
	discard,
};

// What the view needs of the application beyond the settings: work only the frame can do.
struct options_view_actions
{
	// Loads and shows the app in another language.
	std::function<void(std::string_view code)> select_language;
	// Checks, then defragments or resets the index, behind its own dialogs.
	std::function<void()> index_maintenance;
	// Pauses what is playing for as long as the token lives, as a dialog does while it is open.
	std::function<std::shared_ptr<void>()> pause_media;
	// Asks whether to save the page's changes before it is left. Without one, they are discarded, as Cancel does.
	std::function<options_leave_choice()> ask_to_save;
};

// The question ask_to_save puts, in a dialog over `parent`.
options_leave_choice ask_to_save_options(const ui::control_frame_ptr& parent);

// The folders whose library type the page offers: the collection's local folders that are ticked, the other
// folders it names by path, and any folder given a type before. Device labels and exclusions have no type.
std::vector<df::folder_path> library_type_folders(const settings_t::index_t& collection,
                                                  const local_folders_result& local);

// Copies what the Options view edits from `from` to `to`, and nothing else: what the app changed while the
// page was open - window state, recent lists, the fields a task remembers - keeps its newer value.
void apply_edited_options(const settings_t& from, settings_t& to);

// Whether anything the Options view edits differs between `a` and `b`: the same settings Save copies.
bool edited_options_differ(const settings_t& a, const settings_t& b);

class options_view final : public view_base, public std::enable_shared_from_this<options_view>
{
public:
	// The copy the page edits, with the text forms of what is stored another way. Shared with the panel's
	// controls, which bind to its fields.
	struct edit_state
	{
		settings_t settings;
		std::string more_folders;
		std::vector<std::pair<df::folder_path, prop::library_kind>> library_types;
		std::vector<std::string> library_type_names;
		std::string language_name;
		std::string subtitle_language_name;
		std::string sound_device_name;
		bool subtitles_off = false;
		bool subtitles_always = false;
		bool subtitles_foreign = false;
		bool repeat_none = false;
		bool repeat_one = false;
		bool repeat_all = false;
		bool navigator_auto_hide = false;
		bool navigator_off = false;
	};

private:
	view_state& _state;
	view_host_ptr _host;
	options_view_actions _actions;
	std::shared_ptr<edit_state> _edit;
	// The page's copy as Save would have stored it when the page opened. A change is a difference from this,
	// so one made and then undone is none, and the trip from stored text to the page's own forms and back
	// counts for nothing.
	std::shared_ptr<const settings_t> _opened;
	options_section _section = options_section::top;
	std::shared_ptr<void> _media_pause;
	std::string _title;

	// Back to Items, as every task view returns: Fullscreen is left when another view opens.
	void close();
	// Close, Escape and closing Diffractor all leave here. With nothing to save the page just closes; with
	// changes the user chooses: save them, close without them, or stay. Answers whether the page was left.
	bool leave();

public:
	options_view(view_state& state, view_host_ptr host, options_view_actions actions) :
		_state(state), _host(std::move(host)), _actions(std::move(actions))
	{
		_show_render_window = false;
	}

	// Starts editing a fresh copy of the settings and shows the page at a section.
	void open(options_section section);
	void save();
	// Discards the page's changes without asking: the button says that is what it does.
	void cancel();
	static void help();

	// The copy the page's controls edit; empty until the page opens.
	const std::shared_ptr<edit_state>& editing() const
	{
		return _edit;
	}

	// Whether the page holds a change Save would commit.
	bool has_changes() const;

	view_controls_host_ptr controls(const ui::control_frame_ptr& owner);

	void activate(sizei extent) override
	{
	}

	// Every way out of the page passes here, so what it paused resumes however the page was left.
	void deactivate() override
	{
		_media_pause.reset();
	}

	void refresh() override
	{
	}

	bool escape() override
	{
		leave();
		return true;
	}

	void exit() override
	{
		leave();
	}

	bool confirm_exit() override
	{
		return !has_changes() || leave();
	}

	std::string_view title() override;
	view_bars bars() const override;
};
