// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Property definitions and metadata formatting. Defines all item property types
// (dimensions, dates, camera info, etc.) and provides formatting functions for display.

#pragma once

#include "model_dates.h"
#include "model_location.h"

enum class icon_index : int;
class file_type;
class file_group;

using file_type_ref = const file_type*;
using file_group_ref = const file_group*;

namespace core
{
	class surface;
	class image;
	enum class orientation : uint8_t;
}

// Compact category-presence mask used only to reject impossible search candidates.
// It is not a probabilistic hash: shared category bits may produce false positives,
// and the exact search matcher must always decide whether a candidate matches.
struct search_presence_mask
{
	static constexpr uint32_t duplicates = 1 << 30u;
	static constexpr uint32_t crc32c = 1 << 29u;

	static constexpr uint32_t album = 1 << 8u;
	static constexpr uint32_t artist = 1 << 9u;
	static constexpr uint32_t camera = 1 << 10u;
	static constexpr uint32_t camera_settings = 1 << 11u;
	static constexpr uint32_t codec = 1 << 12u;
	static constexpr uint32_t audio_codec = 1 << 13u;
	static constexpr uint32_t created = 1 << 14u;
	static constexpr uint32_t credit = 1 << 15u;
	static constexpr uint32_t duration = 1 << 16u;
	static constexpr uint32_t general = 1 << 17u;
	static constexpr uint32_t genre = 1 << 18u;
	static constexpr uint32_t location = 1 << 19u;
	static constexpr uint32_t rating_label = 1 << 20u;
	static constexpr uint32_t game = 1 << 21u;
	static constexpr uint32_t season = 1 << 22u;
	static constexpr uint32_t tag = 1 << 23u;
	static constexpr uint32_t text = 1 << 24u;
	static constexpr uint32_t year = 1 << 25u;
	static constexpr uint32_t doc_id = 1 << 26u;
	static constexpr uint32_t panorama = 1 << 27u;
	static constexpr uint32_t person = 1 << 28u;

	uint32_t types = 0;

	search_presence_mask() noexcept = default;
	~search_presence_mask() noexcept = default;
	search_presence_mask(const search_presence_mask&) noexcept = default;
	search_presence_mask& operator=(const search_presence_mask&) noexcept = default;
	search_presence_mask(search_presence_mask&&) noexcept = default;
	search_presence_mask& operator=(search_presence_mask&&) noexcept = default;

	const search_presence_mask& operator|=(const search_presence_mask& other)
	{
		types |= other.types;
		return *this;
	}

	bool contains_required(const search_presence_mask& required) const
	{
		return (types & required.types) == required.types;
	}
};

constexpr auto label_select_text = "select";
constexpr auto label_second_text = "second";
constexpr auto label_approved_text = "approved";
constexpr auto label_review_text = "review";
constexpr auto label_to_do_text = "to do";

namespace df
{
	struct search_term;
};

// Metadata Constants
namespace prop
{
	struct item_metadata;
	class key;

	// Database-only properties, deliberately absent from the public media-property registry.
	namespace cache
	{
		constexpr uint16_t faces = 0xfffe;
	}

	// The projection a file's own GPano metadata declares. Persisted in the index, so a value's
	// meaning is fixed once assigned: retire one by leaving its number unused, never by reusing it.
	// `none` is the answer for a file carrying no panorama projection metadata at all, so anything
	// else is the flag - the file says it is a panorama, rather than merely being shaped like one.
	enum class panorama_projection : uint8_t
	{
		none = 0,
		equirectangular = 1,
		cylindrical = 2,
		// GPano panorama properties are present but name no projection this build knows. Still a
		// panorama, and recorded as one, because the declaration is the fact being stored.
		unspecified = 3,
	};

