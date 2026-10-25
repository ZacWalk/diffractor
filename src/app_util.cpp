// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Utility functions for file operations including import, sync, and batch
// processing. Handles file renaming, metadata updates, folder synchronization, and the copies
// prepared for sharing.

#include "pch.h"
#include "libdesktop/util_strings.h"
#include "model_display.h"
#include "model_db.h"
#include "app_command_status.h"
#include "app_command_line.h"
#include "app_util.h"
#include "model_index.h"
#include "util_crash_files_db.h"
#include "util_zip.h"

// Process-level state, declared in app_command_line.h and app.h. It is used across the model and
// file layers, so it does not belong to the application frame that used to define it.

command_line_t command_line;

std::function<void(df::file_path path)> test_before_import_rollback_cleanup;
std::function<std::optional<platform::file_op_result>(df::file_path destination)>
test_import_sidecar_write_override;

crash_files_db& crash_files()
{
	static crash_files_db instance(df::probe_data_file("diffractor-files-that-crash.txt"),
	                               crash_files_db::release_tag(s_app_version));
	return instance;
}

void flush_open_files_to_crash_files_list()
{
	crash_files().flush_open_files();
}

void log_open_files_to_crash_files_list()
{
	crash_files().log_open_files();
}

void view_state::modify_items(const df::results_ptr& results, icon_index icon, const std::string_view title,
                              const df::item_elements& items_to_modify, const metadata_edits& edits,
                              const view_host_base_ptr& view)
{
	batch_edit_spec spec;
	spec.make_edits = [edits](df::file_path) { return item_edits{edits, {}}; };
	spec.process_type = df::process_items_type::can_save_metadata;
	spec.encode_params.jpeg_save_quality = setting.jpeg_save_quality;
	// This overload never applies photo edits, so orientation is the only thing that can change what
	// is drawn.
	spec.changes_presentation = edits.changes_presentation();

	modify_items(results, items_to_modify, spec, view);
}

void view_state::modify_items(const df::results_ptr& results, const df::item_elements& items_to_modify,
                              const batch_edit_spec& spec, const view_host_base_ptr& view)
{
	clear_error_items(view);
	const auto can_process = df::item_set(items_to_modify).can_process(spec.process_type, true, view);

	if (can_process.success())
	{
		// Handles must stay detached for the whole write, and the destructor touches UI-owned
		// state (display, player, selection), so the last reference is handed to the closing UI
		// callback. A stack object would reopen the files while the background write is running
		// whenever the caller's status does not block (view_command_status::wait_for_complete).
		const auto detach = std::make_shared<detach_file_handles>(*this);

		struct modify_request
		{
			index_state::item_scan_request scan;
			str::cached name;
			df::file_path path;
			str::cached xmp;
		};

		std::vector<modify_request> requests;
		requests.reserve(items_to_modify.size());
		std::vector<df::file_path> claimed;
		claimed.reserve(items_to_modify.size());
		for (const auto& item : items_to_modify)
		{
			// A write that cannot change what is drawn leaves the existing thumbnail correct, so the
			// post-write rescan must not regenerate it. Re-decoding one is slow on large video and
			// republishing it makes the tile visibly blank and reappear.
			const auto load_thumbnail = spec.changes_presentation || !item->has_thumb();
			requests.emplace_back(item_index.make_scan_request(item, load_thumbnail, false), item->name(),
			                      item->path(), item->xmp());
			claimed.emplace_back(item->path());
		}

		// Declared here, on the UI thread and before the write starts, so the modified time it
		// produces does not send the thumbnail and the displayed image back through a reload that
		// would return the same result.
		if (!spec.changes_presentation)
		{
			for (const auto& item : items_to_modify)
			{
				item->retain_thumbnail_across_next_write();
			}

			if (const auto d = display_state())
			{
				const auto is_modified = [&items_to_modify](const df::item_element_ptr& i)
				{
					if (!i) return false;
					for (const auto& item : items_to_modify) if (item == i) return true;
					return false;
				};

				if (d->_selected_texture1 && is_modified(d->_item1)) d->_selected_texture1->mark_visuals_current();
				if (d->_selected_texture2 && is_modified(d->_item2)) d->_selected_texture2->mark_visuals_current();
			}
		}

		// No read of these files may be queued while the write runs; anything asked for meanwhile is
		// deferred and re-requested on release.
		item_index.claim_for_write(claimed);

		queue_async(async_queue::work,
		            [this, requests = std::move(requests), claimed = std::move(claimed), spec, results, detach,
			            displayed_paths = spec.changes_presentation
				                              ? displayed_photo_paths()
				                              : std::vector<df::file_path>{},
			            detached_av_path = detached_display_av_path()]() mutable
		            {
			            result_scope rr(results);
			            std::string message;
			            files ff;

			            // Only the UI hop at the end releases the claim, and drain_queue swallows an
			            // exception before it is ever posted - which parks these items with no thumbnail and
			            // no metadata refresh for the rest of the session. The release is therefore owned by
			            // a guard rather than by the success path. The guard keeps its own copy of the paths
			            // because the success path moves the originals into the UI callback.
			            auto claim_handed_over = false;
			            df::scope_exit release_claim([this, &claim_handed_over, claimed]
			            {
				            if (!claim_handed_over)
				            {
					            _async.queue_ui([this, claimed] { item_index.release_write_claim(claimed); });
				            }
			            });

			            // Items the write already re-scanned for us vs. those whose scan failed and still need a
			            // forced background rescan.
			            std::vector<index_state::item_scan_request> immediate_done;
			            std::vector<index_state::item_scan_request> needs_force;
			            // The written bytes for each item on screen, handed over instead of read back. The modified
			            // time travels with them so the display stamps in the file clock domain.
			            struct written_image
			            {
				            df::file_path path;
				            file_load_result loaded;
				            df::date_t modified;
			            };
			            std::vector<written_image> written_images;
			            // The open handle over the playable item the display is about to reopen.
			            platform::file_ptr detached_av_handle;

			            for (const auto& request : requests)
			            {
				            // Asked before the write, not after it. A cancel that arrived while the
				            // previous item was being written - or before this loop started at all -
				            // must not reach one more file, and the row it stops at is reported as the
				            // cancellation it was rather than left out of the result list entirely.
				            if (results->is_canceled())
				            {
					            results->end_item(request.name, item_status::cancel);
					            continue;
				            }

				            results->start_item(request.name);

				            const auto edits = spec.make_edits(request.path);
				            file_update_result update_result;
				            const auto is_displayed = std::ranges::find(displayed_paths, request.path) !=
					            displayed_paths.end();

				            if (edits)
				            {
					            update_result = ff.update(request.path, request.path, edits->metadata, edits->image,
					                                      spec.encode_params, false, request.xmp, request.xmp,
					                                      index_state::make_rescan_spec(request.scan, request.xmp,
						                                      is_displayed,
						                                      request.path == detached_av_path));
				            }

				            results->end_item(request.name, to_status(update_result.code));

				            if (!update_result.success())
				            {
					            if (failed_write_needs_forced_rescan(update_result))
					            {
						            needs_force.emplace_back(request.scan);
					            }
					            message = update_result.format_error();
					            break;
				            }

				            if (request.path == detached_av_path) detached_av_handle = std::move(
					            update_result.display_handle);

				            if (is_displayed)
				            {
					            written_images.emplace_back(request.path, std::move(update_result.loaded),
					                                        df::date_t(update_result.modified));
				            }

				            if (item_index.apply_write_scan(request.scan, update_result))
				            {
					            needs_force.emplace_back(request.scan);
				            }
				            else
				            {
					            immediate_done.emplace_back(request.scan);
				            }
			            }

			            // Background rescan: force the items whose immediate scan failed (the mtime-tie heuristic
			            // can miss a rapid successive edit whose sidecar modified time ties with the previous scan
			            // timestamp); the scanned items are already current, so queue them non-forced as a cheap
			            // no-op safety net. The detach reference is handed over here so the handles stay
			            // detached until the writes finish and the destructor runs on the UI thread.
			            _async.queue_ui(
				            [this, detach = std::move(detach), claimed = std::move(claimed), immediate_done =
					            std::move(immediate_done), needs_force = std::move(needs_force), written_images =
					            std::move(written_images), detached_av_path, detached_av_handle = std::move(
						            detached_av_handle)]() mutable
				            {
					            item_index.release_write_claim(claimed);

					            for (auto& written : written_images)
					            {
						            publish_written_image(written.path, std::move(written.loaded), written.modified);
					            }

					            // Before the guard is released below, because releasing it is what reopens the session.
					            publish_written_handle(detached_av_path, std::move(detached_av_handle));

					            df::item_set immediate_items;
					            df::item_set force_items;
					            for (const auto& request : immediate_done)
					            {
						            const auto item = request.lifetime.lock();
						            if (item && item->path() == request.path) immediate_items.add(item);
					            }
					            for (const auto& request : needs_force)
					            {
						            const auto item = request.lifetime.lock();
						            if (item && item->path() == request.path) force_items.add(item);
					            }
					            if (!force_items.empty()) item_index.queue_scan_modified_items(
						            std::move(force_items), true);
					            if (!immediate_items.empty()) item_index.queue_scan_modified_items(
						            std::move(immediate_items), false);
				            });

			            claim_handed_over = true;

			            rr.complete(message);
		            });

		results->wait_for_complete();
	}
	else
	{
		results->show_message(can_process.to_string());
	}
}


