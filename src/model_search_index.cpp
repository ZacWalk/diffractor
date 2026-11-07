// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Building the search index snapshot and answering searches from it. Every answer either comes
// from a value table read with the very predicate the matcher applies to that value, or from the matcher
// itself - asked once for each value or day a group of documents shares, or about each document - so the
// snapshot finds exactly what a scan finds.

#include "pch.h"

#include "model_search_index.h"
#include "files.h"

namespace
{
	constexpr uint32_t no_value = std::numeric_limits<uint32_t>::max();

	// Enough for every row of a sidebar refresh or the predictions of a keystroke to share their terms,
	// and small enough that a large collection's sets cannot accumulate.
	constexpr size_t max_cached_leaves = 64;
	// The distinct presence requirements of one pass's searches are a handful.
	constexpr size_t max_cached_eligible = 16;

	// A file size as format_size reads it: the unit and the size in tenths of it.
	struct size_key
	{
		const char* unit = nullptr;
		uint64_t tenths = 0;

		bool operator==(const size_key&) const = default;
	};

	struct size_key_hash
	{
		size_t operator()(const size_key& k) const
		{
			return std::hash<uint64_t>{}(k.tenths) ^ std::hash<const void*>{}(k.unit) << 1;
		}
	};

	uint32_t intern_key(df::hash_map<std::string, uint32_t>& ids, std::string key)
	{
		const auto next = static_cast<uint32_t>(ids.size());
		return ids.try_emplace(std::move(key), next).first->second;
	}

	size_t key_map_bytes(const df::hash_map<std::string, uint32_t>& ids)
	{
		auto result = ids.bucket_count() * sizeof(void*);

		for (const auto& id : ids)
		{
			result += sizeof(id) + 2 * sizeof(void*);
			if (id.first.capacity() > sizeof(std::string)) result += id.first.capacity();
		}

		return result;
	}
}

void df::doc_set::set_range(const uint32_t first, const uint32_t count)
{
	if (count == 0) return;

	const auto last = first + count - 1;
	const auto first_word = first >> 6;
	const auto last_word = last >> 6;
	const auto first_mask = ~uint64_t{0} << (first & 63);
	const auto last_mask = ~uint64_t{0} >> (63 - (last & 63));

	if (first_word == last_word)
	{
		_words[first_word] |= first_mask & last_mask;
		return;
	}

	_words[first_word] |= first_mask;
	for (auto w = first_word + 1; w < last_word; ++w) _words[w] = ~uint64_t{0};
	_words[last_word] |= last_mask;
}

void df::doc_set::reset_range(const uint32_t first, const uint32_t count)
{
	if (count == 0) return;

	const auto last = first + count - 1;
	const auto first_word = first >> 6;
	const auto last_word = last >> 6;
	const auto first_mask = ~uint64_t{0} << (first & 63);
	const auto last_mask = ~uint64_t{0} >> (63 - (last & 63));

	if (first_word == last_word)
	{
		_words[first_word] &= ~(first_mask & last_mask);
		return;
	}

	_words[first_word] &= ~first_mask;
	for (auto w = first_word + 1; w < last_word; ++w) _words[w] = 0;
	_words[last_word] &= ~last_mask;
}

void df::doc_set::intersect(const doc_set& other)
{
	assert_true(_size == other._size);
	const auto n = std::min(_words.size(), other._words.size());
	for (size_t i = 0; i < n; ++i) _words[i] &= other._words[i];
}

void df::doc_set::unite(const doc_set& other)
{
	assert_true(_size == other._size);
	const auto n = std::min(_words.size(), other._words.size());
	for (size_t i = 0; i < n; ++i) _words[i] |= other._words[i];
}

void df::doc_set::invert()
{
	for (auto& w : _words) w = ~w;
	clear_tail();
}

bool df::doc_set::any() const
{
	return std::ranges::any_of(_words, [](const uint64_t w) { return w != 0; });
}

uint64_t df::doc_set::count() const
{
	uint64_t result = 0;
	for (const auto w : _words) result += std::popcount(w);
	return result;
}

uint64_t df::doc_set::count_and(const doc_set& other) const
{
	assert_true(_size == other._size);
	const auto n = std::min(_words.size(), other._words.size());
	uint64_t result = 0;
	for (size_t i = 0; i < n; ++i) result += std::popcount(_words[i] & other._words[i]);
	return result;
}

