// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The application's Linux glue: no hardware video decode for FFmpeg to use - the counterpart
// of platform_win_av.cpp - no platform tests to register, since the Windows-only subject file is not
// built here, and the entry point, which runs the application's headless suite.

#include "pch.h"
#include "av_format.h"
#include "av_sound.h"
#include "files.h"
#include "model.h"
#include "test_runner.h"
#include "libdesktop/util_base64.h"

#include <pthread.h>

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
void av_platform_decode_lock(const AVHWFramesContext*, ui::video_picture&)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Tests. The Windows-only subject file is not built here, so the registration is empty.
///////////////////////////////////////////////////////////////////////////////////////////////////

void register_platform_tests(view_state&, test_registry&)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Entry point
///////////////////////////////////////////////////////////////////////////////////////////////////

int main(const int argc, const char* const argv[])
{
	platform::set_ui_thread();

	if (!std::setlocale(LC_CTYPE, "C.UTF-8"))
	{
		std::fprintf(stderr, "Cannot initialize the UTF-8 character locale required for filenames.\n");
		return 1;
	}

	// /test[:filter] runs the suite headless. The '-' forms are the Linux spelling; '/test' is
	// accepted too because it is what the Windows documentation and habits say.
	for (auto i = 1; i < argc; ++i)
	{
		std::string_view arg(argv[i]);

		if (arg.starts_with("--")) arg.remove_prefix(2);
		else if (arg.starts_with('-') || arg.starts_with('/')) arg.remove_prefix(1);
		else continue;

		if (arg == "test" || arg.starts_with("test:"))
		{
			const auto colon = arg.find(':');
			const auto filter = colon == std::string_view::npos ? "*"sv : arg.substr(colon + 1);
			load_file_types();
			return run_console_tests(filter);
		}
	}

	printf("Diffractor %s (linux stage-one build)\n", df::format_version(false).c_str());

	// Exercise the portable core so a successful run proves linkage, not only a successful build.
	const auto sample = "Diffractor"sv;
	const std::vector<uint8_t> sample_bytes(sample.begin(), sample.end());

	printf("  str::to_lower       %s\n", str::to_lower(sample).c_str());
	printf("  str::icmp           %d\n", str::icmp(sample, "DIFFRACTOR"sv));
	printf("  str::fnv1a_i        %u\n", str::fnv1a_i(sample));
	printf("  base64 round trip   %s\n", base64_decode(base64_encode(sample)) == sample_bytes ? "ok" : "FAILED");
	printf("  known app_data      %s\n",
	       std::string(platform::known_path(platform::known_folder::app_data).text()).c_str());
	printf("\nRun with /test to run the suite.\n");

	return 0;
}
