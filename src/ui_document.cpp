// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implements document_control: measuring the document into rows, drawing the rows the clip
// meets with their colours and the selection, and the controller that selects with the mouse.

#include "pch.h"
#include "ui_document.h"
#include "model_display.h"

namespace
{
	// What a character is drawn in. Everything but base is a highlight over the view's text colour.
	enum colour_slot : uint8_t
	{
		base,
		dim,
		control,
		high_byte,
		keyword,
		comment,
		number,
		string,
		preprocessor,
		heading1,
		heading2,
		heading3,
		bold,
		italic,
		link,
		muted,
		bullet,
		slot_count,
	};

	colour_slot slot_of(const pf::ui::text_style style)
	{
		using ts = pf::ui::text_style;

		switch (style)
		{
		case ts::code_keyword: return keyword;
		case ts::code_comment:
		case ts::md_quote: return comment;
		case ts::code_number: return number;
		case ts::code_string:
		case ts::md_code: return string;
		case ts::code_preprocessor: return preprocessor;
		case ts::md_heading1: return heading1;
		case ts::md_heading2: return heading2;
		case ts::md_heading3: return heading3;
		case ts::md_bold: return bold;
		case ts::md_italic: return italic;
		case ts::md_link_text: return link;
		case ts::md_link_url:
		case ts::md_marker: return muted;
		case ts::md_bullet: return bullet;
		default: return base;
		}
	}

	// Picked to read on the view's dark background, as the dump's colours always were.
	std::array<ui::color, slot_count> make_palette(const ui::draw_context& dc)
	{
		const auto alpha = dc.colors.alpha;
		std::array<ui::color, slot_count> result;
		result[base] = ui::color(dc.colors.foreground, alpha);
		result[dim] = ui::color(dc.colors.foreground, alpha * 0.77f);
		result[control] = ui::color(ui::lighten(ui::style::colors.view_selected_background, 0.22f), alpha);
		result[high_byte] = ui::color(ui::lighten(app_colors::duplicate_background, 0.66f), alpha);
		result[keyword] = ui::color(ui::bgr(0x569CD6), alpha);
		result[comment] = ui::color(ui::bgr(0x6A9955), alpha);
		result[number] = ui::color(ui::bgr(0xB5CEA8), alpha);
		result[string] = ui::color(ui::bgr(0xCE9178), alpha);
		result[preprocessor] = ui::color(ui::bgr(0xC586C0), alpha);
		result[heading1] = ui::color(ui::bgr(0x4FC1FF), alpha);
		result[heading2] = ui::color(ui::bgr(0x569CD6), alpha);
		result[heading3] = ui::color(ui::bgr(0x9CDCFE), alpha);
		result[bold] = ui::color(ui::bgr(0xDCDCAA), alpha);
		result[italic] = ui::color(ui::bgr(0xD7BA7D), alpha);
		result[link] = ui::color(ui::bgr(0x4FC1FF), alpha);
		result[muted] = ui::color(ui::bgr(0x9D9D9D), alpha);
		result[bullet] = ui::color(ui::bgr(0x6796E6), alpha);
		return result;
	}

	colour_slot byte_slot(const uint8_t b)
	{
		if (b == ' ' || b == '\t') return base;
		if (b < 32) return control;
		if (b >= 127) return high_byte;
		return base;
	}

	// The dump's offset is dimmed, and its control and high bytes coloured in both columns.
	void dump_slots(std::vector<colour_slot>& slots, const df::cspan bytes, const int bytes_per_row, const size_t row)
	{
		std::fill_n(slots.begin(), std::min<size_t>(8, slots.size()), dim);

		const auto start = row * bytes_per_row;
		if (start >= bytes.size) return;

		const auto limit = std::min(static_cast<size_t>(bytes_per_row), bytes.size - start);
		const auto ascii = static_cast<size_t>(document_state::dump_ascii_column(bytes_per_row));

		for (size_t j = 0; j < limit; ++j)
		{
			const auto slot = byte_slot(bytes.data[start + j]);
			if (slot == base) continue;

			const auto hex = 10 + j * 3 + j / 8;
			const auto chr = ascii + j + j / 8;
			if (hex + 1 < slots.size()) slots[hex] = slots[hex + 1] = slot;
			if (chr < slots.size()) slots[chr] = slot;
		}
	}

