// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Render tracker modules directly in libopenmpt, placed at any sample exactly; the UI reads snapshots,
// never the live decoder. The module's details include how its patterns play each sample or instrument, and
// which channels they leave empty.

#include "pch.h"
#include "av_tracker.h"
#include "av_format.h"
#include "libopenmpt/libopenmpt_ext.hpp"
#include <sstream>

namespace
{
	constexpr size_t max_module_bytes = 64_z * 1024 * 1024;
	constexpr uint64_t cache_frames = tracker_sample_rate * 4;
	constexpr size_t snapshot_frames = tracker_sample_rate / 100;
	// The most a seek plays unheard to reach the moment it asked for from the row libopenmpt placed it at: a row
	// of the slowest tempo lasts a few seconds.
	constexpr double max_place_seconds = 30.0;

	// Past this many pattern cells a module shows its bank's names alone, so a pathological one cannot hold up
	// its own playback while they are read.
	constexpr int64_t max_use_cells = 4'000'000;

	// How the patterns play the bank their instrument column names, read in the order they play. A note with
	// no instrument of its own plays the one its channel was last given.
	std::vector<tracker_bank_use> read_bank_use(const openmpt::module& module, const int bank_size)
	{
		const auto channels = std::max(0, module.get_num_channels());
		std::vector<tracker_bank_use> use(static_cast<size_t>(std::max(0, bank_size)));
		std::vector<uint8_t> heard(use.size() * static_cast<size_t>(channels));
		std::vector<int> last(static_cast<size_t>(channels));
		int64_t cells = 0;

		for (auto o = 0; o < module.get_num_orders(); ++o)
		{
			const auto pattern = module.get_order_pattern(o);
			const auto rows = module.get_pattern_num_rows(pattern);
			cells += static_cast<int64_t>(rows) * channels;
			if (cells > max_use_cells) return {};

			for (auto r = 0; r < rows; ++r)
			{
				for (auto c = 0; c < channels; ++c)
				{
					const int instrument = module.get_pattern_row_channel_command(pattern, r, c,
					                                                              openmpt::module::command_instrument);
					if (instrument > 0) last[c] = instrument;

					const int note = module.get_pattern_row_channel_command(pattern, r, c,
					                                                        openmpt::module::command_note);
					const auto entry = last[c] - 1;
					if (note < 1 || note > tracker_highest_note || entry < 0 || entry >= static_cast<int>(use.size()))
						continue;

					auto& u = use[entry];
					u.lowest = u.notes == 0 ? note : std::min(u.lowest, note);
					u.highest = std::max(u.highest, note);
					++u.notes;
					heard[static_cast<size_t>(entry) * channels + c] = 1;
				}
			}
		}

		for (auto i = 0_z; i < use.size(); ++i)
		{
			const auto first = heard.begin() + static_cast<std::ptrdiff_t>(i * channels);
			use[i].channels = static_cast<int>(std::count(first, first + channels, uint8_t{1}));
		}

		return use;
	}

	// Which channels have nothing - no note, instrument, volume or effect - in any pattern the module plays.
	std::vector<bool> read_empty_channels(const openmpt::module& module)
	{
		const auto channels = std::max(0, module.get_num_channels());
		std::vector<bool> empty(static_cast<size_t>(channels), true);
		std::set<int> patterns;
		int64_t cells = 0;

		for (auto o = 0; o < module.get_num_orders(); ++o)
		{
			// The order list plays some patterns more than once.
			const auto pattern = module.get_order_pattern(o);
			if (!patterns.insert(pattern).second) continue;

			const auto rows = module.get_pattern_num_rows(pattern);
			cells += static_cast<int64_t>(rows) * channels;
			if (cells > max_use_cells) return {};

			for (auto r = 0; r < rows; ++r)
			{
				for (auto c = 0; c < channels; ++c)
				{
					if (!empty[c]) continue;

					for (const auto command : {openmpt::module::command_note, openmpt::module::command_instrument,
					                           openmpt::module::command_volumeffect, openmpt::module::command_effect})
					{
						if (module.get_pattern_row_channel_command(pattern, r, c, command) != 0)
						{
							empty[c] = false;
							break;
						}
					}
				}
			}
		}

		return empty;
	}

	// The order nearest `from`, stepping by `step`, that plays a pattern: past the order list's skip markers, and
	// none past its end or a stop marker. -1 for none.
	int playable_order(const openmpt::module& module, int from, const int step)
	{
		constexpr auto skip_marker = 0xFFFE;

		for (; from >= 0 && from < module.get_num_orders(); from += step)
		{
			const auto pattern = module.get_order_pattern(from);
			if (pattern == skip_marker) continue;
			return module.get_pattern_num_rows(pattern) > 0 ? from : -1;
		}

		return -1;
	}
}

