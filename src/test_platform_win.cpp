// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for Windows platform integration. These are the only tests whose subject is the
// operating system itself -- extended path syntax, DXGI device loss, adapter classification and the
// texture budget taken from it, the adapter the decode device is made on, texture recycling, the
// crash-guard recovery session, the system font stack, the registry settings store, time zone
// conversion, the shell drag
// data object and the common-control paint contract the flicker-free control buffering depends on.
// Keeping them here is what lets every other test file stay free of system headers.

#include "pch.h"
#include "libdesktop/platform_win.h"
#include "libdesktop/platform_win_render.h"
#include "libdesktop/platform_win_visual.h"
#include "av_format.h"
#include "libdesktop/platform_win_render_res.h"
#include "test_fixtures.h"

#include <mmreg.h>

extern "C" {
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
}

// The recent-folder list must not wait on a share that has dropped, so a network location is
// recognised from its spelling and volume type without being reached.
static void should_recognise_network_locations()
{
	assert_equal(true, platform::is_network_location(df::folder_path(R"(\\server\share\photos)")),
	             "a UNC share is a network location");
	assert_equal(false, platform::is_network_location(test_files_folder), "the local test folder is not");
}

static void should_preserve_writability_probe_collisions()
{
	const auto folder = _temps.next_folder("write-probe");
	const auto name = std::format("df-write-probe-{:08x}.tmp", static_cast<uint32_t>(GetCurrentProcessId()));
	const auto collision = folder.combine_file(name);
	write_test_file(collision, "existing file");

	assert_equal(true, platform::is_writable(folder), "a probe collision does not make a writable folder read-only");
	assert_equal("existing file", read_test_file(collision), "the probe must not truncate or delete an existing file");
	assert_equal(1_z, platform::iterate_file_items(folder, true).files.size(), "the probe cleans up only its own file");
}

static void should_convert_extended_file_system_paths()
{
	const auto long_unc = std::string("\\\\server\\share\\") + std::string(MAX_PATH, 'x');
	const auto converted = platform::to_file_system_path(df::folder_path(long_unc));
	assert_equal(std::wstring(L"\\\\?\\UNC\\server\\share\\") + std::wstring(MAX_PATH, L'x'), converted,
	             "long UNC path");

	const auto extended = std::wstring(L"\\\\?\\C:\\") + std::wstring(MAX_PATH, L'x');
	assert_equal(extended, platform::to_file_system_path(df::folder_path(str::utf16_to_utf8(extended))),
	             "extended path unchanged");
}

static void should_convert_dates_with_their_own_dst_offset()
{
	TIME_ZONE_INFORMATION zone{};
	zone.Bias = 300;
	zone.DaylightBias = -60;
	zone.DaylightDate = {0, 3, 0, 2, 2, 0, 0, 0};
	zone.StandardDate = {0, 11, 0, 1, 2, 0, 0, 0};
	constexpr uint64_t fraction = 1234567;

	for (const auto month : {1, 7})
	{
		const auto utc = df::date_t(2024, month, 15, 12, 0, 0).to_int64() + fraction;
		const auto local = df::date_t(2024, month, 15, month == 1 ? 7 : 8, 0, 0).to_int64() + fraction;
		assert_equal(local, platform::utc_to_local(utc, &zone), "conversion uses the timestamp's season");
		assert_equal(utc, platform::local_to_utc(local, &zone), "reverse conversion preserves every tick");
	}

	const auto after_jump = df::date_t(2024, 3, 10, 3, 30, 0).to_int64();
	assert_equal(df::date_t(2024, 3, 10, 7, 30, 0).to_int64(), platform::local_to_utc(after_jump, &zone),
	             "an unambiguous time after the spring transition uses daylight bias");
	assert_equal(uint64_t{0}, platform::utc_to_local(0, &zone), "an absent timestamp remains absent");
}

namespace
{
	constexpr uint64_t test_ticks_per_ms = 10'000;
	constexpr uint64_t test_ticks_per_minute = 60'000 * test_ticks_per_ms;

	struct tabulation_case
	{
		std::string_view name;
		TIME_ZONE_INFORMATION zone;
		// 2024's transitions as UTC instants, and as the local readings the rules name.
		std::vector<uint64_t> instants;
		std::vector<uint64_t> readings;
	};

	TIME_ZONE_INFORMATION make_zone(const LONG bias, const LONG daylight_bias, const SYSTEMTIME daylight,
	                                const SYSTEMTIME standard)
	{
		TIME_ZONE_INFORMATION zone{};
		zone.Bias = bias;
		zone.DaylightBias = daylight_bias;
		zone.DaylightDate = daylight;
		zone.StandardDate = standard;
		return zone;
	}

	uint64_t stamp(const int month, const int day, const int hour, const int minute, const int second = 0,
	               const int ms = 0)
	{
		return df::date_t(2024, month, day, hour, minute, second).to_int64() + static_cast<uint64_t>(ms) *
			test_ticks_per_ms;
	}

	std::vector<uint64_t> samples_around(const std::vector<uint64_t>& changes, const bool include_gap_edges)
	{
		std::vector<uint64_t> result;
		const auto year_start = stamp(1, 1, 0, 0);

		for (auto hour = 0; hour < 366 * 24; hour += 6)
			result.push_back(year_start + static_cast<uint64_t>(hour) * 60 * test_ticks_per_minute + 1234567);

		for (const auto change : changes)
		{
			const auto at = [change](const int64_t minutes, const int64_t ms)
			{
				return static_cast<uint64_t>(static_cast<int64_t>(change) +
					minutes * static_cast<int64_t>(test_ticks_per_minute) +
					ms * static_cast<int64_t>(test_ticks_per_ms));
			};

			for (auto minute = -180; minute <= 180; ++minute)
				result.push_back(at(minute, 0));

			const auto edges = include_gap_edges ? std::vector<int>{-60, -30, 0, 30, 60} : std::vector<int>{0};
			for (const auto edge : edges)
			{
				for (auto ms = -3; ms <= 3; ++ms)
					result.push_back(at(edge, ms) + 4321);
			}
		}

		return result;
	}
}

// Dates are converted per item in search, grouping and summaries, and the system conversion costs
// microseconds, so each year's offsets are tabulated from the system once. The tables must agree
// with the system to the tick, including at the millisecond a transition takes effect, for zones
// whose rules span the new year, move by half an hour, or change just before midnight. Shifting a
// tabulated boundary by one millisecond fails this test.
static void should_tabulate_time_zone_offsets_like_the_system()
{
	const std::vector<tabulation_case> cases = {
		{
			"northern", make_zone(300, -60, {0, 3, 0, 2, 2, 0, 0, 0}, {0, 11, 0, 1, 2, 0, 0, 0}),
			{stamp(3, 10, 7, 0), stamp(11, 3, 6, 0)}, {stamp(3, 10, 2, 0), stamp(11, 3, 2, 0)}
		},
		{
			"southern", make_zone(-600, -60, {0, 10, 0, 1, 2, 0, 0, 0}, {0, 4, 0, 1, 3, 0, 0, 0}),
			{stamp(4, 6, 16, 0), stamp(10, 5, 16, 0)}, {stamp(4, 7, 3, 0), stamp(10, 6, 2, 0)}
		},
		{
			"half-hour daylight", make_zone(-630, -30, {0, 10, 0, 1, 2, 0, 0, 0}, {0, 4, 0, 1, 2, 0, 0, 0}),
			{stamp(4, 6, 15, 0), stamp(10, 5, 15, 30)}, {stamp(4, 7, 2, 0), stamp(10, 6, 2, 0)}
		},
		{
			"before midnight", make_zone(-120, -60, {0, 3, 4, 5, 23, 59, 59, 999}, {0, 10, 5, 5, 1, 0, 0, 0}),
			{stamp(3, 28, 21, 59, 59, 999), stamp(10, 24, 22, 0)},
			{stamp(3, 28, 23, 59, 59, 999), stamp(10, 25, 1, 0)}
		},
		{"no daylight", make_zone(-330, 0, {}, {}), {}, {}},
	};

	for (const auto& c : cases)
	{
		for (const auto instant : c.instants)
		{
			const auto earlier = instant - test_ticks_per_ms;
			assert_equal(true, platform::utc_to_local(earlier, &c.zone) - earlier !=
			             platform::utc_to_local(instant, &c.zone) - instant,
			             std::format("{}: the system changes offset at the stated instant", c.name));
		}

		for (const auto to_local : {true, false})
		{
			const auto stamps = samples_around(to_local ? c.instants : c.readings, !to_local);
			const auto tabulated = platform::tabulated_time_zone_conversions(stamps, c.zone, to_local);
			auto mismatches = 0;
			std::string first;

			for (auto i = 0u; i < stamps.size(); ++i)
			{
				const auto expected = to_local
					                      ? platform::utc_to_local(stamps[i], &c.zone)
					                      : platform::local_to_utc(stamps[i], &c.zone);
				if (tabulated[i] != expected && mismatches++ == 0)
					first = std::format(" first at {}: {} != {}", stamps[i], tabulated[i], expected);
			}

			assert_equal(0, mismatches, std::format("{} {} tabulation matches the system{}", c.name,
			                                        to_local ? "UTC-to-local" : "local-to-UTC", first));
		}
	}
}

// The tables serve whatever zone this machine is in, through the same rule selection as the
// per-call conversion: the year's dynamic rules, or standard time when adjustment is turned off.
static void should_convert_the_current_zone_like_the_system()
{
	DYNAMIC_TIME_ZONE_INFORMATION current{};
	assert_equal(true, GetDynamicTimeZoneInformation(&current) != TIME_ZONE_ID_INVALID, "current zone is known");

	for (const auto year : {2006, 2007, 2019, 2024})
	{
		TIME_ZONE_INFORMATION rules{};
		assert_equal(true, GetTimeZoneInformationForYear(static_cast<USHORT>(year), &current, &rules) != FALSE,
		             "the year's rules are known");
		if (current.DynamicDaylightTimeDisabled)
		{
			rules.StandardDate.wMonth = 0;
			rules.DaylightDate.wMonth = 0;
		}

		auto mismatches = 0;
		const auto start = df::date_t(year, 1, 1, 0, 0, 0).to_int64() + 7654321;

		for (auto half_day = 0; half_day < 730; ++half_day)
		{
			const auto ts = start + static_cast<uint64_t>(half_day) * 720 * test_ticks_per_minute;
			if (platform::utc_to_local(ts) != platform::utc_to_local(ts, &rules)) ++mismatches;
			if (platform::local_to_utc(ts) != platform::local_to_utc(ts, &rules)) ++mismatches;
		}

		assert_equal(0, mismatches, std::format("{} converts as the system does", year));
	}
}

// Converting each date through the system took about five microseconds; a large collection paid
// that for every date in every search and grouping. Removing the year cache fails this test.
static void should_tabulate_local_time_once_per_year()
{
	const auto start = df::date_t(2023, 1, 1, 0, 0, 0).to_int64();
	constexpr uint64_t step = 31'536'000'000ull;
	const auto before = platform::time_zone_system_queries();
	uint64_t total = 0;

	for (auto i = 0u; i < 10000; ++i)
	{
		const auto ts = start + i * step;
		total += platform::utc_to_local(ts) / 10000000;
		total += platform::local_to_utc(ts) / 10000000;
	}

	const auto queries = platform::time_zone_system_queries() - before;
	assert_equal(true, queries <= 1000,
	             std::format("{} system conversions for 20000 dates (checksum {})", queries, total));
}

