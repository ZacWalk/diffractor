// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The application's version and build, which app.cpp defines, and how they read.

#pragma once

extern const std::string_view s_app_version;
extern const std::string_view g_app_build;

namespace df
{
	std::string format_version(bool short_text);
}
