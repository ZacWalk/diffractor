# Movie mixer

Movie mixer assembles an ordered timeline of photos and videos into one MP4 video. It provides clip
ordering, video trims, photo durations, one transition style, end fades, preview, project files and
rendering. It does not modify, move or delete source media.

## 1. Behaviour contract

Movie receives photos and videos from the complete visibly selected set in listing order. The
timeline then becomes an independent ordered document. Rendering writes one MP4 destination and Save
project writes one OTIO document; source media remains unchanged. Failed or cancelled writes preserve
an existing destination, and timeline edits are undoable during the session.

## 2. Entry and document lifecycle

**Movie mixer** is a task view. Its command is offered when the visible selection contains at least
one photo or video, or when an existing Movie timeline can be resumed.

Entering Movie with a selection of photos and videos that differs from the one its timeline was built
from, in its items or their listing order, builds a new timeline from it in listing order. Entering
with the same selection, or with none, returns to the timeline left behind, including edits that were
saved and a project that was opened or saved empty; a project opened in Movie answers to the selection
the timeline it replaced was built from.

Leaving Movie settles unsaved edits. When the timeline has changed since it was built, opened or
saved, Close, Items and Escape offer Save project, Discard or Cancel. An edited timeline that has
had every clip removed is still unsaved work; a pristine empty timeline is not. Save writes a valid
project, including one with no clips, and leaves once the project is written; Discard throws the
timeline away and keeps the movie settings; Cancel stays with the draft and its undo history. A
cancelled or failed save never counts as permission to discard the timeline. A running render is
asked about first. Open, Import and quitting while Movie is open ask the same question before the
timeline is replaced or lost.

Only a change is an edit. Undoing back to the state last built, opened or saved leaves nothing to
ask about, and a handle pressed and released in place, a setting chosen again, or a slider dragged
back to where it began, changes nothing. The probe filling in a clip's measured length and size is
not an edit either.

Project Open, Save and Relink perform file access on workers. Save writes a sibling temporary file
and replaces the destination only after the complete project has been written. Edits made while a
save is running, including an Undo, remain marked as unsaved. Saving does not wait for a running
render.

## 3. View and timeline

The view contains the shared top bar, a fitted preview, movie transport and scrubber, a one-row
horizontal timeline, and a right-hand controls panel.

Timeline tiles have equal width and play left to right. A tile shows its thumbnail, retained duration,
trim state, missing-source state and playback position. The timeline scrolls horizontally without
growing vertically: by its scroll bar, by either wheel axis or a touchpad, and by holding a dragged
block near either end.

Selection, focus and playback are separate:

- Filled tiles are selected and receive timeline commands.
- One outlined tile is focused and supplies the clip controls.
- A bar at the tile foot shows the playhead, under the clip playing from its own start: during a
  crossfade, the incoming one.

Plain click selects and focuses one clip. Ctrl+click toggles membership without allowing an empty
selection. Shift+click selects from the anchor. Pressing an already selected tile moves focus without
collapsing the selected block, so dragging moves the complete selection. A block keeps its internal
order and focus, and a no-op drop creates no undo entry. Escape during a drag drops nothing: the block,
a trim handle or the scrubber returns to where it was pressed, and Movie stays open.

Files dropped on the timeline insert at the displayed boundary; files dropped elsewhere append. While
files dragged from another application are over the strip, the boundary they would insert at is
drawn, as it is for a block dragged within the strip. Photos and videos are accepted and other files
are ignored. Remove, reorder, send to end, trim, add and relink are undoable. One Add, drop or Relink
is one undo step regardless of how many files it carries, and one drag of a slider, or one length
typed into it, is one undo step however many values it passes through.

## 4. Preview and clip controls

The preview and renderer use the same timeline timing and composition decision. Frames are fitted,
never stretched or cropped, and unused canvas area is black. Every source is used upright: a photo's
Exif orientation and a video's rotation are applied where its frame is made, so the strip, preview,
output dimensions and rendered file agree. A crossfade holds both contributing clips and mixes them
with the same weights used for audio.

