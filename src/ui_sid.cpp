// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The SID monitor, the presentation of a playing SID tune, laid out as turbo8bit's SID page lays it
// out and drawn in Diffractor's colours: each voice's waveform, note, envelope and ADSR, the chip's output, its
// filter and volume, and the registers the tune writes - with the file's other tunes, and per-voice mute and
// solo, as controls. What moves with the music is drawn through a vertex buffer the display updates for every
// present, as the audio visualizer is.

#include "pch.h"
#include "app_text.h"
#include "model.h"
#include "ui_sid.h"
#include "ui_view.h"

namespace
{
	// A translucent surface colour: black sinks a panel into what is behind it and white raises a control off
	// it, as Diffractor's own panels do, so the monitor sits on whatever the view draws behind it.
	struct shade
	{
		ui::color32 rgb;
		float alpha;
	};

	// Diffractor's own palette: its panels' shades, its view text, its selection blue for what is lit, and its
	// success, important and warning colours for the gate and the envelope, the solo and the mute.
	namespace palette
	{
		constexpr shade panel{0x000000, 0.22f};
		constexpr shade sunken{0x000000, 0.3f};
		constexpr shade raised{0xFFFFFF, 0.12f};
		constexpr shade line{0xFFFFFF, 0.1f};

		ui::color32 text() { return ui::style::color::view_text; }
		ui::color32 text_muted() { return ui::lerp(ui::style::color::view_text, ui::style::color::view_background, 0.35f); }
		ui::color32 text_faint() { return ui::lerp(ui::style::color::view_text, ui::style::color::view_background, 0.6f); }
		ui::color32 accent() { return ui::style::color::view_selected_background; }
		ui::color32 accent_light() { return ui::lerp(ui::style::color::view_selected_background, ui::style::color::view_text, 0.4f); }
		ui::color32 information() { return ui::style::color::info_background; }
		ui::color32 green() { return ui::style::color::success_background; }
		ui::color32 orange() { return ui::style::color::important_background; }
		ui::color32 red() { return ui::style::color::warning_background; }
	}

	// A silenced voice is drawn faded, as the tune goes on playing it.
	constexpr float silenced_alpha = 0.45f;

	// How often the values, notes and register contents beside the moving parts are redrawn while they change.
	// They are text, which only rebuilding the scene redraws, and ten times a second is as fast as they can be
	// read.
	constexpr double text_refresh_seconds = 0.1;

	// A monitor not drawn for this long is not on screen. The view is rebuilt at least once a second while a
	// tune plays, as the elapsed time it shows changes.
	constexpr double hidden_after_seconds = 2.0;

	// As the data sheet names the registers $D400-$D418.
	constexpr std::string_view register_names[sid_register_count] = {
		"FQ1L", "FQ1H", "PW1L", "PW1H", "CTL1", "AD1", "SR1",
		"FQ2L", "FQ2H", "PW2L", "PW2H", "CTL2", "AD2", "SR2",
		"FQ3L", "FQ3H", "PW3L", "PW3H", "CTL3", "AD3", "SR3",
		"FCLO", "FCHI", "RESF", "MVOL",
	};

	constexpr int register_columns = 7;
	constexpr int register_rows = (sid_register_count + register_columns - 1) / register_columns;

	// Frames the register grid's highlight takes to fade: about a tenth of a second, as on turbo8bit.
	constexpr size_t activity_frames = 6;

	// Samples the scope reads behind the playback position, and the most it shows of them after the
	// trigger.
	constexpr size_t scope_window = 2048;
	constexpr size_t scope_visible = 1024;

	// A press that runs one action on release, so a press the user drags away from is abandoned.
	class sid_click_controller final : public view_controller
	{
	public:
		sid_click_controller(view_host_ptr host, const recti bounds, std::function<void()> invoke) :
			view_controller(std::move(host), bounds), _invoke(std::move(invoke))
		{
		}

		ui::style::cursor cursor() const override { return ui::style::cursor::link; }

		void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
		{
			if (_bounds.contains(loc)) _invoke();
		}

	private:
		std::function<void()> _invoke;
	};

	// Text the monitor draws after its moving parts, so a label reads over the lamp behind it.
	struct queued_text
	{
		std::string text;
		recti bounds;
		ui::color colour;
		ui::style::text_style style = ui::style::text_style::single_line;
	};

	// What the monitor draws with: the context, the alpha everything fades by, the display scale, and the
	// text it holds back until the moving parts are drawn.
	struct painter
	{
		ui::draw_context& dc;
		float alpha;
		double scale;
		std::vector<queued_text>& texts;

		int px(const double v) const { return std::max(1, df::round(v * scale)); }
		ui::color c(const ui::color32 rgb, const float a = 1.0f) const { return {rgb, alpha * a}; }
		ui::color c(const shade s, const float a = 1.0f) const { return {s.rgb, alpha * a * s.alpha}; }

		void fill(const recti r, const ui::color colour, const int radius) const
		{
			dc.draw_rounded_rect(r, colour, radius);
		}

		void text(const std::string_view s, const recti r, const ui::color32 colour, const float a = 1.0f,
		          const ui::style::text_style style = ui::style::text_style::single_line) const
		{
			texts.push_back({std::string(s), r, c(colour, a), style});
		}

