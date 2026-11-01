// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The .nfo files media managers - Kodi, Jellyfin, Emby - keep beside films and episodes: finding the
// ones that describe a video, reading their title, show, season, episode, year, dates, plot, genres and studio,
// and setting those over a container's tags. Read on the scanning thread, and only when the user asks.

#include "pch.h"
#include "files.h"
#include "av_format.h"
#include "util_text.h"

namespace
{
	// An .nfo is a page of text at most; a file far larger is something else under the extension.
	constexpr uint64_t max_nfo_bytes = 1024 * 1024;

	struct nfo_element
	{
		std::string_view name;
		std::string text;
	};

	std::string element_text(std::string_view content)
	{
		content = str::trim(content);

		constexpr std::string_view cdata_open = "<![CDATA[";
		constexpr std::string_view cdata_close = "]]>";

		if (content.starts_with(cdata_open) && content.ends_with(cdata_close))
		{
			return std::string(content.substr(cdata_open.size(), content.size() - cdata_open.size() - cdata_close.size()));
		}

		return str::decode_xml_entities(content);
	}

	// The root's name and the text of each element directly inside it, in order. An element holding others - an
	// actor's name and role, a set of ratings - is passed over whole. Only the first root is read: a file holding
	// two episodes describes the first.
	std::pair<std::string_view, std::vector<nfo_element>> read_nfo_elements(const std::string_view text)
	{
		std::string_view root;
		std::vector<nfo_element> children;
		std::string_view child;
		auto child_holds_elements = false;
		size_t content_start = 0;
		auto depth = 0;
		size_t i = 0;

		while (i < text.size())
		{
			const auto open = text.find('<', i);
			if (open == std::string_view::npos) break;

			const auto skip_to = [&](const std::string_view end_marker, const size_t from)
			{
				const auto end = text.find(end_marker, from);
				i = end == std::string_view::npos ? text.size() : end + end_marker.size();
			};

			if (text.substr(open, 9) == "<![CDATA[")
			{
				skip_to("]]>", open + 9);
				continue;
			}

			if (text.substr(open, 4) == "<!--")
			{
				skip_to("-->", open + 4);
				continue;
			}

			const auto close = text.find('>', open + 1);
			if (close == std::string_view::npos) break;

			const auto tag = text.substr(open + 1, close - open - 1);
			i = close + 1;

			if (tag.empty() || tag.front() == '?' || tag.front() == '!') continue;

			if (tag.front() == '/')
			{
				--depth;

				if (depth == 1 && !child.empty())
				{
					if (!child_holds_elements) children.push_back({child, element_text(text.substr(content_start, open - content_start))});
					child = {};
				}

				if (depth <= 0) break;
				continue;
			}

			const auto self_closing = tag.back() == '/';
			const auto name = tag.substr(0, std::min(tag.find_first_of(" \t\r\n/"), tag.size()));
			if (name.empty()) continue;

			if (depth == 0)
			{
				root = name;
				if (self_closing) break;
				depth = 1;
			}
			else if (depth == 1)
			{
				if (self_closing) continue;
				child = name;
				child_holds_elements = false;
				content_start = i;
				depth = 2;
			}
			else
			{
				child_holds_elements = true;
				if (!self_closing) ++depth;
			}
		}

		return {root, std::move(children)};
	}

	int to_int(const std::string_view s, const int fallback)
	{
		const auto trimmed = str::trim(s);
		auto value = fallback;
		const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
		return ec == std::errc{} ? value : fallback;
	}

	// 2019-05-01, as every media manager writes a date; or the year alone.
	df::date_t to_date(const std::string_view s)
	{
		const auto parts = str::split(str::trim(s), false, [](const char c) { return c == '-' || c == '/'; });
		if (parts.empty()) return {};

		const auto year = to_int(parts[0], 0);
		const auto month = parts.size() > 1 ? to_int(parts[1], 0) : 0;
		const auto day = parts.size() > 2 ? to_int(parts[2], 0) : 0;

		if (year <= 1800 || year >= 2100) return {};
		if (month < 1 || month > 12 || day < 1 || day > 31) return df::date_t(year, 1, 1, 0, 0, 0);

		const df::date_t result(year, month, day, 0, 0, 0);
		return result.is_valid() ? result : df::date_t(year, 1, 1, 0, 0, 0);
	}

	std::string read_nfo_text(const df::file_path path)
	{
		const auto f = platform::open_file(path, platform::file_open_mode::read);
		if (!f) return {};

		const auto len = f->size();
		if (len == 0 || len > max_nfo_bytes) return {};

		std::string text(static_cast<size_t>(len), '\0');
		if (f->read(std::bit_cast<uint8_t*>(text.data()), len) != len) return {};

		if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
		// Media managers write UTF-8; an older one, or an .nfo edited by hand, may be in the system's code page.
		return subtitle_text_as_utf8(text);
	}

	nfo_details read_nfo_file(const df::file_path path)
	{
		if (path.is_empty() || !platform::exists(path)) return {};
		return parse_nfo(read_nfo_text(path));
	}

