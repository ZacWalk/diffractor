// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tests for the Movie document (model_movie) -- the derived output geometry, crossfade and duration
// arithmetic, when captions show and how loud each sound plays, timeline edits and undo, what entering Movie
// does with the timeline it holds, and the .dfv project file. docs/movie.md owns the behaviour.

#include "pch.h"
#include "test.h"
#include "files.h"
#include "model.h"
#include "model_movie.h"
#include "test_runner.h"
#include "view_movie.h"

static core::folder_path movie_test_folder()
{
	return core::folder_path(core::windows_path_semantics ? "c:\\movie" : "/movie");
}

static core::file_path movie_test_path(const std::string_view name)
{
	return movie_test_folder().combine_file(name);
}

static movie_clip make_video(const std::string_view name, const double duration, const sizei extent, const double rate)
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

static movie_clip make_hd(const std::string_view name, const double duration)
{
	return make_video(name, duration, {1920, 1080}, 30);
}

//
// Output geometry
//

static void should_derive_movie_output()
{
	assert_equal(1920, derive_movie_output({}).extent.cx, "an empty timeline falls back to 1080p");
	assert_equal(movie_default_frame_rate, derive_movie_output({}).frame_rate, "and to the default rate");

	const auto mixed = derive_movie_output(
		{make_video("a.mp4", 10, {1280, 720}, 60), make_video("b.mp4", 10, {1920, 1080}, 30)});

	assert_equal(1920, mixed.extent.cx, "the largest video decides the width");
	assert_equal(1080, mixed.extent.cy, "the largest video decides the height");
	assert_equal(30, mixed.frame_rate, "60 fps footage plays in a 30 fps movie");
	assert_equal(6220800, mixed.video_bitrate, "bitrate follows frame size and rate");

	// The rule that matters most: a 48-megapixel photo beside a 720p clip must not produce an
	// 8000-pixel movie. Removing the have_video test makes this line fail with 8000.
	const auto with_photo = derive_movie_output(
		{make_photo("big.jpg", 4, {8000, 6000}), make_video("small.mp4", 10, {1280, 720}, 25)});

	assert_equal(1280, with_photo.extent.cx, "a photo never drives the frame size when a video is present");
	assert_equal(25, with_photo.frame_rate, "the only video decides the rate");

	const auto photos_only = derive_movie_output({make_photo("big.jpg", 4, {8000, 6000})});

	assert_equal(1440, photos_only.extent.cx, "a photo-only timeline is clamped inside HD");
	assert_equal(1080, photos_only.extent.cy, "and keeps its aspect ratio while clamped");
	assert_equal(movie_default_frame_rate, photos_only.frame_rate, "with no video there is no source rate");

	// A movie is HD at most, so 4K clips among HD ones are scaled down rather than making every frame 4K.
	const auto uhd = derive_movie_output({make_hd("a.mp4", 10), make_video("uhd.mp4", 10, {3840, 2160}, 60)});
	assert_equal(true, uhd.extent == sizei{1920, 1080}, "4K sources make an HD movie");
	assert_equal(6220800, uhd.video_bitrate, "at the bitrate HD takes");

	const auto standing = derive_movie_output({make_video("phone.mp4", 10, {2160, 3840}, 30)});
	assert_equal(true, standing.extent == sizei{1080, 1920}, "and a portrait one is HD standing up");

	// The frame takes the shape most of the movie is in, so one larger 4:3 clip does not put black bars
	// round every 16:9 one, nor one portrait clip at the start stand the whole movie up.
	const auto mostly_wide = derive_movie_output(
		{make_hd("a.mp4", 20), make_video("gopro.mp4", 2, {1920, 1440}, 60), make_hd("b.mp4", 20)});
	assert_equal(true, mostly_wide.extent == sizei{1920, 1080}, "the shape most of the movie is in decides the frame");
	assert_equal(30, mostly_wide.frame_rate, "as most of it decides the rate");

	const auto upright_first = derive_movie_output(
		{make_video("phone.mp4", 3, {1080, 1920}, 30), make_hd("a.mp4", 10)});
	assert_equal(true, upright_first.extent == sizei{1920, 1080}, "a portrait clip first does not stand a movie up");

	const auto upright = derive_movie_output({make_video("phone.mp4", 10, {1080, 1920}, 30), make_hd("a.mp4", 3)});
	assert_equal(true, upright.extent == sizei{1080, 1920}, "but a movie mostly upright stands");

	const auto odd = derive_movie_output({make_video("odd.mp4", 10, {1279, 719}, 30)});

	assert_equal(1278, odd.extent.cx, "an odd width is rounded down");
	assert_equal(718, odd.extent.cy, "an odd height is rounded down");

	const auto fast = derive_movie_output({make_video("fast.mp4", 10, {1920, 1080}, 240)});
	assert_equal(movie_max_frame_rate, fast.frame_rate, "an extreme source rate plays at a rate a movie is made at");

	// A movie is made at 24, 25 or 30 fps: whichever most of its video was filmed for, by how long each clip
	// shows, so a camera's 25 fps footage is not shown with a frame repeated five times a second.
	const auto pal = derive_movie_output({make_hd("phone.mp4", 5), make_video("camera.mp4", 20, {1920, 1080}, 25)});
	assert_equal(25, pal.frame_rate, "a movie filmed mostly at 25 fps is made at 25");
	assert_equal(25, derive_movie_output({make_video("pal.mp4", 10, {1280, 720}, 50)}).frame_rate,
	             "and 50 fps footage plays at 25");
	assert_equal(24, derive_movie_output({make_video("film.mp4", 10, {1920, 1080}, 23.976)}).frame_rate, "film at 24");
	assert_equal(30,
	             derive_movie_output(
					 {make_video("ntsc.mp4", 10, {1920, 1080}, 29.97), make_video("pal.mp4", 10, {1920, 1080}, 25)})
	                 .frame_rate,
	             "and an even split at 30");

	// A phone's variable-rate video declares 120 while its frames come 30 a second. Counted at 120, one such
	// clip would have every frame of the movie made twice over.
	assert_equal(30.02, movie_source_frame_rate(120, 30.02),
	             "a variable-rate video counts at the rate its frames come");
	assert_equal(59.94, movie_source_frame_rate(59.94, 60), "one as fast as it declares keeps its rate");
	assert_equal(25.0, movie_source_frame_rate(0, 25), "and one that declares none is taken at its average");

	const auto paged = derive_movie_output({make_movie_text("Trip"), make_video("portrait.mp4", 10, {1080, 1920}, 30)});
	assert_equal(1080, paged.extent.cx, "a text screen has no size of its own: the clips make the frame");
	assert_equal(true, derive_movie_output({make_movie_text("Trip")}).extent == sizei{1920, 1080},
	             "and a movie of text screens alone is drawn in 1080p");
}

//
// Timing
//

