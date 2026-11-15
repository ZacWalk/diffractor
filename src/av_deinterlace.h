// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Interlaced video made progressive on the CPU. A frame holds two fields - alternate lines, taken
// a field period apart - and each becomes a whole picture: its own lines kept, and the lines between made
// for the instant it was taken, from the other field where nothing moved and from its own lines where
// something did.

#pragma once

struct AVFrame;

// A layout the deinterlacer reads: planar or semi-planar, of one or two bytes a sample in the machine's byte
// order. A hardware frame's is not; it is downloaded first.
bool av_can_deinterlace(int pixel_format);

// A frame its decoder marks as two fields.
bool av_is_interlaced(const AVFrame& frame);

// Fills out - allocated here, with current's properties and no longer marked interlaced - with one field of
// current as a whole picture: the field shown first, or with second_field the one shown after it. previous is
// the frame before current in the stream, or null where there is none - the first after a seek, or a frame
// decoded alone - which leaves the lines between to be made from the field alone. False for a layout
// av_can_deinterlace refuses, or when out cannot be allocated.
bool av_deinterlace_field(const AVFrame& current, const AVFrame* previous, bool second_field, AVFrame& out);
