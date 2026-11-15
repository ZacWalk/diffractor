// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: What the agent tools (app_mcp*.cpp) build their answers with: a JSON object written as it is built, for a
// tool's structured result, and the text helpers they share.

#pragma once

#include <charconv>
#include <cmath>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

class json_writer
{
	rapidjson::StringBuffer _buffer;
	rapidjson::Writer<rapidjson::StringBuffer> _writer{_buffer};

	static rapidjson::SizeType size_of(const std::string_view text)
	{
		return static_cast<rapidjson::SizeType>(text.size());
	}

public:
	json_writer() = default;
	json_writer(const json_writer&) = delete;
	json_writer& operator=(const json_writer&) = delete;

	json_writer& object()
	{
		_writer.StartObject();
		return *this;
	}

	json_writer& end_object()
	{
		_writer.EndObject();
		return *this;
	}

	json_writer& array()
	{
		_writer.StartArray();
		return *this;
	}

	json_writer& end_array()
	{
		_writer.EndArray();
		return *this;
	}

	json_writer& key(const std::string_view k)
	{
		_writer.Key(k.data(), size_of(k));
		return *this;
	}

	json_writer& text(const std::string_view v)
	{
		_writer.String(v.data(), size_of(v));
		return *this;
	}

	// A number that is not finite has no JSON spelling, so it is written as null.
	json_writer& number(const double v)
	{
		if (std::isfinite(v)) _writer.Double(v);
		else _writer.Null();
		return *this;
	}

	json_writer& integer(const int64_t v)
	{
		_writer.Int64(v);
		return *this;
	}

	json_writer& boolean(const bool v)
	{
		_writer.Bool(v);
		return *this;
	}

	json_writer& null()
	{
		_writer.Null();
		return *this;
	}

	// A value that is JSON already.
	json_writer& raw(const std::string_view json)
	{
		_writer.RawValue(json.data(), json.size(), rapidjson::kObjectType);
		return *this;
	}

	json_writer& text(const std::string_view k, const std::string_view v) { return key(k).text(v); }
	json_writer& number(const std::string_view k, const double v) { return key(k).number(v); }
	json_writer& integer(const std::string_view k, const int64_t v) { return key(k).integer(v); }
	json_writer& boolean(const std::string_view k, const bool v) { return key(k).boolean(v); }

	std::string str() const { return {_buffer.GetString(), _buffer.GetSize()}; }
};

inline std::string join_text(const std::vector<std::string>& parts, const std::string_view separator)
{
	std::string result;

	for (const auto& part : parts)
	{
		if (!result.empty()) result += separator;
		result += part;
	}

	return result;
}

// The whole number text begins with, or zero.
inline int parse_int(const std::string_view text)
{
	auto value = 0;
	std::from_chars(text.data(), text.data() + text.size(), value);
	return value;
}
