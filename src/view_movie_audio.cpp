// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Draws Movie's audio clips. A SID tune or a tracker module is its monitor, drawn from a display of its own
// whose clock is the frame's moment; any other audio file is the visualizer's bars, fed from its decoded sound,
// beside its cover art. Each frame is laid out under the file's title, on a landscape frame or a Short's upright one,
// and drawn by the software renderer, whose text engine belongs to the worker drawing. SID tunes are shared, one
// render per tune of a file, by every reader of their pictures and sound.

#include "pch.h"
#include "view_movie_audio.h"

#include "app_text.h"
#include "av_sid.h"
#include "av_tracker.h"
#include "files.h"
#include "model_display.h"
#include "ui_elements.h"
#include "ui_music.h"
#include "ui_sid.h"
#include "ui_tracker.h"
#include "libdesktop/render/software.h"

namespace
{
	// How large a frame's text and panels are on a 1080-line frame, against a window at 100%: a movie is watched
	// from further away than a window is read.
	constexpr double ui_scale_at_1080 = 1.5;
	// The smallest frame drawn. One asked for smaller - a tile of the strip - is drawn at this size and shrunk,
	// because panels and text drawn a tenth of their size are not a picture of anything.
	constexpr sizei least_drawn_extent{640, 360};
	// An upright frame is a Short's, which fills a phone's screen, and so does its picture: inside a margin this part of
	// the frame's width all round, with a SID monitor drawn as large as that holds, its voices stacked beside its register
	// grid. YouTube lays its title and buttons over it as over a video's picture; only the movie's words keep clear of
	// them.
	constexpr double upright_margin = 0.025;
	// A Short is watched on a phone, so the title and byline over its picture are this much larger than a movie's.
	constexpr double upright_caption_scale = 1.3;

	// The rate a clip's pictures move at in a Short: a SID tune's play routine runs once a frame of the machine it was
	// written for - 312 lines of 63 cycles at the PAL clock, 263 of 65 at the NTSC one - and a tracker module steps at
	// the 50 a second of the Amiga it came from.
	constexpr double sid_pal_picture_rate = 985248.0 / (63.0 * 312.0);
	constexpr double sid_ntsc_picture_rate = 1022727.0 / (65.0 * 263.0);
	constexpr double tracker_picture_rate = 50.0;

	// The bars hear this much of the sound before a frame that does not follow the last, so they stand where
	// playing would have left them.
	constexpr double bars_warm_up_seconds = 0.5;
	// The steps the bars move in between two frames, as the display steps them for every present.
	constexpr double bars_step_seconds = 1.0 / 60.0;
	// The sound the bars hear, decoded this much at a time.
	constexpr double bars_window_seconds = 12.0;
	constexpr int bars_sample_rate = 48000;
	// How far past a frame the bars must have heard: the visualizer looks a little ahead of the moment it shows.
	constexpr double bars_lead_seconds = 0.05;

	const auto separator = " \xC2\xB7 "sv;

	// A display reaches the application only through these, and one drawn apart from any view asks nothing of
	// it: there is no session to open, nothing to load and no view to repaint.
	class inert_async_strategy final : public async_strategy
	{
	public:
		void queue_ui(std::function<void()>) override {}

		void queue_async(async_queue, std::function<void()>) override {}

		void queue_async_after(async_queue, uint32_t, std::function<void()>) override {}

		void queue_location(std::function<void(location_cache&)>) override {}

		void queue_database(std::function<void(database&)>) override {}

		void queue_tile_db(std::function<void(tile_cache_db&)>) override {}

		void queue_media_preview(std::function<void(media_preview_state&)>, bool) override {}

		void web_service_cache(std::string, std::function<void(const std::string&)>) override {}

		void web_service_cache(std::string, std::string) override {}

		void invalidate_view(view_invalid) override {}
	};

	bool is_sid_tune(const core::file_path path)
	{
		const auto* const type = files::file_type_from_name(path);
		return type && type->has_trait(file_traits::sid_tune);
	}

