// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The tracker monitor as panels of the media stack: the module's position and the pattern under the
// playhead in a grid, centred in its panel and scrolling smoothly through a bar with the patterns either side of
// it, whose channel header and row numbers stay in view, each channel's level, mute and solo, its empty channels
// left out; and the sample and instrument banks in a common table below the selection panel, with how the
// patterns play them.

#include "pch.h"
#include "ui_tracker.h"
#include "ui_music.h"
#include "ui_view.h"
#include "ui_controls.h"
#include "app_text.h"
#include "model.h"
#include "libdesktop/ui/table_element.h"

namespace
{
	namespace palette = monitor_palette;

	// The rows the pattern shows when the panel has the room it asks for, centred on the playing row; and the
	// fewest it keeps when the stack is short of space.
	constexpr int natural_rows = 21;
	constexpr int least_rows = 5;

	// A silenced channel is drawn faded, as the module goes on playing it.
	constexpr float silenced_alpha = 0.45f;

	// What a cell is sized by until the module's patterns are known: note, instrument, volume and effect.
	constexpr std::string_view template_cell = "C-5 01 v64 C20";

	std::vector<std::string_view> cell_fields(const std::string_view cell)
	{
		std::vector<std::string_view> fields;
		size_t start = 0;

		while (start < cell.size())
		{
			const auto space = cell.find(' ', start);
			const auto stop = space == std::string_view::npos ? cell.size() : space;
			if (stop > start) fields.emplace_back(cell.substr(start, stop - start));
			start = stop + 1;
		}

		return fields;
	}

	// The decoder writes a field with nothing in it as dots.
	bool is_empty_field(const std::string_view field)
	{
		return std::ranges::all_of(field, [](const char c) { return c == '.'; });
	}

	// A press that runs one action on release, so a press dragged away is abandoned, and says what it does. A release
	// with no press of its own - the one after a press that closed a menu - does nothing.
	class tracker_click_controller final : public view_controller
	{
	public:
		tracker_click_controller(view_host_ptr host, const recti bounds, std::string tip,
		                         std::function<void()> invoke) :
			view_controller(std::move(host), bounds), _tip(std::move(tip)), _invoke(std::move(invoke))
		{
		}

		platform::cursor cursor() const override { return platform::cursor::link; }

		void on_mouse_left_button_down(const pointi /*loc*/, const platform::key_state /*keys*/) override
		{
			_pressed = true;
		}

		void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
		{
			if (std::exchange(_pressed, false) && _bounds.contains(loc)) _invoke();
		}

		void popup_from_location(view_hover_element& hover) override
		{
			if (_tip.empty()) return;
			hover.elements->add(std::make_shared<text_element>(_tip));
			hover.active_bounds = hover.window_bounds = _bounds;
		}

	private:
		std::string _tip;
		std::function<void()> _invoke;
		bool _pressed = false;
	};
}

std::string tracker_cell_text(const std::string_view cell, const tracker_cell_detail detail)
{
	const auto fields = cell_fields(cell);
	if (fields.empty()) return std::string(cell);

	std::vector<std::string_view> kept;

	switch (detail)
	{
	case tracker_cell_detail::note: kept.assign(fields.begin(), fields.begin() + 1); break;
	case tracker_cell_detail::instrument:
		kept.assign(fields.begin(), fields.begin() + std::min<size_t>(2, fields.size()));
		break;
	case tracker_cell_detail::effect:
		kept.assign(fields.begin(), fields.begin() + std::min<size_t>(2, fields.size()));
		if (fields.size() > 2) kept.emplace_back(fields.back());
		break;
	case tracker_cell_detail::full: kept = fields; break;
	}

	std::string result;

	for (const auto field : kept)
	{
		if (!result.empty()) result += ' ';
		result += field;
	}

	return result;
}

std::string tracker_note_name(const int note)
{
	static constexpr std::array<std::string_view, 12> names = {"C-", "C#", "D-", "D#", "E-", "F-",
	                                                           "F#", "G-", "G#", "A-", "A#", "B-"};

	if (note < 1 || note > tracker_highest_note) return {};
	return std::format("{}{}", names[static_cast<size_t>(note - 1) % names.size()], (note - 1) / 12);
}

std::string tracker_note_range(const tracker_bank_use& use)
{
	if (use.notes == 0) return {};
	if (use.lowest == use.highest) return tracker_note_name(use.lowest);
	return std::format("{}\xE2\x80\x93{}", tracker_note_name(use.lowest), tracker_note_name(use.highest));
}