	// The show a season of episodes belongs to, from the tvshow.nfo in its folder or the one above. Remembered
	// briefly, so a season scanned episode by episode reads it once.
	std::string show_title(const df::folder_path folder)
	{
		static platform::mutex cache_mutex;
		static df::folder_path cached_folder;
		static std::string cached_title;
		static int64_t cached_ms = 0;
		constexpr int64_t cache_ms = 60 * 1000;

		const auto now = df::now_ms();

		{
			platform::exclusive_lock lock(cache_mutex);
			if (cached_folder == folder && now - cached_ms < cache_ms) return cached_title;
		}

		std::string title;

		for (const auto candidate : {folder, folder.parent()})
		{
			if (candidate.is_empty()) continue;

			const auto show = read_nfo_file(candidate.combine_file("tvshow.nfo"));

			if (show.type == nfo_details::kind::show && !show.title.empty())
			{
				title = show.title;
				break;
			}
		}

		platform::exclusive_lock lock(cache_mutex);
		cached_folder = folder;
		cached_title = title;
		cached_ms = now;
		return title;
	}
}

bool is_named_for_its_folder(const std::string_view stem, const std::string_view folder)
{
	const auto film = scan_info_from_title(stem);
	const auto named = scan_info_from_title(folder);
	if (film.title.empty() || str::icmp(film.title, named.title) != 0) return false;
	return film.year == 0 || named.year == 0 || film.year == named.year;
}

nfo_details parse_nfo(const std::string_view text)
{
	nfo_details result;
	const auto [root, elements] = read_nfo_elements(text);

	if (str::icmp(root, "movie") == 0) result.type = nfo_details::kind::film;
	else if (str::icmp(root, "episodedetails") == 0) result.type = nfo_details::kind::episode;
	else if (str::icmp(root, "tvshow") == 0) result.type = nfo_details::kind::show;
	else if (str::icmp(root, "musicvideo") == 0) result.type = nfo_details::kind::music_video;
	else return result;

	std::vector<std::string> genres;
	std::string outline;

	for (const auto& [name, value] : elements)
	{
		if (value.empty()) continue;

		if (str::icmp(name, "title") == 0 && result.title.empty()) result.title = value;
		else if (str::icmp(name, "showtitle") == 0 && result.show.empty()) result.show = value;
		else if (str::icmp(name, "plot") == 0 && result.plot.empty()) result.plot = value;
		else if (str::icmp(name, "outline") == 0 && outline.empty()) outline = value;
		else if (str::icmp(name, "genre") == 0) genres.emplace_back(value);
		else if (str::icmp(name, "studio") == 0 && result.studio.empty()) result.studio = value;
		else if (str::icmp(name, "year") == 0) result.year = std::clamp(to_int(value, 0), 0, 9999);
		else if (str::icmp(name, "season") == 0) result.season = to_int(value, -1);
		else if (str::icmp(name, "episode") == 0) result.episode = to_int(value, -1);
		else if ((str::icmp(name, "premiered") == 0 || str::icmp(name, "aired") == 0) && !result.released.is_valid())
			result.released = to_date(value);
	}

	if (result.plot.empty()) result.plot = std::move(outline);
	if (result.year <= 1800 || result.year >= 2100) result.year = result.released.is_valid() ? result.released.year() : 0;
	if (result.season < 0 || result.season > 9999) result.season = -1;
	// Episodes are kept in 16 bits.
	if (result.episode < 0 || result.episode > 32767) result.episode = -1;

	// A show's .nfo names the show as its title.
	if (result.type == nfo_details::kind::show) result.show = result.title;

	result.genre = str::combine(genres, ", ", false);
	return result;
}

nfo_details read_video_nfo(const df::file_path video)
{
	const auto folder = video.folder();
	const auto stem = video.file_name_without_extension();

	auto result = read_nfo_file(folder.combine_file(std::string(stem) + ".nfo"));

	if (result.is_empty() && is_named_for_its_folder(stem, folder.name()))
	{
		result = read_nfo_file(folder.combine_file("movie.nfo"));
		if (result.type != nfo_details::kind::film) result = {};
	}

	// An episode's own .nfo may leave its show to the show's; an episode with none takes its show from there too.
	const auto is_episode = result.type == nfo_details::kind::episode ||
		(result.is_empty() && scan_info_from_title(stem).has_episode);

	if (is_episode && result.show.empty())
	{
		auto show = show_title(folder);

		if (!show.empty())
		{
			if (result.is_empty()) result.type = nfo_details::kind::episode;
			result.show = std::move(show);
		}
	}

	return result;
}

void apply_nfo(prop::item_metadata& md, const nfo_details& nfo)
{
	if (nfo.is_empty() || nfo.type == nfo_details::kind::show) return;

	if (!nfo.title.empty()) md.title = str::strip_and_cache(nfo.title);
	if (!nfo.plot.empty()) md.synopsis = str::strip_and_cache(nfo.plot);
	if (!nfo.genre.empty()) md.genre = str::strip_and_cache(nfo.genre);
	if (!nfo.studio.empty()) md.publisher = str::strip_and_cache(nfo.studio);
	if (nfo.year != 0) md.year = static_cast<uint16_t>(nfo.year);

	if (nfo.type == nfo_details::kind::episode)
	{
		if (!nfo.show.empty()) md.show = str::strip_and_cache(nfo.show);
		if (nfo.season >= 0) md.season = static_cast<uint16_t>(nfo.season);
		if (nfo.episode > 0) md.episode = prop::number_pair(nfo.episode, 0);
	}

	// A music video keeps its year but adds no date: that date is what marks a film the .nfo described.
	if (nfo.type == nfo_details::kind::music_video) return;

	if (nfo.released.is_valid()) md.dates.add(prop::date_source::media_manager, nfo.released);
	else if (nfo.year != 0) md.dates.add(prop::date_source::media_manager, df::date_t(nfo.year, 1, 1, 0, 0, 0));
}
