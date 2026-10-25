// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: SHA-1 and SHA-256 hash algorithm implementations. Computes secure
// message digests for authentication and data integrity.

#include "pch.h"
#include "crypto_sha.h"

using namespace crypto;

// Help macros 
#define SHA1_ROL(value, bits) (((value) << (bits)) | (((value) & 0xffffffff) >> (32 - (bits))))
#define SHA1_BLK(i) (block[i&15] = SHA1_ROL(block[(i+13)&15] ^ block[(i+8)&15] ^ block[(i+2)&15] ^ block[i&15],1))

// (R0+R1), R2, R3, R4 are the different operations used in SHA1 
#define SHA1_R0(v,w,x,y,z,i) z += ((w&(x^y))^y)     + block[i]    + 0x5a827999 + SHA1_ROL(v,5); w=SHA1_ROL(w,30);
#define SHA1_R1(v,w,x,y,z,i) z += ((w&(x^y))^y)     + SHA1_BLK(i) + 0x5a827999 + SHA1_ROL(v,5); w=SHA1_ROL(w,30);
#define SHA1_R2(v,w,x,y,z,i) z += (w^x^y)           + SHA1_BLK(i) + 0x6ed9eba1 + SHA1_ROL(v,5); w=SHA1_ROL(w,30);
#define SHA1_R3(v,w,x,y,z,i) z += (((w|x)&y)|(w&x)) + SHA1_BLK(i) + 0x8f1bbcdc + SHA1_ROL(v,5); w=SHA1_ROL(w,30);
#define SHA1_R4(v,w,x,y,z,i) z += (w^x^y)           + SHA1_BLK(i) + 0xca62c1d6 + SHA1_ROL(v,5); w=SHA1_ROL(w,30);

sha1::sha1()
{
	reset();
}


void sha1::update(const df::cspan input)
{
	const uint8_t* data = input.data;
	size_t remaining = input.size;

	// Fill existing buffer first
	if (buffer.size() > 0)
	{
		const size_t fill = std::min(remaining, BLOCK_BYTES - buffer.size());
		buffer.insert(buffer.end(), data, data + fill);
		data += fill;
		remaining -= fill;

		if (buffer.size() == BLOCK_BYTES)
		{
			uint32_t block[BLOCK_INTS];
			buffer_to_block(buffer.data(), block);
			transform(block);
			buffer.clear();
		}
	}

	// Process full blocks directly from input
	while (remaining >= BLOCK_BYTES)
	{
		uint32_t block[BLOCK_INTS];
		buffer_to_block(data, block);
		transform(block);
		data += BLOCK_BYTES;
		remaining -= BLOCK_BYTES;
	}

	// Buffer remaining bytes
	if (remaining > 0)
	{
		buffer.insert(buffer.end(), data, data + remaining);
	}
}

void sha1::final(uint8_t* digest_result)
{
	// Total number of hashed bits 
	const uint64_t total_bits = (transforms * BLOCK_BYTES + buffer.size()) * 8;

	// Padding 
	buffer.emplace_back(0x80);

	const auto orig_size = buffer.size();
	while (buffer.size() < BLOCK_BYTES)
	{
		buffer.emplace_back(0x00);
	}

	uint32_t block[BLOCK_INTS];
	buffer_to_block(buffer.data(), block);

	if (orig_size > BLOCK_BYTES - 8)
	{
		transform(block);
		for (uint32_t i = 0; i < BLOCK_INTS - 2; i++)
		{
			block[i] = 0;
		}
	}

	// Append total_bits, split this uint64 into two uint32 
	block[BLOCK_INTS - 1] = static_cast<uint32_t>(total_bits);
	block[BLOCK_INTS - 2] = static_cast<uint32_t>(total_bits >> 32);
	transform(block);

	for (unsigned int& i : digest)
	{
		i = df::byteswap32(i);
	}

	memcpy_s(digest_result, DIGEST_SIZE, digest, DIGEST_SIZE);

	// Reset for next run 
	reset();
}

