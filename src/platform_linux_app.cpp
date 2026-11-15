// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The application's Linux glue: no hardware video decode for FFmpeg to use - the counterpart
// of platform_win_av.cpp - no screen recorder in a Debug build, which platform_win_record.cpp is on Windows,
// and no platform tests to register, since the Windows-only subject file is not built here. The entry point
// and generated icon-font registration are shared in app.cpp, which calls libdesktop's UI runner on both
// platforms.

#include "pch.h"
#include "app_record.h"
#include "av_format.h"
#include "av_sound.h"
#include "files.h"
#include "model.h"
#include "test_runner.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Media
///////////////////////////////////////////////////////////////////////////////////////////////////

// No hardware decode until there is a renderer to present its surfaces. VAAPI belongs here, and
// only once the GPU backend can import an NV12 surface - a decoder whose output cannot be shown
// is slower than software, not faster.
av_hw_decode_target av_platform_hw_decode_target()
{
	return {};
}

// Never asked while av_platform_hw_decode_target answers that there is no hardware path.
bool av_platform_hw_decode_supported(AVBufferRef*, AVCodecID, int, int, int)
{
	return false;
}

bool av_platform_hw_device_usable(AVBufferRef*)
{
	return false;
}

// Never asked while av_platform_hw_decode_target answers that there is no hardware path.
AVBufferRef* av_platform_create_hw_device(int)
{
	return nullptr;
}

// No hardware picture is decoded here, so there is no device lock to lend.
void av_platform_decode_lock(const AVHWFramesContext*, render::video_picture&)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Recording. Linux has no movie writer, so there is nothing to record with.
///////////////////////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<screen_recording> start_screen_recording(const screen_recording_request&)
{
	return nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Tests. The Windows-only subject file is not built here, so the registration is empty.
///////////////////////////////////////////////////////////////////////////////////////////////////

void register_platform_tests(view_state&, test_registry&)
{
}
