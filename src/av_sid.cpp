// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: SID tunes as audio the player plays like any other file. lib64 runs a tune's play routine and
// its samples reach FFmpeg as a WAV stream, so the session's clock, pause, seeking and end need nothing of
// their own; each frame's SID registers are kept beside the samples for the SID monitor to show.

#include "pch.h"
#include "av_sid.h"
#include "av_format.h"
#include "files.h"

#include "machine.h"

namespace
{
	// More than one PAL frame of samples at 44.1kHz, which is under 900.
	constexpr int max_frame_samples = 4096;

	// A .sid file holds at most 64K of C64 memory and a 124-byte header. A file that fills this was cut
	// short reading it, so it is refused rather than half loaded.
	constexpr size_t max_sid_file_bytes = 128 * 1024;

	// The scan's measures, in frames of the play routine's clock. A tune that sounds nothing for this long
	// has finished, and the moment after its last sound is kept for the release to ring out.
	constexpr int silent_frames_to_end = 150;
	constexpr int release_frames = 50;
	// A repeat is a stretch of frames that sounded before, found by its first window of frames, no shorter than
	// the least a tune's repeating part could be; and it counts once it has gone on through all of what it
	// repeats and a while more, so a passage played twice within a tune is not taken for the tune starting again.
	constexpr int repeat_window_frames = 64;
	constexpr int least_repeat_frames = 100;
	constexpr int repeat_confirm_frames = 750;

	uint64_t mix_hash(const uint64_t h, const uint64_t v)
	{
		return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
	}

	// What a frame sounds like, as far as telling one pass of a tune from the next goes: each voice's gate,
	// waveform and the note its frequency is nearest. A sweep of a pulse or a filter that runs on from one pass
	// to the next leaves the notes the same, so they are what repeats.
	uint64_t frame_signature(const sid_snapshot& s, const uint32_t clock_hz)
	{
		uint64_t h = 0;

		for (int v = 0; v < 3; ++v)
		{
			const auto* const r = s.regs + v * 7;
			const auto frequency = static_cast<unsigned>(r[0] | (r[1] << 8));
			const auto note = frequency == 0 ? -1
			                                 : static_cast<int>(std::lround(
												   12.0 * std::log2(frequency * static_cast<double>(clock_hz) /
			                                                        16777216.0 / 440.0) +
												   69.0));
			h = mix_hash(h, r[4] & 0xF1u);
			h = mix_hash(h, static_cast<uint64_t>(note) + 1);
		}

		return h;
	}

	// Whether anything can be heard: a voice gated on with a waveform at a volume above nothing, or samples
	// played through the volume register, which a tune that plays them writes many times a frame.
	bool frame_sounds(const sid_snapshot& s, const sid_snapshot& before)
	{
		if (s.writes[0x18] - before.writes[0x18] > 2) return true;
		if ((s.regs[0x18] & 0x0F) == 0) return false;

		for (int v = 0; v < 3; ++v)
		{
			const auto control = s.regs[v * 7 + 4];
			if ((control & 0x01) && (control & 0xF0)) return true;
		}

		return false;
	}

	// What a scan found, for a file as it is - its path, size and content - and the tune asked about.
	struct sid_length_key
	{
		std::string path;
		uint64_t size = 0;
		uint32_t crc = 0;
		int song = 0;
		double max_seconds = 0;
		auto operator<=>(const sid_length_key&) const = default;
	};

	core::mutex sid_lengths_mutex;
	_Guarded_by_(sid_lengths_mutex) std::map<sid_length_key, sid_tune_length> sid_lengths;
	constexpr size_t max_sid_lengths = 256;
}

