// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Movie document. An ordered list of trimmed clips, the settings applied across them,
// the derived output geometry, the project file read and written for both, which of several
// same-named files a moved source most likely became, and what entering Movie does with the
// timeline it holds. docs/movie.md owns the behaviour; nothing here decodes, encodes, draws or
// touches the UI.

#pragma once

enum class movie_transition
{
	cut,
	crossfade,
};

// One entry in the timeline. A photo has no source duration of its own, so `end` is the length it
// is held for and `start` is always zero; a video is trimmed to [start, end) of its source.
struct movie_clip
{
	df::file_path path;
	bool is_photo = false;
	// True until the source has been probed and found. The project readers do no I/O, so a freshly
	// read project starts with every clip missing and the view resolves them; a clip that stays
	// missing keeps its stored times, so the trim survives a relink.
	bool is_missing = true;
	// Whether the probe has answered for this clip at all. Not saved: it is the difference between
	// "we have not looked yet" and "we looked and it is gone", and only the second is a thing to
	// tell the user about or to offer to repair.
	bool is_probed = false;
	double source_duration = 0;
	double start = 0;
	double end = 0;
	bool photo_duration_is_default = true;
	sizei extent;
	double frame_rate = 0;

	double duration() const
	{
		return std::max(0.0, end - start);
	}

	// Probed, and not there. Distinct from not yet probed, which looks identical in the document and
	// means the opposite to the user.
	bool is_lost() const
	{
		return is_probed && is_missing;
	}

	bool is_trimmed() const
	{
		return !is_photo && (start > 0.0001 || (source_duration > 0 && end < source_duration - 0.0001));
	}

	// Compared so an edit that changes nothing can be told from one that does: a no-op must not
	// cost an undo step or mark the timeline as holding unsaved work.
	bool operator==(const movie_clip&) const = default;
};

struct movie_settings
{
	movie_transition transition = movie_transition::crossfade;
	double transition_seconds = 1.0;
	bool fade_in = true;
	bool fade_out = true;
	double photo_seconds = 4.0;

	bool operator==(const movie_settings&) const = default;
};

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

// Ceilings the derivation clamps to. A photo-only timeline must not produce an 8000-pixel movie
// from a 45-megapixel source, and no codec here accepts an odd dimension.
inline constexpr int movie_max_width = 3840;
inline constexpr int movie_max_height = 2160;
inline constexpr int movie_max_frame_rate = 60;
inline constexpr int movie_default_frame_rate = 30;

// Bits per pixel per frame. 1920x1080x30 lands at ~6.2 Mbit/s, which is what a consumer H.264
// encoder wants for camera footage.
inline constexpr double movie_bits_per_pixel = 0.10;
inline constexpr int movie_min_bitrate = 1000000;
inline constexpr int movie_max_bitrate = 60000000;

// The document's own bounds on the two durations the settings carry. The panel already clamps to
// these; a project file is untrusted input and is held to the same range, because a value from
// outside it reaches the frame-count arithmetic as a duration no encoder can express.
inline constexpr double movie_max_transition_seconds = 10.0;
inline constexpr double movie_min_photo_seconds = 0.1;
inline constexpr double movie_max_photo_seconds = 60.0;

// A day of video at the highest rate the output can take. Past this the frame count stops fitting
// the arithmetic that walks it, and a movie that long is a broken document rather than a request.
inline constexpr int64_t movie_max_frames = 24ll * 60ll * 60ll * movie_max_frame_rate;

// The most any one clip may cover: the longest movie a render will make. A project file's clip range
// past it is a broken document, and it would reach the frame arithmetic and the displayed clock as a
// number no integer can hold.
inline constexpr double movie_max_clip_seconds = static_cast<double>(movie_max_frames) / movie_max_frame_rate;

movie_output derive_movie_output(const std::vector<movie_clip>& clips);

// Seconds two adjacent clips overlap. Half of the shorter clip is the ceiling, so a transition
// longer than the clip it joins shortens rather than consuming it, and no clip can be eaten
// entirely by the transitions on both of its sides.
double movie_overlap(const movie_clip& left, const movie_clip& right, const movie_settings& settings);

// Start time of each clip on the finished movie's timeline, plus the total. Both answers come from
// one walk because the caller that draws the timeline needs both and they must not disagree.
struct movie_timing
{
	std::vector<double> starts;
	double duration = 0;
};

movie_timing calc_movie_timing(const std::vector<movie_clip>& clips, const movie_settings& settings);

// One clip's contribution to one frame.
struct movie_frame_source
{
	int index = -1;
	// Seconds into the source file, already offset by the clip's in point. A photo answers 0.
	double source_time = 0;
	double weight = 0;
};

