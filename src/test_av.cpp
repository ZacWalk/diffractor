// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for the audio and video layer (av*) -- stream naming, audio buffering, resampling and
// visualization, container probing, seeking, playback timing, the hover preview decoder, and subtitles:
// their styling, placement, timing and choice, and decoding them from sidecars and streams.

#include "pch.h"
#include "test.h"
#include "av_format.h"
#include "av_player.h"
#include "av_sid.h"
#include "av_sound.h"
#include "av_visualizer.h"
#include "files.h"
#include "model.h"
#include "model_display.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "app_text.h"

#include <condition_variable>
#include <future>
#include <mutex>

extern "C"
{
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
}

extern thread_local std::function<void(AVFrame&)> test_audio_pcm_frame;
extern thread_local std::function<int(int)> test_audio_receive_status;

static void should_format_audio_stream_names()
{
	av_stream_info stream;
	stream.type = av_stream_type::audio;
	stream.language = "eng";
	stream.audio_channels = 2;
	assert_equal("English - stereo", format_audio_stream_name(stream, 1), "language and channels");

	stream.language.clear();
	stream.title = "Director";
	stream.is_commentary = true;
	stream.audio_channels = 6;
	assert_equal("Director - commentary - 5.1 surround", format_audio_stream_name(stream, 1),
	             "title, role and channels");

	stream = {};
	stream.type = av_stream_type::audio;
	stream.audio_channels = 2;
	assert_equal("Audio track 2 - stereo", format_audio_stream_name(stream, 2), "numbered fallback");

	stream.audio_channels = 0;
	stream.codec = "aac";
	assert_equal("Audio track 3 - aac", format_audio_stream_name(stream, 3), "codec fallback");
}

// Not static: av_visualizer.h declares this a friend, which names the external-linkage function.
void should_compact_consumed_audio_only_when_needed()
{
	audio_info_t format;
	format.channel_layout = av_get_def_channel_layout(2);
	format.sample_fmt = prop::audio_sample_t::signed_16bit;
	format.sample_rate = 10;

	audio_buffer buffer;
	buffer.init(format);

	const std::array<uint8_t, 60> first{};
	const std::array<uint8_t, 40> second{};
	buffer.append(first.data(), static_cast<uint32_t>(first.size()), 1.0, 1);
	buffer.remove(40);
	buffer.append(second.data(), static_cast<uint32_t>(second.size()), 2.0, 1);

	assert_equal(60u, buffer.used_bytes(), "audio bytes retained after cursor compaction");
	assert_equal(1.5, buffer.seconds(), "audio duration retained after cursor compaction");
	assert_equal(1.5, buffer.start_time(), "audio start time follows appended frame timing");
	assert_equal(3.0, buffer.end_time(), "audio end time follows appended frame timing");

	audio_info_t visualizer_format;
	visualizer_format.channel_layout = av_get_def_channel_layout(2);
	visualizer_format.sample_fmt = prop::audio_sample_t::signed_16bit;
	visualizer_format.sample_rate = 48000;

	audio_buffer visualizer_buffer;
	visualizer_buffer.init(visualizer_format);
	std::array<int16_t, FFT_BUFFER_SIZE * 4> samples{};
	std::fill_n(samples.begin(), FFT_BUFFER_SIZE * 2, 100);
	for (size_t frame = 0; frame < FFT_BUFFER_SIZE; ++frame)
	{
		const auto sample = static_cast<int16_t>(12000.0 * sin(2.0 * M_PI * 16.0 * frame / FFT_BUFFER_SIZE));
		const auto i = FFT_BUFFER_SIZE * 2 + frame * 2;
		samples[i] = sample;
		samples[i + 1] = sample;
	}
	visualizer_buffer.append(std::bit_cast<const uint8_t*>(samples.data()),
	                         static_cast<uint32_t>(samples.size() * sizeof(int16_t)), 0.0, 1);
	visualizer_buffer.remove(FFT_BUFFER_SIZE * 4);

	av_visualizer visualizer;
	visualizer.update(visualizer_buffer);
	assert_equal(true, visualizer.step(1.0),
	             "visualizer consumes the live audio window");
	assert_equal(true, std::any_of(std::begin(visualizer._frame._data[0]),
	                               std::end(visualizer._frame._data[0]), [](const int bar) { return bar > 0; }),
	             "visualizer ignores consumed samples before the cursor");
}

// An endpoint's frames hold its own count of channels, and its speaker mask only places them. A mask
// that disagrees once made a mono endpoint with a stereo mask take stereo frames - twice the bytes of
// the buffer it hands out for them.
static void should_lay_out_an_endpoint_by_its_channel_count()
{
	const auto layout = [](const uint64_t mask, const int channels)
	{
		audio_info_t format;
		format.channel_layout = av_get_channel_layout(mask, channels);
		return format;
	};
	const auto mask_of = [](const audio_info_t& format)
	{
		return format.channel_layout->order == AV_CHANNEL_ORDER_NATIVE ? format.channel_layout->u.mask : 0;
	};

	const auto quad = layout(AV_CH_LAYOUT_QUAD, 4);
	assert_equal(4u, quad.channel_count(), "a mask naming every channel");
	assert_equal(true, mask_of(quad) == AV_CH_LAYOUT_QUAD, "places them on its speakers");

	const auto fewer = layout(AV_CH_LAYOUT_5POINT1_BACK, 4);
	assert_equal(4u, fewer.channel_count(), "speakers named past the endpoint's channels are ignored");
	assert_equal(true, mask_of(fewer) == AV_CH_LAYOUT_3POINT1, "and its first four kept");

	assert_equal(1u, layout(AV_CH_LAYOUT_STEREO, 1).channel_count(), "a mono endpoint with a stereo mask");
	assert_equal(6u, layout(AV_CH_LAYOUT_STEREO, 6).channel_count(), "channels beyond the speakers named count");
	assert_equal(6u, layout(0, 6).channel_count(), "missing speaker mask uses endpoint channel count");
}

// An endpoint with room for a set number of frames, keeping what it is handed.
class fake_audio_output final : public platform::audio_output
{
public:
	platform::audio_output_format mix;
	size_t frame_bytes = 1;
	size_t room_frames = 0;
	std::vector<uint8_t> written;

	std::string id() override { return "fake"; }
	platform::audio_output_format format() override { return mix; }
	double time() const override { return 0.0; }
	bool is_stopped() const override { return true; }
	bool is_device_lost() const override { return false; }
	void reset() override {}
	void start() override {}
	void stop() override {}
	void wait_for_buffer(const platform::thread_event&, uint32_t) override {}

	size_t write(const df::cspan samples) override
	{
		const auto frames = std::min(samples.size / frame_bytes, room_frames);
		const auto bytes = frames * frame_bytes;
		written.insert(written.end(), samples.data, samples.data + bytes);
		room_frames -= frames;
		return bytes;
	}

	void write_silence() override {}
	void volume(double) override {}
};

// The playback and preview buffers are laid out by the format their endpoint describes, hand it what
// it has room for, and keep the rest queued, in order and on time, for the next write.
static void should_hand_an_endpoint_the_samples_it_has_room_for()
{
	fake_audio_output endpoint;
	endpoint.mix = {10, platform::audio_sample_format::int16, 2, AV_CH_LAYOUT_STEREO};
	endpoint.frame_bytes = 4;

	const auto format = av_audio_info(endpoint.format());
	assert_equal(10u, format.sample_rate, "the endpoint's rate");
	assert_equal(true, format.sample_fmt == prop::audio_sample_t::signed_16bit, "its samples");
	assert_equal(2u, format.channel_count(), "and its channels lay out the buffer");

	audio_buffer buffer;
	buffer.init(format);

	std::array<uint8_t, 40> second = {};
	for (size_t i = 0; i < second.size(); ++i) second[i] = static_cast<uint8_t>(i + 1);
	buffer.append(second.data(), static_cast<uint32_t>(second.size()), 5.0, 1);

	endpoint.room_frames = 3;
	buffer.write_to(endpoint);
	assert_equal(12u, static_cast<uint32_t>(endpoint.written.size()), "the endpoint takes the frames it has room for");
	assert_equal(28u, buffer.used_bytes(), "and the rest stays queued");
	assert_equal(true, df::equiv(buffer.start_time(), 5.3), "from the time the endpoint reached");

	buffer.write_to(endpoint);
	assert_equal(28u, buffer.used_bytes(), "a full endpoint takes nothing");

	endpoint.room_frames = 100;
	buffer.write_to(endpoint);
	assert_equal(true, buffer.is_empty(), "and one with room takes the rest");
	assert_equal(true, std::ranges::equal(endpoint.written, second), "in order");

	const auto surround = av_audio_info({48000, platform::audio_sample_format::float32, 6, AV_CH_LAYOUT_5POINT1});
	assert_equal(true, surround.sample_fmt == prop::audio_sample_t::signed_float, "float samples");
	assert_equal(6u, surround.channel_count(), "on six channels");
	assert_equal(true, av_audio_info({48000, platform::audio_sample_format::int32, 2, 0}).sample_fmt ==
	             prop::audio_sample_t::signed_32bit, "32-bit samples");
	assert_equal(true, av_audio_info({48000, platform::audio_sample_format::none, 2, 0}).sample_fmt ==
	             prop::audio_sample_t::none, "and none the mixer does not speak");
}

// audio_buffer keeps its samples private; this declared friend lets the test read them.
struct audio_ramp_probe
{
	static int16_t sample(const audio_buffer& buffer, const size_t i)
	{
		return std::bit_cast<const int16_t*>(buffer.data + buffer.start_pos)[i];
	}
};

static void should_ramp_audio_at_buffer_edges()
{
	audio_info_t format;
	format.channel_layout = av_get_def_channel_layout(2);
	format.sample_fmt = prop::audio_sample_t::signed_16bit;
	format.sample_rate = 1000;

	audio_buffer buffer;
	buffer.init(format);

	std::array<int16_t, 2000> samples{};
	samples.fill(1000);
	buffer.append(std::bit_cast<const uint8_t*>(samples.data()),
	              static_cast<uint32_t>(samples.size() * sizeof(int16_t)), 0.0, 1);

	buffer.apply_fade_in(0.010);
	buffer.apply_fade_out(0.010);

	// 1000 frames of stereo audio at 1kHz, so a 10ms ramp covers 10 frames = 20 samples.
	const auto sample = [&buffer](const size_t i) { return audio_ramp_probe::sample(buffer, i); };

	assert_equal(true, sample(0) < 200, "playback starts near silence");
	assert_equal(1000, static_cast<int>(sample(19)), "the fade in reaches full level after 10ms");
	assert_equal(1000, static_cast<int>(sample(1000)), "audio between the ramps is untouched");
	assert_equal(0, static_cast<int>(sample(1999)), "playback ends at silence");
	assert_equal(true, sample(1979) == 1000, "the fade out only covers the last 10ms");
	assert_equal(4000u, buffer.used_bytes(), "ramping does not consume buffered audio");
}

// Not static: av_visualizer.h declares this a friend, which names the external-linkage function.
void should_time_visualizer_independently_of_refresh_rate()
{
	auto animate = [](const double frame_seconds)
	{
		av_visualizer visualizer;
		av_visualizer::frame peak(0.0);
		peak._data[0][8] = 1000;
		visualizer._frames.push(peak);

		for (auto time = 0.0; time <= 0.1; time += frame_seconds)
		{
			visualizer.step(time);
		}

		return visualizer._frame._data[0][8];
	};

	const auto level_30hz = animate(1.0 / 30.0);
	const auto level_120hz = animate(1.0 / 120.0);
	assert_equal(true, std::abs(level_30hz - level_120hz) < 30,
	             "visualizer attack is independent of display refresh rate");

	av_visualizer visualizer;
	av_visualizer::frame loud(0.010);
	loud._data[0][8] = 1000;
	av_visualizer::frame quiet(0.020);
	visualizer._frames.push(loud);
	visualizer._frames.push(quiet);
	visualizer.step(0.0);
	assert_equal(true, visualizer._frame._data[0][8] > 0,
	             "visualizer preserves a transient between presentation frames");
}

// MEDIA-007 - the frequency table is one immutable object. The old constructor rewrote the shared
// bins for every visualizer instance; this catches that deterministically without a race detector.
void should_share_one_immutable_visualizer_frequency_table()
{
	const auto& first = av_visualizer::frequency_scale();
	const auto* const first_address = first.data();
	const auto first_contents = first;
	const auto init_count = av_visualizer::frequency_scale_init_count().load();

	for (auto i = 0; i < 64; ++i)
	{
		av_visualizer visualizer;
	}

	const auto& second = av_visualizer::frequency_scale();
	assert_equal(true, first_address == second.data(), "visualizer instances share one frequency table object");
	assert_equal(true, std::ranges::equal(first_contents, second), "constructing visualizers does not rewrite bins");
	assert_equal(init_count, av_visualizer::frequency_scale_init_count().load(),
	             "constructing visualizers does not reinitialize bins");
}

// MEDIA-007 - constructing one visualizer must not rewrite the frequency bins another session's
// audio path is reading. This exercises the session-switch interleaving with independent inputs.
void should_keep_visualizer_frequency_bins_stable_across_sessions()
{
	audio_info_t format;
	format.channel_layout = av_get_def_channel_layout(2);
	format.sample_fmt = prop::audio_sample_t::signed_16bit;
	format.sample_rate = 48000;

	const auto fill_buffer = [&format](audio_buffer& buffer, const double frequency)
	{
		buffer.clear();

		std::array<int16_t, FFT_BUFFER_SIZE * 2> samples{};
		for (size_t frame = 0; frame < FFT_BUFFER_SIZE; ++frame)
		{
			const auto sample = static_cast<int16_t>(14000.0 * sin(2.0 * M_PI * frequency * frame / FFT_BUFFER_SIZE));
			samples[frame * 2] = sample;
			samples[frame * 2 + 1] = sample;
		}

		buffer.append(std::bit_cast<const uint8_t*>(samples.data()),
		              static_cast<uint32_t>(samples.size() * sizeof(int16_t)), 0.0, 1);
	};

	std::atomic_bool stop = false;
	std::mutex update_mutex;
	std::condition_variable update_changed;
	auto updates_requested = 0;
	auto updates_completed = 0;

	av_visualizer first;
	const auto worker = std::async(std::launch::async, [&]
	{
		audio_buffer buffer;
		buffer.init(format);

		for (;;)
		{
			{
				std::unique_lock lock(update_mutex);
				update_changed.wait(lock, [&]
				{
					return stop.load() || updates_completed < updates_requested;
				});

				if (stop.load()) return;
			}

			fill_buffer(buffer, 12.0);
			first.update(buffer);

			{
				std::lock_guard lock(update_mutex);
				++updates_completed;
			}

			update_changed.notify_all();
		}
	});
	const df::scope_exit stop_worker([&]
	{
		stop = true;
		update_changed.notify_all();
	});

	(void)av_visualizer::frequency_scale();
	const auto init_count = av_visualizer::frequency_scale_init_count().load();

	for (auto i = 0; i < 64; ++i)
	{
		audio_buffer buffer;
		buffer.init(format);
		fill_buffer(buffer, 48.0);

		av_visualizer second;
		second.update(buffer);
		assert_equal(true, second.step(1.0), "new session visualizer has bins");

		{
			std::lock_guard lock(update_mutex);
			++updates_requested;
		}

		update_changed.notify_all();

		{
			std::unique_lock lock(update_mutex);
			assert_equal(true, update_changed.wait_for(lock, std::chrono::seconds(5), [&]
			             {
				             return updates_completed >= updates_requested;
			             }),
			             "old session updated while new visualizers were constructed");
		}
	}

	stop = true;
	update_changed.notify_all();
	worker.wait();

	audio_buffer first_buffer;
	first_buffer.init(format);
	fill_buffer(first_buffer, 12.0);
	first.update(first_buffer);

	audio_buffer second_buffer;
	second_buffer.init(format);
	fill_buffer(second_buffer, 48.0);
	av_visualizer second;
	second.update(second_buffer);

	assert_equal(64, updates_completed, "old session updated once for each constructed visualizer");
	assert_equal(init_count, av_visualizer::frequency_scale_init_count().load(),
	             "constructing visualizers while another updates does not reinitialize bins");
	assert_equal(true, first.step(1.0), "old session visualizer still has bins");
	assert_equal(true, second.step(1.0), "new session visualizer still has bins");
}

static void should_extract_dv_datetime()
{
	// Build a minimal raw DV frame (one DIF sequence) carrying the VAUX
	// recording-date (0x62) and recording-time (0x63) packs at the offsets the
	// DV format places them (VAUX DIF block 3 of the sequence).
	std::vector<uint8_t> frame(12000, 0);

	auto* const date_pack = &frame[80 * 3 + 13];
	date_pack[0] = 0x62; // VAUX recording date pack id
	date_pack[1] = 0xff; // timezone unknown
	date_pack[2] = 0xc0 | (1 << 4) | 5; // day 15 (reserved bits set)
	date_pack[3] = (0 << 4) | 7; // month 07
	date_pack[4] = (0 << 4) | 3; // year 03 -> 2003

	auto* const time_pack = &frame[80 * 3 + 18];
	time_pack[0] = 0x63; // VAUX recording time pack id
	time_pack[1] = 0xff; // frames unknown
	time_pack[2] = (4 << 4) | 5; // 45 seconds
	time_pack[3] = (3 << 4) | 0; // 30 minutes
	time_pack[4] = (1 << 4) | 4; // 14 hours

	const auto actual = dv_extract_rec_datetime(frame.data(), frame.size());
	assert_equal(df::date_t(2003, 7, 15, 14, 30, 45), actual, "dv rec datetime");

	// A frame without recording packs must yield an invalid (absent) date.
	const std::vector<uint8_t> empty_frame(12000, 0);
	assert_equal(false, dv_extract_rec_datetime(empty_frame.data(), empty_frame.size()).is_valid(),
	             "dv no packs");
}

static void should_correct_pts()
{
	// av_pts_correction takes FFmpeg's AVFrame::best_effort_timestamp (falling back to pts
	// then pkt_dts) and guarantees a strictly increasing result so the presenter can always
	// order frames. AV_NOPTS_VALUE is INT64_MIN; mirror it here so the test does not need to
	// pull in the libav* headers.
	constexpr int64_t nopts = std::numeric_limits<int64_t>::min();

	// Clean, monotonic timestamps pass straight through.
	{
		av_pts_correction pc;
		assert_equal(0, static_cast<int>(pc.guess(0, 0, 0, 100)), "monotonic 0");
		assert_equal(100, static_cast<int>(pc.guess(100, 100, 100, 100)), "monotonic 100");
		assert_equal(200, static_cast<int>(pc.guess(200, 200, 200, 100)), "monotonic 200");
	}

	// best_effort_timestamp wins over pts and pkt_dts; those are only consulted when the
	// decoder had nothing to publish.
	{
		av_pts_correction pc;
		assert_equal(500, static_cast<int>(pc.guess(500, 700, 900, 100)), "best effort preferred");
		assert_equal(700, static_cast<int>(pc.guess(nopts, 700, 900, 100)), "falls back to pts");
		assert_equal(900, static_cast<int>(pc.guess(nopts, nopts, 900, 100)), "falls back to dts");
	}

	// A duplicated timestamp must not be returned verbatim - that would make the
	// presenter treat the frame as "not newer" and stall - so it is advanced by
	// one frame duration instead.
	{
		av_pts_correction pc;
		assert_equal(0, static_cast<int>(pc.guess(0, nopts, nopts, 100)), "dup first");
		assert_equal(100, static_cast<int>(pc.guess(100, nopts, nopts, 100)), "dup second");
		assert_equal(200, static_cast<int>(pc.guess(100, nopts, nopts, 100)), "dup advanced by duration");
	}

	// With no timestamps at all (raw / MJPEG streams) and no reported duration,
	// the timeline still advances using the cadence learned from earlier frames.
	{
		av_pts_correction pc;
		assert_equal(0, static_cast<int>(pc.guess(0, nopts, nopts, 0)), "nopts first");
		assert_equal(40, static_cast<int>(pc.guess(40, nopts, nopts, 0)), "nopts learn cadence");
		assert_equal(80, static_cast<int>(pc.guess(nopts, nopts, nopts, 0)), "nopts synth 1");
		assert_equal(120, static_cast<int>(pc.guess(nopts, nopts, nopts, 0)), "nopts synth 2");
	}

	// The learned cadence is the smallest step seen, not the most recent one: a gap in a
	// damaged stream must not become the synthetic step and run the timeline away.
	{
		av_pts_correction pc;
		assert_equal(0, static_cast<int>(pc.guess(0, nopts, nopts, 0)), "gap first");
		assert_equal(40, static_cast<int>(pc.guess(40, nopts, nopts, 0)), "gap learn cadence");
		assert_equal(9000, static_cast<int>(pc.guess(9000, nopts, nopts, 0)), "gap jump");
		assert_equal(9040, static_cast<int>(pc.guess(nopts, nopts, nopts, 0)), "synth uses smallest step");
	}

	// Invariant: however messy the timestamps (duplicates, backward jumps), the output is
	// always strictly increasing.
	{
		av_pts_correction pc;
		const int64_t messy[] = {0, 200, 100, 100, 400, 300, 500};
		auto prev = nopts;
		for (const auto p : messy)
		{
			const auto t = pc.guess(p, nopts, nopts, 50);
			if (prev != nopts) assert_equal(true, t > prev, "strictly increasing");
			prev = t;
		}
	}
}

// MEDIA-006 - EOF is not the end of frame output for codecs with delayed/reordered pictures. The
// delayed frames must pass through the same nearest-choice logic before extraction concludes.
static void should_drain_delayed_video_frames_for_nearest_eof_choice()
{
	constexpr auto wanted = 1.19;
	av_nearest_frame_choice choice;

	assert_equal(true, choice.consider(1.0, wanted, 0.0), "pre-EOF frame is the initial candidate");
	assert_equal(true, should_drain_delayed_video_frames_at_eof(choice), "EOF drains delayed decoder output");

	assert_equal(true, choice.consider(1.16, wanted, 0.0), "first delayed frame improves the choice");
	assert_equal(true, choice.consider(1.20, wanted, 0.0), "final delayed frame is nearest");

	assert_equal(true, df::equiv(1.20, choice.best_time),
	             std::format("delayed final frame wins nearest choice (got {:.2f})", choice.best_time));
}

// The index scan bounds FFmpeg's stream probe; the inspect scan does not. Every property the index
// records has to survive that bound, so the two intents are compared across the AV containers.
static void should_scan_av_metadata_with_a_bounded_probe()
{
	auto fixtures_carrying_xmp = 0;

	for (const auto* const name : {
		     "gizmo.mp4", "indy.mp4", "anamorphic.mp4", "ipod.mov", "StPauls.MOV", "tagged.mkv",
		     "Byzantium.avi", "Colorblind.mp3"
	     })
	{
		const auto path = test_files_folder.combine_file(name);
		const auto* const ft = files::file_type_from_name(path);

		files ff;
		const auto inspected = ff.scan_file(path, false, ft, {}, {}, libfiles::scan_intent::inspect);
		const auto indexed = ff.scan_file(path, false, ft, {}, {}, libfiles::scan_intent::index);

		// The player's summary comes from a playback open, which lays libfiles' reading of the container
		// over FFmpeg's streams.
		av_format_decoder decoder;
		assert_equal(true, decoder.open(path, media_intent::playback), name, "open media for its displayed summary");
		const auto displayed = decoder.info();
		assert_equal(inspected.video_codec.sv(), displayed.video_codec.sv(), name,
		             "attached artwork cannot replace the main video codec");
		assert_equal(inspected.pixel_format.sv(), displayed.pixel_format.sv(), name,
		             "displayed pixel format belongs to the same video stream as the index");

		assert_equal(true, indexed.success, name, "index scan succeeded");
		assert_equal(inspected.width, indexed.width, name, "width");
		assert_equal(inspected.height, indexed.height, name, "height");
		assert_equal(inspected.duration, indexed.duration, name, "duration");
		assert_equal(inspected.video_codec.sv(), indexed.video_codec.sv(), name, "video codec");
		assert_equal(inspected.pixel_format.sv(), indexed.pixel_format.sv(), name, "pixel format");
		assert_equal(inspected.audio_codec.sv(), indexed.audio_codec.sv(), name, "audio codec");
		assert_equal(inspected.audio_sample_rate, indexed.audio_sample_rate, name, "audio sample rate");
		assert_equal(inspected.audio_channels, indexed.audio_channels, name, "audio channels");
		assert_equal(static_cast<int>(inspected.audio_sample_type), static_cast<int>(indexed.audio_sample_type),
		             name, "audio sample type");
		assert_equal(inspected.bitrate.sv(), indexed.bitrate.sv(), name, "bit rate");
		assert_equal(inspected.orientation, indexed.orientation, name, "orientation");
		assert_equal(inspected.created_utc, indexed.created_utc, name, "created");

		// gizmo.mp4, ipod.mov and Byzantium.avi carry their XMP packet in the last 1% of the file,
		// megabytes past the probe budget. It survives because container metadata is read by the
		// demuxer's read_header, inside avformat_open_input, which the bound is applied after.
		assert_equal(inspected.metadata.xmp.size(), indexed.metadata.xmp.size(), name, "xmp size");
		assert_equal(true, std::ranges::equal(inspected.metadata.xmp, indexed.metadata.xmp), name, "xmp bytes");

		if (!inspected.metadata.xmp.empty()) ++fixtures_carrying_xmp;
	}

	// Without this the XMP assertions above would pass on an empty packet and prove nothing.
	assert_equal(3, fixtures_carrying_xmp, "fixtures carrying a trailing xmp packet");
}

