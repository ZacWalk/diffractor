// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Direct libopenmpt playback with bounded PCM and immutable pattern/sample monitor snapshots - how far
// through its row each is, and the patterns either side - how the patterns use the module's samples and
// instruments, and which channels they leave empty.

#pragma once

#include "av_pcm.h"
#include <deque>

constexpr int tracker_sample_rate = 48000;

bool is_tracker_extension(std::string_view extension);

// libopenmpt numbers notes from 1, C-0, to 128; the values above them are commands such as note off and cut.
constexpr int tracker_highest_note = 128;

// How the patterns play one sample or instrument: how many notes start it, the lowest and highest of them, and
// on how many channels. A sample no note plays has none of these.
struct tracker_bank_use
{
	int notes = 0;
	int lowest = 0;
	int highest = 0;
	int channels = 0;
};

struct tracker_tune_details
{
	std::string title;
	std::string artist;
	std::string type;
	std::string tracker;
	std::string message;
	std::string date;
	// One of the module's pattern cells as the decoder writes it - note, instrument, volume and effect - which it
	// writes at one width for the whole module, so this is what the pattern grid is sized by.
	std::string cell;
	std::vector<std::string> channels;
	// Which channels have nothing in them - no note, instrument, volume or effect - in any pattern the module plays.
	// Empty for a module too large to read through.
	std::vector<bool> empty_channels;
	std::vector<std::string> samples;
	std::vector<std::string> instruments;
	// How the patterns play the bank their instrument column names, read in the order they play: the
	// instruments, where the module has them, and its samples where not. The other is empty, and both are for
	// a module too large to read through.
	std::vector<tracker_bank_use> sample_use;
	std::vector<tracker_bank_use> instrument_use;
	std::vector<int> orders;
	double duration = 0;
	int patterns = 0;
};

struct tracker_channel_mix
{
	std::vector<bool> muted;
	int solo = -1;

	bool is_silent(const int channel) const
	{
		return solo >= 0 ? channel != solo
		                 : channel >= 0 && static_cast<size_t>(channel) < muted.size() && muted[channel];
	}

	friend bool operator==(const tracker_channel_mix&, const tracker_channel_mix&) = default;
};

struct tracker_pattern_cell
{
	std::string text;
	std::string highlight;
	int instrument = 0;
};

struct tracker_pattern
{
	int index = 0;
	int rows = 0;
	int channels = 0;
	std::vector<tracker_pattern_cell> cells;
};

struct tracker_frame_state
{
	int order = 0;
	int row = 0;
	// How far through the row the moment is, from 0 as it starts towards 1 as the next does.
	double row_progress = 0;
	int speed = 0;
	double tempo = 0;
	std::vector<float> levels;
	std::shared_ptr<const tracker_pattern> pattern;
	// The pattern played before this one, and the last of its rows played; and the pattern the order list plays
	// after it. Null where there is none.
	std::shared_ptr<const tracker_pattern> previous_pattern;
	int previous_row = 0;
	std::shared_ptr<const tracker_pattern> next_pattern;
};

class tracker_tune_render final : public pcm_audio_render<float>
{
public:
	explicit tracker_tune_render(core::file_path path, platform::file_ptr file = {});
	~tracker_tune_render() override;

	bool load() override;
	rendered_audio_format format() const override { return {tracker_sample_rate, 2}; }
	uint64_t sample_count() const override;
	size_t read(uint64_t first, float* out, size_t count) override;
	core::file_path path() const override { return _path; }
	void describe(av_media_info& info) const override;

	std::shared_ptr<const tracker_tune_details> details() const;
	bool frame_at(double seconds, tracker_frame_state& out) const;
	uint64_t revision() const;
	void mix(const tracker_channel_mix& mix);

private:
	struct snapshot
	{
		uint64_t first = 0;
		uint64_t end = 0;
		tracker_frame_state state;
	};

	void publish_frame(uint64_t first, uint64_t end);
	// Puts the module at a sample of its timeline exactly.
	void place(uint64_t first);
	// One of the module's patterns, formatted once while it or a neighbour plays; null for none.
	std::shared_ptr<const tracker_pattern> formatted_pattern(int index);

	const core::file_path _path;
	platform::file_ptr _file;
	mutable core::mutex _render_mutex;
	std::unique_ptr<class tracker_decoder> _decoder;
	bool _loaded = false;
	bool _ok = false;
	uint64_t _next_sample = 0;
	uint64_t _cache_start = 0;
	std::vector<float> _pcm;
	tracker_channel_mix _applied_mix;
	std::shared_ptr<const tracker_pattern> _pattern;
	// The order the frames last published were in, its neighbours, and the patterns formatted for them.
	int _order = -1;
	int _row = 0;
	std::shared_ptr<const tracker_pattern> _previous_pattern;
	int _previous_row = 0;
	std::shared_ptr<const tracker_pattern> _next_pattern;
	std::map<int, std::shared_ptr<const tracker_pattern>> _formatted;

	mutable core::mutex _data_mutex;
	std::shared_ptr<const tracker_tune_details> _details;
	std::deque<snapshot> _frames;
	tracker_channel_mix _mix;
	uint64_t _revision = 0;
};
