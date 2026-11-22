// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Core file operations and format handling. Coordinates format detection, metadata extraction,
// image loading/saving, and file type classification for photos, videos, and archives.

#pragma once

#include "model_tags.h"
#include "model_location.h"
#include "model_property.h"

#include "libfiles/commodore.h"
#include "libfiles/decode.h"
#include "libfiles/encode.h"
#include "libfiles/exif.h"
#include "libfiles/properties.h"
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
struct av_edits;
class file_scan_result;
struct nfo_details;
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

	// An episode marker was found, so an episode of 0 is a special (S03E00) rather than no episode.
	bool has_episode = false;
	// The name carries a release tag - a resolution, codec, disc or broadcast source, or a frame size - so it
	// names a download rather than a recording of the user's own.
	bool is_release = false;
	// A dated episode (Show.2024.01.07) aired on this day of `year`.
	int aired_month = 0;
	int aired_day = 0;
};

using file_group_by_name = core::hash_map<std::string_view, file_group_ref, core::ihash, core::ieq>;
using file_type_by_extension = core::hash_map<std::string_view, file_type_ref, core::ihash, core::ieq>;

// Many tools can open the same extension or group, so every match is kept in declaration order.
using file_tool_by_extension = core::hash_map<std::string_view, std::vector<file_tool_ptr>, core::ihash, core::ieq>;
using file_tool_by_group = core::hash_map<std::string_view, std::vector<file_tool_ptr>, core::ihash, core::ieq>;

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
	music_metadata = 1 << 15,
	video_metadata = 1 << 16,
	photo_metadata = 1 << 17,
	edit = 1 << 18,

	av = 1 << 20,
	bitmap = 1 << 21,
	archive = 1 << 22,
	commodore = 1 << 23,
	// A tune lib64 plays: the player reads it through av_sid's WAV stream rather than opening the file.
	sid_tune = 1 << 24,
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

class file_group
{
public:
	file_group(const std::string_view name, const std::string_view plural_name, const core::color32 color,
	           const icon_index icon, const file_traits traits, const group_key_type key,
	           std::vector<std::string_view> sidecars) :
		name(name), plural_name(plural_name), key(key), color(color), icon(icon), traits(traits),
		sidecars(std::move(sidecars))
	{
	}

	std::string_view name;
	std::string_view plural_name;
	group_key_type key;
	core::color32 color;
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

	uint32_t search_presence_bit() const { return 1 << (id % 8); }

	bool has_trait(const file_traits t) const { return traits && t; }

	core::color32 text_color(const core::color32 default_text_color) const
	{
		return core::average(default_text_color, color);
	}

	operator file_group_ref() const { return this; }
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
	          const file_traits traits) : group(group), extension(extension), text(text), traits(traits)
	{
	}

	bool has_trait(const file_traits t) const { return (traits && t) || group->has_trait(t); }

	bool is_media() const { return has_trait(file_traits::bitmap) || has_trait(file_traits::av); }

	bool can_cache() const { return has_trait(file_traits::cache_metadata); }

	bool is_playable() const { return has_trait(file_traits::av); }

	bool can_edit() const { return has_trait(file_traits::edit); }

	// The single test for what the photo edit view accepts, so entry, the selector filter
	// and save-and-next navigation cannot drift apart.
	bool can_edit_photo() const { return has_trait(file_traits::bitmap) && can_edit(); }

	std::string display_name(const bool is_plural) const { return group->display_name(is_plural); }

	core::color32 text_color(const core::color32 default_text_color) const
	{
		return group->text_color(default_text_color);
	}

	core::const_surface_ptr default_thumbnail() const;

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

	operator file_type_ref() const { return this; }
};

// Comparison is like with like. Two selected files is a cardinality fact, not a claim that the
// pair can be compared, so eligibility reads stable traits only: never online status, metadata
// arrival, or panel width.
inline bool can_compare_file_types(const file_type_ref ft1, const file_type_ref ft2)
{
	if (!ft1 || !ft2) return false;

	const auto both = [ft1, ft2](const file_traits t)
	{
		return ft1->has_trait(t) && ft2->has_trait(t);
	};
	return both(file_traits::bitmap) || both(file_traits::preview_video);
}

struct file_tool
{
	core::cached exe = {};
	core::cached invoke_text = {};
	core::cached text = {};
	core::cached extensions = {};
	core::cached group = {};
	core::file_path exe_path = {};

	bool invoke(core::file_path path) const;

	bool exists() const { return !exe_path.is_empty(); }
};

// A configurable map/location service (from diffractor-tools.json). The url may
// contain {latitude} and {longitude} tokens that are substituted with the GPS
// coordinate of the focused item before the link is opened in the browser.
struct file_map_link
{
	core::cached text = {};
	core::cached url = {};
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
	libfiles::image_format format = libfiles::image_format::unknown;

	uint32_t width = 0;
	uint32_t height = 0;

	core::cached pixel_format = {};
	// The same in libfiles' one vocabulary, which tells a picture coded as RGB from one coded as YCbCr 4:4:4.
	libfiles::picture_format picture;
	core::orientation orientation = core::orientation::top_left;
	// The decoder already rotated the pixels for a transform stored in the container (HEIF 'irot'
	// / 'imir'), so `orientation` describes nothing further to do. Tracked separately for the
	// thumbnail because it is a distinct item that need not carry the same transform.
	bool orientation_applied = false;
	bool thumbnail_orientation_applied = false;
	// The picture is the first frame of an animation, so a picture written back from it would keep that
	// frame alone (libfiles::scan_result::animated).
	bool animated = false;
	// The picture's own colour description names PQ or HLG, which it is shown by in place of its ICC profile
	// (libfiles::scan_result::hdr).
	bool hdr = false;
	// The photo carries a gain map (libfiles::scan_result::gain_map).
	bool gain_map = false;
	// The H.273 code points a picture's or a video's own colour description gives its primaries and its transfer;
	// 2, unspecified, where it gives none.
	uint8_t colour_primaries = 2;
	uint8_t colour_transfer = 2;
	// A video's frames a second.
	double frame_rate = 0.0;
	// What a Commodore disk or tape holds: its files' names, one a line.
	core::cached contents = {};

	mutable core::const_image_ptr thumbnail_image;
	core::surface_ptr thumbnail_surface;
	mutable core::const_image_ptr cover_art;
	// The whole encoded file wrapped for display. Set only where the scan already read the file into
	// memory and the bytes are directly displayable (JPEG/PNG/WEBP), so it costs nothing to keep.
	core::const_image_ptr full_image;

	// The blocks as libfiles read them. Move-only: each is a full metadata buffer, so an implicit copy
	// would be a silent deep copy.
	libfiles::metadata_blocks metadata;
	std::optional<libfiles::exif_metadata> tiff_metadata;

	metadata_kv_list ffmpeg_metadata;
	metadata_kv_list libraw_metadata;
	// An audio or video file as libfiles read it: its properties and tags stand in for FFmpeg's metadata,
	// and the info panel shows its tags and structure.
	std::shared_ptr<const libfiles::media_scan_result> media;
	// What a media manager's .nfo beside a video says of it, when the user asks for those to be read.
	std::shared_ptr<const nfo_details> nfo;

