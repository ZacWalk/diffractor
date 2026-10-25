// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for the Movie document (model_movie) -- the derived output geometry, transition
// and duration arithmetic, timeline edits and undo, what entering Movie does with the timeline it
// holds, the OpenTimelineIO project file, the Windows Live Movie Maker reader, and the choice of a
// moved source among same-named files. docs/movie.md owns the behaviour.

#include "pch.h"
#include "test.h"
#include "model.h"
#include "model_movie.h"
#include "test_runner.h"
#include "libdesktop/util_json.h"
#include "view_movie.h"

static df::folder_path movie_test_folder()
{
	return df::folder_path(df::windows_path_semantics ? "c:\\movie" : "/movie");
}

static df::file_path movie_test_path(const std::string_view name)
{
	return movie_test_folder().combine_file(name);
}

static movie_clip make_video(const std::string_view name, const double duration, const sizei extent,
                             const double rate)
{
	movie_clip c;
	c.path = movie_test_path(name);
	c.is_photo = false;
	c.source_duration = duration;
	c.start = 0;
	c.end = duration;
	c.extent = extent;
	c.frame_rate = rate;
	return c;
}

static movie_clip make_photo(const std::string_view name, const double hold, const sizei extent)
{
	movie_clip c;
	c.path = movie_test_path(name);
	c.is_photo = true;
	c.end = hold;
	c.extent = extent;
	return c;
}

//
// Output geometry
//

static void should_derive_movie_output()
{
	assert_equal(1920, derive_movie_output({}).extent.cx, "an empty timeline falls back to 1080p");
	assert_equal(movie_default_frame_rate, derive_movie_output({}).frame_rate, "and to the default rate");

	const auto mixed = derive_movie_output({
		make_video("a.mp4", 10, {1280, 720}, 60),
		make_video("b.mp4", 10, {1920, 1080}, 30)
	});

	assert_equal(1920, mixed.extent.cx, "the largest video decides the width");
	assert_equal(1080, mixed.extent.cy, "the largest video decides the height");
	assert_equal(60, mixed.frame_rate, "the highest source rate decides the rate");
	assert_equal(12441600, mixed.video_bitrate, "bitrate follows frame size and rate");

	// The rule that matters most: a 48-megapixel photo beside a 720p clip must not produce an
	// 8000-pixel movie. Removing the have_video test makes this line fail with 8000.
	const auto with_photo = derive_movie_output({
		make_photo("big.jpg", 4, {8000, 6000}),
		make_video("small.mp4", 10, {1280, 720}, 25)
	});

	assert_equal(1280, with_photo.extent.cx, "a photo never drives the frame size when a video is present");
	assert_equal(25, with_photo.frame_rate, "the only video decides the rate");

	const auto photos_only = derive_movie_output({make_photo("big.jpg", 4, {8000, 6000})});

	assert_equal(2880, photos_only.extent.cx, "a photo-only timeline is clamped inside 4K");
	assert_equal(2160, photos_only.extent.cy, "and keeps its aspect ratio while clamped");
	assert_equal(movie_default_frame_rate, photos_only.frame_rate, "with no video there is no source rate");

	const auto odd = derive_movie_output({make_video("odd.mp4", 10, {1921, 1081}, 30)});

	assert_equal(1920, odd.extent.cx, "an odd width is rounded down");
	assert_equal(1080, odd.extent.cy, "an odd height is rounded down");

	const auto fast = derive_movie_output({make_video("fast.mp4", 10, {1920, 1080}, 240)});
	assert_equal(movie_max_frame_rate, fast.frame_rate, "an extreme source rate is clamped");
}

//
// Timing
//

static void should_time_movie_transitions()
{
	const std::vector clips{
		make_video("a.mp4", 10, {1920, 1080}, 30),
		make_video("b.mp4", 10, {1920, 1080}, 30),
		make_video("c.mp4", 10, {1920, 1080}, 30)
	};

	movie_settings cut;
	cut.transition = movie_transition::cut;

	const auto cut_timing = calc_movie_timing(clips, cut);
	assert_equal(30.0, cut_timing.duration, "cuts do not shorten the movie");
	assert_equal(10.0, cut_timing.starts[1], "a cut starts the next clip where the last ended");
	assert_equal(20.0, cut_timing.starts[2], "and so on");

	movie_settings fade;
	fade.transition = movie_transition::crossfade;
	fade.transition_seconds = 1.0;

	const auto fade_timing = calc_movie_timing(clips, fade);
	assert_equal(28.0, fade_timing.duration, "each crossfade shortens the movie by its length");
	assert_equal(0.0, fade_timing.starts[0], "the first clip starts at zero");
	assert_equal(9.0, fade_timing.starts[1], "the second clip starts one transition early");
	assert_equal(18.0, fade_timing.starts[2], "and the third two");

	// A transition longer than the clip it joins must shorten rather than consume it. Dropping the
	// half-clip ceiling in movie_overlap makes this 18, and the middle clip disappears.
	const std::vector short_middle{
		make_video("a.mp4", 10, {1920, 1080}, 30),
		make_video("b.mp4", 2, {1920, 1080}, 30),
		make_video("c.mp4", 10, {1920, 1080}, 30)
	};

	movie_settings long_fade;
	long_fade.transition_seconds = 4.0;

	const auto clamped = calc_movie_timing(short_middle, long_fade);
	assert_equal(1.0, movie_overlap(short_middle[0], short_middle[1], long_fade),
	             "the overlap is capped at half the shorter clip");
	assert_equal(20.0, clamped.duration, "a two second clip survives a four second transition");
	assert_equal(10.0, clamped.starts[2], "and still separates its neighbours");

	assert_equal(0.0, calc_movie_timing({}, fade).duration, "an empty timeline has no duration");
	assert_equal(10.0, calc_movie_timing({clips[0]}, fade).duration, "one clip has no transition to apply");
}

static void should_compose_a_movie_frame()
{
	const std::vector clips{
		make_video("a.mp4", 10, {1920, 1080}, 30),
		make_video("b.mp4", 10, {1920, 1080}, 30)
	};

	movie_settings settings;
	settings.transition = movie_transition::crossfade;
	settings.transition_seconds = 2.0;
	settings.fade_in = false;
	settings.fade_out = false;

	// The movie is 18s: 0-8 is a alone, 8-10 is the crossfade, 10-18 is b alone.
	const auto early = calc_movie_frame(clips, settings, 4.0);
	assert_equal(0, early.a.index, "one clip carries the frame outside a transition");
	assert_equal(4.0, early.a.source_time, "at its own position in the source");
	assert_equal(1.0, early.a.weight, "at full weight");
	assert_equal(-1, early.b.index, "with nothing mixed into it");

	const auto middle = calc_movie_frame(clips, settings, 9.0);
	assert_equal(0, middle.a.index, "the outgoing clip is first");
	assert_equal(1, middle.b.index, "the incoming clip is second");
	assert_equal(0.5, middle.a.weight, "halfway through the transition they are equal");
	assert_equal(0.5, middle.b.weight, "on both sides");
	assert_equal(9.0, middle.a.source_time, "the outgoing clip is near its end");
	assert_equal(1.0, middle.b.source_time, "and the incoming one just past its start");

	const auto late = calc_movie_frame(clips, settings, 14.0);
	assert_equal(1, late.a.index, "after the transition only the incoming clip is left");
	assert_equal(6.0, late.a.source_time, "at its own position");
	assert_equal(-1, late.b.index, "with nothing mixed into it");

	// The final instant must still draw. An exclusive end test here leaves the last frame empty,
	// which is one black frame at the end of every movie.
	const auto last = calc_movie_frame(clips, settings, 18.0);
	assert_equal(1, last.a.index, "the final instant still draws a clip");
	assert_equal(true, calc_movie_frame({}, settings, 0).is_empty(), "an empty timeline draws nothing");

	// A trimmed clip reports positions in its source, not on the timeline.
	auto trimmed_clips = clips;
	trimmed_clips[0].start = 3;
	trimmed_clips[0].end = 9;
	assert_equal(5.0, calc_movie_frame(trimmed_clips, settings, 2.0).a.source_time,
	             "a trim offsets the position asked of the source");

	assert_equal(0.0, calc_movie_frame({make_photo("p.jpg", 5, {4000, 3000})}, settings, 3.0).a.source_time,
	             "a photo has one frame however long it is held");

	settings.fade_in = true;
	settings.fade_out = true;

	assert_equal(1.0, calc_movie_frame(clips, settings, 0.0).fade_to_black, "the movie opens black");
	assert_equal(0.5, calc_movie_frame(clips, settings, 1.0).fade_to_black, "and fades up");
	assert_equal(0.0, calc_movie_frame(clips, settings, 5.0).fade_to_black, "and is clear in the middle");
	assert_equal(0.5, calc_movie_frame(clips, settings, 17.0).fade_to_black, "then fades down");
	assert_equal(1.0, calc_movie_frame(clips, settings, 18.0).fade_to_black, "and closes black");
}

//
// The timeline as a document
//

static void should_edit_the_movie_timeline()
{
	movie_project project;

	assert_equal(true, project.is_empty(), "a new project holds nothing");
	assert_equal(false, project.can_undo(), "and has nothing to undo");
	assert_equal(false, project.is_modified(), "and is unmodified");

	project.append(make_video("a.mp4", 10, {1920, 1080}, 30));
	project.append(make_video("b.mp4", 10, {1920, 1080}, 30));
	project.append(make_photo("c.jpg", 4, {4000, 3000}));

	assert_equal(3, static_cast<int>(project.size()), "three clips were appended");
	assert_equal(2, static_cast<int>(project.current()), "the last append is current");
	assert_equal(true, project.is_modified(), "editing marks the project modified");

	// The same source twice is the case a set-based selector could not express, and the reason the
	// timeline is a document rather than a view of the selection.
	project.append(make_video("a.mp4", 10, {1920, 1080}, 30));
	assert_equal(4, static_cast<int>(project.size()), "the same source can appear twice");
	assert_equal("a.mp4", project.clips()[3].path.name().sv(), "and it is the same source");

	project.insert(0, make_video("first.mp4", 5, {1920, 1080}, 30));
	assert_equal("first.mp4", project.clips()[0].path.name().sv(), "insert places the clip at the index");
	assert_equal(0, static_cast<int>(project.current()), "and makes it current");

	project.move(0, 2);
	assert_equal("first.mp4", project.clips()[2].path.name().sv(), "move relocates the clip");
	assert_equal("a.mp4", project.clips()[0].path.name().sv(), "and closes the gap behind it");

	project.undo();
	assert_equal("first.mp4", project.clips()[0].path.name().sv(), "undo restores the order");
	const auto stale_revision = project.edit_revision();
	project.trim(0, 1, 4);
	project.undo();
	project.mark_saved(movie_test_path("movie.otio"), stale_revision);
	assert_equal(true, project.is_modified(), "a save started before undo cannot clear the changed document");

	project.current(1);
	project.remove(1);
	assert_equal(4, static_cast<int>(project.size()), "remove takes one clip out");
	assert_equal(1, static_cast<int>(project.current()), "and current stays in range");

	project.remove(3);
	assert_equal(1, static_cast<int>(project.current()), "removing after current leaves current alone");
	project.current(2);
	project.remove(0);
	assert_equal(1, static_cast<int>(project.current()), "removing before current shifts current back");

	project.remove(99);
	assert_equal(2, static_cast<int>(project.size()), "an out of range remove does nothing");

	while (project.can_undo()) project.undo();
	assert_equal(true, project.is_empty(), "undo unwinds every edit");
}

