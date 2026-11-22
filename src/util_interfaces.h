// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Common interfaces and abstract types. Defines shared interfaces
// used across modules for decoupling and extensibility, and the few small helpers they share, such as where a
// path is once its folder has moved.

#pragma once

#include <variant>

namespace df
{
	extern std::atomic_int file_handles_detached;
	extern std::atomic_int jobs_running;
	extern std::atomic_int loading_media;
	extern std::atomic_int dragging_items;
	extern std::atomic_int handling_crash;
	extern std::atomic<core::file_path> last_loaded_path;
	// Whether work on the user's behalf may reach the network: an update check, a crash report, a map's tiles, a
	// spelling dictionary. An agent session clears it unless it was started with -mcp-allow:network.
	extern std::atomic_bool network_allowed;

	// Counts in loading_media while any copy is held. Work for the window takes one as it is queued and hands a copy
	// to the task that brings its result back, so the window reads as busy from the queue until the result reaches it
	// - or the work is dropped - and not only while a worker runs.
	using loading_media_hold = std::shared_ptr<const core::scope_locked_inc>;

	inline loading_media_hold hold_loading_media()
	{
		return std::make_shared<core::scope_locked_inc>(loading_media);
	}

	// Where a path is once folders have moved: beneath one of them, by the platform's rule for when two paths name one
	// place - case aside on Windows, not on Linux - it moves with the first that holds it; elsewhere it stays put.
	inline std::optional<core::file_path> moved_with_folder(
		const core::file_path path, const std::span<const std::pair<core::folder_path, core::folder_path>> folder_moves)
	{
		const auto text = path.str();

		for (const auto& [from, to] : folder_moves)
		{
			const auto prefix = from.text().sv();
			const auto consumed = core::path_prefix_length(text, prefix);
			if (consumed == std::string_view::npos) continue;
			// A root keeps its separator, so the boundary is tested on the text without it.
			const auto boundary = prefix.empty() ? size_t{0} : consumed - (core::is_path_sep(prefix.back()) ? 1 : 0);
			if (boundary >= text.size() || !core::is_path_sep(text[boundary])) continue;
			return core::file_path(std::format("{}{}", to.text(), std::string_view(text).substr(boundary)));
		}

		return {};
	}
}

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

// The tree of view elements, libdesktop's UI element tree (libdesktop/ui/elements.h), named here for the
// headers that pass one around.
namespace ui
{
	class elements;
}

using view_elements = ui::elements;
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
	tags,
	// The collection on a globe beside its timeline; not a task, and opens its items in Items.
	explore,
	// The people the collection names and the faces nobody has named (docs/faces.md#9-the-faces-view); not a task.
	faces,
	// Every setting, on one page; not a task, and not reached from one.
	options
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

// docs/design.md#selection-and-viewing: what a tick does when the displayed item reaches its end.
enum class playback_advance
{
	none,    // nothing has ended yet
	stop,    // the sequence cannot continue
	hold,    // stay on this item - repeat one, or a wrap that lands back on it
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

	// False when nothing is displayed, or the view cannot play. A running command or an open menu does not
	// reach here: the item waits at its end until it has gone.
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

// docs/design.md#metadata-editing-and-files: every destination-writing operation resolves
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
	// Decodes, scales and encodes the stills scan_displayed_items has read, on several threads, so the thread
	// that reads the visible plan's sources moves on to the next file while the last one decodes.
	thumbnail_decode,
	// Decodes the thumbnails items hold into the surfaces they are drawn from, on several threads: a folder opening
	// asks for hundreds at once, which one thread works through for the best part of a second, and nothing else
	// waits behind them.
	thumbnail_stage,
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
	// Reads the collection into the search index. Its own thread, so a build never waits behind, or
	// holds up, the indexing, search, sidebar and summary queues.
	index_search_single,
	web,
	map_tile,
	// A running Commodore 64: one task for as long as it runs, so its own thread, started on first use.
	emulator,
	// An agent's long work - a Movie render, the duplicate report, the documentation (app_mcp_tools.cpp): its own
	// thread, started on first use, so the window's work queue never waits behind a render.
	agent,
	// Face search's scan (docs/faces.md#32-the-scan): one thread reading pictures in order, started on first use,
	// and the few that decode, detect and embed what it read, and lend the overview a hand with its comparisons.
	faces,
	faces_work,
	// The overview of people and suggestions (docs/faces.md#7-recognizing-people): its own thread, started on first
	// use, so a pass never holds back a picture being read, and a long one never waits behind them.
	faces_overview,
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
	core::const_surface_ptr surface;
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

	metadata_kv(std::string k, std::string v) noexcept : key(std::move(k)), value(std::move(v)) {}

	metadata_kv(const std::string_view k, const std::string_view v) : key(k), value(v) {}

	metadata_kv(const std::string_view k, const char* v) : key(k), value(v) {}

	metadata_kv(const core::cached k, std::string v) : key(k.sv()), value(std::move(v)) {}

	metadata_kv(const core::cached k, const std::string_view v) : key(k.sv()), value(v) {}

	metadata_kv(const core::cached k, const char* v) : key(k.sv()), value(v) {}
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