static df::blob make_tga(const uint16_t width, const uint16_t height, const uint8_t bits_per_pixel,
                         const std::vector<uint8_t>& pixels, const uint8_t alpha_bits = 8)
{
	df::blob result(18, 0);
	result[2] = 2; // uncompressed true-colour image
	result[12] = static_cast<uint8_t>(width & 0xff);
	result[13] = static_cast<uint8_t>(width >> 8);
	result[14] = static_cast<uint8_t>(height & 0xff);
	result[15] = static_cast<uint8_t>(height >> 8);
	result[16] = bits_per_pixel;
	result[17] = static_cast<uint8_t>(0x20 | (bits_per_pixel == 32 ? alpha_bits : 0)); // top-left origin
	result.insert(result.end(), pixels.begin(), pixels.end());
	return result;
}

static df::blob make_gif(const bool transparent)
{
	df::blob result{
		'G', 'I', 'F', '8', '9', 'a',
		1, 0, 1, 0,
		0x80, 0x00, 0x00,
		0xff, 0x00, 0x00,
		0x00, 0x00, 0xff
	};

	if (transparent)
	{
		constexpr std::array<uint8_t, 8> transparency{
			0x21, 0xf9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00
		};
		result.insert(result.end(), transparency.begin(), transparency.end());
	}

	constexpr std::array<uint8_t, 16> image{
		0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0,
		0x02, 0x02, 0x44, 0x01, 0x00,
		0x3b
	};
	result.insert(result.end(), image.begin(), image.end());
	return result;
}

static df::blob make_bmp(const uint32_t width, const uint32_t height)
{
	const auto stride = (width * 3 + 3) & ~3u;
	const auto pixel_bytes = stride * height;
	const auto file_bytes = 14u + 40u + pixel_bytes;

	df::blob result(file_bytes);
	result[0] = 'B';
	result[1] = 'M';
	result[2] = static_cast<uint8_t>(file_bytes & 0xff);
	result[3] = static_cast<uint8_t>((file_bytes >> 8) & 0xff);
	result[4] = static_cast<uint8_t>((file_bytes >> 16) & 0xff);
	result[5] = static_cast<uint8_t>((file_bytes >> 24) & 0xff);
	result[10] = 14 + 40;
	result[14] = 40;
	result[18] = static_cast<uint8_t>(width & 0xff);
	result[19] = static_cast<uint8_t>((width >> 8) & 0xff);
	result[20] = static_cast<uint8_t>((width >> 16) & 0xff);
	result[21] = static_cast<uint8_t>((width >> 24) & 0xff);
	result[22] = static_cast<uint8_t>(height & 0xff);
	result[23] = static_cast<uint8_t>((height >> 8) & 0xff);
	result[24] = static_cast<uint8_t>((height >> 16) & 0xff);
	result[25] = static_cast<uint8_t>((height >> 24) & 0xff);
	result[26] = 1;
	result[28] = 24;
	result[34] = static_cast<uint8_t>(pixel_bytes & 0xff);
	result[35] = static_cast<uint8_t>((pixel_bytes >> 8) & 0xff);
	result[36] = static_cast<uint8_t>((pixel_bytes >> 16) & 0xff);
	result[37] = static_cast<uint8_t>((pixel_bytes >> 24) & 0xff);
	std::fill(result.begin() + 54, result.end(), 0x80);
	return result;
}

static uint8_t composite_over_green(const ui::const_surface_ptr& surface, const int x)
{
	const auto* const p = surface->pixels_line(0) + x * 4;
	const auto alpha = surface->format() == ui::texture_format::ARGB ? p[3] / 255.0f : 1.0f;
	return static_cast<uint8_t>(std::clamp(static_cast<int>(p[1] * alpha + 255.0f * (1.0f - alpha) + 0.5f),
	                                      0, 255));
}

// MEDIA-008 - still fallback conversion must retain alpha-capable source semantics.
static void should_preserve_alpha_when_decoding_ffmpeg_stills()
{
	std::array<uint8_t, 8> bgra{0, 0, 255, 255, 255, 0, 0, 0};
	AVFrame frame{};
	frame.format = AV_PIX_FMT_BGRA;
	frame.width = 2;
	frame.height = 1;
	frame.data[0] = bgra.data();
	frame.linesize[0] = 8;

	av_scaler scaler;
	ui::surface_ptr with_alpha;
	assert_equal(true, scaler.scale_frame(frame, with_alpha, {}, 0.0, ui::orientation::top_left, {}, true),
	             "alpha frame converted");
	assert_equal(static_cast<int>(ui::texture_format::ARGB), static_cast<int>(with_alpha->format()),
	             "alpha frame keeps alpha-capable surface format");
	assert_equal(0, static_cast<int>(composite_over_green(with_alpha, 0)), "opaque red covers green background");
	assert_equal(255, static_cast<int>(composite_over_green(with_alpha, 1)),
	             "transparent pixel reveals green background");

	ui::surface_ptr opaque;
	assert_equal(true, scaler.scale_frame(frame, opaque, {}, 0.0, ui::orientation::top_left),
	             "opaque video frame converted");
	assert_equal(static_cast<int>(ui::texture_format::RGB), static_cast<int>(opaque->format()),
	             "opaque video frames keep the opaque fast path");

	std::array<uint8_t, 2> palette_indices{0, 1};
	std::array<uint32_t, 256> palette{};
	palette[0] = 0xffff0000u;
	palette[1] = 0xff0000ffu;

	AVFrame palette_frame{};
	palette_frame.format = AV_PIX_FMT_PAL8;
	palette_frame.width = 2;
	palette_frame.height = 1;
	palette_frame.data[0] = palette_indices.data();
	palette_frame.linesize[0] = static_cast<int>(palette_indices.size());
	palette_frame.data[1] = std::bit_cast<uint8_t*>(palette.data());

	ui::surface_ptr opaque_palette;
	assert_equal(true, scaler.scale_frame(palette_frame, opaque_palette, {}, 0.0, ui::orientation::top_left, {}, true),
	             "opaque palette frame converted");
	assert_equal(static_cast<int>(ui::texture_format::RGB), static_cast<int>(opaque_palette->format()),
	             "opaque palette frames keep the opaque fast path");

	palette[1] = 0x000000ffu;
	ui::surface_ptr transparent_palette;
	assert_equal(true, scaler.scale_frame(palette_frame, transparent_palette, {}, 0.0, ui::orientation::top_left, {},
	                                      true),
	             "transparent palette frame converted");
	assert_equal(static_cast<int>(ui::texture_format::ARGB), static_cast<int>(transparent_palette->format()),
	             "transparent palette frames keep alpha");
}

// MEDIA-008 - a 32-bit TGA with zero alpha-bit count stores padding, not transparency.
static void should_decode_zero_alpha_tga_as_opaque()
{
	const auto tga = make_tga(1, 1, 32, {0, 0, 255, 0}, 0);
	const auto decoded = av_decode_still(tga, {}, "tga");
	assert_equal(true, is_valid(decoded), "zero-alpha tga decoded");
	assert_equal(static_cast<int>(ui::texture_format::RGB), static_cast<int>(decoded->format()),
	             "zero-alpha tga remains opaque");
	assert_equal(0, static_cast<int>(composite_over_green(decoded, 0)), "opaque red covers green background");
}

// MEDIA-008 - GIF frames are alpha-capable, but opaque palettes do not need ARGB.
static void should_decode_opaque_gif_palette_as_rgb()
{
	const auto opaque = av_decode_still(make_gif(false), {}, ".gif");
	assert_equal(true, is_valid(opaque), "opaque gif decoded");
	assert_equal(static_cast<int>(ui::texture_format::RGB), static_cast<int>(opaque->format()),
	             "opaque palette gif remains RGB");
}

// MEDIA-009 - the application pixel ceiling is supplied before FFmpeg's still-image probe decoder.
static void should_refuse_over_budget_stills_during_probe()
{
	constexpr uint16_t width = 16;
	constexpr uint16_t height = 16;
	const auto bmp = make_bmp(width, height);

	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });
	df::max_decode_bytes = static_cast<int64_t>(width) * height * 4 - 1;

	load_diagnostic diagnostic;
	const auto refused = av_decode_still(bmp, {}, ".bmp", &diagnostic);
	assert_equal(false, is_valid(refused), "over-budget still is refused");
	assert_equal(true, diagnostic.over_budget, "probe refusal reports the budget reason");
	assert_equal(static_cast<int>(width), diagnostic.source_dimensions.cx, "budget report width");
	assert_equal(static_cast<int>(height), diagnostic.source_dimensions.cy, "budget report height");
}

// MEDIA-012 - MJPEG admission accounts for codec working storage, not only BGRA source pixels.
static void should_refuse_mjpeg_when_codec_working_storage_exceeds_budget()
{
	const auto data = df::blob_from_file(test_files_folder.combine_file("Test.jpg"));
	const auto control = av_decode_still(data, {}, ".jpg");
	assert_equal(true, is_valid(control), "jpeg decodes through ffmpeg control path");

	const auto source_bytes = static_cast<int64_t>(control->dimensions().cx) * control->dimensions().cy * 4;
	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });

	df::max_decode_bytes = source_bytes * 2;
	const auto refused = av_decode_still(data, {}, ".jpg");
	assert_equal(false, is_valid(refused), "codec working storage is budgeted separately from source pixels");
}

// FFmpeg falls back to matching a demuxer on the file extension alone, so a TypeScript source file
// named index.ts is handed to the MPEG-TS demuxer, opens without any error and then describes no
// stream whatsoever. The scan must report that as a failure rather than publish an empty video.
static void should_reject_a_non_media_file()
{
	const auto path = test_files_folder.combine("excluded1").combine_file("not-media.ts");
	const auto* const ft = files::file_type_from_name(path);

	// The extension alone still says video; only the header settles it.
	assert_equal(true, path.exists(), "fixture is present");
	assert_equal(true, ft->has_trait(file_traits::av), "ts is an av extension");
	assert_equal(false, files::media_header_matches(path.extension(), df::blob_head_from_file(path, 1024)),
	             "the header rule refuses it before the demuxer sees it");

	av_format_decoder decoder;
	assert_equal(false, decoder.open(path, media_intent::metadata), "decoder rejects the file");

	files ff;
	assert_equal(false, ff.scan_file(path, false, ft, {}, {}, libfiles::scan_intent::inspect).success, "inspect scan fails");
	assert_equal(false, ff.scan_file(path, false, ft, {}, {}, libfiles::scan_intent::index).success, "index scan fails");
}

// Issue #78 - Some videos ignore aspect ratio.
// anamorphic.mp4 is stored at 640x480 with a 4:3 pixel (sample) aspect ratio,
// i.e. a 16:9 display. The scanner must report the display dimensions (640x360)
// rather than the stored frame size (640x480).
static void should_apply_video_aspect_ratio()
{
	const auto load_path = test_files_folder.combine_file("anamorphic.mp4");

	files ff;
	const auto actual = ff_scan_file(ff, load_path);
	const auto md = actual.to_props();

	assert_equal(640, md->width, "anamorphic display width");
	assert_equal(360, md->height, "anamorphic display height");
}

// Issue #78 - thumbnails and the scrubber preview ignored the aspect ratio long after playback
// respected it. An MP4 'pasp' box reaches AVStream::sample_aspect_ratio only: FFmpeg copies it
// neither onto the codec parameters nor onto a decoded AVFrame. A file whose H.264 VUI declares
// no aspect (or square pixels) therefore hands the frame scaler a 1:1 frame the container
// contradicts, and every decoded picture came out at the stored 4:3 shape instead of 16:9.
// anamorphic-pasp.mp4 is anamorphic.mp4 with its VUI aspect_ratio_idc rewritten to 1 (square),
// leaving the 4:3 pasp box as the only surviving declaration.
static void should_apply_container_aspect_ratio_to_decoded_frames()
{
	const auto load_path = test_files_folder.combine_file("anamorphic-pasp.mp4");

	files ff;
	const auto md = ff_scan_file(ff, load_path).to_props();
	assert_equal(640, md->width, "scanned display width");
	assert_equal(360, md->height, "scanned display height");

	av_format_decoder decoder;
	assert_equal(true, decoder.open(load_path, media_intent::thumbnail), "open anamorphic-pasp.mp4");
	decoder.init_streams(-1, -1, false, false, false);
	assert_equal(true, decoder.has_video(), "anamorphic-pasp.mp4 has video");

	constexpr sizei max_dim(256, 256);

	// 640x480 stored, 16:9 displayed: bounded by 256 the thumbnail is 256x144, not 256x192.
	ui::surface_ptr thumbnail;
	assert_equal(true, decoder.extract_thumbnail(thumbnail, max_dim, 1, 100), "thumbnail decoded");
	assert_equal(true, is_valid(thumbnail), "thumbnail valid");
	assert_equal(256, thumbnail->dimensions().cx, "thumbnail width");
	assert_equal(144, thumbnail->dimensions().cy, "thumbnail height");

	// The hover preview over the timeline decodes through a different entry point.
	ui::surface_ptr preview;
	assert_equal(true, decoder.extract_seek_frame(preview, max_dim, 50, 100), "seek frame decoded");
	assert_equal(true, is_valid(preview), "seek frame valid");
	assert_equal(256, preview->dimensions().cx, "seek frame width");
	assert_equal(144, preview->dimensions().cy, "seek frame height");
}

// Issue #78 - the file the reporter supplied after retesting 1.27.1 still showed square-pixel
// thumbnails. tvp.mp4 is broadcast-derived: 528x560 stored with a 249:176 pixel aspect, i.e. a
// 4:3 display. The scan reported 528x396 correctly from 1.27.0 onwards, which is why playback
// looked right while the thumbnail and the timeline hover preview stayed at the stored shape.
static void should_apply_aspect_ratio_to_broadcast_video_thumbnails()
{
	const auto load_path = test_files_folder.combine_file("tvp.mp4");

	files ff;
	const auto md = ff_scan_file(ff, load_path).to_props();
	assert_equal(528, md->width, "scanned display width");
	assert_equal(396, md->height, "scanned display height");

	av_format_decoder decoder;
	assert_equal(true, decoder.open(load_path, media_intent::thumbnail), "open tvp.mp4");
	decoder.init_streams(-1, -1, false, false, false);
	assert_equal(true, decoder.has_video(), "tvp.mp4 has video");

	constexpr sizei max_dim(256, 256);

	// 4:3 displayed, so bounded by 256 the thumbnail is 256x192, not the stored 241x256.
	ui::surface_ptr thumbnail;
	assert_equal(true, decoder.extract_thumbnail(thumbnail, max_dim, 1, 100), "thumbnail decoded");
	assert_equal(true, is_valid(thumbnail), "thumbnail valid");
	assert_equal(256, thumbnail->dimensions().cx, "thumbnail width");
	assert_equal(192, thumbnail->dimensions().cy, "thumbnail height");

	ui::surface_ptr preview;
	assert_equal(true, decoder.extract_seek_frame(preview, max_dim, 5, 10), "seek frame decoded");
	assert_equal(true, is_valid(preview), "seek frame valid");
	assert_equal(256, preview->dimensions().cx, "seek frame width");
	assert_equal(192, preview->dimensions().cy, "seek frame height");
}

// A container-level seek does not flush the codec, so extract_thumbnail must flush
// the decoder after seeking - otherwise, when the decoder is reused across calls, a
// later-position thumbnail can be served from a frame that was buffered before the
// seek. This reuses one decoder for an early and a late thumbnail (the exact reuse
// scenario) and asserts the two frames differ.
static void should_flush_decoder_on_thumbnail_seek()
{
	const auto load_path = test_files_folder.combine_file("gizmo.mp4");

	av_format_decoder decoder;
	assert_equal(true, decoder.open(load_path, media_intent::thumbnail), "open gizmo.mp4");
	decoder.init_streams(-1, -1, false, false, false);
	assert_equal(true, decoder.has_video(), "gizmo.mp4 has video");

	constexpr sizei max_dim(256, 256);

	ui::surface_ptr early;
	assert_equal(true, decoder.extract_thumbnail(early, max_dim, 1, 100), "early thumbnail decoded");
	assert_equal(true, is_valid(early), "early thumbnail valid");

	// Reuse the same decoder; the seek to 95% must flush the frames buffered by the
	// early extraction above rather than replaying one of them.
	ui::surface_ptr late;
	assert_equal(true, decoder.extract_thumbnail(late, max_dim, 95, 100), "late thumbnail decoded");
	assert_equal(true, is_valid(late), "late thumbnail valid");

	const auto same_size = is_valid(early) && is_valid(late) && early->size() == late->size();
	assert_equal(true, same_size, "thumbnails allocated to the same size");

	const auto identical = same_size && memcmp(early->pixels(), late->pixels(), early->size()) == 0;
	assert_equal(false, identical, "late thumbnail differs from early (decoder flushed after seek)");
}

// Issue #78 - the aspect ratio a video plays at. should_apply_container_aspect_ratio_to_decoded_frames
// covers the thumbnail and scrubber-preview half of that report; this holds the player to the same
// answer, because the session's display dimensions are what the media view sizes its box from. The
// two have to agree or the same file is correct as a tile and wrong as a video.
static void should_report_container_aspect_ratio_to_the_player()
{
	const auto load_path = test_files_folder.combine_file("anamorphic-pasp.mp4");

	files ff;
	const auto scanned = ff_scan_file(ff, load_path).to_props();

	const auto ses = make_test_session();
	assert_equal(true, ses->open(load_path, files::file_type_from_name(load_path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");

	const auto info = ses->info();

	// 640x480 stored, 4:3 pasp, so 16:9 displayed.
	assert_equal(640, info.display_dimensions.cx, "player display width");
	assert_equal(360, info.display_dimensions.cy, "player display height");
	assert_equal(static_cast<int>(scanned->width), info.display_dimensions.cx, "player agrees with the scan on width");
	assert_equal(static_cast<int>(scanned->height), info.display_dimensions.cy,
	             "player agrees with the scan on height");

	ses->close(false);
}

// Issue #252 - a rotated video. The scan reads the rotation from the container display matrix and
// the player reads it again once it decodes, so the two have to agree: a disagreement is what made
// a portrait video draw a portrait tile and then play back landscape, or the reverse.
static void should_report_container_rotation_to_the_player()
{
	const auto load_path = test_files_folder.combine("excluded1").combine_file("rotated90.mp4");

	files ff;
	const auto scanned = ff_scan_file(ff, load_path).to_props();
	assert_equal(ui::orientation::right_top, scanned->orientation, "scan reads the display matrix");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(load_path, files::file_type_from_name(load_path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");

	assert_equal(ui::orientation::right_top, ses->info().display_orientation, "player agrees with the scan");

	ses->close(false);
}

// A mirrored video. A container's transform can mirror the picture as well as turn it, and the scan and
// the player both read the mirror from it, as they read the turn, so a mirrored file's tile and its
// playback agree. rotated90.mp4 is made mirrored here by its track's matrix alone. Before, the scan
// dropped the mirror and the player read a horizontal mirror as a half turn: these failed with the scan
// upright and the player turned 180 degrees, and with the scan turned a quarter (6) and the player
// three quarters (8).
static void should_report_a_mirrored_container_to_the_player()
{
	const auto original = read_test_file(test_files_folder.combine("excluded1").combine_file("rotated90.mp4"));

	// The matrix's first four entries, 16.16 fixed point, of the track rotated90.mp4 turns a quarter.
	constexpr uint32_t one = 0x00010000u;
	constexpr uint32_t minus_one = 0xFFFF0000u;
	const auto load_be32 = [](const std::string& s, const size_t at)
	{
		return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 16 |
			static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 8 | static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3]));
	};

	size_t matrix_at = 0;

	for (auto at = original.find("tkhd"); at != std::string::npos && matrix_at == 0; at = original.find("tkhd", at + 4))
	{
		// After the type: version and flags, the times, track and duration - wider in version 1 - then
		// eight reserved bytes, layer, group, volume and two more reserved.
		const auto version = static_cast<uint8_t>(original[at + 4]);
		const auto candidate = at + 4 + 4 + (version == 1 ? 32 : 20) + 8 + 8;

		if (candidate + 16 <= original.size() && load_be32(original, candidate) == 0 && load_be32(original, candidate + 4) == one &&
			load_be32(original, candidate + 8) == 0 && load_be32(original, candidate + 12) == minus_one)
		{
			matrix_at = candidate;
		}
	}

	assert_equal(true, matrix_at != 0, "rotated90.mp4 has a track turned a quarter");

	struct variant
	{
		const char* name;
		// The matrix's a, b, c and d; the projection entry u between b and c keeps its value.
		uint32_t a, b, c, d;
		ui::orientation expected;
	};

	const variant variants[] = {
		{"mirrored", minus_one, 0, 0, one, ui::orientation::top_right},
		{"mirrored, then turned a quarter", 0, minus_one, minus_one, 0, ui::orientation::right_bottom},
	};

	for (const auto& v : variants)
	{
		auto bytes = original;
		const auto store_be32 = [&bytes](const size_t at, const uint32_t value)
		{
			for (auto i = 0; i < 4; ++i) bytes[at + i] = static_cast<char>(value >> (24 - 8 * i) & 0xFF);
		};

		store_be32(matrix_at, v.a);
		store_be32(matrix_at + 4, v.b);
		store_be32(matrix_at + 12, v.c);
		store_be32(matrix_at + 16, v.d);

		const auto path = _temps.next_path(".mp4");
		write_test_file(path, bytes);

		files ff;
		const auto scanned = ff_scan_file(ff, path).to_props();
		assert_equal(v.expected, scanned->orientation, std::format("scan reads a track {}", v.name));

		const auto ses = make_test_session();
		assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false, false, false),
		             "session opened");
		assert_equal(v.expected, ses->info().display_orientation, std::format("player agrees with the scan: {}", v.name));
		ses->close(false);
	}
}

// A media seek can only land on a key frame, so the caller has to say which side of the
// requested time it may land on. avformat_seek_file clears AVSEEK_FLAG_BACKWARD and instead
// derives the direction from the min/max window, so the window centred on the target that
// this used to pass always resolved to the key frame *after* the request. Nothing can decode
// backwards from there, so the scrubber preview - and playback resuming at a saved position -
// silently skipped up to a whole GOP of content. indy.mp4 has ~10s between key frames, which
// is far wider than the tolerance here.
static void should_seek_to_the_frame_at_the_requested_time()
{
	const auto load_path = test_files_folder.combine_file("indy.mp4");

	av_format_decoder decoder;
	assert_equal(true, decoder.open(load_path, media_intent::thumbnail), "open indy.mp4");
	decoder.init_streams(-1, -1, false, false, false);
	assert_equal(true, decoder.has_video(), "indy.mp4 has video");

	constexpr sizei max_dim(256, 256);
	const auto start = decoder.start_time();
	const auto len = decoder.end_time() - start;
	assert_equal(true, len > 60.0, "indy.mp4 is long enough to span several key frames");

	constexpr double numerator = 60;
	constexpr double denominator = 100;
	const auto wanted = start + floor(numerator * len / denominator);

	ui::surface_ptr s;
	assert_equal(true, decoder.extract_seek_frame(s, max_dim, numerator, denominator), "seek frame decoded");
	assert_equal(true, is_valid(s), "seek frame valid");
	assert_equal(true, fabs(s->time() - wanted) < 1.0,
	             std::format("decoded frame is at the requested time (wanted {}, got {})", wanted, s->time()));
}

