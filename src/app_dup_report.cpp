// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implements the /dup-report command line option. Measures, over a real library, what the
// perceptual-hash stage of index_state::update_predictions would group together: capture-time bucket
// sizes, accepted edge distances, and - the part a pairwise threshold does not constrain - the size
// and Hamming diameter of the transitively closed components a union produces. Read-only: it opens
// no database, writes no file except the report, and changes no application behaviour.

#include "pch.h"
#include "app_command_line.h"
#include "crypto.h"
#include "files.h"
#include "metadata_xmp.h"
#include "libdesktop/platform.h"

#include <cstdio>
#include <numeric>

namespace
{
	// Mirrors model_index.cpp so the report measures the shipped rule rather than a restatement of it.
	constexpr int shipped_threshold = 6;
	constexpr uint64_t max_report_file_bytes = 128ull * 1024ull * 1024ull;

	// The report sweeps the threshold so the group-size curve is visible, not just the one point.
	constexpr int max_swept_threshold = 12;

	// Mirrors model_index.cpp: a picture that matches more than this many others under one timestamp
	// is a burst frame, and the whole capture time is declined rather than reported.
	constexpr size_t max_similar_pictures_at_one_capture_time = 2;

	// Mirrors model_index.cpp: past this a capture time is not compared at all.
	constexpr size_t max_photos_sharing_capture_time = 8;

	// A capture second shared by more than this many photos is not a re-encode; enumerating its pairs
	// is quadratic and would dominate the run, so it is counted and reported instead of hashed.
	constexpr size_t max_bucket_for_pairs = 512;

	struct photo_entry
	{
		df::file_path path;
		uint64_t created_key = 0;
		df::date_t created;
		bool created_from_exif = false;
		crypto::phash_rotations phash{};
	};

	struct report_writer
	{
		platform::file_ptr file;
		bool write_failed = false;

		void line(const std::string_view text) const
		{
			printf("%.*s\n", static_cast<int>(text.size()), text.data());
			if (file)
			{
				const auto line = std::format("{}\n", text);
				if (file->write(std::bit_cast<const uint8_t*>(line.data()), line.size()) != line.size())
				{
					const_cast<report_writer*>(this)->write_failed = true;
				}
			}
		}
	};

	void collect_photos(const df::folder_path folder, std::vector<df::file_path>& result)
	{
		auto contents = platform::iterate_file_items(folder, false);

		for (const auto& f : contents.files)
		{
			const auto ft = files::file_type_from_name(f.name);

			if (ft && ft->has_trait(file_traits::bitmap))
			{
				result.emplace_back(folder, f.name);
			}
		}

		for (const auto& sub : contents.folders)
		{
			collect_photos(folder.combine(sub.name.sv()), result);
		}
	}

	struct union_find
	{
		std::vector<size_t> parents;
		std::vector<uint8_t> ranks;

		explicit union_find(const size_t n) : parents(n), ranks(n, 0)
		{
			std::iota(parents.begin(), parents.end(), 0u);
		}

		size_t root(size_t item)
		{
			while (parents[item] != item)
			{
				parents[item] = parents[parents[item]];
				item = parents[item];
			}
			return item;
		}

		void unite(const size_t left, const size_t right)
		{
			auto left_root = root(left);
			auto right_root = root(right);
			if (left_root == right_root) return;
			if (ranks[left_root] < ranks[right_root]) std::swap(left_root, right_root);
			parents[right_root] = left_root;
			if (ranks[left_root] == ranks[right_root]) ++ranks[left_root];
		}
	};

	struct candidate_pair
	{
		size_t left = 0;
		size_t right = 0;
		int distance = 0;
		size_t bucket_size = 0;
	};

	std::string format_histogram(const std::map<size_t, size_t>& counts, const std::string_view key_label)
	{
		std::string result;

		for (const auto& [key, count] : counts)
		{
			result += std::format("      {} {:<6} : {}\n", key_label, key, count);
		}

		return result;
	}

