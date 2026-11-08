// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: File indexing engine and duplicate detection. Scans folders, maintains item index,
// calculates summaries, and identifies duplicate files by name, date, and CRC. search_counter counts
// many searches - sidebar rows, address-box predictions - against one listing of the index.

#pragma once

#include "model_items.h"
#include "model_postings.h"
#include "model_search_index.h"

struct search_part;
class location_cache;
class database;
class async_strategy;

// The content identity a hash was computed against. A checksum or picture hash describes the bytes
// that were read, so publishing one against a path alone attaches it to whatever occupies that path
// when the work finishes. A file replaced while it was being hashed is a different file at the same
// name, and giving it the old hash reports it as a copy of what the previous content matched.
struct index_file_revision
{
	core::date_t modified;
	uint64_t size = 0;

	bool operator==(const index_file_revision& other) const
	{
		return modified == other.modified && size == other.size;
	}
};

inline index_file_revision revision_of(const df::index_file_item& file)
{
	return {file.file_modified.load(), file.size.to_int64()};
}

// One file the predictions walk wants a picture hash for, with the revision it was wanted at.
struct phash_request
{
	core::file_path path;
	index_file_revision revision;
};

struct phash_result
{
	core::file_path path;
	index_file_revision revision;
	crypto::phash_rotations rotations{};
};

struct item_db_write
{
	core::file_path path;
	std::optional<prop::item_metadata_ptr> md;
	std::optional<core::date_t> modified;
	std::optional<double> media_position;
	std::optional<core::const_image_ptr> thumb;
	std::optional<core::const_image_ptr> cover_art;
	std::optional<core::date_t> thumb_scanned;
	std::optional<core::date_t> metadata_scanned;
	std::optional<uint32_t> crc32c;
	std::optional<crypto::phash_rotations> phash;
	// The bytes changed, so the stored checksum and picture hash describe content that is gone. The
	// optional fields above cannot say this: a write that carries no hash keeps the stored one.
	bool clear_hashes = false;

	item_db_write() noexcept = default;
	item_db_write(const item_db_write&) = delete;
	item_db_write& operator=(const item_db_write&) = delete;
	item_db_write(item_db_write&&) noexcept = default;
	item_db_write& operator=(item_db_write&&) noexcept = default;
};

struct db_item_t
{
	core::cached path = {};
	core::date_t metadata_scanned = {};
	prop::item_metadata_ptr metadata = {};
	uint32_t crc32c = 0;
	crypto::phash_rotations phash{};

	db_item_t() noexcept = default;
	db_item_t(const db_item_t&) = delete;
	db_item_t& operator=(const db_item_t&) = delete;
	db_item_t(db_item_t&&) noexcept = default;
	db_item_t& operator=(db_item_t&&) noexcept = default;
};

struct key_val
{
	core::cached key;
	core::cached val;

	key_val() noexcept = default;

	key_val(const core::cached k, const core::cached v) noexcept : key(k), val(v)
	{
	}

	bool operator<(const key_val& other) const
	{
		return compare(other) < 0;
	}

	bool operator==(const key_val& other) const
	{
		return compare(other) == 0;
	}

	bool operator!=(const key_val& other) const
	{
		return compare(other) != 0;
	}

	int compare(const key_val& other) const
	{
		const auto f = icmp(key, other.key);
		return f == 0 ? icmp(val, other.val) : f;
	}
};

struct phash
{
	size_t operator()(const key_val& r) const
	{
		crypto::hash_gen h;
		h.append(r.key);
		h.append(r.val);
		return h.result();
	}
};

struct peq
{
	bool operator()(const key_val& l, const key_val& r) const
	{
		return l.compare(r) == 0;
	}
};

struct index_statistic
{
	int index_folder_count = 0;
	int index_item_remaining = 0;
	int items_saved = 0;
	int thumbs_saved = 0;

	int media_item_count = 0;
	int picture_item_count = 0;
	int index_item_count = 0;
	int indexed_dup_folder_count = 0;
	int indexed_max_compare_count = 0;
	int indexed_crc_count = 0;
	// Perceptual hashes the collection holds. Uninvited counts the ones whose picture shares its
	// capture time with nothing, so it should stay near zero.
	int indexed_phash_count = 0;
	int indexed_phash_declined_count = 0;
	int indexed_phash_uninvited_count = 0;

	int index_load_ms = 0;
	int predictions_ms = 0;
	int count_matches_ms = 0;

	// The search index as last published: what it holds and what reading it cost.
	int search_index_docs = 0;
	int search_index_build_ms = 0;
	size_t search_index_bytes = 0;

	int scan_items_ms = 0;
	int update_presence_ms = 0;

	core::file_size database_size;
	core::file_path database_path;
};

struct index_progress_snapshot
{
	int index_item_count = 0;
	int index_item_remaining = 0;
};

using unique_key_vals = core::hash_map<key_val, core::int_counter, phash, peq>;
using db_items_t = std::vector<db_item_t>;
using items_by_folder_t = core::dense_hash_map<core::folder_path, df::index_folder_item_ptr, core::ihash, core::ieq>;

using item_writes_t = platform::queue<item_db_write>;
using index_folders_t = std::vector<std::pair<core::folder_path, df::index_folder_item_ptr>>;

struct index_folder_snapshot
{
	df::index_folder_item_ptr folder;
	uint64_t content_revision = 0;
};

class index_items
{
	mutable core::mutex _rw;
	_Guarded_by_(_rw) items_by_folder_t _index;

	struct child_replacement
	{
		df::index_folder_item_ptr parent;
		std::shared_ptr<const df::index_folder_infos> expected;
		std::shared_ptr<const df::index_folder_infos> replacement;

		explicit operator bool() const
		{
			return parent && expected && replacement;
		}
	};

public:
	// What an in-place update changes. Either kind stops a folder rebuilt from an older copy replacing the
	// node; only a searchable change stales the search index's read of the folder, which reads what is
	// derived from an item's bytes - its duplicates, checksum and picture hashes - live.
	enum class file_change
	{
		searchable,
		derived
	};

	index_folder_snapshot find_snapshot(const core::folder_path folder) const
	{
		core::shared_lock lock(_rw);
		const auto found_in_index = _index.find(folder);
		if (found_in_index == _index.end()) return {};

		const auto current = found_in_index->second;
		return {current, current ? current->revision_snapshot() : 0};
	}

	df::index_folder_item_ptr find(const core::folder_path folder) const
	{
		return find_snapshot(folder).folder;
	}

	void replace(const folder_path folder_path, const df::index_folder_item_ptr& i)
	{
		for (;;)
		{
			df::index_folder_item_ptr old_node;
			child_replacement child;
			prepare_child_replacement(folder_path, i, child);
			auto retry = false;

			{
				core::exclusive_lock lock(_rw);
				if (child && !is_child_replacement_current(folder_path, child))
				{
					retry = true;
				}
				else
				{
					retry = !replace_locked(folder_path, i, child, old_node);
				}
			}

			if (retry) continue;
			old_node.reset();
			return;
		}
	}