tracker_grid_fit fit_tracker_grid(const std::string_view cell, const int channels, const double width,
                                  const double char_width)
{
	tracker_grid_fit fit;
	if (channels <= 0 || char_width <= 0.0) return fit;

	for (const auto detail : {tracker_cell_detail::full, tracker_cell_detail::effect, tracker_cell_detail::instrument,
	                          tracker_cell_detail::note})
	{
		fit.detail = detail;
		fit.cell_chars = static_cast<int>(tracker_cell_text(cell, detail).size());

		// A channel is its cell and the character that parts it from the next.
		const auto shown = static_cast<int>((width + char_width) / ((fit.cell_chars + 1) * char_width));
		fit.visible_channels = std::clamp(shown, 1, channels);
		if (shown >= channels) break;
	}

	return fit;
}

class tracker_monitor_impl : public std::enable_shared_from_this<tracker_monitor_impl>
{
public:
	// Null, with the host, for a monitor drawn apart from any view.
	view_state* state = nullptr;
	display_state_ptr display;
	view_host_ptr host;
	std::shared_ptr<const tracker_tune_details> details;
	// The pattern last drawn.
	std::shared_ptr<const tracker_pattern> pattern;

	// As the last layout placed them, in the element's coordinates.
	recti bounds;
	recti status;
	recti follow;
	recti previous_channels;
	recti next_channels;
	recti header;
	recti grid;
	// How far the row highlights reach past the table either side, when it is centred with room to spare.
	int band_pad = 0;
	int numbers_width = 0;
	int line_height = 0;
	int row_height = 0;
	double char_width = 8.0;
	tracker_grid_fit fit;
	int rows_visible = 0;
	std::vector<recti> headers;
	std::vector<recti> mutes;
	std::vector<recti> solos;

	tracker_monitor_impl(view_state* s, display_state_ptr d, view_host_ptr h) :
		state(s), display(std::move(d)), host(std::move(h))
	{
		details = display->_tracker ? display->_tracker->details() : nullptr;
		choose_channels();
	}

	// A monitor in a view has Follow playback, the arrows that step through channels too many to show, and each
	// channel's mute and solo; one drawn apart from any view, as a movie's frame is, has none of them.
	bool has_controls() const { return state != nullptr; }

	// What sizes the grid: one of the module's cells, which are all one width, or a typical one until it is read.
	std::string_view cell() const
	{
		return details && !details->cell.empty() ? std::string_view(details->cell) : template_cell;
	}

	// The channels the grid shows, by the module's numbering: every one with anything in it - all of them where none
	// has, or until the module has been read.
	std::vector<int> channels;

	void choose_channels()
	{
		const auto count = details && !details->channels.empty() ? static_cast<int>(details->channels.size())
		                   : pattern                             ? pattern->channels
		                                                         : 0;
		const auto is_empty = [this](const int c)
		{
			return details && static_cast<size_t>(c) < details->empty_channels.size() && details->empty_channels[c];
		};

		channels.clear();
		for (auto c = 0; c < count; ++c)
			if (!is_empty(c)) channels.push_back(c);
		if (channels.empty())
			for (auto c = 0; c < count; ++c) channels.push_back(c);
	}

	int channel_count() const { return static_cast<int>(channels.size()); }

	// The module's channel shown at a place in the grid's order.
	int channel_at(const int shown) const { return shown >= 0 && shown < channel_count() ? channels[shown] : shown; }

	int pattern_rows() const { return pattern ? pattern->rows : 64; }

	int first_channel() const
	{
		return std::clamp(display->_tracker_first_channel, 0, std::max(0, channel_count() - fit.visible_channels));
	}

	// Everything above the rows, in a width: the status line, the gap under it and the channel header.
	struct frame_heights
	{
		int pad = 0;
		int status = 0;
		int header = 0;
		int gap = 0;
		bool stacked_toggles = false;
	};

	frame_heights measure_frame(render::measure_context& mc, const int width)
	{
		frame_heights f;
		f.pad = core::round(ui::panel_element::content_padding * mc.scale_factor);
		f.gap = std::max(2, core::round(4 * mc.scale_factor));
		line_height = mc.text_line_height(render::font_face::code);
		row_height = line_height + std::max(1, core::round(2 * mc.scale_factor));
		char_width =
			mc.measure_text(std::string(64, '0'), render::font_face::code, render::text_style::single_line, 100000).cx /
			64.0;
		f.status = line_height + core::round(6 * mc.scale_factor);

		const auto digits = pattern_rows() > 256 ? 3 : 2;
		numbers_width = core::round(char_width * (digits + 1));
		const auto channels_width = std::max(0, width - f.pad * 2 - numbers_width);
		fit = fit_tracker_grid(cell(), channel_count(), channels_width, char_width);

		// Mute and solo share the line under each channel's level when they fit side by side, and take a line
		// each when its cells are too narrow for that. Without them the header is the level alone.
		if (!has_controls())
		{
			f.header = line_height;
			return f;
		}

		const auto toggle = toggle_size(mc);
		f.stacked_toggles = fit.cell_chars * char_width < toggle * 2 + f.gap;
		f.header = line_height + f.gap + toggle + (f.stacked_toggles ? f.gap + toggle : 0);
		return f;
	}

