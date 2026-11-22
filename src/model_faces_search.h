// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Face search - the face store that keeps every picture's faces in memory, the scan that reads the
// collection's pictures for faces and records them in the index, the face book of what the user told Diffractor, and
// what is made of them together: similar faces, named people, Unknown and Hidden.
// Owned by docs/faces.md.

#pragma once

#include "model_faces.h"
#include "model_faces_book.h"

class index_state;
class async_strategy;
class database;
class file_type;
class files;
using file_type_ref = const file_type*;
struct av_key_frame;
struct av_key_frame_codec;
using av_key_frame_codec_ptr = std::shared_ptr<const av_key_frame_codec>;

namespace df
{
	struct index_file_item;
}

namespace faces
{
	// One picture's faces in the store, which are consecutive: docs/faces.md#52-the-face-store.
	struct picture_run
	{
		core::file_path path;
		uint32_t first = 0;
		uint16_t count = 0;
		uint32_t model = 0;
		core::date_t stamp;
		sizei extent;
		record_status status = record_status::read;
		uint8_t detection_revision = 0;
		// Read from a video's frames: docs/faces.md#33-videos.
		bool video = false;
		// The people each face has been dismissed for, by its position in the picture.
		std::vector<std::vector<std::string>> dismissed;
		// In a video, the parts each face's person was seen in, by its position.
		std::vector<std::vector<time_span>> seen;
		// Set once a later run replaces this one, or its picture's faces are put away.
		mutable std::atomic_bool retired = false;

		bool is_current() const { return !retired.load(std::memory_order_acquire); }
	};

	// What the store holds of a face beside its face hash: its box and landmarks as fractions of the upright
	// picture, in 16 bits, and its measures.
	struct face_entry
	{
		std::array<int16_t, 4> box{};
		std::array<int16_t, landmark_count * 2> marks{};
		uint32_t run = 0;
		// In a video, the frame it was found in, in milliseconds; 0 in a picture.
		uint32_t time_ms = 0;
		uint16_t eye_distance = 0;
		uint8_t confidence = 0;
		uint8_t frontal = 0;
		uint8_t flags = 0;
		// Its place in its picture's stable order, from 0.
		uint8_t position = 0;
		bool has_hash = false;

		rectd bounds() const;
		pointd mark(int i) const;
		bool is_good() const { return (flags & face_flag::good) != 0; }
		bool is_hidden() const { return (flags & face_flag::hidden) != 0; }
	};

	// Every picture's faces, appended by one writer at a time - the load at start, then the scan - in blocks
	// that never move, so a face's number resolves without a lock and what it names never changes once the
	// counts that include it are published.
	class face_store final : core::no_copy
	{
	public:
		static constexpr uint32_t block_bits = 12;
		static constexpr uint32_t block_size = 1u << block_bits;
		static constexpr uint32_t max_blocks = 1u << 14;

	private:
		struct face_block
		{
			alignas(64) std::array<std::array<int8_t, vector_size>, block_size> values;
			std::array<float, block_size> scales;
			std::array<face_entry, block_size> entries;
		};

		struct run_block
		{
			std::array<picture_run, block_size> runs;
		};

		std::unique_ptr<std::atomic<face_block*>[]> _face_blocks;
		std::unique_ptr<std::atomic<run_block*>[]> _run_blocks;
		std::vector<std::unique_ptr<face_block>> _owned_faces;
		std::vector<std::unique_ptr<run_block>> _owned_runs;
		std::atomic<uint32_t> _face_count = 0;
		std::atomic<uint32_t> _run_count = 0;
		std::atomic<uint64_t> _generation = 1;

		mutable core::mutex _paths_rw;
		_Guarded_by_(_paths_rw) core::hash_map<core::file_path, uint32_t, core::ihash, core::ieq> _by_path;

		picture_run& run_at_writer(uint32_t r);

	public:
		face_store();
		~face_store();

		// The writer's side. A picture's new record appends a run and retires the one it replaces; false where
		// the store is full.
		bool add(core::file_path path, const face_record& record);
		void retire(core::file_path path);
		// Publishes what has been added since: readers see it from now on.
		void publish();

