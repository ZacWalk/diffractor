// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shared test infrastructure, helper classes, and utility functions
// used across every test file. The taxonomy is documented in docs/testing.md.

#pragma once

#include "model.h"
#include "model_db.h"
#include "model_index.h"
#include "model_locations.h"
#include "files.h"
#include "test.h"
#include "av_player.h"
#include "ui_controls.h"
#include "app_command_line.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared constants
///////////////////////////////////////////////////////////////////////////////////////////////////

inline core::folder_path test_files_folder()
{
	return known_path(platform::known_folder::test_files_folder);
}

// Beside the test folder, not in it: expected_cached_item_count below counts what is in there.
inline core::folder_path test_formats_folder()
{
	return test_files_folder().parent().combine("test-formats");
}

// Files an encoder wrote once, kept as written because it writes other bytes on other platforms: a
// record of what decodes them could not be taken over files written afresh.
inline core::folder_path test_generated_folder()
{
	return test_files_folder().parent().combine("test-generated");
}
inline constexpr sizei thumbnail_max_dimension = {256, 256};
inline constexpr int expected_cached_item_count = 49;

inline constexpr auto long_text =
	"The Commodore 64, also known as the C64, C-64, C= 64, or occasionally CBM 64 or VIC-64, is an 8-bit home computer introduced in January 1982 by Commodore International. "
	"It is listed in the Guinness World Records as the highest-selling single computer model of all time, with independent estimates placing the number sold between 10 and 17 million units. "
	"Volume production started in early 1982, with machines being released on to the market in August at a price of US $595(roughly equivalent to $1, 500 in 2015)."
	"Preceded by the Commodore VIC - 20 and Commodore PET, the C64 takes its name from its 64 kilobytes(65, 536 bytes) of RAM, and has technologically superior sound and graphical specifications when compared to some earlier systems such as the Apple II and Atari 800, with multi - color sprites and a more advanced sound processor.";

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared helper classes
///////////////////////////////////////////////////////////////////////////////////////////////////

class null_item_results_ui final : public df::status_i
{
public:
	int abort_count = 0;
	int complete_count = 0;

	void start_item(const std::string_view name) override
	{
	}

	void end_item(const std::string_view name, const item_status status) override
	{
	}

	bool has_failures() const override { return false; }

	void abort(const std::string_view error_message) override
	{
		++abort_count;
	}

	void complete(const std::string_view message) override
	{
		++complete_count;
	}

	void show_errors() override
	{
	}

	void message(const std::string_view message, int64_t pos, int64_t total) override
	{
	}

	void show_message(const std::string_view message) override
	{
	}

	bool is_canceled() const override { return false; }

	void wait_for_complete() const override
	{
	}
};

// Not final: a test that needs one member answered differently derives from it rather than
// restating every override.
class null_state_strategy : public state_strategy
{
public:
	int toggle_full_screen_count = 0;

	explicit null_state_strategy() = default;

	void toggle_full_screen() override
	{
		++toggle_full_screen_count;
	}

	bool can_open_search(const df::search_t& path) override { return true; }

	void report_scope_unavailable(const df::search_t& path) override
	{
	}

	void element_broadcast(const view_element_event& event) override
	{
	}

	void item_focus_changed(const df::item_element_ptr& focus, const df::item_element_ptr& previous) override
	{
	}

	void display_changed() override
	{
	}

	void view_changed(view_type m) override
	{
	}

	void search_complete(const df::search_t& path, bool path_changed) override
	{
	}

	void invoke(const commands id) override
	{
	}

	void make_visible(const df::item_element_ptr& i) override
	{
	}

	void command_hover(const ui::command_ptr& c, const recti window_bounds) override
	{
	}

	bool is_command_checked(const commands cmd) override { return false; }

	ui::command_ptr find_command(const commands cmd) const override { return nullptr; }

	void focus_view() override
	{
	}

	void delete_items(const df::item_set& items) override
	{
	}

	void invalidate_view(const view_invalid invalid) override
	{
	}

	void track_menu(const ui::frame_ptr& parent, const recti bounds,
	                const std::vector<ui::command_ptr>& commands) override
	{
	}

	void free_graphics_resources(const bool items_only, const bool offscreen_only) override
	{
	}
};

// The one gazetteer shared by every test: loading a 24 MB index per context is not affordable.
location_cache& test_locations();