static platform::file_op_result move_or_copy(const df::file_path source_path, const df::file_path dest_path,
                                             const bool is_move, const bool fail_if_exists)
{
	return is_move
		       ? platform::move_file(source_path, dest_path, fail_if_exists)
		       : platform::copy_file(source_path, dest_path, fail_if_exists, false);
}

static platform::file_op_result delete_temp_file(const df::file_path path)
{
	if (path.is_empty()) return {platform::file_op_result_code::OK};
	if (!path.exists()) return {platform::file_op_result_code::OK};

	platform::make_file_writable(path);
	const auto result = platform::delete_file(path);
	if (result.failed())
	{
		df::log(__FUNCTION__, std::format("could not delete temporary file {}; {}", path.str(), result.format_error()));
	}
	return result;
}

static df::file_path rename_destination(const rename_source& item, const std::string_view name)
{
	const auto source = item.source;
	return item.is_folder
		       ? df::file_path(source.folder().parent(), name)
		       : df::file_path(source.folder(), name, source.extension());
}

bool rename_path_exists(const df::file_path path, const bool is_folder)
{
	return is_folder ? platform::exists(path.folder().combine(path.name())) : path.exists();
}

std::string rename_path_key(const df::file_path path, const bool is_folder)
{
	return is_folder ? std::string(path.folder().combine(path.name()).text()) : path.pack();
}

std::vector<rename_source> snapshot_rename_sources(const df::item_set& items)
{
	std::vector<rename_source> result;
	result.reserve(items.size());
	for (const auto& item : items.items())
	{
		rename_source source;
		source.source = item->path();
		source.original_name = item->base_name();
		source.metadata = item->metadata();
		source.media_created = item->calc_media_created();
		source.is_folder = item->is_folder();
		if (!source.is_folder)
			for (const auto& sidecar_name : split(item->sidecars(), true))
				source.sidecars.emplace_back(source.source.folder().combine_file(sidecar_name));
		result.emplace_back(std::move(source));
	}
	return result;
}

std::vector<rename_item> calc_item_renames(const std::vector<rename_source>& items,
                                           const std::string_view template_name,
                                           const int start, const collision_policy policy)
{
	std::vector<rename_item> results;
	results.reserve(items.size());
	std::vector<char> name_is_usable;
	name_is_usable.reserve(items.size());

	// Every path this plan moves away from. A destination held by one of them is free by the time
	// the run reaches it, so renaming a set onto the names it is vacating is not a collision.
	std::set<std::string, df::path_key_less> vacated;
	auto seq = start;

	const auto assign_sidecars = [](rename_item& rename, const rename_source& i, const df::file_path primary)
	{
		rename.sidecars.clear();
		rename.sidecar_destinations_exist.clear();
		rename.sidecar_source_attributes.clear();
		rename.sidecar_destination_attributes.clear();
		if (i.is_folder) return;

		for (const auto sidecar_source : i.sidecars)
		{
			const auto sidecar_destination =
				primary.folder().combine_file(primary.file_name_without_extension()).extension(
					sidecar_source.extension());
			const auto destination_attributes = platform::file_attributes(sidecar_destination);
			rename.sidecar_destinations_exist.emplace_back(destination_attributes.exists());
			rename.sidecar_source_attributes.emplace_back(platform::file_attributes(sidecar_source));
			rename.sidecar_destination_attributes.emplace_back(destination_attributes);
			rename.sidecars.emplace_back(sidecar_source, sidecar_destination);
		}
	};

	for (const auto& i : items)
	{
		const auto& md = i.metadata;
		const auto& original_name = i.original_name;
		auto name = format_sequence(original_name, template_name, seq);
		name = prop::replace_tokens(name, md, original_name, i.media_created);

		rename_item rename;
		rename.source = i.source;
		rename.is_folder = i.is_folder;
		rename.source_attributes = i.is_folder ? platform::file_attributes_t{} : platform::file_attributes(i.source);
		rename.policy = policy;
		rename.original_name = original_name;
		rename.new_name = name;
		rename.destination = rename_destination(i, name);

		const auto source_key = rename_path_key(rename.source, rename.is_folder);
		const auto destination_key = rename_path_key(rename.destination, rename.is_folder);
		rename.noop = source_key == destination_key;
		assign_sidecars(rename, i, rename.destination);

		// A case-only rename does not free the path it renames within, so it vacates nothing - but only
		// where the filesystem agrees the two spellings are one path.
		if (df::compare_path_key(source_key, destination_key) != 0)
		{
			vacated.emplace(source_key);

			for (const auto& [sidecar_source, sidecar_destination] : rename.sidecars)
				if (df::compare_path_key(sidecar_source.pack(), sidecar_destination.pack()) != 0)
					vacated.emplace(sidecar_source.pack());
		}

		name_is_usable.emplace_back(
			platform::is_valid_file_name(name) && rename.destination.is_save_valid() ? 1 : 0);
		seq += 1;
		results.emplace_back(std::move(rename));
	}

	// Resolve the collision with the one named policy before validity is decided.
	for (auto index = 0_z; index < results.size(); ++index)
	{
		auto& rename = results[index];
		const auto& i = items[index];
		const auto source_key = rename_path_key(rename.source, rename.is_folder);

		auto group_collides = [&]
		{
			const auto destination_key = rename_path_key(rename.destination, rename.is_folder);
			rename.destination_exists = rename_path_exists(rename.destination, rename.is_folder);
			rename.destination_attributes = rename.is_folder
				                                ? platform::file_attributes_t{}
				                                : platform::file_attributes(rename.destination);

			if (df::compare_path_key(source_key, destination_key) != 0 && rename.destination_exists &&
				!vacated.contains(destination_key))
				return true;

			for (auto sidecar = 0_z; sidecar < rename.sidecars.size(); ++sidecar)
			{
				const auto& [sidecar_source, sidecar_destination] = rename.sidecars[sidecar];
				if (df::compare_path_key(sidecar_source.pack(), sidecar_destination.pack()) != 0 &&
					rename.sidecar_destinations_exist[sidecar] &&
					!vacated.contains(sidecar_destination.pack()))
					return true;
			}
			return false;
		};

		rename.collides = group_collides();

		if (rename.collides && policy == collision_policy::auto_rename)
		{
			const auto base_name = rename.new_name;
			const auto base_destination = rename.destination;

			for (auto suffix = 2; suffix < 10000; ++suffix)
			{
				const auto candidate = std::format("{} ({})", base_name, suffix);
				rename.destination = df::file_path(rename.destination.folder(), candidate,
				                                   rename.destination.extension());
				assign_sidecars(rename, i, rename.destination);

				if (!group_collides())
				{
					rename.new_name = std::string(rename.destination.file_name_without_extension());
					rename.renamed_to_avoid_collision = true;
					rename.collides = false;
					break;
				}
			}

			// Exhausted: the row stays invalid, but it must not keep pointing at the last candidate.
			// destination feeds the duplicate-destination pass, so a name this row will never write
			// would invalidate whichever row legitimately asked for it.
			if (rename.collides)
			{
				rename.destination = base_destination;
				assign_sidecars(rename, i, rename.destination);
			}
		}
		else if (rename.collides && policy == collision_policy::skip)
		{
			rename.skipped = true;
			rename.noop = true;
			// Resolved, so the row must not keep the whole batch invalid. The duplicate-destination
			// pass below still re-sets it for collisions no policy can resolve.
			rename.collides = false;
		}

		rename.valid = name_is_usable[index] != 0 && (!rename.collides || policy == collision_policy::replace);
	}

	if (policy == collision_policy::skip)
	{
		// Skip resolves a collision by leaving the source where it is, so it no longer frees that
		// path for whatever row was cleared to take it, and settling that can cascade.
		std::map<std::string, size_t, df::path_key_less> wanted;

		for (auto index = 0_z; index < results.size(); ++index)
		{
			wanted.emplace(rename_path_key(results[index].destination, results[index].is_folder), index);
			for (const auto& sidecar : results[index].sidecars) wanted.emplace(sidecar.second.pack(), index);
		}

		std::vector<size_t> stalled;
		for (auto index = 0_z; index < results.size(); ++index)
			if (results[index].skipped) stalled.emplace_back(index);

		while (!stalled.empty())
		{
			const auto& stalled_row = results[stalled.back()];
			stalled.pop_back();

			std::vector<std::string> released{rename_path_key(stalled_row.source, stalled_row.is_folder)};
			for (const auto& sidecar : stalled_row.sidecars) released.emplace_back(sidecar.first.pack());

			for (const auto& key : released)
			{
				if (vacated.erase(key) == 0) continue;

				const auto found = wanted.find(key);
				if (found == wanted.end()) continue;

				auto& blocked = results[found->second];
				if (blocked.noop) continue;

				blocked.skipped = true;
				blocked.noop = true;
				stalled.emplace_back(found->second);
			}
		}
	}

	// Two rows targeting the same new name is a collision the policy cannot resolve
	// safely without reordering, so it always blocks.
	std::map<std::string, int, df::path_key_less> destination_counts;

	for (const auto& rename : results)
	{
		++destination_counts[rename_path_key(rename.destination, rename.is_folder)];
		for (const auto& sidecar : rename.sidecars) ++destination_counts[sidecar.second.pack()];
	}

	for (auto& rename : results)
	{
		const auto duplicates = destination_counts[rename_path_key(rename.destination, rename.is_folder)] > 1 ||
			std::ranges::any_of(rename.sidecars, [&](const auto& sidecar)
			{
				return destination_counts[sidecar.second.pack()] > 1;
			});
		if (duplicates)
		{
			rename.valid = false;
			rename.collides = true;
			rename.skipped = false;
			rename.renamed_to_avoid_collision = false;
		}
	}

	return results;
}