		// The readers' side.
		uint32_t face_count() const { return _face_count.load(std::memory_order_acquire); }
		uint32_t run_count() const { return _run_count.load(std::memory_order_acquire); }
		// Moves with every change a reader could see, so work begun before a change can tell it is stale.
		uint64_t generation() const { return _generation.load(std::memory_order_acquire); }

		const face_entry& entry(uint32_t face) const;
		const int8_t* values(uint32_t face) const;
		float scale(uint32_t face) const;
		face_hash hash(uint32_t face) const;
		face_signature signature(const uint32_t face) const { return {run_of_face(face).model, hash(face)}; }
		const picture_run& run(uint32_t r) const;
		const picture_run& run_of_face(uint32_t face) const { return run(entry(face).run); }
		// The current run of a picture, if it has one.
		std::optional<uint32_t> find(core::file_path path) const;
		// The current record of a picture, rebuilt from the store, if it has one.
		std::optional<face_record> record(core::file_path path) const;
		// A face by its picture and position.
		std::optional<uint32_t> face_at(core::file_path path, int position) const;
	};

	using face_store_ptr = std::shared_ptr<face_store>;
	using face_store_const_ptr = std::shared_ptr<const face_store>;

	// docs/faces.md#53-comparing-faces: the cosine between a face in the store and a vector.
	float similarity(const face_store& store, uint32_t face, const face_vector& v);
	float similarity(const face_store& store, uint32_t a, uint32_t b);

	struct scored_face
	{
		uint32_t face = 0;
		float score = 0.0f;
	};

	// Every current face whose similarity to `query` reaches `floor`, strongest first, the query among them.
	std::vector<scored_face> similar_faces(const face_store& store, const face_hash& query, float floor);

	// ---- Face terms (docs/faces.md#8-searching) ------------------------------------------------------

	// The store searches are answered from: the one face search has published, null while it is off.
	face_store_const_ptr active_store();
	void set_active_store(face_store_const_ptr store);

	// A face spelled as a term: a file name, without its folder, and a position in its stable order from 1.
	struct face_term
	{
		std::string file;
		int position = 1;
	};

	std::optional<face_term> parse_face_term(std::string_view text);
	// `:1` is left out: most pictures hold one face.
	std::string format_face_term(std::string_view file, int position);

	// The faces a term names: at most eight files of that name, in path order, each at that position.
	std::vector<uint32_t> faces_named(const face_store& store, const face_term& term);

	using picture_set = core::hash_set<core::file_path, core::ihash, core::ieq>;

	// The pictures `face:` lists: those holding a face whose similarity to a face the term names reaches
	// `similar`. Answered once for each store generation and term, and empty while face search is off.
	std::shared_ptr<const picture_set> pictures_like(std::string_view term);
	// Whether `@face` lists a picture: at least one face found in it.
	bool has_found_faces(core::file_path path);

	// ---- People (docs/faces.md#7-recognizing-people) --------------------------------------------------

	enum class member_kind : uint8_t
	{
		unknown,
		confirmed,
		named,
		suggested,
	};

	constexpr bool is_held(const member_kind kind)
	{
		return kind == member_kind::confirmed || kind == member_kind::named;
	}

	constexpr float not_compared = -2.0f;

	// docs/faces.md#72-suggestions: a face a person's suggestions are compared with - one of theirs, or one ruled out
	// for them - and how clear it is, which decides between faces when there are too many to keep.
	struct face_exemplar
	{
		// UINT32_MAX for a portable sample with no picture here.
		uint32_t face = 0;
		face_hash hash;
		double quality = 0.0;
	};

	struct person_member
	{
		uint32_t face = 0;
		float score = not_compared;
		member_kind kind = member_kind::unknown;
	};

	struct face_person
	{
		// The book's stable ID, or zero for a person known only from files.
		uint32_t id = 0;
		std::string name;
		// Each section best match first. A suggestion never feeds back into comparisons.
		std::vector<person_member> confirmed;
		std::vector<person_member> suggested;
		std::vector<uint32_t> samples;
		// The pictures holding their confirmed or named faces, and those holding a face suggested for them too.
		uint32_t confirmed_pictures = 0;
		uint32_t pictures = 0;
		// docs/faces.md#72-suggestions: the bounded profile their suggestions were compared with, and the faces ruled
		// out for them, which naming a face ranks people by too (docs/faces.md#95-naming-a-face).
		std::vector<face_exemplar> positive, negative;
	};

