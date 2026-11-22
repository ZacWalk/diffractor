// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Movie document. An ordered list of items - clips of photos, videos and audio files shown as
// Diffractor plays them, and text screens on black - the captions and sound files clips carry, when each
// item, caption and sound plays, the derived output geometry, the .dfv project file it is saved as, and
// what entering Movie does with the timeline it holds. docs/movie.md owns the behaviour; nothing here
// decodes, encodes, draws or touches the UI.

#pragma once

// Every pair of neighbouring items crossfades for this long, and the movie fades in from black and out
// to black over the same. One length for all of it, so a movie has no transition settings to learn.
inline constexpr double movie_crossfade_seconds = 1.0;

// How long a photo is held when it is added, and how long a new text screen shows.
inline constexpr double movie_photo_seconds = 4.0;
inline constexpr double movie_screen_seconds = 5.0;

// The bounds any document holds a photo's or a text screen's length to: the panel cannot set one
// outside them, and a project file is untrusted input.
inline constexpr double movie_min_hold_seconds = 0.1;
inline constexpr double movie_max_hold_seconds = 60.0;

// A text screen or a caption is a few words, not a document. Longer text from a file is cut here, at a
// character's start.
inline constexpr size_t movie_max_text_bytes = 2000;
// How long a caption takes to appear and to go. A shorter caption fades over a third of its length.
inline constexpr double movie_text_fade_seconds = 0.5;

// A caption shows for as long as it takes to read: a moment to notice it, then fifteen characters a
// second, never less than three seconds nor more than twelve.
inline constexpr double movie_caption_notice_seconds = 2.0;
inline constexpr double movie_caption_characters_per_second = 15.0;
inline constexpr double movie_caption_min_seconds = 3.0;
inline constexpr double movie_caption_max_seconds = 12.0;

double movie_caption_seconds(std::string_view text);

// The same text held to the bounds a document keeps, cut at a character's start.
std::string bound_movie_text(std::string_view text);

// A clip's sound at or under this level, as the root mean square of full scale, where a crossfade brings
// it in is nearly silent: about -40 dBFS, below a quiet room on most cameras.
inline constexpr double movie_quiet_level = 0.01;
// A sound carried through a crossfade lets go over this last moment rather than stopping dead, which
// clicks.
inline constexpr double movie_sound_release_seconds = 0.05;

// Whether interleaved 16-bit samples are nearly silent. No samples at all are: a video with no sound
// track decodes to none.
bool movie_sound_is_quiet(std::span<const int16_t> samples);

// One item in the timeline. A text screen is its words on black for `end` seconds; it has no file. A
// photo has no source duration of its own, so `end` is the length it is held for and `start` is always
// zero; a video is trimmed to [start, end) of its source, and so is an audio file, which has no picture
// of its own and is shown as Diffractor plays it: a SID tune's or a tracker module's monitor, or the
// visualizer over the file's cover art.
struct movie_clip
{
	core::file_path path;
	bool is_text = false;
	bool is_photo = false;
	bool is_audio = false;
	// Marked to be stabilized. Kept and saved; the render does not stabilize yet.
	bool stabilize = false;
	// True until the source has been probed and found. The project reader does no I/O, so a freshly
	// read project starts with every clip missing and the view resolves them; a clip that stays
	// missing keeps its stored times, so the trim survives a relink. A text screen has no source and is
	// never missing.
	bool is_missing = true;
	// Whether the probe has answered for this clip at all. Not saved: it is the difference between
	// "we have not looked yet" and "we looked and it is gone", and only the second is a thing to
	// tell the user about or to offer to repair.
	bool is_probed = false;
	double source_duration = 0;
	double start = 0;
	double end = 0;
	sizei extent;
	double frame_rate = 0;
	// The tune of a SID file the clip plays, from 1; 0 for the one the file starts on, as for any other file.
	int song = 0;
	// How many tunes the file holds, as the probe found: 1 for anything but a SID file with more. Not saved.
	int songs = 1;
	// A text screen's words, or the caption a clip carries. A caption shows once its clip has crossfaded
	// in, for as long as it takes to read, and runs on over the clips after it if its own is shorter.
	std::string text;
	// A sound file played in place of the clip's own sound (`!<file>` in a project): from that file's start,
	// as the clip begins, for as long as the clip shows. Empty for the clip's own sound.
	core::file_path sound_file;
	// Measured, not saved, as the source is: whether the sound file has been probed, whether it was there
	// with sound to play, and for how long it plays.
	bool sound_file_is_probed = false;
	bool sound_file_is_missing = true;
	double sound_file_duration = 0;
	// Measured, not saved: whether the clip's sound is nearly silent where a crossfade brings it in, as
	// heard from the in point `sound_checked_at`. A trim since then leaves it unknown, which counts as sound.
	double sound_checked_at = -1;
	bool sound_is_quiet = false;
	// Measured, not saved: where a SID tune starts again, in seconds of its source, when its scan found it to
	// repeat; zero for anything else. A Short of that tune alone ends there, so YouTube's loop of it is seamless.
	double loop_end = 0;

