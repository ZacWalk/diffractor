// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: FFmpeg media decoder implementation. Provides video/audio decoding, frame scaling,
// audio resampling, metadata extraction, hardware acceleration support, decode-thread preparation
// of pictures for upload, and the HDR-to-SDR tone mapping cube.

#include "pch.h"
#include "av_format.h"
#include "av_player.h"
#include "metadata_xmp.h"
#include "files.h"

// Both of these are MSVC dialect repairs. excpt.h has no equivalent elsewhere, and __restrict__ is
// a real keyword on GCC and Clang: defining it away there would strip the qualifier FFmpeg's
// headers rely on rather than supply one MSVC lacks.
#ifdef _MSC_VER
#include <excpt.h>
#define __restrict__
#endif

#define __STDC_CONSTANT_MACROS
#define FF_API_PIX_FMT 0

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/display.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/pixdesc.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
}

df_assert_movable(av_stream_info);

////////////////////////////////////////////////
////////////////////////////////////////////////
////////////////////////////////////////////////
////////////////////////////////////////////////

static void av_log(void*, const int level, const char* format, va_list argList)
{
#ifdef _DEBUG
	if (level <= AV_LOG_WARNING)
	{
		if (strstr(format, "%td") == nullptr && strstr(format, "%ti") == nullptr) // Don't handle '%td'
		{
			const auto length = _vscprintf(format, argList);
			std::string result(length + 1u, 0);
			vsprintf_s(result.data(), length + 1u, format, argList);
			platform::trace(result);
		}
	}
#endif
}

void av_initialise()
{
	av_log_set_level(AV_LOG_WARNING);
	av_log_set_callback(av_log);
}

std::vector<av_codec_doc> av_supported_codecs()
{
	std::vector<av_codec_doc> result;

	const AVCodec* codec = nullptr;
	void* iter = nullptr;

	while ((codec = av_codec_iterate(&iter)) != nullptr)
	{
		if (!av_codec_is_decoder(codec))
		{
			continue;
		}

		auto media_type = av_codec_media_type::other;

		switch (codec->type)
		{
		case AVMEDIA_TYPE_VIDEO:
			media_type = av_codec_media_type::video;
			break;
		case AVMEDIA_TYPE_AUDIO:
			media_type = av_codec_media_type::audio;
			break;
		default:
			continue; // only document video and audio codecs
		}

		result.emplace_back(av_codec_doc{
			codec->name ? codec->name : "",
			codec->long_name ? codec->long_name : "",
			media_type
		});
	}

	return result;
}

static double calc_duration(int64_t t, const AVRational& base, const int64_t start)
{
	if (t == AV_NOPTS_VALUE) t = 0;
	if (start != AV_NOPTS_VALUE) t -= start;
	// Scale in double: a 64-bit timestamp times a large time-base numerator overflows.
	return static_cast<double>(t) * base.num / base.den;
}

static double calc_duration(int64_t t, const int64_t& start)
{
	if (t == AV_NOPTS_VALUE) t = 0;
	if (start != AV_NOPTS_VALUE) t -= start;
	return t / static_cast<double>(AV_TIME_BASE);
}


static int hex_char_to_int(const char byte)
{
	if (byte >= '0' && byte <= '9') return byte - '0';
	if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
	if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
	return 0;
}

static df::blob unescape_xmp(const char* sz)
{
	std::string result;

	const auto len = strlen(sz);
	result.reserve(len);

	for (auto i = 0u; i < len; i++)
	{
		auto c = sz[i];
		if (c == '\\')
		{
			if (i + 3 < len && sz[i + 1] == 'x')
			{
				c = hex_char_to_int(sz[i + 2]) << 4 |
					hex_char_to_int(sz[i + 3]);
				i += 3;
			}
		}
		result += c;
	}

	return {result.data(), result.data() + result.size()};
}

static int64_t codec_working_bytes_per_pixel(const AVCodecID codec_id)
{
	switch (codec_id)
	{
	case AV_CODEC_ID_MJPEG:
	case AV_CODEC_ID_MJPEGB:
	case AV_CODEC_ID_LJPEG:
		// Progressive 4:4:4 JPEG/MJPEG can hold full sample planes plus coefficient and
		// non-zero-count arrays before the BGRA destination exists. Ten bytes per pixel is a
		// conservative admission bound for that finite working set.
		return 10;
	default:
		return 4;
	}
}

static int64_t codec_pixel_ceiling(const AVCodecID codec_id)
{
	if (df::max_decode_bytes <= 0) return 0;
	return std::max<int64_t>(1, df::max_decode_bytes / codec_working_bytes_per_pixel(codec_id));
}

static void set_probe_decode_ceiling(AVDictionary** const opts, const AVCodecID codec_id)
{
	const auto max_pixels = codec_pixel_ceiling(codec_id);
	if (max_pixels > 0) av_dict_set_int(opts, "max_pixels", max_pixels, 0);
}

static AVDictionary** alloc_stream_probe_options(const AVFormatContext* const fc)
{
	if (!fc || fc->nb_streams == 0 || df::max_decode_bytes <= 0) return nullptr;

	auto** result = static_cast<AVDictionary**>(av_calloc(fc->nb_streams, sizeof(AVDictionary*)));
	if (!result) return nullptr;

	for (unsigned i = 0; i < fc->nb_streams; ++i)
	{
		const auto* const stream = fc->streams[i];
		if (stream && stream->codecpar) set_probe_decode_ceiling(&result[i], stream->codecpar->codec_id);
	}

	return result;
}

static void free_stream_probe_options(AVDictionary*** const opts, const unsigned count)
{
	if (!opts || !*opts) return;
	for (unsigned i = 0; i < count; ++i) av_dict_free(&(*opts)[i]);
	av_freep(opts);
}

static bool reject_over_budget_codec_working(load_diagnostic* const diagnostic, const AVCodecID codec_id,
                                             const sizei source_dimensions, const std::string_view format)
{
	if (diagnostic) diagnostic->source_dimensions = source_dimensions;

	const auto bytes = static_cast<int64_t>(source_dimensions.cx) * source_dimensions.cy *
		codec_working_bytes_per_pixel(codec_id);
	if (bytes <= df::max_decode_bytes) return false;

	if (diagnostic) diagnostic->over_budget = true;

	df::log(__FUNCTION__, std::format("{} {} x {} needs {} of codec working storage, over the {} budget",
	                                  format, source_dimensions.cx, source_dimensions.cy,
	                                  df::file_size(bytes).str(), df::file_size(df::max_decode_bytes).str()));
	return true;
}

static bool frame_palette_uses_alpha(const AVFrame& frame)
{
	if (!frame.data[0] || !frame.data[1] || frame.width <= 0 || frame.height <= 0) return false;

	std::array<bool, 256> used{};

	for (auto y = 0; y < frame.height; ++y)
	{
		const auto* const line = frame.data[0] + static_cast<ptrdiff_t>(y) * frame.linesize[0];

		for (auto x = 0; x < frame.width; ++x)
		{
			used[line[x]] = true;
		}
	}

	const auto* const palette = std::bit_cast<const uint32_t*>(frame.data[1]);

	for (auto index = 0u; index < used.size(); ++index)
	{
		if (used[index] && ((palette[index] >> 24) & 0xff) != 0xff) return true;
	}

	return false;
}

static bool frame_packed_alpha_is_used(const AVFrame& frame, const AVPixFmtDescriptor& desc)
{
	if (desc.nb_components < 4 || frame.width <= 0 || frame.height <= 0) return false;

	const auto alpha = desc.comp[desc.nb_components - 1];
	if (alpha.depth != 8 || alpha.plane < 0 || alpha.plane >= 4 || !frame.data[alpha.plane]) return true;

	auto all_zero = true;
	auto all_opaque = true;

	for (auto y = 0; y < frame.height; ++y)
	{
		const auto* const line = frame.data[alpha.plane] + static_cast<ptrdiff_t>(y) * frame.linesize[alpha.plane];

		for (auto x = 0; x < frame.width; ++x)
		{
			const auto a = line[x * alpha.step + alpha.offset];
			all_zero &= a == 0;
			all_opaque &= a == 0xff;

			if (!all_zero && !all_opaque) return true;
		}
	}

	return false;
}

static bool av_frame_uses_alpha(const AVFrame& frame)
{
	const auto fmt = static_cast<AVPixelFormat>(frame.format);
	const auto* const desc = av_pix_fmt_desc_get(fmt);
	if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_ALPHA)) return false;
	if (fmt == AV_PIX_FMT_PAL8) return frame_palette_uses_alpha(frame);
	return frame_packed_alpha_is_used(frame, *desc);
}

static std::string normalize_still_extension_hint(const std::string_view extension_hint)
{
	if (extension_hint.empty() || extension_hint.front() == '.') return std::string(extension_hint);
	return std::format(".{}", extension_hint);
}

static bool tga_declares_alpha(const df::cspan data, const std::string_view extension_hint)
{
	if (str::icmp(extension_hint, ".tga") != 0 || data.size < 18) return true;
	if (data.data[16] != 32) return true;
	return (data.data[17] & 0x0f) != 0;
}

static sizei hinted_tga_dimensions(const df::cspan data, const std::string_view extension_hint)
{
	if (str::icmp(extension_hint, ".tga") != 0 || data.size < 18) return {};
	return {
		static_cast<int>(data.data[12] | (data.data[13] << 8)),
		static_cast<int>(data.data[14] | (data.data[15] << 8))
	};
}

static sizei hinted_bmp_dimensions(const df::cspan data, const std::string_view extension_hint)
{
	if (str::icmp(extension_hint, ".bmp") != 0 || data.size < 26 || data.data[0] != 'B' || data.data[1] != 'M')
	{
		return {};
	}

	const auto width = static_cast<int32_t>(data.data[18] | (data.data[19] << 8) | (data.data[20] << 16) |
		(data.data[21] << 24));
	const auto height = static_cast<int32_t>(data.data[22] | (data.data[23] << 8) | (data.data[24] << 16) |
		(data.data[25] << 24));
	return {std::abs(width), std::abs(height)};
}

static bool reject_hinted_over_budget_still(load_diagnostic* const diagnostic, const df::cspan data,
                                            const std::string_view extension_hint)
{
	if (const auto tga_dimensions = hinted_tga_dimensions(data, extension_hint); !tga_dimensions.is_empty())
	{
		return reject_over_budget_source(diagnostic, tga_dimensions, "ffmpeg");
	}

	if (const auto bmp_dimensions = hinted_bmp_dimensions(data, extension_hint); !bmp_dimensions.is_empty())
	{
		return reject_over_budget_source(diagnostic, bmp_dimensions, "ffmpeg");
	}

	return false;
}

// Decodes a 3x3 display matrix into a clockwise rotation normalised onto [0,360).
// Shared by the container-level (stream) and frame-level side data so both map
// onto the same set of orientations.
static double rotation_from_display_matrix(const uint8_t* const data, const size_t size)
{
	if (!data || size < 9 * sizeof(int32_t))
	{
		return 0.0;
	}

	const auto theta = -av_display_rotation_get(reinterpret_cast<const int32_t*>(data));
	return theta - 360.0 * floor(theta / 360.0 + 0.9 / 360.0);
}

double get_rotation(const AVStream* const st)
{
	const AVPacketSideData* side_data = av_packet_side_data_get(st->codecpar->coded_side_data,
	                                                            st->codecpar->nb_coded_side_data,
	                                                            AV_PKT_DATA_DISPLAYMATRIX);

	return side_data ? rotation_from_display_matrix(side_data->data, side_data->size) : 0.0;
}

// Locates a DV VAUX recording-date (0x62) or recording-time (0x63) pack within a
// raw DV frame. These packs live in the three VAUX DIF blocks (block index 3, 4
// and 5) of each DIF sequence and are duplicated at two pack slots within each
// block. The offsets mirror those written by the DV muxer (libavformat/dvenc.c
// dv_inject_metadata). Returns a pointer to the 5-byte pack, or null.
static const uint8_t* dv_find_vaux_pack(const uint8_t* frame, const size_t frame_size,
                                        const uint8_t pack_id, const int off_a, const int off_b)
{
	constexpr int dif_sequence_size = 12000; // 150 DIF blocks * 80 bytes
	constexpr int vaux_blocks[] = {80 * 3, 80 * 4, 80 * 5};

	for (int seq = 0; seq < 12; ++seq)
	{
		const int seq_base = seq * dif_sequence_size;
		if (static_cast<size_t>(seq_base) >= frame_size) break;

		for (const int block : vaux_blocks)
		{
			for (const int pack : {off_a, off_b})
			{
				const int offs = seq_base + block + pack;
				if (offs >= 0 && static_cast<size_t>(offs) + 5 <= frame_size &&
					frame[offs] == pack_id)
				{
					return &frame[offs];
				}
			}
		}
	}

	return nullptr;
}

df::date_t dv_extract_rec_datetime(const uint8_t* frame, const size_t frame_size)
{
	if (!frame) return {};

	const auto* const date_pack = dv_find_vaux_pack(frame, frame_size, 0x62, 13, 58);
	if (!date_pack) return {};

	// Date pack: PC2 = day, PC3 = month, PC4 = two-digit year (all BCD).
	const int day = ((date_pack[2] >> 4) & 0x03) * 10 + (date_pack[2] & 0x0f);
	const int month = ((date_pack[3] >> 4) & 0x01) * 10 + (date_pack[3] & 0x0f);
	const int year2 = ((date_pack[4] >> 4) & 0x0f) * 10 + (date_pack[4] & 0x0f);

	if (day < 1 || day > 31 || month < 1 || month > 12 || year2 > 99) return {};

	const int year = year2 < 75 ? 2000 + year2 : 1900 + year2;

	// Time pack: PC2 = seconds, PC3 = minutes, PC4 = hours (all BCD). Optional.
	int hour = 0, minute = 0, second = 0;
	const auto* const time_pack = dv_find_vaux_pack(frame, frame_size, 0x63, 18, 63);

	if (time_pack)
	{
		second = ((time_pack[2] >> 4) & 0x07) * 10 + (time_pack[2] & 0x0f);
		minute = ((time_pack[3] >> 4) & 0x07) * 10 + (time_pack[3] & 0x0f);
		hour = ((time_pack[4] >> 4) & 0x03) * 10 + (time_pack[4] & 0x0f);

		if (hour > 23 || minute > 59 || second > 59)
		{
			hour = minute = second = 0;
		}
	}

	return {year, month, day, hour, minute, second};
}

// Reads the first full DV video frame from the demuxer and extracts its embedded
// recording date/time. Used for DVCAM / DV-in-AVI files, whose creation date is
// stored inside the DV frames rather than the container.
static df::date_t read_dv_rec_datetime(AVFormatContext* fc, const int video_stream_index)
{
	if (!fc || video_stream_index < 0 || video_stream_index >= static_cast<int>(fc->nb_streams)) return {};

	// The date is read whether or not the picture is being decoded, and a stream nothing decodes is
	// discarded, so the demuxer would otherwise never hand a DV frame back.
	auto* const stream = fc->streams[video_stream_index];
	const auto previous_discard = stream->discard;
	stream->discard = AVDISCARD_DEFAULT;

	// Rewind so we read the first recorded frame (a thumbnail extraction may have
	// left the demuxer positioned mid-stream).
	av_seek_frame(fc, -1, 0, AVSEEK_FLAG_BACKWARD);

	df::date_t result;
	auto* pkt = av_packet_alloc();

	for (int tries = 0; tries < 64 && av_read_frame(fc, pkt) >= 0; ++tries)
	{
		// A full SD DV frame is 120000 (NTSC) / 144000 (PAL) bytes; require at
		// least one DIF sequence so the VAUX pack offsets are in range.
		if (pkt->stream_index == video_stream_index && pkt->data && pkt->size >= 12000)
		{
			result = dv_extract_rec_datetime(pkt->data, static_cast<size_t>(pkt->size));
			av_packet_unref(pkt);
			if (result.is_valid()) break;
			continue;
		}

		av_packet_unref(pkt);
	}

	av_packet_free(&pkt);
	stream->discard = previous_discard;
	return result;
}

