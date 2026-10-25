// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Common interfaces and abstract types. Defines shared interfaces
// used across modules for decoupling and extensibility.

#pragma once

#include <variant>

// The groups the app's commands fall in, which a ui::command carries for it; none comes first, so a
// value-initialized group is none.
enum class command_group
{
	none,
	navigation,
	selection,
	open,
	tools,
	rate_flag,
	options,
	media_playback,
	help,
	file_management,
	group_by,
	sort_by,
	edit_item
};

// The application's tree of view elements (ui_elements.h), named here for the headers that pass one
// around. The UI layer knows one only as a bubble's content.
class view_elements;
using view_elements_ptr = std::shared_ptr<view_elements>;

enum class view_type
{
	none,
	items,
	media,
	edit,
	movie,
	rename,
	batch,
	import,
	sync,
	locate,
	tags
};

enum class repeat_mode
{
	repeat_none = 0,
	repeat_all,
	repeat_one
};

constexpr bool is_valid_repeat_mode(const uint32_t value) noexcept
{
	return value <= static_cast<uint32_t>(repeat_mode::repeat_one);
}

// design.md "Play and Slideshow": what a tick does when the displayed item reaches its end.
enum class playback_advance
{
	none, // nothing has ended yet
	stop, // the sequence cannot continue
	hold, // stay on this item - repeat one, or a wrap that lands back on it
	advance, // move to the next item
};

// Everything the decision depends on. The caller resolves the candidates because finding them walks
// the visible items; deciding what to do with them is separable, and is what the rules are about.
struct playback_tick
{
	bool is_slideshow = false;
	bool is_photo = false;
	bool is_av = false;
	bool media_ended = false;
	bool photo_delay_elapsed = false;

	// False when a command is running, nothing is displayed, or the view cannot play.
	bool can_next = false;

	bool has_next = false; // a next media item without wrapping
	bool next_is_current = false;
	bool has_wrapped_next = false; // a next media item once wrapping is allowed
	bool wrapped_is_current = false;

	repeat_mode repeat = repeat_mode::repeat_none;
	bool auto_advance = false;
};

inline playback_advance calc_playback_advance(const playback_tick& t)
{
	// A slideshow that has landed on something that can neither play nor time out would stall
	// forever, so it ends instead.
	if (t.is_slideshow && !t.is_photo && !t.is_av) return playback_advance::stop;

	if (!(t.is_photo ? t.photo_delay_elapsed : t.media_ended)) return playback_advance::none;

	if (!t.can_next) return playback_advance::stop;

	// Repeat one holds on the current item; photos simply restart their delay.
	if (t.repeat == repeat_mode::repeat_one) return playback_advance::hold;

	// Continuing while browsing normally is a preference; a slideshow always continues, because
	// continuing is what the mode means.
	if (!t.is_slideshow && !t.auto_advance) return playback_advance::stop;

	if (t.has_next) return t.next_is_current ? playback_advance::hold : playback_advance::advance;

	// Only repeat all returns to the first item after the last.
	if (t.repeat == repeat_mode::repeat_all && t.has_wrapped_next)
	{
		return t.wrapped_is_current ? playback_advance::hold : playback_advance::advance;
	}

	return playback_advance::stop;
}

// Closed product ontology (docs/design.md): every destination-writing operation resolves
// destination collisions with exactly one explicitly named policy. Block Run is the
// conservative default - the run is refused and the reason is stated.
enum class collision_policy
{
	block_run = 0,
	skip,
	replace,
	auto_rename
};

enum class item_status
{
	success = 0,
	fail,
	ignore,
	cancel
};


enum class async_queue
{
	crc,
	scan_folder,
	scan_modified_items,
	scan_displayed_items,
	work,
	auto_complete,
	cloud,
	load,
	load_raw,
	render,
	render_display,
	query,
	sidebar,
	index,
	index_predictions_single,
	index_summary_single,
	index_presence_single,
	web,
	map_tile,
};


constexpr auto thumbnail_quality = 85;
constexpr auto thumbnail_webp_quality = 80;


struct metadata_text_detail
{
	std::string text;
};

struct metadata_binary_detail
{
	std::vector<uint8_t> bytes;
	// A complete encoded image rather than an opaque payload, so the pane may show it instead of dumping it.
	bool is_image = false;
};

// Decoded off the UI thread by whoever built the block, because painting never decodes.
struct metadata_image_detail
{
	ui::const_surface_ptr surface;
};

struct metadata_numeric_detail
{
	std::vector<uint16_t> values;
	int columns = 0;
};

using metadata_detail = std::variant<std::monostate, metadata_text_detail, metadata_binary_detail,
                                     metadata_numeric_detail, metadata_image_detail>;

// A metadata row. A list is held in the source block's own document order; `depth` and `container`
// describe the hierarchy that block actually has, and `detail` holds one typed expanded presentation.
// Metadata belongs to the displayed scan result, so none of its text enters the process-lifetime
// intern pool.
struct metadata_kv
{
	std::string key;
	std::string value;
	std::string shape;
	metadata_detail detail;
	std::string id;
	int depth = 0;
	bool container = false;
	bool open_by_default = false; // a section worth reading before the user asks, whatever its size
	// Detail is prose rather than a dump, so it is drawn in the reading face and replaces the
	// value preview while open instead of repeating its first line.
	bool prose = false;

	metadata_kv() = default;

	metadata_kv(std::string k, std::string v) noexcept : key(std::move(k)), value(std::move(v))
	{
	}

	metadata_kv(const std::string_view k, const std::string_view v) : key(k), value(v)
	{
	}

	metadata_kv(const std::string_view k, const char* v) : key(k), value(v)
	{
	}

	metadata_kv(const str::cached k, std::string v) : key(k.sv()), value(std::move(v))
	{
	}

	metadata_kv(const str::cached k, const std::string_view v) : key(k.sv()), value(v)
	{
	}

	metadata_kv(const str::cached k, const char* v) : key(k.sv()), value(v)
	{
	}
};

using metadata_kv_list = std::vector<metadata_kv>;

namespace df
{
	class search_t;
	class item_element;
	struct index_file_item;

	using item_element_ptr = std::shared_ptr<item_element>;

	struct status_i
	{
		virtual ~status_i() = default;
		virtual void start_item(std::string_view name) = 0;
		virtual void end_item(std::string_view name, item_status status) = 0;

		virtual bool has_failures() const = 0;
		virtual void abort(std::string_view error_message) = 0;
		virtual void complete(std::string_view message = {}) = 0;
		virtual void show_errors() = 0;

		virtual void message(std::string_view message, int64_t pos, int64_t total) = 0;

		virtual void show_message(std::string_view message) = 0;
		virtual bool is_canceled() const = 0;
		virtual void wait_for_complete() const = 0;
	};

	using results_ptr = std::shared_ptr<status_i>;

	struct async_i
	{
		virtual void queue_ui(std::function<void()> f) = 0;
		virtual void queue_async(async_queue q, std::function<void()> f) = 0;
	};
};