	// Publishes only while the folder still holds the node the caller built from, and answers what
	// the index holds afterwards. A rebuild reads the node, enumerates the file system without a
	// lock, then publishes - and another thread that rebuilt or scanned the same folder inside that
	// window left newer state in the index. Storing unconditionally puts the pre-enumeration copy
	// back over it, losing every scan result recorded since.
	df::index_folder_item_ptr replace_if(const folder_path folder_path,
	                                     const df::index_folder_item_ptr& expected,
	                                     const uint64_t expected_content_revision,
	                                     const df::index_folder_item_ptr& i)
	{
		for (;;)
		{
			df::index_folder_item_ptr old_node;
			child_replacement child;
			prepare_child_replacement(folder_path, i, child);
			auto retry = false;
			df::index_folder_item_ptr result;

			{
				core::exclusive_lock lock(_rw);

				const auto found_in_index = _index.find(folder_path);
				const auto current = found_in_index != _index.end() ? found_in_index->second : nullptr;

				if (current != expected) return current;
				if (current && current->revision_snapshot() != expected_content_revision)
				{
					return current;
				}
				if (child && !is_child_replacement_current(folder_path, child))
				{
					retry = true;
				}
				else
				{
					retry = !replace_locked(folder_path, i, child, old_node);
					if (!retry) result = i;
				}
			}

			if (retry) continue;
			old_node.reset();
			return result;
		}
	}

	template <typename Func>
	bool update_file(const core::file_path path, Func func, const file_change change = file_change::searchable) const
	{
		core::shared_lock lock(_rw);
		const auto found_folder = _index.find(path.folder());
		if (found_folder == _index.end()) return false;

		const auto folder = found_folder->second;
		if (!folder) return false;

		const auto found_file = std::lower_bound(folder->files.begin(), folder->files.end(), path.name());
		if (found_file == folder->files.end()) return false;
		if (icmp(found_file->name, path.name()) != 0) return false;
		if (!func(folder, *found_file)) return false;
		folder->mark_content_changed(change == file_change::searchable);
		return true;
	}

	template <typename Func>
	bool update_folder(const folder_path folder_path, Func func) const
	{
		core::exclusive_lock lock(_rw);
		const auto found_folder = _index.find(folder_path);
		if (found_folder == _index.end() || !found_folder->second) return false;
		if (!func(found_folder->second)) return false;
		found_folder->second->mark_content_changed();
		return true;
	}

	// Erases each folder and everything indexed beneath it. The index is flat and keyed by folder
	// path, so erasing only the named key left every descendant indexed - still searchable, still
	// counted - after the folder that held them was deleted.
	void erase(const std::vector<core::folder_path>& folders)
	{
		if (folders.empty()) return;

		core::exclusive_lock lock(_rw);

		std::erase_if(_index, [&folders](const auto& entry)
		{
			return std::ranges::any_of(folders, [&entry](const core::folder_path removed)
			{
				return core::folder_contains(removed.text().sv(), entry.first.text().sv());
			});
		});
	}

	index_folders_t all_folders() const
	{
		for (;;)
		{
			size_t size = 0;
			{
				core::shared_lock lock(_rw);
				size = _index.size();
			}

			index_folders_t result;
			result.reserve(size);

			{
				core::shared_lock lock(_rw);
				if (result.capacity() < _index.size()) continue;
				result.insert(result.end(), _index.begin(), _index.end());
			}

			return result;
		}
	}

	df::index_folder_item_ptr find_or_create(const folder_path folder_path,
	                                         df::index_folder_item_ptr candidate)
	{
		{
			core::shared_lock lock(_rw);
			const auto found_in_index = _index.find(folder_path);
			if (found_in_index != _index.end())
			{
				return found_in_index->second;
			}
		}

		{
			core::exclusive_lock lock(_rw);
			const auto [found, inserted] = _index.try_emplace(folder_path, std::move(candidate));
			return found->second;
		}
	}

	void clear()
	{
		core::exclusive_lock lock(_rw);
		_index.clear();
	}

private:
	_Requires_lock_held_(_rw) bool replace_locked(const folder_path folder_path,
	                                              const df::index_folder_item_ptr& i,
	                                              child_replacement& child,
	                                              df::index_folder_item_ptr& old_node)
	{
		if (child)
		{
			auto expected = child.expected;
			if (!child.parent->child_folders.compare_exchange_strong(expected, child.replacement)) return false;
			child.parent->mark_content_changed();
		}

		const auto [found, inserted] = _index.try_emplace(folder_path, i);
		if (!inserted)
		{
			old_node.swap(found->second);
			found->second = i;
		}

		return true;
	}

	void prepare_child_replacement(const folder_path folder_path,
	                               const df::index_folder_item_ptr& i,
	                               child_replacement& child) const
	{
		child = {};
		if (folder_path.is_root()) return;

		df::index_folder_item_ptr parent;
		{
			core::shared_lock lock(_rw);
			const auto found_parent = _index.find(folder_path.parent());
			if (found_parent == _index.end()) return;
			parent = found_parent->second;
		}
		if (!parent) return;

		auto existing = parent->folders_snapshot();
		auto updated = std::make_shared<df::index_folder_infos>(*existing);
		const auto found = std::lower_bound(updated->begin(), updated->end(), i->name,
		                                    [](const df::index_folder_item_ptr& left, const std::string_view right)
		                                    {
			                                    return icmp(left->name, right) < 0;
		                                    });

		if (found == updated->end() || icmp((*found)->name, i->name) != 0) return;
		*found = i;
		child = {parent, std::move(existing), std::move(updated)};
	}

	_Requires_lock_held_(_rw) bool is_child_replacement_current(const folder_path folder_path,
	                                                            const child_replacement& child) const
	{
		const auto found_parent = _index.find(folder_path.parent());
		return found_parent != _index.end() &&
			found_parent->second == child.parent &&
			child.parent->folders_snapshot() == child.expected;
	}

};

// Hamming distance across 63 meaningful bits. Every hash sets exactly 31 of them, so a distance is
// always even and only 0, 2, 4 and 6 are distinct settings; 6 is the loosest of the four.
constexpr auto max_duplicate_phash_distance = 6;