	// The SID tunes Movie has open, one render for each tune of each file as it is. A tune only plays forward, so
	// every reader of a clip - its pictures, the preview's sound and the render's - shares one, and a reader behind
	// the others copies what they have played rather than playing the tune again from its start. Held weakly: a tune
	// nothing reads is let go.
	std::shared_ptr<sid_tune_render> shared_sid_render(const core::file_path path, const int song)
	{
		using key = std::tuple<std::string, uint64_t, uint64_t, int>;
		static core::mutex renders_mutex;
		static std::map<key, std::weak_ptr<sid_tune_render>> renders;

		const auto attributes = platform::file_attributes(path);
		const key k{path.str(), attributes.size, attributes.modified, song};

		{
			core::shared_lock lock(renders_mutex);
			if (const auto found = renders.find(k); found != renders.end())
			{
				if (auto render = found->second.lock()) return render;
			}
		}

		// Scanned and loaded outside the lock: both read the file, and the scan runs the tune.
		const auto length = find_sid_tune_length(path, song);
		auto render = std::make_shared<sid_tune_render>(path, song, sid_heard_seconds(length));
		render->set_length(length);
		if (!render->load()) return {};

		core::exclusive_lock lock(renders_mutex);
		std::erase_if(renders, [](const auto& entry) { return entry.second.expired(); });

		auto& entry = renders[k];
		if (auto existing = entry.lock()) return existing;
		entry = render;
		return render;
	}

	std::string join_parts(const std::initializer_list<std::string_view> parts)
	{
		std::string result;

		for (const auto part : parts)
		{
			if (part.empty()) continue;
			if (!result.empty()) result += separator;
			result += part;
		}

		return result;
	}

	// Cover art over the frame drawn beneath it: an opaque picture replaces what is there, one with an alpha
	// channel is laid over it.
	void blend_over(core::surface& canvas, const core::surface& art, const pointi at)
	{
		const auto has_alpha = art.format() == core::texture_format::ARGB;
		const auto canvas_extent = canvas.dimensions();
		const auto art_extent = art.dimensions();

		for (auto y = 0; y < art_extent.cy; ++y)
		{
			const auto dst_y = at.y + y;
			if (dst_y < 0 || dst_y >= canvas_extent.cy) continue;

			const auto* const src = std::bit_cast<const uint32_t*>(art.pixels_line(y));
			auto* const dst = std::bit_cast<uint32_t*>(canvas.pixels_line(dst_y));

			for (auto x = std::max(0, -at.x); x < art_extent.cx && at.x + x < canvas_extent.cx; ++x)
			{
				const auto in = src[x];
				const auto a = has_alpha ? in >> 24 : 255u;
				if (a == 0) continue;

				const auto out = dst[at.x + x];
				uint32_t result = 0xff000000u;

				for (auto shift = 0; shift < 24; shift += 8)
				{
					const auto s = (in >> shift) & 0xffu;
					const auto d = (out >> shift) & 0xffu;
					result |= ((s * a + d * (255u - a) + 127u) / 255u) << shift;
				}

				dst[at.x + x] = result;
			}
		}
	}
}

class movie_audio_frames_impl final
{
public:
	movie_audio_frames_impl(core::file_path path, const int song) : _path(path), _song(song)
	{
		if (is_sid_tune(_path)) _kind = kind::sid;
		else if (is_tracker_extension(_path.extension())) _kind = kind::tracker;
	}

	core::file_path path() const { return _path; }

	int song() const { return _song; }

	core::const_surface_ptr frame(double seconds, sizei max_dim, const core::cancel_token& abandon, sizei shape);

private:
	enum class kind
	{
		sid,
		tracker,
		other,
	};

	bool open();
	bool reach(double seconds, const core::cancel_token& abandon);
	void hear(double seconds);
	void read_sound(double from);
	void draw_scene(sizei extent, double seconds);
	double fill_scale(sizei extent, double base, recti frame);
	std::pair<render::font_face, render::font_face> caption_faces(double scale, bool upright, int width) const;
	recti arrange_monitor(render::draw_context& dc, recti below);
	recti arrange_bars(recti below, double scale) const;
	void draw_bars(render::draw_context& dc, recti panel, double scale, double seconds);

	core::file_path _path;
	int _song = 0;
	kind _kind = kind::other;
	bool _opened = false;
	bool _ok = false;

	std::string _title;
	std::string _byline;

