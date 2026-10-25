// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implements the Movie task view -- the timeline strip, the composed preview and its
// transport, the two-halved settings panel, source frames, and the project I/O and render workers
// shared with the command line. The document's rules live in model_movie.

#include "pch.h"
#include "view_movie.h"

#include "app_text.h"
#include "av_format.h"
#include "files.h"
#include "model_index.h"
#include "ui_elements.h"

namespace
{
	constexpr int timeline_tile_cx = 96;
	constexpr int timeline_tile_cy = 72;
	// How fast a block dragged against either end of the strip carries it along.
	constexpr double edge_scroll_tiles_per_second = 6.0;
	// One frame, larger than the two ends it replaced, because judging a cut from a thumbnail is the
	// thing this control exists to stop.
	constexpr int trim_frame_cy = 120;
	constexpr double frame_step_seconds = movie_frame_step_seconds;
	// Bounded by bytes, not by count: one preview frame at 4K is 33 MB and sixty thumbnails are less
	// than one of them, so a count would either thrash the strip or hold half a gigabyte.
	constexpr size_t max_cached_bytes = 64 * 1024 * 1024;
	// Deep enough to hold every visible thumbnail plus the two the preview mixes.
	constexpr size_t max_queued_requests = 64;

	// Slots that keep a decoder open. Two for the frame the movie composes, because a crossfade
	// mixes exactly two clips; two more for the ends of a trim, which are two distant positions in
	// one file and would otherwise seek over each other on a single decoder.
	constexpr size_t slot_frame_a = 0;
	constexpr size_t slot_frame_b = 1;
	constexpr size_t slot_trim_in = 2;
	constexpr size_t slot_trim_out = 3;
	constexpr size_t slot_count = 4;

	// Peak audio levels measured across a source. One bar per 20 ms of a ten-minute clip is finer
	// than any track can draw; this is enough that a spoken word is still a visible bump.
	constexpr int audio_peak_buckets = 1024;

	// How far a preview session may sit from the position the movie wants before it is seeked. A
	// session already walking towards that position arrives sooner than a seek would, and one that
	// re-seeks every frame is exactly the slideshow the read-ahead exists to stop.
	constexpr double preview_seek_tolerance = 0.5;

	// How far ahead the clip a transition is about to bring in is opened. Long enough to cover an
	// open, a seek and the first decode, so the crossfade begins with two pictures.
	constexpr double preview_preopen_seconds = 1.5;

	// Seconds of mixed sound kept ahead of the playhead: long enough that a paint or a layout
	// cannot starve the endpoint, short enough that a setting changed during playback is heard soon.
	// A seek restarts the sound, so this is never heard as the position the user has just left.
	constexpr double preview_audio_lead = 0.5;
	constexpr double preview_audio_chunk = 0.05;
	// The endpoint's ring. The mixer refills it from the UI tick rather than from a thread waiting on
	// the device, so it must outlast the gap between two ticks with room for the whole lead: the
	// engine's own minimum is a device period or two, and the preview stuttered on every tick.
	constexpr double preview_audio_ring_seconds = 1.0;
	// How long the picture waits for its sound when playback starts or jumps. Longer than a window
	// takes to read, so a start is not silent; short enough that a busy reader cannot freeze it.
	constexpr double preview_audio_max_priming = 1.5;
	// Each window is read in one task; shorter ones start sooner. The overlap lets a chunk that
	// straddles the boundary read from one buffer.
	constexpr double preview_audio_window_seconds = 10.0;
	constexpr double preview_audio_window_overlap = 1.0;
	// Where the playhead's next sound is asked for, ahead of where it is mixed. Two points, so a run
	// of clips shorter than the reach is still read before each is heard.
	constexpr double preview_audio_prefetch_near = 1.5;
	constexpr double preview_audio_prefetch_far = 3.0;
	constexpr double movie_audio_cache_seconds = 30.0;
	constexpr double movie_audio_overlap_seconds = 2.0;
	// The mixer works in stereo 16-bit, because that is what a clip buffer is. The endpoint may want
	// float, or more channels; this is the only place that difference is expressed. Channels past
	// the second are silent rather than a copy of the front pair, which would place a stereo mix in
	// the middle of a surround field.
	void write_device_samples(uint8_t* const dest, const std::vector<int32_t>& mixed, const uint32_t channels,
	                          const prop::audio_sample_t format)
	{
		const auto frames = mixed.size() / 2;

		for (size_t f = 0; f < frames; ++f)
		{
			for (uint32_t ch = 0; ch < channels; ++ch)
			{
				const auto index = f * channels + ch;
				const auto value = ch < 2 ? std::clamp(mixed[f * 2 + ch], -32768, 32767) : 0;

				switch (format)
				{
				case prop::audio_sample_t::signed_float:
					std::bit_cast<float*>(dest)[index] = static_cast<float>(value) / 32768.0f;
					break;
				case prop::audio_sample_t::signed_16bit:
					std::bit_cast<int16_t*>(dest)[index] = static_cast<int16_t>(value);
					break;
				case prop::audio_sample_t::signed_32bit:
					std::bit_cast<int32_t*>(dest)[index] = value << 16;
					break;
				default:
					break;
				}
			}
		}
	}

	double quantize(const double time)
	{
		return std::round(time / frame_step_seconds) * frame_step_seconds;
	}

	std::string format_clock(const double seconds)
	{
		// std::max answers 0 for a NaN, and the upper bound keeps the conversion defined for a
		// duration no clock can show.
		const auto total = static_cast<int>(std::min(std::max(0.0, seconds),
		                                             static_cast<double>(std::numeric_limits<int>::max())));
		return std::format("{}:{:02}", total / 60, total % 60);
	}

	// Movie takes photos and videos and nothing else, wherever they come from.
	std::vector<df::file_path> media_paths(const df::item_set& items)
	{
		std::vector<df::file_path> result;

		for (const auto& i : items.items())
		{
			const auto* const mt = i->file_type();
			if (!mt || (mt->group != file_group::photo && mt->group != file_group::video)) continue;
			result.emplace_back(i->path());
		}

		return result;
	}

	struct audio_window
	{
		double start = 0;
		double end = 0;
	};