// Strongest evidence linking two indexed files, or none. This is the exact half of the copy
// relation: every rule here is a conjunction of equalities, so it is transitive and safe to close
// over (docs/collections.md section 7.2). The perceptual half is not, and is not decided here.
__forceinline bool same_picture_shape(const sizei a, const sizei b, const bool allow_swap = true)
{
	if (a.is_empty() || b.is_empty()) return true;

	const auto close = [](const sizei left, const sizei right)
	{
		constexpr double mcu_block = 16.0;
		const auto left_aspect = static_cast<double>(left.cx) / left.cy;
		const auto right_aspect = static_cast<double>(right.cx) / right.cy;
		const auto shortest = std::min({left.cx, left.cy, right.cx, right.cy});
		const auto tolerance = mcu_block / shortest + 0.01;
		return std::abs(left_aspect - right_aspect) <= std::max(left_aspect, right_aspect) * tolerance;
	};

	if (close(a, b)) return true;

	return allow_swap && close(a, {b.cy, b.cx});
}

__forceinline df::copy_grade dup_match_grade(const df::index_file_item* file,
                                             const df::index_file_item* other_file)
{
	if (file->crc32c != 0 && file->size == other_file->size && file->crc32c == other_file->crc32c)
	{
		return df::copy_grade::identical;
	}

	const auto name_match = icmp(file->name, other_file->name) == 0;

	if (name_match && file->ft->has_trait(file_traits::av) && file->size == other_file->size)
	{
		return df::copy_grade::same_file;
	}

	return name_match && file->created() == other_file->created()
		       ? df::copy_grade::same_file
		       : df::copy_grade::none;
}

__forceinline bool is_dup_match(const df::index_file_item* file, const df::index_file_item* other_file)
{
	return dup_match_grade(file, other_file) != df::copy_grade::none;
}

__forceinline df::copy_grade dup_match_grade(const df::index_file_item& file, const df::item_element_ptr& other_file)
{
	if (file.crc32c != 0 && file.size == other_file->file_size() && file.crc32c == other_file->crc32c())
	{
		return df::copy_grade::identical;
	}

	const auto name_match = icmp(file.name, other_file->path().name()) == 0;

	if (name_match && file.ft->has_trait(file_traits::av) && file.size == other_file->file_size())
	{
		return df::copy_grade::same_file;
	}

	return name_match && file.created() == other_file->media_created()
		       ? df::copy_grade::same_file
		       : df::copy_grade::none;
}

__forceinline bool is_dup_match(const df::index_file_item& file, const df::item_element_ptr& other_file)
{
	return dup_match_grade(file, other_file) != df::copy_grade::none;
}

// Confidence rank of a duplicate match against the item a related search started at: 0 is the
// strongest evidence, negative means no match. The grade travels with the rank so a related search
// reports the same strength duplicate search does. is_dup_match is the same rules read as a boolean.
struct dup_match_result
{
	int rank = -1;
	df::copy_grade grade = df::copy_grade::none;

	bool matched() const
	{
		return grade != df::copy_grade::none;
	}
};

__forceinline dup_match_result dup_match(const df::related_info& file, const df::index_file_item& other_file)
{
	const auto other_dups = other_file.duplicates.load();

	// Exact duplicate sets are transitive, so common exact membership is pair evidence. Perceptual
	// sets are anchored rather than chained and may be mixed with exact subgroups.
	if (file.exact_group != 0 && file.exact_group == other_file.exact_duplicate_group.load())
	{
		return {0, other_dups.grade == df::copy_grade::none ? df::copy_grade::same_file : df::strongest(other_dups.grade, df::copy_grade::same_file)};
	}

	if (file.crc32c != 0 && file.size == other_file.size && file.crc32c == other_file.crc32c)
	{
		return {0, df::copy_grade::identical};
	}

	if (icmp(file.path.name(), other_file.name) == 0)
	{
		// A related search restored from text may not have resolved a file type, so the
		// audio/video rule is skipped rather than dereferencing an unknown type.
		if (file.ft && file.ft->has_trait(file_traits::av) &&
			file.size.to_int64() != 0 && other_file.size.to_int64() != 0 &&
			file.size == other_file.size)
		{
			return {1, df::copy_grade::same_file};
		}

		const auto created = file.created();
		if (created.is_valid() && created == other_file.created())
		{
			return {2, df::copy_grade::same_file};
		}
	}

	const auto other_hashes = other_file.phash.load();
	const auto other_metadata = other_file.metadata.load();
	if (file.ft && file.ft->has_trait(file_traits::bitmap) &&
		other_file.ft->has_trait(file_traits::bitmap) &&
		file.created() == other_file.created() &&
		crypto::phash_is_usable(file.phash[0]) &&
		other_hashes && other_hashes->is_usable() &&
		!file.duplicate_crowded &&
		!other_dups.same_picture_crowded &&
		same_picture_shape(file.dimensions, other_metadata ? other_metadata->dimensions() : sizei{}) &&
		crypto::phash_distance(file.phash[0], other_hashes->rotations) <= max_duplicate_phash_distance)
	{
		return {3, df::copy_grade::same_picture};
	}

	return {};
}

__forceinline int dup_match_rank(const df::related_info& file, const df::index_file_item& other_file)
{
	const auto result = dup_match(file, other_file);
	return result.matched() ? result.rank : -1;
}

__forceinline bool is_dup_match(const df::related_info& file, const df::index_file_item& other_file)
{
	return dup_match(file, other_file).matched();
}

struct location_group
{
	core::cached name = {};
	core::cached state = {};
	core::cached country = {};
	uint32_t count = 0;
	pointi loc = {};
	double latitude_sum = 0.0;
	double longitude_sum = 0.0;

	gps_coordinate centroid() const
	{
		return count == 0
			       ? gps_coordinate{}
			       : gps_coordinate(latitude_sum / count, longitude_sum / count);
	}

	bool operator==(const location_group& other) const
	{
		return name == other.name &&
			state == other.state &&
			country == other.country &&
			count == other.count &&
			loc == other.loc &&
			core::equiv(latitude_sum, other.latitude_sum) &&
			core::equiv(longitude_sum, other.longitude_sum);
	}

	bool operator !=(const location_group& other) const
	{
		return name != other.name ||
			state != other.state ||
			country != other.country ||
			count != other.count ||
			loc != other.loc ||
			!core::equiv(latitude_sum, other.latitude_sum) ||
			!core::equiv(longitude_sum, other.longitude_sum);
	}
};

struct index_histograms
{
	index_histograms() noexcept = default;
	~index_histograms() = default;
	index_histograms(const index_histograms&) = default;
	index_histograms& operator=(const index_histograms&) = default;
	index_histograms(index_histograms&&) noexcept = default;
	index_histograms& operator=(index_histograms&&) noexcept = default;

