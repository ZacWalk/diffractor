// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The ordinary transport, one flex row of the existing play and seek controls, used by every media
// presentation.

#include "pch.h"
#include "ui_music.h"
#include "ui_controls.h"
#include "model_faces_search.h"
#include "model_search.h"

view_elements_ptr make_media_transport(view_state& state, const display_state_ptr& display)
{
	auto transport = std::make_shared<view_elements>(flex_item::grow | flex_item::line_break | flex_item::stretch);
	transport->padding(0);
	transport->margin(0);
	transport->flex_container.wrap = flex_wrap::no_wrap;
	transport->flex_container.gap = {8, 0};
	transport->add(std::make_shared<play_control>(state, flex_item::no_break));

	auto scrubber = std::make_shared<scrubber_element>(state._player, display);

	// docs/faces.md#8-searching: where in a video the faces the search found it by are seen, worked out again only
	// as the video, the search or what face search knows changes, not for every frame drawn.
	struct found_cache
	{
		core::file_path path;
		std::string terms;
		const void* overview = nullptr;
		uint64_t generation = 0;
		scrubber_element::found_spans spans;
	};

	scrubber->found_by([&state, display, cache = std::make_shared<found_cache>()]
	{
		const auto& item = display->is_one() ? display->_item1 : nullptr;
		if (!item || !item->file_type()->has_trait(file_traits::av)) return scrubber_element::found_spans{};

		const auto overview = faces::active_overview();
		const auto generation = overview && overview->store ? overview->store->generation() : 0;
		auto terms = state.search().format_terms();
		if (item->path() != cache->path || terms != cache->terms || overview.get() != cache->overview ||
		    generation != cache->generation)
		{
			cache->path = item->path();
			cache->terms = std::move(terms);
			cache->overview = overview.get();
			cache->generation = generation;
			cache->spans.clear();
			for (const auto& s : df::face_spans_found(state.search(), item->path()))
				cache->spans.emplace_back(s.start / 1000.0, s.end / 1000.0);
		}
		return cache->spans;
	});

	transport->add(std::move(scrubber));
	return transport;
}
