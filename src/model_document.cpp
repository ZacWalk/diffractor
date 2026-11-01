// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Decodes, lays out and selects the read-only document model_document.h describes: which
// encoding the head of a file reads in, its lines and highlighter states, rows wrapped at word
// boundaries or cut from a hex dump, the position under a column, and the text a copy takes.

#include "pch.h"
#include "model_document.h"

namespace
{
	// A head read from a longer file can end inside a UTF-8 sequence.
	constexpr size_t utf8_cut_short = SIZE_MAX;

	// The length of the UTF-8 sequence at p[i]: 0 when it is not one, utf8_cut_short when the end of the
	// bytes comes first. RFC 3629: no overlong forms, no surrogates, nothing past U+10FFFF.
	size_t utf8_sequence(const uint8_t* p, const size_t i, const size_t n)
	{
		const auto c = p[i];
		if (c < 0x80) return 1;

		size_t extra = 0;
		uint8_t lo = 0x80;
		uint8_t hi = 0xBF;

		if (c >= 0xC2 && c <= 0xDF) extra = 1;
		else if (c == 0xE0) extra = 2, lo = 0xA0;
		else if (c >= 0xE1 && c <= 0xEC) extra = 2;
		else if (c == 0xED) extra = 2, hi = 0x9F;
		else if (c >= 0xEE && c <= 0xEF) extra = 2;
		else if (c == 0xF0) extra = 3, lo = 0x90;
		else if (c >= 0xF1 && c <= 0xF3) extra = 3;
		else if (c == 0xF4) extra = 3, hi = 0x8F;
		else return 0;

		for (size_t k = 1; k <= extra; ++k)
		{
			if (i + k >= n) return utf8_cut_short;
			const auto b = p[i + k];
			if (b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xBF)) return 0;
		}

		return extra + 1;
	}

	bool is_valid_utf8(const uint8_t* p, const size_t n)
	{
		for (size_t i = 0; i < n;)
		{
			const auto len = utf8_sequence(p, i, n);
			if (len == utf8_cut_short) return true;
			if (len == 0) return false;
			i += len;
		}

		return true;
	}

	// Copies UTF-8, dropping a sequence the end cuts short and replacing a byte that begins none with
	// U+FFFD - which only a byte order mark can ask of bytes that are not valid.
	void append_utf8(std::string& out, const uint8_t* p, const size_t n)
	{
		for (size_t i = 0; i < n;)
		{
			const auto len = utf8_sequence(p, i, n);
			if (len == utf8_cut_short) break;

			if (len == 0)
			{
				out += "\xEF\xBF\xBD";
				++i;
			}
			else
			{
				out.append(reinterpret_cast<const char*>(p + i), len);
				i += len;
			}
		}
	}

	void append_utf16(std::string& out, const uint8_t* p, const size_t n, const bool big_endian)
	{
		std::u16string units;
		units.reserve(n / 2);

		for (size_t i = 0; i + 1 < n; i += 2)
		{
			units.push_back(big_endian
				                ? static_cast<char16_t>(p[i] << 8 | p[i + 1])
				                : static_cast<char16_t>(p[i] | p[i + 1] << 8));
		}

		// A lead surrogate the end of the head separated from its trail is not a character.
		if (!units.empty() && units.back() >= 0xD800 && units.back() <= 0xDBFF) units.pop_back();

		out += str::utf16_to_utf8(std::u16string_view(units));
	}

	// The 0x80 to 0x9F row of Windows-1252. The five bytes it leaves undefined stay the C1 controls
	// they name in Latin-1, as Windows decodes them.
	constexpr char16_t windows_1252_row8[32] = {
		0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
		0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
		0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
		0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
	};

	void append_windows_1252(std::string& out, const uint8_t* p, const size_t n)
	{
		auto inserter = std::back_inserter(out);

		for (size_t i = 0; i < n; ++i)
		{
			const auto c = p[i];
			str::char32_to_utf8(inserter, c >= 0x80 && c < 0xA0 ? windows_1252_row8[c - 0x80] : c);
		}
	}

	// Text holds tab, the line breaks, vertical tab and form feed, backspace from overstruck manual
	// pages, escape from terminal colour in logs, and DOS's end-of-file mark. More than one byte in a
	// hundred of any other control says the bytes are not text. Only text read by its byte order mark
	// can hold a NUL here, and it is one more control: a log padded with a few is still a log.
	bool reads_as_text(const std::string_view text)
	{
		size_t controls = 0;

		for (const auto ch : text)
		{
			const auto c = static_cast<uint8_t>(ch);

			if (c < 0x20 || c == 0x7F)
			{
				if (c != '\t' && c != '\n' && c != '\r' && c != 0x0B && c != 0x0C && c != 0x08 && c != 0x1A && c !=
					0x1B)
				{
					++controls;
				}
			}
		}

		return controls * 100 <= text.size();
	}

	pf::ui::syntax::language language_of(std::string_view extension)
	{
		if (extension.starts_with('.')) extension.remove_prefix(1);

		if (str::icmp(extension, "md") == 0 || str::icmp(extension, "markdown") == 0)
		{
			return pf::ui::syntax::language::markdown;
		}

		return pf::ui::syntax::language_from_extension(extension);
	}

	void split_lines(document_text& doc)
	{
		const auto& text = doc.text;
		uint32_t start = 0;

		for (uint32_t i = 0; i < text.size(); ++i)
		{
			const auto c = text[i];

			if (c == '\n' || c == '\r')
			{
				doc.lines.push_back({start, i - start});
				if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
				start = i + 1;
			}
		}

		doc.lines.push_back({start, static_cast<uint32_t>(text.size()) - start});
	}

	// Runs the highlighter down the document once, with no room for runs, for the state each line
	// starts in: a block comment opened above a line colours it.
	void record_cookies(document_text& doc)
	{
		using pf::ui::syntax::language;
		if (doc.language == language::plain || doc.language == language::markdown) return;

		const auto highlight = pf::ui::syntax::for_language(doc.language);
		doc.cookies.resize(doc.lines.size());

		uint32_t cookie = 0;
		auto carried = false;
		int count = 0;

		for (size_t i = 0; i < doc.lines.size(); ++i)
		{
			doc.cookies[i] = cookie;
			carried |= cookie != 0;
			cookie = highlight(cookie, doc.line(i), std::span<pf::ui::text_block>{}, count);
		}

		if (!carried) doc.cookies.clear();
	}

	bool is_word_byte(const uint8_t c)
	{
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
	}

	bool is_control(const uint32_t cp)
	{
		return cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
	}
}

