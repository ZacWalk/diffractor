// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The application's embedded assets - the index database's schema and the world map - which
// app_resources.cpp links in from arrays the build generates from src/Res, on every platform.

#pragma once

enum class app_resource
{
	sql,
	map_png
};

df::blob load_app_resource(app_resource r);

namespace ui
{
	class surface;
}

// Draws the application's mark centred on a surface, transparent around it.
void fill_app_logo(ui::surface& surface);
