// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: ICC color profile parsing. Reads and interprets ICC profile data
// to extract color space information and profile metadata.

#include "pch.h"

#include "metadata_icc.h"


enum icc_types
{
	TYPE_XYZ = 0x58595A20,
	// 'XYZ '
	TYPE_PARA = 0x70617261,
	// 'para' - Fixed: was incorrectly set to 0
	TYPE_RGB = 1,
	// 'RGB '
	TYPE_DESC = 0x64657363,
	// 'desc' - textDescriptionType
	TYPE_MLUC = 0x6D6C7563,
	// 'mluc' - multiLocalizedUnicodeType
	TYPE_TEXT = 0x74657874,
	// 'text'
	TAG_DESC = 0x64657363,
	// 'desc'
	TAG_CPRT = 0x63707274,
	// 'cprt'
	TAG_DMND = 0x646D6E64,
	// 'dmnd'
	TAG_DMDD = 0x646D6464,
	// 'dmdd'
	TAG_WTPT = 0x77747074,
	// 'wtpt'
	TAG_R_XYZ = 0x7258595A,
	// 'rXYZ'
	TAG_R_TRC = 0x72545243,
	// 'rTRC'
	TAG_G_XYZ = 0x6758595A,
	// 'gXYZ'
	TAG_G_TRC = 0x67545243,
	// 'gTRC'
	TAG_B_XYZ = 0x6258595A,
	// 'bXYZ'
	TAG_B_TRC = 0x62545243,
	// 'bTRC'
	TYPE_CURV = 0x63757276,
	// 'curv'
};


class icc_profile
{
public:
	struct DateTime
	{
		uint16_t year_ = 0;
		uint16_t month_ = 0;
		uint16_t day_ = 0;
		uint16_t hour_ = 0;
		uint16_t min_ = 0;
		uint16_t sec_ = 0;
	};

	struct XYZ
	{
		double x_ = 0.0;
		double y_ = 0.0;
		double z_ = 0.0;
	};

	struct Tag
	{
		uint32_t type_ = 0;

		// Shared, because several tags may name one region of the profile - the spec allows it, and
		// the common sRGB profile points all three tone curves at a single curve. Each is a view of
		// that region, not a copy of it.
		std::shared_ptr<const std::vector<uint8_t>> payload_;

		Tag() = default;

		Tag(const uint32_t type, std::shared_ptr<const std::vector<uint8_t>> payload) : type_(type),
			payload_(std::move(payload))
		{
		}

		const std::vector<uint8_t>& data() const
		{
			static const std::vector<uint8_t> none;
			return payload_ ? *payload_ : none;
		}
	};

	uint32_t profileSize_ = 0;
	uint32_t cmmType_ = 0;
	uint32_t profileVersion_ = 0;
	uint32_t profileClass_ = 0;
	uint32_t colorSpace_ = 0;
	uint32_t connectionSpace_ = 0;
	DateTime dtime_;
	uint32_t acsp_ = 0;
	uint32_t platform_ = 0;
	uint32_t flags_ = 0;
	uint32_t deviceManufacture_ = 0;
	uint32_t deviceModel_ = 0;
	uint64_t deviceAttrib_ = 0;
	uint32_t intent_ = 0;
	XYZ connectionIllum_;
	uint32_t creator_ = 0;

	std::map<uint32_t, Tag> tags_;
	std::vector<std::pair<uint32_t, std::string>> unread_;
	uint32_t declared_tag_count_ = 0;

	static std::string dump4(const uint32_t u)
	{
		return str::print("%c%c%c%c", u >> 24 & 0xFF, u >> 16 & 0xFF, u >> 8 & 0xFF, u & 0xFF);
	}

