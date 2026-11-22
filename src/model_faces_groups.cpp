// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Resolve file and book evidence per face, compare the unassigned faces with named people, and order Unknown.
// This pass never writes identity, training data or visibility. Owned by docs/faces.md#7-recognizing-people.

#include "pch.h"

#include "model_faces_search.h"

#include <bit>
#include <numeric>

#if defined(_M_X64)
#include <immintrin.h>
#endif

namespace faces
{
	namespace
	{
		double quality(const face_entry& e)
		{
			return e.confidence / 100.0 * 0.3 + e.frontal / 255.0 * 0.4 + std::min(e.eye_distance / 70.0, 1.0) * 0.3;
		}

		// Sample from trusted appearances, not just the largest cluster: e.g. a single childhood portrait.
		// Farthest-first costs O(limit * candidates); bound both sides for very well photographed people.
		std::vector<face_exemplar> exemplars(std::vector<face_exemplar> faces, const size_t limit)
		{
			if (faces.size() <= limit) return faces;
			constexpr size_t max_candidates = 4096;
			if (faces.size() > max_candidates)
			{
				for (size_t i = 0; i < max_candidates; ++i) faces[i] = faces[i * faces.size() / max_candidates];
				faces.resize(max_candidates);
			}

			auto next = static_cast<size_t>(std::ranges::max_element(faces, {}, &face_exemplar::quality) -
			                                faces.begin());
			std::vector<face_exemplar> result;
			std::vector<float> nearest(faces.size(), not_compared);
			for (size_t n = 0; n < std::min(limit, faces.size()); ++n)
			{
				const auto chosen = faces[next];
				result.push_back(chosen);
				nearest[next] = 2.0f;
				for (size_t i = 0; i < faces.size(); ++i)
				{
					if (nearest[i] < 2.0f) nearest[i] = std::max(nearest[i], similarity(chosen.hash, faces[i].hash));
				}
				next = static_cast<size_t>(std::ranges::min_element(nearest) - nearest.begin());
			}
			return result;
		}

#if defined(_M_X64)
		// docs/faces.md#53-comparing-faces: the same comparison as similarity(), with the query widened once and kept in
		// registers while every exemplar streams past it - what a suggestion pass spends nearly all its time on. The
		// dot product is exact in integers, and scaled in the same order, so the answer is the SSE2 one to the bit.
		float best_match_avx2(const face_hash& query, const uint32_t face, const std::vector<face_exemplar>& exemplars)
		{
			static_assert(vector_size == 128);
			std::array<__m256i, 8> q;
			for (size_t i = 0; i < q.size(); ++i)
				q[i] = _mm256_cvtepi8_epi16(
					_mm_loadu_si128(std::bit_cast<const __m128i*>(query.values.data() + i * 16)));

			auto best = not_compared;
			for (const auto& other : exemplars)
			{
				if (face == other.face) continue;
				const auto* const v = other.hash.values.data();
				auto sum = _mm256_madd_epi16(q[0],
				                             _mm256_cvtepi8_epi16(_mm_loadu_si128(std::bit_cast<const __m128i*>(v))));
				for (size_t i = 1; i < q.size(); ++i)
				{
					const auto b = _mm256_cvtepi8_epi16(_mm_loadu_si128(std::bit_cast<const __m128i*>(v + i * 16)));
					sum = _mm256_add_epi32(sum, _mm256_madd_epi16(q[i], b));
				}
				auto half = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
				half = _mm_add_epi32(half, _mm_shuffle_epi32(half, 0x4e));
				half = _mm_add_epi32(half, _mm_shuffle_epi32(half, 0xb1));
				const auto s = static_cast<float>(_mm_cvtsi128_si32(half)) * query.scale * other.hash.scale;
				best = std::max(best, s);
			}
			return best;
		}
#endif

		float best_match(const face_hash& query, const uint32_t face, const std::vector<face_exemplar>& exemplars)
		{
#if defined(_M_X64)
			if (core::has_avx2()) return best_match_avx2(query, face, exemplars);
#endif
			auto best = not_compared;
			for (const auto& other : exemplars)
				if (face != other.face) best = std::max(best, similarity(query, other.hash));
			return best;
		}

