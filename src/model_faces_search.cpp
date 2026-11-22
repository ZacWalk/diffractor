// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Face search's store and scan - the blocks every picture's faces are kept in, similarity over them,
// the scan that reads the collection's pictures for faces, a metadata write's new stamp put on a picture's record, the
// faces a picture shows and whom each is, and the face book's changes handed to the face database. Owned by
// docs/faces.md; the people, unknown and hidden faces made over the store and the book are in
// model_faces_groups.cpp.

#include "pch.h"

#include "model_faces_search.h"

#include "app_settings.h"
#include "av_format.h"
#include "files.h"
#include "model.h"
#include "model_db.h"
#include "model_index.h"
#include "model_index_internal.h"

#include <condition_variable>

namespace faces
{
	static constexpr double entry_scale = 16384.0;

	static int16_t entry_value(const double v)
	{
		return static_cast<int16_t>(std::clamp(std::lround(v * entry_scale), -32768L, 32767L));
	}

	rectd face_entry::bounds() const
	{
		return {box[0] / entry_scale, box[1] / entry_scale, box[2] / entry_scale, box[3] / entry_scale};
	}

	pointd face_entry::mark(const int i) const
	{
		return {marks[i * 2] / entry_scale, marks[i * 2 + 1] / entry_scale};
	}

	// ---- The store -----------------------------------------------------------------------------------

	face_store::face_store() :
		_face_blocks(std::make_unique<std::atomic<face_block*>[]>(max_blocks)),
		_run_blocks(std::make_unique<std::atomic<run_block*>[]>(max_blocks))
	{
	}

	face_store::~face_store() = default;

	bool face_store::add(const core::file_path path, const face_record& record)
	{
		const auto first = _face_count.load(std::memory_order_relaxed);
		const auto r = _run_count.load(std::memory_order_relaxed);
		const auto count = static_cast<uint32_t>(std::min<size_t>(record.faces.size(), UINT16_MAX));

		if ((first + count) >> block_bits >= max_blocks || r >> block_bits >= max_blocks) return false;

		// Blocks are made before anything in them is published, and never move.
		const auto ensure_face_block = [this](const uint32_t index)
		{
			const auto block = index >> block_bits;
			if (!_face_blocks[block].load(std::memory_order_relaxed))
			{
				auto made = std::make_unique<face_block>();
				_face_blocks[block].store(made.get(), std::memory_order_release);
				_owned_faces.emplace_back(std::move(made));
			}
			return _face_blocks[block].load(std::memory_order_relaxed);
		};

		for (uint32_t i = 0; i < count; ++i)
		{
			const auto face = first + i;
			auto* const block = ensure_face_block(face);
			const auto slot = face & (block_size - 1);
			const auto& f = record.faces[i];

			auto& e = block->entries[slot];
			e.box = {entry_value(f.box.X), entry_value(f.box.Y), entry_value(f.box.Width), entry_value(f.box.Height)};
			for (auto m = 0; m < landmark_count; ++m)
			{
				e.marks[m * 2] = entry_value(f.marks[m].X);
				e.marks[m * 2 + 1] = entry_value(f.marks[m].Y);
			}
			e.run = r;
			e.time_ms = f.time_ms;
			e.eye_distance = f.eye_distance;
			e.confidence = f.confidence;
			e.frontal = f.frontal;
			e.flags = f.flags;
			e.position = static_cast<uint8_t>(std::min<uint32_t>(i, UINT8_MAX));
			e.has_hash = !f.hash.is_empty();
			block->values[slot] = f.hash.values;
			block->scales[slot] = f.hash.scale;
		}

		const auto run_block_index = r >> block_bits;
		if (!_run_blocks[run_block_index].load(std::memory_order_relaxed))
		{
			auto made = std::make_unique<run_block>();
			_run_blocks[run_block_index].store(made.get(), std::memory_order_release);
			_owned_runs.emplace_back(std::move(made));
		}

		auto& run = _run_blocks[run_block_index].load(std::memory_order_relaxed)->runs[r & (block_size - 1)];
		run.path = path;
		run.first = first;
		run.count = static_cast<uint16_t>(count);
		run.model = record.model;
		run.stamp = record.stamp;
		run.extent = record.extent;
		run.status = record.status;
		run.detection_revision = record.detection_revision;
		run.video = record.video;
		run.dismissed.clear();
		if (std::ranges::any_of(record.faces, [](const found_face& f) { return !f.dismissed.empty(); }))
		{
			run.dismissed.resize(count);
			for (uint32_t i = 0; i < count; ++i) run.dismissed[i] = record.faces[i].dismissed;
		}
		run.seen.clear();
		if (std::ranges::any_of(record.faces, [](const found_face& f) { return !f.seen.empty(); }))
		{
			run.seen.resize(count);
			for (uint32_t i = 0; i < count; ++i) run.seen[i] = record.faces[i].seen;
		}

		_face_count.store(first + count, std::memory_order_release);
		_run_count.store(r + 1, std::memory_order_release);

		std::optional<uint32_t> replaced;
		{
			core::exclusive_lock lock(_paths_rw);
			const auto [found, inserted] = _by_path.try_emplace(path, r);
			if (!inserted)
			{
				replaced = found->second;
				found->second = r;
			}
		}

		if (replaced) run_at_writer(*replaced).retired.store(true, std::memory_order_release);
		return true;
	}

	void face_store::retire(const core::file_path path)
	{
		std::optional<uint32_t> removed;
		{
			core::exclusive_lock lock(_paths_rw);
			if (const auto found = _by_path.find(path); found != _by_path.end())
			{
				removed = found->second;
				_by_path.erase(found);
			}
		}

		if (removed) run_at_writer(*removed).retired.store(true, std::memory_order_release);
	}

	void face_store::publish()
	{
		_generation.fetch_add(1, std::memory_order_acq_rel);
	}

	const face_entry& face_store::entry(const uint32_t face) const
	{
		return _face_blocks[face >> block_bits].load(std::memory_order_acquire)->entries[face & (block_size - 1)];
	}

	const int8_t* face_store::values(const uint32_t face) const
	{
		return _face_blocks[face >> block_bits].load(std::memory_order_acquire)->values[face & (block_size - 1)].data();
	}

	float face_store::scale(const uint32_t face) const
	{
		return _face_blocks[face >> block_bits].load(std::memory_order_acquire)->scales[face & (block_size - 1)];
	}

	face_hash face_store::hash(const uint32_t face) const
	{
		face_hash result;
		const auto* const v = values(face);
		std::copy_n(v, vector_size, result.values.begin());
		result.scale = scale(face);
		return result;
	}

	const picture_run& face_store::run(const uint32_t r) const
	{
		return _run_blocks[r >> block_bits].load(std::memory_order_acquire)->runs[r & (block_size - 1)];
	}

	picture_run& face_store::run_at_writer(const uint32_t r)
	{
		return _run_blocks[r >> block_bits].load(std::memory_order_acquire)->runs[r & (block_size - 1)];
	}