	// The profile name is the one thing a user wants from an ICC block, and it is buried in a tag
	// whose payload is either a Mac/ASCII pair or a table of UTF-16BE strings.
	static std::string decode_text(const Tag& t)
	{
		const auto& d = t.data();

		const auto be32 = [&d](const size_t i)
		{
			return static_cast<size_t>(d[i]) << 24 | static_cast<size_t>(d[i + 1]) << 16 |
				static_cast<size_t>(d[i + 2]) << 8 | d[i + 3];
		};

		if (t.type_ == TYPE_DESC)
		{
			// textDescriptionType: 4 byte reserved, ASCII count, then the ASCII string.
			if (d.size() < 8) return {};
			const auto count = be32(4);
			if (count == 0 || count > d.size() - 8) return {};
			const auto* const sz = std::bit_cast<const char*>(d.data() + 8);
			return std::string(sz, strnlen(sz, count));
		}

		if (t.type_ == TYPE_MLUC)
		{
			// multiLocalizedUnicodeType: 4 byte reserved, record count, record size, then records
			// of language, country, length and tag-relative offset.
			if (d.size() < 24) return {};
			const auto count = be32(4);
			const auto record_size = be32(8);
			constexpr size_t records_at = 12;
			constexpr size_t minimum_record_size = 12;
			if (count == 0 || record_size < minimum_record_size) return {};
			if (count > (d.size() - records_at) / record_size) return {};

			for (size_t record = 0; record < count; ++record)
			{
				const auto record_at = records_at + record * record_size;
				const auto len = be32(record_at + 4);
				auto offset = be32(record_at + 8);
				// Offsets are from the start of the tag, and the payload begins after the 4 byte signature.
				if (offset < 4) continue;
				offset -= 4;
				if (len < 2 || (len & 1u) != 0 || offset > d.size() || len > d.size() - offset) continue;

				// The payload is UTF-16BE, so the code unit is two bytes on every platform.
				std::u16string utf16;
				utf16.reserve(len / 2);

				for (size_t i = 0; i + 1 < len; i += 2)
				{
					utf16.push_back(static_cast<char16_t>(d[offset + i] << 8 | d[offset + i + 1]));
				}

				return str::utf16_to_utf8(utf16);
			}
		}

		if (t.type_ == TYPE_TEXT)
		{
			// textType: 4 byte reserved then a NUL terminated ASCII string.
			if (d.size() < 5) return {};
			const auto* const sz = std::bit_cast<const char*>(d.data() + 4);
			return std::string(sz, strnlen(sz, d.size() - 4));
		}

		return {};
	}

	static std::string_view intent_name(const uint32_t intent)
	{
		switch (intent)
		{
		case 0: return "Perceptual";
		case 1: return "Relative colorimetric";
		case 2: return "Saturation";
		case 3: return "Absolute colorimetric";
		default: return {};
		}
	}

	static std::string_view class_name(const uint32_t v)
	{
		switch (v)
		{
		case 0x73636E72: return "Input device";
		case 0x6D6E7472: return "Display device";
		case 0x70727472: return "Output device";
		case 0x6C696E6B: return "Device link";
		case 0x73706163: return "Colour space conversion";
		case 0x61627374: return "Abstract";
		case 0x6E6D636C: return "Named colour";
		default: return {};
		}
	}

	static std::string_view platform_name(const uint32_t v)
	{
		switch (v)
		{
		case 0x4150504C: return "Apple";
		case 0x4D534654: return "Microsoft";
		case 0x53474920: return "Silicon Graphics";
		case 0x53554E57: return "Sun Microsystems";
		case 0: return "Not identified";
		default: return {};
		}
	}

	static std::string_view space_name(const uint32_t v)
	{
		switch (v)
		{
		case 0x58595A20: return "CIEXYZ";
		case 0x4C616220: return "CIELAB";
		case 0x52474220: return "RGB";
		case 0x47524159: return "Greyscale";
		case 0x434D594B: return "CMYK";
		case 0x434D5920: return "CMY";
		case 0x59436272: return "YCbCr";
		case 0x48535620: return "HSV";
		case 0x484C5320: return "HLS";
		default: return {};
		}
	}

	// Signatures are four printable characters, so the decoded meaning is shown with the code
	// itself rather than replacing it.
	static std::string dump4_named(const uint32_t v, const std::string_view name)
	{
		const auto code = std::string(str::strip(dump4(v)));
		if (name.empty()) return code.empty() ? "(none)" : code;
		if (code.empty()) return std::string(name);
		return std::format("{} ({})", name, code);
	}

	static double s15f16(const std::vector<uint8_t>& d, const size_t offset)
	{
		if (offset + 4 > d.size()) return 0.0;
		const auto raw = static_cast<uint32_t>(d[offset]) << 24 | static_cast<uint32_t>(d[offset + 1]) << 16 |
			static_cast<uint32_t>(d[offset + 2]) << 8 | d[offset + 3];
		return static_cast<int32_t>(raw) / 65536.0;
	}