static void should_initialize_unicode_ini_settings()
{
	const auto folder = _temps.next_folder("unicode-ini");
	const auto ini = folder.combine_file("diffractor.ini");
	const auto settings = platform::create_ini_file_settings(folder);
	const std::string value = "C:\\\xe6\x9d\xb1\xe4\xba\xac\\\xf0\x9f\x8e\xac.mp4";
	assert_equal(true, settings->write("paths", "movie", value), "Unicode setting is written");
	const auto bytes = df::blob_from_file(ini);
	assert_equal(true, bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe,
	             "new INI files explicitly select Unicode independent of the ANSI code page");

	const auto reopened = platform::create_ini_file_settings(folder);
	std::string read;
	assert_equal(true, reopened->read("paths", "movie", read), "setting remains readable after reopening");
	assert_equal(value, read, "the complete Unicode path survives");
	assert_equal(false, reopened->root_created(), "existing configuration is not replaced");
}

// The measured length excludes the terminator a string_view does not carry, so converting with -1
// both over-read the view and asked for one wchar more than the measurement allowed. The call then
// failed and left the buffer zeroed, which made every ordinary path convert to a run of NULs.
static void should_convert_utf8_to_ansi()
{
	assert_equal("photo.jpg", platform::utf8_to_a("photo.jpg"), "an ordinary name converts");
	assert_equal(9_z, platform::utf8_to_a("photo.jpg").size(), "and is not padded with terminators");
	assert_equal(true, platform::utf8_to_a("").empty(), "an empty name converts to an empty name");

	// The source has no terminator of its own: the conversion must respect the view's length rather
	// than read past it.
	const std::string backing("C:\\photos\\holiday.jpgTRAILING");
	assert_equal("C:\\photos\\holiday.jpg", platform::utf8_to_a(std::string_view(backing).substr(0, 21)),
	             "a view stops where it says it stops");

	// The console commands run with a UTF-8 C locale, where each of these characters takes three
	// bytes. An output sized at two bytes per UTF-16 unit truncated a name that is more CJK than
	// ASCII, and a truncated conversion answers empty - so the file could not be opened by name.
	const std::string cjk = "D:\\\xe5\x8b\x95\xe7\x94\xbb\\\xe6\x9d\xb1\xe4\xba\xac\xe6\x97\x85\xe8\xa1\x8c"
		"\xe3\x81\xae\xe6\x80\x9d\xe3\x81\x84\xe5\x87\xba.mp4";
	assert_equal(cjk, platform::utf8_to_a(cjk), "a name of three-byte characters converts whole");
}

// Undocking or unplugging a second display leaves a saved rect that intersects no monitor at all.
static void should_restore_a_window_onto_a_display()
{
	constexpr recti work(0, 0, 1920, 1040);

	const auto from_a_missing_display = platform::fit_window_to_work_area({2200, 300, 3000, 900}, work);
	assert_equal(800, from_a_missing_display.width(), "width is preserved");
	assert_equal(600, from_a_missing_display.height(), "height is preserved");
	assert_equal(true, work.intersects(from_a_missing_display), "and the window lands on the display");
	assert_equal(1120, from_a_missing_display.left, "pushed just inside the right edge");
	assert_equal(300, from_a_missing_display.top, "the axis that already fitted is left alone");

	const auto from_the_left = platform::fit_window_to_work_area({-3000, -400, -2600, -100}, work);
	assert_equal(0, from_the_left.left, "a negative origin comes back to the work area");
	assert_equal(0, from_the_left.top, "on both axes");

	const auto too_large = platform::fit_window_to_work_area({4000, 0, 8000, 3000}, work);
	assert_equal(1920, too_large.width(), "a window larger than the display is clamped to it");
	assert_equal(1040, too_large.height(), "on both axes");
	assert_equal(0, too_large.left, "and positioned at the origin rather than hanging off");

	// Which saved rects are corrected at all. Reaching a display is not on its own enough to be usable.
	assert_equal(true, platform::window_needs_refit({2200, 300, 3000, 900}, work, false),
	             "a rect saved on a display that is gone");
	assert_equal(false, platform::window_needs_refit({100, 100, 900, 700}, work, true),
	             "an ordinary rect is restored exactly as it was saved");
	assert_equal(false, platform::window_needs_refit({1500, 100, 2400, 700}, work, true),
	             "and so is one deliberately straddling two displays");
	assert_equal(true, platform::window_needs_refit({0, 0, 3840, 2160}, work, true),
	             "a window saved on a larger display is clamped to this one");
	assert_equal(true, platform::window_needs_refit({1900, 100, 2700, 700}, work, true),
	             "a sliver at the edge leaves nothing to grab");
	assert_equal(true, platform::window_needs_refit({100, 1030, 900, 1630}, work, true),
	             "and neither has one pushed off the bottom");
}

static void should_classify_dxgi_device_loss()
{
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_REMOVED), "device removed");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_RESET), "device reset");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_HUNG), "device hung");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DRIVER_INTERNAL_ERROR), "driver internal error");
	assert_equal(false, is_device_loss_error(DXGI_STATUS_OCCLUDED), "occlusion is not device loss");
	assert_equal(false, is_device_loss_error(E_FAIL), "generic failure is not device loss");
	assert_equal(false, is_device_loss_error(S_OK), "success is not device loss");
}

static void should_use_screen_coordinates_for_saved_window_placement()
{
	const recti placement(100, 100, 900, 700);
	const auto top_taskbar = platform::placement_to_screen(placement, {0, 0, 1920, 1080}, {0, 40, 1920, 1080});
	assert_equal(140, top_taskbar.top, "workspace origin is below a top taskbar");
	assert_equal(800, top_taskbar.width(), "conversion does not resize the window");
	const auto left_taskbar = platform::placement_to_screen(placement, {0, 0, 1920, 1080}, {48, 0, 1920, 1080});
	assert_equal(148, left_taskbar.left, "workspace origin is beside a left taskbar");
	const auto secondary = platform::placement_to_screen({-1800, 100, -1000, 700},
	                                                    {-1920, 0, 0, 1080}, {-1920, 40, 0, 1080});
	assert_equal(-1800, secondary.left, "secondary-monitor screen origin is retained");
	assert_equal(140, secondary.top, "secondary-monitor work offset is applied once");

	// Full screen goes the other way: monitor bounds in screen coordinates become a placement.
	const auto full_screen = platform::screen_to_placement({0, 0, 1920, 1080}, {0, 0, 1920, 1080}, {48, 0, 1920, 1080});
	assert_equal(-48, full_screen.left, "monitor bounds start left of a left taskbar's workspace origin");
	assert_equal(1920, full_screen.width(), "conversion does not resize a full-screen window");
	const auto round_trip = platform::screen_to_placement(secondary, {-1920, 0, 0, 1080}, {-1920, 40, 0, 1080});
	assert_equal(100, round_trip.top, "screen to placement inverts placement to screen");
	assert_equal(-1800, round_trip.left, "on both axes");
}

static void should_not_treat_pointer_wheel_ids_as_modifiers()
{
	for (const auto message : {0x024eu, 0x024fu})
	{
		const auto none = platform::wheel_key_state(message, MAKEWPARAM(12, WHEEL_DELTA), {});
		assert_equal(false, none.control || none.shift, "pointer ID bits are not modifier flags");
		const auto held = platform::wheel_key_state(message, MAKEWPARAM(1, WHEEL_DELTA), {true, true, false});
		assert_equal(true, held.control && held.shift, "pointer wheels use keyboard state");
	}
	const auto mouse = platform::wheel_key_state(WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), {});
	assert_equal(true, mouse.control, "mouse wheel flags retain their existing meaning");
	assert_equal(false, mouse.shift, "unpressed mouse modifiers stay clear");
}

// A machine with no working GPU driver is handed WARP posing as a hardware adapter. Drawn through, it
// redrew the whole window on the CPU every frame, so it is recognised and the CPU backend used instead.
static void should_recognise_the_basic_render_driver()
{
	assert_equal(true, is_software_adapter(0x1414, 0x8c, 0), "the Microsoft Basic Render Driver");
	assert_equal(true, is_software_adapter(0x10de, 0x2684, DXGI_ADAPTER_FLAG_SOFTWARE),
	             "any adapter that says it is software");
	assert_equal(false, is_software_adapter(0x8086, 0x3ea0, 0), "an Intel integrated GPU");
	assert_equal(false, is_software_adapter(0x10de, 0x2684, 0), "an NVIDIA card");
	assert_equal(false, is_software_adapter(0x1414, 0x5353, 0), "the vendor alone does not make an adapter WARP");
}

// NV12 and P010 are drawn by sampling views of their planes. A device that could only hold the format
// accepted the texture, refused its views, and the picture never appeared.
static void should_need_sampling_for_planar_video()
{
	assert_equal(false, can_sample_texture_format(D3D11_FORMAT_SUPPORT_TEXTURE2D), "holding the format is not enough");
	assert_equal(false, can_sample_texture_format(D3D11_FORMAT_SUPPORT_SHADER_SAMPLE),
	             "nor is sampling a format that cannot be a 2D texture");
	assert_equal(false, can_sample_texture_format(0), "nothing supported");
	assert_equal(true, can_sample_texture_format(D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
		             D3D11_FORMAT_SUPPORT_DECODER_OUTPUT), "a device that samples it");
}

// Intel parts report the 128MB carved out at boot as dedicated memory, and allocate textures from shared
// memory. Read as the card's own, it held every Intel laptop at the 64MB floor, and a photo over 16
// megapixels was shown downscaled even at 100%.
static void should_count_shared_memory_for_an_integrated_gpu()
{
	constexpr uint64_t mb = 1024ull * 1024ull;
	constexpr uint64_t gb = 1024ull * mb;
	constexpr auto laptop = static_cast<int64_t>(16 * gb);
	const auto budget = [](const uint64_t gpu_bytes, const int64_t total_phys)
	{
		return static_cast<uint64_t>(calc_texture_budget(gpu_bytes, total_phys));
	};

	assert_equal(128 * mb + 8 * gb, gpu_texture_memory(128 * mb, 8 * gb, true), "an integrated part uses shared memory");
	assert_equal(8 * gb, gpu_texture_memory(8 * gb, 16 * gb, false), "a discrete card uses its own");
	assert_equal(8 * gb, gpu_texture_memory(0, 8 * gb, false), "an adapter with none of its own still has somewhere");

	assert_equal(128 * mb, budget(gpu_texture_memory(128 * mb, 8 * gb, true), laptop),
	             "an integrated laptop GPU is given the full ceiling");
	assert_equal(64 * mb, budget(128 * mb, laptop), "which the carve-out alone held at the floor");
	assert_equal(64 * mb, budget(512 * mb, laptop), "a small discrete card is still held back");
	assert_equal(128 * mb, budget(8 * gb, laptop), "a large card is held to the ceiling");
	assert_equal(96 * mb, budget(gpu_texture_memory(128 * mb, 768 * mb, true), static_cast<int64_t>(1536 * mb)),
	             "physical memory still bounds a small machine");
	assert_equal(128 * mb, budget(0, 0), "with nothing to ask, the shipped ceiling");
}

