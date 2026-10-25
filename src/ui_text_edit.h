// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Window-free state and editing behavior for rendered single-line text controls.

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
	class single_line_edit_model
	{
		std::string _text;
		std::string _original_text;
		size_t _caret = 0;
		size_t _anchor = 0;

		struct edit_state
		{
			std::string text;
			size_t caret = 0;
			size_t anchor = 0;
		};

		std::vector<edit_state> _undo;
		std::vector<edit_state> _redo;

		static bool is_continuation(const unsigned char c) noexcept
		{
			return (c & 0xc0) == 0x80;
		}

		size_t clamp_boundary(size_t pos) const noexcept
		{
			pos = std::min(pos, _text.size());
			while (pos > 0 && pos < _text.size() && is_continuation(static_cast<unsigned char>(_text[pos]))) --pos;
			return pos;
		}

		size_t previous_boundary(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			if (pos == 0) return 0;
			--pos;
			while (pos > 0 && is_continuation(static_cast<unsigned char>(_text[pos]))) --pos;
			return pos;
		}

		size_t next_boundary(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			if (pos >= _text.size()) return _text.size();
			++pos;
			while (pos < _text.size() && is_continuation(static_cast<unsigned char>(_text[pos]))) ++pos;
			return pos;
		}

		static bool is_word_byte(const unsigned char c) noexcept
		{
			return c >= 0x80 || std::isalnum(c) != 0 || c == '_';
		}

		size_t previous_word(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			while (pos > 0 && !is_word_byte(static_cast<unsigned char>(_text[previous_boundary(pos)])))
				pos = previous_boundary(pos);
			while (pos > 0 && is_word_byte(static_cast<unsigned char>(_text[previous_boundary(pos)])))
				pos = previous_boundary(pos);
			return pos;
		}

		size_t next_word(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			while (pos < _text.size() && is_word_byte(static_cast<unsigned char>(_text[pos]))) pos = next_boundary(pos);
			while (pos < _text.size() && !is_word_byte(static_cast<unsigned char>(_text[pos]))) pos =
				next_boundary(pos);
			return pos;
		}

		void record_undo()
		{
			_undo.push_back({_text, _caret, _anchor});
			if (_undo.size() > 100) _undo.erase(_undo.begin());
			_redo.clear();
		}

		void restore(std::vector<edit_state>& from, std::vector<edit_state>& to)
		{
			if (from.empty()) return;
			to.push_back({_text, _caret, _anchor});
			auto state = std::move(from.back());
			from.pop_back();
			_text = std::move(state.text);
			_caret = state.caret;
			_anchor = state.anchor;
		}

		void erase_selection_impl()
		{
			if (!has_selection()) return;
			const auto start = selection_start();
			_text.erase(start, selection_end() - start);
			_caret = _anchor = start;
		}

		void collapse_or_move(const bool right, const bool extend)
		{
			if (has_selection() && !extend)
			{
				_caret = _anchor = right ? selection_end() : selection_start();
				return;
			}

			_caret = right ? next_boundary(_caret) : previous_boundary(_caret);
			if (!extend) _anchor = _caret;
		}

	public:
		const std::string& text() const noexcept { return _text; }
		size_t caret() const noexcept { return _caret; }
		size_t anchor() const noexcept { return _anchor; }
		bool has_selection() const noexcept { return _caret != _anchor; }
		size_t selection_start() const noexcept { return std::min(_caret, _anchor); }
		size_t selection_end() const noexcept { return std::max(_caret, _anchor); }

		void text(std::string value)
		{
			_text = std::move(value);
			_caret = _anchor = _text.size();
			_undo.clear();
			_redo.clear();
		}

		void begin_edit()
		{
			_original_text = _text;
		}

		void cancel_edit()
		{
			text(_original_text);
		}

		void select(const size_t anchor, const size_t caret)
		{
			_anchor = clamp_boundary(anchor);
			_caret = clamp_boundary(caret);
		}

		void select_all()
		{
			_anchor = 0;
			_caret = _text.size();
		}

		void move_left(const bool extend = false) { collapse_or_move(false, extend); }
		void move_right(const bool extend = false) { collapse_or_move(true, extend); }

		void move_word_left(const bool extend = false)
		{
			if (has_selection() && !extend)
			{
				_caret = _anchor = selection_start();
				return;
			}
			_caret = previous_word(_caret);
			if (!extend) _anchor = _caret;
		}

		void move_word_right(const bool extend = false)
		{
			if (has_selection() && !extend)
			{
				_caret = _anchor = selection_end();
				return;
			}
			_caret = next_word(_caret);
			if (!extend) _anchor = _caret;
		}

		void move_home(const bool extend = false)
		{
			_caret = 0;
			if (!extend) _anchor = _caret;
		}

		void move_end(const bool extend = false)
		{
			_caret = _text.size();
			if (!extend) _anchor = _caret;
		}

		void erase_selection()
		{
			if (!has_selection()) return;
			record_undo();
			erase_selection_impl();
		}

		std::string selected_text() const
		{
			return _text.substr(selection_start(), selection_end() - selection_start());
		}

		void select_word(const size_t pos)
		{
			auto start = clamp_boundary(pos);
			if (start == _text.size() && start > 0) start = previous_boundary(start);
			if (start >= _text.size()) return select(start, start);
			const auto word = is_word_byte(static_cast<unsigned char>(_text[start]));
			auto end = next_boundary(start);
			while (start > 0 && is_word_byte(static_cast<unsigned char>(_text[previous_boundary(start)])) == word)
				start = previous_boundary(start);
			while (end < _text.size() && is_word_byte(static_cast<unsigned char>(_text[end])) == word)
				end = next_boundary(end);
			select(start, end);
		}

		void insert(const std::string_view value)
		{
			std::string single_line;
			single_line.reserve(value.size());
			for (const auto c : value)
			{
				if (c == '\r' || c == '\n')
				{
					if (single_line.empty() || single_line.back() != ' ') single_line.push_back(' ');
				}
				else
				{
					single_line.push_back(c);
				}
			}

			if (single_line.empty() && !has_selection()) return;
			record_undo();
			erase_selection_impl();
			_text.insert(_caret, single_line);
			_caret += single_line.size();
			_anchor = _caret;
		}

		void backspace()
		{
			if (has_selection())
			{
				record_undo();
				erase_selection_impl();
				return;
			}
			if (_caret == 0) return;
			record_undo();
			const auto previous = previous_boundary(_caret);
			_text.erase(previous, _caret - previous);
			_caret = _anchor = previous;
		}

		bool paste_from_clipboard(const std::function<std::optional<std::string>()>& read_text)
		{
			const auto value = read_text();
			if (!value) return false;
			insert(*value);
			return true;
		}

		bool cut_to_clipboard(const std::function<bool(std::string_view)>& write_text)
		{
			const auto selected = selected_text();
			if (selected.empty()) return false;
			if (!write_text(selected)) return false;
			erase_selection();
			return true;
		}

		void delete_forward()
		{
			if (has_selection())
			{
				record_undo();
				erase_selection_impl();
				return;
			}
			if (_caret >= _text.size()) return;
			record_undo();
			const auto next = next_boundary(_caret);
			_text.erase(_caret, next - _caret);
			_anchor = _caret;
		}

		void backspace_word()
		{
			if (has_selection()) return backspace();
			const auto previous = previous_word(_caret);
			if (previous == _caret) return;
			record_undo();
			_text.erase(previous, _caret - previous);
			_caret = _anchor = previous;
		}

		void delete_word()
		{
			if (has_selection()) return delete_forward();
			const auto next = next_word(_caret);
			if (next == _caret) return;
			record_undo();
			_text.erase(_caret, next - _caret);
			_anchor = _caret;
		}

		void undo() { restore(_undo, _redo); }
		void redo() { restore(_redo, _undo); }
	};
}