std::vector<rename_item> calc_item_renames(const df::item_set& items, const std::string_view template_name,
                                           const int start, const collision_policy policy)
{
	return calc_item_renames(snapshot_rename_sources(items), template_name, start, policy);
}

bool can_rename_items(const std::vector<rename_item>& renames)
{
	return !renames.empty() && std::ranges::all_of(renames, [](const rename_item& item) { return item.valid; }) &&
		std::ranges::any_of(renames, [](const rename_item& item) { return !item.noop; });
}

int count_rename_collisions(const std::vector<rename_item>& renames, const collision_policy policy)
{
	const auto count = [&renames](auto&& predicate)
	{
		return static_cast<int>(std::ranges::count_if(renames, predicate));
	};

	switch (policy)
	{
	case collision_policy::skip: return count([](const rename_item& item) { return item.skipped; });
	case collision_policy::auto_rename: return count([](const rename_item& item)
		{
			return item.renamed_to_avoid_collision;
		});
	case collision_policy::replace:
	case collision_policy::block_run: break;
	}

	return count([](const rename_item& item) { return item.collides; });
}

std::string format_sequence(const std::string_view original_name, const std::string_view template_name,
                            const int seq)

{
	static auto numbers = "0123456789";
	std::string result;

	if (!template_name.empty())
	{
		const auto reverse_template = std::string(template_name.rbegin(), template_name.rend());
		const auto reverse_name = std::string(original_name.rbegin(), original_name.rend());

		const auto org_len = reverse_name.size();
		auto i_div = seq;
		auto original_index = 0u;
		const auto sequence_digits = static_cast<int>(
			std::count(reverse_template.begin(), reverse_template.end(), '#'));
		auto sequence_placeholders = sequence_digits;

		for (const auto c : reverse_template)
		{
			switch (c)
			{
			case L'#':
				result.push_back(numbers[i_div % 10]);
				i_div /= 10;
				--sequence_placeholders;
				if (sequence_placeholders == 0)
				{
					while (i_div > 0)
					{
						result.push_back(numbers[i_div % 10]);
						i_div /= 10;
					}
				}
				break;
			case L'?':
				result.push_back(org_len > original_index ? reverse_name[original_index] : ' ');
				++original_index;
				break;
			default:
				result.push_back(c);
				break;
			}
		}
	}

	return std::string(result.rbegin(), result.rend());
}

bool unchanged_since_analysis(const df::file_path path, const uint64_t modified, const uint64_t size)
{
	const auto current = platform::file_attributes(path);
	return current.exists() && current.modified == modified && current.size == size;
}

import_analysis_result import_analysis(const std::vector<folder_scan_item>& src_items,
                                       const import_options& options, const item_import_set& previous_imported,
                                       const df::cancel_token& token)
{
	import_analysis_result result;
	const auto now = platform::now();

	// Destinations already claimed by earlier items in this plan. Two sources can resolve to the
	// same destination (the same file name in different source folders), which the file system
	// cannot report because nothing has been written yet. Without this, Replace would silently
	// discard one source and Auto-rename would hand both the same free name.
	df::unique_paths planned_destinations;

	df::unique_paths attached_sidecars;
	for (const auto& item : src_items)
	{
		if (token.is_cancelled()) return result;
		if (const auto md = item.item.metadata.load())
			for (const auto& name : split(md->sidecars, true))
			{
				const auto sidecar = item.folder.combine_file(name);
				if (sidecar != item.folder.combine_file(item.item.name)) attached_sidecars.emplace(sidecar);
			}
	}
	df::hash_map<df::file_path, df::file_path, df::ihash, df::ieq> sidecar_owners;

	for (const auto& i : src_items)
	{
		if (token.is_cancelled()) break;

		const auto md = i.item.metadata.load();

		df::date_t created;

		if (md)
		{
			created = md->created();
		}

		const auto import_folder_out = replace_tokens(options.dest_structure, md, i.item.name, i.item.created());
		const auto path_in = i.folder.combine_file(i.item.name);
		if (attached_sidecars.contains(path_in)) continue;
		auto path_out = options.dest_folder.combine(import_folder_out).combine_file(path_in.name());
		const auto import_rec = item_import{i.item.name, i.item.file_modified, i.item.size, now};
		std::vector<df::file_path> sidecar_paths_in;
		if (md)
		{
			for (const auto& file_name : split(md->sidecars, true))
			{
				const auto sidecar_path_in = i.folder.combine_file(file_name);
				if (sidecar_path_in != path_in &&
					std::ranges::find(sidecar_paths_in, sidecar_path_in) == sidecar_paths_in.end())
					sidecar_paths_in.emplace_back(sidecar_path_in);
			}
		}

		auto sidecar_destination = [](const df::file_path primary, const df::file_path sidecar)
		{
			return primary.folder().combine_file(primary.file_name_without_extension()).extension(sidecar.extension());
		};
		auto group_exists = [&](const df::file_path primary)
		{
			if (primary.exists() || planned_destinations.contains(primary)) return true;
			return std::ranges::any_of(sidecar_paths_in,
			                           [&](const auto sidecar)
			                           {
				                           const auto destination = sidecar_destination(primary, sidecar);
				                           const auto owner = sidecar_owners.find(destination);
				                           return destination.exists() ||
					                           (planned_destinations.contains(destination) &&
						                           (owner == sidecar_owners.end() || owner->second != sidecar));
			                           });
		};

		const bool already_exists = group_exists(path_out);

		// One explicit collision policy decides what happens when the destination exists.
		// Auto-rename resolves the whole media/sidecar group with one shared suffix.
		auto blocked_by_collision = false;

		if (already_exists)
		{
			switch (options.collision)
			{
			case collision_policy::auto_rename:
				for (auto suffix = 2; suffix < 10000; ++suffix)
				{
					const auto candidate = df::file_path(path_out.folder(),
					                                     std::format("{} ({})", path_out.file_name_without_extension(),
					                                                 suffix), path_out.extension());
					if (!group_exists(candidate))
					{
						path_out = candidate;
						break;
					}
				}
				break;
			case collision_policy::replace:
				break;
			case collision_policy::skip:
			case collision_policy::block_run:
				blocked_by_collision = true;
				break;
			}
		}

		const auto action = previous_imported.contains(import_rec) ? import_action::already_imported :
			blocked_by_collision ? import_action::already_exists : import_action::import;
		const auto importing = action == import_action::import;
		std::vector<import_analysis_item::sidecar_move> sidecars;
		sidecars.reserve(sidecar_paths_in.size());

		for (const auto sidecar_path_in : sidecar_paths_in)
		{
			const auto sidecar_path_out = sidecar_destination(path_out, sidecar_path_in);
			sidecars.emplace_back(sidecar_path_in, sidecar_path_out,
			                      importing ? platform::file_attributes(sidecar_path_in) : platform::file_attributes_t{},
			                      importing && options.collision == collision_policy::replace
				                      ? platform::file_attributes(sidecar_path_out)
				                      : platform::file_attributes_t{});
			if (importing)
			{
				planned_destinations.emplace(sidecar_path_out);
				sidecar_owners.emplace(sidecar_path_out, sidecar_path_in);
			}
		}

		// Ignored rows retain their dependencies, so Move cannot remove a companion they still need.
		const auto destination_fi = importing && options.collision == collision_policy::replace
			                            ? platform::file_attributes(path_out)
			                            : platform::file_attributes_t{};

		result[path_out.folder()].emplace_back(path_in, path_out, action, created, import_rec,
		                                       already_exists, import_folder_out,
		                                       importing ? platform::file_attributes(path_in) : platform::file_attributes_t{},
		                                       destination_fi, std::move(sidecars));
		if (importing) planned_destinations.emplace(path_out);
	}

	return result;
}