static bool same_adapter(const LUID a, const LUID b)
{
	return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

struct listed_adapter
{
	LUID luid = {};
	bool makes_video_devices = false;
};

// In the order Windows lists them, so the first is the default FFmpeg would take when left to choose.
static std::vector<listed_adapter> list_adapters()
{
	std::vector<listed_adapter> result;
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;

	ComPtr<IDXGIAdapter1> adapter;

	for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i)
	{
		DXGI_ADAPTER_DESC1 desc = {};
		if (FAILED(adapter->GetDesc1(&desc))) continue;

		ComPtr<ID3D11Device> device;
		const auto makes_video = SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		                                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
		                                                     D3D11_SDK_VERSION, &device, nullptr, nullptr));
		result.push_back({desc.AdapterLuid, makes_video});
	}

	return result;
}

static LUID adapter_of(const AVBufferRef* hw_device)
{
	const auto* const ctx = reinterpret_cast<const AVHWDeviceContext*>(hw_device->data);
	const auto* const hwctx = static_cast<const AVD3D11VADeviceContext*>(ctx->hwctx);
	ComPtr<IDXGIDevice> dxgi_device;
	ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc = {};

	if (hwctx->device && SUCCEEDED(hwctx->device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
		SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
	{
		return desc.AdapterLuid;
	}

	return {};
}

// Left to itself FFmpeg made the decode device on whichever adapter was the default when the first
// video opened. Where that was not the renderer's, the shared-texture bridge could not cross to it and
// every frame went through system memory. The adapters asked about here are the last ones listed,
// which are never that default wherever there are two: on every machine since Windows 8 the
// Microsoft Basic Render Driver is listed after the GPUs.
static void should_create_the_decode_device_on_the_render_adapter()
{
	assert_equal(true, av_platform_create_hw_device(AV_HWDEVICE_TYPE_DXVA2) == nullptr,
	             "a device the renderer cannot present from is not made");

	const auto adapters = list_adapters();
	if (adapters.empty()) return;

	const auto last = adapters.back().luid;
	DXGI_ADAPTER_DESC1 found = {};
	const auto adapter = find_adapter_by_luid(last);
	assert_equal(true, adapter && SUCCEEDED(adapter->GetDesc1(&found)) && same_adapter(last, found.AdapterLuid),
	             "an adapter is found by what it is, not where it is listed");
	assert_equal(true, find_adapter_by_luid({}) == nullptr, "and none for the renderer that is not there");

	const auto video = std::ranges::find_if(adapters.rbegin(), adapters.rend(),
	                                        [](const listed_adapter& a) { return a.makes_video_devices; });
	if (video == adapters.rend()) return;

	const auto previous = render_adapter();
	const df::scope_exit restore([previous] { publish_render_adapter(previous); });
	publish_render_adapter(video->luid);

	auto* device = av_platform_create_hw_device(AV_HWDEVICE_TYPE_D3D11VA);
	const df::scope_exit release([&device] { av_buffer_unref(&device); });

	assert_equal(true, device != nullptr, "a decode device is made");
	if (!device) return;

	assert_equal(true, same_adapter(video->luid, adapter_of(device)), "on the adapter the renderer draws with");
	assert_equal(true, av_platform_hw_device_usable(device), "and FFmpeg can decode on it");
}

// The renderer copies a hardware picture out of the decoder's surface under the lock the decoder takes
// around it, so the lock a picture lends must be that device's own: its functions, with its context.
static void should_lend_the_renderer_the_decode_device_lock()
{
	auto* device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	assert_equal(true, device != nullptr, "a D3D11VA device context");
	if (!device) return;
	const df::scope_exit release_device([&device] { av_buffer_unref(&device); });

	auto held = 0;
	auto* const hwctx = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);
	hwctx->lock = [](void* context) { ++*static_cast<int*>(context); };
	hwctx->unlock = [](void* context) { --*static_cast<int*>(context); };
	hwctx->lock_ctx = &held;

	auto* frames = av_hwframe_ctx_alloc(device);
	assert_equal(true, frames != nullptr, "a frames context on it");
	if (!frames) return;
	const df::scope_exit release_frames([&frames] { av_buffer_unref(&frames); });

	ui::video_picture picture;
	av_platform_decode_lock(reinterpret_cast<const AVHWFramesContext*>(frames->data), picture);
	assert_equal(true, picture.lock != nullptr && picture.unlock != nullptr, "the picture carries a lock");

	if (picture.lock && picture.unlock)
	{
		picture.lock(picture.lock_context);
		assert_equal(1, held, "which is the device's own");
		picture.unlock(picture.lock_context);
		assert_equal(0, held, "and is released with the device's own");
	}

	ui::video_picture none;
	av_platform_decode_lock(nullptr, none);
	assert_equal(true, none.lock == nullptr && none.unlock == nullptr, "a picture with no device carries none");
}

// Defined in platform_win_sound.cpp: what the device's format() answers for its mix format.
platform::audio_output_format wasapi_mix_format(const WAVEFORMATEX* format);

// An endpoint's mix format is a WAVEFORMATEX, extended or not, which the device describes as a sample
// format and the channels with the speakers they feed. An extension too short to hold its speaker mask
// names neither its samples nor its speakers.
static void should_describe_a_wasapi_mix_format()
{
	constexpr GUID ieee_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
	constexpr GUID pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

	WAVEFORMATEXTENSIBLE mix = {};
	mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	mix.Format.nChannels = 6;
	mix.Format.nSamplesPerSec = 48000;
	mix.Format.wBitsPerSample = 32;
	mix.Format.nBlockAlign = 24;
	mix.Format.nAvgBytesPerSec = 48000 * 24;
	mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	mix.Samples.wValidBitsPerSample = 32;
	mix.dwChannelMask = 0x3F; // 5.1: front left, right and centre, LFE, back left and right
	mix.SubFormat = ieee_float;

	const auto described = wasapi_mix_format(&mix.Format);
	assert_equal(48000u, described.sample_rate, "the endpoint's rate");
	assert_equal(true, described.sample_format == platform::audio_sample_format::float32, "its float samples");
	assert_equal(6u, described.channels, "its channels");
	assert_equal(true, described.channel_mask == 0x3F, "and the speakers they feed");

	mix.SubFormat = pcm;
	assert_equal(true, wasapi_mix_format(&mix.Format).sample_format == platform::audio_sample_format::int32,
	             "32-bit integer samples");

	mix.Format.cbSize = 0;
	const auto short_extension = wasapi_mix_format(&mix.Format);
	assert_equal(true, short_extension.sample_format == platform::audio_sample_format::none,
	             "an extension too short for its mask names no samples");
	assert_equal(true, short_extension.channel_mask == 0, "and no speakers");

	WAVEFORMATEX stereo = {};
	stereo.wFormatTag = WAVE_FORMAT_PCM;
	stereo.nChannels = 2;
	stereo.nSamplesPerSec = 44100;
	stereo.wBitsPerSample = 16;
	stereo.nBlockAlign = 4;
	stereo.nAvgBytesPerSec = 44100 * 4;
	const auto plain = wasapi_mix_format(&stereo);
	assert_equal(true, plain.sample_format == platform::audio_sample_format::int16, "a plain format's 16-bit samples");
	assert_equal(2u, plain.channels, "on its channels");
	assert_equal(true, plain.channel_mask == 0, "names no speakers");

	stereo.wBitsPerSample = 24;
	stereo.nBlockAlign = 6;
	assert_equal(true, wasapi_mix_format(&stereo).sample_format == platform::audio_sample_format::none,
	             "packed 24-bit samples are not spoken");
	assert_equal(0u, wasapi_mix_format(nullptr).channels, "and no format describes no channels");
}

// WARP is in every Windows since 8 and needs no GPU, so the texture tests run the same everywhere.
static ComPtr<ID3D11Device> create_warp_device(ComPtr<ID3D11DeviceContext>& context)
{
	ComPtr<ID3D11Device> device;

	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
	                             0, D3D11_SDK_VERSION, &device, nullptr, &context)))
	{
		return {};
	}

	return device;
}

static ComPtr<ID3D11Texture2D> create_plain_texture(ID3D11Device* device, const sizei dims)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = dims.cx;
	desc.Height = dims.cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	ComPtr<ID3D11Texture2D> texture;
	device->CreateTexture2D(&desc, nullptr, &texture);
	return texture;
}

// Thumbnails come and go by the hundred as a listing scrolls, and each was a CreateTexture2D on the UI
// thread. A released texture is handed to the next one of its exact size and format - but never while
// a scene a redraw can replay still draws it, or that redraw would show the new picture in its place.
static void should_recycle_a_texture_only_when_nothing_draws_it()
{
	ComPtr<ID3D11DeviceContext> context;
	const auto device = create_warp_device(context);
	assert_equal(true, device != nullptr, "a WARP device");
	if (!device) return;

	constexpr sizei thumb = {64, 48};
	constexpr auto argb = ui::texture_format::ARGB;
	const auto thumb_bytes = d3d11_texture_pool::texture_bytes(thumb, argb);
	d3d11_texture_pool pool(thumb_bytes * 8);

	const auto released = create_plain_texture(device.Get(), thumb);
	pool.give({thumb, argb, released, {}});
	assert_equal(true, pool.take({64, 50}, argb).texture == nullptr, "a different size is not handed out");
	assert_equal(true, pool.take(thumb, ui::texture_format::RGB).texture == nullptr, "nor a different format");
	assert_equal(true, pool.take(thumb, argb).texture == released, "the same size and format is");
	assert_equal(0_z, pool.size(), "and leaves the pool");

	auto drawn = std::make_shared<const texture_views>();
	pool.give({thumb, argb, released, drawn});
	assert_equal(true, pool.take(thumb, argb).texture == nullptr, "not while a scene still draws it");
	drawn.reset();
	assert_equal(true, pool.take(thumb, argb).texture == released, "but once nothing does");

	std::vector<ComPtr<ID3D11Texture2D>> made;

	for (auto i = 0; i < 9; ++i)
	{
		made.emplace_back(create_plain_texture(device.Get(), thumb));
		pool.give({thumb, argb, made.back(), {}});
	}

	assert_equal(8_z, pool.size(), "the budget holds eight");
	assert_equal(true, pool.bytes() <= thumb_bytes * 8, "and never more than it");
	assert_equal(true, pool.take(thumb, argb).texture == made[1], "the oldest went to make room");

	const auto photograph = create_plain_texture(device.Get(), {128, 128});
	const auto before = pool.size();
	pool.give({{128, 128}, argb, photograph, {}});
	assert_equal(before, pool.size(), "a texture over an eighth of the budget is not kept");

	pool.clear();
	assert_equal(0_z, pool.size(), "a cleared pool holds nothing");
	assert_equal(0_z, pool.bytes(), "and costs nothing");
}

