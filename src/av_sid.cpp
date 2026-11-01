// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: SID tunes as audio the player plays like any other file. lib8bit runs a tune's play routine and
// its samples reach FFmpeg as a WAV stream, so the session's clock, pause, seeking and end need nothing of
// their own; each frame's SID registers are kept beside the samples for the SID monitor to show.

#include "pch.h"
#include "av_sid.h"
#include "av_format.h"
#include "files.h"

#include "machine.h"

namespace
{
	constexpr uint64_t wav_header_bytes = 44;

	// The most a read hands the demuxer: about 90ms of audio. The read thread asks for a quarter of a
	// megabyte at a time, which would put seconds of rendered sound between a silenced voice and the
	// listener.
	constexpr uint64_t max_read_bytes = 8192;

	// More than one PAL frame of samples at 44.1kHz, which is under 900.
	constexpr int max_frame_samples = 4096;

	// A .sid file holds at most 64K of C64 memory and a 124-byte header. A file that fills this was cut
	// short reading it, so it is refused rather than half loaded.
	constexpr size_t max_sid_file_bytes = 128 * 1024;

	void put_le16(uint8_t* p, const uint32_t v)
	{
		p[0] = static_cast<uint8_t>(v & 0xFF);
		p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
	}

	void put_le32(uint8_t* p, const uint32_t v)
	{
		put_le16(p, v & 0xFFFF);
		put_le16(p + 2, v >> 16);
	}

	std::array<uint8_t, wav_header_bytes> make_wav_header(const uint64_t samples)
	{
		const auto data_bytes = static_cast<uint32_t>(samples * 2);
		std::array<uint8_t, wav_header_bytes> h = {};
		memcpy(h.data(), "RIFF", 4);
		put_le32(h.data() + 4, 36 + data_bytes);
		memcpy(h.data() + 8, "WAVEfmt ", 8);
		put_le32(h.data() + 16, 16);
		put_le16(h.data() + 20, 1); // PCM
		put_le16(h.data() + 22, 1); // mono
		put_le32(h.data() + 24, sid_sample_rate);
		put_le32(h.data() + 28, sid_sample_rate * 2);
		put_le16(h.data() + 32, 2);
		put_le16(h.data() + 34, 16);
		memcpy(h.data() + 36, "data", 4);
		put_le32(h.data() + 40, data_bytes);
		return h;
	}

	class sid_wav_stream final : public platform::file
	{
		std::shared_ptr<sid_tune_render> _render;
		mutable uint64_t _pos = 0;
		mutable bool _loaded = false;
		mutable bool _ok = false;
		mutable uint64_t _samples = 0;
		mutable std::array<uint8_t, wav_header_bytes> _header = {};
		mutable std::vector<int16_t> _scratch;

		void ensure_loaded() const
		{
			if (_loaded) return;
			_loaded = true;
			_ok = _render->load();

			if (_ok)
			{
				_samples = _render->sample_count();
				_header = make_wav_header(_samples);
			}
		}

	public:
		explicit sid_wav_stream(std::shared_ptr<sid_tune_render> render) : _render(std::move(render))
		{
		}

		uint64_t size() const override
		{
			ensure_loaded();
			return _ok ? wav_header_bytes + _samples * 2 : 0;
		}

		uint64_t read(uint8_t* buf, const uint64_t buf_size) const override
		{
			const auto total = size();
			if (!_ok || _pos >= total || buf_size == 0) return 0;

			const auto wanted = std::min({buf_size, total - _pos, max_read_bytes});
			uint64_t done = 0;

			if (_pos < wav_header_bytes)
			{
				const auto n = std::min(wanted, wav_header_bytes - _pos);
				memcpy(buf, _header.data() + _pos, static_cast<size_t>(n));
				done = n;
			}

			if (done < wanted)
			{
				// The samples are little-endian 16-bit, and a read may start or end in the middle of one.
				const auto byte = _pos + done - wav_header_bytes;
				const auto first = byte / 2;
				const auto last = (byte + (wanted - done) - 1) / 2;
				const auto count = static_cast<size_t>(last - first + 1);

				_scratch.resize(count);
				const auto got = _render->read(first, _scratch.data(), count);
				const auto skip = byte % 2;

				for (auto i = 0_z; i < got && done < wanted; ++i)
				{
					const auto v = static_cast<uint16_t>(_scratch[i]);
					const uint8_t bytes[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8)};

					for (auto b = (i == 0 ? skip : 0); b < 2 && done < wanted; ++b)
					{
						buf[done++] = bytes[b];
					}
				}
			}

			_pos += done;
			return done;
		}

		uint64_t write(const uint8_t*, uint64_t) override { return 0; }
		bool flush() const override { return true; }

