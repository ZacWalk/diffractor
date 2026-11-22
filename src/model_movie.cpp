// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Implements the Movie document -- timeline edits and undo, crossfade and duration arithmetic,
// when each caption shows, which file each item's sound comes from and how loud it plays, the derived
// output geometry, the decision entering Movie makes about the timeline it holds, and the .dfv project
// file. Pure: no I/O, no decoding, no UI. docs/movie.md owns the behaviour.

#include "pch.h"
#include "model_movie.h"

#include "files.h"

static bool is_qualified_path(const std::string_view s)
{
	if constexpr (!core::windows_path_semantics)
	{
		return !s.empty() && core::is_path_sep(s[0]);
	}
	else
	{
		if (s.size() < 3) return false;
		if (core::is_path_sep(s[0]) && core::is_path_sep(s[1])) return true;
		return s[1] == ':' && core::is_path_sep(s[2]);
	}
}

//
// Text and sound
//

std::string bound_movie_text(const std::string_view text)
{
	if (text.size() <= movie_max_text_bytes) return std::string(text);

	// Backed off to the start of a character, so a cut never leaves half of one.
	auto end = movie_max_text_bytes;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) --end;
	return std::string(text.substr(0, end));
}

double movie_caption_seconds(const std::string_view text)
{
	// Characters, not bytes: a caption in Czech reads no slower for its accents. A line break is read as
	// the space it stands for.
	size_t characters = 0;

	for (const auto c : text)
	{
		const auto byte = static_cast<unsigned char>(c);
		if ((byte & 0xC0u) != 0x80u && (byte >= 0x20u || c == '\n')) ++characters;
	}

	return std::clamp(movie_caption_notice_seconds + characters / movie_caption_characters_per_second,
	                  movie_caption_min_seconds, movie_caption_max_seconds);
}

bool movie_sound_is_quiet(const std::span<const int16_t> samples)
{
	if (samples.empty()) return true;

	auto sum = 0.0;

	for (const auto sample : samples)
	{
		const auto level = sample / 32768.0;
		sum += level * level;
	}

	return std::sqrt(sum / static_cast<double>(samples.size())) <= movie_quiet_level;
}

//
// Timing
//

double movie_overlap(const movie_clip& left, const movie_clip& right)
{
	return std::max(0.0, std::min({movie_crossfade_seconds, left.duration() / 2, right.duration() / 2}));
}

movie_timing calc_movie_timing(const std::vector<movie_clip>& clips, const movie_kind kind)
{
	movie_timing result;
	result.starts.reserve(clips.size());

	double t = 0;

	for (size_t i = 0; i < clips.size(); ++i)
	{
		result.starts.emplace_back(t);
		t += clips[i].duration();
		if (i + 1 < clips.size()) t -= movie_overlap(clips[i], clips[i + 1]);
	}

	result.duration = std::max(0.0, t);
	result.whole = result.duration;

	// A Short of one tune that repeats ends where it starts again, unfaded, so YouTube's loop joins it seamlessly;
	// any other ends by three minutes, fading out there as a movie fades out at its end.
	if (movie_loops_seamlessly(clips, kind))
	{
		result.duration = clips.front().loop_end - clips.front().start;
		result.fades = false;
	}
	else if (kind == movie_kind::youtube_short && result.duration > movie_short_max_seconds)
	{
		result.duration = movie_short_max_seconds;
		result.cut = true;
	}

	// A caption goes no further than the next text screen, which is a page of its own; it may run on
	// over clips. Walked backwards, so each item knows where the next screen begins.
	std::vector<double> next_screen(clips.size(), result.duration);

	for (auto i = clips.size(); i-- > 1;)
	{
		next_screen[i - 1] = clips[i].is_text ? result.starts[i] : next_screen[i];
	}

	for (size_t i = 0; i < clips.size(); ++i)
	{
		const auto& clip = clips[i];
		if (clip.is_text || clip.text.empty()) continue;

		// Once the crossfade bringing its clip in has finished, so the words arrive with their picture
		// rather than over the one before it.
		const auto in = i > 0 ? movie_overlap(clips[i - 1], clip) : 0.0;
		const auto start = std::min(result.starts[i] + in, result.duration);
		const auto end = std::min({start + movie_caption_seconds(clip.text), next_screen[i], result.duration});
		if (end > start) result.captions.push_back({static_cast<int>(i), start, end});
	}

	// Later text replaces earlier: a caption still showing when the next one begins ends there, so
	// two never sit on top of each other.
	std::ranges::stable_sort(result.captions,
	                         [](const movie_text_span& l, const movie_text_span& r) { return l.start < r.start; });

	for (size_t k = 0; k + 1 < result.captions.size(); ++k)
	{
		result.captions[k].end = std::min(result.captions[k].end, result.captions[k + 1].start);
	}

	std::erase_if(result.captions, [](const movie_text_span& span) { return !span.is_shown(); });
	return result;
}

bool movie_loops_seamlessly(const std::vector<movie_clip>& clips, const movie_kind kind)
{
	if (kind != movie_kind::youtube_short || clips.size() != 1) return false;

	// From the tune's start to at least where it starts again, played as itself rather than under a sound file.
	const auto& c = clips.front();
	return c.is_audio && !c.is_missing && !c.has_sound_file() && c.loop_end > 0 &&
	       c.loop_end <= movie_short_max_seconds && c.start <= 0.0005 && c.end >= c.loop_end - 0.0005;
}

movie_frame calc_movie_frame(const std::vector<movie_clip>& clips, const double time)
{
	return calc_movie_frame(clips, calc_movie_timing(clips), time);
}

// How far a caption has faded in at `at`: up over the start of its span and down over its end.
static double text_opacity(const movie_text_span& span, const double at)
{
	if (!span.is_shown() || at < span.start || at >= span.end) return 0;

	const auto fade = std::min(movie_text_fade_seconds, (span.end - span.start) / 3);
	if (fade <= 0) return 1;
	return std::clamp(std::min(at - span.start, span.end - at) / fade, 0.0, 1.0);
}

