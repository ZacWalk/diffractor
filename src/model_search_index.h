// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The search index - an immutable snapshot of the collection laid out to answer a search a set
// at a time - and its evaluation. Items are numbered in path order, the distinct values free-text and tag
// terms read are mapped to the items holding them, rating and label terms are answered once per value and
// date terms once per day, and a search's terms become sets combined through the same steps the matcher
// applies to one item, so an answer from here is the answer a scan of every item gives. index_state
// builds and publishes it and scans the folders that changed since it was read.

#pragma once

#include "model_items.h"
#include "model_postings.h"
#include "model_search.h"

namespace df
{
	// A set of search index documents, one bit per document.
	class doc_set
	{
		std::vector<uint64_t> _words;
		uint32_t _size = 0;

	public:
		doc_set() noexcept = default;

		explicit doc_set(const uint32_t size, const bool fill = false) :
			_words((static_cast<size_t>(size) + 63) / 64, fill ? ~uint64_t{0} : uint64_t{0}), _size(size)
		{
			if (fill) clear_tail();
		}

		uint32_t size() const
		{
			return _size;
		}

		void set(const uint32_t doc)
		{
			_words[doc >> 6] |= uint64_t{1} << (doc & 63);
		}

		void reset(const uint32_t doc)
		{
			_words[doc >> 6] &= ~(uint64_t{1} << (doc & 63));
		}

		bool test(const uint32_t doc) const
		{
			return (_words[doc >> 6] >> (doc & 63) & 1) != 0;
		}

		void set_range(uint32_t first, uint32_t count);
		void reset_range(uint32_t first, uint32_t count);
		void intersect(const doc_set& other);
		void unite(const doc_set& other);
		void invert();
		bool any() const;
		uint64_t count() const;
		// The documents in both, counted without building the intersection.
		uint64_t count_and(const doc_set& other) const;

		template <typename Fn>
		void for_each(Fn&& fn) const
		{
			for (size_t w = 0; w < _words.size(); ++w)
			{
				auto bits = _words[w];

				while (bits != 0)
				{
					fn(static_cast<uint32_t>(w * 64 + std::countr_zero(bits)));
					bits &= bits - 1;
				}
			}
		}

		size_t byte_size() const
		{
			return _words.capacity() * sizeof(uint64_t);
		}

	private:
		void clear_tail()
		{
			if (const auto bits = _size % 64; bits != 0) _words.back() &= (uint64_t{1} << bits) - 1;
		}
	};

	// Each value's documents, ascending, held contiguously.
	class value_docs
	{
		std::vector<uint32_t> _offsets;
		std::vector<uint32_t> _docs;

	public:
		value_docs() noexcept = default;

		value_docs(std::vector<uint32_t> offsets, std::vector<uint32_t> docs) noexcept :
			_offsets(std::move(offsets)), _docs(std::move(docs))
		{
		}

		std::span<const uint32_t> docs(const uint32_t value) const
		{
			return {_docs.data() + _offsets[value], _docs.data() + _offsets[value + 1]};
		}

		void add_to(doc_set& set, const uint32_t value) const
		{
			for (const auto doc : docs(value)) set.set(doc);
		}

		size_t byte_size() const
		{
			return (_offsets.capacity() + _docs.capacity()) * sizeof(uint32_t);
		}
	};

	// Collects (value, document) pairs in document order and lays them out as value_docs, each value's
	// documents ascending and each document once.
	class value_docs_builder
	{
		std::vector<std::pair<uint32_t, uint32_t>> _pairs;

	public:
		void add(const uint32_t value, const uint32_t doc)
		{
			_pairs.emplace_back(value, doc);
		}

		value_docs freeze(uint32_t value_count);
	};

	class search_index final
	{
	public:
		using folder_list = std::vector<std::pair<core::folder_path, index_folder_item_ptr>>;

		struct folder_entry
		{
			core::folder_path path;
			// Holds the node alive, so every document's record stays valid as long as the snapshot is.
			index_folder_item_ptr node;
			// The node's search revision when it was read: what it held then holds while this does.
			uint64_t revision = 0;
			uint32_t first_doc = 0;
			uint32_t doc_count = 0;
		};

