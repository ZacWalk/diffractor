// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Subtitles: reading ASS scripts and dialogue as FFmpeg's decoders give them, placing and
// rasterising events for the picture as it is drawn, one picture at a time from an image stream, the event
// store a playing session keeps, and the choice of track from a video's streams and sidecars.

#include "pch.h"
#include "av_subtitle.h"
#include "files.h"

namespace
{
	std::vector<std::string_view> split_fields(std::string_view s, const char separator, const size_t max_fields)
	{
		std::vector<std::string_view> result;

		while (result.size() + 1 < max_fields)
		{
			const auto at = s.find(separator);
			if (at == std::string_view::npos) break;
			result.emplace_back(s.substr(0, at));
			s = s.substr(at + 1);
		}

		result.emplace_back(s);
		return result;
	}

	int to_int(const std::string_view s, const int fallback = 0)
	{
		const auto trimmed = str::trim(s);
		auto value = fallback;
		const auto* const first = trimmed.data();
		const auto* const last = trimmed.data() + trimmed.size();
		std::from_chars(first + (trimmed.starts_with('+') ? 1 : 0), last, value);
		return value;
	}

	double to_double(const std::string_view s, const double fallback = 0)
	{
		const auto trimmed = str::trim(s);
		auto value = fallback;
		std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
		return value;
	}

	// &HAABBGGRR, &HBBGGRR or a decimal, with ASS's alpha where 0 is opaque, as 0xAABBGGRR straight alpha.
	uint32_t ass_color(std::string_view s, const uint32_t fallback)
	{
		s = str::trim(s);
		while (!s.empty() && (s.back() == '&' || s.back() == 'H' || s.back() == 'h')) s.remove_suffix(1);

		uint32_t value = 0;
		std::from_chars_result r{};

		if (s.size() >= 2 && s[0] == '&' && (s[1] == 'H' || s[1] == 'h'))
		{
			s.remove_prefix(2);
			r = std::from_chars(s.data(), s.data() + s.size(), value, 16);
		}
		else
		{
			int64_t decimal = 0;
			const auto rd = std::from_chars(s.data(), s.data() + s.size(), decimal);
			if (rd.ec != std::errc()) return fallback;
			value = static_cast<uint32_t>(decimal);
			r.ec = rd.ec;
		}

		if (r.ec != std::errc()) return fallback;

		const auto ass_alpha = value >> 24;
		return (value & 0x00FFFFFFu) | (255u - ass_alpha) << 24;
	}

	bool ass_bool(const std::string_view s)
	{
		return to_int(s) != 0;
	}

	// SSA's \a numbers its alignments 1-3 along the bottom, +4 for the top and +8 for the middle.
	int legacy_alignment(const int a)
	{
		const auto column = ((a - 1) & 3) + 1;
		if (a & 4) return column + 6;
		if (a & 8) return column + 3;
		return column;
	}
}

const subtitle_style& subtitle_script::style(const std::string_view name) const
{
	static const subtitle_style fallback;

	for (const auto& s : styles)
	{
		if (str::icmp(s.name, name) == 0) return s;
	}

	// "*Default" is how some scripts name the style a line falls back to.
	for (const auto& s : styles)
	{
		if (str::icmp(s.name, "Default") == 0) return s;
	}

	return styles.empty() ? fallback : styles.front();
}