	// How the file itself is assembled, as read by the format scanner rather than a metadata parser.
	metadata_kv_list structure_metadata;

	core::date_t created_utc;
	core::date_t created_local;
	prop::date_source created_local_source = prop::date_source::none;
	int iso_speed = 0;
	float exposure_time = 0;
	float f_number = 0;
	float focal_length = 0;

	core::cached comment = {};
	core::cached artist = {};
	core::cached camera_model = {};
	core::cached camera_manufacturer = {};
	core::cached video_codec = {};
	core::cached copyright_notice = {};
	core::cached title = {};
	mutable std::vector<core::cached> keywords;
	mutable std::vector<core::cached> windows_categories;

	uint32_t nb_streams = 0;
	double duration = 0.0;

	core::cached audio_codec = {};
	int audio_sample_rate = 0;
	int audio_channels = 0;
	prop::audio_sample_t audio_sample_type = prop::audio_sample_t::none;
	core::cached bitrate = {};
	mutable gps_coordinate gps;
	uint32_t crc32c = 0;
	bool success = false;

	file_scan_result() noexcept = default;
	file_scan_result(const file_scan_result&) = delete;
	file_scan_result& operator=(const file_scan_result&) = delete;
	file_scan_result(file_scan_result&&) noexcept = default;
	file_scan_result& operator=(file_scan_result&&) noexcept = default;

	sizei dimensions() const { return {static_cast<int>(width), static_cast<int>(height)}; }

	// Borrows the scanned blocks; fallback holds synthesised EXIF when nothing was scanned.
	const libfiles::metadata_blocks& save_metadata(libfiles::metadata_blocks& fallback) const;
	prop::item_metadata_ptr to_props() const;
	av_media_info to_info() const;
};

// What a block, or blocks libfiles reconciled, state, onto an item: each property stated replaces the
// item's, even empty, and each date joins the item's dates. libfiles reads each block by its one table
// and reconciles them by its one rule, libfiles::overlay; this is where its properties meet an item's.
void apply_properties(prop::item_metadata& md, const libfiles::properties& p);

// What the decoded pixels are for. `display` is anything the user looks at full size, where cheap
// chroma upsampling shows as blocky colour on saturated edges; `thumbnail` covers small previews,
// hashes and scans, where speed wins. Scale alone cannot tell these apart - a display shrunk below
// half size still deserves the better filter.
enum class decode_intent : uint8_t
{
	display,
	thumbnail,
	// For an edit to work on: as display, at sixteen bits a channel where the file holds more than eight - a
	// 16-bit PNG, a HEIF, AVIF or JPEG XL of more than eight, a raw file developed - and an HDR picture as its
	// signal, where the budget holds them at that, so the edit's resampling and colour are worked out at the
	// precision and in the range the picture has, and rounded once, when it is saved. Colours an ICC profile
	// describes stay the file's own, where display and thumbnail show them as sRGB's.
	edit
};

// What a decode for an edit found of the picture beyond its pixels: its depth and range, its gain map and its
// profile.
struct picture_signal
{
	// The pixels are an HDR picture's signal - PQ or HLG, at sixteen bits - described so, rather than the SDR
	// rendition it is shown as.
	std::optional<libfiles::hdr_description> hdr;
	// The picture holds more than a byte a channel, or is HDR, but the decode budget held it only at a byte a
	// channel, so it is edited at that.
	bool narrowed = false;
	// Narrowed from HDR: the pixels are its SDR rendition, and an edit saved over it would lose its HDR.
	bool tone_mapped = false;
	// The photo holds a gain map: a JPEG's after its primary, a HEIF's ISO 21496-1 or Apple one.
	bool gain_map = false;
	// The ICC profile the pixels' colours are in, where they are not sRGB's - a phone's Display P3, a camera's Adobe
	// RGB (1998): the edit shows and adjusts their sRGB rendition, and the save carries the change back into them,
	// the profile kept. Empty for sRGB, a profile libfiles cannot map, and an HDR picture.
	std::vector<uint8_t> profile;

	bool is_hdr() const { return hdr.has_value() || tone_mapped || gain_map; }
};

// A gain map an edit carries with its photo: its pixels, a byte a channel, standing as the photo does once
// decoded, and how it applies, in ISO 21496-1's terms.
struct gain_map_surface
{
	core::const_surface_ptr pixels;
	libfiles::gain_map_metadata metadata;
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

	core::const_surface_ptr s;
	core::const_image_ptr i;
	// For an edit's load: the picture's depth, range and gain map. An HDR picture's s is its signal, which to_surface
	// gives an edit as it is and anything else as its SDR rendition.
	picture_signal signal;

	file_load_result() noexcept = default;
	file_load_result(const file_load_result&) = default;
	file_load_result& operator=(const file_load_result&) = default;
	file_load_result(file_load_result&&) noexcept = default;
	file_load_result& operator=(file_load_result&&) noexcept = default;

	sizei dimensions() const;
	// The picture as the intent takes it: an edit's at the precision and in the range it was loaded at, and anything
	// else's at a byte a channel - an HDR picture's signal as its SDR rendition, colours a profile describes as their
	// sRGB rendition.
	core::const_surface_ptr to_surface(sizei scale_hint = {}, bool can_use_yuv = false,
	                                   const core::cancel_token& token = {},
	                                   decode_intent intent = decode_intent::display) const;
	core::pixel_difference_result calc_pixel_difference(const file_load_result& other) const;

	void clear()
	{
		s.reset();
		i.reset();
		success = false;
		reason = failure::none;
		source_dimensions = {};
		signal = {};
	}

	bool is_empty() const { return core::is_empty(s) && core::is_empty(i); }

	core::orientation orientation() const
	{
		if (success)
		{
			if (is_valid(i)) return i->orientation();
			if (is_valid(s)) return s->orientation();
		}
		return {};
	}

	bool is_jpeg() const { return success && core::is_jpeg(i); }
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
	bool jxl_lossless = false;
	// libjxl's own default, which it calls visually lossless.
	int jxl_quality = 90;
	bool avif_lossless = false;
	// 0 to 100, as libheif reads it for libaom; libfiles' default, high.
	int avif_quality = 75;
	// Bits a sample an AVIF takes from sixteen-bit pixels: 10, or 12 for a picture that had twelve.
	int avif_bit_depth = 10;
	// The surface is an HDR picture's signal, described so: AVIF, JPEG XL and PNG write the description with it.
	std::optional<libfiles::hdr_description> hdr;
	// A gain map written with the photo: by an AVIF beside it, and by a JPEG after it.
	std::optional<gain_map_surface> gain_map;

	// The JPEG an edit is being written back over, when there is one. Encoding against its own
	// quantization tables and chroma sampling leaves an untouched block quantizing to itself,
	// where the quality slider's tables would re-quantize the whole image.
	core::cspan jpeg_source;
	// A conversion: the pixels are encoded afresh with these settings wherever the result goes to a
	// new path, even into the source's own format - not copied, transformed losslessly, or encoded
	// against the source's tables. A write over the source itself is never re-encoded for it.
	bool reencode = false;
};

