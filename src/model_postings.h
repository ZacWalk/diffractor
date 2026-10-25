// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Compressed posting lists for the search inverted index. Encodes a strictly
// increasing list of document (item) ids as delta + variable-byte (VByte) bytes. The
// trigram index built on top of it accelerates substring prediction in the address box;
// the boolean set operations and the inverted index above them are implemented and tested

#pragma once

namespace df
{
	// Append `v` to `out` using unsigned LEB128 / variable-byte encoding (7 data bits per
	// byte, high bit signals continuation). Small values take one byte.
	inline void append_varint(std::vector<uint8_t>& out, uint32_t v)
	{
		while (v >= 0x80)
		{
			out.push_back(static_cast<uint8_t>(v) | 0x80);
			v >>= 7;
		}

		out.push_back(static_cast<uint8_t>(v));
	}

	// Read one varint starting at `pos`, advancing `pos` past it. Well-formed input is at most
	// five bytes; a longer run is corrupt, so stop rather than shift past the width of the result.
	inline uint32_t read_varint(const std::vector<uint8_t>& bytes, size_t& pos)
	{
		uint32_t v = 0;
		int shift = 0;

		while (pos < bytes.size())
		{
			const uint8_t b = bytes[pos++];
			v |= static_cast<uint32_t>(b & 0x7f) << shift;
			if ((b & 0x80) == 0) break;
			shift += 7;
			if (shift >= 32) break;
		}

		return v;
	}

	// A compressed posting list: the sorted, de-duplicated set of item ids that contain a
	// given term, stored as delta-encoded varints. Building expects ids in increasing order.
	class posting_list
	{
		std::vector<uint8_t> _bytes;
		uint32_t _last = 0; // last id appended (deltas are relative to this)
		uint32_t _count = 0;
		bool _has_last = false;

	public:
		posting_list() = default;

		// Append the next id. Ids must be strictly increasing; duplicates are ignored so a
		// term seen twice for the same item does not add a second posting.
		void add(const uint32_t id)
		{
			if (_has_last)
			{
				if (id <= _last) return; // ignore out-of-order / duplicate
				append_varint(_bytes, id - _last);
			}
			else
			{
				append_varint(_bytes, id); // first delta is the absolute id
			}

			_last = id;
			_has_last = true;
			_count += 1;
		}

		uint32_t count() const { return _count; }
		bool empty() const { return _count == 0; }
		size_t byte_size() const { return _bytes.size(); }
		const std::vector<uint8_t>& bytes() const { return _bytes; }

		// Materialise the ids into a sorted vector.
		std::vector<uint32_t> to_vector() const
		{
			std::vector<uint32_t> result;
			result.reserve(_count);

			size_t pos = 0;
			uint32_t prev = 0;
			bool first = true;

			while (pos < _bytes.size())
			{
				const auto delta = read_varint(_bytes, pos);
				prev = first ? delta : prev + delta;
				first = false;
				result.push_back(prev);
			}

			return result;
		}

		// Invoke `fn(id)` for each posting in ascending order without materialising a vector.
		template <typename Fn>
		void for_each(Fn&& fn) const
		{
			size_t pos = 0;
			uint32_t prev = 0;
			bool first = true;

			while (pos < _bytes.size())
			{
				const auto delta = read_varint(_bytes, pos);
				prev = first ? delta : prev + delta;
				first = false;
				fn(prev);
			}
		}

		static posting_list from_sorted(const std::vector<uint32_t>& sorted_ids)
		{
			posting_list result;
			for (const auto id : sorted_ids) result.add(id);
			return result;
		}
	};

