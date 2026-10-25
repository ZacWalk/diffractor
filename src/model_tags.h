// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tag management utilities. Provides tag_set class for managing keyword
// collections with add/remove operations and string formatting.

#pragma once

#include "libdesktop/util.h"


using float_counts_type = df::hash_map<double, df::int_counter>;
using id_counts_type = df::hash_map<int, df::int_counter>;

class tag_set
{
	std::vector<std::string> _tags;

public:
	tag_set() noexcept = default;
	tag_set(const tag_set&) = default;
	tag_set& operator=(const tag_set&) = default;
	tag_set(tag_set&&) noexcept = default;
	tag_set& operator=(tag_set&&) noexcept = default;

	tag_set(const std::string_view tags)
	{
		append(str::split(tags, true));
	}

	tag_set(const std::vector<std::string_view>& tags)
	{
		append(tags);
	}

	tag_set(std::vector<std::string> tags) : _tags(std::move(tags))
	{
	}

	friend bool operator==(const tag_set& lhs, const tag_set& rhs)
	{
		return lhs._tags == rhs._tags;
	}

	friend bool operator!=(const tag_set& lhs, const tag_set& rhs)
	{
		return !(lhs == rhs);
	}

	void append(const std::vector<std::string_view>& tags)
	{
		_tags.insert(_tags.end(), tags.begin(), tags.end());
		std::ranges::sort(_tags, df::iless());
	}

	size_t size() const
	{
		return _tags.size();
	}

	bool is_empty() const
	{
		return _tags.empty();
	}

	void clear()
	{
		_tags.clear();
	}

	std::string to_string(const std::string_view join = " ", const bool quote = true) const
	{
		return str::combine(_tags, join, quote);
	}

	const std::string& operator[](const int i) const
	{
		return _tags[i];
	}

	void add_one(const std::string_view tag)
	{
		_tags.emplace_back(tag);
	};

	void make_unique();
	void add(const tag_set& other);
	void remove(const tag_set& other);
};