movie_frame calc_movie_frame(const std::vector<movie_clip>& clips, const movie_timing& timing, const double time)
{
	movie_frame result;
	if (clips.empty() || timing.starts.size() != clips.size()) return result;

	const auto at = std::clamp(time, 0.0, timing.duration);

	const auto contribute = [&](const size_t i, const double weight)
	{
		const auto& clip = clips[i];
		movie_frame_source source;
		source.index = static_cast<int>(i);
		source.weight = weight;
		source.sound = weight;
		// A photo and a text screen have one picture however long they show, so asking for a position in
		// them is meaningless; the compositor makes it once and reuses it.
		const auto into = std::clamp(at - timing.starts[i], 0.0, clip.duration());
		source.source_time = clip.is_photo || clip.is_text ? 0.0 : clip.start + into;
		// A sound file plays from its own start as the clip begins, whatever the clip's in point.
		source.sound_time = clip.has_sound_file() ? into : source.source_time;

		(result.a.index < 0 ? result.a : result.b) = source;
	};

	for (size_t i = 0; i < clips.size() && result.b.index < 0; ++i)
	{
		const auto start = timing.starts[i];
		const auto end = start + clips[i].duration();

		// The last item includes its end, so the final instant of the movie still draws something.
		const auto ends_movie = i + 1 == clips.size();
		const auto inside = at >= start && (at < end || (ends_movie && at <= end));
		if (!inside) continue;

		if (result.a.index < 0)
		{
			contribute(i, 1.0);
			continue;
		}

		const auto overlap = movie_overlap(clips[i - 1], clips[i]);
		if (overlap <= 0) break;

		const auto progress = std::clamp((at - start) / overlap, 0.0, 1.0);
		result.a.weight = 1.0 - progress;
		result.a.sound = result.a.weight;
		contribute(i, progress);
	}

	// A clip going out under an item with no sound of its own keeps its sound at full volume, so words
	// said as the crossfade begins are heard to the end of the clip, which lets go over its last moment
	// rather than stopping dead.
	if (result.b.index >= 0 && clips[result.b.index].has_no_sound())
	{
		const auto ends = timing.starts[result.a.index] + clips[result.a.index].duration();
		result.a.sound = std::clamp((ends - at) / movie_sound_release_seconds, 0.0, 1.0);
	}

	// The movie fades in from black and out to black over the crossfade's length, picture and sound - unless it
	// is a Short that loops seamlessly, whose last frame leads straight back into its first.
	const auto fade = timing.fades ? std::min(movie_crossfade_seconds, timing.duration / 2) : 0.0;

	if (fade > 0)
	{
		if (at < fade) result.fade_to_black = 1.0 - at / fade;
		if (at > timing.duration - fade)
		{
			result.fade_to_black = std::max(result.fade_to_black, 1.0 - (timing.duration - at) / fade);
		}
	}

	// The caption showing now is the last to have begun, while it has not ended.
	const auto after = std::upper_bound(timing.captions.begin(), timing.captions.end(), at,
	                                    [](const double t, const movie_text_span& span) { return t < span.start; });

	if (after != timing.captions.begin())
	{
		const auto& span = *(after - 1);
		if (const auto opacity = text_opacity(span, at); opacity > 0) result.caption = {span.index, opacity};
	}

	return result;
}

double movie_sound_level(const movie_frame& frame, const int index)
{
	if (index < 0) return 0;

	const auto level = frame.a.index == index ? frame.a.sound : frame.b.index == index ? frame.b.sound : 0.0;
	return level * (1.0 - frame.fade_to_black);
}

movie_sound movie_clip_sound(const movie_clip& clip)
{
	// Heard for as long as the clip shows: a sound file shorter than that falls silent, and a longer one is cut.
	if (clip.has_sound_file()) return {clip.sound_file, 0, 0.0, clip.duration()};
	if (clip.is_photo || clip.is_text) return {};
	return {clip.path, clip.song, clip.start, clip.end};
}

//
// Output geometry
//

static sizei fit_within(const sizei extent, const int max_cx, const int max_cy)
{
	if (extent.cx <= 0 || extent.cy <= 0) return {};

	const auto scale = std::min({
		1.0,
		static_cast<double>(max_cx) / extent.cx,
		static_cast<double>(max_cy) / extent.cy,
	});

	const auto cx = std::max(2, static_cast<int>(std::lround(extent.cx * scale)));
	const auto cy = std::max(2, static_cast<int>(std::lround(extent.cy * scale)));

	// Every encoder in reach refuses an odd dimension, so rounding down is the only safe direction.
	return {cx - (cx & 1), cy - (cy & 1)};
}

// 1920 x 1080, 1280 x 720 and 3840 x 2160 are one shape; 4:3, 3:2 and a portrait frame are others.
static bool is_same_shape(const sizei l, const sizei r)
{
	const auto l_shape = static_cast<double>(l.cx) / l.cy;
	const auto r_shape = static_cast<double>(r.cx) / r.cy;
	return std::abs(l_shape - r_shape) <= 0.03 * std::max(l_shape, r_shape);
}

static int64_t area_of(const sizei extent)
{
	return static_cast<int64_t>(extent.cx) * extent.cy;
}

// How unevenly footage filmed at `rate` plays in a movie made at `movie_rate`: not at all when one is a
// whole multiple of the other, as 60 and 15 are of 30, and the more the further it is from that.
static double movie_rate_mismatch(const double rate, const int movie_rate)
{
	const auto dropping = rate / std::max(1.0, std::round(rate / movie_rate));
	const auto repeating = movie_rate / std::max(1.0, std::round(movie_rate / rate));
	return std::min(std::abs(dropping - movie_rate) / movie_rate, std::abs(repeating - rate) / rate);
}

