// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The application's Linux glue: no hardware video decode for FFmpeg to use - the counterpart
// of platform_win_av.cpp - no platform tests to register, since the Windows-only subject file is not
// built here. The entry point and generated icon-font registration are shared in app.cpp, which calls
// libdesktop's UI runner on both platforms.

#include "pch.h"
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
// Tests. The Windows-only subject file is not built here, so the registration is empty.
///////////////////////////////////////////////////////////////////////////////////////////////////

void register_platform_tests(view_state&, test_registry&)
{
}