subtitle_script parse_ass_header(const std::string_view header)
{
	subtitle_script result;
	std::vector<std::string_view> format;
	auto in_styles = false;
	// SSA's [V4 Styles] number alignments as \a does; ASS's [V4+ Styles] as a keypad.
	auto legacy = false;

	for (auto line : str::split(header, false, [](const char c) { return c == '\n' || c == '\r'; }))
	{
		line = str::trim(line);
		if (line.empty()) continue;

		if (line.front() == '[')
		{
			legacy = str::icmp(line, "[V4 Styles]") == 0;
			in_styles = legacy || str::icmp(line, "[V4+ Styles]") == 0;
			continue;
		}

		const auto colon = line.find(':');
		if (colon == std::string_view::npos) continue;

		const auto key = str::trim(line.substr(0, colon));
		const auto value = str::trim(line.substr(colon + 1));

		if (str::icmp(key, "PlayResX") == 0) result.play_res_x = std::max(1, to_int(value, 384));
		else if (str::icmp(key, "PlayResY") == 0) result.play_res_y = std::max(1, to_int(value, 288));
		else if (in_styles && str::icmp(key, "Format") == 0)
		{
			format.clear();
			for (const auto f : split_fields(value, ',', 64)) format.emplace_back(str::trim(f));
		}
		else if (in_styles && str::icmp(key, "Style") == 0)
		{
			subtitle_style style;
			const auto fields = split_fields(value, ',', std::max<size_t>(format.size(), 1));

			for (size_t i = 0; i < fields.size() && i < format.size(); ++i)
			{
				const auto name = format[i];
				const auto field = str::trim(fields[i]);

				if (str::icmp(name, "Name") == 0) style.name = field;
				else if (str::icmp(name, "Fontname") == 0) style.font = field;
				else if (str::icmp(name, "Fontsize") == 0) style.size = std::max(1.0, to_double(field, 16));
				else if (str::icmp(name, "PrimaryColour") == 0) style.primary = ass_color(field, style.primary);
				else if (str::icmp(name, "OutlineColour") == 0) style.outline_color = ass_color(field, style.outline_color);
				else if (str::icmp(name, "Bold") == 0) style.bold = ass_bool(field);
				else if (str::icmp(name, "Italic") == 0) style.italic = ass_bool(field);
				else if (str::icmp(name, "Outline") == 0) style.outline = std::max(0.0, to_double(field, 1));
				else if (str::icmp(name, "Alignment") == 0)
				{
					const auto a = to_int(field, 2);
					if (legacy && a >= 1 && a <= 11) style.alignment = legacy_alignment(a);
					else if (!legacy && a >= 1 && a <= 9) style.alignment = a;
				}
				else if (str::icmp(name, "MarginL") == 0) style.margin_l = to_int(field, 10);
				else if (str::icmp(name, "MarginR") == 0) style.margin_r = to_int(field, 10);
				else if (str::icmp(name, "MarginV") == 0) style.margin_v = to_int(field, 10);
			}

			result.styles.emplace_back(std::move(style));
		}
	}

	return result;
}

ass_event_fields split_ass_event(const std::string_view ass)
{
	ass_event_fields result;
	const auto fields = split_fields(ass, ',', 9);

	if (fields.size() < 9)
	{
		result.text = ass;
		return result;
	}

	result.style = str::trim(fields[2]);
	result.margin_l = to_int(fields[4]);
	result.margin_r = to_int(fields[5]);
	result.margin_v = to_int(fields[6]);
	result.text = fields[8];
	return result;
}

std::string subtitle_text::plain_text() const
{
	std::string result;
	for (const auto& r : runs) result += r.text;
	return result;
}

