// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Application view state management. Coordinates display state, media playback,
// selection handling, navigation history, and view mode transitions.

#include "pch.h"
#include "model.h"
#include "model_display.h"
#include "model_index.h"

#include "app_command_line.h"
#include "files.h"
#include "model_items.h"
#include "av_visualizer.h"
#include "av_format.h"
#include "metadata_xmp.h"
#include "ui_dialog.h"
#include "ui_controls.h"
#include "app_command_status.h"


static constexpr sizei video_preview_size = {256, 256};

// A hovered thumbnail tells the user what the video contains, not where they are in it, so a frame
// anywhere within this fraction of the duration answers the hover. The scrubber has no such slack:
// it promises the frame playback will jump to.
static constexpr double hover_thumbnail_tolerance = 0.02;
fast_fft av_visualizer::fft;

static bool av_can_use_hw()
{
	return setting.use_d3d11va && !command_line.no_gpu && setting.use_gpu &&
		!platform::crash_guard_suppressed(platform::crash_guard::gpu_render);
}

df::folder_path view_state::save_path() const
{
	if (!_search.has_selector()) return known_path(platform::known_folder::pictures);
	return _search.selectors().front().folder();
}

int view_state::displayed_rating() const
{
	for (const auto& i : _selected.items())
	{
		const auto md = i->metadata();

		if (md && md->rating)
		{
			return md->rating;
		}
	}

	return 0;
}

void view_state::toggle_rating(const df::results_ptr& results, const df::item_elements& items, const int r,
                               const view_host_base_ptr& view)
{
	bool add_rating = false;

	if (r != 0)
	{
		for (const auto& i : items)
		{
			const auto md = i->metadata();

			if (md && md->rating != r)
			{
				add_rating = true;
			}
		}
	}

	metadata_edits edits;

	if (add_rating)
	{
		edits.rating = r;
	}
	else
	{
		edits.remove_rating = true;
	}

	modify_items(results, icon_index::star, tt.title_rate, items, edits, view);
}


void view_state::modify_items(const ui::control_frame_ptr& frame, const icon_index icon, const std::string_view title,
                              const df::item_elements& items_to_modify, const metadata_edits& edits,
                              const view_host_base_ptr& view)
{
	auto dlg = make_dlg(frame);
	const auto results = std::make_shared<command_status>(_async, dlg, icon, title, selected_count());
	modify_items(results, icon, title, items_to_modify, edits, view);
}


void display_state_t::load_compare_preview(const int elapsed_numerator, const int elapsed_denominator)
{
	if (_is_compare_video && df::file_handles_detached == 0)
	{
		const auto i1 = _item1;
		const auto i2 = _item2;
		const auto st1 = _selected_texture1;
		const auto st2 = _selected_texture2;
		const auto md1 = i1->metadata();
		const auto md2 = i2->metadata();
		const auto duration1 = md1 ? std::max(1, static_cast<int>(md1->duration)) : 1;
		const auto duration2 = md2 ? std::max(1, static_cast<int>(md2->duration)) : 1;
		const auto max_duration = std::max(duration1, duration2);
		const auto elapsed = std::clamp(df::mul_div(elapsed_numerator, max_duration,
		                                            std::max(1, elapsed_denominator)), 0, max_duration);
		const auto pos1 = std::min(elapsed, duration1);
		const auto pos2 = std::min(elapsed, duration2);

		// Detached on the UI thread: item_element is UI-owned, so the worker gets values, not items.
		const auto path1 = i1->path();
		const auto path2 = i2->path();

		// The pane these land in, not the hover popup's 256 square: a compare pane is half the media
		// area, so the hover constant would upscale visibly. Decoding at the source's own size is
		// the other end of the same mistake - two 4K frames per pointer move, past the texture budget.
		const auto pane1 = st1->display_bounds().extent();
		const auto pane2 = st2->display_bounds().extent();
		const auto max_dim1 = pane1.is_empty() ? video_preview_size : pane1;
		const auto max_dim2 = pane2.is_empty() ? video_preview_size : pane2;

		_async.queue_media_preview(
			[t = ui_owned(_async, shared_from_this()), path1, path2, st1 = ui_owned(_async, st1),
				st2 = ui_owned(_async, st2), pos1, pos2, duration1, duration2, max_dim1, max_dim2](
			media_preview_state& decoder)
			{
				if (decoder.open1(path1) &&
					decoder.open2(path2))
				{
					auto surface1 = std::make_shared<ui::surface>();
					auto surface2 = std::make_shared<ui::surface>();

					if (decoder.decoder1->extract_seek_frame(surface1, max_dim1, pos1, duration1) &&
						decoder.decoder2->extract_seek_frame(surface2, max_dim2, pos2, duration2))
					{
						t->_async.queue_ui([t, st1, st2, surface1, surface2]
						{
							// The compared pair can be swapped out while the two seeks are in flight.
							if (t->_selected_texture1.get() != st1.get() ||
								t->_selected_texture2.get() != st2.get())
								return;

							st1->update(surface1);
							st2->update(surface2);
							t->_async.invalidate_view(view_invalid::view_redraw);
						});
					}
				}
			});
	}
}

void display_state_t::load_seek_preview(int pos_numerator, int pos_denominator, std::function<void()> callback)
{
	const auto item = _item1;

	if (player_has_video() && item && item->online_status() == df::item_online_status::disk && df::file_handles_detached
		== 0)
	{
		// Detached on the UI thread: the worker gets the path, and the item travels only as a
		// UI-owned currency token that is compared after the hop back.
		const auto path = item->path();

		_async.queue_media_preview(
			[t = ui_owned(_async, shared_from_this()), item = ui_owned(_async, item), path, pos_numerator,
				pos_denominator, callback](
			media_preview_state& decoder)
			{
				if (decoder.open1(path))
				{
					auto surface = std::make_shared<ui::surface>();

					if (decoder.decoder1->extract_seek_frame(surface, video_preview_size, pos_numerator,
					                                        pos_denominator, decoder.abandon_token()))
					{
						t->_async.queue_ui([t, item, surface = std::move(surface), callback]() mutable
						{
							// The display item can change while the seek decodes; a preview of the
							// previous video must not become the current hover surface.
							if (t->_item1.get() != item.get()) return;

							t->_hover_surface = std::move(surface);
							callback();
						});
					}
				}
			});
	}
}

void display_state_t::update_av_session(const std::shared_ptr<av_session>& ses)
{
	if (_session != ses)
	{
		_session = ses;
		// The latch belongs to the clip that ended, not to the display.
		_media_end_handled = false;
	}

	_av_open_failed = !ses;

	if (ses)
	{
		_player_media_info = ses->info();
		_full_metadata_loaded = true;
		load_xmp_sidecar();
	}
	else
	{
		load_selected_item_data();
	}

	_async.invalidate_view(
		view_invalid::view_layout |
		view_invalid::screen_saver |
		view_invalid::media_elements |
		view_invalid::command_state);
}

void display_state_t::load_selected_item_data()
{
	df::assert_true(ui::is_ui_thread());

	const auto item = _item1;
	if (!item) return;

	_async.queue_async(async_queue::load,
	                   [self = ui_owned(_async, shared_from_this()), path = item->path()]
	                   {
		                   df::scope_locked_inc l(df::loading_media);
		                   // Shared because the UI task is stored in a std::function, which requires a copyable target.
		                   auto data = std::make_shared<df::blob>(blob_head_from_file(path, df::one_meg));

		                   self->_async.queue_ui([self, data]
		                   {
			                   self->_selected_item_data = std::move(*data);
			                   self->_async.invalidate_view(
				                   view_invalid::media_elements | view_invalid::view_layout);
		                   });
	                   });
}

void display_state_t::load_xmp_sidecar()
{
	df::assert_true(ui::is_ui_thread());

	const auto item = _item1;
	if (!item) return;

	const auto xmp_name = item->xmp();
	if (xmp_name.is_empty()) return;

	for (const auto& b : _player_media_info.metadata)
	{
		if (b.standard == metadata_standard::xmp) return;
	}

	const auto sidecar = item->path().folder().combine_file(xmp_name.sv());
	const auto generation = _av_generation;

	_async.queue_async(async_queue::load, [weak = weak_from_this(), &async = _async, sidecar, generation]
	{
		const auto packet = df::blob_from_file(sidecar);
		if (packet.empty()) return;

		bool parsed = false;
		auto kv = metadata_xmp::to_info(packet, parsed);

		// The packet is the block's real content, so it stays reachable whether or not the toolkit
		// could make a tree from it.
		constexpr size_t max_raw_bytes = 256 * 1024;
		std::string raw;
		raw.assign(std::bit_cast<const char*>(packet.data()), std::min(packet.size(), max_raw_bytes));

		metadata_block block(metadata_standard::xmp, std::move(kv), packet.size(), parsed, std::move(raw));

		async.queue_ui([weak, block = std::move(block), generation]() mutable
		{
			const auto t = weak.lock();

			// A newer open has the display; its own sidecar read will publish.
			if (!t || t->_av_generation != generation) return;

			for (const auto& b : t->_player_media_info.metadata)
			{
				if (b.standard == metadata_standard::xmp) return;
			}

			t->_player_media_info.metadata.emplace_back(std::move(block));
			t->_async.invalidate_view(view_invalid::view_layout | view_invalid::media_elements);
		});
	});
}

void display_state_t::calc_pixel_difference()
{
	const auto st1 = _selected_texture1;
	const auto st2 = _selected_texture2;

	if (st1 && st2)
	{
		const auto loaded1 = st1->_loaded;
		const auto loaded2 = st2->_loaded;
		_async.queue_media_preview(
			[weak = weak_from_this(), st1 = ui_owned(_async, st1), st2 = ui_owned(_async, st2),
				loaded1, loaded2, &async = _async](
			media_preview_state& decoder)
			{
				const auto result = loaded1.calc_pixel_difference(loaded2);

				async.queue_ui([weak, st1, st2, result]
				{
					const auto t = weak.lock();
					if (!t || t->_selected_texture1.get() != st1.get() ||
						t->_selected_texture2.get() != st2.get())
						return;

					t->_pixel_difference = result;
					t->_async.invalidate_view(view_invalid::view_layout | view_invalid::media_elements);
				});
			});
	}
}


void view_state::load_hover_thumb(const df::item_element_ptr& item, double pos_numerator,
                                  double pos_denominator)
{
	df::assert_true(ui::is_ui_thread());
	const auto generation = ++_hover_thumbnail_generation;

	if (item &&
		(item->file_type()->group->traits && file_traits::preview_video) &&
		item->online_status() == df::item_online_status::disk &&
		df::file_handles_detached == 0)
	{
		const auto path = item->path();
		const auto modified = item->file_modified();
		const auto lifetime = item->weak_from_this();
		_async.queue_media_preview(
			[this, path, modified, lifetime, generation, pos_numerator, pos_denominator](media_preview_state& decoder)
			{
				if (decoder.open1(path))
				{
					auto surface = std::make_shared<ui::surface>();

					if (decoder.decoder1->
					            extract_thumbnail(surface, video_preview_size, pos_numerator, pos_denominator, true,
					                              hover_thumbnail_tolerance, decoder.abandon_token()))
					{
						// The store has one encoder for every producer. Encoding a scrub frame as PNG
						// instead ran a zlib deflate per pointer position and put a blob into the
						// thumbnail cache several times the size the cache is designed around.
						if (!decoder.encoder) decoder.encoder = std::make_unique<files>();
						const auto image = decoder.encoder->surface_to_thumbnail(surface);

						if (is_valid(image))
						{
							const auto cover_art = decoder.decoder1->cover_art();

							queue_ui([this, lifetime, path, image, cover_art, modified, generation]
							{
								if (generation == _hover_thumbnail_generation)
								{
									if (const auto current = lifetime.lock(); current && current->path() == path &&
										current->file_modified() == modified)
									{
										current->thumbnail(image, cover_art, modified);
										item_index.queue_stage_thumbnails({current});
										item_index.save_thumbnail(path, image, cover_art, modified);
										invalidate_view(view_invalid::view_redraw);
									}
								}
							});
						}
					}
				}
			});
	}
}

void view_state::clear_hover_codec()
{
	df::assert_true(ui::is_ui_thread());
	++_hover_thumbnail_generation;
	_async.queue_media_preview([](media_preview_state& decoder)
	{
		decoder.close();
	}, true);
}

void view_state::rescan_hydrated_display_item()
{
	const auto d = _display;

	if (!d || !d->_item1 || !d->_selected_texture1)
	{
		return;
	}

	const auto& item = d->_item1;

	// Re-index a cloud-only placeholder once, after it has been hydrated by viewing it. The trigger
	// is the completion of the full-file metadata scan (_full_metadata_loaded): reading the whole
	// file is what finishes the OneDrive download and clears the offline attribute -- a partial
	// preview read (which is enough to show the image) leaves the file a placeholder, so triggering
	// off the loaded texture alone would re-index it while still offline and miss the metadata.
	// Once the file is fully hydrated the re-index runs the normal online scan, and that scan's own
	// invalidate_view(index_summary | media_elements) rebuilds the summary/tag list from the freshly
	// scanned item->metadata() -- no polling or forced refresh needed.
	if (item->online_status() != df::item_online_status::offline ||
		!d->_full_metadata_loaded ||
		d->_selected_texture1->loaded().is_empty() ||
		_hydration_rescan_done == item->path())
	{
		return;
	}

	_hydration_rescan_done = item->path();

	// Build a thumbnail from the image already in memory so the items view updates without waiting
	// on an async re-scan, and persist it to the database. Decoding to a surface and re-encoding are
	// both expensive, so they run on a worker; the result is published back with a path/modified
	// check because the display item can change while the work is in flight.
	_async.queue_async(async_queue::load,
	                   [this, loaded = d->_selected_texture1->loaded(), lifetime = std::weak_ptr(item),
		                   path = item->path(), modified = item->file_modified()]() mutable
	                   {
		                   files ff;
		                   const auto thumb_surface = loaded.to_surface(setting.thumbnail_max_dimension, false, {},
		                                                                decode_intent::thumbnail);
		                   if (!ui::is_valid(thumb_surface)) return;

		                   auto thumb_image = ff.surface_to_thumbnail(thumb_surface);
		                   if (!ui::is_valid(thumb_image) || thumb_image->data().size() >= df::two_fifty_six_k) return;

		                   queue_ui([this, lifetime, path, modified, thumb_image = std::move(thumb_image)]
		                   {
			                   const auto current = lifetime.lock();
			                   if (!current || current->path() != path || current->file_modified() != modified) return;

			                   current->thumbnail(thumb_image, {}, modified);
			                   item_index.save_thumbnail(path, thumb_image, {}, modified);
			                   invalidate_view(view_invalid::view_redraw | view_invalid::view_layout);
		                   });
	                   });

	// Queue the rescan so the index catches up: online status clears, the real metadata (tags,
	// camera, etc.) is read, and a content hash is computed for duplicate detection.
	df::item_set to_rescan;
	item->add_to(to_rescan);
	item_index.queue_scan_modified_items(to_rescan);
}

// Releases one signal once every handle owner has reported, so a teardown that waits on several
// workers is bounded by a single timeout instead of one timeout per owner.
struct teardown_signal
{
	std::atomic_int remaining;
	platform::thread_event done{true, false};

	explicit teardown_signal(const int owners) : remaining(owners)
	{
	}

	void complete()
	{
		if (--remaining == 0) done.set();
	}
};

// Runs where the first guard waits for the display's media to let go, which pumps messages. Whatever
// the user does in there - closing the view that asked for the guard, say - happens before its caller
// has carried on.
std::function<void()> test_during_detach_file_handles;

detach_file_handles::detach_file_handles(view_state& s) : _state(s)
{
	df::gauge_enter(df::file_handles_detached);
	if (++s._file_handle_detach_count > 1) return;
	s._detached_display_item.reset();
	s._detached_display_is_playable = false;
	s._detached_display_is_playing = false;
	s._detached_display_should_reopen = true;
	s._detached_display_video_track = -1;
	s._detached_display_audio_track = -1;
	// Anything left from an earlier window is stale, and holding it would keep a file open across
	// the rename, replace or delete this guard exists for.
	s._detached_display_handle.reset();

	const auto d = s.display_state();

	if (d)
	{
		// Supersede any open still in flight. It would otherwise publish a live session onto the
		// display, reopening the very file the caller is about to rename, replace or delete.
		++d->_av_generation;

		s._detached_display_item = d->_item1;
		s._detached_display_is_playable = d->can_play_media();
		s._detached_display_is_playing = d->is_playing_media();

		if (d->_session)
		{
			s._detached_display_video_track = d->_session->video_stream_id();
			s._detached_display_audio_track = d->_session->audio_stream_id();
		}

		if (s._detached_display_is_playable)
		{
			// Shared, not stack-local: the wait is bounded, and on timeout the workers still own their
			// signal long after this frame is gone. One signal for both owners bounds the whole teardown
			// by a single timeout rather than one per owner.
			const auto teardown = std::make_shared<teardown_signal>(d->_session ? 2 : 1);

			d->_hover_surface.reset();

			if (d->_session)
			{
				s._player->close(d->_session, [teardown]
				{
					teardown->complete();
				});

				// The close owns its own reference; leaving this one set would let present, transport
				// state and the position bar keep using a session that is being torn down.
				d->_session.reset();
			}

			// Must run: a superseded close would leave the file handles open that the caller is about to
			// rename, replace or delete over.
			s._async.queue_media_preview([teardown](media_preview_state& decoder)
			{
				decoder.close();
				teardown->complete();
			}, true);

			platform::wait_for({teardown->done, platform::event_exit}, 10000, false);

			s._async.invalidate_view(view_invalid::view_layout |
				view_invalid::screen_saver |
				view_invalid::app_layout |
				view_invalid::media_elements |
				view_invalid::command_state);
		}

		d->stop_slideshow();
	}

	if (test_during_detach_file_handles) test_during_detach_file_handles();

	s.clear_hover_codec();
}

detach_file_handles::~detach_file_handles()
{
	df::gauge_leave(df::file_handles_detached);

	// A worker can own the last reference, because the guard is handed to callers through completion
	// callbacks. Everything the release touches is UI-owned, including the non-atomic nesting count.
	if (ui::is_ui_thread())
	{
		_state.release_detached_file_handles(_reopen_display);
	}
	else
	{
		auto& s = _state;
		s._async.queue_ui([&s, reopen = _reopen_display] { s.release_detached_file_handles(reopen); });
	}
}

void view_state::release_detached_file_handles(const bool reopen_display)
{
	df::assert_true(ui::is_ui_thread());

	if (!reopen_display) _detached_display_should_reopen = false;
	if (--_file_handle_detach_count != 0) return;

	const auto d = display_state();

	if (_detached_display_should_reopen && d && _detached_display_item &&
		_detached_display_item == d->_item1)
	{
		const auto path = _detached_display_item->path();

		if (path.exists())
		{
			if (_detached_display_is_playable)
			{
				open_av_session(d, _detached_display_item, _detached_display_is_playing,
				                _detached_display_video_track, _detached_display_audio_track, true,
				                std::move(_detached_display_handle));
			}
		}
		else
		{
			select_nothing({});
			close();
		}
	}

	// Unconditional: a handle nobody consumed must not outlive the window it was handed over for.
	_detached_display_handle.reset();
	_detached_display_item.reset();
	item_index.queue_scan_modified_items(selected_items());
}

void view_state::publish_written_image(const df::file_path path, file_load_result loaded,
                                       const df::date_t modified) const
{
	df::assert_true(ui::is_ui_thread());

	const auto d = display_state();

	if (!d) return;

	if (d->_selected_texture1 && d->_item1 && d->_item1->path() == path)
	{
		d->_selected_texture1->publish_written_image(path, std::move(loaded), modified);
	}
	else if (d->_selected_texture2 && d->_item2 && d->_item2->path() == path)
	{
		d->_selected_texture2->publish_written_image(path, std::move(loaded), modified);
	}
}

// Two selected items are both on screen, so both can be handed the bytes a write produced.
std::vector<df::file_path> view_state::displayed_photo_paths() const
{
	df::assert_true(ui::is_ui_thread());

	const auto d = display_state();
	if (!d) return {};

	std::vector<df::file_path> result;
	if (d->_item1) result.emplace_back(d->_item1->path());
	if (d->_item2) result.emplace_back(d->_item2->path());
	return result;
}

void view_state::publish_written_handle(const df::file_path path, platform::file_ptr file)
{
	df::assert_true(ui::is_ui_thread());

	// The reopen only uses the handle for the item it detached, so anything else would just be a
	// file held open for nothing.
	if (!file || !_detached_display_item || _detached_display_item->path() != path)
	{
		return;
	}

	_detached_display_handle = std::move(file);
}

df::file_path view_state::detached_display_av_path() const
{
	df::assert_true(ui::is_ui_thread());

	return _detached_display_is_playable && _detached_display_should_reopen && _detached_display_item
		       ? _detached_display_item->path()
		       : df::file_path{};
}