	// The stretch of a clip's sound the preview reads to hear `source_time`. Windows start on whole
	// multiples of the window length from the in point, so every caller asking about one moment
	// names the same window and finds it already read.
	audio_window preview_audio_window(const movie_clip& clip, const double source_time)
	{
		const auto offset = std::max(0.0, source_time - clip.start);
		const auto start = clip.start + std::floor(offset / preview_audio_window_seconds) *
			preview_audio_window_seconds;
		return {start, std::min(clip.end, start + preview_audio_window_seconds + preview_audio_window_overlap)};
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The source frames.
//
// Two kinds of answer, because they are two different jobs. A strip thumbnail is one frame of one
// clip, wanted once and then kept, so it opens the file, takes its frame and closes it. A preview
// frame is the next frame of a clip that is playing, so it comes off a decoder that stays open and
// walks forward; reopening and seeking the file for every 40 ms step is what makes a preview a
// slideshow with nothing on it.
//
// One decode at a time, off the UI thread, latest wins. Everything published comes back through
// queue_ui and is checked against the generation it was asked under, so a frame decoded for a
// superseded scrub is dropped rather than drawn over the settled one.
///////////////////////////////////////////////////////////////////////////////////////////////////

struct movie_frame_key
{
	df::file_path path;
	int time_ms = 0;
	int max_dim = 0;

	bool operator==(const movie_frame_key& other) const
	{
		return time_ms == other.time_ms && max_dim == other.max_dim && path == other.path;
	}
};

// Every input the decoded frame depends on is in the key: a key that omitted the size would serve
// the timeline's thumbnail to the preview.
struct movie_frame_key_hash
{
	size_t operator()(const movie_frame_key& k) const noexcept
	{
		return df::ihash{}(k.path) ^ (static_cast<size_t>(k.time_ms) * 2654435761u) ^ (k.max_dim * 40503u);
	}
};

// The decoders the preview slots keep open. Reached through a shared_ptr, but only ever touched
// from inside a task on the render queue, which one thread serves: tasks there run one after
// another, so this is single-context state and needs no lock of its own.
class movie_slot_decoders
{
public:
	ui::const_surface_ptr frame(const size_t slot, const df::file_path path, const double time, const int max_dim,
	                            const double tolerance)
	{
		if (slot >= _decoders.size()) return {};

		auto& decoder = _decoders[slot];

		if (!decoder || decoder->path() != path)
		{
			decoder.reset();

			auto opened = std::make_unique<av_format_decoder>();
			if (!opened->open(path, media_intent::thumbnail)) return {};

			// Threaded, and video only: this walks whole groups of pictures forward and never wants
			// the audio, which the level indicator measures on its own decoder.
			opened->init_streams(-1, -1, false, true, true);
			if (!opened->has_video()) return {};

			decoder = std::move(opened);
		}

		ui::surface_ptr surface;

		// The slack is half a frame of the clip's own, so the frame decoded is the nearest one: a fixed
		// step wider than a frame let a high-rate clip answer with its neighbour.
		if (!decoder->extract_frame_at(surface, {max_dim, max_dim}, time, tolerance)) return {};

		return upright_movie_frame(surface);
	}

	void close()
	{
		for (auto& decoder : _decoders) decoder.reset();
	}

private:
	std::array<std::unique_ptr<av_format_decoder>, slot_count> _decoders;
};

class movie_source_cache final : public std::enable_shared_from_this<movie_source_cache>
{
public:
	explicit movie_source_cache(view_state& s) : _state(s), _decoders(std::make_shared<movie_slot_decoders>())
	{
	}

	~movie_source_cache()
	{
		if (_peaks_cancel) _peaks_cancel->store(true);
	}

	// The controls panel is a separate window with its own frame, so invalidating the view does not
	// repaint it. Set by the owning view, and called on the UI thread as each answer lands.
	std::function<void()> on_answered;

	// Answers from the cache when it can, and asks for the frame when it cannot. A miss returns
	// null and the caller draws nothing there until the answer arrives and invalidates the view.
	// `urgent` marks the frame the user is looking at, which jumps the queue of thumbnails behind it.
	ui::const_surface_ptr fetch(const df::file_path path, const bool is_photo, const double time, const int max_dim,
	                            const bool urgent = false)
	{
		if (path.is_empty() || max_dim <= 0) return {};

		const movie_frame_key key{path, is_photo ? 0 : static_cast<int>(std::lround(quantize(time) * 1000)), max_dim};

		if (const auto found = _cache.find(key); found != _cache.end())
		{
			touch(key);
			return found->second;
		}

		// A frame that could not be decoded is not asked for again. Without this a source that has
		// moved, or one frame a codec refuses, starts a fresh decode on every paint forever.
		if (_failed.contains(key)) return {};

		request({key, is_photo, no_slot}, urgent);
		return {};
	}

	// The frame one slot should draw. The slot keeps the last frame it was given and answers with
	// that until the one asked for arrives, so a preview waiting on a decode holds its picture
	// instead of blinking to black between frames. `period` is one frame of the clip's source: a slot
	// shows the frame at the time asked, so it is keyed to the millisecond and decoded to the nearest.
	ui::const_surface_ptr slot_frame(const size_t slot, const df::file_path path, const bool is_photo,
	                                 const double time, const int max_dim, const double period)
	{
		if (slot >= _slots.size() || path.is_empty() || max_dim <= 0) return {};

		auto& state = _slots[slot];

		// A slot that has moved to another clip must not go on showing the last one: at a cut that
		// draws the wrong source for as long as the decode takes.
		if (state.path != path)
		{
			state.path = path;
			state.shown = {};
			state.surface.reset();
		}

		if (is_photo)
		{
			// A photo is one frame however long it is held, and the strip wants the same one, so it
			// comes from the shared cache -- but the slot holds on to it once it has it. Two preview
			// photos in a crossfade on a large display outweigh the cache's whole budget, and each one
			// arriving evicted the other, which the next paint decoded again, for ever.
			const movie_frame_key key{path, 0, max_dim};
			if (state.shown == key && is_valid(state.surface)) return state.surface;

			if (auto surface = fetch(path, true, 0, max_dim, true); is_valid(surface))
			{
				state.shown = key;
				state.surface = std::move(surface);
			}

			return state.surface;
		}

		const movie_frame_key key{path, static_cast<int>(std::lround(time * 1000)), max_dim};

		if (state.shown == key && is_valid(state.surface)) return state.surface;

		// The same refusal the strip's frames get, and for the same reason: a paused preview on a
		// frame that cannot be decoded would otherwise start a decode on every refresh.
		if (_failed.contains(key)) return state.surface;

		request({key, false, slot, period / 2}, true);
		return state.surface;
	}

	// Peak audio levels across the whole of `path`: empty while they are being measured, and empty
	// for a file that carries no audio. Measured once per source and kept, because the control that
	// draws them repaints on every pointer move.
	const std::vector<uint8_t>& audio_peaks(const df::file_path path)
	{
		static const std::vector<uint8_t> none;

		if (path.is_empty()) return none;

		if (const auto found = _peaks.find(path); found != _peaks.end()) return found->second;

		if (_peaks_path != path) request_peaks(path);
		return none;
	}

	void clear()
	{
		++_generation;
		_cache.clear();
		_order.clear();
		_bytes = 0;
		_queue.clear();
		_in_flight = false;
		_failed.clear();
		_peaks.clear();
		cancel_peaks();

		for (auto& slot : _slots) slot = {};

		// The decoders belong to the worker, so they are closed there. One thread serves that queue,
		// so this lands after any decode already running and before any that follows.
		_state.queue_async(async_queue::render, [decoders = _decoders] { decoders->close(); });
	}

private:
	static constexpr size_t no_slot = std::numeric_limits<size_t>::max();

	struct request_state
	{
		movie_frame_key key;
		bool is_photo = false;
		// Which slot asked, or no_slot for a strip thumbnail. A slot request decodes on the decoder
		// that slot keeps open, and supersedes any earlier request from the same slot: a preview
		// wants the newest frame, never a backlog of the ones it has already gone past.
		size_t slot = no_slot;
		// How far from the time asked a slot's frame may be: half a frame of the clip's own.
		double tolerance = 0;
		// Asked for by what the user is looking at, so it jumps the queue and is the last dropped.
		bool urgent = false;
	};

	struct slot_state
	{
		df::file_path path;
		movie_frame_key shown;
		ui::const_surface_ptr surface;
	};

	void touch(const movie_frame_key& key)
	{
		std::erase(_order, key);
		_order.emplace_back(key);
	}

	void publish(const request_state& req, ui::const_surface_ptr surface, const uint64_t generation)
	{
		// The key carries every input the frame depends on, so a decoded frame is never wrong and is
		// always worth keeping -- only a cache that has since been cleared refuses it.
		const auto decision = decide_movie_frame_cache_completion(_generation, generation, !_queue.empty());
		if (!decision.accept) return;

		if (decision.clear_in_flight) _in_flight = false;

		if (is_valid(surface))
		{
			if (req.slot < _slots.size())
			{
				auto& state = _slots[req.slot];

				// The slot may have moved to another clip while this was decoding, and a frame of
				// the clip it left is not an answer to what it is asking now.
				if (state.path == req.key.path)
				{
					state.shown = req.key;
					state.surface = std::move(surface);
				}
			}
			else
			{
				const auto existing = _cache.find(req.key);
				const auto replaced = existing != _cache.end() && existing->second ? existing->second->size() : 0_z;
				_bytes = movie_cache_bytes_after_store(_bytes, replaced, surface->size());
				_cache.insert_or_assign(req.key, std::move(surface));
				touch(req.key);

				while (_bytes > max_cached_bytes && _order.size() > 1)
				{
					const auto oldest = _order.front();

					if (const auto found = _cache.find(oldest); found != _cache.end())
					{
						_bytes -= std::min(_bytes, found->second->size());
						_cache.erase(found);
					}

					_order.erase(_order.begin());
				}
			}

			_state.invalidate_view(view_invalid::view_redraw);
			if (on_answered) on_answered();
		}
		else
		{
			_failed.emplace(req.key);
		}

		if (decision.dispatch_next && !_queue.empty())
		{
			const auto next = _queue.front();
			_queue.erase(_queue.begin());
			start(next);
		}
	}

	void request(request_state req, const bool urgent)
	{
		req.urgent = urgent;

		// Latest wins, per slot. A playing movie asks for twenty-five frames a second; queueing them
		// would build a backlog the preview then plays out long after the playhead has left.
		if (req.slot != no_slot)
		{
			std::erase_if(_queue, [&req](const request_state& q) { return q.slot == req.slot; });
		}

		if (!_in_flight)
		{
			start(req);
			return;
		}

		if (req.slot == no_slot && std::find_if(_queue.begin(), _queue.end(),
		                                        [&req](const request_state& q) { return q.key == req.key; }) !=
			_queue.end())
		{
			return;
		}

		// A single pending slot meant a strip of twenty clips asked for twenty frames per paint and
		// kept only the last, so the thumbnails filled in one erratic tile at a time.
		if (urgent) _queue.insert(_queue.begin(), req);
		else _queue.emplace_back(req);

		// Full, the oldest thumbnail makes room. It was asked for by a paint the strip has since scrolled
		// away from; dropping the newest instead kept every tile now on screen blank until the
		// backlog of ones that were not had decoded.
		if (_queue.size() > max_queued_requests)
		{
			const auto oldest = std::ranges::find_if(_queue, [](const request_state& q) { return !q.urgent; });
			_queue.erase(oldest != _queue.end() ? oldest : std::prev(_queue.end()));
		}
	}

	void start(const request_state& req)
	{
		_in_flight = true;

		const auto weak = weak_from_this();
		const auto generation = _generation;
		const auto decoders = _decoders;

		_state.queue_async(async_queue::render, [weak, req, decoders, generation, &s = _state]
		{
			auto surface = req.slot == no_slot
				               ? decode(req.key, req.is_photo)
				               : decoders->frame(req.slot, req.key.path, req.key.time_ms / 1000.0, req.key.max_dim,
				                                 req.tolerance);

			s.queue_ui([weak, req, generation, surface = std::move(surface)]() mutable
			{
				// The weak pointer is a lifetime token only; it is not locked until the work is
				// back on the thread that owns the cache.
				if (const auto self = weak.lock()) self->publish(req, std::move(surface), generation);
			});
		});
	}

	// The work queue, not the render or the load queue: measuring reads a whole file, which takes
	// seconds, and the preview's frames must not wait behind it on the first, nor Save, the probe and
	// the preview's sound -- and the Items view's media, once Movie is left -- on the second.
	//
	// The level track is drawn for one clip at a time, so asking for another clip's levels cancels the
	// scan for the last where it stands, rather than letting it read a file nobody is looking at.
	void request_peaks(const df::file_path path)
	{
		cancel_peaks();

		const auto weak = weak_from_this();
		const auto generation = _generation;
		const auto cancelled = std::make_shared<std::atomic_bool>(false);

		_peaks_path = path;
		_peaks_cancel = cancelled;

		_state.queue_async(async_queue::work, [weak, path, generation, cancelled, &s = _state]
		{
			std::vector<uint8_t> peaks;
			const df::cancel_token token(*cancelled);
			av_format_decoder decoder;

			if (!token.is_cancelled() && decoder.open(path, media_intent::playback))
			{
				decoder.init_streams(-1, -1, false, false, false);
				peaks = decoder.extract_audio_peaks(audio_peak_buckets, token);
			}

			const auto completed = !token.is_cancelled();

			s.queue_ui([weak, path, generation, cancelled, completed, peaks = std::move(peaks)]() mutable
			{
				if (const auto self = weak.lock())
				{
					self->publish_peaks(path, std::move(peaks), generation, cancelled, completed);
				}
			});
		});
	}

	void cancel_peaks()
	{
		if (_peaks_cancel) _peaks_cancel->store(true);
		_peaks_cancel.reset();
		_peaks_path.clear();
	}

	void publish_peaks(const df::file_path path, std::vector<uint8_t> peaks, const uint64_t generation,
	                   const std::shared_ptr<std::atomic_bool>& request, const bool completed)
	{
		// A file with no audio answers with an empty result, and that answer is kept: without it the
		// next paint would ask again, and every paint after that.
		const auto decision = decide_movie_peak_completion(_generation, generation, request == _peaks_cancel,
		                                                   completed);
		if (!decision.accept) return;

		if (decision.clear_pending)
		{
			_peaks_cancel.reset();
			_peaks_path.clear();
		}

		if (!decision.store) return;

		_peaks.insert_or_assign(path, std::move(peaks));
		_state.invalidate_view(view_invalid::view_redraw);
		if (on_answered) on_answered();
	}

	static ui::const_surface_ptr decode(const movie_frame_key& key, const bool is_photo)
	{
		const sizei max_dim{key.max_dim, key.max_dim};

		if (is_photo)
		{
			const auto loaded = files{}.load(key.path, true);
			return loaded.success ? upright_movie_frame(loaded.to_surface(max_dim)) : nullptr;
		}

		av_format_decoder decoder;
		if (!decoder.open(key.path, media_intent::thumbnail)) return {};

		// open() only reads the container. Without this there is no video context, and every decode
		// below answers false with no error -- which is exactly a blank strip and a blank preview.
		decoder.init_streams(-1, -1, false, true, false);

		const auto duration = decoder.end_time() - decoder.start_time();
		if (duration <= 0) return {};

		ui::surface_ptr surface;
		const auto wanted = std::clamp(key.time_ms / 1000.0, 0.0, duration);

		return decoder.extract_thumbnail(surface, max_dim, wanted, duration, true, 0.0)
			       ? upright_movie_frame(surface)
			       : nullptr;
	}

	view_state& _state;
	std::shared_ptr<movie_slot_decoders> _decoders;
	df::hash_map<movie_frame_key, ui::const_surface_ptr, movie_frame_key_hash> _cache;
	df::hash_set<movie_frame_key, movie_frame_key_hash> _failed;
	df::hash_map<df::file_path, std::vector<uint8_t>, df::ihash, df::ieq> _peaks;
	// The one source whose levels are being measured, and the flag that cancels that scan. The flag is
	// shared with the scan, which outlives this object's interest in it.
	df::file_path _peaks_path;
	std::shared_ptr<std::atomic_bool> _peaks_cancel;
	std::array<slot_state, slot_count> _slots;
	std::vector<movie_frame_key> _order;
	std::vector<request_state> _queue;
	size_t _bytes = 0;
	uint64_t _generation = 0;
	bool _in_flight = false;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// The timeline.
//
// This is the document, not a view of the selection: the order is meaningful, a source may appear
// twice, and removing a tile removes it from the movie. That is why it is not the selector strip.
///////////////////////////////////////////////////////////////////////////////////////////////////

class movie_timeline_element final : public view_element, public std::enable_shared_from_this<movie_timeline_element>
{
public:
	movie_timeline_element(view_state& s, movie_view* view) : view_element(view_element_style::can_invoke),
	                                                          _state(s), _view(view)
	{
		padding = {4, 4};
	}

	sizei measure(ui::measure_context& mc, const int cx) const override
	{
		// The scrollbar's row is always reserved, so a timeline that grows past the width does not
		// push the strip, and the preview above it, upwards.
		return {
			cx, df::round(timeline_tile_cy * mc.scale_factor) + mc.padding2 * 3 +
			mc.text_line_height(ui::style::font_face::dialog) + scrollbar_row_cy(mc)
		};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;

		// Hit testing has to use the device metrics the tiles were laid out with. Recomputing them
		// from a nominal scale of 1 put every hit one tile out on any display above 100%.
		_tile_cx = df::round(timeline_tile_cx * mc.scale_factor);
		_tile_cy = df::round(timeline_tile_cy * mc.scale_factor);
		_pad = mc.padding2;
		_scrollbar_cy = scrollbar_row_cy(mc);

		if (_pending_reveal)
		{
			const auto index = *_pending_reveal;
			_pending_reveal.reset();
			reveal(index);
		}

		clamp_scroll();
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto& project = _view->project();
		const auto r = bounds.offset(element_offset);

		dc.draw_rect(r, ui::color(ui::style::color::group_background, dc.colors.alpha));

		const auto text_cy = dc.text_line_height(ui::style::font_face::dialog);
		const auto playing_index = clip_at_playhead();
		const auto dragging = _drag_from >= 0 && _drag_to >= 0;

		_drawn.clear();

		{
			// A tile scrolled part way out of the strip is cut at the strip's edge rather than drawn
			// over the margin beside it.
			const ui::scoped_clip strip_clip(dc, r);

			for (size_t i = 0; i < project.size(); ++i)
			{
				const auto tile = tile_bounds(i, r);
				if (tile.right < r.left || tile.left > r.right) continue;

				const auto& clip = project.clips()[i];
				const auto is_focus = i == project.current();
				const auto is_selected = project.is_selected(i);

				// Selection is the set a command acts on; focus is the one the panel describes. They
				// are drawn differently because they are different facts.
				dc.draw_rect(tile, ui::color(is_selected
					                             ? ui::style::color::dialog_selected_background
					                             : ui::style::color::dialog_background,
				                             dc.colors.alpha * (dragging && is_selected ? 0.5f : 1.0f)));

				draw_thumbnail(dc, clip, i, tile.inflate(-dc.padding1));

				if (is_focus)
				{
					// Focus is one clip and the selection is a set, so focus is an outline over the
					// selected fill rather than another fill.
					dc.draw_border(tile.inflate(-dc.padding1), tile,
					               ui::color(dc.colors.foreground, dc.colors.alpha),
					               ui::color(dc.colors.foreground, 0.0f));
				}

				if (static_cast<int>(i) == playing_index)
				{
					const auto play_clr = ui::color(ui::style::color::important_background, dc.colors.alpha);
					const recti bar{tile.left, tile.bottom - dc.padding1, tile.right, tile.bottom};
					dc.draw_rect(bar, play_clr);
				}

				const recti label{tile.left, tile.bottom, tile.right, tile.bottom + text_cy};
				const auto text = clip.is_trimmed()
					                  ? std::format("{} *", format_clock(clip.duration()))
					                  : format_clock(clip.duration());

				dc.draw_text(text, label, ui::style::font_face::dialog, ui::style::text_style::single_line_center,
				             ui::color(dc.colors.foreground, dc.colors.alpha), {});
			}

			const auto marker = dragging ? std::optional<int>(_drag_to) : _external_drop_to;

			if (marker)
			{
				const auto x = r.left + _pad + *marker * (_tile_cx + _pad) - _pad / 2 - _scroll;
				dc.draw_rect({x - 1, r.top + _pad, x + 2, r.top + _pad + _tile_cy},
				             ui::color(dc.colors.foreground, dc.colors.alpha));
			}
		}

		// The same track and thumb the selector strip draws, so a strip that runs off the side reads
		// the same way in every view.
		if (can_scroll())
		{
			const auto track = scrollbar_bounds().offset(element_offset);
			const auto thumb = scrollbar_thumb_bounds().offset(element_offset);
			dc.draw_rounded_rect(track, ui::color(dc.colors.foreground, dc.colors.alpha * 0.15f), track.height() / 2);
			dc.draw_rounded_rect(thumb, ui::color(dc.colors.foreground, dc.colors.alpha * 0.55f), thumb.height() / 2);
		}

		prune_textures();
	}

	// Which tile the point is over, or -1 when it is past the last one or on the scrollbar.
	int hit(const pointi loc) const
	{
		if (!bounds.contains(loc) || (can_scroll() && loc.y >= tiles_bottom())) return -1;

		const auto offset = loc.x - (bounds.left + _pad) + _scroll;
		if (offset < 0) return -1;

		const auto index = offset / std::max(1, _tile_cx + _pad);
		return index < static_cast<int>(_view->project().size()) ? index : -1;
	}

	// The region a controller made for `loc` should claim. It has to be the tile, not the strip: the
	// framework only rebuilds a controller once the pointer leaves its bounds, so a controller that
	// claimed the whole strip went on answering for whichever tile the pointer first entered over,
	// and every click after that selected the wrong clip. It stops above the scrollbar's row, which
	// is the scrollbar's own region.
	recti hit_bounds(const pointi loc) const
	{
		const auto step = std::max(1, _tile_cx + _pad);
		const auto offset = loc.x - (bounds.left + _pad) + _scroll;
		const auto count = static_cast<int>(_view->project().size());
		const auto bottom = tiles_bottom();

		if (offset < 0) return {bounds.left, bounds.top, bounds.left + _pad, bottom};

		const auto slot = offset / step;

		if (slot >= count)
		{
			const auto x = bounds.left + _pad + count * step - _scroll;
			return {std::max(bounds.left, x), bounds.top, bounds.right, bottom};
		}

		const auto x = bounds.left + _pad + slot * step - _scroll;
		return {x, bounds.top, x + step, bottom};
	}

	// Where a dropped block would land: between tiles, never on one, because dropping onto a clip
	// has no meaning in a single-track timeline.
	int drop_index(const pointi loc) const
	{
		const auto step = std::max(1, _tile_cx + _pad);
		const auto offset = loc.x - (bounds.left + _pad) + _scroll;
		return std::clamp((offset + step / 2) / step, 0, static_cast<int>(_view->project().size()));
	}

	void scroll_by(const int dx)
	{
		set_scroll(_scroll + dx);
	}

	// Brings a tile fully into view. Asked before the first layout there are no bounds to fit it
	// into, so the request waits for them rather than scrolling against a width of nothing.
	void reveal(const size_t index)
	{
		if (bounds.width() <= 0)
		{
			_pending_reveal = index;
			return;
		}

		const auto left = _pad + static_cast<int>(index) * (_tile_cx + _pad);
		const auto right = left + _tile_cx;

		if (left - _pad < _scroll) set_scroll(left - _pad);
		else if (right + _pad > _scroll + bounds.width()) set_scroll(right + _pad - bounds.width());
	}

	bool can_scroll() const
	{
		return bounds.width() > 0 && content_width() > bounds.width();
	}

	// The row reserved for the scrollbar beneath the clip lengths. The whole row answers for the
	// scrollbar, not only the track painted inside it, so no sliver between the track and the tiles
	// belongs to neither.
	recti scrollbar_row() const
	{
		return {bounds.left, bounds.bottom - _scrollbar_cy, bounds.right, bounds.bottom};
	}

	// The painted track, inset within its row.
	recti scrollbar_bounds() const
	{
		const auto row = scrollbar_row();
		const auto inset = std::max(2, _scrollbar_cy / 4);
		return {row.left + _pad, row.top + inset, row.right - _pad, row.bottom - inset};
	}

	// Sized to the share of the timeline in view, and never too small to grab.
	recti scrollbar_thumb_bounds() const
	{
		const auto track = scrollbar_bounds();
		const auto content = std::max(1, content_width());
		const auto thumb_cx = std::min(track.width(), std::max(_scrollbar_cy * 3,
		                                                       df::mul_div(track.width(), bounds.width(), content)));
		const auto travel = std::max(0, track.width() - thumb_cx);
		const auto left = track.left + df::mul_div(_scroll, travel, std::max(1, max_scroll()));
		return {left, track.top, left + thumb_cx, track.bottom};
	}

	// Puts the thumb's left edge at `x`, so a drag keeps the point it was grabbed by.
	void scrollbar_to(const int x)
	{
		const auto track = scrollbar_bounds();
		const auto travel = std::max(1, track.width() - scrollbar_thumb_bounds().width());
		set_scroll(df::mul_div(std::clamp(x - track.left, 0, travel), max_scroll(), travel));
	}

	// A wheel or touchpad delta, in the units the frame accumulates them in, scaled with the tiles so
	// one detent moves the strip the same distance on screen at any display scale.
	void scroll_by_wheel(const int delta)
	{
		scroll_by(df::mul_div(delta, _tile_cx, timeline_tile_cx));
	}

	bool is_dragging() const
	{
		return _drag_from >= 0 && _drag_to >= 0;
	}

	// Files dragged in from outside are shown where they would land, as a block dragged within the
	// strip is: the drop inserts at the boundary under the pointer, and nothing said which one that was.
	// Empty once the drag leaves the strip, ends, or drops.
	void external_drag_over(const std::optional<pointi> loc)
	{
		const auto to = loc ? std::optional<int>(drop_index(*loc)) : std::nullopt;
		if (to == _external_drop_to) return;

		_external_drop_to = to;
		_state.invalidate_view(view_invalid::view_redraw);
	}

	// Moves the drop marker to the pointer, carrying the strip along when the pointer is held near
	// either end, so a block can be dropped at a position that was out of view when the drag began.
	void drag_over(const pointi loc)
	{
		_drag_loc = loc;
		edge_scroll();
		_drag_to = drop_index(loc);
	}

	// One step of the carry. Called on every pointer move and on the idle tick, so a pointer resting
	// at an edge keeps the strip moving. Timed rather than counted, so the speed does not depend on
	// how often either arrives. Answers whether the strip moved.
	bool edge_scroll()
	{
		const auto zone = std::max(1, _tile_cx / 2);
		const auto into_left = bounds.left + zone - _drag_loc.x;
		const auto into_right = _drag_loc.x - (bounds.right - zone);
		const auto depth = into_left > 0 ? -std::min(into_left, zone) : into_right > 0 ? std::min(into_right, zone) : 0;

		if (!is_dragging() || depth == 0)
		{
			_edge_scroll_tick = 0;
			_edge_scroll_carry = 0;
			return false;
		}

		const auto now = platform::tick_count();
		// Bounded, so a stall between two steps does not throw the strip to its end.
		const auto elapsed = _edge_scroll_tick == 0 ? 0.0 : std::min(0.25, (now - _edge_scroll_tick) / 1000.0);
		_edge_scroll_tick = now;

		// Up to six tiles a second at the very edge, slower nearer the middle. The part of a pixel a
		// step falls short by is carried to the next, or a pointer that reports often near the
		// zone's inner edge would round every step to nothing and the strip would never move.
		const auto speed = edge_scroll_tiles_per_second * (_tile_cx + _pad) * depth / zone;
		const auto travel = speed * elapsed + _edge_scroll_carry;
		const auto step = static_cast<int>(travel);
		_edge_scroll_carry = travel - step;

		const auto before = _scroll;
		set_scroll(_scroll + step);

		if (_scroll == before) return false;

		_drag_to = drop_index(_drag_loc);
		return true;
	}

	void end_drag()
	{
		_drag_from = _drag_to = -1;
		_edge_scroll_tick = 0;
		_edge_scroll_carry = 0;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources) _textures.clear();
	}

	int _drag_from = -1;
	int _drag_to = -1;
	// Where files dragged in from outside would be inserted, while they are over the strip.
	std::optional<int> _external_drop_to;

private:
	recti tile_bounds(const size_t index, const recti r) const
	{
		const auto x = r.left + _pad + static_cast<int>(index) * (_tile_cx + _pad) - _scroll;
		return {x, r.top + _pad, x + _tile_cx, r.top + _pad + _tile_cy};
	}

	static int scrollbar_row_cy(const ui::measure_context& mc)
	{
		return std::max(df::round(10 * mc.scale_factor), mc.padding1);
	}

	// Every tile and the gap after it, plus the gap before the first.
	int content_width() const
	{
		return static_cast<int>(_view->project().size()) * (_tile_cx + _pad) + _pad;
	}

	int max_scroll() const
	{
		return std::max(0, content_width() - bounds.width());
	}

	// Where the tile columns end: above the scrollbar's row while there is a scrollbar, and at the
	// strip's foot when there is not, where that row is only margin.
	int tiles_bottom() const
	{
		return can_scroll() ? bounds.bottom - _scrollbar_cy : bounds.bottom;
	}

	// Every change of scroll goes through here. The tile under a resting pointer is a different clip
	// afterwards, so the controller made for the old one is retired rather than left to act on the
	// new one at the next click. One made while a button is held is retired on its release.
	void set_scroll(const int scroll)
	{
		const auto clamped = std::clamp(scroll, 0, max_scroll());
		if (clamped == _scroll) return;

		_scroll = clamped;
		_state.invalidate_view(view_invalid::view_redraw | view_invalid::controller);
	}

	void clamp_scroll()
	{
		set_scroll(_scroll);
	}

	int clip_at_playhead() const
	{
		const auto& project = _view->project();
		if (project.is_empty()) return -1;

		// The clip playing from its own start. During a crossfade that is the incoming one: the clip
		// Previous and Next count from, and the one a click on its tile parks the playhead at the start
		// of. The outgoing clip marked it before, so a click on a tile put the bar under its neighbour.
		return static_cast<int>(movie_step_origin(project.timing().starts, 0, true, _view->movie_state().playhead));
	}

	void draw_thumbnail(ui::draw_context& dc, const movie_clip& clip, const size_t index, const recti target) const
	{
		// A clip whose source was not found has nothing to draw and nothing to ask for. The probe
		// owns that answer, so the strip states it rather than decoding to rediscover it -- and it
		// states it only once the probe has answered, since a clip nobody has looked for yet is not
		// a clip that is gone.
		if (clip.is_lost())
		{
			const auto clr = ui::color(ui::style::color::warning_background, dc.colors.alpha);
			const auto text_cy = dc.text_line_height(ui::style::font_face::dialog);
			const recti icon{target.left, target.top, target.right, target.bottom - text_cy};

			xdraw_icon(dc, icon_index::error, icon, clr, {});
			dc.draw_text(clip.path.name().sv(), {target.left, icon.bottom, target.right, target.bottom},
			             ui::style::font_face::dialog, ui::style::text_style::single_line_center, clr, {});
			return;
		}

		const auto max_dim = std::max(target.width(), target.height());
		const auto surface = _view->sources()->fetch(clip.path, clip.is_photo, clip.start, max_dim);

		if (!is_valid(surface))
		{
			dc.draw_text(clip.path.name().sv(), target, ui::style::font_face::dialog,
			             ui::style::text_style::multiline_center,
			             ui::color(dc.colors.foreground, dc.colors.alpha * 0.5f), {});
			return;
		}

		// Keyed on what the surface is of, not on where it happens to live: an address freed by the
		// source cache and handed back for a different frame would otherwise draw a stale texture.
		const movie_frame_key key{clip.path, clip.is_photo ? 0 : static_cast<int>(std::lround(clip.start * 1000)),
		                          max_dim};
		_drawn.emplace_back(key);

		auto& entry = _textures[key];

		if (!entry.second || entry.first != surface)
		{
			auto t = dc.create_texture();
			if (!t || t->update(surface) == ui::texture_update_result::failed) return;
			entry = {surface, t};
		}

		// Fitted, never stretched: the movie letterboxes mixed aspect ratios and so does the strip
		// that stands for it.
		const auto fitted = ui::scale_dimensions(surface->dimensions(), target.extent(), false);
		dc.draw_texture(entry.second, recti(fitted).offset(target.center() - recti(fitted).center()),
		                dc.colors.alpha);
	}

	// The texture map holds its surfaces alive, so it has to shrink to what is actually on screen.
	void prune_textures() const
	{
		if (_textures.size() <= _drawn.size()) return;

		std::erase_if(_textures, [this](const auto& entry)
		{
			return std::find(_drawn.begin(), _drawn.end(), entry.first) == _drawn.end();
		});
	}

	view_state& _state;
	movie_view* _view;
	int _scroll = 0;
	int _tile_cx = timeline_tile_cx;
	int _tile_cy = timeline_tile_cy;
	int _pad = 6;
	int _scrollbar_cy = 10;
	std::optional<size_t> _pending_reveal;
	pointi _drag_loc;
	int64_t _edge_scroll_tick = 0;
	double _edge_scroll_carry = 0;
	mutable std::vector<movie_frame_key> _drawn;
	mutable df::hash_map<movie_frame_key, std::pair<ui::const_surface_ptr, ui::texture_ptr>, movie_frame_key_hash>
	_textures;
};


///////////////////////////////////////////////////////////////////////////////////////////////////
// The trim control: one track, two handles, the frame at each end, and where the sound is.
//
// A trim is set by looking at it. The numbers say where the handles are; the two end frames say
// what is being kept, and the level track says where the talking is -- which is what most trims are
// actually aimed at.
///////////////////////////////////////////////////////////////////////////////////////////////////

class movie_trim_control final : public view_element, public std::enable_shared_from_this<movie_trim_control>
{
public:
	movie_trim_control(movie_view* view) : view_element(view_element_style::can_invoke), _view(view)
	{
		padding = {2, 6};
	}

	double _start = 0;
	double _end = 0;
	double _limit = 0;
	// Which handle is captured, so the preview can be scrubbed to that handle's frame while it
	// moves and returned to the playhead when it is released.
	int _tracking = -1;
	// Which handle the frame shows once nothing is captured, so releasing a handle leaves the frame
	// it was set by on screen rather than snapping back to the other end.
	int _last_handle = 0;

	sizei measure(ui::measure_context& mc, const int cx) const override
	{
		const auto frame_cy = df::round(trim_frame_cy * mc.scale_factor);

		// A photo has no trim, no sound and no player, so the control is only its frame.
		if (is_photo()) return {cx, frame_cy};

		return {
			cx,
			frame_cy + mc.padding1 + level_track_cy(mc.padding1) +
			mc.text_line_height(ui::style::font_face::dialog)
		};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		_pad = mc.padding1;
		_frame_cy = df::round(trim_frame_cy * mc.scale_factor);
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto r = bounds.offset(element_offset);
		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		draw_handle_frame(dc, {r.left, r.top, r.right, r.top + _frame_cy});

		// Both pictures always show something, and a photo's frame is the whole of what this control
		// has to say about it: there is nothing to trim, hear or play.
		if (is_photo()) return;

		const auto track = track_bounds(r);
		dc.draw_rect(track, ui::color(ui::style::color::dialog_background, dc.colors.alpha));

		if (_limit <= 0) return;

		const recti selected{to_x(track, _start), track.top, to_x(track, _end), track.bottom};
		dc.draw_rect(selected, ui::color(ui::style::color::dialog_selected_background, dc.colors.alpha));

		draw_levels(dc, track);

		// Where the clip player has reached, on the same track the trim is set on.
		if (_view->clip_position())
		{
			const auto x = to_x(track, *_view->clip_position());
			dc.draw_rect({x, track.top, x + 2, track.bottom},
			             ui::color(ui::style::color::important_background, dc.colors.alpha));
		}

		draw_handle(dc, track, _start, clr);
		draw_handle(dc, track, _end, clr);

		const recti label{r.left, track.bottom, r.right, r.bottom};
		dc.draw_text(std::format("{} - {}", format_clock(_start), format_clock(_end)), label,
		             ui::style::font_face::dialog, ui::style::text_style::single_line_center, clr, {});

		if (can_play())
		{
			xdraw_icon(dc, _view->clip_position() ? icon_index::pause : icon_index::play, play_bounds(r), clr, {});
		}

		if (can_reset()) xdraw_icon(dc, icon_index::undo, reset_bounds(r), clr, {});
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_in = {};
			_out = {};
		}
	}

	// The trim is only worth resetting once it has been moved off the whole source.
	bool can_reset() const
	{
		const auto* const clip = _view->project().current_clip();
		return clip && !clip->is_photo && !clip->is_missing && clip->is_trimmed();
	}

	bool can_play() const
	{
		const auto* const clip = _view->project().current_clip();
		return clip && !clip->is_photo && !clip->is_missing && clip->duration() > 0;
	}

	bool is_photo() const
	{
		const auto* const clip = _view->project().current_clip();
		return clip && clip->is_photo;
	}

	recti reset_bounds(const recti r) const
	{
		const auto track = track_bounds(r);
		const auto cy = r.bottom - track.bottom;
		return {r.right - cy, track.bottom, r.right, r.bottom};
	}

	recti play_bounds(const recti r) const
	{
		const auto track = track_bounds(r);
		const auto cy = r.bottom - track.bottom;
		return {r.left, track.bottom, r.left + cy, r.bottom};
	}

	// Which handle the point is nearest, when it is near enough to grab one.
	int hit_handle(const pointi loc) const
	{
		if (_limit <= 0 || !bounds.contains(loc)) return -1;

		const auto track = track_bounds(bounds);
		const auto grab = std::max(6, _pad * 4);
		const auto dx_start = std::abs(loc.x - to_x(track, _start));
		const auto dx_end = std::abs(loc.x - to_x(track, _end));

		// Only the band around the track grabs a handle. The frame above it is what the trim is being
		// judged against and the row below it carries the buttons, and a click on either used to drag
		// the handle out from under the user.
		if (loc.y < track.top - grab || loc.y > track.bottom + grab) return -1;
		if (std::min(dx_start, dx_end) > grab) return -1;
		return pick_movie_trim_handle(dx_start, dx_end, loc.x < to_x(track, _start), _start, _end, _limit);
	}

	// The box a handle answers for, for the same reason the strip's tiles have their own: a
	// controller claiming the whole control never notices the pointer moving to the other handle.
	recti handle_bounds(const int handle) const
	{
		const auto track = track_bounds(bounds);
		const auto grab = std::max(6, _pad * 4);
		const auto x = to_x(track, handle == 0 ? _start : _end);
		return {x - grab, track.top - grab, x + grab, track.bottom + grab};
	}

	// A time moved by a pointer's sideways travel, at the track's scale. Relative rather than read off
	// the pointer's position, so a handle that has not been moved sideways keeps its exact time
	// instead of the time of the pixel it is drawn on.
	double time_moved(const double time, const int dx) const
	{
		const auto track = track_bounds(bounds);
		if (track.width() <= 0 || _limit <= 0) return time;
		return std::clamp(time + static_cast<double>(dx) / track.width() * _limit, 0.0, _limit);
	}

private:
	// A texture and the surface it was made from, so a frame that has not changed is not uploaded
	// again and a surface the cache has replaced never draws from a stale texture.
	struct held_frame
	{
		ui::const_surface_ptr surface;
		ui::texture_ptr texture;
	};

	// Tall enough that a spoken word is a visible bump rather than a flicker on a hairline.
	static int level_track_cy(const int padding1)
	{
		return std::max(12, padding1 * 6);
	}

	recti track_bounds(const recti r) const
	{
		const auto top = r.top + _frame_cy + _pad;
		return {r.left + _pad * 2, top, r.right - _pad * 2, top + level_track_cy(_pad)};
	}

	int to_x(const recti track, const double time) const
	{
		if (_limit <= 0) return track.left;
		return track.left + df::round(track.width() * std::clamp(time / _limit, 0.0, 1.0));
	}

	void draw_handle(ui::draw_context& dc, const recti track, const double time, const ui::color clr) const
	{
		const auto x = to_x(track, time);
		const auto half = std::max(3, dc.padding1);
		dc.draw_rounded_rect({x - half, track.top - half, x + half, track.bottom + half}, clr, half);
	}

	// One frame, showing what the handle in hand is selecting -- or, while the clip player runs, what
	// the clip is doing. Two frames at once halved the size of each and still answered a question
	// nobody was asking: while a handle moves, the only frame that matters is that handle's.
	void draw_handle_frame(ui::draw_context& dc, const recti target) const
	{
		const auto* const clip = _view->project().current_clip();

		dc.draw_rect(target, ui::color(ui::style::color::group_background, dc.colors.alpha));

		if (!clip || clip->is_missing) return;

		const auto max_dim = std::max(target.width(), target.height());

		// A photo is one frame however long it is held, so it comes from the shared cache the strip
		// uses rather than from a slot decoder, and it needs no handle to choose it.
		if (clip->is_photo)
		{
			const auto surface = _view->sources()->fetch(clip->path, true, 0, max_dim, true);
			if (is_valid(surface)) draw_fitted(dc, target, surface, _in);
			return;
		}

		if (_limit <= 0) return;

		const auto playing = _view->clip_position();

		// While the clip player runs the picture comes from its session, which reads ahead and is
		// timed by the audio device. The decoded frame below is for a handle being dragged.
		if (playing && clip->extent.cx > 0 && clip->extent.cy > 0)
		{
			if (const auto tex = _view->clip_texture(dc))
			{
				const auto fitted = ui::scale_dimensions(clip->extent, target.extent(), false);
				const auto destination = recti(fitted).offset(target.center() - recti(fitted).center());
				dc.draw_texture(tex, upright_movie_destination(destination, tex->_orientation),
				                recti({}, tex->dimensions()), dc.colors.alpha, ui::texture_sampler::bilinear);
				return;
			}
		}

		const auto at_end = !playing && (_tracking < 0 ? _last_handle : _tracking) == 1;

		// The out point is the first instant *not* kept, so the frame shown for it is the last one
		// before it: asking for the boundary itself shows a frame the movie will not contain.
		const auto time = playing ? *playing : (at_end ? movie_last_kept_frame_time(*clip, _start, _end) : _start);
		const auto slot = at_end ? slot_trim_out : slot_trim_in;
		auto& held = at_end ? _out : _in;

		const auto surface = _view->sources()->slot_frame(slot, clip->path, false, time, max_dim,
		                                                  movie_frame_period(*clip));
		if (is_valid(surface)) draw_fitted(dc, target, surface, held);
	}

	// Fitted, never stretched, as everything else that stands for a movie frame is. The surface it
	// was made from is held beside the texture so an unchanged frame is not uploaded again and a
	// surface the cache has replaced never draws from a stale texture.
	static void draw_fitted(ui::draw_context& dc, const recti target, const ui::const_surface_ptr& surface,
	                        held_frame& held)
	{
		if (held.surface != surface)
		{
			if (!held.texture) held.texture = dc.create_texture();
			if (!held.texture || held.texture->update(surface) == ui::texture_update_result::failed) return;
			held.surface = surface;
		}

		const auto fitted = ui::scale_dimensions(surface->dimensions(), target.extent(), false);
		dc.draw_texture(held.texture, recti(fitted).offset(target.center() - recti(fitted).center()), dc.colors.alpha);
	}

	// Where the sound is, drawn on the track being dragged. A trim is usually aimed at the start or
	// end of someone talking, and that is invisible in a picture.
	void draw_levels(ui::draw_context& dc, const recti track) const
	{
		const auto* const clip = _view->project().current_clip();
		if (!clip || clip->is_photo || clip->is_missing) return;

		const auto& peaks = _view->sources()->audio_peaks(clip->path);
		const auto cx = track.width();
		if (peaks.empty() || cx <= 0) return;

		const auto bar_cx = std::max(1, dc.padding1 / 2);
		const auto step = bar_cx + 1;
		const auto middle = (track.top + track.bottom) / 2;
		const auto half_cy = std::max(1, (track.height() - 2) / 2);

		const auto inside = ui::color(dc.colors.foreground, dc.colors.alpha);
		// Outside the trim is dimmed rather than hidden: the point of showing the whole source is
		// seeing what a handle would have to move to include.
		const auto outside = ui::color(dc.colors.foreground, dc.colors.alpha * 0.35f);
		const auto start_x = to_x(track, _start);
		const auto end_x = to_x(track, _end);

		for (auto x = track.left; x < track.right; x += step)
		{
			const auto bucket = static_cast<size_t>(static_cast<int64_t>(x - track.left) *
				static_cast<int64_t>(peaks.size()) / cx);
			const auto level = peaks[std::min(bucket, peaks.size() - 1)] / 255.0;
			const auto cy = df::round(half_cy * level);
			if (cy <= 0) continue;

			dc.draw_rect({x, middle - cy, std::min(x + bar_cx, track.right), middle + cy},
			             x >= start_x && x < end_x ? inside : outside);
		}
	}

	movie_view* _view;
	int _pad = 3;
	int _frame_cy = trim_frame_cy;
	mutable held_frame _in;
	mutable held_frame _out;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Controllers
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	// A press that runs one action on release, for the transport steps and the trim reset. Running on
	// release is what lets a press the user changes their mind about and drags away from be abandoned.
	class movie_click_controller final : public view_controller
	{
	public:
		movie_click_controller(view_host_ptr host, const recti bounds, std::function<void()> invoke) :
			view_controller(std::move(host), bounds), _invoke(std::move(invoke))
		{
		}

		ui::style::cursor cursor() const override { return ui::style::cursor::link; }

		void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
		{
			if (_bounds.contains(loc)) _invoke();
		}

	private:
		std::function<void()> _invoke;
	};

	class timeline_controller final : public view_controller
	{
	public:
		timeline_controller(view_host_ptr host, const recti bounds, movie_view* view,
		                    std::shared_ptr<movie_timeline_element> strip, const int index) :
			view_controller(std::move(host), bounds), _view(view), _strip(std::move(strip)), _index(index)
		{
		}

		~timeline_controller() override
		{
			_strip->end_drag();
		}

		ui::style::cursor cursor() const override { return ui::style::cursor::link; }

		void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_held = true;
			if (_index < 0) return;

			// Picking a clip is moving on to it, so whatever was playing stops rather than running on
			// underneath the panel that now describes something else.
			_view->stop_playback();

			const auto index = static_cast<size_t>(_index);

			// A press inside an existing multi-selection must not collapse it, or dragging a block
			// would be impossible: the press that starts the drag would have unselected the rest.
			// The collapse happens on release instead, if no drag followed.
			if (!keys.control && !keys.shift && _view->project().is_selected(index) &&
				_view->project().selected().size() > 1)
			{
				_collapse_on_release = true;
				_view->set_focus(index);
			}
			else
			{
				_view->select_clip(index, keys.shift, keys.control);
			}
		}

		void on_mouse_move(const pointi loc) override
		{
			// A controller is made on hover, so without this the strip reorders itself under a pointer
			// that is only passing over it.
			if (!_held || _index < 0) return;
			if (_strip->_drag_from < 0 && std::abs(loc.x - _start_loc.x) < drag_threshold) return;

			_collapse_on_release = false;
			_strip->_drag_from = _index;
			_strip->drag_over(loc);
			_host->frame()->invalidate();
		}

		void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
		{
			if (_strip->is_dragging())
			{
				_view->move_selection(static_cast<size_t>(_strip->_drag_to));
			}
			else if (_collapse_on_release && _index >= 0)
			{
				_view->select_clip(static_cast<size_t>(_index), false, false);
			}

			_strip->end_drag();
			_collapse_on_release = false;
			_held = false;
		}

		// Escape drops the block where it was, and lets go of the press too: still held, the next twitch
		// of the pointer passed the drag threshold again, restarted the drag, and the release moved the
		// block after all.
		bool escape() override
		{
			if (_strip->_drag_from < 0) return false;
			_strip->end_drag();
			_held = false;
			_collapse_on_release = false;
			_host->frame()->invalidate();
			return true;
		}

	private:
		// Far enough that a click with a shaky hand is still a click.
		static constexpr int drag_threshold = 6;

		movie_view* _view;
		std::shared_ptr<movie_timeline_element> _strip;
		int _index;
		bool _collapse_on_release = false;
		bool _held = false;
	};

