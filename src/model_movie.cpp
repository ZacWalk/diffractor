// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implements the Movie document -- timeline edits and undo, transition and duration
// arithmetic, the derived output geometry, the decision entering Movie makes about the timeline it
// holds, the OpenTimelineIO project file, the Windows Live Movie Maker reader, and the choice among
// same-named files for a source that has moved. Pure: no I/O, no decoding, no UI. docs/movie.md
// owns the behaviour.

#include "pch.h"
#include "model_movie.h"

#include "files.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// OpenTimelineIO stores times as a value over a rate. Values are written exactly, over a rate of 1000
// so they still read as milliseconds: rounded to whole milliseconds, a clip whose source is not a
// whole number of them long read back a fraction short of it, and showed as trimmed.
static constexpr double otio_rate = 1000.0;

// The convention a project's clip ranges follow. From version 2 a range stops at the cut and each
// transition reaches past it into the media on both sides, as OpenTimelineIO has it; before that,
// Movie wrote each clip's whole retained range and other tools read every crossfade as extra length.
static constexpr int otio_project_version = 2;

static bool is_qualified_path(const std::string_view s)
{
	if constexpr (!df::windows_path_semantics)
	{
		return !s.empty() && df::is_path_sep(s[0]);
	}
	else
	{
		if (s.size() < 3) return false;
		if (df::is_path_sep(s[0]) && df::is_path_sep(s[1])) return true;
		return s[1] == ':' && df::is_path_sep(s[2]);
	}
}

//
// Timing
//

double movie_overlap(const movie_clip& left, const movie_clip& right, const movie_settings& settings)
{
	if (settings.transition != movie_transition::crossfade) return 0;
	if (settings.transition_seconds <= 0) return 0;

	return std::min({settings.transition_seconds, left.duration() / 2, right.duration() / 2});
}

movie_timing calc_movie_timing(const std::vector<movie_clip>& clips, const movie_settings& settings)
{
	movie_timing result;
	result.starts.reserve(clips.size());

	double t = 0;

	for (size_t i = 0; i < clips.size(); ++i)
	{
		result.starts.emplace_back(t);
		t += clips[i].duration();
		if (i + 1 < clips.size()) t -= movie_overlap(clips[i], clips[i + 1], settings);
	}

	result.duration = std::max(0.0, t);
	return result;
}

movie_frame calc_movie_frame(const std::vector<movie_clip>& clips, const movie_settings& settings,
                             const double time)
{
	return calc_movie_frame(clips, settings, calc_movie_timing(clips, settings), time);
}

movie_frame calc_movie_frame(const std::vector<movie_clip>& clips, const movie_settings& settings,
	const movie_timing& timing, const double time)
{
	movie_frame result;
	if (clips.empty()) return result;

	const auto at = std::clamp(time, 0.0, timing.duration);

	const auto contribute = [&](const size_t i, const double weight)
	{
		const auto& clip = clips[i];
		movie_frame_source source;
		source.index = static_cast<int>(i);
		source.weight = weight;
		// A photo has one frame however long it is held, so asking for a position in it is
		// meaningless; the compositor decodes it once and reuses it.
		source.source_time = clip.is_photo
			                     ? 0.0
			                     : clip.start + std::clamp(at - timing.starts[i], 0.0, clip.duration());

		(result.a.index < 0 ? result.a : result.b) = source;
	};

	for (size_t i = 0; i < clips.size() && result.b.index < 0; ++i)
	{
		const auto start = timing.starts[i];
		const auto end = start + clips[i].duration();

		// The last clip includes its end, so the final instant of the movie still draws something.
		const auto inside = at >= start && (at < end || (i + 1 == clips.size() && at <= end));
		if (!inside) continue;

		if (result.a.index < 0)
		{
			contribute(i, 1.0);
			continue;
		}

		const auto overlap = movie_overlap(clips[i - 1], clips[i], settings);

		if (overlap <= 0)
		{
			break;
		}

		const auto progress = std::clamp((at - start) / overlap, 0.0, 1.0);
		result.a.weight = 1.0 - progress;
		contribute(i, progress);
	}

	// The fade is as long as a transition, because one number the user already set is easier to
	// predict than a second one that means nearly the same thing.
	const auto fade = std::clamp(settings.transition_seconds, 0.0, timing.duration / 2);

	if (fade > 0)
	{
		if (settings.fade_in && at < fade) result.fade_to_black = 1.0 - at / fade;
		if (settings.fade_out && at > timing.duration - fade)
		{
			result.fade_to_black = std::max(result.fade_to_black, 1.0 - (timing.duration - at) / fade);
		}
	}

	return result;
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
		static_cast<double>(max_cy) / extent.cy
	});

	const auto cx = std::max(2, static_cast<int>(std::lround(extent.cx * scale)));
	const auto cy = std::max(2, static_cast<int>(std::lround(extent.cy * scale)));

	// Every encoder in reach refuses an odd dimension, so rounding down is the only safe direction.
	return {cx - (cx & 1), cy - (cy & 1)};
}

movie_output derive_movie_output(const std::vector<movie_clip>& clips)
{
	movie_output result;

	// Videos decide the frame, and only fall back to photos when there is no video at all. A
	// 45-megapixel photo beside a 1080p clip must not produce an 8000-pixel movie.
	const movie_clip* largest = nullptr;
	auto largest_area = 0LL;
	auto have_video = false;
	auto rate = 0.0;

	for (const auto& c : clips)
	{
		if (c.extent.cx <= 0 || c.extent.cy <= 0) continue;
		if (!c.is_photo) rate = std::max(rate, c.frame_rate);

		const auto video_here = !c.is_photo;
		if (have_video && !video_here) continue;

		const auto area = static_cast<int64_t>(c.extent.cx) * c.extent.cy;

		if (video_here && !have_video)
		{
			have_video = true;
			largest = &c;
			largest_area = area;
		}
		else if (area > largest_area)
		{
			largest = &c;
			largest_area = area;
		}
	}

	result.extent = largest ? fit_within(largest->extent, movie_max_width, movie_max_height) : sizei{1920, 1080};
	if (result.extent.cx <= 0 || result.extent.cy <= 0) result.extent = {1920, 1080};

	result.frame_rate = rate > 0
		                    ? std::clamp(static_cast<int>(std::lround(rate)), 1, movie_max_frame_rate)
		                    : movie_default_frame_rate;

	const auto bits = static_cast<int64_t>(result.extent.cx) * result.extent.cy * result.frame_rate *
		movie_bits_per_pixel;
	result.video_bitrate = static_cast<int>(std::clamp(bits, static_cast<double>(movie_min_bitrate),
	                                                   static_cast<double>(movie_max_bitrate)));

	return result;
}