// Where in movie_frame_rates the rate footage at `rate` plays best at is; the first when two do as well.
static size_t best_movie_rate(const double rate)
{
	size_t best = 0;

	for (size_t i = 1; i < movie_frame_rates.size(); ++i)
	{
		if (movie_rate_mismatch(rate, movie_frame_rates[i]) < movie_rate_mismatch(rate, movie_frame_rates[best]) - 1e-9)
		{
			best = i;
		}
	}

	return best;
}

// Where in movie_short_frame_rates footage at `rate` plays best: with nothing repeated or dropped unevenly, and of
// the rates that manage that, the nearest its own - 30 fps footage at 30 rather than each frame twice at 60.
static size_t best_short_rate(const double rate)
{
	size_t best = 0;

	for (size_t i = 1; i < movie_short_frame_rates.size(); ++i)
	{
		const auto mismatch = movie_rate_mismatch(rate, movie_short_frame_rates[i]);
		const auto best_mismatch = movie_rate_mismatch(rate, movie_short_frame_rates[best]);
		const auto nearer = std::abs(movie_short_frame_rates[i] - rate) <
		                    std::abs(movie_short_frame_rates[best] - rate);

		if (mismatch < best_mismatch - 1e-9 || (std::abs(mismatch - best_mismatch) <= 1e-9 && nearer)) best = i;
	}

	return best;
}

static int movie_video_bitrate(const sizei extent, const int frame_rate)
{
	const auto bits = static_cast<int64_t>(extent.cx) * extent.cy * frame_rate * movie_bits_per_pixel;
	return static_cast<int>(
		std::clamp(bits, static_cast<double>(movie_min_bitrate), static_cast<double>(movie_max_bitrate)));
}

// A Short is upright HD, at the rate most of what moves in it was made for, by how long each shows; one of photos
// and text screens alone at a steady 30.
static movie_output derive_short_output(const std::vector<movie_clip>& clips)
{
	movie_output result;
	result.extent = movie_short_extent;

	std::array<double, movie_short_frame_rates.size()> rate_seconds{};
	auto moving = false;

	for (const auto& c : clips)
	{
		if (c.is_text || c.is_photo || c.frame_rate <= 0) continue;
		rate_seconds[best_short_rate(c.frame_rate)] += c.duration();
		moving = true;
	}

	result.frame_rate = movie_short_still_frame_rate;

	if (moving)
	{
		const auto most = std::ranges::max_element(rate_seconds) - rate_seconds.begin();
		result.frame_rate = movie_short_frame_rates[static_cast<size_t>(most)];
	}

	result.video_bitrate = movie_video_bitrate(result.extent, result.frame_rate);
	return result;
}

movie_output derive_movie_output(const std::vector<movie_clip>& clips, const movie_kind kind)
{
	if (kind == movie_kind::youtube_short) return derive_short_output(clips);

	movie_output result;

	// Videos decide the frame, and only fall back to photos when there is no video at all. A
	// 45-megapixel photo beside a 1080p clip must not produce an 8000-pixel movie. A text screen has no
	// size of its own: it is drawn on whatever frame the clips make.
	const auto have_video = std::ranges::any_of(clips, [](const movie_clip& c)
	{ return !c.is_text && !c.is_photo && c.extent.cx > 0 && c.extent.cy > 0; });

	// The frame takes the shape most of the movie is in, by how long each clip shows, so one 4:3 clip or
	// one held upright does not put black bars round every other; and the size of the largest clip of that
	// shape.
	struct shape_time
	{
		const movie_clip* largest = nullptr;
		double seconds = 0;
	};

	std::vector<shape_time> shapes;
	// Seconds of video filmed for each of movie_frame_rates.
	std::array<double, movie_frame_rates.size()> rate_seconds{};

	for (const auto& c : clips)
	{
		if (c.is_text || c.extent.cx <= 0 || c.extent.cy <= 0) continue;
		// An audio clip's pictures count as a video drawn at a steady 30, whatever they could move at in a Short.
		const auto rate = c.is_audio ? movie_audio_frame_rate : c.frame_rate;
		if (!c.is_photo && rate > 0) rate_seconds[best_movie_rate(rate)] += c.duration();
		if (have_video && c.is_photo) continue;

		auto shape = std::ranges::find_if(shapes, [&c](const shape_time& s)
		{ return is_same_shape(s.largest->extent, c.extent); });

		if (shape == shapes.end()) shape = shapes.insert(shapes.end(), shape_time{&c, 0.0});
		if (area_of(c.extent) > area_of(shape->largest->extent)) shape->largest = &c;
		shape->seconds += c.duration();
	}

	// The first shape shown wins a tie.
	const movie_clip* largest = nullptr;
	auto most_seconds = -1.0;

	for (const auto& shape : shapes)
	{
		if (shape.seconds > most_seconds)
		{
			largest = shape.largest;
			most_seconds = shape.seconds;
		}
	}

	// HD at most, in the frame's own orientation, so a portrait movie is as sharp as a landscape one.
	if (largest)
	{
		const auto portrait = largest->extent.cy > largest->extent.cx;
		result.extent = portrait ? fit_within(largest->extent, movie_max_short_side, movie_max_long_side)
		                         : fit_within(largest->extent, movie_max_long_side, movie_max_short_side);
	}

	if (result.extent.cx <= 0 || result.extent.cy <= 0) result.extent = {1920, 1080};

	// The rate most of the video was filmed for; a movie with none is made at the first.
	const auto most_filmed = std::ranges::max_element(rate_seconds) - rate_seconds.begin();
	result.frame_rate = movie_frame_rates[static_cast<size_t>(most_filmed)];
	result.video_bitrate = movie_video_bitrate(result.extent, result.frame_rate);

	return result;
}

bool is_movie_source(const file_type* const type)
{
	return type &&
	       (type->group == file_group::photo || type->group == file_group::video || type->group == file_group::audio);
}

