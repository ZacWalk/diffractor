// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The date pack. Holds every date a media file carries, grouped by value with the tags
// each was read from, and resolves those to the three dates a user sees. docs/metadata.md#dates
// owns which tag feeds which date and where each ranks.

#pragma once

namespace prop
{
	// One bit per tag a date can be read from. These values are persisted inside the date pack, so
	// a bit's meaning is fixed once assigned: retire a source by leaving its bit unused, never by
	// giving it to something else. 64 bits rather than 32 because widening it later would change the
	// stored layout, and changing the stored layout costs every user a re-index.
	enum class date_source : uint64_t
	{
		none = 0,

		exif_original = 1ull << 0,
		xmp_exif_original = 1ull << 1,
		iptc_created = 1ull << 2,
		id3_original_release = 1ull << 3,
		photoshop_created = 1ull << 4,
		shell_date_taken = 1ull << 5,

		exif_digitized = 1ull << 6,
		xmp_exif_digitized = 1ull << 7,
		iptc_digital_created = 1ull << 8,
		xmp_create = 1ull << 9,
		container_created = 1ull << 10,
		embedded_created = 1ull << 11,
		rip_date = 1ull << 12,
		file_created = 1ull << 13,

		exif_datetime = 1ull << 14,
		xmp_modify = 1ull << 15,
		file_modified = 1ull << 16,

		gps_stamp = 1ull << 17,

		// Synthesised from a database row written before the date pack existed. Ranked below every
		// real source of the same date so the background re-scan always replaces it.
		legacy_original = 1ull << 18,
		legacy_created = 1ull << 19,
		legacy_modified = 1ull << 20,
	};

	constexpr date_source operator|(const date_source a, const date_source b)
	{
		return static_cast<date_source>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
	}

	constexpr date_source operator&(const date_source a, const date_source b)
	{
		return static_cast<date_source>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
	}

	constexpr bool operator&&(const date_source a, const date_source b)
	{
		return (static_cast<uint64_t>(a) & static_cast<uint64_t>(b)) != 0;
	}

	// The three dates a user holds, plus the one kind of tag that answers none of them. A reference
	// source is read to calibrate another date and is never displayed or resolved.
	enum class date_concept : uint8_t
	{
		original,
		created,
		modified,
		reference,
	};

	struct date_source_info
	{
		date_source source;
		date_concept kind;
		uint8_t authority; // 1 is highest
	};

	// Ordered by kind, then by authority within it. `resolve` walks this in order and takes the
	// first source the file supplied, so the order here *is* the precedence rule.
	inline constexpr date_source_info date_sources[] = {
		{date_source::exif_original, date_concept::original, 1},
		{date_source::xmp_exif_original, date_concept::original, 2},
		{date_source::iptc_created, date_concept::original, 3},
		{date_source::id3_original_release, date_concept::original, 4},
		// Nominally a mirror of DateTimeOriginal, but the one an image editor rewrites on save.
		// Ranked last so it serves files carrying nothing else without moving a photograph to the
		// day it was retouched.
		{date_source::photoshop_created, date_concept::original, 5},
		// The Windows shell's reading of DateTimeOriginal, used only for a cloud placeholder that
		// cannot be opened without downloading it. Nothing better is available when it is.
		{date_source::shell_date_taken, date_concept::original, 6},
		{date_source::legacy_original, date_concept::original, 7},

		{date_source::exif_digitized, date_concept::created, 1},
		{date_source::xmp_exif_digitized, date_concept::created, 2},
		{date_source::iptc_digital_created, date_concept::created, 3},
		{date_source::xmp_create, date_concept::created, 4},
		{date_source::container_created, date_concept::created, 5},
		{date_source::embedded_created, date_concept::created, 6},
		{date_source::rip_date, date_concept::created, 7},
		{date_source::file_created, date_concept::created, 8},
		{date_source::legacy_created, date_concept::created, 9},

		// A concept is answered by whatever is authoritative for the thing it describes. Original and
		// Created describe the content, so metadata outranks the filesystem. Modified describes the
		// file, and the filesystem stamp is the only thing that tracks an actual edit: a metadata
		// modify tag records when some tool last wrote the metadata, which a copy preserves and an
		// editor that does not maintain the tag never touches. So the stamp leads here, and the two
		// tags answer only where there is no usable one - a remote item, or an unhydrated placeholder.
		// The asymmetry with Created is the point: copying destroys the creation stamp and preserves
		// the modification stamp.
		{date_source::file_modified, date_concept::modified, 1},
		// EXIF 0x0132 is defined as the time the file last changed, and lives in IFD0 which is
		// walked before the Exif SubIFD. Reading it as a creation date is why an edited photograph
		// grouped under the day of its edit.
		{date_source::exif_datetime, date_concept::modified, 2},
		{date_source::xmp_modify, date_concept::modified, 3},
		{date_source::legacy_modified, date_concept::modified, 4},

		{date_source::gps_stamp, date_concept::reference, 1},
	};

#pragma pack(push, 1)