	double duration() const { return std::max(0.0, end - start); }

	// Probed, and not there. Distinct from not yet probed, which looks identical in the document and
	// means the opposite to the user.
	bool is_lost() const { return is_probed && is_missing; }

	bool is_trimmed() const
	{
		return !is_photo && !is_text && (start > 0.0001 || (source_duration > 0 && end < source_duration - 0.0001));
	}

	bool has_sound_file() const { return !sound_file.is_empty(); }

	// The sound file was probed and is not there, or has no sound to play.
	bool is_sound_file_lost() const { return has_sound_file() && sound_file_is_probed && sound_file_is_missing; }

	// Nothing worth hearing where a crossfade brings this item in: a text screen or a photo with no sound
	// file, a source or sound file that is gone, or a clip measured as nearly silent there. One not yet
	// probed is not known, so counts as sound.
	bool has_no_sound() const
	{
		if (is_lost() || is_sound_file_lost() || (sound_is_quiet && sound_checked_at == start)) return true;
		return !has_sound_file() && (is_text || is_photo);
	}

	// Compared so an edit that changes nothing can be told from one that does: a no-op must not
	// cost an undo step or mark the timeline as holding unsaved work.
	bool operator==(const movie_clip&) const = default;
};

// Where an item's sound comes from, and the part of that file heard while the item shows: its sound file
// from that file's start, or its own source over the part it plays. Empty for a photo or a text screen
// with no sound file. The preview and the render both read an item's sound from here.
struct movie_sound
{
	core::file_path path;
	// Which of a SID file's tunes, as movie_clip::song; a sound file plays the tune it starts on.
	int song = 0;
	double start = 0;
	double end = 0;

	bool is_empty() const { return path.is_empty() || end <= start; }
};

movie_sound movie_clip_sound(const movie_clip& clip);

// Everything the render is told, all of it derived. docs/movie.md#7-rendering states the rules; this
// is only their result, and the view displays it rather than offering it.
struct movie_output
{
	sizei extent;
	int frame_rate = 30;
	int video_bitrate = 0;
	int audio_bitrate = 192000;
	int sample_rate = 48000;
	int channels = 2;

	bool operator==(const movie_output&) const = default;
};

// Ceilings the derivation clamps to. A movie is HD at most: 1920 x 1080, or 1080 x 1920 standing up. A
// few 4K clips among HD ones would otherwise make every frame 4K, several times the file for detail the
// HD clips do not have. Nor may a photo-only timeline make an 8000-pixel movie from a 45-megapixel
// source, and no codec here accepts an odd dimension.
inline constexpr int movie_max_long_side = 1920;
inline constexpr int movie_max_short_side = 1080;