	int toggle_size(render::measure_context& mc) const
	{
		return std::min(mc.icon_cxy, line_height + core::round(4 * mc.scale_factor));
	}

	int natural_height(render::measure_context& mc, const int width)
	{
		const auto f = measure_frame(mc, width);
		return f.pad + f.status + f.gap * 2 + f.header + f.gap + natural_rows * row_height + f.pad;
	}

	void arrange(render::measure_context& mc, const recti r)
	{
		bounds = r;
		const auto f = measure_frame(mc, r.width());
		const auto inner = r.inflate(-f.pad);

		status = recti(inner.left, inner.top, inner.right, inner.top + f.status);
		follow = previous_channels = next_channels = {};

		if (has_controls())
		{
			const auto follow_width = mc.measure_text(tt.tracker_follow, render::font_face::dialog,
			                                          render::text_style::single_line, 10000)
			                              .cx +
			                          mc.padding2 * 2;
			follow = recti(status.right - follow_width, status.top, status.right, status.bottom);

			// The arrows step through channels that do not fit, and are there only when some do not.
			if (fit.visible_channels < channel_count())
			{
				const auto arrow = status.height() + mc.padding2;
				next_channels = recti(follow.left - f.gap * 2 - arrow, status.top, follow.left - f.gap * 2,
				                      status.bottom);
				previous_channels = next_channels.offset(-(arrow + f.gap), 0);
			}
		}

		header = recti(inner.left, status.bottom + f.gap * 2, inner.right, status.bottom + f.gap * 2 + f.header);
		const auto grid_top = header.bottom + f.gap;
		rows_visible = std::max(least_rows, (inner.bottom - grid_top) / std::max(1, row_height));
		grid = recti(inner.left, grid_top, inner.right, grid_top + rows_visible * row_height);

		// The table - the row numbers and the channels shown - stands in the middle of the panel when it needs
		// less than the panel's width; the status line above it keeps the whole width.
		const auto column = (fit.cell_chars + 1) * char_width;
		const auto table_width = std::min(inner.width(),
		                                  numbers_width + core::round(fit.visible_channels * column - char_width));
		const auto table_left = inner.left + std::max(0, (inner.width() - table_width) / 2);
		header.left = grid.left = table_left;
		header.right = grid.right = table_left + table_width;
		band_pad = std::min(core::round(char_width / 2), table_left - inner.left);

		headers.clear();
		mutes.clear();
		solos.clear();

		const auto toggle = toggle_size(mc);
		const auto left = table_left + numbers_width;

		for (auto c = 0; c < fit.visible_channels; ++c)
		{
			const auto x0 = left + core::round(c * column);
			const auto x1 = left + core::round(c * column + fit.cell_chars * char_width);
			headers.emplace_back(x0, header.top, x1, header.bottom);
			if (!has_controls()) continue;

			const auto top = header.top + line_height + f.gap;

			if (f.stacked_toggles)
			{
				mutes.emplace_back(x0, top, x1, top + toggle);
				solos.emplace_back(x0, top + toggle + f.gap, x1, top + toggle * 2 + f.gap);
			}
			else
			{
				const auto middle = (x0 + x1) / 2;
				mutes.emplace_back(x0, top, middle - f.gap / 2, top + toggle);
				solos.emplace_back(middle + (f.gap + 1) / 2, top, x1, top + toggle);
			}
		}
	}

	// The row the grid starts at: the playing row in the middle while it follows playback, and where the user
	// left it when not - but never so far that the pattern is scrolled out of sight.
	int top_row(const int playing) const
	{
		const auto half = rows_visible / 2;
		if (display->_tracker_follow && playing >= 0) return playing - half;
		return std::clamp(display->_tracker_top_row, -half, std::max(-half, pattern_rows() - 1 - half));
	}