// What a post-write re-scan needs. files::update runs the scan itself, while the file it just wrote
// is still open, so no caller ever reopens it by name (see docs/file-io.md#coherent-read-back).
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
	libfiles::scan_intent intent = libfiles::scan_intent::index;
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
	// What a repairing copy of a damaged video or sound file left out: the damaged parts it skipped, and where it ends,
	// in seconds, when the file could be read no further - negative when it was read to its end.
	int repair_skipped = 0;
	double repair_ended_at = -1.0;
	// A repaired copy that was not put in place of its file: it leaves part of the file out, and no backup keeps it.
	bool repair_needs_backup = false;

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

extern std::function<void(core::file_path destination)> test_before_file_update_publish;

// Shared by the format scanners to build the file-structure block: a leaf row, a section heading,
// and a pass that drops the sections a given file had nothing to put in.
void add_structure_row(metadata_kv_list& kv, std::string_view key, std::string value, std::string_view shape = {},
                       std::string detail = {});

// As above, but `payload` is the segment's own bytes; the hex listing is built only once the row is
// opened, so a scan never formats a dump nobody reads.
void add_structure_bytes(metadata_kv_list& kv, std::string_view key, std::string value, std::string_view shape,
                         const uint8_t* payload, size_t payload_len);
void add_structure_section(metadata_kv_list& kv, std::string_view key, std::string_view id,
                           bool open_by_default = false);
void finish_structure_sections(metadata_kv_list& kv);

media_name_props scan_info_from_title(std::string_view name);
// A subtitle sidecar's extension, with or without its dot: srt, vtt, ass, ssa, smi, sub, idx, sup or mpl2.
bool is_subtitle_extension(std::string_view ext);
// A word a subtitle's file name adds after its video's: a language (en, eng, English, pt-BR) or a flag (forced,
// sdh, cc), as in Movie.en.forced.srt. No other word is one: Alien.Resurrection.srt is another film's.
bool is_subtitle_suffix(std::string_view word);
// Where a sidecar of `primary` goes when that file is renamed, moved or copied to `destination`: the new name,
// then whatever the sidecar's own name carries after the old one's. Movie.en.forced.srt beside Movie.mkv becomes
// New.en.forced.srt, so two sidecars that share an extension never land on one name.
core::file_path moved_sidecar_path(core::file_path primary, core::file_path destination, core::file_path sidecar);
// The name's reading, with the show and season an episode's folders give when its name leaves them out:
// Show/Season 2/S02E05.mkv, or Show.Name.S01.1080p/E05.mkv. Folder names, not paths.
media_name_props scan_info_from_path(std::string_view name, std::string_view folder, std::string_view parent_folder);
// Fills what a video's tags leave out from its name and folders, marking each value so none is stored, and
// replacing what an earlier call filled - so it runs on every load and scan, and a better reading of names needs
// no rescan. A container title or comment that is only a release name, a website or the file's name counts as
// absent. The library type decides whether the item is a film or an episode; one that is files under its release
// year or air date when its own dates name another.
void derive_name_values(prop::item_metadata& md, std::string_view name, std::string_view folder,
                        std::string_view parent_folder, prop::library_kind library);

// What a media manager - Kodi, Jellyfin, Emby - keeps about a film or an episode in the .nfo beside it. A scene
// release's .nfo shares the extension but is a text file, not XML, so it reads as nothing.
struct nfo_details
{
	enum class kind
	{
		none,
		film,
		episode,
		show,
		music_video,
	};

	kind type = kind::none;
	std::string title;
	// The show an episode belongs to; a show's own .nfo names it as its title.
	std::string show;
	std::string plot;
	std::string genre;
	std::string studio;
	int year = 0;
	// -1 when the .nfo does not say: season 0 holds a show's specials.
	int season = -1;
	int episode = -1;
	// A film's premiere or an episode's air date, at the precision the .nfo gives.
	core::date_t released;

	bool is_empty() const { return type == kind::none; }
};

// The first film, episode, show or music video an .nfo document describes.
nfo_details parse_nfo(std::string_view text);
// Whether a film is kept in a folder of its own name, as media managers lay films out: Dune (2021)/Dune.2021.mkv.
// What such a folder keeps beside the film - its movie.nfo, its poster - is the film's.
bool is_named_for_its_folder(std::string_view stem, std::string_view folder);
// What the .nfo files about a video say: its own (Movie.nfo, or a film folder's movie.nfo when the film is named
// for its folder), with the show its tvshow.nfo names, in its folder or the one above. Reads the file system.
nfo_details read_video_nfo(core::file_path video);
// Sets what an .nfo says over what the container's tags said: a media manager's details are chosen by the user,
// where a container's are often a release group's. A file's XMP is applied after, so the user's own edits win.
void apply_nfo(prop::item_metadata& md, const nfo_details& nfo);

// What a loader read from the header before it committed to decoding. Lets a caller tell "too big
// for this machine" apart from "broken", and report the size either way.
struct load_diagnostic
{
	sizei source_dimensions;
	bool over_budget = false;
};

// Records what a loader read and answers whether decoding must be refused because the frame would
// not fit in render::max_decode_bytes at bytes_per_pixel. Called before any pixels are allocated.
bool reject_over_budget_source(load_diagnostic* diagnostic, sizei source_dimensions, std::string_view format,
                               int bytes_per_pixel = 4);

// A PSD's composite image, decoded by libfiles. Throws for a file cut short, so a damaged PSD is not
// offered to ffmpeg. map_to_srgb shows colours its ICC profile describes as sRGB's.
core::surface_ptr load_psd(read_stream& s, load_diagnostic* diagnostic = nullptr, bool map_to_srgb = false);
// wide develops a raw file at sixteen bits a channel, where the budget holds it at that.
file_load_result load_raw(core::file_path path, bool can_load_preview, bool wide = false);
// wide keeps a 16-bit PNG's sixteen bits, where the budget holds it at that. With an edit's signal, an HDR PNG is kept
// as its signal too, and the signal says what was kept. map_to_srgb shows colours an iCCP profile describes as sRGB's.
core::surface_ptr load_png(core::cspan data, bool wide = false, picture_signal* edit = nullptr,
                           bool map_to_srgb = false);
// map_to_srgb shows colours an ICC profile describes as sRGB's, and a WebP in such colours is never planar.
core::surface_ptr load_webp(core::cspan data, bool can_use_yuv = false, sizei target_extent = {},
                            bool map_to_srgb = false);
// With an edit's signal, a picture of more than eight bits is decoded at sixteen and an HDR one as its signal, where
// the budget holds that, and the signal says what was kept; otherwise at a byte a channel, HDR mapped to SDR, and
// map_to_srgb shows colours its ICC profile describes as sRGB's.
core::surface_ptr load_heif(read_stream& s, load_diagnostic* diagnostic = nullptr, picture_signal* edit = nullptr,
                            bool map_to_srgb = false);
// The libheif decoder plugin HEVC goes to - "ffmpeg" where libheif carries that plugin - or null.
const char* heif_hevc_decoder_id();
// The decoder plugin a file's primary image is steered to, or null where libheif keeps its own choice.
const char* heif_primary_decoder_id(read_stream& s);
// The gain map a HEIF or AVIF holds - its ISO 21496-1 tone map's input, or Apple's - as a byte a channel, turned as its
// photo decodes; nothing when it holds none. With a probe, only whether it holds one, nothing decoded.
std::optional<gain_map_surface> load_heif_gain_map(read_stream& s, bool probe = false);
// As load_heif.
core::surface_ptr load_jxl(read_stream& s, load_diagnostic* diagnostic = nullptr, picture_signal* edit = nullptr);