	// What makes a picture or a video HDR, as the file itself says. Persisted in the index, so a value's
	// meaning is fixed once assigned: retire one by leaving its number unused, never by reusing it.
	enum class hdr_kind : uint8_t
	{
		none = 0,
		// Coded with SMPTE ST 2084's perceptual quantizer: HDR10, and the base Dolby Vision builds on.
		pq = 1,
		// Coded with hybrid log-gamma: the HDR video phones record, and broadcast HDR.
		hlg = 2,
		// An SDR picture that carries a gain map - Ultra HDR, ISO 21496-1 or Apple's - by which a display that
		// shows HDR brightens it.
		gain_map = 3,
	};

	// What a picture holds beyond its pixels, as its file says. Persisted in the index as bits, so a bit's meaning
	// is fixed once assigned: retire one by leaving it unused, never by reusing it.
	namespace picture_flag
	{
		// The first of several frames an animation shows: an animated GIF, WebP or PNG, or an image sequence.
		constexpr uint8_t animated = 1 << 0;
		// An alpha channel: the picture may be transparent in places.
		constexpr uint8_t alpha = 1 << 1;
		// A gain map, by which a display that shows HDR brightens the picture: Ultra HDR, ISO 21496-1 or Apple's.
		constexpr uint8_t gain_map = 1 << 2;
		// Its colour description is a video stream's, whose primaries are named as video editors name them.
		constexpr uint8_t video = 1 << 3;
	}

	// Which patch of the sphere a file's pixels cover, in GPano's terms: a notional full panorama
	// of full_width x full_height spanning the whole sphere, and the file's own pixels as a crop
	// out of it. Read from the file at display time rather than indexed, because only the item
	// being looked at needs it and the answer is worth no field of its own.
	struct panorama_geometry
	{
		int full_width = 0;
		int full_height = 0;
		int cropped_left = 0;
		int cropped_top = 0;
		int cropped_width = 0;
		int cropped_height = 0;

		bool operator==(const panorama_geometry&) const noexcept = default;

		// Every member arrives straight from the file as a 32-bit integer, so the two sums are widened:
		// adding two declarations near the top of the range is signed overflow, and a wrapped negative
		// would pass the bound this test exists to enforce.
		bool is_valid() const noexcept
		{
			return full_width > 0 && full_height > 0 && cropped_width > 0 && cropped_height > 0 && cropped_left >= 0 &&
			       cropped_top >= 0 && static_cast<int64_t>(cropped_left) + cropped_width <= full_width &&
			       static_cast<int64_t>(cropped_top) + cropped_height <= full_height;
		}

		// A writer that declared equirectangular and no crop. The pixels are taken to span the full
		// circle and to sit on the horizon, which is the identity for a true 2:1 sphere and the
		// least wrong guess for anything else - assuming a strip runs pole to pole would bend the
		// horizon of every panorama ever stitched.
		static panorama_geometry assumed(const sizei source) noexcept
		{
			if (source.cx <= 0 || source.cy <= 0) return {};

			const auto full_height = std::max(source.cy, source.cx / 2);

			return {source.cx, full_height, 0, (full_height - source.cy) / 2, source.cx, source.cy};
		}

		// Fills in whatever the file left out, and rejects a declaration that contradicts the
		// pixels. A crop wider than the full panorama, or a cropped extent that is not the image,
		// describes a different file than the one being drawn.
		static panorama_geometry resolve(const panorama_geometry& declared, const sizei source) noexcept
		{
			if (declared.is_valid() && declared.cropped_width == source.cx && declared.cropped_height == source.cy)
			{
				return declared;
			}

			return assumed(source);
		}
	};

	enum class data_type
	{
		int32 = 1,
		size = 2,
		date = 3,
		float32 = 4,
		string = 5,
		int_pair = 7,
		uint32 = 8,
		string2 = string,
	};

