// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Media playback session and player management. Coordinates audio/video decoding threads,
// handles play/pause/seek operations, and synchronizes audio-video timing.

#pragma once

#include "files.h"
#include "model_items.h"
#include "av_format.h"
#include "av_host.h"
#include "av_sound.h"
#include "av_visualizer.h"

enum class av_play_state
{
	closed,
	playing,
	paused,
	detached,
};

enum class render_valid
{
	invalid,
	present,
	valid
};

struct av_texture_upload_state
{
	double time = -1;
	const ui::texture* destination = nullptr;
};

inline bool should_upload_video_frame(const av_texture_upload_state& state, const ui::texture* const destination,
                                      const bool destination_valid, const double time)
{
	return destination && (!destination_valid || state.destination != destination || !is_equal(state.time, time));
}

// design.md: resume "applies to media longer than ten seconds only when the saved position is more
// than two seconds from the start and five seconds from the end, avoiding a surprising resume for
// barely started or effectively completed media".
inline bool should_resume_at(const double start_time, const double end_time, const double saved_position)
{
	return end_time - start_time > 10.0 &&
		saved_position > start_time + 2.0 &&
		saved_position < end_time - 5.0;
}


// design.md: closing "saves the last presented position ...; if a seek or resume is still
// synchronizing, it saves the accepted target instead, so closing before the first resumed frame
// arrives does not replace that position with zero".
inline double position_to_save(const bool is_synchronizing, const double accepted_seek,
                               const double last_frame_time)
{
	return is_synchronizing ? accepted_seek : last_frame_time;
}

inline bool should_drain_audio_while_output_unavailable(const bool has_audio, const bool output_available)
{
	return has_audio && !output_available;
}

enum class video_queue_front_action
{
	present,
	discard,
	mark_eof
};

inline video_queue_front_action classify_video_queue_front(const int frame_gen, const int seek_gen, const bool eof)
{
	if (frame_gen != seek_gen) return video_queue_front_action::discard;
	return eof ? video_queue_front_action::mark_eof : video_queue_front_action::present;
}

inline bool should_finish_video_settle(const bool has_current_frame_candidate, const bool reached,
                                       const double best_frame_time, const double target_time,
                                       const bool current_eof_handled)
{
	return current_eof_handled || (has_current_frame_candidate && (reached || best_frame_time >= target_time));
}

// A scrub request near the position already asked for is merged rather than sought: the demuxer and
// the decoder stay where the earlier request put them, and seek() only moves the target the
// presenter settles onto. Frames ahead of the one shown are usually decoded already, so a merged step
// forward lands with no decoder restart at all; a step behind frames already shown is caught by
// should_correct_settled_seek, which asks for one real seek. Outside a scrub, only a request that
// arrives before the previous one has settled is merged.
inline bool should_coalesce_seek_request(const double last_seek, const double pos, const bool pending_time_sync,
                                         const bool was_scrubbing, const bool scrubbing, const bool force = false)
{
	if (force) return false;
	const auto near_existing = fabs(last_seek - pos) <= 0.1 && pos >= 0.1;
	return near_existing && (pending_time_sync || (was_scrubbing && scrubbing));
}

// A merged seek moves the target without moving the decoder, and the presenter only walks forward,
// so a target behind the frame it has already reached cannot be shown from this generation. When the
// settled frame is past the target by more than half a frame, a nearer frame exists that only a real
// seek can decode. Asked once per target: one before the stream's first key frame lands after it
// however often it is sought.
inline bool should_correct_settled_seek(const bool scrubbing, const bool has_current_frame, const double frame_time,
                                        const double target, const double frame_interval,
                                        const double corrected_target)
{
	if (!scrubbing || !has_current_frame || frame_interval <= 0.0) return false;
	return frame_time - target > frame_interval / 2.0 && !df::equiv(corrected_target, target);
}

// How many sessions the player will decode frames for beside the one it is playing. Two, because
// the only caller is Movie's preview and a crossfade mixes exactly two clips. docs/movie.md owns
// why the preview needs them and why it cannot pump them itself.
inline constexpr size_t av_max_frame_sessions = 2;

class av_session final : public std::enable_shared_from_this<av_session>
{
	av_visualizer _visualizer;

	// Read by the audio thread and the UI thread, written when the session opens and closes.
	// Atomic so the null check and the dereference cannot straddle a close.
	std::atomic<file_type_ref> _mt = nullptr;

	av_packet_queue _audio_packets;
	av_packet_queue _video_packets;

	av_frame_queue _audio_frames;
	av_frame_queue _video_frames;

	mutable platform::mutex _decoder_rw;
	_Guarded_by_(_decoder_rw) av_format_decoder _decoder;

	// Cached copies of the decoder's stream presence, set under _decoder_rw when the
	// session opens. The decode/audio/UI threads read these on hot paths without taking
	// the decoder lock, so they must be atomic. A session opens exactly once, so these
	// never change after open() publishes them.
	std::atomic<bool> _has_video = false;
	std::atomic<bool> _has_audio = false;

	// Open-time snapshots of everything a non-decode thread needs to know about the decoder.
	// The decoder itself is only safe to touch under _decoder_rw from the read and decode
	// threads - close() frees the format context and clears the stream table - so the UI reads
	// these instead. Published once by open() and never changed afterwards, including by close().
	std::atomic<int> _video_stream_id = -1;
	std::atomic<int> _audio_stream_id = -1;
	std::atomic<std::shared_ptr<const av_media_info>> _info;

	std::atomic<double> _end_time = 0;
	std::atomic<double> _last_frame_decoded = 0;
	std::atomic<double> _start_time = 0;

	std::atomic<int> _seek_gen = 1;


	const int max_loop_iteration = 256;

	// A gap between presents longer than this is a stall - the process suspended, the machine
	// slept, a long modal operation - not playback. Well above the 200ms idle present rate.
	static constexpr double present_stall_seconds = 1.0;

	// Read by the audio, video and read threads and written from the read thread
	// (and, on device loss, the audio thread); atomic so a pause/seek is seen
	// promptly and consistently rather than via a benign-but-fragile data race.
	std::atomic<av_play_state> _state = av_play_state::detached;

	std::atomic<bool> _scrubbing = false;

	// Shared timing state: written by the audio/read/decode threads and read by the UI
	// thread (pos(), times(), last_frame_time()). Atomic to avoid torn reads of the
	// double and to publish updates promptly across threads.
	std::atomic<double> _last_seek = 0.0;
	std::atomic<double> _time_offset = 0.0;

	std::atomic<bool> _reset_time_offset = false;
	std::atomic<bool> _pending_time_sync = false;
	std::atomic<bool> _settling = false;

