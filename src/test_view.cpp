// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: View-geometry tests. Covers the zoom model (fit, stepping, anchoring, panning and the
// navigator), the item selector / view scroller widgets, and the Movie view's pure decisions
// (clip stepping, session retirement, stale completions). No file, codec or database access, so
// these run in microseconds and stay apart from the media-edit tests.

#include "pch.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "files.h"
#include "model_zoom.h"
#include "ui_elements.h"
#include "ui_dialog.h"
#include "ui_panorama.h"
#include "ui_text_edit.h"
#include "ui_date_edit.h"
#include "app_sidebar.h"
#include "view_items.h"
#include "view_edit.h"
#include "view_list.h"
#include "view_locate.h"
#include "view_media.h"
#include "view_movie.h"
#include "view_selector.h"
#include "view_sync.h"
#include "view_tags.h"

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

	auto clip = make_movie_clip(df::file_path(df::windows_path_semantics ? "c:\\movie\\b.mp4" : "/movie/b.mp4"), {});
	clip.start = 12.0;
	assert_equal(12.0, rewound_movie_clip_playhead(&clip), "rewind parks at the focused clip's in point");
	assert_equal(0.0, rewound_movie_clip_playhead(nullptr), "an empty timeline rewinds to zero");
}

static void should_step_movie_clips_from_the_playhead_while_playing()
{
	std::vector<movie_clip> clips;

	for (auto i = 0; i < 9; ++i)
	{
		auto clip = make_movie_clip(df::file_path(df::windows_path_semantics ? "c:\\movie\\c.mp4" : "/movie/c.mp4"), {});
		clip.start = 0.0;
		clip.end = 10.0;
		clips.emplace_back(clip);
	}

	movie_settings settings;
	settings.transition = movie_transition::crossfade;
	settings.transition_seconds = 1.0;

	// Clip i starts at 9i and its crossfade into clip i + 1 runs from 9(i + 1) to 9(i + 1) + 1.
	const auto starts = calc_movie_timing(clips, settings).starts;
	constexpr size_t focus = 0;

	// Counting from the focus, as the transport used to, makes the first answer 1.
	assert_equal(size_t{7}, movie_step_target(starts, focus, true, 58.0, true),
	             "Next while clip 6 plays goes to clip 7, whatever is focused");
	assert_equal(size_t{5}, movie_step_target(starts, focus, true, 58.0, false),
	             "and Previous to clip 5");

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
	const auto nearest_frame = [](const double time) { return static_cast<int>(std::lround(time * 60)); };

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

	const auto shared = df::file_path(df::windows_path_semantics ? "c:\\movie\\shared.mp4" : "/movie/shared.mp4");
	std::vector<movie_probe_result> results(3);
	for (auto& result : results)
	{
		result.path = shared;
		result.changed_during_probe = true;
	}

	df::hash_map<df::file_path, int, df::ihash, df::ieq> retries;
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

// A Movie Maker clip the user never trimmed arrives with no end of its own. The probe is what
// measures its source, so the probe is what must give it the rest of that source -- the same rule
// as a video just added, shared by the view and the command line.
static void should_apply_a_movie_probe_to_the_clip_it_measured()
{
	const auto path = df::file_path(df::windows_path_semantics ? "c:\\movie\\clip.mp4" : "/movie/clip.mp4");

	movie_probe_result measured;
	measured.path = path;
	measured.found = true;
	measured.duration = 30.0;
	measured.extent = {640, 480};
	measured.frame_rate = 25.0;

	movie_clip untrimmed;
	untrimmed.path = path;
	const auto whole = apply_movie_probe(untrimmed, measured, 4.0);
	assert_equal(true, whole.is_probed && !whole.is_missing, "a measured source is present");
	assert_equal(30.0, whole.end, "a clip with no end runs to the end of its source");
	assert_equal(false, whole.is_trimmed(), "and is not shown as trimmed");
	assert_equal(25.0, whole.frame_rate, "and takes the measured rate");

	movie_clip start_only;
	start_only.path = path;
	start_only.start = 4.5;
	const auto rest = apply_movie_probe(start_only, measured, 4.0);
	assert_equal(4.5, rest.start, "a clip trimmed only at its start keeps its in point");
	assert_equal(30.0, rest.end, "and runs to the end of its source");

	movie_clip overlong;
	overlong.path = path;
	overlong.start = 2.0;
	overlong.end = 50.0;
	const auto clamped = apply_movie_probe(overlong, measured, 4.0);
	assert_equal(2.0, clamped.start, "a stored trim keeps its in point");
	assert_equal(30.0, clamped.end, "and is clamped to the source it measured");

	auto gone = measured;
	gone.found = false;
	const auto lost = apply_movie_probe(overlong, gone, 4.0);
	assert_equal(true, lost.is_lost(), "a source that is not there is lost rather than unexamined");
	assert_equal(50.0, lost.end, "and keeps its stored times for a relink");

	auto still = measured;
	still.is_photo = true;
	still.duration = 0;
	movie_clip photo;
	photo.path = path;
	const auto held = apply_movie_probe(photo, still, 4.0);
	assert_equal(true, held.is_photo, "a measured photo is a photo");
	assert_equal(4.0, held.end, "and a photo with no hold takes the default one");
}

// A Movie Maker project names its sources where they were years ago. Import finds each one by name
// among the files it is offered, keeps a namesake only when its length agrees with the one the
// project recorded, and hands back the clips already measured -- the work Movie > Import and
// -movie-import share.
static void should_import_a_movie_maker_project_from_the_collection()
{
	const auto video = test_files_folder.combine_file("gizmo.mp4");
	const auto measured = probe_movie_source(video);
	assert_equal(true, measured.found && measured.duration > 1.6, "the fixture video measures");

	const auto gone = test_files_folder.combine("no such folder");
	const auto moved = gone.combine_file("gizmo.mp4");
	const auto other = gone.combine_file("trip.mp4");
	const auto lost = gone.combine_file("lost.mp4");

	const auto xml = std::format(R"(<Project>
  <MediaItems>
    <MediaItem id="1" filePath="{}" duration="{}" />
    <MediaItem id="2" filePath="{}" duration="{}" />
    <MediaItem id="3" filePath="{}" duration="12.5" />
    <MediaItem id="4" filePath="{}" duration="{}" />
  </MediaItems>
  <Extents>
    <VideoClip extentID="10" mediaItemID="1" inTime="0" outTime="0" />
    <VideoClip extentID="11" mediaItemID="2" inTime="0" outTime="0" />
    <VideoClip extentID="12" mediaItemID="3" inTime="1" outTime="0" />
    <VideoClip extentID="13" mediaItemID="4" inTime="0" outTime="0" />
    <VideoClip extentID="14" mediaItemID="1" inTime="0.5" outTime="1.5" />
  </Extents>
</Project>)", moved, measured.duration, other, measured.duration + 30.0, lost, video, measured.duration);

	auto searches = 0;
	const movie_source_finder find = [&](const std::vector<df::file_path>& missing)
	{
		++searches;
		std::vector<std::vector<df::file_path>> result(missing.size());

		// The fixture stands in for both namesakes: the right file for one, a different one for the other.
		for (size_t i = 0; i < missing.size(); ++i)
		{
			if (missing[i] == moved || missing[i] == other) result[i].emplace_back(video);
		}

		return result;
	};

	const auto imported = import_movie_maker_project(xml, test_files_folder, find);
	const auto& clips = imported.loaded.clips;

	assert_equal(true, static_cast<bool>(imported.loaded), "the project imports");
	assert_equal(5_z, clips.size(), "with every clip, found or not");
	assert_equal(4_z, imported.sources.size(), "and one report per source");
	assert_equal(1, searches, "every missing source is looked for at once");

	assert_equal(true, imported.sources[0].match == movie_source_match::only_name, "a moved source is found by name");
	assert_equal(video.pack(), clips[0].path.pack(), "and the clip points at it");
	assert_equal(true, clips[0].is_probed && !clips[0].is_missing, "already measured");
	assert_equal(measured.duration, clips[0].end, "an untrimmed clip runs to the end of the file found");
	assert_equal(1.5, clips[4].end, "a trimmed clip of the same source keeps its trim");
	assert_equal(2, imported.relinked_clips, "both clips of the moved source are relinked");

	assert_equal(true, imported.sources[1].match == movie_source_match::other_length,
	             "a namesake whose length disagrees with the record is another file");
	assert_equal(other.pack(), clips[1].path.pack(), "so the clip keeps its stored path");
	assert_equal(true, clips[1].is_lost(), "and is missing rather than wrong");
	assert_equal(measured.duration + 30.0, clips[1].end, "holding the length the project recorded");

	assert_equal(true, imported.sources[2].match == movie_source_match::missing, "a name found nowhere is missing");
	assert_equal(1.0, clips[2].start, "a lost clip keeps its in point");
	assert_equal(12.5, clips[2].end, "and runs to the recorded end, so a saved project keeps it");

	assert_equal(true, imported.sources[3].match == movie_source_match::stored, "a source where it was stored stays");
	assert_equal(true, clips[3].is_probed && !clips[3].is_missing, "and is measured");
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
	assert_equal(true, sizei{1200, 1800} == upright_movie_extent({1800, 1200}, ui::orientation::right_top),
	             "a quarter-turned source is measured upright");
	assert_equal(true, sizei{1800, 1200} == upright_movie_extent({1800, 1200}, ui::orientation::bottom_right),
	             "a half-turned source keeps its shape");

	const auto source = std::make_shared<ui::surface>();
	source->alloc(3, 2, ui::texture_format::RGB, ui::orientation::right_top);
	for (auto y = 0; y < 2; ++y)
	{
		for (auto x = 0; x < 3; ++x) source->set_pixel(x, y, ui::rgba(10 + x + y * 3, 0, 0));
	}

	const auto turned = upright_movie_frame(source);
	assert_equal(true, sizei{2, 3} == turned->dimensions(), "the frame is turned upright");
	assert_equal(static_cast<int>(ui::orientation::top_left), static_cast<int>(turned->orientation()),
	             "and no longer asks to be turned again");

	// Orientation 6 stores the visual top as column 0 and the visual right as row 0, so each displayed
	// corner is a known stored pixel - an expectation that does not borrow the transform under test.
	assert_equal(source->get_pixel(0, 1), turned->get_pixel(0, 0), "top left is the stored bottom of column 0");
	assert_equal(source->get_pixel(0, 0), turned->get_pixel(1, 0), "top right is the stored top of column 0");
	assert_equal(source->get_pixel(2, 1), turned->get_pixel(0, 2), "bottom left is the stored bottom of column 2");
	assert_equal(source->get_pixel(2, 0), turned->get_pixel(1, 2), "bottom right is the stored top of column 2");

	const auto upright = std::make_shared<ui::surface>();
	upright->alloc(3, 2, ui::texture_format::RGB, ui::orientation::top_left);
	assert_equal(true, upright_movie_frame(upright) == ui::const_surface_ptr(upright),
	             "an upright frame is used as it is");

	const auto target = recti(0, 0, 20, 30);
	assert_equal(true, upright_movie_destination(target, ui::orientation::top_left) == quadd(target),
	             "an upright texture draws into its target unchanged");
	assert_equal(false, upright_movie_destination(target, ui::orientation::right_top) == quadd(target),
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

static df::item_element_ptr make_versioned_photo_item(const df::file_path path, const df::date_t file_modified,
                                                      const df::date_t thumbnail_modified)
{
	df::index_file_item info;
	info.ft = files::file_type_from_name(path.name());
	info.file_created = file_modified;
	info.file_modified = file_modified;
	auto result = std::make_shared<df::item_element>(path, info);
	result->thumbnail(nullptr, nullptr, thumbnail_modified);
	return result;
}

static void update_versioned_photo_item(const df::item_element_ptr& item, const df::file_path path,
                                        const df::date_t file_modified, const df::date_t thumbnail_modified)
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
	const auto path = df::file_path("c:\\source-version.jpg");
	const auto item = make_versioned_photo_item(path, df::date_t(200), df::date_t(200));
	auto texture = std::make_shared<texture_state>(as, item);

	texture->load_image(item);
	assert_equal(1_z, as.pending_worker_count(async_queue::load), "the initial load is queued");

	update_versioned_photo_item(item, path, df::date_t(100), df::date_t(200));
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "a changed file stamp reloads even when the maximum timestamp is unchanged");
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load), "the same tuple does not reload again");

	update_versioned_photo_item(item, path, df::date_t(100), df::date_t(100));
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "thumbnail stamp changes alone do not full-decode the displayed photo again");

	texture->mark_visuals_current();
	update_versioned_photo_item(item, path, df::date_t(50), df::date_t(100));
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "a metadata-only write keeps the current visual across its new stamp");
	texture->refresh(item);
	assert_equal(2_z, as.pending_worker_count(async_queue::load),
	             "the retained metadata-only version is adopted rather than reloading later");

	file_load_result written;
	written.success = true;
	const auto written_stamp = df::date_t(2026, 1, 5, 12, 0, 0);
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
		if (ui::is_ui_thread()) ++counts.ui;
		else ++counts.worker;
		delete texture;
	});
}