static void should_time_movie_crossfades()
{
	const std::vector clips{make_hd("a.mp4", 10), make_hd("b.mp4", 10), make_hd("c.mp4", 10)};

	const auto timing = calc_movie_timing(clips);
	assert_equal(28.0, timing.duration, "each crossfade shortens the movie by its length");
	assert_equal(0.0, timing.starts[0], "the first clip starts at zero");
	assert_equal(9.0, timing.starts[1], "the second clip starts a crossfade early");
	assert_equal(18.0, timing.starts[2], "and the third two");

	// A crossfade longer than half the item it joins must shorten rather than consume it. Dropping the
	// half-item ceiling in movie_overlap makes this 19, and the middle clip disappears.
	const std::vector short_middle{make_hd("a.mp4", 10), make_hd("b.mp4", 1), make_hd("c.mp4", 10)};

	const auto clamped = calc_movie_timing(short_middle);
	assert_equal(0.5, movie_overlap(short_middle[0], short_middle[1]),
	             "the overlap is capped at half the shorter item");
	assert_equal(20.0, clamped.duration, "a one second clip survives the crossfades on both its sides");
	assert_equal(10.0, clamped.starts[2], "and still separates its neighbours");

	// A text screen is an item like a clip: the clips beside it crossfade out of and into it.
	const auto paged = calc_movie_timing(
		{make_movie_text("Trip", 5), make_hd("a.mp4", 10), make_movie_text("The End", 4)});
	assert_equal(4.0, paged.starts[1], "a clip comes in out of the screen before it");
	assert_equal(13.0, paged.starts[2], "and goes out into the screen after it");
	assert_equal(17.0, paged.duration, "with each crossfade as long as any other");

	assert_equal(0.0, calc_movie_timing({}).duration, "an empty timeline has no duration");
	assert_equal(10.0, calc_movie_timing({clips[0]}).duration, "one item has nothing to crossfade with");
}

static void should_compose_a_movie_frame()
{
	const std::vector clips{make_hd("a.mp4", 10), make_hd("b.mp4", 10)};

	// The movie is 19s: 0-9 is a alone, 9-10 is the crossfade, 10-19 is b alone.
	const auto early = calc_movie_frame(clips, 4.0);
	assert_equal(0, early.a.index, "one clip carries the frame outside a crossfade");
	assert_equal(4.0, early.a.source_time, "at its own position in the source");
	assert_equal(1.0, early.a.weight, "at full weight");
	assert_equal(-1, early.b.index, "with nothing mixed into it");

	const auto middle = calc_movie_frame(clips, 9.5);
	assert_equal(0, middle.a.index, "the outgoing clip is first");
	assert_equal(1, middle.b.index, "the incoming clip is second");
	assert_equal(0.5, middle.a.weight, "halfway through the crossfade they are equal");
	assert_equal(0.5, middle.b.weight, "on both sides");
	assert_equal(9.5, middle.a.source_time, "the outgoing clip is near its end");
	assert_equal(0.5, middle.b.source_time, "and the incoming one just past its start");

	const auto late = calc_movie_frame(clips, 14.0);
	assert_equal(1, late.a.index, "after the crossfade only the incoming clip is left");
	assert_equal(5.0, late.a.source_time, "at its own position");
	assert_equal(-1, late.b.index, "with nothing mixed into it");

	// The final instant must still draw. An exclusive end test here leaves the last frame empty,
	// which is one black frame at the end of every movie.
	assert_equal(1, calc_movie_frame(clips, 19.0).a.index, "the final instant still draws a clip");
	assert_equal(true, calc_movie_frame({}, 0).is_empty(), "an empty timeline draws nothing");

	// A trimmed clip reports positions in its source, not on the timeline.
	auto trimmed_clips = clips;
	trimmed_clips[0].start = 3;
	trimmed_clips[0].end = 9;
	assert_equal(5.0, calc_movie_frame(trimmed_clips, 2.0).a.source_time,
	             "a trim offsets the position asked of the source");

	assert_equal(0.0, calc_movie_frame({make_photo("p.jpg", 5, {4000, 3000})}, 3.0).a.source_time,
	             "a photo has one frame however long it is held");

	const auto out_of_screen = calc_movie_frame({make_movie_text("Trip", 5), make_hd("a.mp4", 10)}, 4.5);
	assert_equal(0, out_of_screen.a.index, "a text screen is a picture the next clip crossfades out of");
	assert_equal(0.5, out_of_screen.a.weight, "at the weight any outgoing item has");
	assert_equal(0.0, out_of_screen.a.source_time, "and, like a photo, one picture however long it shows");

	assert_equal(1.0, calc_movie_frame(clips, 0.0).fade_to_black, "the movie opens black");
	assert_equal(0.5, calc_movie_frame(clips, 0.5).fade_to_black, "and fades up over a crossfade's length");
	assert_equal(0.0, calc_movie_frame(clips, 5.0).fade_to_black, "and is clear in the middle");
	assert_equal(0.5, calc_movie_frame(clips, 18.5).fade_to_black, "then fades down");
	assert_equal(1.0, calc_movie_frame(clips, 19.0).fade_to_black, "and closes black");
}

// A caption shows for as long as it takes to read, once its clip has come in, and runs on over the clips
// after it if its own is short -- but never on to a text screen, and never under the next caption.
static void should_show_movie_captions_for_as_long_as_they_take_to_read()
{
	assert_equal(movie_caption_min_seconds, movie_caption_seconds("Lukla"), "a place's name shows for the least time");
	assert_equal(6.0, movie_caption_seconds(std::string(60, 'x')),
	             "a sentence for a moment more than it takes to read at fifteen characters a second");
	assert_equal(movie_caption_max_seconds, movie_caption_seconds(std::string(600, 'x')),
	             "and a paragraph for no longer than the most a caption shows");

	std::string accented;
	for (auto i = 0; i < 45; ++i) accented += "\xC5\x88";
	assert_equal(5.0, movie_caption_seconds(accented), "counting characters, not the bytes that spell them");

	auto clips = std::vector{make_hd("a.mp4", 10), make_hd("b.mp4", 10)};
	clips[0].text = "Kathmandu";
	clips[1].text = "Lukla";

	const auto timing = calc_movie_timing(clips);
	assert_equal(2_z, timing.captions.size(), "each caption has a span");
	assert_equal(0.0, timing.captions[0].start, "the first clip's from the movie's start");
	assert_equal(3.0, timing.captions[0].end, "for as long as it takes to read");
	assert_equal(10.0, timing.captions[1].start, "a later one once its clip has crossfaded in");
	assert_equal(13.0, timing.captions[1].end, "and for its own reading time");

	const auto shown = calc_movie_frame(clips, 1.5);
	assert_equal(0, shown.caption.index, "the caption showing names its clip");
	assert_equal(1.0, shown.caption.opacity, "and is shown in full once it has faded in");
	assert_equal(0.5, calc_movie_frame(clips, 0.25).caption.opacity, "having faded in over its first half second");
	assert_equal(false, calc_movie_frame(clips, 5.0).caption.is_shown(), "and is gone once read");

	// A long caption on a short clip runs on over the clip after it, and gives way to a text screen.
	auto running = std::vector{make_hd("a.mp4", 2), make_hd("b.mp4", 10), make_movie_text("The End", 3)};
	running[0].text = std::string(150, 'x');

	const auto ran = calc_movie_timing(running);
	assert_equal(1_z, ran.captions.size(), "the screen's words are no caption");
	assert_equal(10.0, ran.captions[0].end, "a caption runs over the next clip and stops where a screen begins");

	running[1].text = "Lukla";
	assert_equal(2.0, calc_movie_timing(running).captions[0].end,
	             "and gives way to the next caption once that one's clip has come in");

	running[0].text.clear();
	assert_equal(1_z, calc_movie_timing(running).captions.size(), "a clip with no words has no caption");
}

