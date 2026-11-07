// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Face input reduction, alignment, vectors, resemblance groups, and the sample face that
// identifies one. Owned by docs/faces.md. Nothing here reads a file,
// touches the database, or needs a window; the detector and the embedding model live behind
// face_engine so that three of the four stages stay testable on their own.

#pragma once

#include "libdesktop/ui.h"
#include "libdesktop/util_geometry.h"

namespace df
{
	struct face_set;
}

namespace faces
{
	// The joint version of the detector, the reference template and the embedding model. A vector's
	// meaning is the product of all three, so changing any one of them invalidates every stored
	// vector and this number is what makes that detectable rather than silent. A row stamped with
	// anything else is treated as unscanned. Version 2 also moved stored geometry into upright space.
	// Version 4 uses full-range luminance for both packed and planar detector input. Version 5 stores
	// a file's faces in a stable order, which is what the `:n` in a face term counts.
#ifdef DIFFRACTOR_FACE_MODEL_HASH
	constexpr uint32_t model_version = 5u ^ DIFFRACTOR_FACE_MODEL_HASH;
#else
	constexpr uint32_t model_version = 5;
#endif

	constexpr int vector_size = 128;
	constexpr int aligned_extent = 112;
	constexpr int landmark_count = 5;

	using face_vector = std::array<float, vector_size>;

	// Both eye centres, the nose tip, then both mouth corners, in that order. The order is the
	// detector's and the reference template's at once, so it is not free to change.
	using landmarks = std::array<pointd, landmark_count>;

	// Kept together so measurement has one place to correct and no caller can quietly invent a
	// second opinion. See docs/faces.md.
	struct tuning
	{
		// libfacedetection reports confidence as an integer percentage.
		int detector_confidence = 70;
		// The long edge the detector's own input is reduced to. Detection cost is linear in pixels
		// and the smallest face it can still find scales with this, so it trades directly against
		// finding face_groups in group photographs.
		int detector_edge = 640;
		// Smallest admissible face, as a fraction of the frame's short edge. A face fourteen pixels
		// across carries no identity and costs an embedding to learn that.
		double min_face_edge_fraction = 0.03;
		// A resemblance threshold, not an identity decision. Search buckets may overlap.
		float similar = 0.55f;
		// How square-on a face has to be before it is fit to stand for its group. A profile makes a
		// poor tile and a worse reference, so a group passes over one entirely while any member is
		// facing the camera. See faces::frontality.
		double min_sample_frontality = 0.5;
		// An alignment that would sample this much outside the source is refused rather than padded.
		// A half-face at the frame edge embeds to something confidently similar to other half-faces
		// at frame edges - a group that is real, stable, and about nothing.
		double max_outside_fraction = 0.25;
	};

	const tuning& defaults();
	bool reduce_to_luma(const ui::const_surface_ptr& surface, sizei destination,
	                    ui::surface_ptr& scratch, std::vector<uint8_t>& result);

	// The five-point reference template for a 112x112 crop, in that crop's own coordinates. This is
	// Diffractor's constant and it must match the one the embedding model was trained against;
	// changing it changes the meaning of every stored vector, so it is versioned with the model.
	const landmarks& reference_template();

	// Least-squares fit of five landmarks onto the reference template, as a SIMILARITY transform:
	// rotation, uniform scale and translation only. Not a full affine - a general affine has the
	// freedom to shear a face into the template, which destroys exactly the geometry the embedding
	// reads. Least squares over all five pairs, so one mis-placed landmark does not decide the result.
	affined align_transform(const landmarks& marks);

	// True when the aligned crop would sample substantially outside a source of this size.
	bool alignment_is_clipped(const affined& transform, sizei source_extent, const tuning& t);

	// L2 normalisation, applied once when a vector is produced so that every later comparison is a
	// dot product. A zero vector is left alone rather than dividing by zero.
	void normalise(face_vector& v);

	// Cosine similarity, which for normalised vectors is the dot product, in [-1, 1].
	float similarity(const face_vector& a, const face_vector& b);

	// The mean of a set of vectors, re-normalised for automatic anchor refinement.
	face_vector mean_vector(std::span<const face_vector> vectors);

