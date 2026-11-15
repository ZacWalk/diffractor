// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Diffractor's file handling over libfiles: loading, saving and format detection for images and
// media files; the still formats libfiles names from their signatures; scanning stills, audio and video
// onto an item, with the art and the media manager's .nfo kept beside music and films; the JPEG, PNG,
// WebP and PSD codecs held to Diffractor's surfaces, settings and decode budget; and Commodore 64 images, drawn
// and listed onto an item, and SID tunes. HEIF, JPEG XL and camera raw, each built only with its library, are
// files_heif.cpp, files_jxl.cpp and files_raw.cpp.

#include "pch.h"

#include "files.h"
#include "app_text.h"
#include "av_format.h"
#include "av_sid.h"
#include "metadata.h"
#include "util_text.h"

#include "libfiles/decode.h"
#include "libfiles/encode.h"
#include "libfiles/hdr.h"
#include "libfiles/icc.h"
#include "libfiles/media.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"

#include "machine.h"

#define LIBARCHIVE_STATIC
#include <archive.h>
#include <archive_entry.h>

// windows.h arrives through the vendored headers above and brings the min/max function macros with
// it, which swallow the std:: calls further down this file. Both go, not just min.
#undef GetObject
#undef min
#undef max

///////////////////////////////////////////////////////////////////////////////////////////////////
// Core file handling operations. Manages file loading, saving, and format detection for images and media
// files. Coordinates metadata extraction across different file types, with the art and the media
// manager's .nfo kept beside music and films, and names each type's sidecars.

df_assert_move_only(file_scan_result);
df_assert_move_only(libfiles::metadata_blocks);
df_assert_movable(metadata_kv_list);
df_assert_movable(file_load_result);

std::string file_group::display_name(const bool is_plural) const
{
	const auto result = is_plural && !core::is_empty(plural_name) ? plural_name : name;
	return tt.translate_text(std::string(result));
}

core::const_surface_ptr file_type::default_thumbnail() const
{
	return render::create_icon_surface(static_cast<char32_t>(group->icon));
}

struct file_type_config
{
	// Replaced whole, never edited in place. parse_file_group reads it on workers - auto-complete and
	// query - while a language switch registers that language's spellings on the UI thread, and an
	// insert that rehashed under a lookup left the reader walking freed buckets.
	std::atomic<std::shared_ptr<const file_group_by_name>> groups_by_name{std::make_shared<const file_group_by_name>()};
	file_type_by_extension types_by_name;

	std::vector<file_group_ref> groups;
	std::vector<file_type> types;
};

static file_type_config s_config;

bool file_tool::invoke(const core::file_path path) const
{
	if (exe_path.is_empty()) return false;

	auto substitute = [&](std::ostringstream& result, const std::string_view token)
	{
		if (token == "item-path") result << core::quote_if_white_space(path.str());
		else if (token == "exe-path") result << core::quote_if_white_space(exe_path.str());
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
			if (!core::is_empty(name)) groups->try_emplace(core::cache(name), g);
		};

		add(g->plural_name);
		add(g->display_name(false));
		add(g->display_name(true));
	}

	// The machine's own name, which is how its owners say it.
	groups->try_emplace(core::cache("c64"sv), &file_group::commodore);

	s_config.groups_by_name.store(std::shared_ptr<const file_group_by_name>(std::move(groups)));
}

static constexpr file_traits photo_traits = file_traits::bitmap | file_traits::cache_metadata | file_traits::zoom |
                                            file_traits::hide_overlays | file_traits::thumbnail |
                                            file_traits::photo_metadata;
static constexpr file_traits video_traits = file_traits::av | file_traits::preview_video | file_traits::cache_metadata |
                                            file_traits::hide_overlays | file_traits::thumbnail |
                                            file_traits::video_metadata;
static constexpr file_traits audio_traits = file_traits::av | file_traits::visualize_audio |
                                            file_traits::cache_metadata | file_traits::music_metadata;

// A Commodore image's own name and the files it lists are indexed, so a search finds a game on a disk.
static constexpr file_traits commodore_traits = file_traits::commodore | file_traits::cache_metadata |
                                                file_traits::no_metadata_grouping;
static constexpr file_traits archive_traits = file_traits::archive | file_traits::no_metadata_grouping;

file_group file_group::other("other", "others", 0x5E5E5E, icon_index::document, file_traits::no_metadata_grouping,
                             group_key_type::other, {});
file_group file_group::folder("folder", "folders", 0x18A59C, icon_index::folder, file_traits::no_metadata_grouping,
                              group_key_type::folder, {});
file_group file_group::photo("photo", "photos", 0x18A549, icon_index::photo, photo_traits, group_key_type::photo,
                             {"xmp"});
// Subtitles travel with the video they belong to, as its sidecars: text (srt, vtt, ass, ssa, smi, mpl2) and
// images (sub with its idx, sup); so does the .nfo a media manager or a release keeps about it.
file_group file_group::video("video", "videos", 0xA55018, icon_index::video, video_traits, group_key_type::video,
                             {"srt", "smi", "vtt", "ass", "ssa", "sub", "idx", "sup", "mpl2", "thm", "xmp", "nfo"});

bool is_subtitle_extension(std::string_view ext)
{
	if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);

	static const core::hash_set<std::string_view, core::ihash, core::ieq> extensions{"srt", "vtt", "ass", "ssa", "smi",
	                                                                                 "sub", "idx", "sup", "mpl2"};

	return extensions.contains(ext);
}

bool is_subtitle_suffix(const std::string_view word)
{
	static const core::hash_set<std::string_view, core::ihash, core::ieq> flags{"forced", "sdh", "cc"};
	return flags.contains(word) || is_language_word(word);
}

core::file_path moved_sidecar_path(const core::file_path primary, const core::file_path destination,
                                   const core::file_path sidecar)
{
	const auto stem = primary.file_name_without_extension();
	const auto name = sidecar.name().sv();
	// A sidecar named for another file - the RAW an XMP describes - keeps only its extension, as it always has.
	const auto after_stem = name.size() > stem.size() && name[stem.size()] == '.' &&
	                        core::icmp(name.substr(0, stem.size()), stem) == 0;
	const auto tail = after_stem ? name.substr(stem.size()) : sidecar.extension();
	return destination.folder().combine_file(std::string(destination.file_name_without_extension()).append(tail));
}
file_group file_group::audio("audio", {}, 0xA5184B, icon_index::audio, audio_traits, group_key_type::audio, {});

file_group file_group::archive("archive", "archives", 0x5588DD, icon_index::archive, archive_traits,
                               group_key_type::archive, {});
file_group file_group::commodore("commodore", {}, 0xFF8811, icon_index::retro, commodore_traits, group_key_type::retro,
                                 {});

file_type file_type::folder(file_group::folder, {}, {}, {});
file_type file_type::other(file_group::other, {}, {}, {});