double movie_source_frame_rate(const double declared, const double average)
{
	// A declared rate within half again of the average is the same rate give or take a dropped frame, as
	// 59.94 declared over 60 averaged is; one twice the average or more is a variable-rate stream's base.
	if (average > 0 && declared > average * 1.5) return average;
	return declared > 0 ? declared : std::max(0.0, average);
}

movie_clip make_movie_clip(core::file_path path)
{
	movie_clip result;
	result.path = path;

	const auto mt = files::file_type_from_name(path);
	result.is_photo = mt && mt->group == file_group::photo;
	result.is_audio = mt && mt->group == file_group::audio;
	if (result.is_photo) result.end = movie_photo_seconds;

	return result;
}

static double bound_hold(const double seconds, const double fallback)
{
	return std::clamp(std::isfinite(seconds) ? seconds : fallback, movie_min_hold_seconds, movie_max_hold_seconds);
}

movie_clip make_movie_text(const std::string_view text, const double seconds)
{
	movie_clip result;
	result.is_text = true;
	result.is_missing = false;
	result.is_probed = true;
	result.text = bound_movie_text(text);
	result.end = bound_hold(seconds, movie_screen_seconds);
	return result;
}

bool movie_output_names_a_clip(const std::vector<movie_clip>& clips, const core::file_path output)
{
	if (output.is_empty()) return false;

	const auto names = [output](const core::file_path path)
	{
		return !path.is_empty() && core::compare_path_key(path.pack(), output.pack()) == 0;
	};

	return std::ranges::any_of(clips,
	                           [&names](const movie_clip& clip) { return names(clip.path) || names(clip.sound_file); });
}

//
// The project file
//
// One item per line, in the order they play, so a person can read it and an agent can write it:
//
//   t <seconds> '<text>'                                     a text screen on black
//   c <from>-<to>[#track] <file> ['<caption>'] [!<sound>]    a clip; s instead of c marks one to stabilize
//
// docs/movie.md#6-project-files owns the grammar; this is its one reader and its one writer.
//

static bool is_dfv_space(const char c)
{
	return c == ' ' || c == '\t';
}

namespace
{
	struct dfv_field
	{
		std::string text;
		bool quoted = false;
		// Written with a leading !: the sound file a clip plays in place of its own sound. The ! is not
		// part of the text.
		bool sound = false;
	};
}

// The fields of one line. A "double-quoted" field is taken as it stands, which is how a path with a space
// in it is written; a 'single-quoted' one reads \n as a new line, and \' and \\ as themselves. A field
// starting with ! names a sound file, quoted or not after the !. Nothing for a quote left open, or a quote
// run straight into the next field.
static std::optional<std::vector<dfv_field>> split_dfv_line(const std::string_view line)
{
	std::vector<dfv_field> result;
	size_t i = 0;

	while (true)
	{
		while (i < line.size() && is_dfv_space(line[i])) ++i;
		if (i >= line.size()) break;

		dfv_field field;

		// A quote straight after the ! quotes the sound file's path; anything else after it is read below.
		if (line[i] == '!' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\''))
		{
			field.sound = true;
			++i;
		}

		if (line[i] == '"')
		{
			const auto close = line.find('"', i + 1);
			if (close == std::string_view::npos) return std::nullopt;

			field.text = line.substr(i + 1, close - i - 1);
			field.quoted = true;
			i = close + 1;
		}
		else if (line[i] == '\'')
		{
			field.quoted = true;
			auto closed = false;
			++i;

			while (i < line.size())
			{
				const auto c = line[i++];

				if (c == '\'')
				{
					closed = true;
					break;
				}

				if (c == '\\' && i < line.size())
				{
					const auto escaped = line[i];

					if (escaped == 'n')
					{
						field.text += '\n';
						++i;
						continue;
					}

					if (escaped == '\'' || escaped == '\\' || escaped == '"')
					{
						field.text += escaped;
						++i;
						continue;
					}
				}

				field.text += c;
			}

			if (!closed) return std::nullopt;
		}
		else
		{
			const auto begin = i;
			while (i < line.size() && !is_dfv_space(line[i])) ++i;
			field.text = line.substr(begin, i - begin);

			if (field.text.starts_with('!'))
			{
				field.sound = true;
				field.text.erase(0, 1);
			}
		}

		if (field.quoted && i < line.size() && !is_dfv_space(line[i])) return std::nullopt;
		result.emplace_back(std::move(field));
	}

	return result;
}

// A field as it was written, for a problem that names it.
static std::string dfv_field_text(const dfv_field& field)
{
	return field.sound ? "!" + field.text : field.text;
}

static std::optional<double> parse_dfv_decimal(const std::string_view text)
{
	auto digits = 0;
	auto dots = 0;

	for (const auto c : text)
	{
		if (c == '.') ++dots;
		else if (c >= '0' && c <= '9') ++digits;
		else return {};
	}

	if (digits == 0 || dots > 1) return {};

	auto value = 0.0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return {};
	return value;
}

// Seconds, written as seconds (83.5), minutes and seconds (1:23.5) or hours too (1:01:23.5). Only the
// seconds may have a fraction, and the parts after the first count up to 59.
static std::optional<double> parse_dfv_time(const std::string_view text)
{
	auto total = 0.0;
	size_t parts = 0;
	size_t from = 0;

	while (true)
	{
		const auto colon = text.find(':', from);
		const auto part = text.substr(from, colon == std::string_view::npos ? std::string_view::npos : colon - from);
		const auto last = colon == std::string_view::npos;

		if (++parts > 3) return {};

		const auto value = parse_dfv_decimal(part);
		if (!value) return {};
		if (!last && part.find('.') != std::string_view::npos) return {};
		if (parts > 1 && *value >= 60) return {};

		total = total * 60 + *value;
		if (last) break;
		from = colon + 1;
	}

	return total;
}

namespace
{
	// The part of a source a clip plays: from and to in seconds, either left open, and which of a SID
	// file's tunes.
	struct dfv_segment
	{
		std::optional<double> from;
		std::optional<double> to;
		int track = 0;
	};
}