	enum class audio_sample_t
	{
		none = 0,
		unsigned_8bit = 8,
		signed_16bit = 16,
		signed_24bit = 24,
		signed_32bit = 32,
		signed_64bit = 64,
		signed_float = 32 | 1,
		signed_double = 64 | 1,
		// What FFmpeg's decoders put out, which older scans recorded; a scan records the coded samples.
		unsigned_planar_8bit = 8 | 2,
		signed_planar_16bit = 16 | 2,
		signed_planar_32bit = 32 | 2,
		signed_planar_64bit = 64 | 2,
		planar_float = 32 | 1 | 2,
		planar_double = 64 | 1 | 2,
	};

	namespace style
	{
		enum
		{
			none = 0,
			groupable = 1 << 1,
			sortable = 1 << 2,
			multi_value = 1 << 3,
			fuzzy_search = 1 << 4,
			auto_complete = 1 << 5,
		};
	};

	class key;
	using key_ref = const key*;

	class key
	{
		key(const key& other) = delete;
		key& operator=(const key& other) = delete;
		key(key&& other) noexcept = delete;
		key& operator=(key&& other) = delete;

	public:
		key(uint16_t id, std::string_view sn, std::string_view n, text_t& tx, icon_index i, data_type t, uint32_t f,
		    uint32_t bit);

		uint16_t id = 0;
		icon_index icon = {};
		core::cached short_name = {};
		core::cached name = {};
		text_t& text_key;
		data_type data_type = {};
		uint32_t flags = 0;
		uint32_t search_presence_bit = 0;

		friend bool operator==(const key& lhs, const key& rhs) { return lhs.id == rhs.id; }

		friend bool operator!=(const key& lhs, const key& rhs) { return !(lhs == rhs); }

		constexpr bool operator<(const key& other) const { return id < other.id; }

		constexpr operator key_ref() const { return this; }

		std::string text() const;
	};

	extern key album;
	extern key artist;
	extern key album_artist;
	extern key audio_codec;
	extern key audio_sample_rate;
	extern key audio_sample_rate_full;
	extern key audio_sample_type;
	extern key bitrate;
	extern key camera_manufacturer;
	extern key camera_model;
	extern key audio_channels;
	extern key location_place;
	extern key comment;
	extern key description;
	extern key disk_num;
	extern key composer;
	extern key copyright_credit;
	extern key copyright_source;
	extern key copyright_creator;
	extern key copyright_notice;
	extern key copyright_url;
	extern key location_country;
	extern key created_exif;
	extern key created_digitized;
	extern key created_utc;
	extern key dates_packed;
	extern key dimensions;
	extern key duration;
	extern key encoder;
	extern key encoding_tool;
	extern key episode;
	extern key exposure_time;
	extern key f_number;
	extern key focal_length_35mm_equivalent;
	extern key focal_length;
	extern key pixel_format;
	extern key genre;
	extern key iso_speed;
	extern key altitude;
	extern key gps_speed;
	extern key latitude;
	extern key lens;
	extern key longitude;
	extern key media_category;
	extern key megapixels;
	extern key modified;
	extern key null;
	extern key orientation;
	extern key publisher;
	extern key performer;
	extern key rating;
	extern key season;
	extern key show;
	extern key file_size;
	extern key location_state;
	extern key streams;
	extern key synopsis;
	extern key tag;
	extern key track_num;
	extern key title;
	extern key video_codec;
	extern key year;
	extern key unique_id;
	extern key file_name;
	extern key raw_file_name;
	extern key system;
	extern key game;
	extern key crc32c;
	extern key label;
	extern key doc_id;
	extern key panorama;
	extern key color_profile;
	extern key hdr;
	extern key bit_depth;
	extern key frame_rate;
	extern key contents;
	extern key picture_flags;
	extern key person;
	extern key face_regions;
	extern key icc_profile;
	extern key colour_coding;

	constexpr bool is_null(const std::string_view s)
	{
		return core::is_empty(s);
	}

	constexpr bool is_null(const float n)
	{
		return core::is_zero(n);
	}

	constexpr bool is_null(const int n)
	{
		return n == 0;
	}

