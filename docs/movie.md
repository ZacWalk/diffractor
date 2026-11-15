# Movie mixer

Movie mixer assembles an ordered list of photos, videos, audio files and screens of text into one MP4
video. It provides ordering, trims, how long a photo or a screen shows, a caption on any clip, a sound file
a project can name to play in place of a clip's own sound, preview, a plain-text project file a person or
an agent can write, rendering, and a command line that renders without a window. Every pair of neighbouring
items crossfades and the movie fades in from black and out to it, with nothing to set. It does not modify,
move or delete source media.

## 1. Behaviour contract

Movie receives photos, videos and audio files from the complete visibly selected set in listing order. The
timeline then becomes an independent ordered document. Rendering writes one MP4 destination and Save
project writes one `.dfv` project; source media remains unchanged. Failed or cancelled writes preserve
an existing destination, and timeline edits are undoable during the session.

## 2. Entry and document lifecycle

**Movie mixer** is a task view. Its command is offered when the visible selection contains at least
one photo, video or audio file, or when an existing Movie timeline can be resumed.

Entering Movie with a selection that differs from the one its timeline was built
from, in its items or their listing order, builds a new timeline from it in listing order. Entering
with the same selection, or with none, returns to the timeline left behind, including edits that were
saved and a project that was opened or saved empty; a project opened in Movie answers to the selection
the timeline it replaced was built from.

Leaving Movie settles unsaved edits. When the timeline has changed since it was built, opened or
saved, Close, Items and Escape offer Save project, Discard or Cancel. An edited timeline that has
had every item removed is still unsaved work; a pristine empty timeline is not. Save writes a valid
project, including one with no items, and leaves once the project is written; Discard throws the
timeline away; Cancel stays with the draft and its undo history. A cancelled or failed save never
counts as permission to discard the timeline. A running render is asked about first. Open and
quitting while Movie is open ask the same question before the timeline is replaced or lost.

Only a change is an edit. Undoing back to the state last built, opened or saved leaves nothing to
ask about, and a handle pressed and released in place, or a slider dragged back to where it began,
changes nothing. The probe filling in a clip's measured length and size is not an edit either, and
nor is Movie hearing whether a clip opens quietly.

Project Open, Save and Relink perform file access on workers. Save writes a sibling temporary file
and replaces the destination only after the complete project has been written. Edits made while a
save is running, including an Undo, remain marked as unsaved. Saving does not wait for a running
render.

## 3. View and timeline

The view contains the shared top bar, a fitted preview, movie transport and scrubber, a one-row
horizontal timeline, and a right-hand controls panel.

Timeline tiles have equal width and play left to right. A clip's tile shows its thumbnail, retained
duration, trim state, missing-source state and playback position, and the first line of its caption on
a dark band across the foot of the picture. A text screen's tile is black with its words, or with
"Text screen" dimmed while it has none. The timeline scrolls horizontally without
growing vertically: by its scroll bar, by either wheel axis or a touchpad, and by holding a dragged
block near either end.

Selection, focus and playback are separate:

- Filled tiles are selected and receive timeline commands.
- One outlined tile is focused and supplies the item controls.
- A bar at the tile foot shows the playhead, under the item playing from its own start: during a
  crossfade, the incoming one.

Plain click selects and focuses one item. Ctrl+click toggles membership without allowing an empty
selection. Shift+click selects from the anchor. Pressing an already selected tile moves focus without
collapsing the selected block, so dragging moves the complete selection. A block keeps its internal
order and focus, and a no-op drop creates no undo entry. Escape during a drag drops nothing: the block,
a trim handle or the scrubber returns to where it was pressed, and Movie stays open.

Files dropped on the timeline insert at the displayed boundary; files dropped elsewhere append. While
files dragged from another application are over the strip, the boundary they would insert at is
drawn, as it is for a block dragged within the strip. Photos, videos and audio files are accepted, from a
drop, a selection or Add files alike, and other files are ignored. Add text screen puts an empty screen
after the focused item, or first in an empty movie, focuses it, and puts the cursor in its text box.
Every change to the timeline is undoable. One Add, drop or Relink is one undo step regardless of how
many files it carries, and one drag of a slider, or one length typed into it, is one undo step however
many values it passes through. So is a run of typing in one item's text; typing that ends where it
began is no edit, and typing in another item's text starts a step of its own.

