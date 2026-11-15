// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Precompiled header file. Includes libdesktop's UI umbrella - the core, platform, render
// and UI interfaces - and the application's core headers, which every file uses.

#pragma once

#include "libdesktop/ui.h"
// Diffractor writes libdesktop's core vocabulary - geometry, strings, paths, dates, pixels - unqualified.
using namespace core;

#include "util_interfaces.h"
#include "app_version.h"
#include "app_perf.h"
#include "crypto.h"
#include "app_icons.h"
#include "app_resources.h"
#include "app_colors.h"
#include "app_settings.h"