	std::optional<uint32_t> face_store::find(const core::file_path path) const
	{
		core::shared_lock lock(_paths_rw);
		const auto found = _by_path.find(path);
		if (found == _by_path.end() || found->second >= run_count()) return {};
		return found->second;
	}

	std::optional<face_record> face_store::record(const core::file_path path) const
	{
		const auto r = find(path);
		if (!r) return {};

		const auto& run = this->run(*r);
		face_record result;
		result.model = run.model;
		result.stamp = run.stamp;
		result.extent = run.extent;
		result.status = run.status;
		result.detection_revision = run.detection_revision;
		result.video = run.video;
		result.faces.resize(run.count);

		for (uint32_t i = 0; i < run.count; ++i)
		{
			const auto face = run.first + i;
			const auto& e = entry(face);
			auto& f = result.faces[i];
			f.box = e.bounds();
			for (auto m = 0; m < landmark_count; ++m) f.marks[m] = e.mark(m);
			f.confidence = e.confidence;
			f.frontal = e.frontal;
			f.eye_distance = e.eye_distance;
			f.flags = e.flags;
			f.time_ms = e.time_ms;
			if (e.has_hash) f.hash = hash(face);
			if (i < run.dismissed.size()) f.dismissed = run.dismissed[i];
			if (i < run.seen.size()) f.seen = run.seen[i];
		}

		return result;
	}

	std::optional<uint32_t> face_store::face_at(const core::file_path path, const int position) const
	{
		const auto r = find(path);
		if (!r) return {};
		const auto& run = this->run(*r);
		if (position < 0 || position >= run.count) return {};
		return run.first + static_cast<uint32_t>(position);
	}

	// ---- Similarity ----------------------------------------------------------------------------------

	float similarity(const face_store& store, const uint32_t face, const face_vector& v)
	{
		const auto* const values = store.values(face);
		auto dot = 0.0f;
		for (auto i = 0; i < vector_size; ++i) dot += values[i] * v[i];
		return dot * store.scale(face);
	}

	float similarity(const face_store& store, const uint32_t a, const uint32_t b)
	{
		return static_cast<float>(dot_values(store.values(a), store.values(b))) * store.scale(a) * store.scale(b);
	}

	std::vector<scored_face> similar_faces(const face_store& store, const face_hash& query, const float floor)
	{
		std::vector<scored_face> result;
		if (query.is_empty()) return result;

		const auto model = model_id();
		const auto runs = store.run_count();

		for (uint32_t r = 0; r < runs; ++r)
		{
			const auto& run = store.run(r);
			if (!run.is_current() || run.model != model) continue;

			for (uint32_t face = run.first; face < run.first + run.count; ++face)
			{
				if (!store.entry(face).has_hash) continue;
				const auto score = static_cast<float>(dot_values(store.values(face), query.values.data())) *
				                   store.scale(face) * query.scale;
				if (score >= floor) result.push_back({face, score});
			}
		}

		std::ranges::sort(result, [](const scored_face& a, const scored_face& b)
		{ return a.score != b.score ? a.score > b.score : a.face < b.face; });
		return result;
	}

	const face_person* overview::find_person(const std::string_view name) const
	{
		if (core::trim(name).empty()) return nullptr;
		const auto found = std::ranges::find_if(people, [name](const face_person& p)
		{ return !p.name.empty() && core::icmp(p.name, core::trim(name)) == 0; });
		return found == people.end() ? nullptr : &*found;
	}

	const face_person* overview::person_of_face(const uint32_t face) const
	{
		const auto found = members.find(face);
		return found == members.end() ? nullptr : &people[found->second.person];
	}

	const person_member* overview::member_of_face(const uint32_t face) const
	{
		const auto found = members.find(face);
		if (found == members.end()) return nullptr;
		const auto& [p, i, confirmed] = found->second;
		return confirmed ? &people[p].confirmed[i] : &people[p].suggested[i];
	}

	bool overview::file_name_contradicted(const core::file_path path, const std::string_view name) const
	{
		const auto at = contradicted.find(path);
		return at != contradicted.end() &&
		       std::ranges::any_of(at->second, [&](const std::string& n) { return core::icmp(n, name) == 0; });
	}

	static std::atomic<overview_ptr> active_people;

	overview_ptr active_overview()
	{
		return active_people.load();
	}

	void set_active_overview(overview_ptr o)
	{
		active_people.store(std::move(o));
	}

	std::vector<std::string> people_in(const core::file_path path)
	{
		const auto o = active_overview();
		if (!o) return {};
		const auto found = o->picture_people.find(path);
		std::vector<std::string> names;
		if (found != o->picture_people.end())
			for (const auto& member : found->second)
				if (member.confirmed) names.push_back(o->people[member.person].name);
		return names;
	}

	std::vector<face_on_picture> faces_on_picture(const face_store& store, const overview* o, const face_book& book,
	                                              const core::file_path path)
	{
		std::vector<face_on_picture> result;
		const auto r = store.find(path);
		if (!r || store.run(*r).model != model_id()) return result;

		const auto& run = store.run(*r);
		const auto people = book.people();
		if (o && o->store.get() != &store) o = nullptr; // Face indices are local to a scan store.
		// A name the user just gave is already authoritative while the overview is being rebuilt.
		const bool current = o && o->book_generation == book.generation() && o->generation == store.generation();
		const auto marks = current ? std::vector<face_mark>{} : book.marks(path);
		std::vector<const face_mark*> recent;
		if (!marks.empty())
		{
			std::vector<rectd> boxes;
			boxes.reserve(run.count);
			for (uint32_t i = 0; i < run.count; ++i) boxes.push_back(store.entry(run.first + i).bounds());
			const auto matches = match_marks(marks, boxes);
			recent.resize(run.count);
			for (size_t i = 0; i < matches.size(); ++i)
				if (matches[i] >= 0) recent[matches[i]] = &marks[i];
		}

		for (uint32_t i = 0; i < run.count; ++i)
		{
			const auto face = run.first + i;
			face_on_picture f{face, store.entry(face).bounds()};
			if (run.video) f.frame_ms = store.entry(face).time_ms;
			const auto* const person = o ? o->person_of_face(face) : nullptr;
			const auto* mark = recent.empty() ? nullptr : recent[i];
			std::optional<face_mark> exact;
			if (!current)
			{
				const auto key = store.signature(face);
				if (mark && mark->signature && *mark->signature != key) mark = nullptr;
				if (store.entry(face).has_hash) exact = book.signature_mark(key);
				if (exact) mark = &*exact;
			}
			if (mark)
			{
				if (mark->hidden) continue;
				if (const auto* placed = people->person(mark->person))
				{
					f.name = placed->name;
					if (person && person->id == mark->person)
					{
						f.pictures = person->confirmed_pictures;
						f.suggested = static_cast<uint32_t>(person->suggested.size());
					}
				}
			}
			else if (current ? std::ranges::binary_search(o->hidden, face) : store.entry(face).is_hidden()) continue;
			else if (person)
			{
				const auto* kept = person->id ? people->person(person->id) : nullptr;
				const auto* member = o->member_of_face(face);
				// Undo can remove the mark entirely; do not display an obsolete confirmation in the meantime.
				if (current || (member && member->kind != member_kind::confirmed))
				{
					f.name = kept ? kept->name : person->name;
					f.offered = member && member->kind == member_kind::suggested;
					f.pictures = person->confirmed_pictures;
					f.suggested = static_cast<uint32_t>(person->suggested.size());
				}
			}

			result.push_back(std::move(f));
		}

		return result;
	}

