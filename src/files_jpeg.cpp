// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: JPEG encoding and lossless turns, which libfiles does, held to Diffractor's surfaces, metadata,
// decode budget and errors; and the signatures of the metadata a JPEG's markers carry. libfiles also
// decodes JPEGs and reads their structure.

#include "pch.h"
#include "files_jpeg.h"

#include "files.h"
#include "libfiles/encode.h"

static bool has_signature(const df::cspan cs, const void* signature, const size_t signature_len)
{
	return cs.size >= signature_len && memcmp(signature, cs.data, signature_len) == 0;
}

bool is_exif_signature(const df::cspan cs)
{
	return has_signature(cs, exif_signature.data(), exif_signature.size());
}

df::blob strip_exif_tiff_prefix(df::blob data)
{
	if (data.size() >= 4)
	{
		const size_t tiff_offset = 4u + (static_cast<size_t>(data[0]) << 24 |
			static_cast<size_t>(data[1]) << 16 |
			static_cast<size_t>(data[2]) << 8 |
			static_cast<size_t>(data[3]));

		// The bound is what keeps a hostile offset from erasing past the end.
		if (tiff_offset <= data.size())
		{
			data.erase(data.begin(), data.begin() + tiff_offset);
		}
	}

	if (is_exif_signature(data))
	{
		data.erase(data.begin(), data.begin() + exif_signature.size());
	}

	return data;
}

bool is_iptc_signature(const df::cspan cs)
{
	return has_signature(cs, iptc_signature.data(), iptc_signature.size());
}

bool is_xmp_signature(const df::cspan cs)
{
	return has_signature(cs, xmp_signature.data(), xmp_signature.size());
}

bool is_icc_signature(const df::cspan cs)
{
	return has_signature(cs, icc_signature.data(), icc_signature.size());
}

static constexpr size_t default_photoshop_resource_cap = 16u * 1024u * 1024u;
static std::atomic<size_t> s_photoshop_resource_cap{default_photoshop_resource_cap};

void files_test_hooks::set_jpeg_photoshop_resource_cap(const size_t bytes)
{
	s_photoshop_resource_cap.store(bytes == 0 ? default_photoshop_resource_cap : bytes, std::memory_order_relaxed);
}

size_t files_test_hooks::jpeg_photoshop_resource_cap()
{
	return s_photoshop_resource_cap.load(std::memory_order_relaxed);
}

libfiles::embedded_metadata embedded_metadata_of(const metadata_parts& metadata)
{
	const auto view = [](const df::blob& b) { return std::span<const uint8_t>(b.data(), b.size()); };
	return {view(metadata.exif), view(metadata.xmp), view(metadata.iptc), view(metadata.icc),
	        view(metadata.photoshop_resources)};
}

namespace
{
	libfiles::jpeg_options jpeg_options_of(const file_encode_params& params)
	{
		libfiles::jpeg_options options;
		options.quality = params.jpeg_save_quality;
		options.source = {params.jpeg_source.data, params.jpeg_source.size};
		return options;
	}

	// What libfiles noted - a source whose tables could not be adopted, a Photoshop resource too large for
	// any marker - goes to the log, where the encoder wrote it before it moved.
	void log_notes(const libfiles::jpeg_encoder& encoder)
	{
		for (const auto& note : encoder.notes()) df::log("jpeg_encoder", note);
	}

	[[noreturn]] void fail(const std::string_view where, const libfiles::encode_error& e)
	{
		df::log(where, e.what());
		throw app_exception(e.what());
	}
}

jpeg_encoder::jpeg_encoder() : _encoder(std::make_unique<libfiles::jpeg_encoder>())
{
}

jpeg_encoder::~jpeg_encoder() = default;

void jpeg_encoder::start(const uint32_t cx, const uint32_t cy, const ui::orientation orientation,
                         const metadata_parts& metadata, const file_encode_params& params)
{
	try
	{
		_encoder->start(cx, cy, static_cast<uint16_t>(orientation), embedded_metadata_of(metadata),
		                jpeg_options_of(params));
		log_notes(*_encoder);
	}
	catch (const libfiles::encode_error& e)
	{
		log_notes(*_encoder);
		fail(__FUNCTION__, e);
	}
}

df::blob jpeg_encoder::encode(const uint32_t cx, const uint32_t cy, const uint8_t* bitmap, const uint32_t stride,
                              const ui::orientation orientation, const metadata_parts& metadata,
                              const file_encode_params& params)
{
	try
	{
		const auto data = _encoder->encode(cx, cy, bitmap, stride, static_cast<uint16_t>(orientation),
		                                   embedded_metadata_of(metadata), jpeg_options_of(params));
		log_notes(*_encoder);
		return df::blob(data.begin(), data.end());
	}
	catch (const libfiles::encode_error& e)
	{
		log_notes(*_encoder);
		fail(__FUNCTION__, e);
	}
}

sizei jpeg_encoder::dimensions() const
{
	return {static_cast<int>(_encoder->width()), static_cast<int>(_encoder->height())};
}

jpeg_transformer::jpeg_transformer() : _transformer(std::make_unique<libfiles::jpeg_transformer>())
{
}

jpeg_transformer::~jpeg_transformer() = default;

bool jpeg_transformer::read_header(const df::cspan cs)
{
	try
	{
		return _transformer->read_header({cs.data, cs.size});
	}
	catch (const libfiles::encode_error& e)
	{
		fail(__FUNCTION__, e);
	}
}

sizei jpeg_transformer::dimensions() const
{
	return {static_cast<int>(_transformer->width()), static_cast<int>(_transformer->height())};
}

int64_t jpeg_transformer::coefficient_bytes() const
{
	return _transformer->coefficient_bytes();
}

df::blob jpeg_transformer::transform(const df::cspan src, jpeg_encoder& encoder, const simple_transform transform,
                                     bool* const over_budget) const
{
	if (over_budget) *over_budget = false;

	try
	{
		// simple_transform counts from none at 0, where the EXIF orientation libfiles takes counts from 1.
		const auto orientation = static_cast<uint16_t>(static_cast<int>(transform) + 1);
		const auto budget = static_cast<uint64_t>(std::max<int64_t>(df::max_decode_bytes, 0));
		const auto turned = _transformer->turn({src.data, src.size}, *encoder._encoder, orientation, budget);

		if (turned.over_budget)
		{
			df::log(__FUNCTION__, std::format("rotate of {} x {} needs {}, over the {} budget", turned.width,
			                                  turned.height, df::file_size(turned.workspace_bytes).str(),
			                                  df::file_size(df::max_decode_bytes).str()));
			if (over_budget) *over_budget = true;
		}

		return df::blob(turned.data.begin(), turned.data.end());
	}
	catch (const libfiles::encode_error& e)
	{
		fail(__FUNCTION__, e);
	}
}

ui::image_ptr save_jpeg(const ui::const_surface_ptr& surface_in, const metadata_parts& metadata,
                        const file_encode_params& encoder_params)
{
	const auto dimensions_in = surface_in->dimensions();
	const auto orientation_in = surface_in->orientation();

	jpeg_encoder encoder;
	auto result_data = encoder.encode(dimensions_in.cx, dimensions_in.cy, surface_in->pixels(),
	                                  static_cast<uint32_t>(surface_in->stride()), orientation_in, metadata,
	                                  encoder_params);

	return std::make_shared<ui::image>(std::move(result_data), encoder.dimensions(), ui::image_format::JPEG,
	                                   orientation_in);
}