df::value_docs df::value_docs_builder::freeze(const uint32_t value_count)
{
	std::vector<uint32_t> offsets(static_cast<size_t>(value_count) + 1, 0);

	for (const auto& pair : _pairs)
	{
		++offsets[pair.first + 1];
	}

	for (size_t i = 1; i < offsets.size(); ++i)
	{
		offsets[i] += offsets[i - 1];
	}

	std::vector<uint32_t> docs(_pairs.size());

	{
		auto cursor = offsets;

		for (const auto& pair : _pairs)
		{
			docs[cursor[pair.first]++] = pair.second;
		}
	}

	_pairs.clear();
	_pairs.shrink_to_fit();

	// Pairs arrive in document order, so each value's documents are already ascending and a value read
	// twice from one document - a name that is also its file_name field - sits beside itself.
	uint32_t out = 0;

	for (uint32_t value = 0; value < value_count; ++value)
	{
		const auto begin = offsets[value];
		const auto end = offsets[value + 1];
		offsets[value] = out;
		auto last = no_value;

		for (auto i = begin; i < end; ++i)
		{
			if (docs[i] != last)
			{
				last = docs[i];
				docs[out++] = last;
			}
		}
	}

	offsets[value_count] = out;
	docs.resize(out);
	docs.shrink_to_fit();
	return {std::move(offsets), std::move(docs)};
}

std::string df::search_index::current_format_signature()
{
	const auto* const picture = files::file_type_from_name(std::string_view("a.jpg"));
	const auto* const video = files::file_type_from_name(std::string_view("a.mp4"));

	return std::format("{}|{}|{}|{}|{}|{}|{}|{}",
	                   format_size(file_size(uint64_t{1536})),
	                   prop::format_pixels({}, picture),
	                   prop::format_pixels({100, 100}, picture),
	                   prop::format_pixels({500, 500}, picture),
	                   prop::format_pixels({1920, 1080}, video),
	                   prop::format_audio_sample_rate(44100u),
	                   date_t(2024, 1, 15, 12).system_to_local().to_int64(),
	                   date_t(2024, 7, 15, 12).system_to_local().to_int64());
}