bool media_preview_state::open1(const df::file_path file_path)
{
	const auto new_path = !decoder1 || decoder1->path() != file_path;

	if (new_path)
	{
		const auto new_decoder = std::make_shared<av_format_decoder>();

		if (new_decoder->open(file_path, media_intent::thumbnail))
		{
			// Threaded: a hover decodes a whole GOP forward to reach the frame it was asked for.
			new_decoder->init_streams(-1, -1, false, true, true);
			decoder1 = new_decoder;
		}
		else
		{
			decoder1.reset();
		}
	}

	return decoder1 != nullptr;
}


bool media_preview_state::open2(const df::file_path file_path)
{
	const auto new_path = !decoder2 || decoder2->path() != file_path;

	if (new_path)
	{
		const auto new_decoder = std::make_shared<av_format_decoder>();

		if (new_decoder->open(file_path, media_intent::thumbnail))
		{
			// Threaded: a hover decodes a whole GOP forward to reach the frame it was asked for.
			new_decoder->init_streams(-1, -1, false, true, true);
			decoder2 = new_decoder;
		}
		else
		{
			decoder2.reset();
		}
	}

	return decoder2 != nullptr;
}

df::unique_paths make_unique_paths(df::paths selection)
{
	df::unique_paths result(selection.files.begin(), selection.files.end());
	for (const auto& f : selection.folders) result.emplace(f);
	return result;
}

void view_state::browse_forward(const view_host_base_ptr& view)
{
	history_state::history_entry e;

	if (history.history_entry_at(1, e))
	{
		_pending_history_move = {e.search, 1, _selected.ids()};
		if (!open(view, e.search, make_unique_paths(e.selected))) _pending_history_move.reset();
	}
}

void view_state::browse_back(const view_host_base_ptr& view)
{
	history_state::history_entry e;

	if (history.history_entry_at(-1, e))
	{
		_pending_history_move = {e.search, -1, _selected.ids()};
		if (!open(view, e.search, make_unique_paths(e.selected))) _pending_history_move.reset();
	}
}

void view_state::capture_display(const std::function<void(file_load_result)>& f) const
{
	const auto d = _display;

	if (d)
	{
		// The frame of a playing video arrives later, on this thread, and only the latest request
		// may act on it: a Copy overtaken by a newer Copy would otherwise land last and put the older
		// frame on a clipboard the user has since filled. Moving on to another item does not
		// overtake it - every caller takes what it needs when it asks, so Copy followed at once by
		// Next still copies. The display itself is not held: this callback waits on the media read
		// thread, which must never be left holding the last reference to UI-owned state.
		const auto generation = ++_capture_generation;
		const auto deliver = [this, generation, f](file_load_result lr)
		{
			if (generation != _capture_generation) return;
			f(std::move(lr));
		};

		// player_has_video() reports the media info, which outlives the session across a
		// file-handle detach, so the session itself has to be checked too.
		if (d->player_has_video() && d->_session)
		{
			_player->capture(d->_session, deliver);
		}
		else if (d->_selected_texture1)
		{
			deliver(d->_selected_texture1->loaded());
		}
		else
		{
			deliver({});
		}
	}
}

void view_state::view_mode(const view_type m)
{
	if (_view_mode != m)
	{
		_view_mode = m;
		record_feature_use(features::view_bit(m));
		_events.view_changed(_view_mode);

		// A task view needs the top bar and its own controls, both of which full screen hides. Full
		// screen is also refused from a view, so the two states never coexist in either direction.
		if (is_full_screen && m != view_type::media)
		{
			_events.toggle_full_screen();
		}
	}
}

void view_state::play(const view_host_base_ptr& view)
{
	const auto d = _display;

	if (!d)
		return;

	// Play is media transport only. It never starts a slideshow, but it does stop one so the
	// same key that started playing always stops it.
	if (d->is_playing())
	{
		stop();
	}
	else if (d->can_play_media())
	{
		_player->play(d->_session);
	}
}

void view_state::toggle_slideshow(const view_host_base_ptr& view)
{
	const auto d = _display;

	if (!d)
		return;

	if (d->is_slideshow())
	{
		stop();
		return;
	}

	if (!can_slideshow())
		return;

	const auto displayed = d->is_one() ? d->_item1 : nullptr;
	const auto start = displayed && displayed->is_media() ? displayed : next_media_item(true, true);

	if (!start)
		return;

	record_feature_use(features::slideshow);

	// Set before selecting so the display state opened for the first item already knows a
	// slideshow is running and starts any video or audio playing.
	_common_display_state._is_slideshow = true;

	if (start != displayed)
	{
		select(view, start, false, false, true);
	}
	else if (d->can_play_media() && !d->is_playing_media())
	{
		_player->play(d->_session);
	}

	invalidate_view(view_invalid::command_state | view_invalid::view_redraw | view_invalid::screen_saver);
}

void view_state::stop()
{
	const auto d = _display;

	_play_next_on_open = false;

	if (d)
	{
		d->stop_slideshow();

		if (d->is_playing_media() && d->_session)
		{
			_player->pause(d->_session);
		}
	}

	_common_display_state._is_slideshow = false;

	invalidate_view(view_invalid::command_state | view_invalid::screen_saver);
}

bool display_state_t::publish_av_session(const std::shared_ptr<av_session>& ses, const uint32_t generation)
{
	if (generation != _av_generation) return false;

	update_av_session(ses);
	return true;
}

void view_state::open_av_session(const std::shared_ptr<display_state_t>& d, const df::item_element_ptr& i,
                                 const bool auto_play, const int video_track, const int audio_track,
                                 const bool use_last_played_pos, platform::file_ptr file) const
{
	const auto generation = ++d->_av_generation;

	_player->open(i, auto_play, video_track, audio_track, av_can_use_hw(), use_last_played_pos,
	              [d = ui_owned(_async, d), generation, player = _player](const std::shared_ptr<av_session>& ses)
	              {
		              // Dropping a superseded session instead of closing it would leave the file open
		              // across the operation the teardown was for.
		              if (!d->publish_av_session(ses, generation) && ses)
		              {
			              player->close(ses, {});
		              }
	              }, std::move(file));
}

void view_state::change_tracks(const int video_track, const int audio_track) const
{
	const auto d = _display;

	if (d && d->_session)
	{
		const auto auto_play = d->_session->is_playing();

		// The replacement reopens the same file, so the outgoing session must be closed here
		// rather than left for whichever thread happens to drop the last reference - that left
		// two decoders open on one file and ran the teardown on a decode thread.
		_player->close(d->_session, {});
		d->_session.reset();

		open_av_session(d, d->_item1, auto_play, video_track, audio_track, true);
	}
}

void view_state::change_audio_device(const std::string& id) const
{
	if (_player)
	{
		_player->audio_device_id(id);
	}
}

void view_state::toggle_group_order()
{
	constexpr group_by options[] = {
		group_by::file_type,
		group_by::size,
		group_by::extension,
		group_by::location,
		group_by::rating_label,
		group_by::date_original,
		group_by::date_created,
		group_by::date_modified,
		group_by::camera,
		group_by::resolution,
		group_by::aspect_ratio,
		group_by::album_show,
		group_by::folder,
		group_by::presence,
	};

	bool found_order = false;

	for (auto i = 0u; i < std::size(options); ++i)
	{
		if (_group_order == options[i])
		{
			group_order(options[(i + 1) % std::size(options)], _sort_order);
			found_order = true;
			break;
		}
	}

	if (!found_order)
	{
		group_order(group_by::file_type, _sort_order);
	}
}

void view_state::group_order(const std::optional<group_by> group, const std::optional<sort_by> order)
{
	auto changed = false;
	const auto is_currently_shuffle = _group_order == group_by::shuffle;

	if (order.has_value() && (_sort_order != order || is_currently_shuffle))
	{
		_sort_order = order.value();

		if (is_currently_shuffle)
		{
			_group_order = group_by::file_type;
		}

		changed = true;
	}

	if (group.has_value() && _group_order != group)
	{
		_group_order = group.value();
		changed = true;
	}

	if (changed)
	{
		++_group_title_generation;

		if (group == group_by::shuffle)
		{
			_search_items.shuffle();
		}

		_events.invalidate_view(
			view_invalid::view_layout |
			view_invalid::group_layout |
			view_invalid::command_state |
			view_invalid::app_layout);
	}
}

ui::const_image_ptr view_state::first_selected_thumb() const
{
	ui::const_image_ptr result;

	for (const auto& b : _item_groups)
	{
		for (const auto& i : b->items())
		{
			if (i->has_thumb() && i->is_selected()) result = i->thumbnail();
		}
	}

	return result;
}

df::unique_items view_state::existing_items() const
{
	df::unique_items results;

	if (_edit_item)
	{
		results._items.insert_or_assign(_edit_item->path(), _edit_item);
	}

	_search_items.append_unique(results);

	return results;
}

void view_state::append_items(const view_host_base_ptr& view, df::item_set items, const df::unique_paths& selection,
                              const bool is_first, const bool is_complete)
{
	df::item_elements select_list;

	if (is_first)
	{
	}

	if (selection.empty())
	{
		for (const auto& i : items.items())
		{
			if (i->is_selected())
			{
				select_list.emplace_back(i);
			}
		}
	}
	else
	{
		for (const auto& i : items.items())
		{
			if (selection.contains(i->path()))
			{
				select_list.emplace_back(i);
			}
		}
	}

	_focus = items.contains(_focus) ? _focus : nullptr;
	_selection_anchor = items.contains(_selection_anchor) ? _selection_anchor : nullptr;
	_pin_item = items.contains(_pin_item) ? _pin_item : nullptr;
	_summary_total = items.summary();
	_search_items = std::move(items);

	update_item_groups();
	select(view, select_list, false);
	update_selection();

	// An open that names the items to select exists to show the user those items, so the completed
	// listing brings focus back into view. Without this a paste or a reveal leaves the new selection
	// scrolled off screen, because the intermediate short listings clamp the item scroll to the top.
	if (is_complete && !selection.empty() && !select_list.empty())
	{
		invalidate_view(view_invalid::focus_item_visible);
	}

	const auto d = _display;

	if (d)
	{
		if (d->_selected_texture1)
		{
			d->_selected_texture1->refresh(d->_item1);
		}

		if (d->_selected_texture2)
		{
			d->_selected_texture2->refresh(d->_item2);
		}
	}

	invalidate_view(view_invalid::group_layout_complete);
}

bool view_state::update_selection()
{
	df::item_set selected;

	for (const auto& g : _item_groups)
	{
		for (const auto& i : g->items())
		{
			if (i->is_selected())
			{
				i->add_to(selected);
			}
		}
	}

	// design.md: the pin is a visibly selected item. One that a filter, a new search, or an
	// inverted selection has dropped is state the user can no longer see or reach, so it stops
	// being the pin rather than silently outliving the item it names.
	if (_pin_item && !_pin_item->is_selected())
	{
		_pin_item.reset();
		invalidate_view(view_invalid::command_state);
	}

	const bool changed = _selected != selected;

	if (changed)
	{
		_selected = selected;
		load_display_state();
	}

	return changed;
}

df::item_element_ptr view_state::find_displayed_item_by_name(const std::string_view file_name) const
{
	for (const auto& b : _item_groups)
	{
		for (const auto& i : b->items())
		{
			if (icmp(i->name(), file_name) == 0)
			{
				return i;
			}
		}
	}

	return nullptr;
}


bool view_state::select(const view_host_base_ptr& view, const std::string_view file_name, const bool toggle)
{
	const auto i = find_displayed_item_by_name(file_name);

	if (i)
	{
		select(view, i, toggle, false, false);
		return true;
	}

	return false;
}

void view_state::select(const view_host_base_ptr& view, const df::item_element_ptr& selected_item, const bool toggle,
                        const bool extend, const bool continue_slideshow)
{
	if (is_item_displayed(selected_item))
	{
		if (extend)
		{
			// The range always runs between the anchor and the clicked item. Shift replaces the
			// range; Ctrl+Shift adds to what is already selected.
			auto anchor = _selection_anchor;

			if (!anchor || !is_item_displayed(anchor)) anchor = _focus;
			if (!anchor || !is_item_displayed(anchor)) anchor = selected_item;

			df::item_element_ptr start_item;
			df::item_element_ptr end_item;

			for (const auto& b : _item_groups)
			{
				for (const auto& i : b->items())
				{
					if (i == anchor || i == selected_item)
					{
						if (!start_item) start_item = i;
						end_item = i;
					}
				}
			}

			bool is_selecting = false;
			bool is_end = false;

			for (const auto& b : _item_groups)
			{
				for (const auto& i : b->items())
				{
					if (i == start_item) is_selecting = true;

					if (is_selecting && !is_end)
					{
						i->select(true, view, i);
					}
					else if (!toggle && i->is_selected())
					{
						i->select(i == _pin_item, view, i);
					}

					if (i == end_item) is_end = true;
				}
			}
		}
		else if (toggle)
		{
			selected_item->invert_selection(view, selected_item);
		}
		else
		{
			for (const auto& b : _item_groups)
			{
				for (const auto& i : b->items())
				{
					if (i == selected_item)
					{
						i->select(true, view, i);
					}
					else if (i->is_selected())
					{
						i->select(i == _pin_item, view, i);
					}
				}
			}
		}

		// A plain or toggling click is where the next Shift range starts from.
		if (!extend)
		{
			_selection_anchor = selected_item;
		}

		if (selected_item != _focus)
		{
			const auto previous = _focus;
			_focus = selected_item;
			_events.item_focus_changed(selected_item, previous);
		}

		invalidate_view(view_invalid::selection_list);

		if (!continue_slideshow)
		{
			stop_slideshow();
		}
	}
}

void view_state::select(const view_host_base_ptr& view, const recti selection_bounds, const bool toggle)
{
	df::item_elements selections;

	for (const auto& b : _item_groups)
	{
		if (b->bounds.intersects(selection_bounds))
		{
			for (const auto& i : b->items())
			{
				if (i->bounds.intersects(selection_bounds))
				{
					selections.emplace_back(i);
				}
			}
		}
	}

	if (!selections.empty())
	{
		select(view, selections, toggle);
	}
}

void view_state::unselect(const view_host_base_ptr& view, const df::item_element_ptr& i)
{
	i->select(false, view, i);

	const auto f = first_selected();

	if (_focus == i)
	{
		const auto previous = _focus;
		_focus = f.item;
		_events.item_focus_changed(f.item, previous);
	}

	// An anchor that is no longer selected would measure the next Shift range from an item the user
	// just took out of the selection.
	if (_selection_anchor == i)
	{
		_selection_anchor = _focus;
	}

	invalidate_view(view_invalid::selection_list);
}

void view_state::hover_item(const view_host_base_ptr& view, const df::item_element_ptr& i, const bool is_hover)
{
	if (is_hover)
	{
		if (_hover != i)
		{
			if (_hover) _hover->set_style_bit(view_element_style::hover, false, view, _hover);
			_hover = i;
			if (_hover) _hover->set_style_bit(view_element_style::hover, true, view, _hover);
		}
	}
	else
	{
		if (i) i->set_style_bit(view_element_style::hover, false, view, i);
		if (_hover == i) _hover.reset();
	}
}

df::item_element_ptr view_state::item_from_location(const pointi loc) const
{
	// The hovered item can paint outside its layout bounds, so it wins the hit test.
	if (_hover && _hover->interactive_bounds().contains(loc)) return _hover;
	if (_focus && _focus->interactive_bounds().contains(loc)) return _focus;

	return item_from_layout_location(loc);
}

df::item_element_ptr view_state::item_from_layout_location(const pointi loc) const
{
	for (const auto& b : _item_groups)
	{
		if (b->bounds.contains(loc))
		{
			const auto item = b->drawable_from_layout_location(loc);
			if (item) return item;
		}
	}

	return nullptr;
}

group_and_item view_state::selected_item_group() const
{
	if (_focus)
	{
		for (const auto& g : _item_groups)
		{
			for (const auto& i : g->items())
			{
				if (_focus == i)
				{
					return {g, i};
				}
			}
		}
	}

	return first_selected();
}

bool view_state::is_item_displayed(const df::item_element_ptr& first_selection) const
{
	for (const auto& b : _item_groups)
	{
		for (const auto& i : b->items())
		{
			if (i == first_selection)
			{
				return true;
			}
		}
	}

	return false;
}

void view_state::select(const view_host_base_ptr& view, const df::item_elements& items_to_select, const bool toggle)
{
	if (toggle)
	{
		for (const auto& i : items_to_select)
		{
			if (_pin_item == i)
			{
				i->select(true, view, i);
			}
			else
			{
				i->select(!i->is_selected(), view, i);
			}
		}
	}
	else
	{
		for (const auto& b : _item_groups)
		{
			for (const auto& i : b->items())
			{
				i->select(_pin_item == i, view, i);
			}
		}

		for (const auto& i : items_to_select)
		{
			i->select(true, view, i);
		}
	}

	df::item_element_ptr new_focus;

	if (!items_to_select.empty())
	{
		auto first_selection = items_to_select.front();

		if (is_item_displayed(first_selection))
		{
			new_focus = std::move(first_selection);
		}
	}

	if (new_focus != _focus)
	{
		const auto previous_focus = _focus;
		_focus = new_focus;
		_events.item_focus_changed(new_focus, previous_focus);
	}

	// Shift extends from focus, so a gesture that moves focus also moves the anchor it measures from.
	_selection_anchor = new_focus;

	invalidate_view(view_invalid::selection_list);
}

void view_state::select_all(const view_host_base_ptr& view)
{
	for (const auto& b : _item_groups)
	{
		for (const auto& i : b->items())
		{
			i->select(true, view, i);
		}
	}

	invalidate_view(view_invalid::selection_list);
}

void view_state::select_nothing(const view_host_base_ptr& view)
{
	for (const auto& b : _item_groups)
	{
		for (const auto& i : b->items())
		{
			i->select(false, view, i);
			i->set_style_bit(view_element_style::highlight, false);
		}
	}

	_focus.reset();
	_pin_item.reset();
	_selection_anchor.reset();
	invalidate_view(view_invalid::selection_list);
}

void view_state::select_inverse(const view_host_base_ptr& view)
{
	for (const auto& b : _item_groups)
	{
		for (const auto& i : b->items())
		{
			i->invert_selection(view, i);
		}
	}

	invalidate_view(view_invalid::selection_list);
}

void view_state::select_end(const view_host_base_ptr& view, const bool forward, const bool toggle, const bool extend)
{
	select(view, end_item(forward), toggle, extend, false);
	stop_slideshow();
	make_visible(focus_item());
}

void view_state::select_next(const view_host_base_ptr& view, const bool forward, const bool toggle, const bool extend)
{
	select(view, next_item(forward, toggle || extend), toggle, extend, false);
	stop_slideshow();
	make_visible(focus_item());
}

void view_state::select_next_media(const view_host_base_ptr& view, const bool forward)
{
	if (const auto next = next_media_item(forward, false))
	{
		select(view, next, false, false, false);
		stop_slideshow();
		make_visible(focus_item());
	}
}

view_state::view_state(state_strategy& ev, async_strategy& ac, index_state& item_index,
                       std::shared_ptr<av_player> player) :
	_events(ev),
	_async(ac),
	item_index(item_index),
	_player(std::move(player)),
	_display(std::make_shared<display_state_t>(ac, _common_display_state))

{
}

view_state::~view_state()
{
	_display.reset();
	_search_items.clear();
	_item_groups.clear();
	_selected.clear();
	_focus = nullptr;
}

bool view_state::enter(const view_host_base_ptr& view)
{
	if (_selected.size() == 1 &&
		_selected.has_folders())
	{
		const auto folder = _selected.items().front();
		folder->open(*this, view);
		return true;
	}

	if (_selected.empty())
	{
		select_next(view, true, false, false);
	}

	if (!_selected.has_folders() &&
		!_selected.items().empty())
	{
		auto all_media = true;

		for (const auto& i : _selected.items())
		{
			all_media &= i->file_type()->is_media();
		}

		if (all_media)
		{
			if (!is_full_screen)
			{
				_events.toggle_full_screen();
			}
			else
			{
				select_next(view, true, false, false);
			}
		}
		else
		{
			invoke(commands::tool_open_with);
		}

		return true;
	}

	return false;
}

class search_element final : public text_element_base, public std::enable_shared_from_this<search_element>
{
public:
	view_state& _state;
	const df::search_t _search;
	prop::key_ref _prop_key;

	explicit search_element(view_state& state, const std::string_view text, df::search_t search,
	                        const prop::key_ref prop_key) noexcept
		: text_element_base(text, view_element_style::has_tooltip | view_element_style::can_invoke), _state(state),
		  _search(std::move(search)), _prop_key(prop_key)
	{
	}

