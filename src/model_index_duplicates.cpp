// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Duplicate prediction and presence. Groups the index by identity - byte-identical CRC, then
// perceptual hash within capture time and picture shape - to predict duplicate sets, asks for the
// hashes a set still needs, and answers whether an item is already present in the collection.

#include "pch.h"

#include "model_index.h"
#include "model_index_internal.h"
#include "model_locations.h"
#include "model_property.h"
#include "model.h"
#include "util_text.h"

// Capture time is recorded to the second and cameras shoot faster than that. A picture that matches
// this many others in the SAME orientation under one timestamp is a burst frame rather than a
// re-save: continuous shooting produces frames that match each other at any threshold. A turned
// match is never a burst frame, so it is not counted here (docs/collections.md section 7.3).
constexpr size_t max_similar_pictures_at_one_capture_time = 2;

// A work bound, not a judgement: past this a capture time is unambiguously continuous shooting, and
// hashing every frame of it would decode a great deal to reach a refusal. Duplicate search and
// presence share it, so neither can report a set the other cannot see.
constexpr size_t max_photos_sharing_capture_time = 8;

// Hashing reads and decodes, so a pass asks for a bounded batch and the pass that follows asks for
// the next. A large collection converges over several rounds rather than stalling on the first.
constexpr size_t max_phash_requests_per_pass = 256;

// A picture worth this much I/O to identify. Beyond it the read costs more than the answer is worth.
// The hash reads the file whole, so the bound is also the most a whole read may hold: above that the
// read answers empty, and a file refused for its size was recorded as one that failed to decode.
constexpr uint64_t max_phash_file_bytes = df::max_blob_size;

// Runs once an anchor has been chosen and before its hash is compared, where a concurrent rescan can
// clear the record's hash.
std::function<void(const df::index_file_item& anchor)> test_after_duplicate_anchor;


static auto next_dup_group = 1000u;

static __forceinline uint32_t x64to32(const uint64_t n)
{
	constexpr uint64_t fnv_prime = 1099511628211u;
	constexpr uint64_t fnv_offset_basis = 14695981039346656037u;
	const auto result = (fnv_offset_basis ^ n) * fnv_prime;
	return static_cast<uint32_t>(result);
}


std::vector<std::pair<df::file_path, df::index_file_item>> index_state::duplicate_list(const uint32_t group) const
{
	std::vector<std::pair<df::file_path, df::index_file_item>> result;
	const auto folders = _items.all_folders();

	for (const auto& ifn : folders)
	{
		if (ifn.second->is_in_collection)
		{
			for (const auto& file : ifn.second->files)
			{
				if (file.duplicates.load().group == group)
				{
					result.emplace_back(df::file_path(ifn.first, file.name), file);
				}
			}
		}
		else
		{
			for (const auto& file : ifn.second->files)
			{
				_items.update_file(df::file_path(ifn.first, file.name), [](const df::index_folder_item_ptr&,
				                                                           const df::index_file_item& current_file)
				{
					if (current_file.exact_duplicate_group.load() == 0) return false;
					current_file.exact_duplicate_group = 0;
					return true;
				});
			}
		}
	}

	return result;
}