import_result import_copy(index_state& index, df::results_ptr results, const import_analysis_result& src_items,
                          const import_options& options, df::cancel_token token)
{
	import_result result;
	result_scope rr(results);

	std::vector<folder_scan_item> existing;
	std::vector<folder_scan_item> previous;
	df::unique_folders write_folders;
	struct import_group_state
	{
		bool attempted = false;
		bool success = true;
	};
	df::hash_map<item_import, import_group_state, item_import_hash, item_import_eq> import_states;
	struct published_sidecar
	{
		df::file_path source;
		platform::file_attributes_t attributes;
	};
	df::hash_map<df::file_path, published_sidecar, df::ihash, df::ieq> published_sidecars;
	df::hash_map<df::file_path, size_t, df::ihash, df::ieq> sidecar_consumers;
	std::string sidecar_cleanup_error;
	// Set when a destination folder cannot be made. The run stops there, but everything already
	// written still has to be recorded and rescanned below - returning from the middle of the walk
	// lost both, so a later run imported those files again and the index never saw them.
	std::string abort_message;

	for (const auto& ff_dest : src_items)
	{
		for (const auto& item : ff_dest.second)
		{
			if (item.action == import_action::import) import_states.try_emplace(item.import_rec);
			if (options.is_move)
				for (const auto& sidecar : item.sidecars) ++sidecar_consumers[sidecar.source];
		}
	}

	for (const auto& ff_dest : src_items)
	{
		const auto folder_out = ff_dest.first;
		if (token.is_cancelled())
		{
			for (const auto& item : ff_dest.second)
			{
				if (item.action == import_action::import)
				{
					import_states[item.import_rec].success = false;
					results->start_item(item.source.name());
					results->end_item(item.source.name(), item_status::cancel);
				}
				else
				{
					results->end_item(item.source.name(), item_status::ignore);
				}
			}
			continue;
		}

		// Nothing is written here when every item was already imported or resolved away by the
		// collision policy, so do not create a destination folder that would stay empty.
		const auto has_imports = std::ranges::any_of(ff_dest.second,
		                                             [](const auto& item)
		                                             {
			                                             return item.action == import_action::import;
		                                             });

		if (has_imports)
		{
			const bool folder_out_exists = folder_out.exists();
			const auto create_folder_result = folder_out_exists
				                                  ? platform::file_op_result{platform::file_op_result_code::OK, {}, {}}
				                                  : platform::create_folder(folder_out);

			if (create_folder_result.failed())
			{
				abort_message = create_folder_result.format_error(
					str_format(tt.error_create_folder_failed_fmt.sv(), folder_out));
				break;
			}

			write_folders.emplace(folder_out);
		}

		for (const auto& i : ff_dest.second)
		{
			// Every reviewed row is accounted for in the results, so a file that was skipped or
			// already imported is stated as ignored rather than dropping out of the list.
			if (i.action != import_action::import)
			{
				results->end_item(i.source.name(), item_status::ignore);
				continue;
			}

			if (results->is_canceled())
			{
				import_states[i.import_rec].success = false;
				results->start_item(i.source.name());
				results->end_item(i.source.name(), item_status::cancel);
				continue;
			}

			results->start_item(i.source.name());

			const auto path_in = i.source;
			const auto path_out = i.destination;

			// Only the Replace policy is allowed to write over an existing destination.
			const auto fail_if_exists = options.collision != collision_policy::replace;

			// ...and only over the destination the review actually saw. A Replace row whose destination
			// was free at analysis is not a reviewed replacement, so the write itself has to prove the
			// path is still free rather than overwriting whatever appeared since.
			const auto primary_fail_if_exists = fail_if_exists || !i.destination_fi.exists();

			// The reviewed row named specific files. A source that no longer matches would import
			// content nobody approved, and under Replace a changed destination would be written over
			// without ever having been reviewed. Neither is recoverable once the write starts, and
			// the sidecars are held to it too - they travel as part of the group, so a group that is
			// no longer what was reviewed cannot be written at either end.
			const auto reviewed_source = [](const df::file_path path, const platform::file_attributes_t& reviewed)
			{
				return unchanged_since_analysis(path, reviewed.modified, reviewed.size);
			};

			const auto reviewed_destination = [fail_if_exists](const df::file_path path,
			                                                   const platform::file_attributes_t& reviewed)
			{
				return fail_if_exists || !reviewed.exists() ||
					unchanged_since_analysis(path, reviewed.modified, reviewed.size);
			};

			const auto source_unchanged = reviewed_source(path_in, i.source_fi) &&
				std::ranges::all_of(i.sidecars, [&reviewed_source](const import_analysis_item::sidecar_move& sc)
				{
					return reviewed_source(sc.source, sc.source_fi);
				});

			const auto destination_unchanged = reviewed_destination(path_out, i.destination_fi) &&
				std::ranges::all_of(i.sidecars, [&](const import_analysis_item::sidecar_move& sc)
				{
					if (const auto found = published_sidecars.find(sc.destination); found != published_sidecars.end())
						return found->second.source == sc.source &&
							reviewed_source(sc.destination, found->second.attributes);
					return reviewed_destination(sc.destination, sc.destination_fi);
				});

			if (!source_unchanged || !destination_unchanged)
			{
				auto& state = import_states[i.import_rec];
				state.attempted = true;
				state.success = false;
				results->end_item(i.source.name(), item_status::fail);
				++result.refused;
				continue;
			}

			struct sidecar_undo
			{
				df::file_path source;
				df::file_path destination;
				df::file_path rollback;
			};

			// Copy companions before moving a primary. A shared source stays until every dependent
			// primary has moved; a failed row only undoes companion destinations it created itself.
			auto undo_sidecar = [&](const sidecar_undo& undo)
			{
				write_folders.emplace(undo.source.folder());
				write_folders.emplace(undo.destination.folder());

				platform::file_op_result undo_result{platform::file_op_result_code::OK, {}, {}};
				if (!undo.rollback.is_empty())
				{
					platform::make_file_writable(undo.destination);
					undo_result = platform::replace_file(undo.destination, undo.rollback, false);
					if (undo_result.failed())
					{
						df::log(__FUNCTION__, std::format("could not restore import sidecar {}; original retained as {}",
						                                  undo.destination.str(), undo.rollback.str()));
					}
				}
				else
				{
					undo_result = delete_temp_file(undo.destination);
				}

				return undo_result;
			};
			const auto restore_replaced_sidecar_if_changed = [&](const sidecar_undo& undo,
			                                                     const platform::file_attributes_t& reviewed)
			{
				if (undo.rollback.is_empty() ||
					unchanged_since_analysis(undo.destination, reviewed.modified, reviewed.size))
				{
					return delete_temp_file(undo.rollback);
				}

				platform::make_file_writable(undo.destination);
				const auto restore_result = platform::replace_file(undo.destination, undo.rollback, false);
				if (restore_result.failed())
				{
					df::log(__FUNCTION__, std::format("could not restore import sidecar {}; original retained as {}",
					                                  undo.destination.str(), undo.rollback.str()));
				}
				return restore_result;
			};

			std::vector<sidecar_undo> moved_sidecars;
			auto move_or_copy_result = platform::file_op_result{platform::file_op_result_code::OK, {}, {}};

			for (const auto& sidecar : i.sidecars)
			{
				if (published_sidecars.contains(sidecar.destination)) continue;
				// The same rule the primary takes: only a destination the review actually saw is a
				// reviewed replacement, so anything that appeared since has to stop the write.
				const auto sidecar_fail_if_exists = fail_if_exists || !sidecar.destination_fi.exists();
				sidecar_undo undo{sidecar.source, sidecar.destination};

				if (!sidecar_fail_if_exists)
				{
					undo.rollback = platform::temp_file(sidecar.destination.extension(), sidecar.destination.folder());
					move_or_copy_result = platform::copy_file(sidecar.destination, undo.rollback, true, false);
					if (move_or_copy_result.failed())
					{
						delete_temp_file(undo.rollback);
						break;
					}
				}

				move_or_copy_result = move_or_copy(sidecar.source, sidecar.destination, false,
				                                   sidecar_fail_if_exists);
				if (move_or_copy_result.success() && test_import_sidecar_write_override)
				{
					if (auto overridden = test_import_sidecar_write_override(sidecar.destination))
					{
						move_or_copy_result = std::move(*overridden);
					}
				}
				if (move_or_copy_result.failed())
				{
					if (!undo.rollback.is_empty())
					{
						restore_replaced_sidecar_if_changed(undo, sidecar.destination_fi);
					}
					break;
				}
				moved_sidecars.emplace_back(std::move(undo));
			}

			if (move_or_copy_result.success())
			{
				move_or_copy_result = move_or_copy(path_in, path_out, options.is_move, primary_fail_if_exists);
			}

			if (move_or_copy_result.failed())
			{
				for (auto s = moved_sidecars.rbegin(); s != moved_sidecars.rend(); ++s)
				{
					const auto undo_result = undo_sidecar(*s);
					if (undo_result.failed()) move_or_copy_result = undo_result;
				}
			}
			else
			{
				for (const auto& undo : moved_sidecars)
				{
					if (!undo.rollback.is_empty())
					{
						if (test_before_import_rollback_cleanup) test_before_import_rollback_cleanup(undo.rollback);
						const auto cleanup_result = delete_temp_file(undo.rollback);
						(void)cleanup_result;
					}
				}

				for (const auto& sidecar : i.sidecars)
				{
					if (!published_sidecars.contains(sidecar.destination))
						published_sidecars.emplace(sidecar.destination,
						                          published_sidecar{sidecar.source,
							                          platform::file_attributes(sidecar.destination)});
				}

				if (options.is_move && path_in != path_out)
					for (const auto& sidecar : i.sidecars)
					{
						if (--sidecar_consumers.at(sidecar.source) != 0 ||
							published_sidecars.contains(sidecar.source)) continue;

						write_folders.emplace(sidecar.source.folder());
						auto cleanup = platform::file_op_result{platform::file_op_result_code::ALREADY_EXISTS};
						if (reviewed_source(sidecar.source, sidecar.source_fi))
						{
							platform::make_file_writable(sidecar.source);
							cleanup = platform::delete_file(sidecar.source);
						}
						if (cleanup.failed())
						{
							if (move_or_copy_result.success()) move_or_copy_result = cleanup;
							const auto message = std::format("Could not remove imported sidecar source {}: {}",
							                                 sidecar.source.str(), cleanup.format_error());
							df::log(__FUNCTION__, message);
							if (sidecar_cleanup_error.empty()) sidecar_cleanup_error = message;
						}
					}
			}

			if (move_or_copy_result.success())
			{
				import_states[i.import_rec].attempted = true;
				if (options.set_created_date && i.created_date.is_valid())
				{
					// The file arrived, so this is not a failed import - but the date was asked for
					// and the filesystem refused it, and a row reported as plain success would say
					// the opposite. Counted so the run can state it.
					if (!platform::created_date(path_out, i.created_date))
					{
						df::log(__FUNCTION__, std::format("could not set created date on {}", path_out.str()));
						++result.dates_unset;
					}
				}

				// An import that moves empties the folder it took from, which is only in the index
				// when importing from one collection folder to another.
				if (options.is_move) write_folders.emplace(path_in.folder());

				result.folder = path_out.folder();
			}
			else
			{
				auto& state = import_states[i.import_rec];
				state.attempted = true;
				state.success = false;
				// A destination that appeared after the review is a file that changed, not a disk error.
				if (move_or_copy_result.code == platform::file_op_result_code::ALREADY_EXISTS) ++result.refused;
			}

			results->end_item(i.source.name(), to_status(move_or_copy_result.code));
		}
	}

	for (const auto& [import_rec, state] : import_states)
	{
		if (state.attempted && state.success) result.imports.emplace(import_rec);
	}

	index.queue_scan_folders(write_folders);

	if (!abort_message.empty())
	{
		results->abort(abort_message);
		return result;
	}

	std::string result_text = sidecar_cleanup_error;

	if (!existing.empty())
	{
		result_text += format_plural_text(tt.ignored_exist_already_fmt, existing.front().item.name,
		                                  static_cast<int>(existing.size()), {},
		                                  static_cast<int>(src_items.size()));
	}

	if (!previous.empty())
	{
		if (!result_text.empty()) result_text += "\n\n";
		result_text += format_plural_text(tt.ignored_previous_fmt, previous.front().item.name,
		                                  static_cast<int>(previous.size()), {},
		                                  static_cast<int>(src_items.size()));
	}

	if (result.dates_unset > 0)
	{
		if (!result_text.empty()) result_text += "\n\n";
		result_text += format_plural_text(tt.import_date_unset_fmt, result.dates_unset);
	}

	// A refused row is a file that moved on since the review, not a disk error, and the two are
	// indistinguishable from the row alone. Said here because this is the completion the run
	// shows: the first one reported is the only one, so a caller adding it afterwards was ignored.
	if (result.refused > 0)
	{
		if (!result_text.empty()) result_text += "\n\n";
		result_text += tt.sync_analysis_changed.sv();
	}

	rr.complete(result_text);

	return result;
}