		int text_width(const std::string_view s) const
		{
			return dc.measure_text(s, ui::style::font_face::code, ui::style::text_style::single_line, 10000).cx;
		}

		void badge(const std::string_view s, const recti r, const ui::color fill_colour, const ui::color32 colour,
		           const float a = 1.0f) const
		{
			fill(r, fill_colour, px(3));
			text(s, r, colour, a, ui::style::text_style::single_line_center);
		}
	};

	void add_rect(std::vector<ui::vertex_rect>& out, const recti r, const ui::color colour)
	{
		if (!r.is_empty() && colour.a > 0.0f) out.push_back({r, colour});
	}

	// The vertex buffer draws rectangles, not lines, so a trace is narrow columns, each joined to the one
	// before it. ys holds one height per column.
	void add_trace(std::vector<ui::vertex_rect>& out, const recti r, const std::vector<int>& ys, const int thickness,
	               const ui::color colour)
	{
		if (ys.empty()) return;

		const auto column = static_cast<double>(r.width()) / static_cast<double>(ys.size());

		for (auto i = 0_z; i < ys.size(); ++i)
		{
			const auto y = ys[i];
			const auto previous = i > 0 ? ys[i - 1] : y;
			const auto left = r.left + df::round(i * column);
			const auto right = std::max(left + 1, r.left + df::round((i + 1) * column));
			const auto top = std::min(y, previous) - thickness / 2;
			const auto bottom = std::max(y, previous) + (thickness + 1) / 2;
			add_rect(out, recti(left, top, right, bottom), colour);
		}
	}

	// A meter's fill, inside its track.
	void add_meter(std::vector<ui::vertex_rect>& out, const recti track, const double fraction, const int inset,
	               const ui::color colour)
	{
		if (track.is_empty()) return;

		const auto inner = track.inflate(-inset);
		const auto width = df::round(inner.width() * std::clamp(fraction, 0.0, 1.0));
		if (width > 0) add_rect(out, recti(inner.left, inner.top, inner.left + width, inner.bottom), colour);
	}

	// One voice's registers, decoded.
	struct voice_view
	{
		uint16_t frequency = 0;
		uint16_t pulse_width = 0;
		uint8_t control = 0;
		int attack = 0, decay = 0, sustain = 0, release = 0;
		uint8_t envelope = 0;

		voice_view(const sid_frame_state& f, const int v)
		{
			const auto* const r = f.regs.data() + v * 7;
			frequency = static_cast<uint16_t>(r[0] | (r[1] << 8));
			pulse_width = static_cast<uint16_t>((r[2] | (r[3] << 8)) & 0x0FFF);
			control = r[4];
			attack = r[5] >> 4;
			decay = r[5] & 0x0F;
			sustain = r[6] >> 4;
			release = r[6] & 0x0F;
			envelope = f.envelope[v];
		}

		bool gate() const { return (control & 0x01) != 0; }
	};

	// Two cycles of the voice's waveform, as tall as its envelope, so pulse width and the ADSR read as
	// shapes rather than numbers - turbo8bit's sketch, noise included.
	std::vector<int> voice_shape(const voice_view& v, const recti r, const int columns)
	{
		std::vector<int> ys(static_cast<size_t>(std::max(columns, 1)));
		const auto mid = (r.top + r.bottom) / 2;
		const auto amplitude = (v.envelope / 255.0) * (r.height() / 2.0 - 3.0);
		const auto duty = v.pulse_width / 4096.0;

		for (auto x = 0_z; x < ys.size(); ++x)
		{
			const auto phase = std::fmod(static_cast<double>(x) / static_cast<double>(ys.size()) * 2.0, 1.0);
			double level = 0.0;

			if (v.control & 0x80)
			{
				// Any fixed scatter will do: it is the texture of noise, not a sample of it.
				const auto h = (static_cast<uint32_t>(x) + 1u) * 2654435761u;
				level = static_cast<double>((h >> 8) & 0xFFFF) / 32767.5 - 1.0;
			}
			else if (v.control & 0x40) level = phase < duty ? 1.0 : -1.0;
			else if (v.control & 0x20) level = phase * 2.0 - 1.0;
			else if (v.control & 0x10) level = phase < 0.5 ? phase * 4.0 - 1.0 : 3.0 - phase * 4.0;

			ys[x] = mid - df::round(level * amplitude);
		}

		return ys;
	}

	// The output scope, triggered on a rising zero crossing so a steady note stands still.
	std::vector<int> scope_trace(const int16_t* samples, const size_t count, const recti r, const int columns)
	{
		std::vector<int> ys(static_cast<size_t>(std::max(columns, 1)));
		const auto mid = (r.top + r.bottom) / 2;
		size_t start = 0;

		for (auto i = 1_z; i < count / 2; ++i)
		{
			if (samples[i - 1] < 0 && samples[i] >= 0)
			{
				start = i;
				break;
			}
		}

		const auto visible = std::min(scope_visible, count - start);
		const auto half = r.height() / 2.0 - 4.0;

		for (auto x = 0_z; x < ys.size(); ++x)
		{
			double level = 0.0;

			if (visible > 0)
			{
				const auto at = start + std::min(visible - 1, x * visible / ys.size());
				level = std::clamp(samples[at] / 32768.0 * 2.5, -1.0, 1.0);
			}

			ys[x] = mid - df::round(level * half);
		}

		return ys;
	}

