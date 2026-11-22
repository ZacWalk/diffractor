// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Diffractor's own Windows resource identifiers. Libdesktop's platform and render assets are
// linked into their libraries and are not part of this executable resource script.

// clang-format off
#define IDI_APP_ICON   1
// Windows reads an executable's manifest only as resource 1 - CREATEPROCESS_MANIFEST_RESOURCE_ID - and an icon of the
// same number is a different resource type, so the two do not collide.
#define IDR_MANIFEST   1
// clang-format on

// Next default values for new objects
//
#ifdef APSTUDIO_INVOKED
// clang-format off
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE        230
#define _APS_NEXT_COMMAND_VALUE         40633
#define _APS_NEXT_CONTROL_VALUE         1097
// clang-format on
#define _APS_NEXT_SYMED_VALUE 120
#endif
#endif