	struct anchor_stats
	{
		size_t sets = 0;
		size_t grouped_photos = 0;
		size_t largest = 0;
		int worst_diameter = 0;
		size_t declined_capture_times = 0;
		size_t declined_photos = 0;
		std::map<size_t, size_t> size_histogram;
	};

	anchor_stats calc_anchor_stats(const std::vector<photo_entry>& photos,
	                               const std::map<uint64_t, std::vector<size_t>>& buckets)
	{
		anchor_stats result;

		for (const auto& [key, members] : buckets)
		{
			if (members.size() < 2 || members.size() > max_photos_sharing_capture_time) continue;

			// Lowest path among the pictures that could be identified.
			std::optional<size_t> anchor;

			for (const auto m : members)
			{
				if (!crypto::phash_is_usable(photos[m].phash[0])) continue;
				if (!anchor.has_value() || photos[m].path < photos[*anchor].path) anchor = m;
			}

			if (!anchor.has_value()) continue;

			std::vector<size_t> matched;

			// Mirrors the shipped crowd rule: only an untuned match is burst evidence, because
			// continuous shooting never produces a rotated frame.
			size_t same_orientation_matches = 0;

			for (const auto m : members)
			{
				if (m == *anchor || !crypto::phash_is_usable(photos[m].phash[0])) continue;
				if (crypto::phash_distance(photos[*anchor].phash[0], photos[m].phash) <= shipped_threshold)
				{
					matched.push_back(m);

					if (crypto::phash_distance(photos[*anchor].phash[0], photos[m].phash[0]) <= shipped_threshold)
					{
						++same_orientation_matches;
					}
				}
			}

			if (matched.empty()) continue;

			if (same_orientation_matches > max_similar_pictures_at_one_capture_time)
			{
				++result.declined_capture_times;
				result.declined_photos += matched.size() + 1;
				continue;
			}

			++result.sets;
			const auto set_size = matched.size() + 1;
			result.grouped_photos += set_size;
			result.largest = std::max(result.largest, set_size);
			++result.size_histogram[set_size];

			matched.push_back(*anchor);

			for (size_t a = 0; a < matched.size(); ++a)
			{
				for (size_t b = a + 1; b < matched.size(); ++b)
				{
					result.worst_diameter = std::max(result.worst_diameter,
					                                crypto::phash_distance(photos[matched[a]].phash[0],
					                                                       photos[matched[b]].phash));
				}
			}
		}

		return result;
	}
}

duplicate_report_anchor_stats calc_duplicate_report_anchor_stats_for_tests(
	const std::vector<duplicate_report_test_photo>& test_photos)
{
	std::vector<photo_entry> photos;
	photos.reserve(test_photos.size());

	for (const auto& test_photo : test_photos)
	{
		photo_entry photo;
		photo.path = test_photo.path;
		photo.created_key = test_photo.created_key;
		photo.phash = test_photo.phash;
		photos.emplace_back(photo);
	}

	std::map<uint64_t, std::vector<size_t>> buckets;
	for (auto i = 0_z; i < photos.size(); ++i) buckets[photos[i].created_key].push_back(i);

	const auto stats = calc_anchor_stats(photos, buckets);
	return {stats.sets, stats.grouped_photos, stats.declined_capture_times};
}

