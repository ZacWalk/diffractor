// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Audio output interface and buffer management. Defines audio format structures,
// sample buffering, and abstract audio device interface for playback.

#pragma once
#include "model_property.h"
#include "libdesktop/platform_render.h"

struct AVChannelLayout;
using channel_layout_ptr = std::shared_ptr<AVChannelLayout>;

channel_layout_ptr av_get_def_channel_layout(int num_channels);
// An endpoint's layout: channels per frame, placed by its speaker mask where the two agree.
channel_layout_ptr av_get_channel_layout(uint64_t mask, int channels);

struct audio_info_t
{
	uint32_t sample_rate = 0;
	prop::audio_sample_t sample_fmt = prop::audio_sample_t::none;
	channel_layout_ptr channel_layout;

	friend bool operator==(const audio_info_t& lhs, const audio_info_t& rhs);

	friend bool operator!=(const audio_info_t& lhs, const audio_info_t& rhs)
	{
		return !(lhs == rhs);
	}

	uint32_t bytes_per_second() const;
	uint32_t channel_count() const;
	uint32_t bytes_per_sample() const;
};

// An endpoint's mix format as the buffers and the mixer take it.
audio_info_t av_audio_info(const platform::audio_output_format& format);

class audio_buffer final : df::no_copy
{
	int gen = -1;

	uint8_t* data = nullptr;
	uint32_t size = 0;
	uint32_t start_pos = 0;
	uint32_t end_pos = 0;

	double time = 0.0;
	const uint32_t data_alignment = 16u;
	const uint32_t data_padding = 64u;

	audio_info_t format;

public:
	void init(const audio_info_t& format_in)
	{
		format = format_in;
		const auto wanted = 4 * format.bytes_per_second();

		// A failed realloc answers null and leaves the old block where it was. Assigning the result
		// unexamined leaked that block and kept a size describing memory that was not there, which
		// the next append then wrote into. Refused, the buffer holds nothing and plays silence.
		auto* const resized = static_cast<uint8_t*>(_aligned_realloc(
			data, static_cast<size_t>(wanted) + static_cast<size_t>(data_padding), data_alignment));

		if (resized)
		{
			data = resized;
			size = wanted;
		}
		else
		{
			size = 0;
		}

		start_pos = 0;
		end_pos = 0;
	}

	~audio_buffer() override
	{
		_aligned_free(data);
	}

	bool is_empty() const
	{
		return start_pos == end_pos;
	}

	bool should_fill() const
	{
		return used_bytes() <= format.bytes_per_second();
	}

	uint32_t used_bytes() const
	{
		return end_pos - start_pos;
	}

	df::cspan cdata() const
	{
		return {data ? data + start_pos : nullptr, used_bytes()};
	}

	// Hands the endpoint as many of the samples as it has room for; what it takes leaves the buffer.
	void write_to(platform::audio_output& device)
	{
		remove(static_cast<uint32_t>(device.write(cdata())));
	}

	uint32_t avail_bytes() const
	{
		return size - end_pos;
	}

	uint32_t bytes_per_sample() const
	{
		return format.bytes_per_sample();
	}

	uint32_t bytes_per_second() const
	{
		return format.bytes_per_second();
	}

	int generation() const
	{
		return gen;
	}

	void clear()
	{
		start_pos = 0;
		end_pos = 0;
		time = 0.0;
		if (data) memset(data, 0, size);
	}

	void remove(const uint32_t bytes_to_remove)
	{
		df::assert_true(bytes_to_remove <= used_bytes());
		start_pos += bytes_to_remove;
		const auto bps = format.bytes_per_second();
		if (bps > 0) time += bytes_to_remove / static_cast<double>(bps);

		if (start_pos == end_pos)
		{
			start_pos = 0;
			end_pos = 0;
		}
	}