	// A seek another thread needs but cannot make: the demuxer belongs to the read thread, which
	// performs it from process_pending_forced_seek. The audio thread asks after reopening a lost
	// endpoint; the presenter asks when a merged seek left its target behind the frames still to come.
	mutable platform::mutex _forced_seek_mutex;
	_Guarded_by_(_forced_seek_mutex) double _forced_seek = -1.0;
	_Guarded_by_(_forced_seek_mutex) int _forced_seek_gen = 0;
	// A settle correction is for one target. If a later merged seek has moved the target by the time
	// the read thread gets to it, the presenter judges the new target itself.
	_Guarded_by_(_forced_seek_mutex) bool _forced_seek_is_correction = false;

	// Set once the audio stream's end has been handled (decoder tail drained and a
	// silence pad queued). has_ended() waits for this for audio sessions so the wall
	// clock - which races ahead when the device under-runs at the end - cannot fire
	// end-of-stream early and seek away before the silence tail is produced (which
	// left the WASAPI ring to loop its last buffer). Re-armed on every open/seek.
	std::atomic<bool> _audio_eof_handled = false;

	// Set once the video stream's end-of-stream marker has been consumed. Re-armed on every
	// open/seek. Without audio there is no clock but the wall clock, which a slow decode lets
	// run past _end_time while frames are still queued - and which an unknown container
	// duration leaves at zero from the first frame. has_ended() waits for this instead.
	std::atomic<bool> _video_eof_handled = false;

	// Set by the audio thread when the endpoint could not be opened. The audio device is the
	// master clock, so without one the session has to fall back to the wall clock exactly as a
	// session with no audio track does - otherwise pos() sits on the sought position for the
	// whole clip and a slideshow never moves past it.
	std::atomic<bool> _audio_unavailable = false;

	// _audio_data_end: absolute time of the last real audio sample (captured when the
	// EOF is handled, after the decoder tail is drained). _audio_clock: the audio
	// device's current play position (base_time + device clock), updated each audio
	// iteration. has_ended() ends the clip when the device has actually played out to
	// _audio_data_end rather than on the wall clock, which drifts ahead of the device.
	std::atomic<double> _audio_data_end = 0.0;
	std::atomic<double> _audio_clock = 0.0;

	std::atomic<int> _volume = 1000;
	std::atomic<bool> _mute = false;

	// Cleared by a caller whose playback is not the user viewing the file. Movie plays a clip to
	// judge a trim, and without this every scrub would rewrite the resume position the user set by
	// actually watching it.
	std::atomic<bool> _remembers_position = true;

	mutable platform::mutex _presentation_mutex;
	df::file_path _path;

	// Read by the UI thread (pos(), last_frame_time()) and written by the read/decode
	// threads (update_texture/update_visualizer); atomic to avoid a torn read.
	mutable std::atomic<double> _last_frame_time = -1;
	// Written by update_texture under _presentation_mutex. The destination is part of the upload
	// identity: after device loss the same frame must fill the replacement texture.
	_Guarded_by_(_presentation_mutex) mutable av_texture_upload_state _last_texture;

	// Wall clock reading at the previous present, used only to spot a gap no playback could
	// explain. UI thread only, under _presentation_mutex.
	_Guarded_by_(_presentation_mutex) double _last_present_time = 0.0;

	// The target a settle correction was last asked for, so a target no seek can reach is asked for
	// once rather than on every present. Cleared as soon as the target moves. Under _presentation_mutex.
	_Guarded_by_(_presentation_mutex) double _correction_target = -1.0;

	_Guarded_by_(_presentation_mutex)
	av_frame_ptr _frame;

	std::atomic<std::shared_ptr<audio_resampler>> _playback_resampler;
	std::atomic<std::shared_ptr<audio_resampler>> _vis_resampler;

	av_host& _host;
	std::function<void(df::file_path, double)> _save_media_position;

public:
	av_session(av_host& host, std::function<void(df::file_path, double)> save_media_position) :
		_host(host), _save_media_position(std::move(save_media_position))
	{
	}

	~av_session()
	{
		close(false);
	}

	int video_stream_id() const
	{
		return _video_stream_id;
	}

	int audio_stream_id() const
	{
		return _audio_stream_id;
	}

	// True when the audio device is driving the clock. A session whose endpoint could not be
	// opened has an audio track but no device clock, and must be timed like a silent one.
	bool has_audio_clock() const
	{
		return _has_audio && !_audio_unavailable;
	}

	void mark_audio_output_unavailable(const double time_now = df::now())
	{
		if (_audio_unavailable.exchange(true)) return;
		if (_pending_time_sync.exchange(false))
		{
			_time_offset = time_now - _last_seek;
		}
	}

	void request_audio_recovery_seek(const double time_now = df::now())
	{
		const auto gen = _seek_gen.load();
		const auto recovery_pos = pos(time_now);
		platform::exclusive_lock lock(_forced_seek_mutex);
		_forced_seek_gen = gen;
		_forced_seek = std::max(0.0, recovery_pos);
		_forced_seek_is_correction = false;
	}

	bool process_pending_forced_seek()
	{
		double target = -1.0;
		int gen = 0;
		bool is_correction = false;

		{
			platform::exclusive_lock lock(_forced_seek_mutex);
			target = _forced_seek;
			gen = _forced_seek_gen;
			is_correction = _forced_seek_is_correction;
			_forced_seek = -1.0;
		}

		if (target < 0.0) return false;
		if (gen != _seek_gen) return false;
		if (is_correction && !df::equiv(target, _last_seek)) return false;
		seek(target, _scrubbing, true);
		return true;
	}

	// Seeks that restarted the decoder. A merged seek does not count, which is what makes it cheap.
	int seek_generation() const
	{
		return _seek_gen;
	}

	// True when this media is presented as an audio visualisation rather than a video texture.
	bool visualize_audio() const
	{
		const auto mt = _mt.load();
		return mt != nullptr && (mt->group->traits && file_traits::visualize_audio);
	}

	double pos(const double time_now) const
	{
		// Read order matters: the audio thread publishes _time_offset before clearing
		// _pending_time_sync, and seek() publishes _last_seek before setting it, so testing the
		// flag first and reading the value second never pairs a new flag with a stale value.
		if (_scrubbing || _pending_time_sync) return _last_seek;
		if (_state != av_play_state::playing) return _last_frame_time;
		return time_now - _time_offset;
	}

	// Raises the known media end without losing a concurrent raise; both the audio thread and
	// the UI thread extend it, and has_ended() reads the result.
	void raise_end_time(const double t)
	{
		auto current = _end_time.load();
		while (current < t && !_end_time.compare_exchange_weak(current, t))
		{
		}
	}

	void adjust_volume()
	{
		_volume = _scrubbing || _mute.load() ? 0 : setting.media_volume;
	}

	bool is_open() const
	{
		return _state == av_play_state::playing || _state == av_play_state::paused;
	}