The movie transport provides previous clip, play/pause, next clip, elapsed time, total time and a
whole-movie scrubber. Selecting a clip parks the preview on its first retained frame and stops movie
playback; while the movie plays, previous and next clip instead count from the clip the playhead is
in, not the focus, and carry playback on from the start of the clip they reach. A clip is the one
playing from its own start, so during a crossfade the incoming clip is the one counted from.
Starting movie playback or clip playback stops the other, and a clip whose source cannot be opened
stops the clip player rather than leaving it playing a still picture.

Video controls provide in and out points, a two-handle range scrubber, retained-region playback,
endpoint frames, an audio-level track and Reset. Pressing a handle stops playback. The out-point
preview shows the last retained frame, one of the clip's own frames before the out point. While a
handle is dragged the preview follows it, and once the pointer rests the preview shows the frame at
the handle, not one near it. Handles drawn on one point are told apart by which can still move.
Photo controls provide a duration and an option to follow the movie default; turning the default off
keeps the photo's current length. The controls always show the document's values, including after
Undo, Open or a change of focus.

Playing preview video uses two persistent frame sessions, bound to clip identity and opened ahead of a
crossfade. Photo and parked frames come from the bounded frame cache, and each preview slot keeps the
frame it is showing, so the two photos of a crossfade stay drawn however large the preview. A parked
frame is the source frame at the playhead, to half a frame of the clip's own. Preview audio is decoded
off the UI thread in bounded windows of each contributing clip's retained interval, held by source
window so a crossfade's incoming clip keeps its sound, and read ahead of the playhead. It is mixed at
the endpoint rate and the user's media volume into an endpoint ring long enough to outlast the UI tick
that refills it. When playback starts or jumps, the picture waits briefly for its sound so the two
begin together; a window still unread after that plays as silence rather than stalling, and sound that
falls behind the picture restarts at the playhead. A seek discards queued sound from the old position.

## 5. Movie settings

Movie settings are:

- Cut or one uniform crossfade between adjacent clips.
- Transition length. This one number is both the crossfade duration and the length of the end fades,
  because a second number meaning nearly the same thing is harder to predict than one. It is offered
  whenever either use is switched on, and is labelled for the one the current settings make.
- Fade in from black and fade out to black, applied to picture and sound.
- Default photo duration. Changing it updates photos that still follow the default.

The panel shows lengths in tenths of a second. A length it shows rounded, such as another tool's
0.75 s crossfade, keeps its exact value until the user changes it, and a length typed in is held to
the document's bounds.

Output is read-only and reports the derived dimensions, frame rate and duration.

## 6. Project files

The native project format is OpenTimelineIO (`.otio`). Movie writes one video track containing clips,
source ranges and uniform transitions, with times written exactly. A saved project may have no clips.
Source paths are relative to the project folder when possible. Photos are represented as image
references with retained durations.

Open and Save offer only native `.otio` projects. Windows Live Movie Maker `.wlmp` projects enter
through Import, never Open; saving an imported timeline writes a native project.

Transitions follow OpenTimelineIO's model: a clip's source range stops at the cut, and each crossfade
reaches half its length past the cut into the media on both sides, so another tool lays the clips end
to end to the movie's own length and draws each crossfade from the media Movie mixes. Movie's metadata
records this convention with a version; a project saved before it holds each clip's whole retained
range and is read as it was written.

Movie reads the first video track and reports constructs it cannot represent. It finds a clip's media
in OpenTimelineIO's named media references, using the active one, as well as in the older single
reference. A range in timecoded media counts from the media's first frame. A clip with no range, which
OpenTimelineIO reads as the whole media, and one saved before its source was measured are kept, and
take the length of their source when it is probed. A disabled clip is left out, and an effect such as
a speed change is not applied while its clip keeps its place; both are counted. A foreign OTIO project
defaults to cuts and no end fades. A foreign dissolve is adopted only when every clip boundary carries
a dissolve of the same length that both clips can hold; adopting it widens each clip by the media the
dissolve reaches into. Mixed or unsupported transitions are reported as ignored rather than silently
applied to other boundaries.

