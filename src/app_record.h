// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Debug build's screen recorder, for tutorial videos made from the running application: its
// window's client area as the screen shows it - menus, bubbles and the pointer included - written to an MP4
// at a steady rate until it is stopped, with the pointer and clicks a script shows drawn in, and the moments
// it names written beside the movie. What the application asks of it; the capture is the platform's
// (platform_win_record.cpp) and the encoder is libdesktop's movie writer. Linux records nothing.

#pragma once

#ifdef _DEBUG

// The rate a recording is made at: one Movie mixer makes movies at, so a recording joins a movie with no frame
// repeated or dropped unevenly.
inline constexpr int screen_recording_frame_rate = 30;

struct screen_recording_request
{
	// The window whose client area is recorded, as its frame's handle() gives it.
	std::any window;
	core::file_path path;
	int frame_rate = screen_recording_frame_rate;
	// Stops by itself once this long has been written; zero records until stop().
	double seconds = 0;
	// Draws the system pointer into the frames, where it is over the window.
	bool draw_pointer = true;
	// Holds the window still for an audience while it records: above every other window, so nothing that pops up
	// covers it, and with square corners and no border, so its edges are its own.
	bool present_window = false;
};

class screen_recording
{
public:
	virtual ~screen_recording() = default;

	// True once the recording has ended by itself: its length written, or a failure.
	virtual bool is_finished() const = 0;
	// Ends the recording and finishes its file, waiting for both. Safe to call more than once.
	virtual void stop() = 0;
	// What was written, or why nothing was, in one line for the log and the console. Final once stopped.
	virtual std::string summary() const = 0;

	// Seconds into the movie now: where a frame captured now lands. Negative until the first frame is captured.
	virtual double elapsed() const = 0;
	// A pointer drawn at this point on the screen, gliding there from where it was over glide_seconds, so a
	// script can show where it acts without moving the user's own; none hides it. It is drawn whatever
	// draw_pointer says, which is about the system's.
	virtual void pointer(std::optional<pointi> screen, double glide_seconds) = 0;
	// A ring spreads from the drawn pointer, as a click shows.
	virtual void click() = 0;
	// Names this moment of the movie. The names are written beside it, one "<seconds><TAB><name>" a line, in
	// <movie>.markers.txt, once it is finished.
	virtual void mark(std::string_view name) = 0;
};

// Starts recording on a thread of its own. Null where the platform cannot record.
std::unique_ptr<screen_recording> start_screen_recording(const screen_recording_request& request);

// Where a recording's markers are written.
inline core::file_path screen_recording_markers_path(const core::file_path movie)
{
	return movie.folder().combine_file(std::format("{}.markers.txt", movie.file_name_without_extension()));
}

#endif
