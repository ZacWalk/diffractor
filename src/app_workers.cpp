// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Background execution behind async_strategy. Starts the worker threads and their queues,
// routes each queue_* request to its queue, runs the media read/decode and preview workers, and
// records queue wait counters for diagnostics.

#include "pch.h"

#include "app_text.h"
#include "app_command_line.h"

#include "model_location.h"
#include "model_db.h"
#include "model_index.h"
#include "model.h"

#include "ui_dialog.h"
#include "view_items.h"

#include "app.h"

#include <utility>

#include "libdesktop/util_log.h"

static std::atomic_int index_version;

df::cancel_token make_scan_uncached_token_for_index_workers()
{
	return df::cancel_token::current(index_version);
}

df::cancel_token make_index_update_token_for_worker_tests()
{
	return df::cancel_token(index_version);
}

// Declared ahead of the queueing entry points below, which start these workers on first use.
void start_worker(platform::task_queue& q, std::string_view name);
void start_map_worker(platform::task_queue& q);
static void start_media_preview();
static void start_tile_db_worker(tile_cache_db& db, platform::task_queue& queue);

struct worker_queue_binding
{
	platform::task_queue* queue;
	// A literal, so the view outlives the thread that registers a perf row under it.
	std::string_view name;
};

void start_worker_group(std::vector<worker_queue_binding> bindings, std::string_view group_name);

static void record_queue_wait(const platform::task_queue::task_t& task, df::queue_counters* const perf)
{
	const auto wait_us = task.wait_us();
	df::bump(perf->wait_us, wait_us);
	df::record_peak(perf->wait_max_us, static_cast<uint32_t>(std::min<uint64_t>(wait_us, UINT32_MAX)));
}

platform::thread_event media_preview_event(false, false);
static platform::mutex media_preview_mutex;
static _Guarded_by_(media_preview_mutex) std::function<void(media_preview_state&)> next_media_preview;
static _Guarded_by_(media_preview_mutex) std::deque<std::function<void(media_preview_state&)>> media_preview_must_run;

// True while a newer speculative preview waits behind the one running. Set and cleared under
// media_preview_mutex so the running task cannot miss a request queued after it started.
static std::atomic_bool media_preview_superseded = false;

// The collection roots the settings name. Taken as a value rather than read from the global: the
// callers copy the settings on the thread that owns them and hand the copy to a worker.
df::index_roots index_folders(const settings_t::index_t& collection)
{
	df::index_roots result;
	const auto local_folders = platform::local_folders();

	if (collection.pictures && !local_folders.pictures.is_empty())
		result.folders.emplace(
			local_folders.pictures);
	if (collection.video && !local_folders.video.is_empty()) result.folders.emplace(local_folders.video);
	if (collection.music && !local_folders.music.is_empty()) result.folders.emplace(local_folders.music);
	if (collection.drop_box && !local_folders.dropbox_photos.is_empty())
		result.folders.emplace(
			local_folders.dropbox_photos);
	if (collection.onedrive_pictures && !local_folders.onedrive_pictures.is_empty())
		result.folders.emplace(
			local_folders.onedrive_pictures);
	if (collection.onedrive_video && !local_folders.onedrive_video.is_empty())
		result.folders.emplace(
			local_folders.onedrive_video);
	if (collection.onedrive_music && !local_folders.onedrive_music.is_empty())
		result.folders.emplace(
			local_folders.onedrive_music);

	parse_more_folders(result, collection.more_folders);

	return result;
}