Windows Live Movie Maker (`.wlmp`) import accepts video and image extents in the order of the
project's main track, and leaves out an extent the main track does not place; a file without a main
track is ordered by timeline position. It preserves trims and photo durations. A clip with no out
point, which is how Movie Maker records one never trimmed, runs to the end of its source. A relative
media path is relative to the project's folder. The movie settings are Movie's defaults, whose
crossfade stands in for Movie Maker's crossfades. Titles, captions, narration, music, effects,
animations, other transitions, and a clip's speed, rotation, volume or mute are left out. Each is
counted in the view, and the command line names them.

Import then finds each source missing from its stored path among the collection's members, by name.
These rules choose among the files that carry the name, in order:

1. A file whose length differs by more than half a second from the length the project recorded is
   another file with the same name, and is never used.
2. The only file of that name left is the source.
3. Of several, the source is the one whose folders share the most trailing names with the stored
   path, including at least its own folder.
4. Of several, the source is the one in the folder where most of the project's other sources from
   the same stored folder were found.
5. Otherwise the source stays missing.

A clip whose source stays missing keeps its stored path, and the length the project recorded for an
untrimmed one, so the timeline still shows it and a saved project keeps it for a later relink.
Import measures every source while it runs, and the timeline arrives probed. The view states how
many clips were relinked. Import modifies no file; Save writes the native project.

Newly opened clips are unresolved until an asynchronous probe supplies type, dimensions, duration and
frame rate. A source confirmed missing remains in the timeline with its trim and an error marker.
Relink missing files checks a chosen folder for matching names, updates every match together, and
probes the replacements before clamping trims.

## 7. Rendering

Render is shown only when the running platform provides an encoder. The platform is asked once, on a
worker at startup, because asking starts its media framework; Render appears when it answers. It is
enabled only when every clip has been probed, exists, has a positive retained duration and has
decodable dimensions.

Output is derived without asking for codec settings:

- Dimensions come from the largest displayed video, or the largest photo for a photo-only movie,
  each measured upright.
- Dimensions are fitted within 3840 x 2160, so a portrait movie is at most 2160 pixels tall, and
  rounded down to even values.
- Frame rate is the highest video rate, limited to 60 fps; photo-only movies use 30 fps.
- Audio is 48 kHz stereo, and video bitrate follows output size and rate. An audio track is written
  whenever the timeline holds a video, whether or not any of those videos carries sound.

Windows writes H.264 video and AAC audio in MP4 through Media Foundation. The codec is the system's
to supply rather than Diffractor's to ship: H.264 and AAC carry patent licences that a vendored
encoder would oblige Diffractor to hold, and the outbound licence in [third-party](third-party.md)
is chosen on the assumption it does not. This is why `platform::can_write_movies` exists at all, why
the FFmpeg build is configured without encoders on both platforms, and why Render is absent rather
than dimmed where the platform has no writer.

The render worker receives an
immutable project snapshot, precomputes clip timing once, walks frames in order and uses CPU surfaces
independent of the display device: a driver update or a device-removed reset mid-render would
otherwise destroy an hour of work. It retains at most the two frame decoders and two bounded audio
chunks needed by a crossfade, and hands frames to the encoder only as fast as the encoder takes them,
so a render's memory stays bounded whether composing or encoding is the slower side.

The output is staged in the destination folder. Any missing or undecodable contributing frame fails
the render instead of writing black. A clip whose sound outlasts its picture, as camera files often
do, holds its last frame to its end, because that frame is the nearest one the source has. Cancel abandons the writer and is checked again after encoder
finalization; the staged file replaces the destination only after successful completion. A failure
is reported as a translated reason; the encoder's own diagnostic text goes to the log.

## 8. Ownership and limits