// Measures nothing, so a layout test observes only the arrangement the layout code chose rather than
// the font metrics of whichever machine ran it.
class flex_test_measure_context final : public render::measure_context
{
public:
	sizei measure_text(std::string_view text, render::font_face font, render::text_style style, int cx,
	                   int cy = 0) override
	{
		return {};
	}

	int text_line_height(render::font_face font) override
	{
		return 0;
	}

	render::text_layout_ptr create_text_layout(render::font_face font) override
	{
		return {};
	}
};

class flex_test_element final : public view_element
{
	sizei _desired;

public:
	explicit flex_test_element(const sizei desired) : _desired(desired)
	{
		padding(0);
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		return {std::min(_desired.cx, width_limit), _desired.cy};
	}
};

class null_async_strategy : public async_strategy
{
public:
	location_cache& locations = test_locations();

	void queue_ui(const std::function<void()> f) override { f(); }

	void queue_media_preview(std::function<void(media_preview_state&)>, bool) override
	{
	}

	void queue_database(const std::function<void(database&)> f) override
	{
	}

	void queue_tile_db(const std::function<void(tile_cache_db&)> f) override
	{
	}

	void web_service_cache(std::string key, std::function<void(const std::string&)> f) override
	{
	}

	void web_service_cache(std::string key, std::string value) override
	{
	}

	void queue_async(async_queue q, const std::function<void()> f) override { f(); }

	// The delay is a scheduling concession to the running application; a test wants the result.
	void queue_async_after(async_queue q, uint32_t, const std::function<void()> f) override { f(); }

	void queue_location(const std::function<void(location_cache&)> f) override
	{
		if (!locations.is_index_loaded()) locations.load_index();
		f(locations);
	}

	void invalidate_view(const view_invalid invalid) override
	{
	}
};

class deferred_async_strategy final : public null_async_strategy
{
	std::deque<std::function<void()>> _ui;
	std::deque<std::pair<async_queue, std::function<void()>>> _workers;
	view_invalid _invalids = view_invalid::none;

public:
	void queue_ui(const std::function<void()> f) override
	{
		_ui.emplace_back(f);
	}

	void queue_async(const async_queue q, const std::function<void()> f) override
	{
		_workers.emplace_back(q, f);
	}

	// Deferred like any other worker task; run_next drives it, so the debounce is not a test's problem.
	void queue_async_after(const async_queue q, uint32_t, const std::function<void()> f) override
	{
		_workers.emplace_back(q, f);
	}

	void invalidate_view(const view_invalid invalid) override
	{
		_invalids |= invalid;
	}

	void drain_ui()
	{
		while (!_ui.empty())
		{
			auto f = std::move(_ui.front());
			_ui.pop_front();
			f();
		}
	}

	size_t pending_ui_count() const
	{
		return _ui.size();
	}

	size_t pending_worker_count(const async_queue q) const
	{
		return std::ranges::count(_workers, q, &std::pair<async_queue, std::function<void()>>::first);
	}

	bool was_invalidated(const view_invalid invalid) const
	{
		return (_invalids & invalid) != view_invalid::none;
	}

	bool run_next(const async_queue q)
	{
		const auto found = std::ranges::find(_workers, q, &std::pair<async_queue, std::function<void()>>::first);
		if (found == _workers.end()) return false;

		const auto f = std::move(found->second);
		_workers.erase(found);
		f();
		return true;
	}
};

class temp_files
{
	mutable core::folder_path _folder;
	core::file_paths _paths;
	mutable int _folder_seq = 0;
	mutable bool _owns_folder = false;

	// Base folder for temp files. Overridable via the /test-temp:<path> command
	// line option so round-trip tests can be run against a network drive.
	static core::folder_path temp_base()
	{
		if (!command_line.test_temp_folder.empty())
		{
			return core::folder_path(command_line.test_temp_folder);
		}

		return platform::temp_folder();
	}

	// Resolved lazily so the /test-temp override (parsed after this global is
	// constructed) is honoured on first use.
	core::folder_path resolve_folder() const
	{
		if (_folder.is_empty())
		{
			const auto base = temp_base();
			if (!platform::exists(base))
			{
				platform::create_folder(base);
			}

			for (auto attempt = 0; attempt < 100; ++attempt)
			{
				const auto candidate = base.combine(std::format("diffractor-{}-{}", core::tick_count(), attempt));
				if (platform::exists(candidate)) continue;

				const auto result = platform::create_folder(candidate);
				if (result.success())
				{
					_folder = candidate;
					_owns_folder = true;
					break;
				}
			}

			if (_folder.is_empty())
			{
				throw std::runtime_error("Failed to create an exclusive test temp folder");
			}
		}

		return _folder;
	}

public:
	temp_files() = default;