	// A monitor's display, which no view shares, and what a display needs to be one. Declared before the display,
	// which refers to them, so they outlive it.
	inert_async_strategy _async;
	common_display_state_t _common;
	display_state_ptr _display;
	view_element_ptr _monitor;
	recti _monitor_bounds;

	std::shared_ptr<sid_tune_render> _sid;
	// How far into the tune it has been played for this clip's frames.
	uint64_t _sid_reached = 0;
	std::shared_ptr<tracker_tune_render> _tracker;
	uint64_t _tracker_next = 0;
	bool _tracker_placed = false;
	std::vector<float> _tracker_scratch;

	// Any other audio file: its cover art, at the size last drawn, and the bars with the sound they hear.
	core::const_image_ptr _art_image;
	core::surface_ptr _art;
	recti _art_bounds;
	std::unique_ptr<av_visualizer> _bars;
	render::vertices_ptr _bar_verts;
	std::vector<int16_t> _sound;
	double _sound_start = 0.0;
	bool _sound_read = false;
	bool _sound_ends = false;
	// How far the bars have heard, and the moment they stand at.
	double _heard = -1.0;
	double _shown = -1.0;

	render::draw_context_device_ptr _dc;
	core::surface_ptr _canvas;
	av_scaler _scaler;
	// The scale an upright frame of this size draws its SID monitor at, once searched for.
	sizei _fill_extent;
	double _fill_scale = 0.0;
};

bool movie_audio_frames_impl::open()
{
	if (_opened) return _ok;
	_opened = true;

	switch (_kind)
	{
	case kind::sid:
	{
		_sid = shared_sid_render(_path, _song);
		if (!_sid) return false;

		// Which of the file's tunes it is, where it has more than one: the monitor shows no selector of its own.
		const auto details = _sid->details();
		_title = details.title;
		_byline = join_parts({details.author, details.released,
		                      details.songs > 1
		                          ? std::format("{} {}/{}", tt.prop_name_track.sv(), details.song, details.songs)
		                          : std::string{}});

		_display = std::make_shared<display_state_t>(_async, _common);
		_display->_sid = _sid;
		_monitor = std::make_shared<sid_monitor_control>(_display);
		break;
	}
	case kind::tracker:
	{
		_tracker = std::make_shared<tracker_tune_render>(_path);
		if (!_tracker->load()) return false;

		if (const auto details = _tracker->details())
		{
			_title = details->title;
			_byline = details->artist;
		}

		_display = std::make_shared<display_state_t>(_async, _common);
		_display->_tracker = _tracker;
		_monitor = std::make_shared<tracker_monitor_control>(_display);
		break;
	}
	case kind::other:
	{
		{
			av_format_decoder decoder;
			if (!open_movie_audio(decoder, _path, media_intent::metadata)) return false;
			decoder.init_streams(-1, -1, false, false, false);
			if (!decoder.has_audio()) return false;
		}

		// The file's tags and cover, as the Items view shows them.
		files ff;
		const auto scanned = ff.scan_file(_path, true, files::file_type_from_name(_path));

		if (scanned.success)
		{
			if (const auto md = scanned.to_props())
			{
				const auto artist = !md->artist.is_empty() ? md->artist.sv() : md->album_artist.sv();
				const auto year = md->year > 0 ? std::to_string(md->year) : std::string();
				_title = md->title.str();
				_byline = join_parts({artist, md->album.sv(), year});
			}

			_art_image = scanned.cover_art;
		}

		_bars = std::make_unique<av_visualizer>();
		break;
	}
	}

	if (_title.empty()) _title = std::string(_path.file_name_without_extension());
	_ok = true;
	return true;
}