	bool tag_xyz(const uint32_t sig, XYZ& out) const
	{
		const auto found = tags_.find(sig);
		if (found == tags_.end() || found->second.type_ != TYPE_XYZ) return false;
		const auto& d = found->second.data();
		if (d.size() < 16) return false;
		out = {s15f16(d, 4), s15f16(d, 8), s15f16(d, 12)};
		return true;
	}

	// The question an ICC block usually has to answer is how wide the gamut is, and that is only
	// legible once the colorant tags are compared against the sets people actually recognise.
	std::string primaries_name() const
	{
		XYZ r, g, b;
		if (!tag_xyz(TAG_R_XYZ, r) || !tag_xyz(TAG_G_XYZ, g) || !tag_xyz(TAG_B_XYZ, b)) return {};

		struct known
		{
			std::string_view name;
			double v[9];
		};

		// D50 adapted colorants as written by the reference profiles for each space.
		static constexpr known table[] = {
			{"sRGB", {0.4360, 0.2225, 0.0139, 0.3851, 0.7169, 0.0971, 0.1431, 0.0606, 0.7141}},
			{"Adobe RGB (1998)", {0.6097, 0.3111, 0.0195, 0.2052, 0.6257, 0.0609, 0.1492, 0.0632, 0.7448}},
			{"Display P3", {0.5151, 0.2412, -0.0011, 0.2920, 0.6922, 0.0419, 0.1571, 0.0666, 0.7841}},
			{"Rec. 2020", {0.6734, 0.2790, -0.0019, 0.1656, 0.6757, 0.0299, 0.1250, 0.0453, 0.7973}},
			{"ProPhoto RGB", {0.7977, 0.2880, 0.0000, 0.1352, 0.7119, 0.0000, 0.0313, 0.0001, 0.8249}},
		};

		const double actual[9] = {r.x_, r.y_, r.z_, g.x_, g.y_, g.z_, b.x_, b.y_, b.z_};

		for (const auto& k : table)
		{
			auto matches = true;
			for (auto i = 0; i < 9 && matches; ++i) matches = std::abs(actual[i] - k.v[i]) < 0.02;
			if (matches) return std::format("approximately {}", k.name);
		}

		return "custom";
	}

	static std::string_view illuminant_name(const double x, const double y)
	{
		if (std::abs(x - 0.3457) < 0.005 && std::abs(y - 0.3585) < 0.005) return "D50";
		if (std::abs(x - 0.3127) < 0.005 && std::abs(y - 0.3290) < 0.005) return "D65";
		if (std::abs(x - 0.3324) < 0.005 && std::abs(y - 0.3474) < 0.005) return "D55";
		if (std::abs(x - 0.2831) < 0.005 && std::abs(y - 0.2971) < 0.005) return "D93";
		return {};
	}

	static std::string format_xyz(const XYZ& v)
	{
		return str::print("%.4f, %.4f, %.4f", v.x_, v.y_, v.z_);
	}

	static std::string describe_chromaticity(const XYZ& v)
	{
		const auto sum = v.x_ + v.y_ + v.z_;
		if (sum <= 0.0) return format_xyz(v);

		const auto x = v.x_ / sum;
		const auto y = v.y_ / sum;
		const auto name = illuminant_name(x, y);
		const auto chroma = str::print("x %.4f, y %.4f", x, y);
		return name.empty() ? chroma : std::format("{} ({})", name, chroma);
	}

	// A tone curve is a table or a parametric function; either way the shape is what matters.
	static std::string describe_curve(const Tag& t)
	{
		const auto& d = t.data();

		if (t.type_ == TYPE_CURV)
		{
			if (d.size() < 8) return {};
			const auto count = static_cast<size_t>(d[4]) << 24 | static_cast<size_t>(d[5]) << 16 |
				static_cast<size_t>(d[6]) << 8 | d[7];
			if (count == 0) return "linear";
			if (count == 1 && d.size() >= 10)
			{
				const auto gamma = (static_cast<uint32_t>(d[8]) << 8 | d[9]) / 256.0;
				return str::print("gamma %.2f", gamma);
			}
			return str::print("curve, %zu points", count);
		}

		if (t.type_ == TYPE_PARA)
		{
			if (d.size() < 6) return {};
			const auto function = static_cast<uint32_t>(d[4]) << 8 | d[5];
			if (function == 0 && d.size() >= 12) return str::print("gamma %.2f", s15f16(d, 8));
			if (function == 3) return "sRGB style curve";
			return str::print("parametric, type %u", function);
		}

		return {};
	}