	df::file_group_histogram _file_types;
	df::date_histogram _dates;
	df::location_heat_map _locations;
	std::vector<double> _location_latitude_sums = std::vector<double>(
		df::location_heat_map::map_width * df::location_heat_map::map_height);
	std::vector<double> _location_longitude_sums = std::vector<double>(
		df::location_heat_map::map_width * df::location_heat_map::map_height);
	std::vector<double> _location_min_latitudes = std::vector<double>(
		df::location_heat_map::map_width * df::location_heat_map::map_height, std::numeric_limits<double>::max());
	std::vector<double> _location_min_longitudes = std::vector<double>(
		df::location_heat_map::map_width * df::location_heat_map::map_height, std::numeric_limits<double>::max());
	std::vector<double> _location_max_latitudes = std::vector<double>(
		df::location_heat_map::map_width * df::location_heat_map::map_height, std::numeric_limits<double>::lowest());
	std::vector<double> _location_max_longitudes = std::vector<double>(
		df::location_heat_map::map_width * df::location_heat_map::map_height, std::numeric_limits<double>::lowest());
	core::hash_map<uint32_t, location_group> _location_groups;

	// Sampled once when the histogram is built. A static would freeze the current year for the
	// lifetime of the process; re-reading the clock per file would cost a syscall per item.
	int _year = core::utc_now().year();

	void record(const location_cache& locations, const df::index_file_item& file, const core::file_path& path = {});
	std::vector<map_location_area> map_locations(int cell_span) const;
	std::optional<map_location_area> find_map_location(std::string_view name, const location_cache& locations,
	                                                   gps_coordinate default_location) const;
};

using strings_by_prop = core::hash_map<prop::key_ref, core::dense_unique_strings>;
using prop_text_summary = core::hash_map<std::string_view, df::file_group_histogram, core::ihash, core::ieq>;
using prop_num_summary = std::array<df::file_group_histogram, 6>;
using tag_companion_counts = core::hash_map<std::string, core::string_counts, core::ihash, core::ieq>;
using index_histograms_const_ptr = std::shared_ptr<const index_histograms>;

// Pure prefix lookup over a term list sorted case-insensitively (by core::icmp). Returns the
// contiguous [first, last) range of terms that begin with `query` (case-insensitive) in
// O(log N + k), driving fast typeahead prediction without scanning the whole vocabulary.
inline std::pair<std::vector<std::string_view>::const_iterator, std::vector<std::string_view>::const_iterator>
word_prefix_range(const std::vector<std::string_view>& sorted_terms, const std::string_view query)
{
	// Every term has the empty string as a prefix. (core::icmp also treats "" as greatest,
	// so the binary search below must not be used for an empty query.)
	if (query.empty()) return {sorted_terms.begin(), sorted_terms.end()};

	const auto lo = std::ranges::lower_bound(sorted_terms, query,
	                                         [](const std::string_view a, const std::string_view b)
	                                         {
		                                         return core::icmp(a, b) < 0;
	                                         });
	auto hi = lo;
	while (hi != sorted_terms.end() && core::starts(*hi, query)) ++hi;
	return {lo, hi};
}

struct index_metadata_summary
{
	strings_by_prop _distinct_text;
	core::dense_string_counts _distinct_words;
	std::vector<std::string_view> _sorted_words; // keys of _distinct_words sorted by core::icmp for prefix lookup
	df::trigram_index _word_trigrams; // vocabulary trigram index: term-id (index into _sorted_words) by trigram

	prop_text_summary _distinct_tags;
	tag_companion_counts _tag_companions;
	prop_text_summary _distinct_labels;
	prop_num_summary _distinct_ratings;
};

using index_metadata_summary_const_ptr = std::shared_ptr<const index_metadata_summary>;

struct index_summary
{
	index_summary() noexcept = default;
	~index_summary() = default;
	index_summary(const index_summary&) = delete;
	index_summary& operator=(const index_summary&) = delete;
	index_summary(index_summary&&) noexcept = delete;
	index_summary& operator=(index_summary&&) noexcept = delete;

	core::index_roots _roots;
	// Each collection folder whose owner set what it holds. The nearest one above a folder answers for it.
	std::vector<std::pair<core::folder_path, prop::library_kind>> _library_kinds;
	std::shared_ptr<const core::unique_folders> _distinct_folders = std::make_shared<core::unique_folders>();
	std::shared_ptr<const core::unique_folders> _distinct_other_folders = std::make_shared<core::unique_folders>();
	std::shared_ptr<const core::unique_folders> _distinct_prime_folders = std::make_shared<core::unique_folders>();
	index_metadata_summary_const_ptr _metadata = std::make_shared<index_metadata_summary>();
	index_histograms_const_ptr _histograms = std::make_shared<index_histograms>();
	// Null until the collection has been read once, and while indexing has left it too stale to help.
	std::shared_ptr<const df::search_index> _search_index;
};

enum class location_matrix_projection
{
	web_mercator,
	location_heat_map
};

struct location_matrix_params
{
	int zoom = 16;
	int cell_size = 44;
	double min_latitude = -85.05112878;
	double min_longitude = -180.0;
	double max_latitude = 85.05112878;
	double max_longitude = 180.0;
	location_matrix_projection projection = location_matrix_projection::web_mercator;
	int area_cell_span = 1;

	bool contains(gps_coordinate coordinate) const;
	pointi cell(gps_coordinate coordinate) const;
};

struct location_matrix
{
	struct cell
	{
		pointi index;
		core::file_path representative_path;
		gps_coordinate centroid;
		uint32_t count = 0;
		double latitude_sum = 0.0;
		double longitude_sum = 0.0;
		double min_latitude = 0.0;
		double min_longitude = 0.0;
		double max_latitude = 0.0;
		double max_longitude = 0.0;
	};

	location_matrix_params params;
	std::vector<cell> cells;

	location_matrix() = default;

	explicit location_matrix(const location_matrix_params& value) : params(value)
	{
	}

	void add(core::file_path path, gps_coordinate coordinate, bool can_thumbnail, int rating);
	void finalize();

private:
	// Keyed on the cell index packed into one word, so building a matrix costs no tree node per
	// cell. Cleared by finalize(); nothing outside add()/finalize() reads it.
	core::hash_map<uint64_t, uint32_t> _cell_lookup;
	std::vector<uint8_t> _representative_ranks;
};