	// The strip's scrollbar. A press on the thumb drags it by the point it was grabbed at; a press on
	// the track centres the thumb there -- the selector strip's behaviour, so both strips answer the
	// same way.
	class timeline_scroll_controller final : public view_controller
	{
	public:
		timeline_scroll_controller(view_host_ptr host, const recti bounds, view_state& state,
		                           std::shared_ptr<movie_timeline_element> strip) :
			view_controller(std::move(host), bounds), _state(state), _strip(std::move(strip))
		{
		}

		ui::style::cursor cursor() const override { return ui::style::cursor::left_right; }

		void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_tracking = true;

			// The whole row is the control, so a press over the thumb grabs it at any height in the
			// row; testing the painted thumb's height read a press in the row's margin as a press
			// on the track, and the thumb jumped to centre on it.
			const auto thumb = _strip->scrollbar_thumb_bounds();
			_grab_offset = loc.x >= thumb.left && loc.x <= thumb.right ? loc.x - thumb.left : thumb.width() / 2;
			scroll_to(loc);
		}

		void on_mouse_move(const pointi loc) override
		{
			if (_tracking) scroll_to(loc);
		}

		void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
		{
			if (_tracking) scroll_to(loc);
			_tracking = false;
		}

		bool escape() override
		{
			if (!_tracking) return false;
			_tracking = false;
			return true;
		}

	private:
		void scroll_to(const pointi loc) const
		{
			_strip->scrollbar_to(loc.x - _grab_offset);
			_state.invalidate_view(view_invalid::view_redraw);
		}

		view_state& _state;
		std::shared_ptr<movie_timeline_element> _strip;
		int _grab_offset = 0;
		bool _tracking = false;
	};

	class scrubber_controller final : public view_controller
	{
	public:
		scrubber_controller(view_host_ptr host, const recti bounds, movie_view* view) :
			view_controller(std::move(host), bounds), _view(view)
		{
		}

		ui::style::cursor cursor() const override { return ui::style::cursor::left_right; }

		void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_held = true;
			_press_playhead = _view->movie_state().playhead;
			seek(loc);
		}

		// A controller is made on hover, so a scrubber that acted on every move would seek the movie
		// under a pointer that was only crossing it on its way somewhere else.
		void on_mouse_move(const pointi loc) override
		{
			if (_held) seek(loc);
		}

		void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
		{
			_held = false;
		}

		// Escape puts the playhead back where the press found it. Unanswered here, it went on to leave
		// Movie with the button still down.
		bool escape() override
		{
			if (!_held) return false;

			_held = false;
			_view->seek(_press_playhead);
			return true;
		}

	private:
		void seek(const pointi loc) const
		{
			if (_bounds.width() <= 0) return;
			const auto fraction = std::clamp(static_cast<double>(loc.x - _bounds.left) / _bounds.width(), 0.0, 1.0);
			_view->seek(fraction * _view->duration());
		}

		movie_view* _view;
		double _press_playhead = 0;
		bool _held = false;
	};

	class trim_controller final : public view_controller
	{
	public:
		trim_controller(view_host_ptr host, const recti bounds, movie_view* view,
		                std::shared_ptr<movie_trim_control> trim, const int handle) :
			view_controller(std::move(host), bounds), _view(view), _trim(std::move(trim)), _handle(handle)
		{
		}

		~trim_controller() override
		{
			_trim->_tracking = -1;
			_view->end_preview_override();
		}

		ui::style::cursor cursor() const override { return ui::style::cursor::left_right; }

		void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);

			const auto* const clip = _view->project().current_clip();
			if (!clip) return;

			// The press decides which clip is trimmed. Keys still reach the view while the button is
			// down, and a release that trimmed whichever clip focus had moved to by then changed a clip
			// the user never touched.
			_clip_index = _view->project().current();
			_clip_path = clip->path;

			// A handle shows the frame it is on, and a movie playing on beneath would move the picture
			// away from it, so pressing one stops playback as picking a clip does.
			_view->stop_playback();

			// Capture begins on the press, not on the hover that made this controller: until then the
			// preview belongs to the playhead and the handle belongs to the document.
			_trim->_tracking = _handle;
			_held = true;

			// The handle travels as far as the pointer does from the press, and nowhere until then.
			// Placed at the pointer instead, a press moved it by up to half a grab box, and any wobble
			// rounded a trim stored to the millisecond onto the time of a pixel.
			_press_x = loc.x;
			_press_time = _handle == 0 ? _trim->_start : _trim->_end;
			show_handle_frame();
		}

		void on_mouse_move(const pointi loc) override
		{
			if (_held) move(loc);
		}

		void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
		{
			if (!_held) return;

			_held = false;
			_trim->_tracking = -1;

			if (is_pressed_clip_current()) _view->trim_current(_trim->_start, _trim->_end);
			else let_go();
		}

		// Escape lets go of the handle, and the trim stays as the document has it. Unanswered here, it
		// went on to leave Movie with the handle still held, and the release then trimmed a timeline
		// leaving had already settled.
		bool escape() override
		{
			if (!_held) return false;

			_held = false;
			_trim->_tracking = -1;
			let_go();
			return true;
		}

	private:
		bool is_pressed_clip_current() const
		{
			const auto* const clip = _view->project().current_clip();
			return clip && _view->project().current() == _clip_index && clip->path == _clip_path;
		}

		// Back to what the document holds: the handles re-read it once nothing is tracked.
		void let_go() const
		{
			_view->end_preview_override();
			_view->changed(false);
		}

		void move(const pointi loc) const
		{
			const auto time = _trim->time_moved(_press_time, loc.x - _press_x);

			// The handles cannot cross, and a clip cannot be trimmed to nothing.
			if (_handle == 0) _trim->_start = std::min(time, _trim->_end - 0.1);
			else _trim->_end = std::max(time, _trim->_start + 0.1);

			show_handle_frame();
		}

		void show_handle_frame() const
		{
			const auto* const clip = _view->project().current_clip();
			if (!clip) return;

			const auto preview_time = _handle == 0
				                          ? _trim->_start
				                          : movie_last_kept_frame_time(*clip, _trim->_start, _trim->_end);
			_view->seek_preview_only(preview_time);
			_host->frame()->invalidate();
		}

		movie_view* _view;
		std::shared_ptr<movie_trim_control> _trim;
		int _handle;
		size_t _clip_index = 0;
		df::file_path _clip_path;
		int _press_x = 0;
		double _press_time = 0;
		bool _held = false;
	};
}

