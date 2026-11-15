// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Commodore 64 in the media pane: the listing of a disk, a tape or a program file, whose rows run
// what they name; a cartridge, drawn as one with its title on its label; and the machine running either, with
// the way back. The machine itself is c64_session (av_c64.h).

#pragma once

#include "ui_elements.h"
#include "model_display.h"
#include "libfiles/commodore.h"

// The size the machine's picture is drawn at in space of avail: as large as fits, keeping its shape, and at a
// whole multiple of its pixels when that gives up at most a fifth of the size, so they stay square and sharp.
// whole says which, so the picture is point sampled at a whole multiple and filtered otherwise.
sizei fit_c64_picture(sizei avail, bool& whole);

// Whether a Commodore 64 file is a cartridge, which holds one thing to run and is shown as itself.
bool is_c64_cartridge(const file_type& type);

// The title a cartridge's label carries: the name its header gives it, or the file's when that is blank.
std::string c64_cartridge_title(const libfiles::c64_listing& listing, std::string_view file_name);

// Draws Commodore's own cartridge for the C64 and VIC-20 into r, standing as it goes into the machine and in its
// shell's proportions, with the depth the sidebar's calendar gives its blocks: a black shell whose grip ridges cross
// its top third and wrap down its side, a silver label carrying title in Commodore's blue beside its rainbow in the
// recess below them, and the board's gold contacts under it. shadow keeps the soft shadow it stands in between
// draws, made again when the cartridge changes size.
void draw_c64_cartridge(render::draw_context& dc, recti r, std::string_view title, bool hot,
                        render::texture_ptr& shadow);

// The listing as the C64 lists a disk, in its own blue. A row naming something the machine can start is drawn
// reversed under the pointer and runs it when clicked; a file holding just one such thing - a program file, a
// tape of one program - has a Start button under it as well.
class commodore_disk_control final :
	public view_element,
	public std::enable_shared_from_this<commodore_disk_control>,
	public keyboard_parts_source
{
public:
	commodore_disk_control(view_state& state, display_state_ptr display, const view_element_options& style_in);

	sizei measure(render::measure_context& mc, int width_limit) const override;
	void layout(render::measure_context& mc, recti bounds_in) override;
	void render(render::draw_context& dc, pointi element_offset) const override;
	void hover(interaction_context& ic) override;
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;
	void dispatch_event(const view_element_event& event) override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
	// Each row that runs, then Start.
	std::vector<recti> keyboard_parts() const override;

	// What a point in the element's coordinates is on: the number of a row that runs, Start, or nothing. And
	// where a part is, in the same coordinates. As the last layout placed them.
	static constexpr int no_part = -1;
	static constexpr int start_part = -2;
	int part_at(pointi loc) const;
	recti part_bounds(int part) const;
	// The row Start runs; -1 when there is no Start button.
	int start_row() const { return _start_row; }

private:
	double fit_scale(int inner_width, double scale_factor) const;

	view_state& _state;
	display_state_ptr _display;

	libfiles::c64_listing _listing;
	int _cols = 0;
	int _rows = 0;
	// Whether each row names something the machine can start.
	std::vector<bool> _runs;
	int _start_row = -1;

	// Where the listing's characters and Start were placed, and the listing's scale.
	recti _listing_bounds;
	recti _start_bounds;
	double _scale = 1.0;

	// The part under the pointer, whether it is held down, and the part a press began on.
	int _hot = no_part;
	bool _pressed = false;
	int _tracked = no_part;

	mutable render::texture_ptr _texture;
	// The listing again in reverse video, light blue on blue as the C64 shows a reversed character, for the
	// row under the pointer.
	mutable render::texture_ptr _reversed;
	mutable sizei _tex_extent;
};

// A cartridge file in the media pane: the cartridge drawn as one, in a panel of the stack, with Start under it.
// A click on the cartridge or on Start runs it, as Start under a program's listing does.
class c64_cartridge_control final :
	public view_element,
	public std::enable_shared_from_this<c64_cartridge_control>,
	public keyboard_parts_source
{
public:
	c64_cartridge_control(view_state& state, display_state_ptr display);

	sizei measure(render::measure_context& mc, int width_limit) const override;
	void layout(render::measure_context& mc, recti bounds_in) override;
	void render(render::draw_context& dc, pointi element_offset) const override;
	void hover(interaction_context& ic) override;
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;
	void dispatch_event(const view_element_event& event) override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
	// Start, which the cartridge itself does too.
	std::vector<recti> keyboard_parts() const override;

	enum class part
	{
		none,
		cartridge,
		start,
	};

	// As the last layout placed them, in the element's coordinates.
	part part_at(pointi loc) const;
	recti part_bounds(part p) const;
	const std::string& title() const { return _title; }
	// Whether the cartridge holds something the machine can start.
	bool runs() const { return _row >= 0; }

private:
	view_state& _state;
	display_state_ptr _display;

	std::string _title;
	int _row = -1;

	recti _cartridge;
	recti _start;

	part _hot = part::none;
	bool _pressed = false;
	part _tracked = part::none;

	mutable render::texture_ptr _shadow;
};

// The running machine: its picture, fitted to the pane, and under it Back, the port its joystick is in and the
// keys it takes. The keys reach it through view_state::c64_key_down while the view has the focus.
class c64_screen_control final :
	public view_element,
	public std::enable_shared_from_this<c64_screen_control>,
	public keyboard_parts_source
{
public:
	c64_screen_control(view_state& state, display_state_ptr display);

	sizei measure(render::measure_context& mc, int width_limit) const override;
	void layout(render::measure_context& mc, recti bounds_in) override;
	void render(render::draw_context& dc, pointi element_offset) const override;
	void hover(interaction_context& ic) override;
	void dispatch_event(const view_element_event& event) override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
	// Back, then the joystick's port while the machine runs. The picture is the machine's, which has the keys.
	std::vector<recti> keyboard_parts() const override;

	enum class part
	{
		none,
		back,
		port,
	};

	// As the last layout placed them, in the element's coordinates.
	part part_at(pointi loc) const;
	recti part_bounds(part p) const;
	recti picture_bounds() const { return _picture; }

private:
	view_state& _state;
	display_state_ptr _display;

	recti _picture;
	bool _whole_scale = true;
	recti _back;
	recti _port;
	recti _hint;

	part _hot = part::none;
	bool _pressed = false;
	part _tracked = part::none;
};
