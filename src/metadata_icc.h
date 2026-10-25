// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: ICC color profile parsing. Extracts color space information from
// embedded ICC profiles in image files.

#pragma once

namespace metadata_icc
{
	metadata_kv_list to_info(df::cspan data);
}
