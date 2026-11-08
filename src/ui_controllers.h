// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: UI input controllers for moving a selection and dragging its handles. A clickable
// element's controller is libdesktop-render's.

#pragma once

#include "libdesktop/ui.h"
#include "ui_view.h"

template <class TParent>
class selection_move_controller final : public view_controller
{
public:
	TParent& _parent;
	rectd _device_start;
	quadd _existing_selection;
	bool _tracking = false;

	selection_move_controller(const view_host_ptr& host, TParent& parent, const recti bounds_in) :
		view_controller(host, bounds_in), _parent(parent)
	{
		_tracking = false;
	}

	~selection_move_controller() override
	{
		if (_tracking)
		{
			_tracking = false;
			_parent.selection(_existing_selection);
		}
	}

	ui::style::cursor cursor() const override
	{
		return _tracking ? ui::style::cursor::hand_up : ui::style::cursor::hand_down;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		if (!_tracking)
		{
			_tracking = true;
			_last_loc = _start_loc = loc;
			_device_start = _parent.device_selection();
			_existing_selection = _parent.selection();
		}
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_tracking)
		{
			_last_loc = loc;
			update(true);
		}
	}

	void update(bool tracking) const
	{
		const auto offset = _start_loc - _last_loc;
		auto rr = _device_start.offset(-offset.x, -offset.y);
		_parent.device_selection(rr, false, true);
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_tracking)
		{
			_tracking = false;
			_last_loc = loc;
			_parent._state.invalidate_view(view_invalid::controller);
			update(false);
		}
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		_parent.selection(_existing_selection);
		return true;
	}
};

template <class TParent>
class handle_move_controller final : public view_controller
{
public:
	TParent& _parent;
	rectd _device_start;
	quadd _existing_selection;
	bool _tracking;

	bool _left = false;
	bool _top = false;
	bool _right = false;
	bool _bottom = false;

	handle_move_controller(const view_host_ptr& host, TParent& parent, const rectd& start, const bool l, const bool t,
	                       const bool r,
	                       const bool b) : view_controller(host, start.round()), _parent(parent)
	{
		_left = l;
		_top = t;
		_right = r;
		_bottom = b;

		_tracking = false;
	}

	~handle_move_controller() override
	{
		if (_tracking)
		{
			_tracking = false;
			_parent.selection(_existing_selection);
		}
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::move;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		if (!_tracking)
		{
			_last_loc = _start_loc = loc;
			_device_start = _parent.device_selection();
			_existing_selection = _parent.selection();
			_tracking = true;
		}
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_tracking)
		{
			_last_loc = loc;
			update(true);
		}
	}

	void update(bool tracking) const
	{
		const auto offset = _start_loc - _last_loc;
		auto rr = _device_start.round();

		if (_left) rr.left = std::min(rr.left - offset.x, rr.right - 1);
		if (_top) rr.top = std::min(rr.top - offset.y, rr.bottom - 1);
		if (_right) rr.right = std::max(rr.right - offset.x, rr.left + 1);
		if (_bottom) rr.bottom = std::max(rr.bottom - offset.y, rr.top + 1);

		// 0 2
		// 3 2
		int active_point = 0;
		if (_top && _right) active_point = 1;
		if (_bottom && _right) active_point = 2;
		if (_bottom && _left) active_point = 3;

		_parent.device_selection2(rr, active_point);
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_tracking)
		{
			_tracking = false;
			_last_loc = loc;
			update(false);
		}
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		_parent.selection(_existing_selection);
		return true;
	}
};