size_t count_imports(const std::vector<import_analysis_item>& items)
{
	size_t result = 0;

	for (const auto& i : items)
	{
		if (i.action == import_action::import)
		{
			++result;
		}
	}

	return result;
}

size_t count_imports(const import_analysis_result& items)
{
	size_t result = 0;

	for (const auto& i : items)
	{
		result += count_imports(i.second);
	}

	return result;
}

size_t count_import_collisions(const import_analysis_result& items)
{
	size_t result = 0;
	for (const auto& [folder, folder_items] : items)
	{
		result += std::ranges::count_if(folder_items, [](const auto& item) { return item.already_exists; });
	}
	return result;
}

size_t count_import_colliding_writes(const import_analysis_result& items)
{
	size_t result = 0;
	for (const auto& [folder, folder_items] : items)
	{
		result += std::ranges::count_if(folder_items, [](const auto& item)
		{
			return item.already_exists && item.action == import_action::import;
		});
	}
	return result;
}

std::optional<import_source> calc_import_selected_source(const view_state& s)
{
	if (s.has_selection())
	{
		import_source source;
		source.text = str_format(tt.selected_items_fmt.sv(),
		                         format_plural_text(tt.title_item_count_fmt, s.selected_items()));
		source.items = s.selected_items();
		return source;
	}

	return {};
}

std::vector<import_source> calc_import_sources(std::optional<import_source> selected_source)
{
	std::vector<import_source> result;

	if (selected_source)
	{
		result.emplace_back(std::move(*selected_source));
	}

	const auto onedrive_camera_roll = known_path(platform::known_folder::onedrive_camera_roll);

	if (onedrive_camera_roll.exists())
	{
		result.emplace_back(std::string(onedrive_camera_roll.text()), onedrive_camera_roll);
	}

	auto drives = platform::scan_drives();
	constexpr size_t drive_max = 5;

	for (const auto& d : import_source_drives(drives, drive_max))
	{
		const auto text = std::format("{} {} {}", d.name, d.vol_name, d.used);
		result.emplace_back(text, df::folder_path(d.name));
	}

	return result;
}

platform::drives import_source_drives(const platform::drives& drives, const size_t max)
{
	platform::drives result;

	for (const auto& d : drives)
	{
		if (result.size() >= max) break;

		// A card-reader slot with no card in it reports no capacity.
		if (d.type == platform::drive_type::removable && d.capacity.is_valid())
		{
			result.emplace_back(d);
		}
	}

	return result;
}


std::string relative_combine(const std::string& relative, const str::cached name)
{
	auto result = relative;

	if (!result.empty() && result.back() != '\\')
	{
		result += '\\';
	}

	result += name;
	return result;
}

// FAT32 keeps a modified time to two seconds and exFAT to ten milliseconds, so a file copied onto a USB
// drive reads back a little older or newer than the file it came from. Compared exactly, that copy looks
// changed again and is copied again on every Sync. Times are FILETIME ticks of 100 ns.
constexpr uint64_t sync_same_time_ticks = 2 * 10'000'000ull;

sync_action calc_sync_action(const bool local_exists, const bool remote_exists,
                             const uint64_t local_modified, const uint64_t remote_modified,
                             const uint64_t local_size, const uint64_t remote_size,
                             const bool sync_local_remote, const bool sync_remote_local,
                             const bool sync_delete_local, const bool sync_delete_remote)
{
	if (local_exists && remote_exists)
	{
		const auto difference = local_modified > remote_modified
			                        ? local_modified - remote_modified
			                        : remote_modified - local_modified;
		const auto same_time = difference <= sync_same_time_ticks;

		if (!same_time && local_modified > remote_modified && sync_local_remote) return sync_action::copy_remote;
		if (!same_time && remote_modified > local_modified && sync_remote_local) return sync_action::copy_local;
		if (same_time && local_size != remote_size)
		{
			if (sync_local_remote && !sync_remote_local) return sync_action::copy_remote;
			if (sync_remote_local && !sync_local_remote) return sync_action::copy_local;
		}
	}
	else if (local_exists)
	{
		if (sync_local_remote) return sync_action::copy_remote;
		if (sync_delete_local) return sync_action::delete_local;
	}
	else if (remote_exists)
	{
		if (sync_remote_local) return sync_action::copy_local;
		if (sync_delete_remote) return sync_action::delete_remote;
	}

	return sync_action::none;
}

static bool path_contains(const df::folder_path parent, const df::folder_path child)
{
	return df::folder_contains(parent.text().sv(), child.text().sv());
}