	// False for playback that stands for something other than the user viewing this file, so
	// closing it leaves their resume position alone. See _remembers_position.
	void remembers_position(const bool x)
	{
		_remembers_position = x;
	}

	bool is_playing() const
	{
		return _state == av_play_state::playing;
	}

	double last_frame_time() const
	{
		return _last_frame_time;
	}

	void state(av_play_state new_state);

	friend class av_player;

	av_media_info info() const
	{
		const auto snapshot = _info.load();
		return snapshot ? *snapshot : av_media_info{};
	}

	// Caller must hold _decoder_rw exclusively; only open() does.
	void create_resampler()
	{
		if (!_playback_resampler.load())
		{
			_playback_resampler = _decoder.make_audio_resampler();
		}

		if (!_vis_resampler.load())
		{
			_vis_resampler = _decoder.make_audio_resampler();
		}
	}

	bool open(const df::file_path path, const file_type_ref file_type, const double starting_position,
	          const bool auto_play, const int video_track, const int audio_track, const bool can_use_hw,
	          const bool use_last_played_pos, const bool can_use_threads, const bool video_only = false,
	          const platform::file_ptr& file = {})
	{
		df::assert_true(_state == av_play_state::detached);

		platform::exclusive_lock lock_dec(_decoder_rw);

		// A handle handed over by the write that just replaced this file. Opening the path again is
		// the read-after-write the hand-over exists to avoid.
		const auto result = file
			                    ? _decoder.open(file, path, media_intent::playback)
			                    : _decoder.open(path, media_intent::playback);

		if (result)
		{
			// video_only is not an optimisation. process_io stops reading when *either* packet queue
			// fills, so a session whose audio nothing drains stalls its own video -- which is exactly
			// what a caller that wants frames and supplies its own sound would otherwise get.
			_decoder.init_streams(video_track, audio_track, can_use_hw, video_only, can_use_threads);

			_has_video = _decoder.has_video();
			_has_audio = _decoder.has_audio();
			_video_stream_id = _decoder.video_stream_id();
			_audio_stream_id = _decoder.audio_stream_id();
			_info.store(std::make_shared<const av_media_info>(_decoder.info()));

			const auto start_time = _decoder.start_time();
			const auto end_time = _decoder.end_time();

			_end_time = end_time;
			{
				platform::exclusive_lock lock_present(_presentation_mutex);
				_frame.reset();
			}
			_path = path;
			_last_frame_time = 0;
			_last_seek = 0;
			_mt = file_type;
			_scrubbing = false;
			_start_time = start_time;
			_state = auto_play ? av_play_state::playing : av_play_state::paused;
			_time_offset = df::now();
			_pending_time_sync = true;
			_reset_time_offset = !_has_audio; // && !scrubbing;
			_settling = true;
			_audio_eof_handled = false;
			_video_eof_handled = false;
			_audio_unavailable = false;
			_seek_gen = 1;

			_playback_resampler.store(nullptr);
			_vis_resampler.store(nullptr);

			create_resampler();

			if (use_last_played_pos)
			{
				if (should_resume_at(start_time, end_time, starting_position))
				{
					_decoder.seek(starting_position);
					_last_seek = starting_position;
				}
			}
		}

		_video_packets.clear();
		_audio_packets.clear();
		_video_frames.clear();
		_audio_frames.clear();

		return result;
	}

	void close(const bool save_position = true)
	{
		const auto position = position_to_save(_scrubbing || _pending_time_sync, _last_seek.load(),
		                                       _last_frame_time.load());

		// Single-shot. A queued close and the destructor can both reach here, so the exchange -
		// not a separate load - is what makes the position save and the decoder teardown happen
		// exactly once.
		const auto previous = _state.exchange(av_play_state::closed);

		if (previous == av_play_state::closed) return;

		// A session that never opened has nothing on screen to invalidate.
		if (previous != av_play_state::detached)
		{
			_host.invalidate_view(view_invalid::view_layout |
				view_invalid::screen_saver |
				view_invalid::app_layout |
				view_invalid::media_elements |
				view_invalid::command_state);
		}

		platform::exclusive_lock lock(_decoder_rw);
		const auto path = _path;

		{
			platform::exclusive_lock lock_present(_presentation_mutex);
			_frame.reset();
		}

		_mt = nullptr;
		_path = {};

		if (save_position && _remembers_position && !path.is_empty() && _save_media_position)
		{
			_save_media_position(path, position);
		}

		_decoder.close();
		_audio_packets.clear();
		_end_time = 0;
		_last_frame_decoded = 0;
		_start_time = 0;
		_video_packets.clear();
		_video_frames.clear();
		_audio_frames.clear();
		_playback_resampler.store(nullptr);
		_vis_resampler.store(nullptr);
	}

	void seek(double pos, bool scrubbing, bool force = false);

	void process_video(const platform::thread_event& _read_event)
	{
		auto loop_iteration = 0;

		if (_has_video)
		{
			while (_video_frames.should_receive())
			{
				{
					platform::shared_lock lock_dec(_decoder_rw);
					_decoder.receive_frames(_video_packets, _video_frames);
				}

				if (_video_packets.should_receive())
				{
					_read_event.set();
					break;
				}

				if (_state == av_play_state::closed || df::is_closing || ++loop_iteration > max_loop_iteration)
				{
					break;
				}
			}
		}
	}

