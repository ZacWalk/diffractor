// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Adapts the decoded document to the shared panel and formatted views, retaining the text and
// dump source selection and their syntax colours while the owning pane scrolls them.

#include "pch.h"
#include "ui_document.h"
#include "model_display.h"
#include "libdesktop/ui/views.h"

namespace
{
	// The formatted views read a colour's alpha from its high byte, which the shared palette leaves clear, as it
	// holds RGB alone.
	constexpr core::color32 opaque(const core::color32 rgb)
	{
		return core::rgba(core::get_r(rgb), core::get_g(rgb), core::get_b(rgb));
	}

	class document_theme final : public ui::view_theme
	{
	public:
		core::color32 style_color(const ui::text_style style) const override
		{
			switch (style)
			{
			// None, so the panel's own background shows, as it does behind plain text.
			case ui::text_style::normal_bkgnd: return 0;
			case ui::text_style::normal_text: return opaque(ui::style::colors.view_text);
			case ui::text_style::sel_bkgnd: return opaque(ui::style::colors.view_selected_background);
			default: return view_theme::style_color(style);
			}
		}
	};

	const ui::view_theme& formatted_theme()
	{
		static const document_theme theme;
		return theme;
	}

	template <typename View> class document_view_adapter final : public View
	{
		std::weak_ptr<document_state> _document;

		void selection_from_model() const
		{
			if (const auto doc = _document.lock())
			{
				const ui::text_selection selection{doc->anchor.offset, doc->anchor.line, doc->caret.offset,
				                                   doc->caret.line};
				if (this->current_selection() != selection)
					const_cast<document_view_adapter*>(this)->set_selection(selection);
			}
		}

		void selection_to_model(const int before_height)
		{
			auto changed = false;
			if (const auto doc = _document.lock())
			{
				const auto selection = this->current_selection();
				const document_position anchor{selection._start.y, selection._start.x};
				const document_position caret{selection._end.y, selection._end.x};
				changed = doc->anchor != anchor || doc->caret != caret;
				doc->select(anchor, caret);
			}
			if (changed) this->invalidate();
			if (before_height != this->content_extent().cy)
			{
				if (const auto host = this->host())
				{
					if (const auto listener = std::dynamic_pointer_cast<ui::element_host>(host->listener()))
						listener->invalidate(ui::host_invalid::layout | ui::host_invalid::redraw);
				}
			}
		}

	public:
		explicit document_view_adapter(const std::shared_ptr<document_state>& document) :
			View(formatted_theme()), _document(document)
		{
			auto buffer = std::make_shared<ui::text_buffer>(this->default_view_host());
			buffer->set_text(document->text().text);
			this->set_buffer(buffer, {});
		}

		render::font_face body_font() const override { return render::font_face::code; }
		void paint(render::draw_context& dc, const bool focused) const override
		{
			selection_from_model();
			View::paint(dc, focused);
		}
		void on_focus(const bool focused, const bool select_all) override
		{
			selection_from_model();
			const auto height = this->content_extent().cy;
			View::on_focus(focused, select_all);
			if (const auto doc = _document.lock()) doc->focused = focused;
			selection_to_model(height);
		}
		void on_mouse_down(const pointi point, const platform::key_state keys) override
		{
			selection_from_model();
			const auto height = this->content_extent().cy;
			if (const auto doc = _document.lock()) doc->focused = true;
			View::on_mouse_down(point, keys);
			selection_to_model(height);
		}
		void on_mouse_move(const pointi point, const bool captured) override
		{
			selection_from_model();
			const auto height = this->content_extent().cy;
			View::on_mouse_move(point, captured);
			selection_to_model(height);
		}
		void on_mouse_up(const pointi point, const platform::key_state keys) override
		{
			selection_from_model();
			const auto height = this->content_extent().cy;
			View::on_mouse_up(point, keys);
			selection_to_model(height);
		}
		void on_double_click(const pointi point, const platform::key_state keys) override
		{
			selection_from_model();
			const auto height = this->content_extent().cy;
			View::on_double_click(point, keys);
			selection_to_model(height);
		}
		bool on_key(const char32_t key, const platform::key_state keys) override
		{
			selection_from_model();
			const auto height = this->content_extent().cy;
			const auto handled = View::on_key(key, keys);
			selection_to_model(height);
			return handled;
		}
	};