	// The module's details arrive once the player has read it: the bank below the panel is built from them and the
	// grid is sized by one of their cells, and shows the channels they say have anything in them, so the stack is
	// built, and laid out, again for them.
	void observe(const tracker_frame_state& frame, const bool has_frame)
	{
		const auto current = display->_tracker ? display->_tracker->details() : nullptr;
		if (current && !details && state)
			state->invalidate_view(view_invalid::media_elements | view_invalid::view_layout);
		const auto pattern_channels = pattern ? pattern->channels : 0;
		const auto changed = current != details;
		details = current;
		if (has_frame && frame.pattern) pattern = frame.pattern;
		if (changed || (pattern ? pattern->channels : 0) != pattern_channels) choose_channels();
	}

	void toggle_mix(const int channel, const bool solo) const
	{
		auto mix = display->_tracker_mix;

		if (solo)
		{
			mix.solo = mix.solo == channel ? -1 : channel;
		}
		else
		{
			if (mix.muted.size() <= static_cast<size_t>(channel)) mix.muted.resize(channel + 1);
			mix.muted[channel] = !mix.muted[channel];
		}

		if (state) state->mix_tracker_channels(mix);
	}
};

tracker_monitor_control::tracker_monitor_control(view_state& state, display_state_ptr display, view_host_ptr host) :
	view_element(flex_item::media),
	_impl(std::make_shared<tracker_monitor_impl>(&state, std::move(display), std::move(host)))
{
	// Its panel meets the column's edges, as the selection panel below it does.
	padding(0);
	margin(0);
}

tracker_monitor_control::tracker_monitor_control(display_state_ptr display) :
	view_element(flex_item::media), _impl(std::make_shared<tracker_monitor_impl>(nullptr, std::move(display), nullptr))
{
	padding(0);
	margin(0);
}

tracker_monitor_control::~tracker_monitor_control() = default;

sizei tracker_monitor_control::measure(render::measure_context& mc, const int width_limit) const
{
	return {width_limit, _impl->natural_height(mc, width_limit)};
}

void tracker_monitor_control::layout(render::measure_context& mc, const recti r)
{
	bounds = r;
	_impl->arrange(mc, r);
}