static void should_render_only_a_resolved_movie()
{
	movie_project project;
	auto clip = make_video("a.mp4", 10, {1920, 1080}, 30);
	project.append(clip);
	assert_equal(false, project.is_ready_to_render(), "an unprobed clip cannot render");

	clip.is_probed = true;
	project.replace_quietly(0, clip);
	assert_equal(false, project.is_ready_to_render(), "a clip the probe did not find cannot render");

	clip.is_missing = false;
	project.replace_quietly(0, clip);
	assert_equal(true, project.is_ready_to_render(), "a resolved clip with dimensions and duration can render");

	clip.extent = {};
	project.replace_quietly(0, clip);
	assert_equal(false, project.is_ready_to_render(), "a source with no decodable dimensions cannot render");
}

static void should_trim_movie_clips()
{
	movie_project project;
	project.append(make_video("a.mp4", 10, {1920, 1080}, 30));

	project.trim(0, 2, 8);
	assert_equal(2.0, project.clips()[0].start, "the in point is taken");
	assert_equal(8.0, project.clips()[0].end, "the out point is taken");
	assert_equal(6.0, project.clips()[0].duration(), "and the clip is that long");
	assert_equal(true, project.clips()[0].is_trimmed(), "a trimmed clip says so");

	project.trim(0, -5, 100);
	assert_equal(0.0, project.clips()[0].start, "a trim below zero is clamped");
	assert_equal(10.0, project.clips()[0].end, "a trim past the source is clamped");
	assert_equal(false, project.clips()[0].is_trimmed(), "and a full-length clip is untrimmed");

	movie_project photos;
	movie_settings settings;
	settings.photo_seconds = 4.0;
	photos.reset({}, settings, {});
	photos.append(make_photo("a.jpg", 4, {4000, 3000}));
	photos.append(make_photo("b.jpg", 4, {4000, 3000}));

	photos.trim(1, 0, 9);
	assert_equal(9.0, photos.clips()[1].end, "a photo takes the duration it is given");
	assert_equal(false, photos.clips()[1].photo_duration_is_default, "and stops following the default");

	settings.photo_seconds = 6.0;
	photos.settings(settings);

	assert_equal(6.0, photos.clips()[0].end, "a photo still on the default follows it");
	assert_equal(9.0, photos.clips()[1].end, "a photo the user set keeps its own length");
}

static void should_select_and_move_movie_clips()
{
	movie_project project;

	for (auto i = 0; i < 5; ++i)
	{
		project.append(make_video(std::format("{}.mp4", i), 10, {1920, 1080}, 30));
	}

	const auto names = [&]
	{
		std::string result;
		for (const auto& c : project.clips()) result += c.path.file_name_without_extension();
		return result;
	};

	assert_equal("01234", names(), "five clips in the order they were added");

	project.select(1, false, false);
	assert_equal(1, static_cast<int>(project.selected().size()), "a plain click selects one");
	assert_equal(1, static_cast<int>(project.current()), "and focuses it");

	project.select(3, true, false);
	assert_equal(3, static_cast<int>(project.selected().size()), "shift runs from the anchor");
	assert_equal(3, static_cast<int>(project.current()), "and focus lands on the clicked clip");
	assert_equal(true, project.is_selected(2), "including everything between");

	project.select(0, false, true);
	assert_equal(4, static_cast<int>(project.selected().size()), "ctrl adds one");
	assert_equal(0, static_cast<int>(project.selected().front()), "and the set stays sorted");

	project.select(0, false, true);
	assert_equal(3, static_cast<int>(project.selected().size()), "ctrl on a selected clip removes it");

	// The last selected clip cannot be toggled away, or a command that acts on the selection would
	// have nothing to act on while the strip still shows a focused clip.
	project.select(1, false, false);
	project.select(1, false, true);
	assert_equal(1, static_cast<int>(project.selected().size()), "the final selected clip is kept");

	// Moving a block keeps the clips in their own order and carries focus with them. Extending from
	// the anchor already leaves focus on the clip that was clicked last.
	project.select(1, false, false);
	project.select(2, true, false);
	assert_equal(2, static_cast<int>(project.current()), "focus is on the end of the extended range");
	project.focus(1);
	assert_equal(2, static_cast<int>(project.selected().size()), "moving focus keeps the selected block");
	assert_equal(1, static_cast<int>(project.current()), "and focuses the pressed clip");
	project.move_selection(5);

	assert_equal("03412", names(), "a selected block moves to the drop point in its own order");
	assert_equal(3, static_cast<int>(project.selected().front()), "and is selected where it landed");
	assert_equal(3, static_cast<int>(project.current()), "with focus still on the pressed clip");

	project.undo();
	assert_equal("01234", names(), "undo restores the order");
	assert_equal(2, static_cast<int>(project.selected().size()), "and the selection that made it");

	// Dropping a block back where it already is must not cost an undo step, or Ctrl+Z would appear
	// to do nothing after a mis-drag.
	const auto before = project.can_undo();
	project.select(0, false, false);
	project.move_selection(0);
	assert_equal(before, project.can_undo(), "a block dropped where it already is changes nothing");

	movie_project no_history;
	no_history.insert_many(0, {make_video("a.mp4", 10, {1920, 1080}, 30), make_video("b.mp4", 10, {1920, 1080}, 30)});
	no_history.mark_seeded();
	no_history.select(0, false, false);
	no_history.move_selection(0);
	assert_equal(false, no_history.can_undo(), "a no-op move consumes no undo step from an empty history");

	project.select(1, false, false);
	project.select(2, true, false);
	project.remove_selection();
	assert_equal("034", names(), "removing the selection takes every clip in it");
	assert_equal(1, static_cast<int>(project.current()), "and focus lands where the block was");
	assert_equal(1, static_cast<int>(project.selected().size()), "with something selected again");

	project.select_all();
	assert_equal(3, static_cast<int>(project.selected().size()), "select all takes every clip");
	project.remove_selection();
	assert_equal(true, project.is_empty(), "and removing them empties the timeline");
	assert_equal(0, static_cast<int>(project.selected().size()), "leaving nothing selected");
}

//
// The project file
//

// A project file is untrusted input. Its durations used to be read straight into the settings, and
// a hold of 1e308 seconds reached the frame count as a number no integer can hold - the conversion
// that walks it is undefined, which is not a thing a document should be able to ask for.
// A project file is untrusted. Nesting followed on the stack overflowed it, which no handler can catch.
static void should_refuse_a_deeply_nested_movie_project()
{
	constexpr size_t depth = 500'000;
	std::string json = R"({"OTIO_SCHEMA":"Timeline.1","tracks":)";
	json.append(depth, '[');
	json.append(depth, ']');
	json += "}";

	const auto loaded = read_otio(json, movie_test_path("x.otio").folder());
	assert_equal(true, loaded.clips.empty(), "a nested project loads no clips and the process survives");
}

static void should_bound_movie_project_durations()
{
	const auto url = str::replace(movie_test_path("a.mp4").pack(), "\\", "/");

	const auto project_with = [&url](const std::string_view photo_seconds, const std::string_view transition_seconds)
	{
		return std::format(R"({{
			"OTIO_SCHEMA": "Timeline.1",
			"metadata": {{ "diffractor": {{
				"transition": "crossfade",
				"transition_seconds": {},
				"photo_seconds": {}
			}} }},
			"tracks": {{
				"OTIO_SCHEMA": "Stack.1",
				"children": [
					{{
						"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
							{{
								"OTIO_SCHEMA": "Clip.1",
								"source_range": {{
									"OTIO_SCHEMA": "TimeRange.1",
									"start_time": {{ "OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 0.0 }},
									"duration": {{ "OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 48.0 }}
								}},
								"media_reference": {{
									"OTIO_SCHEMA": "ExternalReference.1", "target_url": "{}"
								}}
							}}
						]
					}}
				]
			}}
		}})", transition_seconds, photo_seconds, url);
	};

	const auto enormous = read_otio(project_with("1e308", "1e308"), movie_test_folder());
	assert_equal(true, static_cast<bool>(enormous), "the project still reads");
	assert_equal(movie_max_photo_seconds, enormous.settings.photo_seconds, "an enormous hold is bounded");
	assert_equal(movie_max_transition_seconds, enormous.settings.transition_seconds,
	             "and so is an enormous transition");

	const auto negative = read_otio(project_with("-5.0", "-5.0"), movie_test_folder());
	assert_equal(movie_min_photo_seconds, negative.settings.photo_seconds, "a negative hold is bounded");
	assert_equal(0.0, negative.settings.transition_seconds, "and a negative transition is no transition");

	// A value the panel could have produced is left exactly as written.
	const auto ordinary = read_otio(project_with("6.5", "2.25"), movie_test_folder());
	assert_equal(6.5, ordinary.settings.photo_seconds, "an ordinary hold is untouched");
	assert_equal(2.25, ordinary.settings.transition_seconds, "and so is an ordinary transition");

	// Each clip's own range is held too. The settings above were bounded while a clip's range reached
	// the frame arithmetic and the displayed clock unbounded, so one enormous clip still overflowed.
	const auto clip_project = [](const std::string_view file, const std::string_view duration_value)
	{
		const auto clip_url = str::replace(movie_test_path(file).pack(), "\\", "/");

		return std::format(R"({{
			"OTIO_SCHEMA": "Timeline.1",
			"tracks": {{
				"OTIO_SCHEMA": "Stack.1",
				"children": [
					{{
						"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
							{{
								"OTIO_SCHEMA": "Clip.1",
								"source_range": {{
									"OTIO_SCHEMA": "TimeRange.1",
									"start_time": {{ "OTIO_SCHEMA": "RationalTime.1", "rate": 1.0, "value": 0.0 }},
									"duration": {{ "OTIO_SCHEMA": "RationalTime.1", "rate": 1.0, "value": {} }}
								}},
								"media_reference": {{
									"OTIO_SCHEMA": "ExternalReference.1", "target_url": "{}"
								}}
							}}
						]
					}}
				]
			}}
		}})", duration_value, clip_url);
	};

	// Compared rather than formatted: str::to_string cannot hold an unbounded value, so a regression
	// here would take the whole run down instead of failing this test.
	const auto enormous_photo = read_otio(clip_project("b.jpg", "1e308"), movie_test_folder());
	assert_equal(1_z, enormous_photo.clips.size(), "an enormous still is still read");
	assert_equal(true, enormous_photo.clips.front().duration() == movie_max_clip_seconds, "with its hold bounded");

	const auto enormous_video = read_otio(clip_project("a.mp4", "1e308"), movie_test_folder());
	assert_equal(1_z, enormous_video.clips.size(), "an enormous video range is still read");
	assert_equal(true, enormous_video.clips.front().duration() == movie_max_clip_seconds, "with its range bounded");

	const auto ordinary_photo = read_otio(clip_project("b.jpg", "120.0"), movie_test_folder());
	assert_equal(1_z, ordinary_photo.clips.size(), "an ordinary still is read");
	assert_equal(true, ordinary_photo.clips.front().duration() == 120.0, "a long but ordinary still keeps its hold");
}