void index_state::update_predictions()
{
	const auto folders = _items.all_folders();
	const auto start_ms = df::now_ms();
	const auto folder_count = folders.size();
	// dup on:
	// filename
	// metadata created
	// file created and size
	// crc32c
	// the same picture at the same capture time, by perceptual hash


	struct duplicate_candidate
	{
		const df::index_folder_item_ptr& folder;
		const df::index_file_item* file;
		df::folder_path path;
	};

	std::vector<duplicate_candidate> files;

	enum class duplicate_evidence_kind : uint8_t
	{
		crc,
		av_name_size,
		name_created,
	};

	struct duplicate_evidence
	{
		duplicate_evidence_kind kind = duplicate_evidence_kind::crc;
		std::string name;
		uint64_t first = 0;
		uint64_t second = 0;
		size_t index = 0;
		df::copy_grade grade = df::copy_grade::none;

		auto key() const
		{
			return std::tie(kind, name, first, second);
		}
	};

	std::vector<duplicate_evidence> dups;

	{
		// Three complete evidence tuples per file - reserve from the real count rather than a fixed guess.
		size_t indexed_file_count = 0;

		for (const auto& ifn : folders)
		{
			if (ifn.second->is_in_collection) indexed_file_count += ifn.second->files.size();
		}

		dups.reserve(indexed_file_count * 3);
		files.reserve(indexed_file_count);
	}

	int indexed_crc_count = 0;

	for (const auto& ifn : folders)
	{
		if (ifn.second->is_in_collection)
		{
			for (const auto& file : ifn.second->files)
			{
				if (df::is_closing) return;
				const auto file_index = files.size();
				files.push_back({ifn.second, &file, ifn.first});

				if (file.crc32c)
				{
					dups.emplace_back(duplicate_evidence_kind::crc, std::string{}, file.crc32c,
					                  static_cast<uint64_t>(file.size.to_int64()), file_index,
					                  df::copy_grade::identical);
					++indexed_crc_count;
				}

				if (file.ft->has_trait(file_traits::av) && file.size.to_int64() != 0)
				{
					dups.emplace_back(duplicate_evidence_kind::av_name_size, str::to_lower(file.name),
					                  static_cast<uint64_t>(file.size.to_int64()), 0, file_index,
					                  df::copy_grade::same_file);
				}

				const auto created = file.created();

				if (created.is_valid())
				{
					dups.emplace_back(duplicate_evidence_kind::name_created, str::to_lower(file.name),
					                  static_cast<uint64_t>(created.to_int64()), 0, file_index,
					                  df::copy_grade::same_file);
				}
			}
		}
		else
		{
			for (const auto& file : ifn.second->files)
			{
				_items.update_file(df::file_path(ifn.first, file.name), [](const df::index_folder_item_ptr&,
				                                                           const df::index_file_item& current_file)
				{
					if (current_file.exact_duplicate_group.load() == 0) return false;
					current_file.exact_duplicate_group = 0;
					return true;
				});
			}
		}
	}

	std::ranges::sort(dups, [](const duplicate_evidence& left, const duplicate_evidence& right)
	{
		return left.key() < right.key();
	});

	std::vector<size_t> parents(files.size());
	std::vector<uint8_t> ranks(files.size());
	std::iota(parents.begin(), parents.end(), 0);

	const auto find_root = [&parents](size_t item)
	{
		while (parents[item] != item)
		{
			parents[item] = parents[parents[item]];
			item = parents[item];
		}
		return item;
	};

	const auto unite = [&parents, &ranks, &find_root](const size_t left, const size_t right)
	{
		auto left_root = find_root(left);
		auto right_root = find_root(right);
		if (left_root == right_root) return;

		if (ranks[left_root] < ranks[right_root]) std::swap(left_root, right_root);
		parents[right_root] = left_root;
		if (ranks[left_root] == ranks[right_root]) ++ranks[left_root];
	};

	const auto& cdups = dups;

	int max_compare_count = 0;
	const auto dup_size = cdups.size();

	// How each file joined its set. Only the strongest evidence for a file is kept, so a file that is
	// byte-identical to one member and merely a possible copy of another reports the stronger claim.
	std::vector<df::copy_grade> grades(files.size(), df::copy_grade::none);

	const auto record_grade = [&grades](const size_t item, const df::copy_grade grade)
	{
		grades[item] = df::strongest(grades[item], grade);
	};

	for (auto i = 0u; i < dup_size; ++i)
	{
		if (df::is_closing) return;

		auto hi = i + 1;
		while (hi < dup_size && cdups[hi].key() == cdups[i].key()) ++hi;

		const auto bucket_size = static_cast<int>(hi - i);
		max_compare_count = std::max(max_compare_count, bucket_size);

		if (bucket_size > 1)
		{
			const auto first = cdups[i].index;

			for (auto member = i; member < hi; ++member)
			{
				if (df::is_closing) return;
				unite(first, cdups[member].index);
				record_grade(cdups[member].index, cdups[member].grade);
			}
		}

		i = hi - 1;
	}

	df::hash_map<size_t, df::int_counter> exact_component_counts;
	for (auto i = 0u; i < files.size(); ++i)
	{
		++exact_component_counts[find_root(i)];
	}

	df::hash_map<size_t, uint32_t> exact_component_groups;
	for (const auto& [root, count] : exact_component_counts)
	{
		if (count > 1) exact_component_groups[root] = ++next_dup_group;
	}

	for (auto i = 0u; i < files.size(); ++i)
	{
		const auto root = find_root(i);
		const auto found = exact_component_groups.find(root);
		const auto group = found == exact_component_groups.end() ? 0u : found->second;
		_items.update_file(df::file_path(files[i].path, files[i].file->name),
		                    [group](const df::index_folder_item_ptr&, const df::index_file_item& file)
		{
			if (file.exact_duplicate_group.load() == group) return false;
			file.exact_duplicate_group = group;
			return true;
		});
	}

	std::vector<uint8_t> crowded(files.size(), 0);

	// The perceptual stage. It is deliberately not part of the walk above: a picture match is a
	// tolerance, not an equality, so it may not be closed over transitively. Every candidate is
	// compared against one anchor chosen for the capture time and never against another candidate,
	// which makes each set a star rather than a chain (docs/collections.md section 7.2).
	std::vector<phash_request> phash_wanted;

	{
		df::hash_map<uint64_t, std::vector<size_t>> capture_times;
		uint32_t dated_pictures = 0;

		for (size_t i = 0; i < files.size(); ++i)
		{
			const auto* const file = files[i].file;
			if (!file->ft->has_trait(file_traits::bitmap)) continue;

			const auto md = file->metadata.load(); // important to hold ref
			if (!md) continue;

			const auto created = md->created();
			if (!created.is_valid()) continue;

			capture_times[created.to_int64()].push_back(i);
			++dated_pictures;
		}

		// A hash is only earned by a picture that shares a capture time with another, so marking the
		// candidates is what makes an excess measurable rather than merely suspected.
		std::vector<uint8_t> is_candidate(files.size(), 0);
		uint32_t candidate_count = 0;
		uint32_t wanted_count = 0;
		uint32_t matched_count = 0;
		uint32_t crowded_count = 0;

		// The shape narrowing the gate applies, and what a gate blind to rotation would have refused.
		uint32_t dims_unknown = 0;
		uint32_t aspect_solo = 0;
		uint32_t aspect_solo_swap = 0;
		uint32_t matched_cross_aspect = 0;

		const auto dims_of = [&files](const size_t i)
		{
			const auto md = files[i].file->metadata.load(); // important to hold ref
			return md ? md->dimensions() : sizei{};
		};

		for (const auto& [created, members] : capture_times)
		{
			if (df::is_closing) return;

			// One photograph proves nothing, and past this a capture time is unambiguously continuous
			// shooting. Presence applies the same bound, so neither surface can see a set the other
			// cannot (docs/collections.md section 7).
			if (members.size() < 2) continue;
			if (members.size() > max_photos_sharing_capture_time)
			{
				for (const auto member : members) crowded[member] = 1;
				continue;
			}

			// Sharing a capture second is a weak claim on its own. A picture whose neighbours are all
			// a different shape cannot be a copy of any of them, and refusing it here is a decode
			// saved rather than a judgement made. A quarter turn transposes the stored extent, so a
			// transposed neighbour still counts (docs/collections.md section 7.2).
			std::vector<size_t> shaped;
			shaped.reserve(members.size());

			for (const auto member : members)
			{
				const auto member_dims = dims_of(member);

				if (member_dims.is_empty())
				{
					++dims_unknown;
					// Shape is unknown rather than different, so the picture keeps its place.
					shaped.push_back(member);
					continue;
				}

				auto has_peer = false;
				auto has_peer_with_swap = false;

				for (const auto other : members)
				{
					if (other == member) continue;

					const auto other_dims = dims_of(other);

					if (same_picture_shape(member_dims, other_dims, false)) has_peer = true;
					if (same_picture_shape(member_dims, other_dims, true)) has_peer_with_swap = true;
					if (has_peer && has_peer_with_swap) break;
				}

				if (!has_peer) ++aspect_solo;
				if (!has_peer_with_swap) ++aspect_solo_swap;
				if (has_peer_with_swap) shaped.push_back(member);
			}

			if (shaped.size() < 2) continue;

			candidate_count += static_cast<uint32_t>(shaped.size());

			for (const auto member : shaped) is_candidate[member] = 1;

			// Every picture here has to be compared before any of them is reported: the crowd rule
			// counts matches, so judging a half-hashed capture time could let a burst through it.
			auto evidence_complete = true;

			for (const auto member : shaped)
			{
				if (files[member].file->phash.load() != nullptr) continue;

				evidence_complete = false;
				++wanted_count;

				// Hashing reads the file, which this walk of the index must not wait on, so the work is
				// only noted here. The pass that follows the hashes will see them and compare.
				if (phash_wanted.size() < max_phash_requests_per_pass)
				{
					phash_wanted.emplace_back(df::file_path(files[member].path, files[member].file->name),
					                          revision_of(*files[member].file));
				}
			}

			if (!evidence_complete) continue;

			// Lowest path, so which item anchors the set never depends on the order the index
			// happened to be walked in. A picture that declined to be identified cannot anchor, and
			// skipping it here stops one blank frame from suppressing the whole capture time.
			// Sentinel is files.size(): shaped holds indices into files, so files.size() is not a
			// value a real member can take.
			auto anchor = files.size();

			// The set the anchor was chosen for. This walk holds no index lock, and a rescan of an
			// edited picture clears its hash at any moment, so loading the record again here could
			// answer null for the anchor this loop has just seen hashed.
			df::picture_hashes_ptr anchor_hashes;

			for (const auto member : shaped)
			{
				auto held = files[member].file->phash.load();
				if (!held || !held->is_usable()) continue;

				if (anchor == files.size() ||
					df::file_path(files[member].path, files[member].file->name) <
					df::file_path(files[anchor].path, files[anchor].file->name))
				{
					anchor = member;
					anchor_hashes = std::move(held);
				}
			}

			if (anchor == files.size()) continue;

			if (test_after_duplicate_anchor) test_after_duplicate_anchor(*files[anchor].file);

			const auto anchor_hash = anchor_hashes->stored();

			// Collected rather than applied, because how many match decides whether any of them are
			// reported: a crowd around one anchor is a burst.
			std::vector<size_t> matched;

			// Continuous shooting produces frames in one orientation; it never produces a turned one.
			// So only an untuned match is evidence of a burst, and the crowd rule counts those alone.
			size_t same_orientation_matches = 0;

			for (const auto member : shaped)
			{
				if (member == anchor) continue;

				const auto member_hashes = files[member].file->phash.load();

				if (!member_hashes || !member_hashes->is_usable()) continue;

				// The member's four turns are the complete orbit, so this covers every relative
				// rotation without the anchor needing its own.
				if (crypto::phash_distance(anchor_hash, member_hashes->rotations) > max_duplicate_phash_distance)
				{
					continue;
				}

				matched.push_back(member);

				if (crypto::phash_distance(anchor_hash, member_hashes->stored()) <= max_duplicate_phash_distance)
				{
					++same_orientation_matches;
				}
			}

			if (same_orientation_matches > max_similar_pictures_at_one_capture_time)
			{
				for (const auto member : shaped) crowded[member] = 1;
				++crowded_count;
				continue;
			}

			const auto anchor_dims = dims_of(anchor);

			for (const auto member : matched)
			{
				unite(anchor, member);
				record_grade(anchor, df::copy_grade::same_picture);
				record_grade(member, df::copy_grade::same_picture);

				if (!same_picture_shape(anchor_dims, dims_of(member), true)) ++matched_cross_aspect;
			}

			matched_count += static_cast<uint32_t>(matched.size());
		}

		// Held against invited. A picture keeps its hash once computed, so a large uninvited count is
		// not drift - it is hashing that was asked for by something other than the candidate rule.
		uint32_t usable_held = 0;
		uint32_t declined_held = 0;
		uint32_t uninvited = 0;

		for (size_t i = 0; i < files.size(); ++i)
		{
			const auto held = files[i].file->phash.load();
			if (!held) continue;

			if (held->is_usable()) ++usable_held;
			else ++declined_held;

			if (!is_candidate[i]) ++uninvited;
		}

		df::set_gauge(df::index_perf.pass_pictures, dated_pictures);
		df::set_gauge(df::index_perf.pass_buckets, static_cast<uint32_t>(capture_times.size()));
		df::set_gauge(df::index_perf.pass_candidates, candidate_count);
		df::set_gauge(df::index_perf.pass_wanted, wanted_count);
		df::set_gauge(df::index_perf.pass_matched, matched_count);
		df::set_gauge(df::index_perf.pass_crowded, crowded_count);
		df::set_gauge(df::index_perf.pass_usable_held, usable_held);
		df::set_gauge(df::index_perf.pass_declined_held, declined_held);
		df::set_gauge(df::index_perf.pass_uninvited, uninvited);
		df::set_gauge(df::index_perf.pass_dims_unknown, dims_unknown);
		df::set_gauge(df::index_perf.pass_aspect_solo, aspect_solo);
		df::set_gauge(df::index_perf.pass_aspect_solo_swap, aspect_solo_swap);
		df::set_gauge(df::index_perf.pass_matched_cross_aspect, matched_cross_aspect);

		stats.indexed_phash_count = static_cast<int>(usable_held);
		stats.indexed_phash_declined_count = static_cast<int>(declined_held);
		stats.indexed_phash_uninvited_count = static_cast<int>(uninvited);
	}

	df::hash_map<size_t, df::int_counter> component_counts;
	for (auto i = 0u; i < files.size(); ++i)
	{
		++component_counts[find_root(i)];
	}

	df::hash_map<size_t, uint32_t> component_groups;
	for (const auto& [root, count] : component_counts)
	{
		if (count > 1) component_groups[root] = ++next_dup_group;
	}

	for (auto i = 0u; i < files.size(); ++i)
	{
		if (df::is_closing) return;
		const auto root = find_root(i);
		const auto count = static_cast<uint32_t>(component_counts[root]);
		const auto found_group = component_groups.find(root);
		const auto group = found_group == component_groups.end() ? 0u : found_group->second;
		const auto grade = group == 0 ? df::copy_grade::none : grades[i];
		const auto dup_info = df::duplicate_info{group, count, crowded[i], grade};
		_items.update_file(df::file_path(files[i].path, files[i].file->name),
		                    [dup_info](const df::index_folder_item_ptr& folder, const df::index_file_item& file)
		{
			const auto existing = file.duplicates.load();
			if (existing.count == dup_info.count && existing.group == dup_info.group &&
				existing.grade == dup_info.grade && existing.same_picture_crowded == dup_info.same_picture_crowded)
			{
				return false;
			}
			file.update_duplicates(folder, dup_info);
			return true;
		});
	}

	stats.indexed_dup_folder_count = static_cast<int>(component_groups.size());
	stats.indexed_crc_count = indexed_crc_count;
	stats.indexed_max_compare_count = max_compare_count;
	stats.predictions_ms = static_cast<int>(df::now_ms() - start_ms);

	df::set_gauge(df::index_perf.pass_files, static_cast<uint32_t>(files.size()));
	df::set_gauge(df::index_perf.pass_crc_held, static_cast<uint32_t>(indexed_crc_count));
	df::set_gauge(df::index_perf.pass_dup_groups, static_cast<uint32_t>(component_groups.size()));

	df::trace(std::format("Index update predictions: {} folders in {} ms", folder_count, stats.predictions_ms));

	if (!phash_wanted.empty() && !df::is_closing)
	{
		queue_calc_perceptual_hashes(std::move(phash_wanted));
	}
}