	constexpr bool is_null(const uint32_t n)
	{
		return n == 0;
	}

	inline bool is_null(const core::date_t d)
	{
		return !d.is_valid();
	}

	constexpr bool is_null(const sizei s)
	{
		return s.is_empty();
	}

	constexpr bool is_null(const core::xy32 xy)
	{
		return xy.is_empty();
	}

	// A number with an optional total - track 3, or 3 of 12. Only a missing number is null: a track or episode
	// with no total is still that track or episode.
	constexpr bool is_null(const core::xy8 xy)
	{
		return xy.x == 0;
	}

	// The number and total that core::xy8 holds in 16 bits each; its constructor takes bytes.
	constexpr core::xy8 number_pair(const int n, const int of)
	{
		core::xy8 result{0, 0};
		result.x = static_cast<int16_t>(std::clamp(n, 0, static_cast<int>(INT16_MAX)));
		result.y = static_cast<int16_t>(std::clamp(of, 0, static_cast<int>(INT16_MAX)));
		return result;
	}

	// What an item is to a library of films and television. Derived on load from its name, its tags and its
	// collection folder's library type, and never stored.
	enum class media_kind : uint8_t
	{
		none,
		film,
		episode,
	};

	// Started and not finished: past its first minute - or its first twentieth, for a short recording - and short
	// of its last twentieth, where the credits leave a finished film's position.
	inline bool is_in_progress(const double position, const int length_seconds)
	{
		if (position <= 0.0 || length_seconds <= 0) return false;
		const auto length = static_cast<double>(length_seconds);
		return position >= std::min(60.0, length * 0.05) && position <= length * 0.95;
	}

	// What a collection folder holds, as its owner sets it in Options. Stored in settings by name.
	enum class library_kind : uint8_t
	{
		// Names decide: a release name - one with a resolution, codec or source tag - is a film or an episode.
		automatic,
		// The user's own recordings: nothing in it is a film or an episode, whatever its name.
		personal,
		// Films and television: every video is an episode when its name numbers one, and otherwise a film.
		films_tv,
		// Music: its videos are music videos, never films.
		music,
	};

	// Which of an item's values were read from its name or folders rather than from the file. They are derived
	// afresh whenever the item loads or is scanned, so a better reading of names reaches every item without a
	// rescan, and they are never written to the index.
	namespace from_name
	{
		constexpr uint8_t show = 1 << 0;
		constexpr uint8_t title = 1 << 1;
		constexpr uint8_t year = 1 << 2;
		constexpr uint8_t season = 1 << 3;
		constexpr uint8_t episode = 1 << 4;
	}

#pragma pack(push, 1)

	struct item_metadata
	{
		item_metadata() noexcept = default;

		sizei dimensions() const { return {width, height}; }

		core::cached album;
		core::cached album_artist;
		core::cached artist;
		core::cached audio_codec;
		core::cached bitrate;
		core::cached camera_manufacturer;
		core::cached camera_model;
		core::cached comment;
		core::cached composer;
		core::cached copyright_creator;
		core::cached copyright_credit;
		core::cached copyright_url;
		core::cached copyright_notice;
		core::cached copyright_source;
		core::cached description;
		core::cached encoder;
		core::cached file_name;
		core::cached genre;
		core::cached lens;
		core::cached location_place;
		core::cached location_country;
		core::cached location_state;
		core::cached performer;
		core::cached pixel_format;
		core::cached publisher;
		core::cached show;
		core::cached synopsis;
		core::cached title;
		core::cached video_codec;
		core::cached raw_file_name;
		core::cached tags;
		core::cached game;
		core::cached system;
		core::cached label;
		core::cached doc_id;
		// The colour space the file says its pixels are in: an ICC profile's own name, or the primaries a coded
		// colour description names - "Display P3", "BT.2020". Read, with hdr, from the colour the file states below.
		core::cached color_profile;
		// The colour the file states, kept as it states it, from which color_profile and hdr are read as the record
		// is, so a correction to how they are named needs nothing read again: its ICC profile's name, and the H.273
		// primaries and transfer its coded colour description names, 0 where it names none.
		core::cached icc_profile;
		uint8_t colour_primaries = 0;
		uint8_t colour_transfer = 0;
		// What a Commodore disk or tape holds: the names of the files on it, one a line, as it lists them.
		core::cached contents;
		// Whom the picture's metadata names, joined as tags are: docs/faces.md#21-reading-names.
		core::cached people;
		// Its face regions as the file states them, packed by faces::pack_regions and turned upright, against the
		// dimensions and orientation above, by faces::unpack_regions.
		core::cached face_regions;