static void should_round_trip_a_movie_project()
{
	std::vector<movie_clip> clips;

	auto video = make_video("a.mp4", 12, {1920, 1080}, 30);
	video.start = 1.5;
	video.end = 8.25;
	clips.emplace_back(video);

	auto photo = make_photo("b.jpg", 5, {4000, 3000});
	photo.photo_duration_is_default = false;
	clips.emplace_back(photo);

	movie_settings settings;
	settings.transition = movie_transition::crossfade;
	settings.transition_seconds = 1.5;
	settings.fade_in = false;
	settings.fade_out = true;
	settings.photo_seconds = 7.0;

	const auto json = write_otio(clips, settings, movie_test_folder());

	assert_equal(true, str::contains(json, "\"OTIO_SCHEMA\":\"Timeline.1\""), "the file declares its schema");
	assert_equal(true, str::contains(json, "\"target_url\":\"a.mp4\""),
	             "a source beside the project is stored relative to it");
	assert_equal(true, str::contains(json, "SMPTE_Dissolve"), "the crossfade is written where other tools read it");

	const auto read = read_otio(json, movie_test_folder());

	assert_equal(true, static_cast<bool>(read), "the project reads back");
	assert_equal(2, static_cast<int>(read.clips.size()), "with both clips");
	assert_equal(0, read.ignored_elements, "and nothing dropped");

	assert_equal(video.path.pack(), read.clips[0].path.pack(), "the relative path resolves back");
	assert_equal(1.5, read.clips[0].start, "the in point survives");
	assert_equal(8.25, read.clips[0].end, "the out point survives");
	assert_equal(12.0, read.clips[0].source_duration, "the source length survives");
	assert_equal(false, read.clips[0].is_photo, "a video is still a video");

	assert_equal(true, read.clips[1].is_photo, "a photo is still a photo");
	assert_equal(5.0, read.clips[1].end, "its hold survives");
	assert_equal(false, read.clips[1].photo_duration_is_default, "and so does its override");

	assert_equal(true, read.settings.transition == movie_transition::crossfade, "the transition survives");
	assert_equal(1.5, read.settings.transition_seconds, "its length survives");
	assert_equal(false, read.settings.fade_in, "fade in survives");
	assert_equal(true, read.settings.fade_out, "fade out survives");
	assert_equal(7.0, read.settings.photo_seconds, "the photo default survives");

	// An absolute path is written whole when the source is nowhere near the project, and must still
	// resolve when the project is read from a different folder. It is forward-slashed so the JSON
	// carries no escaped separators.
	const auto elsewhere = write_otio(clips, settings, {});
	assert_equal(true, str::contains(elsewhere, str::replace(video.path.pack(), "\\", "/")),
	             "a distant source is stored absolute");
	assert_equal(false, str::contains(elsewhere, "\\\\"), "and with no escaped separators");
	assert_equal(video.path.pack(), read_otio(elsewhere, {}).clips[0].path.pack(), "and resolves without a root");

	const auto project_folder = movie_test_folder().combine("projects");
	const auto sibling_path = movie_test_folder().combine("media").combine_file("sibling.mp4");
	auto sibling = video;
	sibling.path = sibling_path;
	const auto sibling_json = write_otio({sibling}, settings, project_folder);
	assert_equal(true, str::contains(sibling_json, "../media/sibling.mp4"),
	             "a same-root sibling source is stored relative");
	assert_equal(sibling_path.pack(), read_otio(sibling_json, project_folder).clips[0].path.pack(),
	             "and the sibling relative path resolves back");

	const auto empty_json = write_otio({}, settings, movie_test_folder());
	const auto empty_read = read_otio(empty_json, movie_test_folder());
	assert_equal(true, static_cast<bool>(empty_read), "an empty Diffractor project is a valid project");
	assert_equal(0_z, empty_read.clips.size(), "with no clips");
	assert_equal(true, empty_read.settings.transition == movie_transition::crossfade,
	             "and it keeps the saved settings");
}

// A '%' went into the project as it was and came back through the reader's percent decoding, so
// "50% off.jpg" did not come back at all and "a%20b.jpg" came back naming "a b.jpg" instead.
static void should_round_trip_percent_signs_in_clip_paths()
{
	const std::vector clips{make_photo("50% off.jpg", 5, {4000, 3000}), make_photo("a%20b.jpg", 5, {4000, 3000})};
	const movie_settings settings;

	const auto json = write_otio(clips, settings, movie_test_folder());
	const auto read = read_otio(json, movie_test_folder());
	assert_equal(2_z, read.clips.size(), "both clips come back");
	assert_equal(clips[0].path.pack(), read.clips[0].path.pack(), "a bare percent sign survives");
	assert_equal(clips[1].path.pack(), read.clips[1].path.pack(), "and so does one that reads like an escape");

	// A project written before the sign was escaped still opens: a '%' that begins no escape is the
	// character itself rather than a reason to drop the clip.
	const auto legacy = read_otio(str::replace(json, "50%25", "50%"), movie_test_folder());
	assert_equal(2_z, legacy.clips.size(), "an older project keeps both clips");
	assert_equal(clips[0].path.pack(), legacy.clips[0].path.pack(), "with the name it was given");
}

static void should_read_a_movie_project_it_did_not_write()
{
	// One video track carrying a Gap, beside an audio track. Movie takes the clips it understands
	// and counts the rest; refusing the file over an audio track would make interchange useless.
	const auto path = movie_test_path("a.mp4").pack();
	auto url = path;
	for (auto&& c : url) if (c == '\\') c = '/';

	const auto json = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"tracks": {{
			"OTIO_SCHEMA": "Stack.1",
			"children": [
				{{
					"OTIO_SCHEMA": "Track.1", "kind": "Video",
					"children": [
						{{
							"OTIO_SCHEMA": "Clip.1", "name": "a.mp4",
							"source_range": {{
								"OTIO_SCHEMA": "TimeRange.1",
								"start_time": {{ "OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 24.0 }},
								"duration": {{ "OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 48.0 }}
							}},
							"media_reference": {{
								"OTIO_SCHEMA": "ExternalReference.1", "target_url": "{}"
							}}
						}},
						{{ "OTIO_SCHEMA": "Gap.1", "name": "gap" }},
						{{
							"OTIO_SCHEMA": "Transition.1", "transition_type": "SMPTE_Dissolve",
							"in_offset": {{ "OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 12.0 }},
							"out_offset": {{ "OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 12.0 }}
						}}
					]
				}},
				{{ "OTIO_SCHEMA": "Track.1", "kind": "Audio", "children": [] }}
			]
		}}
	}})", url);

	const auto read = read_otio(json, movie_test_folder());

	assert_equal(true, static_cast<bool>(read), "a foreign project reads");
	assert_equal(1, static_cast<int>(read.clips.size()), "with the clip it understands");
	assert_equal(1.0, read.clips[0].start, "at the frame rate the file used");
	assert_equal(3.0, read.clips[0].end, "and for the duration it stated");
	assert_equal(3, read.ignored_elements, "and the audio track, gap and unusable transition are counted");
	assert_equal(true, read.settings.transition == movie_transition::cut,
	             "a foreign project does not gain transitions it did not define between clips");
	assert_equal(false, read.settings.fade_in, "and does not gain a fade in");
	assert_equal(false, read.settings.fade_out, "or a fade out");

	const auto spaced_path = movie_test_path("clip one.mp4");
	auto spaced_url = str::replace(spaced_path.pack(), "\\", "/");
	spaced_url = str::replace(spaced_url, " ", "%20");
	spaced_url = std::string(df::windows_path_semantics ? "file:///" : "file://") + spaced_url;
	const auto file_uri_json = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [{{
			"OTIO_SCHEMA": "Clip.1",
			"source_range": {{ "start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
			"media_reference": {{ "target_url": "{}" }}
		}}] }}] }}
	}})", spaced_url);
	const auto file_uri_read = read_otio(file_uri_json, movie_test_folder());
	assert_equal(true, static_cast<bool>(file_uri_read), "a foreign file URI reads");
	assert_equal(spaced_path.pack(), file_uri_read.clips[0].path.pack(),
	             "file URI escapes and the local drive form resolve");

	const auto duplicate_transitions = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{
					"start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_reference": {{ "target_url": "{}" }} }},
				{{ "OTIO_SCHEMA": "Transition.1", "transition_type": "SMPTE_Dissolve",
					"in_offset": {{ "rate": 1, "value": 0.5 }}, "out_offset": {{ "rate": 1, "value": 0.5 }} }},
				{{ "OTIO_SCHEMA": "Transition.1", "transition_type": "SMPTE_Dissolve",
					"in_offset": {{ "rate": 1, "value": 0.5 }}, "out_offset": {{ "rate": 1, "value": 0.5 }} }},
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{
					"start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_reference": {{ "target_url": "{}" }} }}
			]
		}}] }}
	}})", url, url);
	const auto duplicate_read = read_otio(duplicate_transitions, movie_test_folder());
	assert_equal(true, duplicate_read.settings.transition == movie_transition::cut,
	             "two dissolves at one boundary do not create transitions at every boundary");
	assert_equal(2, duplicate_read.ignored_elements, "both misplaced dissolves are reported");

	assert_equal(false, static_cast<bool>(read_otio("not json at all", {})), "junk is refused");
	assert_equal(false, static_cast<bool>(read_otio("{}", {})), "an empty object is refused");
	assert_equal(true, read_otio("{}", {}).status == movie_load_status::empty, "and says it held nothing");
}

static void should_import_a_movie_maker_project()
{
	const auto video = movie_test_path("a.mp4").pack();
	const auto image = movie_test_path("b&c.jpg").pack();
	const auto music = movie_test_path("track.mp3").pack();

	// The ampersand is here to prove the attribute decoder: a raw & would leave the name as
	// "b&amp;c.jpg" and the clip would point at a file that cannot exist.
	const auto image_attribute = str::replace(image, "&", "&amp;");

	const auto xml = std::format(R"(<?xml version="1.0" encoding="utf-8"?>
<Project version="9.0">
	<MediaItems>
		<MediaItem id="1" filePath="{}" />
		<MediaItem id="2" filePath="{}" />
		<MediaItem id="3" filePath="{}" />
	</MediaItems>
	<Extents>
		<VideoClip extentID="10" mediaItemID="1" position="6" inTime="2" outTime="7.5" />
		<ImageExtent extentID="11" mediaItemID="2" position="0" duration="6" />
		<AudioClip extentID="12" mediaItemID="3" inTime="0" outTime="60" />
		<TitleClip extentID="13" mediaItemID="2" duration="3" />
		<VideoClip extentID="14" mediaItemID="99" inTime="0" outTime="5" />
		<Effect extentID="15" />
		<Animation extentID="16" />
	</Extents>
</Project>)", video, image_attribute, music);

	const auto read = read_wlmp(xml, movie_test_folder());

	assert_equal(true, static_cast<bool>(read), "the project imports");
	assert_equal(2, static_cast<int>(read.clips.size()), "with the video and the image only");

	assert_equal(image, read.clips[0].path.pack(), "timeline position puts the image first");
	assert_equal(true, read.clips[0].is_photo, "an image extent is a photo");
	assert_equal(6.0, read.clips[0].end, "and is held for its stated duration");
	assert_equal(false, read.clips[0].photo_duration_is_default, "an imported hold is not the default");

	assert_equal(video, read.clips[1].path.pack(), "the later video path resolves");
	assert_equal(false, read.clips[1].is_photo, "a video clip is a video");
	assert_equal(2.0, read.clips[1].start, "its in point is taken");
	assert_equal(7.5, read.clips[1].end, "and its out point");

	// The music, title, missing clip, effect and animation.
	assert_equal(5, read.ignored_elements, "everything it cannot represent is counted");

	auto named = 0;
	for (const auto& [name, count] : read.omitted) named += count;
	assert_equal(read.ignored_elements, named, "and the breakdown names every one of them");

	assert_equal(false, static_cast<bool>(read_wlmp("", movie_test_folder())), "an empty file is refused");
	assert_equal(false, static_cast<bool>(read_wlmp("<Project><Extents /></Project>", movie_test_folder())),
	             "a project with no media is refused");
}