subtitle_text parse_ass_dialogue(const std::string_view text, const subtitle_style& style)
{
	subtitle_text result;
	result.font = style.font;
	result.size = style.size;
	result.outline = style.outline;
	result.outline_color = style.outline_color;
	result.alignment = style.alignment;
	result.margin_l = style.margin_l;
	result.margin_r = style.margin_r;
	result.margin_v = style.margin_v;

	platform::text_run current;
	current.bold = style.bold;
	current.italic = style.italic;
	current.color = style.primary;
	auto drawing = false;

	const auto flush = [&]
	{
		if (!current.text.empty())
		{
			if (!result.runs.empty() && result.runs.back().bold == current.bold &&
				result.runs.back().italic == current.italic && result.runs.back().color == current.color)
			{
				result.runs.back().text += current.text;
			}
			else
			{
				result.runs.emplace_back(current);
			}

			current.text.clear();
		}
	};

	// An override's argument: a number, or what its parentheses hold.
	const auto apply_override = [&](std::string_view tag)
	{
		const auto starts = [&tag](const std::string_view prefix)
		{
			if (!tag.starts_with(prefix)) return false;
			tag.remove_prefix(prefix.size());
			return true;
		};

		if (starts("pos("))
		{
			const auto close = tag.find(')');
			const auto args = split_fields(tag.substr(0, close), ',', 2);
			if (args.size() == 2) result.position = pointd(to_double(args[0]), to_double(args[1]));
		}
		else if (starts("an"))
		{
			const auto a = to_int(tag);
			if (a >= 1 && a <= 9) result.alignment = a;
		}
		else if (starts("1a") || starts("alpha"))
		{
			// One byte, where ASS's 00 is opaque.
			auto digits = str::trim(tag);
			while (!digits.empty() && (digits.back() == '&' || digits.back() == 'H' || digits.back() == 'h')) digits.remove_suffix(1);
			if (digits.size() >= 2 && digits[0] == '&' && (digits[1] == 'H' || digits[1] == 'h')) digits.remove_prefix(2);
			uint32_t v = 0;
			if (std::from_chars(digits.data(), digits.data() + digits.size(), v, 16).ec == std::errc())
				current.color = (current.color & 0x00FFFFFFu) | (255u - (v & 0xFFu)) << 24;
		}
		else if (starts("a"))
		{
			if (!tag.empty() && std::isdigit(static_cast<unsigned char>(tag.front())))
			{
				const auto a = to_int(tag);
				if (a >= 1 && a <= 11) result.alignment = legacy_alignment(a);
			}
		}
		else if (starts("1c") || starts("c"))
		{
			const auto rgb = ass_color(tag, current.color) & 0x00FFFFFFu;
			current.color = (current.color & 0xFF000000u) | rgb;
		}
		else if (starts("b"))
		{
			if (tag.empty()) current.bold = style.bold;
			else if (std::isdigit(static_cast<unsigned char>(tag.front()))) current.bold = to_int(tag) != 0;
		}
		else if (starts("i"))
		{
			if (tag.empty()) current.italic = style.italic;
			else if (std::isdigit(static_cast<unsigned char>(tag.front()))) current.italic = to_int(tag) != 0;
		}
		else if (starts("p"))
		{
			if (!tag.empty() && std::isdigit(static_cast<unsigned char>(tag.front()))) drawing = to_int(tag) != 0;
		}
		else if (tag == "r" || starts("r"))
		{
			current.bold = style.bold;
			current.italic = style.italic;
			current.color = style.primary;
		}
	};

	for (size_t i = 0; i < text.size();)
	{
		const auto c = text[i];

		if (c == '{')
		{
			const auto close = text.find('}', i + 1);

			if (close == std::string_view::npos)
			{
				if (!drawing) current.text += text.substr(i);
				break;
			}

			flush();

			// Overrides are separated by backslashes; a parenthesised argument may hold more.
			const auto block = text.substr(i + 1, close - i - 1);
			size_t at = 0;

			while (at < block.size())
			{
				const auto slash = block.find('\\', at);
				if (slash == std::string_view::npos) break;

				auto end = slash + 1;
				auto depth = 0;

				while (end < block.size() && (block[end] != '\\' || depth > 0))
				{
					if (block[end] == '(') ++depth;
					else if (block[end] == ')') --depth;
					++end;
				}

				apply_override(str::trim(block.substr(slash + 1, end - slash - 1)));
				at = end;
			}

			i = close + 1;
			continue;
		}

		if (c == '\\' && i + 1 < text.size())
		{
			const auto next = text[i + 1];

			if (next == 'N' || next == 'n')
			{
				if (!drawing) current.text += '\n';
				i += 2;
				continue;
			}

			if (next == 'h')
			{
				if (!drawing) current.text += "\xC2\xA0";
				i += 2;
				continue;
			}
		}

		if (!drawing) current.text += c;
		++i;
	}

	flush();

	// Trailing breaks would only add an empty line under the dialogue.
	while (!result.runs.empty())
	{
		auto& last = result.runs.back().text;
		while (!last.empty() && (last.back() == '\n' || last.back() == ' ')) last.pop_back();
		if (!last.empty()) break;
		result.runs.pop_back();
	}

	return result;
}