// The rates a movie is made at: the three every player and television shows smoothly. Between them they
// take any camera's footage with no frame repeated or dropped unevenly -- 60 and 15 fps footage plays at
// 30, 50 at 25, 48 at 24 -- and a movie is made at whichever most of its video was filmed for. The first
// wins a tie.
inline constexpr std::array movie_frame_rates{30, 25, 24};
inline constexpr int movie_max_frame_rate = 30;
inline constexpr int movie_default_frame_rate = 30;

// The frame an audio clip is drawn in, landscape HD, and the rate its monitor is drawn at. It counts as a
// video of that size and rate when the output is derived.
inline constexpr sizei movie_audio_extent{1920, 1080};
inline constexpr int movie_audio_frame_rate = 30;

// What a movie is made for. A YouTube Short stands upright, runs three minutes at most, and keeps what matters clear
// of what YouTube lays over it. docs/movie.md#youtube-shorts owns the rules.
enum class movie_kind
{
	movie,
	youtube_short,
};

// A Short is upright HD whatever shape its sources are: one of another shape sits whole in the middle over a
// blurred, darkened copy of itself that fills the frame, and an audio clip is drawn upright.
inline constexpr sizei movie_short_extent{1080, 1920};
// YouTube takes a vertical video of three minutes or less as a Short. A Short ends this far inside that, so the
// encoder's rounding of the last frame and its sound's priming cannot carry it over.
inline constexpr double movie_short_max_seconds = 179.5;
// A Short moves as smoothly as its sources were made to, up to 60 frames a second: a PAL SID tune's monitor at 50,
// an NTSC one's at 60, a video at its own rate. An audio clip's frame_rate is the rate its pictures move at, which
// only a Short uses; a movie counts every audio clip at movie_audio_frame_rate.
inline constexpr std::array movie_short_frame_rates{60, 50, 30, 25, 24};
inline constexpr int movie_short_still_frame_rate = 30;
// What of a Short YouTube covers: the bottom fifth with the title, the channel and the progress bar, and the
// right edge with its buttons. Words keep clear of both; an audio clip's picture fills the frame beneath them, as a
// video's does.
inline constexpr double movie_short_bottom_reserve = 0.2;
inline constexpr double movie_short_side_reserve = 0.12;

// The rate a video's frames come at. A phone's variable-rate video declares a rate of 60 or 120 while
// its frames come 30 a second, and one such clip must not make every frame of a movie twice over; a
// video whose frames do come as fast as it declares keeps its rate. Either may be unknown, as zero.
double movie_source_frame_rate(double declared, double average);

// What Movie takes, wherever it comes from: photos, videos and audio files.
class file_type;
bool is_movie_source(const file_type* type);

// Bits per pixel per frame. 1920x1080x30 lands at ~6.2 Mbit/s, which is what a consumer H.264
// encoder wants for camera footage.
inline constexpr double movie_bits_per_pixel = 0.10;
inline constexpr int movie_min_bitrate = 1000000;
inline constexpr int movie_max_bitrate = 60000000;

// A day of video at the highest rate the output can take. Past this the frame count stops fitting
// the arithmetic that walks it, and a movie that long is a broken document rather than a request.
inline constexpr int64_t movie_max_frames = 24ll * 60ll * 60ll * movie_max_frame_rate;

// The most any one clip may cover: the longest movie a render will make. A project file's clip range
// past it is a broken document, and it would reach the frame arithmetic and the displayed clock as a
// number no integer can hold.
inline constexpr double movie_max_clip_seconds = static_cast<double>(movie_max_frames) / movie_max_frame_rate;

movie_output derive_movie_output(const std::vector<movie_clip>& clips, movie_kind kind = movie_kind::movie);

// Seconds two neighbouring items overlap: the crossfade, held to half the shorter of the two, so a
// crossfade longer than the item it joins shortens rather than consuming it, and no item can be eaten
// entirely by the crossfades on both of its sides.
double movie_overlap(const movie_clip& left, const movie_clip& right);