	// How square-on the landmarks are, in [0, 1]. Built from how evenly the nose sits between the
	// eyes and how level the eye line is, which is all five points can say about pose.
	double frontality(const landmarks& marks);

	// Stored geometry is upright: the space the picture is shown in, not the grid it was decoded on.
	// The detector runs on the decode, which for most formats still carries the file's rotation, so a
	// portrait photograph detects on a landscape surface. Storing what was measured would put every
	// box on its side the moment anything drew one, and would make the assignment locator mean a
	// different part of the picture depending on the format the picture arrived in.
	//
	// This is the inverse of edit_view_state::crop_from_displayed_rect, and the same space regions
	// the user draws are already kept in.
	sizei upright_extent(sizei stored_extent, ui::orientation orientation);
	pointd to_upright(pointd p, sizei stored_extent, ui::orientation orientation);
	rectd to_upright(rectd r, sizei stored_extent, ui::orientation orientation);

	struct detection;

	// The deterministic score that orders faces: detector confidence, then crop size, then pose. It
	// decides which face leads a group and which face a palette tile shows, so it must depend only on
	// the face itself.
	double candidate_rank(const detection& d, sizei source_extent);

	// One face in one file: the file, and the face's position in that file's stable face order. The
	// order is deterministic - see order_faces - so this pair means the same face in the next
	// session, which is what lets it be written down as a search term.
	struct face_ref
	{
		str::cached path;
		int index = 0;

		bool operator==(const face_ref& other) const
		{
			return index == other.index && icmp(path, other.path) == 0;
		}
	};

	struct face_ref_hash
	{
		size_t operator()(const face_ref& r) const
		{
			return crypto::hash_gen(r.path).append(static_cast<uint32_t>(r.index)).result();
		}
	};

	// How a face is written down: the file's own name and the 1-based position of the face in it,
	// with `:1` left off because most pictures hold one face and `IMG_1234.jpg` is the whole of what
	// there is to say about them. The folder is deliberately absent - a term has to be readable and
	// typeable - so two files of one name spell one term and it asks for both faces.
	std::string format_face_token(std::string_view file_name, int index);

	struct face_token
	{
		std::string name;
		// Zero-based, matching face_ref::index. The text is 1-based.
		int index = 0;
	};

	// False for text that names no face at all. A trailing `:0` or `:-1` is not a position.
	bool parse_face_token(std::string_view text, face_token& result);

	// One face offered to the grouper. Carries only what ordering and comparison need, so grouping
	// can be exercised without a detector, an embedder or a file.
	struct candidate
	{
		face_ref ref;
		std::shared_ptr<const df::face_set> source;
		face_vector vec{};
		// The face's box in the source file's own coordinates, which is what the palette scales onto
		// whatever dimensions the cached thumbnail turns out to have.
		rectd crop;
		// The dimensions `crop` is measured in. The detection ran on a bounded decode, so this is not
		// the file's own size and cannot be inferred from anything else.
		sizei crop_space;
		// Position in the deterministic order: detector confidence, crop size and frontality, with
		// the file path breaking ties so the answer never depends on enumeration order.
		double rank = 0;
		// How square-on the landmarks were, kept because the sample face is chosen on pose and detail
		// rather than on the order rank encodes, and the landmarks are gone by the time it is chosen.
		double frontality = 0;
		// False when another file in the collection carries the same name, which makes this face's
		// term ask for both. Sample selection prefers a face that spells only itself.
		bool unique_name = true;
		// Detached metadata used when a worker builds the palette projection.
		df::date_t created;
		str::cached place;
	};

	// Faces resembling one sample face. The sample is a real face in a real file, so a group is shown,
	// identified and searched for as the same thing; there is no id and no label.
	struct group
	{
		face_ref sample;
		// The sample's own vector, so the count on a tile is what a search for it returns.
		face_vector anchor{};
		std::vector<face_ref> members;
	};

	struct grouping_result
	{
		std::vector<group> groups;
		// Ordinary near-ties are placed; resemblance does not require an exclusive identity claim.
		std::vector<face_ref> ungrouped;
		bool cancelled = false;
	};