	// ---- Face terms ----------------------------------------------------------------------------------

	static std::atomic<face_store_const_ptr> active;

	face_store_const_ptr active_store()
	{
		return active.load();
	}

	void set_active_store(face_store_const_ptr store)
	{
		active.store(std::move(store));
	}

	std::optional<face_term> parse_face_term(std::string_view text)
	{
		text = core::trim(text);
		if (text.empty()) return {};

		face_term result;
		const auto colon = text.rfind(':');

		if (colon != std::string_view::npos && colon + 1 < text.size() &&
		    std::ranges::all_of(text.substr(colon + 1), [](const char c) { return c >= '0' && c <= '9'; }))
		{
			const auto position = core::to_int(text.substr(colon + 1));
			if (position < 1 || position > 255) return {};
			result.position = position;
			text = text.substr(0, colon);
		}

		if (text.empty()) return {};
		result.file = std::string(text);
		return result;
	}

	std::string format_face_term(const std::string_view file, const int position)
	{
		return position <= 1 ? std::string(file) : std::format("{}:{}", file, position);
	}

	std::vector<uint32_t> faces_named(const face_store& store, const face_term& term)
	{
		constexpr size_t max_files = 8;
		const auto model = model_id();
		std::vector<std::pair<core::file_path, uint32_t>> found;
		const auto runs = store.run_count();

		for (uint32_t r = 0; r < runs; ++r)
		{
			const auto& run = store.run(r);
			if (!run.is_current() || run.model != model || run.count < term.position) continue;
			if (core::icmp(run.path.name(), term.file) != 0) continue;
			found.emplace_back(run.path, run.first + static_cast<uint32_t>(term.position - 1));
		}

		std::ranges::sort(found, [](const auto& a, const auto& b) { return icmp(a.first.str(), b.first.str()) < 0; });

		std::vector<uint32_t> result;
		for (const auto& [path, face] : found)
		{
			if (result.size() >= max_files) break;
			if (store.entry(face).has_hash) result.push_back(face);
		}
		return result;
	}

	std::shared_ptr<const picture_set> pictures_like(const std::string_view text)
	{
		const auto store = active_store();
		if (!store) return {};

		// Keyed by everything the answer depends on: the store, what it holds and the term. The store is held
		// weakly, so an answer never keeps a store face search has put away.
		struct cached_answer
		{
			std::weak_ptr<const face_store> store;
			uint64_t generation;
			std::string term;
			std::shared_ptr<const picture_set> pictures;
		};

		static core::mutex answers_rw;
		static std::vector<cached_answer> answers;
		const auto generation = store->generation();

		{
			core::exclusive_lock lock(answers_rw);
			const auto found = std::ranges::find_if(answers, [&](const cached_answer& a)
			{ return a.store.lock() == store && a.generation == generation && core::icmp(a.term, text) == 0; });
			if (found != answers.end()) return found->pictures;
		}

		auto pictures = std::make_shared<picture_set>();

		if (const auto term = parse_face_term(text))
		{
			for (const auto face : faces_named(*store, *term))
			{
				for (const auto& like : similar_faces(*store, store->hash(face), defaults().similar))
					pictures->insert(store->run_of_face(like.face).path);
			}
		}

		core::exclusive_lock lock(answers_rw);
		if (answers.size() >= 8) answers.erase(answers.begin());
		answers.push_back({store, generation, std::string(text), pictures});
		return pictures;
	}

	bool has_found_faces(const core::file_path path)
	{
		const auto store = active_store();
		if (!store) return false;
		const auto r = store->find(path);
		if (!r) return false;
		const auto& run = store->run(*r);
		return run.count > 0 && run.model == model_id();
	}

	// ---- Scan admission ------------------------------------------------------------------------------

	struct scan_io_gate::state
	{
		struct count
		{
			int active = 0;
			int not_seek_free = 0;

			bool blocks(const bool seek_free) const { return seek_free ? not_seek_free > 0 : active > 0; }
		};

		std::mutex mutex;
		std::condition_variable changed;
		count index, faces;
	};

	scan_io_gate::scan_io_gate() : _state(std::make_shared<state>())
	{
	}

	scan_io_gate::permit_ptr scan_io_gate::acquire(const bool face, const bool seek_free)
	{
		auto ticket = std::make_shared<permit>();
		std::unique_lock lock(_state->mutex);
		auto& own = face ? _state->faces : _state->index;
		const auto& other = face ? _state->index : _state->faces;
		if (platform::is_closing || (face && other.blocks(seek_free))) return {};

		// Count a waiting index as well, or incoming face reads could keep it waiting indefinitely.
		++own.active;
		if (!seek_free) ++own.not_seek_free;
		ticket->_owner = _state;
		ticket->_face = face;
		ticket->_seek_free = seek_free;

		while (!face && other.blocks(seek_free))
		{
			if (platform::is_closing) return {};
			_state->changed.wait_for(lock, std::chrono::milliseconds(100));
		}
		return ticket;
	}

	scan_io_gate::permit::~permit()
	{
		if (!_owner) return;
		{
			const std::lock_guard lock(_owner->mutex);
			auto& count = _face ? _owner->faces : _owner->index;
			--count.active;
			if (!_seek_free) --count.not_seek_free;
		}
		_owner->changed.notify_all();
	}

	// ---- Face search ---------------------------------------------------------------------------------

	face_search::face_search(index_state& index, async_strategy& async) : _index(index), _async(async)
	{
		// Each change the user makes is written to the face database, on the database's thread, and the overview
		// made again with it. One wake on the empty-to-nonempty edge, as the index's writes have: the thread
		// writes the queue after every batch of its tasks.
		_book.persist_to([this](book_write write)
		{
			if (_book_writes.enqueue(std::move(write))) _async.queue_database([](database&) {});
			queue_overview();
			_index.invalidate_view(view_invalid::faces);
		});
	}

	// The store and the overview searches read go with the face search that made them.
	face_search::~face_search()
	{
		_book.persist_to(nullptr);
		if (auto mine = face_store_const_ptr(_store.load())) active.compare_exchange_strong(mine, nullptr);
		if (auto mine = _overview.load()) active_people.compare_exchange_strong(mine, nullptr);
	}

