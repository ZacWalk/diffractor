// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Window-free state and editing behavior for the segmented date field. Segment order,
// typing, stepping and roll-over are arithmetic and are tested without a window; the element that
// draws it and the platform that answers the display locale are elsewhere.

#pragma once

namespace ui
{
	enum class date_segment : uint8_t
	{
		year,
		month,
		day,
		hour,
		minute,
		second,
	};

	// The order the display locale writes a date in. Only the date part varies: every locale writes
	// a clock most-significant first.
	enum class date_field_order : uint8_t
	{
		ymd,
		dmy,
		mdy,
	};

	class date_edit_model
	{
		std::vector<date_segment> _segments;
		df::date_t _value;
		size_t _active = 0;
		// Digits typed into the active segment since it was entered. Typing runs left to right, so a
		// segment holds a partial value until it is full or the field is left.
		int _typed = 0;
		int _typed_digits = 0;

		static constexpr int days_in_month(const int year, const int month) noexcept
		{
			constexpr int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
			if (month < 1 || month > 12) return 31;
			if (month != 2) return lengths[month - 1];
			const auto leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
			return leap ? 29 : 28;
		}

		df::day_t parts() const
		{
			return _value.date();
		}

		void assign(df::day_t d)
		{
			// A month or year step can land on a day the new month does not have, and 31 April is
			// not a date. Clamping keeps the segment the user is not editing where they left it.
			d.year = std::clamp<int>(d.year, min_year, max_year);
			d.month = std::clamp<int>(d.month, 1, 12);
			d.day = std::clamp<int>(d.day, 1, days_in_month(d.year, d.month));
			_value = df::date_t(d.year, d.month, d.day, d.hour, d.minute, d.second);
		}

	public:
		// df::date_t counts from 1601, so 1600 is not a date it can hold: on Windows the conversion
		// fails and takes the whole value with it, and on Linux it wraps to a far-future one.
		static constexpr int min_year = 1601;
		static constexpr int max_year = 9999;

		date_edit_model(const date_field_order order, const bool include_time)
		{
			switch (order)
			{
			case date_field_order::ymd:
				_segments = {date_segment::year, date_segment::month, date_segment::day};
				break;
			case date_field_order::mdy:
				_segments = {date_segment::month, date_segment::day, date_segment::year};
				break;
			default:
				_segments = {date_segment::day, date_segment::month, date_segment::year};
				break;
			}

			if (include_time)
			{
				_segments.push_back(date_segment::hour);
				_segments.push_back(date_segment::minute);
				_segments.push_back(date_segment::second);
			}
		}

		const std::vector<date_segment>& segments() const noexcept { return _segments; }
		size_t active() const noexcept { return _active; }
		date_segment active_segment() const noexcept { return _segments[_active]; }
		df::date_t value() const noexcept { return _value; }

		void value(const df::date_t v)
		{
			_value = v;
			commit_typing();
		}

		void active(const size_t index)
		{
			if (index >= _segments.size() || index == _active) return;
			commit_typing();
			_active = index;
		}

		// Left and Right walk the segments and stop at the ends, so the arrow keys stay inside the
		// field. Tab asks the same question and takes a false answer as "leave the control".
		bool move(const int direction)
		{
			if (direction == 0) return false;
			const auto next = static_cast<int>(_active) + (direction > 0 ? 1 : -1);
			if (next < 0 || next >= static_cast<int>(_segments.size())) return false;
			active(static_cast<size_t>(next));
			return true;
		}

		static int digit_count(const date_segment segment) noexcept
		{
			return segment == date_segment::year ? 4 : 2;
		}

		int maximum(const date_segment segment) const
		{
			const auto d = parts();

			switch (segment)
			{
			case date_segment::year: return max_year;
			case date_segment::month: return 12;
			case date_segment::day: return days_in_month(d.year, d.month);
			case date_segment::hour: return 23;
			case date_segment::minute:
			case date_segment::second: return 59;
			}

			return 0;
		}

		static int minimum(const date_segment segment) noexcept
		{
			switch (segment)
			{
			case date_segment::year: return min_year;
			case date_segment::month:
			case date_segment::day: return 1;
			default: return 0;
			}
		}

