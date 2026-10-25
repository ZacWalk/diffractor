// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: FFmpeg's D3D11VA hardware decoding on Windows, the application's side of it: the one decoder
// whose pictures the renderer can present, whether the driver decodes a stream, and the decode device,
// made on the GPU the renderer draws with.

#include "pch.h"

#include "libdesktop/platform_win.h"
#include "libdesktop/platform_win_render.h"

// Before FFmpeg's header, which includes it inside extern "C", where its C++ operators cannot be declared.
#include <d3d11.h>

#include "av_format.h"

extern "C" {
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
}

// The one hardware decoder the renderer can present from. Frames must arrive as D3D11 array
// textures for update() to share them with the render device, so any other hwaccel the codec
// advertises - dxva2 among them - is deliberately not selected.
av_hw_decode_target av_platform_hw_decode_target()
{
	return {AV_HWDEVICE_TYPE_D3D11VA, AV_PIX_FMT_D3D11};
}

// The decoder profiles FFmpeg's D3D11VA hwaccels ask the driver for (libavcodec/dxva2.c), spelled out
// so the check needs no GUID library. Values as d3d11.h's D3D11_DECODER_PROFILE_*.
namespace
{
	constexpr GUID profile_h264_nofgt = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
	constexpr GUID profile_h264_fgt = {0x1b81be69, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
	constexpr GUID profile_mpeg2 = {0xee27417f, 0x5e28, 0x4e65, {0xbe, 0xea, 0x1d, 0x26, 0xb5, 0x08, 0xad, 0xc9}};
	constexpr GUID profile_mpeg2and1 = {0x86695f12, 0x340e, 0x4f04, {0x9f, 0xd3, 0x92, 0x53, 0xdd, 0x32, 0x74, 0x60}};
	constexpr GUID profile_vc1 = {0x1b81bea3, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
	constexpr GUID profile_vc1_d2010 = {0x1b81bea4, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
	constexpr GUID profile_hevc_main = {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
	constexpr GUID profile_hevc_main10 = {0x107af0e0, 0xef1a, 0x4d19, {0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13}};
	constexpr GUID profile_vp9_0 = {0x463707f8, 0xa1d0, 0x4585, {0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e}};
	constexpr GUID profile_vp9_2 = {0xa4c749ef, 0x6ecf, 0x48aa, {0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7}};
	constexpr GUID profile_av1_0 = {0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};

	// The profiles that decode this codec at this depth; any one of them will do.
	std::vector<GUID> hw_decoder_profiles(const AVCodecID codec, const int bit_depth)
	{
		switch (codec)
		{
		case AV_CODEC_ID_H264:
			if (bit_depth <= 8) return {profile_h264_nofgt, profile_h264_fgt};
			break;
		case AV_CODEC_ID_MPEG2VIDEO:
			if (bit_depth <= 8) return {profile_mpeg2, profile_mpeg2and1};
			break;
		case AV_CODEC_ID_VC1:
		case AV_CODEC_ID_WMV3:
			if (bit_depth <= 8) return {profile_vc1_d2010, profile_vc1};
			break;
		case AV_CODEC_ID_HEVC:
			if (bit_depth <= 8) return {profile_hevc_main};
			if (bit_depth == 10) return {profile_hevc_main10};
			break;
		case AV_CODEC_ID_VP9:
			if (bit_depth <= 8) return {profile_vp9_0};
			if (bit_depth == 10) return {profile_vp9_2};
			break;
		case AV_CODEC_ID_AV1:
			if (bit_depth <= 10) return {profile_av1_0};
			break;
		default:
			break;
		}

		return {};
	}

	ID3D11VideoDevice* hw_video_device(AVBufferRef* device)
	{
		const auto* const device_ctx = device ? std::bit_cast<const AVHWDeviceContext*>(device->data) : nullptr;
		const auto* const hwctx = device_ctx ? static_cast<const AVD3D11VADeviceContext*>(device_ctx->hwctx) : nullptr;
		return hwctx ? hwctx->video_device : nullptr;
	}
}

// The questions FFmpeg's hwaccel asks at initialisation, asked first: the profile, the output format,
// and a decoder configuration at this size.
bool av_platform_hw_decode_supported(AVBufferRef* device, const AVCodecID codec, const int bit_depth, const int width,
                                     const int height)
{
	auto* const video_device = hw_video_device(device);
	if (!video_device || width <= 0 || height <= 0) return false;

	const auto wanted = hw_decoder_profiles(codec, bit_depth);
	const auto format = bit_depth > 8 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
	const auto count = video_device->GetVideoDecoderProfileCount();

	for (UINT i = 0; i < count; ++i)
	{
		GUID profile = {};
		if (FAILED(video_device->GetVideoDecoderProfile(i, &profile))) continue;
		if (std::ranges::find(wanted, profile) == wanted.end()) continue;

		BOOL supported = FALSE;
		if (FAILED(video_device->CheckVideoDecoderFormat(&profile, format, &supported)) || !supported) continue;

		const D3D11_VIDEO_DECODER_DESC desc = {profile, static_cast<UINT>(width), static_cast<UINT>(height), format};
		UINT configs = 0;

		if (SUCCEEDED(video_device->GetVideoDecoderConfigCount(&desc, &configs)) && configs > 0) return true;
	}

	return false;
}

bool av_platform_hw_device_usable(AVBufferRef* device)
{
	const auto* const device_ctx = device ? std::bit_cast<const AVHWDeviceContext*>(device->data) : nullptr;
	const auto* const hwctx = device_ctx ? static_cast<const AVD3D11VADeviceContext*>(device_ctx->hwctx) : nullptr;
	return hwctx && hwctx->device && hwctx->device->GetDeviceRemovedReason() == S_OK;
}

// Left to itself FFmpeg makes the device on whichever adapter is the default when the first video
// opens. That need not be the renderer's - docking, or an external display becoming the primary one,
// moves the default - and the shared-texture bridge cannot cross adapters, so every frame was then
// downloaded and uploaded again.
AVBufferRef* av_platform_create_hw_device(const int device_type)
{
	if (device_type != AV_HWDEVICE_TYPE_D3D11VA) return nullptr;

	if (const auto adapter = find_adapter_by_luid(render_adapter()))
	{
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		const auto name = str::utf16_to_utf8(desc.Description);

		// What FFmpeg's own creation asks for: the video interfaces, at the default feature levels.
		ComPtr<ID3D11Device> device;
		const auto hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		                                  D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
		                                  &device, nullptr, nullptr);

		if (SUCCEEDED(hr))
		{
			// Decode threads and the UI thread's copy out of the decoder's surfaces share the device, so
			// it is protected the way FFmpeg protects the ones it makes.
			ComPtr<ID3D10Multithread> multithread;
			if (SUCCEEDED(device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

			auto* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);

			if (ref)
			{
				// The context owns the device from here, and releases it on a failed init as well as with
				// its last reference. Init fills in the contexts, the video interfaces and the lock.
				auto* const device_ctx = std::bit_cast<AVHWDeviceContext*>(ref->data);
				static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx)->device = device.Detach();

				if (av_hwdevice_ctx_init(ref) == 0)
				{
					df::log(__FUNCTION__, "hardware decode device on " + name);
					return ref;
				}

				av_buffer_unref(&ref);
			}

			df::log(__FUNCTION__, std::format("FFmpeg refused the decode device on {} - using the default adapter",
			                                  name));
		}
		else
		{
			df::log(__FUNCTION__, std::format("no decode device on {} ({:x}) - using the default adapter", name,
			                                  static_cast<uint32_t>(hr)));
		}
	}

	AVBufferRef* ref = nullptr;
	return av_hwdevice_ctx_create(&ref, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0 ? ref : nullptr;
}

// FFmpeg's D3D11VA device takes its lock around every use of its pictures, decoding included, and the
// renderer holds it while it copies one out.
void av_platform_decode_lock(const AVHWFramesContext* frames, ui::video_picture& picture)
{
	auto* const hwctx = frames && frames->device_ctx
		                    ? static_cast<AVD3D11VADeviceContext*>(frames->device_ctx->hwctx)
		                    : nullptr;
	if (!hwctx) return;

	picture.lock = hwctx->lock;
	picture.unlock = hwctx->unlock;
	picture.lock_context = hwctx->lock_ctx;
}