	void process_audio(audio_buffer& playback_buffer, audio_buffer& vis_buffer,
	                   const platform::thread_event& read_event)
	{
		auto loop_iteration = 0;

		if (_has_audio)
		{
			while (_audio_frames.should_receive() || playback_buffer.should_fill() || _seek_gen != playback_buffer.
				generation())
			{
				if (_audio_frames.should_receive())
				{
					platform::shared_lock lock(_decoder_rw);
					_decoder.receive_frames(_audio_packets, _audio_frames);
				}

				if (playback_buffer.should_fill() || _seek_gen != playback_buffer.generation())
				{
					av_frame_ptr frame;

					if (_audio_frames.pop(frame))
					{
						const auto sv = av_seek_gen_from_frame(frame);
						const auto is_current = sv == _seek_gen;
						const auto eof = av_frame_is_eof(frame);

						// A frame decoded for a position we have already seeked away from is
						// dropped, EOF markers included. Honouring a stale EOF would queue the
						// silence tail and arm _audio_eof_handled for a stream that has not
						// ended, letting has_ended() end the clip right after a seek near the end.
						if (is_current && eof)
						{
							// End of the audio stream: recover any tail the resamplers
							// still hold, fade the very end so a clip ending on a non-zero
							// sample does not click, then pad with silence. Do this once
							// per end-of-stream (process_io keeps re-issuing EOF markers as
							// the queue drains; appending a blank second for each would grow
							// _end_time without bound and the clip would never end).
							if (!_audio_eof_handled)
							{
								const auto pr = _playback_resampler.load();
								const auto vr = _vis_resampler.load();

								if (pr) pr->drain(playback_buffer, _seek_gen);
								if (vr) vr->drain(vis_buffer, _seek_gen);

								// The real audio ends here (after the decoder tail is
								// recovered, before the silence pad). has_ended() ends the
								// clip once the device has played out to this point.
								_audio_data_end = playback_buffer.end_time();

								playback_buffer.apply_fade_out(0.005);

								playback_buffer.append_blank_second();
								vis_buffer.append_blank_second();

								// Tell has_ended() the tail is now queued so it may end the
								// clip. Set last so end_time below reflects the appended pad.
								_audio_eof_handled = true;
							}
						}
						else if (is_current)
						{
							const auto pr = _playback_resampler.load();
							const auto vr = _vis_resampler.load();

							// A seek can only land on a key sample, which may be a long way before
							// the position asked for - the whole clip when a file carries a single
							// key frame. update_for_present settles the video forward onto the
							// target; audio has no such step, so drop the frames that end before
							// it. Without this the buffer starts at the key sample, the device
							// clock anchors there, and the view sits frozen on the settled frame
							// until the clock catches back up.
							const auto epoch_started = playback_buffer.generation() == sv;
							const auto frame_end = av_time_from_frame(frame) + av_audio_frame_duration(frame);
							const auto before_target = !epoch_started && frame_end <= _last_seek;

							if (pr && vr && !before_target)
							{
								if (sv != playback_buffer.generation())
								{
									pr->flush();
									vr->flush();

									df::trace(std::format("Player clear audio_buffer on seek_ver {}", sv));
								}

								// Volume above 100% cannot be delivered by the device
								// (its volume is 0.0-1.0), so boost it here in software.
								// The device handles 0-100% attenuation; the 100%..200%
								// setting range maps onto a 1x..media_volume_boost_gain
								// software gain so the boost is strong enough to lift very
								// quiet sources (clamping in apply_audio_gain stops louder
								// material distorting). 1.0 = no boost for the normal range.
								const auto boost = std::clamp(_volume.load(), 1000, media_volume_boost);
								pr->set_gain(1.0 + (boost - 1000) / 1000.0 * (media_volume_boost_gain - 1.0));

								pr->resample(frame, playback_buffer);
								vr->resample(frame, vis_buffer);
							}

							// Keep _end_time at the media duration: do NOT let the appended
							// silence pad push it out, or has_ended() (which waits for the
							// media end to pass) would hold the last frame for the pad's length.
							raise_end_time(playback_buffer.end_time());
						}
					}
				}

				if (_audio_packets.should_receive())
				{
					read_event.set();
					break;
				}

				if (_state == av_play_state::closed || df::is_closing || ++loop_iteration > max_loop_iteration)
				{
					break;
				}
			}
		}
	}

	void discard_audio_while_output_unavailable(const platform::thread_event& read_event)
	{
		auto loop_iteration = 0;

		if (should_drain_audio_while_output_unavailable(_has_audio, false))
		{
			while (_audio_frames.should_receive())
			{
				{
					platform::shared_lock lock(_decoder_rw);
					_decoder.receive_frames(_audio_packets, _audio_frames);
				}

				av_frame_ptr frame;

				while (_audio_frames.pop(frame))
				{
					const auto is_current = av_seek_gen_from_frame(frame) == _seek_gen;
					if (is_current && av_frame_is_eof(frame)) _audio_eof_handled = true;
				}

				if (_audio_packets.should_receive())
				{
					read_event.set();
				}

				if (_state == av_play_state::closed || df::is_closing || ++loop_iteration > max_loop_iteration)
				{
					break;
				}
			}
		}
	}

	void process_io(const platform::thread_event& video_event, const platform::thread_event& audio_event);

	bool has_ended(const double time_now) const
	{
		if (!is_playing()) return false;

		// For audio sessions, do not declare the end until the audio stream's EOF has
		// actually been handled (tail drained + silence pad queued). The master clock
		// pos() is wall-clock based and races ahead of the audio device when the ring
		// under-runs at the end; without this guard it crosses _end_time early and the
		// resulting seek-to-start discards the EOF frame before its silence tail is
		// produced, leaving the device to loop its last buffer. A hard +2s margin still
		// ends a stream that never signals EOF so playback can never get stuck.
		if (has_audio_clock() && !_audio_eof_handled)
		{
			return pos(time_now) >= _end_time + 2.0;
		}

		if (has_audio_clock())
		{
			// The EOF has been handled, so a silence tail is queued. End the clip when
			// the audio device has actually played out the real audio (_audio_clock is
			// the device play position). The wall clock pos() drifts ahead of the device
			// (a start-up under-run stalls the device clock), so ending on it would hold
			// the last frame for ~1s while it catches up. Still require the media end to
			// pass so a video longer than its audio plays fully; +2s is a hard fallback.
			const auto audio_played_out = _audio_clock >= _audio_data_end;
			const auto media_played_out = pos(time_now) >= _end_time;
			return (audio_played_out && media_played_out) || pos(time_now) >= _end_time + 2.0;
		}

		if (_has_video)
		{
			// No audio device, so pos() is a free-running wall clock. It crosses _end_time while
			// frames are still queued whenever decoding falls behind, and _end_time itself starts
			// at zero when the container declares no duration - which ended such clips on their
			// very first tick. Require the stream's end to have actually been reached; the +2s
			// fallback still ends a file that never signals one, and presented frames keep
			// raising _end_time so it cannot fire while the clip is still producing pictures.
			return (pos(time_now) >= _end_time && _video_eof_handled) || pos(time_now) >= _end_time + 2.0;
		}

		return pos(time_now) >= _end_time;
	}

	static double time_distance(const double l, const double r)
	{
		return l < r ? r - l : l - r;
	}

	// Nominal for a variable-rate stream, which is all half a frame of judgement needs.
	double video_frame_interval() const
	{
		const auto info = _info.load();
		const auto rate = info ? info->video_frame_rate : 0.0;
		return rate > 0.0 ? 1.0 / rate : 1.0 / 25.0;
	}

	// The presenter's half of should_correct_settled_seek. Recorded here and performed by the read
	// thread, which owns the demuxer, as an audio recovery seek is.
	void request_settle_correction_seek(const double target)
	{
		const auto gen = _seek_gen.load();
		platform::exclusive_lock lock(_forced_seek_mutex);
		_forced_seek_gen = gen;
		_forced_seek = std::max(0.0, target);
		_forced_seek_is_correction = true;
	}