// Plays the tune up to the frame, for the monitor shows the chip, or the pattern, as it was at that moment - which
// is known only once it has been played that far. Played forward in steps, so a long way in can be abandoned.
bool movie_audio_frames_impl::reach(const double seconds, const core::cancel_token& abandon)
{
	if (_sid)
	{
		const auto wanted = static_cast<uint64_t>(seconds * sid_sample_rate) + 1;
		int16_t sample = 0;

		while (_sid_reached < wanted)
		{
			if (abandon.is_cancelled()) return false;
			const auto next = std::min<uint64_t>(wanted, _sid_reached + sid_sample_rate);
			_sid->read(next - 1, &sample, 1);
			_sid_reached = next;
		}
	}
	else if (_tracker)
	{
		constexpr uint64_t step = tracker_sample_rate / 10;
		// Played this far past the frame as well, so the monitor knows when the row it shows ends.
		constexpr uint64_t ahead = tracker_sample_rate / 2;
		const auto wanted = static_cast<uint64_t>(seconds * tracker_sample_rate) + 1;

		// Back past what has been played, or further on than playing there is worth: the module is placed just
		// before the frame instead.
		if (!_tracker_placed || wanted + ahead < _tracker_next || wanted > _tracker_next + 2 * tracker_sample_rate)
		{
			_tracker_next = wanted > step ? wanted - step : 0;
			_tracker_placed = true;
		}

		_tracker_scratch.resize(step * 2);

		while (_tracker_next < wanted + ahead)
		{
			if (abandon.is_cancelled()) return false;
			const auto read = _tracker->read(_tracker_next, _tracker_scratch.data(),
			                                 static_cast<size_t>(std::min(step, wanted + ahead - _tracker_next)));
			if (read == 0) break;
			_tracker_next += read;
		}
	}

	return true;
}

// The bars as they stand at a moment, having heard the sound before it as playing would have: frames in order
// carry them on, and anything else starts them again from a little before.
void movie_audio_frames_impl::hear(const double seconds)
{
	const auto follows = _shown >= 0.0 && seconds >= _shown && seconds - _shown <= bars_warm_up_seconds;

	if (!follows)
	{
		_bars->clear();
		_heard = std::max(0.0, seconds - bars_warm_up_seconds);
		_shown = _heard;
	}

	const auto rate = static_cast<double>(bars_sample_rate);
	const auto until = seconds + bars_lead_seconds;
	const auto window_end = _sound_start + static_cast<double>(_sound.size() / 2) / rate;
	const auto fft_seconds = static_cast<double>(FFT_BUFFER_SIZE) / rate;

	if (!_sound_read || _heard < _sound_start || (until + fft_seconds > window_end && !_sound_ends))
	{
		read_sound(_heard);
	}

	const auto frames = _sound.size() / 2;
	const auto first = static_cast<size_t>(std::max(0.0, std::round((_heard - _sound_start) * rate)));
	const auto last = std::min(frames, static_cast<size_t>(std::max(0.0, std::ceil((until - _sound_start) * rate))) +
	                                       FFT_BUFFER_SIZE);

	if (first < last)
	{
		const auto used = _bars->update(_sound.data() + first * 2, last - first, bars_sample_rate,
		                                _sound_start + static_cast<double>(first) / rate);
		_heard = _sound_start + static_cast<double>(first + used) / rate;
	}

	for (auto at = _shown + bars_step_seconds; at < seconds; at += bars_step_seconds) _bars->step(at);
	_bars->step(seconds);
	_shown = seconds;
}

void movie_audio_frames_impl::read_sound(const double from)
{
	_sound_read = true;
	_sound_start = from;
	_sound.clear();
	_sound_ends = true;

	av_format_decoder decoder;
	if (!open_movie_audio(decoder, _path, media_intent::playback)) return;

	decoder.init_streams(-1, -1, false, false, false);
	_sound = decoder.extract_audio_pcm_range(bars_sample_rate, from, bars_window_seconds);

	// A window that came back short reached the end of the file, and asking again would only read the same end.
	_sound_ends = _sound.size() / 2 < static_cast<size_t>(bars_window_seconds * bars_sample_rate) - FFT_BUFFER_SIZE;
}