		gps_coordinate coordinate;

		date_pack dates;

		float exposure_time = 0.0f;
		float f_number = 0.0f;
		float focal_length = 0.0f;
		// A video's frames a second, averaged where the container times each frame.
		float frame_rate = 0.0f;

		// docs/locations.md#item-locations: metres relative to sea level, negative below it, and km/h. Zero means
		// unknown, which reads the same as sea level or standing still and so claims nothing.
		float altitude = 0.0f;
		float gps_speed = 0.0f;

		uint16_t duration = 0;
		uint16_t focal_length_35mm_equivalent = 0;
		uint16_t height = 0;
		uint16_t iso_speed = 0;
		int16_t rating = 0;
		uint16_t audio_channels = 0;
		uint32_t audio_sample_rate = 0;
		uint16_t audio_sample_type = 0;
		uint16_t width = 0;
		uint16_t year = 0;
		uint16_t season = 0;
		panorama_projection panorama = panorama_projection::none;
		// Bits a sample, as the picture or video is coded: 8, 10, 12, 16. Zero where the file does not say.
		uint8_t bit_depth = 0;
		// prop::picture_flag bits.
		uint8_t picture_flags = 0;
		hdr_kind hdr = hdr_kind::none;
		core::orientation orientation = core::orientation::top_left;
		core::xy8 disk = {0, 0};
		core::xy8 episode = {0, 0};
		core::xy8 track = {0, 0};

		// prop::from_name bits: the values read from the name, which are never stored.
		uint8_t from_name = 0;
		media_kind kind = media_kind::none;
		// An episode named S03E00: a special, which an episode number of 0 cannot say on its own.
		bool is_special = false;

		double media_position = 0.0;

		core::cached sidecars;
		core::cached xmp;

		// The one date shown where there is room for one. docs/metadata.md#dates owns the ladder;
		// this is the only place it is implemented.
		core::date_t created() const { return dates.best(); }

		search_presence_mask calc_search_presence() const;

		// The file's own declaration, not a guess from its shape. An image merely wide enough to
		// look like a panorama answers false here.
		bool is_panorama() const { return panorama != panorama_projection::none; }

		// docs/zoom.md: what the *display* treats as a panorama, which is a wider question than what
		// the file declares. Failing a declaration, an aspect ratio of at least 2:1 with a long edge
		// of at least 4000 pixels qualifies. Aspect alone would claim every wide screenshot, and the
		// size floor excludes them. Both halves are stored dimensions, so the shape test costs no
		// field and can be retuned without a re-index. `@panorama` stays the declaration alone,
		// because a search is an assertion about the file rather than about how it is drawn.
		bool displays_as_panorama() const
		{
			if (is_panorama()) return true;

			constexpr int min_long_edge = 4000;
			const auto long_edge = std::max<int>(width, height);
			const auto short_edge = std::min<int>(width, height);

			return short_edge > 0 && long_edge >= min_long_edge && long_edge >= short_edge * 2;
		}

		bool has_gps() const { return coordinate.is_valid(); }

		bool is_hdr() const { return hdr != hdr_kind::none; }
		bool is_animated() const { return (picture_flags & picture_flag::animated) != 0; }
		bool has_alpha() const { return (picture_flags & picture_flag::alpha) != 0; }

