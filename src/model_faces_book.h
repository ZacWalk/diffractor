// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Named people and the user's per-face decisions, independent of resemblance or the scan cache.
// Held in memory, written through to the face database and backed up as CSV. Owned by docs/faces.md#6-the-face-book.

#pragma once

#include <bit>
#include <compare>
#include <deque>
#include <map>
#include <mutex>

#include "model_faces.h"

namespace faces
{
	// Exact portable identity, never a similarity comparison. A different model describes a different space.
	struct face_signature
	{
		uint32_t model = 0;
		face_hash hash;

		bool valid() const
		{
			const auto bits = std::bit_cast<uint32_t>(hash.scale);
			return model && bits > 0 && bits < 0x7f800000u; // Positive and finite, including subnormals.
		}
		auto operator<=>(const face_signature& other) const
		{
			if (auto c = model <=> other.model; c != 0) return c;
			if (auto c = hash.values <=> other.hash.values; c != 0) return c;
			return std::bit_cast<uint32_t>(hash.scale) <=> std::bit_cast<uint32_t>(other.hash.scale);
		}
		bool operator==(const face_signature& other) const { return (*this <=> other) == 0; }
	};

	// One face the book has a mark for, found again by where it is on its picture.
	struct face_mark
	{
		// Fractions of the upright picture: the found face's box, or a named region's area.
		rectd box;
		// Its place in its picture's stable order, which decides between boxes that overlap alike.
		int position = 0;
		uint32_t person = 0; // Confirmed person, or zero for unknown.
		bool hidden = false;
		std::vector<uint32_t> rejected;            // Sorted, unique person IDs. Never contains person.
		std::optional<face_signature> signature{}; // Only on a picture mark, linking it to its portable decision.

		bool operator==(const face_mark&) const = default;
	};

	struct kept_person
	{
		uint32_t id = 0;
		std::string name;

		bool operator==(const kept_person&) const = default;
	};

	struct roster
	{
		struct name_less : core::iless
		{
			using is_transparent = void;
		};

		std::vector<kept_person> people; // Ordered by stable ID.
		// IDs are stable across edits, but not across loading a different book.
		uint64_t loaded_at = 0;
		std::map<std::string, uint32_t, name_less> aliases; // Former names, each pointing directly to a kept ID.

		const kept_person* person(uint32_t id) const;
		// Resolves a former name to the kept spelling; every other name is returned unchanged.
		std::string_view canonical_name(std::string_view name) const;
		// The person kept under a name (including a former name), without regard to case.
		const kept_person* named(std::string_view name) const;
	};

	using roster_ptr = std::shared_ptr<const roster>;
	using signature_marks = std::map<face_signature, face_mark>;

	// A face the user acts on: its picture, and where it is on it.
	struct face_ref
	{
		core::file_path path;
		rectd box;
		int position = 0;
		std::optional<face_signature> signature{};
	};

	// What one change wrote: the rows it touched as they now stand, for the face database.
	struct book_write
	{
		// The book's generation once the change was made, so a write of the whole book can tell which changes it
		// already holds. Zero for a write made outside the book.
		uint64_t generation = 0;
		// Only an explicit request to forget the book clears it, and a write of the whole book, which replaces what
		// the file held.
		bool clear = false;
		std::vector<kept_person> people;
		std::vector<uint32_t> people_removed;
		std::vector<std::pair<std::string, uint32_t>> aliases; // Set the name's owner, or remove it for ID zero.
		// Each picture's marks, all of them, as they now stand; none removes the picture's.
		std::vector<std::pair<core::file_path, std::vector<face_mark>>> pictures;
		std::map<face_signature, std::optional<face_mark>> signatures; // nullopt removes the key (Undo).

		bool empty() const
		{
			return !clear && people.empty() && people_removed.empty() && aliases.empty() && pictures.empty() &&
			       signatures.empty();
		}
	};

	using marks_by_picture = core::hash_map<core::file_path, std::vector<face_mark>, core::ihash, core::ieq>;

	struct book_edit
	{
		uint64_t generation = 0;
		roster_ptr before;
		roster_ptr after;
		book_write reverse;
	};

	enum class face_action
	{
		assign,
		reject,
		hide,
		unhide,
		unassign
	};

	struct placement_batch
	{
		// For assignment/rejection, either an existing ID or a name to resolve or create.
		uint32_t person = 0;
		std::string name;
		std::vector<face_ref> faces;
	};

	struct placed_faces : book_edit
	{
		std::vector<uint32_t> people;
	};

	// What reading a CSV file into the book did.
	struct import_result
	{
		bool ok = false;
		std::string error;
		size_t people_added = 0;
		size_t people_joined = 0;
		size_t faces = 0;
	};

