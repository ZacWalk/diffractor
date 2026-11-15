// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The app's own colours beside the UI's palette: the highlights that mark ranked items,
// sidecars and duplicates.

#pragma once

namespace app_colors
{
	constexpr core::color32 rank_background = core::bgr(0x00997711);
	constexpr core::color32 sidecar_background = core::bgr(0x006677CC);
	constexpr core::color32 duplicate_background = core::bgr(0x007711AA);
}