	void face_search::book_opened(const core::file_path path, const book_keeping keeping)
	{
		{
			core::exclusive_lock lock(_rw);
			_book_path = path;
		}

		_book.set_read_only(keeping == book_keeping::shared);
		_book_keeping = keeping;
		queue_overview();
		_index.invalidate_view(view_invalid::faces | view_invalid::command_state);
	}

	void face_search::book_written(const bool written)
	{
		auto state = book_state();
		if (state != book_keeping::kept && state != book_keeping::unsaved) return;
		const auto now = written ? book_keeping::kept : book_keeping::unsaved;
		if (_book_keeping.compare_exchange_strong(state, now) && state != now)
			_index.invalidate_view(view_invalid::faces);
	}

	core::file_path face_search::book_path() const
	{
		core::shared_lock lock(_rw);
		return _book_path;
	}

	scan_progress face_search::progress() const
	{
		core::shared_lock lock(_rw);
		return _progress;
	}

	bool face_search::is_settled() const
	{
		if (!_enabled.load() || _paused.load()) return true;
		if (_scanning.load() || _scan_wanted.load() || _overview_running.load() || _overview_wanted.load() ||
		    _in_flight.load() != 0)
			return false;

		const auto p = progress();
		const auto store = _store.load();
		const auto o = _overview.load();
		return !p.running && p.left() == 0 && store && o && o->store == store && o->generation == store->generation() &&
		       o->book_generation == _book.generation();
	}

	void face_search::load(const core::file_path path, const std::span<const uint8_t> data)
	{
		const auto record = unpack_record(data);
		if (!record) return;
		// docs/faces.md#31-turning-it-on-and-off: a kind face search does not read is put away, as off puts all away.
		if (!(record->video ? _videos.load() : _photos.load())) return;

		if (!_loading) _loading = std::make_shared<face_store>();
		_loading->add(path, *record);
	}

	void face_search::loaded()
	{
		auto store = _loading ? std::move(_loading) : std::make_shared<face_store>();
		_loading.reset();

		{
			// Under the lock turning face search off takes, so a load it overtook is not published after it.
			core::exclusive_lock lock(_rw);
			if (!_enabled.load()) return;
			store->publish();
			set_active_store(store);
			_store.store(std::move(store));
		}

		queue_overview();
		_index.invalidate_view(view_invalid::faces);
	}

	void face_search::configure(const bool photos, const bool videos, const bool paused)
	{
		_enabled = photos || videos;
		_photos = photos;
		_videos = videos;
		_paused = paused;
	}

	void face_search::turn_on(const bool photos, const bool videos)
	{
		if (!photos && !videos)
		{
			turn_off();
			return;
		}

		// Asked for other kinds, what is held is put away and loaded again: the store holds the kinds it reads alone.
		if (_enabled.load() && (_photos.load() != photos || _videos.load() != videos)) turn_off();
		_photos = photos;
		_videos = videos;
		if (_enabled.exchange(true)) return;

		const auto session = _session.fetch_add(1) + 1;
		{
			core::exclusive_lock lock(_rw);
			_progress = {};
			_failures.clear();
			_absent_folders.clear();
		}

		// What the index already holds is loaded first, on the database's thread, so turning face search on
		// again carries on where it stopped.
		_async.queue_database([this, session](database& db)
		{
			if (!is_current(session)) return;
			_loading.reset();
			db.load_face_records(*this);

			if (!is_current(session))
			{
				_loading.reset();
				return;
			}

			loaded();
			start_scan();
		});

		_index.invalidate_view(view_invalid::faces);
	}

	void face_search::turn_off()
	{
		// What was published is released once the lock is let go: a store can be large.
		face_store_const_ptr store;
		overview_ptr overview;

		{
			// Under the lock a load or a pass publishes by, so nothing either made is published after this.
			core::exclusive_lock lock(_rw);
			_enabled = false;
			_session.fetch_add(1);
			store = _store.exchange(nullptr);
			overview = _overview.exchange(nullptr);
			set_active_store(nullptr);
			set_active_overview(nullptr);
			_progress = {};
			_priority.reset();
			_results.clear();
			_marks.clear();
		}

		_wake.set();
		_index.invalidate_view(view_invalid::faces);
	}

	void face_search::delete_data()
	{
		turn_off();
		_async.queue_database([](database& db) { db.delete_face_records(); });
	}

	void face_search::pause(const bool paused)
	{
		_paused = paused;
		_wake.set();
		if (!paused) start_scan();
		_index.invalidate_view(view_invalid::faces);
	}

	void face_search::start_scan()
	{
		_scan_wanted = true;
		if (!_enabled.load() || _paused.load() || !_store.load() || !_index.is_init_complete()) return;
		if (_scanning.exchange(true)) return;

		const auto session = _session.load();
		_async.queue_async(async_queue::faces, [this, session] { scan(session); });
	}

	void face_search::prioritize(const core::file_path path)
	{
		if (!_enabled.load()) return;

		{
			core::exclusive_lock lock(_rw);
			_priority = path;
		}

		_wake.set();
	}

	// A picture that cannot be opened for now is tried again on a later pass, this many times a session at most.
	static constexpr int max_failures = 3;
	// Reads waiting for a worker, beyond the workers themselves, and the bytes they may hold.
	static constexpr int64_t max_bytes_in_flight = 256ll * 1024 * 1024;

	// The drive or share a folder is on - `E:\` or `\\server\share` - where its path names one, so a missing drive or an
	// offline share is looked for once rather than once for each of its folders. Empty where the path names none.
	static core::folder_path volume_of(const core::folder_path folder)
	{
		const auto text = folder.text().sv();
		if (text.size() >= 2 && text[1] == ':') return core::folder_path(std::string(text.substr(0, 2)) + "\\");
		if (text.size() > 2 && core::is_path_sep(text[0]) && core::is_path_sep(text[1]))
		{
			const auto server = text.find_first_of("\\/", 2);
			if (server == std::string_view::npos) return {};
			const auto share = text.find_first_of("\\/", server + 1);
			return core::folder_path(text.substr(0, share));
		}
		return {};
	}

	core::const_surface_ptr decode_for_faces(files& ff, const core::file_path path, const file_type_ref ft,
	                                         const std::span<const uint8_t> bytes, const sizei bound)
	{
		core::const_surface_ptr decoded;

		if (!bytes.empty())
		{
			// Fitted within the bound once decoded, so a JPEG is reduced as far as its fitted size allows rather
			// than as far as covering the whole bound does.
			still_decode_request request{bound, false, decode_intent::thumbnail};
			request.fit_within_target = true;
			auto surface = ff.decode_still(core::cspan{bytes.data(), bytes.size()}, request);
			if (surface) decoded = ff.fit_within(core::const_surface_ptr(std::move(surface)), bound);
		}

		// A raw file, a still too large to read whole, or one the bytes did not decode: the file's own load,
		// through LibRaw where it is raw.
		if (!decoded && ft)
		{
			const auto loaded = ff.load(path, true, decode_intent::thumbnail);
			if (loaded.success)
				decoded = ff.fit_within(loaded.to_surface(bound, false, {}, decode_intent::thumbnail), bound);
		}

		return decoded;
	}

