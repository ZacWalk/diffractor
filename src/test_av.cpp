// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for the audio and video layer (av*) -- stream naming, audio buffering, resampling and
// visualization, container probing, seeking, playback timing and the hover preview decoder.

#include "pch.h"
#include "test.h"
#include "av_format.h"
#include "av_player.h"
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
		const auto inspected = ff.scan_file(path, false, ft, {}, {}, scan_intent::inspect);
		const auto indexed = ff.scan_file(path, false, ft, {}, {}, scan_intent::index);

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
	assert_equal(false, ff.scan_file(path, false, ft, {}, {}, scan_intent::inspect).success, "inspect scan fails");
	assert_equal(false, ff.scan_file(path, false, ft, {}, {}, scan_intent::index).success, "index scan fails");
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
	tests.add("Should coalesce repeated scrub seeks"s, should_coalesce_repeated_scrub_seeks);
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