	// Boolean composition over sorted id lists. These map directly onto Diffractor's query
	// semantics: AND of terms (intersect), OR / "with:" (union), and negation / "without:"
	// (difference). Inputs must be sorted ascending; outputs are sorted ascending.
	//
	// NOTE on what is live vs. foundation: `postings_intersect` is used in production by the
	// trigram substring-prediction path. `postings_union` and `postings_difference` are NOT yet
	// wired into production - they are the retained, unit-tested foundation for the deferred
	// term->items search-matching reverse index. Live search *matching* still uses the existing
	// folder/item presence-mask prefilter, so composing OR/AND-NOT over item postings is not
	// needed until (and if) that matcher is profiled against real libraries and migrated. They
	// are kept (rather than removed and re-added) so the composition stays complete and tested.
	inline std::vector<uint32_t> postings_intersect(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
	{
		std::vector<uint32_t> result;
		result.reserve(std::min(a.size(), b.size()));
		std::ranges::set_intersection(a, b, std::back_inserter(result));
		return result;
	}

	inline std::vector<uint32_t> postings_union(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
	{
		std::vector<uint32_t> result;
		result.reserve(a.size() + b.size());
		std::ranges::set_union(a, b, std::back_inserter(result));
		return result;
	}

	inline std::vector<uint32_t> postings_difference(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
	{
		std::vector<uint32_t> result;
		result.reserve(a.size());
		std::ranges::set_difference(a, b, std::back_inserter(result));
		return result;
	}

	// A minimal in-memory inverted index: term -> compressed posting list of the item ids that
	// contain that term. Documents must be added in ascending id order so each term's postings
	// stay sorted. Term lookup is case-insensitive (matching search semantics).
	//
	// STATUS: foundation, not yet wired into production. This is the *exact*-term reverse index
	// intended for a future term->items search-matching path. It is deliberately retained and
	// unit-tested (see should_query_inverted_index) but is NOT used by live search, which keeps
	// its folder/item presence-mask prefilter. Two reasons it is not wired yet:
	//   1. The live text matcher is substring + wildcard across many fields (see compare_text),
	//      which an exact-token index cannot reproduce - substring acceleration is instead done
	//      by trigram_index (below), which IS live for typeahead prediction.
	//   2. Flipping item matching onto an inverted index is a core-search rewrite that overlaps
	//      the existing presence-mask prefilter and should be justified by profiling on a real library.
	// It is kept as a validated building block so that migration is a small, guarded step.
	class inverted_index
	{
		df::hash_map<std::string, posting_list, df::ihash, df::ieq> _terms;
		uint32_t _doc_count = 0;

	public:
		// Add all terms of one document. `id` must be >= any previously added id.
		void add_document(const uint32_t id, const std::vector<std::string_view>& terms)
		{
			for (const auto& t : terms)
			{
				_terms[std::string(t)].add(id);
			}

			_doc_count = std::max(_doc_count, id == UINT32_MAX ? id : id + 1);
		}

		size_t term_count() const { return _terms.size(); }

		// The sorted item ids that contain `term` (empty if the term is unknown).
		std::vector<uint32_t> find(const std::string_view term) const
		{
			const auto found = _terms.find(std::string(term));
			return found == _terms.end() ? std::vector<uint32_t>{} : found->second.to_vector();
		}

		// Total compressed posting bytes across all terms (for sizing / diagnostics).
		size_t byte_size() const
		{
			size_t total = 0;
			for (const auto& kv : _terms) total += kv.second.byte_size();
			return total;
		}
	};

	// Invoke `fn(key)` for each case-folded trigram (3 code points) of `text`. Each key packs
	// three folded code points (21 bits each). Because indexing and querying fold identically,
	// if `q` is a (case-insensitive) substring of `text` then every trigram of `q` is also a
	// trigram of `text` - the property that lets the trigram index generate substring-match
	// candidates with no false negatives.
	template <typename Fn>
	void for_each_trigram(const std::string_view text, Fn&& fn)
	{
		char32_t a = 0;
		char32_t b = 0;
		int filled = 0;
		auto p = text.begin();
		const auto end = text.end();

		while (p < end)
		{
			const auto c = static_cast<char32_t>(str::normalize_for_compare(static_cast<int>(str::pop_utf8_char(p, end))));

			if (filled >= 2)
			{
				fn((static_cast<uint64_t>(a) << 42) | (static_cast<uint64_t>(b) << 21) | static_cast<uint64_t>(c));
			}
			else
			{
				++filled;
			}

			a = b;
			b = c;
		}
	}

	// A substring-search accelerator: maps each case-folded trigram to the posting list of item
	// ids whose text contains it. `candidates(q)` intersects the posting lists of q's trigrams
	// to produce a superset of the true substring matches; the caller then verifies each
	// candidate with str::contains, so results are identical to a full scan - only faster.
	// Queries shorter than three code points cannot be indexed and return nullopt (scan).
	//
	// Two phases: add() every document, then freeze() once. Freezing collapses the whole index
	// into a sorted gram table plus one contiguous posting blob, so the built index costs two
	// allocations rather than a map node and a posting buffer per distinct trigram, and a lookup
	// is a binary search over contiguous memory. Querying before freeze() returns nullopt, which
	// the caller already handles by scanning.
	class trigram_index
	{
		struct gram_entry
		{
			uint64_t gram;
			uint32_t offset; // start of this gram's postings within _postings
			uint32_t count; // ids encoded there
		};

#pragma pack(push, 4)
		// One (gram, document) pair as added. Packed because the build holds one per trigram
		// occurrence in the whole vocabulary, and the natural layout would waste a third of that.
		struct pending_t
		{
			uint64_t gram;
			uint32_t id;
		};
#pragma pack(pop)

		std::vector<pending_t> _pending;
		std::vector<gram_entry> _grams;
		std::vector<uint8_t> _postings;
		uint32_t _doc_count = 0;

		// freeze() must see (gram, id) lexicographic order, and this is the whole build's dominant
		// cost, so it is a stable LSD radix on the gram alone: add() takes ids in ascending order, so
		// the input is already ordered by id and a stable pass by gram completes the key. Every byte
		// histogram is built in one read pass, and a pass whose byte never varies - the high halves of
		// each code point, for anything Latin - is skipped rather than run. Unlike a comparison sort
		// this is not in place: it costs one scratch buffer the size of the input, freed on return.
		static void sort_by_gram(std::vector<pending_t>& v)
		{
			constexpr size_t passes = sizeof(uint64_t);
			const auto n = v.size();
			if (n < 2) return;

			std::array<std::array<size_t, 256>, passes> counts{};

			for (const auto& e : v)
			{
				for (size_t p = 0; p < passes; ++p)
				{
					++counts[p][static_cast<uint8_t>(e.gram >> (p * 8))];
				}
			}

			std::vector<pending_t> scratch;

			for (size_t p = 0; p < passes; ++p)
			{
				// Counts are order-independent, so any present byte answers "do all elements share it".
				if (counts[p][static_cast<uint8_t>(v.front().gram >> (p * 8))] == n) continue;

				if (scratch.size() != n) scratch.resize(n);

				std::array<size_t, 256> at{};
				size_t offset = 0;

				for (size_t b = 0; b < 256; ++b)
				{
					at[b] = offset;
					offset += counts[p][b];
				}

				for (const auto& e : v)
				{
					scratch[at[static_cast<uint8_t>(e.gram >> (p * 8))]++] = e;
				}

				v.swap(scratch);
			}
		}

		template <typename Fn>
		void for_each_posting(const gram_entry& entry, Fn&& fn) const
		{
			size_t pos = entry.offset;
			uint32_t prev = 0;

			for (uint32_t i = 0; i < entry.count; ++i)
			{
				const auto delta = read_varint(_postings, pos);
				prev = i == 0 ? delta : prev + delta;
				fn(prev);
			}
		}

		std::vector<uint32_t> to_vector(const gram_entry& entry) const
		{
			std::vector<uint32_t> result;
			result.reserve(entry.count);
			for_each_posting(entry, [&result](const uint32_t id) { result.emplace_back(id); });
			return result;
		}

	public:
		// Index document `id` (ids must be added in ascending order so postings stay sorted).
		void add(const uint32_t id, const std::string_view text)
		{
			// repeated trigrams need no dedup here - freeze() drops an id it has just emitted
			for_each_trigram(text, [this, id](const uint64_t key) { _pending.emplace_back(key, id); });
			_doc_count = std::max(_doc_count, id == UINT32_MAX ? id : id + 1);
		}

		// Collapse everything added so far into the queryable form. Call once, after the last add().
		void freeze()
		{
			sort_by_gram(_pending);

			_grams.clear();
			_postings.clear();
			_postings.reserve(_pending.size()); // at least one byte per posting

			for (size_t i = 0; i < _pending.size();)
			{
				const auto gram = _pending[i].gram;
				const auto offset = static_cast<uint32_t>(_postings.size());
				uint32_t count = 0;
				uint32_t last = 0;

				while (i < _pending.size() && _pending[i].gram == gram)
				{
					const auto id = _pending[i].id;
					++i;

					if (count != 0 && id <= last) continue; // same gram repeated within one document

					append_varint(_postings, count == 0 ? id : id - last);
					last = id;
					++count;
				}

				_grams.emplace_back(gram, offset, count);
			}

			_pending.clear();
			_pending.shrink_to_fit();
			_grams.shrink_to_fit();
			_postings.shrink_to_fit();
		}

		// Candidate ids that MAY contain `query` (a superset of true substring matches), or
		// nullopt when the query is shorter than a trigram and a full scan is required.
		std::optional<std::vector<uint32_t>> candidates(const std::string_view query) const
		{
			// Not frozen, so there is nothing to intersect: ask the caller to scan rather than
			// claim an empty candidate set, which would silently lose matches.
			if (!_pending.empty()) return std::nullopt;

			std::vector<uint64_t> qgrams;
			for_each_trigram(query, [&qgrams](const uint64_t key) { qgrams.push_back(key); });
			std::ranges::sort(qgrams);
			qgrams.erase(std::ranges::unique(qgrams).begin(), qgrams.end());

			if (qgrams.empty()) return std::nullopt;

			std::vector<const gram_entry*> lists;
			lists.reserve(qgrams.size());

			for (const auto key : qgrams)
			{
				const auto found = std::ranges::lower_bound(_grams, key, {}, &gram_entry::gram);
				if (found == _grams.end() || found->gram != key) return std::vector<uint32_t>{};
				// trigram absent -> no matches
				lists.emplace_back(&*found);
			}

			// Intersect rarest first so the candidate set shrinks fastest, and stream every list
			// but the smallest instead of materialising it. This runs per keystroke.
			std::ranges::sort(lists, {}, [](const gram_entry* l) { return l->count; });

			auto result = to_vector(*lists.front());

			for (size_t i = 1; i < lists.size() && !result.empty(); ++i)
			{
				std::vector<uint32_t> next;
				next.reserve(std::min(result.size(), static_cast<size_t>(lists[i]->count)));

				size_t r = 0;

				for_each_posting(*lists[i], [&result, &next, &r](const uint32_t id)
				{
					while (r < result.size() && result[r] < id) ++r;

					if (r < result.size() && result[r] == id)
					{
						next.emplace_back(id);
						++r;
					}
				});

				result = std::move(next);
			}

			return result;
		}
	};
}