// An HDR picture's signal as an SDR display shows it, as libfiles maps one: at sixteen bits a channel, or a byte.
core::surface_ptr sdr_rendition(const core::const_surface_ptr& signal, const libfiles::hdr_description& hdr, bool wide);
// Pixels in a profile's colours as an sRGB display shows them, as libfiles maps them: at sixteen bits a channel where
// they have sixteen and wide asks, else a byte. The pixels as they are where the profile needs no mapping or they
// cannot be mapped - planar, say.
core::const_surface_ptr profile_rendition(const core::const_surface_ptr& pixels, std::span<const uint8_t> profile,
                                          bool wide);
// A packed surface as libfiles reads one - RGB as bgrx8, ARGB as bgra8, RGBX16 and RGBA16 as themselves - and nothing
// for a planar one.
std::optional<libfiles::image_view> libfiles_view(const core::surface& s);
// The gain map a JPEG holds after its primary, decoded, with how it applies; nothing when it holds none, or says
// nothing a gain map applies by. Apple's is said in ISO 21496-1's terms, its headroom from the primary's maker note.
std::optional<gain_map_surface> load_jpeg_gain_map(std::span<const uint8_t> jpeg);

core::image_ptr save_png(const core::const_surface_ptr& surface_in, const libfiles::metadata_blocks& metadata,
                         const std::optional<libfiles::hdr_description>& hdr = {});
core::image_ptr save_webp(const core::const_surface_ptr& surface_in, const libfiles::metadata_blocks& metadata,
                          const file_encode_params& params);
// Written by encoder, the caller's long-lived one, or by one of its own when none is given.
core::image_ptr save_jpeg(const core::const_surface_ptr& surface_in, const libfiles::metadata_blocks& metadata,
                          const file_encode_params& encoder_params, libfiles::jpeg_encoder* encoder = nullptr);
// The pixels' orientation goes in the codestream, and the metadata in boxes, so the packet given is the
// one the file is written with. An orientation given overrides the surface's, which an edited surface
// does not hold reliably.
core::image_ptr save_jxl(const core::const_surface_ptr& surface_in, const libfiles::metadata_blocks& metadata,
                         const file_encode_params& params, std::optional<core::orientation> orientation = {});
// As save_jxl: the orientation goes in the file as the image's rotation and mirror, which a HEIF reader turns the
// picture by whatever the EXIF says, and the metadata in items of their own.
core::image_ptr save_avif(const core::const_surface_ptr& surface_in, const libfiles::metadata_blocks& metadata,
                          const file_encode_params& params, std::optional<core::orientation> orientation = {});
core::image_ptr save_surface(const core::image_format& format, const core::const_surface_ptr& surface,
                             const libfiles::metadata_blocks& metadata, const file_encode_params& params);
core::image_format extension_to_format(std::string_view ext);

// What Diffractor adds to libfiles' JPEG codecs - its decode budget for a lossless turn, its log and its
// errors - and the signatures the metadata blocks in a JPEG's markers begin with. libfiles encodes,
// decodes and turns JPEGs; its jpeg_encoder and jpeg_transformer are used directly.
inline constexpr auto xmp_signature = "http://ns.adobe.com/xap/1.0/\0"sv;
inline constexpr auto icc_signature = "ICC_PROFILE\0"sv;
inline constexpr std::array<uint8_t, 6> exif_signature = {0x45, 0x78, 0x69, 0x66, 0x00, 0x00};

bool is_exif_signature(std::span<const uint8_t> block);

inline constexpr size_t xmp_signature_len = xmp_signature.size();

// A JPEG turned losslessly, encoder writing the result. Empty when the turn cannot be lossless - it
// would land on a partial MCU - or the stream cannot be read. over_budget, when given, is set when it
// could have been but its coefficients would not fit the decode budget: a refusal a caller must not
// answer with a re-encode, which holds more still. A libjpeg failure part way is logged and thrown as an
// app_exception; both codecs stay usable.
core::blob turn_jpeg_losslessly(const libfiles::jpeg_transformer& transformer, libfiles::jpeg_encoder& encoder,
                                std::span<const uint8_t> src, simple_transform transform, bool* over_budget = nullptr);

struct pack128
{
	uint32_t u1;
	uint32_t u2;
	uint32_t u3;
	uint32_t u4;

	operator core::cspan() const { return {std::bit_cast<const uint8_t*>(this), 16}; }

	operator std::span<const uint8_t>() const { return {std::bit_cast<const uint8_t*>(this), 16}; }
};

class read_stream
{
protected:
	uint64_t _file_size = 0;

	// Overflow-safe range check. All parsing here runs over untrusted file data,
	// so pos + len must never be evaluated directly.
	void check_range(const uint64_t pos, const uint64_t len) const
	{
		if (pos > _file_size || len > _file_size - pos) throw app_exception("read past end of file"s);
	}

public:
	virtual ~read_stream() = default;

	virtual uint8_t peek8(uint64_t pos) = 0;
	virtual uint16_t peek16(uint64_t pos) = 0;
	virtual uint32_t peek32(uint64_t pos) = 0;
	virtual uint64_t peek64(uint64_t pos) = 0;
	virtual pack128 peek128(uint64_t pos) = 0;

	virtual core::blob read_all() = 0;
	virtual core::blob read(uint64_t pos, size_t len) = 0;
	virtual void read(uint64_t pos, uint8_t* buffer, size_t len) = 0;

	// As read, but bypassing any window the stream keeps, for a caller that buffers for itself.
	virtual void read_through(const uint64_t pos, uint8_t* buffer, const size_t len) { read(pos, buffer, len); }

	// Empty unless the whole stream is already resident and can be borrowed without copying.
	virtual core::cspan view() const { return {}; }

	// Borrows the whole stream when possible, otherwise loads it into owner and borrows that.
	core::cspan view_all(core::blob& owner)
	{
		const auto borrowed = view();
		if (!borrowed.empty()) return borrowed;
		owner = read_all();
		return owner;
	}

	const uint64_t size() const { return _file_size; }
};

class mem_read_stream final : public read_stream
{
	const uint8_t* const _data = nullptr;

public:
	mem_read_stream(const core::cspan cs) : _data(cs.data) { _file_size = cs.size; }

	template <typename T> T peek(const uint64_t pos)
	{
		check_range(pos, sizeof(T));
		T result;
		memcpy(&result, _data + pos, sizeof(T));
		return result;
	}

	uint8_t peek8(const uint64_t pos) override { return peek<uint8_t>(pos); }

	uint16_t peek16(const uint64_t pos) override { return peek<uint16_t>(pos); }

	uint32_t peek32(const uint64_t pos) override { return peek<uint32_t>(pos); }

	uint64_t peek64(const uint64_t pos) override { return peek<uint64_t>(pos); }

	pack128 peek128(const uint64_t pos) override { return peek<pack128>(pos); }

	void read(const uint64_t pos, uint8_t* buffer, const size_t len) override
	{
		check_range(pos, len);
		memcpy(buffer, _data + pos, len);
	}