static int omitted_count(const movie_load_result& read, const std::string_view name)
{
	const auto found = std::ranges::find_if(read.omitted, [name](const auto& entry) { return entry.first == name; });
	return found == read.omitted.end() ? 0 : found->second;
}

// Laid out as Movie Maker 2012 writes a project: every extent in one flat list, the timeline in the
// track bound to the "Main" placeholder, and an out point of zero on any clip the user did not
// trim. A reader that took the list's order and wanted a positive out point dropped every
// untrimmed clip and played the rest in the order they were first added.
static void should_import_a_movie_maker_project_as_movie_maker_writes_it()
{
	const auto video = movie_test_path("a.mp4");
	const auto photo = movie_test_path("b.jpg");
	const auto music = movie_test_path("track.mp3");
	const auto unplaced = movie_test_path("d.mp4");

	const auto xml = std::format(R"(<?xml version="1.0" encoding="utf-8"?>
<Project name="Trip" themeId="0" version="65538" templateID="SimpleProjectTemplate">
  <MediaItems>
    <MediaItem id="1" filePath="{}" arWidth="640" arHeight="480" duration="20" />
    <MediaItem id="2" filePath="{}" arWidth="640" arHeight="480" duration="0" />
    <MediaItem id="3" filePath="clips\c.mp4" arWidth="640" arHeight="480" duration="12" />
    <MediaItem id="4" filePath="{}" duration="180" />
    <MediaItem id="5" filePath="{}" duration="30" />
  </MediaItems>
  <Extents>
    <VideoClip extentID="10" gapBefore="0" mediaItemID="1" inTime="0" outTime="0" speed="1">
      <Effects />
      <Transitions />
      <BoundProperties>
        <BoundPropertyBool Name="Mute" Value="false" />
        <BoundPropertyInt Name="rotateStepNinety" Value="0" />
        <BoundPropertyFloat Name="Volume" Value="1" />
      </BoundProperties>
    </VideoClip>
    <ImageClip extentID="11" gapBefore="0" mediaItemID="2" duration="7">
      <Effects>
        <PanAndZoomShapeEffect effectTemplateID="PanAndZoomEffectAutomaticTemplate">
          <BoundProperties><BoundPropertyInt Name="effectType" Value="5" /></BoundProperties>
        </PanAndZoomShapeEffect>
      </Effects>
      <Transitions><ShapeEffect effectTemplateID="CrossfadeTransitionTemplate" duration="1.5"><BoundProperties /></ShapeEffect></Transitions>
      <BoundProperties><BoundPropertyInt Name="rotateStepNinety" Value="2" /></BoundProperties>
    </ImageClip>
    <VideoClip extentID="12" gapBefore="0" mediaItemID="3" inTime="4.5" outTime="0" speed="4">
      <Effects />
      <Transitions><ShapeEffect effectTemplateID="BlurTransitionTemplate" duration="1.5"><BoundProperties /></ShapeEffect></Transitions>
      <BoundProperties>
        <BoundPropertyBool Name="Mute" Value="true" />
        <BoundPropertyFloat Name="Volume" Value="0.5" />
      </BoundProperties>
    </VideoClip>
    <VideoClip extentID="13" gapBefore="0" mediaItemID="5" inTime="0" outTime="0" speed="32">
      <Effects />
      <Transitions />
      <BoundProperties />
    </VideoClip>
    <TitleClip extentID="14" gapBefore="0" duration="5">
      <Effects><TextEffect effectTemplateID="TextEffectStretchTemplate" TextScriptId="1"><BoundProperties /></TextEffect></Effects>
      <Transitions />
      <BoundProperties />
    </TitleClip>
    <AudioClip extentID="15" gapBefore="0" mediaItemID="4" inTime="0" outTime="0">
      <Effects />
      <Transitions />
      <BoundProperties><BoundPropertyFloat Name="Volume" Value="0.5" /></BoundProperties>
    </AudioClip>
    <VideoClip extentID="16" gapBefore="0" mediaItemID="1" inTime="2" outTime="9.25" speed="1">
      <Effects />
      <Transitions />
      <BoundProperties />
    </VideoClip>
    <ExtentSelector extentID="1" gapBefore="0" primaryTrack="true">
      <Effects />
      <Transitions />
      <BoundProperties />
      <ExtentRefs>
        <ExtentRef id="14" />
        <ExtentRef id="16" />
        <ExtentRef id="12" />
        <ExtentRef id="11" />
        <ExtentRef id="10" />
      </ExtentRefs>
    </ExtentSelector>
    <ExtentSelector extentID="2" gapBefore="0" primaryTrack="false">
      <Effects />
      <Transitions />
      <BoundProperties />
      <ExtentRefs><ExtentRef id="15" /></ExtentRefs>
    </ExtentSelector>
    <ExtentSelector extentID="0" gapBefore="0" primaryTrack="false">
      <Effects />
      <Transitions />
      <BoundProperties />
      <ExtentRefs><ExtentRef id="13" /></ExtentRefs>
    </ExtentSelector>
  </Extents>
  <BoundPlaceholders>
    <BoundPlaceholder placeholderID="SingleExtentView" extentID="0" />
    <BoundPlaceholder placeholderID="Main" extentID="1" />
    <BoundPlaceholder placeholderID="SoundTrack" extentID="2" />
  </BoundPlaceholders>
  <BoundProperties>
    <BoundPropertyFloat Name="SoundTrackMix" Value="0" />
  </BoundProperties>
</Project>)", video, photo, music, unplaced);

	const auto read = read_wlmp(xml, movie_test_folder());

	assert_equal(true, static_cast<bool>(read), "the project imports");
	assert_equal(4, static_cast<int>(read.clips.size()), "with the four media clips the main track places");

	const auto beside = movie_test_folder().combine("clips").combine_file("c.mp4");
	assert_equal(video.pack(), read.clips[0].path.pack(), "the main track orders the timeline, not the list");
	assert_equal(beside.pack(), read.clips[1].path.pack(), "a relative path is found beside the project");
	assert_equal(photo.pack(), read.clips[2].path.pack(), "the photo is third on the track");
	assert_equal(video.pack(), read.clips[3].path.pack(), "and the first clip added plays last");

	assert_equal(2.0, read.clips[0].start, "a trimmed clip keeps its in point");
	assert_equal(9.25, read.clips[0].end, "and its out point");

	// End zero is what a newly added video has: the probe gives it the rest of its source.
	assert_equal(4.5, read.clips[1].start, "a clip trimmed only at the start keeps its in point");
	assert_equal(0.0, read.clips[1].end, "and runs to the end of its source");
	assert_equal(0.0, read.clips[3].start, "an untrimmed clip starts at the start");
	assert_equal(0.0, read.clips[3].end, "and is kept rather than dropped");
	assert_equal(false, read.clips[3].is_trimmed(), "and is not shown as trimmed");

	assert_equal(true, read.clips[2].is_photo, "an image clip is a photo");
	assert_equal(7.0, read.clips[2].end, "held for its stated duration");

	assert_equal(1, omitted_count(read, "TitleClip"), "the title on the main track is left out");
	assert_equal(1, omitted_count(read, "AudioClip"), "and so is the music");
	assert_equal(1, omitted_count(read, "clip off the main track"), "a clip the timeline does not place is left out");
	assert_equal(1, omitted_count(read, "speed"), "only a placed clip's speed is counted");
	assert_equal(1, omitted_count(read, "BlurTransitionTemplate"), "a transition Movie cannot draw is counted");
	assert_equal(0, omitted_count(read, "CrossfadeTransitionTemplate"), "a crossfade is Movie's own transition");
	assert_equal(1, omitted_count(read, "PanAndZoomEffectAutomaticTemplate"), "an effect on a clip is counted");
	assert_equal(1, omitted_count(read, "rotateStepNinety"), "and so is a rotation");
	assert_equal(1, omitted_count(read, "Mute"), "a muted clip");
	assert_equal(1, omitted_count(read, "Volume"), "and a changed volume, but not the music's");
	assert_equal(0, omitted_count(read, "Effects"), "an empty effect list is not an element left out");
	assert_equal(0, omitted_count(read, "TextEffectStretchTemplate"), "nor is the inside of a title already counted");
	assert_equal(9, read.ignored_elements, "and the total is the sum of what was named");

	const auto without_folder = read_wlmp(xml, {});
	assert_equal(3, static_cast<int>(without_folder.clips.size()), "a relative path needs the project's folder");
	assert_equal(1, omitted_count(without_folder, "clip with no media file"), "and its clip says why it is gone");
}