static std::optional<dfv_segment> parse_dfv_segment(const std::string_view text)
{
	dfv_segment result;
	auto range = text;

	if (const auto hash = text.find('#'); hash != std::string_view::npos)
	{
		range = text.substr(0, hash);
		const auto track = text.substr(hash + 1);
		if (track.empty() || track.size() > 4 ||
		    !std::ranges::all_of(track, [](const char c) { return c >= '0' && c <= '9'; }))
		{
			return {};
		}

		std::from_chars(track.data(), track.data() + track.size(), result.track);

		// "#4" alone is the whole of tune 4.
		if (range.empty()) return result;
	}

	const auto dash = range.find('-');
	if (dash == std::string_view::npos || range.find('-', dash + 1) != std::string_view::npos) return {};

	const auto from = range.substr(0, dash);
	const auto to = range.substr(dash + 1);

	if (!from.empty())
	{
		result.from = parse_dfv_time(from);
		if (!result.from) return {};
	}

	if (!to.empty())
	{
		result.to = parse_dfv_time(to);
		// A part ending at its source's start has nothing in it; an end of zero is not "to the end".
		if (!result.to || *result.to <= 0) return {};
	}

	if (result.from && result.to && *result.to <= *result.from) return {};
	return result;
}

// A path relative to the project's folder, which is how a project and the media beneath it move
// together. Empty for a path the process code page cannot convert, a byte that is not UTF-8 among it:
// that names no file here, and its line is reported rather than the whole project refused.
static core::file_path resolve_in_project_folder(const std::string_view relative,
                                                 const core::folder_path project_folder)
{
	try
	{
		const auto resolved = (std::filesystem::path(project_folder.text().sv()) / std::filesystem::path(relative))
		                          .lexically_normal();
		return core::file_path(resolved.string());
	}
	catch (const std::exception&)
	{
		return {};
	}
}

// Either separator divides a path in a project, whichever platform wrote it.
static core::file_path read_dfv_path(const std::string_view text, const core::folder_path project_folder)
{
	auto native = std::string(text);
	for (auto&& c : native)
		if (c == '/' || c == '\\') c = core::preferred_path_sep;

	if (is_qualified_path(native)) return core::file_path(native);
	if (project_folder.is_empty() || native.empty()) return {};
	return resolve_in_project_folder(native, project_folder);
}

// A clip read from a file is untrusted: its range is held to what any clip in a document may be, and a
// photo's length to what one may show for.
static void bound_clip(movie_clip& clip)
{
	clip.text = bound_movie_text(clip.text);

	if (clip.is_photo || clip.is_text)
	{
		clip.end = bound_hold(clip.end - clip.start, movie_photo_seconds);
		clip.start = 0;
		return;
	}

	clip.start = std::clamp(clip.start, 0.0, movie_max_clip_seconds);
	// An end of zero is a clip that runs to the end of its source, which the probe measures.
	clip.end = clip.start + std::min(clip.end - clip.start, movie_max_clip_seconds);
}

movie_load_result read_dfv(const std::string_view text, const core::folder_path project_folder)
{
	movie_load_result result;

	auto body = text;
	if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);

	auto line_number = 0;
	size_t from = 0;

	while (from <= body.size())
	{
		const auto newline = body.find('\n', from);
		auto line = body.substr(from, newline == std::string_view::npos ? std::string_view::npos : newline - from);
		from = newline == std::string_view::npos ? body.size() + 1 : newline + 1;
		++line_number;

		if (line.ends_with('\r')) line.remove_suffix(1);
		line = core::trim(line);
		if (line.empty() || line.front() == '#') continue;

		const auto problem = [&result, line_number](std::string reason)
		{
			result.problems.push_back({line_number, std::move(reason)});
		};

		const auto fields = split_dfv_line(line);

		if (!fields || fields->empty())
		{
			problem("a quote is not closed, or runs straight into the next field");
			continue;
		}

		const auto& f = *fields;
		const auto op = f[0].quoted || f[0].sound ? std::string{} : core::to_lower(f[0].text);
		const auto names_sound = std::ranges::any_of(f, [](const dfv_field& field) { return field.sound; });

		if (op == "short")
		{
			if (f.size() != 1)
			{
				problem("a YouTube Short is the word short alone on its line");
				continue;
			}

			result.kind = movie_kind::youtube_short;
		}
		else if (op == "t")
		{
			const auto seconds = f.size() >= 2 ? parse_dfv_time(f[1].text) : std::nullopt;

			if (names_sound)
			{
				problem("a text screen plays no sound file: put the ! field on a clip");
				continue;
			}

			if (f.size() < 2 || f.size() > 3 || !seconds || *seconds <= 0)
			{
				problem("write a text screen as: t <seconds> '<text>'");
				continue;
			}

			result.clips.emplace_back(make_movie_text(f.size() > 2 ? f[2].text : std::string{}, *seconds));
		}
		else if (op == "c" || op == "s")
		{
			// After its file, a clip may have a caption and a sound file, in either order: one of each.
			const auto extras = f.size() > 3 ? std::span(f).subspan(3) : std::span<const dfv_field>{};
			const auto sounds = std::ranges::count_if(extras, [](const dfv_field& field) { return field.sound; });
			const auto captions = std::ssize(extras) - sounds;

			if (f.size() < 3 || f[1].sound || f[2].sound || sounds > 1 || captions > 1)
			{
				problem("write a clip as: c <from>-<to>[#track] <file> ['<caption>'] [!<sound file>]");
				continue;
			}

			const auto segment = f[1].quoted ? std::nullopt : parse_dfv_segment(f[1].text);

			if (!segment)
			{
				problem(
					std::format("{} is not a part of a clip: write it as from-to, -to, from-, - or #track", f[1].text));
				continue;
			}

			const auto path = read_dfv_path(f[2].text, project_folder);

			if (path.is_empty())
			{
				problem(std::format("{} is not a file this project can name", f[2].text));
				continue;
			}

			const auto sound = std::ranges::find_if(extras, [](const dfv_field& field) { return field.sound; });
			const auto caption = std::ranges::find_if(extras, [](const dfv_field& field) { return !field.sound; });
			auto sound_path = core::file_path{};

			if (sound != extras.end())
			{
				sound_path = read_dfv_path(sound->text, project_folder);

				if (sound_path.is_empty())
				{
					problem(sound->text.empty()
					            ? std::string("a ! names no sound file: write it as !<file>")
					            : std::format("{} is not a file this project can name", dfv_field_text(*sound)));
					continue;
				}
			}

			auto clip = make_movie_clip(path);
			clip.stabilize = op == "s";
			clip.song = segment->track;
			clip.start = segment->from.value_or(0.0);
			clip.sound_file = sound_path;

			// A photo shows for as long as its part spans; given no end, for the default.
			if (clip.is_photo) clip.end = segment->to ? *segment->to : clip.start + movie_photo_seconds;
			else clip.end = segment->to.value_or(0.0);

			if (caption != extras.end()) clip.text = caption->text;
			bound_clip(clip);
			result.clips.emplace_back(std::move(clip));
		}
		else
		{
			problem(std::format("{} is not an instruction: a line starts with t, c or s, or is short",
			                    dfv_field_text(f[0])));
		}
	}

	// A file every line of which was a problem is something else, not a project.
	result.status = result.clips.empty() && !result.problems.empty() ? movie_load_status::unreadable
	                                                                 : movie_load_status::ok;
	return result;
}

