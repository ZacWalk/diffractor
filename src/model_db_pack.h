// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Metadata serialization for database storage. Packs and unpacks item
// metadata into compact binary format for efficient SQLite storage.

#pragma once


class metadata_packer
{
public:
	static constexpr uint8_t version = 1;

	df::blob _data;

	void reset_to_header()
	{
		_data.clear();
		_data.reserve(256);
		_data.push_back(0xff); // marker
		_data.push_back(version);
	}

	metadata_packer()
	{
		reset_to_header();
	}

	df::cspan cdata() const
	{
		return _data;
	}

	size_t size() const
	{
		return _data.size();
	}

	void write_prop_id(const uint16_t id)
	{
		_data.push_back(id & 0xff);
		_data.push_back(id >> 8 & 0xff);
	}

	void write_len(const size_t val_len)
	{
		if (val_len < 0xfe)
		{
			_data.push_back(val_len & 0xff);
		}
		else if (val_len < 0xffff)
		{
			_data.push_back(0xff);
			_data.push_back(val_len & 0xff);
			_data.push_back(val_len >> 8 & 0xff);
		}
		else
		{
			_data.push_back(0xfe);
			_data.push_back(val_len & 0xff);
			_data.push_back(val_len >> 8 & 0xff);
			_data.push_back(val_len >> 16 & 0xff);
			_data.push_back(val_len >> 24 & 0xff);
		}
	}

	template <typename T>
	void write(const uint16_t id, const T& v)
	{
		constexpr auto val_len = sizeof(v);

		write_prop_id(id);
		write_len(val_len);

		const auto existing_len = _data.size();
		_data.resize(existing_len + val_len);
		std::memcpy(_data.data() + existing_len, &v, val_len);
	}

	void write(const uint16_t id, const str::cached v)
	{
		const auto val_len = v.size();

		if (val_len >= df::one_meg)
		{
			df::log(__FUNCTION__, std::format("metadata value for property {} dropped: {} bytes", id, val_len));
			return;
		}

		if (val_len > 0)
		{
			write_prop_id(id);
			write_len(val_len);

			const auto existing_len = _data.size();
			_data.resize(existing_len + val_len);
			const auto* const src = std::bit_cast<const uint8_t*>(v.sz());
			std::copy(src, src + val_len, _data.begin() + existing_len);
		}
	}

	// Grouped by value, so the six date tags a phone writes usually cost one group. Written as one
	// property, so the record framing and the forward-compatible skip both come for free.
	//
	// The body describes its own shape: a reader takes the fields it knows from the front of each
	// group record and steps over the rest by the stated stride, so a later release can append a
	// field to a group, or to the trailer, without the stored dates becoming unreadable. A layout
	// change that cost every user a re-index is the thing this exists to prevent, and the one byte
	// each of stride and trailer length is what buys it.
	static constexpr uint8_t date_pack_version = 2;
	static constexpr uint8_t date_pack_group_stride = 18; // uint64 sources, int64 value, int16 offset
	static constexpr uint8_t date_pack_trailer_len = 8; // uint64 overflow
	static constexpr size_t date_pack_header_len = 4;

	void write_date_pack(const uint16_t id, const prop::date_pack& dates)
	{
		const auto count = dates.group_count();
		if (count <= 0) return;

		df::blob body;
		body.reserve(date_pack_header_len + count * date_pack_group_stride + date_pack_trailer_len);
		body.push_back(date_pack_version);
		body.push_back(static_cast<uint8_t>(count));
		body.push_back(date_pack_group_stride);
		body.push_back(date_pack_trailer_len);

		const auto push = [&body](const void* p, const size_t n)
		{
			const auto* const src = static_cast<const uint8_t*>(p);
			body.insert(body.end(), src, src + n);
		};

		for (auto i = 0; i < count; ++i)
		{
			const auto sources = static_cast<uint64_t>(dates.group_sources(i));
			const auto value = dates.group_value(i).to_int64();
			const auto offset = dates.group_offset(i);

			push(&sources, sizeof(sources));
			push(&value, sizeof(value));
			push(&offset, sizeof(offset));
		}

		const auto overflow = static_cast<uint64_t>(dates.overflow());
		push(&overflow, sizeof(overflow));

		write_prop_id(id);
		write_len(body.size());
		_data.insert(_data.end(), body.begin(), body.end());
	}

	void pack(const prop::item_metadata_ptr& md);
};


class metadata_unpacker
{
	const df::cspan _data;
	size_t _pos = 2;
	uint32_t _version = 0;

public:
	metadata_unpacker(const df::cspan data) : _data(data)
	{
		if (_data.size >= 2 && _data.data[0] == 0xFF)
		{
			_version = _data.data[1];
		}
	}

	size_t remaining() const
	{
		return _pos >= _data.size ? 0 : _data.size - _pos;
	}

	// True once no further (id, length, value) record can be read. Unpacking must terminate on
	// this rather than on read_type() returning null, so that a property this build does not
	// recognise is skipped by its length instead of truncating everything that follows it.
	bool at_end() const
	{
		return (_version == 0 || _version > metadata_packer::version) || remaining() < 2;
	}