	bool update_for_present(const double time_now)
	{
		platform::exclusive_lock lock_present(_presentation_mutex);

		const auto audio_clock = has_audio_clock();
		const auto is_playing_state = _state == av_play_state::playing;

		// A gap between presents that no playback could explain - the process suspended, the
		// machine slept, a long modal operation - must not be absorbed by the media clock, or a
		// clip with no device to anchor to jumps forward by the whole gap and usually declares
		// itself finished. Holding the clock still across the gap resumes where it left off.
		// With an audio device this cannot happen: the device clock re-anchors every pass.
		if (!audio_clock && is_playing_state && _last_present_time > 0.0)
		{
			const auto elapsed = time_now - _last_present_time;

			if (elapsed > present_stall_seconds || elapsed < 0.0)
			{
				_time_offset = _time_offset + elapsed;
			}
		}

		_last_present_time = time_now;

		// Nothing will ever clear the pending sync without a device clock, so anchor the wall
		// clock to the sought position here. A video track re-anchors more precisely below,
		// once the settle has landed on the frame that position actually refers to.
		if (_pending_time_sync && is_playing_state && !audio_clock && !_reset_time_offset)
		{
			_time_offset = time_now - _last_seek;
			_pending_time_sync = false;
		}

		bool result = false;
		const auto time = this->pos(time_now);

		const auto is_playing_audio = visualize_audio();

		if (is_playing_audio)
		{
			result = _visualizer.step(time);
		}

		av_frame_ptr f;
		auto frame_popped = false;

		// Consume stale frames and current-generation end-of-stream markers before distance
		// comparisons. A frame decoded after a seek's queue clear may still arrive with the old
		// generation; accepting it would let it settle or present a position the user already left.
		for (auto front = _video_frames.front(); front; front = _video_frames.front())
		{
			const auto action = classify_video_queue_front(av_seek_gen_from_frame(front), _seek_gen,
			                                               av_frame_is_eof(front));

			if (action == video_queue_front_action::present) break;
			if (action == video_queue_front_action::mark_eof) _video_eof_handled = true;

			av_frame_ptr discarded;
			if (!_video_frames.pop(discarded)) break;
		}

		const auto seek_ver_invalid = av_seek_gen_from_frame(_frame) != _seek_gen;
		const auto current_ft = av_time_from_frame(_frame);

		// After a seek the demuxer leaves us on a key frame that can be a whole GOP
		// before the sought position. While scrubbing (audio is stopped) and for
		// audio-less video we "settle" the view forward onto the target frame so it
		// matches the scrubber preview and the position the user asked for. During
		// audio playback the audio device stays the master clock, so we keep the
		// existing key-frame-then-play behaviour to avoid an A/V desync.
		const auto settling = _scrubbing || (_settling && !audio_clock);

		if (seek_ver_invalid)
		{
			frame_popped = _video_frames.pop(f);
		}

		if (settling)
		{
			// Drain decoded frames toward the sought time (pos() returns _last_seek
			// while scrubbing or pending) rather than stepping one frame per present,
			// so the view reaches the target as decoding catches up.
			auto best_ft = frame_popped ? av_time_from_frame(f) : current_ft;
			auto has_best = !seek_ver_invalid || (frame_popped && !av_is_frame_empty(f));
			auto reached = false;

			while (!_video_frames.is_empty())
			{
				const auto front_time = _video_frames.front_time();

				if (has_best && time_distance(best_ft, time) <= time_distance(front_time, time) &&
					!df::equiv(best_ft, front_time))
				{
					reached = true; // the next frame is past the target; this one is nearest
					break;
				}

				av_frame_ptr next;

				if (!_video_frames.pop(next))
				{
					break;
				}

				const auto next_action = classify_video_queue_front(av_seek_gen_from_frame(next), _seek_gen,
				                                                    av_frame_is_eof(next));

				if (next_action == video_queue_front_action::mark_eof)
				{
					reached = true; // no frame beyond the target will arrive
					break;
				}

				if (next_action == video_queue_front_action::discard)
				{
					continue;
				}

				if (!av_is_frame_empty(next))
				{
					f = std::move(next);
					best_ft = av_time_from_frame(f);
					has_best = true;
					frame_popped = true;
				}
			}

			// A settle that runs out of frames at the end of the stream would otherwise never
			// complete, leaving pos() frozen on the sought position.
			if (should_finish_video_settle(has_best, reached, best_ft, time, _video_eof_handled))
			{
				_settling = false;
			}

			if (!df::equiv(_correction_target, time)) _correction_target = -1.0;

			if (should_correct_settled_seek(_scrubbing, has_best, best_ft, time, video_frame_interval(),
			                                _correction_target))
			{
				_correction_target = time;
				request_settle_correction_seek(time);
			}
		}
		else if (!seek_ver_invalid && _state == av_play_state::playing && !_pending_time_sync && !_reset_time_offset)
		{
			const auto front_time = _video_frames.front_time();

			if (time_distance(current_ft, time) > time_distance(front_time, time) || df::equiv(current_ft, front_time))
			{
				frame_popped = _video_frames.pop(f);
			}
		}

		if (frame_popped && !av_is_frame_empty(f))
		{
			result = true;

			_frame = std::move(f);
			const auto frame_time = av_time_from_frame(_frame);
			_last_frame_decoded = frame_time;
			raise_end_time(frame_time);
		}

		// Lock the wall clock to the displayed frame only once the view has settled
		// on the sought position, so a no-audio seek anchors playback to the target
		// frame rather than the key frame the settle started from.
		if (_reset_time_offset && av_seek_gen_from_frame(_frame) == _seek_gen && !_settling)
		{
			_time_offset = time_now - _last_frame_decoded;
			_pending_time_sync = false;
			_reset_time_offset = false;
		}

		return result;
	}

	file_load_result capture_first_frame() const
	{
		platform::shared_lock lock_dec(_decoder_rw);
		platform::shared_lock lock_present(_presentation_mutex);

		if (_frame)
		{
			return _decoder.render_frame(_frame);
		}

		return {};
	}

	double time() const
	{
		platform::shared_lock lock_present(_presentation_mutex);
		return av_time_from_frame(_frame);
	}

	void update_visualizer(const ui::vertices_ptr& verts, const recti rect, const pointi offset, const float alpha,
	                       const double time_now) const
	{
		const auto time = pos(time_now);
		_last_frame_time = time;
		_visualizer.render(verts, rect, offset, alpha, time);
	}

	render_valid update_texture(const ui::texture_ptr& texture) const
	{
		platform::exclusive_lock lock_present(_presentation_mutex);
		auto result = render_valid::valid;
		const auto vf = _frame;

		if (vf && texture)
		{
			const auto time = av_time_from_frame(vf);
			const auto needs_render = should_upload_video_frame(_last_texture, texture.get(), texture->is_valid(), time);

			if (needs_render)
			{
				const auto update_result = texture->update(av_video_frame(vf));

				if (update_result != ui::texture_update_result::failed)
				{
					_last_frame_time = time;
					_last_texture = {time, texture.get()};

					if (update_result == ui::texture_update_result::tex_created) result = render_valid::invalid;
					if (update_result == ui::texture_update_result::tex_updated) result = render_valid::present;
				}
			}
		}

		return result;
	}

