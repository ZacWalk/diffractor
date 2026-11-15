// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Debug build's screen recorder on Windows. A thread of its own copies the window's client area as
// the desktop shows it - menus, bubbles and the pointer included - by DXGI desktop duplication, or by GDI where
// the system offers no duplication, at a steady rate, and writes it through libdesktop's movie writer. A script's
// pointer, gliding and clicking, is drawn into the frames, and the moments it names are written beside the movie.

#include "pch.h"

#ifdef _DEBUG

#include "libdesktop/platform/win32.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <wtsapi32.h>

#include "app_record.h"

namespace
{
	// Bits per pixel per frame. A recording is a first generation that Movie mixer encodes again at 0.10, so it
	// is kept well above that: interface text and thumbnail edges are what a second generation softens first.
	constexpr double recording_bits_per_pixel = 0.25;
	constexpr double recording_max_bitrate = 60000000.0;

	// Duplication lost this many times in a row - a remote session, a desktop mode it does not take - gives way
	// to GDI, which is an order slower but always there.
	constexpr int duplication_attempts = 30;

	// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, which the SDK declares only for Windows 10 targets.
	constexpr DWORD create_waitable_timer_high_resolution = 0x00000002;

	// DWMWA_WINDOW_CORNER_PREFERENCE and DWMWA_BORDER_COLOR with their values, which the SDK declares only for
	// Windows 11 targets; an older system ignores them.
	constexpr DWORD dwm_corner_preference = 33;
	constexpr DWORD dwm_border_color = 34;
	constexpr DWORD dwm_corners_default = 0;
	constexpr DWORD dwm_corners_square = 1;
	constexpr COLORREF dwm_color_default = 0xFFFFFFFF;
	constexpr COLORREF dwm_color_none = 0xFFFFFFFE;

	// A window presented for a recording: above every other, with square corners and no border, and back as it was
	// afterwards. On the window's thread, both ways: from the recording's, a change would wait on a window whose
	// thread may itself be waiting for the recording.
	class presented_window
	{
	public:
		explicit presented_window(const HWND window) : _window(window)
		{
			if (!_window || !IsWindow(_window)) return;

			if (!(GetWindowLongPtrW(_window, GWL_EXSTYLE) & WS_EX_TOPMOST))
			{
				_made_topmost = SetWindowPos(_window, HWND_TOPMOST, 0, 0, 0, 0,
				                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
			}

			const auto corners = dwm_corners_square;
			const auto border = dwm_color_none;
			_restyled = SUCCEEDED(DwmSetWindowAttribute(_window, dwm_corner_preference, &corners, sizeof(corners)));
			_restyled = SUCCEEDED(DwmSetWindowAttribute(_window, dwm_border_color, &border, sizeof(border))) ||
			            _restyled;
		}

		~presented_window()
		{
			if (!_window || !IsWindow(_window)) return;

			if (_made_topmost)
			{
				SetWindowPos(_window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			}

			if (_restyled)
			{
				const auto corners = dwm_corners_default;
				const auto border = dwm_color_default;
				DwmSetWindowAttribute(_window, dwm_corner_preference, &corners, sizeof(corners));
				DwmSetWindowAttribute(_window, dwm_border_color, &border, sizeof(border));
			}
		}

		presented_window(const presented_window&) = delete;
		presented_window& operator=(const presented_window&) = delete;

	private:
		HWND _window = nullptr;
		bool _made_topmost = false;
		bool _restyled = false;
	};

	RECT client_on_screen(const HWND hwnd)
	{
		RECT rc{};
		POINT origin{0, 0};
		if (!GetClientRect(hwnd, &rc) || !ClientToScreen(hwnd, &origin)) return {};
		OffsetRect(&rc, origin.x, origin.y);
		return rc;
	}

	// Whether the user's own desktop is what the screen shows: not while the session is locked, when the lock screen
	// covers it, nor while a secure prompt has the screen. (Windows 7 reports the lock the other way round; this is
	// a Windows 10 and later tool.)
	bool user_desktop_is_shown()
	{
		LPWSTR buffer = nullptr;
		DWORD bytes = 0;

		if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &buffer,
		                                &bytes) &&
		    buffer)
		{
			const auto* const info = reinterpret_cast<const WTSINFOEXW*>(buffer);
			const auto locked = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
			WTSFreeMemory(buffer);
			if (locked) return false;
		}

		const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
		if (!desktop) return false;