void movie_audio_frames_impl::draw_scene(const sizei extent, const double seconds)
{
	auto& dc = *_dc;
	// Sized by the frame's shorter side, so a movie's text and panels are the Media view's on a 1080-line screen.
	const auto base = std::min(extent.cx, extent.cy) / 1080.0 * ui_scale_at_1080;
	const auto upright = extent.cy > extent.cx;
	if (auto* const sid = dynamic_cast<sid_monitor_control*>(_monitor.get())) sid->arrange_upright(upright);
	const auto frame = recti(extent).inflate(upright ? -core::round(extent.cx * upright_margin)
	                                                 : -core::round(40 * base));
	const auto scale = upright ? fill_scale(extent, base, frame) : base;

	dc.scale_metrics(scale);
	_dc->begin_draw(extent, std::max(1, core::round(render::default_font_size * scale)), {});

	dc.colors.background = ui::style::colors.view_background;
	dc.colors.foreground = ui::style::colors.view_text;
	dc.colors.selected = ui::style::colors.view_selected_background;
	dc.colors.alpha = 1.0f;
	dc.time_now = seconds;
	dc.clear(core::color(ui::style::colors.view_background, 1.0f));

	const auto gap = core::round(ui::panel_element::spacing * scale);
	const auto [title_face, byline_face] = caption_faces(scale, upright, frame.width());
	const auto title_cy = dc.text_line_height(title_face);
	const auto byline_cy = _byline.empty() ? 0 : dc.text_line_height(byline_face);
	const auto caption_cy = title_cy + byline_cy + gap * 3;

	// The picture is laid out under the caption to learn how tall it is, and the two are then centred together.
	const recti below(frame.left, frame.top + caption_cy, frame.right, frame.bottom);
	const auto picture = _monitor ? arrange_monitor(dc, below) : arrange_bars(below, scale);
	const auto shift = std::max(0, (below.height() - picture.height()) / 2);
	const auto top = frame.top + shift;

	// The file's title, and who made it, over the picture.
	dc.draw_text(_title, recti(frame.left, top, frame.right, top + title_cy), title_face,
	             render::text_style::single_line_center, core::color(ui::style::colors.view_text, 1.0f), {});

	if (!_byline.empty())
	{
		dc.draw_text(_byline, recti(frame.left, top + title_cy, frame.right, top + title_cy + byline_cy), byline_face,
		             render::text_style::single_line_center, core::color(monitor_palette::text_muted(), 1.0f), {});
	}

	_art_bounds = {};

	if (_monitor)
	{
		if (shift > 0)
		{
			_monitor_bounds = _monitor_bounds.offset(0, shift);
			_monitor->layout(dc, _monitor_bounds);
		}

		_display->_position_override = seconds;
		_monitor->render(dc, {0, 0});
	}
	else
	{
		draw_bars(dc, picture.offset(0, shift), scale, seconds);
	}
}

// The scale an upright frame's SID monitor fills it at, the title over it: as large as the frame's height holds,
// across its width. Laid out at `base` across `base / s` of the width, the monitor is the one drawn at `s` made
// `s / base` times larger - its text and its panels grow together - so each step of the search is a layout rather
// than a drawing, and the scale found is then checked with its text at that size, which comes in whole points.
// Searched once for each size of frame.
double movie_audio_frames_impl::fill_scale(const sizei extent, const double base, const recti frame)
{
	if (!dynamic_cast<sid_monitor_control*>(_monitor.get()) || frame.is_empty()) return base;
	if (_fill_extent == extent && _fill_scale > 0.0) return _fill_scale;

	auto& dc = *_dc;
	const auto text_at = [&](const double s)
	{
		dc.scale_metrics(s);
		_dc->begin_draw(extent, std::max(1, core::round(render::default_font_size * s)), {});
	};
	const auto height_at = [&](const double drawn, const double laid_out)
	{
		const auto gap = core::round(ui::panel_element::spacing * laid_out);
		const auto width = std::max(1, core::round(frame.width() * laid_out / drawn));
		const auto [title_face, byline_face] = caption_faces(laid_out, true, width);
		const auto caption_cy = dc.text_line_height(title_face) +
		                        (_byline.empty() ? 0 : dc.text_line_height(byline_face)) + gap * 3;
		return (caption_cy + _monitor->measure(dc, width).cy) * drawn / laid_out;
	};

	text_at(base);
	auto fits = base;

	if (height_at(base, base) <= frame.height())
	{
		auto overflows = base * 4.0;

		for (auto i = 0; i < 12; ++i)
		{
			const auto mid = (fits + overflows) / 2.0;
			if (height_at(mid, base) <= frame.height()) fits = mid;
			else overflows = mid;
		}

		for (auto i = 0; i < 8 && fits > base; ++i)
		{
			text_at(fits);
			if (height_at(fits, fits) <= frame.height()) break;
			fits = std::max(base, fits * 0.98);
		}
	}

	_fill_extent = extent;
	_fill_scale = fits;
	return fits;
}