	std::string hex2(const int v)
	{
		return std::format("{:02X}", v & 0xFF);
	}
}

sid_monitor_layout layout_sid_monitor(const recti bounds, const double scale, const int songs, const int line_height,
                                      const int detail, const int button_text)
{
	const auto px = [scale](const double v) { return std::max(1, df::round(v * scale)); };
	const auto lh = std::max(line_height, px(10));
	const auto pad = px(12);
	const auto gap = px(10);

	sid_monitor_layout l;
	l.compact = detail <= 0;
	const auto width = std::min(bounds.width(), px(980));
	l.panel = recti(bounds.left + (bounds.width() - width) / 2, bounds.top,
	                bounds.left + (bounds.width() - width) / 2 + width, bounds.top);

	const auto x0 = l.panel.left + pad;
	const auto x1 = l.panel.right - pad;
	auto y = l.panel.top + pad;

	// The monitor's name, then the tune's title, then its author and release. Compact leaves them to the
	// panel the monitor sits in, which names the file anyway.
	if (!l.compact)
	{
		l.heading = recti(x0, y, x1, y + lh * 3 + px(8));
		y = l.heading.bottom + gap;
	}

	if (songs > 1)
	{
		const auto chip_w = std::max(px(30), lh * 2);
		const auto chip_h = lh + px(6);
		const auto chip_gap = px(4);
		const auto label_w = px(72);
		const auto chips_left = x0 + label_w + chip_gap;
		const auto per_row = std::max(1, (x1 - chips_left + chip_gap) / (chip_w + chip_gap));
		const auto rows = (songs + per_row - 1) / per_row;

		l.tunes_label = recti(x0, y, x0 + label_w, y + chip_h);

		if (rows <= 3)
		{
			for (int i = 0; i < songs; ++i)
			{
				const auto left = chips_left + (i % per_row) * (chip_w + chip_gap);
				const auto top = y + (i / per_row) * (chip_h + chip_gap);
				l.tune_chips.emplace_back(left, top, left + chip_w, top + chip_h);
			}

			l.tunes = recti(x0, y, x1, y + rows * chip_h + (rows - 1) * chip_gap);
		}
		else
		{
			// Too many to show at once: step through them instead.
			l.previous_tune = recti(chips_left, y, chips_left + chip_w, y + chip_h);
			l.tune_counter = recti(l.previous_tune.right + chip_gap, y,
			                       l.previous_tune.right + chip_gap + px(96), y + chip_h);
			l.next_tune = recti(l.tune_counter.right + chip_gap, y, l.tune_counter.right + chip_gap + chip_w,
			                    y + chip_h);
			l.tunes = recti(x0, y, x1, y + chip_h);
		}

		y = l.tunes.bottom + gap;
	}

	// The voices sit side by side when there is room, as turbo8bit's grid does, and stack when not. Mute and
	// solo sit beside the gate and the note when they fit, and below them when not.
	const auto inner = x1 - x0;
	const auto min_voice_w = px(l.compact ? 150 : 190);
	const auto columns = std::clamp((inner + gap) / (min_voice_w + gap), 1, 3);
	const auto voice_w = (inner - (columns - 1) * gap) / columns;
	const auto button_h = lh + px(4);
	const auto button_w = std::max(px(40), button_text + px(12));
	// The gate badge and a note such as C#-1, in the code font: about four line heights.
	const auto gate_and_note = lh * 4 + px(20);
	l.buttons_wrap = gate_and_note + button_w * 2 + px(4) > voice_w - pad * 2;
	const auto meter_rows = l.compact ? 2 : 3;
	const auto voice_h = pad * 2 + (lh + px(9)) + (px(36) + px(8)) + meter_rows * (lh + px(6)) + button_h +
		(l.buttons_wrap ? button_h + px(6) : 0);

	for (int v = 0; v < 3; ++v)
	{
		const auto left = x0 + (v % columns) * (voice_w + gap);
		const auto top = y + (v / columns) * (voice_h + gap);
		const recti panel(left, top, left + voice_w, top + voice_h);
		l.voices[v] = panel;

		if (l.buttons_wrap)
		{
			// A row of their own, shared between the two.
			const auto half = (voice_w - pad * 2 - px(4)) / 2;
			l.mute[v] = recti(panel.left + pad, panel.bottom - pad - button_h, panel.left + pad + half,
			                  panel.bottom - pad);
			l.solo[v] = recti(panel.right - pad - half, l.mute[v].top, panel.right - pad, l.mute[v].bottom);
		}
		else
		{
			l.solo[v] = recti(panel.right - pad - button_w, panel.bottom - pad - button_h, panel.right - pad,
			                  panel.bottom - pad);
			l.mute[v] = l.solo[v].offset(-(button_w + px(4)), 0);
		}
	}

	const auto voice_rows = (3 + columns - 1) / columns;
	y += voice_rows * voice_h + (voice_rows - 1) * gap + gap;

	if (detail >= 2)
	{
		l.output = recti(x0, y, x1, y + pad * 2 + lh + px(8) + px(90));
		y = l.output.bottom + gap;
	}

	// The cutoff and resonance meters share the row with the six mode and routing badges where they fit,
	// as on turbo8bit; a narrow column puts the badges on a row of their own.
	l.filter_wraps = inner < px(560);

	if (detail >= 3)
	{
		const auto badge_row_h = l.filter_wraps ? lh + px(8) + px(10) : 0;
		l.filter = recti(x0, y, x1, y + pad * 2 + lh + px(8) + (lh * 2 + px(18)) + badge_row_h + px(12) + px(24));
		y = l.filter.bottom + gap;
	}

	if (detail >= 4)
	{
		const auto cell_h = lh * 2 + px(8);
		l.registers = recti(x0, y, x1,
		                    y + pad * 2 + lh + px(8) + register_rows * cell_h + (register_rows - 1) * px(4));
		y = l.registers.bottom + gap;
	}

	y += pad - gap;
	l.panel.bottom = y;
	l.height = y - l.panel.top;
	return l;
}