// The library thumbnail is the key frame at or before a tenth of the way in, decoded once rather
// than walked to. Skipping the seek for any position under two seconds - which is every clip
// shorter than twenty - silently thumbnailed those from frame zero instead of the tenth asked for.
static void should_seek_short_video_thumbnails()
{
	const auto path = test_files_folder.combine_file("StPauls.MOV");

	av_format_decoder dec;
	assert_equal(true, dec.open(path, media_intent::thumbnail), "decoder opened");
	dec.init_streams(-1, -1, false, true, false);
	assert_equal(true, dec.has_video(), "StPauls.MOV has video");

	const auto duration = dec.end_time() - dec.start_time();
	assert_equal(true, duration > 2.0 && duration < 20.0, "the clip is short enough to have skipped the seek");

	ui::surface_ptr thumbnail;
	assert_equal(true, dec.extract_thumbnail(thumbnail, {256, 256}, 10, 100, false), "thumbnail decoded");
	assert_equal(true, is_valid(thumbnail), "thumbnail surface");

	const auto wanted = duration / 10.0;

	assert_equal(true, thumbnail->time() > 0.0,
	             std::format("a short clip thumbnails from {:.2f}s, not frame zero", thumbnail->time()));
	assert_equal(true, thumbnail->time() <= wanted + 0.5,
	             std::format("the key frame is at or before {:.2f}s (got {:.2f}s)", wanted, thumbnail->time()));

	dec.close();
}

// A clip with no audio track has no device clock, so it is timed off the wall clock and the
// stream's own end is the only thing that separates "played out" from "decode fell behind". That
// end-of-stream marker carries no media timestamp, so leaving it at the head of the frame queue
// made front_time() report zero and every distance comparison refuse to look past it - the marker
// was never consumed and the clip could only end on the two-second hard fallback.
static void should_end_a_silent_clip_at_the_stream_end()
{
	df::file_path silent_path;

	for (const auto* const name : {"anamorphic.mp4", "gizmo.mp4", "tagged.mkv", "tagged.webm"})
	{
		const auto candidate = test_files_folder.combine_file(name);

		av_format_decoder probe;
		if (!probe.open(candidate, media_intent::playback)) continue;
		probe.init_streams(-1, -1, false, false, false);

		if (probe.has_video() && !probe.has_audio())
		{
			silent_path = candidate;
			break;
		}
	}

	assert_equal(false, silent_path.is_empty(), "a video-only test file is available");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(silent_path, files::file_type_from_name(silent_path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");
	assert_equal(true, ses->is_playing(), "session auto-plays");

	const auto media_end = ses->info().end;
	assert_equal(true, media_end > 0.0, "the test clip declares a duration");

	auto now = df::now();
	assert_equal(false, ses->has_ended(now), "a freshly opened clip has not ended");

	// Drive the demux, decode and present work the player threads normally own.
	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);

	auto ended_at = -1.0;

	for (auto i = 0; i < 4000 && ended_at < 0.0; ++i)
	{
		ses->process_io(video_event, audio_event);
		ses->process_video(read_event);
		now += 0.02;
		ses->update_for_present(now);
		if (ses->has_ended(now)) ended_at = ses->pos(now);
	}

	assert_equal(true, ended_at >= 0.0, "the clip ends");
	assert_equal(true, ended_at >= media_end - 0.25,
	             std::format("does not end before the stream end (end {:.2f}, ended at {:.2f})",
	                         media_end, ended_at));
	assert_equal(true, ended_at < media_end + 1.0,
	             std::format("ends on the stream end, not the 2s fallback (end {:.2f}, ended at {:.2f})",
	                         media_end, ended_at));

	ses->close(false);
}

// Drives a video-only session's demux and decode until its frame queue holds frames, as the player threads
// would before the first present.
static void prime_video_only_session(const std::shared_ptr<av_session>& ses, const int passes)
{
	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);

	for (auto i = 0; i < passes; ++i)
	{
		ses->process_io(video_event, audio_event);
		ses->process_video(read_event);
	}
}

static bool is_signalled(platform::thread_event& e)
{
	return platform::wait_for({e}, 1, false) == 0;
}

// The presenter is the only thing that frees a place in the frame queue, so taking a frame wakes the decode
// thread. Without it a stream with no audio - whose interleaved packets would otherwise wake it - waited out
// the decode thread's poll with its queue running dry: three 4K frames last 50 ms at 60 fps, as long as the
// poll itself.
static void should_wake_the_decoder_when_the_presenter_takes_a_frame()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");
	const auto wake = std::make_shared<platform::thread_event>(false, false);
	const auto ses = make_test_session_with_decode_wake(wake);
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false, false, true, true),
	             "the video opens without its audio");
	const df::scope_exit close([ses] { ses->close(false); });

	prime_video_only_session(ses, 8);
	assert_equal(false, is_signalled(*wake), "decoding alone does not wake the decoder");

	auto now = df::now();
	auto presented = false;

	for (auto i = 0; i < 50 && !presented; ++i)
	{
		now += 0.02;
		presented = ses->update_for_present(now);
	}

	assert_equal(true, presented, "a frame is presented");
	assert_equal(true, is_signalled(*wake), "and taking it wakes the decoder");
}

// One pass decodes every packet already queued while the frame queue wants frames. Stopping after each
// packet left decoding waiting on the reader with a backlog queued, so a stalled read - a network share, a
// cloud file - throttled the picture to one packet per wake.
static void should_decode_queued_packets_without_waiting_on_the_reader()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");
	const auto ses = make_test_session();
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false, false, true, true),
	             "the video opens without its audio");
	const df::scope_exit close([ses] { ses->close(false); });

	prime_video_only_session(ses, 1);

	// Nothing decodes from here: every frame presented below came from that single pass.
	const auto interval = ses->video_frame_interval();
	auto now = df::now();
	auto presented = 0;

	for (auto i = 0; i < 40; ++i)
	{
		now += interval;
		if (ses->update_for_present(now)) ++presented;
	}

	assert_equal(true, presented >= 3,
	             std::format("one pass decodes the queued packets, not one of them (presented {})", presented));
}

// The clock can pass more than one frame between presents - content faster than the tick, or a tick that
// came late - and the presenter takes every frame it has reached and shows the nearest. Taking one a tick
// left the picture behind its clock for good.
static void should_present_the_nearest_frame_when_the_clock_passes_several()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");
	const auto ses = make_test_session();
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false, false, true, true),
	             "the video opens without its audio");
	const df::scope_exit close([ses] { ses->close(false); });

	prime_video_only_session(ses, 8);

	const auto interval = ses->video_frame_interval();
	auto now = df::now();
	auto presented = false;

	for (auto i = 0; i < 50 && !presented; ++i)
	{
		now += 0.001;
		presented = ses->update_for_present(now);
	}

	assert_equal(true, presented, "the first frame is presented");

	// Step once more so the clock is anchored to a presented frame, then let it pass four frames at once.
	now += interval;
	ses->update_for_present(now);
	const auto before = ses->time();

	now += interval * 4.0;
	ses->update_for_present(now);
	const auto advanced = ses->time() - before;

	assert_equal(true, advanced > interval * 3.0,
	             std::format("the picture keeps up with its clock (advanced {:.3f}s for {:.3f}s)", advanced,
	                         interval * 4.0));
}

// Movie's preview wants frames from a session and supplies its own sound, so the session opens
// without its audio stream. That is not a saving: process_io stops reading when *either* packet
// queue fills, so a session whose audio nothing drains stalls its own video and delivers nothing.
static void should_open_a_session_without_its_audio()
{
	df::file_path path;

	// Short, so driving it to the end is a test rather than a wait, and with sound, because opening
	// it without that sound is the whole claim.
	for (const auto* const name : {"StPauls.MOV", "tvp.mp4", "gizmo.mp4", "indy.mp4"})
	{
		const auto candidate = test_files_folder.combine_file(name);

		av_format_decoder probe;
		if (!probe.open(candidate, media_intent::metadata)) continue;
		probe.init_streams(-1, -1, false, false, false);

		const auto duration = probe.end_time() - probe.start_time();

		if (probe.has_audio() && probe.has_video() && duration > 0.0 && duration < 30.0)
		{
			path = candidate;
			break;
		}
	}

	assert_equal(false, path.is_empty(), "a short test clip with sound is available");

	const auto file_type = files::file_type_from_name(path);
	const auto ses = make_test_session();

	assert_equal(true, ses->open(path, file_type, 0.0, true, -1, -1, false, false, true, true),
	             "session opened video only");

	assert_equal(true, ses->video_stream_id() >= 0, "the video stream is open");
	assert_equal(-1, ses->audio_stream_id(), "and the audio stream is not");
	assert_equal(false, ses->has_audio_clock(), "so the wall clock times it, as a silent clip is timed");

	// Drive the demux, decode and present work the player threads normally own. Reaching the end of
	// the stream is the claim: an undrained audio queue stops the reader, so a stalled session runs
	// out of frames part way through and never gets there.
	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);

	auto now = df::now();
	auto presented = 0;
	auto ended = false;
	auto ended_at = -1.0;

	for (auto i = 0; i < 3000 && !ended; ++i)
	{
		ses->process_io(video_event, audio_event);
		ses->process_video(read_event);
		now += 0.02;
		if (ses->update_for_present(now)) ++presented;
		ended = ses->has_ended(now);
		if (ended) ended_at = ses->pos(now);
	}

	assert_equal(true, presented > 10,
	             std::format("a video-only session keeps presenting frames (presented {})", presented));
	assert_equal(true, ended, "and reaches the end of the stream rather than stalling part way");
	assert_equal(true, ended_at >= ses->info().end - 0.5,
	             std::format("video-only playback reaches the stream end (end {:.2f}, ended at {:.2f})",
	                         ses->info().end, ended_at));
	assert_equal(true, ended_at < ses->info().end + 1.0,
	             std::format("video-only playback does not finish on the timeout fallback (end {:.2f}, ended at {:.2f})",
	                         ses->info().end, ended_at));

	ses->close(false);
}

// MEDIA-003 - an unavailable endpoint still selects the audio stream, so the selected audio queue
// has to be consumed or it back-pressures demux and the video queue eventually runs dry.
static void should_keep_video_moving_when_audio_output_is_unavailable()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");
	assert_equal(true, ses->info().has_audio && ses->info().has_video, "fixture has both streams");

	ses->mark_audio_output_unavailable();
	assert_equal(false, ses->has_audio_clock(), "the session falls back to the wall clock");
	assert_equal(true, should_drain_audio_while_output_unavailable(true, false),
	             "selected audio drains while output is unavailable");
	assert_equal(false, should_drain_audio_while_output_unavailable(true, true),
	             "available output keeps audio for playback");

	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);

	auto now = df::now();
	auto presented = 0;
	auto last_presented = 0.0;

	for (auto i = 0; i < 1200 && last_presented < 4.0; ++i)
	{
		ses->process_io(video_event, audio_event);
		ses->process_video(read_event);
		ses->discard_audio_while_output_unavailable(read_event);
		now += 0.02;
		if (ses->update_for_present(now))
		{
			++presented;
			last_presented = ses->time();
		}
	}

	assert_equal(true, presented > 10,
	             std::format("video keeps presenting while audio is discarded (presented {})", presented));
	assert_equal(true, last_presented >= 4.0,
	             std::format("video advances through later timestamps (got {:.2f})", last_presented));

	ses->close(false);
}

// MEDIA-003 - failed endpoint retries happen repeatedly while the discard path keeps demux moving.
// Only the transition to unavailable may re-anchor the wall clock; repeated failures must not keep
// snapping playback back to the accepted seek position.
static void should_not_reanchor_playback_time_on_repeated_audio_output_failures()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");

	const auto start = df::now();
	ses->mark_audio_output_unavailable(start);
	assert_equal(1.0, ses->pos(start + 1.0), "first failure starts the wall clock");

	ses->mark_audio_output_unavailable(start + 1.0);
	assert_equal(2.0, ses->pos(start + 2.0), "repeated failure does not reset the wall clock");

	ses->close(false);
}

// MEDIA-003 - audio endpoint recovery runs on the audio thread, but FFmpeg seeks must be serialized
// with demux on the read thread. Recovery therefore queues a seek request that reading() applies
// before process_io, rather than seeking immediately from the audio path.
static void should_queue_audio_recovery_seek_for_the_read_thread()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");

	const auto now = df::now();
	assert_equal(0.0, ses->pos(now), "opened session is still at the accepted start");

	ses->request_audio_recovery_seek(now);
	assert_equal(0.0, ses->pos(now), "requesting recovery does not seek on the audio thread");

	assert_equal(true, ses->process_pending_forced_seek(), "read thread applies recovery seek");
	assert_equal(0.0, ses->pos(now), "recovery seek is visible after read-thread processing");
	assert_equal(false, ses->process_pending_forced_seek(), "recovery request is single-shot");

	ses->close(false);
}

// MEDIA-003 - a recovery request captures the seek generation observed by the audio thread. If a
// queued user seek runs before the read thread processes recovery, the stale request is ignored.
static void should_drop_stale_audio_recovery_seek()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");

	const auto now = df::now();
	ses->request_audio_recovery_seek(now);
	ses->seek(4.0, false);

	assert_equal(false, ses->process_pending_forced_seek(), "stale recovery request is discarded");
	assert_equal(4.0, ses->pos(now), "newer user seek remains current");

	ses->close(false);
}

// Playing a clip to judge a trim is not the user watching that file. Saving a position for it would
// overwrite the resume point they set by actually watching it, silently and on every scrub.
static void should_not_save_a_position_for_borrowed_playback()
{
	const auto path = test_files_folder.combine_file("indy.mp4");
	const auto file_type = files::file_type_from_name(path);

	auto saved_for = df::file_path{};
	auto save_count = 0;

	const auto record = [&saved_for, &save_count](const df::file_path p, double)
	{
		saved_for = p;
		++save_count;
	};

	// The control: ordinary playback still records where it was left.
	const auto watched = make_test_session(record);
	assert_equal(true, watched->open(path, file_type, 0.0, true, -1, -1, false, false, true), "session opened");
	watched->close(true);
	assert_equal(1, save_count, "watching a file saves its position");
	assert_equal(path.pack(), saved_for.pack(), "and saves it against that file");

	const auto borrowed = make_test_session(record);
	borrowed->remembers_position(false);
	assert_equal(true, borrowed->open(path, file_type, 0.0, true, -1, -1, false, false, true), "session opened");
	borrowed->close(true);

	assert_equal(1, save_count, "playback that stands for something else leaves the position alone");
}

// Read-ahead used to be counted in frames alone, so what it cost depended entirely on the
// resolution: sixteen queued 1920x816 frames measured 63 MB of process commit, and 4K is four
// times the frame. The budget is stated in bytes now, and this holds the queue to it.
static void should_bound_video_read_ahead_by_bytes()
{
	const auto path = test_files_folder.combine_file("indy.mp4");

	av_format_decoder dec;
	assert_equal(true, dec.open(path, media_intent::playback), "decoder opened");
	dec.init_streams(-1, -1, false, false, true);
	assert_equal(true, dec.has_video(), "indy.mp4 has video");

	av_packet_queue packets;
	av_frame_queue decoded;
	av_frame_queue video;
	auto at_end = false;

	for (auto i = 0; i < 3000 && !at_end && video.should_receive(); ++i)
	{
		auto p = dec.read_packet();
		if (!p) break;

		packets.push(p);
		dec.receive_frames(packets, decoded);

		// The decoder queue carries both streams and the end-of-stream marker; only the video
		// frames are under test.
		for (av_frame_ptr f; decoded.pop(f); f.reset())
		{
			if (av_frame_is_eof(f)) at_end = true;
			else if (!av_is_frame_empty(f)) video.push(f);
		}
	}

	// Otherwise the queue stopped because the clip ran out, and the budget was never tested.
	assert_equal(false, at_end, "the clip is long enough to fill the read-ahead budget");

	size_t count = 0;
	size_t bytes = 0;
	size_t frame_bytes = 0;

	for (av_frame_ptr f; video.pop(f); f.reset())
	{
		frame_bytes = av_queued_payload_bytes(f);
		bytes += frame_bytes;
		++count;
	}

	assert_equal(true, frame_bytes > 0, "a decoded frame charges what its buffers cost");
	assert_equal(true, count >= av_read_ahead_min_frames, "read-ahead keeps enough frames to absorb decode jitter");
	assert_equal(true, count < av_read_ahead_max_frames,
	             std::format("the byte budget is reached before the frame count cap ({} frames)", count));

	// The queue only stops asking once the budget is met, so it can overshoot by the frames the
	// packet in flight produced - but by no more than that.
	assert_equal(true, bytes <= av_read_ahead_bytes + frame_bytes,
	             std::format("read-ahead stays inside its budget ({} frames, {} bytes)", count, bytes));

	dec.close();
}

// Scrubbing sets the wall clock from the position the user asked for, then the audio device
// re-anchors it to the first sample it is handed. Those two have to agree: if the audio timeline
// lands somewhere other than the sought position, pos() jumps when the device starts and the view
// sits frozen until the clock catches back up to the frame already on screen.
// A seek can only land on a key sample. gizmo.mp4 carries a single video key frame, so seeking
// anywhere in it puts the demuxer back at the start. update_for_present settles the video forward
// onto the position asked for; audio has no such step, so without trimming it the buffer - and the
// device clock anchored to it - starts at the key sample and the view sits frozen on the settled
// frame until the clock catches back up. That freeze is what a scrub used to produce.
static void should_land_audio_and_video_on_the_sought_position()
{
	const auto load_path = test_files_folder.combine_file("gizmo.mp4");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(load_path, files::file_type_from_name(load_path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");

	const auto media_end = ses->info().end;
	assert_equal(true, ses->info().has_audio && ses->info().has_video, "gizmo.mp4 has both streams");

	constexpr auto wanted = 3.0;
	assert_equal(true, media_end > wanted + 1.0, "the clip is long enough to seek into");

	ses->seek(wanted, true);

	audio_info_t fmt;
	fmt.channel_layout = av_get_def_channel_layout(2);
	fmt.sample_fmt = prop::audio_sample_t::signed_16bit;
	fmt.sample_rate = 48000;

	audio_buffer playback_buffer;
	audio_buffer vis_buffer;
	playback_buffer.init(fmt);
	vis_buffer.init(fmt);

	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);

	auto now = df::now();

	for (auto i = 0; i < 200; ++i)
	{
		ses->process_io(video_event, audio_event);
		ses->process_video(read_event);
		ses->process_audio(playback_buffer, vis_buffer, read_event);
		now += 0.02;
		ses->update_for_present(now);
	}

	assert_equal(false, playback_buffer.is_empty(), "audio decoded after the seek");

	const auto audio_at = playback_buffer.start_time();
	const auto video_at = ses->time();

	assert_equal(true, fabs(audio_at - wanted) < 0.5,
	             std::format("audio lands on the sought position (wanted {:.2f}, got {:.2f})", wanted, audio_at));
	assert_equal(true, fabs(video_at - wanted) < 0.5,
	             std::format("video lands on the sought position (wanted {:.2f}, got {:.2f})", wanted, video_at));

	ses->close(false);
}

class playback_time_test_texture final : public ui::texture
{
public:
	ui::texture_update_result update(const ui::video_frame_ptr& frame) override
	{
		return frame ? ui::texture_update_result::tex_updated : ui::texture_update_result::failed;
	}

	ui::texture_update_result update(const ui::const_surface_ptr&) override
	{
		return ui::texture_update_result::failed;
	}

	ui::texture_update_result update(sizei, ui::texture_format, ui::orientation,
	                                 const uint8_t*, size_t, size_t) override
	{
		return ui::texture_update_result::failed;
	}

	bool is_valid() const override { return true; }
};

static void should_refresh_paused_media_time_after_seeking()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const view_host_base_ptr view;
	state.view_mode(view_type::items);
	state.open(view, df::search_t().add_selector(test_files_folder).with("gizmo.mp4"), {});
	state.update_item_groups();
	const auto item = state.find_displayed_item_by_name("gizmo.mp4");
	assert_equal(true, item != nullptr, "the video is visible");
	state.select(view, item, false, false, false);
	state.update_selection();
	const auto display = state.display_state();
	assert_equal(true, display != nullptr, "selection has a display");

	const auto path = test_files_folder.combine_file("gizmo.mp4");
	const auto session = make_test_session();
	assert_equal(true, session->open(path, files::file_type_from_name(path), 0.0, false, -1, -1,
	                                false, false, false), "paused session opened");
	const df::scope_exit close([session] { session->close(false); });
	session->mark_audio_output_unavailable();
	display->update_av_session(session);

	const auto texture = std::make_shared<playback_time_test_texture>();
	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);
	auto now = df::now();

	for (const auto target : {0.0, 3.0, 1.0})
	{
		session->seek(target, false);
		for (auto i = 0; i < 300 && std::abs(session->last_frame_time() - target) > 0.2; ++i)
		{
			session->process_io(video_event, audio_event);
			session->process_video(read_event);
			now += 0.02;
			session->update_for_present(now);
			session->update_texture(texture);
		}

		assert_equal(true, std::abs(session->last_frame_time() - target) <= 0.2, "seek presents the requested frame");
		assert_equal(false, session->is_playing(), "seeking does not resume playback");
		if (target == 0.0) display->update_scrubber();
		state.tick(view, now);
		assert_equal(str::format_seconds(df::round(session->last_frame_time())), display->_time,
		             "paused elapsed text follows the presented frame without a hover callback");
	}
}

// A playing video hides its controls, which then draw at alpha 0. Repainting the whole view whenever the
// elapsed time or the bar moved changed no pixels, and cost a scene build and a present each time.
static void should_repaint_the_scrubber_only_while_the_controls_show()
{
	struct recording_async_strategy final : null_async_strategy
	{
		view_invalid invalids = view_invalid::none;

		void invalidate_view(const view_invalid invalid) override
		{
			invalids |= invalid;
		}
	};

	recording_async_strategy as;
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(as, common);

	const auto path = test_files_folder.combine_file("gizmo.mp4");
	const auto session = make_test_session();
	assert_equal(true, session->open(path, files::file_type_from_name(path), 0.0, false, -1, -1,
	                                false, false, false), "paused session opened");
	const df::scope_exit close([session] { session->close(false); });
	session->mark_audio_output_unavailable();
	display->update_av_session(session);
	display->_scrubber_width = 1000;

	const auto texture = std::make_shared<playback_time_test_texture>();
	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);
	auto now = df::now();

	const auto present_at = [&](const double target)
	{
		session->seek(target, false);

		for (auto i = 0; i < 300 && std::abs(session->last_frame_time() - target) > 0.2; ++i)
		{
			session->process_io(video_event, audio_event);
			session->process_video(read_event);
			now += 0.02;
			session->update_for_present(now);
			session->update_texture(texture);
		}

		assert_equal(true, std::abs(session->last_frame_time() - target) <= 0.2, "the sought frame is presented");
	};

	present_at(3.0);
	as.invalids = view_invalid::none;
	display->update_scrubber(false);
	assert_equal(str::format_seconds(df::round(session->last_frame_time())), display->_time,
	             "hidden controls still follow the presented frame");
	assert_equal(false, as.invalids && view_invalid::view_redraw, "but do not repaint the view");

	present_at(1.0);
	display->update_scrubber(true);
	assert_equal(str::format_seconds(df::round(session->last_frame_time())), display->_time,
	             "shown controls follow the presented frame");
	assert_equal(true, as.invalids && view_invalid::view_redraw, "and repaint the view when they move");
}

// MEDIA-004 - duplicate suppression is only for an outstanding request. Once playback has moved
// away from a settled target, asking for that target again is a real seek.
static void should_seek_to_the_same_position_after_playback_has_advanced()
{
	const auto load_path = test_files_folder.combine_file("gizmo.mp4");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(load_path, files::file_type_from_name(load_path), 0.0, true, -1, -1, false,
	                             false, false), "session opened");
	assert_equal(true, ses->info().has_video, "fixture has video");
	ses->mark_audio_output_unavailable();

	constexpr auto wanted = 3.0;
	ses->seek(wanted, false);

	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);

	auto now = df::now();

	for (auto i = 0; i < 300 && std::abs(ses->time() - wanted) > 0.75; ++i)
	{
		ses->process_io(video_event, audio_event);
		ses->process_video(read_event);
		now += 0.02;
		ses->update_for_present(now);
	}

	assert_equal(true, std::abs(ses->time() - wanted) < 0.75,
	             std::format("first seek settles at {:.2f} (got {:.2f})", wanted, ses->time()));
	const auto first_settled = ses->time();

	for (auto i = 0; i < 300 && ses->time() < first_settled + 1.0; ++i)
	{
		ses->process_io(video_event, audio_event);
		ses->process_video(read_event);
		now += 0.02;
		ses->update_for_present(now);
	}

	assert_equal(true, ses->time() > first_settled + 0.5,
	             std::format("playback advanced away from the seek target (got {:.2f})", ses->time()));

	ses->seek(wanted, false);

	for (auto i = 0; i < 300 && std::abs(ses->time() - first_settled) > 0.25; ++i)
	{
		ses->process_io(video_event, audio_event);
		ses->process_video(read_event);
		now += 0.02;
		ses->update_for_present(now);
	}

	assert_equal(true, std::abs(ses->time() - first_settled) < 0.25,
	             std::format("second identical seek returns to {:.2f} (got {:.2f})", wanted, ses->time()));

	ses->close(false);
}