	core::const_surface_ptr decode_face_source(files& ff, const core::file_path path, const file_type_ref ft,
	                                           const bool video, const uint32_t time_ms, const sizei bound)
	{
		if (!video)
		{
			const auto bytes = files::read_thumbnail_source(path, ft);
			return decode_for_faces(ff, path, ft, std::span<const uint8_t>(bytes.data(), bytes.size()), bound);
		}

		const auto read = read_face_frames(path, std::span(&time_ms, 1));
		return decode_face_frame(read, 0, bound);
	}

	face_frames read_face_frames(const core::file_path path, const std::span<const uint32_t> times_ms)
	{
		face_frames result;
		result.frames.resize(times_ms.size());

		av_format_decoder decoder;
		if (!decoder.open(path, media_intent::thumbnail)) return result;
		decoder.init_streams(-1, -1, false, true, false);
		result.codec = decoder.key_frame_codec();
		if (!result.codec) return result;

		// The key frame a face was found in is the first at or after its time, which was kept to the millisecond.
		for (size_t i = 0; i < times_ms.size(); ++i)
		{
			if (auto frame = decoder.read_key_frame(times_ms[i] / 1000.0 - 0.0005))
				result.frames[i] = std::make_shared<const av_key_frame>(std::move(*frame));
		}

		return result;
	}

	core::const_surface_ptr decode_face_frame(const face_frames& read, const size_t frame, const sizei bound)
	{
		if (!read.codec || frame >= read.frames.size() || !read.frames[frame]) return {};
		return av_decode_key_frame(*read.codec, *read.frames[frame], bound);
	}

	static void wait_on(core::thread_event& event, const uint32_t ms)
	{
		platform::wait_for({event, platform::event_exit}, ms, false);
	}

	void face_search::scan(const uint64_t session)
	{
		// A start asked for from here on - a resume, or face search turned on again, while this pass still drains
		// what it began - runs another pass once this one lets go.
		_scan_wanted = false;

		{
			struct clear_on_exit
			{
				std::atomic_bool& flag;
				~clear_on_exit() { flag = false; }
			} scanning{_scanning};

			scan_pass(session);
		}

		if (_scan_wanted.load() && !platform::is_closing) start_scan();
	}

	void face_search::scan_pass(const uint64_t session)
	{
		const auto store = _store.load();
		if (!store || !is_current(session)) return;

		struct candidate
		{
			core::file_path path;
			core::date_t modified;
			core::date_t created;
			bool video = false;
		};

		std::vector<candidate> candidates;
		scan_progress counted;
		const auto model = model_id();
		core::hash_map<core::folder_path, core::date_t, core::ihash, core::ieq> newest;
		// The store's runs whose pictures the collection still holds. This thread is the store's only writer, so
		// nothing is added while the collection is visited.
		const auto runs = store->run_count();
		std::vector<bool> seen(runs, false);

		// docs/faces.md#32-the-scan: folders an earlier pass found missing are looked for again - each drive or share
		// once - and their pictures wait while they are still missing rather than count as unreadable.
		core::hash_set<core::folder_path, core::ihash, core::ieq> absent;
		{
			core::hash_set<core::folder_path, core::ihash, core::ieq> looked_for;
			{
				core::shared_lock lock(_rw);
				looked_for = _absent_folders;
			}

			core::hash_map<core::folder_path, bool, core::ihash, core::ieq> volumes;
			for (const auto& folder : looked_for)
			{
				if (platform::is_closing) return;
				const auto volume = volume_of(folder);
				auto present = true;
				if (!volume.is_empty())
				{
					const auto [known, added] = volumes.try_emplace(volume, true);
					if (added) known->second = platform::exists(volume);
					present = known->second;
				}
				if (!present || !platform::exists(folder)) absent.insert(folder);
			}

			core::exclusive_lock lock(_rw);
			for (const auto& folder : looked_for)
				if (!absent.contains(folder)) _absent_folders.erase(folder);
		}

		_index.visit_collection_pictures(_photos.load(), _videos.load(),
		                                 [&](const core::file_path path, const df::index_file_item& file,
		                                     const bool video)
		{
			++counted.eligible;
			const auto modified = file.file_modified.load();

			if (const auto r = store->find(path))
			{
				if (*r < runs) seen[*r] = true;
				const auto& run = store->run(*r);
				if (run.model == model && run.stamp == modified &&
				    (run.detection_revision >= face_detection_revision || run.status == record_status::undecodable))
				{
					++counted.done;
					counted.faces += run.count;
					return;
				}
			}

			{
				core::shared_lock lock(_rw);
				if (const auto found = _failures.find(path); found != _failures.end() && found->second >= max_failures)
				{
					++counted.unreadable;
					return;
				}
			}

			if (absent.contains(path.folder()))
			{
				++counted.waiting;
				return;
			}

			const auto created = file.created();
			candidates.push_back({path, modified, created, video});
			auto& folder_newest = newest[path.folder()];
			if (created > folder_newest) folder_newest = created;
		});

		// A picture the index no longer holds - deleted, or moved or renamed and so read afresh under its new path -
		// has its faces put away, so no group, suggestion or page shows a face whose picture is gone. One the
		// collection holds but the scan passes over, such as a file now only in the cloud, keeps its faces.
		auto retired = false;

		for (uint32_t r = 0; r < runs; ++r)
		{
			if (seen[r]) continue;
			const auto& run = store->run(r);
			if (!run.is_current() || _index.find_item(run.path).ft) continue;
			store->retire(run.path);
			retired = true;
		}

		if (retired)
		{
			store->publish();
			queue_overview();
		}

		// The folders holding the newest pictures first, each in path order, so reads stay together on disk and
		// recent pictures are answered early.
		std::ranges::stable_sort(candidates, [&newest](const candidate& a, const candidate& b)
		{
			const auto fa = a.path.folder();
			const auto fb = b.path.folder();
			if (fa != fb)
			{
				const auto na = newest[fa];
				const auto nb = newest[fb];
				if (na != nb) return na > nb;
				return icmp(fa.text(), fb.text()) < 0;
			}
			return icmp(a.path.name(), b.path.name()) < 0;
		});

		{
			core::exclusive_lock lock(_rw);
			_progress.eligible = counted.eligible;
			_progress.done = counted.done;
			_progress.unreadable = counted.unreadable;
			_progress.waiting = counted.waiting;
			_progress.faces = counted.faces;
			_progress.running = !candidates.empty();
			_rate_start_ms = static_cast<double>(core::now_ms());
			_rate_count = 0;
		}

		_index.invalidate_view(view_invalid::faces);

		const auto max_in_flight = index_state::face_threads() * 2;
		const auto started_ms = core::now_ms();
		auto last_apply_ms = started_ms;
		auto last_overview_ms = started_ms;
		// What the workers have read goes into the store, and the window is told, a few times a second rather than as
		// each picture is read: told for each, the Faces view would redraw a hundred times a second through a scan.
		constexpr int64_t apply_interval_ms = 250;
		const auto apply_due = [&]
		{
			if (const auto now = core::now_ms(); now - last_apply_ms >= apply_interval_ms)
			{
				apply_results(session);
				last_apply_ms = now;
			}
		};
		std::vector<bool> taken(candidates.size());
		size_t next = 0;
		scan_storage_probe storage;
		core::folder_path source_folder;
		auto seek_free = false;

		while (is_current(session) && !platform::is_closing && !_paused.load())
		{
			// Let on-demand item scans go first. The permit below coordinates the background disk reads.
			if (_index.scanning_items.load() > 0 || _in_flight.load() >= max_in_flight ||
			    _bytes_in_flight.load() > max_bytes_in_flight)
			{
				apply_due();
				wait_on(_wake, 100);
				continue;
			}

			// The picture on screen first, out of turn, looked for without holding the lock the UI thread reads by.
			std::optional<core::file_path> wanted;
			{
				core::exclusive_lock lock(_rw);
				wanted = std::exchange(_priority, std::nullopt);
			}

			std::optional<size_t> pick;
			if (wanted)
			{
				for (size_t i = 0; i < candidates.size(); ++i)
				{
					if (!taken[i] && candidates[i].path == *wanted)
					{
						pick = i;
						break;
					}
				}
			}

			if (!pick)
			{
				while (next < candidates.size() && taken[next]) ++next;
				if (next >= candidates.size()) break;
				pick = next;
			}

			const auto& c = candidates[*pick];
			{
				// Its folder was found missing since the pass began: it waits with the other pictures there.
				core::shared_lock lock(_rw);
				if (_absent_folders.contains(c.path.folder()))
				{
					lock.unlock();
					taken[*pick] = true;
					core::exclusive_lock counting(_rw);
					++_progress.waiting;
					continue;
				}
			}
			if (const auto folder = c.path.folder(); folder != source_folder)
			{
				source_folder = folder;
				seek_free = storage.seek_free(folder);
			}

			auto permit = _scan_io.try_start_face(seek_free);
			while (!permit && is_current(session) && !platform::is_closing && !_paused.load())
			{
				apply_due();
				wait_on(_wake, 100);
				{
					core::shared_lock lock(_rw);
					if (_priority && *_priority != c.path) break;
				}
				permit = _scan_io.try_start_face(seek_free);
			}
			if (!is_current(session) || platform::is_closing || _paused.load()) break;
			if (!permit) continue;
			taken[*pick] = true;

			const auto file = _index.find_scanned_item(c.path);
			if (!file.ft || file.file_modified.load() != c.modified ||
			    (file.flags && (df::index_item_flags::is_sidecar | df::index_item_flags::is_offline)))
				continue;

			if (c.video)
			{
				// Read here, a key frame at a time, while the permit is held; decoded on the workers after.
				read_video(session, c.path, c.modified, [&]
				{
					apply_due();
					wait_on(_wake, 100);
				});
				apply_due();
				continue;
			}

			std::vector<named_region> regions;
			if (const auto md = file.metadata.load())
				regions = unpack_regions(md->face_regions, md->dimensions(), md->orientation);

			// Shared with the worker rather than copied for it: a picture's bytes can be tens of megabytes.
			const auto bytes = std::make_shared<const core::blob>(files::read_thumbnail_source(c.path, file.ft));
			_bytes_in_flight += static_cast<int64_t>(bytes->size());
			++_in_flight;

			_async.queue_async(async_queue::faces_work,
			                   [this, session, path = c.path, ft = file.ft, modified = c.modified, bytes,
			                    permit = std::move(permit), regions = std::move(regions)]() mutable
			{
				const auto held = std::move(permit); // RAW and other fallback loads can still read on this worker.
				read_one(session, path, ft, modified, *bytes, std::move(regions));
			});

			apply_due();

			// Include newly detected faces while a long scan is running.
			const auto now = core::now_ms();
			if (now - last_overview_ms > 60000)
			{
				queue_overview();
				last_overview_ms = now;
			}
		}

		while (_in_flight.load() > 0 && !platform::is_closing)
		{
			apply_due();
			wait_on(_wake, 100);
		}

		apply_results(session);

		{
			core::exclusive_lock lock(_rw);
			_progress.running = false;
		}

		queue_overview();
		_index.invalidate_view(view_invalid::faces);
	}