static void populate_properties(const AVFormatContext* ctx, file_scan_result& result)
{
	if (ctx)
	{
		result.nb_streams = ctx->nb_streams;
		result.duration = calc_duration(ctx->duration, AV_NOPTS_VALUE);


		const AVDictionaryEntry* tag = nullptr;

		while ((tag = av_dict_get(ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
		{
			if (str::icmp(tag->key, "xmp") == 0 || str::icmp(tag->key, "id3v2_priv.XMP") == 0)
			{
				result.metadata.xmp = unescape_xmp(tag->value);
			}
			else
			{
				result.ffmpeg_metadata.emplace_back(tag->key, str::utf8_cast(tag->value));
			}
		}
	}
}

static AVSampleFormat to_AVSampleFormat(const prop::audio_sample_t sample_fmt)
{
	switch (sample_fmt)
	{
	case prop::audio_sample_t::none: return AV_SAMPLE_FMT_NONE;
	case prop::audio_sample_t::unsigned_8bit: return AV_SAMPLE_FMT_U8;
	case prop::audio_sample_t::signed_16bit: return AV_SAMPLE_FMT_S16;
	case prop::audio_sample_t::signed_32bit: return AV_SAMPLE_FMT_S32;
	case prop::audio_sample_t::signed_64bit: return AV_SAMPLE_FMT_S64;
	case prop::audio_sample_t::signed_float: return AV_SAMPLE_FMT_FLT;
	case prop::audio_sample_t::signed_double: return AV_SAMPLE_FMT_DBL;
	case prop::audio_sample_t::unsigned_planar_8bit: return AV_SAMPLE_FMT_U8P;
	case prop::audio_sample_t::signed_planar_16bit: return AV_SAMPLE_FMT_S16P;
	case prop::audio_sample_t::signed_planar_32bit: return AV_SAMPLE_FMT_S32P;
	case prop::audio_sample_t::signed_planar_64bit: return AV_SAMPLE_FMT_S64P;
	case prop::audio_sample_t::planar_float: return AV_SAMPLE_FMT_FLTP;
	case prop::audio_sample_t::planar_double: return AV_SAMPLE_FMT_DBLP;
	default: ;
	}

	return AV_SAMPLE_FMT_NONE;
}

static prop::audio_sample_t to_sample_type(const AVSampleFormat format)
{
	switch (format)
	{
	case AV_SAMPLE_FMT_U8: return prop::audio_sample_t::unsigned_8bit;
	case AV_SAMPLE_FMT_S16: return prop::audio_sample_t::signed_16bit;
	case AV_SAMPLE_FMT_S32: return prop::audio_sample_t::signed_32bit;
	case AV_SAMPLE_FMT_FLT: return prop::audio_sample_t::signed_float;
	case AV_SAMPLE_FMT_DBL: return prop::audio_sample_t::signed_double;
	case AV_SAMPLE_FMT_U8P: return prop::audio_sample_t::unsigned_planar_8bit;
	case AV_SAMPLE_FMT_S16P: return prop::audio_sample_t::signed_planar_16bit;
	case AV_SAMPLE_FMT_S32P: return prop::audio_sample_t::signed_planar_32bit;
	case AV_SAMPLE_FMT_FLTP: return prop::audio_sample_t::planar_float;
	case AV_SAMPLE_FMT_DBLP: return prop::audio_sample_t::planar_double;
	case AV_SAMPLE_FMT_S64: return prop::audio_sample_t::signed_64bit;
	case AV_SAMPLE_FMT_S64P: return prop::audio_sample_t::signed_planar_64bit;
	default:
	case AV_SAMPLE_FMT_NONE:
		break;
	}

	return prop::audio_sample_t::none;
}

static ui::orientation calc_orientation_impl(const int rr)
{
	if (rr == 90) return ui::orientation::right_top;
	if (rr == 180) return ui::orientation::bottom_right;
	if (rr == 270) return ui::orientation::left_bottom;
	return ui::orientation::top_left;
}

// Name of the decoder that would be used for a stream, or empty when none is built in.
static str::cached decoder_name(const AVCodecParameters* const codec)
{
	const auto* const found = codec ? avcodec_find_decoder(codec->codec_id) : nullptr;
	return found ? str::cache(found->name) : str::cached{};
}

// Human-readable name of an AVPixelFormat value, or empty when unset/unknown.
static str::cached pixel_format_name(const int format)
{
	const auto* const desc = format == AV_PIX_FMT_NONE
		                         ? nullptr
		                         : av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
	return desc ? str::cache(desc->name) : str::cached{};
}

static void populate_audio_properties(const AVStream* const s, file_scan_result& result)
{
	auto* codec = s->codecpar;

	if (codec && codec->codec_type == AVMEDIA_TYPE_AUDIO)
	{
		if (const auto name = decoder_name(codec); !str::is_empty(name)) result.audio_codec = name;

		result.audio_sample_rate = codec->sample_rate;
		result.audio_channels = codec->ch_layout.nb_channels;
		result.audio_sample_type = to_sample_type(static_cast<AVSampleFormat>(codec->format));
	}
}

static void populate_video_properties(const AVStream* const s, file_scan_result& result)
{
	const auto* codec = s->codecpar;

	if (codec && codec->codec_type == AVMEDIA_TYPE_VIDEO)
	{
		if (const auto name = decoder_name(codec); !str::is_empty(name)) result.video_codec = name;

		result.width = codec->width;
		result.height = codec->height;

		// Apply the pixel (sample) aspect ratio so anamorphic / non-square-pixel
		// video reports its display dimensions rather than the stored frame size,
		// matching the aspect used during playback (#78).
		auto sar = codec->sample_aspect_ratio;

		if (sar.num == 0 || sar.den == 0 || sar.num == sar.den)
		{
			sar = s->sample_aspect_ratio;
		}

		if (sar.num > 0 && sar.den > 0 && sar.num != sar.den && codec->width > 0 && codec->height > 0)
		{
			const auto w = static_cast<int64_t>(codec->width);
			result.height = static_cast<uint32_t>(df::mul_div(
				w, static_cast<int64_t>(sar.den) * codec->height, static_cast<int64_t>(sar.num) * w));
		}

		if (const auto name = pixel_format_name(codec->format); !str::is_empty(name))
		{
			result.pixel_format = name;
		}

		result.orientation = calc_orientation_impl(df::round(get_rotation(s)));
	}
}

int try_avcodec_send_packet(AVCodecContext* avctx, const AVPacket* avpkt)
{
	// A fault here (typically in a hardware decoder / GPU driver) is deliberately NOT caught:
	// it propagates to the global unhandled-exception handler so the app writes a minidump,
	// reports the crash, and is relaunched by Application Restart. The persisted hardware-decode
	// crash guard (see init_streams / apply_gpu_crash_guard) then disables HW video decoding on
	// the next launch. Swallowing the fault here would instead leave playback silently broken.
	return avcodec_send_packet(avctx, avpkt);
}

// Number of live decoders currently using hardware video decode. The crash guard flag is
// written on the 0->1 transition and cleared on the 1->0 transition so concurrent decoders
// (playback plus any preview) keep it set for as long as any hardware decode is in progress.
static std::atomic<int> g_hw_decode_sessions{0};

////////////////////////////////////////////////
////////////////////////////////////////////////
////////////////////////////////////////////////
////////////////////////////////////////////////

class av_frame final : public ui::video_frame
{
public:
	double time = 0.0;
	int gen = 0;
	ui::orientation orientation = ui::orientation::top_left;
	bool eof = false;

	// Allocated by FFmpeg rather than held by value: the struct's size is the library's to change,
	// and only av_frame_alloc is sized to the library actually linked.
	AVFrame* frm = nullptr;

	// The picture converted for upload on the decode thread, so presenting it is an upload and nothing
	// more. Once this exists frm is released, so the queue holds the picture only once.
	ui::const_surface_ptr surface;

	// For an HDR picture the renderer shares straight from the decoder: the mapping to SDR its shader
	// applies, made here on the decode thread because making it is far too slow for a paint.
	ui::tone_map_lut_ptr tone_map;

	bool operator<(const av_frame& other) const
	{
		return gen == other.gen ? time < other.time : gen < other.gen;
	}

	av_frame() : frm(av_frame_alloc())
	{
		if (!frm) throw std::bad_alloc();
	}

	av_frame(av_frame&& other) : av_frame()
	{
		av_frame_move_ref(frm, other.frm);
		copy_properties(other);
	}

	av_frame(const av_frame& other) : av_frame()
	{
		av_frame_ref(frm, other.frm);
		copy_properties(other);
	}

	av_frame& operator=(const av_frame& other)
	{
		if (this != &other)
		{
			av_frame_unref(frm);
			av_frame_ref(frm, other.frm);
			copy_properties(other);
		}
		return *this;
	}

	av_frame& operator=(av_frame&& other) noexcept
	{
		if (this != &other)
		{
			av_frame_unref(frm);
			av_frame_move_ref(frm, other.frm);
			copy_properties(other);
		}
		return *this;
	}

	~av_frame()
	{
		av_frame_free(&frm);
	}

	bool is_empty() const
	{
		if (surface) return surface->empty();
		return frm->width == 0 || frm->height == 0 || frm->data[0] == nullptr;
	}

	ui::const_surface_ptr prepared_surface() const override
	{
		return surface;
	}

	ui::video_picture picture() const override;

	void request_cpu_frames() const override
	{
		av_request_cpu_video_frames();
	}

private:
	void copy_properties(const av_frame& other)
	{
		gen = other.gen;
		time = other.time;
		orientation = other.orientation;
		eof = other.eof;
		surface = other.surface;
		tone_map = other.tone_map;
	}
};

class av_packet
{
public:
	AVPacket* pkt = nullptr;
	int seek_ver = 0;
	bool eof = false;

	av_packet() : pkt(av_packet_alloc())
	{
		if (!pkt) throw std::bad_alloc();
	}

	av_packet(av_packet&& other) : av_packet()
	{
		av_packet_move_ref(pkt, other.pkt);
		seek_ver = other.seek_ver;
		eof = other.eof;
	}

	av_packet(const av_packet& other) : av_packet()
	{
		copy(other.pkt);
		seek_ver = other.seek_ver;
		eof = other.eof;
	}

	av_packet& operator=(const av_packet& other)
	{
		if (this != &other)
		{
			av_packet copied(other);
			*this = std::move(copied);
		}
		return *this;
	}

	av_packet& operator=(av_packet&& other) noexcept
	{
		if (this != &other)
		{
			av_packet_unref(pkt);
			av_packet_move_ref(pkt, other.pkt);
			seek_ver = other.seek_ver;
			eof = other.eof;
		}
		return *this;
	}

	~av_packet() noexcept
	{
		av_packet_free(&pkt);
	}

	void copy(const AVPacket* src_avpkt) const
	{
		if (src_avpkt == pkt) return;
		av_packet_unref(pkt);
		const auto result = av_packet_ref(pkt, src_avpkt);
		if (result < 0) throw app_exception(std::format("Cannot reference audio/video packet: {}", result));
	}

	void move(AVPacket* src_avpkt) const
	{
		av_packet_move_ref(pkt, src_avpkt);
	}

	bool is_empty() const
	{
		return pkt->data == nullptr;
	};
};

size_t av_queued_payload_bytes(const av_packet_ptr& p)
{
	return p && p->pkt && p->pkt->size > 0 ? static_cast<size_t>(p->pkt->size) : 0;
}

size_t av_queued_payload_bytes(const av_frame_ptr& f)
{
	if (!f) return 0;
	if (f->surface) return f->surface->size();

	const auto& frm = *f->frm;

	// A hardware frame's own buffer is a handle, not pixels. What it costs is the pool surface it
	// keeps checked out, and that pool is allocated in full when the stream opens, so charging the
	// surface is what makes one read-ahead budget size both the queue and the pool.
	if (frm.hw_frames_ctx)
	{
		const auto* const ctx = std::bit_cast<const AVHWFramesContext*>(frm.hw_frames_ctx->data);
		const auto bytes = av_image_get_buffer_size(ctx->sw_format, ctx->width, ctx->height, 1);
		return bytes > 0 ? static_cast<size_t>(bytes) : 0;
	}

	size_t result = 0;

	for (const auto* const buf : frm.buf)
	{
		if (buf) result += buf->size;
	}

	for (auto i = 0; i < frm.nb_extended_buf; ++i)
	{
		if (frm.extended_buf[i]) result += frm.extended_buf[i]->size;
	}

	return result;
}


av_pts_correction::av_pts_correction()
{
	clear();
}

void av_pts_correction::clear()
{
	last_output = AV_NOPTS_VALUE;
	frame_interval = 0;
}

int64_t av_pts_correction::guess(const int64_t best_effort, const int64_t pts, const int64_t dts,
                                 const int64_t duration)
{
	// Step 1 - take FFmpeg's own answer. avcodec_receive_frame runs guess_correct_pts over
	// (pts, pkt_dts) and publishes the result as best_effort_timestamp, using fault counters
	// that avcodec_flush_buffers resets with the decoder. Running a second, separately reset
	// copy of that heuristic here could only diverge from the decoder's view.
	int64_t result = best_effort;

	if (result == AV_NOPTS_VALUE) result = pts;
	if (result == AV_NOPTS_VALUE) result = dts;

	// Step 2 - guarantee a usable, strictly increasing result. Some codecs and
	// containers (raw video, MJPEG sequences, damaged MPEG-TS) supply no usable
	// timestamp or one that fails to advance. Emitting a stale/duplicate value
	// makes the presenter treat the frame as "not newer" and stall, so instead we
	// extend the timeline by one frame interval - preferring the decoder-reported
	// duration and otherwise the cadence learned from earlier frames.
	if (result != AV_NOPTS_VALUE && last_output != AV_NOPTS_VALUE && result > last_output)
	{
		// Smallest positive step seen, not the most recent one: a gap in a damaged stream
		// would otherwise become the synthetic step and run the timeline away from the media.
		const auto observed = result - last_output;
		frame_interval = frame_interval > 0 ? std::min(frame_interval, observed) : observed;
	}

	const auto step = duration > 0 ? duration : (frame_interval > 0 ? frame_interval : 1);

	if (result == AV_NOPTS_VALUE)
	{
		result = last_output == AV_NOPTS_VALUE ? 0 : last_output + step;
	}
	else if (last_output != AV_NOPTS_VALUE && result <= last_output)
	{
		result = last_output + step;
	}

	last_output = result;
	return result;
}

///////////////////////////////////////////////////
////////////////////////////////////////////////
////////////////////////////////////////////////
////////////////////////////////////////////////

// Maps an AVFrame's signalled colour space + range onto the app's color_space enum.
// When the matrix is unspecified (very common) it falls back to the standard
// resolution heuristic: SD -> BT.601, HD -> BT.709, UHD -> BT.2020.
static ui::color_space av_frame_color_space(const AVFrame& frm)
{
	const bool full_range = frm.color_range == AVCOL_RANGE_JPEG;

	auto matrix = frm.colorspace;

	if (matrix == AVCOL_SPC_UNSPECIFIED)
	{
		if (frm.height >= 2000) matrix = AVCOL_SPC_BT2020_NCL;
		else if (frm.height > 576) matrix = AVCOL_SPC_BT709;
		else matrix = AVCOL_SPC_BT470BG;
	}

	switch (matrix)
	{
	case AVCOL_SPC_BT709:
		return full_range ? ui::color_space::rec709_full : ui::color_space::rec709_limited;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		return full_range ? ui::color_space::rec2020_full : ui::color_space::rec2020_limited;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
	default:
		return full_range ? ui::color_space::rec601_full : ui::color_space::rec601_limited;
	}
}

ui::video_picture av_frame::picture() const
{
	ui::video_picture result;
	result.width = frm->width;
	result.height = frm->height;
	result.orientation = orientation;
	result.color_space = av_frame_color_space(*frm);
	result.tone_map = tone_map;

	if (frm->format == AV_PIX_FMT_D3D11)
	{
		result.texture = frm->data[0];
		result.index = std::bit_cast<uintptr_t>(frm->data[1]);
		av_platform_decode_lock(std::bit_cast<const AVHWFramesContext*>(frm->hw_frames_ctx->data), result);
	}

	return result;
}

double av_time_from_frame(const av_frame_ptr& f)
{
	return f ? f->time : 0.0;
}

int av_seek_gen_from_frame(const av_frame_ptr& f)
{
	return f ? f->gen : -1;
}

bool av_frame_is_eof(const av_frame_ptr& f)
{
	return f ? f->eof : false;
}

int av_packet_stream_index(const av_packet_ptr& p)
{
	return p && p->pkt ? p->pkt->stream_index : -1;
}

bool av_packet_is_eof(const av_packet_ptr& p)
{
	return p ? p->eof : false;
}

double av_audio_frame_duration(const av_frame_ptr& f)
{
	if (!f || f->frm->sample_rate <= 0) return 0.0;
	return static_cast<double>(f->frm->nb_samples) / f->frm->sample_rate;
}

bool av_is_frame_empty(const av_frame_ptr& f)
{
	return f ? f->is_empty() : false;
}

static bool is_yuv_format(const AVPixelFormat f)
{
	return f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P;
}

video_info_t av_format_decoder::video_information() const
{
	video_info_t result;

	if (_video_context)
	{
		auto ar = _video_context->sample_aspect_ratio;

		if (ar.num == 0 || ar.den == 0 || ar.den == ar.num)
		{
			ar = {_video_stream_aspect_ratio.num, _video_stream_aspect_ratio.den};
		}

		if (ar.num != 0 && ar.den != 0)
		{
			result.aspect_ratio = {ar.num, ar.den};
		}

		if (ar.num == 0 || ar.den == 0 || ar.den == ar.num)
		{
			result.display_dimensions = {_video_context->width, _video_context->height};
		}
		else
		{
			const auto width = static_cast<int64_t>(_video_context->width);
			const auto height = df::mul_div(width, ar.den * static_cast<int64_t>(_video_context->height),
			                                ar.num * width);
			result.display_dimensions = {static_cast<int>(width), static_cast<int>(height)};
		}

		result.render_dimensions = {_video_context->width, _video_context->height};
		result.format = _video_context->pix_fmt;
		result.is_yuv = is_yuv_format(_video_context->pix_fmt);
	}

	return result;
}

double av_format_decoder::video_frame_rate() const
{
	if (!_format_context || _video_stream_index < 0) return 0;

	// The codec context often carries nothing, and a stream that declares no rate still has one that
	// can be inferred from its timestamps, which is what av_guess_frame_rate is for.
	const auto guessed = av_guess_frame_rate(_format_context, _format_context->streams[_video_stream_index],
	                                         nullptr);

	return guessed.num > 0 && guessed.den > 0 ? av_q2d(guessed) : 0.0;
}

// An empty, owning AVChannelLayout. The deleter uninitialises the layout (it can
// own a heap allocation for custom orders) before releasing it.
static channel_layout_ptr make_channel_layout()
{
	return {
		new AVChannelLayout{}, [](AVChannelLayout* layout)
		{
			av_channel_layout_uninit(layout);
			delete layout;
		}
	};
}

channel_layout_ptr av_get_def_channel_layout(const int num_channels)
{
	auto dst = make_channel_layout();
	av_channel_layout_default(dst.get(), num_channels);
	return dst;
}

// A frame holds the endpoint's own count of channels whatever its speaker mask names: WAVEFORMATEXTENSIBLE
// ignores speakers named past that count and leaves channels beyond the ones named unplaced. So the
// mask places the channels where it names as many speakers, its first ones where it names more, and
// the default layout places them where it names fewer. Audio mixed for the mask's count instead would
// fill more of the endpoint's buffer than it hands out.
channel_layout_ptr av_get_channel_layout(uint64_t mask, const int channels)
{
	while (mask && std::popcount(mask) > channels) mask &= ~std::bit_floor(mask);

	auto dst = make_channel_layout();

	if (mask == 0 || std::popcount(mask) != channels || av_channel_layout_from_mask(dst.get(), mask) < 0)
	{
		av_channel_layout_default(dst.get(), channels);
	}

	return dst;
}

audio_info_t av_audio_info(const platform::audio_output_format& format)
{
	audio_info_t result;
	result.sample_rate = format.sample_rate;

	switch (format.sample_format)
	{
	case platform::audio_sample_format::float32:
		result.sample_fmt = prop::audio_sample_t::signed_float;
		break;
	case platform::audio_sample_format::int16:
		result.sample_fmt = prop::audio_sample_t::signed_16bit;
		break;
	case platform::audio_sample_format::int32:
		result.sample_fmt = prop::audio_sample_t::signed_32bit;
		break;
	case platform::audio_sample_format::none:
		result.sample_fmt = prop::audio_sample_t::none;
		break;
	}

	result.channel_layout = av_get_channel_layout(format.channel_mask, static_cast<int>(format.channels));
	return result;
}

static channel_layout_ptr av_copy_to_ptr(const AVChannelLayout& src)
{
	auto dst = make_channel_layout();
	if (av_channel_layout_copy(dst.get(), &src) < 0) return {};
	return dst;
}

audio_info_t av_format_decoder::audio_info() const
{
	audio_info_t result;

	if (_audio_context)
	{
		result.channel_layout = av_copy_to_ptr(_audio_context->ch_layout);
		result.sample_rate = _audio_context->sample_rate;
		result.sample_fmt = to_sample_type(_audio_context->sample_fmt);
	}

	return result;
};

bool av_format_decoder::io_should_stop() const
{
	return df::is_closing || (_io_abandon && _io_abandon->is_cancelled());
}

av_format_decoder::stream_discard_scope::stream_discard_scope(AVStream* stream, const int discard) : _stream(stream)
{
	if (_stream)
	{
		_previous = _stream->discard;
		_stream->discard = static_cast<AVDiscard>(discard);
	}
}

av_format_decoder::stream_discard_scope::~stream_discard_scope()
{
	if (_stream) _stream->discard = static_cast<AVDiscard>(_previous);
}

// The audio walks read the whole stream. With the video track discarded beside them, MP4 and MOV
// skip every video byte rather than reading it only to throw it away.
av_format_decoder::stream_discard_scope av_format_decoder::discard_video_while_reading_audio() const
{
	auto* const stream = _format_context && _video_stream_index >= 0
		                     ? _format_context->streams[_video_stream_index]
		                     : nullptr;
	return {stream, AVDISCARD_ALL};
}

int av_format_decoder::read_io(void* opaque, uint8_t* buf, const int buf_size)
{
	df::assert_true(buf_size != 0);

	auto* const decoder = static_cast<av_format_decoder*>(opaque);
	if (decoder->io_should_stop()) return AVERROR_EXIT;

	const auto read = static_cast<int>(decoder->_file->read(buf, buf_size));
	if (read < 1) return AVERROR_EOF;
	return read;
}

// FFmpeg checks this between the steps of its own long loops - the stream probe above all - where a
// read callback alone would only stop it one buffer at a time.
int av_format_decoder::interrupt_io(void* opaque)
{
	return static_cast<const av_format_decoder*>(opaque)->io_should_stop() ? 1 : 0;
}

int64_t av_format_decoder::seek_io(void* opaque, const int64_t offset, const int whence)
{
	const auto* const h = static_cast<const av_format_decoder*>(opaque)->_file.get();
	int64_t result = 0;
	const auto seek_whence = whence & ~AVSEEK_FORCE;

	if (AVSEEK_SIZE == seek_whence)
	{
		result = static_cast<int64_t>(h->size());
	}
	else if (seek_whence == SEEK_SET)
	{
		result = static_cast<int64_t>(h->seek(offset, platform::file::whence::begin));
	}
	else if (seek_whence == SEEK_CUR)
	{
		result = static_cast<int64_t>(h->seek(offset, platform::file::whence::current));
	}
	else if (seek_whence == SEEK_END)
	{
		result = static_cast<int64_t>(h->seek(offset, platform::file::whence::end));
	}
	else
	{
		df::assert_true(false);
	}

	return result;
}

static int get_stream_type(const AVFormatContext* ctx, const int stream_num)
{
	if (stream_num >= 0 && stream_num < static_cast<int>(ctx->nb_streams))
	{
		const auto* const stream = ctx->streams[stream_num];

		if (stream && stream->codecpar)
		{
			return stream->codecpar->codec_type;
		}
	}

	return AVMEDIA_TYPE_UNKNOWN;
}

bool av_format_decoder::seek(const double wanted) const
{
	auto success = false;
	clear_sequential_frame_bracket();

	auto* const fc = _format_context;

	if (fc)
	{
		df::trace(std::format("av_format_decoder::seek {}", wanted));

		const auto to_ts = [](const double t) { return static_cast<int64_t>(t * AV_TIME_BASE); };

		// avformat_seek_file takes absolute container timestamps, but `wanted` is on the
		// presentation timeline, which starts at _time_origin. Files whose first PTS is not
		// zero - MPEG-TS especially - were therefore seeked short by their whole start offset.
		const auto target = to_ts(wanted) + _time_origin;
		const auto file_min = std::min(target, to_ts(_start_time) + _time_origin);
		const auto file_max = std::max(target, to_ts(_end_time) + _time_origin);

		// avformat_seek_file clears AVSEEK_FLAG_BACKWARD and derives the direction from the
		// window instead: it only searches backwards when the target sits nearer max_ts than
		// min_ts. A window centred on the target therefore always resolved to a forward seek,
		// landing on the key frame *after* the request and skipping up to a whole GOP. Asking
		// for a window that ends at the target is the only way to express "at or before".
		auto ret = avformat_seek_file(fc, -1, file_min, target, target, 0);

		if (ret < 0)
		{
			// No key frame at or before the target; take the first one that does exist rather
			// than leaving the demuxer where it was.
			ret = avformat_seek_file(fc, -1, file_min, target, file_max, 0);
		}

		_eof = ret == AVERROR_EOF;
		success = ret >= 0;

		if (_eof)
		{
			df::trace("av_format_decoder:seek end of stream");
		}

		if (success)
		{
			// Breaks MP3 seeking
			//avformat_flush(fc);
		}
	}
	return success;
}

void av_format_decoder::clear_sequential_frame_bracket() const
{
	_sequential_time = -1;
	_sequential_prev.reset();
	_sequential_next.reset();
}

bool av_format_decoder::scale_sequential_frame(ui::surface_ptr& dest_surface, const sizei max_dim,
                                               const av_frame& frame) const
{
	if (!_scaler)
	{
		_scaler = std::make_unique<av_scaler>();
	}

	return _scaler->scale_frame(*frame.frm, dest_surface, max_dim, frame.time, frame.orientation,
	                            _video_stream_aspect_ratio);
}

av_packet_ptr av_format_decoder::read_packet() const
{
	auto* const fc = _format_context;

	if (!fc || _eof)
	{
		return {};
	}

	// Read straight into the wrapper's packet: demuxing into a second AVPacket and
	// moving the reference costs an alloc/free pair on the hottest playback path.
	auto result = std::make_shared<av_packet>();
	const auto ret = av_read_frame(fc, result->pkt);

	if (ret == AVERROR_EOF)
	{
		_eof = true;
		result->eof = true;
		df::trace("av_format_decoder:read_packet end of stream");
		return result;
	}

	if (ret != 0)
	{
		return {};
	}

	return result;
}

void av_format_decoder::extract_metadata(file_scan_result& sr) const
{
	const auto* const fc = _format_context;

	if (fc)
	{
		populate_properties(fc, sr);
		const AVStream* audio_stream = nullptr;
		const AVStream* video_stream = nullptr;

		for (int i = 0; i < static_cast<int>(fc->nb_streams); ++i)
		{
			const auto* const stream = fc->streams[i];

			if (stream)
			{
				const auto is_cover_art = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;

				if (is_cover_art && !sr.cover_art)
				{
					const auto& packet = stream->attached_pic;
					const auto decoded = load_image_file({packet.data, static_cast<size_t>(packet.size)});
					if (decoded) sr.cover_art = decoded;
				}

				if (stream->codecpar)
				{
					const auto ct = stream->codecpar->codec_type;
					// An attached-picture (cover art) stream reports as AVMEDIA_TYPE_VIDEO but is a
					// still image, not a real video track. Never treat it as the video stream, or an
					// audio file with embedded cover art would report the image's dimensions/codec as
					// video properties. Cover art is extracted separately above.
					if (ct == AVMEDIA_TYPE_VIDEO && !is_cover_art && !video_stream) video_stream = stream;
					if (ct == AVMEDIA_TYPE_AUDIO && !audio_stream) audio_stream = stream;
				}
			}
		}

		if (audio_stream) populate_audio_properties(audio_stream, sr);
		if (video_stream) populate_video_properties(video_stream, sr);

		// DVCAM / DV-in-AVI files store their recording date/time inside the DV
		// frames rather than the container, so extract it from the first frame.
		if (video_stream && video_stream->codecpar &&
			video_stream->codecpar->codec_id == AV_CODEC_ID_DVVIDEO)
		{
			const auto dv_date = read_dv_rec_datetime(_format_context, video_stream->index);

			if (dv_date.is_valid())
			{
				// DV times are local wall-clock; store so created() round-trips it.
				sr.created_utc = dv_date.local_to_system();
			}
		}

		const auto bit_rate = fc->bit_rate;

		if (bit_rate > 0)
		{
			sr.bitrate = str::cache(prop::format_bit_rate(bit_rate));
		}

		if (fc->duration != AV_NOPTS_VALUE)
		{
			sr.duration = df::round(calc_duration(fc->duration, AV_NOPTS_VALUE));
		}

		// _rotation is only established by init_streams, and a decoded frame may then correct it.
		// Until then it holds its default, which means "not yet known" rather than "upright", so
		// letting it through erased the container rotation populate_video_properties had just read
		// from the display matrix. Every scan that does not decode a frame - indexing, and the
		// rescan that follows a rating, label or tag write - therefore recorded a rotated video as
		// upright, and the item's tile flipped from portrait to landscape after the first write (#252).
		if (_video_context)
		{
			sr.orientation = calc_orientation();
		}
	}
}

int64_t av_format_decoder::bitrate() const
{
	const auto* const fc = _format_context;

	if (fc)
	{
		return fc->bit_rate;
	}

	return 0;
}

void av_format_decoder::close()
{
	// The codec contexts are about to be freed, so there is nothing to gain from
	// draining them first - and draining a hardware decoder is far from free.
	avcodec_free_context(&_video_context);
	avcodec_free_context(&_audio_context);
	av_buffer_unref(&_hw_device_ctx);

	// Release this decoder's hold on the hardware-decode crash guard. Clearing on the
	// 1->0 transition marks a clean end to HW decode so a later unrelated crash is not
	// misattributed to video decoding.
	if (_hw_decode_guard_held)
	{
		_hw_decode_guard_held = false;
		if (g_hw_decode_sessions.fetch_sub(1) == 1)
		{
			platform::set_crash_guard(platform::crash_guard::hw_video_decode, false);
		}
	}

	_pts_vid.clear();
	_pts_aud.clear();
	clear_sequential_frame_bracket();

	AVFormatContext* fc = nullptr;
	std::swap(fc, _format_context);

	if (fc)
	{
		auto* pb = fc->pb;
		avformat_close_input(&fc);

		if (pb)
		{
			// A caller-supplied AVIOContext (AVFMT_FLAG_CUSTOM_IO) is left alone by
			// avformat_close_input, and avio_context_free does not release the read
			// buffer - both must go or every opened file leaks its 256K buffer.
			av_freep(&pb->buffer);
			avio_context_free(&pb);
		}
	}

	_scaler.reset();
	_file.reset();
	_path.clear();
	_eof = false;
	_has_video = false;
	_has_audio = false;
	_has_multiple_audio_streams = false;
	_video_stream_index = -1;
	_audio_stream_index = -1;
	_bitrate = 0;
	_streams.clear();
	_cover_art.reset();
	_start_time = 0;
	_end_time = 0;
	_rotation = 0;
	_video_base = {};
	_audio_base = {};
	_video_stream_aspect_ratio = {};
	_video_start_time = 0;
	_audio_start_time = 0;
	_time_origin = 0;
}


///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<audio_resampler> av_format_decoder::make_audio_resampler() const
{
	return std::make_unique<audio_resampler>(audio_info());
}

// True when the container header alone already named every playable stream and sized every picture,
// so the demuxer has a real header rather than a raw or transport stream that must be discovered by
// reading it. Only video and audio are judged: MOV and MP4 timecode (tmcd) and Apple metadata (mebx)
// tracks legitimately reach the app with no codec id at all, and camera and phone footage almost
// always carries one, so counting them as an incomplete header disqualified most real video.
static bool has_header_codec_parameters(const AVFormatContext* fc)
{
	if (!fc || fc->nb_streams == 0) return false;
	if (fc->ctx_flags & AVFMTCTX_NOHEADER) return false;

	auto described_streams = 0;

	for (unsigned i = 0; i < fc->nb_streams; ++i)
	{
		const auto* const stream = fc->streams[i];
		if (!stream) return false;

		const auto* const codec = stream->codecpar;
		if (!codec) return false;

		if (codec->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			if (codec->codec_id == AV_CODEC_ID_NONE || codec->width <= 0 || codec->height <= 0) return false;
			++described_streams;
		}
		else if (codec->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			if (codec->codec_id == AV_CODEC_ID_NONE || codec->sample_rate <= 0 ||
				codec->ch_layout.nb_channels <= 0)
			{
				return false;
			}

			++described_streams;
		}
	}

	return described_streams > 0;
}

// True when probing found at least one stream the app could actually show, play or list. FFmpeg
// falls back to matching a demuxer on the file extension alone, so any file carrying a media
// extension - a TypeScript index.ts picked up by the MPEG-TS demuxer, say - opens cleanly and then
// describes nothing. Judged after avformat_find_stream_info, when a raw or transport stream that
// had to be discovered by reading has had its chance to name its streams.
static bool has_presentable_stream(const AVFormatContext* fc)
{
	if (!fc) return false;

	for (unsigned i = 0; i < fc->nb_streams; ++i)
	{
		const auto* const stream = fc->streams[i];
		if (!stream) continue;

		const auto* const codec = stream->codecpar;
		if (!codec || codec->codec_id == AV_CODEC_ID_NONE) continue;

		switch (codec->codec_type)
		{
		case AVMEDIA_TYPE_VIDEO:
			if (codec->width > 0 && codec->height > 0) return true;
			break;
		case AVMEDIA_TYPE_AUDIO:
			if (codec->sample_rate > 0 && codec->ch_layout.nb_channels > 0) return true;
			break;
		case AVMEDIA_TYPE_SUBTITLE:
			return true;
		default:
			break;
		}
	}

	return false;
}

bool av_format_decoder::open(const df::file_path path, const media_intent intent)
{
	const auto file = open_file(path, platform::file_open_mode::read);

	if (!file)
	{
		return false;
	}

	df::trace(std::format("av_format_decoder::open {}", path.name()));

	return open(file, path, intent);
}

bool av_format_decoder::open(const platform::file_ptr& file, const df::file_path path, const media_intent intent)
{
	close();

	// The AVIOContext assumes the stream starts at zero, and a handed-over handle has already been
	// read - by the write that produced it, or by the scan that inspected it.
	file->seek(0, platform::file::whence::begin);

	// An extension shared with a text format is settled by the header before ffmpeg probes it, so a
	// TypeScript .ts is refused outright rather than part-opening as a stream with nothing in it.
	if (files::has_media_header_rule(path.extension()))
	{
		uint8_t header[files::media_header_probe_bytes];
		const auto header_read = file->read(header, sizeof(header));
		file->seek(0, platform::file::whence::begin);

		if (!files::media_header_matches(path.extension(), {header, static_cast<size_t>(header_read)}))
		{
			df::trace(std::format("av_format_decoder::open header mismatch {}", path.name()));
			return false;
		}
	}

	static constexpr int io_buffer_size = df::two_fifty_six_k;
	auto* io_buffer = static_cast<uint8_t*>(av_mallocz(io_buffer_size + 16));
	auto* fc = io_buffer ? avformat_alloc_context() : nullptr;

	// The I/O callbacks reach the file through the decoder, so they can also see whether to stop.
	_file = file;
	auto* pb = fc ? avio_alloc_context(io_buffer, io_buffer_size, 0, this, read_io, nullptr, seek_io) : nullptr;

	if (!pb)
	{
		// close() frees fc->pb itself, which is correct only while fc->pb is the context allocated
		// here. Handing avformat_open_input a null pb would let FFmpeg open the file and own the
		// AVIOContext, and the teardown would then free it twice.
		if (fc) avformat_free_context(fc);
		av_freep(&io_buffer);
		_file.reset();
		df::log(__FUNCTION__, "could not allocate the format context");
		return false;
	}

	fc->pb = pb;
	fc->flags |= AVFMT_FLAG_GENPTS;
	fc->interrupt_callback = {interrupt_io, this};

	AVDictionary* opts = nullptr;
	av_dict_set_int(&opts, "export_xmp", 1, 0);

	if (avformat_open_input(&fc, str::utf8_to_a(path.str()).c_str(), nullptr, &opts) != 0)
	{
		// avformat_open_input frees fc on failure and sets it to NULL,
		// but pb and its buffer are not freed. We saved pb above.
		av_dict_free(&opts);
		av_freep(&pb->buffer);
		avio_context_free(&pb);
		_file.reset();
		return false;
	}

	av_dict_free(&opts);

	_format_context = fc;
	_path = path;

	// avformat_find_stream_info keeps entropy-decoding H.264 until it has guessed the reorder delay -
	// 7 frames, up to 20 - and nothing a metadata scan reports uses that answer. Two separate bounds
	// hold it back, both applied AFTER open_input so container metadata is already gathered and
	// demuxers that consume probesize in their own header read (mpeg-ts) are untouched.
	const auto opts_stream_count = fc->nb_streams;
	AVDictionary** stream_opts = alloc_stream_probe_options(fc);

	if (intent == media_intent::metadata)
	{
		// The probe decoder is the only entropy decoding an index scan performs, and H.264 is the one
		// codec the probe keeps decoding after the stream is already characterised. AVDISCARD_ALL drops
		// each slice once its header is read; the fork applies the SPS on that path, so width, height,
		// pixel format and frame rate still land. This bounds decoding, not reading, so unlike the read
		// bound below it needs no guarantee about the header - a stream the demuxer has to discover is
		// still discovered, just without the entropy decode. find_stream_info takes one dictionary per
		// stream and hands it to that stream's probe decoder at avcodec_open2.
		if (stream_opts)
		{
			for (unsigned i = 0; i < opts_stream_count; ++i)
			{
				const auto* const stream = fc->streams[i];

				if (stream && stream->codecpar && stream->codecpar->codec_id == AV_CODEC_ID_H264)
				{
					av_dict_set(&stream_opts[i], "skip_frame", "all", 0);
				}
			}
		}

		// The read bound is the narrower of the two: it truncates the probe, so it is only safe for a
		// container that already named its streams. Anything that must be discovered by reading still
		// gets the full probe. Probe cost is roughly linear in bytes read, so probesize is the budget.
		if (has_header_codec_parameters(fc))
		{
			fc->probesize = df::two_fifty_six_k * 2;
			fc->max_analyze_duration = AV_TIME_BASE;
		}
	}

	avformat_find_stream_info(fc, stream_opts);

	free_stream_probe_options(&stream_opts, opts_stream_count);

	if (!has_presentable_stream(fc))
	{
		df::log(__FUNCTION__, std::format("no media stream in {}", path.name()));
		close();
		return false;
	}

	// Read the bit rate only after probing: many containers (and every stream that
	// needs its rate estimating) report 0 until avformat_find_stream_info has run.
	_bitrate = fc->bit_rate;

	auto audio_stream_count = 0;

	for (int i = 0; i < static_cast<int>(fc->nb_streams); ++i)
	{
		const auto* const stream = fc->streams[i];

		if (stream)
		{
			const AVDictionaryEntry* tag = nullptr;
			av_stream_info s;
			s.index = i;

			auto* const codec = stream->codecpar;

			while (stream->metadata && (tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
			{
				if (is_key(tag->key, "title")) s.title = str::utf8_cast(tag->value);
				if (is_key(tag->key, "codec")) s.codec = str::utf8_cast(tag->value);
				if (is_key(tag->key, "FourCC")) s.fourcc = str::utf8_cast(tag->value);
				if (is_key(tag->key, "language")) s.language = str::utf8_cast(tag->value);
				s.metadata.emplace_back(tag->key, str::utf8_cast(tag->value));
			}

			s.is_commentary = (stream->disposition & AV_DISPOSITION_COMMENT) != 0;
			s.is_audio_description = (stream->disposition & AV_DISPOSITION_VISUAL_IMPAIRED) != 0;
			s.rotation = df::round(get_rotation(stream));

			if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) && !_cover_art)
			{
				const auto& packet = stream->attached_pic;
				const auto decoded = load_image_file({packet.data, static_cast<size_t>(packet.size)});
				if (decoded) _cover_art = decoded;
			}

			if (codec)
			{
				if (s.fourcc.empty() && stream->codecpar->codec_tag)
				{
					char name[AV_FOURCC_MAX_STRING_SIZE];
					s.fourcc = str::utf8_cast(av_fourcc_make_string(name, stream->codecpar->codec_tag));
				}

				if (codec->codec_type == AVMEDIA_TYPE_VIDEO)
				{
					s.pixel_format = pixel_format_name(codec->format);

					if (const auto* const profile = avcodec_profile_name(codec->codec_id, codec->profile))
					{
						s.profile = str::utf8_cast(profile);
					}

					if (const auto* const desc = codec->format == AV_PIX_FMT_NONE
						                             ? nullptr
						                             : av_pix_fmt_desc_get(static_cast<AVPixelFormat>(codec->format)))
					{
						s.bit_depth = desc->comp[0].depth;
					}

					if (codec->color_trc == AVCOL_TRC_SMPTE2084) s.hdr_transfer = "PQ";
					else if (codec->color_trc == AVCOL_TRC_ARIB_STD_B67) s.hdr_transfer = "HLG";
				}

				if (codec->codec_type == AVMEDIA_TYPE_AUDIO)
				{
					s.audio_sample_rate = codec->sample_rate;
					s.audio_channels = codec->ch_layout.nb_channels;
					s.audio_sample_type = to_sample_type(static_cast<AVSampleFormat>(codec->format));
				}
				const auto codec_name = std::string(str::utf8_cast(avcodec_get_name(codec->codec_id)));
				if (s.codec.empty()) s.codec = codec_name;
				s.metadata.emplace_back("codec"_c, codec_name);

				if (codec->codec_tag)
				{
					char name[AV_FOURCC_MAX_STRING_SIZE];
					s.metadata.emplace_back("fourcc"_c,
					                        std::string(
						                        str::utf8_cast(av_fourcc_make_string(name, codec->codec_tag))));
				}

				switch (codec->codec_type)
				{
				case AVMEDIA_TYPE_SUBTITLE:
					s.type = av_stream_type::subtitle;
					break;
				case AVMEDIA_TYPE_VIDEO:
					s.type = av_stream_type::video;
					break;
				case AVMEDIA_TYPE_AUDIO:
					s.type = av_stream_type::audio;
					audio_stream_count += 1;
					break;
				case AVMEDIA_TYPE_UNKNOWN:
				case AVMEDIA_TYPE_DATA:
				case AVMEDIA_TYPE_ATTACHMENT:
				case AVMEDIA_TYPE_NB:
				default:
					s.type = av_stream_type::data;
					break;
				}
			}

			_streams.emplace_back(s);
		}
	}

	_has_multiple_audio_streams = audio_stream_count > 1;

	return true;
}

static AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                   const AVPixelFormat* pix_fmts)
{
	const auto wanted = av_platform_hw_decode_target().pix_fmt;
	const AVPixelFormat* p = pix_fmts;

	for (; *p != AV_PIX_FMT_NONE; p++)
	{
		if (*p == wanted)
			return *p;
	}

	// FFmpeg asks again without the hardware format when the driver refuses the stream at
	// initialisation, and lists a software format last whenever one exists. Answering NONE gave up
	// on a stream the decoder could still decode, and the picture simply never arrived.
	const auto software = p > pix_fmts ? *(p - 1) : AV_PIX_FMT_NONE;
	const auto* const desc = av_pix_fmt_desc_get(software);

	if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
	{
		df::log(__FUNCTION__, std::format("hardware decode refused - decoding {} in software", desc->name));
		return software;
	}

	df::log(__FUNCTION__, "Failed to get hardware surface format");
	return AV_PIX_FMT_NONE;
}

static const AVCodecHWConfig* find_hw_config(const AVCodec* codec, const av_hw_decode_target& target)
{
	// Advertised configs are not ordered, and a non-matching one must not end the search.
	for (int i = 0;; i++)
	{
		const auto* const config = avcodec_get_hw_config(codec, i);
		if (!config) return nullptr;

		if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->pix_fmt == target.pix_fmt &&
			static_cast<int>(config->device_type) == target.device_type)
		{
			return config;
		}
	}
}

// One decode device for the process. A device per video cost its creation on every open, and the
// renderer's cross-device handoff is keyed on the decode device, so each clip also rebuilt the shared
// texture chain. FFmpeg serialises decoders on the device's own lock, and only the playing session
// decodes in hardware. A device the driver has removed is replaced rather than handed out. The
// platform makes it on the renderer's GPU, which is what lets the handoff share pictures at all. It
// lives for the process: nothing else releases a GPU device at exit, and the system reclaims it.
static AVBufferRef* acquire_hw_decode_device(const int device_type)
{
	static platform::mutex mutex;
	static AVBufferRef* shared = nullptr;

	platform::exclusive_lock lock(mutex);

	if (shared && !av_platform_hw_device_usable(shared))
	{
		df::log(__FUNCTION__, "hardware decode device was removed - creating another");
		av_buffer_unref(&shared);
	}

	if (!shared) shared = av_platform_create_hw_device(device_type);
	if (!shared) return nullptr;

	return av_buffer_ref(shared);
}

struct hw_decode_choice
{
	const AVCodec* codec = nullptr;
	AVBufferRef* device = nullptr;
};

// Settled before the codec context exists, because the decoder itself can differ: FFmpeg's own AV1
// decoder only drives a hardware accelerator, while software AV1 is libdav1d, which has none.
static hw_decode_choice choose_hw_decode(const AVCodecParameters* par, const AVCodec* software_codec)
{
	const auto target = av_platform_hw_decode_target();
	if (!par || !target.is_available()) return {};

	const auto format = static_cast<AVPixelFormat>(par->format);
	if (!av_hw_decode_eligible(par->codec_id, format)) return {};

	const auto* const codec = par->codec_id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("av1") : software_codec;
	if (!codec || !find_hw_config(codec, target)) return {};

	auto* device = acquire_hw_decode_device(target.device_type);
	if (!device) return {};

	const auto* const desc = av_pix_fmt_desc_get(format);
	const auto bit_depth = desc ? desc->comp[0].depth : 8;

	if (!av_platform_hw_decode_supported(device, par->codec_id, bit_depth, par->width, par->height))
	{
		df::log(__FUNCTION__, std::format("no {} {}-bit hardware decoder at {} x {} - decoding in software",
		                                  avcodec_get_name(par->codec_id), bit_depth, par->width, par->height));
		av_buffer_unref(&device);
		return {};
	}

	return {codec, device};
}

// One decoded picture in the stream's own format, which is what a frame-threading decoder holds per
// thread.
static size_t decoded_frame_bytes(const AVCodecParameters* par)
{
	if (!par || par->width <= 0 || par->height <= 0) return 0;

	const auto format = static_cast<AVPixelFormat>(par->format);
	const auto bytes = format == AV_PIX_FMT_NONE ? 0 : av_image_get_buffer_size(format, par->width, par->height, 1);
	if (bytes > 0) return static_cast<size_t>(bytes);

	return static_cast<size_t>(par->width) * static_cast<size_t>(par->height) * 3 / 2;
}

// Surfaces this app checks out of the pool beyond the read-ahead queue: the frame on screen, the
// two update_for_present holds while it settles onto a sought position, and one for a frame the
// decoder emits after the queue has stopped asking for more.
static constexpr size_t hw_frames_held_outside_queue = 4;

// What one hardware surface costs in the decoder's pool. Deliberately computed from the coded
// size rather than the aligned one the pool actually rounds up to: under-reading the cost buys one
// more surface, and an over-read would buy one fewer than the queue is about to hold.
static size_t hw_surface_bytes(const AVCodecParameters* par)
{
	if (!par || par->width <= 0 || par->height <= 0) return 0;

	const auto* const desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
	const size_t bytes_per_sample = desc && desc->comp[0].depth > 8 ? 2 : 1;

	// NV12, or P010 when the stream is deeper than 8 bits: one full luma plane and a half-height
	// interleaved chroma plane.
	return static_cast<size_t>(par->width) * static_cast<size_t>(par->height) * 3 * bytes_per_sample / 2;
}


void av_format_decoder::init_streams(int video_track, int audio_track, const bool can_use_hw, const bool video_only,
                                     const bool can_use_threads)
{
	auto* const fc = _format_context;

#ifdef _DEBUG
#endif


	const AVStream* video_stream = nullptr;
	const AVStream* audio_stream = nullptr;

	// validate stream selection
	if (get_stream_type(fc, video_track) != AVMEDIA_TYPE_VIDEO) video_track = -1;
	if (get_stream_type(fc, audio_track) != AVMEDIA_TYPE_AUDIO) audio_track = -1;

	const AVCodec* video_codec = nullptr;
	const auto video_stream_index = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, video_track, -1, &video_codec, 0);

	if (video_stream_index >= 0)
	{
		video_stream = fc->streams[video_stream_index];

		// Same ceiling av_decode_still applies, for the same reason: a container is free to declare
		// an implausible frame size, and the index walk opens every av file it finds. Refused before
		// anything is allocated for it, including the hardware device context below.
		if (video_stream && video_stream->codecpar &&
			reject_over_budget_source(nullptr, {video_stream->codecpar->width, video_stream->codecpar->height},
			                          "ffmpeg video"))
		{
			video_stream = nullptr;
		}

		if (video_stream && video_stream->codecpar && video_codec)
		{
			const auto* const par = video_stream->codecpar;
			const auto is_cover_art = (video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
			auto hw = can_use_hw && !is_cover_art ? choose_hw_decode(par, video_codec) : hw_decode_choice{};
			if (hw.device) video_codec = hw.codec;

			auto* vc = avcodec_alloc_context3(video_codec);

			if (!vc)
			{
				av_buffer_unref(&hw.device);
			}
			else
			{
				avcodec_parameters_to_context(vc, par);
				vc->workaround_bugs = FF_BUG_AUTODETECT;

				if (hw.device)
				{
					_hw_device_ctx = hw.device;
					vc->get_format = get_hw_format;

					// The whole pool is one texture array created when the stream opens, so every
					// surface is paid for whether or not it is used. FFmpeg already provisions the
					// decoder's own reference frames; these are the extra surfaces this app checks
					// out - the read-ahead queue, the frame on screen, and the ones
					// update_for_present holds while it settles - so they follow the same byte
					// budget the queue does.
					vc->extra_hw_frames = static_cast<int>(
						av_read_ahead_frames(hw_surface_bytes(par)) + hw_frames_held_outside_queue);

					// Frame threading costs one more pool surface per thread and buys almost nothing
					// once the GPU is doing the decoding.
					vc->thread_count = 1;
					vc->hw_device_ctx = av_buffer_ref(_hw_device_ctx);
				}
				else
				{
					const auto caps = video_codec->capabilities;
					const auto threads = (caps & (AV_CODEC_CAP_FRAME_THREADS | AV_CODEC_CAP_SLICE_THREADS |
						AV_CODEC_CAP_OTHER_THREADS)) != 0;

					vc->thread_count = threads && can_use_threads && !is_cover_art
						                   ? av_video_decode_threads((caps & AV_CODEC_CAP_FRAME_THREADS) != 0,
						                                             decoded_frame_bytes(par),
						                                             std::thread::hardware_concurrency())
						                   : 1;
					vc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
				}

				// Decoding-side timing inputs. Without pkt_timebase FFmpeg cannot express a
				// frame duration or a priming-sample adjustment, and leaves both unset.
				vc->pkt_timebase = video_stream->time_base;
				vc->framerate = av_guess_frame_rate(fc, fc->streams[video_stream_index], nullptr);

				// codecpar can understate what the bitstream then asks for, so the ceiling the check
				// above applied is restated where the decoder itself enforces it.
				if (const auto max_pixels = codec_pixel_ceiling(video_codec->id); max_pixels > 0)
				{
					vc->max_pixels = max_pixels;
				}

				if (avcodec_open2(vc, video_codec, nullptr) == 0)
				{
					_has_video = true;
					_video_base = {video_stream->time_base.num, video_stream->time_base.den};
					_video_stream_index = video_stream_index;
					_video_context = vc;
					_video_stream_aspect_ratio = {
						video_stream->sample_aspect_ratio.num, video_stream->sample_aspect_ratio.den
					};
					_rotation = df::round(get_rotation(video_stream));
				}
				else
				{
					// Publishing the index would route every packet of this stream into a queue
					// nothing drains; publishing the context would hand receive_frames a context
					// that was never opened.
					avcodec_free_context(&vc);
					video_stream = nullptr;
				}

				// Hardware decode is now live for this decoder. Raise the process-wide crash
				// guard so a fault during HW decode is detected on the next launch and only HW
				// video decoding is disabled (GPU rendering is left on).
				if (_has_video && _hw_device_ctx && !_hw_decode_guard_held)
				{
					_hw_decode_guard_held = true;
					if (g_hw_decode_sessions.fetch_add(1) == 0)
					{
						platform::set_crash_guard(platform::crash_guard::hw_video_decode, true);
					}
					df::log(__FUNCTION__, "hardware video decode active");
				}
			}
		}
	}

	if (!video_only)
	{
		const AVCodec* aud_decoder = nullptr;
		const auto aud_stream = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, audio_track, video_stream_index,
		                                            &aud_decoder, 0);

		if (aud_stream >= 0)
		{
			audio_stream = fc->streams[aud_stream];

			if (audio_stream && audio_stream->codecpar && aud_decoder)
			{
				auto* ac = avcodec_alloc_context3(aud_decoder);

				if (ac)
				{
					avcodec_parameters_to_context(ac, audio_stream->codecpar);
					ac->workaround_bugs = FF_BUG_AUTODETECT;
					ac->request_sample_fmt = AV_SAMPLE_FMT_S16;
					// Required for FFmpeg to shift the timestamps of gapless formats (AAC, MP3,
					// Opus) by their encoder delay. Without it the decoder still drops the priming
					// samples but leaves the PTS where it was, so the audio timeline starts early
					// by that delay and every video frame is matched against it.
					ac->pkt_timebase = audio_stream->time_base;

					if (avcodec_open2(ac, aud_decoder, nullptr) == 0)
					{
						_has_audio = true;
						_audio_base = {audio_stream->time_base.num, audio_stream->time_base.den};
						_audio_stream_index = aud_stream;
						_audio_context = ac;
					}
					else
					{
						avcodec_free_context(&ac);
						audio_stream = nullptr;
					}
				}
			}
		}
	}

	for (auto&& st : _streams)
	{
		st.is_playing = st.index == _audio_stream_index || st.index == _video_stream_index;
	}

	// A stream nothing decodes is skipped by the demuxer rather than read and thrown away: MP4 and MOV
	// never read a discarded track, so a picture-only preview does not read the soundtrack beside it.
	for (unsigned i = 0; fc && i < fc->nb_streams; ++i)
	{
		const auto index = static_cast<int>(i);

		if (fc->streams[i] && index != _video_stream_index && index != _audio_stream_index)
		{
			fc->streams[i]->discard = AVDISCARD_ALL;
		}
	}

	// One origin for the whole presentation: the earliest start among the streams actually
	// being played, expressed once and then rescaled into each stream's own time base. Taking
	// each stream's own start_time (as this used to) pulled both to zero independently and so
	// discarded the offset between them - the container-level A/V delay that MPEG-TS and
	// edit-listed MP4 rely on. The earliest playing stream, rather than fc->start_time, keeps
	// the result non-negative even when the container's own figure covers streams we ignore.
	auto origin = std::numeric_limits<int64_t>::max();

	if (video_stream && video_stream->start_time != AV_NOPTS_VALUE)
	{
		origin = std::min(origin, av_rescale_q(video_stream->start_time, video_stream->time_base, AV_TIME_BASE_Q));
	}

	if (audio_stream && audio_stream->start_time != AV_NOPTS_VALUE)
	{
		origin = std::min(origin, av_rescale_q(audio_stream->start_time, audio_stream->time_base, AV_TIME_BASE_Q));
	}

	if (origin == std::numeric_limits<int64_t>::max())
	{
		origin = fc && fc->start_time != AV_NOPTS_VALUE ? fc->start_time : 0;
	}

	_time_origin = origin;
	_video_start_time = video_stream ? av_rescale_q(origin, AV_TIME_BASE_Q, video_stream->time_base) : 0;
	_audio_start_time = audio_stream ? av_rescale_q(origin, AV_TIME_BASE_Q, audio_stream->time_base) : 0;

	double end_time_context = 0;
	double end_time_video = 0;
	double end_time_audio = 0;

	if (fc && fc->duration != AV_NOPTS_VALUE)
	{
		end_time_context = calc_duration(fc->duration, AV_NOPTS_VALUE);
	}

	if (video_stream && video_stream->duration != AV_NOPTS_VALUE)
	{
		end_time_video = calc_duration(video_stream->duration, video_stream->time_base, AV_NOPTS_VALUE);
	}

	if (audio_stream && audio_stream->duration != AV_NOPTS_VALUE)
	{
		end_time_audio = calc_duration(audio_stream->duration, audio_stream->time_base, AV_NOPTS_VALUE);
	}

	_start_time = 0.0; // std::min(start_time_context, start_time_video, start_time_audio);
	_end_time = _start_time + std::max(std::max(end_time_context, end_time_video), end_time_audio);
}