// Decoding cannot happen on the predictions walk, so the pairs it could not judge are hashed here
// and the pass is asked for again. Each pass narrows the work, so a large collection converges over
// several rounds instead of stalling on the first.
void index_state::queue_calc_perceptual_hashes(std::vector<phash_request> requests)
{
	_async.queue_async(async_queue::crc, [this, requests = std::move(requests)]
	{
		// Results are published in groups. Hashing a file takes milliseconds, so publishing each one on
		// its own found both the work queue and the write queue empty every time and woke two threads
		// per file for a few microseconds of work each.
		constexpr size_t publish_group = 32;
		std::vector<phash_result> hashed;
		hashed.reserve(publish_group);
		auto published_any = false;

		for (const auto& request : requests)
		{
			if (df::is_closing) break;

			const auto& path = request.path;
			crypto::phash_rotations hash{};
			auto readable = false;

			{
				df::scope_locked_inc loading(df::loading_media);
				df::perf_timer timer(df::index_perf.phash_us, &df::index_perf.phash_max_us);
				df::bump(df::index_perf.phash_computed);
				file_read_stream stream;

				if (stream.open(path) && stream.size() <= max_phash_file_bytes)
				{
					files ff;
					df::blob owner;
					readable = true;
					df::bump(df::index_perf.phash_bytes, stream.size());
					hash = ff.calc_perceptual_hash_rotations(stream.view_all(owner));
				}
				else
				{
					df::bump(df::index_perf.phash_unreadable);
				}
			}

			// Every attempt is recorded, including a refusal and a file that could not be read or
			// decoded. Without that the next pass asks for the same file again, forever.
			if (crypto::phash_is_usable(hash[0]))
			{
				hashed.emplace_back(path, request.revision, hash);
				df::bump(df::index_perf.phash_usable);
			}
			else
			{
				hashed.emplace_back(path, request.revision, crypto::phash_rotations{crypto::phash_declined, 0, 0, 0});
				if (readable) df::bump(df::index_perf.phash_declined);
			}

			if (hashed.size() >= publish_group)
			{
				published_any = true;
				save_phashes(std::move(hashed), false);
				hashed.clear();
				hashed.reserve(publish_group);
			}
		}

		// Published even when shutdown cut the loop short, so attempts already made are not repeated.
		published_any = published_any || !hashed.empty();
		save_phashes(std::move(hashed), published_any);
	});
}