	bool is_control_at(const std::string_view line, const uint32_t at)
	{
		const auto b = static_cast<uint8_t>(line[at]);
		if (b < 0x20) return b != '\t';
		if (b == 0x7F) return true;
		// U+0080 to U+009F, which Windows-1252's undefined bytes decode to.
		return b == 0xC2 && at + 1 < line.size() && static_cast<uint8_t>(line[at + 1]) < 0xA0;
	}

	class document_select_controller final : public view_controller
	{
		std::shared_ptr<document_control> _control;
		pointi _element_offset;
		bool _tracking = false;

		// The pane scrolls under a drag - the wheel still turns while the button is held - so where the
		// document is drawn is read from the display each time, not kept from when the press landed.
		pointi element_offset() const
		{
			const auto& display = _control->display();
			return display ? display->media_offset : _element_offset;
		}

	public:
		document_select_controller(const view_host_ptr& host, std::shared_ptr<document_control> control,
		                           const recti bounds, const pointi element_offset) :
			view_controller(host, bounds), _control(std::move(control)), _element_offset(element_offset)
		{
		}

		ui::style::cursor cursor() const override
		{
			return ui::style::cursor::text_select;
		}

		void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);

			auto& doc = _control->document();
			const auto at = _control->position_from_point(loc, element_offset());

			// Shift extends the selection from where it was anchored, as a text field does.
			if (keys.shift) doc.caret = at;
			else doc.select(at, at);

			doc.focused = true;
			_tracking = true;
			_host->invalidate_element(_control);
		}

		void on_mouse_move(const pointi loc) override
		{
			if (!_tracking) return;

			auto& doc = _control->document();
			const auto at = _control->position_from_point(loc, element_offset());

			if (at != doc.caret)
			{
				doc.caret = at;
				_host->invalidate_element(_control);
			}
		}

		void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
		{
			if (!_tracking) return;
			on_mouse_move(loc);
			_tracking = false;
		}

		// The word of the character under the pointer, not of the gap nearest it: the right half of a
		// word's last letter is nearer the space after it.
		void on_mouse_left_button_double_click(const pointi loc, const ui::key_state keys) override
		{
			auto& doc = _control->document();
			doc.select_word(_control->position_from_point(loc, element_offset(), true), _control->bytes());
			doc.focused = true;
			_tracking = false;
			_host->invalidate_element(_control);
		}

		bool escape() override
		{
			if (!_tracking) return false;
			_tracking = false;
			return true;
		}
	};
}

document_control::document_control(display_state_ptr display, const view_element_options& style_in) noexcept :
	view_element(style_in), _display(std::move(display))
{
	_document = _display && _display->_document ? _display->_document : std::make_shared<document_state>();
}

document_control::document_control(std::vector<uint8_t> bytes, const view_element_options& style_in) noexcept :
	view_element(style_in), _bytes(std::move(bytes)), _document(std::make_shared<document_state>())
{
}

df::cspan document_control::bytes() const
{
	if (_display) return _display->_selected_item_data;
	return {_bytes.data(), _bytes.size()};
}

int document_control::text_left(const recti logical_bounds) const
{
	if (_document->is_text()) return logical_bounds.left;
	return (logical_bounds.left + logical_bounds.right - _dump_width) / 2;
}

