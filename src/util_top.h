// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Top-N tracking container. Maintains a sorted list of the
// N largest or most frequent items for statistical summaries.

#pragma once

template <typename val_t, typename sorter_t> std::vector<val_t> top_vec(std::vector<std::pair<int, val_t>> all, int max)
{
	if (max <= 0 || all.empty()) return {};

	const auto result_count = std::min(static_cast<size_t>(max), all.size());
	const auto by_count_then_value = [](const auto& l, const auto& r)
	{
		if (l.first != r.first) return l.first > r.first;
		return sorter_t{}(l.second, r.second);
	};

	std::ranges::partial_sort(all, all.begin() + static_cast<ptrdiff_t>(result_count), by_count_then_value);

	std::vector<val_t> results;
	results.reserve(result_count);

	for (auto i = 0_z; i < result_count; ++i)
	{
		results.emplace_back(all[i].second);
	}

	std::ranges::sort(results, sorter_t());

	return results;
}

inline std::vector<std::string_view> top_map(const core::string_counts& counts, const int limit)
{
	std::vector<std::pair<int, std::string_view>> all;
	all.reserve(counts.size());

	for (const auto& i : counts)
	{
		all.emplace_back(i.second, i.first);
	}

	return top_vec<std::string_view, core::iless>(std::move(all), limit);
}
