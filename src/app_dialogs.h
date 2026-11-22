// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The dialogs commands open that are features in their own right - the update offer, the
// keyboard reference, About, index maintenance and sharing. app_commands.cpp registers
// the commands that open them; the settings themselves are the Options view's (view_options.h).

#pragma once

#include "app_commands.h"

class view_state;

#ifndef WINSTORE
void show_update_dialog(view_state& s, const ui::frame_ptr& parent);
#endif
void show_keyboard_reference(view_state& s, const ui::frame_ptr& parent, const commands_map& commands);
void about_invoke(view_state& s, const ui::frame_ptr& parent, commands_map& commands);
// Checks the index, then offers to defragment or reset it, each behind its own dialog.
void index_maintenance_invoke(const view_state& s, const ui::frame_ptr& parent);
// How large the index is and how many items it describes.
std::string format_index_text(const view_state& s);
void share_invoke(view_state& s, const ui::frame_ptr& parent, const view_host_base_ptr& view);

// A command that acts on the selection first asks whether it can. When it cannot it says why, under
// the command's own title, rather than doing nothing. Defined in app_commands.cpp.
bool can_process_selection_or_explain(const view_state& s, const view_host_base_ptr& view, const ui::frame_ptr& parent,
                                      std::string_view title, df::process_items_type type);