movie_clip make_movie_clip(df::file_path path, const movie_settings& settings)
{
	movie_clip result;
	result.path = path;

	const auto mt = files::file_type_from_name(path);
	result.is_photo = mt && mt->group == file_group::photo;

	if (result.is_photo)
	{
		result.end = settings.photo_seconds;
		result.photo_duration_is_default = true;
	}

	return result;
}

bool movie_output_names_a_clip(const std::vector<movie_clip>& clips, const df::file_path output)
{
	if (output.is_empty()) return false;

	return std::ranges::any_of(clips, [output](const movie_clip& clip)
	{
		return !clip.path.is_empty() && df::compare_path_key(clip.path.pack(), output.pack()) == 0;
	});
}

//
// The project file
//

// Always forward-slashed, whether relative or absolute. A native Windows path would be written
// into the JSON with every separator escaped, which is both unreadable and a needless difference
// between the file this platform writes and the one the other reads. A '%' is escaped: the reader
// decodes percent escapes, as a URL wants, so a name holding "%20" came back naming another file
// and one holding "50%" did not come back at all.
static std::string to_url(const df::file_path path, const df::folder_path project_folder)
{
	const auto escape_percent = [](const std::string_view text)
	{
		std::string escaped;
		escaped.reserve(text.size());

		for (const auto c : text)
		{
			if (c == '%') escaped += "%25";
			else escaped += c;
		}

		return escaped;
	};

	if (!project_folder.is_empty())
	{
		try
		{
			const auto relative = std::filesystem::path(path.pack()).lexically_relative(
				std::filesystem::path(project_folder.text().sv()));
			if (!relative.empty()) return escape_percent(relative.generic_string());
		}
		catch (const std::exception&)
		{
			// A narrow path converts through the process code page, which refuses a byte that is not
			// UTF-8 -- and a name decoded from another tool's file URI can hold one. Such a path is
			// written whole, as one on another drive is.
		}
	}

	auto result = path.pack();
	for (auto&& c : result) if (c == '\\') c = '/';
	return escape_percent(result);
}