struct folder_scan_item
{
	core::folder_path folder;
	df::index_file_item item;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// index_state - Central File Indexing Engine
////////////////////////////////////////////////////////////////////////////////////////////////////
//
// PURPOSE:
// The index_state class maintains an in-memory index of all files in the user's collection.
// It coordinates folder scanning, metadata extraction, duplicate detection, and provides
// fast search capabilities. All operations are designed to work asynchronously to keep the
// UI responsive.
//
// ARCHITECTURE:
// - Thread-safe index storage via index_items (_items) with reader-writer locking
// - Summary data (_summary) protected by _summary_rw mutex for aggregate statistics
// - Database write queue (_db_writes) for batched SQLite persistence
// - Integration with async_strategy for background thread coordination
//
// INDEXING PIPELINE:
// The indexing process is a multi-stage async pipeline that efficiently context-switches
// between threads to maximize throughput while keeping the UI responsive:
//
// Stage 1: index_roots() - Set collection root folders
//   - Called on UI thread to configure which folders to index
//   - Stores roots in _summary._roots under exclusive lock
//
// Stage 2: index_folders() - Discover folder structure
//   - Runs on index_task_queue background thread
//   - Recursively walks folder tree calling validate_folder() for each
//   - Marks folders as is_in_collection, tracks excludes
//   - Updates histograms progressively, triggers sidebar UI updates
//   - Sets _folders_indexed = true when complete
//
// Stage 3: scan_uncached() - Extract file metadata
//   - Runs on index_task_queue after folder discovery
//   - Iterates all indexed files, calls scan_item() for those needing metadata
//   - scan_item() performs I/O-heavy metadata extraction
//   - Queues item_db_write records to _db_writes for database persistence
//   - Sets _fully_loaded = true when complete
//
// SCANNING METHODS:
//
// scan_items(const core::index_roots&, bool recursive, bool scan_if_offline, core::cancel_token)
//   - Batch scan for a set of root folders
//   - Used during initial indexing or import operations
//   - Calls validate_folder() then scan_item() for each file
//   - Returns vector<folder_scan_item> with all discovered files
//
// scan_items(const df::item_set&, load_thumbs, refresh_fs, only_if_needed, scan_if_offline, token)
//   - Scan specific items (e.g., visible items in view)
//   - Groups items by folder for efficient batch processing
//   - Optionally loads thumbnails if load_thumbs=true
//   - Can force filesystem refresh with refresh_from_file_system=true
//
// scan_item(folder, file_path, load_thumb, scan_if_offline, item, ft)
//   - Low-level per-file scanner
//   - Calls files::scan_file() to extract metadata/thumbnail
//   - Updates index_file_item in-place
//   - Queues item_db_write for database persistence
//   - Triggers location lookup for GPS coordinates
//
// scan_folder(folder_path, folder_ptr)
//   - Scans all files in a folder, recurses into subfolders
//   - Queues child folders via queue_scan_folder()
//
// validate_folder(folder_path, refresh_from_file_system, timestamp)
//   - Core folder synchronization method
//   - Compares in-memory index with filesystem contents
//   - Merges changes: adds new files, removes deleted files
//   - Handles sidecar file associations (XMP, etc.)
//   - Returns validate_folder_result with folder and was_updated flag
//
// ASYNC QUEUE METHODS:
// These methods queue work to appropriate background threads via async_strategy:
//
// queue_scan_folder(path)           -> async_queue::scan_folder
// queue_scan_folders(paths)         -> async_queue::scan_folder
// queue_scan_listed_items(items)    -> async_queue::scan_folder (with cancel token)
// queue_scan_modified_items(items)  -> async_queue::scan_modified_items
// queue_scan_displayed_items(items) -> async_queue::scan_displayed_items (with cancel token)
// queue_update_presence(items)      -> async_queue::index_presence_single
// queue_update_predictions()        -> async_queue::index_predictions_single
// queue_update_summary()            -> async_queue::index_summary_single
//
// The "*_single" queues use reset_and_enqueue() to cancel pending work,
// ensuring only the latest request is processed.
//
// DUPLICATE DETECTION:
// update_predictions() runs after indexing to identify duplicate files:
//   - Hashes files by: name, created date, CRC32C, file size
//   - Groups files with matching hashes
//   - Confirms matches using is_dup_match() (name + date, or CRC32C)
//   - Assigns duplicate group IDs for UI display
//
// DATABASE PERSISTENCE:
// Item writes are queued to _db_writes (item_writes_t) and batched:
//   - item_db_write contains optional fields for partial updates
//   - database thread calls db.perform_writes() periodically
//   - merge_folder() loads cached items back from database at startup
//
////////////////////////////////////////////////////////////////////////////////////////////////////

class index_state final : public core::no_copy
{
	friend class search_counter;

	async_strategy& _async;

	core::mutex _summary_rw;

	index_items _items;
	_Guarded_by_(_summary_rw) index_summary _summary;
	_Guarded_by_(_summary_rw) uint64_t _summary_generation = 0;
	std::atomic_uint64_t _predictions_generation = 0;
	// Numbers each published search index. Written only by update_search_index, which runs on the
	// search index queue alone.
	uint64_t _search_index_generation = 0;
	// Written only on the UI thread by queue_scan_offline_thumbnails, read by the cloud worker so a
	// batch queued for a screenful that has since scrolled away can stop issuing network thumbnail
	// fetches. Atomic because the two contexts are unavoidably different: the worker cannot read the
	// UI-owned per-item visibility flag, and the value must not be raced on.
	std::atomic_uint64_t _offline_thumbnail_batch = 0;
	item_writes_t _db_writes;

	// Progress flags set on background scan/index threads and read from other threads
	// (is_init_complete, folder scanning). Atomic so the transition to true is published
	// safely rather than through a data race on a plain bool.
	std::atomic<bool> _cache_items_loaded = false;
	// Manual reset: the transition is terminal, so a waiter that arrives after the load has finished
	// must still be released rather than wait for a signal that has already been consumed.
	core::thread_event _cache_loaded_event{true, false};
	std::atomic<bool> _folders_indexed = false;
	std::atomic<bool> _collection_discovery_complete = false;
	const location_cache& _locations;
	std::atomic<bool> _fully_loaded = false;
	// Set when the library kinds change and cleared only by a re-reading of names that ran to the end, so a
	// pass another index update cancels is made again by the next.
	std::atomic<bool> _rederive_pending = false;

	// Paths a writer currently holds, counted because two batches can claim one path at once and a
	// plain set would let the first to finish release the second's claim. Every consumer queues from
	// the UI thread and claims are taken and released there too, so this needs no lock. Claimed items
	// are deferred, never dropped.
	core::hash_map<core::file_path, int, core::ihash, core::ieq> _write_claims;
	df::item_set _deferred_modified_scans;

	// Folders whose validation gave up to writers still publishing into them, each with whether its
	// follow-up must rescan the folder's items and subfolders rather than only revalidate it. Keyed by
	// folder so a folder that keeps giving up holds one queued follow-up, not one per attempt.
	core::mutex _deferred_validations_rw;
	_Guarded_by_(_deferred_validations_rw) core::hash_map<core::folder_path, bool, core::ihash, core::ieq>
	_deferred_validations;

	void add_distinct_other_folders(core::unique_folders folders);
	void enqueue_db_write(item_db_write write);
	void enqueue_db_writes(std::vector<item_db_write> writes);
	bool is_excluded_from_collection(core::folder_path folder) const;
	void queue_deferred_validation(folder_path folder_path, bool rescan);
	void publish_search_index(std::shared_ptr<const df::search_index> index);

public:
	bool is_collection_search(const df::search_t& search) const;

