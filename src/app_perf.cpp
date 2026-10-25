// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The session's performance counters and the summary of them written to the log at exit.

#include "pch.h"

df::thumbnail_counters df::thumbnail_perf;
df::db_counters df::db_perf;
df::query_counters df::query_perf;
df::file_counters df::file_perf;
df::index_counters df::index_perf;

namespace
{
	constexpr size_t max_perf_queues = 32;
	std::array<df::queue_counters, max_perf_queues> perf_queues;
	// Publishes the slots below it: log_perf_summary reads the names without perf_queue_mutex, so the
	// count is the only edge that orders a reader against the plain string_view write in register_queue.
	std::atomic_size_t perf_queue_count;
	platform::mutex perf_queue_mutex;
	const int64_t perf_start_us = df::now_us();

	std::string format_us(const uint64_t us)
	{
		if (us >= 10'000'000) return std::format("{:.1f}s", static_cast<double>(us) / 1'000'000.0);
		if (us >= 1'000'000) return std::format("{:.2f}s", static_cast<double>(us) / 1'000'000.0);
		if (us >= 100'000) return std::format("{:.0f}ms", static_cast<double>(us) / 1'000.0);
		if (us >= 10'000) return std::format("{:.1f}ms", static_cast<double>(us) / 1'000.0);
		if (us >= 1'000) return std::format("{:.2f}ms", static_cast<double>(us) / 1'000.0);
		return std::format("{}us", us);
	}

	std::string format_average(const uint64_t total_us, const uint64_t count)
	{
		return count == 0 ? "0us" : format_us(total_us / count);
	}

