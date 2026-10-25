// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Cryptographic utilities including HMAC-SHA1, CRC32 checksums, and FNV-1a hashing for
// file integrity and authentication.

#include "pch.h"

#include "libdesktop/util_base64.h"
#include "libdesktop/util_simd.h"
#include "crypto.h"
#include "crypto_sha.h"

///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////

static constexpr int SHA1_DIGEST_LENGTH = 20;
static constexpr int SHA1_BLOCK_SIZE = 64;

std::string crypto::hmac_sha1(const std::string_view key, const std::string_view data)
{
	sha1 sha1;

	uint8_t hash[SHA1_BLOCK_SIZE] = {};

	if (key.size() > SHA1_BLOCK_SIZE)
	{
		sha1.update({std::bit_cast<const uint8_t*>(key.data()), key.size()});
		sha1.final(hash);
	}
	else
	{
		memcpy_s(hash, SHA1_BLOCK_SIZE, key.data(), key.size());
	}

	uint8_t ipad[SHA1_BLOCK_SIZE];
	uint8_t opad[SHA1_BLOCK_SIZE];

	for (int i = 0; i < SHA1_BLOCK_SIZE; i++)
	{
		ipad[i] = 0x36 ^ hash[i];
		opad[i] = 0x5c ^ hash[i];
	}

	sha1.update({ipad, SHA1_BLOCK_SIZE});
	sha1.update({std::bit_cast<const uint8_t*>(data.data()), data.size()});
	sha1.final(hash);

	sha1.update({opad, SHA1_BLOCK_SIZE});
	sha1.update({hash, SHA1_DIGEST_LENGTH});
	sha1.final(hash);

	return base64_encode(hash, SHA1_DIGEST_LENGTH);
}

///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////
// Perceptual hash

namespace
{
	// Side of the low-frequency DCT block kept as the hash. 8x8 is 64 coefficients, one of which is
	// the DC term the median threshold discards, so the hash is 63 meaningful bits in a 64-bit word.
	constexpr size_t phash_block = 8;

	// Below this spread across the kept coefficients the picture has no structure to identify it,
	// only noise around a flat field, and any hash of it collides with every other flat field.
	constexpr double phash_min_deviation = 1.0;

	// The DCT-II basis is separable and the input extent is fixed, so the cosine table is built once
	// rather than per image: this is called on candidate pairs, not on the whole collection.
	const auto& phash_basis()
	{
		static const auto table = []
		{
			auto result = std::make_unique<std::array<double, crypto::phash_extent * phash_block>>();

			for (size_t u = 0; u < phash_block; ++u)
			{
				for (size_t x = 0; x < crypto::phash_extent; ++x)
				{
					(*result)[u * crypto::phash_extent + x] =
						std::cos((2.0 * x + 1.0) * u * M_PI / (2.0 * crypto::phash_extent));
				}
			}

			return result;
		}();

		return *table;
	}
}

uint64_t crypto::perceptual_hash(const uint8_t* gray, const size_t len)
{
	if (gray == nullptr || len < phash_pixels)
	{
		return 0;
	}

	const auto& basis = phash_basis();

	// Rows first, then columns: separating the 2D transform turns 32*32*8*8 products into 2*32*32*8.
	// Held transposed (u-major) so the column pass reads both operands contiguously.
	std::array<double, phash_block * phash_extent> rows{};
	std::array<double, phash_extent> row{};

	for (size_t y = 0; y < phash_extent; ++y)
	{
		// Widened once per row, not once per tap: a byte-to-double conversion inside the dot product
		// is what stops a compiler vectorising it, and each row is read once per output coefficient.
		for (size_t x = 0; x < phash_extent; ++x)
		{
			row[x] = gray[y * phash_extent + x];
		}

		for (size_t u = 0; u < phash_block; ++u)
		{
			double sum = 0.0;
			for (size_t x = 0; x < phash_extent; ++x)
			{
				sum += row[x] * basis[u * phash_extent + x];
			}
			rows[u * phash_extent + y] = sum;
		}
	}

	std::array<double, phash_block * phash_block> coefficients{};

	for (size_t u = 0; u < phash_block; ++u)
	{
		for (size_t v = 0; v < phash_block; ++v)
		{
			double sum = 0.0;
			for (size_t y = 0; y < phash_extent; ++y)
			{
				sum += rows[v * phash_extent + y] * basis[u * phash_extent + y];
			}
			coefficients[u * phash_block + v] = sum;
		}
	}

	// The DC term carries overall brightness, which is exactly what a re-encode or an exposure tweak
	// changes, so it is excluded from the threshold rather than allowed to dominate it.
	std::array<double, phash_block * phash_block - 1> ranked{};
	std::copy(coefficients.begin() + 1, coefficients.end(), ranked.begin());

	const auto middle = ranked.begin() + ranked.size() / 2;
	std::nth_element(ranked.begin(), middle, ranked.end());
	const auto median = *middle;

	double deviation = 0.0;
	for (size_t i = 1; i < coefficients.size(); ++i)
	{
		deviation += std::abs(coefficients[i] - median);
	}

	if ((deviation / (coefficients.size() - 1)) < phash_min_deviation)
	{
		return 0;
	}

	uint64_t result = 0;

	for (size_t i = 1; i < coefficients.size(); ++i)
	{
		if (coefficients[i] > median)
		{
			result |= 1ull << i;
		}
	}

	// A structured image can still land on an all-zero pattern. Bit 0 is reserved, so 2 is the
	// smallest value that reads as a real hash.
	return result == 0 ? 2ull : result;
}

namespace
{
	// A quarter turn clockwise: the value at row r, column c moves to row c, column N-1-r.
	void rotate_quarter_turn(std::array<uint8_t, crypto::phash_pixels>& grid)
	{
		constexpr auto n = crypto::phash_extent;
		std::array<uint8_t, crypto::phash_pixels> rotated{};

		for (size_t r = 0; r < n; ++r)
		{
			for (size_t c = 0; c < n; ++c)
			{
				rotated[c * n + (n - 1 - r)] = grid[r * n + c];
			}
		}

		grid = rotated;
	}
}

crypto::phash_rotations crypto::perceptual_hash_rotations(const uint8_t* gray, const size_t len)
{
	phash_rotations result{};

	if (gray == nullptr || len < phash_pixels)
	{
		return result;
	}

	std::array<uint8_t, phash_pixels> grid{};
	std::copy_n(gray, phash_pixels, grid.begin());

	for (auto& hash : result)
	{
		hash = perceptual_hash(grid.data(), grid.size());
		rotate_quarter_turn(grid);
	}

	// The detail test is applied per orientation, so a picture near the floor could speak in one
	// turn and decline in another. Answering only when every orientation agrees keeps the result
	// from depending on which way round the file happened to be saved.
	if (std::ranges::any_of(result, [](const uint64_t h) { return !phash_is_usable(h); }))
	{
		result.fill(0);
	}

	return result;
}