		// Reads every collection folder in `folders`. Answers null when `abandon` turns true part way, so a
		// build that indexing has overtaken stops rather than competing with it.
		static std::shared_ptr<const search_index> build(const folder_list& folders, uint64_t generation,
		                                                 const std::function<bool()>& abandon);

		// What free-text terms compare formatted numbers in - the decimal separator and the translated words
		// some formats use - and the time zone date keys are converted in. A snapshot read in another
		// answers those terms by asking each document.
		static std::string current_format_signature();

		uint32_t doc_count() const
		{
			return static_cast<uint32_t>(_files.size());
		}

		uint64_t generation() const
		{
			return _generation;
		}

		size_t byte_size() const
		{
			return _bytes;
		}

		const std::string& format_signature() const
		{
			return _format_signature;
		}

		std::span<const folder_entry> folders() const
		{
			return _folders;
		}

		// Every document but sidecars, which a search lists only beside a related item.
		const doc_set& listed_docs() const
		{
			return _listed_docs;
		}

		std::optional<uint32_t> folder_of(const index_folder_item* node) const;
		std::optional<uint32_t> find_folder(core::folder_path path) const;
		// The document holding this file, when its folder is one the snapshot read.
		std::optional<uint32_t> find_doc(uint32_t folder, core::cached name) const;

		uint32_t doc_folder(const uint32_t doc) const
		{
			return _doc_folder[doc];
		}

		const index_file_item& file(const uint32_t doc) const
		{
			return *_files[doc];
		}

		const prop::item_metadata* metadata(const uint32_t doc) const
		{
			return _metadata[doc].get();
		}

		core::file_path path(const uint32_t doc) const
		{
			return _folders[_doc_folder[doc]].path.combine_file(_files[doc]->name);
		}

		// A term answered from the value tables rather than by asking each document. Date terms are
		// answered so only while `dates_current`: on the day the snapshot was read, in its time zone.
		bool is_indexed_term(const search_term& term, bool formats_current, bool dates_current) const;

		// One term's documents, as search_matcher::match_term answers each. Documents outside `within`
		// are only answered when the term is indexed; otherwise they are left out.
		doc_set evaluate_term(const search_matcher& matcher, const search_term& term, const doc_set& within,
		                      bool formats_current, bool dates_current, const core::cancel_token& token) const;

		// The day the snapshot was read on.
		uint32_t date_day() const
		{
			return _date_day;
		}

	private:
		doc_set evaluate_text(const search_matcher& matcher, const search_term& term, const core::cancel_token& token) const;
		doc_set evaluate_tag(const search_term& term) const;
		doc_set evaluate_each(const search_matcher& matcher, const search_term& term, const doc_set& within,
		                      const core::cancel_token& token) const;

		std::vector<folder_entry> _folders;
		core::hash_map<const index_folder_item*, uint32_t> _folder_by_node;
		core::hash_map<core::folder_path, uint32_t, core::ihash, core::ieq> _folder_by_path;
		std::vector<const index_file_item*> _files;
		std::vector<uint32_t> _doc_folder;
		// Read once, as the folder's revision was read; while the revision holds, so does every pointer.
		std::vector<prop::item_metadata_ptr> _metadata;
		doc_set _listed_docs;

		// The distinct names and text fields a free-text term searches inside, with a trigram index over
		// them so a term is checked against the few values that could contain it.
		std::vector<core::cached> _text_values;
		value_docs _text_docs;
		trigram_index _text_trigrams;

		// The sizes and numbers a free-text term matches whole, as displayed, keyed by search_fold_key.
		core::hash_map<std::string, uint32_t> _formatted_ids;
		value_docs _formatted_docs;
		std::string _format_signature;

		// The entries of tags and artists a free-text term matches one of, keyed by search_fold_key.
		core::hash_map<std::string, uint32_t> _split_ids;
		value_docs _split_docs;

		// Tags as a tag: term compares them, trimmed, keyed by search_fold_key.
		core::hash_map<std::string, uint32_t> _tag_ids;
		value_docs _tag_docs;

		// Documents grouped by the value of a property its value terms read nothing else of an item for,
		// so a term is asked of one document holding each value and that answer stands for the rest.
		struct property_groups
		{
			value_docs docs;
			std::vector<uint32_t> representatives;
		};

		property_groups _ratings;
		property_groups _labels;