// Normally the sound going out fades as the picture does. A clip going out under an item with no sound
// of its own keeps its sound at full volume, so what someone was saying as the crossfade began is heard.
static void should_carry_a_clips_sound_over_an_item_with_none()
{
	const auto talking = make_hd("talking.mp4", 10);

	const std::vector onto_photo{talking, make_photo("view.jpg", 4, {4000, 3000})};
	const auto carried = calc_movie_frame(onto_photo, 9.5);
	assert_equal(0.5, carried.a.weight, "the picture crossfades into a photo as usual");
	assert_equal(1.0, carried.a.sound, "while the sound going out stays up");
	assert_near(0.4, calc_movie_frame(onto_photo, 9.98).a.sound, 1e-9, "and lets go over the crossfade's last moment");

	assert_equal(1.0, calc_movie_frame({talking, make_movie_text("The End", 4)}, 9.5).a.sound,
	             "it stays up under a text screen too");

	auto gone = make_hd("gone.mp4", 10);
	gone.is_probed = true;
	assert_equal(1.0, calc_movie_frame({talking, gone}, 9.5).a.sound, "and over a source that is gone");

	auto view = make_hd("view.mp4", 10);
	assert_equal(0.5, calc_movie_frame({talking, view}, 9.5).a.sound,
	             "a clip coming in whose sound is not known crossfades with the one going out");

	view.sound_checked_at = view.start;
	view.sound_is_quiet = true;
	const auto onto_quiet = calc_movie_frame({talking, view}, 9.5);
	assert_equal(1.0, onto_quiet.a.sound, "one nearly silent where it comes in counts as having no sound");
	assert_equal(0.5, onto_quiet.b.sound, "and its own sound still fades up");

	view.start = 2;
	assert_equal(0.5, calc_movie_frame({talking, view}, 9.5).a.sound,
	             "a measure made at another in point is no longer known");

	const auto ending = calc_movie_frame({talking}, 9.5);
	assert_equal(0.5, movie_sound_level(ending, 0), "the movie's own fade takes the sound down with the picture");
	assert_equal(0.0, movie_sound_level(ending, 1), "and an item not playing makes none");

	assert_equal(true, movie_sound_is_quiet({}), "no samples at all are silence");
	assert_equal(true, movie_sound_is_quiet(std::vector<int16_t>(9600, 100)), "a camera's hiss is nearly silent");
	assert_equal(false, movie_sound_is_quiet(std::vector<int16_t>(9600, 3000)), "a voice is not");

	// Only the opening a crossfade would mix is heard: a clip silent for its first second is quiet,
	// whatever it says after.
	std::vector<int16_t> speaks_later(48000 * 2 * 3, 0);
	std::fill(speaks_later.begin() + 48000 * 2, speaks_later.end(), int16_t{3000});
	assert_equal(true, movie_opening_is_quiet(speaks_later, 48000), "a clip that speaks a second in opens quiet");
	assert_equal(false, movie_opening_is_quiet(std::vector<int16_t>(48000 * 2, 3000), 48000),
	             "and one that speaks at once does not");
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

	project.append(make_hd("a.mp4", 10));
	project.append(make_hd("b.mp4", 10));
	project.append(make_photo("c.jpg", 4, {4000, 3000}));

	assert_equal(3, static_cast<int>(project.size()), "three clips were appended");
	assert_equal(2, static_cast<int>(project.current()), "the last append is current");
	assert_equal(true, project.is_modified(), "editing marks the project modified");

	// The same source twice is the case a set-based selector could not express, and the reason the
	// timeline is a document rather than a view of the selection.
	project.append(make_hd("a.mp4", 10));
	assert_equal(4, static_cast<int>(project.size()), "the same source can appear twice");
	assert_equal("a.mp4", project.clips()[3].path.name().sv(), "and it is the same source");

	project.insert(0, make_hd("first.mp4", 5));
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
	project.mark_saved(movie_test_path("movie.dfv"), stale_revision);
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
	auto clip = make_hd("a.mp4", 10);
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

	movie_project screens;
	screens.append(make_movie_text("Trip"));
	assert_equal(true, screens.is_ready_to_render(), "a text screen needs no source to render");
	assert_equal(false, movie_project{}.is_ready_to_render(), "and an empty timeline has nothing to render");
}

static void should_trim_movie_clips()
{
	movie_project project;
	project.append(make_hd("a.mp4", 10));

	project.trim(0, 2, 8);
	assert_equal(2.0, project.clips()[0].start, "the in point is taken");
	assert_equal(8.0, project.clips()[0].end, "the out point is taken");
	assert_equal(6.0, project.clips()[0].duration(), "and the clip is that long");
	assert_equal(true, project.clips()[0].is_trimmed(), "a trimmed clip says so");

	project.trim(0, -5, 100);
	assert_equal(0.0, project.clips()[0].start, "a trim below zero is clamped");
	assert_equal(10.0, project.clips()[0].end, "a trim past the source is clamped");
	assert_equal(false, project.clips()[0].is_trimmed(), "and a full-length clip is untrimmed");

	movie_project held;
	held.append(make_photo("a.jpg", 4, {4000, 3000}));
	held.append(make_movie_text("Trip", 5));

	held.trim(0, 0, 9);
	assert_equal(9.0, held.clips()[0].end, "a photo shows for the length it is given");
	held.trim(1, 0, 2.5);
	assert_equal(2.5, held.clips()[1].duration(), "and so does a text screen");
	held.trim(1, 0, 1e9);
	assert_equal(movie_max_hold_seconds, held.clips()[1].duration(), "held to the most either may show for");
	held.trim(0, 0, 0);
	assert_equal(movie_min_hold_seconds, held.clips()[0].duration(), "and to the least");
	assert_equal(false, held.clips()[1].is_trimmed(), "a screen has no source to be trimmed from");
}