	struct picture_evidence
	{
		std::vector<named_region> regions;
		int year = 0;
		// Also includes names with no associated face region.
		core::cached names;
		picture_evidence(std::vector<named_region> r, const int y = 0, const core::cached n = {}) :
			regions(std::move(r)), year(y), names(n)
		{
		}
	};
	using regions_of = std::function<picture_evidence(core::file_path path)>;

	struct picture_person
	{
		uint32_t person = 0; // Index into overview::people.
		bool confirmed = false;
	};

	// Published whole. Unknown contains visible faces with neither an identity nor a current offer.
	struct overview
	{
		face_store_const_ptr store;
		uint64_t generation = 0;
		uint64_t book_generation = 0;
		roster_ptr roster;
		std::vector<face_person> people;
		// Both in ascending face ID order for cheap visibility checks on a photograph.
		std::vector<uint32_t> unknown;
		std::vector<uint32_t> hidden;
		// docs/faces.md#91-people: the same Unknown faces in review order - runs of faces that look alike, the largest
		// first, then single faces, then from `unclear_from` the small or unclear ones - with each run's length. Empty
		// where the pass was cancelled before ordering them.
		std::vector<uint32_t> unknown_by_similarity;
		std::vector<uint32_t> unknown_runs;
		size_t unclear_from = 0;
		const std::vector<uint32_t>& unknown_for_review() const
		{
			return unknown_by_similarity.empty() ? unknown : unknown_by_similarity;
		}

		struct membership
		{
			uint32_t person, offset;
			bool confirmed;
		};
		core::hash_map<uint32_t, membership> members;
		// docs/faces.md#72-suggestions: the people each face is ruled out for, by the book's IDs, sorted.
		core::hash_map<uint32_t, std::vector<uint32_t>> ruled_out;
		core::hash_map<core::file_path, std::vector<picture_person>, core::ihash, core::ieq> picture_people;
		core::hash_map<core::file_path, std::vector<std::string>, core::ihash, core::ieq> contradicted;
		// Adjustments to metadata-only picture counts. Summed once here rather than in each autocomplete.
		core::hash_map<std::string, int, core::ihash, core::ieq> count_adjustments;

		const face_person* find_person(std::string_view name) const;
		const face_person* person_of_face(uint32_t face) const;
		const person_member* member_of_face(uint32_t face) const;
		bool file_name_contradicted(core::file_path path, std::string_view name) const;
	};

	using overview_ptr = std::shared_ptr<const overview>;

	// Runs body(begin, end) over [0, count) in pieces, on as many threads as the caller lends it, and returns once every
	// piece has run. Empty, everything runs on the calling thread.
	using parallel_for = std::function<void(size_t count, const std::function<void(size_t, size_t)>& body)>;

	// The people, unknown and hidden faces, from the store and the face book as they stand. `cancel` answers
	// true once the pass is no longer wanted.
	overview_ptr make_overview(const face_store_const_ptr& store, const regions_of& regions, const face_book& book,
	                           const std::function<bool()>& cancel, const tuning& t = defaults(),
	                           const parallel_for& parallel = {});

	// The overview searches are answered from, null while face search is off; and whom a picture's faces are, for
	// person: and with:person, which find a name the face book gives as they find one a file gives.
	overview_ptr active_overview();
	void set_active_overview(overview_ptr o);
	std::vector<std::string> people_in(core::file_path path);

	// docs/faces.md#93-faces-on-a-picture: a face found on a picture that shows on it, with whom the overview and the
	// face book say it is.
	struct face_on_picture
	{
		uint32_t face = 0;
		// Fractions of the upright picture.
		rectd box;
		// Empty for a face nobody has named.
		std::string name;
		// Offered by resemblance, not confirmed.
		bool offered = false;
		// The pictures holding the person's confirmed faces, and the faces suggested for them.
		uint32_t pictures = 0;
		uint32_t suggested = 0;
		// In a video, the frame it was found in: it is on that frame alone.
		std::optional<uint32_t> frame_ms;
	};