	void append(const uint8_t* data_in, const uint32_t bytes_in, const double time_in, const int generation_in)
	{
		if (generation_in != gen)
		{
			if (bytes_in <= size)
			{
				memcpy_s(data, size, data_in, bytes_in);
				start_pos = 0;
				end_pos = bytes_in;
				time = time_in;
				gen = generation_in;
			}
		}
		else
		{
			const auto original_used_bytes = used_bytes();

			if (bytes_in > avail_bytes() && start_pos > 0)
			{
				memmove_s(data, size, data + start_pos, original_used_bytes);
				start_pos = 0;
				end_pos = original_used_bytes;
			}

			if (bytes_in <= avail_bytes())
			{
				memcpy_s(data + end_pos, size - end_pos, data_in, bytes_in);
				end_pos += bytes_in;

				const auto bps = format.bytes_per_second();
				time = bps > 0 ? time_in - original_used_bytes / static_cast<double>(bps) : time_in;
			}
		}
	}

	void append_blank_second()
	{
		const auto blank_bytes = format.bytes_per_second();

		if (blank_bytes > avail_bytes() && start_pos > 0)
		{
			const auto bytes = used_bytes();
			memmove_s(data, size, data + start_pos, bytes);
			start_pos = 0;
			end_pos = bytes;
		}

		if (blank_bytes <= avail_bytes())
		{
			memset(data + end_pos, 0, blank_bytes);
			end_pos += blank_bytes;
		}
	}

	// Linearly ramps the last `seconds` of buffered audio down to silence so a clip
	// that ends on a non-zero sample does not produce an audible click when the
	// following silence (or stop) begins.
	void apply_fade_out(const double seconds)
	{
		const auto frames = ramp_frames(seconds);

		if (frames > 0)
		{
			apply_ramp(data + end_pos - frames * frame_bytes(), frames, 1.0f, 0.0f);
		}
	}

	// The same ramp at the head of the buffer: a device started on a non-zero sample
	// steps the output away from silence, which the speaker reproduces as a pop.
	void apply_fade_in(const double seconds)
	{
		const auto frames = ramp_frames(seconds);

		if (frames > 0)
		{
			apply_ramp(data + start_pos, frames, 0.0f, 1.0f);
		}
	}

	double end_time() const
	{
		const auto bps = format.bytes_per_second();
		return bps > 0 ? time + used_bytes() / static_cast<double>(bps) : time;
	}

	double start_time() const
	{
		return time;
	}

	double seconds() const
	{
		const auto bps = format.bytes_per_second();
		return bps > 0 ? static_cast<double>(used_bytes()) / bps : 0.0;
	}

private:
	uint32_t frame_bytes() const
	{
		return format.channel_count() * format.bytes_per_sample();
	}

	// Whole frames a `seconds` ramp covers, limited to what is buffered.
	uint32_t ramp_frames(const double seconds) const
	{
		const auto bps = format.bytes_per_second();
		const auto fb = frame_bytes();

		if (bps == 0 || fb == 0 || seconds <= 0.0 || is_empty())
		{
			return 0;
		}

		return std::min(used_bytes(), static_cast<uint32_t>(seconds * bps)) / fb;
	}

	void apply_ramp(uint8_t* p, const uint32_t frames, const float from, const float to)
	{
		const auto channels = format.channel_count();
		const auto fb = frame_bytes();
		const auto step = (to - from) / static_cast<float>(frames);

		for (uint32_t f = 0; f < frames; ++f)
		{
			const auto gain = from + step * static_cast<float>(f + 1);

			for (uint32_t ch = 0; ch < channels; ++ch)
			{
				switch (format.sample_fmt)
				{
				case prop::audio_sample_t::signed_float:
					reinterpret_cast<float*>(p)[ch] *= gain;
					break;
				case prop::audio_sample_t::signed_16bit:
					reinterpret_cast<int16_t*>(p)[ch] = static_cast<int16_t>(reinterpret_cast<int16_t*>(p)[ch] * gain);
					break;
				case prop::audio_sample_t::signed_32bit:
					reinterpret_cast<int32_t*>(p)[ch] = static_cast<int32_t>(reinterpret_cast<int32_t*>(p)[ch] * gain);
					break;
				default:
					break;
				}
			}

			p += fb;
		}
	}

	friend class av_visualizer;
	friend class audio_resampler;
	friend struct audio_ramp_probe;
};

