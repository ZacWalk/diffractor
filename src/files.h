// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Core file operations and format handling. Coordinates format detection, metadata extraction,
// image loading/saving, and file type classification for photos, videos, and archives.

#pragma once

#include "files_jpeg.h"

#include "model_tags.h"
#include "model_location.h"
#include "model_property.h"

#include "libfiles/commodore.h"
#include "libfiles/decode.h"
#include "libfiles/exif.h"
#include "libfiles/stream.h"

enum class group_key_type : uint32_t
{
	folder = 1 << 0,
	photo = 1 << 1,
	video = 1 << 2,
	audio = 1 << 3,
	grouped_value = 1 << 4,
	grouped_no_value = 1 << 5,
	archive = 1 << 6,
	retro = 1 << 7,
	other = 1 << 8,
};

namespace libfiles::xmp
{
	class meta;
}

namespace libfiles
{
	struct media_scan_result;
}

class read_stream;
class files;
class av_media_info;
class av_scaler;
class file_scan_result;
class file_group;
struct file_tool;

// Tools are shared so a menu command can keep the one it captured alive across a config reload.
using file_tool_ptr = std::shared_ptr<file_tool>;

struct media_name_props
{
	std::string show;
	std::string title;

	int season = 0;
	int episode = 0;
	int episode_of = 0;
	int year = 0;
};

using file_group_by_name = df::hash_map<std::string_view, file_group_ref, df::ihash, df::ieq>;
using file_type_by_extension = df::hash_map<std::string_view, file_type_ref, df::ihash, df::ieq>;

// Many tools can open the same extension or group, so every match is kept in declaration order.
using file_tool_by_extension = df::hash_map<std::string_view, std::vector<file_tool_ptr>, df::ihash, df::ieq>;
using file_tool_by_group = df::hash_map<std::string_view, std::vector<file_tool_ptr>, df::ihash, df::ieq>;

enum class file_traits : uint32_t
{
	none = 0,
	raw = 1 << 0,
	embedded_xmp = 1 << 1,
	// A metadata-only edit that fits the space the file holds is patched where the file lies; one that
	// does not is staged.
	in_place_metadata = 1 << 2,
	no_metadata_grouping = 1 << 3,
	disk_image = 1 << 4,
	text = 1 << 5,
	visualize_audio = 1 << 6,
	preview_video = 1 << 7,
	cache_metadata = 1 << 8,
	zoom = 1 << 9,
	hide_overlays = 1 << 12,
	thumbnail = 1 << 13,
	file_name_metadata = 1 << 14,
	music_metadata = 1 << 15,
	video_metadata = 1 << 16,
	photo_metadata = 1 << 17,
	edit = 1 << 18,

	av = 1 << 20,
	bitmap = 1 << 21,
	archive = 1 << 22,
	commodore = 1 << 23,
};


