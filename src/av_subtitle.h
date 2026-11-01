// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Subtitles. What a subtitle track holds - text with the subset of ASS styling a player draws, or
// pictures on the video's canvas - which track a video shows, from its own streams and the sidecars beside
// it, and the events a playing session keeps. Events are rasterised off the UI thread into bitmaps laid out
// for the picture as it is drawn - turned, for a video shown turned - which the video control draws scaled
// to the picture. FFmpeg decodes them, in av_format.cpp; nothing here touches a file or the UI thread's
// drawing.

#pragma once

// A style from an ASS script's [V4+ Styles], with the fields a player is asked to honour. Sizes and margins
// are in the script's own pixels; colours are 0xAABBGGRR with straight alpha.
struct subtitle_style
{
	std::string name = "Default";
	std::string font = "Arial";
	double size = 16;
	uint32_t primary = 0xFFFFFFFF;
	uint32_t outline_color = 0xFF000000;
	double outline = 1;
	bool bold = false;
	bool italic = false;
	// As a numeric keypad lays them out: 1 to 3 along the bottom, 4 to 6 the middle, 7 to 9 the top.
	int alignment = 2;
	int margin_l = 10;
	int margin_r = 10;
	int margin_v = 10;
};

// An ASS script's header: the canvas its positions and sizes are in, and its styles. FFmpeg gives every text
// subtitle decoder one, so SRT and WebVTT read through the same styles as ASS does.
struct subtitle_script
{
	int play_res_x = 384;
	int play_res_y = 288;
	std::vector<subtitle_style> styles;

	// The named style, or the first, or a default one: a dialogue line naming a missing style still shows.
	const subtitle_style& style(std::string_view name) const;
};

subtitle_script parse_ass_header(std::string_view header);

// The fields FFmpeg gives an ASS rectangle: "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text".
struct ass_event_fields
{
	std::string_view style;
	std::string_view text;
	int margin_l = 0;
	int margin_r = 0;
	int margin_v = 0;
};

ass_event_fields split_ass_event(std::string_view ass);

// One line of dialogue, as runs styled by the overrides it carries and the style it names: bold, italic,
// colour, alignment and position. Other overrides are dropped, and so is drawing-mode text.
struct subtitle_text
{
	std::vector<platform::text_run> runs;
	std::string font = "Arial";
	double size = 16;
	double outline = 1;
	uint32_t outline_color = 0xFF000000;
	int alignment = 2;
	int margin_l = 10;
	int margin_r = 10;
	int margin_v = 10;
	// \pos(x,y), in the script's pixels.
	std::optional<pointd> position;

	std::string plain_text() const;
};

subtitle_text parse_ass_dialogue(std::string_view text, const subtitle_style& style);

// A picture subtitle as decoded - Blu-ray PGS, DVD VobSub, DVB - kept as palette indices, a quarter of the
// size the bitmap it becomes takes, until it is near enough to be shown.
struct subtitle_image
{
	recti rect;
	std::vector<uint8_t> indices;
	// 0xAARRGGBB, as FFmpeg gives a palette.
	std::vector<uint32_t> palette;
};

ui::surface_ptr subtitle_image_surface(const subtitle_image& image);

// A rasterised subtitle, placed on the canvas: the video's pixels.
struct subtitle_bitmap
{
	ui::const_surface_ptr surface;
	rectd bounds;
};

// One subtitle on the presentation timeline.
struct subtitle_event
{
	double start = 0;
	double end = 0;
	// The seek generation an embedded event was decoded in; a seek drops the older ones, as it drops
	// frames. Zero for a sidecar's events, which are read whole and kept for the session.
	int gen = 0;
	uint64_t id = 0;
	// A forced subtitle translates foreign speech in a film otherwise in the viewer's language, and shows
	// even when subtitles are off.
	bool forced = false;
	// The coordinate space its positions are in: the script's canvas for an image subtitle, the video's
	// pixels for everything once placed.
	sizei canvas;
	std::optional<subtitle_text> text;
	std::vector<subtitle_image> images;
	// Filled while the event is near enough to show, and released once it is behind or far ahead.
	std::vector<subtitle_bitmap> bitmaps;
};

// Rasterises a text event into one bitmap per block, placed as its alignment, margins and position say on a
// video of `video` pixels. Called by the thread that decodes, never the UI thread.
std::vector<subtitle_bitmap> render_subtitle_text(const subtitle_text& text, const subtitle_script& script,
                                                  sizei video);

// Converts an image event's pictures from its own canvas to the video's pixels.
std::vector<subtitle_bitmap> render_subtitle_images(const std::vector<subtitle_image>& images, sizei canvas,
                                                    sizei video);

// Where a block of width x height lands on the video for an alignment, margins and an optional position, in
// the script's pixels scaled to the video's.
rectd place_subtitle(sizei video, sizei script_canvas, sized block, int alignment, int margin_l, int margin_r,
                     int margin_v, std::optional<pointd> position);

// One bitmap on show and where it lands in the view.
struct subtitle_placement
{
	const subtitle_bitmap* bitmap = nullptr;
	recti bounds;
};

