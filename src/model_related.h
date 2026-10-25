// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Bounded nearest-match collection for related-item searches. Holds only the closest
// matches on each relation axis so a related search answers with a useful, bounded set.

#pragma once

namespace df
{
	// Relation kinds a related search reports. This order is both the priority used when an item
	// qualifies on more than one axis and the order the groups are presented in.
	enum class related_axis : uint8_t
	{
		duplicate,
		album,
		series,
		time,
		location,
	};

	inline constexpr size_t related_axis_count = static_cast<size_t>(related_axis::location) + 1;

	// The cap is the tolerance: a related search answers with the closest matches rather than with
	// everything that qualifies. The windows stop a sparse collection from filling a cap with items
	// that are only nearest by default.
	inline constexpr size_t related_axis_limit = 64;
	inline constexpr int64_t related_time_window_seconds = 24LL * 60LL * 60LL;
	inline constexpr double related_location_window_km = 25.0;

	struct related_match
	{
		related_axis axis = related_axis::duplicate;
		// Distance from the item the search started at, in units chosen by the axis. Lower is closer.
		int64_t distance = 0;
	};

	// Keeps the lowest-distance payloads offered to it, dropping the worst once full. Ties break on
	// path so the surviving set never depends on the order the index happened to be walked in.
	template <typename T>
	class bounded_best
	{
	public:
		struct entry
		{
			int64_t distance = 0;
			file_path path;
			T value = {};
		};

	private:
		std::vector<entry> _heap;
		size_t _limit = related_axis_limit;

		// Orders closest first, which makes the heap root under this comparison the worst entry held.
		static bool closer(const entry& l, const entry& r)
		{
			if (l.distance != r.distance) return l.distance < r.distance;
			return l.path.icmp(r.path) < 0;
		}

	public:
		void limit(const size_t v)
		{
			_limit = v;
		}

		size_t size() const
		{
			return _heap.size();
		}

		bool empty() const
		{
			return _heap.empty();
		}

		void offer(const int64_t distance, const file_path path, T value)
		{
			if (_limit == 0) return;

			entry e{distance, path, std::move(value)};

			if (_heap.size() < _limit)
			{
				_heap.emplace_back(std::move(e));
				std::push_heap(_heap.begin(), _heap.end(), closer);
				return;
			}

			if (!closer(e, _heap.front())) return;

			std::pop_heap(_heap.begin(), _heap.end(), closer);
			_heap.back() = std::move(e);
			std::push_heap(_heap.begin(), _heap.end(), closer);
		}

		// Closest first. Leaves the collector empty.
		std::vector<entry> take()
		{
			std::ranges::sort(_heap, closer);
			auto result = std::move(_heap);
			_heap.clear();
			return result;
		}
	};

	template <typename T>
	class related_collector
	{
		bounded_best<T> _axes[related_axis_count];

	public:
		void limit(const size_t v)
		{
			for (auto& axis : _axes) axis.limit(v);
		}

		void offer(const related_match& match, const file_path path, T value)
		{
			_axes[static_cast<size_t>(match.axis)].offer(match.distance, path, std::move(value));
		}

		size_t size() const
		{
			size_t result = 0;
			for (const auto& axis : _axes) result += axis.size();
			return result;
		}

		size_t size(const related_axis axis) const
		{
			return _axes[static_cast<size_t>(axis)].size();
		}

		// Axis order, then closest first inside each axis.
		template <typename F>
		void drain(F&& fn)
		{
			for (auto& axis : _axes)
			{
				for (auto& e : axis.take())
				{
					fn(e.path, std::move(e.value));
				}
			}
		}
	};
}