ui::surface_ptr subtitle_image_surface(const subtitle_image& image)
{
	const auto w = image.rect.width();
	const auto h = image.rect.height();
	if (w <= 0 || h <= 0 || image.indices.size() < static_cast<size_t>(w) * h) return {};

	auto surface = std::make_shared<ui::surface>();
	if (!surface->alloc(w, h, ui::texture_format::ARGB, ui::orientation::top_left)) return {};

	for (auto y = 0; y < h; ++y)
	{
		auto* const dst = surface->pixels_line(y);
		const auto* const src = image.indices.data() + static_cast<size_t>(y) * w;

		for (auto x = 0; x < w; ++x)
		{
			const auto index = src[x];
			const auto c = index < image.palette.size() ? image.palette[index] : 0u;
			auto* const p = dst + static_cast<ptrdiff_t>(x) * 4;
			p[0] = static_cast<uint8_t>(c & 0xFF);
			p[1] = static_cast<uint8_t>(c >> 8 & 0xFF);
			p[2] = static_cast<uint8_t>(c >> 16 & 0xFF);
			p[3] = static_cast<uint8_t>(c >> 24 & 0xFF);
		}
	}

	return surface;
}

rectd place_subtitle(const sizei video, const sizei script_canvas, const sized block, const int alignment,
                     const int margin_l, const int margin_r, const int margin_v, const std::optional<pointd> position)
{
	const auto sx = script_canvas.cx > 0 ? static_cast<double>(video.cx) / script_canvas.cx : 1.0;
	const auto sy = script_canvas.cy > 0 ? static_cast<double>(video.cy) / script_canvas.cy : 1.0;
	const auto column = (std::clamp(alignment, 1, 9) - 1) % 3; // 0 left, 1 centre, 2 right
	const auto row = (std::clamp(alignment, 1, 9) - 1) / 3; // 0 bottom, 1 middle, 2 top

	double x;
	double y;

	if (position)
	{
		// The position is the point the alignment names on the block: its bottom centre for 2, its top
		// left for 7.
		const auto px = position->X * sx;
		const auto py = position->Y * sy;
		x = column == 0 ? px : column == 1 ? px - block.Width / 2 : px - block.Width;
		y = row == 0 ? py - block.Height : row == 1 ? py - block.Height / 2 : py;
	}
	else
	{
		const auto left = margin_l * sx;
		const auto right = video.cx - margin_r * sx;
		x = column == 0 ? left : column == 1 ? (left + right - block.Width) / 2 : right - block.Width;
		y = row == 0 ? video.cy - margin_v * sy - block.Height : row == 1 ? (video.cy - block.Height) / 2 : margin_v * sy;
	}

	// Kept on the picture: a block wider than the picture keeps its left edge.
	x = std::clamp(x, 0.0, std::max(0.0, video.cx - block.Width));
	y = std::clamp(y, 0.0, std::max(0.0, video.cy - block.Height));
	return {x, y, block.Width, block.Height};
}