	// Every date read from one file, grouped by value: sources that agree cost one stored value and
	// a bit each. A value is either a naive reading or a UTC instant, and which it is travels with
	// it, so the pack resolves to a wall clock without the scanning machine's timezone ever being
	// written into the index. Groups are held in ascending order so an unchanged file always
	// produces an identical pack.
	class date_pack
	{
	public:
		static constexpr int max_groups = 4;

		// A naive reading: EXIF states 18:11:56 with no zone, so it is already the wall clock.
		static constexpr int16_t no_offset = INT16_MIN;
		// A UTC instant: a container creation time or a filesystem stamp, converted when the pack
		// resolves rather than when it is filled.
		static constexpr int16_t utc_instant = INT16_MIN + 1;

	private:
		df::date_t _values[max_groups] = {};
		uint64_t _sources[max_groups] = {};
		int16_t _offsets[max_groups] = {};
		uint64_t _overflow = 0;
		uint8_t _count = 0;

		static constexpr uint64_t whole_seconds(const df::date_t d)
		{
			return d.to_int64() / df::date_t::intervals_per_second;
		}

		static constexpr bool is_second_aligned(const df::date_t d)
		{
			return (d.to_int64() % df::date_t::intervals_per_second) == 0;
		}

		static df::date_t to_wall_clock(const df::date_t value, const int16_t offset)
		{
			return offset == utc_instant ? value.system_to_local() : value;
		}

		static constexpr bool compatible_representation(const int16_t left, const int16_t right)
		{
			return (left == utc_instant) == (right == utc_instant);
		}

	public:
		date_pack() noexcept = default;

		void clear()
		{
			*this = {};
		}

		bool is_empty() const
		{
			return _count == 0;
		}

		int group_count() const
		{
			return _count;
		}

		df::date_t group_value(const int i) const
		{
			return _values[i];
		}

		// What the group reads as on a clock, which is what grouping and display want.
		df::date_t group_wall_clock(const int i) const
		{
			return to_wall_clock(_values[i], _offsets[i]);
		}

		date_source group_sources(const int i) const
		{
			return static_cast<date_source>(_sources[i]);
		}

		int16_t group_offset(const int i) const
		{
			return _offsets[i];
		}

		// Sources present in the file whose distinct value did not fit. add() overflows on arrival, not
		// on authority, so this is not merely the least authoritative readings - with five or more
		// distinct values a source that would have resolved can be the one dropped. Recorded so the count
		// is not a lie, and so a pack that lost a reading says which one.
		date_source overflow() const
		{
			return static_cast<date_source>(_overflow);
		}

		// Only for restoring a stored pack. The groups are replayed through add(), which cannot rebuild
		// a mask whose values were never written, so the mask is carried across as itself.
		void restore_overflow(const date_source s)
		{
			_overflow |= static_cast<uint64_t>(s);
		}

		date_source all_sources() const
		{
			auto result = _overflow;
			for (auto i = 0; i < _count; ++i) result |= _sources[i];
			return static_cast<date_source>(result);
		}

		bool has_source(const date_source s) const
		{
			return all_sources() && s;
		}

		// A source with no date says nothing and must never erase one already recorded.
		void add(const date_source source, const df::date_t value, const int16_t utc_offset_mins = no_offset)
		{
			if (source == date_source::none || !value.is_valid()) return;

			const auto wall_clock = to_wall_clock(value, utc_offset_mins);

			for (auto i = 0; i < _count; ++i)
			{
				if (whole_seconds(group_wall_clock(i)) != whole_seconds(wall_clock)) continue;
				if (!compatible_representation(_offsets[i], utc_offset_mins)) continue;

				_sources[i] |= static_cast<uint64_t>(source);

				// The same instant arrives whole-second from EXIF and with milliseconds from XMP.
				// Keep the more precise reading; they are one date either way.
				if (is_second_aligned(_values[i]) && !is_second_aligned(value))
				{
					_values[i] = value;
					_offsets[i] = utc_offset_mins;
				}
				else if (_offsets[i] == no_offset && utc_offset_mins != no_offset && utc_offset_mins != utc_instant)
				{
					_offsets[i] = utc_offset_mins;
				}

				return;
			}

			if (_count == max_groups)
			{
				_overflow |= static_cast<uint64_t>(source);
				return;
			}

			auto at = _count;
			while (at > 0 && wall_clock < group_wall_clock(at - 1))
			{
				_values[at] = _values[at - 1];
				_sources[at] = _sources[at - 1];
				_offsets[at] = _offsets[at - 1];
				--at;
			}

			_values[at] = value;
			_sources[at] = static_cast<uint64_t>(source);
			_offsets[at] = utc_offset_mins;
			++_count;
		}