// FFmpeg offers hardware surfaces only for 4:2:0, and for H.264, MPEG-2 and VC-1 only at 8 bits. A
// stream outside that has to decode in software from the start: handed a hardware decoder, it had no
// surface to receive and playback showed no picture at all.
static void should_decide_which_streams_can_decode_in_hardware()
{
	assert_equal(true, av_hw_decode_eligible(AV_CODEC_ID_H264, AV_PIX_FMT_YUV420P), "8-bit 4:2:0 H.264");
	assert_equal(false, av_hw_decode_eligible(AV_CODEC_ID_H264, AV_PIX_FMT_YUV420P10LE), "not 10-bit H.264");
	assert_equal(false, av_hw_decode_eligible(AV_CODEC_ID_H264, AV_PIX_FMT_YUV422P), "not 4:2:2 H.264");
	assert_equal(true, av_hw_decode_eligible(AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV420P10LE), "HEVC Main 10");
	assert_equal(false, av_hw_decode_eligible(AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV422P10LE), "not 4:2:2 HEVC");
	assert_equal(false, av_hw_decode_eligible(AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV444P), "not 4:4:4 HEVC");
	assert_equal(true, av_hw_decode_eligible(AV_CODEC_ID_AV1, AV_PIX_FMT_YUV420P10LE), "10-bit AV1");
	assert_equal(true, av_hw_decode_eligible(AV_CODEC_ID_VP9, AV_PIX_FMT_YUV420P), "VP9 profile 0");
	assert_equal(false, av_hw_decode_eligible(AV_CODEC_ID_MPEG2VIDEO, AV_PIX_FMT_YUV422P), "not 4:2:2 MPEG-2");
	assert_equal(false, av_hw_decode_eligible(AV_CODEC_ID_MPEG4, AV_PIX_FMT_YUV420P), "no hwaccel for MPEG-4 part 2");
	assert_equal(true, av_hw_decode_eligible(AV_CODEC_ID_H264, AV_PIX_FMT_NONE),
	             "an undeclared format is left for the decoder to settle");
}

// Slice threading holds no extra pictures, so cores alone bound it; frame threading holds one per
// thread, so what a picture costs bounds it too.
static void should_bound_video_decode_threads()
{
	constexpr size_t hd = 1920 * 1080 * 3 / 2;
	constexpr size_t uhd_422_10bit = 3840 * 2160 * 4;
	constexpr size_t eight_k_10bit = 7680 * 4320 * 3;

	assert_equal(8, av_video_decode_threads(true, hd, 32), "1080p takes the full eight on a large machine");
	assert_equal(4, av_video_decode_threads(true, hd, 4), "and no more threads than cores");
	assert_equal(6, av_video_decode_threads(true, uhd_422_10bit, 32), "10-bit 4:2:2 4K is held by its picture budget");
	assert_equal(2, av_video_decode_threads(true, eight_k_10bit, 32), "8K keeps two");
	assert_equal(8, av_video_decode_threads(false, eight_k_10bit, 32), "slice threads hold no pictures");
	assert_equal(1, av_video_decode_threads(true, hd, 0), "an unknown core count still decodes");
}

// Hardware decode is on by default, so these are the streams a camera user actually meets: 10-bit
// H.264 and 4:2:2 HEVC. Before the eligibility check the session attached a hardware decoder FFmpeg
// could not use and never produced a picture -- on a machine with a GPU, which is where this fails.
// Neither stream reaches a hardware device now, so the test leaves the persisted crash guard alone.
static void should_play_streams_the_hardware_path_cannot_decode()
{
	for (const auto* const name : {"h264-10bit.mp4", "hevc-422-10bit.mp4"})
	{
		const auto path = test_files_folder.combine("excluded1").combine_file(name);

		const auto ses = make_test_session();
		assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, true, false,
		                             true, true), std::format("{} opened with hardware decode allowed", name));

		const platform::thread_event video_event(false, false);
		const platform::thread_event audio_event(false, false);
		const platform::thread_event read_event(false, false);

		auto presented = false;
		auto now = df::now();

		for (auto i = 0; i < 200 && !presented; ++i)
		{
			ses->process_io(video_event, audio_event);
			ses->process_video(read_event);
			now += 0.02;
			presented = ses->update_for_present(now);
		}

		assert_equal(true, presented, std::format("{} presents a picture", name));
		ses->close(false);
	}
}

// The first picture a playback decoder hands the presenter, prepared for upload as playback prepares it.
static av_frame_ptr first_presented_picture(const df::file_path path)
{
	av_format_decoder decoder;
	av_frame_ptr result;

	if (!decoder.open(path, media_intent::playback)) return result;
	decoder.init_streams(-1, -1, false, true, false);

	av_packet_queue packets;
	av_frame_queue frames;

	for (auto i = 0; i < 200 && !result; ++i)
	{
		const auto packet = decoder.read_packet();
		if (!packet || av_packet_is_eof(packet)) break;
		if (av_packet_stream_index(packet) != decoder.video_stream_id()) continue;

		packets.push(packet);
		decoder.receive_frames(packets, frames);

		av_frame_ptr frame;
		if (frames.pop(frame) && !av_frame_is_eof(frame)) result = frame;
	}

	return result;
}

// The decode thread writes each prepared picture over one the queue, the presenter and the renderer have let
// go, rather than allocating a frame's worth of fresh pages for every frame. One still held is never written
// over.
static void should_reuse_a_prepared_picture_once_it_is_let_go()
{
	auto* frame = av_frame_alloc();
	const df::scope_exit free_frame([&frame] { av_frame_free(&frame); });
	assert_equal(true, frame != nullptr, "a frame");
	if (!frame) return;

	frame->format = AV_PIX_FMT_YUV420P;
	frame->width = 64;
	frame->height = 48;
	assert_equal(true, av_frame_get_buffer(frame, 0) == 0, "with planes");

	for (auto plane = 0; plane < 3; ++plane)
	{
		const auto rows = plane == 0 ? frame->height : frame->height / 2;
		memset(frame->data[plane], 128, static_cast<size_t>(frame->linesize[plane]) * rows);
	}

	av_scaler scaler;
	auto first = scaler.presentable_surface(*frame, 1.0, ui::orientation::top_left);
	assert_equal(true, first != nullptr, "a picture is prepared");
	if (!first) return;

	const auto* const first_surface = first.get();
	const auto* const first_pixels = first->pixels();

	const auto held = scaler.presentable_surface(*frame, 2.0, ui::orientation::top_left);
	assert_equal(true, held && held.get() != first_surface, "one still held is not written over");

	first.reset();
	const auto again = scaler.presentable_surface(*frame, 3.0, ui::orientation::top_left);
	assert_equal(true, again.get() == first_surface, "one let go is");
	assert_equal(true, again && again->pixels() == first_pixels, "in the block it had");
	assert_equal(3.0, again ? again->time() : 0.0, "as the new picture");
}

// A decoded picture is converted on the decode thread, so presenting it is an upload. The planes have
// to be the ones the renderer samples, laid out as it reads them -- NV12 or P010 with the chroma
// interleaved U then V -- or BGRA for a renderer that samples no YUV. Converting the prepared planes
// back must give the picture a direct conversion gives, which a swapped or misplaced plane does not.
// A device can sample NV12 and refuse P010; a 10-bit picture handed to it as planes it cannot upload
// would never be shown, so it gets BGRA.
static void should_prepare_decoded_pictures_for_upload()
{
	const auto yuv_before = ui::yuv_textures_enabled;
	const auto p010_before = ui::p010_textures_enabled;
	const df::scope_exit restore_yuv([yuv_before, p010_before]
	{
		ui::yuv_textures_enabled = yuv_before;
		ui::p010_textures_enabled = p010_before;
	});

	const auto close_to = [](const ui::color32 a, const ui::color32 b)
	{
		for (auto shift = 0; shift < 24; shift += 8)
		{
			if (std::abs(static_cast<int>((a >> shift) & 0xff) - static_cast<int>((b >> shift) & 0xff)) > 12) return false;
		}

		return true;
	};

	for (const auto& [name, planar] : {std::pair{"anamorphic.mp4", ui::texture_format::NV12},
	                                  std::pair{"excluded1/h264-10bit.mp4", ui::texture_format::P010}})
	{
		const auto path = std::string_view(name).starts_with("excluded1/")
			                  ? test_files_folder.combine("excluded1").combine_file(std::string_view(name).substr(10))
			                  : test_files_folder.combine_file(name);

		ui::yuv_textures_enabled = false;
		ui::p010_textures_enabled = false;
		const auto packed = av_frame_surface(first_presented_picture(path));
		assert_equal(true, packed && packed->format() == ui::texture_format::RGB,
		             std::format("{} is prepared as BGRA for a renderer without YUV", name));

		ui::yuv_textures_enabled = true;
		const auto nv12_only = av_frame_surface(first_presented_picture(path));
		const auto nv12_only_format = planar == ui::texture_format::NV12 ? planar : ui::texture_format::RGB;
		assert_equal(true, nv12_only && nv12_only->format() == nv12_only_format,
		             std::format("{} is prepared as {} for a device that samples NV12 but not P010", name,
		                         to_string(nv12_only_format)));

		ui::p010_textures_enabled = true;
		const auto planes = av_frame_surface(first_presented_picture(path));
		assert_equal(true, planes && planes->format() == planar,
		             std::format("{} is prepared as {} for a renderer that samples it", name, to_string(planar)));

		if (!packed || !planes) continue;

		assert_equal(true, packed->dimensions() == planes->dimensions(), "at the picture's own size");

		av_scaler scaler;
		const auto unpacked = std::make_shared<ui::surface>();
		assert_equal(true, scaler.convert_yuv_surface(*planes, unpacked), "the planes convert back");

		const auto size = packed->dimensions();
		auto mismatches = 0;
		auto compared = 0;

		// Compared only where the picture is flat: the two conversions upsample chroma differently, so
		// they part at every edge, while a swapped or misplaced plane changes the colour of flat areas.
		const auto flat = [&](const int x, const int y)
		{
			for (const auto& [dx, dy] : {std::pair{-2, 0}, std::pair{2, 0}, std::pair{0, -2}, std::pair{0, 2}})
			{
				if (!close_to(packed->get_pixel(x, y), packed->get_pixel(x + dx, y + dy))) return false;
			}

			return true;
		};

		for (auto y = 2; y < size.cy - 2; y += 3)
		{
			for (auto x = 2; x < size.cx - 2; x += 3)
			{
				if (!flat(x, y)) continue;

				++compared;
				if (!close_to(packed->get_pixel(x, y), unpacked->get_pixel(x, y))) ++mismatches;
			}
		}

		assert_equal(true, compared > 50, std::format("{} has flat areas to compare ({})", name, compared));
		assert_equal(0, mismatches, std::format("{} planes hold the same picture", name));
	}
}

// FFmpeg's own AV1 decoder only drives a hardware accelerator, so before libdav1d was registered an
// AV1 file had no software decoder at all: no thumbnail, no hover preview, no Movie frame.
static void should_decode_av1_without_hardware()
{
	const auto* const preferred = avcodec_find_decoder(AV_CODEC_ID_AV1);
	assert_equal(true, preferred && std::string_view(preferred->name) == "libdav1d",
	             "software AV1 decodes with libdav1d");
	assert_equal(true, avcodec_find_decoder_by_name("av1") != nullptr,
	             "and FFmpeg's own decoder stays registered for the hardware path");

	const auto path = test_files_folder.combine("excluded1").combine_file("av1.mp4");

	av_format_decoder decoder;
	assert_equal(true, decoder.open(path, media_intent::thumbnail), "AV1 file opened");
	decoder.init_streams(-1, -1, false, true, false);
	assert_equal(true, decoder.has_video(), "an AV1 decoder exists without the GPU");

	ui::surface_ptr thumbnail;
	assert_equal(true, decoder.extract_thumbnail(thumbnail, {64, 64}, 1, 100), "AV1 thumbnail decoded");
	assert_equal(true, is_valid(thumbnail), "with pixels");
}

// The Streams table names what a video stream holds beyond its codec: the profile, which decides what
// can play it, the bits a sample carries, and the HDR transfer the picture is graded for. FFmpeg built
// with --enable-small has no profile names at all, so a missing name is a build regression here.
static void should_describe_a_video_stream_profile_depth_and_transfer()
{
	struct expected_stream
	{
		std::string_view name;
		std::string_view profile;
		int bit_depth;
		std::string_view hdr_transfer;
	};

	for (const auto& e : {expected_stream{"gizmo.mp4", "Constrained Baseline", 8, ""},
	                      expected_stream{"excluded1/hdr-pq.mp4", "Main 10", 10, "PQ"},
	                      expected_stream{"excluded1/hdr-hlg.mp4", "Main 10", 10, "HLG"}})
	{
		const auto path = e.name.starts_with("excluded1/")
			                  ? test_files_folder.combine("excluded1").combine_file(e.name.substr(10))
			                  : test_files_folder.combine_file(e.name);

		av_format_decoder decoder;
		assert_equal(true, decoder.open(path, media_intent::thumbnail), std::format("{} opened", e.name));

		const auto info = decoder.info();
		const auto video = std::ranges::find_if(info.streams, [](const av_stream_info& s)
		{
			return s.type == av_stream_type::video;
		});

		assert_equal(true, video != info.streams.end(), std::format("{} has a video stream", e.name));
		if (video == info.streams.end()) continue;

		assert_equal(e.profile, std::string_view(video->profile), std::format("{} profile", e.name));
		assert_equal(e.bit_depth, video->bit_depth, std::format("{} bit depth", e.name));
		assert_equal(e.hdr_transfer, std::string_view(video->hdr_transfer), std::format("{} HDR transfer", e.name));
	}
}

// Each fixture is a flat field of HDR reference white, and each route a picture takes must show it at
// the level its transfer sets: the thumbnail, the BGRA a renderer without YUV draws, and the P010
// planes and cube a YUV renderer samples -- whose CPU arithmetic has to agree with the BGRA, so the
// two backends show the same picture.
//
// PQ is absolute light, and its reference white is the light an SDR picture shows as white; read as
// SDR it is a dull grey, 148. HLG is relative and is rendered for a display whose peak is SDR white,
// on which FFmpeg's BT.2100 HLG EOTF, at the system gamma of 1.0 it uses below 1000 nits, puts the
// fixture's 74% signal at a quarter of white's light: 144. Read as SDR it is 190, and rendered as
// 1000-nit light rolled off into white, as PQ is, it came out at 228 - which is what blew out phone
// video, whose HLG lies mostly above reference white.
static void should_tone_map_hdr_video_for_an_sdr_display()
{
	const auto yuv_before = ui::yuv_textures_enabled;
	const auto p010_before = ui::p010_textures_enabled;
	const df::scope_exit restore_yuv([yuv_before, p010_before]
	{
		ui::yuv_textures_enabled = yuv_before;
		ui::p010_textures_enabled = p010_before;
	});

	const auto centre = [](const ui::const_surface_ptr& s)
	{
		const auto px = s->get_pixel(s->dimensions().cx / 2, s->dimensions().cy / 2);
		return std::array{static_cast<int>(px >> 16 & 0xff), static_cast<int>(px >> 8 & 0xff), static_cast<int>(px & 0xff)};
	};

	struct expected_white
	{
		const char* name;
		int lowest;
		int highest;
		std::string_view shown;
	};

	for (const auto& e : {expected_white{"hdr-pq.mp4", 220, 255, "as white"},
	                      expected_white{"hdr-hlg.mp4", 139, 149, "at a quarter of white's light"}})
	{
		const auto* const name = e.name;
		const auto at_level = [&e](const std::array<int, 3>& rgb)
		{
			return std::ranges::min(rgb) >= e.lowest && std::ranges::max(rgb) <= e.highest;
		};

		const auto path = test_files_folder.combine("excluded1").combine_file(name);

		av_format_decoder decoder;
		assert_equal(true, decoder.open(path, media_intent::thumbnail), std::format("{} opened", name));
		decoder.init_streams(-1, -1, false, true, false);

		ui::surface_ptr thumbnail;
		assert_equal(true, decoder.extract_thumbnail(thumbnail, {64, 64}, 0, 100, false) && is_valid(thumbnail),
		             std::format("{} thumbnail decoded", name));
		if (!is_valid(thumbnail)) continue;

		const auto thumbnail_rgb = centre(thumbnail);
		assert_equal(true, at_level(thumbnail_rgb), std::format("{} thumbnail shows reference white {} ({}, {}, {})",
		                                                         name, e.shown, thumbnail_rgb[0], thumbnail_rgb[1],
		                                                         thumbnail_rgb[2]));

		ui::yuv_textures_enabled = false;
		ui::p010_textures_enabled = false;
		const auto packed = av_frame_surface(first_presented_picture(path));
		assert_equal(true, packed && packed->format() == ui::texture_format::RGB,
		             std::format("{} is prepared as BGRA for a renderer without YUV", name));

		ui::yuv_textures_enabled = true;
		ui::p010_textures_enabled = true;
		const auto planes = av_frame_surface(first_presented_picture(path));
		assert_equal(true, planes && planes->format() == ui::texture_format::P010 && planes->tone_map(),
		             std::format("{} is prepared as P010 planes with the cube that maps them", name));

		if (!packed || !planes) continue;

		const auto packed_rgb = centre(packed);
		assert_equal(true, at_level(packed_rgb), std::format("{} BGRA shows reference white {} ({}, {}, {})",
		                                                     name, e.shown, packed_rgb[0], packed_rgb[1], packed_rgb[2]));

		av_scaler scaler;
		const auto mapped = std::make_shared<ui::surface>();
		assert_equal(true, scaler.convert_yuv_surface(*planes, mapped), std::format("{} planes convert", name));

		const auto mapped_rgb = centre(mapped);
		auto spread = 0;
		for (auto c = 0; c < 3; ++c) spread = std::max(spread, std::abs(mapped_rgb[c] - packed_rgb[c]));

		assert_equal(true, spread <= 4, std::format("{} planes through the cube match the BGRA ({}, {}, {} against {}, {}, {})",
		                                            name, mapped_rgb[0], mapped_rgb[1], mapped_rgb[2],
		                                            packed_rgb[0], packed_rgb[1], packed_rgb[2]));
	}
}

// The index thumbnail takes the first picture the decoder gives. A decoder that reorders frames holds
// the only picture of a one-frame clip until it is drained, so stopping at the end of the stream
// left that clip with no thumbnail at all.
static void should_thumbnail_a_clip_whose_only_picture_comes_at_the_end()
{
	const auto path = test_files_folder.combine("excluded1").combine_file("single-frame.mp4");

	av_format_decoder decoder;
	assert_equal(true, decoder.open(path, media_intent::thumbnail), "one-picture clip opened");
	decoder.init_streams(-1, -1, false, true, false);

	ui::surface_ptr thumbnail;
	assert_equal(true, decoder.extract_thumbnail(thumbnail, {64, 64}, 0, 100, false), "thumbnail decoded");
	assert_equal(true, is_valid(thumbnail), "with pixels");
}

// A stream nothing decodes is discarded, so the demuxer never hands its packets back: a picture-only
// decoder does not read the soundtrack, and an audio walk does not read the picture.
static void should_not_read_streams_nothing_decodes()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");

	av_format_decoder video_only;
	assert_equal(true, video_only.open(path, media_intent::thumbnail), "fixture opened");
	video_only.init_streams(-1, -1, false, true, false);

	const auto info = video_only.info();
	const auto audio = std::ranges::find_if(info.streams, [](const av_stream_info& s)
	{
		return s.type == av_stream_type::audio;
	});
	assert_equal(true, audio != info.streams.end(), "fixture carries a soundtrack");

	const auto count_packets = [](const av_format_decoder& decoder, const int stream)
	{
		auto count = 0;

		for (auto i = 0; i < 200; ++i)
		{
			const auto packet = decoder.read_packet();
			if (!packet || av_packet_is_eof(packet)) break;
			if (av_packet_stream_index(packet) == stream) ++count;
		}

		return count;
	};

	av_format_decoder with_sound;
	assert_equal(true, with_sound.open(path, media_intent::playback), "fixture opened with its soundtrack");
	with_sound.init_streams(-1, -1, false, false, false);
	const auto audio_packets_decoded = count_packets(with_sound, audio->index);

	av_format_decoder picture_only;
	assert_equal(true, picture_only.open(path, media_intent::thumbnail), "fixture reopened for its picture");
	picture_only.init_streams(-1, -1, false, true, false);
	assert_equal(true, count_packets(picture_only, picture_only.video_stream_id()) > 0, "the picture is read");

	av_format_decoder picture_only_again;
	assert_equal(true, picture_only_again.open(path, media_intent::thumbnail), "and opened once more");
	picture_only_again.init_streams(-1, -1, false, true, false);

	// The probe has already read and buffered a packet or two of every stream before anything is
	// discarded, so those still come back; the rest of the soundtrack does not.
	const auto audio_packets_discarded = count_packets(picture_only_again, audio->index);
	assert_equal(true, audio_packets_discarded * 10 < audio_packets_decoded,
	             std::format("the soundtrack beside it is not ({} audio packets against {})", audio_packets_discarded,
	                         audio_packets_decoded));

	av_format_decoder both;
	assert_equal(true, both.open(path, media_intent::playback), "fixture reopened");
	both.init_streams(-1, -1, false, false, false);
	assert_equal(true, both.has_audio() && both.has_video(), "both streams decoded");
	assert_equal(false, both.extract_audio_peaks(64).empty(), "an audio walk still measures the soundtrack");

	assert_equal(true, both.seek(0.0), "rewound after the walk");

	auto picture_after_walk = false;

	for (auto i = 0; i < 400 && !picture_after_walk; ++i)
	{
		const auto packet = both.read_packet();
		if (!packet || av_packet_is_eof(packet)) break;
		picture_after_walk = av_packet_stream_index(packet) == both.video_stream_id();
	}

	assert_equal(true, picture_after_walk, "and leaves the picture readable once it is done");
}

// A drag posts a seek for every step it lands on, and the read thread makes only the latest of those still
// waiting. Each costs a demuxer seek and a decoder flush, so on slow media working through the stale ones in
// turn left the picture trailing the pointer.
static void should_make_only_the_latest_of_the_waiting_seeks()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");
	const auto player = make_test_player();
	const auto ses = make_test_session();
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, false, -1, -1, false, false, false),
	             "the video opens");
	const df::scope_exit close([ses] { ses->close(false); });

	const auto before = ses->seek_generation();
	player->seek(ses, 1.0, false);
	player->seek(ses, 2.0, false);
	player->seek(ses, 3.0, false);
	assert_equal(true, player->run_queued_commands(), "the read thread's queue runs");

	assert_equal(before + 1, ses->seek_generation(), "one seek is made");
	assert_equal(3.0, ses->pos(df::now()), "to where the pointer stopped");

	player->seek(ses, 1.0, false);
	assert_equal(true, player->run_queued_commands(), "and a seek on its own");
	assert_equal(before + 2, ses->seek_generation(), "is made as before");
}

// MEDIA-004 - a scrub drag can report the same whole-second target repeatedly after the previous
// seek has settled, and a trim handle moves its target a frame at a time. Near scrub targets merge
// into the request already made -- merging retargets the presenter rather than dropping the target --
// while the same target remains seekable after playback has resumed.
static void should_coalesce_repeated_scrub_seeks()
{
	assert_equal(true, should_coalesce_seek_request(3.0, 3.0, true, false, true),
	             "pending duplicate seeks still coalesce");
	assert_equal(false, should_coalesce_seek_request(3.0, 3.0, true, false, true, true),
	             "forced recovery seeks are not coalesced as pending duplicates");
	assert_equal(true, should_coalesce_seek_request(3.0, 3.0, false, true, true),
	             "settled scrub-to-scrub duplicate seeks coalesce");
	assert_equal(false, should_coalesce_seek_request(3.0, 3.0, false, false, false),
	             "completed playback seeks to the same historical target remain real seeks");
	assert_equal(true, should_coalesce_seek_request(3.0, 3.0 + 1.0 / 25.0, false, true, true),
	             "a frame-granular scrub step retargets the presenter instead of restarting the decoder");
	assert_equal(false, should_coalesce_seek_request(3.0, 3.2, false, true, true),
	             "different scrub targets remain real seeks");
	assert_equal(false, should_coalesce_seek_request(0.05, 0.0, true, true, true),
	             "a target at the very start is always sought");
}

