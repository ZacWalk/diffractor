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

#pragma once

class av_media_info;

// Seconds of each tune the player presents. A SID tune plays for ever and the file does not say how long
// a tune lasts, so every tune gets the same span, ending in a fade rather than a cut.
constexpr double sid_tune_seconds = 180.0;
constexpr double sid_fade_seconds = 3.0;
constexpr int sid_sample_rate = 44100;

// The 25 registers the CPU writes: three voices of seven, then the filter and volume.
constexpr int sid_register_count = 25;

// What the SID monitor shows for one PAL frame of a tune: the last value the play routine wrote to each
// register - the chip's registers are write-only - each voice's envelope level, and how many writes each
// register took during the frame.
struct sid_frame_state
{
	std::array<uint8_t, sid_register_count> regs = {};
	std::array<uint8_t, 3> envelope = {};
	std::array<uint8_t, sid_register_count> writes = {};
};

// The voices the listener has silenced. A solo silences every other voice and takes precedence over the
// mutes, as turbo8bit's player does. Neither is chip state: the tune keeps running underneath, so a voice
// comes back mid-note.
struct sid_voice_mix
{
	std::array<bool, 3> muted = {};
	int solo = -1;

	bool is_silent(const int voice) const
	{
		return solo >= 0 ? voice != solo : muted[voice];
	}

	uint8_t silent_mask() const
	{
		uint8_t mask = 0;
		for (int v = 0; v < 3; ++v) if (is_silent(v)) mask |= static_cast<uint8_t>(1 << v);
		return mask;
	}

	friend bool operator==(const sid_voice_mix&, const sid_voice_mix&) = default;
};

// The tune a render plays, from its header.
struct sid_tune_details
{
	std::string title;
	std::string author;
	std::string released;
	int songs = 1;
	int song = 1; // the tune this render plays, 1-based
	bool chip_8580 = false;
	bool ntsc = false;
	uint32_t clock_hz = 985248;
};

// One tune of a .sid file, rendered as it is read. The decoder's read thread renders through the stream
// open_sid_stream wraps it in; the UI thread reads the chip state and the samples already rendered. The
// samples are kept, so going back is a copy and only going forward runs the emulator.
class sid_tune_render final : df::no_copy
{
public:
	// song is 1-based; 0 plays the tune the file starts on.
	sid_tune_render(df::file_path path, int song, double seconds = sid_tune_seconds);
	~sid_tune_render();

	// Reads and checks the tune: false when the file is not one lib8bit can play. Called on the thread
	// that renders, before anything else, and only once.
	bool load();

	uint64_t sample_count() const;

	// Copies up to count samples starting at first into out, rendering any not rendered yet. Returns the
	// number copied, which is fewer only at the end.
	size_t read(uint64_t first, int16_t* out, size_t count);

	// The chip as it was at a playback position. False until that frame is rendered.
	bool frame_at(double seconds, sid_frame_state& out) const;

	// The frames up to and including the one at a playback position, oldest first, so a monitor can fade
	// what changed recently. Returns how many there were.
	size_t frames_before(double seconds, sid_frame_state* out, size_t count) const;

	// The samples that end at a playback position, newest last. Returns how many there were.
	size_t samples_before(double seconds, int16_t* out, size_t count) const;

	sid_tune_details details() const;
	df::file_path path() const { return _path; }

	// A session plays the WAV the render makes, and its info describes that WAV: PCM at 689 kbit/s, and
	// the WAV's layout. This puts the tune in its place - the chip for the codec, no bit rate, and the
	// tags and layout the index reads from the file. Leaves info alone until the render has loaded.
	void describe(av_media_info& info) const;

	// Takes effect from the next frame rendered.
	void mix(const sid_voice_mix& m);

private:
	void render_frame();
	// The sample as heard: the last moments of the span fade out.
	int16_t faded(uint64_t at, int16_t sample) const;

	const df::file_path _path;
	const int _requested_song;
	const double _seconds;

	// The emulator and what only the rendering thread touches. Held for the whole of a read.
	mutable platform::mutex _render_mutex;
	_Guarded_by_(_render_mutex) std::unique_ptr<class machine> _machine;
	_Guarded_by_(_render_mutex) std::array<uint32_t, sid_register_count> _last_writes = {};
	_Guarded_by_(_render_mutex) bool _loaded = false;
	_Guarded_by_(_render_mutex) bool _load_ok = false;

	// What both threads read. Taken only for copies, never while the emulator runs.
	mutable platform::mutex _data_mutex;
	_Guarded_by_(_data_mutex) std::vector<int16_t> _pcm;
	_Guarded_by_(_data_mutex) std::vector<uint64_t> _frame_starts;
	_Guarded_by_(_data_mutex) std::vector<sid_frame_state> _frames;
	_Guarded_by_(_data_mutex) sid_tune_details _details;
	// The file as the index describes it, read when the render loads.
	_Guarded_by_(_data_mutex) std::shared_ptr<const av_media_info> _description;
	std::atomic<uint64_t> _sample_count = 0;

	std::atomic<uint8_t> _silent_mask = 0;
};

// The render as a 16-bit mono WAV file, for av_format_decoder to open. Reads are kept short, so the
// demuxer holds only a moment of audio ahead of the listener and a voice silenced in the monitor is heard
// to go quiet promptly.
platform::file_ptr open_sid_stream(std::shared_ptr<sid_tune_render> render);
bool is_sid_stream(const platform::file_ptr& file);

// Packet size the WAV demuxer is asked for on a SID stream: about 12ms of audio, for the same reason.
constexpr int sid_stream_packet_bytes = 1024;

// The name of the waveforms a voice's control register selects, as the chip's data sheet abbreviates
// them; "OFF" when none is selected.
std::string sid_waveform_name(uint8_t control);

// The chip a tune was written for, which is what the panel and the index give as its codec.
std::string_view sid_chip_name(bool chip_8580);

// The title, author and release a .sid file's header holds, as the format writes them: ISO-8859-1, padded to
// 32 bytes. lib8bit reads them as ASCII, but composers' names need their accents. Empty when the data is too
// short to hold a header; the caller has already checked it is one.
struct sid_header_texts
{
	std::string title;
	std::string author;
	std::string released;
};

sid_header_texts read_sid_header_texts(df::cspan data);

// The equal-tempered note nearest a voice's frequency register, such as "A4"; empty for zero.
std::string sid_note_name(uint16_t frequency, uint32_t clock_hz);