static std::optional<std::string> relative_folder(const df::folder_path root, const df::folder_path path)
{
	if (root == path) return std::string{};
	if (!path_contains(root, path)) return {};

	auto result = std::string(path.text().sv().substr(root.text().sv().size()));

	while (!result.empty() && (result.front() == '\\' || result.front() == '/'))
	{
		result.erase(result.begin());
	}

	return result;
}

std::string sync_invalid_message(const sync_analysis_result& analysis)
{
	if (analysis.valid) return {};

	switch (analysis.reason)
	{
	case sync_invalid_reason::folder_unreadable:
		return str_format(tt.is_not_valid_folder_fmt.sv(), analysis.invalid_folder.text());
	case sync_invalid_reason::empty_paths:
	case sync_invalid_reason::overlapping_paths:
	case sync_invalid_reason::ambiguous_local_root:
		return std::string(tt.platform_text.error_invalid_files.sv());
	case sync_invalid_reason::none:
		break;
	}

	return std::string(tt.platform_text.error_cannot_continue.sv());
}

sync_analysis_result sync_analysis(const df::index_roots& local_roots, const df::folder_path remote_path,
                                   const bool sync_local_remote, const bool sync_remote_local,
                                   const bool sync_delete_local, const bool sync_delete_remote,
                                   const df::cancel_token& token)
{
	sync_analysis_result result;
	if (remote_path.is_empty() || local_roots.folders.empty())
	{
		result.valid = false;
		result.reason = sync_invalid_reason::empty_paths;
		return result;
	}

	for (const auto& local_root : local_roots.folders)
	{
		if (path_contains(local_root, remote_path) || path_contains(remote_path, local_root))
		{
			result.valid = false;
			result.reason = sync_invalid_reason::overlapping_paths;
			result.invalid_folder = remote_path;
			return result;
		}
	}

	// Which local root owns each relative folder, and the relative folders claimed by more than one
	// root. A relative folder claimed twice cannot name a local destination for a remote-only file.
	std::map<std::string, df::folder_path, df::path_key_less> local_roots_by_relative;
	std::set<std::string, df::path_key_less> ambiguous_relatives;
	struct relative_root_claim
	{
		df::unique_folders roots;
		df::unique_folders excluded_roots;
	};
	std::map<std::string, relative_root_claim, df::path_key_less> local_root_claims_by_relative;

	for (const auto& root : local_roots.folders)
	{
		for (const auto& exclude : local_roots.excludes)
		{
			const auto relative = relative_folder(root, exclude);
			if (!relative) continue;
			auto& claim = local_root_claims_by_relative[*relative];
			claim.roots.emplace(root);
			claim.excluded_roots.emplace(root);
		}
	}

	std::vector<sync_analysis_folder> local_folders_to_scan;

	for (const auto& f : local_roots.folders)
	{
		local_folders_to_scan.emplace_back(f, f);
	}

	while (!local_folders_to_scan.empty())
	{
		if (token.is_cancelled())
		{
			result.clear();
			result.valid = false;
			return result;
		}

		const auto folder = local_folders_to_scan.back();
		local_folders_to_scan.pop_back();

		if (!is_excluded(local_roots, folder.path))
		{
			const auto contents = platform::iterate_file_items(folder.path, setting.show_hidden);
			if (!contents.success)
			{
				result.clear();
				result.valid = false;
				result.reason = sync_invalid_reason::folder_unreadable;
				result.invalid_folder = folder.path;
				return result;
			}

			for (const auto& file : contents.files)
			{
				if (token.is_cancelled()) break;

				auto& i = result[folder.relative][file.name.str()];
				if (!i.local_path.is_empty() && i.local_root != folder.root)
				{
					result.clear();
					result.valid = false;
					result.reason = sync_invalid_reason::ambiguous_local_root;
					result.invalid_folder = folder.path;
					return result;
				}
				i.local_path = folder.path.combine_file(file.name);
				i.local_fi = file;
				i.local_root = folder.root;
			}

			for (const auto& sub_folder : contents.folders)
			{
				auto relative = relative_combine(folder.relative, sub_folder.name);
				auto sub_folder_path = folder.path.combine(sub_folder.name);
				local_root_claims_by_relative[relative].roots.emplace(folder.root);
				sync_analysis_folder unknown = {sub_folder_path, folder.root, relative};
				local_folders_to_scan.emplace_back(unknown);

				const auto claimed = local_roots_by_relative.find(relative);

				if (claimed == local_roots_by_relative.end())
				{
					local_roots_by_relative[relative] = folder.root;
				}
				else if (claimed->second != folder.root)
				{
					ambiguous_relatives.emplace(relative);
				}
			}
		}
	}

	std::vector<sync_analysis_folder> remote_folders_to_scan;
	remote_folders_to_scan.emplace_back(remote_path, remote_path);

	const auto local_counterpart = [&](const std::string& relative)
	{
		const auto found_relative = local_roots_by_relative.find(relative);

		if (found_relative != local_roots_by_relative.end() && !ambiguous_relatives.contains(relative))
		{
			return found_relative->second.combine(relative);
		}

		if (local_roots.folders.size() == 1)
		{
			return local_roots.folders.begin()->combine(relative);
		}

		return df::folder_path{};
	};

	enum class remote_exclusion_result
	{
		scan,
		skip,
		ambiguous,
	};

	const auto remote_exclusion = [&](const std::string& relative)
	{
		const auto found = local_root_claims_by_relative.find(relative);
		if (found == local_root_claims_by_relative.end()) return remote_exclusion_result::scan;

		const auto& claim = found->second;
		if (claim.roots.size() == claim.excluded_roots.size()) return remote_exclusion_result::skip;
		if (!claim.excluded_roots.empty()) return remote_exclusion_result::ambiguous;
		return remote_exclusion_result::scan;
	};

	while (!remote_folders_to_scan.empty())
	{
		if (token.is_cancelled())
		{
			result.clear();
			result.valid = false;
			return result;
		}

		const auto folder = remote_folders_to_scan.back();
		remote_folders_to_scan.pop_back();
		const auto counterpart = local_counterpart(folder.relative);
		const auto exclusion = remote_exclusion(folder.relative);

		if (exclusion == remote_exclusion_result::ambiguous)
		{
			result.clear();
			result.valid = false;
			result.reason = sync_invalid_reason::ambiguous_local_root;
			result.invalid_folder = folder.path;
			return result;
		}

		if (exclusion != remote_exclusion_result::skip &&
			!is_excluded(local_roots, folder.path) &&
			(counterpart.is_empty() || !is_excluded(local_roots, counterpart)))
		{
			const auto contents = platform::iterate_file_items(folder.path, setting.show_hidden);
			if (!contents.success)
			{
				result.clear();
				result.valid = false;
				result.reason = sync_invalid_reason::folder_unreadable;
				result.invalid_folder = folder.path;
				return result;
			}

			for (const auto& file : contents.files)
			{
				if (token.is_cancelled()) break;

				auto& i = result[folder.relative][file.name.str()];
				i.remote_path = folder.path.combine_file(file.name);
				i.remote_fi = file;
				i.remote_root = folder.root;
			}

			for (const auto& sub_folder : contents.folders)
			{
				sync_analysis_folder unknown = {
					folder.path.combine(sub_folder.name), remote_path,
					relative_combine(folder.relative, sub_folder.name)
				};
				remote_folders_to_scan.emplace_back(unknown);
			}
		}
	}

	for (auto&& i : result)
	{
		for (auto&& f : i.second)
		{
			if (token.is_cancelled())
			{
				result.clear();
				result.valid = false;
				return result;
			}

			const auto local_exists = !f.second.local_path.is_empty();
			const auto remote_exists = !f.second.remote_path.is_empty();

			if (f.second.local_root.is_empty())
			{
				// Keyed by the relative folder, not the file name. A remote-only file belongs to the
				// local root that owns its folder; with a single root there is only one answer.
				const auto found_relative = local_roots_by_relative.find(i.first);

				if (found_relative != local_roots_by_relative.end() && !ambiguous_relatives.contains(i.first))
				{
					f.second.local_root = found_relative->second;
				}
				else if (local_roots.folders.size() == 1)
				{
					f.second.local_root = *local_roots.folders.begin();
				}
			}

			const auto action = calc_sync_action(local_exists, remote_exists,
			                                     f.second.local_fi.attributes.modified,
			                                     f.second.remote_fi.attributes.modified,
			                                     f.second.local_fi.attributes.size,
			                                     f.second.remote_fi.attributes.size,
			                                     sync_local_remote, sync_remote_local, sync_delete_local,
			                                     sync_delete_remote);

			// Only copying a remote-only file into the collection needs a local destination. Reporting
			// a remote file the run would ignore as an invalid path fails a whole sync over nothing.
			if (f.second.local_root.is_empty() && action == sync_action::copy_local)
			{
				result.clear();
				result.valid = false;
				result.reason = sync_invalid_reason::ambiguous_local_root;
				return result;
			}

			if (f.second.remote_root.is_empty())
			{
				f.second.remote_root = remote_path;
			}

			if (f.second.local_path.is_empty() && !f.second.local_root.is_empty())
			{
				f.second.local_path = f.second.local_root.combine(i.first).combine_file(f.first);
			}

			if (f.second.remote_path.is_empty())
			{
				f.second.remote_path = f.second.remote_root.combine(i.first).combine_file(f.first);
			}

			f.second.action = action;

			if (f.second.action == sync_action::delete_local)
			{
				f.second.delete_crc = platform::file_crc32_checked(f.second.local_path, token);
			}
			else if (f.second.action == sync_action::delete_remote)
			{
				f.second.delete_crc = platform::file_crc32_checked(f.second.remote_path, token);
			}
		}
	}

	return result;
}