	friend class av_player;
};

class av_player final : public std::enable_shared_from_this<av_player>
{
	mutable platform::thread_event _video_event;
	mutable platform::thread_event _audio_event;
	mutable platform::thread_event _read_event;

	platform::mutex _queue_mutex;
	platform::mutex _thread_mutex;
	std::atomic<std::shared_ptr<av_session>> _thread_session;

	// Sessions the player decodes for but never plays. Movie's preview needs pictures from two
	// clips at once and supplies its own sound, so it opens these video-only and reads their frames
	// itself. They share the read and video threads with the playing session -- a second pump would
	// be a second set of threads doing the same work -- and never reach the audio thread, because a
	// video-only session has no audio stream to drain.
	std::array<std::atomic<std::shared_ptr<av_session>>, av_max_frame_sessions> _frame_sessions;

	mutable _Guarded_by_(_thread_mutex) std::string _audio_device_id;
	mutable _Guarded_by_(_thread_mutex) std::string _play_audio_device_id;

	_Guarded_by_(_queue_mutex) std::deque<std::function<void(std::shared_ptr<av_player>)>> _q;
	av_host& _host;
	std::function<void(df::file_path, double)> _save_media_position;

public:
	av_player(av_host& host, std::function<void(df::file_path, double)> save_media_position = {}) :
		_video_event(false, false),
		_audio_event(false, false),
		_read_event(false, false), _host(host), _save_media_position(std::move(save_media_position))
	{
	}

	void queue(std::function<void(std::shared_ptr<av_player>)> f)
	{
		{
			platform::exclusive_lock lock(_queue_mutex);
			_q.emplace_back(std::move(f));
		}

		_read_event.set();
	}

	void seek(const std::shared_ptr<av_session>& ses, const double pos, bool scrubbing)
	{
		queue([ses, pos, scrubbing](const std::shared_ptr<av_player>& p) { p->seek_impl(ses, pos, scrubbing); });
	}

	void open(const df::item_element_ptr& item, const bool auto_play, const int video_track, const int audio_track,
	          const bool can_use_hw,
	          const bool use_last_played_pos,
	          const std::function<void(std::shared_ptr<av_session>)>& cb,
	          platform::file_ptr file = {})
	{
		const auto path = item->path();
		const auto file_type = item->file_type();
		const auto starting_position = item->media_position();
		queue([path, file_type, starting_position, auto_play, video_track, audio_track, can_use_hw,
				use_last_played_pos, cb, file = std::move(file)](
			const std::shared_ptr<av_player>& p)
			{
				df::scope_locked_inc l(df::loading_media);
				auto ses = p->open_impl(path, file_type, starting_position, auto_play, video_track, audio_track,
				                        can_use_hw, use_last_played_pos, false, file);
				if (cb) p->_host.queue_ui([cb, ses] { cb(ses); });
			});
	}

	// For a caller that has a path rather than an item, and whose playback stands for something
	// other than the user viewing that file: Movie's clip player plays a clip of a timeline, so it
	// must not write a resume position for the file that clip came from.
	void open_detached(const df::file_path path, const file_type_ref file_type, const double start_position,
	                   const bool auto_play, const std::function<void(std::shared_ptr<av_session>)>& cb)
	{
		queue([path, file_type, start_position, auto_play, cb](const std::shared_ptr<av_player>& p)
		{
			df::scope_locked_inc l(df::loading_media);
			auto ses = p->open_impl(path, file_type, start_position, auto_play, -1, -1, false, false, false, {});

			if (ses)
			{
				ses->remembers_position(false);
				if (start_position > 0) ses->seek(start_position, false);
			}

			if (cb) p->_host.queue_ui([cb, ses] { cb(ses); });
		});
	}

	void close(const std::shared_ptr<av_session>& ses, const std::function<void()>& cb)
	{
		queue([ses, cb](const std::shared_ptr<av_player>& p) { p->close_impl(ses, cb); });
	}

	// Opens a session in `slot` purely as a source of pictures. Video only -- process_io stops
	// reading when *either* packet queue fills, so a session whose audio nothing drains would stall
	// its own video -- and software decoded, because a hardware surface the caller has to download
	// every frame costs more than it saves. It never becomes the playing session, and it never
	// writes a resume position: it stands for a clip of a timeline, not for the user watching a file.
	void open_frames(const size_t slot, const df::file_path path, const file_type_ref file_type,
	                 const double start_position, const std::function<void(std::shared_ptr<av_session>)>& cb)
	{
		if (slot >= av_max_frame_sessions) return;

		queue([slot, path, file_type, start_position, cb](const std::shared_ptr<av_player>& p)
		{
			df::scope_locked_inc l(df::loading_media);
			auto ses = p->open_frames_impl(slot, path, file_type, start_position);
			if (cb) p->_host.queue_ui([cb, ses] { cb(ses); });
		});
	}

	void close_frames(const size_t slot, const std::shared_ptr<av_session>& ses)
	{
		if (slot >= av_max_frame_sessions) return;

		queue([slot, ses](const std::shared_ptr<av_player>& p)
		{
			if (ses) ses->close(false);
			if (p->_frame_sessions[slot].load() == ses) p->_frame_sessions[slot].store(nullptr);

			p->_read_event.set();
			p->_video_event.set();
		});
	}

	void play(const std::shared_ptr<av_session>& ses)
	{
		queue([ses](const std::shared_ptr<av_player>& p) { p->play_impl(ses); });
	}

	void pause(const std::shared_ptr<av_session>& ses)
	{
		queue([ses](const std::shared_ptr<av_player>& p) { p->pause_impl(ses); });
	}

private:
	void seek_impl(const std::shared_ptr<av_session>& ses, const double pos, const bool scrubbing) const
	{
		ses->seek(pos, scrubbing);

		_read_event.set();
		_video_event.set();
		_audio_event.set();
	}

	std::shared_ptr<av_session> open_impl(const df::file_path path, const file_type_ref file_type,
	                                      const double starting_position, const bool auto_play,
	                                      const int video_track, const int audio_track, const bool can_use_hw,
	                                      const bool use_last_played_pos, const bool video_only,
	                                      const platform::file_ptr& file)
	{
		const auto ses = std::make_shared<av_session>(_host, _save_media_position);
		const auto open_result = ses->open(path, file_type, starting_position, auto_play, video_track, audio_track,
		                                   can_use_hw, use_last_played_pos, true, video_only, file);
		auto result = open_result ? ses : nullptr;

		// Only a successful open takes over the decode threads. Publishing a null would silently
		// stop whatever is still playing, which the caller never asked for.
		if (result)
		{
			_thread_session.store(result);
		}

		_read_event.set();
		_audio_event.set();
		_video_event.set();

		return result;
	}