// When one caption shows on the finished movie, already cut short where the next caption or a text
// screen begins.
struct movie_text_span
{
	// The clip whose caption it is.
	int index = -1;
	double start = 0;
	double end = 0;

	bool is_shown() const { return end > start; }
};

// Start time of each item on the finished movie's timeline, plus the total. Both answers come from
// one walk because the caller that draws the timeline needs both and they must not disagree. Every
// caption's span comes from the same walk, for the same reason.
struct movie_timing
{
	std::vector<double> starts;
	double duration = 0;
	// In the order they begin.
	std::vector<movie_text_span> captions;
	// Whether the movie fades in from black and out to it, which a Short that loops seamlessly does not.
	bool fades = true;
	// A Short whose items run past three minutes ends there, fading out as any movie does; `whole` is how long
	// its items would have run.
	bool cut = false;
	double whole = 0;
};

movie_timing calc_movie_timing(const std::vector<movie_clip>& clips, movie_kind kind = movie_kind::movie);

// A Short of one SID tune that repeats within three minutes, played from its start: it ends where the tune
// starts again, with no fade at either end, so YouTube's loop of it never shows a join.
bool movie_loops_seamlessly(const std::vector<movie_clip>& clips, movie_kind kind);

// One item's contribution to one frame.
struct movie_frame_source
{
	int index = -1;
	// Seconds into the source file, already offset by the clip's in point. A photo and a text screen
	// answer 0.
	double source_time = 0;
	// Seconds into the file its sound comes from (movie_clip_sound): the source time for its own sound,
	// and how long it has been showing for a sound file.
	double sound_time = 0;
	// How much of the picture it makes.
	double weight = 0;
	// How loud its sound is mixed: its weight, except that a clip going out under an item with no sound
	// of its own stays at full volume, so words said as the crossfade begins are still heard.
	double sound = 0;
};

enum class movie_text_kind
{
	none,
	// A text screen's words, in the middle of the frame.
	screen,
	// A clip's caption, in the frame's lower band.
	caption,
};

// The caption a frame shows, and how far it has faded in.
struct movie_frame_text
{
	// The clip whose caption it is.
	int index = -1;
	double opacity = 0;

	bool is_shown() const { return index >= 0 && opacity > 0; }
};

// What the compositor draws at one instant. At most two items contribute: a crossfade joins exactly
// two, and the half-item ceiling in movie_overlap stops a third reaching across a short one. A text
// screen contributes its words on black, as a photo contributes its picture.
//
// This is the decision; drawing it is separate, so the preview and the render cannot disagree about
// what a frame contains and neither needs a window to be tested.
struct movie_frame
{
	movie_frame_source a;
	movie_frame_source b;
	// Drawn over the pictures and under the fade.
	movie_frame_text caption;
	// How far the movie's own fade from or to black has gone: 0 draws the items untouched, 1 is black.
	double fade_to_black = 0;

	// No picture: black.
	bool is_empty() const { return a.index < 0; }
};

movie_frame calc_movie_frame(const std::vector<movie_clip>& clips, double time);
movie_frame calc_movie_frame(const std::vector<movie_clip>& clips, const movie_timing& timing, double time);

// How loud the item `index` plays in `frame`, the movie's own fade included; zero when it does not play.
double movie_sound_level(const movie_frame& frame, int index);

// Builds a clip from a path. The duration and extent come from the caller because probing is I/O;
// a photo is held for the default length.
movie_clip make_movie_clip(core::file_path path);
// A text screen showing `text` on black for `seconds`, held to the document's bounds.
movie_clip make_movie_text(std::string_view text, double seconds = movie_screen_seconds);

// True when a proposed output path names one of the files the render reads: a clip's source or its sound
// file. The render replaces the output only after its last read, so an output that names a source destroys
// that source silently and leaves the timeline pointing at a file that is now the movie. Compared by path,
// which is the identity every other destination-writing operation uses.
bool movie_output_names_a_clip(const std::vector<movie_clip>& clips, core::file_path output);