		// A container creation time or filesystem stamp, which are instants rather than readings.
		void add_utc(const date_source source, const df::date_t instant)
		{
			add(source, instant, utc_instant);
		}

		df::date_t resolve(const date_concept kind) const
		{
			for (const auto& info : date_sources)
			{
				if (info.kind != kind) continue;

				for (auto i = 0; i < _count; ++i)
				{
					if (_sources[i] & static_cast<uint64_t>(info.source)) return group_wall_clock(i);
				}
			}

			return {};
		}

		// Which tag answered, so the properties panel can say where a date came from rather than
		// leaving the user to guess why it is what it is.
		date_source resolved_source(const date_concept kind) const
		{
			for (const auto& info : date_sources)
			{
				if (info.kind != kind) continue;

				for (auto i = 0; i < _count; ++i)
				{
					if (_sources[i] & static_cast<uint64_t>(info.source)) return info.source;
				}
			}

			return date_source::none;
		}

		df::date_t original() const { return resolve(date_concept::original); }
		df::date_t created() const { return resolve(date_concept::created); }
		df::date_t modified() const { return resolve(date_concept::modified); }

		// The Created reading, but only when it is stored as a UTC instant. A build older than the
		// pack keeps `created_utc` in that form, and a wall clock written into it would be shifted
		// again on read. There is no conversion available here that would not write the scanning
		// machine's timezone into the index, so a naive reading answers nothing and the older build
		// falls back to the Original date, which is the better answer anyway.
		df::date_t created_as_utc_instant() const
		{
			for (const auto& info : date_sources)
			{
				if (info.kind != date_concept::created) continue;

				for (auto i = 0; i < _count; ++i)
				{
					if (_sources[i] & static_cast<uint64_t>(info.source))
					{
						return _offsets[i] == utc_instant ? _values[i] : df::date_t{};
					}
				}
			}

			return {};
		}

		// The single date shown where there is room for one. A scan carrying no capture time still
		// files under the day it was scanned rather than under nothing.
		df::date_t best() const
		{
			auto d = original();
			if (!d.is_valid()) d = created();
			if (!d.is_valid()) d = modified();
			return d;
		}

		friend bool operator==(const date_pack& lhs, const date_pack& rhs)
		{
			if (lhs._count != rhs._count || lhs._overflow != rhs._overflow) return false;

			for (auto i = 0; i < lhs._count; ++i)
			{
				if (lhs._values[i] != rhs._values[i] ||
					lhs._sources[i] != rhs._sources[i] ||
					lhs._offsets[i] != rhs._offsets[i])
					return false;
			}

			return true;
		}

		friend bool operator!=(const date_pack& lhs, const date_pack& rhs) { return !(lhs == rhs); }
	};

#pragma pack(pop)

	// The pack is resident for every indexed file, so its size is a collection-wide cost: changing
	// it is a deliberate decision, not a side effect of adding a field.
	// 61 -> 81 when the source mask went from 32 bits to 64. That is 20 bytes per indexed file, about
	// 2.8 MB on a 140,000 item collection, bought once so that adding a date source can never again
	// change the stored layout - and a stored layout change is what costs every user a re-index.
	static_assert(sizeof(date_pack) == 81, "date_pack size is a per-indexed-file memory cost");