sid_monitor_layout fit_sid_monitor(const recti bounds, const double scale, const int songs, const int line_height,
                                   const int button_text, double& used_scale)
{
	const auto available = bounds.height();
	const auto lay_out = [&](const recti at, const double s, const int detail)
	{
		return layout_sid_monitor(at, s, songs, line_height, detail, button_text);
	};

	// Smaller first, down to three quarters: text keeps its size, so the geometry cannot shrink much
	// further before the labels crowd. Then fewer parts, the register grid first and the heading last.
	const auto smallest = scale * 0.75;
	auto detail = 0;
	used_scale = smallest;

	for (int d = 4; d >= 0; --d)
	{
		if (lay_out(bounds, scale, d).height <= available)
		{
			detail = d;
			used_scale = scale;
			break;
		}

		if (lay_out(bounds, smallest, d).height <= available)
		{
			// Searched for, because the text keeps its size: the height is not in proportion to the scale,
			// so a step in proportion to the shortfall still overflowed, and gave up a part - leaving a gap
			// its size - wherever the space was a little short of it.
			auto fits = smallest;
			auto overflows = scale;
			for (int i = 0; i < 10; ++i)
			{
				const auto mid = (fits + overflows) / 2.0;
				if (lay_out(bounds, mid, d).height <= available) fits = mid;
				else overflows = mid;
			}

			detail = d;
			used_scale = fits;
			break;
		}
	}

	// Centred in the height it is given, which is more than it takes whenever a part or a step of scale did
	// not fit. One still too tall starts at the top, and the clip cuts off its foot.
	const auto l = lay_out(bounds, used_scale, detail);
	const auto spare = available - l.height;
	return spare >= 2 ? lay_out(bounds.offset(0, spare / 2), used_scale, detail) : l;
}

std::array<float, sid_register_count> sid_register_activity(const sid_frame_state* frames, const size_t count)
{
	std::array<float, sid_register_count> result = {};
	if (count < 2) return result;

	// A register glows when the tune gave it a new value, and the glow fades over the frames after.
	for (auto f = 1_z; f < count; ++f)
	{
		const auto age = static_cast<float>(count - 1 - f);
		const auto strength = std::max(0.0f, 1.0f - age / static_cast<float>(activity_frames));

		for (int r = 0; r < sid_register_count; ++r)
		{
			if (frames[f].regs[r] != frames[f - 1].regs[r])
			{
				result[r] = std::max(result[r], strength);
			}
		}
	}

	return result;
}

sid_monitor_control::sid_monitor_control(view_state& state, display_state_ptr display, view_host_ptr host) noexcept :
	view_element(view_element_style::can_invoke | flex_item::media), _state(state), _display(std::move(display)),
	_host(std::move(host))
{
}

int sid_monitor_control::song_count() const
{
	// The render knows once the player has opened the file; until then the index's track property, which
	// the scan sets to the starting tune out of the tunes the file holds, is the same answer.
	if (_display->_sid)
	{
		const auto songs = _display->_sid->details().songs;
		if (songs > 1) return songs;
	}

	if (_display->_item1)
	{
		if (const auto md = _display->_item1->metadata())
		{
			if (md->track.y > 1) return md->track.y;
		}
	}

	return 1;
}

sizei sid_monitor_control::measure(ui::measure_context& mc, const int width_limit) const
{
	const auto songs = song_count();
	_measured_songs = songs;
	const auto button_text = std::max(
		mc.measure_text(tt.command_volume0, ui::style::font_face::code, ui::style::text_style::single_line, 10000).cx,
		mc.measure_text(tt.sid_solo, ui::style::font_face::code, ui::style::text_style::single_line, 10000).cx);
	const auto l = layout_sid_monitor(recti(0, 0, width_limit, 0), mc.scale_factor, songs,
	                                  mc.text_line_height(ui::style::font_face::code), 4, button_text);
	return {width_limit, l.height};
}

void sid_monitor_control::dispatch_event(const view_element_event& event)
{
	if (event.type == view_element_event_type::invoke)
	{
		_state.play(event.host);
	}
}