// Seconds to the millisecond, without a trailing zero or point: 4, 12.5, 18.045.
static std::string format_dfv_seconds(const double seconds)
{
	auto text = std::format("{:.3f}", std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0);
	while (text.ends_with('0')) text.pop_back();
	if (text.ends_with('.')) text.pop_back();
	return text;
}

static std::string quote_dfv_text(const std::string_view text)
{
	std::string result = "'";

	for (const auto c : text)
	{
		switch (c)
		{
		case '\\': result += "\\\\"; break;
		case '\'': result += "\\'"; break;
		case '\n': result += "\\n"; break;
		case '\r': break;
		default: result += c; break;
		}
	}

	result += '\'';
	return result;
}

// A source beneath the project's folder as a path relative to it, with forward slashes, which every
// platform reads; any other source whole.
static std::string dfv_path_text(const core::file_path path, const core::folder_path project_folder)
{
	auto full = path.pack();

	if (!project_folder.is_empty())
	{
		auto base = std::string(project_folder.text().sv());
		if (!base.empty() && !core::is_path_sep(base.back())) base += core::preferred_path_sep;

		if (full.size() > base.size() &&
		    core::compare_path_key(std::string_view(full).substr(0, base.size()), base) == 0)
		{
			auto relative = full.substr(base.size());
			for (auto&& c : relative)
				if (c == '\\') c = '/';
			return relative;
		}
	}

	return full;
}

// A path as one field: as it stands when it has no space and does not begin with a quote or a !, in double
// quotes when it has, and in single quotes, escaped, in the one case a double quote is in the name.
static std::string quote_dfv_path(const std::string_view text)
{
	const auto plain = !text.empty() && text.front() != '"' && text.front() != '\'' && text.front() != '!' &&
	                   std::ranges::none_of(text,
	                                        [](const char c) { return is_dfv_space(c) || c == '\n' || c == '\r'; });
	if (plain) return std::string(text);
	if (text.find('"') == std::string_view::npos && text.find('\n') == std::string_view::npos)
	{
		return std::format("\"{}\"", text);
	}
	return quote_dfv_text(text);
}

// The part of its source a clip plays. An in point at the start and an out point at the end are left
// out, so an untrimmed clip is "-" and follows its source whatever length it turns out to be.
static std::string dfv_segment_text(const movie_clip& clip)
{
	std::string result;

	if (clip.is_photo)
	{
		result = "-" + format_dfv_seconds(clip.duration());
	}
	else
	{
		const auto from = format_dfv_seconds(clip.start);
		const auto runs_to_end = clip.end <= 0 ||
		                         (clip.source_duration > 0 && clip.end >= clip.source_duration - 0.0005);
		result = (from == "0" ? std::string{} : from) + "-" +
		         (runs_to_end ? std::string{} : format_dfv_seconds(clip.end));
	}

	if (clip.song > 0) result += std::format("#{}", clip.song);
	return result;
}

std::string write_dfv(const std::vector<movie_clip>& clips, const core::folder_path project_folder,
                      const movie_kind kind)
{
	std::string result =
		"# Diffractor video: one item per line, played in order.\n"
		"#   t <seconds> '<text>'                                        text on black\n"
		"#   c <from>-<to>[#track] <file> ['<caption>'] [!<sound file>]  a clip; s instead of c marks one to stabilize\n"
		"#   A clip's !<sound file> plays in place of its own sound, from the clip's start.\n"
		"#   A line short makes the movie a YouTube Short: upright, and three minutes at most.\n";

	if (kind == movie_kind::youtube_short) result += "short\n";

	for (const auto& clip : clips)
	{
		if (clip.is_text)
		{
			result += std::format("t {} {}\n", format_dfv_seconds(clip.duration()), quote_dfv_text(clip.text));
			continue;
		}

		result += clip.stabilize ? "s " : "c ";
		result += dfv_segment_text(clip);
		result += ' ';
		result += quote_dfv_path(dfv_path_text(clip.path, project_folder));

		if (!clip.text.empty())
		{
			result += ' ';
			result += quote_dfv_text(clip.text);
		}

		if (clip.has_sound_file())
		{
			result += " !";
			result += quote_dfv_path(dfv_path_text(clip.sound_file, project_folder));
		}

		result += '\n';
	}

	return result;
}

//
// The document
//

