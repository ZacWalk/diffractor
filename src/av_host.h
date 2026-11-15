// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The two things a media session asks of the application that hosts it - repaint, and run
// this on the UI thread. Kept apart from av_player.h so the application's strategies can implement
// it without compiling the player.

#pragma once

#include "ui_view.h"

class av_host
{
public:
	virtual ~av_host() = default;

	virtual void invalidate_view(view_invalid invalid) = 0;
	virtual void queue_ui(std::function<void()> f) = 0;
};