sid_tune_length scan_sid_tune_length(const core::cspan file, const int song, const double max_seconds,
                                     const std::function<bool()>& abandoned)
{
	sid_tune_info info;
	if (!machine::parse_sid(file.data, file.size, info)) return {};

	auto m = std::make_unique<machine>();
	if (!m->load_sid(file.data, file.size, song)) return {};

	// Without its sound the chip only takes the registers the play routine writes, which is all the scan reads,
	// and the machine runs many times faster.
	m->set_audio_enabled(false);

	const auto clock_hz = info.ntsc ? sid_clock_ntsc : sid_clock_pal;
	const auto frame_seconds = static_cast<double>(pal_cycles_per_frame) / clock_hz;
	const auto max_frames = static_cast<int>(std::max(0.0, max_seconds) / frame_seconds);

	std::vector<uint64_t> signatures;
	signatures.reserve(static_cast<size_t>(std::min(max_frames, 64 * 1024)));
	std::unordered_map<uint64_t, std::vector<int>> windows;
	// Each period a stretch of frames has been heard to repeat at, and the frame its repeat began.
	std::map<int, int> repeats;
	uint64_t window_scale = 1;
	for (int i = 0; i < repeat_window_frames; ++i) window_scale *= 1099511628211ull;
	uint64_t window = 0;

	sid_snapshot before;
	m->capture_sid(before);
	auto last_sound = -1;

	for (auto n = 0; n < max_frames; ++n)
	{
		if (abandoned && (n & 255) == 0 && abandoned()) return {};

		m->exec(pal_cycles_per_frame);
		sid_snapshot s;
		m->capture_sid(s);

		if (frame_sounds(s, before)) last_sound = n;
		before = s;

		if (last_sound >= 0 && n - last_sound >= silent_frames_to_end)
		{
			return {sid_tune_length::ending::falls_silent, (last_sound + 1 + release_frames) * frame_seconds};
		}

		const auto signature = frame_signature(s, clock_hz);
		signatures.push_back(signature);
		window = window * 1099511628211ull + signature;
		if (n >= repeat_window_frames) window -= signatures[n - repeat_window_frames] * window_scale;

		// A repeat this frame does not carry on is no repeat.
		std::erase_if(repeats, [&](const auto& r) { return signatures[n] != signatures[n - r.first]; });

		if (n >= repeat_window_frames - 1)
		{
			auto& ends = windows[window];

			for (const auto k : ends)
			{
				const auto period = n - k;
				if (period < least_repeat_frames || repeats.contains(period)) continue;

				auto same = true;
				for (auto j = 0; j < repeat_window_frames && same; ++j) same = signatures[n - j] == signatures[k - j];
				if (same) repeats.emplace(period, n - repeat_window_frames + 1);
			}

			ends.push_back(n);
		}

		for (const auto& [period, start] : repeats)
		{
			if (n - start + 1 >= period + repeat_confirm_frames)
			{
				return {sid_tune_length::ending::repeats, start * frame_seconds};
			}
		}
	}

	return {};
}

bool find_sid_tune_end(const std::weak_ptr<sid_tune_render>& render)
{
	core::file_path path;
	int song = 0;

	if (const auto r = render.lock())
	{
		path = r->path();
		song = r->requested_song();
	}
	else
	{
		return false;
	}

	const auto length = find_sid_tune_length(path, song, [&render] { return render.expired(); });
	const auto r = render.lock();
	if (!r) return false;
	r->set_length(length);
	return length.how != sid_tune_length::ending::unknown;
}

sid_tune_length find_sid_tune_length(const core::file_path path, const int song, const std::function<bool()>& abandoned)
{
	const auto bytes = platform::blob_head_from_file(path, max_sid_file_bytes);
	if (bytes.empty() || bytes.size() >= max_sid_file_bytes) return {};

	const sid_length_key key{path.str(), bytes.size(), core::crc32c(bytes.data(), bytes.size()), song,
	                         sid_scan_seconds};

	{
		core::shared_lock lock(sid_lengths_mutex);
		if (const auto found = sid_lengths.find(key); found != sid_lengths.end()) return found->second;
	}

	const auto length = scan_sid_tune_length({bytes.data(), bytes.size()}, song, sid_scan_seconds, abandoned);

	// A scan stopped part way knows nothing about the tune, so it is not remembered as having found nothing.
	if (abandoned && abandoned()) return length;

	core::exclusive_lock lock(sid_lengths_mutex);
	if (sid_lengths.size() >= max_sid_lengths) sid_lengths.clear();
	sid_lengths[key] = length;
	return length;
}

sid_tune_render::sid_tune_render(core::file_path path, const int song, const double seconds) :
	_path(path), _requested_song(song), _seconds(seconds)
{
	// Until a scan says otherwise, the span every tune is given, fading out over its last moments.
	const auto total = static_cast<uint64_t>(std::llround(std::max(1.0, _seconds) * sid_sample_rate));
	const auto end = std::min(total, static_cast<uint64_t>(std::llround(sid_tune_seconds * sid_sample_rate)));
	_end_samples = end;
	_fade_samples = std::min<uint64_t>(static_cast<uint64_t>(sid_fade_seconds * sid_sample_rate), end / 4);
}

sid_tune_render::~sid_tune_render() = default;