struct presence_match
{
	item_presence state = item_presence::unknown;
	df::duplicate_info duplicates = {};
};

struct presence_request
{
	std::weak_ptr<df::item_element> lifetime;
	df::file_path path;
	df::file_size size;
	df::date_t file_modified;
	df::date_t media_created;
	uint32_t crc32c = 0;
	bool is_folder = false;
	bool is_bitmap = false;
	sizei dimensions;
};

struct presence_result
{
	presence_request source;
	presence_match match;
};

static int presence_rank(const item_presence presence)
{
	switch (presence)
	{
	case item_presence::newer_in: return 3;
	case item_presence::similar_in: return 2;
	case item_presence::older_in: return 1;
	default: return 0;
	}
}

static bool prefer_duplicate_info(const df::duplicate_info candidate, const df::duplicate_info current)
{
	if (current.group == 0) return candidate.group != 0;
	if (candidate.group == 0) return false;
	if (candidate.group != current.group) return candidate.group < current.group;
	return candidate.count > current.count;
}

static df::copy_grade dup_match_grade(const df::index_file_item& file, const presence_request& other)
{
	if (file.crc32c != 0 && file.size == other.size && file.crc32c == other.crc32c)
	{
		return df::copy_grade::identical;
	}

	const auto name_match = icmp(file.name, other.path.name()) == 0;

	if (name_match && file.ft->has_trait(file_traits::av) &&
		file.size.to_int64() != 0 && other.size.to_int64() != 0 &&
		file.size == other.size)
	{
		return df::copy_grade::same_file;
	}

	const auto created = file.created();
	return name_match && created.is_valid() && created == other.media_created
		       ? df::copy_grade::same_file
		       : df::copy_grade::none;
}

