// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: File indexing engine. Scans folders, extracts metadata, keeps the in-memory item
// collection and answers queries over it. Duplicate prediction and presence live in
// model_index_duplicates.cpp; the summary and its vocabulary in model_index_summary.cpp.

#include "pch.h"

#include "model_index.h"
#include "model_index_internal.h"
#include "model_db.h"
#include "model_locations.h"
#include "model_property.h"
#include "model.h"
#include "metadata_xmp.h"
#include "util_crash_files_db.h"
#include "util_text.h"

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

constexpr auto max_folders_to_index = 100000;

// A row written before the quarter turns were stored carries the first hash alone. Reporting it as
// unhashed asks for the other three, rather than leaving a rotated copy permanently unrecognisable.
static df::picture_hashes_ptr picture_hashes_from_db(const crypto::phash_rotations& rotations)
{
	if (rotations[0] == 0) return nullptr;
	if (rotations[0] == crypto::phash_declined) return df::make_picture_hashes(rotations);

	const auto complete = std::ranges::all_of(rotations, [](const uint64_t h) { return crypto::phash_is_usable(h); });
	return complete ? df::make_picture_hashes(rotations) : nullptr;
}

df_assert_pod(df::file_path);df_assert_pod(df::file_group_histogram);
df_assert_pod(search_presence_mask);
df_assert_pod(key_val);
df_assert_movable(df::index_file_item);
df_assert_movable(folder_scan_item);
df_assert_movable(index_state::query_item);
df_assert_movable(index_state::item_scan_request);
df_assert_movable(index_state::thumbnail_result);
df_assert_movable(index_state::validate_folder_result);

static_assert(sizeof(search_presence_mask) == 4);
static_assert(sizeof(df::file_path) == sizeof(str::cached) * 2);
static_assert(sizeof(key_val) == sizeof(str::cached) * 2);

// Every query walks these fields once per candidate. A type that outgrows a machine word makes
// std::atomic fall back to a lock, which x64 hides for 16 bytes and ARM64 does not, so a widened
// field would turn the hot index record into a contended mutex rather than fail here.
static_assert(std::atomic<df::date_t>::is_always_lock_free);
static_assert(std::atomic<df::duplicate_info>::is_always_lock_free);
static_assert(std::atomic<search_presence_mask>::is_always_lock_free);

std::function<void(df::folder_path folder, int attempt)> test_after_validate_folder_snapshot;

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

struct query_items_result
{
	index_state::query_item_results results;

	void match_item(const df::file_path id, const df::index_file_item& file, const df::search_result& match)
	{
		results.push_back({index_state::query_item_kind::file, id, file, {}, {}, match});
	}

	void match_folder(const df::folder_path folder_path, const df::index_folder_item_ptr& folder)
	{
		results.push_back({index_state::query_item_kind::folder, {}, {}, folder_path, folder, {}});
	}
};

struct count_items_result
{
	df::file_group_histogram summary;

	void match_item(const df::file_path id, const df::index_file_item& file, const df::search_result& match)
	{
		summary.record(file, id);
	}

	static void match_folder(const df::folder_path folder_path, const df::index_folder_item_ptr& folder)
	{
	}
};

// A related search written as text - typed, or restored from a favorite or saved search -
// carries only the path, so every field the relation axes compare is recovered from the
// index before any matching happens. Files outside the collection fall back to the path.
static const df::search_t& resolve_related(const df::search_t& search, const index_state& state,
                                           df::search_t& storage)
{
	if (!search.has_related())
	{
		return search;
	}

	const auto path = search.related().path;
	const auto load_index_facts = [](df::related_info& r, const df::index_file_item& found)
	{
		r.name = found.name;
		r.size = found.size;
		r.file_created = found.file_created;
		r.ft = found.ft;
		r.crc32c = found.crc32c.load();
		const auto duplicates = found.duplicates.load();
		r.group = duplicates.group;
		r.exact_group = found.exact_duplicate_group.load();
		r.duplicate_grade = duplicates.grade;
		r.duplicate_crowded = duplicates.same_picture_crowded;

		if (const auto hashes = found.phash.load())
		{
			r.phash = hashes->rotations;
		}

		const auto md = found.metadata.load();

		if (md)
		{
			r.gps = md->coordinate;
			r.metadata_created = md->created();
			r.dimensions = md->dimensions();
			r.album = md->album;
			r.album_artist = md->album_artist;
			r.show = md->show;
			r.season = md->season;
			r.episode = md->episode;
			r.disk = md->disk;
			r.track = md->track;
		}
	};

	if (search.related().is_loaded)
	{
		// The snapshot was taken when the command ran, but duplicate grouping is recomputed behind it
		// and perceptual groups are anchored, so the direct pair evidence is refreshed from the index.
		const auto found = state.find_item(path);

		if (!found.ft)
		{
			return search;
		}

		df::related_info r = search.related();
		load_index_facts(r, found);

		storage = search;
		storage.related(r);
		return storage;
	}

	const auto found = state.find_item(path);

	df::related_info r;
	r.path = path;

	if (found.ft)
	{
		load_index_facts(r, found);
	}
	else
	{
		r.name = str::cache(path.name());
		r.ft = files::file_type_from_name(path);
	}

	r.is_loaded = true;

	storage = search;
	storage.related(r);
	return storage;
}