	// The property for the next record, or prop::null when this build does not know the id.
	prop::key_ref read_type()
	{
		if (at_end())
		{
			return prop::null;
		}

		auto id = static_cast<uint16_t>(_data.data[_pos++]);
		id |= static_cast<uint16_t>(_data.data[_pos++] << 8);
		return prop::from_id(id);
	}

	size_t read_len()
	{
		if (remaining() < 1) return 0;
		size_t result = _data.data[_pos++];

		if (result == 0xFF)
		{
			if (remaining() < 2) return 0;
			result = static_cast<size_t>(_data.data[_pos++]);
			result |= static_cast<size_t>(_data.data[_pos++]) << 8;
		}
		else if (result == 0xFE)
		{
			if (remaining() < 4) return 0;
			result = static_cast<size_t>(_data.data[_pos++]);
			result |= static_cast<size_t>(_data.data[_pos++]) << 8;
			result |= static_cast<size_t>(_data.data[_pos++]) << 16;
			result |= static_cast<size_t>(_data.data[_pos++]) << 24;
		}

		return result;
	}

	template <typename T>
	void read_val(T& v)
	{
		const auto ser_len = read_len();
		df::assert_true(sizeof(v) == ser_len);

		if (sizeof(v) == ser_len && remaining() >= ser_len)
		{
			std::memcpy(&v, _data.data + _pos, ser_len);
		}

		_pos += ser_len;
	}

	template <typename T>
	void read_val_compatible(T& v)
	{
		const auto ser_len = read_len();

		if (remaining() >= ser_len)
		{
			if (ser_len == sizeof(uint16_t))
			{
				uint16_t stored = 0;
				std::memcpy(&stored, _data.data + _pos, sizeof(stored));
				v = static_cast<T>(stored);
			}
			else if (ser_len == sizeof(T))
			{
				std::memcpy(&v, _data.data + _pos, ser_len);
			}
		}

		_pos += ser_len;
	}

	// Consume the value of a record this build does not understand.
	void skip_val()
	{
		_pos += read_len();
	}

	// A pack whose body is malformed, or written to a pack version older than this build reads, is
	// stepped over whole. A half-read pack would claim dates the file does not have, which is worse
	// than having none. A body written by a later release is read as far as this build understands
	// it: the header states how long a group record and the trailer are, so fields appended after
	// these are stepped over rather than misread, and the dates stay readable without a re-index.
	// Answers whether any date was restored, so a caller that has the legacy records beside the pack
	// can fall back to them rather than leaving the row with no date at all.
	bool read_date_pack(prop::date_pack& dates)
	{
		const auto restored_before = dates.group_count();
		const auto ser_len = read_len();
		const auto end = _pos + ser_len;

		if (remaining() < ser_len)
		{
			_pos = end;
			return false;
		}

		if (ser_len < metadata_packer::date_pack_header_len)
		{
			_pos = end;
			return false;
		}

		const auto* p = _data.data + _pos;
		const auto version = p[0];
		const auto count = p[1];
		const size_t stride = p[2];
		const size_t trailer_len = p[3];

		const auto known_group = static_cast<size_t>(metadata_packer::date_pack_group_stride);
		const auto known_trailer = static_cast<size_t>(metadata_packer::date_pack_trailer_len);
		const auto body_len = metadata_packer::date_pack_header_len + count * stride + trailer_len;

		if (version < metadata_packer::date_pack_version || stride < known_group ||
			trailer_len < known_trailer || ser_len < body_len)
		{
			_pos = end;
			return false;
		}

		p += metadata_packer::date_pack_header_len;

		for (auto i = 0; i < count; ++i)
		{
			uint64_t sources = 0;
			uint64_t value = 0;
			int16_t offset = 0;

			std::memcpy(&sources, p, sizeof(sources));
			std::memcpy(&value, p + 8, sizeof(value));
			std::memcpy(&offset, p + 16, sizeof(offset));
			p += stride;

			// Restored one source at a time, so grouping, ordering and de-duplication are the
			// same code that filled it and a hand-edited row cannot produce a pack add() would
			// never build.
			for (const auto& info : prop::date_sources)
			{
				if (sources & static_cast<uint64_t>(info.source))
				{
					dates.add(info.source, df::date_t(value), offset);
				}
			}
		}

		// The overflow mask cannot be replayed through add(): those readings have no stored value,
		// which is what put them here. Without this the pack answers has_source differently once it
		// has been through the index, and its own equality operator disagrees with itself.
		uint64_t overflow = 0;
		std::memcpy(&overflow, p, sizeof(overflow));
		dates.restore_overflow(static_cast<prop::date_source>(overflow));

		_pos = end;

		// Whether the pack yielded a date, not whether its framing was acceptable. A body written by a
		// later release can be structurally perfect and still add nothing here - every source bit in it
		// may be one this build has no name for - and the caller must then fall back to the legacy
		// records beside it rather than leave the row with no date at all.
		return dates.group_count() > restored_before;
	}

	void read_val(str::cached& v)
	{
		const auto ser_len = read_len();

		if (remaining() >= ser_len)
		{
			v = str::cache(std::string_view{std::bit_cast<const char*>(_data.data + _pos), ser_len});
		}

		_pos += ser_len;
	}

	void unpack(const prop::item_metadata_ptr& md);
};