void tracker_monitor_control::render(render::draw_context& dc, const pointi offset) const
{
	auto& impl = *_impl;
	const auto& d = impl.display;

	// Advances the session's clock, as the audio control does, so the monitor shows what is heard.
	d->update_for_present(dc.time_now);

	const auto position = d->playback_position(dc.time_now);
	const auto revision = d->_tracker ? d->_tracker->revision() : 0;
	tracker_frame_state frame;
	const auto has_frame = d->_tracker && d->_tracker->frame_at(position, frame);
	impl.observe(frame, has_frame);

	d->_tracker_drawn_time = dc.time_now;
	d->_tracker_drawn_position = position;
	d->_tracker_drawn_revision = revision;

	const auto alpha = dc.colors.alpha;
	const auto c = [alpha](const core::color32 rgb, const float a = 1.0f)
	{
		return core::color(rgb, alpha * a);
	};
	const auto shaded = [alpha](const palette::shade s, const float a = 1.0f)
	{
		return core::color(s.rgb, alpha * a * s.alpha);
	};
	const auto panel = bounds.offset(offset);
	draw_stack_panel(dc, panel);

	const render::scoped_clip clip(dc, panel);
	const auto radius = std::max(1, core::round(3 * dc.scale_factor));
	const auto& mix = d->_tracker_mix;
	const auto channels = impl.channel_count();
	const auto first = impl.first_channel();

	// Where the module is: its order and pattern, the row, and how fast it goes - as much of that as fits,
	// giving up the speed first, then the order, then the tempo, and keeping the pattern and the row.
	{
		const auto text_right = !impl.previous_channels.is_empty() ? impl.previous_channels.left
		                        : !impl.follow.is_empty()          ? impl.follow.left
		                                                           : impl.status.right;
		const auto room = text_right - dc.padding2 - impl.status.left;
		std::string text;

		if (impl.details && has_frame)
		{
			const auto orders = std::max(1, static_cast<int>(impl.details->orders.size()));
			const std::array<std::string, 5> parts = {
				std::format("{:02X}/{:02X}", frame.order, orders - 1),
				std::format("{} {:02X}", tt.tracker_pattern.sv(), frame.pattern ? frame.pattern->index : 0),
				std::format("{} {:02X}", tt.tracker_row.sv(), frame.row),
				std::format("{:.0f} BPM", frame.tempo),
				std::format("{} {}", tt.tracker_speed.sv(), frame.speed),
			};
			std::array<bool, 5> shown = {true, true, true, true, true};

			const auto join = [&]
			{
				std::string joined;

				for (auto i = 0_z; i < parts.size(); ++i)
				{
					if (!shown[i]) continue;
					if (!joined.empty()) joined += "  \xC2\xB7  ";
					joined += parts[i];
				}

				return joined;
			};

			text = join();

			for (const auto drop : {4, 0, 3})
			{
				if (dc.measure_text(text, render::font_face::code, render::text_style::single_line, 100000).cx <= room)
					break;
				shown[drop] = false;
				text = join();
			}
		}
		else
		{
			text = std::string(tt.loading.sv());
		}

		dc.draw_text(text,
		             recti(impl.status.left, impl.status.top, text_right - dc.padding2, impl.status.bottom)
		                 .offset(offset),
		             render::font_face::code, render::text_style::single_line, c(palette::text()), {});

		const auto badge = [&](const std::string_view label, const recti r, const bool lit, const auto font)
		{
			dc.draw_rounded_rect(r.offset(offset), lit ? c(palette::accent()) : shaded(palette::raised), radius);
			dc.draw_text(label, r.offset(offset), font, render::text_style::single_line_center,
			             c(lit ? palette::text() : palette::text_muted()), {});
		};

		if (!impl.previous_channels.is_empty())
		{
			badge("<", impl.previous_channels, false, render::font_face::code);
			badge(">", impl.next_channels, false, render::font_face::code);
		}

		if (!impl.follow.is_empty())
			badge(tt.tracker_follow, impl.follow, d->_tracker_follow, render::font_face::dialog);
	}

	const auto silent_alpha = [&mix](const int channel)
	{
		return mix.is_silent(channel) ? silenced_alpha : 1.0f;
	};
	const auto column = (impl.fit.cell_chars + 1) * impl.char_width;
	const auto grid_left = impl.grid.left + impl.numbers_width;

	// The parting lines between channels, through the header and the rows.
	for (auto i = 1; i < static_cast<int>(impl.headers.size()); ++i)
	{
		const auto x = grid_left + core::round(i * column - impl.char_width / 2);
		dc.draw_rect(recti(x, impl.header.top, x + 1, impl.grid.bottom).offset(offset), shaded(palette::line));
	}

	// Each channel's header: its number over its level, then mute and solo where there are controls. It stays where
	// it is while the rows scroll under it.
	for (auto i = 0; i < static_cast<int>(impl.headers.size()); ++i)
	{
		const auto channel = impl.channel_at(first + i);
		const auto a = silent_alpha(channel);
		const auto h = impl.headers[i];
		const recti level_bounds(h.left, h.top, h.right, h.top + impl.line_height);
		const auto level = channel < static_cast<int>(frame.levels.size())
		                       ? std::clamp(frame.levels[channel], 0.0f, 1.0f)
		                       : 0.0f;

		dc.draw_rounded_rect(level_bounds.offset(offset), shaded(palette::raised, a * 0.6f), radius);

		if (level > 0.0f)
		{
			auto lit = level_bounds;
			lit.right = lit.left + core::round(lit.width() * level);
			dc.draw_rounded_rect(lit.offset(offset), c(palette::green(), a * 0.75f), radius);
		}

		dc.draw_text(std::to_string(channel + 1), level_bounds.offset(offset), render::font_face::code,
		             render::text_style::single_line_center, c(palette::text(), a), {});

		if (i >= static_cast<int>(impl.mutes.size())) continue;

		const auto toggle = [&](const recti r, const icon_index icon, const bool on, const core::color32 colour)
		{
			dc.draw_rounded_rect(r.offset(offset), on ? c(colour) : shaded(palette::raised), radius);
			xdraw_icon(dc, icon, r.offset(offset), c(on ? palette::text() : palette::text_muted()), {});
		};

		const auto muted = static_cast<size_t>(channel) < mix.muted.size() && mix.muted[channel];
		toggle(impl.mutes[i], icon_index::mute, muted, palette::red());
		toggle(impl.solos[i], icon_index::headphones, mix.solo == channel, palette::orange());
	}

	// The rows, every fourth marked as a beat, each one line of text whose fields are coloured as a tracker colours
	// them: the note, the instrument, the volume and the effect. Following playback, they scroll up through a bar
	// across the middle of the grid, the playing row more in it than any other for as long as it plays - half in as
	// it starts, centred halfway through, half out as the next comes in - with the patterns played before and after
	// this one fainter above and below it. Scrolled by hand, they stand still and the playing row is lit wherever it is.
	const auto pattern = impl.pattern;
	const auto rows = pattern ? pattern->rows : 0;
	const auto playing = has_frame ? frame.row : -1;
	const auto top = impl.top_row(playing);
	if (d->_tracker_follow) d->_tracker_top_row = top;

	// Paused, the playing row rests on the bar rather than wherever in its time the pause found it.
	const auto following = d->_tracker_follow && playing >= 0;
	const auto glide = following && (!impl.state || d->is_playing_media()) ? frame.row_progress - 0.5 : 0.0;
	const auto glide_px = core::round(glide * impl.row_height);

	struct shown_row
	{
		const tracker_pattern* source = nullptr;
		int row = 0; // of the source
		bool lit = false;
		bool faint = false;
		recti bounds;
	};

	std::vector<shown_row> shown;

	// One more than fit either side, for the rows coming in and going out as they scroll.
	for (auto i = -1; i <= impl.rows_visible; ++i)
	{
		const auto row = top + i;
		const auto row_top = impl.grid.top + i * impl.row_height - glide_px;
		const recti row_bounds(impl.grid.left, row_top, impl.grid.right, row_top + impl.row_height);

		if (row >= 0 && row < rows)
		{
			shown.push_back({pattern.get(), row, row == playing, false, row_bounds});
		}
		else if (following && row < 0 && frame.previous_pattern && frame.previous_row + 1 + row >= 0)
		{
			shown.push_back({frame.previous_pattern.get(), frame.previous_row + 1 + row, false, true, row_bounds});
		}
		else if (following && row >= rows && frame.next_pattern && row - rows < frame.next_pattern->rows)
		{
			shown.push_back({frame.next_pattern.get(), row - rows, false, true, row_bounds});
		}
	}

	const render::scoped_clip grid_clip(dc, impl.grid.inflate(impl.band_pad, 0).offset(offset));

	for (const auto& r : shown)
	{
		const auto band = r.bounds.inflate(impl.band_pad, 0).offset(offset);
		const auto faint = r.faint ? 0.5f : 1.0f;

		if (r.lit && !following) dc.draw_rect(band, c(palette::accent(), 0.55f));
		else if (r.row % 4 == 0) dc.draw_rect(band, shaded(palette::line, 0.6f * faint));

		// Where one pattern gives way to the next.
		if (r.row == 0 && following)
			dc.draw_rect(recti(band.left, band.top, band.right, band.top + 1), shaded(palette::line, 2.0f));
	}

	if (following)
	{
		const auto bar_top = impl.grid.top + (playing - top) * impl.row_height;
		dc.draw_rect(recti(impl.grid.left, bar_top, impl.grid.right, bar_top + impl.row_height)
		                 .inflate(impl.band_pad, 0)
		                 .offset(offset),
		             c(palette::accent(), 0.55f));
	}

	for (const auto& r : shown)
	{
		const auto faint = r.faint ? 0.5f : 1.0f;
		const auto& row_bounds = r.bounds;

		dc.draw_text(std::format("{:02X}", r.row),
		             recti(row_bounds.left, row_bounds.top, grid_left, row_bounds.bottom).offset(offset),
		             render::font_face::code, render::text_style::single_line,
		             c(r.lit ? palette::text() : palette::text_faint(), faint), {});

		std::string line;
		std::vector<render::text_highlight_t> spans;

		for (auto v = 0; v < impl.fit.visible_channels; ++v)
		{
			const auto channel = impl.channel_at(first + v);
			if (v > 0) line += ' ';

			const auto start = line.size();
			const auto index = static_cast<size_t>(r.row) * r.source->channels + channel;
			auto text = channel < r.source->channels && index < r.source->cells.size()
			                ? tracker_cell_text(r.source->cells[index].text, impl.fit.detail)
			                : std::string{};
			text.resize(impl.fit.cell_chars, ' ');
			line += text;

			const auto a = silent_alpha(channel) * faint;
			const auto fields = cell_fields(text);
			auto field_start = start;

			for (auto f = 0_z; f < fields.size(); ++f)
			{
				const auto& field = fields[f];
				const auto at = line.find(field, field_start);
				if (at == std::string::npos) break;

				core::color32 colour = palette::text();
				auto field_alpha = a;

				if (is_empty_field(field))
				{
					colour = palette::text_faint();
					field_alpha *= 0.6f;
				}
				else if (f == 1) colour = palette::accent_light();
				else if (f > 1 && f + 1 == fields.size()) colour = palette::orange();
				else if (f > 1) colour = palette::green();

				spans.push_back(
					{static_cast<uint32_t>(at), static_cast<uint32_t>(field.size()), c(colour, field_alpha)});
				field_start = at + field.size();
			}
		}

		dc.draw_text(line, spans, recti(grid_left, row_bounds.top, row_bounds.right, row_bounds.bottom).offset(offset),
		             render::font_face::code, render::text_style::single_line, c(palette::text(), faint), {});
	}

	// A pattern scrolled by hand says so, as long as the playhead is somewhere else.
	if (!d->_tracker_follow && channels > 0 && (playing < top || playing >= top + impl.rows_visible))
	{
		const auto table = impl.grid.inflate(impl.band_pad, 0);
		dc.draw_rect(recti(table.left, table.bottom - 2, table.right, table.bottom).offset(offset),
		             c(palette::accent(), 0.6f));
	}
}

