// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: ICC colour profiles, which libfiles lists - the summary, header and tag directory the info
// pane shows - as an item's metadata rows.

#include "pch.h"

#include "metadata_icc.h"
#include "files.h"

#include "libfiles/icc.h"

metadata_kv_list metadata_icc::to_info(const df::cspan data)
{
	auto rows = libfiles::list_icc({data.data, data.size});
	return to_metadata_kv(rows);
}