// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: ICC colour profiles, which libfiles lists: the summary, header and tag directory an item's
// info pane shows.

#pragma once

namespace metadata_icc
{
	metadata_kv_list to_info(df::cspan data);
}
