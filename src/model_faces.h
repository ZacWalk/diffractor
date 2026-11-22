// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Faces - the face regions a picture's metadata names, packed as the index keeps them, and face
// detection: the faces in a decoded picture as boxes and landmarks in the upright space the picture is shown
// in, in a stable order. Owned by docs/faces.md. Nothing here reads a file, touches the database or needs a
// window; libmug finds the faces and makes their vectors, and model_faces.cpp hands it the decoded pictures.

#pragma once

#include "libmug/detect.h"
#include "libmug/embed.h"

namespace faces
{
	// A face region as a file's metadata keeps it: whom it names, if anyone, and where it is on the picture
	// as stored - fractions of its width and height from its top left - where the file says.
	struct stored_region
	{
		std::string name;
		std::optional<rectd> area;
	};

	// A face region as the index keeps it: whom it names, if anyone, and where it is as fractions of the
	// upright picture, empty where the file places it nowhere or its place is no longer known.
	struct named_region
	{
		std::string name;
		rectd area;

		bool has_area() const { return area.Width > 0.0 && area.Height > 0.0; }
		bool operator==(const named_region&) const = default;
	};

	// The space a packet's areas were measured in, by the shape of the picture mwg-rs:AppliedToDimensions names
	// - `applied_to`, empty where it names none - against the stored picture's: docs/faces.md#22-where-a-region-is.
	enum class region_space
	{
		stored,
		upright,
		// The picture has been cropped since: its areas no longer say where anything is.
		stale,
	};

	region_space space_of_regions(sizei applied_to, sizei stored, core::orientation orientation);

	// A file's face regions turned upright: docs/faces.md#22-where-a-region-is. `applied_to` is the
	// picture mwg-rs:AppliedToDimensions says the areas were measured on, empty where it says none, and
	// `stored` the picture's own dimensions as the file stores them.
	std::vector<named_region> upright_regions(std::span<const stored_region> regions, sizei applied_to, sizei stored,
	                                          core::orientation orientation);

	// A rectangle of fractions of a picture as stored, turned as the picture is shown, and back.
	rectd to_upright(const rectd& stored, core::orientation orientation);
	rectd to_stored(const rectd& upright, core::orientation orientation);

	// The regions as one interned value, as an item's metadata holds them: as the file states them - each area as the
	// writer measured it, with the dimensions it says it measured them on - so how a region's space is judged is
	// decided whenever they are read, and a correction to that judgment needs no picture read again. Unpacking turns
	// them upright against the picture's stored dimensions and orientation, which the item's metadata holds beside
	// them; a packing from before regions were kept as stated is already upright, and is read as it is. A value that
	// is not a packing this build reads unpacks to none.
	core::cached pack_regions(std::span<const stored_region> regions, sizei applied_to);
	std::vector<named_region> unpack_regions(core::cached packed, sizei stored, core::orientation orientation);

	constexpr int landmark_count = 5;

	// Both eye centres, the nose tip, then both mouth corners, in that order: the detector's.
	using landmarks = std::array<pointd, landmark_count>;

	// Kept together so measurement has one place to correct. See docs/faces.md.
	struct tuning
	{
		// The detector reports confidence as an integer percentage. Detections below this are dropped.
		int detector_confidence = 70;
		// The long edge the detector's own input is reduced to. Detection cost is linear in pixels,
		// and the smallest face it can still find grows as this shrinks.
		int detector_edge = 640;
		// Smallest face reported by the first pass, as a fraction of the picture's short edge.
		double min_face_edge_fraction = 0.03;

		// The long edge of the decode a face's aligned crop is sampled from (docs/faces.md#42-alignment).
		int decode_edge = 1600;
		// A picture keeps at most this many faces, the largest.
		int max_faces = 64;
		// A crop sampling more than this fraction outside the picture is refused rather than embedded.
		double max_outside = 0.25;

		// A good face (docs/faces.md#44-quality): eyes at least this far apart in the decode, square-on
		// at least this much, and found at least this confidently.
		double good_eye_distance = 28.0;
		double good_frontal = 0.6;
		int good_confidence = 80;

		// docs/faces.md#53-comparing-faces. `similar` starts at SFace's published operating point for one
		// person, and `confident` is the bar above it.
		float similar = 0.363f;
		float confident = 0.50f;
		// How much more a face must resemble the person it is offered to than anyone else: young children resemble
		// each other more than a threshold can separate, while each still resembles their own faces most.
		float margin = 0.05f;
		// Maximum positive (and separately negative) exemplars per person: docs/faces.md#72-suggestions.
		int exemplars = 64;
		// docs/faces.md#33-videos: faces from two of a video's frames are one person's where they resemble each
		// other at least `video_same`, or `video_nearby` where they stand in one place a frame read apart.
		float video_same = 0.6f;
		float video_nearby = 0.45f;
	};