	// One detector, embedder and decoder to a worker, made on its first picture and kept for the next.
	struct face_worker
	{
		libmug::face_detector detector;
		libmug::face_embedder embedder;
		std::unique_ptr<files> ff;
	};

	static face_worker& this_worker()
	{
		thread_local face_worker w;
		return w;
	}

	void face_search::add_result(const core::file_path path, std::optional<face_record> record, const uint64_t session)
	{
		core::exclusive_lock lock(_rw);
		_results.push_back({path, std::move(record), session});
	}

	void face_search::read_one(const uint64_t session, const core::file_path path, const file_type_ref ft,
	                           const core::date_t modified, const core::blob& bytes, std::vector<named_region> regions)
	{
		auto& w = this_worker();
		std::optional<face_record> record;
		const auto byte_count = static_cast<int64_t>(bytes.size());

		try
		{
			if (!w.ff) w.ff = std::make_unique<files>();

			if (is_current(session) && w.embedder.ready())
			{
				const auto& t = defaults();
				const auto decoded = decode_for_faces(*w.ff, path, ft,
				                                      std::span<const uint8_t>(bytes.data(), bytes.size()),
				                                      {t.decode_edge, t.decode_edge});

				if (decoded)
				{
					record = read_record(w.detector, w.embedder, decoded, regions, t);
				}
				else if (!bytes.empty() || platform::open_file(path, platform::file_open_mode::read))
				{
					// It opened, so it is the file that is at fault, and it is not read again until it changes.
					record = face_record{};
					record->model = model_id();
					record->status = record_status::undecodable;
				}

				if (record) record->stamp = modified;
			}
		}
		catch (const std::exception& e)
		{
			core::log(__FUNCTION__, e.what());
			record.reset();
		}

		add_result(path, std::move(record), session);
		_bytes_in_flight -= byte_count;
		--_in_flight;
		_wake.set();
	}

	// docs/faces.md#33-videos: a video being read - its frames decoded so far - made one record once the scan has read
	// its last key frame and the workers have decoded every one.
	struct face_search::video_job
	{
		core::file_path path;
		core::date_t modified;
		uint64_t session = 0;
		double length = 0.0;
		// One for the reading, and one for each frame read and not yet decoded: whoever brings it to nothing makes the
		// record.
		std::atomic<int> pending = 1;
		// Paused, turned off or closing: nothing is made of what was read, and a later pass reads the video whole.
		std::atomic_bool abandoned = false;
		// It could not be read for now - its folder gone, or no worker able to read faces - and is tried again later.
		std::atomic_bool unreadable = false;
		core::mutex rw;
		_Guarded_by_(rw) std::vector<video_frame> frames;
	};