std::vector<subtitle_placement> place_subtitles_on_picture(const std::vector<subtitle_event>& shown, const sizei video,
                                                           const recti picture, const int clear_above,
                                                           const double raise, const int gap)
{
	std::vector<subtitle_placement> result;
	if (video.cx <= 0 || video.cy <= 0 || picture.is_empty()) return result;

	struct placed
	{
		const subtitle_bitmap* bitmap = nullptr;
		rectd bounds;
		bool lower = false;
	};

	std::vector<placed> layout;
	const auto middle = video.cy / 2.0;

	for (const auto& e : shown)
	{
		// An event's own blocks never cover each other; only a later event's can cover them.
		const auto first = layout.size();

		for (const auto& b : e.bitmaps)
		{
			if (b.surface) layout.push_back({&b, b.bounds, b.bounds.Y + b.bounds.Height / 2 >= middle});
		}

		for (auto i = first; i < layout.size(); ++i)
		{
			auto& p = layout[i];

			for (auto attempt = 0; attempt < 8; ++attempt)
			{
				const auto earlier = std::span(layout).first(first);
				const auto hit = std::ranges::find_if(earlier, [&p](const placed& e) { return e.bounds.intersects(p.bounds); });
				if (hit == earlier.end()) break;
				p.bounds.Y = p.lower ? hit->bounds.Y - p.bounds.Height : hit->bounds.bottom();
			}
		}
	}

	const auto sx = static_cast<double>(picture.width()) / video.cx;
	const auto sy = static_cast<double>(picture.height()) / video.cy;
	const auto to_view = [&](const rectd r)
	{
		return rectd(picture.left + r.X * sx, picture.top + r.Y * sy, r.Width * sx, r.Height * sy);
	};

	auto lift = 0.0;

	if (clear_above > 0 && raise > 0)
	{
		auto lowest = 0.0;
		for (const auto& p : layout) if (p.lower) lowest = std::max(lowest, to_view(p.bounds).bottom());
		lift = std::max(0.0, lowest + gap - clear_above) * std::min(raise, 1.0);
	}

	for (const auto& p : layout)
	{
		auto r = to_view(p.bounds);
		if (p.lower) r.Y -= lift;
		result.push_back({p.bitmap, r.round()});
	}

	return result;
}

std::vector<subtitle_bitmap> render_subtitle_text(const subtitle_text& text, const subtitle_script& script,
                                                  const sizei video)
{
	if (text.runs.empty() || video.cx <= 0 || video.cy <= 0) return {};

	const auto sx = static_cast<double>(video.cx) / script.play_res_x;
	const auto sy = static_cast<double>(video.cy) / script.play_res_y;
	const auto column = (std::clamp(text.alignment, 1, 9) - 1) % 3;

	platform::styled_text spec;
	spec.runs = text.runs;
	spec.font_family = text.font;
	spec.font_px = static_cast<float>(text.size * sy);
	spec.outline_px = static_cast<float>(std::max(1.0, text.outline * sy));
	spec.outline_color = text.outline_color;
	spec.alignment = column;
	spec.wrap_width = std::max(1, static_cast<int>(video.cx - (text.margin_l + text.margin_r) * sx));

	auto surface = platform::render_styled_text(spec);
	if (!surface) return {};

	const auto block = sized(surface->width(), surface->height());
	const auto bounds = place_subtitle(video, {script.play_res_x, script.play_res_y}, block, text.alignment,
	                                   text.margin_l, text.margin_r, text.margin_v, text.position);
	return {{std::move(surface), bounds}};
}

std::vector<subtitle_bitmap> render_subtitle_images(const std::vector<subtitle_image>& images, const sizei canvas,
                                                    const sizei video)
{
	std::vector<subtitle_bitmap> result;
	if (video.cx <= 0 || video.cy <= 0) return result;

	// A canvas the stream does not state is the video's.
	const auto from = canvas.cx > 0 && canvas.cy > 0 ? canvas : video;
	const auto sx = static_cast<double>(video.cx) / from.cx;
	const auto sy = static_cast<double>(video.cy) / from.cy;

	for (const auto& image : images)
	{
		if (auto surface = subtitle_image_surface(image))
		{
			const rectd bounds(image.rect.left * sx, image.rect.top * sy, image.rect.width() * sx,
			                   image.rect.height() * sy);
			result.push_back({std::move(surface), bounds});
		}
	}

	return result;
}

void subtitle_events::reset(subtitle_script script, const sizei video)
{
	platform::exclusive_lock lock(_mutex);
	_events.clear();
	_script = std::move(script);
	_video = video;
}

void subtitle_events::add(subtitle_event e)
{
	platform::exclusive_lock lock(_mutex);
	e.id = _next_id++;

	// Repeats are common: a packet read again after a seek lands in the generation that replaced it.
	for (const auto& existing : _events)
	{
		if (existing.gen == e.gen && df::equiv(existing.start, e.start) && existing.text.has_value() == e.text.has_value() &&
			(!e.text || existing.text->plain_text() == e.text->plain_text()) && existing.images.size() == e.images.size())
		{
			return;
		}
	}

	if (!e.images.empty())
	{
		for (auto& existing : _events)
		{
			if (existing.images.empty()) continue;
			if (existing.start < e.start && existing.end > e.start) existing.end = e.start;
			// Read after the picture that replaced it, as a packet can be.
			else if (existing.start > e.start && existing.start < e.end) e.end = existing.start;
		}
	}

	const auto at = std::ranges::upper_bound(_events, e.start, {}, &subtitle_event::start);
	_events.insert(at, std::move(e));
}