		int field(const date_segment segment) const
		{
			const auto d = parts();

			switch (segment)
			{
			case date_segment::year: return d.year;
			case date_segment::month: return d.month;
			case date_segment::day: return d.day;
			case date_segment::hour: return d.hour;
			case date_segment::minute: return d.minute;
			case date_segment::second: return d.second;
			}

			return 0;
		}

		void field(const date_segment segment, const int v)
		{
			auto d = parts();

			switch (segment)
			{
			case date_segment::year: d.year = v;
				break;
			case date_segment::month: d.month = v;
				break;
			case date_segment::day: d.day = v;
				break;
			case date_segment::hour: d.hour = v;
				break;
			case date_segment::minute: d.minute = v;
				break;
			case date_segment::second: d.second = v;
				break;
			}

			assign(d);
		}

		// Up, Down and the wheel. One step is one unit of the active segment applied to the whole
		// date, so 23:59:59 rolls into the next day and December rolls into January of the next
		// year - which is what a clock does, and is why this is not a per-field wrap.
		void step(const int direction)
		{
			if (direction == 0) return;
			commit_typing();

			const auto delta = direction > 0 ? 1 : -1;
			auto d = parts();

			switch (active_segment())
			{
			case date_segment::year:
				d.year += delta;
				assign(d);
				return;
			case date_segment::month:
			{
				auto month = d.month + delta;
				while (month > 12)
				{
					month -= 12;
					++d.year;
				}
				while (month < 1)
				{
					month += 12;
					--d.year;
				}
				d.month = month;
				assign(d);
				return;
			}
			case date_segment::day:
				step_ticks(delta * static_cast<int64_t>(df::date_t::intervals_per_day));
				return;
			case date_segment::hour:
				step_ticks(delta * static_cast<int64_t>(df::date_t::intervals_per_second) * 3600);
				return;
			case date_segment::minute:
				step_ticks(delta * static_cast<int64_t>(df::date_t::intervals_per_second) * 60);
				return;
			case date_segment::second:
				step_ticks(delta * static_cast<int64_t>(df::date_t::intervals_per_second));
				return;
			}
		}

		// Digits type left to right and the field advances when the segment can hold no more, so a
		// date is typed straight through without reaching for a separator key.
		bool type_digit(const char c)
		{
			if (c < '0' || c > '9') return false;

			const auto segment = active_segment();
			const auto digit = c - '0';
			const auto width = digit_count(segment);

			auto pending = _typed_digits > 0 ? _typed * 10 + digit : digit;
			auto digits = _typed_digits + 1;

			// A value the segment cannot hold starts the segment again from this digit rather than
			// being refused, which is what a user retyping a wrong month means by it.
			if (pending > maximum(segment))
			{
				pending = digit;
				digits = 1;
			}

			_typed = pending;
			_typed_digits = digits;

			// Applied as it is typed, so the field always shows what it holds. A partial value below
			// the minimum - a lone 0 in a month - is shown but not yet written.
			if (pending >= minimum(segment)) field(segment, pending);

			// Nothing further could be added, so the segment is finished.
			if (digits >= width || pending * 10 > maximum(segment))
			{
				commit_typing();
				move(1);
			}

			return true;
		}

		// What the segment displays, including a partial value that is still being typed.
		std::string text(const size_t index) const
		{
			if (index >= _segments.size()) return {};

			const auto segment = _segments[index];
			const auto width = digit_count(segment);
			const auto v = index == _active && _typed_digits > 0 ? _typed : field(segment);

			auto result = std::to_string(v);
			while (result.size() < static_cast<size_t>(width)) result.insert(result.begin(), '0');
			return result;
		}

		// The month is the one segment with a name, so it is the one segment worth a drop menu:
		// everything else is a number faster to type than to pick.
		bool active_has_menu() const noexcept
		{
			return active_segment() == date_segment::month;
		}

		void commit_typing()
		{
			_typed = 0;
			_typed_digits = 0;
		}

	private:
		void step_ticks(const int64_t ticks)
		{
			const auto current = static_cast<int64_t>(_value.to_int64());
			if (ticks < 0 && current + ticks < 0) return;
			const auto stepped = df::date_t(static_cast<uint64_t>(current + ticks));
			const auto d = stepped.date();
			if (d.year < min_year || d.year > max_year) return;
			_value = stepped;
		}
	};
}