std::shared_ptr<const df::search_index> df::search_index::build(const folder_list& folders, const uint64_t generation,
                                                                const std::function<bool()>& abandon)
{
	auto result = std::make_shared<search_index>();
	auto& index = *result;
	index._generation = generation;
	index._format_signature = current_format_signature();
	index._date_day = platform::now().to_days();

	folder_list members;
	members.reserve(folders.size());
	size_t doc_estimate = 0;

	for (const auto& [path, node] : folders)
	{
		if (node && node->is_in_collection.load())
		{
			members.emplace_back(path, node);
			doc_estimate += node->files.size();
		}
	}

	// Path order, so a folder's documents are contiguous and documents compare in the order paths do.
	std::ranges::sort(members, [](const auto& left, const auto& right) { return left.first.compare(right.first) < 0; });

	index._folders.reserve(members.size());
	index._files.reserve(doc_estimate);
	index._doc_folder.reserve(doc_estimate);
	index._metadata.reserve(doc_estimate);
	index._date_keys.reserve(doc_estimate * 3);

	hash_map<uint32_t, uint32_t> text_ids;
	value_docs_builder text_docs;
	value_docs_builder formatted_docs;
	value_docs_builder split_docs;
	value_docs_builder tag_docs;

	// What a rating or label value term reads of a document: whether it has metadata, and the value.
	constexpr auto no_metadata = std::numeric_limits<uint32_t>::max();
	hash_map<uint32_t, uint32_t> rating_ids;
	hash_map<uint32_t, uint32_t> label_ids;
	value_docs_builder rating_docs;
	value_docs_builder label_docs;

	const auto group = [](hash_map<uint32_t, uint32_t>& ids, std::vector<uint32_t>& representatives,
	                      value_docs_builder& docs, const uint32_t value, const uint32_t doc)
	{
		const auto [found, inserted] = ids.try_emplace(value, static_cast<uint32_t>(representatives.size()));
		if (inserted) representatives.emplace_back(doc);
		docs.add(found->second, doc);
	};

	hash_map<uint32_t, uint32_t> date_ids;

	const auto date_id = [&index, &date_ids](const date_t date)
	{
		const auto [found, inserted] = date_ids.try_emplace(date.to_days(), static_cast<uint32_t>(index._date_values.size()));
		if (inserted) index._date_values.emplace_back(date);
		return found->second;
	};

	// Formatting costs far more than a lookup, and a collection holds few distinct sizes once rounded and
	// few distinct numbers per field, so each is formatted once for every document sharing it.
	const auto format_fields = text_format_fields();
	std::vector<hash_map<uint64_t, uint32_t>> format_memo(format_fields.size());
	hash_map<size_key, uint32_t, size_key_hash> size_memo;
	const auto split_fields = text_split_fields();
	std::vector<hash_map<uint32_t, std::vector<uint32_t>>> split_memo(split_fields.size());
	hash_map<uint32_t, std::vector<uint32_t>> tag_memo;

	const auto add_text = [&index, &text_ids, &text_docs](const str::cached value, const uint32_t doc)
	{
		const auto next = static_cast<uint32_t>(index._text_values.size());
		const auto [found, inserted] = text_ids.try_emplace(value.id, next);
		if (inserted) index._text_values.emplace_back(value);
		text_docs.add(found->second, doc);
	};

	const auto formatted_id = [&index](const std::string& text)
	{
		// Empty never equals a term with text, and an empty term is asked of each document instead.
		return text.empty() ? no_value : intern_key(index._formatted_ids, search_fold_key(text));
	};

	for (const auto& [path, node] : members)
	{
		if (abandon()) return nullptr;

		const auto folder = static_cast<uint32_t>(index._folders.size());
		// Read before the files: a change that lands after this read bumps the revision past it, so the
		// folder reads as changed even when the change itself was seen below.
		const auto revision = node->search_revision_snapshot();
		const auto first_doc = static_cast<uint32_t>(index._files.size());

		for (const auto& file : node->files)
		{
			const auto doc = static_cast<uint32_t>(index._files.size());
			auto md = file.metadata.load();

			add_text(file.name, doc);

			group(rating_ids, index._ratings.representatives, rating_docs,
			      md ? static_cast<uint32_t>(static_cast<uint16_t>(md->rating)) : no_metadata, doc);
			group(label_ids, index._labels.representatives, label_docs, md ? md->label.id : no_metadata, doc);

			const auto dates = date_term_keys_of(date_parts_prop::any, file, md.get());
			index._date_keys.push_back(date_id(dates.original));
			index._date_keys.push_back(date_id(dates.created));
			index._date_keys.push_back(date_id(dates.modified));

			const auto rounded = round_size(static_cast<uint64_t>(file.size.to_int64()));
			const auto [size_found, size_inserted] = size_memo.try_emplace(
				size_key{rounded.unit.data(), rounded.n * 10 + rounded.dec}, no_value);
			if (size_inserted) size_found->second = formatted_id(format_size(file.size));
			if (size_found->second != no_value) formatted_docs.add(size_found->second, doc);

			if (md)
			{
				const auto& metadata = *md;

				for (const auto& field : text_contains_fields())
				{
					const auto value = metadata.*field.member;
					if (!value.is_empty()) add_text(value, doc);
				}

				for (size_t i = 0; i < format_fields.size(); ++i)
				{
					const auto& field = format_fields[i];
					const auto [found, inserted] = format_memo[i].try_emplace(field.inputs(metadata, file.ft), no_value);
					if (inserted) found->second = formatted_id(field.format(metadata, file.ft));
					if (found->second != no_value) formatted_docs.add(found->second, doc);
				}

				for (size_t i = 0; i < split_fields.size(); ++i)
				{
					const auto& field = split_fields[i];
					const auto value = metadata.*field.member;
					if (value.is_empty()) continue;

					const auto [found, inserted] = split_memo[i].try_emplace(value.id);
					auto& parts = found->second;

					if (inserted)
					{
						split2(value, true, [&index, &parts](const std::string_view part)
						{
							parts.emplace_back(intern_key(index._split_ids, search_fold_key(part)));
						}, field.separator);
					}

					for (const auto id : parts) split_docs.add(id, doc);
				}

				if (!metadata.tags.is_empty())
				{
					const auto [found, inserted] = tag_memo.try_emplace(metadata.tags.id);
					auto& parts = found->second;

					if (inserted)
					{
						split2(metadata.tags, true, [&index, &parts](const std::string_view part)
						{
							parts.emplace_back(intern_key(index._tag_ids, search_fold_key(str::trim(part))));
						});
					}

					for (const auto id : parts) tag_docs.add(id, doc);
				}
			}

			index._files.emplace_back(&file);
			index._doc_folder.emplace_back(folder);
			index._metadata.emplace_back(std::move(md));
		}

		index._folder_by_node.emplace(node.get(), folder);
		index._folder_by_path.emplace(path, folder);
		index._folders.push_back({path, node, revision, first_doc, static_cast<uint32_t>(index._files.size()) - first_doc});
	}

	index._listed_docs = doc_set(index.doc_count(), true);

	for (uint32_t doc = 0; doc < index.doc_count(); ++doc)
	{
		if (index._files[doc]->flags && index_item_flags::is_sidecar) index._listed_docs.reset(doc);
	}

	index._text_docs = text_docs.freeze(static_cast<uint32_t>(index._text_values.size()));
	index._formatted_docs = formatted_docs.freeze(static_cast<uint32_t>(index._formatted_ids.size()));
	index._split_docs = split_docs.freeze(static_cast<uint32_t>(index._split_ids.size()));
	index._tag_docs = tag_docs.freeze(static_cast<uint32_t>(index._tag_ids.size()));
	index._ratings.docs = rating_docs.freeze(static_cast<uint32_t>(index._ratings.representatives.size()));
	index._labels.docs = label_docs.freeze(static_cast<uint32_t>(index._labels.representatives.size()));

	// contains_term compares the NFC form of a value, so that is the form whose trigrams are indexed.
	for (uint32_t id = 0; id < index._text_values.size(); ++id)
	{
		if ((id & 0xfff) == 0 && abandon()) return nullptr;

		const auto value = index._text_values[id].sv();
		const auto normalized = nfc_for_match(value);
		index._text_trigrams.add(id, normalized.empty() ? value : std::string_view(normalized));
	}

	index._text_trigrams.freeze();

	index._bytes = sizeof(search_index) +
		index._folders.capacity() * sizeof(folder_entry) +
		index._folder_by_node.bucket_count() * sizeof(void*) +
		index._folder_by_node.size() * (sizeof(std::pair<const index_folder_item*, uint32_t>) + 2 * sizeof(void*)) +
		index._folder_by_path.bucket_count() * sizeof(void*) +
		index._folder_by_path.size() * (sizeof(std::pair<folder_path, uint32_t>) + 2 * sizeof(void*)) +
		index._files.capacity() * sizeof(const index_file_item*) +
		index._doc_folder.capacity() * sizeof(uint32_t) +
		index._metadata.capacity() * sizeof(prop::item_metadata_ptr) +
		index._listed_docs.byte_size() +
		index._text_values.capacity() * sizeof(str::cached) +
		index._text_docs.byte_size() +
		index._text_trigrams.byte_size() +
		key_map_bytes(index._formatted_ids) + index._formatted_docs.byte_size() +
		key_map_bytes(index._split_ids) + index._split_docs.byte_size() +
		key_map_bytes(index._tag_ids) + index._tag_docs.byte_size() +
		index._ratings.docs.byte_size() + index._ratings.representatives.capacity() * sizeof(uint32_t) +
		index._labels.docs.byte_size() + index._labels.representatives.capacity() * sizeof(uint32_t) +
		index._date_keys.capacity() * sizeof(uint32_t) + index._date_values.capacity() * sizeof(date_t);

	return result;
}