class media_preview_test_async final : public null_async_strategy
{
	std::deque<std::function<void()>> _ui;
	std::function<void(media_preview_state&)> _preview;

public:
	void queue_ui(const std::function<void()> f) override
	{
		_ui.emplace_back(f);
	}

	void queue_media_preview(std::function<void(media_preview_state&)> f, bool) override
	{
		_preview = std::move(f);
	}

	void drain_ui()
	{
		while (!_ui.empty())
		{
			auto f = std::move(_ui.front());
			_ui.pop_front();
			f();
		}
	}

	size_t pending_ui_count() const
	{
		return _ui.size();
	}

	void drop_preview_on_worker()
	{
		auto preview = std::move(_preview);
		std::thread worker([preview = std::move(preview)]() mutable
		{
		});
		worker.join();
	}

	void run_preview_on_worker()
	{
		auto preview = std::move(_preview);
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
	const auto path1 = df::file_path("c:\\compare-a.jpg");
	const auto path2 = df::file_path("c:\\compare-b.jpg");

	const auto run_case = [&](const bool run_preview, const bool keep_current_until_ui, const std::string_view label)
	{
		media_preview_test_async as;
		common_display_state_t common;
		auto display = std::make_shared<display_state_t>(as, common);
		texture_release_counts counts;

		auto texture1 = make_counted_texture(as, make_versioned_photo_item(path1, df::date_t(1), {}), counts);
		auto texture2 = make_counted_texture(as, make_versioned_photo_item(path2, df::date_t(1), {}), counts);
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
			assert_equal(0, counts.worker.load(), std::format("{}: successful callback still releases no worker texture", label));
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

static void should_select_settled_zoom_sampler()
{
	assert_equal(true, calc_sampler({1000, 500}, {1000, 500}, ui::orientation::top_left) ==
	             ui::texture_sampler::point, "one-to-one is exact");
	assert_equal(true, calc_sampler({1003, 500}, {1000, 500}, ui::orientation::top_left) ==
	             ui::texture_sampler::bicubic, "just above one-to-one is settled smooth");
	assert_equal(true, calc_sampler({1500, 750}, {1000, 500}, ui::orientation::top_left) ==
	             ui::texture_sampler::bicubic, "one and a half times is settled smooth");
	assert_equal(true, calc_sampler({3000, 1500}, {1000, 500}, ui::orientation::top_left) ==
	             ui::texture_sampler::bicubic, "three times remains smooth");
	assert_equal(true, calc_sampler({3001, 1500}, {1000, 500}, ui::orientation::top_left) ==
	             ui::texture_sampler::point, "above three times keeps source pixels exact");
	assert_equal(true, calc_sampler({3001, 1500}, {1000, 500}, ui::orientation::top_left, false, true) ==
	             ui::texture_sampler::bicubic, "a magnified stand-in stays smooth rather than blocky");
	assert_equal(true, calc_sampler({1000, 500}, {1000, 500}, ui::orientation::top_left, false, true) ==
	             ui::texture_sampler::point, "a stand-in at one-to-one is still exact");
	assert_equal(true, calc_sampler({1003, 500}, {1000, 500}, ui::orientation::top_left, true) ==
	             ui::texture_sampler::bilinear, "interactive magnification just above one-to-one uses fast sampler");
	assert_equal(true, calc_sampler({1500, 750}, {1000, 500}, ui::orientation::top_left, true) ==
	             ui::texture_sampler::bilinear, "interactive magnification uses fast sampler");
	assert_equal(true, calc_sampler({3001, 1500}, {1000, 500}, ui::orientation::top_left, true) ==
	             ui::texture_sampler::point, "interactive magnification above three times keeps source pixels exact");
}

static void should_accumulate_precision_wheel_deltas()
{
	// The frame boundary accumulates the raw platform delta, one detent being 120.
	auto pending = 0.0;
	assert_equal(0, ui::accumulate_wheel_steps(pending, 40.0), "first partial detent");
	assert_equal(0, ui::accumulate_wheel_steps(pending, 40.0), "second partial detent");
	assert_equal(0, ui::accumulate_wheel_steps(pending, 39.0), "detent not rounded early");
	assert_equal(1, ui::accumulate_wheel_steps(pending, 1.0), "exact detent steps once");
	assert_zoom_near(0.0, pending, "positive detent consumed exactly");
	assert_equal(-2, ui::accumulate_wheel_steps(pending, -240.0), "multiple reverse detents");
	assert_zoom_near(0.0, pending, "negative detents consumed exactly");

	// The scroll answer is quantised to whole logical units by the same carry, so a device reporting
	// a single raw unit at a time scrolls rather than halving to nothing on every event.
	auto scroll = 0.0;
	assert_equal(0, ui::accumulate_wheel_steps(scroll, 1.0 / 2.0, 1.0), "half a unit is carried, not dropped");
	assert_equal(1, ui::accumulate_wheel_steps(scroll, 1.0 / 2.0, 1.0), "two halves make a unit");
	assert_equal(60, ui::accumulate_wheel_steps(scroll, 120.0 / 2.0, 1.0), "a whole detent scrolls its full amount");
}

static void should_validate_zoom_navigator_mode()
{
	assert_equal(true, is_valid_zoom_navigator_mode(static_cast<uint32_t>(zoom_navigator_mode::auto_hide)),
	             "auto-hide is valid");
	assert_equal(true, is_valid_zoom_navigator_mode(static_cast<uint32_t>(zoom_navigator_mode::pinned)),
	             "pinned is valid");
	assert_equal(true, is_valid_zoom_navigator_mode(static_cast<uint32_t>(zoom_navigator_mode::off)),
	             "off is valid");
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
	const pointd source_anchor{
		old_center.X * source.Width + (anchor.X - viewport.Width / 2.0) / fit,
		old_center.Y * source.Height + (anchor.Y - viewport.Height / 2.0) / fit
	};

	zoom.set_anchored(1.0, fit, source, viewport, anchor);
	const auto center = zoom.center();
	const pointd projected{
		viewport.Width / 2.0 + (source_anchor.X - center.X * source.Width),
		viewport.Height / 2.0 + (source_anchor.Y - center.Y * source.Height)
	};
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
	const pointd source_anchor{
		zoom.center().X * source.Width + (anchor.X - viewport.Width / 2.0) / fit,
		zoom.center().Y * source.Height + (anchor.Y - viewport.Height / 2.0) / fit
	};

	zoom.step(1, fit, source, viewport, anchor);
	const auto scale = zoom.effective_scale(fit);
	const auto center = zoom.center();
	const pointd projected{
		viewport.Width / 2.0 + (source_anchor.X - center.X * source.Width) * scale,
		viewport.Height / 2.0 + (source_anchor.Y - center.Y * source.Height) * scale
	};
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
	const pointd projected{
		new_origin.X + new_viewport.Width / 2.0 + (source_anchor.X - center.X * source.Width) * scale,
		new_origin.Y + new_viewport.Height / 2.0 + (source_anchor.Y - center.Y * source.Height) * scale
	};
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
	assert_equal(2000, df::round(u), "and lands on the middle column");
	assert_equal(1000, df::round(v), "and on the middle row");

	// Off centre it must not merely be "somewhere else": a pixel to the right of centre is east of
	// it, and one above is north. A symmetric check would pass for a mirrored projection too.
	double right_u = 0.0, right_v = 0.0;
	assert_equal(true, view.texel_at(700.0, 300.0, viewport, sphere, right_u, right_v), "so does one to its right");
	assert_equal(true, right_u > u, "which is further east");
	assert_equal(1000, df::round(right_v), "on the same row");

	double above_u = 0.0, above_v = 0.0;
	assert_equal(true, view.texel_at(400.0, 100.0, viewport, sphere, above_u, above_v), "and one above it");
	assert_equal(true, above_v < v, "which is further north");
	assert_equal(2000, df::round(above_u), "in the same column");

	// Crossing the viewport turns by exactly one field of view, which is what "grab the world and
	// drag it" means. Dragging right brings what was to the west into view, so the camera turns west.
	const auto start = view;
	view.drag({viewport.Width, 0.0}, viewport, start);
	assert_equal(df::round(-start.fov_degrees() * 1000.0), df::round(view.yaw() * 180.0 / M_PI * 1000.0),
	             "a drag of one viewport width turns by one field of view");
	assert_equal(0.0, view.pitch(), "and does not tilt");

	// The drag is recomputed from its origin, so re-applying the same total delta is idempotent -
	// a coalesced or dropped move message cannot change where it ends up.
	const auto after_first = view.yaw();
	view.drag({viewport.Width, 0.0}, viewport, start);
	assert_equal(df::round(after_first * 1e6), df::round(view.yaw() * 1e6), "and repeats to the same place");

	// A vertical drag turns by the same amount per pixel, and straight up and straight down are both
	// reachable without rolling past. Two viewport widths is 170 degrees of turn, so the clamp is
	// what stops it rather than the drag running out.
	view.drag({0.0, viewport.Width}, viewport, start);
	assert_equal(df::round(start.fov_degrees()), df::round(view.pitch() * 180.0 / M_PI),
	             "dragging down one width looks up by one field of view");
	view.drag({0.0, viewport.Width * 2.0}, viewport, start);
	assert_equal(90, df::round(view.pitch() * 180.0 / M_PI), "and further looks straight up, no more");
	view.drag({0.0, -viewport.Width * 2.0}, viewport, start);
	assert_equal(-90, df::round(view.pitch() * 180.0 / M_PI), "dragging up looks straight down, no more");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// zoom.md: inside a sphere there is no pixel to be one-to-one with, so the ladder is a field of
// view. Its wide end is the floor, the same shape as L2 stepping out to Fit.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_step_the_panorama_field_of_view_ladder()
{
	panorama_view view;
	assert_equal(df::round(panorama_default_fov_deg), df::round(view.fov_degrees()), "a new view opens at the default");

	assert_equal(true, view.step_fov(1), "stepping in is a step");
	assert_equal(70, df::round(view.fov_degrees()), "and narrows to the next stop");
	assert_equal(true, view.step_fov(-1), "stepping out is a step");
	assert_equal(85, df::round(view.fov_degrees()), "and returns to the one it came from");

	// n steps in and n steps out returns exactly, which is L4 in the ladder this replaces.
	for (auto i = 0; i < 4; ++i) view.step_fov(1);
	for (auto i = 0; i < 4; ++i) view.step_fov(-1);
	assert_equal(85, df::round(view.fov_degrees()), "four in and four out is where it started");

	// The narrow end holds rather than stepping past; the wide end reports that there is nowhere
	// wider, which is what makes the caller leave the projection instead of pretending.
	for (auto i = 0; i < 20; ++i) view.step_fov(1);
	assert_equal(df::round(panorama_min_fov_deg), df::round(view.fov_degrees()), "the narrow end holds");
	assert_equal(true, view.step_fov(1), "and reports a step it did not need to take");

	for (auto i = 0; i < 20; ++i) view.step_fov(-1);
	assert_equal(df::round(panorama_max_fov_deg), df::round(view.fov_degrees()), "the wide end is the floor");
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
	assert_equal(0, df::round(view.yaw() * 180.0 / M_PI), "the middle aims at the middle");
	assert_equal(0, df::round(view.pitch() * 180.0 / M_PI), "and level");

	view.aim({800.0, 300.0}, viewport, sphere);
	assert_equal(180, std::abs(df::round(view.yaw() * 180.0 / M_PI)), "the right edge aims at the far side");

	view.aim({0.0, 300.0}, viewport, sphere);
	assert_equal(180, std::abs(df::round(view.yaw() * 180.0 / M_PI)), "and so does the left, from the other way");

	// Asymmetric probes: a quarter across is a quarter round, east of centre and not west.
	view.aim({600.0, 300.0}, viewport, sphere);
	assert_equal(90, df::round(view.yaw() * 180.0 / M_PI), "a quarter to the right is a quarter turn east");
	view.aim({200.0, 300.0}, viewport, sphere);
	assert_equal(-90, df::round(view.yaw() * 180.0 / M_PI), "and a quarter to the left is a quarter west");

	view.aim({400.0, 0.0}, viewport, sphere);
	assert_equal(90, df::round(view.pitch() * 180.0 / M_PI), "the top aims straight up");
	view.aim({400.0, 600.0}, viewport, sphere);
	assert_equal(-90, df::round(view.pitch() * 180.0 / M_PI), "and the bottom straight down");

	// A partial panorama maps its own coverage, not the whole sphere: crossing the viewport must
	// reach the ends of what the file holds and no further, or most of the gesture would aim at
	// nothing.
	const auto strip = prop::panorama_geometry::assumed({4000, 400});
	view.aim({400.0, 0.0}, viewport, strip);
	assert_equal(18, df::round(view.pitch() * 180.0 / M_PI), "the top of a strip aims at the top of the strip");
	view.aim({400.0, 600.0}, viewport, strip);
	assert_equal(-18, df::round(view.pitch() * 180.0 / M_PI), "and the bottom at its bottom");

	// Off the edge is clamped rather than wrapped: a pointer dragged outside the element must not
	// spin the view round.
	view.aim({-500.0, -500.0}, viewport, sphere);
	assert_equal(180, std::abs(df::round(view.yaw() * 180.0 / M_PI)), "a pointer off the left clamps to the edge");
	assert_equal(90, df::round(view.pitch() * 180.0 / M_PI), "and off the top clamps to straight up");
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
	assert_equal(18, df::round(panorama_latitude_at_top(strip) * 180.0 / M_PI), "18 north at its top");
	assert_equal(36, df::round(panorama_latitude_span(strip) * 180.0 / M_PI), "36 degrees of sky in all");
	assert_equal(0, df::round(panorama_center_latitude(strip) * 180.0 / M_PI), "centred on the horizon");

	// A true 2:1 sphere is the identity, so the assumption costs nothing where it is not needed.
	const auto whole = prop::panorama_geometry::assumed({4000, 2000});
	assert_equal(2000, whole.full_height, "a 2:1 image is taken as the whole sphere");
	assert_equal(0, whole.cropped_top, "with nothing missing above it");

	// Sphere the file does not hold is not sampled: it reads as absence, not as the nearest pixel.
	double u = 0.0, v = 0.0;
	assert_equal(false, panorama_texel_at(strip, 0.0, 60.0 * M_PI / 180.0, u, v), "60 north is not in the strip");
	assert_equal(true, panorama_texel_at(strip, 0.0, 0.0, u, v), "the horizon is");
	assert_equal(200, df::round(v), "half way down it");

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
	const df::file_path path("c:\\same-path-panorama.jpg");
	const df::file_path other_path("c:\\other-panorama.jpg");
	const prop::panorama_geometry old_decl{4000, 2000, 0, 0, 4000, 2000};
	const prop::panorama_geometry stale_decl{4000, 2000, 100, 0, 2000, 1000};
	const prop::panorama_geometry new_decl{8000, 5000, 0, 500, 8000, 4000};
	const prop::panorama_geometry same_size_decl{8000, 5000, 0, 750, 8000, 4000};

	display.panorama_item(path, {4000, 2000}, df::date_t(1));
	display.panorama_geometry(path, old_decl, {4000, 2000}, df::date_t(1));
	assert_equal(true, common._panorama.resolved, "the first declaration resolves");

	const auto start = common._panorama.view;
	common._panorama.view.drag({400, 0}, {800, 600}, start);
	const auto yaw = common._panorama.view.yaw();
	common._panorama.flat = true;

	display.panorama_item(path, {8000, 4000}, df::date_t(2));
	assert_equal(false, common._panorama.resolved, "changed same-path dimensions need a new declaration");
	assert_equal(true, common._panorama.flat, "the user's flat choice is retained for the same path");
	assert_zoom_near(yaw, common._panorama.view.yaw(), "the camera is retained for the same path");

	display.panorama_geometry(path, stale_decl, {4000, 2000}, df::date_t(1));
	assert_equal(false, common._panorama.resolved, "a stale declaration for the old source cannot win");

	display.panorama_geometry(path, new_decl, {8000, 4000}, df::date_t(2));
	assert_equal(true, common._panorama.resolved, "the latest declaration resolves");
	assert_equal(new_decl.cropped_top, common._panorama.geometry.cropped_top, "the latest crop is applied");
	assert_equal(true, common._panorama.flat, "the declaration does not reset the flat choice");
	assert_zoom_near(yaw, common._panorama.view.yaw(), "the declaration does not reset the camera");

	display.panorama_item(path, {8000, 4000}, df::date_t(3));
	assert_equal(false, common._panorama.resolved, "same-size source revision needs a new declaration");
	display.panorama_geometry(path, same_size_decl, {8000, 4000}, df::date_t(3));
	assert_equal(same_size_decl.cropped_top, common._panorama.geometry.cropped_top,
	             "same-size declaration edits are applied");
	assert_zoom_near(yaw, common._panorama.view.yaw(), "same-path declaration edits still keep the camera");

	display.panorama_item(other_path, {8000, 4000}, df::date_t(4));
	common._panorama.view.drag({400, 0}, {800, 600}, common._panorama.view);
	display.panorama_geometry(other_path, same_size_decl, {8000, 4000}, df::date_t(4));
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
	constexpr sizei stored{4000, 3000};

	const auto mapped = [](const rectd normalised, const ui::orientation o, const bool was_rotated)
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
	assert_rect(mapped({0.25, 0.5, 0.25, 0.25}, ui::orientation::top_left, true),
	            1000, 1500, 1000, 750, "an unrotated capture is not moved");

	// With show_rotated off the picture is drawn in its stored orientation, so the fractions are
	// already fractions of the stored picture and only the corner order carries the rotation.
	assert_rect(mapped({0.25, 0.5, 0.25, 0.25}, ui::orientation::right_top, false),
	            1000, 1500, 1000, 750, "already stored space when the picture is not turned");

	// One asymmetric probe per orientation, mapped by hand from the EXIF definition. A symmetric
	// probe would pass under any orientation's formula, so it would pin nothing: the quarter chosen
	// here lands in a different corner of the stored picture for every one of the seven.
	// Displayed rect is the top-left quarter-ish block {0.0, 0.0, 0.25, 0.5} of the displayed extent.
	struct probe
	{
		ui::orientation orientation;
		double x, y, w, h;
	};

	// Non-flipping: displayed extent is 4000x3000, so the block is 1000x1500 at the displayed origin.
	constexpr probe upright_probes[]{
		{ui::orientation::top_right, 3000, 0, 1000, 1500},   // mirrored left to right
		{ui::orientation::bottom_right, 3000, 1500, 1000, 1500}, // turned half way round
		{ui::orientation::bottom_left, 0, 1500, 1000, 1500}, // mirrored top to bottom
	};

	for (const auto& p : upright_probes)
	{
		assert_rect(mapped({0.0, 0.0, 0.25, 0.5}, p.orientation, true), p.x, p.y, p.w, p.h,
		            "the displayed top-left block lands where the orientation says");
	}

	// Flipping: displayed extent is 3000x4000, so the block is 750x2000 and the axes swap on the way
	// back, giving a 2000x750 rectangle in stored space.
	constexpr probe turned_probes[]{
		{ui::orientation::left_top, 0, 0, 2000, 750},        // transposed about the main diagonal
		{ui::orientation::right_top, 0, 2250, 2000, 750},    // rotated a quarter turn
		{ui::orientation::right_bottom, 2000, 2250, 2000, 750}, // transposed about the anti-diagonal
		{ui::orientation::left_bottom, 2000, 0, 2000, 750},  // rotated a quarter turn the other way
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
	assert_zoom_near(1.0, df::zoom_view_state::look_around_center({800, 900}, wide, viewport, 1.0).Y,
	                 "and down");
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
	const auto rotated = selector_view::thumbnail_destination({4, 3}, image_bounds,
	                                                          ui::orientation::right_top, true);
	const auto unrotated = selector_view::thumbnail_destination({4, 3}, image_bounds,
	                                                            ui::orientation::right_top, false);

	assert_equal(90.0, rotated.angle(), "selector applies right-top orientation");
	assert_equal(true, rotated.bounding_rect().extent().round() == sizei(75, 100),
	             "oriented selector thumbnail fits as portrait");
	assert_equal(0.0, unrotated.angle(), "selector preserves orientation when rotation is disabled");
	assert_equal(true, unrotated.bounding_rect().extent().round() == sizei(100, 75),
	             "unrotated selector thumbnail fits as landscape");
}

static void should_range_select_across_selector()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(test_files_folder), {});
	s.update_item_groups();
	s.update_selection();

	const auto is_file = [](const df::item_element_ptr& i) { return !i->is_folder(); };

	df::item_elements shown;
	df::item_element_ptr hidden;

	for (const auto& group : s.groups())
	{
		for (const auto& i : group->items())
		{
			if (is_file(i)) shown.emplace_back(i);
			else hidden = i;
		}
	}

	assert_equal(true, shown.size() > 4, "test folder has enough items for a range");

	const auto selector = std::make_shared<selector_view>(s, nullptr, nullptr);
	selector->filter(is_file);
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
	constexpr auto max_band_entries = static_cast<size_t>(extent.cx * 2 / item_width + 4);

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
	s.open(view, df::search_t().add_selector(test_files_folder), {});
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
	constexpr sizei stored{528, 560}; // what the frame decodes to
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
	s.open(view, df::search_t().add_selector(test_files_folder), {});
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
	ui::control_layouts positions;

	const auto media = std::make_shared<flex_test_element>(sizei{200, 400});
	media->flex = media->flex | flex_item::media;

	const std::vector<view_element_ptr> priority{media, std::make_shared<flex_test_element>(sizei{200, 20})};
	const auto all = priority;

	constexpr recti pane{0, 0, 200, 600};

	const auto arrange = [&](const std::vector<view_element_ptr>& detail)
	{
		const media_column_inputs in{&priority, &detail, &all, pane, media};
		return layout_media_column(in, mc, positions);
	};

	// Enough detail to take the column past the pane, which is where a late row could have cost the
	// picture its height.
	const auto row = [] { return std::make_shared<flex_test_element>(sizei{200, 300}); };
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
	assert_equal(centred_bounds.top - 20, media->bounds.top, "it moves the composition by half the row");
}

// The media shrinks to keep the first information group visible - a portrait image that took the
// whole pane pushed that group off the bottom, where nothing said it was there - and it stops at the
// half-pane floor rather than being centred off the top of an overflowing column.
static void should_fit_the_whole_primary_block_in_the_media_column()
{
	flex_test_measure_context mc;
	ui::control_layouts positions;
	constexpr recti pane{0, 0, 200, 300};
	const std::vector<view_element_ptr> no_detail;

	const auto arrange = [&](const sizei media_extent, const view_element_ptr& toggle)
	{
		const auto media = std::make_shared<flex_test_element>(media_extent);
		media->flex = media->flex | flex_item::media;
		const std::vector<view_element_ptr> priority{media, std::make_shared<flex_test_element>(sizei{200, 20}), toggle};
		const media_column_inputs in{&priority, &no_detail, &priority, pane, media};
		layout_media_column(in, mc, positions);
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
	layout_media_column(cramped_in, mc, positions);

	assert_equal(true, squeezed->bounds.top >= short_pane.top, "an overflowing block is never clipped off the top");
}

// A description is placed under the block it belongs to rather than at the bottom edge of the pane.
// Anchoring it to the pane spent the free space the centring created on nothing and put the text a
// whole pane-height of scrolling away, with nothing on screen saying it was there - and the panel
// drops its own Description line precisely because the section below is supposed to carry it.
static void should_follow_a_centred_block_with_its_detail()
{
	flex_test_measure_context mc;
	ui::control_layouts positions;
	constexpr recti pane{0, 0, 200, 600};

	const auto media = std::make_shared<flex_test_element>(sizei{200, 200});
	media->flex = media->flex | flex_item::media;
	const auto group = std::make_shared<flex_test_element>(sizei{200, 40});
	const auto description = std::make_shared<flex_test_element>(sizei{200, 60});

	const std::vector<view_element_ptr> priority{media, group};
	const std::vector<view_element_ptr> detail{description};
	const std::vector<view_element_ptr> all{media, group, description};

	const media_column_inputs in{&priority, &detail, &all, pane, media};
	layout_media_column(in, mc, positions);

	assert_equal(group->bounds.bottom, description->bounds.top, "the detail follows the block it belongs to");
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
	ui::control_layouts positions;
	constexpr recti pane{0, 0, 200, 400};

	const auto media = std::make_shared<flex_test_element>(sizei{200, 400});
	media->flex = media->flex | flex_item::media;
	// More information than the whole pane can hold: a panel plus a stream table.
	const auto group = std::make_shared<flex_test_element>(sizei{200, 500});

	const std::vector<view_element_ptr> priority{media, group};
	const std::vector<view_element_ptr> no_detail;

	const media_column_inputs in{&priority, &no_detail, &priority, pane, media};
	const auto content_height = layout_media_column(in, mc, positions);

	assert_equal(pane.height() / 2, media->bounds.height(), "the media keeps half the pane");
	assert_equal(media->bounds.bottom, group->bounds.top, "the information follows it");
	assert_equal(true, content_height > pane.height(), "what does not fit scrolls rather than crushing the media");

	// A media pane that never wanted the half keeps its own height; the budget is a floor on shrinking,
	// never a reservation that pads a small picture out.
	const auto small = std::make_shared<flex_test_element>(sizei{200, 80});
	small->flex = small->flex | flex_item::media;
	const std::vector<view_element_ptr> small_priority{small, group};
	const media_column_inputs small_in{&small_priority, &no_detail, &small_priority, pane, small};
	layout_media_column(small_in, mc, positions);

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
	ui::control_layouts positions;
	constexpr recti pane{0, 0, 200, 300};

	const auto media = std::make_shared<flex_test_element>(sizei{200, 100});
	media->flex = media->flex | flex_item::media;
	const auto group = std::make_shared<flex_test_element>(sizei{200, 20});
	const auto verbose = std::make_shared<flex_test_element>(sizei{200, 50});

	const std::vector<view_element_ptr> priority{media, group};
	const std::vector<view_element_ptr> detail{verbose};
	const std::vector<view_element_ptr> all{media, group, verbose};

	const media_column_inputs in{&priority, &detail, &all, pane, media};
	const auto content_height = layout_media_column(in, mc, positions);

	const auto above = media->bounds.top - pane.top;
	const auto below = pane.bottom - verbose->bounds.bottom;

	assert_equal(true, above > 0, "a little metadata does not pin the picture to the top");
	assert_equal(true, std::abs(above - below) <= 1, "the column centres as one composition");
	assert_equal(group->bounds.bottom, verbose->bounds.top, "verbose metadata follows with no gap");
	assert_equal(true, content_height >= verbose->bounds.bottom - pane.top, "the scroll extent reaches it");

	// More metadata than the pane can hold: there is nothing to centre in, so the column starts at the
	// top and the rest scrolls.
	const auto long_verbose = std::make_shared<flex_test_element>(sizei{200, 400});
	const std::vector<view_element_ptr> long_detail{long_verbose};
	const std::vector<view_element_ptr> long_all{media, group, long_verbose};
	const media_column_inputs long_in{&priority, &long_detail, &long_all, pane, media};
	const auto long_height = layout_media_column(long_in, mc, positions);

	assert_equal(pane.top, media->bounds.top, "an overflowing column is aligned to the top");
	assert_equal(group->bounds.bottom, long_verbose->bounds.top, "and still follows with no gap");
	assert_equal(true, long_height > pane.height(), "what does not fit scrolls");
}

// A column that is all primary content and has no detail form at all - every multiple selection, and
// a comparison - centres by the same rule as one that has.
static void should_centre_a_media_column_with_no_detail()
{
	flex_test_measure_context mc;
	ui::control_layouts positions;
	constexpr recti pane{0, 0, 200, 300};

	const auto collage = std::make_shared<flex_test_element>(sizei{200, 100});
	const auto controls = std::make_shared<flex_test_element>(sizei{200, 40});

	const std::vector<view_element_ptr> priority{collage, controls};
	const std::vector<view_element_ptr> no_detail;

	const media_column_inputs in{&priority, &no_detail, &priority, pane, nullptr};
	layout_media_column(in, mc, positions);

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

static void should_reserve_view_scroller_footer()
{
	view_scroller scroller;
	constexpr recti client_bounds(0, 0, 100, 200);
	constexpr recti full_scroll_bounds(90, 0, 100, 200);
	const auto footer = scroller.layout_with_footer(
		{100, 1000}, client_bounds, full_scroll_bounds, 10, 2);

	assert_equal(true, footer == recti(90, 190, 100, 200), "scroll footer occupies requested base");
	assert_equal(true, scroller.scroll_bounds() == recti(90, 0, 100, 188),
	             "scroll track ends before footer");

	// The section bands are placed with logical_to_scrollbar_pos, so it must land on the track the
	// thumb actually travels rather than on the client height the footer no longer covers.
	const auto track = scroller.track_bounds();
	assert_equal(true, track == recti(90, 2, 100, 186), "scroll track excludes its own edge inset");
	assert_equal(track.height(), scroller.logical_to_scrollbar_pos(1000), "list end maps to track end");
	assert_equal(1000, scroller.scrollbar_pos_to_logical(track.bottom), "track end maps to list end");
	assert_equal(0, scroller.scrollbar_pos_to_logical(track.top), "track start maps to list start");
}

class scroller_test_host final : public std::enable_shared_from_this<scroller_test_host>, public view_host
{
public:
	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::control_frame_ptr owner() override { return nullptr; }

	void on_window_layout(ui::measure_context& mc, const sizei extent, bool is_minimized) override {}
	void on_window_paint(ui::draw_context& dc) override {}
	void tick() override {}
	void activate(bool is_active) override {}
	bool key_down(const int c, const ui::key_state keys) override { return false; }
	void invoke(const commands cmd) override {}
	bool is_command_checked(const commands cmd) override { return false; }
	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& e) override {}
	void invalidate_view(const view_invalid invalid) override {}
	view_controller_ptr controller_from_location(pointi loc, hit_test_context& ctx) override { return nullptr; }
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

	// The whole point: no window, ever.
	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::control_frame_ptr owner() override { return nullptr; }

	void on_window_layout(ui::measure_context& mc, const sizei extent, bool is_minimized) override {}
	void on_window_paint(ui::draw_context& dc) override {}
	void tick() override {}
	void activate(bool is_active) override {}
	bool key_down(const int c, const ui::key_state keys) override { return false; }
	void invoke(const commands cmd) override {}
	bool is_command_checked(const commands cmd) override { return false; }
	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override {}
	void controller_changed() override { ++controller_changes; }
	void invalidate_element(const view_element_ptr& e) override { ++invalidations; }
	void invalidate_view(const view_invalid invalid) override { ++invalidations; }

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
	{
		++controller_requests;
		return nullptr;
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

	cancellable_test_controller(view_host_ptr host, const recti bounds) : view_controller(std::move(host), bounds)
	{
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		view_controller::on_mouse_left_button_down(loc, keys);
		tracking = true;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
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
	const ui::control_frame_ptr owner() override { return nullptr; }

	void on_window_layout(ui::measure_context& mc, const sizei extent, bool is_minimized) override {}
	void on_window_paint(ui::draw_context& dc) override {}
	void tick() override {}
	void activate(bool is_active) override {}
	bool key_down(const int c, const ui::key_state keys) override { return false; }
	void invoke(const commands cmd) override {}
	bool is_command_checked(const commands cmd) override { return false; }
	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& e) override {}
	void invalidate_view(const view_invalid invalid) override {}

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
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

	idle_escape_test_controller(view_host_ptr host, const recti bounds) : view_controller(std::move(host), bounds)
	{
	}

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
	const ui::control_frame_ptr owner() override { return nullptr; }

	void on_window_layout(ui::measure_context& mc, const sizei extent, bool is_minimized) override {}
	void on_window_paint(ui::draw_context& dc) override {}
	void tick() override {}
	void activate(bool is_active) override {}
	bool key_down(const int c, const ui::key_state keys) override { return false; }
	void invoke(const commands cmd) override {}
	bool is_command_checked(const commands cmd) override { return false; }
	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& e) override {}
	void invalidate_view(const view_invalid invalid) override {}

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
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

// Answers one controller over a fixed region, having first recorded an occluder, so what the host
// caches can be told apart from what the controller itself claims.
class stability_test_host final : public std::enable_shared_from_this<stability_test_host>, public view_host
{
public:
	recti answer_bounds{0, 0, 200, 400};
	recti occluder;
	int builds = 0;

	const ui::frame_ptr frame() const override { return ui::no_frame(); }
	const ui::control_frame_ptr owner() override { return nullptr; }

	void on_window_layout(ui::measure_context& mc, const sizei extent, bool is_minimized) override {}
	void on_window_paint(ui::draw_context& dc) override {}
	void tick() override {}
	void activate(bool is_active) override {}
	bool key_down(const int c, const ui::key_state keys) override { return false; }
	void invoke(const commands cmd) override {}
	bool is_command_checked(const commands cmd) override { return false; }
	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override {}
	void controller_changed() override {}
	void invalidate_element(const view_element_ptr& e) override {}
	void invalidate_view(const view_invalid invalid) override {}

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
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
class invoke_counting_element final : public std::enable_shared_from_this<invoke_counting_element>,
                                      public view_element
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
class list_test_measure_context final : public ui::measure_context
{
public:
	static constexpr int line_height = 20;

	sizei measure_text(const std::string_view text, ui::style::font_face font, ui::style::text_style style,
	                   const int cx, int cy = 0) override
	{
		return {std::min(cx, static_cast<int>(text.size()) * 8), line_height};
	}

	int text_line_height(ui::style::font_face font) override { return line_height; }
	ui::text_layout_ptr create_text_layout(ui::style::font_face font) override { return {}; }
};

class single_line_test_layout final : public ui::text_layout
{
public:
	std::string text;
	int* updates = nullptr;

	explicit single_line_test_layout(int& update_count) : updates(&update_count)
	{
	}

	void update(const std::string_view text_in, ui::style::text_style style) override
	{
		text = text_in;
		++*updates;
	}

	sizei measure_text(int cx, int cy) override
	{
		return {std::min(cx, static_cast<int>(text.size()) * 8), 20};
	}

	std::vector<int> offset_xs(const std::vector<size_t>& utf8_offsets, int cx, int cy) override
	{
		std::vector<int> result;
		result.reserve(utf8_offsets.size());
		for (const auto offset : utf8_offsets) result.emplace_back(static_cast<int>(offset) * 8);
		return result;
	}
};

class edit_test_measure_context final : public ui::measure_context
{
public:
	int layout_updates = 0;
	int prefix_measures = 0;

	sizei measure_text(const std::string_view text, ui::style::font_face font, ui::style::text_style style,
	                   const int cx, int cy = 0) override
	{
		++prefix_measures;
		return {std::min(cx, static_cast<int>(text.size()) * 8), 20};
	}

	int text_line_height(ui::style::font_face font) override { return 20; }

	ui::text_layout_ptr create_text_layout(ui::style::font_face font) override
	{
		return std::make_shared<single_line_test_layout>(layout_updates);
	}
};

class forwarding_test_controller final : public view_controller
{
public:
	forwarding_test_controller(view_host_ptr host, const recti bounds) : view_controller(std::move(host), bounds)
	{
	}
};

class forwarding_child_element final : public view_element
{
public:
	int control_area_requests = 0;
	int controller_requests = 0;

	forwarding_child_element() : view_element(view_element_style::can_invoke)
	{
		bounds = {10, 20, 70, 40};
	}

	bool is_control_area(const pointi loc, const pointi element_offset) const override
	{
		++const_cast<forwarding_child_element*>(this)->control_area_requests;
		return is_visible() && bounds.offset(element_offset).contains(loc);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		++controller_requests;
		const auto device_bounds = bounds.offset(element_offset);
		return device_bounds.contains(loc) ? std::make_shared<forwarding_test_controller>(host, device_bounds) : nullptr;
	}
};

static ui::surface_ptr make_test_surface(const sizei dimensions, const ui::orientation orientation = ui::orientation::top_left)
{
	auto surface = std::make_shared<ui::surface>();
	surface->alloc(dimensions, ui::texture_format::ARGB, orientation);
	surface->make_blank();
	return surface;
}

static ui::const_image_ptr make_valid_test_image()
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
	assert_equal_strict(std::string(tt.image_display_failed.sv()), std::string(edit_load_status_text(unreadable, false)),
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
	assert_equal(false, should_accept_edit_analysis(9, 9, 3, 2),
	             "analysis captured before a crop gesture is rejected");
}

// The preview decode treats the size Edit asks for as a ceiling. It can return a pixel short of it,
// or the native size of a picture smaller than the view. Judging the preview by its own extent threw
// those away on every repaint and left Edit on "Loading" for good: under that rule this test fails at
// 101 x 300, where Test.jpg is asked for 101 x 67 and decodes at 100 x 67, and at 1100 x 800, where
// it is asked for 1100 x 734 and is not enlarged.
static void should_draw_edit_preview_decoded_short_of_its_request()
{
	files ff;
	const auto loaded = ff.load(test_files_folder.combine_file("Test.jpg"), false);
	assert_equal(true, edit_load_has_source_pixels(loaded), "the fixture loads");

	ui::const_surface_ptr last_preview;
	sizei last_request;

	for (const auto extent : {sizei(101, 300), sizei(1100, 800), sizei(512, 512)})
	{
		const auto label = std::format("{} x {}", extent.cx, extent.cy);
		const auto request = ui::scale_dimensions(loaded.dimensions(), extent);
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
	const auto png = df::blob_from_file(test_files_folder.combine_file("engine.png"));
	file_load_result truncated;
	truncated.i = load_image_file({png.data(), png.size() / 2});
	truncated.success = is_valid(truncated.i);
	assert_equal(true, edit_load_has_source_pixels(truncated), "a header-only load is offered as editable");

	const auto request = ui::scale_dimensions(truncated.dimensions(), sizei(640, 480));
	const auto unreadable = edit_preview_failure(truncated.to_surface(request), truncated, request);
	assert_equal(true, unreadable == file_load_result::failure::unreadable, "an undecodable preview is unreadable");

	const auto failed = edit_load_without_preview(truncated, unreadable);
	assert_equal(false, edit_load_has_source_pixels(failed), "an undecodable file has no editable pixels");
	assert_equal_strict(std::string(tt.image_display_failed.sv()), std::string(edit_load_status_text(failed, false)),
	                    "an undecodable file says it failed rather than loading");

	files ff;
	const auto loaded = ff.load(test_files_folder.combine_file("Test.jpg"), false);
	const auto fitted = ui::scale_dimensions(loaded.dimensions(), sizei(640, 480));
	assert_equal(true, edit_preview_failure(loaded.to_surface(fitted), loaded, fitted) ==
	             file_load_result::failure::none, "a decoded preview is not a failure");

	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });
	df::max_decode_bytes = 64 * 1024;

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
	assert_equal(50, df::round(bounds.left()), "pending crop maps left to source pixels");
	assert_equal(25, df::round(bounds.top()), "pending crop maps top to source pixels");
	assert_equal(150, df::round(bounds.right()), "pending crop maps right to source pixels");
	assert_equal(75, df::round(bounds.bottom()), "pending crop maps bottom to source pixels");

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

static std::shared_ptr<display_state_t> make_zoom_pan_display(async_strategy& async,
                                                              common_display_state_t& common)
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
	assert_zoom_near(fitted_center.X, items._display->zoom_state().center().X,
	                 "fitted media ignores touch pan x");
	assert_zoom_near(fitted_center.Y, items._display->zoom_state().center().Y,
	                 "fitted media ignores touch pan y");
}

static ui::const_image_ptr make_corrupt_test_image()
{
	return std::make_shared<ui::image>(df::blob{0xff, 0xd8, 0xff, 0xd9}, sizei{32, 32},
	                                   ui::image_format::JPEG, ui::orientation::top_left);
}

static df::item_element_ptr make_thumbnail_test_item(const std::string& name,
                                                     const ui::const_image_ptr& thumbnail)
{
	df::index_file_item indexed;
	indexed.name = str::cache(name);
	indexed.ft = files::file_type_from_name(indexed.name);

	const auto item = std::make_shared<df::item_element>(df::file_path("c:\\" + name), indexed);
	item->thumbnail(thumbnail, {}, {});
	return item;
}

static void should_refresh_tooltip_when_staged_thumbnail_lands()
{
	deferred_async_strategy as;
	const auto item = make_thumbnail_test_item("tooltip.jpg", make_valid_test_image());

	item->stage_thumbnail_surface(as, false, true);
	item->stage_thumbnail_surface(as, false, true);
	assert_equal(1_z, as.pending_worker_count(async_queue::render), "tooltip staging queues one decode");

	assert_equal(true, as.run_next(async_queue::render), "decode runs");
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
	assert_equal(true, stage.run_next(async_queue::render), "cover-art decode runs");
	stage.drain_ui();
	assert_equal(true, item->has_cached_surface(), "cover art can cache even when the thumbnail failed");
	assert_equal(false, ui::is_valid(item->thumbnail_surface()), "thumbnail surface is still absent");

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
	assert_equal(true, as.run_next(async_queue::render), "failed decode runs");
	as.drain_ui();

	group.scroll_tooltip(next, elements, as);
	assert_equal(1_z, as.pending_worker_count(async_queue::render),
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

	assert_equal(1_z, as.pending_worker_count(async_queue::render),
	             "one hover queues at most one fallback thumbnail stage");
}

static void should_publish_async_selection_thumbnail_strip()
{
	deferred_async_strategy as;
	auto strip = std::make_shared<ui::selection_thumbnails_control>(ui::no_control_frame());
	const std::vector<ui::const_image_ptr> images{make_valid_test_image(), make_valid_test_image()};

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
	auto strip = std::make_shared<ui::selection_thumbnails_control>(ui::no_control_frame());
	const std::vector<ui::const_image_ptr> images{make_valid_test_image()};

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
	auto frame = std::make_shared<ui::null_control_frame>();
	const std::weak_ptr<ui::null_control_frame> weak_frame = frame;
	const auto strip = std::make_shared<ui::selection_thumbnails_control>(frame);
	const std::vector<ui::const_image_ptr> images{make_valid_test_image()};

	strip->selection_async(images, images.size(), as);
	frame.reset();
	assert_equal(true, weak_frame.expired(), "the strip does not own the frame it is shown in");

	assert_equal(true, as.run_next(async_queue::render), "strip decode runs");
	as.drain_ui();
	assert_equal(true, strip->is_visible(), "a strip whose frame has gone still publishes its thumbnails");
}

static void should_preview_rotation_with_inverse_destination_transform()
{
	assert_equal(static_cast<int>(simple_transform::rot_270),
	             static_cast<int>(ui::before_after_control::preview_destination_transform(simple_transform::rot_90)),
	             "clockwise source edit previews with the inverse destination transform");
	assert_equal(static_cast<int>(simple_transform::rot_90),
	             static_cast<int>(ui::before_after_control::preview_destination_transform(simple_transform::rot_270)),
	             "anticlockwise source edit previews with the inverse destination transform");
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
	void initialise(std::function<void(const ui::auto_complete_results&)> complete) override
	{
		complete({});
	}
};

class null_list_frame_control_frame final : public ui::control_frame
{
public:
	std::any handle() const override { return {}; }
	void destroy() override {}
	void enable(bool) override {}
	std::string window_text() const override { return {}; }
	void window_text(std::string_view) override {}
	void focus() override {}
	sizei measure(int) const override { return {}; }
	bool is_visible() const override { return false; }
	bool has_focus() const override { return false; }
	recti window_bounds() const override { return {}; }
	void window_bounds(recti, bool) override {}
	void show(bool) override {}
	void options_changed() override {}
	void invalidate(recti = {}, bool = false) override {}
	void layout() override {}
	void redraw() override {}
	void redraw_now() override {}
	void scroll(int, int, recti, bool) override {}
	void track_menu(recti, const std::vector<ui::command_ptr>&) override {}
	void close(bool = false) override {}
	bool is_enabled() const override { return false; }
	bool is_maximized() const override { return false; }
	bool is_occluded() const override { return true; }
	void set_cursor(ui::style::cursor) override {}
	void reset_graphics() override {}
	pointi cursor_location() override { return {-1, -1}; }

	ui::edit_ptr create_edit(const ui::edit_styles& styles, std::string_view text,
	                         std::function<void(const std::string&)> changed) override
	{
		return ui::no_control_frame()->create_edit(styles, text, std::move(changed));
	}

	ui::trackbar_ptr create_slider(int min, int max, std::function<void(int, bool)> changed) override
	{
		return ui::no_control_frame()->create_slider(min, max, std::move(changed));
	}

	ui::toolbar_ptr create_toolbar(const ui::toolbar_styles& styles,
	                               const std::vector<ui::command_ptr>& buttons) override
	{
		return ui::no_control_frame()->create_toolbar(styles, buttons);
	}

	ui::button_ptr create_button(std::string_view text, std::function<void()> invoke, bool default_button) override
	{
		return ui::no_control_frame()->create_button(text, std::move(invoke), default_button);
	}

	ui::button_ptr create_button(icon_index icon, std::string_view title, std::string_view details,
	                             std::function<void()> invoke, bool default_button) override
	{
		return ui::no_control_frame()->create_button(icon, title, details, std::move(invoke), default_button);
	}

	ui::button_ptr create_check_button(bool val, std::string_view text, bool is_radio,
	                                  std::function<void(bool)> changed, int radio_group) override
	{
		return ui::no_control_frame()->create_check_button(val, text, is_radio, std::move(changed), radio_group);
	}

	ui::date_time_control_ptr create_date_time_control(df::date_t text, std::function<void(df::date_t)> changed,
	                                                   bool include_time) override
	{
		return ui::no_control_frame()->create_date_time_control(text, std::move(changed), include_time);
	}

	ui::control_frame_ptr create_dlg(ui::frame_host_weak_ptr host, bool is_popup) override
	{
		return ui::no_control_frame()->create_dlg(std::move(host), is_popup);
	}

	ui::frame_ptr create_frame(ui::frame_host_weak_ptr, const ui::frame_style&) override { return nullptr; }
	ui::bubble_window_ptr create_bubble() override { return ui::no_control_frame()->create_bubble(); }
	void apply_layout(const ui::control_layouts&, pointi) override {}
	ui::close_result wait_for_close(uint32_t) override { return ui::close_result::cancel; }
	void focus_first() override {}
	void position(recti) override {}
	void save_window_position(platform::setting_file_ptr&) override {}
	bool is_canceled() const override { return true; }
	double scale_factor() const override { return 1.0; }
};

class slot_test_texture final : public ui::texture
{
public:
	int source_index = -1;
	bool fail_update = false;

	slot_test_texture(const int source_index_in, const bool fail_update_in) :
		source_index(source_index_in), fail_update(fail_update_in)
	{
	}

	ui::texture_update_result update(const ui::video_frame_ptr&) override { return ui::texture_update_result::failed; }

	ui::texture_update_result update(const ui::const_surface_ptr& surface) override
	{
		if (fail_update) return ui::texture_update_result::failed;
		_dimensions = surface->dimensions();
		_src_extent = surface->dimensions();
		_orientation = surface->orientation();
		_format = surface->format();
		return ui::texture_update_result::tex_created;
	}

	ui::texture_update_result update(sizei dims, ui::texture_format format, ui::orientation orientation,
	                                 const uint8_t*, size_t, size_t) override
	{
		if (fail_update) return ui::texture_update_result::failed;
		_dimensions = dims;
		_src_extent = dims;
		_orientation = orientation;
		_format = format;
		return ui::texture_update_result::tex_created;
	}

	bool is_valid() const override { return !fail_update; }
};

class slot_test_draw_context final : public ui::draw_context
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
	int created = 0;

	explicit slot_test_draw_context(std::vector<bool> fail_uploads_in) : fail_uploads(std::move(fail_uploads_in))
	{
		colors.alpha = 1.0f;
	}

	void clear(ui::color) override {}
	void draw_rounded_rect(recti, ui::color, int) override {}
	void draw_rect(recti, ui::color) override {}
	void draw_text(std::string_view, recti, ui::style::font_face, ui::style::text_style, ui::color, ui::color) override {}
	void draw_text(std::string_view, const std::vector<ui::text_highlight_t>&, recti, ui::style::font_face,
	               ui::style::text_style, ui::color, ui::color) override {}
	void draw_text(const ui::text_layout_ptr&, recti, ui::color, ui::color) override {}
	void draw_border(recti, recti, ui::color, ui::color) override {}
	void draw_texture(const ui::texture_ptr& t, recti dst, float, ui::texture_sampler) override
	{
		const auto texture = std::dynamic_pointer_cast<slot_test_texture>(t);
		draws.push_back({texture ? texture->source_index : -1, dst, recti({}, t->dimensions())});
	}
	void draw_texture(const ui::texture_ptr& t, recti dst, recti src, float, ui::texture_sampler, float) override
	{
		const auto texture = std::dynamic_pointer_cast<slot_test_texture>(t);
		draws.push_back({texture ? texture->source_index : -1, dst, src});
	}
	void draw_texture(const ui::texture_ptr& t, const quadd& dst, recti src, float, ui::texture_sampler) override
	{
		const auto texture = std::dynamic_pointer_cast<slot_test_texture>(t);
		draws.push_back({texture ? texture->source_index : -1, dst.bounding_rect_i(), src});
	}
	void draw_texture(const ui::texture_ptr& t, const quadd& dst, recti src, float, ui::texture_sampler,
	                  const ui::texture_transform&) override
	{
		const auto texture = std::dynamic_pointer_cast<slot_test_texture>(t);
		draws.push_back({texture ? texture->source_index : -1, dst.bounding_rect_i(), src});
	}
	void draw_vertices(const ui::vertices_ptr&) override {}
	sizei measure_text(std::string_view text, ui::style::font_face, ui::style::text_style, int width,
	                   int = 0) override
	{
		return {std::min(width, static_cast<int>(text.size()) * 8), 20};
	}
	int text_line_height(ui::style::font_face) override { return 20; }
	ui::texture_ptr create_texture() override
	{
		const auto index = created++;
		const auto fail = static_cast<size_t>(index) < fail_uploads.size() && fail_uploads[index];
		return std::make_shared<slot_test_texture>(index, fail);
	}
	ui::vertices_ptr create_vertices() override { return {}; }
	ui::text_layout_ptr create_text_layout(ui::style::font_face) override { return {}; }
	recti clip_bounds() const override { return {}; }
	void clip_bounds(recti) override {}
	void restore_clip() override {}
};


class processing_test_view final : public list_view
{
public:	std::string _text;

	processing_test_view(view_state& state, view_host_ptr host) : list_view(state, std::move(host))
	{
	}

	std::string_view status() override { return _text; }

	void refresh() override
	{
	}

	void add_rows(const int count)
	{
		for (auto i = 0; i < count; ++i)
		{
			auto row = std::make_shared<row_element>(*this);
			row->_order = i;
			row->_work_index = i;
			_rows.emplace_back(std::move(row));
		}
	}

	int active_device_top() const { return _active_row->bounds.top - _scroller.scroll_offset().y; }
	int active_device_bottom() const { return _active_row->bounds.bottom - _scroller.scroll_offset().y; }
};

// A run keeps the row it is working on visible, and visible means clear of the chrome painted over
// the list rather than merely inside the scrolling area: the column headers are drawn across the top
// of it and view_frame::draw_view_status paints the status band across its base. Scrolling the row to
// the very edge left the highlight under one or the other for the whole of a long run.
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

	// Far enough down that the list has to scroll to reach it, which is where the row used to come to
	// rest against the bottom edge with the status band drawn over it.
	view->processing_work_item(60);

	assert_equal(true, view->active_device_bottom() <= extent.cy - band - margin,
	             "the row stops clear of the status band");
	assert_equal(true, view->active_device_top() >= 0, "and is still inside the view");

	// Back up the list: the same rule applies to the column headers above it.
	view->processing_work_item(5);

	assert_equal(true, view->active_device_top() >= band + margin,
	             "the row stops clear of the column headers");
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
	const df::cancel_token first(*first_source);

	view->begin_processing(1);
	const auto second_source = view->processing_cancel_source();
	const df::cancel_token second(*second_source);

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
	const auto panel = view->controls(ui::no_control_frame());
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
	const auto root = _temps.next_folder("refresh-during-sync");
	const auto local = root.combine("local");
	const auto remote = root.combine("remote");
	platform::create_folder(local);
	platform::create_folder(remote);
	write_test_file(local.combine_file("copy.txt"), "copy");

	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	df::index_roots roots;
	roots.folders.emplace(local);
	index.index_roots(roots);
	view_state s(ss, as, index, make_test_player());

	const auto saved = setting.sync;
	const df::scope_exit restore([saved] { setting.sync = saved; });
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

	assert_equal(true, remote.combine_file("copy.txt").exists(), "Refresh did not stop the copy");
	assert_equal(false, view->progress().active, "the run reports its end");
}

static void should_answer_a_null_frame_without_side_effects()
{
	const auto f = ui::no_frame();
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
	auto checkbox = std::make_shared<ui::check_control>(ui::no_control_frame(), "Rating", checked);
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

static void should_reject_failed_dialog_thumbnail_decodes()
{
	const auto valid_image = make_valid_test_image();
	assert_equal(true, ui::is_valid(valid_image), "test fixture has a valid encoded image");

	const auto corrupt = std::make_shared<ui::image>(df::blob{0xff, 0xd8, 0xff, 0xd9}, sizei{32, 32},
	                                                 ui::image_format::JPEG, ui::orientation::top_left);
	std::vector<ui::const_image_ptr> images;
	images.emplace_back(corrupt);
	for (auto i = 0; i < 7; ++i) images.emplace_back(valid_image);

	ui::title_control2 title(ui::no_control_frame(), icon_index::none, "Title", "Selection", images, images.size());
	const auto dims = title.surface_dims();
	assert_equal(8u, static_cast<unsigned>(dims.size()), "failed decode did not consume a thumbnail slot");

	list_test_measure_context mc;
	ui::selection_thumbnails_control strip(ui::no_control_frame());
	std::vector<ui::const_surface_ptr> surfaces;
	for (auto i = 0; i < 7; ++i) surfaces.emplace_back(make_test_surface({16, 12}));
	strip.selection(surfaces, images.size());
	const auto measured = strip.measure(mc, 700);
	assert_equal(true, measured.cy > 0, "staged surfaces measure the strip");

	ui::selection_thumbnails_control failed_strip(ui::no_control_frame());
	failed_strip.selection(std::vector<ui::const_surface_ptr>{}, 2);
	assert_equal(false, failed_strip.is_visible(), "thumbnail strips without staged surfaces stay hidden");
}

static void should_use_non_null_search_list_frame()
{
	auto value = std::string{};
	auto completes = std::make_shared<absent_frame_complete_strategy>();
	const auto frame = std::make_shared<null_list_frame_control_frame>();
	ui::search_control search(frame, value, completes);

	auto visited = 0;
	search.visit_controls([&](const ui::control_base_ptr& control)
	{
		++visited;
		assert_equal(true, control != nullptr, "visitor receives a non-null control");
	});
	assert_equal(2, visited, "edit and list frame are visited");

	list_test_measure_context mc;
	ui::control_layouts positions;
	search.measure(mc, 200);
	search.layout(mc, recti(0, 0, 200, 200), positions);
	assert_equal(2u, static_cast<unsigned>(positions.size()), "edit and list frame are laid out");
	assert_equal(true, positions[1].control != nullptr, "layout receives the non-null list frame accessor");
	assert_equal(true, positions[1].control == ui::no_frame(), "absent native list frame uses no_frame");
}

static void should_preserve_thumbnail_texture_slots_after_upload_failure()
{
	const std::vector<ui::const_surface_ptr> surfaces{
		make_test_surface({80, 40}, ui::orientation::top_left),
		make_test_surface({30, 90}, ui::orientation::top_left)
	};
	ui::title_control2 title(ui::no_control_frame(), icon_index::none, "Title", "Selection", surfaces);
	list_test_measure_context mc;
	ui::control_layouts positions;
	title.measure(mc, 320);
	title.layout(mc, recti(0, 0, 320, 100), positions);

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
	check({0, 0, 1600, 900}, {
		      {1200, 200}, {180, 900}, {400, 400}, {300, 800}, {1400, 350}, {640, 480},
		      {200, 1000}, {1600, 300}, {500, 500}, {900, 1600}, {1000, 250}, {300, 300}
	      }, 12);
	check({0, 0, 1920, 1080}, std::vector<sizei>(24, sizei(320, 240)), 24);
	check({0, 0, 1920, 1080}, std::vector<sizei>(30, sizei(320, 240)), 24);

	const auto aesthetic_bounds = ui::layout_collage({0, 0, 1200, 800}, std::vector<sizei>(9, sizei(256, 256)));
	const auto first_row_top = aesthetic_bounds.front().top;
	assert_equal(true, std::any_of(aesthetic_bounds.begin(), aesthetic_bounds.end(),
	                               [first_row_top](const recti& bounds) { return bounds.top != first_row_top; }),
	             "collage uses multiple rows");
	const auto [smallest, largest] = std::minmax_element(aesthetic_bounds.begin(), aesthetic_bounds.end(),
	                                                     [](const recti& left, const recti& right)
	                                                     {
		                                                     return left.area() < right.area();
	                                                     });
	assert_equal(true, largest->area() > smallest->area() * 3 / 2, "collage varies cell sizes");
	assert_equal(800, std::max_element(aesthetic_bounds.begin(), aesthetic_bounds.end(),
	                                   [](const recti& left, const recti& right)
	                                   {
		                                   return left.bottom < right.bottom;
	                                   })->bottom,
	             "collage fills available height");

	const auto portrait_bounds = ui::layout_collage({0, 0, 600, 1200}, std::vector<sizei>(9, sizei(256, 256)));
	assert_equal(1200, std::max_element(portrait_bounds.begin(), portrait_bounds.end(),
	                                    [](const recti& left, const recti& right)
	                                    {
		                                    return left.bottom < right.bottom;
	                                    })->bottom,
	             "portrait collage fills available height");
	assert_equal(true, std::any_of(portrait_bounds.begin(), portrait_bounds.end(),
	                               [](const recti& bounds) { return bounds.height() > bounds.width(); }),
	             "portrait collage has vertical cells");
}

static void should_edit_single_line_text()
{
	ui::single_line_edit_model edit;
	edit.text("Hello world");
	edit.select(6, 11);
	edit.insert("Diffractor");
	assert_equal_strict("Hello Diffractor", edit.text(), "replace selection");

	edit.move_left(true);
	assert_equal(true, edit.has_selection(), "shift-left selects");
	edit.move_left();
	assert_equal(false, edit.has_selection(), "left collapses selection");
	assert_equal(15_z, edit.caret(), "left collapses to selection start");

	edit.text("A\u00e9B");
	edit.move_left();
	edit.backspace();
	assert_equal_strict("AB", edit.text(), "backspace removes one UTF-8 code point");
	assert_equal(1_z, edit.caret(), "caret remains on UTF-8 boundary");

	edit.select_all();
	edit.insert("one\r\ntwo\nthree");
	assert_equal_strict("one two three", edit.text(), "paste is normalized to one line");

	edit.text("keep selection");
	edit.select(5, 14);
	assert_equal(false, edit.paste_from_clipboard([]() -> std::optional<std::string> { return std::nullopt; }),
	             "unavailable clipboard text is not pasted");
	assert_equal_strict("keep selection", edit.text(), "unavailable paste preserves text");
	assert_equal(true, edit.has_selection(), "unavailable paste preserves selection");
	assert_equal(true, edit.paste_from_clipboard([] { return std::optional<std::string>{""}; }),
	             "valid empty clipboard text is pasteable");
	assert_equal_strict("keep ", edit.text(), "valid empty paste erases selection");
	edit.undo();
	assert_equal_strict("keep selection", edit.text(), "empty paste remains undoable");

	edit.select(5, 14);
	assert_equal(false, edit.cut_to_clipboard([](std::string_view) { return false; }),
	             "failed cut publication is reported");
	assert_equal_strict("keep selection", edit.text(), "failed cut preserves text");
	assert_equal(true, edit.has_selection(), "failed cut preserves selection");
	std::string cut_text;
	assert_equal(true, edit.cut_to_clipboard([&cut_text](const std::string_view value)
	{
		cut_text = value;
		return true;
	}), "successful cut is reported");
	assert_equal_strict("selection", cut_text, "cut publishes selection before erasing");
	assert_equal_strict("keep ", edit.text(), "successful cut erases selection");

	edit.text("one two three");
	edit.move_word_left();
	assert_equal(8_z, edit.caret(), "control-left moves to previous word");
	edit.backspace_word();
	assert_equal_strict("one three", edit.text(), "control-backspace removes previous word");
	edit.undo();
	assert_equal_strict("one two three", edit.text(), "undo restores word deletion");
	edit.redo();
	assert_equal_strict("one three", edit.text(), "redo reapplies word deletion");

	edit.text("alpha beta");
	edit.select_word(7);
	assert_equal_strict("beta", edit.selected_text(), "double-click model selects a word");
	edit.erase_selection();
	assert_equal_strict("alpha ", edit.text(), "cut removes selected word");
	edit.undo();
	assert_equal_strict("alpha beta", edit.text(), "undo restores cut text");

	edit.begin_edit();
	edit.select_all();
	edit.insert("changed");
	edit.cancel_edit();
	assert_equal_strict("alpha beta", edit.text(), "cancel restores edit-start value");

	filter_t filter;
	filter.wildcard("cat");
	assert_equal_strict("cat", filter.text(), "filter preserves user input");
	assert_equal(true, filter.match_text(str::cache("bobcatfish")), "filter applies contains matching");
}

static void should_shape_edit_text_once_per_change()
{
	edit_test_measure_context mc;
	ui::control_layouts positions;
	edit_element edit;

	auto text = std::string(2048, 'a');
	edit.text(text);
	edit.layout(mc, recti(0, 0, 120, 28), positions);
	assert_equal(1, mc.layout_updates, "initial text is shaped once");
	assert_equal(0, mc.prefix_measures, "prefix measuring is not used when shaping succeeds");
	assert_equal(2048_z, edit.caret_from_x(20000), "hit testing reaches the end of long text");

	edit.layout(mc, recti(0, 0, 120, 28), positions);
	assert_equal(1, mc.layout_updates, "unchanged layout reuses caret metrics");

	text.assign(4096, 'b');
	edit.text(text);
	edit.layout(mc, recti(0, 0, 160, 28), positions);
	assert_equal(2, mc.layout_updates, "changed text is shaped once more");
	assert_equal(4096_z, edit.caret_from_x(40000), "larger text keeps bounded shaping");

	edit.text("A\u00e9B");
	edit.layout(mc, recti(0, 0, 160, 28), positions);
	assert_equal(3, mc.layout_updates, "utf-8 text is shaped as one layout");
	assert_equal(1_z, edit.caret_from_x(16), "hit testing still returns a utf-8 boundary");
	assert_equal(3_z, edit.caret_from_x(32), "multi-byte character boundary is preserved");
}

static void should_accumulate_rtl_text_widths_in_logical_order()
{
	const std::vector<uint32_t> positions{0, 1, 2, 3, 4};
	const std::vector<ui::text_cluster_metric> rtl_visual_widths{
		{1, 9.7f}, {1, 3.8f}, {1, 7.7f}, {1, 11.0f}
	};
	const auto xs = ui::logical_text_offset_xs(positions, rtl_visual_widths);
	assert_equal(0, xs[0], "text starts at zero");
	assert_equal(true, std::ranges::is_sorted(xs), "logical caret widths are ascending");
	assert_equal(32, xs.back(), "end width is the accumulated cluster width");

	const std::vector<uint32_t> mixed_positions{0, 1, 2, 3};
	const std::vector<ui::text_cluster_metric> mixed_clusters{{2, 13.0f}, {1, 5.0f}};
	const auto mixed_xs = ui::logical_text_offset_xs(mixed_positions, mixed_clusters);
	assert_equal(13, mixed_xs[1], "a position inside a cluster advances to the cluster boundary");
	assert_equal(13, mixed_xs[2], "the cluster boundary shares that same logical x");
	assert_equal(18, mixed_xs[3], "following clusters continue from the accumulated width");
}

static void should_map_slider_input_to_the_drawn_track()
{
	list_test_measure_context mc;
	mc.padding2 = 8;
	mc.scale_factor = 1.0;
	ui::control_layouts positions;
	auto value = 100;
	auto changes = 0;
	slider_element slider([&value] { return value; }, [&](const int v)
	{
		value = v;
		++changes;
	}, 0, 100, tt.tooltip_thumbnail_size);
	slider.layout(mc, recti(0, 0, 100, 20), positions);

	interaction_context at_max{{94, 10}, {}, true};
	slider.hover(at_max);
	assert_equal(100, value, "pressing the maximum thumb stays at maximum");
	assert_equal(0, changes, "unchanged maximum does not notify");

	value = 0;
	interaction_context at_min{{6, 10}, {}, true};
	slider.hover(at_min);
	assert_equal(0, value, "minimum track endpoint maps to minimum");

	interaction_context at_mid{{50, 10}, {}, true};
	slider.hover(at_mid);
	assert_equal(50, value, "track midpoint maps to midpoint");

	slider.layout(mc, recti(20, 0, 120, 20), positions);
	value = 100;
	changes = 0;
	interaction_context at_offset_max{{114, 10}, {}, true};
	slider.hover(at_offset_max);
	assert_equal(100, value, "offset maximum endpoint maps to maximum before repaint");

	mc.scale_factor = 2.0;
	slider.layout(mc, recti(0, 0, 200, 40), positions);
	value = 0;
	interaction_context at_scaled_max{{188, 20}, {}, true};
	slider.hover(at_scaled_max);
	assert_equal(100, value, "scaled maximum endpoint maps to maximum");

	mc.scale_factor = 1.0;
	slider.layout(mc, recti(0, 0, 100, 20), positions);
	const recti track_bounds{6, 8, 94, 12};
	changes = 0;
	for (auto expected = 0; expected <= 100; ++expected)
	{
		value = expected;
		const auto thumb_x = slider_value_to_track_x(track_bounds, expected, 0, 100);
		interaction_context at_thumb{{thumb_x, 10}, {}, true};
		slider.hover(at_thumb);
		assert_equal(expected, value, "pressing each painted thumb centre preserves its value");
	}
	assert_equal(0, changes, "painted thumb centre presses do not notify");

	// Dense ranges put several values on one pixel; dragging to either end must still reach it.
	for (const auto& [track_width, max_value] : {std::pair{40, 100}, std::pair{25, 100}, std::pair{100, 1000}})
	{
		const recti dense_track{10, 0, 10 + track_width, 4};
		assert_equal(max_value, slider_track_x_to_value(dense_track, dense_track.right, 0, max_value, 0),
		             "dragging from the minimum to the right end reaches the maximum");
		assert_equal(0, slider_track_x_to_value(dense_track, dense_track.left, 0, max_value, max_value),
		             "dragging from the maximum to the left end reaches the minimum");

		const auto mid_x = dense_track.left + track_width / 2;
		const auto proportional = df::round(static_cast<double>(track_width / 2) * max_value / track_width);
		const auto mid_value = slider_track_x_to_value(dense_track, mid_x, 0, max_value, 0);
		assert_equal(slider_value_to_track_x(dense_track, mid_value, 0, max_value), mid_x,
		             "a mid-track drag lands on a value painted under the pointer");
		assert_equal(proportional, mid_value, "a mid-track drag stays proportional");
	}
}

static void should_merge_control_colours_as_straight_alpha()
{
	const auto near = [](const float expected, const float actual, const std::string_view message)
	{
		assert_equal(true, std::abs(expected - actual) < 0.0001f, message);
	};

	ui::color transparent_red(1.0f, 0.0f, 0.0f, 0.0f);
	transparent_red.merge(ui::color(0.0f, 0.0f, 1.0f, 1.0f));
	near(0.0f, transparent_red.r, "transparent source leaves destination red");
	near(0.0f, transparent_red.g, "transparent source leaves destination green");
	near(1.0f, transparent_red.b, "transparent source leaves destination blue");
	near(1.0f, transparent_red.a, "transparent source leaves destination alpha");

	ui::color opaque_red(1.0f, 0.0f, 0.0f, 1.0f);
	opaque_red.merge(ui::color(0.0f, 0.0f, 1.0f, 0.0f));
	near(1.0f, opaque_red.r, "opaque source keeps red");
	near(0.0f, opaque_red.b, "opaque source hides transparent destination");
	near(1.0f, opaque_red.a, "opaque source keeps alpha");

	ui::color zero(0.4f, 0.2f, 0.1f, 0.0f);
	zero.merge(ui::color(0.8f, 0.1f, 0.2f, 0.0f));
	near(0.0f, zero.r, "zero over zero clears red");
	near(0.0f, zero.g, "zero over zero clears green");
	near(0.0f, zero.b, "zero over zero clears blue");
	near(0.0f, zero.a, "zero over zero clears alpha");

	ui::color half_red(1.0f, 0.0f, 0.0f, 0.5f);
	half_red.merge(ui::color(0.0f, 0.0f, 1.0f, 0.5f));
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

///////////////////////////////////////////////////////////////////////////////////////////////////
// The Windows date picker is the only platform control left in the UI: it ignores the app's
// scaling and theme and has no Linux counterpart. Its replacement is a segmented field, and the
// segments, the typing and the stepping are arithmetic that can be pinned without a window. Only
// the model ships here - ui::date_control still creates the platform picker, and drawing the field
// is 1.27.3's - so this pins the arithmetic ahead of the element that will use it.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_edit_a_segmented_date()
{
	using seg = ui::date_segment;

	// Order and separators follow the display locale, so the field is built from it rather than
	// from one hard-coded arrangement.
	{
		const ui::date_edit_model dmy(ui::date_field_order::dmy, false);
		assert_equal(true, dmy.segments() == std::vector{seg::day, seg::month, seg::year}, "day first");

		const ui::date_edit_model mdy(ui::date_field_order::mdy, false);
		assert_equal(true, mdy.segments() == std::vector{seg::month, seg::day, seg::year}, "month first");

		const ui::date_edit_model ymd(ui::date_field_order::ymd, true);
		assert_equal(true, ymd.segments() == std::vector{
			             seg::year, seg::month, seg::day, seg::hour, seg::minute, seg::second
		             }, "year first, and a clock is always most significant first");
	}

	ui::date_edit_model m(ui::date_field_order::dmy, true);
	m.value(df::date_t(2025, 8, 16, 18, 11, 56));

	// Left and Right walk the segments and stop at the ends, so the arrows stay inside the field and
	// Tab has a false answer to act on.
	assert_equal(true, m.active_segment() == seg::day, "the field opens on its first segment");
	assert_equal(false, m.move(-1), "and there is nothing to the left of it");
	assert_equal(true, m.move(1), "Right moves on");
	assert_equal(true, m.active_segment() == seg::month, "to the next segment in locale order");
	m.active(5);
	assert_equal(false, m.move(1), "and stops at the last segment rather than wrapping");

	// Every segment is padded to its width, so the field does not change shape as it is edited.
	assert_equal_strict("16", m.text(0), "day");
	assert_equal_strict("08", m.text(1), "month is padded");
	assert_equal_strict("2025", m.text(2), "year is four digits");
	assert_equal_strict("18", m.text(3), "hour");
	assert_equal_strict("11", m.text(4), "minute");
	assert_equal_strict("56", m.text(5), "second");

	// One step is one unit of the active segment applied to the whole date, so a bound rolls into
	// the neighbour rather than wrapping the segment on its own. That is what a clock does.
	m.value(df::date_t(2025, 12, 31, 23, 59, 59));
	m.active(5);
	m.step(1);
	assert_equal(df::date_t(2026, 1, 1, 0, 0, 0), m.value(), "a second rolls the whole date over");

	m.value(df::date_t(2025, 12, 31, 0, 0, 0));
	m.active(1);
	m.step(1);
	assert_equal(df::date_t(2026, 1, 31, 0, 0, 0), m.value(), "December rolls into January of the next year");

	m.value(df::date_t(2026, 1, 1, 0, 0, 0));
	m.active(1);
	m.step(-1);
	assert_equal(df::date_t(2025, 12, 1, 0, 0, 0), m.value(), "and back the other way");

	// A month step can land on a day the new month does not have. 31 April is not a date, so the
	// day is clamped rather than the whole value being refused or silently rolled into May.
	m.value(df::date_t(2025, 1, 31, 0, 0, 0));
	m.active(1);
	m.step(1);
	assert_equal(df::date_t(2025, 2, 28, 0, 0, 0), m.value(), "31 January steps to the end of February");

	m.value(df::date_t(2024, 1, 31, 0, 0, 0));
	m.active(1);
	m.step(1);
	assert_equal(df::date_t(2024, 2, 29, 0, 0, 0), m.value(), "and a leap year has the extra day");

	// Stepping the year off a leap day does the same.
	m.value(df::date_t(2024, 2, 29, 0, 0, 0));
	m.active(2);
	m.step(1);
	assert_equal(df::date_t(2025, 2, 28, 0, 0, 0), m.value(), "29 February steps to the 28th");

	// Digits type left to right and the segment advances when it can hold no more, so a date is
	// typed straight through without reaching for a separator key.
	m.value(df::date_t(2025, 8, 16, 0, 0, 0));
	m.active(0);
	m.type_digit('0');
	m.type_digit('4');
	assert_equal(4, m.field(seg::day), "two digits fill the day");
	assert_equal(true, m.active_segment() == seg::month, "and the field moves on by itself");

	m.type_digit('7');
	assert_equal(7, m.field(seg::month), "a digit no second digit could follow finishes the segment");
	assert_equal(true, m.active_segment() == seg::year, "so that one advances too");

	m.type_digit('1');
	m.type_digit('9');
	m.type_digit('9');
	m.type_digit('9');
	assert_equal(1999, m.field(seg::year), "a year takes four");
	assert_equal(df::date_t(1999, 7, 4, 0, 0, 0), m.value(), "and the typed date is the value");

	// A value the segment cannot hold starts again from that digit rather than being refused, which
	// is what a user retyping a wrong month means by it.
	m.value(df::date_t(2025, 8, 16, 0, 0, 0));
	m.active(1);
	m.type_digit('1');
	assert_equal(1, m.field(seg::month), "1 could still become 10, 11 or 12");
	m.type_digit('9');
	assert_equal(9, m.field(seg::month), "19 is not a month, so 9 starts the segment again");

	// Only the month has a name, so it is the only segment worth a drop menu.
	m.active(1);
	assert_equal(true, m.active_has_menu(), "the month offers its names");
	m.active(0);
	assert_equal(false, m.active_has_menu(), "a day is a number faster to type than to pick");
	m.active(2);
	assert_equal(false, m.active_has_menu(), "and so is a year");
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
	explicit flex_wrapping_test_element(const sizei desired) : _desired(desired)
	{
		padding(0);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
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

	ui::control_layouts positions;
	const auto applied_extent = layout_flex_elements(column_elements, mc, positions, {10, 20, 110, 80}, column);
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
	const auto column_row = calc_flex_layout(
		std::vector<view_element_ptr>{fixed, flexible_first, flexible_second}, mc, {100, -1}, columns);
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
	const auto centered = calc_flex_layout(std::vector<view_element_ptr>{padded_first, padded_second}, mc,
	                                       {100, -1}, centered_row);
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
	assert_equal(false, can_compare_file_types(&file_type::folder, &file_type::folder),
	             "two folders do not compare");
	assert_equal(false, can_compare_file_types(nullptr, jpg), "an unknown type does not compare");
}

// A video with embedded cover art draws the cover art on its tile, so the tile has to be the shape
// of the art. Sizing it from the video's own dimensions letterboxed the art inside a frame-shaped
// tile, and the republish that follows every scan pass used to revert it to the video shape.
static void should_shape_the_tile_by_what_it_draws()
{
	df::index_file_item indexed;
	indexed.name = str::cache("clip.mp4");
	indexed.ft = files::file_type_from_name(indexed.name);

	const auto md = std::make_shared<prop::item_metadata>();
	md->width = 1920;
	md->height = 1080;
	indexed.metadata.store(md);

	const auto path = df::file_path("c:\\clip.mp4");
	const auto item = std::make_shared<df::item_element>(path, indexed);

	assert_equal(1920, item->layout_dims().cx, "video width shapes the tile before cover art arrives");
	assert_equal(1080, item->layout_dims().cy, "video height shapes the tile before cover art arrives");
	assert_equal(true, item->layout_aspect_known(), "indexed dimensions are an exact aspect");

	const auto cover_art = std::make_shared<ui::image>(df::blob(16), sizei(600, 600),
	                                                   ui::image_format::JPEG, ui::orientation::top_left);
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
	indexed.name = str::cache("unscanned.jpg");
	indexed.ft = files::file_type_from_name(indexed.name);

	const auto path = df::file_path("c:\\unscanned.jpg");
	const auto item = std::make_shared<df::item_element>(path, indexed);

	assert_equal(false, item->layout_aspect_known(), "nothing known before a scan");

	const auto thumb = std::make_shared<ui::image>(df::blob(16), sizei(160, 120),
	                                               ui::image_format::JPEG, ui::orientation::top_left);
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
		indexed.name = str::cache(name);
		indexed.ft = files::file_type_from_name(indexed.name);

		const auto md = std::make_shared<prop::item_metadata>();
		md->width = static_cast<uint16_t>(cx);
		md->height = static_cast<uint16_t>(cy);
		indexed.metadata.store(md);

		return std::make_shared<df::item_element>(df::file_path("c:\\" + name), indexed);
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
		return list_view::row_for_work_index(rows.size(), work_index, [&rows](const size_t i) { return rows[i]; });
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
	s.open(view, df::search_t().add_selector(test_files_folder), {});
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

void register_view_tests(view_state& state, test_registry& tests)
{
	// VIEW-012 - Movie clip sessions are retired before playback/focus no longer owns them.
	tests.add("Should decide when Movie clip sessions are retired"s,
	          should_decide_when_movie_clip_sessions_are_retired);
	tests.add("Should step Movie clips from the playhead while playing"s,
	          should_step_movie_clips_from_the_playhead_while_playing);
	// VIEW-013 - stale Movie cache completions must not mutate current request state.
	tests.add("Should reject stale Movie cache completions before state changes"s,
	          should_reject_stale_movie_cache_completions_before_state_changes);
	// VIEW-014 - a source that moves during probing gets bounded retries, then an explicit state.
	tests.add("Should bound Movie probe retries for moving sources"s,
	          should_bound_movie_probe_retries_for_moving_sources);
	tests.add("Should apply a Movie probe to the clip it measured"s,
	          should_apply_a_movie_probe_to_the_clip_it_measured);
	tests.add("Should import a Movie Maker project from the collection"s,
	          should_import_a_movie_maker_project_from_the_collection);
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
	// Edit stuck on Loading - a file whose header loads but whose pixels do not decode.
	tests.add("Should report Edit preview that cannot decode as a load failure"s,
	          should_report_edit_preview_that_cannot_decode_as_a_load_failure);
	// G14b follow-up - a drawn crop must survive until the async bitmap load can apply it.
	tests.add("Should apply pending Edit crop after async load"s,
	          should_apply_pending_edit_crop_after_async_load);
	// G14b follow-up - resizing keeps the last visible preview until the replacement is ready.
	tests.add("Should draw retained Edit preview while replacement is pending"s,
	          should_draw_retained_edit_preview_while_replacement_is_pending);
	// G14b follow-up - Edit load/preview workers coalesce superseded requests.
	tests.add("Should coalesce Edit load and preview requests to the latest"s,
	          should_coalesce_edit_load_and_preview_requests_to_the_latest);
	// G14b follow-up - side-panel pixel controls obey the actual source-pixel state.
	tests.add("Should withhold Edit pixel controls until source pixels are loaded"s,
	          should_withhold_edit_pixel_controls_until_source_pixels_are_loaded);
	// VIEW-011 - Items routes touch pan through the same magnified-media zoom operation as Fullscreen.
	tests.add("Should pan Items touch zoom on magnified media like Fullscreen"s,
	          should_pan_items_touch_on_magnified_media_like_fullscreen);

	tests.add("Should track the active row across a reorder"s, should_track_the_active_row_across_a_reorder);
	tests.add("Should resolve list rows by work index"s, should_resolve_list_rows_by_work_index);
	tests.add("Should select settled zoom sampler"s, should_select_settled_zoom_sampler);
	// MOD-016 - same-path source versions can move backward or change without increasing a maximum.
	tests.add("Should reload displayed photo when source version moves backward"s,
	          should_reload_displayed_photo_when_source_version_moves_backward);
	// MOD-017 - displayed cache entries do not stop offscreen texture-budget eviction.
	tests.add("Should skip displayed textures when evicting retained texture budget"s,
	          should_skip_displayed_textures_when_evicting_retained_texture_budget);
	// MOD-018 - comparison preview texture identities are handed back on worker exits.
	tests.add("Should release comparison preview textures on UI after worker exit"s,
	          should_release_comparison_preview_textures_on_ui_after_worker_exit);
	tests.add("Should accumulate precision wheel deltas"s, should_accumulate_precision_wheel_deltas);
	tests.add("Should validate zoom navigator mode"s, should_validate_zoom_navigator_mode);
	tests.add("Should keep comparison zoom panes matched"s, should_keep_comparison_zoom_panes_matched);
	tests.add("Should calculate zoom model fit without enlarging"s, should_calculate_zoom_fit_without_enlarging);
	tests.add("Should recalculate zoom fit variants"s, should_recalculate_zoom_fit_variants);
	tests.add("Should keep the pan when a fit variant refits"s, should_keep_the_pan_when_a_fit_variant_refits);
	tests.add("Should step zoom model reversibly through fit"s, should_step_zoom_reversibly_through_fit);
	tests.add("Should keep zoom model anchor still"s, should_keep_zoom_anchor_still);
	tests.add("Should step zoom model at pointer anchor"s, should_step_zoom_at_pointer_anchor);
	tests.add("Should preserve zoom model center across dimensions"s,
	          should_preserve_zoom_center_across_dimensions);
	tests.add("Should suspend carried zoom model below fit"s, should_suspend_carried_zoom_below_fit);
	tests.add("Should clamp zoom model pan to edges"s, should_clamp_zoom_model_pan_to_edges);
	tests.add("Should toggle zoom model fit to last explicit view"s, should_toggle_fit_to_last_explicit_zoom);
	tests.add("Should toggle zoom model fit back to a fit variant"s, should_toggle_fit_back_to_a_fit_variant);
	tests.add("Should hit test ratings within five stars"s, should_hit_test_ratings_within_five_stars);
	tests.add("Should keep zoom model anchor through layout change"s,
	          should_keep_zoom_anchor_through_layout_change);
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
	tests.add("Should rearm selector decode after resource event"s,
	          should_rearm_selector_decode_after_resource_event);
	tests.add("Should refresh tooltip when staged thumbnail lands"s,
	          should_refresh_tooltip_when_staged_thumbnail_lands);
	tests.add("Should not refresh tooltip for fresh request when only cover art is cached"s,
	          should_not_refresh_tooltip_for_fresh_request_when_only_cover_art_is_cached);
	tests.add("Should stage one scroll tooltip thumbnail"s, should_stage_one_scroll_tooltip_thumbnail);
	tests.add("Should retry scroll tooltip after failed stage"s, should_retry_scroll_tooltip_after_failed_stage);
	tests.add("Should publish async selection thumbnail strip"s,
	          should_publish_async_selection_thumbnail_strip);
	tests.add("Should discard async selection strip after clear"s,
	          should_discard_async_selection_strip_after_clear);
	tests.add("Should not keep selection strip frame alive"s, should_not_keep_selection_strip_frame_alive);
	tests.add("Should preview rotation with inverse destination transform"s,
	          should_preview_rotation_with_inverse_destination_transform);
	tests.add("Should stage neighbour stand ins"s, should_stage_neighbour_stand_ins);
	// Issue #78 - anamorphic video fills its box at the declared aspect
	tests.add("Should shape video by its display dimensions"s, should_shape_video_by_its_display_dimensions);
	tests.add("Should show selected thumbnail without waiting"s, should_show_selected_thumbnail_without_waiting);
	tests.add("Should hold media column still as detail arrives"s, should_hold_media_column_still_as_detail_arrives);
	tests.add("Should fit the whole primary block in the media column"s,
	          should_fit_the_whole_primary_block_in_the_media_column);
	tests.add("Should follow the primary block when verbose is open"s,
	          should_follow_the_primary_block_when_verbose_is_open);
	tests.add("Should centre a media column with no detail"s,
	          should_centre_a_media_column_with_no_detail);
	tests.add("Should follow a centred media column block with its detail"s,
	          should_follow_a_centred_block_with_its_detail);
	tests.add("Should keep half the media column pane for the media"s, should_keep_half_the_pane_for_the_media);
	tests.add("Should bound the media overlay to a quarter"s, should_bound_the_media_overlay_to_a_quarter);
	tests.add("Should leave full screen for task views"s, should_leave_full_screen_for_task_views);
	tests.add("Should preserve view scroller anchor across layout"s,
	          should_preserve_view_scroller_anchor_across_layout);
	tests.add("Should reserve view scroller footer"s, should_reserve_view_scroller_footer);
	tests.add("Should drag view scroller thumb from grab point"s,
	          should_drag_view_scroller_thumb_from_grab_point);
	tests.add("Should close view scroller bands over track"s, should_close_view_scroller_bands_over_track);
	tests.add("Should survive a host with no window"s, should_survive_a_host_with_no_window);
	tests.add("Should not perform a cancelled gesture on release"s,
	          should_not_perform_a_cancelled_gesture_on_release);
	tests.add("Should offer escape to an idle controller"s, should_offer_escape_to_an_idle_controller);
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
	// G13 UI-002 - failed decoded dialog thumbnails do not enter the surface collection or cap.
	tests.add("Should reject failed dialog thumbnail decodes"s, should_reject_failed_dialog_thumbnail_decodes);
	// G13 UI-003 - search controls expose the list frame through the non-null accessor.
	tests.add("Should use non-null search list frame"s, should_use_non_null_search_list_frame);
	// G13 UI-012 - failed texture uploads leave a hole rather than compacting later thumbnails.
	tests.add("Should preserve thumbnail texture slots after upload failure"s,
	          should_preserve_thumbnail_texture_slots_after_upload_failure);
	tests.add("Should layout selection thumbnail collage"s, should_layout_selection_thumbnail_collage);
	// PLAT-005 - clipboard access and publication failures preserve text edits.
	tests.add("Should edit single-line text"s, should_edit_single_line_text);
	tests.add("Should shape edit text once per change"s, should_shape_edit_text_once_per_change);
	tests.add("Should accumulate RTL text widths in logical order"s,
	          should_accumulate_rtl_text_widths_in_logical_order);
	tests.add("Should map slider input to the drawn track"s, should_map_slider_input_to_the_drawn_track);
	tests.add("Should merge control colours as straight alpha"s, should_merge_control_colours_as_straight_alpha);
	tests.add("Should hit test comparison divider in device coordinates"s,
	          should_hit_test_comparison_divider_in_device_coordinates);
	tests.add("Should edit a segmented date"s, should_edit_a_segmented_date);
	tests.add("Should clear detail row layout metrics"s, should_clear_detail_row_layout_metrics);
	tests.add("Should classify aspect ratio groups"s, should_classify_aspect_ratio_groups);

	//
	// Flex layout
	//
	tests.add("Should layout flex elements"s, should_layout_flex_elements);

	//
	// Item tile geometry
	//
	tests.add("Should limit comparison to like pairs"s, should_limit_comparison_to_like_pairs);
	tests.add("Should shape the tile by what it draws"s, should_shape_the_tile_by_what_it_draws);
	tests.add("Should not justify a tile shaped by a thumbnail"s, should_not_justify_a_tile_shaped_by_a_thumbnail);
	tests.add("Should keep tile aspect when laying out a row"s, should_keep_tile_aspect_when_laying_out_a_row);
}
