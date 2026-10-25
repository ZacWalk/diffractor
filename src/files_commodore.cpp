// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Commodore 64 disk, tape, cartridge and program images: the listing libfiles makes of one, drawn
// by libfiles into a surface for the disk control's texture.

#include "pch.h"
#include "files.h"

ui::const_surface_ptr files::c64_listing_surface(const libfiles::c64_listing& listing, const uint32_t fg,
                                                 const uint32_t bg)
{
	if (listing.rows.empty() || listing.columns == 0) return {};

	auto surface = std::make_shared<ui::surface>();
	auto* const pixels = surface->alloc(static_cast<int>(listing.width()), static_cast<int>(listing.height()),
	                                    ui::texture_format::RGB);
	if (!pixels) return {};

	// RGB textures upload as B8G8R8X8, the order libfiles draws in, given its colours as 0xRRGGBB. A
	// listing is a whole number of eight-pixel cells wide, so its rows fill their stride, and no padding
	// goes to the GPU unwritten.
	libfiles::draw_c64_listing(listing, {pixels, surface->stride()}, ui::bgr(fg), ui::bgr(bg));
	return surface;
}