// Merging a scrub seek moves only the target, so a target behind the frame the presenter already
// reached needs one real seek -- and only one, or a target before the first key frame would re-seek
// on every present.
static void should_correct_a_settled_frame_only_when_it_is_past_the_target()
{
	constexpr auto interval = 1.0 / 25.0;

	assert_equal(true, should_correct_settled_seek(true, true, 3.0, 2.92, interval, -1.0),
	             "a frame two frames past the target needs a real seek");
	assert_equal(false, should_correct_settled_seek(true, true, 3.0, 2.99, interval, -1.0),
	             "a frame within half a frame of the target is the nearest one");
	assert_equal(false, should_correct_settled_seek(true, true, 2.80, 2.92, interval, -1.0),
	             "a frame short of the target is still being walked forward");
	assert_equal(false, should_correct_settled_seek(true, true, 3.0, 2.92, interval, 2.92),
	             "a target already corrected once is not sought again");
	assert_equal(false, should_correct_settled_seek(false, true, 3.0, 2.92, interval, -1.0),
	             "playback keeps its own clock rather than restarting the decoder");
	assert_equal(false, should_correct_settled_seek(true, false, 3.0, 2.92, interval, -1.0),
	             "nothing is judged before this generation has produced a frame");
}

// A trim handle dragged with the movie stopped issues frame-sized scrub seeks, often while the
// previous one is still decoding. The last one must land on its own frame, whichever side of the
// earlier target it falls -- and a step forward must not restart the decoder to get there.
static void should_land_merged_scrub_seeks_on_their_own_frame()
{
	const auto path = test_files_folder.combine_file("gizmo.mp4");

	const auto ses = make_test_session();
	assert_equal(true, ses->open(path, files::file_type_from_name(path), 0.0, false, -1, -1, false,
	                             false, false, true), "session opened video only, as a Movie preview is");

	const auto rate = ses->info().video_frame_rate;
	assert_equal(true, rate > 0.0, "fixture declares a frame rate");
	const auto half_frame = 0.5 / rate;

	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	const platform::thread_event read_event(false, false);

	auto now = df::now();

	// Pumps until the presented frame stops moving. Stopping as soon as it came near the target would
	// catch a frame the presenter was only walking past on its way somewhere else.
	const auto settle = [&]
	{
		auto last = -1.0;
		auto stable = 0;

		for (auto i = 0; i < 400 && stable < 25; ++i)
		{
			// What reading() does every pass: a correction the presenter asked for is sought first.
			ses->process_pending_forced_seek();
			ses->process_io(video_event, audio_event);
			ses->process_video(read_event);
			now += 0.02;
			ses->update_for_present(now);

			const auto t = ses->time();
			stable = df::equiv(t, last) ? stable + 1 : 0;
			last = t;
		}

		return ses->time();
	};

	// Two frames apart, the second issued before the first has produced a picture. Dropping the second
	// as a duplicate of the first leaves the preview on 3.00.
	const auto first = 3.0;
	const auto behind = first - 2.0 / rate;
	ses->seek(first, true);
	ses->seek(behind, true);

	const auto merged_landing = settle();
	assert_equal(true, std::abs(merged_landing - behind) <= half_frame,
	             std::format("a seek merged while the first decoded lands on its own frame (wanted {:.3f}, got {:.3f})",
	                         behind, merged_landing));

	const auto ahead = behind + 2.0 / rate;
	const auto generation_before_step = ses->seek_generation();
	ses->seek(ahead, true);

	const auto forward_landing = settle();
	assert_equal(true, std::abs(forward_landing - ahead) <= half_frame,
	             std::format("a settled step forward lands on its frame (wanted {:.3f}, got {:.3f})", ahead,
	                         forward_landing));
	assert_equal(generation_before_step, ses->seek_generation(),
	             "and takes it from frames already decoded, without restarting the decoder");

	const auto back = ahead - 2.0 / rate;
	ses->seek(back, true);

	const auto backward_landing = settle();
	assert_equal(true, std::abs(backward_landing - back) <= half_frame,
	             std::format("a settled step back lands on its frame (wanted {:.3f}, got {:.3f})", back,
	                         backward_landing));
	assert_equal(generation_before_step + 1, ses->seek_generation(),
	             "with exactly one decoder restart to reach frames already passed");

	ses->close(false);
}

// MEDIA-011 - stale generation frames can be nearer to a seek target than the current queued frame,
// but they belong to the position the user already left and cannot participate in settling.
static void should_reject_stale_video_generations_while_settling()
{
	constexpr auto current_generation = 4;
	constexpr auto stale_generation = current_generation - 1;
	constexpr auto wanted = 3.0;
	constexpr auto current_displayed = 2.0;
	constexpr auto stale_front = 3.0;
	constexpr auto current_front = 3.2;

	assert_equal(true, std::abs(stale_front - wanted) < std::abs(current_front - wanted),
	             "the stale frame is the tempting nearest candidate");
	assert_equal(static_cast<int>(video_queue_front_action::discard),
	             static_cast<int>(classify_video_queue_front(stale_generation, current_generation, false)),
	             "stale normal frames are discarded before front-time comparison");
	assert_equal(static_cast<int>(video_queue_front_action::discard),
	             static_cast<int>(classify_video_queue_front(stale_generation, current_generation, true)),
	             "stale EOF markers do not settle the current seek");
	assert_equal(static_cast<int>(video_queue_front_action::present),
	             static_cast<int>(classify_video_queue_front(current_generation, current_generation, false)),
	             "current normal frames may settle the seek");
	assert_equal(static_cast<int>(video_queue_front_action::mark_eof),
	             static_cast<int>(classify_video_queue_front(current_generation, current_generation, true)),
	             "only current EOF marks the stream end");
	assert_equal(true, std::abs(current_displayed - wanted) > std::abs(current_front - wanted),
	             "after discarding stale frames the current generation advances settling");
	assert_equal(false, should_finish_video_settle(false, false, 40.0, wanted, false),
	             "stale displayed frames cannot finish a newer backward seek");
	assert_equal(true, should_finish_video_settle(true, false, current_front, wanted, false),
	             "a current-generation candidate beyond the target can finish settling");
}

// The scrubber tooltip and the hovered item thumbnail both scrub through a video by asking the
// preview decoder - a second FFmpeg instance, separate from playback - for the frame nearest a
// position. Each position must answer with its own frame; a decoder that returns the same key
// frame everywhere looks exactly like a preview that has stopped working.
static void should_preview_video_frames_at_hover_positions()
{
	const auto path = test_files_folder.combine_file("indy.mp4");

	media_preview_state preview;
	assert_equal(true, preview.open1(path), "preview decoder opened");

	const auto duration = preview.decoder1->end_time() - preview.decoder1->start_time();
	assert_equal(true, duration > 1.0, "the clip is long enough to scrub");

	double previous_time = -1.0;

	for (const auto pos : {10, 45, 80})
	{
		auto surface = std::make_shared<ui::surface>();

		assert_equal(true, preview.decoder1->extract_seek_frame(surface, {256, 256}, pos, 100),
		             std::format("seek preview decoded at {}%", pos));
		assert_equal(true, is_valid(surface), std::format("seek preview surface at {}%", pos));

		const auto expected = duration * pos / 100.0;
		assert_equal(true, std::abs(surface->time() - expected) < 2.0,
		             std::format("preview at {}% lands near {:.2f}s, not {:.2f}s", pos, expected, surface->time()));
		assert_equal(true, surface->time() > previous_time,
		             std::format("preview at {}% is later than the one before it", pos));

		previous_time = surface->time();
	}

	// The hovered-thumbnail path re-enters the same open decoder. Asking it for a position near the
	// start after it has been left near the end is what proves it seeks: every frame where it was
	// left is already past the requested time, so a decoder that walks forward without seeking
	// answers with the frame it happens to be sitting on.
	auto thumbnail = std::make_shared<ui::surface>();
	assert_equal(true, preview.decoder1->extract_thumbnail(thumbnail, {256, 256}, 1, 100),
	             "hover thumbnail decoded from the reused decoder");
	assert_equal(true, is_valid(thumbnail), "hover thumbnail surface");
	assert_equal(true, thumbnail->time() < previous_time,
	             std::format("a backward hover rewinds the reused decoder ({:.2f}s, was {:.2f}s)",
	                         thumbnail->time(), previous_time));

	preview.close();
}

// A hover queues a preview for every pixel the pointer moves, so the decoder behind them has to
// survive the sequence: reopening the container costs a probe that decodes frames of its own, far
// more than the preview itself. It is also what makes an abandoned walk safe to retry.
static void should_reuse_the_preview_decoder_across_hovers()
{
	const auto path = test_files_folder.combine_file("indy.mp4");

	media_preview_state preview;
	assert_equal(true, preview.open1(path), "preview decoder opened");

	const auto* const first = preview.decoder1.get();

	for (const auto pos : {10, 20, 30, 20, 10})
	{
		assert_equal(true, preview.open1(path), std::format("preview decoder available at {}%", pos));
		assert_equal(true, first == preview.decoder1.get(),
		             std::format("hover at {}% reused the open decoder rather than reopening", pos));

		auto surface = std::make_shared<ui::surface>();
		assert_equal(true, preview.decoder1->extract_seek_frame(surface, {256, 256}, pos, 100),
		             std::format("preview decoded at {}%", pos));
	}

	// A newer hover is already waiting, so the walk toward the exact frame stops at the nearest
	// frame it has reached instead of finishing a result nobody will see.
	std::atomic_bool superseded = true;
	preview.superseded = &superseded;

	auto abandoned = std::make_shared<ui::surface>();
	assert_equal(true,
	             preview.decoder1->extract_seek_frame(abandoned, {256, 256}, 60, 100, preview.abandon_token()),
	             "an abandoned walk still answers with the frame it reached");
	assert_equal(true, is_valid(abandoned), "abandoned preview surface");

	preview.close();
}