std::optional<uint32_t> df::search_index::folder_of(const index_folder_item* node) const
{
	const auto found = _folder_by_node.find(node);
	if (found == _folder_by_node.end()) return std::nullopt;
	return found->second;
}

std::optional<uint32_t> df::search_index::find_folder(const folder_path path) const
{
	const auto found = _folder_by_path.find(path);
	if (found == _folder_by_path.end()) return std::nullopt;
	return found->second;
}

std::optional<uint32_t> df::search_index::find_doc(const uint32_t folder, const str::cached name) const
{
	const auto& entry = _folders[folder];
	const auto first = _files.begin() + entry.first_doc;
	const auto last = first + entry.doc_count;
	const auto found = std::lower_bound(first, last, name.sv(), [](const index_file_item* file, const std::string_view n)
	{
		return icmp(file->name, n) < 0;
	});

	if (found == last || icmp((*found)->name, name) != 0) return std::nullopt;
	return static_cast<uint32_t>(found - _files.begin());
}

bool df::search_index::is_indexed_term(const search_term& term, const bool formats_current,
                                       const bool dates_current) const
{
	if (groups_of(term)) return true;
	if (term.is_date()) return dates_current;

	// An empty or wildcard term matches by other rules than equality and containment, so the documents
	// are asked one by one.
	if (term.text.empty() || term._is_wildcard) return false;
	if (term.type == search_term_type::text) return formats_current;

	// A tag: term compares equality, which no ordering modifier changes the meaning of.
	return term.type == search_term_type::value && term.key == prop::tag &&
		!term.modifiers.greater_than && !term.modifiers.less_than;
}

