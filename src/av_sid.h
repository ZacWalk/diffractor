// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: SID tunes as audio the player plays like any other file. lib64 runs a tune's play routine and
// its samples reach FFmpeg as a WAV stream, so the session's clock, pause, seeking and end need nothing of
// their own; each frame's SID registers are kept beside the samples for the SID monitor to show. How long a
// tune lasts is found by running it apart, silently and far faster than it plays: until its last note has
// died away and nothing more sounds, or until it starts again from where it first began to repeat.

#pragma once

#include "av_pcm.h"

class av_media_info;

// Seconds of a tune the player presents until the scan below says how long it lasts, and of one the scan
// cannot tell: the file does not say, and a tune can play for ever. A tune that repeats ends in a fade.
constexpr double sid_tune_seconds = 180.0;
constexpr double sid_fade_seconds = 3.0;
constexpr int sid_sample_rate = 44100;

// The most of a tune the scan runs. Silent, it runs at nearly two hundred times the speed it plays, so this is
// at most about ten seconds of a worker's time, for a tune it never finds the end of; a repeat has to be heard
// through once more and a minute beyond before it counts, so the longest repeating tune it finds is a little
// under half of it - the longest of the epics written for the chip.
constexpr double sid_scan_seconds = 1800.0;
// The span a tune's stream is declared with, so a tune the scan finds to be long has the room to play out.
constexpr double sid_stream_seconds = sid_scan_seconds + sid_fade_seconds;

// How long a tune lasts as far as running it shows: until it falls silent, the moment its last note has died
// away; or until it starts again, the moment it begins to repeat. Unknown for a tune that does neither within
// the scan.
struct sid_tune_length
{
	enum class ending
	{
		unknown,
		falls_silent,
		repeats,
	};

	ending how = ending::unknown;
	double seconds = 0.0;
};

// Runs one tune of a .sid file - song is 1-based, 0 the one the file starts on - for at most max_seconds of
// its time, or until abandoned says to stop.
sid_tune_length scan_sid_tune_length(core::cspan file, int song, double max_seconds = sid_scan_seconds,
                                     const std::function<bool()>& abandoned = {});

// The same for a file on disk, remembered for the file as it is - its path, size and content - so asking about a
// tune again costs nothing. Unknown for a file that cannot be read as a tune.
sid_tune_length find_sid_tune_length(core::file_path path, int song, const std::function<bool()>& abandoned = {});