sizei document_control::measure(ui::measure_context& mc, const int width_limit) const
{
	auto& doc = *_document;
	const auto data = bytes();

	if (doc.is_text())
	{
		// Measured over a run of characters, since one character's width is rarely a whole pixel.
		constexpr int sample_chars = 64;
		// A pane with no width yet - a minimised window - would otherwise wrap every character onto
		// a row of its own, which for a MiB of text is a million rows.
		constexpr int min_columns = 8;
		const auto extent = mc.measure_text(std::string(sample_chars, '0'), _font,
		                                    ui::style::text_style::single_line, sample_chars * 256);
		_char_width = std::max(1.0, extent.cx / static_cast<double>(sample_chars));
		_line_height = extent.cy + padding;
		doc.layout(std::max(min_columns, static_cast<int>(width_limit / _char_width)), data.size);
		return {width_limit, _line_height * static_cast<int>(doc.row_count())};
	}

	// The widest row of whole groups of four bytes that fits, up to 32 a row.
	const auto payload_width = std::max(byte_step, std::min(max_bytes_per_line,
	                                                        df::round_up(static_cast<int>(data.size), byte_step)));
	auto bytes_per_line = 0;
	_dump_width = 0;

	for (auto candidate = byte_step; candidate <= payload_width; candidate += byte_step)
	{
		const auto chars = document_state::dump_row_chars(candidate);
		const auto extent = mc.measure_text(std::string(chars, '0'), _font, ui::style::text_style::single_line,
		                                    width_limit + 200);

		if (extent.cx > width_limit && bytes_per_line > 0) break;

		bytes_per_line = candidate;
		_dump_width = extent.cx;
		_char_width = std::max(1.0, extent.cx / static_cast<double>(chars));
		_line_height = extent.cy + padding;
		if (extent.cx > width_limit) break;
	}

	doc.layout(bytes_per_line, data.size);
	return {width_limit, _line_height * static_cast<int>(doc.row_count())};
}

document_position document_control::position_from_point(const pointi loc, const pointi element_offset,
                                                        const bool character) const
{
	const auto& doc = *_document;
	const auto rows = static_cast<int>(doc.row_count());
	if (rows == 0 || _line_height <= 0) return {};

	const auto logical_bounds = bounds.offset(element_offset);
	const auto data = bytes();
	const auto at = [&](const int row, const double column)
	{
		return character ? doc.character_at(row, column, data) : doc.position_at(row, column, data);
	};

	if (loc.y < logical_bounds.top) return at(0, 0.0);

	const auto row = (loc.y - logical_bounds.top) / _line_height;
	if (row >= rows) return at(rows - 1, std::numeric_limits<double>::max());

	return at(row, (loc.x - text_left(logical_bounds)) / _char_width);
}

view_controller_ptr document_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                               const pointi element_offset, hit_test_context& ctx)
{
	// Only the display's document is selected: a metadata block's dump is rebuilt with the panel, and
	// would lose its selection to the next late scan result.
	if (!_display) return nullptr;

	const auto logical_bounds = bounds.offset(element_offset);
	if (!ctx.occluded(logical_bounds)) return nullptr;

	return std::make_shared<document_select_controller>(host, shared_from_this(), logical_bounds, element_offset);
}