static bool texture_holds(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                          const uint32_t bgra)
{
	D3D11_TEXTURE2D_DESC desc = {};
	texture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	ComPtr<ID3D11Texture2D> staging;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;

	context->CopyResource(staging.Get(), texture);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

	auto holds = true;

	for (UINT y = 0; y < desc.Height && holds; ++y)
	{
		const auto* const row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) +
			static_cast<size_t>(mapped.RowPitch) * y);
		holds = std::all_of(row, row + desc.Width, [bgra](const uint32_t px) { return px == bgra; });
	}

	context->Unmap(staging.Get(), 0);
	return holds;
}

// A recycled texture still holds the last picture until it is overwritten, so the reuse is only right if
// the new picture lands in it in full. Read back from the texture the second picture was given, which is
// the one the first picture released.
static void should_overwrite_a_recycled_texture_with_the_new_picture()
{
	ComPtr<ID3D11DeviceContext> context;
	const auto device = create_warp_device(context);
	assert_equal(true, device != nullptr, "a WARP device");
	if (!device) return;

	const auto f = std::make_shared<factories>();
	f->d3d_device = device;
	f->d3d_context = context;
	f->texture_pool = std::make_unique<d3d11_texture_pool>(1024 * 1024);

	constexpr sizei dims = {64, 48};
	constexpr auto stride = static_cast<size_t>(dims.cx) * 4;
	constexpr auto bytes = stride * dims.cy;
	constexpr uint32_t red = 0xFFFF0000u;
	constexpr uint32_t green = 0xFF00FF00u;
	const std::vector<uint32_t> first_picture(static_cast<size_t>(dims.cx) * dims.cy, red);
	const std::vector<uint32_t> second_picture(static_cast<size_t>(dims.cx) * dims.cy, green);
	const auto created_before = df::gpu_perf.textures_created.load();
	const auto reused_before = df::gpu_perf.textures_reused.load();

	{
		const auto first = d3d11_create_texture(f);
		assert_equal(true, first->update(dims, ui::texture_format::ARGB, ui::orientation::top_left,
		                                 reinterpret_cast<const uint8_t*>(first_picture.data()), stride, bytes) ==
		             ui::texture_update_result::tex_created, "the first picture makes a texture");
	}

	assert_equal(1_z, f->texture_pool->size(), "which is kept when it is released");
	auto kept = f->texture_pool->take(dims, ui::texture_format::ARGB);
	const auto first_texture = kept.texture;
	f->texture_pool->give(std::move(kept));

	{
		const auto second = d3d11_create_texture(f);
		assert_equal(true, second->update(dims, ui::texture_format::ARGB, ui::orientation::top_left,
		                                  reinterpret_cast<const uint8_t*>(second_picture.data()), stride, bytes) ==
		             ui::texture_update_result::tex_created, "a texture taken from the pool is new to its caller");
		assert_equal(0_z, f->texture_pool->size(), "the second picture took it");
	}

	assert_equal(1ull, df::gpu_perf.textures_created.load() - created_before, "one texture was made");
	assert_equal(1ull, df::gpu_perf.textures_reused.load() - reused_before, "and one reused");

	const auto returned = f->texture_pool->take(dims, ui::texture_format::ARGB);
	assert_equal(true, returned.texture == first_texture, "the second picture was drawn into the first texture");
	assert_equal(true, returned.texture && texture_holds(device.Get(), context.Get(), returned.texture.Get(), green),
	             "and holds the new picture, not the old one");
}

// A decoded frame as the renderers see it, described by the test rather than by a decoder.
class fake_video_frame final : public ui::video_frame
{
public:
	ui::const_surface_ptr prepared;
	ui::video_picture described;
	mutable int cpu_requests = 0;

	ui::const_surface_ptr prepared_surface() const override { return prepared; }
	ui::video_picture picture() const override { return described; }
	void request_cpu_frames() const override { ++cpu_requests; }
};

// Converts any frame into a 32 x 16 picture, and counts the frames it was asked for.
class fake_frame_scaler final : public ui::surface_scaler
{
public:
	std::shared_ptr<int> frames;

	bool scale_surface(const ui::const_surface_ptr&, ui::surface_ptr&, sizei, bool) override { return false; }
	bool convert_yuv_surface(const ui::surface&, const ui::surface_ptr&) override { return false; }

	bool scale_surface(const ui::video_frame&, const ui::surface_ptr& surface_out) override
	{
		++*frames;
		return surface_out->alloc(32, 16, ui::texture_format::RGB) != nullptr;
	}
};

static std::function<std::unique_ptr<ui::surface_scaler>()> make_fake_frame_scaler(std::shared_ptr<int> frames)
{
	return [frames]() -> std::unique_ptr<ui::surface_scaler>
	{
		auto s = std::make_unique<fake_frame_scaler>();
		s->frames = frames;
		return s;
	};
}

static std::shared_ptr<fake_video_frame> make_prepared_video_frame(const sizei dimensions)
{
	auto frame = std::make_shared<fake_video_frame>();
	const auto picture = std::make_shared<ui::surface>();
	picture->alloc(dimensions, ui::texture_format::RGB);
	frame->prepared = picture;
	return frame;
}

// A texture presents a video frame through the frame's interface alone: the picture the decode thread
// prepared when there is one, and otherwise one the application's scaler converts. A hardware picture
// is copied only under its decode device's lock, so one that brings no lock is refused, and with no
// scaler there is nothing to fall back to.
static void should_present_a_video_frame_through_its_interface()
{
	ComPtr<ID3D11DeviceContext> context;
	const auto device = create_warp_device(context);
	assert_equal(true, device != nullptr, "a WARP device");
	if (!device) return;

	const auto f = std::make_shared<factories>();
	f->d3d_device = device;
	f->d3d_context = context;
	f->texture_pool = std::make_unique<d3d11_texture_pool>(1024 * 1024);

	const auto frames_converted = std::make_shared<int>(0);
	const auto restore_scaler = ui::make_surface_scaler;
	const df::scope_exit restore([restore_scaler] { ui::make_surface_scaler = restore_scaler; });
	ui::make_surface_scaler = make_fake_frame_scaler(frames_converted);

	const auto texture = d3d11_create_texture(f);
	assert_equal(true, texture->update(make_prepared_video_frame({64, 48})) == ui::texture_update_result::tex_created,
	             "a prepared picture is uploaded");
	assert_equal(true, texture->dimensions() == sizei{64, 48}, "as it was prepared");
	assert_equal(0, *frames_converted, "with nothing converted");

	const auto unprepared = std::make_shared<fake_video_frame>();
	assert_equal(true, texture->update(unprepared) != ui::texture_update_result::failed,
	             "a picture in system memory is converted by the scaler");
	assert_equal(true, texture->dimensions() == sizei{32, 16}, "and uploaded as converted");
	assert_equal(1, *frames_converted, "once");
	assert_equal(0, unprepared->cpu_requests, "which asks the decoder for nothing");

	const auto unlocked = std::make_shared<fake_video_frame>();
	auto texture_name = 0;
	unlocked->described.texture = &texture_name;
	assert_equal(true, d3d11_create_texture(f)->update(unlocked) == ui::texture_update_result::failed,
	             "a hardware picture without its device's lock is refused");
	assert_equal(1, *frames_converted, "rather than converted");

	ui::make_surface_scaler = nullptr;
	assert_equal(true, d3d11_create_texture(f)->update(unprepared) == ui::texture_update_result::failed,
	             "and with no scaler a picture in system memory cannot be presented");
}

// The software renderer never samples the decoder's surfaces, so a hardware picture is converted by
// the scaler like any other, and the decode thread is asked to deliver the rest of the clip in system
// memory instead.
static void should_present_a_video_frame_in_software_through_its_interface()
{
	const auto frames_converted = std::make_shared<int>(0);
	const auto restore_scaler = ui::make_surface_scaler;
	const df::scope_exit restore([restore_scaler] { ui::make_surface_scaler = restore_scaler; });
	ui::make_surface_scaler = make_fake_frame_scaler(frames_converted);

	const auto renderer = create_software_draw_context(nullptr, nullptr, false, normal_font_size);
	const auto texture = renderer->create_texture();
	assert_equal(true, texture->update(make_prepared_video_frame({64, 48})) == ui::texture_update_result::tex_created,
	             "a prepared picture is copied");
	assert_equal(true, texture->dimensions() == sizei{64, 48}, "as it was prepared");
	assert_equal(0, *frames_converted, "with nothing converted");

	const auto unprepared = std::make_shared<fake_video_frame>();
	assert_equal(true, texture->update(unprepared) == ui::texture_update_result::tex_created,
	             "a picture in system memory is converted by the scaler");
	assert_equal(true, texture->dimensions() == sizei{32, 16}, "and held as converted");
	assert_equal(1, *frames_converted, "once");
	assert_equal(0, unprepared->cpu_requests, "which asks the decoder for nothing");

	const auto hardware = std::make_shared<fake_video_frame>();
	auto texture_name = 0;
	hardware->described.texture = &texture_name;
	assert_equal(true, texture->update(hardware) == ui::texture_update_result::tex_updated,
	             "a hardware picture is converted too");
	assert_equal(2, *frames_converted, "by the scaler");
	assert_equal(1, hardware->cpu_requests, "and the decoder is asked for pictures in system memory");

	ui::make_surface_scaler = nullptr;
	assert_equal(true,
	             renderer->create_texture()->update(unprepared) == ui::texture_update_result::failed,
	             "with no scaler a picture in system memory cannot be presented");
}

static void should_suppress_gpu_for_recovery_session()
{
	platform::suppress_crash_guard(platform::crash_guard::gpu_render, true);
	// Suppression is process-global; a failing assertion below must not leave it on.
	const df::scope_exit restore_guard([]
	{
		platform::suppress_crash_guard(platform::crash_guard::gpu_render, false);
	});
	assert_equal(true, platform::crash_guard_suppressed(platform::crash_guard::gpu_render),
	             "GPU suppressed during recovery");
	assert_equal(false, platform::crash_guard_suppressed(platform::crash_guard::hw_video_decode),
	             "decode suppression remains independent");
	platform::suppress_crash_guard(platform::crash_guard::gpu_render, false);
	assert_equal(false, platform::crash_guard_suppressed(platform::crash_guard::gpu_render),
	             "GPU suppression can be cleared");
}