		// The main portrait is clear; the others favour a different year and pose. Neither a missing date nor
		// a missing hash prevents a confirmed face being shown, but a face large and square-on enough to carry an
		// identity is preferred over one that is not: docs/faces.md#44-quality. A photo's face is preferred to a
		// video's, which is seldom as sharp, and is not on the thumbnail some portraits are cut from.
		void choose_samples(face_person& p, const face_store& store, const std::vector<int>& years)
		{
			while (p.samples.size() < 3)
			{
				std::optional<uint32_t> best;
				double highest = -1;
				for (const auto& m : p.confirmed)
				{
					if (std::ranges::find(p.samples, m.face) != p.samples.end()) continue;
					const auto& e = store.entry(m.face);
					double diversity = 1.0;
					for (const auto sample : p.samples)
					{
						const auto& s = store.entry(sample);
						const auto era = years[e.run] && years[s.run]
						                     ? std::min(1.0, std::abs(years[e.run] - years[s.run]) / 10.0)
						                     : 0.0;
						const auto pose = std::abs(e.frontal - s.frontal) / 255.0;
						diversity = std::min(diversity, era * 0.55 + pose * 0.45);
					}
					const auto score = diversity + quality(e) * 0.2 + (e.is_good() ? 0.5 : 0.0) +
					                   (store.run(e.run).video ? 0.0 : 1.0);
					if (!best || score > highest || (score == highest && m.face < *best))
					{
						highest = score;
						best = m.face;
					}
				}
				if (!best) break;
				p.samples.push_back(*best);
			}
		}

		bool contains_name(const std::vector<std::string>& names, const std::string_view name)
		{
			return std::ranges::any_of(names, [&](const auto& n) { return core::icmp(n, name) == 0; });
		}

		struct unknown_order
		{
			std::vector<uint32_t> faces;
			std::vector<uint32_t> runs;
			size_t unclear_from = 0;
		};

