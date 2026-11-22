// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: A flex-based SID monitor in Diffractor's colours, drawn as panels of the media stack: the file's other
// tunes, each voice's waveform, note, envelope and ADSR, the chip's output, its filter and volume, and the
// registers the tune writes - with the tune selector and per-voice mute and solo as controls, which the keyboard
// reaches as the pointer does and one drawn for a movie leaves out. What moves with the music is drawn through a
// vertex buffer the display updates for every present, as the audio visualizer is.

#pragma once

#include "ui_elements.h"
#include "model_display.h"

// Where the monitor puts each part. Computed from the same inputs for drawing and for clicks, so the two
// cannot disagree.
struct sid_monitor_layout
{
	view_elements_ptr panels;
	recti panel; // all of the monitor's panels, as wide as the space it is given
	recti tunes; // the tune selector's panel; empty for a file with one tune, or a monitor without controls
	recti tunes_label;
	std::vector<recti> tune_chips; // the first tune's first; empty when there are too many to show
	recti previous_tune;           // used instead of chips when there are too many
	recti next_tune;
	recti tune_counter;
	std::array<recti, 3> voices = {};
	std::array<recti, 3> mute = {}; // empty for a monitor without controls
	std::array<recti, 3> solo = {};
	bool controls = true;      // the tune selector and each voice's mute and solo, which a movie's frame has no use for
	bool compact = false;      // no ADSR row: the voices keep only their waveform, meters and gate
	bool buttons_wrap = false; // mute and solo take a row of their own below the gate and the note
	recti output;
	recti filter;
	bool filter_wraps = false; // the mode and routing badges take a row of their own below the meters
	recti registers;
	// Upright, the grid stands beside the voices: a column for each voice's registers, the filter's below them.
	bool registers_by_voice = false;
	int height = 0; // of all the panels and the gaps between them
};

// bounds is where the element sits; scale the display scale; mc supplies the user's font metrics;
// button_text the width of the wider of the mute and solo labels. detail says how much to show when space is
// short: 4 is everything, 3 drops the register grid, 2 the filter as well, 1 the output, and 0 is compact. Without
// controls there is no tune selector and no mute or solo. upright, as a Short's frame is, stacks the voices, all one
// width, with the register grid down their right side, and the output and the filter across the column below.
sid_monitor_layout layout_sid_monitor(recti bounds, double scale, int songs, render::measure_context& mc,
                                      int detail = 4, int button_text = 0, bool controls = true, bool upright = false);

// The fullest layout that fits bounds, at its top: at full size if it can be, as large as fits down to three
// quarters of it if that is enough, and with fewer parts, in detail's order, only when it is not. used_scale is
// the scale it was laid out at. Its height says what it took; a compact monitor still too tall is cut off.
sid_monitor_layout fit_sid_monitor(recti bounds, double scale, int songs, render::measure_context& mc, int button_text,
                                   double& used_scale, bool controls = true, bool upright = false);

// How far the register grid's highlight has faded for each register: 1 for one written with a new value in
// the frame shown, falling to 0 over the frames before it. frames are oldest first, the shown frame last.
std::array<float, sid_register_count> sid_register_activity(const sid_frame_state* frames, size_t count);

// What of the monitor moves with the music - the voices' waveforms, meters and gates, the output scope, the
// filter's meters and lamps, the volume and the register grid's highlight - drawn through a vertex buffer, so a
// playing tune is presented every frame without its scene being rebuilt. The monitor places the parts each time
// it draws; the display brings them up to the session's position for every present in between.
class sid_monitor_motion final : core::no_copy
{
public:
	struct voice_parts
	{
		recti shape;
		recti frequency; // the meters' tracks
		recti envelope;
		recti gate;
	};

	// Where each part goes, in the view's coordinates; empty for a part the layout left out.
	struct parts
	{
		std::array<voice_parts, 3> voices = {};
		recti scope;
		recti cutoff;
		recti resonance;
		std::array<recti, 6> filter_lamps = {}; // LP, BP and HP, then V1, V2 and V3
		std::array<recti, 15> volume = {};
		std::array<recti, sid_register_count> registers = {};
		int thickness = 2; // of the traces, in pixels
		int column = 1;    // the width of a trace's columns
		int inset = 1;     // between a lamp and the badge it lights
	};

	// Called by the monitor as it draws: where the parts are now, what they show, and the frame the text
	// it drew beside them came from.
	void place(const parts& p, std::shared_ptr<sid_tune_render> sid, const sid_voice_mix& mix, float alpha,
	           double position, const sid_frame_state& text_state, double time_now);

	// Brings the parts up to a playback position. present when they moved; invalid when the text beside them
	// has fallen behind, or the tune is no longer the one placed, so the scene is due to be rebuilt; valid
	// when there is nothing to do - which is also the answer once the monitor is no longer being drawn.
	render_valid update(const std::shared_ptr<sid_tune_render>& sid, double position, double time_now);

	// The geometry for a playback position, as update hands it to the vertex buffer. Exposed for the tests.
	const std::vector<render::vertex_rect>& build(double position);

	render::vertices_ptr _verts;

private:
	parts _parts;
	std::shared_ptr<sid_tune_render> _sid;
	sid_voice_mix _mix;
	float _alpha = 1.0f;
	double _built_position = -1.0;
	std::vector<render::vertex_rect> _rects;

	// The frame the monitor's text was last drawn from, and when.
	sid_frame_state _text_state;
	double _text_time = 0.0;
};

class sid_monitor_control final :
	public view_element,
	public std::enable_shared_from_this<sid_monitor_control>,
	public keyboard_parts_source
{
public:
	sid_monitor_control(view_state& state, display_state_ptr display);
	// Drawn apart from any view, as a frame of a movie is: it plays, picks and mixes nothing, so it shows no
	// controls to do so.
	explicit sid_monitor_control(display_state_ptr display);

	// A Short's upright frame stacks the voices beside the register grid; see layout_sid_monitor.
	void arrange_upright(bool upright);

	sizei measure(render::measure_context& mc, int width_limit) const override;
	void layout(render::measure_context& mc, recti bounds) override;
	void render(render::draw_context& dc, pointi element_offset) const override;
	void dispatch_event(const view_element_event& event) override;
	void visit_children(const std::function<void(const view_element_ptr&)>& visit) const override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
	// The tune selector's chips or arrows, then each voice's mute and solo, as the layout last drawn placed them.
	std::vector<recti> keyboard_parts() const override;

private:
	int song_count() const;
	void update_layout(render::measure_context& mc, bool force = false) const;

	// Null for a monitor drawn apart from any view.
	view_state* _state = nullptr;
	display_state_ptr _display;
	bool _upright = false;
	mutable view_elements_ptr _panels;
	mutable recti _layout_bounds;
	mutable double _layout_scale = 0;
	mutable double _used_scale = 1;
	mutable int _layout_line_height = 0;
	mutable int _layout_button_width = 0;
	mutable int _layout_songs = 0;
	mutable bool _layout_upright = false;

	// The layout last drawn, for the clicks to find their targets in. UI thread only.
	mutable sid_monitor_layout _drawn;
	mutable int _measured_songs = 0;
};