static int hex_digit(const char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static std::optional<std::string> decode_url_path(const std::string_view url)
{
	std::string result;
	result.reserve(url.size());

	for (size_t i = 0; i < url.size(); ++i)
	{
		if (url[i] != '%')
		{
			result += url[i];
			continue;
		}

		// Not an escape, so it is the character itself. A project written before '%' was escaped
		// holds names such as "50% off.jpg", and refusing the whole path dropped the clip.
		const auto high = i + 2 < url.size() ? hex_digit(url[i + 1]) : -1;
		const auto low = i + 2 < url.size() ? hex_digit(url[i + 2]) : -1;

		if (high < 0 || low < 0)
		{
			result += url[i];
			continue;
		}

		const auto decoded = static_cast<char>((high << 4) | low);
		if (decoded == '\0') return {};
		result += decoded;
		i += 2;
	}

	return result;
}

// A path relative to the project's folder, which is how a project and the media beside it move
// together. The caller has already put the separators in the form this platform reads. Empty for a
// path the process code page cannot convert, a byte that is not UTF-8 among it: that names no file
// here, and its clip is reported as having none rather than the whole project being refused.
static df::file_path resolve_in_project_folder(const std::string_view relative, const df::folder_path project_folder)
{
	try
	{
		const auto resolved = (std::filesystem::path(project_folder.text().sv()) / std::filesystem::path(relative))
			.lexically_normal();
		return df::file_path(resolved.string());
	}
	catch (const std::exception&)
	{
		return {};
	}
}

static df::file_path from_url(const std::string_view url, const df::folder_path project_folder)
{
	if (url.empty()) return {};

	auto encoded = url;
	auto file_uri = false;

	if (url.size() >= 5 && str::icmp(url.substr(0, 5), "file:") == 0)
	{
		file_uri = true;
		encoded.remove_prefix(5);
	}
	else if (url.find("://") != std::string_view::npos)
	{
		return {};
	}

	auto decoded = decode_url_path(encoded);
	if (!decoded) return {};
	auto native = std::move(*decoded);

	if (file_uri && native.starts_with("//"))
	{
		native.erase(0, 2);
		const auto slash = native.find('/');
		const auto authority = native.substr(0, slash);
		native = slash == std::string::npos ? std::string{} : native.substr(slash);

		if (!authority.empty() && str::icmp(authority, "localhost") != 0)
		{
			native = "//" + authority + native;
		}
	}

	if constexpr (df::windows_path_semantics)
	{
		if (file_uri && native.size() >= 3 && native[0] == '/' && native[2] == ':') native.erase(0, 1);
	}

	if (is_qualified_path(native)) return df::file_path(native);
	if (project_folder.is_empty()) return {};

	for (auto&& c : native) if (c == '/') c = df::preferred_path_sep;
	return resolve_in_project_folder(native, project_folder);
}

using json_writer = rapidjson::Writer<rapidjson::StringBuffer>;

// A value JSON cannot express -- an infinity, or not a number -- goes out as a key with no value, and
// the project no longer parses. A document read from a file is not trusted to the last digit.
static double finite_or_zero(const double value)
{
	return std::isfinite(value) ? value : 0.0;
}

static void write_rational_time(json_writer& w, const double seconds)
{
	w.StartObject();
	w.Key("OTIO_SCHEMA");
	w.String("RationalTime.1");
	w.Key("rate");
	w.Double(otio_rate);
	w.Key("value");
	w.Double(finite_or_zero(seconds * otio_rate));
	w.EndObject();
}

std::string write_otio(const std::vector<movie_clip>& clips, const movie_settings& settings,
                       const df::folder_path project_folder)
{
	rapidjson::StringBuffer buffer;
	json_writer w(buffer);

	w.StartObject();
	w.Key("OTIO_SCHEMA");
	w.String("Timeline.1");
	w.Key("name");
	w.String("Diffractor movie");
	w.Key("global_start_time");
	w.Null();

	w.Key("tracks");
	w.StartObject();
	w.Key("OTIO_SCHEMA");
	w.String("Stack.1");
	w.Key("name");
	w.String("tracks");
	w.Key("children");
	w.StartArray();

	w.StartObject();
	w.Key("OTIO_SCHEMA");
	w.String("Track.1");
	w.Key("name");
	w.String("Video");
	w.Key("kind");
	w.String("Video");
	w.Key("children");
	w.StartArray();

	for (size_t i = 0; i < clips.size(); ++i)
	{
		const auto& c = clips[i];

		// A transition straddles its cut and reaches half its length into the clip on each side, which
		// is where OpenTimelineIO puts one: a clip's range stops at the cut and the transition draws on
		// the media past it. Another tool then lays the clips end to end to the movie's own length.
		const auto head = i > 0 ? movie_overlap(clips[i - 1], c, settings) / 2 : 0.0;
		const auto tail = i + 1 < clips.size() ? movie_overlap(c, clips[i + 1], settings) / 2 : 0.0;

		if (head > 0)
		{
			w.StartObject();
			w.Key("OTIO_SCHEMA");
			w.String("Transition.1");
			w.Key("name");
			w.String("Crossfade");
			w.Key("transition_type");
			w.String("SMPTE_Dissolve");
			w.Key("in_offset");
			write_rational_time(w, head);
			w.Key("out_offset");
			write_rational_time(w, head);
			w.Key("metadata");
			w.StartObject();
			w.EndObject();
			w.EndObject();
		}

		w.StartObject();
		w.Key("OTIO_SCHEMA");
		w.String("Clip.1");
		w.Key("name");
		const auto clip_name = c.path.name().sv();
		w.String(clip_name.data(), static_cast<rapidjson::SizeType>(clip_name.size()));

		w.Key("source_range");
		w.StartObject();
		w.Key("OTIO_SCHEMA");
		w.String("TimeRange.1");
		w.Key("start_time");
		write_rational_time(w, c.start + head);
		w.Key("duration");
		write_rational_time(w, std::max(0.0, c.duration() - head - tail));
		w.EndObject();

		w.Key("media_reference");
		w.StartObject();
		w.Key("OTIO_SCHEMA");
		w.String("ExternalReference.1");
		w.Key("target_url");
		w.String(to_url(c.path, project_folder));
		w.Key("available_range");
		w.Null();
		w.Key("metadata");
		w.StartObject();
		w.EndObject();
		w.EndObject();

		// OTIO has no photo, and inventing one would make the file unreadable elsewhere. The flag
		// lives in our own metadata namespace, and a reader without it infers from the extension.
		w.Key("metadata");
		w.StartObject();
		w.Key("diffractor");
		w.StartObject();
		w.Key("photo");
		w.Bool(c.is_photo);
		w.Key("photo_duration_is_default");
		w.Bool(c.photo_duration_is_default);
		w.Key("source_duration");
		w.Double(finite_or_zero(c.source_duration));
		w.EndObject();
		w.EndObject();

		w.EndObject();
	}

	w.EndArray();
	w.Key("metadata");
	w.StartObject();
	w.EndObject();
	w.EndObject(); // track

	w.EndArray();
	w.Key("metadata");
	w.StartObject();
	w.EndObject();
	w.EndObject(); // stack

	// The movie settings have no OTIO home. Another tool reads the transitions written above and
	// ignores this; Diffractor reads this and rebuilds them exactly.
	w.Key("metadata");
	w.StartObject();
	w.Key("diffractor");
	w.StartObject();
	w.Key("version");
	w.Int(otio_project_version);
	w.Key("transition");
	w.String(settings.transition == movie_transition::crossfade ? "crossfade" : "cut");
	w.Key("transition_seconds");
	w.Double(finite_or_zero(settings.transition_seconds));
	w.Key("fade_in");
	w.Bool(settings.fade_in);
	w.Key("fade_out");
	w.Bool(settings.fade_out);
	w.Key("photo_seconds");
	w.Double(finite_or_zero(settings.photo_seconds));
	w.EndObject();
	w.EndObject();

	w.EndObject();

	return {buffer.GetString(), buffer.GetSize()};
}

using json_value = rapidjson::GenericValue<rapidjson::UTF8<char>>;

static double read_rational_time(const json_value& v)
{
	if (!v.IsObject()) return 0;

	const auto rate = v.FindMember("rate");
	const auto value = v.FindMember("value");
	if (value == v.MemberEnd() || !value->value.IsNumber()) return 0;

	const auto r = rate != v.MemberEnd() && rate->value.IsNumber() ? rate->value.GetDouble() : 0.0;
	if (r <= 0) return 0;

	const auto result = value->value.GetDouble() / r;
	return std::isfinite(result) ? result : 0.0;
}

static bool read_bool(const json_value& v, const char* name, const bool fallback)
{
	if (!v.IsObject()) return fallback;
	const auto found = v.FindMember(name);
	return found != v.MemberEnd() && found->value.IsBool() ? found->value.GetBool() : fallback;
}

static double read_double(const json_value& v, const char* name, const double fallback)
{
	if (!v.IsObject()) return fallback;
	const auto found = v.FindMember(name);
	if (found == v.MemberEnd() || !found->value.IsNumber()) return fallback;
	const auto result = found->value.GetDouble();
	return std::isfinite(result) ? result : fallback;
}

// A project file is untrusted input. The panel cannot express a duration outside its own range, so
// neither may a document: a photo hold of 1e308 seconds reaches the frame count as a number no
// integer can hold, and the conversion that walks it is undefined.
static double read_bounded_double(const json_value& v, const char* name, const double fallback,
                                  const double lowest, const double highest)
{
	return std::clamp(read_double(v, name, fallback), lowest, highest);
}

// The same for each clip's own range. A still has no timeline of its own, so only its hold means
// anything; every clip is held to the longest movie a render will make, and so is its in point, which
// no source that long can have past.
static void bound_clip_range(movie_clip& clip)
{
	clip.start = std::clamp(clip.start, 0.0, movie_max_clip_seconds);
	const auto duration = std::min(clip.end - clip.start, movie_max_clip_seconds);

	if (clip.is_photo)
	{
		clip.start = 0.0;
		clip.end = std::max(duration, movie_min_photo_seconds);
	}
	else
	{
		clip.end = clip.start + duration;
	}
}

static bool is_schema(const json_value& v, const std::string_view prefix)
{
	if (!v.IsObject()) return false;

	const auto found = v.FindMember("OTIO_SCHEMA");
	if (found == v.MemberEnd() || !found->value.IsString()) return false;
	return str::starts(found->value.GetString(), prefix);
}

void movie_load_result::omit(const std::string_view name, const int count)
{
	if (count <= 0) return;

	ignored_elements += count;

	// The kinds named are bounded. A crafted file naming a million distinct effects made every count a
	// search of all of them, and a report of a million names says less than a count of the rest.
	constexpr size_t max_named = 32;

	const auto found = std::ranges::find_if(omitted, [name](const auto& entry) { return entry.first == name; });
	if (found != omitted.end()) found->second += count;
	else if (omitted.size() < max_named) omitted.emplace_back(std::string(name), count);
	else if (omitted.size() == max_named) omitted.emplace_back("other", count);
	else omitted.back().second += count;
}

// The media a clip plays. OpenTimelineIO 0.15 and later write Clip.2, which keeps its references in a
// map with the key of the one in use; earlier versions wrote the one reference alone.
static const json_value& clip_media_reference(const json_value& clip)
{
	const auto& references = df::util::json::safe_object(clip, "media_references");

	if (references.MemberCount() > 0)
	{
		const auto key = df::util::json::safe_string(clip, "active_media_reference_key");
		const auto& active = df::util::json::safe_object(references, key.empty() ? "DEFAULT_MEDIA" : key.c_str());
		if (active.MemberCount() > 0) return active;

		// A key that names no reference is a broken file, and the media there is beats none.
		for (const auto& member : references.GetObject())
		{
			if (member.value.IsObject()) return member.value;
		}
	}

	return df::util::json::safe_object(clip, "media_reference");
}

// Where the media's own time begins. An editor states a range in timecoded media in that time, from
// its first frame's timecode rather than from zero, and Movie counts from the first frame.
static double media_start_time(const json_value& reference)
{
	const auto& available = df::util::json::safe_object(reference, "available_range");
	const auto start = available.FindMember("start_time");
	return start != available.MemberEnd() ? std::max(0.0, read_rational_time(start->value)) : 0.0;
}

movie_load_result read_otio(const std::string_view json, const df::folder_path project_folder)
{
	movie_load_result result;

	rapidjson::Document doc;
	// Iterative: a project is untrusted input, and the recursive parser follows nesting on the stack,
	// so a file of nested brackets well inside the size cap would end the process.
	doc.Parse<rapidjson::kParseIterativeFlag>(json.data(), json.size());

	if (doc.HasParseError() || !doc.IsObject()) return result;

	const auto& stack = df::util::json::safe_object(doc, "tracks");
	const auto children = stack.IsObject() ? stack.FindMember("children") : stack.MemberEnd();

	if (children == stack.MemberEnd() || !children->value.IsArray())
	{
		result.status = movie_load_status::empty;
		return result;
	}

	// The first video track, and only that one. A file from another tool routinely carries audio
	// tracks, effects and markers Movie has no answer for; refusing the file over them would make
	// interchange useless, so they are counted and dropped.
	const json_value* track = nullptr;

	for (const auto& child : children->value.GetArray())
	{
		if (!is_schema(child, "Track")) continue;

		const auto kind = df::util::json::safe_string(child, "kind");

		if (!track && (kind.empty() || str::icmp(kind, "Video") == 0))
		{
			track = &child;
		}
		else
		{
			result.omit(kind.empty() ? "Track"s : std::format("{} track", kind));
		}
	}

	if (!track)
	{
		result.status = movie_load_status::empty;
		return result;
	}

	const auto& metadata = df::util::json::safe_object(doc, "metadata");
	const auto& doc_meta = df::util::json::safe_object(metadata, "diffractor");

	const auto has_diffractor_settings = metadata.IsObject() && metadata.HasMember("diffractor") &&
		metadata["diffractor"].IsObject();
	result.settings.transition = has_diffractor_settings &&
		str::icmp(df::util::json::safe_string(doc_meta, "transition"), "crossfade") == 0
		                             ? movie_transition::crossfade
		                             : movie_transition::cut;
	result.settings.transition_seconds = read_bounded_double(doc_meta, "transition_seconds", 1.0, 0.0,
	                                                        movie_max_transition_seconds);
	result.settings.fade_in = read_bool(doc_meta, "fade_in", false);
	result.settings.fade_out = read_bool(doc_meta, "fade_out", false);
	result.settings.photo_seconds = read_bounded_double(doc_meta, "photo_seconds", 4.0, movie_min_photo_seconds,
	                                                    movie_max_photo_seconds);

	// Which convention the clip ranges follow. OpenTimelineIO's own -- a range stops at the cut, and a
	// transition reaches past it into the media on each side -- is what other tools write and what
	// Movie writes from version 2. Earlier Movie projects hold each clip's whole retained range.
	const auto ranges_stop_at_cuts = !has_diffractor_settings ||
		read_double(doc_meta, "version", 1.0) >= otio_project_version;

	const auto track_children = track->FindMember("children");

	if (track_children == track->MemberEnd() || !track_children->value.IsArray())
	{
		result.status = movie_load_status::empty;
		return result;
	}

	// One transition standing on a cut between two clips the reader kept.
	struct cut_transition
	{
		// How far it reaches into the outgoing clip's media before the cut, and the incoming clip's
		// after it.
		double in_offset = 0;
		double out_offset = 0;
		bool is_dissolve = false;
	};

	std::vector<cut_transition> transitions;
	std::optional<cut_transition> pending_transition;
	auto foreign_transition_count = 0;
	auto transitions_stand_on_cuts = true;
	auto previous_is_kept_clip = false;

	// How far the transitions beside each kept clip reach into its media: the one before it into its
	// head, the one after into its tail. Each side belongs to the clip it touches, whatever the file
	// left out on the other side of that transition.
	struct clip_handles
	{
		double head = 0;
		double tail = 0;
	};

	std::vector<clip_handles> handles;
	std::optional<double> pending_head;

	// Whatever is left out breaks the cut a transition waiting for its next clip was standing on, so
	// that transition is not carried on to join two clips it never stood between.
	const auto leave_out = [&](const std::string_view name)
	{
		if (pending_transition)
		{
			transitions_stand_on_cuts = false;
			pending_transition.reset();
		}

		pending_head.reset();
		previous_is_kept_clip = false;
		result.omit(name);
	};

	for (const auto& child : track_children->value.GetArray())
	{
		if (is_schema(child, "Transition"))
		{
			if (!has_diffractor_settings) ++foreign_transition_count;

			const auto in = child.FindMember("in_offset");
			const auto out = child.FindMember("out_offset");

			cut_transition transition;
			transition.in_offset = in != child.MemberEnd() ? read_rational_time(in->value) : 0.0;
			transition.out_offset = out != child.MemberEnd() ? read_rational_time(out->value) : 0.0;
			transition.is_dissolve = str::icmp(df::util::json::safe_string(child, "transition_type"),
			                                   "SMPTE_Dissolve") == 0 && transition.in_offset >= 0 &&
				transition.out_offset >= 0 && transition.in_offset + transition.out_offset > 0;

			// Only a transition standing directly between two clips the reader keeps has a cut to
			// stand on.
			if (!previous_is_kept_clip || pending_transition)
			{
				transitions_stand_on_cuts = false;
			}
			else
			{
				pending_transition = transition;
			}

			if (previous_is_kept_clip) handles.back().tail = std::max(0.0, transition.out_offset);
			pending_head = std::max(0.0, transition.in_offset);
			previous_is_kept_clip = false;
			continue;
		}

		if (!is_schema(child, "Clip"))
		{
			const auto schema = df::util::json::safe_string(child, "OTIO_SCHEMA");
			leave_out(schema.empty() ? "unknown element"sv : std::string_view(schema));
			continue;
		}

		// A disabled clip is a gap where it stands, and Movie has no gaps.
		if (!read_bool(child, "enabled", true))
		{
			leave_out("disabled clip");
			continue;
		}

		const auto& reference = clip_media_reference(child);
		const auto path = from_url(df::util::json::safe_string(reference, "target_url"), project_folder);

		if (path.is_empty())
		{
			leave_out("clip with no media file");
			continue;
		}

		movie_clip clip;
		clip.path = path;

		// A clip with no range is OpenTimelineIO's whole media, and one saved before its source was
		// measured has a range of no length. Either is kept with no length, for the probe to give it
		// the length of its source, as a clip just added is.
		const auto& range = df::util::json::safe_object(child, "source_range");
		const auto start = range.FindMember("start_time");
		const auto duration = range.FindMember("duration");

		clip.start = start != range.MemberEnd() ? read_rational_time(start->value) : 0.0;
		const auto clip_duration = duration != range.MemberEnd() ? read_rational_time(duration->value) : 0.0;

		// A range already counted from zero is left alone: it cannot start before the media does.
		const auto media_start = media_start_time(reference);
		if (media_start > 0 && clip.start >= media_start) clip.start -= media_start;

		clip.end = clip.start + clip_duration;

		if (!std::isfinite(clip.start) || !std::isfinite(clip.end) || clip.start < 0 || clip_duration < 0)
		{
			leave_out("clip with no usable range");
			continue;
		}

		const auto& clip_meta = df::util::json::safe_object(df::util::json::safe_object(child, "metadata"),
		                                                   "diffractor");

		const auto mt = files::file_type_from_name(path);
		clip.is_photo = read_bool(clip_meta, "photo", mt && mt->group == file_group::photo);
		clip.photo_duration_is_default = read_bool(clip_meta, "photo_duration_is_default", false);
		// Negative until the range is final: a file that recorded no source length takes the range's end.
		clip.source_duration = read_double(clip_meta, "source_duration", -1.0);

		// A speed change, a freeze or any other effect is not something Movie can apply. The clip keeps
		// its place, and what it carried is reported.
		if (const auto effects = child.FindMember("effects");
			effects != child.MemberEnd() && effects->value.IsArray())
		{
			for (const auto& effect : effects->value.GetArray())
			{
				const auto schema = df::util::json::safe_string(effect, "OTIO_SCHEMA");
				result.omit(schema.empty() ? "effect"sv : std::string_view(schema));
			}
		}

		if (pending_transition)
		{
			transitions.emplace_back(*pending_transition);
			pending_transition.reset();
		}

		result.clips.emplace_back(clip);
		handles.emplace_back(clip_handles{pending_head.value_or(0.0), 0.0});
		pending_head.reset();
		previous_is_kept_clip = true;
	}

	if (pending_transition) transitions_stand_on_cuts = false;

	// Each clip widened by the transitions beside it reaching past its cuts, which is the range Movie
	// overlaps. A still is the same picture before its range as in it, so it simply holds longer.
	const auto widened = [&result, &handles]
	{
		auto clips = result.clips;

		for (size_t i = 0; i < clips.size(); ++i)
		{
			auto& clip = clips[i];
			clip.end += handles[i].tail;
			if (clip.is_photo) clip.end += handles[i].head;
			else clip.start = std::max(0.0, clip.start - handles[i].head);
		}

		return clips;
	};

	if (has_diffractor_settings)
	{
		// Movie's own transitions are in its settings; the ones in the track only say where its ranges
		// stop.
		if (ranges_stop_at_cuts) result.clips = widened();
	}
	else
	{
		// Movie draws one crossfade, the same at every cut, so another tool's dissolves are adopted only
		// when one of the same length stands at every cut, and no clip is too short to give it.
		auto adopt = transitions_stand_on_cuts && result.clips.size() > 1 &&
			transitions.size() == result.clips.size() - 1;
		const auto length = adopt ? transitions.front().in_offset + transitions.front().out_offset : 0.0;
		adopt = adopt && length <= movie_max_transition_seconds &&
			std::ranges::all_of(transitions, [length](const cut_transition& transition)
			{
				return transition.is_dissolve &&
					std::abs(transition.in_offset + transition.out_offset - length) < 0.0001;
			});

		if (adopt)
		{
			auto clips = widened();

			for (size_t i = 1; i < clips.size() && adopt; ++i)
			{
				adopt = length <= std::min(clips[i - 1].duration(), clips[i].duration()) / 2 + 0.0001;
			}

			if (adopt)
			{
				result.clips = std::move(clips);
				result.settings.transition = movie_transition::crossfade;
				result.settings.transition_seconds = length;
			}
		}

		if (!adopt) result.omit("Transition", foreign_transition_count);
	}

	for (auto& clip : result.clips)
	{
		// A still with no range of its own follows the movie's hold.
		if (clip.is_photo && clip.duration() <= 0)
		{
			clip.end = clip.start + result.settings.photo_seconds;
			clip.photo_duration_is_default = true;
		}

		bound_clip_range(clip);

		if (!std::isfinite(clip.source_duration) || clip.source_duration < 0)
		{
			clip.source_duration = clip.end;
		}
		else if (!clip.is_photo && clip.source_duration > 0 && std::abs(clip.end - clip.source_duration) <= 0.001)
		{
			// A clip running to the end of its source, saved by an earlier build with its range rounded to
			// the millisecond, would otherwise open a fraction short of it and show as trimmed.
			clip.end = clip.source_duration;
		}
	}

	result.status = result.clips.empty() && !has_diffractor_settings ? movie_load_status::empty : movie_load_status::ok;
	return result;
}

//
// Windows Live Movie Maker
//

// A tolerant scraper, not a parser. The .wlmp schema is undocumented and has never been stable
// across the tools that wrote it, so matching on exact element names would break on the next file.
// It reads attributes off any element that looks like a media item or an extent, and takes only
// two things from the document's structure: the track bound to the "Main" placeholder, which is
// the timeline, and which extent the effects and settings it meets belong to. It resolves no
// entity it was not given inline and follows no reference, so there is no external-entity surface
// here.
namespace
{
	struct xml_element
	{
		std::string_view name;
		std::vector<std::pair<std::string_view, std::string>> attributes;

		const std::string* find(const std::string_view key) const
		{
			for (const auto& [k, v] : attributes)
			{
				if (str::icmp(k, key) == 0) return &v;
			}

			return nullptr;
		}
	};

	std::string decode_entities(const std::string_view text)
	{
		if (text.find('&') == std::string_view::npos) return std::string(text);

		std::string result;
		result.reserve(text.size());

		for (size_t i = 0; i < text.size();)
		{
			if (text[i] != '&')
			{
				result += text[i++];
				continue;
			}

			// An entity is a handful of characters, so its ';' is looked for only that far. Looking to the
			// end of the text from every '&' made a run of bare ampersands cost the square of its length.
			const auto semicolon = text.substr(i, 11).find(';');

			if (semicolon == std::string_view::npos)
			{
				result += text[i++];
				continue;
			}

			const auto end = i + semicolon;
			const auto entity = text.substr(i + 1, end - i - 1);

			if (entity == "amp") result += '&';
			else if (entity == "lt") result += '<';
			else if (entity == "gt") result += '>';
			else if (entity == "quot") result += '"';
			else if (entity == "apos") result += '\'';
			else if (entity.size() > 1 && entity[0] == '#')
			{
				const auto hex = entity[1] == 'x' || entity[1] == 'X';
				const auto digits = entity.substr(hex ? 2 : 1);
				uint32_t code = 0;

				const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), code,
				                                       hex ? 16 : 10);

				// A surrogate is half of a UTF-16 pair and no character of its own; encoded alone it would
				// put a path into the timeline that no file system call can convert.
				if (ec == std::errc{} && ptr == digits.data() + digits.size() && code > 0 && code < 0x110000 &&
					(code < 0xD800 || code > 0xDFFF))
				{
					str::char32_to_utf8(std::back_inserter(result), code);
				}
			}
			else
			{
				result.append(text.substr(i, end - i + 1));
			}

			i = end + 1;
		}

		return result;
	}

	std::vector<xml_element> scan_xml(const std::string_view text)
	{
		std::vector<xml_element> result;
		size_t i = 0;

		while (i < text.size())
		{
			const auto open = text.find('<', i);
			if (open == std::string_view::npos) break;

			i = open + 1;
			if (i >= text.size()) break;

			// Declarations, comments, doctypes and closing tags carry nothing this reads.
			if (text[i] == '?' || text[i] == '!' || text[i] == '/')
			{
				const auto close = text.find('>', i);
				if (close == std::string_view::npos) break;
				i = close + 1;
				continue;
			}

			const auto name_start = i;
			while (i < text.size() && !str::is_white_space(text[i]) && text[i] != '>' && text[i] != '/') ++i;

			xml_element element;
			element.name = text.substr(name_start, i - name_start);

			while (i < text.size() && text[i] != '>')
			{
				while (i < text.size() && (str::is_white_space(text[i]) || text[i] == '/')) ++i;
				if (i >= text.size() || text[i] == '>') break;

				const auto key_start = i;
				while (i < text.size() && text[i] != '=' && text[i] != '>' && !str::is_white_space(text[i])) ++i;
				const auto key = text.substr(key_start, i - key_start);

				while (i < text.size() && str::is_white_space(text[i])) ++i;
				if (i >= text.size() || text[i] != '=') continue;
				++i;
				while (i < text.size() && str::is_white_space(text[i])) ++i;
				if (i >= text.size()) break;

				const auto quote = text[i];
				if (quote != '"' && quote != '\'') continue;

				++i;
				const auto value_start = i;
				while (i < text.size() && text[i] != quote) ++i;
				const auto value = text.substr(value_start, i - value_start);
				if (i < text.size()) ++i;

				if (!key.empty()) element.attributes.emplace_back(key, decode_entities(value));
			}

			if (i < text.size()) ++i;
			if (!element.name.empty()) result.emplace_back(std::move(element));
		}

		return result;
	}

	// from_chars rather than stod: stod reads the C locale's decimal point, and a project file's
	// numbers are always '.' whatever the user's regional settings say. A setting that is absent or
	// malformed reads as `fallback`, the value it would have had.
	double parse_number(const std::string* text, const double fallback)
	{
		if (!text || text->empty()) return fallback;

		double value = 0;
		const auto* const first = text->data();
		const auto* const last = first + text->size();
		const auto [ptr, ec] = std::from_chars(first, last, value);

		return ec != std::errc{} || !std::isfinite(value) ? fallback : value;
	}

	// A malformed time means "this attribute is absent", so the caller falls back to the clip's
	// other timings.
	double parse_seconds(const std::string* text)
	{
		return parse_number(text, -1);
	}

	bool is_named(const xml_element& element, const std::string_view name)
	{
		return str::icmp(element.name, name) == 0;
	}

	// Movie Maker ran on Windows, so a drive letter or a UNC prefix is absolute wherever the file is
	// read, and either separator divides a relative path. A relative path is how it records media
	// kept beneath the project's own folder.
	df::file_path wlmp_media_path(const std::string_view text, const df::folder_path project_folder)
	{
		const auto drive = text.size() >= 3 && text[1] == ':' && df::is_path_sep(text[2]);
		const auto unc = text.size() >= 2 && df::is_path_sep(text[0]) && df::is_path_sep(text[1]);
		if (drive || unc || is_qualified_path(text)) return df::file_path(text);
		if (project_folder.is_empty()) return {};

		auto relative = std::string(text);
		for (auto&& c : relative) if (df::is_path_sep(c)) c = df::preferred_path_sep;
		return resolve_in_project_folder(relative, project_folder);
	}

	bool is_ignored_extent(const std::string_view name)
	{
		static constexpr std::string_view ignored[] = {
			"audio", "title", "caption", "credit", "narration", "music", "text", "bounded", "sound",
			"effect", "animation"
		};

		for (const auto prefix : ignored)
		{
			if (name.size() >= prefix.size() && str::icmp(name.substr(0, prefix.size()), prefix) == 0) return true;
		}

		return false;
	}
}