enum class movie_load_status
{
	ok,
	unreadable,
};

// A line of a project file the reader could not use, and why, in words for the person or agent who
// wrote it.
struct movie_load_problem
{
	// From 1, as an editor counts them.
	int line = 0;
	std::string reason;
};

struct movie_load_result
{
	movie_load_status status = movie_load_status::unreadable;
	std::vector<movie_clip> clips;
	std::vector<movie_load_problem> problems;
	movie_kind kind = movie_kind::movie;

	explicit operator bool() const { return status == movie_load_status::ok; }
};

// The project file: a .dfv, plain text with one item per line in the order they play, which a person or
// an agent can write by hand. docs/movie.md#6-project-files owns the grammar. A path beneath the
// project's folder is written relative to it, so the two move together; any other is written whole.
std::string write_dfv(const std::vector<movie_clip>& clips, core::folder_path project_folder,
                      movie_kind kind = movie_kind::movie);
// Reads every line it can. A line it cannot use is skipped and named in `problems`; a file with
// problems and no item at all is not a project.
movie_load_result read_dfv(std::string_view text, core::folder_path project_folder);

// Which control an edit came from, where that control reports one gesture as a stream of values: a
// slider dragged across fifty values, or text typed a key at a time. Edits naming the same group
// merge into one undo step until the gesture settles or anything else happens, so one drag is one
// edit, one Undo takes it back, and a drag that ends where it began is no edit at all.
enum class movie_edit_group
{
	none,
	// The length a photo is held for or a text screen shows.
	hold,
	// A text screen's words or a clip's caption, a keystroke at a time.
	text,
};

// The timeline as a document: ordered, editable, and undoable. UI-thread-owned; the render takes a
// detached copy of `clips()` and never reads this object again.
class movie_project
{
public:
	// Deep enough that an accidental multi-select delete is recoverable, bounded because each entry
	// is a copy of the whole item list.
	static constexpr size_t max_undo = 64;

	const std::vector<movie_clip>& clips() const { return _clips; }
	core::file_path path() const { return _path; }
	bool is_modified() const { return _modified; }
	// Advances on every change to the document, including the probe filling in what it measured.
	uint64_t revision() const { return _revision; }
	// Advances only on what the user did -- an edit, an undo, a replacement. Save and Open ask whether
	// the user changed the timeline while they ran, and a probe landing meanwhile is not an answer.
	uint64_t edit_revision() const { return _edit_revision; }
	size_t size() const { return _clips.size(); }
	bool is_empty() const { return _clips.empty(); }
	bool is_ready_to_render() const;

	// Index of the item the controls panel describes and the strip marks. Always in range while the
	// timeline is not empty, so the panel never has to test it. Focus is one item; the selection is
	// a set, and the two are different facts.
	size_t current() const { return _current; }
	void current(size_t i);
	const movie_clip* current_clip() const;

	// Sorted, and empty only when the timeline is. A command that acts on "the items" acts on this.
	const std::vector<size_t>& selected() const { return _selected; }
	bool is_selected(size_t i) const;
	// One click's worth of selection, by the rules every list selects by: plain replaces, `extend` runs from the
	// anchor - with `toggle` too, adding the run - and `toggle` alone adds or removes one. Focus always lands on
	// `index` whichever it was.
	void select(size_t index, bool extend, bool toggle);
	// Moves focus without changing the selected set. Used while a press inside an existing
	// multi-selection may become a block drag.
	void focus(size_t index);
	void select_all();