document_text decode_document(const df::cspan bytes, const std::string_view extension, const bool text_allowed)
{
	document_text result;
	if (!text_allowed || bytes.empty()) return result;

	const auto* const p = bytes.data;
	const auto n = bytes.size;
	auto encoding = document_encoding::binary;
	std::string text;

	if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
	{
		encoding = document_encoding::utf8;
		append_utf8(text, p + 3, n - 3);
	}
	else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE)
	{
		// UTF-32 has the same mark, and reads here as UTF-16 full of NULs, which is not text.
		encoding = document_encoding::utf16le;
		append_utf16(text, p + 2, n - 2, false);
	}
	else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF)
	{
		encoding = document_encoding::utf16be;
		append_utf16(text, p + 2, n - 2, true);
	}
	else
	{
		// Without a byte order mark a NUL is enough: no text file holds one, and every binary format
		// has them early.
		if (std::memchr(p, 0, n) != nullptr) return result;

		if (is_valid_utf8(p, n))
		{
			encoding = document_encoding::utf8;
			append_utf8(text, p, n);
		}
		else
		{
			encoding = document_encoding::windows_1252;
			append_windows_1252(text, p, n);
		}
	}

	if (!reads_as_text(text)) return result;

	result.encoding = encoding;
	result.language = language_of(extension);
	result.text = std::move(text);
	split_lines(result);
	record_cookies(result);
	return result;
}