view_controller_ptr tracker_monitor_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                      const pointi offset, hit_test_context& /*ctx*/)
{
	const auto& impl = *_impl;
	if (!impl.state) return nullptr;

	const auto at = [&](const recti r)
	{
		return !r.is_empty() && r.offset(offset).contains(loc);
	};
	const auto weak = std::weak_ptr(_impl);
	const auto click = [&](const recti r, const std::string_view tip, std::function<void()> f)
	{
		return std::make_shared<tracker_click_controller>(host, r.offset(offset), std::string(tip), std::move(f));
	};

	// Acted on at release, and the stack can be rebuilt while the button is held, destroying this monitor; so
	// each action holds the monitor weakly and does nothing once it has gone.
	if (at(impl.follow))
	{
		return click(impl.follow, tt.tracker_follow, [weak]
		{
			if (const auto self = weak.lock())
			{
				self->display->_tracker_follow = !self->display->_tracker_follow;
				self->host->invalidate(ui::host_invalid::redraw);
			}
		});
	}

	for (const auto forward : {false, true})
	{
		const auto& arrow = forward ? impl.next_channels : impl.previous_channels;

		if (at(arrow))
		{
			return click(arrow, {}, [weak, forward]
			{
				if (const auto self = weak.lock())
				{
					const auto step = std::max(1, self->fit.visible_channels);
					self->display->_tracker_first_channel = std::clamp(self->first_channel() + (forward ? step : -step),
					                                                   0,
					                                                   std::max(0, self->channel_count() -
					                                                                   self->fit.visible_channels));
					self->host->invalidate(ui::host_invalid::layout | ui::host_invalid::redraw);
				}
			});
		}
	}

	const auto first = impl.first_channel();

	for (auto i = 0; i < static_cast<int>(impl.mutes.size()); ++i)
	{
		const auto channel = impl.channel_at(first + i);

		for (const auto solo : {false, true})
		{
			const auto& toggle = solo ? impl.solos[i] : impl.mutes[i];
			if (!at(toggle)) continue;

			return click(toggle, solo ? tt.sid_solo : tt.command_volume0, [weak, channel, solo]
			{
				const auto self = weak.lock();
				if (self && self->state->display_state() == self->display) self->toggle_mix(channel, solo);
			});
		}
	}

	return nullptr;
}

