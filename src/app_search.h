// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The address box editing session. Holds the committed address, the typed draft and
// whether a completion is being previewed, and answers what each key should do with them. Kept
// apart from app_frame so the behavior design.md specifies can be reasoned about, and tested,
// without a window.

#pragma once

namespace ui
{
	class complete_strategy_t;
}

class view_state;

// The address bar's completion strategy. Built through a factory so the ranking and content rules
// it implements can be driven from a test without a window; the concrete type stays in app_search.cpp.
std::shared_ptr<ui::complete_strategy_t> make_search_auto_complete(view_state& state,
                                                                   std::function<void(std::string)> set_text);

class search_edit_session
{
	// The address as it stood when editing began. Escape returns here.
	std::string _original;

	// The latest text the user actually typed, which a completion preview must not overwrite.
	std::string _typed;

	bool _previewing = false;

public:
	// What the caller should do with the address box and the completion popup.
	struct outcome
	{
		std::string edit_text;
		bool set_edit_text = false;
		bool select_first_completion = false;
		bool refresh_completions = false;
		bool close_popup = false;
		bool focus_view = false;
		bool handled = false;
	};

	// Focusing the address box begins a session and snapshots the committed address.
	void begin(const std::string_view committed)
	{
		_original = committed;
		_typed = committed;
		_previewing = false;
	}

	// Focus leaving the address box ends the session. A preview is a suggestion the user was
	// looking at, not one they chose, so the draft they typed comes back with them - otherwise the
	// highlighted completion is left sitting in the box and becomes the address as far as the next
	// Enter, and anything else reading the box, can tell.
	outcome end()
	{
		outcome result;

		if (_previewing)
		{
			result.edit_text = _typed;
			result.set_edit_text = true;
		}

		_previewing = false;
		return result;
	}

	// Typing maintains a draft separate from the committed address.
	void typed(const std::string_view text)
	{
		_previewing = false;
		_typed = text;
	}

	// Up and Down preview a completion in the address box without changing the draft.
	void preview(const std::string_view text)
	{
		_previewing = true;
		(void)text;
	}

	// Escape is two-stage: back to the draft first, then back to where editing began.
	outcome escape()
	{
		outcome result;
		result.handled = true;

		if (_previewing)
		{
			_previewing = false;
			result.edit_text = _typed;
			result.set_edit_text = true;
			result.select_first_completion = true;
			return result;
		}

		result.edit_text = _original;
		result.set_edit_text = true;
		result.close_popup = true;
		result.focus_view = true;
		return result;
	}

	// Tab accepts the highlighted completion into the draft without running it.
	outcome accept(const bool has_selection, const std::string_view selected_text)
	{
		outcome result;

		if (!has_selection) return result;

		_previewing = false;
		_typed = selected_text;

		result.handled = true;
		result.edit_text = _typed;
		result.set_edit_text = true;
		result.refresh_completions = true;
		return result;
	}

	// Enter commits the highlighted completion when there is one, otherwise the visible address.
	std::string commit(const bool has_selection, const std::string_view selected_text,
	                   const std::string_view visible_text) const
	{
		return std::string(has_selection ? selected_text : visible_text);
	}

	bool is_previewing() const { return _previewing; }
	const std::string& typed_text() const { return _typed; }
	const std::string& original_text() const { return _original; }
};