int document_column_width(const uint32_t cp)
{
	// Combining marks, zero-width spaces and joiners, variation selectors and the byte order mark
	// draw on what precedes them.
	if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
		(cp >= 0x200B && cp <= 0x200F) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
		(cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0xFEFF)
	{
		return 0;
	}

	if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
		(cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
		(cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
		(cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
		(cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD))
	{
		return 2;
	}

	return 1;
}

document_state::document_state(document_text text) : _text(std::move(text))
{
}

void document_state::layout(const int columns, const size_t byte_count)
{
	const auto c = std::max(1, columns);

	if (!is_text())
	{
		if (c != _columns && _columns != 0) clear_selection();
		_columns = c;
		_byte_count = byte_count;
		return;
	}

	if (c == _columns) return;

	_columns = c;
	_rows.clear();

	// Characters that take no column - combining marks, joiners - would otherwise put any number of
	// themselves in one row, and a row is drawn and hit tested whole.
	const auto max_row_chars = static_cast<uint32_t>(c) * 4 + 64;

	for (uint32_t li = 0; li < _text.lines.size(); ++li)
	{
		const auto s = _text.line(li);
		const auto n = static_cast<uint32_t>(s.size());
		uint32_t begin = 0;

		do
		{
			auto i = begin;
			auto col = 0;
			uint32_t chars = 0;
			uint32_t space_end = 0;

			while (i < n)
			{
				if (chars >= max_row_chars && i > begin) break;

				auto it = s.begin() + i;
				const auto cp = str::pop_utf8_char(it, s.end());
				const auto next = static_cast<uint32_t>(it - s.begin());
				const auto w = cp == '\t' ? tab_size - col % tab_size : document_column_width(cp);

				if (col + w > c && i > begin)
				{
					// Spaces that reach the edge hang past it, so the next row starts with a word - but
					// no more of them than a row holds, or a line of nothing else is one row of any size.
					if (cp == ' ' || cp == '\t')
					{
						const auto hang_end = std::min(n, i + static_cast<uint32_t>(c));
						i = next;
						while (i < hang_end && (s[i] == ' ' || s[i] == '\t')) ++i;
						space_end = i;
					}

					break;
				}

				col += w;
				i = next;
				++chars;
				if (cp == ' ' || cp == '\t') space_end = i;
			}

			const auto end = i < n && space_end > begin ? space_end : i;
			_rows.push_back({li, begin, end});
			begin = end;
		}
		while (begin < n);
	}
}

size_t document_state::row_count() const
{
	if (is_text()) return _rows.size();
	return _columns > 0 ? (_byte_count + _columns - 1) / static_cast<size_t>(_columns) : 0;
}

int document_state::row_line(const size_t row) const
{
	return is_text() ? static_cast<int>(_rows[row].line) : static_cast<int>(row);
}

size_t document_state::row_of(const document_position pos) const
{
	const auto count = row_count();
	if (count == 0) return 0;

	if (!is_text()) return std::clamp(static_cast<size_t>(std::max(0, pos.line)), 0_z, count - 1);

	// The last row starting at or before the position: one at a wrap belongs to the row it starts.
	const auto found = std::upper_bound(_rows.begin(), _rows.end(), pos, [](const document_position p, const text_row& r)
	{
		return p.line < static_cast<int>(r.line) || (p.line == static_cast<int>(r.line) && p.offset < static_cast<int>(r.begin));
	});

	return found == _rows.begin() ? 0 : static_cast<size_t>(found - _rows.begin()) - 1;
}

int document_state::dump_ascii_column(const int bytes_per_row)
{
	return 10 + bytes_per_row * 3 + bytes_per_row / 8;
}

int document_state::dump_row_chars(const int bytes_per_row)
{
	return static_cast<int>(sizeof(uint32_t)) * 2 + 4 + bytes_per_row * 4 + bytes_per_row / 8 * 2;
}

std::string document_state::dump_row(const df::cspan bytes, const int bytes_per_row, const size_t row)
{
	static constexpr char hex_chars[] = "0123456789ABCDEF";

	std::string line(dump_row_chars(bytes_per_row), ' ');
	const auto start = row * bytes_per_row;
	if (bytes_per_row <= 0 || start >= bytes.size) return line;

	const auto limit = std::min(static_cast<size_t>(bytes_per_row), bytes.size - start);
	const auto address = static_cast<uint32_t>(start);
	auto x = 0;

	for (auto shift = 28; shift >= 0; shift -= 4) line[x++] = hex_chars[address >> shift & 0xF];

	x += 2;

	for (size_t j = 0; j < limit; ++j)
	{
		const auto b = bytes.data[start + j];
		line[x] = hex_chars[b >> 4];
		line[x + 1] = hex_chars[b & 0xF];
		x += j % 8 == 7 ? 4 : 3;
	}

	x = dump_ascii_column(bytes_per_row);

	for (size_t j = 0; j < limit; ++j)
	{
		const auto b = bytes.data[start + j];
		line[x] = b == '\t' ? ' ' : (b < 32 || b >= 127 ? '.' : static_cast<char>(b));
		x += j % 8 == 7 ? 2 : 1;
	}

	return line;
}

document_row_text document_state::row_text(const size_t row, const df::cspan bytes) const
{
	document_row_text result;

	if (!is_text())
	{
		result.is_dump = true;
		result.display = dump_row(bytes, _columns, row);

		for (uint32_t k = 0; k <= result.display.size(); ++k)
		{
			result.cells.push_back({k, k, static_cast<int>(k)});
		}

		return result;
	}

	const auto& r = _rows[row];
	const auto s = _text.line(r.line);
	auto col = 0;
	auto i = r.begin;

	while (i < r.end)
	{
		auto it = s.begin() + i;
		const auto cp = str::pop_utf8_char(it, s.end());
		const auto next = static_cast<uint32_t>(it - s.begin());

		result.cells.push_back({i, static_cast<uint32_t>(result.display.size()), col});

		if (cp == '\t')
		{
			const auto w = tab_size - col % tab_size;
			result.display.append(w, ' ');
			col += w;
		}
		else if (is_control(cp))
		{
			// A middle dot stands in for a control character, which no font draws.
			result.display += "\xC2\xB7";
			col += 1;
		}
		else
		{
			result.display.append(s.substr(i, next - i));
			col += document_column_width(cp);
		}

		i = next;
	}

	result.cells.push_back({r.end, static_cast<uint32_t>(result.display.size()), col});
	return result;
}

document_position document_state::position_at(const size_t row, const double column, const df::cspan bytes) const
{
	if (row >= row_count()) return {};

	const auto t = row_text(row, bytes);
	const auto line = row_line(row);

	for (size_t k = 0; k + 1 < t.cells.size(); ++k)
	{
		if (column < (t.cells[k].column + t.cells[k + 1].column) / 2.0)
		{
			return {line, static_cast<int>(t.cells[k].source)};
		}
	}

	return {line, static_cast<int>(t.cells.back().source)};
}

document_position document_state::character_at(const size_t row, const double column, const df::cspan bytes) const
{
	if (row >= row_count()) return {};

	const auto t = row_text(row, bytes);
	const auto line = row_line(row);
	const auto& cells = t.cells;
	// The row's last character; an empty row has only its end.
	const auto last = cells.size() > 1 ? cells.size() - 2 : 0_z;

	for (size_t k = 0; k < last; ++k)
	{
		if (column < cells[k + 1].column) return {line, static_cast<int>(cells[k].source)};
	}

	return {line, static_cast<int>(cells[last].source)};
}

int document_state::line_count(const df::cspan bytes) const
{
	return is_text() ? static_cast<int>(_text.lines.size()) : static_cast<int>(row_count());
}

std::string_view document_state::line_text(const int line, const df::cspan bytes, std::string& dump) const
{
	if (is_text()) return _text.line(line);
	dump = dump_row(bytes, _columns, line);
	return dump;
}

void document_state::select_all(const df::cspan bytes)
{
	const auto lines = line_count(bytes);
	if (lines == 0) return;

	std::string dump;
	const auto last = line_text(lines - 1, bytes, dump);
	select({}, {lines - 1, static_cast<int>(last.size())});
}

void document_state::select_word(const document_position at, const df::cspan bytes)
{
	if (at.line < 0 || at.line >= line_count(bytes)) return;

	std::string dump;
	const auto s = line_text(at.line, bytes, dump);
	const auto size = static_cast<int>(s.size());
	const auto in_word = [dump_text = !is_text(), s](const int i)
	{
		const auto c = static_cast<uint8_t>(s[i]);
		return dump_text ? c != ' ' : is_word_byte(c);
	};

	auto begin = std::clamp(at.offset, 0, size);
	auto end = begin;

	if (end < size && in_word(end))
	{
		while (begin > 0 && in_word(begin - 1)) --begin;
		while (end < size && in_word(end)) ++end;
	}
	else if (end < size)
	{
		// Only ASCII is outside a word, so the character is one byte.
		++end;
	}

	select({at.line, begin}, {at.line, end});
}

std::string document_state::selected_text(const df::cspan bytes) const
{
	if (!has_selection()) return {};

	const auto [from, to] = selection();
	const auto lines = line_count(bytes);
	std::string result;
	std::string dump;

	for (auto line = std::max(0, from.line); line <= to.line && line < lines; ++line)
	{
		const auto s = line_text(line, bytes, dump);
		const auto begin = line == from.line ? std::min(static_cast<size_t>(std::max(0, from.offset)), s.size()) : 0_z;
		const auto end = line == to.line ? std::min(static_cast<size_t>(std::max(0, to.offset)), s.size()) : s.size();
		auto part = s.substr(begin, end > begin ? end - begin : 0);

		// A dump's rows are padded to one width; the padding is not part of what was selected.
		if (!is_text())
		{
			while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
		}

		if (line != from.line) result += "\r\n";
		result += part;
	}

	return result;
}