	void face_search::read_video(const uint64_t session, const core::file_path path, const core::date_t modified,
	                             const std::function<void()>& keep_up)
	{
		av_format_decoder decoder;
		const auto opened = decoder.open(path, media_intent::thumbnail);
		if (opened) decoder.init_streams(-1, -1, false, true, false);
		const auto codec = opened ? decoder.key_frame_codec() : nullptr;

		if (!codec)
		{
			// It opened, or could be, so it is the file that is at fault, or it holds no picture this build decodes: it
			// is not read again until it changes. One that cannot be opened is tried again on a later pass.
			std::optional<face_record> record;
			if (opened || platform::open_file(path, platform::file_open_mode::read))
			{
				record = face_record{};
				record->model = model_id();
				record->video = true;
				record->status = record_status::undecodable;
				record->stamp = modified;
			}
			add_result(path, std::move(record), session);
			_wake.set();
			return;
		}

		const auto job = std::make_shared<video_job>();
		job->path = path;
		job->modified = modified;
		job->session = session;
		job->length = std::max(0.0, decoder.end_time() - decoder.start_time());

		const auto max_in_flight = index_state::face_threads() * 2;
		const auto stopped = [&]
		{
			return !is_current(session) || platform::is_closing || _paused.load();
		};
		auto in_order = false;
		auto reads = 0;

		read_video_slices(job->length, [&](const double from) -> std::optional<double>
		{
			// Each frame waits for room among the workers, within the bounds pictures wait in.
			while (!stopped() && (_in_flight.load() >= max_in_flight || _bytes_in_flight.load() > max_bytes_in_flight))
				keep_up();

			if (stopped())
			{
				job->abandoned = true;
				return std::nullopt;
			}

			auto frame = decoder.read_key_frame(from, in_order);
			// A file that cannot seek is read in order from its start.
			if (!frame && reads == 0 && !in_order)
			{
				in_order = true;
				frame = decoder.read_key_frame(from, true);
			}
			++reads;
			if (!frame) return std::nullopt;

			const auto time = frame->time;
			const auto bytes = static_cast<int64_t>(frame->bytes());
			_bytes_in_flight += bytes;
			++_in_flight;
			++job->pending;

			_async.queue_async(async_queue::faces_work, [this, job, codec, frame = std::move(*frame), bytes]
			{
				read_video_frame(job, codec, frame);
				_bytes_in_flight -= bytes;
				--_in_flight;
				_wake.set();
			});

			return time;
		});

		// A folder gone while it was read - a drive pulled, a share gone offline - leaves it to wait for the folder.
		if (!job->abandoned.load() && !platform::exists(path.folder())) job->unreadable = true;
		if (--job->pending == 0) finish_video(*job);
	}

	void face_search::read_video_frame(const std::shared_ptr<video_job>& job, const av_key_frame_codec_ptr& codec,
	                                   const av_key_frame& frame)
	{
		auto& w = this_worker();
		video_frame read{frame.time, {}};
		read.found.status = record_status::undecodable;

		try
		{
			if (!job->abandoned.load() && is_current(job->session))
			{
				if (!w.embedder.ready())
				{
					job->unreadable = true;
				}
				else
				{
					const auto& t = defaults();
					if (const auto decoded = av_decode_key_frame(*codec, frame, {t.decode_edge, t.decode_edge}))
						read.found = read_record(w.detector, w.embedder, decoded, {}, t);
				}
			}
		}
		catch (const std::exception& e)
		{
			core::log(__FUNCTION__, e.what());
		}

		{
			core::exclusive_lock lock(job->rw);
			job->frames.push_back(std::move(read));
		}

		if (--job->pending == 0) finish_video(*job);
	}

	void face_search::finish_video(video_job& job)
	{
		if (job.abandoned.load() || !is_current(job.session)) return;

		if (job.unreadable.load())
		{
			add_result(job.path, std::nullopt, job.session);
			return;
		}

		std::vector<video_frame> frames;
		{
			core::exclusive_lock lock(job.rw);
			frames = std::move(job.frames);
		}

		auto record = merge_video_frames(std::move(frames), job.length);
		record.stamp = job.modified;
		add_result(job.path, std::move(record), job.session);
	}

	size_t face_search::apply_results(const uint64_t session)
	{
		apply_marks();

		std::vector<result> results;
		{
			core::exclusive_lock lock(_rw);
			std::swap(results, _results);
		}

		const auto store = _store.load();
		if (results.empty() || !store || !is_current(session)) return 0;

		size_t applied = 0;
		uint32_t faces_added = 0;
		uint32_t failed = 0;
		uint32_t waiting = 0;

		for (auto& r : results)
		{
			if (r.session != session) continue;

			if (!r.record)
			{
				// docs/faces.md#32-the-scan: a picture whose folder is gone - a drive disconnected, a share offline -
				// waits for it rather than counting as unreadable, and is read once the folder is back.
				const auto folder = r.path.folder();
				const auto missing = !platform::exists(folder);
				core::exclusive_lock lock(_rw);
				if (missing)
				{
					_absent_folders.insert(folder);
					++waiting;
				}
				else
				{
					++_failures[r.path];
					++failed;
				}
				continue;
			}

			if (const auto previous = store->record(r.path))
			{
				// Read again with its pixels as they were - by a new model, or a corrected way of finding faces - the
				// marks the user put on its faces stand, on the faces found again in their places.
				if (previous->stamp == r.record->stamp) carry_marks(*previous, *r.record);
				// Read again after its pixels changed - turned, cropped or straightened - the face book's marks go to the
				// faces that resemble theirs, as only faces one model described can be compared.
				else if (previous->model == r.record->model)
					_book.follow(r.path, previous->faces, r.record->faces, r.record->model);
			}

			if (store->add(r.path, *r.record))
			{
				_index.save_face_record(r.path, pack_record(*r.record), session);
				faces_added += static_cast<uint32_t>(r.record->faces.size());
				++applied;
			}
		}

		store->publish();

		{
			core::exclusive_lock lock(_rw);
			_progress.done += static_cast<uint32_t>(applied);
			_progress.faces += faces_added;
			_progress.unreadable += failed;
			_progress.waiting += waiting;
			_rate_count += applied;
			const auto elapsed = (static_cast<double>(core::now_ms()) - _rate_start_ms) / 1000.0;
			if (elapsed > 1.0) _progress.rate = static_cast<double>(_rate_count) / elapsed;
		}

		_index.invalidate_view(view_invalid::faces);
		return applied;
	}

	// ---- Restamps ------------------------------------------------------------------------------------

	void face_search::restamp(const core::file_path path, const core::date_t from, const core::date_t to,
	                          const core::orientation orientation_from, const core::orientation orientation_to)
	{
		if (!_store.load()) return;

		{
			core::exclusive_lock lock(_rw);
			_marks.push_back({path, from, to, orientation_from, orientation_to});
		}

		queue_marks();
	}

