// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The read-only document the selection panel shows for a file Diffractor has no viewer
// for: the head of its bytes as text when they read as text, and as a hex dump when they do not,
// laid out in rows, selected and copied. Decoding is pure and runs on a load thread; layout and the
// selection belong to the UI thread. No drawing and no I/O - ui_document.h draws it, and
// docs/selection-controls.md owns the behaviour.

#pragma once

#include "ui/syntax.h"

enum class document_encoding : uint8_t
{
	binary,
	utf8,
	utf16le,
	utf16be,
	windows_1252,
};

// A place in the document: a line and a byte offset into its UTF-8. In a hex dump the line is a row of
// the dump and the offset a column of its text, which is ASCII.
struct document_position
{
	int line = 0;
	int offset = 0;

	friend bool operator==(const document_position&, const document_position&) = default;
	friend auto operator<=>(const document_position&, const document_position&) = default;
};

// The decoded text of a file. Immutable once made, so a load thread builds it and hands it over.
struct document_text
{
	struct line_span
	{
		uint32_t start = 0;
		uint32_t length = 0;
	};

	document_encoding encoding = document_encoding::binary;
	pf::ui::syntax::language language = pf::ui::syntax::language::plain;
	// UTF-8 without its line breaks; lines index into it.
	std::string text;
	std::vector<line_span> lines;
	// The highlighter's state at the start of each line, so any line can be coloured without the lines
	// above it. Empty when the language carries no state from line to line.
	std::vector<uint32_t> cookies;

	bool is_text() const
	{
		return encoding != document_encoding::binary;
	}

	std::string_view line(const size_t i) const
	{
		return std::string_view(text).substr(lines[i].start, lines[i].length);
	}
};

// Reads the head of a file as text. Bytes that do not read as text - or text_allowed false - leave the
// encoding binary and the text empty, and the document is a hex dump of the bytes themselves. The
// extension, with or without its dot, chooses the highlighter.
document_text decode_document(df::cspan bytes, std::string_view extension, bool text_allowed);

// The columns a code point takes in a monospaced font: two for the wide East Asian ranges and emoji,
// none for combining marks, joiners and variation selectors, one for anything else.
int document_column_width(uint32_t cp);

// One code point of a row as it is drawn: where it starts in the source line, where in the drawn
// text, and at which column. A row's cells end with one for the row's end.
struct document_cell
{
	uint32_t source = 0;
	uint32_t display = 0;
	int column = 0;
};

struct document_row_text
{
	std::string display;
	std::vector<document_cell> cells;
	// The drawn text of a dump, which a copy takes, rather than the source text.
	bool is_dump = false;
};

// A document's layout and selection. Text wraps at word boundaries to the columns it is given; a dump
// shows as many bytes a row as it is given. Every call that needs the bytes of a dump is passed them,
// because they belong to whoever loaded them.
class document_state
{
public:
	static constexpr int tab_size = 4;
	// A line longer than this is drawn without syntax colouring: a minified file is one line, and
	// colouring a row starts from the start of its line.
	static constexpr size_t max_highlighted_line = 16 * 1024;

	explicit document_state(document_text text = {});

	const document_text& text() const
	{
		return _text;
	}

	bool is_text() const
	{
		return _text.is_text();
	}

	// Text: wraps to `columns` characters a row. Dump: `columns` bytes a row over byte_count bytes.
	// A dump laid out at a new width loses its selection, whose columns no longer name the same bytes.
	void layout(int columns, size_t byte_count);

	int columns() const
	{
		return _columns;
	}

	size_t row_count() const;
	document_row_text row_text(size_t row, df::cspan bytes) const;
	// The document line a row shows: its own index in a dump.
	int row_line(size_t row) const;
	size_t row_of(document_position pos) const;

	// The position nearest a column of a row, rounding to the closer side of the character it lands on.
	document_position position_at(size_t row, double column, df::cspan bytes) const;
	// The character a column of a row falls on - what a double click selects the word of - where
	// position_at gives the gap nearest it. Past the end of the row, its last character.
	document_position character_at(size_t row, double column, df::cspan bytes) const;

	// The drawn text of one row of a dump: the offset, then the bytes in hex, then the bytes as ASCII.
	static std::string dump_row(df::cspan bytes, int bytes_per_row, size_t row);
	static int dump_ascii_column(int bytes_per_row);
	static int dump_row_chars(int bytes_per_row);

	document_position anchor;
	document_position caret;
	// Set by a press on the document and cleared by any other press in the view. Copy and Select all
	// act on the document only while it is set.
	bool focused = false;

	bool has_selection() const
	{
		return anchor != caret;
	}

	std::pair<document_position, document_position> selection() const
	{
		return anchor < caret ? std::make_pair(anchor, caret) : std::make_pair(caret, anchor);
	}

	void select(const document_position from, const document_position to)
	{
		anchor = from;
		caret = to;
	}

	void clear_selection()
	{
		anchor = caret = {};
	}

	void select_all(df::cspan bytes);
	void select_word(document_position at, df::cspan bytes);
	std::string selected_text(df::cspan bytes) const;

private:
	struct text_row
	{
		uint32_t line = 0;
		uint32_t begin = 0;
		uint32_t end = 0;
	};

	document_text _text;
	std::vector<text_row> _rows;
	int _columns = 0;
	size_t _byte_count = 0;

	std::string_view line_text(int line, df::cspan bytes, std::string& dump) const;
	int line_count(df::cspan bytes) const;
};