// Movie Maker projects name their sources where they were years ago. The batch finds each by name
// and must choose between same-named files the way a person would.
static void should_find_moved_movie_sources()
{
	const auto old_root = movie_test_folder().combine("old");
	const auto park = old_root.combine("Alfie in the park");
	const auto dated = old_root.combine("Pictures").combine("2010").combine("2010-08-28");

	const auto collection = movie_test_folder().combine("collection");
	const auto january = collection.combine("2013").combine("2013-01-16");
	const auto december = collection.combine("2012").combine("2012-12-26");
	const auto august = collection.combine("2010").combine("2010-08-28");
	const auto march = collection.combine("2005").combine("2005-03-26");

	const std::vector<df::file_path> stored{
		park.combine_file("IMG_0737.MOV"),
		park.combine_file("IMG_0738.MOV"),
		park.combine_file("IMG_0754.MOV"),
		dated.combine_file("IMG_4823.JPG"),
		park.combine_file("IMG_0001.MOV"),
		park.combine_file("gone.mov"),
		park.combine_file("IMG_0760.MOV"),
		park.combine_file("IMG_0761.MOV"),
		park.combine_file("IMG_0099.MOV"),
	};

	const std::vector<std::vector<df::file_path>> candidates{
		{january.combine_file("IMG_0737.MOV")},
		{january.combine_file("IMG_0738.MOV")},
		{december.combine_file("IMG_0754.MOV"), january.combine_file("IMG_0754.MOV")},
		{march.combine_file("IMG_4823.JPG"), august.combine_file("IMG_4823.JPG")},
		{march.combine_file("IMG_0001.MOV"), august.combine_file("IMG_0001.MOV")},
		{},
		{january.combine_file("IMG_0760.MOV"), december.combine_file("IMG_0760.MOV")},
		{january.combine_file("IMG_0761.MOV"), december.combine_file("IMG_0761.MOV")},
		{march.combine_file("IMG_0099.MOV")},
	};

	const std::vector<double> recorded{20.0, 20.0, 0.0, 0.0, 0.0, 0.0, 12.5, 12.5, 27.8};

	const df::hash_map<df::file_path, double, df::ihash, df::ieq> lengths{
		{january.combine_file("IMG_0737.MOV"), 20.0},
		{january.combine_file("IMG_0738.MOV"), 20.2},
		{january.combine_file("IMG_0760.MOV"), 30.0},
		{december.combine_file("IMG_0760.MOV"), 12.4},
		{january.combine_file("IMG_0761.MOV"), 12.4},
		{december.combine_file("IMG_0761.MOV"), 12.6},
		{march.combine_file("IMG_0099.MOV"), 116.6},
	};

	std::vector<df::file_path> measured;
	const auto measure = [&measured, &lengths](const df::file_path path)
	{
		measured.emplace_back(path);
		const auto found = lengths.find(path);
		return found == lengths.end() ? 0.0 : found->second;
	};

	const auto chosen = choose_movie_sources(stored, candidates, recorded, measure);
	assert_equal(stored.size(), chosen.size(), "one answer per source");

	assert_equal(true, chosen[0].match == movie_source_match::only_name, "the only file of that name");
	assert_equal(candidates[0][0].pack(), chosen[0].path.pack(), "is the source when its length agrees");

	assert_equal(true, chosen[3].match == movie_source_match::same_folders,
	             "the one in folders named as the stored ones");
	assert_equal(august.combine_file("IMG_4823.JPG").pack(), chosen[3].path.pack(), "beats an older namesake");

	assert_equal(true, chosen[2].match == movie_source_match::siblings,
	             "the one where its neighbours went");
	assert_equal(january.combine_file("IMG_0754.MOV").pack(), chosen[2].path.pack(), "beats one elsewhere");

	assert_equal(true, chosen[6].match == movie_source_match::same_length, "the one as long as the project recorded");
	assert_equal(december.combine_file("IMG_0760.MOV").pack(), chosen[6].path.pack(),
	             "outranks where its neighbours went");

	assert_equal(true, chosen[7].match == movie_source_match::siblings, "a length both share settles nothing");
	assert_equal(january.combine_file("IMG_0761.MOV").pack(), chosen[7].path.pack(), "so the neighbours do");

	assert_equal(true, chosen[8].match == movie_source_match::other_length,
	             "a lone namesake of another length is another file");
	assert_equal(true, chosen[8].path.is_empty(), "and is never used in its place");

	assert_equal(true, chosen[4].match == movie_source_match::ambiguous, "with no reason to prefer one");
	assert_equal(true, chosen[4].path.is_empty(), "nothing is guessed");

	assert_equal(true, chosen[5].match == movie_source_match::missing, "a name found nowhere is missing");
	assert_equal(true, chosen[5].path.is_empty(), "and has no path");

	// A probe per candidate is the expensive part, so only a source with a length to check pays.
	assert_equal(7_z, measured.size(), "only the candidates of a source with a recorded length are measured");

	// Neighbours that went two ways settle nothing.
	const std::vector<df::file_path> split_stored{
		park.combine_file("a.mov"), park.combine_file("b.mov"), park.combine_file("c.mov")
	};
	const std::vector<std::vector<df::file_path>> split_candidates{
		{january.combine_file("a.mov")},
		{december.combine_file("b.mov")},
		{january.combine_file("c.mov"), december.combine_file("c.mov")},
	};

	const auto split = choose_movie_sources(split_stored, split_candidates, {}, {});
	assert_equal(true, split[2].match == movie_source_match::ambiguous, "a tied vote is no answer");
}

// A timeline built from a selection is not an edit the user made: it has nothing to save and nothing
// to undo, and the first change they make is what leaving Movie has to ask about.
static void should_tell_a_seeded_timeline_from_an_edited_one()
{
	movie_project project;
	project.insert_many(0, {make_video("a.mp4", 10, {1920, 1080}, 30), make_video("b.mp4", 10, {1920, 1080}, 30)});
	project.mark_seeded();

	assert_equal(false, project.is_modified(), "seeding is not an edit the user made");
	assert_equal(false, project.can_undo(), "and there is nothing before a seed to undo to");

	project.trim(0, 1.0, 5.0);
	assert_equal(true, project.is_modified(), "a trim is an edit, so the timeline now holds unsaved work");

	project.undo();
	assert_equal(false, project.is_modified(), "undoing back to the seed leaves nothing to lose");

	project.reset({make_video("c.mp4", 10, {1920, 1080}, 30)}, {}, movie_test_path("p.otio"));
	assert_equal(false, project.is_modified(), "a freshly opened project is unmodified");
}

// Re-entering Movie with five videos selected showed the timeline from the first visit, because an
// edited timeline was kept and never asked about until the application closed. Leaving now settles
// unsaved work, so the selection decides what entering shows.
static void should_decide_what_entering_movie_does()
{
	const std::vector first{movie_test_path("a.mp4"), movie_test_path("b.mp4")};
	const std::vector second{movie_test_path("c.mp4"), movie_test_path("d.mp4"), movie_test_path("e.mp4")};
	const std::vector<df::file_path> none;

	movie_project project;
	assert_equal(true, decide_movie_entry(project, first, none) == movie_entry::seed,
	             "an empty timeline is built from the selection");
	assert_equal(true, decide_movie_entry(project, none, first) == movie_entry::seed,
	             "and stays empty when nothing is selected");

	project.insert_many(0, {make_video("a.mp4", 10, {1920, 1080}, 30), make_video("b.mp4", 10, {1920, 1080}, 30)});
	project.mark_seeded();

	assert_equal(true, decide_movie_entry(project, first, first) == movie_entry::resume,
	             "the same selection returns to the timeline left behind");
	assert_equal(true, decide_movie_entry(project, none, first) == movie_entry::resume,
	             "so does entering with no photo or video selected");
	assert_equal(true, decide_movie_entry(project, second, first) == movie_entry::seed,
	             "a different selection starts a new timeline");

	// A saved timeline is the work the user kept, so the selection that made it returns to it - and
	// it is on disk, so another selection can replace it without losing anything.
	project.trim(0, 1, 5);
	project.mark_saved(movie_test_path("holiday.otio"), project.edit_revision());
	assert_equal(true, decide_movie_entry(project, first, first) == movie_entry::resume,
	             "the selection that made a saved timeline returns to it, trims and all");
	assert_equal(true, decide_movie_entry(project, second, first) == movie_entry::seed,
	             "and a different selection replaces it");

	// A project opened in Movie answers to the selection the timeline it replaced was built from.
	project.reset({make_video("p.mp4", 10, {1920, 1080}, 30)}, {}, movie_test_path("p.otio"));
	assert_equal(true, decide_movie_entry(project, first, first) == movie_entry::resume,
	             "an opened project is returned to while the selection is unchanged");

	// Leaving asks about unsaved work, so a timeline still holding some got here another way and
	// replacing it would lose work nobody was asked about.
	project.trim(0, 2, 5);
	assert_equal(true, decide_movie_entry(project, second, first) == movie_entry::resume,
	             "a timeline holding unsaved work is never replaced");

	project.select_all();
	project.remove_selection();
	assert_equal(true, project.is_empty(), "removing every clip can leave an edited empty timeline");
	assert_equal(true, project.is_modified(), "that empty timeline is still unsaved work");
	assert_equal(true, decide_movie_entry(project, second, first) == movie_entry::resume,
	             "so entering again must not reseed over it silently");

	// An empty timeline that was opened or saved is a document like any other: the selection it answers
	// to, or none, returns to it, where it used to be rebuilt from the selection and lose its file.
	movie_project saved_empty;
	saved_empty.reset({}, {}, movie_test_path("empty.otio"));
	assert_equal(true, decide_movie_entry(saved_empty, none, first) == movie_entry::resume,
	             "an opened empty project is returned to when nothing is selected");
	assert_equal(true, decide_movie_entry(saved_empty, first, first) == movie_entry::resume,
	             "and when the selection is the one it answers to");
	assert_equal(true, decide_movie_entry(saved_empty, second, first) == movie_entry::seed,
	             "while another selection builds a new timeline, as it would over any saved one");
}

// The panel's native controls report every notification they raise, including the echo of a value
// the view wrote, and a trim handle can be pressed and let go without moving. None of those is an
// edit: before this, any of them cost an undo step and made leaving ask about unsaved work.
static void should_not_count_a_no_op_as_an_edit()
{
	movie_project project;
	project.insert_many(0, {make_video("a.mp4", 10, {1920, 1080}, 30), make_photo("b.jpg", 4, {4000, 3000})});
	project.mark_seeded();

	project.settings(project.settings());
	assert_equal(false, project.can_undo(), "settings set to what they already are are not an edit");

	project.trim(0, 0, 10);
	assert_equal(false, project.can_undo(), "a trim to the range already kept is not an edit");

	project.replace(1, project.clips()[1]);
	assert_equal(false, project.can_undo(), "replacing a clip with itself is not an edit");

	project.replace_many({{0, project.clips()[0]}, {1, project.clips()[1]}});
	assert_equal(false, project.can_undo(), "nor is relinking every clip to where it already is");
	assert_equal(false, project.is_modified(), "so the timeline holds nothing to save");

	project.trim(1, 0, 4);
	assert_equal(true, project.can_undo(), "a photo given its own length, even its current one, is an edit");
	assert_equal(false, project.clips()[1].photo_duration_is_default, "because it stops following the default");

	auto settings = project.settings();
	settings.fade_in = !settings.fade_in;
	project.settings(settings);
	project.settings(settings);
	project.undo();
	assert_equal(true, settings.fade_in != project.settings().fade_in,
	             "a setting reported twice is one edit, so one undo takes it back");
}

// Undo returns to the state last saved, seeded or opened, and from there leaving has nothing to ask.
// Past that point, or once the history that led back to it is gone, the timeline is a change again.
static void should_undo_back_to_an_unmodified_timeline()
{
	movie_project project;
	project.insert_many(0, {make_video("a.mp4", 10, {1920, 1080}, 30), make_video("b.mp4", 10, {1920, 1080}, 30)});
	project.mark_seeded();

	project.trim(0, 1, 9);
	project.mark_saved(movie_test_path("movie.otio"), project.edit_revision());
	assert_equal(false, project.is_modified(), "a save settles the timeline");

	project.trim(0, 2, 8);
	assert_equal(true, project.is_modified(), "an edit after the save is unsaved work");
	project.undo();
	assert_equal(false, project.is_modified(), "undoing it returns to what was saved");
	project.undo();
	assert_equal(true, project.is_modified(), "undoing past the save is a change from what is on disk");

	// An edit made after undoing past the save replaces the history that led back to it, so the depth
	// the save stood at now holds something else.
	project.trim(1, 3, 7);
	project.trim(1, 4, 6);
	project.undo();
	assert_equal(true, project.is_modified(), "the depth the save stood at no longer holds what was saved");

	// A save overtaken by an edit wrote a state no undo depth stands for.
	movie_project overtaken;
	overtaken.insert_many(0, {make_video("a.mp4", 10, {1920, 1080}, 30)});
	overtaken.mark_seeded();
	const auto stale = overtaken.edit_revision();
	overtaken.trim(0, 1, 9);
	overtaken.mark_saved(movie_test_path("movie.otio"), stale);
	overtaken.undo();
	assert_equal(true, overtaken.is_modified(), "an overtaken save leaves the timeline needing another");

	// An Undo that lands while a save runs can return to the earlier clean point, but the file on disk
	// then holds the state that was undone, so the timeline still differs from it.
	movie_project undone;
	undone.insert_many(0, {make_video("a.mp4", 10, {1920, 1080}, 30)});
	undone.mark_seeded();
	undone.trim(0, 1, 9);
	const auto saving = undone.edit_revision();
	undone.undo();
	assert_equal(false, undone.is_modified(), "undo reaches the seeded state");
	undone.mark_saved(movie_test_path("movie.otio"), saving);
	assert_equal(true, undone.is_modified(), "a save overtaken by undo leaves the timeline unsaved");

	// The bounded stack can drop the clean snapshot itself.
	movie_project deep;
	deep.insert_many(0, {make_video("a.mp4", 100, {1920, 1080}, 30)});
	deep.mark_seeded();

	for (size_t i = 0; i <= movie_project::max_undo; ++i)
	{
		deep.trim(0, static_cast<double>(i % 50) + 1, 99);
	}

	while (deep.can_undo()) deep.undo();
	assert_equal(true, deep.is_modified(), "a clean state that fell off the stack is not claimed after undo");
}