std::vector<recti> tracker_monitor_control::keyboard_parts() const
{
	const auto& impl = *_impl;
	if (!impl.state) return {};

	std::vector<recti> parts;
	const auto add = [&parts](const recti r)
	{
		if (!r.is_empty()) parts.emplace_back(r);
	};

	add(impl.previous_channels);
	add(impl.next_channels);
	add(impl.follow);

	for (auto i = 0_z; i < impl.mutes.size(); ++i)
	{
		add(impl.mutes[i]);
		if (i < impl.solos.size()) add(impl.solos[i]);
	}

	return parts;
}

bool tracker_monitor_control::wheel(const pointi loc, const pointi offset, const int delta)
{
	auto& impl = *_impl;
	if (!impl.state) return false;

	// Anywhere across the rows and their header, the table's margins included when it is centred.
	const recti area(impl.bounds.left, impl.header.top, impl.bounds.right, impl.grid.bottom);
	if (!area.offset(offset).contains(loc)) return false;
	if (delta == 0 || !impl.pattern) return true;

	// Scrolling the pattern by hand stops it following playback, from where it was - the top row render keeps
	// while following - and Follow playback resumes it.
	auto& d = *impl.display;
	const auto rows = static_cast<int>(std::lround(-delta / 40.0));
	const auto steps = rows != 0 ? rows : (delta > 0 ? -1 : 1);
	const auto half = impl.rows_visible / 2;
	d._tracker_follow = false;
	d._tracker_top_row = std::clamp(d._tracker_top_row + steps, -half, std::max(-half, impl.pattern_rows() - 1 - half));
	impl.host->invalidate(ui::host_invalid::redraw);
	return true;
}

