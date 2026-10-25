// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: MD5 hash algorithm implementation. Computes 128-bit message digests
// for data integrity verification.

#include "pch.h"
#include "crypto_md5.h"

// MD5 simple initialization method

md5::md5()
{
	initialise();
}

md5::md5(const std::string_view s)
{
	initialise(); // must be called be all constructors
	update({s.data(), s.size()});
	finalize();
}

void md5::initialise()
{
	// Nothing counted, so count=0
	count[0] = 0;
	count[1] = 0;

	// Load magic initialization constants.
	state[0] = 0x67452301;
	state[1] = 0xefcdab89;
	state[2] = 0x98badcfe;
	state[3] = 0x10325476;
}

// MD5 block update operation. Continues an MD5 message-digest
// operation, processing another message block, and updating the
// context.

void md5::update(const df::cspan data)
{
	if (_finalized)
	{
		throw app_exception("MD5::update: failed to update a finalized digest!"s);
	}

	const auto size = data.size;

	// Compute number of bytes mod 64
	size_t buffer_index = count[0] >> 3 & 0x3F;

	// Update number of bits (treat full 64-bit length to avoid truncation for inputs >= 4 GB)
	const uint64_t bit_count_low = static_cast<uint64_t>(size) << 3;
	const uint64_t bit_count_high = static_cast<uint64_t>(size) >> 29;

	if ((count[0] += static_cast<uint32_t>(bit_count_low)) < static_cast<uint32_t>(bit_count_low))
		count[1]++;

	count[1] += static_cast<uint32_t>(bit_count_high);

	const size_t buffer_space = 64 - buffer_index; // how much space is left in buffer
	size_t input_index = 0;

	// Transform as many times as possible.
	if (size >= buffer_space)
	{
		// ie. we have enough to fill the buffer
		// fill the rest of the buffer and transform
		memcpy_s(buffer + buffer_index, sizeof(buffer) - buffer_index, data.data, buffer_space);
		Transform(buffer);

		// now, transform each 64-byte piece of the input, bypassing the buffer
		for (input_index = buffer_space; input_index + 63 < size; input_index += 64)
			Transform(data.data + input_index);

		buffer_index = 0; // so we can buffer remaining
	}
	else
	{
		input_index = 0; // so we can buffer the whole input
	}

	// and here we do the buffering:
	memcpy_s(buffer + buffer_index, sizeof(buffer) - buffer_index, data.data + input_index, size - input_index);
}

// MD5 finalization. Ends an MD5 message-digest operation, writing the
// the message digest and zeroizing the context.

void md5::finalize()
{
	static constexpr uint8_t PADDING[64] = {
		0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};

	if (_finalized)
	{
		throw app_exception("MD5::finalize: Already finalized this digest!"s);
	}

	// save number of bits
	uint8_t bits[8];
	Encode(bits, count, 8);

	// Pad out to 56 mod 64.
	const auto index = count[0] >> 3 & 0x3f;
	const uint32_t padLen = index < 56 ? 56 - index : 120 - index;
	update({PADDING, padLen});

	// Append length (before padding)
	update({bits, 8});

	// Store state in digest
	Encode(digest, state, 16);

	// Zeroize sensitive information (volatile writes prevent compiler optimization)
	platform::secure_zero(buffer, sizeof(buffer));
	platform::secure_zero(state, sizeof(state));
	platform::secure_zero(count, sizeof(count));

	_finalized = true;
}

// Constants for MD5Transform routine.
// Although we could use C++ style constants, defines are actually better,
// since they let us easily evade scope clashes.

#define S11 7
#define S12 12
#define S13 17
#define S14 22
#define S21 5
#define S22 9
#define S23 14
#define S24 20
#define S31 4
#define S32 11
#define S33 16
#define S34 23
#define S41 6
#define S42 10
#define S43 15
#define S44 21

// RotateLeft rotates x left n bits.
static uint32_t RotateLeft(const uint32_t x, const uint32_t n)
{
	return x << n | x >> (32 - n);
}

// F, G, H and I are basic MD5 functions.
static uint32_t F(const uint32_t x, const uint32_t y, const uint32_t z)
{
	return (x & y) | (~x & z);
}

static uint32_t G(const uint32_t x, const uint32_t y, const uint32_t z)
{
	return (x & z) | (y & ~z);
}