	// How a search is answered. A scan asks every item; automatic answers a selector-free search from the
	// search index where it is current and scans only what changed since. Both find the same items.
	enum class search_execution
	{
		automatic,
		scan,
	};

	// One listing of the index, and the search index evaluator classified against it when the index
	// answers. Built once and reused, so many searches share the listing and each other's terms.
	struct search_context
	{
		index_folders_t folders;
		std::optional<df::search_index_evaluator> evaluator;
	};

	search_context make_search_context(search_execution execution = search_execution::automatic) const;

	std::shared_ptr<const df::search_index> search_index() const
	{
		core::shared_lock lock(_summary_rw);
		return _summary._search_index;
	}

	// Reads the collection into a new search index when the published one has gone stale, drops it when
	// indexing has left it too stale to help, and does nothing while indexing is running. Runs on the
	// search index queue, never on a queue that indexing, searching or the sidebar waits for.
	void update_search_index();
	void queue_update_search_index();

	// Whether one item, by path, is among the results of a selector-free collection search: what the
	// scan of its folder would answer for it.
	bool matches_collection_search(const df::search_matcher& matcher, core::file_path path) const;

	enum class query_item_kind
	{
		file,
		folder,
		existing
	};

	struct query_item
	{
		query_item_kind kind = query_item_kind::file;
		core::file_path path;
		df::index_file_item file;
		folder_path folder_path;
		df::index_folder_item_ptr folder;
		df::search_result match;
	};

	using query_item_results = std::vector<query_item>;

	struct item_scan_request
	{
		std::weak_ptr<df::item_element> lifetime;
		core::file_path path;
		core::folder_path folder;
		file_type_ref file_type;
		bool is_folder = false;
		bool load_thumbnail = false;
		bool thumbnail_needed = false;
		bool had_thumbnail = false;
		uint64_t thumbnail_generation = 0;
	};

	using item_scan_requests = std::vector<item_scan_request>;

	// Per-batch scan accounting, so counters can be attributed to the caller that asked for the work.
	struct scan_batch_stats
	{
		uint64_t thumbs_requested = 0;
		uint64_t thumbs_scanned = 0;
	};

	struct thumbnail_result
	{
		std::weak_ptr<df::item_element> lifetime;
		core::file_path path;
		core::const_image_ptr thumbnail;
		core::const_image_ptr cover_art;
		core::date_t timestamp;
		uint64_t generation = 0;
	};

	using thumbnail_results = std::vector<thumbnail_result>;

	explicit index_state(async_strategy& as, const location_cache& locations);
	const location_cache& locations() const { return _locations; }

	void init_item_index();
	void reset();

	void invalidate_view(view_invalid invalid) const;

	std::vector<std::pair<core::file_path, df::index_file_item>> duplicate_list(uint32_t group) const;

	std::atomic_int scanning_items = 0;
	std::atomic_int thumbnailing_items = 0;
	std::atomic_int detecting = 0;
	std::atomic_int indexing = 0;
	std::atomic_int searching = 0;

	index_statistic stats;
	std::atomic<std::shared_ptr<const index_progress_snapshot>> _indexing_progress =
		std::make_shared<const index_progress_snapshot>();

	void cache_load_complete()
	{
		_cache_items_loaded = true;
		_cache_loaded_event.set();
	}

	// Waited on by the index bring-up, which must not validate folders while merge_folder is still
	// writing the same folder nodes. Also set when no cache can arrive, so the wait is not a timeout.
	core::thread_event& cache_loaded_event()
	{
		return _cache_loaded_event;
	}
	void merge_folder(folder_path folder_path, const db_items_t& items);

	void save_media_position(core::file_path id, double media_position);
	// Each carries the revision its hash was read from; a file whose index entry no longer matches
	// keeps neither the published hash nor the database row, so the next pass hashes it again.
	void save_crc(core::file_path id, index_file_revision revision, uint32_t crc);
	void save_phash(core::file_path id, index_file_revision revision, const crypto::phash_rotations& phash);
	void save_phashes(std::vector<phash_result> hashes, bool continue_predictions = true);
	void queue_calc_perceptual_hashes(std::vector<phash_request> requests);
	void save_thumbnail(core::file_path id, const core::const_image_ptr& thumbnail_image,
	                    const core::const_image_ptr& cover_art, core::date_t scan_timestamp);
	void publish_thumbnail(std::weak_ptr<df::item_element> item, core::file_path path,
	                       core::const_image_ptr thumbnail, core::const_image_ptr cover_art,
	                       core::date_t timestamp, uint64_t generation = 0, bool fade_in = false,
	                       bool stage_surface = false) const;
	void publish_thumbnails(thumbnail_results results, bool invalidate_group_layout) const;
	void publish_item_update(std::weak_ptr<df::item_element> item, core::file_path path) const;
	void publish_thumbnail_failure(std::weak_ptr<df::item_element> item, core::file_path path,
	                               uint64_t generation = 0) const;
	void publish_crc(std::weak_ptr<df::item_element> item, core::file_path path, core::file_size size,
	                 core::date_t modified, df::item_online_status online_status, uint32_t existing_crc, uint32_t crc);

	df::index_file_item find_item(core::file_path id) const;

	df::file_group_histogram count_matches(const df::search_t& a, const core::cancel_token& token,
	                                       search_execution execution = search_execution::automatic);

	df::file_group_histogram label_summary(const std::string_view label) const
	{
		index_metadata_summary_const_ptr summary;
		{
			core::shared_lock lock(_summary_rw);
			summary = _summary._metadata;
		}
		const auto found = summary->_distinct_labels.find(label);
		return found != summary->_distinct_labels.end() ? found->second : df::file_group_histogram{};
	}

	df::file_group_histogram tag_summary(const std::string_view tag) const
	{
		index_metadata_summary_const_ptr summary;
		{
			core::shared_lock lock(_summary_rw);
			summary = _summary._metadata;
		}
		const auto found = summary->_distinct_tags.find(tag);
		return found != summary->_distinct_tags.end() ? found->second : df::file_group_histogram{};
	}

	df::file_group_histogram rating_summary(int r) const
	{
		if (r == 0 || r > 5) return {};
		if (r == -1) r = 0;
		index_metadata_summary_const_ptr summary;
		{
			core::shared_lock lock(_summary_rw);
			summary = _summary._metadata;
		}
		return summary->_distinct_ratings[r];
	}

	void update_predictions();
	void update_summary(uint64_t generation = 0);

	index_progress_snapshot indexing_progress() const
	{
		const auto snapshot = _indexing_progress.load();
		return snapshot ? *snapshot : index_progress_snapshot{};
	}

	void query_items(const df::search_t& search,
	                 const std::function<void(query_item_results, bool)>& found_callback,
	                 const core::cancel_token& token,
	                 search_execution execution = search_execution::automatic);
	df::item_set materialize_query_items(query_item_results items, const df::unique_items& existing) const;