		// docs/faces.md#91-people: Unknown in review order. Clear faces that look alike are runs, the largest first, so the
		// people photographed most can be named first, a run at a time; then the single clear faces; then those too
		// small or unclear to carry an identity, alike or not, which the scan finds in crowds and backgrounds and which
		// are least worth a look.
		unknown_order order_unknown(const face_store& store, const std::vector<uint32_t>& unknown, const float floor,
		                            const std::function<bool()>& cancel, const parallel_for& parallel)
		{
			constexpr size_t nearby = 24;
			constexpr size_t exact_limit = nearby * 4;
			struct neighbour
			{
				size_t nearest = SIZE_MAX;
				float score = not_compared;

				void offer(const size_t to, const float s)
				{
					if (s > score || (s == score && to < nearest))
					{
						nearest = to;
						score = s;
					}
				}
			};
			std::vector<uint64_t> projections(unknown.size());
			std::vector<size_t> comparable;
			for (size_t i = 0; i < unknown.size(); ++i)
			{
				if ((i & 255) == 0 && cancel()) return {};
				// Only a face clear enough to carry an identity joins a run. The rest are listed with the small or
				// unclear faces however alike they look: the network finds blurred faces, pets and toys alike, and
				// runs of them led Unknown, the largest run first.
				const auto& entry = store.entry(unknown[i]);
				if (!entry.has_hash || !entry.is_good()) continue;
				comparable.push_back(i);
				const auto* values = store.values(unknown[i]);
				auto& key = projections[i];
				for (size_t bit = 0; bit < 64; ++bit)
					key |= static_cast<uint64_t>(values[bit] > values[bit + 64]) << bit;
			}

			// Compare small inboxes exactly. On large ones, four projections propose bounded neighbourhoods;
			// only the actual cosine can connect two faces. A quadratic pass would stall every new overview.
			// Each projection finds its own nearest faces, on its own thread where one is lent, and the nearest of
			// them all is kept: the same answer, whichever finishes first.
			const auto exact = comparable.size() <= exact_limit;
			std::vector<std::vector<neighbour>> found(exact ? 1 : 4);
			std::atomic_bool stopped = false;
			const auto look = [&](const size_t begin, const size_t end)
			{
				for (auto pass = begin; pass < end && !stopped.load(); ++pass)
				{
					auto& matches = found[pass];
					matches.resize(unknown.size());
					auto order = comparable;
					if (!exact)
						std::ranges::sort(order, {}, [&](const size_t i)
						{ return std::pair{std::rotl(projections[i], static_cast<int>(pass) * 16), i}; });
					for (size_t a = 0; a < order.size(); ++a)
					{
						if ((a & 255) == 0 && (stopped.load(std::memory_order_relaxed) || cancel()))
						{
							stopped = true;
							return;
						}
						const auto i = order[a];
						const auto f = unknown[i];
						const auto run = store.entry(f).run;
						const auto end_at = exact ? order.size() : std::min(order.size(), a + nearby + 1);
						for (auto b = a + 1; b < end_at; ++b)
						{
							const auto j = order[b];
							if (store.entry(unknown[j]).run == run) continue;
							const auto score = similarity(store, f, unknown[j]);
							if (score < floor) continue;
							matches[i].offer(j, score);
							matches[j].offer(i, score);
						}
					}
				}
			};
			if (parallel) parallel(found.size(), look);
			else look(0, found.size());
			if (stopped.load() || cancel()) return {};

			auto& matches = found.front();
			for (size_t pass = 1; pass < found.size(); ++pass)
				for (const auto i : comparable)
					if (const auto& other = found[pass][i]; other.nearest != SIZE_MAX)
						matches[i].offer(other.nearest, other.score);

			std::vector<size_t> parent(unknown.size());
			std::iota(parent.begin(), parent.end(), 0_z);
			const auto root = [&](size_t i)
			{
				while (parent[i] != i)
				{
					parent[i] = parent[parent[i]];
					i = parent[i];
				}
				return i;
			};
			for (const auto i : comparable)
			{
				if (matches[i].nearest == SIZE_MAX) continue;
				const auto a = root(i), b = root(matches[i].nearest);
				parent[std::max(a, b)] = std::min(a, b);
			}
			if (cancel()) return {};

			// Each run in the order its faces were found, so a run reads as the pictures do; the runs largest first. A
			// run's root is its first face, as a union always keeps the lower.
			std::vector<uint32_t> size(unknown.size());
			for (size_t i = 0; i < unknown.size(); ++i) ++size[root(i)];
			std::vector<size_t> runs;
			std::vector<size_t> clear, unclear;
			for (size_t i = 0; i < unknown.size(); ++i)
			{
				if (root(i) != i) continue;
				if (size[i] > 1) runs.push_back(i);
				else
				{
					const auto& e = store.entry(unknown[i]);
					(e.has_hash && e.is_good() ? clear : unclear).push_back(i);
				}
			}
			std::ranges::sort(runs, [&size](const size_t a, const size_t b)
			{ return size[a] != size[b] ? size[a] > size[b] : a < b; });

			unknown_order order;
			order.faces.resize(unknown.size());
			std::vector<size_t> next(unknown.size());
			size_t at = 0;
			for (const auto r : runs)
			{
				order.runs.push_back(size[r]);
				next[r] = at;
				at += size[r];
			}
			for (size_t i = 0; i < unknown.size(); ++i)
				if (const auto r = root(i); size[r] > 1) order.faces[next[r]++] = unknown[i];
			for (const auto i : clear) order.faces[at++] = unknown[i];
			order.unclear_from = at;
			for (const auto i : unclear) order.faces[at++] = unknown[i];
			return order;
		}
	}

