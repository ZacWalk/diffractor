// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The dialogs commands open that are features in their own right - advanced search, the
// update offer, the keyboard reference, About, settings, collection settings and maintenance, sidebar
// customisation and sharing. app_commands.cpp registers the commands that open them.

#pragma once

class view_state;

void advanced_search_invoke(view_state& state, const ui::control_frame_ptr& parent, const view_host_base_ptr& view);
#ifndef WINSTORE
void show_update_dialog(view_state& s, const ui::control_frame_ptr& parent);
#endif
void show_keyboard_reference(view_state& s, const ui::control_frame_ptr& parent, const commands_map& commands);
void about_invoke(view_state& s, const ui::control_frame_ptr& parent, commands_map& commands);
void settings_invoke(view_state& s, const ui::control_frame_ptr& parent);
void index_settings_invoke(view_state& s, const ui::control_frame_ptr& parent, settings_t::index_t collection_settings);
void customise_invoke(view_state& s, const ui::control_frame_ptr& parent);
void share_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view);

// A command that acts on the selection first asks whether it can. When it cannot it says why, under
// the command's own title, rather than doing nothing. Defined in app_commands.cpp.
bool can_process_selection_or_explain(const view_state& s, const view_host_base_ptr& view,
                                      const ui::control_frame_ptr& parent, std::string_view title,
                                      df::process_items_type type);

bool should_refresh_advanced_search_hover_thumbnail(bool has_surface, bool has_thumbnail, bool is_staging,
                                                    int& hover_retries);