// Where the bitmaps on show land in the view: scaled from the video's pixels onto the picture, a line that would
// cover an earlier one moved off it, away from the edge it is placed against, and the lines in the lower half
// raised to clear `clear_above` - the top of controls floating over the picture, or zero for none - by `gap`,
// as far as `raise` (0 to 1, as the controls fade in) says.
std::vector<subtitle_placement> place_subtitles_on_picture(const std::vector<subtitle_event>& shown, sizei video,
                                                           recti picture, int clear_above, double raise, int gap);

// Every subtitle event a session holds. Written by the read thread as packets decode, or all at once from a
// sidecar when the session opens; read by the UI thread, which takes only finished bitmaps.
class subtitle_events
{
	mutable platform::mutex _mutex;
	_Guarded_by_(_mutex) std::vector<subtitle_event> _events;
	_Guarded_by_(_mutex) subtitle_script _script;
	_Guarded_by_(_mutex) sizei _video;
	_Guarded_by_(_mutex) uint64_t _next_id = 1;

public:
	void reset(subtitle_script script, sizei video);
	// A picture ends the one on show before it, whatever end that one states: an image stream shows one picture
	// at a time, and PGS states no end at all.
	void add(subtitle_event e);
	void add_all(std::vector<subtitle_event> events);
	// The size lines are laid out for: the video as its picture is drawn, turned when it is shown turned. A new
	// size drops the bitmaps laid out for the old one, and prepare() lays them out again.
	void layout(sizei video);
	sizei layout() const;
	// A seek drops the embedded events of earlier generations; a sidecar's stay.
	void drop_older_than(int gen);
	// An image stream's clear: the pictures on show at `t` end there.
	void end_open(double t);
	// Rasterises what is near `pos` and releases what is not, so memory follows the playhead rather than
	// the length of the film. Rasterising runs outside the lock, on the calling thread.
	void prepare(double pos);
	// The bitmaps on show at `pos`: events of the current generation or a sidecar's, in start order.
	std::vector<subtitle_event> at(double pos, int gen, bool forced_only) const;
	// Names what at() would show, without copying it: the UI compares it with what it drew to know when a
	// line has come, gone, or finished rasterising while the picture stood still. Zero when nothing shows.
	uint64_t shown_key(double pos, int gen, bool forced_only) const;
	size_t size() const;
};

// The key of a set of events on show, as shown_key gives it.
uint64_t subtitle_shown_key(const std::vector<subtitle_event>& shown);

// A subtitle file's events as they show: one picture at a time, each picture, and each clear - an event with
// neither text nor pictures - ending the one before it. Sorts them by start and drops the clears.
void end_replaced_pictures(std::vector<subtitle_event>& events);

// A subtitle the player can show: one of the file's own streams, or a sidecar file beside it.
struct subtitle_choice
{
	int stream = -1;
	df::file_path sidecar;

	bool is_none() const { return stream < 0 && sidecar.is_empty(); }
	friend bool operator==(const subtitle_choice&, const subtitle_choice&) = default;
};

// A subtitle a video offers, for the track menu and the automatic choice.
struct subtitle_offer
{
	subtitle_choice choice;
	std::string language;
	std::string title;
	bool forced = false;
};

// The language a sidecar names after its video's name, and whether it is forced: Movie.en.srt,
// Movie.English.forced.srt, Movie.pt-BR.sdh.srt. Empty when it names none. Only the words after the video's
// name count, so It.2017.srt beside It.2017.mkv names no language.
struct sidecar_subtitle_name
{
	std::string language;
	bool forced = false;
};

sidecar_subtitle_name read_sidecar_subtitle_name(std::string_view sidecar_name, std::string_view video_name);

// Whether two language names - ISO 639-1 or -2 codes, or English names - name the same language.
bool same_language(std::string_view a, std::string_view b);

// Whether a word names a language: an ISO 639-1 or -2 code, with or without its region (pt-BR), or an English name.
bool is_language_word(std::string_view word);

// The track a video opens with: the preferred language's, when the mode asks for one; otherwise a forced
// track in the viewer's language, if there is one, because a forced track only translates what the viewer
// would not understand.
subtitle_choice choose_subtitles(const std::vector<subtitle_offer>& offers, subtitle_mode mode,
                                 std::string_view preferred_language, std::string_view audio_language);

// What the viewer asks of a video as it opens: a track picked from the menu, or the preferences that pick
// one; and the sidecars beside it, which only the index knows of.
struct subtitle_request
{
	std::optional<subtitle_choice> chosen;
	std::vector<subtitle_offer> sidecars;
	subtitle_mode mode = subtitle_mode::foreign;
	std::string language;
};

// What a session shows and could show, for the track menu.
struct subtitle_state
{
	std::vector<subtitle_offer> offers;
	subtitle_choice choice;
};

// The sidecar subtitles beside a video, from the sidecar names its index entry records.
std::vector<subtitle_offer> sidecar_subtitle_offers(df::file_path video, std::string_view sidecars);