void sha1::reset()
{
	// SHA1 initialization constants 
	digest[0] = 0x67452301;
	digest[1] = 0xefcdab89;
	digest[2] = 0x98badcfe;
	digest[3] = 0x10325476;
	digest[4] = 0xc3d2e1f0;

	// Reset counters 
	transforms = 0;
	buffer.clear();
}

void sha1::transform(uint32_t block[BLOCK_INTS])
{
	// Copy digest[] to working vars 
	auto a = digest[0];
	auto b = digest[1];
	auto c = digest[2];
	auto d = digest[3];
	auto e = digest[4];


	// 4 rounds of 20 operations each. Loop unrolled. 
	SHA1_R0(a, b, c, d, e, 0);
	SHA1_R0(e, a, b, c, d, 1);
	SHA1_R0(d, e, a, b, c, 2);
	SHA1_R0(c, d, e, a, b, 3);
	SHA1_R0(b, c, d, e, a, 4);
	SHA1_R0(a, b, c, d, e, 5);
	SHA1_R0(e, a, b, c, d, 6);
	SHA1_R0(d, e, a, b, c, 7);
	SHA1_R0(c, d, e, a, b, 8);
	SHA1_R0(b, c, d, e, a, 9);
	SHA1_R0(a, b, c, d, e, 10);
	SHA1_R0(e, a, b, c, d, 11);
	SHA1_R0(d, e, a, b, c, 12);
	SHA1_R0(c, d, e, a, b, 13);
	SHA1_R0(b, c, d, e, a, 14);
	SHA1_R0(a, b, c, d, e, 15);
	SHA1_R1(e, a, b, c, d, 16);
	SHA1_R1(d, e, a, b, c, 17);
	SHA1_R1(c, d, e, a, b, 18);
	SHA1_R1(b, c, d, e, a, 19);
	SHA1_R2(a, b, c, d, e, 20);
	SHA1_R2(e, a, b, c, d, 21);
	SHA1_R2(d, e, a, b, c, 22);
	SHA1_R2(c, d, e, a, b, 23);
	SHA1_R2(b, c, d, e, a, 24);
	SHA1_R2(a, b, c, d, e, 25);
	SHA1_R2(e, a, b, c, d, 26);
	SHA1_R2(d, e, a, b, c, 27);
	SHA1_R2(c, d, e, a, b, 28);
	SHA1_R2(b, c, d, e, a, 29);
	SHA1_R2(a, b, c, d, e, 30);
	SHA1_R2(e, a, b, c, d, 31);
	SHA1_R2(d, e, a, b, c, 32);
	SHA1_R2(c, d, e, a, b, 33);
	SHA1_R2(b, c, d, e, a, 34);
	SHA1_R2(a, b, c, d, e, 35);
	SHA1_R2(e, a, b, c, d, 36);
	SHA1_R2(d, e, a, b, c, 37);
	SHA1_R2(c, d, e, a, b, 38);
	SHA1_R2(b, c, d, e, a, 39);
	SHA1_R3(a, b, c, d, e, 40);
	SHA1_R3(e, a, b, c, d, 41);
	SHA1_R3(d, e, a, b, c, 42);
	SHA1_R3(c, d, e, a, b, 43);
	SHA1_R3(b, c, d, e, a, 44);
	SHA1_R3(a, b, c, d, e, 45);
	SHA1_R3(e, a, b, c, d, 46);
	SHA1_R3(d, e, a, b, c, 47);
	SHA1_R3(c, d, e, a, b, 48);
	SHA1_R3(b, c, d, e, a, 49);
	SHA1_R3(a, b, c, d, e, 50);
	SHA1_R3(e, a, b, c, d, 51);
	SHA1_R3(d, e, a, b, c, 52);
	SHA1_R3(c, d, e, a, b, 53);
	SHA1_R3(b, c, d, e, a, 54);
	SHA1_R3(a, b, c, d, e, 55);
	SHA1_R3(e, a, b, c, d, 56);
	SHA1_R3(d, e, a, b, c, 57);
	SHA1_R3(c, d, e, a, b, 58);
	SHA1_R3(b, c, d, e, a, 59);
	SHA1_R4(a, b, c, d, e, 60);
	SHA1_R4(e, a, b, c, d, 61);
	SHA1_R4(d, e, a, b, c, 62);
	SHA1_R4(c, d, e, a, b, 63);
	SHA1_R4(b, c, d, e, a, 64);
	SHA1_R4(a, b, c, d, e, 65);
	SHA1_R4(e, a, b, c, d, 66);
	SHA1_R4(d, e, a, b, c, 67);
	SHA1_R4(c, d, e, a, b, 68);
	SHA1_R4(b, c, d, e, a, 69);
	SHA1_R4(a, b, c, d, e, 70);
	SHA1_R4(e, a, b, c, d, 71);
	SHA1_R4(d, e, a, b, c, 72);
	SHA1_R4(c, d, e, a, b, 73);
	SHA1_R4(b, c, d, e, a, 74);
	SHA1_R4(a, b, c, d, e, 75);
	SHA1_R4(e, a, b, c, d, 76);
	SHA1_R4(d, e, a, b, c, 77);
	SHA1_R4(c, d, e, a, b, 78);
	SHA1_R4(b, c, d, e, a, 79);

	// Add the working vars back into digest[] 
	digest[0] += a;
	digest[1] += b;
	digest[2] += c;
	digest[3] += d;
	digest[4] += e;

	// Count the number of transformations 
	transforms++;
}