template <typename T>
static void iterate_items(const df::search_t& search_in,
                          T& results,
                          index_state& state,
                          df::cancel_token token,
                          index_items& index,
                          bool refresh_from_file_system,
                          bool show_sidecars)
{
	df::search_t resolved_related;
	const auto& search = resolve_related(search_in, state, resolved_related);

	df::search_matcher matcher(search, platform::now().to_days(), &state.locations());

	const auto now = platform::now();
	const auto& selectors = search.selectors();
	const auto has_selector = !selectors.empty();
	const auto has_related = search.has_related();

	// A related search answers with the closest matches on each axis, so its results are collected
	// and ranked here rather than reported as they are found. Counting shares this path so a count
	// can never disagree with the set the same search displays.
	struct related_payload
	{
		df::index_file_item file;
		df::search_result match;
	};

	df::related_collector<related_payload> related_slots;

	// Two selectors can name overlapping trees - `folders_scanned` only stops one selector walking a
	// folder twice - and a file matched by both is still one file. The set is only paid for when
	// there is more than one selector to overlap, and folders cannot repeat within a single walk.
	df::unique_paths emitted;
	df::unique_folders emitted_folders;
	const auto selectors_can_overlap = selectors.size() > 1;

	const auto report = [&results, &related_slots, &emitted, has_related, selectors_can_overlap](
		const df::file_path id,
		const df::index_file_item& file,
		const df::search_result& match)
	{
		if (selectors_can_overlap && !emitted.emplace(id).second)
		{
			return;
		}

		if (has_related)
		{
			related_slots.offer({df::related_axis_of(match.type), match.distance}, id, {file, match});
		}
		else
		{
			results.match_item(id, file, match);
		}
	};

	const auto report_folder = [&results, &emitted_folders, selectors_can_overlap](
		const df::folder_path path, const df::index_folder_item_ptr& folder)
	{
		if (selectors_can_overlap && !emitted_folders.emplace(path).second)
		{
			return;
		}

		results.match_folder(path, folder);
	};

	if (has_selector)
	{
		for (const auto& selector : selectors)
		{
			if (token.is_cancelled())
				break;

			const auto recursive = selector.is_recursive();
			const auto wildcard = selector.wildcard();

			files ff;
			std::vector<df::folder_path> folders = {selector.folder()};
			df::unique_folders folders_scanned;

			while (!folders.empty())
			{
				if (token.is_cancelled()) break;

				const auto current_folder = folders.back();
				folders.pop_back();

				if (!folders_scanned.contains(current_folder))
				{
					folders_scanned.emplace(current_folder);

					const auto found_node = state.validate_folder(current_folder, refresh_from_file_system, now);

					if (found_node.folder)
					{
						// Keep the snapshot alive for the whole loop: folders_snapshot() returns a
						// temporary shared_ptr, and dereferencing it into the range-for only extends the
						// vector's lifetime through the initializer, not the loop body. A concurrent
						// replace_child() dropping the last other reference mid-loop would otherwise free
						// the vector out from under the iteration.
						const auto folders_snapshot = found_node.folder->folders_snapshot();

						for (const auto& folder_entry : *folders_snapshot)
						{
							if (token.is_cancelled()) break;

							const auto folder_path = current_folder.combine(folder_entry->name);

							if (folder_entry->is_excluded) continue;

							// The wildcard names the files being looked for, not the folders they are
							// under. Gating the descent on it left "*.jpg" searching only folders called
							// *.jpg, so every nested match was missed.
							if (recursive && folder_entry->can_recurse)
							{
								folders.emplace_back(folder_path);
							}

							if (selector.has_wildcard() && !wildcard_icmp(folder_entry->name, wildcard))
							{
								continue;
							}

							if ((!recursive && !matcher.has_terms) || matcher.match_folder(
								folder_path.text(), folder_path.name()).is_match())
							{
								report_folder(folder_path, folder_entry);
							}
						}

						if (matcher.need_metadata)
						{
							for (const auto& f : found_node.folder->files)
							{
								if (token.is_cancelled())
									break;

								state.scan_item(found_node.folder, {current_folder, f.name}, false, false, false, false,
								                {}, false, f.ft);
							}
						}

						if (matcher.can_contain(found_node.folder->search_presence_summary))
						{
							for (const auto& file_node : found_node.folder->files)
							{
								if (token.is_cancelled())
									break;

								const auto& file = file_node;

								if (!selector.has_wildcard() || wildcard_icmp(file_node.name, wildcard))
								{
									if (!(file.flags && df::index_item_flags::is_sidecar) || show_sidecars)
									{
										const auto id = current_folder.combine_file(file_node.name);
										const auto match = matcher.match_item(id, file);

										if (match.is_match())
										{
											report(id, file, match);
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
	else
	{
		const auto folders = index.all_folders();

		for (const auto& folder_node : folders)
		{
			if (token.is_cancelled()) break;

			if (folder_node.second->is_in_collection)
			{
				// Folder masks are an early rejection only; each surviving item is checked
				// against its own mask and then by the authoritative exact matcher.
				if (has_related ||
					matcher.can_match_folder ||
					matcher.can_contain(folder_node.second->search_presence_summary))
				{
					for (const auto& file_node : folder_node.second->files)
					{
						if (token.is_cancelled()) break;

						if (!(file_node.flags && df::index_item_flags::is_sidecar) || show_sidecars)
						{
							const auto path = folder_node.first.combine_file(file_node.name);
							const auto match = matcher.match_item(path, file_node);

							if (match.is_match())
							{
								report(path, file_node, match);
							}
						}
					}
				}

				const auto match = matcher.match_folder(folder_node.first.text(), folder_node.first.name());

				if (match.is_match())
				{
					results.match_folder(folder_node.first, folder_node.second);
				}
			}
		}
	}

	if (has_related && !token.is_cancelled())
	{
		related_slots.drain([&results](const df::file_path id, related_payload&& payload)
		{
			results.match_item(id, payload.file, payload.match);
		});
	}
}

void index_state::query_items(const df::search_t& search,
                              const std::function<void(query_item_results, bool)>& found_callback,
                              const df::cancel_token& token)
{
	df::scope_locked_inc l(searching);

	_async.invalidate_view(view_invalid::view_layout);


	if (search.has_related())
	{
		record_feature_use(features::search_related);
	}

	if (search.is_duplicates())
	{
		record_feature_use(features::search_duplicates);
	}

	const auto selectors = search.selectors();
	const auto has_selector = !selectors.empty();

	if (has_selector)
	{
		record_feature_use(features::search_folder);

		for (const auto& selector : selectors)
		{
			if (selector.is_recursive())
			{
				record_feature_use(features::search_flatten);
			}
		}
	}

	query_items_result results;
	{
		df::bump(df::query_perf.queries);
		df::perf_timer timer(df::query_perf.query_us, &df::query_perf.query_max_us);
		iterate_items(search, results, *this, token, _items, true, search.has_related());
	}

	df::bump(df::query_perf.query_items, results.results.size());

	if (search.has_related())
	{
		const auto id = search.related().path;
		const auto folder = _items.find(id.folder());
		if (!(folder && folder->is_in_collection))
		{
			// The item the search started at is outside the collection, so the scan never saw it. It
			// still leads its own answer, which is what the negative distance orders it to.
			df::search_result anchor(df::search_result_type::similar);
			anchor.distance = -1;

			results.results.push_back({query_item_kind::existing, id, {}, {}, {}, anchor});
		}
	}

	if (search.has_term_type(df::search_term_type::text))
	{
		record_feature_use(features::search_text);
	}

	if (search.has_term_type(df::search_term_type::value))
	{
		record_feature_use(features::search_property);
	}

	if (search.has_term_type(df::search_term_type::has_type))
	{
		record_feature_use(features::search_type);
	}

	// The walk stops where the token was raised, so a cancelled pass has a partial set and must not
	// claim otherwise. Its caller re-opens and publishes the whole answer; a partial set announced
	// as complete settled the list showing fewer items than exist and said it was finished.
	found_callback(std::move(results.results), !token.is_cancelled());
}

df::item_set index_state::materialize_query_items(query_item_results items, const df::unique_items& existing) const
{
	df::assert_true(ui::is_ui_thread());
	df::bump(df::query_perf.materializations);
	df::bump(df::query_perf.materialize_items, items.size());
	df::perf_timer timer(df::query_perf.materialize_us, &df::query_perf.materialize_max_us);
	df::item_set results;
	results.reserve(items.size());

	for (auto& result : items)
	{
		df::item_element_ptr item;
		if (result.kind == query_item_kind::file)
		{
			item = existing.find(result.path);
			if (item)
			{
				item->update(result.path, result.file);
			}
			else
			{
				item = std::make_shared<df::item_element>(result.path, result.file);
			}
			item->search(result.match);
		}
		else if (result.kind == query_item_kind::folder)
		{
			item = existing.find(result.folder_path);
			if (!item)
			{
				item = std::make_shared<df::item_element>(result.folder_path, std::move(result.folder));
			}
		}
		else
		{
			item = existing.find(result.path);
			if (item) item->search(result.match);
		}

		if (item) results.add(item);
	}

	return results;
}

df::file_group_histogram index_state::count_matches(const df::search_t& search, const df::cancel_token& token)
{
	count_items_result result;

	if (is_collection_search(search))
	{
		df::measure_ms ms(stats.count_matches_ms);
		df::bump(df::query_perf.counts);
		df::perf_timer timer(df::query_perf.count_us);
		iterate_items(search, result, *this, token, _items, false, search.has_related());
	}

	return result.summary;
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

index_state::index_state(async_strategy& as, const location_cache& locations) : _async(as), _locations(locations)
{
}

void index_state::enqueue_db_write(item_db_write write)
{
	if (_db_writes.enqueue(std::move(write)))
	{
		// The database worker flushes writes after every task batch. One no-op task on the
		// empty-to-nonempty transition gives the write queue an explicit wake without flooding it.
		_async.queue_database([](database&)
		{
		});
	}
}

void index_state::enqueue_db_writes(std::vector<item_db_write> writes)
{
	if (writes.empty()) return;

	// The worker drains the write queue on every pass, so a producer that enqueues one row at a time
	// always finds it empty and wakes the database thread once per row - and each of those wakes opens
	// its own transaction. Handing over the whole group keeps it to one wake and one transaction.
	if (_db_writes.enqueue_all(std::move(writes)))
	{
		_async.queue_database([](database&)
		{
		});
	}
}

void index_state::init_item_index()
{
	auto summary = std::make_shared<index_metadata_summary>();

	for (const auto& f : all_file_groups())
	{
		++summary->_distinct_words[str::cache(std::format("@{}", f->name))];
		++summary->_distinct_words[str::cache(std::format("@{}", f->plural_name))];
	}

	summary->_distinct_text[prop::genre] = df::dense_unique_strings
	{
		"Abstract"_c,
		"Action & Adventure"_c,
		"Action"_c,
		"Aerial"_c,
		"Alternative"_c,
		"Analog"_c,
		"Animation"_c,
		"Anime"_c,
		"Architectural"_c,
		"Aviation"_c,
		"Blues"_c,
		"Brazilian"_c,
		"Candid"_c,
		"Children's"_c,
		"Christian & Gospel"_c,
		"Classic"_c,
		"Classical"_c,
		"Close-up"_c,
		"Cloudscape"_c,
		"Comedy"_c,
		"Conceptual"_c,
		"Concert Films"_c,
		"Concert"_c,
		"Conservation"_c,
		"Country"_c,
		"Dance"_c,
		"Documentary"_c,
		"Drama"_c,
		"Easy Listening"_c,
		"Electronic"_c,
		"Family"_c,
		"Fashion"_c,
		"Film Noir"_c,
		"Film still"_c,
		"Fine-art"_c,
		"Fire"_c,
		"Fireworks"_c,
		"Fitness & Workout"_c,
		"Food"_c,
		"Foreign"_c,
		"Forensic"_c,
		"Geophotography"_c,
		"Glamour"_c,
		"High key"_c,
		"High-speed"_c,
		"Hip-Hop/Rap"_c,
		"Holiday"_c,
		"Horror"_c,
		"Independent"_c,
		"Instrumental"_c,
		"Jazz"_c,
		"Kids & Family"_c,
		"Kids"_c,
		"Kirlian"_c,
		"Landscape"_c,
		"Latin"_c,
		"Lifestyle"_c,
		"Lo-fi"_c,
		"Lomography"_c,
		"Long-exposure"_c,
		"Low key"_c,
		"Macro"_c,
		"Medical"_c,
		"Monochrome"_c,
		"Music Documentaries"_c,
		"Music Feature Films"_c,
		"Musicals"_c,
		"Narrative"_c,
		"New Age"_c,
		"Night"_c,
		"Nonfiction"_c,
		"Opera"_c,
		"Panorama"_c,
		"Panoramic"_c,
		"Photo op"_c,
		"Photobiography"_c,
		"Photojournalism"_c,
		"Photowalking"_c,
		"Podcast"_c,
		"Polaroid"_c,
		"Pop"_c,
		"Portrait"_c,
		"R&B"_c,
		"Reality TV"_c,
		"Reggae"_c,
		"Rock"_c,
		"Romance"_c,
		"Satellite"_c,
		"Sci-Fi & Fantasy"_c,
		"Short Films"_c,
		"Singer/Songwriter"_c,
		"Social"_c,
		"Soft focus"_c,
		"Soul"_c,
		"Soundtrack"_c,
		"Special Interest"_c,
		"Sports"_c,
		"Star trail"_c,
		"Still life"_c,
		"Stock"_c,
		"Street"_c,
		"Subminiature"_c,
		"Teens"_c,
		"Thriller"_c,
		"Time-lapse"_c,
		"Travel"_c,
		"Ultraviolet"_c,
		"Underwater"_c,
		"Urban"_c,
		"Vernacular"_c,
		"Vintage"_c,
		"Vocal"_c,
		"War"_c,
		"Western"_c,
		"World"_c
	};

	rebuild_sorted_words(*summary);

	index_metadata_summary_const_ptr published = std::move(summary);
	{
		platform::exclusive_lock lock(_summary_rw);
		_summary._metadata.swap(published);
	}
}

void index_state::reset()
{
	_items.clear();
}

void index_state::add_distinct_other_folders(df::unique_folders folders)
{
	for (;;)
	{
		std::shared_ptr<const df::unique_folders> current;
		{
			platform::shared_lock lock(_summary_rw);
			current = _summary._distinct_other_folders;
		}

		if (std::ranges::all_of(folders, [&](const auto& folder) { return current->contains(folder); }))
		{
			return;
		}

		// Build the immutable replacement outside the write lock, then publish only if current is unchanged.
		auto next = std::make_shared<df::unique_folders>(folders);
		next->insert(current->begin(), current->end());

		{
			std::shared_ptr<const df::unique_folders> published = next;
			platform::exclusive_lock lock(_summary_rw);
			if (_summary._distinct_other_folders == current)
			{
				_summary._distinct_other_folders.swap(published);
				return;
			}
		}
	}
}

void index_state::invalidate_view(const view_invalid invalid) const
{
	_async.invalidate_view(invalid);
}


bool index_state::is_in_collection(const df::folder_path folder) const
{
	const auto parent = folder.parent();

	{
		platform::shared_lock lock(_summary_rw);
		const auto roots = _summary._roots;

		if (df::is_excluded(roots, folder))
		{
			return false;
		}

		if (roots.folders.contains(folder) ||
			roots.folders.contains(parent))
		{
			return true;
		}
	}

	const auto found_folder = _items.find(folder);

	if (found_folder)
	{
		return found_folder->is_in_collection;
	}

	const auto found_parent = _items.find(parent);

	if (found_parent)
	{
		return found_parent->is_in_collection;
	}

	return false;
}

static df::index_folder_item_ptr find_or_create_folder(index_items& items, const df::folder_path path,
                                                       const platform::folder_info& fd)
{
	df::assert_true(!is_empty(fd.name));

	auto candidate = std::make_shared<df::index_folder_item>();
	candidate->name = fd.name;
	candidate->modified = fd.attributes.modified;
	candidate->created = fd.attributes.created;
	candidate->is_read_only = fd.attributes.is_readonly;
	candidate->can_recurse = fd.can_recurse;
	const auto result = items.find_or_create(path, std::move(candidate));
	result->can_recurse = fd.can_recurse;
	return result;
}

void populate_file_info(df::index_file_item& file_node, const platform::file_info& fd, const bool cache_items_loaded)
{
	df::assert_true(!is_empty(fd.name));

	const auto name = fd.name;
	file_node.file_modified = df::date_t(fd.attributes.modified);
	file_node.file_created = fd.attributes.created;
	// Set or CLEAR these flags to reflect the current filesystem state. Clearing matters
	// for cloud-only placeholders: when OneDrive hydrates a file (offline -> online) the
	// is_offline flag must drop so the file is re-scanned via the normal (thumbnail) path.
	if (fd.attributes.is_readonly) file_node.flags |= df::index_item_flags::is_read_only;
	else file_node.flags &= ~df::index_item_flags::is_read_only;
	if (fd.attributes.is_offline) file_node.flags |= df::index_item_flags::is_offline;
	else file_node.flags &= ~df::index_item_flags::is_offline;
	file_node.size = fd.attributes.size;

	auto md = file_node.metadata.load();

	if (file_node.name != name)
	{
		file_node.name = name;
		file_node.ft = files::file_type_from_name(name);

		if (cache_items_loaded &&
			md == nullptr &&
			file_node.ft->has_trait(file_traits::file_name_metadata))
		{
			const auto ext_pos = df::find_ext(name);

			if (ext_pos != std::string_view::npos)
			{
				md = std::make_shared<prop::item_metadata>();
				const auto name_props = scan_info_from_title(name.substr(0, ext_pos));

				if (!str::is_empty(name_props.show)) md->show = str::cache(name_props.show);
				if (!str::is_empty(name_props.title)) md->title = str::cache(name_props.title);
				if (name_props.year != 0) md->year = name_props.year;
				if (name_props.episode != 0) md->episode = df::xy8::make(name_props.episode, name_props.episode_of);
				if (name_props.season != 0) md->season = name_props.season;
				md->file_name = name;
				file_node.metadata.store(md);
			}
		}
	}

	if (md && md->file_name.sv() != name)
	{
		auto updated = std::make_shared<prop::item_metadata>(*md);
		updated->file_name = name;
		file_node.metadata.store(std::move(updated));
	}
}

index_state::validate_folder_result index_state::validate_folder(const df::folder_path folder_path,
                                                                 const bool refresh_from_file_system,
                                                                 const df::date_t timestamp)
{
	auto existing_snapshot = _items.find_snapshot(folder_path);
	auto existing_folder = existing_snapshot.folder;

	if (refresh_from_file_system || !existing_folder)
	{
		bool changes_detected = false;

		df::index_item_infos updated_files;
		df::index_folder_infos updated_folders;
		std::vector<df::folder_path> removed_folders;
		std::unordered_multimap<std::string_view, str::cached, df::ihash, df::ieq> sidecars;
		df::hash_set<std::string_view, df::ihash, df::ieq> sidecar_extensions;
		// Files whose stored checksum or picture hash no longer describes their bytes. Clearing the
		// node's copy only lasts the session: the rows keep theirs, and the next launch loads them.
		std::vector<df::file_path> stale_hashes;
		df::dense_unique_strings loaded_from_database;

		[[maybe_unused]] auto less_ptr_name = [](const auto& a, const auto& b) { return str::icmp(a->name, b->name) < 0; };
		auto less_name = [](const auto& a, const auto& b) { return str::icmp(a.name, b.name) < 0; };
		[[maybe_unused]] auto less_id = [](const auto& a, const auto& b) { return str::icmp(a.name, b.name) < 0; };

		auto contents = platform::iterate_file_items(folder_path, setting.show_hidden);

		if (!contents.success)
		{
			// Enumeration failed (offline volume, denied access, network drop). Rebuilding from
			// the empty listing would erase this branch of the index and expire its database rows.
			add_distinct_other_folders({folder_path});
			return {existing_folder, false, true};
		}

		std::ranges::sort(contents.folders, less_name);
		std::ranges::sort(contents.files, less_name);

		for (auto publication_attempt = 0; publication_attempt < 3; ++publication_attempt)
		{
		existing_snapshot = _items.find_snapshot(folder_path);
		existing_folder = existing_snapshot.folder;
		if (test_after_validate_folder_snapshot) test_after_validate_folder_snapshot(folder_path, publication_attempt);
		changes_detected = false;
		updated_files.clear();
		updated_folders.clear();
		updated_folders.reserve(contents.folders.size());
		updated_files.reserve(contents.files.size());
		removed_folders.clear();
		sidecars.clear();
		sidecar_extensions.clear();
		stale_hashes.clear();
		loaded_from_database.clear();

		auto folder_first = contents.folders.begin();
		const auto folder_last = contents.folders.end();

		if (existing_folder)
		{
			const auto existing_folders = existing_folder->folders_snapshot();
			df::assert_true(std::ranges::is_sorted(contents.folders, less_name));
			df::assert_true(std::ranges::is_sorted(*existing_folders, less_ptr_name));

			if (contents.files.size() != existing_folder->files.size() ||
				contents.folders.size() != existing_folders->size())
			{
				changes_detected = true;
			}

			auto old_first = existing_folders->begin();
			const auto old_last = existing_folders->end();

			while (folder_first != folder_last && old_first != old_last)
			{
				const auto d = icmp(folder_first->name, (*old_first)->name);

				if (d < 0)
				{
					// create: only in new
					updated_folders.emplace_back(
						find_or_create_folder(_items, folder_path.combine(folder_first->name), *folder_first));
					changes_detected = true;
					++folder_first;
				}
				else if (d > 0)
				{
					// remove: only in old
					removed_folders.emplace_back(folder_path.combine((*old_first)->name));
					changes_detected = true;
					++old_first;
				}
				else
				{
					// copy: in both
					auto index_folder_item = find_or_create_folder(_items, folder_path.combine(folder_first->name),
					                                               *folder_first);
					updated_folders.emplace_back(index_folder_item);
					if (*old_first != index_folder_item) changes_detected = true;
					++folder_first;
					++old_first;
				}
			}

			// Folders left only in the old list. The merge above stops as soon as either side runs
			// out, so without this a folder deleted from the end - or the only folder there was -
			// was never recorded as removed, and everything indexed beneath it stayed searchable.
			while (old_first != old_last)
			{
				removed_folders.emplace_back(folder_path.combine((*old_first)->name));
				changes_detected = true;
				++old_first;
			}
		}
		else
		{
			changes_detected = true;
		}

		while (folder_first != folder_last)
		{
			// only in new
			updated_folders.emplace_back(
				find_or_create_folder(_items, folder_path.combine(folder_first->name), *folder_first));
			changes_detected = true;
			++folder_first;
		}

		auto file_first = contents.files.begin();
		const auto file_last = contents.files.end();

		const auto forget_hashes = [&stale_hashes, &folder_path](df::index_file_item& info)
		{
			// Asked of the copy, so a file cleared for two reasons is recorded once.
			if (info.crc32c.load() != 0 || info.phash.load() != nullptr)
			{
				stale_hashes.emplace_back(folder_path.combine_file(info.name));
			}

			info.crc32c = 0;
			info.phash = nullptr;
		};

		if (existing_folder)
		{
			auto old_first = existing_folder->files.begin();
			const auto old_last = existing_folder->files.end();

			while (file_first != file_last && old_first != old_last)
			{
				const auto d = icmp(file_first->name, old_first->name);

				if (d < 0)
				{
					// create: only in new
					df::index_file_item info;
					populate_file_info(info, *file_first, _cache_items_loaded);
					updated_files.emplace_back(info);
					changes_detected = true;
					++file_first;
				}
				else if (d > 0)
				{
					// remove: only in old
					changes_detected = true;
					++old_first;
				}
				else
				{
					if (old_first->file_modified != df::date_t(file_first->attributes.modified))
					{
						changes_detected = true;
					}

					// copy: in both
					df::index_file_item info = *old_first;
					const auto was_loaded_from_database = !old_first->file_modified.load().is_valid();
					info.metadata.store(old_first->metadata);
					info.metadata_scanned = old_first->metadata_scanned.load();
					info.crc32c = old_first->crc32c.load();
					info.phash = old_first->phash.load();

					// The bytes changed, so a hash of the previous bytes is no longer a description of
					// this file. Clearing it is what asks the displayed-item worker to compute it again;
					// carrying it forward would report an edited file and an untouched copy as identical.
					//
					// A node built from the database carries no filesystem stamp at all - db_item_t has
					// neither a modified time nor a size - so for those the question is answered by the
					// scan stamp instead, which is persisted and is the same test needs_scan_impl makes.
					// Reading the absent stamp as a difference wiped every cached checksum on the first
					// validation of every launch; reading it as a match would keep a stale one forever.
					const auto fs_modified = df::date_t(file_first->attributes.modified);
					const auto old_modified = old_first->file_modified.load();

					const auto content_changed = old_modified.is_valid()
						                             ? (old_modified != fs_modified ||
							                             old_first->size.to_int64() != file_first->attributes.size)
						                             : (old_first->metadata_scanned.load() < fs_modified);

					if (content_changed)
					{
						changes_detected = true;
						// The picture hash describes the previous bytes just as the checksum does, and
						// it is persisted - so a hash left behind here reports an edited file and an
						// untouched copy as the same picture for every session that follows.
						forget_hashes(info);
					}

					const auto was_offline = old_first->flags && df::index_item_flags::is_offline;
					populate_file_info(info, *file_first, _cache_items_loaded);
					if (was_loaded_from_database) loaded_from_database.emplace(info.name);
					const auto now_offline = info.flags && df::index_item_flags::is_offline;

					if (was_offline != now_offline)
					{
						// The cloud (offline) status flipped. OneDrive hydration preserves the
						// file's modified time, so this is often the ONLY change -- it must mark the
						// folder dirty, otherwise the updated node (with the cleared is_offline flag)
						// is discarded and the file is never re-scanned online.
						changes_detected = true;
					}

					if (was_offline && !now_offline)
					{
						// A cloud-only placeholder was just hydrated (downloaded). Force a full
						// re-scan so it gets a real thumbnail, content hash and full metadata (tags,
						// camera, etc.) that the offline shell path could not provide.
						info.metadata_scanned = df::date_t{};
						forget_hashes(info);
					}

					updated_files.emplace_back(info);
					++file_first;
					++old_first;
				}
			}
		}

		while (file_first != file_last)
		{
			// only in new
			df::index_file_item info;
			populate_file_info(info, *file_first, _cache_items_loaded);
			updated_files.emplace_back(info);
			changes_detected = true;
			++file_first;
		}

		for (const auto& f : updated_files)
		{
			for (const auto& sc : f.ft->sidecars)
			{
				sidecar_extensions.emplace(sc);
			}
		}

		for (const auto& f : updated_files)
		{
			if (!(f.flags && df::index_item_flags::is_offline) && f.name[0] != '.')
			{
				const auto name = f.name;
				const auto extension_pos = df::find_ext(name);
				auto ext = name.substr(extension_pos);
				if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);

				if (sidecar_extensions.contains(ext))
				{
					const auto path = folder_path.combine_file(name);
					const auto without_extension = path.file_name_without_extension();

					sidecars.emplace(without_extension, name);

					if (str::icmp(ext, "xmp") == 0)
					{
						auto ps = f.metadata.load();

						if (f.metadata_scanned < f.file_modified)
						{
							auto updated = ps
								               ? std::make_shared<prop::item_metadata>(*ps)
								               : std::make_shared<prop::item_metadata>();
							metadata_xmp::parse(*updated, path);
							f.metadata.store(updated);
							ps = std::move(updated);
							f.metadata_scanned = timestamp;
							changes_detected = true;
						}

						const auto raw_file = ps ? ps->raw_file_name : str::cached{};

						if (!is_empty(raw_file))
						{
							sidecars.emplace(raw_file, name);
						}
					}
				}
			}
		}

		for (const auto& file : updated_files)
		{
			const auto name = file.name;
			const auto path = folder_path.combine_file(name);

			if (!file.ft->sidecars.empty())
			{
				const auto without_extension = path.file_name_without_extension();

				std::set<std::string_view> updated_sidecars;
				const auto found_with_extension = sidecars.equal_range(name);

				for (auto it = found_with_extension.first; it != found_with_extension.second; ++it)
				{
					updated_sidecars.emplace(it->second);
				}

				const auto found_without_extension = sidecars.equal_range(without_extension);

				for (auto it = found_without_extension.first; it != found_without_extension.second; ++it)
				{
					updated_sidecars.emplace(it->second);
				}

				const auto ps = file.metadata.load();
				const auto combined = str::combine(updated_sidecars);
				str::cached xmp;

				for (const auto& sc_name : updated_sidecars)
				{
					const auto ext = sc_name.substr(df::find_ext(sc_name));

					if (str::icmp(ext, ".xmp") == 0)
					{
						xmp = str::cache(sc_name);
					}

					const auto found = find_file(updated_files, sc_name);

					if (found != updated_files.end())
					{
						found->flags |= df::index_item_flags::is_sidecar;
					}
				}

				if ((ps && (icmp(ps->sidecars, combined) != 0 || icmp(ps->xmp, xmp) != 0)) ||
					(!ps && (!combined.empty() || !is_empty(xmp))))
				{
					const auto was_loaded_from_database = loaded_from_database.contains(file.name);
					auto updated = ps
						               ? std::make_shared<prop::item_metadata>(*ps)
						               : std::make_shared<prop::item_metadata>();
					updated->sidecars = str::cache(combined);
					updated->xmp = xmp;
					file.metadata.store(std::move(updated));
					if (!was_loaded_from_database) file.metadata_scanned = df::date_t{};
					changes_detected = true;
				}
			}
		}

		for (const auto& f : updated_files)
		{
			f.calc_search_presence();
		}

		if (changes_detected)
		{
			df::assert_true(std::ranges::is_sorted(updated_files, less_id));
			df::assert_true(std::ranges::is_sorted(updated_folders, less_ptr_name));

			auto folder_node = std::make_shared<df::index_folder_item>(std::move(updated_files),
			                                                           std::move(updated_folders));

			if (existing_folder)
			{
				folder_node->name = existing_folder->name;
				folder_node->is_read_only = existing_folder->is_read_only;
				folder_node->is_excluded = existing_folder->is_excluded.load();
				folder_node->is_in_collection = existing_folder->is_in_collection.load();
				folder_node->volume = existing_folder->volume;
				folder_node->created = existing_folder->created;
				folder_node->modified = existing_folder->modified;
			}
			else
			{
				folder_node->name = folder_path.name();
			}

			df::assert_true(!is_empty(folder_node->name));
			folder_node->reset_search_presence();

			// The snapshot this node was built from was read before the enumeration, which holds no
			// lock. Another thread that rebuilt or scanned this folder since then has newer state in
			// the index, and it is in the node rather than in the file items - which were copied,
			// atomics and all, when this pass started. Publishing over it would lose every scan
			// result recorded in that window and send those files back through the scanner.
			auto published = _items.replace_if(folder_path, existing_folder, existing_snapshot.content_revision,
			                                   folder_node);

			// Except a node that holds nothing, which has nothing to lose. When this folder had no
			// node at all, the usual one is the placeholder a validation of the parent creates for it
			// while this pass enumerates - and losing to that answered an empty folder as its
			// contents. It is replaced, keeping what the parent learned about the folder itself.
			if (published != folder_node && published && !existing_folder && published->files.empty() &&
				published->folders_snapshot()->empty())
			{
				folder_node->is_read_only = published->is_read_only;
				folder_node->is_excluded = published->is_excluded.load();
				folder_node->is_in_collection = published->is_in_collection.load();
				folder_node->volume = published->volume;
				folder_node->created = published->created;
				folder_node->modified = published->modified;
				const auto current_snapshot = _items.find_snapshot(folder_path);
				published = current_snapshot.folder == published
					            ? _items.replace_if(folder_path, published, current_snapshot.content_revision,
					                                folder_node)
					            : current_snapshot.folder;
			}

			if (published != folder_node)
			{
				if (published == existing_folder && existing_folder)
				{
					if (publication_attempt + 1 < 3) continue;
					queue_deferred_validation(folder_path, false);
				}
				// Lost the race. Discarding this pass costs only work that will be redone; the
				// removals it found were decided against the same stale view, so they go too.
				add_distinct_other_folders({folder_path});
				return {published, false, false, published == existing_folder && existing_folder};
			}

			_items.erase(removed_folders);

			// The published node no longer holds the changed files' hashes; the rows are cleared to
			// match, or the next launch would load them back as a description of bytes that are gone.
			if (!stale_hashes.empty())
			{
				std::vector<item_db_write> writes;
				writes.reserve(stale_hashes.size());

				for (const auto& path : stale_hashes)
				{
					item_db_write write;
					write.path = path;
					write.clear_hashes = true;
					writes.emplace_back(std::move(write));
				}

				enqueue_db_writes(std::move(writes));
			}

			return {folder_node, changes_detected};
		}
		break;
		}
	}

	add_distinct_other_folders({folder_path});

	return {existing_folder, false};
}


bool needs_scan_impl(const df::index_folder_item_ptr& f, const df::index_file_item& file,
                     const bool thumbnail_needed, const bool scan_if_offline)
{
	// Offline (cloud-only) files are also eligible for scanning: scan_item routes them
	// through the Windows Shell property store, which reads cached metadata WITHOUT
	// hydrating (downloading) the file. scan_if_offline is retained for API stability.
	(void)scan_if_offline;

	if (file.ft->is_media())
	{
		if (thumbnail_needed)
		{
			return true;
		}

		const auto xmp_file_name = file.xmp();

		if (!is_empty(xmp_file_name))
		{
			const auto& xmp_file = find_file(f->files, xmp_file_name);

			if (xmp_file != f->files.end())
			{
				// xmp metadata is scanned during validate folders
				// here return true if scan is newer then parent folder scan
				if (file.metadata_scanned < xmp_file->file_modified)
				{
					return true;
				}
			}
		}

		return file.metadata_scanned < file.file_modified;
	}

	return false;
}


void index_state::scan_uncached(const df::cancel_token& token)
{
	df::scope_locked_inc l(indexing);
	_fully_loaded = false;
	std::vector<df::file_path> uncached;

	size_t items_in_index = 0;

	{
		// create list of uncached
		const auto folders = _items.all_folders();

		for (const auto& folder : folders)
		{
			if (folder.second->is_in_collection)
			{
				for (const auto& file : folder.second->files)
				{
					if (file.ft->is_media())
					{
						items_in_index += 1;

						if (needs_scan_impl(folder.second, file, false, false))
						{
							uncached.emplace_back(folder.first, file.name);
						}
					}
				}
			}
		}
	}

	stats.index_item_count = static_cast<int>(items_in_index);
	stats.index_item_remaining = static_cast<int>(uncached.size());
	const auto publish_progress = [this, total = stats.index_item_count](const int remaining,
	                                                                    const bool invalidate)
	{
		_indexing_progress.store(std::make_shared<const index_progress_snapshot>(
			index_progress_snapshot{total, remaining}));
		if (invalidate) _async.invalidate_view(view_invalid::view_layout);
	};

	publish_progress(stats.index_item_remaining, true);

	// A first index walks the whole collection here, so the database hand-off is grouped: one row at
	// a time woke the database thread and opened a transaction per file.
	db_write_batch writes(*this);
	auto scanned_since_progress = 0;

	for (const auto& id : uncached)
	{
		if (token.is_cancelled()) break;

		const auto f = _items.find(id.folder());

		if (f)
		{
			scan_item(f, id, false, false, false, false, {}, false, files::file_type_from_name(id.name()), false,
			          true, true, &writes);
		}

		--stats.index_item_remaining;
		if (++scanned_since_progress >= 64)
		{
			scanned_since_progress = 0;
			publish_progress(stats.index_item_remaining, true);
		}
	}

	writes.flush();
	stats.index_item_remaining = 0;
	publish_progress(0, false);

	_async.invalidate_view(view_invalid::view_layout | view_invalid::group_layout);
	_fully_loaded = !token.is_cancelled() && _collection_discovery_complete;
}

std::vector<folder_scan_item> index_state::scan_items(const df::index_roots& roots, const bool recursive,
                                                      const bool scan_if_offline, const df::cancel_token& token)
{
	const auto now = platform::now();

	std::vector<folder_scan_item> results;
	std::vector<df::folder_path> folders_to_scan = {roots.folders.begin(), roots.folders.end()};

	auto update_index_summary = false;
	db_write_batch writes(*this);
	df::unique_folders revisited;

	while (!folders_to_scan.empty())
	{
		if (token.is_cancelled()) break;

		const auto folder_path = folders_to_scan.back();
		folders_to_scan.pop_back();

		if (!is_excluded(roots, folder_path))
		{
			const auto node = validate_folder(folder_path, true, now);

			// The node may predate the file system, so it is listed once the rest of the walk is done,
			// by when the writers holding it up have usually moved on. A second deferral lists it as it
			// stands rather than leave it out.
			if (node.deferred && revisited.emplace(folder_path).second)
			{
				folders_to_scan.insert(folders_to_scan.begin(), folder_path);
				continue;
			}

			// Null when enumeration failed for a folder that was never indexed - an offline volume,
			// a dropped share, or a directory that grants write but not list.
			if (!node.folder) continue;

			for (const auto& file : node.folder->files)
			{
				if (token.is_cancelled()) break;
				scan_item(node.folder, folder_path.combine_file(file.name), false, false, false, scan_if_offline, {},
				          false, file.ft, false, true, true, &writes);
				results.emplace_back(folder_path, file);
			}

			// Per folder, so a long walk neither holds the rows nor loses more than one folder's work.
			writes.flush();

			if (recursive)
			{
				// See the comment at the other folders_snapshot() call sites: the returned shared_ptr
				// must outlive the loop, not just the range-for initializer.
				const auto folders_snapshot = node.folder->folders_snapshot();

				for (const auto& sub_folder : *folders_snapshot)
				{
					if (sub_folder->can_recurse) folders_to_scan.emplace_back(folder_path.combine(sub_folder->name));
				}
			}

			update_index_summary = update_index_summary || (node.folder->is_in_collection && node.was_updated);
		}
	}

	for (const auto& file_path : roots.files)
	{
		const auto node = validate_folder(file_path.folder(), true, now);

		if (token.is_cancelled()) break;
		if (!node.folder) continue;
		const auto found_file = find_file(node.folder->files, file_path.name());

		if (found_file != node.folder->files.end())
		{
			scan_item(node.folder, file_path, false, false, false, scan_if_offline, {}, false, found_file->ft, false,
			          true, true, &writes);
			results.emplace_back(file_path.folder(), *found_file);
		}
	}

	if (update_index_summary)
	{
		_async.invalidate_view(view_invalid::index_summary);
	}

	return results;
}

// What the shell's property store says about a file, in the item's terms, with its cached thumbnail
// decoded.
static platform::get_cached_file_properties_response read_cached_file_properties(
	const df::file_path path, prop::item_metadata& properties_out, ui::const_image_ptr& thumbnail_out)
{
	platform::shell_properties shell;
	const auto result = platform::read_shell_properties(path, shell);
	if (result != platform::get_cached_file_properties_response::ok) return result;

	if (shell.album_artist) properties_out.album_artist = str::cache(*shell.album_artist);
	if (shell.artist) properties_out.artist = str::cache(*shell.artist);
	if (shell.title) properties_out.title = str::cache(*shell.title);
	if (shell.genre) properties_out.genre = str::cache(*shell.genre);
	if (shell.album) properties_out.album = str::cache(*shell.album);
	if (shell.comment) properties_out.comment = str::cache(*shell.comment);
	if (shell.year) properties_out.year = static_cast<uint16_t>(*shell.year);
	if (shell.track) properties_out.track.x = static_cast<int16_t>(*shell.track);
	if (shell.width) properties_out.width = static_cast<uint16_t>(std::min<uint32_t>(*shell.width, 0xffff));
	if (shell.height) properties_out.height = static_cast<uint16_t>(std::min<uint32_t>(*shell.height, 0xffff));
	if (shell.orientation) properties_out.orientation = static_cast<ui::orientation>(*shell.orientation);
	if (shell.date_taken) properties_out.dates.add_utc(prop::date_source::shell_date_taken, df::date_t(*shell.date_taken));

	// Joined into Diffractor's space-separated (quoted) tag form.
	if (!shell.keywords.empty() && is_empty(properties_out.tags))
	{
		properties_out.tags = str::cache(str::combine(shell.keywords));
	}

	if (shell.latitude && shell.longitude) properties_out.coordinate = gps_coordinate(*shell.latitude, *shell.longitude);
	if (!shell.thumbnail.empty()) thumbnail_out = load_image_file(shell.thumbnail);
	return result;
}

// The shell's thumbnail, made into an item thumbnail. Pixels that make no thumbnail are a fail.
static platform::get_cached_file_properties_response read_shell_thumbnail(
	const df::file_path path, const sizei requested_extent, const bool allow_network, ui::const_image_ptr& thumbnail_out)
{
	platform::bgra_pixels pixels;
	const auto response = platform::get_shell_thumbnail(path, requested_extent, allow_network, pixels);
	if (response != platform::get_cached_file_properties_response::ok) return response;

	const auto surface = std::make_shared<ui::surface>();

	if (!surface->alloc(static_cast<int>(pixels.width), static_cast<int>(pixels.height), ui::texture_format::ARGB,
	                    ui::orientation::top_left))
	{
		return platform::get_cached_file_properties_response::fail;
	}

	const auto row_bytes = static_cast<size_t>(pixels.width) * 4u;

	for (uint32_t y = 0; y < pixels.height; ++y)
	{
		memcpy(surface->pixels() + static_cast<size_t>(y) * static_cast<size_t>(surface->stride()),
		       pixels.pixels.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
	}

	files ff;
	auto image = ff.surface_to_thumbnail(surface);
	if (!is_valid(image)) return platform::get_cached_file_properties_response::fail;

	thumbnail_out = std::move(image);
	return platform::get_cached_file_properties_response::ok;
}

void index_state::scan_offline_item(const df::index_folder_item_ptr& folder,
                                    const df::file_path file_path,
                                    const bool thumbnail_needed,
                                    const std::weak_ptr<df::item_element>& item,
                                    const bool publish_to_item,
                                    const df::index_file_item& file,
                                    const df::date_t now,
                                    const bool invalidate_summary,
                                    const uint64_t thumbnail_generation)
{
	// Cloud-only placeholder (OneDrive Files On-Demand, GVFS, etc.). Read cached metadata
	// (and, when the shell has one, a cached thumbnail) via the Windows Shell property
	// store WITHOUT hydrating (downloading) the file. Verified empirically that this does
	// not trigger a download for online-only files. We never compute a content hash
	// (crc32c) for these items, so they are excluded from hash-based duplicate matching.
	// Full hydration only happens when the user explicitly opens/accesses the file.
	const auto want_thumb = thumbnail_needed && publish_to_item;

	auto metadata = std::make_shared<prop::item_metadata>();
	ui::const_image_ptr shell_thumbnail;
	const auto resp = read_cached_file_properties(file_path, *metadata, shell_thumbnail);

	item_db_write write;
	write.path = file_path;
	write.metadata_scanned = now;
	auto in_collection = false;
	if (resp == platform::get_cached_file_properties_response::ok)
	{
		df::scope_locked_inc l(scanning_items);

		metadata->file_name = file_path.name();

		ui::const_image_ptr thumbnail_image;

		if (is_valid(shell_thumbnail))
		{
			files ff;

			thumbnail_image = shell_thumbnail;
			const auto max_extent = setting.thumbnail_max_dimension;
			const auto thumb_extent = thumbnail_image->dimensions();

			if (max_extent.cx < thumb_extent.cx || max_extent.cy < thumb_extent.cy)
			{
				const auto surf = ff.image_to_surface(thumbnail_image, max_extent, false, {},
				                                      decode_intent::thumbnail);
				thumbnail_image = ff.surface_to_thumbnail(surf);
			}

			if (is_valid(thumbnail_image) && thumbnail_image->data().size() < df::two_fifty_six_k)
			{
				write.thumb = thumbnail_image;
				write.thumb_scanned = file.file_modified;
			}
			else
			{
				thumbnail_image.reset();
			}
		}
		else if (want_thumb)
		{
			// Metadata came back but no cached thumbnail is available offline.
			publish_thumbnail_failure(item, file_path, thumbnail_generation);
		}

		const auto published = _items.update_file(file_path, [&](const df::index_folder_item_ptr& current_folder,
		                                                         const df::index_file_item& current_file)
		{
			write.modified = current_file.file_modified;
			const auto existing_metadata = current_file.metadata.load();

			if (existing_metadata)
			{
				metadata->sidecars = existing_metadata->sidecars;
				metadata->xmp = existing_metadata->xmp;
				metadata->media_position = existing_metadata->media_position;
			}

			current_file.metadata_scanned = now;
			current_file.metadata.store(metadata);
			write.md = metadata;
			current_file.calc_search_presence();
			current_folder->update_search_presence(current_file);
			in_collection = current_folder->is_in_collection.load();
			return true;
		});

		if (!published) return;

		if (publish_to_item && is_valid(thumbnail_image))
		{
			publish_thumbnail(item, file_path, thumbnail_image, {}, write.modified.value_or(df::date_t{}),
			                  thumbnail_generation, false, true);
		}
	}
	else
	{
		// Shell had no cached data. Still persist a properties row (with metadata_scanned)
		// so we do not re-scan this file on every startup. We write the (empty) metadata via
		// write.md so perform_writes performs an insert-or-replace that creates the row -- a
		// bare metadata_scanned update would affect zero rows when no row exists yet.
		metadata->file_name = file_path.name();
		const auto published = _items.update_file(file_path, [&](const df::index_folder_item_ptr& current_folder,
		                                                         const df::index_file_item& current_file)
		{
			write.modified = current_file.file_modified;
			const auto existing_metadata = current_file.metadata.load();
			if (existing_metadata)
			{
				metadata->sidecars = existing_metadata->sidecars;
				metadata->xmp = existing_metadata->xmp;
				metadata->media_position = existing_metadata->media_position;
			}
			current_file.metadata_scanned = now;
			current_file.metadata.store(metadata);
			write.md = metadata;
			current_file.calc_search_presence();
			current_folder->update_search_presence(current_file);
			in_collection = current_folder->is_in_collection.load();
			return true;
		});

		if (!published) return;

		if (want_thumb)
		{
			publish_thumbnail_failure(item, file_path, thumbnail_generation);
		}
	}

	enqueue_db_write(std::move(write));

	if (invalidate_summary && in_collection)
	{
		_async.invalidate_view(view_invalid::index_summary);
	}
}

void index_state::apply_scan_result(const df::index_folder_item_ptr& folder,
                                    const df::file_path file_path,
                                    const file_scan_result& sr,
                                    const df::date_t now,
                                    const df::date_t thumbnail_version,
                                    const bool load_thumb,
                                    const bool thumbnail_needed,
                                    const bool had_thumbnail,
                                    const std::weak_ptr<df::item_element>& item,
                                    const bool publish_to_item,
                                    const bool publish_item_update_immediately,
                                    const bool invalidate_summary,
                                    db_write_batch* writes,
                                    const bool clear_existing_hashes,
                                    const uint64_t thumbnail_generation)
{
	const auto queue_write = [this, writes](item_db_write w)
	{
		if (writes) writes->add(std::move(w));
		else enqueue_db_write(std::move(w));
	};

	const auto found_file = find_file(folder->files, file_path.name());

	if (found_file == folder->files.end())
	{
		return;
	}

	files ff;

	if (sr.success)
	{
		df::scope_locked_inc l(scanning_items);
		const auto* const mt = files::file_type_from_name(file_path);
		const auto metadata = sr.to_props();
		const auto thumbnail_was_loaded = is_valid(sr.thumbnail_surface) ||
			is_valid(sr.thumbnail_image);

		if (mt->has_trait(file_traits::video_metadata))
		{
			const auto name_props = scan_info_from_title(file_path.file_name_without_extension());

			if (is_empty(metadata->show) && !str::is_empty(name_props.show))
				metadata->show = str::cache(
					name_props.show);
			if (is_empty(metadata->title) && !str::is_empty(name_props.title))
				metadata->title = str::cache(
					name_props.title);
			if (metadata->year == 0 && name_props.year != 0) metadata->year = name_props.year;
			if (metadata->episode == df::xy8::make(0, 0) && name_props.episode != 0)
				metadata->episode =
					df::xy8::make(name_props.episode, name_props.episode_of);
			if (metadata->season == 0 && name_props.season != 0) metadata->season = name_props.season;
		}

		item_db_write write;
		write.path = file_path;
		write.md = metadata;
		write.metadata_scanned = now;
		write.clear_hashes = clear_existing_hashes;

		ui::const_image_ptr cover_art;
		ui::const_image_ptr thumbnail_image;
		ui::const_surface_ptr thumbnail_surface;

		// Indexing is metadata only. A thumbnail produced here was provisional anyway - it is stored
		// with no scan timestamp, so the visible-item pass replaced it on first display - and paying
		// to decode, scale and re-encode every item in the collection to get one made indexing far
		// and away the most expensive thing the application does. Visuals are acquired on demand.
		if (load_thumb && is_valid(sr.cover_art))
		{
			cover_art = sr.cover_art;
			const auto max_extent = setting.thumbnail_max_dimension;
			const auto cover_art_extent = cover_art->dimensions();

			// The bytes come straight out of an arbitrary container's attached-picture stream, so the
			// size is chosen by the file rather than by us: a small image padded to megabytes passes a
			// test on dimensions alone. assert_true evaluates nothing in Release, so the ceiling has to
			// be a gate or the index stores whatever it was handed.
			if (max_extent.cx < cover_art_extent.cx || max_extent.cy < cover_art_extent.cy ||
				cover_art->data().size() >= df::two_fifty_six_k)
			{
				auto surf = ff.image_to_surface(cover_art, max_extent, false, {}, decode_intent::thumbnail);
				cover_art = ff.surface_to_thumbnail(surf);
			}

			if (is_valid(cover_art) && cover_art->data().size() < df::two_fifty_six_k)
			{
				write.cover_art = cover_art;
			}
			else
			{
				// A re-encode that is still over the ceiling is dropped rather than kept: the local is
				// what publish_thumbnail charges against the in-memory budget, so gating only the
				// database write would keep the oversized blob in the item.
				if (is_valid(cover_art)) df::log(__FUNCTION__, "cover art over the thumbnail ceiling");
				cover_art.reset();
			}
		}

		if (load_thumb && is_valid(sr.thumbnail_surface))
		{
			// fit_within is the ceiling test: a surface already inside it comes back untouched, so
			// there is nothing here to decide.
			auto surf = ff.fit_within(sr.thumbnail_surface, setting.thumbnail_max_dimension);
			thumbnail_image = ff.surface_to_thumbnail(surf);
			thumbnail_surface = surf;

			// Same ceiling as the two above, and a real gate for the same reason: assert_true evaluates
			// nothing in Release, so an encode that came back over the ceiling would reach both the
			// database row and the in-memory budget unchallenged.
			if (is_valid(thumbnail_image) && thumbnail_image->data().size() < df::two_fifty_six_k)
			{
				write.thumb = thumbnail_image;
			}
			else
			{
				if (is_valid(thumbnail_image)) df::log(__FUNCTION__, "thumbnail over the ceiling");
				thumbnail_image.reset();
				thumbnail_surface.reset();
			}
		}
		else if (load_thumb && is_valid(sr.thumbnail_image))
		{
			thumbnail_image = sr.thumbnail_image;

			if (is_valid(thumbnail_image))
			{
				const auto max_extent = setting.thumbnail_max_dimension;
				const auto thumb_extent = thumbnail_image->dimensions();

				// Same rule as the cover art above: these bytes are an embedded thumbnail stored verbatim
				// by the camera, so the size is the file's choice and a small image padded to megabytes
				// passes a test on dimensions alone.
				if (max_extent.cx < thumb_extent.cx || max_extent.cy < thumb_extent.cy ||
					thumbnail_image->data().size() >= df::two_fifty_six_k)
				{
					auto surf = ff.image_to_surface(thumbnail_image, max_extent, false, {},
					                                decode_intent::thumbnail);
					thumbnail_image = ff.surface_to_thumbnail(surf);
					thumbnail_surface = surf;
				}

				if (is_valid(thumbnail_image) && thumbnail_image->data().size() < df::two_fifty_six_k)
				{
					write.thumb = thumbnail_image;
				}
				else
				{
					if (is_valid(thumbnail_image)) df::log(__FUNCTION__, "thumbnail over the ceiling");
					thumbnail_image.reset();
				}
			}
			else
			{
				// Thumbnail decode produced an invalid image (corrupt file, unsupported format, etc.)
				thumbnail_image.reset();

				if (publish_to_item)
				{
					publish_thumbnail_failure(item, file_path, thumbnail_generation);
				}
			}
		}
		else if (load_thumb)
		{
			if (publish_to_item)
			{
				publish_thumbnail_failure(item, file_path, thumbnail_generation);
			}
		}

		if (sr.crc32c)
		{
			write.crc32c = sr.crc32c;
		}

		if (load_thumb && thumbnail_was_loaded)
		{
			write.thumb_scanned = thumbnail_version;
		}

		auto in_collection = false;
		const auto published = _items.update_file(file_path, [&](const df::index_folder_item_ptr& current_folder,
		                                                         const df::index_file_item& current_file)
		{
			if (clear_existing_hashes)
			{
				current_file.crc32c = 0;
				current_file.phash = nullptr;
			}

			const auto existing_metadata = current_file.metadata.load();

			if (existing_metadata && metadata)
			{
				metadata->sidecars = existing_metadata->sidecars;
				metadata->xmp = existing_metadata->xmp;
				// Playback position lives only in the index and database, so a rescan that
				// did not read it must carry it or the resume point is lost.
				metadata->media_position = existing_metadata->media_position;
			}

			metadata->file_name = file_path.name();
			current_file.metadata_scanned = now;
			current_file.metadata.store(metadata);

			// Same guard as the database write above: scan_file only computes a CRC when it read the whole
			// file, so an ordinary thumbnail scan of a video answers zero. Storing that would clear a value
			// the database still holds and silently drop the item out of duplicate detection.
			if (sr.crc32c)
			{
				current_file.crc32c = sr.crc32c;
			}

			// search_presence is a hard rejection filter, so it must be refreshed with every
			// published metadata snapshot or a newly matching item stays invisible to search
			current_file.calc_search_presence();
			current_folder->update_search_presence(current_file);
			write.modified = current_file.file_modified;
			in_collection = current_folder->is_in_collection.load();
			return true;
		});

		if (!published) return;

		// For the immediate post-edit scan, metadata_scanned is stamped with the file's own
		// (handle-read) modified time. That makes needs_scan_impl (metadata_scanned <
		// file_modified) false both now - the cached file_modified is the older pre-edit mtime -
		// and after the background validate_folder refreshes file_modified to the same post-edit
		// mtime, so the queued background rescan is a no-op and never reopens the file BY NAME
		// (which could read stale SMB-cached bytes).

		if (publish_to_item)
		{
			if ((thumbnail_was_loaded || !had_thumbnail) && (is_valid(thumbnail_image) || is_valid(
				cover_art)))
			{
				df::assert_true(
					!is_valid(thumbnail_image) || thumbnail_image->data().size() < df::two_fifty_six_k);
				df::assert_true(!is_valid(cover_art) || cover_art->data().size() < df::two_fifty_six_k);

				// Stage as soon as the item that asked for this thumbnail has it: an unstaged
				// thumbnail cannot draw, so deferring to the end of the batch made a whole
				// screenful appear at once instead of filling in as each one decoded.
				publish_thumbnail(item, file_path, thumbnail_image, cover_art,
				                  thumbnail_was_loaded ? thumbnail_version : df::date_t::null,
				                  thumbnail_generation,
				                  thumbnail_was_loaded,
				                  thumbnail_needed || publish_item_update_immediately);
			}
		}

		queue_write(std::move(write));

		if (invalidate_summary && in_collection)
		{
			_async.invalidate_view(view_invalid::index_summary);
		}
	}
	else
	{
		item_db_write write;
		write.path = file_path;
		// Stamped with the file's own modified time, not with now. Nothing was read, so a stamp in
		// the present would record this version as examined at a moment it never was - and any
		// later touch of the file that lands before that moment would then read as already scanned.
		// Against the file's own time the record says which version was attempted, and a file that
		// changes at all re-opens the question.
		const auto published = _items.update_file(file_path, [&](const df::index_folder_item_ptr&,
		                                                         const df::index_file_item& current_file)
		{
			write.metadata_scanned = current_file.file_modified.load();
			write.modified = current_file.file_modified;
			return true;
		});
		if (!published) return;
		queue_write(std::move(write));

		if (load_thumb && publish_to_item)
		{
			publish_thumbnail_failure(item, file_path, thumbnail_generation);
		}
	}
}

void index_state::scan_item(const df::index_folder_item_ptr& folder,
                            const df::file_path file_path,
                            const bool load_thumb,
                            const bool thumbnail_needed,
                            const bool had_thumbnail,
                            const bool scan_if_offline,
                            const std::weak_ptr<df::item_element>& item,
                            const bool publish_to_item,
                            const file_type_ref ft,
                            const bool force,
                            const bool publish_item_update_immediately,
                            const bool invalidate_summary,
                            db_write_batch* writes,
                            const uint64_t thumbnail_generation)
{
	const auto now = platform::now();
	const auto found_file = find_file(folder->files, file_path.name());

	if (found_file != folder->files.end())
	{
		const auto& file = *found_file;
		const auto thumbnail_version = file.file_modified.load();

		if (force || needs_scan_impl(folder, file, thumbnail_needed, scan_if_offline))
		{
			if (!crash_files().is_known_crash_file(file_path))
			{
				df::assert_true(ft->is_media());

				record_open_path record(crash_files(), file_path, str::utf8_cast(__FUNCTION__));

				if (file.flags && df::index_item_flags::is_offline)
				{
					// Cloud-only placeholder: never hydrate during indexing. Read cached
					// shell metadata/thumbnail instead. See scan_offline_item().
					scan_offline_item(folder, file_path, thumbnail_needed, item, publish_to_item, *found_file, now,
					                  invalidate_summary, thumbnail_generation);
				}
				else
				{
					files ff;
					const auto sr = ff.scan_file(file_path, load_thumb, ft, file.xmp(),
					                             setting.thumbnail_max_dimension);

					apply_scan_result(folder, file_path, sr, now, thumbnail_version, load_thumb, thumbnail_needed,
					                  had_thumbnail, item, publish_to_item, publish_item_update_immediately,
					                  invalidate_summary, writes, false, thumbnail_generation);
				}
			}
			else if (load_thumb && publish_to_item)
			{
				// Skipping silently would leave the tile blank, which reads as still loading rather
				// than as a file the app refuses to open. See docs/design.md system states.
				publish_thumbnail_failure(item, file_path, thumbnail_generation);
			}
		}

		if (publish_to_item && publish_item_update_immediately)
		{
			publish_item_update(item, file_path);
		}
	}
}

void index_state::scan_item(const df::item_element_ptr& i, const bool load_thumb, const bool scan_if_offline)
{
	const auto node = validate_folder(i->folder(), true, platform::now());

	// validate_folder answers null for a folder never indexed that cannot now be enumerated - an
	// ejected card, a dropped share, a directory that grants write but not list.
	if (!node.folder) return;

	scan_item(node.folder, i->path(), load_thumb, load_thumb && i->should_load_thumbnail(), i->has_thumb(),
	          scan_if_offline, i, true, i->file_type());
}

rescan_spec index_state::make_rescan_spec(const item_scan_request& request, const std::string_view xmp_sidecar,
                                          const bool want_image, const bool want_handle)
{
	rescan_spec spec;
	spec.wanted = true;
	spec.load_thumbnail = request.load_thumbnail;
	spec.want_image = want_image;
	spec.want_handle = want_handle;
	spec.file_type = request.file_type;
	spec.xmp_sidecar = xmp_sidecar;
	spec.max_thumb_size = setting.thumbnail_max_dimension;
	return spec;
}

void index_state::apply_scan_now(const item_scan_request& request, const file_scan_result& sr, const bool coherent,
                                 const df::date_t known_modified)
{
	// Reuse the cached folder node - do NOT refresh from the filesystem here. A by-name refresh is
	// exactly the stale-read window we are avoiding; the scan already carries the authoritative
	// post-edit content.
	const auto node = validate_folder(request.folder, false, platform::now());
	if (!node.folder) return;
	const auto found_file = find_file(node.folder->files, request.path.name());

	if (found_file == node.folder->files.end())
	{
		return;
	}

	const auto revision_changed = coherent && found_file->file_modified.load() != known_modified;

	// A coherent scan came through the post-swap handle, so stamping both times from the file's own
	// modified time makes the later background rescan a no-op. The record's own modified time has to
	// advance with them: leaving it at the pre-edit value makes thumbnail_timestamp != file_modified,
	// which is what should_load_thumbnail() tests, so the "no-op" background rescan would re-read and
	// re-decode the file we just wrote.
	if (coherent)
	{
		const auto updated = _items.update_file(request.path, [known_modified](const df::index_folder_item_ptr&,
		                                                                       const df::index_file_item& file)
		{
			file.file_modified = known_modified;
			return true;
		});
		if (!updated) return;
	}

	const auto now = coherent ? known_modified : platform::now();
	const auto thumbnail_version = coherent ? known_modified : found_file->file_modified.load();

	apply_scan_result(node.folder, request.path, sr, now, thumbnail_version, request.load_thumbnail,
	                  request.thumbnail_needed, request.had_thumbnail, request.lifetime, true, true, true, nullptr,
	                  revision_changed, request.thumbnail_generation);

	publish_item_update(request.lifetime, request.path);
}

bool index_state::apply_write_scan(const item_scan_request& request, const file_update_result& result)
{
	if (!result.success() || !result.scanned)
	{
		return true;
	}

	apply_scan_now(request, result.scan, result.coherent, df::date_t(result.modified));
	return false;
}

bool index_state::needs_scan(const df::item_element_ptr& item) const
{
	const auto id = item->path();
	const auto found_folder = _items.find(id.folder());

	if (found_folder)
	{
		const auto found_file = find_file(found_folder->files, id.name());

		if (found_file != found_folder->files.end())
		{
			return needs_scan_impl(found_folder, *found_file, false, false);
		}
	}

	return true;
}

df::index_file_item index_state::find_item(const df::file_path id) const
{
	const auto found_folder = _items.find(id.folder());

	if (found_folder)
	{
		const auto found_file = find_file(found_folder->files, id.name());

		if (found_file != found_folder->files.end())
		{
			return *found_file;
		}
	}

	return {};
}


inline bool index_state::is_collection_search(const df::search_t& search) const
{
	for (const auto& sel : search.selectors())
	{
		if (!is_in_collection(sel.folder()))
		{
			return false;
		}
	}

	return true;
}

void index_state::save_media_position(const df::file_path id, const double media_position)
{
	// The index copy is what a rescan carries forward and what a new item element resumes from.
	// Saving only the row left that copy at the position loaded at startup, and the next rescan of the
	// file wrote it back over the one just saved. Published like any metadata: cloned and finished
	// outside the lock, then swapped in only over the snapshot it was cloned from.
	for (auto attempt = 0; attempt < 3; ++attempt)
	{
		prop::item_metadata_ptr current;
		_items.update_file(id, [&current](const df::index_folder_item_ptr&, const df::index_file_item& file)
		{
			current = file.metadata.load();
			return false;
		});

		if (!current || df::equiv(current->media_position, media_position)) break;

		auto updated = std::make_shared<prop::item_metadata>(*current);
		updated->media_position = media_position;

		auto published = false;
		_items.update_file(id, [&](const df::index_folder_item_ptr&, const df::index_file_item& file)
		{
			auto expected = current;
			published = file.metadata.compare_exchange_strong(expected, updated);
			return published;
		});

		if (published) break;
	}

	item_db_write write;
	write.path = id;
	write.media_position = media_position;
	enqueue_db_write(std::move(write));
}

void index_state::save_crc(const df::file_path id, const index_file_revision revision, const uint32_t crc)
{
	_async.queue_async(async_queue::work, [this, id, revision, crc]
	{
		const auto published = _items.update_file(id, [&](const df::index_folder_item_ptr& folder,
		                                                  const df::index_file_item& file)
		{
			// The checksum describes the bytes that were read. A file replaced while it was being read
			// is a different file at the same name, so the checksum is dropped rather than attached to
			// content it does not describe.
			if (!(revision_of(file) == revision)) return false;

			file.crc32c = crc;
			file.calc_search_presence();
			folder->update_search_presence(file);
			return true;
		});
		if (!published) return;

		item_db_write write;
		write.path = id;
		write.crc32c = crc;
		enqueue_db_write(std::move(write));
	});
}

void index_state::save_phash(const df::file_path id, const index_file_revision revision,
                             const crypto::phash_rotations& phash)
{
	std::vector<phash_result> one;
	one.emplace_back(id, revision, phash);
	save_phashes(std::move(one));
}

void index_state::save_phashes(std::vector<phash_result> hashes, const bool continue_predictions)
{
	if (hashes.empty() && !continue_predictions) return;

	// Verified and published on one hop, and the database row written only for what survived: a
	// write enqueued ahead of the check would persist a hash the index had already refused.
	_async.queue_async(async_queue::work, [this, hashes = std::move(hashes), continue_predictions]
	{
		std::vector<item_db_write> writes;
		writes.reserve(hashes.size());

		for (const auto& result : hashes)
		{
			const auto picture_hashes = df::make_picture_hashes(result.rotations);
			const auto found = _items.update_file(result.path, [&](const df::index_folder_item_ptr&,
			                                                       const df::index_file_item& file)
			{
				if (!(revision_of(file) == result.revision)) return false;
				// Published as complete sets, so the walk never sees a picture with some
				// orientations filled in.
				file.phash = picture_hashes;
				return true;
			});

			if (found)
			{
				item_db_write write;
				write.path = result.path;
				write.phash = result.rotations;
				writes.emplace_back(std::move(write));
			}
			else
			{
				// The database write is an update keyed on an existing row, so a path the collection
				// no longer holds at this revision keeps no hash and is decoded again when asked.
				df::bump(df::index_perf.phash_unpersisted);
			}
		}

		enqueue_db_writes(std::move(writes));

		if (continue_predictions && !df::is_closing)
		{
			queue_update_predictions();
		}
	});
}

void index_state::save_thumbnail(const df::file_path id, const ui::const_image_ptr& thumbnail_image,
                                 const ui::const_image_ptr& cover_art, const df::date_t scan_timestamp)
{
	item_db_write write;
	write.path = id;

	if (is_valid(thumbnail_image))
	{
		write.thumb = thumbnail_image;
	}

	if (is_valid(cover_art))
	{
		write.cover_art = cover_art;
	}

	write.thumb_scanned = scan_timestamp;
	enqueue_db_write(std::move(write));
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

void index_state::index_roots(df::index_roots roots)
{
	_fully_loaded = false;
	_collection_discovery_complete = false;
	{
		platform::exclusive_lock lock(_summary_rw);
		std::swap(_summary._roots, roots);
	}
}

void index_state::index_folders(df::cancel_token token, const bool scan_collection)
{
	if (!scan_collection)
	{
		_fully_loaded = false;
		_collection_discovery_complete = false;
		_folders_indexed = true;
		return;
	}

	df::scope_locked_inc l(detecting);
	df::index_roots roots;

	{
		platform::shared_lock lock(_summary_rw);
		roots = _summary._roots;
	}

	const auto now = platform::now();
	std::vector<df::folder_path> folders(roots.folders.begin(), roots.folders.end());
	df::unique_folders unique_folder_paths(folders.cbegin(), folders.cend());

	index_histograms histograms;
	int count = 0;
	int picture_count = 0;
	stats.index_folder_count = 0;
	auto next_histogram_publish_ms = df::now_ms();
	auto exhausted = unique_folder_paths.size() > max_folders_to_index;
	std::vector<df::folder_path> failed_enumerations;
	df::unique_folders revisited;
	// Folders whose files the walk has recorded. A node republished since then has lost the walk's
	// mark, and the fix-up below must not record its files a second time.
	df::unique_folders counted;

	for (const auto& f : _items.all_folders())
	{
		f.second->is_in_collection = false;
		f.second->is_excluded = false;
	}

	while (!folders.empty() && !token.is_cancelled())
	{
		if (token.is_cancelled())
		{
			break;
		}

		auto folder_path = folders.back();
		folders.pop_back();

		if (!is_excluded(roots, folder_path))
		{
			const auto node = validate_folder(folder_path, true, now);

			// The node may predate the file system - a database-loaded one lists no subfolders yet - so
			// the folder is walked again once the rest of the collection has been. A second deferral
			// walks the node as it stands, and the subtree it may not list is kept like a failed
			// enumeration's rather than swept.
			if (node.deferred && revisited.emplace(folder_path).second)
			{
				folders.insert(folders.begin(), folder_path);
				continue;
			}

			if (node.enumeration_failed || node.deferred) failed_enumerations.emplace_back(folder_path);
			if (!node.folder) continue;
			node.folder->is_in_collection = true;

			for (const auto& file : node.folder->files)
			{
				histograms.record(_locations, file, df::file_path(folder_path, file.name));

				if (file.ft->is_media())
				{
					count += 1;
				}
				if (file.ft->has_trait(file_traits::bitmap)) ++picture_count;
			}

			counted.emplace(folder_path);

			// See the comment at the other folders_snapshot() call sites: the returned shared_ptr must
			// outlive the loop, not just the range-for initializer.
			const auto folders_snapshot = node.folder->folders_snapshot();

			for (const auto& sub_folder : *folders_snapshot)
			{
				const auto sub_folder_path = folder_path.combine(sub_folder->name);
				const auto is_excluded = df::is_excluded(roots, sub_folder_path);

				if (!unique_folder_paths.contains(sub_folder_path) &&
					!is_excluded)
				{
					if (unique_folder_paths.size() < max_folders_to_index)
					{
						folders.emplace_back(sub_folder_path);
						unique_folder_paths.emplace(sub_folder_path);
						++stats.index_folder_count;
					}
					else
					{
						exhausted = true;
					}
				}

				sub_folder->is_excluded = is_excluded;
			}
		}

		// Histogram snapshots are large; rate-limit progress copies while always publishing the final state below.
		const auto current_ms = df::now_ms();
		if (current_ms >= next_histogram_publish_ms)
		{
			index_histograms_const_ptr histogram_snapshot = std::make_shared<index_histograms>(histograms);
			{
				platform::exclusive_lock lock(_summary_rw);
				_summary._histograms.swap(histogram_snapshot);
			}
			_async.invalidate_view(view_invalid::sidebar_file_types_and_dates | view_invalid::tooltip);
			next_histogram_publish_ms = current_ms + 100;
		}
	}

	const auto containing_root = [&roots](const df::folder_path folder) -> std::optional<df::folder_path>
	{
		std::optional<df::folder_path> result;

		for (const auto root : roots.folders)
		{
			if (!df::folder_contains(root.text().sv(), folder.text().sv())) continue;
			if (!result || result->text().size() < root.text().size()) result = root;
		}

		return result;
	};

	const auto has_excluded_ancestor = [&roots](const df::folder_path root, df::folder_path folder)
	{
		for (;;)
		{
			if (df::is_excluded(roots, folder)) return true;
			if (folder == root || folder.is_root()) return false;
			folder = folder.parent();
		}
	};

	for (const auto& cached : _items.all_folders())
	{
		if (cached.second->is_in_collection) continue;

		const auto under_failed_enumeration = std::ranges::any_of(failed_enumerations, [&cached](
			const df::folder_path failed)
		{
			return df::folder_contains(failed.text().sv(), cached.first.text().sv());
		});

		if (!under_failed_enumeration) continue;

		const auto root = containing_root(cached.first);
		if (!root || has_excluded_ancestor(*root, cached.first)) continue;

		cached.second->is_in_collection = true;
		unique_folder_paths.emplace(cached.first);
		if (counted.contains(cached.first)) continue;

		for (const auto& file : cached.second->files)
		{
			histograms.record(_locations, file, df::file_path(cached.first, file.name));

			if (file.ft->is_media())
			{
				count += 1;
			}
			if (file.ft->has_trait(file_traits::bitmap)) ++picture_count;
		}
	}

	const auto discovery_complete = !token.is_cancelled() && !exhausted;
	_collection_discovery_complete = discovery_complete;

	if (discovery_complete)
	{
		std::vector<df::folder_path> stale_cached_folders;

		for (const auto& cached : _items.all_folders())
		{
			const auto root = containing_root(cached.first);
			if (!root) continue;
			if (unique_folder_paths.contains(cached.first)) continue;
			if (has_excluded_ancestor(*root, cached.first)) continue;

			const auto under_failed_enumeration = std::ranges::any_of(failed_enumerations, [&cached](
				const df::folder_path failed)
			{
				return df::folder_contains(failed.text().sv(), cached.first.text().sv());
			});

			if (!under_failed_enumeration) stale_cached_folders.emplace_back(cached.first);
		}

		_items.erase(stale_cached_folders);

		stats.index_item_count = stats.media_item_count = count;
		stats.picture_item_count = picture_count;

		_async.queue_database([cached_items = all_indexed_items()](const database& db)
		{
			db.clean(cached_items);
		});
	}

	{
		stats.index_folder_count = static_cast<int>(unique_folder_paths.size());
		std::shared_ptr<const df::unique_folders> folders_snapshot =
			std::make_shared<df::unique_folders>(std::move(unique_folder_paths));
		platform::exclusive_lock lock(_summary_rw);
		_summary._distinct_folders.swap(folders_snapshot);
	}

	df::unique_folders distinct_prime_folders;

	for (const auto& r : roots.folders)
	{
		distinct_prime_folders.emplace(r);
	}

	for (const auto& d : platform::drives())
	{
		distinct_prime_folders.emplace(df::folder_path(d.name));
	}

	std::shared_ptr<const df::unique_folders> prime_folders_snapshot =
		std::make_shared<df::unique_folders>(std::move(distinct_prime_folders));
	index_histograms_const_ptr histogram_snapshot = std::make_shared<index_histograms>(std::move(histograms));

	{
		platform::exclusive_lock lock(_summary_rw);
		_summary._distinct_prime_folders.swap(prime_folders_snapshot);
		_summary._histograms.swap(histogram_snapshot);
	}

	_folders_indexed = true;
}


static void scan_trim(std::string& s)
{
	const auto pred = [](const int ch)
	{
		return !std::iswspace(ch) && ch != '-';
	};

	s.erase(s.begin(), std::ranges::find_if(s, pred));
	s.erase(std::find_if(s.rbegin(), s.rend(), pred).base(), s.end());
}

std::string build_result_string(const std::vector<std::string>& tokens,
                                const std::vector<std::string>::const_iterator& begin,
                                const std::vector<std::string>::const_iterator& end)
{
	std::string result;
	bool brackets = false;
	auto i = begin;

	while (i < end)
	{
		if (*i == "[")
		{
			brackets = true;
		}
		else if (brackets)
		{
			brackets = *i != "]";
		}
		else
		{
			if (!result.empty()) result += " ";
			result += *i;
		}

		++i;
	}

	return std::string(str::utf8_cast(result));
}

media_name_props scan_info_from_title(const std::string_view name8)
{
	media_name_props result;

	static const df::hash_set<std::string_view, df::ihash, df::ieq> stop_words
	{
		"480p",
		"720p",
		"720",
		"1080p",
		"1080",
		"1440p",
		"1440",
		"2160p",
		"2160",
		"4320p",
		"4320",
		"4k",
		"8k",
		"uhd",
		"hdtv",
		"x264",
		"x265",
		"h264",
		"h265",
		"hevc",
		"av1",
		"ac3",
		"eac3",
		"ddp",
		"dts",
		"aac",
		"brrip",
		"bdrip",
		"bluray",
		"hdrip",
		"dvdrip",
		"web",
		"webdl",
		"webrip",
		"pdtv",
		"dvdscr",
		"xvid",
		"hdr",
		"hdr10",
		"hdr10plus",
		"dv",
		"dolbyvision",
		"remux",
		"proper",
		"repack",
		"internal",
		"unrated",
		"10bit",
		"extended",
		"5.1",
		"7.1"
	};

	static const df::hash_set<std::string_view, df::ihash, df::ieq> pre_title_stop_words
	{
		"(",
		"[",
		"-",
	};

	static const df::hash_set<std::string_view, df::ihash, df::ieq> pre_show_stop_words
	{
		"-",
	};

	static const auto split_rx = std::regex{R"([\s]+|_|\-|\.|\(|\[|\]|\))"s};

	const auto name = std::string_view(std::bit_cast<const char*>(name8.data()), name8.size());

	auto tokens = std::vector<std::string>(
		std::regex_token_iterator<std::string_view::const_iterator>{name.begin(), name.end(), split_rx, {-1, 0}},
		std::regex_token_iterator<std::string_view::const_iterator>{}
	);

	static const auto separator_rx = std::regex{R"([\s]+|_|\.)"s};

	std::erase_if(tokens, [](auto&& i) { return i.empty() || std::regex_match(i, separator_rx); });

	static const auto episode_rx = std::regex{"s([0-9]{1,2})e([0-9]{1,3})"s, std::regex_constants::icase};
	static const auto episode_of_rx = std::regex{"([0-9]{1,3})of([0-9]{1,3})"s, std::regex_constants::icase};
	static const auto episode_x_rx = std::regex{"([0-9]{1,2})x([0-9]{1,3})"s, std::regex_constants::icase};
	static const auto year_rx = std::regex{"(19|20)[0-9]{2}"s};
	static const auto dimensions_rx = std::regex{"([0-9]{3,5})x([0-9]{3,5})"s, std::regex_constants::icase};

	struct episode_info
	{
		int season = 0;
		int episode = 0;
		int episode_of = 0;
	};

	const auto parse_episode = [&](const std::string& token) -> std::optional<episode_info>
	{
		std::smatch match;

		if (std::regex_match(token, match, episode_rx))
		{
			const auto season = str::to_int(match[1].str());
			const auto episode = str::to_int(match[2].str());
			if (season <= 99 && episode > 0 && episode <= 255) return episode_info{season, episode, 0};
		}
		else if (std::regex_match(token, match, episode_of_rx))
		{
			const auto episode = str::to_int(match[1].str());
			const auto episode_of = str::to_int(match[2].str());
			if (episode > 0 && episode <= episode_of && episode_of <= 255)
				return episode_info{0, episode, episode_of};
		}
		else if (std::regex_match(token, match, episode_x_rx))
		{
			const auto season = str::to_int(match[1].str());
			const auto episode = str::to_int(match[2].str());
			if (season <= 99 && episode > 0 && episode <= 255) return episode_info{season, episode, 0};
		}

		return std::nullopt;
	};

	const auto found_episode_num = std::ranges::find_if(tokens, [&](const auto& token)
	{
		return parse_episode(token).has_value();
	});

	auto end_show = found_episode_num;

	while (end_show != tokens.begin() &&
		end_show != tokens.end() &&
		pre_show_stop_words.contains(*(end_show - 1)))
	{
		--end_show;
	}

	if (end_show != tokens.begin() &&
		end_show != tokens.end() &&
		*(end_show - 1) == ")")
	{
		--end_show;

		if (end_show != tokens.begin() &&
			end_show != tokens.end() &&
			std::regex_match(*(end_show - 1), year_rx))
		{
			--end_show;

			if (end_show != tokens.begin() &&
				end_show != tokens.end() &&
				*(end_show - 1) == "(")
			{
				result.year = str::to_int(*end_show);
				--end_show;
			}
		}
	}

	auto end_title = std::ranges::find_if(tokens, [&](const auto& token)
	{
		if (stop_words.contains(token)) return true;

		std::smatch match;
		if (!std::regex_match(token, match, dimensions_rx)) return false;

		const auto width = str::to_int(match[1].str());
		const auto height = str::to_int(match[2].str());
		return width >= 320 && height >= 200;
	});

	while (end_title != tokens.begin() &&
		end_title != tokens.end() &&
		pre_title_stop_words.contains(*(end_title - 1)))
	{
		--end_title;
	}

	if (found_episode_num != tokens.end() && found_episode_num != tokens.begin())
	{
		result.show = build_result_string(tokens, tokens.begin(), end_show);

		const auto episode = parse_episode(*found_episode_num);
		if (episode)
		{
			result.season = episode->season;
			result.episode = episode->episode;
			result.episode_of = episode->episode_of;
		}

		result.title = build_result_string(tokens, found_episode_num + 1, end_title);
	}
	else
	{
		auto movie_title_end = end_title;
		auto found_year = false;

		if (movie_title_end != tokens.begin() && std::regex_match(*(movie_title_end - 1), year_rx))
		{
			result.year = str::to_int(*(movie_title_end - 1));
			--movie_title_end;
			found_year = true;
		}
		else if (movie_title_end - tokens.begin() >= 3 && *(movie_title_end - 1) == ")" &&
			std::regex_match(*(movie_title_end - 2), year_rx) && *(movie_title_end - 3) == "(")
		{
			result.year = str::to_int(*(movie_title_end - 2));
			movie_title_end -= 3;
			found_year = true;
		}

		if ((end_title != tokens.end() || found_year) && movie_title_end != tokens.begin())
		{
			result.title = build_result_string(tokens, tokens.begin(), movie_title_end);
		}
	}

	scan_trim(result.title);
	scan_trim(result.show);

	return result;
}


index_state::item_scan_request index_state::make_scan_request(const df::item_element_ptr& item,
                                                              const bool load_thumbnail, const bool claim_loading)
{
	df::assert_true(ui::is_ui_thread());
	const auto is_folder = item->is_folder();
	const auto thumbnail_needed = load_thumbnail && !is_folder && item->should_load_thumbnail();
	const auto thumbnail_generation = thumbnail_needed && claim_loading ? item->begin_thumbnail_load() : 0;
	return {
		item, item->path(), item->folder(), item->file_type(), is_folder, load_thumbnail && !is_folder,
		thumbnail_needed, item->has_thumb(), thumbnail_generation
	};
}

index_state::item_scan_requests index_state::make_scan_requests(const df::item_set& items,
                                                                const bool load_thumbnails)
{
	df::assert_true(ui::is_ui_thread());
	item_scan_requests requests;
	requests.reserve(items.size());

	for (const auto& item : items.items())
	{
		requests.emplace_back(make_scan_request(item, load_thumbnails));
	}

	return requests;
}

bool index_state::scan_items(const df::item_set& items_to_scan, const bool load_thumbs,
                             const bool refresh_from_file_system, const bool only_if_needed,
                             const bool scan_if_offline, const df::cancel_token& token, const bool force)
{
	return scan_items(make_scan_requests(items_to_scan, load_thumbs), refresh_from_file_system, only_if_needed,
	                  scan_if_offline, token, force);
}

bool index_state::scan_items(const item_scan_requests& requests,
                             const bool refresh_from_file_system,
                             const bool only_if_needed,
                             const bool scan_if_offline,
                             const df::cancel_token& token,
                             const bool force,
                             scan_batch_stats* stats_out)
{
	auto metadata_refresh_needed = false;
	uint64_t thumbs_scanned = 0;
	std::vector<std::pair<std::weak_ptr<df::item_element>, df::file_path>> updated_items;
	updated_items.reserve(requests.size());
	{
		df::measure_ms ms(stats.scan_items_ms);
		df::scope_locked_inc l(scanning_items);
		df::folder_groups items_by_folder;
		items_by_folder.build(requests,
		                      [](const item_scan_request& r) { return r.folder; },
		                      [](const item_scan_request& r) { return !r.is_folder; });

		const auto now = platform::now();
		db_write_batch writes(*this);

		for (const auto& ff : items_by_folder.groups())
		{
			if (token.is_cancelled()) break;

			const auto node = validate_folder(ff.folder, refresh_from_file_system, now);
			if (!node.folder) continue;

			for (const auto request_index : items_by_folder.elements(ff))
			{
				const auto& request = requests[request_index];

				if (token.is_cancelled()) break;

				if (!only_if_needed || request.thumbnail_needed)
				{
					const auto found_file = find_file(node.folder->files, request.path.name());
					const auto is_new = found_file == node.folder->files.end();
					// Thumbnail generation on its own is not a metadata refresh, so it must not force a regroup.
					const auto metadata_scan_wanted = is_new || force ||
						needs_scan_impl(node.folder, *found_file, false, scan_if_offline);
					const auto scan_possible = metadata_scan_wanted || request.thumbnail_needed;
					const auto scanned_before = is_new ? df::date_t{} : found_file->metadata_scanned.load();

					scan_item(node.folder, request.path, request.load_thumbnail, request.thumbnail_needed,
					          request.had_thumbnail, scan_if_offline, request.lifetime, true, request.file_type, force,
					          false, false, &writes, request.thumbnail_generation);

					if (request.thumbnail_needed) ++thumbs_scanned;

					if (metadata_scan_wanted)
					{
						// A file that cannot be scanned never advances metadata_scanned, so it reports
						// needs_scan on every pass - summarise on effect, not on intent.
						const auto found_after = find_file(node.folder->files, request.path.name());
						metadata_refresh_needed |= found_after != node.folder->files.end() &&
							!(found_after->metadata_scanned.load() == scanned_before);
					}

					// Items already carry the index record the query materialised them from, so a republish
					// is only worth a UI hop when a scan could run or validate_folder refreshed the folder.
					if (scan_possible || node.was_updated)
					{
						updated_items.emplace_back(request.lifetime, request.path);
					}
				}
			}

			// Per folder, so a long batch neither holds the rows nor loses more than one folder's work.
			writes.flush();
		}

		struct folder_update
		{
			std::weak_ptr<df::item_element> lifetime;
			df::folder_path folder;
			df::index_folder_item_ptr info;
			df::count_and_size total;
		};

		std::vector<folder_update> folder_updates;

		for (const auto& request : requests)
		{
			if (token.is_cancelled()) break;

			if (request.is_folder)
			{
				const auto folder_path = request.folder;
				const auto node = validate_folder(folder_path, refresh_from_file_system, now);
				// A folder that cannot be enumerated and was never indexed answers null, and
				// update_folder dereferences whatever record it is handed.
				if (!node.folder) continue;
				// Re-summarising a folder is not a metadata refresh. Reporting one unconditionally made
				// every scan of a listing that contains a folder re-invalidate index_summary (and
				// group_layout via queue_scan_displayed_items), which never settled.
				metadata_refresh_needed |= node.was_updated;
				folder_updates.emplace_back(request.lifetime, folder_path, node.folder,
				                            platform::calc_folder_summary(folder_path, setting.show_hidden, token));
			}
		}

		// One publication for the whole batch rather than a UI hop per folder.
		if (!folder_updates.empty())
		{
			_async.queue_ui([folder_updates = std::move(folder_updates)]
			{
				for (const auto& update : folder_updates)
				{
					const auto item = update.lifetime.lock();
					if (item && item->is_folder() && item->folder() == update.folder)
					{
						item->update_folder(update.info, update.total);
					}
				}
			});
		}
	}

	struct completed_thumbnail
	{
		std::weak_ptr<df::item_element> item;
		df::file_path path;
		uint64_t generation = 0;
	};

	std::vector<completed_thumbnail> completed;
	completed.reserve(requests.size());
	for (const auto& request : requests)
	{
		if (request.thumbnail_needed) completed.emplace_back(request.lifetime, request.path,
		                                                     request.thumbnail_generation);
	}

	if (stats_out)
	{
		stats_out->thumbs_requested = completed.size();
		stats_out->thumbs_scanned = thumbs_scanned;
	}

	if (!completed.empty() || !updated_items.empty())
	{
		_async.queue_ui([this, completed = std::move(completed), updated_items = std::move(updated_items), token]
		{
			for (const auto& completion : completed)
			{
				const auto current_item = completion.item.lock();
				if (current_item && current_item->path() == completion.path &&
					current_item->is_current_thumbnail_load(completion.generation))
				{
					current_item->is_loading_thumbnail(false);
					// Staging must run even when the batch was cancelled: this path publishes with
					// stage_surface false, so anything loaded before the cancel would stay unstaged and
					// the item would never ask for its thumbnail again.
					current_item->stage_thumbnail_surface(_async);
				}
				else
				{
					df::bump(df::thumbnail_perf.scan_completions_stale);
				}
			}

			auto layout_changed = false;

			for (const auto& [item, path] : updated_items)
			{
				const auto current_item = item.lock();
				if (!current_item || current_item->path() != path) continue;

				const auto current_info = find_item(path);
				if (current_info.name == path.name()) layout_changed |= current_item->update(path, current_info);
			}

			// A republished item can change its tile geometry - a newly scanned size, or cover art the
			// tile is now shaped by - and nothing else in this hop asks for the layout it needs.
			if (layout_changed) _async.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
		});
	}

	const auto load_thumbs = std::ranges::any_of(requests, [](const auto& request)
	{
		return request.load_thumbnail;
	});
	df::trace(std::format("Index scan {} items (thumbs={} refresh-fs={}) in {} ms", requests.size(),
	                      load_thumbs,
	                      refresh_from_file_system, stats.scan_items_ms));
	if (metadata_refresh_needed)
	{
		_async.invalidate_view(view_invalid::index_summary);
	}
	return metadata_refresh_needed;
}

void index_state::scan_folder(const df::folder_path folder_path, const df::index_folder_item_ptr& folder)
{
	for (const auto& f : folder->files)
	{
		scan_item(folder, {folder_path, f.name}, false, false, false, false, {}, false, f.ft);
	}

	// See the comment at the other folders_snapshot() call sites: the returned shared_ptr must
	// outlive the loop, not just the range-for initializer.
	const auto folders_snapshot = folder->folders_snapshot();

	for (const auto& f : *folders_snapshot)
	{
		queue_scan_folder(folder_path.combine(f->name));
	}
}

bool index_state::scan_folder(const df::folder_path folder_path, const bool mark_is_indexed, const df::date_t timestamp)
{
	df::scope_locked_inc l(scanning_items);
	const auto node = validate_folder(folder_path, true, timestamp);

	if (node.deferred)
	{
		// The node may not list what is on disk - folders just copied in, for one - and the walk below
		// would never reach them. The follow-up rescans the whole folder instead.
		queue_deferred_validation(folder_path, true);
		return false;
	}

	if (!node.folder) return false;
	node.folder->is_in_collection = mark_is_indexed;
	scan_folder(folder_path, node.folder);

	if (node.folder->is_in_collection && node.was_updated)
	{
		_async.invalidate_view(view_invalid::index_summary);
	}

	return node.was_updated;
}

void index_state::queue_scan_listed_items(const df::item_set& listed_items)
{
	static std::atomic_int version;
	df::cancel_token token(version);
	auto requests = make_scan_requests(listed_items, false);

	_async.queue_async(async_queue::scan_folder, [this, requests = std::move(requests), token]
	{
		// Presence matching walks every file in every indexed folder for every listed item, so it is only
		// worth doing when this scan actually moved an index record.
		if (scan_items(requests, false, false, false, token))
		{
			_async.invalidate_view(view_invalid::presence);
		}
	});
}

void index_state::claim_for_write(const std::vector<df::file_path>& paths)
{
	df::assert_true(ui::is_ui_thread());

	for (const auto& path : paths)
	{
		++_write_claims[path];
	}
}

void index_state::release_write_claim(const std::vector<df::file_path>& paths)
{
	df::assert_true(ui::is_ui_thread());

	for (const auto& path : paths)
	{
		const auto found = _write_claims.find(path);
		if (found == _write_claims.end()) continue;
		if (--found->second <= 0) _write_claims.erase(found);
	}

	if (_deferred_modified_scans.empty()) return;

	df::item_set still_claimed;
	df::item_set ready;

	for (const auto& i : _deferred_modified_scans.items())
	{
		if (_write_claims.contains(i->path())) still_claimed.add(i);
		else ready.add(i);
	}

	_deferred_modified_scans = std::move(still_claimed);

	// Deferred because a write was running, so the index record is stale by construction.
	if (!ready.empty()) queue_scan_modified_items(std::move(ready), true);
}

void index_state::queue_scan_modified_items(df::item_set items_to_scan, const bool force)
{
	df::assert_true(ui::is_ui_thread());

	if (!_write_claims.empty())
	{
		df::item_set unclaimed;

		for (const auto& i : items_to_scan.items())
		{
			if (_write_claims.contains(i->path())) _deferred_modified_scans.add(i);
			else unclaimed.add(i);
		}

		if (unclaimed.empty()) return;
		items_to_scan = std::move(unclaimed);
	}

	auto requests = make_scan_requests(items_to_scan, true);
	_async.queue_async(async_queue::scan_modified_items, [this, requests = std::move(requests), force]
	{
		const auto start_ms = df::now_ms();
		scan_items(requests, true, false, false, {}, force);
		_async.invalidate_view(view_invalid::index_summary | view_invalid::media_elements |
			view_invalid::presence);
		df::trace(std::format("Index scan modified {} items in {} ms", requests.size(),
		                      df::now_ms() - start_ms));
	});
}

void index_state::queue_scan_displayed_items(df::item_set visible)
{
	df::assert_true(ui::is_ui_thread());

	if (!_write_claims.empty())
	{
		df::item_set unclaimed;

		// No loading claim has been made yet, so a skipped item is simply re-offered by
		// items_view::retry_visible_thumbnails once the write releases it.
		for (const auto& i : visible.items())
		{
			if (!_write_claims.contains(i->path())) unclaimed.add(i);
		}

		if (unclaimed.empty()) return;
		visible = std::move(unclaimed);
	}

	// Visible-thumbnail cancellation invariant (see also queue_scan_offline_thumbnails): a batch may
	// EITHER use a cancel token OR mark items with a batch-wide "pending" flag up front, never both --
	// a cancelled batch that pre-marked a batch flag would leave unprocessed items stuck forever.
	// This (local) path uses a cancel token: a fresh token per call cancels the previous in-flight
	// batch so scrolling abandons work for items scrolled past. It marks NO batch-wide flag; the only
	// per-item loading claim is made on UI while building the immutable request batch and is cleared
	// by path-checked UI completion even under cancellation. That release is why the batch must still
	// reach the worker; items_view::retry_visible_thumbnails re-requests whatever a cancelled batch
	// abandoned, since the visible set stops changing once scrolling stops.
	static std::atomic_int version;
	df::cancel_token token(version);
	auto requests = make_scan_requests(visible, true);

	// Depth is the gauge that shows whether scroll enqueues batches faster than the worker drains
	// them - the cost this queue pays for using enqueue rather than reset_and_enqueue.
	df::bump(df::thumbnail_perf.scan_batches_queued);
	df::record_peak(df::thumbnail_perf.scan_batches_pending_peak,
	                df::thumbnail_perf.scan_batches_pending.fetch_add(1, std::memory_order_relaxed) + 1);

	_async.queue_async(async_queue::scan_displayed_items, [this, requests = std::move(requests), token]
	{
		df::scope_locked_inc thumbnailing(thumbnailing_items);
		df::thumbnail_perf.scan_batches_pending.fetch_sub(1, std::memory_order_relaxed);

		if (!requests.empty())
		{
			scan_batch_stats stats;
			const auto metadata_refresh_needed = scan_items(requests, false, true, false, token, false, &stats);

			df::bump(df::thumbnail_perf.scan_batches);
			if (token.is_cancelled()) df::bump(df::thumbnail_perf.scan_batches_cancelled);
			df::bump(df::thumbnail_perf.scan_thumbs_requested, stats.thumbs_requested);
			df::bump(df::thumbnail_perf.scan_thumbs_scanned, stats.thumbs_scanned);

			_async.queue_ui([this, token, metadata_refresh_needed]
			{
				if (token.is_cancelled()) return;
				_async.invalidate_view(metadata_refresh_needed
					                       ? view_invalid::view_layout | view_invalid::group_layout
					                       : view_invalid::view_redraw);
			});
		}
	});
}

void index_state::queue_stage_thumbnails(const df::item_elements& items)
{
	if (items.empty()) return;

	df::assert_true(ui::is_ui_thread());
	for (const auto& item : items)
	{
		item->stage_thumbnail_surface(_async, true);
	}
}

void index_state::publish_thumbnail(std::weak_ptr<df::item_element> item, df::file_path path,
                                    ui::const_image_ptr thumbnail, ui::const_image_ptr cover_art,
                                    const df::date_t timestamp,
                                    const uint64_t generation, const bool fade_in, const bool stage_surface) const
{
	_async.queue_ui([this, item = std::move(item), path = std::move(path), thumbnail = std::move(thumbnail),
			cover_art = std::move(cover_art), timestamp, generation, fade_in, stage_surface]() mutable
		{
			const auto current_item = item.lock();
			if (!current_item || current_item->path() != path) return;
			const auto owns_loading = generation != 0 && current_item->is_current_thumbnail_load(generation);
			const auto owns_pixels = generation == 0 || current_item->is_current_thumbnail_request(generation);
			if (owns_loading) current_item->is_loading_thumbnail(false);
			if (!owns_pixels) return;

			const auto previous_dims = current_item->layout_dims();
			const auto previous_orientation = current_item->layout_orientation();
			current_item->thumbnail(std::move(thumbnail), std::move(cover_art), timestamp, fade_in);
			const auto geometry_changed = previous_dims != current_item->layout_dims() ||
				previous_orientation != current_item->layout_orientation();
			if (stage_surface)
			{
				current_item->stage_thumbnail_surface(_async, true);
			}
			else
			{
				_async.invalidate_view(view_invalid::view_redraw);
			}
			if (geometry_changed) _async.invalidate_view(view_invalid::view_layout);
		});
}

void index_state::publish_thumbnails(thumbnail_results results, const bool invalidate_group_layout) const
{
	if (results.empty() && !invalidate_group_layout) return;

	_async.queue_ui([this, results = std::move(results), invalidate_group_layout]() mutable
	{
		auto geometry_changed = false;
		df::bump(df::thumbnail_perf.published_db, results.size());

		for (auto& result : results)
		{
			const auto item = result.lifetime.lock();
			if (!item || item->path() != result.path) continue;
			if (result.generation != 0 && !item->is_current_thumbnail_request(result.generation)) continue;

			const auto previous_dims = item->layout_dims();
			const auto previous_orientation = item->layout_orientation();
			item->thumbnail(std::move(result.thumbnail), std::move(result.cover_art), result.timestamp);
			geometry_changed = geometry_changed || previous_dims != item->layout_dims() ||
				previous_orientation != item->layout_orientation();
		}

		_async.invalidate_view(invalidate_group_layout || geometry_changed
			                       ? view_invalid::view_redraw | view_invalid::view_layout
			                       : view_invalid::view_redraw);
	});
}

void index_state::publish_item_update(std::weak_ptr<df::item_element> item, df::file_path path) const
{
	_async.queue_ui([this, item = std::move(item), path = std::move(path)]
	{
		const auto current_item = item.lock();
		if (!current_item || current_item->path() != path) return;

		const auto current_info = find_item(path);
		if (current_info.name != path.name()) return;

		if (current_item->update(path, current_info))
		{
			_async.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
		}
	});
}

void index_state::publish_thumbnail_failure(std::weak_ptr<df::item_element> item, df::file_path path,
                                            const uint64_t generation) const
{
	_async.queue_ui([item = std::move(item), path = std::move(path), generation]
	{
		const auto current_item = item.lock();
		if (!current_item || current_item->path() != path) return;
		if (generation != 0 && !current_item->is_current_thumbnail_request(generation)) return;

		df::bump(df::thumbnail_perf.load_failures);
		current_item->failed_loading_thumbnail(true);
	});
}

void index_state::publish_crc(std::weak_ptr<df::item_element> item, df::file_path path, const df::file_size size,
                              const df::date_t modified, const df::item_online_status online_status,
                              const uint32_t existing_crc, const uint32_t crc)
{
	_async.queue_ui(
		[this, item = std::move(item), path = std::move(path), size, modified, online_status, existing_crc, crc]
		{
			const auto current_item = item.lock();

			// Size alone does not identify content: a file replaced by one of the same length is a
			// different file, and its modified time is what says so.
			if (!current_item || current_item->path() != path || current_item->file_size() != size ||
				current_item->file_modified() != modified ||
				current_item->online_status() != online_status || current_item->crc32c() != existing_crc)
			{
				return;
			}

			save_crc(path, {modified, size.to_int64()}, crc);
			current_item->crc32c(crc);
			// A checksum changes no tile geometry, so a redraw is enough; relayout here re-wrapped the
			// grid under the pointer while duplicate detection worked through a folder.
			_async.invalidate_view(view_invalid::view_redraw | view_invalid::presence);
		});
}

void index_state::queue_load_visible_thumbnails(const df::item_elements& visible)
{
	df::assert_true(ui::is_ui_thread());
	if (visible.empty()) return;

	queue_stage_thumbnails(visible);

	auto queue_sources = [this](const df::item_elements& items)
	{
		df::item_elements local;
		df::item_elements offline;
		for (const auto& item : items)
		{
			item->add_if_thumbnail_load_needed(local);
			item->add_if_shell_thumbnail_needed(offline);
		}
		if (!local.empty()) queue_scan_displayed_items(std::move(local));
		if (!offline.empty()) queue_scan_offline_thumbnails(std::move(offline));
	};

	database::thumbnail_requests requests;
	df::item_elements resolved;
	for (const auto& item : visible)
	{
		if (const auto generation = item->begin_db_thumbnail_query())
		{
			requests.emplace_back(item, item->path(), item->folder(), generation, item->is_folder(),
			                      item->has_thumb());
		}
		else
		{
			resolved.emplace_back(item);
		}
	}

	if (!requests.empty())
	{
		_async.queue_database([this, requests = std::move(requests)](const database& db)
		{
			db.load_thumbnails(*this, requests);
			_async.queue_ui([this, requests = std::move(requests)]
			{
				df::item_elements current_visible;
				current_visible.reserve(requests.size());
				for (const auto& request : requests)
				{
					auto item = request.lifetime.lock();
					if (item && item->path() == request.path && item->is_visible())
						current_visible.emplace_back(std::move(item));
				}
				queue_load_visible_thumbnails(std::move(current_visible));
			});
		});
	}

	queue_sources(resolved);
}

void index_state::queue_load_thumbnail(df::item_element_ptr item, const view_invalid invalid)
{
	if (!item || item->has_thumb()) return;

	// The database hop copies this lambda, so the item crosses a worker queue: ui_owned_ptr hands
	// the final reference back to the UI thread if a truncated queue drops it there.
	auto load_from_source = [this, item = ui_owned(_async, item), invalid]
	{
		if (item->has_thumb())
		{
			queue_stage_thumbnails({item.shared()});
			_async.invalidate_view(invalid);
			return;
		}

		df::item_set items;
		item->add_to(items);

		if (item->online_status() == df::item_online_status::offline)
		{
			queue_scan_offline_thumbnails(std::move(items), false, invalid);
		}
		else if (item->should_load_thumbnail())
		{
			auto requests = make_scan_requests(items, true);
			_async.queue_async(async_queue::scan_folder, [this, requests = std::move(requests), invalid]
			{
				df::scope_locked_inc thumbnailing(thumbnailing_items);
				scan_items(requests, false, true, false, {});
				_async.invalidate_view(invalid);
			});
		}
	};

	if (item->begin_db_thumbnail_query())
	{
		df::item_set items;
		item->add_to(items);
		auto requests = database::make_thumbnail_requests(items);
		_async.queue_database([this, requests = std::move(requests), load_from_source](const database& db)
		{
			db.load_thumbnails(*this, requests);
			_async.queue_ui(load_from_source);
		});
	}
	else
	{
		load_from_source();
	}
}

void index_state::queue_scan_offline_thumbnails(const df::item_set& items, const bool visible_only,
	                                             const view_invalid invalid)
{
	// Cloud (OneDrive) thumbnail fetch for VISIBLE offline placeholders. Unlike the local
	// displayed-items path (queue_scan_displayed_items), this does NOT use a cancel token: it is
	// re-enqueued on every layout to drive the view_state::tick retry of icon-only items, and a
	// cancel token would let each re-enqueue cancel the in-flight batch and strand its pre-marked
	// items. Instead, staying on the visible set is done PER ITEM: each item carries an is_visible()
	// flag maintained by items_view::update_visible_items_list, and the batch skips (and re-arms) any
	// item that has scrolled out of view since it was queued -- so scrolling around a large folder
	// abandons stale screenfuls instead of grinding through them. The worker cannot read that
	// UI-owned flag, so a batch counter tells it when a newer visible set has been queued and the
	// rest of its own items are stale. Items are marked shell_thumbnail_pending up front to
	// de-duplicate concurrent batches; that flag is cleared on every exit path (fetched, skipped or
	// failed) so nothing is ever left stuck pending.

	df::assert_true(ui::is_ui_thread());

	struct request
	{
		std::weak_ptr<df::item_element> lifetime;
		df::file_path path;
		df::date_t modified;
		uint64_t generation = 0;
		bool eligible = false;
		bool abandoned = false;
	};

	struct result
	{
		request source;
		platform::get_cached_file_properties_response response =
			platform::get_cached_file_properties_response::fail;
		ui::const_image_ptr thumbnail;
	};

	std::vector<request> requests;
	requests.reserve(items.size());
	for (const auto& i : items.items())
	{
		const auto eligible = (!visible_only || i->is_visible()) && !i->is_folder() &&
			i->online_status() == df::item_online_status::offline;
		requests.emplace_back(i, i->path(), i->file_modified(), i->begin_thumbnail_request(), eligible);
	}

	// The single-item hover path does not supersede a visible batch.
	const auto batch = visible_only ? ++_offline_thumbnail_batch : _offline_thumbnail_batch.load();

	_async.queue_async(async_queue::cloud,
	                  [this, requests = std::move(requests), visible_only, batch, invalid]() mutable
	{
		df::scope_locked_inc thumbnailing(thumbnailing_items);
		std::vector<result> results;
		results.reserve(requests.size());

		for (auto& request : requests)
		{
			// On shutdown the UI publication pass will not run at all, so there is nothing left to
			// clear and the remaining requests are simply dropped.
			if (df::is_closing) break;

			// A newer visible set has been queued, so the rest of this batch is stale. Report it as
			// abandoned rather than breaking out: the UI pass must still clear every pending claim, and
			// items that are still visible are re-armed for the next retry pass.
			if (visible_only && batch != _offline_thumbnail_batch.load(std::memory_order_relaxed))
			{
				request.abandoned = true;
			}

			if (!request.eligible || request.abandoned)
			{
				results.emplace_back(request, platform::get_cached_file_properties_response::pending, nullptr);
				continue;
			}

			ui::const_image_ptr shell_thumb;
			const auto response = read_shell_thumbnail(
				request.path, setting.thumbnail_max_dimension, true, shell_thumb);
			result completed{request, response, {}};

			if (response == platform::get_cached_file_properties_response::ok && is_valid(shell_thumb))
			{
				// Downscale/re-encode to the thumbnail size budget, matching scan_offline_item.
				files ff;

				auto thumbnail_image = shell_thumb;
				const auto max_extent = setting.thumbnail_max_dimension;
				const auto thumb_extent = thumbnail_image->dimensions();

				if (max_extent.cx < thumb_extent.cx || max_extent.cy < thumb_extent.cy)
				{
					auto surf = ff.image_to_surface(thumbnail_image, max_extent, false, {},
					                                decode_intent::thumbnail);
					thumbnail_image = ff.surface_to_thumbnail(surf);
				}

				if (is_valid(thumbnail_image) && thumbnail_image->data().size() < df::two_fifty_six_k)
				{
					completed.thumbnail = thumbnail_image;
					save_thumbnail(request.path, thumbnail_image, {}, request.modified);
				}
			}

			results.emplace_back(std::move(completed));
		}

		_async.queue_ui([this, results = std::move(results), visible_only, invalid]() mutable
		{
			auto geometry_changed = false;
			for (auto& completed : results)
			{
				const auto item = completed.source.lifetime.lock();
				if (!item || !item->is_current_thumbnail_request(completed.source.generation)) continue;

				item->shell_thumbnail_pending(false);
				if (!completed.source.eligible || (visible_only && !item->is_visible()) || item->is_folder() ||
					item->online_status() != df::item_online_status::offline)
				{
					continue;
				}

				if (completed.source.abandoned)
				{
					// Superseded by a newer visible set before it was fetched - re-arm it for the retry
					// pass without spending one of its bounded provider attempts.
					item->shell_thumbnail_retry_pending(true, false);
					continue;
				}

				if (is_valid(completed.thumbnail))
				{
					df::bump(df::thumbnail_perf.published_shell);
					const auto previous_dims = item->layout_dims();
					const auto previous_orientation = item->layout_orientation();
					item->thumbnail(std::move(completed.thumbnail), {}, completed.source.modified, true);
					geometry_changed = geometry_changed || previous_dims != item->layout_dims() ||
						previous_orientation != item->layout_orientation();
					item->stage_thumbnail_surface(_async);
				}
				else if (completed.response == platform::get_cached_file_properties_response::pending)
				{
					df::bump(df::thumbnail_perf.shell_retries);
					item->shell_thumbnail_retry_pending(true);
				}
				else
				{
					df::bump(df::thumbnail_perf.load_failures);
					item->failed_loading_thumbnail(true);
				}
			}

			_async.invalidate_view(invalid |
				(geometry_changed ? view_invalid::view_layout : view_invalid::none));
		});
	});
}

// Both passes walk the whole index, and apply_scan_result raises view_invalid::index_summary per
// scanned item, so a request arrives on every UI drain while a scan is running. The delay collapses
// that burst into one pass; the queue holds it, so no worker thread is spent waiting it out.
constexpr uint32_t summary_debounce_ms = 333;

void index_state::queue_update_predictions()
{
	const auto generation = ++_predictions_generation;

	_async.queue_async_after(async_queue::index_predictions_single, summary_debounce_ms, [this, generation]
	{
		if (df::is_closing) return;
		if (generation != _predictions_generation.load()) return;

		update_predictions();
		if (generation != _predictions_generation.load()) return;

		// Predictions only feed the sidebar; asking for refresh_items here closed a cycle that never
		// reached a steady state. Counts only: nothing about which rows exist has changed, so rebuilding
		// them would discard every sidebar text layout to publish a number.
		_async.invalidate_view(view_invalid::sidebar_counts);
	});
}

void index_state::queue_update_summary()
{
	uint64_t generation;
	{
		platform::exclusive_lock lock(_summary_rw);
		generation = ++_summary_generation;
	}

	_async.queue_async_after(async_queue::index_summary_single, summary_debounce_ms, [this, generation]
	{
		if (df::is_closing) return;
		{
			platform::shared_lock lock(_summary_rw);
			if (generation != _summary_generation) return;
		}

		update_summary(generation);
		{
			platform::shared_lock lock(_summary_rw);
			if (generation != _summary_generation) return;
		}
		// update_summary already asked for the rebuild its new vocabulary needs; this only has to
		// re-earn the counts that vocabulary changed.
		_async.invalidate_view(view_invalid::sidebar_counts);
	});
}


void index_state::queue_validate_changed_folders(df::unique_folders paths)
{
	_async.queue_async(async_queue::scan_folder, [this, paths = std::move(paths)]
	{
		const auto now = platform::now();
		auto changed = false;

		for (const auto& path : paths)
		{
			// A large set is still a bounded walk, but nothing here is worth holding a worker open
			// for while the application is closing.
			if (df::is_closing) return;
			changed |= validate_folder(path, true, now).was_updated;
		}

		if (changed)
		{
			_async.invalidate_view(view_invalid::refresh_items);
		}
	});
}

void index_state::queue_deferred_validation(const df::folder_path folder_path, const bool rescan)
{
	{
		platform::exclusive_lock lock(_deferred_validations_rw);
		const auto [found, inserted] = _deferred_validations.try_emplace(folder_path, rescan);

		if (!inserted)
		{
			found->second = found->second || rescan;
			return;
		}
	}

	_async.queue_async(async_queue::scan_folder, [this, folder_path]
	{
		// Claimed before the pass rather than after, so a pass that gives up again queues the next.
		auto rescan = false;
		{
			platform::exclusive_lock lock(_deferred_validations_rw);
			const auto found = _deferred_validations.find(folder_path);

			if (found != _deferred_validations.end())
			{
				rescan = found->second;
				_deferred_validations.erase(found);
			}
		}

		if (df::is_closing) return;

		const auto now = platform::now();
		const auto changed = rescan
			                     ? scan_folder(folder_path, is_in_collection(folder_path), now)
			                     : validate_folder(folder_path, true, now).was_updated;

		if (changed)
		{
			_async.invalidate_view(view_invalid::refresh_items);
		}
	});
}

void index_state::queue_scan_folder(const df::folder_path path)
{
	_async.queue_async(async_queue::scan_folder, [this, path]
	{
		const auto now = platform::now();
		scan_folder(path, is_in_collection(path), now);
	});
}

void index_state::queue_scan_folders(df::unique_folders paths)
{
	_async.queue_async(async_queue::scan_folder, [this, paths = std::move(paths)]
	{
		const auto now = platform::now();
		auto changed = false;

		for (const auto& path : paths)
		{
			if (df::is_closing) return;
			changed |= scan_folder(path, is_in_collection(path), now);
		}

		// Only a folder that differs needs the search re-run, and only the batch asks for it: the
		// recursive per-folder path would re-open the search once per folder it walked.
		_async.invalidate_view(changed
			                       ? view_invalid::view_layout | view_invalid::refresh_items
			                       : view_invalid::view_layout);
	});
}

void index_state::merge_folder(const df::folder_path folder_path, const db_items_t& items)
{
	const auto found_in_index = _items.find(folder_path);
	df::index_folder_item_ptr folder_node;
	struct cached_db_item
	{
		str::cached path;
		prop::item_metadata_ptr metadata;
		df::date_t metadata_scanned;
		uint32_t crc32c = 0;
		df::picture_hashes_ptr phash;
	};

	std::vector<cached_db_item> prepared_items;
	prepared_items.reserve(items.size());
	for (const auto& item : items)
	{
		prepared_items.emplace_back(item.path, item.metadata, item.metadata_scanned, item.crc32c,
		                            picture_hashes_from_db(item.phash));
	}

	if (found_in_index && !found_in_index->files.empty())
	{
		// Rows whose hashes describe bytes the file no longer holds. Cleared from the database as well
		// as kept off the node, or every later launch would load them back.
		std::vector<df::file_path> stale_hashes;

		_items.update_folder(folder_path, [&](const df::index_folder_item_ptr& current_folder)
		{
			df::assert_true(std::is_sorted(current_folder->files.begin(), current_folder->files.end()));

			auto file_first = prepared_items.begin();
			const auto file_last = prepared_items.end();
			auto old_first = current_folder->files.begin();
			const auto old_last = current_folder->files.end();

			while (file_first != file_last && old_first != old_last)
			{
				const auto d = icmp(file_first->path, old_first->name);

				if (d < 0)
				{
					// skip: only in new
					++file_first;
				}
				else if (d > 0)
				{
					// skip: only in old
					++old_first;
				}
				else
				{
					// merge: in both
					//
					// A node that is already here was built by a validation that ran ahead of the cache -
					// usually the startup query for the folder on screen - so it carries the file's
					// modified time. The row is judged against it the way validate_folder judges a node
					// loaded from the database: a row scanned before the file last changed describes
					// bytes that are gone. Adopting its hashes kept an edited file's old checksum for
					// every session after, since nothing on the node then read as changed, and paired
					// it with an untouched copy as identical.
					const auto live_modified = old_first->file_modified.load();
					const auto row_describes_file = !live_modified.is_valid() ||
						!(file_first->metadata_scanned < live_modified);

					// What this session has already scanned is newer than anything the row recorded.
					const auto live_is_newer = old_first->metadata.load() != nullptr &&
						!(old_first->metadata_scanned.load() < file_first->metadata_scanned);

					if (!live_is_newer)
					{
						old_first->metadata = file_first->metadata;
						old_first->metadata_scanned = file_first->metadata_scanned;
					}

					if (row_describes_file)
					{
						if (old_first->crc32c.load() == 0) old_first->crc32c = file_first->crc32c;
						if (!old_first->phash.load()) old_first->phash = file_first->phash;
					}
					else if (file_first->crc32c != 0 || file_first->phash)
					{
						stale_hashes.emplace_back(folder_path.combine_file(old_first->name));
					}

					old_first->calc_search_presence();
					++file_first;
					++old_first;
				}
			}

			// the merged metadata can both add and remove bits, so rebuild the summary rather than
			// OR-ing into the stale one
			current_folder->reset_search_presence();
			return true;
		});

		if (!stale_hashes.empty())
		{
			std::vector<item_db_write> writes;
			writes.reserve(stale_hashes.size());

			for (const auto& path : stale_hashes)
			{
				item_db_write write;
				write.path = path;
				write.clear_hashes = true;
				writes.emplace_back(std::move(write));
			}

			enqueue_db_writes(std::move(writes));
		}
	}
	else
	{
		df::index_item_infos files;
		files.resize(std::distance(prepared_items.begin(), prepared_items.end()));
		auto node = files.begin();

		for (auto i = prepared_items.begin(); i != prepared_items.end(); ++i)
		{
			const auto metadata = i->metadata;
			const auto id = i->path;
			const auto* const mt = files::file_type_from_name(i->path);

			auto& file_node = *node;
			file_node.name = id;
			file_node.ft = mt;
			file_node.metadata = metadata;
			file_node.crc32c = i->crc32c;
			file_node.phash = i->phash;
			file_node.metadata_scanned = i->metadata_scanned;

			file_node.calc_search_presence();
			++node;
		}

		df::assert_true(std::is_sorted(files.begin(), files.end()));

		folder_node = std::make_shared<df::index_folder_item>(std::move(files));
		folder_node->name = folder_path.name();

		if (found_in_index)
		{
			// the replacement stands in for the same folder, so it must keep what the scan learned
			folder_node->is_in_collection = found_in_index->is_in_collection.load();
			folder_node->is_excluded = found_in_index->is_excluded.load();
			folder_node->is_read_only = found_in_index->is_read_only;
			folder_node->volume = found_in_index->volume;
			folder_node->created = found_in_index->created;
			folder_node->modified = found_in_index->modified;
			folder_node->child_folders = found_in_index->folders_snapshot();
		}

		folder_node->reset_search_presence();
		_items.replace(folder_path, folder_node);
	}
}