// Issue #219 - Font glyph fallback for missing (Hangul) glyphs.
// The custom UI renders text with the system font "Calibri" (factories::font_face),
// which has NO Hangul. DirectWrite substitutes a fallback face (e.g. Malgun Gothic)
// for Korean. This test verifies the fallback path resolves Korean glyphs, and
// guards the render_glyph fix: metrics for a glyph must be read from the glyph
// run's OWN face (glyph_run->fontFace), not the primary UI font (_face). Reading
// them from the primary face returns a different glyph's metrics (or fails for an
// out-of-range index, dropping the glyph). render_glyph now queries glyph_face.
static void should_fall_back_for_missing_glyphs()
{
	constexpr char32_t hangul = U'\uAC00'; // 가

	// "Calibri" is the app's dialog/UI font; "Malgun Gothic" is the Windows Korean font.
	const auto probe = platform::probe_glyph_fallback("Calibri", "Malgun Gothic", hangul);

	if (!probe.available)
	{
		// Neither font is guaranteed on every machine. Assert the probe reported that
		// honestly rather than silently passing with nothing checked.
		assert_equal(0, probe.primary_glyph, "an unavailable probe reports no glyph data");
		return;
	}

	// 1. The primary UI font genuinely lacks Hangul -> fallback is mandatory.
	assert_equal(0, probe.primary_glyph, "Calibri has no Hangul glyph (fallback required)");

	// 2. The fallback face maps the same character to a real glyph, and querying
	//    that face for the glyph (what render_glyph SHOULD do) succeeds.
	assert_equal(true, probe.fallback_glyph != 0, "fallback face has a Hangul glyph");
	assert_equal(true, probe.fallback_metrics_ok, "fallback-face metrics query succeeds (correct face)");

	// 3. Querying the PRIMARY face for the same (fallback) glyph index yields the
	//    wrong glyph's metrics - the latent render_glyph bug.
	assert_equal(true, probe.primary_metrics_differ,
	             "primary-face metrics for a fallback glyph are wrong (latent bug)");
}

// Issue #232 - a missing Calibri produced a huge log, because every draw re-ran the family
// lookup and re-logged the failure. Issue #189 - after toggling Large Font some glyphs kept
// drawing at the old size. Both are properties of the same font-face cache: a repeated
// request must be served from the cache, and the requested size must be part of the identity
// so a size change cannot return the previous face.
static void should_cache_font_faces_per_face_and_size()
{
	const auto probe = platform::probe_font_cache(16);

	if (!probe.available)
	{
		// No text engine on this machine. Say so rather than passing with nothing checked.
		assert_equal(0, probe.entries_after_first, "an unavailable probe reports no cache entries");
		return;
	}

	assert_equal(1, probe.entries_after_first, "one request caches exactly one face");

	// #232: the repeat is answered from the cache, so no lookup and no log line repeats.
	assert_equal(true, probe.same_request_is_cached, "an identical request is served from the cache");

	// #189: the size is part of the cache identity.
	assert_equal(true, probe.size_change_is_distinct, "a font-size change yields a distinct face");
	assert_equal(true, probe.face_change_is_distinct, "a face-type change yields a distinct face");

	// A settings change resets the fonts; nothing may survive that.
	assert_equal(true, probe.reset_clears_cache, "resetting fonts empties the cache");
}

// #189 again, one level down: a cached glyph raster belongs to a size as well as to a face.
// IDWriteFontFace carries no size - the size lives on the glyph run - so a cache keyed on the
// face alone serves a raster made at the previous font size to text drawn at the current one,
// which is how a popup ended up mixing glyph sizes within one string.
static void should_key_glyph_cache_by_size()
{
	glyph_face_keys keys;

	assert_equal(true, keys.key(nullptr, 16.0f, 42) == keys.key(nullptr, 16.0f, 42),
	             "the same face, size and glyph give the same key");
	assert_equal(true, keys.key(nullptr, 16.0f, 42) != keys.key(nullptr, 24.0f, 42),
	             "a font-size change yields a distinct glyph key");
	assert_equal(true, keys.key(nullptr, 16.0f, 42) != keys.key(nullptr, 16.0f, 43),
	             "a different glyph yields a distinct key");
}

class registry_test_root
{
	std::string _root;
	std::wstring _root_w;

public:
	explicit registry_test_root(const std::string_view name)
	{
		_root = std::format("Software\\DiffractorTest\\{}-{}", name, platform::tick_count());
		_root_w = str::utf8_to_utf16(_root);
	}

	~registry_test_root()
	{
		RegDeleteTreeW(HKEY_CURRENT_USER, _root_w.c_str());
	}

	registry_test_root(const registry_test_root&) = delete;
	registry_test_root& operator=(const registry_test_root&) = delete;

	const std::string& path() const
	{
		return _root;
	}

	std::wstring subkey(const std::string_view section) const
	{
		return str::utf8_to_utf16(std::format("{}\\{}", _root, section));
	}
};

static void should_persist_to_registry()
{
	const registry_test_root root("strings");
	const auto archive = platform::create_registry_settings(root.path());

	const std::vector<std::string> vals = {
		"Hello World"s,
		"\r\n\t hello"s,
		"Доброго ранку!"s,
		"Japanese こんにちは世界"s,
		"Доброго ранку!"s,
		std::string(64, 'x')
	};

	for (const auto& expected : vals)
	{
		std::string actual;
		archive->write({}, "test", expected);
		archive->read({}, "test", actual);

		assert_equal_strict(expected, actual, "Persist To Registry");
	}
}

static void should_validate_registry_value_types_and_sizes()
{
	const registry_test_root root("malformed");
	const auto section = std::format("test-malformed-{}", platform::tick_count());
	const auto sectionW = root.subkey(section);
	HKEY key = nullptr;
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegCreateKeyExW(HKEY_CURRENT_USER, sectionW.c_str(), 0, nullptr,
	                                              REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr)),
	             "create registry test section");
	const df::scope_exit close_key([&key] { if (key) RegCloseKey(key); });

	constexpr wchar_t text[] = L"text";
	constexpr uint16_t short_number = 42;
	constexpr wchar_t unterminated[] = {L'r', L'a', L'w'};
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegSetValueExW(key, L"wrong-number-type", 0, REG_SZ,
	                                             reinterpret_cast<const BYTE*>(text), sizeof(text))),
	             "write wrong-number-type");
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegSetValueExW(key, L"short-number", 0, REG_DWORD,
	                                             reinterpret_cast<const BYTE*>(&short_number),
	                                             sizeof(short_number))),
	             "write short-number");
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegSetValueExW(key, L"unterminated-string", 0, REG_SZ,
	                                             reinterpret_cast<const BYTE*>(unterminated),
	                                             sizeof(unterminated))),
	             "write unterminated-string");
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegSetValueExW(key, L"wrong-binary-type", 0, REG_SZ,
	                                             reinterpret_cast<const BYTE*>(text), sizeof(text))),
	             "write wrong-binary-type");
	RegCloseKey(key);
	key = nullptr;

	{
		const auto archive = platform::create_registry_settings(root.path());
		uint32_t number = 99;
		assert_equal(false, archive->read(section, "wrong-number-type", number), "reject numeric type");
		assert_equal(99u, number, "preserve numeric output after wrong type");
		assert_equal(false, archive->read(section, "short-number", number), "reject short numeric value");
		assert_equal(99u, number, "preserve numeric output after short value");

		std::string string_value;
		assert_equal(true, archive->read(section, "unterminated-string", string_value),
		             "read unterminated registry string");
		assert_equal("raw"s, string_value, "unterminated registry string value");

		uint8_t binary[16] = {};
		auto binary_size = std::size(binary);
		assert_equal(false, archive->read(section, "wrong-binary-type", binary, binary_size),
		             "reject binary type");
	}

}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Premiere duplicate-import investigation
// The drag/clipboard IDataObject advertises BOTH CF_HDROP and CFSTR_SHELLIDLIST. Per the Windows
// shell contract a conformant drop target enumerates the offered formats and consumes the FIRST
// one it supports (one format => one copy of each item). This test proves that each file-bearing
// format INDEPENDENTLY resolves to the cached items exactly once, so a well-behaved consumer
// imports a clip once, whereas a consumer that greedily reads multiple formats (the suspected
// Premiere behaviour) would import the same clip twice.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_offer_each_drag_format_once()
{
	const auto file1 = test_files_folder.combine_file("Test.jpg");
	const auto file2 = test_files_folder.combine_file("Small.jpg");

	assert_equal(true, file1.exists(), "Test.jpg exists");
	assert_equal(true, file2.exists(), "small.jpg exists");

	const std::vector<df::file_path> files{file1, file2};
	const std::vector<df::folder_path> folders;

	const auto probe = platform::probe_drag_data_object(files, folders);

	// The source advertises BOTH file-bearing formats. This is spec-compliant source
	// behaviour (Explorer does exactly the same), not a bug in itself.
	assert_equal(true, probe.advertises_hdrop, "advertises CF_HDROP");
	assert_equal(true, probe.advertises_shell_id_list, "advertises CFSTR_SHELLIDLIST");

	// CF_HDROP is enumerated before CFSTR_SHELLIDLIST. A conformant target picks the
	// first format it understands - so it consumes CF_HDROP and stops.
	assert_equal(true, probe.hdrop_enum_index >= 0, "CF_HDROP is enumerated");
	assert_equal(true, probe.shell_id_list_enum_index >= 0, "CFSTR_SHELLIDLIST is enumerated");
	assert_equal(true, probe.hdrop_enum_index < probe.shell_id_list_enum_index,
	             "CF_HDROP enumerated before CFSTR_SHELLIDLIST");

	// Each file-bearing format INDEPENDENTLY resolves to exactly the 2 input items.
	// Neither format duplicates a clip on its own - both are well-formed.
	assert_equal(2, probe.hdrop_count, "CF_HDROP yields 2 files");
	assert_equal(2, probe.shell_id_list_count, "CFSTR_SHELLIDLIST yields 2 items");
	assert_equal(2, static_cast<int>(probe.hdrop_paths.size()), "CF_HDROP resolved path count");
	assert_equal(2, static_cast<int>(probe.shell_id_list_paths.size()), "CFSTR_SHELLIDLIST resolved path count");

	const auto contains_name = [](const std::vector<std::wstring>& paths, const wchar_t* name)
	{
		return std::ranges::any_of(paths, [name](const std::wstring& p)
		{
			auto lower = p;
			std::ranges::transform(lower, lower.begin(), towlower);
			return lower.find(name) != std::wstring::npos;
		});
	};

	// Both formats resolve to the SAME two files.
	assert_equal(true, contains_name(probe.hdrop_paths, L"test.jpg"), "CF_HDROP resolves Test.jpg");
	assert_equal(true, contains_name(probe.hdrop_paths, L"small.jpg"), "CF_HDROP resolves small.jpg");
	assert_equal(true, contains_name(probe.shell_id_list_paths, L"test.jpg"), "CFSTR_SHELLIDLIST resolves Test.jpg");
	assert_equal(true, contains_name(probe.shell_id_list_paths, L"small.jpg"), "CFSTR_SHELLIDLIST resolves small.jpg");

	// The duplicate mechanism: a conformant consumer reads ONE format => 2 imports.
	// A consumer that greedily harvests BOTH file formats sees each clip twice => 4 imports.
	assert_equal(2, probe.hdrop_count, "conformant consumer (one format) imports each clip once");
	assert_equal(4, probe.hdrop_count + probe.shell_id_list_count,
	             "greedy consumer (both file formats) would import each clip twice");
}

static int WINAPI shell_success_w(SHFILEOPSTRUCTW*)
{
	return 0;
}

static int WINAPI shell_aborts_w(SHFILEOPSTRUCTW* op)
{
	op->fAnyOperationsAborted = TRUE;
	return 0;
}

static int WINAPI shell_fails_w(SHFILEOPSTRUCTW*)
{
	return ERROR_ACCESS_DENIED;
}