void document_control::render(ui::draw_context& dc, const pointi element_offset) const
{
	const auto& doc = *_document;
	const auto data = bytes();
	const auto rows = static_cast<int>(doc.row_count());
	if (rows == 0 || _line_height <= 0) return;

	const auto logical_bounds = bounds.offset(element_offset);
	const auto clip_bounds = dc.clip_bounds().intersection(logical_bounds);

	// Only the rows that meet the clip are drawn. Apart from it, the intersection is empty and placed at
	// the origin, which named rows before the first and read before the data.
	if (clip_bounds.is_empty()) return;

	const auto first_row = std::max(0, (clip_bounds.top - logical_bounds.top) / _line_height);
	const auto last_row = std::min(rows, first_row + (clip_bounds.height() + _line_height) / _line_height);
	const auto left = text_left(logical_bounds);
	const auto palette = make_palette(dc);
	const auto selection_clr = ui::color(ui::style::colors.view_selected_background,
	                                     dc.colors.alpha * (doc.focused ? 0.85f : 0.4f));
	const auto has_selection = doc.has_selection();
	const auto [sel_from, sel_to] = doc.selection();

	const auto& text = doc.text();
	const auto highlight = text.language == pf::ui::syntax::language::plain
		                       ? pf::ui::highlight_fn{}
		                       : pf::ui::syntax::for_language(text.language);

	std::vector<colour_slot> slots;
	std::vector<ui::text_highlight_t> highlights;
	std::vector<pf::ui::text_block> runs;
	auto runs_line = -1;
	auto run_count = 0;

	for (auto i = first_row; i < last_row; ++i)
	{
		const auto row = doc.row_text(i, data);
		const auto& cells = row.cells;
		const auto line = doc.row_line(i);
		const auto y = logical_bounds.top + i * _line_height;

		slots.assign(cells.size() - 1, base);

		if (row.is_dump)
		{
			dump_slots(slots, data, doc.columns(), i);
		}
		else
		{
			const auto source = text.line(line);

			if (highlight && source.size() <= document_state::max_highlighted_line)
			{
				if (runs_line != line)
				{
					runs.resize(pf::ui::syntax::blocks_for_line(source.size()));
					highlight(text.cookies.empty() ? 0u : text.cookies[line], source, runs, run_count);
					runs_line = line;
				}

				auto k = 0;

				for (size_t c = 0; c < slots.size(); ++c)
				{
					const auto at = static_cast<int>(cells[c].source);
					while (k + 1 < run_count && runs[k + 1]._char_pos <= at) ++k;
					if (run_count > 0 && runs[k]._char_pos <= at) slots[c] = slot_of(runs[k]._color);
				}
			}

			for (size_t c = 0; c < slots.size(); ++c)
			{
				if (is_control_at(source, cells[c].source)) slots[c] = control;
			}
		}

		if (has_selection && line >= sel_from.line && line <= sel_to.line)
		{
			const document_position row_start{line, static_cast<int>(cells.front().source)};
			const document_position row_end{line, static_cast<int>(cells.back().source)};
			const auto from = std::max(sel_from, row_start);
			const auto to = std::min(sel_to, row_end);
			const auto index_of = [&cells](const int source)
			{
				const auto found = std::ranges::lower_bound(cells, static_cast<uint32_t>(source), {},
				                                            &document_cell::source);
				return static_cast<size_t>(found - cells.begin());
			};

			// The break at the end of a selected line shows as one more column after its last row.
			const auto selects_break = !row.is_dump && row_end.offset == static_cast<int>(text.lines[line].length) &&
				sel_from <= row_end && sel_to.line > line;
			auto first = cells.size() - 1;
			auto last = first;

			if (from < to)
			{
				first = std::min(index_of(from.offset), cells.size() - 1);
				last = std::min(index_of(to.offset), cells.size() - 1);
				std::fill(slots.begin() + first, slots.begin() + last, base);
			}

			if (first < last || selects_break)
			{
				const auto x0 = left + static_cast<int>(std::lround(cells[first].column * _char_width));
				const auto x1 = left + static_cast<int>(std::lround(
					(cells[last].column + (selects_break ? 1 : 0)) * _char_width));
				dc.draw_rect(recti(x0, y, x1, y + _line_height), selection_clr);
			}
		}

		highlights.clear();

		for (size_t c = 0; c < slots.size();)
		{
			const auto slot = slots[c];
			auto d = c + 1;
			while (d < slots.size() && slots[d] == slot) ++d;

			if (slot != base)
			{
				highlights.emplace_back(cells[c].display, cells[d].display - cells[c].display, palette[slot]);
			}

			c = d;
		}

		dc.draw_text(row.display, highlights, recti(left, y, logical_bounds.right, y + _line_height), _font,
		             ui::style::text_style::single_line, palette[base], {});
	}
}