	// The faces found on a picture, in their stable order, without those put away or of a person hidden.
	std::vector<face_on_picture> faces_on_picture(const face_store& store, const overview* o, const face_book& book,
	                                              core::file_path path);

	// docs/faces.md#95-naming-a-face: a person naming faces offers at a press, with their likeness to the faces.
	struct likely_person
	{
		std::string name;
		float score = not_compared;
	};

	// Whom faces of the overview's store most likely are: the person they resemble most on average, at least
	// `similar`, and up to two more Diffractor cannot tell from them - within `margin` - the most alike first. Nobody
	// named in `leave_out` is offered, nor a person a face is ruled out for or confirmed beside in its picture, nor one
	// the faces resemble no more than the faces ruled out for them.
	std::vector<likely_person> likely_people(const overview& o, std::span<const uint32_t> faces,
	                                         std::span<const std::string> leave_out, const tuning& t = defaults());

	// ---- The scan (docs/faces.md#3-face-search) --------------------------------------------------------

	// A picture decoded to fit `bound` as face search reads one: from the bytes read for it where they decode,
	// otherwise by the file's own load, through LibRaw where it is raw. Null where neither decodes it.
	core::const_surface_ptr decode_for_faces(files& ff, core::file_path path, file_type_ref ft,
	                                         std::span<const uint8_t> bytes, sizei bound);

	// What a face is cut from, decoded to fit `bound`: a photo as decode_for_faces decodes it, or the frame of a video
	// the face was found in, `time_ms` into it. Null where it cannot be read. Reads the file: a worker's.
	core::const_surface_ptr decode_face_source(files& ff, core::file_path path, file_type_ref ft, bool video,
	                                           uint32_t time_ms, sizei bound);

	// The frames of a video its faces were found in, read for decoding apart, so the reads can keep their turn on the
	// disk while the decoding does not: what decodes them, and each frame, null where it could not be read.
	struct face_frames
	{
		av_key_frame_codec_ptr codec;
		std::vector<std::shared_ptr<const av_key_frame>> frames;
	};

	face_frames read_face_frames(core::file_path path, std::span<const uint32_t> times_ms);
	core::const_surface_ptr decode_face_frame(const face_frames& read, size_t frame, sizei bound);

	struct scan_progress
	{
		// Pictures face search covers, and those read with a current record.
		uint32_t eligible = 0;
		uint32_t done = 0;
		// Pictures that could not be read this session, apart from those done.
		uint32_t unreadable = 0;
		// Pictures whose folder could not be reached - on a drive that is not connected, or a share that is offline -
		// left until it can be: docs/faces.md#32-the-scan.
		uint32_t waiting = 0;
		uint32_t faces = 0;
		bool running = false;
		// Pictures read a second, as measured this session.
		double rate = 0.0;

		uint32_t left() const
		{
			const auto settled = done + unreadable + waiting;
			return eligible > settled ? eligible - settled : 0;
		}
	};

	// Gives the regular index priority without interrupting an in-flight photo.
	class scan_io_gate : core::no_copy
	{
		struct state;
		std::shared_ptr<state> _state;

	public:
		class permit : core::no_copy
		{
			friend class scan_io_gate;
			std::shared_ptr<state> _owner;
			bool _face = false;
			bool _seek_free = false;

		public:
			~permit();
		};
		using permit_ptr = std::shared_ptr<const permit>;

		scan_io_gate();
		permit_ptr start_index(bool seek_free = false) { return acquire(false, seek_free); }
		permit_ptr try_start_face(bool seek_free) { return acquire(true, seek_free); }

	private:
		permit_ptr acquire(bool face, bool seek_free);
	};

	// How the face database keeps the face book: docs/faces.md#64-the-face-database.
	enum class book_keeping : uint8_t
	{
		// Not opened yet.
		opening,
		// Each change is written as it is made.
		kept,
		// A change could not be written: the book is held in memory and written whole until a write succeeds.
		unsaved,
		// Another Diffractor window writes the face database: this one shows what it holds and changes nothing, until
		// that window closes.
		shared,
		// The face database could not be opened: changes last this session, and can be exported.
		memory,
	};