	const tuning& defaults();

	// One face, in the upright space the picture is shown in: its box, its landmarks and the
	// detector's confidence as a percentage. A box can reach past the picture's edge where the face
	// does.
	struct face
	{
		rectd bounds;
		landmarks marks{};
		int confidence = 0;
	};

	// The faces in one picture.
	struct face_set
	{
		// The upright dimensions the faces are measured in: those of the surface detection ran on,
		// turned as the picture is shown. A bounded decode makes these smaller than the file's own,
		// so a caller scales by them rather than assuming the file's.
		sizei extent;
		// In libmug's stable order: left to right, then top to bottom, then larger first.
		std::vector<face> faces;
	};

	// The faces in one decoded picture - packed 32-bit or NV12, as the decode ladder produces it - found by
	// libmug, upright. Any other picture finds none. One detector serves one thread at a time.
	face_set find_faces(libmug::face_detector& detector, const core::const_surface_ptr& surface,
	                    const tuning& t = defaults());

	// The dimensions a picture of these stored dimensions is shown at.
	sizei upright_extent(sizei extent, core::orientation orientation);

	// ---- Embedding (docs/faces.md#42-alignment, #43-embedding) -----------------------------------------

	constexpr int vector_size = libmug::vector_size;

	// What SFace makes of a face: 128 values, normalised, so a comparison is a dot product.
	using face_vector = libmug::face_vector;

	// The vector of the face whose upright landmarks are `marks` in a decoded picture - packed 32-bit or NV12 - as
	// libmug makes it: fitted to SFace's template, sampled and embedded. False where it makes none: the crop lies
	// more than t.max_outside past the picture, or the network gives nothing finite. One embedder serves one thread
	// at a time.
	bool embed_face(libmug::face_embedder& embedder, const core::const_surface_ptr& decoded, const landmarks& marks,
	                face_vector& result, const tuning& t = defaults());

	// A square of the upright picture as the Faces view shows a face, `edge` pixels a side, cut from a packed
	// decode: a box widened to take in the head, kept inside the picture, or the middle of the whole picture where the
	// box is empty. None for a decode that is not packed.
	core::surface_ptr cut_tile(const core::const_surface_ptr& decoded, const rectd& upright_box, int edge);

	// One number for everything a face hash depends on: libmug's model id - its networks, and how it finds, aligns
	// and samples a face - and how Diffractor decodes the picture libmug reads. Two face hashes are compared only
	// when they carry the same one.
	uint32_t model_id();

	// ---- Quality (docs/faces.md#44-quality) -------------------------------------------------------------

	// How far apart the eyes are, in the decode's pixels, and how square-on the face is, from 0 to 1: how evenly
	// the nose sits between the eyes, and how level they are.
	struct face_quality
	{
		double eye_distance = 0.0;
		double frontal = 0.0;
	};

	face_quality measure_quality(const landmarks& marks);
	bool is_good(const face_quality& quality, int confidence, const tuning& t = defaults());

	// ---- Face hashes (docs/faces.md#51-the-face-record) -------------------------------------------------

	// A normalised vector quantized to 128 signed bytes and one scale.
	struct face_hash
	{
		std::array<int8_t, vector_size> values{};
		float scale = 0.0f;

		bool is_empty() const { return scale <= 0.0f; }
		bool operator==(const face_hash&) const = default;
	};

	face_hash quantize(const face_vector& v);
	// The dot product of two face hashes' values, unscaled.
	int32_t dot_values(const int8_t* a, const int8_t* b);
	// The cosine between two faces: the dot product of their face hashes, scaled.
	float similarity(const face_hash& a, const face_hash& b);
	float similarity(const face_vector& a, const face_vector& b);

	// ---- The face record (docs/faces.md#51-the-face-record) ---------------------------------------------

	// A stretch of a video, in milliseconds on the timeline playback uses.
	struct time_span
	{
		uint32_t start = 0;
		uint32_t end = 0;

		bool operator==(const time_span&) const = default;
	};

	enum class record_status : uint8_t
	{
		read = 0,
		// The file opened but did not decode, so it is not read again until it changes.
		undecodable = 1,
	};

	// Persisted bits: retire one by leaving it unused, never by reusing it.
	namespace face_flag
	{
		// Large and square-on enough to carry an identity: docs/faces.md#44-quality.
		constexpr uint8_t good = 1 << 0;
		// Put away by the user before the face book kept marks: honoured where no mark says otherwise
		// (docs/faces.md#62-marks).
		constexpr uint8_t hidden = 1 << 1;
		// Found by looking again within a named region the detector's first look missed.
		constexpr uint8_t in_region = 1 << 2;
	}