		uint64_t seek(const uint64_t pos, const whence w) const override
		{
			// Offsets from the current position and the end arrive as two's complement.
			const auto total = static_cast<int64_t>(size());
			const auto offset = static_cast<int64_t>(pos);
			int64_t target = offset;
			if (w == whence::current) target = static_cast<int64_t>(_pos) + offset;
			else if (w == whence::end) target = total + offset;
			_pos = static_cast<uint64_t>(std::clamp<int64_t>(target, 0, total));
			return _pos;
		}

		uint64_t pos() const override { return _pos; }
		bool trunc(uint64_t) const override { return false; }
		df::file_path path() const override { return _render->path(); }
	};
}

sid_tune_render::sid_tune_render(df::file_path path, const int song, const double seconds) :
	_path(std::move(path)), _requested_song(song), _seconds(seconds)
{
}

sid_tune_render::~sid_tune_render() = default;

bool sid_tune_render::load()
{
	platform::exclusive_lock render_lock(_render_mutex);
	if (_loaded) return _load_ok;
	_loaded = true;

	const auto bytes = df::blob_head_from_file(_path, max_sid_file_bytes);
	if (bytes.empty() || bytes.size() >= max_sid_file_bytes) return false;

	sid_tune_info info;
	if (!machine::parse_sid(bytes.data(), bytes.size(), info)) return false;

	auto m = std::make_unique<machine>();
	if (!m->load_sid(bytes.data(), bytes.size(), _requested_song)) return false;

	const auto texts = read_sid_header_texts(bytes);

	sid_tune_details details;
	details.title = texts.title;
	details.author = texts.author;
	details.released = texts.released;
	details.songs = info.songs;
	details.song = m->sid_song();
	details.chip_8580 = info.chip_8580;
	details.ntsc = info.ntsc;
	details.clock_hz = info.ntsc ? sid_clock_ntsc : sid_clock_pal;

	auto description = std::make_shared<const av_media_info>(files::scan_sid(bytes).to_info());

	const auto samples = static_cast<uint64_t>(std::llround(std::max(1.0, _seconds) * sid_sample_rate));
	const auto frames = static_cast<size_t>(samples / 800 + 2);

	{
		platform::exclusive_lock data_lock(_data_mutex);
		_details = std::move(details);
		_description = std::move(description);
		_pcm.reserve(static_cast<size_t>(samples) + max_frame_samples);
		_frames.reserve(frames);
		_frame_starts.reserve(frames);
	}

	sid_snapshot initial;
	m->capture_sid(initial);
	std::copy_n(std::begin(initial.writes), sid_register_count, _last_writes.begin());

	_machine = std::move(m);
	_sample_count = samples;
	_load_ok = true;
	return true;
}

uint64_t sid_tune_render::sample_count() const
{
	return _sample_count;
}

void sid_tune_render::render_frame()
{
	const auto silent = _silent_mask.load();
	for (int v = 0; v < 3; ++v) _machine->set_sid_voice_muted(v, (silent & (1 << v)) != 0);

	_machine->exec(pal_cycles_per_frame);

	int16_t samples[max_frame_samples];
	const auto n = std::max(0, _machine->generate_audio(samples, max_frame_samples));

	sid_snapshot snap;
	_machine->capture_sid(snap);

	sid_frame_state frame;
	for (int r = 0; r < sid_register_count; ++r)
	{
		frame.regs[r] = snap.regs[r];
		frame.writes[r] = static_cast<uint8_t>(std::min<uint32_t>(255, snap.writes[r] - _last_writes[r]));
		_last_writes[r] = snap.writes[r];
	}
	for (int v = 0; v < 3; ++v) frame.envelope[v] = snap.envelope[v];

	platform::exclusive_lock data_lock(_data_mutex);
	_frame_starts.emplace_back(_pcm.size());
	_frames.emplace_back(frame);
	_pcm.insert(_pcm.end(), samples, samples + n);
}

size_t sid_tune_render::read(const uint64_t first, int16_t* out, const size_t count)
{
	platform::exclusive_lock render_lock(_render_mutex);
	if (!_load_ok) return 0;

	const auto total = _sample_count.load();
	const auto end = std::min<uint64_t>(first + count, total);
	if (first >= end) return 0;

	for (;;)
	{
		{
			platform::shared_lock data_lock(_data_mutex);
			if (_pcm.size() >= end) break;
		}

		render_frame();
	}

	platform::shared_lock data_lock(_data_mutex);
	const auto n = static_cast<size_t>(end - first);

	for (auto i = 0_z; i < n; ++i)
	{
		out[i] = faded(first + i, _pcm[static_cast<size_t>(first + i)]);
	}

	return n;
}

int16_t sid_tune_render::faded(const uint64_t at, const int16_t sample) const
{
	const auto total = _sample_count.load();
	const auto fade_samples = std::min<uint64_t>(static_cast<uint64_t>(sid_fade_seconds * sid_sample_rate), total / 4);
	if (fade_samples == 0 || at + fade_samples < total) return sample;

	const auto remaining = at < total ? total - at : 0;
	return static_cast<int16_t>(sample * static_cast<double>(remaining) / static_cast<double>(fade_samples));
}