// A hovered thumbnail says what the video contains, not where in it the pointer sits, so it accepts
// any frame within a fraction of the duration. That slack is the whole point: the key frame the
// seek already landed on qualifies, so the walk stops there instead of decoding the rest of the GOP.
static void should_allow_tolerance_for_hover_thumbnails()
{
	const auto path = test_files_folder.combine_file("indy.mp4");

	av_format_decoder dec;
	assert_equal(true, dec.open(path, media_intent::thumbnail), "decoder opened");
	dec.init_streams(-1, -1, false, true, true);
	assert_equal(true, dec.has_video(), "indy.mp4 has video");

	const auto duration = dec.end_time() - dec.start_time();
	constexpr auto pos = 10.0;
	const auto wanted = duration * pos / 100.0;

	ui::surface_ptr key_frame;
	assert_equal(true, dec.extract_thumbnail(key_frame, {256, 256}, pos, 100, false), "key frame decoded");

	ui::surface_ptr exact;
	assert_equal(true, dec.extract_thumbnail(exact, {256, 256}, pos, 100, true, 0.0), "exact frame decoded");

	ui::surface_ptr toleranced;
	assert_equal(true, dec.extract_thumbnail(toleranced, {256, 256}, pos, 100, true, 0.02),
	             "toleranced frame decoded");

	// Unless the exact frame is a real walk past the key frame, the tolerance proves nothing.
	assert_equal(true, exact->time() > key_frame->time(), "the exact frame is a walk past the key frame");
	assert_equal(true, std::abs(key_frame->time() - wanted) <= duration * 0.02,
	             "the key frame is inside the tolerance this test asks for");

	assert_equal(true, df::equiv(toleranced->time(), key_frame->time()),
	             std::format("the walk stopped at the key frame ({:.2f}s, key {:.2f}s, exact {:.2f}s)",
	                         toleranced->time(), key_frame->time(), exact->time()));

	dec.close();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Session lifetime
///////////////////////////////////////////////////////////////////////////////////////////////////

// A reopen queued by detach_file_handles lands on the UI thread long after the teardown that queued
// it. If a second teardown or a navigation has taken the display since, publishing would reopen the
// file the caller is renaming, replacing or deleting -- the handle the detach exists to release. The
// generation is what makes that visible to the callback, which then closes rather than publishes.
static void should_reject_superseded_av_session()
{
	null_async_strategy as;
	common_display_state_t common;
	const auto d = std::make_shared<display_state_t>(as, common);

	const auto first = ++d->_av_generation;
	const auto opened = make_test_session();

	assert_equal(true, d->publish_av_session(opened, first), "current generation publishes");
	assert_equal(true, d->_session == opened, "session installed on the display");

	// A teardown supersedes it and clears the display, exactly as detach_file_handles does.
	const auto second = ++d->_av_generation;
	d->_session.reset();

	const auto late = make_test_session();
	assert_equal(false, d->publish_av_session(late, first), "superseded generation rejected");
	assert_equal(true, d->_session == nullptr, "display left detached, caller owns closing the session");

	assert_equal(true, d->publish_av_session(late, second), "current generation publishes again");
	assert_equal(true, d->_session == late, "newest session installed");
}

static void should_key_video_uploads_by_destination_texture()
{
	av_texture_upload_state state;
	ui::texture* first = std::bit_cast<ui::texture*>(static_cast<uintptr_t>(0x1000));
	ui::texture* second = std::bit_cast<ui::texture*>(static_cast<uintptr_t>(0x2000));

	assert_equal(true, should_upload_video_frame(state, first, false, 4.0),
	             "an empty destination needs the held frame");
	state = {4.0, first};
	assert_equal(false, should_upload_video_frame(state, first, true, 4.0),
	             "the same valid destination and frame does not upload twice");
	assert_equal(true, should_upload_video_frame(state, second, false, 4.0),
	             "a replacement destination needs the same held frame");
	assert_equal(true, should_upload_video_frame(state, first, true, 4.04),
	             "a newer frame still uploads to the same destination");
}

// design.md fixes these three thresholds, and they are the difference between a helpful resume and
// a video that will not start from the beginning. Nothing else defends the numbers.
static void should_resume_only_in_the_middle_of_long_media()
{
	// A clip at or under ten seconds never resumes, however far in the saved position is.
	assert_equal(false, should_resume_at(0.0, 10.0, 5.0), "ten seconds is not long enough to resume");
	assert_equal(false, should_resume_at(0.0, 9.0, 5.0), "a short clip never resumes");
	assert_equal(true, should_resume_at(0.0, 10.5, 5.0), "just over ten seconds can resume");

	// Barely started: inside two seconds of the start is a restart, not a resume.
	assert_equal(false, should_resume_at(0.0, 60.0, 0.0), "a zero position is not a resume");
	assert_equal(false, should_resume_at(0.0, 60.0, 2.0), "exactly two seconds in is still the start");
	assert_equal(true, should_resume_at(0.0, 60.0, 2.5), "past two seconds is a resume");

	// Effectively finished: inside five seconds of the end would resume onto the credits.
	assert_equal(false, should_resume_at(0.0, 60.0, 55.0), "exactly five seconds from the end is the end");
	assert_equal(false, should_resume_at(0.0, 60.0, 59.9), "a position at the end is not a resume");
	assert_equal(true, should_resume_at(0.0, 60.0, 54.5), "before the last five seconds is a resume");

	// The window is measured from the stream's own start, not from zero, so a container whose first
	// timestamp is not zero gets the same two-second grace.
	assert_equal(false, should_resume_at(100.0, 160.0, 101.0), "the start grace follows the stream start");
	assert_equal(true, should_resume_at(100.0, 160.0, 103.0), "past the start grace on an offset stream");
	assert_equal(false, should_resume_at(100.0, 160.0, 156.0), "the end grace follows the stream end");

	// A saved position outside the media entirely cannot resume.
	assert_equal(false, should_resume_at(0.0, 60.0, -5.0), "a negative position is refused");
	assert_equal(false, should_resume_at(0.0, 60.0, 120.0), "a position past the end is refused");
}

// design.md: closing while a seek or resume is still synchronizing must save the accepted target.
// Saving the presented time instead writes zero over the position the user just resumed to.
static void should_save_the_accepted_target_while_synchronizing()
{
	assert_equal(42.0, position_to_save(false, 17.0, 42.0), "a settled session saves the presented time");

	// The frame time is still 0 because no resumed frame has arrived yet; saving it would lose the
	// position. This is the case the branch exists for.
	assert_equal(17.0, position_to_save(true, 17.0, 0.0), "a synchronizing session saves the seek target");
	assert_equal(17.0, position_to_save(true, 17.0, 42.0), "synchronizing wins over a stale presented time");
}

// The Movie preview asks for a frame every fortieth of a second, each a few frames on from the
// last. Reopening the file and seeking for every one of those is what made the preview a blank
// rectangle - the playhead runs on the wall clock and never comes back to a position whose decode
// has finally landed. So the decoder walks forward from where it already sits, and seeks only when
// the request is behind it.
static void should_walk_video_frames_forward()
{
	const auto path = test_files_folder.combine_file("indy.mp4");

	av_format_decoder dec;
	assert_equal(true, dec.open(path, media_intent::thumbnail), "decoder opened");
	dec.init_streams(-1, -1, false, true, false);
	assert_equal(true, dec.has_video(), "indy.mp4 has video");

	const auto start = dec.start_time();
	assert_equal(true, dec.end_time() - start > 1.0, "the clip is long enough to walk");

	auto previous = -1.0;

	for (auto step = 0; step < 8; ++step)
	{
		const auto wanted = start + 0.2 + step * 0.08;

		ui::surface_ptr s;
		assert_equal(true, dec.extract_frame_at(s, {256, 256}, wanted, 0.02),
		             std::format("frame decoded at {:.2f}s", wanted));
		assert_equal(true, is_valid(s), "frame surface");
		assert_equal(true, std::abs(s->time() - wanted) < 0.5,
		             std::format("step lands near {:.2f}s (got {:.2f}s)", wanted, s->time()));
		assert_equal(true, s->time() >= previous, "a forward walk never goes backwards");
		if (step > 0)
		{
			assert_equal(true, s->time() > previous + 0.01,
			             std::format("a forward walk advances timestamps (previous {:.2f}, got {:.2f})",
			                         previous, s->time()));
		}

		previous = s->time();
	}

	// Backwards is the case a walk cannot answer. Without a seek the decoder is already past the
	// request, so every frame it reaches is late and it hands back the first one it sees.
	const auto back_wanted = start + 0.2;

	ui::surface_ptr back;
	assert_equal(true, dec.extract_frame_at(back, {256, 256}, back_wanted, 0.02), "frame decoded behind the walk");
	assert_equal(true, std::abs(back->time() - back_wanted) < 0.5,
	             std::format("a request behind the decoder seeks (wanted {:.2f}s, got {:.2f}s)",
	                         back_wanted, back->time()));
	assert_equal(true, back->time() < previous,
	             std::format("a backward request returns before the last forward frame ({:.2f}s, last {:.2f}s)",
	                         back->time(), previous));

	dec.close();
}

// MEDIA-005 - the nearest frame may be the frame already decoded just before or just after the
// target. A sequential extractor must retain that bounded bracket rather than walking past it.
static void should_reuse_the_nearest_frame_from_the_sequential_bracket()
{
	const auto path = test_files_folder.combine_file("indy.mp4");

	av_format_decoder dec;
	assert_equal(true, dec.open(path, media_intent::thumbnail), "decoder opened");
	dec.init_streams(-1, -1, false, true, false);
	assert_equal(true, dec.has_video(), "indy.mp4 has video");

	const auto start = dec.start_time();
	const auto target = start + 0.35;

	ui::surface_ptr first;
	assert_equal(true, dec.extract_frame_at(first, {256, 256}, target, 0.0), "first frame decoded");
	assert_equal(true, is_valid(first), "first frame surface");

	ui::surface_ptr repeated;
	assert_equal(true, dec.extract_frame_at(repeated, {256, 256}, first->time(), 0.0),
	             "exact repeated timestamp decoded");
	assert_equal(true, is_valid(repeated), "repeated frame surface");
	assert_equal(true, df::equiv(repeated->time(), first->time()),
	             std::format("repeating {:.3f}s reuses that frame instead of walking to {:.3f}s",
	                         first->time(), repeated->time()));

	ui::surface_ptr next;
	const auto near_next = first->time() + 0.02;
	assert_equal(true, dec.extract_frame_at(next, {256, 256}, near_next, 0.0),
	             "nearby forward frame decoded");
	assert_equal(true, is_valid(next), "nearby frame surface");
	assert_equal(true, next->time() >= first->time(),
	             std::format("nearby request stays on the retained bracket (first {:.3f}s, got {:.3f}s)",
	                         first->time(), next->time()));
	assert_equal(true, next->time() - first->time() < 0.2,
	             std::format("nearby request does not skip a retained neighbor (first {:.3f}s, got {:.3f}s)",
	                         first->time(), next->time()));

	dec.close();
}

// MEDIA-005 - when walking forward from a retained bracket, the newly decoded frame is not always
// nearest. For source frames 0, .04, .08, .12, a 60fps export request at .09 must reuse .08 rather
// than jump to .12.
static void should_choose_the_retained_frame_when_it_is_nearest_to_a_forward_request()
{
	assert_equal(true, should_use_kept_sequential_frame(0.08, 0.12, 0.09),
	             "the retained previous frame is nearest to .09");
	assert_equal(false, should_use_kept_sequential_frame(0.08, 0.08, 0.08),
	             "the just decoded frame is not substituted with itself");
	assert_equal(false, should_use_kept_sequential_frame(0.04, 0.08, 0.065),
	             "the newly decoded next frame is nearest to .065");
	assert_equal(true, should_use_kept_sequential_frame(0.04, 0.08, 0.06),
	             "ties keep the earlier retained frame");
}

// MEDIA-006 - Movie export keeps one frame-threaded decoder per source and asks it for sequential
// frames up to the source end. Once a request reaches EOF and drains delayed decoder output, the
// next forward request must seek rather than trying to walk from an already-drained decoder.
static void should_extract_threaded_video_frames_sequentially_to_eof()
{
	const auto path = test_files_folder.combine_file("StPauls.MOV");

	av_format_decoder dec;
	assert_equal(true, dec.open(path, media_intent::thumbnail), "decoder opened");
	dec.init_streams(-1, -1, false, true, true);
	assert_equal(true, dec.has_video(), "StPauls.MOV has video");

	const auto fps = std::clamp(dec.video_frame_rate(), 1.0, 120.0);
	const auto step = 1.0 / fps;
	const auto end = dec.end_time();
	const auto start = std::max(dec.start_time(), end - step * 8.0);
	assert_equal(true, end > start + step * 3.0, "fixture has enough tail frames");

	auto decoded = 0;
	auto previous_time = -1.0;

	for (auto target = start; target < end; target += step)
	{
		ui::surface_ptr frame;
		assert_equal(true, dec.extract_frame_at(frame, {256, 256}, target, step / 2.0),
		             std::format("threaded frame decoded at {:.3f}s", target));
		assert_equal(true, is_valid(frame), "threaded frame surface");
		assert_equal(true, frame->time() + step >= target,
		             std::format("frame at {:.3f}s reaches target {:.3f}s", frame->time(), target));
		assert_equal(true, frame->time() + 0.001 >= previous_time,
		             std::format("threaded extraction remains ordered ({:.3f}s after {:.3f}s)",
		                         frame->time(), previous_time));
		previous_time = frame->time();
		++decoded;
	}

	assert_equal(true, decoded >= 4, "decoded several tail frames");
	dec.close();
}

// A container can run past its last picture -- sound a frame or two longer than video is common in
// camera files -- and Movie gives an untrimmed clip the container's end. The render walks a decoder
// frame by frame up to that end. A decoder that handed out the last frame without reading to the end
// of the stream walked off it on the next request and answered nothing, failing the render of any
// project holding such a clip. Past the last frame the last frame is the nearest there is, which is
// what a seek to the same time already answered.
static void should_answer_with_the_last_frame_past_the_end_of_the_picture()
{
	const auto path = test_files_folder.combine("excluded1").combine_file("h264-10bit.mp4");

	for (const auto threads : {false, true})
	{
		av_format_decoder dec;
		assert_equal(true, dec.open(path, media_intent::thumbnail), "fixture opened");
		dec.init_streams(-1, -1, false, true, threads);

		const auto step = 1.0 / std::clamp(dec.video_frame_rate(), 1.0, 120.0);
		const auto end = dec.end_time();
		auto last = -1.0;

		for (auto target = dec.start_time(); target <= end; target += step)
		{
			ui::surface_ptr frame;
			assert_equal(true, dec.extract_frame_at(frame, {256, 256}, target, step / 2.0),
			             std::format("a frame is answered at {:.3f}s of {:.3f}s, threads {}", target, end, threads));
			if (!is_valid(frame)) break;
			last = frame->time();
		}

		ui::surface_ptr past;
		assert_equal(true, dec.extract_frame_at(past, {256, 256}, end, step / 2.0),
		             std::format("the container's end is answered, threads {}", threads));
		assert_equal(true, is_valid(past) && df::equiv(last, past->time()), "with the last frame there is");
	}
}

// The Movie trim control draws where the sound is, because a trim is usually aimed at the start or
// the end of someone talking and that is invisible in a picture. Peaks are per bucket over the
// whole stream, so a stream with content must produce buckets that differ from one another - a
// measurement that answers the same number everywhere draws a flat line and says nothing.
static void should_measure_audio_peaks()
{
	df::file_path voiced_path;

	for (const auto* const name : {"indy.mp4", "gizmo.mp4", "tagged.mkv", "tagged.webm", "anamorphic.mp4"})
	{
		const auto candidate = test_files_folder.combine_file(name);

		av_format_decoder probe;
		if (!probe.open(candidate, media_intent::metadata)) continue;
		probe.init_streams(-1, -1, false, false, false);

		if (probe.has_audio())
		{
			voiced_path = candidate;
			break;
		}
	}

	assert_equal(false, voiced_path.is_empty(), "a test file with audio is available");

	av_format_decoder dec;
	assert_equal(true, dec.open(voiced_path, media_intent::playback), "decoder opened");
	dec.init_streams(-1, -1, false, false, false);
	assert_equal(true, dec.has_audio(), "the test file has audio");

	constexpr int buckets = 64;
	const auto peaks = dec.extract_audio_peaks(buckets);

	assert_equal(static_cast<size_t>(buckets), peaks.size(), "one level per bucket");

	const auto loudest = *std::ranges::max_element(peaks);
	const auto quietest = *std::ranges::min_element(peaks);

	assert_equal(true, loudest > 0, "the stream registers a level somewhere");
	assert_equal(true, loudest > quietest, "the level varies across the stream rather than reading flat");

	dec.close();

	// A file opened without its audio answers empty rather than a row of zeros, so the control can
	// tell silence it measured from a track that is not there.
	av_format_decoder video_only;
	assert_equal(true, video_only.open(voiced_path, media_intent::playback), "decoder reopened");
	video_only.init_streams(-1, -1, false, true, false);
	assert_equal(true, video_only.extract_audio_peaks(buckets).empty(), "no audio stream means no levels");
}

// The Movie preview plays a clip from a buffer rather than by chasing packets against a clock, so
// the whole stream is decoded once into interleaved stereo at a known rate. The cap is what stops a
// long clip spending hundreds of megabytes on a preview.
static void should_decode_audio_into_a_buffer()
{
	df::file_path voiced_path;

	for (const auto* const name : {"indy.mp4", "gizmo.mp4", "tagged.mkv", "tagged.webm", "anamorphic.mp4"})
	{
		const auto candidate = test_files_folder.combine_file(name);

		av_format_decoder probe;
		if (!probe.open(candidate, media_intent::metadata)) continue;
		probe.init_streams(-1, -1, false, false, false);

		if (probe.has_audio())
		{
			voiced_path = candidate;
			break;
		}
	}

	assert_equal(false, voiced_path.is_empty(), "a test file with audio is available");

	constexpr int rate = 48000;
	constexpr double cap_seconds = 0.5;

	av_format_decoder capped_dec;
	assert_equal(true, capped_dec.open(voiced_path, media_intent::playback), "decoder opened");
	capped_dec.init_streams(-1, -1, false, false, false);
	assert_equal(true, capped_dec.has_audio(), "the test file has audio");

	const auto capped = capped_dec.extract_audio_pcm(rate, cap_seconds);

	assert_equal(false, capped.empty(), "the stream decodes to samples");
	assert_equal(true, capped.size() % 2 == 0, "interleaved stereo is a whole number of pairs");
	assert_equal(static_cast<size_t>(cap_seconds * rate) * 2, capped.size(),
	             "and stops exactly at the cap rather than reading the whole file");

	// Silence would pass every check above, so the buffer has to be shown to carry signal. The cap
	// above is deliberately short, and the opening of a clip is often quiet, so this reads enough of
	// the stream to be sure.
	av_format_decoder full_dec;
	assert_equal(true, full_dec.open(voiced_path, media_intent::playback), "decoder reopened for a longer read");
	full_dec.init_streams(-1, -1, false, false, false);

	const auto samples = full_dec.extract_audio_pcm(rate, 60.0);
	assert_equal(false, samples.empty(), "a longer read also decodes");

	const auto loudest = std::ranges::max(samples, {}, [](const int16_t v) { return std::abs(v); });
	assert_equal(true, std::abs(loudest) > 0, "the buffer carries signal rather than silence");

	av_format_decoder range_dec;
	assert_equal(true, range_dec.open(voiced_path, media_intent::playback), "decoder reopened for a retained range");
	range_dec.init_streams(-1, -1, false, false, false);
	const auto loudest_at = static_cast<double>(
		std::distance(samples.begin(), std::ranges::max_element(samples, {},
			[](const int16_t v) { return std::abs(v); }))) / 2.0 / rate;
	const auto range_start = std::max(0.0, loudest_at - 0.25);
	const auto range = range_dec.extract_audio_pcm_range(rate, range_start, 0.5);
	assert_equal(static_cast<size_t>(0.5 * rate) * 2, range.size(),
	             "a retained range has its complete requested duration");
	const auto range_loudest = std::ranges::max(range, {}, [](const int16_t v) { return std::abs(v); });
	assert_equal(true, std::abs(range_loudest) > 0, "and carries decoded signal rather than padded silence");
	assert_equal(true, range_dec.extract_audio_pcm_range(rate, 0, 61.0).empty(),
	             "one extraction cannot allocate an unbounded clip");

	av_format_decoder video_only;
	assert_equal(true, video_only.open(voiced_path, media_intent::playback), "decoder reopened");
	video_only.init_streams(-1, -1, false, true, false);
	assert_equal(true, video_only.extract_audio_pcm(rate, cap_seconds).empty(), "no audio stream means no buffer");
}

// A damaged packet is a decode error for that packet alone, and FFmpeg goes on to decode the ones
// after it. One such error used to discard the whole waveform and the whole PCM range.
static void should_keep_audio_around_a_damaged_packet()
{
	df::file_path voiced_path;

	for (const auto* const name : {"indy.mp4", "gizmo.mp4", "tagged.mkv", "tagged.webm", "anamorphic.mp4"})
	{
		const auto candidate = test_files_folder.combine_file(name);
		av_format_decoder probe;
		if (!probe.open(candidate, media_intent::metadata)) continue;
		probe.init_streams(-1, -1, false, false, false);

		if (probe.has_audio())
		{
			voiced_path = candidate;
			break;
		}
	}

	assert_equal(false, voiced_path.is_empty(), "a test file with audio is available");

	auto received = 0;
	test_audio_receive_status = [&received](const int status)
	{
		return status == 0 && ++received == 1 ? AVERROR_INVALIDDATA : status;
	};
	const df::scope_exit restore([] { test_audio_receive_status = {}; });

	constexpr int buckets = 64;
	av_format_decoder waveform;
	assert_equal(true, waveform.open(voiced_path, media_intent::playback), "waveform decoder opened");
	waveform.init_streams(-1, -1, false, false, false);
	const auto peaks = waveform.extract_audio_peaks(buckets);
	assert_equal(true, received > 1, "frames after the damaged packet are still decoded for the waveform");
	assert_equal(static_cast<size_t>(buckets), peaks.size(), "the waveform survives a damaged packet");
	assert_equal(true, *std::ranges::max_element(peaks) > 0, "and still measures the soundtrack");

	// A frame of this stream can hold a second of audio, so the range spans several.
	received = 0;
	constexpr int rate = 48000;
	constexpr double seconds = 20.0;
	av_format_decoder clip;
	assert_equal(true, clip.open(voiced_path, media_intent::playback), "PCM decoder opened");
	clip.init_streams(-1, -1, false, false, false);
	const auto samples = clip.extract_audio_pcm_range(rate, 0, seconds);
	assert_equal(true, received > 1, std::format("frames after the damaged packet are still decoded for the clip "
	                                             "({} frames, {} samples)", received, samples.size()));
	assert_equal(static_cast<size_t>(seconds * rate) * 2, samples.size(),
	             "the clip keeps its audio around a damaged packet");
	assert_equal(true, std::ranges::any_of(samples, [](const int16_t v) { return v != 0; }),
	             "and the rest of it still carries signal");
}

// The Movie preview reads a clip's sound in windows and the render in longer ones, and each window is
// its own seek. A window must hold what the stream holds at those instants. A decoder flushed by the
// seek starts without the frame its first frame overlaps, and a window that began on that frame
// opened on an attenuated stretch: a dip at every window boundary and at the start of every clip.
static void should_read_an_audio_range_as_the_stream_holds_it()
{
	df::file_path voiced_path;

	for (const auto* const name : {"indy.mp4", "gizmo.mp4", "tagged.mkv", "tagged.webm", "anamorphic.mp4"})
	{
		const auto candidate = test_files_folder.combine_file(name);

		av_format_decoder probe;
		if (!probe.open(candidate, media_intent::metadata)) continue;
		probe.init_streams(-1, -1, false, false, false);

		if (probe.has_audio())
		{
			voiced_path = candidate;
			break;
		}
	}

	assert_equal(false, voiced_path.is_empty(), "a test file with audio is available");

	constexpr int rate = 48000;
	// The opening of a clip is often quiet, so enough is read to be sure of finding signal.
	constexpr double whole_seconds = 60.0;

	av_format_decoder whole_dec;
	assert_equal(true, whole_dec.open(voiced_path, media_intent::playback), "decoder opened");
	whole_dec.init_streams(-1, -1, false, false, false);
	const auto whole = whole_dec.extract_audio_pcm_range(rate, 0, whole_seconds);
	assert_equal(false, whole.empty(), "the stream decodes from its start");

	// The boundary goes where the stream is loud, so an attenuated stretch cannot hide in silence.
	const auto frames = whole.size() / 2;
	const auto lo = static_cast<size_t>(0.5 * rate);
	const auto hi = frames > static_cast<size_t>(0.6 * rate) ? frames - static_cast<size_t>(0.6 * rate) : lo;
	assert_equal(true, hi > lo, "the stream is long enough to put a boundary inside it");

	size_t loudest = lo;
	for (auto f = lo; f < hi; ++f)
	{
		if (std::abs(whole[f * 2]) > std::abs(whole[loudest * 2])) loudest = f;
	}

	const auto boundary_frame = loudest - static_cast<size_t>(0.01 * rate);
	const auto boundary = static_cast<double>(boundary_frame) / rate;

	av_format_decoder window_dec;
	assert_equal(true, window_dec.open(voiced_path, media_intent::playback), "decoder reopened for a window");
	window_dec.init_streams(-1, -1, false, false, false);
	const auto window = window_dec.extract_audio_pcm_range(rate, boundary, 0.5);
	assert_equal(static_cast<size_t>(0.5 * rate) * 2, window.size(), "the window has its whole duration");

	// Energy rather than samples: the two reads may place a frame a sample apart, which a sample
	// comparison would read as a difference and an energy comparison does not.
	const auto span = static_cast<size_t>(0.04 * rate) * 2;
	const auto first = boundary_frame * 2;
	double window_energy = 0;
	double whole_energy = 0;

	for (size_t i = 0; i < span; ++i)
	{
		window_energy += static_cast<double>(window[i]) * window[i];
		whole_energy += static_cast<double>(whole[first + i]) * whole[first + i];
	}

	assert_equal(true, whole_energy > 0, "the stretch compared carries signal");

	const auto ratio = std::sqrt(window_energy / whole_energy);
	assert_equal(true, ratio > 0.9 && ratio < 1.1,
	             std::format("a window opens on the level the stream has there (ratio {:.3f} at {:.3f}s)",
	                         ratio, boundary));
}

static df::file_path write_resampling_wave(const int input_rate, const bool step)
{
	const auto path = _temps.next_path(".wav");
	std::string wave;
	const auto append = [&wave](const uint32_t value, const int bytes)
	{
		for (auto i = 0; i < bytes; ++i) wave.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
	};
	const auto data_bytes = input_rate / 2 * 4;
	wave += "RIFF";
	append(36 + data_bytes, 4);
	wave += "WAVEfmt ";
	append(16, 4);
	append(1, 2);
	append(2, 2);
	append(input_rate, 4);
	append(input_rate * 4, 4);
	append(4, 2);
	append(16, 2);
	wave += "data";
	append(data_bytes, 4);
	for (auto i = 0; i < input_rate / 2; ++i)
	{
		const auto sign = step && i >= input_rate / 4 ? -1 : 1;
		append(static_cast<uint16_t>(sign * 10000), 2);
		append(static_cast<uint16_t>(sign * -12000), 2);
	}
	write_test_file(path, wave);
	return path;
}

static void should_reset_audio_resampling_after_a_seek()
{
	audio_info_t source;
	source.channel_layout = av_get_def_channel_layout(2);
	source.sample_rate = 44100;
	source.sample_fmt = prop::audio_sample_t::signed_16bit;
	auto output = source;
	output.sample_rate = 48000;
	auto* frame = av_frame_alloc();
	assert_equal(true, frame != nullptr, "audio frame allocated");
	const df::scope_exit release([&frame] { av_frame_free(&frame); });
	frame->format = AV_SAMPLE_FMT_S16;
	frame->sample_rate = source.sample_rate;
	frame->nb_samples = 441;
	assert_equal(0, av_channel_layout_copy(&frame->ch_layout, source.channel_layout.get()), "audio layout copied");
	assert_equal(0, av_frame_get_buffer(frame, 0), "audio frame samples allocated");
	auto* samples = reinterpret_cast<int16_t*>(frame->data[0]);
	std::fill_n(samples, frame->nb_samples * 2, int16_t{20000});
	audio_resampler reused(source);
	audio_buffer discarded;
	discarded.init(output);
	reused.resample(*frame, discarded, 0.0, 0);
	reused.flush();

	std::fill_n(samples, frame->nb_samples * 2, int16_t{-10000});
	audio_buffer after_seek;
	after_seek.init(output);
	reused.resample(*frame, after_seek, 1.0, 1);
	reused.drain(after_seek, 1);
	audio_resampler fresh(source);
	audio_buffer expected;
	expected.init(output);
	fresh.resample(*frame, expected, 1.0, 1);
	fresh.drain(expected, 1);
	assert_equal(true, expected.used_bytes() > 0, "fresh resampler produces audio");
	assert_equal(expected.used_bytes(), after_seek.used_bytes(), "seek reset retains the complete resampled tail");
	assert_equal(true, std::equal(expected.cdata().data, expected.cdata().data + expected.used_bytes(), after_seek.cdata().data),
	             "seek reset discards old filter history");
}

static void should_follow_midstream_pcm_sample_rate()
{
	const auto path = write_resampling_wave(48000, true);
	av_format_decoder decoder;
	assert_equal(true, decoder.open(path, media_intent::playback), "changing-rate fixture opened");
	decoder.init_streams(-1, -1, false, false, false);
	auto frames = 0;
	test_audio_pcm_frame = [&frames](AVFrame& frame)
	{
		if (++frames <= 2) return;
		assert_equal(AV_SAMPLE_FMT_S16, frame.format, "fixture decoder produces packed signed PCM");
		const auto count = frame.nb_samples;
		const auto* source = reinterpret_cast<const int16_t*>(frame.data[0]);
		auto* replacement = av_frame_alloc();
		assert_equal(true, replacement != nullptr, "replacement frame allocated");
		const df::scope_exit release([&replacement] { av_frame_free(&replacement); });
		assert_equal(0, av_frame_copy_props(replacement, &frame), "timestamps retained across rate change");
		replacement->format = AV_SAMPLE_FMT_S16P;
		replacement->sample_rate = frame.sample_rate * 2;
		replacement->nb_samples = count * 2;
		assert_equal(0, av_channel_layout_copy(&replacement->ch_layout, &frame.ch_layout), "channel layout retained");
		assert_equal(0, av_frame_get_buffer(replacement, 0), "higher-rate samples allocated");
		for (auto i = 0; i < count; ++i)
			for (auto channel = 0; channel < 2; ++channel)
			{
				auto* destination = reinterpret_cast<int16_t*>(replacement->extended_data[channel]);
				destination[i * 2] = source[i * 2 + channel];
				destination[i * 2 + 1] = source[i * 2 + channel];
			}
		av_frame_unref(&frame);
		av_frame_move_ref(&frame, replacement);
	};
	const df::scope_exit restore([] { test_audio_pcm_frame = {}; });
	const auto pcm = decoder.extract_audio_pcm_range(48000, 0, 0.5);
	assert_equal(true, frames > 2, "fixture crossed a real decoded-frame format boundary");
	assert_equal(48000_z, pcm.size(), "requested stereo duration is retained");
	for (auto frame = 14400_z; frame < 19200_z; ++frame)
	{
		assert_equal(-10000, pcm[frame * 2], "the signal transition stays at its source timestamp");
		assert_equal(12000, pcm[frame * 2 + 1], "both channels follow the new input rate");
	}
}

static void should_keep_resampled_audio_continuous()
{
	constexpr int output_rate = 48000;
	constexpr int16_t left = 10000;
	constexpr int16_t right = -12000;

	for (const auto input_rate : {44100, 96000})
	{
		const auto path = write_resampling_wave(input_rate, false);

		// Window starts align to both rates so seek rounding cannot masquerade as resampler delay.
		for (const auto [start, duration] : {std::pair{0.0, 0.25}, {0.1, 0.25}, {0.0, 0.5}})
		{
			av_format_decoder decoder;
			assert_equal(true, decoder.open(path, media_intent::playback), "the PCM fixture opens");
			decoder.init_streams(-1, -1, false, false, false);
			assert_equal(true, decoder.has_audio(), "the fixture carries audio");

			const auto pcm = decoder.extract_audio_pcm_range(output_rate, start, duration);
			assert_equal(static_cast<size_t>(duration * output_rate) * 2, pcm.size(),
			             "resampling keeps the exact requested stereo length");

			auto incorrect = size_t{0};
			for (size_t i = 0; i < pcm.size(); ++i)
			{
				if (std::abs(static_cast<int>(pcm[i]) - (i % 2 == 0 ? left : right)) > 1) ++incorrect;
			}
			assert_equal(size_t{0}, incorrect,
			             std::format("{} Hz audio has no inserted silence at {:.3f}s or its drained tail",
			                         input_rate, start));
		}
	}
}

//
// SID tunes
//

static df::file_path write_test_sid_tune(const std::vector<uint8_t>& tune)
{
	const auto path = _temps.next_path(".sid");
	assert_equal(true, df::blob_save_to_file({tune.data(), tune.size()}, path), "the tune is written");
	return path;
}

static std::vector<uint8_t> read_whole_stream(const platform::file_ptr& stream, uint64_t& largest_read)
{
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> chunk(256 * 1024);
	largest_read = 0;

	for (;;)
	{
		const auto n = stream->read(chunk.data(), chunk.size());
		if (n == 0) break;
		largest_read = std::max(largest_read, n);
		bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(n));
	}

	return bytes;
}

static int16_t wav_sample(const std::vector<uint8_t>& wav, const size_t i)
{
	return static_cast<int16_t>(wav[44 + i * 2] | (wav[45 + i * 2] << 8));
}

static int peak_of(const std::vector<uint8_t>& wav, const size_t first, const size_t last)
{
	int peak = 0;
	for (auto i = first; i < last; ++i) peak = std::max(peak, std::abs(static_cast<int>(wav_sample(wav, i))));
	return peak;
}

static void should_play_a_sid_tune_through_its_wav_stream()
{
	const auto path = write_test_sid_tune(make_test_sid_tune());
	const auto render = std::make_shared<sid_tune_render>(path, 0, 2.0);
	const auto stream = open_sid_stream(render);

	assert_equal(true, is_sid_stream(stream), "the stream is a SID stream");
	constexpr size_t samples = 2 * sid_sample_rate;
	assert_equal(uint64_t{44 + samples * 2}, stream->size(), "the WAV holds the tune's span as 16-bit mono");

	const auto details = render->details();
	assert_equal("Diffractor Test Tune", details.title, "the tune's title");
	assert_equal("Test Author", details.author, "its author");
	assert_equal(2, details.song, "the tune the file starts on plays");
	assert_equal(3, details.songs, "the file's tunes");

	uint64_t largest_read = 0;
	const auto wav = read_whole_stream(stream, largest_read);
	assert_equal(stream->size(), static_cast<uint64_t>(wav.size()), "the stream reads to its declared size");
	assert_equal(true, largest_read <= 8192, "a read hands the demuxer only a moment of audio");
	assert_equal("RIFF", std::string_view(reinterpret_cast<const char*>(wav.data()), 4), "RIFF form");
	assert_equal("WAVE", std::string_view(reinterpret_cast<const char*>(wav.data()) + 8, 4), "WAVE form");
	assert_equal("data", std::string_view(reinterpret_cast<const char*>(wav.data()) + 36, 4), "data chunk");

	const auto peak = peak_of(wav, sid_sample_rate / 2, sid_sample_rate);
	assert_equal(true, peak > 1000, "the tune sounds");
	assert_equal(true, peak_of(wav, samples - 100, samples) * 20 < peak, "the tune fades out at its end");

	// Going back is a copy of what was rendered, byte for byte, and another render of the same tune
	// renders the same sound.
	stream->seek(1001, platform::file::whence::begin);
	std::vector<uint8_t> again(4000);
	assert_equal(uint64_t{4000}, stream->read(again.data(), again.size()), "a read after seeking back");
	assert_equal(true, std::equal(again.begin(), again.end(), wav.begin() + 1001), "reads what was read before");

	uint64_t ignored = 0;
	const auto second = read_whole_stream(open_sid_stream(std::make_shared<sid_tune_render>(path, 0, 2.0)), ignored);
	assert_equal(true, second == wav, "a tune renders the same every time");

	sid_frame_state frame;
	assert_equal(true, render->frame_at(1.0, frame), "a rendered frame has the chip's state");
	assert_equal(0x0F, frame.regs[0x18] & 0x0F, "the volume the tune set");
	assert_equal(0x21, static_cast<int>(frame.regs[0x04]), "voice 1 saws with its gate on");
	assert_equal(true, frame.writes[0] >= 1, "the play routine writes the frequency every frame");
	assert_equal(true, frame.envelope[0] > 0, "voice 1 is sounding");
	assert_equal(false, render->frame_at(5.0, frame), "nothing past the end");

	int16_t scope[512];
	assert_equal(512, static_cast<int>(render->samples_before(1.0, scope, std::size(scope))),
	             "the samples behind a playback position");
	auto matching = 0_z;
	for (auto i = 0_z; i < std::size(scope); ++i)
	{
		if (scope[i] == wav_sample(wav, sid_sample_rate - std::size(scope) + i)) ++matching;
	}
	assert_equal(true, matching == std::size(scope), "are the samples that play up to it");
}

static void should_silence_a_muted_sid_voice()
{
	const auto path = write_test_sid_tune(make_test_sid_tune());
	const auto render_with = [&path](const sid_voice_mix& mix)
	{
		const auto render = std::make_shared<sid_tune_render>(path, 0, 1.0);
		render->mix(mix);
		uint64_t ignored = 0;
		const auto wav = read_whole_stream(open_sid_stream(render), ignored);
		return peak_of(wav, sid_sample_rate / 4, sid_sample_rate / 2);
	};

	const auto sounding = render_with({});

	sid_voice_mix muted;
	muted.muted[0] = true;
	sid_voice_mix solo_other;
	solo_other.solo = 1;
	sid_voice_mix solo_own;
	solo_own.solo = 0;
	solo_own.muted[0] = true;

	assert_equal(true, sounding > 1000, "the tune's one voice sounds");
	assert_equal(true, render_with(muted) * 20 < sounding, "muting the voice silences it");
	assert_equal(true, render_with(solo_other) * 20 < sounding, "a solo of another voice silences it");
	assert_equal(true, render_with(solo_own) > sounding / 2, "a voice's own solo wins over its mute");
	assert_equal(static_cast<int>(0b110), static_cast<int>(solo_own.silent_mask()), "a solo silences the rest");
}

