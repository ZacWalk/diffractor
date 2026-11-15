// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The ordinary transport, one flex row of the existing play and seek controls, used by every media
// presentation.

#include "pch.h"
#include "ui_music.h"
#include "ui_controls.h"

view_elements_ptr make_media_transport(view_state& state, const display_state_ptr& display)
{
	auto transport = std::make_shared<view_elements>(flex_item::grow | flex_item::line_break | flex_item::stretch);
	transport->padding(0);
	transport->margin(0);
	transport->flex_container.wrap = flex_wrap::no_wrap;
	transport->flex_container.gap = {8, 0};
	transport->add(std::make_shared<play_control>(state, flex_item::no_break));
	transport->add(std::make_shared<scrubber_element>(state._player, display));
	return transport;
}