## 4. Preview and item controls

The preview and renderer use the same timeline timing and composition decision. Frames are fitted,
never stretched or cropped, and unused canvas area is black. Every source is used upright: a photo's
Exif orientation and a video's rotation are applied where its frame is made, so the strip, preview,
output dimensions and rendered file agree. A crossfade holds both contributing items and mixes them
with the same weights it gives their sound, except where [Sound](#sound) keeps one up.

The movie transport provides previous item, play/pause, next item, elapsed time, total time and a
whole-movie scrubber. Selecting an item parks the preview on its first retained frame and stops movie
playback; while the movie plays, previous and next instead count from the item the playhead is
in, not the focus, and carry playback on from the start of the item they reach. An item is the one
playing from its own start, so during a crossfade the incoming one is the one counted from.
Starting movie playback or clip playback stops the other, and a clip whose source cannot be opened
stops the clip player rather than leaving it playing a still picture.

The panel's description adds how many lines of an opened project could not be read and how many clips
have lost their source. Its Movie section states the output: the derived dimensions, frame rate,
length and approximate size. The focused item's section names it, gives its place in the movie and
where it starts, and offers what that kind of item has:

- A video, or an audio file: in and out points, a two-handle range scrubber, retained-region playback,
  endpoint frames, an audio-level track and Reset.
- A video: Stabilize, which marks the clip to be stabilized. The render does not stabilize yet, and
  the box says so; the mark is kept so a later render can.
- A photo: its frame, and how long it shows.
- A text screen: how long it shows, and its words.
- Every clip: its caption.

Lengths are in tenths of a second, from a tenth to a minute. A length shown rounded, such as a
project's 4.25 s, keeps its exact value until the user changes it. A text box takes several lines, and
Return starts a new one. What is typed is the document's at once, so the strip and the preview show it
as it is typed: the preview parks on the item, a screen as the movie shows it and a clip with its
caption. The controls always show the document's values, including after Undo, Open or a change of
focus.

Pressing a trim handle stops playback. The out-point preview shows the last retained frame, one of the
clip's own frames before the out point. While a handle is dragged the preview follows it, and once the
pointer rests the preview shows the frame at the handle, not one near it. Handles drawn on one point
are told apart by which can still move.

Playing preview video uses two persistent frame sessions, bound to clip identity and opened ahead of a
crossfade. Photo and parked frames come from the bounded frame cache, and each preview slot keeps the
frame it is showing, so the two photos of a crossfade stay drawn however large the preview. A text
screen is drawn whole at the size the preview shows it, and the last two are kept. A parked frame is
the source frame at the playhead, to half a frame of the clip's own. The playing preview is composed
at the movie's own frames, as the render composes them, so the view repaints when the playhead reaches
another one rather than on every tick of the display; the clip player repaints when its session
presents a new frame. Preview audio is decoded
off the UI thread in bounded windows of each contributing clip's retained interval, held by source
window so a crossfade's incoming clip keeps its sound, and read ahead of the playhead. It is mixed at
the endpoint rate and the user's media volume into an endpoint ring long enough to outlast the UI tick
that refills it. When playback starts or jumps, the picture waits briefly for its sound so the two
begin together; a window still unread after that plays as silence rather than stalling, and sound that
falls behind the picture restarts at the playhead. A seek discards queued sound from the old position.

### Audio clips

An audio file has no picture of its own, so its clip shows the file as Diffractor plays it, on a
landscape 1920 x 1080 frame at 30 fps under the file's title and who made it: a SID tune's or a
tracker module's monitor, following the music, or the visualizer's bars beside the file's cover art,
as large as the frame holds. The monitor is laid out for the frame by the stack's own rules, so a SID
monitor has the width to stand its output over its filter beside the register grid, and it shows
nothing to press: no tune selector, mute, solo or Follow. A tracker's pattern scrolls smoothly through
its bar in step with the sound. The frames are drawn by the software renderer, the same for
the strip, the preview and the render on any machine, so the glass Direct3D gives the bars in the
Media view is not in a movie. A SID file holds several tunes, and its clip plays one: the tune its
file starts on until the panel's Track chips, under the trim, choose another, which the clip then
plays whole, for the length the Media view finds for it: to its end, faded where it repeats, or three
minutes. The tile names the tune where the file holds more than one, and the project keeps it. Its
clip trims, plays and sounds as a video's does, and the clip player hears the file through the
ordinary player.

## 5. Text, crossfades and sound

Movie has no settings. Every rule below is fixed, so a project means the same movie wherever it is
rendered.

### Text screens and captions

A text screen is its words on black, for its own length: five seconds when it is added. A movie
usually opens and closes on one, and can have one anywhere between. A screen with no words is black.

A caption belongs to a clip. It appears once the crossfade bringing its clip in has finished, so the
words arrive with their picture, and shows for as long as it takes to read: two seconds to notice it,
then fifteen characters a second, never less than three seconds nor more than twelve. Characters are
counted rather than bytes, so accented text reads no slower. A caption longer than its clip runs on over
the clips after it until the next caption begins, a text screen begins or the movie ends, so two
captions never show at once and none shows over a screen. A caption fades in and out over half a
second, or a third of its length when that is shorter. An item holds at most 2,000 bytes of text.

Text is white Segoe UI with a dark edge, wrapped within 84% of the frame's width and sized to the
frame's shorter side, so a portrait movie's text reads as well as a landscape one's. A screen's words
are bold, a twelfth of that side, and centred in the frame. A caption is a twentieth, centred across the
foot of the frame, 6% above its bottom edge. Text too large for 84% of the frame's width and 90% of its
height is set smaller until it fits, so no line is cut off. A caption is drawn over the pictures and
under the movie's fades. The preview lays text out on the movie's frame fitted into the preview, as the
render does on the full frame, and draws it at the preview's size.

### Crossfades and fades

Every pair of neighbouring items crossfades for one second, held to half the shorter of the two, so
an item is never eaten by the crossfades on both of its sides. A text screen crossfades as a photo
does. The movie fades in from black and out to black over the same second, picture and sound.

### Sound

The sound of a clip going out normally fades as its picture does, while the next one's fades in. When
the item coming in has no sound worth hearing, the clip going out keeps its sound at full volume to its
own end, so words said as the crossfade begins are heard, and lets go over its last 50 milliseconds
rather than stopping dead. An item has no sound worth hearing when it is a photo or a text screen with no
sound file, a clip whose source or sound file is gone, or a clip nearly silent where it comes in: its first
second, all a crossfade mixes, at or below about -40 dBFS RMS. A video with no sound track is silent.

A clip may carry a sound file, which only a [project file](#6-project-files) names: an audio file, or a
video for its sound. It plays in place of the clip's own sound, which is then not heard, from the sound
file's start as the clip begins, whatever the clip's in point, for as long as the clip shows: a shorter
one falls silent and a longer one is cut where the clip ends. In every other way it is the clip's sound.
A crossfade fades it as it would the clip's own, a photo or an audio clip may carry one as a video may,
and one that opens on a nearly silent second counts as no sound worth hearing, so a narration that starts
a second in is heard whole, and the clip before it keeps its own sound to its end. The preview and the
render play it; the clip player in the panel plays the clip's own source.

Movie hears a clip's opening the first time it needs to: the preview from the first window of the
clip's sound it reads, and the render, which trusts nothing the preview heard, when a crossfade first
reaches the clip. Until then the clip counts as having sound. A trim that moves its in point makes it
unknown again, and so does a relinked file, a sound file probed again, or another of a SID file's tunes.
There is no music track: an audio file is a clip of its own, with its picture.

## 6. Project files

A project is a `.dfv` file: UTF-8 text, one item per line in the order they play, so a person can read
it and an agent can write it. Open and Save offer only `.dfv` projects. A saved project may have no
items.

```text
# Diffractor video: one item per line, played in order.
t 4 'Hiking to Everest\nOctober 2019'
c 12.5-31 2019/kathmandu.mp4 'Kathmandu'
s 1:02-1:20.5 "2019/day 3/climb.mp4"
c -6 2019/lukla.jpg 'The airstrip at Lukla' !narration/lukla.wav
c #3 music/commando.sid
t 3 'The End'
```

Each line is one of:

- `t <length> ['<text>']`: a text screen showing its text for the length.
- `c <part> <file> ['<caption>'] [!<sound file>]`: a clip of the file, with its caption if it has one, and the
  sound file it plays in place of its own sound if it names one.
- `s <part> <file> ['<caption>'] [!<sound file>]`: the same clip, marked to be stabilized.

Blank lines and lines starting with `#` are comments. Spaces around a line are ignored, the letter may
be either case, and a UTF-8 byte order mark and Windows line ends are accepted.

A part is `<from>-<to>`, either of which may be left out, and may end in `#<track>` to play one of a
SID file's tunes: `#3` alone is the whole of tune 3, and `-` alone the whole file. A time is seconds
(`83.5`), minutes and seconds (`1:23.5`) or hours, minutes and seconds (`1:01:23.5`); only its last
part has a fraction, and the parts after the first are under 60. A video or audio clip runs from
`from`, or its start, to `to`, or its end, so an untrimmed clip follows its source whatever length it
turns out to be. A photo shows for the part's length, so `-6` is six seconds, and for four seconds with
no `to`.

Fields are separated by spaces, and one with a space in it is quoted. A `"double-quoted"` field is
everything to the next double quote as it stands, which suits a path. A `'single-quoted'` field reads
`\n` as a new line, and `\'`, `\"` and `\\` as the character; any other backslash is itself. A closing
quote is followed by a space or the end of the line.

A field starting with `!` names a clip's sound file, as `!narration.wav`, or quoted straight after the `!`
when its path has a space, as `!"voice over.wav"`. It comes after the clip's file, before or after its
caption, at most once, and a text screen takes none, so a caption that starts with `!` is quoted.

A file is a full path, or a path relative to the project's folder; either `/` or `\` separates its
names. Save writes a source beneath the project's folder relative to it, with `/`, so the two move
together, and any other source in full; a sound file is written the same way, after the caption, and a
file whose name starts with `!` is quoted. It writes times to the millisecond, leaves out an in point at
the start and an out point at the end, and puts text in single quotes.

A photo's or a screen's length is held to between a tenth of a second and a minute. A line Movie
cannot use is skipped: Open states how many in the panel, and the command line names each with its
number and the reason, and renders nothing. An empty file is not a project, and nor is one with lines
of which Movie could use none; a file of comments alone is an empty project.

Newly opened clips are unresolved until an asynchronous probe supplies type, dimensions, duration and
frame rate. A source confirmed missing remains in the timeline with its trim and an error marker, and
is saved as it was written. A sound file is probed too: one missing, or with no sound to play, keeps its
clip from rendering, is counted among the clips that cannot find their source files, and is saved as it
was written. Relink missing files checks a chosen folder for matching names, updates every match together,
and probes the replacements before clamping trims; it does not look for sound files.

## 7. Rendering

Render is shown only when the running platform provides an encoder. The platform is asked once, on a
worker at startup, because asking starts its media framework; Render appears when it answers. It is
enabled only when every clip has been probed, exists, has a positive retained duration and has
decodable dimensions, and every sound file a clip names has been found with sound to play; a text screen
needs no source. Render is the task's own command, so it ends the
status bar and is the last of Movie's commands to give way when the bar is short. While a render runs,
the timeline it works from takes no edits: adding, removing, opening, relinking, reordering, a drop of
files, Delete and Undo all wait for it, while the timeline can still be looked through.

Output is derived without asking for codec settings:

- Dimensions come from the shape most of the movie is in, by how long each clip shows, and the largest
  video of that shape; a photo-only movie uses its photos the same way. Each source is measured
  upright, and an audio clip counts as a video of its 1920 x 1080 frame. So one 4:3 clip does not put
  black bars round every 16:9 one, nor does one portrait clip stand a landscape movie up. A text screen
  has no size of its own and is drawn on the frame the clips make; a movie of text screens alone is
  1920 x 1080.
- Dimensions are fitted within HD, 1920 x 1080 or 1080 x 1920 for a portrait movie, and rounded down to
  even values. 4K sources are scaled down: a few 4K clips among HD ones would otherwise make the whole
  movie 4K, several times the file for detail the HD clips do not have.
- Frame rate is 24, 25 or 30 fps, whichever most of the movie's video was filmed for, by how long each
  clip shows; an even split is 30. Each clip counts for the rate its footage divides into evenly -- 60
  and 15 fps count as 30, 50 as 25 and 48 as 24 -- so only footage filmed for another of the three has
  frames repeated or dropped unevenly. A video's rate is the one its frames come at: a phone's
  variable-rate video declares 60 or 120 while its frames average 30, and counts as 30. An audio clip
  counts as 30 fps, and a movie of photos and text screens alone is made at 30.
- Audio is 48 kHz stereo, and video bitrate follows output size and rate. An audio track is written
  whenever the timeline holds a video, an audio file or a clip with a sound file, whether or not any of
  those videos carries sound.

Windows writes H.264 video and AAC audio in MP4 through Media Foundation. The codec is the system's
to supply rather than Diffractor's to ship: H.264 and AAC carry patent licences that a vendored
encoder would oblige Diffractor to hold, and the outbound licence in [third-party](third-party.md)
is chosen on the assumption it does not. This is why `platform::can_write_movies` exists at all, why
the FFmpeg build is configured without encoders on both platforms, and why Render is absent rather
than dimmed where the platform has no writer.

The render worker receives an
immutable project snapshot, precomputes item timing once, walks frames in order and uses CPU surfaces
independent of the display device: a driver update or a device-removed reset mid-render would
otherwise destroy an hour of work. It retains at most the two frame decoders, two bounded audio
chunks and two text screens needed by a crossfade, and hands frames to the encoder only as fast as the
encoder takes them, so a render's memory stays bounded whether composing or encoding is the slower side.
A chunk of sound is kept by the file it comes from, apart from the picture beside it, so a sound file is
read on its own and asking for a clip's sound never opens its picture.

The output is staged in the destination folder. Any missing or undecodable contributing frame fails
the render instead of writing black. A clip whose sound outlasts its picture, as camera files often
do, holds its last frame to its end, because that frame is the nearest one the source has. Cancel abandons the writer and is checked again after encoder
finalization; the staged file replaces the destination only after successful completion. A failure
is reported as a translated reason; the encoder's own diagnostic text goes to the log.

## 8. Ownership and limits

Timeline and view state are UI-thread-owned. Workers consume detached paths and clip snapshots and
publish complete results through the UI queue with generation, revision, path and trim currency checks.
The audio-level track reads the focused clip's whole source on a worker apart from the one that probes,
opens and saves; that read is cancelled when focus moves to another clip or Movie lets its sources go.
Paint performs no file, database, decode or network work.
An audio clip's frames are drawn on whichever worker asks for them - the render queue for the strip and
the preview, the render's own worker - into a display and a monitor made for that clip, which no view
shares and one thread at a time draws; a SID tune's render is shared by every reader of the clip's
pictures and sound, and its reads are its own to serialise.

Movie intentionally has one video track, one crossfade for every join and one fade at each end, text
only as screens on black and a caption on each clip, in one style with no animation but its fade, no
music or narration track under several clips - an audio file is a clip of its own, with its picture, and
a clip's sound file plays only while that clip shows - no sound file set from the panel, no speed
control, no per-clip volume, no pan-and-zoom, no color grading, no stabilization yet, no settings, no
redo, no output larger than HD or faster than 30 fps, and no HEVC or AV1 output. Rendering is
unavailable where the platform reports no movie writer.

## 9. Command line

The command line checks and renders projects without opening a window, so an agent that writes a
`.dfv` can make its movie. Each switch takes the full path of one project or of a folder, whose projects
are the `.dfv` files directly inside it. Everything runs on the process's own thread before any window
exists, and no source or project is ever modified.

`-movie-check:<project or folder>` reads each project and probes its sources and sound files, and writes
nothing. It reports each line that could not be read, with its number and why; how many videos, photos,
audio files and text screens the movie holds, how long it is and the output it would make; where each
text screen starts; how many clips carry a caption and how many are marked to stabilize; which sources
and sound files are missing or unreadable; each clip whose part starts at or past the end of its source,
so plays nothing; and each sound file longer than its clip, which the clip cuts short.

`-movie-render:<project or folder>` reports the same, then renders each project to its name with
`.mp4`, beside it or into `-movie-out:<folder>`. The movie is staged and published as Render's output
is. An existing movie is skipped unless `-movie-overwrite` is given, and one run never writes the same
file twice. A project with a line that could not be read is not rendered, since that line is part of
the movie its writer meant. Nor is one with a clip it cannot play, a clip whose sound file cannot be
played among them, unless `-movie-skip-missing` is given; that renders the items that remain and reports
how many were left out, but not the text screens of a project whose every clip was left out.

The exit code is zero only when every project ends ready to render, or rendered; a movie skipped
because it already exists is not a failure. Existing destinations are skipped before the project or its
sources are read. Without `-movie-overwrite`, publication also refuses a destination that appears while
the operation runs.

## Where this lives

The timeline, timing, captions and sound rules, output derivation, the `.dfv` reader and writer, and
the decision entering Movie makes about the timeline it holds are in
[model_movie.h](../src/model_movie.h) and [model_movie.cpp](../src/model_movie.cpp).

The task view, timeline controls, preview, the drawing of text screens and captions, project workflows,
and the source and sound-file probes and render worker the command line shares are in [view_movie.h](../src/view_movie.h) and
[view_movie.cpp](../src/view_movie.cpp); an audio clip's frames, length and sound are in
[view_movie_audio.h](../src/view_movie_audio.h) and [view_movie_audio.cpp](../src/view_movie_audio.cpp). Persistent Movie state
is in [model.h](../src/model.h); application entry, shutdown and command availability are in
[app.cpp](../src/app.cpp), [app_commands.cpp](../src/app_commands.cpp) and
[app_toolbar.cpp](../src/app_toolbar.cpp).

The command line batch is [app_movie_batch.cpp](../src/app_movie_batch.cpp). Its switches are parsed
in [app.cpp](../src/app.cpp) into [app_command_line.h](../src/app_command_line.h), and
libdesktop's [platform::run](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/app.cpp) runs it in place of the window.

Frame and audio extraction are in [av_format.h](../src/av_format.h),
[av_format.cpp](../src/av_format.cpp) and [av_player.h](../src/av_player.h). The audio endpoint the
preview mixes into and the platform writer contract are in libdesktop's [platform/media.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/platform/media.h),
with the Windows implementations in libdesktop's [sound.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/sound.cpp) and
[movie_writer.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/movie_writer.cpp) and the unavailable-platform answers in
libdesktop's [Linux backend](https://github.com/ZacWalk/libdesktop/tree/main/src/platform/linux).

Tests are in [test_movie.cpp](../src/test_movie.cpp), [test_view.cpp](../src/test_view.cpp),
[test_av.cpp](../src/test_av.cpp), [test_platform_win.cpp](../src/test_platform_win.cpp) and, for the
command line, [test_util.cpp](../src/test_util.cpp) for its switches and [test_app.cpp](../src/test_app.cpp)
for the batch.