void movie_project::push_undo()
{
	// Every step that is not a gesture's own ends the gesture before it.
	_open_group = movie_edit_group::none;

	// An edit made after undoing past the clean state replaces the history that led back to it.
	if (_clean_depth && _undo.size() < *_clean_depth) _clean_depth.reset();

	_undo.emplace_back(undo_entry{_clips, _selected, _current, _anchor, _kind});

	if (_undo.size() > max_undo)
	{
		_undo.erase(_undo.begin());

		// Every remaining snapshot moved one place down. The clean one, if it was the one dropped,
		// is gone for good.
		if (_clean_depth)
		{
			if (*_clean_depth == 0) _clean_depth.reset();
			else --*_clean_depth;
		}
	}

	_modified = true;
	++_revision;
	++_edit_revision;
}

// The undo step for an edit: a new one, or the one its gesture already holds, which keeps the state from
// before the gesture began.
void movie_project::begin_edit(const movie_edit_group group)
{
	if (group != movie_edit_group::none && group == _open_group && !_undo.empty())
	{
		_modified = true;
		++_revision;
		++_edit_revision;
		return;
	}

	push_undo();
	_open_group = group;
}

// A gesture that has come back to where it began is no edit, so its step is given back.
void movie_project::settle_edit()
{
	if (_open_group == movie_edit_group::none || _undo.empty()) return;
	if (_undo.back().clips != _clips || _undo.back().kind != _kind) return;

	_undo.pop_back();
	_open_group = movie_edit_group::none;
	_modified = !_clean_depth || *_clean_depth != _undo.size();
}

void movie_project::mark_clean()
{
	_clean_depth = _undo.size();
	_modified = false;
	_open_group = movie_edit_group::none;
}

void movie_project::select_only(const size_t index)
{
	if (_clips.empty())
	{
		_selected.clear();
		_current = 0;
		_anchor = 0;
		return;
	}

	_current = std::min(index, _clips.size() - 1);
	_anchor = _current;
	_selected.assign(1, _current);
}

void movie_project::current(const size_t i)
{
	select_only(i);
}

bool movie_project::is_selected(const size_t i) const
{
	return std::find(_selected.begin(), _selected.end(), i) != _selected.end();
}

void movie_project::select(const size_t index, const bool extend, const bool toggle)
{
	if (index >= _clips.size()) return;

	if (extend)
	{
		// The run between the anchor and the clip: in place of the selection, or with Ctrl added to it, as in any list.
		if (!toggle) _selected.clear();

		for (auto i = std::min(_anchor, index); i <= std::max(_anchor, index); ++i)
		{
			if (std::find(_selected.begin(), _selected.end(), i) == _selected.end()) _selected.emplace_back(i);
		}

		std::sort(_selected.begin(), _selected.end());
	}
	else if (toggle)
	{
		const auto found = std::find(_selected.begin(), _selected.end(), index);

		if (found != _selected.end())
		{
			// The last selected item cannot be toggled away: a command that acts on the selection
			// would then have nothing to act on while the strip still shows a focused item.
			if (_selected.size() > 1) _selected.erase(found);
		}
		else
		{
			_selected.emplace_back(index);
			std::sort(_selected.begin(), _selected.end());
		}

		_anchor = index;
	}
	else
	{
		_selected.assign(1, index);
		_anchor = index;
	}

	_current = index;
}

void movie_project::focus(const size_t index)
{
	if (index < _clips.size()) _current = index;
}

void movie_project::select_all()
{
	_selected.clear();
	for (size_t i = 0; i < _clips.size(); ++i) _selected.emplace_back(i);
	if (_current >= _clips.size()) _current = _clips.empty() ? 0 : _clips.size() - 1;
}

const movie_clip* movie_project::current_clip() const
{
	return _current < _clips.size() ? &_clips[_current] : nullptr;
}

bool movie_project::is_ready_to_render() const
{
	return !_clips.empty() && std::all_of(_clips.begin(), _clips.end(), [](const movie_clip& clip)
	{
		if (clip.has_sound_file() && (!clip.sound_file_is_probed || clip.sound_file_is_missing)) return false;
		if (clip.is_text) return clip.duration() > 0;
		return clip.is_probed && !clip.is_missing && clip.duration() > 0 && clip.extent.cx > 0 && clip.extent.cy > 0;
	});
}

void movie_project::append(const movie_clip& clip)
{
	insert(_clips.size(), clip);
}

void movie_project::insert(const size_t at, const movie_clip& clip)
{
	push_undo();
	const auto index = std::min(at, _clips.size());
	_clips.insert(_clips.begin() + index, clip);
	select_only(index);
}

void movie_project::insert_many(const size_t at, const std::vector<movie_clip>& clips)
{
	if (clips.empty()) return;

	push_undo();
	const auto index = std::min(at, _clips.size());
	_clips.insert(_clips.begin() + static_cast<std::ptrdiff_t>(index), clips.begin(), clips.end());
	select_only(index);
}

void movie_project::remove(const size_t at)
{
	if (at >= _clips.size()) return;

	push_undo();
	_clips.erase(_clips.begin() + at);
	select_only(_current > at ? _current - 1 : _current);
}

void movie_project::move(const size_t from, const size_t to)
{
	if (from >= _clips.size() || from == to) return;

	push_undo();
	const auto clip = _clips[from];
	_clips.erase(_clips.begin() + from);
	const auto index = std::min(to, _clips.size());
	_clips.insert(_clips.begin() + index, clip);
	select_only(index);
}