static void should_decode_a_sid_stream_with_ffmpeg()
{
	const auto path = write_test_sid_tune(make_test_sid_tune());
	const auto render = std::make_shared<sid_tune_render>(path, 0, 1.5);
	const auto stream = open_sid_stream(render);

	av_format_decoder decoder;
	assert_equal(true, decoder.open(stream, path, media_intent::playback), "FFmpeg opens the tune's stream");
	decoder.init_streams(-1, -1, false, false, false);
	assert_equal(true, decoder.has_audio(), "as sound");
	assert_equal(false, decoder.has_video(), "with no picture");
	assert_equal(true, std::abs(decoder.end_time() - 1.5) < 0.05, "lasting the tune's span");

	// The panel describes the tune rather than the WAV it is played as.
	auto info = decoder.info();
	assert_equal(true, info.bitrate > 0, "FFmpeg gives the WAV a bit rate");
	render->describe(info);
	assert_equal("MOS 6581"sv, info.audio_codec.sv(), "the codec is the chip");
	assert_equal(0, static_cast<int>(info.bitrate), "a tune has no bit rate");
	assert_equal(true, std::ranges::any_of(info.streams, [](const av_stream_info& st)
	{
		return st.type == av_stream_type::audio && st.codec == "MOS 6581";
	}), "nor does its stream play PCM");

	const auto has_value = [&info](const metadata_standard standard, const std::string_view value)
	{
		return std::ranges::any_of(info.metadata, [&](const metadata_block& block)
		{
			return block.standard == standard && std::ranges::any_of(block.values, [&](const metadata_kv& kv)
			{
				return kv.value == value;
			});
		});
	};

	assert_equal(true, has_value(metadata_standard::media, "Diffractor Test Tune"), "the tags are the tune's");
	assert_equal(true, has_value(metadata_standard::structure, "PSID version 2"), "and so is the layout");
}

static void should_name_sid_notes_and_waveforms()
{
	// The test tune's A4: the register advances a 24-bit accumulator each cycle of the PAL clock.
	assert_equal("A4", sid_note_name(0x1D45, 985248), "A above middle C");
	assert_equal("C4", sid_note_name(0x1167, 985248), "middle C");
	assert_equal("", sid_note_name(0, 985248), "a stopped oscillator plays no note");
	assert_equal("TRI", sid_waveform_name(0x11), "triangle");
	assert_equal("SAW+PULSE", sid_waveform_name(0x61), "combined waveforms");
	assert_equal("NOISE", sid_waveform_name(0x80), "noise");
	assert_equal("OFF", sid_waveform_name(0x08), "test bit alone");
}

//
// Subtitles
//

static void should_read_ass_script_headers()
{
	const auto script = parse_ass_header(
		"[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n\n[V4+ Styles]\n"
		"Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
		"Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
		"MarginR, MarginV, Encoding\n"
		"Style: Default,Verdana,60,&H0000FFFF,&H000000FF,&H00101010,&H00000000,-1,0,0,0,100,100,0,0,1,3,0,8,20,30,40,1\n"
		"Style: Sign,Arial,40,&H80FFFFFF,&H000000FF,&H00000000,&H00000000,0,-1,0,0,100,100,0,0,1,2,0,5,10,10,10,1\n");

	assert_equal(1920, script.play_res_x, "canvas width");
	assert_equal(1080, script.play_res_y, "canvas height");
	assert_equal(2_z, script.styles.size(), "both styles");

	const auto& d = script.style("Default");
	assert_equal("Verdana", d.font, "font");
	assert_equal(true, df::equiv(60.0, d.size), "size");
	assert_equal(0xFF00FFFFu, d.primary, "yellow, where ASS's alpha 00 is opaque");
	assert_equal(0xFF101010u, d.outline_color, "outline colour");
	assert_equal(true, d.bold, "-1 is true");
	assert_equal(8, d.alignment, "top centre");
	assert_equal(20, d.margin_l, "left margin");
	assert_equal(30, d.margin_r, "right margin");
	assert_equal(40, d.margin_v, "vertical margin");

	const auto& sign = script.style("sign");
	assert_equal(0x7FFFFFFFu, sign.primary, "half transparent");
	assert_equal(true, sign.italic, "italic");
	assert_equal(5, sign.alignment, "middle centre");
	assert_equal("Verdana", script.style("Missing").font, "a missing style falls back to Default");

	const auto legacy = parse_ass_header(
		"[V4 Styles]\nFormat: Name, Fontname, Fontsize, Alignment\nStyle: Default,Arial,20,6\nStyle: Mid,Arial,20,10\n");
	assert_equal(8, legacy.style("Default").alignment, "SSA's 6 is the top centre");
	assert_equal(5, legacy.style("Mid").alignment, "SSA's 10 is the middle centre");
	assert_equal(384, legacy.play_res_x, "the default canvas");
}

static void should_style_ass_dialogue()
{
	const subtitle_style style;
	const auto text = parse_ass_dialogue("{\\b1}Bold{\\b0} plain\\N{\\i1}italic{\\i0}{\\c&H0000FF&}red", style);

	assert_equal(4_z, text.runs.size(), "four runs");
	assert_equal("Bold plain\nitalicred", text.plain_text(), "text and line break");
	assert_equal(true, text.runs[0].bold, "bold");
	assert_equal(false, text.runs[1].bold, "bold ends");
	assert_equal(true, text.runs[2].italic, "italic");
	assert_equal(0xFF0000FFu, text.runs[3].color, "red, as 0xAABBGGRR");
	assert_equal(2, text.alignment, "the style's alignment");

	const auto sign = parse_ass_dialogue("{\\an8\\pos(320,40)\\fad(200,200)}Sign{\\alpha&H80&} faded", style);
	assert_equal(8, sign.alignment, "\\an");
	assert_equal(true, sign.position.has_value() && df::equiv(sign.position->X, 320.0) &&
	             df::equiv(sign.position->Y, 40.0), "\\pos");
	assert_equal(0x7Fu, sign.runs.back().color >> 24, "\\alpha");
	assert_equal("Sign faded", sign.plain_text(), "overrides it does not draw are dropped");

	assert_equal("Text", parse_ass_dialogue("{\\p1}m 0 0 l 100 0 100 100{\\p0}Text", style).plain_text(),
	             "drawing mode draws nothing");
	assert_equal("Line", parse_ass_dialogue("Line\\N", style).plain_text(), "no empty line under the dialogue");
	assert_equal("a\xC2\xA0" "b", parse_ass_dialogue("a\\hb", style).plain_text(), "a hard space");
	assert_equal(9, parse_ass_dialogue("{\\a7}x", style).alignment, "SSA's \\a7 is the top right");

	const auto fields = split_ass_event("3,0,Sign,,0010,0020,0030,,Hello, world");
	assert_equal("Sign", fields.style, "style");
	assert_equal(10, fields.margin_l, "left margin");
	assert_equal(20, fields.margin_r, "right margin");
	assert_equal(30, fields.margin_v, "vertical margin");
	assert_equal("Hello, world", fields.text, "the text keeps its commas");
}

static void should_place_subtitles_on_the_picture()
{
	const sizei video{1920, 1080};
	const sizei canvas{384, 288};
	const sized block{400, 100};

	const auto expect = [](const rectd r, const double x, const double y, const std::string_view what)
	{
		assert_equal(true, df::equiv(r.X, x, 0.01) && df::equiv(r.Y, y, 0.01),
		             std::format("{} at {:.1f},{:.1f}", what, r.X, r.Y));
	};

	expect(place_subtitle(video, canvas, block, 2, 10, 10, 10, {}), 760, 942.5, "bottom centre, margins scaled");
	expect(place_subtitle(video, canvas, block, 7, 10, 10, 10, {}), 50, 37.5, "top left");
	expect(place_subtitle(video, canvas, block, 9, 10, 10, 10, {}), 1470, 37.5, "top right");
	expect(place_subtitle(video, canvas, block, 5, 10, 10, 10, {}), 760, 490, "middle centre");
	expect(place_subtitle(video, canvas, block, 5, 10, 10, 10, pointd(192, 144)), 760, 490,
	       "a position names the block's centre for 5");
	expect(place_subtitle(video, canvas, block, 2, 10, 10, 10, pointd(192, 288)), 760, 980,
	       "and its bottom centre for 2");
	expect(place_subtitle(video, canvas, {2000, 100}, 2, 10, 10, 10, {}), 0, 942.5,
	       "a block wider than the picture keeps its left edge");
	expect(place_subtitle(video, canvas, block, 2, 10, 10, 10, pointd(192, 400)), 760, 980,
	       "and one placed below it is kept on it");
}

static void should_lay_subtitles_out_on_the_picture()
{
	const auto surface = std::make_shared<ui::surface>();
	surface->alloc(4, 4, ui::texture_format::ARGB, ui::orientation::top_left);

	const auto line = [&surface](const uint64_t id, const rectd bounds)
	{
		subtitle_event e;
		e.id = id;
		e.bitmaps.push_back({surface, bounds});
		return e;
	};

	const auto top_of = [](const std::vector<subtitle_placement>& placed, const size_t i)
	{
		return i < placed.size() ? placed[i].bounds.top : -1;
	};

	// The picture is the video at half size, 100 across and 50 down the view.
	const sizei video{1920, 1080};
	const recti picture{100, 50, 1060, 590};
	const std::vector bottom = {line(1, {760, 940, 400, 100})};

	const auto placed = place_subtitles_on_picture(bottom, video, picture, 0, 0.0, 8);
	assert_equal(1_z, placed.size(), "one line");
	assert_equal(true, !placed.empty() && placed[0].bounds == recti(480, 520, 680, 570), "scaled onto the picture");

	const std::vector two = {line(1, {760, 940, 400, 100}), line(2, {760, 940, 400, 100})};
	assert_equal(470, top_of(place_subtitles_on_picture(two, video, picture, 0, 0.0, 8), 1),
	             "a second line at the bottom goes above the first");

	const std::vector two_top = {line(1, {760, 40, 400, 100}), line(2, {760, 40, 400, 100})};
	assert_equal(120, top_of(place_subtitles_on_picture(two_top, video, picture, 0, 0.0, 8), 1),
	             "and one at the top below it");

	assert_equal(482, top_of(place_subtitles_on_picture(bottom, video, picture, 540, 1.0, 8), 0),
	             "a line rises clear of the controls");
	assert_equal(501, top_of(place_subtitles_on_picture(bottom, video, picture, 540, 0.5, 8), 0),
	             "halfway as they fade in");
	assert_equal(520, top_of(place_subtitles_on_picture(bottom, video, picture, 600, 1.0, 8), 0),
	             "and not at all for controls below it");

	const std::vector top = {line(1, {760, 40, 400, 100})};
	assert_equal(70, top_of(place_subtitles_on_picture(top, video, picture, 540, 1.0, 8), 0),
	             "a line at the top stays put");
	assert_equal(true, place_subtitles_on_picture(bottom, video, {}, 0, 0.0, 8).empty(), "no picture, no lines");
}

static subtitle_event test_image_event(const double start, const double end, const int gen)
{
	subtitle_image image;
	image.rect = recti(10, 20, 14, 22);
	image.indices = {0, 1, 1, 0, 1, 1, 1, 1};
	// 0xAARRGGBB: clear, and opaque red.
	image.palette = {0x00000000u, 0xFFFF0000u};

	subtitle_event e;
	e.start = start;
	e.end = end;
	e.gen = gen;
	e.canvas = {100, 50};
	e.images.push_back(std::move(image));
	return e;
}

static void should_keep_subtitle_events_by_playhead()
{
	const auto surface = subtitle_image_surface(test_image_event(0, 1, 1).images.front());
	assert_equal(true, surface != nullptr && surface->width() == 4 && surface->height() == 2, "a picture of 4x2");
	if (!surface) return;
	const auto* const red = surface->pixels_line(0) + 4;
	assert_equal(true, red[0] == 0 && red[1] == 0 && red[2] == 0xFF && red[3] == 0xFF, "red as BGRA");
	assert_equal(0, static_cast<int>(surface->pixels_line(0)[3]), "the clear index is transparent");

	subtitle_events events;
	events.reset({}, {200, 100});
	events.add(test_image_event(1.0, 3.0, 1));
	events.add(test_image_event(5.0, 7.0, 1));
	events.add(test_image_event(1.0, 3.0, 1));
	assert_equal(2_z, events.size(), "a packet read twice is kept once");

	assert_equal(0_z, events.at(2.0, 1, false).size(), "nothing shows before it is rasterised");
	events.prepare(2.0);

	const auto shown = events.at(2.0, 1, false);
	assert_equal(1_z, shown.size(), "the line on show");
	assert_equal(1_z, shown.empty() ? 0_z : shown.front().bitmaps.size(), "as one picture");

	if (!shown.empty() && !shown.front().bitmaps.empty())
	{
		const auto b = shown.front().bitmaps.front().bounds;
		assert_equal(true, df::equiv(b.X, 20.0) && df::equiv(b.Y, 40.0) && df::equiv(b.Width, 8.0) &&
		             df::equiv(b.Height, 4.0), "scaled from its canvas to the video");
	}

	assert_equal(0_z, events.at(4.0, 1, false).size(), "nothing between lines");
	assert_equal(1_z, events.at(6.0, 1, false).size(), "the next line was rasterised ahead");
	assert_equal(0_z, events.at(2.0, 2, false).size(), "a stream's line is not shown after a seek");

	const auto key = events.shown_key(2.0, 1, false);
	assert_equal(true, key != 0, "a line on show has a key");
	assert_equal(subtitle_shown_key(events.at(2.0, 1, false)), key, "the key names what at() gives");
	assert_equal(true, key != events.shown_key(6.0, 1, false), "another line, another key");
	assert_equal(uint64_t{0}, events.shown_key(4.0, 1, false), "nothing on show");

	events.drop_older_than(2);
	assert_equal(0_z, events.size(), "a seek drops the stream's earlier lines");

	events.add_all({test_image_event(1.0, 3.0, 0)});
	events.drop_older_than(5);
	assert_equal(1_z, events.size(), "a sidecar's lines stay");
	events.prepare(2.0);
	assert_equal(1_z, events.at(2.0, 5, false).size(), "and show in any generation");
	events.prepare(100.0);
	assert_equal(0_z, events.at(2.0, 5, false).size(), "pictures are released once far behind");
	assert_equal(1_z, events.size(), "but a sidecar's lines are kept");

	events.reset({}, {200, 100});
	events.add(test_image_event(1.0, 3601.0, 1));
	events.end_open(4.0);
	events.prepare(5.0);
	assert_equal(0_z, events.at(5.0, 1, false).size(), "a clear ends the picture on show");
	assert_equal(1_z, events.at(3.5, 1, false).size(), "which showed until then");

	auto forced = test_image_event(10.0, 12.0, 1);
	forced.forced = true;
	events.add(std::move(forced));
	events.prepare(11.0);
	assert_equal(1_z, events.at(11.0, 1, true).size(), "a forced line shows when only forced lines do");
	events.prepare(3.5);
	assert_equal(0_z, events.at(3.5, 1, true).size(), "and another does not");

	events.add(test_image_event(100.0, 101.0, 1));
	events.prepare(200.0);
	assert_equal(0_z, events.size(), "a stream's lines long past are dropped, not only their pictures");
}

// An image stream shows one picture at a time. PGS states no end for a picture, and DVB a timeout that outlasts
// the next page, so each picture ends the one before it, as a clear does, in whatever order the two are read.
static void should_show_one_picture_subtitle_at_a_time()
{
	subtitle_events events;
	events.reset({}, {200, 100});
	events.add(test_image_event(1.0, 3601.0, 1));
	events.add(test_image_event(2.0, 3602.0, 1));
	events.prepare(2.5);
	assert_equal(1_z, events.at(2.5, 1, false).size(), "the next picture replaces the one on show");
	events.prepare(1.5);
	assert_equal(1_z, events.at(1.5, 1, false).size(), "which showed until then");

	events.add(test_image_event(5.0, 3605.0, 1));
	events.add(test_image_event(4.0, 3604.0, 1));
	events.prepare(4.5);
	const auto late = events.at(4.5, 1, false);
	assert_equal(1_z, late.size(), "a picture read after the one that replaced it");
	assert_equal(true, !late.empty() && df::equiv(late.front().end, 5.0), "ends where that one starts");

	subtitle_event clear;
	clear.start = 3.0;
	clear.end = 3.0;
	std::vector file = {
		test_image_event(2.0, 3602.0, 0), clear, test_image_event(1.0, 3601.0, 0), test_image_event(4.0, 6.0, 0)
	};
	end_replaced_pictures(file);
	assert_equal(3_z, file.size(), "a file's clears are dropped once they have ended a picture");

	if (file.size() == 3)
	{
		assert_equal(true, df::equiv(file[0].start, 1.0) && df::equiv(file[0].end, 2.0), "the next picture ends the first");
		assert_equal(true, df::equiv(file[1].end, 3.0), "the clear ends the second");
		assert_equal(true, df::equiv(file[2].end, 6.0), "and the last keeps the end it states");
	}
}

// A phone records its video turned, so the picture shown upright is the stored height wide. Lines are laid out
// for the picture as it is drawn, and laid out again when that changes, rather than squeezed into it.
static void should_lay_subtitles_out_for_the_picture_shape()
{
	subtitle_events events;
	events.reset({}, {200, 100});
	events.add(test_image_event(1.0, 3.0, 1));
	events.prepare(2.0);
	assert_equal(1_z, events.at(2.0, 1, false).size(), "laid out for the stored shape");

	events.layout({100, 200});
	assert_equal(true, events.layout() == sizei(100, 200), "the shape the picture is drawn in");
	assert_equal(0_z, events.at(2.0, 1, false).size(), "a new shape drops what was laid out for the old");

	events.prepare(2.0);
	const auto shown = events.at(2.0, 1, false);
	assert_equal(1_z, shown.size(), "and lays it out again");

	if (!shown.empty() && !shown.front().bitmaps.empty())
	{
		const auto b = shown.front().bitmaps.front().bounds;
		assert_equal(true, df::equiv(b.X, 10.0) && df::equiv(b.Y, 80.0) && df::equiv(b.Width, 4.0) &&
		             df::equiv(b.Height, 8.0), "on that shape");
	}

	events.layout({100, 200});
	assert_equal(1_z, events.at(2.0, 1, false).size(), "the same shape keeps what is laid out");
}

static void should_choose_subtitles_by_language_and_mode()
{
	const auto offer = [](const int stream, const std::string_view language, const bool forced)
	{
		subtitle_offer o;
		o.choice.stream = stream;
		o.language = language;
		o.forced = forced;
		return o;
	};

	subtitle_offer german;
	german.choice.sidecar = df::file_path(R"(C:\films\Movie.de.srt)");
	german.language = "de";

	const std::vector offers = {offer(2, "eng", false), offer(3, "eng", true), offer(4, "fre", false), german};

	assert_equal(2, choose_subtitles(offers, subtitle_mode::always, "en", "en").stream, "always: the full track");
	assert_equal(2, choose_subtitles(offers, subtitle_mode::foreign, "en", "fre").stream,
	             "foreign speech: the full track");
	assert_equal(3, choose_subtitles(offers, subtitle_mode::foreign, "en", "eng").stream,
	             "speech the viewer understands: only what is forced");
	assert_equal(3, choose_subtitles(offers, subtitle_mode::off, "en", "fre").stream, "off still shows what is forced");
	assert_equal(true, choose_subtitles(offers, subtitle_mode::always, "de", "en").sidecar == german.choice.sidecar,
	             "a sidecar in the language");
	assert_equal(true, choose_subtitles(offers, subtitle_mode::always, "es", "en").is_none(), "no Spanish to show");
	assert_equal(true, choose_subtitles(offers, subtitle_mode::foreign, "en", "").stream == 3,
	             "speech of no stated language is not taken as foreign");
	assert_equal(true, choose_subtitles({offer(2, "eng", false)}, subtitle_mode::off, "en", "fre").is_none(),
	             "off shows nothing else");
	assert_equal(5, choose_subtitles({offer(5, "", false)}, subtitle_mode::always, "en", "en").stream,
	             "a lone track of no stated language");

	assert_equal(true, same_language("en", "eng") && same_language("English", "en-GB") && same_language("pt-BR", "por"),
	             "codes and names of one language");
	assert_equal(false, same_language("en", "de"), "two languages");
	assert_equal(false, same_language("", "en"), "no language is none");

	const auto read = read_sidecar_subtitle_name("Movie.2019.en.forced.srt", "Movie.2019.mkv");
	assert_equal("en", read.language, "language after the video's name");
	assert_equal(true, read.forced, "forced");
	assert_equal("German", read_sidecar_subtitle_name("Movie German.srt", "Movie.mkv").language, "a language's name");
	assert_equal("", read_sidecar_subtitle_name("Movie.srt", "Movie.mkv").language, "no language");
	assert_equal("en", read_sidecar_subtitle_name("Movie.en.sdh.srt", "Movie.mkv").language, "the first language");
	assert_equal("", read_sidecar_subtitle_name("It.2017.srt", "It.2017.mkv").language,
	             "the video's own title names none");
	assert_equal("en", read_sidecar_subtitle_name("The.French.Dispatch.en.srt", "The.French.Dispatch.mkv").language,
	             "only the words after it count");
	assert_equal("", read_sidecar_subtitle_name("English.srt", "Movie.mkv").language, "nor does a file of another name");

	const auto video = df::file_path(R"(C:\films\Movie.mkv)");
	const auto sidecars = sidecar_subtitle_offers(video, R"(Movie.en.srt Movie.nfo Movie.idx Movie.sub "Movie.de forced.ass" Other.sub)");
	assert_equal(4_z, sidecars.size(), "every subtitle but the .sub read through its .idx");

	if (sidecars.size() == 4)
	{
		assert_equal(true, sidecars[0].choice.sidecar == df::file_path(R"(C:\films\Movie.en.srt)"), "beside the video");
		assert_equal("en", sidecars[0].language, "its language");
		assert_equal("Movie.idx", sidecars[1].title, "a VobSub by its .idx");
		assert_equal(true, sidecars[2].forced && sidecars[2].language == "de", "a quoted name");
		assert_equal("Other.sub", sidecars[3].title, "a .sub that is no VobSub");
	}

	subtitle_offer named;
	named.language = "eng";
	named.forced = true;
	assert_equal("English - forced", format_subtitle_name(named, 1), "language and forced");
	assert_equal("Subtitles 2", format_subtitle_name({}, 2), "numbered fallback");
}

static void should_read_subtitle_text_in_a_legacy_code_page()
{
	assert_equal("caf\xC3\xA9", subtitle_text_as_utf8("caf\xE9"), "Windows-1252");
	assert_equal("\xE2\x82\xAC 5", subtitle_text_as_utf8("\x80 5"), "the euro sign");
	assert_equal("caf\xC3\xA9", subtitle_text_as_utf8("caf\xC3\xA9"), "UTF-8 is left alone");
	assert_equal("plain", subtitle_text_as_utf8("plain"), "and so is ASCII");
}

static void should_decode_subtitle_sidecars()
{
	const auto folder = _temps.next_folder("subtitles");

	const auto srt = folder.combine_file("Movie.srt");
	write_test_file(srt, "1\r\n00:00:01,000 --> 00:00:02,500\r\nHello <i>world</i>\r\n\r\n"
	                "2\r\n00:00:03,000 --> 00:00:04,000\r\nSecond line\r\ncaf\xE9\r\n\r\n");

	const auto from_srt = decode_subtitle_file(srt);
	assert_equal(2_z, from_srt.events.size(), "two SRT cues");

	if (from_srt.events.size() == 2)
	{
		const auto& first = from_srt.events[0];
		assert_equal(true, df::equiv(first.start, 1.0, 0.001) && df::equiv(first.end, 2.5, 0.001), "timed");
		assert_equal(true, first.text.has_value(), "text");
		assert_equal("Hello world", first.text ? first.text->plain_text() : "", "its tags read as styles");
		assert_equal(true, first.text && first.text->runs.back().italic, "italic");
		assert_equal(0, first.gen, "a sidecar's lines belong to no seek");
		assert_equal("Second line\ncaf\xC3\xA9", from_srt.events[1].text ? from_srt.events[1].text->plain_text() : "",
		             "two lines, in Windows-1252");
	}

	const auto vtt = folder.combine_file("Movie.vtt");
	write_test_file(vtt, "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nHello from WebVTT\n\n");
	const auto from_vtt = decode_subtitle_file(vtt);
	assert_equal(1_z, from_vtt.events.size(), "a WebVTT cue");
	assert_equal("Hello from WebVTT", from_vtt.events.empty() || !from_vtt.events[0].text
		                                  ? ""
		                                  : from_vtt.events[0].text->plain_text(), "WebVTT text");

	const auto ass = folder.combine_file("Movie.ass");
	write_test_file(ass, "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n[V4+ Styles]\n"
	                "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
	                "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
	                "Alignment, MarginL, MarginR, MarginV, Encoding\n"
	                "Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
	                "Style: Top,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,8,10,10,10,1\n\n"
	                "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
	                "Dialogue: 0,0:00:01.00,0:00:03.00,Top,,0,0,0,,{\\b1}Sign{\\b0} text\n");
	const auto from_ass = decode_subtitle_file(ass);
	assert_equal(640, from_ass.script.play_res_x, "the script's canvas");
	assert_equal(1_z, from_ass.events.size(), "an ASS line");

	if (from_ass.events.size() == 1 && from_ass.events[0].text)
	{
		const auto& line = *from_ass.events[0].text;
		assert_equal(8, line.alignment, "its style's alignment");
		assert_equal("Sign text", line.plain_text(), "its text");
		assert_equal(true, line.runs.front().bold, "and overrides");
		assert_equal(true, df::equiv(from_ass.events[0].end, 3.0, 0.001), "its end");
	}

	assert_equal(0_z, decode_subtitle_file(folder.combine_file("missing.srt")).events.size(), "no file, no lines");
}