movie_load_result read_wlmp(const std::string_view xml, const df::folder_path project_folder)
{
	movie_load_result result;

	const auto elements = scan_xml(xml);
	if (elements.empty()) return result;

	struct wlmp_media
	{
		df::file_path path;
		double recorded_seconds = 0;
	};

	df::hash_map<std::string, wlmp_media, df::ihash, df::ieq> media;
	const std::string* main_track = nullptr;

	for (const auto& element : elements)
	{
		if (const auto placeholder = element.find("placeholderID");
			placeholder && str::icmp(*placeholder, "Main") == 0)
		{
			main_track = element.find("extentID");
		}

		const auto id = element.find("id");
		const auto file = element.find("filePath");
		if (!id || !file || id->empty() || file->empty()) continue;

		const auto path = wlmp_media_path(*file, project_folder);
		if (path.is_empty()) continue;

		// A recorded length past the longest movie Movie renders is a broken file, not a source.
		media.insert_or_assign(*id, wlmp_media{
			                       path, std::clamp(parse_seconds(element.find("duration")), 0.0, movie_max_clip_seconds)
		                       });
	}

	if (media.empty()) return result;

	// A clip and what Movie Maker attached to it. Those count as left out only if the clip is placed:
	// a clip off the timeline is one omission, not one plus everything it carried.
	struct wlmp_clip
	{
		std::string id;
		movie_clip clip;
		double recorded_seconds = 0;
		double position = -1;
		std::vector<std::string_view> omitted;
	};

	std::vector<wlmp_clip> clips;
	std::vector<std::string_view> main_refs;
	auto has_main_track = false;

	// The scraper sees opening tags only, so an element belongs to the extent opened last. That is
	// how Movie Maker writes them: an extent, its effects, transitions and settings, then the next.
	auto owner_is_clip = false;
	auto owner_is_main_track = false;
	auto in_transitions = false;

	for (const auto& element : elements)
	{
		if (element.find("placeholderID"))
		{
			// The placeholders follow the extents, and nothing after them belongs to one.
			owner_is_clip = false;
			owner_is_main_track = false;
			continue;
		}

		if (const auto extent_id = element.find("extentID"))
		{
			owner_is_clip = false;
			owner_is_main_track = false;
			in_transitions = false;

			if (is_named(element, "ExtentSelector"))
			{
				owner_is_main_track = main_track && str::icmp(*extent_id, *main_track) == 0;
				has_main_track = has_main_track || owner_is_main_track;
				continue;
			}

			const auto reference = element.find("mediaItemID");

			if (!reference || is_ignored_extent(element.name))
			{
				result.omit(element.name);
				continue;
			}

			const auto found = media.find(*reference);

			if (found == media.end())
			{
				result.omit("clip with no media file");
				continue;
			}

			const auto mt = files::file_type_from_name(found->second.path);
			const auto photo_by_name = mt && mt->group == file_group::photo;

			movie_clip clip;
			clip.path = found->second.path;
			clip.is_photo = str::contains(element.name, "image") || str::contains(element.name, "photo") ||
				(!str::contains(element.name, "video") && photo_by_name);
			clip.photo_duration_is_default = false;

			const auto start = std::max(0.0, parse_seconds(element.find("inTime")));
			const auto out_time = parse_seconds(element.find("outTime"));
			const auto duration = parse_seconds(element.find("duration"));

			if (out_time > start)
			{
				clip.start = start;
				clip.end = out_time;
			}
			else if (duration > 0)
			{
				clip.start = start;
				clip.end = start + duration;
			}
			else if (!clip.is_photo && out_time <= 0)
			{
				// No out point, or one of zero, is how Movie Maker writes a clip that runs to the end
				// of its source. Only the probe can measure where that is, so the end is left at zero
				// as a newly added video's is, and the probe gives the clip the rest of its source.
				clip.start = start;
				clip.end = 0;
			}
			else
			{
				result.omit("clip with no usable range");
				continue;
			}

			bound_clip_range(clip);
			clip.source_duration = clip.end;

			wlmp_clip entry{
				*extent_id, std::move(clip), found->second.recorded_seconds, parse_seconds(element.find("position")), {}
			};
			if (std::abs(parse_number(element.find("speed"), 1.0) - 1.0) > 0.0001) entry.omitted.emplace_back("speed");

			clips.emplace_back(std::move(entry));
			owner_is_clip = true;
			continue;
		}

		if (owner_is_main_track && is_named(element, "ExtentRef"))
		{
			if (const auto id = element.find("id")) main_refs.emplace_back(*id);
			continue;
		}

		if (!owner_is_clip) continue;

		auto& omitted = clips.back().omitted;

		if (is_named(element, "Effects") || is_named(element, "Transitions"))
		{
			in_transitions = is_named(element, "Transitions");
			continue;
		}

		if (const auto effect = element.find("effectTemplateID"))
		{
			// Movie draws its own crossfade between every pair of clips. That stands in for Movie
			// Maker's crossfades, but not for its other transitions, and no effect has an equivalent.
			if (!in_transitions || !str::contains(*effect, "crossfade")) omitted.emplace_back(*effect);
			continue;
		}

		const auto setting = element.find("Name");
		if (!setting) continue;

		const auto value = element.find("Value");

		if (str::icmp(*setting, "rotateStepNinety") == 0 && parse_number(value, 0.0) != 0.0)
		{
			omitted.emplace_back("rotateStepNinety");
		}
		else if (str::icmp(*setting, "Mute") == 0 && value && str::icmp(*value, "true") == 0)
		{
			omitted.emplace_back("Mute");
		}
		else if (str::icmp(*setting, "Volume") == 0 && std::abs(parse_number(value, 1.0) - 1.0) > 0.0001)
		{
			omitted.emplace_back("Volume");
		}
	}

	std::vector<size_t> order;

	if (has_main_track)
	{
		// The main track is the timeline. A clip it does not place is one Movie Maker kept but no
		// longer showed; a title it places was counted where it was read.
		df::hash_map<std::string, size_t, df::ihash, df::ieq> by_id;
		for (size_t i = 0; i < clips.size(); ++i) by_id.try_emplace(clips[i].id, i);

		std::vector<bool> placed(clips.size(), false);

		for (const auto ref : main_refs)
		{
			const auto found = by_id.find(std::string(ref));
			if (found == by_id.end()) continue;

			placed[found->second] = true;
			order.emplace_back(found->second);
		}

		for (size_t i = 0; i < clips.size(); ++i)
		{
			if (!placed[i]) result.omit("clip off the main track");
		}
	}
	else
	{
		// Without a main track, a stated timeline position orders the clips, and the file's own order
		// does where there is none.
		for (size_t i = 0; i < clips.size(); ++i) order.emplace_back(i);

		if (std::any_of(clips.begin(), clips.end(), [](const wlmp_clip& clip) { return clip.position >= 0; }))
		{
			std::stable_sort(order.begin(), order.end(), [&clips](const size_t left, const size_t right)
			{
				const auto left_position = clips[left].position;
				const auto right_position = clips[right].position;
				if (left_position < 0) return false;
				if (right_position < 0) return true;
				return left_position < right_position;
			});
		}
	}

	for (const auto i : order)
	{
		for (const auto name : clips[i].omitted) result.omit(name);
		result.clips.emplace_back(clips[i].clip);
		result.recorded_seconds.emplace_back(clips[i].recorded_seconds);
	}

	result.status = result.clips.empty() ? movie_load_status::empty : movie_load_status::ok;
	return result;
}

