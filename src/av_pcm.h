// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Native music renderers presented to the existing player as short, seekable PCM reads.

#pragma once

class av_media_info;

struct rendered_audio_format
{
	int sample_rate = 44100;
	int channels = 1;
};

template <typename Sample> class pcm_audio_render : public core::no_copy
{
public:
	~pcm_audio_render() override = default;
	virtual bool load() = 0;
	virtual rendered_audio_format format() const = 0;
	virtual uint64_t sample_count() const = 0;
	// Counts are sample frames; stereo output is interleaved.
	virtual size_t read(uint64_t first, Sample* out, size_t count) = 0;
	virtual core::file_path path() const = 0;
	virtual void describe(av_media_info& info) const = 0;
};

platform::file_ptr open_rendered_audio_stream(std::shared_ptr<pcm_audio_render<int16_t>> render);
platform::file_ptr open_rendered_audio_stream(std::shared_ptr<pcm_audio_render<float>> render);
bool is_rendered_audio_stream(const platform::file_ptr& file);
void describe_rendered_audio_stream(const platform::file_ptr& file, av_media_info& info);

constexpr int rendered_audio_packet_bytes = 1024;