	struct validate_folder_result
	{
		df::index_folder_item_ptr folder;
		bool was_updated = false;
		bool enumeration_failed = false;
		// Writers kept publishing into the folder while this pass merged it, so nothing was published
		// and folder holds the current node, which may predate the file system. A follow-up is queued.
		bool deferred = false;
	};

	validate_folder_result validate_folder(folder_path folder_path,
	                                       bool refresh_from_file_system, core::date_t timestamp);

	// A scan loop collects its rows here so the database thread is woken once per group rather than
	// once per file. Only the hand-off is deferred; the in-memory index is still updated per file.
	class db_write_batch : public core::no_copy
	{
		index_state& _index;
		std::vector<item_db_write> _writes;

		// Bounds both the memory held and the work lost if the process dies mid-folder.
		static constexpr size_t max_pending = 64;

	public:
		explicit db_write_batch(index_state& index) : _index(index)
		{
		}

		~db_write_batch()
		{
			flush();
		}

		void add(item_db_write write)
		{
			_writes.emplace_back(std::move(write));
			if (_writes.size() >= max_pending) flush();
		}

		void flush()
		{
			if (_writes.empty()) return;
			_index.enqueue_db_writes(std::move(_writes));
			_writes.clear();
		}
	};

	// The apply half of scan_item. Separate from the scan itself so a result produced where the file
	// is already open (files::update, which owns the only cache-coherent handle) can be applied
	// without a second open.
	void apply_scan_result(const df::index_folder_item_ptr& folder, file_path file_path,
	                       const file_scan_result& sr, core::date_t now, core::date_t thumbnail_version,
	                       bool load_thumb, bool thumbnail_needed, bool had_thumbnail,
	                       const std::weak_ptr<df::item_element>& item, bool publish_to_item,
	                       bool publish_item_update_immediately, bool invalidate_summary,
	                       db_write_batch* writes = nullptr, bool clear_existing_hashes = false,
	                       uint64_t thumbnail_generation = 0);
	void scan_item(const df::index_folder_item_ptr& folder, file_path file_path, bool load_thumbnails,
	               bool thumbnail_needed, bool had_thumbnail, bool scan_if_offline,
	               const std::weak_ptr<df::item_element>& item, bool publish_to_item, file_type_ref ft,
	               bool force = false,
	               bool publish_item_update_immediately = true, bool invalidate_summary = true,
	               db_write_batch* writes = nullptr, uint64_t thumbnail_generation = 0);
	void scan_item(const df::item_element_ptr& i, bool load_thumb, bool scan_if_offline);
	// Immediately (synchronously, on the caller's thread) apply the scan that files::update took
	// through its still-open, cache-coherent handle. Reuses the cached folder node (no filesystem
	// refresh) and, when coherent, stamps file_modified == metadata_scanned == known_modified so the
	// later background rescan is a no-op. This is the SMB read-after-write fix.
	void apply_scan_now(const item_scan_request& request, const file_scan_result& sr, bool coherent,
	                    core::date_t known_modified);
	// Applies the scan a write took behind itself, and reports whether the item still needs a forced
	// background rescan - which it does only when the write produced no scan to apply.
	bool apply_write_scan(const item_scan_request& request, const file_update_result& result);
	// What files::update needs to produce a scan the index can apply without reopening the file.
	static rescan_spec make_rescan_spec(const item_scan_request& request, std::string_view xmp_sidecar,
	                                    bool want_image = false, bool want_handle = false);
	void scan_offline_item(const df::index_folder_item_ptr& folder, file_path file_path, bool thumbnail_needed,
	                       const std::weak_ptr<df::item_element>& item, bool publish_to_item,
	                       const df::index_file_item& file, core::date_t now, bool invalidate_summary,
	                       uint64_t thumbnail_generation = 0);
	bool needs_scan(const df::item_element_ptr&) const;
	bool is_in_collection(core::folder_path folder) const;

	void index_folders(core::cancel_token token, bool scan_collection = true);
	void index_roots(index_roots roots);
	// What each collection folder holds, as its owner set it in Options. Answers whether a video in it can
	// be a film or an episode; a change takes effect through rederive_name_values. Returns whether it changed.
	bool library_kinds(std::vector<std::pair<core::folder_path, prop::library_kind>> kinds);
	// The kind the nearest collection folder holding this folder was given; automatic when none was.
	prop::library_kind library_kind_of(core::folder_path folder) const;
	// Reads every loaded video's name again, as loading it does, and republishes the items it changes, when the
	// library kinds have changed since a pass last ran to the end: without a rescan. Returns whether any item
	// changed.
	bool rederive_name_values(const core::cancel_token& token);
	void scan_uncached(const core::cancel_token& token);
	std::vector<folder_scan_item> scan_items(const core::index_roots& roots, bool recursive, bool scan_if_offline,
	                                         const core::cancel_token& token);
	bool scan_items(const df::item_set& items, bool load_thumbs, bool refresh_from_file_system, bool only_if_needed,
	                bool scan_if_offline, const core::cancel_token& token, bool force = false);
	bool scan_items(const item_scan_requests& requests, bool refresh_from_file_system, bool only_if_needed,
	                bool scan_if_offline, const core::cancel_token& token, bool force = false,
	                scan_batch_stats* stats_out = nullptr);
	static item_scan_request make_scan_request(const df::item_element_ptr& item, bool load_thumbnail,
	                                           bool claim_loading = true);
	static item_scan_requests make_scan_requests(const df::item_set& items, bool load_thumbnails);
	void scan_folder(folder_path folder_path, const df::index_folder_item_ptr& folder);
	// True when the folder differed from what the index held.
	bool scan_folder(folder_path folder_path, bool mark_is_indexed, core::date_t timestamp);

	void queue_scan_folder(core::folder_path path);
	void queue_scan_folders(core::unique_folders paths);
	// Folder-watch response. Compares the watched folders against the index and only asks the view to
	// refresh when the comparison found something, so a write we made ourselves - whose new modified
	// time is already published - costs one enumeration and nothing more.
	void queue_validate_changed_folders(core::unique_folders paths);
	void queue_scan_listed_items(const df::item_set& listed_items);
	void queue_scan_modified_items(df::item_set items_to_scan, bool force = false);
	void queue_scan_displayed_items(df::item_set visible);

