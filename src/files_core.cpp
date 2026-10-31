// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Core file handling operations. Manages file loading, saving, and format detection
// for images and media files. Coordinates metadata extraction across different file types.

#include "pch.h"

#include "app_text.h"
#include "files.h"
#include "metadata_exif.h"
#include "metadata_xmp.h"
#include "av_format.h"
#include "metadata_icc.h"
#include "metadata_iptc.h"
#include "libfiles/media.h"

#define LIBARCHIVE_STATIC
#include <archive.h>
#include <archive_entry.h>

// windows.h arrives through the vendored headers above and brings the min/max function macros with
// it, which swallow the std:: calls further down this file. Both go, not just min.
#undef GetObject
#undef min
#undef max


df_assert_move_only(file_scan_result);
df_assert_move_only(metadata_parts);
df_assert_movable(metadata_kv_list);
df_assert_movable(file_load_result);


std::string file_group::display_name(const bool is_plural) const
{
	const auto result = is_plural && !str::is_empty(plural_name) ? plural_name : name;
	return tt.translate_text(std::string(result));
}

ui::const_surface_ptr file_type::default_thumbnail() const
{
	return platform::create_icon_surface(static_cast<char32_t>(group->icon));
}

struct file_type_config
{
	// Replaced whole, never edited in place. parse_file_group reads it on workers - auto-complete and
	// query - while a language switch registers that language's spellings on the UI thread, and an
	// insert that rehashed under a lookup left the reader walking freed buckets.
	std::atomic<std::shared_ptr<const file_group_by_name>> groups_by_name{
		std::make_shared<const file_group_by_name>()
	};
	file_type_by_extension types_by_name;

	std::vector<file_group_ref> groups;
	std::vector<file_type> types;
};

static file_type_config s_config;

bool file_tool::invoke(const df::file_path path) const
{
	if (exe_path.is_empty()) return false;

	auto substitute = [&](std::ostringstream& result, const std::string_view token)
	{
		if (token == "item-path") result << str::quote_if_white_space(path.str());
		else if (token == "exe-path") result << str::quote_if_white_space(exe_path.str());
	};

	return platform::run(exe_path, replace_tokens(invoke_text, substitute));
}

file_group_ref file_group_from_index(const int from_id)
{
	return s_config.groups[from_id];
}

file_group_ref parse_file_group(const std::string& text)
{
	const auto groups = s_config.groups_by_name.load();
	if (!groups) return nullptr;

	const auto found = groups->find(text);
	return found != groups->end() ? found->second : nullptr;
}

// `@photo` is not the only spelling that reaches parse_file_group: format_term writes the group's
// translated display name, and the search vocabulary offers the plural. A term that cannot be read
// back is dropped silently, so an address box the user can read has to be one the parser accepts.
//
// They are registered here rather than compared per call because parse_file_group runs on the
// auto-complete worker once per candidate per keystroke, and display_name reads the translation
// catalog, which the language switch replaces wholesale. A canonical name always wins a collision.
// The table is copied, extended and published whole, because that worker may be reading it.
void register_file_group_spellings()
{
	auto groups = std::make_shared<file_group_by_name>(*s_config.groups_by_name.load());

	for (const auto g : s_config.groups)
	{
		const auto add = [g, &groups](const std::string_view name)
		{
			if (!str::is_empty(name)) groups->try_emplace(str::cache(name), g);
		};

		add(g->plural_name);
		add(g->display_name(false));
		add(g->display_name(true));
	}

	s_config.groups_by_name.store(std::shared_ptr<const file_group_by_name>(std::move(groups)));
}

static constexpr file_traits photo_traits = file_traits::bitmap | file_traits::cache_metadata |
	file_traits::zoom | file_traits::hide_overlays |
	file_traits::thumbnail | file_traits::photo_metadata;
static constexpr file_traits video_traits = file_traits::av | file_traits::preview_video |
	file_traits::cache_metadata | file_traits::hide_overlays |
	file_traits::thumbnail | file_traits::video_metadata;
static constexpr file_traits audio_traits = file_traits::av | file_traits::visualize_audio |
	file_traits::cache_metadata | file_traits::music_metadata;

static constexpr file_traits commodore_traits = file_traits::commodore | file_traits::no_metadata_grouping;
static constexpr file_traits archive_traits = file_traits::archive | file_traits::no_metadata_grouping;

file_group file_group::other("other", "others", 0x5E5E5E, icon_index::document,
                             file_traits::no_metadata_grouping, group_key_type::other, {});
file_group file_group::folder("folder", "folders", 0x18A59C, icon_index::folder,
                              file_traits::no_metadata_grouping, group_key_type::folder, {});
file_group file_group::photo("photo", "photos", 0x18A549, icon_index::photo, photo_traits,
                             group_key_type::photo, {"xmp"});
file_group file_group::video("video", "videos", 0xA55018, icon_index::video, video_traits,
                             group_key_type::video,
                             {"srt", "smi", "vtt", "mpl2", "thm", "xmp"});
file_group file_group::audio("audio", {}, 0xA5184B, icon_index::audio, audio_traits, group_key_type::audio, {});

file_group file_group::archive("archive", "archives", 0x5588DD, icon_index::archive, archive_traits,
                               group_key_type::archive, {});
file_group file_group::commodore("commodore", {}, 0xFF8811, icon_index::retro, commodore_traits,
                                 group_key_type::retro, {});


file_type file_type::folder(file_group::folder, {}, {}, {});
file_type file_type::other(file_group::other, {}, {}, {});