	overview_ptr make_overview(const face_store_const_ptr& store, const regions_of& regions, const face_book& book,
	                           const std::function<bool()>& cancel, const tuning& t, const parallel_for& parallel)
	{
		if (!store || cancel()) return {};
		auto o = std::make_shared<overview>();
		o->store = store;
		o->generation = store->generation();
		o->book_generation = book.generation();
		o->roster = book.people();
		const auto marks = book.all_marks();
		const auto signatures = book.all_signatures();
		const auto runs = store->run_count();
		const auto model = model_id();
		auto& people = o->people;
		core::hash_map<std::string, uint32_t, core::ihash, core::ieq> by_name;
		core::hash_map<uint32_t, uint32_t> by_id;
		std::vector<core::hash_map<std::string, int>> spellings;
		const auto person = [&](const std::string& name, const uint32_t id)
		{
			const auto [at, added] = by_name.try_emplace(name, static_cast<uint32_t>(people.size()));
			if (added)
			{
				people.push_back({id, name});
				spellings.emplace_back();
			}
			return at->second;
		};
		for (const auto& p : o->roster->people)
			if (!p.name.empty()) by_id[p.id] = person(p.name, p.id);

		std::vector<int> years(runs);
		std::vector<core::cached> file_names(runs);
		core::hash_map<uint32_t, std::vector<face_exemplar>> negatives, positives;
		core::hash_map<uint32_t, std::vector<uint32_t>> never_offer;
		std::set<face_signature> represented;
		core::hash_map<uint32_t, std::vector<uint32_t>> occupied;
		// docs/faces.md#72-suggestions: the face a picture's metadata offers to the one person it names, by face.
		core::hash_map<uint32_t, uint32_t> named_offers;
		for (uint32_t r = 0; r < runs; ++r)
		{
			if ((r & 255) == 0 && cancel()) return {};
			const auto& run = store->run(r);
			if (!run.is_current() || run.model != model || !run.count) continue;
			const auto info = regions(run.path);
			years[r] = info.year;
			file_names[r] = info.names;

			std::vector<found_face> found(run.count);
			std::vector<rectd> boxes(run.count);
			for (uint32_t i = 0; i < run.count; ++i) boxes[i] = found[i].box = store->entry(run.first + i).bounds();
			const auto matched = match_regions(info.regions, found);
			std::vector<const std::string*> name_of(run.count);
			std::vector<std::string> retained, overridden;
			for (size_t k = 0; k < matched.size(); ++k)
			{
				const auto& name = info.regions[k].name;
				if (name.empty()) continue;
				if (matched[k] >= 0) name_of[matched[k]] = &name;
				else retained.push_back(name); // Another region, even one without a detection, still speaks.
			}
			std::vector<const face_mark*> mark_of(run.count);
			if (const auto picture = marks.find(run.path); picture != marks.end())
			{
				const auto matched_marks = match_marks(picture->second, boxes);
				for (size_t k = 0; k < matched_marks.size(); ++k)
					if (matched_marks[k] >= 0) mark_of[matched_marks[k]] = &picture->second[k];
			}
			for (uint32_t i = 0; i < run.count; ++i)
			{
				const auto f = run.first + i;
				const auto* mark = mark_of[i];
				const bool has_hash = store->entry(f).has_hash;
				const auto signature = has_hash ? store->signature(f) : face_signature{};
				if (mark && mark->signature && *mark->signature != signature) mark = nullptr;
				bool train = has_hash;
				if (has_hash)
				{
					if (const auto known = signatures.find(signature); known != signatures.end())
					{
						mark = &known->second;
						train = represented.insert(signature).second;
					}
				}
				const auto* name = name_of[i];
				const bool hidden = mark ? mark->hidden : store->entry(f).is_hidden();
				std::optional<uint32_t> placed;
				auto kind = member_kind::confirmed;
				if (mark)
				{
					if (const auto at = by_id.find(mark->person); at != by_id.end()) placed = at->second;
					if (!mark->rejected.empty()) o->ruled_out[f] = mark->rejected;
					for (const auto id : mark->rejected)
						if (const auto at = by_id.find(id); at != by_id.end())
						{
							never_offer[f].push_back(at->second);
							if (train && !hidden)
								negatives[at->second].push_back({f, signature.hash, quality(store->entry(f))});
						}
				}
				if (name)
				{
					const std::string canonical(o->roster->canonical_name(*name));
					const auto at = by_name.find(canonical);
					const bool rejected = mark && at != by_name.end() &&
					                      std::ranges::binary_search(mark->rejected, people[at->second].id);
					if (hidden || rejected || (placed && core::icmp(people[*placed].name, canonical) != 0))
						overridden.push_back(*name);
					else
					{
						retained.push_back(*name);
						if (!placed)
						{
							placed = person(canonical, 0);
							kind = member_kind::named;
						}
					}
				}
				if (hidden) o->hidden.push_back(f);
				else if (placed)
				{
					people[*placed].confirmed.push_back({f, not_compared, kind});
					if (train) positives[*placed].push_back({f, signature.hash, quality(store->entry(f))});
					occupied[r].push_back(*placed);
				}
				else o->unknown.push_back(f);
			}

			// docs/faces.md#72-suggestions: a picture holding one face, whose metadata names one person but places
			// nobody, offers that face to them: what keywords and Iptc4xmpExt:PersonInImage say without a region. An offer,
			// as a resemblance is, until the user confirms it; a face rejected for them is not offered.
			if (run.count == 1 && !o->unknown.empty() && o->unknown.back() == run.first &&
			    std::ranges::none_of(info.regions, [](const named_region& n) { return n.has_area(); }))
			{
				std::string only;
				auto named = 0;
				split2(info.names, true, [&](const std::string_view n)
				{
					const auto raw = core::trim(n);
					if (raw.empty()) return;
					const std::string canonical(o->roster->canonical_name(raw));
					if (named == 0 || core::icmp(only, canonical) != 0)
					{
						++named;
						only = canonical;
					}
				});
				if (named == 1 && !contains_name(overridden, only))
				{
					const auto p = person(only, 0);
					const auto denied = never_offer.find(run.first);
					if (denied == never_offer.end() || std::ranges::find(denied->second, p) == denied->second.end())
						named_offers.emplace(run.first, p);
				}
			}
			for (const auto& n : overridden)
			{
				if (contains_name(retained, n)) continue;
				auto& discarded = o->contradicted[run.path];
				if (contains_name(discarded, n)) continue;
				discarded.push_back(n);
				--o->count_adjustments[n];
			}
			std::ranges::sort(retained);
			retained.erase(std::unique(retained.begin(), retained.end()), retained.end());
			for (const auto& n : retained)
				if (const auto at = by_name.find(n); at != by_name.end() && !people[at->second].id)
					++spellings[at->second][n];
		}

		const auto limit = static_cast<size_t>(std::max(1, t.exemplars));
		// A portable sample with no picture here is reference evidence, never a displayable face.
		for (const auto& [signature, evidence] : signatures)
		{
			if (signature.model != model || evidence.hidden || represented.contains(signature)) continue;
			const face_exemplar ref{UINT32_MAX, signature.hash, 0.0};
			if (const auto it = by_id.find(evidence.person); it != by_id.end()) positives[it->second].push_back(ref);
			for (const auto id : evidence.rejected)
				if (const auto it = by_id.find(id); it != by_id.end()) negatives[it->second].push_back(ref);
		}
		struct profile
		{
			uint32_t person;
			std::vector<face_exemplar> positive, negative;
		};
		std::vector<profile> profiles;
		for (uint32_t p = 0; p < people.size(); ++p)
		{
			if (cancel()) return {};
			auto& who = people[p];
			if (!spellings[p].empty())
			{
				const auto most = std::ranges::max_element(spellings[p], [](const auto& a, const auto& b)
				{ return a.second != b.second ? a.second < b.second : a.first > b.first; });
				who.name = most->first;
			}
			auto positive = exemplars(std::move(positives[p]), limit);
			if (!positive.empty())
			{
				for (auto& m : who.confirmed)
					if (store->entry(m.face).has_hash) m.score = best_match(store->hash(m.face), m.face, positive);
				profiles.push_back({p, std::move(positive), exemplars(std::move(negatives[p]), limit)});
			}
			choose_samples(who, *store, years);
		}

		// Score against a fixed book. Within a photo give the strongest clear offer first; it can resolve
		// another face's ambiguity, but an offer in one photo can never train offers in another.
		struct offer
		{
			uint32_t person;
			float score, against;
		};

		// docs/faces.md#72-suggestions: every unknown face against every person is nearly all a pass costs, and each
		// face's offers stand apart from every other's, so they are worked out first, on whatever threads `parallel`
		// lends; then settled a photo at a time, in order, as before.
		std::vector<std::vector<offer>> near(o->unknown.size());
		if (!profiles.empty())
		{
			std::atomic_bool stopped = false;
			const auto score = [&](const size_t begin, const size_t end)
			{
				for (auto u = begin; u < end && !stopped.load(std::memory_order_relaxed); ++u)
				{
					if (((u - begin) & 63) == 0 && cancel())
					{
						stopped = true;
						break;
					}
					const auto f = o->unknown[u];
					const auto& e = store->entry(f);
					if (!e.has_hash) continue;
					const auto query = store->hash(f);
					const auto denied = never_offer.find(f);
					const auto busy = occupied.find(e.run);
					auto& offers = near[u];
					for (const auto& pr : profiles)
					{
						if ((denied != never_offer.end() &&
						     std::ranges::find(denied->second, pr.person) != denied->second.end()) ||
						    (busy != occupied.end() &&
						     std::ranges::find(busy->second, pr.person) != busy->second.end()))
							continue;
						const auto s = best_match(query, f, pr.positive);
						if (s >= t.confident - t.margin)
							offers.push_back({pr.person, s, best_match(query, f, pr.negative)});
					}
					std::ranges::stable_sort(offers, std::greater{}, &offer::score);
				}
			};
			if (parallel) parallel(o->unknown.size(), score);
			else score(0, o->unknown.size());
			if (stopped.load() || cancel()) return {};
		}
		// docs/faces.md#95-naming-a-face: each profile stays with its person, for naming faces to rank people by.
		for (auto& pr : profiles)
		{
			people[pr.person].positive = std::move(pr.positive);
			people[pr.person].negative = std::move(pr.negative);
		}

		struct candidate
		{
			size_t at;
			bool offered = false;
		};
		for (size_t u = 0; u < o->unknown.size();)
		{
			if ((u & 255) == 0 && cancel()) return {};
			const auto r = store->entry(o->unknown[u]).run;
			std::vector<candidate> candidates;
			auto at = occupied.find(r);
			auto taken = at != occupied.end() ? at->second : std::vector<uint32_t>{};
			const auto first_face = u;
			for (; u < o->unknown.size() && store->entry(o->unknown[u]).run == r; ++u)
				if (!near[u].empty()) candidates.push_back({u});
			auto offered_any = false;
			for (;;)
			{
				candidate* winner = nullptr;
				offer best{};
				for (auto& c : candidates)
				{
					if (c.offered) continue;
					const offer* first = nullptr;
					auto rival = not_compared;
					for (const auto& l : near[c.at])
					{
						if (std::ranges::find(taken, l.person) != taken.end()) continue;
						if (!first) first = &l;
						else
						{
							rival = l.score;
							break;
						}
					}
					if (first && first->score >= t.confident &&
					    first->score - std::max(rival, first->against) >= t.margin &&
					    (!winner || first->score > best.score))
					{
						winner = &c;
						best = *first;
					}
				}
				if (!winner) break;
				people[best.person].suggested.push_back({o->unknown[winner->at], best.score, member_kind::suggested});
				winner->offered = true;
				offered_any = true;
				taken.push_back(best.person);
			}

			// What a picture's metadata says fills in where resemblance says nothing: a clear resemblance to someone
			// else is the better offer, as the name may be of anyone the picture concerns.
			if (const auto named = named_offers.find(o->unknown[first_face]);
			    named != named_offers.end() && !offered_any)
			{
				auto s = not_compared;
				for (const auto& l : near[first_face])
					if (l.person == named->second) s = l.score;
				people[named->second].suggested.push_back({named->first, s, member_kind::suggested});
			}
		}

		for (auto& p : people)
		{
			core::hash_set<uint32_t> pictures;
			const auto order = [&](std::vector<person_member>& section)
			{
				for (const auto& m : section) pictures.insert(store->entry(m.face).run);
				std::ranges::sort(section, [](const auto& a, const auto& b)
				{ return a.score != b.score ? a.score > b.score : a.face < b.face; });
			};
			order(p.confirmed);
			p.confirmed_pictures = static_cast<uint32_t>(pictures.size());
			order(p.suggested);
			p.pictures = static_cast<uint32_t>(pictures.size());
			for (const auto r : pictures)
			{
				bool in_file = false;
				split2(file_names[r], true, [&](const std::string_view n)
				{
					const auto raw = core::trim(n);
					in_file |= core::icmp(o->roster->canonical_name(raw), p.name) == 0 &&
					           !o->file_name_contradicted(store->run(r).path, raw);
				});
				if (!in_file) ++o->count_adjustments[p.name];
			}
		}
		std::ranges::sort(people, [](const auto& a, const auto& b)
		{
			if (a.confirmed_pictures != b.confirmed_pictures) return a.confirmed_pictures > b.confirmed_pictures;
			return a.pictures != b.pictures ? a.pictures > b.pictures : core::icmp(a.name, b.name) < 0;
		});
		for (uint32_t p = 0; p < people.size(); ++p)
		{
			const auto index = [&](const std::vector<person_member>& section, const bool confirmed)
			{
				for (uint32_t i = 0; i < section.size(); ++i)
				{
					const auto f = section[i].face;
					o->members.emplace(f, overview::membership{p, i, confirmed});
					auto& here = o->picture_people[store->run_of_face(f).path];
					if (here.empty() || here.back().person != p) here.push_back({p, confirmed});
				}
			};
			index(people[p].confirmed, true);
			index(people[p].suggested, false);
		}
		// Unknown is the remaining review work, not a second copy of every person's suggestions.
		std::erase_if(o->unknown, [&](const uint32_t face) { return o->members.contains(face); });
		auto review = order_unknown(*store, o->unknown, t.confident, cancel, parallel);
		o->unknown_by_similarity = std::move(review.faces);
		o->unknown_runs = std::move(review.runs);
		o->unclear_from = review.unclear_from;
		return cancel() ? nullptr : o;
	}