	core::blob read(const uint64_t pos, const size_t len) override
	{
		check_range(pos, len);
		core::blob result(len);
		memcpy(result.data(), _data + pos, len);
		return result;
	}

	core::cspan view() const override { return {_data, static_cast<size_t>(_file_size)}; }

	core::blob read_all() override
	{
		core::blob result(static_cast<size_t>(_file_size));
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

	bool open(core::file_path path);
	bool open(platform::file_ptr h);
	void close();

	~file_read_stream() override;

	template <typename T> T peek(const uint64_t pos)
	{
		load_buffer(pos, sizeof(T));
		T result;
		memcpy(&result, _buffer + (pos - _loaded_start_pos), sizeof(T));
		return result;
	}

	uint8_t peek8(const uint64_t pos) override { return peek<uint8_t>(pos); }

	uint16_t peek16(const uint64_t pos) override { return peek<uint16_t>(pos); }

	uint32_t peek32(const uint64_t pos) override { return peek<uint32_t>(pos); }

	uint64_t peek64(const uint64_t pos) override { return peek<uint64_t>(pos); }

	pack128 peek128(const uint64_t pos) override { return peek<pack128>(pos); }

	void read(uint64_t pos, uint8_t* buffer, size_t len) override;
	void read_through(uint64_t pos, uint8_t* buffer, size_t len) override;

	core::blob read(const uint64_t pos, const size_t len) override
	{
		// Validate before sizing the buffer - len comes from untrusted file fields.
		check_range(pos, len);
		core::blob result(len);
		read(pos, result.data(), len);
		return result;
	}

	// Reads straight off the handle: routing the whole file through the sliding window would
	// allocate and copy it a second time.
	core::blob read_all() override;
};

// A platform file as the stream libfiles reads and writes. Every access seeks first, so the handle's
// own position is not to be shared with another reader.
class platform_file_stream final : public libfiles::writable_stream
{
	platform::file_ptr _f;

public:
	explicit platform_file_stream(platform::file_ptr f) : _f(std::move(f)) {}

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
	explicit read_stream_source(read_stream& s) : _s(s) {}

	uint64_t size() override { return _s.size(); }

	size_t read(const uint64_t offset, void* data, const size_t size) override
	{
		if (offset >= _s.size()) return 0;

		const auto n = static_cast<size_t>(std::min<uint64_t>(size, _s.size() - offset));
		_s.read_through(offset, static_cast<uint8_t*>(data), n);
		return n;
	}
};

struct codec_info final : core::no_copy
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

	image_edits(const int s) : _scale(s, s) {}

	image_edits(const sizei s) : _scale(s) {}

	image_edits(const image_edits& other) = default;
	image_edits& operator=(const image_edits& other) = default;

	friend bool operator==(const image_edits& lhs, const image_edits& rhs)
	{
		return lhs._crop == rhs._crop && lhs._scale == rhs._scale && lhs._brightness == rhs._brightness &&
		       lhs._contrast == rhs._contrast && lhs._darks == rhs._darks && lhs._vibrance == rhs._vibrance &&
		       lhs._lights == rhs._lights && lhs._midtones == rhs._midtones && lhs._saturation == rhs._saturation &&
		       lhs._perspective_horizontal == rhs._perspective_horizontal &&
		       lhs._perspective_vertical == rhs._perspective_vertical && lhs._temperature == rhs._temperature &&
		       lhs._tint == rhs._tint;
	}

	friend bool operator!=(const image_edits& lhs, const image_edits& rhs) { return !(lhs == rhs); }

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

	bool has_scale() const { return _scale.cx != 0 && _scale.cy != 0; }

	void scale(const sizei s) { _scale = s; }

	void scale(const int s)
	{
		_scale.cx = s;
		_scale.cy = s;
	}

	sizei scale() const { return _scale; }

	const quadd& crop_bounds() const { return _crop; }

	const quadd crop_bounds(const sizei size) const
	{
		if (_crop.is_empty()) return quadd(size);
		return _crop;
	}

	bool has_crop_bounds() const { return !_crop.is_empty(); }

	quadd perspective_bounds(sizei size) const;
	quadd effective_crop_bounds(sizei size) const;

	void crop_bounds(const quadd& v) { _crop = v; }

	double contrast() const { return _contrast; }

	void contrast(const double v) { _contrast = v; }

	double brightness() const { return _brightness; }

	void brightness(const double v) { _brightness = v; }

	double saturation() const { return _saturation; }

	void saturation(const double v) { _saturation = v; }

	double vibrance() const { return _vibrance; }

	void vibrance(const double v) { _vibrance = v; }

	double darks() const { return _darks; }

	void darks(const double v) { _darks = v; }

	double lights() const { return _lights; }

	void lights(const double v) { _lights = v; }

	double midtones() const { return _midtones; }

	void midtones(const double v) { _midtones = v; }

	double perspective_horizontal() const { return _perspective_horizontal; }

	void perspective_horizontal(const double v) { _perspective_horizontal = v; }

	double perspective_vertical() const { return _perspective_vertical; }

	void perspective_vertical(const double v) { _perspective_vertical = v; }

	bool has_perspective() const
	{
		return _perspective_horizontal != photo_edits_default::PerspectiveHorizontal ||
		       _perspective_vertical != photo_edits_default::PerspectiveVertical;
	}

	double temperature() const { return _temperature; }

	void temperature(const double v) { _temperature = v; }

	double tint() const { return _tint; }

	void tint(const double v) { _tint = v; }

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

	bool has_rotation() const { return rotation_angle() != 0.0; };

	double rotation_angle() const;
};

// The edits applied to a surface: perspective, straightening, crop and scale, then the colour. The
// surface itself when none of them changes it; empty when a canvas cannot be had or the token cancels.
// An HDR picture's signal, described by hdr, has its colour changed as its SDR rendition is shown changed:
// the rendition is adjusted, and the signal's light scaled to match (libfiles' carry_sdr_edit). Pixels in a
// profile's colours, likewise as their sRGB rendition: the change is carried back into them (carry_srgb_edit).
core::const_surface_ptr apply_image_edits(const core::const_surface_ptr& source, const image_edits& photo_edits,
                                          const core::cancel_token& token = {},
                                          const libfiles::hdr_description* hdr = nullptr,
                                          std::span<const uint8_t> profile = {});

// A gain map edited as its photo is - the edits that move the photo's pixels, at the map's own scale - for a photo of
// photo_extent.
std::optional<gain_map_surface> edited_gain_map(const gain_map_surface& gain_map, const image_edits& edits,
                                                sizei photo_extent);

// A change to whom a picture's faces name, written into its face regions: docs/faces.md#10-saving-names-to-files.
struct face_name_edit
{
	enum class kind : uint8_t
	{
		// Names the face at `area`: in the region there, or in a new one.
		name,
		// Takes `name` off the face at `area` - its region goes - or, with no area, out of the names the packet
		// states without one.
		remove,
		// Changes `name` to `to` wherever the packet states it.
		rename,
	};

	kind what = kind::name;
	// Where the face is, as fractions of the upright picture.
	std::optional<rectd> area;
	std::string name;
	std::string to;
};

