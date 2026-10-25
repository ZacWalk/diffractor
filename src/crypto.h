// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Cryptographic utilities facade. Provides HMAC-SHA1, CRC32C, FNV-1a hashing and a DCT
// perceptual image hash, used throughout the application.

#pragma once

namespace crypto
{
	// Side of the grayscale reduction perceptual_hash consumes.
	constexpr size_t phash_extent = 32;
	constexpr size_t phash_pixels = phash_extent * phash_extent;

	// A 64-bit DCT perceptual hash of a 32x32 grayscale reduction, for recognising the same picture
	// across a re-encode or a resize. Returns 0 when the image carries too little detail to identify
	// it - a blank frame, a solid colour or a plain scanned page - because at that point every such
	// image hashes alike and the answer would be confident and wrong. 0 therefore means "no opinion",
	// never "matches another zero".
	uint64_t perceptual_hash(const uint8_t* gray, size_t len);

	// The same picture rotated is a different bitmap, so a quarter turn cannot be recovered from a
	// finished hash: it permutes the DCT coefficients and flips the sign of half of them, and a
	// "greater than median" bit cannot carry a sign. The four orientations are therefore hashed from
	// the pixels and kept together. Index 0 is the picture as stored; each following entry is one
	// further quarter turn clockwise.
	using phash_rotations = std::array<uint64_t, 4>;
	phash_rotations perceptual_hash_rotations(const uint8_t* gray, size_t len);

	// Bit 0 is never set by a real hash - the DC coefficient it would come from is excluded - so it is
	// free to mark an image that was hashed and declined. Callers that persist a hash need to tell
	// that apart from "not hashed yet", or they re-read the same picture forever.
	constexpr uint64_t phash_declined = 1ull;

	constexpr bool phash_is_usable(const uint64_t h)
	{
		return h > phash_declined;
	}

	inline int phash_distance(const uint64_t left, const uint64_t right)
	{
		return static_cast<int>(std::popcount(left ^ right));
	}

	// The closest the two pictures come in any orientation. Only one side needs its rotations: the
	// four orientations of the right are the complete orbit, so every relative turn is covered.
	inline int phash_distance(const uint64_t left, const phash_rotations& right)
	{
		auto result = 64;

		for (const auto candidate : right)
		{
			if (phash_is_usable(candidate)) result = std::min(result, phash_distance(left, candidate));
		}

		return result;
	}

	std::string hmac_sha1(std::string_view key_bytes, std::string_view data);

	class hash_gen
	{
		const uint32_t FNV_32_PRIME = 0x01000193u;
		const uint32_t FNV_32_INIT = 0x811c9dc5u;
		uint32_t h = FNV_32_INIT;

	public:
		hash_gen()
		{
		}

		hash_gen(const std::string_view sv)
		{
			append(sv);
		}

		hash_gen& append(const std::string_view sv)
		{
			auto p = sv.begin();

			while (p < sv.end())
			{
				h ^= str::to_lower(str::pop_utf8_char(p, sv.end()));
				h *= FNV_32_PRIME;
			}

			return *this;
		}

		hash_gen& append(const int n)
		{
			h ^= n;
			h *= FNV_32_PRIME;
			return *this;
		}

		hash_gen& append(const uint32_t n)
		{
			h ^= n;
			h *= FNV_32_PRIME;
			return *this;
		}

		hash_gen& append(const uint64_t n)
		{
			h ^= static_cast<uint32_t>(n);
			h *= FNV_32_PRIME;
			h ^= static_cast<uint32_t>(n >> 32);
			h *= FNV_32_PRIME;
			return *this;
		}

		uint32_t result() const
		{
			return h;
		}
	};
}