	// Anchored, not transitive. Membership is measured against a group's anchor alone, never against
	// other members, so a group's diameter is bounded by twice the threshold by construction and a
	// chain of pairwise-similar faces cannot collapse into one group. Union-find over the same
	// tolerance relation is single-linkage clustering, and single-linkage clustering chains.
	grouping_result build_groups(std::vector<candidate> candidates, const tuning& t,
	                             const std::function<bool()>& cancel = {});
	bool match_group_members(std::vector<group>& groups, std::span<const candidate> candidates,
	                         const tuning& t, const std::function<bool()>& cancel = {});

	// Resolves a member back to the candidate it came from. Supplied by the caller because the index
	// keeps a hash map for this and a test keeps a vector, and the rule below must not have two copies.
	using candidate_lookup = std::function<const candidate*(const face_ref&)>;

	// Gives every group the sample face that stands for it, re-anchors it on that face's own vector,
	// merges groups that settle on one sample and re-measures membership against it. A mean vector is
	// nobody's face: it cannot be shown, cannot be spelled, and a count taken against it would not be
	// the count a search returns. Run once, after clustering has settled.
	bool adopt_sample_faces(std::vector<group>& groups, std::span<const candidate> candidates,
	                        const candidate_lookup& lookup, const tuning& t,
	                        const std::function<bool()>& cancel = {});

	// Which face stands for a group. Deterministic over the group's membership, so the tile does not
	// reshuffle between sessions; it changes only when the membership does.
	const candidate* sample_face(const group& g, const candidate_lookup& lookup, const tuning& t);

	// How well one face would stand for its group: typical of the group, square-on, and large enough
	// not to be an upsample. Separate from candidate_rank because this may depend on the group and
	// candidate_rank may not - that one fixes the order groups are led in.
	double sample_score(const candidate& c, const face_vector& anchor);
}

namespace df
{
	// One detected face as it is stored: the box and landmarks in SOURCE coordinates, the detector's
	// own score, and the vector the aligned crop embedded to.
	struct stored_face
	{
		rectd bounds;
		faces::landmarks marks{};
		faces::face_vector vec{};
		float score = 0;
	};

	// Every face found in one file, published as one immutable value. Never mutated in place and
	// never published half-built, exactly like the picture hashes beside it. The faces are held in
	// the stable order faces::order_faces puts them in, because their positions are what a face term
	// counts.
	struct face_set
	{
		uint32_t model_version = 0;
		date_t scanned;
		// The dimensions the boxes and landmarks below are measured in. Recorded because the detection
		// ran on a bounded decode, so nothing else in the index knows what they are relative to.
		sizei source_extent;
		std::vector<stored_face> faces;

		bool is_current() const { return model_version == faces::model_version; }
		bool is_current(const date_t source_modified) const
		{
			return is_current() && scanned >= source_modified;
		}
	};

	using face_set_ptr = std::shared_ptr<const face_set>;
	using face_set_aptr = std::atomic<face_set_ptr>;

	// The wire form the database column holds. Self-describing enough that a later release can widen
	// a face record without forcing a re-detection of the whole collection.
	blob encode_faces(const face_set& fs);
	face_set decode_faces(cspan data);
}

namespace faces
{
	// What one detection stage produced, before alignment refuses anything.
	struct detection
	{
		rectd bounds;
		landmarks marks{};
		int confidence = 0;
	};

	// The stable order a file's faces are stored and counted in: upright left to right, then top to
	// bottom, then larger first, with the box breaking the remaining ties. Detector output order is
	// a function of how the model scanned the frame, so a term written against it would move to a
	// different face the next time the file was read.
	void order_faces(std::vector<df::stored_face>& faces);

	// The detector and the embedding model. Behind an interface so the three stages either side of
	// it are testable without them, and so a build without the vendored libraries simply has no
	// engine rather than a stub that answers wrongly.
	//
	// Both stages take the decoded surface as it comes off the decode ladder - packed 32-bit, BGR in
	// the low three bytes. Neither converts the whole frame: detection reduces to its own input size
	// first, and embedding warps only the 112x112 it needs.
	class face_engine
	{
	public:
		virtual ~face_engine() = default;

		// Detections below the confidence floor or the minimum size are refused here and never reach
		// an embedding.
		virtual std::vector<detection> detect(const ui::const_surface_ptr& surface, const tuning& t) = 0;