struct face_name_edits
{
	std::vector<face_name_edit> edits;
	// The picture's pixels as stored, and the orientation it is shown in: areas are written in the stored space.
	sizei stored;
	core::orientation orientation = core::orientation::top_left;
};

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
	std::optional<core::date_t> date_original;
	std::optional<core::date_t> date_created;

	std::optional<int> season;
	std::optional<core::xy8> episode;
	std::optional<int> year;
	std::optional<int> rating;
	std::optional<std::string> label;
	std::optional<core::orientation> orientation;

	std::optional<core::xy8> track_num;
	std::optional<core::xy8> disk_num;

	tag_set add_tags;
	tag_set remove_tags;

	bool remove_rating = false;

	std::optional<gps_coordinate> location_coordinate;
	std::optional<std::string> location_place;
	std::optional<std::string> location_state;
	std::optional<std::string> location_country;

	std::optional<face_name_edits> faces;

	metadata_edits() noexcept = default;
	~metadata_edits() = default;
	metadata_edits(const metadata_edits&) = default;
	metadata_edits& operator=(const metadata_edits&) = default;
	metadata_edits(metadata_edits&&) noexcept = default;
	metadata_edits& operator=(metadata_edits&&) noexcept = default;

	void apply(libfiles::xmp::meta& meta) const;

	bool has_changes() const
	{
		return title.has_value() || copyright_notice.has_value() || copyright_creator.has_value() ||
		       copyright_source.has_value() || copyright_credit.has_value() || copyright_url.has_value() ||
		       description.has_value() || comment.has_value() || synopsis.has_value() || artist.has_value() ||
		       album.has_value() || album_artist.has_value() || genre.has_value() || show.has_value() ||
		       date_original.has_value() || date_created.has_value() || season.has_value() || episode.has_value() ||
		       year.has_value() || rating.has_value() || label.has_value() || track_num.has_value() ||
		       disk_num.has_value() || location_coordinate.has_value() || location_place.has_value() ||
		       location_state.has_value() || location_country.has_value() || orientation.has_value() ||
		       !add_tags.is_empty() || !remove_tags.is_empty() || remove_rating ||
		       (faces.has_value() && !faces->edits.empty());
	}

	// Orientation is the only edit here that changes what is drawn. Every other field leaves both the
	// decoded pixels and the transform applied to them valid, so a held image stays correct.
	bool changes_presentation() const { return orientation.has_value(); }

	friend class files;
};

struct archive_item
{
	std::string filename;
	core::file_size uncompressed_size;
	// Empty where the reader cannot tell, as libarchive's cannot.
	core::file_size compressed_size;
	// When the entry was last changed, which is the one time a zip records for it.
	core::date_t modified;
};

// What a caller decoding in-memory bytes asks for: the extent the result has to fit, whether planar
// YUV may come back, and whether it is for display or a thumbnail.
struct still_decode_request
{
	sizei target_extent;
	bool can_use_yuv = false;
	decode_intent intent = decode_intent::display;
	// The caller fits the result within target_extent: a JPEG is reduced as far as its fitted size allows, where
	// otherwise it keeps enough to cover target_extent in both dimensions.
	bool fit_within_target = false;
};

// Everything Diffractor knows about one still image format libfiles names from its first bytes: the
// file type it registers, how its header is read without decoding, and how it decodes. A format with
// no decoder of its own is decoded by ffmpeg. Supporting another is one entry in files.cpp's
// s_still_formats, plus a still_file_type line at its place in the extension table.
struct still_format
{
	libfiles::image_format format = libfiles::image_format::unknown;
	std::string_view extensions;
	std::string_view description;
	file_traits traits = file_traits::none;
	// A loaded file keeps its encoded bytes in this form rather than decoding them.
	core::image_format encoded = core::image_format::Unknown;
	// Dimensions, metadata and, when asked for, a thumbnail, read without decoding the frame.
	file_scan_result (*scan)(read_stream& s, libfiles::scan_intent intent, bool want_thumbnail,
	                         files* decoder) = nullptr;
	// Decodes a whole file for a load. The diagnostic names a source refused for its size. An edit's signal asks for
	// the picture at the precision and in the range it holds, and is told what was kept.
	core::surface_ptr (*load)(read_stream& s, load_diagnostic* diagnostic, picture_signal* edit) = nullptr;
	// Decodes in-memory bytes for display or a thumbnail.
	core::surface_ptr (*decode)(files& ff, core::cspan data, const still_decode_request& request) = nullptr;
};

std::span<const still_format> still_formats();
const still_format* find_still_format(libfiles::image_format format);
// The extension-table entry for a still format, built from its still_format.
file_type still_file_type(libfiles::image_format format);
// The JPEG entry's decoder. A friend of files, whose decoder state it uses.
core::surface_ptr decode_still_jpeg(files& ff, core::cspan data, const still_decode_request& request);

// Formats whose loaded files stay encoded: the bytes are the image.
inline bool is_image_format(const libfiles::image_format& format)
{
	const auto* const still = find_still_format(format);
	return still && still->encoded != core::image_format::Unknown;
}

core::image_ptr load_image_file(core::cspan data);
// want_thumbnail is opt-in because extracting an embedded thumbnail is not free: it reads the
// thumbnail's bytes off the file and copies them, or decodes them, none of which a metadata scan
// has any use for. decoder is the caller's own files instance, borrowed only when a thumbnail in
// an awkward format has to be decoded; a null one makes the scan build its own.
file_scan_result scan_photo(read_stream& s, libfiles::scan_intent intent = libfiles::scan_intent::index,
                            bool want_thumbnail = false, files* decoder = nullptr);
// The scan of a still libfiles reads with nothing added: JPEG, PNG, WebP, GIF, TIFF, PSD and BMP.
file_scan_result scan_photo_libfiles(read_stream& s, libfiles::scan_intent intent = libfiles::scan_intent::index,
                                     bool want_thumbnail = false, files* decoder = nullptr);
// libfiles' rows of a structure or a listing, as the info pane's rows.
metadata_kv_list to_metadata_kv(std::vector<libfiles::structure_row>& rows);
// What a still format adds to a scan through libfiles.
struct still_scan_extras
{
	// Decompresses a JPEG XL's Brotli-compressed metadata box into at most max_size bytes, or answers
	// nothing.
	std::function<std::optional<std::vector<uint8_t>>(std::span<const uint8_t> compressed, std::string_view box_type,
	                                                  size_t max_size)>
		brotli;
	// Decodes a thumbnail the container holds as a coded image - a HEIF thumbnail item - which a libfiles
	// scan counts and leaves to a decoder. Run only for a caller that wants a thumbnail, before the Exif
	// one.
	std::function<void(read_stream& s, file_scan_result& result)> container_thumbnail;
};

file_scan_result scan_still_libfiles(read_stream& s, libfiles::scan_intent intent, bool want_thumbnail, files* decoder,
                                     const still_scan_extras& extras);
// HEIF/AVIF and JPEG XL through libfiles, including a HEIF thumbnail item and compressed JPEG XL boxes.
file_scan_result scan_heif_libfiles(read_stream& s, libfiles::scan_intent intent, bool want_thumbnail, files* decoder);
file_scan_result scan_jxl_libfiles(read_stream& s, libfiles::scan_intent intent, bool want_thumbnail, files* decoder);
// An audio or video file as libfiles reads it, with its cover art's encoded bytes when asked for.
struct media_scan_read
{
	std::shared_ptr<const libfiles::media_scan_result> scan;
	core::blob cover_art;
};