view_controller_ptr movie_trim_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                 const pointi element_offset,
                                                                 hit_test_context& ctx)
{
	const auto local = loc - element_offset;

	if (can_play() && play_bounds(bounds).contains(local))
	{
		return std::make_shared<movie_click_controller>(host, play_bounds(bounds).offset(element_offset),
		                                                [view = _view] { view->toggle_play_clip(); });
	}

	if (can_reset() && reset_bounds(bounds).contains(local))
	{
		return std::make_shared<movie_click_controller>(host, reset_bounds(bounds).offset(element_offset),
		                                                [view = _view] { view->reset_trim(); });
	}

	const auto handle = hit_handle(local);
	if (handle < 0) return nullptr;

	_last_handle = handle;

	return std::make_shared<trim_controller>(host, handle_bounds(handle).offset(element_offset), _view,
	                                         shared_from_this(), handle);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The document's bound settings
///////////////////////////////////////////////////////////////////////////////////////////////////

void movie_view_state::read_from_project()
{
	const auto& s = project.settings();

	crossfade = s.transition == movie_transition::crossfade;
	cut = !crossfade;
	transition_tenths = std::clamp(df::round(s.transition_seconds * 10), 0, 100);
	fade_in = s.fade_in;
	fade_out = s.fade_out;
	photo_tenths = std::clamp(df::round(s.photo_seconds * 10), 1, 600);

	if (const auto* const clip = project.current_clip(); clip && clip->is_photo)
	{
		clip_hold_tenths = std::clamp(df::round(clip->duration() * 10), 1, 600);
		clip_hold_is_default = clip->photo_duration_is_default;
	}
}

movie_settings movie_view_state::to_settings() const
{
	movie_settings result = project.settings();

	result.transition = crossfade ? movie_transition::crossfade : movie_transition::cut;
	result.fade_in = fade_in;
	result.fade_out = fade_out;

	// The panel speaks tenths. A length it is still showing is the document's own and is kept exactly --
	// another tool's 0.75 s crossfade shows as 0.8 -- so choosing a setting again changes nothing. A
	// length typed in is held to the bounds any document is.
	if (df::round(result.transition_seconds * 10) != transition_tenths)
	{
		result.transition_seconds = std::clamp(transition_tenths / 10.0, 0.0, movie_max_transition_seconds);
	}

	if (df::round(result.photo_seconds * 10) != photo_tenths)
	{
		result.photo_seconds = std::clamp(photo_tenths / 10.0, movie_min_photo_seconds, movie_max_photo_seconds);
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The controls panel
///////////////////////////////////////////////////////////////////////////////////////////////////

void movie_view_controls::create_controls()
{
	auto settled = [this] { _view->end_edit_group(); };

	_info = create_view_info_element(tt.movie_info);

	_movie_title = std::make_shared<ui::title_control>(icon_index::video, tt.movie_settings_title);
	_crossfade_check = std::make_shared<ui::check_control>(_dlg, tt.movie_crossfade, _movie_state.crossfade, true,
	                                                       false, [this](const bool checked)
	                                                       {
		                                                       // A radio button notifies only the one
		                                                       // that was turned on, so its partner is
		                                                       // set here or the two disagree.
		                                                       _movie_state.crossfade = checked;
		                                                       _movie_state.cut = !checked;
		                                                       _view->settings_changed();
	                                                       });
	_cut_check = std::make_shared<ui::check_control>(_dlg, tt.movie_cut, _movie_state.cut, true, false,
	                                                 [this](const bool checked)
	                                                 {
		                                                 _movie_state.cut = checked;
		                                                 _movie_state.crossfade = !checked;
		                                                 _view->settings_changed();
	                                                 });
	_transition_slider = std::make_shared<ui::slider_control>(_dlg, tt.movie_transition_length,
	                                                          _movie_state.transition_tenths, 0, 50,
	                                                          [this]
	                                                          {
		                                                          _view->settings_changed(
			                                                          movie_edit_group::transition_length);
	                                                          }, settled);
	_fade_in_check = std::make_shared<ui::check_control>(_dlg, tt.movie_fade_in, _movie_state.fade_in, false, false,
	                                                     [this](bool) { _view->settings_changed(); });
	_fade_out_check = std::make_shared<ui::check_control>(_dlg, tt.movie_fade_out, _movie_state.fade_out, false,
	                                                      false, [this](bool) { _view->settings_changed(); });
	_photo_slider = std::make_shared<ui::slider_control>(_dlg, tt.movie_photo_duration, _movie_state.photo_tenths, 1,
	                                                     300,
	                                                     [this]
	                                                     {
		                                                     _view->settings_changed(movie_edit_group::photo_duration);
	                                                     }, settled);
	_output_text = std::make_shared<text_element>(std::string_view{}, ui::style::text_style::multiline);

	_clip_divider = std::make_shared<divider_element>();
	_clip_title = std::make_shared<ui::title_control>(icon_index::edit, tt.movie_clip_title);
	_clip_text = std::make_shared<text_element>(std::string_view{}, ui::style::text_style::multiline);
	_trim = std::make_shared<movie_trim_control>(_view.get());
	_clip_hold_slider = std::make_shared<ui::slider_control>(_dlg, tt.movie_clip_duration,
	                                                         _movie_state.clip_hold_tenths, 1, 300,
	                                                         [this] { _view->hold_changed(movie_edit_group::clip_hold); },
	                                                         settled);
	_clip_hold_default_check = std::make_shared<ui::check_control>(_dlg, tt.movie_use_default,
	                                                               _movie_state.clip_hold_is_default, false, false,
	                                                               [this](bool) { _view->hold_changed(); });

	_controls = {
		_info,
		_movie_title,
		_crossfade_check,
		_cut_check,
		_transition_slider,
		_fade_in_check,
		_fade_out_check,
		_photo_slider,
		_output_text,
		_clip_divider,
		_clip_title,
		_clip_text,
		_trim,
		_clip_hold_slider,
		_clip_hold_default_check,
	};

	_clr = ui::color(ui::style::color::dialog_text);
	update_for_document();
}

bool movie_view_controls::update_for_document()
{
	if (_controls.empty()) return false;

	std::vector<bool> visible_before;
	visible_before.reserve(_controls.size());
	for (const auto& c : _controls) visible_before.emplace_back(c->is_visible());

	// Text that gains or loses lines changes the panel's height as surely as a control appearing does.
	auto text_changed = false;
	const auto show_text = [&text_changed](const std::shared_ptr<text_element>& element, std::string& shown,
	                                       const std::string& text)
	{
		if (shown != text) text_changed = true;
		shown = text;
		element->text(text);
	};

	const auto& project = _movie_state.project;
	const auto* const clip = project.current_clip();
	const auto output = project.output();
	const auto timing = project.timing();

	auto output_text = str_format(tt.movie_output_fmt.sv(), output.extent.cx, output.extent.cy, output.frame_rate,
	                              format_clock(timing.duration));

	// The size the render will produce, from the bitrates it will use. A unit and a number need no
	// translating, and a user choosing a destination needs the order of magnitude before they run.
	if (timing.duration > 0)
	{
		const auto bytes = static_cast<uint64_t>((output.video_bitrate + output.audio_bitrate) / 8.0 *
			timing.duration);
		output_text += "\n~ ";
		output_text += df::format_size(df::file_size(bytes));
	}

	show_text(_output_text, _shown_output_text, output_text);

	// What the view is holding that the user did not ask for and cannot see anywhere else: elements
	// an imported project carried that Movie dropped, clips the import pointed at other files than the
	// project named, and clips whose source has gone. They are stated here rather than in a dialog,
	// because none is a thing to dismiss.
	std::string info(tt.movie_info.sv());

	if (_movie_state.import_ignored > 0)
	{
		info += "\n\n";
		info += format_plural_text(tt.movie_ignored_fmt, _movie_state.import_ignored);
	}

	if (_movie_state.import_relinked > 0)
	{
		info += "\n\n";
		info += format_plural_text(tt.movie_relink_fmt, _movie_state.import_relinked);
	}

	if (const auto missing = std::count_if(project.clips().begin(), project.clips().end(),
	                                       [](const movie_clip& c) { return c.is_lost(); }); missing > 0)
	{
		info += "\n\n";
		info += format_plural_text(tt.movie_missing_fmt, missing);
	}

	show_text(_info, _shown_info_text, info);

	// One number sets the crossfade and the fades at the movie's ends. Hiding it under Cut left the
	// fade length unreachable, so it is shown whenever either use is switched on and named for the
	// one the current settings actually make.
	const auto fades = _movie_state.fade_in || _movie_state.fade_out;
	_transition_slider->is_visible(_movie_state.crossfade || fades);
	_transition_slider->label(_movie_state.crossfade ? tt.movie_transition_length : tt.movie_fade_length);

	const auto has_clip = clip != nullptr;
	const auto is_photo = has_clip && clip->is_photo;

	_clip_divider->is_visible(has_clip);
	_clip_title->is_visible(has_clip);
	_clip_text->is_visible(has_clip);
	// Shown for a photo too, where it is only the frame: the panel and the preview must never
	// disagree about whether there is a picture to see.
	_trim->is_visible(has_clip);
	_clip_hold_slider->is_visible(is_photo && !_movie_state.clip_hold_is_default);
	_clip_hold_default_check->is_visible(is_photo);

	if (has_clip)
	{
		auto text = str_format(tt.movie_clip_fmt.sv(), clip->path.name().sv(),
		                       static_cast<int>(project.current()) + 1, static_cast<int>(project.size()),
		                       format_clock(timing.starts[project.current()]));

		// Two numbers and an x need no translating, and the panel has to say what shape the source
		// is before the output line can be read as a consequence of it.
		if (clip->extent.cx > 0 && clip->extent.cy > 0)
		{
			text += "\n";
			text += prop::format_dimensions(clip->extent);
		}

		show_text(_clip_text, _shown_clip_text, text);

		// A handle the user is holding owns its value: writing the document back over it while the
		// pointer is down would drag the trim out from under them on every refresh.
		if (_trim->_tracking < 0)
		{
			_trim->_start = clip->start;
			_trim->_end = clip->end;
			// A photo has no track, so it has no range: a limit would put grabbable handles under a
			// control that draws none.
			_trim->_limit = is_photo ? 0.0 : (clip->source_duration > 0 ? clip->source_duration : clip->end);
		}
	}

	for (size_t i = 0; i < _controls.size(); ++i)
	{
		if (_controls[i]->is_visible() != visible_before[i]) return true;
	}

	return text_changed;
}

void movie_view_controls::layout_controls(ui::measure_context& mc)
{
	if (!_controls.empty())
	{
		update_for_document();
		view_controls_host::layout_controls(mc);
	}
}

void movie_view_controls::show_document_values() const
{
	if (_controls.empty()) return;

	_crossfade_check->show_bound_value();
	_cut_check->show_bound_value();
	_transition_slider->show_bound_value();
	_fade_in_check->show_bound_value();
	_fade_out_check->show_bound_value();
	_photo_slider->show_bound_value();
	show_clip_values();
}

void movie_view_controls::show_clip_values() const
{
	if (_controls.empty()) return;

	_clip_hold_slider->show_bound_value();
	_clip_hold_default_check->show_bound_value();
}

void movie_view_controls::options_changed()
{
	view_controls_host::options_changed();

	if (!_controls.empty())
	{
		_info->text(tt.movie_info);
		_movie_title->text(tt.movie_settings_title);
		_photo_slider->label(tt.movie_photo_duration);
		_clip_title->text(tt.movie_clip_title);
		_clip_hold_slider->label(tt.movie_clip_duration);
		update_for_document();
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The view
///////////////////////////////////////////////////////////////////////////////////////////////////

movie_view::movie_view(view_state& s, view_host_ptr host, movie_view_state& ms) :
	_state(s), _host(std::move(host)), _movie_state(ms)
{
	_sources = std::make_shared<movie_source_cache>(s);

	// The cache is a member, so it cannot outlive the view, and this runs on the UI thread.
	_sources->on_answered = [this] { invalidate_controls(); };
}

movie_view::~movie_view()
{
	close_clip_session();
	close_preview_sources();
	stop_preview_audio();
}

view_controls_host_ptr movie_view::controls(const ui::control_frame_ptr& owner)
{
	if (!_controls)
	{
		_controls = std::make_shared<movie_view_controls>(_state, _movie_state);
		_controls->_view = shared_from_this();
		_controls->_frame = _controls->_dlg = owner->create_dlg(_controls, false);
	}

	return _controls;
}

void movie_view::activate(const sizei extent)
{
	_extent = extent;

	if (!_timeline)
	{
		_timeline = std::make_shared<movie_timeline_element>(_state, this);
	}

	if (_controls->_controls.empty())
	{
		_controls->create_controls();
	}

	_state.stop();
	_movie_state.playing = movie_view_state::playing_t::nothing;

	const auto selection = media_paths(_state.selected_items());

	// Leaving settles unsaved edits, so the timeline held here is saved, untouched or empty, and a
	// different selection can replace it without losing anything. The same selection, or none,
	// returns to it: that is the movie the user left, including what they saved of it.
	if (decide_movie_entry(_movie_state.project, selection, _movie_state.built_from) == movie_entry::seed)
	{
		seed_from(selection);

		// Recorded only when a timeline is built from it. A timeline kept because it still held
		// unsaved work answers to the selection it was built from, so the new one replaces it once
		// that work is settled.
		_movie_state.built_from = selection;
	}
	else
	{
		probe_clips();
	}

	read_document();
	changed();
}

void movie_view::seed_from(const std::vector<df::file_path>& paths)
{
	auto& project = _movie_state.project;

	if (should_clear_movie_probe_retries_after_document_replace(true)) _probe_retries.clear();
	project.reset({}, project.settings(), {});
	_movie_state.import_ignored = 0;
	_movie_state.import_relinked = 0;
	add_paths_at(paths, 0);
	project.mark_seeded();
	if (_timeline) _timeline->reveal(0);
	seek(0);
}

// Discard on leaving throws the timeline away, so entering again starts from whatever is selected
// then. The settings stay: they are how this user makes movies, not part of the one discarded.
void movie_view::discard_timeline()
{
	stop_playback();

	auto& project = _movie_state.project;
	if (should_clear_movie_probe_retries_after_document_replace(true)) _probe_retries.clear();
	project.reset({}, project.settings(), {});
	_movie_state.import_ignored = 0;
	_movie_state.import_relinked = 0;
	_movie_state.playhead = 0;
	_preview_override.reset();
	_show_focus_frame = false;

	read_document();
	changed();
}

void movie_view::read_document()
{
	_movie_state.read_from_project();
	if (_controls) _controls->show_document_values();
}

void movie_view::deactivate()
{
	_movie_state.playing = movie_view_state::playing_t::nothing;
	close_clip_session();
	close_preview_sources();
	stop_preview_audio();

	// The endpoint goes with the view: a movie nobody is watching has no business holding one.
	_preview_device.reset();
	_preview_audio_rate = 0;
	_preview_audio_priming = false;
	_preview_audio_source_generation = next_movie_preview_audio_source_generation(_preview_audio_source_generation);
	for (auto& entry : _preview_audio) entry = {};

	_sources->clear();
	_texture_a.reset();
	_texture_b.reset();
	_drawn_a.reset();
	_drawn_b.reset();
}

void movie_view::refresh()
{
	_host->frame()->invalidate();
}

void movie_view::broadcast_event(const view_element_event& event) const
{
	if (event.type == view_element_event_type::free_graphics_resources)
	{
		_texture_a.reset();
		_texture_b.reset();
		_drawn_a.reset();
		_drawn_b.reset();
		_clip_texture.reset();
		for (const auto& preview : _preview_sources) preview.texture.reset();
	}

	if (_timeline) _timeline->dispatch_event(event);
	if (_controls && _controls->_trim) _controls->_trim->dispatch_event(event);
}

std::string_view movie_view::title()
{
	_title = std::format("{}: {}", s_app_name, tt.movie_title);
	return _title;
}

double movie_view::duration() const
{
	return _movie_state.project.timing().duration;
}

// Whether this timeline can be rendered now. Whether this machine can render at all is the frame's to
// say: it asks on a worker at startup, because asking starts Media Foundation.
bool movie_view::can_render() const
{
	return _movie_state.project.is_ready_to_render() && !_progress.active;
}

void movie_view::layout(ui::measure_context& mc, const sizei extent)
{
	_extent = extent;

	const auto outer = recti(extent).inflate(-mc.padding2);
	const auto strip_cy = _timeline ? _timeline->measure(mc, outer.width()).cy : 0;
	const auto transport_cy = mc.text_line_height(ui::style::font_face::dialog) + mc.padding2 * 2;

	const auto strip_top = std::max(outer.top, outer.bottom - strip_cy);
	const recti strip{outer.left, strip_top, outer.right, outer.bottom};

	const auto transport_top = std::max(outer.top, strip.top - mc.padding2 - transport_cy);
	_transport_bounds = {outer.left, transport_top, outer.right, strip.top - mc.padding2};

	const auto play_cx = mc.icon_cxy * 2;
	_prev_bounds = {
		_transport_bounds.left, _transport_bounds.top, _transport_bounds.left + play_cx, _transport_bounds.bottom
	};
	_play_bounds = {
		_prev_bounds.right, _transport_bounds.top, _prev_bounds.right + play_cx, _transport_bounds.bottom
	};
	_next_bounds = {
		_play_bounds.right, _transport_bounds.top, _play_bounds.right + play_cx, _transport_bounds.bottom
	};

	const auto clock_cx = mc.measure_text("000:00 / 000:00", ui::style::font_face::dialog,
	                                      ui::style::text_style::single_line, _transport_bounds.width()).cx;

	_scrubber_bounds = {
		_next_bounds.right + mc.padding2, _transport_bounds.top,
		std::max(_next_bounds.right + mc.padding2, _transport_bounds.right - clock_cx - mc.padding2),
		_transport_bounds.bottom
	};

	_preview_bounds = {outer.left, outer.top, outer.right, std::max(outer.top, _transport_bounds.top - mc.padding2)};

	if (_timeline)
	{
		ui::control_layouts positions;
		_timeline->layout(mc, strip, positions);
	}
}

recti movie_view::calc_preview_target(const sizei source) const
{
	// A preview squeezed to nothing has no room for a frame. Fitted into an empty limit, one came back
	// at its full source size and was drawn across the transport and the strip.
	if (source.cx <= 0 || source.cy <= 0 || _preview_bounds.is_empty()) return {};

	// Fitted, never stretched. Mixed aspect ratios letterbox on black, which is what the render
	// does, and the preview exists to be believed.
	const auto fitted = ui::scale_dimensions(source, _preview_bounds.extent(), false);
	return recti(fitted).offset(_preview_bounds.center() - recti(fitted).center());
}

void movie_view::render(ui::draw_context& dc, view_controller_ptr controller)
{
	const auto& project = _movie_state.project;

	dc.draw_rect(_preview_bounds, ui::color(0, dc.colors.alpha));

	if (project.is_empty())
	{
		dc.draw_text(tt.movie_empty, _preview_bounds, ui::style::font_face::dialog,
		             ui::style::text_style::multiline_center,
		             ui::color(dc.colors.foreground, dc.colors.alpha), {});
	}
	else
	{
		auto frame = calc_movie_frame(project.clips(), project.settings(), _movie_state.playhead);

		// While a trim handle is captured the preview shows that handle's frame, so the user sets
		// the trim by looking at it rather than by reading a number.
		if (_preview_override)
		{
			frame = {};
			frame.a.index = static_cast<int>(project.current());
			frame.a.source_time = *_preview_override;
			frame.a.weight = 1.0;
		}
		else if (_show_focus_frame)
		{
			if (const auto* const clip = project.current_clip())
			{
				frame = {};
				frame.a.index = static_cast<int>(project.current());
				frame.a.source_time = clip->is_photo ? 0.0 : clip->start;
				frame.a.weight = 1.0;
			}
		}

		const auto max_dim = std::max(_preview_bounds.width(), _preview_bounds.height());

		// Slots are bound to clips, not to the frame's a and b positions -- preview_slot_for owns
		// why. The cache slot below stays positional, because it is only a stand-in.
		const auto session_a = preview_slot_for(frame.a, project.clips(), av_max_frame_sessions);
		const auto session_b = preview_slot_for(frame.b, project.clips(), session_a);

		if (session_b >= av_max_frame_sessions) preopen_preview_source(session_a);

		const auto draw_source = [&](const movie_frame_source& source, const size_t cache_slot,
		                             const size_t session_slot, ui::texture_ptr& texture,
		                             ui::const_surface_ptr& drawn, const float weight) -> recti
		{
			if (source.index < 0 || weight <= 0.0f) return {};

			const auto& clip = project.clips()[source.index];

			// A video comes off a session the player is reading ahead for; a photo is one frame
			// however long it is held, so it stays on the shared cache.
			if (session_slot < av_max_frame_sessions)
			{
				update_preview_source(session_slot, source.index, clip, source.source_time);

				auto& preview = _preview_sources[session_slot];

				if (preview.session)
				{
					preview.session->update_for_present(df::now());

					if (!preview.texture) preview.texture = dc.create_texture();

					if (preview.texture)
					{
						preview.session->update_texture(preview.texture);

						if (preview.texture->is_valid())
						{
							const auto extent = clip.extent.cx > 0
								                    ? clip.extent
								                    : upright_movie_extent(preview.texture->dimensions(),
								                                           preview.texture->_orientation);
							const auto target = calc_preview_target(extent);
							dc.draw_texture(preview.texture,
							                upright_movie_destination(target, preview.texture->_orientation),
							                recti({}, preview.texture->dimensions()), dc.colors.alpha * weight,
							                ui::texture_sampler::bilinear);
							return target;
						}
					}
				}
			}

			// Until the session has a picture -- while it opens, and for a photo, which has none --
			// the frame cache stands in, so the preview never goes blank waiting for a decoder.
			const auto surface = _sources->slot_frame(cache_slot, clip.path, clip.is_photo, source.source_time,
			                                          max_dim, movie_frame_period(clip));
			if (!is_valid(surface)) return {};

			// One texture per slot, updated in place. A fresh texture per frame is a GPU allocation
			// twenty-five times a second, which is what a video path never does.
			if (drawn != surface)
			{
				if (!texture) texture = dc.create_texture();
				if (!texture || texture->update(surface) == ui::texture_update_result::failed) return {};
				drawn = surface;
			}

			const auto target = calc_preview_target(surface->dimensions());
			dc.draw_texture(texture, target, dc.colors.alpha * weight);
			return target;
		};

		const auto has_mix = frame.b.index >= 0 && frame.b.weight > 0;
		draw_source(frame.a, slot_frame_a, session_a, _texture_a, _drawn_a,
		            has_mix ? 1.0f : static_cast<float>(frame.a.weight));
		const auto target_b = draw_source(frame.b, slot_frame_b, session_b, _texture_b, _drawn_b,
		                                  static_cast<float>(frame.b.weight));

		if (has_mix)
		{
			const auto attenuation = ui::color(0, dc.colors.alpha * static_cast<float>(frame.b.weight));
			if (target_b.is_empty())
			{
				dc.draw_rect(_preview_bounds, attenuation);
			}
			else
			{
				dc.draw_rect({_preview_bounds.left, _preview_bounds.top, _preview_bounds.right, target_b.top}, attenuation);
				dc.draw_rect({_preview_bounds.left, target_b.bottom, _preview_bounds.right, _preview_bounds.bottom}, attenuation);
				dc.draw_rect({_preview_bounds.left, target_b.top, target_b.left, target_b.bottom}, attenuation);
				dc.draw_rect({target_b.right, target_b.top, _preview_bounds.right, target_b.bottom}, attenuation);
			}
		}

		if (frame.fade_to_black > 0)
		{
			dc.draw_rect(_preview_bounds,
			             ui::color(0, dc.colors.alpha * static_cast<float>(frame.fade_to_black)));
		}
	}

	const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);
	const auto total = duration();

	// Stepping needs somewhere to step to, so the two clip buttons state whether there is.
	const auto step_clr = ui::color(dc.colors.foreground, dc.colors.alpha * (project.size() > 1 ? 1.0f : 0.35f));

	xdraw_icon(dc, icon_index::small_left, _prev_bounds, step_clr, {});
	xdraw_icon(dc, _movie_state.playing == movie_view_state::playing_t::movie ? icon_index::pause : icon_index::play,
	           _play_bounds, clr, {});
	xdraw_icon(dc, icon_index::small_right, _next_bounds, step_clr, {});

	dc.draw_rect(_scrubber_bounds, ui::color(ui::style::color::dialog_background, dc.colors.alpha));

	if (total > 0)
	{
		const auto x = _scrubber_bounds.left + df::round(
			_scrubber_bounds.width() * std::clamp(_movie_state.playhead / total, 0.0, 1.0));
		dc.draw_rect({_scrubber_bounds.left, _scrubber_bounds.top, x, _scrubber_bounds.bottom},
		             ui::color(ui::style::color::dialog_selected_background, dc.colors.alpha));
	}

	const recti clock{_scrubber_bounds.right + dc.padding2, _transport_bounds.top, _transport_bounds.right,
	                  _transport_bounds.bottom};
	dc.draw_text(std::format("{} / {}", format_clock(_movie_state.playhead), format_clock(total)), clock,
	             ui::style::font_face::dialog, ui::style::text_style::single_line_center, clr, {});

	if (_timeline) _timeline->render(dc, {});
}

view_controller_ptr movie_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                        hit_test_context& ctx)
{
	if (ctx.occluded(_play_bounds))
	{
		return std::make_shared<movie_click_controller>(host, _play_bounds, [this] { toggle_play(); });
	}

	if (ctx.occluded(_prev_bounds))
	{
		return std::make_shared<movie_click_controller>(host, _prev_bounds, [this] { step_clip(false); });
	}

	if (ctx.occluded(_next_bounds))
	{
		return std::make_shared<movie_click_controller>(host, _next_bounds, [this] { step_clip(true); });
	}

	if (ctx.occluded(_scrubber_bounds) && duration() > 0)
	{
		return std::make_shared<scrubber_controller>(host, _scrubber_bounds, this);
	}

	// The scrollbar's row is tested before the tiles above it, so a press on it never picks a clip.
	if (_timeline && _timeline->can_scroll() && ctx.occluded(_timeline->scrollbar_row()))
	{
		return std::make_shared<timeline_scroll_controller>(host, _timeline->scrollbar_row(), _state, _timeline);
	}

	if (_timeline && ctx.occluded(_timeline->bounds))
	{
		const auto index = _timeline->hit(loc);
		return std::make_shared<timeline_controller>(host, _timeline->hit_bounds(loc), this, _timeline, index);
	}

	return nullptr;
}

bool movie_view::mouse_wheel(const pointi loc, const ui::wheel_notch notch)
{
	// The strip is one row that runs off the side, so either axis moves it along: there is nothing
	// else for the wheel to do while the pointer is over it. It follows the smooth delta rather than
	// whole detents, so a precision touchpad moves it as far as the fingers did.
	if (!_timeline || !_timeline->bounds.contains(loc)) return false;

	// Moving the strip retires the controller under the pointer: the tile there is a different clip.
	_timeline->scroll_by_wheel(notch.is_vertical() ? -notch.delta : notch.delta);
	return true;
}

bool movie_view::key_down(const char32_t key, const ui::key_state keys)
{
	// The key constants are runtime values, so this cannot be a switch.
	if (key == ' ')
	{
		toggle_play();
		return true;
	}

	if (key == keys::DEL)
	{
		remove_current();
		return true;
	}

	if (key == keys::LEFT)
	{
		step_clip(false);
		return true;
	}

	if (key == keys::RIGHT)
	{
		step_clip(true);
		return true;
	}

	if (key == 'Z' && keys.control)
	{
		undo();
		return true;
	}

	return false;
}

bool movie_view::escape()
{
	if (!_movie_state.is_playing()) return false;
	stop_playback();
	return true;
}

bool movie_view::can_exit()
{
	// A render reads the timeline's sources and reports into this view's status band, so navigating
	// away while it runs is refused rather than silently detaching it.
	return !_progress.active;
}

// Close, Items and Escape all arrive here, and leaving is where a timeline holding unsaved work
// is settled: saved, discarded, or kept by staying. Settling it here rather than at shutdown is
// what lets entering again with another selection start a new timeline without losing anything.
void movie_view::exit()
{
	// A project read or write finishes in moments. Leaving under it would apply its result to a
	// timeline nobody is looking at, or leave a save the user asked for unfinished.
	if (_project_io_active) return;
	if (!confirm_render_cancel()) return;

	const auto had_unsaved = has_unsaved_changes();
	const auto weak = weak_from_this();

	// Save leaves once the project is written. A cancelled or failed save stays, with the timeline
	// as it was: it never counts as permission to discard.
	if (!confirm_save_or_discard([weak]
	{
		if (const auto self = weak.lock(); self && self->_state.view_mode() == view_type::movie)
		{
			self->_state.view_mode(view_type::items);
		}
	}))
	{
		return;
	}

	if (had_unsaved) discard_timeline();
	_state.view_mode(view_type::items);
}

void movie_view::tick()
{
	// A block held against an end of the strip keeps carrying it while the pointer rests there.
	if (_timeline && _timeline->is_dragging()) _timeline->edge_scroll();

	if (!_movie_state.is_playing()) return;

	const auto now = platform::tick_count();
	const auto elapsed = _last_tick == 0 ? 0.0 : (now - _last_tick) / 1000.0;
	_last_tick = now;

	if (_movie_state.playing == movie_view_state::playing_t::movie)
	{
		const auto total = duration();

		// The picture holds while its sound primes, so the two start together. Priming ends inside
		// the pump, and the next tick moves the playhead from where the sound began.
		if (!_preview_audio_priming) _movie_state.playhead += elapsed;

		if (_movie_state.playhead >= total)
		{
			_movie_state.playhead = total;
			_movie_state.playing = movie_view_state::playing_t::nothing;
			stop_preview_audio();
		}
		else
		{
			pump_preview_audio();
		}
	}
	else
	{
		const auto* const clip = _movie_state.project.current_clip();

		// The clip player runs over the kept region and stops at the out point, because what it is
		// there to answer is "what is left".
		if (!clip || clip->is_photo || clip->is_missing)
		{
			_movie_state.playing = movie_view_state::playing_t::nothing;
			close_clip_session();
		}
		else if (_clip_session)
		{
			// The session's clock, not the wall clock: the audio device drives it, and that is what
			// puts a cut where the user heard it rather than near it.
			const auto media_now = df::now();
			_clip_session->update_for_present(media_now);
			_movie_state.clip_playhead = std::clamp(_clip_session->pos(media_now), clip->start, clip->end);

			if (_movie_state.clip_playhead >= clip->end - 0.001 || _clip_session->has_ended(media_now))
			{
				_movie_state.clip_playhead = clip->end;
				_movie_state.playing = movie_view_state::playing_t::nothing;
				close_clip_session();
			}
		}

		// The clip plays in the panel, not in the view, so the panel is what has to repaint.
		invalidate_controls();
	}

	_state.invalidate_view(view_invalid::view_redraw);
}

void movie_view::toggle_play()
{
	if (_movie_state.project.is_empty()) return;

	const auto was_playing_movie = _movie_state.playing == movie_view_state::playing_t::movie;
	_movie_state.playing = was_playing_movie
		                       ? movie_view_state::playing_t::nothing
		                       : movie_view_state::playing_t::movie;
	_last_tick = platform::tick_count();

	// Playing is the user saying where to look, so the preview stops answering for the focused clip.
	_show_focus_frame = false;

	// Starting the movie retires the clip player, which is what frees the one session slot.
	close_clip_session();

	if (!was_playing_movie && _movie_state.playhead >= duration()) _movie_state.playhead = 0;

	if (was_playing_movie) stop_preview_audio();
	else start_preview_audio();

	// Playing needs frames at the display's rate, not the five a second the idle timer gives.
	_state.invalidate_view(view_invalid::view_redraw | view_invalid::command_state | view_invalid::animations);
}

void movie_view::toggle_play_clip()
{
	const auto& project = _movie_state.project;
	const auto* const clip = project.current_clip();
	if (!clip || clip->is_photo || clip->is_missing || clip->duration() <= 0) return;

	if (_movie_state.playing == movie_view_state::playing_t::clip)
	{
		_movie_state.playing = movie_view_state::playing_t::nothing;
		close_clip_session();
	}
	else
	{
		// Starting the clip stops the movie: two pictures moving at once, to two different times,
		// is not a preview -- and two soundtracks at once is not one either.
		_preview_override.reset();
		stop_preview_audio();
		_movie_state.playing = movie_view_state::playing_t::clip;

		if (_movie_state.clip_playhead < clip->start || _movie_state.clip_playhead >= clip->end - 0.05)
		{
			_movie_state.clip_playhead = clip->start;
		}

		open_clip_session(clip->path, _movie_state.clip_playhead);
	}

	_last_tick = platform::tick_count();
	_state.invalidate_view(view_invalid::view_redraw | view_invalid::command_state | view_invalid::animations);
}

// The clip player is a real session, so it hears the file the ordinary way and the audio device
// times it. It can take the application player's single slot only because it never runs at the same
// time as the movie preview.
void movie_view::open_clip_session(const df::file_path path, const double at)
{
	close_clip_session();

	const auto* const file_type = files::file_type_from_name(path);

	if (!_state._player || !file_type)
	{
		// Nothing can play it, so nothing is playing: left set, the clip player would claim to run.
		_movie_state.playing = movie_view_state::playing_t::nothing;
		return;
	}

	_clip_session_path = path;

	const auto weak = weak_from_this();
	const auto request = ++_clip_session_request;

	_state._player->open_detached(path, file_type, at, true,
	                              [weak, path, request](std::shared_ptr<av_session> ses)
	                              {
		                              // Back on the UI thread before the view is touched: the weak
		                              // pointer is a lifetime token, not a licence to work here.
		                              if (const auto self = weak.lock())
		                              {
			                              self->clip_session_opened(path, request, std::move(ses));
		                              }
	                              });
}

void movie_view::clip_session_opened(const df::file_path path, const uint64_t request,
                                     std::shared_ptr<av_session> ses)
{
	// Focus may have moved, or the player stopped, or been started again, while the file was opening.
	// A session for a clip nobody is waiting on is closed rather than adopted: lifetime is not currency.
	const auto still_wanted = _movie_state.playing == movie_view_state::playing_t::clip && _clip_session_path == path;

	switch (decide_movie_clip_session(ses != nullptr, request == _clip_session_request, still_wanted))
	{
	case movie_clip_session_answer::adopt:
		_clip_session = std::move(ses);
		_state.invalidate_view(view_invalid::view_redraw | view_invalid::animations);
		invalidate_controls();
		break;

	case movie_clip_session_answer::close:
		if (_state._player) _state._player->close(ses, {});
		break;

	case movie_clip_session_answer::stop_playing:
		_movie_state.playing = movie_view_state::playing_t::nothing;
		_clip_session_path.clear();
		_state.invalidate_view(view_invalid::view_redraw | view_invalid::command_state);
		invalidate_controls();
		break;

	case movie_clip_session_answer::ignore:
		break;
	}
}

void movie_view::close_clip_session()
{
	_clip_session_path.clear();
	_clip_texture.reset();

	if (_clip_session)
	{
		if (_state._player) _state._player->close(_clip_session, {});
		_clip_session.reset();
	}
}

// Which preview slot serves one contributor. Bound to the clip, not to the frame's a or b position:
// at the end of a crossfade the incoming clip moves from b to a, and mapping a to slot 0 would tear
// down the warm, reading-ahead session that had just filled and reopen the same file in the other
// slot. That reopen is a black gap in the middle of playback.
//
// Answers av_max_frame_sessions for a contributor no session can serve -- a photo, or a clip whose
// source has gone -- which is the caller's signal to use the frame cache instead.
size_t movie_view::preview_slot_for(const movie_frame_source& source, const std::vector<movie_clip>& clips,
                                    const size_t avoid)
{
	if (source.index < 0 || source.weight <= 0.0) return av_max_frame_sessions;

	const auto& clip = clips[source.index];
	if (clip.is_photo || clip.is_missing) return av_max_frame_sessions;

	for (size_t slot = 0; slot < _preview_sources.size(); ++slot)
	{
		if (slot == avoid) continue;
		if (_preview_sources[slot].index == source.index && _preview_sources[slot].path == clip.path) return slot;
	}

	for (size_t slot = 0; slot < _preview_sources.size(); ++slot)
	{
		if (slot == avoid) continue;
		if (_preview_sources[slot].index < 0) return slot;
	}

	// Both slots hold other clips, so the one the other contributor is not using is the one the
	// movie has left behind.
	return avoid == 0 ? 1 : 0;
}

// Opens the clip the playhead is about to reach, in whichever slot is free, before its transition
// begins. A crossfade whose incoming session opens when the crossfade opens spends its first
// moments mixing one picture with black, which reads as a flicker rather than as a fade.
void movie_view::preopen_preview_source(const size_t used_slot)
{
	if (_movie_state.playing != movie_view_state::playing_t::movie) return;

	const auto& project = _movie_state.project;
	const auto ahead = calc_movie_frame(project.clips(), project.settings(),
	                                    _movie_state.playhead + preview_preopen_seconds);

	// At a transition the incoming clip is b; away from one it is whatever a names, which may still
	// be a clip the playhead has not reached.
	const auto& next = ahead.b.index >= 0 ? ahead.b : ahead.a;
	if (next.index < 0) return;
	if (used_slot < _preview_sources.size() && next.index == _preview_sources[used_slot].index) return;

	const auto slot = preview_slot_for(next, project.clips(), used_slot);
	if (slot >= av_max_frame_sessions) return;

	const auto& clip = project.clips()[next.index];

	// Opened at its in point rather than at where it will be in a second and a half: it is left
	// paused until it contributes, so that is where it has to be waiting.
	if (!ensure_preview_source(slot, next.index, clip, clip.start)) return;

	// Settled while it waits, so the transition's first frame is one the slot already holds.
	_preview_sources[slot].session->update_for_present(df::now());
}

// Points one preview slot at one clip, and answers whether it has a session yet. Opening is a
// request queued on the player; until it lands the frame cache stands in, so nothing here waits.
bool movie_view::ensure_preview_source(const size_t slot, const int clip_index, const movie_clip& clip,
                                       const double source_time)
{
	if (slot >= _preview_sources.size() || !_state._player) return false;

	auto& preview = _preview_sources[slot];

	if (preview.index == clip_index && preview.path == clip.path) return preview.session != nullptr;

	close_preview_source(slot);

	const auto* const file_type = files::file_type_from_name(clip.path);
	if (!file_type) return false;

	preview.index = clip_index;
	preview.path = clip.path;
	preview.opening = true;
	preview.sought = source_time;

	const auto weak = weak_from_this();
	const auto path = clip.path;

	_state._player->open_frames(slot, path, file_type, source_time,
	                            [weak, slot, clip_index, path](std::shared_ptr<av_session> ses)
	                            {
		                            // Back on the UI thread before the view is touched: the weak
		                            // pointer is a lifetime token, not a licence to work here.
		                            if (const auto self = weak.lock())
		                            {
			                            self->preview_source_opened(slot, clip_index, path, std::move(ses));
		                            }
	                            });

	return false;
}

void movie_view::update_preview_source(const size_t slot, const int clip_index, const movie_clip& clip,
                                       const double source_time)
{
	if (!ensure_preview_source(slot, clip_index, clip, source_time)) return;

	auto& preview = _preview_sources[slot];

	// The session runs while the movie runs and holds while it is parked -- or while the movie waits
	// for its sound, or it would walk on ahead of a playhead that is standing still, or while a trim
	// handle is held, whose frame is the one wanted. Both clocks are the wall clock, so a clip that
	// started in step with the movie stays in step with it.
	const auto playing = _movie_state.playing == movie_view_state::playing_t::movie && !_preview_audio_priming &&
		!_preview_override;

	if (playing != preview.session->is_playing())
	{
		if (playing) _state._player->play(preview.session);
		else _state._player->pause(preview.session);

		// Set going or stopped, the session leaves wherever it was last sent, so that is forgotten.
		preview.sought.reset();
	}

	const auto position = preview.session->pos(df::now());
	// Parked, the frame shown must be the one at the playhead or the handle, not a neighbour, so it is
	// held to half a frame of the clip's own.
	const auto tolerance = playing ? preview_seek_tolerance : movie_frame_period(clip) / 2;

	// Only a jump the forward walk cannot absorb is a seek. Re-seeking to every position the
	// playhead passes would decode from a key frame for each one and show almost none of them.
	if (decide_movie_preview_seek(position, source_time, tolerance, preview.sought))
	{
		_state._player->seek(preview.session, source_time, !playing);
	}
}

void movie_view::preview_source_opened(const size_t slot, const int clip_index, const df::file_path path,
                                       std::shared_ptr<av_session> ses)
{
	if (slot >= _preview_sources.size()) return;

	auto& preview = _preview_sources[slot];
	preview.opening = false;

	// The slot may have moved to another clip while the file was opening. A session for a clip
	// nothing is drawing is closed rather than adopted: lifetime is not currency.
	if (!ses || preview.index != clip_index || preview.path != path)
	{
		if (_state._player) _state._player->close_frames(slot, ses);
		return;
	}

	// Left paused. A slot opened ahead of its transition must wait at its in point, and one that is
	// already contributing is set playing by the next paint.
	preview.session = std::move(ses);
	preview.texture.reset();

	_state.invalidate_view(view_invalid::view_redraw);
}

void movie_view::close_preview_source(const size_t slot)
{
	if (slot >= _preview_sources.size()) return;

	auto& preview = _preview_sources[slot];
	if (preview.index < 0 && !preview.session && !preview.opening) return;

	if (_state._player) _state._player->close_frames(slot, preview.session);

	preview = {};
}

void movie_view::close_preview_sources()
{
	for (size_t slot = 0; slot < _preview_sources.size(); ++slot) close_preview_source(slot);
}

// The endpoint is opened on the first play rather than when the view is entered: a movie that is
// only being assembled has nothing to say, and holding an audio device open to say it is rude.
void movie_view::start_preview_audio()
{
	// Set again by the reset below when there is a device to prime. Without one there is nothing to
	// wait for, and a flag left over from before would hold the picture for ever.
	_preview_audio_priming = false;

	if (!_preview_device)
	{
		_preview_device = platform::create_audio_output({}, preview_audio_ring_seconds);

		// No endpoint is not a failure the user has to be told about: the preview is silent, which
		// is what it was before it could speak at all.
		if (!_preview_device) return;

		const auto format = av_audio_info(_preview_device->format());

		// write_device_samples speaks float, 16-bit and 32-bit; an endpoint mixing in anything else
		// is left silent rather than fed noise.
		if (format.sample_rate == 0 || format.channel_count() == 0 || format.bytes_per_sample() == 0 ||
			format.sample_fmt == prop::audio_sample_t::none)
		{
			_preview_device.reset();
			return;
		}

		_preview_audio_buffer.init(format);
		const auto rate = static_cast<int>(format.sample_rate);

		// The windows hold samples at whatever rate the last endpoint wanted, so a different rate
		// takes them with it.
		if (rate != _preview_audio_rate)
		{
			for (auto& entry : _preview_audio) entry = {};
			_preview_audio_rate = rate;
		}
	}

	reset_preview_audio();
}

void movie_view::stop_preview_audio()
{
	_preview_audio_priming = false;
	if (!_preview_device) return;

	_preview_device->stop();
	_preview_audio_buffer.clear();
}

void movie_view::drop_preview_device()
{
	_preview_device.reset();
	_preview_audio_buffer.clear();
	_preview_audio_priming = false;
}

// Everything queued was mixed for where the playhead was going, so a playhead that has gone
// somewhere else drops it and the sound primes again from where the playhead is. The generation is
// what stops a chunk built before the move being appended to one built after it.
void movie_view::reset_preview_audio()
{
	if (!_preview_device) return;

	++_preview_audio_generation;
	_preview_audio_buffer.clear();
	_preview_audio_time = _movie_state.playhead;
	_preview_device->reset();
	_preview_audio_priming = true;
	_preview_audio_priming_since = platform::tick_count();
}

void movie_view::pump_preview_audio()
{
	if (!_preview_device || _preview_audio_rate <= 0) return;

	if (_preview_device->is_device_lost())
	{
		drop_preview_device();
		return;
	}

	const auto& project = _movie_state.project;
	const auto format = av_audio_info(_preview_device->format());
	const auto channels = format.channel_count();
	const auto sample_bytes = format.bytes_per_sample();

	if (channels == 0 || sample_bytes == 0)
	{
		drop_preview_device();
		return;
	}

	// The sound has to stay where the picture is. A queue that ran out behind the playhead -- a stall
	// longer than the lead -- or one left far ahead of it by a jump back, or sound piling up because
	// the endpoint stopped taking it, restarts from the playhead rather than playing out of step with
	// the picture for the rest of the movie. The endpoint's own clock is not asked: a Bluetooth
	// headset reports a latency that is not a drift, and an endpoint whose clock does not move would
	// restart the sound for ever.
	if (!_preview_audio_priming &&
		(_preview_audio_time < _movie_state.playhead ||
			_preview_audio_time > _movie_state.playhead + preview_audio_lead * 2 ||
			_preview_audio_buffer.seconds() > preview_audio_lead))
	{
		reset_preview_audio();
	}

	// While priming, a window still being read holds the start back so the movie begins with its
	// sound. Once playing it is silence instead: a gap is shorter than the restart a stall costs, and
	// the sound comes back in step as soon as the window arrives.
	const auto waited = (platform::tick_count() - _preview_audio_priming_since) / 1000.0;
	const auto may_wait = _preview_audio_priming && waited < preview_audio_max_priming;
	const auto target = std::min(duration(), _movie_state.playhead + preview_audio_lead);

	std::vector<int32_t> mixed;
	std::vector<uint8_t> chunk;

	while (_preview_audio_time < target)
	{
		const auto seconds = std::min(preview_audio_chunk, target - _preview_audio_time);
		const auto frames = static_cast<size_t>(std::llround(seconds * _preview_audio_rate));
		if (frames == 0) break;

		mixed.assign(frames * 2, 0);

		// The same decision the picture is composed from, at the same instant, so the sound and the
		// frame cannot disagree about which clips are playing or at what weight.
		const auto frame = calc_movie_frame(project.clips(), project.settings(), _preview_audio_time);
		auto unread = false;

		const auto mix = [&](const movie_frame_source& source)
		{
			if (source.index < 0 || source.weight <= 0.0) return;

			const auto& clip = project.clips()[source.index];
			if (clip.is_photo || clip.is_missing) return;

			auto window_start = 0.0;
			const auto pcm = preview_pcm(clip, source.source_time, window_start);

			if (!pcm)
			{
				unread = true;
				return;
			}

			// The fade at the movie's ends is a fade of the movie, not of its picture.
			const auto weight = source.weight * (1.0 - frame.fade_to_black);
			const auto first = static_cast<int64_t>(
				std::llround((source.source_time - window_start) * _preview_audio_rate)) * 2;

			for (size_t i = 0; i < mixed.size(); ++i)
			{
				const auto at = first + static_cast<int64_t>(i);
				if (at < 0 || at >= static_cast<int64_t>(pcm->size())) continue;

				mixed[i] += static_cast<int32_t>(std::lround((*pcm)[static_cast<size_t>(at)] * weight));
			}
		};

		mix(frame.a);
		mix(frame.b);

		if (unread && may_wait) break;

		chunk.assign(frames * channels * sample_bytes, 0);
		write_device_samples(chunk.data(), mixed, channels, format.sample_fmt);

		_preview_audio_buffer.append(chunk.data(), static_cast<uint32_t>(chunk.size()), _preview_audio_time,
		                             _preview_audio_generation);

		_preview_audio_time += static_cast<double>(frames) / _preview_audio_rate;
	}

	prefetch_preview_audio();

	// The user's media volume, as the player applies it. Past 100% is a gain the player puts on its
	// decoded samples, and this mixer has none, so it stops at full.
	_preview_device->volume(std::clamp(setting.media_volume, 0, 1000) / 1000.0);

	if (!_preview_audio_buffer.is_empty()) _preview_audio_buffer.write_to(*_preview_device);

	// Started once the lead is queued, so the device begins on a cushion rather than on a sliver it
	// would run through before the next tick.
	if (_preview_audio_priming && (_preview_audio_time >= target - 0.001 || !may_wait))
	{
		_preview_audio_priming = false;
		_preview_device->start();
	}
}

void movie_view::prefetch_preview_audio()
{
	const auto& project = _movie_state.project;
	const auto total = duration();

	for (const auto reach : {preview_audio_prefetch_near, preview_audio_prefetch_far})
	{
		const auto ahead = _preview_audio_time + reach;
		if (ahead >= total) break;

		const auto frame = calc_movie_frame(project.clips(), project.settings(), ahead);

		for (const auto& source : {frame.a, frame.b})
		{
			if (source.index < 0) continue;

			const auto& clip = project.clips()[source.index];
			if (clip.is_photo || clip.is_missing) continue;

			auto window_start = 0.0;
			preview_pcm(clip, source.source_time, window_start);
		}
	}
}

std::shared_ptr<const std::vector<int16_t>> movie_view::preview_pcm(const movie_clip& clip,
                                                                    const double source_time, double& window_start)
{
	if (_preview_audio_rate <= 0) return {};

	const auto window = preview_audio_window(clip, source_time);
	window_start = window.start;

	// Nothing of the clip is left to hear at this instant. That is silence, not a read to wait for:
	// answered as unread, it would hold the start of playback for the whole priming allowance.
	if (window.end <= window.start)
	{
		static const auto silence = std::make_shared<const std::vector<int16_t>>();
		return silence;
	}

	const auto stamp = ++_preview_audio_wanted;
	auto* oldest = &_preview_audio.front();

	for (auto& entry : _preview_audio)
	{
		if (entry.path == clip.path && entry.start == window.start && entry.end == window.end &&
			entry.sample_rate == _preview_audio_rate)
		{
			entry.wanted = stamp;
			return entry.pcm;
		}

		if (entry.wanted < oldest->wanted) oldest = &entry;
	}

	*oldest = {};
	oldest->path = clip.path;
	oldest->start = window.start;
	oldest->end = window.end;
	oldest->sample_rate = _preview_audio_rate;
	oldest->wanted = stamp;

	const auto weak = weak_from_this();
	const auto path = clip.path;
	const auto start = window.start;
	const auto end = window.end;
	const auto rate = _preview_audio_rate;
	const auto source_generation = _preview_audio_source_generation;
	oldest->source_generation = source_generation;

	// Not the render queue: the preview's pictures must not wait behind sound the playhead has not
	// reached yet.
	_state.queue_async(async_queue::load, [weak, path, start, end, rate, source_generation, &s = _state]
	{
		std::vector<int16_t> pcm;
		av_format_decoder decoder;

		if (decoder.open(path, media_intent::playback))
		{
			decoder.init_streams(-1, -1, false, false, false);
			pcm = decoder.extract_audio_pcm_range(rate, start, end - start);
		}

		s.queue_ui([weak, path, start, end, rate, source_generation, pcm = std::move(pcm)]() mutable
		{
			// The weak pointer is a lifetime token only; it is locked here, back on the thread
			// that owns the view.
			if (const auto self = weak.lock())
			{
				self->preview_pcm_loaded(path, start, end, rate, source_generation, std::move(pcm));
			}
		});
	});

	return {};
}

void movie_view::preview_pcm_loaded(const df::file_path path, const double start, const double end,
                                    const int sample_rate, const int source_generation, std::vector<int16_t> pcm)
{
	if (!should_accept_movie_preview_pcm_loaded(_preview_audio_source_generation, source_generation)) return;

	// The window may have been replaced while it was read. Sound for a window nothing is waiting on
	// is dropped rather than kept: lifetime is not currency here either.
	for (auto& entry : _preview_audio)
	{
		if (!entry.pcm && entry.path == path && entry.start == start && entry.end == end &&
			entry.sample_rate == sample_rate && entry.source_generation == source_generation)
		{
			entry.pcm = std::make_shared<const std::vector<int16_t>>(std::move(pcm));
			return;
		}
	}
}

void movie_view::retire_source_state()
{
	close_clip_session();
	close_preview_sources();
	stop_preview_audio();
	if (should_clear_movie_preview_audio_after_source_retire(true))
	{
		_preview_audio_source_generation = next_movie_preview_audio_source_generation(_preview_audio_source_generation);
		for (auto& entry : _preview_audio) entry = {};
	}
	if (_sources) _sources->clear();
	_texture_a.reset();
	_texture_b.reset();
	_drawn_a.reset();
	_drawn_b.reset();
}

ui::texture_ptr movie_view::clip_texture(ui::draw_context& dc)
{
	if (!_clip_session || _movie_state.playing != movie_view_state::playing_t::clip) return {};

	if (!_clip_texture) _clip_texture = dc.create_texture();
	if (!_clip_texture) return {};

	// A session with nothing new leaves the texture holding the frame it last gave, which is what
	// should stay on screen between frames.
	_clip_session->update_texture(_clip_texture);

	return _clip_texture->is_valid() ? _clip_texture : ui::texture_ptr{};
}

void movie_view::seek(const double time)
{
	_movie_state.playhead = std::clamp(time, 0.0, duration());

	// The user has said where to look, so the preview stops answering for the focused clip.
	_show_focus_frame = false;

	// Everything queued was mixed for where the playhead was going.
	reset_preview_audio();

	if (_movie_state.playing == movie_view_state::playing_t::clip)
	{
		_movie_state.playing = movie_view_state::playing_t::nothing;
		close_clip_session();
	}

	_state.invalidate_view(view_invalid::view_redraw);
}

void movie_view::seek_preview_only(const double source_time)
{
	_preview_override = source_time;
	_state.invalidate_view(view_invalid::view_redraw);
}

void movie_view::end_preview_override()
{
	if (!_preview_override) return;
	_preview_override.reset();
	_state.invalidate_view(view_invalid::view_redraw);
}

void movie_view::step_clip(const bool forward)
{
	const auto& project = _movie_state.project;
	if (project.is_empty()) return;

	const auto timing = project.timing();
	const auto playing = _movie_state.playing == movie_view_state::playing_t::movie;
	const auto next = movie_step_target(timing.starts, project.current(), playing, _movie_state.playhead, forward);

	select_clip(next, false, false);

	// While the movie plays, stepping is the transport's skip: playback carries on from the clip's
	// start. Parking on its first frame instead froze the picture while the sound played on.
	if (playing && next < timing.starts.size()) seek(timing.starts[next]);
}

// Picking a clip parks the preview on that clip's first kept frame and puts the playhead at its
// start. The playhead alone is not enough: at a crossfade the movie's instant at a clip's start is
// the *previous* clip at full weight, so seeking there would show everything except what was
// clicked.
void movie_view::show_focused_clip()
{
	const auto& project = _movie_state.project;
	if (project.is_empty()) return;

	const auto timing = project.timing();
	const auto index = project.current();
	if (index >= timing.starts.size()) return;

	_preview_override.reset();
	_show_focus_frame = true;
	_movie_state.playhead = timing.starts[index];
}

void movie_view::select_clip(const size_t index, const bool extend, const bool toggle)
{
	_movie_state.project.select(index, extend, toggle);
	if (_timeline) _timeline->reveal(_movie_state.project.current());
	if (should_rewind_movie_clip_after_document_focus_change(true)) rewind_clip_player();
	show_focused_clip();
	read_focused_clip();
	changed(false);
}

void movie_view::set_focus(const size_t index)
{
	_movie_state.project.focus(index);
	if (_timeline) _timeline->reveal(index);
	if (should_rewind_movie_clip_after_document_focus_change(true)) rewind_clip_player();
	show_focused_clip();
	read_focused_clip();
	changed(false);
}

// Focus changes only what the clip half of the panel describes, so only that half is rewritten.
// A movie setting the user is typing into keeps its text.
void movie_view::read_focused_clip()
{
	_movie_state.read_from_project();
	if (_controls) _controls->show_clip_values();
}

// The clip player belongs to the focused clip, so moving focus retires it. Without this a position
// left over from the last clip could land inside the new one and play it from the middle.
void movie_view::rewind_clip_player()
{
	const auto* const clip = _movie_state.project.current_clip();

	if (_movie_state.playing == movie_view_state::playing_t::clip)
	{
		_movie_state.playing = movie_view_state::playing_t::nothing;
	}

	close_clip_session();
	_movie_state.clip_playhead = rewound_movie_clip_playhead(clip);
}

void movie_view::stop_playback()
{
	if (!should_retire_movie_clip_session_for_stop(_movie_state.playing)) return;

	_movie_state.playing = movie_view_state::playing_t::nothing;
	close_clip_session();
	stop_preview_audio();
	_state.invalidate_view(view_invalid::view_redraw | view_invalid::command_state);
}

void movie_view::select_all()
{
	_movie_state.project.select_all();
	changed(false);
}

void movie_view::move_selection(const size_t to)
{
	_movie_state.project.move_selection(to);
	if (_timeline) _timeline->reveal(_movie_state.project.current());
	changed();
}

void movie_view::send_selection_to_end()
{
	move_selection(_movie_state.project.size());
}

menu_type movie_view::context_menu(const pointi loc)
{
	if (!_timeline || !_timeline->bounds.contains(loc)) return menu_type::view;

	// Right-clicking outside the selection moves it, as everywhere else: the menu must act on what
	// the user just pointed at, not on what happened to be selected before. Picking it stops playback
	// as a click does, rather than parking the picture on it while the sound plays on.
	if (const auto index = _timeline->hit(loc); index >= 0 && !_movie_state.project.is_selected(index))
	{
		stop_playback();
		select_clip(static_cast<size_t>(index), false, false);
	}

	return menu_type::movie;
}

void movie_view::changed(const bool relayout)
{
	if (_controls)
	{
		// Moving focus between a photo and a video swaps the clip controls. The panel is a window of
		// its own that no view relayout reaches, so a control shown or hidden here is positioned now,
		// or a hidden checkbox stays drawn over the trim control that replaced it.
		if (_controls->update_for_document()) _controls->frame()->layout();
		_controls->populate();
	}

	_movie_state.playhead = std::clamp(_movie_state.playhead, 0.0, duration());

	invalidate_controls();
	_state.invalidate_view(view_invalid::view_redraw | view_invalid::command_state |
		(relayout ? view_invalid::view_layout : view_invalid::none));
}

// The panel is its own window with its own frame, so invalidating the view does not repaint it.
// Without this the clip frame only caught up when a pointer happened to wander over the panel.
void movie_view::invalidate_controls() const
{
	if (_controls) _controls->frame()->invalidate();
}

void movie_view::settings_changed(const movie_edit_group group)
{
	_movie_state.project.settings(_movie_state.to_settings(), group);
	changed();
}

void movie_view::end_edit_group()
{
	_movie_state.project.end_edit_group();
}

void movie_view::hold_changed(const movie_edit_group group)
{
	auto& project = _movie_state.project;
	const auto* const clip = project.current_clip();
	if (!clip || !clip->is_photo) return;

	if (_movie_state.clip_hold_is_default)
	{
		auto updated = *clip;
		updated.photo_duration_is_default = true;
		updated.start = 0;
		updated.end = project.settings().photo_seconds;
		project.replace(project.current(), updated);
	}
	else
	{
		// Turning the default off keeps the length the photo already has. The slider that appears
		// last showed whatever this photo held before it followed the default, and the movie default
		// may have moved since.
		if (clip->photo_duration_is_default)
		{
			_movie_state.clip_hold_tenths = std::clamp(df::round(clip->duration() * 10), 1, 600);
			if (_controls) _controls->show_clip_values();
		}

		// A typed length is held to what any clip in a document may be.
		project.trim(project.current(), 0,
		             std::clamp(_movie_state.clip_hold_tenths / 10.0, movie_min_photo_seconds, movie_max_clip_seconds),
		             group);
	}

	changed();
}

void movie_view::trim_current(const double start, const double end)
{
	_preview_override.reset();
	_movie_state.project.trim(_movie_state.project.current(), start, end);
	changed();
}

void movie_view::reset_trim()
{
	auto& project = _movie_state.project;
	const auto* const clip = project.current_clip();
	if (!clip || clip->is_photo) return;

	_preview_override.reset();
	project.trim(project.current(), 0, clip->source_duration > 0 ? clip->source_duration : clip->end);
	changed();
}

void movie_view::add_items(const df::item_set& items)
{
	add_paths(media_paths(items));
}

void movie_view::add_paths(const std::vector<df::file_path>& paths)
{
	add_paths_at(paths, _movie_state.project.size());
}

void movie_view::add_paths_at(const std::vector<df::file_path>& paths, const size_t at)
{
	if (paths.empty()) return;

	auto& project = _movie_state.project;
	std::vector<movie_clip> clips;
	clips.reserve(paths.size());

	for (const auto& path : paths)
	{
		clips.emplace_back(make_movie_clip(path, project.settings()));
	}

	// One Add, or one drop, is one undo step however many files it carried.
	project.insert_many(at, clips);
	if (_timeline) _timeline->reveal(project.current());

	probe_clips();
	read_document();
	rewind_clip_player();
	changed();
}

bool movie_view::drop_paths(const std::vector<df::file_path>& paths, const pointi loc)
{
	std::vector<df::file_path> media;

	for (const auto& path : paths)
	{
		const auto* const mt = files::file_type_from_name(path);
		if (mt && (mt->group == file_group::photo || mt->group == file_group::video)) media.emplace_back(path);
	}

	if (media.empty()) return false;

	// Over the strip the drop lands where the marker showed it would; anywhere else there is no
	// position being pointed at, so it appends.
	const auto at = _timeline && _timeline->bounds.contains(loc)
		                ? static_cast<size_t>(_timeline->drop_index(loc))
		                : _movie_state.project.size();

	add_paths_at(media, at);
	return true;
}

void movie_view::drag_paths_over(const std::optional<pointi> loc)
{
	if (_timeline) _timeline->external_drag_over(loc && _timeline->bounds.contains(*loc) ? loc : std::nullopt);
}

void movie_view::remove_current()
{
	auto& project = _movie_state.project;
	if (project.is_empty()) return;

	if (should_retire_movie_clip_session_for_document_focus_change(true)) stop_playback();
	project.remove_selection();
	read_document();
	rewind_clip_player();
	changed();
}

void movie_view::undo()
{
	if (!_movie_state.project.can_undo()) return;

	if (should_retire_movie_clip_session_for_document_focus_change(true)) stop_playback();
	_movie_state.project.undo();

	// A snapshot taken before the probe answered holds clips it never measured. Left unprobed they
	// would never be ready to render, so they are measured again.
	probe_clips();
	read_document();
	changed();
}

// Probing is I/O, so it runs on a worker and comes back as detached values matched to clips by
// path. A clip whose source has moved keeps its stored times and stays marked missing.
movie_probe_result probe_movie_source(const df::file_path path)
{
	const auto attributes_before = platform::file_attributes(path);
	movie_probe_result probe;
	probe.path = path;

	const auto* const mt = files::file_type_from_name(path);
	probe.is_photo = mt && mt->group == file_group::photo;

	if (probe.is_photo)
	{
		const auto loaded = files{}.load(path, true);
		probe.found = loaded.success;
		probe.extent = upright_movie_extent(loaded.dimensions(), loaded.orientation());
	}
	else
	{
		av_format_decoder decoder;

		if (decoder.open(path, media_intent::metadata))
		{
			decoder.init_streams(-1, -1, false, true, false);

			const auto info = decoder.info();
			probe.found = info.has_video;
			probe.extent = upright_movie_extent(info.display_dimensions, info.display_orientation);
			probe.duration = std::max(0.0, info.end - info.start);
			probe.frame_rate = info.video_frame_rate;
		}
	}

	const auto attributes_after = platform::file_attributes(path);
	probe.changed_during_probe = attributes_before.exists() != attributes_after.exists() ||
		attributes_before.modified != attributes_after.modified ||
		attributes_before.size != attributes_after.size;
	return probe;
}

movie_import import_movie_maker_project(const std::string_view xml, const df::folder_path project_folder,
                                        const movie_source_finder& find)
{
	movie_import result;
	result.loaded = read_wlmp(xml, project_folder);
	if (!result.loaded) return result;

	auto& clips = result.loaded.clips;
	const auto& recorded = result.loaded.recorded_seconds;
	df::hash_map<df::file_path, size_t, df::ihash, df::ieq> source_index;

	for (size_t i = 0; i < clips.size(); ++i)
	{
		if (!source_index.try_emplace(clips[i].path, result.sources.size()).second) continue;

		movie_import_source source;
		source.stored = clips[i].path;
		source.path = clips[i].path;
		source.recorded_seconds = i < recorded.size() ? recorded[i] : 0.0;
		result.sources.emplace_back(std::move(source));
	}

	// Every probe the import makes, kept: a candidate measured to settle a choice is not opened
	// again when its clip is measured.
	df::hash_map<df::file_path, movie_probe_result, df::ihash, df::ieq> probes;

	const auto probe = [&probes](const df::file_path path) -> const movie_probe_result&
	{
		const auto known = probes.find(path);
		if (known != probes.end()) return known->second;
		return probes.emplace(path, probe_movie_source(path)).first->second;
	};

	std::vector<size_t> missing_sources;
	std::vector<df::file_path> missing;

	for (size_t i = 0; i < result.sources.size(); ++i)
	{
		if (result.sources[i].stored.exists()) continue;

		missing_sources.emplace_back(i);
		missing.emplace_back(result.sources[i].stored);
	}

	if (!missing.empty())
	{
		auto candidates = find ? find(missing) : std::vector<std::vector<df::file_path>>{};
		candidates.resize(missing.size());

		std::vector<double> lengths;
		for (const auto i : missing_sources) lengths.emplace_back(result.sources[i].recorded_seconds);

		const auto measure = [&probe](const df::file_path path)
		{
			const auto& measured = probe(path);
			return measured.found && !measured.is_photo ? measured.duration : 0.0;
		};

		const auto choices = choose_movie_sources(missing, candidates, lengths, measure);

		for (size_t j = 0; j < missing.size(); ++j)
		{
			auto& source = result.sources[missing_sources[j]];
			source.match = choices[j].match;
			if (!choices[j].path.is_empty()) source.path = choices[j].path;

			for (const auto& candidate : candidates[j])
			{
				const auto measured = probes.find(candidate);
				const auto seconds = measured != probes.end() && measured->second.found ? measured->second.duration : 0.0;
				source.candidates.emplace_back(candidate, seconds);
			}
		}
	}

	for (auto& source : result.sources) source.probe = probe(source.path);

	for (auto& clip : clips)
	{
		const auto& source = result.sources[source_index.at(clip.path)];

		if (source.path != source.stored)
		{
			clip.path = source.path;
			++result.relinked_clips;
		}

		// A reading taken while the file changed may already be wrong, so that clip is left for the
		// caller's own probe, which retries a moving source.
		if (!source.probe.changed_during_probe)
		{
			clip = apply_movie_probe(clip, source.probe, result.loaded.settings.photo_seconds);
		}

		// A clip that runs to the end of a source nothing could measure keeps the length the project
		// recorded. Left at zero, the timeline would show it as nothing and a saved project would
		// refuse it on reading, losing the clip a relink could still restore.
		if (clip.is_lost() && !clip.is_photo && clip.duration() <= 0 && source.recorded_seconds > clip.start)
		{
			clip.end = source.recorded_seconds;
			clip.source_duration = source.recorded_seconds;
		}
	}

	return result;
}

void movie_view::probe_clips()
{
	std::vector<df::file_path> wanted;

	for (const auto& clip : _movie_state.project.clips())
	{
		if (clip.is_probed) continue;
		if (std::find(wanted.begin(), wanted.end(), clip.path) == wanted.end()) wanted.emplace_back(clip.path);
	}

	if (wanted.empty()) return;

	const auto weak = weak_from_this();
	const auto generation = ++_probe_generation;
	const auto revision = _movie_state.project.revision();

	_state.queue_async(async_queue::load, [weak, wanted, generation, revision, &s = _state]
	{
		std::vector<movie_probe_result> results;
		results.reserve(wanted.size());

		for (const auto& path : wanted)
		{
			results.emplace_back(probe_movie_source(path));
		}

		s.queue_ui([weak, generation, revision, results = std::move(results)]
		{
			const auto self = weak.lock();
			if (!self || generation != self->_probe_generation) return;
			if (revision != self->_movie_state.project.revision())
			{
				self->probe_clips();
				return;
			}
			self->apply_probe(results);
		});
	});
}

void movie_view::apply_probe(const std::vector<movie_probe_result>& results)
{
	auto& project = _movie_state.project;
	auto changed_any = false;
	auto retry_any = false;
	constexpr auto max_probe_retries = 2;
	auto retry_updates = decide_movie_changed_probe_retries(results, _probe_retries, max_probe_retries);

	for (size_t i = 0; i < project.size(); ++i)
	{
		const auto& clip = project.clips()[i];
		if (clip.is_probed) continue;

		const auto found = std::find_if(results.begin(), results.end(),
		                                [&](const movie_probe_result& r) { return r.path == clip.path; });

		if (found == results.end()) continue;

		auto updated = clip;

		if (found->changed_during_probe)
		{
			if (movie_path_list_contains(retry_updates.retry_paths, clip.path))
			{
				retry_any = true;
				continue;
			}
			if (!movie_path_list_contains(retry_updates.missing_paths, clip.path)) continue;

			updated.is_probed = true;
			updated.is_missing = true;
			project.replace_quietly(i, updated);
			changed_any = true;
			continue;
		}

		_probe_retries.erase(clip.path);
		project.replace_quietly(i, apply_movie_probe(clip, *found, project.settings().photo_seconds));
		changed_any = true;
	}

	if (should_erase_movie_probe_retry_after_missing(!retry_updates.missing_paths.empty()))
	{
		for (const auto& path : retry_updates.missing_paths) _probe_retries.erase(path);
	}

	if (changed_any)
	{
		// A probe can only have measured clips, so only the clip half of the panel can have moved.
		read_focused_clip();
		changed();
	}

	if (retry_any) probe_clips();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Project files
///////////////////////////////////////////////////////////////////////////////////////////////////

std::vector<platform::file_dialog_filter> movie_project_filters()
{
	return {
		{std::string(tt.movie_project_files.sv()), "*.otio"},
	};
}

namespace
{
	std::string read_text_file(const df::file_path path)
	{
		const auto file = platform::open_file(path, platform::file_open_mode::read);
		if (!file) return {};

		const auto size = file->size();

		// A project file is a document, not a stream. A refusal here is better than reading a
		// gigabyte of something that was never a project.
		constexpr uint64_t max_project_bytes = 32 * 1024 * 1024;
		if (size == 0 || size > max_project_bytes) return {};

		std::string result(static_cast<size_t>(size), '\0');
		const auto read = file->read(std::bit_cast<uint8_t*>(result.data()), size);
		result.resize(static_cast<size_t>(read));
		return result;
	}

	// The collection files that carry each missing source's name. Answered from the index rather
	// than the disk: it already lists every member, so the search is one pass over memory instead of
	// a walk of the collection. Runs on the import's worker; the index publishes immutable folder
	// snapshots for readers on any thread.
	std::vector<std::vector<df::file_path>> find_in_collection(const index_state& index,
	                                                           const std::vector<df::file_path>& missing)
	{
		df::hash_map<std::string, std::vector<size_t>, df::ihash, df::ieq> wanted;
		for (size_t i = 0; i < missing.size(); ++i) wanted[std::string(missing[i].name())].emplace_back(i);

		std::vector<std::vector<df::file_path>> result(missing.size());

		for (const auto& path : index.all_indexed_items())
		{
			const auto found = wanted.find(std::string(path.name()));
			if (found == wanted.end()) continue;

			for (const auto i : found->second) result[i].emplace_back(path);
		}

		return result;
	}
}

std::string write_movie_project_file(const df::file_path path, const std::string_view text,
                                    const bool fail_if_exists)
{
	const auto staged = platform::temp_file(".otio", path.folder());
	const auto cleanup = df::scope_exit([&staged] { platform::delete_file(staged); });

	if (!df::blob_save_to_file({std::bit_cast<const uint8_t*>(text.data()), text.size()}, staged))
	{
		return "could not write the staged project";
	}

	const auto moved = fail_if_exists
		                   ? platform::move_file(staged, path, true)
		                   : platform::replace_file(path, staged);
	return moved.failed() ? moved.format_error() : std::string{};
}

void movie_view::load_project(const df::file_path path, const bool is_wlmp)
{
	if (_project_io_active) return;

	const auto weak = weak_from_this();
	if (!confirm_save_or_discard([weak, path, is_wlmp]
	{
		if (const auto self = weak.lock()) self->load_project(path, is_wlmp);
	})) return;

	_project_io_active = true;
	const auto generation = ++_project_io_generation;
	const auto revision = _movie_state.project.edit_revision();
	_state.invalidate_view(view_invalid::command_state);

	// Importing measures every source, which for a long Movie Maker project takes seconds, so the
	// wait is said rather than shown as nothing. A render's own status is left alone.
	if (is_wlmp && !_progress.active)
	{
		_status = std::string(tt.loading.sv());
		_state.invalidate_view(view_invalid::status);
	}

	_state.queue_async(async_queue::load,
	                   [weak, path, is_wlmp, generation, revision, &index = _state.item_index, &s = _state]
	{
		movie_load_result loaded;
		auto relinked = 0;

		try
		{
			const auto text = read_text_file(path);

			if (!text.empty() && is_wlmp)
			{
				const auto find = [&index](const std::vector<df::file_path>& missing)
				{
					return find_in_collection(index, missing);
				};

				auto imported = import_movie_maker_project(text, path.folder(), find);
				relinked = imported.relinked_clips;
				loaded = std::move(imported.loaded);
			}
			else if (!text.empty())
			{
				loaded = read_otio(text, path.folder());
			}
		}
		catch (const std::exception& e)
		{
			df::log(__FUNCTION__, e.what());
		}
		catch (...)
		{
			df::log(__FUNCTION__, "project read failed");
		}

		s.queue_ui([weak, path, is_wlmp, generation, revision, relinked, loaded = std::move(loaded)]() mutable
		{
			const auto self = weak.lock();
			if (!self || generation != self->_project_io_generation) return;

			self->_project_io_active = false;
			self->_state.invalidate_view(view_invalid::command_state);

			if (!self->_progress.active && self->_status == tt.loading.sv())
			{
				self->_status.clear();
				self->_state.invalidate_view(view_invalid::status);
			}

			if (!loaded)
			{
				make_dlg(self->_host->owner())->show_message(
					icon_index::error, tt.movie_title,
					str_format(tt.movie_open_failed_fmt.sv(), path.name().sv()));
				return;
			}

			auto result = std::make_shared<movie_load_result>(std::move(loaded));
			const auto apply = [weak, path, is_wlmp, relinked, result]
			{
				const auto current = weak.lock();
				if (!current) return;

				// A Movie Maker project is imported, not opened: saving it proposes an OTIO sibling.
				const auto project_path = is_wlmp ? path.extension(".otio") : path;
				if (should_retire_movie_sources_after_project_change(true))
				{
					current->stop_playback();
					current->retire_source_state();
				}
				current->_movie_state.project.reset(std::move(result->clips), result->settings, project_path);
				current->_movie_state.import_ignored = result->ignored_elements;
				current->_movie_state.import_relinked = relinked;
				current->_probe_retries.clear();
				current->read_document();
				// An import arrives measured; this probes what it could not, and everything an opened
				// project names.
				current->probe_clips();
				if (current->_timeline) current->_timeline->reveal(0);
				if (should_rewind_movie_clip_after_document_focus_change(true)) current->rewind_clip_player();
				current->seek(0);
				current->changed();
			};

			if (revision != self->_movie_state.project.edit_revision())
			{
				if (self->confirm_save_or_discard(apply)) apply();
				return;
			}

			apply();
		});
	});
}

void movie_view::open_project()
{
	std::vector<df::file_path> paths;
	if (!platform::prompt_for_open_paths(paths, movie_project_filters(), false) || paths.empty()) return;

	load_project(paths.front(), false);
}

void movie_view::import_project()
{
	std::vector<df::file_path> paths;

	const std::vector<platform::file_dialog_filter> filters{
		{std::string(tt.movie_maker_files.sv()), "*.wlmp"}
	};

	if (!platform::prompt_for_open_paths(paths, filters, false) || paths.empty()) return;

	load_project(paths.front(), true);
}

void movie_view::save_project()
{
	save_project({});
}

void movie_view::save_project(std::function<void(bool)> complete)
{
	if (_project_io_active)
	{
		if (complete) complete(false);
		return;
	}

	auto path = _movie_state.project.path();

	if (path.is_empty())
	{
		path = df::folder_path(platform::known_path(platform::known_folder::video)).combine_file_ext(
			std::string(tt.movie_title.sv()), ".otio");
	}

	if (!platform::prompt_for_save_path(path, movie_project_filters()))
	{
		if (complete) complete(false);
		return;
	}

	const auto revision = _movie_state.project.edit_revision();
	auto json = write_otio(_movie_state.project.clips(), _movie_state.project.settings(), path.folder());
	const auto generation = ++_project_io_generation;
	const auto weak = weak_from_this();
	_project_save_complete = std::move(complete);
	_project_io_active = true;
	_state.invalidate_view(view_invalid::command_state);

	// Not the work queue: a render holds that one thread for its whole length, and a save queued
	// behind it would leave Save, Open and leaving Movie disabled until the render ended.
	_state.queue_async(async_queue::load,
	                   [weak, path, revision, generation, json = std::move(json), &s = _state]() mutable
	{
		std::string error;

		try
		{
			error = write_movie_project_file(path, json);
		}
		catch (const std::exception& e)
		{
			error = e.what();
		}
		catch (...)
		{
			error = "project write failed";
		}

		s.queue_ui([weak, path, revision, generation, error = std::move(error)]() mutable
		{
			const auto self = weak.lock();
			if (!self || generation != self->_project_io_generation) return;

			self->_project_io_active = false;
			self->_state.invalidate_view(view_invalid::command_state);
			const auto saved = error.empty();
			auto document_clean = false;

			if (saved)
			{
				self->_movie_state.project.mark_saved(path, revision);
				document_clean = !self->_movie_state.project.is_modified();
				self->changed(false);
			}
			else
			{
				make_dlg(self->_host->owner())->show_message(
					icon_index::error, tt.movie_title,
					str_format(tt.movie_save_failed_fmt.sv(), path.name().sv()));
			}

			auto complete = std::move(self->_project_save_complete);
			if (complete) complete(saved && document_clean);
		});
	});
}

void movie_view::add_files()
{
	std::vector<df::file_path> paths;

	const std::vector<platform::file_dialog_filter> filters{
		{std::string(tt.movie_media_files.sv()), "*.mp4;*.mov;*.avi;*.mkv;*.m4v;*.jpg;*.jpeg;*.png;*.heic;*.webp"}
	};

	if (!platform::prompt_for_open_paths(paths, filters, true)) return;

	add_paths(paths);
}

bool movie_view::has_missing_clips() const
{
	const auto& clips = _movie_state.project.clips();
	return std::any_of(clips.begin(), clips.end(), [](const movie_clip& c) { return c.is_lost(); });
}

void movie_view::relink_missing()
{
	if (!has_missing_clips() || _project_io_active) return;

	df::folder_path folder;
	if (!platform::browse_for_folder(folder) || folder.is_empty()) return;

	struct relink_candidate
	{
		size_t index = 0;
		df::file_path original;
		df::file_path candidate;
	};

	std::vector<relink_candidate> candidates;
	const auto& project = _movie_state.project;

	for (size_t i = 0; i < project.size(); ++i)
	{
		const auto& clip = project.clips()[i];
		if (!clip.is_lost()) continue;

		candidates.emplace_back(relink_candidate{i, clip.path, folder.combine_file(clip.path.name())});
	}

	_project_io_active = true;
	const auto generation = ++_project_io_generation;
	const auto weak = weak_from_this();
	_state.invalidate_view(view_invalid::command_state);

	_state.queue_async(async_queue::load,
	                   [weak, generation, candidates = std::move(candidates), &s = _state]() mutable
	{
		auto failed = false;

		try
		{
			std::erase_if(candidates,
			              [](const relink_candidate& candidate) { return !candidate.candidate.exists(); });
		}
		catch (const std::exception& e)
		{
			df::log(__FUNCTION__, e.what());
			failed = true;
		}
		catch (...)
		{
			df::log(__FUNCTION__, "relink probe failed");
			failed = true;
		}

		s.queue_ui([weak, generation, failed, candidates = std::move(candidates)]() mutable
		{
			const auto self = weak.lock();
			if (!self || generation != self->_project_io_generation) return;

			self->_project_io_active = false;
			self->_state.invalidate_view(view_invalid::command_state);
			if (failed)
			{
				make_dlg(self->_host->owner())->show_message(icon_index::error, tt.movie_title, tt.platform_text.error_unknown);
				return;
			}

			auto& project = self->_movie_state.project;
			std::vector<std::pair<size_t, movie_clip>> replacements;

			for (const auto& candidate : candidates)
			{
				if (candidate.index >= project.size()) continue;
				const auto& clip = project.clips()[candidate.index];
				if (!clip.is_lost() || clip.path != candidate.original) continue;

				auto updated = clip;
				updated.path = candidate.candidate;
				updated.is_probed = false;
				replacements.emplace_back(candidate.index, std::move(updated));
			}

			if (replacements.empty())
			{
				make_dlg(self->_host->owner())->show_message(icon_index::error, tt.movie_title,
				                                             tt.movie_relink_none);
				return;
			}

			project.replace_many(replacements);
			if (should_retire_movie_sources_after_project_change(true))
			{
				self->stop_playback();
				self->retire_source_state();
			}
			self->_probe_retries.clear();
			self->probe_clips();
			self->read_document();
			if (should_rewind_movie_clip_after_document_focus_change(true)) self->rewind_clip_player();
			self->changed();
		});
	});
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The render
//
// The preview is the render at display rate, so this runs the same decision -- calc_movie_frame --
// with the same weights and the same fit, at output resolution, and hands the result to the
// platform's encoder. It touches no Direct3D device: a driver update or a TDR mid-render would
// otherwise destroy an hour of work, and docs/movie.md#7-rendering is explicit that the display device is
// not the render's to use.
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	// One per clip a crossfade can hold open. The half-clip ceiling in movie_overlap stops a third
	// reaching across a short clip, so two is the whole of it.
	constexpr size_t render_slot_count = 2;

	// The decoders and audio buffers the render keeps open. Worker-owned and shared with nothing:
	// the request is a detached value, and so is everything opened from it.
	class movie_render_sources
	{
	public:
		movie_render_sources(const int frame_rate, df::cancel_token abandon) :
			_abandon(std::move(abandon)), _frame_rate(std::max(1, frame_rate))
		{
		}

		// The frame `clip` contributes at `source_time`, decoded forward from where this slot's
		// decoder already sits. The render walks the movie in order, so every request but the first
		// of a clip is a step forward and costs no seek.
		ui::const_surface_ptr frame(const size_t slot, const movie_clip& clip, const double source_time,
		                            const sizei max_dim)
		{
			if (slot >= _slots.size()) return {};

			auto& state = _slots[slot];
			if (state.path != clip.path) open(state, clip, max_dim);

			if (clip.is_photo) return state.photo;
			if (!state.decoder) return {};

			ui::surface_ptr surface;

			// Half a frame of slack: refining past that costs another walk of the group of pictures
			// for a picture that is the same one.
			return state.decoder->extract_frame_at(surface, max_dim, source_time, 0.5 / _frame_rate, _abandon)
			       ? upright_movie_frame(surface)
			       : ui::const_surface_ptr{};
		}

		// One bounded source-time chunk at the output rate. Walking into the next chunk replaces it,
		// so long clips keep complete audio without retaining their whole soundtrack.
		const std::vector<int16_t>& audio(const size_t slot, const movie_clip& clip, const int sample_rate,
		                                  const double source_time, double& buffer_start)
		{
			static const std::vector<int16_t> none;
			if (slot >= _slots.size()) return none;

			auto& state = _slots[slot];
			if (state.path != clip.path) open(state, clip, {});
			const auto offset = std::max(0.0, source_time - clip.start);
			const auto start = clip.start + std::floor(offset / movie_audio_cache_seconds) * movie_audio_cache_seconds;
			const auto end = std::min(clip.end, start + movie_audio_cache_seconds + movie_audio_overlap_seconds);
			buffer_start = start;

			if (!state.audio_read || state.audio_start != start || state.audio_end != end)
			{
				state.audio_read = true;
				state.audio_start = start;
				state.audio_end = end;
				state.pcm.clear();

				if (!clip.is_photo && end > start)
				{
					// Its own decoder: this one reads the stream end to end, and the picture's is
					// opened video-only and positioned wherever the playhead left it.
					av_format_decoder decoder;

					if (decoder.open(clip.path, media_intent::playback))
					{
						decoder.init_streams(-1, -1, false, false, false);
						state.pcm = decoder.extract_audio_pcm_range(sample_rate, start, end - start, _abandon);
					}
				}
			}

			return state.pcm;
		}

	private:
		struct slot_state
		{
			df::file_path path;
			std::unique_ptr<av_format_decoder> decoder;
			ui::const_surface_ptr photo;
			std::vector<int16_t> pcm;
			bool audio_read = false;
			double audio_start = 0;
			double audio_end = 0;
		};

		void open(slot_state& state, const movie_clip& clip, const sizei max_dim)
		{
			state = {};
			state.path = clip.path;

			if (clip.is_photo)
			{
				// A photo is one frame however long it is held, so it is loaded once and reused for
				// every output frame it covers.
				if (max_dim.cx <= 0) return;
				const auto loaded = files{}.load(clip.path, false);
				if (loaded.success) state.photo = upright_movie_frame(loaded.to_surface(max_dim));
				return;
			}

			auto opened = std::make_unique<av_format_decoder>();
			if (!opened->open(clip.path, media_intent::thumbnail)) return;

			opened->init_streams(-1, -1, false, true, true);
			if (opened->has_video()) state.decoder = std::move(opened);
		}

		std::array<slot_state, render_slot_count> _slots;
		df::cancel_token _abandon;
		int _frame_rate = movie_default_frame_rate;
	};
	// Adds one clip's picture into the canvas at `weight`. The canvas starts black and a frame's
	// weights sum to one, so adding is the whole of the mix: a single contributor lands unchanged,
	// and the letterbox stays black because black is what it is added to.
	void add_weighted(ui::surface& canvas, const ui::surface& src, const pointi at, const double weight)
	{
		const auto scaled = static_cast<uint32_t>(std::lround(std::clamp(weight, 0.0, 1.0) * 256.0));
		if (scaled == 0) return;

		const auto canvas_cx = static_cast<int>(canvas.width());
		const auto canvas_cy = static_cast<int>(canvas.height());
		const auto src_cx = static_cast<int>(src.width());
		const auto src_cy = static_cast<int>(src.height());

		for (auto y = 0; y < src_cy; ++y)
		{
			const auto dst_y = at.y + y;
			if (dst_y < 0 || dst_y >= canvas_cy) continue;

			const auto* const s = std::bit_cast<const uint32_t*>(src.pixels_line(y));
			auto* const d = std::bit_cast<uint32_t*>(canvas.pixels_line(dst_y));

			const auto first = std::max(0, -at.x);
			const auto last = std::min(src_cx, canvas_cx - at.x);

			for (auto x = first; x < last; ++x)
			{
				const auto in = s[x];
				const auto out = d[at.x + x];

				uint32_t result = 0xff000000u;

				for (auto shift = 0; shift < 24; shift += 8)
				{
					const auto v = ((out >> shift) & 0xffu) + ((((in >> shift) & 0xffu) * scaled) >> 8);
					result |= std::min(v, 255u) << shift;
				}

				d[at.x + x] = result;
			}
		}
	}

	// The movie's own fade to black, applied to the finished composite rather than drawn over it, so
	// what reaches the encoder is the pixels the preview showed.
	void darken(ui::surface& canvas, const double fade)
	{
		const auto keep = static_cast<uint32_t>(std::lround(std::clamp(1.0 - fade, 0.0, 1.0) * 256.0));
		if (keep >= 256) return;

		const auto cx = static_cast<int>(canvas.width());

		for (auto y = 0; y < static_cast<int>(canvas.height()); ++y)
		{
			auto* const d = std::bit_cast<uint32_t*>(canvas.pixels_line(y));

			for (auto x = 0; x < cx; ++x)
			{
				const auto in = d[x];
				uint32_t result = 0xff000000u;

				for (auto shift = 0; shift < 24; shift += 8)
				{
					result |= ((((in >> shift) & 0xffu) * keep) >> 8) << shift;
				}

				d[x] = result;
			}
		}
	}

	// Adds one clip's samples into the chunk at the same weight its picture is drawn with. Reading
	// past the end of a buffer is silence, which is what a clip whose audio is shorter than its
	// picture contributes.
	void add_weighted_audio(std::vector<int16_t>& chunk, const std::vector<int16_t>& pcm, const double source_time,
	                        const int sample_rate, const double weight)
	{
		if (pcm.empty() || weight <= 0.0) return;

		const auto first = static_cast<int64_t>(std::llround(source_time * sample_rate)) * 2;

		for (size_t i = 0; i < chunk.size(); ++i)
		{
			const auto at = first + static_cast<int64_t>(i);
			if (at < 0 || at >= static_cast<int64_t>(pcm.size())) continue;

			const auto mixed = chunk[i] + static_cast<int32_t>(std::lround(pcm[static_cast<size_t>(at)] * weight));
			chunk[i] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
		}
	}

	// Where a source lands on the canvas: fitted, never stretched, letterboxed on black. The rule the
	// preview draws by, so the two cannot disagree about what a mixed-aspect movie looks like.
	recti fit_into(const sizei source, const sizei canvas)
	{
		const auto fitted = ui::scale_dimensions(source, canvas, false);
		return recti(fitted).offset(recti(canvas).center() - recti(fitted).center());
	}
}

movie_render_result render_movie_to_file(const movie_render_request& request,
                                         const std::function<void(double)>& report,
                                         const df::cancel_token& token,
                                         const std::shared_ptr<movie_render_control>& control)
{
	movie_render_result result;

	const auto timing = calc_movie_timing(request.clips, request.settings);
	const auto frame_rate = std::max(1, request.output.frame_rate);
	const auto sample_rate = std::max(8000, request.output.sample_rate);

	if (timing.duration <= 0 || request.output.extent.cx <= 0 || request.output.extent.cy <= 0)
	{
		result.reason = movie_render_error::no_length;
		return result;
	}

	// The output is replaced only after the last clip has been read, so an output that names a
	// clip would consume that source without the timeline ever showing it gone. Refused here,
	// where the replace is owned, rather than trusted to whichever caller chose the path.
	if (movie_output_names_a_clip(request.clips, request.path))
	{
		result.reason = movie_render_error::output_is_source;
		return result;
	}

	// Staged beside the destination and moved into place on success, the rule docs/file-io.md
	// already owns: a cancelled or failed render leaves no partial file behind.
	const auto staged = platform::temp_file(".mp4", request.path.folder());

	platform::movie_writer_request writer_request;
	writer_request.path = staged;
	writer_request.extent = request.output.extent;
	writer_request.frame_rate = frame_rate;
	writer_request.video_bitrate = request.output.video_bitrate;
	writer_request.sample_rate = sample_rate;
	writer_request.channels = request.output.channels;
	writer_request.audio_bitrate = request.output.audio_bitrate;
	writer_request.with_audio = std::any_of(request.clips.begin(), request.clips.end(),
	                                        [](const movie_clip& c) { return !c.is_photo && !c.is_missing; });

	const auto writer = platform::create_movie_writer(writer_request);

	if (!writer)
	{
		result.reason = movie_render_error::no_encoder;
		return result;
	}

	movie_render_sources sources(frame_rate, token);

	av_scaler scaler;

	const auto canvas = std::make_shared<ui::surface>();
	if (!canvas->alloc(request.output.extent, ui::texture_format::RGB))
	{
		result.reason = movie_render_error::no_memory;
		writer->abandon();
		return result;
	}

	ui::surface_ptr fitted;
	std::vector<int16_t> chunk;

	const auto frames = std::ceil(timing.duration * frame_rate);

	// The document bounds its own durations, but a frame count that does not fit the arithmetic
	// that walks it is not a movie to attempt: the conversion below would be undefined.
	if (!std::isfinite(frames) || frames > static_cast<double>(movie_max_frames))
	{
		result.reason = movie_render_error::no_length;
		return result;
	}

	const auto frame_count = std::max<int64_t>(1, static_cast<int64_t>(frames));
	int64_t samples_written = 0;

	for (int64_t i = 0; i < frame_count; ++i)
	{
		if (token.is_cancelled())
		{
			writer->abandon();
			result.cancelled = true;
			return result;
		}

		const auto time = static_cast<double>(i) / frame_rate;
		const auto frame = calc_movie_frame(request.clips, request.settings, timing, time);

		canvas->make_blank();

		const auto draw = [&](const movie_frame_source& source, const size_t slot)
		{
			if (source.index < 0 || source.weight <= 0.0) return true;

			const auto& clip = request.clips[source.index];
			if (clip.is_missing) return false;

			// Square, on the output's longer side: the limit applies to the stored frame, and a quarter-
			// turned source is stored the other way round, so an output-shaped limit would decode it at
			// a fraction of the size it is drawn and stretch it back up.
			const auto decode_side = std::max(request.output.extent.cx, request.output.extent.cy);
			const auto surface = sources.frame(slot, clip, source.source_time, {decode_side, decode_side});
			if (!is_valid(surface)) return false;

			const auto target = fit_into(surface->dimensions(), request.output.extent);
			if (target.width() <= 0 || target.height() <= 0) return false;
			if (!scaler.scale_surface(surface, fitted, target.extent())) return false;

			add_weighted(*canvas, *fitted, target.top_left(), source.weight);
			return true;
		};

		const movie_frame_source* failed = nullptr;
		if (!draw(frame.a, 0)) failed = &frame.a;
		else if (!draw(frame.b, 1)) failed = &frame.b;

		if (failed)
		{
			writer->abandon();

			if (token.is_cancelled())
			{
				result.cancelled = true;
			}
			else
			{
				result.reason = movie_render_error::undecodable;
				result.source = request.clips[failed->index].path;
				result.source_time = failed->source_time;
				df::log(__FUNCTION__, std::format("no frame from {} at {:.3f}s", result.source, result.source_time));
			}

			return result;
		}

		if (frame.fade_to_black > 0) darken(*canvas, frame.fade_to_black);

		if (!writer->write_frame(canvas, time))
		{
			df::log(__FUNCTION__, writer->last_error());
			result.reason = movie_render_error::encoder_failed;
			writer->abandon();
			return result;
		}

		if (writer_request.with_audio)
		{
			// Counted from the sample the last chunk ended on rather than from the frame index,
			// so a rate that does not divide the frame rate cannot drift the sound off the
			// picture over a long movie.
			const auto next = static_cast<int64_t>(
				std::llround(static_cast<double>(i + 1) / frame_rate * sample_rate));
			const auto frames = std::max<int64_t>(0, next - samples_written);

			chunk.assign(static_cast<size_t>(frames) * 2, 0);

			const auto mix = [&](const movie_frame_source& source, const size_t slot)
			{
				if (source.index < 0 || source.weight <= 0.0) return;

				const auto& clip = request.clips[source.index];
				if (clip.is_photo || clip.is_missing) return;

				// The fade at the movie's ends is a fade of the movie, not of its picture: an
				// image fading to black over a soundtrack still at full level is the thing
				// nobody means by "fade out".
				auto buffer_start = source.source_time;
				const auto& pcm = sources.audio(slot, clip, sample_rate, source.source_time, buffer_start);
				add_weighted_audio(chunk, pcm, source.source_time - buffer_start,
				                   sample_rate, source.weight * (1.0 - frame.fade_to_black));
			};

			mix(frame.a, 0);
			mix(frame.b, 1);

			if (!chunk.empty() && !writer->write_audio(chunk.data(), chunk.size(),
			                                           static_cast<double>(samples_written) / sample_rate))
			{
				df::log(__FUNCTION__, writer->last_error());
				result.reason = movie_render_error::encoder_failed;
				writer->abandon();
				return result;
			}

			samples_written = next;
		}

		report(static_cast<double>(i + 1) / static_cast<double>(frame_count));
	}

	if (!writer->close())
	{
		df::log(__FUNCTION__, writer->last_error());
		result.reason = movie_render_error::encoder_failed;
		writer->abandon();
		return result;
	}

	// Cancel and this replace decide each other here: only one of them can claim the render, so a
	// cancel that lands while the encoder was closing discards the file, and one that lands after
	// this point leaves the movie the user already has.
	if (!control || !control->claim_publication())
	{
		platform::delete_file(staged);
		result.cancelled = true;
		return result;
	}

	const auto moved = request.fail_if_exists
		                   ? platform::move_file(staged, request.path, true)
		                   : platform::replace_file(request.path, staged);

	if (moved.failed())
	{
		platform::delete_file(staged);
		// Already worded by the operating system in the user's own language.
		result.message = moved.format_error();
		return result;
	}

	result.ok = true;
	return result;
}

void movie_view::render_movie()
{
	if (!can_render()) return;

	const auto& project = _movie_state.project;

	auto path = project.path().is_empty()
		            ? df::folder_path(platform::known_path(platform::known_folder::video)).combine_file_ext(
			            std::string(tt.movie_title.sv()), ".mp4")
		            : project.path().folder().combine_file_ext(
			            std::string(project.path().file_name_without_extension()), ".mp4");

	const std::vector<platform::file_dialog_filter> filters{
		{std::string(tt.movie_video_files.sv()), "*.mp4"}
	};

	if (!platform::prompt_for_save_path(path, filters)) return;

	// Playing and rendering are two readers of the same files at once, and the render is the one
	// that matters.
	stop_playback();

	movie_render_request request;
	request.clips = project.clips();
	request.settings = project.settings();
	request.output = project.output();
	request.path = path;

	++_render_generation;
	_render_control = std::make_shared<movie_render_control>();
	_progress = {true, 0, 100};
	_status = std::string(tt.movie_rendering.sv());

	const auto generation = _render_generation;
	const auto weak = weak_from_this();
	const auto control = _render_control;
	const auto token = df::cancel_token(control->cancelled);

	_state.invalidate_view(view_invalid::status | view_invalid::command_state | view_invalid::app_layout);

	_state.queue_async(async_queue::work, [weak, request, token, control, generation, &s = _state]
	{
		// Coalesced to whole percent: a queued UI message per frame is tens a second of work the UI
		// thread has no use for.
		auto last_percent = -1;

		const auto report = [&](const double fraction)
		{
			const auto percent = std::clamp(static_cast<int>(fraction * 100), 0, 100);
			if (percent == last_percent) return;
			last_percent = percent;

			s.queue_ui([weak, generation, percent]
			{
				// The weak pointer is a lifetime token only; it is locked here, on the thread that
				// owns the view, and nowhere else.
				if (const auto self = weak.lock()) self->render_progress(generation, percent);
			});
		};

		movie_render_result result;

		try
		{
			result = render_movie_to_file(request, report, token, control);
		}
		catch (const std::exception& e)
		{
			// A codec that throws is a bad file, not a broken application: it stops this run and
			// reports why.
			df::log(__FUNCTION__, str::utf8_cast(e.what()));
			result.reason = movie_render_error::undecodable;
		}

		s.queue_ui([weak, generation, result, name = request.path.name()]
		{
			if (const auto self = weak.lock())
			{
				self->render_finished(generation, result.ok, result.cancelled, result.reason, result.message, name);
			}
		});
	});
}

void movie_view::render_progress(const size_t generation, const int percent)
{
	if (generation != _render_generation || !_progress.active) return;

	_progress.position = percent;
	_state.invalidate_view(view_invalid::status);
}

void movie_view::render_finished(const size_t generation, const bool ok, const bool cancelled,
                                 const movie_render_error reason, const std::string& message,
                                 const str::cached name)
{
	if (generation != _render_generation) return;

	_progress = {};
	_render_control.reset();

	const auto why = [reason, &message]() -> std::string_view
	{
		switch (reason)
		{
		case movie_render_error::no_length: return tt.movie_error_no_length.sv();
		case movie_render_error::no_encoder: return tt.movie_error_no_encoder.sv();
		case movie_render_error::no_memory: return tt.movie_error_no_memory.sv();
		case movie_render_error::undecodable: return tt.movie_error_undecodable.sv();
		case movie_render_error::encoder_failed: return tt.movie_error_encoder_failed.sv();
		case movie_render_error::output_is_source: return tt.movie_error_output_is_source.sv();
		case movie_render_error::none: break;
		}

		return message.empty() ? tt.platform_text.error_unknown.sv() : std::string_view(message);
	}();

	_status = cancelled
		          ? std::string(tt.movie_render_cancelled.sv())
		          : ok
		          ? str_format(tt.movie_rendered_fmt.sv(), name.sv())
		          : str_format(tt.movie_render_failed_fmt.sv(), why);

	_state.invalidate_view(view_invalid::status | view_invalid::command_state | view_invalid::app_layout);
}

void movie_view::cancel_operation()
{
	if (_render_control)
	{
		_render_control->cancel();
	}
}

std::string_view movie_view::operation_name() const
{
	return tt.command_movie_render.sv();
}

std::string_view movie_view::status()
{
	return _status;
}

view_base::progress_state movie_view::progress() const
{
	return _progress;
}

// Quitting while Movie is open asks what leaving it asks. Quitting from another view finds nothing
// to ask about, because leaving Movie already settled the timeline.
bool movie_view::confirm_exit()
{
	if (!confirm_render_cancel()) return false;

	const auto weak = weak_from_this();
	return confirm_save_or_discard([weak]
	{
		if (const auto self = weak.lock()) self->_host->owner()->close();
	});
}

// A render is asked about before the timeline: it is running work, and the save question is about
// work that is only sitting there. Answers whether the caller may go on.
bool movie_view::confirm_render_cancel()
{
	if (!_progress.active) return true;

	const auto busy = make_dlg(_host->owner());

	const std::vector<view_element_ptr> controls = {
		set_margin(std::make_shared<ui::title_control2>(busy->_frame, icon_index::question,
		                                               tt.cancel_operation_title,
		                                               str_format(tt.cancel_operation_fmt.sv(),
		                                                          operation_name()))),
		std::make_shared<divider_element>(),
		std::make_shared<ui::ok_cancel_control>(busy->_frame, tt.button_cancel_operation,
		                                       tt.button_keep_running),
	};

	if (busy->show_modal(controls, {44}, {33}) != ui::close_result::ok) return false;

	cancel_operation();
	return true;
}

bool movie_view::has_unsaved_changes() const
{
	return _movie_state.project.is_modified();
}

bool movie_view::can_save_project() const
{
	return has_clips() || has_unsaved_changes();
}

bool movie_view::confirm_save_or_discard(std::function<void()> after_save)
{
	if (!has_unsaved_changes()) return true;

	enum class exit_choice
	{
		cancel,
		save,
		discard,
	};

	auto answer = std::make_shared<exit_choice>(exit_choice::cancel);
	const auto dlg = make_dlg(_host->owner());
	const auto frame = dlg->_frame;

	const std::vector<view_element_ptr> controls = {
		set_margin(std::make_shared<ui::title_control2>(frame, icon_index::video, tt.movie_title,
		                                               tt.movie_unsaved_prompt)),
		std::make_shared<divider_element>(),
		std::make_shared<ui::button_control>(frame, icon_index::save, tt.command_movie_save, std::string_view{},
		                                     [answer, frame]
		                                     {
			                                     *answer = exit_choice::save;
			                                     frame->close(false);
		                                     }),
		std::make_shared<ui::button_control>(frame, icon_index::del, tt.movie_discard, std::string_view{},
		                                     [answer, frame]
		                                     {
			                                     *answer = exit_choice::discard;
			                                     frame->close(false);
		                                     }),
		std::make_shared<divider_element>(),
		std::make_shared<ui::close_control>(frame, true, tt.button_cancel),
	};

	if (dlg->show_modal(controls, {44}, {44}) != ui::close_result::ok) return false;
	if (*answer == exit_choice::save)
	{
		save_project([after_save = std::move(after_save)](const bool saved)
		{
			if (saved && after_save) after_save();
		});
		return false;
	}

	return *answer == exit_choice::discard;
}