// What the compositor draws at one instant. At most two clips contribute: a crossfade joins exactly
// two, and the half-clip ceiling in movie_overlap stops a third reaching across a short clip.
//
// This is the decision; drawing it is separate, so the preview and the render cannot disagree about
// what a frame contains and neither needs a window to be tested.
struct movie_frame
{
	movie_frame_source a;
	movie_frame_source b;
	// How far the movie's own fade to black has gone: 0 draws the clips untouched, 1 is black.
	double fade_to_black = 0;

	bool is_empty() const { return a.index < 0; }
};

movie_frame calc_movie_frame(const std::vector<movie_clip>& clips, const movie_settings& settings, double time);
movie_frame calc_movie_frame(const std::vector<movie_clip>& clips, const movie_settings& settings,
	const movie_timing& timing, double time);

// Builds a clip from a path. The duration and extent come from the caller because probing is I/O;
// a photo is given the settings' default hold.
movie_clip make_movie_clip(df::file_path path, const movie_settings& settings);

// True when a proposed output path names one of the clips the render reads. The render replaces the
// output only after its last read, so an output that names a source destroys that source silently
// and leaves the timeline pointing at a file that is now the movie. Compared by path, which is the
// identity every other destination-writing operation uses.
bool movie_output_names_a_clip(const std::vector<movie_clip>& clips, df::file_path output);

enum class movie_load_status
{
	ok,
	unreadable,
	empty,
};

struct movie_load_result
{
	movie_load_status status = movie_load_status::unreadable;
	std::vector<movie_clip> clips;
	// For each clip, the length of its source as the project recorded it, and zero where it recorded
	// none. Empty for a format that records no lengths. A source found again whose measured length
	// differs is probably another file with the same name.
	std::vector<double> recorded_seconds;
	movie_settings settings;
	// Named for the report, not for a dialog: the count of elements the file carried that Movie
	// has no answer for. docs/movie.md#6-project-files owns what becomes of them.
	int ignored_elements = 0;
	// The same count by what the file calls each thing -- an element, an effect template, a clip
	// setting -- in the order first met, for a report that has to say more than a number.
	std::vector<std::pair<std::string, int>> omitted;

	// The only way a reader counts something it left out, so the total and the breakdown agree.
	void omit(std::string_view name, int count = 1);

	explicit operator bool() const { return status == movie_load_status::ok; }
};

// The native project format is OpenTimelineIO. Paths are written relative to the project folder
// when they share a root, so a project and its sources move together.
std::string write_otio(const std::vector<movie_clip>& clips, const movie_settings& settings,
                       df::folder_path project_folder);
movie_load_result read_otio(std::string_view json, df::folder_path project_folder);

// Windows Live Movie Maker. The format is undocumented, so the reader scrapes rather than
// validates: see the note on the implementation. A relative media path is relative to the folder
// the project is in.
movie_load_result read_wlmp(std::string_view xml, df::folder_path project_folder);

// How a source a project names was resolved: where the project stored it, found again by name
// under a search folder or the collection, or not at all. docs/movie.md#6-project-files owns the
// rules; choose_movie_sources decides among the namesakes and never answers `stored`.
enum class movie_source_match
{
	stored,
	missing,
	other_length,
	only_name,
	same_folders,
	same_length,
	siblings,
	ambiguous,
};

// How far a measured length may stray from the one a project recorded and still be the same file.
// Movie Maker measured with other decoders, so the two disagree by a frame or two, not by seconds.
inline constexpr double movie_source_length_tolerance = 0.5;

struct movie_source_choice
{
	movie_source_match match = movie_source_match::missing;
	// Empty unless one candidate was chosen.
	df::file_path path;
};

// `candidates[i]` lists the files under the search folder named as `stored[i]` is, and
// `recorded_seconds[i]` is the length the project recorded for it, or zero. `measure_seconds`
// answers a candidate's length, or zero when it has none to measure; it is the one place I/O can
// enter, so the rules are tested with a stand-in and no disk. It is asked only about the candidates
// of a source with a recorded length.
std::vector<movie_source_choice> choose_movie_sources(const std::vector<df::file_path>& stored,
                                                      const std::vector<std::vector<df::file_path>>& candidates,
                                                      const std::vector<double>& recorded_seconds,
                                                      const std::function<double(df::file_path)>& measure_seconds);

// Which control an edit came from, where that control reports one gesture as a stream of values: a
// slider dragged across fifty values, or a length typed a digit at a time. Edits naming the same group
// merge into one undo step until the gesture settles or anything else happens, so one drag is one
// edit, one Undo takes it back, and a drag that ends where it began is no edit at all.
enum class movie_edit_group
{
	none,
	transition_length,
	photo_duration,
	clip_hold,
};

// The timeline as a document: ordered, editable, and undoable. UI-thread-owned; the render takes a
// detached copy of `clips()` and `settings()` and never reads this object again.
class movie_project
{
public:
	// Deep enough that an accidental multi-select delete is recoverable, bounded because each entry
	// is a copy of the whole clip list.
	static constexpr size_t max_undo = 64;