	explicit search_element(view_state& state, const std::string_view text, df::search_t search) noexcept
		: text_element_base(text, view_element_style::has_tooltip | view_element_style::can_invoke), _state(state),
		  _search(std::move(search)), _prop_key(prop::null)
	{
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void tooltip(view_hover_element& result, const pointi loc, const pointi element_offset) const override
	{
		if (_prop_key != prop::null)
		{
			result.elements->add(make_icon_element(_prop_key->icon, flex_item::no_break));
			result.elements->add(std::make_shared<text_element>(_prop_key->text(), ui::style::font_face::dialog,
			                                                    ui::style::text_style::multiline,
			                                                    flex_item::line_break));
		}

		result.elements->add(std::make_shared<text_element>(_search.text(), ui::style::font_face::dialog,
		                                                    ui::style::text_style::multiline,
		                                                    flex_item::line_break));

		result.active_bounds = result.window_bounds = bounds.offset(element_offset);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			_state.open(event.host, _search, {});
		}
		else if (event.type == view_element_event_type::dpi_changed)
		{
			_tl.reset();
		}
	}
};

static view_element_ptr make_link(view_state& s, std::string_view text, df::search_t& search,
                                  const prop::key_ref prop, const df::search_result& current_search)
{
	auto element = std::make_shared<search_element>(s, text, search, prop);
	if (current_search.is_match(prop)) element->set_style_bit(view_element_style::important, true);
	return element;
}

static view_element_ptr make_link(view_state& s, std::string_view text, const prop::key_ref prop,
                                  const df::search_result& current_search)
{
	auto element = std::make_shared<search_element>(s, text, df::search_t().with(prop, text), prop);
	if (current_search.is_match(prop)) element->set_style_bit(view_element_style::important, true);
	return element;
}

static uint32_t calc_rank_color()
{
	return ui::average(app_colors::rank_background, ui::style::color::view_text);
}

static std::vector<view_element_ptr> format_dims(const uint16_t width, const uint16_t height, const file_type_ref ft,
                                                 const bool is_rank)
{
	std::vector<view_element_ptr> results;

	if (width > 0 && height > 0)
	{
		// Check for potential overflow in area calculation
		const uint64_t area = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
		if (area > static_cast<uint64_t>(std::numeric_limits<int>::max()))
		{
			// Dimensions too large, skip processing
			return results;
		}

		const auto dims = sizei{width, height};
		auto element = std::make_shared<text_element>(prop::format_dimensions(dims));
		if (is_rank) element->foreground_color(calc_rank_color());
		results.emplace_back(element);

		if (ft->has_trait(file_traits::av))
		{
			const auto video_res = prop::format_video_resolution(dims);

			if (!video_res.empty())
			{
				results.emplace_back(std::make_shared<text_element>(video_res));
			}
		}
		else
		{
			const auto mp = ui::calc_mega_pixels(dims.cx, dims.cy);

			if (mp > 0.9)
			{
				results.emplace_back(std::make_shared<text_element>(prop::format_pixels(dims, ft)));
			}
		}
	}

	return results;
}

static std::vector<view_element_ptr> create_camera_elements(view_state& s, const prop::item_metadata_const_ptr& md,
                                                            const df::search_result& search_result)
{
	std::vector<view_element_ptr> results;
	if (!is_empty(md->camera_model))
		results.emplace_back(
			make_link(s, md->camera_model, prop::camera_model, search_result));
	if (!df::is_zero(md->exposure_time))
		results.emplace_back(make_link(s, prop::format_exposure(md->exposure_time),
		                               df::search_t().with(
			                               prop::exposure_time, md->exposure_time),
		                               prop::exposure_time, search_result));
	if (!df::is_zero(md->f_number))
		results.emplace_back(make_link(s, prop::format_f_num(md->f_number),
		                               df::search_t().with(prop::f_number, md->f_number),
		                               prop::f_number, search_result));
	if (md->iso_speed != 0)
		results.emplace_back(make_link(s, prop::format_iso(md->iso_speed),
		                               df::search_t().with(prop::iso_speed, md->iso_speed),
		                               prop::iso_speed, search_result));
	if (!df::is_zero(md->focal_length))
		results.emplace_back(make_link(
			s, prop::format_focal_length(md->focal_length, md->focal_length_35mm_equivalent),
			df::search_t().with(prop::focal_length, md->focal_length), prop::focal_length, search_result));
	if (!is_empty(md->lens)) results.emplace_back(make_link(s, md->lens, prop::lens, search_result));

	return results;
}

static std::vector<view_element_ptr> create_album_elements(view_state& s, const prop::item_metadata_const_ptr& md,
                                                           const df::search_result& search_result)
{
	std::vector<view_element_ptr> results;

	if (!is_empty(md->album)) results.emplace_back(make_link(s, md->album, prop::album, search_result));
	if (!is_empty(md->show)) results.emplace_back(make_link(s, md->show, prop::show, search_result));

	if (md->year != 0)
	{
		results.emplace_back(make_link(s, str::to_string(md->year), df::search_t().with(prop::year, md->year),
		                               prop::year, search_result));
	}

	if (!is_empty(md->genre)) results.emplace_back(make_link(s, md->genre, prop::genre, search_result));

	return results;
}

static std::vector<view_element_ptr> create_artist_elements(view_state& s, const prop::item_metadata_const_ptr& md,
                                                            const df::search_result& search_result)
{
	std::vector<view_element_ptr> results;
	df::hash_map<std::string_view, prop::key_ref> unique;

	if (!is_empty(md->artist))
	{
		const auto parts = split(md->artist, true, str::is_artist_separator);

		for (const auto& a : parts)
		{
			unique[a] = prop::artist;
		}
	}

	if (!is_empty(md->album_artist))
	{
		const auto parts = split(md->album_artist, true, str::is_artist_separator);

		for (const auto& a : parts)
		{
			unique[a] = prop::album_artist;
		}
	}

	for (const auto& a : unique)
	{
		results.emplace_back(make_link(s, a.first, a.second, search_result));
	}

	return results;
}

static std::vector<view_element_ptr> create_retro_elements(view_state& s, const prop::item_metadata_const_ptr& md,
                                                           const df::search_result& search_result)
{
	std::vector<view_element_ptr> results;
	if (!is_empty(md->system)) results.emplace_back(make_link(s, md->system, prop::system, search_result));
	if (!is_empty(md->game)) results.emplace_back(make_link(s, md->game, prop::game, search_result));
	return results;
}

static std::vector<view_element_ptr> create_location_elements(view_state& s, const prop::item_metadata_const_ptr& md,
                                                              const df::search_result& search_result)
{
	std::vector<view_element_ptr> results;
	const auto location_link = [&s, &search_result](const str::cached text, const prop::key_ref prop,
	                                                const df::location_level level)
	{
		auto search = df::search_t().location(text.sv(), level);
		return make_link(s, text, search, prop, search_result);
	};

	if (!is_empty(md->location_place))
		results.emplace_back(location_link(md->location_place, prop::location_place, df::location_level::place));
	if (!is_empty(md->location_state))
		results.emplace_back(location_link(md->location_state, prop::location_state, df::location_level::state));
	if (!is_empty(md->location_country))
		results.emplace_back(location_link(md->location_country, prop::location_country, df::location_level::country));

	// locations.md 2.5 step 1: stored text wins outright. Only a file that names no place at all
	// falls through to the derived answer below.
	if (!is_empty(md->location_place) || !is_empty(md->location_state) || !is_empty(md->location_country))
	{
		return results;
	}

	const auto* const derived = s.derived_location(md->coordinate);

	// The gazetteer read that names a coordinate finishes after the panel is on screen. The row is
	// held open until it does: a coordinate is already proof there is a place to name, and a row that
	// appears from nothing moves the media and every property around it. It says so rather than
	// showing a blank, which in a location row reads as "this photo has none".
	if (!derived)
	{
		if (md->coordinate.is_valid())
		{
			results.emplace_back(std::make_shared<text_element>(tt.loading.sv()));
		}

		return results;
	}

	if (!derived->is_located()) return results;

	// locations.md 2.5: the label states how far it had to reach. `Near` is display honesty --
	// the item still carries that place's identity, so it groups and searches as that place.
	const auto has_place = !str::is_empty(derived->place.place);
	const auto search_text = has_place ? derived->place.place.sv() : derived->place.country.sv();
	auto display = qualified_name(derived->place);

	if (derived->attribution == location_attribution::near)
	{
		display = str_format(tt.location_near_fmt.sv(), display);
	}

	if (display.empty()) display = std::string(tt.location_remote.sv());

	if (search_text.empty())
	{
		results.emplace_back(std::make_shared<text_element>(display));
	}
	else
	{
		auto search = df::search_t().location(search_text,
		                                      has_place ? df::location_level::place : df::location_level::country);
		results.emplace_back(make_link(s, display, search,
		                               has_place ? prop::location_place : prop::location_country, search_result));
	}

	// locations.md 2.7: never a group key and never a search term, so it is plain text and answers
	// only "where was that?".
	if (auto bearing = bearing_descriptor(*derived); !bearing.empty())
	{
		results.emplace_back(std::make_shared<text_element>(bearing));
	}

	return results;
}

static std::vector<view_element_ptr> create_copyright_elements(view_state& s, const prop::item_metadata_const_ptr& md,
                                                               const df::search_result& search_result)
{
	std::vector<view_element_ptr> results;
	if (!is_empty(md->copyright_notice))
		results.emplace_back(
			make_link(s, md->copyright_notice, prop::copyright_notice, search_result));
	if (!is_empty(md->copyright_creator))
		results.emplace_back(
			make_link(s, md->copyright_creator, prop::copyright_creator, search_result));
	if (!is_empty(md->copyright_credit))
		results.emplace_back(
			make_link(s, md->copyright_credit, prop::copyright_credit, search_result));
	if (!is_empty(md->copyright_source))
		results.emplace_back(
			make_link(s, md->copyright_source, prop::copyright_source, search_result));
	if (!is_empty(md->copyright_url))
		results.emplace_back(
			make_link(s, md->copyright_url, prop::copyright_url, search_result));
	return results;
}

static std::vector<view_element_ptr> create_tag_elements(view_state& s, const prop::item_metadata_const_ptr& md,
                                                         const df::search_result& search_result)
{
	std::vector<view_element_ptr> results;

	const auto tag_parts = split(md->tags, true);

	for (const auto& t : tag_parts)
	{
		auto e = std::make_shared<search_element>(s, t, df::search_t().with(prop::tag, t));
		if (search_result.is_match(prop::tag, t)) e->set_style_bit(view_element_style::important, true);
		results.emplace_back(e);
	}

	return results;
}

static void add_row(const std::shared_ptr<ui::table_element>& result, const std::string_view label,
                    std::vector<view_element_ptr> e1, std::vector<view_element_ptr> e2)
{
	if (!e1.empty() || !e2.empty())
	{
		result->add(std::vector<view_element_ptr>{
			std::make_shared<text_element>(label), std::make_shared<view_elements>(e1),
			std::make_shared<view_elements>(e2)
		});
	}
}

static void add_media_elements(view_state& s, const prop::item_metadata_const_ptr& md,
                               std::vector<view_element_ptr>& video,
                               std::vector<view_element_ptr>& audio, const df::search_result& search_result)
{
	const auto video_codec = md->video_codec;
	const auto pixel_format = md->pixel_format;
	const auto bitrate = md->bitrate.sz();
	const auto audio_codec = md->audio_codec;
	const auto audio_channels = md->audio_channels;
	const auto audio_sample_rate = md->audio_sample_rate;
	const auto audio_sample_type = static_cast<prop::audio_sample_t>(md->audio_sample_type);

	if (!is_empty(video_codec)) video.emplace_back(make_link(s, video_codec, prop::video_codec, search_result));
	if (!is_empty(pixel_format)) video.emplace_back(make_link(s, pixel_format, prop::pixel_format, search_result));
	if (!str::is_empty(bitrate)) video.emplace_back(make_link(s, bitrate, prop::bitrate, search_result));

	if (!prop::is_null(audio_sample_rate))
		audio.emplace_back(make_link(
			s, prop::format_audio_sample_rate(audio_sample_rate),
			df::search_t().with(prop::audio_sample_rate, static_cast<int>(md->audio_sample_rate)),
			prop::audio_sample_rate,
			search_result));
	if (audio_sample_type != prop::audio_sample_t::none)
		audio.emplace_back(make_link(
			s, format_audio_sample_type(audio_sample_type),
			df::search_t().with(prop::audio_sample_type, md->audio_sample_type), prop::audio_sample_type,
			search_result));
	if (!prop::is_null(audio_channels))
		audio.emplace_back(make_link(s, prop::format_audio_channels(audio_channels),
		                             df::search_t().with(
			                             prop::audio_channels, md->audio_channels),
		                             prop::audio_channels, search_result));
	if (!prop::is_null(audio_codec)) audio.emplace_back(make_link(s, audio_codec, prop::audio_codec, search_result));
}


// The same count badge the title draws, so the bubble opens with the number the pointer is on.
class title_badge_element final : public std::enable_shared_from_this<title_badge_element>, public view_element
{
	const std::string _text;
	const ui::color32 _background;

public:
	title_badge_element(std::string text, const ui::color32 background,
	                    const view_element_options& options) noexcept :
		view_element(options), _text(std::move(text)), _background(background)
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		dc.draw_text(_text, bounds.offset(element_offset), ui::style::font_face::dialog,
		             ui::style::text_style::single_line_center,
		             ui::color(dc.colors.foreground, dc.colors.alpha),
		             ui::color(_background, dc.colors.alpha * dc.colors.bg_alpha));
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto extent = mc.measure_text(_text, ui::style::font_face::dialog,
		                                    ui::style::text_style::single_line_center, width_limit, mc.icon_cxy);
		return {extent.cx + mc.padding2, extent.cy};
	}
};

class title_link_element final : public std::enable_shared_from_this<title_link_element>, public text_element_base
{
	view_state& _state;
	const df::item_element_ptr _item;
	struct sidecar_attribute
	{
		std::string name;
		df::date_t modified;
		df::file_size size;
	};

	mutable std::string _sidecar_key;
	mutable std::vector<sidecar_attribute> _sidecar_attributes;
	mutable bool _sidecar_attribute_pending = false;
	mutable uint64_t _sidecar_attribute_generation = 0;