void app_frame::queue_media_preview(std::function<void(media_preview_state&)> f, const bool must_run)
{
	if (claim_worker_start(_media_preview_worker_started))
	{
		_threads.start_if_running([] { start_media_preview(); });
	}

	// A superseded preview owns decoded surfaces and item references, so it is released after the
	// lock rather than destroyed inside it.
	std::function<void(media_preview_state&)> superseded;

	{
		platform::exclusive_lock media_lock(media_preview_mutex);

		// Only speculative preview work may be superseded. Teardown must still run: a caller waiting on
		// a dropped close blocks for its whole timeout, and any pending preview is stale once teardown
		// has been requested.
		std::swap(superseded, next_media_preview);

		if (must_run) media_preview_must_run.emplace_back(std::move(f));
		else next_media_preview = std::move(f);

		// Teardown counts too: a caller waiting to rename or delete should not queue behind the rest
		// of a preview walk whose answer is already stale.
		media_preview_superseded.store(true, std::memory_order_relaxed);
	}

	media_preview_event.set();
}

// No collection walk may validate folders before the database cache has loaded. The cache loader
// (merge_folder) and folder validation (validate_folder) both mutate the same folder nodes; if
// validation of a large folder races ahead of the loader it writes back nodes with
// metadata_scanned=0, causing that whole folder to be re-scanned on every startup. The database thread
// reports the load - including when it opened nothing and no cache can arrive - so the timeout is a
// safety net rather than the normal path. The event stays set, so every walk after the load passes
// straight through.
static void wait_for_index_cache(index_state& index)
{
	const std::vector<std::reference_wrapper<platform::thread_event>> cache_events = {
		index.cache_loaded_event(), platform::event_exit
	};

	platform::wait_for(cache_events, 30000, false);
}

// Runs on the UI thread from every completion that can finish the index bring-up - the database open,
// the startup collection walk and any walk that replaced it - and acts for whichever observes init as
// complete first. Starting the workers twice would put two consumers on every index queue: concurrent
// reads against the one disk this pipeline is deliberately serialised on, and concurrent mutation of
// the shared index by two prediction, summary and presence passes. Every walk asks because a Refresh
// or a device change replaces a pending walk, and when that was the startup one the only ask went
// with it: the scan workers then never started.
void app_frame::start_index_workers()
{
	if (!df::is_closing && !_index_workers_started && _state.item_index.is_init_complete())
	{
		_index_workers_started = true;

		if (!command_line.no_indexing)
		{
			const auto token = df::cancel_token(index_version);
			index_task_queue.enqueue([this, token]
			{
				_state.item_index.scan_uncached(token);

				invalidate_view(view_invalid::sidebar |
					view_invalid::command_state |
					view_invalid::item_scan |
					view_invalid::presence |
					view_invalid::refresh_items |
					view_invalid::index_summary);
			});
		}

		_threads.start_if_running([&q = crc_task_queue] { start_worker(q, "crc"); });
		_threads.start_if_running([&q = scan_folder_task_queue] { start_worker(q, "scan_folder"); });
		_threads.start_if_running([&q = scan_modified_items_task_queue]
		{
			start_worker(q, "scan_modified_items");
		});
		_threads.start_if_running([&q = scan_displayed_items_task_queue]
		{
			start_worker(q, "scan_displayed_items");
		});
	}

	invalidate_view(view_invalid::sidebar |
		view_invalid::item_scan |
		view_invalid::refresh_items |
		view_invalid::command_state |
		view_invalid::index_summary);
}

void app_frame::update_index()
{
	queue_index_update(false);
}

void app_frame::rebuild_index()
{
	queue_index_update(true);
}