	std::vector<likely_person> likely_people(const overview& o, const std::span<const uint32_t> faces,
	                                         const std::span<const std::string> leave_out, const tuning& t)
	{
		// At most three of them, the first rows of the naming popup's list; and a large selection is judged by faces spread
		// through it, as cheaply as one run.
		constexpr size_t most = 3;
		constexpr size_t compared = 64;
		const auto& store = o.store;
		if (!store) return {};

		std::vector<uint32_t> hashed;
		for (const auto f : faces)
			if (f < store->face_count() && store->entry(f).has_hash) hashed.push_back(f);

		// Each face as the suggestion pass sees it: nobody is offered for a face beside a face confirmed as them in
		// its picture, nor where it is ruled out for them.
		struct query
		{
			uint32_t face = 0;
			face_hash hash;
			std::vector<uint32_t> beside;
			const std::vector<uint32_t>* ruled_out = nullptr;
		};
		std::vector<query> queries;
		const auto count = std::min(compared, hashed.size());
		for (size_t i = 0; i < count; ++i)
		{
			auto& q = queries.emplace_back();
			q.face = hashed[i * hashed.size() / count];
			q.hash = store->hash(q.face);
			const auto& run = store->run(store->entry(q.face).run);
			for (uint32_t k = 0; k < run.count; ++k)
				if (const auto other = run.first + k; other != q.face)
					if (const auto m = o.members.find(other); m != o.members.end() && m->second.confirmed)
						q.beside.push_back(m->second.person);
			if (const auto r = o.ruled_out.find(q.face); r != o.ruled_out.end()) q.ruled_out = &r->second;
		}

		struct ranked
		{
			uint32_t person = 0;
			float score = not_compared;
		};
		std::vector<ranked> ranking;
		for (uint32_t p = 0; p < o.people.size(); ++p)
		{
			const auto& who = o.people[p];
			if (who.positive.empty() || who.name.empty() || std::ranges::any_of(leave_out, [&](const std::string& n) {
				return core::icmp(o.roster ? o.roster->canonical_name(n) : std::string_view(n), who.name) == 0;
			}))
				continue;
			double likeness = 0.0, against = 0.0;
			size_t compared_with = 0, against_with = 0;
			for (const auto& q : queries)
			{
				if (std::ranges::find(q.beside, p) != q.beside.end() ||
				    (who.id && q.ruled_out && std::ranges::binary_search(*q.ruled_out, who.id)))
					continue;
				const auto s = best_match(q.hash, q.face, who.positive);
				if (s == not_compared) continue;
				likeness += s;
				++compared_with;
				if (const auto a = best_match(q.hash, q.face, who.negative); a != not_compared)
				{
					against += a;
					++against_with;
				}
			}
			if (!compared_with) continue;
			const auto score = static_cast<float>(likeness / static_cast<double>(compared_with));
			if (score < t.similar ||
			    (against_with && against / static_cast<double>(against_with) >= static_cast<double>(score)))
				continue;
			ranking.push_back({p, score});
		}
		std::ranges::sort(ranking, [&o](const ranked& a, const ranked& b)
		{
			if (a.score != b.score) return a.score > b.score;
			return core::icmp(o.people[a.person].name, o.people[b.person].name) < 0;
		});

		std::vector<likely_person> result;
		for (const auto& r : ranking)
		{
			if (result.size() == most || (!result.empty() && result.front().score - r.score >= t.margin)) break;
			result.push_back({o.people[r.person].name, r.score});
		}
		return result;
	}
}