	// Bracket a write so no read of the same file is queued while it runs. Reads asked for in between
	// are deferred and re-requested on release.
	void claim_for_write(const std::vector<core::file_path>& paths);
	void release_write_claim(const std::vector<core::file_path>& paths);
	void queue_stage_thumbnails(const df::item_elements& items);
	void queue_load_visible_thumbnails(const df::item_elements& visible);
	// Load one tooltip/hover thumbnail without cancelling the visible-items thumbnail batch.
	// Checks the thumbnail database first, then decodes locally or asks the cloud shell provider.
	void queue_load_thumbnail(df::item_element_ptr item,
	                         view_invalid invalid = view_invalid::tooltip | view_invalid::view_redraw);
	// Fetch shell (cloud provider) thumbnails for visible cloud-only placeholders WITHOUT
	// hydrating. Best-effort; caches the result in the database so it is not re-fetched.
	void queue_scan_offline_thumbnails(const df::item_set& items, bool visible_only = true,
	                                   view_invalid invalid = view_invalid::tooltip | view_invalid::view_redraw);
	void queue_update_presence(const df::item_set& items);
	void queue_update_predictions();
	void queue_update_summary();

	std::vector<core::file_path> all_indexed_items() const
	{
		const auto folders = _items.all_folders();
		std::vector<core::file_path> results;
		results.reserve(10000);

		for (const auto& ff : folders)
		{
			for (const auto& file : ff.second->files)
			{
				results.emplace_back(ff.first, file.name);
			}
		}

		return results;
	}

	// Clears every cached scan timestamp so needs_scan_impl reports the whole collection as
	// unscanned and the next scan_uncached re-reads it from the files. Metadata payloads are
	// kept: they carry index-only values such as media position that exist nowhere else once
	// the database has been deleted, and a rescan replaces them anyway.
	void forget_cached_metadata()
	{
		const auto folders = _items.all_folders();

		for (const auto& folder : folders)
		{
			_items.update_folder(folder.first, [](const df::index_folder_item_ptr& current_folder)
			{
				for (const auto& file : current_folder->files)
				{
					file.metadata_scanned = core::date_t{};
				}
				current_folder->reset_search_presence();
				return true;
			});
		}

		_fully_loaded = false;
	}

	item_writes_t& db_writes()
	{
		return _db_writes;
	}

	core::unique_folders distinct_folders() const
	{
		std::shared_ptr<const core::unique_folders> folders;
		{
			core::shared_lock lock(_summary_rw);
			folders = _summary._distinct_folders;
		}
		return *folders;
	}

	index_histograms_const_ptr histograms() const
	{
		core::shared_lock lock(_summary_rw);
		return _summary._histograms;
	}

	location_matrix build_location_matrix(const location_matrix_params& params,
	                                      const core::unique_paths& excluded = {}) const;

	df::file_group_histogram file_types() const
	{
		const auto summary = histograms();
		return summary->_file_types;
	}

	std::vector<core::cached> distinct_genres() const;

	core::dense_string_counts distinct_words() const
	{
		index_metadata_summary_const_ptr summary;
		{
			core::shared_lock lock(_summary_rw);
			summary = _summary._metadata;
		}
		return summary->_distinct_words;
	}

	using distinct_results = std::vector<std::pair<std::string_view, df::file_group_histogram>>;

	distinct_results distinct_tags() const
	{
		index_metadata_summary_const_ptr summary;
		{
			core::shared_lock lock(_summary_rw);
			summary = _summary._metadata;
		}
		return {summary->_distinct_tags.begin(), summary->_distinct_tags.end()};
	}

	distinct_results distinct_labels() const
	{
		index_metadata_summary_const_ptr summary;
		{
			core::shared_lock lock(_summary_rw);
			summary = _summary._metadata;
		}
		return {summary->_distinct_labels.begin(), summary->_distinct_labels.end()};
	}

	prop_num_summary distinct_ratings() const
	{
		index_metadata_summary_const_ptr summary;
		{
			core::shared_lock lock(_summary_rw);
			summary = _summary._metadata;
		}
		return summary->_distinct_ratings;
	}

	core::index_roots index_roots() const
	{
		core::shared_lock lock(_summary_rw);
		return _summary._roots;
	}

	// Answers the same question as index_roots().folders.empty() without copying the root set.
	bool has_index_roots() const
	{
		core::shared_lock lock(_summary_rw);
		return !_summary._roots.folders.empty();
	}

	struct folder_total
	{
		core::folder_path folder;
		uint64_t count = 0;
		core::file_size size;
	};

	std::vector<folder_total> includes_with_totals() const;

	bool is_init_complete() const
	{
		return _cache_items_loaded && _folders_indexed;
	}

	struct auto_complete_word
	{
		std::string text;
		std::vector<core::part_t> highlights;
		int occurrences = 0;
	};

	struct auto_complete_folder
	{
		core::folder_path path;
		std::vector<core::part_t> highlights;
	};

	std::vector<auto_complete_word> auto_complete_words(std::string_view query, size_t max_results);
	std::vector<auto_complete_word> auto_complete_tag_companions(const std::vector<std::string>& tags,
	                                                             std::string_view prefix,
	                                                             size_t max_results) const;
	size_t tag_companion_entry_count() const;
	std::vector<auto_complete_word> auto_complete_locations(std::string_view query, size_t max_results,
	                                                        df::location_level level =
		                                                        df::location_level::any) const;
	std::vector<auto_complete_folder> auto_complete_folders(std::string_view query, size_t max_results) const;
	bool rebuild_sorted_words(index_metadata_summary& summary, uint64_t generation = 0) const;
	std::vector<std::string> auto_complete_text(prop::key_ref key);
};

// Items from a current result set, held the way the search index can intersect them: the documents of
// those it read and has not seen change, and the paths of the rest.
struct search_kept_items
{
	df::doc_set docs;
	std::vector<core::file_path> loose;
};

// Counts many searches against one listing of the index, so a sidebar's rows or one keystroke's
// predictions share the listing and the answers to the terms they have in common. Used by one worker at
// a time; reads the index as any search does and holds no lock between calls.
class search_counter final : public core::no_copy
{
	index_state& _index;
	std::optional<index_state::search_context> _context;

	index_state::search_context& context();

public:
	explicit search_counter(index_state& index) : _index(index)
	{
	}

	// Whether selector-free searches are answered by the search index rather than by a scan. Every answer
	// also asks each item the index has not read since it changed, so a pass counting many searches - a
	// sidebar's rows - uses the index only while those are few, and otherwise counts as it did without one.
	bool is_indexed();

	// What count_matches answers.
	df::file_group_histogram count(const df::search_t& search, const core::cancel_token& token);

	// The number of items a search finds, or nothing when only scanning many folders, or reading files,
	// could say: a prediction's count must never cost what running the search costs.
	std::optional<uint64_t> try_count_items(const df::search_t& search, const core::cancel_token& token);

	// Prepares a current result set for count_kept. Folders are skipped: no search's count holds them.
	search_kept_items make_kept_items(const std::vector<core::file_path>& paths);

	// How many of `items` the search finds, when the search index can say. A search scoped to folders or
	// related to an item has no answer here: only walking those folders, and reading what has not been
	// read, could give one.
	std::optional<uint64_t> count_kept(const df::search_t& search, const search_kept_items& items,
	                                   const core::cancel_token& token);
};