void app_frame::queue_index_update(const bool forget_cached_metadata)
{
	if (command_line.no_indexing) return;

	auto token = df::cancel_token(index_version);

	// Copied here, on the thread that owns the settings, and read from the copy on the worker. The
	// collection settings are ordinary mutable globals: reading more_folders on a worker while the
	// options dialog assigns it is a data race on a std::string, and the folder set the worker acts
	// on should in any case be the one the user just applied.
	const auto collection = setting.collection;

	index_task_queue.reset_and_enqueue([this, token, forget_cached_metadata, collection]
	{
		// A Refresh or a device change early in startup gets here before the cache has loaded.
		wait_for_index_cache(_state.item_index);

		if (df::is_closing) return;

		// Forgetting runs on this queue, not the caller's, so it cannot interleave with the
		// scan_uncached it is meant to feed.
		if (forget_cached_metadata)
		{
			_state.item_index.forget_cached_metadata();
		}

		_state.item_index.index_roots(index_folders(collection));
		_state.item_index.index_folders(token);
		invalidate_view(view_invalid::sidebar | view_invalid::presence);

		_state.item_index.scan_uncached(token);
		invalidate_view(view_invalid::sidebar | view_invalid::item_scan | view_invalid::presence |
			view_invalid::refresh_items);

		// This walk can replace the startup one before that has run, and the startup walk carried the
		// only request to bring the index workers up. Once they are up this hop does nothing, so a
		// routine Refresh adds no second round of invalidations.
		queue_ui([this]
		{
			if (!_index_workers_started) start_index_workers();
		});
	});

	_state.update_search_is_favorite_or_collection_root();
}

static void start_media_decode_video(const std::shared_ptr<av_player>& player)
{
	log_func lf(__FUNCTION__);
	platform::set_thread_description("media decode_video");
	platform::thread_init c;
	player->decode_video();
}

static void start_media_decode_audio(const std::shared_ptr<av_player>& player)
{
	log_func lf(__FUNCTION__);
	platform::set_thread_description("media decode_audio");
	platform::thread_init c;
	platform::media_thread_priority audio_priority;
	player->decode_audio();
}

static void start_media_reading(const std::shared_ptr<av_player>& player)
{
	log_func lf(__FUNCTION__);
	platform::set_thread_description("media read");
	platform::thread_init c;
	player->reading();
}