	std::string describe_tag(const Tag& t) const
	{
		const auto decoded = decode_text(t);
		const auto text = str::strip(decoded);
		if (!text.empty()) return std::string(text);

		if (t.type_ == TYPE_XYZ && t.data().size() >= 16)
		{
			return format_xyz({s15f16(t.data(), 4), s15f16(t.data(), 8), s15f16(t.data(), 12)});
		}

		const auto curve = describe_curve(t);
		if (!curve.empty()) return curve;

		return str::print("binary, %zu bytes", t.data().size() + 4);
	}

	void add_row(metadata_kv_list& result, const str::cached key, std::string value, const int depth,
	             const std::string_view shape = {}) const
	{
		auto& row = result.emplace_back(key, std::move(value));
		row.depth = depth;
		if (!shape.empty()) row.shape = shape;
	}

	static metadata_kv& add_container(metadata_kv_list& result, const std::string_view key,
	                                  const std::string_view id)
	{
		auto& row = result.emplace_back(std::string(key), std::string{});
		row.container = true;
		row.id = id;
		return row;
	}

	metadata_kv_list dump()
	{
		metadata_kv_list result;

		// The profile identity is derived from the tags below it, so it is labelled as a summary
		// rather than presented as more of the file's own content.
		add_container(result, "Summary", "icc.summary");

		const std::pair<uint32_t, str::cached> named[] = {
			{TAG_DESC, "Profile"_c},
			{TAG_DMDD, "Device model"_c},
			{TAG_DMND, "Device manufacturer"_c},
			{TAG_CPRT, "Copyright"_c}
		};

		for (const auto& [tag, name] : named)
		{
			const auto found = tags_.find(tag);

			if (found != tags_.end())
			{
				const auto decoded = decode_text(found->second);
				const auto text = str::strip(decoded);
				if (!text.empty()) add_row(result, name, std::string(text), 1);
			}
		}

		add_row(result, "Colour space"_c, dump4_named(colorSpace_, space_name(colorSpace_)), 1);

		const auto primaries = primaries_name();
		if (!primaries.empty()) add_row(result, "Primaries"_c, primaries, 1);

		const auto trc = tags_.find(TAG_R_TRC);
		if (trc != tags_.end())
		{
			const auto curve = describe_curve(trc->second);
			if (!curve.empty()) add_row(result, "Tone response"_c, curve, 1);
		}

		XYZ white;
		if (tag_xyz(TAG_WTPT, white)) add_row(result, "White point"_c, describe_chromaticity(white), 1);

		const auto intent = intent_name(intent_);
		add_row(result, "Rendering intent"_c, intent.empty() ? str::print("%u", intent_) : std::string(intent), 1);

		// The header is fixed content the file really carries, so it is listed whole.
		add_container(result, "Header", "icc.header");

		add_row(result, "Profile size"_c, str::print("%u bytes", profileSize_), 1);
		add_row(result, "Preferred CMM"_c, dump4_named(cmmType_, {}), 1);
		add_row(result, "Version"_c,
		        str::print("%d.%d.%d", profileVersion_ >> 24 & 0xFF, profileVersion_ >> 20 & 0x0F,
		                   profileVersion_ >> 16 & 0x0F), 1);
		add_row(result, "Class"_c, dump4_named(profileClass_, class_name(profileClass_)), 1);
		add_row(result, "Data colour space"_c, dump4_named(colorSpace_, space_name(colorSpace_)), 1);
		add_row(result, "Connection space"_c, dump4_named(connectionSpace_, space_name(connectionSpace_)), 1);

		if (dtime_.year_ != 0)
		{
			add_row(result, "Created"_c,
			        str::print("%04d-%02d-%02d %02d:%02d:%02d", dtime_.year_, dtime_.month_, dtime_.day_,
			                   dtime_.hour_, dtime_.min_, dtime_.sec_), 1);
		}

		add_row(result, "File signature"_c, dump4(acsp_), 1);
		add_row(result, "Primary platform"_c, dump4_named(platform_, platform_name(platform_)), 1);
		add_row(result, "Profile flags"_c, str::print("0x%08x", flags_), 1);
		add_row(result, "Device manufacturer"_c, dump4_named(deviceManufacture_, {}), 1);
		add_row(result, "Device model"_c, dump4_named(deviceModel_, {}), 1);
		add_row(result, "Device attributes"_c, str::print("0x%016llx", deviceAttrib_), 1);
		add_row(result, "Rendering intent"_c,
		        intent.empty() ? str::print("%u", intent_) : std::format("{} ({})", intent, intent_), 1);
		add_row(result, "PCS illuminant"_c, format_xyz(connectionIllum_), 1);
		add_row(result, "Creator"_c, dump4_named(creator_, {}), 1);

		// The tag directory is the profile's actual contents, so every entry is listed in the file's
		// own order with its type and extent, and its bytes remain reachable.
		auto& tags_title = add_container(result, str::print("Tags (%zu)", tags_.size()), "icc.tags");
		tags_title.value = str::print("%u declared", declared_tag_count_);

		for (const auto& [sig, tag] : tags_)
		{
			const auto sig_text = std::string(str::strip(dump4(sig)));
			const auto type_text = std::string(str::strip(dump4(tag.type_)));

			auto& row = result.emplace_back(sig_text, describe_tag(tag));
			row.depth = 1;
			row.shape = std::format("{}, {} bytes", type_text, tag.data().size() + 4);
			row.id = std::format("icc.tag.{}", sig_text);
			const auto kept = std::min(tag.data().size(), str::max_hex_dump_bytes);
			row.detail = metadata_binary_detail{
				std::vector<uint8_t>(tag.data().begin(), tag.data().begin() + kept)
			};
		}

		if (!unread_.empty())
		{
			add_container(result, str::print("Unread tags (%zu)", unread_.size()), "icc.unread");

			for (const auto& [sig, reason] : unread_)
			{
				add_row(result, str::cache("Unread tag"sv),
				        std::format("{}: {}", str::strip(dump4(sig)), reason), 1);
			}
		}

		return result;
	}
};