	// One face a picture's record keeps, its box and landmarks as fractions of the upright picture.
	struct found_face
	{
		rectd box;
		landmarks marks{};
		uint8_t confidence = 0;
		// How square-on, 0 to 255.
		uint8_t frontal = 0;
		// The distance between its eyes, in the pixels of the decode it was measured on.
		uint16_t eye_distance = 0;
		uint8_t flags = 0;
		// Empty where the face could not be embedded.
		face_hash hash;
		// The people it was dismissed for before the face book kept marks: carried, so a record written back loses
		// nothing, and no longer read.
		std::vector<std::string> dismissed;
		// In a video, the frame it was found in, in milliseconds on the timeline playback uses; 0 in a picture.
		uint32_t time_ms = 0;
		// In a video, the parts its person was seen in: docs/faces.md#33-videos. Empty in a picture.
		std::vector<time_span> seen;

		bool is_good() const { return (flags & face_flag::good) != 0; }
		bool is_hidden() const { return (flags & face_flag::hidden) != 0; }
		bool operator==(const found_face&) const = default;
	};

	// Bumped when detection adds faces without changing how previous faces are measured.
	constexpr uint8_t face_detection_revision = 1;

	struct face_record
	{
		uint32_t model = 0;
		// The file's modified time when it was read.
		core::date_t stamp;
		// The upright dimensions its faces were measured on.
		sizei extent;
		record_status status = record_status::read;
		std::vector<found_face> faces;
		uint8_t detection_revision = face_detection_revision;
		// Read from a video's frames rather than from a picture: docs/faces.md#33-videos.
		bool video = false;

		bool operator==(const face_record&) const = default;
	};

	std::vector<uint8_t> pack_record(const face_record& record);
	// None for a record this build does not read: malformed, or in a later format than it knows.
	std::optional<face_record> unpack_record(std::span<const uint8_t> data);

	// A picture's record turned with it, where a write turned its declared orientation alone: the boxes and
	// landmarks move, and the face hashes, which unchanged pixels still describe, stay.
	face_record turn_record(face_record record, core::orientation from, core::orientation to);

	// A picture read again with its pixels unchanged - by a new model, or a corrected way of finding faces - keeps
	// the marks the user put on its faces: each face found again where a marked face was takes its hidden mark and
	// its dismissals, so a correction released later never costs the user what they told Diffractor.
	void carry_marks(const face_record& from, face_record& to);

	// ---- Reading a picture (docs/faces.md#4-finding-a-face) -------------------------------------------

	// How much of a face's box a named region covers, where they are one face: docs/faces.md#45-named-regions-and-found-faces.
	// Zero for two that are not.
	double region_overlap(const rectd& face_box, const rectd& region);

	// Each named region's found face, by index into faces, or -1: each matched once, the largest overlap first.
	std::vector<int> match_regions(std::span<const named_region> regions, std::span<const found_face> faces);

	// The faces in one bounded decode - found, measured and embedded - in the stable order, then any recovered
	// from named regions or a second detection pass. The stamp is the caller's.
	face_record read_record(libmug::face_detector& detector, libmug::face_embedder& embedder,
	                        const core::const_surface_ptr& decoded, std::span<const named_region> regions,
	                        const tuning& t = defaults());

	// ---- Reading a video (docs/faces.md#33-videos) --------------------------------------------------

	// A video is read at the first key frame in each of this many equal parts of its length: a short clip at every key
	// frame, a long one at no more than this.
	constexpr int video_slices = 256;

	// Reads a video's key frames by `read(from)`, which reads the first key frame at or after `from` seconds and answers
	// its time, or nothing past the last: the first in each of `slices` equal parts of `length`, or, where the length
	// is not known, each in turn up to `slices` of them. Answers how many were read.
	template <typename Read> int read_video_slices(const double length, Read&& read, const int slices = video_slices)
	{
		// Rounding between time bases can place a key frame a hair before the time it was sought by.
		constexpr auto after = 0.002;
		auto taken = 0;
		auto slice = 0;
		auto from = 0.0;

		while (taken < slices && slice < slices)
		{
			if (length > 0.0) from = std::max(from, length * slice / slices);
			const std::optional<double> time = read(from);
			if (!time) break;
			++taken;
			from = std::max(from, *time + after);
			slice = length > 0.0 ? std::max(slice, static_cast<int>(std::floor(*time / length * slices))) + 1
			                     : slice + 1;
		}

		return taken;
	}

	// One of a video's frames read for faces: when it is shown, and what read_record found in it.
	struct video_frame
	{
		double time = 0.0;
		face_record found;
	};

	// docs/faces.md#33-videos: a video's frames made one record - each person once, by the clearest face of theirs,
	// with the parts of the video, `length` seconds long, they were seen in. Two faces in one frame are never one
	// person's, and a face with no face hash to compare is left out. Undecodable where no frame decoded.
	face_record merge_video_frames(std::vector<video_frame> frames, double length, const tuning& t = defaults());
}