view_controller_ptr sid_monitor_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                  const pointi element_offset,
                                                                  hit_test_context& ctx)
{
	const auto& l = _drawn;
	const auto at = [&](const recti r) { return !r.is_empty() && r.offset(element_offset).contains(loc); };
	const auto click = [&](const recti r, std::function<void()> f)
	{
		return std::make_shared<sid_click_controller>(host, r.offset(element_offset), std::move(f));
	};

	for (auto i = 0_z; i < l.tune_chips.size(); ++i)
	{
		if (at(l.tune_chips[i]))
		{
			return click(l.tune_chips[i], [&state = _state, song = static_cast<int>(i) + 1]
			{
				state.select_sid_song(song);
			});
		}
	}

	if (_display->_sid && (at(l.previous_tune) || at(l.next_tune)))
	{
		const auto details = _display->_sid->details();
		const auto forward = at(l.next_tune);
		const auto song = std::clamp(details.song + (forward ? 1 : -1), 1, details.songs);
		return click(forward ? l.next_tune : l.previous_tune, [&state = _state, song]
		{
			state.select_sid_song(song);
		});
	}

	for (int v = 0; v < 3; ++v)
	{
		// Acted on at release, and the panel can be rebuilt while the button is held - the tune ends, the
		// folder refreshes - destroying this monitor. So the action holds nothing of the monitor: only the
		// state, and the display it shows, weakly.
		const auto toggle = [&state = _state, weak_display = std::weak_ptr(_display), v, host](const bool solo)
		{
			const auto display = weak_display.lock();
			if (!display) return;

			auto mix = display->_sid_mix;
			if (solo) mix.solo = mix.solo == v ? -1 : v;
			else mix.muted[v] = !mix.muted[v];
			state.mix_sid_voices(mix);
			host->frame()->invalidate();
		};

		if (at(l.mute[v])) return click(l.mute[v], [toggle] { toggle(false); });
		if (at(l.solo[v])) return click(l.solo[v], [toggle] { toggle(true); });
	}

	return view_element::controller_from_location(host, loc, element_offset, ctx);
}