// The ID3 picture type a file marks its front cover with. ID3 and FLAC type every picture they carry, and FFmpeg
// names the type in an attached picture's comment.
constexpr int id3_front_cover = 3;

// Whether a picture a file carries, by its ID3 picture type, shows the recording and so can be its cover art. A
// file's icons do not, and nor does "a bright coloured fish": the type a store embeds its drawing of the track's
// waveform under, so that players do not take it for the cover.
bool is_cover_art_picture_type(int id3_type);

// Reads the container through libfiles, leaving the handle at the start for a decoder opened next.
media_scan_read scan_media_file(const platform::file_ptr& f, libfiles::scan_intent intent, bool want_cover_art);
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
	core::surface_ptr decode_jpeg(files& ff, core::cspan data, sizei target_extent, bool can_use_yuv,
	                              decode_intent intent, bool& is_yuv);
}

class files final : core::no_copy
{
	// libfiles' JPEG codecs, long-lived because setting libjpeg up is not free; each use resets them.
	libfiles::jpeg_transformer _jpeg_transformer;
	libfiles::jpeg_encoder _jpeg_encoder;

	// libfiles' decoders, which keep their codecs' state between images.
	libfiles::decoder _decoder;

	std::unique_ptr<av_scaler> _scaler{};

	file_scan_result scan_raw(core::file_path path, std::string_view xmp_sidecar, bool load_thumb, sizei max,
	                          libfiles::scan_intent intent);

	// The scan of a still held whole in memory. Takes the bytes because a still kept encoded becomes its own
	// thumbnail and full image without a copy.
	file_scan_result scan_still_bytes(core::blob data, libfiles::scan_intent intent, bool load_thumb, bool want_image,
	                                  sizei max_thumb_size);

	// The write itself. update() wraps it so the post-write re-scan happens where the coherent
	// handle is still valid and the handle never escapes. An audio or video copy is staged from the
	// source's packets by av_remux, and then given the source's metadata, with the edits as their own;
	// what a repairing copy left out is told to `av_report`.
	platform::file_op_result update_impl(core::file_path path_src, core::file_path path_dst,
	                                     const metadata_edits& metadata_edits, const image_edits& photo_edits,
	                                     const file_encode_params& params, bool create_original,
	                                     std::string_view src_xmp_name, std::string_view dst_xmp_name,
	                                     const file_update_publish& publish, bool* live_file_maybe_changed,
	                                     const av_edits* av = nullptr, const core::cancel_token* cancel = nullptr,
	                                     file_update_result* av_report = nullptr);

	// What every write does once it is down: the re-scan through the handle that wrote it.
	void finish_update(file_update_result& result, bool live_file_maybe_changed, core::file_path path_dst,
	                   std::string_view dst_xmp_name, const rescan_spec& rescan);

	// The JPEG an edit wrote of source, with source's gain map after it, edited as the photo was; or, when
	// source has none or it cannot be edited, saying none follows. A gain map is the photo's own frame at a
	// resolution of its own, so what moves the photo's pixels moves its pixels the same way, at its scale;
	// the photo's colour edits change what it scales, not it. turn is the lossless turn the photo took,
	// which the gain map takes losslessly too where its blocks allow. The edits are read against
	// photo_extent, the photo's stored frame.
	core::blob with_edited_gain_map(std::span<const uint8_t> edited, std::span<const uint8_t> source,
	                                const image_edits& edits, sizei photo_extent, simple_transform turn);
	std::vector<uint8_t> edit_gain_map(std::span<const uint8_t> gain_map, const image_edits& edits, sizei photo_extent,
	                                   simple_transform turn);
	// A JPEG with a gain map from another format, or one made for an HDR picture, attached after it as Ultra HDR
	// writes one: encoded, given its metadata in Adobe's and ISO 21496-1's schemes, and signalled in the primary. The
	// primary as it was, the gain map left behind, when that cannot be done.
	core::blob with_gain_map(std::span<const uint8_t> primary, const gain_map_surface& gain_map);
	// A gain map encoded as a JPEG holds one, bare of what makes it a gain map, against the tables of the JPEG it
	// was where tables are given.
	std::vector<uint8_t> encode_gain_map(const core::const_surface_ptr& map, std::span<const uint8_t> tables);

	av_scaler& scaler();
	core::surface_ptr decode_jpeg(core::cspan data, sizei target_extent, bool can_use_yuv,
	                              std::optional<core::orientation> orientation_override, bool& is_yuv,
	                              const core::cancel_token& token, decode_intent intent, bool fit_within_target = false);

	friend core::surface_ptr files_test_hooks::decode_jpeg(files& ff, core::cspan data, sizei target_extent,
	                                                       bool can_use_yuv, decode_intent intent, bool& is_yuv);

	// The still_format entry for JPEG decodes through the decoder state this object holds.
	friend core::surface_ptr decode_still_jpeg(files& ff, core::cspan data, const still_decode_request& request);

	// Shared by the two fit_within overloads; defined in files.cpp, where both instantiate it.
	template <typename Ptr> Ptr fit_within_impl(Ptr surface_in, sizei ceiling);

public:
	files();
	~files() override;

	core::const_image_ptr surface_to_image(const core::const_surface_ptr& surface_in,
	                                       const libfiles::metadata_blocks& metadata, const file_encode_params& params,
	                                       core::image_format format);
	core::const_image_ptr surface_to_thumbnail(const core::const_surface_ptr& surface_in);
	core::surface_ptr image_to_surface(const core::const_image_ptr& image, sizei scale_hint = {},
	                                   bool can_use_yuv = false, const core::cancel_token& token = {},
	                                   decode_intent intent = decode_intent::display);
	core::surface_ptr image_to_surface(core::cspan data, sizei scale_hint = {}, bool can_use_yuv = false,
	                                   decode_intent intent = decode_intent::display);
	// In-memory bytes decoded as the request asks; image_to_surface asks for the target alone.
	core::surface_ptr decode_still(core::cspan data, const still_decode_request& request);

	// Reduces a surface to fit within a ceiling, never enlarging: a picture already inside it comes
	// back untouched, and so does one no resampler would take. Whoever asks for a ceiling wants a
	// bound on the pixels it will hold, not a size to fill.
	core::surface_ptr fit_within(core::surface_ptr surface_in, sizei ceiling);
	core::const_surface_ptr fit_within(core::const_surface_ptr surface_in, sizei ceiling);

	// A 64-bit perceptual hash of the picture, or 0 when it has too little detail to identify.
	// Reads the stored pixels, not the oriented ones, so a file whose only change is an orientation
	// tag still hashes as the same picture. Decoding at the hash's own extent is the point: a JPEG
	// scales in the DCT domain, so this costs a fraction of a full decode.
	uint64_t calc_perceptual_hash(core::cspan encoded);
	static uint64_t calc_perceptual_hash(const core::const_surface_ptr& surface);

	// The same picture hashed in all four quarter turns, so a rotated copy can be recognised. One
	// decode and one reduction serve all four.
	crypto::phash_rotations calc_perceptual_hash_rotations(core::cspan encoded);
	static crypto::phash_rotations calc_perceptual_hash_rotations(const core::const_surface_ptr& surface);