static void start_database(database& db, platform::task_queue& database_task_queue, async_strategy& async,
                           const app_frame_ptr& app, std::function<void()> index_loaded_func)
{
	log_func lf(__FUNCTION__);
	platform::set_thread_description("database");
	auto* const perf = df::register_queue("database");

	try
	{
		platform::thread_init c;
		db.open(known_path(platform::known_folder::app_cache_data), "diffractor-cache");
		async.queue_ui(std::move(index_loaded_func));

		// The loop runs whether or not the database opened. Its tasks carry the continuations that
		// close dialogs, release thumbnail queries, and answer web-cache lookups, and it is what
		// drains the write queue - which holds encoded thumbnails and would otherwise grow for the
		// whole session against a database that can never accept it.
		async.invalidate_view(view_invalid::refresh_items);
		const std::vector<std::reference_wrapper<platform::thread_event>> events = {
			database_task_queue._event, platform::event_exit
		};

		while (!df::is_closing)
		{
			// 0 is infinite. A pending write already wakes this thread: enqueue_db_write posts a no-op
			// task on the empty-to-nonempty edge, and perform_writes below runs on every pass, so a
			// periodic tick would only find work it had already been signalled for. PRAGMA optimize
			// rides along with the next flush, which is the only time there is anything to re-analyse.
			if (wait_for(events, 0, false) == 0)
			{
				try
				{
					df::scope_locked_inc l(df::jobs_running);
					const auto tasks = database_task_queue.dequeue_all();

					if (!tasks.empty())
					{
						df::bump(perf->batches);
						df::bump(perf->tasks, tasks.size());
						df::record_peak(perf->batch_peak, static_cast<uint32_t>(tasks.size()));
					}

					for (const auto& t : tasks)
					{
						try
						{
							record_queue_wait(t, perf);
							df::perf_timer timer(perf->busy_us, &perf->task_max_us);
							t();
						}
						catch (const std::exception& e)
						{
							df::log(__FUNCTION__, e.what());
						}
					}
				}
				catch (std::exception& e)
				{
					df::log(__FUNCTION__, e.what());
				}
			}

			try
			{
				df::scope_locked_inc l(df::jobs_running);
				db.perform_writes();
			}
			catch (std::exception& e)
			{
				df::log(__FUNCTION__, e.what());
			}
		}

		db.perform_writes();
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
		app->app_fail(tt.error_index_database_failed, str::utf8_cast(e.what()));
	}

	// Truncate any tasks still queued at shutdown so their captured references
	// are released while owning objects are still alive.
	try
	{
		(void)database_task_queue.dequeue_all();
	}
	catch (...)
	{
		// Shutdown path: nothing left to report to, and the queue is discarded either way.
	}

	try
	{
		if (db.is_open())
		{
			db.close();
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
		app->app_fail(tt.error_index_database_failed, str::utf8_cast(e.what()));
	}
}

void app_frame::queue_ui(std::function<void()> f)
{
	// One idle wake drains the queued UI batch, so additional callbacks need no extra context switch.
	if (_ui_queue.enqueue(std::move(f)))
	{
		_pa->queue_idle();
	}
}

// Safe from any thread: exactly one caller sees the false-to-true transition and starts the worker,
// and every later caller sees true. A refused start (shutdown has begun) is not retried - the queued
// task is truncated with the queue instead.
bool app_frame::claim_worker_start(std::atomic_bool& started)
{
	// A worker created after this point would find its loop condition already false and exit without
	// draining anything, so the thread is never made.
	if (df::is_closing) return false;

	return !started.load(std::memory_order_acquire) && !started.exchange(true, std::memory_order_acq_rel);
}

void app_frame::ensure_worker(std::atomic_bool& started, platform::task_queue& q, const std::string_view name)
{
	if (claim_worker_start(started))
	{
		// Every name passed here is a literal, so the view outlives the thread that holds it.
		_threads.start_if_running([&q, name] { start_worker(q, name); });
	}
}

void app_frame::queue_async(const async_queue q, std::function<void()> f)
{
	switch (q)
	{
	case async_queue::crc:
		crc_task_queue.enqueue(std::move(f));
		break;
	case async_queue::scan_folder:
		scan_folder_task_queue.enqueue(std::move(f));
		break;
	case async_queue::scan_modified_items:
		scan_modified_items_task_queue.enqueue(std::move(f));
		break;
	case async_queue::scan_displayed_items:
		// Not reset_and_enqueue: each batch releases the loading claim its items were marked with on
		// enqueue, so a dropped batch would leave them claimed forever. The cancel token retires
		// superseded batches instead, and measurement says that is cheap - a retired batch breaks on
		// its first token check, so it costs a queue hop rather than a decode. See thumbnail_state.
		scan_displayed_items_task_queue.enqueue(std::move(f));
		break;
	case async_queue::load:
		load_task_queue.enqueue(std::move(f));
		break;
	case async_queue::load_raw:
		load_raw_task_queue.reset_and_enqueue(std::move(f));
		break;
	case async_queue::render:
		render_task_queue.enqueue(std::move(f));
		break;
	case async_queue::render_display:
		render_display_task_queue.enqueue(std::move(f));
		break;
	case async_queue::query:
		// Not reset_and_enqueue: this queue carries two unrelated producers. Dropping a pending task
		// would either lose a search's only publication hop (view_state::open) or strand a day in
		// _counting_days forever, so it would never be counted again. Superseded work retires itself
		// instead - a stale search breaks on its cancel token and a stale count on its generation.
		query_task_queue.enqueue(std::move(f));
		break;
	case async_queue::index_predictions_single:
		predictions_task_queue.reset_and_enqueue(std::move(f));
		break;
	case async_queue::index_summary_single:
		summary_task_queue.reset_and_enqueue(std::move(f));
		break;
	case async_queue::index_presence_single:
		presence_task_queue.reset_and_enqueue(std::move(f));
		break;
	case async_queue::auto_complete:
		ensure_worker(_auto_complete_worker_started, auto_complete_task_queue, "auto_complete");
		auto_complete_task_queue.reset_and_enqueue(std::move(f));
		break;
	case async_queue::cloud:
		ensure_worker(_cloud_worker_started, cloud_task_queue, "cloud");
		cloud_task_queue.enqueue(std::move(f));
		break;
	case async_queue::index:
		index_task_queue.enqueue(std::move(f));
		break;
	case async_queue::sidebar:
		sidebar_task_queue.enqueue(std::move(f));
		break;
	case async_queue::web:
		ensure_worker(_web_worker_started, web_task_queue, "web");
		web_task_queue.enqueue(std::move(f));
		break;
	case async_queue::map_tile:
		if (claim_worker_start(_map_tile_workers_started))
		{
			// Two, so decoding one tile never delays the fetch of the next.
			for (auto i = 0; i < 2; ++i)
			{
				_threads.start_if_running([&q = map_tile_task_queue] { start_map_worker(q); });
			}
		}
		map_tile_task_queue.enqueue(std::move(f));
		break;
	case async_queue::work:
	default:
		work_task_queue.enqueue(std::move(f));
		break;
	}
}

// Only the coalescing queues support a deadline: enqueue_after stamps the queue, so a queue that
// keeps a backlog would apply one task's delay to the tasks behind it.
void app_frame::queue_async_after(const async_queue q, const uint32_t delay_ms, std::function<void()> f)
{
	switch (q)
	{
	case async_queue::index_predictions_single:
		predictions_task_queue.enqueue_after(delay_ms, std::move(f));
		break;
	case async_queue::index_summary_single:
		summary_task_queue.enqueue_after(delay_ms, std::move(f));
		break;
	default:
		df::assert_true(false);
		queue_async(q, std::move(f));
		break;
	}
}

void app_frame::queue_location(std::function<void(location_cache&)> f)
{
	location_task_queue.enqueue([f = std::move(f), &lc = _locations] { f(lc); });
}

void app_frame::queue_database(std::function<void(database&)> f)
{
	database_task_queue.enqueue([f = std::move(f), &db = _db] { f(db); });
}

void app_frame::queue_tile_db(std::function<void(tile_cache_db&)> f)
{
	if (claim_worker_start(_tile_db_worker_started))
	{
		// Opening and pruning the tile store is deferred with it, so a session without a map never pays
		// either at startup.
		_threads.start_if_running([&db = _tile_db, &q = tile_db_task_queue] { start_tile_db_worker(db, q); });
	}

	tile_db_task_queue.enqueue([f = std::move(f), &db = _tile_db] { f(db); });
}

static void start_media_preview()
{
	log_func lf(__FUNCTION__);
	platform::set_thread_description("media_preview");

	// Every other worker truncates its queue on the way out, and for the same reason: these tasks
	// hold handles to UI-owned objects, and leaving them in a file static defers their release to
	// static destruction, after the factories and COM are already gone. It runs on the way out of
	// the loop AND out of the catch, so a throw does not skip it. The lambdas are destroyed after
	// the lock is released, because releasing a ui_owned_ptr posts to the UI queue.
	const df::scope_exit truncate([]
	{
		std::deque<std::function<void(media_preview_state&)>> must_run;
		std::function<void(media_preview_state&)> next;

		{
			platform::exclusive_lock media_lock(media_preview_mutex);
			std::swap(media_preview_must_run, must_run);
			std::swap(next_media_preview, next);
		}
	});

	try
	{
		platform::thread_init c;
		const std::vector<std::reference_wrapper<platform::thread_event>> events = {
			media_preview_event, platform::event_exit
		};
		media_preview_state preview_decoder;
		preview_decoder.superseded = &media_preview_superseded;

		while (!df::is_closing)
		{
			// The idle timeout exists only to release a decoder that is still open. Once it is closed
			// there is nothing left to release, so wait for the next request (0 is infinite) rather than
			// waking every five seconds for the rest of the session.
			const auto idle_close_ms = (preview_decoder.decoder1 || preview_decoder.decoder2) ? 5000u : 0u;
			const auto w = wait_for(events, idle_close_ms, false);

			if (0 == w)
			{
				std::deque<std::function<void(media_preview_state&)>> must_run;
				std::function<void(media_preview_state&)> f;

				{
					platform::exclusive_lock media_lock(media_preview_mutex);
					std::swap(media_preview_must_run, must_run);
					std::swap(next_media_preview, f);
					media_preview_superseded.store(false, std::memory_order_relaxed);
				}

				// Teardown first: it releases the file handles a waiting caller needs before it can
				// rename, replace or delete.
				// Each task is isolated: this is the only worker draining these queues, so letting one
				// throw past the loop would strand every later teardown with no consumer.
				for (const auto& r : must_run)
				{
					try
					{
						r(preview_decoder);
					}
					catch (const std::exception& e)
					{
						df::log(__FUNCTION__, e.what());
					}
				}

				if (f)
				{
					try
					{
						f(preview_decoder);
					}
					catch (const std::exception& e)
					{
						df::log(__FUNCTION__, e.what());
					}
				}
			}
			else if (platform::wait_for_timeout == w)
			{
				preview_decoder.close();
			}
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}
}

// Each task is isolated: the batch has already been dequeued, so letting one throw past this would
// discard every task queued behind it.
static void drain_queue(platform::task_queue& q, df::queue_counters* perf)
{
	try
	{
		df::scope_locked_inc l(df::jobs_running);
		const auto tasks = q.dequeue_all();

		if (!tasks.empty())
		{
			df::bump(perf->batches);
			df::bump(perf->tasks, tasks.size());
			df::record_peak(perf->batch_peak, static_cast<uint32_t>(tasks.size()));
		}

		for (const auto& t : tasks)
		{
			try
			{
				record_queue_wait(t, perf);
				df::perf_timer timer(perf->busy_us, &perf->task_max_us);
				t();
			}
			catch (const std::exception& e)
			{
				df::log(__FUNCTION__, e.what());
			}
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}
}

void start_worker(platform::task_queue& q, const std::string_view name)
{
	log_func lf(__FUNCTION__, name);
	platform::set_thread_description(name);

	// One slot per worker thread, so the shared loop can account every queue without knowing which.
	auto* const perf = df::register_queue(name);

	if (!df::is_closing)
	{
		try
		{
			platform::thread_init c;
			const std::vector<std::reference_wrapper<platform::thread_event>> events = {q._event, platform::event_exit};

			while (!df::is_closing)
			{
				// A debounced queue reports how long is left before its work is due; every other queue
				// reports ready as soon as it holds anything. Spending the delay in this wait is what
				// keeps it off a task, which would otherwise hold this thread for the whole debounce.
				// event_exit is manual-reset and set before df::is_closing is observed, so an indefinite
				// wait still ends at shutdown.
				const auto delay = q.delay_before_ready_ms();

				if (delay == 0)
				{
					drain_queue(q, perf);
				}
				else
				{
					// Either nothing pending (block on the events) or a debounce still running out.
					wait_for(events, delay == platform::task_queue::wait_indefinitely ? 0 : delay, false);
				}
			}
		}
		catch (std::exception& e)
		{
			df::log(__FUNCTION__, e.what());
		}
	}

	// On shutdown: truncate any remaining queued tasks. Lambdas hold references
	// (this, view_state, etc.) that may become invalid as teardown progresses;
	// destroying them now while the owning objects are still alive is safe.
	// Tasks holding the last reference to UI-owned state do not die here - ui_owned_ptr hands those
	// back to _ui_queue, which ~app_frame drains on the UI thread after joining this thread.
	try
	{
		(void)q.dequeue_all();
	}
	catch (...)
	{
		// Shutdown path: nothing left to report to, and the queue is discarded either way.
	}
}

// One thread serving several queues. Each queue keeps its own identity - its own supersede-on-enqueue
// policy, its own debounce deadline and its own perf row - so only the thread is shared. Queues are
// drained in the order given, and a drain restarts that order, so a long task delays the queues above
// it by one task rather than by the whole backlog.
void start_worker_group(std::vector<worker_queue_binding> bindings, const std::string_view group_name)
{
	log_func lf(__FUNCTION__, group_name);
	platform::set_thread_description(group_name);

	std::vector<df::queue_counters*> perf;
	perf.reserve(bindings.size());
	for (const auto& b : bindings) perf.emplace_back(df::register_queue(b.name));

	if (!df::is_closing)
	{
		try
		{
			platform::thread_init c;

			std::vector<std::reference_wrapper<platform::thread_event>> events;
			events.reserve(bindings.size() + 1);
			for (const auto& b : bindings) events.emplace_back(b.queue->_event);
			events.emplace_back(platform::event_exit);

			while (!df::is_closing)
			{
				// wait_indefinitely is the largest uint32, so this also picks the nearest deadline.
				auto wait_ms = platform::task_queue::wait_indefinitely;
				auto drained = false;

				for (size_t i = 0; i < bindings.size() && !df::is_closing; ++i)
				{
					const auto delay = bindings[i].queue->delay_before_ready_ms();

					if (delay == 0)
					{
						drain_queue(*bindings[i].queue, perf[i]);
						drained = true;
						break;
					}

					if (delay < wait_ms) wait_ms = delay;
				}

				if (drained) continue;

				wait_for(events, wait_ms == platform::task_queue::wait_indefinitely ? 0 : wait_ms, false);
			}
		}
		catch (std::exception& e)
		{
			df::log(__FUNCTION__, e.what());
		}
	}

	try
	{
		for (const auto& b : bindings) (void)b.queue->dequeue_all();
	}
	catch (...)
	{
		// Shutdown path: nothing left to report to, and the queues are discarded either way.
	}
}

void start_map_worker(platform::task_queue& q)
{
	log_func lf(__FUNCTION__, "map");
	platform::set_thread_description("map");
	auto* const perf = df::register_queue("map_tile");

	try
	{
		platform::thread_init c;
		const std::vector<std::reference_wrapper<platform::thread_event>> events = {q._event, platform::event_exit};

		while (!df::is_closing)
		{
			// Infinite, as start_worker: the re-signal below is what keeps the siblings moving, so a
			// timeout could only wake a worker to find nothing.
			if (wait_for(events, 0, false) == 0)
			{
				platform::task_queue::task_t task;

				if (q.dequeue(task))
				{
					// Wake another map worker for the next queued tile before this worker blocks on I/O.
					q._event.set();

					try
					{
						df::scope_locked_inc l(df::jobs_running);
						df::bump(perf->batches);
						df::bump(perf->tasks);
						record_queue_wait(task, perf);
						df::perf_timer timer(perf->busy_us, &perf->task_max_us);
						task();
					}
					catch (std::exception& e)
					{
						df::log(__FUNCTION__, e.what());
					}
				}
			}
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}
}

// The tile store gets its own thread because the SQLite build serialises nothing for us: one
// connection, one thread, and every statement issued from here. Decoding and downloading stay on the
// map workers, so a slow disk delays the next lookup rather than the picture on screen.
static void start_tile_db_worker(tile_cache_db& db, platform::task_queue& queue)
{
	log_func lf(__FUNCTION__, "map_tile_db");
	platform::set_thread_description("map tile database");
	auto* const perf = df::register_queue("map_tile_db");

	try
	{
		platform::thread_init c;
		db.open(resolve_tile_cache_db_path());

		// A cache carried in from earlier sessions is bounded before it is served, not once a few
		// hundred writes have already grown it further.
		db.prune();

		const std::vector<std::reference_wrapper<platform::thread_event>> events = {
			queue._event, platform::event_exit
		};

		while (!df::is_closing)
		{
			if (wait_for(events, 1000, false) == 0)
			{
				df::scope_locked_inc l(df::jobs_running);
				const auto tasks = queue.dequeue_all();

				if (!tasks.empty())
				{
					df::bump(perf->batches);
					df::bump(perf->tasks, tasks.size());
					df::record_peak(perf->batch_peak, static_cast<uint32_t>(tasks.size()));
				}

				for (const auto& t : tasks)
				{
					try
					{
						record_queue_wait(t, perf);
						df::perf_timer timer(perf->busy_us, &perf->task_max_us);
						t();
					}
					catch (std::exception& e)
					{
						df::log(__FUNCTION__, e.what());
					}
				}
			}

			// One commit per drained batch: the accessed stamps a single pan earns are worth one
			// transaction between them, never one each. Runs on the idle tick too, so a batch that
			// is not followed by another is still committed.
			db.flush();
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	try
	{
		(void)queue.dequeue_all();
		db.close();
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}
}


void app_frame::start_workers()
{
	// Runs on the work queue, so exit() can latch the thread list underneath it. start_if_running is
	// what makes that safe - an unchecked start here appends workers to a list that has already been
	// joined, and they then outlive COM and the graphics factories.
	if (df::is_closing) return;

	auto app = shared_from_this();
	async_strategy& async = *this;

	_threads.start_if_running([&player = _player] { start_media_decode_video(player); });
	_threads.start_if_running([&player = _player] { start_media_decode_audio(player); });
	_threads.start_if_running([&player = _player] { start_media_reading(player); });

	auto index_loaded_func = [this]
	{
		if (!df::is_closing)
		{
			start_index_workers();
		}
	};

	_threads.start_if_running([&db = _db, &q = database_task_queue, &async, app, index_loaded_func]
	{
		start_database(db, q, async, app, index_loaded_func);
	});

	_threads.start_if_running([&q = load_task_queue] { start_worker(q, "load"); });
	_threads.start_if_running([&q = load_raw_task_queue] { start_worker(q, "load_raw"); });
	_threads.start_if_running([&q = render_task_queue] { start_worker(q, "render"); });
	_threads.start_if_running([&q = render_display_task_queue] { start_worker(q, "render_display"); });
	_threads.start_if_running([&q = query_task_queue] { start_worker(q, "query"); });
	_threads.start_if_running([&q = location_task_queue] { start_worker(q, "locations"); });
	_threads.start_if_running([&q = index_task_queue] { start_worker(q, "index"); });

	// One thread for the four passes that read the index and publish to the sidebar. None of them holds
	// the thread while idle any more - the debounce lives in the queue - so sharing costs only the
	// serialisation of work that totals a couple of seconds across a whole session. Ordered by how
	// directly each feeds something on screen.
	_threads.start_if_running([this]
	{
		start_worker_group({
			                   {&presence_task_queue, "presence"},
			                   {&sidebar_task_queue, "sidebar"},
			                   {&predictions_task_queue, "predictions"},
			                   {&summary_task_queue, "summary"},
		                   }, "index_compute");
	});

	// auto_complete, web, cloud, map_tile, tile_db and media_preview are not started here. Each is
	// created by the entry point that first queues to it, so a session that never searches, never
	// reaches the network, never opens the map and never plays media runs without those threads.

	index_task_queue.enqueue([this, collection = setting.collection, scan_collection = !command_line.no_indexing]
	{
		wait_for_index_cache(_state.item_index);

		if (df::is_closing) return;

		_state.item_index.index_roots(index_folders(collection));

		const auto token = df::cancel_token(index_version);
		_state.item_index.index_folders(token, scan_collection);
		queue_ui([this] { start_index_workers(); });
		invalidate_view(view_invalid::sidebar | view_invalid::command_state | view_invalid::presence |
			view_invalid::index_summary);
	});

	// Issue #119: pick the location display language from the UI language before loading.
	_locations.set_display_language(setting.language);
	location_task_queue.enqueue([this]
	{
		_locations.load_index();
		invalidate_view(view_invalid::sidebar);
	});
}