void sid_monitor_control::render(ui::draw_context& dc, const pointi element_offset) const
{
	// Advances the session's clock, as the audio control does, so the monitor shows what is heard.
	_display->update_for_present(dc.time_now);

	const auto line_height = dc.text_line_height(ui::style::font_face::code);
	const auto button_text = std::max(
		dc.measure_text(tt.command_volume0, ui::style::font_face::code, ui::style::text_style::single_line, 10000).cx,
		dc.measure_text(tt.sid_solo, ui::style::font_face::code, ui::style::text_style::single_line, 10000).cx);
	const auto logical = bounds.offset(element_offset);
	auto scale = dc.scale_factor;
	const auto l = fit_sid_monitor(logical, dc.scale_factor, song_count(), line_height, button_text, scale);

	// A file whose tune count arrived after the layout was measured needs room for the tune selector.
	if (song_count() != _measured_songs)
	{
		_measured_songs = song_count();
		_state.invalidate_view(view_invalid::view_layout);
	}

	_drawn = l;
	for (auto& r : _drawn.tune_chips) r = r.offset(-element_offset.x, -element_offset.y);
	for (auto* r : {&_drawn.previous_tune, &_drawn.next_tune, &_drawn.tune_counter, &_drawn.tunes})
		*r = r->offset(-element_offset.x, -element_offset.y);
	for (int v = 0; v < 3; ++v)
	{
		_drawn.mute[v] = _drawn.mute[v].offset(-element_offset.x, -element_offset.y);
		_drawn.solo[v] = _drawn.solo[v].offset(-element_offset.x, -element_offset.y);
	}

	std::vector<queued_text> texts;
	const painter p{dc, dc.colors.alpha, scale, texts};
	// The last resort when even the heading and voices do not fit: what is left over is cut off rather
	// than drawn over the element below.
	const ui::scoped_clip clip(dc, logical);
	const auto lh = std::max(line_height, p.px(10));
	const auto pad = p.px(12);
	const auto& sid = _display->_sid;
	const auto details = sid ? sid->details() : sid_tune_details{};
	const auto& mix = _display->_sid_mix;
	const auto position = _display->_session ? _display->_session->pos(dc.time_now) : 0.0;

	sid_frame_state state;
	if (sid) sid->frame_at(position, state);

	// A part that moves with the music has its badge, track or panel drawn here and is left to the vertex
	// buffer, which draws over the backgrounds and under the text.
	sid_monitor_motion::parts parts;
	parts.thickness = p.px(2);
	parts.column = p.px(1);
	parts.inset = p.px(1);

	p.fill(l.panel, p.c(palette::panel), p.px(6));

	// The heading: the monitor's name, then what is playing.
	if (!l.heading.is_empty())
	{
		const auto& h = l.heading;
		p.text(tt.sid_monitor, recti(h.left, h.top, h.right, h.top + lh), palette::text_muted());
		const auto title = details.title.empty() && _display->_item1 ? std::string(_display->_item1->name())
		                                                             : details.title;
		p.text(title, recti(h.left, h.top + lh + p.px(4), h.right, h.top + lh * 2 + p.px(4)), palette::text());

		std::string byline = details.author;
		if (!details.released.empty()) byline += byline.empty() ? details.released : "  " + details.released;
		p.text(byline, recti(h.left, h.top + lh * 2 + p.px(8), h.right, h.bottom), palette::text_muted());
	}

	if (!l.tunes.is_empty())
	{
		p.text(tt.prop_name_track, l.tunes_label, palette::text_muted());

		for (auto i = 0_z; i < l.tune_chips.size(); ++i)
		{
			const auto current = static_cast<int>(i) + 1 == details.song;
			p.badge(std::to_string(i + 1), l.tune_chips[i], current ? p.c(palette::accent()) : p.c(palette::raised),
			        current ? palette::text() : palette::text_muted());
		}

		if (!l.tune_counter.is_empty())
		{
			p.badge("<", l.previous_tune, p.c(palette::raised), palette::text_muted());
			p.badge(">", l.next_tune, p.c(palette::raised), palette::text_muted());
			p.text(std::format("{} / {}", details.song, details.songs), l.tune_counter, palette::text(), 1.0f,
			       ui::style::text_style::single_line_center);
		}
	}

	// The voices.
	for (int v = 0; v < 3; ++v)
	{
		const auto& r = l.voices[v];
		auto& part = parts.voices[v];
		const voice_view voice(state, v);
		const auto a = mix.is_silent(v) ? silenced_alpha : 1.0f;
		const auto x0 = r.left + pad;
		const auto x1 = r.right - pad;
		auto y = r.top + pad;

		p.fill(r, p.c(palette::sunken, a), p.px(4));

		const auto wave = sid_waveform_name(voice.control);
		const auto wave_w = p.text_width(wave) + p.px(12);
		p.text(str_format(tt.sid_voice_fmt.sv(), v + 1), recti(x0, y, x1 - wave_w, y + lh), palette::text(), a);
		p.badge(wave, recti(x1 - wave_w, y - p.px(2), x1, y + lh + p.px(2)), p.c(palette::information(), a),
		        palette::text(), a);
		y += lh + p.px(6);
		p.dc.draw_rect(recti(x0, y, x1, y + 1), p.c(palette::line, a));
		y += p.px(3);

		part.shape = recti(x0, y, x1, y + p.px(36));
		y = part.shape.bottom + p.px(8);

		const auto label_w = p.px(40);
		const auto value_w = p.px(50);
		const auto meter_row = [&](const std::string_view label, const int value)
		{
			p.text(label, recti(x0, y, x0 + label_w, y + lh), palette::text_muted(), a);
			const auto bar_h = p.px(12);
			const auto bar_top = y + (lh - bar_h) / 2;
			const recti track(x0 + label_w, bar_top, x1 - value_w, bar_top + bar_h);
			p.fill(track, p.c(palette::raised, a), p.px(2));
			p.text(std::to_string(value), recti(x1 - value_w, y, x1, y + lh), palette::text(), a,
			       ui::style::text_style::single_line_far);
			y += lh + p.px(6);
			return track;
		};

		part.frequency = meter_row("FREQ", voice.frequency);
		part.envelope = meter_row("ENV", voice.envelope);

		// A:1 D:6 S:2 R:10, the letters muted.
		if (!l.compact)
		{
			auto x = x0;
			const std::pair<std::string_view, int> adsr[] = {
				{"A:", voice.attack}, {"D:", voice.decay}, {"S:", voice.sustain}, {"R:", voice.release}
			};

			for (const auto& [name, value] : adsr)
			{
				const auto number = std::to_string(value);
				const auto name_w = p.text_width(name);
				const auto number_w = p.text_width(number);
				p.text(name, recti(x, y, x + name_w, y + lh), palette::text_muted(), a);
				p.text(number, recti(x + name_w, y, x + name_w + number_w, y + lh), palette::text(), a);
				x += name_w + number_w + p.px(12);
			}

			y += lh + p.px(6);
		}

		// The gate, the note it plays, and the listening controls - beside them, or on the row below.
		{
			const auto& mute = l.mute[v];
			const auto row_top = l.buttons_wrap ? y : mute.top;
			const auto row_bottom = row_top + mute.height();
			const auto note_right = l.buttons_wrap ? x1 : mute.left;
			const auto gate_w = p.text_width("GATE") + p.px(10);

			part.gate = recti(x0, row_top, x0 + gate_w, row_bottom);
			p.badge("GATE", part.gate, p.c(palette::raised, a), palette::text(), a);

			const auto note = sid_note_name(voice.frequency, details.clock_hz);
			p.text(note.empty() ? std::string("-") : note,
			       recti(part.gate.right + p.px(8), row_top, note_right, row_bottom), palette::text(), a);

			p.badge(tt.command_volume0, mute, mix.muted[v] ? p.c(palette::red()) : p.c(palette::raised),
			        mix.muted[v] ? palette::text() : palette::text_muted());
			p.badge(tt.sid_solo, l.solo[v], mix.solo == v ? p.c(palette::orange()) : p.c(palette::raised),
			        mix.solo == v ? palette::text() : palette::text_muted());
		}
	}

	// What the chip puts out.
	if (!l.output.is_empty())
	{
		const auto& r = l.output;
		p.fill(r, p.c(palette::sunken), p.px(4));
		p.text(tt.sid_output, recti(r.left, r.top + pad, r.right, r.top + pad + lh), palette::text_muted(), 1.0f,
		       ui::style::text_style::single_line_center);

		parts.scope = recti(r.left + pad, r.top + pad + lh + p.px(8), r.right - pad, r.bottom - pad);
		const auto mid = (parts.scope.top + parts.scope.bottom) / 2;
		p.dc.draw_rect(recti(parts.scope.left, mid, parts.scope.right, mid + 1), p.c(palette::line));
	}

	// The filter and the volume.
	if (!l.filter.is_empty())
	{
		const auto& r = l.filter;
		const auto& regs = state.regs;
		const auto cutoff = ((regs[0x16] << 3) | (regs[0x15] & 0x07)) & 0x7FF;
		const auto resonance = regs[0x17] >> 4;

		p.fill(r, p.c(palette::sunken), p.px(4));
		auto y = r.top + pad;
		p.text(tt.sid_filter, recti(r.left, y, r.right, y + lh), palette::text_muted(), 1.0f,
		       ui::style::text_style::single_line_center);
		y += lh + p.px(8);

		// CUTOFF and RES first, then the filter's modes and the voices it filters, wrapping below the meters
		// when the panel is too narrow for one row.
		const auto x0 = r.left + pad;
		const auto x1 = r.right - pad;
		const auto column_gap = p.px(24);
		const auto badge_gap = p.px(6);
		const auto badge_w = std::max(p.px(30), p.text_width("V3") + p.px(14));
		const auto badges_w = badge_w * 6 + badge_gap * 4 + column_gap;
		const auto bars_w = l.filter_wraps
			                    ? (x1 - x0 - column_gap) / 2
			                    : std::max(p.px(60), (x1 - x0 - badges_w - column_gap * 2) / 2);

		const auto parameter = [&](const int left, const std::string_view label, const int value)
		{
			const auto right = left + bars_w;
			p.text(label, recti(left, y, right, y + lh), palette::text_muted());
			const recti track(left, y + lh + p.px(4), right, y + lh + p.px(14));
			p.fill(track, p.c(palette::raised), p.px(2));
			p.text(std::to_string(value), recti(left, y + lh + p.px(18), right, y + lh * 2 + p.px(18)),
			       palette::text());
			return track;
		};

		parts.cutoff = parameter(x0, "CUTOFF", cutoff);
		parts.resonance = parameter(x0 + bars_w + column_gap, "RES", resonance);

		auto x = l.filter_wraps ? x0 : x0 + (bars_w + column_gap) * 2;
		const auto badge_top = l.filter_wraps ? y + lh * 2 + p.px(18) + p.px(10) : y + p.px(8);
		constexpr std::string_view lamp_names[] = {"LP", "BP", "HP", "V1", "V2", "V3"};

		for (auto i = 0_z; i < std::size(lamp_names); ++i)
		{
			// The modes, then the voices routed through the filter.
			if (i == 3) x += column_gap - badge_gap;
			parts.filter_lamps[i] = recti(x, badge_top, x + badge_w, badge_top + lh + p.px(8));
			p.badge(lamp_names[i], parts.filter_lamps[i], p.c(palette::raised), palette::text());
			x += badge_w + badge_gap;
		}

		y += lh * 2 + p.px(18) + p.px(6);
		if (l.filter_wraps) y += lh + p.px(8) + p.px(10);
		p.dc.draw_rect(recti(x0, y, x1, y + 1), p.c(palette::line));
		y += p.px(6);

		// Fifteen steps, lit green, then orange, then red.
		const auto label_w = std::max(p.px(80), p.text_width(tt.sid_volume) + p.px(12));
		p.text(tt.sid_volume, recti(x0, y, x0 + label_w, y + p.px(24)), palette::text_muted());
		const auto bars_left = x0 + label_w;
		const auto step_gap = p.px(3);
		const auto step_w = std::max(1, (x1 - bars_left - step_gap * 14) / 15);

		for (auto i = 0_z; i < parts.volume.size(); ++i)
		{
			const auto left = bars_left + static_cast<int>(i) * (step_w + step_gap);
			parts.volume[i] = recti(left, y + p.px(2), left + step_w, y + p.px(22));
			p.fill(parts.volume[i], p.c(palette::raised), p.px(2));
		}
	}

	// The registers the tune writes, each highlighted briefly when it changes.
	if (!l.registers.is_empty())
	{
		const auto& r = l.registers;
		p.fill(r, p.c(palette::sunken), p.px(4));
		auto y = r.top + pad;
		p.text(tt.sid_register_writes, recti(r.left, y, r.right, y + lh), palette::text_muted(), 1.0f,
		       ui::style::text_style::single_line_center);
		y += lh + p.px(8);

		const auto x0 = r.left + pad;
		const auto x1 = r.right - pad;
		const auto cell_gap = p.px(4);
		const auto cell_w = (x1 - x0 - cell_gap * (register_columns - 1)) / register_columns;
		const auto cell_h = lh * 2 + p.px(8);

		for (int i = 0; i < sid_register_count; ++i)
		{
			const auto left = x0 + (i % register_columns) * (cell_w + cell_gap);
			const auto top = y + (i / register_columns) * (cell_h + cell_gap);
			const recti cell(left, top, left + cell_w, top + cell_h);
			parts.registers[i] = cell;

			p.fill(cell, p.c(palette::raised), p.px(2));
			p.text(register_names[i], recti(cell.left, cell.top + p.px(4), cell.right, cell.top + p.px(4) + lh),
			       palette::text_muted(), 1.0f, ui::style::text_style::single_line_center);
			p.text(hex2(state.regs[i]), recti(cell.left, cell.top + p.px(4) + lh, cell.right, cell.bottom - p.px(4)),
			       palette::text(), 1.0f, ui::style::text_style::single_line_center);
		}
	}

	// The moving parts, over the backgrounds and under the text.
	if (!_display->_sid_motion) _display->_sid_motion = std::make_shared<sid_monitor_motion>();
	const auto& motion = _display->_sid_motion;
	if (!motion->_verts) motion->_verts = dc.create_vertices();
	motion->place(parts, sid, mix, dc.colors.alpha, position, state, dc.time_now);
	if (motion->_verts) dc.draw_vertices(motion->_verts);

	for (const auto& t : texts)
	{
		dc.draw_text(t.text, t.bounds, ui::style::font_face::code, t.style, t.colour, {});
	}
}