	// The face book. Changed on the UI thread, and as Diffractor moves and changes pictures, and read on any: each change
	// replaces the roster whole and a picture's marks under a lock held only to copy or replace them, then hands what it
	// wrote to the face database.
	class face_book final : core::no_copy
	{
		mutable core::mutex _rw;
		_Guarded_by_(_rw) roster_ptr _roster;
		_Guarded_by_(_rw) marks_by_picture _marks;
		_Guarded_by_(_rw) signature_marks _signatures;
		_Guarded_by_(_rw) core::hash_map<core::file_path, size_t, core::ihash, core::ieq> _unsigned_by_picture;
		_Guarded_by_(_rw) size_t _unportable = 0;
		_Guarded_by_(_rw) uint64_t _next_id = 1;
		std::atomic<uint64_t> _generation = 1;
		// docs/faces.md#64-the-face-database: set while another Diffractor window writes the face database, when this
		// book only shows what that window wrote and refuses every change.
		std::atomic_bool _read_only = false;
		std::function<void(book_write)> _persist;
		_Guarded_by_(_rw) std::deque<book_write> _pending;
		_Guarded_by_(_rw) bool _publishing = false;

		void publish(std::unique_lock<core::mutex>& lock, book_write write);
		uint32_t take_id();
		void count_unsigned(core::file_path path, std::span<const face_mark> marks);

	public:
		face_book();

		// Where each change is written. Unset, the book is kept in memory alone, as a test's is.
		void persist_to(std::function<void(book_write)> persist) { _persist = std::move(persist); }

		// Fills the book from the face database, replacing what it held, and writes nothing.
		void load(roster people, marks_by_picture marks, signature_marks signatures = {});
		// Read-only, every change is refused and nothing is written; load still fills the book.
		void set_read_only(const bool read_only) { _read_only = read_only; }
		bool is_read_only() const { return _read_only.load(); }
		// The whole book as one write that replaces what the face database held, tagged with the generation it stands
		// at: what a failed write is followed by.
		book_write whole() const;

		// Moves with every change, so work begun before one can tell it is stale.
		uint64_t generation() const { return _generation.load(std::memory_order_acquire); }
		roster_ptr people() const;
		std::vector<face_mark> marks(core::file_path path) const;
		marks_by_picture all_marks() const;
		signature_marks all_signatures() const;
		std::optional<face_mark> signature_mark(const face_signature& key) const;
		size_t unportable_count() const;
		size_t mark_count() const;
		// Pair each matched legacy mark, exactly as stored, with its signed scanned ref. Never guesses on conflict.
		bool bind_signatures(std::span<const std::pair<face_ref, face_mark>> bindings, uint64_t expected_generation);

		uint32_t create(std::string_view name);
		// docs/faces.md#61-named-people: `keep_former_name` keeps the old name as a former name, as Combine does, so
		// the pictures that still name them by it go on listing them as one person.
		bool rename(uint32_t person, std::string_view name, bool keep_former_name = false);
		// Either name may be known only from files. The caller verifies both belonged to the selected overview.
		std::optional<book_edit> combine(std::string_view from, std::string_view into, uint64_t expected_generation);
		std::optional<placed_faces> place(std::span<const placement_batch> batches, face_action action,
		                                  uint64_t expected_generation);
		bool can_undo(const book_edit& edit) const;
		bool undo(const book_edit& edit);
		// docs/faces.md#62-marks: pictures renamed or moved take their marks with them, all moved together, so two that
		// swapped names keep their own; a picture moved onto another's path takes its place.
		void moved(std::span<const std::pair<core::file_path, core::file_path>> files);
		// Folders renamed or moved: the marks of every picture in each, at any depth, go with it.
		void moved(std::span<const std::pair<core::folder_path, core::folder_path>> folders);
		// A picture turned by its declared orientation alone, its pixels unchanged: its marks turn with it.
		void turned(core::file_path path, core::orientation from, core::orientation to);
		// A picture read again after its pixels changed - turned, cropped or straightened - by the model that read it
		// before: each mark goes to the face found again that resembles its own as only that face can. One whose face is
		// not found so stays, unless the edit moved the faces and another face is where it was: then it goes.
		void follow(core::file_path path, std::span<const found_face> before, std::span<const found_face> after,
		            uint32_t model = 0);
		// Forgets everything the user told it.
		void clear();

		// docs/faces.md#65-saving-and-moving-the-face-book: the book as one CSV file, and one read back into it,
		// joining what it already holds.
		std::string to_csv(size_t* unportable = nullptr) const;
		import_result import_csv(std::string_view csv);
	};

	// The mark among `marks` for the face at `box`, its place in the stable order `position`: the one that overlaps it
	// most, as a named region is matched with a face.
	const face_mark* find_mark(std::span<const face_mark> marks, const rectd& box, int position);

	// Each mark's face among a picture's faces, by index into `boxes` - the faces in their stable order - or -1: each
	// matched once, the largest overlap first, as find_mark finds one.
	std::vector<int> match_marks(std::span<const face_mark> marks, std::span<const rectd> boxes);
}