// The faces the title and byline are drawn in at a scale: the title and dialog faces, as the context's base font size
// sizes them, or in a Short the same family a little larger - each where it fits the frame's width, so a long title is
// never cut off.
std::pair<render::font_face, render::font_face> movie_audio_frames_impl::caption_faces(const double scale,
                                                                                       const bool upright,
                                                                                       const int width) const
{
	if (!upright) return {render::font_face::title, render::font_face::dialog};

	const auto base = std::max(1, core::round(render::default_font_size * scale));
	const auto larger = [&](const render::font_face face, const double size, const std::string_view text)
	{
		render::font_desc desc;
		desc.family = "Calibri";
		desc.size = std::max(1, core::round(size * upright_caption_scale));
		const auto font = render::make_font(desc);
		const auto fits = text.empty() ||
		                  _dc->measure_text(text, font, render::text_style::single_line, 100000).cx <= width;
		return fits ? font : face;
	};

	return {larger(render::font_face::title, base * 1.5, _title), larger(render::font_face::dialog, base, _byline)};
}

// Lays the monitor out under the caption and answers where it went. A SID monitor takes the frame's width, its
// panels as tall as they need; a tracker's panel is wide enough for every channel at full detail where the frame
// has room, and never narrower than two thirds of it, and as tall as the frame allows, so it shows as many rows as
// fit.
recti movie_audio_frames_impl::arrange_monitor(render::draw_context& dc, const recti below)
{
	auto column = below;

	if (const auto* const control = dynamic_cast<tracker_monitor_control*>(_monitor.get()))
	{
		_monitor->layout(dc, below);
		const auto pad = core::round(ui::panel_element::content_padding * dc.scale_factor);
		const auto wanted = control->header_bounds().width() + pad * 4;
		const auto width = std::clamp(wanted, core::mul_div(below.width(), 2, 3), below.width());
		column.left = below.left + (below.width() - width) / 2;
		column.right = column.left + width;
	}

	_monitor->layout(dc, column);
	_monitor_bounds = _monitor->bounds;
	return _monitor_bounds;
}

// The audio panel laid across the frame as large as it fits: the band of bars twice as wide as it is tall, with the
// cover beside it as tall as the band. Answers the panel, centred across the frame at the top of where it may go.
recti movie_audio_frames_impl::arrange_bars(const recti below, const double scale) const
{
	const auto pad = core::round(ui::panel_element::content_padding * scale) * 2;
	const auto heights_across = _art_image ? 3 : 2;
	const auto gaps_across = _art_image ? pad * 3 : pad * 2;
	const auto band_cy = std::max(0,
	                              std::min((below.width() - gaps_across) / heights_across, below.height() - pad * 2));
	const auto panel_cx = band_cy * heights_across + gaps_across;
	const auto left = below.left + (below.width() - panel_cx) / 2;
	return {left, below.top, left + panel_cx, below.top + band_cy + pad * 2};
}

void movie_audio_frames_impl::draw_bars(render::draw_context& dc, const recti panel, const double scale,
                                        const double seconds)
{
	draw_stack_panel(dc, panel);

	const auto pad = core::round(ui::panel_element::content_padding * scale) * 2;
	const auto inside = panel.inflate(-pad);
	auto band = inside;

	if (_art_image)
	{
		const recti square(inside.left, inside.top, inside.left + inside.height(), inside.bottom);
		_art_bounds = core::scale_dimensions(_art_image->dimensions(), square);
		band.left = square.right + pad;
	}

	// The band's resting line, which the bars rise from and fall to, as the audio panel draws it.
	const auto middle = (band.top + band.bottom) / 2;
	dc.draw_rect(recti(band.left, middle, band.right, middle + std::max(1, core::round(scale))),
	             core::color(ui::style::colors.view_text, 0.12f));

	if (!_bar_verts) _bar_verts = dc.create_vertices();
	if (!_bar_verts) return;

	_bars->render(_bar_verts, band, {0, 0}, 1.0f, seconds);
	dc.draw_vertices(_bar_verts);
}