void subtitle_events::add_all(std::vector<subtitle_event> events)
{
	platform::exclusive_lock lock(_mutex);

	for (auto& e : events)
	{
		e.id = _next_id++;
		_events.emplace_back(std::move(e));
	}

	std::ranges::stable_sort(_events, {}, &subtitle_event::start);
}

void subtitle_events::drop_older_than(const int gen)
{
	platform::exclusive_lock lock(_mutex);
	std::erase_if(_events, [gen](const subtitle_event& e) { return e.gen != 0 && e.gen < gen; });
}

void subtitle_events::layout(const sizei video)
{
	platform::exclusive_lock lock(_mutex);
	if (_video == video) return;

	_video = video;
	for (auto& e : _events) e.bitmaps.clear();
}

sizei subtitle_events::layout() const
{
	platform::exclusive_lock lock(_mutex);
	return _video;
}

void subtitle_events::end_open(const double t)
{
	platform::exclusive_lock lock(_mutex);

	for (auto& e : _events)
	{
		if (!e.images.empty() && e.start < t && e.end > t) e.end = t;
	}
}

void subtitle_events::prepare(const double pos)
{
	// Rasterised a little ahead, so a line is ready when it starts; released once well behind, or far
	// ahead after a seek back.
	constexpr auto ahead = 8.0;
	constexpr auto behind = 2.0;

	struct work
	{
		uint64_t id = 0;
		std::optional<subtitle_text> text;
		std::vector<subtitle_image> images;
		sizei canvas;
	};

	std::vector<work> todo;
	subtitle_script script;
	sizei video;

	{
		platform::exclusive_lock lock(_mutex);
		script = _script;
		video = _video;

		for (auto& e : _events)
		{
			const auto near = e.start <= pos + ahead && e.end >= pos - behind;

			if (!near)
			{
				e.bitmaps.clear();
			}
			else if (e.bitmaps.empty())
			{
				todo.push_back({e.id, e.text, e.images, e.canvas});
			}
		}

		// An embedded stream adds events as it is read, so the ones long past are dropped as well as their
		// bitmaps; a sidecar's are all needed again after a seek back, and are kept as text.
		std::erase_if(_events, [pos](const subtitle_event& e) { return e.gen != 0 && e.end < pos - 60.0; });
	}

	if (todo.empty()) return;

	std::vector<std::pair<uint64_t, std::vector<subtitle_bitmap>>> done;

	for (const auto& w : todo)
	{
		auto bitmaps = w.text ? render_subtitle_text(*w.text, script, video) : render_subtitle_images(w.images, w.canvas, video);
		done.emplace_back(w.id, std::move(bitmaps));
	}

	platform::exclusive_lock lock(_mutex);

	// Laid out for a size the picture has left since: dropped, and laid out again on the next pass.
	if (_video != video) return;

	for (auto& [id, bitmaps] : done)
	{
		const auto found = std::ranges::find(_events, id, &subtitle_event::id);
		if (found != _events.end() && found->bitmaps.empty()) found->bitmaps = std::move(bitmaps);
	}
}

std::vector<subtitle_event> subtitle_events::at(const double pos, const int gen, const bool forced_only) const
{
	std::vector<subtitle_event> result;
	platform::exclusive_lock lock(_mutex);

	for (const auto& e : _events)
	{
		if (e.start > pos) break;
		if (pos >= e.end || e.bitmaps.empty()) continue;
		if (e.gen != 0 && e.gen != gen) continue;
		if (forced_only && !e.forced) continue;

		subtitle_event shown;
		shown.start = e.start;
		shown.end = e.end;
		shown.id = e.id;
		shown.forced = e.forced;
		shown.bitmaps = e.bitmaps;
		result.emplace_back(std::move(shown));
	}

	return result;
}