	explicit temp_files(const core::folder_path existing_folder) : _folder(existing_folder)
	{
	}

	~temp_files()
	{
		delete_temps();
	}

	temp_files(const temp_files& other) noexcept = delete;
	temp_files(temp_files&& other) noexcept = delete;
	temp_files& operator=(const temp_files& other) = delete;
	temp_files& operator=(temp_files&& other) noexcept = delete;

	std::vector<std::string> delete_temps()
	{
		std::vector<std::string> errors;
		core::file_paths remaining_paths;

		for (const auto& path : _paths)
		{
			if (!platform::exists(path)) continue;

			const auto result = platform::delete_file(path);
			if (result.failed() && platform::exists(path))
			{
				errors.emplace_back(result.format_error("delete temp file", path.str()));
				remaining_paths.emplace_back(path);
			}
		}

		_paths = std::move(remaining_paths);

		if (_owns_folder && !_folder.is_empty() && platform::exists(_folder))
		{
			std::error_code ec;
			std::filesystem::remove_all(platform::to_stream_path(_folder), ec);
			if (ec)
			{
				errors.emplace_back(std::format("delete temp folder\n{}\n{}", _folder.text(), ec.message()));
			}

			if (!platform::exists(_folder))
			{
				_folder = {};
				_owns_folder = false;
				_folder_seq = 0;
				_paths.clear();
			}
		}

		return errors;
	}

	core::folder_path folder() const
	{
		return resolve_folder();
	}

	core::file_path next_path(const std::string_view ext = ".jpg")
	{
		const auto folder = resolve_folder();

		auto result = platform::temp_file(ext, folder);
		_paths.emplace_back(result);
		return result;
	}

	// A private subfolder for tests that open a folder as a search scope. Such a test must not use
	// folder(): that is shared by the whole run, so its cost and its result counts would depend on
	// whatever files earlier tests happened to leave behind.
	core::folder_path next_folder(const std::string_view name)
	{
		const auto result = resolve_folder().combine(std::format("{}-{}", name, ++_folder_seq));

		if (!platform::exists(result))
		{
			platform::create_folder(result);
		}

		return result;
	}

	core::file_path next_path_in(const core::folder_path folder, const std::string_view ext = ".jpg")
	{
		if (!platform::exists(folder))
		{
			platform::create_folder(folder);
		}

		auto result = platform::temp_file(ext, folder);
		_paths.emplace_back(result);
		return result;
	}
};