// An outside file the cheap grades could not place, held until the walk is over because deciding it
// means decoding a picture and the walk is reading the index.
struct presence_similar_candidate
{
	size_t request_index = 0;
	df::file_path path;
	crypto::phash_rotations phash{};
	df::date_t file_modified;
	uint64_t size = 0;
	df::duplicate_info duplicates;
	sizei dimensions;
};


static void items_possible_hashes_contains(std::vector<presence_match>& matches,
                                           const std::vector<std::pair<unsigned, size_t>>& possible,
                                           const std::vector<presence_request>& requests,
                                           const df::index_file_item& indexed_file, const uint32_t hash)
{
	auto lb = std::lower_bound(possible.begin(), possible.end(), hash, [](auto&& l, auto&& r) { return l.first < r; });

	while (lb != possible.end() && lb->first == hash)
	{
		const auto request_index = lb->second;
		const auto& request = requests[request_index];
		const auto grade = dup_match_grade(indexed_file, request);

		if (grade != df::copy_grade::none)
		{
			auto candidate = item_presence::unknown;

			if (indexed_file.file_modified == request.file_modified ||
				(indexed_file.crc32c != 0 && indexed_file.crc32c == request.crc32c))
			{
				candidate = item_presence::similar_in;
			}
			else if (indexed_file.file_modified < request.file_modified)
			{
				candidate = item_presence::older_in;
			}
			else if (indexed_file.file_modified > request.file_modified)
			{
				candidate = item_presence::newer_in;
			}

			auto duplicates = indexed_file.duplicates.load();
			duplicates.grade = grade;
			auto& current = matches[request_index];
			if (presence_rank(candidate) > presence_rank(current.state) ||
				(candidate == current.state && prefer_duplicate_info(duplicates, current.duplicates)))
			{
				current.state = candidate;
				current.duplicates = duplicates;
			}
		}

		++lb;
	}
}