// The probe fills in what it measured through replace_quietly, which the user did not do. A save or
// an open that a probe result overtook used to treat it as an edit: the save left the timeline
// modified and Close -> Save asked again, and Open -> Discard asked a second time.
static void should_not_treat_a_probe_result_as_an_edit_during_save()
{
	movie_project project;
	auto clip = make_video("a.mp4", 10, {1920, 1080}, 30);
	project.insert_many(0, {clip});
	project.mark_seeded();
	project.trim(0, 0, 5);
	assert_equal(true, project.is_modified(), "the user edited the timeline");

	const auto saving = project.edit_revision();
	const auto revision_before = project.revision();

	clip = project.clips()[0];
	clip.is_probed = true;
	clip.is_missing = false;
	clip.extent = {1920, 1080};
	project.replace_quietly(0, clip);

	assert_equal(true, project.revision() != revision_before, "the probe changes the document");
	assert_equal(saving, project.edit_revision(), "but is not an edit the user made");

	project.mark_saved(movie_test_path("movie.otio"), saving);
	assert_equal(false, project.is_modified(), "a save the probe landed during still settles the timeline");

	const auto opening = project.edit_revision();
	project.trim(0, 1, 4);
	assert_equal(true, opening != project.edit_revision(), "a real edit during an open still counts");
}

// One Add or one drop of several files is one action, so one undo takes all of it back.
static void should_add_several_clips_as_one_edit()
{
	movie_project project;
	project.append(make_video("a.mp4", 10, {1920, 1080}, 30));
	project.mark_seeded();

	project.insert_many(1, {
		                    make_video("b.mp4", 10, {1920, 1080}, 30), make_photo("c.jpg", 4, {4000, 3000}),
		                    make_video("d.mp4", 10, {1920, 1080}, 30)
	                    });

	assert_equal(4, static_cast<int>(project.size()), "every clip was inserted");
	assert_equal("b.mp4", project.clips()[1].path.name().sv(), "at the drop point, in order");
	assert_equal("d.mp4", project.clips()[3].path.name().sv(), "with the last after the others");
	assert_equal(1, static_cast<int>(project.current()), "focus lands on the first clip inserted");

	project.undo();
	assert_equal(1, static_cast<int>(project.size()), "one undo takes the whole insertion back");
	assert_equal(false, project.can_undo(), "as one step");

	project.insert_many(0, {});
	assert_equal(false, project.can_undo(), "inserting nothing is not an edit");
}

// A clip nobody has looked for yet and a clip that has been looked for and is gone are one state in
// the document and the opposite thing to the user: only the second is marked on the strip, counted
// in the explainer, and offered a relink.
static void should_tell_an_unprobed_clip_from_a_lost_one()
{
	auto clip = make_video("a.mp4", 10, {1920, 1080}, 30);

	assert_equal(true, clip.is_missing, "a clip starts unresolved");
	assert_equal(false, clip.is_probed, "because nothing has looked for it yet");
	assert_equal(false, clip.is_lost(), "so it is not a clip that is gone");

	clip.is_probed = true;
	assert_equal(true, clip.is_lost(), "a probe that did not find it is what makes it gone");

	clip.is_missing = false;
	assert_equal(false, clip.is_lost(), "and a clip that was found is not");

	// Relinking replaces where the work was aimed and keeps the work: the stored trim survives, and
	// the clip goes back to unprobed so the new source is measured before the trim is clamped to it.
	movie_project project;
	auto trimmed = make_video("b.mp4", 10, {1920, 1080}, 30);
	trimmed.is_probed = true;
	project.append(trimmed);
	project.trim(0, 2, 8);

	auto relinked = project.clips()[0];
	relinked.path = movie_test_path("b-moved.mp4");
	relinked.is_probed = false;
	project.replace(0, relinked);

	assert_equal("b-moved.mp4", project.clips()[0].path.name().sv(), "the relink re-points the clip");
	assert_equal(2.0, project.clips()[0].start, "the in point survives it");
	assert_equal(8.0, project.clips()[0].end, "and so does the out point");
	assert_equal(false, project.clips()[0].is_probed, "and the new source is measured before it is trusted");

	project.undo();
	assert_equal("b.mp4", project.clips()[0].path.name().sv(), "a relink is an edit, so it can be undone");

	auto second = make_video("c.mp4", 10, {1920, 1080}, 30);
	second.is_probed = true;
	project.append(second);

	auto moved_first = project.clips()[0];
	moved_first.path = movie_test_path("b-again.mp4");
	auto moved_second = project.clips()[1];
	moved_second.path = movie_test_path("c-moved.mp4");
	project.replace_many({{0, moved_first}, {1, moved_second}});

	assert_equal("b-again.mp4", project.clips()[0].path.name().sv(), "a batch relink updates the first clip");
	assert_equal("c-moved.mp4", project.clips()[1].path.name().sv(), "and the second clip");
	project.undo();
	assert_equal("b.mp4", project.clips()[0].path.name().sv(), "one undo restores the first clip");
	assert_equal("c.mp4", project.clips()[1].path.name().sv(), "and the second clip together");
}

//
// The render's destination and its cancellation
//

static void should_refuse_an_output_that_names_a_clip()
{
	const std::vector clips{
		make_video("a.mp4", 4, {1920, 1080}, 30),
		make_photo("b.jpg", 3, {4000, 3000}),
	};

	assert_equal(true, movie_output_names_a_clip(clips, movie_test_path("a.mp4")),
	             "an output naming a video clip is refused");
	// On Linux B.JPG and b.jpg are two files, and writing one leaves the other intact.
	assert_equal(df::case_insensitive_path_identity, movie_output_names_a_clip(clips, movie_test_path("B.JPG")),
	             "the comparison folds case where the platform's paths do, as every other destination check does");
	assert_equal(false, movie_output_names_a_clip(clips, movie_test_path("movie.mp4")),
	             "an output naming nothing in the timeline is allowed");
	assert_equal(false, movie_output_names_a_clip(clips, {}),
	             "an empty output names no clip");
	assert_equal(false, movie_output_names_a_clip({}, movie_test_path("a.mp4")),
	             "an empty timeline has no clip to name");
}

static void should_let_cancel_and_publication_decide_each_other()
{
	// The defect this fixes: the render's cancel source doubled as its publication claim, and
	// df::cancel_token's version constructor increments what it is handed. The claim was already 1
	// before a frame was drawn, so Cancel's compare-exchange from 0 did nothing and publication's
	// compare-exchange from 0 failed - every completed render was deleted as cancelled.
	{
		movie_render_control control;
		const auto token = df::cancel_token(control.cancelled);
		assert_equal(false, token.is_cancelled(), "a fresh render is not already cancelled");
		assert_equal(true, control.claim_publication(), "an uncancelled render publishes its output");
	}

	{
		movie_render_control control;
		const auto token = df::cancel_token(control.cancelled);
		control.cancel();
		assert_equal(true, token.is_cancelled(), "cancel reaches the worker's token");
		assert_equal(false, control.claim_publication(), "a cancelled render does not publish");
	}

	{
		// Cancel pressed while the encoder was closing: publication got there first, so the movie the
		// user already has stays, and a second cancel cannot take it away afterwards.
		movie_render_control control;
		assert_equal(true, control.claim_publication(), "publication claims the render");
		control.cancel();
		assert_equal(false, control.claim_publication(), "the claim is one-shot");
	}
}

static std::string movie_test_url(const std::string_view name)
{
	return str::replace(movie_test_path(name).pack(), "\\", "/");
}

// The source range of each clip a written project lays on its track, as (start, duration) seconds.
static std::vector<std::pair<double, double>> otio_clip_ranges(const std::string_view json)
{
	std::vector<std::pair<double, double>> result;

	df::util::json::json_doc doc;
	doc.Parse(json.data(), json.size());
	if (doc.HasParseError() || !doc.IsObject()) return result;

	const auto seconds = [](const auto& time) { return time["value"].GetDouble() / time["rate"].GetDouble(); };

	for (const auto& child : doc["tracks"]["children"][0]["children"].GetArray())
	{
		if (!str::starts(child["OTIO_SCHEMA"].GetString(), "Clip")) continue;
		const auto& range = child["source_range"];
		result.emplace_back(seconds(range["start_time"]), seconds(range["duration"]));
	}

	return result;
}

// Two four-second clips joined by one dissolve, under the document metadata given.
static std::string dissolve_project(const std::string_view metadata, const double in_offset, const double out_offset,
                                    const double second_start)
{
	return std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"metadata": {{ {} }},
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{
					"start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 4 }} }},
					"media_reference": {{ "target_url": "{}" }} }},
				{{ "OTIO_SCHEMA": "Transition.1", "transition_type": "SMPTE_Dissolve",
					"in_offset": {{ "rate": 1, "value": {} }}, "out_offset": {{ "rate": 1, "value": {} }} }},
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{
					"start_time": {{ "rate": 1, "value": {} }}, "duration": {{ "rate": 1, "value": 4 }} }},
					"media_reference": {{ "target_url": "{}" }} }}
			]
		}}] }}
	}})", metadata, movie_test_url("a.mp4"), in_offset, out_offset, second_start, movie_test_url("b.mp4"));
}