void load_file_types()
{
	s_config.groups = {
		file_group::folder,
		file_group::photo,
		file_group::video,
		file_group::audio,
		file_group::archive,
		file_group::commodore,
		file_group::other
	};

	s_config.types = {
		{file_group::video, "264", {}, {}},
		{file_group::video, "265", {}, {}},
		{file_group::video, "302", {}, {}},
		{file_group::photo, "3fr", "Hasselblad raw", file_traits::raw | file_traits::edit},
		{file_group::audio, "669", {}, {}},
		{file_group::video, "722", {}, {}},
		{file_group::video, "A64", {}, {}},
		{file_group::video, "aa", {}, {}},
		{file_group::video, "aa3", {}, {}},
		{file_group::audio, "aax", "Audible Audiobook", {}},
		{file_group::audio, "aac,adt,adts", "Advanced Audio Coding", {}},
		{file_group::video, "ac3", "Dolby Digital (AC-3)", {}},
		{file_group::video, "ac4", "Dolby AC-4", {}},
		{file_group::video, "acm", {}, {}},
		{file_group::video, "adf", {}, {}},
		{file_group::video, "adp", {}, {}},
		{file_group::video, "ads", {}, {}},
		{file_group::video, "adx", {}, {}},
		{file_group::video, "aea", {}, {}},
		{file_group::audio, "afc", {}, {}},
		{file_group::audio, "aif, aifc, aiff", "Audio Interchange File Format", file_traits::embedded_xmp},
		{file_group::audio, "au,snd", "Sun Microsystems and NeXT audio", {}},
		{file_group::video, "aix", {}, {}},
		{file_group::audio, "amf", {}, {}},
		{file_group::video, "amr", "Adaptive Multi-Rate audio", {}},
		{file_group::audio, "ams", {}, {}},
		{file_group::audio, "ape", "Monkey's Audio (APE)", {}},
		{file_group::video, "apl", {}, {}},
		{file_group::video, "apng", {}, {}},
		{file_group::video, "apv", "Advanced Professional Video", {}},
		{file_group::video, "aptx", {}, {}},
		{file_group::video, "aptxhd", {}, {}},
		{file_group::video, "aqt", {}, {}},
		{file_group::photo, "arw,arq,sr2,srf", "Sony raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "bay", "Casio raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "cap", "Phase One raw", file_traits::raw | file_traits::edit},
		{
			file_group::video, "asf", "Windows Media",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{file_group::video, "ass", {}, {}},
		{file_group::video, "ast", "Audio Stream", {}},
		{
			file_group::video, "avi", "Audio Visual Interleave",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{file_group::video, "avr", {}, {}},
		{file_group::video, "avs", {}, {}},
		{file_group::video, "avs2", {}, {}},
		{file_group::video, "bcstm", {}, {}},
		{file_group::video, "bfstm", {}, {}},
		still_file_type(detected_format::BMP),
		{file_group::video, "bmv", {}, {}},
		{file_group::video, "brstm", "Binary Revolution Stream", {}},
		{file_group::video, "caf", {}, {}},
		{file_group::video, "cavs", {}, {}},
		{file_group::video, "cdata", {}, {}},
		{file_group::video, "cdg", {}, {}},
		{file_group::video, "cif", {}, {}},
		{file_group::video, "cpk", {}, {}},
		{file_group::photo, "crw,cr2,cr3", "Canon raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "cs1,sti,ia", "Sinar raw", file_traits::raw | file_traits::edit},
		{file_group::video, "cdxl", "Commodore CDXL video", {}},
		{file_group::video, "daud", {}, {}},
		{file_group::video, "dav", {}, {}},
		{file_group::audio, "dbm", {}, {}},
		{file_group::audio, "cda", "CD Audio Track", {}},
		{file_group::photo, "dc2,dcr,dcs,drf,k25,kc2,kdc", "Kodak raw", file_traits::raw | file_traits::edit},
		{file_group::audio, "dff", {}, {}},
		{file_group::audio, "digi", {}, {}},
		{file_group::video, "divx", "DivX video", {}},
		{file_group::audio, "dmf", {}, {}},
		{
			file_group::photo, "dng", "Adobe Digital Negative",
			file_traits::raw | file_traits::edit | file_traits::embedded_xmp | file_traits::in_place_metadata
		},
		{file_group::video, "dnxhd", {}, {}},
		{file_group::video, "dnxhr", {}, {}},
		{file_group::video, "drc", {}, {}},
		{file_group::audio, "dsf", {}, {}},
		{file_group::audio, "dsm", {}, {}},
		{file_group::video, "dss", {}, {}},
		{file_group::video, "dtk", {}, {}},
		{file_group::audio, "dtm", {}, {}},
		{file_group::video, "dts", "DTS (sound system)", {}},
		{file_group::video, "dtshd", "DTS (sound system)", {}},
		{file_group::video, "dv,dif", {}, {}},
		{file_group::video, "dvd", {}, {}},
		{file_group::video, "eac3,ec3", "Dolby Digital Plus (E-AC-3)", {}},
		{file_group::photo, "erf", "Epson raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "mfw", "Mamiya raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "raw", "Panasonic or Leica raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "exr", {}, {}},
		{file_group::video, "fap", {}, {}},
		{file_group::audio, "far", {}, {}},
		{file_group::photo, "fff", "Hasselblad or Imacon raw", file_traits::raw | file_traits::edit},
		{file_group::video, "fits", {}, {}},
		{file_group::audio, "flac", "Free Lossless Audio", {}},
		{file_group::video, "flm", {}, {}},
		{
			file_group::video, "flv", "Flash video",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{file_group::video, "fsb", {}, {}},
		{file_group::video, "fwse", {}, {}},
		{file_group::video, "g722", {}, {}},
		{file_group::video, "g723_1", {}, {}},
		{file_group::video, "g728", {}, {}},
		{file_group::video, "g729", {}, {}},
		{file_group::audio, "gdm", {}, {}},
		{file_group::video, "genh", {}, {}},
		still_file_type(detected_format::GIF),
		{file_group::video, "gsm", "GSM Full Rate", {}},
		{file_group::video, "gxf", {}, {}},
		{file_group::video, "h261", "H.261 video", {}},
		{file_group::video, "h263", "H.263 video", {}},
		{file_group::video, "h264", "H.264 / AVC video", {}},
		{file_group::video, "h265", "H.265 / HEVC video", {}},
		{file_group::video, "hca", {}, {}},
		{file_group::photo, "hdr", {}, {}},
		{file_group::video, "hevc", "H.265 / HEVC video", {}},
		{file_group::video, "evc", "MPEG-5 EVC", {}},
		{file_group::audio, "ice", {}, {}},
		{file_group::photo, "ico", "Microsoft Windows icon", {}},
		{file_group::audio, "id3", {}, {}},
		{file_group::video, "idf", {}, {}},
		{file_group::photo, "iff", "ILBM", {}},
		{file_group::video, "ifv", {}, {}},
		{file_group::photo, "iiq", "Phase One raw", file_traits::raw | file_traits::edit},
		{file_group::audio, "imf", {}, {}},
		{file_group::video, "ircam", {}, {}},
		{file_group::video, "ism", {}, {}},
		{file_group::video, "isma", {}, {}},
		{file_group::video, "ismv", {}, {}},
		{file_group::audio, "it", {}, {}},
		{file_group::video, "ivf", "Indeo Video Technology", {}},
		{file_group::video, "ivr", {}, {}},
		{file_group::audio, "j2b", {}, {}},
		{file_group::photo, "j2c", {}, {}},
		{file_group::photo, "j2k", {}, {}},
		{file_group::photo, "jif", {}, {}},
		{file_group::video, "jls", {}, {}},
		{file_group::photo, "jp2", "JPEG2000", {}},
		still_file_type(detected_format::JPEG),
		// Its metadata goes to a sidecar: nothing Diffractor uses writes a JPEG 2000 packet, and its own
		// scan would not read one back.
		{file_group::photo, "jpx", "Jpeg 2000", file_traits::edit},
		still_file_type(detected_format::JXL),
		{file_group::video, "jss", {}, {}},
		{file_group::photo, "koa", {}, {}},
		{file_group::video, "kux", {}, {}},
		{file_group::video, "latm", {}, {}},
		{file_group::video, "lbc", {}, {}},
		{file_group::photo, "lbm", "ILBM", {}},
		{file_group::video, "ljpg", {}, {}},
		{file_group::video, "loas", {}, {}},
		{file_group::video, "lrc", {}, {}},
		{file_group::video, "lrv", {}, {}},
		{file_group::video, "lvf", {}, {}},
		{file_group::audio, "m15", {}, {}},
		{file_group::audio, "mid,midi,rmi", "Musical Instrument Digital Interface", {}},
		{file_group::video, "m2p", {}, {}},
		{file_group::video, "m2t", {}, {}},
		{file_group::video, "m2ts", "MPEG-2 TS video", {}},
		{file_group::video, "mac", {}, {}},
		{file_group::photo, "mdc,mrw", "Minolta raw", file_traits::raw | file_traits::edit},
		{file_group::audio, "mdl", {}, {}},
		{file_group::audio, "med", {}, {}},
		{file_group::photo, "mef", "Mamiya raw", file_traits::raw | file_traits::edit},
		{file_group::video, "mj2", {}, {}},
		{file_group::video, "mjpeg", {}, {}},
		{file_group::video, "mjpg", {}, {}},
		{file_group::video, "mk3d", {}, {}},
		{file_group::audio, "mka", {}, {}},
		{file_group::video, "mks", {}, {}},
		{file_group::video, "mkv", "Matroska video", {}},
		{file_group::video, "mlp", {}, {}},
		{file_group::audio, "mmcmp", {}, {}},
		{file_group::video, "mmf", {}, {}},
		{file_group::audio, "mms", {}, {}},
		{file_group::photo, "mng", "Multiple Network Graphics animation", {}},
		{file_group::audio, "mo3", {}, {}},
		{file_group::audio, "mod", {}, {}},
		{file_group::photo, "mos", "Leaf raw", file_traits::raw | file_traits::edit},
		{
			file_group::audio, "mp3", "MPEG Layer 3",
			file_traits::embedded_xmp | file_traits::edit | file_traits::thumbnail | file_traits::in_place_metadata
		},
		{
			file_group::video, "mov", {},
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{
			file_group::video, "mp4,mp4a,mp4v,m4v,m4b,f4v,3g2,3gp2,3gp,3gpp,crm", "MPEG-4",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{
			file_group::video, "crm", "Canon Cinema RAW Light",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{
			file_group::audio, "m4a,mp4a,m4r", "MPEG-4 Audio",
			file_traits::embedded_xmp | file_traits::edit | file_traits::thumbnail |
			file_traits::in_place_metadata
		},
		{
			file_group::audio, "m4p", "MPEG-4 (DRM)",
			file_traits::embedded_xmp | file_traits::edit | file_traits::thumbnail |
			file_traits::in_place_metadata
		},
		{file_group::audio, "mpc", "Musepack", {}},
		{file_group::video, "mpd", {}, {}},
		{
			file_group::video, "mpeg,mpg,mpe,m1v,m2v,mp2,mpv,m2p,m2t,vob,ms-pvr,dvr-ms", "MPEG",
			file_traits::edit
		},
		{
			file_group::audio, "mpa,m2a", "MPEG",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		still_file_type(detected_format::HEIF),
		{file_group::video, "avc1", "Advanced Video Coding", {}},
		{file_group::audio, "mptm", {}, {}},
		{file_group::video, "msbc", {}, {}},
		{file_group::video, "msf", {}, {}},
		{file_group::audio, "mt2", {}, {}},
		{file_group::video, "mtaf", {}, {}},
		{file_group::audio, "mtm", {}, {}},
		{file_group::video, "mts", "AVCHD video", {}},
		{file_group::video, "musx", {}, {}},
		{file_group::video, "mvi", {}, {}},
		{file_group::video, "mxf", "SMPTE Material Exchange Format", {}},
		{file_group::video, "mxg", {}, {}},
		{file_group::photo, "nef,nrw", "Nikon raw", file_traits::raw | file_traits::edit},
		{file_group::video, "nist", {}, {}},
		{file_group::video, "nsp", {}, {}},
		{file_group::audio, "nst", {}, {}},
		{file_group::video, "nut", {}, {}},
		{file_group::video, "obu", "AV1 video stream", {}},
		{file_group::audio, "oga", "Ogg audio", {}},
		{file_group::audio, "ogg", "container, multimedia", {}},
		{file_group::video, "ogm", {}, {}},
		{file_group::video, "ogv", {}, {}},
		{file_group::video, "ogx", {}, {}},
		{file_group::audio, "okt", {}, {}},
		{file_group::video, "oma", {}, {}},
		{file_group::video, "omg", {}, {}},
		{file_group::audio, "opus", "Opus audio", {}},
		{file_group::audio, "qoa", "Quite OK Audio", {}},
		{file_group::audio, "iamf", "Immersive Audio Model and Formats", {}},
		{file_group::audio, "lc3", "Low Complexity Communication Codec", {}},
		{file_group::photo, "orf,ori", "Olympus raw", file_traits::raw | file_traits::edit},
		{file_group::video, "paf", {}, {}},
		{file_group::video, "pam", {}, {}},
		{file_group::photo, "pbm", "Portable bitmap", {}},
		{file_group::photo, "pcd", {}, {}},
		{file_group::photo, "pct", "Apple Macintosh PICT image", {}},
		{file_group::photo, "pcx", "ZSoft's PC Paint image", {}},
		{file_group::photo, "pef,ptx", "Pentax raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "pfm", {}, {}},
		{file_group::photo, "pgm", "Portable graymap", {}},
		{file_group::video, "pgmyuv", {}, {}},
		{file_group::photo, "pic", {}, {}},
		{file_group::photo, "pict", "Apple Macintosh PICT image", {}},
		{file_group::video, "pjs", {}, {}},
		{file_group::audio, "plm", {}, {}},
		still_file_type(detected_format::PNG),
		{file_group::photo, "ppm", "Portable Pixmap", {}},
		still_file_type(detected_format::PSD),
		{file_group::audio, "psm", {}, {}},
		{file_group::video, "psp", "Paint Shop Pro image", {}},
		{file_group::audio, "pt36", {}, {}},
		{file_group::audio, "ptm", {}, {}},
		{file_group::video, "pvf", {}, {}},
		{file_group::photo, "pxn", "Logitech raw", file_traits::raw | file_traits::edit},
		{file_group::video, "qcif", {}, {}},
		{file_group::photo, "qtk", "Apple Quicktake raw", file_traits::raw | file_traits::edit},
		{file_group::audio, "ra, rm", "RealAudio", {}},
		{file_group::photo, "raf", "Fuji raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "ras", {}, {}},
		{file_group::video, "rco", {}, {}},
		{file_group::video, "rcv", {}, {}},
		{file_group::video, "rmd,r3d", "RED", {}},
		{file_group::video, "rgb", "Silicon Graphics Image", {}},
		{file_group::video, "rm", "RealAudio (RA, RM)", {}},
		{file_group::video, "roq", "Quake 3 video", {}},
		{file_group::video, "rsd", {}, {}},
		{file_group::video, "rso", {}, {}},
		{file_group::video, "rt", {}, {}},
		{file_group::photo, "rw2", "Panasonic raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "rwl", "Leica raw", file_traits::raw | file_traits::edit},
		{file_group::audio, "s3m", {}, {}},
		{file_group::video, "sami", {}, {}},
		{file_group::video, "sbc", {}, {}},
		{file_group::video, "sbg", {}, {}},
		{file_group::video, "scc", {}, {}},
		{file_group::video, "sdr2", {}, {}},
		{file_group::video, "sds", {}, {}},
		{file_group::video, "sdx", {}, {}},
		{file_group::video, "ser", {}, {}},
		{file_group::video, "sf", {}, {}},
		{file_group::audio, "sfx", {}, {}},
		{file_group::audio, "sfx2", {}, {}},
		{file_group::photo, "sgi", "Silicon Graphics Image", {}},
		{file_group::video, "shn", "Shorten (SHN)", {}},
		{file_group::video, "son", {}, {}},
		{file_group::video, "sox", {}, {}},
		{file_group::video, "spdif", {}, {}},
		{file_group::video, "sph", {}, {}},
		{file_group::audio, "spx", "Speex low bitrate audio", {}},
		{file_group::photo, "srw", "Samsung raw", file_traits::raw | file_traits::edit},
		{file_group::video, "ss2", {}, {}},
		{file_group::video, "ssa", {}, {}},
		{file_group::audio, "st26", {}, {}},
		{file_group::audio, "stk", {}, {}},
		{file_group::video, "stl", {}, {}},
		{file_group::audio, "stm", {}, {}},
		{file_group::audio, "stp", {}, {}},
		{file_group::video, "str", {}, {}},
		{file_group::video, "sub", {}, {}},
		{file_group::video, "sup", {}, {}},
		{file_group::video, "ag", {}, {}},
		{file_group::video, "swf", "Macromedia Flash", file_traits::embedded_xmp},
		{file_group::video, "tak", "Tom's Lossless Audio Kompressor", {}},
		{file_group::photo, "tga,targa", "Truevision TGA (Targa) image", {}},
		{file_group::video, "tco", {}, {}},
		{file_group::video, "thd", {}, {}},
		still_file_type(detected_format::TIFF),
		{file_group::photo, "cin", "Kodak Cineon Image", {}},
		{file_group::photo, "dpx", "Digital Picture Exchange", {}},
		{file_group::video, "tod", {}, {}},
		{file_group::video, "ts", "MPEG-2 Transport Stream", {}},
		{file_group::audio, "tta", "True Audio", {}},
		{file_group::video, "ty", {}, {}},
		{file_group::video, "ty+", {}, {}},
		{file_group::audio, "ult", {}, {}},
		{file_group::audio, "umx", {}, {}},
		{file_group::video, "v210", {}, {}},
		{file_group::video, "vag", {}, {}},
		{file_group::video, "vc1", "SMPTE VC-1 video", {}},
		{file_group::video, "vc2", {}, {}},
		{file_group::video, "viv", {}, {}},
		{file_group::video, "voc", "Creative Labs Soundblaster", {}},
		{file_group::video, "vpk", {}, {}},
		{file_group::video, "vqe", {}, {}},
		{file_group::video, "vqf", "Yamaha TwinVQ", {}},
		{file_group::video, "vql", {}, {}},
		{file_group::video, "w64", {}, {}},
		{file_group::photo, "wap", {}, {}},
		{
			file_group::audio, "wav", "Microsoft Wave",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{file_group::photo, "wbm", {}, {}},
		// WBMP has no metadata container, so its metadata goes to a sidecar.
		{file_group::photo, "wbmp", {}, file_traits::edit},
		{file_group::video, "web", {}, {}},
		{file_group::video, "webm", "WebM video", {}},
		still_file_type(detected_format::WEBP),
		{
			file_group::audio, "wma", "Windows Media Audio 9",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{
			file_group::video, "wmv,wm", "Windows Media video",
			file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata
		},
		{file_group::audio, "wow", {}, {}},
		{file_group::video, "wsd", {}, {}},
		{file_group::video, "wtv", {}, {}},
		{file_group::audio, "wv", "WavPack", {}},
		{file_group::photo, "x3f", "Sigma raw", file_traits::raw | file_traits::edit},
		{file_group::photo, "xbm", "X Window System Bitmap", {}},
		{file_group::video, "xl", {}, {}},
		{file_group::audio, "xm", {}, {}},
		{file_group::video, "xmv", {}, {}},
		{file_group::audio, "xpk", {}, {}},
		{file_group::photo, "xpm", "X Window System Pixmap", {}},
		{file_group::video, "xvag", {}, {}},
		{file_group::audio, "xx", {}, {}},
		{file_group::video, "y4m", {}, {}},
		{file_group::video, "yop", {}, {}},
		{file_group::video, "yuv", "Raw YUV video format", {}},
		{file_group::video, "yuv10", {}, {}},

		{file_group::archive, "zip", {}, {file_traits::archive}},
		{file_group::archive, "rar", {}, {file_traits::archive}},
		{file_group::archive, "7z", {}, {file_traits::archive}},
		{file_group::archive, "gz", {}, {file_traits::archive}},
		{file_group::archive, "tgz", {}, {file_traits::archive}},
		{file_group::archive, "cpio", {}, {file_traits::archive}},
		{file_group::archive, "iso", {}, {file_traits::archive}},
		{file_group::archive, "cab", {}, {file_traits::archive}},
		{file_group::archive, "pax", {}, {file_traits::archive}},
		{file_group::archive, "lzip", {}, {file_traits::archive}},
		{file_group::archive, "lza", {}, {file_traits::archive}},
		{file_group::archive, "bzip2,bz2", {}, {file_traits::archive}},
		{file_group::archive, "tar", {}, {file_traits::archive}},
		{file_group::archive, "lha", {}, {file_traits::archive}},
		{file_group::archive, "a,ar", {}, {file_traits::archive}},

		{file_group::commodore, "d64", {}, {file_traits::disk_image | file_traits::commodore}},
		{file_group::commodore, "d71", {}, {file_traits::disk_image | file_traits::commodore}},
		{file_group::commodore, "d81", {}, {file_traits::disk_image | file_traits::commodore}},
		{file_group::commodore, "t64", {}, {file_traits::commodore}},
		{file_group::commodore, "crt", {}, {file_traits::commodore}},
		{file_group::commodore, "prg", {}, {file_traits::commodore}},
		{file_group::commodore, "p00", {}, {file_traits::commodore}},
	};

	int next_id = 0;
	auto canonical = std::make_shared<file_group_by_name>();

	for (const auto g : s_config.groups)
	{
		(*canonical)[g->name] = g;
		g->id = next_id;
		next_id += 1;
	}

	s_config.groups_by_name.store(std::shared_ptr<const file_group_by_name>(std::move(canonical)));

	// The plural and the current-language spellings, so the address box round trips before any
	// catalog is loaded. Loading one calls this again.
	register_file_group_spellings();

	for (auto& ft : s_config.types)
	{
		auto& sidecars = ft.sidecars;
		sidecars.insert(sidecars.end(), ft.group->sidecars.begin(), ft.group->sidecars.end());
		std::ranges::sort(sidecars);
		sidecars.erase(std::ranges::unique(sidecars).begin(), sidecars.end());

		if (ft.icon == icon_index::document)
		{
			ft.icon = ft.group->icon;
		}

		if (!ft.extension.empty())
		{
			str::split2(ft.extension, true, [pft = &ft](const std::string_view ext)
			{
				s_config.types_by_name.insert_or_assign(str::cache(ext), pft);
			});
		}
	}

	av_initialise();
}

static std::vector<file_map_link> s_map_links;

namespace
{
	// Tools sit a few levels below the configured roots (vendor\product\app.exe). Capping the walk
	// keeps startup bounded on machines with large Program Files trees and stops a directory
	// junction from recursing forever.
	constexpr int max_tool_scan_depth = 4;

	using exe_path_by_name = df::hash_map<std::string_view, df::file_path, df::ihash, df::ieq>;

	// Only the executables named in the config are retained, so memory stays proportional to the
	// config and not to the number of programs installed.
	void find_executables(const df::folder_path folder, const int depth,
	                      const df::hash_set<std::string_view, df::ihash, df::ieq>& wanted,
	                      exe_path_by_name& found)
	{
		if (found.size() >= wanted.size()) return;

		const auto contents = platform::iterate_file_items(folder, true);

		for (const auto& f : contents.files)
		{
			const auto path = folder.combine_file(f.name);

			if (str::icmp(path.extension(), ".exe") == 0)
			{
				const auto match = wanted.find(path.file_name_without_extension());

				if (match != wanted.end())
				{
					found.emplace(*match, path);
				}
			}
		}

		if (depth < max_tool_scan_depth)
		{
			for (const auto& sub : contents.folders)
			{
				find_executables(folder.combine(sub.name.sv()), depth + 1, wanted, found);
			}
		}
	}

	// Map links are handed to the shell, so only http(s) templates are accepted; another scheme in
	// the config would otherwise be a way to launch an arbitrary program from a menu click.
	bool is_web_url(const std::string_view url)
	{
		return str::starts(url, "https://") || str::starts(url, "http://");
	}

	std::vector<file_map_link> built_in_map_links()
	{
		return {
			{str::cache("Google Maps"), str::cache("https://www.google.com/maps/place/{latitude},{longitude}")},
			{str::cache("Google Earth"), str::cache("https://earth.google.com/web/search/{latitude},{longitude}")},
			{str::cache("Bing Maps"), str::cache("https://www.bing.com/maps?cp={latitude}~{longitude}&lvl=17&style=h")},
			{
				str::cache("OpenStreetMap"),
				str::cache(
					"https://www.openstreetmap.org/?mlat={latitude}&mlon={longitude}#map=17/{latitude}/{longitude}")
			},
			{
				str::cache("Apple Maps"),
				str::cache("https://maps.apple.com/?ll={latitude},{longitude}&q=Photo%20Location")
			},
		};
	}
}

file_tools_result scan_tools()
{
	file_tools_result result;
	std::vector<df::folder_path> tool_paths;
	std::vector<file_tool_ptr> tools;

	try
	{
		const auto json = blob_from_file(df::probe_data_file("diffractor-tools.json"));

		if (!json.empty())
		{
			df::util::json::json_doc document;
			document.Parse(std::bit_cast<const char*>(json.data()), json.size());

			if (document.HasParseError())
			{
				df::log(__FUNCTION__, std::format("diffractor-tools.json is not valid json (offset {})",
				                                  document.GetErrorOffset()));
			}
			else if (document.IsObject())
			{
				const auto tools_member = document.FindMember("tools");

				if (tools_member != document.MemberEnd() && tools_member->value.IsObject())
				{
					for (const auto& m : tools_member->value.GetObject())
					{
						if (str::icmp(m.name.GetString(), "folders") == 0)
						{
							if (m.value.IsArray())
							{
								for (const auto& folder : m.value.GetArray())
								{
									if (folder.IsString())
									{
										tool_paths.emplace_back(df::folder_path(folder.GetString()));
									}
								}
							}
						}

						if (str::icmp(m.name.GetString(), "apps") == 0)
						{
							if (m.value.IsArray())
							{
								for (const auto& app : m.value.GetArray())
								{
									if (app.IsObject())
									{
										auto tool = std::make_shared<file_tool>();

										for (const auto& a : app.GetObject())
										{
											if (!a.value.IsString()) continue;

											if (str::icmp(a.name.GetString(), "exe") == 0)
												tool->exe = str::cache(a.value.GetString());
											if (str::icmp(a.name.GetString(), "invoke") == 0)
												tool->invoke_text = str::cache(a.value.GetString());
											if (str::icmp(a.name.GetString(), "text") == 0)
												tool->text = str::cache(a.value.GetString());
											if (str::icmp(a.name.GetString(), "extensions") == 0)
												tool->extensions = str::cache(a.value.GetString());
											if (str::icmp(a.name.GetString(), "group") == 0)
												tool->group = str::cache(a.value.GetString());
										}

										if (!tool->exe.is_empty() && !tool->invoke_text.is_empty())
										{
											tools.emplace_back(std::move(tool));
										}
									}
								}
							}
						}

						if (str::icmp(m.name.GetString(), "maps") == 0)
						{
							if (m.value.IsArray())
							{
								for (const auto& map : m.value.GetArray())
								{
									if (map.IsObject())
									{
										file_map_link link;

										for (const auto& a : map.GetObject())
										{
											if (!a.value.IsString()) continue;

											if (str::icmp(a.name.GetString(), "text") == 0)
												link.text = str::cache(a.value.GetString());
											if (str::icmp(a.name.GetString(), "url") == 0)
												link.url = str::cache(a.value.GetString());
										}

										if (!str::is_empty(link.text) && is_web_url(link.url.sv()))
										{
											result.map_links.emplace_back(link);
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	df::hash_set<std::string_view, df::ihash, df::ieq> wanted;

	for (const auto& tool : tools)
	{
		wanted.emplace(tool->exe.sv());
	}

	exe_path_by_name exe_by_name;

	for (const auto& folder : tool_paths)
	{
		find_executables(folder, 1, wanted, exe_by_name);
	}

	for (const auto& tool : tools)
	{
		const auto found = exe_by_name.find(tool->exe.sv());

		if (found == exe_by_name.end())
		{
			continue;
		}

		tool->exe_path = found->second;

		str::split2(tool->extensions, true, [&result, &tool](const std::string_view ext)
		{
			result.by_extension[str::cache(ext)].emplace_back(tool);
		});

		if (!tool->group.is_empty())
		{
			result.by_group[tool->group.sv()].emplace_back(tool);
		}
	}

	if (result.map_links.empty())
	{
		// Fall back to built-in map services when diffractor-tools.json has no usable "maps"
		// section (e.g. an older config), so location links always work.
		result.map_links = built_in_map_links();
	}

	return result;
}

void apply_tools(file_tools_result result)
{
	for (auto& ft : s_config.types)
	{
		ft.tools.clear();

		if (!ft.extension.empty())
		{
			str::split2(ft.extension, true, [&ft, &result](const std::string_view ext)
			{
				const auto found = result.by_extension.find(ext);

				if (found != result.by_extension.end())
				{
					for (const auto& tool : found->second)
					{
						if (std::ranges::find(ft.tools, tool) == ft.tools.end())
						{
							ft.tools.emplace_back(tool);
						}
					}
				}
			});
		}
	}

	for (const auto g : s_config.groups)
	{
		const auto found = result.by_group.find(g->name);
		g->tools = found != result.by_group.end() ? found->second : std::vector<file_tool_ptr>{};
	}

	s_map_links = std::move(result.map_links);
}

std::vector<file_map_link> all_map_links()
{
	return s_map_links;
}

std::vector<file_group_ref> all_file_groups()
{
	std::vector<file_group_ref> result;
	result.reserve(s_config.groups.size());

	for (auto* const g : s_config.groups)
	{
		result.emplace_back(g);
	}

	return result;
}

std::vector<file_type_ref> all_file_types()
{
	std::vector<file_type_ref> result;
	result.reserve(s_config.types.size());

	for (const auto& ft : s_config.types)
	{
		result.emplace_back(&ft);
	}

	return result;
}

namespace
{
	constexpr auto file_op_stat_count = static_cast<size_t>(file_op_stat::count);
	using file_op_row = std::array<uint64_t, file_op_stat_count>;

	// Bumped from every queue that touches a file and read once at shutdown. Each bump accompanies
	// an open, a decode or a swap, so one short lock costs nothing against the I/O it sits beside.
	platform::mutex file_op_mutex;
	_Guarded_by_(file_op_mutex) df::hash_map<file_type_ref, file_op_row> file_op_stats;
}

std::function<void(df::file_path destination)> test_before_file_update_publish;

void record_file_op(const file_type_ref ft, const file_op_stat stat)
{
	if (!ft) return;

	platform::exclusive_lock lock(file_op_mutex);
	file_op_stats[ft][static_cast<size_t>(stat)] += 1;
}

void log_file_op_summary()
{
	std::vector<std::pair<file_type_ref, file_op_row>> rows;

	{
		platform::exclusive_lock lock(file_op_mutex);
		rows.assign(file_op_stats.begin(), file_op_stats.end());
	}

	if (rows.empty()) return;

	const auto row_total = [](const file_op_row& r)
	{
		uint64_t total = 0;
		for (const auto n : r) total += n;
		return total;
	};

	std::ranges::sort(rows, [&row_total](const auto& a, const auto& b)
	{
		return row_total(a.second) > row_total(b.second);
	});

	// The first extension names the whole group, and the capability marker states what the traits
	// table promises - so a row that patches when it claims it cannot, or replaces when it claims
	// it can patch, is visible without cross-referencing the traits.
	const auto group_name = [](const std::string_view extensions)
	{
		const auto comma = extensions.find(',');
		return comma == std::string_view::npos ? extensions : extensions.substr(0, comma);
	};

	const auto capability = [](const file_type_ref ft)
	{
		return ft->has_trait(file_traits::in_place_metadata) ? "if it fits"sv : "-"sv;
	};

	file_op_row totals{};
	std::string report =
		"\n### Diffractor file operation results\n\n"
		"| File type | In-place capability | Reads | In-place writes | Replacements | Sidecars | Write failures |\n"
		"|---|---|---:|---:|---:|---:|---:|\n";

	for (const auto& [ft, row] : rows)
	{
		for (auto i = 0_z; i < file_op_stat_count; ++i) totals[i] += row[i];
		report += std::format("| {} | {} | {} | {} | {} | {} | {} |\n", group_name(ft->extension),
		                      capability(ft), row[0], row[1], row[2], row[3], row[4]);
	}

	report += std::format("| **Total** | - | {} | {} | {} | {} | {} |\n", totals[0], totals[1], totals[2],
	                      totals[3], totals[4]);
	df::log("perf file types", report);
}

namespace
{
	class loaded_clipboard_image final : public platform::clipboard_image
	{
		file_load_result _loaded;

	public:
		explicit loaded_clipboard_image(file_load_result loaded) : _loaded(std::move(loaded))
		{
		}

		sizei dimensions() const override
		{
			return _loaded.dimensions();
		}

		void write_pixels(const std::span<uint8_t> pixels) const override
		{
			const auto s = _loaded.to_surface();

			if (!is_valid(s))
			{
				// A picture that could not be decoded is not a picture to offer: an empty frame on the
				// clipboard would report the copy as having worked.
				df::log(__FUNCTION__, "clipboard image could not be decoded");
				throw std::invalid_argument("Invalid clipboard image");
			}

			const auto dimensions = _loaded.dimensions();
			const auto stride_out = dimensions.cx * 4_z;
			const auto stride_in = s->stride();
			const auto copy_len = std::min(stride_out, stride_in);
			const auto* const pixels_in = s->pixels();

			if (pixels_in == nullptr)
			{
				df::log(__FUNCTION__, "Invalid pixel buffer pointers");
				throw std::invalid_argument("Invalid pixel buffers");
			}

			for (auto y = 0; y < dimensions.cy; ++y)
			{
				const auto src_offset = stride_in * y;
				const auto dest_offset = stride_out * y;

				// Bounds checking for buffer operations
				if (src_offset + copy_len > s->size() || dest_offset + copy_len > pixels.size())
				{
					df::log(__FUNCTION__, "Buffer bounds exceeded");
					throw std::invalid_argument("Invalid pixel buffer size");
				}

				memcpy(pixels.data() + dest_offset, pixels_in + src_offset, copy_len);
			}
		}
	};
}

platform::clipboard_image_ptr clipboard_image_of(const file_load_result& loaded)
{
	if (!loaded.success) return {};
	return std::make_shared<loaded_clipboard_image>(loaded);
}

sizei file_load_result::dimensions() const
{
	if (success)
	{
		if (is_valid(i)) return i->dimensions();
		if (is_valid(s)) return s->dimensions();
	}
	return {};
}

ui::const_surface_ptr file_load_result::to_surface(const sizei scale_hint, const bool can_use_yuv,
                                                   const df::cancel_token& token, const decode_intent intent) const
{
	if (success)
	{
		files ff;

		if (is_valid(s))
		{
			return ff.fit_within(s, scale_hint);
		}

		if (is_valid(i))
		{
			return ff.image_to_surface(i, scale_hint, can_use_yuv, token, intent);
		}
	}

	return {};
}

ui::pixel_difference_result file_load_result::calc_pixel_difference(const file_load_result& other) const
{
	constexpr sizei scale_hint = {1024 * 4, 1024 * 4};
	const auto s1 = to_surface(scale_hint);
	const auto s2 = other.to_surface(scale_hint);

	if (s1 && s2)
	{
		return s1->pixel_difference(s2);
	}

	return ui::pixel_difference_result::unknown;
}


file_type_ref files::file_type_from_name(const df::file_path path)
{
	return file_type_from_name(path.name());
}

namespace
{
	// Extensions claimed by both a binary container and a widely used text format. Only entries with
	// a signature strong enough to be decisive belong here: a wrong answer either hides real media or
	// keeps offering to play a source file. Runs once per media open, including every av file an
	// index scan touches, so it stays a short-circuiting compare over a two-to-four character string.
	bool is_transport_stream_extension(std::string_view ext)
	{
		if (!ext.empty() && ext.front() == '.') ext = ext.substr(1);

		return str::icmp(ext, "ts") == 0 || str::icmp(ext, "m2t") == 0 ||
			str::icmp(ext, "m2ts") == 0 || str::icmp(ext, "mts") == 0;
	}

	// A transport stream is a run of fixed-size packets, each opening with the 0x47 sync byte: 188
	// bytes broadcast, 192 with the M2TS/AVCHD arrival timestamp, 204 with Reed-Solomon parity. The
	// run is found rather than assumed to start at zero, because a partial capture or a PVR dump can
	// begin mid-packet or behind a prefix. Requiring four aligned packet starts is what separates a
	// stream from a TypeScript file that happens to contain 'G'.
	bool is_mpeg_transport_stream(const df::cspan header)
	{
		constexpr size_t packet_sizes[] = {188, 192, 204};
		constexpr size_t max_packet_size = 204;
		constexpr size_t required_packets = 4;
		constexpr uint8_t sync_byte = 0x47;

		for (auto start = size_t{0}; start < max_packet_size; ++start)
		{
			for (const auto packet_size : packet_sizes)
			{
				// Anything shorter cannot show the run, and is far too short to be a stream anyway.
				if (start + (required_packets - 1) * packet_size >= header.size) continue;

				auto matched = true;

				for (auto i = size_t{0}; i < required_packets && matched; ++i)
				{
					matched = header.data[start + i * packet_size] == sync_byte;
				}

				if (matched) return true;
			}
		}

		return false;
	}
}

bool files::has_media_header_rule(const std::string_view extension)
{
	return is_transport_stream_extension(extension);
}

bool files::media_header_matches(const std::string_view extension, const df::cspan header)
{
	if (is_transport_stream_extension(extension))
	{
		return is_mpeg_transport_stream(header);
	}

	return true;
}

file_type_ref files::file_type_from_name(const std::string_view name)
{
	if (!str::is_empty(name))
	{
		auto ext = name.substr(df::find_ext(name));

		if (!ext.empty())
		{
			if (ext[0] == L'.') ext = ext.substr(1);
			auto found = s_config.types_by_name.find(ext);
			if (found == s_config.types_by_name.cend()) found = s_config.types_by_name.find(name); // Try whole name?
			if (found != s_config.types_by_name.cend()) return found->second;
		}
	}

	return file_type::other;
}

bool image_edits::has_changes(const sizei image_extent) const
{
	return _scale.cx != 0 || _scale.cy != 0 || has_crop(image_extent) || has_rotation() || has_perspective() ||
		has_color_changes();
}

bool image_edits::is_empty() const
{
	// has_changes with the crop widened to any crop at all, so this can only ever be the safer answer.
	return !(_scale.cx != 0 || _scale.cy != 0 || has_crop_bounds() || has_rotation() || has_perspective() ||
		has_color_changes());
}

quadd image_edits::perspective_bounds(const sizei size) const
{
	const auto display_angle = to_radian(-crop_bounds(size).angle());
	const auto angle_sin = sin(display_angle);
	const auto angle_cos = cos(display_angle);
	const auto horizontal = perspective_horizontal() * angle_cos - perspective_vertical() * angle_sin;
	const auto vertical = perspective_horizontal() * angle_sin + perspective_vertical() * angle_cos;
	const quadd source(size);
	quadd result;

	for (auto index = 0; index < 4; ++index)
	{
		const auto point = source[index];
		const auto normalized_x = point.X / size.cx - 0.5;
		const auto normalized_y = point.Y / size.cy - 0.5;
		const auto denominator = 1.0 - horizontal * normalized_x - vertical * normalized_y;
		result[index] = {
			(0.5 + normalized_x / denominator) * size.cx,
			(0.5 + normalized_y / denominator) * size.cy
		};
	}

	return result;
}

quadd image_edits::effective_crop_bounds(const sizei size) const
{
	const auto selection = crop_bounds(size);
	if (!has_perspective()) return selection.crop(rectd(0, 0, size.cx, size.cy));

	const auto angle = selection.angle();
	const auto center = quadd(size).center_point();
	const auto valid = perspective_bounds(size).rotate(-angle, center);
	const auto selected = selection.rotate(-angle, center);
	const auto valid_bounds = valid.bounding_rect();
	const auto selected_bounds = selected.bounding_rect();
	const auto scan_top = (std::max)(valid_bounds.top(), selected_bounds.top());
	const auto scan_bottom = (std::min)(valid_bounds.bottom(), selected_bounds.bottom());
	constexpr auto steps = 128;
	std::array<double, steps + 1> left{};
	std::array<double, steps + 1> right{};

	for (auto step = 0; step <= steps; ++step)
	{
		const auto y = scan_top + (scan_bottom - scan_top) * step / steps;
		left[step] = std::numeric_limits<double>::lowest();
		right[step] = (std::numeric_limits<double>::max)();

		for (const auto* polygon : {&valid, &selected})
		{
			auto polygon_left = (std::numeric_limits<double>::max)();
			auto polygon_right = std::numeric_limits<double>::lowest();

			for (auto edge = 0; edge < 4; ++edge)
			{
				const auto start = (*polygon)[edge];
				const auto end = (*polygon)[(edge + 1) % 4];
				if (y < (std::min)(start.Y, end.Y) || y > (std::max)(start.Y, end.Y) || df::equiv(start.Y, end.Y))
					continue;
				const auto x = start.X + (y - start.Y) * (end.X - start.X) / (end.Y - start.Y);
				polygon_left = (std::min)(polygon_left, x);
				polygon_right = (std::max)(polygon_right, x);
			}

			left[step] = (std::max)(left[step], polygon_left);
			right[step] = (std::min)(right[step], polygon_right);
		}
	}

	rectd largest;
	auto largest_area = 0.0;
	for (auto top = 0; top < steps; ++top)
	{
		auto max_left = left[top];
		auto min_right = right[top];
		for (auto bottom = top + 1; bottom <= steps; ++bottom)
		{
			max_left = (std::max)(max_left, left[bottom]);
			min_right = (std::min)(min_right, right[bottom]);
			const auto y1 = scan_top + (scan_bottom - scan_top) * top / steps;
			const auto y2 = scan_top + (scan_bottom - scan_top) * bottom / steps;
			const auto area = (std::max)(0.0, min_right - max_left) * (y2 - y1);
			if (area > largest_area)
			{
				largest_area = area;
				largest = {max_left, y1, min_right - max_left, y2 - y1};
			}
		}
	}

	return quadd(largest).rotate(angle, center);
}

bool image_edits::has_crop(const sizei image_extent) const
{
	if (_crop.is_empty()) return false;
	quadd bounds(image_extent);
	return !_crop.has_point(bounds[0]) || !_crop.has_point(bounds[1]) || !_crop.has_point(bounds[2]) || !_crop.
		has_point(bounds[3]);
}

bool image_edits::is_no_loss(const sizei image_extent) const
{
	return !has_crop(image_extent) && !has_scale() && !has_perspective() && !has_color_changes();
}

double image_edits::rotation_angle() const
{
	return _crop.angle();
}

bool files::can_save(const df::file_path path)
{
	const auto ext = path.extension();
	return can_save_extension(ext);
}

bool files::can_save_extension(const std::string_view ext)
{
	static const df::hash_set<std::string_view, df::ihash, df::ieq> save_extensions = {
		{".jpg"},
		{".jpeg"},
		{".jpe"},
		{".png"},
		{".webp"},
		{"jpg"},
		{"jpeg"},
		{"jpe"},
		{"png"},
		{"webp"},
	};

	return save_extensions.contains(ext);
}

bool files::is_raw(const df::file_path path)
{
	const auto* const mt = file_type_from_name(path);
	return (mt->traits & file_traits::raw) != file_traits::none;
}

bool files::is_raw(const std::string_view name)
{
	const auto* const mt = file_type_from_name(name);
	return (mt->traits & file_traits::raw) != file_traits::none;
}

bool files::is_jpeg(const df::file_path path)
{
	const auto ext = path.extension();
	return str::icmp(ext, ".jpg") == 0 || str::icmp(ext, ".jpeg") == 0 || str::icmp(ext, ".jpe") == 0;
}

bool files::is_jpeg(const std::string_view name)
{
	const auto ext = name.substr(df::find_ext(name));
	return str::icmp(ext, ".jpg") == 0 || str::icmp(ext, ".jpeg") == 0 || str::icmp(ext, ".jpe") == 0;
}

detected_format files::detect_format(const df::cspan image_buffer_in)
{
	switch (libfiles::detect_image_format({image_buffer_in.data, image_buffer_in.size}))
	{
	case libfiles::image_format::jpeg: return detected_format::JPEG;
	case libfiles::image_format::png: return detected_format::PNG;
	case libfiles::image_format::webp: return detected_format::WEBP;
	case libfiles::image_format::gif: return detected_format::GIF;
	case libfiles::image_format::psd: return detected_format::PSD;
	case libfiles::image_format::bmp: return detected_format::BMP;
	case libfiles::image_format::tiff: return detected_format::TIFF;
	case libfiles::image_format::heif: return detected_format::HEIF;
	case libfiles::image_format::jxl: return detected_format::JXL;
	default: return detected_format::Unknown;
	}
}

files::files()
{
}

files::~files()
{
	_scaler.reset();
}

ui::const_image_ptr files::surface_to_image(const ui::const_surface_ptr& surface_in, const metadata_parts& metadata,
                                            const file_encode_params& params, const ui::image_format format)
{
	ui::const_image_ptr result;

	// save_png and the jpeg encoder both walk the surface as four-byte pixels, so a planar one would
	// be written out as its luma plane read as BGRX - grey and tiled rather than a visible failure.
	if (is_valid(surface_in) && ui::is_packed(surface_in->format()))
	{
		const auto dimensions = surface_in->dimensions();
		const auto has_alpha = surface_in->format() == ui::texture_format::ARGB;
		const auto orientation = surface_in->orientation();

		if (format == ui::image_format::PNG ||
			(format == ui::image_format::Unknown && has_alpha))
		{
			result = save_png(surface_in, metadata);
		}
		else if (format == ui::image_format::WEBP)
		{
			result = save_webp(surface_in, metadata, params);
		}
		else
		{
			result = std::make_shared<ui::image>(
				_jpeg_encoder.encode(dimensions.cx, dimensions.cy, surface_in->pixels(),
				                     static_cast<uint32_t>(surface_in->stride()),
				                     orientation, metadata, params), dimensions, ui::image_format::JPEG, orientation);
		}
	}

	return result;
}

// Thumbnails are a rebuildable cache, so the format is chosen for bytes rather than fidelity. A
// lossy WebP is materially smaller than the JPEG or PNG it replaces at the same measured quality,
// and libwebp drops the alpha plane itself for a surface that turns out to be opaque.
ui::const_image_ptr files::surface_to_thumbnail(const ui::const_surface_ptr& surface_in)
{
	if (!is_valid(surface_in)) return {};

	file_encode_params params;
	params.jpeg_save_quality = thumbnail_quality;
	params.webp_quality = thumbnail_webp_quality;
	params.webp_lossy_alpha = true;
	params.webp_fast = true;

	ui::const_image_ptr result = save_webp(surface_in, {}, params);

	// The fallbacks below encode packed pixels only, so a planar surface the WebP encoder would not
	// take - a video frame carrying a rec709 matrix, say - is converted rather than dropped.
	if (!is_valid(result) && !ui::is_packed(surface_in->format()))
	{
		const auto packed = std::make_shared<ui::surface>();

		if (scaler().convert_yuv_surface(*surface_in, packed) && ui::is_valid(packed))
		{
			result = save_webp(packed, {}, params);
			if (!is_valid(result)) result = surface_to_image(packed, {}, params, ui::image_format::Unknown);
		}

		return result;
	}

	// save_webp accepts packed pixels and the NV12 planes VP8 encodes natively, so anything else
	// falls back to what the thumbnail store held before WebP: PNG when the surface carries alpha,
	// JPEG otherwise.
	if (!is_valid(result)) result = surface_to_image(surface_in, {}, params, ui::image_format::Unknown);

	return result;
}

av_scaler& files::scaler()
{
	if (!_scaler)
	{
		_scaler = std::make_unique<av_scaler>();
	}

	return *_scaler;
}

// A ceiling, not a size to fill. Every caller passes a limit - a thumbnail bound, a display scale
// hint, a preview cap - so a picture already inside it is handed back untouched rather than
// resampled up to fill it, which would cost a pass and an allocation to add no information.
template <typename Ptr>
Ptr files::fit_within_impl(Ptr surface_in, const sizei ceiling)
{
	if (!is_valid(surface_in) || ceiling.is_empty()) return surface_in;

	const auto dimensions_out = [&]
	{
		auto fitted = ui::scale_dimensions(surface_in->dimensions(), ceiling, true);

		// NV12 carries one chroma pair per 2x2 luma block, so a fitted extent has to be even for the
		// planar reduction to take it. Rounding down keeps it inside the ceiling.
		if (surface_in->format() == ui::texture_format::NV12)
		{
			fitted = {std::max(2, fitted.cx & ~1), std::max(2, fitted.cy & ~1)};
		}

		return fitted;
	}();

	if (dimensions_out == surface_in->dimensions()) return surface_in;

	ui::surface_ptr reduced;

	if (ui::area_downscale(surface_in, reduced, dimensions_out)) return reduced;

	// What the area filter will not take - P010, or a planar surface whose fitted extent lands odd -
	// still has to come back reduced, and swscale converts it to packed on the way.
	ui::surface_ptr converted;

	if (scaler().scale_surface(surface_in, converted, dimensions_out) && is_valid(converted))
	{
		return converted;
	}

	// A resampler that failed is not a reason to lose pixels already in hand.
	return surface_in;
}

ui::surface_ptr files::fit_within(ui::surface_ptr surface_in, const sizei ceiling)
{
	return fit_within_impl(std::move(surface_in), ceiling);
}

ui::const_surface_ptr files::fit_within(ui::const_surface_ptr surface_in, const sizei ceiling)
{
	return fit_within_impl(std::move(surface_in), ceiling);
}

ui::pixel_difference_result files::pixel_difference(const ui::const_image_ptr& expected,
                                                    const ui::const_image_ptr& actual)
{
	if (!is_empty(expected) || !is_empty(actual))
	{
		const auto expected_surface = image_to_surface(expected);
		const auto actual_surface = image_to_surface(actual);

		if (!is_empty(expected_surface) && !is_empty(actual_surface))
		{
			return expected_surface->pixel_difference(actual_surface);
		}
	}

	return ui::pixel_difference_result::unknown;
}


namespace
{
	// A surface as the memory libfiles decodes a JPEG into, so it lands there with nothing copied.
	//
	// The decode budget is charged here, the one point both image_to_surface overloads reach, from the
	// frame libfiles reports before it reads a pixel. libjpeg reduces while it decodes, so the charge
	// is what the decode will allocate rather than what the file stores: charging the full size would
	// refuse a large stitch even as a thumbnail, which is the one request that was always affordable.
	// A progressive or non-interleaved stream cannot produce a row until every coefficient of the
	// full-resolution image is buffered, and no reduction touches that, so its coefficients are
	// charged too - without them, a stream whose coefficients alone ran to gigabytes passed a check
	// that said tens of megabytes.
	class jpeg_surface_sink final : public libfiles::image_sink
	{
		const std::optional<ui::orientation> _orientation_override;
		const df::cancel_token& _token;

	public:
		ui::surface_ptr surface;

		jpeg_surface_sink(const std::optional<ui::orientation> orientation_override, const df::cancel_token& token) :
			_orientation_override(orientation_override), _token(token)
		{
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			const auto scale = static_cast<int64_t>(frame.scale_denominator);
			const auto scaled_output_bytes = static_cast<int64_t>(frame.source_width) * frame.source_height * 4 /
				(scale * scale);
			const auto decode_bytes = scaled_output_bytes + static_cast<int64_t>(frame.working_bytes);

			if (decode_bytes > df::max_decode_bytes)
			{
				df::log("files::decode_jpeg", std::format("decode of {} x {} needs {}, over the {} budget",
				                                          frame.source_width, frame.source_height,
				                                          df::file_size(decode_bytes).str(),
				                                          df::file_size(df::max_decode_bytes).str()));
				return {};
			}

			// The decoder reads the EXIF orientation by libfiles' one rule, the scan's and parse's.
			const auto orientation = _orientation_override
				                         ? *_orientation_override
				                         : static_cast<ui::orientation>(frame.orientation);
			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};
			auto s = std::make_shared<ui::surface>();

			if (frame.format == libfiles::pixel_format::nv12)
			{
				// JPEG/JFIF is full-range BT.601, which the shader applies through rec601_full.
				if (!s->alloc(dimensions, ui::texture_format::NV12, orientation)) return {};
				s->color_space(ui::color_space::rec601_full);
			}
			else if (!s->alloc(dimensions, ui::texture_format::RGB, orientation))
			{
				return {};
			}

			surface = std::move(s);
			return {surface->pixels(), surface->stride()};
		}

		bool cancelled() override
		{
			return _token.is_cancelled();
		}
	};
}

// The JPEG decode both image_to_surface overloads share. Returns an unscaled surface; an NV12 one is
// flagged through is_yuv, because the GPU sampler resizes it at draw time. Without an
// orientation_override, the orientation is the one the file's EXIF gives.
ui::surface_ptr files::decode_jpeg(const df::cspan data, const sizei target_extent, const bool can_use_yuv,
                                   const std::optional<ui::orientation> orientation_override, bool& is_yuv,
                                   const df::cancel_token& token, const decode_intent intent)
{
	is_yuv = false;

	try
	{
		libfiles::reader r(std::span<const uint8_t>(data.data, data.size));
		jpeg_surface_sink sink(orientation_override, token);

		libfiles::decode_options options;
		options.target_width = static_cast<uint32_t>(std::max(0, target_extent.cx));
		options.target_height = static_cast<uint32_t>(std::max(0, target_extent.cy));
		options.allow_nv12 = can_use_yuv;
		options.smooth_chroma = intent == decode_intent::display;

		const auto result = _decoder.decode(r, sink, options);

		if (!result.success)
		{
			if (!result.error.empty()) df::log(__FUNCTION__, result.error);
			return {};
		}

		is_yuv = result.frame.format == libfiles::pixel_format::nv12;
		return std::move(sink.surface);
	}
	catch (const std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	return {};
}

ui::surface_ptr files_test_hooks::decode_jpeg(files& ff, const df::cspan data, const sizei target_extent,
                                              const bool can_use_yuv, const decode_intent intent, bool& is_yuv)
{
	return ff.decode_jpeg(data, target_extent, can_use_yuv, {}, is_yuv, {}, intent);
}

int64_t files::estimate_decode_bytes(const sizei source_dimensions)
{
	return static_cast<int64_t>(source_dimensions.cx) * source_dimensions.cy * 4;
}

int64_t files::estimate_decode_bytes(const ui::const_image_ptr& image, const sizei scale_hint)
{
	if (!is_valid(image)) return 0;

	const auto dims = image->dimensions();
	auto pixels = static_cast<int64_t>(dims.cx) * dims.cy;

	if (image->format() == ui::image_format::JPEG)
	{
		// Mirrors what image_to_surface hands libjpeg, so the estimate tracks the real allocation.
		const auto factor = ui::calc_scale_down_factor(dims, scale_hint);
		pixels /= static_cast<int64_t>(factor) * factor;
	}

	return pixels * 4;
}

bool files::exceeds_decode_budget(const sizei source_dimensions)
{
	return estimate_decode_bytes(source_dimensions) > df::max_decode_bytes;
}

bool files::exceeds_decode_budget(const ui::const_image_ptr& image, const sizei scale_hint)
{
	return estimate_decode_bytes(image, scale_hint) > df::max_decode_bytes;
}

bool reject_over_budget_source(load_diagnostic* const diagnostic, const sizei source_dimensions,
                               const std::string_view format)
{
	if (diagnostic) diagnostic->source_dimensions = source_dimensions;

	const auto bytes = files::estimate_decode_bytes(source_dimensions);

	if (bytes <= df::max_decode_bytes) return false;

	if (diagnostic) diagnostic->over_budget = true;

	df::log(__FUNCTION__, std::format("{} {} x {} needs {} to decode, over the {} budget", format,
	                                  source_dimensions.cx, source_dimensions.cy, df::file_size(bytes).str(),
	                                  df::file_size(df::max_decode_bytes).str()));
	return true;
}

ui::surface_ptr files::image_to_surface(const ui::const_image_ptr& image, const sizei target_extent,
                                        const bool can_use_yuv, const df::cancel_token& token,
                                        const decode_intent intent)
{
	ui::surface_ptr surface_result;

	// Nothing downstream can produce a smaller frame than the codec does, so refusing here is what
	// keeps a huge image from exhausting memory on whichever worker asked for it.
	if (exceeds_decode_budget(image, target_extent))
	{
		df::log(__FUNCTION__, std::format("decode of {} x {} needs {}, over the {} budget",
		                                  image->dimensions().cx, image->dimensions().cy,
		                                  df::file_size(estimate_decode_bytes(image, target_extent)).str(),
		                                  df::file_size(df::max_decode_bytes).str()));
		return surface_result;
	}

	df::bump(df::file_perf.decodes);
	df::bump(df::file_perf.decode_bytes, is_valid(image) ? image->data().size() : 0u);
	df::perf_timer timer(df::file_perf.decode_us, &df::file_perf.decode_max_us);

	try
	{
		if (is_valid(image))
		{
			const auto format = image->format();

			if (format == ui::image_format::JPEG)
			{
				bool is_yuv = false;
				auto decoded = decode_jpeg(image->data(), target_extent, can_use_yuv, image->orientation(), is_yuv,
				                           token, intent);

				if (is_valid(decoded))
				{
					// As above: only a decode bound for a texture keeps its native planar size.
					surface_result = is_yuv && intent != decode_intent::thumbnail
						                 ? std::move(decoded)
						                 : fit_within(std::move(decoded), target_extent);
				}
			}
			else if (format == ui::image_format::PNG)
			{
				try
				{
					auto loaded = load_png(image->data());

					if (is_valid(loaded))
					{
						surface_result = fit_within(std::move(loaded), target_extent);
					}
				}
				catch (std::exception& e)
				{
					df::log(__FUNCTION__, e.what());
				}
			}
			else if (format == ui::image_format::WEBP)
			{
				try
				{
					auto loaded = load_webp(image->data(), can_use_yuv, target_extent);

					if (is_valid(loaded))
					{
						surface_result = fit_within(std::move(loaded), target_extent);
					}
				}
				catch (std::exception& e)
				{
					df::log(__FUNCTION__, e.what());
				}
			}
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	return surface_result;
}

ui::surface_ptr files::image_to_surface(const df::cspan image_buffer_in, const sizei target_extent,
                                        const bool can_use_yuv, const decode_intent intent)
{
	ui::surface_ptr surface_result;

	try
	{
		if (!image_buffer_in.empty())
		{
			const auto* const still = find_still_format(detect_format(image_buffer_in));

			if (still && still->decode)
			{
				surface_result = still->decode(*this, image_buffer_in, {target_extent, can_use_yuv, intent});
			}

			if (is_empty(surface_result))
			{
				// Everything without a decoder of its own - GIF, BMP, TIFF, TGA, SGI, the portable
				// pixmaps, DPX - reaches ffmpeg, which carries all of them on every platform.
				surface_result = av_decode_still(image_buffer_in, target_extent);
			}
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	return surface_result;
}

// Reduces a decoded picture to the grayscale square the hash consumes. Separated from hashing so
// the four orientations share one decode and one reduction.
static bool build_phash_gray(const ui::const_surface_ptr& surface, std::array<uint8_t, crypto::phash_pixels>& gray);

uint64_t files::calc_perceptual_hash(const ui::const_surface_ptr& surface)
{
	std::array<uint8_t, crypto::phash_pixels> gray{};
	if (!build_phash_gray(surface, gray)) return 0;
	return crypto::perceptual_hash(gray.data(), gray.size());
}

crypto::phash_rotations files::calc_perceptual_hash_rotations(const ui::const_surface_ptr& surface)
{
	std::array<uint8_t, crypto::phash_pixels> gray{};
	if (!build_phash_gray(surface, gray)) return {};
	return crypto::perceptual_hash_rotations(gray.data(), gray.size());
}

static bool build_phash_gray(const ui::const_surface_ptr& surface, std::array<uint8_t, crypto::phash_pixels>& gray)
{
	if (!is_valid(surface))
	{
		return false;
	}

	// RGB and ARGB are both four bytes a pixel; the planar YUV formats are not laid out this way and
	// are never what a still decodes to here.
	const auto format = surface->format();

	if (format != ui::texture_format::RGB && format != ui::texture_format::ARGB)
	{
		return false;
	}

	const auto extent = static_cast<int>(crypto::phash_extent);
	const auto width = static_cast<int>(surface->width());
	const auto height = static_cast<int>(surface->height());

	if (width <= 0 || height <= 0)
	{
		return false;
	}

	// Box-averaged down to the hash extent, ignoring aspect. Squashing rather than cropping is what
	// makes the hash survive a resize, and averaging rather than sampling is what makes it survive
	// the resampling a re-encode applies. Each cell claims its own source rectangle, so a source
	// narrower or shorter than the hash shares pixels between cells instead of leaving them empty.
	// Cells partition by fraction rather than by pixel count, which is what makes the grid of a
	// rotated picture the rotated grid of the original.
	for (auto cell_y = 0; cell_y < extent; ++cell_y)
	{
		const auto y0 = cell_y * height / extent;
		auto y1 = (cell_y + 1) * height / extent;
		if (y1 <= y0) y1 = y0 + 1;

		for (auto cell_x = 0; cell_x < extent; ++cell_x)
		{
			const auto x0 = cell_x * width / extent;
			auto x1 = (cell_x + 1) * width / extent;
			if (x1 <= x0) x1 = x0 + 1;

			uint32_t sum = 0;
			uint32_t count = 0;

			for (auto y = y0; y < y1 && y < height; ++y)
			{
				const auto* const line = std::bit_cast<const ui::color32*>(surface->pixels_line(y));

				for (auto x = x0; x < x1 && x < width; ++x)
				{
					const auto c = line[x];
					// Rec.601 luma in integer form; the hash needs a consistent gray, not colorimetry.
					sum += (ui::get_r(c) * 77 + ui::get_g(c) * 151 + ui::get_b(c) * 28) >> 8;
					count += 1;
				}
			}

			gray[cell_y * extent + cell_x] = count == 0 ? 0 : static_cast<uint8_t>(std::min(sum / count, 255u));
		}
	}

	return true;
}

crypto::phash_rotations files::calc_perceptual_hash_rotations(const df::cspan encoded)
{
	constexpr auto decode_extent = 128;
	return calc_perceptual_hash_rotations(image_to_surface(encoded, {decode_extent, decode_extent}, false,
	                                                       decode_intent::thumbnail));
}

uint64_t files::calc_perceptual_hash(const df::cspan encoded)
{
	// Decoded well above the hash extent so the reduction has real pixels to average, and still far
	// enough below native size that a JPEG scales in the DCT domain rather than decoding in full.
	constexpr auto decode_extent = 128;
	return calc_perceptual_hash(image_to_surface(encoded, {decode_extent, decode_extent}, false,
	                                             decode_intent::thumbnail));
}

static uint64_t round_up_to_multiple(const uint64_t n, const uint64_t multiple)
{
	if (multiple == 0)
		return n;

	const auto remainder = n % multiple;

	if (remainder == 0)
		return n;

	return n + multiple - remainder;
}

void file_read_stream::read(const uint64_t pos, uint8_t* buffer, const size_t len)
{
	load_buffer(pos, len);
	memcpy(buffer, _buffer + pos - _loaded_start_pos, len);
}

void file_read_stream::read_through(const uint64_t pos, uint8_t* buffer, const size_t len)
{
	check_range(pos, len);

	if (_h->seek(pos, platform::file::whence::begin) != pos)
	{
		const auto message = std::format("invalid read_through seek: {} {}", pos, _h->path());
		df::log(__FUNCTION__, message);
		throw app_exception(message);
	}

	if (_h->read(buffer, len) != len)
	{
		const auto message = std::format("invalid read_through read: {} {}", len, _h->path());
		df::log(__FUNCTION__, message);
		throw app_exception(message);
	}
}

void file_read_stream::load_buffer(const uint64_t pos, const size_t len)
{
	// Overflow-safe: pos and len both originate from untrusted file fields.
	if (pos > _file_size || len > _file_size - pos)
	{
		const auto message = std::format("invalid read past end of file: {}", _h->path());
		df::log(__FUNCTION__, message);
		throw app_exception(message);
	}

	const auto wanted_end_pos = pos + len;

	if (wanted_end_pos > _loaded_end_pos || pos < _loaded_start_pos)
	{
		const auto new_start_pos = pos > _block_size ? round_up_to_multiple(pos - _block_size, _block_size) : 0;
		const auto new_end_pos = std::min(round_up_to_multiple(wanted_end_pos, _block_size), _file_size);
		// The allocator takes a size_t, so a window wider than that can never be held in memory.
		const auto new_window = new_end_pos - new_start_pos;
		const auto new_buffer_size = static_cast<size_t>(new_window);

		if (new_buffer_size != new_window)
		{
			const auto message = std::format("buffer window too large: {} {}", new_window, _h->path());
			df::log(__FUNCTION__, message);
			throw std::bad_alloc();
		}

		// The window is invalid from here until the read completes; any throw in
		// between must not leave _loaded_* describing a buffer that no longer exists.
		_loaded_start_pos = 0;
		_loaded_end_pos = 0;

		if (_buffer_size != new_buffer_size)
		{
			// realloc returns null without freeing the original, so never assign
			// the result directly - that would leak the existing buffer.
			auto* const new_buffer = static_cast<uint8_t*>(_buffer == nullptr
				                                               ? _aligned_malloc(new_buffer_size, 16)
				                                               : _aligned_realloc(_buffer, new_buffer_size, 16));

			if (!new_buffer)
			{
				const auto message = std::format("buffer alloc failed: {}", _h->path());
				df::log(__FUNCTION__, message);
				throw std::bad_alloc();
			}

			_buffer = new_buffer;
			_buffer_size = new_buffer_size;
		}

		if (_h->seek(new_start_pos, platform::file::whence::begin) != new_start_pos)
		{
			const auto message = std::format("invalid load_buffer seek: {} {}", new_start_pos, _h->path());
			df::log(__FUNCTION__, message);
			throw app_exception(message);
		}

		if (_h->read(_buffer, new_buffer_size) != new_buffer_size)
		{
			const auto message = std::format("invalid load_buffer read: {} {}", new_buffer_size, _h->path());
			df::log(__FUNCTION__, message);
			throw app_exception(message);
		}

		_loaded_start_pos = new_start_pos;
		_loaded_end_pos = new_end_pos;
	}
}

bool file_read_stream::open(const df::file_path path)
{
	return open(open_file(path, platform::file_open_mode::sequential_scan));
}

uint64_t platform_file_stream::size()
{
	return _f->size();
}

size_t platform_file_stream::read(const uint64_t offset, void* data, const size_t size)
{
	if (_f->seek(offset, platform::file::whence::begin) != offset) return 0;
	return static_cast<size_t>(_f->read(static_cast<uint8_t*>(data), size));
}

bool platform_file_stream::write(const uint64_t offset, const void* data, const size_t size)
{
	if (_f->seek(offset, platform::file::whence::begin) != offset) return false;
	return _f->write(static_cast<const uint8_t*>(data), size) == size;
}

bool platform_file_stream::resize(const uint64_t size)
{
	return _f->trunc(size);
}

bool platform_file_stream::flush()
{
	return _f->flush();
}

bool file_read_stream::open(platform::file_ptr h)
{
	_loaded_start_pos = 0;
	_loaded_end_pos = 0;
	_file_size = 0;
	_block_size = 0;

	_h = std::move(h);

	if (_h)
	{
		_file_size = _h->size();
		_block_size = df::sixty_four_k; // platform::calc_optimal_read_size(path);
		return true;
	}

	return false;
}

void file_read_stream::close()
{
	_h.reset();
	_loaded_start_pos = 0;
	_loaded_end_pos = 0;
	_file_size = 0;
	_block_size = 0;

	if (_buffer)
	{
		_aligned_free(_buffer);
		_buffer = nullptr;
		_buffer_size = 0;
	}
}

file_read_stream::~file_read_stream()
{
	close();
}

df::blob file_read_stream::read_all()
{
	// Parenthesised: the Windows max macro is in scope here.
	if (_file_size > (std::numeric_limits<size_t>::max)())
		throw app_exception("file too large to load"s);

	// The size is the file's own claim, and the caller is about to hold all of it at once. Without
	// the ceiling every other whole-file read observes, a scan of an arbitrarily large file
	// allocates to match it. Answered empty rather than thrown: one file that cannot be held must
	// not abandon the batch that was scanning it, and every caller already treats an empty view as
	// a file it could not read.
	if (_file_size > df::max_blob_size)
	{
		df::log(__FUNCTION__, std::format("{} is too large to read whole ({})", _h->path(),
		                                  df::file_size(_file_size).str()));
		return {};
	}

	const auto len = static_cast<size_t>(_file_size);
	df::blob result(len);

	if (len != 0)
	{
		// Bypasses the sliding window on purpose: load_buffer would allocate the whole file and
		// then copy it into result. load_buffer always seeks before it reads, so moving the file
		// pointer here cannot disturb the window.
		if (_h->seek(0, platform::file::whence::begin) != 0)
		{
			const auto message = std::format("invalid read_all seek: {}", _h->path());
			df::log(__FUNCTION__, message);
			throw app_exception(message);
		}

		if (_h->read(result.data(), len) != len)
		{
			const auto message = std::format("invalid read_all read: {} {}", len, _h->path());
			df::log(__FUNCTION__, message);
			throw app_exception(message);
		}
	}

	return result;
}

bool files::save(const df::file_path path, const file_load_result& loaded)
{
	const auto save_format = extension_to_format(path.extension());
	ui::const_image_ptr saved;

	if (is_valid(loaded.i) && loaded.i->format() == save_format)
	{
		saved = loaded.i;
	}
	else
	{
		file_encode_params encode_params;
		encode_params.jpeg_save_quality = setting.convert.jpeg_quality;
		encode_params.webp_quality = setting.convert.webp_quality;
		encode_params.webp_lossless = setting.convert.webp_lossless;
		saved = surface_to_image(loaded.to_surface(), {}, encode_params, save_format);
	}

	return is_valid(saved) && blob_save_to_file(saved->data(), path);
}

file_scan_result files::scan_file(const df::file_path path, const bool load_thumb, const file_type_ref ft,
                                  const std::string_view xmp_sidecar, const sizei max_thumb_size,
                                  const scan_intent intent, const bool want_image)
{
	// RAW goes through LibRaw, which opens by path itself, so a handle opened here would be a second
	// open per file that nothing reads.
	auto f = ft->has_trait(file_traits::raw)
		         ? platform::file_ptr{}
		         : open_file(path, platform::file_open_mode::read);

	return scan_file(std::move(f), path, load_thumb, ft, xmp_sidecar, max_thumb_size, intent, want_image);
}

// Overload that scans an ALREADY-OPEN file. Used after replace_file hands back the still-open,
// cache-coherent handle it renamed through, so an edited file is re-scanned via the same handle
// instead of a fresh (possibly stale over SMB) by-name open. Note: the RAW branch (scan_raw) and
// the XMP sidecar are still read BY PATH - the handle only covers the primary media stream, and is
// null for RAW.
file_scan_result files::scan_file(platform::file_ptr f, const df::file_path path, const bool load_thumb,
                                  const file_type_ref ft, const std::string_view xmp_sidecar,
                                  const sizei max_thumb_size, const scan_intent intent, const bool want_image)
{
	file_scan_result result;
	df::bump(df::file_perf.scans);
	record_file_op(ft, file_op_stat::read);
	df::perf_timer timer(df::file_perf.scan_us, &df::file_perf.scan_max_us);

	try
	{
		if (ft->has_trait(file_traits::raw))
		{
			result = scan_raw(path, xmp_sidecar, load_thumb, max_thumb_size, intent);
		}
		else if (f)
		{
			f->seek(0, platform::file::whence::begin);
			const auto file_len = f->size();
			const bool is_bitmap = ft->has_trait(file_traits::bitmap);
			const auto is_small_file = file_len < df::two_fifty_six_k;
			const auto load_from_mem = load_thumb && is_bitmap;
			// blob sizes are size_t; a longer file would be read past the end of a truncated buffer.
			const auto fits_in_memory = file_len == static_cast<size_t>(file_len);

			df::blob data;

			const auto can_load_whole_file = fits_in_memory && file_len <= df::max_blob_size;

			if ((is_small_file || load_from_mem) && can_load_whole_file)
			{
				data.resize(static_cast<size_t>(file_len));
				const auto read = f->read(data.data(), file_len);
				if (read != file_len) return result;
				f->seek(0, platform::file::whence::begin);
			}

			if (is_bitmap)
			{
				if (!data.empty())
				{
					mem_read_stream stream(data);
					result = scan_photo(stream, intent, load_thumb, this);

					// load_image_file re-parses the file and copies it, so it is worth it only to something
					// that will draw or store the result.
					if ((load_thumb || want_image) &&
						data.size() >= sizeof(pack128) && is_image_format(detect_format(stream.peek128(0))))
					{
						result.thumbnail_image = load_image_file(data);
						// The same object: these bytes are the whole file, so a display that wants the
						// written image never has to read it back.
						result.full_image = result.thumbnail_image;
					}
					else
					{
						if (load_thumb)
						{
							// Planar is allowed here: a JPEG or WebP thumbnail reaches the encoder as the
							// planes VP8 wants, skipping the colour conversion in both directions.
							auto s = image_to_surface(data, max_thumb_size, true, decode_intent::thumbnail);

							if (is_valid(s))
							{
								auto i = surface_to_thumbnail(s);

								if (is_valid(i))
								{
									result.thumbnail_image = std::move(i);
								}
							}
						}
					}
				}
				else
				{
					file_read_stream stream;

					if (stream.open(f))
					{
						if (load_from_mem && file_len > df::max_blob_size)
						{
							df::log(__FUNCTION__, std::format("{} is too large to thumbnail ({})", path.name(),
							                                  df::file_size(file_len).str()));
							result = scan_photo(stream, intent, false, this);
						}
						else
						{
							result = scan_photo(stream, intent, load_thumb, this);
						}
					}
				}
			}
			else if (ft->has_trait(file_traits::av))
			{
				// libfiles reads the container. FFmpeg is opened only to decode a thumbnail, or to describe
				// the streams of a file libfiles could not describe whole - when it then keeps libfiles' tags
				// and packet if libfiles read them.
				auto read = scan_media_file(f, intent, true);
				const auto read_whole = read.scan && read.scan->success;
				const auto complete = read_whole && read.scan->complete;
				apply_media_scan(result, std::move(read), complete);
				result.success = complete;

				if (load_thumb || !complete)
				{
					av_format_decoder decoder;

					// An inspect scan reports the file's structure, so it is never held to the metadata budget.
					const auto open_intent = load_thumb
						                         ? media_intent::thumbnail
						                         : (intent == scan_intent::index
							                            ? media_intent::metadata
							                            : media_intent::playback);

					if (decoder.open(f, path, open_intent))
					{
						if (!result.cover_art) result.cover_art = decoder.cover_art();

						if (load_thumb)
						{
							decoder.init_streams(-1, -1, false, true, false);

							int pos_numerator = 10;
							int pos_denominator = 100;

							if (!ft->has_trait(file_traits::thumbnail))
							{
								pos_numerator = 0;
							}

							ui::surface_ptr thumbnail_surface;

							if (decoder.extract_thumbnail(thumbnail_surface, max_thumb_size, pos_numerator,
							                              pos_denominator, false))
							{
								result.thumbnail_surface = std::move(thumbnail_surface);
							}
						}

						if (!complete) decoder.extract_metadata(result, !read_whole);
						result.success = true;
					}
				}

				// Some containers store XMP only in a sidecar rather than embedded — notably
				// MPEG-1/2 program streams, which the Adobe XMP SDK supports via a .xmp sidecar
				// only. If the demuxer surfaced no embedded XMP, merge the sidecar so its
				// tags/ratings/labels are shown and indexed. (#230)
				if (result.metadata.xmp.empty() && !str::is_empty(xmp_sidecar))
				{
					result.metadata.xmp = blob_from_file(path.folder().combine_file(xmp_sidecar));
				}
			}

			if (!data.empty())
			{
				result.crc32c = df::crc32c(data.data(), data.size());
			}
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	return result;
}

ui::image_ptr load_image_file(df::cspan file)
{
	ui::image_ptr result;
	mem_read_stream stream(file);

	if (stream.size() > 16)
	{
		const auto detected = files::detect_format(stream.peek128(0));

		if (is_image_format(detected))
		{
			// want_thumbnail stays false here, and must. scan_exif calls this on an embedded
			// thumbnail, so asking for one again would let a file of nested thumbnails recurse -
			// and scan_jpg carries a 64K buffer per frame, so that ends in a blown stack.
			const auto info = scan_photo(stream);
			const auto* const still = find_still_format(info.format);
			const auto format = still ? still->encoded : ui::image_format::Unknown;

			result = std::make_shared<ui::image>(file, info.dimensions(), format, info.orientation);
		}
		else if (detected != detected_format::Unknown)
		{
			files ff;
			const auto surface = ff.image_to_surface(file, {});
			if (is_valid(surface)) result = save_png(surface, {});
		}
	}

	return result;
}


file_load_result files::load(const df::file_path path, const bool can_load_preview)
{
	df::last_loaded_path.store(path, std::memory_order_relaxed);
	df::bump(df::file_perf.loads);
	df::perf_timer timer(df::file_perf.load_us, &df::file_perf.load_max_us);

	file_load_result result;
	const auto* const mt = file_type_from_name(path);
	record_file_op(mt, file_op_stat::read);

	if (mt->has_trait(file_traits::bitmap))
	{
		if (mt->has_trait(file_traits::raw))
		{
			result = load_raw(path, can_load_preview);
		}
		else
		{
			// Larger than this can hold at once. Reading a prefix would decode a frame whose tail is
			// whatever the codec fills missing rows with, and the edit path would re-encode that over
			// the original. Named as a property of the file, so it is not retried.
			const auto attributes = platform::file_attributes(path);

			if (attributes.size > static_cast<uint64_t>(df::max_blob_size))
			{
				df::log(__FUNCTION__, std::format("{} is too large to load ({})", path.name(),
				                                  df::file_size(attributes.size).str()));
				result.reason = file_load_result::failure::too_large;
				return result;
			}

			const auto file = blob_from_file(path);

			if (!file.empty())
			{
				mem_read_stream stream(file);

				if (stream.size() > 16)
				{
					const auto detected = detect_format(stream.peek128(0));
					const auto* const still = find_still_format(detected);

					if (is_image_format(detected))
					{
						result.i = load_image_file(file);
						result.success = is_valid(result.i);
					}
					else
					{
						// These codecs build the whole frame before anything can be scaled down, so
						// each refuses an oversized source at the point it reads the header and says
						// so here rather than looking like a corrupt file.
						load_diagnostic diagnostic;

						if (still && still->load)
						{
							result.s = still->load(stream, &diagnostic);
						}
						else
						{
							// GIF, BMP and TIFF, plus the bitmap types we recognise by extension but
							// not by signature (TGA, SGI, PPM, DPX), are decoded by ffmpeg. scan_photo
							// reads the geometry from the header without decoding - but only for the
							// formats it has a branch for, so a refusal here is the one that can name the
							// size in the diagnostic, not the one that makes the budget safe. That gate is
							// inside av_decode_still, which sees every source this reaches.
							const auto scanned = scan_photo(stream);
							const sizei scanned_dimensions{
								static_cast<int>(scanned.width), static_cast<int>(scanned.height)
							};

							if (scanned_dimensions.is_empty() ||
								!reject_over_budget_source(&diagnostic, scanned_dimensions, "image"))
							{
								result.s = av_decode_still(file, {}, path.extension(), &diagnostic);
							}
						}

						result.success = is_valid(result.s);

						if (!result.success)
						{
							result.source_dimensions = diagnostic.source_dimensions;
							result.reason = diagnostic.over_budget
								                ? file_load_result::failure::too_large
								                : file_load_result::failure::unreadable;
						}
					}
				}
			}
		}
	}
	else if (mt->has_trait(file_traits::av))
	{
		// Not supported
	}

	return result;
}

static simple_transform angle_to_transform(const int a)
{
	if (a == -90 || a == 270)
	{
		return simple_transform::rot_90;
	}

	if (a == 90 || a == -270)
	{
		return simple_transform::rot_270;
	}

	if (a == 180 || a == -180)
	{
		return simple_transform::rot_180;
	}

	return simple_transform::none;
}


ui::image_format extension_to_format(const std::string_view ext)
{
	static const df::hash_map<std::string_view, ui::image_format, df::ihash, df::ieq> extensions =
	{
		{".jpg", ui::image_format::JPEG},
		{".jpeg", ui::image_format::JPEG},
		{".jpe", ui::image_format::JPEG},
		{".png", ui::image_format::PNG},
		{".webp", ui::image_format::WEBP},
		{"jpg", ui::image_format::JPEG},
		{"jpeg", ui::image_format::JPEG},
		{"jpe", ui::image_format::JPEG},
		{"png", ui::image_format::PNG},
		{"webp", ui::image_format::WEBP},
	};

	const auto found = extensions.find(ext);

	if (found != extensions.end())
	{
		return found->second;
	}

	return ui::image_format::Unknown;
}

ui::image_ptr save_surface(const ui::image_format& format, const ui::const_surface_ptr& surface,
                           const metadata_parts& metadata, const file_encode_params& params)
{
	if (format == ui::image_format::JPEG)
	{
		return save_jpeg(surface, metadata, params);
	}
	if (format == ui::image_format::PNG)
	{
		return save_png(surface, metadata);
	}
	if (format == ui::image_format::WEBP)
	{
		return save_webp(surface, metadata, params);
	}

	return {};
}

// A re-encode copies the source's Exif into the new file. An orientation reset otherwise reaches only
// the XMP, which PNG and WebP do not mirror into the Exif they write, so the copied tag would turn the
// upright pixels a second time. The pixel dimensions move with them.
static metadata_parts upright_save_metadata(const metadata_parts& source, const sizei dimensions)
{
	metadata_parts result;
	result.xmp = source.xmp.clone();
	result.iptc = source.iptc.clone();
	result.icc = source.icc.clone();
	result.photoshop_resources = source.photoshop_resources.clone();

	df::blob exif;
	if (!is_exif_signature(source.exif)) exif.assign(exif_signature.begin(), exif_signature.end());
	exif.insert(exif.end(), source.exif.begin(), source.exif.end());

	result.exif = metadata_exif::fix_dims({exif.data(), exif.size()}, dimensions.cx, dimensions.cy);
	return result;
}


platform::file_op_result files::update_impl(const df::file_path path_src, const df::file_path path_dst,
                                            const metadata_edits& metadata_edits, const image_edits& photo_edits,
                                            const file_encode_params& params, const bool create_original,
                                            const std::string_view src_xmp_name, const std::string_view dst_xmp_name,
                                            const file_update_publish& publish,
                                            bool* const live_file_maybe_changed)
{
	platform::file_op_result result = {platform::file_op_result_code::OK};

	bool temp_file_created = false;
	const auto path_temp = platform::temp_file(path_dst.extension(), path_dst.folder());
	const auto path_rollback = platform::temp_file(path_dst.extension(), path_dst.folder());
	bool rollback_file_created = false;

	// Set when the rollback copy is the last surviving copy of the original bytes, which stops
	// the cleanup below from deleting it.
	bool rollback_holds_sole_original = false;
	xmp_update_result xmp_result;

	// Set only once a write path is entered, so a no-op update records nothing and a failure is
	// attributed to the path that actually produced it.
	file_type_ref op_type = nullptr;
	const df::scope_exit record_write_failure([&result, &op_type]
	{
		if (result.failed() && op_type) record_file_op(op_type, file_op_stat::write_failed);
	});

	// The sidecar the user actually keeps. Nothing writes to it until the media swap below has
	// succeeded; the update stages its own copy beside whichever file is being written.
	const auto path_dst_xmp = dst_xmp_name.empty()
		                          ? path_dst.extension(".xmp")
		                          : path_dst.folder().combine_file(dst_xmp_name);

	try
	{
		bool has_photo_edits = false;

		const auto* const mt = file_type_from_name(path_src);
		const auto extension_change = str::icmp(path_dst.extension(), path_src.extension()) != 0;
		const auto path_change = path_dst != path_src;

		file_scan_result scan_result;

		// The scan only ever sizes a crop and carries the source metadata into a re-encode, so it is
		// worth its read - a full parse plus the embedded thumbnail - only where the pixels may be
		// rewritten. A metadata-only write never rewrites them.
		if (mt->has_trait(file_traits::bitmap) && (extension_change || !photo_edits.is_empty()))
		{
			file_read_stream stream;

			if (stream.open(path_src))
			{
				scan_result = scan_photo(stream);
				has_photo_edits = photo_edits.has_changes(scan_result.dimensions()) || extension_change;
			}
			else
			{
				// Without the source there is no way to honour a crop, a rotation or a re-encode into
				// another format, and falling through would report the no-op return below - or a raw copy
				// under the new extension - as a successful save.
				result.code = platform::file_op_result_code::FAILED;
				result.error_message = "the file could not be opened for reading";
				return result;
			}
		}

		if (!path_change && !has_photo_edits && !metadata_edits.has_changes())
		{
			return result;
		}

		// A metadata-only edit that fits the space the LIVE file holds is patched where it lies:
		// nothing the file holds moves, and libfiles orders the writes so that an interruption leaves
		// a file that reads whole, each field old or new. An edit that does not fit is
		// staged below. A save-as or a pixel edit has to produce a second file, and a backup needs the
		// prior bytes, so none of them is a patch.
		const auto may_patch = !path_change && !has_photo_edits && !create_original &&
			mt->has_trait(file_traits::in_place_metadata);

		if (may_patch)
		{
			// Set before the attempt, so a failure is counted against the format.
			op_type = mt;

			auto patched = false;
			const auto before_attempt = platform::file_attributes(path_dst);

			try
			{
				patched = metadata_xmp::patch(path_dst, metadata_edits, src_xmp_name);
			}
			catch (const app_exception&)
			{
				// A lost race with a reader shows as an opaque open failure, so one retry is worth
				// having - but it cannot be told from any other app_exception, and
				// wait_for_unlocked_write answers true the moment the file is writable, which it also
				// is for a corrupt container that will fail in exactly the same way. This branch
				// writes the LIVE file, so the retry is gated on the file being untouched: an attempt
				// that already wrote part of it must not run again over its own half-finished work.
				const auto after_attempt = platform::file_attributes(path_dst);

				if (after_attempt.modified != before_attempt.modified ||
					after_attempt.size != before_attempt.size)
				{
					if (live_file_maybe_changed) *live_file_maybe_changed = true;
					throw;
				}

				if (!platform::wait_for_unlocked_write(path_dst)) throw;
				if (live_file_maybe_changed) *live_file_maybe_changed = true;
				patched = metadata_xmp::patch(path_dst, metadata_edits, src_xmp_name);
			}

			if (patched)
			{
				record_file_op(mt, file_op_stat::patch_in_place);
				return result;
			}
		}

		// A container with no embedded XMP keeps its metadata in the sidecar, so the media file's
		// bytes never change. Staging and swapping it would copy the whole file - a 60 MB raw for a
		// one-star rating - only to write back what was already there. A backup still stages,
		// because the prior bytes have to be kept before anything is replaced.
		const auto sidecar_only = !path_change && !has_photo_edits && !create_original &&
			!mt->has_trait(file_traits::embedded_xmp);

		if (sidecar_only)
		{
			op_type = mt;
			record_file_op(mt, file_op_stat::sidecar);

			const auto path_temp_xmp = path_temp.extension(".xmp");
			auto committed = false;

			// Nothing outside this scope knows the staged path, so a throw would strand it.
			const df::scope_exit discard_stage([path_temp_xmp, &committed]
			{
				if (!committed) platform::delete_file(path_temp_xmp);
			});

			// Staged like every other write: the live sidecar is replaced only once the new one is
			// complete on disk.
			const auto staged = metadata_xmp::update(path_dst, path_src, metadata_edits, src_xmp_name,
			                                         path_temp_xmp);
			result = platform::replace_file(path_dst_xmp, staged.xmp_path, false);
			committed = result.success();

			// This handle refers to the sidecar, and a caller uses one to re-scan the media file.
			// With no handle the caller re-scans by name with force set, which is what the raw
			// branch does anyway.
			result.coherent_handle.reset();

			return result;
		}

		// Set when the stage was written with the metadata edit already in it.
		auto staged_with_edit = false;

		if (has_photo_edits)
		{
			const auto loaded = load(path_src, false);

			if (!loaded.success)
			{
				// Without a decoded source there is nothing to write; carrying on would
				// hand a non-existent temp file to the metadata update and the replace.
				result.code = platform::file_op_result_code::FAILED;
			}
			else
			{
				const auto dimensions_in = loaded.dimensions();
				const auto dst_path_is_jpeg = is_jpeg(path_dst);
				const auto jpeg_to_jpeg = ui::is_jpeg(loaded.i) && dst_path_is_jpeg;

				df::blob transformed;
				auto rotate_over_budget = false;

				if (jpeg_to_jpeg && photo_edits.is_no_loss(dimensions_in) && params.jpeg_save_quality >= 75)
				{
					// Empty when the rotation would not be lossless, which the re-encode below handles.
					transformed = _jpeg_transformer.transform(loaded.i->data(), _jpeg_encoder,
					                                      angle_to_transform(
						                                      df::round(photo_edits.rotation_angle())),
					                                      &rotate_over_budget);
				}

				if (rotate_over_budget)
				{
					// Lossless was possible and refused for the coefficients it would hold. The
					// re-encode would hold the decoded picture and a rotated copy - more than that -
					// and give up quality besides, so the edit fails rather than falling back.
					result.code = platform::file_op_result_code::FAILED;
				}
				else if (!transformed.empty())
				{
					// The file is created before a short write can fail, so the stage is claimed for
					// cleanup either way; otherwise a full disk leaves a truncated diffractor_* file
					// beside the user's photo for the indexer to find.
					temp_file_created = true;

					if (!blob_save_to_file(transformed, path_temp))
					{
						result.code = platform::file_op_result_code::FAILED;
					}
				}
				else
				{
					const auto surface_in = loaded.to_surface();
					const auto temp_surface = is_empty(surface_in)
						                          ? ui::const_surface_ptr{}
						                          : apply_image_edits(surface_in, photo_edits);

					if (is_empty(temp_surface))
					{
						result.code = platform::file_op_result_code::FAILED;
					}
					else
					{
						auto encode_params = params;

						// Re-encoding against the source's own tables costs one generation at its
						// fidelity rather than re-quantizing to the quality slider.
						if (jpeg_to_jpeg) encode_params.jpeg_source = loaded.i->data();

						metadata_parts save_meta;
						const auto& source_meta = scan_result.save_metadata(save_meta);
						metadata_parts upright_meta;
						const auto reset_orientation = metadata_edits.orientation.has_value() &&
							*metadata_edits.orientation == ui::orientation::top_left && !source_meta.exif.empty();
						if (reset_orientation)
						{
							upright_meta = upright_save_metadata(source_meta, temp_surface->dimensions());
						}

						const auto saved = save_surface(extension_to_format(path_temp.extension()), temp_surface,
						                                reset_orientation ? upright_meta : source_meta,
						                                encode_params);

						if (is_empty(saved))
						{
							result.code = platform::file_op_result_code::FAILED;
						}
						else
						{
							// Claimed before the write, for the same reason as the branch above.
							temp_file_created = true;

							if (!blob_save_to_file(saved->data(), path_temp))
							{
								result.code = platform::file_op_result_code::FAILED;
							}
						}
					}
				}
			}
		}
		else
		{
			// Always staged, whatever the size. Handing the original to the metadata update lets a
			// crash, a full disk or a handler fault mid-rewrite leave the user with a corrupt file
			// and nothing to roll back to.
			// Claimed before anything is written: CopyFile leaves the partial destination behind when it
			// runs out of disk part way, as a failed write does, and this is the branch every
			// metadata-only staged write takes.
			temp_file_created = true;

			// A packet libfiles writes is staged with the edit already in it - in one pass from the
			// source when the edit moves what the file holds, where a copy would then be rewritten in
			// full. Anything else is copied, and updated below.
			if (const auto staged = metadata_xmp::stage_update(path_temp, path_src, metadata_edits, src_xmp_name))
			{
				xmp_result = *staged;
				staged_with_edit = true;
			}
			else
			{
				result = platform::copy_file(path_src, path_temp, true, false);
			}
		}

		if (result.success() && !staged_with_edit)
		{
			const auto has_metadata_changes = metadata_edits.has_changes();

			if (has_metadata_changes || path_change || has_photo_edits)
			{
				// Both destinations are staged; neither the live media file nor the live sidecar
				// is touched until the swap below.
				xmp_result = metadata_xmp::update(path_temp, path_src, metadata_edits, src_xmp_name,
				                                  path_temp.extension(".xmp"));
			}
		}

		if (result.success() && temp_file_created)
		{
			const auto destination_existed = path_dst.exists();
			if (!xmp_result.xmp_path.is_empty() && destination_existed)
			{
				const auto rollback_copy = platform::copy_file(path_dst, path_rollback, true, false);
				if (rollback_copy.failed()) result = rollback_copy;
				else rollback_file_created = true;
			}

			if (result.success())
			{
				op_type = mt;
				record_file_op(mt, file_op_stat::replace);
				if (test_before_file_update_publish) test_before_file_update_publish(path_dst);
				if (publish.fail_if_exists)
				{
					result = platform::move_file(path_temp, path_dst, true);
					temp_file_created = result.failed();
				}
				else
				{
					result = platform::replace_file(path_dst, path_temp, create_original,
					                                publish.require_unchanged_destination
						                                ? std::optional{publish.destination_attributes}
						                                : std::optional<platform::file_attributes_t>{});
				}
			}

			if (result.success() && !xmp_result.xmp_path.is_empty())
			{
				// Preserve the primary media file's coherent handle (and its modified time)
				// across the sidecar swap - the sidecar replace returns its own result and
				// would otherwise clobber the handle the caller needs for the immediate rescan.
				auto media_handle = result.coherent_handle;
				const auto media_modified = result.modified;

				const auto path_temp_xmp = xmp_result.xmp_path;
				const auto xmp_replace = publish.fail_if_exists
					                         ? platform::move_file(path_temp_xmp, path_dst_xmp, true)
					                         : platform::replace_file(path_dst_xmp, path_temp_xmp, create_original);

				if (xmp_replace.failed())
				{
					media_handle.reset();
					auto rollback_result = platform::file_op_result{platform::file_op_result_code::OK};
					if (rollback_file_created)
					{
						rollback_result = platform::replace_file(path_dst, path_rollback, false);
						rollback_file_created = rollback_result.failed();

						// The swap consumed the rollback copy on success. On failure the destination
						// holds the new bytes and this copy is the only original left, so name it in
						// the log - it survives under a temp name that means nothing to the user.
						rollback_holds_sole_original = rollback_file_created;

						if (rollback_holds_sole_original)
						{
							df::log(__FUNCTION__, std::format("could not restore {}; original retained as {}",
							                                  path_dst, path_rollback));
						}
					}
					else
					{
						rollback_result = platform::delete_file(path_dst);
					}

					result = rollback_result.failed() ? rollback_result : xmp_replace;
				}
				else
				{
					result.coherent_handle = std::move(media_handle);
					result.modified = media_modified;
				}
			}
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());

		result.code = platform::file_op_result_code::FAILED;
		result.error_message = str::utf8_cast(e.what());
	}

	if (result.failed())
	{
		if (temp_file_created) platform::delete_file(path_temp);
		// The staged sidecar is cleaned up by its own path rather than the returned one: a throw out
		// of metadata_xmp::update leaves the result unassigned, and the file it had already created
		// would then survive. Only ever a staged copy; the live sidecar is never the cleanup target.
		if (temp_file_created) platform::delete_file(path_temp.extension(".xmp"));
	}
	// Unconditional rather than gated on rollback_file_created, which means "a usable copy exists" and
	// so is false when CopyFile ran out of disk part way and left a partial one. The path is a
	// reserved temp name, so deleting it when nothing was written is a no-op.
	if (!rollback_holds_sole_original) platform::delete_file(path_rollback);

	return result;
}

file_update_result files::update(const df::file_path path_src, const df::file_path path_dst,
                                 const metadata_edits& metadata_edits, const image_edits& photo_edits,
                                 const file_encode_params& params, const bool create_original,
                                 const std::string_view src_xmp_name, const std::string_view dst_xmp_name,
                                 const rescan_spec& rescan, const file_update_publish& publish)
{
	file_update_result result;
	bool live_file_maybe_changed = false;
	static_cast<platform::file_op_result&>(result) = update_impl(path_src, path_dst, metadata_edits, photo_edits,
	                                                             params, create_original, src_xmp_name, dst_xmp_name,
	                                                             publish, &live_file_maybe_changed);
	result.live_file_maybe_changed = result.failed() && live_file_maybe_changed;

	// Only replace_file hands back a handle, and only for the media file, so this distinguishes a
	// staged-and-swapped write from an in-place patch or a sidecar-only write.
	result.staged = result.coherent_handle != nullptr;

	// Consumed here, so the cache-coherent handle never escapes by accident. Every reader of the new
	// bytes is served from what follows.
	platform::file_ptr handle = std::move(result.coherent_handle);
	result.coherent_handle.reset();

	if (result.success() && rescan.wanted && rescan.file_type)
	{
		// The caller's sidecar name is captured before the write, so it is empty for the write that
		// creates the sidecar. Resolve it here or the scan reads the media file alone.
		auto xmp_sidecar = rescan.xmp_sidecar;

		if (xmp_sidecar.empty())
		{
			const auto path_xmp = dst_xmp_name.empty()
				                      ? path_dst.extension(".xmp")
				                      : path_dst.folder().combine_file(dst_xmp_name);
			if (path_xmp.exists()) xmp_sidecar = path_xmp.name();
		}

		result.coherent = handle != nullptr;
		result.scan = handle
			              ? scan_file(handle, path_dst, rescan.load_thumbnail, rescan.file_type, xmp_sidecar,
			                          rescan.max_thumb_size, rescan.intent, rescan.want_image)
			              : scan_file(path_dst, rescan.load_thumbnail, rescan.file_type, xmp_sidecar,
			                          rescan.max_thumb_size, rescan.intent, rescan.want_image);
		result.scanned = result.scan.success;

		// Hand the display the bytes that were just written, so nothing reads the file back to draw
		// it. Only directly displayable formats arrive here; the rest fall back to a by-name load.
		if (result.scanned && rescan.want_image && is_valid(result.scan.full_image))
		{
			result.loaded.success = true;
			result.loaded.i = result.scan.full_image;
		}
	}

	// The one deliberate hand-over, and a move so exactly one owner holds it: the caller reopens this
	// file for playback next, and opening the path again is the stale read we are avoiding.
	if (result.success() && rescan.want_handle)
	{
		result.display_handle = std::move(handle);
	}

	return result;
}

// libarchive wants the native path in the platform's own spelling, and the two entry points differ
// in more than their character type: the wide one is what carries the \\?\ prefix a long Windows
// path needs, while a POSIX path is bytes all the way down and has no wide form to convert to.
// Overloading on platform::native_path lets the call site stay free of a conditional.
static int archive_read_open_native(archive* a, const std::wstring& path, const size_t block_size)
{
	return archive_read_open_filename_w(a, path.c_str(), block_size);
}

static int archive_read_open_native(archive* a, const std::string& path, const size_t block_size)
{
	return archive_read_open_filename(a, path.c_str(), block_size);
}

static df::date_t archive_entry_created(archive_entry* entry)
{
	if (!archive_entry_ctime_is_set(entry)) return {};
	const auto seconds = static_cast<int64_t>(archive_entry_ctime(entry));
	const auto nanos = archive_entry_ctime_nsec(entry);
	constexpr auto ticks = df::date_t::intervals_per_second;
	constexpr auto epoch_seconds = static_cast<int64_t>(df::date_t::offset_1970 / ticks);
	if (nanos < 0 || nanos >= 1000000000 || seconds < -epoch_seconds ||
		seconds > static_cast<int64_t>((UINT64_MAX - 9999999) / ticks) - epoch_seconds)
	{
		df::log_once(__FUNCTION__, "Archive timestamp is outside the supported range");
		return {};
	}
	return df::date_t(static_cast<uint64_t>(seconds + epoch_seconds) * ticks + static_cast<uint64_t>(nanos) / 100);
}

std::vector<archive_item> files::list_archive(const df::file_path zip_file_path)
{
	std::vector<archive_item> results;
	auto* const a = archive_read_new();

	if (!a)
		return results;

	// Entries are attacker-controlled; a crafted archive can declare millions of them.
	constexpr size_t max_entries = 100 * 1000;

	// RAII so a throw from str::utf8_cast or emplace_back cannot leak the handle
	// and libarchive's decompression buffers.
	const df::scope_exit free_archive([a] { archive_read_free(a); });

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);

	const auto native = platform::to_file_system_path(zip_file_path);

	if (archive_read_open_native(a, native, 10240) == ARCHIVE_OK)
	{
		const df::scope_exit close_archive([a] { archive_read_close(a); });

		archive_entry* entry = nullptr;

		for (auto examined = 0_z; examined < max_entries; ++examined)
		{
			const auto status = archive_read_next_header(a, &entry);
			if (status == ARCHIVE_EOF) break;
			if (status != ARCHIVE_OK)
			{
				const auto* error = archive_error_string(a);
				df::log_once(__FUNCTION__, error ? error : "Cannot read archive entry");
				if (status != ARCHIVE_WARN) break;
			}
			if (!entry)
			{
				df::log_once(__FUNCTION__, "Archive reader returned no entry");
				break;
			}

			// Null for entries whose stored name is not valid UTF-8, which is common
			// in older zip/rar archives using a local code page.
			const auto* const pathname = archive_entry_pathname_utf8(entry);

			if (!pathname)
				continue;

			archive_item result_info;
			result_info.filename = str::utf8_cast(pathname);
			result_info.uncompressed_size = df::file_size(archive_entry_size(entry));
			result_info.created = archive_entry_created(entry);
			results.emplace_back(std::move(result_info));
		}
	}

	return results;
}

bool language_matches_ui(const std::string_view iso639, const std::string_view ui_language)
{
	if (ui_language.empty()) return false;

	struct lang_map
	{
		std::string_view ui2;
		std::string_view iso3;
	};
	static constexpr lang_map map[] = {
		{"en", "eng"}, {"de", "deu"}, {"de", "ger"}, {"cs", "ces"}, {"cs", "cze"},
		{"es", "spa"}, {"fr", "fra"}, {"fr", "fre"}, {"it", "ita"}, {"ja", "jpn"},
		{"ko", "kor"}, {"pl", "pol"}, {"pt", "por"}, {"ru", "rus"}, {"tr", "tur"},
		{"uk", "ukr"}, {"zh", "zho"}, {"zh", "chi"}, {"nl", "nld"}, {"nl", "dut"},
		{"br", "bre"}, {"lv", "lav"}, {"sr", "srp"}, {"sr", "scc"},
	};

	for (const auto& m : map)
	{
		if (str::icmp(ui_language, m.ui2) == 0 && str::icmp(iso639, m.iso3) == 0)
			return true;
	}

	return false;
}

// The app UI language is a 2-letter code (app_settings::language). FFmpeg tags id3v2
// COMM/USLT comments with a lowercase ISO 639-2 3-letter code taken verbatim from the
// file, so either the bibliographic (e.g. "ger") or terminological ("deu") variant may
// appear. Returns true when `key` is exactly "comment-<code>" for a code matching ui_lang.
static bool comment_key_matches_ui_language(const std::string_view key, const std::string_view ui_lang)
{
	constexpr std::string_view prefix = "comment-";
	if (key.size() != prefix.size() + 3) return false; // only the plain "comment-<lang>" form
	return language_matches_ui(key.substr(prefix.size()), ui_lang);
}

void file_scan_result::parse_metadata_ffmpeg_kv(prop::item_metadata& result) const
{
	// FFmpeg surfaces id3v2 COMM/USLT frames with a language/descriptor suffix
	// ("comment-eng", "comment-deu", ...); untagged ("und"/"xxx") comments use the plain
	// "comment" key. We show a single comment, preferring the plain/untagged one, then a
	// comment in the current UI language, then any remaining comment. The full set of
	// per-language comments stays visible in the verbose metadata panel (see to_info()).
	const auto ui_lang = setting.language;
	int comment_priority = 0; // 0 none, 1 other language, 2 UI language, 3 plain/untagged

	for (const auto& kv : ffmpeg_metadata)
	{
		if (is_key(kv.key, "album")) result.album = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "show")) result.show = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "programme")) result.show = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "album_artist")) result.album_artist = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "artist")) result.artist = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "comment") || str::starts(kv.key, "comment-"))
		{
			// Pick the best single comment: plain/untagged > current UI language > any other.
			const auto priority = is_key(kv.key, "comment")
				                      ? 3
				                      : comment_key_matches_ui_language(kv.key, ui_lang)
				                      ? 2
				                      : 1;

			if (priority > comment_priority)
			{
				comment_priority = priority;
				result.comment = str::strip_and_cache(kv.value);
			}
		}
		else if (is_key(kv.key, "description")) result.description = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "composer")) result.composer = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "copyright")) result.copyright_notice = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "creation_time") || is_key(kv.key, "date") || is_key(
			kv.key, "com.apple.quicktime.creationdate"))
		{
			if (kv.value.size() == 4 || str::ends(kv.value, "00-00"))
			{
				const auto year = str::to_int(kv.value);

				if (year > 1800 && year < 2100)
				{
					result.year = year;
				}
			}
			else
			{
				const auto date = df::date_t::from(kv.value);

				if (date.is_valid())
				{
					result.dates.add_utc(prop::date_source::container_created, date);
					result.year = date.year();
				}
			}
		}
		else if (is_key(kv.key, "date-eng") || is_key(kv.key, "Rip date"))
		{
			const auto date = df::date_t::from(kv.value);
			if (date.is_valid())
			{
				// A wall-clock reading like TDOR below, not an instant: a rip tag carries no zone, so
				// storing it as UTC would shift it by the scanning machine's offset and move a
				// midnight-valued rip date onto the previous day.
				result.dates.add(prop::date_source::rip_date, date);
			}
		}
		// TDOR is the recording's own release date, which for a rip is decades before the file. It
		// is a wall-clock reading rather than an instant: nobody wrote a zone for 1969. TORY is the
		// same fact in ID3v2.3, and FFmpeg passes both through under their raw four-character name.
		else if (is_key(kv.key, "TDOR") || is_key(kv.key, "TORY") ||
			is_key(kv.key, "originalyear") || is_key(kv.key, "original_year"))
		{
			const auto date = df::date_t::from(kv.value);
			if (date.is_valid())
			{
				result.dates.add(prop::date_source::id3_original_release, date);
			}
			else if (kv.value.size() == 4 && str::is_num(kv.value))
			{
				const auto year = str::to_int(kv.value);
				if (year > 1800 && year < 2100) result.dates.add(prop::date_source::id3_original_release,
				                                                df::date_t(year, 1, 1, 0, 0, 0));
			}
		}
		else if (is_key(kv.key, "encoder") || is_key(kv.key, "encoded_by"))
			result.encoder =
				str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "genre")) result.genre = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "publisher")) result.publisher = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "synopsis")) result.synopsis = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "title")) result.title = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "maker") || is_key(kv.key, "make") || is_key(kv.key, "com.apple.quicktime.make"))
		{
			// AVI normalises to "maker", MP4/MOV to "make"; both mean camera manufacturer.
			result.camera_manufacturer = str::strip_and_cache(kv.value);
		}
		else if (is_key(kv.key, "model") || is_key(kv.key, "com.apple.quicktime.model") || is_key(
			kv.key, "model-eng"))
		{
			result.camera_model = str::strip_and_cache(kv.value);
		}
		else if (is_key(kv.key, "performer")) result.performer = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "year")) result.year = str::to_int(kv.value);
		else if (is_key(kv.key, "disk") || is_key(kv.key, "disc")) result.disk = df::xy8::parse(kv.value);
		else if (is_key(kv.key, "track")) result.track = df::xy8::parse(kv.value);
		else if (is_key(kv.key, "variant_bitrate")) result.bitrate = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "episode_sort")) result.episode = df::xy8::parse(kv.value);
		else if (is_key(kv.key, "season_number")) result.season = str::to_int(kv.value);
		else if (is_key(kv.key, "system")) result.system = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "game")) result.game = str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "song") && prop::is_null(result.title))
			result.title =
				str::strip_and_cache(kv.value);
		else if (is_key(kv.key, "compatible_brands") || is_key(kv.key, "minor_version"))
		{
			// compatible_brands: 3gp4, avc1isom, isomavc1, isomiso2avc1mp41, isomiso2mp41, isommp42, M4A mp42isom, mp41isom, mp42mp41isomavc1, qt
			// minor_version: 3gp4, avc1isom, isomavc1, isomiso2avc1mp41, isomiso2mp41, isommp42, M4A mp42isom, mp41isom, mp42mp41isomavc1, qt
		}
		else if (is_key(kv.key, "rating"))
		{
			result.rating = std::clamp(str::to_int(kv.value), 0, 5);
		}
		else if (is_key(kv.key, "WM/SharedUserRating"))
		{
			// Windows System.Rating is a 0-99 scale (Explorer / Media Player); map it to
			// Diffractor's 0-5 stars. A corresponding XMP property, when present, is
			// parsed afterwards and remains authoritative.
			const auto r = str::to_int(kv.value);
			if (r > 0)
				result.rating = (r <= 12) ? 1 : (r <= 37) ? 2 : (r <= 62) ? 3 : (r <= 87) ? 4 : 5;
		}
		else if (is_key(kv.key, "keywords"))
		{
			str::split2(kv.value, true, [this](const std::string_view text)
			{
				keywords.emplace_back(str::cache(text));
			});
		}
		else if (is_key(kv.key, "WM/Category"))
		{
			// Windows Explorer / Media Player tags (MP4 'Xtra' atom, ASF 'WM/*').
			// Values are ';'-delimited and may contain spaces, so split only on ';'.
			str::split2(kv.value, true, [this](const std::string_view text)
			            {
				            windows_categories.emplace_back(str::cache(str::trim(text)));
			            }, [](const char c) { return c == ';'; });
		}
		else if (is_key(kv.key, "location-eng") || is_key(kv.key, "location") || is_key(
			kv.key, "com.apple.quicktime.location.ISO6709"))
		{
			const auto loc = split_location(kv.value);

			if (loc.success)
			{
				gps = gps_coordinate(loc.x, loc.y);
			}
		}
		else
		{
		}
	}
}