	const std::vector<movie_clip>& clips() const { return _clips; }
	const movie_settings& settings() const { return _settings; }
	df::file_path path() const { return _path; }
	bool is_modified() const { return _modified; }
	// Advances on every change to the document, including the probe filling in what it measured.
	uint64_t revision() const { return _revision; }
	// Advances only on what the user did -- an edit, an undo, a replacement. Save and Open ask whether
	// the user changed the timeline while they ran, and a probe landing meanwhile is not an answer.
	uint64_t edit_revision() const { return _edit_revision; }
	size_t size() const { return _clips.size(); }
	bool is_empty() const { return _clips.empty(); }
	bool is_ready_to_render() const;

	// Index of the clip the controls panel describes and the strip marks. Always in range while the
	// timeline is not empty, so the panel never has to test it. Focus is one clip; the selection is
	// a set, and the two are different facts.
	size_t current() const { return _current; }
	void current(size_t i);
	const movie_clip* current_clip() const;

	// Sorted, and empty only when the timeline is. A command that acts on "the clips" acts on this.
	const std::vector<size_t>& selected() const { return _selected; }
	bool is_selected(size_t i) const;
	// One click's worth of selection: plain replaces, `extend` runs from the anchor, `toggle` adds
	// or removes one. Focus always lands on `index` whichever it was.
	void select(size_t index, bool extend, bool toggle);
	// Moves focus without changing the selected set. Used while a press inside an existing
	// multi-selection may become a block drag.
	void focus(size_t index);
	void select_all();

	void append(const movie_clip& clip);
	void insert(size_t at, const movie_clip& clip);
	// Inserts several clips as one document edit, so one Add or one drop is one undo step. Focus
	// and the selection land on the first clip inserted.
	void insert_many(size_t at, const std::vector<movie_clip>& clips);
	void remove(size_t at);
	void move(size_t from, size_t to);
	// Moves every selected clip to the drop point, keeping their order relative to each other and
	// carrying focus with them. `to` counts positions in the list as it stands before the move.
	void move_selection(size_t to);
	void remove_selection();
	// A replacement equal to the clip it replaces is not an edit.
	void replace(size_t at, const movie_clip& clip);
	// Replaces several clips as one document edit and therefore one undo step, and none at all when
	// every replacement equals what is already there.
	void replace_many(const std::vector<std::pair<size_t, movie_clip>>& replacements);
	// For a change the user did not make and cannot want to undo: the probe filling in a clip's
	// duration and extent. An undo entry here would make Ctrl+Z appear to do nothing.
	void replace_quietly(size_t at, const movie_clip& clip);

	// Changing the default photo length retrims every photo still following it; a photo the user set
	// explicitly keeps its own length, which is what makes the movie setting a default rather than an
	// override. Settings equal to the current ones are not an edit.
	void settings(const movie_settings& s, movie_edit_group group = movie_edit_group::none);

	// Equal ranges are not an edit, so a handle pressed and released in place costs nothing.
	void trim(size_t at, double start, double end, movie_edit_group group = movie_edit_group::none);

	// The control that reported a gesture has let go, so the next edit, from it or anything else, is
	// a step of its own.
	void end_edit_group();

	void reset(std::vector<movie_clip> clips, const movie_settings& s, df::file_path path);
	void mark_saved(df::file_path path, uint64_t edit_revision);

	// A timeline just built from a selection has no state before itself, so there is nothing to
	// undo back to and nothing yet for the user to have changed.
	void mark_seeded();

	bool can_undo() const { return !_undo.empty(); }
	// Undoing back to the state last saved, seeded or opened leaves the timeline unmodified: there is
	// nothing left to lose, so leaving Movie has nothing to ask.
	void undo();

	movie_timing timing() const { return calc_movie_timing(_clips, _settings); }
	movie_output output() const { return derive_movie_output(_clips); }

private:
	struct undo_entry
	{
		std::vector<movie_clip> clips;
		movie_settings settings;
		std::vector<size_t> selected;
		size_t current = 0;
		size_t anchor = 0;
	};

	void push_undo();
	void begin_edit(movie_edit_group group);
	void settle_edit();
	void select_only(size_t index);
	void mark_clean();
	void apply_photo_duration();

	std::vector<movie_clip> _clips;
	movie_settings _settings;
	std::vector<undo_entry> _undo;
	df::file_path _path;
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
	// The gesture whose undo step is still open, so its next value lands in that step.
	movie_edit_group _open_group = movie_edit_group::none;
};

// What entering Movie does with the timeline it already holds. A photo and video selection
// different from the one the timeline was built from starts a new timeline from it; the same
// selection, or none, returns to the timeline left behind. docs/movie.md owns the rule.
enum class movie_entry
{
	resume,
	seed,
};

movie_entry decide_movie_entry(const movie_project& project, const std::vector<df::file_path>& selection,
                               const std::vector<df::file_path>& built_from);