class tracker_decoder
{
public:
	std::ostringstream log;
	openmpt::module_ext module;
	openmpt::ext::interactive* interactive;

	explicit tracker_decoder(const core::blob& bytes) :
		module(bytes.data(), bytes.size(), log,
	           {{"load.skip_plugins", "1"}, {"dither", "0"}, {"seek.sync_samples", "1"}, {"play.at_end", "stop"}}),
		interactive(static_cast<openmpt::ext::interactive*>(module.get_interface(openmpt::ext::interactive_id)))
	{
		module.set_repeat_count(0);
	}
};

bool is_tracker_extension(std::string_view extension)
{
	if (!extension.empty() && extension.front() == '.') extension.remove_prefix(1);
	static const auto extensions = openmpt::get_supported_extensions();
	return std::ranges::any_of(extensions,
	                           [&](const std::string& candidate) { return core::icmp(extension, candidate) == 0; });
}

tracker_tune_render::tracker_tune_render(core::file_path path, platform::file_ptr file) :
	_path(path), _file(std::move(file))
{
}

tracker_tune_render::~tracker_tune_render() = default;

bool tracker_tune_render::load()
{
	core::exclusive_lock render_lock(_render_mutex);
	if (_loaded) return _ok;
	_loaded = true;

	core::blob bytes;
	if (_file)
	{
		const auto size = _file->size();
		if (size > 0 && size < max_module_bytes)
		{
			bytes.resize(static_cast<size_t>(size));
			_file->seek(0, platform::file::whence::begin);
			size_t done = 0;
			while (done < bytes.size())
			{
				const auto n = _file->read(bytes.data() + done, bytes.size() - done);
				if (n == 0) break;
				done += static_cast<size_t>(n);
			}
			if (done != bytes.size()) bytes.clear();
		}
		_file.reset();
	}
	else
	{
		bytes = platform::blob_head_from_file(_path, max_module_bytes);
	}

	if (bytes.empty() || bytes.size() >= max_module_bytes)
	{
		core::log(__FUNCTION__, std::format("could not read tracker module, or it exceeds 64 MiB: {}", _path.name()));
		return false;
	}

	try
	{
		auto decoder = std::make_unique<tracker_decoder>(bytes);
		auto& module = decoder->module;
		auto details = std::make_shared<tracker_tune_details>();
		details->title = module.get_metadata("title");
		details->artist = module.get_metadata("artist");
		details->type = module.get_metadata("type_long");
		details->tracker = module.get_metadata("tracker");
		// The song message alone. "message" stands the sample and instrument names in for a module that has
		// none, and those are listed under the monitor already, so they would show twice.
		details->message = module.get_metadata("message_raw");
		details->date = module.get_metadata("date");
		details->duration = module.get_duration_seconds();
		details->channels = module.get_channel_names();
		details->empty_channels = read_empty_channels(module);
		details->samples = module.get_sample_names();
		details->instruments = module.get_instrument_names();
		details->patterns = module.get_num_patterns();
		for (auto i = 0; i < module.get_num_orders(); ++i) details->orders.emplace_back(module.get_order_pattern(i));

		for (auto p = 0; p < details->patterns && details->cell.empty(); ++p)
		{
			if (module.get_pattern_num_rows(p) > 0) details->cell = module.format_pattern_row_channel(p, 0, 0);
		}

		if (module.get_num_instruments() > 0)
			details->instrument_use = read_bank_use(module, module.get_num_instruments());
		else details->sample_use = read_bank_use(module, module.get_num_samples());

		if (!std::isfinite(details->duration) || details->duration <= 0 ||
		    details->duration * tracker_sample_rate > (UINT32_MAX - 36) / (2 * sizeof(float)) ||
		    details->channels.empty() || !decoder->interactive)
		{
			core::log(__FUNCTION__,
			          std::format("tracker module has no finite playable timeline or channel interface: {}",
			                      _path.name()));
			return false;
		}

		_pcm.resize(static_cast<size_t>(cache_frames) * 2);
		_decoder = std::move(decoder);
		{
			core::exclusive_lock data_lock(_data_mutex);
			_details = std::move(details);
		}
		_ok = true;
		return true;
	}
	catch (const openmpt::exception& e)
	{
		core::log(__FUNCTION__, std::format("could not load {}: {}", _path.name(), e.what()));
		return false;
	}
}

uint64_t tracker_tune_render::sample_count() const
{
	const auto d = details();
	return d ? static_cast<uint64_t>(std::ceil(d->duration * tracker_sample_rate)) : 0;
}

std::shared_ptr<const tracker_tune_details> tracker_tune_render::details() const
{
	core::shared_lock data_lock(_data_mutex);
	return _details;
}