static void should_select_and_move_movie_clips()
{
	movie_project project;

	for (auto i = 0; i < 5; ++i)
	{
		project.append(make_hd(std::format("{}.mp4", i), 10));
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
	no_history.insert_many(0, {make_hd("a.mp4", 10), make_hd("b.mp4", 10)});
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

static void should_tell_a_seeded_timeline_from_an_edited_one()
{
	movie_project project;
	project.insert_many(0, {make_hd("a.mp4", 10), make_hd("b.mp4", 10)});
	project.mark_seeded();

	assert_equal(false, project.is_modified(), "seeding is not an edit the user made");
	assert_equal(false, project.can_undo(), "and there is nothing before a seed to undo to");

	project.trim(0, 1.0, 5.0);
	assert_equal(true, project.is_modified(), "a trim is an edit, so the timeline now holds unsaved work");

	project.undo();
	assert_equal(false, project.is_modified(), "undoing back to the seed leaves nothing to lose");

	project.reset({make_hd("c.mp4", 10)}, movie_test_path("p.dfv"));
	assert_equal(false, project.is_modified(), "a freshly opened project is unmodified");
}

// Re-entering Movie with five videos selected showed the timeline from the first visit, because an
// edited timeline was kept and never asked about until the application closed. Leaving now settles
// unsaved work, so the selection decides what entering shows.
static void should_decide_what_entering_movie_does()
{
	const std::vector first{movie_test_path("a.mp4"), movie_test_path("b.mp4")};
	const std::vector second{movie_test_path("c.mp4"), movie_test_path("d.mp4"), movie_test_path("e.mp4")};
	const std::vector<core::file_path> none;

	movie_project project;
	assert_equal(true, decide_movie_entry(project, first, none) == movie_entry::seed,
	             "an empty timeline is built from the selection");
	assert_equal(true, decide_movie_entry(project, none, first) == movie_entry::seed,
	             "and stays empty when nothing is selected");

	project.insert_many(0, {make_hd("a.mp4", 10), make_hd("b.mp4", 10)});
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
	project.mark_saved(movie_test_path("holiday.dfv"), project.edit_revision());
	assert_equal(true, decide_movie_entry(project, first, first) == movie_entry::resume,
	             "the selection that made a saved timeline returns to it, trims and all");
	assert_equal(true, decide_movie_entry(project, second, first) == movie_entry::seed,
	             "and a different selection replaces it");

	// A project opened in Movie answers to the selection the timeline it replaced was built from.
	project.reset({make_hd("p.mp4", 10)}, movie_test_path("p.dfv"));
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
	saved_empty.reset({}, movie_test_path("empty.dfv"));
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
	project.insert_many(0, {make_hd("a.mp4", 10), make_photo("b.jpg", 4, {4000, 3000})});
	project.mark_seeded();

	project.trim(0, 0, 10);
	assert_equal(false, project.can_undo(), "a trim to the range already kept is not an edit");

	project.trim(1, 0, 4);
	assert_equal(false, project.can_undo(), "nor is a photo given the length it already shows for");

	project.text(0, "");
	assert_equal(false, project.can_undo(), "nor is a caption set to the words it already has");

	project.replace(1, project.clips()[1]);
	assert_equal(false, project.can_undo(), "replacing a clip with itself is not an edit");

	project.replace_many({{0, project.clips()[0]}, {1, project.clips()[1]}});
	assert_equal(false, project.can_undo(), "nor is relinking every clip to where it already is");
	assert_equal(false, project.is_modified(), "so the timeline holds nothing to save");

	project.text(0, "Lukla");
	project.text(0, "Lukla");
	project.undo();
	assert_equal(true, project.clips()[0].text.empty(),
	             "words reported twice are one edit, so one undo takes them back");
}

// Undo returns to the state last saved, seeded or opened, and from there leaving has nothing to ask.
// Past that point, or once the history that led back to it is gone, the timeline is a change again.
static void should_undo_back_to_an_unmodified_timeline()
{
	movie_project project;
	project.insert_many(0, {make_hd("a.mp4", 10), make_hd("b.mp4", 10)});
	project.mark_seeded();

	project.trim(0, 1, 9);
	project.mark_saved(movie_test_path("movie.dfv"), project.edit_revision());
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
	overtaken.insert_many(0, {make_hd("a.mp4", 10)});
	overtaken.mark_seeded();
	const auto stale = overtaken.edit_revision();
	overtaken.trim(0, 1, 9);
	overtaken.mark_saved(movie_test_path("movie.dfv"), stale);
	overtaken.undo();
	assert_equal(true, overtaken.is_modified(), "an overtaken save leaves the timeline needing another");

	// An Undo that lands while a save runs can return to the earlier clean point, but the file on disk
	// then holds the state that was undone, so the timeline still differs from it.
	movie_project undone;
	undone.insert_many(0, {make_hd("a.mp4", 10)});
	undone.mark_seeded();
	undone.trim(0, 1, 9);
	const auto saving = undone.edit_revision();
	undone.undo();
	assert_equal(false, undone.is_modified(), "undo reaches the seeded state");
	undone.mark_saved(movie_test_path("movie.dfv"), saving);
	assert_equal(true, undone.is_modified(), "a save overtaken by undo leaves the timeline unsaved");

	// The bounded stack can drop the clean snapshot itself.
	movie_project deep;
	deep.insert_many(0, {make_hd("a.mp4", 100)});
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
	auto clip = make_hd("a.mp4", 10);
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

	project.mark_saved(movie_test_path("movie.dfv"), saving);
	assert_equal(false, project.is_modified(), "a save the probe landed during still settles the timeline");

	const auto opening = project.edit_revision();
	project.trim(0, 1, 4);
	assert_equal(true, opening != project.edit_revision(), "a real edit during an open still counts");
}

// One Add or one drop of several files is one action, so one undo takes all of it back.
static void should_add_several_clips_as_one_edit()
{
	movie_project project;
	project.append(make_hd("a.mp4", 10));
	project.mark_seeded();

	project.insert_many(1, {make_hd("b.mp4", 10), make_photo("c.jpg", 4, {4000, 3000}), make_hd("d.mp4", 10)});

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
	auto clip = make_hd("a.mp4", 10);

	assert_equal(true, clip.is_missing, "a clip starts unresolved");
	assert_equal(false, clip.is_probed, "because nothing has looked for it yet");
	assert_equal(false, clip.is_lost(), "so it is not a clip that is gone");

	clip.is_probed = true;
	assert_equal(true, clip.is_lost(), "a probe that did not find it is what makes it gone");

	clip.is_missing = false;
	assert_equal(false, clip.is_lost(), "and a clip that was found is not");

	assert_equal(false, make_movie_text("Trip").is_lost(), "a text screen has no source to lose");

	// Relinking replaces where the work was aimed and keeps the work: the stored trim survives, and
	// the clip goes back to unprobed so the new source is measured before the trim is clamped to it.
	movie_project project;
	auto trimmed = make_hd("b.mp4", 10);
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
}

// Dragging a slider reports every value it passes. Each was an undo step, so Undo walked a drag back one
// value at a time, a long drag pushed the saved state out of the history, and a drag that ended where it
// began still left the timeline asking to be saved.
static void should_merge_one_gesture_into_one_undo_step()
{
	movie_project project;
	project.insert_many(0, {make_hd("a.mp4", 10), make_photo("b.jpg", 4, {4000, 3000}), make_movie_text("Trip", 5)});
	project.mark_seeded();

	for (auto tenths = 11; tenths <= 200; ++tenths)
	{
		project.trim(1, 0, tenths / 10.0, movie_edit_group::hold);
	}

	assert_equal(true, project.is_modified(), "a drag is an edit");
	project.undo();
	assert_equal(4.0, project.clips()[1].duration(), "one undo takes the whole drag back");
	assert_equal(false, project.can_undo(), "in one step");
	assert_equal(false, project.is_modified(), "to the state it started from");

	project.trim(2, 0, 9, movie_edit_group::hold);
	project.trim(2, 0, 5, movie_edit_group::hold);
	assert_equal(false, project.is_modified(), "a drag that ends where it began is no edit");
	assert_equal(false, project.can_undo(), "and costs no undo step");

	// A settled drag is a step of its own, and so is the next.
	project.trim(2, 0, 6, movie_edit_group::hold);
	project.end_edit_group();
	project.trim(2, 0, 7, movie_edit_group::hold);
	project.undo();
	assert_equal(6.0, project.clips()[2].duration(), "one settled drag is not merged into the next");

	// One slider serves whichever item has focus, so the same gesture on another item is another step.
	project.trim(1, 0, 3, movie_edit_group::hold);
	project.trim(2, 0, 3, movie_edit_group::hold);
	project.undo();
	assert_equal(6.0, project.clips()[2].duration(), "undo takes back the drag on the second item");
	assert_equal(3.0, project.clips()[1].duration(), "and leaves the first item's");

	// So does a save: what comes after it is a change from what is on disk.
	project.trim(1, 0, 8, movie_edit_group::hold);
	project.mark_saved(movie_test_path("movie.dfv"), project.edit_revision());
	project.trim(1, 0, 5, movie_edit_group::hold);
	project.undo();
	assert_equal(8.0, project.clips()[1].duration(), "undo returns to what was saved");
	assert_equal(false, project.is_modified(), "which leaves nothing to save");
}

// A text box reports every keystroke. Each was a step, so Undo took a caption back a letter at a time;
// and one box serves whichever item has focus, so typing after focus moved must not run on into the
// last item's step.
static void should_merge_typing_into_one_undo_step()
{
	movie_project project;
	project.insert_many(0, {make_hd("a.mp4", 10), make_movie_text("", 5)});
	project.mark_seeded();

	std::string typed;

	for (const auto c : "Lukla"sv)
	{
		typed += c;
		project.text(0, typed, movie_edit_group::text);
	}

	assert_equal("Lukla"s, project.clips()[0].text, "every keystroke lands");

	project.text(1, "T", movie_edit_group::text);
	project.text(1, "The End", movie_edit_group::text);
	project.undo();
	assert_equal(true, project.clips()[1].text.empty(), "undo takes back the typing on the screen");
	assert_equal("Lukla"s, project.clips()[0].text, "and leaves the clip's caption");
	project.undo();
	assert_equal(true, project.clips()[0].text.empty(), "one undo takes back a whole run of typing");
	assert_equal(false, project.can_undo(), "in one step");
	assert_equal(false, project.is_modified(), "to where it began");

	project.text(1, "T", movie_edit_group::text);
	project.text(1, "Tr", movie_edit_group::text);
	project.text(1, "", movie_edit_group::text);
	assert_equal(false, project.is_modified(), "typing words and deleting them again is no edit");
	assert_equal(false, project.can_undo(), "and costs no step");

	project.text(0, std::string(5000, 'x'));
	assert_equal(movie_max_text_bytes, project.clips()[0].text.size(), "text is held to what a document keeps");
}

// The panel binds to the focused item's values. Moving focus must show that item's, never the last one's:
// a caption carried from one clip to the next would be written into it at the next keystroke.
static void should_show_the_focused_movie_item_in_the_panel()
{
	movie_view_state state;
	auto captioned = make_hd("a.mp4", 10);
	captioned.text = "Lukla";
	auto shaky = make_hd("b.mp4", 10);
	shaky.stabilize = true;
	state.project.reset({captioned, make_movie_text("Trip", 7.25), shaky}, {});

	state.read_from_project();
	assert_equal("Lukla"s, state.item_text, "a clip's caption is in the box");

	state.project.current(1);
	state.read_from_project();
	assert_equal("Trip"s, state.item_text, "a text screen's words are");
	assert_equal(73, state.hold_tenths, "with how long it shows, in tenths");

	state.project.current(2);
	state.read_from_project();
	assert_equal(true, state.item_text.empty(), "a clip with no caption shows an empty box");
	assert_equal(true, state.stabilize, "and whether it is to be stabilized");
}

//
// The render's destination and its cancellation
//

static void should_refuse_an_output_that_names_a_clip()
{
	const std::vector clips{make_video("a.mp4", 4, {1920, 1080}, 30), make_photo("b.jpg", 3, {4000, 3000}),
	                        make_movie_text("Trip")};

	assert_equal(true, movie_output_names_a_clip(clips, movie_test_path("a.mp4")),
	             "an output naming a video clip is refused");
	// On Linux B.JPG and b.jpg are two files, and writing one leaves the other intact.
	assert_equal(core::case_insensitive_path_identity, movie_output_names_a_clip(clips, movie_test_path("B.JPG")),
	             "the comparison folds case where the platform's paths do, as every other destination check does");
	assert_equal(false, movie_output_names_a_clip(clips, movie_test_path("movie.mp4")),
	             "an output naming nothing in the timeline is allowed");
	assert_equal(false, movie_output_names_a_clip(clips, {}), "an empty output names no clip");
	assert_equal(false, movie_output_names_a_clip({}, movie_test_path("a.mp4")),
	             "an empty timeline has no clip to name");
}

static void should_let_cancel_and_publication_decide_each_other()
{
	// The defect this fixes: the render's cancel source doubled as its publication claim, and
	// core::cancel_token's version constructor increments what it is handed. The claim was already 1
	// before a frame was drawn, so Cancel's compare-exchange from 0 did nothing and publication's
	// compare-exchange from 0 failed - every completed render was deleted as cancelled.
	{
		movie_render_control control;
		const auto token = core::cancel_token(control.cancelled);
		assert_equal(false, token.is_cancelled(), "a fresh render is not already cancelled");
		assert_equal(true, control.claim_publication(), "an uncancelled render publishes its output");
	}

	{
		movie_render_control control;
		const auto token = core::cancel_token(control.cancelled);
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

//
// The project file
//

static void should_round_trip_a_movie_project()
{
	auto video = make_hd("a.mp4", 12);
	video.start = 1.5;
	video.end = 8.25;
	video.text = "Alfie's \"first\" day\nC:\\path";

	auto shaky = make_hd("shaky.mp4", 20);
	shaky.stabilize = true;

	const auto far_path = core::folder_path(core::windows_path_semantics ? "d:\\other place" : "/other place")
	                          .combine_file("far away.mp4");
	auto far = make_hd("far.mp4", 30);
	far.path = far_path;
	far.end = 10;

	const std::vector clips{make_movie_text("Hiking to\nEverest", 5), video, make_photo("50% off.jpg", 5, {4000, 3000}),
	                        shaky, far};

	const auto text = write_dfv(clips, movie_test_folder());

	assert_equal(true, text.starts_with("# Diffractor video"), "the file says what it is, in a comment");
	assert_equal(true, core::contains(text, "\nt 5 'Hiking to\\nEverest'\n"), "a text screen is its seconds and words");
	assert_equal(true, core::contains(text, "\nc 1.5-8.25 a.mp4 'Alfie\\'s \"first\" day\\nC:\\\\path'\n"),
	             "a clip is the part it plays, its file beside the project, and its caption escaped");
	assert_equal(true, core::contains(text, "\nc -5 \"50% off.jpg\"\n"), "a photo is how long it shows");
	assert_equal(true, core::contains(text, "\ns - shaky.mp4\n"), "a clip to stabilize is an s, and a whole clip -");
	assert_equal(true, core::contains(text, std::format("\nc -10 \"{}\"\n", far_path.pack())),
	             "a source outside the project's folder is named whole");

	const auto read = read_dfv(text, movie_test_folder());
	assert_equal(true, static_cast<bool>(read), "the project reads back");
	assert_equal(true, read.problems.empty(), "every line of it");
	assert_equal(5_z, read.clips.size(), "with every item");

	assert_equal(true, read.clips[0].is_text, "a screen is still a screen");
	assert_equal("Hiking to\nEverest"s, read.clips[0].text, "with its lines");
	assert_equal(5.0, read.clips[0].duration(), "and its length");

	assert_equal(video.path.pack(), read.clips[1].path.pack(), "a relative path resolves back");
	assert_equal(1.5, read.clips[1].start, "the in point survives");
	assert_equal(8.25, read.clips[1].end, "the out point survives");
	assert_equal(video.text, read.clips[1].text, "and so does the caption, quotes and all");

	assert_equal(true, read.clips[2].is_photo, "a photo is still a photo");
	assert_equal(5.0, read.clips[2].end, "its hold survives");
	assert_equal(clips[2].path.pack(), read.clips[2].path.pack(), "and so does a name a URL would have escaped");

	assert_equal(true, read.clips[3].stabilize, "a clip to stabilize stays marked");
	assert_equal(0.0, read.clips[3].end, "and an untrimmed one runs to the end of its source, once measured");
	assert_equal(far_path.pack(), read.clips[4].path.pack(), "a distant source resolves without the project's folder");
	assert_equal(10.0, read.clips[4].end, "with its out point");

	const auto empty = read_dfv(write_dfv({}, movie_test_folder()), movie_test_folder());
	assert_equal(true, static_cast<bool>(empty) && empty.clips.empty() && empty.problems.empty(),
	             "an empty project is a valid project");
}

// The point of the format is that a person or an agent writes it. Everything the grammar allows reads,
// however it is spaced, whatever ends its lines, and whichever of the forms of a clip's part it uses.
static void should_read_a_movie_project_written_by_hand()
{
	const auto text = "\xEF\xBB\xBF# A trip, written by hand\r\n"
					  "\r\n"
					  "T 4 'Our trip'\r\n"
					  "c 2.5-3.3 clips/a.mp4 'Lukla'\r\n"
					  "c -3.3 clips/b.mp4\r\n"
					  "\tc   2.5-   ../c.mp4  \r\n"
					  "c - \"with space.mp4\"\r\n"
					  "c #4 tune.sid\r\n"
					  "c 1:02.5-1:10#2 tune.sid\r\n"
					  "s 0:00:01-0:00:04 shaky.mp4\r\n"
					  "c -4 a.jpg\r\n"
					  "c - b.jpg\r\n"
					  "c 2-6 c.jpg\r\n"
					  "t 3\r\n"sv;

	const auto read = read_dfv(text, movie_test_folder());
	assert_equal(true, static_cast<bool>(read), "a hand-written project reads");
	assert_equal(true, read.problems.empty(), "every line of it");
	assert_equal(12_z, read.clips.size(), "with an item for each instruction");

	assert_equal(true, read.clips[0].is_text && read.clips[0].text == "Our trip" && read.clips[0].duration() == 4.0,
	             "an instruction in capitals is the same instruction");
	assert_equal(movie_test_folder().combine("clips").combine_file("a.mp4").pack(), read.clips[1].path.pack(),
	             "a relative path is the project's folder's");
	assert_near(2.5, read.clips[1].start, 1e-9, "from-to plays that part");
	assert_near(3.3, read.clips[1].end, 1e-9, "of its source");
	assert_equal("Lukla"s, read.clips[1].text, "with its caption");
	assert_equal(true, read.clips[2].start == 0 && read.clips[2].end == 3.3, "-to plays from the start");
	assert_equal(movie_test_folder().parent().combine_file("c.mp4").pack(), read.clips[3].path.pack(),
	             "and may lead out of it");
	assert_equal(true, read.clips[3].start == 2.5 && read.clips[3].end == 0, "from- plays to the end, once measured");
	assert_equal(movie_test_path("with space.mp4").pack(), read.clips[4].path.pack(), "a quoted path keeps its space");
	assert_equal(true, read.clips[5].is_audio && read.clips[5].song == 4 && read.clips[5].end == 0,
	             "#track alone plays the whole of that tune");
	assert_equal(true, read.clips[6].song == 2 && read.clips[6].start == 62.5 && read.clips[6].end == 70,
	             "times may be minutes and seconds, with a tune");
	assert_equal(true, read.clips[7].stabilize && read.clips[7].start == 1 && read.clips[7].end == 4,
	             "s marks a clip to stabilize, and hours are allowed");
	assert_equal(true, read.clips[8].is_photo && read.clips[8].duration() == 4.0, "-4 shows a photo for 4 s");
	assert_equal(movie_photo_seconds, read.clips[9].duration(), "- shows it for the default length");
	assert_equal(4.0, read.clips[10].duration(), "and from-to for as long as the part spans");
	assert_equal(true, read.clips[11].is_text && read.clips[11].text.empty(), "a screen may have no words: black");
}

// A line the reader cannot use is named, with its number and what is wrong, and the rest of the project
// still reads: the view opens what it can and the command line refuses to render it, so either way the
// writer is told.
static void should_report_movie_project_lines_it_cannot_read()
{
	const auto text = "t 3 'Fine'\n"
					  "x 2 'What'\n"
					  "t 'no seconds'\n"
					  "t 0 'zero'\n"
					  "c 5 a.mp4\n"
					  "c 3-2 a.mp4\n"
					  "c -\n"
					  "t 3 'not closed\n"
					  "t 3 'run'into\n"
					  "c - a.mp4 'cap' extra\n"
					  "c 1:75-2:00 a.mp4\n"
					  "c #x tune.sid\n"
					  "c 1e3-2e3 a.mp4\n"
					  "c -0 a.mp4\n"
					  "c - b.mp4\n"sv;

	const auto read = read_dfv(text, movie_test_folder());
	assert_equal(true, static_cast<bool>(read), "a project with good lines still reads");
	assert_equal(2_z, read.clips.size(), "taking every line it can");

	std::vector<int> lines;
	lines.reserve(read.problems.size());
	for (const auto& problem : read.problems) lines.emplace_back(problem.line);
	assert_equal(true, lines == std::vector{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14},
	             "and naming every line it cannot, as an editor numbers them");
	assert_equal(true,
	             std::ranges::none_of(read.problems, [](const movie_load_problem& p) { return p.reason.empty(); }),
	             "each with a reason");

	assert_equal(false, static_cast<bool>(read_dfv("<html>not a project</html>\nnor this\n", movie_test_folder())),
	             "a file every line of which is a problem is not a project");
	const auto comments = read_dfv("# only a comment\n\n", movie_test_folder());
	assert_equal(true, static_cast<bool>(comments) && comments.clips.empty(), "while one of comments is an empty one");
}

// A project file is untrusted input: a length reaches the frame count, which no integer can hold past a
// point, and text reaches a renderer that draws all of it.
static void should_bound_movie_project_durations()
{
	const auto text = "t 99999 'long'\n"
	                  "c -99999 still.jpg\n"
	                  "c 99999999999- late.mp4\n"
	                  "c -" +
	                  std::string(400, '9') +
	                  " huge.mp4\n"
	                  "t 5 '" +
	                  std::string(3000, 'x') + "'\n";

	const auto read = read_dfv(text, movie_test_folder());
	assert_equal(4_z, read.clips.size(), "every bounded line reads");
	assert_equal(movie_max_hold_seconds, read.clips[0].duration(), "a screen shows for no more than a document allows");
	assert_equal(movie_max_hold_seconds, read.clips[1].duration(), "and nor does a photo");
	assert_equal(movie_max_clip_seconds, read.clips[2].start,
	             "an in point no source can have is held to the longest movie");
	assert_equal(1_z, read.problems.size(), "a number too large to read is a line that cannot be read");
	assert_equal(movie_max_text_bytes, read.clips[3].text.size(), "and text is held to what a document keeps");
}

// A clip that runs to the end of its source is written "-", and given the rest of its source again by the
// probe when it is read: untrimmed stays untrimmed, and a clip saved before it was measured is kept.
static void should_keep_a_clip_running_to_its_sources_end_through_save()
{
	const auto camera = make_video("camera.mov", 12.3452, {1920, 1080}, 30);
	auto late_start = camera;
	late_start.start = 2;
	const auto pending = make_video("pending.mp4", 0, {}, 0);

	const auto text = write_dfv({camera, late_start, pending}, movie_test_folder());
	assert_equal(true, core::contains(text, "\nc - camera.mov\n"), "an untrimmed clip is written whole");
	assert_equal(true, core::contains(text, "\nc 2- camera.mov\n"), "one trimmed at its start only runs to its end");
	assert_equal(true, core::contains(text, "\nc - pending.mp4\n"), "and one not yet measured is kept");

	const auto read = read_dfv(text, movie_test_folder());
	movie_probe_result probe;
	probe.path = camera.path;
	probe.found = true;
	probe.duration = 12.3452;
	probe.extent = {1920, 1080};

	const auto measured = apply_movie_probe(read.clips[0], probe);
	assert_equal(false, measured.is_trimmed(), "an untrimmed clip is still untrimmed");
	assert_near(12.3452, measured.end, 1e-9, "and runs to the end of its source");
	assert_equal(2.0, apply_movie_probe(read.clips[1], probe).start, "a late start survives");

	probe.duration = 9.0;
	assert_equal(9.0, apply_movie_probe(read.clips[2], probe).end, "and the probe gives an unmeasured clip its source");
}

// A name can hold a byte that is no UTF-8 character, which converting through the process code page
// refused. Save used to end the application over one; it must write it, and opening must still read the
// lines it can.
static void should_save_clip_paths_that_are_not_utf8()
{
	auto odd = make_hd("x.mp4", 4);
	odd.path = movie_test_folder().combine_file("caf\xE9.mp4");

	const auto text = write_dfv({odd, make_hd("plain.mp4", 4)}, movie_test_folder());
	const auto read = read_dfv(text, movie_test_folder());
	assert_equal(true, static_cast<bool>(read), "a project naming such a file beside it still opens");
	assert_equal(movie_test_path("plain.mp4").pack(), read.clips.back().path.pack(), "with the clips it can name");
}

// What Save writes always reads back, whatever the document holds: an in point no number can write, a
// length that is not a number, and words with every character the grammar quotes.
static void should_write_a_project_that_reads_back_whatever_it_holds()
{
	auto broken = make_hd("broken.mp4", 10);
	broken.start = std::numeric_limits<double>::infinity();
	broken.source_duration = std::numeric_limits<double>::quiet_NaN();
	broken.text = "it's \\ \"odd\"\r\nisn't it";

	const auto read = read_dfv(write_dfv({broken, make_movie_text("x\\'y #1 - 'z'", 2)}, movie_test_folder()),
	                           movie_test_folder());
	assert_equal(true, static_cast<bool>(read) && read.problems.empty(), "it reads back");
	assert_equal(2_z, read.clips.size(), "every item of it");
	assert_equal("it's \\ \"odd\"\nisn't it"s, read.clips[0].text,
	             "a caption is what was typed, but for a carriage return");
	assert_equal("x\\'y #1 - 'z'"s, read.clips[1].text, "and a screen's words are exactly what they were");
}

// An audio file has no picture of its own. Movie shows it as Diffractor plays it, on a landscape frame, so
// it counts as a video of that frame's size and rate; and a project says which of a SID file's tunes it
// plays.
static void should_take_audio_files_into_a_movie()
{
	for (const auto name : {"song.mp3", "tune.sid", "module.mod"})
	{
		assert_equal(true, is_movie_source(files::file_type_from_name(std::string_view(name))),
		             std::format("{} can go into a movie", name));
		const auto clip = make_movie_clip(movie_test_path(name));
		assert_equal(true, clip.is_audio && !clip.is_photo, std::format("{} becomes an audio clip", name));
	}

	assert_equal(true, is_movie_source(files::file_type_from_name(std::string_view("a.mp4"))), "a video can");
	assert_equal(true, is_movie_source(files::file_type_from_name(std::string_view("a.jpg"))), "and a photo");
	assert_equal(false, is_movie_source(files::file_type_from_name(std::string_view("a.txt"))), "but not a document");
	assert_equal(false, make_movie_clip(movie_test_path("a.mp4")).is_audio, "a video is no audio clip");

	movie_probe_result probe;
	probe.path = movie_test_path("song.mp3");
	probe.found = true;
	probe.is_audio = true;
	probe.extent = movie_audio_extent;
	probe.frame_rate = movie_audio_frame_rate;
	probe.duration = 200;
	const auto song = apply_movie_probe(make_movie_clip(probe.path), probe);
	assert_equal(200.0, song.duration(), "an audio clip plays the whole file");
	assert_equal(true, song.is_audio, "and stays an audio clip once measured");

	const auto output = derive_movie_output({song, make_photo("big.jpg", 4, {8000, 6000})});
	assert_equal(movie_audio_extent.cx, output.extent.cx, "its landscape frame sizes the movie, as a video's does");
	assert_equal(movie_audio_extent.cy, output.extent.cy, "both ways");
	assert_equal(movie_audio_frame_rate, output.frame_rate, "and its rate is the movie's");

	// A SID file holds several tunes and a clip plays one, named once the probe has found the file holds others.
	movie_probe_result tunes = probe;
	tunes.path = movie_test_path("tune.sid");
	tunes.songs = 3;
	tunes.first_song = 2;
	const auto tune = apply_movie_probe(make_movie_clip(tunes.path), tunes);
	assert_equal(true, tune.song == 2 && tune.songs == 3, "a tune is the one its file starts on, of those it holds");

	auto third = tune;
	third.song = 3;
	const auto text = write_dfv({third, song}, movie_test_folder());
	assert_equal(true, core::contains(text, "\nc -#3 tune.sid\n"), "a project names the tune a clip plays");

	const auto read = read_dfv(text, movie_test_folder());
	assert_equal(true,
	             read.clips.size() == 2 && read.clips[0].is_audio && read.clips[0].song == 3 && read.clips[1].song == 0,
	             "and keeps it, beside a clip that plays the file's own");
}

// A clip may name a sound file, written after it with a !, which plays in place of its own sound. Quoted
// after the ! when its path has a space, and either before or after the caption.
static void should_read_and_write_a_clips_sound_file()
{
	const auto text = "c - scene.mp4 'Find anything' !narration/scene.wav\n"
					  "c 2-6 b.mp4 !\"voice over.wav\"\n"
					  "c -3 card.png !'it\\'s.wav' 'Title'\n"
					  "c - c.mp4 !../music/song.mp3\n"
					  "c - \"!intro.mp4\"\n"sv;

	const auto read = read_dfv(text, movie_test_folder());
	assert_equal(true, static_cast<bool>(read) && read.problems.empty(), "a project naming sound files reads");
	assert_equal(5_z, read.clips.size(), "with an item for each line");
	assert_equal(movie_test_folder().combine("narration").combine_file("scene.wav").pack(),
	             read.clips[0].sound_file.pack(), "a sound file is beside the project, as a clip's file is");
	assert_equal("Find anything"s, read.clips[0].text, "and the caption is still the caption");
	assert_equal(movie_test_path("voice over.wav").pack(), read.clips[1].sound_file.pack(),
	             "a quoted sound file keeps its space");
	assert_equal(true, read.clips[1].start == 2 && read.clips[1].end == 6, "beside the part its clip plays");
	assert_equal(movie_test_path("it's.wav").pack(), read.clips[2].sound_file.pack(), "and may be single-quoted");
	assert_equal(true, read.clips[2].is_photo && read.clips[2].text == "Title",
	             "a photo may have one, before its caption");
	assert_equal(movie_test_folder().parent().combine("music").combine_file("song.mp3").pack(),
	             read.clips[3].sound_file.pack(), "and lead out of the project's folder");
	assert_equal(movie_test_path("!intro.mp4").pack(), read.clips[4].path.pack(), "a quoted ! is part of a name");
	assert_equal(false, read.clips[4].has_sound_file(), "and names no sound");

	const auto written = write_dfv(read.clips, movie_test_folder());
	assert_equal(true, core::contains(written, "\nc - scene.mp4 'Find anything' !narration/scene.wav\n"),
	             "a sound file is written after the caption, relative to the project");
	assert_equal(true, core::contains(written, "\nc 2-6 b.mp4 !\"voice over.wav\"\n"),
	             "and quoted when it has a space");
	assert_equal(true, core::contains(written, "\nc - \"!intro.mp4\"\n"), "a file whose name starts with ! is quoted");

	const auto again = read_dfv(written, movie_test_folder());
	assert_equal(true, static_cast<bool>(again) && again.problems.empty() && again.clips.size() == read.clips.size(),
	             "what is written reads back");

	for (size_t i = 0; i < read.clips.size(); ++i)
	{
		assert_equal(read.clips[i].sound_file.pack(), again.clips[i].sound_file.pack(),
		             std::format("item {} keeps its sound file", i + 1));
	}

	const auto mistakes = read_dfv("t 3 'Title' !voice.wav\n"
	                               "c - a.mp4 !\n"
	                               "c - a.mp4 !one.wav !two.wav\n"
	                               "c !voice.wav a.mp4\n"
	                               "!c - a.mp4\n"
	                               "c - a.mp4 'one' 'two'\n"
	                               "c - a.mp4 !voice.wav\n"sv,
	                               movie_test_folder());

	std::vector<int> lines;
	lines.reserve(mistakes.problems.size());
	for (const auto& problem : mistakes.problems) lines.emplace_back(problem.line);
	assert_equal(true, lines == std::vector{1, 2, 3, 4, 5, 6},
	             "a sound file on a text screen, a ! naming nothing, two sound files, one in place of the part or the "
	             "instruction, and two captions are each a line that cannot be read");
	assert_equal(1_z, mistakes.clips.size(), "while the good line still reads");
}

// A clip's sound file plays in place of its own sound: from the file's start as the clip begins, whatever the
// clip's in point, and only while the clip shows. A photo with one has sound to cross into.
static void should_play_a_clips_sound_file_in_place_of_its_own()
{
	auto trimmed = make_hd("scene.mp4", 30);
	trimmed.start = 5;
	trimmed.end = 15;
	assert_equal(true, movie_clip_sound(trimmed).path == trimmed.path, "a clip with no sound file plays its own sound");
	assert_equal(true, movie_clip_sound(trimmed).start == 5 && movie_clip_sound(trimmed).end == 15,
	             "over the part it plays");
	assert_equal(true, movie_clip_sound(make_photo("p.jpg", 4, {4000, 3000})).is_empty(), "a photo alone has none");
	assert_equal(true, movie_clip_sound(make_movie_text("Title")).is_empty(), "and nor does a text screen");

	auto narrated = trimmed;
	narrated.sound_file = movie_test_path("narration.wav");
	const auto sound = movie_clip_sound(narrated);
	assert_equal(narrated.sound_file.pack(), sound.path.pack(), "a sound file is heard instead");
	assert_equal(true, sound.start == 0 && sound.end == 10, "from its start, for as long as the clip shows");

	const auto frame = calc_movie_frame({narrated}, 4.0);
	assert_equal(9.0, frame.a.source_time, "the picture comes from the clip's own part");
	assert_equal(4.0, frame.a.sound_time, "while its sound file is four seconds in");
	assert_equal(9.0, calc_movie_frame({trimmed}, 4.0).a.sound_time, "and a clip's own sound is where its picture is");

	const auto talking = make_hd("talking.mp4", 10);
	auto card = make_photo("card.png", 4, {1920, 1080});
	assert_equal(1.0, calc_movie_frame({talking, card}, 9.5).a.sound, "a clip's sound stays up over a silent photo");

	card.sound_file = movie_test_path("intro.wav");
	assert_equal(0.5, calc_movie_frame({talking, card}, 9.5).a.sound, "but crossfades into a photo with a sound file");
	assert_equal(0.5, calc_movie_frame({talking, card}, 9.5).b.sound, "whose sound fades up as its picture does");

	card.sound_file_is_probed = true;
	assert_equal(true, card.is_sound_file_lost(), "a sound file the probe did not find is lost");
	assert_equal(1.0, calc_movie_frame({talking, card}, 9.5).a.sound, "and leaves the clip before it its sound");

	auto ready = make_hd("ready.mp4", 4);
	ready.is_probed = true;
	ready.is_missing = false;
	ready.sound_file = movie_test_path("voice.wav");
	movie_project project;
	project.append(ready);
	assert_equal(false, project.is_ready_to_render(), "a clip whose sound file is not yet probed cannot render");

	movie_sound_probe heard;
	heard.path = ready.sound_file;
	heard.found = true;
	heard.duration = 3.5;
	const auto probed = apply_movie_sound_probe(ready, heard);
	assert_equal(true,
	             probed.sound_file_is_probed && !probed.sound_file_is_missing && probed.sound_file_duration == 3.5,
	             "a probe that found the sound file measures it");
	project.replace_quietly(0, probed);
	assert_equal(true, project.is_ready_to_render(), "and a clip whose sound file was found can render");

	heard.found = false;
	project.replace_quietly(0, apply_movie_sound_probe(ready, heard));
	assert_equal(false, project.is_ready_to_render(), "one whose sound file is gone cannot");

	assert_equal(true, movie_output_names_a_clip({ready}, ready.sound_file),
	             "and a movie may not be written over a sound file it reads");
}

void register_movie_tests(view_state& /*state*/, test_registry& tests)
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
	tests.add("Should time movie crossfades"s, should_time_movie_crossfades);
	tests.add("Should compose a movie frame"s, should_compose_a_movie_frame);
	tests.add("Should show movie captions for as long as they take to read"s,
	          should_show_movie_captions_for_as_long_as_they_take_to_read);
	tests.add("Should carry a movie clip's sound over an item with none"s,
	          should_carry_a_clips_sound_over_an_item_with_none);

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
	tests.add("Should tell an unprobed movie clip from a lost one"s, should_tell_an_unprobed_clip_from_a_lost_one);
	tests.add("Should merge one movie gesture into one undo step"s, should_merge_one_gesture_into_one_undo_step);
	tests.add("Should merge typing into one movie undo step"s, should_merge_typing_into_one_undo_step);
	tests.add("Should show the focused movie item in the panel"s, should_show_the_focused_movie_item_in_the_panel);

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
	tests.add("Should read a movie project written by hand"s, should_read_a_movie_project_written_by_hand);
	tests.add("Should report movie project lines it cannot read"s, should_report_movie_project_lines_it_cannot_read);
	tests.add("Should bound movie project durations"s, should_bound_movie_project_durations);
	tests.add("Should keep a movie clip running to its source's end through save"s,
	          should_keep_a_clip_running_to_its_sources_end_through_save);
	tests.add("Should save movie clip paths that are not UTF-8"s, should_save_clip_paths_that_are_not_utf8);
	tests.add("Should write a movie project that reads back whatever it holds"s,
	          should_write_a_project_that_reads_back_whatever_it_holds);
	tests.add("Should take audio files into a movie"s, should_take_audio_files_into_a_movie);
	tests.add("Should read and write a movie clip's sound file"s, should_read_and_write_a_clips_sound_file);
	tests.add("Should play a movie clip's sound file in place of its own"s,
	          should_play_a_clips_sound_file_in_place_of_its_own);
}