Timeline and view state are UI-thread-owned. Workers consume detached paths, settings and clip
snapshots and publish complete results through the UI queue with generation, revision, path and trim
currency checks. Import reads the collection's members from the index's published snapshot on its
worker rather than walking the disk. The audio-level track reads the focused clip's whole source on a
worker apart from the one that probes, opens and saves; that read is cancelled when focus moves to
another clip or Movie lets its sources go. Paint performs no file, database, decode or network work.

Movie intentionally has one video track, no title or effects track, no music or narration track, no
speed control, no per-clip volume, no pan-and-zoom, no color grading, no output settings, no redo and
no HEVC or AV1 output. Rendering is unavailable where the platform reports no movie writer.

## 9. Command line

The command line runs the same import and render without opening a window. Each takes one project or
a folder, and a folder's projects are those directly inside it. Everything runs on the process's own
thread before any window exists, and no source or Movie Maker project is ever modified.

`-movie-import:<project or folder>` imports each `.wlmp` and writes its native project, named after
it with `.otio`, beside it or into `-movie-out:<folder>`. An existing project is skipped unless
`-movie-overwrite` is given, because it may hold edits. The search covers the collection the app's
settings name, plus any folder given with `-movie-media:<folder>`. The report states what was read
and left out, which stored folder each source moved to, what is ambiguous, missing or unreadable,
and whether each found video is as long as the project recorded.

`-movie-render:<project or folder>` renders native `.otio` projects only, each to its name with
`.mp4`, beside it or into `-movie-out:<folder>`. The movie is staged and published as Render's output
is. An existing movie is skipped unless `-movie-overwrite` is given, and one run never writes the
same file twice. A project with a missing or unreadable source is not rendered, unless
`-movie-skip-missing` is given; that renders the clips that remain and reports how many were left
out.

`-movie-check:<project or folder>` reports what import would make of a `.wlmp`, or what render would
find in an `.otio`, and writes nothing. The exit code is zero only when every project ends ready to
render, or rendered; a project or movie skipped because it already exists is not a failure.
Existing destinations are skipped before the project or its sources are read. Without
`-movie-overwrite`, publication also refuses a destination that appears while the operation runs.

## Where this lives

The timeline, timing, output derivation, project readers, the choice among same-named files for a
moved source, and the decision entering Movie makes about the timeline it holds are in
[model_movie.h](../src/model_movie.h) and [model_movie.cpp](../src/model_movie.cpp).

The task view, timeline controls, preview, project workflows, and the source probe, Movie Maker
import and render worker the command line shares are in [view_movie.h](../src/view_movie.h) and
[view_movie.cpp](../src/view_movie.cpp). Persistent Movie state
is in [model.h](../src/model.h); application entry, shutdown and command availability are in
[app.cpp](../src/app.cpp), [app_commands.cpp](../src/app_commands.cpp) and
[app_toolbar.cpp](../src/app_toolbar.cpp).

The command line batch is [app_movie_batch.cpp](../src/app_movie_batch.cpp). Its switches are parsed
in [app.cpp](../src/app.cpp) into [app_command_line.h](../src/app_command_line.h), and
libdesktop's [platform_win_ui.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_ui.cpp) runs it in place of the window.

Frame and audio extraction are in [av_format.h](../src/av_format.h),
[av_format.cpp](../src/av_format.cpp) and [av_player.h](../src/av_player.h). The audio endpoint the
preview mixes into and the platform writer contract are in libdesktop's [platform_render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform_render.h),
with the Windows implementations in libdesktop's [platform_win_sound.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_sound.cpp) and
[platform_win_encode.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_encode.cpp) and the unavailable-platform answers in
libdesktop's [platform_linux_ui.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_linux_ui.cpp).

Tests are in [test_movie.cpp](../src/test_movie.cpp), [test_view.cpp](../src/test_view.cpp),
[test_av.cpp](../src/test_av.cpp), [test_platform_win.cpp](../src/test_platform_win.cpp) and, for the
command line switches, [test_util.cpp](../src/test_util.cpp).