static uint64_t combine_shown_key(const uint64_t key, const uint64_t id)
{
	return key * 0x100000001B3ull ^ id;
}

uint64_t subtitle_events::shown_key(const double pos, const int gen, const bool forced_only) const
{
	uint64_t key = 0;
	platform::exclusive_lock lock(_mutex);

	for (const auto& e : _events)
	{
		if (e.start > pos) break;
		if (pos >= e.end || e.bitmaps.empty()) continue;
		if (e.gen != 0 && e.gen != gen) continue;
		if (forced_only && !e.forced) continue;
		key = combine_shown_key(key, e.id);
	}

	return key;
}

uint64_t subtitle_shown_key(const std::vector<subtitle_event>& shown)
{
	uint64_t key = 0;
	for (const auto& e : shown) key = combine_shown_key(key, e.id);
	return key;
}

void end_replaced_pictures(std::vector<subtitle_event>& events)
{
	std::ranges::stable_sort(events, {}, &subtitle_event::start);
	subtitle_event* on_show = nullptr;

	for (auto& e : events)
	{
		if (e.text) continue;
		if (on_show && on_show->end > e.start) on_show->end = e.start;
		on_show = e.images.empty() ? nullptr : &e;
	}

	std::erase_if(events, [](const subtitle_event& e) { return !e.text && e.images.empty(); });
}

size_t subtitle_events::size() const
{
	platform::exclusive_lock lock(_mutex);
	return _events.size();
}

namespace
{
	struct language_names
	{
		std::string_view iso1;
		std::string_view iso2b;
		std::string_view iso2t;
		std::string_view name;
	};

	constexpr language_names languages[] = {
		{"en", "eng", "eng", "english"}, {"de", "ger", "deu", "german"}, {"fr", "fre", "fra", "french"},
		{"es", "spa", "spa", "spanish"}, {"it", "ita", "ita", "italian"}, {"pt", "por", "por", "portuguese"},
		{"nl", "dut", "nld", "dutch"}, {"sv", "swe", "swe", "swedish"}, {"da", "dan", "dan", "danish"},
		{"no", "nor", "nor", "norwegian"}, {"nb", "nob", "nob", "bokmal"}, {"fi", "fin", "fin", "finnish"},
		{"pl", "pol", "pol", "polish"}, {"cs", "cze", "ces", "czech"}, {"sk", "slo", "slk", "slovak"},
		{"ru", "rus", "rus", "russian"}, {"uk", "ukr", "ukr", "ukrainian"}, {"hu", "hun", "hun", "hungarian"},
		{"tr", "tur", "tur", "turkish"}, {"el", "gre", "ell", "greek"}, {"he", "heb", "heb", "hebrew"},
		{"ar", "ara", "ara", "arabic"}, {"hi", "hin", "hin", "hindi"}, {"ja", "jpn", "jpn", "japanese"},
		{"ko", "kor", "kor", "korean"}, {"zh", "chi", "zho", "chinese"}, {"th", "tha", "tha", "thai"},
		{"vi", "vie", "vie", "vietnamese"}, {"id", "ind", "ind", "indonesian"}, {"ms", "may", "msa", "malay"},
		{"ro", "rum", "ron", "romanian"}, {"bg", "bul", "bul", "bulgarian"}, {"hr", "hrv", "hrv", "croatian"},
		{"sr", "srp", "srp", "serbian"}, {"sl", "slv", "slv", "slovenian"}, {"et", "est", "est", "estonian"},
		{"lv", "lav", "lav", "latvian"}, {"lt", "lit", "lit", "lithuanian"}, {"is", "ice", "isl", "icelandic"},
		{"ca", "cat", "cat", "catalan"}, {"eu", "baq", "eus", "basque"}, {"gl", "glg", "glg", "galician"},
		{"fa", "per", "fas", "persian"}, {"bn", "ben", "ben", "bengali"}, {"ta", "tam", "tam", "tamil"},
	};