// How long a tune is heard for, by what a scan found: until it falls silent; through once and a fade as it starts
// again where it repeats; and for the span every tune is given where the scan cannot tell.
double sid_heard_seconds(const sid_tune_length& length);

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

	bool is_silent(const int voice) const { return solo >= 0 ? voice != solo : muted[voice]; }

	uint8_t silent_mask() const
	{
		uint8_t mask = 0;
		for (int v = 0; v < 3; ++v)
			if (is_silent(v)) mask |= static_cast<uint8_t>(1 << v);
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
// open_rendered_audio_stream wraps it in; the UI thread reads the chip state and the samples already rendered. The
// samples are kept, so going back is a copy and only going forward runs the emulator. The stream is declared
// with the room a long tune needs; where the tune ends within it starts as the span every tune once got, and
// moves to what find_sid_tune_end learns - silence follows it.
class sid_tune_render final : public pcm_audio_render<int16_t>
{
public:
	// song is 1-based; 0 plays the tune the file starts on.
	sid_tune_render(core::file_path path, int song, double seconds = sid_stream_seconds);
	~sid_tune_render();

	// Reads and checks the tune: false when the file is not one lib64 can play. Called on the thread
	// that renders, before anything else, and only once.
	bool load() override;

	rendered_audio_format format() const override { return {sid_sample_rate, 1}; }
	uint64_t sample_count() const override;

	// Copies up to count samples starting at first into out, rendering any not rendered yet. Returns the
	// number copied, which is fewer only at the end.
	size_t read(uint64_t first, int16_t* out, size_t count) override;

	// Where the tune is heard to end, faded out if it repeats; silence follows it in the stream.
	double end_seconds() const;
	// Whether the end is what a scan found rather than the span every tune is given until one has.
	bool end_found() const { return _end_found.load(); }
	// Ends the tune where a scan says it does. An unknown length leaves the span every tune is given.
	void set_length(const sid_tune_length& length);

	// The chip as it was at a playback position. False until that frame is rendered.
	bool frame_at(double seconds, sid_frame_state& out) const;

	// The frames up to and including the one at a playback position, oldest first, so a monitor can fade
	// what changed recently. Returns how many there were.
	size_t frames_before(double seconds, sid_frame_state* out, size_t count) const;

	// The samples that end at a playback position, newest last. Returns how many there were.
	size_t samples_before(double seconds, int16_t* out, size_t count) const;

	sid_tune_details details() const;
	core::file_path path() const override { return _path; }
	int requested_song() const { return _requested_song; }

	// A session plays the WAV the render makes, and its info describes that WAV: PCM at 689 kbit/s, and
	// the WAV's layout. This puts the tune in its place - the chip for the codec, no bit rate, and the
	// tags and layout the index reads from the file. Leaves info alone until the render has loaded.
	void describe(av_media_info& info) const override;

	// Takes effect from the next frame rendered.
	void mix(const sid_voice_mix& m);

private:
	void render_frame();
	// The sample as heard: the moments before the end fade out when the tune repeats, and nothing after it.
	int16_t faded(uint64_t at, int16_t sample) const;

	const core::file_path _path;
	const int _requested_song;
	const double _seconds;

	// The emulator and what only the rendering thread touches. Held for the whole of a read.
	mutable core::mutex _render_mutex;
	_Guarded_by_(_render_mutex) std::unique_ptr<class machine> _machine;
	_Guarded_by_(_render_mutex) std::array<uint32_t, sid_register_count> _last_writes = {};
	_Guarded_by_(_render_mutex) bool _loaded = false;
	_Guarded_by_(_render_mutex) bool _load_ok = false;

	// What both threads read. Taken only for copies, never while the emulator runs.
	mutable core::mutex _data_mutex;
	_Guarded_by_(_data_mutex) std::vector<int16_t> _pcm;
	_Guarded_by_(_data_mutex) std::vector<uint64_t> _frame_starts;
	_Guarded_by_(_data_mutex) std::vector<sid_frame_state> _frames;
	_Guarded_by_(_data_mutex) sid_tune_details _details;
	// The file as the index describes it, read when the render loads.
	_Guarded_by_(_data_mutex) std::shared_ptr<const av_media_info> _description;
	std::atomic<uint64_t> _sample_count = 0;
	// Where the tune ends, and over how many samples before that it fades. Set at construction, then by a
	// scan on another thread; read by both.
	std::atomic<uint64_t> _end_samples = 0;
	std::atomic<uint64_t> _fade_samples = 0;
	std::atomic<bool> _end_found = false;

	std::atomic<uint8_t> _silent_mask = 0;
};

// Scans the tune a render plays and ends it where the scan says. Run on a worker: it holds the render only
// to read what it needs and to publish what it finds, and stops early once the render is let go. True when it
// found an end.
bool find_sid_tune_end(const std::weak_ptr<sid_tune_render>& render);

// The name of the waveforms a voice's control register selects, as the chip's data sheet abbreviates
// them; "OFF" when none is selected.
std::string sid_waveform_name(uint8_t control);

// The chip a tune was written for, which is what the panel and the index give as its codec.
std::string_view sid_chip_name(bool chip_8580);

// The title, author and release a .sid file's header holds, as the format writes them: ISO-8859-1, padded to
// 32 bytes. lib64 reads them as ASCII, but composers' names need their accents. Empty when the data is too
// short to hold a header; the caller has already checked it is one.
struct sid_header_texts
{
	std::string title;
	std::string author;
	std::string released;
};

sid_header_texts read_sid_header_texts(core::cspan data);

// The equal-tempered note nearest a voice's frequency register, such as "A4"; empty for zero.
std::string sid_note_name(uint16_t frequency, uint32_t clock_hz);
