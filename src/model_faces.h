// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Face detection - finding the faces in a decoded picture, as boxes and landmarks in the
// upright space the picture is shown in, in a stable order. Owned by docs/faces.md. Nothing here
// reads a file, touches the database or needs a window; the network itself is in
// model_faces_detect.cpp, behind face_detector.

#pragma once

namespace faces
{
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
		// Smallest face reported, as a fraction of the picture's short edge.
		double min_face_edge_fraction = 0.03;
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
		// In the stable order order_faces puts them in.
		std::vector<face> faces;
	};

	// The detector. One holds the scratch buffers its calls reuse, so it serves one thread at a
	// time; the weights every detector reads are shared and immutable.
	class face_detector
	{
	public:
		virtual ~face_detector() = default;

		// Takes a decoded surface as the decode ladder produces it - packed 32-bit, or NV12 - and
		// answers its faces upright, in the detector's own order. Anything else finds no faces.
		virtual std::vector<face> detect(const core::const_surface_ptr& surface, const tuning& t) = 0;
	};

	using face_detector_ptr = std::unique_ptr<face_detector>;

	face_detector_ptr create_face_detector();

	// The faces in one decoded picture, upright and in stable order.
	face_set find_faces(face_detector& detector, const core::const_surface_ptr& surface, const tuning& t = defaults());

	// The stable order: left to right, then top to bottom, then larger first. The detector's own
	// order follows its scores, so a picture read again at another size could list the same faces
	// differently.
	void order_faces(std::vector<face>& faces);

	// The picture's luminance at `destination`, as one 8-bit plane, full range whatever range the
	// decode carried. Packed and NV12 surfaces of one picture reduce to the same plane. Never enlarges.
	bool reduce_to_luma(const core::const_surface_ptr& surface, sizei destination, core::surface_ptr& scratch,
	                    std::vector<uint8_t>& result);

	// The dimensions a picture of these stored dimensions is shown at.
	sizei upright_extent(sizei extent, core::orientation orientation);

	// One 8-bit plane in stored orientation, turned as the picture is shown. The network is trained on
	// upright faces, and most formats decode in the file's own orientation, so a portrait photograph
	// arrives on its side.
	bool orient_plane(std::span<const uint8_t> plane, sizei extent, core::orientation orientation,
	                  std::vector<uint8_t>& result);
}