const metadata_parts& file_scan_result::save_metadata(metadata_parts& fallback) const
{
	if (metadata.exif.empty() &&
		metadata.iptc.empty() &&
		metadata.xmp.empty())
	{
		fallback.exif = metadata_exif::make_exif(to_props());
		// An image whose only metadata is a colour profile still has to keep it, or a crop or
		// resize silently reinterprets a wide gamut image as sRGB.
		fallback.icc = metadata.icc.clone();
		return fallback;
	}

	return metadata;
}

prop::item_metadata_ptr file_scan_result::to_props() const
{
	auto result = std::make_shared<prop::item_metadata>();

	if (tiff_metadata)
	{
		metadata_exif::parse(*result, *tiff_metadata);
	}
	else if (!metadata.exif.empty())
	{
		metadata_exif::parse(*result, metadata.exif);
	}

	if (!metadata.iptc.empty())
	{
		metadata_iptc::parse(*result, metadata.iptc);
	}

	if (!ffmpeg_metadata.empty())
	{
		parse_metadata_ffmpeg_kv(*result);
	}

	if (media)
	{
		parse_media_properties(*result);
	}

	if (!metadata.xmp.empty())
	{
		metadata_xmp::parse(*result, metadata.xmp);
	}

	// Most legacy fields are 16 bit in the metadata record. High ISO and a stitched panorama past
	// 65535 pixels exceed that, so saturate - wrapping would report a 70000 pixel stitch as 4464,
	// which is a shape no panorama test can recognise. Audio sample rate is stored wider below.
	const auto to_u16 = [](const int64_t v)
	{
		return static_cast<uint16_t>(std::clamp<int64_t>(v, 0, UINT16_MAX));
	};

	if (created_local.is_valid()) result->dates.add(created_local_source, created_local);
	if (created_utc.is_valid()) result->dates.add_utc(prop::date_source::embedded_created, created_utc);
	if (prop::is_null(result->iso_speed)) result->iso_speed = to_u16(iso_speed);
	if (prop::is_null(result->exposure_time)) result->exposure_time = exposure_time;
	if (prop::is_null(result->f_number)) result->f_number = f_number;
	if (prop::is_null(result->focal_length)) result->focal_length = focal_length;
	if (prop::is_null(result->comment)) result->comment = comment;
	if (prop::is_null(result->artist)) result->artist = artist;
	if (prop::is_null(result->camera_model)) result->camera_model = camera_model;
	if (prop::is_null(result->camera_manufacturer)) result->camera_manufacturer = camera_manufacturer;
	if (prop::is_null(result->video_codec)) result->video_codec = video_codec;
	if (prop::is_null(result->copyright_notice)) result->copyright_notice = copyright_notice;
	if (prop::is_null(result->title)) result->title = title;
	if (prop::is_null(result->duration)) result->duration = df::round(duration);
	if (prop::is_null(result->audio_codec)) result->audio_codec = audio_codec;
	if (prop::is_null(result->audio_channels)) result->audio_channels = audio_channels;
	if (prop::is_null(result->audio_sample_type)) result->audio_sample_type = static_cast<uint16_t>(audio_sample_type);
	if (prop::is_null(result->audio_sample_rate))
		result->audio_sample_rate = static_cast<uint32_t>(std::clamp<int64_t>(audio_sample_rate, 0, UINT32_MAX));

	if (!result->coordinate.is_valid())
	{
		result->coordinate = gps;
	}

	const auto xmp_properties = metadata_xmp::properties(metadata.xmp);
	if (!result->tags.is_empty() || (!keywords.empty() && !xmp_properties.tags) ||
		(!windows_categories.empty() && !xmp_properties.tags))
	{
		auto tags = split(result->tags, true);

		// Native container keywords (the MP4 'KEYW' atom, or Windows Explorer /
		// Media Player tags from the 'Xtra' atom / ASF 'WM/Category') are only merged when
		// XMP has no dc:subject. An unrelated XMP packet says nothing about tags, while an
		// empty dc:subject is authoritative and must not resurrect stale native tags.
		if (!xmp_properties.tags)
		{
			tags.insert(tags.end(), keywords.begin(), keywords.end());
			tags.insert(tags.end(), windows_categories.begin(), windows_categories.end());
		}

		std::ranges::sort(tags, str::iless());
		tags.erase(std::ranges::unique(tags, df::ieq()).begin(), tags.end());
		result->tags = str::cache(str::combine(tags));
	}

	if (!prop::is_null(width)) result->width = to_u16(width);
	if (!prop::is_null(height)) result->height = to_u16(height);
	if (!prop::is_null(pixel_format)) result->pixel_format = pixel_format;
	if (orientation_applied || orientation != ui::orientation::top_left) result->orientation = orientation;

	return result;
}