//
// Finding moved sources
//

// How many components two paths share, counted back from the name. The candidates were found by
// name, so the name always matches and 2 means the folder holding it has the same name too.
static size_t shared_trailing_components(const std::string_view left, const std::string_view right)
{
	size_t shared = 0;
	auto left_end = left.size();
	auto right_end = right.size();

	while (left_end > 0 && right_end > 0)
	{
		const auto left_sep = left.find_last_of("\\/", left_end - 1);
		const auto right_sep = right.find_last_of("\\/", right_end - 1);
		const auto left_begin = left_sep == std::string_view::npos ? 0 : left_sep + 1;
		const auto right_begin = right_sep == std::string_view::npos ? 0 : right_sep + 1;

		if (str::icmp(left.substr(left_begin, left_end - left_begin),
		              right.substr(right_begin, right_end - right_begin)) != 0) break;

		++shared;
		if (left_sep == std::string_view::npos || right_sep == std::string_view::npos) break;
		left_end = left_sep;
		right_end = right_sep;
	}

	return shared;
}

static std::string folder_move_key(const df::folder_path from, const df::folder_path to)
{
	auto key = std::string(from.text());
	key += '\n';
	key += to.text();
	return key;
}

std::vector<movie_source_choice> choose_movie_sources(const std::vector<df::file_path>& stored,
                                                      const std::vector<std::vector<df::file_path>>& candidates,
                                                      const std::vector<double>& recorded_seconds,
                                                      const std::function<double(df::file_path)>& measure_seconds)
{
	std::vector<movie_source_choice> result(stored.size());
	// What is left to choose between once a recorded length has ruled files out.
	std::vector<std::vector<df::file_path>> remaining(stored.size());

	for (size_t i = 0; i < stored.size() && i < candidates.size(); ++i)
	{
		auto found = candidates[i];
		if (found.empty()) continue;

		// A length is a measurement of the file itself, so it rules out a namesake however well its
		// folders fit. A camera that numbers its files from zero again names a dozen clips alike,
		// and the one carrying the name may not be the one the project used. A candidate with no
		// length to measure is not evidence either way, so it stays.
		const auto recorded = i < recorded_seconds.size() ? recorded_seconds[i] : 0.0;
		auto ruled_out = false;

		if (recorded > 0 && measure_seconds)
		{
			const auto before = found.size();

			std::erase_if(found, [&measure_seconds, recorded](const df::file_path path)
			{
				const auto measured = measure_seconds(path);
				return measured > 0 && std::abs(measured - recorded) > movie_source_length_tolerance;
			});

			if (found.empty())
			{
				result[i].match = movie_source_match::other_length;
				continue;
			}

			ruled_out = found.size() < before;
		}

		if (found.size() == 1)
		{
			result[i] = {ruled_out ? movie_source_match::same_length : movie_source_match::only_name, found.front()};
			continue;
		}

		const auto original = stored[i].pack();
		size_t best = 0;
		size_t best_count = 0;
		size_t best_index = 0;

		for (size_t j = 0; j < found.size(); ++j)
		{
			const auto shared = shared_trailing_components(original, found[j].pack());

			if (shared > best)
			{
				best = shared;
				best_count = 1;
				best_index = j;
			}
			else if (shared == best)
			{
				++best_count;
			}
		}

		if (best >= 2 && best_count == 1)
		{
			result[i] = {movie_source_match::same_folders, found[best_index]};
		}
		else
		{
			result[i].match = movie_source_match::ambiguous;
			remaining[i] = std::move(found);
		}
	}

	// Files kept together are usually moved together, so every source settled above is a vote for
	// where the folder it was stored in went. Only a clear winner settles a tie between same names.
	df::hash_map<std::string, int, df::ihash, df::ieq> votes;

	for (size_t i = 0; i < result.size(); ++i)
	{
		if (!result[i].path.is_empty()) ++votes[folder_move_key(stored[i].folder(), result[i].path.folder())];
	}

	for (size_t i = 0; i < result.size(); ++i)
	{
		if (result[i].match != movie_source_match::ambiguous) continue;

		const auto& found = remaining[i];
		auto best = 0;
		size_t best_count = 0;
		size_t best_index = 0;

		for (size_t j = 0; j < found.size(); ++j)
		{
			const auto vote = votes.find(folder_move_key(stored[i].folder(), found[j].folder()));
			const auto count = vote == votes.end() ? 0 : vote->second;

			if (count > best)
			{
				best = count;
				best_count = 1;
				best_index = j;
			}
			else if (count == best)
			{
				++best_count;
			}
		}

		if (best > 0 && best_count == 1) result[i] = {movie_source_match::siblings, found[best_index]};
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

	_undo.emplace_back(undo_entry{_clips, _settings, _selected, _current, _anchor});

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

	const auto& began = _undo.back();
	if (began.clips != _clips || !(began.settings == _settings)) return;

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

	if (toggle)
	{
		const auto found = std::find(_selected.begin(), _selected.end(), index);

		if (found != _selected.end())
		{
			// The last selected clip cannot be toggled away: a command that acts on the selection
			// would then have nothing to act on while the strip still shows a focused clip.
			if (_selected.size() > 1) _selected.erase(found);
		}
		else
		{
			_selected.emplace_back(index);
			std::sort(_selected.begin(), _selected.end());
		}

		_anchor = index;
	}
	else if (extend)
	{
		_selected.clear();

		for (auto i = std::min(_anchor, index); i <= std::max(_anchor, index); ++i)
		{
			_selected.emplace_back(i);
		}
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
		return clip.is_probed && !clip.is_missing && clip.duration() > 0 &&
			clip.extent.cx > 0 && clip.extent.cy > 0;
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
	// against the clips that will still be there once the selection is lifted out.
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
	{
		return replacement.first < _clips.size() && _clips[replacement.first] != replacement.second;
	})) return;

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

void movie_project::settings(const movie_settings& s, const movie_edit_group group)
{
	// The panel reports every notification its native controls raise, including the echo of a value
	// the view itself wrote. One that changes nothing is not an edit.
	if (s == _settings) return;

	begin_edit(group);
	_settings = s;
	apply_photo_duration();
	settle_edit();
}

void movie_project::apply_photo_duration()
{
	for (auto&& c : _clips)
	{
		if (!c.is_photo || !c.photo_duration_is_default) continue;
		c.start = 0;
		c.end = _settings.photo_seconds;
	}
}

void movie_project::trim(const size_t at, const double start, const double end, const movie_edit_group group)
{
	if (at >= _clips.size()) return;

	auto c = _clips[at];

	if (c.is_photo)
	{
		c.start = 0;
		c.end = std::max(0.1, end - start);
		c.photo_duration_is_default = false;
	}
	else
	{
		const auto limit = c.source_duration > 0 ? c.source_duration : end;
		c.start = std::clamp(start, 0.0, limit);
		c.end = std::clamp(end, c.start, limit);
	}

	// A handle pressed and let go where it was is not a trim.
	if (c == _clips[at]) return;

	begin_edit(group);
	_clips[at] = c;
	settle_edit();
}

void movie_project::end_edit_group()
{
	_open_group = movie_edit_group::none;
}

void movie_project::reset(std::vector<movie_clip> clips, const movie_settings& s, const df::file_path path)
{
	_clips = std::move(clips);
	_settings = s;
	_path = path;
	_undo.clear();
	mark_clean();
	++_revision;
	++_edit_revision;
	select_only(0);
}

void movie_project::mark_saved(const df::file_path path, const uint64_t edit_revision)
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
	_settings = entry.settings;
	_selected = std::move(entry.selected);
	_current = entry.current;
	_anchor = entry.anchor;
	_modified = !_clean_depth || *_clean_depth != _undo.size();
	++_revision;
	++_edit_revision;

	// The stored selection came from a list that may have been longer than this one.
	std::erase_if(_selected, [this](const size_t i) { return i >= _clips.size(); });
	if (_selected.empty() && !_clips.empty()) select_only(std::min(_current, _clips.size() - 1));
	if (_current >= _clips.size()) _current = _clips.empty() ? 0 : _clips.size() - 1;
}

movie_entry decide_movie_entry(const movie_project& project, const std::vector<df::file_path>& selection,
                               const std::vector<df::file_path>& built_from)
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