// OpenTimelineIO puts a transition across a cut and draws it from the media past each clip's range,
// so a track lasts as long as its clips' ranges added together. Movie overlaps its clips instead.
// Written as whole retained ranges, another tool played every crossfade over footage the user had
// trimmed away, and the movie a transition longer at every join.
static void should_write_transitions_as_otio_reads_them()
{
	auto first = make_video("a.mp4", 10, {1920, 1080}, 30);
	first.start = 1;
	const std::vector clips{first, make_photo("b.jpg", 5, {4000, 3000}), make_video("c.mp4", 8, {1920, 1080}, 30)};

	movie_settings settings;
	settings.transition = movie_transition::crossfade;
	settings.transition_seconds = 1.0;

	const auto json = write_otio(clips, settings, movie_test_folder());
	const auto ranges = otio_clip_ranges(json);

	assert_equal(3_z, ranges.size(), "every clip is on the track");

	auto track_seconds = 0.0;
	for (const auto& range : ranges) track_seconds += range.second;

	assert_near(calc_movie_timing(clips, settings).duration, track_seconds, 1e-9,
	            "the track another tool lays out lasts as long as the movie");
	assert_near(1.0, ranges[0].first, 1e-9, "the first clip starts at its in point");
	assert_near(8.5, ranges[0].second, 1e-9, "and stops half a crossfade before its out point");
	assert_near(0.5, ranges[1].first, 1e-9, "a clip after a crossfade starts half of it in");
	assert_near(4.0, ranges[1].second, 1e-9, "and one between two gives up half of each");
	assert_near(7.5, ranges[2].second, 1e-9, "the last clip runs to its out point");

	const auto read = read_otio(json, movie_test_folder());
	assert_equal(3_z, read.clips.size(), "the project reads back");
	assert_equal(0, read.ignored_elements, "with its crossfades understood as Movie's own");

	for (size_t i = 0; i < clips.size(); ++i)
	{
		assert_near(clips[i].start, read.clips[i].start, 1e-9, "each clip keeps its in point");
		assert_near(clips[i].end, read.clips[i].end, 1e-9, "and its out point");
	}

	// Saved before the file said which convention its ranges follow, a project holds whole retained
	// ranges, and reads as it was written.
	const auto legacy = read_otio(
		dissolve_project(R"("diffractor": { "transition": "crossfade", "transition_seconds": 1.0 })", 0.5, 0.5, 2),
		movie_test_folder());
	assert_equal(2_z, legacy.clips.size(), "an older project reads");
	assert_equal(4.0, legacy.clips[0].end, "with its ranges as written");
	assert_equal(2.0, legacy.clips[1].start, "on both sides of the crossfade");

	// Another tool's dissolve reaches past the cut into both clips, unevenly if it likes. Adopted, it
	// widens each clip by what it reaches into, so the movie is as long as that tool's track.
	const auto foreign = read_otio(dissolve_project({}, 0.25, 0.75, 2), movie_test_folder());
	assert_equal(true, foreign.settings.transition == movie_transition::crossfade, "a dissolve at every cut is adopted");
	assert_near(1.0, foreign.settings.transition_seconds, 1e-9, "at its whole length");
	assert_near(4.75, foreign.clips[0].end, 1e-9, "the outgoing clip runs on past the cut");
	assert_near(1.75, foreign.clips[1].start, 1e-9, "and the incoming one starts before it");
	assert_near(8.0, calc_movie_timing(foreign.clips, foreign.settings).duration, 1e-9,
	            "and the movie is as long as the track");
	assert_equal(0, foreign.ignored_elements, "nothing is left out");

	// A clip the reader cannot use takes nothing else with it: each transition beside it still widens
	// the clip on its own side, and not the clip beyond.
	const auto broken_middle = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"metadata": {{ "diffractor": {{ "version": 2, "transition": "crossfade", "transition_seconds": 1.0 }} }},
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{ "start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 3.5 }} }},
					"media_reference": {{ "target_url": "{}" }} }},
				{{ "OTIO_SCHEMA": "Transition.1", "transition_type": "SMPTE_Dissolve",
					"in_offset": {{ "rate": 1, "value": 0.5 }}, "out_offset": {{ "rate": 1, "value": 0.5 }} }},
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{ "start_time": {{ "rate": 1, "value": 0.5 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_reference": {{ "target_url": "https://example.com/b.mp4" }} }},
				{{ "OTIO_SCHEMA": "Transition.1", "transition_type": "SMPTE_Dissolve",
					"in_offset": {{ "rate": 1, "value": 0.25 }}, "out_offset": {{ "rate": 1, "value": 0.25 }} }},
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{ "start_time": {{ "rate": 1, "value": 1 }}, "duration": {{ "rate": 1, "value": 3 }} }},
					"media_reference": {{ "target_url": "{}" }} }}
			]
		}}] }}
	}})", movie_test_url("a.mp4"), movie_test_url("c.mp4"));

	const auto without_middle = read_otio(broken_middle, movie_test_folder());
	assert_equal(2_z, without_middle.clips.size(), "the clips it can use are read");
	assert_near(4.0, without_middle.clips[0].end, 1e-9, "the clip before keeps the media its transition reached into");
	assert_near(0.75, without_middle.clips[1].start, 1e-9, "and the clip after keeps what its own transition did");
}

// OpenTimelineIO 0.15 and later write Clip.2: a clip names its media in a map of references with the
// key of the one in use. Read only through the older single reference, every clip of a file from a
// current tool had no media, and the project opened empty. A range in timecoded media counts from the
// media's own first frame, and what the file asks for that Movie cannot do is reported.
static void should_read_clips_as_current_otio_writes_them()
{
	const auto json = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
				{{ "OTIO_SCHEMA": "Clip.2",
					"source_range": {{ "start_time": {{ "rate": 24, "value": 86424 }}, "duration": {{ "rate": 24, "value": 48 }} }},
					"media_references": {{ "DEFAULT_MEDIA": {{ "OTIO_SCHEMA": "ExternalReference.1", "target_url": "{}",
						"available_range": {{ "start_time": {{ "rate": 24, "value": 86400 }}, "duration": {{ "rate": 24, "value": 2400 }} }} }} }},
					"active_media_reference_key": "DEFAULT_MEDIA" }},
				{{ "OTIO_SCHEMA": "Clip.2",
					"source_range": {{ "start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_references": {{
						"high": {{ "OTIO_SCHEMA": "ExternalReference.1", "target_url": "{}" }},
						"proxy": {{ "OTIO_SCHEMA": "ExternalReference.1", "target_url": "{}" }} }},
					"active_media_reference_key": "proxy" }},
				{{ "OTIO_SCHEMA": "Clip.2", "enabled": false,
					"source_range": {{ "start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_references": {{ "DEFAULT_MEDIA": {{ "target_url": "{}" }} }},
					"active_media_reference_key": "DEFAULT_MEDIA" }},
				{{ "OTIO_SCHEMA": "Clip.2",
					"source_range": {{ "start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"effects": [{{ "OTIO_SCHEMA": "LinearTimeWarp.1", "time_scalar": 2.0 }}],
					"media_references": {{ "DEFAULT_MEDIA": {{ "target_url": "{}" }} }},
					"active_media_reference_key": "DEFAULT_MEDIA" }},
				{{ "OTIO_SCHEMA": "Clip.2",
					"source_range": {{ "start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_references": {{ "only": {{ "target_url": "{}" }} }},
					"active_media_reference_key": "gone" }}
			]
		}}] }}
	}})", movie_test_url("timecode.mov"), movie_test_url("high.mov"), movie_test_url("proxy.mov"),
	                              movie_test_url("disabled.mov"), movie_test_url("fast.mov"), movie_test_url("only.mov"));

	const auto read = read_otio(json, movie_test_folder());

	assert_equal(true, static_cast<bool>(read), "a project from a current OpenTimelineIO reads");
	assert_equal(4_z, read.clips.size(), "with every enabled clip");
	assert_equal(movie_test_path("timecode.mov").pack(), read.clips[0].path.pack(), "a clip finds its media");
	assert_near(1.0, read.clips[0].start, 1e-9, "a range in timecoded media counts from the media's first frame");
	assert_near(3.0, read.clips[0].end, 1e-9, "for the length it states");
	assert_equal(movie_test_path("proxy.mov").pack(), read.clips[1].path.pack(), "the media in use is the active one");
	assert_equal(movie_test_path("fast.mov").pack(), read.clips[2].path.pack(),
	             "a clip with an effect Movie cannot apply keeps its place");
	assert_equal(movie_test_path("only.mov").pack(), read.clips[3].path.pack(),
	             "a key that names nothing falls back to the media there is");
	assert_equal(1, omitted_count(read, "disabled clip"), "a disabled clip is left out and said so");
	assert_equal(1, omitted_count(read, "LinearTimeWarp.1"), "and so is the effect");
	assert_equal(2, read.ignored_elements, "and nothing else");
}

// A camera's clip is rarely a whole number of milliseconds long. Saved rounded to the millisecond
// beside its exact source length, an untrimmed clip came back a fraction short and showed as trimmed.
static void should_keep_an_untrimmed_clip_untrimmed_through_save()
{
	const auto camera = make_video("camera.mov", 12.3452, {1920, 1080}, 30);
	const auto read = read_otio(write_otio({camera}, {}, movie_test_folder()), movie_test_folder());

	assert_equal(1_z, read.clips.size(), "the clip reads back");
	assert_equal(false, read.clips[0].is_trimmed(), "an untrimmed clip is still untrimmed");
	assert_near(12.3452, read.clips[0].end, 1e-9, "and runs to the end of its source");

	// A project an earlier build saved holds the length rounded; its clip opens untrimmed as well.
	const auto legacy = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"metadata": {{ "diffractor": {{ "transition": "cut" }} }},
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
				{{ "OTIO_SCHEMA": "Clip.1",
					"source_range": {{ "start_time": {{ "rate": 1000, "value": 0 }}, "duration": {{ "rate": 1000, "value": 12345 }} }},
					"media_reference": {{ "target_url": "{}" }},
					"metadata": {{ "diffractor": {{ "photo": false, "source_duration": 12.3452 }} }} }}
			]
		}}] }}
	}})", movie_test_url("camera.mov"));

	const auto legacy_read = read_otio(legacy, movie_test_folder());
	assert_equal(1_z, legacy_read.clips.size(), "an older project reads");
	assert_equal(false, legacy_read.clips[0].is_trimmed(), "and its untrimmed clip opens untrimmed");
}

// A clip saved before its source was measured -- added a moment before, or gone by then -- has no
// length yet, and OpenTimelineIO's null range means the whole of the media. Both were dropped when
// the project was opened; they come back for the probe to measure.
static void should_keep_an_unmeasured_clip_through_save()
{
	const auto pending = make_video("pending.mp4", 0, {}, 0);
	const auto read = read_otio(write_otio({pending, make_photo("b.jpg", 4, {4000, 3000})}, {}, movie_test_folder()),
	                            movie_test_folder());

	assert_equal(2_z, read.clips.size(), "a clip not yet measured is kept");
	assert_equal(0, read.ignored_elements, "and is not reported left out");
	assert_equal(0.0, read.clips[0].duration(), "it still waits for its length");

	movie_probe_result probe;
	probe.path = read.clips[0].path;
	probe.found = true;
	probe.duration = 9.0;
	probe.extent = {1920, 1080};
	assert_equal(9.0, apply_movie_probe(read.clips[0], probe, 4.0).end, "which the probe gives it from the source");

	const auto json = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": null, "media_reference": {{ "target_url": "{}" }} }},
				{{ "OTIO_SCHEMA": "Clip.1", "media_reference": {{ "target_url": "{}" }} }}
			]
		}}] }}
	}})", movie_test_url("whole.mp4"), movie_test_url("still.jpg"));

	const auto foreign = read_otio(json, movie_test_folder());
	assert_equal(2_z, foreign.clips.size(), "a clip with no range is the whole of its media");
	assert_equal(0.0, foreign.clips[0].duration(), "measured by the probe");
	assert_equal(true, foreign.clips[1].is_photo, "a still with no range is kept");
	assert_equal(foreign.settings.photo_seconds, foreign.clips[1].end, "and held for the default length");
	assert_equal(true, foreign.clips[1].photo_duration_is_default, "which it follows");
}

// A file URI escapes a name byte by byte, and an older tool escaped Latin-1 rather than UTF-8, so
// "caf%E9.mp4" decodes to a byte that is no UTF-8 character. Converting that through the process code
// page threw, and Save -- or leaving Movie and choosing Save -- ended the application.
static void should_save_clip_paths_that_are_not_utf8()
{
	const auto odd_path = movie_test_folder().combine_file("caf\xE9.mp4");
	auto odd = make_video("x.mp4", 4, {1920, 1080}, 30);
	odd.path = odd_path;

	const auto json = write_otio({odd, make_video("plain.mp4", 4, {1920, 1080}, 30)}, {}, movie_test_folder());
	const auto read = read_otio(json, movie_test_folder());
	assert_equal(2_z, read.clips.size(), "both clips save and read back");
	assert_equal(odd_path.pack(), read.clips[0].path.pack(), "with the name as it was read");

	const auto relative = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{ "start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_reference": {{ "target_url": "caf%E9.mp4" }} }},
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{ "start_time": {{ "rate": 1, "value": 0 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_reference": {{ "target_url": "plain.mp4" }} }}
			]
		}}] }}
	}})");

	const auto relative_read = read_otio(relative, movie_test_folder());
	assert_equal(true, static_cast<bool>(relative_read), "a project naming such a file beside it still opens");
	assert_equal(movie_test_path("plain.mp4").pack(), relative_read.clips.back().path.pack(),
	             "with the clips it can name");
}

