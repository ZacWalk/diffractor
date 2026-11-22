// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: How Movie shows and hears an audio clip, which has no picture of its own. Each frame is the file as
// Diffractor plays it - a SID tune's or a tracker module's monitor, or the visualizer's bars beside its cover art -
// under its title, on a landscape frame or a Short's upright one. Frames are drawn by the software renderer on
// whichever one worker asks, into a display and monitor of their own that no view shares, so the preview and the
// render make the same pictures. Also how long such a clip lasts, which tunes a SID file holds, and how its sound is
// opened.

#pragma once

#include "av_format.h"
#include "model_movie.h"

// Whether Movie shows this file as an audio clip, drawn rather than decoded.
bool is_movie_audio(core::file_path path);

// The tunes an audio file holds, and the one it starts on, from 1: a SID file's, and a single tune for anything
// else. None for a SID file lib64 cannot play. Also the rate the clip's pictures move at, which a YouTube Short is
// made at: the frame rate of the machine a SID tune was written for, a tracker module's 50, and otherwise
// movie_audio_frame_rate. Reads the file, so it belongs on a worker.
struct movie_audio_tunes
{
	int count = 1;
	int start = 1;
	double picture_rate = movie_audio_frame_rate;
};

movie_audio_tunes movie_audio_songs(core::file_path path);

// How long Movie plays an audio file: a SID tune to the end its scan finds, fading as it starts again where it
// repeats, or for the span the player gives a tune it cannot tell; anything else for as long as its decoder says.
// Zero for a file nothing here can play. song is the tune of a SID file, from 1, or 0 for the one it starts on.
// Runs the tune or opens the file, so it belongs on a worker.
double movie_audio_seconds(core::file_path path, int song = 0);

// Where a SID tune starts again, in seconds from its start, when its scan found it to repeat; zero for one that
// falls silent or that the scan could not tell, and for any other file. Belongs on a worker, as the scan does.
double movie_audio_loop_seconds(core::file_path path, int song = 0);

// Opens an audio clip for the sound Movie mixes from it. A SID tune plays the tune asked for and ends where
// movie_audio_seconds says, one render shared by every reader; anything else opens as the decoder opens it.
bool open_movie_audio(av_format_decoder& decoder, core::file_path path, media_intent intent, int song = 0);

// The pictures of one audio clip, for one thread at a time. The tune, or the bars, carry on from one frame to the
// next, so frames asked for in order cost only the time between them; a jump starts again just before it lands.
class movie_audio_frames final : core::no_copy
{
public:
	explicit movie_audio_frames(core::file_path path, int song = 0);
	~movie_audio_frames();

	core::file_path path() const;
	int song() const;

	// The picture at `seconds` into the source, in the shape of `shape` - movie_audio_extent for a movie, and an
	// upright Short's own frame, which it fills, a SID monitor as large as that holds - as large as fits max_dim.
	// Null for a file that cannot be played, or once abandoned.
	core::const_surface_ptr frame(double seconds, sizei max_dim, const core::cancel_token& abandon = {},
	                              sizei shape = movie_audio_extent);

private:
	std::unique_ptr<class movie_audio_frames_impl> _impl;
};