void sid_monitor_motion::place(const parts& p, std::shared_ptr<sid_tune_render> sid, const sid_voice_mix& mix,
                               const float alpha, const double position, const sid_frame_state& text_state,
                               const double time_now)
{
	_parts = p;
	_sid = std::move(sid);
	_mix = mix;
	_alpha = alpha;
	_text_state = text_state;
	_text_time = time_now;

	const auto& rects = build(position);
	if (_verts) _verts->update_rects(rects);
}

render_valid sid_monitor_motion::update(const std::shared_ptr<sid_tune_render>& sid, const double position,
                                        const double time_now)
{
	if (!_verts || time_now - _text_time > hidden_after_seconds) return render_valid::valid;

	// Another of the file's tunes, or the file opened again: where the parts go is the monitor's to say.
	if (sid != _sid) return render_valid::invalid;
	if (!_sid) return render_valid::valid;

	auto result = render_valid::valid;

	if (position != _built_position)
	{
		_verts->update_rects(build(position));
		result = render_valid::present;
	}

	if (time_now - _text_time >= text_refresh_seconds)
	{
		sid_frame_state now;

		if (_sid->frame_at(position, now) && (now.regs != _text_state.regs || now.envelope != _text_state.envelope))
		{
			result = render_valid::invalid;
		}
	}

	return result;
}