sync_run_result sync_copy(const df::results_ptr& status, const sync_analysis_result& analysis_result,
                          const df::cancel_token& token)
{
	sync_run_result result;

	for (const auto& i : analysis_result)
	{
		for (const auto& f : i.second)
		{
			const auto local_path = f.second.local_path;
			const auto remote_path = f.second.remote_path;
			const auto action = f.second.action;

			df::assert_true(!remote_path.is_empty());
			df::assert_true(!local_path.is_empty() || action == sync_action::none ||
				action == sync_action::delete_remote);

			// A remote-only file that no local root claims has no local name to report.
			const auto name = local_path.is_empty() ? remote_path.name() : local_path.name();

			// Every reviewed row is accounted for in the results, so a file the run left alone is
			// stated as ignored rather than disappearing from the list the user just approved.
			if (action == sync_action::none)
			{
				status->end_item(name, item_status::ignore);
				continue;
			}

			status->start_item(name);

			if (token.is_cancelled())
			{
				status->end_item(name, item_status::cancel);
				continue;
			}

			const auto& local_attributes = f.second.local_fi.attributes;
			const auto& remote_attributes = f.second.remote_fi.attributes;

			// The source is read immediately after this, so a source that no longer matches the
			// review would copy content nobody approved.
			const auto source_unchanged = [&]
			{
				return action == sync_action::copy_local
					       ? unchanged_since_analysis(remote_path, remote_attributes.modified, remote_attributes.size)
					       : unchanged_since_analysis(local_path, local_attributes.modified, local_attributes.size);
			};

			// A destination the review expected to be free is proved free by the write itself, which
			// leaves no window for it to appear. One that was already there has to still be the file
			// the user agreed to overwrite.
			const auto destination_unchanged = [&](const platform::file_attributes_t& reviewed,
			                                       const df::file_path path)
			{
				return !reviewed.exists() || unchanged_since_analysis(path, reviewed.modified, reviewed.size);
			};

			// Deletes were reviewed against the file's content, so that is what they are held to. An
			// unreadable file proves nothing about its content: the delete is refused rather than
			// allowed through on two reads that both failed and both answered zero.
			const auto delete_content_unchanged = [&](const df::file_path path)
			{
				const auto current = platform::file_crc32_checked(path, token);
				return current.has_value() && f.second.delete_crc.has_value() &&
					current.value() == f.second.delete_crc.value();
			};

			// A cancelled check proves nothing, so it is reported as the cancellation it was rather
			// than as a file that changed.
			const auto refused = [&](const bool checks_passed)
			{
				if (checks_passed) return false;
				const auto cancelled = token.is_cancelled();
				status->end_item(name, cancelled ? item_status::cancel : item_status::fail);
				if (!cancelled) ++result.refused;
				return true;
			};

			// A destination that appeared after the review is a file that changed, not a disk error.
			const auto write = [&](const platform::file_op_result& op)
			{
				if (op.code == platform::file_op_result_code::ALREADY_EXISTS) ++result.refused;
				status->end_item(name, to_status(op.code));
			};

			switch (action)
			{
			case sync_action::none:
				break;
			case sync_action::copy_local:
				if (refused(source_unchanged() && destination_unchanged(local_attributes, local_path))) break;
				write(platform::copy_file(remote_path, local_path, !local_attributes.exists(), true));
				result.folders_changed.emplace(local_path.folder());
				break;
			case sync_action::copy_remote:
				if (refused(source_unchanged() && destination_unchanged(remote_attributes, remote_path))) break;
				write(platform::copy_file(local_path, remote_path, !remote_attributes.exists(), true));
				break;
			case sync_action::delete_local:
				if (refused(unchanged_since_analysis(local_path, local_attributes.modified, local_attributes.size) &&
					delete_content_unchanged(local_path)))
					break;
				status->end_item(name, to_status(platform::delete_file(local_path).code));
				result.folders_changed.emplace(local_path.folder());
				break;
			case sync_action::delete_remote:
				if (refused(unchanged_since_analysis(remote_path, remote_attributes.modified, remote_attributes.size) &&
					delete_content_unchanged(remote_path)))
					break;
				status->end_item(name, to_status(platform::delete_file(remote_path).code));
				break;
			}
		}
	}

	return result;
}

void toggle_collection_entry(settings_t::index_t& collection_settings, const df::folder_path folder,
                             const bool is_remove)
{
	const auto local_folders = platform::local_folders();

	if (is_remove)
	{
		if (local_folders.pictures == folder) collection_settings.pictures = false;
		else if (local_folders.music == folder) collection_settings.music = false;
		else if (local_folders.video == folder) collection_settings.video = false;
		else if (local_folders.onedrive_pictures == folder) collection_settings.onedrive_pictures = false;
		else if (local_folders.onedrive_video == folder) collection_settings.onedrive_video = false;
		else if (local_folders.onedrive_music == folder) collection_settings.onedrive_music = false;
		else if (local_folders.dropbox_photos == folder) collection_settings.drop_box = false;

		std::string more_folders;

		for (const auto existing_folder_path : split_collection_folders(collection_settings.more_folders))
		{
			if (folder != df::folder_path(existing_folder_path))
			{
				if (!more_folders.empty()) more_folders += "\r\n";
				more_folders += existing_folder_path;
			}
		}

		collection_settings.more_folders = more_folders;
	}
	else
	{
		// add
		if (local_folders.pictures == folder) collection_settings.pictures = true;
		else if (local_folders.music == folder) collection_settings.music = true;
		else if (local_folders.video == folder) collection_settings.video = true;
		else if (local_folders.onedrive_pictures == folder) collection_settings.onedrive_pictures = true;
		else if (local_folders.onedrive_video == folder) collection_settings.onedrive_video = true;
		else if (local_folders.onedrive_music == folder) collection_settings.onedrive_music = true;
		else if (local_folders.dropbox_photos == folder) collection_settings.drop_box = true;
		else
		{
			if (!collection_settings.more_folders.empty()) collection_settings.more_folders += "\r\n";
			collection_settings.more_folders += folder.text();
		}
	}
}

std::vector<std::string> check_overwrite(const df::folder_path write_folder, const df::item_set& items,
                                         const std::string_view new_extension)
{
	// One enumeration of the destination rather than a query per item: the selection is unbounded and
	// this runs before a modal, where a round trip per item over a network share would stall the
	// window. A destination that cannot be read reports no collisions, which leaves the caller on its
	// non-overwriting default rather than on a guess.
	const auto contents = platform::iterate_file_items(write_folder, true);
	if (!contents.success) return {};

	std::set<std::string, df::path_key_less> existing;
	for (const auto& f : contents.files) existing.emplace(f.name.str());
	for (const auto& f : contents.folders) existing.emplace(f.name.str());

	std::vector<std::string> result;

	for (const auto f : items.folder_paths())
	{
		if (existing.contains(std::string(f.name()))) result.emplace_back(f.name());
	}

	for (const auto f : items.file_paths())
	{
		const auto dest = write_folder.combine_file(f.name()).extension(
			new_extension.empty() ? f.extension() : new_extension);

		if (existing.contains(std::string(dest.name()))) result.emplace_back(dest.name());
	}

	return result;
}

std::string format_collision_summary(const collision_policy policy, const int count)
{
	if (count <= 0) return {};

	const auto num = str::to_string(count);

	switch (policy)
	{
	case collision_policy::skip: return str_format(tt.collision_skipped_fmt.sv(), num);
	case collision_policy::replace: return str_format(tt.collision_replaced_fmt.sv(), num);
	case collision_policy::auto_rename: return str_format(tt.collision_renamed_fmt.sv(), num);
	case collision_policy::block_run: break;
	}

	return str_format(tt.collision_blocked_fmt.sv(), num);
}

df::file_path next_free_destination(const df::file_path destination)
{
	const auto folder = destination.folder();
	const auto base_name = destination.file_name_without_extension();
	const auto extension = destination.extension();

	// Bounded: give up rather than spin if a folder is saturated with variants.
	for (auto suffix = 2; suffix < 10000; ++suffix)
	{
		auto candidate = df::file_path(folder, std::format("{} ({})", base_name, suffix), extension);
		if (!candidate.exists()) return candidate;
	}

	return destination;
}

