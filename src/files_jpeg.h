// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: JPEG encoding and lossless turns, which libfiles does, and metadata marker detection.
// libfiles also decodes JPEGs.

#pragma once

class file_scan_result;
class read_stream;

inline constexpr auto xmp_signature = "http://ns.adobe.com/xap/1.0/\0"sv;
inline constexpr auto icc_signature = "ICC_PROFILE\0"sv;
inline constexpr auto iptc_signature = "Photoshop 3.0\08BIM\04\04\0\0\0\0"sv;
inline constexpr std::array<uint8_t, 6> exif_signature = {0x45, 0x78, 0x69, 0x66, 0x00, 0x00};

bool is_exif_signature(df::cspan cs);
bool is_iptc_signature(df::cspan cs);
bool is_xmp_signature(df::cspan cs);
bool is_icc_signature(df::cspan cs);

// Strips the ISO/IEC 23008-12 ExifDataBlock header (a 4-byte big-endian count of the bytes
// between that field and the TIFF header) and any following "Exif\0\0" signature, leaving a
// bare TIFF stream. Used by the HEIF and JXL box readers, which share this container format.
df::blob strip_exif_tiff_prefix(df::blob data);

inline constexpr size_t exif_signature_len = exif_signature.size();
inline constexpr size_t iptc_signature_len = iptc_signature.size();
inline constexpr size_t icc_signature_len = icc_signature.size() + 2; // + sequence number and count
inline constexpr size_t xmp_signature_len = xmp_signature.size();

struct metadata_parts;
struct file_encode_params;

namespace libfiles
{
	struct embedded_metadata;
	class jpeg_encoder;
	class jpeg_transformer;
}

// The blocks as libfiles' encoders take them: views of the parts, which must outlive the encode.
libfiles::embedded_metadata embedded_metadata_of(const metadata_parts& metadata);

// libfiles' JPEG encoder, writing BGRX rows with their metadata. Long-lived and reused for every image,
// as the one files holds is.
class jpeg_encoder final : public df::no_copy
{
public:
	std::unique_ptr<libfiles::jpeg_encoder> _encoder;

	jpeg_encoder();
	~jpeg_encoder() override;

	void start(uint32_t cx, uint32_t cy, ui::orientation orientation, const metadata_parts& metadata,
	           const file_encode_params& params);
	df::blob encode(uint32_t cx, uint32_t cy, const uint8_t* bitmap, uint32_t stride, ui::orientation orientation,
	                const metadata_parts& metadata, const file_encode_params& params);

	// The size the image started last is written at.
	sizei dimensions() const;
};

// A JPEG's header, and its lossless transforms: what works on a JPEG's coefficients rather than its
// pixels. The object is long-lived, reused for every image, and reset before and after each use.
class jpeg_transformer final : df::no_copy
{
public:
	std::unique_ptr<libfiles::jpeg_transformer> _transformer;

	jpeg_transformer();
	~jpeg_transformer() override;

	bool read_header(df::cspan cs);
	sizei dimensions() const;

	// Bytes libjpeg holds in one whole-image DCT coefficient array for the stream whose header has
	// been read - the full resolution always, which a lossless transform holds twice. Zero for a
	// lossless stream, whose data units are single samples that this accounting does not describe.
	int64_t coefficient_bytes() const;

	// Empty when the rotation cannot be done losslessly or the stream cannot be read. over_budget,
	// when given, is set when it could have been but its coefficients would not fit the decode
	// budget - a refusal a caller must not answer with a re-encode, which holds more still.
	df::blob transform(df::cspan src, jpeg_encoder& encoder, simple_transform transform,
	                   bool* over_budget = nullptr) const;
};