	void append(const movie_clip& clip);
	void insert(size_t at, const movie_clip& clip);
	// Inserts several items as one document edit, so one Add or one drop is one undo step. Focus
	// and the selection land on the first item inserted.
	void insert_many(size_t at, const std::vector<movie_clip>& clips);
	void remove(size_t at);
	void move(size_t from, size_t to);
	// Moves every selected item to the drop point, keeping their order relative to each other and
	// carrying focus with them. `to` counts positions in the list as it stands before the move.
	void move_selection(size_t to);
	void remove_selection();
	// A replacement equal to the item it replaces is not an edit.
	void replace(size_t at, const movie_clip& clip);
	// Replaces several items as one document edit and therefore one undo step, and none at all when
	// every replacement equals what is already there.
	void replace_many(const std::vector<std::pair<size_t, movie_clip>>& replacements);
	// For a change the user did not make and cannot want to undo: the probe filling in a clip's
	// duration and extent, or a measure of its sound. An undo entry here would make Ctrl+Z appear to do
	// nothing.
	void replace_quietly(size_t at, const movie_clip& clip);

	// A text screen's words or a clip's caption, held to the bounds a document keeps. Text equal to the
	// item's own is not an edit, and typing in one item's box and then another's is two steps.
	void text(size_t at, std::string_view words, movie_edit_group group = movie_edit_group::none);

	// What the movie is made for. A change is an edit, undone with the rest.
	movie_kind kind() const { return _kind; }
	void kind(movie_kind kind);

	// A photo's or a text screen's length is its end, from a start of zero. Equal ranges are not an
	// edit, so a handle pressed and released in place costs nothing.
	void trim(size_t at, double start, double end, movie_edit_group group = movie_edit_group::none);

	// The control that reported a gesture has let go, so the next edit, from it or anything else, is
	// a step of its own.
	void end_edit_group();

	void reset(std::vector<movie_clip> clips, core::file_path path, movie_kind kind = movie_kind::movie);
	void mark_saved(core::file_path path, uint64_t edit_revision);

	// A timeline just built from a selection has no state before itself, so there is nothing to
	// undo back to and nothing yet for the user to have changed.
	void mark_seeded();

	bool can_undo() const { return !_undo.empty(); }
	// Undoing back to the state last saved, seeded or opened leaves the timeline unmodified: there is
	// nothing left to lose, so leaving Movie has nothing to ask.
	void undo();

	movie_timing timing() const { return calc_movie_timing(_clips, _kind); }
	// What the movie shows and plays at `time`, captions included.
	movie_frame frame(const double time) const { return calc_movie_frame(_clips, timing(), time); }
	movie_output output() const { return derive_movie_output(_clips, _kind); }

private:
	struct undo_entry
	{
		std::vector<movie_clip> clips;
		std::vector<size_t> selected;
		size_t current = 0;
		size_t anchor = 0;
		movie_kind kind = movie_kind::movie;
	};

	void push_undo();
	void begin_edit(movie_edit_group group);
	void settle_edit();
	void select_only(size_t index);
	void mark_clean();

	std::vector<movie_clip> _clips;
	std::vector<undo_entry> _undo;
	core::file_path _path;
	movie_kind _kind = movie_kind::movie;
	std::vector<size_t> _selected;
	size_t _current = 0;
	size_t _anchor = 0;
	bool _modified = false;
	uint64_t _revision = 0;
	uint64_t _edit_revision = 0;
	// The undo depth at which the timeline is the one last saved, seeded or opened. Empty once that
	// state can no longer be reached: its snapshot fell off the bounded stack, or an edit made after
	// undoing past it replaced the history that led back to it.
	std::optional<size_t> _clean_depth = 0;
	// The gesture whose undo step is still open, so its next value lands in that step, and the item it
	// is on.
	movie_edit_group _open_group = movie_edit_group::none;
	size_t _open_index = 0;
};

// What entering Movie does with the timeline it already holds. A photo and video selection
// different from the one the timeline was built from starts a new timeline from it; the same
// selection, or none, returns to the timeline left behind. docs/movie.md owns the rule.
enum class movie_entry
{
	resume,
	seed,
};

movie_entry decide_movie_entry(const movie_project& project, const std::vector<core::file_path>& selection,
                               const std::vector<core::file_path>& built_from);
