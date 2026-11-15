// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tag management. Handles tag sets for files, including parsing, merging,
// and comparison of keyword tags used for organization.

#include "pch.h"
#include "model_tags.h"

void tag_set::make_unique()
{
	// The comparator must match the sort: a case-sensitive unique would leave both
	// "Beach" and "beach" behind, which every other tag operation treats as one tag.
	std::ranges::sort(_tags, core::path_iless());
	_tags.erase(std::ranges::unique(_tags, core::ieq()).begin(), _tags.end());
}

void tag_set::add(const tag_set& other)
{
	_tags.insert(_tags.cend(), other._tags.cbegin(), other._tags.cend());
	make_unique();
}

void tag_set::remove(const tag_set& other)
{
	const core::hash_set<std::string_view, core::ihash, core::ieq> unique(other._tags.cbegin(), other._tags.cend());
	std::erase_if(_tags, [&unique](const std::string_view tag) { return unique.contains(tag); });
}
