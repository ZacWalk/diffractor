// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Out-of-line implementations for the controls declared in ui_controls.h. A control that
// needs application services belongs here rather than in the application frame, so the control
// layer can be built without it.

#include "pch.h"

#include "model.h"
#include "app_command_status.h"
#include "ui_controls.h"

void rating_control::dispatch_event(const view_element_event& event)
{
	if (event.type == view_element_event_type::invoke)
	{
		auto dlg = make_dlg(event.host->owner());
		const auto results = std::make_shared<command_status>(_state._async, dlg, icon_index::star,
		                                                      tt.prop_name_rating, 1);
		_state.toggle_rating(results, {_item}, _hover_rating, event.host);
	}
}