	// Accepts either a bare EXIF offset - `+02:00`, which is the whole of OffsetTimeOriginal - or
	// the tail of an ISO timestamp, which is how XMP carries the same fact. It lives beside the
	// encoding it produces so the two cannot drift apart. Trailing NUL and space are tolerated
	// because an Exif ASCII value spans its whole component count and arrives padded.
	constexpr int16_t parse_utc_offset(std::string_view str)
	{
		while (!str.empty() && (str.back() == '\0' || str.back() == ' ')) str.remove_suffix(1);

		if (str.size() > 10 && (str.back() == 'Z' || str.back() == 'z'))
		{
			return date_pack::utc_instant;
		}

		if (str.size() < 6) return date_pack::no_offset;

		const auto tail = str.substr(str.size() - 6);

		const auto is_two_digits = [](const std::string_view s)
		{
			return s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9';
		};

		const auto to_two_digits = [](const std::string_view s)
		{
			return (s[0] - '0') * 10 + (s[1] - '0');
		};

		if ((tail[0] != '+' && tail[0] != '-') || tail[3] != ':') return date_pack::no_offset;
		if (!is_two_digits(tail.substr(1, 2)) || !is_two_digits(tail.substr(4, 2))) return date_pack::no_offset;

		const auto hours = to_two_digits(tail.substr(1, 2));
		const auto mins = to_two_digits(tail.substr(4, 2));

		// No zone is further than 14 hours from Greenwich, and 75 minutes is not a reading. An
		// offset outside that is a field this parser has misread, and it would be added to a wall
		// clock; answering "no offset" leaves the naive reading, which is the honest fallback.
		if (hours > 14 || mins > 59) return date_pack::no_offset;

		const auto minutes = hours * 60 + mins;
		return static_cast<int16_t>(tail[0] == '-' ? -minutes : minutes);
	}

	// EXIF stores the fraction of a second as digits after an implied point, so `12` is 0.12 s and
	// not 12 of anything. Without it a burst shot at 5 frames a second collapses to one instant and
	// the frames order by name.
	constexpr uint64_t parse_sub_second_intervals(const std::string_view str)
	{
		constexpr int digits_per_second = 7; // df::date_t counts 100 ns intervals

		uint64_t result = 0;
		auto used = 0;

		for (const auto c : str)
		{
			if (c < '0' || c > '9') break;
			if (used == digits_per_second) break;

			result = result * 10 + static_cast<uint64_t>(c - '0');
			++used;
		}

		if (used == 0) return 0;

		for (auto i = used; i < digits_per_second; ++i) result *= 10;

		return result;
	}

	// The tag a date was read from, for the properties panel. Not localized: these are tag names as
	// they appear in the standards and in every other tool the user might check against.
	constexpr std::string_view date_source_name(const date_source s)
	{
		switch (s)
		{
		case date_source::exif_original: return "EXIF DateTimeOriginal";
		case date_source::xmp_exif_original: return "XMP exif:DateTimeOriginal";
		case date_source::iptc_created: return "IPTC DateCreated";
		case date_source::id3_original_release: return "ID3 TDOR";
		case date_source::photoshop_created: return "XMP photoshop:DateCreated";
		case date_source::shell_date_taken: return "Windows DateTaken";
		case date_source::exif_digitized: return "EXIF DateTimeDigitized";
		case date_source::xmp_exif_digitized: return "XMP exif:DateTimeDigitized";
		case date_source::iptc_digital_created: return "IPTC DigitalCreationDate";
		case date_source::xmp_create: return "XMP xmp:CreateDate";
		case date_source::container_created: return "Container creation time";
		case date_source::embedded_created: return "Embedded creation time";
		case date_source::rip_date: return "Rip date";
		case date_source::file_created: return "File created";
		case date_source::exif_datetime: return "EXIF DateTime";
		case date_source::xmp_modify: return "XMP xmp:ModifyDate";
		case date_source::file_modified: return "File modified";
		case date_source::gps_stamp: return "EXIF GPSDateStamp";
		case date_source::legacy_original:
		case date_source::legacy_created:
		case date_source::legacy_modified: return "Indexed before rescan";
		default: return {};
		}
	}

	// The date bubble lists Original, Created and Modified, then the filesystem stamps. A stamp that
	// already answered one of those three must not appear again as itself, or the bubble carries the
	// same row twice under two names.
	inline bool shows_own_file_stamp_row(const date_pack& dates, const date_source stamp,
	                                     const df::date_t stamp_value = {})
	{
		constexpr date_concept kinds[] = {
			date_concept::original, date_concept::created, date_concept::modified
		};

		for (const auto kind : kinds)
		{
			if (dates.resolved_source(kind) == stamp) return false;
		}

		// The filesystem stamps are read from the item rather than stored with the metadata, so the
		// tag that answered a concept can be carrying the same instant under a different name. Asking
		// only which tag won would then show the row twice, which is what this exists to prevent.
		if (stamp_value.is_valid())
		{
			const auto stamp_second = stamp_value.to_int64() / df::date_t::intervals_per_second;

			for (const auto kind : kinds)
			{
				const auto d = dates.resolve(kind);
				if (d.is_valid() && d.to_int64() / df::date_t::intervals_per_second == stamp_second) return false;
			}
		}

		return true;
	}
}