df::folder_path outgoing_staging_folder()
{
	return platform::temp_folder().combine("diffractor-share");
}

void clear_outgoing_staging(const df::folder_path staging)
{
	// Flat by construction: nothing but staged copies is written here. A copy a recipient still
	// holds open fails to delete, and the names given to the next share step around it.
	for (const auto& f : platform::iterate_file_items(staging, true).files)
	{
		platform::delete_file(df::file_path(f.folder, f.name));
	}
}

static df::file_path free_staging_path(const df::file_path path)
{
	return path.exists() ? next_free_destination(path) : path;
}

// A limit, never an enlargement. The scale edit fits a photo to the box whichever way that goes, so
// one already inside it is left alone. A photo whose size cannot be read is limited anyway.
static bool exceeds_max_side(files& codecs, const df::file_path path, const int max_side)
{
	const auto dimensions = codecs.scan_file(path, false, files::file_type_from_name(path)).dimensions();
	return dimensions.is_empty() || std::max(dimensions.cx, dimensions.cy) > max_side;
}

outgoing_files prepare_outgoing_files(const std::vector<df::file_path>& sources, const outgoing_options& options,
                                      const df::folder_path staging, df::status_i& status)
{
	outgoing_files result;

	if (options.writes_copies())
	{
		clear_outgoing_staging(staging);

		if (!staging.exists())
		{
			const auto created = platform::create_folder(staging);

			if (created.failed())
			{
				result.error = created.format_error();
				return result;
			}
		}
	}

	files codecs;
	df::zip_file zip;
	df::file_path zip_path;
	std::vector<df::file_path> zipped_copies;

	// Entry names are what the recipient extracts, so two files named alike must not share one.
	std::unordered_set<std::string> entry_names;
	const auto free_entry_name = [&entry_names](const df::file_path path)
	{
		auto name = path.name().str();

		for (auto suffix = 2; !entry_names.insert(str::to_lower(name)).second; ++suffix)
		{
			name = std::format("{} ({}){}", path.file_name_without_extension(), suffix, path.extension());
		}

		return name;
	};

	if (options.zip)
	{
		zip_path = free_staging_path(df::file_path(staging, "items.zip"));
		if (!zip.create(zip_path)) return result;
	}

	const auto total = static_cast<int64_t>(sources.size());
	int64_t pos = 0;

	for (const auto& source : sources)
	{
		if (status.is_canceled()) return result;

		status.message(str_format(tt.email_processing_fmt.sv(), source.name()), pos++, total);
		status.start_item(source.name());

		auto outgoing = source;
		const auto is_bitmap = files::file_type_from_name(source)->has_trait(file_traits::bitmap);
		const auto convert = is_bitmap && options.convert_to_jpeg && !files::is_jpeg(source.name());
		const auto shrink = is_bitmap && options.max_side > 0 && exceeds_max_side(codecs, source, options.max_side);

		if (convert || shrink)
		{
			const auto copy = free_staging_path(
				df::file_path(staging, source.extension(convert ? ".jpg"sv : source.extension()).name()));

			image_edits edits;
			if (shrink) edits.scale(options.max_side);

			const auto written = codecs.update(source, copy, {}, edits, make_file_encode_params(), false, {});

			if (written.failed())
			{
				status.end_item(source.name(), item_status::fail);
				result.error = written.format_error();
				return result;
			}

			outgoing = copy;
			if (options.zip) zipped_copies.emplace_back(copy);
		}

		if (options.zip)
		{
			if (!zip.add(outgoing, free_entry_name(outgoing)))
			{
				status.end_item(source.name(), item_status::fail);
				return result;
			}
		}
		else
		{
			result.paths.emplace_back(outgoing);
		}

		status.end_item(source.name(), item_status::success);
	}

	if (options.zip)
	{
		if (!zip.close()) return result;
		result.paths.emplace_back(zip_path);

		// The archive carries them now.
		for (const auto& copy : zipped_copies)
		{
			platform::delete_file(copy);
		}
	}

	result.success = true;
	return result;
}

std::vector<convert_source> snapshot_convert_sources(const df::item_set& items)
{
	std::vector<convert_source> result;
	result.reserve(items.size());

	for (const auto& item : items.items())
	{
		const auto md = item->metadata();
		result.emplace_back(item->path(), md ? md->dimensions() : sizei{}, item->xmp());
	}

	return result;
}

std::vector<convert_item_plan> plan_convert_outputs(const df::folder_path write_folder,
                                                    const std::vector<convert_source>& sources,
                                                    const std::string_view new_extension, const collision_policy policy)
{
	auto ordered = sources;
	std::ranges::sort(ordered, [](const auto& left, const auto& right)
	{
		return left.path.icmp(right.path) < 0;
	});

	df::hash_set<df::file_path, df::ihash, df::ieq> planned_paths;
	// A destination that names another row's source is a file this run reads. Writing it would
	// destroy an input the review listed as a source, and the later row would then convert whatever
	// the earlier one just wrote, so those paths are occupied whatever the collision policy says.
	// A row's own source is not: that is conversion in place, which is what the row asked for.
	df::hash_set<df::file_path, df::ihash, df::ieq> source_paths;

	for (const auto& source : ordered)
	{
		source_paths.emplace(source.path);
	}

	std::vector<convert_item_plan> result;
	result.reserve(ordered.size());

	for (const auto& source : ordered)
	{
		const auto taken = [&](const df::file_path candidate)
		{
			return planned_paths.contains(candidate) ||
				(source_paths.contains(candidate) && candidate != source.path);
		};

		const auto base_name = source.path.file_name_without_extension();
		auto destination = df::file_path(write_folder, base_name, new_extension);
		// Two sources can share a base name, so a destination another row already claimed is taken
		// even when nothing is on disk yet. Use the " (n)" variant every other operation uses.
		for (auto suffix = 2; taken(destination); ++suffix)
		{
			destination = df::file_path(write_folder, std::format("{} ({})", base_name, suffix), new_extension);
		}

		convert_item_plan plan;
		plan.source = source;
		plan.collides = destination.exists();

		if (plan.collides && policy == collision_policy::auto_rename)
		{
			auto renamed = next_free_destination(destination);
			for (auto suffix = 2; renamed.exists() || taken(renamed); ++suffix)
			{
				renamed = df::file_path(write_folder, std::format("{} ({})", base_name, suffix), new_extension);
			}
			destination = renamed;
			plan.renamed_to_avoid_collision = true;
		}
		else if (plan.collides && policy == collision_policy::skip)
		{
			plan.skipped = true;
		}
		else if (plan.collides)
		{
			plan.destination_fi = platform::file_attributes(destination);
		}

		plan.destination = destination;
		planned_paths.emplace(destination);
		result.emplace_back(std::move(plan));
	}

	return result;
}

df::date_t adjusted_item_date(const df::date_t created, const df::date_t new_start, const df::date_t original_start)
{
	return created.is_valid() && original_start.is_valid() ? created + (new_start - original_start) : new_start;
}

// Adjust dates writes Original, shifting whatever the item resolves to now - Original where the file
// carries one, the rung below it where it does not. Naming that tag before the run is what makes
// mixed provenance in a selection visible: the same shift does not mean the same thing to a file
// dated by its shutter and a file dated by the day it landed on this disk.
std::string_view adjust_date_source_name(const prop::item_metadata_const_ptr& md)
{
	if (md)
	{
		for (const auto kind : {
			     prop::date_concept::original, prop::date_concept::created, prop::date_concept::modified
		     })
		{
			if (md->dates.resolve(kind).is_valid()) return prop::date_source_name(md->dates.resolved_source(kind));
		}
	}

	// Mirrors calc_media_created: with no metadata date the filesystem creation stamp is what is
	// being shifted, and saying so is the whole point of the column.
	return prop::date_source_name(prop::date_source::file_created);
}

icon_index drive_icon(const platform::drive_type d)
{
	switch (d)
	{
	case platform::drive_type::removable: return icon_index::usb;
	case platform::drive_type::fixed: return icon_index::hard_drive;
	case platform::drive_type::remote: return icon_index::network;
	case platform::drive_type::cdrom: return icon_index::disk;
	case platform::drive_type::device: return icon_index::disk;
	default: ;
	}

	return icon_index::hard_drive;
}

bool df::is_excluded(const index_roots& roots, const folder_path path)
{
	if (std::ranges::any_of(roots.excludes, [&roots, path](const folder_path exclude)
	{
		if (!folder_contains(exclude.text().sv(), path.text().sv())) return false;

		return std::ranges::none_of(roots.folders, [&roots, exclude, path](const folder_path root)
		{
			return exclude != root &&
				folder_contains(exclude.text().sv(), root.text().sv()) &&
				folder_contains(root.text().sv(), path.text().sv());
		});
	})) return true;

	const auto name = path.name();

	for (const auto& exclude : roots.exclude_wildcards)
	{
		if (str::wildcard_icmp(name, exclude))
			return true;
	}

	return false;
}