	// Bytes a decode must allocate before its result can be scaled down. libjpeg reduces while
	// decoding, by up to 1/8, so a JPEG never materialises its full frame; the other deferred codecs
	// build the whole thing at native size first.
	static int64_t estimate_decode_bytes(const core::const_image_ptr& image, sizei scale_hint);
	static int64_t estimate_decode_bytes(sizei source_dimensions, int bytes_per_pixel = 4);

	// True when that allocation is more than render::max_decode_bytes allows on this machine.
	static bool exceeds_decode_budget(const core::const_image_ptr& image, sizei scale_hint);
	static bool exceeds_decode_budget(sizei source_dimensions, int bytes_per_pixel = 4);
	core::pixel_difference_result pixel_difference(const core::const_image_ptr& expected,
	                                               const core::const_image_ptr& actual);

	bool save(core::file_path path, const file_load_result& loaded);

	static std::string_view to_string(const core::image_format f)
	{
		switch (f)
		{
		case core::image_format::JPEG: return "JPEG";
		case core::image_format::PNG: return "PNG";
		case core::image_format::WEBP: return "WEBP";
		case core::image_format::JXL: return "JXL";
		case core::image_format::AVIF: return "AVIF";
		case core::image_format::Unknown: break;
		default:;
		}

		return "Unknown";
	}

	static file_type_ref file_type_from_name(core::file_path path);
	static file_type_ref file_type_from_name(std::string_view name);

	// A few media extensions are also common source extensions - .ts is both an MPEG-2 transport
	// stream and a TypeScript file. Where the container carries a signature strong enough to settle
	// it, the header answers before anything tries to decode the bytes as media. Only extensions
	// has_media_header_rule accepts are covered; everything else is left to the decoder as before.
	static constexpr size_t media_header_probe_bytes = 1024;
	static bool has_media_header_rule(std::string_view extension);
	static bool media_header_matches(std::string_view extension, core::cspan header);

	// Whether pixels can be written to a file of this extension: JPEG, PNG, WebP and JPEG XL. A file
	// is rewritten in place only where its format is also one Diffractor edits.
	static bool can_save(core::file_path path);
	static bool can_save_extension(std::string_view ext);
	// Whether Rotate turns the file in place: one whose pixels Diffractor saves, or a HEIF or JPEG XL, whose own
	// structure keeps the turn.
	static bool can_turn(core::file_path path);
	// Whether an edit of a picture of these dimensions is saved over the file itself: any edit in a format Diffractor
	// writes, and quarter turns alone in one that keeps a turn. Anything else is saved as a JPEG of its own.
	static bool saves_in_place(core::file_path path, const image_edits& edits, sizei dimensions);
	// Whether a save of these edits over the file leaves its pixels as they are: quarter turns alone, of a HEIF or a
	// JPEG XL whose own structure keeps them. Reads a JPEG XL's header.
	static bool turns_in_place(core::file_path path, const image_edits& edits, sizei dimensions);
	static bool is_jpeg(core::file_path path);
	static bool is_jpeg(std::string_view name);
	static bool is_raw(core::file_path path);
	static bool is_raw(std::string_view name);
	static bool is_jpeg(uint32_t header);

	// want_image asks for `full_image`: the whole encoded file wrapped for display. A still read whole is
	// wrapped as read, so it costs holding the bytes; a scan that will not display them leaves it unset.
	file_scan_result scan_file(core::file_path path, bool load_thumb, file_type_ref ft,
	                           std::string_view xmp_sidecar = {}, sizei max_thumb_size = {},
	                           libfiles::scan_intent intent = libfiles::scan_intent::index, bool want_image = false);

	// Scan an already-open file (see files.cpp). Lets a just-edited file be re-scanned
	// through the coherent handle returned by replace_file, avoiding a stale SMB by-name reopen.
	file_scan_result scan_file(const platform::file_ptr& f, core::file_path path, bool load_thumb, file_type_ref ft,
	                           std::string_view xmp_sidecar = {}, sizei max_thumb_size = {},
	                           libfiles::scan_intent intent = libfiles::scan_intent::index, bool want_image = false);

	// The bytes of a still that a thumbnail scan reads whole, read on their own so that reading is the only
	// file access the scan makes: scan_thumbnail_source needs no file, so a caller can keep every read on one
	// thread and decode on others. Empty when the file is not such a still, cannot be read whole, or is past
	// the size scan_file reads whole - which reads it in pieces instead, for its metadata alone.
	static core::blob read_thumbnail_source(core::file_path path, file_type_ref ft);
	// scan_file's thumbnail scan of a still, from the bytes read_thumbnail_source read.
	file_scan_result scan_thumbnail_source(core::blob data, sizei max_thumb_size);

	// An edit's intent develops a raw file at sixteen bits a channel; a file kept encoded is decoded at the
	// intent its to_surface asks for.
	file_load_result load(core::file_path path, bool can_load_preview, decode_intent intent = decode_intent::display);

	file_update_result update(core::file_path path_src, core::file_path path_dst, const metadata_edits& metadata_edits,
	                          const image_edits& photo_edits, const file_encode_params& params, bool create_original,
	                          std::string_view src_xmp_name, std::string_view dst_xmp_name = {},
	                          const rescan_spec& rescan = {}, const file_update_publish& publish = {});

	file_update_result update(const core::file_path path, const metadata_edits& metadata_edits,
	                          const image_edits& photo_edits, const file_encode_params& params,
	                          const bool create_original, const std::string_view xmp_name,
	                          const rescan_spec& rescan = {})
	{
		return update(path, path, metadata_edits, photo_edits, params, create_original, xmp_name, xmp_name, rescan);
	}

	// Edit's lossless audio and video edits, written as update() writes a photo's: the copy is staged beside
	// the destination, given the source's metadata - its packet, with the rating and tags in `carried`, which
	// a format keeps besides its packet too - kept as a backup when asked, swapped in and re-scanned. A
	// cancelled copy reports CANCELLED and leaves nothing behind. An MP4 asked for its index first, or whose
	// source had it there, has it moved there last of all, where no metadata update can move it again. A repaired
	// copy that left part of the file out replaces it only when a backup keeps the file.
	file_update_result update_av(core::file_path path_src, core::file_path path_dst, const av_edits& edits,
	                             const metadata_edits& carried, bool create_original, std::string_view src_xmp_name,
	                             std::string_view dst_xmp_name, const rescan_spec& rescan,
	                             const core::cancel_token& cancel);

	static std::vector<archive_item> list_archive(core::file_path zip_file_path);

	// A Commodore 64 image's listing, as libfiles lists it, drawn by libfiles into an RGB surface eight
	// pixels a character, the glyphs in fg on bg; null when there is nothing to draw. The caller scales
	// the texture to fit the space it has.
	static core::const_surface_ptr c64_listing_surface(const libfiles::c64_listing& listing, uint32_t fg, uint32_t bg);
	// A Commodore 64 image's listing as its properties: the name it gives itself as its title, and the names of
	// the files it holds as its contents, so a search finds a game by a file on its disk.
	static file_scan_result scan_c64_media(core::cspan data);
	// A .sid tune's header as its properties: title, author, release, tunes and chip.
	static file_scan_result scan_sid(core::cspan data);
};