// compare_val reads a rating or label term's own property and nothing else, so every document holding a
// value answers such a term alike, whatever its modifiers, wildcards or quoting.
const df::search_index::property_groups* df::search_index::groups_of(const search_term& term) const
{
	if (term.type != search_term_type::value) return nullptr;
	if (term.key == prop::rating) return &_ratings;
	if (term.key == prop::label) return &_labels;
	return nullptr;
}

df::doc_set df::search_index::evaluate_groups(const property_groups& groups, const search_matcher& matcher,
                                              const search_term& term) const
{
	doc_set result(doc_count());

	for (uint32_t id = 0; id < groups.representatives.size(); ++id)
	{
		const auto doc = groups.representatives[id];
		const auto& folder = _folders[_doc_folder[doc]];

		if (matcher.match_term(folder.path.text(), *_files[doc], _metadata[doc].get(), term).is_match())
		{
			groups.docs.add_to(result, id);
		}
	}

	return result;
}

// What match_term answers for a date term, with each document's keys as days: date_term_found decides
// which keys the term tests, and each day is compared once, however many documents fall on it.
df::doc_set df::search_index::evaluate_dates(const search_matcher& matcher, const search_term& term) const
{
	doc_set result(doc_count());
	std::vector<int8_t> answers(_date_values.size(), -1);

	const auto matches = [this, &matcher, &term, &answers](const uint32_t id)
	{
		auto& answer = answers[id];
		if (answer < 0) answer = matcher.date_key_matches(term, _date_values[id]) ? 1 : 0;
		return answer != 0;
	};

	const auto target = term.date_val.target;
	const auto positive = term.modifiers.positive;

	for (uint32_t doc = 0; doc < doc_count(); ++doc)
	{
		const auto* const keys = _date_keys.data() + static_cast<size_t>(doc) * 3;
		if (date_term_found(target, keys[0], keys[1], keys[2], matches) == positive) result.set(doc);
	}

	return result;
}

df::doc_set df::search_index::evaluate_term(const search_matcher& matcher, const search_term& term,
                                            const doc_set& within, const bool formats_current,
                                            const bool dates_current, const cancel_token& token) const
{
	if (const auto* const groups = groups_of(term)) return evaluate_groups(*groups, matcher, term);
	if (term.is_date()) return dates_current ? evaluate_dates(matcher, term) : evaluate_each(matcher, term, within, token);

	if (is_indexed_term(term, formats_current, dates_current))
	{
		return term.type == search_term_type::text ? evaluate_text(matcher, term, token) : evaluate_tag(term);
	}

	return evaluate_each(matcher, term, within, token);
}