		// Whether the file states anything of its colour, and color_profile and hdr read from what it states.
		bool has_stated_colour() const
		{
			return !is_empty(icc_profile) || colour_primaries != 0 || colour_transfer != 0 ||
			       (picture_flags & picture_flag::gain_map) != 0;
		}
		void read_colour();

		// The size the picture is shown at: turned a quarter where its orientation turns it, so a photo the camera
		// held upright is taller than it is wide whatever its sensor recorded.
		sizei displayed_dimensions() const
		{
			return core::flips_xy(orientation) ? sizei{height, width} : sizei{width, height};
		}

		bool is_in_progress() const { return prop::is_in_progress(media_position, duration); }

		std::string format(std::string_view name) const;
	};

#pragma pack(pop)

	using item_metadata_ptr = std::shared_ptr<item_metadata>;
	using item_metadata_aptr = std::atomic<std::shared_ptr<item_metadata>>;
	using item_metadata_const_ptr = std::shared_ptr<const item_metadata>;

	// One populated prose field, in the order the description panel presents them. `duplicate` marks
	// a field repeating text an earlier field already carries, which sidecar round trips make common.
	struct text_field
	{
		std::string_view id; // stable across display languages, unlike name
		std::string_view name;
		core::cached text;
		bool duplicate = false;
	};

	std::vector<text_field> descriptive_fields(const item_metadata& md);

	const key* from_id(uint16_t id);
	const key* from_prefix(std::string_view scope);

	struct prop_scope
	{
		std::string scope;
		const key* type;
	};

	std::vector<prop_scope> search_scopes();
	std::vector<prop_scope> key_scopes();

	int64_t size_bucket(int64_t n);
	double closest_fstop(double fs);
	int64_t exp_round(double d);

	std::string format_f_num(double d);
	std::string format_date(core::date_t d);

	std::string format_duration(int d);
	std::string format_exposure(double d);
	std::string format_focal_length(double d, int filmEquivalent);

	std::string format_gps(double d);
	std::string format_gps(double lat, double lon);
	std::string format_iso(int i);
	std::string format_audio_sample_rate(int v);
	std::string format_audio_sample_rate(uint32_t v);
	std::string format_audio_sample_rate(uint16_t v);
	std::string format_audio_sample_type(audio_sample_t v);
	std::string format_audio_channels(int v);
	std::string format_magnitude(const core::file_size& s);
	std::string format_streams(int s);

	std::string format_rating(int i);
	std::string format_label(std::string_view label);
	std::string format_bit_rate(int64_t i);
	std::string format_dimensions(sizei v);
	std::string format_pixels(sizei v, file_type_ref ft);
	std::string format_video_resolution(sizei v);
	// "PQ", "HLG" or "gain map": what makes the item HDR, as a search for it is typed.
	std::string_view format_hdr(hdr_kind v);
	// "10bit", as a sample type and a pixel format say it.
	std::string format_bit_depth(int v);
	// "29.97fps", as a frame rate is typed, the way "48kHz" is.
	std::string format_frame_rate(double v);

	std::string replace_tokens(std::string_view name_template, const item_metadata_const_ptr& md, std::string_view name,
	                           core::date_t created);

	// An episode's place in its show as a library names it - S01E02, S03E00 for a special, E426 numbered without
	// a season, 2 / 6 for a part, or the day it aired - interned, as it is drawn for every visible item. Empty
	// for anything that is no episode.
	core::cached episode_label(const item_metadata& md);
	// What a library titles an item: "S01E02 · Pilot" for an episode, "Dont Look Up (2021)" for a film, and the
	// title alone for anything else.
	core::cached library_caption(const item_metadata& md);

	inline double aperture_to_fstop(const double d)
	{
		return std::exp(std::log(2.0) * d / 2.0);
	}
};