void tracker_tune_render::mix(const tracker_channel_mix& mix)
{
	core::exclusive_lock data_lock(_data_mutex);
	_mix = mix;
}

size_t tracker_tune_render::read(const uint64_t first, float* out, const size_t count)
{
	if (!load()) return 0;
	core::exclusive_lock render_lock(_render_mutex);
	if (!_ok) return 0;
	const auto total = sample_count();
	if (first >= total || count == 0) return 0;
	const auto end = first + std::min<uint64_t>(count, total - first);

	try
	{
		tracker_channel_mix mix;
		{
			core::shared_lock data_lock(_data_mutex);
			mix = _mix;
		}
		if (mix != _applied_mix)
		{
			for (auto c = 0; c < _decoder->module.get_num_channels(); ++c)
				_decoder->interactive->set_channel_mute_status(c, mix.is_silent(c));
			_applied_mix = std::move(mix);
			_cache_start = _next_sample;
		}

		if (first < _cache_start || first > _next_sample)
		{
			place(first);
			_cache_start = _next_sample = first;
			_pattern.reset();
			_order = -1;
			core::exclusive_lock data_lock(_data_mutex);
			_frames.clear();
			++_revision;
		}

		auto copied = size_t{0};
		while (first + copied < end)
		{
			const auto at = first + copied;
			if (at < _next_sample)
			{
				const auto n = static_cast<size_t>(std::min(_next_sample - at, end - at));
				for (size_t i = 0; i < n; ++i)
					for (auto c = 0; c < 2; ++c) out[(copied + i) * 2 + c] = _pcm[((at + i) % cache_frames) * 2 + c];
				copied += n;
				continue;
			}

			const auto wanted = static_cast<size_t>(std::min<uint64_t>(snapshot_frames, end - at));
			const auto got = _decoder->module.read_interleaved_stereo(tracker_sample_rate, wanted, out + copied * 2);
			if (got == 0) break;
			for (size_t i = 0; i < got; ++i)
				for (auto c = 0; c < 2; ++c) _pcm[((at + i) % cache_frames) * 2 + c] = out[(copied + i) * 2 + c];
			_next_sample += got;
			_cache_start = std::max(_cache_start, _next_sample > cache_frames ? _next_sample - cache_frames : 0);
			publish_frame(at, _next_sample);
			copied += got;
		}
		return copied;
	}
	catch (const openmpt::exception& e)
	{
		_ok = false;
		core::log(__FUNCTION__, std::format("tracker playback failed for {}: {}", _path.name(), e.what()));
		return 0;
	}
}

void tracker_tune_render::place(const uint64_t first)
{
	auto& module = _decoder->module;
	const auto target = static_cast<double>(first) / tracker_sample_rate;

	// libopenmpt places a module at the start of a row: one at or before the moment is found, stepping further back
	// while the row found starts after it, and the module is played on from there to the moment unheard, so every
	// sample, and the row it shows, is where the timeline says.
	auto back = 0.0;
	auto placed = module.set_position_seconds(target);

	while (placed > target && target - back > 0.0)
	{
		back = back > 0.0 ? back * 2.0 : 0.25;
		placed = module.set_position_seconds(std::max(0.0, target - back));
	}

	auto skip = static_cast<uint64_t>(
		std::llround(std::clamp(target - placed, 0.0, max_place_seconds) * tracker_sample_rate));
	std::vector<float> unheard(snapshot_frames * 2);

	while (skip > 0)
	{
		const auto got = module.read_interleaved_stereo(tracker_sample_rate,
		                                                static_cast<size_t>(std::min<uint64_t>(skip, snapshot_frames)),
		                                                unheard.data());
		if (got == 0) break;
		skip -= got;
	}
}

std::shared_ptr<const tracker_pattern> tracker_tune_render::formatted_pattern(const int index)
{
	if (const auto found = _formatted.find(index); found != _formatted.end()) return found->second;

	auto& module = _decoder->module;
	if (index < 0 || index >= module.get_num_patterns()) return {};

	auto pattern = std::make_shared<tracker_pattern>();
	pattern->index = index;
	pattern->rows = module.get_pattern_num_rows(index);
	pattern->channels = module.get_num_channels();
	pattern->cells.reserve(static_cast<size_t>(pattern->rows) * pattern->channels);
	for (auto r = 0; r < pattern->rows; ++r)
		for (auto c = 0; c < pattern->channels; ++c)
			pattern->cells.push_back({
				module.format_pattern_row_channel(index, r, c),
				module.highlight_pattern_row_channel(index, r, c),
				module.get_pattern_row_channel_command(index, r, c, openmpt::module::command_instrument),
			});

	_formatted.emplace(index, pattern);
	return pattern;
}