// What search_matcher::match_term answers for a free-text term, value by value: compare_text matches an
// item when the term is inside its name or a text field, equals its size or a number as displayed, or
// equals one entry of its tags or artists - and a folder's path decides the term for all it holds.
df::doc_set df::search_index::evaluate_text(const search_matcher& matcher, const search_term& term,
                                            const cancel_token& token) const
{
	doc_set result(doc_count());

	const auto query = term._nfc_text.empty() ? std::string_view(term.text) : std::string_view(term._nfc_text);
	const auto candidates = _text_trigrams.candidates(query);

	// A term too short for trigrams is looked for in every distinct value, long enough in a large
	// collection for a newer keystroke to want it stopped. A cancelled search's answer is discarded.
	uint32_t checked = 0;
	const auto add_if_contains = [this, &term, &result, &token, &checked](const uint32_t id)
	{
		if (text_term_contains(term, _text_values[id])) _text_docs.add_to(result, id);
		return (++checked & 0xfff) != 0 || !token.is_cancelled();
	};

	if (candidates)
	{
		for (const auto id : *candidates)
		{
			if (!add_if_contains(id)) return result;
		}
	}
	else
	{
		for (uint32_t id = 0; id < _text_values.size(); ++id)
		{
			if (!add_if_contains(id)) return result;
		}
	}

	const auto key = search_fold_key(term.text);

	if (const auto found = _formatted_ids.find(key); found != _formatted_ids.end())
	{
		_formatted_docs.add_to(result, found->second);
	}

	if (const auto found = _split_ids.find(key); found != _split_ids.end())
	{
		_split_docs.add_to(result, found->second);
	}

	for (const auto& folder : _folders)
	{
		const auto folder_name = folder.path.text();
		const auto decides = matcher.text_term_matches_folder(term, folder_name) ||
			(!term.modifiers.positive && text_term_contains(term, folder_name));

		if (decides) result.set_range(folder.first_doc, folder.doc_count);
	}

	if (!term.modifiers.positive) result.invert();
	return result;
}

df::doc_set df::search_index::evaluate_tag(const search_term& term) const
{
	doc_set result(doc_count());

	if (const auto found = _tag_ids.find(search_fold_key(term.text)); found != _tag_ids.end())
	{
		_tag_docs.add_to(result, found->second);
	}

	if (!term.modifiers.positive) result.invert();
	return result;
}

df::doc_set df::search_index::evaluate_each(const search_matcher& matcher, const search_term& term,
                                            const doc_set& within, const cancel_token& token) const
{
	doc_set result(doc_count());
	uint32_t checked = 0;
	auto cancelled = false;

	within.for_each([&](const uint32_t doc)
	{
		if (cancelled) return;

		if ((++checked & 0xfff) == 0 && token.is_cancelled())
		{
			cancelled = true;
			return;
		}

		const auto folder_name = _folders[_doc_folder[doc]].path.text();

		if (matcher.match_term(folder_name, *_files[doc], _metadata[doc].get(), term).is_match())
		{
			result.set(doc);
		}
	});

	return result;
}

df::search_index_view::search_index_view(const search_index& index, const search_index::folder_list& folders) :
	_clean(index.folders().size(), 0), _clean_docs(index.doc_count())
{
	for (uint32_t position = 0; position < folders.size(); ++position)
	{
		const auto& node = folders[position].second;
		if (!node || !node->is_in_collection.load()) continue;

		_collection_doc_count += node->files.size();

		const auto entry = index.folder_of(node.get());

		if (!entry || index.folders()[*entry].revision != node->search_revision_snapshot())
		{
			_unclean_folders.emplace_back(position);
			continue;
		}

		const auto& folder = index.folders()[*entry];
		_clean[*entry] = 1;
		_clean_docs.set_range(folder.first_doc, folder.doc_count);
		_clean_doc_count += folder.doc_count;
	}
}

bool df::search_index_view::is_clean(const search_index& index, const index_folder_item* node) const
{
	const auto entry = index.folder_of(node);
	return entry && _clean[*entry] != 0;
}

df::search_index_evaluator::search_index_evaluator(std::shared_ptr<const search_index> index,
                                                   const search_index::folder_list& folders) :
	_index(std::move(index)),
	_view(*_index, folders),
	_formats_current(_index->format_signature() == search_index::current_format_signature()),
	_dates_current(_formats_current && _index->date_day() == platform::now().to_days()),
	_base(_view.clean_docs())
{
	_base.intersect(_index->listed_docs());
}