static std::string be16(const int v)
{
	return {static_cast<char>(v >> 8 & 0xFF), static_cast<char>(v & 0xFF)};
}

// One PGS segment as a .sup file holds it: "PG", its presentation time in 90 kHz ticks, a decoding time of
// none, then its type, size and data.
static std::string pgs_segment(const uint32_t pts, const char type, const std::string_view data)
{
	std::string s = "PG";
	for (auto shift = 24; shift >= 0; shift -= 8) s += static_cast<char>(pts >> shift & 0xFF);
	s.append(4, '\0');
	s += type;
	s += be16(static_cast<int>(data.size()));
	s.append(data);
	return s;
}

// A PGS display set: a 4x2 picture in one colour at `at` on a canvas, or a clear when there is no picture.
static std::string pgs_display_set(const double seconds, const int composition, const sizei canvas,
                                   const std::optional<pointi> at)
{
	const auto pts = static_cast<uint32_t>(std::lround(seconds * 90000));

	// The presentation: the canvas, then the composition - an epoch start for a picture, which releases the
	// last one's palette and object - and where its object goes.
	auto pcs = be16(canvas.cx) + be16(canvas.cy);
	pcs += '\x10';
	pcs += be16(composition);
	pcs += at ? '\x80' : '\x00';
	pcs += '\x00';
	pcs += '\x00';
	pcs += at ? '\x01' : '\x00';
	if (at) pcs += be16(0) + std::string(2, '\0') + be16(at->x) + be16(at->y);

	auto result = pgs_segment(pts, '\x16', pcs);

	if (at)
	{
		// Colour 1 is opaque white. Each line is one run of four pixels of it, then the end of the line.
		result += pgs_segment(pts, '\x14', std::string("\x00\x00\x01\xEB\x80\x80\xFF", 7));
		const auto line = std::string("\x00\x84\x01\x00\x00", 5);
		const auto rle = line + line;
		const auto length = static_cast<int>(rle.size()) + 4;
		auto ods = be16(0) + std::string("\x00\xC0", 2);
		ods += static_cast<char>(length >> 16 & 0xFF);
		ods += be16(length & 0xFFFF) + be16(4) + be16(2) + rle;
		result += pgs_segment(pts, '\x15', ods);
	}

	return result + pgs_segment(pts, '\x80', {});
}

// A Blu-ray's subtitles as a .sup file: PGS pictures with no end of their own, each on the canvas its own
// presentation states - which a decoder knows only once the picture decodes, and which can change.
static void should_read_picture_subtitles_from_a_sup_file()
{
	const auto path = _temps.next_path(".sup");
	const auto bytes = pgs_display_set(1.0, 0, {720, 480}, pointi(100, 400)) +
		pgs_display_set(2.0, 1, {1920, 1080}, pointi(900, 1000)) + pgs_display_set(3.0, 2, {1920, 1080}, {});
	assert_equal(true, df::blob_save_to_file({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, path),
	             "the file is written");

	const auto file = decode_subtitle_file(path);
	assert_equal(2_z, file.events.size(), "two pictures, and a clear that ends the second");
	if (file.events.size() != 2) return;

	const auto& first = file.events[0];
	const auto& second = file.events[1];
	assert_equal(true, df::equiv(first.start, 1.0, 0.001) && df::equiv(first.end, 2.0, 0.001),
	             "the next picture ends the first");
	assert_equal(true, df::equiv(second.start, 2.0, 0.001) && df::equiv(second.end, 3.0, 0.001),
	             "and the clear the second");
	assert_equal(true, first.canvas == sizei(720, 480), "each on the canvas its presentation states");
	assert_equal(true, second.canvas == sizei(1920, 1080), "which can change");
	assert_equal(true, first.images.size() == 1 && first.images.front().rect.left == 100 &&
	             first.images.front().rect.top == 400 && first.images.front().rect.width() == 4 &&
	             first.images.front().rect.height() == 2, "placed on it");
}

static std::string mkv_element(const std::string_view id, const std::string_view body)
{
	std::string result(id);
	// Every size in eight bytes, which a reader must accept: simpler than the shortest form. Widened first, as
	// a 32-bit size_t shifted by 32 or more is undefined.
	result += '\x01';
	const auto size = static_cast<uint64_t>(body.size());
	for (auto shift = 48; shift >= 0; shift -= 8) result += static_cast<char>(size >> shift & 0xFF);
	result.append(body);
	return result;
}

static std::string mkv_uint(const std::string_view id, const uint64_t value)
{
	std::string body;
	for (auto shift = 56; shift >= 0; shift -= 8) body += static_cast<char>(value >> shift & 0xFF);
	return mkv_element(id, body);
}

static std::string mkv_text_block(const int time_ms, const int duration_ms, const std::string_view text)
{
	std::string block;
	block += '\x81';
	block += static_cast<char>(time_ms >> 8 & 0xFF);
	block += static_cast<char>(time_ms & 0xFF);
	block += '\0';
	block.append(text);
	return mkv_element("\xA0", mkv_element("\xA1", block) + mkv_uint("\x9B", duration_ms));
}

// A Matroska file of one S_TEXT/UTF8 track, as a muxer writes it: the build has no muxer to make one.
static std::string make_subtitle_mkv()
{
	const auto header = mkv_element("\x1A\x45\xDF\xA3",
	                                mkv_uint("\x42\x86", 1) + mkv_uint("\x42\xF7", 1) + mkv_uint("\x42\xF2", 4) +
	                                mkv_uint("\x42\xF3", 8) + mkv_element("\x42\x82", "matroska") +
	                                mkv_uint("\x42\x87", 4) + mkv_uint("\x42\x85", 2));
	const auto info = mkv_element("\x15\x49\xA9\x66", mkv_uint("\x2A\xD7\xB1", 1000000) +
	                              mkv_element("\x4D\x80", "test") + mkv_element("\x57\x41", "test"));
	const auto track = mkv_uint("\xD7", 1) + mkv_uint("\x73\xC5", 1) + mkv_uint("\x83", 0x11) +
		mkv_element("\x86", "S_TEXT/UTF8") + mkv_element("\x22\xB5\x9C", "eng") + mkv_uint("\x55\xAA", 1);
	const auto tracks = mkv_element("\x16\x54\xAE\x6B", mkv_element("\xAE", track));
	const auto cluster = mkv_element("\x1F\x43\xB6\x75", mkv_uint("\xE7", 0) +
	                                 mkv_text_block(1000, 1500, "Hello <i>there</i>") +
	                                 mkv_text_block(3000, 1000, "Second line"));

	return header + mkv_element("\x18\x53\x80\x67", info + tracks + cluster);
}

static void should_decode_an_embedded_subtitle_track()
{
	const auto path = _temps.next_path(".mkv");
	const auto bytes = make_subtitle_mkv();
	assert_equal(true, df::blob_save_to_file({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, path),
	             "the file is written");

	av_format_decoder dec;
	assert_equal(true, dec.open(path, media_intent::playback), "it opens");
	dec.init_streams(-1, -1, false, false, false);

	const auto info = dec.info();
	const auto stream = std::ranges::find(info.streams, av_stream_type::subtitle, &av_stream_info::type);
	assert_equal(true, stream != info.streams.end(), "it lists the subtitle stream");
	if (stream == info.streams.end()) return;

	assert_equal(true, stream->is_forced, "forced, as its track says");
	assert_equal(true, dec.open_subtitle_stream(stream->index), "the stream opens");

	std::vector<subtitle_event> events;

	for (auto i = 0; i < 100; ++i)
	{
		const auto packet = dec.read_packet();
		if (!packet || av_packet_is_eof(packet)) break;
		for (auto& e : dec.decode_subtitle(packet, 7)) events.push_back(std::move(e));
	}

	assert_equal(2_z, events.size(), "both lines");
	if (events.size() != 2) return;

	assert_equal(true, df::equiv(events[0].start, 1.0, 0.001), "the first starts at its block");
	assert_equal(true, df::equiv(events[0].end, 2.5, 0.001), "and ends after its duration");
	assert_equal("Hello there", events[0].text ? events[0].text->plain_text() : "", "its text");
	assert_equal(true, events[0].text && events[0].text->runs.back().italic, "its tags read as styles");
	assert_equal(true, events[0].forced, "forced");
	assert_equal(7, events[0].gen, "decoded in the session's seek generation");
	assert_equal(true, df::equiv(events[1].start, 3.0, 0.001), "the second");
}

static void should_open_a_video_with_its_sidecar_subtitles()
{
	const auto folder = _temps.next_folder("sidecar-subtitles");
	const auto video = folder.combine_file("Clip.mp4");
	platform::copy_file(test_files_folder.combine_file("gizmo.mp4"), video, false, false);
	write_test_file(folder.combine_file("Clip.en.srt"), "1\n00:00:00,000 --> 00:00:30,000\nA line\n\n");

	subtitle_request request;
	request.sidecars = sidecar_subtitle_offers(video, "Clip.en.srt Clip.xmp");
	request.mode = subtitle_mode::always;
	request.language = "en";

	const auto ses = make_test_session();
	assert_equal(true, ses->open(video, files::file_type_from_name(video), 0.0, false, -1, -1, false, false, false,
	                             false, {}, request), "the video opens");

	const auto state = ses->subtitle_info();
	assert_equal(true, state != nullptr, "the session says what it offers");

	if (state)
	{
		assert_equal(true, state->choice.sidecar == folder.combine_file("Clip.en.srt"), "the English sidecar is shown");
		assert_equal(true, std::ranges::any_of(state->offers, [](const subtitle_offer& o) { return !o.choice.sidecar.is_empty(); }),
		             "and offered");
	}

	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	ses->process_io(video_event, audio_event);

	const auto now = df::now();
	assert_equal(1_z, ses->subtitles(now).size(), "the line is on show from the start");
	assert_equal(true, ses->subtitles_key(now) != 0, "with a key");

	ses->close(false);

	subtitle_request none = request;
	none.chosen = subtitle_choice{};
	const auto off = make_test_session();
	assert_equal(true, off->open(video, files::file_type_from_name(video), 0.0, false, -1, -1, false, false, false,
	                             false, {}, none), "the video opens again");
	assert_equal(true, off->subtitle_info() && off->subtitle_info()->choice.is_none(), "with subtitles turned off");
	off->close(false);
}

// The lines on show are recorded into the scene, so a change to them is a rebuild rather than a replay: a
// replay presented the lines it was recorded with, and a new line waited for whatever next repainted the
// view. The rebuild is asked for once per change, since a view that does not draw the picture never records
// what was asked for and must not be repainted on every tick for it.
static void should_rebuild_the_scene_once_for_each_subtitle_change()
{
	const auto folder = _temps.next_folder("subtitle-repaint");
	const auto video = folder.combine_file("Clip.mp4");
	platform::copy_file(test_files_folder.combine_file("gizmo.mp4"), video, false, false);
	write_test_file(folder.combine_file("Clip.en.srt"), "1\n00:00:00,000 --> 00:00:30,000\nA line\n\n");

	subtitle_request request;
	request.sidecars = sidecar_subtitle_offers(video, "Clip.en.srt");
	request.mode = subtitle_mode::always;
	request.language = "en";

	const auto ses = make_test_session();
	assert_equal(true, ses->open(video, files::file_type_from_name(video), 0.0, false, -1, -1, false, false, false,
	                             false, {}, request), "the video opens");
	const df::scope_exit close([ses] { ses->close(false); });

	null_async_strategy as;
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(as, common);
	display->update_av_session(ses);

	const platform::thread_event video_event(false, false);
	const platform::thread_event audio_event(false, false);
	ses->process_io(video_event, audio_event);

	const auto now = df::now();

	assert_equal(true, ses->subtitles_key(now) != 0, "a line is on show");
	assert_equal(true, display->update_for_present(now) == render_valid::invalid,
	             "a line nothing has drawn yet rebuilds the scene");

	assert_equal(true, display->update_for_present(now) == render_valid::valid,
	             "and asks once: a view that does not draw the picture is not repainted on every tick");
}

void register_av_tests(view_state& state, test_registry& tests)

{
	tests.add("Should reset audio resampling after a seek"s, should_reset_audio_resampling_after_a_seek);
	tests.add("Should follow midstream PCM sample format and rate"s, should_follow_midstream_pcm_sample_rate);
	//
	// Resume
	//
	tests.add("Should resume only in the middle of long media"s, should_resume_only_in_the_middle_of_long_media);
	tests.add("Should save the accepted target while synchronizing"s,
	          should_save_the_accepted_target_while_synchronizing);

	//
	// Audio
	//
	tests.add("Should format audio stream names"s, should_format_audio_stream_names);
	tests.add("Should retain audio buffer timing across cursor compaction"s,
	          should_compact_consumed_audio_only_when_needed);
	tests.add("Should ramp audio at buffer edges"s, should_ramp_audio_at_buffer_edges);
	tests.add("Should lay out an endpoint by its channel count"s, should_lay_out_an_endpoint_by_its_channel_count);
	tests.add("Should hand an endpoint the samples it has room for"s,
	          should_hand_an_endpoint_the_samples_it_has_room_for);
	tests.add("Should time visualizer independently of refresh rate"s,
	          should_time_visualizer_independently_of_refresh_rate);
	// MEDIA-007 - visualizer frequency bins are immutable once initialized.
	tests.add("Should share one immutable visualizer frequency table"s,
	          should_share_one_immutable_visualizer_frequency_table);
	tests.add("Should keep visualizer frequency bins stable across sessions"s,
	          should_keep_visualizer_frequency_bins_stable_across_sessions);

	//
	// SID tunes
	//
	tests.add("Should play a SID tune through its WAV stream"s, should_play_a_sid_tune_through_its_wav_stream);
	tests.add("Should silence a muted SID voice"s, should_silence_a_muted_sid_voice);
	tests.add("Should decode a SID stream with FFmpeg"s, should_decode_a_sid_stream_with_ffmpeg);
	tests.add("Should name SID notes and waveforms"s, should_name_sid_notes_and_waveforms);

	//
	// Subtitles
	//
	tests.add("Should read ASS script headers"s, should_read_ass_script_headers);
	tests.add("Should style ASS dialogue"s, should_style_ass_dialogue);
	tests.add("Should place subtitles on the picture"s, should_place_subtitles_on_the_picture);
	tests.add("Should lay subtitles out on the picture"s, should_lay_subtitles_out_on_the_picture);
	tests.add("Should keep subtitle events by playhead"s, should_keep_subtitle_events_by_playhead);
	tests.add("Should show one picture subtitle at a time"s, should_show_one_picture_subtitle_at_a_time);
	tests.add("Should lay subtitles out for the picture shape"s, should_lay_subtitles_out_for_the_picture_shape);
	tests.add("Should choose subtitles by language and mode"s, should_choose_subtitles_by_language_and_mode);
	tests.add("Should read subtitle text in a legacy code page"s, should_read_subtitle_text_in_a_legacy_code_page);
	tests.add("Should decode subtitle sidecars"s, should_decode_subtitle_sidecars);
	tests.add("Should read picture subtitles from a sup file"s, should_read_picture_subtitles_from_a_sup_file);
	tests.add("Should decode an embedded subtitle track"s, should_decode_an_embedded_subtitle_track);
	tests.add("Should open a video with its sidecar subtitles"s, should_open_a_video_with_its_sidecar_subtitles);
	tests.add("Should rebuild the scene once for each subtitle change"s,
	          should_rebuild_the_scene_once_for_each_subtitle_change);

	//
	// Probe
	//
	tests.add("Should extract dv datetime"s, should_extract_dv_datetime);
	tests.add("Should correct pts"s, should_correct_pts);
	// MEDIA-006 - delayed decoder output is still eligible for nearest-frame extraction.
	tests.add("Should drain delayed video frames for nearest EOF choice"s,
	          should_drain_delayed_video_frames_for_nearest_eof_choice);
	tests.add("Should scan av metadata with a bounded probe"s, should_scan_av_metadata_with_a_bounded_probe);
	// MEDIA-008 - alpha-capable still frames decoded through FFmpeg must remain alpha-capable.
	tests.add("Should preserve alpha when decoding ffmpeg still frames"s,
	          should_preserve_alpha_when_decoding_ffmpeg_stills);
	tests.add("Should decode zero alpha tga as opaque"s, should_decode_zero_alpha_tga_as_opaque);
	tests.add("Should decode opaque gif palette as rgb"s, should_decode_opaque_gif_palette_as_rgb);
	// MEDIA-009 - FFmpeg still probing must inherit the application pixel ceiling.
	tests.add("Should refuse over budget stills during probe"s, should_refuse_over_budget_stills_during_probe);
	// MEDIA-012 - MJPEG working storage is budgeted separately from source pixels.
	tests.add("Should refuse mjpeg frame when codec working storage exceeds budget"s,
	          should_refuse_mjpeg_when_codec_working_storage_exceeds_budget);
	tests.add("Should reject a non media file"s, should_reject_a_non_media_file);

	// Issue #78 - video aspect ratio
	tests.add("Should apply video aspect ratio"s, should_apply_video_aspect_ratio);
	tests.add("Should apply container aspect ratio to decoded frames"s,
	          should_apply_container_aspect_ratio_to_decoded_frames);
	tests.add("Should apply aspect ratio to broadcast video thumbnails"s,
	          should_apply_aspect_ratio_to_broadcast_video_thumbnails);

	//
	// Seeking
	//
	tests.add("Should flush decoder on thumbnail seek"s, should_flush_decoder_on_thumbnail_seek);
	// Issue #78 - the player reports the same aspect ratio the scan does
	tests.add("Should report container aspect ratio to the player"s,
	          should_report_container_aspect_ratio_to_the_player);
	// Issue #252 - the player reports the same rotation the scan does
	tests.add("Should report container rotation to the player"s, should_report_container_rotation_to_the_player);
	tests.add("Should report a mirrored container to the player"s, should_report_a_mirrored_container_to_the_player);
	tests.add("Should seek to the frame at the requested time"s, should_seek_to_the_frame_at_the_requested_time);
	tests.add("Should seek short video thumbnails"s, should_seek_short_video_thumbnails);

	//
	// Playback
	//
	tests.add("Should end a silent clip at the stream end"s, should_end_a_silent_clip_at_the_stream_end);
	tests.add("Should open a session without its audio"s, should_open_a_session_without_its_audio);
	tests.add("Should wake the decoder when the presenter takes a frame"s,
	          should_wake_the_decoder_when_the_presenter_takes_a_frame);
	tests.add("Should decode queued packets without waiting on the reader"s,
	          should_decode_queued_packets_without_waiting_on_the_reader);
	tests.add("Should present the nearest frame when the clock passes several"s,
	          should_present_the_nearest_frame_when_the_clock_passes_several);
	// MEDIA-003 - selected audio must not back-pressure video when no endpoint is available.
	tests.add("Should keep video moving when audio output is unavailable"s,
	          should_keep_video_moving_when_audio_output_is_unavailable);
	tests.add("Should not reanchor playback time on repeated audio output failures"s,
	          should_not_reanchor_playback_time_on_repeated_audio_output_failures);
	tests.add("Should queue audio recovery seek for the read thread"s,
	          should_queue_audio_recovery_seek_for_the_read_thread);
	tests.add("Should drop stale audio recovery seek"s, should_drop_stale_audio_recovery_seek);
	tests.add("Should not save a position for borrowed playback"s,
	          should_not_save_a_position_for_borrowed_playback);
	tests.add("Should bound video read ahead by bytes"s, should_bound_video_read_ahead_by_bytes);
	tests.add("Should land audio and video on the sought position"s,
	          should_land_audio_and_video_on_the_sought_position);
	// MEDIA-004 - completed seek targets remain seekable.
	tests.add("Should seek to the same position after playback has advanced"s,
	          should_seek_to_the_same_position_after_playback_has_advanced);
	// A paused seek moved the frame and bar while the cached elapsed label stayed behind.
	tests.add("Should refresh paused media time after seeking"s, should_refresh_paused_media_time_after_seeking);
	tests.add("Should repaint the scrubber only while the controls show"s,
	          should_repaint_the_scrubber_only_while_the_controls_show);
	tests.add("Should coalesce repeated scrub seeks"s, should_coalesce_repeated_scrub_seeks);
	tests.add("Should make only the latest of the waiting seeks"s, should_make_only_the_latest_of_the_waiting_seeks);
	tests.add("Should decide which streams can decode in hardware"s,
	          should_decide_which_streams_can_decode_in_hardware);
	tests.add("Should bound video decode threads"s, should_bound_video_decode_threads);
	tests.add("Should play streams the hardware path cannot decode"s,
	          should_play_streams_the_hardware_path_cannot_decode);
	tests.add("Should decode AV1 without hardware"s, should_decode_av1_without_hardware);
	tests.add("Should tone map HDR video for an SDR display"s, should_tone_map_hdr_video_for_an_sdr_display);
	tests.add("Should describe a video stream profile depth and transfer"s,
	          should_describe_a_video_stream_profile_depth_and_transfer);
	tests.add("Should prepare decoded pictures for upload"s, should_prepare_decoded_pictures_for_upload);
	tests.add("Should reuse a prepared picture once it is let go"s, should_reuse_a_prepared_picture_once_it_is_let_go);
	tests.add("Should thumbnail a clip whose only picture comes at the end"s,
	          should_thumbnail_a_clip_whose_only_picture_comes_at_the_end);
	tests.add("Should not read streams nothing decodes"s, should_not_read_streams_nothing_decodes);
	tests.add("Should correct a settled frame only when it is past the target"s,
	          should_correct_a_settled_frame_only_when_it_is_past_the_target);
	// A trim-handle drag with Movie stopped left the preview a frame or two off the handle.
	tests.add("Should land merged scrub seeks on their own frame"s, should_land_merged_scrub_seeks_on_their_own_frame);
	// MEDIA-011 - stale decoded frames cannot satisfy a newer seek generation.
	tests.add("Should reject stale video generations while settling"s,
	          should_reject_stale_video_generations_while_settling);

	//
	// Hover preview
	//
	tests.add("Should preview video frames at hover positions"s, should_preview_video_frames_at_hover_positions);
	tests.add("Should reuse the preview decoder across hovers"s, should_reuse_the_preview_decoder_across_hovers);
	tests.add("Should allow tolerance for hover thumbnails"s, should_allow_tolerance_for_hover_thumbnails);
	tests.add("Should walk video frames forward"s, should_walk_video_frames_forward);
	// MEDIA-005 - sequential extraction retains the previous/next nearest-frame bracket.
	tests.add("Should reuse the nearest frame from the sequential bracket"s,
	          should_reuse_the_nearest_frame_from_the_sequential_bracket);
	tests.add("Should choose the retained frame when it is nearest to a forward request"s,
	          should_choose_the_retained_frame_when_it_is_nearest_to_a_forward_request);
	// MEDIA-006 - frame-threaded sequential extraction can run to source EOF.
	tests.add("Should extract threaded video frames sequentially to EOF"s,
	          should_extract_threaded_video_frames_sequentially_to_eof);
	tests.add("Should answer with the last video frame past the end of the picture"s,
	          should_answer_with_the_last_frame_past_the_end_of_the_picture);
	tests.add("Should measure audio peaks"s, should_measure_audio_peaks);
	tests.add("Should decode audio into a buffer"s, should_decode_audio_into_a_buffer);
	tests.add("Should keep audio around a damaged packet"s, should_keep_audio_around_a_damaged_packet);
	tests.add("Should read an audio range as the stream holds it"s, should_read_an_audio_range_as_the_stream_holds_it);
	// Release review: input timestamps do not include the resampler's buffered delay.
	tests.add("Should keep resampled audio continuous"s, should_keep_resampled_audio_continuous);

	//
	// Session lifetime
	//
	tests.add("Should reject superseded av session"s, should_reject_superseded_av_session);
	// MEDIA-002 - held video frames must upload again to a replacement destination texture.
	tests.add("Should key video uploads by destination texture"s, should_key_video_uploads_by_destination_texture);
}
