// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: View-geometry tests. Covers the zoom model (fit, stepping, anchoring, panning and the
// navigator), the item selector / view scroller widgets, the hosts' pointer gestures - the items
// view's drag and the task lists' column dividers among them - the parts the keyboard reaches among
// drawn elements, the Edit view's drafts and where a save of one goes, and the Movie view's pure
// decisions (clip stepping, session retirement, stale completions).

#include "pch.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "files.h"
#include "model_zoom.h"
#include "ui_elements.h"
#include "ui_document.h"
#include "libdesktop/ui/views.h"
#include <numeric>
#include "model_document.h"
#include "ui_dialog.h"
#include "ui_panorama.h"
#include "app_sidebar.h"
#include "view_items.h"
#include "view_edit.h"
#include "view_list.h"
#include "view_locate.h"
#include "view_media.h"
#include "view_movie.h"
#include "view_movie_audio.h"
#include "view_selector.h"
#include "view_sync.h"
#include "view_tags.h"
#include "ui_sid.h"
#include "ui_tracker.h"
#include "ui_c64.h"
#include "view_explore.h"
#include "view_faces.h"
#include "libdesktop/ui/table_element.h"
#include "app_commands.h"
#include "app_command_status.h"
#include "libdesktop/render/software.h"
#include "libdesktop/ui/test_support.h"

extern std::function<void()> test_during_detach_file_handles;

static void assert_zoom_near(const double expected, const double actual, const std::string_view message)
{
	assert_equal(true, std::abs(expected - actual) < 0.000001, message);
}

static void should_decide_when_movie_clip_sessions_are_retired()
{
	assert_equal(false, should_retire_movie_clip_session_for_stop(movie_view_state::playing_t::nothing),
	             "stopping nothing leaves the clip session alone");
	assert_equal(true, should_retire_movie_clip_session_for_stop(movie_view_state::playing_t::clip),
	             "stopping clip playback retires its session");
	assert_equal(true, should_retire_movie_clip_session_for_stop(movie_view_state::playing_t::movie),
	             "stopping movie playback also releases the clip-player slot");

	assert_equal(false, should_retire_movie_clip_session_for_document_focus_change(false),
	             "a document operation that cannot change focus need not close the clip session");
	assert_equal(true, should_retire_movie_clip_session_for_document_focus_change(true),
	             "removing or replacing the focused clip must close the clip session first");
	assert_equal(true, should_rewind_movie_clip_after_document_focus_change(true),
	             "and then park the new focused clip at its in point");

	auto clip = make_movie_clip(core::file_path(core::windows_path_semantics ? "c:\\movie\\b.mp4" : "/movie/b.mp4"));
	clip.start = 12.0;
	assert_equal(12.0, rewound_movie_clip_playhead(&clip), "rewind parks at the focused clip's in point");
	assert_equal(0.0, rewound_movie_clip_playhead(nullptr), "an empty timeline rewinds to zero");
}

// A playing movie's preview is composed at the movie's frames, as the render composes them, so its view is
// repainted when the playhead reaches another one rather than on every tick of the display.
static void should_repaint_a_playing_movie_once_a_movie_frame()
{
	auto repaints = 0;
	int64_t painted = -1;

	for (auto tick = 0; tick < 60; ++tick)
	{
		const auto frame = movie_frame_at(tick / 60.0, 30);
		if (frame != painted) ++repaints;
		painted = frame;
	}

	assert_equal(30, repaints, "a second of a 60 Hz display repaints a 30 fps movie thirty times");
	assert_equal(true, movie_frame_at(1.0 / 30.0 * 0.999, 30) == 0,
	             "a playhead short of the next frame is on this one");
	assert_equal(true, movie_frame_at(1.0 / 30.0 * 1.001, 30) == 1, "and past it is on the next");
	assert_equal(true, movie_frame_at(-1.0, 30) == 0, "a playhead before the start is on the first frame");
	assert_equal(true, movie_frame_at(2.5, 0) == 2, "and a rate the project never set counts whole seconds");
}

static void should_step_movie_clips_from_the_playhead_while_playing()
{
	std::vector<movie_clip> clips;

	for (auto i = 0; i < 9; ++i)
	{
		auto clip = make_movie_clip(
			core::file_path(core::windows_path_semantics ? "c:\\movie\\c.mp4" : "/movie/c.mp4"));
		clip.start = 0.0;
		clip.end = 10.0;
		clips.emplace_back(clip);
	}

	// Clip i starts at 9i and its crossfade into clip i + 1 runs from 9(i + 1) to 9(i + 1) + 1.
	const auto starts = calc_movie_timing(clips).starts;
	constexpr size_t focus = 0;

	// Counting from the focus, as the transport used to, makes the first answer 1.
	assert_equal(size_t{7}, movie_step_target(starts, focus, true, 58.0, true),
	             "Next while clip 6 plays goes to clip 7, whatever is focused");
	assert_equal(size_t{5}, movie_step_target(starts, focus, true, 58.0, false), "and Previous to clip 5");

	// Counting from the clip that dominates the picture -- the outgoing one -- makes these 7: the
	// incoming clip's start is still the outgoing clip at full weight, so Next would land there again.
	assert_equal(size_t{8}, movie_step_target(starts, focus, true, starts[7], true),
	             "a second Next from the start Next landed on moves on");
	assert_equal(size_t{8}, movie_step_target(starts, focus, true, starts[7] + 0.5, true),
	             "inside a crossfade the incoming clip is the one playing");

	assert_equal(size_t{8}, movie_step_target(starts, focus, true, 80.0, true), "Next stops at the last clip");
	assert_equal(size_t{0}, movie_step_target(starts, focus, true, 1.0, false), "and Previous at the first");

	assert_equal(size_t{1}, movie_step_target(starts, focus, false, 58.0, true),
	             "stopped, stepping counts from the focus");
	assert_equal(size_t{3}, movie_step_target(starts, 4, false, 58.0, false), "in both directions");
	assert_equal(size_t{4}, movie_step_target({}, 4, true, 58.0, true), "an empty timeline leaves the focus");
}

static void should_reject_stale_movie_cache_completions_before_state_changes()
{
	const auto stale_frame = decide_movie_frame_cache_completion(2, 1, true);
	assert_equal(false, stale_frame.accept, "a stale frame answer is rejected");
	assert_equal(false, stale_frame.clear_in_flight, "before it clears the current in-flight latch");
	assert_equal(false, stale_frame.dispatch_next, "and before it dispatches a current queued request");

	const auto current_frame = decide_movie_frame_cache_completion(2, 2, true);
	assert_equal(true, current_frame.accept, "a current frame answer is accepted");
	assert_equal(true, current_frame.clear_in_flight, "clears its own in-flight latch");
	assert_equal(true, current_frame.dispatch_next, "and then dispatches the next queued request");

	const auto current_last_frame = decide_movie_frame_cache_completion(2, 2, false);
	assert_equal(true, current_last_frame.clear_in_flight, "a current final answer still clears the latch");
	assert_equal(false, current_last_frame.dispatch_next, "but has no queue to dispatch");

	const auto stale_peak = decide_movie_peak_completion(4, 3, true, true);
	assert_equal(false, stale_peak.accept, "a stale peak answer is rejected");
	assert_equal(false, stale_peak.clear_pending, "before it releases the new generation's pending scan");

	const auto current_peak = decide_movie_peak_completion(4, 4, true, true);
	assert_equal(true, current_peak.accept, "a current peak answer is accepted");
	assert_equal(true, current_peak.clear_pending, "releases its own pending scan");
	assert_equal(true, current_peak.store, "and is kept");

	// The level track is drawn for one clip, so moving focus cancels the scan for the last. What a
	// cancelled scan measured is part of a file, and kept it would read as a file with no sound.
	const auto cancelled_peak = decide_movie_peak_completion(4, 4, true, false);
	assert_equal(true, cancelled_peak.clear_pending, "a cancelled scan releases the request it answered");
	assert_equal(false, cancelled_peak.store, "but what it measured is not kept");

	const auto superseded_peak = decide_movie_peak_completion(4, 4, false, true);
	assert_equal(false, superseded_peak.clear_pending, "a scan finishing after another was asked for leaves that one");
	assert_equal(true, superseded_peak.store, "and is kept, having read the whole file");
}

// A paused preview was left on the frame the movie had played on to: the session had once been sent to
// the clip's in point, and that was still remembered, so parking there again sent nothing.
static void should_seek_a_movie_preview_back_to_where_it_was()
{
	std::optional<double> sought = 2.0;

	assert_equal(false, decide_movie_preview_seek(2.0, 2.0, 0.01, sought), "a session where it is wanted stays");
	assert_equal(false, sought.has_value(), "and where it was sent is forgotten once it is there");
	assert_equal(true, decide_movie_preview_seek(7.0, 2.0, 0.01, sought),
	             "parked back at the in point after playing on, it is sent there again");
	assert_equal(false, decide_movie_preview_seek(7.0, 2.0, 0.01, sought), "once, while it is on its way");
	assert_equal(true, decide_movie_preview_seek(7.0, 4.0, 0.01, sought), "a new destination is sought at once");
	assert_equal(false, decide_movie_preview_seek(4.3, 4.0, 0.5, sought),
	             "and while playing, a session a little off its mark walks there rather than seeking");
}

// The out point is the first instant not kept, so the frame shown for it is the last one before it --
// one of the clip's own frames. A fixed 40 ms step back showed one two frames early at 60 fps.
static void should_show_the_movie_frame_at_a_trim_handle()
{
	movie_clip clip;
	clip.frame_rate = 60;
	const auto nearest_frame = [](const double time)
	{
		return static_cast<int>(std::lround(time * 60));
	};

	assert_equal(119, nearest_frame(movie_last_kept_frame_time(clip, 0, 2.0)),
	             "an out point on a frame's start shows the frame before it");
	assert_equal(120, nearest_frame(movie_last_kept_frame_time(clip, 0, 2.005)),
	             "one inside a frame shows that frame, which the movie keeps part of");
	assert_equal(1.995, movie_last_kept_frame_time(clip, 1.995, 2.0), "and never one before the in point");
	assert_near(1.0 / 120, movie_frame_period(clip) / 2, 1e-12, "it is decoded to the nearest frame of the clip's own");

	clip.frame_rate = 0;
	assert_near(0.04, movie_frame_period(clip), 1e-12, "a clip not yet measured uses the preview's step");
}

// Dragging the out handle to the start of a long clip leaves both handles on one pixel. The in handle
// won every tie, and against an in point of zero it cannot move, so the trim could not be dragged out.
static void should_let_either_movie_trim_handle_be_taken()
{
	assert_equal(0, pick_movie_trim_handle(2, 5, false, 1, 9, 10), "the nearer handle is taken");
	assert_equal(1, pick_movie_trim_handle(5, 2, true, 1, 9, 10), "whichever it is");
	assert_equal(1, pick_movie_trim_handle(3, 3, true, 0, 0.1, 600),
	             "trimmed to the start, the out handle is the one that can move");
	assert_equal(0, pick_movie_trim_handle(3, 3, false, 599.9, 600, 600), "trimmed to the end, the in handle is");
	assert_equal(0, pick_movie_trim_handle(3, 3, true, 300, 300.1, 600), "between, the side of the pointer decides");
	assert_equal(1, pick_movie_trim_handle(3, 3, false, 300, 300.1, 600), "either way");
}

// Play, Pause and Play again before the file had opened adopted the first session and then overwrote it
// without closing it; a clip whose file failed to open left the player "playing" for ever.
static void should_adopt_only_the_movie_clip_session_asked_for()
{
	assert_equal(true, decide_movie_clip_session(true, true, true) == movie_clip_session_answer::adopt,
	             "the session for the latest Play is adopted");
	assert_equal(true, decide_movie_clip_session(true, false, true) == movie_clip_session_answer::close,
	             "one a later Play replaced is closed, not adopted");
	assert_equal(true, decide_movie_clip_session(true, true, false) == movie_clip_session_answer::close,
	             "as is one the player stopped wanting");
	assert_equal(true, decide_movie_clip_session(false, true, true) == movie_clip_session_answer::stop_playing,
	             "a latest Play that could not open stops the player");
	assert_equal(true, decide_movie_clip_session(false, false, true) == movie_clip_session_answer::ignore,
	             "but a failure nobody is waiting on changes nothing");
}

static void should_bound_movie_probe_retries_for_moving_sources()
{
	assert_equal(true, decide_movie_changed_probe_action(1, 2) == movie_probe_changed_action::retry,
	             "the first changed-during-probe result is retried");
	assert_equal(true, decide_movie_changed_probe_action(2, 2) == movie_probe_changed_action::retry,
	             "the last budgeted retry is still retryable");
	assert_equal(true, decide_movie_changed_probe_action(3, 2) == movie_probe_changed_action::mark_missing,
	             "past the retry budget the clip reaches an explicit unresolved state");

	const auto shared = core::file_path(core::windows_path_semantics ? "c:\\movie\\shared.mp4" : "/movie/shared.mp4");
	std::vector<movie_probe_result> results(3);
	for (auto& result : results)
	{
		result.path = shared;
		result.changed_during_probe = true;
	}

	core::hash_map<core::file_path, int, core::ihash, core::ieq> retries;
	const auto first = decide_movie_changed_probe_retries(results, retries, 2);
	assert_equal(1_z, first.retry_paths.size(), "one changed source costs one retry, even with several clips");
	assert_equal(0_z, first.missing_paths.size(), "so no shared clip is marked missing on the first movement");
	assert_equal(1, retries[shared], "the retry counter is per source probe, not per clip");

	const auto second = decide_movie_changed_probe_retries(results, retries, 2);
	assert_equal(1_z, second.retry_paths.size(), "the second source movement is still retryable");
	const auto third = decide_movie_changed_probe_retries(results, retries, 2);
	assert_equal(0_z, third.retry_paths.size(), "the third movement exhausts the budget");
	assert_equal(1_z, third.missing_paths.size(), "and records one missing source decision for every matching clip");
	assert_equal(true, should_erase_movie_probe_retry_after_missing(true),
	             "an exhausted retry entry is erased once the source is recorded missing");
	assert_equal(true, should_clear_movie_probe_retries_after_document_replace(true),
	             "a replaced Movie document starts with a fresh probe retry budget");
}

// A project's clip that runs to the end of its source -- written "-" or "2.5-" -- arrives with no end of
// its own. The probe is what measures its source, so the probe is what must give it the rest of that
// source -- the same rule as a video just added, shared by the view and the command line.
static void should_apply_a_movie_probe_to_the_clip_it_measured()
{
	const auto path = core::file_path(core::windows_path_semantics ? "c:\\movie\\clip.mp4" : "/movie/clip.mp4");

	movie_probe_result measured;
	measured.path = path;
	measured.found = true;
	measured.duration = 30.0;
	measured.extent = {640, 480};
	measured.frame_rate = 25.0;

	movie_clip untrimmed;
	untrimmed.path = path;
	const auto whole = apply_movie_probe(untrimmed, measured);
	assert_equal(true, whole.is_probed && !whole.is_missing, "a measured source is present");
	assert_equal(30.0, whole.end, "a clip with no end runs to the end of its source");
	assert_equal(false, whole.is_trimmed(), "and is not shown as trimmed");
	assert_equal(25.0, whole.frame_rate, "and takes the measured rate");

	movie_clip start_only;
	start_only.path = path;
	start_only.start = 4.5;
	const auto rest = apply_movie_probe(start_only, measured);
	assert_equal(4.5, rest.start, "a clip trimmed only at its start keeps its in point");
	assert_equal(30.0, rest.end, "and runs to the end of its source");

	movie_clip overlong;
	overlong.path = path;
	overlong.start = 2.0;
	overlong.end = 50.0;
	const auto clamped = apply_movie_probe(overlong, measured);
	assert_equal(2.0, clamped.start, "a stored trim keeps its in point");
	assert_equal(30.0, clamped.end, "and is clamped to the source it measured");

	auto gone = measured;
	gone.found = false;
	const auto lost = apply_movie_probe(overlong, gone);
	assert_equal(true, lost.is_lost(), "a source that is not there is lost rather than unexamined");
	assert_equal(50.0, lost.end, "and keeps its stored times for a relink");

	auto still = measured;
	still.is_photo = true;
	still.duration = 0;
	movie_clip photo;
	photo.path = path;
	const auto held = apply_movie_probe(photo, still);
	assert_equal(true, held.is_photo, "a measured photo is a photo");
	assert_equal(4.0, held.end, "and a photo with no hold takes the default one");

	// A source probed afresh -- a relinked file, another of a SID file's tunes -- is heard again rather
	// than judged by what the one before it sounded like.
	auto heard = untrimmed;
	heard.sound_checked_at = 0;
	heard.sound_is_quiet = true;
	const auto afresh = apply_movie_probe(heard, measured);
	assert_equal(-1.0, afresh.sound_checked_at, "a source probed afresh is heard again");
	assert_equal(false, afresh.has_no_sound(), "so a verdict about another source keeps no sound up over it");
}

// Two requests for one frame can both complete. Counting the second store without releasing the
// first leaked bytes until the cache sat permanently over budget, evicted every frame it stored, and
// the strip and preview decoded the same photo in a loop, holding a core at full load.
static void should_count_movie_cache_bytes_once_per_frame()
{
	constexpr size_t frame = 4'000'000;
	auto bytes = movie_cache_bytes_after_store(0, 0, frame);
	assert_equal(frame, bytes, "a new frame adds its bytes");

	bytes = movie_cache_bytes_after_store(bytes, frame, frame);
	assert_equal(frame, bytes, "storing the same frame again replaces it rather than adding to it");

	bytes = movie_cache_bytes_after_store(bytes, frame, frame / 2);
	assert_equal(frame / 2, bytes, "a smaller replacement releases the difference");

	assert_equal(10_z, movie_cache_bytes_after_store(5, 20, 10), "a stale replaced size cannot underflow");
}

// Movie drew and rendered the stored pixels of an Exif-rotated photo or a portrait phone video, so
// those clips came out sideways in the strip, the preview and the rendered file.
static void should_turn_movie_frames_upright()
{
	assert_equal(true, sizei{1200, 1800} == upright_movie_extent({1800, 1200}, core::orientation::right_top),
	             "a quarter-turned source is measured upright");
	assert_equal(true, sizei{1800, 1200} == upright_movie_extent({1800, 1200}, core::orientation::bottom_right),
	             "a half-turned source keeps its shape");

	const auto source = std::make_shared<core::surface>();
	source->alloc(3, 2, core::texture_format::RGB, core::orientation::right_top);
	for (auto y = 0; y < 2; ++y)
	{
		for (auto x = 0; x < 3; ++x) source->set_pixel(x, y, core::rgba(10 + x + y * 3, 0, 0));
	}

	const auto turned = upright_movie_frame(source);
	assert_equal(true, sizei{2, 3} == turned->dimensions(), "the frame is turned upright");
	assert_equal(static_cast<int>(core::orientation::top_left), static_cast<int>(turned->orientation()),
	             "and no longer asks to be turned again");

	// Orientation 6 stores the visual top as column 0 and the visual right as row 0, so each displayed
	// corner is a known stored pixel - an expectation that does not borrow the transform under test.
	assert_equal(source->get_pixel(0, 1), turned->get_pixel(0, 0), "top left is the stored bottom of column 0");
	assert_equal(source->get_pixel(0, 0), turned->get_pixel(1, 0), "top right is the stored top of column 0");
	assert_equal(source->get_pixel(2, 1), turned->get_pixel(0, 2), "bottom left is the stored bottom of column 2");
	assert_equal(source->get_pixel(2, 0), turned->get_pixel(1, 2), "bottom right is the stored top of column 2");

	const auto upright = std::make_shared<core::surface>();
	upright->alloc(3, 2, core::texture_format::RGB, core::orientation::top_left);
	assert_equal(true, upright_movie_frame(upright) == core::const_surface_ptr(upright),
	             "an upright frame is used as it is");

	// The mirrored orientations were left as stored: unmirrored, and sideways for 5 and 7, though the
	// extent measured them upright. Each displayed corner against the stored pixel EXIF puts there.
	struct mirrored
	{
		core::orientation orientation;
		sizei extent;
		pointi top_left, top_right, bottom_left;
	};

	for (const auto& m : {mirrored{core::orientation::top_right, {3, 2}, {2, 0}, {0, 0}, {2, 1}},
	                      mirrored{core::orientation::bottom_left, {3, 2}, {0, 1}, {2, 1}, {0, 0}},
	                      mirrored{core::orientation::left_top, {2, 3}, {0, 0}, {0, 1}, {2, 0}},
	                      mirrored{core::orientation::right_bottom, {2, 3}, {2, 1}, {2, 0}, {0, 1}}})
	{
		const auto name = std::format("orientation {}", static_cast<int>(m.orientation));
		source->orientation(m.orientation);
		const auto shown = upright_movie_frame(source);
		assert_equal(true, m.extent == shown->dimensions(), name + " is measured upright");
		assert_equal(true, m.extent == upright_movie_extent({3, 2}, m.orientation), name + " as the extent says");
		assert_equal(static_cast<int>(core::orientation::top_left), static_cast<int>(shown->orientation()),
		             name + " is no longer to be turned");
		assert_equal(source->get_pixel(m.top_left.x, m.top_left.y), shown->get_pixel(0, 0), name + " top left");
		assert_equal(source->get_pixel(m.top_right.x, m.top_right.y), shown->get_pixel(m.extent.cx - 1, 0),
		             name + " top right");
		assert_equal(source->get_pixel(m.bottom_left.x, m.bottom_left.y), shown->get_pixel(0, m.extent.cy - 1),
		             name + " bottom left");
	}

	const auto target = recti(0, 0, 20, 30);
	assert_equal(true, upright_movie_destination(target, core::orientation::top_left) == quadd(target),
	             "an upright texture draws into its target unchanged");
	assert_equal(false, upright_movie_destination(target, core::orientation::right_top) == quadd(target),
	             "a turned texture is drawn through a turned quad");
}

static void should_retire_movie_source_caches_only_after_successful_replacement()
{
	assert_equal(false, should_retire_movie_sources_after_project_change(false),
	             "a cancelled or failed Open leaves the current source generation alone");
	assert_equal(true, should_retire_movie_sources_after_project_change(true),
	             "a successful reload or relink retires source caches and preview sessions");
	assert_equal(true, should_clear_movie_preview_audio_after_source_retire(true),
	             "retiring source caches also clears same-path preview PCM windows");

	const auto old_generation = 7;
	const auto new_generation = next_movie_preview_audio_source_generation(old_generation);
	assert_equal(false, should_accept_movie_preview_pcm_loaded(new_generation, old_generation),
	             "a PCM read that started before source retirement cannot refill the same key");
	assert_equal(true, should_accept_movie_preview_pcm_loaded(new_generation, new_generation),
	             "a read from the current source generation can fill the window");
}

static df::item_element_ptr make_versioned_photo_item(const core::file_path path, const core::date_t file_modified,
                                                      const core::date_t thumbnail_modified)
{
	df::index_file_item info;
	info.ft = files::file_type_from_name(path.name());
	info.file_created = file_modified;
	info.file_modified = file_modified;
	auto result = std::make_shared<df::item_element>(path, info);
	result->thumbnail(nullptr, nullptr, thumbnail_modified);
	return result;
}

static void update_versioned_photo_item(const df::item_element_ptr& item, const core::file_path path,
                                        const core::date_t file_modified, const core::date_t thumbnail_modified)
{
	df::index_file_item info;
	info.ft = files::file_type_from_name(path.name());
	info.file_created = file_modified;
	info.file_modified = file_modified;
	item->update(path, info);
	item->thumbnail(nullptr, nullptr, thumbnail_modified);
}

// MOD-016 - display freshness compares the source file stamp for inequality, not a monotone maximum
// or the late-arriving thumbnail stamp.
static void should_reload_displayed_photo_when_source_version_moves_backward()
{
	deferred_async_strategy as;
	const auto path = core::file_path("c:\\source-version.jpg");
	const auto item = make_versioned_photo_item(path, core::date_t(200), core::date_t(200));
	auto texture = std::make_shared<texture_state>(as, item);

	texture->load_image(item);
	assert_equal(1_z, as.pending_worker_count(async_queue::load), "the initial load is queued");

	update_versioned_photo_item(item, path, core::date_t(100), core::date_t(200));
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "a changed file stamp reloads even when the maximum timestamp is unchanged");
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load), "the same tuple does not reload again");

	update_versioned_photo_item(item, path, core::date_t(100), core::date_t(100));
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "thumbnail stamp changes alone do not full-decode the displayed photo again");

	texture->mark_visuals_current();
	update_versioned_photo_item(item, path, core::date_t(50), core::date_t(100));
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "a metadata-only write keeps the current visual across its new stamp");
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "the retained metadata-only version is adopted rather than reloading later");

	file_load_result written;
	written.success = true;
	const auto written_stamp = core::date_t(2026, 1, 5, 12, 0, 0);
	texture->publish_written_image(path, std::move(written), written_stamp);
	update_versioned_photo_item(item, path, written_stamp, {});
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "a written image adopted from the save is not read back again");
}

// MOD-017 - texture-budget eviction skips displayed entries and keeps looking for eligible offscreen
// entries rather than stopping at a displayed tail.
static void should_skip_displayed_textures_when_evicting_retained_texture_budget()
{
	const std::array entries{
		retained_texture_budget_entry{false, 10},
		retained_texture_budget_entry{false, 20},
		retained_texture_budget_entry{false, 30},
		retained_texture_budget_entry{true, 0},
	};

	const auto evicted = retained_texture_eviction_indexes(entries, 30);
	assert_equal(1_z, evicted.size(), "only enough offscreen entries are evicted");
	assert_equal(2_z, evicted.front(), "the oldest displayed tail is skipped for the next eligible entry");

	const std::array two_displayed{
		retained_texture_budget_entry{false, 10},
		retained_texture_budget_entry{true, 0},
		retained_texture_budget_entry{false, 30},
		retained_texture_budget_entry{true, 0},
	};

	const auto evicted_with_two_displayed = retained_texture_eviction_indexes(two_displayed, 10);
	assert_equal(1_z, evicted_with_two_displayed.size(), "displayed entries do not count against the budget");
	assert_equal(2_z, evicted_with_two_displayed.front(), "the active entries survive while offscreen cache shrinks");
}

struct texture_release_counts
{
	std::atomic_int worker = 0;
	int ui = 0;
};

static texture_state_ptr make_counted_texture(async_strategy& as, const df::item_element_ptr& item,
                                              texture_release_counts& counts)
{
	return texture_state_ptr(new texture_state(as, item), [&counts](texture_state* texture)
	{
		if (platform::is_ui_thread()) ++counts.ui;
		else ++counts.worker;
		delete texture;
	});
}

class media_preview_test_async final : public null_async_strategy
{
	std::deque<std::function<void()>> _ui;
	std::function<void(media_preview_state&)> _preview;

public:
	void queue_ui(const std::function<void()> f) override { _ui.emplace_back(f); }

	void queue_media_preview(std::function<void(media_preview_state&)> f, bool) override { _preview = std::move(f); }

	void drain_ui()
	{
		while (!_ui.empty())
		{
			auto f = std::move(_ui.front());
			_ui.pop_front();
			f();
		}
	}

	size_t pending_ui_count() const { return _ui.size(); }

	bool has_pending_preview() const { return static_cast<bool>(_preview); }

	void drop_preview_on_worker()
	{
		auto preview = std::exchange(_preview, {});
		std::thread worker([preview = std::move(preview)]() mutable {});
		worker.join();
	}

	void run_preview_on_worker()
	{
		auto preview = std::exchange(_preview, {});
		std::thread worker([preview = std::move(preview)]() mutable
		{
			media_preview_state state;
			preview(state);
		});
		worker.join();
	}
};

// MOD-018 - comparison preview carries UI-owned texture identity through success, stale rejection,
// and dropped worker-task exits, so final texture release is posted back to the UI queue.
static void should_release_comparison_preview_textures_on_ui_after_worker_exit()
{
	const auto path1 = core::file_path("c:\\compare-a.jpg");
	const auto path2 = core::file_path("c:\\compare-b.jpg");

	const auto run_case = [&](const bool run_preview, const bool keep_current_until_ui, const std::string_view label)
	{
		media_preview_test_async as;
		common_display_state_t common;
		auto display = std::make_shared<display_state_t>(as, common);
		texture_release_counts counts;

		auto texture1 = make_counted_texture(as, make_versioned_photo_item(path1, core::date_t(1), {}), counts);
		auto texture2 = make_counted_texture(as, make_versioned_photo_item(path2, core::date_t(1), {}), counts);
		display->_selected_texture1 = texture1;
		display->_selected_texture2 = texture2;
		display->calc_pixel_difference();

		texture1.reset();
		texture2.reset();

		if (!keep_current_until_ui)
		{
			display->_selected_texture1.reset();
			display->_selected_texture2.reset();
		}

		if (run_preview) as.run_preview_on_worker();
		else as.drop_preview_on_worker();

		assert_equal(0, counts.worker.load(), std::format("{}: worker releases no texture", label));
		assert_equal(true, as.pending_ui_count() >= (run_preview ? 1_z : 2_z),
		             std::format("{}: hand-back reaches UI queue", label));

		as.drain_ui();

		if (keep_current_until_ui)
		{
			assert_equal(0, counts.worker.load(),
			             std::format("{}: successful callback still releases no worker texture", label));
			assert_equal(0, counts.ui, std::format("{}: display still owns the successful textures", label));
			display->_selected_texture1.reset();
			display->_selected_texture2.reset();
		}

		assert_equal(0, counts.worker.load(), std::format("{}: final release is not on the worker", label));
		assert_equal(2, counts.ui, std::format("{}: both texture identities release on UI", label));
	};

	run_case(false, false, "early return");
	run_case(true, false, "stale rejection");
	run_case(true, true, "success");
}

// A hover queues a preview for each pointer move and the worker answers them one at a time, in order, so a
// frame for a position the pointer has since left is still the newest there is. Showing only the latest
// request's frame showed nothing at all while the pointer moved. Only the frame the pointer rests on is saved,
// a position already asked for is not asked for again, and no frame is shown for a file being renamed,
// replaced or deleted.
static void should_show_hover_frames_while_the_pointer_moves()
{
	media_preview_test_async as;
	null_state_strategy ss;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto path = test_files_folder().combine_file("tvp.mp4");
	df::index_file_item info;
	info.ft = files::file_type_from_name(path.name());
	info.file_modified = core::date_t(1);
	const auto item = std::make_shared<df::item_element>(path, info);

	s.load_hover_thumb(item, 10, 100);
	as.run_preview_on_worker();
	s.load_hover_thumb(item, 60, 100);
	as.drain_ui();

	assert_equal(true, item->has_thumb(), "the frame for a position the pointer has left is shown");
	assert_equal(true, item->has_cached_surface(), "drawn from the frame itself, not decoded again for staging");
	assert_equal(true, index.db_writes().dequeue_all().empty(), "but not saved, since the pointer moved on");
	const auto passed_time = item->thumbnail_surface()->time();

	as.run_preview_on_worker();
	as.drain_ui();
	assert_equal(true, item->thumbnail_surface()->time() > passed_time, "the frame the pointer rests on follows");
	assert_equal(1_z, index.db_writes().dequeue_all().size(), "and is saved");
	const auto rested_time = item->thumbnail_surface()->time();

	s.load_hover_thumb(item, 60, 100);
	assert_equal(false, as.has_pending_preview(), "moving along the tile's height does not ask for that frame again");

	s.load_hover_thumb(item, 90, 100);
	as.run_preview_on_worker();
	s.clear_hover_codec();
	as.drain_ui();
	assert_equal(true, core::equiv(item->thumbnail_surface()->time(), rested_time),
	             "a frame decoded before the file's handles were released is not shown");
	assert_equal(true, index.db_writes().dequeue_all().empty(), "nor saved");
}

static void should_select_settled_zoom_sampler()
{
	assert_equal(true,
	             calc_sampler({1000, 500}, {1000, 500}, core::orientation::top_left) == render::texture_sampler::point,
	             "one-to-one is exact");
	assert_equal(true,
	             calc_sampler({1003, 500}, {1000, 500}, core::orientation::top_left) ==
	                 render::texture_sampler::bicubic,
	             "just above one-to-one is settled smooth");
	assert_equal(true,
	             calc_sampler({1500, 750}, {1000, 500}, core::orientation::top_left) ==
	                 render::texture_sampler::bicubic,
	             "one and a half times is settled smooth");
	assert_equal(true,
	             calc_sampler({3000, 1500}, {1000, 500}, core::orientation::top_left) ==
	                 render::texture_sampler::bicubic,
	             "three times remains smooth");
	assert_equal(true,
	             calc_sampler({3001, 1500}, {1000, 500}, core::orientation::top_left) == render::texture_sampler::point,
	             "above three times keeps source pixels exact");
	assert_equal(true,
	             calc_sampler({3001, 1500}, {1000, 500}, core::orientation::top_left, false, true) ==
	                 render::texture_sampler::bicubic,
	             "a magnified stand-in stays smooth rather than blocky");
	assert_equal(true,
	             calc_sampler({1000, 500}, {1000, 500}, core::orientation::top_left, false, true) ==
	                 render::texture_sampler::point,
	             "a stand-in at one-to-one is still exact");
	assert_equal(true,
	             calc_sampler({1003, 500}, {1000, 500}, core::orientation::top_left, true) ==
	                 render::texture_sampler::bilinear,
	             "interactive magnification just above one-to-one uses fast sampler");
	assert_equal(true,
	             calc_sampler({1500, 750}, {1000, 500}, core::orientation::top_left, true) ==
	                 render::texture_sampler::bilinear,
	             "interactive magnification uses fast sampler");
	assert_equal(true,
	             calc_sampler({3001, 1500}, {1000, 500}, core::orientation::top_left, true) ==
	                 render::texture_sampler::point,
	             "interactive magnification above three times keeps source pixels exact");
}

// A video frame is drawn at the size it was decoded, so a picture smaller than it is minified by the sampler
// on every frame, where Catmull-Rom aliases as bilinear does at five times the texture reads. The picture's
// own extent decides, not the pane it is fitted into.
static void should_select_video_sampler_by_picture_size()
{
	assert_equal(true,
	             calc_video_sampler({960, 540}, {3840, 2160}, core::orientation::top_left) ==
	                 render::texture_sampler::bilinear,
	             "a 4K frame drawn at a quarter of its size is bilinear");
	assert_equal(true,
	             calc_video_sampler({1400, 787}, {1920, 1080}, core::orientation::top_left) ==
	                 render::texture_sampler::bilinear,
	             "below three quarters is bilinear");
	assert_equal(true,
	             calc_video_sampler({1500, 844}, {1920, 1080}, core::orientation::top_left) ==
	                 render::texture_sampler::bicubic,
	             "just below one-to-one keeps its edges");
	assert_equal(true,
	             calc_video_sampler({1920, 1080}, {1920, 1080}, core::orientation::top_left) ==
	                 render::texture_sampler::point,
	             "a picture at its decoded size is exact, whatever pane it is fitted into");
	assert_equal(true,
	             calc_video_sampler({2880, 1620}, {1920, 1080}, core::orientation::top_left) ==
	                 render::texture_sampler::bicubic,
	             "a magnified picture is smooth");
	assert_equal(true,
	             calc_video_sampler({2880, 1620}, {1920, 1080}, core::orientation::top_left, true) ==
	                 render::texture_sampler::bilinear,
	             "and fast while interacting");
}

static void should_accumulate_precision_wheel_deltas()
{
	// The frame boundary accumulates the raw platform delta, one detent being 120.
	auto pending = 0.0;
	assert_equal(0, platform::accumulate_wheel_steps(pending, 40.0), "first partial detent");
	assert_equal(0, platform::accumulate_wheel_steps(pending, 40.0), "second partial detent");
	assert_equal(0, platform::accumulate_wheel_steps(pending, 39.0), "detent not rounded early");
	assert_equal(1, platform::accumulate_wheel_steps(pending, 1.0), "exact detent steps once");
	assert_zoom_near(0.0, pending, "positive detent consumed exactly");
	assert_equal(-2, platform::accumulate_wheel_steps(pending, -240.0), "multiple reverse detents");
	assert_zoom_near(0.0, pending, "negative detents consumed exactly");

	// The scroll answer is quantised to whole logical units by the same carry, so a device reporting
	// a single raw unit at a time scrolls rather than halving to nothing on every event.
	auto scroll = 0.0;
	assert_equal(0, platform::accumulate_wheel_steps(scroll, 1.0 / 2.0, 1.0), "half a unit is carried, not dropped");
	assert_equal(1, platform::accumulate_wheel_steps(scroll, 1.0 / 2.0, 1.0), "two halves make a unit");
	assert_equal(60, platform::accumulate_wheel_steps(scroll, 120.0 / 2.0, 1.0),
	             "a whole detent scrolls its full amount");
}

static void should_validate_zoom_navigator_mode()
{
	assert_equal(true, is_valid_zoom_navigator_mode(static_cast<uint32_t>(zoom_navigator_mode::auto_hide)),
	             "auto-hide is valid");
	assert_equal(true, is_valid_zoom_navigator_mode(static_cast<uint32_t>(zoom_navigator_mode::pinned)),
	             "pinned is valid");
	assert_equal(true, is_valid_zoom_navigator_mode(static_cast<uint32_t>(zoom_navigator_mode::off)), "off is valid");
	assert_equal(false, is_valid_zoom_navigator_mode(3), "unknown mode is rejected");
}

static void should_keep_comparison_zoom_panes_matched()
{
	df::comparison_zoom_state zoom;
	zoom.mutate([](df::zoom_view_state& state) { state.set_explicit(2.0, {0.25, 0.75}); });
	assert_zoom_near(2.0, zoom.state(df::zoom_pane::primary).explicit_scale(), "primary scale changes");
	assert_zoom_near(2.0, zoom.state(df::zoom_pane::secondary).explicit_scale(), "secondary follows");
	assert_zoom_near(0.25, zoom.state(df::zoom_pane::secondary).center().X, "secondary adopts center");

	zoom.active(df::zoom_pane::secondary);
	assert_equal(true, zoom.active() == df::zoom_pane::secondary, "active pane switches");
	zoom.mutate([](df::zoom_view_state& state) { state.set_explicit(4.0, {0.75, 0.25}); });
	assert_zoom_near(4.0, zoom.state(df::zoom_pane::primary).explicit_scale(), "primary follows the active pane");
	assert_zoom_near(0.75, zoom.state(df::zoom_pane::primary).center().X, "primary adopts center");

	assert_equal(true, df::comparison_zoom_state::other(df::zoom_pane::primary) == df::zoom_pane::secondary,
	             "flip target from primary");
	assert_equal(true, df::comparison_zoom_state::other(df::zoom_pane::secondary) == df::zoom_pane::primary,
	             "flip target from secondary");
}

static void should_calculate_zoom_fit_without_enlarging()
{
	assert_zoom_near(0.5, df::zoom_view_state::fit_scale({4000, 2000}, {2000, 1200}), "large image fit");
	assert_zoom_near(1.0, df::zoom_view_state::fit_scale({200, 100}, {2000, 1200}), "small image fit");
	assert_zoom_near(10.0, df::zoom_view_state::fit_scale({200, 100}, {2000, 1200}, true), "fit and enlarge");
}

static void should_recalculate_zoom_fit_variants()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 3000};
	zoom.fit_width(source, {1000, 500});
	assert_equal(true, zoom.mode() == df::zoom_scale_mode::fit_width, "fit width mode");
	assert_zoom_near(0.25, zoom.effective_scale(0.125), "fit width scale");
	zoom.update_fit_variant(source, {2000, 500});
	assert_zoom_near(0.5, zoom.effective_scale(0.1666666667), "fit width follows viewport");

	zoom.fill(source, {1000, 1000});
	assert_equal(true, zoom.mode() == df::zoom_scale_mode::fill, "fill mode");
	assert_zoom_near(1.0 / 3.0, zoom.effective_scale(0.25), "fill covers viewport");
	const auto geometry = zoom.geometry(source, {1000, 1000}, 0.25);
	assert_zoom_near(1000.0, geometry.destination.Height, "fill height");
	assert_zoom_near(4000.0 / 3.0, geometry.destination.Width, "fill crops width");
}

// update_fit_variant runs on every layout - a resize, a panel opening, a sidebar toggle. It has to
// follow the viewport with the scale, but where the user panned to is theirs: recentring here threw
// a Fit width or Fill pan away every time the window changed shape. Choosing the command still
// recentres, because that is what the command says it does.
static void should_keep_the_pan_when_a_fit_variant_refits()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 3000};

	zoom.fit_width(source, {1000, 500});
	assert_zoom_near(0.5, zoom.center().X, "the command centres the picture");
	assert_zoom_near(0.5, zoom.center().Y, "on both axes");

	zoom.set_center({0.5, 0.8});
	assert_zoom_near(0.8, zoom.center().Y, "the user pans down");

	zoom.update_fit_variant(source, {2000, 500});
	assert_zoom_near(0.5, zoom.effective_scale(0.1666666667), "the scale follows the new viewport");
	assert_zoom_near(0.8, zoom.center().Y, "and the pan survives the relayout");

	// The command itself still recentres.
	zoom.fit_width(source, {2000, 500});
	assert_zoom_near(0.5, zoom.center().Y, "choosing fit width again recentres");

	// Fill behaves the same way.
	zoom.fill(source, {1000, 1000});
	zoom.set_center({0.3, 0.5});
	zoom.update_fit_variant(source, {1200, 1000});
	assert_zoom_near(0.3, zoom.center().X, "a fill pan survives a relayout too");
}

static void should_step_zoom_reversibly_through_fit()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 2000};
	constexpr sized viewport{1600, 1000};
	const auto fit = zoom.fit_scale(source, viewport);
	constexpr pointd anchor{800, 500};

	zoom.step(1, fit, source, viewport, anchor);
	assert_zoom_near(0.5, zoom.effective_scale(fit), "first step above inserted fit");
	zoom.step(1, fit, source, viewport, anchor);
	assert_zoom_near(0.67, zoom.effective_scale(fit), "second ladder step");
	zoom.step(-1, fit, source, viewport, anchor);
	zoom.step(-1, fit, source, viewport, anchor);
	assert_equal(true, zoom.is_fit(), "step out returns to fit");
	assert_zoom_near(fit, zoom.effective_scale(fit), "fit scale restored");
}

static void should_keep_zoom_anchor_still()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 3000};
	constexpr sized viewport{1000, 800};
	constexpr pointd anchor{750, 600};
	const auto fit = zoom.fit_scale(source, viewport);
	constexpr pointd old_center{0.5, 0.5};
	const pointd source_anchor{old_center.X * source.Width + (anchor.X - viewport.Width / 2.0) / fit,
	                           old_center.Y * source.Height + (anchor.Y - viewport.Height / 2.0) / fit};

	zoom.set_anchored(1.0, fit, source, viewport, anchor);
	const auto center = zoom.center();
	const pointd projected{viewport.Width / 2.0 + (source_anchor.X - center.X * source.Width),
	                       viewport.Height / 2.0 + (source_anchor.Y - center.Y * source.Height)};
	assert_zoom_near(anchor.X, projected.X, "anchor x");
	assert_zoom_near(anchor.Y, projected.Y, "anchor y");
}

static void should_step_zoom_at_pointer_anchor()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 3000};
	constexpr sized viewport{1000, 800};
	constexpr pointd anchor{750, 600};
	const auto fit = zoom.fit_scale(source, viewport);
	const pointd source_anchor{zoom.center().X * source.Width + (anchor.X - viewport.Width / 2.0) / fit,
	                           zoom.center().Y * source.Height + (anchor.Y - viewport.Height / 2.0) / fit};

	zoom.step(1, fit, source, viewport, anchor);
	const auto scale = zoom.effective_scale(fit);
	const auto center = zoom.center();
	const pointd projected{viewport.Width / 2.0 + (source_anchor.X - center.X * source.Width) * scale,
	                       viewport.Height / 2.0 + (source_anchor.Y - center.Y * source.Height) * scale};
	assert_zoom_near(anchor.X, projected.X, "stepped anchor x");
	assert_zoom_near(anchor.Y, projected.Y, "stepped anchor y");
}

static void should_preserve_zoom_center_across_dimensions()
{
	df::zoom_view_state zoom;
	zoom.set_explicit(4.0, {0.25, 0.75});
	const auto first = zoom.geometry({6000, 4000}, {1200, 800}, 0.2);
	const auto second = zoom.geometry({3000, 2000}, {1200, 800}, 0.4);

	assert_zoom_near(first.center.X, second.center.X, "normalized center x");
	assert_zoom_near(first.center.Y, second.center.Y, "normalized center y");
}

static void should_suspend_carried_zoom_below_fit()
{
	df::zoom_view_state zoom;
	zoom.update_source({6000, 4000}, 0.2);
	zoom.set_explicit(0.5, {0.25, 0.75});

	zoom.update_source({1000, 800}, 1.0);
	assert_equal(true, zoom.is_carried_fit(), "smaller image is temporarily fitted");
	assert_equal(false, zoom.is_magnified(1.0), "carried fit does not permit pan");
	assert_equal(true, zoom.mode() == df::zoom_scale_mode::explicit_scale, "carried fit remains durable Zoom mode");
	assert_zoom_near(1.0, zoom.effective_scale(1.0), "fit is displayed");
	assert_zoom_near(0.5, zoom.explicit_scale(), "carried scale is retained");

	zoom.update_source({6000, 4000}, 0.2);
	assert_equal(false, zoom.is_carried_fit(), "carried scale resumes on a large image");
	assert_zoom_near(0.5, zoom.effective_scale(0.2), "carried scale is restored");
	assert_zoom_near(0.25, zoom.center().X, "source center x is retained");
	assert_zoom_near(0.75, zoom.center().Y, "source center y is retained");
}

static void should_clamp_zoom_model_pan_to_edges()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 3000};
	constexpr sized viewport{1000, 800};
	zoom.set_explicit(2.0);
	zoom.pan_source({-source.Width, -source.Height}, source, viewport, 0.25);
	assert_zoom_near(viewport.Width / (2.0 * source.Width * 2.0), zoom.center().X,
	                 "stored source center clamps to the visible left edge");
	assert_zoom_near(viewport.Height / (2.0 * source.Height * 2.0), zoom.center().Y,
	                 "stored source center clamps to the visible top edge");
	const auto first = zoom.geometry(source, viewport, 0.25);
	assert_zoom_near(viewport.Width / (2.0 * source.Width * 2.0), first.center.X, "draw center clamps left");
	assert_zoom_near(viewport.Height / (2.0 * source.Height * 2.0), first.center.Y, "draw center clamps top");
	zoom.pan_source({1.0 / 2.0, 0.0}, source, viewport, 0.25);
	assert_zoom_near(first.center.X + 1.0 / (2.0 * source.Width), zoom.center().X,
	                 "one displayed pixel of reversal moves immediately");

	zoom.pan_source({source.Width * 2.0, source.Height * 2.0}, source, viewport, 0.25);
	assert_zoom_near(1.0 - viewport.Width / (2.0 * source.Width * 2.0), zoom.center().X,
	                 "stored source center clamps to the visible right edge");
	assert_zoom_near(1.0 - viewport.Height / (2.0 * source.Height * 2.0), zoom.center().Y,
	                 "stored source center clamps to the visible bottom edge");
	const auto second = zoom.geometry(source, viewport, 0.25);
	assert_zoom_near(1.0 - viewport.Width / (2.0 * source.Width * 2.0), second.center.X, "draw center clamps right");
	assert_zoom_near(1.0 - viewport.Height / (2.0 * source.Height * 2.0), second.center.Y, "draw center clamps bottom");

	df::zoom_view_state narrow;
	narrow.set_explicit(1.0);
	narrow.pan_source({source.Width, source.Height}, {100, 100}, viewport, 1.0);
	assert_zoom_near(0.5, narrow.center().X, "an axis smaller than the viewport remains centred");
	assert_zoom_near(0.5, narrow.center().Y, "both small axes remain centred");
}

static void should_toggle_fit_to_last_explicit_zoom()
{
	df::zoom_view_state zoom;
	zoom.set_explicit(4.0, {0.25, 0.75});
	zoom.toggle_fit();
	assert_equal(true, zoom.is_fit(), "first toggle fits");
	zoom.toggle_fit();
	assert_equal(false, zoom.is_fit(), "second toggle restores explicit mode");
	assert_zoom_near(4.0, zoom.explicit_scale(), "last explicit scale restored");
	assert_zoom_near(0.25, zoom.center().X, "last explicit center x restored");
	assert_zoom_near(0.75, zoom.center().Y, "last explicit center y restored");
}

// Toggling Fit and back returned a Fit width or Fill as the fixed scale it had last computed, so the
// restored view stopped following the window. The variant is what comes back, not its number.
static void should_toggle_fit_back_to_a_fit_variant()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 3000};

	zoom.fit_width(source, {1000, 500});
	zoom.toggle_fit();
	assert_equal(true, zoom.is_fit(), "the first toggle fits");

	zoom.toggle_fit();
	assert_equal(true, zoom.mode() == df::zoom_scale_mode::fit_width, "the second toggle restores fit width");

	zoom.update_fit_variant(source, {2000, 500});
	assert_zoom_near(0.5, zoom.effective_scale(0.1666666667), "which still follows the viewport");
}

// The five cells are whole divisions of the width, so a width that is not a multiple of five left a
// few pixels past the last cell, and the inclusive right edge sits past it too. Both divided out to a
// sixth star, which is a rating nothing can hold.
static void should_hit_test_ratings_within_five_stars()
{
	const recti bounds(0, 0, 104, 20);
	assert_equal(1, rating_control::to_rating(bounds, {0, 10}), "the left edge is one star");
	assert_equal(5, rating_control::to_rating(bounds, {99, 10}), "the last cell is five stars");
	assert_equal(5, rating_control::to_rating(bounds, {103, 10}), "and so are the pixels past it");
	assert_equal(5, rating_control::to_rating(bounds, {104, 10}), "and the inclusive right edge");
}

static void should_keep_zoom_anchor_through_layout_change()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 3000};
	constexpr sized old_viewport{1000, 800};
	constexpr sized new_viewport{1800, 1000};
	constexpr pointd old_origin{700, 100};
	constexpr pointd new_origin{100, 100};
	constexpr pointd old_anchor{750, 600};
	const auto absolute_anchor = old_origin + old_anchor;
	const auto old_fit = zoom.fit_scale(source, old_viewport);
	const auto source_anchor = zoom.source_point_at(source, old_viewport, old_fit, old_anchor);

	zoom.set_anchored(1.0, old_fit, source, old_viewport, old_anchor);
	zoom.center_source_point_at(source_anchor, source, new_viewport, zoom.fit_scale(source, new_viewport),
	                            absolute_anchor - new_origin);
	const auto scale = zoom.effective_scale(zoom.fit_scale(source, new_viewport));
	const auto center = zoom.center();
	const pointd projected{new_origin.X + new_viewport.Width / 2.0 +
	                           (source_anchor.X - center.X * source.Width) * scale,
	                       new_origin.Y + new_viewport.Height / 2.0 +
	                           (source_anchor.Y - center.Y * source.Height) * scale};
	assert_zoom_near(absolute_anchor.X, projected.X, "relayout anchor x");
	assert_zoom_near(absolute_anchor.Y, projected.Y, "relayout anchor y");
}

static void should_zoom_model_region_to_viewport()
{
	df::zoom_view_state zoom;
	constexpr sized source{4000, 3000};
	constexpr sized viewport{1000, 800};
	const auto fit = zoom.fit_scale(source, viewport);
	zoom.zoom_region({250, 200, 500, 400}, source, viewport, fit);

	assert_zoom_near(0.5, zoom.center().X, "region center x");
	assert_zoom_near(0.5, zoom.center().Y, "region center y");
	assert_zoom_near(0.5, zoom.effective_scale(fit), "region scale fills viewport");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// zoom.md: fitted to a browsing window a 12000x1000 stitch is a bright line. What qualifies for the
// look-around treatment is a wider question than what the file declares, and the shape half of it
// must not claim every wide screenshot - which is what the size floor is for.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_qualify_a_panorama_by_declaration_or_shape()
{
	const auto shaped = [](const int width, const int height)
	{
		prop::item_metadata md;
		md.width = static_cast<uint16_t>(width);
		md.height = static_cast<uint16_t>(height);
		return md;
	};

	// The declaration qualifies a file outright, whatever it looks like.
	auto declared = shaped(1024, 768);
	declared.panorama = prop::panorama_projection::equirectangular;
	assert_equal(true, declared.is_panorama(), "the file says so");
	assert_equal(true, declared.displays_as_panorama(), "so it is drawn as one");

	// A stitch that declares nothing still qualifies on its shape.
	assert_equal(false, shaped(12000, 1000).is_panorama(), "an undeclared stitch is not a declaration");
	assert_equal(true, shaped(12000, 1000).displays_as_panorama(), "but it is one to look at");
	assert_equal(true, shaped(1000, 12000).displays_as_panorama(), "and a vertical stitch is too");

	// A wide screenshot is exactly what aspect alone would have claimed. The size floor is the whole
	// reason it does not.
	assert_equal(false, shaped(3840, 1080).displays_as_panorama(), "an ultrawide screenshot is not a panorama");
	assert_equal(false, shaped(5000, 3000).displays_as_panorama(), "nor is a large ordinary photograph");

	// The boundary itself, both ways round, so a retune is a visible change rather than a silent one.
	assert_equal(true, shaped(4000, 2000).displays_as_panorama(), "2:1 at the floor qualifies");
	assert_equal(false, shaped(3999, 1999).displays_as_panorama(), "one pixel short of the floor does not");
	assert_equal(false, shaped(4000, 2001).displays_as_panorama(), "wide enough but not 2:1 does not");

	// Dimensions the scan never established say nothing either way.
	assert_equal(false, shaped(0, 0).displays_as_panorama(), "an unmeasured image claims nothing");
	assert_equal(false, shaped(12000, 0).displays_as_panorama(), "and neither does half a measurement");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// zoom.md: a file that declares equirectangular describes a sphere, so the viewer puts the user
// inside it. The camera is the whole model - which way it faces and how much it takes in - and
// these are the properties every other part of the feature is built on.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_look_around_inside_a_projected_panorama()
{
	// A complete sphere: the file is the whole panorama, so the camera opens facing its middle.
	const prop::panorama_geometry sphere{4000, 2000, 0, 0, 4000, 2000};
	assert_equal(true, sphere.is_valid(), "a full sphere is a valid declaration");
	assert_equal(true, panorama_wraps_longitude(sphere), "and it closes the circle");

	panorama_view view;
	view.reset(sphere);
	assert_equal(0.0, view.yaw(), "a full sphere opens facing its centre");
	assert_equal(0.0, view.pitch(), "and level");

	constexpr sized viewport{800.0, 600.0};
	double u = 0.0, v = 0.0;

	// The pixel under the middle of the viewport is the pixel the camera is pointed at. This is the
	// property that makes every other one meaningful: if the centre drifts, nothing else is aimed.
	assert_equal(true, view.texel_at(400.0, 300.0, viewport, sphere, u, v), "the centre samples the sphere");
	assert_equal(2000, core::round(u), "and lands on the middle column");
	assert_equal(1000, core::round(v), "and on the middle row");

	// Off centre it must not merely be "somewhere else": a pixel to the right of centre is east of
	// it, and one above is north. A symmetric check would pass for a mirrored projection too.
	double right_u = 0.0, right_v = 0.0;
	assert_equal(true, view.texel_at(700.0, 300.0, viewport, sphere, right_u, right_v), "so does one to its right");
	assert_equal(true, right_u > u, "which is further east");
	assert_equal(1000, core::round(right_v), "on the same row");

	double above_u = 0.0, above_v = 0.0;
	assert_equal(true, view.texel_at(400.0, 100.0, viewport, sphere, above_u, above_v), "and one above it");
	assert_equal(true, above_v < v, "which is further north");
	assert_equal(2000, core::round(above_u), "in the same column");

	// Crossing the viewport turns by exactly one field of view, which is what "grab the world and
	// drag it" means. Dragging right brings what was to the west into view, so the camera turns west.
	const auto start = view;
	view.drag({viewport.Width, 0.0}, viewport, start);
	assert_equal(core::round(-start.fov_degrees() * 1000.0), core::round(view.yaw() * 180.0 / M_PI * 1000.0),
	             "a drag of one viewport width turns by one field of view");
	assert_equal(0.0, view.pitch(), "and does not tilt");

	// The drag is recomputed from its origin, so re-applying the same total delta is idempotent -
	// a coalesced or dropped move message cannot change where it ends up.
	const auto after_first = view.yaw();
	view.drag({viewport.Width, 0.0}, viewport, start);
	assert_equal(core::round(after_first * 1e6), core::round(view.yaw() * 1e6), "and repeats to the same place");

	// A vertical drag turns by the same amount per pixel, and straight up and straight down are both
	// reachable without rolling past. Two viewport widths is 170 degrees of turn, so the clamp is
	// what stops it rather than the drag running out.
	view.drag({0.0, viewport.Width}, viewport, start);
	assert_equal(core::round(start.fov_degrees()), core::round(view.pitch() * 180.0 / M_PI),
	             "dragging down one width looks up by one field of view");
	view.drag({0.0, viewport.Width * 2.0}, viewport, start);
	assert_equal(90, core::round(view.pitch() * 180.0 / M_PI), "and further looks straight up, no more");
	view.drag({0.0, -viewport.Width * 2.0}, viewport, start);
	assert_equal(-90, core::round(view.pitch() * 180.0 / M_PI), "dragging up looks straight down, no more");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// zoom.md: inside a sphere there is no pixel to be one-to-one with, so the ladder is a field of
// view. Its wide end is the floor, the same shape as L2 stepping out to Fit.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_step_the_panorama_field_of_view_ladder()
{
	panorama_view view;
	assert_equal(core::round(panorama_default_fov_deg), core::round(view.fov_degrees()),
	             "a new view opens at the default");

	assert_equal(true, view.step_fov(1), "stepping in is a step");
	assert_equal(70, core::round(view.fov_degrees()), "and narrows to the next stop");
	assert_equal(true, view.step_fov(-1), "stepping out is a step");
	assert_equal(85, core::round(view.fov_degrees()), "and returns to the one it came from");

	// n steps in and n steps out returns exactly, as the zoom ladder's steps do (docs/zoom.md#scale-and-pan).
	for (auto i = 0; i < 4; ++i) view.step_fov(1);
	for (auto i = 0; i < 4; ++i) view.step_fov(-1);
	assert_equal(85, core::round(view.fov_degrees()), "four in and four out is where it started");

	// The narrow end holds rather than stepping past; the wide end reports that there is nowhere
	// wider, which is what makes the caller leave the projection instead of pretending.
	for (auto i = 0; i < 20; ++i) view.step_fov(1);
	assert_equal(core::round(panorama_min_fov_deg), core::round(view.fov_degrees()), "the narrow end holds");
	assert_equal(true, view.step_fov(1), "and reports a step it did not need to take");

	for (auto i = 0; i < 20; ++i) view.step_fov(-1);
	assert_equal(core::round(panorama_max_fov_deg), core::round(view.fov_degrees()), "the wide end is the floor");
	assert_equal(false, view.step_fov(-1), "and stepping out from it leaves the projection");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// zoom.md: under a held button the viewport maps the whole file and the pointer picks the direction.
// This is the gesture press-and-hold has always had, applied to a sphere rather than a rectangle.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_aim_a_projected_panorama_with_a_held_pointer()
{
	const prop::panorama_geometry sphere{4000, 2000, 0, 0, 4000, 2000};
	constexpr sized viewport{800.0, 600.0};

	panorama_view view;
	view.reset(sphere);

	// The middle of the viewport is the middle of the file, and the ends are its ends. A press
	// anywhere therefore looks at what was under the pointer, which is what makes the gesture
	// positional rather than relative.
	view.aim({400.0, 300.0}, viewport, sphere);
	assert_equal(0, core::round(view.yaw() * 180.0 / M_PI), "the middle aims at the middle");
	assert_equal(0, core::round(view.pitch() * 180.0 / M_PI), "and level");

	view.aim({800.0, 300.0}, viewport, sphere);
	assert_equal(180, std::abs(core::round(view.yaw() * 180.0 / M_PI)), "the right edge aims at the far side");

	view.aim({0.0, 300.0}, viewport, sphere);
	assert_equal(180, std::abs(core::round(view.yaw() * 180.0 / M_PI)), "and so does the left, from the other way");

	// Asymmetric probes: a quarter across is a quarter round, east of centre and not west.
	view.aim({600.0, 300.0}, viewport, sphere);
	assert_equal(90, core::round(view.yaw() * 180.0 / M_PI), "a quarter to the right is a quarter turn east");
	view.aim({200.0, 300.0}, viewport, sphere);
	assert_equal(-90, core::round(view.yaw() * 180.0 / M_PI), "and a quarter to the left is a quarter west");

	view.aim({400.0, 0.0}, viewport, sphere);
	assert_equal(90, core::round(view.pitch() * 180.0 / M_PI), "the top aims straight up");
	view.aim({400.0, 600.0}, viewport, sphere);
	assert_equal(-90, core::round(view.pitch() * 180.0 / M_PI), "and the bottom straight down");

	// A partial panorama maps its own coverage, not the whole sphere: crossing the viewport must
	// reach the ends of what the file holds and no further, or most of the gesture would aim at
	// nothing.
	const auto strip = prop::panorama_geometry::assumed({4000, 400});
	view.aim({400.0, 0.0}, viewport, strip);
	assert_equal(18, core::round(view.pitch() * 180.0 / M_PI), "the top of a strip aims at the top of the strip");
	view.aim({400.0, 600.0}, viewport, strip);
	assert_equal(-18, core::round(view.pitch() * 180.0 / M_PI), "and the bottom at its bottom");

	// Off the edge is clamped rather than wrapped: a pointer dragged outside the element must not
	// spin the view round.
	view.aim({-500.0, -500.0}, viewport, sphere);
	assert_equal(180, std::abs(core::round(view.yaw() * 180.0 / M_PI)), "a pointer off the left clamps to the edge");
	assert_equal(90, core::round(view.pitch() * 180.0 / M_PI), "and off the top clamps to straight up");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// metadata.md: a phone panorama is almost never a whole sphere. Treating a strip as one bends its
// horizon, which is the visible defect this arithmetic exists to prevent.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_place_a_partial_panorama_on_the_sphere()
{
	// 360 degrees round, 36 degrees tall, sitting on the horizon.
	const auto strip = prop::panorama_geometry::assumed({4000, 400});
	assert_equal(4000, strip.full_width, "the pixels span the full circle");
	assert_equal(2000, strip.full_height, "so the notional sphere is 2:1");
	assert_equal(800, strip.cropped_top, "and the strip sits on the horizon");
	assert_equal(18, core::round(panorama_latitude_at_top(strip) * 180.0 / M_PI), "18 north at its top");
	assert_equal(36, core::round(panorama_latitude_span(strip) * 180.0 / M_PI), "36 degrees of sky in all");
	assert_equal(0, core::round(panorama_center_latitude(strip) * 180.0 / M_PI), "centred on the horizon");

	// A true 2:1 sphere is the identity, so the assumption costs nothing where it is not needed.
	const auto whole = prop::panorama_geometry::assumed({4000, 2000});
	assert_equal(2000, whole.full_height, "a 2:1 image is taken as the whole sphere");
	assert_equal(0, whole.cropped_top, "with nothing missing above it");

	// Sphere the file does not hold is not sampled: it reads as absence, not as the nearest pixel.
	double u = 0.0, v = 0.0;
	assert_equal(false, panorama_texel_at(strip, 0.0, 60.0 * M_PI / 180.0, u, v), "60 north is not in the strip");
	assert_equal(true, panorama_texel_at(strip, 0.0, 0.0, u, v), "the horizon is");
	assert_equal(200, core::round(v), "half way down it");

	// A declaration that contradicts the pixels describes a different file, so it is refused rather
	// than drawn: this is what stops a stale or copied GPano block bending the picture.
	const prop::panorama_geometry lying{4000, 2000, 0, 800, 4000, 400};
	assert_equal(true, lying.is_valid(), "the declaration is self-consistent");
	assert_equal(400, prop::panorama_geometry::resolve(lying, {4000, 400}).cropped_height,
	             "and is kept when it matches the pixels");
	assert_equal(2000, prop::panorama_geometry::resolve(lying, {4000, 2000}).cropped_height,
	             "but replaced when it does not");

	// A crop that runs off the full panorama is not a declaration at all.
	const prop::panorama_geometry impossible{4000, 2000, 3900, 0, 400, 2000};
	assert_equal(false, impossible.is_valid(), "a crop past the edge declares nothing");

	// A file that closes the circle covers all of it. Writers round these numbers, so one a pixel
	// short of the full turn is still seamless - and it has to be *covered* to the full turn, not
	// merely blended across, or the join shows a hairline of background instead of a picture.
	const prop::panorama_geometry rounded{4000, 2000, 0, 800, 3999, 400};
	assert_equal(true, panorama_wraps_longitude(rounded), "a pixel short of the turn still closes it");

	constexpr sized square{600.0, 600.0};
	panorama_view seam_view;
	seam_view.reset(rounded);

	const auto wrapped = panorama_shader_params(rounded, seam_view, square);
	assert_equal(1.0f, wrapped.u_scale, "so its coverage is the whole turn");

	// And a genuinely partial file is still scaled by its own crop, or half a sphere would be
	// stretched across all of it.
	const auto strip_half = prop::panorama_geometry{4000, 2000, 0, 800, 2000, 400};
	assert_equal(false, panorama_wraps_longitude(strip_half), "half a turn does not close the circle");
	assert_equal(2.0f, panorama_shader_params(strip_half, seam_view, square).u_scale,
	             "and it covers half of what a full file would");
}

static void should_refresh_same_path_panorama_declaration_without_resetting_view()
{
	null_async_strategy as;
	common_display_state_t common;
	display_state_t display(as, common);
	const core::file_path path("c:\\same-path-panorama.jpg");
	const core::file_path other_path("c:\\other-panorama.jpg");
	const prop::panorama_geometry old_decl{4000, 2000, 0, 0, 4000, 2000};
	const prop::panorama_geometry stale_decl{4000, 2000, 100, 0, 2000, 1000};
	const prop::panorama_geometry new_decl{8000, 5000, 0, 500, 8000, 4000};
	const prop::panorama_geometry same_size_decl{8000, 5000, 0, 750, 8000, 4000};

	display.panorama_item(path, {4000, 2000}, core::date_t(1));
	display.panorama_geometry(path, old_decl, {4000, 2000}, core::date_t(1));
	assert_equal(true, common._panorama.resolved, "the first declaration resolves");

	const auto start = common._panorama.view;
	common._panorama.view.drag({400, 0}, {800, 600}, start);
	const auto yaw = common._panorama.view.yaw();
	common._panorama.flat = true;

	display.panorama_item(path, {8000, 4000}, core::date_t(2));
	assert_equal(false, common._panorama.resolved, "changed same-path dimensions need a new declaration");
	assert_equal(true, common._panorama.flat, "the user's flat choice is retained for the same path");
	assert_zoom_near(yaw, common._panorama.view.yaw(), "the camera is retained for the same path");

	display.panorama_geometry(path, stale_decl, {4000, 2000}, core::date_t(1));
	assert_equal(false, common._panorama.resolved, "a stale declaration for the old source cannot win");

	display.panorama_geometry(path, new_decl, {8000, 4000}, core::date_t(2));
	assert_equal(true, common._panorama.resolved, "the latest declaration resolves");
	assert_equal(new_decl.cropped_top, common._panorama.geometry.cropped_top, "the latest crop is applied");
	assert_equal(true, common._panorama.flat, "the declaration does not reset the flat choice");
	assert_zoom_near(yaw, common._panorama.view.yaw(), "the declaration does not reset the camera");

	display.panorama_item(path, {8000, 4000}, core::date_t(3));
	assert_equal(false, common._panorama.resolved, "same-size source revision needs a new declaration");
	display.panorama_geometry(path, same_size_decl, {8000, 4000}, core::date_t(3));
	assert_equal(same_size_decl.cropped_top, common._panorama.geometry.cropped_top,
	             "same-size declaration edits are applied");
	assert_zoom_near(yaw, common._panorama.view.yaw(), "same-path declaration edits still keep the camera");

	display.panorama_item(other_path, {8000, 4000}, core::date_t(4));
	common._panorama.view.drag({400, 0}, {800, 600}, common._panorama.view);
	display.panorama_geometry(other_path, same_size_decl, {8000, 4000}, core::date_t(4));
	assert_zoom_near(0.0, common._panorama.view.yaw(), "a new panorama recentres when its declaration arrives");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// design.md: a rectangle drawn on the picture belongs to the picture, not to the window. Held in
// source space it survives a resize, a layout change and a zoom; held in client space it would slide
// off the subject the moment any of the three happened, which would be worse than having none.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_hold_a_drawn_region_in_source_space()
{
	constexpr sized source{4000, 3000};

	// The picture fitted into a small window, and the same picture after the window was resized and
	// the layout moved it. Nothing about the rectangle the user drew has changed.
	constexpr rectd fitted{100, 50, 400, 300};
	constexpr rectd resized{0, 120, 1200, 900};

	const auto drawn = df::client_rect_to_source({200, 125, 100, 75}, fitted, source);

	assert_zoom_near(1000.0, drawn.X, "region left in source pixels");
	assert_zoom_near(750.0, drawn.Y, "region top in source pixels");
	assert_zoom_near(1000.0, drawn.Width, "region width in source pixels");
	assert_zoom_near(750.0, drawn.Height, "region height in source pixels");

	// Round trip through the layout it was drawn in.
	const auto back = df::source_rect_to_client(drawn, fitted, source);
	assert_zoom_near(200.0, back.X, "round trip left");
	assert_zoom_near(125.0, back.Y, "round trip top");
	assert_zoom_near(100.0, back.Width, "round trip width");
	assert_zoom_near(75.0, back.Height, "round trip height");

	// The layout changes and the rectangle still covers the same quarter of the same picture, which
	// is the whole point of storing it in source space.
	const auto after = df::source_rect_to_client(drawn, resized, source);
	assert_zoom_near(300.0, after.X, "the same pixels after a resize");
	assert_zoom_near(345.0, after.Y, "the same pixels after a resize");
	assert_zoom_near(300.0, after.Width, "the same pixels after a resize");
	assert_zoom_near(225.0, after.Height, "the same pixels after a resize");

	// A drag runs in whichever direction the pointer went, and it is clamped to the picture rather
	// than naming pixels that are not there.
	const auto backwards = df::client_rect_to_source({300, 200, -100, -75}, fitted, source);
	assert_zoom_near(1000.0, backwards.X, "a backwards drag is still a rectangle");
	assert_zoom_near(1000.0, backwards.Width, "a backwards drag is still a rectangle");

	const auto overhanging = df::client_rect_to_source({50, 25, 600, 450}, fitted, source);
	assert_zoom_near(0.0, overhanging.X, "clamped to the left edge");
	assert_zoom_near(0.0, overhanging.Y, "clamped to the top edge");
	assert_zoom_near(source.Width, overhanging.Width, "clamped to the right edge");
	assert_zoom_near(source.Height, overhanging.Height, "clamped to the bottom edge");

	// Moving it stops at the edge rather than carrying it off the picture.
	const auto moved = df::offset_source_rect(drawn, {5000, 5000}, source);
	assert_zoom_near(3000.0, moved.X, "a move stops at the right edge");
	assert_zoom_near(2250.0, moved.Y, "a move stops at the bottom edge");
	assert_zoom_near(drawn.Width, moved.Width, "a move does not resize it");
	assert_zoom_near(drawn.Height, moved.Height, "a move does not resize it");

	const auto moved_back = df::offset_source_rect(drawn, {-5000, -5000}, source);
	assert_zoom_near(0.0, moved_back.X, "and at the left edge");
	assert_zoom_near(0.0, moved_back.Y, "and at the top edge");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The destination a pane is laid out for is shaped by what the item is; the surface drawn into it is
// whatever has arrived so far. A stand-in staged before the decode need not be that shape - an
// embedded thumbnail is stored verbatim and cameras routinely write a padded 4:3 for a 3:2 frame -
// and stretching it distorted the picture until the decode landed. That read as a compare-mode
// defect because the carried pane is the one with a picture to distort, but nothing here is about
// comparing, so nothing here may be made compare-specific.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_fit_a_stand_in_rather_than_stretch_it()
{
	// The pane, laid out from the indexed dimensions of a 3:2 photograph.
	constexpr rectd pane{100, 50, 900, 600};

	// The real decode has the shape the pane was laid out for, so nothing moves. A fit that
	// letterboxed here would put bars around every correctly shaped image ever drawn.
	assert_equal(true, df::fit_preserving_aspect(pane, {3000, 2000}) == pane, "the real image fills its pane");

	// A scaled-down decode is the same shape, and integer division makes it a fraction off. One
	// device pixel of disagreement is rounding, not a different shape.
	assert_equal(true, df::fit_preserving_aspect(pane, {1000, 667}) == pane, "a rounded decode still fills it");

	// The padded embedded thumbnail. Stretched it was a 12 per cent distortion; fitted it keeps the
	// subject's shape and gives back the height it does not fill.
	const auto stand_in = df::fit_preserving_aspect(pane, {160, 120});
	assert_zoom_near(800.0, stand_in.Width, "the stand-in keeps its own aspect");
	assert_zoom_near(600.0, stand_in.Height, "and takes the height it can");
	assert_zoom_near(150.0, stand_in.X, "centred in the pane it was given");
	assert_zoom_near(50.0, stand_in.Y, "centred in the pane it was given");
	assert_equal(true, stand_in.Width / stand_in.Height > 1.3 && stand_in.Width / stand_in.Height < 1.34,
	             "which is 4:3, not the 3:2 the pane was laid out for");

	// The other way round: a stand-in taller than the destination is bounded by the width.
	const auto tall = df::fit_preserving_aspect(pane, {600, 800});
	assert_zoom_near(450.0, tall.Width, "a tall stand-in is bounded by the height");
	assert_zoom_near(600.0, tall.Height, "a tall stand-in is bounded by the height");
	assert_zoom_near(325.0, tall.X, "and is centred");

	// Nothing to fit says nothing about the destination, so the destination is what is used.
	assert_equal(true, df::fit_preserving_aspect(pane, {}) == pane, "an unmeasured surface changes nothing");
	assert_equal(true, df::fit_preserving_aspect({}, {160, 120}).is_empty(), "an empty pane draws nothing");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// A crop quad's positions are stored pixels; its corner order carries the orientation. A region the
// user drew is in the space the picture was drawn in, which with show_rotated on is the upright one.
// Permuting the corners reorders points and cannot move them between two spaces, so a rotated
// capture would crop pixels the user never selected - and Edit's preview would be the first place
// anyone found out.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_map_a_drawn_region_onto_stored_pixels()
{
	// A portrait phone photograph: stored 4000x3000 landscape, displayed 3000x4000 upright.
	static constexpr sizei stored{4000, 3000};

	const auto mapped = [](const rectd normalised, const core::orientation o, const bool was_rotated)
	{
		return edit_view_state::crop_from_displayed_rect(normalised, stored, o, was_rotated).bounding_rect();
	};

	const auto assert_rect = [](const rectd actual, const double x, const double y, const double w, const double h,
	                            const std::string_view message)
	{
		assert_zoom_near(x, actual.X, message);
		assert_zoom_near(y, actual.Y, message);
		assert_zoom_near(w, actual.Width, message);
		assert_zoom_near(h, actual.Height, message);
	};

	// The unrotated case must be left exactly where it was, or every ordinary photograph moves.
	assert_rect(mapped({0.25, 0.5, 0.25, 0.25}, core::orientation::top_left, true), 1000, 1500, 1000, 750,
	            "an unrotated capture is not moved");

	// With show_rotated off the picture is drawn in its stored orientation, so the fractions are
	// already fractions of the stored picture and only the corner order carries the rotation.
	assert_rect(mapped({0.25, 0.5, 0.25, 0.25}, core::orientation::right_top, false), 1000, 1500, 1000, 750,
	            "already stored space when the picture is not turned");

	// One asymmetric probe per orientation, mapped by hand from the EXIF definition. A symmetric
	// probe would pass under any orientation's formula, so it would pin nothing: the quarter chosen
	// here lands in a different corner of the stored picture for every one of the seven.
	// Displayed rect is the top-left quarter-ish block {0.0, 0.0, 0.25, 0.5} of the displayed extent.
	struct probe
	{
		core::orientation orientation;
		double x, y, w, h;
	};

	// Non-flipping: displayed extent is 4000x3000, so the block is 1000x1500 at the displayed origin.
	constexpr probe upright_probes[]{
		{core::orientation::top_right, 3000, 0, 1000, 1500},       // mirrored left to right
		{core::orientation::bottom_right, 3000, 1500, 1000, 1500}, // turned half way round
		{core::orientation::bottom_left, 0, 1500, 1000, 1500},     // mirrored top to bottom
	};

	for (const auto& p : upright_probes)
	{
		assert_rect(mapped({0.0, 0.0, 0.25, 0.5}, p.orientation, true), p.x, p.y, p.w, p.h,
		            "the displayed top-left block lands where the orientation says");
	}

	// Flipping: displayed extent is 3000x4000, so the block is 750x2000 and the axes swap on the way
	// back, giving a 2000x750 rectangle in stored space.
	constexpr probe turned_probes[]{
		{core::orientation::left_top, 0, 0, 2000, 750},           // transposed about the main diagonal
		{core::orientation::right_top, 0, 2250, 2000, 750},       // rotated a quarter turn
		{core::orientation::right_bottom, 2000, 2250, 2000, 750}, // transposed about the anti-diagonal
		{core::orientation::left_bottom, 2000, 0, 2000, 750},     // rotated a quarter turn the other way
	};

	for (const auto& p : turned_probes)
	{
		assert_rect(mapped({0.0, 0.0, 0.25, 0.5}, p.orientation, true), p.x, p.y, p.w, p.h,
		            "the displayed top-left block lands where the orientation says");
	}

	// Whatever the orientation, the whole displayed picture is the whole stored picture. That is the
	// invariant every one of the seven maps has to satisfy on top of landing in the right corner.
	for (const auto& [o, x, y, w, h] : upright_probes)
	{
		assert_rect(mapped({0, 0, 1, 1}, o, true), 0, 0, stored.cx, stored.cy, "all of it is all of it");
	}

	for (const auto& [o, x, y, w, h] : turned_probes)
	{
		assert_rect(mapped({0, 0, 1, 1}, o, true), 0, 0, stored.cx, stored.cy, "all of it is all of it");
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// zoom.md: looking around a panorama moves what is centred and nothing else. No button was pressed,
// so the sweep must not answer the question of scale on the user's behalf: setting an explicit scale
// would silently discard a chosen Fit width or Fill, which then stops re-fitting on every later
// resize.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_look_around_without_choosing_a_scale()
{
	constexpr sized wide{12000, 1000};
	constexpr sized viewport{1600, 900};

	// Crossing the viewport sweeps the whole width, and the ends are the ends.
	const auto middle = df::zoom_view_state::look_around_center({800, 450}, wide, viewport, 1.0);
	assert_zoom_near(0.5, middle.X, "the middle of the viewport is the middle of the picture");

	const auto right = df::zoom_view_state::look_around_center({1600, 450}, wide, viewport, 1.0);
	assert_zoom_near(1.0, right.X, "the right edge reaches the right end");

	const auto past = df::zoom_view_state::look_around_center({2400, -100}, wide, viewport, 1.0);
	assert_zoom_near(1.0, past.X, "and a pointer past it goes no further");

	// Scale decides the vertical axis, not the stored size. The same strip is 1000 tall at 100% in a
	// 900 viewport, so the pointer drives it; fitted to half that there is nothing to see above or
	// below, and following the pointer would only be jitter.
	assert_zoom_near(0.0, df::zoom_view_state::look_around_center({800, 0}, wide, viewport, 1.0).Y,
	                 "a strip taller than the viewport follows the pointer up");
	assert_zoom_near(1.0, df::zoom_view_state::look_around_center({800, 900}, wide, viewport, 1.0).Y, "and down");
	assert_zoom_near(0.5, df::zoom_view_state::look_around_center({800, 0}, wide, viewport, 0.5).Y,
	                 "the same strip scaled to fit is pinned");
	assert_zoom_near(0.5, df::zoom_view_state::look_around_center({800, 900}, wide, viewport, 0.5).Y,
	                 "wherever the pointer is");

	// And the sweep itself never chooses a scale, or a chosen Fit width stops re-fitting on resize.
	df::zoom_view_state state;
	state.fit_width(wide, viewport);
	const auto fit_width_scale = state.explicit_scale();

	state.set_center({0.75, 0.5});
	assert_equal(true, state.mode() == df::zoom_scale_mode::fit_width, "a sweep does not choose a scale");
	assert_zoom_near(fit_width_scale, state.explicit_scale(), "and does not change the one in force");
	assert_zoom_near(0.75, state.center().X, "but it does move the centre");

	state.set_explicit(1.0, {0.5, 0.5});
	state.set_center({1.4, -0.2});
	assert_equal(true, state.mode() == df::zoom_scale_mode::explicit_scale, "an explicit scale stays explicit");
	assert_zoom_near(1.0, state.explicit_scale(), "at the scale it already had");
	assert_zoom_near(1.0, state.center().X, "with the centre clamped to the end of the picture");
	assert_zoom_near(0.0, state.center().Y, "and to the top");
}

static void should_accelerate_pan_from_drag_origin()
{
	const auto accelerated = df::zoom_view_state::accelerate_pan({3.0, 4.0}, 10.0);
	assert_zoom_near(4.5, accelerated.X, "accelerated pan x");
	assert_zoom_near(6.0, accelerated.Y, "accelerated pan y");

	const auto dpi_scaled = df::zoom_view_state::accelerate_pan({3.0, 4.0}, 20.0);
	assert_zoom_near(3.75, dpi_scaled.X, "DPI-scaled ramp x");
	assert_zoom_near(5.0, dpi_scaled.Y, "DPI-scaled ramp y");
}

static void should_bound_auto_pan_velocity()
{
	const auto stopped = df::zoom_view_state::auto_pan_velocity({6.0, 8.0});
	assert_zoom_near(0.0, stopped.X, "auto-pan dead zone x");
	assert_zoom_near(0.0, stopped.Y, "auto-pan dead zone y");
	const auto moving = df::zoom_view_state::auto_pan_velocity({15.0, 20.0});
	assert_zoom_near(62.4, moving.X, "auto-pan velocity x");
	assert_zoom_near(83.2, moving.Y, "auto-pan velocity y");
	const auto capped = df::zoom_view_state::auto_pan_velocity({300.0, 400.0});
	assert_zoom_near(720.0, capped.X, "auto-pan capped x");
	assert_zoom_near(960.0, capped.Y, "auto-pan capped y");
}

static void should_map_zoom_navigator_to_source_center()
{
	const auto center = df::zoom_view_state::navigator_center({50.0, 25.0}, {200.0, 100.0});
	assert_zoom_near(0.25, center.X, "navigator center x");
	assert_zoom_near(0.25, center.Y, "navigator center y");

	const auto clamped = df::zoom_view_state::navigator_center({-10.0, 120.0}, {200.0, 100.0});
	assert_zoom_near(0.0, clamped.X, "navigator clamps left");
	assert_zoom_near(1.0, clamped.Y, "navigator clamps bottom");
}

static void should_orient_selector_thumbnails()
{
	constexpr recti image_bounds(0, 0, 100, 100);
	const auto rotated = selector_view::thumbnail_destination({4, 3}, image_bounds, core::orientation::right_top, true);
	const auto unrotated = selector_view::thumbnail_destination({4, 3}, image_bounds, core::orientation::right_top,
	                                                            false);

	assert_equal(90.0, rotated.angle(), "selector applies right-top orientation");
	assert_equal(true, rotated.bounding_rect().extent().round() == sizei(75, 100),
	             "oriented selector thumbnail fits as portrait");
	assert_equal(0.0, unrotated.angle(), "selector preserves orientation when rotation is disabled");
	assert_equal(true, unrotated.bounding_rect().extent().round() == sizei(100, 75),
	             "unrotated selector thumbnail fits as landscape");
}

static bool is_file_item(const df::item_element_ptr& i)
{
	return !i->is_folder();
}

static void should_range_select_across_selector()
{
	browsing_fixture f;
	const auto shown = f.files();
	df::item_element_ptr hidden;

	for (const auto& group : f.state.groups())
	{
		for (const auto& i : group->items())
		{
			if (!is_file_item(i)) hidden = i;
		}
	}

	assert_equal(true, shown.size() > 4, "test folder has enough items for a range");

	const auto selector = std::make_shared<selector_view>(f.state, nullptr, nullptr);
	selector->filter(is_file_item);
	selector->activate({0, 0});

	selector->selection_anchor(shown[1]);
	const auto forward = selector->selection_range(shown[4]);
	assert_equal(4_z, forward.size(), "range spans the anchor to the clicked item");
	assert_equal(true, forward.front() == shown[4], "clicked item leads so focus follows the pointer");

	// A second shift click measures from the same anchor rather than pivoting on the previous click.
	const auto shortened = selector->selection_range(shown[2]);
	assert_equal(2_z, shortened.size(), "a shift click does not move the anchor");
	assert_equal(true, shortened.front() == shown[2], "clicked item still leads");

	const auto backward = selector->selection_range(shown[0]);
	assert_equal(2_z, backward.size(), "range is measured backwards from the anchor");
	assert_equal(true, backward.front() == shown[0], "clicked item leads when the range runs backwards");

	// An item the strip filtered out cannot be reached, so a task is never handed one it cannot write.
	if (hidden) assert_equal(0_z, selector->selection_range(hidden).size(), "filtered items are not selectable");

	// A strip that is not on screen holds no items, so it neither claims item visibility nor queues
	// thumbnails for the view that is.
	selector->deactivate();
	assert_equal(0_z, selector->selection_range(shown[0]).size(), "an inactive strip selects nothing");
}

static void should_reuse_selector_entries_with_one_lookup()
{
	constexpr auto retained_count = 512_z;
	const auto stats = selector_view::test_rebuild_reuse(retained_count);

	assert_equal(retained_count, stats.lookup_entries, "each old entry is indexed once");
	assert_equal(retained_count + 1, stats.probes, "each incoming item is probed once");
	assert_equal(retained_count, stats.reused, "all retained entries are reused");
	assert_equal(retained_count, stats.preserved_markers, "reused entries keep their per-entry state");
}

static void should_bound_selector_retained_resources_to_visible_band()
{
	constexpr auto item_count = 100_z;
	constexpr sizei extent{100, 40};
	constexpr auto item_width = 10;
	constexpr auto scroll_x = 500;
	constexpr auto max_band_entries = static_cast<size_t>(extent.cx) * 2 / item_width + 4;

	const auto stats = selector_view::test_resource_retirement(item_count, extent, item_width, scroll_x);

	assert_equal(item_count, stats.before, "the test starts with one retained resource per visited entry");
	assert_equal(true, stats.after_retire <= max_band_entries, "scrolling retires resources outside the band");
	assert_equal(stats.after_retire, stats.after_stale_publish, "an off-band completion is rejected");
	assert_equal(true, stats.after_reentry_publish <= max_band_entries,
	             "re-entering keeps retention bounded to the new band");
	assert_equal(true, stats.after_reentry_publish > 0_z, "the re-entered item can publish again");
}

// Selecting an item the browser has already drawn must fill the panel from what the item already
// holds - its staged thumbnail surface and its indexed dimensions - rather than decoding again. No
// draw runs here, and nothing else in the selection path produces a surface, so a panel that has
// something to show can only have taken it from the item on the selecting thread.
static void should_show_selected_thumbnail_without_waiting()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(test_files_folder()), {});
	s.update_item_groups();
	s.update_selection();

	df::item_elements photos;

	for (const auto& group : s.groups())
	{
		for (const auto& i : group->items())
		{
			if (!i->is_folder() && i->file_type()->has_trait(file_traits::bitmap)) photos.emplace_back(i);
		}
	}

	assert_equal(true, photos.size() >= 2_z, "test folder has photos to step between");

	// What the browser leaves on an item whose tile is on screen.
	const auto& target = photos.front();
	index.scan_item(target, true, false);
	target->stage_thumbnail_surface(as);

	assert_equal(true, target->has_thumb(), "the tile has its encoded thumbnail");
	assert_equal(true, target->has_cached_surface(), "the tile has its decoded thumbnail surface");
	assert_equal(true, target->metadata() != nullptr, "the tile has its indexed metadata");

	s.select(view, target, false, false, false);
	s.update_selection();

	const auto d = s.display_state();

	assert_equal(true, d && d->_selected_texture1 != nullptr, "selecting built a display texture");
	assert_equal(true, d->_selected_texture1->has_visual(), "the panel can draw the tile's thumbnail at once");
	assert_equal(false, d->_selected_texture1->display_dimensions().is_empty(),
	             "the panel knows the image's shape at once");
	assert_equal(true, d->_selected_texture1->is_provisional(), "what it draws is still marked provisional");
}

// Issue #78 - a decoded video frame can carry non-square pixels, so the stored frame is not the
// shape the media box should be filled at. The reporter's file stores 528x560 and declares 4:3:
// fitting the destination to the texture letterboxed a correctly shaped box with a squeezed
// picture, which is why the thumbnail (whose aspect is baked into the bitmap) looked right while
// playback did not. A photo keeps its texture's shape, because a stand-in staged before the decode
// may not match the item's declared dimensions yet.
static void should_shape_video_by_its_display_dimensions()
{
	constexpr sizei stored{528, 560};   // what the frame decodes to
	constexpr sizei declared{528, 396}; // 4:3, what the container declares

	const auto shape = [](const sizei tex, const sizei display, const bool is_video)
	{
		const auto r = calc_draw_shape(tex, display, is_video);
		return std::format("{}x{}", r.cx, r.cy);
	};

	assert_equal("528x396"s, shape(stored, declared, true), "video takes the declared shape");
	assert_equal("528x560"s, shape(stored, declared, false), "a photo keeps its texture's shape");

	// Before the container's dimensions are known there is nothing better than the texture, and a
	// zero shape would collapse the destination rather than letterbox it.
	assert_equal("528x560"s, shape(stored, {}, true), "unknown display dimensions fall back");
	assert_equal("0x0"s, shape({}, declared, true), "an empty texture stays empty");

	// Square pixels are the common case and must be left exactly alone.
	assert_equal("1920x1080"s, shape(sizei{1920, 1080}, sizei{1920, 1080}, true), "square-pixel video is unchanged");
}

static void should_stage_neighbour_stand_ins()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::media);
	s.open(view, df::search_t().add_selector(test_files_folder()), {});
	s.update_item_groups();
	s.update_selection();

	df::item_elements ordered;

	for (const auto& group : s.groups())
	{
		for (const auto& i : group->items()) ordered.emplace_back(i);
	}

	size_t middle = 0;

	for (size_t n = 1; n + 1 < ordered.size(); ++n)
	{
		if (!ordered[n - 1]->is_folder() && !ordered[n]->is_folder() && !ordered[n + 1]->is_folder())
		{
			middle = n;
			break;
		}
	}

	assert_equal(true, middle != 0_z, "test folder has a file with file neighbours");

	// Nothing here has a thumbnail: the browser's batched database query is what normally supplies
	// them, and there is no browser in this view. Retiring the query leaves the display facing the
	// case that matters - an item with no encoded thumbnail to stand in for it.
	for (const auto& i : ordered) i->begin_db_thumbnail_query();

	s.select(view, ordered[middle], false, false, false);
	s.update_selection();

	const auto previous = s.next_item(false, false);
	const auto next = s.next_item(true, false);

	assert_equal(true, previous && next, "there are items to reach either side");
	assert_equal(true, previous->has_thumb(), "the previous item's thumbnail was requested");
	assert_equal(true, next->has_thumb(), "the next item's thumbnail was requested");
	assert_equal(true, previous->has_cached_surface(), "the previous item's stand-in is staged");
	assert_equal(true, next->has_cached_surface(), "the next item's stand-in is staged");
}

// The items preview fills in as the file is read: the metadata blocks, and any property whose value
// was not in the index, are emitted long after the panel is laid out. Once the column is taller than
// the pane, none of that may move or resize what is already on screen - which is the shape of every
// jump reported against 1.27.1, where the media pane was the column's shrink target so each late row
// was paid for by shrinking the picture. While the column still fits, the composition stays centred,
// so a late row moves the media by half its own height and no more.
static void should_hold_media_column_still_as_detail_arrives()
{
	flex_test_measure_context mc;

	const auto media = std::make_shared<flex_test_element>(sizei{200, 400});
	media->flex = media->flex | flex_item::media;

	const std::vector<view_element_ptr> priority{media, std::make_shared<flex_test_element>(sizei{200, 20})};
	const auto& all = priority;

	constexpr recti pane{0, 0, 200, 600};

	const auto arrange = [&](const std::vector<view_element_ptr>& detail)
	{
		const media_column_inputs in{&priority, &detail, &all, pane, media};
		return layout_media_column(in, mc);
	};

	// Enough detail to take the column past the pane, which is where a late row could have cost the
	// picture its height.
	const auto row = []
	{
		return std::make_shared<flex_test_element>(sizei{200, 300});
	};
	const std::vector<view_element_ptr> many_rows{row(), row()};
	const auto height_with_many = arrange(many_rows);
	const auto overflowing_bounds = media->bounds;

	assert_equal(pane.top, overflowing_bounds.top, "an overflowing column starts at the top of the pane");
	assert_equal(400, overflowing_bounds.height(), "the media keeps its own height");

	const std::vector<view_element_ptr> more_rows{row(), row(), row()};
	const auto height_with_more = arrange(more_rows);

	assert_equal(true, media->bounds == overflowing_bounds, "detail past the pane height never moves the media");
	assert_equal(true, height_with_more > height_with_many, "detail grows the scrollable height instead");

	// While the column fits, it is centred as one composition, so the media gives up exactly half of
	// what the new row took and never any of its own height.
	const std::vector<view_element_ptr> no_detail;
	arrange(no_detail);
	const auto centred_bounds = media->bounds;

	assert_equal(true, centred_bounds.top > pane.top, "a column that fits does not sit against the top");

	const std::vector<view_element_ptr> one_row{std::make_shared<flex_test_element>(sizei{200, 40})};
	arrange(one_row);

	assert_equal(centred_bounds.height(), media->bounds.height(), "a detail row never resizes the media");
	assert_equal(centred_bounds.top - (40 + ui::panel_element::spacing) / 2, media->bounds.top,
	             "it moves the composition by half the row and its standard gap");
}

// The media shrinks to keep the first information group visible - a portrait image that took the
// whole pane pushed that group off the bottom, where nothing said it was there - and it stops at the
// half-pane floor rather than being centred off the top of an overflowing column.
static void should_fit_the_whole_primary_block_in_the_media_column()
{
	flex_test_measure_context mc;
	constexpr recti pane{0, 0, 200, 300};
	const std::vector<view_element_ptr> no_detail;

	const auto arrange = [&](const sizei media_extent, const view_element_ptr& toggle)
	{
		const auto media = std::make_shared<flex_test_element>(media_extent);
		media->flex = media->flex | flex_item::media;
		const std::vector<view_element_ptr> priority{media, std::make_shared<flex_test_element>(sizei{200, 20}),
		                                             toggle};
		const media_column_inputs in{&priority, &no_detail, &priority, pane, media};
		layout_media_column(in, mc);
		return media;
	};

	// Portrait: taller than the pane on its own, so the media must give way.
	auto tall_toggle = std::make_shared<flex_test_element>(sizei{200, 20});
	const auto tall_media = arrange(sizei{200, 400}, tall_toggle);

	assert_equal(true, tall_media->bounds.top >= pane.top, "a portrait image starts inside the pane");
	assert_equal(true, tall_toggle->bounds.bottom <= pane.bottom, "the verbose toggle stays inside the pane");

	// Landscape: everything fits, so the block centres rather than sitting at the top.
	auto short_toggle = std::make_shared<flex_test_element>(sizei{200, 20});
	const auto short_media = arrange(sizei{200, 100}, short_toggle);
	const auto above = short_media->bounds.top - pane.top;
	const auto below = pane.bottom - short_toggle->bounds.bottom;

	assert_equal(true, above > 0, "a block that fits does not sit against the top");
	assert_equal(true, std::abs(above - below) <= 1, "it is centred vertically");

	// Shorter than the block can be made even with the media at its floor. Centring the overflow would
	// push the top of the image above the pane, where the scroller cannot reach it.
	constexpr recti short_pane{0, 0, 200, 150};
	const auto squeezed = std::make_shared<flex_test_element>(sizei{200, 400});
	squeezed->flex = squeezed->flex | flex_item::media;
	const auto big_group = std::make_shared<flex_test_element>(sizei{200, 120});
	const std::vector<view_element_ptr> cramped{squeezed, big_group};
	const media_column_inputs cramped_in{&cramped, &no_detail, &cramped, short_pane, squeezed};
	layout_media_column(cramped_in, mc);

	assert_equal(true, squeezed->bounds.top >= short_pane.top, "an overflowing block is never clipped off the top");
}

// A description is placed under the block it belongs to rather than at the bottom edge of the pane.
// Anchoring it to the pane spent the free space the centring created on nothing and put the text a
// whole pane-height of scrolling away, with nothing on screen saying it was there - and the panel
// drops its own Description line precisely because the section below is supposed to carry it.
static void should_follow_a_centred_block_with_its_detail()
{
	flex_test_measure_context mc;
	constexpr recti pane{0, 0, 200, 600};

	const auto media = std::make_shared<flex_test_element>(sizei{200, 200});
	media->flex = media->flex | flex_item::media;
	const auto group = std::make_shared<flex_test_element>(sizei{200, 40});
	const auto description = std::make_shared<flex_test_element>(sizei{200, 60});

	const std::vector<view_element_ptr> priority{media, group};
	const std::vector<view_element_ptr> detail{description};
	const std::vector<view_element_ptr> all{media, group, description};

	const media_column_inputs in{&priority, &detail, &all, pane, media};
	layout_media_column(in, mc);

	assert_equal(group->bounds.bottom + ui::panel_element::spacing, description->bounds.top,
	             "the detail follows with the same gap as the primary panels");
	assert_equal(true, description->bounds.bottom <= pane.bottom, "a short description is read without scrolling");
	assert_equal(true, media->bounds.top > pane.top, "the block is still centred rather than held at the top");
}

// The information in the block was unbounded while the media was the only thing that ever gave way,
// down to the 64-unit floor of flex_item::media, so a container carrying several streams could
// reduce the picture to a strip. Full screen bounds its overlay against the media; this is the same
// bound read from the other side.
static void should_keep_half_the_pane_for_the_media()
{
	flex_test_measure_context mc;
	constexpr recti pane{0, 0, 200, 400};

	const auto media = std::make_shared<flex_test_element>(sizei{200, 400});
	media->flex = media->flex | flex_item::media;
	// More information than the whole pane can hold: a panel plus a stream table.
	const auto group = std::make_shared<flex_test_element>(sizei{200, 500});

	const std::vector<view_element_ptr> priority{media, group};
	const std::vector<view_element_ptr> no_detail;

	const media_column_inputs in{&priority, &no_detail, &priority, pane, media};
	const auto content_height = layout_media_column(in, mc);

	assert_equal(pane.height() / 2, media->bounds.height(), "the media keeps half the pane");
	assert_equal(media->bounds.bottom + ui::panel_element::spacing, group->bounds.top,
	             "the information follows with the standard gap");
	assert_equal(true, content_height > pane.height(), "what does not fit scrolls rather than crushing the media");

	// A media pane that never wanted the half keeps its own height; the budget is a floor on shrinking,
	// never a reservation that pads a small picture out.
	const auto small = std::make_shared<flex_test_element>(sizei{200, 80});
	small->flex = small->flex | flex_item::media;
	const std::vector<view_element_ptr> small_priority{small, group};
	const media_column_inputs small_in{&small_priority, &no_detail, &small_priority, pane, small};
	layout_media_column(small_in, mc);

	assert_equal(80, small->bounds.height(), "a picture shorter than the budget is not grown to it");
}

// Verbose metadata open. More of it than the pane can hold is the normal case, and the column then
// starts at the top so none of the pane is spent on nothing. A file carrying only a little metadata
// is the case that read as a picture pinned to the ceiling over an empty pane, so a column that fits
// centres instead - and because the offset is the free space halved, it slides to the top as the
// metadata grows rather than snapping there.
static void should_follow_the_primary_block_when_verbose_is_open()
{
	flex_test_measure_context mc;
	constexpr recti pane{0, 0, 200, 300};

	const auto media = std::make_shared<flex_test_element>(sizei{200, 100});
	media->flex = media->flex | flex_item::media;
	const auto group = std::make_shared<flex_test_element>(sizei{200, 20});
	const auto verbose = std::make_shared<flex_test_element>(sizei{200, 50});

	const std::vector<view_element_ptr> priority{media, group};
	const std::vector<view_element_ptr> detail{verbose};
	const std::vector<view_element_ptr> all{media, group, verbose};

	const media_column_inputs in{&priority, &detail, &all, pane, media};
	const auto content_height = layout_media_column(in, mc);

	const auto above = media->bounds.top - pane.top;
	const auto below = pane.bottom - verbose->bounds.bottom;

	assert_equal(true, above > 0, "a little metadata does not pin the picture to the top");
	assert_equal(true, std::abs(above - below) <= 1, "the column centres as one composition");
	assert_equal(group->bounds.bottom + ui::panel_element::spacing, verbose->bounds.top,
	             "verbose metadata follows with the standard panel gap");
	assert_equal(true, content_height >= verbose->bounds.bottom - pane.top, "the scroll extent reaches it");

	// More metadata than the pane can hold: there is nothing to centre in, so the column starts at the
	// top and the rest scrolls.
	const auto long_verbose = std::make_shared<flex_test_element>(sizei{200, 400});
	const std::vector<view_element_ptr> long_detail{long_verbose};
	const std::vector<view_element_ptr> long_all{media, group, long_verbose};
	const media_column_inputs long_in{&priority, &long_detail, &long_all, pane, media};
	const auto long_height = layout_media_column(long_in, mc);

	assert_equal(pane.top, media->bounds.top, "an overflowing column is aligned to the top");
	assert_equal(group->bounds.bottom + ui::panel_element::spacing, long_verbose->bounds.top,
	             "and preserves the same gap when the column scrolls");
	assert_equal(true, long_height > pane.height(), "what does not fit scrolls");
}

// A column that is all primary content and has no detail form at all - every multiple selection, and
// a comparison - centres by the same rule as one that has.
static void should_centre_a_media_column_with_no_detail()
{
	flex_test_measure_context mc;
	constexpr recti pane{0, 0, 200, 300};

	const auto collage = std::make_shared<flex_test_element>(sizei{200, 100});
	const auto controls = std::make_shared<flex_test_element>(sizei{200, 40});

	const std::vector<view_element_ptr> priority{collage, controls};
	const std::vector<view_element_ptr> no_detail;

	const media_column_inputs in{&priority, &no_detail, &priority, pane, nullptr};
	layout_media_column(in, mc);

	const auto above = collage->bounds.top - pane.top;
	const auto below = pane.bottom - controls->bounds.bottom;

	assert_equal(true, above > 0, "a selection panel with no detail does not sit against the top");
	assert_equal(true, std::abs(above - below) <= 1, "it is centred vertically");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// design.md: the media overlay identifies, it does not summarise. It used to grow with whatever
// metadata it found and overflow the picture, competing with the only thing the Media view exists
// for. The bound is a measurement rather than a matter of taste, and it holds at every scaling
// because it is a fraction of the media rather than a number of pixels.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_bound_the_media_overlay_to_a_quarter()
{
	// A file carrying every property it can: the overlay asks for far more than it may have.
	constexpr auto everything = 4000;

	for (const auto scale : {100, 125, 150, 200, 300})
	{
		const auto floor_height = 32 * scale / 100;

		for (const auto media_height : {480, 800, 1200, 2160})
		{
			const auto height = calc_media_overlay_height(everything, media_height, floor_height);

			assert_equal(true, height <= std::max(media_height / 4, floor_height),
			             std::format("at {}% over {}px the overlay stays within a quarter", scale, media_height));
			assert_equal(true, height < everything,
			             std::format("at {}% over {}px it truncates rather than growing", scale, media_height));
		}
	}

	// An overlay that asks for less than the bound is not padded out to it.
	assert_equal(40, calc_media_overlay_height(40, 1200, 64), "a short overlay keeps its own height");

	// A window too small for a quarter to hold anything still identifies the item rather than
	// showing an empty strip.
	assert_equal(64, calc_media_overlay_height(400, 120, 64), "the floor keeps the identification visible");

	// An audio file or an archive has no picture the panel could compete with - the panel is what the
	// view is showing - so it is allowed twice as much before it truncates.
	assert_equal(600, calc_media_overlay_height(everything, 1200, 64, true),
	             "a panel that is the presentation takes half");
	assert_equal(300, calc_media_overlay_height(everything, 1200, 64, false),
	             "a panel over a picture still takes a quarter");
}

static void should_leave_full_screen_for_task_views()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	s.is_full_screen = true;
	s.view_mode(view_type::media);
	assert_equal(0, ss.toggle_full_screen_count, "media is the full screen view");

	s.view_mode(view_type::edit);
	assert_equal(1, ss.toggle_full_screen_count, "a task view entered from full screen leaves it");

	// The stub records the request without applying it, so the windowed state is set here.
	s.is_full_screen = false;
	s.view_mode(view_type::tags);
	assert_equal(1, ss.toggle_full_screen_count, "a windowed task view does not toggle full screen");
}

static void should_preserve_view_scroller_anchor_across_layout()
{
	view_scroller scroller;
	constexpr recti client_bounds(0, 0, 100, 200);
	constexpr recti scroll_bounds(90, 0, 100, 200);
	const auto element = std::make_shared<text_element>("anchor");
	element->bounds = {0, 350, 100, 450};

	scroller.layout({100, 1000}, client_bounds, scroll_bounds);
	scroller._offset.y = 300;
	const auto anchor = scroller.capture_anchor(element);
	assert_equal(50, anchor.device_top, "scroll anchor captures device position");

	element->bounds = {0, 500, 100, 600};
	scroller.layout({100, 1200}, client_bounds, scroll_bounds);
	assert_equal(450, scroller.anchor_offset(anchor, true), "current element preserves device position");
	assert_equal(375, scroller.anchor_offset(anchor, false), "replaced element preserves scroll ratio");

	scroller._offset.y = 0;
	const auto top_anchor = scroller.capture_anchor(element);
	element->bounds = {0, 700, 100, 800};
	assert_equal(0, scroller.anchor_offset(top_anchor, true), "top remains top after layout");
}

static void should_map_the_view_scroller_track_to_the_list()
{
	view_scroller scroller;
	constexpr recti client_bounds(0, 0, 100, 200);
	constexpr recti scroll_bounds(90, 0, 100, 200);
	scroller.layout({100, 1000}, client_bounds, scroll_bounds);

	// The section bands are placed with logical_to_scrollbar_pos, so it must land on the track the
	// thumb actually travels rather than on the client height.
	const auto track = scroller.track_bounds();
	assert_equal(true, track == recti(90, 2, 100, 198), "scroll track excludes its own edge inset");
	assert_equal(track.height(), scroller.logical_to_scrollbar_pos(1000), "list end maps to track end");
	assert_equal(1000, scroller.scrollbar_pos_to_logical(track.bottom), "track end maps to list end");
	assert_equal(0, scroller.scrollbar_pos_to_logical(track.top), "track start maps to list start");
}

class scroller_test_host final : public std::enable_shared_from_this<scroller_test_host>, public view_host
{
public:
	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::frame_ptr owner() override { return nullptr; }

	void on_window_layout(render::measure_context& /*mc*/, const sizei /*extent*/, bool /*is_minimized*/) override {}
	void on_window_paint(render::draw_context& /*dc*/) override {}
	void tick() override {}
	void activate(bool /*is_active*/) override {}
	bool key_down(const char32_t /*c*/, const platform::key_state /*keys*/) override { return false; }
	void invoke(const commands /*cmd*/) override {}
	bool is_command_checked(const commands /*cmd*/) override { return false; }
	void track_menu(const recti /*bounds*/, const std::vector<ui::command_ptr>& /*commands*/) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& /*e*/) override {}
	void invalidate_view(const view_invalid /*invalid*/) override {}
	view_controller_ptr controller_from_location(pointi /*loc*/, hit_test_context& /*ctx*/) override { return nullptr; }
};

static void should_drag_view_scroller_thumb_from_grab_point()
{
	view_scroller scroller;
	scroller.layout({100, 1000}, recti(0, 0, 100, 200), recti(90, 0, 100, 200));
	scroller._offset.y = 400;

	const auto track = scroller.track_bounds();
	const auto thumb = scroller.thumb_bounds();

	assert_equal(track.top + scroller.logical_to_scrollbar_pos(400), scroller.thumb_origin(),
	             "thumb origin follows the scroll position");
	assert_equal(true, thumb.top <= scroller.thumb_origin() && thumb.bottom > scroller.thumb_origin(),
	             "painted thumb covers its origin");

	// A drag holds the point it grabbed: releasing without moving must leave the thumb where it
	// was, rather than re-centring it on the cursor as an unheld press on bare track does.
	const auto host = std::make_shared<scroller_test_host>();
	const auto grab = scroller.thumb_origin() + 3;
	scroll_controller held(host, scroller, scroller.scroll_bounds());
	held.on_mouse_left_button_down({95, grab}, {});
	held.on_mouse_left_button_up({95, grab}, {});
	assert_equal(400, scroller.scroll_offset().y, "grabbing the thumb does not move it");

	const auto expected = scroller.scrollbar_pos_to_logical(scroller.thumb_origin() + 10);
	scroll_controller dragged(host, scroller, scroller.scroll_bounds());
	dragged.on_mouse_left_button_down({95, grab}, {});
	dragged.on_mouse_move({95, grab + 10});
	dragged.on_mouse_left_button_up({95, grab + 10}, {});
	assert_equal(expected, scroller.scroll_offset().y, "grab offset is preserved while dragging");
}

static void should_close_view_scroller_bands_over_track()
{
	view_scroller scroller;
	scroller.layout({100, 1000}, recti(0, 0, 100, 200), recti(90, 0, 100, 200));
	scroller.sections({{"a", icon_index::none, 400}, {"b", icon_index::none, 700}});

	const auto track_height = scroller.track_bounds().height();
	std::vector<std::pair<int, int>> bands;
	auto labelled = 0;

	scroller.for_each_band([&](const int top, const int bottom, const view_scroller_section* so)
	{
		bands.emplace_back(top, bottom);
		if (so) ++labelled;
	});

	assert_equal(3u, static_cast<unsigned>(bands.size()), "sections plus a closing band");
	assert_equal(2, labelled, "only the sections carry labels");
	assert_equal(0, bands.front().first, "bands start at the track");
	assert_equal(track_height, bands.back().second, "bands close off the track");
}

// A host that has never been attached, or whose window has already been destroyed. Diffractor
// 1.27.0 shipped a startup crash of exactly this shape: the sidebar borrowed the items view's
// frame only when it was visible, but kept populating, counting and invalidating while hidden.
class detached_test_host final : public std::enable_shared_from_this<detached_test_host>, public view_host
{
public:
	int controller_requests = 0;
	int controller_changes = 0;
	int invalidations = 0;
	// What the pointer finds over the host, for a test of how the host reuses and replaces it. Nothing by default.
	std::function<view_controller_ptr(const view_host_ptr& host, pointi loc, hit_test_context& ctx)> hit_test;

	// The whole point: no window, ever.
	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::frame_ptr owner() override { return nullptr; }

	void on_window_layout(render::measure_context& /*mc*/, const sizei /*extent*/, bool /*is_minimized*/) override {}
	void on_window_paint(render::draw_context& /*dc*/) override {}
	void tick() override {}
	void activate(bool /*is_active*/) override {}
	bool key_down(const char32_t /*c*/, const platform::key_state /*keys*/) override { return false; }
	void invoke(const commands /*cmd*/) override {}
	bool is_command_checked(const commands /*cmd*/) override { return false; }
	void track_menu(const recti /*bounds*/, const std::vector<ui::command_ptr>& /*commands*/) override {}
	void controller_changed() override { ++controller_changes; }
	void invalidate_element(const view_element_ptr& /*e*/) override { ++invalidations; }
	void invalidate_view(const view_invalid /*invalid*/) override { ++invalidations; }

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
	{
		++controller_requests;
		return hit_test ? hit_test(shared_from_this(), loc, ctx) : nullptr;
	}
};

static void should_survive_a_host_with_no_window()
{
	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {200, 400};

	// Every entry point view_host offers, in the order a real pointer session reaches them. Each
	// one dereferences frame() unconditionally, so a null there is an access violation.
	host->on_mouse_move({10, 10}, false);
	host->on_mouse_left_button_down({10, 10}, {});
	host->on_mouse_left_button_up({10, 10}, {});
	host->on_mouse_middle_button_down({10, 10}, {});
	host->on_mouse_middle_button_up({10, 10}, {});
	host->on_mouse_left_button_double_click({10, 10}, {});
	host->on_mouse_leave({10, 10});
	host->update_cursor();
	host->show_cursor(false);
	host->invalidate_element(nullptr);

	assert_equal(false, host->escape_controller(), "nothing to escape without a controller");
	assert_equal(false, host->key_down_controller(U'a', {}), "no controller claims a key");
	assert_equal(true, host->controller_requests > 0, "hit testing still ran");

	// Scrolling asks the host for its window twice: once to shift the pixels, once to repaint.
	view_scroller scroller;
	scroller.layout({200, 4000}, recti(0, 0, 190, 400), recti(190, 0, 200, 400));
	scroller.scroll_offset(host, 0, 500);
	scroller.offset(host, 0, 120);
	assert_equal(true, scroller.scroll_offset().y > 0, "the scroller still tracks its position");
}

// A gesture that guards its own completion on having been cancelled, which every controller in the
// tree does. What is under test is whether it is still there to be asked.
class cancellable_test_controller final : public view_controller
{
public:
	bool tracking = false;
	bool cancelled = false;
	int performed = 0;

	cancellable_test_controller(view_host_ptr host, const recti bounds) : view_controller(std::move(host), bounds) {}

	void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
	{
		view_controller::on_mouse_left_button_down(loc, keys);
		tracking = true;
	}

	void on_mouse_left_button_up(const pointi /*loc*/, const platform::key_state /*keys*/) override
	{
		if (!cancelled) ++performed;
		tracking = false;
	}

	bool escape() override
	{
		if (!tracking) return false;
		cancelled = true;
		return true;
	}
};

class cancelling_test_host final : public std::enable_shared_from_this<cancelling_test_host>, public view_host
{
public:
	std::vector<std::shared_ptr<cancellable_test_controller>> built;

	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::frame_ptr owner() override { return nullptr; }

	void on_window_layout(render::measure_context& /*mc*/, const sizei /*extent*/, bool /*is_minimized*/) override {}
	void on_window_paint(render::draw_context& /*dc*/) override {}
	void tick() override {}
	void activate(bool /*is_active*/) override {}
	bool key_down(const char32_t /*c*/, const platform::key_state /*keys*/) override { return false; }
	void invoke(const commands /*cmd*/) override {}
	bool is_command_checked(const commands /*cmd*/) override { return false; }
	void track_menu(const recti /*bounds*/, const std::vector<ui::command_ptr>& /*commands*/) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& /*e*/) override {}
	void invalidate_view(const view_invalid /*invalid*/) override {}

	view_controller_ptr controller_from_location(const pointi /*loc*/, hit_test_context& /*ctx*/) override
	{
		auto controller = std::make_shared<cancellable_test_controller>(shared_from_this(), recti(0, 0, 200, 400));
		built.emplace_back(controller);
		return controller;
	}
};

// Escape cancels a gesture the button is still holding, so the release belongs to the controller
// that was cancelled. Replacing it at the moment of the cancel handed the release to a fresh one
// with no memory of it, which then performed the gesture Escape had just refused.
static void should_not_perform_a_cancelled_gesture_on_release()
{
	const auto host = std::make_shared<cancelling_test_host>();
	host->_extent = {200, 400};

	host->on_mouse_move({10, 10}, false);
	host->on_mouse_left_button_down({10, 10}, {});
	assert_equal(1u, static_cast<unsigned>(host->built.size()), "the press latched one controller");

	assert_equal(true, host->escape_controller(), "escape cancelled the gesture");
	assert_equal(1u, static_cast<unsigned>(host->built.size()), "the cancelled controller was not replaced");
	assert_equal(true, host->built.front()->cancelled, "the cancel reached the controller holding the button");

	host->on_mouse_left_button_up({12, 12}, {});
	assert_equal(0, host->built.front()->performed, "the release did not perform the cancelled gesture");
}

class idle_escape_test_controller final : public view_controller
{
public:
	bool escaped = false;

	idle_escape_test_controller(view_host_ptr host, const recti bounds) : view_controller(std::move(host), bounds) {}

	bool escape() override
	{
		escaped = true;
		return true;
	}
};

class idle_escape_test_host final : public std::enable_shared_from_this<idle_escape_test_host>, public view_host
{
public:
	std::shared_ptr<idle_escape_test_controller> built;

	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::frame_ptr owner() override { return nullptr; }

	void on_window_layout(render::measure_context& /*mc*/, const sizei /*extent*/, bool /*is_minimized*/) override {}
	void on_window_paint(render::draw_context& /*dc*/) override {}
	void tick() override {}
	void activate(bool /*is_active*/) override {}
	bool key_down(const char32_t /*c*/, const platform::key_state /*keys*/) override { return false; }
	void invoke(const commands /*cmd*/) override {}
	bool is_command_checked(const commands /*cmd*/) override { return false; }
	void track_menu(const recti /*bounds*/, const std::vector<ui::command_ptr>& /*commands*/) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& /*e*/) override {}
	void invalidate_view(const view_invalid /*invalid*/) override {}

	view_controller_ptr controller_from_location(const pointi /*loc*/, hit_test_context& /*ctx*/) override
	{
		built = std::make_shared<idle_escape_test_controller>(shared_from_this(), recti(0, 0, 200, 400));
		return built;
	}
};

static void should_offer_escape_to_an_idle_controller()
{
	const auto host = std::make_shared<idle_escape_test_host>();
	host->_extent = {200, 400};
	host->on_mouse_move({10, 10}, false);
	host->on_mouse_move({10, 10}, false);
	assert_equal(true, host->escape_controller(), "escape reaches the active controller");
	assert_equal(true, host->built->escaped, "controller consumed escape");
}

// Hit tests reach the real items view; the modal drag is counted rather than run.
class item_drag_test_host final : public std::enable_shared_from_this<item_drag_test_host>, public view_host
{
public:
	items_view& items;
	int drags = 0;

	explicit item_drag_test_host(items_view& view) : items(view) {}

	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::frame_ptr owner() override { return nullptr; }

	void on_window_layout(render::measure_context& /*mc*/, const sizei /*extent*/, bool /*is_minimized*/) override {}
	void on_window_paint(render::draw_context& /*dc*/) override {}
	void tick() override {}
	void activate(bool /*is_active*/) override {}
	bool key_down(const char32_t /*c*/, const platform::key_state /*keys*/) override { return false; }
	void invoke(const commands /*cmd*/) override {}
	bool is_command_checked(const commands /*cmd*/) override { return false; }
	void track_menu(const recti /*bounds*/, const std::vector<ui::command_ptr>& /*commands*/) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& /*e*/) override {}
	void invalidate_view(const view_invalid /*invalid*/) override {}

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
	{
		return items.items_controller_from_location(shared_from_this(), loc, ctx);
	}

	platform::drop_effect perform_drag(const std::vector<core::file_path>& /*files*/,
	                                   const std::vector<core::folder_path>& /*folders*/) override
	{
		++drags;
		return platform::drop_effect::copy;
	}
};

// A drag first lets go of the displayed media, and that wait handles input. A move handled there
// started a nested drag; once it dropped, the outer one began with the button already up and dropped
// wherever the next click landed, so an editor received the clip twice. A release or Escape handled
// in the same wait ends the gesture before any drag has begun.
static void should_start_one_drag_per_press_on_a_selected_item()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(test_files_folder()), {});
	s.update_item_groups();
	s.update_selection();

	const auto item = s.find_displayed_item_by_name("Test.jpg");
	assert_equal(true, item != nullptr, "the fixture photo is listed");
	s.select(view, item, false, false, false);
	s.update_selection();

	// The tile under the press. The move below leaves both it and the start area around the press.
	flex_test_measure_context mc;
	item->layout(mc, recti(0, 0, 100, 100));

	items_view items(s, nullptr);
	const core::scope_exit clear_hook([] { test_during_detach_file_handles = {}; });

	const auto drags_for = [&](const std::function<void(view_host&)>& while_releasing_media)
	{
		const auto host = std::make_shared<item_drag_test_host>(items);
		host->_extent = {1000, 1000};

		test_during_detach_file_handles = [&]
		{
			if (while_releasing_media) while_releasing_media(*host);
		};
		host->on_mouse_left_button_down({50, 50}, {});
		host->on_mouse_move({300, 300}, true);
		test_during_detach_file_handles = {};

		// The controller holds its host, so the pair is released as a closing window releases it.
		host->on_window_destroy();
		return host->drags;
	};

	assert_equal(1, drags_for({}), "a press that leaves the tile drags the selection once");
	assert_equal(1, drags_for([](view_host& h) { h.on_mouse_move({320, 320}, true); }),
	             "a move handled while the media is released starts no second drag");
	assert_equal(0, drags_for([](view_host& h) { h.on_mouse_left_button_up({320, 320}, {}); }),
	             "a release handled meanwhile ends the gesture before any drag");
	assert_equal(0, drags_for([](view_host& h) { h.escape_controller(); }), "and so does Escape");
}

// The press of a click that closes a menu is the menu's own, and the release that follows lands on whatever is under
// the pointer. Alone it is no click: it selects nothing, and leaves the selection as it was.
static void should_select_nothing_on_a_release_without_its_press()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(test_files_folder()), {});
	s.update_item_groups();
	s.update_selection();

	const auto selected = s.find_displayed_item_by_name("Test.jpg");
	const auto other = s.find_displayed_item_by_name("gizmo.mp4");
	assert_equal(true, selected && other, "the fixture photo and video are listed");
	s.select(view, selected, false, false, false);
	s.update_selection();

	flex_test_measure_context mc;
	for (const auto& g : s.groups()) g->bounds = recti(0, 0, 1000, 1000);
	selected->layout(mc, recti(0, 0, 100, 100));
	other->layout(mc, recti(200, 0, 300, 100));

	items_view items(s, nullptr);
	const auto host = std::make_shared<item_drag_test_host>(items);
	host->_extent = {1000, 1000};
	const auto release = [&](const pointi at, const bool pressed)
	{
		host->on_mouse_move(at, false);
		if (pressed) host->on_mouse_left_button_down(at, {});
		host->on_mouse_left_button_up(at, {});
	};

	release({250, 50}, false);
	assert_equal(true, selected->is_selected() && !other->is_selected(), "a release alone selects nothing");

	s.select(view, other, true, false, false);
	s.update_selection();
	release({50, 50}, false);
	assert_equal(true, selected->is_selected() && other->is_selected(), "nor makes one of a selection the only one");
	release({250, 50}, true);
	assert_equal(true, other->is_selected() && !selected->is_selected(), "as a whole click does");

	host->on_window_destroy();
}

// Answers one controller over a fixed region, having first recorded an occluder, so what the host
// caches can be told apart from what the controller itself claims.
class stability_test_host final : public std::enable_shared_from_this<stability_test_host>, public view_host
{
public:
	recti answer_bounds{0, 0, 200, 400};
	recti occluder;
	int builds = 0;

	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::frame_ptr owner() override { return nullptr; }

	void on_window_layout(render::measure_context& /*mc*/, const sizei /*extent*/, bool /*is_minimized*/) override {}
	void on_window_paint(render::draw_context& /*dc*/) override {}
	void tick() override {}
	void activate(bool /*is_active*/) override {}
	bool key_down(const char32_t /*c*/, const platform::key_state /*keys*/) override { return false; }
	void invoke(const commands /*cmd*/) override {}
	bool is_command_checked(const commands /*cmd*/) override { return false; }
	void track_menu(const recti /*bounds*/, const std::vector<ui::command_ptr>& /*commands*/) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& /*e*/) override {}
	void invalidate_view(const view_invalid /*invalid*/) override {}

	view_controller_ptr controller_from_location(const pointi /*loc*/, hit_test_context& ctx) override
	{
		++builds;
		ctx.occluded(occluder);
		return std::make_shared<cancellable_test_controller>(shared_from_this(), answer_bounds);
	}
};

// A controller is cached for its own region narrowed by everything the hit test passed over on the
// way to it. Neither half of that is the controller's to decide, and a tool the walk stepped over
// stays reachable only because the pointer reaching it forces a fresh test.
static void should_bound_a_reused_controller_by_what_it_passed_over()
{
	const auto host = std::make_shared<stability_test_host>();
	host->_extent = {200, 400};
	host->occluder = recti(120, 0, 160, 400);

	host->on_mouse_move({40, 40}, false);
	assert_equal(1, host->builds, "the first move ran one hit test");
	assert_equal(true, host->_controller_bounds.contains(pointi(40, 40)), "the cached region covers the pointer");
	assert_equal(120, host->_controller_bounds.right, "the cached region stops at what was passed over");

	host->on_mouse_move({60, 60}, false);
	assert_equal(1, host->builds, "moving inside the cached region reused the controller");

	host->on_mouse_move({130, 60}, false);
	assert_equal(2, host->builds, "moving onto the passed-over region forced a fresh test");
}

// The controller's own region is never narrowed to describe what covers it: an element wider than
// the answer it is cached for still owns all of itself.
static void should_not_clip_a_controller_to_what_covers_it()
{
	const auto host = std::make_shared<stability_test_host>();
	host->_extent = {200, 400};
	host->answer_bounds = recti(0, 0, 200, 400);
	host->occluder = recti(120, 0, 160, 400);

	host->on_mouse_move({40, 40}, false);

	assert_equal(200, host->_active_controller->bounds().right, "the controller kept its own region");
	assert_equal(120, host->_controller_bounds.right, "only the cached region was narrowed");
}

// An element that answers for part of itself. can_invoke is a style bit rather than a virtual, so
// the bits are set rather than overridden.
class invoke_counting_element final : public std::enable_shared_from_this<invoke_counting_element>, public view_element
{
public:
	int invokes = 0;

	invoke_counting_element()
	{
		bounds = recti(0, 0, 100, 100);
		is_visible(true);
		set_style_bit(view_element_style::can_invoke, true);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke) ++invokes;
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

// The regression the hit-test split fixes: the covering rect used to be subtracted from the
// control's own bounds, and a release is only a click when it lands inside those. Clicking the
// covered half of a control therefore did nothing at all.
static void should_keep_a_partly_covered_control_clickable()
{
	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {200, 400};

	const auto element = std::make_shared<invoke_counting_element>();

	hit_test_context ctx{{20, 20}, recti(0, 0, 200, 400)};
	ctx.occluded(recti(60, 0, 100, 100));

	const auto controller = element->controller_from_location(host, {20, 20}, {}, ctx);
	assert_equal(true, controller != nullptr, "the control answered");
	assert_equal(100, controller->bounds().right, "the controller kept the whole control");
	assert_equal(60, ctx.stability().right, "the cover narrowed only what the answer is reused for");

	controller->on_mouse_left_button_down({70, 50}, {});
	controller->on_mouse_left_button_up({70, 50}, {});
	assert_equal(1, element->invokes, "a click on the covered half still invoked");
}

// An ordinary clickable control had no escape of its own, so a press the user backed out of still
// invoked on release - and Escape is exactly the key that may have changed the view the click was
// made in, so the action landed somewhere the user was no longer looking.
static void should_not_invoke_an_escaped_click()
{
	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {200, 400};

	const auto element = std::make_shared<invoke_counting_element>();
	hit_test_context ctx{{20, 20}, recti(0, 0, 200, 400)};
	const auto controller = element->controller_from_location(host, {20, 20}, {}, ctx);
	assert_equal(true, controller != nullptr, "the control answered");

	controller->on_mouse_left_button_down({20, 20}, {});
	assert_equal(true, controller->escape(), "escape claimed the held press");

	controller->on_mouse_left_button_up({20, 20}, {});
	assert_equal(0, element->invokes, "the release did not invoke the escaped click");

	// The control is not left dead: the next press works as it always did.
	controller->on_mouse_left_button_down({20, 20}, {});
	controller->on_mouse_left_button_up({20, 20}, {});
	assert_equal(1, element->invokes, "a fresh press still invokes");

	assert_equal(false, controller->escape(), "escape claims nothing when no button is held");
}

// A region containing the pointer stops the walk; one that does not is cut away instead. Confining
// is neither: it narrows to a region already chosen, whichever side of the pointer it lies.
static void should_separate_occlusion_from_exclusion()
{
	hit_test_context ctx{{50, 50}, recti(0, 0, 200, 400)};

	assert_equal(false, ctx.occluded(recti{}), "an empty region is not in the way");
	assert_equal(200, ctx.stability().right, "an empty region cuts nothing");

	assert_equal(false, ctx.occluded(recti(120, 0, 160, 400)), "a region beside the pointer is not in the way");
	assert_equal(120, ctx.stability().right, "a region beside the pointer is cut away");

	assert_equal(true, ctx.occluded(recti(40, 40, 60, 60)), "a region under the pointer is in the way");
	assert_equal(120, ctx.stability().right, "a region under the pointer cuts nothing, having stopped the walk");

	ctx.confine(recti(0, 0, 80, 400));
	assert_equal(80, ctx.stability().right, "confining narrows to the chosen region");
	assert_equal(true, ctx.stability().contains(pointi(50, 50)), "and never cuts the pointer out");
}

// The list view's chrome is measured in text lines, so unlike the flex stub this one has to report a
// height. Only the vertical arrangement is under test, so the column widths are nominal.
class list_test_measure_context final : public render::measure_context
{
public:
	static constexpr int line_height = 20;

	sizei measure_text(const std::string_view text, render::font_face /*font*/, render::text_style /*style*/,
	                   const int cx, int /*cy*/ = 0) override
	{
		return {std::min(cx, static_cast<int>(text.size()) * 8), line_height};
	}

	int text_line_height(render::font_face /*font*/) override { return line_height; }
	render::text_layout_ptr create_text_layout(render::font_face /*font*/) override { return {}; }
};

class forwarding_test_controller final : public view_controller
{
public:
	forwarding_test_controller(view_host_ptr host, const recti bounds) : view_controller(std::move(host), bounds) {}
};

class forwarding_child_element final : public view_element
{
public:
	int control_area_requests = 0;
	int controller_requests = 0;

	forwarding_child_element() : view_element(view_element_style::can_invoke) { bounds = {10, 20, 70, 40}; }

	bool is_control_area(const pointi loc, const pointi element_offset) const override
	{
		++const_cast<forwarding_child_element*>(this)->control_area_requests;
		return is_visible() && bounds.offset(element_offset).contains(loc);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& /*ctx*/) override
	{
		++controller_requests;
		const auto device_bounds = bounds.offset(element_offset);
		return device_bounds.contains(loc) ? std::make_shared<forwarding_test_controller>(host, device_bounds)
		                                   : nullptr;
	}
};

static core::surface_ptr make_test_surface(const sizei dimensions,
                                           const core::orientation orientation = core::orientation::top_left)
{
	auto surface = std::make_shared<core::surface>();
	surface->alloc(dimensions, core::texture_format::ARGB, orientation);
	surface->make_blank();
	return surface;
}

static core::const_image_ptr make_valid_test_image()
{
	files ff;
	return ff.surface_to_thumbnail(make_test_surface({16, 12}));
}

static void should_keep_edit_load_failure_separate_from_placeholder_pixels()
{
	file_load_result loaded;
	loaded.success = true;
	loaded.s = make_test_surface({16, 12});

	file_load_result unreadable;
	unreadable.reason = file_load_result::failure::unreadable;

	file_load_result placeholder_for_unreadable;
	placeholder_for_unreadable.success = true;
	placeholder_for_unreadable.reason = file_load_result::failure::unreadable;
	placeholder_for_unreadable.s = make_test_surface({16, 16});

	file_load_result too_large;
	too_large.reason = file_load_result::failure::too_large;
	too_large.source_dimensions = {40000, 30000};

	assert_equal(true, edit_load_has_source_pixels(loaded), "decoded source pixels are editable");
	assert_equal(false, edit_load_has_source_pixels(unreadable), "an unreadable source has no editable pixels");
	assert_equal(false, edit_load_has_source_pixels(placeholder_for_unreadable),
	             "a placeholder for an unreadable source is not editable pixels");
	assert_equal(false, edit_load_has_source_pixels(too_large), "a too-large refusal has no editable pixels");
	assert_equal_strict(std::string(tt.loading.sv()), std::string(edit_load_status_text({}, true)),
	                    "pending source load is reported as loading");
	assert_equal_strict(std::string(tt.image_display_failed.sv()),
	                    std::string(edit_load_status_text(unreadable, false)),
	                    "unreadable source keeps its bounded error");
	assert_equal_strict(std::string(tt.image_too_large.sv()), std::string(edit_load_status_text(too_large, false)),
	                    "too-large source keeps its bounded error");
}

static void should_reject_stale_edit_load_preview_and_analysis_results()
{
	assert_equal(true, should_accept_edit_async_result(4, 4), "current source load is accepted");
	assert_equal(false, should_accept_edit_async_result(5, 4), "old source load is rejected");

	assert_equal(true, should_accept_edit_preview_result(3, 3, 7, 7, {640, 480}, {640, 480}),
	             "current same-size preview is accepted");
	assert_equal(false, should_accept_edit_preview_result(4, 3, 7, 7, {640, 480}, {640, 480}),
	             "preview for an old display is rejected");
	assert_equal(false, should_accept_edit_preview_result(3, 3, 8, 7, {640, 480}, {640, 480}),
	             "preview for an old request is rejected");
	assert_equal(false, should_accept_edit_preview_result(3, 3, 7, 7, {800, 600}, {640, 480}),
	             "preview for an old size is rejected");

	assert_equal(true, should_accept_edit_analysis(9, 9, 2, 2), "current analysis is accepted");
	assert_equal(false, should_accept_edit_analysis(10, 9, 2, 2), "analysis for an old display is rejected");
	assert_equal(false, should_accept_edit_analysis(9, 9, 3, 2), "analysis captured before a crop gesture is rejected");
}

// The preview decode treats the size Edit asks for as a ceiling. It can return a pixel short of it,
// or the native size of a picture smaller than the view. Judging the preview by its own extent threw
// those away on every repaint and left Edit on "Loading" for good: under that rule this test fails at
// 101 x 300, where Test.jpg is asked for 101 x 67 and decodes at 100 x 67, and at 1100 x 800, where
// it is asked for 1100 x 734 and is not enlarged.
static void should_draw_edit_preview_decoded_short_of_its_request()
{
	files ff;
	const auto loaded = ff.load(test_files_folder().combine_file("Test.jpg"), false);
	assert_equal(true, edit_load_has_source_pixels(loaded), "the fixture loads");

	core::const_surface_ptr last_preview;
	sizei last_request;

	for (const auto extent : {sizei(101, 300), sizei(1100, 800), sizei(512, 512)})
	{
		const auto label = std::format("{} x {}", extent.cx, extent.cy);
		const auto request = core::scale_dimensions(loaded.dimensions(), extent);
		const auto preview = loaded.to_surface(request);

		assert_equal(true, is_valid(preview), label, "the preview decodes");
		assert_equal(true, edit_preview_is_current(preview, request, request), label,
		             "a preview is current for the request it answered, whatever its extent");

		if (is_valid(last_preview))
		{
			assert_equal(false, edit_preview_is_current(last_preview, last_request, request), label,
			             "a preview answered for an earlier size is not current");
		}

		last_preview = preview;
		last_request = request;
	}

	assert_equal(false, edit_preview_is_current(nullptr, {640, 480}, {640, 480}), "an absent preview is never current");
}

// A file whose header reads but whose pixels do not decode left Edit on Loading for good, with the
// pixel controls live over a picture that never came. Edit now reports it as the load failure it is;
// keeping the header-only load as the state after the failure fails the "no editable pixels" check.
static void should_report_edit_preview_that_cannot_decode_as_a_load_failure()
{
	// Half a PNG, read the way files::load reads one: the header parses, the image data is cut off.
	const auto png = platform::blob_from_file(test_files_folder().combine_file("engine.png"));
	file_load_result truncated;
	truncated.i = load_image_file({png.data(), png.size() / 2});
	truncated.success = is_valid(truncated.i);
	assert_equal(true, edit_load_has_source_pixels(truncated), "a header-only load is offered as editable");

	const auto request = core::scale_dimensions(truncated.dimensions(), sizei(640, 480));
	const auto unreadable = edit_preview_failure(truncated.to_surface(request), truncated, request);
	assert_equal(true, unreadable == file_load_result::failure::unreadable, "an undecodable preview is unreadable");

	const auto failed = edit_load_without_preview(truncated, unreadable);
	assert_equal(false, edit_load_has_source_pixels(failed), "an undecodable file has no editable pixels");
	assert_equal_strict(std::string(tt.image_display_failed.sv()), std::string(edit_load_status_text(failed, false)),
	                    "an undecodable file says it failed rather than loading");

	files ff;
	const auto loaded = ff.load(test_files_folder().combine_file("Test.jpg"), false);
	const auto fitted = core::scale_dimensions(loaded.dimensions(), sizei(640, 480));
	assert_equal(true,
	             edit_preview_failure(loaded.to_surface(fitted), loaded, fitted) == file_load_result::failure::none,
	             "a decoded preview is not a failure");

	const auto restore_budget = render::max_decode_bytes;
	const core::scope_exit restore([restore_budget] { render::max_decode_bytes = restore_budget; });
	render::max_decode_bytes = 64 * 1024;

	const auto refused = edit_preview_failure(loaded.to_surface(fitted), loaded, fitted);
	assert_equal(true, refused == file_load_result::failure::too_large, "a budget refusal is too large");
	assert_equal_strict(std::string(tt.image_too_large.sv()),
	                    std::string(edit_load_status_text(edit_load_without_preview(loaded, refused), false)),
	                    "a budget refusal says the image is too large");
}

static void should_apply_pending_edit_crop_after_async_load()
{
	file_load_result loaded;
	loaded.success = true;
	loaded.s = make_test_surface({200, 100});

	const auto pending = rectd(0.25, 0.25, 0.5, 0.5);
	const auto crop = edit_pending_crop_for_loaded_photo(pending, loaded, false);

	assert_equal(true, crop.has_value(), "async bitmap load keeps the crop that opened Edit");
	const auto bounds = crop ? crop->bounding_rect() : rectd{};
	assert_equal(50, core::round(bounds.left()), "pending crop maps left to source pixels");
	assert_equal(25, core::round(bounds.top()), "pending crop maps top to source pixels");
	assert_equal(150, core::round(bounds.right()), "pending crop maps right to source pixels");
	assert_equal(75, core::round(bounds.bottom()), "pending crop maps bottom to source pixels");

	file_load_result empty;
	assert_equal(false, edit_pending_crop_for_loaded_photo(pending, empty, false).has_value(),
	             "failed load does not fabricate an editable crop");
}

static void should_draw_retained_edit_preview_while_replacement_is_pending()
{
	const auto retained = decide_edit_preview_render(true, false, true);
	assert_equal(true, retained.draw_texture, "a retained texture remains visible while the next preview decodes");
	assert_equal(true, retained.crop_interactive, "visible retained pixels keep the crop handles live");
	assert_equal(false, retained.show_loading, "loading text does not replace a visible retained preview");

	const auto empty = decide_edit_preview_render(false, false, true);
	assert_equal(false, empty.draw_texture, "no texture means nothing pixel-backed can be drawn");
	assert_equal(false, empty.crop_interactive, "crop handles are cleared while no picture is visible");
	assert_equal(true, empty.show_loading, "no retained texture shows the bounded loading state");
}

static void should_coalesce_edit_load_and_preview_requests_to_the_latest()
{
	auto first = decide_edit_request_coalescing(false);
	assert_equal(true, first.start_now, "the first request starts immediately");
	assert_equal(false, first.remember_latest, "with no in-flight request there is nothing to coalesce");

	auto second = decide_edit_request_coalescing(true);
	auto third = decide_edit_request_coalescing(true);
	assert_equal(false, second.start_now, "an in-flight request blocks immediate duplicate work");
	assert_equal(true, second.remember_latest, "the blocked request becomes the latest wanted work");
	assert_equal(false, third.start_now, "a later in-flight request is also coalesced");
	assert_equal(true, third.remember_latest, "and replaces the previous latest wanted work");
	assert_equal(true, should_start_edit_follow_up_request(true), "completion starts one follow-up");
	assert_equal(false, should_start_edit_follow_up_request(false), "with no latest request, completion starts none");
}

static void should_withhold_edit_pixel_controls_until_source_pixels_are_loaded()
{
	assert_equal(false, edit_pixel_controls_visible(true, false),
	             "bitmap controls are withheld until source pixels exist");
	assert_equal(true, edit_pixel_controls_visible(true, true), "bitmap controls appear once pixels are loaded");
	assert_equal(false, edit_pixel_controls_visible(false, true), "non-bitmaps never show pixel controls");
}

static df::item_element_ptr make_edit_test_item(const core::file_path path)
{
	df::index_file_item indexed;
	indexed.name = core::cache(path.name());
	indexed.ft = files::file_type_from_name(indexed.name);
	return std::make_shared<df::item_element>(path, indexed);
}

// Issue #268 - Edit opens a video or a sound file in a format it copies back, as well as a photo.
static void should_open_in_edit_the_videos_and_sound_files_it_copies_back()
{
	const auto at = [](const std::string_view name)
	{
		return make_edit_test_item(core::file_path(test_files_folder(), name));
	};

	assert_equal(true, can_edit_in_edit_view(at("Test.jpg")), "a photo");
	assert_equal(true, can_edit_in_edit_view(at("clip.mp4")), "a video in a format Edit writes back");
	assert_equal(true, can_edit_in_edit_view(at("song.flac")), "a sound file in one");
	assert_equal(true, can_edit_in_edit_view(at("clip.mpg")), "one in a format it does not write, saved as an MP4");
	assert_equal(false, can_edit_in_edit_view(at("tune.xm")), "not a tune played from what is drawn of it");
}

// A HEIF is edited as any photo is, though Diffractor encodes no HEIC: Edit's own turn, which the file keeps in its
// images' rotations, is saved over it, and anything more is saved as a JPEG of its own. An AVIF, which Diffractor
// encodes, is saved over itself whatever the edit.
static void should_save_a_heif_turned_in_edit_over_itself()
{
	const auto path = test_files_folder().combine_file("melnik.heic");
	assert_equal(true, can_edit_in_edit_view(make_edit_test_item(path)), "a HEIF opens in Edit");

	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index{as, locations};
	view_state state{ss, as, index, make_test_player()};
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(as, common);
	display->_item1 = make_edit_test_item(path);
	display->_is_one = true;
	display->_selected_texture1 = std::make_shared<texture_state>(as, display->_item1);
	state._display = display;

	edit_view_state draft(state);
	const auto view = std::make_shared<edit_view>(state, std::make_shared<detached_test_host>(), draft);
	view->controls(nullptr);
	view->activate({800, 600});

	while (as.run_next(async_queue::load))
	{
	}
	as.drain_ui();

	files ff;
	const auto dimensions = ff.load(path, false).dimensions();
	assert_equal(true, view->can_edit_pixels(), "its picture is loaded to edit");

	view->rotate_clockwise();
	assert_equal(true, files::saves_in_place(path, draft._edits, dimensions), "turned, it is saved over itself");

	draft._edits.crop_bounds(quadd(rectd(0, 0, dimensions.cx / 2.0, dimensions.cy / 2.0)));
	assert_equal(false, files::saves_in_place(path, draft._edits, dimensions), "cropped, it is saved as a JPEG");
	assert_equal(true, files::saves_in_place(test_files_folder().combine_file("Test.jpg"), draft._edits, dimensions),
	             "as a JPEG cropped is saved over itself");
	assert_equal(true,
	             files::saves_in_place(test_files_folder().combine_file("hato.profile0.10bpc.yuv420.avif"),
	                                   draft._edits, dimensions),
	             "and so is an AVIF");

	view->deactivate();
}

// A photo open in Edit with no window: its picture loaded and its preview decoded.
struct edit_photo_fixture
{
	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index{as, locations};
	view_state state{ss, as, index, make_test_player()};
	common_display_state_t common;
	edit_view_state draft{state};
	std::shared_ptr<detached_test_host> host = std::make_shared<detached_test_host>();
	std::shared_ptr<edit_view> view;

	explicit edit_photo_fixture(const core::file_path path)
	{
		const auto display = std::make_shared<display_state_t>(as, common);
		display->_item1 = make_edit_test_item(path);
		display->_is_one = true;
		display->_selected_texture1 = std::make_shared<texture_state>(as, display->_item1);
		state._display = display;
		state._edit_item = display->_item1;

		view = std::make_shared<edit_view>(state, host, draft);
		view->controls(nullptr);
		view->activate({800, 600});

		while (as.run_next(async_queue::load) || as.run_next(async_queue::render) ||
		       as.run_next(async_queue::thumbnail_stage))
			as.drain_ui();
		as.drain_ui();
	}

	edit_photo_fixture(const edit_photo_fixture&) = delete;
	edit_photo_fixture& operator=(const edit_photo_fixture&) = delete;

	~edit_photo_fixture() { view->deactivate(); }
};

// What slides back after a let-go is stepped as the frame loop steps it, until it settles.
static void settle_animations()
{
	for (auto i = 0; i < 1000 && !ui::animations.empty(); ++i)
	{
		std::vector<void*> settled;
		// NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order): each animation steps on its own
		for (const auto& [key, step] : ui::animations)
			if (!step()) settled.push_back(key);
		for (auto* const key : settled) ui::animations.erase(key);
	}
}

static void should_count_the_edit_histogram_as_the_colour_is_graded()
{
	std::vector<std::array<float, 3>> samples;
	samples.insert(samples.end(), 100, {0.0f, 0.0f, 0.0f});
	samples.insert(samples.end(), 100, {1.0f, 1.0f, 1.0f});
	samples.insert(samples.end(), 200, {0.5f, 0.25f, 0.75f});

	const auto as_is = count_edit_histogram(samples);
	assert_equal(400u, as_is.samples, "every sample is counted");
	assert_equal(100u, as_is.counts[0][0], "black at the bottom level");
	assert_equal(100u, as_is.counts[2][edit_histogram::levels - 1], "white at the top");
	assert_equal(true, as_is.counts[0][32] == 200 && as_is.counts[1][16] == 200 && as_is.counts[2][48] == 200,
	             "each channel of a colour at its own level");
	assert_equal(200u, as_is.peak(), "scaled to the fullest level short of black and white");

	render::color_adjust brighter;
	brighter.color_params(0, 0, 0, 0, 0, 0, 0.5);
	render::texture_transform grade;
	brighter.populate_texture_transform(grade);
	const auto graded = count_edit_histogram(samples, &grade);
	const auto& red = graded.counts[0];
	const auto red_level = std::distance(red.begin(), std::ranges::find(red, 200u));
	assert_equal(true, red_level > 32 && red_level < edit_histogram::levels - 1,
	             std::format("graded brighter, a colour moves up the levels ({})", red_level));
	assert_equal(100u, red[0], "and black stays black");

	// Red, green and blue whichever way round a picture keeps them; a wholly transparent pixel is no colour.
	const auto packed = std::make_shared<core::surface>();
	auto* const bytes = packed->alloc({2, 1}, core::texture_format::ARGB);
	const std::array<uint8_t, 8> blue_first{0, 0, 255, 255, 255, 0, 0, 0};
	std::ranges::copy(blue_first, bytes);
	const auto read = edit_histogram_samples(packed);
	assert_equal(1_z, read.size(), "a transparent pixel is left out");
	assert_equal(true, read.front() == std::array{1.0f, 0.0f, 0.0f}, "a packed pixel's red read as red");

	const auto wide = std::make_shared<core::surface>();
	auto* const wide_bytes = wide->alloc({1, 1}, core::texture_format::RGBX16);
	const std::array<uint16_t, 4> red_first{65535, 0, 0, 65535};
	std::memcpy(wide_bytes, red_first.data(), sizeof(red_first));
	assert_equal(true, edit_histogram_samples(wide).front() == std::array{1.0f, 0.0f, 0.0f},
	             "and a sixteen-bit pixel's");

	const auto large = std::make_shared<core::surface>();
	large->alloc({400, 300}, core::texture_format::RGB);
	const auto spread = edit_histogram_samples(large, 1000).size();
	assert_equal(true, spread > 500 && spread <= 1100, std::format("about as many as asked for ({})", spread));

	// The handles under it stand where their sliders move the tone curve's points.
	assert_equal(0.2, edit_tone_handle_position(0, 0.0), "the darks' at a fifth");
	assert_equal(0.6, edit_tone_handle_position(1, 1.0), "the midtones' a tenth further at the most");
	assert_equal(0.7, edit_tone_handle_position(2, -2.0), "the lights' no further than that");
	assert_equal(0.5, edit_tone_handle_value(0, 0.25), "and a handle dragged sets its slider");
	assert_equal(1.0, edit_tone_handle_value(2, 1.0), "no further than its slider goes");
}

static void should_move_a_tone_slider_by_its_histogram_handle()
{
	edit_photo_fixture edit(test_files_folder().combine_file("Test.jpg"));
	assert_equal(true, edit.view->can_edit_pixels(), "the photo is loaded to edit");
	assert_equal(true, edit.view->histogram().samples > 0, "and its preview counted");

	const auto panel = std::dynamic_pointer_cast<edit_view_controls>(edit.view->controls(nullptr));
	ui::test::fake_measure_context mc;
	const recti panel_bounds(0, 0, 400, 3000);
	panel->layout_panel(mc, panel_bounds);
	ui::test::fake_draw_context dc(panel_bounds);
	dc.colors.alpha = 1.0f;
	panel->render_panel(dc);

	// The panel's only triangles are the histogram's handles: the darks', the midtones' and the lights'.
	std::vector<pointd> apexes;
	for (const auto& polygon : dc.polygons)
		if (polygon.points.size() == 3) apexes.push_back(polygon.points.front());
	assert_equal(3_z, apexes.size(), "a handle each for the darks, the midtones and the lights");
	assert_equal(true, apexes[0].X < apexes[1].X && apexes[1].X < apexes[2].X, "left to right");
	assert_equal(4,
	             static_cast<int>(
					 std::ranges::count_if(dc.polygons, [](const auto& p) { return p.points.size() > 3; })),
	             "over a histogram: grey where all three channels reach, and each channel above that");

	// The darks' handle and the lights' stand three fifths of the histogram apart at 0.
	const auto width = (apexes[2].X - apexes[0].X) / 0.6;
	const pointi grab(core::round(apexes[0].X), core::round(apexes[0].Y) + 2);
	hit_test_context ctx(grab, panel_bounds);
	const auto controller = panel->controller_from_location(grab, ctx);
	assert_equal(true, controller != nullptr, "the darks' handle is under the pointer");
	assert_equal(true, controller->cursor() == platform::cursor::left_right, "and is dragged sideways");

	const pointi dragged(grab.x + core::round(width * 0.05), grab.y);
	controller->on_mouse_left_button_down(grab, {});
	controller->on_mouse_move(dragged);
	controller->on_mouse_left_button_up(dragged, {});
	assert_equal(true, std::abs(edit.draft._darks - 0.5) < 0.05,
	             std::format("half its reach to the right raises the darks half way ({})", edit.draft._darks));
	assert_equal(true, core::equiv(edit.draft._edits.darks(), edit.draft._darks), "which the edit takes");

	const auto kept = edit.draft._darks;
	controller->on_mouse_left_button_down(dragged, {});
	controller->on_mouse_move({dragged.x - core::round(width * 0.1), dragged.y});
	assert_equal(true, edit.draft._darks < kept, "dragged back, they fall");
	assert_equal(true, controller->escape(), "Escape takes the drag back");
	assert_equal(kept, edit.draft._darks, "to where it started");

	controller->on_mouse_left_button_double_click(dragged, {});
	assert_equal(0.0, edit.draft._darks, "and a double click puts the handle back at 0");
}

static void should_compare_an_edit_with_the_original_by_its_handle()
{
	// Keeps each picture Edit draws: whether in the edit's colour, where it is drawn and the clip it is drawn in.
	class picture_draw_context final : public ui::test::recording_draw_context
	{
	public:
		struct picture
		{
			bool graded = false;
			rectd at;
			recti clip;
		};

		std::vector<picture> pictures;

		using recording_draw_context::recording_draw_context;
		using recording_draw_context::draw_texture;

		void draw_texture(const render::texture_ptr& /*t*/, const quadd& dst, const recti /*src*/,
		                  const float /*alpha*/, const render::texture_sampler /*sampler*/) override
		{
			pictures.push_back({false, dst.bounding_rect(), clip_bounds()});
		}

		void draw_texture(const render::texture_ptr& /*t*/, const quadd& dst, const recti /*src*/,
		                  const float /*alpha*/, const render::texture_sampler /*sampler*/,
		                  const render::texture_transform& transform) override
		{
			pictures.push_back({transform.has_color_changes, dst.bounding_rect(), clip_bounds()});
		}

		void redraw(edit_view& view)
		{
			reset();
			pictures.clear();
			view.render(*this, nullptr);
		}
	};

	// Turned a quarter, the landscape photo stands upright and leaves the view room beside it.
	edit_photo_fixture edit(test_files_folder().combine_file("Test.jpg"));
	edit.draft._brightness = 0.6;
	edit.view->rotate_clockwise();
	edit.view->changed();

	const recti view_bounds(0, 0, 800, 600);
	picture_draw_context dc(view_bounds);
	dc.colors.alpha = 1.0f;
	dc.redraw(*edit.view);
	assert_equal(1_z, dc.pictures.size(), "at rest the photo is drawn once");
	assert_equal(true, dc.pictures.front().graded, "as edited");
	const auto shown = dc.pictures.front().at;
	assert_equal(true, shown.Height > shown.Width, "turned as the edit turns it");

	const auto knob = std::ranges::find_if(dc.ellipses, [](const auto& e) { return e.filled; });
	assert_equal(true, knob != dc.ellipses.end(), "with the before-and-after handle beside it");
	if (knob == dc.ellipses.end()) return;
	assert_equal(true, knob->bounds.right <= core::round(shown.left()),
	             "resting clear of the picture where the view has room, so it covers none of it");

	const auto at = knob->bounds.center();
	hit_test_context ctx(at, view_bounds);
	const auto controller = edit.view->controller_from_location(edit.host, at, ctx);
	assert_equal(true, controller && controller->cursor() == platform::cursor::left_right,
	             "which the pointer finds, to drag sideways");
	if (!controller) return;

	const pointi middle(view_bounds.width() / 2, at.y);
	controller->on_mouse_left_button_down(at, {});
	controller->on_mouse_move(middle);
	const auto held = edit.view->compare_position();
	assert_equal(true, held > 0.3f && held < 0.7f, std::format("held, its line follows the pointer ({})", held));

	dc.redraw(*edit.view);
	assert_equal(2_z, dc.pictures.size(), "the photo is drawn either side of the line");
	if (dc.pictures.size() != 2) return;
	const auto& before = dc.pictures[0];
	const auto& after = dc.pictures[1];
	assert_equal(false, before.graded, "left of it as it was");
	assert_equal(true, before.at.Width > before.at.Height, "before its turn, as the file shows it");
	assert_equal(true, after.graded, "right of it as edited");
	assert_equal(true, after.at.Height > after.at.Width, "and turned, as the edit will save it");
	assert_equal(before.clip.right, after.clip.left, "the two meeting");
	assert_equal(true, std::abs(before.clip.right - middle.x) <= 1, "under the pointer");
	assert_equal(true, dc.drawn_text().find(tt.edit_original.sv()) != std::string::npos, "each side named");

	controller->on_mouse_left_button_up(middle, {});
	settle_animations();
	assert_equal(0.0f, edit.view->compare_position(), "let go, it slides back to where it rests");
	dc.redraw(*edit.view);
	assert_equal(1_z, dc.pictures.size(), "and the edit shows whole again");

	controller->on_mouse_left_button_down(at, {});
	controller->on_mouse_move(middle);
	assert_equal(true, controller->escape(), "Escape lets go of it too");
	settle_animations();
	assert_equal(0.0f, edit.view->compare_position(), "and it slides back");

	// With no room beside the picture, it rests as far left as the whole handle shows.
	assert_equal(true, edit_view::compare_rest_x(4, 36, 8, 4) == 22, "an edge picture leaves the handle at the edge");
	assert_equal(true, edit_view::compare_rest_x(200, 36, 8, 4) == 174, "a narrow one has it beside the picture");
}

// Issue #268 - what a lossless copy of a video can do is read on a worker, and its draft is shown on the player
// as it will be saved: the trim on the scrubber, playback pausing where it ends, silence, the turn. Leaving Edit
// leaves none of it behind.
static void should_preview_a_video_draft_in_edit()
{
	const auto path = test_temps.next_path(".mp4");
	platform::copy_file(test_files_folder().combine_file("gizmo.mp4"), path, false, false);

	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index{as, locations};
	view_state state{ss, as, index, make_test_player()};
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(as, common);
	display->_item1 = make_edit_test_item(path);
	display->_is_one = true;
	display->_selected_texture1 = std::make_shared<texture_state>(as, display->_item1);
	state._display = display;

	edit_view_state draft(state);
	const auto view = std::make_shared<edit_view>(state, std::make_shared<detached_test_host>(), draft);
	view->controls(nullptr);
	view->activate({800, 600});
	assert_equal(true, view->is_av() && view->av_plan_pending(), "a video's plan is read on a worker");
	assert_equal(false, view->edits_av(), "nothing is editable before it arrives");

	while (as.run_next(async_queue::load))
	{
	}
	as.drain_ui();

	const auto& plan = view->av_plan();
	assert_equal(true, view->edits_av() && !view->av_plan_pending(), "an MP4 is copied losslessly");
	assert_equal(true, plan.readable && plan.has_video && plan.has_audio, "with its picture and its sound");
	assert_equal(true, plan.can_rotate && plan.exact_start, "turned by its track, and started where trimmed");
	assert_equal(false, view->has_changes(), "nothing to save yet");
	assert_equal(true, display->_trim_handles, "the scrubber offers its trim handles");
	assert_equal(std::string(tt.edit_status_unchanged.sv()), std::string(view->status()),
	             "and the status bar says Save As still writes a re-muxed copy");

	draft._trim_start = 2.0;
	draft._trim_end = 4.0;
	draft._remove_sound = true;
	view->rotate_clockwise();

	assert_equal(true, view->has_changes(), "the draft is a change to save");
	assert_equal(2.0, display->_trim_start, "the scrubber marks the start");
	assert_equal(4.0, display->_trim_end, "and the end");
	assert_equal(4.0, state._playback_end, "where playback pauses");
	assert_equal(true, display->_mute_preview, "the video plays silently");
	assert_equal(1, display->_selected_texture1->_preview_quarter_turns, "and turned");
	assert_equal("Start 0:02.0, end 0:04.0, length 0:02.0 \u00B7 Sound removed \u00B7 Rotated right"s,
	             std::string(view->status()), "the status bar lists what a save would change");

	view->trim(1.0, 0.0);
	assert_equal(true, std::string(view->status()).starts_with("Start 0:01.0, end "),
	             "a handle moved on the scrubber shows there at once");
	draft._trim_end = 4.0;

	const auto edits = view->av_draft();
	assert_equal(true, edits.start == 1.0 && edits.end > 4.0 && edits.end < 4.01,
	             "the copy keeps the frame shown where the trim ends");
	assert_equal(true, edits.remove_audio && edits.quarter_turns == 1, "and has no sound, turned once");

	view->rotate_anticlockwise();
	assert_equal(0, view->av_draft().quarter_turns, "turning back leaves it as it was");

	view->deactivate();
	assert_equal(0.0, display->_trim_end, "leaving Edit clears the scrubber");
	assert_equal(false, display->_trim_handles, "takes its handles away");
	assert_equal(0.0, state._playback_end, "lets playback run on");
	assert_equal(false, display->_mute_preview, "brings the sound back");
	assert_equal(true, view->status().empty(), "clears the status bar");
	assert_equal(false, view->has_changes(), "and leaves nothing to save");
}

// Issue #268 - Edit trims with a handle at each end of the scrubber. A handle moves as far as the pointer does from
// where it was pressed, never nearer the other than a tenth of a second, and back at its own end of the track trims
// nothing there; Escape puts the trim back as it was pressed. Outside Edit the scrubber only seeks.
static void should_drag_edit_trim_handles_along_the_scrubber()
{
	null_async_strategy async;
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(async, common);
	display->_player_media_info.end = 10.0;
	const auto scrubber = std::make_shared<scrubber_element>(make_test_player(), display);
	scrubber->trim_changed([&display](const double start, const double end)
	{
		display->_trim_start = start;
		display->_trim_end = end;
	});

	ui::test::fake_measure_context mc;
	const auto extent = scrubber->measure(mc, 1000);
	const recti bounds(0, 0, 1000, extent.cy);
	scrubber->layout(mc, bounds);
	const auto width = scrubber->calc_track_bounds({}).width();
	const auto host = std::make_shared<detached_test_host>();

	const auto take = [&](const int handle)
	{
		const auto at = scrubber->trim_handle_bounds(handle, {}).center();
		hit_test_context ctx{at, bounds};
		const auto controller = scrubber->controller_from_location(host, at, {}, ctx);
		return std::pair{std::dynamic_pointer_cast<scrubber_trim_controller>(controller), at};
	};

	const auto drag = [&](const int handle, const int dx)
	{
		const auto [controller, at] = take(handle);
		assert_equal(true, controller != nullptr, "a handle is taken");
		controller->on_mouse_left_button_down(at, {});
		controller->on_mouse_move({at.x + dx, at.y});
		controller->on_mouse_left_button_up({at.x + dx, at.y}, {});
	};

	const auto near = [](const double expected, const double actual)
	{
		return std::abs(expected - actual) < 0.02;
	};

	assert_equal(true, take(0).first == nullptr, "outside Edit the scrubber has no handles");
	display->_trim_handles = true;

	// The host reuses what it found under the pointer while the pointer stays where that holds. The handles were first
	// left out of where the track's seek holds, so a pointer coming onto one along the track kept seeking, and a press
	// on the handle sought instead of taking it.
	host->_extent = {bounds.width(), bounds.height()};
	host->hit_test = [&scrubber](const view_host_ptr& h, const pointi loc, hit_test_context& ctx)
	{
		return scrubber->controller_from_location(h, loc, {}, ctx);
	};
	const auto handle_at = scrubber->trim_handle_bounds(0, {}).center();
	host->on_mouse_move(scrubber->calc_track_bounds({}).center(), false);
	host->on_mouse_move(handle_at, false);
	host->on_mouse_left_button_down(handle_at, {});
	host->on_mouse_move({handle_at.x + width / 4, handle_at.y}, true);
	host->on_mouse_left_button_up({handle_at.x + width / 4, handle_at.y}, {});
	host->on_mouse_leave({-1, -1});
	assert_equal(true, near(2.5, display->_trim_start),
	             std::format("a handle reached along the track follows the pointer, to {}", display->_trim_start));
	assert_equal(0.0, display->_trim_end, "and the end still keeps the rest");

	drag(1, -width / 2);
	assert_equal(true, near(5.0, display->_trim_end), std::format("the end follows it, to {}", display->_trim_end));

	drag(0, width);
	assert_equal(true, near(4.9, display->_trim_start), "the handles never cross");

	const auto [end_handle, end_at] = take(1);
	const auto [start_handle, start_at] = take(0);
	assert_equal(true, end_handle && start_handle, "handles standing together are each still taken");

	end_handle->on_mouse_left_button_down(end_at, {});
	end_handle->on_mouse_move({end_at.x + width / 10, end_at.y});
	assert_equal(true, near(6.0, display->_trim_end), "a handle moves while it is dragged");
	assert_equal(true, end_handle->escape(), "Escape lets go of it");
	assert_equal(true, near(5.0, display->_trim_end), "and puts the trim back as it was pressed");

	drag(1, width);
	assert_equal(0.0, display->_trim_end, "an end taken back to the end of the track keeps the rest");
	drag(0, -width);
	assert_equal(0.0, display->_trim_start, "as a start taken back to its beginning keeps all before it");
}

// Issue #268 - a held trim handle shows the frame it is on, and letting go puts the picture back where the playhead
// was: setting a trim is a look at another frame, not a move of the playhead. Playback the press stopped carries on;
// Escape goes back the same way, and a video paused before stays paused.
static void should_return_to_the_playhead_when_a_trim_handle_is_let_go()
{
	const auto path = test_files_folder().combine_file("gizmo.mp4");
	const auto session = make_test_session();
	assert_equal(true, session->open(path, files::file_type_from_name(path), 0.0, true, -1, -1, false, false, false),
	             "the video opens playing");
	const core::scope_exit close([session] { session->close(false); });
	session->mark_audio_output_unavailable();

	null_async_strategy async;
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(async, common);
	display->update_av_session(session);
	display->_trim_handles = true;

	const auto player = make_test_player();
	const auto scrubber = std::make_shared<scrubber_element>(player, display);
	scrubber->trim_changed([&display](const double start, const double end)
	{
		display->_trim_start = start;
		display->_trim_end = end;
	});

	ui::test::fake_measure_context mc;
	const recti bounds(0, 0, 1000, scrubber->measure(mc, 1000).cy);
	scrubber->layout(mc, bounds);
	const auto width = scrubber->calc_track_bounds({}).width();
	const auto host = std::make_shared<detached_test_host>();

	constexpr auto playhead = 1.5;
	session->seek(playhead, false);
	session->mark_presented(core::now());
	assert_equal(playhead, display->media_pos(), "the playhead is part way in");

	const auto take = [&](const int handle)
	{
		const auto at = scrubber->trim_handle_bounds(handle, {}).center();
		hit_test_context ctx{at, bounds};
		auto controller = std::dynamic_pointer_cast<scrubber_trim_controller>(
			scrubber->controller_from_location(host, at, {}, ctx));
		assert_equal(true, controller != nullptr, "a handle is taken");
		return std::pair{std::move(controller), at};
	};

	const auto [start_handle, start_at] = take(0);
	start_handle->on_mouse_left_button_down(start_at, {});
	start_handle->on_mouse_move({start_at.x + width / 2, start_at.y});
	player->run_queued_commands();
	assert_equal(false, session->is_playing(), "taking hold of a handle stops playback");
	assert_equal(true, display->_trim_start > playhead, "the handle is taken past the playhead");
	assert_equal(display->_trim_start, session->pos(core::now()), "and the picture shows the frame it is on");

	start_handle->on_mouse_left_button_up({start_at.x + width / 2, start_at.y}, {});
	player->run_queued_commands();
	assert_equal(playhead, session->pos(core::now()), "letting go puts the picture back where the playhead was");
	assert_equal(true, session->is_playing(), "and playback carries on");

	player->pause(session);
	player->run_queued_commands();
	const auto [end_handle, end_at] = take(1);
	end_handle->on_mouse_left_button_down(end_at, {});
	end_handle->on_mouse_move({end_at.x - width / 10, end_at.y});
	player->run_queued_commands();
	assert_equal(display->_trim_end, session->pos(core::now()), "the end handle shows its frame too");
	assert_equal(true, end_handle->escape(), "Escape lets go of it");
	player->run_queued_commands();
	assert_equal(0.0, display->_trim_end, "putting the trim back");
	assert_equal(playhead, session->pos(core::now()), "and the picture back where the playhead was");
	assert_equal(false, session->is_playing(), "a video paused before stays paused");
}

static std::shared_ptr<display_state_t> make_zoom_pan_display(async_strategy& async, common_display_state_t& common)
{
	auto display = std::make_shared<display_state_t>(async, common);
	display->_is_one = true;
	display->_can_zoom = true;
	display->zoom_layout({1000, 800}, {500, 400}, {0, 0});
	display->zoom(true);
	return display;
}

static void should_pan_items_touch_on_magnified_media_like_fullscreen()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	items_view items(state, nullptr);
	items._regions.media = {0, 0, 500, 400};

	common_display_state_t expected_common;
	const auto expected = make_zoom_pan_display(as, expected_common);
	const auto start_zoom = expected->zoom_state();
	expected->pan_zoom({80, 40}, start_zoom);

	common_display_state_t items_common;
	items._display = make_zoom_pan_display(as, items_common);
	items.pan_start({100, 100});
	items.pan({100, 100}, {180, 140});
	items.pan_end({100, 100}, {180, 140});

	assert_zoom_near(expected->zoom_state().center().X, items._display->zoom_state().center().X,
	                 "items touch pan uses the shared zoom center x");
	assert_zoom_near(expected->zoom_state().center().Y, items._display->zoom_state().center().Y,
	                 "items touch pan uses the shared zoom center y");

	common_display_state_t fitted_common;
	items._display = std::make_shared<display_state_t>(as, fitted_common);
	items._display->_is_one = true;
	items._display->_can_zoom = true;
	items._display->zoom_layout({1000, 800}, {500, 400}, {0, 0});
	const auto fitted_center = items._display->zoom_state().center();
	items.pan_start({100, 100});
	items.pan({100, 100}, {180, 140});
	items.pan_end({100, 100}, {180, 140});
	assert_zoom_near(fitted_center.X, items._display->zoom_state().center().X, "fitted media ignores touch pan x");
	assert_zoom_near(fitted_center.Y, items._display->zoom_state().center().Y, "fitted media ignores touch pan y");
}

static core::const_image_ptr make_corrupt_test_image()
{
	return std::make_shared<core::image>(core::blob{0xff, 0xd8, 0xff, 0xd9}, sizei{32, 32}, core::image_format::JPEG,
	                                     core::orientation::top_left);
}

static df::item_element_ptr make_thumbnail_test_item(const std::string& name, const core::const_image_ptr& thumbnail)
{
	df::index_file_item indexed;
	indexed.name = core::cache(name);
	indexed.ft = files::file_type_from_name(indexed.name);

	const auto item = std::make_shared<df::item_element>(core::file_path("c:\\" + name), indexed);
	item->thumbnail(thumbnail, {}, {});
	return item;
}

static void should_refresh_tooltip_when_staged_thumbnail_lands()
{
	deferred_async_strategy as;
	const auto item = make_thumbnail_test_item("tooltip.jpg", make_valid_test_image());

	item->stage_thumbnail_surface(as, false, true);
	item->stage_thumbnail_surface(as, false, true);
	assert_equal(1_z, as.pending_worker_count(async_queue::thumbnail_stage), "tooltip staging queues one decode");

	assert_equal(true, as.run_next(async_queue::thumbnail_stage), "decode runs");
	as.drain_ui();

	assert_equal(true, item->has_cached_surface(), "staging produced a surface");
	assert_equal(true, as.was_invalidated(view_invalid::tooltip), "tooltip is rebuilt when the surface lands");
	assert_equal(true, as.was_invalidated(view_invalid::view_redraw), "view redraw accompanies tooltip refresh");
}

static void should_not_refresh_tooltip_for_fresh_request_when_only_cover_art_is_cached()
{
	deferred_async_strategy stage;
	const auto item = make_thumbnail_test_item("cover-only.jpg", make_corrupt_test_image());
	item->thumbnail(make_corrupt_test_image(), make_valid_test_image(), {});

	item->stage_thumbnail_surface(stage);
	assert_equal(true, stage.run_next(async_queue::thumbnail_stage), "cover-art decode runs");
	stage.drain_ui();
	assert_equal(true, item->has_cached_surface(), "cover art can cache even when the thumbnail failed");
	assert_equal(false, core::is_valid(item->thumbnail_surface()), "thumbnail surface is still absent");

	deferred_async_strategy tooltip;
	item->stage_thumbnail_surface(tooltip, false, true);

	assert_equal(false, tooltip.was_invalidated(view_invalid::tooltip),
	             "a fresh tooltip request must not endlessly rebuild against cover-art-only cache");
	assert_equal(false, tooltip.was_invalidated(view_invalid::view_redraw),
	             "a fresh tooltip request against cover-art-only cache needs no redraw");
}

static void should_retry_scroll_tooltip_after_failed_stage()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	const auto failed = make_thumbnail_test_item("failed.jpg", make_corrupt_test_image());
	const auto next = make_thumbnail_test_item("next.jpg", make_valid_test_image());
	const df::item_group group(s, {failed, next}, df::item_group_display::icons, {});
	const auto elements = std::make_shared<view_elements>();

	group.scroll_tooltip(failed, elements, as);
	assert_equal(true, as.run_next(async_queue::thumbnail_stage), "failed decode runs");
	as.drain_ui();

	group.scroll_tooltip(next, elements, as);
	assert_equal(1_z, as.pending_worker_count(async_queue::thumbnail_stage),
	             "a failed previous stage does not block the new hover item");
}

static void should_stage_one_scroll_tooltip_thumbnail()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	df::item_elements items;
	const auto thumbnail = make_valid_test_image();

	for (auto i = 0; i < 32; ++i)
	{
		items.emplace_back(make_thumbnail_test_item("scroll" + std::to_string(i) + ".jpg", thumbnail));
	}

	const df::item_group group(s, items, df::item_group_display::icons, {});
	const auto elements = std::make_shared<view_elements>();

	group.scroll_tooltip(nullptr, elements, as);
	group.scroll_tooltip(nullptr, elements, as);

	assert_equal(1_z, as.pending_worker_count(async_queue::thumbnail_stage),
	             "one hover queues at most one fallback thumbnail stage");
}

static void should_publish_async_selection_thumbnail_strip()
{
	deferred_async_strategy as;
	auto strip = std::make_shared<ui::selection_thumbnails_control>(ui::no_frame());
	const std::vector<core::const_image_ptr> images{make_valid_test_image(), make_valid_test_image()};

	strip->selection_async(images, images.size(), as);
	assert_equal(1_z, as.pending_worker_count(async_queue::render), "strip decode is queued");

	assert_equal(true, as.run_next(async_queue::render), "strip decode runs");
	as.drain_ui();

	list_test_measure_context mc;
	const auto measured = strip->measure(mc, 300);
	assert_equal(true, measured.cy > 0, "published surfaces measure the thumbnail strip");
	assert_equal(true, strip->is_visible(), "strip becomes visible after async publication");
}

static void should_discard_async_selection_strip_after_clear()
{
	deferred_async_strategy as;
	auto strip = std::make_shared<ui::selection_thumbnails_control>(ui::no_frame());
	const std::vector<core::const_image_ptr> images{make_valid_test_image()};

	strip->selection_async(images, images.size(), as);
	strip->selection({}, 0);

	assert_equal(true, as.run_next(async_queue::render), "stale strip decode runs");
	as.drain_ui();

	assert_equal(false, strip->is_visible(), "cleared strips reject stale async thumbnails");
}

// The frame's own child controls hold the strip in their callbacks, so a strip that owned its frame
// closed a cycle that destroyed the frame while it was clearing those children: exiting after the
// Import view had been open ended in heap corruption.
static void should_not_keep_selection_strip_frame_alive()
{
	deferred_async_strategy as;
	auto frame = std::make_shared<ui::null_frame>();
	const std::weak_ptr<ui::null_frame> weak_frame = frame;
	const auto strip = std::make_shared<ui::selection_thumbnails_control>(frame);
	const std::vector<core::const_image_ptr> images{make_valid_test_image()};

	strip->selection_async(images, images.size(), as);
	frame.reset();
	assert_equal(true, weak_frame.expired(), "the strip does not own the frame it is shown in");

	assert_equal(true, as.run_next(async_queue::render), "strip decode runs");
	as.drain_ui();
	assert_equal(true, strip->is_visible(), "a strip whose frame has gone still publishes its thumbnails");
}

static void should_preview_rotation_with_inverse_destination_transform()
{
	assert_equal(static_cast<int>(core::simple_transform::rot_270),
	             static_cast<int>(
					 ui::before_after_control::preview_destination_transform(core::simple_transform::rot_90)),
	             "clockwise source edit previews with the inverse destination transform");
	assert_equal(static_cast<int>(core::simple_transform::rot_90),
	             static_cast<int>(
					 ui::before_after_control::preview_destination_transform(core::simple_transform::rot_270)),
	             "anticlockwise source edit previews with the inverse destination transform");
}

// Issue #260 - on 1.27.2 the anticlockwise preview turned the picture clockwise. The preview must draw
// what the rotation writes, so both are made here from one picture with its top-left quarter marked,
// and the quarter the mark lands in is compared.
static void should_preview_a_rotation_as_it_is_written()
{
	constexpr sizei extent(20, 10);
	constexpr sizei rotated(10, 20);
	const auto source = std::make_shared<core::surface>();
	source->alloc(extent.cx, extent.cy, core::texture_format::RGB);

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto marked = x < extent.cx / 2 && y < extent.cy / 2;
			source->set_pixel(x, y, marked ? core::rgba(255, 255, 255) : core::rgba(0, 0, 0));
		}
	}

	for (const auto t : {core::simple_transform::rot_90, core::simple_transform::rot_270})
	{
		const auto direction = t == core::simple_transform::rot_90 ? "clockwise"s : "anticlockwise"s;

		image_edits edits;
		edits.crop_bounds(rotate_command_crop_bounds(extent, core::orientation::top_left, t, true));
		const auto written = apply_image_edits(source, edits);
		assert_equal(true, core::is_valid(written) && written->dimensions() == rotated,
		             "the rotation writes the picture on its side", direction);

		std::vector<uint8_t> bits(static_cast<size_t>(rotated.cx) * rotated.cy * 4);
		render::software_canvas canvas;
		canvas._bits = bits.data();
		canvas._stride = rotated.cx * 4;
		canvas._buffer_extent = rotated;
		canvas._origin = {0, 0};
		canvas._clip = recti({}, rotated);
		const auto destination = quadd(recti({}, rotated))
		                             .transform(ui::before_after_control::preview_destination_transform(t));
		canvas.blit_quad(*source, recti({}, extent), destination, 1.0f, render::texture_sampler::point, false);

		// Turned clockwise the top-left quarter goes to the top right; anticlockwise, to the bottom left.
		const auto expected_qx = t == core::simple_transform::rot_90 ? 1 : 0;
		const auto expected_qy = t == core::simple_transform::rot_90 ? 0 : 1;

		for (auto qy = 0; qy < 2; ++qy)
		{
			for (auto qx = 0; qx < 2; ++qx)
			{
				const auto x = rotated.cx / 4 + qx * rotated.cx / 2;
				const auto y = rotated.cy / 4 + qy * rotated.cy / 2;
				const auto expected = qx == expected_qx && qy == expected_qy;
				assert_equal(expected, core::get_g(written->get_pixel(x, y)) > 128,
				             "the rotation puts the mark in the quarter its direction turns it to", direction);
				assert_equal(expected, canvas.pixel(x, y)[1] > 128, "and the preview shows it in the same quarter",
				             direction);
			}
		}
	}
}

static void should_rearm_selector_decode_after_resource_event()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	selector_view selector(s, nullptr, nullptr);
	selector.test_add_pending_decode_for_resource_event();

	selector.broadcast_event({view_element_event_type::free_graphics_resources});

	assert_equal(true, selector.test_resource_event_cleared_pending_decode(),
	             "resource reset clears pending decode latches through broadcast_event");
}

// The arrows move the keyboard along the strip, and Enter shows the item it is on. The place the arrows leave holds
// until the view moves to another item, which the strip then follows rather than going back to that place.
static void should_follow_the_view_s_item_along_the_strip()
{
	browsing_fixture f;
	auto& s = f.state;

	df::item_elements items;
	for (const auto& group : s.groups())
	{
		for (const auto& i : group->items()) items.emplace_back(i);
	}

	assert_equal(true, items.size() > 3, "test folder has enough items to step along");

	df::item_element_ptr chosen;
	const auto choose = [&chosen](const df::item_element_ptr& item, platform::key_state)
	{
		chosen = item;
	};
	const auto selector = std::make_shared<selector_view>(s, std::make_shared<detached_test_host>(), choose);
	selector->activate({0, 0});
	s.select(f.view, items[0], false, false, false);

	selector->key_down(platform::keys::RIGHT, {});
	selector->key_down(platform::keys::RIGHT, {});
	selector->key_down(platform::keys::RETURN, {});
	assert_equal(true, chosen == items[2], "the arrows move along the strip from the view's item");

	s.select(f.view, items[3], false, false, false);
	selector->key_down(platform::keys::RETURN, {});
	assert_equal(true, chosen == items[3], "and once the view moves to another item, the strip is on that one");

	selector->key_down(platform::keys::END, {});
	selector->key_down(platform::keys::RETURN, {});
	assert_equal(true, chosen == items.back(), "End goes to the last item, as in any list");
	selector->key_down(platform::keys::HOME, {});
	selector->key_down(platform::keys::RETURN, {});
	assert_equal(true, chosen == items.front(), "and Home to the first");
}

// Items' scroll bar marks the selection as the strip's does, so an item scrolled out of sight can still be found: each
// selected item where its middle lies in the listing, worked out again when the selection changes.
static void should_mark_the_selection_on_the_items_scroll_bar()
{
	browsing_fixture f;
	auto& s = f.state;

	df::item_elements items;
	for (const auto& group : s.groups())
	{
		for (const auto& i : group->items()) items.emplace_back(i);
	}

	assert_equal(true, items.size() > 2, "test folder has items to select");

	const auto host = std::make_shared<detached_test_host>();
	items_view view(s, host);
	view._items_scroller.layout({800, 4000}, {0, 100, 800, 700}, {760, 100, 800, 700});
	auto top = 100;
	for (const auto& i : items)
	{
		i->bounds = recti(0, top, 100, top + 20);
		top += 40;
	}

	s.select(f.view, items[1], false, false, false);
	s.update_selection();
	view.update_selection_marks();
	assert_equal(true, view._items_scroller._mark_centres == std::vector<int>{items[1]->bounds.center().y - 100},
	             "the selected item is marked where its middle lies in the listing");

	s.select(f.view, items[2], true, false, false);
	s.update_selection();
	view.update_selection_marks();
	assert_equal(2_z, view._items_scroller._mark_centres.size(), "and marked again when the selection changes");
}

// A press between items dragged out of the distance a shaky click covers is a rubber band until it is let go, as
// every list's is: back over the press it is still a band, rather than a click again.
static void should_keep_an_items_band_until_it_is_let_go()
{
	browsing_fixture f;
	const auto host = std::make_shared<detached_test_host>();
	items_view view(f.state, host);
	view._items_scroller.layout({800, 4000}, {0, 100, 800, 700}, {760, 100, 800, 700});

	const pointi press{400, 300};
	hit_test_context ctx(press, recti(0, 0, 800, 800));
	const auto band = view.items_controller_from_location(host, press, ctx);
	assert_equal(true, band != nullptr, "a press between items is taken");

	band->on_mouse_left_button_down(press, {});
	band->on_mouse_move({402, 302});
	assert_equal(true, band->cursor() == platform::cursor::normal, "a shaky press is still a click");
	band->on_mouse_move({480, 380});
	assert_equal(true, band->cursor() == platform::cursor::select, "dragged out, it is a band");
	band->on_mouse_move({401, 301});
	assert_equal(true, band->cursor() == platform::cursor::select, "and back over the press, still a band");
	assert_equal(true, band->escape(), "until Escape lets it go");
	assert_equal(true, band->cursor() == platform::cursor::normal, "and it is no band");
}

// A selected item scrolled out of the strip is still marked on its scroll bar, where the thumb's centre sits while the
// item is in the middle of the strip: the mark is inside the thumb just when the item is on screen, and pressing the
// track at it - as a press away from the thumb centres the thumb there - brings the item into view.
static void should_mark_the_selection_on_the_strip_s_scroll_bar()
{
	browsing_fixture f;
	const auto files = f.files();
	assert_equal(true, files.size() > 8, "test folder has enough files to scroll a strip");

	constexpr sizei extent{300, 140};
	ui::test::fake_measure_context mc;
	const auto selector = std::make_shared<selector_view>(f.state, std::make_shared<detached_test_host>(), nullptr);
	selector->filter(is_file_item);
	selector->activate(extent);
	selector->layout(mc, extent);
	assert_equal(true, selector->can_scroll(), "the strip is too narrow for every file");
	assert_equal(0_z, selector->scrollbar_marks().size(), "nothing selected, nothing marked");

	const auto& last = files.back();
	f.state.select(f.view, last, false, false, false);
	const auto marks = selector->scrollbar_marks();
	assert_equal(1_z, marks.size(), "the selected item is marked");
	const auto mark = marks.front().center().x;
	const auto before = selector->scrollbar_thumb_bounds();
	assert_equal(false, before.left <= mark && mark < before.right, "off screen, its mark is outside the thumb");
	assert_equal(true,
	             selector->scrollbar_bounds().left <= marks.front().left &&
	                 marks.front().right <= selector->scrollbar_bounds().right,
	             "and on the track");

	selector->scrollbar_to(mark - before.width() / 2);
	const auto after = selector->scrollbar_thumb_bounds();
	assert_equal(true, after.left <= mark && mark < after.right, "pressing the track there brings the thumb over it");
	selector->make_visible(last);
	assert_equal(true, selector->scrollbar_thumb_bounds() == after,
	             "and the item on screen, so showing it scrolls no further");

	f.state.select(f.view, df::item_elements{files.front(), last}, false);
	const auto both = selector->scrollbar_marks();
	assert_equal(2_z, both.size(), "each selected item is marked");
	assert_equal(true, both[0].right < both[1].left, "in the strip's order");
}

// A task view opens its strip, and lays it out at a passing size or none before the strip has its own; the item the
// view is on is shown once it does, rather than the strip's first few items with the selection off its end.
static void should_show_the_strip_s_item_once_it_has_its_size()
{
	browsing_fixture f;
	const auto files = f.files();
	assert_equal(true, files.size() > 10, "test folder has enough files to scroll a strip");

	const auto& on = files[8];
	f.state.select(f.view, on, false, false, false);

	ui::test::fake_measure_context mc;
	const auto selector = std::make_shared<selector_view>(f.state, std::make_shared<detached_test_host>(), nullptr);
	selector->filter(is_file_item);
	selector->activate({});
	selector->layout(mc, {});
	selector->layout(mc, {4000, 140});
	selector->layout(mc, {300, 140});

	const auto thumb = selector->scrollbar_thumb_bounds();
	selector->make_visible(on);
	assert_equal(true, selector->scrollbar_thumb_bounds() == thumb, "the item the view is on is on screen");
}

// When the strip's set shrinks - Locate hiding what it has placed - the item it is on stays where it was on screen,
// rather than the scroll holding still while the tiles slide out from under it.
static void should_keep_the_strip_s_item_in_place_as_its_set_shrinks()
{
	browsing_fixture f;
	const auto files = f.files();
	assert_equal(true, files.size() > 10, "test folder has enough files to scroll a strip");

	constexpr sizei extent{300, 140};
	ui::test::fake_measure_context mc;
	core::hash_set<const df::item_element*> hidden;
	const auto selector = std::make_shared<selector_view>(f.state, std::make_shared<detached_test_host>(), nullptr);
	selector->filter([&hidden](const df::item_element_ptr& i) { return is_file_item(i) && !hidden.contains(i.get()); });
	selector->activate(extent);
	selector->layout(mc, extent);

	const auto& on = files[8];
	f.state.select(f.view, on, false, false, false);
	selector->make_visible(on);

	// What placing items does: they leave the set while the listing stays as it was. Four tiles slid out from
	// before it would take the item off screen with them.
	for (size_t i = 0; i < 4; ++i) hidden.emplace(files[i].get());
	selector->layout(mc, extent);

	const auto thumb = selector->scrollbar_thumb_bounds();
	selector->make_visible(on);
	assert_equal(true, selector->scrollbar_thumb_bounds() == thumb, "the item stays on screen");

	f.state.select(f.view, df::item_elements{on, files[0]}, false);
	assert_equal(1_z, selector->scrollbar_marks().size(), "while what left the set has left the strip");
}

// Save-and-next in Edit and Add location and next in Locate step through what their strip offers, from wherever the
// focus is - an item the task has just taken out of that set too - so the item moved to is one the strip shows.
static void should_step_through_what_the_strip_offers()
{
	browsing_fixture f;
	auto& s = f.state;
	const auto files = f.files();
	assert_equal(true, files.size() > 7, "test folder has enough files to step through");

	// Every third file is in the set.
	core::hash_set<const df::item_element*> offered;
	for (size_t i = 0; i < files.size(); i += 3) offered.emplace(files[i].get());
	const auto offers = [&offered](const df::item_element_ptr& i)
	{
		return offered.contains(i.get());
	};

	s.select(f.view, files[1], false, false, false);
	assert_equal(true, s.next_item_where(true, true, offers) == files[3], "forward past what the set leaves out");
	assert_equal(true, s.next_item_where(false, true, offers) == files[0], "and back");

	s.select(f.view, files[3], false, false, false);
	offered.erase(files[3].get());
	assert_equal(true, s.next_item_where(true, true, offers) == files[6],
	             "from an item just taken out of the set, on to the next in it");

	const auto& last_offered = files[(files.size() - 1) / 3 * 3];
	s.select(f.view, last_offered, false, false, false);
	assert_equal(true, s.next_item_where(true, false, offers) == nullptr, "nothing past the end without wrap");
	assert_equal(true, s.next_item_where(true, true, offers) == files[0], "and round to the start with it");

	offered.clear();
	offered.emplace(last_offered.get());
	assert_equal(true, s.next_item_where(true, true, offers) == last_offered, "the only one in the set is itself");
	offered.clear();
	assert_equal(true, s.next_item_where(true, true, offers) == nullptr, "and an empty set has none");
}

class absent_frame_complete_strategy final : public ui::complete_strategy_t
{
public:
	std::string no_results_message() override { return {}; }
	void selected(const ui::auto_complete_match_ptr&, select_type) override {}
	ui::auto_complete_match_ptr selected() const override { return {}; }
	void search(const std::string&, std::function<void(const ui::auto_complete_results&)> complete) override
	{
		complete({});
	}
	void initialise(std::function<void(const ui::auto_complete_results&)> complete) override { complete({}); }
};

// A frame that cannot make the frames asked of it, as when creating a window fails.
class null_list_frame_parent final : public ui::null_frame
{
public:
	ui::frame_ptr create_frame(ui::frame_host_weak_ptr, const ui::frame_style&) override { return nullptr; }
};

// A search whose results are given, which remembers which was picked and how.
class given_results_strategy final : public ui::complete_strategy_t
{
public:
	ui::auto_complete_results results;
	ui::auto_complete_match_ptr picked;
	select_type how = select_type::init;

	given_results_strategy() { auto_select_first = false; }

	std::string no_results_message() override { return {}; }

	void selected(const ui::auto_complete_match_ptr& m, const select_type st) override
	{
		picked = m;
		how = st;
	}

	ui::auto_complete_match_ptr selected() const override { return picked; }

	void search(const std::string&, std::function<void(const ui::auto_complete_results&)> complete) override
	{
		complete(results);
	}

	void initialise(std::function<void(const ui::auto_complete_results&)> complete) override { complete(results); }
};

// A result picked with a press, as a folder in Copy/Move's search or a program in Open with's is.
class pickable_match final : public ui::auto_complete_match, public std::enable_shared_from_this<pickable_match>
{
public:
	ui::complete_strategy_t& _parent;

	explicit pickable_match(ui::complete_strategy_t& parent) :
		auto_complete_match(view_element_style::can_invoke), _parent(parent)
	{
	}

	std::string edit_text() const override { return "result"; }

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::click)
		{
			_parent.selected(shared_from_this(), ui::complete_strategy_t::select_type::click);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

class slot_test_texture final : public render::texture
{
public:
	int source_index = -1;
	bool fail_update = false;

	slot_test_texture(const int source_index_in, const bool fail_update_in) :
		source_index(source_index_in), fail_update(fail_update_in)
	{
	}

	render::texture_update_result update(const render::video_frame_ptr&) override
	{
		return render::texture_update_result::failed;
	}

	render::texture_update_result update(const core::const_surface_ptr& surface) override
	{
		if (fail_update) return render::texture_update_result::failed;
		_dimensions = surface->dimensions();
		_src_extent = surface->dimensions();
		_orientation = surface->orientation();
		_format = surface->format();
		return render::texture_update_result::tex_created;
	}

	render::texture_update_result update(sizei dims, core::texture_format format, core::orientation orientation,
	                                     const uint8_t*, size_t, size_t) override
	{
		if (fail_update) return render::texture_update_result::failed;
		_dimensions = dims;
		_src_extent = dims;
		_orientation = orientation;
		_format = format;
		return render::texture_update_result::tex_created;
	}

	bool is_valid() const override { return !fail_update; }
};

class slot_test_draw_context final : public render::draw_context
{
public:
	struct draw_call
	{
		int source_index = -1;
		recti destination;
		recti source;
	};

	std::vector<bool> fail_uploads;
	std::vector<draw_call> draws;
	std::vector<recti> rectangles;
	std::vector<recti> text_bounds;
	std::vector<std::string> text_values;
	std::vector<recti> highlighted_text_bounds;
	recti clip;
	int created = 0;

	explicit slot_test_draw_context(std::vector<bool> fail_uploads_in) : fail_uploads(std::move(fail_uploads_in))
	{
		colors.alpha = 1.0f;
	}

	void clear(core::color) override {}
	void draw_rounded_rect(recti, core::color, int) override {}
	void draw_polygon(std::span<const core::pointd>, const render::polygon_style&) override {}
	void draw_rect(const recti bounds, core::color) override { rectangles.emplace_back(bounds); }
	void draw_line(core::pointd, core::pointd, core::color, double) override {}
	void draw_polyline(std::span<const core::pointd>, core::color, double) override {}
	void draw_ellipse(recti, core::color, double) override {}
	void fill_ellipse(recti, core::color) override {}
	void draw_text(const std::string_view text, const recti bounds, render::font_face, render::text_style, core::color,
	               core::color) override
	{
		text_bounds.emplace_back(bounds);
		text_values.emplace_back(text);
	}
	void draw_text(std::string_view, const std::vector<render::text_highlight_t>&, const recti bounds,
	               render::font_face, render::text_style, core::color, core::color) override
	{
		highlighted_text_bounds.emplace_back(bounds);
	}
	void draw_text(const render::text_layout_ptr&, recti, core::color, core::color) override {}
	void draw_border(recti, recti, core::color, core::color) override {}
	void draw_texture(const render::texture_ptr& t, recti dst, float, render::texture_sampler) override
	{
		const auto texture = std::dynamic_pointer_cast<slot_test_texture>(t);
		draws.push_back({texture ? texture->source_index : -1, dst, recti({}, t->dimensions())});
	}
	void draw_texture(const render::texture_ptr& t, recti dst, recti src, float, render::texture_sampler,
	                  float) override
	{
		const auto texture = std::dynamic_pointer_cast<slot_test_texture>(t);
		draws.push_back({texture ? texture->source_index : -1, dst, src});
	}
	void draw_texture(const render::texture_ptr& t, const quadd& dst, recti src, float,
	                  render::texture_sampler) override
	{
		const auto texture = std::dynamic_pointer_cast<slot_test_texture>(t);
		draws.push_back({texture ? texture->source_index : -1, dst.bounding_rect_i(), src});
	}
	void draw_texture(const render::texture_ptr& t, const quadd& dst, recti src, float, render::texture_sampler,
	                  const render::texture_transform&) override
	{
		const auto texture = std::dynamic_pointer_cast<slot_test_texture>(t);
		draws.push_back({texture ? texture->source_index : -1, dst.bounding_rect_i(), src});
	}
	void draw_vertices(const render::vertices_ptr&) override {}
	sizei measure_text(std::string_view text, render::font_face, render::text_style, int width, int = 0) override
	{
		return {std::min(width, static_cast<int>(text.size()) * 8), 20};
	}
	int text_line_height(render::font_face) override { return 20; }
	render::texture_ptr create_texture() override
	{
		const auto index = created++;
		const auto fail = static_cast<size_t>(index) < fail_uploads.size() && fail_uploads[index];
		return std::make_shared<slot_test_texture>(index, fail);
	}
	render::vertices_ptr create_vertices() override { return {}; }
	render::text_layout_ptr create_text_layout(render::font_face) override { return {}; }
	recti clip_bounds() const override { return clip; }
	void clip_bounds(recti) override {}
	void restore_clip() override {}
};

static void should_keep_video_thumbnails_clear_of_playback_progress_overlays()
{
	const auto animations = render::animations_enabled;
	const auto debug_info = setting.show_debug_info;
	const core::scope_exit restore([=]
	{
		render::animations_enabled = animations;
		setting.show_debug_info = debug_info;
	});
	render::animations_enabled = false;
	setting.show_debug_info = false;
	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	df::index_file_item indexed;
	indexed.name = core::cache("resume.mp4");
	indexed.ft = files::file_type_from_name(indexed.name);
	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->duration = 120;
	metadata->width = 1280;
	metadata->height = 720;
	indexed.metadata.store(metadata);
	const auto item = std::make_shared<df::item_element>(core::file_path("c:\\resume.mp4"), indexed);
	item->thumbnail(make_valid_test_image(), {}, {});
	item->stage_thumbnail_surface(as, false, true);
	assert_equal(true, as.run_next(async_queue::thumbnail_stage), "the video thumbnail is decoded before drawing");
	as.drain_ui();
	assert_equal(true, item->has_cached_surface(), "the test draws actual thumbnail pixels rather than a placeholder");
	df::item_group group(state, {item}, df::item_group_display::icons, {});
	for (const auto scale : {1, 2})
	{
		slot_test_draw_context dc({});
		dc.scale_factor = scale;
		dc.clip = {0, 0, 1000 * scale, 1000 * scale};
		group.bounds = dc.clip;
		item->bounds = {100 * scale, 100 * scale, 260 * scale, 190 * scale};
		item->media_position(0);
		item->render(dc, group, {});
		assert_equal(1_z, dc.draws.size(), "the video thumbnail is still drawn");
		const auto baseline = dc.rectangles;
		const auto destination = dc.draws.front().destination;
		for (const auto position : {12.0, 60.0, 119.0})
		{
			dc.draws.clear();
			dc.rectangles.clear();
			item->media_position(position);
			item->render(dc, group, {});
			assert_equal(baseline.size(), dc.rectangles.size(), "saved playback position adds no overlay rectangles");
			assert_equal(true, baseline == dc.rectangles,
			             "video thumbnail drawing is independent of playback position");
			assert_equal(1_z, dc.draws.size(), "removing the progress overlay does not remove the thumbnail");
			assert_equal(true, destination == dc.draws.front().destination,
			             "the thumbnail's full media bounds are preserved");
			assert_equal(position, item->media_position(), "the saved position remains available for playback resume");
		}
	}
}

// A hex dump draws only the rows that meet the clip. One wholly below the clip once drew a row named
// by a negative index, read from before its data.
static void should_draw_only_hex_rows_inside_the_clip()
{
	std::vector<uint8_t> bytes(256);
	for (auto i = 0_z; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i);

	const auto hex = std::make_shared<document_control>(bytes, flex_item::center);
	slot_test_draw_context dc({});
	hex->measure(dc, 2000);
	const recti bounds{0, 400, 2000, 800};
	hex->layout(dc, bounds);

	dc.clip = {0, 0, 2000, 300};
	hex->render(dc, {});
	assert_equal(true, dc.highlighted_text_bounds.empty(), "nothing is drawn for a dump below the clip");

	dc.clip = bounds;
	hex->render(dc, {});
	assert_equal(false, dc.highlighted_text_bounds.empty(), "a dump inside the clip is drawn");

	for (const auto& row : dc.highlighted_text_bounds)
	{
		assert_equal(true, row.top >= bounds.top, "every row is the dump's own");
	}
}

// Records what a document draws: each row's text and highlights, and the rectangles filled behind it.
class document_test_draw_context final : public render::draw_context
{
public:
	struct text_call
	{
		std::string text;
		std::vector<render::text_highlight_t> highlights;
		recti bounds;
	};

	std::vector<text_call> texts;
	// What a formatted view draws, a run at a time, in the colour it draws it.
	std::vector<std::pair<std::string, core::color>> runs;
	std::vector<std::pair<recti, core::color>> rects;
	recti clip{0, 0, 4000, 4000};

	document_test_draw_context() { colors.alpha = 1.0f; }

	void clear(core::color) override {}
	void draw_rounded_rect(recti, core::color, int) override {}
	void draw_polygon(std::span<const core::pointd>, const render::polygon_style&) override {}
	void draw_rect(const recti bounds, const core::color c) override { rects.emplace_back(bounds, c); }
	void draw_line(core::pointd, core::pointd, core::color, double) override {}
	void draw_polyline(std::span<const core::pointd>, core::color, double) override {}
	void draw_ellipse(recti, core::color, double) override {}
	void fill_ellipse(recti, core::color) override {}
	void draw_text(const std::string_view text, recti, render::font_face, render::text_style, const core::color c,
	               core::color) override
	{
		runs.emplace_back(std::string(text), c);
	}
	void draw_text(const std::string_view text, const std::vector<render::text_highlight_t>& highlights,
	               const recti bounds, render::font_face, render::text_style, core::color, core::color) override
	{
		texts.push_back({std::string(text), highlights, bounds});
	}
	void draw_text(const render::text_layout_ptr&, recti, core::color, core::color) override {}
	void draw_border(recti, recti, core::color, core::color) override {}
	void draw_texture(const render::texture_ptr&, recti, float, render::texture_sampler) override {}
	void draw_texture(const render::texture_ptr&, recti, recti, float, render::texture_sampler, float) override {}
	void draw_texture(const render::texture_ptr&, const quadd&, recti, float, render::texture_sampler) override {}
	void draw_texture(const render::texture_ptr&, const quadd&, recti, float, render::texture_sampler,
	                  const render::texture_transform&) override
	{
	}
	void draw_vertices(const render::vertices_ptr&) override {}
	sizei measure_text(const std::string_view text, render::font_face, render::text_style, const int width,
	                   int = 0) override
	{
		return {std::min(width, static_cast<int>(text.size()) * 8), 20};
	}
	int text_line_height(render::font_face) override { return 20; }
	render::texture_ptr create_texture() override { return {}; }
	render::vertices_ptr create_vertices() override { return {}; }
	render::text_layout_ptr create_text_layout(render::font_face) override { return {}; }
	recti clip_bounds() const override { return clip; }
	void clip_bounds(recti) override {}
	void restore_clip() override {}
};

// The panel's document for a display whose bytes are given, decoded as the load decodes them.
struct document_test_display
{
	deferred_async_strategy async;
	common_display_state_t common;
	display_state_ptr display;

	explicit document_test_display(const std::string_view bytes, const std::string_view extension = ".txt")
	{
		display = std::make_shared<display_state_t>(async, common);
		display->_selected_item_data = core::blob(bytes.begin(), bytes.end());
		display->_document = std::make_shared<document_state>(
			decode_document(display->_selected_item_data, extension, true));
	}
};

static core::cspan document_test_bytes(const std::string_view s)
{
	return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

static std::string document_test_lines(const document_text& text)
{
	std::string result;

	for (auto i = 0_z; i < text.lines.size(); ++i)
	{
		if (i > 0) result += '|';
		result += text.line(i);
	}

	return result;
}

// The head of a file reads as text in the encoding its bytes name: a byte order mark first, then
// UTF-8 where the bytes are valid as such, and Windows-1252 where they are not. Every kind of line
// break ends a line, and a character the end of the head cut short is left out rather than garbled.
static void should_read_a_document_in_its_encoding()
{
	const auto utf8 = decode_document(document_test_bytes("one\r\ntwo\nthree\rfour"), ".txt", true);
	assert_equal(true, utf8.encoding == document_encoding::utf8, "plain UTF-8");
	assert_equal("one|two|three|four", document_test_lines(utf8), "CRLF, LF and CR each end a line");

	const auto marked = decode_document(document_test_bytes("\xEF\xBB\xBF"
	                                                        "caf\xC3\xA9\n"),
	                                    ".txt", true);
	assert_equal(true, marked.encoding == document_encoding::utf8, "UTF-8 with its byte order mark");
	assert_equal("caf\xC3\xA9|", document_test_lines(marked),
	             "the mark is not text; a final break leaves an empty line");

	const auto cut = decode_document(document_test_bytes("price \xE2\x82"), ".txt", true);
	assert_equal(true, cut.encoding == document_encoding::utf8, "a sequence the end cuts short is still UTF-8");
	assert_equal("price ", document_test_lines(cut), "and is left out");

	const auto ansi = decode_document(document_test_bytes("caf\xE9 \x80 \x93quoted\x94"), ".txt", true);
	assert_equal(true, ansi.encoding == document_encoding::windows_1252, "bytes that are not UTF-8");
	assert_equal("caf\xC3\xA9 \xE2\x82\xAC \xE2\x80\x9Cquoted\xE2\x80\x9D", document_test_lines(ansi),
	             "Windows-1252's euro and quotation marks");

	// "hi", a break and U+1F600, which UTF-16 spells as a surrogate pair.
	const auto le = decode_document(document_test_bytes(std::string_view("\xFF\xFEh\0i\0\n\0\x3D\xD8\x00\xDE", 12)),
	                                ".txt", true);
	assert_equal(true, le.encoding == document_encoding::utf16le, "little-endian UTF-16");
	assert_equal("hi|\xF0\x9F\x98\x80", document_test_lines(le), "a surrogate pair is one character");

	const auto be = decode_document(document_test_bytes(std::string_view("\xFE\xFF\0h\0i\xD8\x3D\xDE\x00", 10)), ".txt",
	                                true);
	assert_equal(true, be.encoding == document_encoding::utf16be, "big-endian UTF-16");
	assert_equal("hi\xF0\x9F\x98\x80", document_test_lines(be), "read high byte first");

	const auto split = decode_document(document_test_bytes(std::string_view("\xFF\xFEh\0\x3D\xD8", 6)), ".txt", true);
	assert_equal("h", document_test_lines(split), "a lead surrogate the end separated from its trail is dropped");

	assert_equal(true,
	             decode_document(document_test_bytes("caf\xC0\x80"), ".txt", true).encoding ==
	                 document_encoding::windows_1252,
	             "an overlong form is not UTF-8");
}

// Bytes that are not text stay the dump they always were: a NUL is enough without a byte order mark,
// and so are control characters past one byte in a hundred. Tabs, form feeds and the escapes of a
// coloured log are text.
static void should_show_bytes_that_are_not_text_as_a_dump()
{
	assert_equal(false, decode_document(document_test_bytes(std::string_view("text\0more", 9)), ".txt", true).is_text(),
	             "a NUL");

	std::string controls(200, 'a');
	controls[10] = '\x01';
	controls[20] = '\x02';
	assert_equal(true, decode_document(document_test_bytes(controls), ".txt", true).is_text(),
	             "one control byte in a hundred");
	controls[30] = '\x03';
	assert_equal(false, decode_document(document_test_bytes(controls), ".txt", true).is_text(), "more than that");

	assert_equal(true, decode_document(document_test_bytes("\x1b[31mred\x1b[0m\tx\f\v\b\n"), ".log", true).is_text(),
	             "a log's colour escapes and the other controls text holds");
	assert_equal(false,
	             decode_document(document_test_bytes(std::string_view("\xFF\xFE\x01\0\x02\0\x03\0", 8)), ".txt", true)
	                 .is_text(),
	             "UTF-16 that decodes to control characters");
	assert_equal(false,
	             decode_document(document_test_bytes(std::string_view("\xFF\xFE\0\0a\0\0\0", 8)), ".txt", true)
	                 .is_text(),
	             "UTF-32, whose mark UTF-16 shares, is a dump");
	assert_equal(false, decode_document(document_test_bytes("plain words"), ".prg", false).is_text(),
	             "a program shown as a listing is never text");

	// With a byte order mark a NUL is one more control, so a log padded with a few is still read.
	std::string padded = "\xEF\xBB\xBF" + std::string(200, 'a');
	padded += '\0';
	assert_equal(true, decode_document(document_test_bytes(padded), ".log", true).is_text(),
	             "a marked file with a NUL among two hundred characters");
	assert_equal(false,
	             decode_document(document_test_bytes(std::string_view(padded).substr(3)), ".log", true).is_text(),
	             "is a dump without its mark");
}

// Text wraps after the last space that fits, so a row ends with a whole word; a word wider than the
// row breaks where it must. Tabs reach the next stop of four, wide characters take two columns, and
// spaces at the edge hang past it rather than starting the next row.
static void should_wrap_document_text_at_word_boundaries()
{
	document_state doc(decode_document(
		document_test_bytes(
			"the quick brown fox\n\nabcdefghijkl\naaaa bbbbb    cc\n\tx\n\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97"),
		".txt", true));

	const auto rows = [&doc]
	{
		std::string result;
		for (auto i = 0_z; i < doc.row_count(); ++i)
		{
			if (i > 0) result += '|';
			result += doc.row_text(i, {}).display;
		}
		return result;
	};

	doc.layout(10, 0);
	assert_equal("the quick |brown fox||abcdefghij|kl|aaaa bbbbb    |cc|    x|\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97",
	             rows(), "ten columns");
	assert_equal(2, doc.row_line(4), "a wrapped row belongs to its line");
	assert_equal(1, static_cast<int>(doc.row_of({0, 10})), "a position at a wrap starts the next row");
	assert_equal(0, static_cast<int>(doc.row_of({0, 9})), "and one before it ends the row");

	doc.layout(5, 0);
	assert_equal(true, rows().ends_with("|\xE4\xB8\xAD\xE6\x96\x87|\xE5\xAD\x97"),
	             "two wide characters fill five columns");

	// The word that reaches the edge moves to the next row whole, and a tab reaches the next stop of
	// four from wherever it starts.
	document_state words(decode_document(document_test_bytes("one two three\nab\tc"), ".txt", true));
	words.layout(10, 0);
	assert_equal("one two ", words.row_text(0, {}).display, "the row ends after the last whole word");
	assert_equal("three", words.row_text(1, {}).display, "and the next starts with the word that did not fit");
	assert_equal("ab  c", words.row_text(2, {}).display, "a tab after two characters takes two columns");

	// A column maps to the nearer side of the character it lands on; a tab and a wide character are
	// each one character however many columns they take.
	document_state tabs(decode_document(document_test_bytes("\tab\xE4\xB8\xADz"), ".txt", true));
	tabs.layout(40, 0);
	assert_equal(0, tabs.position_at(0, 1.9, {}).offset, "inside the tab, nearer its start");
	assert_equal(1, tabs.position_at(0, 2.1, {}).offset, "inside the tab, nearer its end");
	assert_equal(3, tabs.position_at(0, 6.4, {}).offset, "on the first half of the wide character");
	assert_equal(6, tabs.position_at(0, 7.5, {}).offset, "on its second half");
	assert_equal(7, tabs.position_at(0, 100.0, {}).offset, "past the end of the line");

	// A double click wants the character under the pointer instead: the right half of a character is
	// still that character, and past the end of the row is its last one.
	assert_equal(0, tabs.character_at(0, 2.1, {}).offset, "on the tab, past its middle");
	assert_equal(3, tabs.character_at(0, 7.5, {}).offset, "on the second half of the wide character");
	assert_equal(6, tabs.character_at(0, 100.0, {}).offset, "past the end of the line, its last character");

	// Spaces past the edge hang no further than a row holds, and characters that take no column still
	// make rows of a bounded length: a line of either was one row of any size, drawn and hit tested
	// whole on every paint.
	document_state spaces(decode_document(document_test_bytes(std::string(100, ' ') + "x"), ".txt", true));
	spaces.layout(10, 0);
	auto widest = 0_z;
	for (auto i = 0_z; i < spaces.row_count(); ++i) widest = std::max(widest, spaces.row_text(i, {}).display.size());
	assert_equal(20, static_cast<int>(widest), "a run of spaces hangs a row's width past the edge at most");
	assert_equal("x", spaces.row_text(spaces.row_count() - 1, {}).display, "and the text after it follows");

	std::string joiners;
	for (auto i = 0; i < 1000; ++i) joiners += "\xE2\x80\x8B";
	document_state zero_width(decode_document(document_test_bytes(joiners), ".txt", true));
	zero_width.layout(10, 0);
	auto longest = 0_z;
	for (auto i = 0_z; i < zero_width.row_count(); ++i)
	{
		longest = std::max(longest, zero_width.row_text(i, {}).cells.size() - 1);
	}
	assert_equal(true, zero_width.row_count() > 1, "characters that take no column still wrap");
	assert_equal(104, static_cast<int>(longest), "at four characters a column, and a few more");
}

// A copy takes the source text between the ends of the selection - tabs as tabs, not the spaces they
// are drawn as - with Windows line breaks between its lines, however the lines wrap.
static void should_copy_a_document_selection_as_its_source_text()
{
	document_state doc(decode_document(document_test_bytes("first\tline\nsecond line\n\nlast"), ".txt", true));
	doc.layout(8, 0);

	doc.select({0, 3}, {1, 6});
	assert_equal("st\tline\r\nsecond", doc.selected_text({}), "across a line break, keeping the tab");

	doc.select({3, 2}, {0, 6});
	assert_equal("line\r\nsecond line\r\n\r\nla", doc.selected_text({}), "a selection made backwards");

	doc.select_all({});
	assert_equal("first\tline\r\nsecond line\r\n\r\nlast", doc.selected_text({}), "everything");

	doc.select_word({1, 9}, {});
	assert_equal("line", doc.selected_text({}), "a word");

	doc.select_word({0, 5}, {});
	assert_equal("\t", doc.selected_text({}), "a character that is not part of a word");

	document_state accented(decode_document(document_test_bytes("say na\xC3\xAFve_x2 now"), ".txt", true));
	accented.layout(40, 0);
	accented.select_word({0, 6}, {});
	assert_equal("na\xC3\xAFve_x2", accented.selected_text({}),
	             "a word holds underscores, digits and accented letters");

	doc.clear_selection();
	assert_equal("", doc.selected_text({}), "nothing selected copies nothing");
}

// A dump's selection copies its rows as they are drawn, without the padding that lines them up. Laid
// out at a new width, the dump's columns name other bytes, so the selection goes.
static void should_copy_a_dump_selection_as_its_rows()
{
	const std::vector<uint8_t> bytes = {'H', 'i', 0x00, 0xFF, 'a', 'b'};
	const core::cspan span{bytes.data(), bytes.size()};
	document_state doc;
	doc.layout(4, bytes.size());
	assert_equal(2, static_cast<int>(doc.row_count()), "two rows of four bytes");

	doc.select_all(span);
	assert_equal("00000000  48 69 00 FF Hi..\r\n00000004  61 62       ab", doc.selected_text(span), "every row");

	doc.select({0, 10}, {0, 21});
	assert_equal("48 69 00 FF", doc.selected_text(span), "the hex of the first row");

	doc.select_word({1, 14}, span);
	assert_equal("62", doc.selected_text(span), "a double click takes the run of characters it lands in");

	doc.layout(8, bytes.size());
	assert_equal(false, doc.has_selection(), "a new width clears the selection");

	// A selection of nothing but the padding that lines the rows up has nothing to copy, and Copy then
	// leaves the clipboard alone rather than emptying it.
	doc.layout(4, bytes.size());
	doc.select({1, 16}, {1, 21});
	assert_equal(true, doc.has_selection() && doc.selected_text(span).empty(), "padding alone copies nothing");
}

// A block comment opened on one line colours the lines it runs on, so the state each line starts in is
// recorded once, as the document is read: drawing any row then needs only its own line.
static void should_record_the_state_each_document_line_starts_in()
{
	const auto cpp = decode_document(document_test_bytes("int a; /* open\nstill comment\nclosed */ int b;\nint c;"),
	                                 ".cpp", true);
	assert_equal(true, cpp.language == ui::syntax::language::cpp, "C++ by its extension");
	assert_equal(4, static_cast<int>(cpp.cookies.size()), "a state for every line");
	assert_equal(true, cpp.cookies[0] == 0 && cpp.cookies[1] != 0 && cpp.cookies[2] != 0 && cpp.cookies[3] == 0,
	             "inside the comment and out of it");

	const auto md = decode_document(document_test_bytes("# Title\n- item"), ".MD", true);
	assert_equal(true, md.language == ui::syntax::language::markdown, "Markdown by its extension, in any case");
	assert_equal(true, md.cookies.empty(), "Markdown carries nothing from line to line");

	const auto plain = decode_document(document_test_bytes("/* not code */"), ".txt", true);
	assert_equal(true, plain.language == ui::syntax::language::plain && plain.cookies.empty(),
	             "plain text is not coloured");
}

// Pressing on the panel's document and dragging selects its text from the press to the pointer; a
// double click selects a word and Shift extends what is selected. A drag that leaves the document
// selects to the edge it left by, and Escape ends the drag without dropping what it selected.
static void should_select_document_text_by_dragging()
{
	document_test_display test("alpha beta\ngamma");
	const auto control = std::make_shared<document_control>(test.display, flex_item::center);
	document_test_draw_context dc;
	assert_equal(70, control->measure(dc, 400).cy, "two rows and the common panel and leaf insets");
	control->layout(dc, recti(0, 100, 400, 172));
	const auto content = control->content_bounds();
	const auto at = [&content](const int x, const int y)
	{
		return content.top_left() + pointi{x, y};
	};

	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {400, 400};
	hit_test_context ctx(at(8, 10), recti(0, 0, 400, 400));
	const auto controller = control->controller_from_location(host, at(8, 10), {}, ctx);
	assert_equal(true, controller != nullptr, "the document takes a press");
	assert_equal(true, controller->cursor() == platform::cursor::text_select, "under a text cursor");

	const auto& doc = *test.display->_document;
	const auto& bytes = test.display->_selected_item_data;

	controller->on_mouse_left_button_down(at(8, 10), {});
	controller->on_mouse_move(at(24, 34));
	controller->on_mouse_left_button_up(at(24, 34), {});
	assert_equal("lpha beta\r\ngam", doc.selected_text(bytes), "from the press to the release");
	assert_equal(true, doc.focused, "the press gives the document the keyboard");

	controller->on_mouse_left_button_double_click(at(56, 10), {});
	assert_equal("beta", doc.selected_text(bytes), "a double click selects a word");

	// The word of the character under the pointer: the right half of a word's last letter is nearer the
	// space after it, and past the end of a line is its last word.
	controller->on_mouse_left_button_double_click(at(38, 10), {});
	assert_equal("alpha", doc.selected_text(bytes), "a double click on the right half of a word's last letter");
	controller->on_mouse_left_button_double_click(at(300, 10), {});
	assert_equal("beta", doc.selected_text(bytes), "and past the end of the line");

	platform::key_state shift;
	shift.shift = true;
	controller->on_mouse_left_button_down(at(40, 34), shift);
	controller->on_mouse_left_button_up(at(40, 34), shift);
	assert_equal("beta\r\ngamma", doc.selected_text(bytes), "Shift extends from the anchor");

	controller->on_mouse_left_button_down(at(0, 10), {});
	controller->on_mouse_move({200, 1000});
	assert_equal(true, controller->escape(), "Escape ends a drag in progress");
	controller->on_mouse_left_button_up({200, 1000}, {});
	assert_equal("alpha beta\r\ngamma", doc.selected_text(bytes), "a drag below the document selects to its end");
	assert_equal(false, controller->escape(), "with no drag there is nothing to end");
	assert_equal(true, host->invalidations > 0, "each change repaints");

	// The wheel turns the pane under a drag, and the selection follows the text under the pointer, not
	// where that text was when the press landed.
	controller->on_mouse_left_button_down(at(0, 10), {});
	test.display->media_offset = {0, -24};
	controller->on_mouse_move(at(40, 10));
	controller->on_mouse_left_button_up(at(40, 10), {});
	assert_equal("alpha beta\r\ngamma", doc.selected_text(bytes), "a drag continues over the scrolled text");
	test.display->media_offset = {};

	// A metadata block's dump is rebuilt with the panel, so it offers no selection to lose.
	const auto dump = std::make_shared<document_control>(std::vector<uint8_t>{1, 2, 3}, flex_item::center);
	dump->measure(dc, 400);
	dump->layout(dc, recti(0, 0, 400, 48));
	hit_test_context dump_ctx({8, 8}, recti(0, 0, 400, 400));
	assert_equal(true, dump->controller_from_location(host, {8, 8}, {}, dump_ctx) == nullptr,
	             "a dump of its own bytes is not selected");

	control->measure(dc, 0);
	assert_equal(8, test.display->_document->columns(), "a pane with no width yet still wraps at eight characters");
}

// A press anywhere in the view takes the keyboard from the panel's document before any controller sees
// it - a press on the document gives it back - and Escape clears a selection the document holds while
// it has the keyboard, before anything else unwinds.
static void should_release_the_document_on_a_press_elsewhere()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	items_view items(state, nullptr);

	common_display_state_t common;
	items._display = std::make_shared<display_state_t>(as, common);
	items._display->_document = std::make_shared<document_state>(
		decode_document(document_test_bytes("some text"), ".txt", true));
	auto& doc = *items._display->_document;
	doc.layout(40, 0);
	doc.select({0, 0}, {0, 4});
	doc.focused = true;

	assert_equal(true, items.escape(), "Escape clears the document's selection");
	assert_equal(false, doc.has_selection(), "which is gone");
	assert_equal(false, items.escape(), "and then has nothing of the document's to clear");

	doc.select({0, 0}, {0, 4});
	items.mouse_down({10, 10});
	assert_equal(false, doc.focused, "a press gives the keyboard back to the items");
	assert_equal(true, doc.has_selection(), "and leaves the selection to be seen");
	assert_equal(false, items.escape(), "a selection without the keyboard is not Escape's to clear");

	// A menu opened on the media pane may copy the document's text; one opened on the items is about
	// them, and so is whatever view replaces this one.
	items._regions.media = {0, 0, 500, 400};
	items._regions.items = {500, 0, 1000, 400};
	doc.focused = true;
	items.context_menu({100, 100});
	assert_equal(true, doc.focused, "a menu on the media pane leaves the document the keyboard");
	items.context_menu({700, 100});
	assert_equal(false, doc.focused, "a menu on the items takes it back");

	doc.focused = true;
	items.deactivate();
	assert_equal(false, doc.focused, "and so does leaving the view");
}

// The selection is drawn behind its text, a column wider at the end of a line whose break it takes,
// and the text it covers loses its syntax colour so that it reads on the selection.
static void should_draw_the_document_selection_behind_its_text()
{
	document_test_display test("int a;\nint b;", ".cpp");
	const auto control = std::make_shared<document_control>(test.display, flex_item::center);
	document_test_draw_context dc;
	control->measure(dc, 400);
	control->layout(dc, recti(0, 100, 400, 172));
	const auto origin = control->content_bounds().top_left();

	control->render(dc, {});
	assert_equal(true, dc.rects.empty(), "nothing selected, nothing filled");
	assert_equal(2, static_cast<int>(dc.texts.size()), "a call a row");
	assert_equal(true,
	             !dc.texts[1].highlights.empty() && dc.texts[1].highlights.front().offset == 0 &&
	                 dc.texts[1].highlights.front().length == 3,
	             "the keyword is coloured");

	test.display->_document->select({0, 4}, {1, 3});
	dc.texts.clear();
	control->render(dc, {});
	assert_equal(2, static_cast<int>(dc.rects.size()), "a rectangle on each row");
	assert_equal(true, dc.rects[0].first == recti(32, 0, 56, 24).offset(origin),
	             "to the end of the first line, and its break");
	assert_equal(true, dc.rects[1].first == recti(0, 24, 24, 48).offset(origin), "from the start of the second");
	assert_equal(true, dc.texts[1].highlights.empty(), "the selected keyword is drawn in the text colour");

	// No font draws a control character, so one is shown as a middle dot in the control colour.
	document_test_display escapes("a\x1b[1mb");
	const auto escaped = std::make_shared<document_control>(escapes.display, flex_item::center);
	escaped->measure(dc, 400);
	escaped->layout(dc, recti(0, 0, 400, 48));
	dc.texts.clear();
	escaped->render(dc, {});
	assert_equal("a\xC2\xB7[1mb", dc.texts[0].text, "a control character is drawn as a middle dot");
	assert_equal(true,
	             dc.texts[0].highlights.size() == 1 && dc.texts[0].highlights[0].offset == 1 &&
	                 dc.texts[0].highlights[0].length == 2,
	             "in the control colour");
}

// A dump colours a control byte apart from a byte past ASCII, in both of its columns. The hex view read
// each byte as a signed char, so every byte past 0x7F took the control colour and only 0x7F its own.
static void should_colour_a_dump_by_byte_class()
{
	const auto control = std::make_shared<document_control>(std::vector<uint8_t>{'A', 0x01, 0xC8, 0x7F},
	                                                        flex_item::center);
	document_test_draw_context dc;
	control->measure(dc, 2000);
	control->layout(dc, recti(0, 0, 2000, 48));
	control->render(dc, {});
	assert_equal(1, static_cast<int>(dc.texts.size()), "one row");

	const auto& highlights = dc.texts[0].highlights;
	const auto color_at = [&highlights](const uint32_t offset) -> std::optional<core::color>
	{
		for (const auto& h : highlights)
		{
			if (offset >= h.offset && offset < h.offset + h.length) return h.clr;
		}
		return std::nullopt;
	};

	// Four bytes a row: their hex at columns 10, 13, 16 and 19, their characters from column 22.
	assert_equal(true, color_at(0).has_value(), "the offset is dimmed");
	assert_equal(false, color_at(10).has_value() || color_at(22).has_value(), "a printable byte is in the text colour");
	assert_equal(true, color_at(13).has_value() && color_at(23).has_value(),
	             "a control byte is coloured in both columns");
	assert_equal(true, color_at(16).has_value() && color_at(24).has_value(), "and so is a byte past ASCII");
	assert_equal(false, *color_at(13) == *color_at(16), "in different colours");
	assert_equal(true, *color_at(16) == *color_at(19) && *color_at(16) == *color_at(25),
	             "0x7F is coloured as the bytes past it");
}

static void should_use_common_document_panels_and_tables()
{
	document_test_draw_context dc;
	const ui::element leaf;
	const auto inset = ui::panel_element::content_padding + leaf.padding.cx;
	for (const auto extension : {".txt", ".md", ".csv", ".bin"})
	{
		const auto binary = std::string_view(extension) == ".bin";
		document_test_display test(binary ? std::string_view("\0\1", 2) : std::string_view("Name,Value\nalpha,42"),
		                           extension);
		const auto control = std::make_shared<document_control>(test.display, flex_item::stretch);
		const auto extent = control->measure(dc, 400);
		control->layout(dc, {50, 100, 450, 100 + extent.cy});
		const auto content = control->content_bounds();
		assert_equal(50 + inset, content.left, "every document uses the same panel and leaf inset");
		assert_equal(100 + inset, content.top, "every document uses the same top panel inset");
		assert_equal(450 - inset, content.right, "every document fills the panel's inner width");
		assert_equal(100 + extent.cy - inset, content.bottom, "every document uses the same bottom panel inset");
		assert_equal(true,
		             control->padding.cx == 0 && control->padding.cy == 0 && control->margin.cx == 0 &&
		                 control->margin.cy == 0,
		             "panels leave external spacing to their shared parent");
	}

	const auto dump = std::make_shared<document_control>(std::vector<uint8_t>{0, 1}, flex_item::stretch);
	const auto dump_extent = dump->measure(dc, 400);
	assert_equal(24 + inset * 2, dump_extent.cy, "a raw dump has one row and the shared panel and leaf insets");
	dump->layout(dc, {50, 100, 450, 100 + dump_extent.cy});
	assert_equal(true, dump->content_bounds() == recti(50 + inset, 100 + inset, 450 - inset, 124 + inset),
	             "metadata dumps use the same inset as decoded file documents");

	document_test_display csv("Name,Value\nalpha,longer value", ".CSV");
	auto panel = std::make_shared<document_control>(csv.display, flex_item::stretch);
	auto extent = panel->measure(dc, 500);
	panel->layout(dc, {0, 0, 500, extent.cy});
	const auto view = std::dynamic_pointer_cast<ui::csv_view>(csv.display->_document->formatted_view);
	assert_equal(true, view != nullptr, "CSV uses the shared document table view");
	const auto original = view->table().col_widths[0];
	const pointi divider{(2 + original + 3) * view->font_extent().cx, view->font_extent().cy / 2};
	view->on_mouse_down(divider, {});
	view->on_mouse_move({divider.x + 40, divider.y}, true);
	view->on_mouse_up({divider.x + 40, divider.y}, {});
	assert_equal(original + 5, view->table().col_widths[0], "the shared CSV divider resizes in the selection panel");
	assert_equal(true, csv.display->_document->focused, "a formatted document press keeps document command targeting");
	assert_equal(false, csv.display->_document->has_selection(), "a table resize is not a text selection");
	csv.display->_document->select_all(csv.display->_selected_item_data);
	panel->render(dc, {});
	assert_equal("Name,Value\r\nalpha,longer value", view->select_text(),
	             "app Select all reaches the shared source selection");
	panel = std::make_shared<document_control>(csv.display, flex_item::stretch);
	extent = panel->measure(dc, 500);
	panel->layout(dc, {0, 0, 500, extent.cy});
	assert_equal(true, csv.display->_document->formatted_view == view,
	             "panel rebuilding keeps the same captured control");
	assert_equal(original + 5, view->table().col_widths[0], "and its user column sizes");

	document_test_display markdown(
		"# Title\nsee [link](https://example.invalid)\n\n| Name | Value |\n| --- | --- |\n| alpha | 42 |", ".md");
	const auto markdown_panel = std::make_shared<document_control>(markdown.display, flex_item::stretch);
	extent = markdown_panel->measure(dc, 500);
	markdown_panel->layout(dc, {0, 0, 500, extent.cy});
	const auto md = std::dynamic_pointer_cast<ui::markdown_view>(markdown.display->_document->formatted_view);
	assert_equal(true, md != nullptr, "Markdown uses the shared rich document view");
	assert_equal("https://example.invalid", md->link_at(md->text_to_client({6, 1})),
	             "Markdown links remain hit-testable");
	int divider_x = -1;
	const auto header_y = md->text_to_client({0, 3}).y + 1;
	for (int x = 0; x < md->view_extent().cx; ++x)
	{
		if (md->cursor_at({x, header_y}) == platform::cursor::left_right)
		{
			divider_x = x;
			break;
		}
	}
	assert_equal(true, divider_x >= 0, "Markdown exposes its common table divider");
	md->on_mouse_down({divider_x, header_y}, {});
	md->on_mouse_move({divider_x + 40, header_y}, true);
	md->on_mouse_up({divider_x + 40, header_y}, {});
	assert_equal(true, md->cursor_at({divider_x + 40, header_y}) == platform::cursor::left_right,
	             "Markdown table resizing moves its divider without editing the source");
	platform::key_state control;
	control.control = true;
	md->on_key('A', control);
	assert_equal(true, markdown.display->_document->has_selection(), "shared keyboard selection reaches document Copy");
	assert_equal(
		"# Title\r\nsee [link](https://example.invalid)\r\n\r\n| Name | Value |\r\n| --- | --- |\r\n| alpha | 42 |",
		markdown.display->_document->selected_text(markdown.display->_selected_item_data),
		"copy retains all Markdown markers and source rows");
}

// A formatted view reads a colour's alpha from its high byte, which the shared palette leaves clear. Given the
// palette's text colour as it is, a Markdown or CSV document drew its headings and none of its ordinary text.
static void should_draw_the_ordinary_text_of_formatted_documents()
{
	for (const auto& [source, extension, word] : {std::tuple{"# Title\nplain words"sv, ".md"sv, "plain"sv},
	                                              std::tuple{"Name,Value\nalpha,42"sv, ".csv"sv, "alpha"sv}})
	{
		document_test_draw_context dc;
		const document_test_display test(source, extension);
		const auto panel = std::make_shared<document_control>(test.display, flex_item::stretch);
		const auto extent = panel->measure(dc, 500);
		panel->layout(dc, {0, 0, 500, extent.cy});
		panel->render(dc, {});

		const std::string_view needle = word;
		const auto drawn = std::ranges::find_if(dc.runs, [needle](const auto& run)
		{ return run.first.find(needle) != std::string::npos; });
		assert_equal(true, drawn != dc.runs.end(), std::format("a {} document draws its ordinary text", extension));
		assert_equal(true, drawn->second.a > 0.99f,
		             std::format("a {} document draws it opaque, not at alpha {}", extension, drawn->second.a));
	}
}

namespace
{
	struct listing_layout final : render::text_layout
	{
		std::string text;
		render::text_style style = render::text_style::single_line;
		void update(const std::string_view value, const render::text_style value_style) override
		{
			text = value;
			style = value_style;
		}
		sizei measure_text(const int width, int = 1000) override
		{
			return {std::min(width, core::utf8_codepoint_count(text) * 8), 20};
		}
		std::vector<int> offset_xs(const std::vector<size_t>& offsets, int, int = 1000) override
		{
			std::vector<int> result;
			result.reserve(offsets.size());
			for (const auto offset : offsets) result.push_back(static_cast<int>(offset) * 8);
			return result;
		}
	};

	class listing_draw_context final : public ui::test::fake_draw_context
	{
	public:
		using ui::test::fake_draw_context::fake_draw_context;
		using ui::test::fake_draw_context::draw_text;

		render::text_layout_ptr create_text_layout(render::font_face) override
		{
			return std::make_shared<listing_layout>();
		}

		void draw_text(const render::text_layout_ptr& value, const recti bounds, const core::color color,
		               const core::color background) override
		{
			const auto layout = std::dynamic_pointer_cast<listing_layout>(value);
			assert_equal(true, layout != nullptr, "the headless text layout is recorded, not silently discarded");
			ui::test::fake_draw_context::draw_text(layout->text, bounds, render::font_face::dialog, layout->style,
			                                       color, background);
		}
	};
}

static void should_use_shared_tables_for_file_listings()
{
	struct listing_measure final : render::measure_context
	{
		std::vector<std::shared_ptr<listing_layout>> layouts;
		sizei measure_text(const std::string_view text, render::font_face, render::text_style, const int width,
		                   int = 0) override
		{
			return {std::min(width, core::utf8_codepoint_count(text) * 8), 20};
		}
		int text_line_height(render::font_face) override { return 20; }
		render::text_layout_ptr create_text_layout(render::font_face) override
		{
			auto result = std::make_shared<listing_layout>();
			layouts.push_back(result);
			return result;
		}
		bool has_text(const std::string_view value) const
		{
			return std::ranges::any_of(layouts, [&](const auto& layout) { return layout->text == value; });
		}
		bool has_text_style(const std::string_view value, const render::text_style style) const
		{
			return std::ranges::any_of(layouts, [&](const auto& layout)
			{ return layout->text == value && layout->style == style; });
		}
	} mc;
	const auto intrinsic_cell = listing_icon_cell(file_type::folder.icon, "first.jpg");
	const auto narrow_cell = intrinsic_cell->measure(mc, 200);
	const auto wide_cell = intrinsic_cell->measure(mc, 800);
	assert_equal(narrow_cell.cx, wide_cell.cx, "a listing cell's natural width does not grow with the viewport");
	assert_equal(narrow_cell.cy, wide_cell.cy, "intrinsic measurement preserves the listing row height");

	const auto table_in = [](const ui::elements& owner)
	{
		std::shared_ptr<ui::table_element> table;
		owner.visit_children([&](const view_element_ptr& child)
		{
			if (const auto value = std::dynamic_pointer_cast<ui::table_element>(child)) table = value;
		});
		return table;
	};
	const auto cells_in = [](const ui::table_element& table)
	{
		int count = 0;
		table.visit_children([&](const view_element_ptr&) { ++count; });
		return count;
	};
	deferred_async_strategy async;
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(async, common);
	display->_archive_items = {{"first.jpg", core::file_size{4096}, core::file_size{2048}, {}}, {"folder", {}, {}, {}}};
	const auto archive = std::make_shared<file_list_control>(display, flex_item::stretch);
	auto extent = archive->measure(mc, 800);
	archive->layout(mc, {0, 0, 800, extent.cy});
	const auto table = table_in(*archive);
	assert_equal(true, table != nullptr, "archive contents use the library table element");
	assert_equal(8, cells_in(*table), "each archive entry retains its four display columns");
	assert_equal(true, mc.has_text("first.jpg") && mc.has_text("folder"), "file and folder names survive the adapter");
	assert_equal(true,
	             mc.has_text(platform::format_size(core::file_size{4096})) &&
	                 mc.has_text(platform::format_size(core::file_size{2048})),
	             "both size columns survive the adapter");
	assert_equal(true,
	             mc.has_text_style(platform::format_size(core::file_size{4096}), render::text_style::single_line_far),
	             "archive sizes remain right aligned");
	const auto width = table->column_widths()[0];
	const auto point = table->divider_bounds(0).center();
	const auto host = std::make_shared<detached_test_host>();
	hit_test_context ctx(point, {0, 0, 800, 400});
	const auto controller = archive->controller_from_location(host, point, {}, ctx);
	assert_equal(true, controller != nullptr && controller->cursor() == platform::cursor::left_right,
	             "an archive's column divider is reachable through its normal element tree");
	controller->on_mouse_left_button_down(point, {});
	controller->on_mouse_move({point.x + 40, point.y});
	controller->on_mouse_left_button_up({point.x + 40, point.y}, {});
	archive->layout(mc, {0, 0, 800, extent.cy});
	assert_equal(width + 40, table->column_widths()[0], "archive columns resize with the common gesture");
	display->_archive_items[0].filename = "updated.jpg";
	archive->dispatch_event({view_element_event_type::populate, {}});
	extent = archive->measure(mc, 800);
	archive->layout(mc, {0, 0, 800, extent.cy});
	assert_equal(true, table_in(*archive) == table, "a listing refresh keeps the same captured table");
	assert_equal(width + 40, table->column_widths()[0], "and its user column width");
	assert_equal(true, mc.has_text("updated.jpg"), "a refresh still publishes updated row contents");

	const auto before_limit = table->column_widths();
	const auto available_width = std::accumulate(before_limit.begin(), before_limit.end(), 0);
	auto maximum_width = available_width;
	for (size_t i = 1; i < before_limit.size(); ++i)
		maximum_width -= std::min(before_limit[i], core::round(24 * mc.scale_factor));
	const auto limit_point = table->divider_bounds(0).center();
	hit_test_context limit_ctx(limit_point, {0, 0, 800, 400});
	const auto limit_controller = archive->controller_from_location(host, limit_point, {}, limit_ctx);
	assert_equal(true, limit_controller != nullptr, "a resized listing keeps its divider reachable");
	limit_controller->on_mouse_left_button_down(limit_point, {});
	limit_controller->on_mouse_move({limit_point.x + 10000, limit_point.y});
	archive->layout(mc, {0, 0, 800, extent.cy});
	assert_equal(maximum_width, table->column_widths()[0],
	             "a listing resize stops where the following columns leave room");
	assert_equal(available_width, std::accumulate(table->column_widths().begin(), table->column_widths().end(), 0),
	             "a bounded listing resize never extends beyond the table");
	assert_equal(true, limit_controller->escape(), "Escape cancels a listing resize at its bound");
	limit_controller->on_mouse_left_button_up({limit_point.x + 10000, limit_point.y}, {});
	archive->layout(mc, {0, 0, 800, extent.cy});
	assert_equal(width + 40, table->column_widths()[0], "cancelled release restores the listing's user width");

	// A reader that cannot tell an entry's packed size - libarchive's - gives the listing no column for it.
	display->_archive_items = {{"first.jpg", core::file_size{4096}, {}, core::date_t(2024, 7, 13, 14, 47, 6)}};
	archive->dispatch_event({view_element_event_type::populate, {}});
	assert_equal(3_z, table->headings().size(), "no packed size column where none is known");
	assert_equal(3, cells_in(*table), "and no empty cells for it");
	extent = archive->measure(mc, 800);
	archive->layout(mc, {0, 0, 800, extent.cy});
	assert_equal(true, mc.has_text(prop::format_date(core::date_t(2024, 7, 13, 14, 47, 6))),
	             "an entry's modified date is listed");

	// A rebuilt stack makes the listing again, and a sort by the packed size carries on once that column is back.
	display->_archive_items = {{"first.jpg", core::file_size{4096}, core::file_size{2048}, {}}};
	display->_table_choices["archive"] = {{}, 3, false};
	const auto rebuilt = table_in(*std::make_shared<file_list_control>(display, flex_item::stretch));
	assert_equal(true,
	             rebuilt != nullptr && rebuilt->headings().size() == 4 && rebuilt->sort_column() == 3 &&
	                 !rebuilt->sort_ascending(),
	             "a rebuilt listing keeps its sort by the packed size");

	df::file_group_histogram groups;
	groups.counts[file_group::folder.id].count = 2;
	const auto summary = std::make_shared<summary_control>(groups, flex_item::stretch);
	extent = summary->measure(mc, 500);
	summary->layout(mc, {0, 0, 500, extent.cy});
	const auto summary_table = table_in(*summary);
	assert_equal(true, summary_table != nullptr, "selection summaries use the same library table");
	assert_equal(3, cells_in(*summary_table), "a summary retains count, type and size columns");
	assert_equal(true, mc.has_text("2"), "the summary retains its actual item count");
	assert_equal(true, mc.has_text_style("2", render::text_style::single_line_far),
	             "summary counts remain right aligned");
	assert_equal(true, summary_table->divider_bounds(0).is_empty() && summary_table->heading_bounds().is_empty(),
	             "a summary is not headed, so its first row draws no dividers that would make it read as a heading");
	summary->dispatch_event({view_element_event_type::populate, {}});
	assert_equal(3, cells_in(*summary_table), "refreshing a summary never duplicates its rows");
}

static void should_offer_the_face_actions_the_face_book_allows_on_a_picture()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(as, common);
	display->_is_one = true;
	display->_item1 = make_thumbnail_test_item("two-faces.jpg", {});
	display->_selected_texture1 = std::make_shared<texture_state>(as, display->_item1);
	const auto path = display->_item1->path();
	const auto store = std::make_shared<faces::face_store>();
	faces::face_record record;
	record.model = faces::model_id();
	record.faces.resize(2);
	record.faces[0].box = {0.08, 0.2, 0.04, 0.18};
	record.faces[1].box = {0.85, 0.80, 0.1, 0.19};
	store->add(path, record);
	store->publish();
	auto& book = index.faces().book();
	const auto overview = std::make_shared<faces::overview>();
	overview->store = store;
	overview->generation = store->generation();
	overview->book_generation = book.generation();
	overview->people.push_back({0, "Mina", {}, {{1, 0.9f, faces::member_kind::suggested}}, {}, 1});
	overview->members[1] = {0, 0, false};
	const auto old_store = faces::active_store();
	const auto old_overview = faces::active_overview();
	const core::scope_exit restore([&]
	{
		faces::set_active_store(old_store);
		faces::set_active_overview(old_overview);
	});
	faces::set_active_store(store);
	faces::set_active_overview(overview);

	const auto host = std::make_shared<detached_test_host>();
	const auto photo = std::make_shared<photo_control>(state, display, host);
	ui::test::fake_draw_context dc;
	dc.scale_metrics(1.0);
	host->_extent = {600, 360};
	photo->bounds = recti(host->_extent);
	display->_selected_texture1->layout(dc, photo->bounds, display->_item1);
	host->hit_test = [&](const view_host_ptr& h, const pointi loc, hit_test_context& ctx)
	{
		return photo->controller_from_location(h, loc, {}, ctx);
	};
	const core::scope_exit detach([&] { host->on_window_destroy(); });
	const auto hover = [&](const size_t face)
	{
		host->on_mouse_move(photo->face_bounds(face, {}).center(), false);
		dc.reset();
		photo->render_faces(dc, {});
	};
	const auto text_bounds = [&](const std::string_view text)
	{
		const auto it = std::ranges::find_if(dc.texts, [&](const auto& t) { return t.text == text; });
		return it == dc.texts.end() ? recti{} : it->clip;
	};
	const auto popup_at = [&](const pointi point)
	{
		host->on_mouse_move(point, false);
		view_hover_element bubble;
		bubble.clear();
		if (host->_active_controller) host->_active_controller->popup_from_location(bubble);
		std::string title;
		bubble.elements->visit_children([&](const view_element_ptr& child)
		{
			if (const auto action = std::dynamic_pointer_cast<action_element>(child)) title = action->text();
		});
		return title;
	};

	assert_equal(2_z, photo->faces_on_picture().size(), "both faces are visible");
	hover(0);
	const auto show = text_bounds(icon_to_utf8(icon_index::album_artist));
	const auto name = text_bounds(icon_to_utf8(icon_index::rename));
	const auto unnamed_box = photo->face_bounds(0, {});
	assert_equal(true,
	             dc.texts.size() == 2 && !show.is_empty() && !name.is_empty() && show.top >= unnamed_box.bottom &&
	                 name.left >= show.right,
	             "an unnamed face shows only the two icons below its outline");
	for (const auto button : photo->_face_buttons)
		assert_equal(button.height(), button.width(), "an icon without a name has a square hit target");
	const auto strip = photo->face_strip();
	assert_equal(true, std::abs(strip.center().x - unnamed_box.center().x) <= 1,
	             "the icons are centred under the face's outline");
	host->on_mouse_move(unnamed_box.center(), false);
	view_hover_element face_bubble;
	face_bubble.clear();
	if (host->_active_controller) host->_active_controller->popup_from_location(face_bubble);
	assert_equal(true,
	             face_bubble.window_bounds == unnamed_box.make_union(strip) && face_bubble.active_bounds == unnamed_box,
	             "the face's bubble stands beside the face and its icons, never over them, and goes as the pointer "
	             "leaves the face");
	host->on_mouse_move({name.center().x, unnamed_box.bottom - 2}, false);
	dc.reset();
	photo->render_faces(dc, {});
	assert_equal(false, text_bounds(icon_to_utf8(icon_index::rename)).is_empty(),
	             "the naming icon stays reachable diagonally from a small face");
	// From high on the face, out past its side on the way down to a button beyond it - drawn between moves, as the
	// window is - the strip stays: it spans the face's whole height, not just its lower half.
	hover(0);
	assert_equal(true, strip.right > unnamed_box.right + 2, "the icons reach past the small face");
	host->on_mouse_move({unnamed_box.right - 1, unnamed_box.top + 1}, false);
	host->on_mouse_move({unnamed_box.right + 2, unnamed_box.top + 2}, false);
	dc.reset();
	photo->render_faces(dc, {});
	assert_equal(tt.faces_name_face.sv(), popup_at(name.center()),
	             "a button beyond the face is reached from high on the face too");
	assert_equal(tt.faces_show_in_view.sv(), popup_at(show.center()), "the Faces icon explains its action on hover");
	assert_equal(tt.faces_name_face.sv(), popup_at(name.center()), "the naming icon explains its action on hover");

	// A scan publishes other pictures' faces, and an overview arrives, while the pointer rests on this one: the faces
	// are read again on the way to its buttons, and the buttons must still be there.
	hover(0);
	store->publish();
	assert_equal(tt.faces_name_face.sv(), popup_at(name.center()),
	             "the buttons stay reachable when the faces are read again while the pointer is on the face");
	dc.reset();
	photo->render_faces(dc, {});
	assert_equal(false, text_bounds(icon_to_utf8(icon_index::rename)).is_empty(), "and stay drawn under the pointer");

	hover(1);
	const auto label = text_bounds("Mina?");
	const auto confirm = text_bounds(icon_to_utf8(icon_index::check));
	const auto reject = text_bounds(icon_to_utf8(icon_index::remove2));
	const auto someone_else = text_bounds(icon_to_utf8(icon_index::rename));
	assert_equal(true,
	             dc.texts.size() == 5 && !text_bounds(icon_to_utf8(icon_index::album_artist)).is_empty() &&
	                 !label.is_empty() && !confirm.is_empty() && !reject.is_empty() && !someone_else.is_empty(),
	             "a suggestion shows its name, with its mark, and the icons that confirm it, rule it out or name it as "
	             "someone else");
	assert_equal(true,
	             label == label.intersection(photo->bounds) && confirm == confirm.intersection(photo->bounds) &&
	                 reject == reject.intersection(photo->bounds) &&
	                 someone_else == someone_else.intersection(photo->bounds),
	             "a face at the bottom right keeps its actions in reach");
	const pointi on_name(label.right - 2, label.center().y);
	assert_equal(std::format("{} \u00b7 Mina?", tt.faces_show_in_view), popup_at(on_name),
	             "hovering the name explains the same navigation action");
	assert_equal(str_format(tt.faces_confirm_person_fmt, "Mina"), popup_at(confirm.center()),
	             "the confirming icon names the person");
	assert_equal(str_format(tt.faces_not_person_fmt, "Mina"), popup_at(reject.center()),
	             "and so does the one ruling them out");
	assert_equal(tt.faces_name_someone_else.sv(), popup_at(someone_else.center()),
	             "and the one naming the face as someone else says so");
	host->on_mouse_left_button_down(on_name, {});
	host->on_mouse_left_button_up(on_name, {});
	assert_equal(true, state._pending_face == 1u, "clicking the name opens Faces on that face");
	hover(1);
	host->on_mouse_left_button_down(confirm.center(), {});
	host->on_mouse_left_button_up(confirm.center(), {});
	const auto confirmed = book.marks(path);
	assert_equal(true,
	             confirmed.size() == 1 && confirmed.front().person && book.people()->person(confirmed.front().person) &&
	                 book.people()->person(confirmed.front().person)->name == "Mina",
	             "confirming on the picture confirms the suggestion in the face book");

	hover(0);
	const std::array batch{faces::placement_batch{0, "Ann", {{path, record.faces.front().box, 0}}}};
	book.place(batch, faces::face_action::assign, book.generation());
	dc.reset();
	photo->render_faces(dc, {});
	assert_equal(true, !text_bounds("Ann").is_empty() && !text_bounds(icon_to_utf8(icon_index::rename)).is_empty(),
	             "a named face shows its new name before a new overview arrives, and can be renamed");

	index.faces().book_opened(core::file_path{}, faces::book_keeping::shared);
	hover(0);
	dc.reset();
	photo->render_faces(dc, {});
	assert_equal(true, dc.texts.size() == 2 && photo->_face_buttons[1].is_empty(),
	             "while another window changes the face book, the face only opens Faces");
}

static void should_draw_image_errors_without_source_dimensions()
{
	for (const auto comparison : {false, true})
	{
		for (const auto extent : {sizei{360, 240}, sizei{1280, 720}})
		{
			null_state_strategy ss;
			deferred_async_strategy as;
			const location_cache locations;
			index_state index(as, locations);
			view_state state(ss, as, index, make_test_player());
			common_display_state_t common;
			const auto display = std::make_shared<display_state_t>(as, common);
			display->_is_one = !comparison;
			display->_is_two = comparison;
			display->_comparison_eligible = comparison;
			display->_item1 = make_thumbnail_test_item("invalid-one.jpg", {});
			display->_selected_texture1 = std::make_shared<texture_state>(as, display->_item1);
			const auto fail = [](const texture_state_ptr& texture)
			{
				file_load_result failed;
				failed.reason = file_load_result::failure::unreadable;
				for (auto attempt = 0; attempt < 3; ++attempt) texture->complete_load(failed, 0, false);
			};
			fail(display->_selected_texture1);
			if (comparison)
			{
				display->_item2 = make_thumbnail_test_item("invalid-two.jpg", {});
				display->_selected_texture2 = std::make_shared<texture_state>(as, display->_item2);
				fail(display->_selected_texture2);
			}

			const auto host = std::make_shared<detached_test_host>();
			view_element_ptr control;
			if (comparison) control = std::make_shared<side_by_side_control>(state, display, host);
			else control = std::make_shared<photo_control>(state, display, host);
			slot_test_draw_context dc({});
			const auto measured = control->measure(dc, extent.cx);
			control->layout(dc, {10, 20, 10 + extent.cx, 20 + extent.cy});
			control->render(dc, {});
			const auto errors = std::count(dc.text_values.begin(), dc.text_values.end(), tt.image_display_failed.sv());
			assert_equal(comparison ? 2 : 1, static_cast<int>(errors),
			             "each failed image has a visible message even without source dimensions");
			assert_equal(true, measured.cy >= dc.text_line_height(render::font_face::dialog) * 2,
			             "measurement reserves room for failure feedback");
			for (auto i = 0_z; i < dc.text_values.size(); ++i)
			{
				if (dc.text_values[i] == tt.image_display_failed.sv())
					assert_equal(true, dc.text_bounds[i].width() >= 96 && dc.text_bounds[i].height() >= 40,
					             "the failure text has a drawable area");
			}
		}
	}
}

class processing_test_view final : public review_list
{
public:
	std::string _text;

	processing_test_view(view_state& state, view_host_ptr host) : review_list(state, std::move(host)) {}

	std::string_view status() override { return _text; }

	void refresh() override {}

	void add_rows(const int count)
	{
		for (auto i = 0; i < count; ++i)
		{
			auto row = std::make_shared<row_element>(*this);
			row->_work_index = i;
			_rows.emplace_back(std::move(row));
		}
	}

	int active_device_top() const { return _active_row->bounds.top - _scroller.scroll_offset().y; }
	int active_device_bottom() const { return _active_row->bounds.bottom - _scroller.scroll_offset().y; }
};

// A four-column review list, as Import and Sync show: a narrow first column, a long name, an arrow,
// and another long name.
class columns_test_view final : public review_list
{
public:
	columns_test_view(view_state& state, view_host_ptr host) : review_list(state, std::move(host)) {}

	void refresh() override {}

	std::array<text_t, max_col_count> col_titles() override { return {tt.action, tt.source, {}, tt.destination}; }

	void add_row(const std::string_view old_name, const std::string_view new_name)
	{
		auto row = std::make_shared<row_element>(*this);
		row->_text[1] = old_name;
		row->_icons[2] = icon_index::next;
		row->_text[3] = new_name;
		row->_result_index = static_cast<int>(_rows.size());
		_rows.emplace_back(std::move(row));
	}

	int width(const int col) const { return col_widths[col]; }
	int content_width(const int col) const { return _content_col_widths[col]; }
	int narrowest() const { return _min_col_width; }
	int table_left() const { return coll_offset; }
	recti divider(const int col) const { return _col_divider_bounds[col]; }
	int scroll_y() const { return _scroller.scroll_offset().y; }
	recti heading_cell(const int col) const { return _heading_cells[static_cast<size_t>(col)].bounds; }

	// Where the last column ends: the table's left, every column's width and the gap between each two.
	int table_right(const int gap) const
	{
		auto x = coll_offset;
		for (int i = 0; i < col_count; i++) x += col_widths[i] + (i + 1 < col_count ? gap : 0);
		return x;
	}
};

// The task views size their columns to their content and shrink them to fit the view, which cuts a
// long name short. The divider after a column resizes it, as in a file browser's details view: the
// columns before it stay put and the divider stays under the pointer, the columns after it give up
// the room, Escape takes a drag back, a double-click fits the column to its content, and opening the
// view again starts from columns that fit.
static void should_resize_list_columns_by_their_dividers()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto host = std::make_shared<detached_test_host>();
	const auto view = std::make_shared<columns_test_view>(s, host);
	view->add_row(std::string(60, 'o'), std::string(60, 'n'));

	constexpr sizei extent{800, 300};
	list_test_measure_context mc;
	view->layout(mc, extent);

	const auto shrunk = view->width(1);
	assert_equal(true, shrunk < view->content_width(1), "a long old name is cut short to fit the view");

	const auto grab = view->divider(1).center();
	hit_test_context ctx{grab, recti(extent)};
	const auto controller = view->controller_from_location(host, grab, ctx);
	assert_equal(true, std::dynamic_pointer_cast<ui::table_divider_controller>(controller) != nullptr,
	             "the divider after the old names can be grabbed");

	const auto status_width = view->width(0);
	const auto left = view->table_left();
	const auto new_names = view->width(3);

	controller->on_mouse_left_button_down(grab, {});
	controller->on_mouse_left_button_up(grab, {});
	view->layout(mc, extent);
	assert_equal(shrunk, view->width(1), "a press that does not move sizes nothing");
	assert_equal(false, view->is_user_sized(1), "and leaves the column sizing to its content");

	controller->on_mouse_left_button_down(grab, {});
	controller->on_mouse_move({grab.x + 100, grab.y});
	view->layout(mc, extent);
	assert_equal(shrunk + 100, view->width(1), "dragging the divider widens the column before it");
	assert_equal(grab.x + 100, view->divider(1).center().x, "and the divider stays under the pointer");
	assert_equal(status_width, view->width(0), "the columns before it stay as they were");
	assert_equal(left, view->table_left(), "and where they were");
	assert_equal(true, view->width(3) < new_names, "the column after it gives up the room");
	controller->on_mouse_left_button_up({grab.x + 100, grab.y}, {});

	const auto dragged = view->width(1);
	const auto moved = view->divider(1).center();
	controller->on_mouse_left_button_down(moved, {});
	controller->on_mouse_move({moved.x - 60, moved.y});
	assert_equal(true, controller->escape(), "Escape takes a drag back");
	view->layout(mc, extent);
	assert_equal(dragged, view->width(1), "to the width the column had before it");

	controller->on_mouse_left_button_double_click(moved, {});
	view->layout(mc, extent);
	assert_equal(view->content_width(1), view->width(1), "a double-click fits the column to its old names");

	view->activate(extent);
	view->layout(mc, extent);
	assert_equal(shrunk, view->width(1), "and the next use of the view starts from columns that fit");
}

// A finished run's statuses come in the order the review built its rows, and land on those rows however the
// list was sorted meanwhile. The rows keep every column the review had and gain a Status column after them; a
// row the run never reached was not run, and one that did nothing is dimmed.
static void should_show_a_run_on_the_rows_it_ran()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto host = std::make_shared<detached_test_host>();
	const auto view = std::make_shared<columns_test_view>(s, host);
	std::vector<review_list::row_element_ptr> rows;

	for (const auto* const name : {"b-first", "a-second", "c-third"})
	{
		auto row = std::make_shared<review_list::row_element>(*view);
		row->_text[0] = "Copy";
		row->_text[1] = name;
		row->_text[3] = std::format("{}-destination", name);
		rows.emplace_back(std::move(row));
	}

	view->review_rows(std::move(rows));
	view->sort({1, true});
	assert_equal("a-second"sv, view->_rows.front()->_text[1], "a heading sorts the rows by its column");
	assert_equal(4, view->column_count(), "the review shows its own columns");

	view->show_results(std::vector{item_status::success, item_status::fail});
	assert_equal(5, view->column_count(), "a finished run adds a Status column");
	assert_equal(tt.status.sv(), view->column_titles()[4].sv(), "headed Status");

	const auto row_named = [&view](const std::string_view name)
	{
		return *std::ranges::find_if(view->_rows, [name](const auto& row) { return row->_text[1] == name; });
	};

	assert_equal(tt.result_success.sv(), row_named("b-first")->_text[4], "the first reported row succeeded");
	assert_equal(tt.result_failed.sv(), row_named("a-second")->_text[4], "the second failed, sorted first or not");
	assert_equal(tt.result_not_run.sv(), row_named("c-third")->_text[4], "a row the run never reached was not run");
	assert_equal("a-second-destination"sv, row_named("a-second")->_text[3], "what the review said stays");
	assert_equal(true, row_named("c-third")->_text_color[1] != 0, "a row that did nothing is dimmed");
	assert_equal(true, row_named("b-first")->_text_color[1] == 0, "and one that did something is not");
}

// The review is a list the keyboard selects from while the view has it, as any list is: Down puts the cursor on the
// first row and then a row on, Page Down a page on, End on the last and Home on the first, picking out the row it
// reaches and scrolling to keep it in sight. Shift extends what is picked out and Ctrl moves the cursor alone; Ctrl+A
// picks out every row and its text is a line a row, the arrows' column left out. Escape lets go of what is picked out
// before it closes the task, and a letter is left to the window.
static void should_select_a_review_by_its_keys()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto host = std::make_shared<detached_test_host>();
	const auto view = std::make_shared<columns_test_view>(s, host);
	for (auto i = 0; i < 200; ++i) view->add_row(std::format("old-{}", i), std::format("new-{}", i));

	constexpr sizei extent{800, 300};
	list_test_measure_context mc;
	view->layout(mc, extent);
	const auto in_sight = [&view, extent]
	{
		const auto ring = view->keyboard_bounds();
		return ring.top >= 0 && ring.bottom <= extent.cy;
	};

	assert_equal(true, view->key_down(platform::keys::DOWN, {}), "Down moves the cursor");
	assert_equal(0, view->cursor_index(), "onto the first row");
	assert_equal(0, view->scroll_y(), "which is in sight already");
	view->key_down(platform::keys::DOWN, {});
	assert_equal(1, view->cursor_index(), "and then a row on");
	assert_equal(true, view->_rows[1]->_selected && !view->_rows[0]->_selected, "picking out the row it reaches");

	view->key_down(platform::keys::NEXT, {});
	assert_equal(true, view->cursor_index() > 2 && in_sight(), "Page Down goes a page on, kept in sight");
	view->key_down(platform::keys::END, {});
	assert_equal(199, view->cursor_index(), "End goes to the last row");
	assert_equal(true, view->scroll_y() > 0 && in_sight(), "scrolled to show it");
	view->key_down(platform::keys::DOWN, {});
	assert_equal(199, view->cursor_index(), "and Down no further");

	platform::key_state shift;
	shift.shift = true;
	view->key_down(platform::keys::UP, shift);
	view->key_down(platform::keys::UP, shift);
	assert_equal(3_z, view->_selection.size(), "Shift extends what is picked out");
	platform::key_state control;
	control.control = true;
	view->key_down(platform::keys::UP, control);
	assert_equal(196, view->cursor_index(), "Ctrl moves the cursor");
	assert_equal(3_z, view->_selection.size(), "and leaves what is picked out");

	view->key_down(platform::keys::HOME, {});
	assert_equal(0, view->cursor_index(), "Home goes back to the first row");
	assert_equal(0, view->scroll_y(), "and the top of the list");

	assert_equal(true, view->key_down(U'A', control), "Ctrl+A picks out every row");
	assert_equal(200_z, view->_selection.size(), "all of them");
	const auto text = view->selected_rows_text();
	assert_equal(true, text.starts_with("\told-0\tnew-0\r\n\told-1\tnew-1"),
	             "a line a row, each column parted by a tab and the arrows left out");

	assert_equal(true, view->escape(), "Escape lets go of what is picked out");
	assert_equal(true, view->_selection.empty() && !view->_rows[0]->_selected, "all of it");
	assert_equal(false, view->escape(), "and then leaves Escape to close the task");
	assert_equal(false, view->key_down(U'A', {}), "a letter is left to the window");
}

// A review's headings sort its rows as every headed table's do: a first click the column's own way - the first
// column's too - a second the other way and a third back to the review's own order. Text sorts as it reads, numbers
// in it by value, and a column the review gives keys sorts by them. A heading over arrows alone is no button, and a
// press on a row picks it out as a click does in any list.
static void should_sort_a_review_from_its_headings()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto host = std::make_shared<detached_test_host>();
	const auto view = std::make_shared<columns_test_view>(s, host);
	std::vector<review_list::row_element_ptr> rows;

	for (const auto& [action, rank, name] :
	     {std::tuple{"Skip", 3, "IMG_10"}, std::tuple{"Copy", 1, "IMG_9"}, std::tuple{"Exists", 2, "IMG_100"}})
	{
		auto row = std::make_shared<review_list::row_element>(*view);
		row->_text[0] = action;
		row->_keys[0] = rank;
		row->_text[1] = name;
		row->_icons[2] = icon_index::next;
		row->_text[3] = std::format("{}-new", name);
		rows.emplace_back(std::move(row));
	}

	view->review_rows(std::move(rows));
	constexpr sizei extent{800, 300};
	list_test_measure_context mc;
	view->layout(mc, extent);

	const auto names = [&view]
	{
		std::string result;
		for (const auto& row : view->_rows) result += row->_text[1] + ",";
		return result;
	};
	const auto click = [&](const int column)
	{
		const auto at = view->heading_cell(column).center();
		hit_test_context ctx{at, recti(extent)};
		const auto controller = view->controller_from_location(host, at, ctx);
		if (!controller) return false;
		controller->on_mouse_left_button_down(at, {});
		controller->on_mouse_left_button_up(at, {});
		return true;
	};

	assert_equal("IMG_10,IMG_9,IMG_100,"s, names(), "rows start in the review's own order");
	assert_equal(true, click(1), "the names' heading answers a click");
	assert_equal("IMG_9,IMG_10,IMG_100,"s, names(), "text sorts as it reads, its numbers by value");
	click(1);
	assert_equal("IMG_100,IMG_10,IMG_9,"s, names(), "a second click sorts the other way");
	assert_equal(true, view->current_sort() == ui::table_sort{1, false}, "which the heading shows");
	click(1);
	assert_equal("IMG_10,IMG_9,IMG_100,"s, names(), "and a third puts the review's own order back");
	assert_equal(-1, view->current_sort().column, "unsorted");

	click(0);
	assert_equal("IMG_9,IMG_100,IMG_10,"s, names(), "the first column sorts too, by the keys the review gives it");
	assert_equal(false, click(2), "a heading over arrows alone is no button");

	view->review_rows({view->_rows.rbegin(), view->_rows.rend()});
	view->layout(mc, extent);
	assert_equal("IMG_9,IMG_100,IMG_10,"s, names(), "rows a new analysis gives arrive in the order the user chose");

	const auto row_at = view->_rows[1]->bounds.center() - pointi{0, view->scroll_y()};
	hit_test_context row_ctx{row_at, recti(extent)};
	const auto row_press = view->controller_from_location(host, row_at, row_ctx);
	assert_equal(true, row_press != nullptr && view->_rows[1]->_hovered, "a row is lit under the pointer");
	row_press->on_mouse_left_button_down(row_at, {});
	assert_equal(true, view->_rows[1]->_selected && view->cursor_index() == 1, "and picked out by a press");
}

// The keyboard reaches what acts among drawn elements, in the order they are drawn: each link, each part of an
// element that acts on several, and each control, which takes the focus itself - but nothing hidden, disabled or
// only pointed into. Left and Right step through the parts in order, Up and Down to the nearest part in the row
// above or below, and Home and End to either end.
static void should_find_and_step_the_keyboard_s_parts()
{
	struct pointed_into final : view_element, no_keyboard_parts
	{
		pointed_into() : view_element(view_element_style::can_invoke) {}
	};

	const auto link_at = [](const recti bounds)
	{
		auto link = std::make_shared<link_element>("link", [] {});
		link->bounds = bounds;
		return link;
	};

	const auto first = link_at({0, 0, 40, 20});
	const auto second = link_at({60, 0, 100, 20});
	const auto third = link_at({110, 0, 150, 20});
	const auto below_left = link_at({0, 30, 40, 50});
	const auto below_right = link_at({110, 30, 150, 50});
	const auto hidden = link_at({0, 60, 40, 80});
	hidden->is_visible(false);
	const auto disabled = link_at({60, 60, 100, 80});
	disabled->enable(false);
	const auto picture = std::make_shared<pointed_into>();
	picture->bounds = {0, 90, 150, 140};

	auto rating = 0;
	const auto stars = std::make_shared<rating_edit_control>(rating, nullptr);
	ui::test::fake_measure_context mc({8, 16});
	stars->measure(mc, 400);
	stars->bounds = {0, 150, mc.icon_cxy * rating_edit_control::cell_count, 150 + mc.icon_cxy};
	const auto button = ui::make_button("Button", [] {});

	const auto row = std::make_shared<view_elements>();
	row->add(second);
	row->add(third);

	const pointi offset{10, 5};
	const std::vector<view_element_ptr> roots{first,    row,     below_left, below_right, hidden,
	                                          disabled, picture, stars,      button};
	const auto targets = find_keyboard_targets(roots, offset);
	const auto parts = find_keyboard_parts(roots, offset);

	assert_equal(true, std::holds_alternative<ui::control_ptr>(targets.back()), "a control is a target of its own");
	assert_equal(5_z + rating_edit_control::cell_count, parts.size(),
	             "every link that shows and acts, and each star, but nothing hidden, disabled or pointed into");
	assert_equal(true, parts[0].element == first && parts[1].element == second && parts[2].element == third,
	             "in the order they are drawn, into what holds them");
	assert_equal(true, parts[0].bounds == first->bounds.offset(offset), "each where it is drawn");
	assert_equal(true, parts[5].element == stars && parts[10].element == stars && parts[10].index == 5,
	             "the stars, Reject first, each a part of one element");

	assert_equal(1, step_keyboard_part(parts, 0, platform::keys::RIGHT), "Right steps on");
	assert_equal(0, step_keyboard_part(parts, 0, platform::keys::LEFT), "and Left stops at the first");
	assert_equal(10, step_keyboard_part(parts, 0, platform::keys::END), "End goes to the last");
	assert_equal(0, step_keyboard_part(parts, 7, platform::keys::HOME), "and Home to the first");
	assert_equal(4, step_keyboard_part(parts, 2, platform::keys::DOWN), "Down to the nearest part in the row below");
	assert_equal(2, step_keyboard_part(parts, 4, platform::keys::UP), "and Up to the nearest in the row above");
	assert_equal(1, step_keyboard_part(parts, 1, platform::keys::UP), "with nothing above, Up stays put");
	assert_equal(-1, step_keyboard_part(parts, 0, U'A'), "a letter moves nothing");
}

// A part is pressed as a click at its centre presses it, through the controller its host offers there: a link does
// what it does, and each star of a rating gives the rating a click on that star gives.
static void should_press_a_part_as_a_click_does()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	auto pressed = 0;
	auto rating = 0;
	const auto link = std::make_shared<link_element>("Press", [&pressed] { ++pressed; });
	const auto stars = std::make_shared<rating_edit_control>(rating, nullptr);
	const auto panel = std::make_shared<view_controls_host>(s);
	panel->_controls = {link, stars};
	ui::test::fake_measure_context mc({8, 16});
	panel->layout_panel(mc, {0, 0, 400, 300});

	const auto parts = find_keyboard_parts(panel->_controls, panel->elements_offset());
	assert_equal(1_z + rating_edit_control::cell_count, parts.size(), "the link, then Reject and the five stars");

	press_keyboard_part(panel, parts[0]);
	assert_equal(1, pressed, "pressing the link does what a click on it does");
	press_keyboard_part(panel, parts[1 + 3]);
	assert_equal(3, rating, "the third star rates three");
	press_keyboard_part(panel, parts[1]);
	assert_equal(-1, rating, "Reject rejects");
	press_keyboard_part(panel, parts[1]);
	assert_equal(0, rating, "and, pressed again, clears the mark, as a second click does");
}

// A table starts a margin in from the view's left edge however little it holds, and its last column takes
// the rest of the width: a short table centred in the view left margins either side of it that read as part
// of it. A divider narrows the column before it only as far as a column can go, the last column takes up
// the room it gives back and makes the room it takes, and the next use of the view starts again from
// columns that fit.
static void should_run_a_table_from_edge_to_edge()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto host = std::make_shared<detached_test_host>();
	const auto view = std::make_shared<columns_test_view>(s, host);
	view->add_row("abcdefghij", "klmnopqrst");

	constexpr sizei extent{800, 300};
	list_test_measure_context mc;
	view->layout(mc, extent);

	// The text keeps clear of the window's frame on the left, and the last column runs to the right edge.
	const auto margin = mc.padding2 * 2;
	const auto right_edge = extent.cx - mc.scroll_width - mc.padding2 / 2;
	assert_equal(margin, view->table_left(), "a short table starts at its margin");
	assert_equal(right_edge, view->table_right(mc.padding2), "and its last column runs to the right edge");
	assert_equal(true, view->width(3) > view->content_width(3), "wider than its names need");

	for (int i = 0; i < 3; i++)
	{
		assert_equal(view->content_width(i), view->width(i), "the other columns fit their content");
	}

	const auto grab = view->divider(1).center();
	hit_test_context ctx{grab, recti(extent)};
	const auto controller = view->controller_from_location(host, grab, ctx);
	assert_equal(true, std::dynamic_pointer_cast<ui::table_divider_controller>(controller) != nullptr,
	             "the divider after the first name column can be grabbed");

	const auto first_width = view->width(0);
	const auto start_width = view->width(1);
	const auto last_width = view->width(3);

	controller->on_mouse_left_button_down(grab, {});
	controller->on_mouse_move({grab.x - 200, grab.y});
	view->layout(mc, extent);
	assert_equal(view->narrowest(), view->width(1), "the column narrows only as far as a column can");
	assert_equal(margin, view->table_left(), "the table stays at its margin");
	assert_equal(first_width, view->width(0), "the column before it stays as it was");
	assert_equal(last_width + start_width - view->narrowest(), view->width(3), "the last column takes the room");
	assert_equal(right_edge, view->table_right(mc.padding2), "and the table still reaches the right edge");

	controller->on_mouse_move({grab.x + 50, grab.y});
	view->layout(mc, extent);
	assert_equal(start_width + 50, view->width(1), "dragged the other way the column widens");
	assert_equal(grab.x + 50, view->divider(1).center().x, "with the divider under the pointer");
	assert_equal(last_width - 50, view->width(3), "and the last column makes the room");
	assert_equal(right_edge, view->table_right(mc.padding2), "the table ending where it did");
	controller->on_mouse_left_button_up({grab.x + 50, grab.y}, {});

	view->activate(extent);
	view->layout(mc, extent);
	assert_equal(start_width, view->width(1), "the next use of the view starts from columns that fit");
	assert_equal(last_width, view->width(3), "the last column filling the view again");
}

// A run keeps the row it is working on visible, and visible means clear of the chrome painted over
// the list rather than merely inside the scrolling area: the column headers are drawn across the top
// of it. The run's status is in the frame's status bar below the view, so the base of the list is the
// row's to use, and nothing is held back there.
static void should_keep_the_processing_row_clear_of_the_view_chrome()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto host = std::make_shared<detached_test_host>();
	const auto view = std::make_shared<processing_test_view>(s, host);

	constexpr sizei extent{400, 300};
	constexpr auto band = list_test_measure_context::line_height + 8 * 2;
	constexpr auto margin = list_test_measure_context::line_height;

	list_test_measure_context mc;
	view->add_rows(100);
	view->_text = "Processing";
	view->begin_processing(100);
	view->layout(mc, extent);

	// Far enough down that the list has to scroll to reach it.
	view->processing_work_item(60);

	assert_equal(extent.cy, view->active_device_bottom(),
	             "the row comes to rest at the base of the view, which nothing is painted over");
	assert_equal(true, view->active_device_top() >= 0, "and is still inside the view");

	// Back up the list: the column headers above it are still painted over the rows.
	view->processing_work_item(5);

	assert_equal(true, view->active_device_top() >= band + margin, "the row stops clear of the column headers");
	assert_equal(true, view->active_device_bottom() <= extent.cy, "and is still inside the view");

	view->end_processing();
}

// A run that begin_processing replaces is cancelled through the source its worker holds. Swapping in a
// new source alone left an Analyze restarted by Refresh scanning on to an answer nobody would publish.
static void should_cancel_the_run_a_new_one_replaces()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto host = std::make_shared<detached_test_host>();
	const auto view = std::make_shared<processing_test_view>(s, host);

	view->begin_processing(1);
	const auto first_source = view->processing_cancel_source();
	const core::cancel_token first(*first_source);

	view->begin_processing(1);
	const auto second_source = view->processing_cancel_source();
	const core::cancel_token second(*second_source);

	assert_equal(true, first.is_cancelled(), "the replaced run is cancelled");
	assert_equal(false, second.is_cancelled(), "the run that replaced it is not");

	view->end_processing();
}

// A place search answers on the location queue. One still in flight when the user panned the map
// landed afterwards and selected its first match, moving the coordinate back to a place the user had
// left - which Add Location then wrote to the selection.
static void should_not_let_a_late_place_search_override_the_map()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto host = std::make_shared<detached_test_host>();
	const auto view = std::make_shared<locate_view>(s, host);
	const auto panel = view->controls(ui::no_frame());
	const auto search = view->place_search_for_tests();
	assert_equal(true, search != nullptr, "the panel has a place search");

	auto answered = false;
	search->search("Paris", [&answered](const ui::auto_complete_results&) { answered = true; });
	assert_equal(false, answered, "the answer waits for the UI queue");

	view->on_map_panned(gps_coordinate(10.0, 20.0));
	as.drain_ui();

	assert_equal(false, answered, "an answer overtaken by a pan is discarded");
	assert_equal(10, static_cast<int>(view->location()->latitude), "the coordinate stays where the map was panned");
	assert_equal(20, static_cast<int>(view->location()->longitude), "in both axes");
}

// Refresh re-plans, and in Sync it reran Analyze even while a Run was copying. That replaced the run's
// processing state, so the copy either carried on unseen behind new rows or, once a replaced run was
// cancelled, stopped partway without saying what it had done. A Run in progress is left alone.
static void should_leave_a_running_sync_alone_on_refresh()
{
	const auto root = test_temps.next_folder("refresh-during-sync");
	const auto local = root.combine("local");
	const auto remote = root.combine("remote");
	platform::create_folder(local);
	platform::create_folder(remote);
	write_test_file(local.combine_file("copy.txt"), "copy");

	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	core::index_roots roots;
	roots.folders.emplace(local);
	index.index_roots(roots);
	view_state s(ss, as, index, make_test_player());

	const auto saved = setting.sync;
	const core::scope_exit restore([saved] { setting.sync = saved; });
	setting.sync.sync_collection = true;
	setting.sync.remote_path = std::string(remote.text());
	setting.sync.sync_local_remote = true;
	setting.sync.sync_remote_local = false;
	setting.sync.sync_delete_local = false;
	setting.sync.sync_delete_remote = false;

	const auto view = std::make_shared<sync_view>(s, nullptr);
	view->analyze();
	while (as.run_next(async_queue::work))
	{
	}
	as.drain_ui();
	assert_equal(true, view->can_run(), "the analysis found a file to copy");

	view->run();
	assert_equal(true, view->progress().active, "the run is under way");
	const auto queued_by_run = as.pending_worker_count(async_queue::work);

	view->reload();

	assert_equal(queued_by_run, as.pending_worker_count(async_queue::work), "Refresh queued no analysis over the run");
	assert_equal(true, view->progress().active, "the run is still the one in progress");

	while (as.run_next(async_queue::work))
	{
	}
	as.drain_ui();

	assert_equal(true, platform::exists(remote.combine_file("copy.txt")), "Refresh did not stop the copy");
	assert_equal(false, view->progress().active, "the run reports its end");
}

// The drawn controls in an element's tree, in its order, as a frame finds them.
static std::vector<ui::control_ptr> tree_controls(const view_element_ptr& root, const bool with_hidden = false)
{
	std::vector<ui::control_ptr> result;

	ui::walk_tree(root, {}, with_hidden, [&result](ui::element& e, pointi, bool, bool)
	{
		if (e.kind == ui::element_kind::control) result.push_back(static_cast<ui::control&>(e).shared_from_this());
	});

	return result;
}

// Whether a control in root's tree takes input: enabled itself, and under no disabled element.
static bool takes_input(const view_element_ptr& root, const ui::control_ptr& control)
{
	auto result = false;

	ui::walk_tree(root, {}, true, [&result, &control](ui::element& e, pointi, bool, const bool ancestors_enabled)
	{
		if (&e == control.get()) result = ancestors_enabled && e.is_enabled();
	});

	return result;
}

static void should_answer_a_null_frame_without_side_effects()
{
	const auto& f = ui::no_frame();
	assert_equal(true, f != nullptr, "the stand-in is a real object");
	assert_equal(true, f == ui::no_frame(), "one shared instance, so it costs nothing to ask");

	// Answers chosen so a caller that acts on them does less, never more: an absent window is
	// occluded (skip drawing), invisible, unfocused, and its cursor is outside every client rect.
	assert_equal(true, f->is_occluded(), "nothing drawn into it could be seen");
	assert_equal(false, f->is_visible(), "not visible");
	assert_equal(false, f->has_focus(), "cannot hold focus");
	assert_equal(false, f->is_enabled(), "cannot be interacted with");
	assert_equal(true, f->window_bounds().is_empty(), "occupies nothing");
	assert_equal(true, f->cursor_location() == pointi(-1, -1), "cursor is outside every client rect");
}

static void should_forward_checkbox_hits_to_visible_checked_child()
{
	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {200, 100};
	auto checked = true;
	auto checkbox = std::make_shared<ui::check_control>(ui::no_frame(), "Rating", checked);
	const auto child = std::make_shared<forwarding_child_element>();
	checkbox->child(child);

	hit_test_context ctx{{15, 25}, recti(0, 0, 200, 100)};
	assert_equal(true, checkbox->is_control_area({15, 25}, {}), "checked child contributes control area");
	const auto controller = checkbox->controller_from_location(host, {15, 25}, {}, ctx);
	assert_equal(true, controller != nullptr, "checked child receives controller lookup");
	assert_equal(1, child->controller_requests, "controller lookup was forwarded to child");

	checkbox->checked(false);
	assert_equal(false, checkbox->is_control_area({15, 25}, {}), "unchecked child is not interactive");
	hit_test_context unchecked_ctx{{15, 25}, recti(0, 0, 200, 100)};
	assert_equal(true, checkbox->controller_from_location(host, {15, 25}, {}, unchecked_ctx) == nullptr,
	             "unchecked child receives no controller lookup");

	checkbox->checked(true);
	checkbox->collapse_child_when_unchecked();
	checkbox->checked(false);
	assert_equal(false, checkbox->is_control_area({15, 25}, {}), "collapsed child is not interactive");
	hit_test_context collapsed_ctx{{15, 25}, recti(0, 0, 200, 100)};
	assert_equal(true, checkbox->controller_from_location(host, {15, 25}, {}, collapsed_ctx) == nullptr,
	             "collapsed child receives no controller lookup");
}

// Every layout of a task view enabled every control in its panel, so the options of a format not
// chosen came back live: Convert's JPEG quality slider while JPEG XL was the format. A disabled element
// disables what it holds, so the panel and each unticked box answer for their own.
static void should_keep_the_options_of_an_unchosen_format_disabled()
{
	auto jpeg = false;
	auto jxl = true;
	const auto jpeg_check = std::make_shared<ui::check_control>(ui::no_frame(), "JPEG", jpeg, true);
	const auto jxl_check = std::make_shared<ui::check_control>(ui::no_frame(), "JPEG XL", jxl, true);
	const auto jpeg_options = ui::make_edit({}, "90", {});
	const auto jxl_options = ui::make_edit({}, "80", {});
	jpeg_check->child(jpeg_options);
	jxl_check->child(jxl_options);

	const auto panel = std::make_shared<ui::group_control>();
	panel->add(jpeg_check);
	panel->add(jxl_check);

	ui::enable_element(panel, false);
	assert_equal(false, takes_input(panel, jpeg_options), "a running task disables every option");
	assert_equal(false, takes_input(panel, jxl_options), "the chosen format's included");

	ui::enable_element(panel, true);
	assert_equal(true, takes_input(panel, jxl_options), "the chosen format's options come back");
	assert_equal(false, takes_input(panel, jpeg_options), "an unchosen format's stay disabled");

	jxl_check->checked(false);
	jpeg_check->checked(true);
	assert_equal(true, takes_input(panel, jpeg_options), "choosing JPEG brings its options back");
	assert_equal(false, takes_input(panel, jxl_options), "and leaves those of JPEG XL disabled");
}

static void should_reject_failed_dialog_thumbnail_decodes()
{
	const auto valid_image = make_valid_test_image();
	assert_equal(true, core::is_valid(valid_image), "test fixture has a valid encoded image");

	const auto corrupt = std::make_shared<core::image>(core::blob{0xff, 0xd8, 0xff, 0xd9}, sizei{32, 32},
	                                                   core::image_format::JPEG, core::orientation::top_left);
	std::vector<core::const_image_ptr> images;
	images.emplace_back(corrupt);
	for (auto i = 0; i < 7; ++i) images.emplace_back(valid_image);

	ui::title_control2 title(ui::no_frame(), icon_index::none, "Title", "Selection", images, images.size());
	const auto dims = title.surface_dims();
	assert_equal(8u, static_cast<unsigned>(dims.size()), "failed decode did not consume a thumbnail slot");

	list_test_measure_context mc;
	ui::selection_thumbnails_control strip(ui::no_frame());
	std::vector<core::const_surface_ptr> surfaces;
	surfaces.reserve(7);
	for (auto i = 0; i < 7; ++i) surfaces.emplace_back(make_test_surface({16, 12}));
	strip.selection(surfaces, images.size());
	const auto measured = strip.measure(mc, 700);
	assert_equal(true, measured.cy > 0, "staged surfaces measure the strip");

	ui::selection_thumbnails_control failed_strip(ui::no_frame());
	failed_strip.selection(std::vector<core::const_surface_ptr>{}, 2);
	assert_equal(false, failed_strip.is_visible(), "thumbnail strips without staged surfaces stay hidden");
}

// A search's results are an element beside its edit, drawn in whatever frame holds the search - even one
// that cannot make frames - so a frame finds the edit, and lays the list out below it.
static void should_use_non_null_search_list_frame()
{
	auto value = std::string{};
	auto completes = std::make_shared<absent_frame_complete_strategy>();
	const auto frame = std::make_shared<null_list_frame_parent>();
	const auto search = std::make_shared<ui::search_control>(frame, value, completes);

	auto children = 0;
	search->visit_children([&children](const view_element_ptr& e)
	{
		assert_equal(true, e != nullptr, "the visitor receives nothing null");
		++children;
	});
	assert_equal(2, children, "the edit and the list of results");
	assert_equal(1_z, tree_controls(search).size(), "of which the edit is the frame's to give the keyboard");

	list_test_measure_context mc;
	search->measure(mc, 200);
	search->layout(mc, recti(0, 0, 200, 200));
	const auto edit = tree_controls(search).front();
	assert_equal(true, edit->bounds.top == 0 && edit->bounds.bottom < 200, "the edit is laid out at the top");
}

// The controls an element holds are reached through whatever holds the element, so a dialog's frame finds
// them, gives them the keyboard and draws them: as a check box's option, and in a table's cell.
static void should_reach_the_frames_inside_an_option_and_a_table()
{
	auto value = std::string{};
	auto checked = true;
	const auto completes = std::make_shared<absent_frame_complete_strategy>();
	const auto frame = std::make_shared<null_list_frame_parent>();

	const auto option = std::make_shared<ui::check_control>(frame, "Search", checked);
	option->child(std::make_shared<ui::search_control>(frame, value, completes));

	const auto table = std::make_shared<ui::table_element>();
	table->add(std::make_shared<ui::text_element>("Find"),
	           std::make_shared<ui::search_control>(frame, value, completes));

	for (const auto& [name, container] : {std::pair<std::string_view, view_element_ptr>{"an option", option},
	                                      std::pair<std::string_view, view_element_ptr>{"a table", table}})
	{
		assert_equal(true, !tree_controls(container).empty(), std::format("{} reaches its search's edit", name));
	}
}

// A search's results among a dialog's controls - Copy/Move's folders, Open with's programs - are picked with a
// press and scrolled by the wheel, as they were in a window of their own; and a press on them does not move the
// dialog, as a press on its ground does.
static void should_pick_and_scroll_a_search_s_results_in_a_dialog()
{
	auto value = std::string{};
	const auto completes = std::make_shared<given_results_strategy>();
	for (auto i = 0; i < 40; ++i) completes->results.emplace_back(std::make_shared<pickable_match>(*completes));

	const auto search = std::make_shared<ui::search_control>(ui::no_frame(), value, completes);
	const auto dlg = make_dlg(ui::no_frame());
	dlg->_max_width = {44};
	dlg->_max_height = {88};
	dlg->initialise({search});
	list_test_measure_context mc;
	dlg->layout(mc);

	const auto first = completes->results.front();
	const auto loc = first->bounds.center();

	hit_test_context ctx{loc, recti(0, 0, 1000, 1000)};
	const auto controller = dlg->controller_from_location(loc, ctx);
	assert_equal(true, controller != nullptr, "a result answers a press");
	controller->on_mouse_left_button_down(loc, {});
	controller->on_mouse_left_button_up(loc, {});
	assert_equal(true, completes->picked == first, "and is picked by it");
	assert_equal(true, completes->how == ui::complete_strategy_t::select_type::click, "with a click");
	assert_equal(true, dlg->is_control_area(loc), "a press on the results does not move the dialog");

	auto handled = false;
	dlg->on_mouse_wheel(loc, -120, {}, handled);
	assert_equal(true, handled, "the wheel over the results is theirs");
	assert_equal(true, dlg->_scroller.scroll_offset() == pointi{}, "not the dialog's");

	hit_test_context scrolled_ctx{loc, recti(0, 0, 1000, 1000)};
	const auto scrolled = dlg->controller_from_location(loc, scrolled_ctx);
	assert_equal(true, scrolled != nullptr, "the list scrolled under the pointer still answers");
	scrolled->on_mouse_left_button_down(loc, {});
	scrolled->on_mouse_left_button_up(loc, {});
	assert_equal(true, completes->picked != first, "with a result further down");
}

// A dialog's controls can need more or less room once it is up - Delete's title gets its thumbnails after the
// dialog shows - and the window follows them about the centre it has, rather than clipping them or standing
// half empty.
static void should_fit_a_dialog_to_content_that_arrives_after_it_shows()
{
	struct placed_frame final : ui::null_frame
	{
		recti placed;
		int placements = 0;
		bool is_visible() const override { return true; }
		recti window_bounds() const override { return placed; }
		void position(const recti bounds) override
		{
			placed = bounds;
			++placements;
		}
	};

	struct growing_element final : view_element
	{
		int height = 100;
		sizei measure(render::measure_context&, const int width_limit) const override { return {width_limit, height}; }
	};

	const auto content = std::make_shared<growing_element>();
	const auto dlg = make_dlg(ui::no_frame());
	const auto frame = std::make_shared<placed_frame>();
	dlg->_frame = frame;
	dlg->_max_width = {44};
	dlg->_max_height = {88};
	dlg->_center = true;
	dlg->initialise({content});
	list_test_measure_context mc;
	dlg->on_window_layout(mc, {}, false);

	const auto shown = frame->placed;
	assert_equal(1, frame->placements, "the dialog is placed when it is shown");
	dlg->on_window_layout(mc, shown.extent(), false);
	assert_equal(1, frame->placements, "a layout the window already fits moves nothing");

	content->height = 260;
	dlg->on_window_layout(mc, shown.extent(), false);
	assert_equal(2, frame->placements, "content that grows resizes the window");
	assert_equal(shown.height() + 160, frame->placed.height(), "to the room it now needs");
	assert_equal(shown.center().y, frame->placed.center().y, "about the centre it had");

	const auto grown = frame->placed;
	content->height = 40;
	dlg->on_window_layout(mc, grown.extent(), false);
	assert_equal(shown.height() - 60, frame->placed.height(), "and content that shrinks shrinks it");

	const auto placements = frame->placements;
	dlg->on_window_layout(mc, grown.extent(), false);
	assert_equal(placements, frame->placements, "a window that keeps another size is not asked again");
}

// The agent tools read open_dialogs() to tell whether input waits behind a dialog. A status dialog closes by going,
// so it takes itself off as it goes; left there, it held all input as if it still showed.
static void should_take_a_dialog_off_the_open_dialogs_as_it_goes()
{
	const auto before = open_dialogs().size();

	{
		const auto dlg = make_dlg(ui::no_frame());
		dlg->show_status(icon_index::save, "Saving"sv);
		assert_equal(before + 1, open_dialogs().size(), "a status dialog is open while it shows");
	}

	assert_equal(before, open_dialogs().size(), "and is not once it has gone");
}

// A batch's progress is waited out, not answered, so its dialog is not among open_dialogs() while the work runs. But
// once the work ends with a failure, the results wait on Close - and a dialog the agent tools cannot see left an
// agent's batch command waiting on it for ever.
static void should_list_a_batch_s_results_once_they_wait_on_close()
{
	// Open until it is closed, as a dialog's window is: ui::no_frame() reads as cancelled.
	struct open_frame final : ui::null_frame
	{
		bool is_canceled() const override { return false; }
	};

	null_async_strategy as;
	const auto before = open_dialogs().size();
	const auto rating = [&as](const item_status second)
	{
		const auto dlg = make_dlg(ui::no_frame());
		dlg->_frame = std::make_shared<open_frame>();
		const auto status = std::make_shared<command_status>(as, dlg, icon_index::star, "Rating"sv, 2);
		status->start_item("first.heic"sv);
		status->end_item("first.heic"sv, item_status::success);
		status->start_item("second.heic"sv);
		status->end_item("second.heic"sv, second);
		return status;
	};

	{
		const auto status = rating(item_status::fail);
		assert_equal(before, open_dialogs().size(), "progress is not a dialog to answer");

		status->complete();
		assert_equal(before + 1, open_dialogs().size(), "results that wait on Close are");

		status->wait_for_complete();
		assert_equal(before, open_dialogs().size(), "until they have been closed");
	}

	const auto succeeded = rating(item_status::success);
	succeeded->complete();
	assert_equal(before, open_dialogs().size(), "a batch that succeeds closes without waiting on anyone");
}

static void should_preserve_thumbnail_texture_slots_after_upload_failure()
{
	const std::vector<core::const_surface_ptr> surfaces{make_test_surface({80, 40}, core::orientation::top_left),
	                                                    make_test_surface({30, 90}, core::orientation::top_left)};
	ui::title_control2 title(ui::no_frame(), icon_index::none, "Title", "Selection", surfaces);
	list_test_measure_context mc;
	title.measure(mc, 320);
	title.layout(mc, recti(0, 0, 320, 100));

	slot_test_draw_context first_pass({true, false});
	title.render(first_pass, {});
	assert_equal(1u, static_cast<unsigned>(first_pass.draws.size()), "only the successful second texture is drawn");
	assert_equal(1, first_pass.draws.front().source_index, "second texture keeps its source index");
	assert_equal(30, first_pass.draws.front().source.width(), "second texture uses its own source width");
	assert_equal(90, first_pass.draws.front().source.height(), "second texture uses its own source height");

	slot_test_draw_context retry_pass({false});
	title.render(retry_pass, {});
	assert_equal(2u, static_cast<unsigned>(retry_pass.draws.size()), "retry fills the missing first slot");
	assert_equal(0, retry_pass.draws[0].source_index, "recovered first texture draws in slot zero");
	assert_equal(1, retry_pass.draws[1].source_index, "second texture remains in slot one");
}

// An agent's screenshot of a RAW in the media view showed an empty frame: wait_until_idle, and the screenshot's own
// wait, found the window settled while the file was still being read, for neither the read nor the decode after it
// counted as media loading. Each counts from when it is queued until what it made reaches the window.
static void should_count_a_shown_photo_as_loading_until_it_reaches_the_window()
{
	deferred_async_strategy as;
	const auto path = test_files_folder().combine_file("Small.jpg");
	const auto item = make_versioned_photo_item(path, core::date_t(200), core::date_t(200));
	const auto texture = std::make_shared<texture_state>(as, item);
	const auto settled = df::loading_media.load();

	texture->load_image(item);
	assert_equal(settled + 1, df::loading_media.load(), "the read counts from when it is queued");
	assert_equal(true, as.run_next(async_queue::load), "the read runs");
	assert_equal(settled + 1, df::loading_media.load(), "and while what it read waits for the window");
	as.drain_ui();
	assert_equal(settled, df::loading_media.load(), "until the window has it");
	assert_equal(false, texture->loaded().is_empty(), "the photo was read");

	slot_test_draw_context dc({});
	texture->layout(dc, {0, 0, 400, 300}, item);
	assert_equal(false, texture->prepare_picture(dc), "nothing is drawn before the decode");
	assert_equal(settled + 1, df::loading_media.load(), "the decode the draw asks for counts");
	assert_equal(true, as.run_next(async_queue::render_display), "the decode runs");
	assert_equal(settled + 1, df::loading_media.load(), "and its pixels count until the window has them");
	as.drain_ui();
	assert_equal(settled, df::loading_media.load(), "and no longer once it has");
	assert_equal(true, texture->prepare_picture(dc), "the next draw shows the photo");

	texture->load_raw();
	assert_equal(settled + 1, df::loading_media.load(), "a RAW's full read counts as well");
	assert_equal(true, as.run_next(async_queue::load_raw), "the read runs");
	as.drain_ui();
	assert_equal(settled, df::loading_media.load(), "until the window has it");
}

static void should_layout_selection_thumbnail_collage()
{
	const auto check = [](const recti draw_bounds, const std::vector<sizei>& dimensions, const size_t expected_count)
	{
		const auto bounds = ui::layout_collage(draw_bounds, dimensions);
		assert_equal(expected_count, bounds.size(), "collage bounds count");

		for (auto index = 0u; index < bounds.size(); ++index)
		{
			const auto& bound = bounds[index];
			assert_equal(true, bound.area() > 0, "collage cell has area");
			assert_equal(true, draw_bounds.contains(bound.top_left()), "collage cell top-left contained");
			assert_equal(true, draw_bounds.contains({bound.right - 1, bound.bottom - 1}),
			             "collage cell bottom-right contained");

			for (auto other = index + 1; other < bounds.size(); ++other)
			{
				const auto& other_bound = bounds[other];
				const auto overlaps = bound.left < other_bound.right && bound.right > other_bound.left &&
				                      bound.top < other_bound.bottom && bound.bottom > other_bound.top;
				assert_equal(false, overlaps, "collage cells do not overlap");
			}
		}
	};

	check({0, 0, 1200, 800}, std::vector<sizei>(9, sizei(256, 256)), 9);
	// clang-format off
	check({0, 0, 1600, 900}, {
		      {1200, 200}, {180, 900}, {400, 400}, {300, 800}, {1400, 350}, {640, 480},
		      {200, 1000}, {1600, 300}, {500, 500}, {900, 1600}, {1000, 250}, {300, 300}
	      }, 12);
	// clang-format on
	check({0, 0, 1920, 1080}, std::vector<sizei>(24, sizei(320, 240)), 24);
	check({0, 0, 1920, 1080}, std::vector<sizei>(30, sizei(320, 240)), 24);

	const auto aesthetic_bounds = ui::layout_collage({0, 0, 1200, 800}, std::vector<sizei>(9, sizei(256, 256)));
	const auto first_row_top = aesthetic_bounds.front().top;
	assert_equal(true,
	             std::any_of(aesthetic_bounds.begin(), aesthetic_bounds.end(),
	                         [first_row_top](const recti& bounds) { return bounds.top != first_row_top; }),
	             "collage uses multiple rows");
	const auto [smallest, largest] = std::minmax_element(aesthetic_bounds.begin(), aesthetic_bounds.end(),
	                                                     [](const recti& left, const recti& right)
	{ return left.area() < right.area(); });
	assert_equal(true, largest->area() > smallest->area() * 3 / 2, "collage varies cell sizes");
	assert_equal(800,
	             std::max_element(aesthetic_bounds.begin(), aesthetic_bounds.end(),
	                              [](const recti& left, const recti& right)
	{
		return left.bottom < right.bottom;
	})->bottom,
	             "collage fills available height");

	const auto portrait_bounds = ui::layout_collage({0, 0, 600, 1200}, std::vector<sizei>(9, sizei(256, 256)));
	assert_equal(1200,
	             std::max_element(portrait_bounds.begin(), portrait_bounds.end(),
	                              [](const recti& left, const recti& right)
	{
		return left.bottom < right.bottom;
	})->bottom,
	             "portrait collage fills available height");
	assert_equal(true,
	             std::any_of(portrait_bounds.begin(), portrait_bounds.end(),
	                         [](const recti& bounds) { return bounds.height() > bounds.width(); }),
	             "portrait collage has vertical cells");
}

// The filter field keeps what was typed, and finds it anywhere in an item's text.
static void should_keep_the_typed_filter_and_match_text_containing_it()
{
	filter_t filter;
	filter.wildcard("cat");
	assert_equal_strict("cat", filter.text(), "filter preserves user input");
	assert_equal(true, filter.match_text(core::cache("bobcatfish")), "filter applies contains matching");
}
static void should_accumulate_rtl_text_widths_in_logical_order()
{
	const std::vector<uint32_t> positions{0, 1, 2, 3, 4};
	const std::vector<render::text_cluster_metric> rtl_visual_widths{{1, 9.7f}, {1, 3.8f}, {1, 7.7f}, {1, 11.0f}};
	const auto xs = render::logical_text_offset_xs(positions, rtl_visual_widths);
	assert_equal(0, xs[0], "text starts at zero");
	assert_equal(true, std::ranges::is_sorted(xs), "logical caret widths are ascending");
	assert_equal(32, xs.back(), "end width is the accumulated cluster width");

	const std::vector<uint32_t> mixed_positions{0, 1, 2, 3};
	const std::vector<render::text_cluster_metric> mixed_clusters{{2, 13.0f}, {1, 5.0f}};
	const auto mixed_xs = render::logical_text_offset_xs(mixed_positions, mixed_clusters);
	assert_equal(13, mixed_xs[1], "a position inside a cluster advances to the cluster boundary");
	assert_equal(13, mixed_xs[2], "the cluster boundary shares that same logical x");
	assert_equal(18, mixed_xs[3], "following clusters continue from the accumulated width");
}

class slider_test_measure_context final : public render::measure_context
{
public:
	sizei measure_text(const std::string_view text, render::font_face font, const render::text_style style,
	                   const int width, int = 0) override
	{
		const auto natural_width = core::round(text.size() * 8 * scale_factor);
		const auto lines = style == render::text_style::single_line
		                       ? 1
		                       : std::max(1, (natural_width + std::max(1, width) - 1) / std::max(1, width));
		return {std::min(width, natural_width), text_line_height(font) * lines};
	}

	int text_line_height(render::font_face) override { return core::round(20 * scale_factor); }
	render::text_layout_ptr create_text_layout(render::font_face) override { return {}; }
};

static void should_keep_narrow_slider_rows_usable()
{
	for (const auto scale : {1.0, 1.25, 1.5, 2.0})
	{
		for (const auto width_dip : {180, 220, 400})
		{
			for (const auto label :
			     {""sv, "Vibrance"sv, "Horizontal perspective"sv, "A much longer translated adjustment label"sv})
			{
				slider_test_measure_context mc;
				mc.scale_factor = scale;
				mc.padding1 = core::round(4 * scale);
				mc.padding2 = core::round(8 * scale);
				mc.col_widths.c1 = core::round(190 * scale);
				const auto width = core::round(width_dip * scale);
				auto value = 0;
				ui::slider_control slider(ui::no_frame(), label, value, -100, 100);
				const auto measured = slider.measure(mc, width);
				const recti bounds{10, 20, 10 + width, 20 + measured.cy};
				slider.layout(mc, bounds);
				std::vector<recti> placed;
				slider.visit_children([&placed](const view_element_ptr& e) { placed.push_back(e->bounds); });
				assert_equal(2_z, placed.size(), "a row retains its slider and numeric field");
				const auto track = placed[0];
				const auto edit = placed[1];
				assert_equal(true, track.width() >= ui::default_control_height(mc) * 2,
				             "a narrow row retains a usable track");
				assert_equal(true, track.right <= edit.left && edit.width() > 0,
				             "track and numeric field remain separate");
				assert_equal(true,
				             track.left >= bounds.left && edit.right <= bounds.right && track.top >= bounds.top &&
				                 track.bottom <= bounds.bottom && edit.top >= bounds.top &&
				                 edit.bottom <= bounds.bottom,
				             "controls fit the measured row");

				slot_test_draw_context dc({});
				dc.col_widths = mc.col_widths;
				slider.render(dc, {});
				// The row draws its label, then the slider and its box draw themselves - the box its value.
				if (!label.empty())
				{
					const std::string label_text(label);
					assert_equal(1_z, static_cast<size_t>(std::ranges::count(dc.text_values, label_text)),
					             "the label is drawn once");
					const auto text =
						dc.text_bounds[std::ranges::find(dc.text_values, label_text) - dc.text_values.begin()];
					assert_equal(true, text.right <= track.left || text.bottom <= track.top,
					             "the label does not cover the slider or numeric field");
					assert_equal(true,
					             text.left >= bounds.left && text.right <= bounds.right && text.top >= bounds.top &&
					                 text.bottom <= bounds.bottom,
					             "the label fits the measured row");
				}
				assert_equal(0, value, "reflow does not change the setting");
			}
		}
	}
}

// The buttons along a dialog's foot are each padding2 larger than they ask and at least a fifth of the
// dialog's width, padding2 apart and centred together; a lone Close is the same row of one. A frame that
// does not exist makes them, and they measure as any other button does.
static void should_lay_out_dialog_buttons_in_a_centred_row()
{
	slider_test_measure_context mc;
	const auto& frame = ui::no_frame();

	const auto ok_cancel = std::make_shared<ui::ok_cancel_control>(frame, "Save", "A much longer cancel");
	const auto buttons = tree_controls(ok_cancel);
	assert_equal(2_z, buttons.size(), "both buttons are in the row's tree, which its frame finds them in");

	const auto save = buttons[0]->measure_content(mc, 500);
	const auto cancel = buttons[1]->measure_content(mc, 500);
	assert_equal(true, save.cx + 8 < 100 && cancel.cx + 8 > 100, "one name is short and one long");

	const auto height = std::max(save.cy, cancel.cy) + 8;
	const auto extent = ok_cancel->measure(mc, 500);
	assert_equal(500, extent.cx, "the row spans the dialog");
	assert_equal(height, extent.cy, "as tall as its taller button with padding2 added");

	ok_cancel->layout(mc, recti(10, 20, 510, 20 + height));

	const auto cancel_width = cancel.cx + 8;
	const auto left = 10 + (500 - (100 + 8 + cancel_width)) / 2;
	assert_equal(true, buttons[0]->bounds == recti(left, 20, left + 100, 20 + height),
	             "the short one is a fifth of the width");
	assert_equal(true, buttons[1]->bounds == recti(left + 108, 20, left + 108 + cancel_width, 20 + height),
	             "the long one its own width with padding2, padding2 after the first");

	// A hidden row hides the buttons it holds, though they are not hidden themselves: its frame does not
	// find them.
	ok_cancel->is_visible(false);
	assert_equal(true, tree_controls(ok_cancel).empty(), "a hidden row keeps its buttons hidden");
	assert_equal(2_z, tree_controls(ok_cancel, true).size(), "though they are still its own");
	ok_cancel->is_visible(true);

	const auto close = std::make_shared<ui::close_control>(frame, true, "Close");
	close->layout(mc, recti(10, 20, 510, 20 + close->measure(mc, 500).cy));
	const auto lone = tree_controls(close);
	assert_equal(1_z, lone.size(), "a lone button");
	assert_equal(true, lone[0]->bounds.left == 210 && lone[0]->bounds.width() == 100,
	             "is a fifth of the width, centred");

	close->text("Done");
	const auto done = std::static_pointer_cast<ui::button>(lone[0]);
	assert_equal("Done"sv, done->window_text(), "and keeps its text to change");
}

// A task panel's row too narrow for its buttons wraps them onto further lines, each centred and padding2 below the
// last, rather than clipping one at the panel's edge, as video Edit's Rotate row clipped Reset rotation.
static void should_wrap_a_panel_row_of_buttons_that_do_not_fit()
{
	slider_test_measure_context mc;
	const auto make = []
	{
		return std::vector<ui::control_ptr>{ui::make_button("Rotate left", [] {}),
		                                    ui::make_button("Rotate right", [] {}),
		                                    ui::make_button("Reset rotation", [] {})};
	};
	const auto width_of = [&mc](const ui::control_ptr& b)
	{
		return b->measure_content(mc, 500).cx + 8;
	};

	const auto buttons = make();
	const auto two = width_of(buttons[0]) + 8 + width_of(buttons[1]);
	const auto width = two + 8 + width_of(buttons[2]) / 2;

	const auto one_line = make();
	const auto foot = std::make_shared<ui::button_row>(one_line);
	foot->layout(mc, recti(10, 20, 10 + width, 20 + foot->measure(mc, width).cy));
	assert_equal(true, one_line[2]->bounds.right > 10 + width, "the width is too narrow for the three on one line");

	const auto row = std::make_shared<ui::button_row>(buttons, ui::button_row_fit::wrap);
	const auto extent = row->measure(mc, width);
	const recti bounds(10, 20, 10 + width, 20 + extent.cy);
	row->layout(mc, bounds);

	for (const auto& b : buttons)
	{
		assert_equal(true,
		             b->bounds.left >= bounds.left && b->bounds.right <= bounds.right && b->bounds.top >= bounds.top &&
		                 b->bounds.bottom <= bounds.bottom,
		             "every button is whole inside the row");
	}

	assert_equal(buttons[0]->bounds.top, buttons[1]->bounds.top, "the first two share a line");
	assert_equal(buttons[0]->bounds.bottom + 8, buttons[2]->bounds.top, "the last starts the next, padding2 below");
	assert_equal(bounds.left + bounds.right, buttons[2]->bounds.left + buttons[2]->bounds.right, "centred on it");
}

static void should_keep_the_unsaved_decision_outside_the_scrolling_body()
{
	const auto d = make_dlg(ui::no_frame());
	const auto actions = ui::save_discard_cancel([] {}, [] {}, [] {});
	d->_controls = {std::make_shared<text_element>("Unsaved changes"), std::make_shared<divider_element>(), actions};
	assert_equal(1_z, d->body_end(), "the divider and decision stay outside the scrolling body");
	const auto controls = tree_controls(actions);
	assert_equal(3_z, controls.size(), "three choices");
	assert_equal(tt.button_save.sv(), std::static_pointer_cast<ui::button>(controls[0])->window_text(), "save first");
	assert_equal(tt.button_dont_save.sv(), std::static_pointer_cast<ui::button>(controls[1])->window_text(),
	             "discard second");
	assert_equal(tt.button_cancel.sv(), std::static_pointer_cast<ui::button>(controls[2])->window_text(),
	             "cancel last");
	assert_equal(true, controls[2]->traits.default_button, "asking to leave a project defaults to staying");
}

// A review cell's text too wide for its column ends in an ellipsis inside the column, cut between characters; text
// that fits is as it is, and a row redrawn at the same width fits nothing again. The test context measures 8 to a
// byte, and the ellipsis is three bytes.
static void should_fit_a_cell_text_to_its_column_with_an_ellipsis()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	const auto view = std::make_shared<columns_test_view>(s, std::make_shared<detached_test_host>());
	view->add_row("IMG_0001.jpg", "\xC3\x89t\xC3\xA9 \xC3\xA0 Paris.jpg");
	const auto& row = view->_rows.front();
	slot_test_draw_context mc({});

	assert_equal("IMG_0001.jpg"s, std::string(row->fitted_text(mc, 1, 96)), "a name that fits is as it is");
	assert_equal("IMG_0001\xE2\x80\xA6"s, std::string(row->fitted_text(mc, 1, 95)),
	             "one a pixel too wide keeps what fits beside the ellipsis");
	assert_equal(true, row->fitted_text(mc, 1, 95).data() == row->_fitted[1].text.data(),
	             "and is fitted once for its width");
	// Room for four bytes before the ellipsis, where the fourth is half of the second e.
	assert_equal("\xC3\x89t\xE2\x80\xA6"s, std::string(row->fitted_text(mc, 3, 56)), "and never half a character");
}

// A number typed into a slider's box is held to the slider's range, as the slider itself is. What it is
// bound to takes nothing else: a WebP quality typed as 150 failed every save that used it.
static void should_hold_a_typed_slider_value_to_its_range()
{
	auto value = 70;
	ui::slider_control slider(ui::no_frame(), {}, value, 1, 100);

	slider.edit_change("150");
	assert_equal(100, value, "above the range");
	slider.edit_change("-5");
	assert_equal(1, value, "below it");
	slider.edit_change("42");
	assert_equal(42, value, "within it");
}

// A length held in tenths of a second reads, and is typed, in seconds to a tenth, with a point or a comma, and is
// held to the slider's range as a whole number is.
static void should_show_and_take_a_slider_value_in_tenths_as_seconds()
{
	auto value = 40;
	ui::slider_control slider(ui::no_frame(), {}, value, 1, 600, {}, {}, ui::slider_units::tenths);
	ui::edit_ptr box;
	slider.visit_children([&box](const view_element_ptr& e)
	{
		if (const auto edit = std::dynamic_pointer_cast<ui::edit>(e)) box = edit;
	});
	assert_equal(true, box != nullptr, "the row has its box");
	assert_equal("4.0"sv, box->window_text(), "forty tenths show as seconds");

	box->window_text(std::string_view{});
	box->on_text("2.5");
	assert_equal(25, value, "the box takes a typed point");
	slider.edit_change("7,3");
	assert_equal(73, value, "and reads a comma as one");
	slider.edit_change("0");
	assert_equal(1, value, "held to a tenth");
	slider.edit_change("1e999");
	assert_equal(600, value, "and to a minute, however long the typed number");

	value = 125;
	slider.show_bound_value();
	assert_equal("12.5"sv, box->window_text(), "a value its owner sets shows in seconds");
}

// The title bar keeps the title it is given. A task view builds its title afresh each time it is asked - an agent's
// task_state asks between layouts - and a title bar that only borrowed the last one drew freed memory.
static void should_draw_the_title_it_was_given_after_its_source_changes()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	app_logo_element logo(s, render::font_face::title, false, false);

	auto title = std::string("Diffractor: Convert or resize");
	logo.text(title);
	// The same length, so the same buffer, changed under the title bar.
	title.replace(0, 10, "XXXXXXXXXX");
	logo.bounds = {0, 0, 400, 30};

	slot_test_draw_context dc({});
	logo.render(dc, {});
	assert_equal(1_z, dc.text_values.size(), "the title bar draws its title");
	assert_equal("Diffractor: Convert or resize"s, dc.text_values.front(), "as it was given");
}

static void should_merge_control_colours_as_straight_alpha()
{
	const auto near = [](const float expected, const float actual, const std::string_view message)
	{
		assert_equal(true, std::abs(expected - actual) < 0.0001f, message);
	};

	core::color transparent_red(1.0f, 0.0f, 0.0f, 0.0f);
	transparent_red.merge(core::color(0.0f, 0.0f, 1.0f, 1.0f));
	near(0.0f, transparent_red.r, "transparent source leaves destination red");
	near(0.0f, transparent_red.g, "transparent source leaves destination green");
	near(1.0f, transparent_red.b, "transparent source leaves destination blue");
	near(1.0f, transparent_red.a, "transparent source leaves destination alpha");

	core::color opaque_red(1.0f, 0.0f, 0.0f, 1.0f);
	opaque_red.merge(core::color(0.0f, 0.0f, 1.0f, 0.0f));
	near(1.0f, opaque_red.r, "opaque source keeps red");
	near(0.0f, opaque_red.b, "opaque source hides transparent destination");
	near(1.0f, opaque_red.a, "opaque source keeps alpha");

	core::color zero(0.4f, 0.2f, 0.1f, 0.0f);
	zero.merge(core::color(0.8f, 0.1f, 0.2f, 0.0f));
	near(0.0f, zero.r, "zero over zero clears red");
	near(0.0f, zero.g, "zero over zero clears green");
	near(0.0f, zero.b, "zero over zero clears blue");
	near(0.0f, zero.a, "zero over zero clears alpha");

	core::color half_red(1.0f, 0.0f, 0.0f, 0.5f);
	half_red.merge(core::color(0.0f, 0.0f, 1.0f, 0.5f));
	near(2.0f / 3.0f, half_red.r, "half source normalizes red");
	near(0.0f, half_red.g, "half source keeps green zero");
	near(1.0f / 3.0f, half_red.b, "half source normalizes blue");
	near(0.75f, half_red.a, "half source computes source-over alpha");
}

static void should_hit_test_comparison_divider_in_device_coordinates()
{
	const recti logical_bounds{100, 100, 120, 300};
	const auto shifted = comparison_divider_device_bounds(logical_bounds, {0, -50});
	assert_equal(true, shifted == recti(100, 50, 120, 250), "divider bounds shift to device coordinates");
	assert_equal(true, shifted.contains({110, 75}), "visible shifted point hits divider");
	assert_equal(false, shifted.contains({110, 25}), "point above painted divider misses");

	const auto unshifted = comparison_divider_device_bounds(logical_bounds, {});
	assert_equal(true, unshifted == logical_bounds, "zero offset preserves divider bounds");
}

static void should_clear_detail_row_layout_metrics()
{
	df::item_row_draw_info info;
	info.title.extent = 400;
	info.title.width = 300;
	info.file_size.extent = 100;
	info.file_size.width = 80;
	info.file_size.val_min = 10;
	info.file_size.val_max = 1000;
	info.presence.extent = 40;
	info.presence.width = 40;

	info.clear_for_layout();

	assert_equal(0, info.title.extent, "title extent reset");
	assert_equal(0, info.title.width, "title width reset");
	assert_equal(0, info.file_size.extent, "size extent reset");
	assert_equal(0, info.file_size.width, "size width reset");
	assert_equal(static_cast<double>(INT64_MAX), info.file_size.val_min, "size minimum reset");
	assert_equal(static_cast<double>(INT64_MIN), info.file_size.val_max, "size maximum reset");
	assert_equal(0, info.presence.extent, "presence extent reset");
	assert_equal(0, info.presence.width, "presence width reset");
}

static void should_classify_aspect_ratio_groups()
{
	const auto assert_group = [](const sizei dimensions, const aspect_ratio_bucket expected_bucket,
	                             const bool expected_portrait, const std::string_view name)
	{
		const auto actual = calc_aspect_ratio_group(dimensions);
		assert_equal(static_cast<int>(expected_bucket), static_cast<int>(actual.bucket), name, "aspect ratio bucket");
		assert_equal(expected_portrait, actual.is_portrait, name, "aspect ratio orientation");
	};

	assert_group({1000, 1000}, aspect_ratio_bucket::square, false, "square");
	assert_group({1280, 1024}, aspect_ratio_bucket::five_four, false, "5:4");
	assert_group({4000, 3000}, aspect_ratio_bucket::four_three, false, "4:3");
	assert_group({6000, 4000}, aspect_ratio_bucket::three_two, false, "3:2");
	assert_group({1920, 1200}, aspect_ratio_bucket::sixteen_ten, false, "16:10");
	assert_group({1920, 1080}, aspect_ratio_bucket::sixteen_nine, false, "16:9");
	assert_group({2520, 1080}, aspect_ratio_bucket::twenty_one_nine, false, "21:9");
	assert_group({1080, 1920}, aspect_ratio_bucket::sixteen_nine, true, "9:16 portrait");
	assert_group({4032, 3024}, aspect_ratio_bucket::four_three, false, "cropped within tolerance");
	assert_group({1000, 700}, aspect_ratio_bucket::other, false, "other landscape");
	assert_group({700, 1000}, aspect_ratio_bucket::other, true, "other portrait");
	assert_group({}, aspect_ratio_bucket::other, false, "invalid dimensions");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Flex layout
///////////////////////////////////////////////////////////////////////////////////////////////////

// Behaves like text: content that does not fit the width limit wraps onto a second line.
class flex_wrapping_test_element final : public view_element
{
	sizei _desired;

public:
	explicit flex_wrapping_test_element(const sizei desired) : _desired(desired) { padding(0); }

	sizei measure(render::measure_context& /*mc*/, const int width_limit) const override
	{
		if (width_limit >= _desired.cx) return _desired;
		return {width_limit, _desired.cy * 2};
	}
};

static void should_layout_flex_elements()
{
	flex_test_measure_context mc;
	auto first = std::make_shared<flex_test_element>(sizei{40, 10});
	auto second = std::make_shared<flex_test_element>(sizei{40, 20});
	auto third = std::make_shared<flex_test_element>(sizei{40, 15});
	const std::vector<view_element_ptr> wrapping{first, second, third};
	const auto wrapped = calc_flex_layout(wrapping, mc, {100, 0}, {});
	assert_equal(5, wrapped.layout_bounds[0].top, "short item is centered on first flex line");
	assert_equal(0, wrapped.layout_bounds[1].top, "second item remains on first flex line");
	assert_equal(20, wrapped.layout_bounds[2].top, "third item wraps to second flex line");
	assert_equal(35, wrapped.extent.cy, "wrapped flex extent");

	first->flex.grow = 1.0f;
	second->flex.grow = 1.0f;
	const std::vector<view_element_ptr> growing{first, second};
	const auto grown = calc_flex_layout(growing, mc, {100, 0}, {});
	assert_equal(50, grown.layout_bounds[0].width(), "first flex item shares free space");
	assert_equal(50, grown.layout_bounds[1].width(), "second flex item shares free space");

	first->flex.grow = 0.0f;
	second->flex.grow = 0.0f;
	second->flex.main_start_auto = true;
	const auto justified = calc_flex_layout(growing, mc, {100, 0}, {});
	assert_equal(60, justified.layout_bounds[1].left, "auto margin right aligns trailing flex item");

	second->is_visible(false);
	const auto hidden = calc_flex_layout(growing, mc, {100, 0}, {});
	assert_equal(40, hidden.extent.cx, "hidden flex item consumes no space");
	assert_equal(true, hidden.layout_bounds[1] == recti{}, "hidden flex item has empty bounds");

	second->is_visible(true);
	first->flex.shrink = 1.0f;
	second->flex.shrink = 1.0f;
	first->flex.basis = 60;
	second->flex.basis = 60;
	first->flex.min_size.cx = 55;
	second->flex.min_size.cx = 40;
	flex_container_layout no_wrap;
	no_wrap.wrap = flex_wrap::no_wrap;
	const auto shrunk = calc_flex_layout(growing, mc, {100, 0}, no_wrap);
	assert_equal(55, shrunk.layout_bounds[0].width(), "flex shrink respects first minimum");
	assert_equal(45, shrunk.layout_bounds[1].width(), "flex shrink redistributes after minimum");

	auto column_first = std::make_shared<flex_test_element>(sizei{30, 10});
	auto column_second = std::make_shared<flex_test_element>(sizei{40, 20});
	const std::vector<view_element_ptr> column_elements{column_first, column_second};
	flex_container_layout column;
	column.direction = flex_direction::column;
	column.wrap = flex_wrap::no_wrap;
	const auto measured_column = calc_flex_layout(column_elements, mc, {100, -1}, column);
	assert_equal(30, measured_column.extent.cy, "unconstrained flex column measures intrinsic height");
	assert_equal(10, measured_column.layout_bounds[1].top, "unconstrained flex column stacks items");

	mc.padding2 = 8;
	auto divider = std::make_shared<divider_element>();
	divider->padding(0);
	const std::vector<view_element_ptr> divided_column{divider, column_second};
	const auto fixed_height_divider = calc_flex_layout(divided_column, mc, {100, 100}, column);
	assert_equal(8, fixed_height_divider.layout_bounds[0].height(),
	             "column divider keeps its intrinsic height instead of consuming free space");
	assert_equal(8, fixed_height_divider.layout_bounds[1].top,
	             "control after a column divider remains adjacent and visible");
	mc.padding2 = 0;

	column_first->flex.break_after = true;
	const auto broken_column = calc_flex_layout(column_elements, mc, {100, -1}, column);
	assert_equal(30, broken_column.layout_bounds[1].left,
	             "explicit break creates a new column when automatic wrapping is disabled");

	column_first->flex.break_after = false;
	column_first->flex.align_self = flex_align::stretch;
	column.padding = {5, 7};
	const auto padded_column = calc_flex_layout(column_elements, mc, {100, -1}, column);
	assert_equal(5, padded_column.layout_bounds[0].left, "column padding offsets the first item");
	assert_equal(90, padded_column.layout_bounds[0].width(), "explicit stretch fills the column cross axis");
	assert_equal(44, padded_column.extent.cy, "column padding contributes to intrinsic extent");

	mc.scale_factor = 2.0;
	const auto scaled_padded_column = calc_flex_layout(column_elements, mc, {200, -1}, column);
	assert_equal(10, scaled_padded_column.layout_bounds[0].left, "flex padding scales from logical units");
	assert_equal(58, scaled_padded_column.extent.cy, "scaled flex padding contributes once to intrinsic extent");
	mc.scale_factor = 1.0;

	const auto applied_extent = layout_flex_elements(column_elements, mc, {10, 20, 110, 80}, column);
	assert_equal(15, column_first->bounds.left, "applied flex layout offsets child bounds");
	assert_equal(padded_column.extent.cy, applied_extent.cy, "applied flex layout returns content extent");

	column.padding = {};
	column.justify = flex_justify::center;
	const auto centered_column = calc_flex_layout(column_elements, mc, {100, 100}, column);
	assert_equal(35, centered_column.layout_bounds[0].top, "flex column centers intrinsic content vertically");

	column.justify = flex_justify::start;
	column_first->flex.align_self = flex_align::automatic;
	column_first->flex = column_first->flex | flex_item::media;
	column_first->flex.basis = 100;
	const auto shrunk_column = calc_flex_layout(column_elements, mc, {100, 100}, column);
	assert_equal(80, shrunk_column.layout_bounds[0].height(), "flex column shrinks media to fit trailing controls");
	assert_equal(80, shrunk_column.layout_bounds[1].top, "trailing control remains visible after media shrink");

	auto fixed = std::make_shared<flex_test_element>(sizei{30, 10});
	auto flexible_first = std::make_shared<flex_test_element>(sizei{40, 10});
	auto flexible_second = std::make_shared<flex_test_element>(sizei{40, 10});
	fixed->flex.basis = 30;
	fixed->flex.shrink = 1.0f;
	flexible_first->flex.basis = 0;
	flexible_first->flex.grow = 1.0f;
	flexible_second->flex.basis = 0;
	flexible_second->flex.grow = 1.0f;
	flex_container_layout columns;
	columns.wrap = flex_wrap::no_wrap;
	columns.gap.cx = 5;
	const auto column_row = calc_flex_layout(std::vector<view_element_ptr>{fixed, flexible_first, flexible_second}, mc,
	                                         {100, -1}, columns);
	assert_equal(30, column_row.layout_bounds[0].width(), "fixed flex column keeps its basis");
	assert_equal(30, column_row.layout_bounds[1].width(), "first flexible column shares remaining width");
	assert_equal(30, column_row.layout_bounds[2].width(), "second flexible column shares remaining width");

	auto capped = std::make_shared<flex_wrapping_test_element>(sizei{40, 10});
	capped->flex.max_size.cx = 25;
	capped->flex.align_self = flex_align::center;
	const std::vector<view_element_ptr> capped_elements{capped};
	const auto capped_column = calc_flex_layout(capped_elements, mc, {100, -1}, column);
	assert_equal(25, capped_column.layout_bounds[0].width(), "column item is capped by its maximum width");
	assert_equal(20, capped_column.extent.cy, "column item measures inside its maximum width");

	const auto capped_row = calc_flex_layout(capped_elements, mc, {100, -1}, {});
	assert_equal(25, capped_row.layout_bounds[0].width(), "row item is capped by its maximum width");
	assert_equal(20, capped_row.extent.cy, "row item measures inside its maximum width");

	auto padded_first = std::make_shared<flex_test_element>(sizei{20, 10});
	auto padded_second = std::make_shared<flex_test_element>(sizei{20, 10});
	flex_container_layout centered_row;
	centered_row.wrap = flex_wrap::no_wrap;
	centered_row.justify = flex_justify::center;
	centered_row.padding = {10, 0};
	const auto centered = calc_flex_layout(std::vector<view_element_ptr>{padded_first, padded_second}, mc, {100, -1},
	                                       centered_row);
	assert_equal(30, centered.layout_bounds[0].left, "centred line keeps the container's leading padding");
	assert_equal(100, centered.extent.cx, "a justified line reports the whole main axis it positions within");

	auto capped_cross = std::make_shared<flex_test_element>(sizei{20, 10});
	capped_cross->flex.align_self = flex_align::stretch;
	capped_cross->flex.max_size.cy = 12;
	auto tall = std::make_shared<flex_test_element>(sizei{20, 40});
	flex_container_layout stretch_row;
	stretch_row.wrap = flex_wrap::no_wrap;
	const auto stretched = calc_flex_layout(std::vector<view_element_ptr>{capped_cross, tall}, mc, {100, -1},
	                                        stretch_row);
	assert_equal(12, stretched.layout_bounds[0].height(), "stretch respects the item's maximum cross size");

	auto only_child = std::make_shared<flex_test_element>(sizei{20, 10});
	only_child->is_visible(false);
	flex_container_layout hidden_column;
	hidden_column.direction = flex_direction::column;
	hidden_column.padding = {10, 10};
	const auto nothing_visible = calc_flex_layout(std::vector<view_element_ptr>{only_child}, mc, {100, -1},
	                                              hidden_column);
	assert_equal(0, nothing_visible.extent.cx, "a container with nothing visible occupies no width");
	assert_equal(0, nothing_visible.extent.cy, "a container with nothing visible occupies no height");
}

// A flex line measures a picture again at the width it has just given it. Holding the picture's longer side to the
// width offered shrank a portrait on that second pass, and a bubble drew it squashed into a square.
static void should_keep_a_pictures_shape_in_a_flex_line()
{
	struct restore_rotated
	{
		bool kept = setting.show_rotated;
		~restore_rotated() { setting.show_rotated = kept; }
	} restore;
	setting.show_rotated = true;

	flex_test_measure_context mc;
	const auto shape = [&mc](const sizei stored, const core::orientation orientation, const int width)
	{
		const auto surface = std::make_shared<core::surface>();
		surface->alloc(stored, core::texture_format::RGB, orientation);
		const auto picture = std::make_shared<surface_element>(surface, 40, flex_item::center | flex_item::new_line);
		picture->padding(0);
		const auto b = calc_flex_layout(std::vector<view_element_ptr>{picture}, mc, {width, -1}, {}).layout_bounds[0];
		return std::format("{}x{}", b.width(), b.height());
	};

	assert_equal("30x40"sv, shape({60, 80}, core::orientation::top_left, 100), "a portrait keeps its shape");
	assert_equal("30x40"sv, shape({80, 60}, core::orientation::left_bottom, 100),
	             "as does one stored on its side, turned as its pixels say");
	assert_equal("40x30"sv, shape({80, 60}, core::orientation::top_left, 100), "a landscape keeps its shape");
	assert_equal("15x20"sv, shape({60, 80}, core::orientation::top_left, 15),
	             "a portrait wider than the line is as wide as the line");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Item tile geometry
///////////////////////////////////////////////////////////////////////////////////////////////////

// selection-controls.md: comparison is like with like, decided from stable traits alone.
static void should_limit_comparison_to_like_pairs()
{
	const auto jpg = files::file_type_from_name("a.jpg");
	const auto png = files::file_type_from_name("b.png");
	const auto mp4 = files::file_type_from_name("a.mp4");
	const auto mp3 = files::file_type_from_name("a.mp3");
	const auto txt = files::file_type_from_name("a.txt");

	assert_equal(true, can_compare_file_types(jpg, png), "two images compare");
	assert_equal(true, can_compare_file_types(mp4, mp4), "two previewable videos compare");
	assert_equal(false, can_compare_file_types(jpg, mp4), "image and video do not compare");
	assert_equal(false, can_compare_file_types(mp3, mp3), "two audio files do not compare");
	assert_equal(false, can_compare_file_types(txt, txt), "two documents do not compare");
	assert_equal(false, can_compare_file_types(&file_type::folder, &file_type::folder), "two folders do not compare");
	assert_equal(false, can_compare_file_types(nullptr, jpg), "an unknown type does not compare");
}

// A video with embedded cover art draws the cover art on its tile, so the tile has to be the shape
// of the art. Sizing it from the video's own dimensions letterboxed the art inside a frame-shaped
// tile, and the republish that follows every scan pass used to revert it to the video shape.
static void should_shape_the_tile_by_what_it_draws()
{
	df::index_file_item indexed;
	indexed.name = core::cache("clip.mp4");
	indexed.ft = files::file_type_from_name(indexed.name);

	const auto md = std::make_shared<prop::item_metadata>();
	md->width = 1920;
	md->height = 1080;
	indexed.metadata.store(md);

	const auto path = core::file_path("c:\\clip.mp4");
	const auto item = std::make_shared<df::item_element>(path, indexed);

	assert_equal(1920, item->layout_dims().cx, "video width shapes the tile before cover art arrives");
	assert_equal(1080, item->layout_dims().cy, "video height shapes the tile before cover art arrives");
	assert_equal(true, item->layout_aspect_known(), "indexed dimensions are an exact aspect");

	const auto cover_art = std::make_shared<core::image>(core::blob(16), sizei(600, 600), core::image_format::JPEG,
	                                                     core::orientation::top_left);
	item->thumbnail({}, cover_art, {});

	assert_equal(600, item->layout_dims().cx, "cover art width shapes the tile that draws it");
	assert_equal(600, item->layout_dims().cy, "cover art height shapes the tile that draws it");
	assert_equal(true, item->layout_aspect_known(), "a cover art aspect is exact, so the tile may justify");

	// scan_items republishes every displayed item on every pass.
	const auto republished = item->update(path, indexed);

	assert_equal(600, item->layout_dims().cx, "a republish does not revert the tile to the video shape");
	assert_equal(false, republished, "an unchanged republish asks for no layout pass");
}

// A thumbnail is a downscaled stand-in, so it may shape a tile whose real aspect is unknown but must
// never earn the row justification that a known aspect does.
static void should_not_justify_a_tile_shaped_by_a_thumbnail()
{
	df::index_file_item indexed;
	indexed.name = core::cache("unscanned.jpg");
	indexed.ft = files::file_type_from_name(indexed.name);

	const auto path = core::file_path("c:\\unscanned.jpg");
	const auto item = std::make_shared<df::item_element>(path, indexed);

	assert_equal(false, item->layout_aspect_known(), "nothing known before a scan");

	const auto thumb = std::make_shared<core::image>(core::blob(16), sizei(160, 120), core::image_format::JPEG,
	                                                 core::orientation::top_left);
	item->thumbnail(thumb, {}, {});

	assert_equal(160, item->layout_dims().cx, "the thumbnail stands in for an unknown aspect");
	assert_equal(false, item->layout_aspect_known(), "a stand-in aspect is never justified");

	// The scan lands and the index now knows the real size, which outranks the thumbnail.
	const auto md = std::make_shared<prop::item_metadata>();
	md->width = 4000;
	md->height = 3000;
	indexed.metadata.store(md);

	assert_equal(true, item->update(path, indexed), "a newly scanned size asks for a layout pass");
	assert_equal(4000, item->layout_dims().cx, "the indexed size outranks the thumbnail");
	assert_equal(true, item->layout_aspect_known(), "the real aspect is now known");
}

// A tile fills its cell by cropping, so every difference between the cell's shape and the image's
// shape is hidden pixels. The row solves one height and takes each width from it, which is what stops
// a row holding one or two portraits from cutting the top and bottom off them.
// A show plays in season and episode order, a special ahead of its season's first episode; an album in disc and
// track order - whatever the files are named. Unnumbered items follow by name.
static void should_order_shows_and_albums_by_number()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto make_item = [](const std::string& name, const std::function<void(prop::item_metadata&)>& fill)
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(indexed.name);
		const auto md = std::make_shared<prop::item_metadata>();
		fill(*md);
		indexed.metadata.store(md);
		return std::make_shared<df::item_element>(core::file_path("c:\\" + name), indexed);
	};

	const auto episode = [](const int season, const int number, const bool special)
	{
		return [=](prop::item_metadata& md)
		{
			md.show = core::cache("Show");
			md.season = static_cast<uint16_t>(season);
			md.episode = prop::number_pair(number, 0);
			md.is_special = special;
			md.kind = prop::media_kind::episode;
		};
	};

	const auto track = [](const int disc, const int number)
	{
		return [=](prop::item_metadata& md)
		{
			md.album = core::cache("Album");
			md.disk = prop::number_pair(disc, 0);
			md.track = prop::number_pair(number, 0);
		};
	};

	const auto order_of = [&s](df::item_elements items)
	{
		const auto group = std::make_shared<df::item_group>(s, std::move(items), df::item_group_display::icons,
		                                                    df::group_key{});
		group->sort(group_by::album_show, sort_by::def, false);
		std::vector<std::string> names;
		for (const auto& i : group->items()) names.emplace_back(i->name());
		return core::combine(names, ",", false);
	};

	assert_equal("z.mkv,a.mkv,m.mkv,b.mkv,c.mkv"s,
	             order_of({
					 make_item("c.mkv", [](prop::item_metadata& md) { md.show = core::cache("Show"); }),
					 make_item("b.mkv", episode(2, 1, false)),
					 make_item("a.mkv", episode(1, 2, false)),
					 make_item("m.mkv", episode(2, 0, true)),
					 make_item("z.mkv", episode(1, 1, false)),
				 }),
	             "episodes in season order, the special first");

	assert_equal("Wind.mp3,Rain.mp3,Sun.mp3"s,
	             order_of({
					 make_item("Sun.mp3", track(2, 1)),
					 make_item("Rain.mp3", track(1, 2)),
					 make_item("Wind.mp3", track(1, 1)),
				 }),
	             "tracks in disc order");
}

// A detail group is headed by its columns, which sort that group alone - its own way first, then the other - and
// size their columns, the name taking up the room; a sort from the toolbar sets the headings' sorts aside.
static void should_sort_and_size_a_detail_group_from_its_headings()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto make_item = [](const std::string& name, const int duration)
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(indexed.name);
		indexed.size = core::file_size{static_cast<uint64_t>(duration) * 1000};
		const auto md = std::make_shared<prop::item_metadata>();
		md->duration = static_cast<uint16_t>(duration);
		indexed.metadata.store(md);
		return std::make_shared<df::item_element>(core::file_path("c:\\" + name), indexed);
	};

	const auto make_group = [&](const df::group_key& key)
	{
		return std::make_shared<df::item_group>(s,
		                                        df::item_elements{make_item("b.mp3", 30), make_item("c.mp3", 200),
		                                                          make_item("a.mp3", 90)},
		                                        df::item_group_display::detail, key);
	};
	const auto names = [](const df::item_group& g)
	{
		std::vector<std::string> result;
		for (const auto& i : g.items()) result.emplace_back(i->name());
		return core::combine(result, ",", false);
	};

	df::group_key audio(group_key_type::audio);
	df::group_key other(group_key_type::other);
	const auto group = make_group(audio);
	const auto neighbour = make_group(other);
	const auto resort = [&]
	{
		for (const auto& g : {group, neighbour}) g->sort(group_by::file_type, sort_by::name, false);
	};
	resort();
	assert_equal("a.mp3,b.mp3,c.mp3"s, names(*group), "the toolbar's order until a heading is clicked");

	// Laid out and drawn, so the headings are where a press finds them.
	listing_draw_context dc({0, 0, 800, 1000});
	const auto draw = [&]
	{
		const auto extent = group->measure(dc, 800);
		group->layout(dc, recti(0, 0, 800, extent.cy));
		for (const auto& i : group->items()) group->update_detail_row_layout(dc, i, false);
		group->update_row_layout(dc);
		group->render_headings(dc, {}, recti(0, 0, 800, 1000));
	};
	draw();
	const auto band = group->_heading_drawn;
	assert_equal(true, !band.is_empty() && band.top == group->bounds.top, "the headings head the group");
	assert_equal(true, group->items().front()->bounds.top >= band.bottom, "over its rows");

	const auto cell_of = [&group](const df::detail_column c)
	{
		const auto found = std::ranges::find(group->_heading_cells, c, &df::item_group::heading_cell::column);
		return found == group->_heading_cells.end() ? recti{} : found->bounds;
	};
	const auto duration_cell = cell_of(df::detail_column::duration);
	assert_equal(true, !duration_cell.is_empty() && !cell_of(df::detail_column::title).is_empty(),
	             "a heading for the name and for each column the rows fill");

	const auto host = std::make_shared<detached_test_host>();
	const auto press = [&](const pointi at, const pointi release)
	{
		hit_test_context ctx(at, recti(0, 0, 800, 1000));
		const auto controller = group->controller_from_location(host, at, {}, ctx);
		if (controller)
		{
			controller->on_mouse_left_button_down(at, {});
			controller->on_mouse_left_button_up(release, {});
		}
		return controller;
	};

	const auto heading_press = press(duration_cell.center(), duration_cell.center());
	assert_equal(true, heading_press && heading_press->cursor() == platform::cursor::link, "a heading answers a click");
	resort();
	assert_equal("c.mp3,a.mp3,b.mp3"s, names(*group), "a length sorts longest first");
	assert_equal("a.mp3,b.mp3,c.mp3"s, names(*neighbour), "and only the group whose heading it is");

	// The listing the sort rebuilds stays where it was scrolled to: the group is where it was, and only its rows
	// moved. Any other rebuild keeps the row in the middle of the view where it is.
	assert_equal(true, s.take_items_scroll_hold(), "a heading's sort holds the listing's place");
	const auto view_host = std::make_shared<detached_test_host>();
	items_view items(s, view_host);
	items._items_scroller.layout({800, 4000}, {0, 0, 800, 600}, {});
	items._items_scroller.scroll_offset(view_host, 0, 1000);
	items._layout_center_item = group->items().front();
	group->items().front()->bounds = recti(0, 1200, 800, 1220);
	items.items_changed(false);
	assert_equal(true, items._pending_items_anchor.element == group->items().front(),
	             "a rebuild keeps the middle row where it is");
	press(duration_cell.center(), duration_cell.center());
	items.items_changed(false);
	assert_equal(true, items._pending_items_anchor.valid && !items._pending_items_anchor.element,
	             "but one for a heading's sort keeps the listing's place instead");
	assert_equal(false, s.take_items_scroll_hold(), "for that rebuild alone");
	resort();
	assert_equal("b.mp3,a.mp3,c.mp3"s, names(*group), "a second click sorts the other way");

	press(duration_cell.center(), duration_cell.center());
	s.take_items_scroll_hold();
	resort();
	assert_equal("a.mp3,b.mp3,c.mp3"s, names(*group), "and a third gives the group back the toolbar's order");
	assert_equal(-1, s.detail_choice(audio)->sort_column, "with no heading's sort shown");

	press(duration_cell.center(), duration_cell.center());
	s.take_items_scroll_hold();
	press(duration_cell.center(), duration_cell.center());
	s.take_items_scroll_hold();
	resort();
	assert_equal("b.mp3,a.mp3,c.mp3"s, names(*group), "a fourth click starts again from the column's own way");
	press(duration_cell.center(), {duration_cell.center().x, duration_cell.bottom + 200});
	resort();
	assert_equal("b.mp3,a.mp3,c.mp3"s, names(*group), "a press let go off the heading sorts nothing");

	// Group by type builds the group again for the same key, and it keeps its order.
	const auto rebuilt = make_group(audio);
	rebuilt->sort(group_by::file_type, sort_by::name, false);
	assert_equal("b.mp3,a.mp3,c.mp3"s, names(*rebuilt), "a group built again for the same key keeps its sort");

	// A column after the name is sized from the divider before it, the name taking up the room.
	draw();
	const auto dividers = group->heading_dividers();
	const auto divider = std::ranges::find(dividers, df::detail_column::duration,
	                                       &std::pair<df::detail_column, recti>::first);
	assert_equal(true, divider != dividers.end(), "a column the user can size has a divider");
	const auto width = group->_row_draw_info.duration.width;
	const auto name_width = group->_row_draw_info.title.width;
	const auto grip = divider->second.center();
	hit_test_context grip_ctx(grip, recti(0, 0, 800, 1000));
	const auto drag = group->controller_from_location(host, grip, {}, grip_ctx);
	assert_equal(true, drag && drag->cursor() == platform::cursor::left_right, "the divider answers a drag");
	drag->on_mouse_left_button_down(grip, {});
	drag->on_mouse_move({grip.x - 30, grip.y});
	drag->on_mouse_left_button_up({grip.x - 30, grip.y}, {});
	draw();
	assert_equal(width + 30, group->_row_draw_info.duration.width, "dragged towards the name, the column widens");
	assert_equal(name_width - 30, group->_row_draw_info.title.width, "and the name gives it the room");
	drag->on_mouse_left_button_double_click(grip, {});
	draw();
	assert_equal(width, group->_row_draw_info.duration.width, "a double-click fits it to what it holds again");

	s.group_order({}, sort_by::name);
	resort();
	assert_equal("a.mp3,b.mp3,c.mp3"s, names(*group), "a sort from the toolbar is every group's");
	assert_equal(-1, s.detail_choice(audio)->sort_column, "and sets the headings' sorts aside");
}

// The item under the pointer is marked at once: a fade trails a pointer moving down a list.
static void should_mark_the_hovered_item_at_once()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto make_item = [](const std::string& name)
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(indexed.name);
		return std::make_shared<df::item_element>(core::file_path("c:\\" + name), indexed);
	};

	const auto first = make_item("first.mp3");
	const auto second = make_item("second.mp3");
	const auto host = std::make_shared<detached_test_host>();

	s.hover_item(host, first, true);
	assert_equal(true, first->_bg_color.a > 0.0f, "the hovered item is marked the moment the pointer arrives");
	assert_equal(false, ui::animations.contains(first.get()), "with no fade to wait for");
	assert_equal(true, host->invalidations > 0, "and the view repaints to show it");

	s.hover_item(host, second, true);
	assert_equal(0.0f, first->_bg_color.a, "the item left is unmarked at once");
	assert_equal(true, second->_bg_color.a > 0.0f, "as the next is marked");
	assert_equal(false, ui::animations.contains(first.get()) || ui::animations.contains(second.get()), "neither fades");

	s.hover_item(host, second, false);
	assert_equal(0.0f, second->_bg_color.a, "and the last when the pointer leaves");
}

// Play goes on to the show's next episode by season and number: not to another copy of the one that ended, as the
// 1080p file kept beside the 720p one is.
static void should_play_on_to_the_next_episode()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	const view_host_base_ptr view;

	const auto make_episode = [](const std::string& name, const int number, const core::date_t created)
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(indexed.name);
		const auto md = std::make_shared<prop::item_metadata>();
		md->show = core::cache("Show");
		md->season = 1;
		md->episode = prop::number_pair(number, 0);
		md->kind = prop::media_kind::episode;
		md->dates.add_utc(prop::date_source::file_created, created);
		indexed.metadata.store(md);
		return std::make_shared<df::item_element>(core::file_path("c:\\" + name), indexed);
	};

	const auto sd = make_episode("Show.S01E05.720p.mkv", 5, core::date_t(2020, 1, 1, 0, 0, 0));
	const auto hd = make_episode("Show.S01E05.1080p.mkv", 5, core::date_t(2021, 1, 1, 0, 0, 0));
	const auto six = make_episode("Show.S01E06.720p.mkv", 6, core::date_t(2019, 1, 1, 0, 0, 0));
	s.append_items(view, df::item_set(df::item_elements{sd, hd, six}), {}, true, true);

	assert_equal(true, s.next_episode(sd) == six, "the next episode, not another copy of this one");
	assert_equal(true, s.next_episode(hd) == six, "from either copy");
	assert_equal(true, s.next_episode(six) == nullptr, "and none after the last");
}

static void should_keep_tile_aspect_when_laying_out_a_row()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	flex_test_measure_context mc;
	constexpr auto width_limit = 1200;
	constexpr auto crop_tolerance = 0.05;
	auto next_name = 0;

	const auto make_item = [&next_name](const int cx, const int cy)
	{
		const auto name = "item" + std::to_string(++next_name) + ".jpg";

		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(indexed.name);

		const auto md = std::make_shared<prop::item_metadata>();
		md->width = static_cast<uint16_t>(cx);
		md->height = static_cast<uint16_t>(cy);
		indexed.metadata.store(md);

		return std::make_shared<df::item_element>(core::file_path("c:\\" + name), indexed);
	};

	const auto assert_row_aspects = [&](df::item_elements items, const std::string_view name)
	{
		const auto group = std::make_shared<df::item_group>(s, std::move(items), df::item_group_display::icons,
		                                                    df::group_key{});
		group->measure(mc, width_limit);

		const auto& layout_bounds = group->_layout_bounds;
		assert_equal(group->items().size(), layout_bounds.size(), name, "a cell for every item");

		for (auto i = 0u; i < layout_bounds.size(); ++i)
		{
			const auto dims = group->items()[i]->layout_dims();
			const auto image_aspect = static_cast<double>(dims.cx) / dims.cy;
			const auto cell_aspect = static_cast<double>(layout_bounds[i].width()) / layout_bounds[i].height();
			const auto hidden = 1.0 - std::min(image_aspect, cell_aspect) / std::max(image_aspect, cell_aspect);

			assert_equal(true, hidden <= crop_tolerance + 0.01, name, "tile keeps its aspect");
			assert_equal(true, layout_bounds[i].right <= width_limit, name, "tile stays inside the row");
		}
	};

	assert_row_aspects({make_item(2000, 3000)}, "one portrait"sv);
	assert_row_aspects({make_item(2000, 3000), make_item(2000, 3000)}, "two portraits"sv);
	assert_row_aspects({make_item(3000, 2000), make_item(2000, 3000), make_item(4000, 3000)}, "mixed row"sv);
	assert_row_aspects({make_item(400, 400), make_item(64, 64)}, "small squares"sv);
	assert_row_aspects({make_item(12000, 1000)}, "panorama"sv);
}

// A header sort reorders the reviewed rows while the run stays in plan order, so progress and
// results must resolve a work index by identity. Indexing by display position reports one file's
// outcome against another.
static void should_resolve_list_rows_by_work_index()
{
	const auto resolve = [](const std::vector<int>& rows, const size_t work_index)
	{
		return review_list::row_for_work_index(rows.size(), work_index, [&rows](const size_t i) { return rows[i]; });
	};

	// Unsorted, which is the common case and the one the fast path answers without scanning.
	const std::vector<int> in_order{0, 1, 2, 3};
	assert_equal(0, resolve(in_order, 0), "in order work 0"sv);
	assert_equal(2, resolve(in_order, 2), "in order work 2"sv);
	assert_equal(3, resolve(in_order, 3), "in order work 3"sv);
	assert_equal(-1, resolve(in_order, 4), "in order beyond the run"sv);

	// Plan order 0,1,2,3 shown after a sort as 2,0,3,1.
	const std::vector<int> sorted{2, 0, 3, 1};
	assert_equal(1, resolve(sorted, 0), "sorted work 0"sv);
	assert_equal(3, resolve(sorted, 1), "sorted work 1"sv);
	assert_equal(0, resolve(sorted, 2), "sorted work 2"sv);
	assert_equal(2, resolve(sorted, 3), "sorted work 3"sv);

	// Rows the run skips carry -1, so they must never be selected - including for work index 0,
	// which a position-based lookup would hand to the first row.
	const std::vector<int> with_skipped{-1, -1, 0, -1, 1};
	assert_equal(2, resolve(with_skipped, 0), "first acting row"sv);
	assert_equal(4, resolve(with_skipped, 1), "second acting row"sv);

	// No row means no highlight, rather than the nearest one.
	assert_equal(-1, resolve(with_skipped, 2), "work index beyond the run"sv);
	assert_equal(-1, resolve({}, 0), "no rows"sv);
}

// The row being processed is held, not indexed. Reordering the reviewed list mid-run used to clear
// whichever row had since moved into the stored slot, leaving the real one highlighted for good.
static void should_track_the_active_row_across_a_reorder()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(test_files_folder()), {});
	s.update_item_groups();
	s.update_selection();
	s.select_all(view);
	s.update_selection();

	assert_equal(true, s.selected_items().size() > 2, "test folder has a selection to review");

	const auto tags = std::make_shared<tags_view>(s, nullptr);
	tags->activate({100, 100});

	assert_equal(true, tags->_rows.size() > 2, "test folder fills the reviewed list");

	const auto highlighted = [&tags]
	{
		return std::ranges::count_if(tags->_rows, [](const auto& row) { return row->_bg_color.a > 0.0f; });
	};

	tags->begin_processing(tags->_rows.size());
	tags->processing_item(0, 1);

	const auto first = tags->_rows.front();
	assert_equal(1, static_cast<int>(highlighted()), "one row is marked in progress");
	assert_equal(true, first->_bg_color.a > 0.0f, "the row the run named is the marked one");

	// Any reorder will do; a header click during review is the reachable one.
	std::ranges::reverse(tags->_rows);
	tags->processing_item(0, 2);

	assert_equal(1, static_cast<int>(highlighted()), "the previous row is cleared even though it moved");
	assert_equal(true, tags->_rows.front()->_bg_color.a > 0.0f, "the newly named row is the marked one");

	tags->end_processing();
	assert_equal(0, static_cast<int>(highlighted()), "no row stays marked once the run ends");
}

// The SID monitor's parts, as its clicks find them: panels of the stack, as wide as the column and parted by its
// gap, the voices side by side when there is room and stacked when not, the output over the filter beside the
// register grid in a column wide enough for two parts, the voices stacked beside the grid in a Short's upright frame,
// and a tune chip for each tune until there are too many to show. Drawn for a movie, it has no controls.
static void should_lay_out_the_sid_monitor()
{
	ui::test::fake_measure_context mc;
	const auto inside = [](const recti inner, const recti outer)
	{
		return inner.left >= outer.left && inner.right <= outer.right && inner.top >= outer.top &&
		       inner.bottom <= outer.bottom;
	};

	const auto wide = layout_sid_monitor(recti(0, 0, 1200, 0), 1.0, 3, mc);
	assert_equal(1200, wide.panel.width(), "the panels fill the column, as the stack's other panels do");
	assert_equal(0, wide.voices[0].left, "meeting its edges");
	assert_equal(1200, wide.voices[2].right, "on both sides");
	assert_equal(wide.voices[0].top, wide.voices[2].top, "three voices side by side");
	assert_equal(3, static_cast<int>(wide.tune_chips.size()), "a chip for each tune");
	assert_equal(true, wide.height > 0 && wide.panel.bottom == wide.height, "the height is the panels'");
	assert_equal(true, wide.panels != nullptr && wide.panels->size() >= 3,
	             "sections are real children of the shared flex tree");
	assert_equal(true, wide.tunes.top == 0 && inside(wide.tune_chips.front(), wide.tunes),
	             "the tunes lead, in a panel of their own");
	assert_equal(core::round(ui::panel_element::spacing * mc.scale_factor), wide.voices[0].top - wide.tunes.bottom,
	             "parted from the voices by the stack's own gap");

	for (int v = 0; v < 3; ++v)
	{
		assert_equal(true, inside(wide.voices[v], wide.panel), "a voice sits in the panel");
		assert_equal(true, inside(wide.mute[v], wide.voices[v]) && inside(wide.solo[v], wide.voices[v]),
		             "its buttons sit in it");
		assert_equal(true, wide.mute[v].right <= wide.solo[v].left, "mute and solo do not overlap");
	}

	// Wide enough for two parts side by side, as a landscape frame is, the output stands over the filter beside the
	// register grid, which takes the height of both.
	assert_equal(true,
	             wide.output.top >= wide.voices[0].bottom && wide.filter.top > wide.output.bottom &&
	                 wide.filter.left == wide.output.left && wide.registers.left > wide.output.right &&
	                 wide.registers.top == wide.output.top && wide.registers.bottom == wide.filter.bottom &&
	                 inside(wide.registers, wide.panel),
	             "the output over the filter, beside the registers that stand as tall as both");
	const auto wide_no_registers = layout_sid_monitor(recti(0, 0, 1200, 0), 1.0, 3, mc, 3);
	assert_equal(true,
	             wide_no_registers.filter.top == wide_no_registers.output.top &&
	                 wide_no_registers.filter.left > wide_no_registers.output.right,
	             "without the grid, the output and the filter share a row");
	assert_equal(true, wide_no_registers.height < wide.height, "which is shorter still");

	const auto column = layout_sid_monitor(recti(0, 0, 1000, 0), 1.0, 3, mc);
	assert_equal(true,
	             column.output.top >= column.voices[0].bottom && column.filter.top >= column.output.bottom &&
	                 column.registers.top >= column.filter.bottom && inside(column.registers, column.panel),
	             "a narrower column has them follow one under another, in order");
	assert_equal(true, wide.height < column.height, "so the wide arrangement is the shorter");
	const auto shrunk = layout_sid_monitor(recti(0, 0, 1120, 0), 0.9, 3, mc);
	assert_equal(true, shrunk.filter.top >= shrunk.output.bottom && shrunk.registers.top >= shrunk.filter.bottom,
	             "drawn smaller to fit, a monitor keeps the arrangement it has at full size");

	// A movie's frame has no use for anything to click: no tune chips and no mute or solo, and the gate and its note
	// keep their row.
	const auto frame = layout_sid_monitor(recti(0, 0, 1200, 0), 1.0, 3, mc, 4, 0, false);
	assert_equal(true, frame.tunes.is_empty() && frame.tune_chips.empty(), "a frame shows no tune selector");
	assert_equal(true,
	             std::ranges::all_of(frame.mute, &recti::is_empty) && std::ranges::all_of(frame.solo, &recti::is_empty),
	             "nor mute and solo");
	assert_equal(wide.voices[0].height(), frame.voices[0].height(), "the voices as tall as they are with them");
	assert_equal(true, frame.height < wide.height, "so the monitor is the tunes' panel shorter");

	// Upright, as a Short's frame is, the voices stack, all one width, beside the register grid, which stands as tall
	// as them with a column for each voice; the output and the filter cross the whole column below them.
	const auto upright = layout_sid_monitor(recti(0, 0, 600, 0), 1.0, 1, mc, 4, 0, false, true);
	assert_equal(true,
	             upright.voices[1].top > upright.voices[0].bottom && upright.voices[2].top > upright.voices[1].bottom,
	             "upright, the voices stack");
	assert_equal(true,
	             upright.voices[0].left == 0 && upright.voices[1].left == 0 && upright.voices[2].left == 0 &&
	                 upright.voices[0].width() == upright.voices[1].width() &&
	                 upright.voices[1].width() == upright.voices[2].width(),
	             "all one width, from the column's edge");
	assert_equal(true,
	             upright.registers_by_voice && upright.registers.left > upright.voices[0].right &&
	                 upright.registers.right == 600 && upright.registers.top == upright.voices[0].top &&
	                 upright.registers.bottom == upright.voices[2].bottom,
	             "the register grid down their right side, as tall as them");
	assert_equal(true,
	             upright.output.top > upright.voices[2].bottom && upright.output.left == 0 &&
	                 upright.output.right == 600 && upright.filter.top > upright.output.bottom &&
	                 upright.filter.left == 0 && upright.filter.right == 600,
	             "the output, then the filter, across the column below them");
	const auto no_grid = layout_sid_monitor(recti(0, 0, 600, 0), 1.0, 1, mc, 3, 0, false, true);
	assert_equal(true,
	             no_grid.registers.is_empty() && !no_grid.registers_by_voice && no_grid.voices[0].right == 600 &&
	                 no_grid.voices[1].top > no_grid.voices[0].bottom,
	             "without the grid, the stacked voices take the column's width");

	const auto narrow = layout_sid_monitor(recti(0, 0, 360, 0), 1.0, 1, mc);
	assert_equal(true, narrow.voices[1].top > narrow.voices[0].bottom, "voices stack in a narrow column");
	assert_equal(true, narrow.tunes.is_empty() && narrow.tune_chips.empty(), "one tune needs no selector");
	assert_equal(true, narrow.height > wide.height, "and the column grows to hold them");
	assert_equal(true, narrow.filter_wraps && !wide.filter_wraps, "the filter's badges wrap only when narrow");

	const auto many = layout_sid_monitor(recti(0, 0, 600, 0), 1.0, 200, mc);
	assert_equal(true, many.tune_chips.empty(), "too many tunes for chips");
	assert_equal(true, !many.previous_tune.is_empty() && !many.next_tune.is_empty(), "are stepped through");

	// Less room, fewer parts: the register grid goes first, the ADSR row last.
	const auto no_registers = layout_sid_monitor(recti(0, 0, 1000, 0), 1.0, 3, mc, 3);
	assert_equal(true, no_registers.registers.is_empty() && !no_registers.filter.is_empty(), "registers go first");
	const auto no_filter = layout_sid_monitor(recti(0, 0, 1000, 0), 1.0, 3, mc, 2);
	assert_equal(true, no_filter.filter.is_empty() && !no_filter.output.is_empty(), "then the filter");
	const auto voices_only = layout_sid_monitor(recti(0, 0, 1000, 0), 1.0, 3, mc, 1);
	assert_equal(true, voices_only.output.is_empty() && !voices_only.compact, "then the output");
	const auto compact = layout_sid_monitor(recti(0, 0, 1000, 0), 1.0, 3, mc, 0);
	assert_equal(true, compact.compact, "compact keeps only the voices");
	assert_equal(true, compact.voices[0].height() < voices_only.voices[0].height(), "without their ADSR row");
	assert_equal(3, static_cast<int>(compact.tune_chips.size()), "but keeps the tunes");
	assert_equal(true,
	             compact.height < voices_only.height && voices_only.height < no_filter.height &&
	                 no_filter.height < no_registers.height && no_registers.height < column.height,
	             "each step is shorter than the one before");

	// Long button labels in a narrow column take a row of their own, shared, and stay in the voice.
	const auto wrapped = layout_sid_monitor(recti(0, 0, 200, 0), 1.0, 1, mc, 0, 90);
	assert_equal(true, wrapped.buttons_wrap && !wide.buttons_wrap, "mute and solo wrap only when narrow");

	for (int v = 0; v < 3; ++v)
	{
		assert_equal(true, inside(wrapped.mute[v], wrapped.voices[v]) && inside(wrapped.solo[v], wrapped.voices[v]),
		             "wrapped buttons sit in the voice");
		assert_equal(true, wrapped.mute[v].right <= wrapped.solo[v].left && wrapped.mute[v].top == wrapped.solo[v].top,
		             "side by side, without overlapping");
	}
}

// The monitor is drawn as large as its space allows, down to three quarters of its size, before it gives up a
// part, and starts at the top of that space, reporting what it took so the stack closes up behind it. Text keeps
// its size as the rest shrinks, so a single step in proportion to the shortfall still overflowed: a space a
// little short of the whole monitor lost its register grid, and one a little short of that its filter, each
// leaving the gap of what went between the monitor and the controls below it.
static void should_fit_the_sid_monitor_to_its_space()
{
	ui::test::fake_measure_context mc;
	constexpr auto top = 100;
	const auto space = [](const int height)
	{
		return recti(0, top, 700, top + height);
	};
	const auto height_of = [&](const double scale, const int detail)
	{
		return layout_sid_monitor(space(0), scale, 1, mc, detail).height;
	};
	const auto detail_of = [](const sid_monitor_layout& l)
	{
		if (l.compact) return 0;
		if (!l.registers.is_empty()) return 4;
		if (!l.filter.is_empty()) return 3;
		return l.output.is_empty() ? 1 : 2;
	};

	auto scale = 0.0;
	const auto roomy = space(height_of(1.0, 4) + 100);
	const auto whole = fit_sid_monitor(roomy, 1.0, 1, mc, 0, scale);
	assert_equal(1.0, scale, "with room to spare it is drawn at full size");
	assert_equal(4, detail_of(whole), "whole");
	assert_equal(top, whole.panel.top, "at the top of the space it is given");
	assert_equal(roomy.bottom - 100, whole.panel.bottom, "taking only what it needs of it");

	auto previous_detail = 4;

	for (auto height = height_of(1.0, 4); height >= 40; height -= 3)
	{
		const auto r = space(height);
		const auto l = fit_sid_monitor(r, 1.0, 1, mc, 0, scale);
		const auto detail = detail_of(l);
		const auto at = std::format("in {} pixels", height);

		assert_equal(true, detail <= previous_detail, at, "a part never comes back as the space shrinks");
		previous_detail = detail;

		if (detail < 4)
		{
			assert_equal(true, height_of(0.75, detail + 1) > height, at,
			             "a part goes only when three quarters of the monitor with it does not fit");
		}

		assert_equal(top, l.panel.top, at, "it starts at the top");

		if (l.height <= height)
		{
			assert_equal(true, scale >= 0.75 && scale <= 1.0, at, "at no less than three quarters");
			if (scale < 1.0)
			{
				assert_equal(true, height_of(std::min(1.0, scale + 0.001), detail) > height, at,
				             "drawn smaller only as far as it must be");
			}
		}
		else
		{
			assert_equal(true, detail == 0 && scale == 0.75, at, "only the smallest compact monitor overflows");
		}
	}

	assert_equal(0, previous_detail, "a space too short for any of it leaves the compact monitor");
}

static void should_fade_sid_register_activity()
{
	sid_frame_state frames[4];
	frames[1].regs[0] = 1; // changed two frames before the one shown, and unchanged since
	frames[2].regs[0] = 1;
	frames[3].regs[0] = 1;
	frames[3].regs[4] = 0x21; // changed in the frame shown

	const auto activity = sid_register_activity(frames, std::size(frames));
	assert_equal(true, activity[4] > 0.99f, "a register written with a new value glows");
	assert_equal(true, activity[0] > 0.0f && activity[0] < activity[4], "and fades in the frames after");
	assert_equal(true, activity[1] == 0.0f, "a register that never changed stays dark");
	assert_equal(true, sid_register_activity(frames, 1)[4] == 0.0f, "one frame alone shows no change");
}

// Movie shows an audio clip as Diffractor plays it - a SID tune's or a tracker module's monitor, or the
// visualizer's bars - on a landscape frame under the file's title: frames that follow the music, at any size.
static void should_draw_an_audio_clip_for_a_movie()
{
	const auto write = [](const std::string_view extension, const std::vector<uint8_t>& bytes)
	{
		const auto path = test_temps.next_path(extension);
		assert_equal(true, platform::blob_save_to_file({bytes.data(), bytes.size()}, path), "the file is written");
		return path;
	};

	// How many of a frame's pixels are not its background, and how many differ from another frame's.
	const auto drawn = [](const core::const_surface_ptr& s)
	{
		const auto background = s->get_pixel(0, 0) & 0xFFFFFFu;
		auto count = 0;
		for (auto y = 0; y < s->dimensions().cy; ++y)
			for (auto x = 0; x < s->dimensions().cx; ++x) count += (s->get_pixel(x, y) & 0xFFFFFFu) != background;
		return count;
	};
	const auto changed = [](const core::const_surface_ptr& a, const core::const_surface_ptr& b)
	{
		auto count = 0;
		for (auto y = 0; y < a->dimensions().cy; ++y)
			for (auto x = 0; x < a->dimensions().cx; ++x)
				count += (a->get_pixel(x, y) ^ b->get_pixel(x, y)) & 0xFFFFFFu ? 1 : 0;
		return count;
	};

	const auto sid = write(".sid", make_test_sid_tune());
	const struct
	{
		core::file_path path;
		double early;
		double late;
	} clips[] = {
		{sid, 0.5, 1.5},
		{write(".mod", make_test_tracker_module(6)), 0.3, 2.0},
		{test_files_folder().combine_file("Games Without Frontiers.mp3"), 5.0, 6.0},
	};

	for (const auto& clip : clips)
	{
		const auto name = std::string(clip.path.extension());
		assert_equal(true, is_movie_audio(clip.path), std::format("{}: Movie draws it", name));
		assert_equal(true, movie_audio_seconds(clip.path) > clip.late,
		             std::format("{}: for as long as it plays", name));

		movie_audio_frames frames(clip.path);
		const auto first = frames.frame(clip.early, {960, 960});
		const auto second = frames.frame(clip.late, {960, 960});
		assert_equal(true, first && second, std::format("{}: its frames are drawn", name));
		if (!first || !second) continue;

		assert_equal(true, first->dimensions() == sizei{960, 540},
		             std::format("{}: landscape, as large as fits", name));
		assert_equal(true, drawn(first) > 960 * 540 / 20, std::format("{}: with something to see", name));
		assert_equal(true, changed(first, second) > 200, std::format("{}: and moving with the music", name));

		const auto again = frames.frame(clip.early, {960, 960});
		assert_equal(true, again && changed(first, again) < changed(first, second),
		             std::format("{}: going back shows what was there", name));

		const auto tile = frames.frame(clip.early, {96, 96});
		assert_equal(true, tile && tile->dimensions() == sizei{96, 54},
		             std::format("{}: a tile is the frame made small", name));

		// The preview and the render draw on workers, with nothing of the UI thread's.
		core::const_surface_ptr on_worker;
		std::thread worker([&on_worker, &clip]
		{ on_worker = movie_audio_frames(clip.path).frame(clip.late, {640, 640}); });
		worker.join();
		assert_equal(true, on_worker && drawn(on_worker) > 0, std::format("{}: and is drawn on a worker", name));
	}

	// The tune's sound opens spanning the length Movie gives it, so the trim's level track lines up with its picture.
	av_format_decoder decoder;
	assert_equal(true, open_movie_audio(decoder, sid, media_intent::playback), "a tune's sound opens");
	decoder.init_streams(-1, -1, false, false, false);
	assert_equal(true, std::abs(decoder.end_time() - decoder.start_time() - movie_audio_seconds(sid)) < 0.1,
	             "spanning the clip");

	// Each of a SID file's tunes is a source of its own, and the frame names the one it is.
	const auto tunes = movie_audio_songs(sid);
	assert_equal(true, tunes.count == 3 && tunes.start == 2, "the file's tunes, and the one it starts on");
	assert_equal(true, movie_audio_seconds(sid, 3) > 0, "another of them plays too");
	const auto tune_frame = movie_audio_frames(sid, 3).frame(0.5, {960, 960});
	const auto start_frame = movie_audio_frames(sid, 2).frame(0.5, {960, 960});
	assert_equal(true, tune_frame && start_frame && changed(tune_frame, start_frame) > 0,
	             "and is drawn as itself, named for its number");

	const std::string_view junk = "not a tune";
	assert_equal(0.0, movie_audio_seconds(write(".sid", {junk.begin(), junk.end()})),
	             "a file nothing plays has no length");
}

// A text screen's words sit in the middle of the frame and a caption in its lower band, white over a soft black
// shadow - a copy of the letters blurred wide behind them, cast in no direction - so either reads over any picture.
// The preview and the render both draw them this way, the render on a worker.
static void should_draw_movie_text_screens_and_captions()
{
	const sizei frame{1920, 1080};

	const auto title = draw_movie_text("Hiking to\nEverest", movie_text_kind::screen, frame);
	assert_equal(true, title.surface != nullptr, "a screen's words are drawn");
	if (!title.surface) return;

	const auto size = title.surface->dimensions();
	assert_equal(true, std::abs(title.at.x + size.cx / 2 - frame.cx / 2) <= 1, "centred across the frame");
	assert_equal(true, std::abs(title.at.y + size.cy / 2 - frame.cy / 2) <= 1, "and down it");

	auto dark = 0;
	auto deep = 0;
	auto white = 0;
	auto edge_clear = true;
	std::set<uint32_t> shades;

	for (auto y = 0; y < size.cy; ++y)
	{
		for (auto x = 0; x < size.cx; ++x)
		{
			const auto pixel = title.surface->get_pixel(x, y);
			const auto alpha = pixel >> 24;
			const auto brightest = std::max({(pixel >> 16) & 0xFFu, (pixel >> 8) & 0xFFu, pixel & 0xFFu});
			const auto is_dark = alpha > 0 && brightest < 0x40u;
			dark += is_dark;
			// Over white, a pixel this black and this opaque leaves the picture a dark grey.
			deep += alpha >= 0xC0u && (pixel & 0xFFFFFFu) == 0;
			white += alpha == 255 && (pixel & 0xFFFFFFu) == 0xFFFFFFu;
			if (is_dark) shades.insert(alpha);
			if ((x == 0 || y == 0 || x == size.cx - 1 || y == size.cy - 1) && alpha != 0) edge_clear = false;
		}
	}

	assert_equal(true, white > 0, "its letters are white");
	assert_equal(true, dark > 0, "over a black shadow");
	assert_equal(true, deep > 0, "dark enough beside the letters to set them off a bright picture");
	assert_equal(true, shades.size() >= 32,
	             std::format("soft, in many shades rather than an edge ({})", shades.size()));
	assert_equal(true, edge_clear, "fading to nothing before the edge of what is drawn");

	const auto caption = draw_movie_text("Namche Bazaar", movie_text_kind::caption, frame);
	assert_equal(true, caption.surface != nullptr, "a caption is drawn");
	if (!caption.surface) return;

	const auto caption_bottom = caption.at.y + static_cast<int>(caption.surface->height());
	assert_equal(true, caption_bottom > frame.cy * 9 / 10 && caption_bottom < frame.cy, "in the frame's lower band");

	const auto heading = draw_movie_text("Everest", movie_text_kind::screen, frame);
	const auto under = draw_movie_text("Everest", movie_text_kind::caption, frame);
	assert_equal(true, heading.surface && under.surface && under.surface->height() < heading.surface->height(),
	             "and smaller than a screen's words");

	const auto portrait = draw_movie_text("Everest", movie_text_kind::screen, {1080, 1920});
	assert_equal(true, portrait.surface && portrait.surface->height() == heading.surface->height(),
	             "sized by the frame's shorter side, so a portrait movie's text is as large");

	assert_equal(true, draw_movie_text("", movie_text_kind::caption, frame).surface == nullptr,
	             "no words draw nothing");

	// A screen of credits longer than the frame is held still, so all of it has to stay in the frame.
	const sizei small{640, 360};
	std::string credits;
	for (auto line = 0; line < 16; ++line) credits += line == 0 ? "DIRECTED BY"s : "\nAlfie, Jana and Zac Walker"s;
	const auto rolled = draw_movie_text(credits, movie_text_kind::screen, small);
	assert_equal(true, rolled.surface != nullptr, "long credits are drawn");
	if (!rolled.surface) return;

	const auto rolled_letters = rolled.letters.extent();
	assert_equal(true, rolled_letters.cy <= small.cy * 9 / 10 && rolled.at.y + rolled.letters.top > 0,
	             "smaller, so every line is in the frame");
	assert_equal(true, rolled_letters.cy > small.cy / 2, "and no smaller than fitting needs");
	assert_equal(true, rolled.at.y >= 0 && rolled.at.y + static_cast<int>(rolled.surface->height()) <= small.cy,
	             "its shadow reaching no further than the frame");

	const auto word = draw_movie_text(std::string(80, 'W'), movie_text_kind::caption, small);
	assert_equal(true,
	             word.surface && word.at.x >= 0 && word.at.x + static_cast<int>(word.surface->width()) <= small.cx,
	             "a word too long to wrap is set small enough to fit across");

	// Credits taller than any surface at the size they start at are set smaller until they are not, rather
	// than drawn as nothing.
	std::string roll;
	for (auto line = 0; line < 80; ++line) roll += "Walker\n";
	const auto long_roll = draw_movie_text(roll, movie_text_kind::screen, frame);
	assert_equal(true, long_roll.surface && static_cast<int>(long_roll.surface->height()) <= frame.cy,
	             "credits too tall to draw at their first size are set smaller, not left out");

	movie_text_layer on_worker;
	std::thread worker([&on_worker] { on_worker = draw_movie_text("Lukla", movie_text_kind::caption, {1280, 720}); });
	worker.join();
	assert_equal(true, on_worker.surface != nullptr, "and the render draws them on its worker");

	// A screen is a whole picture, composed as a photo is: opaque near black, with its words in the middle and their
	// shadow just showing round them.
	const auto screen = draw_movie_screen("The End", small);
	assert_equal(true, screen && screen->dimensions() == small, "a text screen fills the movie's frame");
	if (!screen) return;

	assert_equal(movie_screen_background, screen->get_pixel(0, 0), "its corners are opaque near black");
	assert_equal(true, (movie_screen_background & 0xFFu) <= 0x1Au, "a tenth of the way to white, no more");
	auto lit = 0;
	auto shaded = 0;
	for (auto x = 0; x < small.cx; ++x)
	{
		const auto pixel = screen->get_pixel(x, small.cy / 2);
		lit += (pixel & 0xFFFFFFu) == 0xFFFFFFu;
		shaded += (pixel & 0xFFu) < (movie_screen_background & 0xFFu);
	}
	assert_equal(true, lit > 0, "and its words cross the middle in white");
	assert_equal(true, shaded > 0, "darker than the screen beside them, where the shadow lies");
	assert_equal(0xff000000u, draw_movie_screen(" ", small)->get_pixel(0, 0), "a screen with no words is black");
}

// docs/movie.md#youtube-shorts: a Short keeps its words clear of what YouTube lays over it - the fifth along its foot
// and the buttons down its right edge - fills its frame with an audio clip's monitor, as a video's picture fills it,
// and puts a picture of another shape whole over a blurred, darkened copy of itself rather than between black bars.
static void should_draw_a_youtube_short()
{
	const auto is_short = movie_kind::youtube_short;
	const auto frame = movie_short_extent;
	const auto clear_below = static_cast<int>(frame.cy * (1.0 - movie_short_bottom_reserve));
	const auto clear_side = static_cast<int>(frame.cx * movie_short_side_reserve);

	const auto caption = draw_movie_text("Namche Bazaar, on the way up", movie_text_kind::caption, frame, is_short);
	assert_equal(true, caption.surface != nullptr, "a Short's caption is drawn");
	if (!caption.surface) return;

	const auto caption_bottom = caption.at.y + caption.letters.bottom;
	assert_equal(true, caption_bottom <= clear_below && caption_bottom > frame.cy / 2,
	             std::format("above the fifth YouTube covers ({} of {})", caption_bottom, frame.cy));
	assert_equal(true,
	             caption.at.x + caption.letters.left >= clear_side - 1 &&
	                 caption.at.x + caption.letters.right <= frame.cx - clear_side + 1,
	             "and inside the width its buttons leave");

	const auto movie_caption = draw_movie_text("Namche Bazaar, on the way up", movie_text_kind::caption, frame);
	assert_equal(true, movie_caption.surface && movie_caption.at.y > caption.at.y,
	             "where a movie's sits lower, in its own lower band");

	const auto words = draw_movie_text("Everest", movie_text_kind::screen, frame, is_short);
	assert_equal(true, words.surface != nullptr, "a Short's screen has its words");
	if (!words.surface) return;
	const auto middle = words.at.y + (words.letters.top + words.letters.bottom) / 2;
	assert_equal(true, std::abs(middle - clear_below / 2) <= 2, "in the middle of what YouTube leaves clear");

	// A picture of another shape sits over a backdrop of itself: small, dark, and in the frame's shape.
	assert_equal(true, movie_needs_backdrop({1920, 1080}, frame), "a landscape picture needs one");
	assert_equal(false, movie_needs_backdrop({720, 1280}, frame), "a picture of the Short's shape does not");

	const auto wide = std::make_shared<core::surface>();
	wide->alloc({400, 100}, core::texture_format::RGB);
	for (auto y = 0; y < 100; ++y)
		for (auto x = 0; x < 400; ++x)
			std::bit_cast<uint32_t*>(wide->pixels_line(y))[x] = x < 200 ? 0xffff4020u : 0xff20c0ffu;

	const auto backdrop = make_movie_backdrop(wide, frame);
	assert_equal(true, backdrop && backdrop->dimensions() == sizei{frame.cx / 16, frame.cy / 16},
	             "a backdrop is made small, in the frame's shape");
	if (!backdrop) return;

	auto brightest = 0u;
	auto lit = 0;
	for (auto y = 0; y < backdrop->dimensions().cy; ++y)
	{
		for (auto x = 0; x < backdrop->dimensions().cx; ++x)
		{
			const auto pixel = backdrop->get_pixel(x, y);
			const auto most = std::max({(pixel >> 16) & 0xFFu, (pixel >> 8) & 0xFFu, pixel & 0xFFu});
			brightest = std::max(brightest, most);
			lit += most > 0x20u;
		}
	}
	assert_equal(true, brightest > 0 && brightest <= 0x80u, std::format("dark ({})", brightest));
	assert_equal(true, lit > backdrop->dimensions().cx * backdrop->dimensions().cy / 2, "but still the picture's own");

	// An audio clip is drawn upright in a Short, its title and monitor as large as the frame holds.
	const auto sid = test_temps.next_path(".sid");
	const auto tune = make_test_sid_tune();
	assert_equal(true, platform::blob_save_to_file({tune.data(), tune.size()}, sid), "the tune is written");
	const auto upright = movie_audio_frames(sid).frame(0.5, {960, 960}, {}, frame);
	assert_equal(true, upright && upright->dimensions() == sizei{540, 960}, "an audio clip is drawn upright");
	if (!upright) return;

	const auto size = upright->dimensions();
	const auto background = upright->get_pixel(0, size.cy - 1);
	recti drawn{size.cx, size.cy, 0, 0};

	for (auto y = 0; y < size.cy; ++y)
	{
		for (auto x = 0; x < size.cx; ++x)
		{
			if (upright->get_pixel(x, y) == background) continue;
			drawn = recti(std::min(drawn.left, x), std::min(drawn.top, y), std::max(drawn.right, x + 1),
			              std::max(drawn.bottom, y + 1));
		}
	}

	assert_equal(true, drawn.left <= size.cx / 20 && drawn.right >= size.cx - size.cx / 20,
	             std::format("its monitor across the frame ({} to {} of {})", drawn.left, drawn.right, size.cx));
	assert_equal(true, drawn.top <= size.cy / 10 && drawn.bottom >= size.cy * 17 / 20,
	             std::format("and down it, title to foot ({} to {} of {})", drawn.top, drawn.bottom, size.cy));
}

// Keeps what it was last given, as a renderer's vertices do.
class recording_vertices final : public render::vertices
{
public:
	int updates = 0;
	std::vector<render::vertex_rect> rects;

	void update_bars(std::span<const render::vertex_rect>) override {}

	void update_rects(const std::span<const render::vertex_rect> r) override
	{
		++updates;
		rects.assign(r.begin(), r.end());
	}

	void update_polygons(std::span<const render::vertex_polygon>) override {}
};

// The monitor's moving parts are what the vertex buffer draws: each in its place, following the tune from
// present to present without the monitor drawing again, and asking for the scene to be rebuilt only when the
// text beside them has fallen behind or the tune has changed - and for nothing once the monitor is off screen.
static void should_move_the_sid_monitor_with_the_tune()
{
	const auto path = test_temps.next_path(".sid");
	const auto tune = make_test_sid_tune();
	assert_equal(true, platform::blob_save_to_file({tune.data(), tune.size()}, path), "the tune is written");

	const auto render = std::make_shared<sid_tune_render>(path, 0, 1.0);
	assert_equal(true, render->load(), "the tune loads");
	std::vector<int16_t> pcm(sid_sample_rate / 2);
	assert_equal(pcm.size(), render->read(0, pcm.data(), pcm.size()), "and plays");

	sid_monitor_motion::parts parts;

	for (int v = 0; v < 3; ++v)
	{
		const auto left = v * 220;
		parts.voices[v] = {recti(left, 0, left + 200, 36), recti(left, 50, left + 200, 62),
		                   recti(left, 70, left + 200, 82), recti(left, 90, left + 40, 106)};
	}

	parts.scope = recti(0, 120, 640, 210);
	for (auto i = 0; i < 15; ++i) parts.volume[i] = recti(i * 20, 220, i * 20 + 16, 240);
	for (auto i = 0; i < sid_register_count; ++i) parts.registers[i] = recti(i * 30, 250, i * 30 + 26, 280);

	const auto verts = std::make_shared<recording_vertices>();
	const auto drawn_in = [&verts](const recti area)
	{
		return std::ranges::count_if(verts->rects, [area](const render::vertex_rect& r)
		{ return area.contains(r.bounds.top_left()) && r.bounds.right <= area.right + 1; });
	};
	const auto lit = [&verts](const recti badge)
	{
		return std::ranges::any_of(verts->rects,
		                           [badge](const render::vertex_rect& r) { return r.bounds == badge.inflate(-1); });
	};

	sid_monitor_motion motion;
	motion._verts = verts;

	constexpr double position = 0.25;
	sid_frame_state state;
	assert_equal(true, render->frame_at(position, state), "the frame shown has been played");
	motion.place(parts, render, {}, 1.0f, position, state, 10.0);
	assert_equal(1, verts->updates, "placing the parts draws them");

	// Voice 1 holds a sawtooth with its gate open, at full volume; the play routine changes its frequency
	// every frame.
	assert_equal(true, drawn_in(parts.voices[0].shape) > 100, "its waveform is traced where its shape goes");
	assert_equal(true, lit(parts.voices[0].gate), "its gate is lit");
	assert_equal(false, lit(parts.voices[1].gate), "and the others' are not");
	assert_equal(true,
	             std::ranges::any_of(verts->rects,
	                                 [&parts, &state](const render::vertex_rect& r)
	{
		const auto track = parts.voices[0].frequency.inflate(-1);
		const auto frequency = state.regs[0] | (state.regs[1] << 8);
		return r.bounds.left == track.left && r.bounds.top == track.top &&
		       r.bounds.width() == core::round(track.width() * (frequency / 65535.0));
	}),
	             "its frequency meter is filled as far as its frequency goes");
	assert_equal(true, drawn_in(parts.scope) > 100, "the output is traced");
	assert_equal(true, std::ranges::all_of(parts.volume, lit), "all fifteen volume steps are lit");
	assert_equal(true, lit(parts.registers[0]), "the register the tune writes every frame glows");
	assert_equal(false, lit(parts.registers[1]), "one it set once and left does not");

	// Between the monitor's draws, a present follows the tune through the vertex buffer.
	assert_equal(true, motion.update(render, position, 10.01) == render_valid::valid,
	             "nothing to do at the same position");
	assert_equal(1, verts->updates, "so nothing is drawn again");
	assert_equal(true, motion.update(render, position + 0.02, 10.02) == render_valid::present,
	             "a later position moves the parts");
	assert_equal(2, verts->updates, "by drawing them again");

	// The values beside them are text, so the scene is rebuilt for them - once they have been up a while.
	assert_equal(true, motion.update(render, position + 0.2, 10.15) == render_valid::invalid,
	             "values out of date for a tenth of a second rebuild the scene");
	assert_equal(true,
	             motion.update(std::make_shared<sid_tune_render>(path, 2, 1.0), position, 10.16) ==
	                 render_valid::invalid,
	             "so does another tune, whose parts only the monitor can place");
	assert_equal(true, motion.update(render, position + 0.3, 13.0) == render_valid::valid,
	             "a monitor not drawn for seconds is off screen, and asks for nothing");

	// A silenced voice fades, as the monitor draws it.
	sid_voice_mix solo;
	solo.solo = 1;
	motion.place(parts, render, solo, 1.0f, position, state, 20.0);
	assert_equal(true,
	             std::ranges::all_of(verts->rects, [&parts](const render::vertex_rect& r)
	{ return !parts.voices[0].shape.contains(r.bounds.top_left()) || r.fill.a < 0.5f; }),
	             "a voice the solo silences is drawn faded");
}

// Mute and Solo act on release, and the panel can be rebuilt while the button is held - the tune ends, the
// folder refreshes - destroying the monitor the press was made on. The monitor here lives in storage that is
// cleared as it goes, so an action still holding the monitor finds a null display and faults at the release,
// rather than reading what a freed block usually still holds and passing. Capturing `this` again fails so.
static void should_keep_music_scrubbers_usable_in_narrow_panels()
{
	null_async_strategy async;
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(async, common);
	scrubber_element scrubber(make_test_player(), display);
	ui::test::fake_measure_context mc;
	const auto compact = scrubber.measure(mc, 160);
	scrubber.layout(mc, recti(0, 0, 160, compact.cy));
	const auto track = scrubber.calc_track_bounds({});
	assert_equal(160, track.width(), "a narrow monitor retains its full seeking range");
	assert_equal(compact.cy / 2, track.height(), "time labels have their own row below the track");
	assert_equal(true, compact.cy >= mc.text_line_height(render::font_face::dialog) * 2,
	             "the stacked transport measures both rows");
	const auto wide = scrubber.measure(mc, 600);
	scrubber.layout(mc, recti(0, 0, 600, wide.cy));
	const auto wide_track = scrubber.calc_track_bounds({});
	assert_equal(true, wide_track.left > 0 && wide_track.right < 600,
	             "wide transports keep the familiar inline time labels");
}

// A tracker channel shows less of each cell before any channel is scrolled away: the volume column goes first,
// then the effect, then the instrument, leaving the note - and only when even notes do not fit are the
// channels stepped through.
static void should_fit_every_tracker_channel_before_any_scrolls_away()
{
	constexpr auto cell = "C-5 01 v64 C20"sv;
	assert_equal("C-5 01 v64 C20"sv, tracker_cell_text(cell, tracker_cell_detail::full), "the whole cell");
	assert_equal("C-5 01 C20"sv, tracker_cell_text(cell, tracker_cell_detail::effect), "without its volume");
	assert_equal("C-5 01"sv, tracker_cell_text(cell, tracker_cell_detail::instrument), "its note and instrument");
	assert_equal("C-5"sv, tracker_cell_text(cell, tracker_cell_detail::note), "its note alone");
	assert_equal("... .. ..."sv, tracker_cell_text("... .. .. ...", tracker_cell_detail::effect),
	             "an empty cell is cut alike");

	auto fit = fit_tracker_grid(cell, 4, 500, 8);
	assert_equal(true, fit.detail == tracker_cell_detail::full && fit.visible_channels == 4,
	             "four channels fit whole in 500 pixels");
	assert_equal(14, fit.cell_chars, "at the cell's own width");

	fit = fit_tracker_grid(cell, 8, 500, 8);
	assert_equal(true, fit.detail == tracker_cell_detail::instrument && fit.visible_channels == 8,
	             "eight fit with their notes and instruments");

	fit = fit_tracker_grid(cell, 32, 500, 8);
	assert_equal(true, fit.detail == tracker_cell_detail::note && fit.visible_channels == 15,
	             "thirty-two show their notes, as many at once as fit");
}

static void should_keep_tracker_channel_headers_in_view()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	common_display_state_t common;
	state._display = std::make_shared<display_state_t>(as, common);
	const auto path = test_temps.next_path(".mod");
	const auto bytes = make_test_tracker_module();
	assert_equal(true, platform::blob_save_to_file({bytes.data(), bytes.size()}, path),
	             "the tracker UI fixture is written");
	const auto render = std::make_shared<tracker_tune_render>(path);
	std::vector<float> pcm(tracker_sample_rate * 2);
	assert_equal(static_cast<size_t>(tracker_sample_rate), render->read(0, pcm.data(), tracker_sample_rate),
	             "native playback publishes real pattern data for the UI");
	state._display->_tracker = render;
	const auto host = std::make_shared<detached_test_host>();
	tracker_monitor_control monitor(state, state._display, host);
	ui::test::fake_draw_context dc;
	dc.colors.alpha = 1;
	dc.colors.foreground = 0xFFFFFF;

	// The first frame tells the monitor how wide the module's cells are; it is arranged by them from then on.
	auto natural = monitor.measure(dc, 1000);
	monitor.layout(dc, recti(20, 30, 1020, 30 + natural.cy));
	monitor.render(dc, {});
	natural = monitor.measure(dc, 1000);
	monitor.layout(dc, recti(20, 30, 1020, 30 + natural.cy));
	dc.texts.clear();
	monitor.render(dc, {});

	const auto details = render->details();
	assert_equal(true, details && details->channels.size() == 4, "the module has four channels");
	assert_equal(true, details->empty_channels == std::vector<bool>{false, false, true, true},
	             "the last two with nothing in any pattern");
	assert_equal(2, monitor.grid_fit().visible_channels, "which the grid leaves out");
	assert_equal(true, monitor.grid_fit().detail == tracker_cell_detail::full, "whole, in a wide panel");
	assert_equal(true, monitor.header_bounds().bottom <= monitor.grid_bounds().top, "the header is above the rows");
	const auto grid = monitor.grid_bounds();
	assert_equal(true, grid.width() < monitor.bounds.width() / 2, "two channels need less than the panel");
	assert_equal(true, std::abs((grid.left - monitor.bounds.left) - (monitor.bounds.right - grid.right)) <= 1,
	             "so the table stands in its middle");
	assert_equal(true, monitor.header_bounds().left == grid.left && monitor.header_bounds().right == grid.right,
	             "its header over its rows");

	const auto headers = monitor.channel_headers();
	const auto mutes = monitor.mute_toggles();
	const auto solos = monitor.solo_toggles();
	assert_equal(2_z, headers.size(), "a header for each channel shown");

	for (auto c = 0_z; c < headers.size(); ++c)
	{
		assert_equal(true, headers[c].contains(mutes[c].center()) && headers[c].contains(solos[c].center()),
		             "mute and solo are in the channel's header");
		if (c > 0) assert_equal(true, headers[c].left > headers[c - 1].right, "the channels do not overlap");
	}

	tracker_frame_state frame;
	assert_equal(true, render->frame_at(0, frame) && frame.pattern, "the current pattern exists");
	assert_equal(true,
	             std::ranges::any_of(dc.texts, [&](const auto& t)
	{ return t.text.find(frame.pattern->cells[0].text) != std::string::npos; }),
	             "the pattern's rows use the decoder's tracker notation");
	assert_equal(true, std::ranges::any_of(dc.texts, [](const auto& t) { return t.text == "2"; }),
	             "each channel is numbered in its header");

	// A movie's frame has nothing to press: the header is each channel's level alone, and there is no Follow.
	tracker_monitor_control frame_monitor(state._display);
	frame_monitor.layout(dc, recti(20, 30, 1020, 30 + frame_monitor.measure(dc, 1000).cy));
	assert_equal(true,
	             frame_monitor.mute_toggles().empty() && frame_monitor.solo_toggles().empty() &&
	                 frame_monitor.follow_toggle().is_empty(),
	             "a frame's monitor has no controls");
	assert_equal(true, frame_monitor.header_bounds().height() < monitor.header_bounds().height(),
	             "and its header is the shorter for it");

	// A narrow panel shows less of each cell rather than fewer channels.
	natural = monitor.measure(dc, 200);
	monitor.layout(dc, recti(0, 0, 200, natural.cy));
	assert_equal(2, monitor.grid_fit().visible_channels, "every channel stays in sight in a narrow panel");
	assert_equal(true, monitor.grid_fit().detail != tracker_cell_detail::full, "with less of each cell");

	// A short stack shows fewer rows rather than pushing the panel below it away.
	const auto rows_natural = monitor.rows_shown();
	monitor.layout(dc, recti(0, 0, 200, natural.cy / 2));
	assert_equal(true, monitor.rows_shown() < rows_natural && monitor.rows_shown() >= 5, "fewer rows, but some");

	// Scrolling the pattern stops it following playback; Follow playback resumes.
	monitor.layout(dc, recti(0, 0, 200, natural.cy));
	assert_equal(true, state._display->_tracker_follow, "the pattern follows playback to begin with");
	assert_equal(true, monitor.wheel(monitor.grid_bounds().center(), {}, -120), "the wheel scrolls the pattern");
	assert_equal(false, state._display->_tracker_follow, "which stops it following");
	assert_equal(false, monitor.wheel({-100, -100}, {}, 120), "a wheel elsewhere is another panel's");

	const auto press = [&](const recti r)
	{
		hit_test_context ctx{r.center(), monitor.bounds};
		const auto controller = monitor.controller_from_location(host, r.center(), {}, ctx);
		assert_equal(true, controller != nullptr, "something to press");
		controller->on_mouse_left_button_down(r.center(), {});
		controller->on_mouse_left_button_up(r.center(), {});
	};

	press(monitor.follow_toggle());
	assert_equal(true, state._display->_tracker_follow, "Follow playback follows again");
	press(monitor.mute_toggles()[1]);
	assert_equal(true, state._display->_tracker_mix.is_silent(1), "a channel's mute silences it");
	press(monitor.solo_toggles()[0]);
	assert_equal(0, state._display->_tracker_mix.solo, "and its solo leaves it alone");

	// The bank is a section of the stack of its own, in the common table.
	const auto bank = make_tracker_bank_panel(state._display);
	auto tables = 0;
	std::function<void(const view_element_ptr&)> inspect = [&](const view_element_ptr& e)
	{
		if (dynamic_cast<ui::table_element*>(e.get())) ++tables;
		e->visit_children(inspect);
	};
	assert_equal(true, bank != nullptr && dynamic_cast<ui::panel_element*>(bank.get()) != nullptr,
	             "the samples are a panel of the stack");
	inspect(bank);
	assert_equal(1, tables, "in the common table");
	listing_draw_context bank_dc({0, 0, 600, 600});
	bank_dc.colors.alpha = 1;
	bank->layout(bank_dc, recti(0, 0, 600, bank->measure(bank_dc, 600).cy));
	bank->render(bank_dc, {});
	assert_equal(true, std::ranges::any_of(bank_dc.texts, [](const auto& t) { return t.text == "Triangle"; }),
	             "the real sample bank is shown");

	// With how the patterns play it: how many notes, between which, on how many channels - in the tracker's own
	// notation, the one its rows are written in.
	const auto& triangle = details->sample_use[0];
	const auto range = tracker_note_range(triangle);
	assert_equal(tracker_note_name(triangle.lowest), frame.pattern->cells[0].text.substr(0, 3),
	             "a note is named as the decoder names it");
	assert_equal(std::format("{}\xE2\x80\x93{}", tracker_note_name(triangle.lowest),
	                         tracker_note_name(triangle.highest)),
	             range, "a range runs from the lowest note to the highest");
	const auto shown = [&bank_dc](const std::string_view text)
	{
		return std::ranges::any_of(bank_dc.texts, [text](const auto& t) { return t.text == text; });
	};
	assert_equal(true,
	             shown(tt.tracker_notes.sv()) && shown(tt.tracker_range.sv()) && shown(tt.prop_name_channels.sv()),
	             "under headings of their own");
	assert_equal(true, shown("2") && shown(range), "the sample's notes and their range are shown");
	assert_equal(std::string{}, tracker_note_range(details->sample_use[1]), "and nothing for a sample never played");

	// The headings sort the bank, and the stack, rebuilt for the same module, keeps the order the user chose.
	const auto table_of = [](const view_element_ptr& panel)
	{
		std::shared_ptr<ui::table_element> found;
		std::function<void(const view_element_ptr&)> find = [&](const view_element_ptr& e)
		{
			if (const auto t = std::dynamic_pointer_cast<ui::table_element>(e)) found = t;
			e->visit_children(find);
		};
		find(panel);
		return found;
	};
	const auto table = table_of(bank);
	assert_equal(5_z, table->headings().size(), "the bank's columns are headed");
	const auto notes_heading = table->heading_cell_bounds(2).center();
	const auto bank_host = std::make_shared<detached_test_host>();
	hit_test_context heading_ctx(notes_heading, {0, 0, 600, 600});
	const auto heading_press = table->controller_from_location(bank_host, notes_heading, {}, heading_ctx);
	assert_equal(true, heading_press != nullptr, "a heading answers a press");
	heading_press->on_mouse_left_button_down(notes_heading, {});
	heading_press->on_mouse_left_button_up(notes_heading, {});
	assert_equal(2, table->sort_column(), "and sorts the bank by its column");
	const auto rebuilt = table_of(make_tracker_bank_panel(state._display));
	assert_equal(2, rebuilt->sort_column(), "which the rebuilt bank keeps");
	assert_equal(table->sort_ascending(), rebuilt->sort_ascending(), "the same way round");
}

static void should_share_ordinary_media_transport_with_native_monitors()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	common_display_state_t common;
	for (const auto name : {"transport.mp4", "transport.mp3", "transport.sid", "transport.mod"})
	{
		state._display = std::make_shared<display_state_t>(as, common);
		state._display->_item1 = make_thumbnail_test_item(name, {});
		state._display->_is_one = true;
		const auto panel = state.create_selection_controls();
		auto play = 0;
		auto seek = 0;
		std::function<void(const view_element_ptr&)> inspect = [&](const view_element_ptr& e)
		{
			if (dynamic_cast<play_control*>(e.get())) ++play;
			if (dynamic_cast<scrubber_element*>(e.get())) ++seek;
			e->visit_children(inspect);
		};
		inspect(panel);
		assert_equal(1, play, "every playable format has one ordinary play control", name);
		assert_equal(1, seek, "every playable format has one ordinary scrubber", name);
		assert_equal(true, dynamic_cast<ui::panel_element*>(panel.get()) != nullptr,
		             "ordinary and native media use the same controls panel");
	}
}

static void should_keep_optional_stream_panels_outside_primary_media()
{
	const settings_t defaults;
	assert_equal(false, defaults.verbose_metadata,
	             "optional stream details are closed without an explicit saved preference");
	const auto verbose = setting.verbose_metadata;
	const core::scope_exit restore([verbose] { setting.verbose_metadata = verbose; });
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	common_display_state_t common;
	state._display = std::make_shared<display_state_t>(as, common);
	state._display->_item1 = make_thumbnail_test_item("streams.sid", {});
	state._display->_is_one = true;
	for (auto i = 0; i < 32; ++i)
	{
		auto& stream = state._display->_player_media_info.streams.emplace_back();
		stream.index = i;
		stream.type = av_stream_type::audio;
		stream.codec = "optional-stream-codec";
		stream.audio_channels = 2;
		stream.audio_sample_rate = 48000;
	}
	const auto host = std::make_shared<detached_test_host>();
	items_view items(state, host);
	listing_draw_context dc({0, 0, 2000, 20000});
	constexpr recti landscape{0, 0, 1100, 500};
	const auto arrange = [&]
	{
		const media_column_inputs input{&items._media_priority_elements, &items._media_detail_elements,
		                                &items._media_elements, landscape, items._media_element};
		layout_media_column(input, dc);
	};
	const auto render_detail = [&]
	{
		dc.texts.clear();
		for (const auto& e : items._media_detail_elements) e->render(dc, {});
		return std::ranges::any_of(dc.texts, [](const auto& text) { return text.text == "optional-stream-codec"; });
	};
	setting.verbose_metadata = false;
	items.update_media_elements();
	arrange();
	assert_equal(2_z, items._media_priority_elements.size(), "only the monitor and ordinary controls are primary");
	assert_equal(1_z, items._media_detail_elements.size(), "a streams-only file still offers the detail toggle");
	assert_equal(false, render_detail(), "streams are not built or drawn until explicitly opened");
	const auto media_height = items._media_element->bounds.height();
	assert_equal(core::round(ui::panel_element::spacing * dc.scale_factor),
	             items._media_priority_elements[1]->bounds.top - items._media_element->bounds.bottom,
	             "the panel follows the monitor at the stack's gap, however much of its slot the monitor took");
	assert_equal(true, media_height >= landscape.height() / 2, "landscape media retains its half-pane floor");

	setting.verbose_metadata = true;
	items.update_media_elements();
	arrange();
	assert_equal(2_z, items._media_priority_elements.size(), "opened streams never extend the primary block");
	assert_equal(2_z, items._media_detail_elements.size(), "streams and their toggle live below the primary block");
	assert_equal(media_height, items._media_element->bounds.height(), "opening many streams cannot shrink the media");
	assert_equal(true, render_detail(), "the optional panel renders every stream's actual codec");
	assert_equal(items._media_priority_elements.back()->bounds.bottom + ui::panel_element::spacing,
	             items._media_detail_elements.front()->bounds.top, "priority and detail share the standard panel gap");
}

// A photo or a film has fullscreen to itself, with the panel over it; anything else is the stack the items view
// shows - a monitor, the visualizer, a collage - with the compact panel under it, centred, no wider than the
// stack grows and parted by the column's one gap.
static void should_present_pictures_alone_and_the_rest_as_a_stack()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	common_display_state_t common;
	const auto host = std::make_shared<detached_test_host>();

	for (const auto& [name, picture] : std::initializer_list<std::pair<std::string_view, bool>>{{"photo.jpg", true},
	                                                                                            {"film.mp4", true},
	                                                                                            {"tune.sid", false},
	                                                                                            {"module.mod", false},
	                                                                                            {"song.mp3", false}})
	{
		const auto display = std::make_shared<display_state_t>(as, common);
		display->_item1 = make_thumbnail_test_item(std::string(name), {});
		display->_is_one = true;
		state._display = display;

		const auto presentation = make_media_presentation(state, display, host, true);
		assert_equal(picture, presentation.picture, name, "a picture has the screen, and the rest stack");
		assert_equal(true, presentation.media != nullptr && presentation.above.front() == presentation.media, name,
		             "the media stands above the panel");
	}

	const auto several = std::make_shared<display_state_t>(as, common);
	several->_is_multi = true;
	several->_selection_item_count = 3;
	const auto collage = make_media_presentation(state, several, host, true);
	assert_equal(true, !collage.picture && std::dynamic_pointer_cast<images_control2>(collage.media) != nullptr,
	             "several items stack their collage over the summary");

	// Two files stand side by side only when they compare; any other pair is its summary alone, as Items shows it.
	for (const auto& [second, compares] :
	     std::initializer_list<std::pair<std::string_view, bool>>{{"other.png", true}, {"notes.txt", false}})
	{
		const auto pair = std::make_shared<display_state_t>(as, common);
		pair->_item1 = make_thumbnail_test_item("photo.jpg", {});
		pair->_item2 = make_thumbnail_test_item(std::string(second), {});
		pair->_is_two = true;
		pair->_comparison_eligible = can_compare_file_types(pair->_item1->file_type(), pair->_item2->file_type());

		for (const auto fullscreen : {false, true})
		{
			const auto presentation = make_media_presentation(state, pair, host, fullscreen);
			assert_equal(compares, presentation.picture, second, "a pair that compares is the picture");
			assert_equal(compares, std::dynamic_pointer_cast<side_by_side_control>(presentation.media) != nullptr,
			             second, "standing side by side, while any other pair has nothing above its summary");
			assert_equal(compares, !presentation.above.empty(), second, "and nothing stands in for the picture");
		}
	}

	const auto display = std::make_shared<display_state_t>(as, common);
	display->_item1 = make_thumbnail_test_item("tune.sid", {});
	display->_is_one = true;
	state._display = display;
	state.is_full_screen = true;

	constexpr sizei screen{2400, 1400};
	media_view media(state, host);
	ui::test::fake_draw_context dc({0, 0, screen.cx, screen.cy});
	media.activate(screen);
	media.layout(dc, screen);

	const auto& stack = media.stack();
	assert_equal(true, stack.size() >= 2 && std::dynamic_pointer_cast<sid_monitor_control>(stack.front()) != nullptr,
	             "the monitor leads the stack");
	assert_equal(media_stack_max_width, stack.front()->bounds.width(), "no wider than the stack grows");

	for (auto i = 1_z; i < stack.size(); ++i)
	{
		assert_equal(stack.front()->bounds.width(), stack[i]->bounds.width(), "each panel as wide as the others");
		assert_equal(stack[i - 1]->bounds.bottom + ui::panel_element::spacing, stack[i]->bounds.top,
		             "parted by the column's gap");
	}

	const auto above = stack.front()->bounds.top;
	const auto below = screen.cy - stack.back()->bounds.bottom;
	assert_equal(true, std::abs(above - below) <= 1, "centred down the screen");
	assert_equal(true, std::abs(stack.front()->bounds.left - (screen.cx - stack.front()->bounds.right)) <= 1,
	             "and across it");
}

// The folder an item is in is a link wherever the view is not showing that folder, and left out where it is: the
// address box already says so.
static void should_link_the_folder_of_an_item_found_elsewhere()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const view_host_base_ptr view;
	const auto shown = test_files_folder();
	assert_equal(true, state.open(view, df::search_t().add_selector(shown), {}), "the view shows a folder");
	common_display_state_t common;

	const auto draws_folder = [&](const core::folder_path folder, const std::string_view name)
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(indexed.name);
		const auto item = std::make_shared<df::item_element>(folder.combine_file(name), indexed);
		state._display = std::make_shared<display_state_t>(as, common);
		state._display->_item1 = item;
		state._display->_is_one = true;

		const auto panel = state.create_selection_controls();
		listing_draw_context dc({0, 0, 1000, 1000});
		dc.colors.alpha = 1;
		panel->layout(dc, recti(0, 0, 800, panel->measure(dc, 800).cy));
		panel->render(dc, {});
		const auto text = std::string(folder.text());
		return std::ranges::any_of(dc.texts, [&text](const auto& t) { return t.text == text; });
	};

	const auto elsewhere = shown.combine("elsewhere");
	assert_equal(true, draws_folder(elsewhere, "photo.jpg"), "an item away from the folder shown links its own");
	assert_equal(false, draws_folder(shown, "photo.jpg"), "one in the folder shown does not repeat it");
	assert_equal(true, draws_folder(elsewhere, "notes.bin"), "so does a file that is not media");
}

// Whom a picture names stands beside where and when it was taken - each name a way to the other pictures naming them
// - in the panel and in full screen alike.
static void should_name_the_people_in_a_picture()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	common_display_state_t common;

	const auto md = std::make_shared<prop::item_metadata>();
	md->people = core::cache(core::combine(std::vector<std::string>{"Jane Doe", "Ann Lee"}));
	df::index_file_item indexed;
	indexed.name = core::cache("photo.jpg"sv);
	indexed.ft = files::file_type_from_name(indexed.name);
	indexed.metadata.store(md);
	state._display = std::make_shared<display_state_t>(as, common);
	state._display->_item1 = std::make_shared<df::item_element>(test_files_folder().combine_file("photo.jpg"), indexed);
	state._display->_is_one = true;

	for (const auto compact : {false, true})
	{
		const auto panel = state.create_selection_controls(compact);
		listing_draw_context dc({0, 0, 1000, 1000});
		dc.colors.alpha = 1;
		panel->layout(dc, recti(0, 0, 800, panel->measure(dc, 800).cy));
		panel->render(dc, {});
		const auto draws = [&dc](const std::string_view name)
		{
			return std::ranges::any_of(dc.texts, [name](const auto& t) { return t.text == name; });
		};
		const auto form = compact ? "in full screen"s : "in the panel"s;
		assert_equal(true, draws("Jane Doe") && draws("Ann Lee"), std::format("both people are named {}", form));
	}
}

static void should_layout_nested_metadata_document_panels()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	items_view items(state, nullptr);
	metadata_block block;
	block.standard = metadata_standard::exif;
	auto& row = block.values.emplace_back();
	row.key = "Payload";
	row.id = "payload";
	row.open_by_default = true;
	metadata_binary_detail binary;
	binary.bytes = {'H', 'i', 0x00, 0xFF};
	row.detail = std::move(binary);
	std::vector<view_element_ptr> sections;
	items.add_metadata_elements(sections, block);
	assert_equal(1_z, sections.size(), "one metadata block creates one shared panel");
	const auto& section = sections.front();
	std::shared_ptr<document_control> document;
	std::function<void(const view_element_ptr&)> inspect = [&](const view_element_ptr& e)
	{
		if (const auto found = std::dynamic_pointer_cast<document_control>(e)) document = found;
		e->visit_children(inspect);
	};
	inspect(section);
	assert_equal(true, document != nullptr, "nested document controls belong to the common element tree");
	ui::test::fake_draw_context dc({0, 0, 1200, 1200});
	const auto height = section->measure(dc, 600).cy;
	section->layout(dc, {50, 100, 650, 100 + height});
	assert_equal(false, document->content_bounds().is_empty(),
	             "layout reaches the document body rather than assigning only the outer panel bounds");
	section->render(dc, {7, 11});
	assert_equal(true,
	             std::ranges::any_of(dc.texts, [](const auto& text)
	{ return text.text.find("48 69 00 FF") != std::string::npos; }),
	             "an expanded metadata panel actually draws its bytes");
	items._metadata_tree->expanded[std::format("{}/payload", static_cast<int>(block.standard))] = false;
	const auto closed_height = section->measure(dc, 600).cy;
	section->layout(dc, {50, 100, 650, 100 + closed_height});
	assert_equal(true, closed_height < height, "closing the row removes its document's layout height");
	assert_equal(false, document->is_visible(), "a closed document is hidden without leaving the owning tree");
}

static void should_toggle_a_sid_voice_after_its_monitor_is_rebuilt()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	common_display_state_t common;
	s._display = std::make_shared<display_state_t>(as, common);

	const auto host = std::make_shared<detached_test_host>();
	alignas(sid_monitor_control) std::byte storage[sizeof(sid_monitor_control)] = {};
	auto destroyed = false;
	std::shared_ptr<sid_monitor_control> monitor(std::construct_at(reinterpret_cast<sid_monitor_control*>(storage), s,
	                                                               s._display),
	                                             [&storage, &destroyed](sid_monitor_control* m)
	{
		std::destroy_at(m);
		std::ranges::fill(storage, std::byte{});
		destroyed = true;
	});
	monitor->bounds = recti(0, 0, 1200, 900);

	// The monitor finds its buttons where it last drew them, laid out from the same inputs as here.
	document_test_draw_context dc;
	monitor->render(dc, {});
	view_element_ptr first_tree;
	monitor->visit_children([&](const view_element_ptr& child) { first_tree = child; });
	dc.time_now += 0.1;
	monitor->render(dc, {});
	view_element_ptr next_tree;
	monitor->visit_children([&](const view_element_ptr& child) { next_tree = child; });
	assert_equal(true, first_tree == next_tree, "a music tick reuses its laid-out flex tree");
	const auto button_text = std::max(
		dc.measure_text(tt.command_volume0, render::font_face::code, render::text_style::single_line, 10000).cx,
		dc.measure_text(tt.sid_solo, render::font_face::code, render::text_style::single_line, 10000).cx);
	auto scale = dc.scale_factor;
	const auto l = fit_sid_monitor(monitor->bounds, dc.scale_factor, 1, dc, button_text, scale);

	const auto press = [&monitor, &host](const recti button)
	{
		hit_test_context ctx{button.center(), monitor->bounds};
		auto controller = monitor->controller_from_location(host, button.center(), {}, ctx);
		if (controller) controller->on_mouse_left_button_down(button.center(), {});
		return controller;
	};

	const auto mute = press(l.mute[1]);
	const auto solo = press(l.solo[2]);
	assert_equal(true, mute != nullptr && solo != nullptr, "the buttons are found where they were drawn");

	monitor.reset();
	assert_equal(true, destroyed, "the rebuild destroyed the monitor the presses were made on");

	mute->on_mouse_left_button_up(l.mute[1].center(), {});
	solo->on_mouse_left_button_up(l.solo[2].center(), {});
	const auto& mix = s._display->_sid_mix;
	assert_equal(true, mix.muted[1] && !mix.muted[0] && !mix.muted[2], "a release still mutes its voice alone");
	assert_equal(2, mix.solo, "and the other solos its voice");
	mute->on_mouse_left_button_up(l.mute[1].center(), {});
	assert_equal(true, mix.muted[1], "and a release with no press of its own does nothing");

	const auto previous = s._display;
	s._display = std::make_shared<display_state_t>(as, common);
	mute->on_mouse_left_button_down(l.mute[1].center(), {});
	mute->on_mouse_left_button_up(l.mute[1].center(), {});
	assert_equal(-1, s._display->_sid_mix.solo, "a held control cannot copy the previous item's mix onto a new item");
	assert_equal(2, previous->_sid_mix.solo, "the outgoing display's mix stays its own");
}

// One Commodore 64 file shown in the media pane, as the load leaves it: its bytes and the document made of
// them. Its worker tasks are held, so a machine started from it is queued and never runs.
struct c64_test_view
{
	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index{as, locations};
	view_state state{ss, as, index, make_test_player()};
	common_display_state_t common;
	display_state_ptr display;
	std::shared_ptr<detached_test_host> host = std::make_shared<detached_test_host>();
	items_view items{state, host};
	listing_draw_context dc{{0, 0, 2000, 20000}};
	static constexpr recti pane{0, 0, 1100, 700};

	c64_test_view(const std::string_view name, const std::vector<uint8_t>& bytes)
	{
		display = std::make_shared<display_state_t>(as, common);
		display->_item1 = make_thumbnail_test_item(std::string(name), {});
		display->_is_one = true;
		display->_selected_item_data = core::blob(bytes.begin(), bytes.end());
		// As the load decodes a Commodore file: never as text, whatever its bytes look like.
		display->_document = std::make_shared<document_state>(
			decode_document(display->_selected_item_data, name.substr(name.rfind('.')), false));
		state._display = display;
	}

	void arrange()
	{
		items.update_media_elements();
		const media_column_inputs input{&items._media_priority_elements, &items._media_detail_elements,
		                                &items._media_elements, pane, items._media_element};
		layout_media_column(input, dc);
	}

	// A press and release at a point of an element, as the pointer makes them.
	void click(const view_element_ptr& e, const pointi loc)
	{
		hit_test_context ctx{loc, pane};
		const auto controller = e->controller_from_location(host, loc, {}, ctx);
		assert_equal(true, controller != nullptr, "something to click");
		controller->on_mouse_left_button_down(loc, {});
		controller->on_mouse_left_button_up(loc, {});
	}
};

// A disk's listing stands where media does, above the item's panel, and a row naming a program runs it. The
// running machine takes the listing's place, held above the panel as a picture is, until Escape or Back.
static void should_run_a_commodore_disk_from_its_listing()
{
	c64_test_view v("games.d64", make_test_d64({
									 {"NOTES", 0x81, {'N', 'O'}},
									 {"HELLO", 0x82, make_test_c64_program("HI")},
								 }));

	v.arrange();
	const auto listing = std::dynamic_pointer_cast<commodore_disk_control>(v.items._media_elements.front());
	assert_equal(true, listing != nullptr, "the listing comes first, where media does");
	assert_equal(true,
	             v.items._media_elements.size() >= 2 &&
	                 listing->bounds.bottom <= v.items._media_elements[1]->bounds.top,
	             "above the item's panel");
	assert_equal(-1, listing->start_row(), "a disk is chosen from by its rows, with no Start");
	assert_equal(commodore_disk_control::no_part, listing->part_at(listing->part_bounds(0).center()),
	             "a sequential file does not run");
	assert_equal(1, listing->part_at(listing->part_bounds(1).center()), "a program does");

	// The column was scrolled to reach the row, which must not leave the machine's picture scrolled out of sight.
	v.items._media_scroller.layout({v.pane.width(), v.pane.height() * 3}, v.pane, {});
	v.items._media_scroller.scroll_offset(v.host, 0, v.pane.height());
	assert_equal(v.pane.height(), v.items._media_scroller.scroll_offset().y, "the listing scrolled to its row");

	v.click(listing, listing->part_bounds(1).center());
	assert_equal(true, v.display->_c64 != nullptr, "clicking the program's row starts the machine");
	assert_equal(1_z, v.as.pending_worker_count(async_queue::emulator), "on the emulator's queue");
	assert_equal(true, v.display->is_running_c64(), "which counts as running from then");

	v.arrange();
	const auto screen = std::dynamic_pointer_cast<c64_screen_control>(v.items._media_element);
	assert_equal(true, screen != nullptr && v.items._media_elements.front() == v.items._media_element,
	             "the machine takes the listing's place");
	assert_equal(0, v.items._media_scroller.scroll_offset().y, "at the top of the column");
	assert_equal(2_z, v.items._media_priority_elements.size(), "held above the fold with the item's panel");
	assert_equal(true, screen->bounds.height() >= v.pane.height() / 2, "keeping half the pane, as media does");
	assert_equal(true,
	             !screen->picture_bounds().is_empty() &&
	                 screen->picture_bounds().bottom < screen->part_bounds(c64_screen_control::part::back).top,
	             "its picture above Back");

	assert_equal(true, v.state.c64_key_down(platform::keys::UP, {}), "the arrows go to the machine");
	assert_equal(true, v.state.c64_key_down(platform::keys::ESCAPE, {}), "and Escape");
	assert_equal(true, v.display->_c64 == nullptr, "which leaves it");
	assert_equal(false, v.state.c64_key_down(platform::keys::UP, {}), "after which the keys are the view's again");

	v.click(listing, listing->part_bounds(1).center());
	v.arrange();
	const auto again = std::dynamic_pointer_cast<c64_screen_control>(v.items._media_element);
	assert_equal(true, again != nullptr, "the program runs again");
	v.click(again, again->part_bounds(c64_screen_control::part::back).center());
	assert_equal(true, v.display->_c64 == nullptr, "and Back leaves it as Escape does");
	v.arrange();
	assert_equal(true, std::dynamic_pointer_cast<commodore_disk_control>(v.items._media_elements.front()) != nullptr,
	             "showing the listing again");
}

// A cartridge holds one thing to run, so it is drawn as one - its title on its label - with Start under it, in a
// panel short enough to be held above the fold with the item's panel.
static void should_start_a_commodore_cartridge()
{
	std::vector<uint8_t> crt(64, 0);
	std::memcpy(crt.data(), "C64 CARTRIDGE   ", 16);
	crt[0x13] = 0x40; // the header's length
	crt[0x14] = 1;    // version 1.0
	std::memcpy(crt.data() + 0x20, "Test Cartridge", 14);

	c64_test_view v("game.crt", crt);
	v.arrange();
	const auto cartridge = std::dynamic_pointer_cast<c64_cartridge_control>(v.items._media_elements.front());
	assert_equal(true, cartridge != nullptr, "the cartridge stands where media does");
	assert_equal(true,
	             v.items._media_elements.size() >= 2 &&
	                 cartridge->bounds.bottom <= v.items._media_elements[1]->bounds.top,
	             "above the item's panel");
	assert_equal("TEST CARTRIDGE"sv, cartridge->title(), "its label carries its header's name, in capitals");
	assert_equal(2_z, v.items._media_priority_elements.size(), "held above the fold with the item's panel");

	const auto body = cartridge->part_bounds(c64_cartridge_control::part::cartridge);
	const auto start = cartridge->part_bounds(c64_cartridge_control::part::start);
	assert_equal(true, body.height() > body.width(), "it stands in the shell's proportions, longer than it is wide");
	assert_equal(true, cartridge->runs() && !start.is_empty() && start.top >= body.bottom, "Start is under it");
	assert_equal(true, cartridge->part_at(start.center()) == c64_cartridge_control::part::start, "where it is found");
	assert_equal(true, cartridge->part_at(body.center()) == c64_cartridge_control::part::cartridge,
	             "and the cartridge itself answers too");

	v.click(cartridge, start.center());
	assert_equal(true, v.display->_c64 != nullptr, "Start starts the machine");
	assert_equal(1_z, v.as.pending_worker_count(async_queue::emulator), "on the emulator's queue");

	libfiles::c64_listing blank;
	blank.rows.emplace_back().file_type = 0x99;
	assert_equal("PITFALL"sv, c64_cartridge_title(blank, "pitfall"), "a blank header gives way to the file's name");
}

// Full screen shows a running machine above the item's panel, as the items view does. A task view shows neither,
// so opening one stops the machine rather than leaving it playing unseen, with the keys.
static void should_keep_a_commodore_64_running_only_where_it_is_seen()
{
	c64_test_view v("games.d64", make_test_d64({{"HELLO", 0x82, make_test_c64_program("HI")}}));
	v.arrange();
	const auto listing = std::dynamic_pointer_cast<commodore_disk_control>(v.items._media_elements.front());
	assert_equal(true, listing != nullptr, "the disk is listed");
	v.click(listing, listing->part_bounds(0).center());
	assert_equal(true, v.display->_c64 != nullptr, "its program runs");

	v.state.is_full_screen = true;
	v.state.view_mode(view_type::media);
	media_view media(v.state, v.host);
	media.activate(v.pane.extent());
	assert_equal(true, std::dynamic_pointer_cast<c64_screen_control>(media.media_element()) != nullptr,
	             "full screen shows the machine");
	assert_equal(true, v.display->_c64 != nullptr, "which keeps running there");

	v.state.view_mode(view_type::edit);
	assert_equal(true, v.display->_c64 == nullptr, "a task view stops it");
}

// The machine's picture is drawn as large as fits and keeps its shape, at a whole multiple of its pixels when
// that gives up little, so they stay square.
static void should_fit_the_commodore_64_picture()
{
	auto whole = false;
	auto fit = fit_c64_picture({800, 600}, whole);
	assert_equal(true, whole && fit == sizei(768, 544), "twice its size, a whole multiple, fits 800 by 600");

	fit = fit_c64_picture({1000, 2000}, whole);
	assert_equal(true, !whole && fit.cx == 1000 && std::abs(fit.cy - 708) <= 1,
	             "filling the width when a whole multiple would give up too much");

	fit = fit_c64_picture({300, 300}, whole);
	assert_equal(true, !whole && fit.cx == 300 && fit.cy <= 300, "shrunk below its own size when it must be");

	fit = fit_c64_picture({0, 300}, whole);
	assert_equal(true, fit.cx == 0 && fit.cy == 0, "nothing with no room");
}

// The Explore timeline reads down from today a line a day, a header above each year: months with nothing in
// them fold into one row, years with nothing into another, and a week or a month is drawn exactly where its days
// are, so what the pointer is over is the part it opens.
static void should_lay_out_the_explore_timeline()
{
	using df::explore_day;

	df::explore_request request;
	request.today = explore_day(2025, 3, 20);

	const auto add = [&request](const uint32_t day)
	{
		df::explore_sample sample;
		sample.day = day;
		sample.kind = df::explore_kind::photo;
		sample.path = core::file_path(std::format("c:\\timeline\\{}-{}.jpg", day, request.samples.size()));
		request.samples.push_back(sample);
	};

	add(explore_day(2025, 3, 10));
	add(explore_day(2025, 3, 10));
	add(explore_day(2025, 1, 30));
	add(explore_day(2023, 6, 1));

	const auto model = df::build_explore_model(std::move(request), nullptr);

	explore_timeline_metrics metrics;
	metrics.day_height = 3;
	const auto layout = layout_explore_timeline(model, metrics);

	using kind = explore_timeline_layout::row_kind;
	const std::vector<kind> expected = {kind::year, kind::days, kind::gap,  kind::days, kind::empty_years,
	                                    kind::year, kind::gap,  kind::days, kind::gap};

	assert_equal(expected.size(), layout.rows.size(), "a row for each year, run of days and run of nothing",
	             "explore timeline");

	for (size_t i = 0; i < std::min(expected.size(), layout.rows.size()); ++i)
	{
		assert_equal(true, expected[i] == layout.rows[i].kind, std::format("row {} is what it should be", i),
		             "explore timeline");
		if (i > 0)
			assert_equal(layout.rows[i - 1].bottom, layout.rows[i].top, "each row starts where the last ended",
			             "explore timeline");
	}

	const auto& march = layout.rows[1];
	assert_equal(explore_day(2025, 3, 20), march.last_day, "the newest day, today, is at the top", "explore timeline");
	assert_equal(explore_day(2025, 3, 1), march.first_day, "down to the first of the month", "explore timeline");
	assert_equal(20 * 3, march.bottom - march.top, "a line a day", "explore timeline");

	const auto& february = layout.rows[2];
	assert_equal(explore_day(2025, 2, 1), february.first_day, "a month with nothing folds away", "explore timeline");
	assert_equal(2024, layout.rows[4].year, "a year with nothing folds away too", "explore timeline");

	for (const auto day : {explore_day(2025, 3, 20), explore_day(2025, 3, 10), explore_day(2025, 1, 30)})
	{
		const auto top = layout.top_of(day);
		assert_equal(true, top.has_value(), "a day drawn has a line", "explore timeline");
		assert_equal(day, layout.day_at(*top).value_or(0), "and the line is that day's", "explore timeline");
		assert_equal(day, layout.day_at(*top + 2).value_or(0), "all the way down it", "explore timeline");
		assert_equal(2025, layout.year_row_at(*top)->year, "under its year's header", "explore timeline");
	}

	assert_equal(false, layout.top_of(explore_day(2025, 2, 14)).has_value(), "a day folded away is not drawn",
	             "explore timeline");

	assert_equal(3u, static_cast<uint32_t>(layout.months.size()), "the three months with items are drawn",
	             "explore timeline");
	for (const auto& span : layout.months)
	{
		const auto& month = model.months[span.index];
		assert_equal(static_cast<int>(month.last_day - month.first_day + 1) * 3, span.bottom - span.top,
		             "a month is as tall as its days", "explore timeline");
	}

	// The week of 27 January to 2 February is drawn only where its January days are.
	const auto week = std::ranges::find(model.weeks, explore_day(2025, 1, 27), &df::explore_week::first_day);
	assert_equal(true, week != model.weeks.end(), "the week is in the model", "explore timeline");
	const auto drawn = std::ranges::find(layout.weeks, static_cast<uint32_t>(week - model.weeks.begin()),
	                                     &explore_timeline_layout::span::index);
	assert_equal(true, drawn != layout.weeks.end(), "and drawn", "explore timeline");
	assert_equal(5 * 3, drawn->bottom - drawn->top, "only as its five January days", "explore timeline");
	assert_equal(drawn->index, explore_timeline_layout::span_at(layout.weeks, drawn->top + 7)->index,
	             "and what is under the pointer there is that week", "explore timeline");

	const auto mid_february = std::ranges::find(model.weeks, explore_day(2025, 2, 10), &df::explore_week::first_day);
	assert_equal(true,
	             std::ranges::find(layout.weeks, static_cast<uint32_t>(mid_february - model.weeks.begin()),
	                               &explore_timeline_layout::span::index) == layout.weeks.end(),
	             "a week wholly folded away is not drawn", "explore timeline");

	assert_equal(layout.rows.back().bottom + metrics.bottom, layout.height, "the timeline ends after its last row",
	             "explore timeline");
}

// Explore on a host with no window, handed models built by hand rather than read from the index.
struct explore_test_view
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index{as, locations};
	view_state state{ss, as, index, make_test_player()};
	std::shared_ptr<detached_test_host> host = std::make_shared<detached_test_host>();
	std::shared_ptr<explore_view> view = std::make_shared<explore_view>(state, host);
	ui::test::fake_measure_context mc;
	sizei extent = {1000, 800};

	explore_test_view() { mc.scale_metrics(1.0); }

	static void add(df::explore_request& request, const uint32_t day, const std::string_view country)
	{
		df::explore_sample sample;
		sample.day = day;
		sample.kind = df::explore_kind::photo;
		sample.country = core::cache(country);
		sample.path = core::file_path(std::format("c:\\explore\\{}-{}.jpg", day, request.samples.size()));
		request.samples.push_back(sample);
	}

	df::explore_model_ptr arrive(df::explore_request request)
	{
		auto model = std::make_shared<const df::explore_model>(df::build_explore_model(std::move(request), nullptr));
		view->model_arrived_for_tests(model);
		return model;
	}

	void lay_out() { view->layout(mc, extent); }

	recti drawn_label(const std::string_view label, const render::font_face font) const
	{
		ui::test::fake_draw_context dc;
		view->render(dc, nullptr);
		const auto it = std::ranges::find_if(dc.texts,
		                                     [&](const auto& t) { return t.font == font && t.text == label; });
		return it == dc.texts.end() ? recti{} : it->clip;
	}

	explore_target at(const pointi loc)
	{
		view->hover(explore_view::pane::timeline, loc);
		return view->hovered();
	}
};

// A model Explore is handed is shown only once the timeline is laid out for it. Shown at once, one handed over
// while Explore was hidden was drawn, on showing, through the layout of the one before it.
static void should_show_an_explore_model_only_once_laid_out()
{
	using df::explore_day;

	explore_test_view f;
	df::explore_request request;
	request.today = explore_day(2025, 3, 20);
	explore_test_view::add(request, explore_day(2025, 3, 10), "Australia");

	const auto first = f.arrive(request);
	f.lay_out();
	assert_equal(true, f.view->model() == first, "laying out shows the model that arrived", "explore view");

	explore_test_view::add(request, explore_day(2023, 6, 1), "Australia");
	const auto second = f.arrive(request);
	assert_equal(true, f.view->model() == first, "one arriving later waits", "explore view");

	f.lay_out();
	assert_equal(true, f.view->model() == second, "until the timeline is laid out for it", "explore view");
}

static void should_put_back_first_in_the_history_views_window_toolbar()
{
	explore_test_view f;
	const auto faces = std::make_shared<faces_view>(f.state, nullptr);
	const std::vector<bar_entry> window_commands{commands::browse_back, commands::view_maximize, commands::view_restore,
	                                             commands::view_close};

	for (const auto& declared : {f.view->bars(), faces->bars()})
	{
		assert_equal(true, declared.top == window_commands, "Back starts the window toolbar and Close ends it");
	}
}

static void should_offer_person_review_in_the_faces_toolbar()
{
	explore_test_view f;
	const auto faces = std::make_shared<faces_view>(f.state, nullptr);
	const auto bar = faces->bars();
	assert_equal(true, bar.bottom_leading == std::vector<bar_entry>{bar_control::face_filter, bar_control::face_size},
	             "filter people and resize portraits without changing the review target");
	// The narrow status bar keeps its trailing decisions first.
	assert_equal(true,
	             bar.bottom.size() >= 3 && bar.bottom[bar.bottom.size() - 3] == bar_entry(commands::face_name) &&
	                 bar.bottom[bar.bottom.size() - 2] == bar_entry(commands::face_reject) &&
	                 bar.bottom.back() == bar_entry(commands::face_confirm),
	             "naming and the two review decisions have priority over search");
}

// Explore stands where it was left: a new model - the collection changed while it was closed - finds its timeline at
// the same day, though every day has moved down it, and its globe faces where the sidebar's was last turned, whole.
static void should_keep_explore_where_it_was_left()
{
	using df::explore_day;

	explore_test_view f;
	df::explore_request request;
	request.today = explore_day(2026, 1, 20);
	for (auto day = explore_day(2022, 1, 1); day <= explore_day(2025, 12, 31); day += 3)
		explore_test_view::add(request, day, "Australia");
	f.arrive(request);
	f.lay_out();

	const auto center = f.view->globe_view();
	f.view->step_globe_zoom(1);
	assert_equal(true, f.view->globe_zoom() > 1.0, "a zoom button magnifies the globe", "explore view");
	assert_equal(true, f.view->globe_view() == center, "button zoom preserves globe facing", "explore view");
	f.view->step_globe_zoom(-1);
	assert_equal(true, f.view->globe_zoom() < 1.00001, "zoom out returns to the whole globe", "explore view");

	platform::wheel_notch notch;
	notch.delta = -1200;
	f.view->mouse_wheel({f.extent.cx - 100, f.extent.cy / 2}, notch);
	const auto scrolled = f.view->timeline_scroll();
	assert_equal(true, scrolled > 0, "the timeline is scrolled back in time", "explore view");

	const auto day = f.view->timeline().day_near(scrolled + f.view->timeline().year_height);
	assert_equal(true, day.has_value(), "a day stands at the top", "explore view");
	if (!day) return;
	const auto top_before = *f.view->timeline().top_of(*day);

	for (auto added = explore_day(2026, 1, 1); added <= explore_day(2026, 1, 18); ++added)
		explore_test_view::add(request, added, "Australia");
	f.arrive(request);
	f.lay_out();

	const auto top_after = f.view->timeline().top_of(*day);
	assert_equal(true, top_after.has_value() && *top_after != top_before, "a newer month moves the day down",
	             "explore view");
	assert_equal(top_before - scrolled, *top_after - f.view->timeline_scroll(), "and the timeline moves with it",
	             "explore view");

	const gps_coordinate sydney(-33.9, 151.2);
	f.state.turn_globes(sydney);
	f.view->activate(f.extent);
	assert_equal(true, f.view->globe_view() == sydney, "the globe faces where the sidebar's was turned",
	             "explore view");
	assert_equal(1.0, f.view->globe_zoom(), "all of it in sight", "explore view");
}

// Show in Explore opens Explore at an item: its timeline at the week it was taken, marked in the middle of the view,
// and, where the item says where it was taken, its globe facing there, close.
static void should_show_an_item_in_explore()
{
	explore_test_view f;
	const auto path = test_files_folder().combine_file("Test.jpg");
	load_item(f.index, path, false);
	const auto file = f.index.find_item(path);
	const auto md = file.metadata.load();
	const auto taken = df::date_term_keys_of(df::date_parts_prop::original, file, md.get()).original;
	assert_equal(true, taken.is_valid(), "the photo says when it was taken", "explore view");
	if (!taken.is_valid()) return;

	const auto day = static_cast<uint32_t>(taken.to_days());
	df::explore_request request;
	request.today = day + 700;
	for (auto d = day - 900; d <= day + 690; d += 2) explore_test_view::add(request, d, "Australia");
	f.arrive(request);
	f.lay_out();
	assert_equal(true, f.view->keyboard_bounds().is_empty(), "no week is marked at first", "explore view");

	f.view->show_item(path);
	f.lay_out();

	const auto marked = f.view->keyboard_bounds();
	assert_equal(false, marked.is_empty(), "the week it was taken is marked", "explore view");
	assert_equal(true, f.view->timeline_scroll() > 0, "scrolled back to it", "explore view");
	assert_equal(true, marked.top >= 0 && marked.bottom <= f.extent.cy, "and in sight", "explore view");

	if (md && md->coordinate.is_valid())
	{
		assert_equal(true, f.view->globe_view() == md->coordinate, "the globe faces where it was taken",
		             "explore view");
		assert_equal(true, f.view->globe_zoom() > 1.0, "close", "explore view");
	}
}

// What the pointer finds on Explore's timeline is what is drawn there: a trip's label where it hangs over its
// year's header, and the header held at the top while its year scrolls, all of it, even when the row at the very
// top is a shorter one of years with nothing in them. A week cut short by a month with nothing in it has its
// bubble beside the days it is drawn as.
static void should_show_explore_timeline_bubbles_before_turning_the_globe()
{
	const auto help = std::exchange(setting.show_help_tooltips, true);
	const auto animations = std::exchange(render::animations_enabled, false);
	const core::scope_exit restore([=]
	{
		setting.show_help_tooltips = help;
		render::animations_enabled = animations;
	});
	explore_test_view f;
	const gps_coordinate start(0.0, 0.0);
	f.state.turn_globes(start);
	f.view->activate(f.extent);
	df::explore_request request;
	request.today = df::explore_day(2026, 1, 12);
	explore_test_view::add(request, request.today, "New Zealand");
	request.samples.back().coordinate = gps_coordinate(-40.0, 175.0);
	f.arrive(std::move(request));
	f.lay_out();
	while (f.as.run_next(async_queue::query)) f.as.drain_ui();

	const auto* year = f.view->timeline().year_row(2026);
	assert_equal(true, year != nullptr, "the year has a timeline header");
	if (!year) return;
	const auto year_label = f.drawn_label("2026", render::font_face::title);
	assert_equal(false, year_label.is_empty(), "the year is drawn");
	const auto loc = year_label.center();
	assert_equal(true, f.at(loc).kind == explore_target_kind::year, "the pointer is on the year");
	view_hover_element bubble;
	bubble.clear();
	bubble.active_delay = bubble_delay::help;
	f.view->bubble(explore_view::pane::timeline, bubble);
	assert_equal(false, bubble.is_empty(), "the year has a preview");
	assert_equal(0u, bubble.active_delay, "the preview does not wait for a turn");
	assert_equal(true, f.view->globe_view() == start, "building the preview does not turn the globe");
	f.view->tick_hover_turn(loc, 1000);
	f.view->tick_hover_turn(loc, 1499);
	assert_equal(true, f.view->globe_view() == start, "the pointer has not rested for long enough");
	f.view->tick_hover_turn(loc, 1500);
	assert_equal(true, f.view->globe_view() != start, "a deliberate hover turns to the year's places");
}

static void should_point_explore_at_what_its_timeline_draws()
{
	using df::explore_day;
	using kind = explore_target_kind;

	explore_test_view f;
	df::explore_request request;
	request.today = explore_day(2026, 1, 10);

	// Home all of 2025 but November, and three days in Croatia to end the year.
	for (auto day = explore_day(2025, 1, 1); day <= explore_day(2025, 12, 20); day += 2)
	{
		if (df::explore_date(day).month != 11) explore_test_view::add(request, day, "Australia");
	}

	for (auto day = explore_day(2025, 12, 29); day <= explore_day(2025, 12, 31); ++day)
	{
		explore_test_view::add(request, day, "Croatia");
		explore_test_view::add(request, day, "Croatia");
	}

	const auto model = f.arrive(std::move(request));
	f.lay_out();
	assert_equal(1u, static_cast<uint32_t>(model->trips.size()), "the days in Croatia are a trip", "explore view");

	const auto& layout = f.view->timeline();
	assert_equal(true, layout.rows.front().kind == explore_timeline_layout::row_kind::empty_years,
	             "a year with nothing in it heads the timeline", "explore view");

	const auto* const header = layout.year_row(2025);
	assert_equal(true, header != nullptr, "2025 has a header", "explore view");
	if (!header) return;
	const auto year_label = f.drawn_label("2025", render::font_face::title);
	assert_equal(false, year_label.is_empty(), "the year's label is drawn", "explore view");
	const auto content_top = year_label.top - header->top;
	const auto guidance_badge = [&]
	{
		return f.drawn_label(icon_to_utf8(icon_index::time), render::font_face::icons);
	};
	const auto intro = guidance_badge();
	assert_equal(true, !intro.is_empty() && intro.bottom <= year_label.top && f.at(intro.center()).is_none(),
	             "the timeline introduction comes before the dates and is not a date target", "explore view");

	auto trip_over_header = false;
	for (auto x = 0; x < f.extent.cx; x += 4)
	{
		trip_over_header |= f.at({x, year_label.bottom - 2}).kind == kind::trip;
	}
	assert_equal(true, trip_over_header, "the trip's label answers where it hangs over the header", "explore view");

	// 1 November 2025 was a Saturday, so the week of 27 October is drawn only as its five October days.
	const auto week = std::ranges::find(model->weeks, explore_day(2025, 10, 27), &df::explore_week::first_day);
	const auto week_index = static_cast<uint32_t>(week - model->weeks.begin());
	const auto drawn = std::ranges::find(layout.weeks, week_index, &explore_timeline_layout::span::index);
	assert_equal(true, drawn != layout.weeks.end(), "the week is drawn", "explore view");
	assert_equal(5 * layout.day_height, drawn->bottom - drawn->top, "as its October days", "explore view");

	auto found_week = false;
	for (auto x = 0; x < f.extent.cx && !found_week; x += 4)
	{
		const auto target = f.at({x, content_top + drawn->top + 4});
		found_week = target.kind == kind::week && target.index == week_index;
	}
	assert_equal(true, found_week, "the pointer finds the week", "explore view");

	ui::hover_bubble bubble;
	bubble.clear();
	f.view->bubble(explore_view::pane::timeline, bubble);
	assert_equal(false, bubble.is_empty(), "the week has a bubble", "explore view");
	assert_equal(content_top + drawn->top, bubble.window_bounds.top, "beside the days the week is drawn as",
	             "explore view");
	assert_equal(content_top + drawn->bottom, bubble.window_bounds.bottom, "all of them", "explore view");

	platform::wheel_notch notch;
	notch.delta = -12;
	f.view->mouse_wheel(intro.center(), notch);
	assert_equal(true, f.view->timeline_scroll() > 0 && guidance_badge() == intro.offset(0, -f.view->timeline_scroll()),
	             "scrolling over the introduction moves it with the dates", "explore view");
	notch.delta = -200;
	f.view->mouse_wheel({f.extent.cx - 100, f.extent.cy / 2}, notch);
	assert_equal(true, guidance_badge().is_empty(), "past the introduction no space is reserved for it",
	             "explore view");

	auto found_year = false;
	for (auto x = 0; x < f.extent.cx; x += 4)
	{
		const auto target = f.at({x, layout.year_height - 4});
		assert_equal(true, target.is_none() || target.kind == kind::year || target.kind == kind::year_place,
		             std::format("the header held at the top answers at {}", x), "explore view");
		found_year |= target.kind == kind::year;
	}
	assert_equal(true, found_year, "for its year", "explore view");
	notch.delta = f.view->timeline_scroll();
	f.view->mouse_wheel(intro.center(), notch);
	assert_equal(true, guidance_badge() == intro, "returning to the top reveals the introduction again",
	             "explore view");
}

void register_view_tests(view_state& /*state*/, test_registry& tests)
{
	tests.add("Should keep video thumbnails clear of playback progress overlays"s,
	          should_keep_video_thumbnails_clear_of_playback_progress_overlays);
	tests.add("Should lay out the SID monitor"s, should_lay_out_the_sid_monitor);
	tests.add("Should lay out the explore timeline"s, should_lay_out_the_explore_timeline);
	tests.add("Should show an explore model only once laid out"s, should_show_an_explore_model_only_once_laid_out);
	tests.add("Should put Back first in the history views' window toolbar"s,
	          should_put_back_first_in_the_history_views_window_toolbar);
	tests.add("Should offer person review in the Faces toolbar"s, should_offer_person_review_in_the_faces_toolbar);
	tests.add("Should keep explore where it was left"s, should_keep_explore_where_it_was_left);
	tests.add("Should show an item in explore"s, should_show_an_item_in_explore);
	tests.add("Should point explore at what its timeline draws"s, should_point_explore_at_what_its_timeline_draws);
	tests.add("Should show Explore timeline bubbles before turning the globe"s,
	          should_show_explore_timeline_bubbles_before_turning_the_globe);
	tests.add("Should keep music scrubbers usable in narrow panels"s,
	          should_keep_music_scrubbers_usable_in_narrow_panels);
	tests.add("Should fit every tracker channel before any scrolls away"s,
	          should_fit_every_tracker_channel_before_any_scrolls_away);
	tests.add("Should keep tracker channel headers in view"s, should_keep_tracker_channel_headers_in_view);
	tests.add("Should share ordinary media transport with native monitors"s,
	          should_share_ordinary_media_transport_with_native_monitors);
	tests.add("Should keep optional stream panels outside primary media"s,
	          should_keep_optional_stream_panels_outside_primary_media);
	tests.add("Should present pictures alone and the rest as a stack"s,
	          should_present_pictures_alone_and_the_rest_as_a_stack);
	tests.add("Should link the folder of an item found elsewhere"s, should_link_the_folder_of_an_item_found_elsewhere);
	tests.add("Should name the people in a picture"s, should_name_the_people_in_a_picture);
	tests.add("Should layout nested metadata document panels"s, should_layout_nested_metadata_document_panels);
	tests.add("Should fit the SID monitor to its space"s, should_fit_the_sid_monitor_to_its_space);
	tests.add("Should fade SID register activity"s, should_fade_sid_register_activity);
	tests.add("Should move the SID monitor with the tune"s, should_move_the_sid_monitor_with_the_tune);
	tests.add("Should draw an audio clip for a movie"s, should_draw_an_audio_clip_for_a_movie);
	tests.add("Should draw movie text screens and captions"s, should_draw_movie_text_screens_and_captions);
	tests.add("Should draw a YouTube Short"s, should_draw_a_youtube_short);
	// Release review - Mute and Solo read a monitor a rebuild had already destroyed.
	tests.add("Should toggle a SID voice after its monitor is rebuilt"s,
	          should_toggle_a_sid_voice_after_its_monitor_is_rebuilt);
	tests.add("Should run a Commodore disk from its listing"s, should_run_a_commodore_disk_from_its_listing);
	tests.add("Should start a Commodore cartridge"s, should_start_a_commodore_cartridge);
	tests.add("Should keep a Commodore 64 running only where it is seen"s,
	          should_keep_a_commodore_64_running_only_where_it_is_seen);
	tests.add("Should fit the Commodore 64 picture"s, should_fit_the_commodore_64_picture);
	// VIEW-012 - Movie clip sessions are retired before playback/focus no longer owns them.
	tests.add("Should decide when Movie clip sessions are retired"s,
	          should_decide_when_movie_clip_sessions_are_retired);
	tests.add("Should step Movie clips from the playhead while playing"s,
	          should_step_movie_clips_from_the_playhead_while_playing);
	tests.add("Should repaint a playing Movie once a movie frame"s, should_repaint_a_playing_movie_once_a_movie_frame);
	// VIEW-013 - stale Movie cache completions must not mutate current request state.
	tests.add("Should reject stale Movie cache completions before state changes"s,
	          should_reject_stale_movie_cache_completions_before_state_changes);
	// VIEW-014 - a source that moves during probing gets bounded retries, then an explicit state.
	tests.add("Should bound Movie probe retries for moving sources"s,
	          should_bound_movie_probe_retries_for_moving_sources);
	tests.add("Should apply a Movie probe to the clip it measured"s,
	          should_apply_a_movie_probe_to_the_clip_it_measured);
	// VIEW-016 - source caches are retired only for successful project/source replacement.
	tests.add("Should retire Movie source caches only after successful replacement"s,
	          should_retire_movie_source_caches_only_after_successful_replacement);
	tests.add("Should turn Movie frames upright"s, should_turn_movie_frames_upright);
	tests.add("Should count Movie cache bytes once per frame"s, should_count_movie_cache_bytes_once_per_frame);
	tests.add("Should seek a Movie preview back to where it was"s, should_seek_a_movie_preview_back_to_where_it_was);
	tests.add("Should show the Movie frame at a trim handle"s, should_show_the_movie_frame_at_a_trim_handle);
	tests.add("Should let either Movie trim handle be taken"s, should_let_either_movie_trim_handle_be_taken);
	tests.add("Should adopt only the Movie clip session asked for"s,
	          should_adopt_only_the_movie_clip_session_asked_for);
	// VIEW-005/006 - Edit load and preview replies are detached source state, not placeholder pixels.
	tests.add("Should keep Edit view load failure separate from placeholder pixels"s,
	          should_keep_edit_load_failure_separate_from_placeholder_pixels);
	// VIEW-005/007 - async Edit replies retire when display, preview size, or edit generation changes.
	tests.add("Should reject stale Edit load preview and analysis results"s,
	          should_reject_stale_edit_load_preview_and_analysis_results);
	// Edit preview stuck on Loading - the decode fits within its request rather than matching it.
	tests.add("Should draw Edit preview decoded short of its request"s,
	          should_draw_edit_preview_decoded_short_of_its_request);
	tests.add("Should show an Edit adjustment that rounds to zero without a sign"s, []
	{
		assert_equal("0.00"s, format_edit_adjustment(-0.0), "a perspective a quarter turn negated");
		assert_equal("0.00"s, format_edit_adjustment(-0.004), "a negative value that rounds to zero");
		assert_equal("-0.01"s, format_edit_adjustment(-0.006), "a negative value that does not");
		assert_equal("0.25"s, format_edit_adjustment(0.25), "a positive value");
	});
	// Saving an edited RAW suggested the same name as JPEG - the camera's own JPEG beside it - and a
	// second Save As checked the RAW's extension for a free name, then offered a JPEG already there.
	tests.add("Should suggest an edit a name of its own beside its source"s, []
	{
		const auto at = [](const std::string_view name)
		{
			return core::file_path(test_files_folder(), name);
		};
		const auto taken = [&at](const core::file_path p)
		{
			return p == at("IMG_1.jpg") || p == at("IMG_1-edit.jpg");
		};
		const auto none = [](const core::file_path)
		{
			return false;
		};

		assert_equal("IMG_1-edit 2.jpg"s, std::string(edit_save_suggestion(at("IMG_1.CR2"), taken).name().sv()),
		             "a RAW is saved as a JPEG of its own, past the edits already there");
		assert_equal("IMG_1-edit.jpg"s, std::string(edit_save_suggestion(at("IMG_1.CR2"), none).name().sv()),
		             "never as the camera's JPEG beside it");
		assert_equal("a-edit.png"s, std::string(edit_save_suggestion(at("a.png"), none).name().sv()),
		             "a format Diffractor writes keeps its own");
		assert_equal("clip-edit.mp4"s, std::string(edit_save_suggestion(at("clip.mp4"), none).name().sv()),
		             "a video's lossless copy keeps its format");
		assert_equal("clip-edit.mp4"s, std::string(edit_save_suggestion(at("clip.mpg"), none).name().sv()),
		             "and one in a format Edit does not write is an MP4");
	});
	// An edited HEIC of more than eight bits, an HDR one or one with a gain map was offered as a JPEG, which keeps
	// none of them; a copy in a format that cannot hold a photo's HDR or gain map was written without a word.
	tests.add("Should offer an AVIF for a deep HEIC and say what a copy loses"s, []
	{
		const auto at = [](const std::string_view name)
		{
			return core::file_path(test_files_folder(), name);
		};
		const auto none = [](const core::file_path)
		{
			return false;
		};

		file_load_result plain;
		plain.success = true;
		const auto eight = std::make_shared<core::surface>();
		eight->alloc(sizei(4, 4), core::texture_format::RGB);
		plain.s = eight;
		auto deep = plain;
		const auto sixteen = std::make_shared<core::surface>();
		sixteen->alloc(sizei(4, 4), core::texture_format::RGBX16);
		deep.s = sixteen;
		auto with_gain_map = plain;
		with_gain_map.signal.gain_map = true;

		assert_equal(true, edit_offers_avif(at("IMG_1.HEIC"), deep), "a HEIC of more than eight bits");
		assert_equal(true, edit_offers_avif(at("IMG_1.heic"), with_gain_map), "one with a gain map");
		assert_equal(false, edit_offers_avif(at("IMG_1.heic"), plain), "not one a JPEG holds whole");
		assert_equal(false, edit_offers_avif(at("IMG_1.avif"), deep), "an AVIF is saved as itself");
		assert_equal(false, edit_offers_avif(at("IMG_1.CR2"), deep), "a raw file as a JPEG");
		assert_equal("IMG_1-edit.avif"s, std::string(edit_save_suggestion(at("IMG_1.heic"), none, true).name().sv()),
		             "offered as an AVIF of its own");

		picture_signal hdr;
		hdr.hdr = libfiles::hdr_description{};
		picture_signal gain_map;
		gain_map.gain_map = true;

		for (const auto name : {"a.avif"sv, "a.jxl"sv, "a.png"sv, "a.jpg"sv})
			assert_equal(true, edit_save_loss(hdr, at(name), false).empty(), name, "holds an HDR picture");
		assert_equal(false, edit_save_loss(hdr, at("a.webp"), false).empty(), "a WebP does not");

		for (const auto name : {"a.avif"sv, "a.jpeg"sv})
			assert_equal(true, edit_save_loss(gain_map, at(name), false).empty(), name, "keeps a gain map");
		for (const auto name : {"a.png"sv, "a.webp"sv, "a.jxl"sv})
			assert_equal(false, edit_save_loss(gain_map, at(name), false).empty(), name, "leaves it out");
		assert_equal(true, edit_save_loss(gain_map, at("a.jxl"), true).empty(), "a JPEG repacked whole keeps it");
		assert_equal(true,
		             edit_save_loss(gain_map, at("IMG_1.HEIC"), false).empty() &&
		                 edit_save_loss(hdr, at("IMG_1.HEIC"), false).empty(),
		             "a HEIC is only ever turned or copied, which keeps all it holds");

		picture_signal narrowed;
		narrowed.narrowed = true;
		auto tone_mapped = narrowed;
		tone_mapped.tone_mapped = true;
		assert_equal(true, edit_photo_note({}).empty(), "a photo edited as it is goes unremarked");
		assert_equal(std::string(tt.edit_too_large_for_depth.sv()), std::string(edit_photo_note(narrowed)),
		             "too large for its depth");
		assert_equal(std::string(tt.edit_hdr_too_large.sv()), std::string(edit_photo_note(tone_mapped)),
		             "too large for HDR");
		assert_equal(std::string(tt.edit_hdr.sv()), std::string(edit_photo_note(hdr)), "HDR, and edited so");
	});
	// Issue #268 - Save As offered a video the formats a photo is saved in. A copy keeps the source's format, under
	// each extension it is written as, the source's first, and is offered as an MP4 besides where an MP4 holds it.
	tests.add("Should offer Save As a video's own format and MP4"s, []
	{
		const auto filters_for = [](const std::string_view name, const bool offers_mp4)
		{
			return edit_save_filters(core::file_path(test_files_folder(), name), offers_mp4);
		};

		const auto video = filters_for("clip.M4V", true);
		assert_equal(true, video.size() == 1, "an MP4 is offered its own format");
		assert_equal("MP4"s, video.front().text, "named as itself");
		assert_equal("*.m4v;*.mp4;*.mp4v"s, video.front().extensions, "under each of its extensions, its own first");

		const auto quicktime = filters_for("clip.mov", true);
		assert_equal(true, quicktime.size() == 2, "another format is offered as itself and as an MP4");
		assert_equal("QuickTime"s, quicktime.front().text, "its own first");
		assert_equal("*.mp4;*.m4v;*.mp4v"s, quicktime.back().extensions, "then MP4's");
		assert_equal(true, filters_for("clip.mov", false).size() == 1, "and not as an MP4 that would hold none of it");

		const auto program = filters_for("clip.mpg", true);
		assert_equal(true, program.size() == 1 && program.front().text == "MP4",
		             "a format Edit does not write is offered as an MP4 alone");
		assert_equal("*.m4a;*.m4b;*.m4r;*.mp4a"s, filters_for("song.m4a", true).front().extensions,
		             "a sound file in an MP4 is offered the extensions sound is kept under");
		assert_equal(true, filters_for("photo.jpg", false).empty(), "a photo is offered the formats Diffractor writes");
	});
	// Edit stuck on Loading - a file whose header loads but whose pixels do not decode.
	tests.add("Should report Edit preview that cannot decode as a load failure"s,
	          should_report_edit_preview_that_cannot_decode_as_a_load_failure);
	// G14b follow-up - a drawn crop must survive until the async bitmap load can apply it.
	tests.add("Should apply pending Edit crop after async load"s, should_apply_pending_edit_crop_after_async_load);
	// G14b follow-up - resizing keeps the last visible preview until the replacement is ready.
	tests.add("Should draw retained Edit preview while replacement is pending"s,
	          should_draw_retained_edit_preview_while_replacement_is_pending);
	// G14b follow-up - Edit load/preview workers coalesce superseded requests.
	tests.add("Should coalesce Edit load and preview requests to the latest"s,
	          should_coalesce_edit_load_and_preview_requests_to_the_latest);
	// G14b follow-up - side-panel pixel controls obey the actual source-pixel state.
	tests.add("Should withhold Edit pixel controls until source pixels are loaded"s,
	          should_withhold_edit_pixel_controls_until_source_pixels_are_loaded);
	tests.add("Should open in Edit the videos and sound files it copies back"s,
	          should_open_in_edit_the_videos_and_sound_files_it_copies_back);
	tests.add("Should save a HEIF turned in Edit over itself"s, should_save_a_heif_turned_in_edit_over_itself);
	tests.add("Should count the edit histogram as the colour is graded"s,
	          should_count_the_edit_histogram_as_the_colour_is_graded);
	tests.add("Should move a tone slider by its histogram handle"s, should_move_a_tone_slider_by_its_histogram_handle);
	tests.add("Should compare an edit with the original by its handle"s,
	          should_compare_an_edit_with_the_original_by_its_handle);
	tests.add("Should preview a video draft in Edit"s, should_preview_a_video_draft_in_edit);
	tests.add("Should drag Edit trim handles along the scrubber"s, should_drag_edit_trim_handles_along_the_scrubber);
	tests.add("Should return to the playhead when a trim handle is let go"s,
	          should_return_to_the_playhead_when_a_trim_handle_is_let_go);
	// VIEW-011 - Items routes touch pan through the same magnified-media zoom operation as Fullscreen.
	tests.add("Should pan Items touch zoom on magnified media like Fullscreen"s,
	          should_pan_items_touch_on_magnified_media_like_fullscreen);

	tests.add("Should track the active row across a reorder"s, should_track_the_active_row_across_a_reorder);
	tests.add("Should resolve list rows by work index"s, should_resolve_list_rows_by_work_index);
	tests.add("Should select settled zoom sampler"s, should_select_settled_zoom_sampler);
	tests.add("Should select video sampler by picture size"s, should_select_video_sampler_by_picture_size);
	// MOD-016 - same-path source versions can move backward or change without increasing a maximum.
	tests.add("Should reload displayed photo when source version moves backward"s,
	          should_reload_displayed_photo_when_source_version_moves_backward);
	tests.add("Should count a shown photo as loading until it reaches the window"s,
	          should_count_a_shown_photo_as_loading_until_it_reaches_the_window);
	// MOD-017 - displayed cache entries do not stop offscreen texture-budget eviction.
	tests.add("Should skip displayed textures when evicting retained texture budget"s,
	          should_skip_displayed_textures_when_evicting_retained_texture_budget);
	// MOD-018 - comparison preview texture identities are handed back on worker exits.
	tests.add("Should release comparison preview textures on UI after worker exit"s,
	          should_release_comparison_preview_textures_on_ui_after_worker_exit);
	tests.add("Should show hover frames while the pointer moves"s, should_show_hover_frames_while_the_pointer_moves);
	tests.add("Should accumulate precision wheel deltas"s, should_accumulate_precision_wheel_deltas);
	tests.add("Should validate zoom navigator mode"s, should_validate_zoom_navigator_mode);
	tests.add("Should keep comparison zoom panes matched"s, should_keep_comparison_zoom_panes_matched);
	tests.add("Should calculate zoom model fit without enlarging"s, should_calculate_zoom_fit_without_enlarging);
	tests.add("Should recalculate zoom fit variants"s, should_recalculate_zoom_fit_variants);
	tests.add("Should keep the pan when a fit variant refits"s, should_keep_the_pan_when_a_fit_variant_refits);
	tests.add("Should step zoom model reversibly through fit"s, should_step_zoom_reversibly_through_fit);
	tests.add("Should keep zoom model anchor still"s, should_keep_zoom_anchor_still);
	tests.add("Should step zoom model at pointer anchor"s, should_step_zoom_at_pointer_anchor);
	tests.add("Should preserve zoom model center across dimensions"s, should_preserve_zoom_center_across_dimensions);
	tests.add("Should suspend carried zoom model below fit"s, should_suspend_carried_zoom_below_fit);
	tests.add("Should clamp zoom model pan to edges"s, should_clamp_zoom_model_pan_to_edges);
	tests.add("Should toggle zoom model fit to last explicit view"s, should_toggle_fit_to_last_explicit_zoom);
	tests.add("Should toggle zoom model fit back to a fit variant"s, should_toggle_fit_back_to_a_fit_variant);
	tests.add("Should hit test ratings within five stars"s, should_hit_test_ratings_within_five_stars);
	tests.add("Should keep zoom model anchor through layout change"s, should_keep_zoom_anchor_through_layout_change);
	tests.add("Should zoom model region to viewport"s, should_zoom_model_region_to_viewport);
	tests.add("Should qualify a panorama by declaration or shape"s, should_qualify_a_panorama_by_declaration_or_shape);
	tests.add("Should look around inside a projected panorama"s, should_look_around_inside_a_projected_panorama);
	tests.add("Should step the panorama field of view ladder"s, should_step_the_panorama_field_of_view_ladder);
	tests.add("Should aim a projected panorama with a held pointer"s,
	          should_aim_a_projected_panorama_with_a_held_pointer);
	tests.add("Should place a partial panorama on the sphere"s, should_place_a_partial_panorama_on_the_sphere);
	tests.add("Should refresh same path panorama declaration without resetting view"s,
	          should_refresh_same_path_panorama_declaration_without_resetting_view);
	tests.add("Should hold a drawn region in source space"s, should_hold_a_drawn_region_in_source_space);
	tests.add("Should map a drawn region onto stored pixels"s, should_map_a_drawn_region_onto_stored_pixels);
	tests.add("Should look around without choosing a scale"s, should_look_around_without_choosing_a_scale);
	tests.add("Should fit a stand in rather than stretch it"s, should_fit_a_stand_in_rather_than_stretch_it);
	tests.add("Should accelerate zoom pan from drag origin"s, should_accelerate_pan_from_drag_origin);
	tests.add("Should bound zoom auto-pan velocity"s, should_bound_auto_pan_velocity);
	tests.add("Should map zoom navigator to source center"s, should_map_zoom_navigator_to_source_center);
	tests.add("Should orient selector thumbnails"s, should_orient_selector_thumbnails);
	tests.add("Should range select across selector"s, should_range_select_across_selector);
	// VIEW-008 - selector rebuilds reuse entries through one lookup instead of repeated scans.
	tests.add("Should reuse selector entries with one lookup"s, should_reuse_selector_entries_with_one_lookup);
	// VIEW-009 - selector-owned resources are bounded to the visible/prefetch band.
	tests.add("Should bound selector retained resources to visible band"s,
	          should_bound_selector_retained_resources_to_visible_band);
	tests.add("Should rearm selector decode after resource event"s, should_rearm_selector_decode_after_resource_event);
	tests.add("Should follow the view's item along the strip"s, should_follow_the_view_s_item_along_the_strip);
	tests.add("Should mark the selection on the strip's scroll bar"s,
	          should_mark_the_selection_on_the_strip_s_scroll_bar);
	tests.add("Should mark the selection on the items scroll bar"s, should_mark_the_selection_on_the_items_scroll_bar);
	tests.add("Should keep an items band until it is let go"s, should_keep_an_items_band_until_it_is_let_go);
	tests.add("Should show the strip's item once it has its size"s, should_show_the_strip_s_item_once_it_has_its_size);
	tests.add("Should keep the strip's item in place as its set shrinks"s,
	          should_keep_the_strip_s_item_in_place_as_its_set_shrinks);
	tests.add("Should step through what the strip offers"s, should_step_through_what_the_strip_offers);
	tests.add("Should refresh tooltip when staged thumbnail lands"s,
	          should_refresh_tooltip_when_staged_thumbnail_lands);
	tests.add("Should not refresh tooltip for fresh request when only cover art is cached"s,
	          should_not_refresh_tooltip_for_fresh_request_when_only_cover_art_is_cached);
	tests.add("Should stage one scroll tooltip thumbnail"s, should_stage_one_scroll_tooltip_thumbnail);
	tests.add("Should retry scroll tooltip after failed stage"s, should_retry_scroll_tooltip_after_failed_stage);
	tests.add("Should publish async selection thumbnail strip"s, should_publish_async_selection_thumbnail_strip);
	tests.add("Should discard async selection strip after clear"s, should_discard_async_selection_strip_after_clear);
	tests.add("Should not keep selection strip frame alive"s, should_not_keep_selection_strip_frame_alive);
	tests.add("Should preview rotation with inverse destination transform"s,
	          should_preview_rotation_with_inverse_destination_transform);
	// Issue #260 - the anticlockwise preview turned the picture clockwise.
	tests.add("Should preview a rotation as it is written"s, should_preview_a_rotation_as_it_is_written);
	// The task views' columns are resized by their dividers.
	tests.add("Should resize list columns by their dividers"s, should_resize_list_columns_by_their_dividers);
	tests.add("Should show a run on the rows it ran"s, should_show_a_run_on_the_rows_it_ran);
	tests.add("Should select a review by its keys"s, should_select_a_review_by_its_keys);
	tests.add("Should sort a review from its headings"s, should_sort_a_review_from_its_headings);
	tests.add("Should find and step the keyboard's parts"s, should_find_and_step_the_keyboard_s_parts);
	tests.add("Should press a part as a click does"s, should_press_a_part_as_a_click_does);
	tests.add("Should run a table from edge to edge"s, should_run_a_table_from_edge_to_edge);
	tests.add("Should stage neighbour stand ins"s, should_stage_neighbour_stand_ins);
	// Issue #78 - anamorphic video fills its box at the declared aspect
	tests.add("Should shape video by its display dimensions"s, should_shape_video_by_its_display_dimensions);
	tests.add("Should show selected thumbnail without waiting"s, should_show_selected_thumbnail_without_waiting);
	tests.add("Should hold media column still as detail arrives"s, should_hold_media_column_still_as_detail_arrives);
	tests.add("Should fit the whole primary block in the media column"s,
	          should_fit_the_whole_primary_block_in_the_media_column);
	tests.add("Should follow the primary block when verbose is open"s,
	          should_follow_the_primary_block_when_verbose_is_open);
	tests.add("Should centre a media column with no detail"s, should_centre_a_media_column_with_no_detail);
	tests.add("Should follow a centred media column block with its detail"s,
	          should_follow_a_centred_block_with_its_detail);
	tests.add("Should keep half the media column pane for the media"s, should_keep_half_the_pane_for_the_media);
	tests.add("Should bound the media overlay to a quarter"s, should_bound_the_media_overlay_to_a_quarter);
	tests.add("Should leave full screen for task views"s, should_leave_full_screen_for_task_views);
	tests.add("Should preserve view scroller anchor across layout"s,
	          should_preserve_view_scroller_anchor_across_layout);
	tests.add("Should map the view scroller track to the list"s, should_map_the_view_scroller_track_to_the_list);
	tests.add("Should drag view scroller thumb from grab point"s, should_drag_view_scroller_thumb_from_grab_point);
	tests.add("Should close view scroller bands over track"s, should_close_view_scroller_bands_over_track);
	tests.add("Should survive a host with no window"s, should_survive_a_host_with_no_window);
	tests.add("Should not perform a cancelled gesture on release"s, should_not_perform_a_cancelled_gesture_on_release);
	tests.add("Should offer escape to an idle controller"s, should_offer_escape_to_an_idle_controller);
	// Issue #255 - a clip dragged into an editor was pasted twice.
	tests.add("Should start one drag per press on a selected item"s,
	          should_start_one_drag_per_press_on_a_selected_item);
	tests.add("Should select nothing on a release without its press"s,
	          should_select_nothing_on_a_release_without_its_press);
	tests.add("Should bound a reused hit test controller by what it passed over"s,
	          should_bound_a_reused_controller_by_what_it_passed_over);
	tests.add("Should not clip a hit test controller to what covers it"s,
	          should_not_clip_a_controller_to_what_covers_it);
	tests.add("Should keep a partly covered control clickable"s, should_keep_a_partly_covered_control_clickable);
	tests.add("Should not invoke an escaped click"s, should_not_invoke_an_escaped_click);
	tests.add("Should separate hit test occlusion from exclusion"s, should_separate_occlusion_from_exclusion);
	tests.add("Should keep the processing row clear of the view chrome"s,
	          should_keep_the_processing_row_clear_of_the_view_chrome);
	// Concurrency review #1 - Refresh restarted Analyze without stopping the scan it replaced.
	tests.add("Should cancel the run a new one replaces"s, should_cancel_the_run_a_new_one_replaces);
	// Concurrency review #27 - a late place search moved the Locate coordinate back after a pan.
	tests.add("Should not let a late place search override the map"s,
	          should_not_let_a_late_place_search_override_the_map);
	// Concurrency fix review - Refresh during a Sync run reran Analyze over the run.
	tests.add("Should leave a running sync alone on refresh"s, should_leave_a_running_sync_alone_on_refresh);
	tests.add("Should answer a null frame without side effects"s, should_answer_a_null_frame_without_side_effects);
	// G13 UI-001 - checkbox composites forward hit testing to the rendered child only while active.
	tests.add("Should forward checkbox hits to visible checked child"s,
	          should_forward_checkbox_hits_to_visible_checked_child);
	tests.add("Should keep the options of an unchosen format disabled"s,
	          should_keep_the_options_of_an_unchosen_format_disabled);
	// G13 UI-002 - failed decoded dialog thumbnails do not enter the surface collection or cap.
	tests.add("Should reject failed dialog thumbnail decodes"s, should_reject_failed_dialog_thumbnail_decodes);
	// G13 UI-003 - search controls expose the list frame through the non-null accessor.
	tests.add("Should use non-null search list frame"s, should_use_non_null_search_list_frame);
	tests.add("Should reach the frames inside an option and a table"s,
	          should_reach_the_frames_inside_an_option_and_a_table);
	tests.add("Should pick and scroll a search's results in a dialog"s,
	          should_pick_and_scroll_a_search_s_results_in_a_dialog);
	tests.add("Should fit a dialog to content that arrives after it shows"s,
	          should_fit_a_dialog_to_content_that_arrives_after_it_shows);
	tests.add("Should take a dialog off the open dialogs as it goes"s,
	          should_take_a_dialog_off_the_open_dialogs_as_it_goes);
	tests.add("Should list a batch's results once they wait on Close"s,
	          should_list_a_batch_s_results_once_they_wait_on_close);
	// G13 UI-012 - failed texture uploads leave a hole rather than compacting later thumbnails.
	tests.add("Should preserve thumbnail texture slots after upload failure"s,
	          should_preserve_thumbnail_texture_slots_after_upload_failure);
	tests.add("Should layout selection thumbnail collage"s, should_layout_selection_thumbnail_collage);
	// PLAT-005 - clipboard access and publication failures preserve text edits; the edit is libdesktop's.
	tests.add("Should keep the typed filter and match text containing it"s,
	          should_keep_the_typed_filter_and_match_text_containing_it);
	tests.add("Should accumulate RTL text widths in logical order"s,
	          should_accumulate_rtl_text_widths_in_logical_order);
	tests.add("Should hold a typed slider value to its range"s, should_hold_a_typed_slider_value_to_its_range);
	tests.add("Should show and take a slider value in tenths as seconds"s,
	          should_show_and_take_a_slider_value_in_tenths_as_seconds);
	tests.add("Should draw the title it was given after its source changes"s,
	          should_draw_the_title_it_was_given_after_its_source_changes);
	// Shared labels and numeric fields consumed the entire track in a narrow controls pane.
	tests.add("Should keep narrow slider rows usable"s, should_keep_narrow_slider_rows_usable);
	tests.add("Should lay out dialog buttons in a centred row"s, should_lay_out_dialog_buttons_in_a_centred_row);
	tests.add("Should keep the unsaved decision outside the scrolling dialog body"s,
	          should_keep_the_unsaved_decision_outside_the_scrolling_body);
	tests.add("Should wrap a panel row of buttons that do not fit"s,
	          should_wrap_a_panel_row_of_buttons_that_do_not_fit);
	tests.add("Should fit a cell text to its column with an ellipsis"s,
	          should_fit_a_cell_text_to_its_column_with_an_ellipsis);
	// A failed image without dimensions collapsed its destination rectangle and hid the error.
	tests.add("Should draw image errors without source dimensions"s,
	          should_draw_image_errors_without_source_dimensions);
	tests.add("Should offer the face actions the face book allows on a picture"s,
	          should_offer_the_face_actions_the_face_book_allows_on_a_picture);
	tests.add("Should draw only hex rows inside the clip"s, should_draw_only_hex_rows_inside_the_clip);
	tests.add("Should read a document in its encoding"s, should_read_a_document_in_its_encoding);
	tests.add("Should show bytes that are not text as a dump"s, should_show_bytes_that_are_not_text_as_a_dump);
	tests.add("Should wrap document text at word boundaries"s, should_wrap_document_text_at_word_boundaries);
	tests.add("Should copy a document selection as its source text"s,
	          should_copy_a_document_selection_as_its_source_text);
	tests.add("Should copy a dump selection as its rows"s, should_copy_a_dump_selection_as_its_rows);
	tests.add("Should record the state each document line starts in"s,
	          should_record_the_state_each_document_line_starts_in);
	tests.add("Should select document text by dragging"s, should_select_document_text_by_dragging);
	tests.add("Should release the document on a press elsewhere"s, should_release_the_document_on_a_press_elsewhere);
	tests.add("Should draw the document selection behind its text"s,
	          should_draw_the_document_selection_behind_its_text);
	tests.add("Should use common document panels and tables"s, should_use_common_document_panels_and_tables);
	tests.add("Should draw the ordinary text of formatted documents"s,
	          should_draw_the_ordinary_text_of_formatted_documents);
	tests.add("Should use shared tables for file listings"s, should_use_shared_tables_for_file_listings);
	tests.add("Should colour a dump by byte class"s, should_colour_a_dump_by_byte_class);
	tests.add("Should merge control colours as straight alpha"s, should_merge_control_colours_as_straight_alpha);
	tests.add("Should hit test comparison divider in device coordinates"s,
	          should_hit_test_comparison_divider_in_device_coordinates);
	tests.add("Should clear detail row layout metrics"s, should_clear_detail_row_layout_metrics);
	tests.add("Should classify aspect ratio groups"s, should_classify_aspect_ratio_groups);

	//
	// Flex layout
	//
	tests.add("Should layout flex elements"s, should_layout_flex_elements);
	tests.add("Should keep a picture's shape in a flex line"s, should_keep_a_pictures_shape_in_a_flex_line);

	//
	// Item tile geometry
	//
	tests.add("Should limit comparison to like pairs"s, should_limit_comparison_to_like_pairs);
	tests.add("Should shape the tile by what it draws"s, should_shape_the_tile_by_what_it_draws);
	tests.add("Should not justify a tile shaped by a thumbnail"s, should_not_justify_a_tile_shaped_by_a_thumbnail);
	tests.add("Should keep tile aspect when laying out a row"s, should_keep_tile_aspect_when_laying_out_a_row);
	tests.add("Should order shows and albums by number"s, should_order_shows_and_albums_by_number);
	tests.add("Should sort and size a detail group from its headings"s,
	          should_sort_and_size_a_detail_group_from_its_headings);
	tests.add("Should mark the hovered item at once"s, should_mark_the_hovered_item_at_once);
	tests.add("Should play on to the next episode"s, should_play_on_to_the_next_episode);
}