	// The primary subtag: pt-BR is Portuguese, zh_Hans is Chinese.
	std::string_view primary_subtag(std::string_view s)
	{
		s = str::trim(s);
		const auto dash = s.find_first_of("-_");
		return dash == std::string_view::npos ? s : s.substr(0, dash);
	}

	const language_names* find_language(const std::string_view s)
	{
		const auto code = primary_subtag(s);
		if (code.empty()) return nullptr;

		for (const auto& l : languages)
		{
			if (str::icmp(code, l.iso1) == 0 || str::icmp(code, l.iso2b) == 0 || str::icmp(code, l.iso2t) == 0 ||
				str::icmp(code, l.name) == 0)
				return &l;
		}

		return nullptr;
	}
}

bool same_language(const std::string_view a, const std::string_view b)
{
	const auto* const la = find_language(a);
	const auto* const lb = find_language(b);
	if (la && lb) return la == lb;

	const auto pa = primary_subtag(a);
	return !pa.empty() && str::icmp(pa, primary_subtag(b)) == 0;
}

bool is_language_word(const std::string_view word)
{
	return find_language(word) != nullptr;
}

sidecar_subtitle_name read_sidecar_subtitle_name(const std::string_view sidecar_name, const std::string_view video_name)
{
	sidecar_subtitle_name result;

	const auto stem = sidecar_name.substr(0, df::find_ext(sidecar_name));
	const auto video_stem = video_name.substr(0, df::find_ext(video_name));
	if (stem.size() < video_stem.size() || str::icmp(stem.substr(0, video_stem.size()), video_stem) != 0) return result;

	for (const auto word : str::split(stem.substr(video_stem.size()), false,
	                                  [](const char c) { return c == '.' || c == ' ' || c == '_'; }))
	{
		if (str::icmp(word, "forced") == 0) result.forced = true;
		else if (result.language.empty() && find_language(word)) result.language = word;
	}

	return result;
}

subtitle_choice choose_subtitles(const std::vector<subtitle_offer>& offers, const subtitle_mode mode,
                                 const std::string_view preferred_language, const std::string_view audio_language)
{
	const auto pick = [&offers](const std::string_view language, const bool forced) -> std::optional<subtitle_choice>
	{
		for (const auto& o : offers)
		{
			if (o.forced == forced && same_language(o.language, language)) return o.choice;
		}

		return std::nullopt;
	};

	const auto speech_is_foreign = !audio_language.empty() && str::icmp(audio_language, "und") != 0 &&
		!same_language(audio_language, preferred_language);
	const auto wanted = mode == subtitle_mode::always || (mode == subtitle_mode::foreign && speech_is_foreign);

	if (wanted)
	{
		if (const auto full = pick(preferred_language, false)) return *full;
		// A track that names no language is offered when it is the only one.
		if (offers.size() == 1 && offers.front().language.empty() && !offers.front().forced) return offers.front().choice;
	}

	if (const auto forced = pick(preferred_language, true)) return *forced;
	return {};
}

std::vector<subtitle_offer> sidecar_subtitle_offers(const df::file_path video, const std::string_view sidecars)
{
	std::vector<subtitle_offer> result;
	const auto video_name = video.name().sv();
	const auto names = str::split(sidecars, true);

	for (const auto name : names)
	{
		const auto ext_pos = std::min(df::find_ext(name), name.size());
		const auto ext = name.substr(ext_pos);
		if (!is_subtitle_extension(ext)) continue;

		// A VobSub's pictures are read through its .idx, so the .sub beside one is not offered twice.
		if (str::icmp(ext, ".sub") == 0)
		{
			const auto idx = std::string(name.substr(0, ext_pos)) + ".idx";
			if (std::ranges::any_of(names, [&idx](const std::string_view n) { return str::icmp(n, idx) == 0; })) continue;
		}

		const auto read = read_sidecar_subtitle_name(name, video_name);
		subtitle_offer offer;
		offer.choice.sidecar = video.folder().combine_file(name);
		offer.language = read.language;
		offer.forced = read.forced;
		offer.title = name;
		result.emplace_back(std::move(offer));
	}

	return result;
}