static int WINAPI shell_aborts_a(SHFILEOPSTRUCTA* op)
{
	op->fAnyOperationsAborted = TRUE;
	return 0;
}

static void should_double_terminate_shell_destination_buffers()
{
	const auto target = df::folder_path("C:\\photos");
	const auto wide_destination = platform::probe_shell_destination_w(target);
	const auto narrow_destination = platform::probe_shell_destination_a(target);

	assert_equal(L'\0', wide_destination[wide_destination.size() - 1], "wide destination final terminator");
	assert_equal(L'\0', wide_destination[wide_destination.size() - 2], "wide destination penultimate terminator");
	assert_equal('\0', narrow_destination[narrow_destination.size() - 1], "narrow destination final terminator");
	assert_equal('\0', narrow_destination[narrow_destination.size() - 2], "narrow destination penultimate terminator");
}

static void should_classify_an_aborted_shell_operation_as_cancelled()
{
	assert_equal(static_cast<int>(platform::file_op_result_code::CANCELLED),
	             static_cast<int>(platform::probe_shell_file_operation_w(shell_aborts_w).result.code),
	             "wide shell abort is cancellation");
	assert_equal(static_cast<int>(platform::file_op_result_code::CANCELLED),
	             static_cast<int>(platform::probe_shell_file_operation_a(shell_aborts_a).result.code),
	             "narrow shell abort is cancellation");
	assert_equal(static_cast<int>(platform::file_op_result_code::OK),
	             static_cast<int>(platform::probe_shell_delete_operation(shell_success_w, true).result.code),
	             "zero and no abort is success");
	assert_equal(static_cast<int>(platform::file_op_result_code::FAILED),
	             static_cast<int>(platform::probe_shell_delete_operation(shell_fails_w, true).result.code),
	             "nonzero shell result is failure");
}

static void should_let_windows_confirm_recycle_that_would_destroy()
{
	const auto recycle_flags = platform::probe_shell_delete_flags(true);
	assert_equal(true, (recycle_flags & FOF_ALLOWUNDO) != 0, "requested recycle allows undo");
	assert_equal(true, (recycle_flags & FOF_WANTNUKEWARNING) != 0,
	             "requested recycle keeps Windows permanent-delete warning");
	assert_equal(true, (recycle_flags & FOF_SILENT) != 0, "requested recycle suppresses progress UI");
	assert_equal(false, (recycle_flags & FOF_NOCONFIRMATION) != 0,
	             "requested recycle lets Windows ask before permanent fallback");

	const auto permanent_flags = platform::probe_shell_delete_flags(false);
	assert_equal(true, (permanent_flags & FOF_NOCONFIRMATION) != 0,
	             "explicit permanent delete suppresses duplicate shell confirmation");
	assert_equal(false, (permanent_flags & FOF_ALLOWUNDO) != 0, "explicit permanent delete does not allow undo");
	assert_equal(false, (permanent_flags & FOF_WANTNUKEWARNING) != 0,
	             "explicit permanent delete was already confirmed by the application");
}

static void should_materialize_advertised_clipboard_images()
{
	files ff;
	const auto loaded = ff.load(test_files_folder.combine_file("Test.jpg"), false);
	assert_equal(true, loaded.success, "test image loads");

	const auto probe = platform::probe_drag_data_object(clipboard_image_of(loaded));

	assert_equal(true, probe.advertises_bitmap, "captured image advertises CF_BITMAP");
	assert_equal(true, probe.advertises_dib, "captured image advertises CF_DIB");
	assert_equal(true, probe.bitmap_retrieved, "advertised bitmap retrieves");
	assert_equal(true, probe.dib_retrieved, "advertised DIB retrieves");
	assert_equal(loaded.dimensions().cx, probe.bitmap_dimensions.cx, "bitmap width");
	assert_equal(loaded.dimensions().cy, probe.bitmap_dimensions.cy, "bitmap height");
	assert_equal(loaded.dimensions().cx, probe.dib_dimensions.cx, "DIB width");
	assert_equal(loaded.dimensions().cy, probe.dib_dimensions.cy, "DIB height");
	assert_equal(2, static_cast<int>(probe.image_formats.size()), "two image formats advertised");
	assert_equal(static_cast<uint32_t>(CF_BITMAP), probe.image_formats[0], "bitmap is first image offer");
	assert_equal(static_cast<uint32_t>(TYMED_GDI), probe.image_tymed[0], "bitmap uses GDI medium");
	assert_equal(static_cast<uint32_t>(CF_DIB), probe.image_formats[1], "DIB is second image offer");
	assert_equal(static_cast<uint32_t>(TYMED_HGLOBAL), probe.image_tymed[1], "DIB uses global memory");
}

static void should_preserve_clipboard_format_contracts()
{
	const auto result = platform::probe_clipboard_contracts();
	assert_equal(true, result.clone_keeps_position, "clones resume at the original enumeration cursor");
	assert_equal(true, result.skip_to_end, "an exact skip succeeds and leaves the cursor at the end");
	assert_equal(true, result.skip_past_end, "an oversized skip reports partial completion and advances to the end");
	assert_equal(true, result.preferred_effect, "the advertised Cut effect passes capability negotiation");
	assert_equal(true, result.rejects_wrong_medium, "query and retrieval reject incompatible media consistently");
}

static void should_pass_bare_save_dialog_extensions()
{
	assert_equal(L"png", platform::dialog_default_extension(".png"), "the leading dot is not passed to the dialog");
	assert_equal(L"webp", platform::dialog_default_extension("webp"), "an already bare extension is unchanged");
	assert_equal(L"", platform::dialog_default_extension(""), "an absent extension remains absent");
}

static void should_publish_pasted_bitmap_after_closing_stage()
{
	const auto folder = _temps.folder().combine(std::format("pasted-bitmap-{}", platform::tick_count()));
	assert_equal(true, platform::create_folder(folder).success(), "create bitmap paste temp folder");

	const uint32_t pixels[] = {
		0xffff0000, 0xff00ff00,
		0xff0000ff, 0xffffffff
	};
	const auto bitmap = CreateBitmap(2, 2, 1, 32, pixels);
	assert_equal(true, bitmap != nullptr, "create test bitmap");
	const df::scope_exit delete_bitmap([bitmap] { DeleteObject(bitmap); });

	const auto result = save_bitmap_info(folder, "pasted", true, bitmap);

	assert_equal(true, result.success(), "bitmap save succeeds");
	assert_equal(1, static_cast<int>(result.created_files.files.size()), "one published file");
	const auto final_path = result.created_files.files.front();
	assert_equal(true, final_path.exists(), "published file exists");

	const auto attributes = GetFileAttributesW(platform::to_file_system_path(final_path).c_str());
	assert_equal(false, attributes == INVALID_FILE_ATTRIBUTES, "published file attributes read");
	assert_equal(false, (attributes & FILE_ATTRIBUTE_TEMPORARY) != 0, "published file is not temporary");

	auto stage_count = 0;
	for (const auto& entry : std::filesystem::directory_iterator(platform::to_file_system_path(folder)))
	{
		const auto name = entry.path().filename().wstring();
		if (name.starts_with(L"diffractor_")) ++stage_count;
	}
	assert_equal(0, stage_count, "owned stage file was removed or published");
}

// Native common controls are double buffered (buffered_control_paint) so a resize or splitter drag
// never composites a control that has been erased but not yet drawn. That only works if the control
// renders itself into the device context it is handed; a control that ignored the request would
// blit an empty buffer and appear blank. This test holds comctl32 to that contract for the two
// classes the app buffers.
static void should_reject_unusable_file_names()
{
	assert_equal(true, platform::is_valid_file_name("holiday 2024.jpg"), "an ordinary name is usable");
	assert_equal(true, platform::is_valid_file_name("caf\xc3\xa9 \xe6\x97\xa5.jpg"), "non-ascii is usable");
	assert_equal(true, platform::is_valid_file_name("console.txt"), "a reserved name as a prefix is usable");

	assert_equal(false, platform::is_valid_file_name(""), "an empty name is not usable");
	assert_equal(false, platform::is_valid_file_name("a?b.jpg"), "a reserved character is not usable");
	assert_equal(false, platform::is_valid_file_name("a\tb.jpg"), "a control character is not usable");
	assert_equal(false, platform::is_valid_file_name("trailing."), "a trailing dot is not usable");
	assert_equal(false, platform::is_valid_file_name("trailing "), "a trailing space is not usable");
	assert_equal(false, platform::is_valid_file_name("CON"), "a device name is not usable");
	assert_equal(false, platform::is_valid_file_name("con.txt"), "a device name with an extension is not usable");
	assert_equal(false, platform::is_valid_file_name("Lpt9.jpeg"), "device names are case insensitive");
}

static void should_render_common_controls_into_a_buffer()
{
	const auto probe = platform::probe_buffered_control_paint();

	assert_equal(true, probe.trackbar_painted_pixels > 0, "trackbar draws into a supplied dc");
	assert_equal(true, probe.trackbar_colors > 1, "trackbar draws more than a flat fill");
	assert_equal(true, probe.toolbar_painted_pixels > 0, "toolbar draws into a supplied dc");
	assert_equal(true, probe.toolbar_colors > 1, "toolbar draws more than a flat fill");
	assert_equal(true, probe.button_painted_pixels > 0, "button draws into a supplied dc");
	assert_equal(true, probe.button_colors > 1, "button draws more than a flat fill");
}

static void should_replace_the_full_trailing_spelling_word()
{
	const auto invalid = [](const std::string_view word)
	{
		return word != "teh" && word != "x";
	};

	const auto replace_first = [](std::wstring text, const std::vector<platform::edit_spelling_range>& ranges)
	{
		assert_equal(1_z, ranges.size(), "one misspelling is found");
		text.replace(static_cast<size_t>(ranges.front().pos_start),
		             static_cast<size_t>(ranges.front().pos_end - ranges.front().pos_start), L"the");
		return text;
	};

	assert_equal(L"the"s, replace_first(L"teh", platform::probe_edit_spelling_ranges(L"teh", invalid)),
	             "a final word includes its last character");
	assert_equal(L"prefix the"s, replace_first(L"prefix teh", platform::probe_edit_spelling_ranges(L"prefix teh", invalid)),
	             "surrounding prefix text is untouched");
	assert_equal(L"the "s, replace_first(L"teh ", platform::probe_edit_spelling_ranges(L"teh ", invalid)),
	             "a word followed by whitespace keeps the separator");

	const auto one = platform::probe_edit_spelling_ranges(L"x", invalid);
	assert_equal(1_z, one.size(), "a one-character final word is found");
	assert_equal(0, one.front().pos_start, "one-character start");
	assert_equal(1, one.front().pos_end, "one-character end remains exclusive");
}