void add_structure_row(metadata_kv_list& kv, const std::string_view key, std::string value,
                       const std::string_view shape, std::string detail)
{
	auto& row = kv.emplace_back(std::string(key), std::move(value));
	row.depth = 1;
	if (!shape.empty()) row.shape = shape;
	if (!detail.empty()) row.detail = metadata_text_detail{std::move(detail)};
}

void add_structure_bytes(metadata_kv_list& kv, const std::string_view key, std::string value,
                         const std::string_view shape, const uint8_t* payload, const size_t payload_len)
{
	auto& row = kv.emplace_back(std::string(key), std::move(value));
	row.depth = 1;
	if (!shape.empty()) row.shape = shape;

	if (payload && payload_len > 0)
	{
		// Only as much as a dump would ever list is kept, so an oversized payload costs no more.
		const auto kept = std::min(payload_len, str::max_hex_dump_bytes);
		row.detail = metadata_binary_detail{std::vector<uint8_t>(payload, payload + kept)};
	}
}

void add_structure_section(metadata_kv_list& kv, const std::string_view key, const std::string_view id,
                           const bool open_by_default)
{
	auto& row = kv.emplace_back(std::string(key), std::string{});
	row.container = true;
	row.id = id;
	row.open_by_default = open_by_default;
}

void finish_structure_sections(metadata_kv_list& kv)
{
	metadata_kv_list kept;
	kept.reserve(kv.size());

	for (size_t i = 0; i < kv.size(); ++i)
	{
		if (kv[i].container)
		{
			size_t count = 0;
			while (i + 1 + count < kv.size() && !kv[i + 1 + count].container) ++count;
			if (count == 0) continue;

			kv[i].key = std::format("{} ({})", kv[i].key, count);
		}

		kept.emplace_back(std::move(kv[i]));
	}

	kv = std::move(kept);
}