	std::shared_ptr<ui::doc_view> formatted_document(const std::shared_ptr<document_state>& document)
	{
		if (!document->is_text()) return {};
		if (document->formatted_view) return document->formatted_view;
		if (document->text().csv)
		{
			document->formatted_view = std::make_shared<document_view_adapter<ui::csv_view>>(document);
		}
		else if (document->text().language == ui::syntax::language::markdown)
		{
			auto markdown = std::make_shared<document_view_adapter<ui::markdown_view>>(document);
			markdown->set_link_handler([](const std::string_view target)
			{
				if (target.starts_with("https://") || target.starts_with("http://") || target.starts_with("mailto:"))
					platform::open(target);
			});
			document->formatted_view = std::move(markdown);
		}
		return document->formatted_view;
	}

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

	colour_slot slot_of(const ui::text_style style)
	{
		using ts = ui::text_style;

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
	std::array<core::color, slot_count> make_palette(const render::draw_context& dc)
	{
		const auto alpha = dc.colors.alpha;
		std::array<core::color, slot_count> result;
		result[base] = core::color(dc.colors.foreground, alpha);
		result[dim] = core::color(dc.colors.foreground, alpha * 0.77f);
		result[control] = core::color(core::lighten(ui::style::colors.view_selected_background, 0.22f), alpha);
		result[high_byte] = core::color(core::lighten(app_colors::duplicate_background, 0.66f), alpha);
		result[keyword] = core::color(core::bgr(0x569CD6), alpha);
		result[comment] = core::color(core::bgr(0x6A9955), alpha);
		result[number] = core::color(core::bgr(0xB5CEA8), alpha);
		result[string] = core::color(core::bgr(0xCE9178), alpha);
		result[preprocessor] = core::color(core::bgr(0xC586C0), alpha);
		result[heading1] = core::color(core::bgr(0x4FC1FF), alpha);
		result[heading2] = core::color(core::bgr(0x569CD6), alpha);
		result[heading3] = core::color(core::bgr(0x9CDCFE), alpha);
		result[bold] = core::color(core::bgr(0xDCDCAA), alpha);
		result[italic] = core::color(core::bgr(0xD7BA7D), alpha);
		result[link] = core::color(core::bgr(0x4FC1FF), alpha);
		result[muted] = core::color(core::bgr(0x9D9D9D), alpha);
		result[bullet] = core::color(core::bgr(0x6796E6), alpha);
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
	void dump_slots(std::vector<colour_slot>& slots, const core::cspan bytes, const int bytes_per_row, const size_t row)
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
		std::shared_ptr<document_content> _control;
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
		document_select_controller(const view_host_ptr& host, std::shared_ptr<document_content> control,
		                           const recti bounds, const pointi element_offset) :
			view_controller(host, bounds), _control(std::move(control)), _element_offset(element_offset)
		{
		}

		platform::cursor cursor() const override { return platform::cursor::text_select; }

		void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
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

		void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
		{
			if (!_tracking) return;
			on_mouse_move(loc);
			_tracking = false;
		}

		// The word of the character under the pointer, not of the gap nearest it: the right half of a
		// word's last letter is nearer the space after it.
		void on_mouse_left_button_double_click(const pointi loc, const platform::key_state /*keys*/) override
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

document_content::document_content(display_state_ptr display, const view_element_options& style_in) noexcept :
	view_element(style_in), _display(std::move(display))
{
	_document = _display && _display->_document ? _display->_document : std::make_shared<document_state>();
	_formatted = formatted_document(_document);
}

document_content::document_content(std::vector<uint8_t> bytes, const view_element_options& style_in) noexcept :
	view_element(style_in), _bytes(std::move(bytes)), _document(std::make_shared<document_state>())
{
}

core::cspan document_content::bytes() const
{
	if (_display) return _display->_selected_item_data;
	return {_bytes.data(), _bytes.size()};
}

int document_content::text_left(const recti logical_bounds) const
{
	if (_document->is_text()) return logical_bounds.left;
	return (logical_bounds.left + logical_bounds.right - _dump_width) / 2;
}

sizei document_content::measure(render::measure_context& mc, const int width_limit) const
{
	if (_formatted)
	{
		_formatted->bounds = {0, 0, std::max(64, width_limit), 1};
		_formatted->on_placed(mc);
		return {std::max(0, width_limit), _formatted->content_extent().cy};
	}
	auto& doc = *_document;
	const auto data = bytes();

	if (doc.is_text())
	{
		// Measured over a run of characters, since one character's width is rarely a whole pixel.
		constexpr int sample_chars = 64;
		// A pane with no width yet - a minimised window - would otherwise wrap every character onto
		// a row of its own, which for a MiB of text is a million rows.
		constexpr int min_columns = 8;
		const auto extent = mc.measure_text(std::string(sample_chars, '0'), _font, render::text_style::single_line,
		                                    sample_chars * 256);
		_char_width = std::max(1.0, extent.cx / static_cast<double>(sample_chars));
		_line_height = extent.cy + padding;
		doc.layout(std::max(min_columns, static_cast<int>(width_limit / _char_width)), data.size);
		return {width_limit, _line_height * static_cast<int>(doc.row_count())};
	}

	// The widest row of whole groups of four bytes that fits, up to 32 a row.
	const auto payload_width = std::max(byte_step, std::min(max_bytes_per_line,
	                                                        core::round_up(static_cast<int>(data.size), byte_step)));
	auto bytes_per_line = 0;
	_dump_width = 0;

	for (auto candidate = byte_step; candidate <= payload_width; candidate += byte_step)
	{
		const auto chars = document_state::dump_row_chars(candidate);
		const auto extent = mc.measure_text(std::string(chars, '0'), _font, render::text_style::single_line,
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

document_position document_content::position_from_point(const pointi loc, const pointi element_offset,
                                                        const bool character) const
{
	if (_formatted)
	{
		const auto position = _formatted->text_at(loc - bounds.top_left() - element_offset);
		return {position.y, position.x};
	}
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

view_controller_ptr document_content::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                               const pointi element_offset, hit_test_context& ctx)
{
	if (_formatted) return view_element::controller_from_location(host, loc, element_offset, ctx);
	// Only the display's document is selected: a metadata block's dump is rebuilt with the panel, and
	// would lose its selection to the next late scan result.
	if (!_display) return nullptr;

	const auto logical_bounds = bounds.offset(element_offset);
	if (!ctx.occluded(logical_bounds)) return nullptr;

	return std::make_shared<document_select_controller>(host, shared_from_this(), logical_bounds, element_offset);
}

void document_content::render(render::draw_context& dc, const pointi element_offset) const
{
	if (_formatted)
	{
		render_children(dc, element_offset);
		return;
	}
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
	const auto selection_clr = core::color(ui::style::colors.view_selected_background,
	                                       dc.colors.alpha * (doc.focused ? 0.85f : 0.4f));
	const auto has_selection = doc.has_selection();
	const auto [sel_from, sel_to] = doc.selection();

	const auto& text = doc.text();
	const auto highlight = text.language == ui::syntax::language::plain ? ui::highlight_fn{}
	                                                                    : ui::syntax::for_language(text.language);

	std::vector<colour_slot> slots;
	std::vector<render::text_highlight_t> highlights;
	std::vector<ui::text_block> runs;
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
					runs.resize(ui::syntax::blocks_for_line(source.size()));
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
				const auto x1 = left + static_cast<int>(
										   std::lround((cells[last].column + (selects_break ? 1 : 0)) * _char_width));
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
		             render::text_style::single_line, palette[base], {});
	}
}

void document_content::layout(render::measure_context& mc, const recti bounds_in)
{
	view_element::layout(mc, bounds_in);
	if (_formatted) _formatted->ui::control::layout(mc, bounds_in);
}

void document_content::visit_children(const std::function<void(const view_element_ptr&)>& visit) const
{
	if (_formatted) visit(_formatted);
}

document_control::document_control(std::shared_ptr<document_content> content, const view_element_options& options) :
	ui::document_panel(content, options), _content(std::move(content))
{
}

document_control::document_control(display_state_ptr display, const view_element_options& options) :
	document_control(std::make_shared<document_content>(std::move(display), view_element_options{}), options)
{
}

document_control::document_control(std::vector<uint8_t> bytes, const view_element_options& options) :
	document_control(std::make_shared<document_content>(std::move(bytes), view_element_options{}), options)
{
}