const std::vector<ui::vertex_rect>& sid_monitor_motion::build(const double position)
{
	_built_position = position;
	_rects.clear();
	if (!_sid) return _rects;

	sid_frame_state frames[activity_frames + 1];
	const auto frame_count = _sid->frames_before(position, frames, std::size(frames));
	const auto state = frame_count ? frames[frame_count - 1] : sid_frame_state{};
	const auto colour = [this](const ui::color32 rgb, const float a = 1.0f) { return ui::color(rgb, _alpha * a); };

	for (int v = 0; v < 3; ++v)
	{
		const auto& part = _parts.voices[v];
		const voice_view voice(state, v);
		const auto a = _mix.is_silent(v) ? silenced_alpha : 1.0f;

		if (!part.shape.is_empty())
		{
			const auto columns = std::max(1, part.shape.width() / std::max(1, _parts.column));
			add_trace(_rects, part.shape, voice_shape(voice, part.shape, columns), _parts.thickness,
			          colour(voice.gate() ? palette::accent_light() : palette::text_faint(), a));
		}

		add_meter(_rects, part.frequency, voice.frequency / 65535.0, _parts.inset, colour(palette::accent(), a));
		add_meter(_rects, part.envelope, voice.envelope / 255.0, _parts.inset, colour(palette::green(), a));
		if (voice.gate() && !part.gate.is_empty())
			add_rect(_rects, part.gate.inflate(-_parts.inset), colour(palette::green(), a));
	}

	if (!_parts.scope.is_empty())
	{
		int16_t samples[scope_window];
		const auto count = _sid->samples_before(position, samples, scope_window);

		if (count > 0)
		{
			const auto columns = std::max(1, _parts.scope.width() / std::max(1, _parts.column));
			add_trace(_rects, _parts.scope, scope_trace(samples, count, _parts.scope, columns), _parts.thickness,
			          colour(palette::accent_light()));
		}
	}

	const auto& regs = state.regs;
	const auto cutoff = ((regs[0x16] << 3) | (regs[0x15] & 0x07)) & 0x7FF;
	const auto resonance = regs[0x17] >> 4;
	const auto routing = regs[0x17];
	const auto mode = regs[0x18];
	const auto volume = mode & 0x0F;

	add_meter(_rects, _parts.cutoff, cutoff / 2047.0, _parts.inset, colour(palette::accent()));
	add_meter(_rects, _parts.resonance, resonance / 15.0, _parts.inset, colour(palette::accent()));

	const bool lit[] = {
		(mode & 0x10) != 0, (mode & 0x20) != 0, (mode & 0x40) != 0,
		(routing & 0x01) != 0, (routing & 0x02) != 0, (routing & 0x04) != 0
	};

	for (auto i = 0_z; i < std::size(lit); ++i)
	{
		if (lit[i] && !_parts.filter_lamps[i].is_empty())
			add_rect(_rects, _parts.filter_lamps[i].inflate(-_parts.inset), colour(palette::accent()));
	}

	for (auto i = 0_z; i < _parts.volume.size() && static_cast<int>(i) < volume; ++i)
	{
		if (_parts.volume[i].is_empty()) continue;
		const auto step = i < 5 ? palette::green() : i < 10 ? palette::orange() : palette::red();
		add_rect(_rects, _parts.volume[i].inflate(-_parts.inset), colour(step));
	}

	const auto activity = sid_register_activity(frames, frame_count);

	for (auto i = 0_z; i < _parts.registers.size(); ++i)
	{
		if (activity[i] > 0.0f && !_parts.registers[i].is_empty())
			add_rect(_rects, _parts.registers[i].inflate(-_parts.inset), colour(palette::accent(), activity[i]));
	}

	return _rects;
}