int run_duplicate_report(const std::string_view folder_text, const std::string_view output_text)
{
	load_file_types();
	metadata_xmp::initialise();

	const auto folder = df::folder_path(folder_text);

	if (!platform::exists(folder))
	{
		printf("dup-report: folder not found: %.*s\n", static_cast<int>(folder_text.size()), folder_text.data());
		return 1;
	}

	report_writer out;

	if (!output_text.empty())
	{
		const auto output_path = df::find_last_slash(output_text) == std::string_view::npos
			                         ? df::file_path(df::folder_path("."), output_text)
			                         : df::file_path(output_text);
		out.file = platform::open_file(output_path, platform::file_open_mode::create);
		if (!out.file)
		{
			printf("dup-report: could not open output file: %.*s\n", static_cast<int>(output_text.size()),
			       output_text.data());
			return 1;
		}
	}

	out.line(std::format("Duplicate grouping report"));
	out.line(std::format("  folder            : {}", folder));
	out.line(std::format("  shipped threshold : {}", shipped_threshold));
	out.line("");

	printf("Enumerating photos...\n");
	std::vector<df::file_path> paths;
	collect_photos(folder, paths);
	printf("  %zu photos found\n", paths.size());

	// Pass 1: capture time only. The perceptual stage never sees a file whose capture time is unique,
	// so hashing before bucketing would decode the whole library for nothing.
	files ff;
	std::vector<photo_entry> photos;
	photos.reserve(paths.size());

	size_t unreadable = 0;
	size_t no_capture_time = 0;

	for (size_t i = 0; i < paths.size(); ++i)
	{
		if ((i % 500) == 0) printf("  scanning %zu / %zu\r", i, paths.size());

		const auto path = paths[i];
		const auto ft = files::file_type_from_name(path);
		auto scan = ff.scan_file(path, false, ft);

		if (!scan.success)
		{
			++unreadable;
			continue;
		}

		const auto props = scan.to_props();

		if (!props)
		{
			++unreadable;
			continue;
		}

		const auto created = props->created();

		if (!created.is_valid())
		{
			++no_capture_time;
			continue;
		}

		photo_entry entry;
		entry.path = path;
		entry.created = created;
		entry.created_key = created.to_int64();
		entry.created_from_exif = props->dates.has_source(prop::date_source::exif_original);
		photos.emplace_back(entry);
	}

	printf("  scanning complete            \n");

	// Bucket by exact capture time - the gate update_predictions applies via shares_capture_time.
	std::map<uint64_t, std::vector<size_t>> buckets;

	for (size_t i = 0; i < photos.size(); ++i)
	{
		buckets[photos[i].created_key].push_back(i);
	}

	std::map<size_t, size_t> bucket_histogram;
	std::vector<size_t> to_hash;
	std::vector<const std::vector<size_t>*> oversized_buckets;
	size_t exif_dated = 0;

	for (const auto& p : photos) if (p.created_from_exif) ++exif_dated;

	for (const auto& [key, members] : buckets)
	{
		++bucket_histogram[members.size()];

		if (members.size() < 2) continue;

		if (members.size() > max_bucket_for_pairs)
		{
			oversized_buckets.push_back(&members);
			continue;
		}

		to_hash.insert(to_hash.end(), members.begin(), members.end());
	}

	// Pass 2: hash only the photos the gate would actually hand to the perceptual stage.
	printf("Hashing %zu candidates...\n", to_hash.size());

	size_t declined = 0;
	size_t hash_failed = 0;

	for (size_t i = 0; i < to_hash.size(); ++i)
	{
		if ((i % 200) == 0) printf("  hashing %zu / %zu\r", i, to_hash.size());

		auto& entry = photos[to_hash[i]];
		file_read_stream stream;

		if (stream.open(entry.path) && stream.size() <= max_report_file_bytes)
		{
			df::blob owner;
			entry.phash = ff.calc_perceptual_hash_rotations(stream.view_all(owner));
		}

		if (entry.phash[0] == 0) ++hash_failed;
		else if (!crypto::phash_is_usable(entry.phash[0])) ++declined;
	}

	printf("  hashing complete            \n\n");

	// Every pair the perceptual stage would judge, with the distance it would judge it on.
	std::vector<candidate_pair> pairs;
	std::map<int, size_t> distance_histogram;

	for (const auto& [key, members] : buckets)
	{
		if (members.size() < 2 || members.size() > max_bucket_for_pairs) continue;

		for (size_t a = 0; a < members.size(); ++a)
		{
			for (size_t b = a + 1; b < members.size(); ++b)
			{
				const auto& left = photos[members[a]];
				const auto& right = photos[members[b]];

				if (!crypto::phash_is_usable(left.phash[0]) || !crypto::phash_is_usable(right.phash[0])) continue;

				const auto distance = crypto::phash_distance(left.phash[0], right.phash);
				++distance_histogram[distance];

				if (distance <= max_swept_threshold)
				{
					pairs.push_back({members[a], members[b], distance, members.size()});
				}
			}
		}
	}

	out.line("Population");
	out.line(std::format("  photos found            : {}", paths.size()));
	out.line(std::format("  unreadable / no props   : {}", unreadable));
	out.line(std::format("  no capture time         : {}", no_capture_time));
	out.line(std::format("  with capture time       : {}", photos.size()));
	out.line(std::format("    from EXIF original    : {}", exif_dated));
	out.line(std::format("    from file/container   : {}", photos.size() - exif_dated));
	out.line(std::format("  distinct capture times  : {}", buckets.size()));
	out.line(std::format("  hashed                  : {}", to_hash.size()));
	out.line(std::format("    declined (low detail) : {}", declined));
	out.line(std::format("    unreadable at hash    : {}", hash_failed));
	out.line(std::format("  buckets over {} photos : {}", max_bucket_for_pairs, oversized_buckets.size()));
	out.line("");

	out.line("Photos sharing one exact capture time");
	out.line("  (a bucket above 2 is a burst, not a re-encode - this is what the gate selects)");
	out.line(format_histogram(bucket_histogram, "photos"));

	out.line("Pairwise distances among photos sharing a capture time");
	{
		std::string lines;
		size_t at_or_under = 0;

		for (const auto& [distance, count] : distance_histogram)
		{
			if (distance <= shipped_threshold) at_or_under += count;
			lines += std::format("      distance {:<3} : {}\n", distance, count);
		}

		out.line(lines);
		out.line(std::format("  pairs at or under the shipped threshold : {}", at_or_under));
		out.line("");
	}

	// The part a pairwise threshold says nothing about: what unite() actually produces.
	out.line("Transitive closure by threshold");
	out.line("  groups = components of size > 1; diameter = largest true distance inside a component");
	out.line("");

	for (auto threshold = 0; threshold <= max_swept_threshold; ++threshold)
	{
		union_find uf(photos.size());

		for (const auto& p : pairs)
		{
			if (p.distance <= threshold) uf.unite(p.left, p.right);
		}

		std::map<size_t, std::vector<size_t>> components;

		for (size_t i = 0; i < photos.size(); ++i)
		{
			if (photos[i].phash[0] == 0) continue;
			components[uf.root(i)].push_back(i);
		}

		size_t groups = 0;
		size_t grouped_photos = 0;
		size_t largest = 0;
		int worst_diameter = 0;
		std::map<size_t, size_t> size_histogram;

		for (const auto& [root, members] : components)
		{
			if (members.size() < 2) continue;

			++groups;
			grouped_photos += members.size();
			largest = std::max(largest, members.size());
			++size_histogram[members.size()];

			for (size_t a = 0; a < members.size(); ++a)
			{
				for (size_t b = a + 1; b < members.size(); ++b)
				{
					worst_diameter = std::max(worst_diameter,
					                          crypto::phash_distance(photos[members[a]].phash[0],
					                                                 photos[members[b]].phash));
				}
			}
		}

		const auto marker = threshold == shipped_threshold ? "  <== shipped" : "";

		out.line(std::format("  threshold {:<2} : {} groups, {} photos, largest {}, worst in-group distance {}{}",
		                     threshold, groups, grouped_photos, largest, worst_diameter, marker));

		if (threshold == shipped_threshold)
		{
			out.line("      group sizes:");
			out.line(format_histogram(size_histogram, "size"));

			// A component wider than the threshold only exists because unite() is transitive.
			out.line("      components whose diameter exceeds the threshold:");
			auto listed = 0;

			for (const auto& [root, members] : components)
			{
				if (members.size() < 2) continue;

				int diameter = 0;

				for (size_t a = 0; a < members.size(); ++a)
				{
					for (size_t b = a + 1; b < members.size(); ++b)
					{
						diameter = std::max(diameter, crypto::phash_distance(photos[members[a]].phash[0],
						                                                    photos[members[b]].phash));
					}
				}

				if (diameter <= threshold) continue;
				if (++listed > 40) break;

				out.line(std::format("        {} photos, diameter {}, captured {}",
				                     members.size(), diameter, photos[members.front()].created));

				for (const auto member : members)
				{
					out.line(std::format("          {}", photos[member].path));
				}
			}

			if (listed == 0) out.line("        none");
			out.line("");
		}
	}

	if (!oversized_buckets.empty())
	{
		out.line("");
		out.line(std::format("Skipped {} capture times holding more than {} photos each:",
		                     oversized_buckets.size(), max_bucket_for_pairs));

		for (const auto* const members : oversized_buckets)
		{
			out.line(std::format("  {} photos at {}", members->size(), photos[members->front()].created));
		}
	}

	// A capture second shared by many photos is a burst, which is evidence against the re-encode the
	// gate is looking for. This is what refusing those buckets would cost and bound.
	out.line("");
	out.line(std::format("Effect of a bucket-size gate at threshold {}", shipped_threshold));

	for (const size_t cap : {size_t{2}, size_t{3}, size_t{4}, size_t{6}, size_t{8}, size_t{16}, SIZE_MAX})
	{
		union_find uf(photos.size());

		for (const auto& p : pairs)
		{
			if (p.distance <= shipped_threshold && p.bucket_size <= cap) uf.unite(p.left, p.right);
		}

		std::map<size_t, std::vector<size_t>> components;

		for (size_t i = 0; i < photos.size(); ++i)
		{
			if (photos[i].phash[0] == 0) continue;
			components[uf.root(i)].push_back(i);
		}

		size_t groups = 0;
		size_t grouped_photos = 0;
		size_t largest = 0;
		int worst_diameter = 0;

		for (const auto& [root, members] : components)
		{
			if (members.size() < 2) continue;

			++groups;
			grouped_photos += members.size();
			largest = std::max(largest, members.size());

			for (size_t a = 0; a < members.size(); ++a)
			{
				for (size_t b = a + 1; b < members.size(); ++b)
				{
					worst_diameter = std::max(worst_diameter,
					                          crypto::phash_distance(photos[members[a]].phash[0],
					                                                 photos[members[b]].phash));
				}
			}
		}

		const auto label = cap == SIZE_MAX ? std::string("none") : std::to_string(cap);

		out.line(std::format("  max photos per capture time {:<5} : {} groups, {} photos, largest {}, "
		                     "worst in-group distance {}",
		                     label, groups, grouped_photos, largest, worst_diameter));
	}

	// The shipped model: sets are anchored on one item rather than closed over transitively, and a
	// capture time whose anchor answers with a crowd is declined in full.
	out.line("");
	out.line(std::format("Anchored sets with the crowd rule (threshold {}, crowd limit {})",
	                     shipped_threshold, max_similar_pictures_at_one_capture_time));

	{
		const auto stats = calc_anchor_stats(photos, buckets);

		out.line(std::format("  sets                         : {}", stats.sets));
		out.line(std::format("  photos in a set              : {}", stats.grouped_photos));
		out.line(std::format("  largest set                  : {}", stats.largest));
		out.line(std::format("  worst in-set distance        : {}", stats.worst_diameter));
		out.line(std::format("  capture times declined       : {}", stats.declined_capture_times));
		out.line(std::format("  photos those would have held : {}", stats.declined_photos));
		out.line("      set sizes:");
		out.line(format_histogram(stats.size_histogram, "size"));
	}

	if (out.file && !out.file->flush()) out.write_failed = true;

	if (out.write_failed)
	{
		printf("dup-report: could not write output file: %.*s\n", static_cast<int>(output_text.size()),
		       output_text.data());
		return 1;
	}

	return 0;
}