constexpr file_traits operator|(file_traits a, file_traits b)
{
	return static_cast<file_traits>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr file_traits operator&(file_traits a, file_traits b)
{
	return static_cast<file_traits>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

constexpr bool operator&&(file_traits a, file_traits b)
{
	return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// Upper bound on a decodable image edge. Header fields are attacker-controlled, so this
// bounds allocations and keeps width * height * bytes_per_pixel well inside 32 bits.
constexpr int64_t max_image_dimension = 65535;

enum class detected_format
{
	Unknown = 0,
	JPEG,
	PNG,
	WEBP,
	GIF,
	PSD,
	BMP,
	TIFF,
	HEIF,
	JXL,
};

class file_group
{
public:
	file_group(
		const std::string_view name,
		const std::string_view plural_name,
		const ui::color32 color,
		const icon_index icon,
		const file_traits traits,
		const group_key_type key,
		std::vector<std::string_view> sidecars)
		: name(name),
		  plural_name(plural_name),
		  key(key),
		  color(color),
		  icon(icon),
		  traits(traits),
		  sidecars(std::move(sidecars))
	{
	}

	std::string_view name;
	std::string_view plural_name;
	group_key_type key;
	ui::color32 color;
	icon_index icon = icon_index::document;
	file_traits traits = file_traits::none;
	std::vector<std::string_view> sidecars;

	// Groups are handed out as const refs, so the tool list resolved at startup is mutable like id.
	mutable std::vector<file_tool_ptr> tools;

	mutable int id = -1;

	static file_group other;
	static file_group folder;
	static file_group photo;
	static file_group video;
	static file_group audio;
	static file_group archive;
	static file_group commodore;

	// An int because it bounds the int indexes file_group_from_index takes.
	static constexpr int max_count = 7;

	std::string display_name(bool is_plural) const;

	uint32_t search_presence_bit() const
	{
		return 1 << (id % 8);
	}

	bool has_trait(const file_traits t) const
	{
		return traits && t;
	}

	ui::color32 text_color(const ui::color32 default_text_color) const
	{
		return ui::average(default_text_color, color);
	}


	operator file_group_ref() const
	{
		return this;
	}
};


class file_type
{
public:
	file_group_ref group = nullptr;
	std::string_view extension;
	std::string_view text;
	file_traits traits = file_traits::none;
	icon_index icon = icon_index::document;
	std::vector<std::string_view> sidecars;
	std::vector<file_tool_ptr> tools;

	static file_type folder;
	static file_type other;

	file_type(const file_group_ref group, const std::string_view extension, const std::string_view text,
	          const file_traits traits)
		: group(group),
		  extension(extension),
		  text(text),
		  traits(traits)
	{
	}

	bool has_trait(const file_traits t) const
	{
		return (traits && t) || group->has_trait(t);
	}

	bool is_media() const
	{
		return has_trait(file_traits::bitmap) || has_trait(file_traits::av);
	}

	bool can_cache() const
	{
		return has_trait(file_traits::cache_metadata);
	}

	bool is_playable() const
	{
		return has_trait(file_traits::av);
	}

	bool can_edit() const
	{
		return has_trait(file_traits::edit);
	}

	// The single test for what the photo edit view accepts, so entry, the selector filter
	// and save-and-next navigation cannot drift apart.
	bool can_edit_photo() const
	{
		return has_trait(file_traits::bitmap) && can_edit();
	}

	std::string display_name(const bool is_plural) const { return group->display_name(is_plural); }

	ui::color32 text_color(const ui::color32 default_text_color) const
	{
		return group->text_color(default_text_color);
	}

	ui::const_surface_ptr default_thumbnail() const;

	// Extension matches first, then group matches, both in config order so the menu is stable.
	std::vector<file_tool_ptr> all_tools() const
	{
		std::vector<file_tool_ptr> result;
		result.reserve(tools.size() + group->tools.size());

		const auto append = [&result](const std::vector<file_tool_ptr>& src)
		{
			for (const auto& t : src)
			{
				if (std::ranges::find(result, t) == result.end()) result.emplace_back(t);
			}
		};

		append(tools);
		append(group->tools);
		return result;
	}

	operator file_type_ref() const
	{
		return this;
	}
};

// Comparison is like with like. Two selected files is a cardinality fact, not a claim that the
// pair can be compared, so eligibility reads stable traits only: never online status, metadata
// arrival, or panel width.
inline bool can_compare_file_types(const file_type_ref ft1, const file_type_ref ft2)
{
	if (!ft1 || !ft2) return false;

	const auto both = [ft1, ft2](const file_traits t) { return ft1->has_trait(t) && ft2->has_trait(t); };
	return both(file_traits::bitmap) || both(file_traits::preview_video);
}

struct file_tool
{
	str::cached exe = {};
	str::cached invoke_text = {};
	str::cached text = {};
	str::cached extensions = {};
	str::cached group = {};
	df::file_path exe_path = {};

	bool invoke(df::file_path path) const;

	bool exists() const
	{
		return !exe_path.is_empty();
	}
};

// A configurable map/location service (from diffractor-tools.json). The url may
// contain {latitude} and {longitude} tokens that are substituted with the GPS
// coordinate of the focused item before the link is opened in the browser.
struct file_map_link
{
	str::cached text = {};
	str::cached url = {};
};

// Detached result of reading diffractor-tools.json and locating the configured executables.
// Produced on a worker by scan_tools; handed to apply_tools on the UI thread, which owns the
// file type and group tables it is published into.
struct file_tools_result
{
	file_tool_by_extension by_extension;
	file_tool_by_group by_group;
	std::vector<file_map_link> map_links;
};

file_group_ref file_group_from_index(int from_id);
file_group_ref parse_file_group(const std::string& text);
// Teaches parse_file_group the plural and current-language spellings of every media type. Called at
// startup and again whenever a catalog is loaded, because format_term writes the translated name.
void register_file_group_spellings();
void load_file_types();
file_tools_result scan_tools();
void apply_tools(file_tools_result result);
std::vector<file_group_ref> all_file_groups();
std::vector<file_type_ref> all_file_types();
std::vector<file_map_link> all_map_links();

// How a file type was actually touched during a session. The write paths are mutually exclusive
// per update, so reading one row tells you whether a container patched its live bytes or was
// staged and swapped - the assumption that is otherwise only inferable from traits.
enum class file_op_stat
{
	read,
	patch_in_place,
	replace,
	sidecar,
	write_failed,
	count,
};

void record_file_op(file_type_ref ft, file_op_stat stat);
void log_file_op_summary();

//
//

struct metadata_parts
{
	df::blob xmp;
	df::blob exif;
	df::blob iptc;
	df::blob icc;
	df::blob photoshop_resources;

	// Move-only: each member is a full metadata buffer, so an implicit copy is a silent deep copy.
	metadata_parts() noexcept = default;
	metadata_parts(const metadata_parts&) = delete;
	metadata_parts& operator=(const metadata_parts&) = delete;
	metadata_parts(metadata_parts&&) noexcept = default;
	metadata_parts& operator=(metadata_parts&&) noexcept = default;
};

namespace files_test_hooks
{
	void fail_next_webp_chunk(std::string_view fourcc);
	void set_jpeg_photoshop_resource_cap(size_t bytes);
	size_t jpeg_photoshop_resource_cap();
}

class file_scan_result
{
	void parse_metadata_ffmpeg_kv(prop::item_metadata& result) const;
	void parse_media_properties(prop::item_metadata& result) const;

public:
	detected_format format = detected_format::Unknown;

	uint32_t width = 0;
	uint32_t height = 0;

	str::cached pixel_format = {};
	ui::orientation orientation = ui::orientation::top_left;
	// The decoder already rotated the pixels for a transform stored in the container (HEIF 'irot'
	// / 'imir'), so `orientation` describes nothing further to do. Tracked separately for the
	// thumbnail because it is a distinct item that need not carry the same transform.
	bool orientation_applied = false;
	bool thumbnail_orientation_applied = false;

	mutable ui::const_image_ptr thumbnail_image;
	ui::surface_ptr thumbnail_surface;
	mutable ui::const_image_ptr cover_art;
	// The whole encoded file wrapped for display. Set only where the scan already read the file into
	// memory and the bytes are directly displayable (JPEG/PNG/WEBP), so it costs nothing to keep.
	ui::const_image_ptr full_image;

	metadata_parts metadata;
	std::optional<libfiles::exif_metadata> tiff_metadata;

	metadata_kv_list ffmpeg_metadata;
	metadata_kv_list libraw_metadata;
	// An audio or video file as libfiles read it: its properties and tags stand in for FFmpeg's metadata,
	// and the info panel shows its tags and structure.
	std::shared_ptr<const libfiles::media_scan_result> media;

	// How the file itself is assembled, as read by the format scanner rather than a metadata parser.
	metadata_kv_list structure_metadata;

	df::date_t created_utc;
	df::date_t created_local;
	prop::date_source created_local_source = prop::date_source::none;
	int iso_speed = 0;
	float exposure_time = 0;
	float f_number = 0;
	float focal_length = 0;

	str::cached comment = {};
	str::cached artist = {};
	str::cached camera_model = {};
	str::cached camera_manufacturer = {};
	str::cached video_codec = {};
	str::cached copyright_notice = {};
	str::cached title = {};
	mutable std::vector<str::cached> keywords;
	mutable std::vector<str::cached> windows_categories;

	uint32_t nb_streams = 0;
	double duration = 0.0;

	str::cached audio_codec = {};
	int audio_sample_rate = 0;
	int audio_channels = 0;
	prop::audio_sample_t audio_sample_type = prop::audio_sample_t::none;
	str::cached bitrate = {};
	mutable gps_coordinate gps;
	uint32_t crc32c = 0;
	bool success = false;

	file_scan_result() noexcept = default;
	file_scan_result(const file_scan_result&) = delete;
	file_scan_result& operator=(const file_scan_result&) = delete;
	file_scan_result(file_scan_result&&) noexcept = default;
	file_scan_result& operator=(file_scan_result&&) noexcept = default;

	sizei dimensions() const
	{
		return {static_cast<int>(width), static_cast<int>(height)};
	}

	// Borrows the scanned parts; fallback holds synthesised EXIF when nothing was scanned.
	const metadata_parts& save_metadata(metadata_parts& fallback) const;
	prop::item_metadata_ptr to_props() const;
	av_media_info to_info() const;
};

// What the decoded pixels are for. `display` is anything the user looks at full size, where cheap
// chroma upsampling shows as blocky colour on saturated edges; `thumbnail` covers small previews,
// hashes and scans, where speed wins. Scale alone cannot tell these apart - a display shrunk below
// half size still deserves the better filter.
enum class decode_intent : uint8_t
{
	display,
	thumbnail
};

struct file_load_result
{
	// Why a load produced nothing. `too_large` is a property of the file rather than a transient
	// fault, so it is never retried and the display can name it.
	enum class failure
	{
		none,
		unreadable,
		too_large
	};

	bool success = false;
	bool is_preview = false;
	failure reason = failure::none;
	// Read from the header, so it survives a refusal that decoded nothing.
	sizei source_dimensions;

	ui::const_surface_ptr s;
	ui::const_image_ptr i;

	file_load_result() noexcept = default;
	file_load_result(const file_load_result&) = default;
	file_load_result& operator=(const file_load_result&) = default;
	file_load_result(file_load_result&&) noexcept = default;
	file_load_result& operator=(file_load_result&&) noexcept = default;

	sizei dimensions() const;
	ui::const_surface_ptr to_surface(sizei scale_hint = {}, bool can_use_yuv = false,
	                                 const df::cancel_token& token = {},
	                                 decode_intent intent = decode_intent::display) const;
	ui::pixel_difference_result calc_pixel_difference(const file_load_result& other) const;

	void clear()
	{
		s.reset();
		i.reset();
		success = false;
		reason = failure::none;
		source_dimensions = {};
	}

	bool is_empty() const
	{
		return ui::is_empty(s) && ui::is_empty(i);
	}

	ui::orientation orientation() const
	{
		if (success)
		{
			if (is_valid(i)) return i->orientation();
			if (is_valid(s)) return s->orientation();
		}
		return {};
	}

	bool is_jpeg() const
	{
		return success && ui::is_jpeg(i);
	}
};

// A loaded picture as the clipboard offers it: decoded into a target's rows only when one asks. Null
// when the load produced nothing.
platform::clipboard_image_ptr clipboard_image_of(const file_load_result& loaded);


struct file_encode_params
{
	int jpeg_save_quality = 85;
	bool webp_lossless = false;
	int webp_quality = 70;
	bool webp_lossy_alpha = false;
	// Trades a few percent of size for a several-times cheaper encode, for output nothing reads back.
	bool webp_fast = false;

	// The JPEG an edit is being written back over, when there is one. Encoding against its own
	// quantization tables and chroma sampling leaves an untouched block quantizing to itself,
	// where the quality slider's tables would re-quantize the whole image.
	df::cspan jpeg_source;
};

// What the caller will do with the result. Indexing and thumbnailing want the cheapest read that
// still yields properties, so only `inspect` pays for the file-structure report and the extra reads
// and formatting it needs.
enum class scan_intent
{
	index,
	inspect
};

// What a post-write re-scan needs. files::update runs the scan itself, while the file it just wrote
// is still open, so no caller ever reopens it by name (see docs/file-io.md section 6).
struct rescan_spec
{
	bool wanted = false;
	bool load_thumbnail = false;
	// Also return the full-size image for the display, which the photo scan already decodes.
	bool want_image = false;
	// Also hand back the open handle. An AV container is far too large to pass as bytes, so the
	// display that is about to reopen the file takes the handle rather than opening the path again.
	bool want_handle = false;
	file_type_ref file_type = nullptr;
	std::string xmp_sidecar;
	sizei max_thumb_size;
	scan_intent intent = scan_intent::index;
};

struct file_update_result : platform::file_op_result
{
	// True when the write staged a replacement and swapped it in, rather than patching the live
	// file in place or touching only a sidecar.
	bool staged = false;
	// True when scan holds a result the caller should apply.
	bool scanned = false;
	// True when the scan was taken through the post-swap handle, so modified is authoritative.
	bool coherent = false;
	// True when an in-place writer failed after it may have changed the live destination. The
	// operation remains failed, but callers must force a rescan because cached state may be stale.
	bool live_file_maybe_changed = false;
	file_scan_result scan;
	// The written bytes as a displayable image, for the item currently on screen. Free for
	// JPEG/PNG/WEBP because the scan already materialises and wraps them.
	file_load_result loaded;
	// The still-open handle over the written file, when the caller asked for it. The only way a
	// handle leaves update(), and only to a caller that reopens the same file straight away.
	platform::file_ptr display_handle;

	file_update_result() noexcept = default;
	file_update_result(const file_update_result&) = delete;
	file_update_result& operator=(const file_update_result&) = delete;
	file_update_result(file_update_result&&) noexcept = default;
	file_update_result& operator=(file_update_result&&) noexcept = default;
};

struct file_update_publish
{
	bool fail_if_exists = false;
	bool require_unchanged_destination = false;
	platform::file_attributes_t destination_attributes;
};

extern std::function<void(df::file_path destination)> test_before_file_update_publish;


// Shared by the format scanners to build the file-structure block: a leaf row, a section heading,
// and a pass that drops the sections a given file had nothing to put in.
void add_structure_row(metadata_kv_list& kv, std::string_view key, std::string value,
                       std::string_view shape = {}, std::string detail = {});

// As above, but `payload` is the segment's own bytes; the hex listing is built only once the row is
// opened, so a scan never formats a dump nobody reads.
void add_structure_bytes(metadata_kv_list& kv, std::string_view key, std::string value,
                         std::string_view shape, const uint8_t* payload, size_t payload_len);
void add_structure_section(metadata_kv_list& kv, std::string_view key, std::string_view id,
                           bool open_by_default = false);
void finish_structure_sections(metadata_kv_list& kv);

media_name_props scan_info_from_title(std::string_view name);

// What a loader read from the header before it committed to decoding. Lets a caller tell "too big
// for this machine" apart from "broken", and report the size either way.
struct load_diagnostic
{
	sizei source_dimensions;
	bool over_budget = false;
};

// Records what a loader read and answers whether decoding must be refused because the frame would
// not fit in df::max_decode_bytes. Called before any pixels are allocated.
bool reject_over_budget_source(load_diagnostic* diagnostic, sizei source_dimensions, std::string_view format);

// A PSD's composite image, decoded by libfiles. Throws for a file cut short, so a damaged PSD is not
// offered to ffmpeg.
ui::surface_ptr load_psd(read_stream& s, load_diagnostic* diagnostic = nullptr);
file_load_result load_raw(df::file_path path, bool can_load_preview);
ui::surface_ptr load_png(df::cspan data);
ui::surface_ptr load_webp(df::cspan data, bool can_use_yuv = false, sizei target_extent = {});
ui::surface_ptr load_heif(read_stream& s, load_diagnostic* diagnostic = nullptr);
// The libheif decoder plugin HEVC goes to - "ffmpeg" where libheif carries that plugin - or null.
const char* heif_hevc_decoder_id();
// The decoder plugin a file's primary image is steered to, or null where libheif keeps its own choice.
const char* heif_primary_decoder_id(read_stream& s);
ui::surface_ptr load_jxl(read_stream& s, load_diagnostic* diagnostic = nullptr);

ui::image_ptr save_png(const ui::const_surface_ptr& surface_in, const metadata_parts& metadata);
ui::image_ptr save_webp(const ui::const_surface_ptr& surface_in, const metadata_parts& metadata,
                        const file_encode_params& params);
ui::image_ptr save_jpeg(const ui::const_surface_ptr& surface_in, const metadata_parts& metadata,
                        const file_encode_params& encoder_params);
ui::image_ptr save_surface(const ui::image_format& format, const ui::const_surface_ptr& surface,
                           const metadata_parts& metadata, const file_encode_params& params);
ui::image_format extension_to_format(std::string_view ext);

struct pack128
{
	uint32_t u1;
	uint32_t u2;
	uint32_t u3;
	uint32_t u4;

	operator df::cspan() const
	{
		return {std::bit_cast<const uint8_t*>(this), 16};
	}
};

class read_stream
{
protected:
	uint64_t _file_size = 0;

	// Overflow-safe range check. All parsing here runs over untrusted file data,
	// so pos + len must never be evaluated directly.
	void check_range(const uint64_t pos, const uint64_t len) const
	{
		if (pos > _file_size || len > _file_size - pos)
			throw app_exception("read past end of file"s);
	}

public:
	virtual ~read_stream() = default;

	virtual uint8_t peek8(uint64_t pos) = 0;
	virtual uint16_t peek16(uint64_t pos) = 0;
	virtual uint32_t peek32(uint64_t pos) = 0;
	virtual uint64_t peek64(uint64_t pos) = 0;
	virtual pack128 peek128(uint64_t pos) = 0;

	virtual df::blob read_all() = 0;
	virtual df::blob read(uint64_t pos, size_t len) = 0;
	virtual void read(uint64_t pos, uint8_t* buffer, size_t len) = 0;

	// As read, but bypassing any window the stream keeps, for a caller that buffers for itself.
	virtual void read_through(const uint64_t pos, uint8_t* buffer, const size_t len)
	{
		read(pos, buffer, len);
	}

	// Empty unless the whole stream is already resident and can be borrowed without copying.
	virtual df::cspan view() const
	{
		return {};
	}

	// Borrows the whole stream when possible, otherwise loads it into owner and borrows that.
	df::cspan view_all(df::blob& owner)
	{
		const auto borrowed = view();
		if (!borrowed.empty()) return borrowed;
		owner = read_all();
		return owner;
	}

	const uint64_t size() const
	{
		return _file_size;
	}
};


class mem_read_stream final : public read_stream
{
	const uint8_t* const _data = nullptr;

public:
	mem_read_stream(const df::cspan cs) : _data(cs.data)
	{
		_file_size = cs.size;
	}

	template <typename T>
	T peek(const uint64_t pos)
	{
		check_range(pos, sizeof(T));
		T result;
		memcpy(&result, _data + pos, sizeof(T));
		return result;
	}

	uint8_t peek8(const uint64_t pos) override
	{
		return peek<uint8_t>(pos);
	}

	uint16_t peek16(const uint64_t pos) override
	{
		return peek<uint16_t>(pos);
	}

	uint32_t peek32(const uint64_t pos) override
	{
		return peek<uint32_t>(pos);
	}

	uint64_t peek64(const uint64_t pos) override
	{
		return peek<uint64_t>(pos);
	}

	pack128 peek128(const uint64_t pos) override
	{
		return peek<pack128>(pos);
	}

	void read(const uint64_t pos, uint8_t* buffer, const size_t len) override
	{
		check_range(pos, len);
		memcpy(buffer, _data + pos, len);
	}

	df::blob read(const uint64_t pos, const size_t len) override
	{
		check_range(pos, len);
		df::blob result(len);
		memcpy(result.data(), _data + pos, len);
		return result;
	}

	df::cspan view() const override
	{
		return {_data, static_cast<size_t>(_file_size)};
	}

	df::blob read_all() override
	{
		df::blob result(static_cast<size_t>(_file_size));
		memcpy(result.data(), _data, static_cast<size_t>(_file_size));
		return result;
	}
};

class file_read_stream final : public read_stream
{
	platform::file_ptr _h;
	uint8_t* _buffer = nullptr;
	// 64-bit throughout: positions can exceed 4 GB and size_t is 32-bit on Win32.
	uint64_t _loaded_start_pos = 0;
	uint64_t _loaded_end_pos = 0;
	uint64_t _buffer_size = 0;
	uint64_t _block_size = 0;

	void load_buffer(uint64_t pos, size_t len);

public:
	file_read_stream() = default;
	// It owns _buffer, which the destructor frees, so a copy would free it twice.
	file_read_stream(const file_read_stream&) = delete;
	file_read_stream& operator=(const file_read_stream&) = delete;

	bool open(df::file_path path);
	bool open(platform::file_ptr h);
	void close();

	~file_read_stream() override;

	template <typename T>
	T peek(const uint64_t pos)
	{
		load_buffer(pos, sizeof(T));
		T result;
		memcpy(&result, _buffer + (pos - _loaded_start_pos), sizeof(T));
		return result;
	}

	uint8_t peek8(const uint64_t pos) override
	{
		return peek<uint8_t>(pos);
	}

	uint16_t peek16(const uint64_t pos) override
	{
		return peek<uint16_t>(pos);
	}

	uint32_t peek32(const uint64_t pos) override
	{
		return peek<uint32_t>(pos);
	}

	uint64_t peek64(const uint64_t pos) override
	{
		return peek<uint64_t>(pos);
	}

	pack128 peek128(const uint64_t pos) override
	{
		return peek<pack128>(pos);
	}

	void read(uint64_t pos, uint8_t* buffer, size_t len) override;
	void read_through(uint64_t pos, uint8_t* buffer, size_t len) override;

	df::blob read(const uint64_t pos, const size_t len) override
	{
		// Validate before sizing the buffer - len comes from untrusted file fields.
		check_range(pos, len);
		df::blob result(len);
		read(pos, result.data(), len);
		return result;
	}

	// Reads straight off the handle: routing the whole file through the sliding window would
	// allocate and copy it a second time.
	df::blob read_all() override;
};

// A platform file as the stream libfiles reads and writes. Every access seeks first, so the handle's
// own position is not to be shared with another reader.
class platform_file_stream final : public libfiles::writable_stream
{
	platform::file_ptr _f;

public:
	explicit platform_file_stream(platform::file_ptr f) : _f(std::move(f))
	{
	}

	uint64_t size() override;
	size_t read(uint64_t offset, void* data, size_t size) override;
	bool write(uint64_t offset, const void* data, size_t size) override;
	bool resize(uint64_t size) override;
	bool flush() override;
};

// A read_stream as libfiles' positional stream. libfiles keeps its own window, so the reads go through
// read_through and the file is not buffered twice.
class read_stream_source final : public libfiles::stream
{
	read_stream& _s;

public:
	explicit read_stream_source(read_stream& s) : _s(s)
	{
	}

	uint64_t size() override
	{
		return _s.size();
	}

	size_t read(const uint64_t offset, void* data, const size_t size) override
	{
		if (offset >= _s.size()) return 0;

		const auto n = static_cast<size_t>(std::min<uint64_t>(size, _s.size() - offset));
		_s.read_through(offset, static_cast<uint8_t*>(data), n);
		return n;
	}
};

struct codec_info final : df::no_copy
{
	bool item_only = false;
	std::string title;
	std::string key;
	std::string extension_default;
};

namespace photo_edits_default
{
	constexpr double Brightness = 0.0;
	constexpr double Contrast = 0.0;
	constexpr double Darks = 0.0;
	constexpr double Vibrance = 0.0;
	constexpr double Lights = 0.0;
	constexpr double Midtones = 0.0;
	constexpr double Rotation = 0.0;
	constexpr double Saturation = 0.0;
	constexpr double Straighten = 0.0;
	constexpr double PerspectiveHorizontal = 0.0;
	constexpr double PerspectiveVertical = 0.0;
	constexpr double Temperature = 0.0;
	constexpr double Tint = 0.0;
};

class image_edits
{
	quadd _crop;
	sizei _scale{0, 0};

	double _brightness = photo_edits_default::Brightness;
	double _contrast = photo_edits_default::Contrast;
	double _darks = photo_edits_default::Darks;
	double _vibrance = photo_edits_default::Vibrance;
	double _lights = photo_edits_default::Lights;
	double _midtones = photo_edits_default::Midtones;
	double _saturation = photo_edits_default::Saturation;
	double _perspective_horizontal = photo_edits_default::PerspectiveHorizontal;
	double _perspective_vertical = photo_edits_default::PerspectiveVertical;
	double _temperature = photo_edits_default::Temperature;
	double _tint = photo_edits_default::Tint;

public:
	image_edits() = default;

	image_edits(const int s) : _scale(s, s)
	{
	}

	image_edits(const sizei s) : _scale(s)
	{
	}

	image_edits(const image_edits& other) = default;
	image_edits& operator=(const image_edits& other) = default;

	friend bool operator==(const image_edits& lhs, const image_edits& rhs)
	{
		return lhs._crop == rhs._crop
			&& lhs._scale == rhs._scale
			&& lhs._brightness == rhs._brightness
			&& lhs._contrast == rhs._contrast
			&& lhs._darks == rhs._darks
			&& lhs._vibrance == rhs._vibrance
			&& lhs._lights == rhs._lights
			&& lhs._midtones == rhs._midtones
			&& lhs._saturation == rhs._saturation
			&& lhs._perspective_horizontal == rhs._perspective_horizontal
			&& lhs._perspective_vertical == rhs._perspective_vertical
			&& lhs._temperature == rhs._temperature
			&& lhs._tint == rhs._tint;
	}

	friend bool operator!=(const image_edits& lhs, const image_edits& rhs)
	{
		return !(lhs == rhs);
	}

	void clear()
	{
		_crop.clear();
		_scale.cx = 0;
		_scale.cy = 0;
		_brightness = photo_edits_default::Brightness;
		_contrast = photo_edits_default::Contrast;
		_darks = photo_edits_default::Darks;
		_vibrance = photo_edits_default::Vibrance;
		_lights = photo_edits_default::Lights;
		_midtones = photo_edits_default::Midtones;
		_saturation = photo_edits_default::Saturation;
		_perspective_horizontal = photo_edits_default::PerspectiveHorizontal;
		_perspective_vertical = photo_edits_default::PerspectiveVertical;
		_temperature = photo_edits_default::Temperature;
		_tint = photo_edits_default::Tint;
	}

	bool has_scale() const
	{
		return _scale.cx != 0 && _scale.cy != 0;
	}

	void scale(const sizei s)
	{
		_scale = s;
	}

	void scale(const int s)
	{
		_scale.cx = s;
		_scale.cy = s;
	}

	sizei scale() const
	{
		return _scale;
	}

	const quadd& crop_bounds() const
	{
		return _crop;
	}

	const quadd crop_bounds(const sizei size) const
	{
		if (_crop.is_empty()) return quadd(size);
		return _crop;
	}

	bool has_crop_bounds() const
	{
		return !_crop.is_empty();
	}

	quadd perspective_bounds(sizei size) const;
	quadd effective_crop_bounds(sizei size) const;

	void crop_bounds(const quadd& v)
	{
		_crop = v;
	}

	double contrast() const
	{
		return _contrast;
	}

	void contrast(const double v)
	{
		_contrast = v;
	}

	double brightness() const
	{
		return _brightness;
	}

	void brightness(const double v)
	{
		_brightness = v;
	}

	double saturation() const
	{
		return _saturation;
	}

	void saturation(const double v)
	{
		_saturation = v;
	}

	double vibrance() const
	{
		return _vibrance;
	}

	void vibrance(const double v)
	{
		_vibrance = v;
	}

	double darks() const
	{
		return _darks;
	}

	void darks(const double v)
	{
		_darks = v;
	}

	double lights() const
	{
		return _lights;
	}

	void lights(const double v)
	{
		_lights = v;
	}

	double midtones() const
	{
		return _midtones;
	}

	void midtones(const double v)
	{
		_midtones = v;
	}

	double perspective_horizontal() const
	{
		return _perspective_horizontal;
	}

	void perspective_horizontal(const double v)
	{
		_perspective_horizontal = v;
	}

	double perspective_vertical() const
	{
		return _perspective_vertical;
	}

	void perspective_vertical(const double v)
	{
		_perspective_vertical = v;
	}

	bool has_perspective() const
	{
		return _perspective_horizontal != photo_edits_default::PerspectiveHorizontal ||
			_perspective_vertical != photo_edits_default::PerspectiveVertical;
	}

	double temperature() const
	{
		return _temperature;
	}

	void temperature(const double v)
	{
		_temperature = v;
	}

	double tint() const
	{
		return _tint;
	}

	void tint(const double v)
	{
		_tint = v;
	}

	bool has_color_changes() const
	{
		return _brightness != photo_edits_default::Brightness || _contrast != photo_edits_default::Contrast ||
			_saturation != photo_edits_default::Saturation || _vibrance != photo_edits_default::Vibrance ||
			_darks != photo_edits_default::Darks || _midtones != photo_edits_default::Midtones ||
			_lights != photo_edits_default::Lights || _temperature != photo_edits_default::Temperature ||
			_tint != photo_edits_default::Tint;
	}

	bool has_changes(sizei image_extent) const;
	// Conservative: false guarantees has_changes is false for every frame, so a caller can decide
	// without reading the source. True does not guarantee the reverse - only a crop needs the frame.
	bool is_empty() const;
	bool has_crop(sizei image_extent) const;
	bool is_no_loss(sizei image_extent) const;

	bool has_rotation() const
	{
		return rotation_angle() != 0.0;
	};

	double rotation_angle() const;
};

// The edits applied to a surface: perspective, straightening, crop and scale, then the colour. The
// surface itself when none of them changes it; empty when a canvas cannot be had or the token cancels.
ui::const_surface_ptr apply_image_edits(const ui::const_surface_ptr& source, const image_edits& photo_edits,
                                        const df::cancel_token& token = {});

class metadata_edits
{
public:
	std::optional<std::string> title;
	std::optional<std::string> copyright_notice;
	std::optional<std::string> copyright_creator;
	std::optional<std::string> copyright_source;
	std::optional<std::string> copyright_credit;
	std::optional<std::string> copyright_url;
	std::optional<std::string> description;
	std::optional<std::string> comment;
	std::optional<std::string> synopsis;
	std::optional<std::string> artist;
	std::optional<std::string> album;
	std::optional<std::string> album_artist;
	std::optional<std::string> genre;
	std::optional<std::string> show;
	std::optional<df::date_t> date_original;
	std::optional<df::date_t> date_created;

	std::optional<int> season;
	std::optional<df::xy8> episode;
	std::optional<int> year;
	std::optional<int> rating;
	std::optional<std::string> label;
	std::optional<ui::orientation> orientation;

	std::optional<df::xy8> track_num;
	std::optional<df::xy8> disk_num;

	tag_set add_tags;
	tag_set remove_tags;

	bool remove_rating = false;

	std::optional<gps_coordinate> location_coordinate;
	std::optional<std::string> location_place;
	std::optional<std::string> location_state;
	std::optional<std::string> location_country;

	metadata_edits() noexcept = default;
	~metadata_edits() = default;
	metadata_edits(const metadata_edits&) = default;
	metadata_edits& operator=(const metadata_edits&) = default;
	metadata_edits(metadata_edits&&) noexcept = default;
	metadata_edits& operator=(metadata_edits&&) noexcept = default;

	void apply(libfiles::xmp::meta& meta) const;

	bool has_changes() const
	{
		return
			title.has_value() ||
			copyright_notice.has_value() ||
			copyright_creator.has_value() ||
			copyright_source.has_value() ||
			copyright_credit.has_value() ||
			copyright_url.has_value() ||
			description.has_value() ||
			comment.has_value() ||
			synopsis.has_value() ||
			artist.has_value() ||
			album.has_value() ||
			album_artist.has_value() ||
			genre.has_value() ||
			show.has_value() ||
			date_original.has_value() ||
			date_created.has_value() ||
			season.has_value() ||
			episode.has_value() ||
			year.has_value() ||
			rating.has_value() ||
			label.has_value() ||
			track_num.has_value() ||
			disk_num.has_value() ||
			location_coordinate.has_value() ||
			location_place.has_value() ||
			location_state.has_value() ||
			location_country.has_value() ||
			orientation.has_value() ||
			!add_tags.is_empty() ||
			!remove_tags.is_empty() ||
			remove_rating;
	}

	// Orientation is the only edit here that changes what is drawn. Every other field leaves both the
	// decoded pixels and the transform applied to them valid, so a held image stays correct.
	bool changes_presentation() const
	{
		return orientation.has_value();
	}

	friend class files;
};

struct archive_item
{
	std::string filename;
	df::file_size uncompressed_size;
	df::file_size compressed_size;
	df::date_t created;
};

// What a caller decoding in-memory bytes asks for: the extent the result has to fit, whether planar
// YUV may come back, and whether it is for display or a thumbnail.
struct still_decode_request
{
	sizei target_extent;
	bool can_use_yuv = false;
	decode_intent intent = decode_intent::display;
};

// Everything Diffractor knows about one still image format libfiles names from its first bytes: the
// file type it registers, how its header is read without decoding, and how it decodes. A format with
// no decoder of its own is decoded by ffmpeg. Supporting another is one entry in files_formats.cpp,
// plus a still_file_type line at its place in the extension table.
struct still_format
{
	detected_format format = detected_format::Unknown;
	std::string_view extensions;
	std::string_view description;
	file_traits traits = file_traits::none;
	// A loaded file keeps its encoded bytes in this form rather than decoding them.
	ui::image_format encoded = ui::image_format::Unknown;
	// Dimensions, metadata and, when asked for, a thumbnail, read without decoding the frame.
	file_scan_result (*scan)(read_stream& s, scan_intent intent, bool want_thumbnail, files* decoder) = nullptr;
	// Decodes a whole file for a load. The diagnostic names a source refused for its size.
	ui::surface_ptr (*load)(read_stream& s, load_diagnostic* diagnostic) = nullptr;
	// Decodes in-memory bytes for display or a thumbnail.
	ui::surface_ptr (*decode)(files& ff, df::cspan data, const still_decode_request& request) = nullptr;
};

std::span<const still_format> still_formats();
const still_format* find_still_format(detected_format format);
// The extension-table entry for a still format, built from its still_format.
file_type still_file_type(detected_format format);
// The JPEG entry's decoder. A friend of files, whose decoder state it uses.
ui::surface_ptr decode_still_jpeg(files& ff, df::cspan data, const still_decode_request& request);

// Formats whose loaded files stay encoded: the bytes are the image.
inline bool is_image_format(const detected_format& format)
{
	const auto* const still = find_still_format(format);
	return still && still->encoded != ui::image_format::Unknown;
}


ui::image_ptr load_image_file(df::cspan data);
// want_thumbnail is opt-in because extracting an embedded thumbnail is not free: it reads the
// thumbnail's bytes off the file and copies them, or decodes them, none of which a metadata scan
// has any use for. decoder is the caller's own files instance, borrowed only when a thumbnail in
// an awkward format has to be decoded; a null one makes the scan build its own.
file_scan_result scan_photo(read_stream& s, scan_intent intent = scan_intent::index,
                            bool want_thumbnail = false, files* decoder = nullptr);
// The scan of a still libfiles reads with nothing added: JPEG, PNG, WebP, GIF, TIFF, PSD and BMP.
file_scan_result scan_photo_libfiles(read_stream& s, scan_intent intent = scan_intent::index,
                                     bool want_thumbnail = false, files* decoder = nullptr);
// libfiles' rows of a structure or a listing, as the info pane's rows.
metadata_kv_list to_metadata_kv(std::vector<libfiles::structure_row>& rows);
// What a still format adds to a scan through libfiles.
struct still_scan_extras
{
	// Decompresses a JPEG XL's Brotli-compressed metadata box into at most max_size bytes, or answers
	// nothing.
	std::function<std::optional<std::vector<uint8_t>>(std::span<const uint8_t> compressed,
	                                                  std::string_view box_type, size_t max_size)> brotli;
	// Decodes a thumbnail the container holds as a coded image - a HEIF thumbnail item - which a libfiles
	// scan counts and leaves to a decoder. Run only for a caller that wants a thumbnail, before the Exif
	// one.
	std::function<void(read_stream& s, file_scan_result& result)> container_thumbnail;
};

file_scan_result scan_still_libfiles(read_stream& s, scan_intent intent, bool want_thumbnail, files* decoder,
                                     const still_scan_extras& extras);
// HEIF/AVIF and JPEG XL through libfiles. libfiles' decoder decodes a HEIF thumbnail item for a caller
// that wants one, and the Brotli a JPEG XL's compressed boxes need is supplied here.
file_scan_result scan_heif_libfiles(read_stream& s, scan_intent intent, bool want_thumbnail, files* decoder);
file_scan_result scan_jxl_libfiles(read_stream& s, scan_intent intent, bool want_thumbnail, files* decoder);

// An audio or video file as libfiles reads it, with its cover art's encoded bytes when asked for.
struct media_scan_read
{
	std::shared_ptr<const libfiles::media_scan_result> scan;
	df::blob cover_art;
};

// Reads the container through libfiles, leaving the handle at the start for a decoder opened next.
media_scan_read scan_media_file(const platform::file_ptr& f, scan_intent intent, bool want_cover_art);
// Takes the packet, cover art, tags and properties of a scan libfiles read, and its streams' facts too
// when stream_facts is set: a scan that was not complete leaves those to FFmpeg.
void apply_media_scan(file_scan_result& result, media_scan_read&& read, bool stream_facts);
// The audio sample type as a stream codes its samples: a depth for PCM and lossless audio, none for lossy.
prop::audio_sample_t coded_sample_type(unsigned bits_per_sample, bool float_samples);
// The info panel's blocks for a scan: its tags under their native names, its packet when with_xmp, and
// its structure.
void add_media_blocks(const libfiles::media_scan_result& scan, av_media_info& info, bool with_xmp);
// The info panel's streams and summary, from a scan alone.
void describe_media_streams(const libfiles::media_scan_result& scan, av_media_info& info);
// libfiles' descriptions over the rows of a session FFmpeg opened - each matched by the container's
// stream number where FFmpeg records one (stream_ids[i] is info.streams[i]'s AVStream::id), else by its
// place among the streams of its kind - and its blocks in place of FFmpeg's metadata.
void overlay_media_scan(const libfiles::media_scan_result& scan, av_media_info& info,
                        const std::vector<uint64_t>& stream_ids);
// True when an ISO 639-2 code, bibliographic or terminological, names the UI's two-letter language.
bool language_matches_ui(std::string_view iso639, std::string_view ui_language);


namespace files_test_hooks
{
	// files::decode_jpeg, which every path that decodes a JPEG takes.
	ui::surface_ptr decode_jpeg(files& ff, df::cspan data, sizei target_extent, bool can_use_yuv,
	                            decode_intent intent, bool& is_yuv);
}

class files final : df::no_copy
{
	jpeg_transformer _jpeg_transformer;
	jpeg_encoder _jpeg_encoder;

	// libfiles' decoders, which keep their codecs' state between images.
	libfiles::decoder _decoder;

	std::unique_ptr<av_scaler> _scaler{};

	file_scan_result scan_raw(df::file_path path, std::string_view xmp_sidecar, bool load_thumb, sizei max,
	                          scan_intent intent);

	// The write itself. update() wraps it so the post-write re-scan happens where the coherent
	// handle is still valid and the handle never escapes.
	platform::file_op_result update_impl(df::file_path path_src, df::file_path path_dst,
	                                     const metadata_edits& metadata_edits, const image_edits& photo_edits,
	                                     const file_encode_params& params, bool create_original,
	                                     std::string_view src_xmp_name, std::string_view dst_xmp_name,
	                                     const file_update_publish& publish, bool* live_file_maybe_changed);

	av_scaler& scaler();
	ui::surface_ptr decode_jpeg(df::cspan data, sizei target_extent, bool can_use_yuv,
	                            std::optional<ui::orientation> orientation_override, bool& is_yuv,
	                            const df::cancel_token& token, decode_intent intent);

	friend ui::surface_ptr files_test_hooks::decode_jpeg(files& ff, df::cspan data, sizei target_extent,
	                                                     bool can_use_yuv, decode_intent intent, bool& is_yuv);

	// The still_format entry for JPEG decodes through the decoder state this object holds.
	friend ui::surface_ptr decode_still_jpeg(files& ff, df::cspan data, const still_decode_request& request);

	// Shared by the two fit_within overloads; defined in files_core.cpp, where both instantiate it.
	template <typename Ptr>
	Ptr fit_within_impl(Ptr surface_in, sizei ceiling);

public:
	files();
	~files() override;

	ui::const_image_ptr surface_to_image(const ui::const_surface_ptr& surface_in, const metadata_parts& metadata,
	                                     const file_encode_params& params, ui::image_format format);
	ui::const_image_ptr surface_to_thumbnail(const ui::const_surface_ptr& surface_in);
	ui::surface_ptr image_to_surface(const ui::const_image_ptr& image, sizei scale_hint = {}, bool can_use_yuv = false,
	                                 const df::cancel_token& token = {},
	                                 decode_intent intent = decode_intent::display);
	ui::surface_ptr image_to_surface(df::cspan data, sizei scale_hint = {}, bool can_use_yuv = false,
	                                 decode_intent intent = decode_intent::display);

	// Reduces a surface to fit within a ceiling, never enlarging: a picture already inside it comes
	// back untouched, and so does one no resampler would take. Whoever asks for a ceiling wants a
	// bound on the pixels it will hold, not a size to fill.
	ui::surface_ptr fit_within(ui::surface_ptr surface_in, sizei ceiling);
	ui::const_surface_ptr fit_within(ui::const_surface_ptr surface_in, sizei ceiling);

	// A 64-bit perceptual hash of the picture, or 0 when it has too little detail to identify.
	// Reads the stored pixels, not the oriented ones, so a file whose only change is an orientation
	// tag still hashes as the same picture. Decoding at the hash's own extent is the point: a JPEG
	// scales in the DCT domain, so this costs a fraction of a full decode.
	uint64_t calc_perceptual_hash(df::cspan encoded);
	static uint64_t calc_perceptual_hash(const ui::const_surface_ptr& surface);

	// The same picture hashed in all four quarter turns, so a rotated copy can be recognised. One
	// decode and one reduction serve all four.
	crypto::phash_rotations calc_perceptual_hash_rotations(df::cspan encoded);
	static crypto::phash_rotations calc_perceptual_hash_rotations(const ui::const_surface_ptr& surface);

	// Bytes a decode must allocate before its result can be scaled down. libjpeg reduces while
	// decoding, by up to 1/8, so a JPEG never materialises its full frame; the other deferred codecs
	// build the whole thing at native size first.
	static int64_t estimate_decode_bytes(const ui::const_image_ptr& image, sizei scale_hint);
	static int64_t estimate_decode_bytes(sizei source_dimensions);

	// True when that allocation is more than df::max_decode_bytes allows on this machine.
	static bool exceeds_decode_budget(const ui::const_image_ptr& image, sizei scale_hint);
	static bool exceeds_decode_budget(sizei source_dimensions);
	ui::pixel_difference_result
	pixel_difference(const ui::const_image_ptr& expected, const ui::const_image_ptr& actual);

	bool save(df::file_path path, const file_load_result& loaded);


	static std::string_view to_string(const ui::image_format f)
	{
		switch (f)
		{
		case ui::image_format::JPEG: return "JPEG";
		case ui::image_format::PNG: return "PNG";
		case ui::image_format::WEBP: return "WEBP";
		case ui::image_format::Unknown: break;
		default: ;
		}

		return "Unknown";
	}

	static detected_format detect_format(df::cspan image_buffer_in);
	static file_type_ref file_type_from_name(df::file_path path);
	static file_type_ref file_type_from_name(std::string_view name);

	// A few media extensions are also common source extensions - .ts is both an MPEG-2 transport
	// stream and a TypeScript file. Where the container carries a signature strong enough to settle
	// it, the header answers before anything tries to decode the bytes as media. Only extensions
	// has_media_header_rule accepts are covered; everything else is left to the decoder as before.
	static constexpr size_t media_header_probe_bytes = 1024;
	static bool has_media_header_rule(std::string_view extension);
	static bool media_header_matches(std::string_view extension, df::cspan header);

	static bool can_save(df::file_path path);
	static bool can_save_extension(std::string_view ext);
	static bool is_jpeg(df::file_path path);
	static bool is_jpeg(std::string_view name);
	static bool is_raw(df::file_path path);
	static bool is_raw(std::string_view name);
	static bool is_jpeg(uint32_t header);

	// want_image asks for `full_image`: the whole encoded file wrapped for display. It costs a second
	// parse and a copy of the file, so a scan that will not display the bytes leaves it unset.
	file_scan_result scan_file(df::file_path path, bool load_thumb, file_type_ref ft,
	                           std::string_view xmp_sidecar = {}, sizei max_thumb_size = {},
	                           scan_intent intent = scan_intent::index, bool want_image = false);

	// Scan an already-open file (see files_core.cpp). Lets a just-edited file be re-scanned
	// through the coherent handle returned by replace_file, avoiding a stale SMB by-name reopen.
	file_scan_result scan_file(platform::file_ptr f, df::file_path path, bool load_thumb, file_type_ref ft,
	                           std::string_view xmp_sidecar = {}, sizei max_thumb_size = {},
	                           scan_intent intent = scan_intent::index, bool want_image = false);

	file_load_result load(df::file_path path, bool can_load_preview);

	file_update_result update(df::file_path path_src, df::file_path path_dst,
	                          const metadata_edits& metadata_edits, const image_edits& photo_edits,
	                          const file_encode_params& params, bool create_original,
	                          std::string_view src_xmp_name, std::string_view dst_xmp_name = {},
	                          const rescan_spec& rescan = {}, const file_update_publish& publish = {});

	file_update_result update(const df::file_path path, const metadata_edits& metadata_edits,
	                          const image_edits& photo_edits, const file_encode_params& params,
	                          const bool create_original, const std::string_view xmp_name,
	                          const rescan_spec& rescan = {})
	{
		return update(path, path, metadata_edits, photo_edits, params, create_original, xmp_name, xmp_name, rescan);
	}

	static std::vector<archive_item> list_archive(df::file_path zip_file_path);

	// A Commodore 64 image's listing, as libfiles lists it, drawn by libfiles into an RGB surface eight
	// pixels a character, the glyphs in fg on bg; null when there is nothing to draw. The caller scales
	// the texture to fit the space it has.
	static ui::const_surface_ptr c64_listing_surface(const libfiles::c64_listing& listing, uint32_t fg, uint32_t bg);
};