	void face_search::queue_marks()
	{
		// The store has one writer: a running scan applies the marks between pictures, and otherwise the scan's
		// own thread does.
		_wake.set();
		_async.queue_async(async_queue::faces, [this]
		{
			apply_marks();
			queue_overview();
		});
	}

	void face_search::apply_marks()
	{
		std::vector<mark> marks;
		{
			core::exclusive_lock lock(_rw);
			std::swap(marks, _marks);
		}

		const auto store = _store.load();
		if (marks.empty() || !store) return;

		for (const auto& m : marks)
		{
			// The database stamps its own record, so a restamp changes only the store's.
			auto record = store->record(m.path);
			if (!record || record->stamp != m.from) continue;
			record->stamp = m.to;
			if (m.orientation_from != m.orientation_to)
				*record = turn_record(std::move(*record), m.orientation_from, m.orientation_to);
			store->add(m.path, *record);
		}

		store->publish();
		_index.invalidate_view(view_invalid::faces);
	}
	// ---- The overview ------------------------------------------------------------------------------

	// docs/faces.md#72-suggestions: a pass's comparisons in pieces, on the overview's thread and on whichever faces_work
	// threads are free to take a piece. This thread takes every piece nobody else has, then waits only for those others
	// hold, so a thread busy reading a picture never holds a pass up; a helper starting after the last piece is taken
	// finds nothing left and touches nothing of the pass.
	static parallel_for lend_faces_work(async_strategy& async)
	{
		return [&async](const size_t count, const std::function<void(size_t, size_t)>& body)
		{
			// Enough pieces for every thread to take several, so one that starts late still finds work.
			const auto threads = static_cast<size_t>(std::max(1, index_state::face_threads())) + 1;
			const auto piece = std::max<size_t>(1, count / (threads * 8));
			struct pass
			{
				const std::function<void(size_t, size_t)>* body = nullptr;
				size_t count = 0;
				size_t piece = 0;
				size_t pieces = 0;
				std::atomic_size_t next = 0;
				std::atomic_size_t done = 0;
				std::atomic_bool failed = false;
			};
			const auto p = std::make_shared<pass>();
			p->body = &body;
			p->count = count;
			p->piece = piece;
			p->pieces = (count + piece - 1) / piece;
			const auto work = [p]
			{
				for (auto i = p->next.fetch_add(1); i < p->pieces; i = p->next.fetch_add(1))
				{
					try
					{
						(*p->body)(i * p->piece, std::min(p->count, (i + 1) * p->piece));
					}
					catch (...)
					{
						p->failed = true;
					}
					if (p->done.fetch_add(1) + 1 == p->pieces) p->done.notify_all();
				}
			};

			const auto helpers = std::min(threads - 1, p->pieces ? p->pieces - 1 : 0);
			for (size_t h = 0; h < helpers; ++h) async.queue_async(async_queue::faces_work, work);
			work();
			for (auto d = p->done.load(); d < p->pieces; d = p->done.load()) p->done.wait(d);
			if (p->failed.load()) throw std::runtime_error("comparing faces failed");
		};
	}

	void face_search::queue_overview_if_stale()
	{
		const auto store = _store.load();
		if (!store) return;

		const auto o = _overview.load();
		if (_names_changed.load() || !o || o->store != store || o->generation != store->generation() ||
		    o->book_generation != _book.generation())
			queue_overview();
	}

	void face_search::bind_legacy_signatures()
	{
		const auto store = _store.load();
		if (!store || !_book.unportable_count()) return;
		const auto version = _book.generation();
		const auto session = _session.load();
		std::vector<std::pair<face_ref, face_mark>> bindings;
		for (const auto& [path, marks] : _book.all_marks())
		{
			if (!is_current(session) || platform::is_closing) return;
			if (std::ranges::none_of(marks, [](const auto& m) { return !m.signature; })) continue;
			const auto r = store->find(path);
			if (!r) continue;
			const auto& run = store->run(*r);
			if (run.model != model_id() || !run.is_current()) continue;
			std::vector<rectd> boxes;
			for (uint32_t i = 0; i < run.count; ++i) boxes.push_back(store->entry(run.first + i).bounds());
			const auto matched = match_marks(marks, boxes);
			for (size_t m = 0; m < matched.size(); ++m)
			{
				if (matched[m] < 0 || marks[m].signature) continue;
				const auto face = run.first + static_cast<uint32_t>(matched[m]);
				if (store->entry(face).has_hash)
					bindings.emplace_back(face_ref{path, marks[m].box, marks[m].position, store->signature(face)},
					                      marks[m]);
			}
		}
		if (!bindings.empty() && is_current(session)) _book.bind_signatures(bindings, version);
	}

	void face_search::queue_overview()
	{
		_overview_wanted = true;
		if (_overview_running.exchange(true)) return;
		// Readers can show that their overview is being updated.
		_index.invalidate_view(view_invalid::faces);

		_async.queue_async(async_queue::faces_overview, [this, function = __FUNCTION__]
		{
			while (_overview_wanted.exchange(false) && !platform::is_closing)
			{
				const auto store = _store.load();
				if (!store) break;

				// Before the names are read: a change reported while they are is caught by the next.
				_names_changed = false;

				const auto session = _session.load();
				const auto regions = [this](const core::file_path path)
				{
					const auto item = _index.find_item(path);
					const auto md = item.metadata.load();
					const auto created = item.created();
					return picture_evidence{md ? unpack_regions(md->face_regions, md->dimensions(), md->orientation)
					                           : std::vector<named_region>{},
					                        created.is_valid() ? created.year() : 0, md ? md->people : core::cached{}};
				};

				const auto started_ms = core::now_ms();
				bind_legacy_signatures();
				// A pass the book has moved on from could never be published; the edit has asked for the next.
				const auto book_generation = _book.generation();
				overview_ptr made;
				try
				{
					made = make_overview(store, regions, _book, [this, session, book_generation] {
						return !is_current(session) || platform::is_closing || _book.generation() != book_generation;
					}, defaults(), lend_faces_work(_async));
				}
				catch (const std::exception& e)
				{
					core::log(function, e.what());
				}

				// A collection large enough to take a while says how long, for whoever reads the log.
				if (const auto took_ms = core::now_ms() - started_ms; made && took_ms >= 1000)
					core::log(function, std::format("Compared {} faces in {} ms", store->face_count(), took_ms));

				if (!made) continue;
				overview_ptr replaced;
				auto published = false;

				{
					// Under the lock turning face search off takes, so an overview is never published after it.
					core::exclusive_lock lock(_rw);
					if (is_current(session) && made->book_generation == _book.generation())
					{
						set_active_overview(made);
						replaced = _overview.exchange(std::move(made));
						published = true;
					}
				}

				if (published) _index.invalidate_view(view_invalid::faces);
			}

			_overview_running = false;
			// Asked for again between the loop's last look and the flag's clearing.
			if (_overview_wanted.load()) queue_overview();
			else _index.invalidate_view(view_invalid::faces);
		});
	}
}