	// Grouped rather than rounded: cross-checking one counter against another (skipped + coalesced +
	// decodes == requests, for example) is how a measurement defect gets caught, and rounding hides it.
	std::string format_count(const uint64_t n)
	{
		auto digits = std::format("{}", n);
		for (auto i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
		{
			digits.insert(i, 1, ',');
		}
		return digits;
	}

	uint64_t load(const std::atomic_uint64_t& v) { return v.load(std::memory_order_relaxed); }
	uint32_t load(const std::atomic_uint32_t& v) { return v.load(std::memory_order_relaxed); }

	std::string format_latency(const df::latency_counters& counters)
	{
		return std::format(">16.7ms={}; >50ms={}; >100ms={}", format_count(load(counters.over_16ms)),
		                   format_count(load(counters.over_50ms)), format_count(load(counters.over_100ms)));
	}

	void append_perf_row(std::string& report, const std::string_view area, const std::string_view operation,
	                     const std::string_view runs, const std::string_view total,
	                     const std::string_view average, const std::string_view maximum,
	                     const std::string_view detail = "-"sv)
	{
		report += std::format("| {} | {} | {} | {} | {} | {} | {} |\n", area, operation, runs, total,
		                      average, maximum, detail);
	}
}

df::queue_counters* df::register_queue(const std::string_view name)
{
	// Runs once per worker thread at startup. Threads that share a queue share its slot, so the
	// summary reports one row per queue rather than one per thread.
	platform::exclusive_lock lock(perf_queue_mutex);
	const auto count = perf_queue_count.load(std::memory_order_acquire);

	for (size_t i = 0; i < count; ++i)
	{
		if (perf_queues[i].name == name) return &perf_queues[i];
	}

	if (count >= max_perf_queues - 1)
	{
		auto& overflow = perf_queues[max_perf_queues - 1];
		overflow.name = std::string_view("overflow");
		perf_queue_count.store(max_perf_queues, std::memory_order_release);
		return &overflow;
	}

	perf_queues[count].name = name;
	perf_queue_count.store(count + 1, std::memory_order_release);
	return &perf_queues[count];
}

void df::log_perf_summary()
{
	const auto& t = thumbnail_perf;
	const auto& u = ui_perf;
	const auto& g = gpu_perf;
	const auto& d = db_perf;
	const auto& q = query_perf;
	const auto& f = file_perf;
	const auto& x = index_perf;
	const auto& h = platform::file_hash_perf;
	const auto queue_count = std::min(perf_queue_count.load(std::memory_order_acquire), max_perf_queues);
	auto has_queue_work = false;
	for (size_t i = 0; i < queue_count; ++i) has_queue_work |= load(perf_queues[i].tasks) != 0;

	if (load(u.idle_drains) == 0 && load(u.paints) == 0 && load(g.frames) == 0 && load(f.scans) == 0 &&
		load(f.loads) == 0 && load(f.decodes) == 0 && load(d.read_batches) == 0 && load(d.write_batches) == 0 &&
		load(q.queries) == 0 && load(q.counts) == 0 && load(h.computed) == 0 && load(x.phash_computed) == 0 &&
		load(t.stage_requests) == 0 && load(t.scan_thumbs_requested) == 0 && !has_queue_work)
	{
		return;
	}

	std::string report = std::format(
		"\n### {} performance results\n\n"
		"| Area | Operation | Runs | Total | Average | Maximum | Detail |\n"
		"|---|---|---:|---:|---:|---:|---|\n", s_app_name);
	append_perf_row(report, "Session", "Uptime", "1", format_us(static_cast<uint64_t>(now_us() - perf_start_us)),
	                "-", "-", std::format("version={}", format_version(true)));

	platform::memory_usage_t memory;
	if (platform::memory_usage(memory))
	{
		append_perf_row(report, "Memory", "Working set", "-", file_size(memory.working_set).str(), "-",
		                file_size(memory.peak_working_set).str(),
		                std::format("private={}; shared={}; commit={}", file_size(memory.private_working_set).str(),
		                            file_size(memory.shared_working_set).str(), file_size(memory.commit).str()));
	}

	append_perf_row(report, "UI", "Idle drain", format_count(load(u.idle_drains)), format_us(load(u.idle_us)),
	                format_average(load(u.idle_us), load(u.idle_drains)), format_us(load(u.idle_max_us)),
	                std::format("tasks={}; peak-batch={}", format_count(load(u.idle_tasks)),
	                            load(u.idle_batch_peak)));
	append_perf_row(report, "UI queue", "Ready-to-start wait", format_count(load(u.idle_tasks)),
	                format_us(load(u.idle_wait_us)), format_average(load(u.idle_wait_us), load(u.idle_tasks)),
	                format_us(load(u.idle_wait_max_us)));

	if (load(u.paints) != 0)
	{
		const auto paints = load(u.paints);
		append_perf_row(report, "Rendering", "Paint", format_count(paints), format_us(load(u.paint_us)),
		                format_average(load(u.paint_us), paints), format_us(load(u.paint_max_us)),
		                format_latency(u.paint_latency));
		append_perf_row(report, "Rendering", "Scene build", format_count(paints), format_us(load(u.scene_build_us)),
		                format_average(load(u.scene_build_us), paints), format_us(load(u.scene_build_max_us)),
		                std::format("{}; {}% of paint", format_latency(u.scene_build_latency),
		                            load(u.paint_us) ? load(u.scene_build_us) * 100 / load(u.paint_us) : 0));
		append_perf_row(report, "Rendering", "Backend render", format_count(paints),
		                format_us(load(u.backend_render_us)), format_average(load(u.backend_render_us), paints),
		                format_us(load(u.backend_render_max_us)), format_latency(u.backend_render_latency));
	}

	append_perf_row(report, "UI", "Frame requests", format_count(load(u.frame_prepares)), "-", "-", "-",
	                std::format("invalidates={}; redraws={}; hidden-skips={}; paints/prepare={:.1f}; texture-uploads={}",
	                            format_count(load(u.invalidates)), format_count(load(u.redraws)),
	                            format_count(load(u.hidden_skips)),
	                            load(u.frame_prepares) == 0
		                            ? 0.0
		                            : static_cast<double>(load(u.paints)) / load(u.frame_prepares),
	                            format_count(load(u.texture_uploads))));
	append_perf_row(report, "UI", "Animation prepares", format_count(load(u.prepares_registered_anim)), "-", "-",
	                "-", std::format("display={}; display-invalid={}; live peak={}",
	                                 format_count(load(u.prepares_display_anim)),
	                                 format_count(load(u.prepares_display_invalid)), load(u.animations_peak)));

	if (load(g.frames) != 0)
	{
		const auto frames = load(g.frames);

		append_perf_row(report, "GPU", "Submit", format_count(frames), format_us(load(g.submit_us)),
		                format_average(load(g.submit_us), frames), format_us(load(g.submit_max_us)),
		                format_latency(g.submit_latency));
		append_perf_row(report, "GPU", "Present", format_count(frames), format_us(load(g.present_us)),
		                format_average(load(g.present_us), frames), format_us(load(g.present_max_us)),
		                format_latency(g.present_latency));

		// Per frame rather than per session: the absolute totals scale with how long the app was
		// left open, but the per-frame figures are what a change to the batching has to move.
		append_perf_row(report, "GPU", "Draw batching", format_count(frames), format_count(load(g.draws)),
		                std::format("{:.1f}/frame", static_cast<double>(load(g.draws)) / frames),
		                format_count(load(g.draws_peak)),
		                std::format("merged={}; geometry={} ({} /frame)", format_count(load(g.merged)),
		                            format_count(load(g.geometry_bytes)), format_count(load(g.geometry_bytes) / frames)));
		append_perf_row(report, "GPU", "State changes", format_count(frames), "-", "-", "-",
		                std::format("shader={}; view={}; sampler={}; cbuffer={}", format_count(load(g.shader_binds)),
		                            format_count(load(g.view_binds)), format_count(load(g.sampler_binds)),
		                            format_count(load(g.cbuffer_uploads))));
		append_perf_row(report, "GPU", "Resources created", "-", "-", "-", "-",
		                std::format("views={}; targets={}; textures={}; reused={}; buffers={}; vram={}MB; peak={}MB",
		                            format_count(load(g.views_created)), format_count(load(g.targets_created)),
		                            format_count(load(g.textures_created)), format_count(load(g.textures_reused)),
		                            format_count(load(g.buffers_created)), load(g.vram_mb), load(g.vram_peak_mb)));
	}

	for (size_t i = 0; i < queue_count; ++i)
	{
		const auto& queue = perf_queues[i];
		const auto tasks = load(queue.tasks);
		if (tasks == 0) continue;

		append_perf_row(report, "Queue busy", queue.name, format_count(tasks), format_us(load(queue.busy_us)),
		                format_average(load(queue.busy_us), tasks), format_us(load(queue.task_max_us)),
		                std::format("batches={}; peak-batch={}", load(queue.batches), load(queue.batch_peak)));
		append_perf_row(report, "Queue wait", queue.name, format_count(tasks), format_us(load(queue.wait_us)),
		                format_average(load(queue.wait_us), tasks), format_us(load(queue.wait_max_us)));
	}

	if (load(d.read_batches) != 0)
	{
		append_perf_row(report, "Database", "Thumbnail reads", format_count(load(d.read_batches)),
		                format_us(load(d.read_us)), format_average(load(d.read_us), load(d.read_batches)),
		                format_us(load(d.read_max_us)),
		                std::format("thumbnails={}", format_count(load(d.thumbnails_read))));
	}

	if (load(d.write_batches) != 0)
	{
		append_perf_row(report, "Database", "Writes", format_count(load(d.write_batches)),
		                format_us(load(d.write_us)), format_average(load(d.write_us), load(d.write_batches)),
		                format_us(load(d.write_max_us)),
		                std::format("items={}; thumbnails={}", format_count(load(d.items_written)),
		                            format_count(load(d.thumbs_written))));
	}

	if (load(q.queries) != 0)
	{
		append_perf_row(report, "Query", "Match", format_count(load(q.queries)), format_us(load(q.query_us)),
		                format_average(load(q.query_us), load(q.queries)), format_us(load(q.query_max_us)),
		                std::format("items={}", format_count(load(q.query_items))));
	}

	if (load(q.materializations) != 0)
	{
		append_perf_row(report, "Query", "Materialize", format_count(load(q.materializations)),
		                format_us(load(q.materialize_us)),
		                format_average(load(q.materialize_us), load(q.materializations)),
		                format_us(load(q.materialize_max_us)),
		                std::format("items={}", format_count(load(q.materialize_items))));
	}

	if (load(q.counts) != 0)
	{
		append_perf_row(report, "Query", "Count", format_count(load(q.counts)), format_us(load(q.count_us)),
		                format_average(load(q.count_us), load(q.counts)), "-");
	}

	if (load(f.scans) != 0)
	{
		append_perf_row(report, "Files", "Scan", format_count(load(f.scans)), format_us(load(f.scan_us)),
		                format_average(load(f.scan_us), load(f.scans)), format_us(load(f.scan_max_us)),
		                std::format("metadata-errors={}", format_count(load(f.metadata_errors))));
	}

	if (load(f.loads) != 0)
	{
		append_perf_row(report, "Files", "Load", format_count(load(f.loads)), format_us(load(f.load_us)),
		                format_average(load(f.load_us), load(f.loads)), format_us(load(f.load_max_us)));
	}

	if (load(f.decodes) != 0)
	{
		append_perf_row(report, "Files", "Decode", format_count(load(f.decodes)), format_us(load(f.decode_us)),
		                format_average(load(f.decode_us), load(f.decodes)), format_us(load(f.decode_max_us)),
		                std::format("bytes={}", file_size(load(f.decode_bytes)).str()));
	}

	if (load(h.computed) != 0 || load(h.failed) != 0 || load(x.pass_files) != 0)
	{
		const auto files_walked = static_cast<uint64_t>(load(x.pass_files));
		const auto crc_held = static_cast<uint64_t>(load(x.pass_crc_held));
		const auto crc_pct = files_walked == 0 ? 0 : static_cast<int>((crc_held * 100) / files_walked);

		append_perf_row(report, "Hash", "CRC", format_count(load(h.computed)), format_us(load(h.us)),
		                format_average(load(h.us), load(h.computed)), format_us(load(h.max_us)),
		                std::format("bytes={}; failed={}", file_size(load(h.bytes)).str(),
		                            format_count(load(h.failed))));
		append_perf_row(report, "Hash", "CRC last pass", format_count(files_walked), "-", "-", "-",
		                std::format("held={} ({}%); duplicate-groups={}", format_count(crc_held), crc_pct,
		                            format_count(load(x.pass_dup_groups))));
	}

	if (load(x.phash_computed) != 0 || load(x.pass_pictures) != 0)
	{
		// A picture is only supposed to be hashed when another picture shares its capture time, so
		// unpersisted and uninvited are the two numbers that say whether that rule is actually holding.
		append_perf_row(report, "Hash", "Perceptual", format_count(load(x.phash_computed)),
		                format_us(load(x.phash_us)), format_average(load(x.phash_us), load(x.phash_computed)),
		                format_us(load(x.phash_max_us)),
		                std::format("bytes={}; usable={}; declined={}; unreadable={}; unpersisted={}; unwritten={}; presence={}",
		                            file_size(load(x.phash_bytes)).str(), format_count(load(x.phash_usable)),
		                            format_count(load(x.phash_declined)), format_count(load(x.phash_unreadable)),
		                            format_count(load(x.phash_unpersisted)), format_count(load(x.phash_unwritten)),
		                            format_count(load(x.phash_presence))));

		const auto candidates = static_cast<uint64_t>(load(x.pass_candidates));
		const auto uninvited = static_cast<uint64_t>(load(x.pass_uninvited));
		const auto held = static_cast<uint64_t>(load(x.pass_usable_held)) + load(x.pass_declined_held);
		const auto excess_pct = candidates == 0 ? 0 : static_cast<int>((uninvited * 100) / candidates);

		append_perf_row(report, "Hash", "Perceptual last pass", format_count(load(x.pass_pictures)), "-", "-", "-",
		                std::format("capture-times={}; candidates={}; wanted={}; crowded={}; matched={}; held={} "
		                            "(usable={}, declined={}); uninvited={} ({}%)",
		                            format_count(load(x.pass_buckets)), format_count(candidates),
		                            format_count(load(x.pass_wanted)), format_count(load(x.pass_crowded)),
		                            format_count(load(x.pass_matched)), format_count(held),
		                            format_count(load(x.pass_usable_held)), format_count(load(x.pass_declined_held)),
		                            format_count(uninvited), excess_pct));

		// solo-with-swap is what the gate refused; solo is what a gate blind to rotation would have
		// refused, and the gap between them is what supporting a quarter turn costs.
		append_perf_row(report, "Hash", "Shape gate", "-", "-", "-", "-",
		                std::format("refused={}; strict-refused={}; cross-shape-matches={}; dimensions-unknown={}",
		                            format_count(load(x.pass_aspect_solo_swap)),
		                            format_count(load(x.pass_aspect_solo)),
		                            format_count(load(x.pass_matched_cross_aspect)),
		                            format_count(load(x.pass_dims_unknown))));
	}

	const auto thumbs_requested = load(t.scan_thumbs_requested);
	const auto thumbs_scanned = load(t.scan_thumbs_scanned);
	const auto decodes = load(t.stage_decodes);
	const auto discarded = load(t.stage_discarded);

	if (load(t.stage_requests) != 0 || thumbs_requested != 0)
	{
		// The two ratios the pipeline is tuned against: how much thumbnail work a batch abandons to
		// cancellation, and how many decoded surfaces are thrown away as stale.
		const auto abandoned_pct = thumbs_requested == 0
			                           ? 0
			                           : static_cast<int>(((thumbs_requested - thumbs_scanned) * 100) /
				                           thumbs_requested);
		const auto discarded_pct = decodes == 0 ? 0 : static_cast<int>((discarded * 100) / decodes);

		append_perf_row(report, "Thumbnails", "Scan", format_count(load(t.scan_batches)), "-", "-", "-",
		                std::format("queued={}; cancelled={}; peak-pending={}; requested={}; scanned={}; "
		                            "abandoned={}%; stale={}",
		                            load(t.scan_batches_queued), load(t.scan_batches_cancelled),
		                            load(t.scan_batches_pending_peak), format_count(thumbs_requested),
		                            format_count(thumbs_scanned), abandoned_pct, load(t.scan_completions_stale)));
		append_perf_row(report, "Thumbnails", "Stage", format_count(load(t.stage_requests)), "-", "-", "-",
		                std::format("skipped={}; coalesced={}; decodes={}; discarded={} ({}%); published-db={}; "
		                            "published-shell={}; retries={}; failures={}",
		                            format_count(load(t.stage_skipped)), format_count(load(t.stage_coalesced)),
		                            format_count(decodes), format_count(discarded), discarded_pct,
		                            format_count(load(t.published_db)), load(t.published_shell), load(t.shell_retries),
		                            load(t.load_failures)));
	}

	log("perf results", report);
}