// A row may carry a complete encoded image rather than an opaque payload. Decoding belongs here, on
// the scan worker, because the pane draws what it is given and never decodes while painting. What
// will not decode keeps its bytes and stays a hex dump, trimmed back to dump size.
static void decode_embedded_images(metadata_kv_list& kv)
{
	files ff;

	// Bigger than the pane will ever draw. A ceiling only ever reduces, so a small payload still
	// decodes at its own size.
	constexpr sizei preview_limit{1024, 1024};

	for (auto& row : kv)
	{
		auto* const binary = std::get_if<metadata_binary_detail>(&row.detail);

		if (!binary || !binary->is_image || binary->bytes.empty()) continue;

		// The declared size is the file's, not ours, and a header can claim far more than the bytes
		// that follow it. Header-scan first and refuse the ones that would allocate past the decode
		// budget: this is a row in a properties pane, not a reason to take a gigabyte. A header this
		// cannot read is not waved through - every decoder image_to_surface can reach carries the same
		// budget refusal, including the ffmpeg fallback.
		const df::cspan bytes{binary->bytes.data(), binary->bytes.size()};
		mem_read_stream header_stream(bytes);
		const auto scanned = scan_photo(header_stream);
		const sizei scanned_dimensions{static_cast<int>(scanned.width), static_cast<int>(scanned.height)};

		if (!scanned_dimensions.is_empty() && files::exceeds_decode_budget(scanned_dimensions)) continue;

		auto surface = ff.image_to_surface(bytes, preview_limit, false, decode_intent::thumbnail);

		if (ui::is_valid(surface))
		{
			row.detail = metadata_image_detail{std::move(surface)};
			// An image is the point of the row, so it is shown without being asked for.
			row.open_by_default = true;
		}
		else
		{
			binary->is_image = false;
			if (binary->bytes.size() > str::max_hex_dump_bytes) binary->bytes.resize(str::max_hex_dump_bytes);
		}
	}
}