// The control checks words against the dictionary it is given and knows of no other: the application
// supplies spell checking, and without it nothing is underlined.
static void should_check_edit_words_against_the_supplied_dictionary()
{
	class listed_dictionary final : public ui::spelling_dictionary
	{
	public:
		mutable std::vector<std::string> asked;

		void queue_load() override {}

		bool is_word_valid(const std::string_view word) const override
		{
			asked.emplace_back(word);
			return word != "teh";
		}

		std::vector<std::string> suggest(std::string_view) const override { return {}; }
		void add_word(std::string_view, std::function<void()> done) override { if (done) done(); }
	};

	listed_dictionary dictionary;
	const auto unknown = platform::probe_edit_unknown_words(L"the teh cat", &dictionary);

	assert_equal(1_z, unknown.size(), "the word the dictionary rejects is underlined");
	assert_equal("teh"s, unknown.front().word, "which word");
	assert_equal(4, unknown.front().pos_start, "where it starts");
	assert_equal(7, unknown.front().pos_end, "where it ends");
	assert_equal(3_z, dictionary.asked.size(), "every word is asked of the dictionary");
	assert_equal(true, platform::probe_edit_unknown_words(L"the teh cat", nullptr).empty(),
	             "without a dictionary nothing is underlined");
}

// The chrome's embedded PNGs decode with the application's decoder, which it installs before any
// backend loads one, so both backends draw the same shadow with or without WIC.
static void should_decode_embedded_chrome_images()
{
	assert_equal(true, static_cast<bool>(ui::decode_embedded_image), "the application installed its decoder");

	const auto shadow = ui::decode_embedded(load_resource(IDB_SHADOW, L"PNG"));
	assert_equal(true, shadow != nullptr, "the shadow decodes");
	assert_equal(true, shadow && !shadow->dimensions().is_empty(), "to an image with pixels");
}

static void should_hit_test_the_last_trailing_spelling_character()
{
	const auto point_x = [](const DWORD p) { return static_cast<int>(static_cast<short>(LOWORD(p))); };
	const auto point_y = [](const DWORD p) { return static_cast<int>(static_cast<short>(HIWORD(p))); };

	const auto parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 240, 60, nullptr, nullptr,
	                                   get_resource_instance, nullptr);
	assert_equal(true, parent != nullptr, "parent window created");
	const df::scope_exit destroy_parent([parent] { if (parent) DestroyWindow(parent); });

	const auto edit = CreateWindowExW(0, L"EDIT", L"prefix teh", WS_CHILD | WS_VISIBLE, 0, 0, 220, 30, parent,
	                                 nullptr, get_resource_instance, nullptr);
	assert_equal(true, edit != nullptr, "edit control created");
	const df::scope_exit destroy_edit([edit] { if (edit) DestroyWindow(edit); });

	const auto last_char = static_cast<DWORD>(SendMessageW(edit, EM_POSFROMCHAR, 9, 0));
	const auto end_pos = static_cast<DWORD>(SendMessageW(edit, EM_POSFROMCHAR, 10, 0));
	assert_equal(true, point_x(last_char) >= 0, "last character has a native edit position");
	assert_equal(-1, point_x(end_pos), "exclusive end is past the last native edit character");

	const auto bounds = platform::probe_edit_spelling_bounds(edit, 7, 10);
	assert_equal(true, !bounds.is_empty(), "the spelling bounds include a trailing word whose end is exclusive");
	assert_equal(true, bounds.contains({point_x(last_char) + 1, point_y(last_char) + 1}),
	             "the spelling hit-test reaches the final character");
}

static void should_delegate_ordinary_edit_context_menus()
{
	using route = platform::edit_context_menu_route;

	assert_equal(static_cast<int>(route::native_edit_procedure),
	             static_cast<int>(platform::probe_edit_context_menu_route(false, false)),
	             "spelling-disabled or correctly-spelled text is delegated");
	assert_equal(static_cast<int>(route::native_edit_procedure),
	             static_cast<int>(platform::probe_edit_context_menu_route(true, false)),
	             "a failed custom menu creation is delegated");
	assert_equal(static_cast<int>(route::custom_spelling_menu),
	             static_cast<int>(platform::probe_edit_context_menu_route(true, true)),
	             "a real misspelling shows exactly the custom spelling menu");
}

static void should_offer_the_maximize_caption_button_only_on_a_resizable_frame()
{
	constexpr uint32_t frame = WS_OVERLAPPEDWINDOW;
	constexpr uint32_t maximized = frame | WS_MAXIMIZE;
	constexpr uint32_t full_screen = frame & ~(WS_CAPTION | WS_THICKFRAME);
	constexpr uint32_t dialog = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;

	assert_equal(true, platform::presents_maximize_caption_button(df::os_release::windows_11, frame),
	             "a resizable Windows 11 frame offers snap layouts over its maximize button");
	assert_equal(true, platform::presents_maximize_caption_button(df::os_release::windows_later, frame),
	             "a later release keeps them");
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::windows_10, frame),
	             "Windows 10 has no snap layouts, so the button stays a toolbar button");
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::unknown, frame),
	             "a release that could not be read is not assumed to have them");
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::windows_11, full_screen),
	             "Fullscreen strips the resizing frame");
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::windows_11, dialog),
	             "a dialog has no maximize box");

	// The flyout would open over the restore button and take the click meant for it.
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::windows_11, maximized),
	             "a maximized frame's restore button stays a toolbar button");
	assert_equal(true, platform::can_present_maximize_caption_button(df::os_release::windows_11, maximized),
	             "a maximized frame still keeps the top of its frame clear for when it is restored");
	assert_equal(false, platform::can_present_maximize_caption_button(df::os_release::windows_10, frame),
	             "Windows 10 frames keep their frame as it was");
}

static void should_invoke_the_maximize_caption_button_only_on_a_release_over_its_press()
{
	platform::caption_button_tracker t;

	assert_equal(true, t.hover(7), "resting on the button lights it");
	assert_equal(false, t.hover(7), "moving within it changes nothing");
	assert_equal(0, t.release(7), "a release without a press invokes nothing");

	assert_equal(true, t.press(7), "pressing shows it pressed");
	assert_equal(7, t.release(7), "a release over the pressed button invokes it");
	assert_equal(0, t.pressed_id, "the press ends with the release");
	assert_equal(0, t.hover_id, "invoking forgets the hover, because the frame moves under the pointer");

	t.press(7);
	assert_equal(true, t.leave(), "leaving puts the light out");
	assert_equal(0, t.release(7), "a press abandoned by leaving invokes nothing");

	t.press(7);
	assert_equal(true, t.hover(0), "resting off the button redraws it");
	assert_equal(0, t.release(7), "a press abandoned by resting elsewhere invokes nothing");

	t.press(7);
	assert_equal(0, t.release(8), "a release over another button invokes nothing");
}

static void should_answer_hit_tests_on_the_maximize_toolbar_button_as_the_caption_button()
{
	const auto probe = platform::probe_toolbar_caption_button();

	assert_equal(HTCLIENT, probe.other_hit, "an ordinary toolbar button stays a toolbar button");
	assert_equal(HTCLIENT, probe.maximized_hit, "a maximized frame's restore button stays a toolbar button");
	assert_equal(HTCLIENT, probe.frameless_hit, "a frame without its resizing border presents no caption button");

	if (probe.offered)
	{
		assert_equal(HTMAXBUTTON, probe.maximize_hit, "the maximize button answers as the window's caption button");
		assert_equal(probe.maximize_id, probe.hovered_id, "resting on it lights it");
		assert_equal(1, probe.invocations, "a press and release over it invokes the command once");
		assert_equal(0, probe.hover_after_leave, "leaving it puts the light out");
	}
	else
	{
		assert_equal(HTCLIENT, probe.maximize_hit, "without snap layouts it stays a toolbar button");
		assert_equal(0, probe.invocations, "a hit test invokes nothing");
	}
}

static void should_read_coherent_number_format_snapshots()
{
	platform::set_number_format_probe_snapshots({
		                                            {L".", L",", 1, 1},
		                                            {L",", L".", 1, 1},
	                                            },
	                                            true);
	const df::scope_exit restore([] { platform::clear_number_format_probe_snapshots(); });

	assert_equal("1,234", platform::format_number("1234"), "the first format uses one complete snapshot");
	assert_equal(",", platform::number_dec_sep(), "invalidation during refresh remains pending for the next read");
	assert_equal("1.234", platform::format_number("1234"), "the second format uses the next complete snapshot");
	assert_equal(",", platform::number_dec_sep(), "the separator belongs to the same refreshed snapshot");
}

static void should_bound_the_software_buffer_as_the_client_grows()
{
	const auto probe = platform::probe_software_tiling();

	// Without this the assertion below would still pass if the buffer went back to tracking the
	// window, which is the thing tiling exists to stop.
	assert_equal(true, probe.grown_buffer_pixels < probe.grown_client_pixels / 4,
	             "the buffer stayed bounded while the client grew");
	assert_equal(probe.grown_client_pixels, probe.grown_writable_pixels,
	             "a grown client is writable to its new edges without reallocating the tile");
}

static void should_translate_highlight_spans_to_utf16_clusters()
{
	const auto highlight = ui::color(1.0f, 0.0f, 0.0f, 1.0f);
	const auto plain = ui::color(0.0f, 0.0f, 1.0f, 1.0f);
	const std::string text = "\xf0\x9f\x98\x80" "caf\xc3\xa9" "X";
	const auto converted = ui::utf8_to_utf16(text, {{4, 5, highlight}});

	assert_equal(7_z, converted.text.size(), "emoji is preserved as a surrogate pair");
	assert_equal(0xd83d, static_cast<int>(converted.text[0]), "lead surrogate");
	assert_equal(0xde00, static_cast<int>(converted.text[1]), "trail surrogate");
	assert_equal(1_z, converted.highlights.size(), "one highlight survives conversion");
	assert_equal(2u, converted.highlights[0].offset, "highlight begins after the surrogate pair");
	assert_equal(4u, converted.highlights[0].length, "highlight length is UTF-16 text, not UTF-8 bytes");

	const uint16_t clusters[] = {0, 0, 1};
	const auto first = ui::text_cluster_span_for_glyph(0, 2, clusters, 3, 10);
	const auto second = ui::text_cluster_span_for_glyph(1, 2, clusters, 3, 10);
	assert_equal(10u, first.offset, "cluster offset includes run text position");
	assert_equal(2u, first.length, "one glyph cluster can span multiple text units");
	assert_equal(12u, second.offset, "the next glyph starts after the multi-unit cluster");
	assert_equal(highlight.r, ui::text_cluster_color(plain, first, {{10, 2, highlight}}).r,
	             "a complete cluster is highlighted");
	assert_equal(highlight.r, ui::text_cluster_color(plain, first, {{10, 1, highlight}}).r,
	             "a base letter highlight colours its combining-mark cluster");
}

static void should_build_text_cluster_spans_once_per_run()
{
	const uint16_t clusters[] = {0, 0, 1, 2, 2};
	const auto spans = ui::text_cluster_spans_for_glyph_run(3, clusters, 5, 4);

	assert_equal(3_z, spans.size(), "one span per glyph");
	assert_equal(4u, spans[0].offset, "first cluster starts at run text position");
	assert_equal(2u, spans[0].length, "first cluster spans two text units");
	assert_equal(6u, spans[1].offset, "second cluster follows");
	assert_equal(1u, spans[1].length, "second cluster is one text unit");
	assert_equal(7u, spans[2].offset, "third cluster follows");
	assert_equal(2u, spans[2].length, "third cluster spans two text units");
}

