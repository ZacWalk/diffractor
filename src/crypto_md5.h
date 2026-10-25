// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: MD5 hash algorithm implementation. Computes 128-bit message digests
// for data integrity verification and identifier generation.

#pragma once

class md5 final : df::no_copy
{
public:
	md5(); // simple initializer
	md5(std::string_view s); // digest string, finalize

	void update(df::cspan data);

	void update(const std::string_view input)
	{
		update({(const uint8_t*)input.data(), input.size()});
	}

	void update(const std::vector<uint8_t>& input)
	{
		update({input.data(), input.size()});
	}

	void update(const char* sz)
	{
		update({(const uint8_t*)sz, str::len(sz)});
	}

	void finalize();

private:
	bool _finalized = false;

	// next, the private data:
	uint32_t state[4]{};
	uint32_t count[2]{}; // number of *bits*, mod 2^64
	uint8_t buffer[64]{}; // input buffer
	uint8_t digest[16]{};

	// last, the private methods, mostly static:
	void initialise(); // called by all constructors
	void Transform(const uint8_t* buffer); // does the real update work. Note 
	// that length is implied to be 64.

	static void Encode(uint8_t* dest, const uint32_t* src, size_t length);
	static void Decode(uint32_t* dest, df::cspan src);
};