bool sid_tune_render::load()
{
	core::exclusive_lock render_lock(_render_mutex);
	if (_loaded) return _load_ok;
	_loaded = true;

	const auto bytes = platform::blob_head_from_file(_path, max_sid_file_bytes);
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
	// Room for the tune as long as it is first given; a scan that finds it longer lets the vectors grow.
	const auto expected = std::min(samples, _end_samples.load());
	const auto frames = static_cast<size_t>(expected / 800 + 2);

	{
		core::exclusive_lock data_lock(_data_mutex);
		_details = std::move(details);
		_description = std::move(description);
		_pcm.reserve(static_cast<size_t>(expected) + max_frame_samples);
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

	core::exclusive_lock data_lock(_data_mutex);
	_frame_starts.emplace_back(_pcm.size());
	_frames.emplace_back(frame);
	_pcm.insert(_pcm.end(), samples, samples + n);
}

size_t sid_tune_render::read(const uint64_t first, int16_t* out, const size_t count)
{
	core::exclusive_lock render_lock(_render_mutex);
	if (!_load_ok) return 0;

	const auto total = _sample_count.load();
	const auto end = std::min<uint64_t>(first + count, total);
	if (first >= end) return 0;

	// Past where the tune ends the stream is silence, and nothing more is rendered for it - not even to reach
	// a read that starts beyond it.
	const auto heard = std::min(end, _end_samples.load());

	for (;;)
	{
		{
			core::shared_lock data_lock(_data_mutex);
			if (heard <= first || _pcm.size() >= heard) break;
		}

		render_frame();
	}

	core::shared_lock data_lock(_data_mutex);
	const auto n = static_cast<size_t>(end - first);

	for (auto i = 0_z; i < n; ++i)
	{
		const auto at = first + i;
		out[i] = at < heard ? faded(at, _pcm[static_cast<size_t>(at)]) : int16_t{0};
	}

	return n;
}

int16_t sid_tune_render::faded(const uint64_t at, const int16_t sample) const
{
	const auto end = _end_samples.load();
	const auto fade_samples = _fade_samples.load();
	if (at >= end) return 0;
	if (fade_samples == 0 || at + fade_samples < end) return sample;

	return static_cast<int16_t>(sample * static_cast<double>(end - at) / static_cast<double>(fade_samples));
}

double sid_tune_render::end_seconds() const
{
	return static_cast<double>(_end_samples.load()) / sid_sample_rate;
}

double sid_heard_seconds(const sid_tune_length& length)
{
	switch (length.how)
	{
	case sid_tune_length::ending::repeats:
		// The whole of the tune, then a fade as it starts again.
		return length.seconds + sid_fade_seconds;
	case sid_tune_length::ending::falls_silent: return length.seconds;
	case sid_tune_length::ending::unknown: break;
	}

	return sid_tune_seconds;
}

void sid_tune_render::set_length(const sid_tune_length& length)
{
	if (length.how == sid_tune_length::ending::unknown) return;

	const auto rate = static_cast<double>(sid_sample_rate);
	const auto total = static_cast<uint64_t>(std::llround(std::max(1.0, _seconds) * rate));
	const auto end = std::clamp<uint64_t>(static_cast<uint64_t>(std::llround(sid_heard_seconds(length) * rate)), 1,
	                                      total);
	const auto fade = length.how == sid_tune_length::ending::repeats
	                      ? static_cast<uint64_t>(std::llround(sid_fade_seconds * rate))
	                      : uint64_t{0};

	_fade_samples = std::min(fade, end);
	_end_samples = end;
	_end_found = true;
}

bool sid_tune_render::frame_at(const double seconds, sid_frame_state& out) const
{
	return frames_before(seconds, &out, 1) == 1;
}

size_t sid_tune_render::frames_before(const double seconds, sid_frame_state* out, const size_t count) const
{
	const auto sample = static_cast<uint64_t>(std::max(0.0, seconds) * sid_sample_rate);

	core::shared_lock data_lock(_data_mutex);
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

	core::shared_lock data_lock(_data_mutex);
	const auto end = std::min<uint64_t>(sample, _pcm.size());
	const auto start = end > count ? end - count : 0;
	const auto n = static_cast<size_t>(end - start);
	for (auto i = 0_z; i < n; ++i) out[i] = faded(start + i, _pcm[static_cast<size_t>(start + i)]);
	return n;
}

sid_tune_details sid_tune_render::details() const
{
	core::shared_lock data_lock(_data_mutex);
	return _details;
}

void sid_tune_render::describe(av_media_info& info) const
{
	core::shared_lock data_lock(_data_mutex);
	if (!_description) return;

	const auto chip = sid_chip_name(_details.chip_8580);
	info.audio_codec = core::cache(chip);
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

sid_header_texts read_sid_header_texts(const core::cspan data)
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

		return std::string(core::trim(text));
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