df::doc_set df::search_index_evaluator::eligible_docs(const search_matcher& matcher)
{
	// The presence prefilter rejects only what lacks a bit the search requires; with none required,
	// every document passes.
	const auto required = matcher.required_presence().types;
	if (required == 0) return _base;

	const auto can_match_folder = matcher.can_match_folder;
	const auto found = std::ranges::find_if(_eligible, [required, can_match_folder](const cached_eligible& eligible)
	{
		return eligible.required_presence == required && eligible.can_match_folder == can_match_folder;
	});

	if (found != _eligible.end()) return found->docs;

	auto result = _base;

	for (const auto& folder : _index->folders())
	{
		if (!can_match_folder && !matcher.can_contain(folder.node->search_presence_summary.load()))
		{
			result.reset_range(folder.first_doc, folder.doc_count);
		}
	}

	result.for_each([this, &matcher, &result](const uint32_t doc)
	{
		if (!matcher.can_contain(_index->file(doc).search_presence.load())) result.reset(doc);
	});

	if (_eligible.size() < max_cached_eligible) _eligible.push_back({required, can_match_folder, result});
	return result;
}

df::doc_set df::search_index_evaluator::leaf_docs(const search_matcher& matcher, const search_term& term,
                                                  const doc_set& within, const bool within_is_eligible,
                                                  const cancel_token& token)
{
	// Grouping and the operator joining a term to the last are the combination's business, not the
	// term's, so they are not part of what identifies its answer.
	auto key = term;
	key.modifiers.logical_op = search_term_modifier_bool::none;
	key.modifiers.begin_group = 0;
	key.modifiers.end_group = 0;

	const auto required = matcher.required_presence().types;
	const auto can_match_folder = matcher.can_match_folder;

	const auto found = std::ranges::find_if(_leaves, [&key, required, can_match_folder](const cached_leaf& leaf)
	{
		return leaf.required_presence == required && leaf.can_match_folder == can_match_folder && leaf.term == key;
	});

	if (found != _leaves.end()) return found->docs;

	const auto indexed = _index->is_indexed_term(term, _formats_current, _dates_current);
	auto docs = _index->evaluate_term(matcher, term, within, _formats_current, _dates_current, token);

	// A set asked only of documents an earlier term had already narrowed to is right for this search
	// alone, so only answers covering every eligible document are kept for the next.
	if ((indexed || within_is_eligible) && !token.is_cancelled())
	{
		if (_leaves.size() >= max_cached_leaves) _leaves.erase(_leaves.begin());
		_leaves.push_back({std::move(key), required, can_match_folder, docs});
	}

	return docs;
}

df::doc_set df::search_index_evaluator::evaluate(const search_matcher& matcher, const cancel_token& token)
{
	const auto& terms = matcher.search().terms();
	if (terms.empty()) return doc_set(_index->doc_count());

	const auto eligible = eligible_docs(matcher);

	const auto is_conjunction = std::ranges::all_of(terms, [](const search_term& term)
	{
		return term.modifiers.begin_group == 0 &&
			term.modifiers.end_group == 0 &&
			term.modifiers.logical_op != search_term_modifier_bool::m_or;
	});

	if (is_conjunction)
	{
		// AND gives one answer in any order, so indexed terms go first and a term asked of each document
		// is asked only of the documents every earlier term kept.
		std::vector<const search_term*> ordered;
		ordered.reserve(terms.size());
		for (const auto& term : terms) ordered.emplace_back(&term);

		std::ranges::stable_partition(ordered, [this](const search_term* term)
		{
			return _index->is_indexed_term(*term, _formats_current, _dates_current);
		});

		auto result = eligible;
		auto narrowed = false;

		for (const auto* term : ordered)
		{
			if (token.is_cancelled() || !result.any()) break;
			result.intersect(leaf_docs(matcher, *term, result, !narrowed, token));
			narrowed = true;
		}

		return result;
	}

	auto result = combine_search_terms<doc_set>(
		terms,
		[this, &matcher, &eligible, &token](const search_term& term)
		{
			return leaf_docs(matcher, term, eligible, true, token);
		},
		[](doc_set& state, const doc_set& value) { state.intersect(value); },
		[](doc_set& state, const doc_set& value) { state.unite(value); },
		[size = _index->doc_count()] { return doc_set(size, true); });

	result.intersect(eligible);
	return result;
}
