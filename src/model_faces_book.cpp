// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The face book - the named people and face evidence the user keeps, handed to the face database
// as each change is made, and written to and read from a CSV file. Owned by docs/faces.md#6-the-face-book.

#include "pch.h"

#include "model_faces_book.h"

#include <charconv>

namespace faces
{
	const kept_person* roster::person(const uint32_t id) const
	{
		const auto found = std::ranges::lower_bound(people, id, {}, &kept_person::id);
		return found != people.end() && found->id == id ? &*found : nullptr;
	}

	std::string_view roster::canonical_name(const std::string_view name) const
	{
		if (aliases.empty()) return name;
		const auto found = aliases.find(core::trim(name));
		if (found != aliases.end())
			if (const auto* kept = person(found->second)) return kept->name;
		return name;
	}

	const kept_person* roster::named(const std::string_view name) const
	{
		const auto trimmed = core::trim(name);
		if (trimmed.empty()) return nullptr;
		if (const auto alias = aliases.find(trimmed); alias != aliases.end()) return person(alias->second);
		const auto found = std::ranges::find_if(people, [trimmed](const kept_person& p)
		{ return core::icmp(p.name, trimmed) == 0; });
		return found != people.end() ? &*found : nullptr;
	}

	static double box_union_share(const rectd& a, const rectd& b)
	{
		const auto width = std::min(a.right(), b.right()) - std::max(a.left(), b.left());
		const auto height = std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top());
		if (width <= 0.0 || height <= 0.0) return 0.0;
		const auto intersection = width * height;
		const auto total = a.Width * a.Height + b.Width * b.Height - intersection;
		return total > 0.0 ? intersection / total : 0.0;
	}

	// The least share of their union a mark's box and a face's overlap by to be one face. The same face measured again
	// moves by far less than this; a neighbour overlaps it by far less.
	constexpr auto least_share = 0.5;

	// A face read again from the same picture after its pixels changed resembles itself far more than this; two people,
	// even brothers and sisters, seldom do.
	constexpr auto same_face = 0.8f;

	static std::optional<size_t> mark_index(const std::span<const face_mark> marks, const rectd& box,
	                                        const int position)
	{
		std::optional<size_t> best;
		auto best_share = least_share;

		for (size_t i = 0; i < marks.size(); ++i)
		{
			const auto share = box_union_share(marks[i].box, box);
			if (share < least_share) continue;
			if (!best || share > best_share || (share == best_share && marks[i].position == position))
			{
				best = i;
				best_share = share;
			}
		}

		return best;
	}

	const face_mark* find_mark(const std::span<const face_mark> marks, const rectd& box, const int position)
	{
		const auto found = mark_index(marks, box, position);
		return found ? &marks[*found] : nullptr;
	}

	static std::vector<int> match_positions(const std::span<const face_mark> marks, const std::span<const rectd> boxes,
	                                        const std::span<const int> positions,
	                                        const std::span<const std::optional<face_signature>> signatures = {})
	{
		struct pair
		{
			double share;
			int mark;
			int face;
		};

		std::vector<pair> pairs;
		for (auto m = 0; m < static_cast<int>(marks.size()); ++m)
		{
			for (auto f = 0; f < static_cast<int>(boxes.size()); ++f)
			{
				if (!signatures.empty() && marks[m].signature && marks[m].signature != signatures[f]) continue;
				const auto share = box_union_share(marks[m].box, boxes[f]);
				const int position = positions.empty() ? f : positions[f];
				if (share >= least_share) pairs.push_back({share + (marks[m].position == position ? 1e-6 : 0.0), m, f});
			}
		}

		std::ranges::sort(pairs, [](const pair& a, const pair& b)
		{
			if (a.share != b.share) return a.share > b.share;
			return a.mark != b.mark ? a.mark < b.mark : a.face < b.face;
		});

		std::vector<int> result(marks.size(), -1);
		std::vector<bool> taken(boxes.size(), false);

		for (const auto& p : pairs)
		{
			if (result[p.mark] >= 0 || taken[p.face]) continue;
			result[p.mark] = p.face;
			taken[p.face] = true;
		}

		return result;
	}

	std::vector<int> match_marks(const std::span<const face_mark> marks, const std::span<const rectd> boxes)
	{
		return match_positions(marks, boxes, {});
	}

	face_book::face_book() : _roster(std::make_shared<roster>())
	{
	}

	void face_book::publish(std::unique_lock<core::mutex>& lock, book_write write)
	{
		if (write.clear)
		{
			_unsigned_by_picture.clear();
			_unportable = 0;
		}
		for (const auto& [path, marks] : write.pictures) count_unsigned(path, marks);
		// Advance the generation with the mutation, not later on the way to the database. A worker can change a
		// different picture and start using a newly created person between an undo check and a delayed notification.
		write.generation = _generation.fetch_add(1, std::memory_order_acq_rel) + 1;
		if (!_persist || write.empty()) return;
		_pending.push_back(std::move(write));
		if (_publishing) return;
		_publishing = true;
		const core::scope_exit done([&]
		{
			if (!lock.owns_lock()) lock.lock();
			_publishing = false;
		});
		// One publisher preserves commit order even when writers race; callbacks run outside the book lock.
		while (!_pending.empty())
		{
			auto next = std::move(_pending.front());
			_pending.pop_front();
			lock.unlock();
			_persist(std::move(next));
			lock.lock();
		}
	}

	uint32_t face_book::take_id()
	{
		return _next_id <= UINT32_MAX ? static_cast<uint32_t>(_next_id++) : 0;
	}

	void face_book::count_unsigned(const core::file_path path, const std::span<const face_mark> marks)
	{
		const auto count = static_cast<size_t>(
			std::ranges::count_if(marks, [](const auto& m) { return !m.signature; }));
		const auto old = _unsigned_by_picture.find(path);
		if (old != _unsigned_by_picture.end()) _unportable -= old->second;
		if (count) _unsigned_by_picture[path] = count;
		else if (old != _unsigned_by_picture.end()) _unsigned_by_picture.erase(old);
		_unportable += count;
	}

	void face_book::load(roster people, marks_by_picture marks, signature_marks signatures)
	{
		std::ranges::sort(people.people, {}, &kept_person::id);
		const auto next = people.people.empty() ? uint64_t{1} : uint64_t{people.people.back().id} + 1;
		std::erase_if(signatures, [](const auto& entry) { return !entry.first.valid(); });
		for (auto& [key, mark] : signatures)
		{
			mark.box = {};
			mark.position = 0;
			mark.signature.reset();
		}
		for (auto& [path, picture] : marks)
			for (auto& m : picture)
				if (m.signature && !signatures.contains(*m.signature)) m.signature.reset();

		{
			core::exclusive_lock lock(_rw);
			people.loaded_at = _generation.fetch_add(1, std::memory_order_acq_rel) + 1;
			_roster = std::make_shared<roster>(std::move(people));
			_marks = std::move(marks);
			_signatures = std::move(signatures);
			_unsigned_by_picture.clear();
			_unportable = 0;
			for (const auto& [path, picture] : _marks) count_unsigned(path, picture);
			_next_id = next;
		}
	}

	roster_ptr face_book::people() const
	{
		core::shared_lock lock(_rw);
		return _roster;
	}

	book_write face_book::whole() const
	{
		core::shared_lock lock(_rw);
		book_write write;
		write.generation = generation();
		write.clear = true;
		write.people = _roster->people;
		for (const auto& [name, id] : _roster->aliases) write.aliases.emplace_back(name, id);
		for (const auto& [key, mark] : _signatures) write.signatures.emplace(key, mark);
		for (const auto& [path, marks] : _marks)
			if (!marks.empty()) write.pictures.emplace_back(path, marks);
		return write;
	}

	static void project(face_mark& mark, const face_mark& exact)
	{
		mark.person = exact.person;
		mark.hidden = exact.hidden;
		mark.rejected = exact.rejected;
	}

	static void project(std::vector<face_mark>& marks, const signature_marks& exact)
	{
		for (auto& m : marks)
			if (m.signature)
				if (const auto i = exact.find(*m.signature); i != exact.end()) project(m, i->second);
	}

	std::vector<face_mark> face_book::marks(const core::file_path path) const
	{
		core::shared_lock lock(_rw);
		const auto found = _marks.find(path);
		auto result = found != _marks.end() ? found->second : std::vector<face_mark>{};
		project(result, _signatures);
		return result;
	}

	marks_by_picture face_book::all_marks() const
	{
		core::shared_lock lock(_rw);
		auto result = _marks;
		for (auto& [path, marks] : result) project(marks, _signatures);
		return result;
	}

	signature_marks face_book::all_signatures() const
	{
		core::shared_lock lock(_rw);
		return _signatures;
	}

	std::optional<face_mark> face_book::signature_mark(const face_signature& key) const
	{
		core::shared_lock lock(_rw);
		const auto i = _signatures.find(key);
		return i == _signatures.end() ? std::nullopt : std::optional(i->second);
	}

	size_t face_book::unportable_count() const
	{
		core::shared_lock lock(_rw);
		return _unportable;
	}

	size_t face_book::mark_count() const
	{
		core::shared_lock lock(_rw);
		return _unportable + _signatures.size();
	}

	uint32_t face_book::create(const std::string_view name)
	{
		const auto trimmed = core::trim(name);
		if (trimmed.empty() || _read_only) return 0;
		std::unique_lock lock(_rw);
		if (const auto* known = _roster->named(trimmed)) return known->id;
		auto changed = std::make_shared<roster>(*_roster);
		const kept_person person{take_id(), std::string(trimmed)};
		if (!person.id) return 0;
		changed->people.push_back(person);
		_roster = std::move(changed);
		book_write write;
		write.people.push_back(person);
		publish(lock, std::move(write));
		return person.id;
	}

	bool face_book::rename(const uint32_t person, const std::string_view name, const bool keep_former_name)
	{
		const auto trimmed = core::trim(name);
		std::unique_lock lock(_rw);
		const auto* was = _roster->person(person);
		if (!was || trimmed.empty() || _read_only) return false;
		if (const auto* other = _roster->named(trimmed); other && other->id != person) return false;
		if (was->name == trimmed) return true;
		const auto former = was->name;
		auto changed = std::make_shared<roster>(*_roster);
		auto& renamed = changed->people[static_cast<size_t>(was - _roster->people.data())];
		renamed.name = trimmed;
		book_write write;
		if (const auto alias = changed->aliases.find(trimmed); alias != changed->aliases.end())
		{
			write.aliases.emplace_back(alias->first, 0);
			changed->aliases.erase(alias);
		}
		if (keep_former_name && core::icmp(former, trimmed) != 0)
		{
			changed->aliases.insert_or_assign(former, person);
			write.aliases.emplace_back(former, person);
		}
		write.people.push_back(renamed);
		_roster = std::move(changed);
		publish(lock, std::move(write));
		return true;
	}

	static void add_rejection(face_mark& mark, const uint32_t id)
	{
		const auto pos = std::ranges::lower_bound(mark.rejected, id);
		if (pos == mark.rejected.end() || *pos != id) mark.rejected.insert(pos, id);
	}

	// Symmetric union of compatible legacy evidence. A conflict needs a deliberate placement, not traversal order.
	static bool join_legacy(face_mark& merged, const face_mark& other)
	{
		if ((merged.person && other.person && merged.person != other.person) ||
		    std::ranges::binary_search(merged.rejected, other.person) ||
		    std::ranges::binary_search(other.rejected, merged.person))
			return false;
		if (!merged.person) merged.person = other.person;
		merged.hidden |= other.hidden;
		for (const auto id : other.rejected) add_rejection(merged, id);
		return true;
	}

	bool face_book::bind_signatures(const std::span<const std::pair<face_ref, face_mark>> bindings,
	                                const uint64_t expected_generation)
	{
		std::unique_lock lock(_rw);
		if (generation() != expected_generation || _read_only) return false;
		struct group
		{
			face_mark merged{};
			bool conflict = false;
			std::vector<std::pair<core::file_path, face_mark*>> marks;
		};
		std::map<face_signature, group> groups;
		std::map<face_mark*, face_signature> claimed;
		for (const auto& [ref, was] : bindings)
		{
			if (was.signature || !ref.signature || !ref.signature->valid()) continue;
			const auto pic = _marks.find(ref.path);
			if (pic == _marks.end()) continue;
			const auto it = std::ranges::find(pic->second, was);
			if (it == pic->second.end()) continue;
			auto& g = groups[*ref.signature];
			if (std::find(std::next(it), pic->second.end(), was) != pic->second.end()) g.conflict = true;
			const auto [c, unique] = claimed.emplace(&*it, *ref.signature);
			if (!unique && c->second != *ref.signature) groups[c->second].conflict = g.conflict = true;
			if (!join_legacy(g.merged, was)) g.conflict = true;
			g.marks.emplace_back(ref.path, &*it);
		}
		book_write write;
		core::hash_set<core::file_path, core::ihash, core::ieq> touched;
		for (auto& [key, g] : groups)
		{
			if (g.conflict) continue;
			if (const auto existing = _signatures.find(key); existing != _signatures.end())
			{
				// Backfill never edits an existing exact decision. Keep contrary legacy evidence local and report it.
				auto merged = existing->second;
				if (!join_legacy(merged, g.merged) || merged != existing->second) continue;
			}
			else
			{
				_signatures.emplace(key, g.merged);
				write.signatures.emplace(key, g.merged);
			}
			for (const auto& [path, m] : g.marks)
			{
				m->signature = key;
				touched.insert(path);
			}
		}
		for (const auto path : touched) write.pictures.emplace_back(path, _marks.at(path));
		if (write.empty()) return false;
		publish(lock, std::move(write));
		return true;
	}

	std::optional<book_edit> face_book::combine(std::string_view from, std::string_view into,
	                                            const uint64_t expected_generation)
	{
		from = core::trim(from);
		into = core::trim(into);
		std::unique_lock lock(_rw);
		if (generation() != expected_generation || _read_only || from.empty() || into.empty() ||
		    core::icmp(from, into) == 0)
			return {};
		const auto* source = _roster->named(from);
		const auto* target = _roster->named(into);
		if (source && source == target) return {};

		book_edit edit;
		edit.before = _roster;
		auto changed = std::make_shared<roster>(*_roster);
		book_write write;
		const auto id = target ? target->id : take_id();
		if (!id) return {};
		if (!target)
		{
			changed->people.push_back({id, std::string(into)});
			write.people.push_back(changed->people.back());
			edit.reverse.people_removed.push_back(id);
		}
		if (source)
		{
			from = source->name;
			// The copy is in the same ID order as the original (any new target was appended).
			changed->people.erase(changed->people.begin() + (source - _roster->people.data()));
			write.people_removed.push_back(source->id);
			edit.reverse.people.push_back(*source);
			for (auto& [name, owner] : changed->aliases)
			{
				if (owner != source->id) continue;
				edit.reverse.aliases.emplace_back(name, owner);
				owner = id;
				write.aliases.emplace_back(name, owner);
			}
			const auto remap = [&](face_mark& mark)
			{
				if (mark.person == source->id) mark.person = id;
				if (std::erase(mark.rejected, source->id) && mark.person != id) add_rejection(mark, id);
				if (mark.person == id) std::erase(mark.rejected, id);
			};
			for (auto& [key, mark] : _signatures)
			{
				if (mark.person != source->id && !std::ranges::binary_search(mark.rejected, source->id)) continue;
				edit.reverse.signatures.emplace(key, mark);
				remap(mark);
				write.signatures.emplace(key, mark);
			}
			for (auto& [path, marks] : _marks)
			{
				bool saved = false;
				for (auto& mark : marks)
				{
					if (mark.person != source->id && !std::ranges::binary_search(mark.rejected, source->id)) continue;
					if (!saved)
					{
						edit.reverse.pictures.emplace_back(path, marks);
						saved = true;
					}
					remap(mark);
				}
				if (saved) write.pictures.emplace_back(path, marks);
			}
		}
		changed->aliases.emplace(from, id);
		write.aliases.emplace_back(from, id);
		edit.reverse.aliases.emplace_back(from, 0);
		_roster = std::move(changed);
		edit.after = _roster;
		edit.generation = expected_generation + 1;
		publish(lock, std::move(write));
		return edit;
	}

	static void apply_action(face_mark& mark, const face_action action, const uint32_t id)
	{
		switch (action)
		{
		case face_action::assign:
			mark.person = id;
			mark.hidden = false;
			std::erase(mark.rejected, id);
			break;
		case face_action::reject:
			if (mark.person == id) mark.person = 0;
			add_rejection(mark, id);
			break;
		case face_action::hide: mark.hidden = true; break;
		case face_action::unhide: mark.hidden = false; break;
		case face_action::unassign: mark.person = 0; break;
		}
	}

	std::optional<placed_faces> face_book::place(const std::span<const placement_batch> batches,
	                                             const face_action action, const uint64_t expected_generation)
	{
		std::unique_lock lock(_rw);
		if (generation() != expected_generation || batches.empty() || _read_only) return {};
		const bool targets_person = action == face_action::assign || action == face_action::reject;
		for (const auto& batch : batches)
		{
			if (batch.faces.empty()) return {};
			for (const auto& ref : batch.faces)
				if (ref.signature && !ref.signature->valid()) return {};
			if (targets_person && (batch.person ? !_roster->person(batch.person) : core::trim(batch.name).empty()))
				return {};
		}

		placed_faces placed;
		placed.before = _roster;
		book_write write;
		std::shared_ptr<roster> changed;
		auto next_id = _next_id;
		struct selection
		{
			std::vector<rectd> boxes;
			std::vector<int> positions;
			std::vector<uint32_t> targets;
			std::vector<std::optional<face_signature>> signatures;
			std::vector<int> previous;
		};
		core::hash_map<core::file_path, selection, core::ihash, core::ieq> touched;
		std::map<face_signature, uint32_t> assigned;
		for (const auto& batch : batches)
		{
			auto id = targets_person ? batch.person : 0;
			if (!id && targets_person)
			{
				const auto& current = changed ? *changed : *_roster;
				if (const auto* known = current.named(batch.name)) id = known->id;
				else
				{
					if (!changed) changed = std::make_shared<roster>(*_roster);
					if (next_id > UINT32_MAX) return {};
					id = static_cast<uint32_t>(next_id++);
					changed->people.push_back({id, std::string(core::trim(batch.name))});
					write.people.push_back(changed->people.back());
					placed.reverse.people_removed.push_back(id);
				}
			}
			placed.people.push_back(id);
			for (const auto& f : batch.faces)
			{
				auto& pic = touched[f.path];
				pic.boxes.push_back(f.box);
				pic.positions.push_back(f.position);
				pic.targets.push_back(id);
				pic.signatures.push_back(f.signature);
				if (f.signature && action == face_action::assign)
				{
					const auto [it, inserted] = assigned.emplace(*f.signature, id);
					if (!inserted && it->second != id) return {};
				}
			}
		}
		signature_marks decisions;
		for (auto& [path, selected] : touched)
		{
			// Two selected faces can overlap. Match all selected faces against the marks *before* the edit,
			// never against a mark just assigned to their neighbour.
			const auto pic = _marks.find(path);
			const auto old = pic == _marks.end() ? std::span<const face_mark>{}
			                                     : std::span<const face_mark>(pic->second);
			const auto matched = match_positions(old, selected.boxes, selected.positions, selected.signatures);
			selected.previous.resize(selected.boxes.size(), -1);
			for (size_t m = 0; m < matched.size(); ++m)
				if (matched[m] >= 0) selected.previous[matched[m]] = static_cast<int>(m);
			for (size_t i = 0; i < selected.signatures.size(); ++i)
			{
				const auto& key = selected.signatures[i];
				if (!key) continue;
				auto& mark = decisions[*key];
				if (const auto exact = _signatures.find(*key); exact != _signatures.end()) mark = exact->second;
				else if (selected.previous[i] >= 0 && !join_legacy(mark, old[selected.previous[i]])) return {};
			}
		}
		for (const auto& [path, selected] : touched)
			for (size_t i = 0; i < selected.signatures.size(); ++i)
				if (const auto& key = selected.signatures[i])
					apply_action(decisions.at(*key), action, selected.targets[i]);
		for (const auto& [key, mark] : decisions)
		{
			const auto old = _signatures.find(key);
			if (old != _signatures.end() && old->second == mark) continue;
			placed.reverse.signatures[key] = old == _signatures.end() ? std::nullopt : std::optional(old->second);
			write.signatures[key] = mark;
			_signatures[key] = mark;
		}
		for (const auto& [path, selected] : touched)
		{
			const auto stored = _marks.try_emplace(path).first;
			auto& old = stored->second;
			auto marks = old;
			for (size_t i = 0; i < selected.previous.size(); ++i)
			{
				const auto previous = selected.previous[i];
				auto& mark = previous >= 0 ? marks[previous] : marks.emplace_back();
				mark.box = selected.boxes[i];
				mark.position = selected.positions[i];
				mark.signature = selected.signatures[i];
				if (mark.signature) project(mark, decisions.at(*mark.signature));
				else apply_action(mark, action, selected.targets[i]);
			}
			if (old == marks) continue;
			placed.reverse.pictures.emplace_back(stored->first, std::move(old));
			old = std::move(marks);
			write.pictures.emplace_back(stored->first, old);
		}
		if (write.empty()) return {};
		_next_id = next_id;
		if (changed) _roster = std::move(changed);
		placed.after = _roster;
		placed.generation = expected_generation + 1;
		publish(lock, std::move(write));
		return placed;
	}

	bool face_book::can_undo(const book_edit& edit) const
	{
		return generation() == edit.generation && !_read_only;
	}

	bool face_book::undo(const book_edit& edit)
	{
		std::unique_lock lock(_rw);
		if (!can_undo(edit) || _roster != edit.after) return false;
		for (const auto& [key, mark] : edit.reverse.signatures)
		{
			if (mark) _signatures[key] = *mark;
			else _signatures.erase(key);
		}
		for (const auto& [path, before] : edit.reverse.pictures)
		{
			if (before.empty()) _marks.erase(path);
			else _marks[path] = before;
		}
		_roster = edit.before;
		publish(lock, edit.reverse);
		return true;
	}

	// The pictures' marks moved, all taken out before any is put back, into what they now are and what that wrote. Under
	// the lock.
	static void move_marks(marks_by_picture& marks,
	                       const std::span<const std::pair<core::file_path, core::file_path>> files, book_write& write)
	{
		std::vector<std::pair<core::file_path, std::vector<face_mark>>> taken;
		// Every path a move touched, as it is left: one given up empty, one moved onto with what came.
		marks_by_picture after;

		for (const auto& [from, to] : files)
		{
			// Renamed in case alone, it is the same picture, found as it was.
			if (from.icmp(to) == 0) continue;
			if (const auto found = marks.find(from); found != marks.end()) taken.emplace_back(to, found->second);
			after.try_emplace(from);
			after.insert_or_assign(to, std::vector<face_mark>{});
		}

		for (auto& [to, moved] : taken) after.insert_or_assign(to, std::move(moved));

		for (auto& [path, now] : after)
		{
			const auto had = marks.erase(path) > 0;
			if (!had && now.empty()) continue;
			if (!now.empty()) marks.emplace(path, now);
			write.pictures.emplace_back(path, std::move(now));
		}
	}

	void face_book::moved(const std::span<const std::pair<core::file_path, core::file_path>> files)
	{
		if (files.empty() || _read_only) return;
		book_write write;

		{
			std::unique_lock lock(_rw);
			move_marks(_marks, files, write);
			if (!write.pictures.empty()) publish(lock, std::move(write));
		}
	}

	void face_book::moved(const std::span<const std::pair<core::folder_path, core::folder_path>> folders)
	{
		if (folders.empty() || _read_only) return;
		book_write write;

		{
			std::unique_lock lock(_rw);
			std::vector<std::pair<core::file_path, core::file_path>> files;

			for (const auto& [path, marks] : _marks)
				if (const auto to = df::moved_with_folder(path, folders)) files.emplace_back(path, *to);

			move_marks(_marks, files, write);
			if (!write.pictures.empty()) publish(lock, std::move(write));
		}
	}

	void face_book::turned(const core::file_path path, const core::orientation from, const core::orientation to)
	{
		if (from == to || _read_only) return;
		book_write write;

		{
			std::unique_lock lock(_rw);
			const auto found = _marks.find(path);
			if (found == _marks.end()) return;
			for (auto& m : found->second) m.box = to_upright(to_stored(m.box, from), to);
			write.pictures.emplace_back(path, found->second);
			publish(lock, std::move(write));
		}
	}

	// A picture's marks after its pixels changed: each goes to the face found again that resembles the face it marked by
	// `same_face`, the likeliest pairs first, each face taken once. A mark whose face is not found again stays where it
	// was - on its own face, where the edit moved none, such as one that changed only colours, and waiting where its
	// face is lost - unless the edit moved its face away and another is there now.
	static std::vector<face_mark> follow_marks(const std::span<const face_mark> marks,
	                                           const std::span<const found_face> before,
	                                           const std::span<const found_face> after, const uint32_t model)
	{
		std::vector<rectd> boxes;
		boxes.reserve(before.size());
		for (const auto& f : before) boxes.push_back(f.box);
		std::vector<std::optional<face_signature>> signatures;
		for (const auto& f : before) signatures.emplace_back(face_signature{model, f.hash});
		const auto marked = match_positions(marks, boxes, {}, signatures);

		struct pair
		{
			float similarity;
			size_t mark;
			size_t face;
		};

		std::vector<pair> pairs;
		for (size_t m = 0; m < marks.size(); ++m)
		{
			if (marked[m] < 0 || before[static_cast<size_t>(marked[m])].hash.is_empty()) continue;
			const auto& was = before[static_cast<size_t>(marked[m])].hash;

			for (size_t f = 0; f < after.size(); ++f)
			{
				if (after[f].hash.is_empty()) continue;
				const auto s = similarity(was, after[f].hash);
				if (s >= same_face) pairs.push_back({s, m, f});
			}
		}

		std::ranges::sort(pairs, [](const pair& a, const pair& b)
		{
			if (a.similarity != b.similarity) return a.similarity > b.similarity;
			return a.mark != b.mark ? a.mark < b.mark : a.face < b.face;
		});

		std::vector<int> to(marks.size(), -1);
		std::vector<bool> taken(after.size(), false);

		for (const auto& p : pairs)
		{
			if (to[p.mark] >= 0 || taken[p.face]) continue;
			to[p.mark] = static_cast<int>(p.face);
			taken[p.face] = true;
		}

		// Whether the edit moved the faces: told by a face that followed its mark, or with none, by whether the faces read
		// before are where they were.
		const auto in_place = [](const rectd& box, const std::span<const found_face> faces)
		{
			return std::ranges::any_of(faces, [&box](const found_face& f)
			{ return box_union_share(box, f.box) >= least_share; });
		};
		auto moved = false;
		auto told = false;

		for (size_t m = 0; m < marks.size(); ++m)
		{
			if (to[m] < 0) continue;
			told = true;
			if (box_union_share(before[static_cast<size_t>(marked[m])].box, after[static_cast<size_t>(to[m])].box) <
			    least_share)
				moved = true;
		}

		if (!told) moved = std::ranges::any_of(before, [&](const found_face& f) { return !in_place(f.box, after); });

		std::vector<face_mark> result;
		result.reserve(marks.size());

		for (size_t m = 0; m < marks.size(); ++m)
		{
			auto mark = marks[m];

			if (to[m] >= 0)
			{
				mark.box = after[static_cast<size_t>(to[m])].box;
				mark.position = to[m];
				// The pixels changed. The old key still describes the original face; this is now local evidence
				// on the edited picture until the new signature is bound with the other matching legacy marks.
				if (mark.signature && mark.signature->hash != after[static_cast<size_t>(to[m])].hash)
					mark.signature.reset();
			}
			else if (moved && marked[m] >= 0 && in_place(mark.box, after))
			{
				// Its face moved and was not found again, and another is where it was.
				continue;
			}

			result.push_back(mark);
		}

		return result;
	}

	void face_book::follow(const core::file_path path, const std::span<const found_face> before,
	                       const std::span<const found_face> after, const uint32_t model)
	{
		if (_read_only) return;
		book_write write;

		{
			std::unique_lock lock(_rw);
			const auto found = _marks.find(path);
			if (found == _marks.end()) return;
			auto effective = found->second;
			project(effective, _signatures);
			auto now = follow_marks(effective, before, after, model);
			if (now == effective) return;
			if (now.empty()) _marks.erase(found);
			else found->second = now;
			write.pictures.emplace_back(path, std::move(now));
			publish(lock, std::move(write));
		}
	}

	void face_book::clear()
	{
		if (_read_only) return;
		std::unique_lock lock(_rw);
		auto empty = std::make_shared<roster>();
		empty->loaded_at = generation() + 1;
		_roster = std::move(empty);
		_marks.clear();
		_signatures.clear();
		book_write write;
		write.clear = true;
		publish(lock, std::move(write));
	}

	// ---- CSV (docs/faces.md#65-saving-and-moving-the-face-book) -------------------------------------

	static constexpr std::string_view csv_format = "Diffractor faces";
	static constexpr int csv_version = 4;
	static constexpr std::array<std::string_view, 9> csv_columns{"type",      "person",     "name",
	                                                             "model",     "scale_bits", "values",
	                                                             "confirmed", "hidden",     "rejected"};
	static constexpr std::array<std::string_view, 12> legacy_columns{"type",     "person",    "name",   "file",
	                                                                 "x",        "y",         "width",  "height",
	                                                                 "position", "confirmed", "hidden", "rejected"};

	static void append_csv_row(std::string& csv, const std::array<std::string_view, csv_columns.size()>& fields)
	{
		for (size_t i = 0; i < fields.size(); ++i)
		{
			if (i) csv += ',';
			const auto text = fields[i];
			const auto quoted = text.find_first_of(",\"\r\n") != std::string_view::npos;
			if (quoted) csv += '"';
			for (const auto c : text)
			{
				if (c == '"') csv += '"';
				csv += c;
			}
			if (quoted) csv += '"';
		}
		csv += "\r\n";
	}

	// Unlike the CSV display's line splitter, a backup must read quoted newlines and reject malformed rows.
	static bool read_csv_row(std::string_view& csv, const std::span<std::string> fields)
	{
		for (size_t i = 0; i < fields.size(); ++i)
		{
			auto& field = fields[i];
			field.clear();
			if (csv.starts_with('"'))
			{
				csv.remove_prefix(1);
				for (;;)
				{
					const auto quote = csv.find('"');
					if (quote == std::string_view::npos) return false;
					field.append(csv.substr(0, quote));
					csv.remove_prefix(quote + 1);
					if (!csv.starts_with('"')) break;
					field += '"';
					csv.remove_prefix(1);
				}
			}
			else
			{
				const auto end = csv.find_first_of(",\r\n");
				const auto text = csv.substr(0, end);
				if (text.find('"') != std::string_view::npos) return false;
				field = text;
				csv.remove_prefix(text.size());
			}

			if (i + 1 < fields.size())
			{
				if (!csv.starts_with(',')) return false;
				csv.remove_prefix(1);
			}
			else if (!csv.empty())
			{
				if (csv.starts_with('\r'))
				{
					csv.remove_prefix(1);
					if (csv.starts_with('\n')) csv.remove_prefix(1);
				}
				else if (csv.starts_with('\n')) csv.remove_prefix(1);
				else return false;
			}
		}
		return true;
	}

	template <typename T> static bool csv_number(const std::string_view text, T& value)
	{
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		return error == std::errc{} && end == text.data() + text.size();
	}

	// A file's full path as a file read from elsewhere spells it, or none for text that is not one: a path is made only
	// from text already known to be one.
	static std::optional<core::file_path> full_path(const std::string_view text)
	{
		if (text.size() < 4 || std::ranges::any_of(text, [](const char c) { return static_cast<uint8_t>(c) < 0x20; }))
			return {};

		const auto qualified = core::windows_path_semantics ? (std::isalpha(static_cast<unsigned char>(text[0])) &&
		                                                       text[1] == ':' && core::is_slash(text[2])) ||
		                                                          (core::is_slash(text[0]) && core::is_slash(text[1]))
		                                                    : text[0] == '/';
		if (!qualified) return {};

		const auto slash = text.find_last_of("\\/");
		if (slash == std::string_view::npos || slash + 1 >= text.size()) return {};

		const auto path = core::file_path(text);
		if (path.is_empty() || path.name().is_empty() || !path.is_valid()) return {};
		return path;
	}

	static std::string hex_values(const face_hash& hash)
	{
		constexpr std::string_view digits = "0123456789abcdef";
		std::string text;
		text.reserve(hash.values.size() * 2);
		for (const auto value : hash.values)
		{
			const auto byte = static_cast<uint8_t>(value);
			text.push_back(digits[byte >> 4]);
			text.push_back(digits[byte & 15]);
		}
		return text;
	}

	static bool parse_key(const std::span<const std::string> row, face_signature& key)
	{
		if (!csv_number(row[3], key.model) || row[4].size() != 8 || row[5].size() != key.hash.values.size() * 2)
			return false;
		const auto hex = [](const std::string_view s, auto& value)
		{
			const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
			return ec == std::errc{} && end == s.data() + s.size();
		};
		uint32_t bits = 0;
		if (!hex(row[4], bits)) return false;
		key.hash.scale = std::bit_cast<float>(bits);
		if (!key.valid()) return false;
		for (size_t i = 0; i < key.hash.values.size(); ++i)
		{
			uint8_t byte = 0;
			if (!hex(std::string_view(row[5]).substr(i * 2, 2), byte)) return false;
			key.hash.values[i] = std::bit_cast<int8_t>(byte);
		}
		return true;
	}

	std::string face_book::to_csv(size_t* unportable) const
	{
		roster_ptr people;
		signature_marks marks;
		{
			core::shared_lock lock(_rw);
			people = _roster;
			marks = _signatures;
			if (unportable) *unportable = _unportable;
		}

		std::string csv;
		append_csv_row(csv, {csv_format, std::to_string(csv_version)});
		append_csv_row(csv, csv_columns);
		for (const auto& p : people->people) append_csv_row(csv, {"person", std::to_string(p.id), p.name});
		for (const auto& [name, id] : people->aliases) append_csv_row(csv, {"alias", std::to_string(id), name});
		for (const auto& [key, m] : marks)
		{
			std::string rejected;
			for (const auto id : m.rejected)
			{
				if (!rejected.empty()) rejected += ' ';
				rejected += std::to_string(id);
			}
			append_csv_row(csv, {"signature",
			                     {},
			                     {},
			                     std::to_string(key.model),
			                     std::format("{:08x}", std::bit_cast<uint32_t>(key.hash.scale)),
			                     hex_values(key.hash),
			                     m.person ? std::to_string(m.person) : std::string{},
			                     m.hidden ? "1" : "0",
			                     rejected});
		}
		return csv;
	}

	import_result face_book::import_csv(std::string_view csv)
	{
		import_result result;
		if (_read_only)
		{
			result.error = "the face book is being changed in another Diffractor window";
			return result;
		}
		if (csv.starts_with("\xef\xbb\xbf")) csv.remove_prefix(3);
		std::array<std::string, legacy_columns.size()> cells;
		const auto width = 1 + static_cast<size_t>(std::ranges::count(csv.substr(0, csv.find_first_of("\r\n")), ','));
		if (width != csv_columns.size() && width != legacy_columns.size())
		{
			result.error = "invalid face data header";
			return result;
		}
		const auto row = std::span(cells).first(static_cast<size_t>(width));
		if (!read_csv_row(csv, row) || row[0] != csv_format ||
		    !std::all_of(row.begin() + 2, row.end(), [](const auto& s) { return s.empty(); }))
		{
			result.error = "not a Diffractor faces CSV file";
			return result;
		}

		int version = 0;
		if (!csv_number(row[1], version) || (version != 2 && version != 3 && version != csv_version))
		{
			result.error = "unsupported face data version";
			return result;
		}
		const bool portable = version == csv_version;
		if (!read_csv_row(csv, row) ||
		    !(portable ? std::ranges::equal(row, csv_columns) : std::ranges::equal(row, legacy_columns)))
		{
			result.error = "invalid face data columns";
			return result;
		}

		std::vector<kept_person> imported;
		marks_by_picture incoming;
		signature_marks exact;
		core::hash_set<uint32_t> ids;
		std::map<std::string, uint32_t, roster::name_less> names;
		size_t record = 2;
		while (!csv.empty())
		{
			++record;
			auto valid = read_csv_row(csv, row);
			if (valid && (row[0] == "person" || (version >= 3 && row[0] == "alias")))
			{
				kept_person p;
				p.name = core::trim(row[2]);
				valid = csv_number(row[1], p.id) && p.id && !p.name.empty() && names.emplace(p.name, p.id).second &&
				        std::all_of(row.begin() + 3, row.end(), [](const auto& s) { return s.empty(); });
				if (valid && row[0] == "person")
				{
					valid = ids.insert(p.id).second;
					if (valid) imported.push_back(std::move(p));
				}
			}
			else if (valid && row[0] == (portable ? "signature" : "face"))
			{
				std::optional<core::file_path> path;
				face_signature key;
				face_mark m{};
				valid = row[1].empty() && row[2].empty();
				if (portable) valid = valid && parse_key(row, key);
				else
				{
					path = full_path(row[3]);
					valid = valid && path && csv_number(row[4], m.box.X) && std::isfinite(m.box.X) &&
					        csv_number(row[5], m.box.Y) && std::isfinite(m.box.Y) && csv_number(row[6], m.box.Width) &&
					        m.box.Width > 0.0 && m.box.Width <= 2.0 && csv_number(row[7], m.box.Height) &&
					        m.box.Height > 0.0 && m.box.Height <= 2.0 && csv_number(row[8], m.position) &&
					        m.position >= 0 && m.position <= 255;
				}
				const auto state = row.last<3>();
				valid = valid && (state[0].empty() || (csv_number(state[0], m.person) && m.person)) &&
				        (state[1] == "0" || state[1] == "1");
				m.hidden = state[1] == "1";
				auto rejected = std::string_view(state[2]);
				while (valid && !rejected.empty())
				{
					const auto end = rejected.find(' ');
					uint32_t id = 0;
					valid = csv_number(rejected.substr(0, end), id) && id && id != m.person;
					if (valid) m.rejected.push_back(id);
					if (end == std::string_view::npos) break;
					rejected.remove_prefix(end + 1);
					if (rejected.empty()) valid = false;
				}
				std::ranges::sort(m.rejected);
				valid = valid && std::ranges::adjacent_find(m.rejected) == m.rejected.end();
				if (valid && portable) valid = exact.emplace(key, std::move(m)).second;
				else if (valid)
				{
					auto& picture = incoming[*path];
					valid = !std::ranges::any_of(picture, [&](const auto& old)
					{ return old.position == m.position && box_union_share(old.box, m.box) >= least_share; });
					if (valid) picture.push_back(std::move(m));
				}
			}
			else valid = false;

			if (!valid)
			{
				result.error = std::format("invalid face data at CSV record {}", record);
				return result;
			}
		}
		for (const auto& [name, id] : names)
			if (!ids.contains(id))
			{
				result.error = "a former name refers to a person not in the file";
				return result;
			}
		const auto dangling = [&](const face_mark& m)
		{
			return (m.person && !ids.contains(m.person)) ||
			       std::ranges::any_of(m.rejected, [&](const auto id) { return !ids.contains(id); });
		};
		if (std::ranges::any_of(exact, [&](const auto& entry) { return dangling(entry.second); }) ||
		    std::ranges::any_of(incoming,
		                        [&](const auto& entry) { return std::ranges::any_of(entry.second, dangling); }))
		{
			result.error = "a face refers to a person not in the file";
			return result;
		}

		std::unique_lock lock(_rw);
		core::hash_map<uint32_t, uint32_t> local_ids;
		book_write write;
		auto changed = std::make_shared<roster>(*_roster);
		std::map<std::string_view, uint32_t, core::iless> known;
		for (const auto& p : _roster->people) known.emplace(p.name, p.id);
		for (const auto& [name, id] : _roster->aliases) known.emplace(name, id);
		for (const auto& [name, id] : names)
		{
			const auto found = known.find(name);
			if (found == known.end()) continue;
			const auto [it, first] = local_ids.emplace(id, found->second);
			if (!first && it->second != found->second)
			{
				result.error = "the names of an imported person belong to different local people";
				return result;
			}
		}
		if (imported.size() - local_ids.size() > uint64_t{UINT32_MAX} + 1 - _next_id)
		{
			result.error = "no more person IDs available";
			return result;
		}
		result.people_joined = local_ids.size();
		for (const auto& p : imported)
		{
			if (local_ids.contains(p.id)) continue;
			const auto id = take_id();
			local_ids.emplace(p.id, id);
			changed->people.push_back({id, p.name});
			write.people.push_back(changed->people.back());
			++result.people_added;
		}
		for (const auto& [name, imported_id] : names)
		{
			if (known.contains(name)) continue;
			const auto id = local_ids.at(imported_id);
			if (core::icmp(name, changed->person(id)->name) == 0) continue;
			changed->aliases.emplace(name, id);
			write.aliases.emplace_back(name, id);
		}

		const auto remap = [&](face_mark& m)
		{
			if (m.person) m.person = local_ids.at(m.person);
			for (auto& id : m.rejected) id = local_ids.at(id);
			std::ranges::sort(m.rejected);
			m.rejected.erase(std::unique(m.rejected.begin(), m.rejected.end()), m.rejected.end());
			// Two IDs in the backup may denote one local person.
			if (std::ranges::binary_search(m.rejected, m.person)) m.person = 0;
		};
		const auto merge_local = [](face_mark& m, const face_mark& old)
		{
			if (old.person) m.person = old.person;
			m.hidden |= old.hidden;
			for (const auto id : old.rejected) add_rejection(m, id);
			if (old.person) std::erase(m.rejected, old.person);
			else if (std::ranges::binary_search(m.rejected, m.person)) m.person = 0;
		};
		for (auto& [key, m] : exact)
		{
			remap(m);
			++result.faces;
			if (const auto it = _signatures.find(key); it != _signatures.end())
			{
				merge_local(m, it->second);
				if (m == it->second) continue;
			}
			_signatures[key] = m;
			write.signatures[key] = m;
		}

		for (auto& [path, marks] : incoming)
		{
			const auto stored = _marks.try_emplace(path).first;
			auto& local = stored->second;
			std::vector<rectd> boxes;
			std::vector<int> positions;
			std::vector<size_t> unsigned_rows;
			for (size_t i = 0; i < local.size(); ++i)
			{
				if (local[i].signature) continue;
				boxes.push_back(local[i].box);
				positions.push_back(local[i].position);
				unsigned_rows.push_back(i);
			}
			const auto matches = match_positions(marks, boxes, positions);
			bool updated = false;
			for (size_t i = 0; i < marks.size(); ++i)
			{
				auto& m = marks[i];
				remap(m);
				if (matches[i] >= 0)
				{
					auto& old = local[unsigned_rows[matches[i]]];
					merge_local(m, old);
					if (m == old) continue;
					old = std::move(m);
				}
				else
				{
					// Importing an old filename must not change an exact decision. Already represented evidence
					// needs no second row; otherwise keep it path-only, not silently discarded or made portable.
					if (const auto* old = find_mark(local, m.box, m.position); old && old->signature)
					{
						const auto& existing = _signatures.at(*old->signature);
						auto joined = existing;
						if (join_legacy(joined, m) && joined == existing) continue;
					}
					local.push_back(std::move(m));
				}
				updated = true;
			}
			result.faces += marks.size();
			if (updated) write.pictures.emplace_back(stored->first, local);
		}
		if (!write.people.empty() || !write.aliases.empty()) _roster = std::move(changed);
		if (!write.empty()) publish(lock, std::move(write));
		result.ok = true;
		return result;
	}
}