	void close_impl(const std::shared_ptr<av_session>& ses, const std::function<void()>& cb)
	{
		if (ses)
		{
			ses->close();
		}

		if (_thread_session.load() == ses)
		{
			_thread_session.store(nullptr);
		}

		_read_event.set();
		_audio_event.set();
		_video_event.set();

		if (cb)
		{
			_host.queue_ui(cb);
		}
	}

	std::shared_ptr<av_session> open_frames_impl(const size_t slot, const df::file_path path,
	                                             const file_type_ref file_type, const double start_position)
	{
		// Replacing a slot closes what was there first, or the read thread goes on demuxing a file
		// nothing is looking at for as long as the new one takes to open.
		if (const auto previous = _frame_sessions[slot].exchange(nullptr)) previous->close(false);

		const auto ses = std::make_shared<av_session>(_host, _save_media_position);

		// Paused, not playing: the caller decides when the picture moves, because it is composing a
		// timeline and its playhead is not this file's.
		if (!ses->open(path, file_type, start_position, false, -1, -1, false, false, true, true, {}))
		{
			return nullptr;
		}

		ses->remembers_position(false);
		if (start_position > 0) ses->seek(start_position, true);

		_frame_sessions[slot].store(ses);

		_read_event.set();
		_video_event.set();

		return ses;
	}

public:
	void audio_device_id(const std::string_view id) const
	{
		platform::exclusive_lock lock(_thread_mutex);
		_audio_device_id = id;
		_audio_event.set();
	}

	std::string audio_device_id() const
	{
		platform::exclusive_lock lock(_thread_mutex);
		return _audio_device_id;
	}

	void play_audio_device_id(const std::string_view id) const
	{
		platform::exclusive_lock lock(_thread_mutex);
		_play_audio_device_id = id;
		_audio_event.set();
	}

	std::string play_audio_device_id() const
	{
		platform::exclusive_lock lock(_thread_mutex);
		return _audio_device_id.empty() ? _play_audio_device_id : _audio_device_id;
	}

	void decode_video() const
	{
		const std::vector<std::reference_wrapper<platform::thread_event>> events = {platform::event_exit, _video_event};

		while (!df::is_closing)
		{
			wait_for(events, 50, false);

			std::shared_ptr<av_session> session = _thread_session.load();

			if (session)
			{
				session->process_video(_read_event);
			}

			// The frame sources decode on the same thread rather than on one of their own: they are
			// two more video streams, and a preview that had its own decode thread would compete
			// with playback for the same cores while claiming to be the cheaper path.
			for (const auto& slot : _frame_sessions)
			{
				if (const auto frames = slot.load()) frames->process_video(_read_event);
			}
		}
	}