// clang-format off
tracker_grid_fit tracker_monitor_control::grid_fit() const { return _impl->fit; }
int tracker_monitor_control::rows_shown() const { return _impl->rows_visible; }
recti tracker_monitor_control::grid_bounds() const { return _impl->grid; }
recti tracker_monitor_control::header_bounds() const { return _impl->header; }
std::vector<recti> tracker_monitor_control::channel_headers() const { return _impl->headers; }
std::vector<recti> tracker_monitor_control::mute_toggles() const { return _impl->mutes; }
std::vector<recti> tracker_monitor_control::solo_toggles() const { return _impl->solos; }
recti tracker_monitor_control::follow_toggle() const { return _impl->follow; }
// clang-format on

view_element_ptr make_tracker_bank_panel(const display_state_ptr& display)
{
	const auto details = display && display->_tracker ? display->_tracker->details() : nullptr;
	if (!details || (details->samples.empty() && details->instruments.empty())) return {};

	// A section of the stack, as a file's streams are: a title over a common table, the samples and then the
	// instruments, numbered as the module numbers them, under headings that sort and size the columns as the
	// stack's other tables do. Their names are often the module's message, laid out in the tracker's
	// fixed-width font, so they keep it. The bank the patterns play says how: how many notes start each entry,
	// between which notes, and on how many channels - blank for one no note plays, which leaves a message
	// written across the names to read as one.
	auto panel = std::make_shared<ui::panel_element>(flex_item::stretch);
	panel->flex_container.direction = flex_direction::column;
	panel->flex_container.wrap = flex_wrap::no_wrap;
	panel->flex_container.align_items = flex_align::stretch;
	panel->flex_container.gap = {0, 4};

	const auto code = [](const std::string_view text, const render::text_style style)
	{
		return std::make_shared<text_element>(text, render::font_face::code, style, view_element_style::none);
	};

	const auto add_bank = [&](const std::string_view title, const std::string_view name,
	                          const std::vector<std::string>& names, const std::vector<tracker_bank_use>& use)
	{
		if (names.empty()) return;

		const auto with_use = use.size() == names.size();
		auto table = std::make_shared<ui::table_element>(flex_item::stretch);
		table->no_shrink_col[0] = true;

		std::vector<ui::table_heading> headings = {{"#", true, true, true}, stack_heading(tt.sort_by_name.sv())};

		if (with_use)
		{
			for (const auto column : {2, 3, 4}) table->no_shrink_col[column] = true;
			headings.emplace_back(stack_heading(tt.tracker_notes.sv(), true));
			headings.push_back({std::string(tt.tracker_range.sv()), true, true, true});
			headings.emplace_back(stack_heading(tt.prop_name_channels.sv(), true));
		}

		table->headings(std::move(headings));
		keep_table_choices(table, display, name);

		for (auto i = 0_z; i < names.size(); ++i)
		{
			std::vector<view_element_ptr> cells = {
				code(std::format("{:02X}", i + 1), render::text_style::single_line),
				code(names[i].empty() ? std::string_view("-") : std::string_view(names[i]),
			         render::text_style::single_line),
			};
			std::vector<ui::table_sort_key> keys = {ui::table_sort_key(i + 1), names[i].empty()
			                                                                       ? ui::table_sort_key{}
			                                                                       : ui::table_sort_key(names[i])};

			// An entry no note plays has nothing to sort by in these columns, so it follows those that are played.
			if (with_use)
			{
				const auto& u = use[i];
				const auto count = [](const int n)
				{
					return n > 0 ? std::to_string(n) : std::string{};
				};
				const auto key = [](const int n)
				{
					return n > 0 ? ui::table_sort_key(n) : ui::table_sort_key{};
				};
				cells.emplace_back(code(count(u.notes), render::text_style::single_line_far));
				cells.emplace_back(code(tracker_note_range(u), render::text_style::single_line_far));
				cells.emplace_back(code(count(u.channels), render::text_style::single_line_far));
				keys.emplace_back(key(u.notes));
				keys.emplace_back(key(u.notes > 0 ? u.lowest : 0));
				keys.emplace_back(key(u.channels));
			}

			table->add_row(cells, std::move(keys));
		}

		panel->add(std::make_shared<group_title_control>(title));
		panel->add(table);
	};

	add_bank(tt.tracker_samples, "samples", details->samples, details->sample_use);
	add_bank(tt.tracker_instruments, "instruments", details->instruments, details->instrument_use);
	return panel;
}