// Decides the outside photographs the cheap grades could not place. Reading and decoding happens
// here, after the index walk, and only for a capture time the collection does not crowd: the same
// refusal duplicate search makes, so both surfaces answer alike (docs/collections.md section 7.3).
static void resolve_similar_presence(index_state& index, const std::vector<presence_request>& requests,
                                     std::vector<presence_match>& matches,
                                     std::vector<presence_similar_candidate>& candidates)
{
	if (candidates.empty()) return;

	std::ranges::sort(candidates, [](auto&& left, auto&& right)
	{
		return left.request_index < right.request_index;
	});

	files decoder;

	// A member is only hashed by the predictions pass when another member shares its capture time, so
	// the picture an outside file is being compared against often has no hash yet. It is computed
	// here and saved, because answering "checking" forever would be an absence in all but name.
	const auto hash_of = [&decoder, &index](const df::file_path path, const index_file_revision revision,
	                                        const crypto::phash_rotations& known) -> crypto::phash_rotations
	{
		if (known[0] != 0) return known;

		crypto::phash_rotations hash{};

		{
			df::scope_locked_inc loading(df::loading_media);
			df::perf_timer timer(df::index_perf.phash_us, &df::index_perf.phash_max_us);
			df::bump(df::index_perf.phash_computed);
			df::bump(df::index_perf.phash_presence);
			file_read_stream stream;

			if (stream.open(path) && stream.size() <= max_phash_file_bytes)
			{
				df::blob owner;
				df::bump(df::index_perf.phash_bytes, stream.size());
				hash = decoder.calc_perceptual_hash_rotations(stream.view_all(owner));
				df::bump(crypto::phash_is_usable(hash[0])
					         ? df::index_perf.phash_usable
					         : df::index_perf.phash_declined);
			}
			else
			{
				df::bump(df::index_perf.phash_unreadable);
			}
		}

		if (!crypto::phash_is_usable(hash[0])) hash = {crypto::phash_declined, 0, 0, 0};

		index.save_phash(path, revision, hash);
		return hash;
	};

	for (auto i = candidates.begin(); i != candidates.end();)
	{
		if (df::is_closing) return;

		auto group_end = i;
		while (group_end != candidates.end() && group_end->request_index == i->request_index) ++group_end;

		const auto member_count = static_cast<size_t>(std::distance(i, group_end));
		const auto request_index = i->request_index;
		const auto& request = requests[request_index];

		// A cheaper grade already answered this file; a picture cannot make that claim stronger.
		const auto already_matched = matches[request_index].state != item_presence::unknown;

		if (!already_matched && member_count <= max_photos_sharing_capture_time)
		{
			const auto probe_hash = hash_of(request.path, {request.file_modified, request.size.to_int64()}, {});

			if (crypto::phash_is_usable(probe_hash[0]))
			{
				// The outside file is the anchor here, so the same crowd rule applies: many members
				// matching one picture in one orientation at one capture time is a burst, not a set
				// of copies. A turned match is never burst evidence, exactly as duplicate search
				// counts it (docs/collections.md section 7.3).
				std::vector<const presence_similar_candidate*> matched;
				size_t same_orientation_matches = 0;

				for (auto candidate = i; candidate != group_end; ++candidate)
				{
					if (candidate->duplicates.same_picture_crowded) continue;

					// Shape narrows before the picture is decoded, exactly as duplicate search does.
					if (!same_picture_shape(request.dimensions, candidate->dimensions)) continue;

					const auto candidate_hash = hash_of(candidate->path,
					                                    {candidate->file_modified, candidate->size},
					                                    candidate->phash);

					if (!crypto::phash_is_usable(candidate_hash[0])) continue;
					if (crypto::phash_distance(probe_hash[0], candidate_hash) > max_duplicate_phash_distance)
					{
						continue;
					}

					matched.push_back(&*candidate);

					if (crypto::phash_distance(probe_hash[0], candidate_hash[0]) <= max_duplicate_phash_distance)
					{
						++same_orientation_matches;
					}
				}

				if (same_orientation_matches <= max_similar_pictures_at_one_capture_time)
				{
					for (const auto* const candidate : matched)
					{
						auto state = item_presence::similar_in;

						if (candidate->file_modified < request.file_modified) state = item_presence::older_in;
						else if (candidate->file_modified > request.file_modified)
							state = item_presence::newer_in;

						auto duplicates = candidate->duplicates;
						duplicates.grade = df::copy_grade::same_picture;
						auto& current = matches[request_index];

						if (presence_rank(state) > presence_rank(current.state))
						{
							current.state = state;
							current.duplicates = duplicates;
						}
					}
				}
			}
		}

		i = group_end;
	}
}

