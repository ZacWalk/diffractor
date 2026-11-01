// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Draws the read-only document of model_document.h in the selection panel - text in its
// syntax colours, or a hex dump - with its selection, and turns presses and drags on it into
// selections. The display's document keeps its selection while the panel is rebuilt around it; a raw
// metadata block's dump is drawn the same way and is not selected.

#pragma once

#include "model.h"
#include "model_document.h"
#include "ui_elements.h"

class document_control final : public std::enable_shared_from_this<document_control>, public view_element
{
public:
	// The display's document, decoded when its bytes were read.
	document_control(display_state_ptr display, const view_element_options& style_in) noexcept;
	// A dump of bytes this control holds itself.
	document_control(std::vector<uint8_t> bytes, const view_element_options& style_in) noexcept;

	sizei measure(ui::measure_context& mc, int width_limit) const override;
	void render(ui::draw_context& dc, pointi element_offset) const override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

	document_state& document() const
	{
		return *_document;
	}

	// The display whose document this is; none for a dump of bytes the control holds.
	const display_state_ptr& display() const
	{
		return _display;
	}

	df::cspan bytes() const;

	// The position under a point. Above the document is its start and below it its end, so a drag
	// that leaves the document still selects to the edge it left by. With character set, the
	// character under the point rather than the gap nearest it - what a double click selects.
	document_position position_from_point(pointi loc, pointi element_offset, bool character = false) const;

private:
	static constexpr int padding = 4;
	static constexpr int byte_step = 4;
	static constexpr int max_bytes_per_line = 32;

	display_state_ptr _display;
	std::vector<uint8_t> _bytes;
	std::shared_ptr<document_state> _document;
	ui::style::font_face _font = ui::style::font_face::code;

	mutable int _line_height = 0;
	mutable double _char_width = 0.0;
	// A dump is centred on the width of its rows, as the hex view always was.
	mutable int _dump_width = 0;

	int text_left(recti logical_bounds) const;
};
