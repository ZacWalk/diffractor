// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The application's embedded assets - the index database's schema and the world map - which
// app_resources.cpp links in from arrays the build generates from src/Res, on every platform.

#pragma once

enum class app_resource
{
	sql,
	map_png
};

core::blob load_app_resource(app_resource r);

namespace ui
{
	class surface;
}

// Draws the application's mark centred on a surface, transparent around it.
void fill_app_logo(core::surface& surface);