static uint32_t H(const uint32_t x, const uint32_t y, const uint32_t z)
{
	return x ^ y ^ z;
}

static uint32_t I(const uint32_t x, const uint32_t y, const uint32_t z)
{
	return y ^ (x | ~z);
}

// FF, GG, HH, and II transformations for rounds 1, 2, 3, and 4.
// Rotation is separate from addition to prevent recomputation.

static void FF(uint32_t& a, const uint32_t b, const uint32_t c, const uint32_t d, const uint32_t x, const uint32_t s,
               const uint32_t ac)
{
	a += F(b, c, d) + x + ac;
	a = RotateLeft(a, s) + b;
}

static void GG(uint32_t& a, const uint32_t b, const uint32_t c, const uint32_t d, const uint32_t x, const uint32_t s,
               const uint32_t ac)
{
	a += G(b, c, d) + x + ac;
	a = RotateLeft(a, s) + b;
}

static void HH(uint32_t& a, const uint32_t b, const uint32_t c, const uint32_t d, const uint32_t x, const uint32_t s,
               const uint32_t ac)
{
	a += H(b, c, d) + x + ac;
	a = RotateLeft(a, s) + b;
}

static void II(uint32_t& a, const uint32_t b, const uint32_t c, const uint32_t d, const uint32_t x, const uint32_t s,
               const uint32_t ac)
{
	a += I(b, c, d) + x + ac;
	a = RotateLeft(a, s) + b;
}