void sha1::buffer_to_block(const uint8_t* buf, uint32_t block[BLOCK_INTS])
{
	// Convert the byte buffer to a uint32 array (MSB) 
	for (uint32_t i = 0; i < BLOCK_INTS; i++)
	{
		block[i] = (buf[4 * i + 3] & 0xff)
			| (buf[4 * i + 2] & 0xff) << 8
			| (buf[4 * i + 1] & 0xff) << 16
			| (buf[4 * i + 0] & 0xff) << 24;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////

#define SHA2_SHFR(x, n)    (x >> n)
#define SHA2_ROTR(x, n)   ((x >> n) | (x << ((sizeof(x) << 3) - n)))
#define SHA2_ROTL(x, n)   ((x << n) | (x >> ((sizeof(x) << 3) - n)))
#define SHA2_CH(x, y, z)  ((x & y) ^ (~x & z))
#define SHA2_MAJ(x, y, z) ((x & y) ^ (x & z) ^ (y & z))
#define SHA256_F1(x) (SHA2_ROTR(x,  2) ^ SHA2_ROTR(x, 13) ^ SHA2_ROTR(x, 22))
#define SHA256_F2(x) (SHA2_ROTR(x,  6) ^ SHA2_ROTR(x, 11) ^ SHA2_ROTR(x, 25))
#define SHA256_F3(x) (SHA2_ROTR(x,  7) ^ SHA2_ROTR(x, 18) ^ SHA2_SHFR(x,  3))
#define SHA256_F4(x) (SHA2_ROTR(x, 17) ^ SHA2_ROTR(x, 19) ^ SHA2_SHFR(x, 10))
#define SHA2_UNPACK32(x, str)                 \
{                                             \
    *((str) + 3) = (uint8_t) ((x)      );       \
    *((str) + 2) = (uint8_t) ((x) >>  8);       \
    *((str) + 1) = (uint8_t) ((x) >> 16);       \
    *((str) + 0) = (uint8_t) ((x) >> 24);       \
}
#define SHA2_PACK32(str, x)                   \
{                                             \
    *(x) =   ((uint32_t) *((str) + 3)      )    \
           | ((uint32_t) *((str) + 2) <<  8)    \
           | ((uint32_t) *((str) + 1) << 16)    \
           | ((uint32_t) *((str) + 0) << 24);   \
}

const uint32_t sha256::sha256_k[64] = //UL = uint32
{
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

void sha256::transform(const uint8_t* message, const size_t block_nb)
{
	uint32_t w[64];
	uint32_t wv[8];

	int j;
	for (auto i = 0_z; i < block_nb; i++)
	{
		const uint8_t* sub_block = message + (i << 6);
		for (j = 0; j < 16; j++)
		{
			SHA2_PACK32(&sub_block[j << 2], &w[j]);
		}
		for (j = 16; j < 64; j++)
		{
			w[j] = SHA256_F4(w[j - 2]) + w[j - 7] + SHA256_F3(w[j - 15]) + w[j - 16];
		}
		for (j = 0; j < 8; j++)
		{
			wv[j] = m_h[j];
		}
		for (j = 0; j < 64; j++)
		{
			const uint32_t t1 = wv[7] + SHA256_F2(wv[4]) + SHA2_CH(wv[4], wv[5], wv[6])
				+ sha256_k[j] + w[j];
			const uint32_t t2 = SHA256_F1(wv[0]) + SHA2_MAJ(wv[0], wv[1], wv[2]);
			wv[7] = wv[6];
			wv[6] = wv[5];
			wv[5] = wv[4];
			wv[4] = wv[3] + t1;
			wv[3] = wv[2];
			wv[2] = wv[1];
			wv[1] = wv[0];
			wv[0] = t1 + t2;
		}
		for (j = 0; j < 8; j++)
		{
			m_h[j] += wv[j];
		}
	}
}

sha256::sha256()
{
	reset();
}

void sha256::reset()
{
	m_h[0] = 0x6a09e667;
	m_h[1] = 0xbb67ae85;
	m_h[2] = 0x3c6ef372;
	m_h[3] = 0xa54ff53a;
	m_h[4] = 0x510e527f;
	m_h[5] = 0x9b05688c;
	m_h[6] = 0x1f83d9ab;
	m_h[7] = 0x5be0cd19;
	m_tot_len = 0;
	m_len = 0;
}

void sha256::update(const df::cspan cs)
{
	const auto len = cs.size;
	const auto* input = cs.data;
	const auto tmp_len = SHA224_256_BLOCK_SIZE - m_len;
	auto rem_len = len < tmp_len ? len : tmp_len;

	memcpy(&m_block[m_len], input, rem_len);

	if (m_len + len < SHA224_256_BLOCK_SIZE)
	{
		m_len += len;
		return;
	}

	const auto new_len = len - rem_len;
	const auto block_nb = new_len / SHA224_256_BLOCK_SIZE;
	const auto* const shifted_message = input + rem_len;

	transform(m_block, 1);
	transform(shifted_message, block_nb);

	rem_len = new_len % SHA224_256_BLOCK_SIZE;
	memcpy(m_block, &shifted_message[block_nb << 6], rem_len);

	m_len = rem_len;
	m_tot_len += (block_nb + 1) << 6;
}

void sha256::final(uint8_t* digest)
{
	const auto block_nb = 1 + (SHA224_256_BLOCK_SIZE - 9 < m_len % SHA224_256_BLOCK_SIZE);
	// The padded message length is a 64-bit bit-count; size_t is only 32 bits on a 32-bit build.
	const uint64_t len_b = (static_cast<uint64_t>(m_tot_len) + m_len) << 3;
	const auto pm_len = block_nb << 6;

	memset(m_block + m_len, 0, pm_len - m_len);
	m_block[m_len] = 0x80;

	SHA2_UNPACK32(static_cast<uint32_t>(len_b >> 32), m_block + pm_len - 8);
	SHA2_UNPACK32(static_cast<uint32_t>(len_b), m_block + pm_len - 4);
	transform(m_block, block_nb);

	memset(digest, 0, DIGEST_SIZE);

	for (auto i = 0; i < 8; i++)
	{
		SHA2_UNPACK32(m_h[i], &digest[i << 2]);
	}

	reset();
}