	void queue_sidecar_attributes(const std::vector<std::string>& sidecar_parts) const
	{
		const auto key = str::combine(sidecar_parts, "|");
		if (_sidecar_key != key)
		{
			_sidecar_key = key;
			_sidecar_attributes.clear();
			_sidecar_attribute_pending = false;
			++_sidecar_attribute_generation;
		}

		if (_sidecar_attribute_pending || _sidecar_attributes.size() == sidecar_parts.size()) return;

		_sidecar_attribute_pending = true;
		const auto generation = ++_sidecar_attribute_generation;
		const auto folder = _item->folder();
		const auto weak = weak_from_this();
		auto parts = sidecar_parts;
		auto& async = _state._async;

		_state.queue_async(async_queue::work, [weak, generation, folder, parts = std::move(parts), &async]() mutable
		{
			std::vector<sidecar_attribute> attributes;
			attributes.reserve(parts.size());

			for (const auto& part : parts)
			{
				const auto attribs = platform::file_attributes(folder.combine_file(part));
				attributes.push_back({part, df::date_t(attribs.modified), df::file_size(attribs.size)});
			}

			async.queue_ui([weak, generation, attributes = std::move(attributes)]() mutable
			{
				const auto self = weak.lock();
				if (!self || self->_sidecar_attribute_generation != generation) return;

				self->_sidecar_attribute_pending = false;
				self->_sidecar_attributes = std::move(attributes);
				self->_state.invalidate_view(view_invalid::tooltip);
			});
		});
	}

public:
	title_link_element(view_state& s, df::item_element_ptr i, const std::string_view text,
	                   const view_element_options& style_in) noexcept :
		text_element_base(text, style_in | view_element_style::has_tooltip | view_element_style::can_invoke),
		_state(s), _item(std::move(i))
	{
		_font = ui::style::font_face::title;
		_text_style = ui::style::text_style::single_line_center;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			df::related_info r;
			r.load(_item);
			_state.open(event.host, df::search_t().related(r), {{}});
			_state.view_mode(view_type::items);
		}
		else if (event.type == view_element_event_type::dpi_changed)
		{
			_tl.reset();
		}
	}

	// Trailing count badges. Both are read live each time the title draws, so a presence or
	// duplicate result that resolves after the panel was built appears without a rebuild.
	struct title_badge
	{
		std::string text;
		ui::color32 background;
	};

	std::vector<title_badge> badges() const
	{
		std::vector<title_badge> result;

		const auto sidecars = _item->sidecars_count();

		if (sidecars > 0)
		{
			result.emplace_back(str::to_string(sidecars), app_colors::sidecar_background);
		}

		// Issue #137 - the count includes this file, so below two it says "no other copy", which the
		// absence of a badge already says. Presence is still the gate: a count drawn while the check
		// is running would read as an answer it has not reached.
		if (_item->presence() != item_presence::unknown && _item->duplicates().count > 1)
		{
			result.emplace_back(str::format_count(_item->duplicates().count),
			                    app_colors::duplicate_background);
		}

		return result;
	}

	// The badges draw inside the title's own bounds and render takes their width out of the text, so
	// a measurement of the text alone hands back bounds that clip the last characters of the title.
	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto& tl = text_layout(mc);
		if (!tl) return {};

		auto extent = tl->measure_text(width_limit);
		auto width = extent.cx + mc.padding2;

		for (const auto& badge : badges())
		{
			const auto badge_extent = mc.measure_text(badge.text, ui::style::font_face::dialog,
			                                          ui::style::text_style::single_line_center,
			                                          width_limit, extent.cy);
			width += badge_extent.cx + mc.padding2;
		}

		return {std::min(width, width_limit), extent.cy};
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		// Built here rather than assumed, so a layout that failed while the device was lost does not
		// leave the title permanently blank.
		const auto& tl = text_layout(dc);

		if (tl)
		{
			const auto text_extent = tl->measure_text(logical_bounds.width() + 100);
			const auto bg_alpha = dc.colors.alpha * dc.colors.bg_alpha;
			const auto bg = calc_background_color(dc);

			const auto min_width = std::min(100, text_extent.cx);
			const auto show_badges = logical_bounds.width() > min_width;
			const auto badge_list = show_badges ? badges() : std::vector<title_badge>{};

			auto badge_widths = std::vector<int>(badge_list.size());
			auto badges_width = 0;

			for (size_t i = 0; i < badge_list.size(); ++i)
			{
				const auto extent = dc.measure_text(badge_list[i].text, ui::style::font_face::dialog,
				                                    ui::style::text_style::single_line_center,
				                                    logical_bounds.width(), logical_bounds.height());
				badge_widths[i] = extent.cx + dc.padding2;
				badges_width += badge_widths[i];
			}

			const auto text_width = std::max(0, std::min(text_extent.cx + dc.padding2,
			                                             bounds.width() - badges_width));

			if (bg.a > 0.0f)
			{
				auto bg_bounds = logical_bounds;
				bg_bounds.right = bg_bounds.left + text_width + badges_width;
				dc.draw_rounded_rect(bg_bounds, bg, dc.padding1);
			}

			const ui::color text_clr(dc.colors.foreground, dc.colors.alpha);

			auto text_bounds = logical_bounds;
			text_bounds.right = text_bounds.left + text_width;
			dc.draw_text(tl, text_bounds, text_clr, {});

			auto x = text_bounds.right;

			for (size_t i = 0; i < badge_list.size(); ++i)
			{
				const recti badge_bounds(x, logical_bounds.top, x + badge_widths[i], logical_bounds.bottom);
				dc.draw_text(badge_list[i].text, badge_bounds, ui::style::font_face::dialog,
				             ui::style::text_style::single_line_center, text_clr,
				             ui::color(badge_list[i].background, bg_alpha));
				x += badge_widths[i];
			}
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		const auto i = _item;

		auto row = std::make_shared<view_elements>(flex_item::line_break);
		row->add(std::make_shared<text_element>(tt.presence_tile, ui::style::font_face::dialog,
													ui::style::text_style::single_line,
				                                               view_element_style::none));

		// The badges the title carries sit at the top right of the bubble, so the number the pointer
		// is on is still visible while its explanation is read.
		const auto badge_list = badges();

		if (!badge_list.empty())
		{
			for (const auto& badge : badge_list)
			{
				row->add(std::make_shared<title_badge_element>(badge.text, badge.background,
				                                               view_element_style::none | flex_item::right_justified));
			}
		}

		hover.elements->add(std::move(row));

		hover.elements->add(std::make_shared<text_element>(item_presence_text(i->presence(), true),
													ui::style::font_face::dialog,
													ui::style::text_style::multiline,
													flex_item::line_break));

		if (i)
		{													   
			const auto sidecars = i->sidecars();
			const bool has_sidecars = !sidecars.is_empty();

			if (has_sidecars)
			{
				const auto table = std::make_shared<ui::table_element>(flex_item::center);
				table->no_shrink_col[1] = true;
				table->no_shrink_col[2] = true;

				table->add(tt.prop_name_filename, tt.prop_name_modified, tt.prop_name_size);

				const auto sidecar_parts = split(sidecars, true);
				const std::set<std::string, df::iless> unique(sidecar_parts.begin(), sidecar_parts.end());
				std::vector<std::string> ordered_unique(unique.begin(), unique.end());
				queue_sidecar_attributes(ordered_unique);

				for (const auto& part : ordered_unique)
				{
					const auto found = std::ranges::find(_sidecar_attributes, part, &sidecar_attribute::name);
					table->add(part, ui::average(app_colors::sidecar_background, ui::style::color::view_text),
					           found == _sidecar_attributes.end()
						           ? std::string{}
						           : platform::format_date(found->modified.system_to_local()),
					           found == _sidecar_attributes.end() ? std::string{} : df::format_size(found->size));
				}

				hover.elements->add(table);
			}

			// The copies the badge counts, then how this item stands against the collection. They are
			// different claims - one about redundancy, one about membership - so they are stated
			// separately rather than folded into the badge (docs/collections.md section 6).
			const auto duplicates = i->duplicates();

			if (duplicates.group != 0)
			{
				const auto related = _state.item_index.duplicate_list(duplicates.group);

				if (!related.empty())
				{
					const auto table = std::make_shared<ui::table_element>(flex_item::center);
					table->no_shrink_col[1] = true;
					table->no_shrink_col[2] = true;
					table->add(tt.prop_name_filename, tt.prop_name_modified, tt.prop_name_size);
					table->add(i->name(), platform::format_date(i->file_modified().system_to_local()),
					           df::format_size(i->file_size()));

					for (const auto& item : related)
					{
						if (item.first != i->path())
						{
							table->add(item.second.name,
							           ui::average(app_colors::duplicate_background,
							                       ui::style::color::view_text),
							           platform::format_date(item.second.file_modified.load().system_to_local()),
							           df::format_size(item.second.size));
						}
					}

					hover.elements->add(table);
				}
			}
		}

		hover.elements->add(std::make_shared<action_element>(tt.show_related));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

static prop::item_metadata_const_ptr safe_metadata(const df::item_element_ptr& i)
{
	auto md = i->metadata();
	return md ? md : std::make_shared<const prop::item_metadata>();
}

static std::vector<view_element_ptr> create_comp_controls(view_state& s, const df::item_element_ptr& i)
{
	std::vector<view_element_ptr> controls;
	controls.emplace_back(std::make_shared<rate_label_control>(s, i, true, view_element_style::none));

	if (i->file_type()->has_trait(file_traits::edit))
	{
		controls.emplace_back(std::make_shared<rating_control>(s, i, true, view_element_style::none));
	}

	// Pin, Unselect, and Delete are the three ways a comparison ends: keep this one, drop this
	// one, destroy this one. Pinning holds this column so the next item compares against it.
	controls.emplace_back(std::make_shared<pin_control>(s, i, true, view_element_style::none));
	controls.emplace_back(std::make_shared<delete_element>(s, i, view_element_style::none));
	controls.emplace_back(std::make_shared<unselect_element>(s, i, view_element_style::none));
	return controls;
}

void append_bullet(std::vector<view_element_ptr>& result, icon_index icon,
                   const std::vector<view_element_ptr>& children)
{
	if (!children.empty())
	{
		view_element_ptr elements = std::make_shared<view_elements>(children);
		view_element_ptr bullet = std::make_shared<bullet_element>(icon, elements, view_element_style::none);
		result.emplace_back(bullet);
	}
}

std::shared_ptr<text_element> make_rank_element(std::string text, const bool is_rank)
{
	auto result = std::make_shared<text_element>(text);
	if (is_rank) result->foreground_color(calc_rank_color());
	return result;
}

// The commands that act on the selection belong with the panel that describes it, so the command
// and the thing it changes are always read together. Commands that cannot run dim in place rather
// than disappearing, so the set stays stable as the selection changes.
static void add_command_links(const view_state& s, const view_elements_ptr& row,
                              const std::initializer_list<commands> ids)
{
	for (const auto id : ids)
	{
		auto command = s.find_command(id);
		if (command) row->add(std::make_shared<command_link_element>(std::move(command)));
	}
}

// A field the user can fill in leads with the command that edits it, so the icon in front of the
// values is the way in. A neutral field icon plus a separate button would say the same thing twice,
// and the line then reads the same whether it has values or not.
static void append_editable_bullet(const view_state& s, std::vector<view_element_ptr>& elements,
                                   const commands id, const std::vector<view_element_ptr>& values)
{
	auto row = std::make_shared<view_elements>();

	if (auto command = s.find_command(id))
	{
		row->add(std::make_shared<command_link_element>(std::move(command), flex_item::no_break));
	}

	row->add(values);

	if (!row->is_empty()) elements.emplace_back(std::move(row));
}

// Selections that have no title row of their own still need stable navigation and selected-set
// actions. Separate wrapping regions keep narrow panels from hiding trailing commands.
view_elements_ptr view_state::create_selection_controls(const bool compact)
{
	auto result = std::make_shared<view_elements>(flex_item::stretch);

	auto& s = *this;
	const auto& selected = selected_items();

	const auto d = _display;

	if (d)
	{
		if (d->is_one())
		{
			std::vector<view_element_ptr> elements;

			const auto item = d->_item1;
			const auto is_media = item->is_media();
			const auto ft = item->file_type();
			const auto search_result = item->search();

			if (is_media)
			{
				const auto& info = d->_player_media_info;
				const auto md = item->metadata();

				// The decoder could not make media of this file - a TypeScript .ts is the usual case - so
				// the panel offers no transport. The pane below it is a hex dump, and a scrubber over that
				// claims a position in something there is no way to play.
				const auto is_playable = ft->has_trait(file_traits::av) && !d->_av_open_failed;

				if (is_playable)
				{
					auto transport = std::make_shared<view_elements>(
						flex_item::grow | flex_item::line_break);
					transport->add(std::make_shared<play_control>(s, flex_item::no_break));
					transport->add(std::make_shared<scrubber_element>(_player, d));
					elements.emplace_back(transport);
				}

				const auto name = item->path().file_name_without_extension();
				const auto title_text = md && !is_empty(md->title) ? md->title.sv() : name;
				flex_item_layout title_layout;
				title_layout.shrink = 1.0f;
				title_layout.min_size.cx = 64;
				elements.emplace_back(std::make_shared<title_link_element>(*this, item, title_text, title_layout));

				// The title takes the width it needs and everything after it is pushed to the right,
				// so the viewing group carries the justification the badge used to.
				auto viewing = std::make_shared<view_elements>(flex_item::right_justified);

				if (is_playable)
				{
					add_command_links(s, viewing, {commands::menu_playback});
				}

				if (auto slideshow_command = find_command(commands::slideshow))
				{
					auto link = std::make_shared<command_link_element>(std::move(slideshow_command));
					// A held photo reports how much of the delay has elapsed. Video and audio report their
					// own position on the scrubber, so the toggle shows the mode and no fill.
					link->progress = [d] { return d->is_playing_slideshow() ? d->slideshow_pos() : -1; };
					viewing->add(std::move(link));
				}

				// Pin holds this item so the following one joins it instead of replacing it, which is how
				// a user reaches comparison deliberately. It changes what is on screen, not the file.
				viewing->add(std::make_shared<pin_control>(s, item, true, view_element_style::none));

				if (md && md->orientation != ui::orientation::top_left && md->orientation != ui::orientation::none)
				{
					viewing->add(make_icon_link_element(icon_index::orientation, commands::option_show_rotated,
					                                    view_element_style::none));
				}

				if (d->_selected_texture1 && d->_selected_texture1->can_preview())
				{
					viewing->add(std::make_shared<preview_control>(s, d->_selected_texture1, true,
					                                               view_element_style::none));
				}

				add_command_links(s, viewing, {commands::option_scale_up, commands::view_fullscreen});
				elements.emplace_back(viewing);

				auto actions = std::make_shared<view_elements>();
				actions->add(std::make_shared<rate_label_control>(s, item, true, view_element_style::none));

				if (ft->has_trait(file_traits::edit))
				{
					actions->add(std::make_shared<rating_control>(s, item, true, view_element_style::none));
				}

				add_command_links(s, actions, {
					                  commands::tool_rotate_anticlockwise, commands::tool_rotate_clockwise,
					                  commands::tool_edit,
					                  commands::menu_open, commands::menu_tools_toolbar
				                  });
				elements.emplace_back(actions);
				elements.emplace_back(std::make_shared<divider_element>());

				if (compact)
				{
					// When the photo was taken describes the subject, not the file that holds it, so it
					// survives full screen even though the folder, filename and size do not.
					append_bullet(elements, icon_index::time,
					              {std::make_shared<items_dates_control>(s, item)});
				}
				else
				{
					if (!_search.is_showing_folder())
					{
						elements.emplace_back(std::make_shared<bullet_element>(
							icon_index::folder,
							std::make_shared<link_element>(std::format("{}\\", item->folder().text()),
							                               commands::browse_open_containingfolder),
							view_element_style::none));
					}

					std::vector<view_element_ptr> file_elements;
					auto file_browser_element = std::make_shared<link_element>(
						item->path().name(), commands::browse_open_in_file_browser);
					if (search_result.is_match(prop::file_name))
						file_browser_element->set_style_bit(
							view_element_style::important, true);
					file_elements.emplace_back(file_browser_element);
					file_elements.emplace_back(std::make_shared<items_dates_control>(s, item));

					auto file_size = std::make_shared<text_element>(item->file_size().str(), view_element_style::none);
					if (search_result.is_match(prop::file_size))
						file_size->set_style_bit(
							view_element_style::important, true);
					file_elements.emplace_back(file_size);

					append_bullet(elements, icon_index::document, file_elements);
				}

				if (md)
				{
					// design.md: the overlay identifies, it does not summarise. A photograph or a video
					// is already on screen, so its name, date, rating and label are the whole of what an
					// overlay adds; anything more competes with the only thing the Media view exists for,
					// and the questions it answers are asked in the panel and the metadata pane. Audio is
					// the exception - the media is not a picture, so the overlay *is* the presentation.
					const auto overlay_is_the_presentation = !compact || ft->has_trait(file_traits::music_metadata);

					// Compact keeps what describes the subject and drops what describes the container,
					// because full screen is where the picture is read, not the file.
					std::vector<view_element_ptr> pixel_elements;
					std::vector<view_element_ptr> audio_elements;

					if (!compact)
					{
						if (md->width && md->height)
						{
							for (const auto& e : format_dims(md->width, md->height, ft, false))
							{
								pixel_elements.emplace_back(e);
							}
						}

						const auto video_codec = info.video_codec.is_empty() ? md->video_codec : info.video_codec;
						const auto pixel_format = info.pixel_format.is_empty() ? md->pixel_format : info.pixel_format;
						const auto bitrate = info.bitrate ? prop::format_bit_rate(info.bitrate) : md->bitrate.sz();
						const auto audio_codec = prop::is_null(info.audio_codec) ? md->audio_codec : info.audio_codec;
						const auto audio_channels = info.audio_channels == 0 ? md->audio_channels : info.audio_channels;
						const auto audio_sample_rate = prop::is_null(info.audio_sample_rate)
							                               ? md->audio_sample_rate
							                               : info.audio_sample_rate;
						const auto audio_sample_type = info.audio_sample_type == prop::audio_sample_t::none
							                               ? static_cast<prop::audio_sample_t>(md->audio_sample_type)
							                               : info.audio_sample_type;

						if (!is_empty(video_codec))
							pixel_elements.emplace_back(
								make_link(s, video_codec, prop::video_codec, search_result));
						if (!is_empty(pixel_format))
							pixel_elements.emplace_back(
								make_link(s, pixel_format, prop::pixel_format, search_result));
						if (!str::is_empty(bitrate))
							pixel_elements.emplace_back(
								make_link(s, bitrate, prop::bitrate, search_result));

						if (!prop::is_null(audio_sample_rate))
							audio_elements.emplace_back(
								make_link(s, prop::format_audio_sample_rate(audio_sample_rate),
								          df::search_t().with(prop::audio_sample_rate, static_cast<int>(audio_sample_rate)),
								          prop::audio_sample_rate, search_result));
						if (audio_sample_type != prop::audio_sample_t::none)
							audio_elements.emplace_back(
								make_link(s, format_audio_sample_type(audio_sample_type),
								          df::search_t().with(prop::audio_sample_type,
								                              static_cast<int>(audio_sample_type)),
								          prop::audio_sample_type, search_result));
						if (!prop::is_null(audio_channels))
							audio_elements.emplace_back(
								make_link(s, prop::format_audio_channels(audio_channels),
								          df::search_t().with(prop::audio_channels, audio_channels),
								          prop::audio_channels, search_result));
						if (!prop::is_null(audio_codec))
							audio_elements.emplace_back(
								make_link(s, audio_codec, prop::audio_codec, search_result));

						append_bullet(elements, ft->icon, pixel_elements);
						append_bullet(elements, icon_index::audio, audio_elements);
						append_bullet(elements, icon_index::camera, create_camera_elements(s, md, search_result));
					}

					if (overlay_is_the_presentation)
					{
						append_bullet(elements, icon_index::disk, create_album_elements(s, md, search_result));
						append_bullet(elements, icon_index::person, create_artist_elements(s, md, search_result));
						append_bullet(elements, icon_index::retro, create_retro_elements(s, md, search_result));
					}

					if (compact)
					{
						// Where it was taken describes the subject as much as when it was, so location keeps
						// its place beside the date in full screen. It identifies rather than invites an
						// edit, so it carries no command icon and is absent when the item has no place.
						append_bullet(elements, icon_index::location,
						              create_location_elements(s, md, search_result));
					}

					if (!compact)
					{
						append_bullet(elements, icon_index::copyright,
						              create_copyright_elements(s, md, search_result));
					}
				}

				if (!compact)
				{
					// Location, Tags and Description close the panel. Location and Tags lead with the
					// command that edits them, so the icon in front of the row both names the field and
					// opens it.
					std::vector<view_element_ptr> location_elements;
					if (md) location_elements = create_location_elements(s, md, search_result);
					append_editable_bullet(s, elements, commands::tool_locate, location_elements);

					std::vector<view_element_ptr> tag_elements;
					if (md) tag_elements = create_tag_elements(s, md, search_result);
					constexpr size_t max_tags = 6;
					if (tag_elements.size() > max_tags)
					{
						const auto hidden_count = tag_elements.size() - max_tags;
						tag_elements.resize(max_tags);
						tag_elements.emplace_back(std::make_shared<text_element>(std::format("+{}", hidden_count)));
					}
					append_editable_bullet(s, elements, commands::tool_tag, tag_elements);

					// A populated description gets its own section below this panel, and that section
					// carries the same edit command, so only its absence needs an affordance here.
					if (!md || is_empty(md->description))
					{
						if (auto description_command = s.find_command(commands::tool_edit_description))
						{
							elements.emplace_back(
								std::make_shared<command_link_element>(std::move(description_command)));
						}
					}
				}

				result->add(elements);
			}
			else
			{
				const auto table = std::make_shared<ui::table_element>(flex_item::center);
				table->no_shrink_col[0] = true;

				// Nothing here can be rotated, edited, or stepped through as media, so the row carries only
				// what applies to an unsupported file: who it is, whether copies exist, the hold, its
				// grade, the way out to an application that can open it, and the same route to the rest.
				std::vector<view_element_ptr> controls;
				controls.emplace_back(
					std::make_shared<title_link_element>(*this, item, item->name(), flex_item::grow));
				controls.emplace_back(
					std::make_shared<pin_control>(s, item, true, view_element_style::none));
				controls.emplace_back(
					std::make_shared<rate_label_control>(s, item, true, view_element_style::none));

				if (auto open_command = s.find_command(commands::menu_open))
				{
					controls.emplace_back(std::make_shared<command_link_element>(std::move(open_command)));
				}

				if (auto tools_command = s.find_command(commands::menu_tools_toolbar))
				{
					controls.emplace_back(std::make_shared<command_link_element>(std::move(tools_command)));
				}

				table->add(tt.sort_by_name, std::make_shared<view_elements>(controls));

				if (!_search.is_showing_folder())
				{
					table->add(tt.folder_title, item->folder().text());
				}

				auto file_size = std::make_shared<text_element>(item->file_size().str(), view_element_style::none);
				if (search_result.is_match(prop::file_size))
					file_size->set_style_bit(
						view_element_style::important, true);
				table->add(tt.size_title, file_size);

				auto media_created = std::make_shared<text_element>(platform::format_date_time(item->media_created()),
				                                                    view_element_style::none);
				if (search_result.type == df::search_result_type::match_date && s.search().
					is_match(prop::created_exif, item->media_created()))
					media_created->set_style_bit(
						view_element_style::important, true);
				table->add(tt_prep(tt.prop_name_original.sv()), media_created);

				auto file_modified = std::make_shared<text_element>(
					platform::format_date_time(item->file_modified().system_to_local()),
					view_element_style::none);
				if (search_result.type == df::search_result_type::match_date && s.search().
					is_match(prop::modified, item->file_modified().system_to_local()))
					file_modified->set_style_bit(
						view_element_style::important, true);
				table->add(tt.prop_name_modified, file_modified);

				const auto icon = ft->icon;
				auto split = std::make_shared<split_element>(
					platform::create_icon_surface(static_cast<char32_t>(icon)), table, flex_item::center);
				split->padding = {4, 4};
				split->margin = {4, 4};
				result->add(split);
			}
		}
		else if (d->is_comparison())
		{
			// No command row here: with two items displayed the target of an item action is ambiguous.
			auto table = std::make_shared<ui::table_element>(flex_item::center);
			table->no_shrink_col[0] = true;

			const auto i1 = d->_item1;
			const auto i2 = d->_item2;
			const auto search_result1 = i1->search();
			const auto search_result2 = i2->search();

			const auto controls1 = create_comp_controls(s, i1);
			const auto controls2 = create_comp_controls(s, i2);
			add_row(table, {}, controls1, controls2);

			// The name carries its own copy count, so the comparison spends no extra row or column on it.
			std::vector<view_element_ptr> name1{
				std::make_shared<title_link_element>(*this, i1, i1->name(), flex_item::grow)
			};
			std::vector<view_element_ptr> name2{
				std::make_shared<title_link_element>(*this, i2, i2->name(), flex_item::grow)
			};
			table->add(tt.sort_by_name, std::make_shared<view_elements>(name1),
			           std::make_shared<view_elements>(name2));

			table->add(tt.folder_title,
			           std::make_shared<search_element>(s, i1->folder().text(),
			                                            df::search_t().add_selector(i1->folder())),
			           std::make_shared<search_element>(s, i2->folder().text(),
			                                            df::search_t().add_selector(i2->folder())));

			table->add(tt.size_title,
			           make_rank_element(i1->file_size().str(), i1->file_size() > i2->file_size()),
			           make_rank_element(i2->file_size().str(), i1->file_size() < i2->file_size()));

			table->add(tt_prep(tt.prop_name_original.sv()),
			           make_rank_element(platform::format_date_time(i1->media_created()),
			                             i1->media_created() > i2->media_created()),
			           make_rank_element(platform::format_date_time(i2->media_created()),
			                             i1->media_created() < i2->media_created()));

			table->add(tt.prop_name_modified,
			           make_rank_element(platform::format_date_time(i1->file_modified().system_to_local()),
			                             i1->file_modified() > i2->file_modified()),
			           make_rank_element(platform::format_date_time(i2->file_modified().system_to_local()),
			                             i1->file_modified() < i2->file_modified()));

			// A comparison row states what has arrived. Waiting for both snapshots leaves the table blank
			// while one side loads, which reads as no difference rather than not yet known.
			const auto md1 = safe_metadata(i1);
			const auto md2 = safe_metadata(i2);

			if (md1->duration && md2->duration)
			{
				table->add(tt.prop_name_duration, str::format_seconds(md1->duration),
				           str::format_seconds(md2->duration));
			}

			// uint16_t operands promote to int, which overflows past about 46341 x 46341
			const auto pixels1 = static_cast<uint64_t>(md1->width) * md1->height;
			const auto pixels2 = static_cast<uint64_t>(md2->width) * md2->height;

			std::vector<view_element_ptr> video1 = format_dims(md1->width, md1->height, i1->file_type(),
			                                                   pixels1 > pixels2);
			std::vector<view_element_ptr> video2 = format_dims(md2->width, md2->height, i2->file_type(),
			                                                   pixels1 < pixels2);
			std::vector<view_element_ptr> audio1;
			std::vector<view_element_ptr> audio2;

			add_media_elements(s, md1, video1, audio1, search_result1);
			add_media_elements(s, md2, video2, audio2, search_result2);

			add_row(table, tt.pixels_title, video1, video2);
			add_row(table, tt.audio_title, audio1, audio2);

			add_row(table, tt.prop_name_camera, create_camera_elements(s, md1, search_result1),
			        create_camera_elements(s, md2, search_result2));
			add_row(table, tt.prop_name_album, create_album_elements(s, md1, search_result1),
			        create_album_elements(s, md2, search_result2));
			add_row(table, tt.prop_name_artist, create_artist_elements(s, md1, search_result1),
			        create_artist_elements(s, md2, search_result2));
			add_row(table, tt.retro_title, create_retro_elements(s, md1, search_result1),
			        create_retro_elements(s, md2, search_result2));
			add_row(table, tt.location_title, create_location_elements(s, md1, search_result1),
			        create_location_elements(s, md2, search_result2));
			add_row(table, tt.copyright_title, create_copyright_elements(s, md1, search_result1),
			        create_copyright_elements(s, md2, search_result2));
			add_row(table, tt.tags_title, create_tag_elements(s, md1, search_result1),
			        create_tag_elements(s, md2, search_result2));

			if (!is_empty(md1->description) || !is_empty(md2->description))
			{
				table->add(tt.prop_name_description, md1->description, md2->description);
			}

			if (!is_empty(md1->comment) || !is_empty(md2->comment))
			{
				table->add(tt.prop_name_comment, md1->comment, md2->comment);
			}

			if (!is_empty(md1->synopsis) || !is_empty(md2->synopsis))
			{
				table->add(tt.prop_name_synopsis, md1->synopsis, md2->synopsis);
			}

			std::string_view identical_text;


			const bool crc32_loaded = i1->crc32c() != 0 && i2->crc32c() != 0;
			const auto file_size_same = i1->file_size() == i2->file_size();

			if (crc32_loaded && file_size_same && i1->crc32c() == i2->crc32c())
			{
				identical_text = tt.items_identical;
			}
			else if (d->_pixel_difference == ui::pixel_difference_result::equal)
			{
				identical_text = tt.pixels_identical_files_not_identical;
			}
			else if (d->_pixel_difference == ui::pixel_difference_result::not_equal)
			{
				identical_text = tt.items_not_identical;
			}

			if (!identical_text.empty())
			{
				const auto element = std::make_shared<text_element>(identical_text, ui::style::font_face::dialog,
				                                                    ui::style::text_style::multiline,
				                                                    flex_item::line_break |
				                                                    flex_item::center);
				element->set_style_bit(view_element_style::info, true);
				result->add(set_margin(element));
			}

			result->add(table);
		}
		else if (!_selected.is_empty())
		{
			int folder_count = 0;
			int item_count = 0;

			df::item_element_ptr first_folder;
			df::item_element_ptr first_item;

			for (const auto& i : selected.items())
			{
				if (i->is_folder())
				{
					folder_count += 1;
					if (!first_folder) first_folder = i;
				}
				else
				{
					item_count += 1;
					if (!first_item) first_item = i;
				}
			}

			const auto elements = std::make_shared<view_elements>();

			if (folder_count > 0)
			{
				std::string title;

				if (folder_count == 1)
				{
					title = str_format(tt.title_folder.sv(), first_folder->name());
				}
				else
				{
					title = format_plural_text(tt.title_folder_count_fmt, folder_count);
				}

				elements->add(std::make_shared<text_element>(title, ui::style::font_face::title,
				                                             ui::style::text_style::multiline,
				                                             flex_item::line_break));
			}

			if (item_count > 0)
			{
				std::string title;

				if (item_count == 1)
				{
					title = std::format("{}:{}", first_item->file_type()->group->name, first_item->name());
				}
				else
				{
					title = format_plural_text(tt.title_item_count_fmt, item_count);
				}

				elements->add(std::make_shared<text_element>(title, ui::style::font_face::title,
				                                             ui::style::text_style::multiline,
				                                             flex_item::line_break));
			}

			if (folder_count + item_count > 0)
			{
				const auto summary = selected.summary();
				elements->add(std::make_shared<summary_control>(summary, flex_item::new_line));
			}

			// A set of files, drawn as a stack of sheets. It used to borrow the recursion glyph, which
			// is an expand-all arrow and reads as a download.
			auto icon = file_type::folder.icon;
			if (item_count > 0) icon = icon_index::set;
			result->add(std::make_shared<split_element>(platform::create_icon_surface(static_cast<char32_t>(icon)),
			                                            elements, flex_item::center));
			result->add(std::make_shared<divider_element>());

			// What the selection is comes first, then everything that acts on it in one centred row,
			// so the panel reads as a single statement instead of commands hunting for their subject.
			// The row must fill its line before justify can centre anything inside it.
			auto command_row = std::make_shared<view_elements>(
				flex_item::grow | flex_item::new_line | flex_item::line_break);
			command_row->flex_container.justify = flex_justify::center;
			add_command_links(s, command_row, {
				                  commands::tool_rotate_anticlockwise, commands::tool_rotate_clockwise,
				                  commands::tool_edit, commands::tool_tag, commands::menu_open,
				                  commands::menu_tools_toolbar
			                  });

			// A large selection cannot show the held item on every visible tile, so the panel names
			// it and offers the same release the badge does.
			if (_pin_item)
			{
				command_row->add(std::make_shared<pin_control>(s, _pin_item, true, view_element_style::none));
				command_row->add(std::make_shared<text_element>(_pin_item->name(),
				                                                ui::style::font_face::dialog,
				                                                ui::style::text_style::single_line,
				                                                view_element_style::none));
			}

			result->add(command_row);
		}
	}

	df::assert_true(selected.empty() || !result->is_empty());

	return result;
}

view_element_ptr view_state::create_selection_description()
{
	const auto d = _display;
	if (!d || !d->is_one()) return {};

	const auto md = d->_item1->metadata();
	if (!md) return {};

	// Fullscreen has room for one passage beside the controls, so it shows the leading prose field
	// under its own name rather than the items view's whole list.
	const auto fields = prop::descriptive_fields(*md);
	if (fields.empty()) return {};

	const auto& primary = fields.front();

	auto result = std::make_shared<view_elements>(flex_item::stretch);
	// Fullscreen lays this out as its own bounded panel, so the inset is the container's rather than
	// the surrounding view's.
	result->flex_container.padding = {8, 8};
	const auto title = std::make_shared<group_title_control>(primary.name);
	title->flex.break_after = true;
	if (auto command = find_command(commands::tool_edit_description))
	{
		title->elements.emplace_back(std::make_shared<command_link_element>(std::move(command)));
	}
	result->add(title);
	result->add(std::make_shared<text_element>(primary.text, ui::style::font_face::dialog,
	                                           ui::style::text_style::multiline,
	                                           flex_item::grow | flex_item::line_break));
	return result;
}

const located_place* view_state::derived_location(const gps_coordinate& coord)
{
	df::assert_true(ui::is_ui_thread());
	if (!coord.is_valid()) return nullptr;

	if (const auto found = _resolved_places.find(attribution_cell(coord)); found != _resolved_places.end())
	{
		return &found->second;
	}

	// Attribution reads the gazetteer file, so it never runs here. Queue it once and let the
	// published result invalidate the panel that asked.
	if (_resolving_places.emplace(coord).second)
	{
		queue_location([this, coord, generation = _resolved_places_generation](const location_cache& locations)
		{
			auto resolved = locations.find_attributed(coord);

			_async.queue_ui([this, coord, generation, resolved = std::move(resolved)]
			{
				// A display-language change clears the memo, which makes any result still in
				// flight an answer to a question nobody is asking now.
				if (generation != _resolved_places_generation) return;

				_resolving_places.erase(coord);
				_resolved_places.insert_or_assign(attribution_cell(coord), resolved);
				invalidate_view(view_invalid::media_elements);
			});
		});
	}

	return nullptr;
}

const uint64_t* view_state::day_item_count(const df::date_t d)
{
	df::assert_true(ui::is_ui_thread());
	if (!d.is_valid()) return nullptr;

	const auto st = d.date();
	const auto key = (static_cast<uint32_t>(st.year) << 9) | (static_cast<uint32_t>(st.month) << 5) |
		static_cast<uint32_t>(st.day);

	if (const auto found = _day_counts.find(key); found != _day_counts.end())
	{
		return &found->second;
	}

	// A day search has no selector, so counting it walks every indexed folder. Queue it once and let
	// the published result invalidate the tooltip that asked.
	if (_counting_days.emplace(key).second)
	{
		_async.queue_async(async_queue::query, [this, key, st, generation = _day_counts_generation]
		{
			// A shared version would make each queued day cancel the one before it; only shutdown
			// should stop a count.
			const auto matches = item_index.count_matches(df::search_t().day(st.day, st.month, st.year), {});
			const auto count = matches.total_items().count;

			_async.queue_ui([this, key, count, generation]
			{
				if (generation != _day_counts_generation) return;

				_counting_days.erase(key);
				_day_counts[key] = count;
				invalidate_view(view_invalid::tooltip);
			});
		});
	}

	return nullptr;
}

void view_state::invalidate_day_counts()
{
	df::assert_true(ui::is_ui_thread());

	_day_counts.clear();
	_counting_days.clear();
	++_day_counts_generation;
}

uint64_t view_state::count_total(const file_group_ref fg) const
{
	if (fg->id < static_cast<int>(_summary_total.counts.size()))
	{
		return _summary_total.counts[fg->id].count;
	}

	return 0;
}

df::item_element_ptr view_state::command_item() const
{
	const auto d = _display;

	if (d && d->is_one())
	{
		return d->_item1;
	}

	return _focus;
}

view_state::selection_status_result view_state::selection_status() const
{
	selection_status_result result;

	const auto d = _display;

	if (d)
	{
		result.is_playing = d->is_playing();
		result.is_playing_media = d->is_playing_media();
		result.is_slideshow = d->is_slideshow();
		result.can_play_media = d->can_play_media();
		result.can_zoom = d->can_zoom();

		if (d->is_one() && _selected.size() == 1)
		{
			const auto i = d->_item1;

			if (i)
			{
				const auto ft = i->file_type();
				result.has_single_media_selection = i->is_media();
				result.showing_image = ft->has_trait(file_traits::bitmap) || d->player_has_video();
			}
		}
	}

	result.has_single_folder_selection = _selected.size() == 1 && _selected.has_folders();

	return result;
}

bool view_state::can_edit_media() const
{
	const auto d = _display;

	if (d && d->is_one() && _selected.size() == 1)
	{
		const auto i = d->_item1;
		return i && i->file_type()->can_edit_photo();
	}

	return false;
}

bool view_state::should_show_overlays() const
{
	if (_display && _display->is_zoom_mode())
	{
		return ui::ticks_since_last_user_action < ui::default_ticks_per_second * 5 || df::command_active;
	}

	if (_selected.has_folders())
	{
		return true;
	}

	if (_selected.items().size() != 1 || _selected.size() != 1)
	{
		return true;
	}

	if (view_mode() == view_type::items)
	{
		return true;
	}

	if (!_selected.items()[0]->file_type()->has_trait(file_traits::hide_overlays))
	{
		return true;
	}

	// A playing video hides its chrome so the picture is alone. A paused one keeps it: the transport
	// is both how the user resumes and the only thing saying where in the file they stopped.
	if (_display && _display->can_play_media() && !_display->is_playing_media())
	{
		return true;
	}

	return ui::ticks_since_last_user_action < ui::default_ticks_per_second * 5 || df::command_active;
}

void view_state::stop_slideshow() const
{
	const auto d = _display;

	if (d)
	{
		d->stop_slideshow();
	}
}

void view_state::reset()
{
	close();

	_search_items.clear();
	_display_items.clear();
	_selected.clear();
	_focus.reset();
	_item_groups.clear();
	_edit_item.reset();
	_pin_item.reset();
	_display.reset();
}

static bool calc_search_is_favorite(const df::search_t& search)
{
	if (!search.is_empty())
	{
		for (auto i = 0; i < setting.search.count; i++)
		{
			if (!str::is_empty(setting.search.path[i]))
			{
				const auto fav = df::search_t::parse(setting.search.path[i]);

				if (search == fav)
				{
					return true;
				}
			}
		}
	}

	return false;
}

static bool calc_search_search_is_in_collection(const index_state& index, const df::search_t& search)
{
	const auto item_selectors = search.selectors();

	if (item_selectors.empty())
		return true;

	for (const auto& sel : item_selectors)
	{
		if (!index.is_in_collection(sel.folder()))
			return false;
	}

	return true;
}

void view_state::update_search_is_favorite_or_collection_root()
{
	_search_is_favorite = calc_search_is_favorite(_search);
	_search_is_in_collection = calc_search_search_is_in_collection(item_index, _search);
}

void view_state::update_pixel_difference() const
{
	const auto d = _display;

	if (d)
	{
		d->calc_pixel_difference();
	}
}

bool view_state::escape(const view_host_base_ptr& view)
{
	// Unwind most-local-first: the mode turned on last comes off first.
	const auto d = _display;

	if (d)
	{
		if (d->is_playing_slideshow())
		{
			stop_slideshow();
			return true;
		}

		if (d->is_zoom_mode())
		{
			d->zoom(false);
			return true;
		}
	}

	if (is_full_screen)
	{
		_events.toggle_full_screen();
		return true;
	}

	if (view_mode() != view_type::items)
	{
		view_mode(view_type::items);
		return true;
	}

	if (d && d->is_playing())
	{
		stop();
		return true;
	}

	if (has_error_items())
	{
		clear_error_items(view);
		invalidate_view(view_invalid::view_redraw);
		return true;
	}

	if (search().has_recursive_selector())
	{
		auto s = search();
		s.remove_recursive();
		open(view, s, {});
		return true;
	}

	if (!_filter.is_empty())
	{
		clear_filters();
		return true;
	}

	if (search().has_related())
	{
		open(view, search().related().path);
		return true;
	}

	return false;
}

df::search_parent find_parent_search(const df::search_t& search)
{
	df::search_parent result;

	if (search.has_related())
	{
		result.parent = df::search_t().add_selector(df::item_selector(search.related().path.folder()));
		result.name = search.related().path.name();
		result.selection.files = {search.related().path};
		return result;
	}

	// Broaden the query one named term at a time before leaving the folder scope, so the step size
	// is the same whether or not a folder selector is present. Parent must never jump somewhere
	// derived from the current selection - that is a different scope, not a wider one.
	if (search.has_date())
	{
		result.parent = search;
		result.parent.clear_date_properties();

		const auto parts = search.find_date_parts();

		if (parts.day && parts.month)
		{
			if (parts.year)
			{
				result.parent.day(0, parts.month, parts.year, parts.target);
			}
			else
			{
				result.parent.month(parts.month, parts.target);
			}
		}
		else if (parts.year && parts.month)
		{
			result.parent.year(parts.year, parts.target);
		}

		return result;
	}

	if (search.has_media_type())
	{
		result.parent = search;
		result.parent.clear_media_type();
		return result;
	}

	if (search.has_terms())
	{
		// A place search is written place, state, country, so removing the last term would widen
		// the broadest end and leave the narrow one behind. Parent instead keeps only the broadest
		// level named, stepping a place inside a state inside a country straight out to the country.
		if (const auto level = search.broadest_location_level(); level != df::location_level::any)
		{
			auto parent = search;
			parent.remove_location_terms_below(level);

			if (parent.terms().size() != search.terms().size())
			{
				result.parent = std::move(parent);
				return result;
			}
		}

		result.parent = search;
		result.parent.remove_last_term();
		return result;
	}

	if (search.has_selector())
	{
		const auto selector = search.selectors().front();

		if (selector.has_wildcard() || selector.is_recursive())
		{
			result.parent = df::search_t().add_selector(selector.parent());
		}
		else
		{
			const auto folder = selector.folder();
			const auto parent_folder = folder.parent();

			// A drive or share root is its own parent, so there is nothing wider to show.
			if (parent_folder != folder)
			{
				result.parent = df::search_t().add_selector(df::item_selector(parent_folder));
				result.name = folder.name();
				result.selection.folders = {folder};
			}
		}
	}

	return result;
}


void view_state::open(const view_host_base_ptr& view, const std::string_view text)
{
	// Recorded here so a search typed and entered is remembered the same way as one picked
	// from the list.
	if (!text.empty()) recent_searches.add(text);

	if (_search.has_selector() && text == "**")
	{
		auto search = _search;
		const auto s = search.selectors().front();
		const df::item_selector sel(s.folder(), true, s.wildcard());

		open(view, search.clear_selectors().add_selector(sel), {});
	}
	else if (text == "..")
	{
		const auto p = parent_search();

		// A drive root has no parent, so ".." there would open an empty search and show nothing.
		if (p.parent.is_empty()) return;

		open(view, p.parent, make_unique_paths(p.selection));
	}
	else
	{
		auto search = _search.parse_from_input(text);
		for (const auto& [name, area] : _map_locations) search.resolve_area(area);
		open(view, search, {});
	}
}

df::item_element_ptr view_state::next_unselected_item() const
{
	auto found = false;
	const auto start = _focus;
	df::item_element_ptr first; // first unselected in the listing
	df::item_element_ptr previous; // nearest unselected at or before the focus

	for (const auto& b : _item_groups)
	{
		for (const auto& i : b->items())
		{
			if (found && !i->is_selected())
			{
				return i;
			}

			if (!i->is_selected())
			{
				if (!first) first = i;
				if (!found) previous = i;
			}

			found = found || i == start;
		}
	}

	// Nothing follows the set, which is a selection running to the end of the listing. Settle on what
	// precedes it rather than on the first item: jumping to the top is the cursor reset #250 reported,
	// and it is the same reset whether the set was in the middle or at the end. A focus that is not in
	// the listing has nothing to be before, so the first item is still the only answer there.
	return found ? previous : first;
}

df::item_element_ptr view_state::end_item(const bool forward) const
{
	if (!_item_groups.empty())
	{
		if (forward)
		{
			return _item_groups.back()->items().back();
		}
		return _item_groups.front()->items().front();
	}

	return nullptr;
}

df::item_element_ptr view_state::next_item(const bool forward, const bool extend) const
{
	auto found = false;
	const auto focus = _focus;

	if (forward)
	{
		for (auto b = _item_groups.cbegin(); b != _item_groups.cend(); ++b)
		{
			auto&& items = (*b)->items();

			for (auto i = items.cbegin(); i != items.cend(); ++i)
			{
				const auto& p = *i;

				if (found && (!extend || !p->is_selected()))
				{
					return p;
				}

				found = found || p == focus;
			}
		}
	}
	else
	{
		for (auto b = _item_groups.rbegin(); b != _item_groups.rend(); ++b)
		{
			auto&& items = (*b)->items();

			for (auto i = items.rbegin(); i != items.rend(); ++i)
			{
				const auto& p = *i;

				if (found && (!extend || !p->is_selected()))
				{
					return p;
				}

				found = found || p == focus;
			}
		}
	}

	if (forward)
	{
		for (auto b = _item_groups.cbegin(); b != _item_groups.cend(); ++b)
		{
			auto&& items = (*b)->items();

			for (auto i = items.cbegin(); i != items.cend(); ++i)
			{
				const auto& p = *i;

				if (!extend || !p->is_selected())
				{
					return *i;
				}
			}
		}
	}
	else
	{
		for (auto b = _item_groups.rbegin(); b != _item_groups.rend(); ++b)
		{
			auto&& items = (*b)->items();

			for (auto i = items.rbegin(); i != items.rend(); ++i)
			{
				const auto& p = *i;

				if (!extend || !p->is_selected())
				{
					return *i;
				}
			}
		}
	}

	return nullptr;
}

df::item_element_ptr view_state::next_media_item(const bool forward, const bool wrap) const
{
	std::vector<df::item_element_ptr> ordered;

	for (const auto& g : _item_groups)
	{
		const auto& items = g->items();
		ordered.insert(ordered.end(), items.cbegin(), items.cend());
	}

	const auto n = static_cast<int>(ordered.size());

	if (n == 0)
		return nullptr;

	const auto focus = _focus;
	auto pos = forward ? -1 : n;

	for (auto i = 0; i < n; ++i)
	{
		if (ordered[i] == focus)
		{
			pos = i;
			break;
		}
	}

	// Only photos, videos and audio can play or hold for the slideshow delay. Stepping over
	// everything else stops a sequence stalling on a folder, document or archive.
	for (auto step = 0; step < n; ++step)
	{
		pos += forward ? 1 : -1;

		if (pos < 0 || pos >= n)
		{
			if (!wrap) return nullptr;
			pos = forward ? 0 : n - 1;
		}

		const auto& i = ordered[pos];

		if (i && i->is_media())
		{
			return i;
		}
	}

	return nullptr;
}

bool view_state::can_slideshow() const
{
	const auto displayed = _display && _display->is_one() ? _display->_item1 : nullptr;
	if (displayed && displayed->is_media()) return true;

	// Command state is refreshed constantly, so this must not materialise the ordered list the way
	// next_media_item does. Any playable item at all means a slideshow can run.
	for (const auto& g : _item_groups)
	{
		for (const auto& i : g->items())
		{
			if (i && i->is_media()) return true;
		}
	}

	return false;
}

df::item_element_ptr view_state::next_group_item(const bool forward) const
{
	const auto focus = selected_item_group();
	const auto start = focus.item ? focus.group : nullptr;

	auto found = start == nullptr;
	auto groups = _item_groups;

	if (groups.size() > 1)
	{
		if (!forward)
		{
			std::ranges::reverse(groups);
		}

		for (const auto& b : groups)
		{
			if (found)
			{
				for (const auto& i : b->items())
				{
					return i;
				}
			}

			found = found || b == start;
		}

		for (const auto& b : groups)
		{
			for (const auto& i : b->items())
			{
				return i;
			}
		}
	}
	else if (!groups.empty() && !groups[0]->items().empty())
	{
		return forward ? groups[0]->items().back() : groups[0]->items().front();
	}

	return nullptr;
}

df::string_counts view_state::selected_tags() const
{
	df::string_counts result;

	for (const auto& i : _selected.items())
	{
		auto md = i->metadata();

		if (md)
		{
			const auto tag_parts = split(md->tags, true);

			for (const auto& t : tag_parts)
			{
				++result[t];
			}
		}
	}

	return result;
}

void view_state::open(const view_host_base_ptr& view, const df::file_path path)
{
	df::unique_paths sel;
	sel.emplace(path);
	open(view, df::search_t().add_selector(path.folder()), sel);
}


bool view_state::resolve_area_search(df::search_t& search) const
{
	const auto original = search;
	for (const auto& [name, area] : _map_locations) search.resolve_area(area);
	if (!item_index.locations().is_index_loaded() || item_index.thumbnailing_items.load() != 0)
	{
		return search != original;
	}

	index_histograms_const_ptr histograms;
	for (const auto& term : search.terms())
	{
		if (term.type == df::search_term_type::area && term.location_cell_span == 0)
		{
			if (!histograms) histograms = item_index.histograms();
			const auto area = histograms->find_map_location(
				term.text, item_index.locations(), setting.default_location);
			if (area) search.resolve_area(*area);
		}
	}
	return search != original;
}

bool view_state::open(const view_host_base_ptr& view, const df::search_t& new_search, const df::unique_paths& selection)
{
	auto resolved_search = new_search;
	resolve_area_search(resolved_search);
	if (resolved_search != new_search)
	{
		return open(view, resolved_search, selection);
	}

	for (const auto& s : new_search.selectors())
	{
		if (!s.can_iterate())
		{
			// The location is disconnected, renamed or removed. Report it instead of doing nothing.
			_events.report_scope_unavailable(new_search);
			return false;
		}
	}

	if (!_events.can_open_search(new_search))
	{
		return false;
	}

	if (_pending_history_move && _pending_history_move->search != new_search)
	{
		_pending_history_move.reset();
	}

	return open_validated(view, new_search, selection);
}

bool view_state::open_validated(const view_host_base_ptr& view, const df::search_t& new_search,
                                const df::unique_paths& selection)
{
	static std::atomic_int version;
	auto token = df::cancel_token(version);
	const auto path_changed = new_search != _search;

	if (path_changed)
	{
		stop_slideshow();
		_search = new_search;
		_parent_search = find_parent_search(_search);
		refresh_sibling_folders();
		++_group_title_generation;
		update_search_is_favorite_or_collection_root();

		if (_pending_history_move && _pending_history_move->search == new_search)
		{
			history.commit_history_pos(_pending_history_move->direction, std::move(_pending_history_move->selected));
			_pending_history_move.reset();
		}
		else
		{
			history.history_add(new_search, _selected.ids());
		}
	}
	else if (_pending_history_move && _pending_history_move->search == new_search)
	{
		_pending_history_move.reset();
	}

	// A pass superseded before it publishes is dropped, but what it was opened for is still owed. A
	// refresh of the same search is the usual superseder - Delete re-opens the listing naming the
	// item to land on, and validating the deleted files refreshes it straight away with nothing to
	// select - so the refresh inherits the selection and the path change it would otherwise lose.
	auto publish_selection = selection;
	auto publish_path_changed = path_changed;

	if (_unpublished_open && _unpublished_open->search == new_search)
	{
		if (publish_selection.empty()) publish_selection = _unpublished_open->selection;
		publish_path_changed = publish_path_changed || _unpublished_open->path_changed;
	}

	_unpublished_open = unpublished_open_t{new_search, publish_selection, publish_path_changed};

	_events.invalidate_view(view_invalid::address);

	// The host is UI-owned and is only ever used on the UI hop below, so it crosses the worker as a
	// handle whose release is posted back rather than as a plain owning reference - a truncated
	// query queue would otherwise run a view host destructor on the query thread.
	_async.queue_async(async_queue::query,
	                   [this, view = ui_owned(_async, view), new_search, selection = std::move(publish_selection),
		                   path_changed = publish_path_changed, token]
	                   {
		                   // A newer open supersedes this one before the worker reaches it, so retire
		                   // here rather than walking the index for a search nobody is waiting on.
		                   if (token.is_cancelled()) return;

		                   bool is_first = true;

		                   auto cb = [this, view, &is_first, new_search, selection, path_changed, token](
			                   index_state::query_item_results query_items, const bool is_complete)
		                   {
			                   _async.queue_ui(
				                   [this, view, new_search, query_items = std::move(query_items), selection, is_first,
					                   is_complete, path_changed, token]
				                   {
					                   // The identity test alone cannot retire a superseded pass:
					                   // re-opening the same search leaves _search equal to it, so a
					                   // cancelled walk's partial results were appended over the
					                   // pass that replaced it.
					                   if (token.is_cancelled()) return;

					                   if (new_search == _search)
					                   {
						                   auto append_items = item_index.
							                   materialize_query_items(std::move(query_items), existing_items());
						                   this->append_items(view.shared(), std::move(append_items), selection,
						                                      is_first, is_complete);

						                   if (is_complete)
						                   {
							                   // Settled: nothing this open asked for is owed any more.
							                   _unpublished_open.reset();
							                   _events.search_complete(new_search, path_changed);
						                   }
					                   }
				                   });

			                   is_first = false;
		                   };

		                   item_index.query_items(new_search, cb, token);
	                   });


	return true;
}

void view_state::open(const view_host_base_ptr& view, const df::item_element_ptr& i)
{
	if (!i->is_selected())
	{
		select(view, i, false, false, false);
	}

	if (i)
	{
		i->open(*this, view);
	}
}

using existing_textures_t = df::hash_map<df::file_path, std::shared_ptr<texture_state>, df::ihash, df::ieq>;

static texture_state_ptr get_tex(const existing_textures_t& existing_textures, const df::item_element_ptr& item,
                                 async_strategy& as)
{
	const auto found = existing_textures.find(item->path());

	const auto result = found != existing_textures.end()
		                    ? found->second
		                    : std::make_shared<texture_state>(as, item);

	// Both arrivals need it: a new item has nothing decoded yet, and a cached one gave up everything it
	// had decoded when it left the display.
	result->seed_placeholder(item);
	return result;
}

void view_state::load_display_state()
{
	const auto new_display = std::make_shared<display_state_t>(_async, _common_display_state);
	new_display->populate(*this);

	const auto d = _display;

	if (d)
	{
		const auto display_item = new_display->_item1;

		// Supersede whether or not a session exists yet: an open in flight for the outgoing display
		// would otherwise land on it after it has been replaced, holding the file open unseen.
		++d->_av_generation;

		if (d->_session)
		{
			_player->close(d->_session, {});
			d->_session.reset();
		}

		if (d->_selected_texture1 && d->_selected_texture1 != new_display->_selected_texture1)
			d->_selected_texture1->cancel_pending_decode();
		if (d->_selected_texture2 && d->_selected_texture2 != new_display->_selected_texture2)
			d->_selected_texture2->cancel_pending_decode();

		if (new_display->is_one())
		{
			const auto display_file_type = display_item->file_type();
			const auto display_path = display_item->path();
			const auto display_xmp = display_item->xmp();
			const auto is_bitmap = display_file_type->has_trait(file_traits::bitmap);
			const auto is_av = display_file_type->has_trait(file_traits::av);
			const auto is_playable = display_item && display_file_type->is_playable();

			if (is_playable)
			{
				const auto play_view = is_items_or_media_view();
				const auto continue_playing = _common_display_state._is_slideshow || _play_next_on_open;
				const auto auto_play = (setting.auto_play || continue_playing) && play_view &&
					df::file_handles_detached == 0;

				open_av_session(new_display, display_item, auto_play, -1, -1, setting.last_played_pos);
			}
			else
			{
				_async.queue_async(async_queue::load,
				                   [new_display = ui_owned(_async, new_display), display_path, display_xmp,
					                   display_file_type]
				                   {
					                   df::scope_locked_inc l(df::loading_media);
					                   files ff;
					                   const auto scan_result = ff.scan_file(
						                   display_path, false, display_file_type, display_xmp, {},
						                   scan_intent::inspect);
					                   auto mi = scan_result.to_info();

					                   new_display->_async.queue_ui([new_display, mi]
					                   {
						                   new_display->_player_media_info = mi;
						                   new_display->_full_metadata_loaded = true;
						                   new_display->_async.invalidate_view(
							                   view_invalid::view_layout |
							                   view_invalid::app_layout |
							                   view_invalid::media_elements |
							                   view_invalid::command_state);
					                   });
				                   });
			}

			if (!is_bitmap && !is_av)
			{
				if (display_file_type->group == file_group::archive)
				{
					_async.queue_async(async_queue::load,
					                   [new_display = ui_owned(_async, new_display), display_path]
					                   {
						                   df::scope_locked_inc l(df::loading_media);
						                   auto archive_items = files::list_archive(display_path);

						                   new_display->_async.queue_ui(
							                   [new_display, archive_items = std::move(archive_items)]() mutable
							                   {
								                   new_display->_archive_items = std::move(archive_items);
								                   new_display->_async.invalidate_view(
									                   view_invalid::media_elements | view_invalid::view_layout);
							                   });
					                   });
				}
				else
				{
					new_display->load_selected_item_data();
				}
			}

			const auto mt = display_file_type;

			if (mt->group == file_group::photo)
			{
				if (files::is_raw(display_item->name()))
				{
					record_feature_use(features::show_raw);
				}
				else
				{
					record_feature_use(features::show_photo);
				}
			}
			else if (mt->group == file_group::video)
			{
				record_feature_use(features::show_video);
			}
			else if (mt->group == file_group::audio)
			{
				record_feature_use(features::show_audio);
			}
		}
		else if (new_display->is_two())
		{
		}

		if (_selected.items().size() <= 2)
		{
			struct crc_request
			{
				std::weak_ptr<df::item_element> item;
				df::file_path path;
				df::file_size size;
				df::date_t modified;
				df::item_online_status online_status = df::item_online_status::offline;
				uint32_t existing_crc = 0;
			};

			std::vector<crc_request> requests;
			const auto two_files_size_same = new_display->_item1 && new_display->_item2 &&
				new_display->_item1->file_size() == new_display->_item2->file_size();
			constexpr auto max_load_size = 16u * static_cast<uint64_t>(df::one_meg);

			for (const auto& item : {new_display->_item1, new_display->_item2})
			{
				if (item && item->crc32c() == 0 && item->online_status() == df::item_online_status::disk &&
					(two_files_size_same || item->file_size().to_int64() < max_load_size))
				{
					requests.emplace_back(item, item->path(), item->file_size(), item->file_modified(),
					                      item->online_status(), item->crc32c());
				}
			}

			queue_async(async_queue::crc, [index = &item_index, requests = std::move(requests)]
			{
				for (const auto& request : requests)
				{
					df::scope_locked_inc loading(df::loading_media);
					const auto crc = platform::file_crc32(request.path);

					if (crc)
					{
						index->publish_crc(request.item, request.path, request.size, request.modified,
						                   request.online_status, request.existing_crc, crc);
					}
				}
			});
		}
	}

	_display = new_display;
	_play_next_on_open = false;

	// Once the new display is live, whatever it did not take keeps only its loaded representation.
	_common_display_state.release_undisplayed(new_display->_selected_texture1, new_display->_selected_texture2);

	_events.display_changed();
}


media_preview_state::media_preview_state() = default;
media_preview_state::~media_preview_state() = default;

void media_preview_state::close()
{
	decoder1.reset();
	decoder2.reset();
	encoder.reset();
}

void view_state::close() const
{
	const auto d = _display;

	if (d && d->_session)
	{
		_player->close(d->_session, {});
	}

	_async.queue_media_preview([](media_preview_state& decoder) { decoder.close(); }, true);
}

void view_state::tick(const view_host_base_ptr& view, const double time_now)
{
	const auto d = _display;

	if (d)
	{
		const auto is_media_playing = d->_session && d->_session->is_playing();
		const auto is_slideshow = _common_display_state._is_slideshow;

		if (is_slideshow || is_media_playing)
		{
			const auto slide_show_ticks = std::max(1, setting.slideshow_delay) * ui::default_ticks_per_second;
			const auto display_item = d->_item1;
			const auto display_type = display_item ? display_item->file_type() : nullptr;
			const auto is_photo = display_type && display_type->has_trait(file_traits::bitmap);
			const auto is_av = display_type && display_type->has_trait(file_traits::av);

			if (is_slideshow && !is_photo && !is_av)
			{
				stop();
			}
			else
			{
				if (is_slideshow && is_av && d->can_play_media() && !is_media_playing && !d->_session->
					has_ended(time_now))
				{
					_player->play(d->_session);
				}

				if (is_photo)
				{
					d->_next_photo_tick += 1;
					invalidate_view(view_invalid::view_redraw);
				}

				const auto media_ended = !is_photo && d->_session && d->_session->has_ended(time_now);
				// pause and seek are applied by the player thread, so the session can still report
				// the same end on the next tick. Acting on it twice advanced two items at once.
				const auto is_media_end = media_ended && !d->_media_end_handled;
				d->_media_end_handled = media_ended;
				const auto is_photo_end = is_photo && d->_next_photo_tick > slide_show_ticks;

				if (is_media_end)
				{
					df::trace("view_state::tick detected media played to end");

					if (d->_session)
					{
						const auto start = d->_player_media_info.start;

						_player->pause(d->_session);
						_player->seek(d->_session, start, false);
					}
				}

				if (is_media_end || is_photo_end)
				{
					const auto mode_can_play = view_mode() == view_type::items || view_mode() == view_type::media;

					playback_tick t;
					t.is_slideshow = is_slideshow;
					t.is_photo = is_photo;
					t.is_av = is_av;
					t.media_ended = is_media_end;
					t.photo_delay_elapsed = is_photo_end;
					t.can_next = df::command_active == 0 && display_item && mode_can_play && has_display_items();
					t.repeat = setting.repeat;
					t.auto_advance = setting.auto_advance;

					df::item_element_ptr next;

					if (t.can_next && t.repeat != repeat_mode::repeat_one && (is_slideshow || setting.auto_advance))
					{
						next = next_media_item(true, false);
						t.has_next = next != nullptr;
						t.next_is_current = next == display_item;

						if (!next && t.repeat == repeat_mode::repeat_all)
						{
							next = next_media_item(true, true);
							t.has_wrapped_next = next != nullptr;
							t.wrapped_is_current = next == display_item;
						}
					}

					switch (calc_playback_advance(t))
					{
					case playback_advance::hold:
						// Wrapping onto the only displayed item. select() is a no-op for the item
						// already displayed, which would leave the player paused at the first frame
						// and _play_next_on_open armed, so restart in place instead.
						if (is_media_end && d->_session)
						{
							_player->play(d->_session);
						}
						break;

					case playback_advance::advance:
						_play_next_on_open = true;
						select(view, next, false, false, true);
						break;

					case playback_advance::stop:
					case playback_advance::none:
						stop();
						break;
					}

					d->_next_photo_tick = 0;
				}

				d->update_scrubber();
			}
		}

		if (d->_selected_texture1)
		{
			d->_selected_texture1->refresh(d->_item1);
		}

		if (d->_selected_texture2)
		{
			d->_selected_texture2->refresh(d->_item2);
		}
	}
}


// The file version a texture's pixels came from. The source file is authoritative: thumbnails may
// arrive later with a null-or-file stamp and must not force a second full decode of unchanged pixels.
static texture_source_version item_source_version(const df::item_element_ptr& i)
{
	return i ? texture_source_version{i->file_modified(), {}} : texture_source_version{};
}

void texture_state::load_image(const df::item_element_ptr& i)
{
	if (i)
	{
		if (_path != i->path()) _load_retry_count = 0;
		_photo_loaded = true;
		_load_retry_pending = false;
		_photo_version = item_source_version(i);
		_path = i->path();
		const auto generation = ++_load_generation;

		_async.queue_async(async_queue::load,
		                   [&as = _async, t = ui_owned(_async, shared_from_this()), path = _path, generation]
		                   {
			                   files loader;
			                   auto loaded = loader.load(path, true);

			                   as.queue_ui([t, loaded = std::move(loaded), generation, &as]() mutable
			                   {
				                   t->complete_load(std::move(loaded), generation, false);
				                   as.invalidate_view(view_invalid::view_layout | view_invalid::image_compare);
			                   });
		                   });
	}
}

void texture_state::seed_placeholder(const df::item_element_ptr& i)
{
	df::assert_true(ui::is_ui_thread());

	// Anything already drawn or waiting to be drawn is at least as good as the thumbnail, and
	// replacing it would step backwards.
	if (!i || _tex || _staged_surface) return;

	// Staged without touching _display_dimensions: the pane is laid out for what the item is, and a
	// thumbnail is not always that shape. draw fits it rather than stretching it - see
	// df::fit_preserving_aspect. Publishing the thumbnail's shape here instead would jump the layout
	// when the decode arrived.
	const auto& s = i->thumbnail_surface();
	if (ui::is_valid(s)) _staged_surface = s;
}

void texture_state::load_raw()
{
	const auto generation = ++_load_generation;
	_async.queue_async(async_queue::load_raw,
	                   [&as = _async, t = ui_owned(_async, shared_from_this()), path = _path, generation]
	                   {
		                   df::scope_locked_inc l(t->_preview_rendering);
		                   files loader;
		                   auto loaded = loader.load(path, false);

		                   as.queue_ui([t, loaded = std::move(loaded), generation, &as]() mutable
		                   {
			                   t->complete_load(std::move(loaded), generation, true);
			                   as.invalidate_view(view_invalid::view_layout | view_invalid::image_compare);
		                   });
	                   });
}

void texture_state::complete_load(file_load_result loaded, const uint64_t generation, const bool raw)
{
	if (generation != _load_generation) return;
	if (loaded.success)
	{
		update(std::move(loaded));
	}
	else if (loaded.reason == file_load_result::failure::too_large)
	{
		// No retry changes the size of the file. The item thumbnail is dropped along with everything
		// else, because leaving it on screen would imply the image itself is what is displayed.
		cancel_pending_decode();
		++_decode_generation;
		_display_problem = display_problem::too_large;
		_load_retry_pending = false;
		_loaded.clear();
		_staged_surface.reset();
		_retained_surface.reset();
		_zoom_staged_surface.reset();
		_tex.reset();
		_zoom_texture.reset();

		if (!loaded.source_dimensions.is_empty())
		{
			_display_dimensions = loaded.source_dimensions;
			_display_geometry_known = true;
		}

		_async.invalidate_view(view_invalid::view_redraw);
	}
	else if (!raw)
	{
		_photo_loaded = false;
		_load_retry_count += 1;
		_load_retry_pending = _load_retry_count < 3;
	}
}

void texture_state::free_graphics_resources()
{
	_tex.reset();
	_vid_tex.reset();
	_zoom_texture.reset();
	_last_draw_tex.reset();
	_fade_out_tex.reset();
	_panorama_tex.reset();
	_panorama_gpu_tex.reset();
	_panorama_gpu_source.reset();
	_panorama_rendered = false;
	_tex_invalid = true;

	// The texture this was dissolving from has just gone; leaving the animation part-way would fade the
	// rebuilt one up out of the background instead.
	_display_alpha_animation.reset(1.0f);
}

void texture_state::release_decoded_surfaces()
{
	// An in-flight decode would republish straight back into what is being dropped, so it is cancelled
	// and its generation retired rather than left to land.
	cancel_pending_decode();
	++_decode_generation;

	_staged_surface.reset();
	_retained_surface.reset();
	_zoom_staged_surface.reset();
	_zoom_version = {};

	// The mip pyramid is the biggest thing here: it holds a third again as much as the source it was
	// built from, and the source is already going.
	_panorama_renderer.set_source(nullptr);
	_panorama_source.reset();
	_panorama_surface.reset();

	free_graphics_resources();
}

void texture_state::cancel_pending_decode()
{
	if (_decode_cancel) _decode_cancel->store(true, std::memory_order_relaxed);
}

void texture_state::refresh(const df::item_element_ptr& i)
{
	// A thumbnail the display had to ask for arrives long after the texture state was built, so the
	// stand-in is adopted here rather than only when the display was assembled.
	seed_placeholder(i);

	if (_is_photo)
	{
		const auto item_version = item_source_version(i);
		auto out_of_date = i && _photo_version != item_version;

		if (out_of_date && _retain_visuals_on_modify)
		{
			// The write that armed this changed no pixels, so adopt its stamp rather than re-read the
			// file.
			_retain_visuals_on_modify = false;
			_photo_version = item_version;
			out_of_date = false;
		}

		const auto reload = _photo_loaded && out_of_date;
		const auto first_load_needed = _load_retry_count == 0 &&
			(_display_bounds.width() > _loaded.dimensions().cx || out_of_date);
		const auto load_needed = !_photo_loaded && (_load_retry_pending || first_load_needed);

		if (load_needed || reload)
		{
			if (out_of_date)
			{
				_load_retry_count = 0;
				_display_geometry_known = false;
			}
			load_image(i);
		}
	}
}

void texture_state::mark_visuals_current()
{
	df::assert_true(ui::is_ui_thread());
	_retain_visuals_on_modify = true;
}

void texture_state::publish_written_image(const df::file_path path, file_load_result loaded,
                                          const df::date_t modified)
{
	df::assert_true(ui::is_ui_thread());

	if (!loaded.success || _path != path)
	{
		return;
	}

	// The writer produced these bytes, so they supersede anything a load already in flight can
	// return; bumping the generation makes that load a no-op when it completes.
	++_load_generation;
	_photo_loaded = true;
	_retain_visuals_on_modify = false;
	_photo_version = {prop::is_null(modified) ? platform::now() : modified, {}};
	_display_geometry_known = false;
	update(std::move(loaded));
}

void texture_state::update(file_load_result loaded)
{
	++_decode_generation;
	_loaded = std::move(loaded);
	_retained_surface.reset();
	_load_retry_count = 0;
	_load_retry_pending = false;
	_is_placeholder = false;
	_zoom_texture.reset();
	_zoom_staged_surface.reset();
	_display_problem = display_problem::none;
	if (!_display_geometry_known)
	{
		_display_dimensions = _loaded.dimensions();
		_display_orientation = _loaded.orientation();
		_display_geometry_known = !_display_dimensions.is_empty();
	}
	_tex_invalid = true;

	if (_loaded.is_preview && !setting.raw_preview)
	{
		load_raw();
	}
}

void texture_state::update(const ui::const_surface_ptr& staged_surface)
{
	if (_display_dimensions != staged_surface->dimensions() ||
		_display_orientation != staged_surface->orientation())
	{
		_async.invalidate_view(view_invalid::view_layout);
	}

	_vid_tex.reset();
	_staged_surface = staged_surface;
	_display_dimensions = staged_surface->dimensions();
	_display_orientation = staged_surface->orientation();
	_display_problem = display_problem::none;

	_async.invalidate_view(view_invalid::view_redraw);
}

// Says why the media area is empty. Only reached when there is no texture at all, so it can never
// cover an image.
static void draw_display_problem(ui::draw_context& rc, const recti media_bounds,
                                 const texture_state::display_problem problem, const sizei dims, const float alpha)
{
	if (problem == texture_state::display_problem::none || media_bounds.width() < 96) return;

	auto text = std::string(problem == texture_state::display_problem::too_large
		                        ? tt.image_too_large.sv()
		                        : tt.image_display_failed.sv());

	if (!dims.is_empty())
	{
		text += std::format("\n{} x {}", dims.cx, dims.cy);
	}

	rc.draw_text(text, media_bounds, ui::style::font_face::dialog, ui::style::text_style::multiline_center,
	             ui::color(ui::style::color::view_text, alpha), {});
}

static sizei oriented_dimensions(const file_load_result& loaded)
{
	auto dims = loaded.dimensions();

	if (setting.show_rotated && flips_xy(loaded.orientation()))
	{
		std::swap(dims.cx, dims.cy);
	}

	return dims;
}

// A texture's extent as it lands on screen. The destination is in display space, so an extent
// compared against it has to be in the same one.
static sizei oriented_extent(sizei dims, const ui::orientation orientation)
{
	if (setting.show_rotated && flips_xy(orientation))
	{
		std::swap(dims.cx, dims.cy);
	}

	return dims;
}

// Fits an extent inside both budgets the draw backend published: the pixels a texture may cost, and
// the largest edge the device accepts. Aspect ratio is kept, so an over-wide panorama is shown
// scaled down rather than failing its upload and showing nothing.
static sizei clamp_to_texture_budget(sizei target)
{
	const auto max_pixels = df::max_texture_bytes / 4;
	const auto pixel_count = static_cast<int64_t>(target.cx) * target.cy;

	if (pixel_count > max_pixels)
	{
		const auto memory_scale = std::sqrt(max_pixels / static_cast<double>(pixel_count));
		target = {
			std::max(1, df::round(target.cx * memory_scale)),
			std::max(1, df::round(target.cy * memory_scale))
		};
	}

	const auto max_dimension = df::max_texture_dimension;
	const auto longest = std::max(target.cx, target.cy);

	if (longest > max_dimension)
	{
		const auto edge_scale = max_dimension / static_cast<double>(longest);
		target = {
			std::clamp(df::round(target.cx * edge_scale), 1, max_dimension),
			std::clamp(df::round(target.cy * edge_scale), 1, max_dimension)
		};
	}

	return target;
}

sizei texture_state::calc_scale_hint() const
{
	const auto dims = oriented_dimensions(_loaded);

	// A projection samples the sphere, not the rectangle, so the viewport says nothing about how
	// much source it needs: at a narrow field of view a small window reads a large arc at close to
	// one texel per pixel. The arc the viewport covers is what sets the resolution, capped at the
	// file's own, and the decode budget above still refuses what will not fit.
	if (_panorama.active && _panorama.geometry.is_valid())
	{
		const auto full_width_needed = _display_bounds.width() * 2.0 * M_PI / std::max(0.01, _panorama.view.fov());
		const auto wanted = std::min(1.0, full_width_needed / std::max(1, _panorama.geometry.full_width));
		const auto target = sizei{
			std::max(1, df::round(dims.cx * wanted)), std::max(1, df::round(dims.cy * wanted))
		};

		return clamp_to_texture_budget(target);
	}

	const auto scale = ui::calc_scale_down_factor(dims, _display_bounds.extent());
	const auto target = scale < 2 ? dims : sizei{dims.cx / scale, dims.cy / scale};

	return clamp_to_texture_budget(target);
}


// The decode ladder, shared by the flat and projected presentations. What differs between them is
// the resolution asked for, and calc_scale_hint owns that.
void texture_state::update_decode(ui::draw_context& rc)
{
	const auto media_bounds = _display_bounds;

	if (media_bounds.is_empty()) return;

	{
		const auto scale_hint = calc_scale_hint();

		// A projection reads the decoded pixels itself, so it needs them packed. Everything else hands
		// the surface straight to a texture, where planar YUV is cheaper and the backend does the
		// conversion. The requirement is part of what a decode was asked for, so changing it has to
		// re-decode even when the size did not change.
		const auto wants_packed = _panorama.active;

		if ((_tex_invalid || _loading_scale_hint != scale_hint || _loading_packed != wants_packed) &&
			!_loaded.is_empty())
		{
			cancel_pending_decode();
			_loading_scale_hint = scale_hint;
			_loading_packed = wants_packed;
			_tex_invalid = false;

			const auto decode_bytes = files::estimate_decode_bytes(_loaded.i, scale_hint);

			if (decode_bytes > df::max_decode_bytes)
			{
				// The codec has to build the whole frame before anything can be scaled down, and that
				// frame will not fit. Nothing is queued; the media area says why instead.
				_display_problem = display_problem::too_large;
				df::log(__FUNCTION__, std::format("{} needs {} to decode, over the {} budget", _path.name(),
				                                  df::file_size(decode_bytes).str(),
				                                  df::file_size(df::max_decode_bytes).str()));
			}
			else
			{
				_display_problem = display_problem::none;
				const auto generation = ++_decode_generation;
				const auto placeholder = _is_placeholder;
				const auto retained = _retained_surface;
				const auto cancel = std::make_shared<std::atomic_bool>(false);
				_decode_cancel = cancel;
				const df::cancel_token token(*cancel);

				// A projection reads the pixels itself, and the CPU backend presents BGRA, so planar
				// is asked for only when it can reach the sampler as planes.
				const auto can_use_yuv = !wants_packed && ui::yuv_textures_enabled;

				// A placeholder decodes in a fraction of the time a full-size image does, so it goes to
				// the thumbnail queue rather than waiting behind the full-size decodes on the display
				// queue.
				_async.queue_async(placeholder ? async_queue::render : async_queue::render_display,
				                   [&as = _async, ld = _loaded, retained, scale_hint, placeholder, generation, cancel,
					                   token, wants_packed, can_use_yuv, t = ui_owned(_async, shared_from_this())]
				                   {
					                   if (cancel->load(std::memory_order_relaxed)) return;
					                   files loader;
					                   const auto can_reuse = ui::is_valid(retained) && retained->dimensions().cx >=
						                   scale_hint.cx &&
						                   retained->dimensions().cy >= scale_hint.cy &&
						                   (!wants_packed || ui::is_packed(retained->format()));
					                   auto s = can_reuse
						                            ? loader.fit_within(retained, scale_hint)
						                            : ld.to_surface(scale_hint, can_use_yuv, token);
					                   auto zoom = ui::is_valid(s)
						                               ? loader.fit_within(
							                               s, df::zoom_view_state::navigator_surface_extent)
						                               : nullptr;

					                   as.queue_ui([t, s, zoom, placeholder, generation, can_reuse, wants_packed]
					                   {
						                   // Scale changes and phase upgrades can complete out of order. Only the current
						                   // request may publish, and a placeholder may never replace a later phase.
						                   if (generation != t->_decode_generation || placeholder != t->_is_placeholder)
						                   {
							                   // Except over nothing. A placeholder that lost the race to the file load
							                   // still beats the empty frame that would otherwise stand in for the whole
							                   // full-size decode. It is staged alone: the retained surface and the
							                   // navigator downsample belong to the phase that superseded it.
							                   if (placeholder && ui::is_valid(s) && !t->_tex && !t->_staged_surface)
							                   {
								                   t->_staged_surface = s;
								                   t->_async.invalidate_view(view_invalid::view_redraw);
							                   }

							                   return;
						                   }

						                   t->_staged_surface = s;
						                   t->_zoom_staged_surface = zoom;

						                   // The request was not superseded, so a missing surface is a real decode failure
						                   // rather than a cancellation.
						                   if (!ui::is_valid(s)) t->_display_problem = display_problem::failed;

						                   // What is worth holding is decided by what will read it. Comparing
						                   // allocated bytes compares two formats against each other - planar YUV
						                   // costs 1.5 bytes a pixel and packed costs 4 - so a large planar
						                   // surface from a flat view outweighed the packed decode a projection
						                   // had just asked for, and the projection then had nothing it could
						                   // read and drew an empty frame for as long as the item was open.
						                   if (!can_reuse && ui::is_valid(s))
						                   {
							                   const auto& held = t->_retained_surface;
							                   const auto held_valid = ui::is_valid(held);
							                   const auto keeps_format = !wants_packed || !held_valid ||
								                   ui::is_packed(s->format()) == ui::is_packed(held->format());
							                   const auto pixels = [](const ui::const_surface_ptr& surface)
							                   {
								                   const auto d = surface->dimensions();
								                   return static_cast<int64_t>(d.cx) * d.cy;
							                   };

							                   if (!held_valid ||
								                   (!keeps_format && ui::is_packed(s->format())) ||
								                   (keeps_format && pixels(s) > pixels(held)))
							                   {
								                   t->_retained_surface = s;
							                   }
						                   }
						                   t->_async.invalidate_view(view_invalid::view_redraw);
					                   });
				                   });
			}
		}

		const auto s = std::move(_staged_surface);

		if (s)
		{
			const auto tex = rc.create_texture();
			if (tex && tex->update(s) != ui::texture_update_result::failed)
			{
				_tex = tex;
				fade_out();
			}
			else
			{
				_display_problem = display_problem::failed;
			}
		}
	}
}

void texture_state::draw(ui::draw_context& rc, const pointi offset, const int compare_pos, const bool first_texture,
                         const bool interactive)
{
	const auto media_bounds = _display_bounds;

	// Leaving the projection is the only moment its resources stop being needed while the item stays
	// on screen, and nothing else releases them until it is navigated away from.
	if (_panorama.active) release_panorama_resources();

	_panorama = {};
	update_decode(rc);

	// After the upload above, because that is what starts the dissolve. Read before it, this is the
	// previous fade's finished value: the incoming texture would draw opaque for one frame, the
	// outgoing one would be released as complete, and the fade would then run against the background.
	auto alpha = _display_alpha_animation.val();

	const auto tex = _vid_tex && _vid_tex->is_valid() ? _vid_tex : _tex;
	if ((!tex || !tex->is_valid()) && !media_bounds.is_empty())
	{
		rc.draw_rect(media_bounds.offset(offset), ui::color(ui::style::color::group_background, alpha));
		draw_display_problem(rc, media_bounds.offset(offset), _display_problem, calc_display_dimensions(), alpha);
	}

	// The outgoing resolution sits underneath at full opacity while the new one dissolves in over it,
	// so the composite never dips toward the background mid-fade. It is drawn at the incoming image's
	// current bounds rather than the ones it was last drawn at: a zoom moves those every frame, and a
	// frozen copy would drift out of register for the length of the fade.
	if (_fade_out_tex && _fade_out_tex->is_valid())
	{
		const auto fade_dims = _fade_out_tex->source_extent();
		const auto fades_whole_texture = _fade_out_source_rect.width() >= fade_dims.cx &&
			_fade_out_source_rect.height() >= fade_dims.cy;

		if (!fades_whole_texture)
		{
			// In compare the last source rect is the split portion, and fitting the destination to
			// that shape would shrink the outgoing picture for the length of the dissolve. Without an
			// underlay there is nothing to dissolve from, so the fade is abandoned rather than run
			// against the background - a flash is not an improvement on a resize.
			_fade_out_tex.reset();
			_display_alpha_animation.reset(1.0f);
			alpha = 1.0f;
		}
		else if (alpha < 1.0f)
		{
			const auto fade_orientation = _fade_out_tex->_orientation;
			const auto fade_bounds = df::fit_preserving_aspect(
				rectd(media_bounds), sized(oriented_extent(fade_dims, fade_orientation)));
			const auto fade_dst_quad = setting.show_rotated
				                           ? quadd(fade_bounds.offset(pointd(offset))).transform(
					                           to_simple_transform(fade_orientation))
				                           : fade_bounds.offset(pointd(offset));

			rc.draw_texture(_fade_out_tex, fade_dst_quad, _fade_out_source_rect, 1.0f, _fade_out_sampler);
		}
		else
		{
			_fade_out_tex.reset();
		}
	}

	if (tex && tex->is_valid())
	{
		const auto tex_dims = tex->source_extent();
		const auto orientation = tex->_orientation;
		const auto sampler = calc_sampler(media_bounds.extent(), tex_dims, orientation, interactive,
		                                  tex == _tex && is_provisional());

		// rendering.md: the destination is shaped by what the item is, not by what has arrived to
		// draw into it, and a stand-in staged before the decode need not be that shape. Fitting it
		// keeps the subject's shape; stretching it distorted the picture until the decode landed,
		// which is what read as a compare-mode defect. Video is the exception: its frame can carry
		// non-square pixels, so the stored frame is not the shape to fill and the container's
		// display dimensions are what the item actually is (#78).
		const auto image_bounds = df::fit_preserving_aspect(
			rectd(media_bounds), sized(calc_draw_shape(oriented_extent(tex_dims, orientation),
			                                           calc_display_dimensions(), tex == _vid_tex)));
		auto draw_bounds = image_bounds;
		auto tex_bounds = rectd(tex_dims);

		if (compare_pos)
		{
			if (first_texture)
			{
				const auto split_x = std::clamp(compare_pos - 1.0, draw_bounds.X + 1.0, draw_bounds.right() - 1.0) -
					draw_bounds.X;
				draw_bounds.Width = split_x;

				const auto cx = tex_dims.cx * draw_bounds.Width / image_bounds.Width;
				const auto cy = tex_dims.cy * draw_bounds.Width / image_bounds.Width;

				if (flips_xy(orientation))
				{
					if (is_inverted(orientation))
					{
						tex_bounds.Y = tex_bounds.bottom() - cy;
					}

					tex_bounds.Height = cy;
				}
				else
				{
					if (is_inverted(orientation))
					{
						tex_bounds.X = tex_bounds.right() - cx;
					}

					tex_bounds.Width = cx;
				}
			}
			else
			{
				const auto split_x = std::clamp(compare_pos + 1.0, draw_bounds.X + 1.0, draw_bounds.right() - 1.0) -
					draw_bounds.X;
				draw_bounds.X += split_x;
				draw_bounds.Width -= split_x;

				const auto cx = tex_dims.cx * draw_bounds.Width / image_bounds.Width;
				const auto cy = tex_dims.cy * draw_bounds.Width / image_bounds.Width;

				if (flips_xy(orientation))
				{
					if (!is_inverted(orientation))
					{
						tex_bounds.Y = tex_bounds.bottom() - cy;
					}

					tex_bounds.Height = cy;
				}
				else
				{
					if (!is_inverted(orientation))
					{
						tex_bounds.X = tex_bounds.right() - cx;
					}

					tex_bounds.Width = cx;
				}
			}
		}


		const auto dst_quad = setting.show_rotated
			                      ? quadd(draw_bounds.offset(offset)).transform(to_simple_transform(orientation))
			                      : draw_bounds.offset(offset);

		rc.draw_texture(tex, dst_quad, tex_bounds.round(), alpha, sampler);

		draw_texture_info(rc, image_bounds.round().offset(offset), tex, orientation, sampler, alpha);

		_last_draw_tex = tex;
		_last_draw_source_rect = tex_bounds.round();
		_last_drawn_sampler = sampler;
	}

	// Names the rung of the ladder on screen, so a flash can be attributed rather than guessed at.
	if (setting.show_debug_info && media_bounds.width() > 96)
	{
		const auto phase = !tex || !tex->is_valid()
			                   ? "phase 0 empty"
			                   : (_is_placeholder ? "phase 1 stand-in" : (_loaded.is_preview ? "phase 2 preview" : "loaded"));

		auto r = media_bounds.offset(offset);
		r.left += 8;
		r.top += rc.text_line_height(ui::style::font_face::dialog) + 8;
		r.bottom = r.top + rc.text_line_height(ui::style::font_face::dialog) + 8;

		rc.draw_text(std::format("{} alpha {:.2f}{}{}", phase, alpha, _fade_out_tex ? " over previous" : "",
		                         _staged_surface ? " staged" : ""),
		             r, ui::style::font_face::dialog, ui::style::text_style::single_line,
		             ui::color(0xFFFF00, 1.0f), {});
	}
}

void texture_state::release_panorama_resources()
{
	_panorama_renderer.set_source(nullptr);
	_panorama_source.reset();
	_panorama_surface.reset();
	_panorama_tex.reset();
	_panorama_gpu_tex.reset();
	_panorama_gpu_source.reset();
	_panorama_rendered = false;
}

void texture_state::layout(ui::measure_context& mc, const recti bounds, const df::item_element_ptr& i)
{
	_display_bounds = bounds;
	refresh(i);
}

// zoom.md: the file declared a sphere, so the user is put inside it. The GPU projects it with a
// shader where it can and the CPU rasterises the same frame where it cannot; rendering.md owns the
// two tiers, and the camera both are handed is derived once so neither can hold a different one.
// The camera and the decoded source together decide the frame, so a repaint that moved neither
// reuses the texture it already has.
void texture_state::draw_panorama(ui::draw_context& rc, const pointi offset, const prop::panorama_geometry& geometry,
                                  const panorama_view& view)
{
	_panorama = {true, geometry, view};
	update_decode(rc);

	// A decode that lands while projected arms a dissolve nothing here draws, and builds a flat
	// texture nothing here draws either. Both are kept deliberately: leaving the projection is one
	// keystroke, and the flat picture is expected to be there when it happens rather than decoded
	// from scratch. Left armed, the dissolve would run against that picture the moment it appeared.
	_fade_out_tex.reset();
	_display_alpha_animation.reset(1.0f);

	const auto media_bounds = _display_bounds;
	if (media_bounds.is_empty()) return;

	// Sphere the file does not hold, and every frame before the first decode arrives, read as the
	// same empty surround rather than as whatever was drawn there last.
	rc.draw_rect(media_bounds.offset(offset), ui::color(ui::style::color::group_background, 1.0f));

	const auto& source = ui::is_valid(_retained_surface) ? _retained_surface : _loaded.s;

	if (!ui::is_valid(source) || !ui::is_packed(source->format()) || !geometry.is_valid())
	{
		// A projection asks for more source than the flat view does, so it can be refused where the
		// flat view was not. Saying why is what the flat path already does; an unexplained grey
		// rectangle is the same defect wearing a different mode.
		draw_display_problem(rc, media_bounds.offset(offset), _display_problem, calc_display_dimensions(), 1.0f);
		return;
	}

	const auto viewport = sized(media_bounds.extent());
	const auto params = panorama_shader_params(geometry, view, viewport);

	// The backend projects it if it can. A shader turns panning into a constant-buffer write, so the
	// whole per-frame resample below is work only the CPU renderer has to do.
	if (_panorama_gpu_source != source)
	{
		_panorama_gpu_tex = rc.create_texture();

		if (_panorama_gpu_tex && _panorama_gpu_tex->update_mipped(source) == ui::texture_update_result::failed)
		{
			_panorama_gpu_tex.reset();
		}

		_panorama_gpu_source = source;
	}

	if (_panorama_gpu_tex && rc.draw_panorama(_panorama_gpu_tex, media_bounds.offset(offset), params, 1.0f))
	{
		return;
	}

	if (_panorama_source != source)
	{
		_panorama_renderer.set_source(source);
		_panorama_source = source;
		_panorama_rendered = false;
	}

	if (!_panorama_renderer.is_ready()) return;

	// Normally the viewport, because a projected panorama owns the whole media area. The budget is
	// the guard for a frame drawn between entering the projection and the layout that resizes for
	// it, where the bounds still describe the flat picture at full zoom - which would be a hundreds
	// of megabytes allocation and a raster to match.
	const auto extent = clamp_to_texture_budget(media_bounds.extent());

	if (!_panorama_tex)
	{
		_panorama_tex = rc.create_texture();
		_panorama_rendered = false;
	}

	if (!_panorama_tex) return;

	const auto view_moved = !_panorama_rendered ||
		_panorama_rendered_geometry != geometry ||
		!df::equiv(_panorama_rendered_view.yaw(), view.yaw()) ||
		!df::equiv(_panorama_rendered_view.pitch(), view.pitch()) ||
		!df::equiv(_panorama_rendered_view.fov(), view.fov());

	if (view_moved || !_panorama_surface || _panorama_surface->dimensions() != extent)
	{
		// The buffer is kept because a drag re-renders on every frame and would otherwise allocate
		// on every one of them.
		if (!_panorama_surface || _panorama_surface->dimensions() != extent)
		{
			_panorama_surface = std::make_shared<ui::surface>();

			if (!_panorama_surface->alloc(extent, ui::texture_format::ARGB))
			{
				// A failed alloc still reports the requested extent, so the buffer has to be dropped
				// rather than left to be recognised as the right size on the next frame.
				_panorama_surface.reset();
				return;
			}
		}

		if (!_panorama_renderer.render(*_panorama_surface, geometry, view)) return;
		if (_panorama_tex->update(_panorama_surface) == ui::texture_update_result::failed) return;

		_panorama_rendered_view = view;
		_panorama_rendered_geometry = geometry;
		_panorama_rendered = true;
	}

	// Bilinear rather than the default point: the destination is normally the viewport exactly, but
	// the budget can clamp it, and a clamped frame magnified with point sampling is blocky.
	rc.draw_texture(_panorama_tex, media_bounds.offset(offset), 1.0f, ui::texture_sampler::bilinear);
}


ui::texture_ptr texture_state::zoom_texture(ui::draw_context& rc, const sizei extent)
{
	if (!_zoom_texture || _zoom_version != _photo_version)
	{
		const auto t = rc.create_texture();

		if (t && _zoom_staged_surface &&
			t->update(_zoom_staged_surface) != ui::texture_update_result::failed)
		{
			_zoom_texture = t;
			_zoom_version = _photo_version;
		}
	}

	// The caller is a paint path that already handles a null texture by skipping the overlay.
	// Throwing here would log an exception on every frame for as long as creation keeps failing.
	return _zoom_texture;
}

void display_state_t::populate(const view_state& state)
{
	existing_textures_t existing_textures;

	const auto d = state._display;

	if (d)
	{
		if (d->_item1 && d->_selected_texture1) _common.retain_texture(d->_item1->path(), d->_selected_texture1);
		if (d->_item2 && d->_selected_texture2) _common.retain_texture(d->_item2->path(), d->_selected_texture2);
	}
	for (const auto& [path, texture] : _common._recent_textures) existing_textures[path] = texture;

	const auto& selected_items = state.selected_items();
	const auto item_count = selected_items.items().size();
	const auto no_folders = !selected_items.has_folders();

	_is_one = item_count == 1 && no_folders;
	_is_two = item_count == 2 && no_folders;
	_is_multi = item_count >= 3 && no_folders;
	_can_zoom = false;

	if (_is_one)
	{
		_item1 = selected_items.items()[0];
		_selected_texture1 = get_tex(existing_textures, _item1, _async);
		_can_zoom = _item1->file_type()->has_trait(file_traits::zoom);
		if (_item1->file_type()->has_trait(file_traits::bitmap) && !_selected_texture1->_photo_loaded)
			_selected_texture1->load_image(_item1);

		// The media view has no grid loading or staging thumbnails behind it, so the display supplies its
		// own stand-in for what it can reach next. queue_load_thumbnail covers the item that has no
		// encoded thumbnail at all, which otherwise leaves the media area empty for the whole file load.
		for (const auto& i : {_item1, state.next_item(false, false), state.next_item(true, false)})
		{
			if (!i) continue;
			if (i->has_thumb()) i->stage_thumbnail_surface(_async);
			else state.item_index.queue_load_thumbnail(i);
		}

		// zoom.md: which patch of the sphere a declared panorama holds. Read from the file rather
		// than from the index, so it costs no stored field - and only for the item on screen, which
		// is the only one that can be looked around.
		if (declares_equirectangular())
		{
			const auto md = _item1->metadata();
			const auto path = _item1->path();
			const auto source = md->dimensions();
			const auto modified = _item1->file_modified();

			panorama_item(path, source, modified);

			if (!_common._panorama.resolved &&
				(_panorama_read_path != path || _panorama_read_source != source ||
					_panorama_read_modified != modified))
			{
				_panorama_read_path = path;
				_panorama_read_source = source;
				_panorama_read_modified = modified;
				const auto weak = weak_from_this();

				_async.queue_async(async_queue::load, [weak, path, source, modified, &async = _async]
				{
					const auto declared = metadata_xmp::panorama(path);

					async.queue_ui([weak, path, source, modified, declared]
					{
						// The display may have moved on, and the session is keyed on the path for
						// exactly that reason.
						if (const auto display = weak.lock()) display->panorama_geometry(path, declared, source, modified);
					});
				});
			}
		}
	}
	else if (_is_two)
	{
		_item1 = selected_items.items()[0];
		_item2 = selected_items.items()[1];

		_selected_texture1 = get_tex(existing_textures, _item1, _async);
		_selected_texture2 = get_tex(existing_textures, _item2, _async);

		const auto file_type1 = _item1->file_type();
		const auto file_type2 = _item2->file_type();
		_can_zoom = file_type1->has_trait(file_traits::zoom) && file_type2->has_trait(file_traits::zoom);

		_can_compare = file_type1->has_trait(file_traits::bitmap) && file_type2->has_trait(
			file_traits::bitmap);

		_comparison_eligible = can_compare_file_types(file_type1, file_type2);

		_is_compare_video =
			file_type1->has_trait(file_traits::preview_video) &&
			file_type2->has_trait(file_traits::preview_video) &&
			_item1->online_status() == df::item_online_status::disk &&
			_item2->online_status() == df::item_online_status::disk;
	}
	else if (_is_multi)
	{
		_selection_item_count = item_count;
		const auto thumbnail_limit = item_count > max_surfaces ? max_surfaces - 1 : max_surfaces;

		// The pinned item leads the collage so the held item is always the first cell, whatever the
		// sort order does, and never falls off the end of a large selection.
		df::item_elements ordered;
		ordered.reserve(selected_items.items().size());
		const auto pin = state._pin_item;
		if (pin && selected_items.contains(pin)) ordered.emplace_back(pin);
		for (const auto& i : selected_items.items()) if (i != pin) ordered.emplace_back(i);

		_images.clear();
		_collage_source_items.clear();

		for (const auto& i : ordered)
		{
			if (_images.size() >= thumbnail_limit) break;
			if (!i->has_thumb()) continue;
			_images.emplace_back(i->thumbnail());
			_collage_source_items.emplace_back(i);
		}

		const auto images = _images;
		const auto weak = weak_from_this();

		_async.queue_async(async_queue::render, [images, weak, &async = _async]
		{
			files ff;
			constexpr sizei max_dims(256, 256);
			std::vector<ui::const_surface_ptr> surfaces;
			surfaces.reserve(images.size());

			for (const auto& image : images)
			{
				surfaces.emplace_back(ff.image_to_surface(image, ui::scale_dimensions(image->dimensions(), max_dims)));
				if (surfaces.size() >= max_surfaces) break;
			}

			async.queue_ui([weak, surfaces = std::move(surfaces), &async]() mutable
			{
				if (const auto display = weak.lock())
				{
					// A thumbnail the decoder refuses yields no surface, and the collage reads the two
					// lists by the same index, so an empty entry is dropped from both rather than left
					// for the layout to take dimensions from.
					std::vector<ui::const_surface_ptr> kept;
					df::item_elements kept_items;
					kept.reserve(surfaces.size());
					kept_items.reserve(std::min(surfaces.size(), display->_collage_source_items.size()));

					for (auto i = 0_z; i < surfaces.size(); ++i)
					{
						if (!ui::is_valid(surfaces[i])) continue;
						if (i < display->_collage_source_items.size())
							kept_items.emplace_back(display->_collage_source_items[i]);
						kept.emplace_back(std::move(surfaces[i]));
					}

					display->_surfaces = std::move(kept);
					display->_collage_source_items = std::move(kept_items);
					display->_textures.clear();
					async.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
				}
			});
		});
	}

	if (!_can_zoom)
	{
		// Non-zoomable selections release the carried single-image zoom.
		_common._zoom.fit();
	}
}

render_valid display_state_t::update_for_present(const double time_now) const
{
	auto result = render_valid::valid;

	if (_session)
	{
		const auto present_updated = _session->update_for_present(time_now);
		if (_item1)
		{
			_item1->media_position(_session->pos(time_now));
		}

		if (present_updated)
		{
			if (_audio_verts)
			{
				auto vert_bounds = _audio_element_bounds;

				if (vert_bounds.width() > 100)
				{
					vert_bounds.left += 20;
					vert_bounds.right -= 20;
				}

				const auto has_tex = _player_media_info.has_video && _selected_texture1 && _selected_texture1->_vid_tex;
				vert_bounds.top = vert_bounds.top + (has_tex ? df::mul_div(_audio_element_bounds.height(), 3, 5) : 0);

				const auto tex_bounds = ui::scale_dimensions(sizei{512, 256}, vert_bounds);
				_session->update_visualizer(_audio_verts, tex_bounds, _audio_element_offset, _audio_element_alpha,
				                            time_now);

				result = render_valid::present;
			}

			if (_selected_texture1 && _selected_texture1->_vid_tex)
			{
				const auto update_texture_result = _session->update_texture(_selected_texture1->_vid_tex);

				if (update_texture_result != render_valid::valid)
				{
					if (!_selected_texture1->_is_video_tex)
					{
						_selected_texture1->_is_video_tex = true;
						_selected_texture1->fade_out();
					}

					if (_selected_texture1->display_dimensions() != _player_media_info.display_dimensions ||
						_selected_texture1->display_orientation() != _player_media_info.display_orientation)
					{
						_selected_texture1->_display_dimensions = _player_media_info.display_dimensions;
						_selected_texture1->_display_orientation = _player_media_info.display_orientation;

						_async.invalidate_view(view_invalid::view_layout);
					}
				}

				if (update_texture_result == render_valid::invalid || result == render_valid::invalid)
				{
					result = render_valid::invalid;
				}
				else if (update_texture_result == render_valid::present || result == render_valid::present)
				{
					result = render_valid::present;
				}
			}
		}
	}

	return result;
}

texture_state::texture_state(async_strategy& async, const df::item_element_ptr& i) : _async(async)
{
	_loaded.clear();
	_loaded.i = i->thumbnail();
	_loaded.success = !_loaded.is_empty();

	const auto mt = i->file_type();
	const auto md = i->metadata();

	if (md)
	{
		_display_dimensions = md->dimensions();
		_display_orientation = md->orientation;
		_display_geometry_known = !_display_dimensions.is_empty();
	}

	_is_photo = mt->has_trait(file_traits::bitmap);
	_is_raw = mt->has_trait(file_traits::raw);
	_tex_invalid = true;

	// A newly displayed item is shown at once; the only dissolve is between resolutions of one item.
	_display_alpha_animation.reset(1.0f);

	if (_display_dimensions.is_empty() && ui::is_valid(_loaded.i))
	{
		// A stand-in, not an answer: the thumbnail carries the shape but a rounded version of it, so the
		// geometry stays unknown and the first load replaces it. Claiming it as known froze the rounded
		// aspect until some later refresh happened to clear the flag, and the correction landed then.
		_display_dimensions = _loaded.i->dimensions();
		_display_orientation = _loaded.i->orientation();
	}
}


void draw_texture_info(ui::draw_context& rc, const recti media_bounds, const ui::texture_ptr& tex,
                       const ui::orientation orientation, const ui::texture_sampler sampler, const float alpha)
{
	if (tex && setting.show_debug_info && media_bounds.width() > 64)
	{
		auto tex_dims = tex->dimensions();

		if (setting.show_rotated && flips_xy(orientation))
		{
			std::swap(tex_dims.cx, tex_dims.cy);
		}

		auto r = media_bounds;

		const auto text = std::format("{} {}x{} -> {}x{} {}", to_string(tex->format()), tex_dims.cx, tex_dims.cy,
		                              r.width(), r.height(), to_string(sampler));

		r.left += 8;
		r.bottom = r.top + rc.text_line_height(ui::style::font_face::dialog) + 8;
		rc.draw_text(text, r, ui::style::font_face::dialog, ui::style::text_style::single_line,
		             ui::color(0xFFFFFF, alpha), {});
	}
}

ui::texture_sampler calc_sampler(const sizei draw_extent, const sizei texture_extent,
                                 const ui::orientation& orientation, const bool interactive, const bool provisional)
{
	auto dims = texture_extent;

	if (setting.show_rotated && flips_xy(orientation))
	{
		std::swap(dims.cx, dims.cy);
	}

	const auto sx = draw_extent.cx / static_cast<double>(dims.cx);

	// Point sampling is exact - and the crispest possible result - only at 1:1. Even a
	// fraction of a percent away it drops or duplicates whole rows and columns, which reads
	// as broken lines in screenshots, text and fine detail, so the tolerance is tight.
	constexpr double one_to_one_tolerance = 0.002;

	if (std::abs(sx - 1.0) <= one_to_one_tolerance)
	{
		return ui::texture_sampler::point;
	}

	// Above 3x, point sampling shows the source's own pixels, which is what someone judging focus
	// needs. A stand-in has no source pixels to be exact about, so the same rule would only put
	// hard blocks where the image is about to appear.
	if (sx > 3.0 && !provisional)
	{
		return ui::texture_sampler::point;
	}

	if (interactive) return ui::texture_sampler::bilinear;

	// Catmull-Rom in both directions until a magnified source pixel covers roughly three
	// device pixels. Beyond that threshold point sampling keeps source pixels exact.
	// Minification never exceeds ~2x because calc_scale_hint() already pre-scales the
	// decoded surface, so the 4x4 footprint stays well sampled.
	return ui::texture_sampler::bicubic;
}

sizei calc_draw_shape(const sizei oriented_texture_extent, const sizei display_dimensions, const bool is_video)
{
	// Only video can present non-square pixels, and only once the container's dimensions are known.
	if (!is_video || display_dimensions.is_empty() || oriented_texture_extent.is_empty())
	{
		return oriented_texture_extent;
	}

	return display_dimensions;
}

sizei texture_state::calc_display_dimensions() const
{
	auto dims = _display_dimensions;

	if (setting.show_rotated && flips_xy(_display_orientation))
	{
		std::swap(dims.cx, dims.cy);
	}

	return dims;
}

void texture_state::clear()
{
	_tex.reset();
	_last_draw_tex.reset();
	_fade_out_tex.reset();
	_display_alpha_animation.reset(1.0f);
}

bool texture_state::is_empty() const
{
	return _tex == nullptr;
}

inline void texture_state::fade_out()
{
	// Nothing has been drawn yet, so there is nothing to dissolve from and the first image appears at
	// once rather than fading up out of the background.
	if (!_last_draw_tex)
	{
		_display_alpha_animation.reset(1.0f);
		return;
	}

	_fade_out_tex = _last_draw_tex;
	_fade_out_source_rect = _last_draw_source_rect;
	_fade_out_sampler = _last_drawn_sampler;

	_display_alpha_animation.reset(0.0f, 1.0f);
}

void texture_state::display_dimensions(const sizei dims)
{
	if (_display_dimensions != dims)
	{
		_display_dimensions = dims;
	}
}

df::process_result view_state::can_process_selection_and_mark_errors(const view_host_base_ptr& view,
                                                                     const df::process_items_type file_types) const
{
	return can_process_items_and_mark_errors(view, _selected, file_types);
}

df::process_result view_state::can_process_items_and_mark_errors(const view_host_base_ptr& view,
                                                                 const df::item_set& items,
                                                                 const df::process_items_type file_types) const
{
	clear_error_items(view);
	return items.can_process(file_types, true, view);
}

bool view_state::can_process_selection(const view_host_base_ptr& view, const df::process_items_type file_types) const
{
	return !_selected.empty() && _selected.can_process(file_types, false, view).success();
}

df::process_result view_state::selection_process_result(const df::process_items_type file_types) const
{
	if (_selected.empty())
	{
		df::process_result result;
		result.code = df::process_result_code::nothing_selected;
		return result;
	}

	return _selected.can_process(file_types, false, {});
}


df::folder_counts view_state::known_folders() const
{
	df::folder_counts results;

	const auto distinct_folders = item_index.distinct_folders();

	for (const auto& path : distinct_folders)
	{
		results[path] = 255 - std::min(static_cast<int>(path.size()), 255); // short file names more likely
	}

	for (const auto& i : _search_items.items())
	{
		if (i->is_folder())
		{
			results[i->folder()] += 1 << 8;
		}
	}

	recent_folders.count(results, 2 << 8);
	history.count_folders(results, 2 << 8);

	const auto write_folder = df::folder_path(setting.write_folder);
	const auto import_destination_path = df::folder_path(setting.import.destination_path);

	if (!import_destination_path.is_empty()) results[import_destination_path] += 2 << 8;
	if (!write_folder.is_empty()) results[write_folder] += 2 << 8;

	return results;
}


// An empty result means there is no sibling in that direction. Navigation then does nothing rather
// than silently changing level.
static df::folder_path next_folder(const df::folder_path current, const bool is_forward)
{
	const auto parent = current.parent();
	auto peers = platform::select_folders(df::item_selector(parent), setting.show_hidden);

	std::ranges::sort(peers, [](auto&& l, auto&& r) { return str::icmp(l.name, r.name) < 0; });

	const auto name = current.name();
	const auto found = std::ranges::find_if(peers, [name](auto&& peer) { return str::icmp(name, peer.name) == 0; });

	if (found == peers.end())
	{
		return {};
	}

	if (is_forward)
	{
		const auto next = std::next(found);
		return next == peers.end() ? df::folder_path{} : parent.combine(next->name);
	}

	return found == peers.begin() ? df::folder_path{} : parent.combine(std::prev(found)->name);
}

void view_state::refresh_sibling_folders()
{
	_sibling_folders = {};

	if (!_search.has_selector())
	{
		return;
	}

	const auto scope = _search;
	const auto folder = _search.selectors().front().folder();

	_async.queue_async(async_queue::scan_folder, [this, scope, folder]
	{
		sibling_folders_t found;
		found.scope = scope;

		if (folder.exists())
		{
			found.next = next_folder(folder, true);
			found.previous = next_folder(folder, false);
		}

		_async.queue_ui([this, found = std::move(found)]
		{
			if (found.scope == _search)
			{
				_sibling_folders = found;
				invalidate_view(view_invalid::command_state);
			}
		});
	});
}

std::string view_state::next_path(const bool forward) const
{
	if (_search.has_selector())
	{
		const auto& sibling = forward ? _sibling_folders.next : _sibling_folders.previous;
		return sibling.is_empty() ? std::string{} : std::string(sibling.text());
	}

	if (_search.has_date())
	{
		auto a = _search;
		a.next_date(forward);
		return a.text();
	}

	return {};
}

bool view_state::has_next_path(const bool forward) const
{
	if (_search.has_selector())
	{
		return !(forward ? _sibling_folders.next : _sibling_folders.previous).is_empty();
	}

	return _search.has_date();
}

void view_state::open_next_path(const view_host_base_ptr& view, const bool forward)
{
	if (_search.has_selector())
	{
		const auto sibling = forward ? _sibling_folders.next : _sibling_folders.previous;

		if (!sibling.is_empty())
		{
			auto a = _search;
			open(view, a.clear_selectors().add_selector(sibling), {});
		}
	}
	else if (_search.has_date())
	{
		auto a = _search;
		a.next_date(forward);
		open(view, a, {});
	}

	stop_slideshow();
}