		// Warps the aligned 112x112 crop out of the same surface and embeds it. Returns false when the
		// crop could not be taken or the model could not run.
		virtual bool embed(const ui::const_surface_ptr& surface, const affined& transform, face_vector& result) = 0;
	};

	using face_engine_ptr = std::unique_ptr<face_engine>;

	// Null when this build has no detector and no model compiled in, which is the only reason face
	// search would be unavailable rather than merely switched off.
	face_engine_ptr create_face_engine();
	bool face_engine_available();

	// What the palette draws, as a detached value: built away from the UI thread and applied in one
	// batch. The crop is in the source file's own coordinates and is scaled to whatever the cached
	// thumbnail's dimensions turn out to be, so drawing never causes a decode.
	struct palette_entry
	{
		// The sample face: what the tile shows and what the group is. Everything else here describes
		// the faces that resemble it.
		face_ref sample;
		// How the sample is written down - `IMG_1234.jpg:2` - which is the term clicking this entry
		// searches for. Carried rather than recomputed so the tile, the tooltip and the query cannot
		// end up spelling it three ways.
		std::string term;
		std::shared_ptr<const df::face_set> source;
		rectd crop;
		sizei crop_space;
		int file_count = 0;
		using file_set = df::hash_set<str::cached, df::ihash, df::ieq>;
		std::shared_ptr<const file_set> files;
		// The sample's own vector, which is what both the count above and a search for `term` measure
		// against.
		face_vector anchor{};
		// When this face was first and last photographed, and the places it turns up in, most frequent
		// first. Both are what the face view lists a group by; both are empty until a member carries it.
		df::date_t first_seen;
		df::date_t last_seen;
		std::vector<std::string> places;
	};

	// Largest first: the palette is a way in to the collection, so the faces that account for most of
	// it come first. Ties break on the sample face, so two groups of one size do not swap places
	// between sessions.
	void order_palette(std::vector<palette_entry>& entries);

	// The sidebar's shortlist: the largest groups, with near-duplicate samples suppressed, capped.
	// Just the entries - how many were omitted is not reported, because nothing shows it: the groups
	// that did not fit are reached through the face view rather than through a count.
	std::vector<palette_entry> select_sidebar_palette(std::span<const palette_entry> entries);
	constexpr int max_sidebar_faces = 20;

	// How many faces one term may resolve to. A term spells a file NAME, so a collection where a
	// name repeats - every camera writes IMG_0001.jpg eventually - would otherwise let one term cost
	// a vector comparison per copy per item. Taken in path order, so which ones answer does not
	// depend on how the index enumerated.
	constexpr size_t max_term_anchors = 8;

	// Neighbourhood order, for a view that shows every group at once rather than a page of the biggest.
	// Groups that resemble each other sit together, so one face at two ages - or two people who look
	// alike - can be compared in one place instead of being found one at a time. Neighbourhoods lead
	// with their largest member, and are themselves ordered largest first, so this degrades to
	// order_palette when nothing resembles anything.
	void order_by_similarity(std::vector<palette_entry>& entries);

	// Where a stored crop lands in an image of a different size. A crop is kept in the coordinates of
	// the decode it was measured on and a cached thumbnail is whatever size it happens to be, so the
	// two never have to agree. Clamped to at least one pixel inside the image, so a caller may copy
	// the result without re-checking it.
	recti crop_in(rectd crop, sizei crop_space, sizei dims);

	// What the picture on screen says about one of its faces: what searching for it would ask for,
	// and how much of the collection that accounts for. Detached, because paint may not ask the index
	// anything. Both are empty until a pass has grouped the face.
	struct face_summary
	{
		// The term for the group this face belongs to, spelled from that group's sample rather than
		// from this face, so the results are the count beside it.
		std::string term;
		int file_count = 0;
	};

	rectd normalized_face_locator(rectd bounds, sizei source_extent);

	// What a search needs to answer its face terms: the vectors each one resolves to. A term names a
	// face in a file, so resolution reads the index once per search rather than once per item.
	// Several anchors may carry one term, because a file name two files share spells one term and
	// both faces answer it.
	struct face_anchor
	{
		std::string term;
		face_vector vec{};
	};

	struct query_context
	{
		bool enabled = false;
		std::vector<face_anchor> anchors;
	};
}