bool sid_tune_render::frame_at(const double seconds, sid_frame_state& out) const
{
	return frames_before(seconds, &out, 1) == 1;
}

size_t sid_tune_render::frames_before(const double seconds, sid_frame_state* out, const size_t count) const
{
	const auto sample = static_cast<uint64_t>(std::max(0.0, seconds) * sid_sample_rate);

	platform::shared_lock data_lock(_data_mutex);
	if (count == 0 || _frame_starts.empty() || sample >= _pcm.size()) return 0;

	// The frame whose samples include this one: its state is the chip's at the end of that frame.
	const auto next = std::ranges::upper_bound(_frame_starts, sample);
	const auto last = static_cast<size_t>(std::max<ptrdiff_t>(0, next - _frame_starts.begin() - 1));
	const auto first = last + 1 > count ? last + 1 - count : 0;
	const auto n = last + 1 - first;

	std::copy_n(_frames.begin() + static_cast<ptrdiff_t>(first), n, out);
	return n;
}

size_t sid_tune_render::samples_before(const double seconds, int16_t* out, const size_t count) const
{
	const auto sample = static_cast<uint64_t>(std::max(0.0, seconds) * sid_sample_rate);

	platform::shared_lock data_lock(_data_mutex);
	const auto end = std::min<uint64_t>(sample, _pcm.size());
	const auto start = end > count ? end - count : 0;
	const auto n = static_cast<size_t>(end - start);
	for (auto i = 0_z; i < n; ++i) out[i] = faded(start + i, _pcm[static_cast<size_t>(start + i)]);
	return n;
}

sid_tune_details sid_tune_render::details() const
{
	platform::shared_lock data_lock(_data_mutex);
	return _details;
}

void sid_tune_render::describe(av_media_info& info) const
{
	platform::shared_lock data_lock(_data_mutex);
	if (!_description) return;

	const auto chip = sid_chip_name(_details.chip_8580);
	info.audio_codec = str::cache(chip);
	info.bitrate = 0;
	info.metadata = _description->metadata;

	for (auto& st : info.streams)
	{
		if (st.type == av_stream_type::audio) st.codec = chip;
	}
}

void sid_tune_render::mix(const sid_voice_mix& m)
{
	_silent_mask = m.silent_mask();
}

platform::file_ptr open_sid_stream(std::shared_ptr<sid_tune_render> render)
{
	return std::make_shared<sid_wav_stream>(std::move(render));
}

bool is_sid_stream(const platform::file_ptr& file)
{
	return file && dynamic_cast<const sid_wav_stream*>(file.get()) != nullptr;
}

std::string sid_waveform_name(const uint8_t control)
{
	std::string result;
	const auto add = [&result](const std::string_view name)
	{
		if (!result.empty()) result += '+';
		result += name;
	};

	if (control & 0x10) add("TRI");
	if (control & 0x20) add("SAW");
	if (control & 0x40) add("PULSE");
	if (control & 0x80) add("NOISE");
	return result.empty() ? "OFF" : result;
}

std::string_view sid_chip_name(const bool chip_8580)
{
	return chip_8580 ? "MOS 8580" : "MOS 6581";
}

sid_header_texts read_sid_header_texts(const df::cspan data)
{
	sid_header_texts result;
	if (data.size < 0x76) return result;

	const auto field = [&data](const size_t offset)
	{
		std::string text;

		for (auto i = 0_z; i < 32; ++i)
		{
			const auto c = data.data[offset + i];
			if (c == 0) break;

			if (c < 0x20 || (c >= 0x7F && c < 0xA0)) text += ' ';
			else if (c < 0x80) text += static_cast<char>(c);
			else
			{
				text += static_cast<char>(0xC0 | (c >> 6));
				text += static_cast<char>(0x80 | (c & 0x3F));
			}
		}

		return std::string(str::trim(text));
	};

	result.title = field(0x16);
	result.author = field(0x36);
	result.released = field(0x56);
	return result;
}

std::string sid_note_name(const uint16_t frequency, const uint32_t clock_hz)
{
	if (frequency == 0 || clock_hz == 0) return {};

	// The oscillator adds the register to a 24-bit accumulator every cycle, so it completes
	// frequency * clock / 2^24 cycles a second.
	static constexpr std::string_view names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	const auto hertz = frequency * static_cast<double>(clock_hz) / 16777216.0;
	const auto midi = static_cast<int>(std::lround(12.0 * std::log2(hertz / 440.0) + 69.0));
	if (midi < 0) return {};

	return std::format("{}{}", names[midi % 12], midi / 12 - 1);
}
