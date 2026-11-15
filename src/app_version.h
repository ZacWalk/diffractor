// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The application's version and build, which app.cpp defines, and how they read.

#pragma once

extern const std::string_view s_app_version;
extern const std::string_view g_app_build;

namespace df
{
	std::string format_version(bool short_text);
}
