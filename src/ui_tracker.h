// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The tracker monitor, drawn as panels of the media stack: where the module is - its order, pattern,
// row, tempo and speed - over the pattern under the playhead, centred in its panel and scrolling smoothly through
// a bar as it plays, with every channel that has anything in it, and its level, mute and solo, in a header that
// stays in view as the pattern scrolls; drawn for a movie, with no controls; and the module's sample and
// instrument names, with how the patterns play them, as a panel below the selection panel.

#pragma once

#include "ui_elements.h"
#include "model_display.h"

// How much of a pattern cell the grid shows: all of it, then without its volume column, then its note and
// instrument, then its note alone - given up in that order, so every channel fits before any is scrolled away.
enum class tracker_cell_detail
{
	note,
	instrument,
	effect,
	full,
};

// A cell as the decoder writes it - note, instrument, volume and effect, separated by spaces - cut to detail.
std::string tracker_cell_text(std::string_view cell, tracker_cell_detail detail);

// How the pattern grid fits its width: the detail every channel is shown at, the characters a cell takes at
// it, and how many channels are shown at once - all of them, unless even their notes alone are too wide.
struct tracker_grid_fit
{
	tracker_cell_detail detail = tracker_cell_detail::full;
	int cell_chars = 0;
	int visible_channels = 0;
};

// cell is one of the module's cells, which the decoder writes at one width for the whole module. width is what
// the grid has for its channels, char_width the code font's advance; a channel is its cell and one character.
tracker_grid_fit fit_tracker_grid(std::string_view cell, int channels, double width, double char_width);

// A note as a tracker writes it - "C#5" - and the notes a sample or instrument is played between, "C-4" for one
// played at a single pitch; nothing for no note, or for one no note plays.
std::string tracker_note_name(int note);
std::string tracker_note_range(const tracker_bank_use& use);

class tracker_monitor_control final : public view_element, public keyboard_parts_source
{
	std::shared_ptr<class tracker_monitor_impl> _impl;

public:
	tracker_monitor_control(view_state& state, display_state_ptr display, view_host_ptr host);
	// Drawn apart from any view, as a frame of a movie is: it scrolls, mutes and solos nothing.
	explicit tracker_monitor_control(display_state_ptr display);
	~tracker_monitor_control() override;

	sizei measure(render::measure_context& mc, int width_limit) const override;
	void layout(render::measure_context& mc, recti bounds) override;
	void render(render::draw_context& dc, pointi element_offset) const override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
	bool wheel(pointi loc, pointi element_offset, int delta) override;
	// The arrows through the channels and Follow playback, then each shown channel's mute and solo.
	std::vector<recti> keyboard_parts() const override;

	// As the last layout placed them, in the element's coordinates: the grid's arrangement, the rows it shows,
	// and where each shown channel's header, mute and solo are.
	tracker_grid_fit grid_fit() const;
	int rows_shown() const;
	recti grid_bounds() const;
	recti header_bounds() const;
	std::vector<recti> channel_headers() const;
	std::vector<recti> mute_toggles() const;
	std::vector<recti> solo_toggles() const;
	recti follow_toggle() const;
};

// The module's sample and instrument names, as a panel of the stack, with how the patterns play the bank they name:
// how many notes, between which, on how many channels. Null until the module has been read.
view_element_ptr make_tracker_bank_panel(const display_state_ptr& display);
