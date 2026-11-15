// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: SHA-1 and SHA-256 hash algorithm implementations. Computes secure
// cryptographic digests for data integrity and authentication.

#pragma once

namespace crypto
{
	class sha1
	{
	public:
		sha1();
		void update(core::cspan cs);
		void final(uint8_t* digest);

		static constexpr uint32_t DIGEST_SIZE = 5 * 4;

	private:
		static constexpr uint32_t DIGEST_INTS = 5; /* number of 32bit integers per SHA1 digest */
		static constexpr uint32_t BLOCK_INTS = 16; /* number of 32bit integers per SHA1 block */
		static constexpr uint32_t BLOCK_BYTES = BLOCK_INTS * 4;

		std::vector<uint8_t> buffer;
		uint64_t transforms;
		uint32_t digest[DIGEST_INTS];

		void reset();
		void transform(uint32_t block[DIGEST_INTS]);

		static void buffer_to_block(const uint8_t* buf, uint32_t block[BLOCK_INTS]);
	};

	class sha256
	{
	public:
		sha256();
		void update(core::cspan cs);
		void final(uint8_t* digest);

		static constexpr uint32_t DIGEST_SIZE = 256 / 8;

	private:
		const static uint32_t sha256_k[];
		static constexpr uint32_t SHA224_256_BLOCK_SIZE = 512 / 8;

		void transform(const uint8_t* message, size_t block_nb);
		void reset();

		size_t m_tot_len = 0;
		size_t m_len = 0;
		uint8_t m_block[2 * SHA224_256_BLOCK_SIZE];
		uint32_t m_h[8];
	};

	inline std::string to_sha1(const std::string_view input)
	{
		sha1 checksum;
		checksum.update({std::bit_cast<const uint8_t*>(input.data()), input.size()});

		uint8_t digest[sha1::DIGEST_SIZE];
		checksum.final(digest);

		return core::to_hex(digest, sha1::DIGEST_SIZE, false);
	}

	inline std::string to_sha256(const std::string_view input)
	{
		sha256 checksum;
		checksum.update({std::bit_cast<const uint8_t*>(input.data()), input.size()});

		uint8_t digest[sha256::DIGEST_SIZE];
		checksum.final(digest);

		return core::to_hex(digest, sha256::DIGEST_SIZE, false);
	}
}
