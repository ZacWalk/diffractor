// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Session-aggregate performance diagnostics: what thumbnails, the database, queries, files, the
// index and the worker queues cost this session, summarised once at exit with what painting and
// Direct3D cost, which the render layer counts (platform_render.h).

#pragma once

namespace df
{
	struct thumbnail_counters
	{
		std::atomic_uint32_t scan_batches = 0;
		std::atomic_uint32_t scan_batches_cancelled = 0;
		std::atomic_uint32_t scan_batches_queued = 0;
		std::atomic_uint32_t scan_batches_pending = 0;
		std::atomic_uint32_t scan_batches_pending_peak = 0;
		std::atomic_uint64_t scan_thumbs_requested = 0;
		std::atomic_uint64_t scan_thumbs_scanned = 0;
		std::atomic_uint64_t scan_completions_stale = 0;
		std::atomic_uint64_t stage_requests = 0;
		std::atomic_uint64_t stage_skipped = 0;
		std::atomic_uint64_t stage_coalesced = 0;
		std::atomic_uint64_t stage_decodes = 0;
		std::atomic_uint64_t stage_discarded = 0;
		std::atomic_uint64_t published_db = 0;
		std::atomic_uint64_t published_shell = 0;
		std::atomic_uint64_t shell_retries = 0;
		std::atomic_uint64_t load_failures = 0;
	};

	struct db_counters
	{
		std::atomic_uint64_t read_batches = 0;
		std::atomic_uint64_t thumbnails_read = 0;
		std::atomic_uint64_t read_us = 0;
		std::atomic_uint32_t read_max_us = 0;
		std::atomic_uint64_t write_batches = 0;
		std::atomic_uint64_t items_written = 0;
		std::atomic_uint64_t thumbs_written = 0;
		std::atomic_uint64_t write_us = 0;
		std::atomic_uint32_t write_max_us = 0;
	};

	// Query work is split because the two halves run on different threads: matching on a worker,
	// materializing into item_elements on the UI thread where its cost is felt directly.
	struct query_counters
	{
		std::atomic_uint64_t queries = 0;
		std::atomic_uint64_t query_us = 0;
		std::atomic_uint32_t query_max_us = 0;
		std::atomic_uint64_t query_items = 0;
		std::atomic_uint64_t materializations = 0;
		std::atomic_uint64_t materialize_us = 0;
		std::atomic_uint32_t materialize_max_us = 0;
		std::atomic_uint64_t materialize_items = 0;
		std::atomic_uint64_t counts = 0;
		std::atomic_uint64_t count_us = 0;
	};

	struct file_counters
	{
		std::atomic_uint64_t scans = 0;
		std::atomic_uint64_t scan_us = 0;
		std::atomic_uint32_t scan_max_us = 0;
		// Full-image loads for display, one per step through a folder, and the slowest thing the load
		// queue does - a backlog here is what leaves the viewer blank.
		std::atomic_uint64_t loads = 0;
		std::atomic_uint64_t load_us = 0;
		std::atomic_uint32_t load_max_us = 0;
		std::atomic_uint64_t decodes = 0;
		std::atomic_uint64_t decode_us = 0;
		std::atomic_uint32_t decode_max_us = 0;
		std::atomic_uint64_t decode_bytes = 0;
		// Metadata parse failures on per-item paths, counted rather than logged so a bad batch of
		// files cannot flood the log with one line each.
		std::atomic_uint64_t metadata_errors = 0;
	};

	// Content hashing for duplicate detection. A CRC costs a full read and a perceptual hash costs a
	// read and a decode, so what these answer is how much of that work was avoidable: a hash computed
	// for a path that already carried one, or held by a picture that could never have matched.
	struct index_counters
	{
		std::atomic_uint64_t phash_computed = 0;
		std::atomic_uint64_t phash_usable = 0;
		std::atomic_uint64_t phash_declined = 0;
		std::atomic_uint64_t phash_unreadable = 0;
		// Hashed, but nothing retained the result - no index item at that path, or a database update
		// that matched no row - so the next pass over the same file pays for the decode again.
		std::atomic_uint64_t phash_unpersisted = 0;
		std::atomic_uint64_t phash_unwritten = 0;
		std::atomic_uint64_t phash_presence = 0;
		std::atomic_uint64_t phash_bytes = 0;
		std::atomic_uint64_t phash_us = 0;
		std::atomic_uint32_t phash_max_us = 0;

		// Gauges, not totals: each holds what the most recent predictions pass saw. Summing passes
		// would count one picture once per pass and hide whether the candidate rule is holding.
		std::atomic_uint32_t pass_files = 0;
		std::atomic_uint32_t pass_crc_held = 0;
		std::atomic_uint32_t pass_dup_groups = 0;
		std::atomic_uint32_t pass_pictures = 0;
		std::atomic_uint32_t pass_buckets = 0;
		std::atomic_uint32_t pass_candidates = 0;
		std::atomic_uint32_t pass_wanted = 0;
		std::atomic_uint32_t pass_usable_held = 0;
		std::atomic_uint32_t pass_declined_held = 0;
		std::atomic_uint32_t pass_uninvited = 0;
		std::atomic_uint32_t pass_matched = 0;
		std::atomic_uint32_t pass_crowded = 0;

		// The shape narrowing applied alongside the shared capture time. Solo counts pictures refused
		// because nothing under their timestamp shares their shape; the swap variant is the one the gate
		// actually applies, because a quarter turn transposes the stored extent.
		std::atomic_uint32_t pass_dims_unknown = 0;
		std::atomic_uint32_t pass_aspect_solo = 0;
		std::atomic_uint32_t pass_aspect_solo_swap = 0;
		std::atomic_uint32_t pass_matched_cross_aspect = 0;
	};

	// One slot per worker queue, claimed once by its worker thread at startup so the dispatch loop
	// can account tasks without knowing which queue it is draining.
	// Aligned because the slots are an array whose elements have different owners: unpadded, two
	// queues share a line and every worker's per-task bump invalidates the other's copy. 64 bytes is
	// the line on every target this builds for; the library constant is tuning-dependent on GCC.
	struct alignas(64) queue_counters
	{
		std::string_view name;
		std::atomic_uint64_t tasks = 0;
		std::atomic_uint64_t busy_us = 0;
		std::atomic_uint32_t task_max_us = 0;
		std::atomic_uint64_t wait_us = 0;
		std::atomic_uint32_t wait_max_us = 0;
		std::atomic_uint32_t batches = 0;
		std::atomic_uint32_t batch_peak = 0;
	};

	// Never null: extra queues share an overflow slot rather than forcing a null check into the
	// dispatch loop. Name must outlive the process (a literal).
	queue_counters* register_queue(std::string_view name);

	extern thumbnail_counters thumbnail_perf;
	extern db_counters db_perf;
	extern query_counters query_perf;
	extern file_counters file_perf;
	extern index_counters index_perf;

	// Writes the whole-session summary as one grouped block, or nothing at all when the app did no
	// measurable work, so an idle run adds no log noise.
	void log_perf_summary();
}