	// Face search: the store, the scan that fills it and the overview made from it. Owned by the index, so the
	// database can load what it holds as the index loads.
	class face_search final : core::no_copy
	{
		index_state& _index;
		async_strategy& _async;

		// Mirrors of the user's settings, set on the UI thread and read by the workers: whether face search is on, which
		// of photos and videos it reads and shows while it is, and whether it is paused.
		std::atomic_bool _enabled = false;
		std::atomic_bool _photos = true;
		std::atomic_bool _videos = true;
		std::atomic_bool _paused = false;

		std::atomic<std::shared_ptr<face_store>> _store;
		// The store the database fills as it loads, published once it is full. The database's thread alone.
		std::shared_ptr<face_store> _loading;
		std::atomic<overview_ptr> _overview;
		// Moved by turning face search off and by deleting its data, so nothing begun before can be published
		// after.
		std::atomic<uint64_t> _session = 1;

		std::atomic_bool _scanning = false;
		// Set by every start asked for, and cleared as a pass begins, so one asked for while a pass runs is not lost.
		std::atomic_bool _scan_wanted = false;
		std::atomic_bool _overview_running = false;
		std::atomic_bool _overview_wanted = false;
		// Set as the index reports a change, after which the names files give may differ, and cleared as an overview
		// starts reading them.
		std::atomic_bool _names_changed = true;
		std::atomic<int> _in_flight = 0;
		std::atomic<int64_t> _bytes_in_flight = 0;
		scan_io_gate _scan_io;

		mutable core::mutex _rw;
		_Guarded_by_(_rw) scan_progress _progress;
		_Guarded_by_(_rw) std::optional<core::file_path> _priority;
		_Guarded_by_(_rw) core::hash_map<core::file_path, int, core::ihash, core::ieq> _failures;
		// Folders a read found missing - on a drive that is not connected, or a share that is offline. Their pictures
		// are not failures: each pass looks for the folder again, and reads them once it is back.
		_Guarded_by_(_rw) core::hash_set<core::folder_path, core::ihash, core::ieq> _absent_folders;
		// When this scan began and how many pictures it has read, for its rate.
		_Guarded_by_(_rw) double _rate_start_ms = 0.0;
		_Guarded_by_(_rw) size_t _rate_count = 0;

		struct result
		{
			core::file_path path;
			std::optional<face_record> record;
			uint64_t session = 0;
		};

		_Guarded_by_(_rw) std::vector<result> _results;

		// A change to a picture's record that is not a new reading of it, applied by the store's writer.
		struct mark
		{
			core::file_path path;
			core::date_t from;
			core::date_t to;
			core::orientation orientation_from = core::orientation::top_left;
			core::orientation orientation_to = core::orientation::top_left;
		};

		_Guarded_by_(_rw) std::vector<mark> _marks;
		core::thread_event _wake{false, false};

		// What the user told Diffractor about faces, and where the face database keeps it.
		face_book _book;
		std::atomic<book_keeping> _book_keeping = book_keeping::opening;
		_Guarded_by_(_rw) core::file_path _book_path;
		// The book's changes waiting for the database thread, in the order they were made. It writes them after each
		// batch of its tasks and once more as Diffractor closes, when tasks still queued are dropped: nothing makes
		// the face book again.
		platform::queue<book_write> _book_writes;

		void scan(uint64_t session);
		void scan_pass(uint64_t session);
		void read_one(uint64_t session, core::file_path path, file_type_ref ft, core::date_t modified,
		              const core::blob& bytes, std::vector<named_region> regions);
		// docs/faces.md#33-videos: a video's key frames read on the scan's thread, each decoded on a worker, and made
		// one record once the last is. `keep_up` applies what the workers have read while it waits for them.
		struct video_job;
		void read_video(uint64_t session, core::file_path path, core::date_t modified,
		                const std::function<void()>& keep_up);
		void read_video_frame(const std::shared_ptr<video_job>& job, const av_key_frame_codec_ptr& codec,
		                      const av_key_frame& frame);
		void finish_video(video_job& job);
		void add_result(core::file_path path, std::optional<face_record> record, uint64_t session);
		size_t apply_results(uint64_t session);
		void apply_marks();
		void queue_marks();
		bool is_current(uint64_t session) const { return _session.load() == session; }