static void should_apply_positive_ascender_offsets_upward()
{
	assert_equal(80.0f, ui::glyph_top_from_baseline(100.0f, 20, 0.0f), "no ascender offset");
	assert_equal(77.0f, ui::glyph_top_from_baseline(100.0f, 20, 3.0f), "positive ascender moves up");
	assert_equal(83.0f, ui::glyph_top_from_baseline(100.0f, 20, -3.0f), "negative ascender moves down");
}

static void should_clear_text_highlights_between_draws()
{
	std::vector<ui::text_highlight_t> highlights = {{0, 3, ui::color(1.0f, 0.0f, 0.0f, 1.0f)}};
	ui::clear_text_highlights(highlights);
	assert_equal(true, highlights.empty(), "plain/layout draws start without stale highlight intervals");
}

static void should_gate_native_bubble_alpha_steps()
{
	const auto restore_animations = ui::animations_enabled;
	const df::scope_exit restore_scope([restore_animations] { ui::animations_enabled = restore_animations; });

	ui::animations_enabled = true;
	assert_equal(72, ui::fade_alpha_step(0, 255), "enabled fade advances toward visible");
	assert_equal(0, ui::fade_alpha_step(1, 0), "enabled fade snaps the terminal hidden alpha");

	ui::animations_enabled = false;
	auto alpha = 0;
	alpha = ui::gated_fade_alpha_step(alpha, 255);
	assert_equal(255, alpha, "disabled show snaps visible");
	alpha = ui::gated_fade_alpha_step(alpha, 0);
	assert_equal(0, alpha, "disabled hide snaps hidden");
}

static void should_handle_unavailable_font_layout()
{
	text_layout_impl layout(nullptr, ui::style::font_face::dialog);

	layout.update("text", ui::style::text_style::single_line);
	const auto measured = layout.measure_text(100, 100);
	assert_equal(0, measured.cx, "unavailable font has no measured width");
	assert_equal(0, measured.cy, "unavailable font has no measured height");

	const auto offsets = layout.offset_xs({0, 2, 4}, 100, 100);
	assert_equal(3_z, offsets.size(), "unavailable font still answers each requested offset");
	assert_equal(0, offsets[0], "first unavailable offset");
	assert_equal(0, offsets[1], "middle unavailable offset");
	assert_equal(0, offsets[2], "last unavailable offset");
	assert_equal(true, should_retry_font_renderer(nullptr), "GPU renderer retries a transient null font");
	font_renderer::draw(nullptr, nullptr, layout._layout.Get(), {0, 0, 100, 100}, {}, {});
	assert_equal(true, layout._layout == nullptr, "drawing an unavailable layout leaves it unavailable");
}

static void should_snap_gpu_text_in_pixel_coordinates()
{
	assert_equal(true, platform::probe_d3d11_pixel_snapping(),
	             "text callbacks report enabled snapping, identity transform and one pixel per DIP");
}

static void should_refuse_an_impossible_movie()
{
	// The probe is the whole of whether Render is offered, so an unstable answer would make the
	// command appear and disappear between toolbar rebuilds.
	const auto answer = platform::can_write_movies();
	assert_equal(answer, platform::can_write_movies(), "the encoder probe answers the same way twice");

	platform::movie_writer_request request;
	request.path = _temps.next_path(".mp4");
	request.extent = {0, 0};
	request.frame_rate = 30;
	request.video_bitrate = 6000000;

	// A writer that accepted this would leave a zero-byte file where the movie should be, and the
	// render would report success.
	assert_equal(true, platform::create_movie_writer(request) == nullptr, "a zero-sized movie is refused");
	assert_equal(false, platform::exists(request.path), "and no partial file is left behind");

	request.extent = {1920, 1080};
	request.frame_rate = 0;
	assert_equal(true, platform::create_movie_writer(request) == nullptr, "a movie with no frame rate is refused");
	assert_equal(false, platform::exists(request.path), "and still leaves nothing behind");
}

void register_platform_tests(view_state& state, test_registry& tests)
{
	tests.add("Should preserve writability probe collisions"s, should_preserve_writability_probe_collisions);
	tests.add("Should convert dates with their own DST offset"s, should_convert_dates_with_their_own_dst_offset);
	tests.add("Should tabulate time zone offsets like the system"s, should_tabulate_time_zone_offsets_like_the_system);
	tests.add("Should convert the current zone like the system"s, should_convert_the_current_zone_like_the_system);
	tests.add("Should tabulate local time once per year"s, should_tabulate_local_time_once_per_year);
	tests.add("Should initialize Unicode INI settings"s, should_initialize_unicode_ini_settings);
	tests.add("Should use screen coordinates for saved window placement"s, should_use_screen_coordinates_for_saved_window_placement);
	tests.add("Should not treat pointer wheel IDs as modifiers"s, should_not_treat_pointer_wheel_ids_as_modifiers);
	tests.add("Should snap GPU text in pixel coordinates"s, should_snap_gpu_text_in_pixel_coordinates);
	tests.add("Should preserve clipboard format contracts"s, should_preserve_clipboard_format_contracts);
	tests.add("Should pass bare save dialog extensions"s, should_pass_bare_save_dialog_extensions);
	tests.add("Should convert extended file system paths"s, should_convert_extended_file_system_paths);
	tests.add("Should recognise network locations"s, should_recognise_network_locations);
	tests.add("Should convert utf8 to ansi"s, should_convert_utf8_to_ansi);
	tests.add("Should restore a window onto a display that still exists"s, should_restore_a_window_onto_a_display);
	tests.add("Should classify DXGI device loss"s, should_classify_dxgi_device_loss);
	tests.add("Should recognise the Microsoft Basic Render Driver as software"s,
	          should_recognise_the_basic_render_driver);
	tests.add("Should need shader sampling for planar video textures"s, should_need_sampling_for_planar_video);
	tests.add("Should count shared memory for an integrated GPU"s, should_count_shared_memory_for_an_integrated_gpu);
	tests.add("Should create the decode device on the render adapter"s,
	          should_create_the_decode_device_on_the_render_adapter);
	tests.add("Should lend the renderer the decode device's lock"s, should_lend_the_renderer_the_decode_device_lock);
	tests.add("Should describe a WASAPI mix format"s, should_describe_a_wasapi_mix_format);
	tests.add("Should recycle a texture only when nothing draws it"s, should_recycle_a_texture_only_when_nothing_draws_it);
	tests.add("Should overwrite a recycled texture with the new picture"s,
	          should_overwrite_a_recycled_texture_with_the_new_picture);
	tests.add("Should present a video frame through its interface"s, should_present_a_video_frame_through_its_interface);
	tests.add("Should present a video frame in software through its interface"s,
	          should_present_a_video_frame_in_software_through_its_interface);
	tests.add("Should suppress GPU for one recovery session"s, should_suppress_gpu_for_recovery_session);
	tests.add("Issue #219: Should fall back for missing glyphs"s, should_fall_back_for_missing_glyphs);
	tests.add("Issue #232/#189: Should cache font faces per face and size"s,
	          should_cache_font_faces_per_face_and_size);
	tests.add("Issue #189: Should key glyph cache by size"s, should_key_glyph_cache_by_size);
	tests.add("Should persist strings in registry"s, should_persist_to_registry);
	tests.add("Should validate registry value types and sizes"s, should_validate_registry_value_types_and_sizes);
	tests.add("Premiere dup: drag offers each format once"s, should_offer_each_drag_format_once);
	// PLAT-002 - shell destination double-NUL.
	tests.add("Should double-terminate shell destination buffers"s,
	          should_double_terminate_shell_destination_buffers);
	// PLAT-003 - shell aborted flag sequencing.
	tests.add("Should classify an aborted shell operation as cancelled"s,
	          should_classify_an_aborted_shell_operation_as_cancelled);
	// PLAT-001 - Windows owns permanent-delete confirmation for recycle fallback.
	tests.add("Should let Windows confirm recycle that would destroy"s,
	          should_let_windows_confirm_recycle_that_would_destroy);
	// PLAT-004 - advertised clipboard image formats are materializable.
	tests.add("Should materialize advertised clipboard images"s, should_materialize_advertised_clipboard_images);
	// PLAT-006 - pasted bitmap stages close before publication and cleanup.
	tests.add("Should publish pasted bitmap after closing stage"s, should_publish_pasted_bitmap_after_closing_stage);
	tests.add("Should reject unusable file names"s, should_reject_unusable_file_names);
	tests.add("Should render common controls into a buffer"s, should_render_common_controls_into_a_buffer);
	// PLAT-011 - trailing spelling correction range.
	tests.add("Should replace the full trailing spelling word"s, should_replace_the_full_trailing_spelling_word);
	tests.add("Should check edit words against the supplied dictionary"s,
	          should_check_edit_words_against_the_supplied_dictionary);
	tests.add("Should decode embedded chrome images"s, should_decode_embedded_chrome_images);
	tests.add("Should hit test the last trailing spelling character"s,
	          should_hit_test_the_last_trailing_spelling_character);
	// PLAT-012 - native edit context-menu routing.
	tests.add("Should delegate ordinary common controls edit context menus"s,
	          should_delegate_ordinary_edit_context_menus);
	// Windows 11 snap layouts appear over the button a window reports as its maximize caption button.
	tests.add("Should offer the maximize caption button only on a resizable frame"s,
	          should_offer_the_maximize_caption_button_only_on_a_resizable_frame);
	tests.add("Should invoke the maximize caption button only on a release over its press"s,
	          should_invoke_the_maximize_caption_button_only_on_a_release_over_its_press);
	tests.add("Should answer hit tests on the maximize toolbar button as the caption button"s,
	          should_answer_hit_tests_on_the_maximize_toolbar_button_as_the_caption_button);
	// PLAT-018 - coherent locale-format snapshots.
	tests.add("Should read coherent number format snapshots"s, should_read_coherent_number_format_snapshots);
	tests.add("Should bound the software buffer as the client grows"s,
	          should_bound_the_software_buffer_as_the_client_grows);
	// UI-006 - Unicode highlights must be translated to the UTF-16 clusters DirectWrite draws.
	tests.add("Should translate highlight spans to utf16 clusters"s, should_translate_highlight_spans_to_utf16_clusters);
	// UI-006 - cluster spans are computed once for the run instead of scanning for each glyph.
	tests.add("Should build text cluster spans once per run"s, should_build_text_cluster_spans_once_per_run);
	// PLAT-015 - DirectWrite positive ascender offsets are upward in screen coordinates.
	tests.add("Should apply positive ascender offsets upward"s, should_apply_positive_ascender_offsets_upward);
	// PLAT-014 - plain and layout draws must drop transient highlight state from previous draws.
	tests.add("Should clear text highlights between draws"s, should_clear_text_highlights_between_draws);
	// PLAT-017 - Native bubble fades must honor the shared animation gate.
	tests.add("Should gate native bubble alpha steps"s, should_gate_native_bubble_alpha_steps);
	// PLAT-016 - a missing font renderer is a degraded text outcome, not a null dereference.
	tests.add("Should handle unavailable font layout"s, should_handle_unavailable_font_layout);
	tests.add("Should refuse an impossible movie"s, should_refuse_an_impossible_movie);
}