struct metadata_type
{
	static constexpr uint32_t EXIF = 1;
	static constexpr uint32_t IPTC = 2;
	static constexpr uint32_t XMP = 4;
	static constexpr uint32_t ALL = EXIF | IPTC | XMP;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared mutable globals (defined in test_runner.cpp)
///////////////////////////////////////////////////////////////////////////////////////////////////

extern temp_files _temps;
extern std::atomic_int test_version;
extern core::cancel_token test_token;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared fixtures built in memory
///////////////////////////////////////////////////////////////////////////////////////////////////

// A SID tune small enough to write out in full. Its init routine sets voice 1 sawing at 440Hz at full
// volume, and its play routine nudges that voice's frequency every frame, so the tune sounds, and its
// registers change, from the first frame. It holds three tunes and starts on the second, so selecting
// one has something to choose from.
inline std::vector<uint8_t> make_test_sid_tune()
{
	std::vector<uint8_t> t(0x7C, 0);
	const auto text = [&t](const size_t at, const std::string_view s) { std::ranges::copy(s, t.begin() + at); };

	text(0x00, "PSID");
	t[0x05] = 2; // version
	t[0x07] = 0x7C; // data offset; the load address is the data's first two bytes
	t[0x0A] = 0x10; // init at $1000
	t[0x0C] = 0x10;
	t[0x0D] = 0x20; // play at $1020
	t[0x0F] = 3; // tunes
	t[0x11] = 2; // the tune it starts on
	text(0x16, "Diffractor Test Tune");
	text(0x36, "Test Author");
	text(0x56, "1987 Test Publisher");
	t[0x77] = 0x14; // a 6581, PAL

	const uint8_t program[] = {
		0x00, 0x10, // loads at $1000
		0xA9, 0x0F, 0x8D, 0x18, 0xD4, // LDA #$0F, STA $D418: full volume
		0xA9, 0x45, 0x8D, 0x00, 0xD4, // voice 1 frequency $1D45: A4 on a PAL machine
		0xA9, 0x1D, 0x8D, 0x01, 0xD4,
		0xA9, 0x09, 0x8D, 0x05, 0xD4, // attack 0, decay 9
		0xA9, 0xF0, 0x8D, 0x06, 0xD4, // sustain 15, release 0
		0xA9, 0x21, 0x8D, 0x04, 0xD4, // sawtooth, gate on
		0x60, // RTS
		0xEA, // NOP, up to $1020
		0xE6, 0xFB, // INC $FB
		0xA5, 0xFB, // LDA $FB
		0x8D, 0x00, 0xD4, // STA $D400
		0x60, // RTS
	};

	t.insert(t.end(), std::begin(program), std::end(program));
	return t;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared test context
///////////////////////////////////////////////////////////////////////////////////////////////////

struct shared_test_context
{
	null_async_strategy as;
	// Searches resolve against the same loaded gazetteer the async strategy uses, so read-time
	// attribution is exercised rather than silently answering from an empty index.
	location_cache& locations = test_locations();
	index_state test_index;
	index_state empty_index;
	bool loaded = false;

	shared_test_context() : test_index(as, locations), empty_index(as, locations)
	{
	}

	void lazy_load_index();
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared helper functions (defined in tests_core_utils.cpp)
///////////////////////////////////////////////////////////////////////////////////////////////////

file_scan_result ff_scan_file(files& ff, core::file_path path, std::string_view xmp_sidecar = {});
// Ask files::update to re-scan what it wrote, the way the app does: through the coherent handle
// replace_file kept open, so a by-name reopen cannot return pre-swap content over SMB. The sidecar
// is left unset so the write resolves the one it just created.
rescan_spec ff_inspect_rescan(core::file_path path);
// The result of that scan, falling back to a by-name scan if the write produced none.
file_scan_result ff_scan_after_update(files& ff, file_update_result& result, core::file_path path,
                                      std::string_view xmp_sidecar = {});
file_scan_result ff_scan_and_load_thumb(files& ff, core::file_path path,
                                        std::string_view xmp_sidecar = {});
void assert_metadata(const prop::item_metadata& expected, const prop::item_metadata& actual,
                     std::string_view message = {});
prop::item_metadata_ptr extract_properties(core::file_path path, uint32_t t = metadata_type::ALL);
prop::item_metadata_ptr expected_test_jpg();
prop::item_metadata_ptr metadata_from_cache(index_state& index, core::file_path path);
df::index_file_item make_index_file_info(core::date_t date);
df::item_element_ptr load_item(index_state& index, core::file_path path, bool load_thumb);
std::string_view detect_xmp_sidecar(core::file_path path);
void build_index(index_state& index, database& db);
int count_search_results(index_state& index, const df::search_t& search);
int count_search_results(index_state& index, std::string_view query);
std::shared_ptr<av_player> make_test_player();
// Never opened, so it holds no file and no decoder threads; only its identity is of interest.
std::shared_ptr<av_session> make_test_session();
std::shared_ptr<av_session> make_test_session(std::function<void(core::file_path, double)> save_position);
// A session as a player makes one: the presenter sets decode_wake when it takes frames from the queue.
std::shared_ptr<av_session> make_test_session_with_decode_wake(std::shared_ptr<const core::thread_event> decode_wake);

// The gazetteer costs seconds to load, so every test that needs real place data shares one
// loaded instance. A test that mutates it (set_display_language) must restore it before returning.
location_cache& test_locations();

inline void write_test_file(const core::file_path path, const std::string_view text)
{
	std::ofstream fs(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
	fs << text;
}

inline std::string read_test_file(const core::file_path path)
{
	const auto data = platform::blob_from_file(path);
	return {std::bit_cast<const char*>(data.data()), data.size()};
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared prop_test helper for search matching tests
///////////////////////////////////////////////////////////////////////////////////////////////////

class prop_test
{
	df::index_file_item _f;
	uint32_t now_days = core::date_t(2000, 1, 1).to_days();

public:
	prop_test()
	{
		_f.ft = files::file_type_from_name("test.jpg");
	}

	prop_test& tag(const std::string_view s)
	{
		_f.safe_ps()->tags = core::cache(s);
		return *this;
	}

	prop_test& date(const int y, const int m, const int d)
	{
		_f.safe_ps()->dates.add(prop::date_source::exif_original, core::date_t(y, m, d));
		return *this;
	}

	prop_test& file_created_date(const int y, const int m, const int d)
	{
		_f.file_created = core::date_t(y, m, d);
		_f.file_modified = core::date_t(y, m, d);
		return *this;
	}

	prop_test& desc(const std::string_view s)
	{
		_f.safe_ps()->description = core::cache(s);
		return *this;
	}

	prop_test& genre(const std::string_view s)
	{
		_f.safe_ps()->genre = core::cache(s);
		return *this;
	}

	prop_test& rate(const int16_t n)
	{
		_f.safe_ps()->rating = n;
		return *this;
	}

	prop_test& track(const uint8_t n, const uint8_t of = 0)
	{
		_f.safe_ps()->track = core::xy8::make(n, of);
		return *this;
	}

	prop_test& disk(const uint8_t n, const uint8_t of = 0)
	{
		_f.safe_ps()->disk = core::xy8::make(n, of);
		return *this;
	}

	prop_test& episode(const uint8_t n, const uint8_t of = 0)
	{
		_f.safe_ps()->episode = core::xy8::make(n, of);
		return *this;
	}

	prop_test& iso(const uint16_t n)
	{
		_f.safe_ps()->iso_speed = n;
		return *this;
	}

	prop_test& duration(const uint16_t seconds)
	{
		_f.safe_ps()->duration = seconds;
		return *this;
	}

	prop_test& exposure_time(const double seconds)
	{
		_f.safe_ps()->exposure_time = static_cast<float>(seconds);
		return *this;
	}

	prop_test& digitized(const int y, const int m, const int d)
	{
		_f.safe_ps()->dates.add(prop::date_source::exif_digitized, core::date_t(y, m, d));
		return *this;
	}

	prop_test& orientation(const core::orientation o)
	{
		_f.safe_ps()->orientation = o;
		return *this;
	}

	prop_test& dimensions(const int cx, const int cy)
	{
		_f.safe_ps()->width = static_cast<uint16_t>(cx);
		_f.safe_ps()->height = static_cast<uint16_t>(cy);
		return *this;
	}

	prop_test& focal_length(const double mm)
	{
		_f.safe_ps()->focal_length = static_cast<float>(mm);
		return *this;
	}

	prop_test& audio_channels(const uint16_t n)
	{
		_f.safe_ps()->audio_channels = n;
		return *this;
	}

	prop_test& audio_sample_rate(const uint16_t n)
	{
		_f.safe_ps()->audio_sample_rate = n;
		return *this;
	}

	prop_test& media_type(const std::string_view name)
	{
		_f.ft = files::file_type_from_name(name);
		return *this;
	}

	prop_test& kind(const prop::media_kind k)
	{
		_f.safe_ps()->kind = k;
		return *this;
	}

	prop_test& media_position(const double seconds)
	{
		_f.safe_ps()->media_position = seconds;
		return *this;
	}

	prop_test& is_match(const std::string_view query)
	{
		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search, now_days);

		assert_equal(true, matcher.match_all_terms({}, _f).is_match(), query);
		return *this;
	}

	prop_test& is_not_match(const std::string_view query)
	{
		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search, now_days);

		assert_equal(false, matcher.match_all_terms({}, _f).is_match(), query);
		return *this;
	}
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Additional assert_equal overloads for test files
///////////////////////////////////////////////////////////////////////////////////////////////////

static void assert_equal(const core::orientation expected, const core::orientation actual,
                         const std::string_view name = {},
                         const std::string_view message = {})
{
	assert_equal(orientation_to_string(expected), orientation_to_string(actual), name, message);
}

static void assert_equal(df::search_term_modifier_bool expected, df::search_term_modifier_bool actual,
                         const std::string_view name = {}, const std::string_view message = {})
{
	assert_equal(static_cast<int>(expected), static_cast<int>(actual), name, message);
}