class icc_stream
{
public:
	const uint8_t* buffer_;
	size_t size_ = 0;
	size_t index_ = 0;

	icc_stream(const df::cspan data, const size_t index = 0)
		: buffer_(data.data), size_(data.size), index_(index)
	{
	}

	~icc_stream()
	{
		buffer_ = nullptr;
		size_ = 0;
	}

	bool eof() const { return index_ >= size_; }

	void skip(const size_t bytes)
	{
		index_ += bytes;
		if (index_ > size_)
			index_ = size_; // Fixed: was size_ - 1, should be size_
	}

	size_t seek(const size_t index)
	{
		const size_t current = index_;
		index_ = index;
		if (index_ > size_)
			index_ = size_; // Fixed: was size_ - 1, should be size_
		return current;
	}

	uint8_t uint8()
	{
		if (index_ >= size_)
			return 0; // EOF
		const uint8_t ret = buffer_[index_];
		index_++;
		return ret;
	}

	int8_t int8() { return static_cast<int8_t>(uint8()); }

	// ICC profile uses big endian only.
	uint16_t uint16()
	{
		if (index_ + 1 >= size_) return 0; // Bounds check
		return uint8() << 8 | uint8();
	}

	int16_t int16()
	{
		if (index_ + 1 >= size_) return 0; // Bounds check
		return int8() << 8 | uint8();
	}

	uint32_t uint32()
	{
		if (index_ + 3 >= size_) return 0; // Bounds check
		return uint16() << 16 | uint16();
	}

	int32_t int32()
	{
		if (index_ + 3 >= size_) return 0; // Bounds check
		return int16() << 16 | uint16();
	}

	uint64_t uint64()
	{
		if (index_ + 7 >= size_) return 0; // Bounds check
		return static_cast<uint64_t>(uint32()) << 32 | uint32();
	}

	double s15Fixed16() { return static_cast<double>(int32()) / 0x10000; }

	std::vector<uint8_t> array(const size_t s)
	{
		// Add bounds checking for array reads
		if (s == 0 || index_ + s > size_)
		{
			return {};
		}

		std::vector<uint8_t> ret(s);
		std::copy(buffer_ + index_, buffer_ + index_ + s, ret.begin());
		index_ += s; // Update index after reading
		return ret;
	}

	icc_profile::DateTime dateTime()
	{
		icc_profile::DateTime dt;
		dt.year_ = uint16();
		dt.month_ = uint16();
		dt.day_ = uint16();
		dt.hour_ = uint16();
		dt.min_ = uint16();
		dt.sec_ = uint16();
		return dt;
	}