		wchar_t name[32]{};
		DWORD needed = 0;
		const auto named = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed) != FALSE;
		CloseDesktop(desktop);
		return named && _wcsicmp(name, L"Default") == 0;
	}

	void fill_black(core::surface& frame)
	{
		const auto size = frame.dimensions();

		for (auto y = 0; y < size.cy; ++y)
		{
			std::fill_n(std::bit_cast<uint32_t*>(frame.pixels_line(y)), size.cx, 0xff000000u);
		}
	}

	// The composed desktop of one output, as DXGI duplicates it for a capture program: every window on it, read
	// back from the GPU in a millisecond or two.
	class desktop_duplication
	{
	public:
		enum class result
		{
			captured,
			// Nothing on the desktop has changed since the last capture.
			unchanged,
			// Lost - a mode change, the secure desktop - and to be opened again.
			failed,
		};

		bool is_open() const { return _duplication != nullptr; }
		HMONITOR monitor() const { return _monitor; }

		bool open(const HMONITOR monitor)
		{
			close();

			ComPtr<IDXGIFactory1> factory;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

			ComPtr<IDXGIAdapter1> adapter;

			for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
			{
				ComPtr<IDXGIOutput> output;

				for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
				{
					DXGI_OUTPUT_DESC desc{};
					if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
					{
						return open(adapter.Get(), output.Get(), desc);
					}
				}
			}

			return false;
		}

		void close()
		{
			_duplication.Reset();
			_staging.Reset();
			_context.Reset();
			_device.Reset();
			_staging_extent = {};
			_monitor = nullptr;
		}

		// The desktop under `area` into `frame`, whose top left is the area's.
		result capture(const RECT& area, core::surface& frame)
		{
			DXGI_OUTDUPL_FRAME_INFO info{};
			ComPtr<IDXGIResource> resource;
			const auto hr = _duplication->AcquireNextFrame(0, &info, &resource);
			if (hr == DXGI_ERROR_WAIT_TIMEOUT) return result::unchanged;
			if (FAILED(hr)) return result::failed;

			const auto copied = copy(area, resource.Get(), frame);
			_duplication->ReleaseFrame();
			return copied ? result::captured : result::failed;
		}

	private:
		bool open(IDXGIAdapter1* const adapter, IDXGIOutput* const output, const DXGI_OUTPUT_DESC& desc)
		{
			// A device of its own, on the adapter the output hangs from: duplication is refused on any other.
			if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
			                             &_device, nullptr, &_context)))
			{
				close();
				return false;
			}

			ComPtr<IDXGIOutput1> output1;
			auto hr = output->QueryInterface(IID_PPV_ARGS(&output1));
			if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(_device.Get(), &_duplication);

			if (FAILED(hr))
			{
				core::log(__FUNCTION__, std::format("desktop duplication refused ({:x})", static_cast<uint32_t>(hr)));
				close();
				return false;
			}

			DXGI_OUTDUPL_DESC duplicated{};
			_duplication->GetDesc(&duplicated);

			// A rotated desktop is duplicated in its own orientation; GDI reads it the right way up.
			if (duplicated.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
			    duplicated.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
			{
				close();
				return false;
			}

			_monitor = desc.Monitor;
			_desktop = desc.DesktopCoordinates;
			return true;
		}

		bool copy(const RECT& area, IDXGIResource* const resource, core::surface& frame)
		{
			ComPtr<ID3D11Texture2D> desktop;
			if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&desktop)))) return false;

			D3D11_TEXTURE2D_DESC desktop_desc{};
			desktop->GetDesc(&desktop_desc);

			// Eight bits a channel in the order the encoder takes. An HDR desktop duplicates otherwise, and GDI
			// reads it as everything else does.
			if (desktop_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
			    desktop_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
			{
				return false;
			}

			// What of the area this output shows. The rest - off its edge, or on another output - is black.
			RECT shown{};
			const auto size = frame.dimensions();
			const auto covered = IntersectRect(&shown, &area, &_desktop) != FALSE;
			const auto whole = covered && EqualRect(&shown, &area) != FALSE;
			if (!whole) fill_black(frame);
			if (!covered) return true;

			if (!_staging || _staging_extent != size)
			{
				D3D11_TEXTURE2D_DESC staging_desc{};
				staging_desc.Width = static_cast<UINT>(size.cx);
				staging_desc.Height = static_cast<UINT>(size.cy);
				staging_desc.MipLevels = 1;
				staging_desc.ArraySize = 1;
				staging_desc.Format = desktop_desc.Format;
				staging_desc.SampleDesc.Count = 1;
				staging_desc.Usage = D3D11_USAGE_STAGING;
				staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

				_staging.Reset();
				if (FAILED(_device->CreateTexture2D(&staging_desc, nullptr, &_staging))) return false;
				_staging_extent = size;
			}

			const auto x = shown.left - area.left;
			const auto y = shown.top - area.top;
			const auto cx = shown.right - shown.left;
			const auto cy = shown.bottom - shown.top;

			D3D11_BOX box{};
			box.left = static_cast<UINT>(shown.left - _desktop.left);
			box.top = static_cast<UINT>(shown.top - _desktop.top);
			box.right = box.left + static_cast<UINT>(cx);
			box.bottom = box.top + static_cast<UINT>(cy);
			box.back = 1;
			_context->CopySubresourceRegion(_staging.Get(), 0, static_cast<UINT>(x), static_cast<UINT>(y), 0,
			                                desktop.Get(), 0, &box);

			// Mapping waits for the copy, so the desktop's frame is still held while it is read.
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(_context->Map(_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

			const auto* const pixels = static_cast<const uint8_t*>(mapped.pData);

			for (auto row = y; row < y + cy; ++row)
			{
				memcpy(frame.pixels_line(row) + static_cast<size_t>(x) * 4,
				       pixels + static_cast<size_t>(row) * mapped.RowPitch + static_cast<size_t>(x) * 4,
				       static_cast<size_t>(cx) * 4);
			}

			_context->Unmap(_staging.Get(), 0);
			return true;
		}

		ComPtr<ID3D11Device> _device;
		ComPtr<ID3D11DeviceContext> _context;
		ComPtr<IDXGIOutputDuplication> _duplication;
		ComPtr<ID3D11Texture2D> _staging;
		sizei _staging_extent;
		HMONITOR _monitor = nullptr;
		RECT _desktop{};
	};

	// A 32-bit top-down bitmap GDI draws on and the recorder reads.
	class gdi_bitmap
	{
	public:
		~gdi_bitmap() { release(); }

		HDC dc() const { return _dc; }
		uint8_t* bits() const { return _bits; }

		bool ensure(const sizei size)
		{
			if (_bitmap && _size == size) return true;
			release();

			_dc = CreateCompatibleDC(nullptr);
			if (!_dc) return false;

			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = size.cx;
			info.bmiHeader.biHeight = -size.cy;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;

			void* bits = nullptr;
			_bitmap = CreateDIBSection(_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);

			if (!_bitmap || !bits)
			{
				release();
				return false;
			}

			_previous = SelectObject(_dc, _bitmap);
			_bits = static_cast<uint8_t*>(bits);
			_size = size;
			return true;
		}

	private:
		void release()
		{
			if (_dc && _previous) SelectObject(_dc, _previous);
			if (_bitmap) DeleteObject(_bitmap);
			if (_dc) DeleteDC(_dc);
			_dc = nullptr;
			_bitmap = nullptr;
			_previous = nullptr;
			_bits = nullptr;
			_size = {};
		}

		HDC _dc = nullptr;
		HBITMAP _bitmap = nullptr;
		HGDIOBJ _previous = nullptr;
		uint8_t* _bits = nullptr;
		sizei _size;
	};

	// The screen under `area` as GDI copies it.
	bool capture_with_gdi(gdi_bitmap& bitmap, const RECT& area, core::surface& frame)
	{
		const auto size = frame.dimensions();
		if (!bitmap.ensure(size)) return false;

		const auto screen = GetDC(nullptr);
		if (!screen) return false;
		// CAPTUREBLT, for the layered windows a bubble is drawn in.
		const auto copied = BitBlt(bitmap.dc(), 0, 0, size.cx, size.cy, screen, area.left, area.top,
		                           SRCCOPY | CAPTUREBLT) != FALSE;
		ReleaseDC(nullptr, screen);
		if (!copied) return false;

		GdiFlush();
		const auto row_bytes = static_cast<size_t>(size.cx) * 4;

		for (auto y = 0; y < size.cy; ++y)
		{
			memcpy(frame.pixels_line(y), bitmap.bits() + y * row_bytes, row_bytes);
		}

		return true;
	}

	// What is drawn over a frame - a pointer, a click's ring - kept so it can be taken off again, and the frame put
	// back as the desktop showed it for the next frame that does not change.
	class frame_overlay
	{
	public:
		// Keeps what the frame shows in this area, as much of it as is in the frame, before it is drawn over.
		void keep(core::surface& frame, const recti area)
		{
			restore(frame);

			const auto size = frame.dimensions();
			const recti kept{std::max(0, area.left), std::max(0, area.top), std::min(size.cx, area.right),
			                 std::min(size.cy, area.bottom)};
			if (kept.left >= kept.right || kept.top >= kept.bottom) return;

			const auto row_bytes = static_cast<size_t>(kept.width()) * 4;
			_saved.resize(row_bytes * kept.height());

			for (auto y = kept.top; y < kept.bottom; ++y)
			{
				memcpy(_saved.data() + (y - kept.top) * row_bytes,
				       frame.pixels_line(y) + static_cast<size_t>(kept.left) * 4, row_bytes);
			}

			_kept = kept;
		}

		void restore(core::surface& frame)
		{
			if (_kept.is_empty()) return;

			const auto row_bytes = static_cast<size_t>(_kept.width()) * 4;

			for (auto y = _kept.top; y < _kept.bottom; ++y)
			{
				memcpy(frame.pixels_line(y) + static_cast<size_t>(_kept.left) * 4,
				       _saved.data() + (y - _kept.top) * row_bytes, row_bytes);
			}

			_kept = {};
		}

	private:
		std::vector<uint8_t> _saved;
		recti _kept;
	};

	// A pointer to draw: its image, the size it is drawn at, its hot spot in it, and where in the frame the hot spot
	// goes.
	struct cursor_image
	{
		HCURSOR cursor = nullptr;
		sizei extent;
		pointi hot_spot;
		pointi at;

		recti bounds() const
		{
			return {at.x - hot_spot.x, at.y - hot_spot.y, at.x - hot_spot.x + extent.cx, at.y - hot_spot.y + extent.cy};
		}
	};

	// A pointer's size and hot spot as Windows draws it.
	std::optional<cursor_image> measure_cursor(const HCURSOR cursor)
	{
		ICONINFO icon{};
		if (!cursor || !GetIconInfo(cursor, &icon)) return {};

		BITMAP mask{};
		const auto measured = icon.hbmMask && GetObjectW(icon.hbmMask, sizeof(mask), &mask) != 0;
		if (icon.hbmMask) DeleteObject(icon.hbmMask);
		if (icon.hbmColor) DeleteObject(icon.hbmColor);
		// A monochrome pointer stacks its AND mask over its XOR mask in one bitmap twice its height.
		const sizei extent{mask.bmWidth, icon.hbmColor ? mask.bmHeight : mask.bmHeight / 2};
		if (!measured || extent.cx <= 0 || extent.cy <= 0) return {};

		return cursor_image{cursor, extent, {static_cast<int>(icon.xHotspot), static_cast<int>(icon.yHotspot)}, {}};
	}

	// The system pointer, where it is over `area`.
	std::optional<cursor_image> system_cursor(const RECT& area)
	{
		CURSORINFO info{};
		info.cbSize = sizeof(info);
		if (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING)) return {};

		auto image = measure_cursor(info.hCursor);
		if (image) image->at = {info.ptScreenPos.x - area.left, info.ptScreenPos.y - area.top};
		return image;
	}

	// Draws a pointer into the frame as Windows draws it over the pixels under it, so one that inverts what it
	// covers - the text beam - inverts the picture recorded.
	void draw_cursor(gdi_bitmap& bitmap, const cursor_image& image, core::surface& frame)
	{
		const auto bounds = image.bounds();
		const auto size = frame.dimensions();
		const recti drawn{std::max(0, bounds.left), std::max(0, bounds.top), std::min(size.cx, bounds.right),
		                  std::min(size.cy, bounds.bottom)};
		if (drawn.left >= drawn.right || drawn.top >= drawn.bottom || !bitmap.ensure(image.extent)) return;

		const auto row_bytes = static_cast<size_t>(drawn.width()) * 4;
		const auto at = [&](const int x, const int y)
		{
			return bitmap.bits() + (static_cast<size_t>(y - bounds.top) * image.extent.cx + (x - bounds.left)) * 4;
		};

		for (auto y = drawn.top; y < drawn.bottom; ++y)
		{
			memcpy(at(drawn.left, y), frame.pixels_line(y) + static_cast<size_t>(drawn.left) * 4, row_bytes);
		}

		GdiFlush();
		DrawIconEx(bitmap.dc(), 0, 0, image.cursor, image.extent.cx, image.extent.cy, 0, nullptr, DI_NORMAL);
		GdiFlush();

		for (auto y = drawn.top; y < drawn.bottom; ++y)
		{
			auto* const pixels = frame.pixels_line(y) + static_cast<size_t>(drawn.left) * 4;
			memcpy(pixels, at(drawn.left, y), row_bytes);
			// GDI leaves alpha as it pleases; the frame is opaque.
			for (size_t x = 0; x < row_bytes; x += 4) pixels[x + 3] = 0xff;
		}
	}

	// The ring a click spreads: Diffractor's accent, smoothed at its edges, `opacity` of it over what is under.
	constexpr uint8_t ring_blue = 0xff;
	constexpr uint8_t ring_green = 0x9e;
	constexpr uint8_t ring_red = 0x4a;

	recti ring_bounds(const pointi center, const double radius, const double thickness)
	{
		const auto reach = static_cast<int>(std::ceil(radius + thickness)) + 1;
		return {center.x - reach, center.y - reach, center.x + reach + 1, center.y + reach + 1};
	}

	void draw_ring(core::surface& frame, const pointi center, const double radius, const double thickness,
	               const double opacity)
	{
		const auto size = frame.dimensions();
		const auto bounds = ring_bounds(center, radius, thickness);
		const auto blend = [](const uint8_t under, const uint8_t over, const double amount)
		{
			return static_cast<uint8_t>(std::lround(under + (over - under) * amount));
		};

		for (auto y = std::max(0, bounds.top); y < std::min(size.cy, bounds.bottom); ++y)
		{
			auto* const line = frame.pixels_line(y);

			for (auto x = std::max(0, bounds.left); x < std::min(size.cx, bounds.right); ++x)
			{
				const auto distance = std::hypot(x - center.x, y - center.y);
				const auto coverage = std::clamp(thickness / 2.0 + 0.5 - std::abs(distance - radius), 0.0, 1.0) *
				                      opacity;
				if (coverage <= 0.0) continue;

				auto* const pixel = line + static_cast<size_t>(x) * 4;
				pixel[0] = blend(pixel[0], ring_blue, coverage);
				pixel[1] = blend(pixel[1], ring_green, coverage);
				pixel[2] = blend(pixel[2], ring_red, coverage);
			}
		}
	}

	// How long a click's ring takes to spread and fade.
	constexpr double click_seconds = 0.45;

	// The pointer a script shows: where it glides from and to, and when, and when it last clicked - all in seconds
	// into the movie, so each frame draws it where it was when the frame was captured.
	struct scripted_pointer
	{
		bool shown = false;
		pointi from;
		pointi to;
		double move_start = 0.0;
		double move_seconds = 0.0;
		double click_at = -1.0;

		pointi at(const double t) const
		{
			if (move_seconds <= 0.0 || t >= move_start + move_seconds) return to;
			if (t <= move_start) return from;

			// Eased in and out, as a hand moves a mouse.
			const auto u = (t - move_start) / move_seconds;
			const auto eased = u * u * (3.0 - 2.0 * u);
			return {from.x + static_cast<int>(std::lround((to.x - from.x) * eased)),
			        from.y + static_cast<int>(std::lround((to.y - from.y) * eased))};
		}
	};

	// Waits out the rest of a frame. High resolution where the system has it (Windows 10 1803 on), so a 33 ms
	// frame is not rounded to the scheduler's 15.6 ms tick.
	class frame_timer
	{
	public:
		frame_timer()
		{
			_timer = CreateWaitableTimerExW(nullptr, nullptr, create_waitable_timer_high_resolution, TIMER_ALL_ACCESS);
			if (!_timer) _timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
		}

		~frame_timer()
		{
			if (_timer) CloseHandle(_timer);
		}

		frame_timer(const frame_timer&) = delete;
		frame_timer& operator=(const frame_timer&) = delete;

		void wait(const double seconds) const
		{
			if (seconds <= 0) return;

			if (_timer)
			{
				LARGE_INTEGER due{};
				due.QuadPart = -static_cast<LONGLONG>(seconds * 10000000.0);
				if (SetWaitableTimer(_timer, &due, 0, nullptr, nullptr, FALSE) &&
				    WaitForSingleObject(_timer, INFINITE) == WAIT_OBJECT_0)
				{
					return;
				}
			}

			Sleep(static_cast<DWORD>(seconds * 1000.0));
		}

	private:
		HANDLE _timer = nullptr;
	};

	// The scripted pointer's size against the system's: a viewer watches it on a smaller screen than it was made on.
	constexpr double scripted_pointer_scale = 1.5;

	// The window's DPI: GetDpiForWindow where the system has it, the screen's otherwise.
	UINT window_dpi(const HWND window)
	{
		using get_dpi_for_window = UINT(WINAPI*)(HWND);
		static const auto proc = reinterpret_cast<get_dpi_for_window>(
			GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
		if (proc && window) return std::max<UINT>(USER_DEFAULT_SCREEN_DPI, proc(window));

		const auto dc = GetDC(window);
		const auto dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : USER_DEFAULT_SCREEN_DPI;
		if (dc) ReleaseDC(window, dc);
		return std::max<UINT>(USER_DEFAULT_SCREEN_DPI, dpi);
	}

	// The arrow's file in the user's pointer scheme, or Windows' own where the scheme names none.
	std::wstring arrow_cursor_file()
	{
		wchar_t value[MAX_PATH]{};
		DWORD bytes = sizeof(value);
		std::wstring path;

		if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"Arrow",
		                 RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, value, &bytes) == ERROR_SUCCESS)
		{
			path = value;
		}

		if (path.empty()) path = L"%SystemRoot%\\Cursors\\aero_arrow.cur";

		wchar_t expanded[MAX_PATH]{};
		const auto length = ExpandEnvironmentStringsW(path.c_str(), expanded, MAX_PATH);
		return length > 0 && length <= MAX_PATH ? std::wstring(expanded) : std::wstring();
	}

	// The arrow, at the size the scripted pointer is drawn for this window: read from its file, which holds it at
	// several sizes, so it is sharp at any. A system cursor loads only at the size the system chose, so where the
	// file cannot be read the system's arrow is drawn stretched.
	class scripted_cursor
	{
	public:
		explicit scripted_cursor(const HWND window)
		{
			// A pointer is 32 pixels at 96 DPI, the system's size before its own scale.
			const auto size = static_cast<int>(
				std::lround(32.0 * window_dpi(window) / USER_DEFAULT_SCREEN_DPI * scripted_pointer_scale));

			if (const auto file = arrow_cursor_file(); !file.empty())
			{
				_cursor = static_cast<HCURSOR>(
					LoadImageW(nullptr, file.c_str(), IMAGE_CURSOR, size, size, LR_LOADFROMFILE));
			}

			if (_cursor)
			{
				_owned = true;
				_image = measure_cursor(_cursor);
				return;
			}

			// Shared by the system, so never destroyed here.
			_cursor = LoadCursorW(nullptr, IDC_ARROW);
			_image = measure_cursor(_cursor);

			if (_image)
			{
				const auto native = _image->extent;
				_image->extent = {size, size};
				_image->hot_spot = {_image->hot_spot.x * size / std::max(1, native.cx),
				                    _image->hot_spot.y * size / std::max(1, native.cy)};
			}
		}

		~scripted_cursor()
		{
			if (_owned && _cursor) DestroyCursor(_cursor);
		}

		scripted_cursor(const scripted_cursor&) = delete;
		scripted_cursor& operator=(const scripted_cursor&) = delete;

		const std::optional<cursor_image>& image() const { return _image; }

	private:
		HCURSOR _cursor = nullptr;
		bool _owned = false;
		std::optional<cursor_image> _image;
	};

	class win_screen_recording final : public screen_recording
	{
	public:
		explicit win_screen_recording(screen_recording_request request) : _request(std::move(request))
		{
			const auto* const window = std::any_cast<HWND>(&_request.window);
			_window = window ? *window : nullptr;
			if (_request.present_window) _presented = std::make_unique<presented_window>(_window);
			_thread = std::thread([this] { run(); });
		}

		~win_screen_recording() override { stop(); }

		win_screen_recording(const win_screen_recording&) = delete;
		win_screen_recording& operator=(const win_screen_recording&) = delete;

		bool is_finished() const override { return _finished.load(); }

		void stop() override
		{
			_stopping.store(true);
			if (_thread.joinable()) _thread.join();
			_presented.reset();
		}

		std::string summary() const override
		{
			const std::lock_guard lock(_summary_mutex);
			return _summary;
		}

		double elapsed() const override
		{
			const auto started = _started.load();
			if (started == not_started) return -1.0;
			return std::chrono::duration<double>(clock::now().time_since_epoch() - clock::duration(started)).count();
		}

		void pointer(const std::optional<pointi> screen, const double glide_seconds) override
		{
			const auto now = std::max(0.0, elapsed());
			const std::lock_guard lock(_pointer_mutex);

			if (!screen)
			{
				_pointer.shown = false;
				return;
			}

			_pointer.from = _pointer.shown ? _pointer.at(now) : *screen;
			_pointer.to = *screen;
			_pointer.move_start = now;
			_pointer.move_seconds = _pointer.shown ? std::max(0.0, glide_seconds) : 0.0;
			_pointer.shown = true;
		}

		void click() override
		{
			const auto now = std::max(0.0, elapsed());
			const std::lock_guard lock(_pointer_mutex);
			_pointer.click_at = now;
			_click_point = _pointer.at(now);
		}

		void mark(const std::string_view name) override
		{
			const auto now = std::max(0.0, elapsed());
			const std::lock_guard lock(_markers_mutex);
			_markers.emplace_back(now, std::string(name));
		}

	private:
		using clock = std::chrono::steady_clock;
		static constexpr clock::rep not_started = std::numeric_limits<clock::rep>::min();

		void run()
		{
			const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			auto summary = record();
			if (SUCCEEDED(com)) CoUninitialize();

			core::log(__FUNCTION__, summary);

			{
				const std::lock_guard lock(_summary_mutex);
				_summary = std::move(summary);
			}

			_finished.store(true);
		}

		// The pointers and ring of the frame captured `t` seconds in, drawn over it; `overlay` keeps what they cover.
		void draw_pointers(const RECT& area, const double t, const std::optional<cursor_image>& arrow,
		                   gdi_bitmap& bitmap, frame_overlay& overlay, core::surface& frame)
		{
			scripted_pointer scripted;
			pointi click_point;

			{
				const std::lock_guard lock(_pointer_mutex);
				scripted = _pointer;
				click_point = _click_point;
			}

			const auto to_frame = [&area](const pointi screen)
			{
				return pointi{screen.x - static_cast<int>(area.left), screen.y - static_cast<int>(area.top)};
			};

			std::optional<cursor_image> system;
			if (_request.draw_pointer) system = system_cursor(area);

			std::optional<cursor_image> drawn;

			if (scripted.shown && arrow)
			{
				drawn = arrow;
				drawn->at = to_frame(scripted.at(t));
			}

			// The ring grows from the pointer's tip, eased out, and fades as it does.
			const auto click_age = t - scripted.click_at;
			const auto clicking = scripted.shown && scripted.click_at >= 0.0 && click_age >= 0.0 &&
			                      click_age < click_seconds;
			const auto ring_scale = arrow ? arrow->extent.cx : 48;
			const auto progress = clicking ? click_age / click_seconds : 1.0;
			const auto spread = 1.0 - (1.0 - progress) * (1.0 - progress);
			const auto radius = ring_scale * (0.12 + 0.5 * spread);
			const auto thickness = std::max(2.0, ring_scale * 0.07);
			const auto ring_center = to_frame(click_point);

			recti covered;
			const auto cover = [&covered](const recti r)
			{
				covered = covered.is_empty() ? r : covered.make_union(r);
			};
			if (system) cover(system->bounds());
			if (drawn) cover(drawn->bounds());
			if (clicking) cover(ring_bounds(ring_center, radius, thickness));
			if (covered.is_empty()) return;

			overlay.keep(frame, covered);
			if (system) draw_cursor(bitmap, *system, frame);
			if (clicking) draw_ring(frame, ring_center, radius, thickness, 0.9 * (1.0 - progress));
			if (drawn) draw_cursor(bitmap, *drawn, frame);
		}

		void write_markers(const double length) const
		{
			std::string text;

			{
				const std::lock_guard lock(_markers_mutex);
				for (const auto& [seconds, name] : _markers) text += std::format("{:.3f}\t{}\n", seconds, name);
			}

			text += std::format("{:.3f}\tend\n", length);

			const auto path = screen_recording_markers_path(_request.path);
			if (!platform::blob_save_to_file({std::bit_cast<const uint8_t*>(text.data()), text.size()}, path))
			{
				core::log(__FUNCTION__, std::format("Recording's markers could not be written to {}", path));
			}
		}

		std::string record()
		{
			if (!_window || !IsWindow(_window)) return "Recording failed: there is no window to record";

			const auto fps = std::max(1, _request.frame_rate);
			const auto client = client_on_screen(_window);
			// Even, as every encoder here wants, and fixed for the whole recording: a window resized meanwhile is
			// recorded at the size it started at, from its top left.
			const sizei extent{(client.right - client.left) & ~1, (client.bottom - client.top) & ~1};
			if (extent.cx < 16 || extent.cy < 16) return "Recording failed: the window has no client area to record";

			platform::movie_writer_request request;
			request.path = _request.path;
			request.extent = extent;
			request.frame_rate = fps;
			request.video_bitrate = static_cast<int>(
				std::min(recording_max_bitrate,
			             static_cast<double>(extent.cx) * extent.cy * fps * recording_bits_per_pixel));
			const auto writer = platform::create_movie_writer(request);
			if (!writer) return std::format("Recording failed: no movie can be written to {}", _request.path);

			const auto frame = std::make_shared<core::surface>();
			if (!frame->alloc(extent, core::texture_format::RGB))
			{
				writer->abandon();
				return "Recording failed: no memory for a frame";
			}

			fill_black(*frame);

			desktop_duplication duplication;
			gdi_bitmap gdi;
			gdi_bitmap pointer_bitmap;
			frame_overlay overlay;
			const scripted_cursor arrow(_window);
			const frame_timer timer;
			auto use_gdi = false;
			auto duplication_failures = 0;

			const auto max_frames = _request.seconds > 0 ? std::max<int64_t>(1, std::llround(_request.seconds * fps))
			                                             : std::numeric_limits<int64_t>::max();
			_started.store(clock::now().time_since_epoch().count());

			int64_t written = 0;
			int64_t captures = 0;
			int64_t late = 0;
			int64_t hidden = 0;
			auto desktop_shown = true;
			auto capture_seconds = 0.0;
			std::string failure;

			while (!_stopping.load() && written < max_frames && IsWindow(_window))
			{
				const auto origin = client_on_screen(_window);
				const RECT area{origin.left, origin.top, origin.left + extent.cx, origin.top + extent.cy};
				const auto capture_started = elapsed();

				// Twice a second is often enough to say how much of a movie is black.
				if (captures % (fps / 2 + 1) == 0) desktop_shown = user_desktop_is_shown();

				if (!use_gdi)
				{
					const auto monitor = MonitorFromWindow(_window, MONITOR_DEFAULTTONEAREST);
					const auto opened = (!duplication.is_open() || duplication.monitor() != monitor) &&
					                    duplication.open(monitor);

					const auto result = duplication.is_open() ? duplication.capture(area, *frame)
					                                          : desktop_duplication::result::failed;

					if (result == desktop_duplication::result::failed)
					{
						duplication.close();

						if (++duplication_failures >= duplication_attempts)
						{
							core::log(__FUNCTION__, "desktop duplication is unavailable: recording with GDI");
							use_gdi = true;
						}
					}
					else
					{
						duplication_failures = 0;
						// Duplication hands over a picture when the desktop changes, which a still one may not have
						// done since it was opened, so the first is read the slow way.
						if (opened && result == desktop_duplication::result::unchanged)
							capture_with_gdi(gdi, area, *frame);
					}
				}

				if (use_gdi) capture_with_gdi(gdi, area, *frame);

				capture_seconds += elapsed() - capture_started;
				++captures;

				draw_pointers(area, capture_started, arrow.image(), pointer_bitmap, overlay, *frame);

				// Every frame up to now is written, so the movie keeps the wall clock's time: a frame the loop was late
				// for repeats the picture rather than shortening the movie, and a script's timings stay true in it.
				const auto due = std::min(max_frames - 1, static_cast<int64_t>(elapsed() * fps));
				late += std::max<int64_t>(0, due - written);
				const auto first = written;

				for (; written <= due; ++written)
				{
					if (!writer->write_frame(frame, static_cast<double>(written) / fps))
					{
						failure = writer->last_error();
						break;
					}
				}

				if (!desktop_shown) hidden += written - first;

				overlay.restore(*frame);
				if (!failure.empty()) break;

				timer.wait(static_cast<double>(written) / fps - elapsed());
			}

			if (!failure.empty())
			{
				writer->abandon();
				return std::format("Recording failed: {}", failure);
			}

			if (written == 0)
			{
				writer->abandon();
				return "Recording failed: no frame was written";
			}

			if (!writer->close())
				return std::format("Recording failed to finish {}: {}", _request.path, writer->last_error());

			const auto length = static_cast<double>(written) / fps;
			write_markers(length);

			const auto hidden_note =
				hidden > 0 ? std::format("; {:.1f} s of it while the screen was locked or held by a secure "
			                             "prompt, so it shows that and not the window",
			                             static_cast<double>(hidden) / fps)
						   : std::string();

			return std::format(
				"Recorded {}: {}x{} at {} fps, {:.2f} s in {} frames ({} late) by {}, {:.1f} ms a capture{}",
				_request.path, extent.cx, extent.cy, fps, length, written, late,
				use_gdi ? "GDI" : "desktop duplication",
				captures > 0 ? capture_seconds * 1000.0 / static_cast<double>(captures) : 0.0, hidden_note);
		}

		screen_recording_request _request;
		HWND _window = nullptr;
		std::unique_ptr<presented_window> _presented;
		std::atomic_bool _stopping = false;
		std::atomic_bool _finished = false;
		std::atomic<clock::rep> _started = not_started;
		mutable std::mutex _summary_mutex;
		std::string _summary;
		mutable std::mutex _pointer_mutex;
		scripted_pointer _pointer;
		pointi _click_point;
		mutable std::mutex _markers_mutex;
		std::vector<std::pair<double, std::string>> _markers;
		// Last, so everything the thread reads exists before it starts.
		std::thread _thread;
	};
}

std::unique_ptr<screen_recording> start_screen_recording(const screen_recording_request& request)
{
	return std::make_unique<win_screen_recording>(request);
}

#endif