// MD5 basic transformation. Transforms state based on block.
void md5::Transform(const uint8_t* block)
{
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
	Decode(x, {block, 64});
	df::assert_true(!_finalized); // not just a user error, since the method is private

	/* Round 1 */
	FF(a, b, c, d, x[0], S11, 0xd76aa478); /* 1 */
	FF(d, a, b, c, x[1], S12, 0xe8c7b756); /* 2 */
	FF(c, d, a, b, x[2], S13, 0x242070db); /* 3 */
	FF(b, c, d, a, x[3], S14, 0xc1bdceee); /* 4 */
	FF(a, b, c, d, x[4], S11, 0xf57c0faf); /* 5 */
	FF(d, a, b, c, x[5], S12, 0x4787c62a); /* 6 */
	FF(c, d, a, b, x[6], S13, 0xa8304613); /* 7 */
	FF(b, c, d, a, x[7], S14, 0xfd469501); /* 8 */
	FF(a, b, c, d, x[8], S11, 0x698098d8); /* 9 */
	FF(d, a, b, c, x[9], S12, 0x8b44f7af); /* 10 */
	FF(c, d, a, b, x[10], S13, 0xffff5bb1); /* 11 */
	FF(b, c, d, a, x[11], S14, 0x895cd7be); /* 12 */
	FF(a, b, c, d, x[12], S11, 0x6b901122); /* 13 */
	FF(d, a, b, c, x[13], S12, 0xfd987193); /* 14 */
	FF(c, d, a, b, x[14], S13, 0xa679438e); /* 15 */
	FF(b, c, d, a, x[15], S14, 0x49b40821); /* 16 */

	/* Round 2 */
	GG(a, b, c, d, x[1], S21, 0xf61e2562); /* 17 */
	GG(d, a, b, c, x[6], S22, 0xc040b340); /* 18 */
	GG(c, d, a, b, x[11], S23, 0x265e5a51); /* 19 */
	GG(b, c, d, a, x[0], S24, 0xe9b6c7aa); /* 20 */
	GG(a, b, c, d, x[5], S21, 0xd62f105d); /* 21 */
	GG(d, a, b, c, x[10], S22, 0x2441453); /* 22 */
	GG(c, d, a, b, x[15], S23, 0xd8a1e681); /* 23 */
	GG(b, c, d, a, x[4], S24, 0xe7d3fbc8); /* 24 */
	GG(a, b, c, d, x[9], S21, 0x21e1cde6); /* 25 */
	GG(d, a, b, c, x[14], S22, 0xc33707d6); /* 26 */
	GG(c, d, a, b, x[3], S23, 0xf4d50d87); /* 27 */
	GG(b, c, d, a, x[8], S24, 0x455a14ed); /* 28 */
	GG(a, b, c, d, x[13], S21, 0xa9e3e905); /* 29 */
	GG(d, a, b, c, x[2], S22, 0xfcefa3f8); /* 30 */
	GG(c, d, a, b, x[7], S23, 0x676f02d9); /* 31 */
	GG(b, c, d, a, x[12], S24, 0x8d2a4c8a); /* 32 */

	/* Round 3 */
	HH(a, b, c, d, x[5], S31, 0xfffa3942); /* 33 */
	HH(d, a, b, c, x[8], S32, 0x8771f681); /* 34 */
	HH(c, d, a, b, x[11], S33, 0x6d9d6122); /* 35 */
	HH(b, c, d, a, x[14], S34, 0xfde5380c); /* 36 */
	HH(a, b, c, d, x[1], S31, 0xa4beea44); /* 37 */
	HH(d, a, b, c, x[4], S32, 0x4bdecfa9); /* 38 */
	HH(c, d, a, b, x[7], S33, 0xf6bb4b60); /* 39 */
	HH(b, c, d, a, x[10], S34, 0xbebfbc70); /* 40 */
	HH(a, b, c, d, x[13], S31, 0x289b7ec6); /* 41 */
	HH(d, a, b, c, x[0], S32, 0xeaa127fa); /* 42 */
	HH(c, d, a, b, x[3], S33, 0xd4ef3085); /* 43 */
	HH(b, c, d, a, x[6], S34, 0x4881d05); /* 44 */
	HH(a, b, c, d, x[9], S31, 0xd9d4d039); /* 45 */
	HH(d, a, b, c, x[12], S32, 0xe6db99e5); /* 46 */
	HH(c, d, a, b, x[15], S33, 0x1fa27cf8); /* 47 */
	HH(b, c, d, a, x[2], S34, 0xc4ac5665); /* 48 */

	/* Round 4 */
	II(a, b, c, d, x[0], S41, 0xf4292244); /* 49 */
	II(d, a, b, c, x[7], S42, 0x432aff97); /* 50 */
	II(c, d, a, b, x[14], S43, 0xab9423a7); /* 51 */
	II(b, c, d, a, x[5], S44, 0xfc93a039); /* 52 */
	II(a, b, c, d, x[12], S41, 0x655b59c3); /* 53 */
	II(d, a, b, c, x[3], S42, 0x8f0ccc92); /* 54 */
	II(c, d, a, b, x[10], S43, 0xffeff47d); /* 55 */
	II(b, c, d, a, x[1], S44, 0x85845dd1); /* 56 */
	II(a, b, c, d, x[8], S41, 0x6fa87e4f); /* 57 */
	II(d, a, b, c, x[15], S42, 0xfe2ce6e0); /* 58 */
	II(c, d, a, b, x[6], S43, 0xa3014314); /* 59 */
	II(b, c, d, a, x[13], S44, 0x4e0811a1); /* 60 */
	II(a, b, c, d, x[4], S41, 0xf7537e82); /* 61 */
	II(d, a, b, c, x[11], S42, 0xbd3af235); /* 62 */
	II(c, d, a, b, x[2], S43, 0x2ad7d2bb); /* 63 */
	II(b, c, d, a, x[9], S44, 0xeb86d391); /* 64 */

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;

	// Zeroize sensitive information.
	platform::secure_zero(x, sizeof(x));
}

// Encodes input (uint32_t) into output (uint8_t). Assumes len is
// a multiple of 4.
void md5::Encode(uint8_t* output, const uint32_t* input, const size_t len)
{
	uint32_t i, j;

	for (i = 0, j = 0; j < len; i++, j += 4)
	{
		output[j] = static_cast<uint8_t>(input[i] & 0xff);
		output[j + 1] = static_cast<uint8_t>(input[i] >> 8 & 0xff);
		output[j + 2] = static_cast<uint8_t>(input[i] >> 16 & 0xff);
		output[j + 3] = static_cast<uint8_t>(input[i] >> 24 & 0xff);
	}
}

// Decodes input (uint8_t) into output (uint32_t). Assumes len is
// a multiple of 4.
void md5::Decode(uint32_t* output, const df::cspan input)
{
	uint32_t i, j;

	for (i = 0, j = 0; j < input.size; i++, j += 4)
	{
		output[i] = static_cast<uint32_t>(input.data[j]) |
			static_cast<uint32_t>(input.data[j + 1]) << 8 |
			static_cast<uint32_t>(input.data[j + 2]) << 16 |
			static_cast<uint32_t>(input.data[j + 3]) << 24;
	}
}
