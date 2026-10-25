// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

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
