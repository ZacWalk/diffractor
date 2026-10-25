// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Animated plasma backdrop. Generates the cycling colour field drawn behind the
// application mark, fading between colour and the sidebar tone as background work starts or stops.

#pragma once

#include "libdesktop/ui.h"

struct plasma
{
	uint32_t _palette[256];
	std::unique_ptr<uint8_t, df::free_delete> _pixels = nullptr;

	double _fade = 0.0;
	int _stride = 0;
	bool _show_color = false;
	bool _hover = false;
	int _cosinus[256];

	// Animation phase in seconds. Per-instance so a second plasma cannot advance this one's frame,
	// and continuous so the frame rate sets smoothness rather than speed.
	double _phase = 0.0;
	double _last_step_time = 0.0;

	constexpr static int fade_max = 40;
	constexpr static int _width = 96;
	constexpr static int _height = 96;

	// The field is redrawn at this rate and no faster. Both phase terms below are truncated to an
	// integer cosine index, so a frame that does not advance that index regenerates an identical
	// image - which is what made a display-rate plasma cost full-rate repaints for nothing.
	constexpr static int frames_per_second = 20;

	// The faster t3/t4 term still advances one cosine index per frame, so the field changes every
	// frame at 20 fps, while the t1/t2 sweep behind it moves at half that for a gentler drift.
	// phase_period is the interval over which both terms wrap the 256-entry table together
	// (256 / t1_per_second), which keeps the pattern seamless across the fmod.
	constexpr static double fade_seconds = 8.0;
	constexpr static double t1_per_second = frames_per_second / 2.0;
	constexpr static double t3_per_second = frames_per_second;
	constexpr static double phase_period = 256.0 / t1_per_second;
	constexpr static double max_step_seconds = 0.25;

	plasma()
	{
		for (auto i = 0; i < 256; ++i)
		{
			_cosinus[i] = static_cast<int>(127.0 * cos(i * M_PI / 64.0) + 128.0);
		}

		_stride = calc_stride(_width, 32);
		_pixels = df::unique_alloc<uint8_t>(_height * _stride);

		init_plasma();
		step_plasma();
	}

	~plasma()
	{
		_pixels.reset();
	}


	void step_plasma()
	{
		const auto phase_to_u8 = [](const double v)
		{
			return static_cast<uint8_t>(static_cast<int32_t>(v) & 0xff);
		};

		uint8_t t1 = phase_to_u8(_phase * t1_per_second);
		uint8_t t2 = phase_to_u8(-_phase * t1_per_second);
		const uint8_t t3_start = phase_to_u8(_phase * t3_per_second);
		const uint8_t t4_start = phase_to_u8(-_phase * t3_per_second);

		for (auto y = 0; y < _height; ++y)
		{
			uint8_t t3 = t3_start;
			uint8_t t4 = t4_start;

			const auto t = _cosinus[t1] + _cosinus[t2];
			auto d = std::bit_cast<uint32_t*>(_pixels.get() + y * _stride);

			for (auto x = 0; x < _width; ++x)
			{
				*d++ = _palette[(t + _cosinus[t3++] + _cosinus[t4]) >> 2 & 0x000000ff];
				t4 += 2;
			}

			t1 += 2;
			t2 += 1;
		}
	}


	void init_plasma()
	{
		const auto fc = fade_max - _fade;

		// Optimize using XMScalarSinCosEst?

		// Palette entries are stored blue first because they upload as texture_format::RGB, which
		// maps to DXGI_FORMAT_B8G8R8X8. Tone the faded colour to be blue like the app.
		const auto fb = _fade * 0x24;
		const auto fg = _fade * 0x22;
		const auto fr = _fade * 0x20;
		constexpr auto cd = static_cast<double>(fade_max) * 3 * 0x24;

		for (int i = 0; i < 256; ++i)
		{
			const auto b = _cosinus[i];
			const auto g = _cosinus[(i + 32) & 0x0ff];
			const auto r = _cosinus[(i + 64) & 0x0ff];

			const auto c = b + g + r;
			const auto bb = df::round(b * fc / fade_max + c * fb / cd);
			const auto gg = df::round(g * fc / fade_max + c * fg / cd);
			const auto rr = df::round(r * fc / fade_max + c * fr / cd);

			_palette[i] = ui::average(ui::style::color::sidebar_background, ui::rgb(bb, gg, rr));
		}
	}

	// time_now is monotonic seconds. Elapsed time, not call count, drives the animation, so a dropped
	// frame costs smoothness rather than speed. Returns false when the call arrived too soon to
	// change anything, which is the caller's cue not to invalidate.
	bool step(const double time_now)
	{
		constexpr auto step_interval = 1.0 / frames_per_second;

		if (_last_step_time > 0.0 && (time_now - _last_step_time) < step_interval)
		{
			return false;
		}

		const auto elapsed = _last_step_time > 0.0
			                     ? std::clamp(time_now - _last_step_time, 0.0, max_step_seconds)
			                     : 0.0;
		_last_step_time = time_now;

		_show_color = df::jobs_running > 0 || _hover;

		const auto fade_delta = elapsed * (fade_max / fade_seconds);
		const auto fade = std::clamp(_show_color ? _fade - fade_delta : _fade + fade_delta, 0.0,
		                             static_cast<double>(fade_max));

		if (fade != _fade)
		{
			_fade = fade;
			init_plasma();
		}

		_phase = fmod(_phase + elapsed, phase_period);
		step_plasma();
		return true;
	}

	void render(const ui::texture_ptr& tex, const sizei dims) const
	{
		df::assert_true(dims.cx <= _width);
		df::assert_true(dims.cy <= _height);

		tex->update({_height, _width}, ui::texture_format::RGB, ui::orientation::top_left, _pixels.get(), _stride,
		            _height * _stride);
	}

	bool is_active() const
	{
		return _show_color || _fade < fade_max;
	}

	static int calc_stride(const int width, const int bpp) noexcept
	{
		const int bpp_width = width * bpp;
		return ((bpp_width + (bpp_width % 8 ? 8 : 0)) / 8 + 3) & ~3;
	}
};