void tracker_tune_render::publish_frame(const uint64_t first, const uint64_t end)
{
	auto& module = _decoder->module;
	const auto order = module.get_current_order();
	const auto index = module.get_current_pattern();

	if (order != _order || !_pattern || _pattern->index != index)
	{
		// Played on into this order, what came before is the pattern left and the row it was left at; placed here
		// by a seek, it is what the order list has before.
		if (_order >= 0 && _pattern)
		{
			_previous_pattern = _pattern;
			_previous_row = _row;
		}
		else
		{
			const auto before = playable_order(module, order - 1, -1);
			_previous_pattern = before >= 0 ? formatted_pattern(module.get_order_pattern(before)) : nullptr;
			_previous_row = _previous_pattern ? _previous_pattern->rows - 1 : 0;
		}

		const auto after = playable_order(module, order + 1, 1);
		_pattern = formatted_pattern(index);
		_next_pattern = after >= 0 ? formatted_pattern(module.get_order_pattern(after)) : nullptr;
		_order = order;

		// Only the patterns about the playhead stay formatted.
		std::erase_if(_formatted, [this](const auto& entry)
		{ return entry.second != _pattern && entry.second != _previous_pattern && entry.second != _next_pattern; });
	}

	tracker_frame_state state;
	state.order = order;
	state.row = _row = module.get_current_row();
	state.speed = module.get_current_speed();
	state.tempo = module.get_current_tempo2();
	state.pattern = _pattern;
	state.previous_pattern = _previous_pattern;
	state.previous_row = _previous_row;
	state.next_pattern = _next_pattern;
	state.levels.reserve(static_cast<size_t>(module.get_num_channels()));
	for (auto c = 0; c < module.get_num_channels(); ++c)
		state.levels.emplace_back(module.get_current_channel_vu_mono(c));

	core::exclusive_lock data_lock(_data_mutex);
	_frames.push_back({first, end, std::move(state)});
	++_revision;

	// Let go of only what played longer ago than the cache holds. A change of mix sets the cache's start past what was
	// rendered before it, which is still to be heard and still looks as it did.
	const auto oldest = _next_sample > cache_frames ? _next_sample - cache_frames : 0;
	while (!_frames.empty() && _frames.front().end < oldest) _frames.pop_front();
}

bool tracker_tune_render::frame_at(const double seconds, tracker_frame_state& out) const
{
	if (!std::isfinite(seconds) || seconds < 0) return false;
	const auto sample = static_cast<uint64_t>(seconds * tracker_sample_rate);
	core::shared_lock data_lock(_data_mutex);
	const auto next = std::ranges::upper_bound(_frames, sample, {}, &snapshot::first);
	if (next == _frames.begin()) return false;
	const auto at = std::prev(next);
	if (sample >= at->end) return false;
	out = at->state;

	// How far through its row the moment is: from the first frame of the row to the first of the next, or, while
	// that is still to be rendered, for as long as the row lasts at its speed and tempo - a tick is 2.5 / tempo
	// seconds.
	const auto same_row = [&at](const snapshot& s)
	{
		return s.state.order == at->state.order && s.state.row == at->state.row;
	};
	auto start = at;
	while (start != _frames.begin() && same_row(*std::prev(start))) --start;
	auto stop = next;
	while (stop != _frames.end() && same_row(*stop)) ++stop;

	const auto row_start = static_cast<double>(start->first);
	const auto row_samples = stop != _frames.end() ? static_cast<double>(stop->first) - row_start
	                         : out.tempo > 0.0     ? out.speed * 2.5 / out.tempo * tracker_sample_rate
	                                               : 0.0;
	out.row_progress = row_samples > 0.0 ? std::clamp((static_cast<double>(sample) - row_start) / row_samples, 0.0, 1.0)
	                                     : 0.0;
	return true;
}

uint64_t tracker_tune_render::revision() const
{
	core::shared_lock data_lock(_data_mutex);
	return _revision;
}

void tracker_tune_render::describe(av_media_info& info) const
{
	const auto d = details();
	if (!d) return;
	info.audio_codec = core::cache(d->type);
	info.bitrate = 0;
	info.metadata.clear();
	metadata_kv_list tags;
	if (!d->title.empty()) tags.emplace_back("title", d->title);
	if (!d->artist.empty()) tags.emplace_back("artist", d->artist);
	if (!d->tracker.empty()) tags.emplace_back("encoder", d->tracker);
	if (!d->message.empty()) tags.emplace_back("comment", d->message);
	if (!d->date.empty()) tags.emplace_back("date", d->date);
	tags.emplace_back("type", d->type);
	info.metadata.emplace_back(metadata_standard::ffmpeg, std::move(tags));
	for (auto& stream : info.streams)
		if (stream.type == av_stream_type::audio) stream.codec = d->type;
}