// A project file is untrusted input. An in point no source can have became an infinity the writer
// could not express: the key went out with no value, Save reported success, and the project no longer
// opened.
static void should_write_a_project_that_reads_back_whatever_it_holds()
{
	const auto json = std::format(R"({{
		"OTIO_SCHEMA": "Timeline.1",
		"tracks": {{ "OTIO_SCHEMA": "Stack.1", "children": [{{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
				{{ "OTIO_SCHEMA": "Clip.1", "source_range": {{ "start_time": {{ "rate": 1, "value": 1e306 }}, "duration": {{ "rate": 1, "value": 2 }} }},
					"media_reference": {{ "target_url": "{}" }} }}
			]
		}}] }}
	}})", movie_test_url("late.mp4"));

	const auto late = read_otio(json, movie_test_folder());
	assert_equal(1_z, late.clips.size(), "a clip starting impossibly late is read");
	assert_equal(true, late.clips[0].start <= movie_max_clip_seconds, "with its in point bounded");
	assert_equal(true, static_cast<bool>(read_otio(write_otio(late.clips, late.settings, movie_test_folder()),
	                                               movie_test_folder())), "and saved as a project that opens");

	auto broken = make_video("broken.mp4", 10, {1920, 1080}, 30);
	broken.start = std::numeric_limits<double>::infinity();
	broken.source_duration = std::numeric_limits<double>::quiet_NaN();
	assert_equal(true, static_cast<bool>(read_otio(write_otio({broken}, {}, movie_test_folder()), movie_test_folder())),
	             "what Save writes always reads back");

	const auto xml = std::format(R"(<Project>
  <MediaItems><MediaItem id="1" filePath="{}" duration="1e308" /></MediaItems>
  <Extents><VideoClip extentID="10" mediaItemID="1" inTime="1e306" outTime="0" /></Extents>
</Project>)", movie_test_path("long.mp4"));

	const auto wlmp = read_wlmp(xml, movie_test_folder());
	assert_equal(1_z, wlmp.recorded_seconds.size(), "a Movie Maker clip is read");
	assert_equal(true, wlmp.recorded_seconds[0] <= movie_max_clip_seconds, "with the length it recorded bounded");
	assert_equal(true, wlmp.clips[0].start <= movie_max_clip_seconds, "and its in point too");
}

// A Movie Maker project is untrusted input too. A path of ampersands made the entity decoder look to
// the end of the text from every one of them, and a clip carrying thousands of distinct effects made
// the report search its whole list for each, so a crafted file held the import for hours.
static void should_bound_what_a_movie_maker_project_costs()
{
	const std::string ampersands(20000, '&');
	std::string effects;

	for (auto i = 0; i < 200; ++i)
	{
		effects += std::format(R"(<ShapeEffect effectTemplateID="Effect{}" />)", i);
	}

	const auto xml = std::format(R"(<Project>
  <MediaItems><MediaItem id="1" filePath="{}" duration="4" /></MediaItems>
  <Extents>
    <VideoClip extentID="10" mediaItemID="1" inTime="0" outTime="4"><Effects>{}</Effects></VideoClip>
  </Extents>
</Project>)", movie_test_path(ampersands + "x.mp4"), effects);

	const auto read = read_wlmp(xml, movie_test_folder());
	assert_equal(1_z, read.clips.size(), "the clip is read");
	assert_equal(ampersands + "x.mp4", std::string(read.clips[0].path.name()), "bare ampersands are kept as they are");
	assert_equal(200, read.ignored_elements, "every effect left out is counted");
	assert_equal(true, read.omitted.size() <= 33, "but the report names a bounded number of kinds");
	assert_equal(1, omitted_count(read, "Effect0"), "naming the first it met");
}

// Dragging a slider reports every value it passes. Each was an undo step, so Undo walked a drag back one
// value at a time, a long drag pushed the saved state out of the history, and a drag that ended where it
// began still left the timeline asking to be saved.
static void should_merge_one_gesture_into_one_undo_step()
{
	movie_project project;
	project.insert_many(0, {make_video("a.mp4", 10, {1920, 1080}, 30), make_photo("b.jpg", 4, {4000, 3000})});
	project.mark_seeded();
	const auto before = project.settings();

	auto settings = before;

	for (auto tenths = 11; tenths <= 200; ++tenths)
	{
		settings.transition_seconds = tenths / 10.0;
		project.settings(settings, movie_edit_group::transition_length);
	}

	assert_equal(true, project.is_modified(), "a drag is an edit");
	project.undo();
	assert_equal(true, project.settings() == before, "one undo takes the whole drag back");
	assert_equal(false, project.can_undo(), "in one step");
	assert_equal(false, project.is_modified(), "to the state it started from");

	settings = before;
	settings.photo_seconds = 9;
	project.settings(settings, movie_edit_group::photo_duration);
	project.settings(before, movie_edit_group::photo_duration);
	assert_equal(false, project.is_modified(), "a drag that ends where it began is no edit");
	assert_equal(false, project.can_undo(), "and costs no undo step");

	// A settled drag is a step of its own, and so is the next.
	settings.photo_seconds = 6;
	project.settings(settings, movie_edit_group::photo_duration);
	project.end_edit_group();
	settings.photo_seconds = 7;
	project.settings(settings, movie_edit_group::photo_duration);
	project.undo();
	assert_equal(6.0, project.settings().photo_seconds, "one settled drag is not merged into the next");

	// Any other edit closes the gesture.
	project.trim(1, 0, 3, movie_edit_group::clip_hold);
	project.trim(1, 0, 5, movie_edit_group::clip_hold);
	project.trim(0, 1, 9);
	project.undo();
	project.undo();
	assert_equal(true, project.clips()[1].photo_duration_is_default, "the held photo follows the default again");

	// So does a save: what comes after it is a change from what is on disk.
	settings.photo_seconds = 8;
	project.settings(settings, movie_edit_group::photo_duration);
	project.mark_saved(movie_test_path("movie.otio"), project.edit_revision());
	settings.photo_seconds = 5;
	project.settings(settings, movie_edit_group::photo_duration);
	project.undo();
	assert_equal(8.0, project.settings().photo_seconds, "undo returns to what was saved");
	assert_equal(false, project.is_modified(), "which leaves nothing to save");
}

// The panel shows lengths in tenths and takes whatever is typed into it. A length it is still showing is
// the document's own and is kept exactly; a typed one is held to the document's bounds.
static void should_hold_panel_settings_to_the_document()
{
	movie_view_state state;
	auto settings = state.project.settings();
	settings.transition_seconds = 0.75;
	state.project.reset({}, settings, {});
	state.read_from_project();

	assert_equal(0.75, state.to_settings().transition_seconds, "a length shown rounded keeps its exact value");
	assert_equal(true, state.to_settings() == state.project.settings(), "so choosing a setting again changes nothing");

	state.transition_tenths = 150;
	assert_equal(movie_max_transition_seconds, state.to_settings().transition_seconds, "a typed length is bounded");
	state.photo_tenths = 0;
	assert_equal(movie_min_photo_seconds, state.to_settings().photo_seconds, "so is a typed photo length");
	state.photo_tenths = 100000;
	assert_equal(movie_max_photo_seconds, state.to_settings().photo_seconds, "at both ends");
}

void register_movie_tests(view_state& state, test_registry& tests)
{
	//
	// Output
	//
	tests.add("Should derive movie output geometry"s, should_derive_movie_output);
	tests.add("Should refuse a movie output that names a clip"s, should_refuse_an_output_that_names_a_clip);
	tests.add("Should let movie cancel and publication decide each other"s,
	          should_let_cancel_and_publication_decide_each_other);

	//
	// Timing
	//
	tests.add("Should time movie transitions"s, should_time_movie_transitions);
	tests.add("Should compose a movie frame"s, should_compose_a_movie_frame);

	//
	// The timeline document
	//
	tests.add("Should edit the movie timeline"s, should_edit_the_movie_timeline);
	tests.add("Should render only a resolved movie"s, should_render_only_a_resolved_movie);
	// TEST-013 - a no-op movie move must not consume an undo step.
	tests.add("Should select and move movie clips"s, should_select_and_move_movie_clips);
	tests.add("Should trim movie clips"s, should_trim_movie_clips);
	tests.add("Should tell a seeded movie timeline from an edited one"s,
	          should_tell_a_seeded_timeline_from_an_edited_one);
	tests.add("Should not count a movie no-op as an edit"s, should_not_count_a_no_op_as_an_edit);
	tests.add("Should undo back to an unmodified movie timeline"s, should_undo_back_to_an_unmodified_timeline);
	tests.add("Should not treat a movie probe result as an edit during save"s,
	          should_not_treat_a_probe_result_as_an_edit_during_save);
	tests.add("Should add several movie clips as one edit"s, should_add_several_clips_as_one_edit);
	tests.add("Should tell an unprobed movie clip from a lost one"s,
	          should_tell_an_unprobed_clip_from_a_lost_one);
	tests.add("Should merge one movie gesture into one undo step"s, should_merge_one_gesture_into_one_undo_step);
	tests.add("Should hold movie panel settings to the document"s, should_hold_panel_settings_to_the_document);

	//
	// Entering Movie
	//
	// VIEW-015 - an edited empty Movie document is still unsaved work.
	tests.add("Should decide what entering movie does"s, should_decide_what_entering_movie_does);

	//
	// The project file
	//
	// VIEW-015 - Save/Open support a valid empty Movie project.
	tests.add("Should round trip a movie project"s, should_round_trip_a_movie_project);
	tests.add("Should round trip percent signs in clip paths"s, should_round_trip_percent_signs_in_clip_paths);
	tests.add("Should bound movie project durations"s, should_bound_movie_project_durations);
	tests.add("Should refuse a deeply nested movie project"s, should_refuse_a_deeply_nested_movie_project);
	tests.add("Should read a movie project it did not write"s, should_read_a_movie_project_it_did_not_write);
	tests.add("Should write movie transitions as OTIO reads them"s, should_write_transitions_as_otio_reads_them);
	tests.add("Should read movie clips as current OTIO writes them"s, should_read_clips_as_current_otio_writes_them);
	tests.add("Should keep an untrimmed movie clip untrimmed through save"s,
	          should_keep_an_untrimmed_clip_untrimmed_through_save);
	tests.add("Should keep an unmeasured movie clip through save"s, should_keep_an_unmeasured_clip_through_save);
	tests.add("Should save movie clip paths that are not UTF-8"s, should_save_clip_paths_that_are_not_utf8);
	tests.add("Should write a movie project that reads back whatever it holds"s,
	          should_write_a_project_that_reads_back_whatever_it_holds);
	tests.add("Should import a movie maker project"s, should_import_a_movie_maker_project);
	tests.add("Should import a movie maker project as movie maker writes it"s,
	          should_import_a_movie_maker_project_as_movie_maker_writes_it);
	tests.add("Should bound what a Movie Maker project costs"s, should_bound_what_a_movie_maker_project_costs);
	tests.add("Should find moved movie sources"s, should_find_moved_movie_sources);
}