av_media_info file_scan_result::to_info() const
{
	av_media_info result;

	if (media)
	{
		describe_media_streams(*media, result);
		add_media_blocks(*media, result, false);
	}
	else if (!ffmpeg_metadata.empty())
	{
		result.metadata.emplace_back(metadata_standard::media, ffmpeg_metadata);
	}

	if (!libraw_metadata.empty())
	{
		result.metadata.emplace_back(metadata_standard::raw, libraw_metadata);
	}

	if (!metadata.exif.empty())
	{
		auto kv = metadata_exif::to_info(metadata.exif);
		const auto parsed = !kv.empty();
		result.metadata.emplace_back(metadata_standard::exif, std::move(kv), metadata.exif.size(), parsed);
	}

	if (!metadata.iptc.empty())
	{
		auto kv = metadata_iptc::to_info(metadata.iptc);
		const auto parsed = !kv.empty();
		result.metadata.emplace_back(metadata_standard::iptc, std::move(kv), metadata.iptc.size(), parsed);
	}

	if (!metadata.xmp.empty())
	{
		bool parsed = false;
		auto kv = metadata_xmp::to_info(metadata.xmp, parsed);

		// The packet is the block's real content, so it stays reachable whether or not the toolkit
		// could make a tree from it.
		constexpr auto max_raw_bytes = 256_z * 1024_z;
		std::string raw;
		raw.assign(std::bit_cast<const char*>(metadata.xmp.data()),
		           std::min(metadata.xmp.size(), max_raw_bytes));

		result.metadata.emplace_back(metadata_standard::xmp, std::move(kv), metadata.xmp.size(), parsed,
		                             std::move(raw));
	}

	if (!metadata.icc.empty())
	{
		auto kv = metadata_icc::to_info(metadata.icc);
		const auto parsed = !kv.empty();
		result.metadata.emplace_back(metadata_standard::icc, std::move(kv), metadata.icc.size(), parsed);
	}

	if (!structure_metadata.empty())
	{
		result.metadata.emplace_back(metadata_standard::structure, structure_metadata);
	}

	for (auto& block : result.metadata)
	{
		decode_embedded_images(block.values);
	}

	return result;
}