static void build_file_types()
{
	s_config.groups = {
		file_group::folder,  file_group::photo,     file_group::video, file_group::audio,
		file_group::archive, file_group::commodore, file_group::other,
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
		{file_group::video, "asf", "Windows Media",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		{file_group::video, "ast", "Audio Stream", {}},
		{file_group::video, "avi", "Audio Visual Interleave",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		{file_group::video, "avr", {}, {}},
		{file_group::video, "avs", {}, {}},
		{file_group::video, "avs2", {}, {}},
		{file_group::video, "bcstm", {}, {}},
		{file_group::video, "bfstm", {}, {}},
		still_file_type(libfiles::image_format::bmp),
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
		{file_group::photo, "dng", "Adobe Digital Negative",
	     file_traits::raw | file_traits::edit | file_traits::embedded_xmp | file_traits::in_place_metadata},
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
		{file_group::video, "flv", "Flash video",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		{file_group::video, "fsb", {}, {}},
		{file_group::video, "fwse", {}, {}},
		{file_group::video, "g722", {}, {}},
		{file_group::video, "g723_1", {}, {}},
		{file_group::video, "g728", {}, {}},
		{file_group::video, "g729", {}, {}},
		{file_group::audio, "gdm", {}, {}},
		{file_group::video, "genh", {}, {}},
		still_file_type(libfiles::image_format::gif),
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
		still_file_type(libfiles::image_format::jpeg),
		// Its metadata goes to a sidecar: nothing Diffractor uses writes a JPEG 2000 packet, and its own
		// scan would not read one back.
		{file_group::photo, "jpx", "Jpeg 2000", file_traits::edit},
		still_file_type(libfiles::image_format::jxl),
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
		{file_group::audio, "mp3", "MPEG Layer 3",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::thumbnail | file_traits::in_place_metadata},
		{file_group::video, "mov", {}, file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		{file_group::video, "mp4,mp4a,mp4v,m4v,m4b,f4v,3g2,3gp2,3gp,3gpp,crm", "MPEG-4",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		{file_group::video, "crm", "Canon Cinema RAW Light",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		{file_group::audio, "m4a,mp4a,m4r", "MPEG-4 Audio",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::thumbnail | file_traits::in_place_metadata},
		{file_group::audio, "m4p", "MPEG-4 (DRM)",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::thumbnail | file_traits::in_place_metadata},
		{file_group::audio, "mpc", "Musepack", {}},
		{file_group::video, "mpd", {}, {}},
		{file_group::video, "mpeg,mpg,mpe,m1v,m2v,mp2,mpv,m2p,m2t,vob,ms-pvr,dvr-ms", "MPEG", file_traits::edit},
		{file_group::audio, "mpa,m2a", "MPEG",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		still_file_type(libfiles::image_format::heif),
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
		still_file_type(libfiles::image_format::png),
		{file_group::photo, "ppm", "Portable Pixmap", {}},
		still_file_type(libfiles::image_format::psd),
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
		{file_group::audio, "sid", "Commodore 64 SID tune", file_traits::sid_tune},
		{file_group::video, "son", {}, {}},
		{file_group::video, "sox", {}, {}},
		{file_group::video, "spdif", {}, {}},
		{file_group::video, "sph", {}, {}},
		{file_group::audio, "spx", "Speex low bitrate audio", {}},
		{file_group::photo, "srw", "Samsung raw", file_traits::raw | file_traits::edit},
		{file_group::video, "ss2", {}, {}},
		{file_group::audio, "st26", {}, {}},
		{file_group::audio, "stk", {}, {}},
		{file_group::video, "stl", {}, {}},
		{file_group::audio, "stm", {}, {}},
		{file_group::audio, "stp", {}, {}},
		{file_group::video, "str", {}, {}},
		{file_group::video, "ag", {}, {}},
		{file_group::video, "swf", "Macromedia Flash", file_traits::embedded_xmp},
		{file_group::video, "tak", "Tom's Lossless Audio Kompressor", {}},
		{file_group::photo, "tga,targa", "Truevision TGA (Targa) image", {}},
		{file_group::video, "tco", {}, {}},
		{file_group::video, "thd", {}, {}},
		still_file_type(libfiles::image_format::tiff),
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
		{file_group::audio, "wav", "Microsoft Wave",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		{file_group::photo, "wbm", {}, {}},
		// WBMP has no metadata container, so its metadata goes to a sidecar.
		{file_group::photo, "wbmp", {}, file_traits::edit},
		{file_group::video, "web", {}, {}},
		{file_group::video, "webm", "WebM video", {}},
		still_file_type(libfiles::image_format::webp),
		{file_group::audio, "wma", "Windows Media Audio 9",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
		{file_group::video, "wmv,wm", "Windows Media video",
	     file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata},
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

		// A disk image is drawn as a floppy and a cartridge as one, rather than as the group's controller.
		if (ft.has_trait(file_traits::disk_image)) ft.icon = icon_index::floppy;
		else if (ft.group == file_group::commodore && ft.extension == "crt") ft.icon = icon_index::cartridge;

		if (ft.icon == icon_index::document)
		{
			ft.icon = ft.group->icon;
		}

		if (!ft.extension.empty())
		{
			core::split2(ft.extension, true, [pft = &ft](const std::string_view ext)
			{ s_config.types_by_name.insert_or_assign(core::cache(ext), pft); });
		}
	}

	av_initialise();
}

// Once a process. The tables are handed out by reference for as long as it runs, so building them again would leave
// every reference to the first build dangling - and the tools a console run loads them for also run in a session that
// already has.
void load_file_types()
{
	static std::once_flag loaded;
	std::call_once(loaded, build_file_types);
}

static std::vector<file_map_link> s_map_links;

namespace
{
	// Tools sit a few levels below the configured roots (vendor\product\app.exe). Capping the walk
	// keeps startup bounded on machines with large Program Files trees and stops a directory
	// junction from recursing forever.
	constexpr int max_tool_scan_depth = 4;

	using exe_path_by_name = core::hash_map<std::string_view, core::file_path, core::ihash, core::ieq>;

	// Only the executables named in the config are retained, so memory stays proportional to the
	// config and not to the number of programs installed.
	void find_executables(const core::folder_path folder, const int depth,
	                      const core::hash_set<std::string_view, core::ihash, core::ieq>& wanted,
	                      exe_path_by_name& found)
	{
		if (found.size() >= wanted.size()) return;

		const auto contents = platform::iterate_file_items(folder, true);

		for (const auto& f : contents.files)
		{
			const auto path = folder.combine_file(f.name);

			if (core::icmp(path.extension(), ".exe") == 0)
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
		return core::starts(url, "https://") || core::starts(url, "http://");
	}

	std::vector<file_map_link> built_in_map_links()
	{
		return {
			{core::cache("Google Maps"), core::cache("https://www.google.com/maps/place/{latitude},{longitude}")},
			{core::cache("Google Earth"), core::cache("https://earth.google.com/web/search/{latitude},{longitude}")},
			{core::cache("Bing Maps"),
		     core::cache("https://www.bing.com/maps?cp={latitude}~{longitude}&lvl=17&style=h")},
			{core::cache("OpenStreetMap"),
		     core::cache(
				 "https://www.openstreetmap.org/?mlat={latitude}&mlon={longitude}#map=17/{latitude}/{longitude}")},
			{core::cache("Apple Maps"),
		     core::cache("https://maps.apple.com/?ll={latitude},{longitude}&q=Photo%20Location")},
		};
	}
}

file_tools_result scan_tools()
{
	file_tools_result result;
	std::vector<core::folder_path> tool_paths;
	std::vector<file_tool_ptr> tools;

	try
	{
		const auto json = platform::blob_from_file(platform::probe_data_file("diffractor-tools.json"));

		if (!json.empty())
		{
			core::json::json_doc document;
			document.Parse(std::bit_cast<const char*>(json.data()), json.size());

			if (document.HasParseError())
			{
				core::log(__FUNCTION__, std::format("diffractor-tools.json is not valid json (offset {})",
				                                    document.GetErrorOffset()));
			}
			else if (document.IsObject())
			{
				const auto tools_member = document.FindMember("tools");

				if (tools_member != document.MemberEnd() && tools_member->value.IsObject())
				{
					for (const auto& m : tools_member->value.GetObject())
					{
						if (core::icmp(m.name.GetString(), "folders") == 0)
						{
							if (m.value.IsArray())
							{
								for (const auto& folder : m.value.GetArray())
								{
									if (folder.IsString())
									{
										tool_paths.emplace_back(core::folder_path(folder.GetString()));
									}
								}
							}
						}

						if (core::icmp(m.name.GetString(), "apps") == 0)
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

											if (core::icmp(a.name.GetString(), "exe") == 0)
												tool->exe = core::cache(a.value.GetString());
											if (core::icmp(a.name.GetString(), "invoke") == 0)
												tool->invoke_text = core::cache(a.value.GetString());
											if (core::icmp(a.name.GetString(), "text") == 0)
												tool->text = core::cache(a.value.GetString());
											if (core::icmp(a.name.GetString(), "extensions") == 0)
												tool->extensions = core::cache(a.value.GetString());
											if (core::icmp(a.name.GetString(), "group") == 0)
												tool->group = core::cache(a.value.GetString());
										}

										if (!tool->exe.is_empty() && !tool->invoke_text.is_empty())
										{
											tools.emplace_back(std::move(tool));
										}
									}
								}
							}
						}

						if (core::icmp(m.name.GetString(), "maps") == 0)
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

											if (core::icmp(a.name.GetString(), "text") == 0)
												link.text = core::cache(a.value.GetString());
											if (core::icmp(a.name.GetString(), "url") == 0)
												link.url = core::cache(a.value.GetString());
										}

										if (!core::is_empty(link.text) && is_web_url(link.url.sv()))
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
		core::log(__FUNCTION__, e.what());
	}

	core::hash_set<std::string_view, core::ihash, core::ieq> wanted;

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

		core::split2(tool->extensions, true, [&result, &tool](const std::string_view ext)
		{ result.by_extension[core::cache(ext)].emplace_back(tool); });

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
			core::split2(ft.extension, true, [&ft, &result](const std::string_view ext)
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
	core::mutex file_op_mutex;
	_Guarded_by_(file_op_mutex) core::hash_map<file_type_ref, file_op_row> file_op_stats;
}

std::function<void(core::file_path destination)> test_before_file_update_publish;

void record_file_op(const file_type_ref ft, const file_op_stat stat)
{
	if (!ft) return;

	core::exclusive_lock lock(file_op_mutex);
	file_op_stats[ft][static_cast<size_t>(stat)] += 1;
}

void log_file_op_summary()
{
	std::vector<std::pair<file_type_ref, file_op_row>> rows;

	{
		core::exclusive_lock lock(file_op_mutex);
		rows.assign(file_op_stats.begin(), file_op_stats.end());
	}

	if (rows.empty()) return;

	const auto row_total = [](const file_op_row& r)
	{
		uint64_t total = 0;
		for (const auto n : r) total += n;
		return total;
	};

	std::ranges::sort(rows,
	                  [&row_total](const auto& a, const auto& b) { return row_total(a.second) > row_total(b.second); });

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
		report += std::format("| {} | {} | {} | {} | {} | {} | {} |\n", group_name(ft->extension), capability(ft),
		                      row[0], row[1], row[2], row[3], row[4]);
	}

	report += std::format("| **Total** | - | {} | {} | {} | {} | {} |\n", totals[0], totals[1], totals[2], totals[3],
	                      totals[4]);
	core::log("perf file types", report);
}

namespace
{
	class loaded_clipboard_image final : public platform::clipboard_image
	{
		file_load_result _loaded;

	public:
		explicit loaded_clipboard_image(file_load_result loaded) : _loaded(std::move(loaded)) {}

		sizei dimensions() const override { return _loaded.dimensions(); }

		void write_pixels(const std::span<uint8_t> pixels) const override
		{
			const auto s = _loaded.to_surface();

			if (!is_valid(s))
			{
				// A picture that could not be decoded is not a picture to offer: an empty frame on the
				// clipboard would report the copy as having worked.
				core::log(__FUNCTION__, "clipboard image could not be decoded");
				throw std::invalid_argument("Invalid clipboard image");
			}

			const auto dimensions = _loaded.dimensions();
			const auto stride_out = dimensions.cx * 4_z;
			const auto stride_in = s->stride();
			const auto copy_len = std::min(stride_out, stride_in);
			const auto* const pixels_in = s->pixels();

			if (pixels_in == nullptr)
			{
				core::log(__FUNCTION__, "Invalid pixel buffer pointers");
				throw std::invalid_argument("Invalid pixel buffers");
			}

			for (auto y = 0; y < dimensions.cy; ++y)
			{
				const auto src_offset = stride_in * y;
				const auto dest_offset = stride_out * y;

				// Bounds checking for buffer operations
				if (src_offset + copy_len > s->size() || dest_offset + copy_len > pixels.size())
				{
					core::log(__FUNCTION__, "Buffer bounds exceeded");
					throw std::invalid_argument("Invalid pixel buffer size");
				}

				memcpy(pixels.data() + dest_offset, pixels_in + src_offset, copy_len);
			}
		}

		bool offers_png() const override { return true; }

		// The rows the bitmap formats give, as a PNG: the sRGB rendition Diffractor shows, with the alpha a target
		// taking a bitmap may drop, and no orientation of its own, as the bitmap has none.
		std::vector<uint8_t> png() const override
		{
			const auto s = _loaded.to_surface();
			const auto view = is_valid(s) ? libfiles_view(*s) : std::nullopt;

			if (!view)
			{
				core::log(__FUNCTION__, "clipboard image could not be decoded");
				throw std::invalid_argument("Invalid clipboard image");
			}

			auto encoded = libfiles::encode_png(*view, 1, {});
			for (const auto& note : encoded.notes) core::log_once("libpng", note);
			return std::move(encoded.data);
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

core::const_surface_ptr file_load_result::to_surface(const sizei scale_hint, const bool can_use_yuv,
                                                     const core::cancel_token& token, const decode_intent intent) const
{
	if (success)
	{
		files ff;

		if (is_valid(s))
		{
			auto fitted = ff.fit_within(s, scale_hint);
			if (intent == decode_intent::edit || !is_valid(fitted)) return fitted;
			// Sixteen bits a channel are for an edit to work on; anything else takes a byte, an HDR picture's signal as
			// its SDR rendition, and colours a profile describes as their sRGB rendition.
			if (signal.hdr && core::is_wide(fitted->format())) return sdr_rendition(fitted, *signal.hdr, false);
			if (!signal.profile.empty()) return profile_rendition(fitted, signal.profile, false);
			if (!core::is_wide(fitted->format())) return fitted;
			return core::to_8_bit(fitted);
		}

		if (is_valid(i))
		{
			return ff.image_to_surface(i, scale_hint, can_use_yuv, token, intent);
		}
	}

	return {};
}

core::pixel_difference_result file_load_result::calc_pixel_difference(const file_load_result& other) const
{
	constexpr sizei scale_hint = {1024 * 4, 1024 * 4};
	const auto s1 = to_surface(scale_hint);
	const auto s2 = other.to_surface(scale_hint);

	if (s1 && s2)
	{
		return s1->pixel_difference(s2);
	}

	return core::pixel_difference_result::unknown;
}

file_type_ref files::file_type_from_name(const core::file_path path)
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

		return core::icmp(ext, "ts") == 0 || core::icmp(ext, "m2t") == 0 || core::icmp(ext, "m2ts") == 0 ||
		       core::icmp(ext, "mts") == 0;
	}

	// A transport stream is a run of fixed-size packets, each opening with the 0x47 sync byte: 188
	// bytes broadcast, 192 with the M2TS/AVCHD arrival timestamp, 204 with Reed-Solomon parity. The
	// run is found rather than assumed to start at zero, because a partial capture or a PVR dump can
	// begin mid-packet or behind a prefix. Requiring four aligned packet starts is what separates a
	// stream from a TypeScript file that happens to contain 'G'.
	bool is_mpeg_transport_stream(const core::cspan header)
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

bool files::media_header_matches(const std::string_view extension, const core::cspan header)
{
	if (is_transport_stream_extension(extension))
	{
		return is_mpeg_transport_stream(header);
	}

	return true;
}

file_type_ref files::file_type_from_name(const std::string_view name)
{
	if (!core::is_empty(name))
	{
		auto ext = name.substr(core::find_ext(name));

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
		result[index] = {(0.5 + normalized_x / denominator) * size.cx, (0.5 + normalized_y / denominator) * size.cy};
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
				if (y < (std::min)(start.Y, end.Y) || y > (std::max)(start.Y, end.Y) || core::equiv(start.Y, end.Y))
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
	return !_crop.has_point(bounds[0]) || !_crop.has_point(bounds[1]) || !_crop.has_point(bounds[2]) ||
	       !_crop.has_point(bounds[3]);
}

bool image_edits::is_no_loss(const sizei image_extent) const
{
	return !has_crop(image_extent) && !has_scale() && !has_perspective() && !has_color_changes();
}

double image_edits::rotation_angle() const
{
	return _crop.angle();
}

bool files::can_save(const core::file_path path)
{
	const auto ext = path.extension();
	return can_save_extension(ext);
}

bool files::can_save_extension(const std::string_view ext)
{
	static const core::hash_set<std::string_view, core::ihash, core::ieq> save_extensions = {
		{".jpg"}, {".jpeg"}, {".jpe"}, {".png"}, {".webp"}, {".jxl"}, {".avif"},
		{"jpg"},  {"jpeg"},  {"jpe"},  {"png"},  {"webp"},  {"jxl"},  {"avif"},
	};

	return save_extensions.contains(ext);
}

bool files::can_turn(const core::file_path path)
{
	if (can_save(path) && file_type_from_name(path)->can_edit_photo()) return true;
	const auto format = libfiles::info(libfiles::file_type_from_extension(path.extension())).update_format;
	return format == libfiles::file_format::heif || format == libfiles::file_format::jxl;
}

bool files::is_raw(const core::file_path path)
{
	const auto* const mt = file_type_from_name(path);
	return (mt->traits & file_traits::raw) != file_traits::none;
}

bool files::is_raw(const std::string_view name)
{
	const auto* const mt = file_type_from_name(name);
	return (mt->traits & file_traits::raw) != file_traits::none;
}

bool files::is_jpeg(const core::file_path path)
{
	const auto ext = path.extension();
	return core::icmp(ext, ".jpg") == 0 || core::icmp(ext, ".jpeg") == 0 || core::icmp(ext, ".jpe") == 0;
}

bool files::is_jpeg(const std::string_view name)
{
	const auto ext = name.substr(core::find_ext(name));
	return core::icmp(ext, ".jpg") == 0 || core::icmp(ext, ".jpeg") == 0 || core::icmp(ext, ".jpe") == 0;
}

files::files()
{
}

files::~files()
{
	_scaler.reset();
}

core::const_image_ptr files::surface_to_image(const core::const_surface_ptr& surface_in,
                                              const libfiles::metadata_blocks& metadata,
                                              const file_encode_params& params, const core::image_format format)
{
	core::const_image_ptr result;

	// save_png and the jpeg encoder both walk the surface as four-byte pixels, so a planar one would
	// be written out as its luma plane read as BGRX - grey and tiled rather than a visible failure.
	if (is_valid(surface_in) && core::is_packed(surface_in->format()))
	{
		const auto has_alpha = surface_in->format() == core::texture_format::ARGB;

		if (format == core::image_format::PNG || (format == core::image_format::Unknown && has_alpha))
		{
			result = save_png(surface_in, metadata);
		}
		else if (format == core::image_format::WEBP)
		{
			result = save_webp(surface_in, metadata, params);
		}
		else if (format == core::image_format::JXL)
		{
			result = save_jxl(surface_in, metadata, params);
		}
		else if (format == core::image_format::AVIF)
		{
			result = save_avif(surface_in, metadata, params);
		}
		else
		{
			result = save_jpeg(surface_in, metadata, params, &_jpeg_encoder);
		}
	}

	return result;
}

// Thumbnails are a rebuildable cache, so the format is chosen for bytes rather than fidelity. A
// lossy WebP is materially smaller than the JPEG or PNG it replaces at the same measured quality,
// and libwebp drops the alpha plane itself for a surface that turns out to be opaque.
core::const_image_ptr files::surface_to_thumbnail(const core::const_surface_ptr& surface_in)
{
	if (!is_valid(surface_in)) return {};

	file_encode_params params;
	params.jpeg_save_quality = thumbnail_quality;
	params.webp_quality = thumbnail_webp_quality;
	params.webp_lossy_alpha = true;
	params.webp_fast = true;

	core::const_image_ptr result = save_webp(surface_in, {}, params);

	// The fallbacks below encode packed pixels only, so a planar surface the WebP encoder would not
	// take - a video frame carrying a rec709 matrix, say - is converted rather than dropped.
	if (!is_valid(result) && !core::is_packed(surface_in->format()))
	{
		const auto packed = std::make_shared<core::surface>();

		if (scaler().convert_yuv_surface(*surface_in, packed) && core::is_valid(packed))
		{
			result = save_webp(packed, {}, params);
			if (!is_valid(result)) result = surface_to_image(packed, {}, params, core::image_format::Unknown);
		}

		return result;
	}

	// save_webp accepts packed pixels and the NV12 planes VP8 encodes natively, so anything else
	// falls back to what the thumbnail store held before WebP: PNG when the surface carries alpha,
	// JPEG otherwise.
	if (!is_valid(result)) result = surface_to_image(surface_in, {}, params, core::image_format::Unknown);

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
template <typename Ptr> Ptr files::fit_within_impl(Ptr surface_in, const sizei ceiling)
{
	if (!is_valid(surface_in) || ceiling.is_empty()) return surface_in;

	const auto dimensions_out = [&]
	{
		auto fitted = core::scale_dimensions(surface_in->dimensions(), ceiling, true);

		// NV12 carries one chroma pair per 2x2 luma block, so a fitted extent has to be even for the
		// planar reduction to take it. Rounding down keeps it inside the ceiling.
		if (surface_in->format() == core::texture_format::NV12)
		{
			fitted = {std::max(2, fitted.cx & ~1), std::max(2, fitted.cy & ~1)};
		}

		return fitted;
	}();

	if (dimensions_out == surface_in->dimensions()) return surface_in;

	core::surface_ptr reduced;

	if (core::area_downscale(surface_in, reduced, dimensions_out)) return reduced;

	// What the area filter will not take - P010, or a planar surface whose fitted extent lands odd -
	// still has to come back reduced, and swscale converts it to packed on the way.
	core::surface_ptr converted;

	if (scaler().scale_surface(surface_in, converted, dimensions_out) && is_valid(converted))
	{
		return converted;
	}

	// A resampler that failed is not a reason to lose pixels already in hand.
	return surface_in;
}

core::surface_ptr files::fit_within(core::surface_ptr surface_in, const sizei ceiling)
{
	return fit_within_impl(std::move(surface_in), ceiling);
}

core::const_surface_ptr files::fit_within(core::const_surface_ptr surface_in, const sizei ceiling)
{
	return fit_within_impl(std::move(surface_in), ceiling);
}

core::pixel_difference_result files::pixel_difference(const core::const_image_ptr& expected,
                                                      const core::const_image_ptr& actual)
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

	return core::pixel_difference_result::unknown;
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
		const std::optional<core::orientation> _orientation_override;
		const core::cancel_token& _token;

	public:
		core::surface_ptr surface;

		jpeg_surface_sink(const std::optional<core::orientation> orientation_override,
		                  const core::cancel_token& token) : _orientation_override(orientation_override), _token(token)
		{
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			const auto scale = static_cast<int64_t>(frame.scale_denominator);
			const auto scaled_output_bytes = static_cast<int64_t>(frame.source_width) * frame.source_height * 4 /
			                                 (scale * scale);
			const auto decode_bytes = scaled_output_bytes + static_cast<int64_t>(frame.working_bytes);

			if (decode_bytes > render::max_decode_bytes)
			{
				core::log("files::decode_jpeg",
				          std::format("decode of {} x {} needs {}, over the {} budget", frame.source_width,
				                      frame.source_height, platform::format_size(core::file_size(decode_bytes)),
				                      platform::format_size(core::file_size(render::max_decode_bytes))));
				return {};
			}

			// The decoder reads the EXIF orientation by libfiles' one rule, the scan's and parse's.
			const auto orientation = _orientation_override ? *_orientation_override
			                                               : static_cast<core::orientation>(frame.orientation);
			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};
			auto s = std::make_shared<core::surface>();

			if (frame.format == libfiles::pixel_format::nv12)
			{
				// JPEG/JFIF is full-range BT.601, which the shader applies through rec601_full.
				if (!s->alloc(dimensions, core::texture_format::NV12, orientation)) return {};
				s->color_space(color_space::rec601_full);
			}
			else if (!s->alloc(dimensions, core::texture_format::RGB, orientation))
			{
				return {};
			}

			surface = std::move(s);
			return {surface->pixels(), surface->stride()};
		}

		bool cancelled() override { return _token.is_cancelled(); }
	};
}

// The JPEG decode both image_to_surface overloads share. Returns an unscaled surface; an NV12 one is
// flagged through is_yuv, because the GPU sampler resizes it at draw time. Without an
// orientation_override, the orientation is the one the file's EXIF gives.
core::surface_ptr files::decode_jpeg(const core::cspan data, const sizei target_extent, const bool can_use_yuv,
                                     const std::optional<core::orientation> orientation_override, bool& is_yuv,
                                     const core::cancel_token& token, const decode_intent intent)
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
		options.smooth_chroma = intent != decode_intent::thumbnail;
		// Shown in sRGB's colours where a profile describes others; an edit works in the file's own.
		options.map_to_srgb = intent != decode_intent::edit;

		const auto result = _decoder.decode(r, sink, options);

		if (!result.success)
		{
			if (!result.error.empty()) core::log(__FUNCTION__, result.error);
			return {};
		}

		is_yuv = result.frame.format == libfiles::pixel_format::nv12;
		return std::move(sink.surface);
	}
	catch (const std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	return {};
}

core::surface_ptr files_test_hooks::decode_jpeg(files& ff, const core::cspan data, const sizei target_extent,
                                                const bool can_use_yuv, const decode_intent intent, bool& is_yuv)
{
	return ff.decode_jpeg(data, target_extent, can_use_yuv, {}, is_yuv, {}, intent);
}

int64_t files::estimate_decode_bytes(const sizei source_dimensions, const int bytes_per_pixel)
{
	return static_cast<int64_t>(source_dimensions.cx) * source_dimensions.cy * bytes_per_pixel;
}

int64_t files::estimate_decode_bytes(const core::const_image_ptr& image, const sizei scale_hint)
{
	if (!is_valid(image)) return 0;

	const auto dims = image->dimensions();
	auto pixels = static_cast<int64_t>(dims.cx) * dims.cy;

	if (image->format() == core::image_format::JPEG)
	{
		// Mirrors what image_to_surface hands libjpeg, so the estimate tracks the real allocation.
		const auto factor = core::calc_scale_down_factor(dims, scale_hint);
		pixels /= static_cast<int64_t>(factor) * factor;
	}

	return pixels * 4;
}

bool files::exceeds_decode_budget(const sizei source_dimensions, const int bytes_per_pixel)
{
	return estimate_decode_bytes(source_dimensions, bytes_per_pixel) > render::max_decode_bytes;
}

bool files::exceeds_decode_budget(const core::const_image_ptr& image, const sizei scale_hint)
{
	return estimate_decode_bytes(image, scale_hint) > render::max_decode_bytes;
}

bool reject_over_budget_source(load_diagnostic* const diagnostic, const sizei source_dimensions,
                               const std::string_view format, const int bytes_per_pixel)
{
	if (diagnostic) diagnostic->source_dimensions = source_dimensions;

	const auto bytes = files::estimate_decode_bytes(source_dimensions, bytes_per_pixel);

	if (bytes <= render::max_decode_bytes) return false;

	if (diagnostic) diagnostic->over_budget = true;

	core::log(__FUNCTION__,
	          std::format("{} {} x {} needs {} to decode, over the {} budget", format, source_dimensions.cx,
	                      source_dimensions.cy, platform::format_size(core::file_size(bytes)),
	                      platform::format_size(core::file_size(render::max_decode_bytes))));
	return true;
}

core::surface_ptr files::image_to_surface(const core::const_image_ptr& image, const sizei target_extent,
                                          const bool can_use_yuv, const core::cancel_token& token,
                                          const decode_intent intent)
{
	core::surface_ptr surface_result;

	// Nothing downstream can produce a smaller frame than the codec does, so refusing here is what
	// keeps a huge image from exhausting memory on whichever worker asked for it.
	if (exceeds_decode_budget(image, target_extent))
	{
		core::log(__FUNCTION__,
		          std::format("decode of {} x {} needs {}, over the {} budget", image->dimensions().cx,
		                      image->dimensions().cy,
		                      platform::format_size(core::file_size(estimate_decode_bytes(image, target_extent))),
		                      platform::format_size(core::file_size(render::max_decode_bytes))));
		return surface_result;
	}

	core::bump(df::file_perf.decodes);
	core::bump(df::file_perf.decode_bytes, is_valid(image) ? image->data().size() : 0u);
	core::perf_timer timer(df::file_perf.decode_us, &df::file_perf.decode_max_us);

	try
	{
		if (is_valid(image))
		{
			const auto format = image->format();

			if (format == core::image_format::JPEG)
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
			else if (format == core::image_format::PNG)
			{
				try
				{
					// Sixteen bits a channel cost twice the bytes, so an edit takes them only where the
					// budget holds that.
					const auto wide = intent == decode_intent::edit &&
					                  estimate_decode_bytes(image, target_extent) * 2 <= render::max_decode_bytes;
					auto loaded = load_png(image->data(), wide, nullptr, intent != decode_intent::edit);

					if (is_valid(loaded))
					{
						surface_result = fit_within(std::move(loaded), target_extent);
					}
				}
				catch (std::exception& e)
				{
					core::log(__FUNCTION__, e.what());
				}
			}
			else if (format == core::image_format::WEBP)
			{
				try
				{
					auto loaded = load_webp(image->data(), can_use_yuv, target_extent, intent != decode_intent::edit);

					if (is_valid(loaded))
					{
						surface_result = fit_within(std::move(loaded), target_extent);
					}
				}
				catch (std::exception& e)
				{
					core::log(__FUNCTION__, e.what());
				}
			}
			else if (format == core::image_format::JXL)
			{
				mem_read_stream stream(image->data());
				auto loaded = load_jxl(stream);

				if (is_valid(loaded))
				{
					surface_result = fit_within(std::move(loaded), target_extent);
				}
			}
			else if (format == core::image_format::AVIF)
			{
				mem_read_stream stream(image->data());
				auto loaded = load_heif(stream, nullptr, nullptr, intent != decode_intent::edit);

				if (is_valid(loaded))
				{
					surface_result = fit_within(std::move(loaded), target_extent);
				}
			}
		}
	}
	catch (std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	return surface_result;
}

core::surface_ptr files::image_to_surface(const core::cspan image_buffer_in, const sizei target_extent,
                                          const bool can_use_yuv, const decode_intent intent)
{
	core::surface_ptr surface_result;

	try
	{
		if (!image_buffer_in.empty())
		{
			const auto* const still = find_still_format(libfiles::detect_image_format(image_buffer_in));

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
		core::log(__FUNCTION__, e.what());
	}

	return surface_result;
}

// Reduces a decoded picture to the grayscale square the hash consumes. Separated from hashing so
// the four orientations share one decode and one reduction.
static bool build_phash_gray(const core::const_surface_ptr& surface, std::array<uint8_t, crypto::phash_pixels>& gray);

uint64_t files::calc_perceptual_hash(const core::const_surface_ptr& surface)
{
	std::array<uint8_t, crypto::phash_pixels> gray{};
	if (!build_phash_gray(surface, gray)) return 0;
	return crypto::perceptual_hash(gray.data(), gray.size());
}

crypto::phash_rotations files::calc_perceptual_hash_rotations(const core::const_surface_ptr& surface)
{
	std::array<uint8_t, crypto::phash_pixels> gray{};
	if (!build_phash_gray(surface, gray)) return {};
	return crypto::perceptual_hash_rotations(gray.data(), gray.size());
}

static bool build_phash_gray(const core::const_surface_ptr& surface, std::array<uint8_t, crypto::phash_pixels>& gray)
{
	if (!is_valid(surface))
	{
		return false;
	}

	// RGB and ARGB are both four bytes a pixel; the planar YUV formats are not laid out this way and
	// are never what a still decodes to here.
	const auto format = surface->format();

	if (format != core::texture_format::RGB && format != core::texture_format::ARGB)
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
				const auto* const line = std::bit_cast<const core::color32*>(surface->pixels_line(y));

				for (auto x = x0; x < x1 && x < width; ++x)
				{
					const auto c = line[x];
					// Rec.601 luma in integer form; the hash needs a consistent gray, not colorimetry.
					sum += (core::get_r(c) * 77 + core::get_g(c) * 151 + core::get_b(c) * 28) >> 8;
					count += 1;
				}
			}

			gray[cell_y * extent + cell_x] = count == 0 ? 0 : static_cast<uint8_t>(std::min(sum / count, 255u));
		}
	}

	return true;
}

crypto::phash_rotations files::calc_perceptual_hash_rotations(const core::cspan encoded)
{
	constexpr auto decode_extent = 128;
	return calc_perceptual_hash_rotations(
		image_to_surface(encoded, {decode_extent, decode_extent}, false, decode_intent::thumbnail));
}

uint64_t files::calc_perceptual_hash(const core::cspan encoded)
{
	// Decoded well above the hash extent so the reduction has real pixels to average, and still far
	// enough below native size that a JPEG scales in the DCT domain rather than decoding in full.
	constexpr auto decode_extent = 128;
	return calc_perceptual_hash(
		image_to_surface(encoded, {decode_extent, decode_extent}, false, decode_intent::thumbnail));
}

static uint64_t round_up_to_multiple(const uint64_t n, const uint64_t multiple)
{
	if (multiple == 0) return n;

	const auto remainder = n % multiple;

	if (remainder == 0) return n;

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
		core::log(__FUNCTION__, message);
		throw app_exception(message);
	}

	if (_h->read(buffer, len) != len)
	{
		const auto message = std::format("invalid read_through read: {} {}", len, _h->path());
		core::log(__FUNCTION__, message);
		throw app_exception(message);
	}
}

void file_read_stream::load_buffer(const uint64_t pos, const size_t len)
{
	// Overflow-safe: pos and len both originate from untrusted file fields.
	if (pos > _file_size || len > _file_size - pos)
	{
		const auto message = std::format("invalid read past end of file: {}", _h->path());
		core::log(__FUNCTION__, message);
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
			core::log(__FUNCTION__, message);
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
				core::log(__FUNCTION__, message);
				throw std::bad_alloc();
			}

			_buffer = new_buffer;
			_buffer_size = new_buffer_size;
		}

		if (_h->seek(new_start_pos, platform::file::whence::begin) != new_start_pos)
		{
			const auto message = std::format("invalid load_buffer seek: {} {}", new_start_pos, _h->path());
			core::log(__FUNCTION__, message);
			throw app_exception(message);
		}

		if (_h->read(_buffer, new_buffer_size) != new_buffer_size)
		{
			const auto message = std::format("invalid load_buffer read: {} {}", new_buffer_size, _h->path());
			core::log(__FUNCTION__, message);
			throw app_exception(message);
		}

		_loaded_start_pos = new_start_pos;
		_loaded_end_pos = new_end_pos;
	}
}

bool file_read_stream::open(const core::file_path path)
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
		_block_size = core::sixty_four_k; // platform::calc_optimal_read_size(path);
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

core::blob file_read_stream::read_all()
{
	// Parenthesised: the Windows max macro is in scope here.
	if (_file_size > (std::numeric_limits<size_t>::max)()) throw app_exception("file too large to load"s);

	// The size is the file's own claim, and the caller is about to hold all of it at once. Without
	// the ceiling every other whole-file read observes, a scan of an arbitrarily large file
	// allocates to match it. Answered empty rather than thrown: one file that cannot be held must
	// not abandon the batch that was scanning it, and every caller already treats an empty view as
	// a file it could not read.
	if (_file_size > core::max_blob_size)
	{
		core::log(__FUNCTION__, std::format("{} is too large to read whole ({})", _h->path(),
		                                    platform::format_size(core::file_size(_file_size))));
		return {};
	}

	const auto len = static_cast<size_t>(_file_size);
	core::blob result(len);

	if (len != 0)
	{
		// Bypasses the sliding window on purpose: load_buffer would allocate the whole file and
		// then copy it into result. load_buffer always seeks before it reads, so moving the file
		// pointer here cannot disturb the window.
		if (_h->seek(0, platform::file::whence::begin) != 0)
		{
			const auto message = std::format("invalid read_all seek: {}", _h->path());
			core::log(__FUNCTION__, message);
			throw app_exception(message);
		}

		if (_h->read(result.data(), len) != len)
		{
			const auto message = std::format("invalid read_all read: {} {}", len, _h->path());
			core::log(__FUNCTION__, message);
			throw app_exception(message);
		}
	}

	return result;
}

bool files::save(const core::file_path path, const file_load_result& loaded)
{
	const auto save_format = extension_to_format(path.extension());
	core::const_image_ptr saved;

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
		encode_params.jxl_quality = setting.convert.jxl_quality;
		encode_params.jxl_lossless = setting.convert.jxl_lossless;
		encode_params.avif_quality = setting.convert.avif_quality;
		encode_params.avif_lossless = setting.convert.avif_lossless;
		saved = surface_to_image(loaded.to_surface(), {}, encode_params, save_format);
	}

	return is_valid(saved) && platform::blob_save_to_file(saved->data(), path);
}

// The most folder art read from one file before it is passed over: art kept beside music is bounded as the art
// embedded in it is.
constexpr uint64_t max_folder_art_bytes = 16ull * 1024ull * 1024ull;

static core::image_ptr read_folder_art_file(const core::file_path path)
{
	if (!platform::exists(path)) return nullptr;

	const auto f = platform::open_file(path, platform::file_open_mode::read);
	if (!f) return nullptr;

	const auto len = f->size();
	if (len < 16 || len > max_folder_art_bytes) return nullptr;

	core::blob data(static_cast<size_t>(len));
	if (f->read(data.data(), len) != len) return nullptr;

	auto image = load_image_file(data);
	return is_valid(image) ? image : nullptr;
}

// What a folder keeps beside its music or its film, for a file with no art of its own: an album's Folder.jpg,
// cover.jpg or front.jpg; a video's poster named for it (Movie-poster.jpg); and a film's folder art (poster.jpg)
// when the folder holds that film alone, named for it. Elsewhere - home videos beside Explorer's folder.jpg, a
// folder of many films, a season of episodes - folder art would make every video look the same. The folder's own
// art is remembered briefly, so an album read track by track reads it once.
static core::image_ptr load_folder_art(const core::file_path path, const file_type_ref ft)
{
	static core::mutex cache_mutex;
	static core::folder_path cached_folder;
	static bool cached_is_audio = false;
	static core::image_ptr cached_art;
	static int64_t cached_ms = 0;
	constexpr int64_t cache_ms = 60 * 1000;

	const auto folder = path.folder();
	const auto stem = path.file_name_without_extension();
	const auto is_audio = ft->has_trait(file_traits::visualize_audio);

	if (!is_audio)
	{
		for (const auto suffix : {"-poster.jpg", "-poster.png", "-thumb.jpg", "-thumb.png"})
		{
			if (auto art = read_folder_art_file(folder.combine_file(std::string(stem) + suffix))) return art;
		}

		if (!is_named_for_its_folder(stem, folder.name())) return nullptr;
	}

	const auto now = core::now_ms();

	{
		core::exclusive_lock lock(cache_mutex);
		if (cached_folder == folder && cached_is_audio == is_audio && now - cached_ms < cache_ms) return cached_art;
	}

	static constexpr std::string_view album_names[] = {
		"Folder.jpg", "folder.jpg", "Folder.png", "cover.jpg",    "Cover.jpg",
		"cover.png",  "front.jpg",  "Front.jpg",  "AlbumArt.jpg",
	};
	static constexpr std::string_view film_names[] = {
		"poster.jpg", "Poster.jpg", "poster.png", "folder.jpg", "Folder.jpg", "cover.jpg", "movie.jpg",
	};

	core::image_ptr art;

	for (const auto name :
	     is_audio ? std::span<const std::string_view>(album_names) : std::span<const std::string_view>(film_names))
	{
		art = read_folder_art_file(folder.combine_file(name));
		if (art) break;
	}

	{
		core::exclusive_lock lock(cache_mutex);
		cached_folder = folder;
		cached_is_audio = is_audio;
		cached_art = art;
		cached_ms = now;
	}

	return art;
}

file_scan_result files::scan_file(const core::file_path path, const bool load_thumb, const file_type_ref ft,
                                  const std::string_view xmp_sidecar, const sizei max_thumb_size,
                                  const libfiles::scan_intent intent, const bool want_image)
{
	// RAW goes through LibRaw, which opens by path itself, so a handle opened here would be a second
	// open per file that nothing reads.
	auto f = ft->has_trait(file_traits::raw) ? platform::file_ptr{} : open_file(path, platform::file_open_mode::read);

	return scan_file(f, path, load_thumb, ft, xmp_sidecar, max_thumb_size, intent, want_image);
}

// Overload that scans an ALREADY-OPEN file. Used after replace_file hands back the still-open,
// cache-coherent handle it renamed through, so an edited file is re-scanned via the same handle
// instead of a fresh (possibly stale over SMB) by-name open. Note: the RAW branch (scan_raw) and
// the XMP sidecar are still read BY PATH - the handle only covers the primary media stream, and is
// null for RAW.
file_scan_result files::scan_file(const platform::file_ptr& f, const core::file_path path, const bool load_thumb,
                                  const file_type_ref ft, const std::string_view xmp_sidecar,
                                  const sizei max_thumb_size, const libfiles::scan_intent intent, const bool want_image)
{
	file_scan_result result;
	core::bump(df::file_perf.scans);
	record_file_op(ft, file_op_stat::read);
	core::perf_timer timer(df::file_perf.scan_us, &df::file_perf.scan_max_us);

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
			const auto is_small_file = file_len < core::two_fifty_six_k;
			const auto load_from_mem = load_thumb && is_bitmap;
			// blob sizes are size_t; a longer file would be read past the end of a truncated buffer.
			const auto fits_in_memory = file_len == static_cast<size_t>(file_len);

			core::blob data;

			const auto can_load_whole_file = fits_in_memory && file_len <= core::max_blob_size;

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
					result = scan_still_bytes(std::move(data), intent, load_thumb, want_image, max_thumb_size);
				}
				else
				{
					file_read_stream stream;

					if (stream.open(f))
					{
						if (load_from_mem && file_len > core::max_blob_size)
						{
							core::log(__FUNCTION__, std::format("{} is too large to thumbnail ({})", path.name(),
							                                    platform::format_size(core::file_size(file_len))));
							result = scan_photo(stream, intent, false, this);
						}
						else
						{
							result = scan_photo(stream, intent, load_thumb, this);
						}
					}
				}
			}
			else if (ft->has_trait(file_traits::sid_tune))
			{
				// A tune is a C64 program, not a container FFmpeg or libfiles can read: its header is all
				// there is to describe it, and the whole file is a few kilobytes.
				result = scan_sid(data);
			}
			else if (ft->has_trait(file_traits::commodore))
			{
				// Listed from as much of the image as the items view lists it from, which holds the largest
				// disk whole. Read apart from `data`, which is checksummed below only when it is the file.
				if (!data.empty())
				{
					result = scan_c64_media(data);
				}
				else
				{
					core::blob head;
					head.resize(static_cast<size_t>(std::min<uint64_t>(file_len, core::one_meg)));
					if (f->read(head.data(), head.size()) == head.size()) result = scan_c64_media(head);
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
					                             : (intent == libfiles::scan_intent::index ? media_intent::metadata
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

							core::surface_ptr thumbnail_surface;

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
				if (result.metadata.xmp.empty() && !core::is_empty(xmp_sidecar))
				{
					const auto sidecar = platform::blob_from_file(path.folder().combine_file(xmp_sidecar));
					result.metadata.xmp.assign(sidecar.begin(), sidecar.end());
				}

				// What a media manager's .nfo says of a video, when the user asks for those to be read.
				if (setting.read_nfo && ft->has_trait(file_traits::video_metadata))
				{
					auto nfo = read_video_nfo(path);
					if (!nfo.is_empty()) result.nfo = std::make_shared<const nfo_details>(std::move(nfo));
				}

				// Music and films with no art of their own take the art kept beside them. Looked for only
				// when a thumbnail is wanted: indexing does not need it.
				if (load_thumb && !result.cover_art)
				{
					if (auto art = load_folder_art(path, ft)) result.cover_art = std::move(art);
				}
			}

			// A still's bytes went to scan_still_bytes, which checksums them; a small file of any other kind
			// is checksummed here.
			if (!is_bitmap && !data.empty())
			{
				result.crc32c = crc32c(data.data(), data.size());
			}
		}
	}
	catch (std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	return result;
}

core::blob files::read_thumbnail_source(const core::file_path path, const file_type_ref ft)
{
	core::blob data;

	// The stills scan_file reads whole for a thumbnail. RAW goes through LibRaw, which opens by path.
	if (!ft->has_trait(file_traits::bitmap) || ft->has_trait(file_traits::raw)) return data;

	record_file_op(ft, file_op_stat::read);

	try
	{
		const auto f = platform::open_file(path, platform::file_open_mode::read);
		if (!f) return data;

		const auto file_len = f->size();

		// As scan_file: blob sizes are size_t, and a still past the blob ceiling is read in pieces, for its
		// metadata alone. An empty file is left to scan_file's stream, which reports it.
		if (file_len == 0 || file_len != static_cast<size_t>(file_len) || file_len > core::max_blob_size) return data;

		data.resize(static_cast<size_t>(file_len));
		if (f->read(data.data(), file_len) != file_len) data.clear();
	}
	catch (const std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
		data.clear();
	}

	return data;
}

file_scan_result files::scan_thumbnail_source(core::blob data, const sizei max_thumb_size)
{
	core::bump(df::file_perf.scans);
	core::perf_timer timer(df::file_perf.scan_us, &df::file_perf.scan_max_us);

	try
	{
		return scan_still_bytes(std::move(data), libfiles::scan_intent::index, true, false, max_thumb_size);
	}
	catch (const std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	return {};
}

file_scan_result files::scan_still_bytes(core::blob data, const libfiles::scan_intent intent, const bool load_thumb,
                                         const bool want_image, const sizei max_thumb_size)
{
	mem_read_stream stream(data);
	const auto detected = data.size() >= sizeof(pack128) ? libfiles::detect_image_format(stream.peek128(0))
	                                                     : libfiles::image_format::unknown;
	const auto keep_encoded = (load_thumb || want_image) && is_image_format(detected);

	// A still kept encoded is its own thumbnail, so one embedded in it would be read only to be replaced.
	auto result = scan_photo(stream, intent, load_thumb && !keep_encoded, this);
	result.crc32c = crc32c(data.data(), data.size());

	if (keep_encoded)
	{
		// The bytes read become the image, rather than a copy of them parsed a second time: the scan above
		// has already said what they are.
		const auto* const still = find_still_format(detected);
		const auto format = still ? still->encoded : core::image_format::Unknown;
		result.thumbnail_image = std::make_shared<core::image>(std::move(data), result.dimensions(), format,
		                                                       result.orientation);
		// The same object: these bytes are the whole file, so a display that wants the written image never
		// has to read it back.
		result.full_image = result.thumbnail_image;
	}
	else if (load_thumb && !is_valid(result.thumbnail_surface))
	{
		// A thumbnail the container holds - a HEIF thumbnail item - is decoded by the scan and is the one
		// apply_scan_result keeps, so the whole picture is decoded only for a still without one.
		// Planar is allowed here: a JPEG or WebP thumbnail reaches the encoder as the planes VP8 wants,
		// skipping the colour conversion in both directions.
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

	return result;
}

core::image_ptr load_image_file(core::cspan file)
{
	core::image_ptr result;
	mem_read_stream stream(file);

	if (stream.size() > 16)
	{
		const auto detected = libfiles::detect_image_format(stream.peek128(0));

		if (is_image_format(detected))
		{
			// want_thumbnail stays false here, and must. scan_exif calls this on an embedded
			// thumbnail, so asking for one again would let a file of nested thumbnails recurse -
			// and scan_jpg carries a 64K buffer per frame, so that ends in a blown stack.
			const auto info = scan_photo(stream);
			const auto* const still = find_still_format(info.format);
			const auto format = still ? still->encoded : core::image_format::Unknown;

			result = std::make_shared<core::image>(file, info.dimensions(), format, info.orientation);
		}
		else if (detected != libfiles::image_format::unknown)
		{
			files ff;
			const auto surface = ff.image_to_surface(file, {});
			if (is_valid(surface)) result = save_png(surface, {});
		}
	}

	return result;
}

file_load_result files::load(const core::file_path path, const bool can_load_preview, const decode_intent intent)
{
	df::last_loaded_path.store(path, std::memory_order_relaxed);
	core::bump(df::file_perf.loads);
	core::perf_timer timer(df::file_perf.load_us, &df::file_perf.load_max_us);

	file_load_result result;
	const auto* const mt = file_type_from_name(path);
	record_file_op(mt, file_op_stat::read);

	// Every caller waits on the answer - the display and the editor each hold a load in flight until it
	// arrives - so a decoder's throw, a PSD cut short or memory a decode could not have, is answered as
	// a file that cannot be read rather than escaping to the queue, which would only log it.
	try
	{
		if (mt->has_trait(file_traits::bitmap))
		{
			if (mt->has_trait(file_traits::raw))
			{
				result = load_raw(path, can_load_preview, intent == decode_intent::edit);
			}
			else
			{
				// Larger than this can hold at once. Reading a prefix would decode a frame whose tail is
				// whatever the codec fills missing rows with, and the edit path would re-encode that over
				// the original. Named as a property of the file, so it is not retried.
				const auto attributes = platform::file_attributes(path);

				if (attributes.size > static_cast<uint64_t>(core::max_blob_size))
				{
					core::log(__FUNCTION__, std::format("{} is too large to load ({})", path.name(),
					                                    platform::format_size(core::file_size(attributes.size))));
					result.reason = file_load_result::failure::too_large;
					return result;
				}

				const auto file = platform::blob_from_file(path);

				if (!file.empty())
				{
					mem_read_stream stream(file);

					if (stream.size() > 16)
					{
						const auto detected = libfiles::detect_image_format(stream.peek128(0));
						const auto* const still = find_still_format(detected);

						if (is_image_format(detected))
						{
							result.i = load_image_file(file);
							result.success = is_valid(result.i);

							if (result.success && intent == decode_intent::edit)
							{
								if (detected == libfiles::image_format::png)
								{
									// Decoded now, at the precision and in the range the save will take it at, so
									// what the edit shows and the signal it reports are what the save works on. One
									// that cannot be is left to the decode the edit's preview makes, which says why.
									try
									{
										result.s = load_png(file, true, &result.signal);
									}
									catch (const std::exception& e)
									{
										core::log(__FUNCTION__, e.what());
									}

									if (!is_valid(result.s)) result.signal = {};
								}
								else if (detected == libfiles::image_format::jpeg)
								{
									result.signal.gain_map = !libfiles::find_jpeg_gain_map({file.data(), file.size()})
									                              .empty();
								}
							}
						}
						else
						{
							// These codecs build the whole frame before anything can be scaled down, so
							// each refuses an oversized source at the point it reads the header and says
							// so here rather than looking like a corrupt file.
							load_diagnostic diagnostic;
							auto* const edit = intent == decode_intent::edit ? &result.signal : nullptr;

							if (still && still->load)
							{
								result.s = still->load(stream, &diagnostic, edit);

								if (edit && detected == libfiles::image_format::heif && is_valid(result.s))
								{
									result.signal.gain_map = load_heif_gain_map(stream, true).has_value();
								}
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
								const sizei scanned_dimensions{static_cast<int>(scanned.width),
								                               static_cast<int>(scanned.height)};

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
								result.reason = diagnostic.over_budget ? file_load_result::failure::too_large
								                                       : file_load_result::failure::unreadable;
							}
						}

						// An edit is shown and adjusted in sRGB's colours, and carried back into those a profile
						// describes. An HDR picture's colours are its signal's, or were mapped to SDR.
						if (result.success && intent == decode_intent::edit && !result.signal.hdr &&
						    !result.signal.tone_mapped)
						{
							libfiles::reader profile_reader(std::span<const uint8_t>(file.data(), file.size()));
							auto icc = libfiles::scan(profile_reader).metadata.icc;
							if (libfiles::icc_maps_to_srgb(icc)) result.signal.profile = std::move(icc);
						}
					}
				}
			}
		}
		else if (mt->has_trait(file_traits::av))
		{
			// Not supported
		}
	}
	catch (const std::exception& e)
	{
		core::log(__FUNCTION__, std::format("{}: {}", path.name(), e.what()));
		result = {};
		result.reason = file_load_result::failure::unreadable;
	}

	return result;
}

static simple_transform angle_to_transform(const int a)
{
	if (a == -90 || a == 270)
	{
		return core::simple_transform::rot_90;
	}

	if (a == 90 || a == -270)
	{
		return core::simple_transform::rot_270;
	}

	if (a == 180 || a == -180)
	{
		return core::simple_transform::rot_180;
	}

	return core::simple_transform::none;
}

core::image_format extension_to_format(const std::string_view ext)
{
	static const core::hash_map<std::string_view, core::image_format, core::ihash, core::ieq> extensions = {
		{".jpg", core::image_format::JPEG},  {".jpeg", core::image_format::JPEG}, {".jpe", core::image_format::JPEG},
		{".png", core::image_format::PNG},   {".webp", core::image_format::WEBP}, {".jxl", core::image_format::JXL},
		{".avif", core::image_format::AVIF}, {"jpg", core::image_format::JPEG},   {"jpeg", core::image_format::JPEG},
		{"jpe", core::image_format::JPEG},   {"png", core::image_format::PNG},    {"webp", core::image_format::WEBP},
		{"jxl", core::image_format::JXL},    {"avif", core::image_format::AVIF},
	};

	const auto found = extensions.find(ext);

	if (found != extensions.end())
	{
		return found->second;
	}

	return core::image_format::Unknown;
}

core::image_ptr save_surface(const core::image_format& format, const core::const_surface_ptr& surface,
                             const libfiles::metadata_blocks& metadata, const file_encode_params& params)
{
	if (format == core::image_format::JPEG)
	{
		return save_jpeg(surface, metadata, params);
	}
	if (format == core::image_format::PNG)
	{
		return save_png(surface, metadata, params.hdr);
	}
	if (format == core::image_format::WEBP)
	{
		return save_webp(surface, metadata, params);
	}
	if (format == core::image_format::JXL)
	{
		return save_jxl(surface, metadata, params);
	}
	if (format == core::image_format::AVIF)
	{
		return save_avif(surface, metadata, params);
	}

	return {};
}

// A re-encode copies the source's Exif into the new file. An orientation reset otherwise reaches only
// the XMP, which PNG and WebP do not mirror into the Exif they write, so the copied tag would turn the
// upright pixels a second time. The pixel dimensions move with them.
static libfiles::metadata_blocks upright_save_metadata(const libfiles::metadata_blocks& source, const sizei dimensions)
{
	libfiles::metadata_blocks result;
	result.xmp = source.xmp;
	result.iptc = source.iptc;
	result.icc = source.icc;
	result.photoshop = source.photoshop;

	core::blob exif;
	if (!is_exif_signature(source.exif)) exif.assign(exif_signature.begin(), exif_signature.end());
	exif.insert(exif.end(), source.exif.begin(), source.exif.end());

	const auto fixed = metadata_exif::fix_dims({exif.data(), exif.size()}, dimensions.cx, dimensions.cy);
	result.exif.assign(fixed.begin(), fixed.end());
	return result;
}

// Which way the pixels a re-encode wrote stand, for a JPEG XL or an AVIF, whose readers take it from the codestream
// or the image's rotation and mirror rather than the EXIF. An edited surface does not say reliably - a resize draws
// onto a fresh canvas - so it is the orientation the file ends with: the edit's, else the EXIF's carried over, which
// is where the other formats keep it, else the surface's, which they write when there is no EXIF.
static core::orientation written_orientation(const metadata_edits& edits, const libfiles::metadata_blocks& metadata,
                                             const core::const_surface_ptr& surface)
{
	if (edits.orientation.has_value() && *edits.orientation != core::orientation::none) return *edits.orientation;

	if (!metadata.exif.empty())
	{
		const auto skip = is_exif_signature(metadata.exif) ? exif_signature.size() : 0u;
		const auto summary = libfiles::summarize_exif({metadata.exif.data() + skip, metadata.exif.size() - skip});
		return summary.orientation && *summary.orientation >= 1 && *summary.orientation <= 8
		           ? static_cast<core::orientation>(*summary.orientation)
		           : core::orientation::top_left;
	}

	return surface->orientation();
}

// An edit that only turns the whole picture by quarter turns, unmirrored: no crop, scale, perspective or colour.
static bool is_quarter_turn(const image_edits& edits, const sizei dimensions)
{
	if (!edits.is_no_loss(dimensions) || !edits.has_crop_bounds()) return false;

	return std::ranges::any_of(std::array{simple_transform::rot_90, simple_transform::rot_180,
	                                      simple_transform::rot_270},
	                           [&](const simple_transform t)
	{ return edits.crop_bounds() == quadd(dimensions).transform(t); });
}

bool files::saves_in_place(const core::file_path path, const image_edits& edits, const sizei dimensions)
{
	return can_save(path) || (can_turn(path) && is_quarter_turn(edits, dimensions));
}

// Whether libfiles writes a JPEG XL's orientation as asked. It refuses one whose codestream it cannot rewrite, and one
// made from a JPEG whose EXIF cannot take the turn, which would leave that JPEG unturned.
static bool jxl_takes_orientation(const core::file_path path, const core::orientation orientation)
{
	auto f = platform::open_file(path, platform::file_open_mode::read);
	if (!f) return false;

	platform_file_stream stream(std::move(f));
	libfiles::reader r(stream);
	libfiles::metadata_update turn;
	turn.legacy.orientation = static_cast<int>(orientation);
	return libfiles::plan_update(r, turn).ok();
}

bool files::turns_in_place(const core::file_path path, const image_edits& edits, const sizei dimensions)
{
	if (!is_quarter_turn(edits, dimensions)) return false;

	// As update_impl writes the turn.
	const auto format = libfiles::info(libfiles::file_type_from_extension(path.extension())).update_format;
	const auto orientation = static_cast<core::orientation>(
		static_cast<int>(angle_to_transform(core::round(edits.rotation_angle()))) + 1);
	return format == libfiles::file_format::heif ||
	       (format == libfiles::file_format::jxl && jxl_takes_orientation(path, orientation));
}

// A JPEG repacked as a JPEG XL without loss, holding the packet the JPEG XL is to hold. The packet is written into
// the JPEG first, where its segment has room for it, so the JPEG the JPEG XL gives back holds it too. Empty when
// libjxl cannot keep the JPEG - CMYK, arithmetic-coded or of 12 bits - which is then encoded from its pixels.
static core::blob repack_as_jxl(const core::cspan jpeg, const core::file_path path_src, const core::file_path path_dst,
                                const metadata_edits& edits, const std::string_view src_xmp_name, const bool in_sidecar)
{
	const auto path_stage = platform::temp_file(".jpg", path_dst.folder());
	const core::scope_exit discard_stage([path_stage] { platform::delete_file(path_stage); });

	if (!platform::blob_save_to_file(jpeg, path_stage)) return {};

	metadata_xmp::update(path_stage, path_src, edits, src_xmp_name, path_stage.extension(".xmp"), in_sidecar, path_dst);
	const auto staged = platform::blob_from_file(path_stage);
	auto repacked = libfiles::transcode_jpeg_to_jxl({staged.data(), staged.size()});
	for (const auto& note : repacked.notes) core::log(__FUNCTION__, note);
	return core::blob(std::move(repacked.data));
}

// A HEIF's gain map restated as its photo stands. libheif turns each item by its own rotation and mirror, and Apple's
// gain map has none of its own beside a primary that has: it arrives tagged with the turn the primary was given.
static core::const_surface_ptr standing_as(const core::const_surface_ptr& map, const core::orientation photo)
{
	const auto own = map->orientation() == core::orientation::none ? core::orientation::top_left : map->orientation();
	const auto target = photo == core::orientation::none ? core::orientation::top_left : photo;
	if (own == target) return map;

	core::surface_ptr turned;
	if (const auto upright = core::to_simple_transform(own); upright != simple_transform::none)
		turned = map->transform(upright);

	if (const auto into = core::to_simple_transform_inv(target); into != simple_transform::none)
	{
		const auto from = turned ? core::const_surface_ptr(turned) : map;
		turned = from->transform(into);
	}

	if (!turned) return {};
	turned->orientation(target);
	return turned;
}

// The gain map a photo holds - a JPEG's after its primary, a HEIF's ISO 21496-1 or Apple one - edited as the photo is,
// for a re-encoding that keeps one. Nothing when it holds none, or the one it holds cannot be read or edited. A JPEG's
// stands as its primary is stored, whatever its own EXIF says.
static std::optional<gain_map_surface> source_gain_map(const file_load_result& loaded, const file_scan_result& scan,
                                                       const core::file_path path, const image_edits& edits,
                                                       const core::const_surface_ptr& photo)
{
	std::optional<gain_map_surface> found;

	if (core::is_jpeg(loaded.i))
	{
		const auto& data = loaded.i->data();
		found = load_jpeg_gain_map({data.data(), data.size()});
	}
	else if (scan.format == libfiles::image_format::heif && loaded.signal.gain_map)
	{
		const auto file = platform::blob_from_file(path);
		mem_read_stream stream(file);
		found = load_heif_gain_map(stream);
		if (found) found->pixels = standing_as(found->pixels, photo->orientation());
	}

	return found && is_valid(found->pixels) ? edited_gain_map(*found, edits, photo->dimensions()) : std::nullopt;
}

// A gain map made for an HDR picture whose SDR rendition a JPEG holds, at half the picture's resolution, so the JPEG
// still shows the picture's HDR where a display can: Ultra HDR's.
static std::optional<gain_map_surface> gain_map_for(const core::const_surface_ptr& signal,
                                                    const libfiles::hdr_description& hdr,
                                                    const core::const_surface_ptr& rendition)
{
	if (!is_valid(signal) || !is_valid(rendition) || !core::is_wide(signal->format()) ||
	    !core::is_packed(rendition->format()) || signal->dimensions() != rendition->dimensions())
	{
		return {};
	}

	const libfiles::image_view signal_view{signal->pixels(), signal->stride(), signal->width(), signal->height(),
	                                       core::has_alpha(signal->format()) ? libfiles::pixel_format::rgba16
	                                                                         : libfiles::pixel_format::rgbx16};
	const libfiles::image_view base_view{rendition->pixels(), rendition->stride(), rendition->width(),
	                                     rendition->height(),
	                                     core::has_alpha(rendition->format()) ? libfiles::pixel_format::bgra8
	                                                                          : libfiles::pixel_format::bgrx8};
	const auto made = libfiles::make_gain_map(signal_view, hdr, base_view, 2);
	if (made.pixels.empty()) return {};

	auto map = std::make_shared<core::surface>();
	if (!map->alloc(static_cast<int>(made.width), static_cast<int>(made.height), core::texture_format::RGB)) return {};

	const auto row = std::size_t{4} * made.width;
	for (std::uint32_t y = 0; y < made.height; ++y)
		std::memcpy(map->pixels_line(static_cast<int>(y)), made.pixels.data() + row * y, row);

	return gain_map_surface{std::move(map), made.metadata};
}

platform::file_op_result files::update_impl(const core::file_path path_src, const core::file_path path_dst,
                                            const metadata_edits& metadata_edits, const image_edits& photo_edits,
                                            const file_encode_params& params, const bool create_original,
                                            const std::string_view src_xmp_name, const std::string_view dst_xmp_name,
                                            const file_update_publish& publish, bool* const live_file_maybe_changed,
                                            const av_edits* const av, const core::cancel_token* const cancel,
                                            file_update_result* const av_report)
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
	const core::scope_exit record_write_failure([&result, &op_type]
	{
		if (result.failed() && op_type) record_file_op(op_type, file_op_stat::write_failed);
	});

	// The sidecar the user actually keeps. Nothing writes to it until the media swap below has
	// succeeded; the update stages its own copy beside whichever file is being written.
	const auto path_dst_xmp = dst_xmp_name.empty() ? path_dst.extension(".xmp")
	                                               : path_dst.folder().combine_file(dst_xmp_name);

	try
	{
		bool has_photo_edits = false;

		const auto* const mt = file_type_from_name(path_src);
		const auto extension_change = core::icmp(path_dst.extension(), path_src.extension()) != 0;
		const auto path_change = path_dst != path_src;
		const auto reencode = params.reencode && path_change;

		file_scan_result scan_result;

		// The scan only ever sizes a crop and carries the source metadata into a re-encode, so it is
		// worth its read - a full parse plus the embedded thumbnail - only where the pixels may be
		// rewritten. A metadata-only write never rewrites them.
		if (mt->has_trait(file_traits::bitmap) && (extension_change || reencode || !photo_edits.is_empty()))
		{
			file_read_stream stream;

			if (stream.open(path_src))
			{
				scan_result = scan_photo(stream);
				has_photo_edits = photo_edits.has_changes(scan_result.dimensions()) || extension_change || reencode;
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

		// A decode gives the first frame of an animation, so a picture written back over it would leave the
		// others out. A copy written elsewhere is a picture of that frame, which is what it was asked for.
		if (has_photo_edits && !path_change && scan_result.animated)
		{
			result.code = platform::file_op_result_code::FAILED;
			result.error_message = "it is an animation, and its picture written back would keep only the first frame";
			return result;
		}

		if (!path_change && !has_photo_edits && !metadata_edits.has_changes() && !av)
		{
			return result;
		}

		// A HEIF or a JPEG XL turned by quarter turns keeps its picture: the turn is an orientation, written where the
		// file keeps one - its images' rotations and mirrors, its codestream's header - as a JPEG's EXIF keeps one. A
		// JPEG XL that cannot take one is turned as a picture instead.
		if (has_photo_edits && !path_change && !av && !reencode &&
		    is_quarter_turn(photo_edits, scan_result.dimensions()))
		{
			auto turned = metadata_edits;
			turned.orientation = static_cast<core::orientation>(
				static_cast<int>(angle_to_transform(core::round(photo_edits.rotation_angle()))) + 1);

			if (scan_result.format == libfiles::image_format::heif ||
			    (scan_result.format == libfiles::image_format::jxl &&
			     jxl_takes_orientation(path_src, *turned.orientation)))
			{
				const auto written = update_impl(path_src, path_dst, turned, image_edits{}, params, create_original,
				                                 src_xmp_name, dst_xmp_name, publish, live_file_maybe_changed, av,
				                                 cancel, av_report);
				if (written.success() || scan_result.format == libfiles::image_format::heif) return written;
			}
		}

		// Where the file's metadata lives: in its own packet, or in a sidecar - the one a raw needs, or
		// the one a DNG has beside it.
		const auto in_sidecar = metadata_xmp::sidecar_holds_metadata(path_src, src_xmp_name);

		// A metadata-only edit that fits the space the LIVE file holds is patched where it lies:
		// nothing the file holds moves, and libfiles orders the writes so that an interruption leaves
		// a file that reads whole, each field old or new. An edit that does not fit is
		// staged below. A save-as or a pixel edit has to produce a second file, and a backup needs the
		// prior bytes, so none of them is a patch.
		const auto may_patch = !path_change && !has_photo_edits && !create_original && !in_sidecar && !av &&
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

				if (after_attempt.modified != before_attempt.modified || after_attempt.size != before_attempt.size)
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

		// A file whose metadata lives in its sidecar has only the sidecar written, so the media file's
		// bytes never change. Staging and swapping it would copy the whole file - a 60 MB raw for a
		// one-star rating - only to write back what was already there. A backup still stages,
		// because the prior bytes have to be kept before anything is replaced.
		const auto sidecar_only = !path_change && !has_photo_edits && !create_original && in_sidecar && !av;

		if (sidecar_only)
		{
			op_type = mt;
			record_file_op(mt, file_op_stat::sidecar);

			const auto path_temp_xmp = path_temp.extension(".xmp");
			auto committed = false;

			// Nothing outside this scope knows the staged path, so a throw would strand it.
			const core::scope_exit discard_stage([path_temp_xmp, &committed]
			{
				if (!committed) platform::delete_file(path_temp_xmp);
			});

			// Staged like every other write: the live sidecar is replaced only once the new one is
			// complete on disk. Told where the metadata lives, so a sidecar removed since it was asked
			// cannot send the edit into the live file.
			const auto staged = metadata_xmp::update(path_dst, path_src, metadata_edits, src_xmp_name, path_temp_xmp,
			                                         in_sidecar);
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

		// A JPEG XL copied unchanged to another path keeps its boxes as they are. Writing the same packet
		// would only uncompress it, or be refused in one whose JPEG it keeps has no room for the packet as
		// it is written now.
		const auto copied_as_is = !has_photo_edits && !metadata_edits.has_changes() && !av &&
		                          extension_to_format(path_dst.extension()) == core::image_format::JXL;

		if (av)
		{
			// The source's own packets, in its own format: nothing decoded or encoded. Claimed first, as a
			// copy that fails part way leaves what it wrote.
			temp_file_created = true;
			op_type = mt;

			const core::cancel_token never;
			const auto copied = av_remux(path_src, path_temp, *av, cancel ? *cancel : never);

			if (av_report)
			{
				av_report->repair_skipped = copied.repairs.skipped;
				av_report->repair_ended_at = copied.repairs.ended_at;
			}

			if (copied.status == av_remux_status::cancelled)
			{
				result.code = platform::file_op_result_code::CANCELLED;
			}
			else if (copied.status != av_remux_status::done)
			{
				result.code = platform::file_op_result_code::FAILED;
				result.error_message = copied.error;
			}
			else if (copied.repairs.any() && !path_change && !create_original)
			{
				// What a repaired copy left out may be the only part another tool could still recover, so the file
				// is replaced by it only where a backup keeps the file.
				result.code = platform::file_op_result_code::FAILED;
				result.error_message = "the repaired copy leaves part of the file out, and no backup would keep it";
				if (av_report) av_report->repair_needs_backup = true;
			}
		}
		else if (has_photo_edits)
		{
			const auto dst_format = extension_to_format(path_dst.extension());
			file_load_result loaded;

			// A JPEG XL written as a JPEG or a JPEG XL is read whole. One repacked from a JPEG gives that JPEG back,
			// which the edit starts from as it would from the JPEG itself: turned, or written as it is, without its
			// pixels decoded and encoded once more. Any other says how it was compressed.
			core::blob jxl_source;

			if (extension_to_format(path_src.extension()) == core::image_format::JXL &&
			    (dst_format == core::image_format::JPEG || dst_format == core::image_format::JXL))
			{
				jxl_source = platform::blob_from_file(path_src);
				const auto given_back = libfiles::reconstruct_jpeg({jxl_source.data(), jxl_source.size()});

				if (!given_back.empty())
				{
					loaded.i = load_image_file({given_back.data(), given_back.size()});
					loaded.success = core::is_jpeg(loaded.i);
				}
			}

			const auto given_back = loaded.success;

			// At the precision the source holds, so the edits are worked at it and rounded once, by the
			// encoder of the format saved.
			if (!given_back) loaded = load(path_src, false, decode_intent::edit);

			if (!loaded.success)
			{
				// Without a decoded source there is nothing to write; carrying on would
				// hand a non-existent temp file to the metadata update and the replace.
				result.code = platform::file_op_result_code::FAILED;
			}
			else if (loaded.signal.tone_mapped && !path_change)
			{
				// Too large to edit in HDR, the picture was decoded as its SDR rendition, which never replaces it. A
				// copy written elsewhere is that rendition, which is what it was asked for.
				result.code = platform::file_op_result_code::FAILED;
				result.error_message = "it is HDR, too large to edit in HDR, and written back would lose it";
			}
			else
			{
				const auto dimensions_in = loaded.dimensions();
				const auto dst_path_is_jpeg = is_jpeg(path_dst);
				const auto jpeg_to_jpeg = core::is_jpeg(loaded.i) && dst_path_is_jpeg;
				const auto no_loss = photo_edits.is_no_loss(dimensions_in);
				const auto turn = angle_to_transform(core::round(photo_edits.rotation_angle()));
				// A JPEG goes into a JPEG XL whole where the edit loses nothing: always when it was one, which keeps
				// how it was compressed as a WebP does, unless a conversion asks for its settings; otherwise when a
				// lossless JPEG XL is asked for.
				const auto repack = core::is_jpeg(loaded.i) && dst_format == core::image_format::JXL && no_loss &&
				                    (params.jxl_lossless || (given_back && !reencode));

				core::blob transformed;
				auto rotate_over_budget = false;

				// The JPEG turned as the edit asks; empty when the turn would not be lossless, which the re-encode
				// below handles.
				const auto turned_jpeg = [&]
				{
					auto turned = turn_jpeg_losslessly(_jpeg_transformer, _jpeg_encoder, loaded.i->data(), turn,
					                                   &rotate_over_budget);
					if (!turned.empty())
						turned = with_edited_gain_map(turned, loaded.i->data(), photo_edits, dimensions_in, turn);
					return turned;
				};

				if (given_back && dst_path_is_jpeg && no_loss && turn == core::simple_transform::none)
				{
					// The JPEG the JPEG XL was repacked from, as it was.
					transformed = loaded.i->data().clone();
				}
				else if (jpeg_to_jpeg && !reencode && no_loss && params.jpeg_save_quality >= 75)
				{
					transformed = turned_jpeg();
				}
				else if (repack)
				{
					const auto jpeg = turn == core::simple_transform::none ? loaded.i->data().clone() : turned_jpeg();

					if (!jpeg.empty())
					{
						transformed = repack_as_jxl(jpeg, path_src, path_dst, metadata_edits, src_xmp_name, in_sidecar);
						staged_with_edit = !transformed.empty();
					}
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

					if (!platform::blob_save_to_file(transformed, path_temp))
					{
						result.code = platform::file_op_result_code::FAILED;
					}
				}
				else
				{
					const auto surface_in = loaded.to_surface({}, false, {}, decode_intent::edit);
					// An HDR picture's signal is edited as the signal it is, its colour as its SDR rendition shows it;
					// colours a profile describes, as their sRGB rendition shows them, the profile kept.
					const auto* const hdr = loaded.signal.hdr ? &*loaded.signal.hdr : nullptr;
					const auto edited = is_empty(surface_in) ? core::const_surface_ptr{}
					                                         : apply_image_edits(surface_in, photo_edits, {}, hdr,
					                                                             loaded.signal.profile);
					const auto save_format = extension_to_format(path_temp.extension());
					// AVIF, JPEG XL and PNG keep an HDR picture's signal; JPEG and WebP take its SDR rendition.
					const auto keeps_hdr = hdr && (save_format == core::image_format::AVIF ||
					                               save_format == core::image_format::JXL ||
					                               save_format == core::image_format::PNG);
					const auto temp_surface = hdr && !keeps_hdr && is_valid(edited)
					                              ? core::const_surface_ptr(sdr_rendition(edited, *hdr, false))
					                              : edited;

					if (is_empty(temp_surface))
					{
						result.code = platform::file_op_result_code::FAILED;
					}
					else
					{
						auto encode_params = params;
						if (keeps_hdr) encode_params.hdr = *hdr;
						// An AVIF keeps the twelve bits a twelve-bit HEIF or AVIF had. Fewer readers decode twelve,
						// so anything else deeper than eight is written at ten.
						if (scan_result.format == libfiles::image_format::heif && scan_result.picture.bit_depth >= 12)
							encode_params.avif_bit_depth = 12;

						// The gain map an AVIF or a JPEG is written with: the photo's own - a JPEG's, a HEIF's -
						// edited as the photo is, or one made for an HDR picture a JPEG holds the SDR rendition of. A
						// JPEG written as a JPEG keeps its own as it is, below.
						if (save_format == core::image_format::AVIF && !hdr)
						{
							encode_params.gain_map = source_gain_map(loaded, scan_result, path_src, photo_edits,
							                                         surface_in);
						}
						else if (save_format == core::image_format::JPEG && !jpeg_to_jpeg)
						{
							encode_params.gain_map = hdr ? gain_map_for(edited, *hdr, temp_surface)
							                             : source_gain_map(loaded, scan_result, path_src, photo_edits,
							                                               surface_in);
						}

						// Re-encoding against the source's own tables costs one generation at its
						// fidelity rather than re-quantizing to the quality slider - unless the
						// slider is what a conversion asks for.
						if (jpeg_to_jpeg && !reencode) encode_params.jpeg_source = loaded.i->data();

						// A WebP written as a WebP again keeps how it was compressed, as a JPEG keeps its
						// tables: a lossless one stays lossless, and a lossy one is written at no less than
						// the quality it was - unless the slider is what a conversion asks for.
						if (!reencode && loaded.i && loaded.i->format() == core::image_format::WEBP &&
						    extension_to_format(path_dst.extension()) == core::image_format::WEBP)
						{
							const auto& source = loaded.i->data();
							const auto compression = libfiles::read_webp_compression({source.data(), source.size()});

							if (compression && compression->lossless) encode_params.webp_lossless = true;
							if (compression && !compression->lossless)
								encode_params.webp_quality = std::max(encode_params.webp_quality, compression->quality);
						}

						// A JPEG XL written as a JPEG XL again keeps how it was compressed, as a WebP does: a lossless
						// one stays lossless - unless the settings are what a conversion asks for. A lossy one holds
						// no quality to keep, and is written at the one asked for.
						if (!reencode && !jxl_source.empty() && dst_format == core::image_format::JXL)
						{
							const auto compression = libfiles::read_jxl_compression(
								{jxl_source.data(), jxl_source.size()});
							if (compression && compression->lossless) encode_params.jxl_lossless = true;
						}

						// An AVIF written as an AVIF again keeps how it was compressed, as a JPEG XL does: one coded as
						// RGB itself - the identity matrix a lossless AVIF is coded with - stays lossless, unless the
						// settings are what a conversion asks for. A lossy one holds no quality to keep.
						if (!reencode && dst_format == core::image_format::AVIF &&
						    extension_to_format(path_src.extension()) == core::image_format::AVIF &&
						    scan_result.picture.chroma == libfiles::chroma_format::rgb)
						{
							encode_params.avif_lossless = true;
						}

						libfiles::metadata_blocks save_meta;
						const auto& source_meta = scan_result.save_metadata(save_meta);
						libfiles::metadata_blocks upright_meta;
						// The decoder has already turned a HEIF by its item's own rotation and mirror, and the
						// EXIF orientation a phone writes beside them describes that same turn: carried over to
						// the upright copy, it would turn it a second time.
						const auto edit_orientation = metadata_edits.orientation.value_or(core::orientation::none);
						const auto reset_orientation = !source_meta.exif.empty() &&
						                               (edit_orientation == core::orientation::top_left ||
						                                (edit_orientation == core::orientation::none &&
						                                 scan_result.orientation_applied));
						if (reset_orientation)
						{
							upright_meta = upright_save_metadata(source_meta, temp_surface->dimensions());
						}

						const auto* carried_meta = reset_orientation ? &upright_meta : &source_meta;
						// An HDR picture's SDR rendition is sRGB, whatever profile the picture had.
						libfiles::metadata_blocks rendition_meta;

						if (hdr && !keeps_hdr && !carried_meta->icc.empty())
						{
							rendition_meta = carried_meta->clone();
							rendition_meta.icc.clear();
							carried_meta = &rendition_meta;
						}

						const auto& encode_meta = *carried_meta;

						// A JPEG XL or an AVIF is encoded with its packet - the edit, and what a conversion
						// carries over - rather than encoded bare and then updated: libjxl and libheif write
						// the boxes and items in the one pass, and the metadata update below is skipped.
						libfiles::metadata_blocks packet_meta;
						const auto encodes_packet = save_format == core::image_format::JXL ||
						                            save_format == core::image_format::AVIF;

						if (encodes_packet)
						{
							// Claimed first: the packet is read before the encode, and a test seam
							// can fail it having left a byte at the stage.
							temp_file_created = true;
							packet_meta.exif = encode_meta.exif;
							packet_meta.icc = encode_meta.icc;
							const auto packet = metadata_xmp::encoded_packet(path_src, path_temp, metadata_edits,
							                                                 src_xmp_name);
							packet_meta.xmp.assign(packet.begin(), packet.end());
							staged_with_edit = true;
						}

						core::image_ptr saved;

						// A JPEG XL repacked from a JPEG is written again as that JPEG would be - encoded against its
						// own tables, with its own segments, upright - and repacked, so it still gives a JPEG back:
						// unless a conversion names its settings, or a lossless JPEG XL is asked for.
						if (given_back && !reencode && !params.jxl_lossless && dst_format == core::image_format::JXL &&
						    written_orientation(metadata_edits, encode_meta, temp_surface) ==
						        core::orientation::top_left)
						{
							auto jpeg_params = encode_params;
							jpeg_params.jpeg_source = loaded.i->data();
							const auto bare = save_surface(core::image_format::JPEG, temp_surface, {}, jpeg_params);
							const auto& source = loaded.i->data();
							const auto encoded = is_empty(bare) ? std::vector<uint8_t>{}
							                                    : libfiles::carry_jpeg_metadata(source, bare->data(),
							                                                                    bare->dimensions().cx,
							                                                                    bare->dimensions().cy);

							if (!encoded.empty())
							{
								const auto jpeg = with_edited_gain_map(encoded, source, photo_edits,
								                                       surface_in->dimensions(),
								                                       simple_transform::none);
								auto repacked = repack_as_jxl(jpeg, path_src, path_dst, metadata_edits, src_xmp_name,
								                              in_sidecar);

								if (!repacked.empty())
									saved = std::make_shared<core::image>(std::move(repacked), bare->dimensions(),
									                                      core::image_format::JXL,
									                                      core::orientation::top_left);
							}
						}

						if (is_empty(saved) && encodes_packet)
						{
							const auto orientation = written_orientation(metadata_edits, encode_meta, temp_surface);
							saved = save_format == core::image_format::AVIF
							            ? save_avif(temp_surface, packet_meta, encode_params, orientation)
							            : save_jxl(temp_surface, packet_meta, encode_params, orientation);
						}
						else if (is_empty(saved))
						{
							saved = save_surface(save_format, temp_surface, encode_meta, encode_params);
						}

						// A JPEG keeps its gain map, so the photo still shows in HDR where it did.
						if (jpeg_to_jpeg && !is_empty(saved))
						{
							saved = std::make_shared<core::image>(with_edited_gain_map(saved->data(), loaded.i->data(),
							                                                           photo_edits,
							                                                           surface_in->dimensions(),
							                                                           simple_transform::none),
							                                      saved->dimensions(), core::image_format::JPEG,
							                                      saved->orientation());
						}
						else if (save_format == core::image_format::JPEG && encode_params.gain_map && !is_empty(saved))
						{
							saved = std::make_shared<core::image>(with_gain_map(saved->data(), *encode_params.gain_map),
							                                      saved->dimensions(), core::image_format::JPEG,
							                                      saved->orientation());
						}

						if (is_empty(saved))
						{
							result.code = platform::file_op_result_code::FAILED;
						}
						else
						{
							// Claimed before the write, for the same reason as the branch above.
							temp_file_created = true;

							if (!platform::blob_save_to_file(saved->data(), path_temp))
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
			const auto staged = copied_as_is
			                        ? std::nullopt
			                        : metadata_xmp::stage_update(path_temp, path_src, metadata_edits, src_xmp_name);

			if (staged)
			{
				xmp_result = *staged;
				staged_with_edit = true;
			}
			else
			{
				result = platform::copy_file(path_src, path_temp, true, false);
				// The copy of a read-only source is read-only too, and the update below has to write it.
				if (result.success()) platform::make_file_writable(path_temp);
			}
		}

		if (result.success() && !staged_with_edit)
		{
			const auto has_metadata_changes = metadata_edits.has_changes();

			if (av)
			{
				// FFmpeg's muxer writes back the tags its demuxer read, and nothing libfiles keeps: the packet,
				// and the rating and tags Windows reads beside it. The source's are written into the copy, where
				// libfiles can store them; a metadata sidecar goes with a copy made under another name, and an
				// unchanged name keeps the one it has. Nothing is written for a packet that would hold nothing.
				const auto wanted = in_sidecar ? path_change : metadata_xmp::can_store_packet(path_temp);

				if (wanted && !metadata_xmp::encoded_packet(path_src, path_temp, metadata_edits, src_xmp_name).empty())
				{
					xmp_result = metadata_xmp::update(path_temp, path_src, metadata_edits, src_xmp_name,
					                                  path_temp.extension(".xmp"), in_sidecar);
				}
			}
			else if ((has_metadata_changes || path_change || has_photo_edits) && !copied_as_is)
			{
				// Both destinations are staged; neither the live media file nor the live sidecar
				// is touched until the swap below.
				xmp_result = metadata_xmp::update(path_temp, path_src, metadata_edits, src_xmp_name,
				                                  path_temp.extension(".xmp"));
			}
		}

		// An MP4's index goes before its media last of all: a metadata update whose movie box has to grow writes it
		// whole at the end of the file.
		if (result.success() && av && (av->index_first || av_index_first(path_src)))
		{
			const auto path_moved = platform::temp_file(path_dst.extension(), path_dst.folder());
			const core::scope_exit discard_moved([path_moved] { platform::delete_file(path_moved); });
			const core::cancel_token never;
			const auto moved = av_move_index_first(path_temp, path_moved, cancel ? *cancel : never);

			if (moved == av_index_move::cancelled)
			{
				result.code = platform::file_op_result_code::CANCELLED;
			}
			else if (moved == av_index_move::failed)
			{
				result.code = platform::file_op_result_code::FAILED;
				result.error_message = "the copy's index could not be moved before its media";
			}
			else if (moved == av_index_move::moved)
			{
				// Replaced rather than moved over: a network share can hold the copy just read open a moment longer,
				// which a replace waits out.
				if (const auto swapped = platform::replace_file(path_temp, path_moved, false); swapped.failed())
				{
					result = swapped;
				}
			}
		}

		if (result.success() && temp_file_created)
		{
			const auto destination_existed = platform::exists(path_dst);
			if (!xmp_result.xmp_path.is_empty() && destination_existed)
			{
				const auto rollback_copy = platform::copy_file(path_dst, path_rollback, true, false);
				if (rollback_copy.failed()) result = rollback_copy;
				else rollback_file_created = true;
			}

			if (result.success())
			{
				if (test_before_file_update_publish) test_before_file_update_publish(path_dst);
			}

			// A save is cancelled up to the moment its file is replaced. One cancelled while the copy's tags were
			// written, after the copy itself was made, discards it and leaves the file as it was.
			if (result.success() && cancel && cancel->is_cancelled())
			{
				result.code = platform::file_op_result_code::CANCELLED;
			}

			if (result.success())
			{
				op_type = mt;
				record_file_op(mt, file_op_stat::replace);
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
							core::log(__FUNCTION__, std::format("could not restore {}; original retained as {}",
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
		core::log(__FUNCTION__, e.what());

		result.code = platform::file_op_result_code::FAILED;
		result.error_message = core::utf8_cast(e.what());
	}

	if (result.failed())
	{
		if (temp_file_created)
		{
			platform::make_file_writable(path_temp);
			platform::delete_file(path_temp);
		}
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

file_update_result files::update(const core::file_path path_src, const core::file_path path_dst,
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
	finish_update(result, live_file_maybe_changed, path_dst, dst_xmp_name, rescan);
	return result;
}

file_update_result files::update_av(const core::file_path path_src, const core::file_path path_dst,
                                    const av_edits& edits, const metadata_edits& carried, const bool create_original,
                                    const std::string_view src_xmp_name, const std::string_view dst_xmp_name,
                                    const rescan_spec& rescan, const core::cancel_token& cancel)
{
	file_update_result result;
	bool live_file_maybe_changed = false;
	static_cast<platform::file_op_result&>(result) = update_impl(path_src, path_dst, carried, {}, {}, create_original,
	                                                             src_xmp_name, dst_xmp_name, {},
	                                                             &live_file_maybe_changed, &edits, &cancel, &result);
	finish_update(result, live_file_maybe_changed, path_dst, dst_xmp_name, rescan);
	return result;
}

void files::finish_update(file_update_result& result, const bool live_file_maybe_changed,
                          const core::file_path path_dst, const std::string_view dst_xmp_name,
                          const rescan_spec& rescan)
{
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
			const auto path_xmp = dst_xmp_name.empty() ? path_dst.extension(".xmp")
			                                           : path_dst.folder().combine_file(dst_xmp_name);
			if (platform::exists(path_xmp)) xmp_sidecar = path_xmp.name();
		}

		result.coherent = handle != nullptr;
		result.scan = handle ? scan_file(handle, path_dst, rescan.load_thumbnail, rescan.file_type, xmp_sidecar,
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
}

// libarchive wants the native path in the platform's own spelling, and the two entry points differ
// in more than their character type: the wide one is what carries the \\?\ prefix a long Windows
// path needs, while a POSIX path is bytes all the way down and has no wide form to convert to.
// Overloading on platform::native_path lets the call site stay free of a conditional.
[[maybe_unused]] static int archive_read_open_native(archive* a, const std::wstring& path, const size_t block_size)
{
	return archive_read_open_filename_w(a, path.c_str(), block_size);
}

[[maybe_unused]] static int archive_read_open_native(archive* a, const std::string& path, const size_t block_size)
{
	return archive_read_open_filename(a, path.c_str(), block_size);
}

// When an entry was last changed: what a zip records for each, and what a tar or 7z records beside the rest.
// The change time stands in only for an archive that keeps nothing else.
static core::date_t archive_entry_modified(archive_entry* entry)
{
	const auto has_mtime = archive_entry_mtime_is_set(entry) != 0;
	if (!has_mtime && !archive_entry_ctime_is_set(entry)) return {};
	const auto seconds = static_cast<int64_t>(has_mtime ? archive_entry_mtime(entry) : archive_entry_ctime(entry));
	const auto nanos = has_mtime ? archive_entry_mtime_nsec(entry) : archive_entry_ctime_nsec(entry);
	constexpr auto ticks = core::date_t::intervals_per_second;
	constexpr auto epoch_seconds = static_cast<int64_t>(core::date_t::offset_1970 / ticks);
	if (nanos < 0 || nanos >= 1000000000 || seconds < -epoch_seconds ||
	    seconds > static_cast<int64_t>((UINT64_MAX - 9999999) / ticks) - epoch_seconds)
	{
		core::log_once(__FUNCTION__, "Archive timestamp is outside the supported range");
		return {};
	}
	return core::date_t(static_cast<uint64_t>(seconds + epoch_seconds) * ticks + static_cast<uint64_t>(nanos) / 100);
}

std::vector<archive_item> files::list_archive(const core::file_path zip_file_path)
{
	std::vector<archive_item> results;
	auto* const a = archive_read_new();

	if (!a) return results;

	// Entries are attacker-controlled; a crafted archive can declare millions of them.
	constexpr size_t max_entries = 100 * 1000;

	// RAII so a throw from core::utf8_cast or emplace_back cannot leak the handle
	// and libarchive's decompression buffers.
	const core::scope_exit free_archive([a] { archive_read_free(a); });

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);

	const auto native = platform::to_file_system_path(zip_file_path);

	if (archive_read_open_native(a, native, 10240) == ARCHIVE_OK)
	{
		const core::scope_exit close_archive([a] { archive_read_close(a); });

		archive_entry* entry = nullptr;

		for (auto examined = 0_z; examined < max_entries; ++examined)
		{
			const auto status = archive_read_next_header(a, &entry);
			if (status == ARCHIVE_EOF) break;
			if (status != ARCHIVE_OK)
			{
				const auto* error = archive_error_string(a);
				core::log_once(__FUNCTION__, error ? error : "Cannot read archive entry");
				if (status != ARCHIVE_WARN) break;
			}
			if (!entry)
			{
				core::log_once(__FUNCTION__, "Archive reader returned no entry");
				break;
			}

			// Null for entries whose stored name is not valid UTF-8, which is common
			// in older zip/rar archives using a local code page.
			const auto* const pathname = archive_entry_pathname_utf8(entry);

			if (!pathname) continue;

			archive_item result_info;
			result_info.filename = core::utf8_cast(pathname);
			result_info.uncompressed_size = core::file_size(archive_entry_size(entry));
			result_info.modified = archive_entry_modified(entry);
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
		// clang-format off
		{"en", "eng"}, {"de", "deu"}, {"de", "ger"}, {"cs", "ces"}, {"cs", "cze"},
		{"es", "spa"}, {"fr", "fra"}, {"fr", "fre"}, {"it", "ita"}, {"ja", "jpn"},
		{"ko", "kor"}, {"pl", "pol"}, {"pt", "por"}, {"ru", "rus"}, {"tr", "tur"},
		{"uk", "ukr"}, {"zh", "zho"}, {"zh", "chi"}, {"nl", "nld"}, {"nl", "dut"},
		{"br", "bre"}, {"lv", "lav"}, {"sr", "srp"}, {"sr", "scc"},
		// clang-format on
	};

	for (const auto& m : map)
	{
		if (core::icmp(ui_language, m.ui2) == 0 && core::icmp(iso639, m.iso3) == 0) return true;
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

// Matroska names a tag in a language other than its default "<key>-<lang>" and nothing else, and MOV
// adds such a copy beside the plain key (#238). Splits a key that ends in "-" and a three-letter ISO
// 639-2 code into the two; any other key comes back whole, with no language.
static std::pair<std::string_view, std::string_view> split_language_suffix(const std::string_view key)
{
	constexpr auto code_size = 3_z;
	if (key.size() <= code_size + 1 || key[key.size() - code_size - 1] != '-') return {key, {}};

	const auto code = key.substr(key.size() - code_size);
	const auto is_letter = [](const char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	};
	if (!std::ranges::all_of(code, is_letter)) return {key, {}};

	return {key.substr(0, key.size() - code_size - 1), code};
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

	// The text fields a container can carry in several languages take the same order as the comment:
	// the plain key, then the UI language, then any other, whichever arrives first. A field two keys
	// name shares its priority through the first of them.
	struct text_key
	{
		std::string_view key;
		core::cached prop::item_metadata::* field;
	};

	static constexpr text_key text_keys[] = {
		{"album", &prop::item_metadata::album},
		{"album_artist", &prop::item_metadata::album_artist},
		{"artist", &prop::item_metadata::artist},
		{"composer", &prop::item_metadata::composer},
		{"copyright", &prop::item_metadata::copyright_notice},
		{"description", &prop::item_metadata::description},
		{"encoder", &prop::item_metadata::encoder},
		{"encoded_by", &prop::item_metadata::encoder},
		{"game", &prop::item_metadata::game},
		{"genre", &prop::item_metadata::genre},
		{"performer", &prop::item_metadata::performer},
		{"publisher", &prop::item_metadata::publisher},
		{"show", &prop::item_metadata::show},
		{"programme", &prop::item_metadata::show},
		{"synopsis", &prop::item_metadata::synopsis},
		{"system", &prop::item_metadata::system},
		{"title", &prop::item_metadata::title},
	};

	int text_priority[std::size(text_keys)] = {};

	for (const auto& kv : ffmpeg_metadata)
	{
		const auto [base_key, language] = split_language_suffix(kv.key);
		const auto text = std::ranges::find_if(text_keys,
		                                       [base_key](const text_key& t) { return is_key(base_key, t.key); });

		if (text != std::ranges::end(text_keys))
		{
			const auto first_naming = std::ranges::find(text_keys, text->field, &text_key::field);
			auto& best = text_priority[first_naming - std::ranges::begin(text_keys)];
			const auto priority = language.empty() ? 3 : language_matches_ui(language, ui_lang) ? 2 : 1;

			if (priority >= best)
			{
				best = priority;
				result.*(text->field) = core::strip_and_cache(kv.value);
			}

			continue;
		}

		if (is_key(kv.key, "comment") || core::starts(kv.key, "comment-") || is_key(base_key, "comment"))
		{
			// Pick the best single comment: plain/untagged > current UI language > any other.
			const auto priority = is_key(kv.key, "comment")                          ? 3
			                      : comment_key_matches_ui_language(kv.key, ui_lang) ? 2
			                                                                         : 1;

			if (priority > comment_priority)
			{
				comment_priority = priority;
				result.comment = core::strip_and_cache(kv.value);
			}
		}
		else if (is_key(kv.key, "creation_time") || is_key(kv.key, "date") ||
		         is_key(kv.key, "com.apple.quicktime.creationdate"))
		{
			if (kv.value.size() == 4 || core::ends(kv.value, "00-00"))
			{
				const auto year = core::to_int(kv.value);

				if (year > 1800 && year < 2100)
				{
					result.year = static_cast<uint16_t>(year);
				}
			}
			else
			{
				const auto date = core::date_t::from(kv.value);

				if (date.is_valid())
				{
					result.dates.add_utc(prop::date_source::container_created, date);
					result.year = static_cast<uint16_t>(date.year());
				}
			}
		}
		else if (is_key(kv.key, "date-eng") || is_key(kv.key, "Rip date"))
		{
			const auto date = core::date_t::from(kv.value);
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
		else if (is_key(kv.key, "TDOR") || is_key(kv.key, "TORY") || is_key(kv.key, "originalyear") ||
		         is_key(kv.key, "original_year"))
		{
			const auto date = core::date_t::from(kv.value);
			if (date.is_valid())
			{
				result.dates.add(prop::date_source::id3_original_release, date);
			}
			else if (kv.value.size() == 4 && core::is_num(kv.value))
			{
				const auto year = core::to_int(kv.value);
				if (year > 1800 && year < 2100)
					result.dates.add(prop::date_source::id3_original_release, core::date_t(year, 1, 1, 0, 0, 0));
			}
		}
		else if (is_key(kv.key, "maker") || is_key(kv.key, "make") || is_key(kv.key, "com.apple.quicktime.make"))
		{
			// AVI normalises to "maker", MP4/MOV to "make"; both mean camera manufacturer.
			result.camera_manufacturer = core::strip_and_cache(kv.value);
		}
		else if (is_key(kv.key, "model") || is_key(kv.key, "com.apple.quicktime.model") || is_key(kv.key, "model-eng"))
		{
			result.camera_model = core::strip_and_cache(kv.value);
		}
		else if (is_key(kv.key, "year")) result.year = static_cast<uint16_t>(core::to_int(kv.value));
		else if (is_key(kv.key, "disk") || is_key(kv.key, "disc")) result.disk = core::xy8::parse(kv.value);
		else if (is_key(kv.key, "track")) result.track = core::xy8::parse(kv.value);
		else if (is_key(kv.key, "variant_bitrate")) result.bitrate = core::strip_and_cache(kv.value);
		else if (is_key(kv.key, "episode_sort")) result.episode = core::xy8::parse(kv.value);
		else if (is_key(kv.key, "season_number")) result.season = static_cast<uint16_t>(core::to_int(kv.value));
		else if (is_key(kv.key, "song") && prop::is_null(result.title)) result.title = core::strip_and_cache(kv.value);
		else if (is_key(kv.key, "compatible_brands") || is_key(kv.key, "minor_version"))
		{
			// compatible_brands: 3gp4, avc1isom, isomavc1, isomiso2avc1mp41, isomiso2mp41, isommp42, M4A mp42isom, mp41isom, mp42mp41isomavc1, qt
			// minor_version: 3gp4, avc1isom, isomavc1, isomiso2avc1mp41, isomiso2mp41, isommp42, M4A mp42isom, mp41isom, mp42mp41isomavc1, qt
		}
		else if (is_key(kv.key, "rating"))
		{
			result.rating = static_cast<int16_t>(std::clamp(core::to_int(kv.value), 0, 5));
		}
		else if (is_key(kv.key, "WM/SharedUserRating"))
		{
			// Windows System.Rating is a 0-99 scale (Explorer / Media Player); map it to
			// Diffractor's 0-5 stars. A corresponding XMP property, when present, is
			// parsed afterwards and remains authoritative.
			const auto r = core::to_int(kv.value);
			if (r > 0) result.rating = (r <= 12) ? 1 : (r <= 37) ? 2 : (r <= 62) ? 3 : (r <= 87) ? 4 : 5;
		}
		else if (is_key(kv.key, "keywords"))
		{
			core::split2(kv.value, true,
			             [this](const std::string_view text) { keywords.emplace_back(core::cache(text)); });
		}
		else if (is_key(kv.key, "WM/Category"))
		{
			// Windows Explorer / Media Player tags (MP4 'Xtra' atom, ASF 'WM/*').
			// Values are ';'-delimited and may contain spaces, so split only on ';'.
			core::split2(kv.value, true, [this](const std::string_view text)
			{ windows_categories.emplace_back(core::cache(core::trim(text))); }, [](const char c) { return c == ';'; });
		}
		else if (is_key(kv.key, "location-eng") || is_key(kv.key, "location") ||
		         is_key(kv.key, "com.apple.quicktime.location.ISO6709"))
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

const libfiles::metadata_blocks& file_scan_result::save_metadata(libfiles::metadata_blocks& fallback) const
{
	if (metadata.exif.empty() && metadata.iptc.empty() && metadata.xmp.empty())
	{
		const auto exif = metadata_exif::make_exif(to_props());
		fallback.exif.assign(exif.begin(), exif.end());
		// An image whose only metadata is a colour profile still has to keep it, or a crop or
		// resize silently reinterprets a wide gamut image as sRGB.
		fallback.icc = metadata.icc;
		return fallback;
	}

	return metadata;
}

static prop::date_source date_source_of(const libfiles::date_source source)
{
	switch (source)
	{
	case libfiles::date_source::exif_original: return prop::date_source::exif_original;
	case libfiles::date_source::exif_digitized: return prop::date_source::exif_digitized;
	case libfiles::date_source::exif_modified: return prop::date_source::exif_datetime;
	case libfiles::date_source::exif_gps: return prop::date_source::gps_stamp;
	case libfiles::date_source::iptc_created: return prop::date_source::iptc_created;
	case libfiles::date_source::iptc_digitized: return prop::date_source::iptc_digital_created;
	case libfiles::date_source::xmp_photoshop_created: return prop::date_source::photoshop_created;
	case libfiles::date_source::xmp_create: return prop::date_source::xmp_create;
	case libfiles::date_source::xmp_modify: return prop::date_source::xmp_modify;
	case libfiles::date_source::xmp_exif_digitized: return prop::date_source::xmp_exif_digitized;
	case libfiles::date_source::xmp_exif_original: return prop::date_source::xmp_exif_original;
	}

	return prop::date_source::none;
}

static prop::panorama_projection panorama_of(const libfiles::panorama_projection projection)
{
	switch (projection)
	{
	case libfiles::panorama_projection::unspecified: return prop::panorama_projection::unspecified;
	case libfiles::panorama_projection::equirectangular: return prop::panorama_projection::equirectangular;
	case libfiles::panorama_projection::cylindrical: return prop::panorama_projection::cylindrical;
	case libfiles::panorama_projection::none: break;
	}

	return prop::panorama_projection::none;
}

// A date as the pack holds it: the wall clock to the 100 ns, and its zone - a UTC instant, an offset, or
// none. One the calendar or the pack does not take is left out.
static void add_dated(prop::item_metadata& md, const libfiles::dated& d)
{
	const auto& w = d.when;
	core::date_t value;
	if (!value.date(core::day_t{w.year, w.month, w.day, w.hour, w.minute, w.second})) return;
	if (w.nanoseconds != 0) value = core::date_t(value.to_int64() + static_cast<uint64_t>(w.nanoseconds / 100));

	const auto offset = w.zone == libfiles::date::time_zone::utc      ? prop::date_pack::utc_instant
	                    : w.zone == libfiles::date::time_zone::offset ? static_cast<int16_t>(w.offset_minutes)
	                                                                  : prop::date_pack::no_offset;
	md.dates.add(date_source_of(d.source), value, offset);
}

void apply_properties(prop::item_metadata& md, const libfiles::properties& p)
{
	using libfiles::property;

	const auto text = [&p](core::cached& field, const std::string& value, const property id)
	{
		if (p.states(id)) field = core::cache(value);
	};

	text(md.title, p.title, property::title);
	text(md.artist, p.artist, property::artist);
	text(md.album, p.album, property::album);
	text(md.album_artist, p.album_artist, property::album_artist);
	text(md.composer, p.composer, property::composer);
	text(md.performer, p.performer, property::performer);
	text(md.genre, p.genre, property::genre);
	text(md.publisher, p.publisher, property::publisher);
	text(md.copyright_notice, p.copyright, property::copyright);
	text(md.description, p.description, property::description);
	text(md.synopsis, p.synopsis, property::synopsis);
	text(md.show, p.show, property::show);
	text(md.encoder, p.encoder, property::encoder);
	text(md.camera_manufacturer, p.camera_make, property::camera_make);
	text(md.camera_model, p.camera_model, property::camera_model);
	text(md.lens, p.lens, property::lens);
	text(md.location_place, p.city, property::city);
	text(md.location_state, p.state, property::state);
	text(md.location_country, p.country, property::country);
	text(md.copyright_credit, p.credit, property::credit);
	text(md.copyright_source, p.source, property::source);
	text(md.copyright_url, p.copyright_url, property::copyright_url);
	text(md.label, p.label, property::label);
	text(md.raw_file_name, p.raw_file_name, property::raw_file_name);

	if (p.states(property::comments))
		md.comment = p.comments.empty() ? core::cached{} : core::cache(p.comments.front().text);
	if (p.states(property::creators)) md.copyright_creator = core::cache(core::combine(p.creators));
	// A tag of more than one word is quoted, so the item's tags split back into what the file named.
	if (p.states(property::keywords)) md.tags = core::cache(core::combine(p.keywords));
	if (p.states(property::rating) && p.rating) md.rating = static_cast<int16_t>(*p.rating);
	if (p.states(property::year)) md.year = static_cast<uint16_t>(p.year);
	if (p.states(property::track)) md.track = prop::number_pair(p.track, p.track_count);
	if (p.states(property::disc)) md.disk = prop::number_pair(p.disc, p.disc_count);
	if (p.states(property::episode)) md.episode = prop::number_pair(p.episode, p.episode_count);
	if (p.states(property::season)) md.season = static_cast<uint16_t>(p.season);
	if (p.states(property::f_number)) md.f_number = static_cast<float>(p.f_number);
	if (p.states(property::exposure_time)) md.exposure_time = static_cast<float>(p.exposure_time);
	if (p.states(property::focal_length)) md.focal_length = static_cast<float>(p.focal_length);
	if (p.states(property::focal_length_35mm))
		md.focal_length_35mm_equivalent = static_cast<uint16_t>(p.focal_length_35mm);
	if (p.states(property::iso_speed)) md.iso_speed = static_cast<uint16_t>(p.iso_speed);
	if (p.states(property::altitude) && p.altitude) md.altitude = static_cast<float>(*p.altitude);
	if (p.states(property::speed)) md.gps_speed = static_cast<float>(p.speed);
	if (p.states(property::orientation) && p.orientation)
		md.orientation = static_cast<core::orientation>(*p.orientation);
	if (p.states(property::panorama)) md.panorama = panorama_of(p.panorama);

	// A position the index cannot hold pins the item nowhere rather than somewhere wrong.
	if (p.states(property::location) && p.location)
	{
		const gps_coordinate coordinate(p.location->latitude, p.location->longitude);
		if (coordinate.is_valid()) md.coordinate = coordinate;
	}

	for (const auto& d : p.dates) add_dated(md, d);
}

// What makes a picture or a video HDR, by the H.273 transfer its own colour description names, else by the gain
// map it carries.
static prop::hdr_kind hdr_kind_of(const uint8_t transfer, const bool gain_map)
{
	if (transfer == 16) return prop::hdr_kind::pq;
	if (transfer == 18) return prop::hdr_kind::hlg;
	return gain_map ? prop::hdr_kind::gain_map : prop::hdr_kind::none;
}

// The colour space a file says its pixels are in: its ICC profile's own name, else the primaries its coded colour
// description names, as photographers and video editors each call them - BT.709's primaries are sRGB's. Nothing
// for a file that says neither, rather than the sRGB it is most likely to be.
static core::cached color_profile_of(const std::span<const uint8_t> icc, const uint8_t primaries, const bool video)
{
	if (!icc.empty())
	{
		if (const auto summary = libfiles::summarize_icc(icc))
		{
			constexpr size_t max_name = 96;
			const auto name = core::trim(std::string_view(summary->name).substr(0, max_name));
			if (!name.empty()) return core::cache(name);
		}
	}

	switch (primaries)
	{
	case 1: return core::cache(video ? "BT.709"sv : "sRGB"sv);
	case 5:
	case 6: return core::cache("BT.601"sv);
	case 9: return core::cache("BT.2020"sv);
	case 11: return core::cache("DCI-P3"sv);
	case 12: return core::cache("Display P3"sv);
	default: return {};
	}
}

prop::item_metadata_ptr file_scan_result::to_props() const
{
	auto result = std::make_shared<prop::item_metadata>();

	// A still's EXIF, then its IPTC over it, reconciled by libfiles' rule.
	libfiles::properties blocks;
	if (tiff_metadata) blocks = libfiles::properties_from_exif(*tiff_metadata);
	else if (!metadata.exif.empty()) blocks = libfiles::properties_from_exif(libfiles::read_exif(metadata.exif));
	if (!metadata.iptc.empty())
		libfiles::overlay(blocks, libfiles::properties_from_iptc(libfiles::read_iptc(metadata.iptc)));
	apply_properties(*result, blocks);

	if (!ffmpeg_metadata.empty())
	{
		parse_metadata_ffmpeg_kv(*result);
	}

	if (media)
	{
		parse_media_properties(*result);
	}

	// A media manager's details over the container's tags, and under the file's XMP: the user's own edits.
	if (nfo)
	{
		apply_nfo(*result, *nfo);
	}

	const auto packet = libfiles::properties_from_xmp(metadata.xmp);
	apply_properties(*result, packet);

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
	if (prop::is_null(result->duration)) result->duration = static_cast<uint16_t>(core::round(duration));
	if (prop::is_null(result->audio_codec)) result->audio_codec = audio_codec;
	if (prop::is_null(result->audio_channels)) result->audio_channels = static_cast<uint16_t>(audio_channels);
	if (prop::is_null(result->audio_sample_type)) result->audio_sample_type = static_cast<uint16_t>(audio_sample_type);
	if (prop::is_null(result->audio_sample_rate))
		result->audio_sample_rate = static_cast<uint32_t>(std::clamp<int64_t>(audio_sample_rate, 0, UINT32_MAX));

	if (!result->coordinate.is_valid())
	{
		result->coordinate = gps;
	}

	// Native container keywords join the tags only where the packet does not state its own: an unrelated
	// packet says nothing about tags, while an empty dc:subject is authoritative and must not resurrect
	// stale native tags.
	const auto packet_states_tags = packet.states(libfiles::property::keywords);
	if (!result->tags.is_empty() || (!keywords.empty() && !packet_states_tags) ||
	    (!windows_categories.empty() && !packet_states_tags))
	{
		auto tags = split(result->tags, true);

		// Native container keywords (the MP4 'KEYW' atom, or Windows Explorer /
		// Media Player tags from the 'Xtra' atom / ASF 'WM/Category') are only merged when
		// XMP has no dc:subject.
		if (!packet_states_tags)
		{
			tags.insert(tags.end(), keywords.begin(), keywords.end());
			tags.insert(tags.end(), windows_categories.begin(), windows_categories.end());
		}

		std::ranges::sort(tags, core::iless());
		tags.erase(std::ranges::unique(tags, core::ieq()).begin(), tags.end());
		result->tags = core::cache(core::combine(tags));
	}

	if (!prop::is_null(width)) result->width = to_u16(width);
	if (!prop::is_null(height)) result->height = to_u16(height);
	if (!prop::is_null(pixel_format)) result->pixel_format = pixel_format;
	if (orientation_applied || orientation != core::orientation::top_left) result->orientation = orientation;

	// What the picture or video is coded as, from the file alone: no packet states these.
	result->bit_depth = picture.bit_depth;
	if (animated) result->picture_flags |= prop::picture_flag::animated;
	if (picture.alpha) result->picture_flags |= prop::picture_flag::alpha;
	result->hdr = hdr_kind_of(colour_transfer, gain_map);
	if (frame_rate > 0.0 && frame_rate < 100000.0) result->frame_rate = static_cast<float>(frame_rate);
	result->color_profile = color_profile_of(metadata.icc, colour_primaries, media != nullptr);
	if (!prop::is_null(contents)) result->contents = contents;

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
		const auto kept = std::min(payload_len, core::max_hex_dump_bytes);
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
		const core::cspan bytes{binary->bytes.data(), binary->bytes.size()};
		mem_read_stream header_stream(bytes);
		const auto scanned = scan_photo(header_stream);
		const sizei scanned_dimensions{static_cast<int>(scanned.width), static_cast<int>(scanned.height)};

		if (!scanned_dimensions.is_empty() && files::exceeds_decode_budget(scanned_dimensions)) continue;

		auto surface = ff.image_to_surface(bytes, preview_limit, false, decode_intent::thumbnail);

		if (core::is_valid(surface))
		{
			row.detail = metadata_image_detail{std::move(surface)};
			// An image is the point of the row, so it is shown without being asked for.
			row.open_by_default = true;
		}
		else
		{
			binary->is_image = false;
			if (binary->bytes.size() > core::max_hex_dump_bytes) binary->bytes.resize(core::max_hex_dump_bytes);
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
		raw.assign(std::bit_cast<const char*>(metadata.xmp.data()), std::min(metadata.xmp.size(), max_raw_bytes));

		result.metadata.emplace_back(metadata_standard::xmp, std::move(kv), metadata.xmp.size(), parsed,
		                             std::move(raw));
	}

	if (!metadata.icc.empty())
	{
		auto kv = metadata_icc::to_info(metadata.icc, hdr);
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

///////////////////////////////////////////////////////////////////////////////////////////////////
// The still image formats libfiles names from their signatures, one entry each: the file type it
// registers, how its header is scanned without decoding, how a file of it loads, and how in-memory bytes
// of it decode. Scanning, loading and decoding all dispatch through this table.

namespace
{
	core::surface_ptr fitted(files& ff, core::surface_ptr loaded, const still_decode_request& request)
	{
		return is_valid(loaded) ? ff.fit_within(std::move(loaded), request.target_extent) : core::surface_ptr{};
	}

	// These decoders report a malformed file by throwing. The failure is logged and answered with
	// nothing, so the caller still offers the bytes to ffmpeg.
	template <typename Decode> core::surface_ptr decode_or_log(Decode&& decode)
	{
		try
		{
			return decode();
		}
		catch (const std::exception& e)
		{
			core::log("files::image_to_surface", e.what());
			return {};
		}
	}

	constexpr auto xmp_edit = file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata;

	constexpr still_format s_still_formats[] = {
		// clang-format off
		{
			.format = libfiles::image_format::psd,
			.extensions = "psd",
			.description = "Adobe Photoshop Drawing",
			.traits = xmp_edit,
			.scan = scan_photo_libfiles,
			.load = [](read_stream& s, load_diagnostic* const d, picture_signal* const e)
			{ return load_psd(s, d, e == nullptr); },
			.decode = [](files& ff, const core::cspan data, const still_decode_request& r)
			{
				mem_read_stream stream(data);
				return fitted(ff, load_psd(stream, nullptr, r.intent != decode_intent::edit), r);
			},
		},
		{
			.format = libfiles::image_format::gif,
			.extensions = "gif,giff",
			.description = "CompuServe's Graphics Interchange Format",
			.traits = xmp_edit,
			.scan = scan_photo_libfiles,
		},
		{
			.format = libfiles::image_format::jxl,
			.extensions = "jxl",
			.description = "JPEG XL",
			.traits = xmp_edit,
			.scan = scan_jxl_libfiles,
			.load = [](read_stream& s, load_diagnostic* const d, picture_signal* const e) { return load_jxl(s, d, e); },
			.decode = [](files& ff, const core::cspan data, const still_decode_request& r)
			{
				return decode_or_log([&ff, data, &r]
				{
					mem_read_stream stream(data);
					return fitted(ff, load_jxl(stream), r);
				});
			},
		},
		{
			.format = libfiles::image_format::webp,
			.extensions = "webp",
			.traits = xmp_edit,
			.encoded = core::image_format::WEBP,
			.scan = scan_photo_libfiles,
			.decode = [](files& ff, const core::cspan data, const still_decode_request& r)
			{
				return decode_or_log([&ff, data, &r]
				{
					return fitted(ff, load_webp(data, r.can_use_yuv, r.target_extent, r.intent != decode_intent::edit),
					              r);
				});
			},
		},
		{
			.format = libfiles::image_format::jpeg,
			.extensions = "jpeg,jpg,jpe,jfif",
			.description = "Joint Photographic Experts Group",
			.traits = xmp_edit,
			.encoded = core::image_format::JPEG,
			.scan = scan_photo_libfiles,
			.decode = decode_still_jpeg,
		},
		{
			.format = libfiles::image_format::bmp,
			.extensions = "bmp",
			.description = "Microsoft Windows Bitmap",
			.scan = scan_photo_libfiles,
		},
		{
			.format = libfiles::image_format::png,
			.extensions = "png",
			.description = "Portable Network Graphic",
			.traits = xmp_edit,
			.encoded = core::image_format::PNG,
			.scan = scan_photo_libfiles,
			.decode = [](files& ff, const core::cspan data, const still_decode_request& r)
			{
				return decode_or_log([&ff, data, &r]
				{
					const auto edit = r.intent == decode_intent::edit;
					return fitted(ff, load_png(data, edit, nullptr, !edit), r);
				});
			},
		},
		{
			.format = libfiles::image_format::tiff,
			.extensions = "tiff, tif",
			.description = "Tagged Image File Format",
			.traits = xmp_edit,
			.scan = scan_photo_libfiles,
		},
		{
			.format = libfiles::image_format::heif,
			.extensions = "heif, heifs, heic, heics, avci, avcs, avif, avifs",
			.description = "High Efficiency Image File Format",
			.traits = xmp_edit,
			.scan = scan_heif_libfiles,
			.load = [](read_stream& s, load_diagnostic* const d, picture_signal* const e)
			{ return load_heif(s, d, e, e == nullptr); },
			.decode = [](files& ff, const core::cspan data, const still_decode_request& r)
			{
				return decode_or_log([&ff, data, &r]
				{
					mem_read_stream stream(data);
					return fitted(ff, load_heif(stream, nullptr, nullptr, r.intent != decode_intent::edit), r);
				});
			},
		},
		// clang-format on
	};
}

// A JPEG decodes at a reduced scale already. Planar YUV for display goes to the sampler as it is,
// which resizes at draw time; a thumbnail is about to be encoded, so it is reduced here - and stays
// planar, because that is the form VP8 wants.
core::surface_ptr decode_still_jpeg(files& ff, const core::cspan data, const still_decode_request& request)
{
	bool is_yuv = false;
	auto decoded = ff.decode_jpeg(data, request.target_extent, request.can_use_yuv, {}, is_yuv, {}, request.intent);

	if (!is_valid(decoded)) return {};

	return is_yuv && request.intent != decode_intent::thumbnail
	           ? std::move(decoded)
	           : ff.fit_within(std::move(decoded), request.target_extent);
}

std::span<const still_format> still_formats()
{
	return s_still_formats;
}

const still_format* find_still_format(const libfiles::image_format format)
{
	for (const auto& f : s_still_formats)
	{
		if (f.format == format) return &f;
	}

	return nullptr;
}

// Every still format is a photo.
file_type still_file_type(const libfiles::image_format format)
{
	const auto* const f = find_still_format(format);
	core::assert_true(f != nullptr);
	return f ? file_type(file_group::photo, f->extensions, f->description, f->traits) : file_type::other;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Still-image scanning. Every still format - JPEG, PNG, WebP, GIF, TIFF, PSD, BMP, HEIF and AVIF, and
// JPEG XL - is read by libfiles, whose result is mapped onto file_scan_result here; a format adds what
// libfiles leaves to its caller, decoding a HEIF thumbnail item or JPEG XL's Brotli. Embedded thumbnails
// are read and decoded only for a caller that wants one.

// Reached only for an embedded thumbnail whose format load_image_file cannot wrap, which needs a
// files instance to decode. Borrows the caller's rather than building a second libjpeg context.
static core::surface_ptr decode_thumbnail(files* const decoder, const core::cspan data)
{
	if (decoder) return decoder->image_to_surface(data, {}, false, decode_intent::thumbnail);
	files ff;
	return ff.image_to_surface(data, {}, false, decode_intent::thumbnail);
}

metadata_kv_list to_metadata_kv(std::vector<libfiles::structure_row>& rows)
{
	metadata_kv_list result;
	result.reserve(rows.size());

	for (auto& row : rows)
	{
		auto& kv = result.emplace_back(std::move(row.key), std::move(row.value));
		kv.shape = std::move(row.shape);
		kv.id = std::move(row.id);
		kv.depth = row.depth;
		kv.container = row.container;
		kv.open_by_default = row.open_by_default;

		if (auto* const text = std::get_if<std::string>(&row.detail))
		{
			kv.detail = metadata_text_detail{std::move(*text)};
		}
		else if (auto* const bytes = std::get_if<std::vector<uint8_t>>(&row.detail))
		{
			kv.detail = metadata_binary_detail{std::move(*bytes)};
		}
		else if (auto* const table = std::get_if<libfiles::structure_table>(&row.detail))
		{
			kv.detail = metadata_numeric_detail{std::move(table->values), table->columns};
		}
	}

	return result;
}

namespace
{

	// An embedded thumbnail: JPEG, PNG and WebP stay encoded, anything else recognisable is decoded.
	void load_thumbnail(file_scan_result& result, const core::cspan data, files* const decoder)
	{
		const auto detected = libfiles::detect_image_format(data);
		if (detected == libfiles::image_format::unknown) return;

		if (is_image_format(detected)) result.thumbnail_image = load_image_file(data);
		else result.thumbnail_surface = decode_thumbnail(decoder, data);
	}

	// The Exif thumbnail, when wanted, and the orientation every thumbnail takes: an Exif thumbnail is
	// stored unrotated whatever the container did, and a decoded one needs it only where the decoder
	// did not already apply that item's own transform.
	void finish_with_exif(file_scan_result& result, const libfiles::byte_range thumbnail, const bool want_thumbnail,
	                      files* const decoder)
	{
		if (want_thumbnail && !thumbnail.empty())
		{
			load_thumbnail(result,
			               {result.metadata.exif.data() + thumbnail.offset, static_cast<size_t>(thumbnail.size)},
			               decoder);
		}

		if (result.thumbnail_image) result.thumbnail_image->orientation(result.orientation);

		if (result.thumbnail_surface && !result.thumbnail_orientation_applied)
		{
			result.thumbnail_surface->orientation(result.orientation);
		}

		// The decoder already turned the pixels for a transform the container stored.
		if (result.orientation_applied)
		{
			result.orientation = core::orientation::top_left;
		}
	}
}

file_scan_result scan_still_libfiles(read_stream& s, const libfiles::scan_intent intent, const bool want_thumbnail,
                                     files* const decoder, const still_scan_extras& extras)
{
	file_scan_result result;

	if (s.size() < sizeof(pack128)) return result;

	try
	{
		read_stream_source source(s);
		const auto resident = s.view();
		std::optional<libfiles::reader> r;

		if (resident.empty()) r.emplace(source);
		else r.emplace(std::span(resident.data, resident.size));

		libfiles::scan_limits limits;
		limits.jpeg_photoshop_resources = files_test_hooks::jpeg_photoshop_resource_cap();
		limits.brotli = extras.brotli;

		auto scanned = libfiles::scan(*r, intent, limits);

		if (!scanned.error.empty())
		{
			// A fault discards the whole scan, as a throw from a scanner always has.
			core::log(__FUNCTION__, scanned.error);
			return result;
		}

		result.format = scanned.format;

		// BMP alone names its format only once its header has been read.
		if (scanned.format == libfiles::image_format::bmp && !scanned.success)
			result.format = libfiles::image_format::unknown;

		result.width = scanned.width;
		result.height = scanned.height;
		if (!scanned.pixel_format.empty()) result.pixel_format = core::cache(scanned.pixel_format);
		result.picture = scanned.picture;
		if (scanned.orientation) result.orientation = static_cast<core::orientation>(*scanned.orientation);

		result.metadata = std::move(scanned.metadata);
		result.tiff_metadata = std::move(scanned.tiff_metadata);
		result.structure_metadata = to_metadata_kv(scanned.structure);

		// A TIFF file's own GPS position, read as an EXIF block's is.
		if (scanned.gps)
		{
			const gps_coordinate coordinate(scanned.gps->latitude, scanned.gps->longitude);
			if (coordinate.is_valid()) result.gps = coordinate;
		}

		result.success = scanned.success;
		// A HEIF item's own rotation or mirror, which its decoder applies.
		result.orientation_applied = scanned.container_transform;
		result.animated = scanned.animated;
		result.hdr = scanned.hdr;
		result.gain_map = scanned.gain_map;
		result.colour_primaries = scanned.colour_primaries;
		result.colour_transfer = scanned.colour_transfer;

		// A thumbnail costs a read and a copy, so it is read only for a caller that wants one. The JFIF
		// extension thumbnail stands only where the Exif one does not.
		if (want_thumbnail)
		{
			if (extras.container_thumbnail) extras.container_thumbnail(s, result);

			if (!scanned.jfif_thumbnail.empty())
			{
				const auto bytes = r->read(scanned.jfif_thumbnail.offset,
				                           static_cast<size_t>(scanned.jfif_thumbnail.size));
				result.thumbnail_image = load_image_file({bytes.data(), bytes.size()});
			}

			if (!scanned.tiff_thumbnail.empty())
			{
				const auto bytes = r->read(scanned.tiff_thumbnail.offset,
				                           static_cast<size_t>(scanned.tiff_thumbnail.size));
				if (bytes.size() > 16) load_thumbnail(result, {bytes.data(), bytes.size()}, decoder);
			}
		}

		if (!result.metadata.exif.empty())
		{
			finish_with_exif(result, scanned.exif_thumbnail, want_thumbnail, decoder);
		}
	}
	catch (std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	return result;
}

file_scan_result scan_photo_libfiles(read_stream& s, const libfiles::scan_intent intent, const bool want_thumbnail,
                                     files* const decoder)
{
	return scan_still_libfiles(s, intent, want_thumbnail, decoder, {});
}

file_scan_result scan_photo(read_stream& s, const libfiles::scan_intent intent, const bool want_thumbnail,
                            files* const decoder)
{
	file_scan_result result;

	// Empty or too small to contain a recognisable image signature.
	if (s.size() < sizeof(pack128)) return result;

	try
	{
		const auto* const still = find_still_format(libfiles::detect_image_format(s.peek128(0)));

		if (still && still->scan)
		{
			result = still->scan(s, intent, want_thumbnail, decoder);
		}
	}
	catch (std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Audio and video scanning. libfiles reads the container - its streams, duration, tags, XMP packet and
// cover art - and its result is mapped here onto file_scan_result, the item's properties and the info
// panel, in Diffractor's own terms: short codec names, a pixel format as chroma and bit depth, and the
// audio sample type as coded. FFmpeg is opened only to decode, and to describe the streams of a file
// libfiles cannot describe whole.

namespace
{
	// The most cover art a scan reads into memory, as FFmpeg's attached picture was bounded by the file.
	constexpr uint64_t max_cover_art_bytes = 16ull * 1024ull * 1024ull;

	const libfiles::media_stream* first_stream(const libfiles::media_scan_result& scan,
	                                           const libfiles::media_stream_kind kind)
	{
		const auto found = std::ranges::find(scan.streams, kind, &libfiles::media_stream::kind);
		return found == scan.streams.end() ? nullptr : &*found;
	}

	// The front cover when there is one, else the first picture that can be cover art; none when every picture
	// the file carries is something else.
	const libfiles::media_picture* cover_picture(const libfiles::media_scan_result& scan)
	{
		const auto front = std::ranges::find(scan.pictures, id3_front_cover, &libfiles::media_picture::type);
		if (front != scan.pictures.end()) return &*front;

		const auto art = std::ranges::find_if(scan.pictures, [](const libfiles::media_picture& p)
		{ return is_cover_art_picture_type(p.type); });
		return art == scan.pictures.end() ? nullptr : &*art;
	}

	// The container's turn and mirror, numbered as EXIF numbers an orientation, as the player reads them from
	// FFmpeg's display matrix.
	core::orientation orientation_of(const libfiles::video_format& v)
	{
		return static_cast<core::orientation>(v.orientation());
	}

	// The height a non-square pixel displays at, as the FFmpeg path computes it for anamorphic video.
	uint32_t display_height(const libfiles::video_format& v)
	{
		const auto num = v.pixel_aspect_width;
		const auto den = v.pixel_aspect_height;
		if (num == 0 || den == 0 || num == den || v.width == 0 || v.height == 0) return v.height;
		return static_cast<uint32_t>(
			core::mul_div(static_cast<int64_t>(v.height), static_cast<int64_t>(den), static_cast<int64_t>(num)));
	}

	core::date_t to_date(const libfiles::media_date& d)
	{
		if (!d.valid() || d.month == 0) return {};
		return {d.year, d.month, d.day == 0 ? 1 : d.day, d.hour, d.minute, d.second};
	}

	// A date as the pack records it: an instant, a wall-clock reading at a stated offset, or one in a
	// zone the file does not name.
	void add_date(prop::item_metadata& result, const prop::date_source source, const libfiles::media_date& d)
	{
		const auto date = to_date(d);
		if (!date.is_valid()) return;

		switch (d.zone)
		{
		case libfiles::media_date::time_zone::utc: result.dates.add_utc(source, date); break;
		case libfiles::media_date::time_zone::offset:
			result.dates.add(source, date, static_cast<int16_t>(d.offset_minutes));
			break;
		case libfiles::media_date::time_zone::unstated: result.dates.add(source, date); break;
		}
	}

	std::string tag_key(const libfiles::media_tag& tag)
	{
		if (tag.language.empty()) return tag.key;
		return std::format("{} ({})", tag.key, tag.language);
	}

	av_stream_type to_stream_type(const libfiles::media_stream_kind kind)
	{
		switch (kind)
		{
		case libfiles::media_stream_kind::video: return av_stream_type::video;
		case libfiles::media_stream_kind::audio: return av_stream_type::audio;
		case libfiles::media_stream_kind::subtitle: return av_stream_type::subtitle;
		case libfiles::media_stream_kind::data: return av_stream_type::data;
		}

		return av_stream_type::data;
	}

	// What libfiles says of a stream, over what the panel's row held.
	void describe_stream(const libfiles::media_stream& s, av_stream_info& row)
	{
		if (!s.codec.empty()) row.codec = s.codec;
		if (!s.codec_tag.empty()) row.fourcc = s.codec_tag;
		if (!s.title.empty()) row.title = s.title;
		if (!s.language.empty()) row.language = s.language;
		row.is_commentary = s.commentary;
		row.is_audio_description = s.audio_description;

		if (s.kind == libfiles::media_stream_kind::video)
		{
			row.pixel_format = s.video.pixel_format();
			row.profile = s.video.profile;
			row.bit_depth = s.video.bit_depth;
			row.hdr_transfer = std::string(s.video.hdr_transfer());
			row.rotation = s.video.rotation;

			// A stream FFmpeg opened is shown by the colour and fields FFmpeg found, so libfiles says what it left
			// unsaid. libfiles' code points are H.273's too, and its range is full only where the stream says so.
			const auto& v = s.video;
			const auto no_matrix = v.chroma == libfiles::chroma_format::rgb ||
			                       v.chroma == libfiles::chroma_format::gray;
			if (row.color_primaries == 2) row.color_primaries = v.color_primaries;
			if (row.color_transfer == 2) row.color_transfer = v.transfer;
			if (row.color_matrix == 2) row.color_matrix = no_matrix ? 0 : v.matrix;
			if (!row.full_range.has_value() && v.full_range) row.full_range = true;
			if (row.coded_size.is_empty()) row.coded_size = {static_cast<int>(v.width), static_cast<int>(v.height)};
			if (row.scan == av_scan::unknown && v.interlaced) row.scan = av_scan::interlaced;
		}
		else if (s.kind == libfiles::media_stream_kind::audio)
		{
			row.audio_sample_rate = static_cast<int>(s.audio.sample_rate);
			row.audio_channels = static_cast<int>(s.audio.channels);
			row.audio_sample_type = coded_sample_type(s.audio.bits_per_sample, s.audio.float_samples);
			if (!s.audio.profile.empty()) row.profile = s.audio.profile;
		}
	}
}

prop::audio_sample_t coded_sample_type(const unsigned bits_per_sample, const bool float_samples)
{
	if (bits_per_sample == 0) return prop::audio_sample_t::none;
	if (float_samples)
		return bits_per_sample > 32 ? prop::audio_sample_t::signed_double : prop::audio_sample_t::signed_float;
	if (bits_per_sample <= 8) return prop::audio_sample_t::unsigned_8bit;
	if (bits_per_sample <= 16) return prop::audio_sample_t::signed_16bit;
	if (bits_per_sample <= 24) return prop::audio_sample_t::signed_24bit;
	if (bits_per_sample <= 32) return prop::audio_sample_t::signed_32bit;
	return prop::audio_sample_t::signed_64bit;
}

bool is_cover_art_picture_type(const int id3_type)
{
	// The 32x32 file icon, another file icon, and the bright coloured fish.
	return id3_type != 1 && id3_type != 2 && id3_type != 17;
}

media_scan_read scan_media_file(const platform::file_ptr& f, const libfiles::scan_intent intent,
                                const bool want_cover_art)
{
	media_scan_read result;
	if (!f) return result;

	try
	{
		platform_file_stream stream(f);
		libfiles::reader r(stream);
		auto scan = std::make_shared<libfiles::media_scan_result>(libfiles::scan_media(r, intent));

		// A format libfiles does not read is FFmpeg's to describe, which is no fault.
		if (!scan->error.empty() && scan->format != libfiles::media_format::unknown)
		{
			core::log(__FUNCTION__, scan->error);
		}

		if (want_cover_art && scan->success)
		{
			if (const auto* const picture = cover_picture(*scan))
			{
				if (!picture->data.empty())
				{
					result.cover_art.assign(picture->data.begin(), picture->data.end());
				}
				else if (!picture->location.empty() && picture->location.size <= max_cover_art_bytes &&
				         r.contains(picture->location.offset, picture->location.size))
				{
					const auto bytes = r.read(picture->location.offset, static_cast<size_t>(picture->location.size));
					result.cover_art.assign(bytes.begin(), bytes.end());
				}
			}
		}

		result.scan = std::move(scan);
	}
	catch (const std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	// FFmpeg reads the handle from where it stands, so a decoder opened next must find it at the start.
	f->seek(0, platform::file::whence::begin);
	return result;
}

void apply_media_scan(file_scan_result& result, media_scan_read&& read, const bool stream_facts)
{
	if (!read.scan || !read.scan->success) return;

	const auto& scan = *read.scan;
	result.metadata.xmp.assign(scan.xmp.begin(), scan.xmp.end());
	if (!read.cover_art.empty()) result.cover_art = load_image_file(read.cover_art);

	if (stream_facts)
	{
		result.nb_streams = static_cast<uint32_t>(scan.streams.size());
		result.duration = core::round(scan.duration);
		if (scan.bit_rate > 0) result.bitrate = core::cache(prop::format_bit_rate(static_cast<int64_t>(scan.bit_rate)));

		if (const auto* const v = first_stream(scan, libfiles::media_stream_kind::video))
		{
			if (!v->codec.empty()) result.video_codec = core::cache(v->codec);
			result.width = v->video.width;
			result.height = display_height(v->video);
			if (const auto name = v->video.pixel_format(); !name.empty()) result.pixel_format = core::cache(name);
			result.picture = v->video.picture();
			result.orientation = orientation_of(v->video);
			result.frame_rate = v->video.frame_rate;
			result.colour_primaries = v->video.color_primaries;
			result.colour_transfer = v->video.transfer;
		}

		if (const auto* const a = first_stream(scan, libfiles::media_stream_kind::audio))
		{
			if (!a->codec.empty()) result.audio_codec = core::cache(a->codec);
			result.audio_sample_rate = static_cast<int>(a->audio.sample_rate);
			result.audio_channels = static_cast<int>(a->audio.channels);
			result.audio_sample_type = coded_sample_type(a->audio.bits_per_sample, a->audio.float_samples);
		}
	}

	result.media = std::move(read.scan);
}

void file_scan_result::parse_media_properties(prop::item_metadata& result) const
{
	const auto& p = media->properties;

	const auto set = [](core::cached& to, const std::string& value)
	{
		if (!value.empty()) to = core::strip_and_cache(value);
	};

	set(result.album, p.album);
	set(result.show, p.show);
	set(result.album_artist, p.album_artist);
	set(result.artist, p.artist);
	set(result.description, p.description);
	set(result.composer, p.composer);
	set(result.copyright_notice, p.copyright);
	set(result.encoder, p.encoder);
	set(result.genre, p.genre);
	set(result.publisher, p.publisher);
	set(result.synopsis, p.synopsis);
	set(result.title, p.title);
	set(result.camera_manufacturer, p.camera_make);
	set(result.camera_model, p.camera_model);
	set(result.performer, p.performer);

	// One comment is shown: an untagged one first, then one in the UI language, then any other.
	// Every comment stays in the info panel's tags.
	auto comment_priority = 0;

	for (const auto& c : p.comments)
	{
		const auto priority = c.language.empty() ? 3 : language_matches_ui(c.language, setting.language) ? 2 : 1;

		if (priority > comment_priority && !core::is_empty(c.text))
		{
			comment_priority = priority;
			result.comment = core::strip_and_cache(c.text);
		}
	}

	if (p.year != 0) result.year = static_cast<uint16_t>(p.year);
	if (p.track != 0) result.track = prop::number_pair(p.track, p.track_count);
	if (p.disc != 0) result.disk = prop::number_pair(p.disc, p.disc_count);

	// An iTunes film (stik 9) is no episode, whatever tvsn and tves it was left with.
	if (p.media_kind != 9)
	{
		if (p.season != 0) result.season = static_cast<uint16_t>(std::clamp(p.season, 0, 9999));
		if (p.episode != 0) result.episode = prop::number_pair(p.episode, 0);
	}

	if (p.rating) result.rating = static_cast<int16_t>(std::clamp(*p.rating, 0, 5));

	add_date(result, prop::date_source::container_created, p.created);
	if (p.created.valid() && p.created.year > 1800 && p.created.year < 2100 && prop::is_null(result.year))
		result.year = static_cast<uint16_t>(p.created.year);

	// A tag giving only a year - ©day "1998", TYER, WM/Year - dates its track or film to that year, rather than
	// leaving it to the day the file was copied.
	const auto year_only = p.created.valid() ? (p.created.month == 0 ? p.created.year : 0) : p.year;
	if (year_only > 1800 && year_only < 2100)
		result.dates.add(prop::date_source::container_created, core::date_t(year_only, 1, 1, 0, 0, 0));

	if (p.original_release.valid())
	{
		if (p.original_release.month == 0)
		{
			if (p.original_release.year > 1800 && p.original_release.year < 2100)
				result.dates.add(prop::date_source::id3_original_release,
				                 core::date_t(p.original_release.year, 1, 1, 0, 0, 0));
		}
		else
		{
			add_date(result, prop::date_source::id3_original_release, p.original_release);
		}
	}

	// A rip tag is a wall-clock reading: nobody wrote a zone for it.
	for (const auto& tag : media->tags)
	{
		if (core::icmp(tag.key, "TXXX:Rip date") == 0)
		{
			const auto date = core::date_t::from(tag.value);
			if (date.is_valid()) result.dates.add(prop::date_source::rip_date, date);
		}
	}

	for (const auto& k : p.keywords)
	{
		if (!k.empty()) keywords.emplace_back(core::cache(k));
	}

	if (p.location)
	{
		const gps_coordinate coordinate(p.location->latitude, p.location->longitude);
		if (coordinate.is_valid()) gps = coordinate;
	}
}

void add_media_blocks(const libfiles::media_scan_result& scan, av_media_info& info, const bool with_xmp)
{
	metadata_kv_list tags;

	for (const auto& tag : scan.tags)
	{
		tags.emplace_back(tag_key(tag), tag.value);
	}

	if (!tags.empty()) info.metadata.emplace_back(metadata_standard::media, std::move(tags));

	if (with_xmp && !scan.xmp.empty())
	{
		bool parsed = false;
		const core::cspan packet{scan.xmp.data(), scan.xmp.size()};
		auto kv = metadata_xmp::to_info(packet, parsed);

		constexpr size_t max_raw_bytes = 256 * 1024;
		std::string raw(std::bit_cast<const char*>(scan.xmp.data()), std::min(scan.xmp.size(), max_raw_bytes));
		info.metadata.emplace_back(metadata_standard::xmp, std::move(kv), scan.xmp.size(), parsed, std::move(raw));
	}

	if (!scan.structure.empty())
	{
		auto rows = scan.structure;
		info.metadata.emplace_back(metadata_standard::structure, to_metadata_kv(rows));
	}
}

void describe_media_streams(const libfiles::media_scan_result& scan, av_media_info& info)
{
	info.streams.clear();

	for (size_t i = 0; i < scan.streams.size(); ++i)
	{
		const auto& s = scan.streams[i];
		av_stream_info row;
		row.index = static_cast<int>(i);
		row.type = to_stream_type(s.kind);
		describe_stream(s, row);

		for (const auto& tag : s.tags)
		{
			row.metadata.emplace_back(tag_key(tag), tag.value);
		}

		info.streams.emplace_back(std::move(row));
	}

	const auto* const v = first_stream(scan, libfiles::media_stream_kind::video);
	const auto* const a = first_stream(scan, libfiles::media_stream_kind::audio);
	info.has_video = v != nullptr;
	info.has_audio = a != nullptr;
	info.has_multiple_audio_streams = std::ranges::count(scan.streams, libfiles::media_stream_kind::audio,
	                                                     &libfiles::media_stream::kind) > 1;
	info.bitrate = static_cast<int64_t>(scan.bit_rate);
	info.end = scan.duration;

	if (v)
	{
		info.video_codec = core::cache(v->codec);
		info.pixel_format = core::cache(v->video.pixel_format());
		info.video_frame_rate = v->video.frame_rate;
		info.render_dimensions = {static_cast<int>(v->video.width), static_cast<int>(v->video.height)};
		info.display_dimensions = {static_cast<int>(v->video.width), static_cast<int>(display_height(v->video))};
		info.display_orientation = orientation_of(v->video);
	}

	if (a)
	{
		info.audio_codec = core::cache(a->codec);
		info.audio_sample_rate = static_cast<int>(a->audio.sample_rate);
		info.audio_channels = static_cast<int>(a->audio.channels);
		info.audio_sample_type = coded_sample_type(a->audio.bits_per_sample, a->audio.float_samples);
	}
}

void overlay_media_scan(const libfiles::media_scan_result& scan, av_media_info& info,
                        const std::vector<uint64_t>& stream_ids)
{
	if (!scan.success) return;

	// The rows stay FFmpeg's, in its order and with its indexes, since the track menu switches streams by
	// them; libfiles describes each row it can match. A row is matched by the container's stream number
	// where FFmpeg's demuxer records one, and otherwise - Matroska's leaves it zero - by its place among
	// the streams of its kind, which both read in the container's order.
	std::map<av_stream_type, int> ordinal;

	for (size_t i = 0; i < info.streams.size(); ++i)
	{
		auto& row = info.streams[i];
		const auto kind_index = ordinal[row.type]++;
		const auto id = i < stream_ids.size() ? stream_ids[i] : 0;
		const libfiles::media_stream* match = nullptr;

		if (id != 0)
		{
			const auto found = std::ranges::find_if(scan.streams, [&](const libfiles::media_stream& s)
			{ return s.id == id && to_stream_type(s.kind) == row.type; });
			if (found != scan.streams.end()) match = &*found;
		}

		if (!match)
		{
			auto k = 0;

			for (const auto& s : scan.streams)
			{
				if (to_stream_type(s.kind) != row.type) continue;
				if (k++ == kind_index)
				{
					match = &s;
					break;
				}
			}
		}

		if (match) describe_stream(*match, row);
	}

	if (const auto* const v = first_stream(scan, libfiles::media_stream_kind::video))
	{
		if (!v->codec.empty()) info.video_codec = core::cache(v->codec);
		if (const auto name = v->video.pixel_format(); !name.empty()) info.pixel_format = core::cache(name);
	}

	if (const auto* const a = first_stream(scan, libfiles::media_stream_kind::audio))
	{
		if (!a->codec.empty()) info.audio_codec = core::cache(a->codec);
		info.audio_sample_type = coded_sample_type(a->audio.bits_per_sample, a->audio.float_samples);
	}

	// The container's own tags, its packet and its structure replace what FFmpeg's demuxer made of them.
	std::erase_if(info.metadata, [](const metadata_block& b)
	{
		return b.standard == metadata_standard::ffmpeg || b.standard == metadata_standard::media ||
		       b.standard == metadata_standard::xmp || b.standard == metadata_standard::structure;
	});

	add_media_blocks(scan, info, true);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The .nfo files media managers - Kodi, Jellyfin, Emby - keep beside films and episodes: finding the ones
// that describe a video, reading their title, show, season, episode, year, dates, plot, genres and
// studio, and setting those over a container's tags. Read on the scanning thread, and only when the user
// asks.

namespace
{
	// An .nfo is a page of text at most; a file far larger is something else under the extension.
	constexpr uint64_t max_nfo_bytes = 1024 * 1024;

	struct nfo_element
	{
		std::string_view name;
		std::string text;
	};

	std::string element_text(std::string_view content)
	{
		content = core::trim(content);

		constexpr std::string_view cdata_open = "<![CDATA[";
		constexpr std::string_view cdata_close = "]]>";

		if (content.starts_with(cdata_open) && content.ends_with(cdata_close))
		{
			return std::string(
				content.substr(cdata_open.size(), content.size() - cdata_open.size() - cdata_close.size()));
		}

		return str::decode_xml_entities(content);
	}

	// The root's name and the text of each element directly inside it, in order. An element holding others - an
	// actor's name and role, a set of ratings - is passed over whole. Only the first root is read: a file holding
	// two episodes describes the first.
	std::pair<std::string_view, std::vector<nfo_element>> read_nfo_elements(const std::string_view text)
	{
		std::string_view root;
		std::vector<nfo_element> children;
		std::string_view child;
		auto child_holds_elements = false;
		size_t content_start = 0;
		auto depth = 0;
		size_t i = 0;

		while (i < text.size())
		{
			const auto open = text.find('<', i);
			if (open == std::string_view::npos) break;

			const auto skip_to = [&](const std::string_view end_marker, const size_t from)
			{
				const auto end = text.find(end_marker, from);
				i = end == std::string_view::npos ? text.size() : end + end_marker.size();
			};

			if (text.substr(open, 9) == "<![CDATA[")
			{
				skip_to("]]>", open + 9);
				continue;
			}

			if (text.substr(open, 4) == "<!--")
			{
				skip_to("-->", open + 4);
				continue;
			}

			const auto close = text.find('>', open + 1);
			if (close == std::string_view::npos) break;

			const auto tag = text.substr(open + 1, close - open - 1);
			i = close + 1;

			if (tag.empty() || tag.front() == '?' || tag.front() == '!') continue;

			if (tag.front() == '/')
			{
				--depth;

				if (depth == 1 && !child.empty())
				{
					if (!child_holds_elements)
						children.push_back({child, element_text(text.substr(content_start, open - content_start))});
					child = {};
				}

				if (depth <= 0) break;
				continue;
			}

			const auto self_closing = tag.back() == '/';
			const auto name = tag.substr(0, std::min(tag.find_first_of(" \t\r\n/"), tag.size()));
			if (name.empty()) continue;

			if (depth == 0)
			{
				root = name;
				if (self_closing) break;
				depth = 1;
			}
			else if (depth == 1)
			{
				if (self_closing) continue;
				child = name;
				child_holds_elements = false;
				content_start = i;
				depth = 2;
			}
			else
			{
				child_holds_elements = true;
				if (!self_closing) ++depth;
			}
		}

		return {root, std::move(children)};
	}

	int to_int(const std::string_view s, const int fallback)
	{
		const auto trimmed = core::trim(s);
		auto value = fallback;
		const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
		return ec == std::errc{} ? value : fallback;
	}

	// 2019-05-01, as every media manager writes a date; or the year alone.
	core::date_t nfo_date(const std::string_view s)
	{
		const auto parts = core::split(core::trim(s), false, [](const char c) { return c == '-' || c == '/'; });
		if (parts.empty()) return {};

		const auto year = to_int(parts[0], 0);
		const auto month = parts.size() > 1 ? to_int(parts[1], 0) : 0;
		const auto day = parts.size() > 2 ? to_int(parts[2], 0) : 0;

		if (year <= 1800 || year >= 2100) return {};
		if (month < 1 || month > 12 || day < 1 || day > 31) return core::date_t(year, 1, 1, 0, 0, 0);

		const core::date_t result(year, month, day, 0, 0, 0);
		return result.is_valid() ? result : core::date_t(year, 1, 1, 0, 0, 0);
	}

	std::string read_nfo_text(const core::file_path path)
	{
		const auto f = platform::open_file(path, platform::file_open_mode::read);
		if (!f) return {};

		const auto len = f->size();
		if (len == 0 || len > max_nfo_bytes) return {};

		std::string text(static_cast<size_t>(len), '\0');
		if (f->read(std::bit_cast<uint8_t*>(text.data()), len) != len) return {};

		if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
		// Media managers write UTF-8; an older one, or an .nfo edited by hand, may be in the system's code page.
		return subtitle_text_as_utf8(text);
	}

	nfo_details read_nfo_file(const core::file_path path)
	{
		if (path.is_empty() || !platform::exists(path)) return {};
		return parse_nfo(read_nfo_text(path));
	}

	// The show a season of episodes belongs to, from the tvshow.nfo in its folder or the one above. Remembered
	// briefly, so a season scanned episode by episode reads it once.
	std::string show_title(const core::folder_path folder)
	{
		static core::mutex cache_mutex;
		static core::folder_path cached_folder;
		static std::string cached_title;
		static int64_t cached_ms = 0;
		constexpr int64_t cache_ms = 60 * 1000;

		const auto now = core::now_ms();

		{
			core::exclusive_lock lock(cache_mutex);
			if (cached_folder == folder && now - cached_ms < cache_ms) return cached_title;
		}

		std::string title;

		for (const auto candidate : {folder, folder.parent()})
		{
			if (candidate.is_empty()) continue;

			const auto show = read_nfo_file(candidate.combine_file("tvshow.nfo"));

			if (show.type == nfo_details::kind::show && !show.title.empty())
			{
				title = show.title;
				break;
			}
		}

		core::exclusive_lock lock(cache_mutex);
		cached_folder = folder;
		cached_title = title;
		cached_ms = now;
		return title;
	}
}

bool is_named_for_its_folder(const std::string_view stem, const std::string_view folder)
{
	const auto film = scan_info_from_title(stem);
	const auto named = scan_info_from_title(folder);
	if (film.title.empty() || core::icmp(film.title, named.title) != 0) return false;
	return film.year == 0 || named.year == 0 || film.year == named.year;
}

nfo_details parse_nfo(const std::string_view text)
{
	nfo_details result;
	const auto [root, elements] = read_nfo_elements(text);

	if (core::icmp(root, "movie") == 0) result.type = nfo_details::kind::film;
	else if (core::icmp(root, "episodedetails") == 0) result.type = nfo_details::kind::episode;
	else if (core::icmp(root, "tvshow") == 0) result.type = nfo_details::kind::show;
	else if (core::icmp(root, "musicvideo") == 0) result.type = nfo_details::kind::music_video;
	else return result;

	std::vector<std::string> genres;
	std::string outline;

	for (const auto& [name, value] : elements)
	{
		if (value.empty()) continue;

		if (core::icmp(name, "title") == 0 && result.title.empty()) result.title = value;
		else if (core::icmp(name, "showtitle") == 0 && result.show.empty()) result.show = value;
		else if (core::icmp(name, "plot") == 0 && result.plot.empty()) result.plot = value;
		else if (core::icmp(name, "outline") == 0 && outline.empty()) outline = value;
		else if (core::icmp(name, "genre") == 0) genres.emplace_back(value);
		else if (core::icmp(name, "studio") == 0 && result.studio.empty()) result.studio = value;
		else if (core::icmp(name, "year") == 0) result.year = std::clamp(to_int(value, 0), 0, 9999);
		else if (core::icmp(name, "season") == 0) result.season = to_int(value, -1);
		else if (core::icmp(name, "episode") == 0) result.episode = to_int(value, -1);
		else if ((core::icmp(name, "premiered") == 0 || core::icmp(name, "aired") == 0) && !result.released.is_valid())
			result.released = nfo_date(value);
	}

	if (result.plot.empty()) result.plot = std::move(outline);
	if (result.year <= 1800 || result.year >= 2100)
		result.year = result.released.is_valid() ? result.released.year() : 0;
	if (result.season < 0 || result.season > 9999) result.season = -1;
	// Episodes are kept in 16 bits.
	if (result.episode < 0 || result.episode > 32767) result.episode = -1;

	// A show's .nfo names the show as its title.
	if (result.type == nfo_details::kind::show) result.show = result.title;

	result.genre = core::combine(genres, ", ", false);
	return result;
}

nfo_details read_video_nfo(const core::file_path video)
{
	const auto folder = video.folder();
	const auto stem = video.file_name_without_extension();

	auto result = read_nfo_file(folder.combine_file(std::string(stem) + ".nfo"));

	if (result.is_empty() && is_named_for_its_folder(stem, folder.name()))
	{
		result = read_nfo_file(folder.combine_file("movie.nfo"));
		if (result.type != nfo_details::kind::film) result = {};
	}

	// An episode's own .nfo may leave its show to the show's; an episode with none takes its show from there too.
	const auto is_episode = result.type == nfo_details::kind::episode ||
	                        (result.is_empty() && scan_info_from_title(stem).has_episode);

	if (is_episode && result.show.empty())
	{
		auto show = show_title(folder);

		if (!show.empty())
		{
			if (result.is_empty()) result.type = nfo_details::kind::episode;
			result.show = std::move(show);
		}
	}

	return result;
}

void apply_nfo(prop::item_metadata& md, const nfo_details& nfo)
{
	if (nfo.is_empty() || nfo.type == nfo_details::kind::show) return;

	if (!nfo.title.empty()) md.title = core::strip_and_cache(nfo.title);
	if (!nfo.plot.empty()) md.synopsis = core::strip_and_cache(nfo.plot);
	if (!nfo.genre.empty()) md.genre = core::strip_and_cache(nfo.genre);
	if (!nfo.studio.empty()) md.publisher = core::strip_and_cache(nfo.studio);
	if (nfo.year != 0) md.year = static_cast<uint16_t>(nfo.year);

	if (nfo.type == nfo_details::kind::episode)
	{
		if (!nfo.show.empty()) md.show = core::strip_and_cache(nfo.show);
		if (nfo.season >= 0) md.season = static_cast<uint16_t>(nfo.season);
		if (nfo.episode > 0) md.episode = prop::number_pair(nfo.episode, 0);
	}

	// A music video keeps its year but adds no date: that date is what marks a film the .nfo described.
	if (nfo.type == nfo_details::kind::music_video) return;

	if (nfo.released.is_valid()) md.dates.add(prop::date_source::media_manager, nfo.released);
	else if (nfo.year != 0) md.dates.add(prop::date_source::media_manager, core::date_t(nfo.year, 1, 1, 0, 0, 0));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// JPEG encoding and lossless turns through libfiles' codecs, held to Diffractor's surfaces, settings,
// decode budget, log and errors; and the signatures of the metadata a JPEG's markers carry. libfiles also
// decodes JPEGs and reads their structure.

bool is_exif_signature(const std::span<const uint8_t> block)
{
	return block.size() >= exif_signature.size() &&
	       memcmp(exif_signature.data(), block.data(), exif_signature.size()) == 0;
}

static constexpr size_t default_photoshop_resource_cap = 16u * 1024u * 1024u;
static std::atomic<size_t> s_photoshop_resource_cap{default_photoshop_resource_cap};

void files_test_hooks::set_jpeg_photoshop_resource_cap(const size_t bytes)
{
	s_photoshop_resource_cap.store(bytes == 0 ? default_photoshop_resource_cap : bytes, std::memory_order_relaxed);
}

size_t files_test_hooks::jpeg_photoshop_resource_cap()
{
	return s_photoshop_resource_cap.load(std::memory_order_relaxed);
}

namespace
{
	// What libfiles noted - a source whose tables could not be adopted, a Photoshop resource too large for
	// any marker - goes to the log.
	void log_notes(const libfiles::jpeg_encoder& encoder)
	{
		for (const auto& note : encoder.notes()) core::log("jpeg_encoder", note);
	}

	[[noreturn]] void fail(const std::string_view where, const libfiles::encode_error& e)
	{
		core::log(where, e.what());
		throw app_exception(e.what());
	}
}

core::blob turn_jpeg_losslessly(const libfiles::jpeg_transformer& transformer, libfiles::jpeg_encoder& encoder,
                                const std::span<const uint8_t> src, const simple_transform transform,
                                bool* const over_budget)
{
	if (over_budget) *over_budget = false;

	try
	{
		// simple_transform counts from none at 0, where the EXIF orientation libfiles takes counts from 1.
		const auto orientation = static_cast<uint16_t>(static_cast<int>(transform) + 1);
		const auto budget = static_cast<uint64_t>(std::max<int64_t>(render::max_decode_bytes, 0));
		auto turned = transformer.turn(src, encoder, orientation, budget);

		if (turned.over_budget)
		{
			core::log(__FUNCTION__,
			          std::format("rotate of {} x {} needs {}, over the {} budget", turned.width, turned.height,
			                      platform::format_size(core::file_size(turned.workspace_bytes)),
			                      platform::format_size(core::file_size(render::max_decode_bytes))));
			if (over_budget) *over_budget = true;
		}

		return core::blob(std::move(turned.data));
	}
	catch (const libfiles::encode_error& e)
	{
		fail(__FUNCTION__, e);
	}
}

// The edits that move a photo's pixels, at its gain map's scale.
static image_edits gain_map_edits(const image_edits& edits, const sizei photo, const sizei gain_map)
{
	image_edits result;

	if (edits.has_crop_bounds())
	{
		auto crop = edits.crop_bounds();

		// Multiplied first, so a corner on the photo's edge lands exactly on the gain map's.
		for (auto i = 0; i < 4; ++i)
		{
			crop[i] = pointd(crop[i].X * gain_map.cx / photo.cx, crop[i].Y * gain_map.cy / photo.cy);
		}

		result.crop_bounds(crop);
	}

	if (edits.has_scale())
	{
		const auto limit = edits.scale();
		result.scale(sizei(std::max(1, core::round(static_cast<double>(limit.cx) * gain_map.cx / photo.cx)),
		                   std::max(1, core::round(static_cast<double>(limit.cy) * gain_map.cy / photo.cy))));
	}

	result.perspective_horizontal(edits.perspective_horizontal());
	result.perspective_vertical(edits.perspective_vertical());
	return result;
}

// A gain map encoded as a JPEG holds one: at a quality of 95 - small beside the photo, and every step of its loss is a
// step in the HDR rendition's brightness - one channel where it is grey, and against the tables of the JPEG it was,
// where it was one they can be taken from. Bare: what makes it a gain map is its caller's to add.
std::vector<uint8_t> files::encode_gain_map(const core::const_surface_ptr& map, const std::span<const uint8_t> tables)
{
	const auto edited = core::to_8_bit(map);
	if (is_empty(edited) || !core::is_packed(edited->format())) return {};

	const auto extent = edited->dimensions();
	const auto width = static_cast<size_t>(extent.cx);

	// A gain map of one channel stays one channel.
	auto grey = true;

	for (auto y = 0; grey && y < extent.cy; ++y)
	{
		const auto* const line = edited->pixels_line(y);

		for (size_t x = 0; x < width; ++x)
		{
			if (line[x * 4] != line[x * 4 + 1] || line[x * 4 + 1] != line[x * 4 + 2])
			{
				grey = false;
				break;
			}
		}
	}

	libfiles::jpeg_options options;
	options.quality = 95;
	options.grayscale = grey;
	options.source = tables;

	std::vector<uint8_t> grey_rows;
	const auto* pixels = edited->pixels();
	auto stride = edited->stride();

	if (grey)
	{
		grey_rows.resize(width * extent.cy);

		for (auto y = 0; y < extent.cy; ++y)
		{
			const auto* const line = edited->pixels_line(y);
			for (size_t x = 0; x < width; ++x) grey_rows[y * width + x] = line[x * 4 + 1];
		}

		pixels = grey_rows.data();
		stride = width;
	}

	auto encoded = _jpeg_encoder.encode(extent.cx, extent.cy, pixels, stride, 1, {}, options);
	log_notes(_jpeg_encoder);
	return encoded;
}

std::vector<uint8_t> files::edit_gain_map(const std::span<const uint8_t> gain_map, const image_edits& edits,
                                          const sizei photo_extent, const simple_transform turn)
{
	if (turn == simple_transform::none && !edits.has_crop(photo_extent) && !edits.has_rotation() &&
	    !edits.has_scale() && !edits.has_perspective())
	{
		return {gain_map.begin(), gain_map.end()};
	}

	if (turn != simple_transform::none)
	{
		auto turned = turn_jpeg_losslessly(_jpeg_transformer, _jpeg_encoder, gain_map, turn);
		if (!turned.empty()) return {turned.begin(), turned.end()};
	}

	// Otherwise the edits that move the photo's pixels, at the gain map's scale: a turn its blocks refuse
	// losslessly is decoded and turned as a photo re-encoded is.
	const auto source = image_to_surface(core::cspan(gain_map.data(), gain_map.size()));
	if (is_empty(source)) return {};

	const auto edited = apply_image_edits(source, gain_map_edits(edits, photo_extent, source->dimensions()));
	if (is_empty(edited)) return {};

	const auto encoded = encode_gain_map(edited, gain_map);
	if (encoded.empty()) return {};

	// Its metadata - Adobe's XMP, ISO 21496-1's segment - is what makes it a gain map.
	const auto extent = edited->dimensions();
	return libfiles::carry_jpeg_metadata(gain_map, encoded, extent.cx, extent.cy);
}

core::blob files::with_edited_gain_map(const std::span<const uint8_t> edited, const std::span<const uint8_t> source,
                                       const image_edits& edits, const sizei photo_extent, const simple_transform turn)
{
	std::vector<uint8_t> gain_map;
	const auto found = libfiles::find_jpeg_gain_map(source);

	if (!found.empty())
	{
		try
		{
			gain_map = edit_gain_map(found, edits, photo_extent, turn);
		}
		catch (const std::exception& e)
		{
			core::log(__FUNCTION__, e.what());
		}

		if (gain_map.empty()) core::log(__FUNCTION__, "the gain map could not be edited, and is left behind");
	}

	auto attached = libfiles::attach_jpeg_gain_map(edited, gain_map);
	if (attached.empty()) return core::blob(edited.begin(), edited.end());
	return core::blob(std::move(attached));
}

core::blob files::with_gain_map(const std::span<const uint8_t> primary, const gain_map_surface& gain_map)
{
	std::vector<uint8_t> marked;

	try
	{
		const auto encoded = encode_gain_map(gain_map.pixels, {});
		if (!encoded.empty()) marked = libfiles::mark_jpeg_gain_map(encoded, gain_map.metadata);
	}
	catch (const std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	auto attached = marked.empty() ? std::vector<uint8_t>{} : libfiles::attach_jpeg_gain_map(primary, marked);

	if (attached.empty())
	{
		core::log(__FUNCTION__, "the gain map could not be written, and is left behind");
		return core::blob(primary.begin(), primary.end());
	}

	return core::blob(std::move(attached));
}

core::surface_ptr sdr_rendition(const core::const_surface_ptr& signal, const libfiles::hdr_description& hdr,
                                const bool wide)
{
	if (!is_valid(signal) || !core::is_wide(signal->format())) return {};

	const auto alpha = core::has_alpha(signal->format());
	const auto format = wide ? signal->format() : alpha ? core::texture_format::ARGB : core::texture_format::RGB;
	auto out = std::make_shared<core::surface>();
	if (!out->alloc(signal->dimensions(), format, signal->orientation())) return {};

	const libfiles::image_view view{signal->pixels(), signal->stride(), signal->width(), signal->height(),
	                                alpha ? libfiles::pixel_format::rgba16 : libfiles::pixel_format::rgbx16};
	const auto to = wide ? (alpha ? libfiles::pixel_format::rgba16 : libfiles::pixel_format::rgbx16)
	                     : (alpha ? libfiles::pixel_format::bgra8 : libfiles::pixel_format::bgrx8);
	if (!libfiles::map_hdr_to_sdr(view, hdr, to, {out->pixels(), out->stride()})) return {};
	return out;
}

std::optional<libfiles::image_view> libfiles_view(const core::surface& s)
{
	libfiles::pixel_format format;

	switch (s.format())
	{
	case core::texture_format::RGB: format = libfiles::pixel_format::bgrx8; break;
	case core::texture_format::ARGB: format = libfiles::pixel_format::bgra8; break;
	case core::texture_format::RGBX16: format = libfiles::pixel_format::rgbx16; break;
	case core::texture_format::RGBA16: format = libfiles::pixel_format::rgba16; break;
	default: return {};
	}

	return libfiles::image_view{s.pixels(), s.stride(), s.width(), s.height(), format};
}

core::const_surface_ptr profile_rendition(const core::const_surface_ptr& pixels, const std::span<const uint8_t> profile,
                                          const bool wide)
{
	if (!is_valid(pixels) || profile.empty()) return pixels;

	const auto view = libfiles_view(*pixels);
	if (!view) return pixels;

	auto out = std::make_shared<core::surface>();
	if (!out->alloc(pixels->dimensions(), pixels->format(), pixels->orientation())) return {};
	if (!libfiles::map_icc_to_srgb(*view, profile, {out->pixels(), out->stride()})) return pixels;
	if (!wide && core::is_wide(out->format())) return core::to_8_bit(out);
	return out;
}

std::optional<gain_map_surface> load_jpeg_gain_map(const std::span<const uint8_t> jpeg)
{
	const auto found = libfiles::find_jpeg_gain_map(jpeg);
	if (found.empty()) return {};

	files ff;
	auto pixels = ff.image_to_surface(core::cspan(found.data(), found.size()));
	if (is_empty(pixels) || !core::is_packed(pixels->format())) return {};

	if (const auto metadata = libfiles::read_jpeg_gain_map_metadata(found))
		return gain_map_surface{std::move(pixels), *metadata};

	// Apple's says how it applies otherwise: its values are rewritten in ISO 21496-1's terms, by the headroom its
	// own XMP or the primary's maker note gives.
	libfiles::reader map_reader(found);
	libfiles::reader photo_reader(jpeg);
	const auto map_scan = libfiles::scan(map_reader);
	const auto photo_scan = libfiles::scan(photo_reader);
	const auto headroom = libfiles::apple_gain_map_headroom(map_scan.metadata.xmp, photo_scan.metadata.exif);
	if (!headroom) return {};

	const auto metadata = libfiles::convert_apple_gain_map({pixels->pixels(), pixels->stride()}, pixels->width(),
	                                                       pixels->height(), *headroom);
	return gain_map_surface{std::move(pixels), metadata};
}

std::optional<gain_map_surface> edited_gain_map(const gain_map_surface& gain_map, const image_edits& edits,
                                                const sizei photo_extent)
{
	if (!is_valid(gain_map.pixels) || photo_extent.is_empty()) return {};

	// A gain map stands as its photo does, at a scale of its own. One whose shape says it stands otherwise would
	// brighten the wrong parts of the photo, and is left behind.
	const auto map = gain_map.pixels->dimensions();
	const auto across = static_cast<double>(map.cx) / photo_extent.cx;
	const auto down = static_cast<double>(map.cy) / photo_extent.cy;
	if (std::abs(across - down) > 0.02 * std::max(across, down) + 1.0 / std::min(map.cx, map.cy))
	{
		core::log(__FUNCTION__, "the gain map does not stand as its photo does, and is left behind");
		return {};
	}

	auto edited = apply_image_edits(gain_map.pixels, gain_map_edits(edits, photo_extent, map));
	if (is_empty(edited)) return {};
	return gain_map_surface{std::move(edited), gain_map.metadata};
}

// JPEG and WebP hold a byte a channel: a wide picture is rounded to it here, once, after everything an edit
// did to it. Anything else is encoded as it is.
static core::const_surface_ptr narrowed(const core::const_surface_ptr& surface)
{
	return is_valid(surface) && core::is_wide(surface->format()) ? core::to_8_bit(surface) : surface;
}

core::image_ptr save_jpeg(const core::const_surface_ptr& surface, const libfiles::metadata_blocks& metadata,
                          const file_encode_params& encoder_params, libfiles::jpeg_encoder* const encoder)
{
	const auto surface_in = narrowed(surface);
	if (!is_valid(surface_in)) return {};

	const auto dimensions_in = surface_in->dimensions();
	const auto orientation_in = surface_in->orientation();

	libfiles::jpeg_options options;
	options.quality = encoder_params.jpeg_save_quality;
	options.source = encoder_params.jpeg_source;

	std::optional<libfiles::jpeg_encoder> own;
	auto& e = encoder ? *encoder : own.emplace();

	try
	{
		auto data = e.encode(dimensions_in.cx, dimensions_in.cy, surface_in->pixels(), surface_in->stride(),
		                     static_cast<uint16_t>(orientation_in), metadata, options);
		log_notes(e);

		return std::make_shared<core::image>(core::blob(std::move(data)),
		                                     sizei{static_cast<int>(e.width()), static_cast<int>(e.height())},
		                                     core::image_format::JPEG, orientation_in);
	}
	catch (const libfiles::encode_error& ex)
	{
		log_notes(e);
		fail(__FUNCTION__, ex);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// PNG files: decoded into a surface and encoded by libfiles, with their ICC profile, EXIF and XMP.

core::image_ptr save_png(const core::const_surface_ptr& surface_in, const libfiles::metadata_blocks& metadata,
                         const std::optional<libfiles::hdr_description>& hdr)
{
	if (!is_valid(surface_in)) return {};

	const auto dims = surface_in->dimensions();

	libfiles::image_view view;
	view.pixels = surface_in->pixels();
	view.stride = surface_in->stride();
	view.width = static_cast<uint32_t>(dims.cx);
	view.height = static_cast<uint32_t>(dims.cy);
	// Sixteen bits a channel stay sixteen. Anything but RGB is written with its fourth channel as alpha.
	switch (surface_in->format())
	{
	case core::texture_format::RGB: view.format = libfiles::pixel_format::bgrx8; break;
	case core::texture_format::RGBX16: view.format = libfiles::pixel_format::rgbx16; break;
	case core::texture_format::RGBA16: view.format = libfiles::pixel_format::rgba16; break;
	default: view.format = libfiles::pixel_format::bgra8; break;
	}

	libfiles::embedded_metadata embedded(metadata);
	embedded.hdr = hdr;

	try
	{
		auto encoded = libfiles::encode_png(view, static_cast<uint16_t>(surface_in->orientation()), embedded);

		// libpng's warnings go to the quiet debug log rather than the console and the test output.
		for (const auto& note : encoded.notes) core::log_once("libpng", note);

		return std::make_shared<core::image>(core::blob(std::move(encoded.data)), dims, core::image_format::PNG,
		                                     surface_in->orientation());
	}
	catch (const libfiles::encode_error& e)
	{
		core::log_once(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
}

namespace
{
	// A surface as the memory libfiles decodes a PNG into: straight-alpha BGRA, or - for a caller that takes
	// them - a 16-bit PNG's sixteen bits a channel, red first, its fourth channel alpha or 65535. Its size is
	// judged against the decode budget, at the bytes a pixel of that takes, before any memory is taken, as
	// the loader always judged it.
	class png_surface_sink final : public libfiles::image_sink
	{
	public:
		core::surface_ptr surface;
		bool over_budget = false;
		// Refused at sixteen bits a channel, where a byte a channel may yet fit.
		bool over_budget_wide = false;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};
			const auto format = frame.format == libfiles::pixel_format::rgba16   ? core::texture_format::RGBA16
			                    : frame.format == libfiles::pixel_format::rgbx16 ? core::texture_format::RGBX16
			                                                                     : core::texture_format::ARGB;

			// The cspan decode path carries no budget gate of its own, and IHDR dimensions cost nothing to
			// declare: a small, highly compressible PNG can otherwise ask for gigabytes.
			if (reject_over_budget_source(nullptr, dimensions, "PNG", core::bytes_per_pixel(format)))
			{
				over_budget = true;
				over_budget_wide = core::is_wide(format);
				return {};
			}

			surface = std::make_shared<core::surface>();
			auto* const pixels = surface->alloc(dimensions, format);
			if (!pixels) return {};
			return {pixels, surface->stride()};
		}
	};
}

core::surface_ptr load_png(const core::cspan data, const bool wide, picture_signal* const edit, const bool map_to_srgb)
{
	static constexpr uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

	if (data.size < 8 || !std::equal(std::begin(signature), std::end(signature), data.data))
	{
		throw app_exception("load_png invalid png header"s);
	}

	libfiles::reader r(std::span(data.data, data.size));
	png_surface_sink sink;
	libfiles::decode_options options;
	options.allow_16bit = wide;
	options.keep_hdr = wide && edit;
	options.map_to_srgb = map_to_srgb;
	const auto result = libfiles::decode(r, sink, options);

	// libpng's warnings - an iTXt with a damaged CRC, say - go to the quiet debug log rather than the
	// console, where they are still there for diagnosis.
	for (const auto& warning : result.warnings) core::log_once("libpng", warning);

	if (sink.over_budget)
	{
		if (!sink.over_budget_wide) return {};
		// Too large to edit at sixteen bits a channel or in HDR: at a byte a channel, HDR shown as SDR.
		picture_signal narrowed;
		auto narrow = load_png(data, false, edit ? &narrowed : nullptr, map_to_srgb);

		if (edit)
		{
			edit->narrowed = true;
			edit->tone_mapped = narrowed.tone_mapped;
		}

		return narrow;
	}

	if (!result.error.empty())
	{
		if (!result.truncated) core::log_once("load_png", result.error);
		throw app_exception(result.error);
	}

	if (edit)
	{
		edit->tone_mapped = result.frame.tone_mapped;
		if (!result.frame.tone_mapped) edit->hdr = result.frame.hdr;
	}

	// A surface that could not be allocated is returned empty, as it always was.
	if (sink.surface) sink.surface->orientation(static_cast<core::orientation>(result.frame.orientation));
	return sink.surface;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// WebP files: still images decoded into a surface and encoded by libfiles, with their ICC profile, EXIF
// and XMP.

void files_test_hooks::fail_next_webp_chunk(const std::string_view fourcc)
{
	libfiles::test_hooks::fail_next_webp_chunk(fourcc);
}

namespace
{
	// A surface as the memory libfiles decodes a WebP into. Its size is judged against the decode budget
	// before any memory is taken: the source's, because that is what a hostile file declares, and
	// libwebp's 16383-pixel edge still permits a ~1 GB surface.
	class webp_surface_sink final : public libfiles::image_sink
	{
	public:
		core::surface_ptr surface;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			const sizei source{static_cast<int>(frame.source_width), static_cast<int>(frame.source_height)};
			if (reject_over_budget_source(nullptr, source, "WEBP")) return {};

			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};
			auto s = std::make_shared<core::surface>();
			uint8_t* pixels = nullptr;

			if (frame.format == libfiles::pixel_format::nv12)
			{
				// libfiles crops an odd edge, as decode_jpeg does, so an odd-sized image still reaches the
				// GPU as NV12: a 320x213 thumbnail would otherwise cost 4 bytes a pixel rather than 1.5.
				pixels = s->alloc(dimensions, core::texture_format::NV12);
				s->color_space(frame.limited_range ? color_space::rec601_limited : color_space::rec601_full);
			}
			else
			{
				// Opaque images decode into the ignored X byte, so tag them RGB and let the renderer skip
				// blending.
				pixels = s->alloc(dimensions, frame.format == libfiles::pixel_format::bgra8
				                                  ? core::texture_format::ARGB
				                                  : core::texture_format::RGB);
			}

			if (!pixels) return {};
			surface = std::move(s);
			return {pixels, surface->stride()};
		}
	};
}

// libwebp rescales inside the decoder, so a reduced decode comes back at the size the caller asked for
// rather than at the file's size with a downscale still owed: the size core::scale_dimensions gives, which
// files::fit_within uses, so it is a drop-in for decode-then-scale. NV12 is the caller's choice, and a
// reduced decode is packed regardless: NV12 exists to hand the renderer native planes.
core::surface_ptr load_webp(const core::cspan data, const bool can_use_yuv, const sizei target_extent,
                            const bool map_to_srgb)
{
	libfiles::reader r(std::span(data.data, data.size));
	webp_surface_sink sink;

	libfiles::decode_options options;
	options.target_width = static_cast<uint32_t>(std::max(0, target_extent.cx));
	options.target_height = static_cast<uint32_t>(std::max(0, target_extent.cy));
	options.allow_nv12 = can_use_yuv;
	options.map_to_srgb = map_to_srgb;

	const auto result = libfiles::decode(r, sink, options);
	if (!result.success) return {};

	sink.surface->orientation(static_cast<core::orientation>(result.frame.orientation));
	return sink.surface;
}

core::image_ptr save_webp(const core::const_surface_ptr& surface, const libfiles::metadata_blocks& metadata,
                          const file_encode_params& params)
{
	const auto surface_in = narrowed(surface);
	if (!is_valid(surface_in)) return {};

	const auto dimensions = surface_in->dimensions();
	const auto cs = surface_in->color_space();

	libfiles::image_view view;
	view.pixels = surface_in->pixels();
	view.stride = surface_in->stride();
	view.width = static_cast<uint32_t>(dimensions.cx);
	view.height = static_cast<uint32_t>(dimensions.cy);

	switch (surface_in->format())
	{
	case core::texture_format::RGB: view.format = libfiles::pixel_format::bgrx8; break;
	case core::texture_format::ARGB: view.format = libfiles::pixel_format::bgra8; break;
	case core::texture_format::NV12:
		// Only the matrix VP8 assumes: a rec709 or rec2020 frame would be encoded as if it were 601.
		if (cs != core::color_space::rec601_full && cs != core::color_space::rec601_limited) return {};
		view.format = libfiles::pixel_format::nv12;
		view.limited_range = cs == core::color_space::rec601_limited;
		break;
	default: return {};
	}

	libfiles::webp_options options;
	options.lossless = params.webp_lossless;
	options.quality = params.webp_quality;
	options.lossy_alpha = params.webp_lossy_alpha;
	options.fast = params.webp_fast;

	auto encoded = libfiles::encode_webp(view, static_cast<uint16_t>(surface_in->orientation()), metadata, options);
	for (const auto& note : encoded.notes) core::log(__FUNCTION__, note);
	if (encoded.data.empty()) return {};

	return std::make_shared<core::image>(core::blob(std::move(encoded.data)), dimensions, core::image_format::WEBP,
	                                     surface_in->orientation());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Adobe Photoshop (PSD) files: the composite image, decoded by libfiles into a surface.

namespace
{
	// A surface as the memory libfiles decodes into, so the image lands in it with nothing copied. Its
	// size is judged against the decode budget before any memory is taken, as the loaders judge it.
	class surface_sink final : public libfiles::image_sink
	{
		load_diagnostic* const _diagnostic;
		const std::string_view _format;

	public:
		core::surface_ptr surface;

		surface_sink(load_diagnostic* const diagnostic, const std::string_view format) :
			_diagnostic(diagnostic), _format(format)
		{
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			if (frame.format != libfiles::pixel_format::bgrx8) return {};

			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};
			if (reject_over_budget_source(_diagnostic, dimensions, _format)) return {};

			auto s = std::make_shared<core::surface>();
			if (!s->alloc(dimensions, core::texture_format::RGB)) return {};
			surface = std::move(s);
			return {surface->pixels(), surface->stride()};
		}
	};
}

core::surface_ptr load_psd(read_stream& s, load_diagnostic* const diagnostic, const bool map_to_srgb)
{
	read_stream_source source(s);
	libfiles::reader r(source);
	surface_sink sink(diagnostic, "PSD");
	libfiles::decode_options options;
	options.map_to_srgb = map_to_srgb;
	const auto result = libfiles::decode(r, sink, options);

	// A file cut short throws, as this loader always has, so a damaged PSD is not offered to ffmpeg.
	if (result.truncated) throw app_exception(result.error);
	return result.success ? std::move(sink.surface) : core::surface_ptr{};
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Commodore 64 disk, tape, cartridge and program images: the listing libfiles makes of one, drawn by
// libfiles into a surface for the disk control's texture. And SID tunes, whose header lib64 reads.

namespace
{
	// "1987 System 3" and "1987-88 Ocean": a release field usually leads with its year, and the publisher
	// follows. Anything else - "198? Unknown", "(C) Hewson" - is all publisher.
	std::pair<int, std::string_view> split_sid_release(const std::string_view released)
	{
		if (released.size() >= 4 && core::is_num(released.substr(0, 4)))
		{
			const auto year = core::to_int(released.substr(0, 4));

			if (year >= 1980 && year < 2100)
			{
				auto rest = released.substr(4);
				const auto range_end = rest.find_first_not_of("-0123456789");
				rest = range_end == std::string_view::npos ? std::string_view{} : rest.substr(range_end);
				return {year, core::trim(rest)};
			}
		}

		return {0, core::trim(released)};
	}

	std::string sid_address(const uint16_t address)
	{
		return std::format("${:04X}", address);
	}
}

core::const_surface_ptr files::c64_listing_surface(const libfiles::c64_listing& listing, const uint32_t fg,
                                                   const uint32_t bg)
{
	if (listing.rows.empty() || listing.columns == 0) return {};

	auto surface = std::make_shared<core::surface>();
	auto* const pixels = surface->alloc(static_cast<int>(listing.width()), static_cast<int>(listing.height()),
	                                    core::texture_format::RGB);
	if (!pixels) return {};

	// RGB textures upload as B8G8R8X8, the order libfiles draws in, given its colours as 0xRRGGBB. A
	// listing is a whole number of eight-pixel cells wide, so its rows fill their stride, and no padding
	// goes to the GPU unwritten.
	libfiles::draw_c64_listing(listing, {pixels, surface->stride()}, core::bgr(fg), core::bgr(bg));
	return surface;
}

file_scan_result files::scan_c64_media(const core::cspan data)
{
	file_scan_result result;
	if (data.empty()) return result;

	const auto listing = libfiles::list_c64_media({data.data, data.size});
	if (listing.rows.empty() && listing.title.empty()) return result;

	// Each file by its name as the listing prints it, between its quotes. A cartridge, or a program file holding the
	// one program its own name already says, adds nothing to its title.
	std::string contents;

	for (const auto& row : listing.rows)
	{
		// A cartridge's row is its title again, as ASCII the row's PETSCII text cannot spell.
		if (row.file_type == 0x99) continue;

		const auto open = row.text.find('"');
		const auto close = row.text.rfind('"');
		if (open == std::string::npos || close <= open) continue;

		const auto name = core::trim(std::string_view(row.text).substr(open + 1, close - open - 1));
		if (name.empty() || (listing.rows.size() == 1 && core::icmp(name, listing.title) == 0)) continue;

		if (!contents.empty()) contents += '\n';
		contents += name;
	}

	if (!listing.title.empty()) result.title = core::cache(listing.title);
	if (!contents.empty()) result.contents = core::cache(contents);
	// As a SID tune's, so one search for the machine finds its tunes and its software together.
	result.ffmpeg_metadata.emplace_back("system", "Commodore 64");
	result.success = true;
	return result;
}

file_scan_result files::scan_sid(const core::cspan data)
{
	file_scan_result result;
	sid_tune_info info;
	if (data.empty() || !machine::parse_sid(data.data, data.size, info)) return result;

	const auto texts = read_sid_header_texts(data);
	const auto& title = texts.title;
	const auto& author = texts.author;
	const auto& released = texts.released;
	const auto [year, publisher] = split_sid_release(released);
	const auto chip = sid_chip_name(info.chip_8580);

	// Named as FFmpeg names a container's tags, so the properties are read the way every other audio
	// file's are. The release field was once the copyright field, and its year and publisher are what a
	// collection is browsed by. The track is the tune the file starts on, out of the tunes it holds.
	auto& kv = result.ffmpeg_metadata;
	if (!title.empty()) kv.emplace_back("title", title);
	if (!author.empty()) kv.emplace_back("artist", author);
	if (!released.empty()) kv.emplace_back("copyright", released);
	if (year) kv.emplace_back("year", std::to_string(year));
	if (!publisher.empty()) kv.emplace_back("publisher", std::string(publisher));
	kv.emplace_back("track", std::format("{}/{}", info.start_song, info.songs));
	kv.emplace_back("system", "Commodore 64");

	result.duration = sid_tune_seconds;
	result.audio_codec = core::cache(chip);
	result.audio_sample_rate = sid_sample_rate;
	result.audio_channels = 1;
	result.audio_sample_type = prop::audio_sample_t::signed_16bit;
	result.nb_streams = 1;

	auto& rows = result.structure_metadata;
	add_structure_section(rows, "SID tune", "sid.summary", true);
	add_structure_row(rows, "Format", std::format("{} version {}", info.rsid ? "RSID" : "PSID", info.version));
	add_structure_row(rows, "Tunes", std::format("{}, starting with {}", info.songs, info.start_song));
	add_structure_row(rows, "Chip", std::string(chip));
	add_structure_row(rows, "Clock", info.ntsc ? "NTSC" : "PAL");
	add_structure_row(rows, "Load address", sid_address(info.load_address));
	add_structure_row(rows, "Init address", sid_address(info.init_address));
	add_structure_row(rows, "Play address",
	                  info.play_address ? sid_address(info.play_address) : "set by the tune's own interrupt");
	finish_structure_sections(rows);

	result.success = true;
	return result;
}