		// Each document's original, created and modified date keys, three to a document, as the day each
		// falls on: an index into _date_values, which holds one date of that day. A date term compares
		// only the day, so each day is asked once rather than each document converting its stamps.
		std::vector<uint32_t> _date_keys;
		std::vector<core::date_t> _date_values;
		// Which keys date_term_keys_of chooses, and what is a valid date, depend on the day it is.
		uint32_t _date_day = 0;

		const property_groups* groups_of(const search_term& term) const;
		doc_set evaluate_groups(const property_groups& groups, const search_matcher& matcher,
		                        const search_term& term) const;
		doc_set evaluate_dates(const search_matcher& matcher, const search_term& term) const;

		uint64_t _generation = 0;
		size_t _bytes = 0;
	};

	// Which of a snapshot's folders still hold exactly what it read, against one listing of the index.
	class search_index_view
	{
		std::vector<uint8_t> _clean;
		doc_set _clean_docs;
		uint64_t _clean_doc_count = 0;
		uint64_t _collection_doc_count = 0;
		// Positions, in the folder list the view was taken from, of the collection folders the snapshot
		// does not answer for: changed since it was read, or never read by it.
		std::vector<uint32_t> _unclean_folders;

	public:
		search_index_view(const search_index& index, const search_index::folder_list& folders);

		bool is_clean(const search_index& index, const index_folder_item* node) const;

		bool is_clean_entry(const uint32_t folder) const
		{
			return _clean[folder] != 0;
		}

		const std::vector<uint32_t>& unclean_folders() const
		{
			return _unclean_folders;
		}

		const doc_set& clean_docs() const
		{
			return _clean_docs;
		}

		uint64_t clean_doc_count() const
		{
			return _clean_doc_count;
		}

		// Every document the collection holds now, answered by the snapshot or not.
		uint64_t collection_doc_count() const
		{
			return _collection_doc_count;
		}

		// Read by the snapshot but changed since, plus held now but never read by it.
		uint64_t stale_doc_count(const search_index& index) const
		{
			return index.doc_count() - _clean_doc_count + (_collection_doc_count - _clean_doc_count);
		}

		// Most of the collection is as the snapshot read it. Past that, scanning the changed folders on top
		// would cost more than the scan the snapshot exists to replace.
		bool is_useful() const
		{
			return _clean_doc_count * 2 >= _collection_doc_count;
		}
	};

	// Answers searches against one snapshot and one view of it. A term repeated across the searches one
	// evaluator is asked - a sidebar's rows, a keystroke's predictions - is answered once.
	class search_index_evaluator
	{
		std::shared_ptr<const search_index> _index;
		search_index_view _view;
		bool _formats_current = false;
		bool _dates_current = false;
		// Documents in clean folders that are not sidecars: every search starts from these.
		doc_set _base;

		struct cached_leaf
		{
			search_term term;
			uint32_t required_presence = 0;
			bool can_match_folder = false;
			doc_set docs;
		};

		std::vector<cached_leaf> _leaves;

		// The documents through each presence prefilter asked of this evaluator. Most of the searches one
		// evaluator is asked share a requirement - every tag row requires tags - and reading each
		// document's presence is the one step whose cost grows with the collection.
		struct cached_eligible
		{
			uint32_t required_presence = 0;
			bool can_match_folder = false;
			doc_set docs;
		};

		std::vector<cached_eligible> _eligible;

	public:
		search_index_evaluator(std::shared_ptr<const search_index> index, const search_index::folder_list& folders);

		const search_index& index() const
		{
			return *_index;
		}

		const search_index_view& view() const
		{
			return _view;
		}

		// Answers only while its view is useful.
		bool is_useful() const
		{
			return _view.is_useful();
		}

		// The snapshot documents a related-free, selector-free search matches: in a clean folder, not a
		// sidecar, through the presence prefilter, and matched by its terms combined as the matcher
		// combines them.
		doc_set evaluate(const search_matcher& matcher, const core::cancel_token& token);

	private:
		doc_set eligible_docs(const search_matcher& matcher);
		doc_set leaf_docs(const search_matcher& matcher, const search_term& term, const doc_set& within,
		                  bool within_is_eligible, const core::cancel_token& token);
	};
}