void index_state::queue_update_presence(const df::item_set& items)
{
	df::assert_true(ui::is_ui_thread());
	if (items.empty()) return;

	std::vector<presence_request> requests;
	requests.reserve(items.size());
	for (const auto& item : items.items())
	{
		const auto ft = item->file_type();
		const auto md = item->metadata();
		requests.emplace_back(item, item->path(), item->file_size(), item->file_modified(),
		                      item->media_created(), item->crc32c(), item->is_folder(),
		                      ft && ft->has_trait(file_traits::bitmap),
		                      md ? md->dimensions() : sizei{});
	}

	_async.queue_async(async_queue::index_presence_single, [this, requests = std::move(requests)]() mutable
	{
		df::measure_ms ms(stats.update_presence_ms);
		df::index_folder_info_map indexed_folders;
		std::vector<presence_match> matches(requests.size());

		for (const auto& request : requests)
		{
			if (!request.is_folder)
			{
				const auto folder = _items.find(request.path.folder());
				if (folder && folder->is_in_collection)
				{
					indexed_folders[request.path.folder()] = folder;
				}
			}
		}

		std::vector<std::pair<uint32_t, size_t>> items_possible_hashes;
		// Outside photographs whose capture time a member might share, keyed exactly rather than by the
		// folded hash above, because a picture comparison is too expensive to run on a fold collision.
		df::hash_map<uint64_t, std::vector<size_t>> requests_by_capture_time;

		for (size_t index = 0; index < requests.size(); ++index)
		{
			const auto& request = requests[index];
			if (request.is_folder) continue;

			const auto is_indexed_folder = indexed_folders.contains(request.path.folder());

			if (is_indexed_folder)
			{
				auto& match = matches[index];
				match.state = item_presence::this_in;

				const auto& folder = indexed_folders.at(request.path.folder());
				const auto file = find_file(folder->files, request.path.name());
				if (file != folder->files.end()) match.duplicates = file->duplicates.load();
			}
			else
			{
				if (request.crc32c)
				{
					items_possible_hashes.emplace_back(request.crc32c, index);
				}

				if (request.media_created.is_valid())
				{
					items_possible_hashes.emplace_back(x64to32(request.media_created.to_int64()), index);

					if (request.is_bitmap)
					{
						requests_by_capture_time[request.media_created.to_int64()].push_back(index);
					}
				}

				items_possible_hashes.emplace_back(request.path.name().ihash(), index);
			}
		}

		std::vector<presence_similar_candidate> similar_candidates;

		if (!items_possible_hashes.empty())
		{
			std::ranges::sort(items_possible_hashes, [](auto&& left, auto&& right)
			{
				return left.first < right.first;
			});
			const auto folders = _items.all_folders();

			for (const auto& ifn : folders)
			{
				if (ifn.second->is_in_collection)
				{
					for (const auto& file : ifn.second->files)
					{
						if (file.crc32c)
						{
							items_possible_hashes_contains(matches, items_possible_hashes, requests, file,
							                               file.crc32c);
						}

						const auto created_date = file.created();

						if (created_date.is_valid())
						{
							items_possible_hashes_contains(matches, items_possible_hashes, requests, file,
							                               x64to32(created_date.to_int64()));

							const auto shares_time = requests_by_capture_time.find(created_date.to_int64());

							if (shares_time != requests_by_capture_time.end() &&
								file.ft->has_trait(file_traits::bitmap))
							{
								const auto held = file.phash.load();
								const auto rotations = held ? held->rotations : crypto::phash_rotations{};
								const auto file_md = file.metadata.load(); // important to hold ref
								const auto file_dims = file_md ? file_md->dimensions() : sizei{};

								for (const auto request_index : shares_time->second)
								{
									similar_candidates.emplace_back(request_index,
									                                df::file_path(ifn.first, file.name),
									                                rotations,
									                                file.file_modified.load(),
									                                file.size.to_int64(),
									                                file.duplicates.load(),
									                                file_dims);
								}
							}
						}

						items_possible_hashes_contains(matches, items_possible_hashes, requests, file,
						                               file.name.ihash());
					}
				}
			}
		}

		resolve_similar_presence(*this, requests, matches, similar_candidates);

		for (size_t index = 0; index < matches.size(); ++index)
		{
			auto& match = matches[index];
			const auto& request = requests[index];
			if (request.is_folder)
			{
				match = {};
				continue;
			}

			if (match.state == item_presence::unknown)
			{
				auto evidence_complete = false;
				if (_fully_loaded)
				{
					const auto folder = _items.find(request.path.folder());
					if (folder)
					{
						const auto file = find_file(folder->files, request.path.name());
						evidence_complete = file != folder->files.end() &&
							!needs_scan_impl(folder, *file, false, false);
					}
				}
				match.state = evidence_complete ? item_presence::not_in : item_presence::unknown;
				match.duplicates = df::duplicate_info{};
			}
		}

		const auto result_count = requests.size();
		std::vector<presence_result> results;
		results.reserve(result_count);
		for (size_t index = 0; index < result_count; ++index)
		{
			results.emplace_back(std::move(requests[index]), matches[index]);
		}

		_async.queue_ui([this, results = std::move(results)]
		{
			auto changed = false;
			for (const auto& result : results)
			{
				const auto item = result.source.lifetime.lock();
				if (!item || item->path() != result.source.path || item->file_size() != result.source.size ||
					item->file_modified() != result.source.file_modified ||
					item->media_created() != result.source.media_created || item->crc32c() != result.source.crc32c)
				{
					continue;
				}

				if (item->presence() == result.match.state && item->duplicates() == result.match.duplicates)
				{
					continue;
				}

				item->presence(result.match.state);
				item->duplicates(result.match.duplicates);
				changed = true;
			}

			if (changed)
			{
				_async.invalidate_view(view_invalid::view_layout | view_invalid::group_layout);
			}
		});

		df::trace(std::format("Index update presence {} items in {} ms", result_count, stats.update_presence_ms));
	});
}