	void decode_audio() const
	{
		std::string device_id;
		platform::audio_output_ptr ds;
		audio_buffer playback_buffer;
		audio_buffer vis_buffer;

		audio_info_t vis_format;
		vis_format.channel_layout = av_get_def_channel_layout(2);
		vis_format.sample_fmt = prop::audio_sample_t::signed_16bit;
		vis_format.sample_rate = 48000;

		vis_buffer.init(vis_format);

		const std::vector<std::reference_wrapper<platform::thread_event>> events = {platform::event_exit, _audio_event};

		auto has_audio = false;
		auto base_time = 0.0;
		auto playback_gen = 0;
		auto vis_gen = 0;
		auto pending_fade_in = false;
		auto need_create_device = false;

		std::shared_ptr<av_session> session;

		while (!df::is_closing)
		{
			const auto device_is_playing = ds && has_audio && session &&
				session->_state == av_play_state::playing && !session->_scrubbing && !ds->is_stopped();

			if (device_is_playing)
			{
				ds->wait_for_buffer(_audio_event, 50);
			}
			else
			{
				const auto idle_timeout = need_create_device
					                          ? 1000u
					                          : std::numeric_limits<uint32_t>::max();
				wait_for(events, idle_timeout, false);
			}

			if (audio_device_id() != device_id)
			{
				device_id = audio_device_id();

				if (ds)
				{
					playback_buffer.clear();
					vis_buffer.clear();
					need_create_device = true;
				}
			}

			const auto thread_session = _thread_session.load();
			if (session != thread_session)
			{
				playback_buffer.clear();
				vis_buffer.clear();

				session = thread_session;
				playback_gen = 0;
				vis_gen = 0;

				if (session)
				{
					has_audio = session->_has_audio;

					if (has_audio && !ds)
					{
						need_create_device = true;
					}
					else if (ds)
					{
						// Reset covers both switches: to another item with audio, and to a silent one.
						// A device left primed keeps playing - and on underrun WASAPI replays its ring -
						// so the previous item would still be heard over media that has no sound at all.
						ds->reset();
					}
				}
				else
				{
					has_audio = false;
					ds.reset();
				}
			}

			if (need_create_device)
			{
				if (ds)
				{
					ds->reset();
					ds.reset();
				}

				ds = platform::create_audio_output(device_id);

				if (ds)
				{
					const auto recovered_audio = session && session->_audio_unavailable.exchange(false);
					play_audio_device_id(ds->id());
					playback_buffer.init(av_audio_info(ds->format()));
					need_create_device = false;
					if (recovered_audio)
					{
						session->request_audio_recovery_seek();
						_read_event.set();
						playback_buffer.clear();
						vis_buffer.clear();
					}
				}
				else
				{
					// Device creation failed - for example the audio endpoint was
					// momentarily unavailable (device switch, exclusive grab by another
					// app, or not yet ready at startup). Leave need_create_device set so
					// we retry on a later iteration. Otherwise the track would advance on
					// the wall clock and play silently for its whole duration.
					playback_buffer.clear();
					need_create_device = has_audio && session != nullptr;

					// There is no device clock for the session to sync to. Tell it so it can
					// time itself off the wall clock; without that it freezes on the sought
					// position, which on a machine with no audio endpoint at all means every
					// video stops dead and a slideshow never advances.
					if (session) session->mark_audio_output_unavailable();
				}

				playback_gen = 0;
				vis_gen = 0;
			}

			if (has_audio && session)
			{
				if (ds)
				{
					const auto ds_is_stopped = ds->is_stopped();
					const auto is_playing_state = session->_state == av_play_state::playing;
					const auto should_be_stopped = !is_playing_state || session->_scrubbing;

					if (!ds_is_stopped && should_be_stopped)
					{
						ds->stop();
					}

					session->process_audio(playback_buffer, vis_buffer, _read_event);

					// process_audio can run long enough for the session to be paused -
					// notably at end of stream, where the view pauses AND seeks back to
					// the start. Re-read the state here: using the value captured above
					// would let us write and play the freshly sought start-of-stream
					// audio, an audible blip of the track restarting as the video ended.
					const auto should_play = session->_state == av_play_state::playing && !session->_scrubbing;

					if (!should_play && !ds->is_stopped())
					{
						ds->stop();
					}

					if (!playback_buffer.is_empty())
					{
						if (playback_gen != playback_buffer.generation())
						{
							base_time = playback_buffer.start_time();
							playback_gen = playback_buffer.generation();
							ds->reset();
							session->_pending_time_sync = true;
							session->_time_offset = df::now() - base_time;

							// reset() empties the endpoint ring, so the next write is the
							// first audio the restarted device plays. Ramp it, but not here -
							// the buffer may hold a single decoded frame at this point.
							pending_fade_in = true;
						}

						if (should_play)
						{
							// Prime the ring before the first start. Starting with an
							// almost-empty buffer makes the device under-run immediately:
							// it loops its tiny initial contents (an audible blip, notably
							// at each loop restart) and its clock stalls, running behind the
							// wall clock for the rest of the clip. Wait for a cushion (or an
							// already-running device, or the stream's end) before starting.
							const auto primed = !ds->is_stopped()
								|| playback_buffer.seconds() >= 0.3
								|| session->_audio_eof_handled;

							if (primed)
							{
								if (pending_fade_in)
								{
									// Nothing has been written since the reset, so the head of
									// the buffer is what the device plays first: ramp it up or
									// the output steps from silence to an arbitrary sample and
									// pops the speaker.
									playback_buffer.apply_fade_in(0.010);
									pending_fade_in = false;
								}

								playback_buffer.write_to(*ds);

								if (ds->is_stopped())
								{
									ds->start();
								}

								if (session->_pending_time_sync)
								{
									const auto time = base_time + ds->time();
									session->_time_offset = df::now() - time;
									session->_pending_time_sync = false;
								}
							}
						}
					}
					else if (should_play && !ds->is_stopped())
					{
						// No decoded audio is available (end of stream, or a brief
						// decode gap). Push silence so the WASAPI ring does not replay
						// (loop) its last contents on underrun - that is what made the
						// audio tail repeat a few times after the video ended. The
						// device is stopped normally once has_ended pauses the session.
						ds->write_silence();
					}

					// Only the audio visualisation draws these frames, and only it drains the queue
					// (see update_for_present). Feeding it during video playback grew the queue for
					// the whole session because nothing ever popped from it.
					if (session->visualize_audio() &&
						vis_buffer.used_bytes() >= session->_visualizer.min_sample_bytes())
					{
						// A seek re-generations the buffer; the visualizer's accumulated
						// samples are then from the old position, so drop them. Compare and
						// store the same value or the clear would repeat every iteration.
						if (vis_gen != vis_buffer.generation())
						{
							vis_gen = vis_buffer.generation();
							session->_visualizer.clear();
						}

						session->_visualizer.update(vis_buffer);
					}

					if (!ds->is_stopped())
					{
						// Device volume is 0.0-1.0; anything above 100% is applied as a
						// software gain on the decoded samples (see set_gain above).
						ds->volume(std::min(session->_volume.load(), 1000) / 1000.0);
					}

					if (ds->is_device_lost())
					{
						session->state(av_play_state::paused);
						audio_device_id({});
						need_create_device = true;
					}

					const auto audio_time = base_time + ds->time();
					session->_audio_clock = audio_time;

					// Keep the smooth UI clock anchored to samples the endpoint has actually
					// consumed. A one-time start anchor drifts ahead after an underrun or a
					// device-clock stall, making video and the visualizer lead audible audio.
					if (!ds->is_stopped() && !session->_pending_time_sync)
					{
						session->_time_offset = df::now() - audio_time;
					}
				}
				else
				{
					session->discard_audio_while_output_unavailable(_read_event);
				}
			}
		}
	}

	void reading()
	{
		const auto player = shared_from_this();
		const std::vector<std::reference_wrapper<platform::thread_event>> events = {platform::event_exit, _read_event};

		while (!df::is_closing)
		{
			const auto e = wait_for(events, 50, false);

			switch (e)
			{
			case 0: // Exit
				break;
			case 1: // kick
				break;
			}

			std::function<void(std::shared_ptr<av_player>)> f;

			while (dequeue(f))
			{
				if (df::is_closing) return;
				f(player);
			}

			if (const auto session = _thread_session.load())
			{
				session->process_pending_forced_seek();
				session->process_io(_video_event, _audio_event);
			}

			for (const auto& slot : _frame_sessions)
			{
				if (const auto frames = slot.load())
				{
					frames->process_pending_forced_seek();
					frames->process_io(_video_event, _audio_event);
				}
			}
		}
	}

	void pause_impl(const std::shared_ptr<av_session>& ses) const
	{
		if (ses->_state == av_play_state::playing)
		{
			ses->state(av_play_state::paused);
			_audio_event.set();
			_video_event.set();
		}
	}

	void play_impl(const std::shared_ptr<av_session>& ses) const
	{
		// Note: no restart-at-end handling is needed here. view_state::tick detects
		// the end of a clip and issues pause + seek-to-start, so a session that has
		// played out is always already positioned at its start when play arrives.
		ses->_pending_time_sync = true;
		// With audio, the audio thread re-establishes the clock and clears
		// _pending_time_sync. With no audio there is no such clock, so we must
		// re-arm _reset_time_offset to let update_for_present rebuild the wall
		// clock from the next frame - otherwise pos() stays frozen at _last_seek
		// and playback appears stuck on resume.
		ses->_reset_time_offset = !ses->_has_audio;
		ses->state(av_play_state::playing);
		_audio_event.set();
		_video_event.set();
	}

	void capture(const std::shared_ptr<av_session>& ses, const std::function<void(file_load_result)>& cb)
	{
		if (!cb) return;

		// The display can lose its session between the caller's check and this call, notably
		// across a file-handle detach, which leaves the media info saying there is video.
		if (!ses)
		{
			_host.queue_ui([cb] { cb({}); });
			return;
		}

		queue([ses, cb](const std::shared_ptr<av_player>& p)
		{
			// Delivered on the UI thread so the callback never has to reach back into
			// UI-owned state from the read thread.
			p->_host.queue_ui([cb, lr = ses->capture_first_frame()]() mutable { cb(std::move(lr)); });
		});
	}

private:
	bool dequeue(std::function<void(std::shared_ptr<av_player>)>& f)
	{
		platform::exclusive_lock lock(_queue_mutex);
		if (_q.empty()) return false;
		f = std::move(_q.front());
		_q.pop_front();
		return true;
	}
};
