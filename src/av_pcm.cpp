// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: A shared in-memory WAV stream for native SID and tracker renderers, including unaligned reads.

#include "pch.h"
#include "av_pcm.h"

namespace
{
	constexpr uint64_t wav_header_bytes = 44;
	constexpr uint64_t max_read_bytes = 8192;

	void put_le16(uint8_t* p, const uint32_t v)
	{
		p[0] = static_cast<uint8_t>(v);
		p[1] = static_cast<uint8_t>(v >> 8);
	}

	void put_le32(uint8_t* p, const uint32_t v)
	{
		put_le16(p, v);
		put_le16(p + 2, v >> 16);
	}

	class rendered_audio_file_base : public platform::file
	{
	public:
		virtual void describe(av_media_info& info) const = 0;
	};

	template <typename Sample> class rendered_audio_file final : public rendered_audio_file_base
	{
		static constexpr int sample_bytes = sizeof(Sample);
		std::shared_ptr<pcm_audio_render<Sample>> _render;
		mutable uint64_t _pos = 0;
		mutable bool _loaded = false;
		mutable bool _ok = false;
		mutable uint64_t _samples = 0;
		mutable rendered_audio_format _format;
		mutable std::array<uint8_t, wav_header_bytes> _header = {};
		mutable std::vector<Sample> _scratch;

		void ensure_loaded() const
		{
			if (_loaded) return;
			_loaded = true;
			if (!_render->load()) return;
			_format = _render->format();
			_samples = _render->sample_count();

			if (_format.sample_rate <= 0 || _format.channels < 1 || _format.channels > 2 || _samples == 0 ||
			    _samples > (UINT32_MAX - 36) / (static_cast<uint64_t>(sample_bytes) * _format.channels) ||
			    static_cast<uint64_t>(_format.sample_rate) * _format.channels * sample_bytes > UINT32_MAX)
			{
				core::log(__FUNCTION__, "native music cannot be represented as a PCM WAV stream");
				return;
			}

			const auto bytes_per_frame = _format.channels * sample_bytes;
			const auto data_bytes = static_cast<uint32_t>(_samples * bytes_per_frame);
			memcpy(_header.data(), "RIFF", 4);
			put_le32(_header.data() + 4, 36 + data_bytes);
			memcpy(_header.data() + 8, "WAVEfmt ", 8);
			put_le32(_header.data() + 16, 16);
			put_le16(_header.data() + 20, std::is_same_v<Sample, float> ? 3 : 1);
			put_le16(_header.data() + 22, _format.channels);
			put_le32(_header.data() + 24, _format.sample_rate);
			put_le32(_header.data() + 28,
			         static_cast<uint32_t>(static_cast<uint64_t>(_format.sample_rate) * bytes_per_frame));
			put_le16(_header.data() + 32, bytes_per_frame);
			put_le16(_header.data() + 34, sample_bytes * 8);
			memcpy(_header.data() + 36, "data", 4);
			put_le32(_header.data() + 40, data_bytes);
			_ok = true;
		}

	public:
		explicit rendered_audio_file(std::shared_ptr<pcm_audio_render<Sample>> render) : _render(std::move(render)) {}

		uint64_t size() const override
		{
			ensure_loaded();
			return _ok ? wav_header_bytes + _samples * _format.channels * sample_bytes : 0;
		}

		uint64_t read(uint8_t* buf, const uint64_t buf_size) const override
		{
			const auto total = size();
			if (!_ok || _pos >= total || buf_size == 0) return 0;
			const auto wanted = std::min({buf_size, total - _pos, max_read_bytes});
			uint64_t done = 0;

			if (_pos < wav_header_bytes)
			{
				done = std::min(wanted, wav_header_bytes - _pos);
				memcpy(buf, _header.data() + _pos, static_cast<size_t>(done));
			}

			if (done < wanted)
			{
				const auto frame_bytes = static_cast<uint64_t>(_format.channels) * sample_bytes;
				const auto byte = _pos + done - wav_header_bytes;
				const auto first = byte / frame_bytes;
				const auto skip = byte % frame_bytes;
				const auto count = static_cast<size_t>((skip + wanted - done + frame_bytes - 1) / frame_bytes);
				_scratch.resize(count * _format.channels);
				const auto got = _render->read(first, _scratch.data(), count);
				const auto bytes = std::min<uint64_t>(wanted - done,
				                                      got * frame_bytes > skip ? got * frame_bytes - skip : 0);

				for (uint64_t i = 0; i < bytes; ++i)
				{
					const auto at = skip + i;
					using sample_bits = std::conditional_t<sizeof(Sample) == 2, uint16_t, uint32_t>;
					const auto sample = std::bit_cast<sample_bits>(_scratch[static_cast<size_t>(at / sample_bytes)]);
					buf[done + i] = static_cast<uint8_t>(sample >> ((at % sample_bytes) * 8));
				}

				done += bytes;
			}

			_pos += done;
			return done;
		}

		uint64_t write(const uint8_t*, uint64_t) override { return 0; }
		bool flush() const override { return true; }
		bool trunc(uint64_t) const override { return false; }
		uint64_t pos() const override { return _pos; }
		core::file_path path() const override { return _render->path(); }

		uint64_t seek(const uint64_t pos, const whence w) const override
		{
			const auto total = static_cast<int64_t>(size());
			const auto offset = static_cast<int64_t>(pos);
			int64_t target = offset;
			if (w != whence::begin)
			{
				const auto base = w == whence::current ? static_cast<int64_t>(_pos) : total;
				target = offset > 0 && offset > total - base ? total : base + offset;
			}
			_pos = static_cast<uint64_t>(std::clamp<int64_t>(target, 0, total));
			return _pos;
		}

		void describe(av_media_info& info) const override { _render->describe(info); }
	};
}

platform::file_ptr open_rendered_audio_stream(std::shared_ptr<pcm_audio_render<int16_t>> render)
{
	return std::make_shared<rendered_audio_file<int16_t>>(std::move(render));
}

platform::file_ptr open_rendered_audio_stream(std::shared_ptr<pcm_audio_render<float>> render)
{
	return std::make_shared<rendered_audio_file<float>>(std::move(render));
}

bool is_rendered_audio_stream(const platform::file_ptr& file)
{
	return file && dynamic_cast<const rendered_audio_file_base*>(file.get()) != nullptr;
}

void describe_rendered_audio_stream(const platform::file_ptr& file, av_media_info& info)
{
	if (const auto* stream = dynamic_cast<const rendered_audio_file_base*>(file.get())) stream->describe(info);
}