void movie_project::move_selection(const size_t to)
{
	if (_selected.empty() || _clips.empty()) return;

	std::vector<movie_clip> moved;
	std::vector<movie_clip> rest;
	size_t insert_at = 0;
	size_t focus_offset = 0;

	// `to` counts positions in the list as it stands, so the destination has to be re-expressed
	// against the items that will still be there once the selection is lifted out.
	for (size_t i = 0; i < _clips.size(); ++i)
	{
		if (is_selected(i))
		{
			if (i == _current) focus_offset = moved.size();
			moved.emplace_back(_clips[i]);
		}
		else
		{
			if (i < to) ++insert_at;
			rest.emplace_back(_clips[i]);
		}
	}

	if (moved.empty()) return;

	insert_at = std::min(insert_at, rest.size());

	// A block dropped where it already is changes nothing, and must not cost an undo step.
	if (_selected.front() == insert_at && _selected.back() - _selected.front() + 1 == _selected.size()) return;

	push_undo();

	rest.insert(rest.begin() + insert_at, moved.begin(), moved.end());
	_clips = std::move(rest);

	_selected.clear();
	for (size_t i = 0; i < moved.size(); ++i) _selected.emplace_back(insert_at + i);

	_current = insert_at + focus_offset;
	_anchor = _current;
}

void movie_project::remove_selection()
{
	if (_selected.empty() || _clips.empty()) return;

	push_undo();

	const auto first = _selected.front();

	// Erased from the back so the earlier indices stay valid as the list shrinks.
	for (auto it = _selected.rbegin(); it != _selected.rend(); ++it)
	{
		if (*it < _clips.size()) _clips.erase(_clips.begin() + *it);
	}

	select_only(first);
}

void movie_project::replace(const size_t at, const movie_clip& clip)
{
	if (at >= _clips.size() || _clips[at] == clip) return;

	push_undo();
	_clips[at] = clip;
}

void movie_project::replace_many(const std::vector<std::pair<size_t, movie_clip>>& replacements)
{
	if (replacements.empty()) return;
	if (std::none_of(replacements.begin(), replacements.end(), [this](const auto& replacement)
	{ return replacement.first < _clips.size() && _clips[replacement.first] != replacement.second; }))
		return;

	push_undo();

	for (const auto& [at, clip] : replacements)
	{
		if (at < _clips.size()) _clips[at] = clip;
	}
}

void movie_project::replace_quietly(const size_t at, const movie_clip& clip)
{
	if (at >= _clips.size()) return;

	_clips[at] = clip;
	++_revision;
}

void movie_project::text(const size_t at, const std::string_view words, const movie_edit_group group)
{
	if (at >= _clips.size()) return;

	auto bounded = bound_movie_text(words);
	if (bounded == _clips[at].text) return;

	// The same box describes whichever item has focus, so typing on after focus moved is a new step.
	if (group != movie_edit_group::none && group == _open_group && at != _open_index) end_edit_group();

	begin_edit(group);
	_open_index = at;
	_clips[at].text = std::move(bounded);
	settle_edit();
}

void movie_project::trim(const size_t at, const double start, const double end, const movie_edit_group group)
{
	if (at >= _clips.size()) return;

	auto c = _clips[at];

	if (c.is_photo || c.is_text)
	{
		c.start = 0;
		c.end = bound_hold(end - start, c.duration());
	}
	else
	{
		const auto limit = c.source_duration > 0 ? c.source_duration : end;
		c.start = std::clamp(start, 0.0, limit);
		c.end = std::clamp(end, c.start, limit);
	}

	// A handle pressed and let go where it was is not a trim.
	if (c == _clips[at]) return;

	if (group != movie_edit_group::none && group == _open_group && at != _open_index) end_edit_group();

	begin_edit(group);
	_open_index = at;
	_clips[at] = c;
	settle_edit();
}

void movie_project::end_edit_group()
{
	_open_group = movie_edit_group::none;
}

void movie_project::kind(const movie_kind kind)
{
	if (kind == _kind) return;

	push_undo();
	_kind = kind;
}

void movie_project::reset(std::vector<movie_clip> clips, const core::file_path path, const movie_kind kind)
{
	_clips = std::move(clips);
	_path = path;
	_kind = kind;
	_undo.clear();
	mark_clean();
	++_revision;
	++_edit_revision;
	select_only(0);
}

void movie_project::mark_saved(const core::file_path path, const uint64_t edit_revision)
{
	_path = path;

	// A save that was overtaken by an edit wrote a state the history no longer points at, so no undo
	// depth stands for the file any more and only another save can settle the document. That holds
	// even when the overtaking step was an Undo back to an earlier clean point: the file on disk now
	// holds the state the user undid.
	if (_edit_revision == edit_revision) mark_clean();
	else
	{
		_clean_depth.reset();
		_modified = true;
		_open_group = movie_edit_group::none;
	}
}

void movie_project::mark_seeded()
{
	_undo.clear();
	mark_clean();
}

void movie_project::undo()
{
	if (_undo.empty()) return;

	_open_group = movie_edit_group::none;

	auto entry = std::move(_undo.back());
	_undo.pop_back();

	_clips = std::move(entry.clips);
	_selected = std::move(entry.selected);
	_current = entry.current;
	_anchor = entry.anchor;
	_kind = entry.kind;
	_modified = !_clean_depth || *_clean_depth != _undo.size();
	++_revision;
	++_edit_revision;

	// The stored selection came from a list that may have been longer than this one.
	std::erase_if(_selected, [this](const size_t i) { return i >= _clips.size(); });
	if (_selected.empty() && !_clips.empty()) select_only(std::min(_current, _clips.size() - 1));
	if (_current >= _clips.size()) _current = _clips.empty() ? 0 : _clips.size() - 1;
}

movie_entry decide_movie_entry(const movie_project& project, const std::vector<core::file_path>& selection,
                               const std::vector<core::file_path>& built_from)
{
	// A pristine empty timeline with no file behind it has nothing to return to. One that was opened or
	// saved empty is a document like any other, and the rules below answer for it.
	if (project.is_empty() && !project.is_modified() && project.path().is_empty()) return movie_entry::seed;
	if (selection.empty() || selection == built_from) return movie_entry::resume;

	// Leaving Movie settles unsaved edits, so a timeline still holding them reached here some other
	// way. Replacing it would lose work nobody was asked about, so it is kept for leaving to ask.
	if (project.is_modified()) return movie_entry::resume;

	return movie_entry::seed;
}