	icc_profile::XYZ xyz()
	{
		icc_profile::XYZ xyz;
		xyz.x_ = s15Fixed16();
		xyz.y_ = s15Fixed16();
		xyz.z_ = s15Fixed16();
		return xyz;
	}
};

bool load_from_mem(icc_profile& p, const df::cspan data)
{
	icc_stream stream(data);

	// Validate minimum header size
	if (data.size < 128)
	{
		return false;
	}

	p.profileSize_ = stream.uint32();

	// Validate profile size matches data size
	if (p.profileSize_ > data.size || p.profileSize_ < 128)
	{
		return false;
	}

	p.cmmType_ = stream.uint32();
	p.profileVersion_ = stream.uint32();
	p.profileClass_ = stream.uint32();
	p.colorSpace_ = stream.uint32();
	p.connectionSpace_ = stream.uint32();
	p.dtime_ = stream.dateTime();
	p.acsp_ = stream.uint32(); // must be acsp
	p.platform_ = stream.uint32();
	p.flags_ = stream.uint32();
	p.deviceManufacture_ = stream.uint32();
	p.deviceModel_ = stream.uint32();
	p.deviceAttrib_ = stream.uint64();
	p.intent_ = stream.uint32();
	p.connectionIllum_ = stream.xyz();
	p.creator_ = stream.uint32();
	stream.seek(128); // header

	if (p.acsp_ != 0x61637370)
	{
		// not valid Signature 'acsp'
		return false;
	}
	if (stream.eof())
		return false;

	const uint32_t tagCount = stream.uint32();

	// Reasonable limit on tag count to prevent memory issues
	if (tagCount > 1000)
	{
		return false;
	}

	p.declared_tag_count_ = tagCount;

	// A tag's payload is a region of this profile, so the regions the tags name cannot legitimately
	// amount to more than the profile itself. Without this running total a thousand tags pointing
	// into the same megabyte copied a gigabyte out of a file that size. A region several tags share
	// is charged once and its payload shared: the spec allows the sharing, and the sRGB profile most
	// cameras embed points all three tone curves at one curve, so charging each tag for it refused
	// two of the three.
	const auto max_total_tag_bytes = data.size;
	size_t total_tag_bytes = 0;
	std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<const std::vector<uint8_t>>> regions;

	for (uint32_t u = 0; u < tagCount; u++)
	{
		if (stream.index_ + 12 > data.size) // Need 12 bytes for tag entry
		{
			break;
		}

		uint32_t sig = stream.uint32();
		const uint32_t offs = stream.uint32();
		const uint32_t size = stream.uint32();

		// A tag that cannot be read is still part of the profile, so it is recorded rather than
		// silently dropped.
		if (offs >= data.size || size == 0 || size > data.size - offs)
		{
			p.unread_.emplace_back(sig, str::print("out of range (offset %u, size %u)", offs, size));
			continue;
		}

		// Reasonable size limit for individual tags
		if (size > 1024 * 1024) // 1MB limit
		{
			p.unread_.emplace_back(sig, str::print("too large to read (%u bytes)", size));
			continue;
		}

		const size_t current = stream.seek(offs);
		if (stream.index_ + 4 > data.size) // Need 4 bytes for tag type
		{
			stream.seek(current);
			p.unread_.emplace_back(sig, "truncated"s);
			continue;
		}

		const uint32_t tag_type = stream.uint32();
		const uint32_t remaining_size = size >= 4 ? size - 4 : 0;
		const auto region = std::make_pair(offs, size);

		if (const auto shared = regions.find(region); shared != regions.end())
		{
			stream.seek(current);
			p.tags_[sig] = icc_profile::Tag(tag_type, shared->second);
			continue;
		}

		if (total_tag_bytes + remaining_size > max_total_tag_bytes)
		{
			stream.seek(current);
			p.unread_.emplace_back(sig, str::print("beyond the profile's own size (%u bytes)", size));
			continue;
		}

		total_tag_bytes += remaining_size;

		auto payload = std::make_shared<const std::vector<uint8_t>>(stream.array(remaining_size));
		regions.emplace(region, payload);
		p.tags_[sig] = icc_profile::Tag(tag_type, std::move(payload));
		stream.seek(current);
	}

	return true;
}

metadata_kv_list metadata_icc::to_info(const df::cspan data)
{
	icc_profile p;

	if (load_from_mem(p, data))
	{
		return p.dump();
	}

	return {};
}