	public:
		face_search(index_state& index, async_strategy& async);
		~face_search();

		// The user's settings, before the index loads: which of photos and videos face search reads, neither while it is
		// off, and whether it is paused.
		void configure(bool photos, bool videos, bool paused);
		bool is_enabled() const { return _enabled.load(); }
		// docs/faces.md#31-turning-it-on-and-off: whether face search reads and shows photos, and videos.
		bool reads_photos() const { return _enabled.load() && _photos.load(); }
		bool reads_videos() const { return _enabled.load() && _videos.load(); }
		bool is_paused() const { return _paused.load(); }
		scan_io_gate& scan_io() { return _scan_io; }
		// The session face search is in, which a face record written by an earlier one is not written for.
		uint64_t session() const { return _session.load(); }

		// The store as it stands, or null while face search is off.
		face_store_const_ptr store() const { return _store.load(); }
		overview_ptr current_overview() const { return _overview.load(); }
		scan_progress progress() const;
		// Whether face search has read every picture it can and grouped what it found as the store and the face book
		// stand; true while it is off or paused, with nothing to wait for.
		bool is_settled() const;

		// Called by the database, as it reads the index at start, with each picture's record; only while face
		// search is on.
		void load(core::file_path path, std::span<const uint8_t> record);
		// The load is finished: the store is published, and the overview made.
		void loaded();
		// Promote older path-only decisions once their original scanned faces are available.
		void bind_legacy_signatures();

		// Turning face search on loads what the index holds of the photos and videos it reads, then scans; asked for
		// other kinds while on, it puts away what it holds and loads them again. Off puts the store away, keeping the
		// records; deleting removes them too. Each moves the session, so work in flight is not published.
		void turn_on(bool photos = true, bool videos = true);
		void turn_off();
		void delete_data();
		void pause(bool paused);
		// Starts the scan where face search is on, not paused and the index is ready; does nothing otherwise.
		void start_scan();
		// The picture on screen is read next, out of turn.
		void prioritize(core::file_path path);
		// Makes the overview again, off the UI thread: one pass running and at most one waiting.
		void queue_overview();
		// docs/faces.md#9-the-faces-view: whether a pass is making the overview again, or waiting to, which the Faces view
		// says while it does.
		bool is_grouping() const { return _overview_running.load() || _overview_wanted.load(); }
		// The same, only where something it is made from may have changed since the last: the store, the face book, or
		// the names files give - which the index's reports of a change, passed on here, may have changed. Grouping a
		// large collection takes seconds, so the view showing it does not ask for one each time it is opened.
		void queue_overview_if_stale();
		void names_may_have_changed()
		{
			_names_changed = true;
			if (_enabled.load() && _store.load()) queue_overview();
		}

		// docs/faces.md#6-the-face-book: what the user told Diffractor about faces. Each change is written to the face
		// database and has the overview made again.
		face_book& book() { return _book; }
		const face_book& book() const { return _book; }
		// The face database was opened, or could not be: where it is, and how the book's changes are kept there. Told
		// again as the book is read again or taken over from another window, and as writes fail or succeed.
		void book_opened(core::file_path path, book_keeping keeping);
		void book_written(bool written);
		book_keeping book_state() const { return _book_keeping.load(); }
		bool is_book_kept() const { return book_state() == book_keeping::kept; }
		// Whether the user can change the face book in this window: not while another window writes it.
		bool can_change_book() const { return book_state() != book_keeping::shared; }
		// Whether the database thread should be asked again: to take the book over, read it again, or write it whole.
		bool book_needs_polling() const
		{
			const auto state = book_state();
			return state == book_keeping::shared || state == book_keeping::unsaved;
		}
		core::file_path book_path() const;
		// The book's changes not yet written, taken by the database thread to write.
		std::deque<book_write> take_book_writes() { return _book_writes.dequeue_all(); }

		// A write of metadata alone: the store's record of the picture is stamped with the file's new time, and
		// turned where its declared orientation alone changed. The index's own record is the database's to stamp.
		void restamp(core::file_path path, core::date_t from, core::date_t to, core::orientation orientation_from,
		             core::orientation orientation_to);
	};
}