core::const_surface_ptr movie_audio_frames_impl::frame(const double seconds, const sizei max_dim,
                                                       const core::cancel_token& abandon, const sizei shape)
{
	if (abandon.is_cancelled() || !open()) return {};

	const auto wanted = core::scale_dimensions(shape.cx > 0 && shape.cy > 0 ? shape : movie_audio_extent, max_dim);
	if (wanted.cx <= 0 || wanted.cy <= 0) return {};

	// Never drawn smaller than the least that reads, in the frame's own orientation.
	const auto least = wanted.cy > wanted.cx ? sizei{least_drawn_extent.cy, least_drawn_extent.cx} : least_drawn_extent;
	const auto extent = std::min(wanted.cx, wanted.cy) < std::min(least.cx, least.cy) ? least : wanted;
	const auto at = std::max(0.0, seconds);

	if (!reach(at, abandon)) return {};
	if (_bars) hear(at);

	if (!_canvas) _canvas = std::make_shared<core::surface>();
	if (!_dc) _dc = render::create_surface_context(_canvas);
	if (!_dc) return {};

	draw_scene(extent, at);
	if (_dc->render().failed || _canvas->dimensions() != extent) return {};

	if (!_art_bounds.is_empty() && _art_image)
	{
		if (!_art || _art->dimensions() != _art_bounds.extent())
		{
			files ff;
			const auto decoded = ff.image_to_surface(_art_image, _art_bounds.extent());
			_art.reset();
			if (decoded) _scaler.scale_surface(decoded, _art, _art_bounds.extent());
		}

		if (_art) blend_over(*_canvas, *_art, _art_bounds.top_left());
	}

	auto result = std::make_shared<core::surface>();
	result->copy(*_canvas, recti(extent));
	if (extent == wanted) return result;

	core::surface_ptr shrunk;
	return _scaler.scale_surface(result, shrunk, wanted) ? shrunk : nullptr;
}

bool is_movie_audio(const core::file_path path)
{
	const auto* const type = files::file_type_from_name(path);
	return type && type->group == file_group::audio;
}

movie_audio_tunes movie_audio_songs(const core::file_path path)
{
	if (is_tracker_extension(path.extension())) return {1, 1, tracker_picture_rate};
	if (!is_sid_tune(path)) return {};

	sid_tune_render render(path, 0);
	if (!render.load()) return {0, 0};

	// The tune moves at the frame rate of the machine it was written for: its play routine is called once a frame.
	const auto details = render.details();
	return {std::max(1, details.songs), std::clamp(details.song, 1, std::max(1, details.songs)),
	        details.ntsc ? sid_ntsc_picture_rate : sid_pal_picture_rate};
}

double movie_audio_seconds(const core::file_path path, const int song)
{
	if (is_sid_tune(path))
	{
		// A file lib64 cannot play is no tune of any length.
		if (!sid_tune_render(path, song).load()) return 0.0;
		return sid_heard_seconds(find_sid_tune_length(path, song));
	}

	av_format_decoder decoder;
	if (!decoder.open(path, media_intent::metadata)) return 0.0;

	decoder.init_streams(-1, -1, false, false, false);
	const auto info = decoder.info();
	return info.has_audio ? std::max(0.0, info.end - info.start) : 0.0;
}

double movie_audio_loop_seconds(const core::file_path path, const int song)
{
	if (!is_sid_tune(path)) return 0.0;

	// Remembered from the scan movie_audio_seconds ran, so asking again runs nothing.
	const auto length = find_sid_tune_length(path, song);
	return length.how == sid_tune_length::ending::repeats ? length.seconds : 0.0;
}

bool open_movie_audio(av_format_decoder& decoder, const core::file_path path, const media_intent intent, const int song)
{
	if (is_sid_tune(path))
	{
		const auto render = shared_sid_render(path, song);
		return render && decoder.open(open_rendered_audio_stream(render), path, intent);
	}

	return decoder.open(path, intent);
}

movie_audio_frames::movie_audio_frames(core::file_path path, const int song) :
	_impl(std::make_unique<movie_audio_frames_impl>(path, song))
{
}

movie_audio_frames::~movie_audio_frames() = default;

core::file_path movie_audio_frames::path() const
{
	return _impl->path();
}

int movie_audio_frames::song() const
{
	return _impl->song();
}

core::const_surface_ptr movie_audio_frames::frame(const double seconds, const sizei max_dim,
                                                  const core::cancel_token& abandon, const sizei shape)
{
	return _impl->frame(seconds, max_dim, abandon, shape);
}