av_media_info av_format_decoder::info() const
{
	const auto vid_info = video_information();

	av_media_info result;
	result.streams = _streams;
	result.has_multiple_audio_streams = _has_multiple_audio_streams;
	result.has_audio = _has_audio;
	result.has_video = _has_video;
	result.bitrate = _bitrate;
	result.start = _start_time;
	result.end = _end_time;
	result.cover_art = _cover_art;
	result.render_dimensions = vid_info.render_dimensions;
	result.display_dimensions = vid_info.display_dimensions;
	result.display_orientation = calc_orientation();
	result.video_frame_rate = video_frame_rate();

	const auto* ctx = _format_context;

	if (ctx)
	{
		metadata_kv_list kv;
		const AVDictionaryEntry* tag = nullptr;

		while ((tag = av_dict_get(ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
		{
			if (str::icmp(tag->key, "id3v2_priv.XMP") == 0 || str::icmp(tag->key, "xmp") == 0)
			{
				const auto packet = unescape_xmp(tag->value);
				bool parsed = false;
				auto xmp_kv = metadata_xmp::to_info(packet, parsed);

				// The packet is the block's real content, so it stays reachable whether or not the
				// toolkit could make a tree from it.
				constexpr size_t max_raw_bytes = 256 * 1024;
				std::string raw;
				raw.assign(std::bit_cast<const char*>(packet.data()), std::min(packet.size(), max_raw_bytes));

				result.metadata.emplace_back(metadata_standard::xmp, std::move(xmp_kv), packet.size(), parsed,
				                             std::move(raw));
			}
			else
			{
				kv.emplace_back(tag->key, str::utf8_cast(tag->value));
			}
		}

		result.metadata.emplace_back(metadata_standard::ffmpeg, kv);

		auto video_properties_set = false;
		for (uint32_t i = 0; i < ctx->nb_streams; ++i)
		{
			const auto* const stream = ctx->streams[i];
			const auto* const codec = stream->codecpar;

			if (!codec)
			{
				continue;
			}

			if (codec->codec_type == AVMEDIA_TYPE_VIDEO &&
				(stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0 && !video_properties_set)
			{
				video_properties_set = true;
				if (const auto name = decoder_name(codec); !str::is_empty(name)) result.video_codec = name;
				if (const auto fmt = pixel_format_name(codec->format); !str::is_empty(fmt)) result.pixel_format = fmt;
			}
			else if (codec->codec_type == AVMEDIA_TYPE_AUDIO)
			{
				if (const auto name = decoder_name(codec); !str::is_empty(name)) result.audio_codec = name;

				result.audio_sample_rate = codec->sample_rate;
				result.audio_channels = codec->ch_layout.nb_channels;
				result.audio_sample_type = to_sample_type(static_cast<AVSampleFormat>(codec->format));
			}
		}
	}

	return result;
}


ui::orientation av_format_decoder::calc_orientation() const
{
	return calc_orientation_impl(_rotation);
}

void av_format_decoder::update_orientation(const AVFrame* const frame)
{
	if (!frame)
	{
		return;
	}

	const AVFrameSideData* side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);

	if (side_data)
	{
		// Adopt the frame's rotation outright. Subtracting the whole-turn part of the
		// angle (as this used to) left _rotation at the container value and silently
		// dropped a rotation signalled only per frame.
		_rotation = df::round(rotation_from_display_matrix(side_data->data, side_data->size));
	}
}

bool av_format_decoder::decode_frame(ui::surface_ptr& dest_surface, AVCodecContext* ctx, const AVPacket* packet,
                                     const sizei max_dim)
{
	if (try_avcodec_send_packet(ctx, packet) != 0) return false;

	// av_frame_alloc rather than a local AVFrame: the struct's size is FFmpeg's to change, and only the
	// allocator is sized to the library actually linked.
	auto* frame = av_frame_alloc();
	if (!frame) return false;
	const df::scope_exit free_frame([&frame] { av_frame_free(&frame); });

	if (avcodec_receive_frame(ctx, frame) != 0) return false;

	const auto pts = _pts_vid.guess(frame->best_effort_timestamp, frame->pts, frame->pkt_dts, frame->duration);
	auto time = to_video_seconds(pts);

	if (frame->repeat_pict)
	{
		time += 1 / 25.0;
	}

	if (!_scaler) _scaler = std::make_unique<av_scaler>();
	return _scaler->scale_frame(*frame, dest_surface, max_dim, time, calc_orientation(), _video_stream_aspect_ratio);
}


bool av_format_decoder::decode_nearest_frame(ui::surface_ptr& dest_surface, const sizei max_dim,
                                             const double wanted_time, const double tolerance,
                                             df::cancel_token abandon)
{
	if (!_has_video)
	{
		return false;
	}

	auto* const ctx = _video_context;

	if (!_scaler)
	{
		_scaler = std::make_unique<av_scaler>();
	}

	// A seek lands on the key frame at or before the target, so every frame between the two is
	// decoded on the way. Only a reference to the nearest one is kept: scaling each improving frame
	// in turn cost a bicubic pass and a fresh surface per frame of the GOP, and all but the last
	// were thrown away.
	av_frame best;
	av_nearest_frame_choice choice;

	const auto accept_frame = [&](av_frame& frame)
	{
		const auto pts = _pts_vid.guess(frame.frm->best_effort_timestamp, frame.frm->pts, frame.frm->pkt_dts,
		                                frame.frm->duration);
		const auto time = to_video_seconds(pts);

		frame.time = time;
		frame.orientation = calc_orientation();

		if (time <= wanted_time || !_sequential_prev)
		{
			_sequential_prev = std::make_shared<av_frame>(frame);
		}

		if (time >= wanted_time)
		{
			_sequential_next = std::make_shared<av_frame>(frame);
		}

		// Frames arrive in presentation order, so the distance to the target shrinks until we
		// pass it; the last improvement is the nearest frame.
		if (choice.consider(time, wanted_time, tolerance))
		{
			av_frame_unref(best.frm);
			av_frame_ref(best.frm, frame.frm);
		}
	};

	const auto drain_delayed_frames = [&]()
	{
		if (try_avcodec_send_packet(ctx, nullptr) != 0)
		{
			return;
		}

		av_frame frame;

		while (!choice.reached && avcodec_receive_frame(ctx, frame.frm) == 0)
		{
			accept_frame(frame);
			av_frame_unref(frame.frm);
		}

		avcodec_flush_buffers(ctx);
	};

	// The walk normally stops the moment it passes wanted_time; this only bounds a stream that
	// never gets there (a truncated or corrupt file). It therefore has to be wide enough to
	// span a whole GOP of interleaved video and audio packets - a ten second GOP alone is well
	// over a thousand - or the caller silently gets a frame short of the position it asked for.
	constexpr int max_packets = 8192;

	for (int i = 0; i < max_packets && !choice.reached && !df::is_closing; i++)
	{
		const auto packet = read_packet();

		if (!packet || packet->eof)
		{
			if (packet && packet->eof && should_drain_delayed_video_frames_at_eof(choice))
			{
				drain_delayed_frames();
			}
			break;
		}

		if (packet->pkt->stream_index != _video_stream_index)
		{
			continue;
		}

		if (try_avcodec_send_packet(ctx, packet->pkt) != 0)
		{
			continue;
		}

		av_frame frame;

		while (!choice.reached && avcodec_receive_frame(ctx, frame.frm) == 0)
		{
			accept_frame(frame);
			av_frame_unref(frame.frm);
		}

		// The caller always needs something to show, so the first frame decoded is never given up -
		// only the refinement toward the exact one is. A pointer that has already moved on makes that
		// refinement worthless.
		if (choice.found && abandon.is_cancelled())
		{
			break;
		}
	}

	return choice.found && _scaler->scale_frame(*best.frm, dest_surface, max_dim, choice.best_time,
	                                            calc_orientation(), _video_stream_aspect_ratio);
}

bool av_format_decoder::extract_seek_frame(ui::surface_ptr& dest_surface, const sizei max_dim,
                                           const double pos_numerator,
                                           const double pos_denominator, df::cancel_token abandon)
{
	if (!_has_video)
	{
		return false;
	}

	auto* const ctx = _video_context;
	const auto start = start_time();
	const auto len = end_time() - start;

	// Use the same target time the live scrubber seeks to (media start + a
	// fraction of the duration) and then decode forward to the frame nearest that
	// time. Showing only the first (key) frame after the seek - as extract_thumbnail
	// used to - leaves the preview up to a whole GOP away from the pointed-at
	// position, which is not the frame the player jumps to.
	const auto x = std::clamp(pos_numerator, 0.0, pos_denominator);
	const auto wanted_time = start + floor(x * len / std::max(1.0, pos_denominator));

	seek(wanted_time);

	// The preview decoder is reused across hovers without going through the normal
	// flush path, so drop any buffered frames and reset the timestamp estimator -
	// otherwise a backward hover is pulled forward by guess()'s monotonic guard.
	avcodec_flush_buffers(ctx);
	_pts_vid.clear();

	return decode_nearest_frame(dest_surface, max_dim, wanted_time, 0.0, abandon);
}

bool av_format_decoder::extract_thumbnail(ui::surface_ptr& dest_surface, const sizei max_dim,
                                          const double pos_numerator,
                                          const double pos_denominator,
                                          const bool exact_frame,
                                          const double tolerance_fraction, df::cancel_token abandon)
{
	auto success = false;

	if (_has_video)
	{
		auto* const ctx = _video_context;
		const auto duration = end_time() - start_time();
		const auto time_wanted = duration * pos_numerator / pos_denominator;

		if (duration > 0)
		{
			// The preview decoder is reused across hovers, so it only sits at the start of the stream
			// when it has just been opened. Without a seek, a hover near the start answers from
			// wherever the previous hover left the decoder - and every frame from there is already
			// past the requested time, so the walk returns the first one it sees.
			auto seek_success = seek(time_wanted);

			if (seek_success)
			{
				// A container-level seek does not flush the decoder, so drop any
				// frames buffered before the seek and reset the timestamp estimator -
				// otherwise the thumbnail can come from a stale pre-seek frame (matches
				// the flush in extract_seek_frame).
				avcodec_flush_buffers(ctx);
				_pts_vid.clear();
			}
			else
			{
				// Decoding forward is only meaningful from a known position, so a stream that cannot
				// seek answers from the start and nowhere else.
				seek_success = time_wanted <= 2.0;
			}

			if (seek_success)
			{
				if (exact_frame)
				{
					success = decode_nearest_frame(dest_surface, max_dim, time_wanted,
					                               duration * std::max(0.0, tolerance_fraction), abandon);
				}
				else
				{
					for (int i = 0; i < 1024 && !success && !df::is_closing; i++)
					{
						const auto packet = read_packet();

						if (!packet || packet->eof)
						{
							// A decoder that holds pictures back for reordering or threading keeps the
							// only picture of a short clip until it is drained, and stopping here at
							// the end of the stream left that clip with no thumbnail at all.
							if (packet && packet->eof)
							{
								success = decode_frame(dest_surface, ctx, nullptr, max_dim);
								avcodec_flush_buffers(ctx);
							}

							break;
						}

						if (packet->pkt->stream_index == _video_stream_index)
						{
							success = decode_frame(dest_surface, ctx, packet->pkt, max_dim);
						}
					}
				}
			}
		}
	}

	return success;
}

bool av_format_decoder::extract_frame_at(ui::surface_ptr& dest_surface, const sizei max_dim, const double wanted_time,
                                         const double tolerance_seconds, df::cancel_token abandon)
{
	if (!_has_video || !_video_context)
	{
		return false;
	}

	const auto start = start_time();
	const auto target = std::clamp(wanted_time, start, std::max(start, end_time()));

	if (_sequential_prev && fabs(_sequential_prev->time - target) <= tolerance_seconds)
	{
		_sequential_time = _sequential_prev->time;
		return scale_sequential_frame(dest_surface, max_dim, *_sequential_prev);
	}

	if (_sequential_next && fabs(_sequential_next->time - target) <= tolerance_seconds)
	{
		_sequential_time = _sequential_next->time;
		return scale_sequential_frame(dest_surface, max_dim, *_sequential_next);
	}

	if (_sequential_prev && _sequential_next &&
		_sequential_prev->time <= target && target <= _sequential_next->time)
	{
		const auto& nearest = fabs(_sequential_prev->time - target) <= fabs(_sequential_next->time - target)
			                      ? _sequential_prev
			                      : _sequential_next;
		_sequential_time = nearest->time;
		return scale_sequential_frame(dest_surface, max_dim, *nearest);
	}

	if (_sequential_prev && target < _sequential_prev->time)
	{
		clear_sequential_frame_bracket();
	}
	else if (_sequential_next && target > _sequential_next->time)
	{
		_sequential_prev = _sequential_next;
		_sequential_next.reset();
		_sequential_time = _sequential_prev->time;
	}

	// Walking forward is the whole point, but only while walking is the cheaper answer. Past this
	// much unread stream a seek reaches the target sooner than decoding every frame between.
	constexpr double max_forward_walk_seconds = 3.0;

	if (_eof || _sequential_time < 0 || target < _sequential_time || target > _sequential_time + max_forward_walk_seconds)
	{
		// A container-level seek does not flush the decoder, so buffered pre-seek frames are dropped
		// and the timestamp estimator reset - the same pairing every other seeking caller here uses.
		if (!seek(target) && target > start + 2.0)
		{
			return false;
		}

		avcodec_flush_buffers(_video_context);
		_pts_vid.clear();
		clear_sequential_frame_bracket();
	}

	ui::surface_ptr decoded;

	if (!decode_nearest_frame(decoded, max_dim, target, std::max(0.0, tolerance_seconds), abandon))
	{
		// A walk that ran out of stream past the last frame holds the nearest frame there is, and a
		// seek to the same time lands on it too. A container whose sound outlasts its picture asks
		// exactly this at its end, and failing it failed every Movie render holding such a clip.
		if (_eof && _sequential_prev && _sequential_prev->time <= target)
		{
			_sequential_time = _sequential_prev->time;
			return scale_sequential_frame(dest_surface, max_dim, *_sequential_prev);
		}

		// The decoder is now somewhere unknown - at end of stream, or wherever a failed walk left
		// it - so the next call must seek rather than assume it can step on from here.
		_sequential_time = -1;
		return false;
	}

	if (_sequential_prev && should_use_kept_sequential_frame(_sequential_prev->time, decoded->time(), target))
	{
		_sequential_time = _sequential_prev->time;
		return scale_sequential_frame(dest_surface, max_dim, *_sequential_prev);
	}

	_sequential_time = decoded->time();
	dest_surface = std::move(decoded);
	return true;
}

// One sample as a signed value in -1..1. Only the formats FFmpeg's decoders actually emit are
// handled; anything else answers silence rather than reading the buffer as the wrong type.
static double read_audio_sample(const uint8_t* base, const AVSampleFormat packed, const int index)
{
	switch (packed)
	{
	case AV_SAMPLE_FMT_U8:
		return (static_cast<int>(base[index]) - 128) / 128.0;
	case AV_SAMPLE_FMT_S16:
		return std::bit_cast<const int16_t*>(base)[index] / 32768.0;
	case AV_SAMPLE_FMT_S32:
		return std::bit_cast<const int32_t*>(base)[index] / 2147483648.0;
	case AV_SAMPLE_FMT_FLT:
		return std::bit_cast<const float*>(base)[index];
	case AV_SAMPLE_FMT_DBL:
		return std::bit_cast<const double*>(base)[index];
	default:
		return 0.0;
	}
}

// Confined to the test's calling thread; lets a test give one decoded audio frame the decode error a
// damaged packet produces.
thread_local std::function<int(int)> test_audio_receive_status;

static int receive_audio_frame(AVCodecContext* const context, AVFrame* const frame)
{
	const auto status = avcodec_receive_frame(context, frame);
	return test_audio_receive_status ? test_audio_receive_status(status) : status;
}

static audio_info_t frame_audio_info(const AVFrame& frame, const audio_info_t& fallback)
{
	audio_info_t result;
	result.channel_layout = av_channel_layout_check(&frame.ch_layout)
		                        ? av_copy_to_ptr(frame.ch_layout) : fallback.channel_layout;
	result.sample_fmt = frame.format < 0 ? fallback.sample_fmt : to_sample_type(static_cast<AVSampleFormat>(frame.format));
	result.sample_rate = frame.sample_rate == 0 ? fallback.sample_rate : frame.sample_rate;
	return result;
}

// Peak, not mean: a level indicator exists so speech stands out at a glance, and averaging over a
// bucket flattens exactly the thing being looked for.
static void accumulate_audio_peaks(const AVFrame& frame, std::vector<uint8_t>& peaks,
                                   const double samples_per_bucket, int64_t& sample_index)
{
	const auto samples = frame.nb_samples;
	if (samples <= 0 || peaks.empty() || samples_per_bucket <= 0) return;

	const auto fmt = static_cast<AVSampleFormat>(frame.format);
	const auto packed = av_get_packed_sample_fmt(fmt);
	const auto channels = std::max(1, frame.ch_layout.nb_channels);

	// One channel is read, not all of them: this is a level indicator, and reading the first keeps
	// the walk linear in the stream's length rather than in its channel count.
	const auto* const base = frame.extended_data ? frame.extended_data[0] : frame.data[0];
	const auto stride = av_sample_fmt_is_planar(fmt) ? 1 : channels;

	if (base)
	{
		for (int i = 0; i < samples; ++i)
		{
			const auto bucket = static_cast<size_t>((sample_index + i) / samples_per_bucket);
			if (bucket >= peaks.size()) break;

			const auto level = std::clamp(std::abs(read_audio_sample(base, packed, i * stride)), 0.0, 1.0);
			peaks[bucket] = std::max(peaks[bucket], static_cast<uint8_t>(std::lround(level * 255.0)));
		}
	}

	sample_index += samples;
}

std::vector<uint8_t> av_format_decoder::extract_audio_peaks(const int buckets, df::cancel_token abandon)
{
	if (!_has_audio || !_audio_context || buckets <= 0)
	{
		return {};
	}

	const auto duration = end_time() - start_time();
	const auto sample_rate = _audio_context->sample_rate;

	if (duration <= 0 || sample_rate <= 0)
	{
		return {};
	}

	std::vector<uint8_t> result(static_cast<size_t>(buckets), 0);

	// Buckets are filled by sample position rather than by timestamp, so a stream whose timestamps
	// are broken still lands its samples in the column they belong to.
	const auto samples_per_bucket = std::max(1.0, duration * sample_rate / buckets);
	int64_t sample_index = 0;

	_io_abandon = &abandon;
	const df::scope_exit end_abandon([this] { _io_abandon = nullptr; });
	const auto keep_video_unread = discard_video_while_reading_audio();

	av_frame frame;
	// A damaged packet costs only its own samples; the rest of the stream still draws its waveform.
	const auto receive = [&]
	{
		int status = 0;
		while ((status = receive_audio_frame(_audio_context, frame.frm)) == 0)
		{
			accumulate_audio_peaks(*frame.frm, result, samples_per_bucket, sample_index);
			av_frame_unref(frame.frm);
		}
		if (status != AVERROR(EAGAIN) && status != AVERROR_EOF)
		{
			df::log_once(__FUNCTION__, std::format("Cannot decode waveform samples: {}", status));
		}
	};

	for (;;)
	{
		if (df::is_closing || abandon.is_cancelled())
		{
			return {};
		}

		const auto packet = read_packet();

		if (!packet || packet->eof)
		{
			auto status = try_avcodec_send_packet(_audio_context, nullptr);
			if (status == AVERROR(EAGAIN))
			{
				receive();
				status = try_avcodec_send_packet(_audio_context, nullptr);
			}
			if (status < 0 && status != AVERROR_EOF)
			{
				df::log_once(__FUNCTION__, std::format("Cannot drain waveform decoder: {}", status));
			}
			else
			{
				receive();
			}
			break;
		}

		if (packet->pkt->stream_index != _audio_stream_index)
		{
			continue;
		}

		auto status = try_avcodec_send_packet(_audio_context, packet->pkt);
		if (status == AVERROR(EAGAIN))
		{
			receive();
			status = try_avcodec_send_packet(_audio_context, packet->pkt);
		}
		if (status < 0)
		{
			df::log_once(__FUNCTION__, std::format("Cannot submit waveform packet: {}", status));
			continue;
		}
		receive();
	}

	return result;
}

double av_format_decoder::to_video_seconds(const int64_t vt) const
{
	return calc_duration(vt, {_video_base.num, _video_base.den}, _video_start_time);
}

// Interleaved stereo, so one sample pair is two adjacent values and a position in the clip is a
// multiply. Every caller of this wants to play from a time, not to walk a packet stream.
static constexpr int pcm_channels = 2;
// Confined to the test's calling thread; other decoder workers never see injected frames.
thread_local std::function<void(AVFrame&)> test_audio_pcm_frame;

std::vector<int16_t> av_format_decoder::extract_audio_pcm(const int sample_rate, const double max_seconds,
                                                          df::cancel_token abandon)
{
	return extract_audio_pcm_range(sample_rate, 0, max_seconds, std::move(abandon));
}

std::vector<int16_t> av_format_decoder::extract_audio_pcm_range(const int sample_rate, const double start_seconds,
	const double duration_seconds, df::cancel_token abandon)
{
	constexpr double max_range_seconds = 60.0;
	constexpr int max_sample_rate = 384000;
	const auto max_timestamp_seconds = static_cast<double>(std::numeric_limits<int64_t>::max()) / AV_TIME_BASE;

	if (!_has_audio || !_audio_context || sample_rate <= 0 || sample_rate > max_sample_rate ||
		!std::isfinite(start_seconds) || !std::isfinite(duration_seconds) || start_seconds < 0 ||
		duration_seconds <= 0 || duration_seconds > max_range_seconds ||
		start_seconds > max_timestamp_seconds - duration_seconds)
	{
		return {};
	}

	const auto layout = av_get_def_channel_layout(pcm_channels);
	if (!layout) return {};

	SwrContext* swr = nullptr;
	const df::scope_exit release_resampler([&swr] { swr_free(&swr); });
	const auto stream_format = audio_info();
	audio_info_t input_format;

	const auto wanted_start = std::max(0.0, start_seconds);
	const auto wanted_frames_double = std::ceil(duration_seconds * sample_rate);
	if (wanted_frames_double > static_cast<double>(std::numeric_limits<size_t>::max() / pcm_channels))
	{
		return {};
	}

	const auto wanted_frames = static_cast<size_t>(wanted_frames_double);
	std::vector<int16_t> result(wanted_frames * pcm_channels, 0);
	std::vector<int16_t> converted;
	auto finished = false;
	auto failed = false;
	std::optional<int64_t> next_destination_frame;

	_io_abandon = &abandon;
	const df::scope_exit end_abandon([this] { _io_abandon = nullptr; });

	// Sought before the video track is set aside: a demuxer that finds key frames by reading them
	// needs that track to land the seek.
	seek(wanted_start);
	avcodec_flush_buffers(_audio_context);
	_pts_aud.clear();
	const auto keep_video_unread = discard_video_while_reading_audio();

	// Shared by the decode loop and the flush that follows it, so the tail swr is holding is not
	// left behind - a dropped tail is a click at the end of every clip.
	const auto drain = [&](const AVFrame* frame, const double frame_time)
	{
		if (!swr) return 0;
		const auto capacity = swr_get_out_samples(swr, frame ? frame->nb_samples : 0);
		if (capacity < 0)
		{
			df::log(__FUNCTION__, std::format("Cannot size PCM resampling output: {}", capacity));
			failed = finished = true;
			return 0;
		}
		if (capacity == 0) return 0;

		converted.resize(static_cast<size_t>(capacity) * pcm_channels);

		auto* out = std::bit_cast<uint8_t*>(converted.data());
		const auto in = frame ? frame->extended_data : nullptr;
		// The rate product expresses the buffered delay exactly, including fractional output samples.
		// A converted block starts before its input frame's timestamp by this delay.
		const auto delayed_frames = frame
			                            ? static_cast<double>(swr_get_delay(
				                            swr, static_cast<int64_t>(input_format.sample_rate) * sample_rate)) /
				                            input_format.sample_rate
			                            : 0.0;
		const auto produced = swr_convert(swr, &out, capacity, in, frame ? frame->nb_samples : 0);

		if (produced < 0)
		{
			df::log(__FUNCTION__, std::format("Cannot resample PCM frame: {}", produced));
			failed = finished = true;
			return 0;
		}
		if (produced == 0) return 0;

		const auto source_frames = static_cast<int64_t>(produced);

		// A malformed container can state a timestamp far outside anything the clip covers, and
		// llround of a double past int64's range is undefined. Anything beyond the buffer
		// contributes nothing regardless - the clamps below reduce it to a zero-length copy - so
		// bounding the offset changes no placement while keeping the conversion defined.
		const auto placement_limit = static_cast<double>(wanted_frames) + static_cast<double>(source_frames) + 1.0;
		const auto offset_frames = (frame_time - wanted_start) * sample_rate - delayed_frames;
		const auto timestamp_frame = std::isfinite(offset_frames)
			                             ? static_cast<int64_t>(std::llround(
				                             std::clamp(offset_frames, -placement_limit, placement_limit)))
			                             : int64_t{0};
		auto destination_frame = next_destination_frame
			                         ? std::max(timestamp_frame, *next_destination_frame)
			                         : timestamp_frame;
		const auto output_end = destination_frame + source_frames;
		auto source_frame = int64_t{0};

		if (destination_frame < 0)
		{
			source_frame = std::min(source_frames, -destination_frame);
			destination_frame = 0;
		}

		const auto room = std::max<int64_t>(0, static_cast<int64_t>(wanted_frames) - destination_frame);
		const auto take = std::min(source_frames - source_frame, room);
		if (take > 0)
		{
			// The clamps above keep every offset inside both buffers, so it fits the iterators'
			// difference type even where that is 32 bits.
			std::copy_n(converted.begin() + static_cast<std::ptrdiff_t>(source_frame * pcm_channels),
			            static_cast<std::ptrdiff_t>(take * pcm_channels),
			            result.begin() + static_cast<std::ptrdiff_t>(destination_frame * pcm_channels));
		}

		next_destination_frame = output_end;
		if (destination_frame >= static_cast<int64_t>(wanted_frames) ||
			destination_frame + take >= static_cast<int64_t>(wanted_frames)) finished = true;
		return produced;
	};

	const auto drain_tail = [&]
	{
		while (!finished && swr)
		{
			const auto next_time = wanted_start + static_cast<double>(next_destination_frame.value_or(0)) / sample_rate;
			if (drain(nullptr, next_time) == 0) break;
		}
	};

	const auto receive = [&]
	{
		av_frame frame;
		int status = AVERROR(EAGAIN);

		while (!finished && (status = receive_audio_frame(_audio_context, frame.frm)) == 0)
		{
			if (test_audio_pcm_frame) test_audio_pcm_frame(*frame.frm);
			const auto format = frame_audio_info(*frame.frm, stream_format);
			if (!swr || format != input_format)
			{
				drain_tail();
				if (finished) break;
				swr_free(&swr);
				if (!format.channel_layout || format.sample_rate <= 0 ||
					swr_alloc_set_opts2(&swr, layout.get(), AV_SAMPLE_FMT_S16, sample_rate,
					                   format.channel_layout.get(), to_AVSampleFormat(format.sample_fmt),
					                   format.sample_rate, 0, nullptr) < 0 || !swr || swr_init(swr) < 0)
				{
					df::log(__FUNCTION__, "Cannot configure PCM resampling for decoded frame format");
					failed = finished = true;
					break;
				}
				input_format = format;
			}
			const auto pts = _pts_aud.guess(frame.frm->best_effort_timestamp, frame.frm->pts,
			                                frame.frm->pkt_dts, frame.frm->duration);
			const auto frame_time = calc_duration(pts, {_audio_base.num, _audio_base.den}, _audio_start_time);
			drain(frame.frm, frame_time);
			av_frame_unref(frame.frm);
		}
		// A damaged packet leaves its own span silent; later frames are still placed by timestamp.
		if (!finished && status != AVERROR(EAGAIN) && status != AVERROR_EOF)
		{
			df::log_once(__FUNCTION__, std::format("Cannot decode PCM samples: {}", status));
		}
	};

	while (!finished)
	{
		if (df::is_closing || abandon.is_cancelled())
		{
			return {};
		}

		const auto packet = read_packet();

		if (!packet || packet->eof)
		{
			break;
		}

		if (packet->pkt->stream_index != _audio_stream_index)
		{
			continue;
		}

		auto status = try_avcodec_send_packet(_audio_context, packet->pkt);
		if (status == AVERROR(EAGAIN))
		{
			receive();
			if (finished) break;
			status = try_avcodec_send_packet(_audio_context, packet->pkt);
		}
		if (status < 0)
		{
			df::log_once(__FUNCTION__, std::format("Cannot submit PCM packet: {}", status));
			continue;
		}

		receive();
	}

	if (!finished)
	{
		auto status = try_avcodec_send_packet(_audio_context, nullptr);
		if (status == AVERROR(EAGAIN))
		{
			receive();
			status = try_avcodec_send_packet(_audio_context, nullptr);
		}
		if (status < 0 && status != AVERROR_EOF)
		{
			df::log_once(__FUNCTION__, std::format("Cannot drain PCM decoder: {}", status));
		}
		receive();
		drain_tail();
	}

	if (failed) return {};
	return result;
}


file_load_result av_format_decoder::render_frame(const av_frame_ptr& frame_in) const
{
	file_load_result result;

	if (!_video_context || !frame_in)
	{
		return result;
	}

	if (!_scaler) _scaler = std::make_unique<av_scaler>();

	// scale_surface sizes and allocates the destination and downloads a hardware
	// frame itself, so doing either here would only duplicate the work (the readback
	// this used to perform was a full GPU->CPU frame copy that was then thrown away).
	const auto s = std::make_shared<ui::surface>();

	if (_scaler->scale_surface(frame_in, s))
	{
		result.s = s;
		result.success = !result.is_empty();
	}

	return result;
}


audio_resampler::audio_resampler(const audio_info_t& info) : _stream_info(info)
{
}

audio_resampler::~audio_resampler()
{
	if (_aud_resampler)
	{
		swr_close(_aud_resampler);
		swr_free(&_aud_resampler);
	}

	av_freep(&_out_buffer);
}

// Points `planes` at the reusable output buffer, growing it when the requested run
// of samples does not fit. Keeping one buffer for the whole session removes an
// allocate/free pair from every decoded audio frame and gives resample(), drain()
// and flush() a single, correctly sized place to convert into.
bool audio_resampler::prepare_output(uint8_t** planes, const int samples, const audio_info_t& format)
{
	const auto channels = static_cast<int>(format.channel_count());
	const auto fmt = to_AVSampleFormat(format.sample_fmt);

	if (samples <= 0 || channels <= 0 || fmt == AV_SAMPLE_FMT_NONE)
	{
		return false;
	}

	if (av_sample_fmt_is_planar(fmt) && channels > AV_NUM_DATA_POINTERS)
	{
		return false;
	}

	const auto needed = av_samples_get_buffer_size(nullptr, channels, samples, fmt, 0);

	if (needed <= 0)
	{
		return false;
	}

	if (needed > _out_buffer_size)
	{
		av_freep(&_out_buffer);
		_out_buffer = static_cast<uint8_t*>(av_malloc(needed));
		_out_buffer_size = _out_buffer ? needed : 0;
	}

	return _out_buffer &&
		av_samples_fill_arrays(planes, nullptr, _out_buffer, channels, samples, fmt, 0) >= 0;
}

void audio_resampler::flush()
{
	if (!_aud_resampler)
	{
		return;
	}

	if (swr_init(_aud_resampler) < 0)
	{
		df::log(__FUNCTION__, "Cannot reset audio resampler after discontinuity");
		swr_free(&_aud_resampler);
	}
}

void audio_resampler::drain(audio_buffer& audio_buffer, const int gen)
{
	if (!_aud_resampler)
	{
		return;
	}

	const auto dest_format = audio_buffer.format;
	const auto out_num_channels = dest_format.channel_count();
	const auto out_sample_size = dest_format.bytes_per_sample();

	if (out_num_channels == 0 || out_sample_size == 0)
	{
		return;
	}

	for (int guard = 0; guard < 8; ++guard)
	{
		const auto pending = swr_get_out_samples(_aud_resampler, 0);

		if (pending <= 0)
		{
			break;
		}

		uint8_t* planes[AV_NUM_DATA_POINTERS] = {};

		if (!prepare_output(planes, pending, dest_format))
		{
			break;
		}

		// NULL input flushes the resampler's internal buffer.
		const auto out_samples = swr_convert(_aud_resampler, planes, pending, nullptr, 0);

		if (out_samples <= 0)
		{
			break;
		}

		audio_buffer.append(planes[0], out_samples * out_num_channels * out_sample_size,
		                    audio_buffer.end_time(), gen);
	}
}


// Scales `total_samples` interleaved samples in place by `gain`, clamping to the
// format's range so a boost above 1.0 hard-limits instead of wrapping/overflowing.
static void apply_audio_gain(uint8_t* const data, const int total_samples, const AVSampleFormat fmt,
                             const double gain)
{
	switch (fmt)
	{
	case AV_SAMPLE_FMT_FLT:
		{
			auto* const p = reinterpret_cast<float*>(data);
			const auto g = static_cast<float>(gain);

			for (int i = 0; i < total_samples; ++i)
			{
				const auto v = p[i] * g;
				p[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
			}

			break;
		}
	case AV_SAMPLE_FMT_S16:
		{
			auto* const p = reinterpret_cast<int16_t*>(data);

			for (int i = 0; i < total_samples; ++i)
			{
				const auto v = static_cast<int32_t>(p[i] * gain);
				p[i] = static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
			}

			break;
		}
	case AV_SAMPLE_FMT_S32:
		{
			auto* const p = reinterpret_cast<int32_t*>(data);

			for (int i = 0; i < total_samples; ++i)
			{
				const auto v = p[i] * gain;
				p[i] = static_cast<int32_t>(v > 2147483647.0 ? 2147483647.0 : (v < -2147483648.0 ? -2147483648.0 : v));
			}

			break;
		}
	default:
		break;
	}
}

void audio_resampler::resample(const av_frame_ptr& frame, audio_buffer& audio_buffer)
{
	resample(*frame->frm, audio_buffer, frame->time, frame->gen);
}

void audio_resampler::resample(const AVFrame& frame, audio_buffer& audio_buffer, const double time, const int generation)
{
	const auto source_format = frame_audio_info(frame, _stream_info);

	const auto dest_format = audio_buffer.format;
	const auto dest_sample_fmt = to_AVSampleFormat(dest_format.sample_fmt);

	if (!_aud_resampler || source_format != _frame_info || dest_format != _output_info)
	{
		SwrContext* swr = nullptr;
		std::swap(swr, _aud_resampler);

		if (0 == swr_alloc_set_opts2(&swr,
		                             dest_format.channel_layout.get(),
		                             dest_sample_fmt,
		                             dest_format.sample_rate,
		                             source_format.channel_layout.get(),
		                             to_AVSampleFormat(source_format.sample_fmt),
		                             source_format.sample_rate,
		                             0,
		                             nullptr) && swr && swr_init(swr) == 0)
		{
			_aud_resampler = swr;
			_frame_info = source_format;
			_output_info = dest_format;
		}
		else if (swr)
		{
			// Leave the cached formats alone so a later frame retries the setup
			// rather than silently dropping every remaining sample.
			swr_free(&swr);
		}
	}

	if (!_aud_resampler)
	{
		return;
	}

	// Use the resolved source format/layout (which may fall back to the stream
	// info) so planarity and the expected plane count match what swr was set up
	// with, even when the frame left format/ch_layout unset.
	const auto is_planar = av_sample_fmt_is_planar(to_AVSampleFormat(source_format.sample_fmt));
	const int planes_expected = is_planar ? static_cast<int>(source_format.channel_count()) : 1;

	auto is_valid = frame.linesize[0] != 0 && frame.extended_data != nullptr;

	for (int i = 0; is_valid && i < planes_expected; ++i)
	{
		is_valid = frame.extended_data[i] != nullptr;
	}

	const auto expected_out_samples = swr_get_out_samples(_aud_resampler, frame.nb_samples);
	const auto out_num_channels = dest_format.channel_count();
	const auto out_sample_size = dest_format.bytes_per_sample();

	uint8_t* planes[AV_NUM_DATA_POINTERS] = {};

	if (!prepare_output(planes, expected_out_samples, dest_format))
	{
		return;
	}

	if (!is_valid)
	{
		// The frame carries no usable sample data - emit the equivalent run of
		// silence so the timeline does not jump.
		const auto silence_size = expected_out_samples * out_num_channels * out_sample_size;
		memset(planes[0], 0, silence_size);
		audio_buffer.append(planes[0], silence_size, time, generation);
		return;
	}

	const auto out_samples = swr_convert(_aud_resampler, planes, expected_out_samples,
	                                     frame.extended_data, frame.nb_samples);

	if (out_samples < 0)
	{
		df::log(__FUNCTION__, "swr_convert failed");
		return;
	}

	if (_gain != 1.0 && out_samples > 0)
	{
		apply_audio_gain(planes[0], out_samples * out_num_channels, dest_sample_fmt, _gain);
	}

	audio_buffer.append(planes[0], out_samples * out_num_channels * out_sample_size, time, generation);
}

av_scaler::~av_scaler()
{
	if (_scaler)
	{
		sws_freeContext(_scaler);
		_scaler = nullptr;
	}
}

namespace
{
	// A whole encoded image is already in memory here, so the AVIOContext reads from the span rather
	// than a file. ffmpeg still probes it, which is what settles the format.
	struct memory_source
	{
		const uint8_t* data = nullptr;
		int64_t size = 0;
		int64_t pos = 0;
	};

	int memory_read(void* opaque, uint8_t* buffer, int wanted)
	{
		auto* const source = static_cast<memory_source*>(opaque);
		const auto available = source->size - source->pos;

		if (available <= 0) return AVERROR_EOF;

		const auto count = std::min(static_cast<int64_t>(wanted), available);
		std::memcpy(buffer, source->data + source->pos, static_cast<size_t>(count));
		source->pos += count;
		return static_cast<int>(count);
	}

	int64_t memory_seek(void* opaque, const int64_t offset, const int whence)
	{
		auto* const source = static_cast<memory_source*>(opaque);

		// The probe asks for the size through this same callback.
		if (whence == AVSEEK_SIZE) return source->size;

		const auto base = whence == SEEK_CUR ? source->pos : whence == SEEK_END ? source->size : 0;
		const auto target = base + offset;

		if (target < 0 || target > source->size) return AVERROR(EINVAL);

		source->pos = target;
		return target;
	}
}

ui::surface_ptr av_decode_still(const df::cspan data, const sizei max_dim, const std::string_view extension_hint,
                                load_diagnostic* const diagnostic)
{
	if (data.data == nullptr || data.size == 0) return {};

	const auto normalized_extension_hint = normalize_still_extension_hint(extension_hint);
	const auto still_can_use_alpha = tga_declares_alpha(data, normalized_extension_hint);

	if (const auto hinted_dimensions = hinted_tga_dimensions(data, normalized_extension_hint); !hinted_dimensions.is_empty() &&
		reject_hinted_over_budget_still(diagnostic, data, normalized_extension_hint))
	{
		return {};
	}

	memory_source source{data.data, static_cast<int64_t>(data.size), 0};

	static constexpr int io_buffer_size = df::sixty_four_k;
	auto* const io_buffer = static_cast<uint8_t*>(av_mallocz(io_buffer_size + 16));
	if (!io_buffer) return {};

	auto* pb = avio_alloc_context(io_buffer, io_buffer_size, 0, &source, memory_read, nullptr, memory_seek);

	if (!pb)
	{
		av_free(io_buffer);
		return {};
	}

	auto* fc = avformat_alloc_context();

	if (!fc)
	{
		av_freep(&pb->buffer);
		avio_context_free(&pb);
		return {};
	}

	fc->pb = pb;

	// The probe reads the extension off this name. There is no file to open: pb already holds the
	// bytes, and a format with a signature is found whether or not a name is given.
	const auto probe_name = normalized_extension_hint.empty()
		                        ? std::string{}
		                        : std::format("image{}", normalized_extension_hint);

	if (avformat_open_input(&fc, probe_name.empty() ? nullptr : probe_name.c_str(), nullptr, nullptr) != 0)
	{
		// open_input frees fc itself on failure, but not the context it was given.
		av_freep(&pb->buffer);
		avio_context_free(&pb);
		if (diagnostic) reject_hinted_over_budget_still(diagnostic, data, normalized_extension_hint);
		return {};
	}

	const df::scope_exit close_input([&fc, &pb]
	{
		avformat_close_input(&fc);
		if (pb) av_freep(&pb->buffer);
		avio_context_free(&pb);
	});

	fc->max_streams = fc->nb_streams;
	const auto opts_stream_count = fc->nb_streams;
	AVDictionary** stream_opts = alloc_stream_probe_options(fc);
	const df::scope_exit free_probe_options([&stream_opts, opts_stream_count]
	{
		free_stream_probe_options(&stream_opts, opts_stream_count);
	});

	if (avformat_find_stream_info(fc, stream_opts) < 0)
	{
		for (unsigned i = 0; i < fc->nb_streams; ++i)
		{
			const auto* const stream = fc->streams[i];
			if (!stream || !stream->codecpar) continue;

			const sizei probed_dimensions{stream->codecpar->width, stream->codecpar->height};
			if (!probed_dimensions.is_empty() &&
				(reject_over_budget_source(diagnostic, probed_dimensions, "ffmpeg") ||
					reject_over_budget_codec_working(diagnostic, stream->codecpar->codec_id, probed_dimensions,
					                                 "ffmpeg")))
			{
				break;
			}
		}

		if (diagnostic && !diagnostic->over_budget)
		{
			reject_hinted_over_budget_still(diagnostic, data, normalized_extension_hint);
		}

		return {};
	}

	const auto stream_index = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (stream_index < 0)
	{
		if (diagnostic && !diagnostic->over_budget)
		{
			reject_hinted_over_budget_still(diagnostic, data, normalized_extension_hint);
		}
		return {};
	}

	const auto* const params = fc->streams[stream_index]->codecpar;

	// Every still decoder with a format of its own refuses an over-budget source by returning an
	// empty surface, and an empty surface is exactly what routes a file here - so without this gate
	// the fallback re-decodes what the budget just refused. It is also the only gate the formats
	// ffmpeg alone carries (TGA, SGI, the portable pixmaps, DPX) ever get, because scan_photo reads
	// no header for them and the caller's check is skipped when the geometry is unknown.
	const sizei source_dimensions{params->width, params->height};
	if (reject_over_budget_source(diagnostic, source_dimensions, "ffmpeg")) return {};
	if (reject_over_budget_codec_working(diagnostic, params->codec_id, source_dimensions, "ffmpeg")) return {};

	const auto* const codec = avcodec_find_decoder(params->codec_id);
	if (!codec) return {};

	auto* cc = avcodec_alloc_context3(codec);
	if (!cc) return {};

	const df::scope_exit free_codec([&cc] { avcodec_free_context(&cc); });

	if (avcodec_parameters_to_context(cc, params) < 0) return {};

	// codecpar can understate what the bitstream then asks for, so the ceiling is restated where the
	// decoder itself will enforce it.
	if (const auto max_pixels = codec_pixel_ceiling(params->codec_id); max_pixels > 0) cc->max_pixels = max_pixels;

	if (avcodec_open2(cc, codec, nullptr) != 0)
	{
		if (diagnostic && !diagnostic->over_budget)
		{
			reject_hinted_over_budget_still(diagnostic, data, normalized_extension_hint);
		}
		return {};
	}

	auto* frame = av_frame_alloc();
	auto* packet = av_packet_alloc();

	const df::scope_exit free_av([&frame, &packet]
	{
		if (frame) av_frame_free(&frame);
		if (packet) av_packet_free(&packet);
	});

	if (!frame || !packet) return {};

	// One frame is the whole image. An animation stops at its first, which is the frame the browser
	// and the still viewer both show.
	while (av_read_frame(fc, packet) >= 0)
	{
		const df::scope_exit unref([packet] { av_packet_unref(packet); });

		if (packet->stream_index != stream_index) continue;
		if (avcodec_send_packet(cc, packet) != 0) continue;

		if (avcodec_receive_frame(cc, frame) == 0)
		{
			ui::surface_ptr result;
			av_scaler scaler;

			if (scaler.scale_frame(*frame, result, max_dim, 0.0, ui::orientation::top_left, {},
			                       still_can_use_alpha))
			{
				return result;
			}

			return {};
		}
	}

	if (diagnostic && !diagnostic->over_budget)
	{
		reject_hinted_over_budget_still(diagnostic, data, normalized_extension_hint);
	}
	return {};
}

// swscale defaults to BT.601 limited range for any YUV source, which is wrong for most HD and for
// anything full range, so the signalled matrix has to be pushed into the context explicitly.
static void apply_colorspace_details(SwsContext* scaler, const ui::color_space cs)
{
	int colorspace = SWS_CS_ITU601;
	bool full_range = false;

	switch (cs)
	{
	case ui::color_space::rec709_limited: colorspace = SWS_CS_ITU709;
		break;
	case ui::color_space::rec709_full: colorspace = SWS_CS_ITU709;
		full_range = true;
		break;
	case ui::color_space::rec2020_limited: colorspace = SWS_CS_BT2020;
		break;
	case ui::color_space::rec2020_full: colorspace = SWS_CS_BT2020;
		full_range = true;
		break;
	case ui::color_space::rec601_full: full_range = true;
		break;
	case ui::color_space::rec601_limited:
	default: break;
	}

	const auto* const coefficients = sws_getCoefficients(colorspace);

	// Refused when the source is RGB and there is no matrix to set, which is not an error here.
	sws_setColorspaceDetails(scaler, coefficients, full_range, coefficients, true, 0, 1 << 16, 1 << 16);
}

// PQ and HLG carry light an SDR display cannot show. Drawn as they are, the signal is read as if it
// were SDR: reference white comes out a dull grey and the colours wash out.
static bool is_hdr(const AVFrame& frame)
{
	return frame.color_trc == AVCOL_TRC_SMPTE2084 || frame.color_trc == AVCOL_TRC_ARIB_STD_B67;
}

// What an SDR picture shows as white, in the light PQ and HLG describe: 203 nits, where BT.2408 puts
// HDR reference white.
constexpr double sdr_white_nits = 203.0;

// The brightest light a PQ picture is meant to hold: the content's own measured peak where the file
// records it, else its mastering display's, else the 1000 nits most PQ is graded to.
static double pq_source_peak_nits(const AVFrame& frame)
{
	if (const auto* const sd = av_frame_get_side_data(&frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL))
	{
		const auto* const cll = std::bit_cast<const AVContentLightMetadata*>(sd->data);
		if (cll->MaxCLL > 0) return std::clamp(static_cast<double>(cll->MaxCLL), sdr_white_nits, 10000.0);
	}

	if (const auto* const sd = av_frame_get_side_data(&frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
	{
		const auto* const mdm = std::bit_cast<const AVMasteringDisplayMetadata*>(sd->data);

		if (mdm->has_luminance && mdm->max_luminance.num > 0 && mdm->max_luminance.den > 0)
		{
			return std::clamp(av_q2d(mdm->max_luminance), sdr_white_nits, 10000.0);
		}
	}

	return 1000.0;
}

// Everything the mapping reads from a frame. HLG is rendered from its transfer and primaries alone;
// PQ adds the peak its highlights roll off from and what swscale reads of its mastering display.
// HDR10+ per-scene metadata is set aside: a cached cube can only hold the static mapping, and the CPU
// paths must show what the GPU shows.
static std::array<int, 21> tone_map_key(const AVFrame& frame)
{
	std::array<int, 21> key = {};
	key[0] = frame.color_trc;
	key[1] = frame.color_primaries;

	if (frame.color_trc != AVCOL_TRC_SMPTE2084) return key;

	key[2] = static_cast<int>(std::lround(pq_source_peak_nits(frame)));

	if (const auto* const sd = av_frame_get_side_data(&frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
	{
		const auto* const mdm = std::bit_cast<const AVMasteringDisplayMetadata*>(sd->data);

		if (mdm->has_luminance)
		{
			key[3] = 1;
			key[4] = mdm->min_luminance.num;
			key[5] = mdm->min_luminance.den;
			key[6] = mdm->max_luminance.num;
			key[7] = mdm->max_luminance.den;
		}

		if (mdm->has_primaries)
		{
			key[8] = 1;

			for (auto i = 0; i < 3; ++i)
			{
				key[9 + i * 4] = mdm->display_primaries[i][0].num;
				key[10 + i * 4] = mdm->display_primaries[i][0].den;
				key[11 + i * 4] = mdm->display_primaries[i][1].num;
				key[12 + i * 4] = mdm->display_primaries[i][1].den;
			}
		}
	}

	return key;
}

// SMPTE ST 2084: PQ signal, 0 to 1, against absolute light in nits.
namespace pq
{
	constexpr double m1 = 2610.0 / 16384.0;
	constexpr double m2 = 2523.0 / 4096.0 * 128.0;
	constexpr double c1 = 3424.0 / 4096.0;
	constexpr double c2 = 2413.0 / 4096.0 * 32.0;
	constexpr double c3 = 2392.0 / 4096.0 * 32.0;

	static double to_nits(const double signal)
	{
		const auto p = std::pow(std::clamp(signal, 0.0, 1.0), 1.0 / m2);
		return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
	}

	static double from_nits(const double nits)
	{
		const auto y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
		return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
	}
}

// The BT.2390 EETF, between PQ signals: light from a source whose peak is source_peak, onto a display
// whose peak is target_peak. Below the knee the signal passes untouched, so midtones and reference
// white keep their brightness; above it a Hermite spline rolls the highlights into the target's peak
// rather than clipping them.
static double bt2390_eetf(const double signal, const double source_peak, const double target_peak)
{
	if (source_peak <= target_peak) return std::min(signal, target_peak);

	const auto max_lum = target_peak / source_peak;
	const auto knee = 1.5 * max_lum - 0.5;
	const auto e = std::clamp(signal / source_peak, 0.0, 1.0);
	if (e < knee) return e * source_peak;

	const auto t = (e - knee) / (1.0 - knee);
	const auto t2 = t * t;
	const auto t3 = t2 * t;
	const auto rolled = (2.0 * t3 - 3.0 * t2 + 1.0) * knee + (t3 - 2.0 * t2 + t) * (1.0 - knee) +
		(-2.0 * t3 + 3.0 * t2) * max_lum;

	return rolled * source_peak;
}

static ui::tone_map_lut_ptr make_tone_map_lut(const AVFrame& frame)
{
	// 33 samples an axis is the size colour grading uses: fine enough that trilinear blending between
	// samples stays well inside one 8-bit step of the mapping it approximates.
	constexpr int n = 33;

	auto* src = av_frame_alloc();
	auto* dst = av_frame_alloc();
	SwsContext* ctx = sws_alloc_context();

	const df::scope_exit release([&src, &dst, &ctx]
	{
		av_frame_free(&src);
		av_frame_free(&dst);
		sws_free_context(&ctx);
	});

	if (!src || !dst || !ctx) return {};

	// Every R'G'B' signal the YUV matrix can produce, laid out as one picture tagged with the video's
	// own transfer and primaries.
	src->format = AV_PIX_FMT_RGB48LE;
	src->width = n * n;
	src->height = n;
	src->color_trc = frame.color_trc;
	src->color_primaries = frame.color_primaries;
	src->colorspace = AVCOL_SPC_RGB;
	src->color_range = AVCOL_RANGE_JPEG;

	const auto hlg = frame.color_trc == AVCOL_TRC_ARIB_STD_B67;

	if (hlg)
	{
		// HLG describes the scene, and a display renders it for its own peak - which swscale reads from
		// the mastering display, else the 1000-nit HLG reference display. Rendered for 1000 nits and
		// then compressed into SDR white, as PQ is, a phone's HLG lands mostly in the compressed top of
		// the range and is blown out. Named here as SDR white, the display is the one the picture is
		// going to: BT.2100's HLG EOTF at FFmpeg's system gamma below 1000 nits, 1.0, puts the signal's
		// peak on white with highlights already rolled off by HLG's own log segment, and reference
		// white at a quarter of white's light, close to how the platform's own player shows it.
		auto* const display = av_mastering_display_metadata_create_side_data(src);
		if (!display) return {};

		display->has_luminance = 1;
		display->min_luminance = av_make_q(0, 1);
		display->max_luminance = av_make_q(static_cast<int>(sdr_white_nits), 1);
	}
	else if (const auto* const sd = av_frame_get_side_data(&frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
	{
		if (auto* const copy = av_frame_new_side_data(src, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, sd->size))
		{
			memcpy(copy->data, sd->data, sd->size);
		}
	}

	if (av_frame_get_buffer(src, 0) < 0) return {};

	for (auto b = 0; b < n; ++b)
	{
		auto* const row = std::bit_cast<uint16_t*>(src->data[0] + static_cast<ptrdiff_t>(src->linesize[0]) * b);

		for (auto g = 0; g < n; ++g)
		{
			for (auto r = 0; r < n; ++r)
			{
				auto* const px = row + (g * n + r) * 3;
				px[0] = static_cast<uint16_t>(r * 65535 / (n - 1));
				px[1] = static_cast<uint16_t>(g * 65535 / (n - 1));
				px[2] = static_cast<uint16_t>(b * 65535 / (n - 1));
			}
		}
	}

	// swscale takes the signal to light: it decodes PQ, or renders HLG for the display named above, and
	// maps the wide gamut into BT.709 -- colorimetrically, so every light level stays where it was. Its
	// own perceptual tone mapping is not used: it rescales the whole PQ range, which pulls reference
	// white down to a grey darker than the untouched signal shows.
	dst->format = AV_PIX_FMT_RGB48LE;
	dst->width = src->width;
	dst->height = src->height;
	dst->color_trc = AVCOL_TRC_SMPTE2084;
	dst->color_primaries = AVCOL_PRI_BT709;
	dst->colorspace = AVCOL_SPC_RGB;
	dst->color_range = AVCOL_RANGE_JPEG;

	ctx->intent = SWS_INTENT_RELATIVE_COLORIMETRIC;

	if (sws_scale_frame(ctx, dst, src) < 0)
	{
		df::log(__FUNCTION__, "swscale could not map this HDR signal");
		return {};
	}

	const auto source_peak = hlg ? 0.0 : pq::from_nits(pq_source_peak_nits(frame));
	const auto target_peak = pq::from_nits(sdr_white_nits);

	auto lut = std::make_shared<ui::tone_map_lut>();
	lut->size = n;
	lut->rgba.resize(static_cast<size_t>(n) * n * n * 4);

	for (auto b = 0; b < n; ++b)
	{
		const auto* const row = std::bit_cast<const uint16_t*>(dst->data[0] + static_cast<ptrdiff_t>(dst->linesize[0]) * b);

		for (auto i = 0; i < n * n; ++i)
		{
			const auto* const px = row + i * 3;
			const std::array light = {pq::to_nits(px[0] / 65535.0), pq::to_nits(px[1] / 65535.0), pq::to_nits(px[2] / 65535.0)};

			// PQ is rolled off on luminance, with every channel scaled alike so a highlight keeps its hue
			// as it compresses. Luminance is a smooth function of the signal, so the cube's blending
			// between samples follows it; the brightest channel bends along the grey axis, and blended
			// there it would darken every neutral tone. HLG already peaks at white.
			const auto luminance = 0.2126 * light[0] + 0.7152 * light[1] + 0.0722 * light[2];
			const auto mapped = hlg ? luminance : pq::to_nits(bt2390_eetf(pq::from_nits(luminance), source_peak, target_peak));
			const auto gain = luminance > 0.0 ? mapped / luminance : 1.0;
			const auto white = std::min(mapped / sdr_white_nits, 1.0);

			std::array<double, 3> relative = {};
			for (auto c = 0; c < 3; ++c) relative[c] = light[c] * gain / sdr_white_nits;

			// A saturated highlight can carry one channel past white while its luminance fits. It is
			// desaturated towards that luminance until it fits, where clipping the channel would shift
			// its hue.
			const auto brightest = std::ranges::max(relative);

			if (brightest > 1.0)
			{
				const auto fit = (1.0 - white) / (brightest - white);
				for (auto& v : relative) v = white + (v - white) * fit;
			}

			auto* const out = lut->rgba.data() + (static_cast<size_t>(b) * n * n + i) * 4;

			// Encoded for BT.1886, the display an SDR video signal is made for, with 203 nits as white.
			for (auto c = 0; c < 3; ++c)
			{
				out[c] = static_cast<uint16_t>(std::lround(std::pow(std::clamp(relative[c], 0.0, 1.0), 1.0 / 2.4) * 65535.0));
			}

			out[3] = 0xffff;
		}
	}

	return lut;
}

// The mapping a clip's frames share, generated once per clip rather than per frame: swscale's colour
// management is far too costly to run that often, and a paint must never wait on it.
static ui::tone_map_lut_ptr tone_map_lut_for(const AVFrame& frame)
{
	static platform::mutex mutex;
	static std::vector<std::pair<std::array<int, 21>, ui::tone_map_lut_ptr>> cache;
	constexpr size_t max_cached = 8;

	const auto key = tone_map_key(frame);

	{
		platform::exclusive_lock lock(mutex);
		const auto found = std::ranges::find(cache, key, &std::pair<std::array<int, 21>, ui::tone_map_lut_ptr>::first);
		if (found != cache.end()) return found->second;
	}

	// Made outside the lock, so two clips opening together do not wait on each other's mapping.
	auto lut = make_tone_map_lut(frame);
	if (!lut) return {};

	platform::exclusive_lock lock(mutex);
	if (cache.size() >= max_cached) cache.erase(cache.begin());
	cache.emplace_back(key, lut);
	return lut;
}

// R'G'B' signal rows, three 16-bit samples a pixel, through the cube into packed BGRA.
static void tone_map_rows(const uint16_t* const signal, const sizei extent, const ui::tone_map_lut& lut,
                          ui::surface& surface_out)
{
	const auto row_samples = static_cast<size_t>(extent.cx) * 3;

	for (auto y = 0; y < extent.cy; ++y)
	{
		lut.apply(signal + row_samples * y, static_cast<size_t>(extent.cx),
		          std::bit_cast<ui::color32*>(surface_out.pixels_line(y)));
	}
}

uint16_t* av_scaler::hdr_signal_rows(const sizei extent)
{
	const auto samples = static_cast<size_t>(extent.cx) * 3 * extent.cy;
	if (_hdr_signal.size() < samples) _hdr_signal.resize(samples);
	return _hdr_signal.data();
}

bool av_scaler::convert_hdr(const AVFrame& frame, const ui::tone_map_lut& lut, const ui::surface_ptr& surface_out,
                            const sizei dimensions_out, const double time, const ui::orientation orientation,
                            const bool high_quality)
{
	// The YUV matrix alone, into 16-bit R'G'B' for the cube: the same two steps the renderer takes, so a
	// picture made here matches one the GPU draws.
	_scaler = sws_getCachedContext(_scaler, frame.width, frame.height, static_cast<AVPixelFormat>(frame.format),
	                               dimensions_out.cx, dimensions_out.cy, AV_PIX_FMT_RGB48LE,
	                               high_quality ? SWS_BICUBIC : SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!_scaler) return false;

	apply_colorspace_details(_scaler, av_frame_color_space(frame));

	const auto row_samples = static_cast<size_t>(dimensions_out.cx) * 3;
	auto* const signal = hdr_signal_rows(dimensions_out);
	uint8_t* dst_data[4] = {std::bit_cast<uint8_t*>(signal), nullptr, nullptr, nullptr};
	const int dst_stride[4] = {static_cast<int>(row_samples * sizeof(uint16_t)), 0, 0, 0};

	if (sws_scale(_scaler, frame.data, frame.linesize, 0, frame.height, dst_data, dst_stride) != dimensions_out.cy)
	{
		return false;
	}

	if (!surface_out->alloc(dimensions_out, ui::texture_format::RGB, orientation, time)) return false;

	tone_map_rows(signal, dimensions_out, lut, *surface_out);
	return true;
}

bool av_scaler::scale_surface(const ui::const_surface_ptr& surface_in, ui::surface_ptr& surface_out,
                              const sizei dimensions_out, const bool high_quality)
{
	const auto source_extent = surface_in->dimensions();
	const auto fmt = surface_in->format();
	const auto source_fmt = fmt == ui::texture_format::NV12
		                        ? AV_PIX_FMT_NV12
		                        : fmt == ui::texture_format::P010
		                        ? AV_PIX_FMT_P010LE
		                        : fmt == ui::texture_format::RGB || fmt == ui::texture_format::ARGB
		                        ? AV_PIX_FMT_BGRA
		                        : AV_PIX_FMT_NONE;
	if (source_fmt == AV_PIX_FMT_NONE) return false;

	// Planes with a tone map hold HDR signal, which converted directly would read as SDR. They are
	// taken through the cube first, as the renderer takes them.
	if (surface_in->tone_map() && !ui::is_packed(fmt))
	{
		const auto mapped = std::make_shared<ui::surface>();
		if (!convert_yuv_surface(*surface_in, mapped)) return false;
		return scale_surface(mapped, surface_out, dimensions_out, high_quality);
	}

	// swscale has no RGB->RGB scaler: it converts to planar YUV and back, and a BGRA source carries
	// no chroma subsampling, so libswscale forces SWS_FULL_CHR_H_INT and the scalar
	// yuv2bgra32_full_X_c output converter. Packed surfaces are answered directly instead - by the
	// area filter when this is a reduction, and bilinear when an axis grows.
	if (source_fmt == AV_PIX_FMT_BGRA)
	{
		if (ui::area_downscale(surface_in, surface_out, dimensions_out)) return true;
		if (ui::bilinear_resize(surface_in, surface_out, dimensions_out)) return true;
	}

	constexpr auto output_fmt = AV_PIX_FMT_BGRA;
	_scaler = sws_getCachedContext(_scaler, source_extent.cx, source_extent.cy, source_fmt, dimensions_out.cx,
	                               dimensions_out.cy, output_fmt,
	                               high_quality ? SWS_BICUBIC : SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

	if (_scaler)
	{
		// Without this a full-range JPEG NV12 surface would be converted as limited range here while the
		// GPU sampler converts it as full range, so the same image shifts colour between scales.
		apply_colorspace_details(_scaler, surface_in->color_space());

		surface_out = std::make_shared<ui::surface>();
		// YUV carries no alpha, so a converted frame is opaque RGB - flagging it ARGB would force the
		// software canvas down its per-pixel Porter-Duff path for every draw.
		const auto destination_format = source_fmt == AV_PIX_FMT_BGRA ? fmt : ui::texture_format::RGB;

		if (!surface_out->alloc(dimensions_out.cx, dimensions_out.cy, destination_format, surface_in->orientation()))
		{
			surface_out.reset();
			return false;
		}

		surface_out->color_space(surface_in->color_space());

		const auto stride = static_cast<int>(surface_in->stride());
		const auto* const pixels = surface_in->pixels();
		const auto is_yuv = source_fmt == AV_PIX_FMT_NV12 || source_fmt == AV_PIX_FMT_P010LE;
		const uint8_t* src_data[4] = {
			pixels,
			is_yuv ? pixels + static_cast<ptrdiff_t>(stride) * source_extent.cy : nullptr,
			nullptr,
			nullptr
		};
		const int src_stride[4] = {stride, is_yuv ? stride : 0, 0, 0};

		uint8_t* dst_data[4] = {surface_out->pixels(), nullptr, nullptr, nullptr};
		const int dst_stride[4] = {static_cast<int>(surface_out->stride()), 0, 0, 0};

		const auto scaled = sws_scale(_scaler, src_data, src_stride, 0, source_extent.cy,
		                              dst_data, dst_stride);

		return scaled == dimensions_out.cy && is_valid(surface_out);
	}

	return false;
}

bool av_scaler::convert_yuv_surface(const ui::surface& surface_in, const ui::surface_ptr& surface_out)
{
	const auto source_extent = surface_in.dimensions();
	const auto fmt = surface_in.format();
	const auto source_fmt = fmt == ui::texture_format::NV12
		                        ? AV_PIX_FMT_NV12
		                        : fmt == ui::texture_format::P010
		                        ? AV_PIX_FMT_P010LE
		                        : AV_PIX_FMT_NONE;
	if (source_fmt == AV_PIX_FMT_NONE || !surface_out) return false;

	// HDR planes are taken to 16-bit R'G'B' and mapped through their cube with the shader's own
	// arithmetic, so a backend without the shader still shows what the GPU shows.
	const auto lut = surface_in.tone_map();
	const auto render_fmt = lut ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_BGRA;

	_scaler = sws_getCachedContext(_scaler, source_extent.cx, source_extent.cy, source_fmt,
	                               source_extent.cx, source_extent.cy, render_fmt,
	                               SWS_POINT, nullptr, nullptr, nullptr);
	if (!_scaler) return false;

	apply_colorspace_details(_scaler, surface_in.color_space());

	if (!surface_out->alloc(source_extent, ui::texture_format::RGB, surface_in.orientation(), surface_in.time()))
	{
		return false;
	}
	surface_out->color_space(surface_in.color_space());

	const auto stride = static_cast<int>(surface_in.stride());
	const auto* const y_plane = surface_in.pixels();
	const uint8_t* src_data[4] = {
		y_plane, y_plane + static_cast<ptrdiff_t>(stride) * source_extent.cy,
		nullptr, nullptr
	};
	const int src_stride[4] = {stride, stride, 0, 0};

	if (!lut)
	{
		uint8_t* dst_data[4] = {surface_out->pixels(), nullptr, nullptr, nullptr};
		const int dst_stride[4] = {static_cast<int>(surface_out->stride()), 0, 0, 0};

		return sws_scale(_scaler, src_data, src_stride, 0, source_extent.cy, dst_data, dst_stride) == source_extent.cy;
	}

	const auto row_samples = static_cast<size_t>(source_extent.cx) * 3;
	auto* const signal = hdr_signal_rows(source_extent);
	uint8_t* dst_data[4] = {std::bit_cast<uint8_t*>(signal), nullptr, nullptr, nullptr};
	const int dst_stride[4] = {static_cast<int>(row_samples * sizeof(uint16_t)), 0, 0, 0};

	if (sws_scale(_scaler, src_data, src_stride, 0, source_extent.cy, dst_data, dst_stride) != source_extent.cy)
	{
		return false;
	}

	tone_map_rows(signal, source_extent, *lut, *surface_out);
	return true;
}

AVFrame* av_scaler::download(const AVFrame& frame)
{
	auto* result = av_frame_alloc();

	// av_hwframe_transfer_data moves pixels, not properties, and the matrix, range and transfer the
	// conversion needs are properties.
	if (result && av_hwframe_transfer_data(result, &frame, 0) == 0 && av_frame_copy_props(result, &frame) == 0)
	{
		return result;
	}

	// Left as a hardware frame, sws would refuse the pixel format and answer null - a black picture
	// with nothing in the log to attribute it to.
	if (!_hw_download_failure_logged)
	{
		_hw_download_failure_logged = true;
		df::log(__FUNCTION__, "could not download the hardware frame");
	}

	av_frame_free(&result);
	return nullptr;
}

bool av_scaler::convert_frame(const AVFrame& frame, const ui::color_space cs, const ui::surface_ptr& surface_out,
                              const double time, const ui::orientation orientation)
{
	// Without its cube an HDR picture is converted as SDR, as it always was, rather than not at all.
	if (is_hdr(frame))
	{
		if (const auto lut = tone_map_lut_for(frame))
		{
			return convert_hdr(frame, *lut, surface_out, {frame.width, frame.height}, time, orientation, false);
		}
	}

	const sizei src_extent = {frame.width, frame.height};
	const auto source_fmt = static_cast<AVPixelFormat>(frame.format);
	constexpr auto render_fmt = AV_PIX_FMT_BGRA;

	_scaler = sws_getCachedContext(_scaler, src_extent.cx, src_extent.cy, source_fmt,
	                               src_extent.cx, src_extent.cy, render_fmt,
	                               SWS_BILINEAR, nullptr, nullptr, nullptr);

	if (!_scaler) return false;

	apply_colorspace_details(_scaler, cs);

	if (!surface_out->alloc(src_extent, ui::texture_format::RGB, orientation, time)) return false;

	uint8_t* data[4] = {surface_out->pixels(), nullptr, nullptr, nullptr};
	const int linesize[4] = {static_cast<int>(surface_out->stride()), 0, 0, 0};

	const auto ret = sws_scale(_scaler, frame.data, frame.linesize, 0, src_extent.cy, data, linesize);

	// alloc does not zero, so a short conversion would publish uninitialised heap below the rows it
	// did convert. The sibling call sites require the full height for the same reason.
	if (ret != src_extent.cy)
	{
		df::log(__FUNCTION__, std::format("sws_scale converted {} of {} rows", ret, src_extent.cy));
		return false;
	}

	return true;
}

bool av_scaler::scale_surface(const av_frame_ptr& frame_in, const ui::surface_ptr& surface_out)
{
	return scale_surface(static_cast<const ui::video_frame&>(*frame_in), surface_out);
}

bool av_scaler::scale_surface(const ui::video_frame& source, const ui::surface_ptr& surface_out)
{
	// The application hands the renderers only its own frames, so this is one of them.
	const auto& frame_in = static_cast<const av_frame&>(source);

	// A picture the decode thread already prepared needs copying, or converting out of its planes.
	if (const auto& prepared = frame_in.surface)
	{
		if (!ui::is_packed(prepared->format())) return convert_yuv_surface(*prepared, surface_out);
		if (!surface_out->alloc(prepared->dimensions(), prepared->format(), prepared->orientation(), prepared->time()))
		{
			return false;
		}

		const auto row_bytes = std::min(surface_out->stride(), prepared->stride());

		for (auto y = 0; y < prepared->dimensions().cy; ++y)
		{
			memcpy(surface_out->pixels_line(y), prepared->pixels_line(y), row_bytes);
		}

		return true;
	}

	const AVFrame* frame = frame_in.frm;
	AVFrame* downloaded = nullptr;
	const df::scope_exit free_downloaded([&downloaded] { av_frame_free(&downloaded); });

	if (frame->hw_frames_ctx)
	{
		downloaded = download(*frame);
		if (!downloaded) return false;
		frame = downloaded;
	}

	return convert_frame(*frame, av_frame_color_space(*frame), surface_out, frame_in.time, frame_in.orientation);
}

// NV12 and P010 are one allocation: the full-height luma plane, then the half-height interleaved
// chroma plane at stride * height. These copy a decoder's 4:2:0 planes into that layout.
static ui::surface_ptr copy_to_nv12(const AVFrame& frame, const ui::color_space cs, const double time,
                                    const ui::orientation orientation)
{
	auto result = std::make_shared<ui::surface>();
	if (!result->alloc(frame.width, frame.height, ui::texture_format::NV12, orientation, time)) return {};
	result->color_space(cs);

	const auto width = static_cast<size_t>(frame.width);
	const auto stride = static_cast<ptrdiff_t>(result->stride());
	auto* const luma = result->pixels();
	auto* const chroma = luma + stride * frame.height;

	for (auto y = 0; y < frame.height; ++y)
	{
		memcpy(luma + stride * y, frame.data[0] + static_cast<ptrdiff_t>(frame.linesize[0]) * y, width);
	}

	for (auto y = 0; y < frame.height / 2; ++y)
	{
		auto* const dst = chroma + stride * y;

		if (frame.format == AV_PIX_FMT_NV12)
		{
			memcpy(dst, frame.data[1] + static_cast<ptrdiff_t>(frame.linesize[1]) * y, width);
			continue;
		}

		const auto* const u = frame.data[1] + static_cast<ptrdiff_t>(frame.linesize[1]) * y;
		const auto* const v = frame.data[2] + static_cast<ptrdiff_t>(frame.linesize[2]) * y;

		for (size_t x = 0; x < width / 2; ++x)
		{
			dst[x * 2] = u[x];
			dst[x * 2 + 1] = v[x];
		}
	}

	return result;
}

// P010 keeps its 10 bits at the top of each 16-bit sample, where a decoder's planar 10-bit output
// keeps them at the bottom.
static ui::surface_ptr copy_to_p010(const AVFrame& frame, const ui::color_space cs, const double time,
                                    const ui::orientation orientation)
{
	auto result = std::make_shared<ui::surface>();
	if (!result->alloc(frame.width, frame.height, ui::texture_format::P010, orientation, time)) return {};
	result->color_space(cs);

	const auto width = static_cast<size_t>(frame.width);
	const auto stride = static_cast<ptrdiff_t>(result->stride());
	auto* const luma = result->pixels();
	auto* const chroma = luma + stride * frame.height;
	const auto already_p010 = frame.format == AV_PIX_FMT_P010LE;

	for (auto y = 0; y < frame.height; ++y)
	{
		const auto* const src = std::bit_cast<const uint16_t*>(frame.data[0] + static_cast<ptrdiff_t>(frame.linesize[0]) * y);
		auto* const dst = std::bit_cast<uint16_t*>(luma + stride * y);

		if (already_p010) memcpy(dst, src, width * 2);
		else for (size_t x = 0; x < width; ++x) dst[x] = static_cast<uint16_t>(src[x] << 6);
	}

	for (auto y = 0; y < frame.height / 2; ++y)
	{
		auto* const dst = std::bit_cast<uint16_t*>(chroma + stride * y);

		if (already_p010)
		{
			memcpy(dst, frame.data[1] + static_cast<ptrdiff_t>(frame.linesize[1]) * y, width * 2);
			continue;
		}

		const auto* const u = std::bit_cast<const uint16_t*>(frame.data[1] + static_cast<ptrdiff_t>(frame.linesize[1]) * y);
		const auto* const v = std::bit_cast<const uint16_t*>(frame.data[2] + static_cast<ptrdiff_t>(frame.linesize[2]) * y);

		for (size_t x = 0; x < width / 2; ++x)
		{
			dst[x * 2] = static_cast<uint16_t>(u[x] << 6);
			dst[x * 2 + 1] = static_cast<uint16_t>(v[x] << 6);
		}
	}

	return result;
}

ui::surface_ptr av_scaler::presentable_surface(const AVFrame& frame_in, const double time,
                                               const ui::orientation orientation)
{
	const AVFrame* frame = &frame_in;
	AVFrame* downloaded = nullptr;
	const df::scope_exit free_downloaded([&downloaded] { av_frame_free(&downloaded); });

	if (frame->hw_frames_ctx)
	{
		downloaded = download(*frame);
		if (!downloaded) return {};
		frame = downloaded;
	}

	const auto cs = av_frame_color_space(*frame);
	const auto format = static_cast<AVPixelFormat>(frame->format);

	// The YUV textures are 4:2:0 at an even size: D3D11 refuses NV12 and P010 any other way.
	const auto even = frame->width > 0 && frame->height > 0 && frame->width % 2 == 0 && frame->height % 2 == 0;

	if (ui::yuv_textures_enabled && even)
	{
		// HDR planes go up only as P010 with the cube that maps them to SDR. Without a cube, for the
		// rare 8-bit HDR stream, or on a device that samples no P010, the picture is tone mapped here,
		// into BGRA, instead.
		const auto hdr = is_hdr(*frame);
		ui::surface_ptr planes;

		if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_NV12)
		{
			if (!hdr) planes = copy_to_nv12(*frame, cs, time, orientation);
		}
		else if ((format == AV_PIX_FMT_YUV420P10LE || format == AV_PIX_FMT_P010LE) && ui::p010_textures_enabled)
		{
			const auto lut = hdr ? tone_map_lut_for(*frame) : ui::tone_map_lut_ptr{};

			if (!hdr || lut)
			{
				planes = copy_to_p010(*frame, cs, time, orientation);
				if (planes) planes->tone_map(lut);
			}
		}

		if (planes) return planes;
	}

	auto result = std::make_shared<ui::surface>();
	return convert_frame(*frame, cs, result, time, orientation) ? result : nullptr;
}

bool av_scaler::scale_frame(const AVFrame& frame, ui::surface_ptr& surface, const sizei max_dim, const double time,
                            const ui::orientation orientation, const av_rational container_sar,
                            const bool preserve_alpha)
{
	bool success = false;
	const auto fmt = static_cast<AVPixelFormat>(frame.format);
	const sizei src_dims(frame.width, frame.height);

	// Correct for the pixel (sample) aspect ratio so anamorphic / non-square-pixel
	// video is scaled to its display shape rather than the stored frame shape (#78).
	auto disp_dims = src_dims;
	auto sar = frame.sample_aspect_ratio;

	// A pasp box in an MP4 reaches AVStream::sample_aspect_ratio only, so a file whose bitstream
	// VUI carries no aspect ratio hands the decoder a square-pixel frame the container contradicts.
	if (sar.num == 0 || sar.den == 0 || sar.num == sar.den)
	{
		sar = {container_sar.num, container_sar.den};
	}

	if (sar.num > 0 && sar.den > 0 && sar.num != sar.den && frame.width > 0 && frame.height > 0)
	{
		const auto w = static_cast<int64_t>(frame.width);
		disp_dims.cy = static_cast<int>(df::mul_div(
			w, static_cast<int64_t>(sar.den) * frame.height, static_cast<int64_t>(sar.num) * w));
	}

	const auto dst_dims = ui::scale_dimensions(disp_dims, max_dim);

	if (is_hdr(frame))
	{
		if (const auto lut = tone_map_lut_for(frame))
		{
			surface = std::make_shared<ui::surface>();
			if (convert_hdr(frame, *lut, surface, dst_dims, time, orientation, true)) return true;
			surface.reset();
			return false;
		}
	}

	_scaler = sws_getCachedContext(_scaler, src_dims.cx, src_dims.cy, fmt, dst_dims.cx, dst_dims.cy,
	                               AV_PIX_FMT_BGRA, SWS_BICUBIC, nullptr, nullptr, nullptr);

	if (_scaler)
	{
		apply_colorspace_details(_scaler, av_frame_color_space(frame));

		surface = std::make_shared<ui::surface>();

		const auto destination_format = preserve_alpha && av_frame_uses_alpha(frame)
			                                ? ui::texture_format::ARGB
			                                : ui::texture_format::RGB;

		if (!surface->alloc(dst_dims.cx, dst_dims.cy, destination_format, orientation, time))
		{
			surface.reset();
			return false;
		}

		uint8_t* data[4] = {(surface->pixels()), nullptr, nullptr, nullptr};
		const int linesize[4] = {static_cast<int>(surface->stride()), 0, 0, 0};

		success = sws_scale(_scaler, frame.data, frame.linesize, 0, src_dims.cy, data, linesize) == dst_dims.cy;

		if (!success)
		{
			df::log(__FUNCTION__, "sws_scale failed");
		}
	}

	return success;
}

void av_session::process_io(const platform::thread_event& video_event, const platform::thread_event& audio_event)
{
	// The open-time snapshots, not the decoder: these are read before the lock is taken.
	const auto has_audio = _has_audio.load();
	const auto has_video = _has_video.load();
	const auto video_stream = _video_stream_id.load();
	const auto audio_stream = _audio_stream_id.load();
	auto loop_iteration = 0;

	while ((has_audio && _audio_packets.should_receive()) || (has_video && _video_packets.should_receive()))
	{
		// The condition above is an OR, so a queue that never drains keeps the loop alive.
		// The ceiling is what stops the other stream buffering the rest of the file.
		if ((has_video && _video_packets.is_full()) || (has_audio && _audio_packets.is_full()))
		{
			break;
		}

		platform::shared_lock lock(_decoder_rw);
		const auto packet = _decoder.read_packet();

		if (packet)
		{
			if (packet->eof)
			{
				// Push a stream-tagged EOF marker to each queue so receive_frames can
				// flush (drain) the matching decoder's buffered tail before signalling
				// end of stream.
				if (has_video)
				{
					auto vp = std::make_shared<av_packet>();
					vp->eof = true;
					vp->seek_ver = _seek_gen;
					vp->pkt->stream_index = video_stream;
					_video_packets.push(vp);
				}

				if (has_audio)
				{
					auto ap = std::make_shared<av_packet>();
					ap->eof = true;
					ap->seek_ver = _seek_gen;
					ap->pkt->stream_index = audio_stream;
					_audio_packets.push(ap);
				}

				audio_event.set();
				video_event.set();
				break;
			}
			if (has_video && packet->pkt->stream_index == video_stream)
			{
				packet->seek_ver = _seek_gen;
				_video_packets.push(packet);
				video_event.set();
			}
			else if (has_audio && packet->pkt->stream_index == audio_stream)
			{
				packet->seek_ver = _seek_gen;
				_audio_packets.push(packet);
				audio_event.set();
			}
		}
		else
		{
			break;
		}

		if (_state == av_play_state::closed || df::is_closing || ++loop_iteration > max_loop_iteration)
		{
			break;
		}
	}
}


void av_format_decoder::receive_available_frames(AVCodecContext* const ctx, av_pts_correction& pts,
                                                 const av_rational base, const int64_t start, const int seek_gen,
                                                 av_frame_queue& frames)
{
	const AVRational time_base{base.num, base.den};
	av_frame_ptr frame;

	for (;;)
	{
		if (!frame) frame = std::make_shared<av_frame>();

		if (avcodec_receive_frame(ctx, frame->frm) != 0)
		{
			break;
		}

		update_orientation(frame->frm);

		frame->gen = seek_gen;
		frame->time = calc_duration(pts.guess(frame->frm->best_effort_timestamp, frame->frm->pts, frame->frm->pkt_dts,
		                                      frame->frm->duration),
		                            time_base, start);
		frame->orientation = calc_orientation();

		if (ctx == _video_context) prepare_for_presentation(*frame);

		frames.push(std::move(frame));
	}
}

// Set once a renderer has found it cannot share the decoder's hardware surfaces. The driver does not
// change its mind within a run, so neither does this.
static std::atomic<bool> g_cpu_video_frames{false};

void av_request_cpu_video_frames()
{
	if (!g_cpu_video_frames.exchange(true))
	{
		df::log(__FUNCTION__, "hardware frames will be downloaded on the decode thread");
	}
}

ui::const_surface_ptr av_frame_surface(const av_frame_ptr& f)
{
	return f ? f->surface : nullptr;
}

ui::video_frame_ptr av_video_frame(const av_frame_ptr& f)
{
	return f;
}

void av_format_decoder::prepare_for_presentation(av_frame& frame)
{
	if (frame.frm->hw_frames_ctx && !g_cpu_video_frames)
	{
		// Shared with the renderer as it is, so only its tone mapping is made here.
		if (is_hdr(*frame.frm)) frame.tone_map = tone_map_lut_for(*frame.frm);
		return;
	}

	if (!_presentation_scaler) _presentation_scaler = std::make_unique<av_scaler>();

	auto surface = _presentation_scaler->presentable_surface(*frame.frm, frame.time, frame.orientation);
	if (!surface) return;

	frame.surface = std::move(surface);
	av_frame_unref(frame.frm);
}

void av_format_decoder::receive_frames(av_packet_queue& packets, av_frame_queue& frames)
{
	av_packet_ptr packet;

	if (!packets.pop(packet))
	{
		return;
	}

	AVCodecContext* c = nullptr;
	av_pts_correction* pts = nullptr;
	av_rational base;
	int64_t start = AV_NOPTS_VALUE;
	const auto si = packet->pkt->stream_index;
	const auto* stream_name = "unknown stream";

	if (si == _video_stream_index)
	{
		c = _video_context;
		pts = &_pts_vid;
		base = _video_base;
		start = _video_start_time;
		stream_name = "video stream";
	}
	else if (si == _audio_stream_index)
	{
		c = _audio_context;
		pts = &_pts_aud;
		base = _audio_base;
		start = _audio_start_time;
		stream_name = "audio stream";
	}

	const auto seek_gen = packet->seek_ver;

	if (packet->eof)
	{
		if (c)
		{
			// Drain the decoder so frames it still holds (codecs such as AAC delay
			// output) are emitted rather than dropped - that lost tail is what cut the
			// sound short at the end - then push an EOF marker for the output path to
			// follow with silence.
			avcodec_send_packet(c, nullptr);
			receive_available_frames(c, *pts, base, start, seek_gen, frames);
			avcodec_flush_buffers(c); // reset for a later seek / replay
		}

		auto eof_frame = std::make_shared<av_frame>();
		eof_frame->eof = true;
		eof_frame->gen = seek_gen;
		frames.push(std::move(eof_frame));
		return;
	}

	if (!c)
	{
		return;
	}

	if (packet->is_empty())
	{
		// A seek queues an empty packet as a flush marker: drop the frames decoded
		// for the old position and reset the decoder and timestamp estimator.
		frames.clear();
		pts->clear();
		avcodec_flush_buffers(c);
		df::trace(std::format("av_format_decoder::receive_frames avcodec_flush_buffers {}", stream_name));
		return;
	}

	// The decoder refuses a new packet with EAGAIN while it still has output
	// buffered, so alternate sending and draining until it takes the packet. The
	// iteration cap keeps a misbehaving decoder from spinning this thread forever.
	for (int attempt = 0; attempt < 64; ++attempt)
	{
		const auto send_res = try_avcodec_send_packet(c, packet->pkt);

		if (send_res != 0 && send_res != AVERROR(EAGAIN))
		{
			break;
		}

		receive_available_frames(c, *pts, base, start, seek_gen, frames);

		if (send_res == 0)
		{
			break;
		}
	}
}

void av_session::state(const av_play_state new_state)
{
	if (_state.exchange(new_state) != new_state)
	{
		_host.invalidate_view(view_invalid::view_layout |
			view_invalid::screen_saver |
			view_invalid::app_layout |
			view_invalid::media_elements |
			view_invalid::command_state);
	}
}

void av_session::seek(const double pos, const bool scrubbing, const bool force)
{
	const auto was_scrubbing = _scrubbing.exchange(scrubbing);

	if (should_coalesce_seek_request(_last_seek, pos, _pending_time_sync, was_scrubbing, scrubbing, force))
	{
		// Merged: the decoder walks on from where the earlier request put it, and the presenter
		// settles onto this position instead of that one. Dropping the request outright left a
		// stopped trim handle's preview a frame or two away from the handle.
		_last_seek = pos;
		return;
	}

	{
		platform::shared_lock lock(_decoder_rw);

		if (_decoder.seek(pos))
		{
			_video_packets.clear();
			_audio_packets.clear();
			_video_frames.clear();
			_audio_frames.clear();

			if (_decoder.has_video())
			{
				auto packet = std::make_shared<av_packet>();
				packet->pkt->stream_index = _decoder._video_stream_index;
				_video_packets.push(packet);
			}

			if (_decoder.has_audio())
			{
				auto packet = std::make_shared<av_packet>();
				packet->pkt->stream_index = _decoder._audio_stream_index;
				_audio_packets.push(packet);
			}

			_seek_gen += 1;
			// Published before _pending_time_sync, which is what tells pos() to use it.
			_last_seek = pos;
			_pending_time_sync = true;
			_reset_time_offset = !_decoder.has_audio(); // && !scrubbing;
			_settling = true;
			_audio_eof_handled = false;
			_video_eof_handled = false;
		}
	}
}

uint32_t audio_info_t::bytes_per_second() const
{
	return bytes_per_sample() * channel_count() * sample_rate;
}

uint32_t audio_info_t::channel_count() const
{
	if (!channel_layout) return 0;
	return channel_layout->nb_channels;
}

uint32_t audio_info_t::bytes_per_sample() const
{
	return av_get_bytes_per_sample(to_AVSampleFormat(sample_fmt));
}

bool operator==(const audio_info_t& lhs, const audio_info_t& rhs)
{
	if (lhs.sample_rate != rhs.sample_rate ||
		lhs.sample_fmt != rhs.sample_fmt)
	{
		return false;
	}

	if (lhs.channel_layout == nullptr && rhs.channel_layout == nullptr)
	{
		return true;
	}

	if (lhs.channel_layout == nullptr || rhs.channel_layout == nullptr)
	{
		return false;
	}

	return av_channel_layout_compare(lhs.channel_layout.get(), rhs.channel_layout.get()) == 0;
}
