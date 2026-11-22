// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tests for application coordination (app*, view_* task views): command registration, what
// each view declares for the frame's bars and how a bar is arranged, input dispatch, settings
// persistence and the Options view that edits them, headless Movie batches, and rename, import, sync and
// convert planning.

#include "pch.h"
#include "files.h"
#include "test.h"
#include "model_db.h"
#include "model_faces_search.h"
#include "model_index.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "util_crash_files_db.h"
#include "util_zip.h"
#include "app_command_line.h"
#include "app_dup_report.h"
#include "app_mcp.h"
#include "app_movie_batch.h"
#include "app_util.h"
#include "app_settings.h"
#include "app_text.h"
#include "app_commands.h"
#include "app_search.h"
#include "app_match.h"
#include "ui_dialog.h"
#include "app.h"
#include "app_sidebar.h"
#include "app_dialogs.h"
#include "av_format.h"
#include "view_movie.h"
#include "view_rename.h"
#include "view_batch.h"
#include "view_import.h"
#include "view_faces.h"
#include "view_sync.h"
#include "view_tags.h"
#include "view_items.h"
#include "view_media.h"
#include "view_options.h"
#include "libdesktop/ui/test_support.h"

extern std::function<void()> test_during_detach_file_handles;

// Rolling a release back is ordinary, and the settings store is shared with the build rolled back
// to. An order it cannot name falls through its switches to a default, so the user silently loses
// the order they chose - which is exactly the user this release's migration just moved onto one.
static void should_store_an_order_an_older_build_can_read()
{
	const auto plain = [](const group_by o)
	{
		return static_cast<uint32_t>(group_order_older_builds_understand(o));
	};
	const auto ex = [](const group_by o)
	{
		return static_cast<uint32_t>(o);
	};

	// Every order an older build already knows is stored unchanged.
	for (const auto order : {group_by::file_type, group_by::size, group_by::date_created, group_by::date_modified,
	                         group_by::camera, group_by::folder})
	{
		assert_equal(static_cast<int>(order), static_cast<int>(group_order_older_builds_understand(order)),
		             "a known order is stored as itself");
	}

	// The one it does not is stood in for by the order that meant the same thing to it.
	assert_equal(static_cast<int>(group_by::date_created),
	             static_cast<int>(group_order_older_builds_understand(group_by::date_original)),
	             "Original stands in as date created");
	assert_equal(static_cast<int>(sort_by::date_created),
	             static_cast<int>(sort_order_older_builds_understand(sort_by::date_original)),
	             "and the same for sorting");

	// A round trip through both keys returns the real order, stand-in or not.
	assert_equal(static_cast<int>(group_by::date_original),
	             static_cast<int>(resolve_stored_order<group_by>(plain(group_by::date_original),
	                                                             ex(group_by::date_original), true,
	                                                             group_order_older_builds_understand)),
	             "the true order survives its own stand-in");
	assert_equal(static_cast<int>(group_by::camera),
	             static_cast<int>(resolve_stored_order<group_by>(plain(group_by::camera), ex(group_by::camera), true,
	                                                             group_order_older_builds_understand)),
	             "and so does an order needing none");

	// An older build knows only the plain key. When it has written one, that is the newer choice and
	// the stale extended value must not overrule it.
	assert_equal(static_cast<int>(group_by::camera),
	             static_cast<int>(resolve_stored_order<group_by>(static_cast<uint32_t>(group_by::camera),
	                                                             ex(group_by::date_original), true,
	                                                             group_order_older_builds_understand)),
	             "a choice made in the older build wins");

	// Upgrading for the first time: only the plain key exists.
	assert_equal(static_cast<int>(group_by::date_created),
	             static_cast<int>(resolve_stored_order<group_by>(static_cast<uint32_t>(group_by::date_created), 0,
	                                                             false, group_order_older_builds_understand)),
	             "a store with no extended key answers with what it has");
}

static void should_persist_to_ini_file()
{
	const auto settings_folder = test_temps.folder().combine("ini-settings");
	platform::create_folder(settings_folder);
	const auto settings = platform::create_ini_file_settings(settings_folder);

	// A folder without an INI is a new settings root.
	assert_equal(true, settings->root_created(), "root_created", "INI file settings");

	// Test uint32_t
	constexpr uint32_t test_uint32 = 12345;
	settings->write("test_section", "uint32_value", test_uint32);
	const auto existing_settings = platform::create_ini_file_settings(settings_folder);
	assert_equal(false, existing_settings->root_created(), "existing root", "INI file settings");
	uint32_t read_uint32 = 0;
	assert_equal(true, settings->read("test_section", "uint32_value", read_uint32), "read uint32", "INI file settings");
	assert_equal(test_uint32, read_uint32, "uint32 value", "INI file settings");

	// Test uint64_t
	constexpr uint64_t test_uint64 = 0xFFFFFFFFFFFFull;
	settings->write("test_section", "uint64_value", test_uint64);
	uint64_t read_uint64 = 0;
	assert_equal(true, settings->read("test_section", "uint64_value", read_uint64), "read uint64", "INI file settings");
	assert_equal(test_uint64, read_uint64, "uint64 value", "INI file settings");

	// Test string
	const auto test_string = "Hello, World! With special chars: äöü"s;
	settings->write("test_section", "string_value", test_string);
	std::string read_string;
	assert_equal(true, settings->read("test_section", "string_value", read_string), "read string", "INI file settings");
	assert_equal(test_string, read_string, "string value", "INI file settings");

	for (const auto value : {"\"folder with spaces\" \"photo.jpg\""sv, "'quoted tag'"sv})
	{
		settings->write("test_section", "literal_value", value);
		const auto reopened = platform::create_ini_file_settings(settings_folder);
		std::string read_literal;
		assert_equal(true, reopened->read("test_section", "literal_value", read_literal), "literal value is stored");
		assert_equal(value, read_literal, "settings preserve literal quotes");
	}

	// Test binary data (base64 encoded)
	const std::vector<uint8_t> test_binary = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD};
	settings->write("test_section", "binary_value", core::cspan{test_binary.data(), test_binary.size()});
	std::vector<uint8_t> read_buffer(test_binary.size());
	size_t read_len = read_buffer.size();
	assert_equal(true, settings->read("test_section", "binary_value", read_buffer.data(), read_len), "read binary",
	             "INI file settings");
	assert_equal(test_binary.size(), read_len, "binary length", "INI file settings");
	for (size_t i = 0; i < test_binary.size(); ++i)
	{
		assert_equal(static_cast<uint32_t>(test_binary[i]), static_cast<uint32_t>(read_buffer[i]), "binary byte",
		             "INI file settings");
	}
}

// Builds before 1.27.3 wrote the recent lists unquoted and the INI reader stripped a quote pair from
// each end, so an upgraded profile can hold an entry made of several paths glued by stray quotes.
// The value below is the shape a real profile was found holding.
static void should_drop_damaged_recent_entries()
{
	const auto root = core::windows_path_semantics ? "C:\\Users\\someone\\"s : "/home/someone/"s;
	const auto damaged =
		std::format(R"({0}Pictures"' "{0}Videos" "{0}Music" "{0}OneDrive - Work\Desktop" '"{0}Downloads)", root);

	assert_equal(true, recent_state::is_damaged_entry(std::format(R"({0}Pictures"' "{0}Videos)", root)),
	             "a quote wedged inside a path is damage");
	for (const auto& healthy :
	     {root + "Pictures", std::format(R"({}photos "new york")", root), "\"" + root + "My Photos\" beach",
	      "Bob's \"party\""s, "@photo beach"s, std::format(R"({}Photos loc:"Boulder City")", root),
	      std::format(R"({}Photos -"draft")", root), std::format(R"({}Bob's photos tag:"summer")", root)})
	{
		assert_equal(false, recent_state::is_damaged_entry(healthy), "a real entry is kept", healthy);
	}

	{
		// A folder-scoped search with a quoted term is ordinary recent text and must survive a restart.
		const auto search = std::format(R"({}Photos loc:"Boulder City")", root);
		const auto search_folder = test_temps.folder().combine("ini-recent-search");
		platform::create_folder(search_folder);
		const auto store = platform::create_ini_file_settings(search_folder);
		recent_state saved;
		saved.add(search);
		saved.write("", "recent_searches", store);
		recent_state loaded;
		loaded.read("", "recent_searches", platform::create_ini_file_settings(search_folder));
		assert_equal(1_z, loaded.items().size(), "the quoted search is reloaded");
		assert_equal(search, loaded.items().front(), "and reloaded exactly");
	}

	const auto settings_folder = test_temps.folder().combine("ini-recent");
	platform::create_folder(settings_folder);
	const auto settings = platform::create_ini_file_settings(settings_folder);
	settings->write("", "recent_searches", damaged + " " + root + "Kept");

	assert_equal(true, recent_state::is_damaged_entry(std::format(R"('"{}Downloads)", root)),
	             "stray quotes leading a path are damage");

	recent_state recents;
	recents.read("", "recent_searches", settings);
	const std::vector<std::string> expected{root + "Videos", root + "Music", root + R"(OneDrive - Work\Desktop)",
	                                        root + "Kept"};
	assert_equal(core::combine(expected, "|", false), core::combine(recents.items(), "|", false),
	             "exactly the readable entries load, in order");

	recents.write("", "recent_searches", settings);
	recent_state reread;
	reread.read("", "recent_searches", settings);
	assert_equal(true, recents.items() == reread.items(), "the cleaned list round-trips");
}

#ifndef _WIN32
static void should_commit_linux_ini_file_settings_after_durable_save()
{
	const auto settings_folder = test_temps.folder().combine("linux-ini-settings");
	platform::create_folder(settings_folder);
	const auto settings_path = settings_folder.combine_file("diffractor.ini");

	auto settings = platform::create_ini_file_settings(settings_folder);
	assert_equal(true, settings->write("test_section", "string_value", "original"), "initial write");

	for (const auto failure :
	     {platform::linux_settings_test_failure::after_open, platform::linux_settings_test_failure::after_write,
	      platform::linux_settings_test_failure::before_replace})
	{
		platform::test_linux_settings_failure(failure);
		assert_equal(false, settings->write("test_section", "string_value", "updated"), "failed write reports false");
		platform::test_linux_settings_failure(platform::linux_settings_test_failure::none);

		std::string same_instance;
		assert_equal(true, settings->read("test_section", "string_value", same_instance),
		             "same instance still has committed value");
		assert_equal("original", same_instance, "failed write does not publish uncommitted value");
		assert_equal(false, platform::exists(settings_folder.combine_file(".diffractor.ini.tmp")),
		             "failed write removes stage");

		const auto reopened = platform::create_ini_file_settings(settings_folder);
		std::string reopened_value;
		assert_equal(true, reopened->read("test_section", "string_value", reopened_value),
		             "reopened settings read committed value");
		assert_equal("original", reopened_value, "failed write preserves on-disk value");
		assert_equal(true, platform::exists(settings_path), "settings file remains present");
	}

	assert_equal(true, settings->write("test_section", "string_value", "updated"), "write succeeds after failures");
	const auto reopened = platform::create_ini_file_settings(settings_folder);
	std::string reopened_value;
	assert_equal(true, reopened->read("test_section", "string_value", reopened_value), "reopened updated value");
	assert_equal("updated", reopened_value, "successful write reaches disk");
}

static void should_skip_unchanged_linux_ini_file_settings()
{
	const auto settings_folder = test_temps.folder().combine("linux-ini-unchanged");
	platform::create_folder(settings_folder);
	auto settings = platform::create_ini_file_settings(settings_folder);

	platform::test_linux_reset_settings_save_count();
	assert_equal(true, settings->write("test_section", "string_value", "same"), "initial write");
	const auto saves_after_first = platform::test_linux_settings_save_count();
	assert_equal(true, saves_after_first > 0, "initial write saves");

	assert_equal(true, settings->write("test_section", "string_value", "same"), "unchanged write succeeds");
	assert_equal(saves_after_first, platform::test_linux_settings_save_count(), "unchanged write does not save");
}

static void should_commit_linux_ini_file_settings_from_owned_stage()
{
	const auto settings_folder = test_temps.folder().combine("linux-ini-owned-stage");
	platform::create_folder(settings_folder);
	auto first = platform::create_ini_file_settings(settings_folder);
	auto second = platform::create_ini_file_settings(settings_folder);

	assert_equal(true, first->write("test_section", "string_value", "original"), "initial write");

	bool nested = false;
	platform::test_linux_before_settings_replace = [&]
	{
		if (nested) return;
		nested = true;
		platform::test_linux_before_settings_replace = {};
		assert_equal(true, second->write("test_section", "string_value", "second"), "nested write");
	};

	const auto first_write = first->write("test_section", "string_value", "first");
	platform::test_linux_before_settings_replace = {};

	assert_equal(true, first_write, "outer write succeeds with its own stage");
	const auto reopened = platform::create_ini_file_settings(settings_folder);
	std::string value;
	assert_equal(true, reopened->read("test_section", "string_value", value), "reopened setting");
	assert_equal("first", value, "outer complete write wins with complete bytes");
}
#endif

static void should_rename_with_substitutions()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const core::file_path src_path(test_files_folder(), "Test.jpg");
	const auto save_path_1 = test_temps.next_path(".jpg");
	const auto save_path_2 = test_temps.next_path(".jpg");

	platform::copy_file(src_path, save_path_1, false, false);

	const auto test_item = load_item(index, save_path_1, false);

	assert_equal(true, test_item->rename(index, save_path_2.file_name_without_extension()).success(), "can rename");
	assert_equal(save_path_2.name(), test_item->name(), "renamed");
	assert_equal(true, platform::exists(save_path_2), "renamed exists");

	// Verify item can be found at new path after rename
	const auto reloaded = load_item(index, save_path_2, false);
	assert_equal(save_path_2.name(), reloaded->name(), "index finds renamed item");
}

static void should_format_rename()
{
	assert_equal("photo-005", format_sequence("original", "photo-###", 5), "sequence format");
	assert_equal("photo-042", format_sequence("original", "photo-###", 42), "sequence zero pad");
	assert_equal("photo-1000", format_sequence("original", "photo-###", 1000), "sequence does not truncate");
	assert_equal("trip-0001", format_sequence("original", "trip-####", 1), "four digit sequence");
	assert_equal("riginal", format_sequence("original", "???????", 0), "question mark substitution");
	assert_equal("al-file", format_sequence("original", "?\?-file", 0), "mixed question mark substitution");
	// Typed below zero, a start number has no digits to give: each one taken from it was a byte from
	// before the digit table.
	assert_equal("photo-000", format_sequence("original", "photo-###", -3), "a negative start counts from zero");
}

// The TV template renders as release names and media servers write them: two-digit seasons and episodes, and
// E00 for a special. It used to render Chernobyl.S1EUnknown.
static void should_rename_episodes_with_padded_numbers()
{
	const auto episode = std::make_shared<prop::item_metadata>();
	episode->show = core::cache("Chernobyl");
	episode->title = core::cache("Please Remain Calm");
	episode->season = 1;
	episode->episode = prop::number_pair(5, 0);
	assert_equal("Chernobyl.S01E05.Please Remain Calm",
	             prop::replace_tokens("{show}.S{season}E{episode}.{title}", episode, "original", {}), "an episode");

	const auto special = std::make_shared<prop::item_metadata>(*episode);
	special->season = 3;
	special->episode = {0, 0};
	special->is_special = true;
	assert_equal("Chernobyl.S03E00.Please Remain Calm",
	             prop::replace_tokens("{show}.S{season}E{episode}.{title}", special, "original", {}), "a special");

	const auto film = std::make_shared<prop::item_metadata>();
	film->title = core::cache("Dont Look Up");
	film->year = 2021;
	assert_equal("Dont Look Up (2021)", prop::replace_tokens("{title} ({year})", film, "original", {}), "a film");
}

static void should_rename_name_token_without_extension()
{
	df::item_set items;
	const auto item = std::make_shared<df::item_element>(core::file_path(test_files_folder(), "Test.jpg"),
	                                                     df::index_file_item{});
	items.add(item);

	const auto renames = calc_item_renames(items, "{name}-edited", 1, collision_policy::block_run);
	assert_equal(1ULL, static_cast<uint64_t>(renames.size()), "one rename planned");
	assert_equal("Test-edited", renames.front().new_name, "name token excludes extension");
}

static void should_reject_unusable_rename_targets()
{
	df::item_set items;
	items.add(
		std::make_shared<df::item_element>(core::file_path(test_files_folder(), "Test.jpg"), df::index_file_item{}));

	const auto empty_template = calc_item_renames(items, "", 1, collision_policy::block_run);
	assert_equal(false, can_rename_items(empty_template), "an empty template names nothing");

#ifdef _WIN32
	// Windows drops a trailing space, so the file would not carry the name the preview shows, and a
	// device name is not a file at all. Neither is true of a filesystem that takes the name as given.
	const auto trailing_space = calc_item_renames(items, "photo ", 1, collision_policy::block_run);
	assert_equal("photo ", trailing_space.front().new_name, "the preview shows the template result");
	assert_equal(false, can_rename_items(trailing_space), "a trailing space is rejected");

	const auto device_name = calc_item_renames(items, "con", 1, collision_policy::block_run);
	assert_equal(false, can_rename_items(device_name), "a reserved device name is rejected");
#endif

	const auto usable = calc_item_renames(items, "photo", 1, collision_policy::block_run);
	assert_equal(true, can_rename_items(usable), "an ordinary name is still accepted");
}

static void should_reject_duplicate_rename_targets()
{
	df::item_set items;
	items.add(
		std::make_shared<df::item_element>(core::file_path(test_files_folder(), "Test.jpg"), df::index_file_item{}));
	items.add(
		std::make_shared<df::item_element>(core::file_path(test_files_folder(), "Small.jpg"), df::index_file_item{}));

	const auto renames = calc_item_renames(items, "same", 1, collision_policy::block_run);
	assert_equal(false, can_rename_items(renames), "duplicate rename targets rejected");
	assert_equal(false, renames.front().valid, "first duplicate target invalid");
	assert_equal(false, renames.back().valid, "second duplicate target invalid");
}

static std::vector<rename_source> make_rename_sources(const core::folder_path root,
                                                      const std::vector<std::string>& names)
{
	const core::blob contents = {1};
	std::vector<rename_source> sources;
	for (const auto& name : names)
	{
		const auto path = root.combine_file(name + ".jpg");
		platform::blob_save_to_file(contents, path);
		rename_source source;
		source.source = path;
		source.original_name = name;
		sources.emplace_back(std::move(source));
	}
	return sources;
}

static void should_plan_the_default_rename_template()
{
	const auto root = test_temps.next_folder("rename-default");
	const auto sources = make_rename_sources(root, {"alpha", "beta", "gamma"});

	const auto renames = calc_item_renames(sources, "Item ###", 1, collision_policy::block_run);
	assert_equal(3_z, renames.size(), "one row per item");
	assert_equal("Item 001", renames.front().new_name, "first row uses the start sequence");
	assert_equal(false, renames.front().collides, "a free destination does not collide");
	assert_equal(true, renames.front().valid, "a free destination is valid");
	assert_equal(true, can_rename_items(renames), "the default template can run");

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	const auto with_sidecar = root.combine_file("with-sidecar.jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), with_sidecar, false, false);
	platform::copy_file(core::file_path(test_files_folder(), "IMG_0604.xmp"), with_sidecar.extension(".xmp"), false,
	                    false);

	df::item_set items;
	for (const auto& source : sources) items.add(load_item(index, source.source, false));
	items.add(load_item(index, with_sidecar, false));

	const auto item_renames = calc_item_renames(items, "Item ###", 1, collision_policy::block_run);
	assert_equal(4_z, item_renames.size(), "one row per indexed item");
	for (const auto& rename : item_renames)
		assert_equal(true, rename.valid, std::format("{} is valid", rename.new_name));
	assert_equal(true, can_rename_items(item_renames), "indexed items can run the default template");
}

static void should_rename_a_set_onto_names_it_is_vacating()
{
	const auto root = test_temps.next_folder("rename-rotate");
	auto sources = make_rename_sources(root, {"Item 001", "Item 002", "Item 003"});
	std::ranges::reverse(sources);

	const auto renames = calc_item_renames(sources, "Item ###", 1, collision_policy::block_run);
	assert_equal(true, can_rename_items(renames), "a set may be renamed onto its own names");
	assert_equal(0, count_rename_collisions(renames, collision_policy::block_run), "no row collides");

	// A case-only rename frees nothing, so a second row cannot be cleared to take that name.
	const auto shadowed = make_rename_sources(root, {"only"});
	const auto case_only = calc_item_renames(shadowed, "ONLY", 1, collision_policy::block_run);
	assert_equal(true, can_rename_items(case_only), "a case-only rename is not a collision");
}

// Rename used to revalidate by existence alone. A source replaced between the review and the run was
// then moved under a name chosen for content the user never saw, and under Replace a destination
// that had become a different file was written over. The plan records the identity the run is held
// to, which is the same test Sync makes.
static void should_record_rename_identity()
{
	const auto root = test_temps.next_folder("rename-identity");
	const auto sources = make_rename_sources(root, {"alpha"});
	const auto occupied = root.combine_file("renamed.jpg");
	platform::blob_save_to_file(core::blob{1, 2, 3}, occupied);

	const auto renames = calc_item_renames(sources, "renamed", 1, collision_policy::replace);
	assert_equal(1_z, renames.size(), "one row");

	const auto& row = renames.front();
	assert_equal(true, row.source_attributes.exists(), "the source identity was recorded");
	assert_equal(true, row.destination_attributes.exists(), "so was the destination the row will replace");
	assert_equal(true, unchanged_since_analysis(row.source, row.source_attributes.modified, row.source_attributes.size),
	             "an untouched source still matches the review");
	assert_equal(true,
	             unchanged_since_analysis(row.destination, row.destination_attributes.modified,
	                                      row.destination_attributes.size),
	             "and so does an untouched destination");

	// Both paths still exist, which is all the run used to ask. Only the recorded identity says the
	// content behind them is no longer what was reviewed.
	platform::blob_save_to_file(core::blob{1, 2, 3, 4, 5}, row.source);
	platform::blob_save_to_file(core::blob{9}, row.destination);

	assert_equal(true, platform::exists(row.source), "the source path is still occupied");
	assert_equal(true, platform::exists(row.destination), "and so is the destination path");
	assert_equal(false,
	             unchanged_since_analysis(row.source, row.source_attributes.modified, row.source_attributes.size),
	             "a rewritten source no longer matches the review");
	assert_equal(false,
	             unchanged_since_analysis(row.destination, row.destination_attributes.modified,
	                                      row.destination_attributes.size),
	             "nor does a destination that became another file");
}

static void should_cascade_skipped_rename_rows()
{
	const auto root = test_temps.next_folder("rename-cascade");
	// Item 001 is not part of the plan, so the row that wants it can only be skipped - which leaves
	// Item 002 where it is, so the row that was cleared to take Item 002 has to be skipped too.
	const auto occupied = make_rename_sources(root, {"Item 001"});
	const auto sources = make_rename_sources(root, {"Item 002", "beta"});

	const auto renames = calc_item_renames(sources, "Item ###", 1, collision_policy::skip);
	assert_equal(true, renames.front().skipped, "the blocked row is skipped");
	assert_equal(true, renames.back().skipped, "the row waiting on it is skipped too");
	assert_equal(false, can_rename_items(renames), "a plan that would write nothing cannot run");
}

static void should_group_rename_sidecar_collisions()
{
	const auto root = test_temps.folder().combine(std::format("rename-sidecar-{}", core::tick_count()));
	platform::create_folder(root);
	const auto source = root.combine_file("photo.jpg");
	const auto sidecar = root.combine_file("photo.xmp");
	const core::blob contents = {1};
	platform::blob_save_to_file(contents, source);
	platform::blob_save_to_file(contents, sidecar);
	platform::blob_save_to_file(contents, root.combine_file("renamed.xmp"));

	rename_source item;
	item.source = source;
	item.original_name = "photo";
	item.sidecars.emplace_back(sidecar);

	const auto blocked = calc_item_renames(std::vector{item}, "renamed", 1, collision_policy::block_run);
	assert_equal(false, can_rename_items(blocked), "sidecar collision blocks whole rename");

	const auto renamed = calc_item_renames(std::vector{item}, "renamed", 1, collision_policy::auto_rename);
	assert_equal(true, can_rename_items(renamed), "sidecar collision can auto-rename whole group");
	assert_equal("renamed (2).jpg", renamed.front().destination.name(), "primary uses shared suffix");
	assert_equal("renamed (2).xmp", renamed.front().sidecars.front().second.name(), "sidecar uses shared suffix");
	platform::delete_items({}, {root}, false);
}

// A video's subtitles name their language, and whether they are forced, after its name. Moved with it, each keeps
// what it adds, so two subtitles with one extension never meet on one name; and a new name with a dot keeps all of it.
static void should_rename_subtitles_with_their_language()
{
	const auto root = test_temps.folder().combine(std::format("rename-subtitles-{}", core::tick_count()));
	platform::create_folder(root);
	const core::blob contents = {1};
	const auto video = root.combine_file("Movie.mkv");
	const auto english = root.combine_file("Movie.en.srt");
	const auto forced = root.combine_file("Movie.English.forced.srt");
	for (const auto& f : {video, english, forced}) platform::blob_save_to_file(contents, f);

	rename_source item;
	item.source = video;
	item.original_name = "Movie";
	item.sidecars = {english, forced};

	const auto plan = calc_item_renames(std::vector{item}, "Film", 1, collision_policy::block_run);
	assert_equal(true, can_rename_items(plan), "the subtitles do not meet on one name");
	assert_equal(2_z, plan.empty() ? 0_z : plan.front().sidecars.size(), "both move with the video");

	if (!plan.empty() && plan.front().sidecars.size() == 2)
	{
		assert_equal("Film.en.srt"sv, plan.front().sidecars[0].second.name().sv(), "each keeps its language");
		assert_equal("Film.English.forced.srt"sv, plan.front().sidecars[1].second.name().sv(), "and that it is forced");
	}

	assert_equal("The.Film.en.srt"sv, moved_sidecar_path(video, root.combine_file("The.Film.mkv"), english).name().sv(),
	             "a new name with a dot keeps all of it");
	assert_equal("New.xmp"sv,
	             moved_sidecar_path(root.combine_file("IMG_1.CR2"), root.combine_file("New.CR2"),
	                                root.combine_file("Other.xmp"))
	                 .name()
	                 .sv(),
	             "a sidecar named for another file keeps only its extension");
	platform::delete_items({}, {root}, false);
}

// A RAW and the camera's JPEG share a sidecar by name, and both rows moved it: the second move failed
// its row, or with the JPEG first the RAW's metadata went to the JPEG's new name.
static void should_rename_a_shared_sidecar_with_the_raw_that_reads_it()
{
	const auto root = test_temps.folder().combine(std::format("rename-shared-sidecar-{}", core::tick_count()));
	platform::create_folder(root);
	const auto sidecar = root.combine_file("IMG_1.xmp");
	const core::blob contents = {1};
	platform::blob_save_to_file(contents, root.combine_file("IMG_1.CR2"));
	platform::blob_save_to_file(contents, root.combine_file("IMG_1.JPG"));
	platform::blob_save_to_file(contents, sidecar);

	rename_source raw;
	raw.source = root.combine_file("IMG_1.CR2");
	raw.original_name = "IMG_1";
	raw.sidecars.emplace_back(sidecar);
	auto jpeg = raw;
	jpeg.source = root.combine_file("IMG_1.JPG");

	for (const auto& order : {std::vector{raw, jpeg}, std::vector{jpeg, raw}})
	{
		const auto plan = calc_item_renames(order, "Trip ###", 1, collision_policy::block_run);
		assert_equal(2_z, plan.size(), "both files are planned");
		assert_equal(1,
		             static_cast<int>(
						 std::ranges::count_if(plan, [](const rename_item& r) { return !r.sidecars.empty(); })),
		             "the shared sidecar moves once");

		for (const auto& r : plan)
		{
			if (r.sidecars.empty()) continue;
			assert_equal(".CR2"s, std::string(r.source.extension()), "with the RAW, which reads it");
			assert_equal(std::string(r.destination.file_name_without_extension()) + ".xmp",
			             std::string(r.sidecars.front().second.name().sv()), "under the RAW's new name");
		}

		assert_equal(true, can_rename_items(plan), "and the pair is renamed together");
	}

	platform::delete_items({}, {root}, false);
}

static void should_plan_unique_convert_outputs()
{
	std::vector<convert_source> sources;
	sources.emplace_back(core::file_path("c:\\one\\photo.jpg"), sizei{}, core::cached{});
	sources.emplace_back(core::file_path("c:\\two\\photo.png"), sizei{}, core::cached{});

	const auto plan = plan_convert_outputs(core::folder_path("c:\\destination"), sources, ".webp",
	                                       collision_policy::block_run);
	assert_equal(2_z, plan.size(), "all conversion sources planned");
	assert_equal("photo.webp", plan[0].destination.name(), "first conversion keeps basename");
	assert_equal("photo (2).webp", plan[1].destination.name(), "duplicate conversion basename is suffixed");
	assert_equal(false, plan[0].destination == plan[1].destination, "conversion outputs are unique");
}

// "Limit output dimensions" is a ceiling. The review shows an image within it at its own size, and the
// run wrote it enlarged to the limit - over the original, with Replace and an in-place destination.
static void should_limit_only_converted_images_over_the_limit()
{
	assert_equal(0, convert_max_side({1000, 800}, 2000), "an image within the limit keeps its size");
	assert_equal(0, convert_max_side({2000, 1500}, 2000), "and one exactly at it");
	assert_equal(2000, convert_max_side({4000, 3000}, 2000), "a larger one is reduced");
	assert_equal(2000, convert_max_side({1000, 4000}, 2000), "whichever side is longer");
	assert_equal(2000, convert_max_side({}, 2000), "one whose size is not known yet takes the limit");
	assert_equal(0, convert_max_side({4000, 3000}, 0), "no limit, no resize");
}

// A destination that names another row's source is a file this run reads. Writing it destroyed an
// input the review listed as a source, and that row then converted whatever the earlier one had
// just written over it. Ordering matters: the clobbering row is planned first, so the source it
// lands on has not yet claimed its own destination.
static void should_not_plan_a_convert_output_over_a_source()
{
	std::vector<convert_source> sources;
	sources.emplace_back(core::file_path("c:\\a\\photo.png"), sizei{}, core::cached{});
	sources.emplace_back(core::file_path("c:\\destination\\photo.jpg"), sizei{}, core::cached{});

	const auto plan = plan_convert_outputs(core::folder_path("c:\\destination"), sources, ".jpg",
	                                       collision_policy::replace);
	assert_equal(2_z, plan.size(), "both conversion sources planned");

	for (const auto& entry : plan)
	{
		for (const auto& source : sources)
		{
			if (entry.source.path == source.path) continue;
			assert_equal(false, entry.destination == source.path, "a destination never names another row's source");
		}
	}

	assert_equal(sources[0].path.str(), plan[0].source.path.str(), "the row planned first is the one from elsewhere");
	assert_equal("photo (2).jpg", plan[0].destination.name(), "and it is moved off the other row's source");
	assert_equal("photo.jpg", plan[1].destination.name(), "a row may still convert itself in place");
}

// The folder box is planned as it is typed, so it is empty whenever it is cleared to type another
// folder, as an agent's task_set clears it. A name joined to no folder broke a Debug build's path
// check, and in a release named a file relative to wherever the program ran.
static void should_plan_no_convert_output_without_a_folder()
{
	std::vector<convert_source> sources;
	sources.emplace_back(core::file_path("c:\\one\\photo.jpg"), sizei{}, core::cached{});

	const auto plan = plan_convert_outputs(core::folder_path(), sources, ".webp", collision_policy::auto_rename);
	assert_equal(1_z, plan.size(), "the source is still listed");
	assert_equal(true, plan[0].destination.is_empty(), "with no destination until a folder is given");
	assert_equal(false, plan[0].collides, "which nothing collides with");
}

// A recipient sees a shared copy under its staged name, so two photos named alike in different
// folders must not land on one name, and a file the options leave alone goes as itself. Breaking
// the free-name step fails the "(2)" row; skipping the clear fails the stale row.
static void should_prepare_shared_copies_under_their_own_names()
{
	const auto root = test_temps.next_folder("share");
	const auto staging = root.combine("staging");
	const auto one = root.combine("one");
	const auto two = root.combine("two");
	platform::create_folder(staging);
	platform::create_folder(one);
	platform::create_folder(two);

	const auto png = test_files_folder().combine("excluded1").combine_file("gamma.png");
	const auto jpeg = root.combine_file("Progressive.jpg");
	const std::vector sources = {one.combine_file("photo.png"), two.combine_file("photo.png"), jpeg};
	platform::copy_file(png, sources[0], false, false);
	platform::copy_file(png, sources[1], false, false);
	platform::copy_file(test_files_folder().combine("excluded1").combine_file("Progressive.jpg"), jpeg, false, false);
	const auto source_bytes = platform::blob_from_file(sources[0]);

	const auto stale = staging.combine_file("stale.jpg");
	platform::blob_save_to_file(core::blob{1}, stale);

	null_item_results_ui status;
	outgoing_options convert;
	convert.convert_to_jpeg = true;

	// Whether a share left a file is asked of the folder's listing, which is what the clear reads. On a
	// network share, which `dd bean` stages on, the client answers an existence check from its cache
	// for some seconds after a delete, while the listing is current.
	const auto staged = [&staging](const core::file_path path)
	{
		const auto listing = platform::iterate_file_items(staging, true).files;
		return std::ranges::any_of(listing, [&path](const platform::file_info& f)
		{ return core::icmp(f.name.sv(), path.name()) == 0; });
	};

	const auto converted = prepare_outgoing_files(sources, convert, staging, status);
	assert_equal(true, converted.success, "the conversion is prepared");
	assert_equal(3_z, converted.paths.size(), "one outgoing file per source");
	assert_equal(staging.combine_file("photo.jpg").str(), converted.paths[0].str(), "the copy keeps its name");
	assert_equal(staging.combine_file("photo (2).jpg").str(), converted.paths[1].str(),
	             "a second photo of that name does not overwrite the first");
	assert_equal(jpeg.str(), converted.paths[2].str(), "a JPEG with nothing to convert goes as itself");
	assert_equal(false, staged(stale), "what the previous share staged is cleared");
	assert_equal(true, platform::blob_from_file(sources[0]) == source_bytes, "the source is unchanged");

	const auto copy_bytes = platform::blob_from_file(converted.paths[1]);
	assert_equal(true, copy_bytes.size() > 2 && copy_bytes[0] == 0xFF && copy_bytes[1] == 0xD8, "the copy is a JPEG");

	outgoing_options zip;
	zip.zip = true;
	const auto zipped = prepare_outgoing_files(sources, zip, staging, status);
	assert_equal(true, zipped.success, "the archive is prepared");
	assert_equal(1_z, zipped.paths.size(), "the files go as one archive");
	assert_equal("items.zip", zipped.paths.front().name(), "named for what it holds");
	const auto entries = df::zip_file::list(zipped.paths.front());
	assert_equal(3_z, entries.size(), "holding every source");
	assert_equal("photo.png", entries[0].filename, "an entry keeps its name");
	assert_equal("photo (2).png", entries[1].filename, "a second file of that name does not replace it on extraction");
	assert_equal(false, staged(converted.paths[0]), "the earlier share's copies are cleared");

	const auto unchanged = prepare_outgoing_files(sources, {}, staging, status);
	assert_equal(true, unchanged.success, "nothing to prepare still succeeds");
	assert_equal(sources.size(), unchanged.paths.size(), "every source goes");
	assert_equal(sources[1].str(), unchanged.paths[1].str(), "as itself");
	assert_equal(true, staged(zipped.paths.front()), "and nothing staged is cleared when nothing is written");

	// The fixture is 256 pixels square. A limit above that must not enlarge it, which is what the
	// scale edit does unless the size is checked first.
	outgoing_options limit;
	limit.max_side = 1024;
	const auto inside = prepare_outgoing_files({jpeg}, limit, staging, status);
	assert_equal(jpeg.str(), inside.paths.front().str(), "a photo already inside the limit goes as itself");

	limit.max_side = 128;
	const auto limited = prepare_outgoing_files({jpeg}, limit, staging, status);
	assert_equal(staging.combine_file("Progressive.jpg").str(), limited.paths.front().str(),
	             "a photo beyond the limit goes as a copy");
	files codecs;
	const auto limited_size =
		codecs.scan_file(limited.paths.front(), false, files::file_type_from_name(limited.paths.front())).dimensions();
	assert_equal(128, std::max(limited_size.cx, limited_size.cy), "its longest side is the limit");

	// Diffractor writes no HEIC, so one shrunk goes as a JPEG.
	const auto heic = root.combine_file("melnik.heic");
	platform::copy_file(test_files_folder().combine_file("melnik.heic"), heic, false, false);
	const auto shrunk = prepare_outgoing_files({heic}, limit, staging, status);
	assert_equal(true, shrunk.success, "a HEIC beyond the limit is prepared");
	assert_equal(staging.combine_file("melnik.jpg").str(), shrunk.paths.front().str(), "as a JPEG");
	const auto shrunk_size =
		codecs.scan_file(shrunk.paths.front(), false, files::file_type_from_name(shrunk.paths.front())).dimensions();
	assert_equal(128, std::max(shrunk_size.cx, shrunk_size.cy), "within the limit");
}

static void should_adjust_item_dates_from_snapshot()
{
	const core::date_t original_start(2025, 8, 16, 18, 11, 56);
	const core::date_t new_start(2025, 8, 17, 9, 30, 0);
	constexpr auto offset = 10 * core::date_t::intervals_per_second;
	assert_equal(new_start + offset, adjusted_item_date(original_start + offset, new_start, original_start),
	             "dated item preserves offset");
	assert_equal(new_start, adjusted_item_date({}, new_start, original_start), "undated item uses new start");

	// The row says which tag it is about to shift, so a selection whose files are dated by different
	// things is visible before the run rather than after it.
	const auto with_shutter = std::make_shared<prop::item_metadata>();
	with_shutter->dates.add(prop::date_source::exif_original, core::date_t(2025, 8, 16, 18, 11, 56));
	with_shutter->dates.add(prop::date_source::file_modified, core::date_t(2026, 8, 19, 21, 30, 0));
	assert_equal("EXIF DateTimeOriginal", adjust_date_source_name(with_shutter), "the shutter answers first");

	const auto scanned = std::make_shared<prop::item_metadata>();
	scanned->dates.add(prop::date_source::xmp_create, core::date_t(2019, 5, 4, 9, 0, 0));
	assert_equal("XMP xmp:CreateDate", adjust_date_source_name(scanned), "the rung below it answers next");

	// Mirrors calc_media_created, which falls through to the filesystem when nothing else answers.
	assert_equal("File created", adjust_date_source_name(nullptr), "an unscanned file shifts its own stamp");
	assert_equal("File created", adjust_date_source_name(std::make_shared<prop::item_metadata>()),
	             "so does a file carrying no date at all");
}

#ifdef _WIN32
// The numbers are MAPI's own result codes, so there is nothing to classify where MAPI is absent.
static void should_classify_mapi_results()
{
	assert_equal(true, platform::classify_mapi_send_result(0) == platform::mapi_send_result::sent, "MAPI success");
	assert_equal(true, platform::classify_mapi_send_result(1) == platform::mapi_send_result::canceled,
	             "MAPI user abort");
	assert_equal(true, platform::classify_mapi_send_result(3) == platform::mapi_send_result::failed,
	             "MAPI login failure");
}
#endif

// Records what a run reported for each row so revalidation decisions can be asserted.
struct recording_status final : df::status_i
{
	std::vector<std::pair<std::string, item_status>> items;
	// The first completion only, as the views show it: a later one is ignored.
	std::string completion;
	bool completed = false;
	bool canceled = false;

	void start_item(std::string_view) override {}

	void end_item(const std::string_view name, const item_status status) override { items.emplace_back(name, status); }

	bool has_failures() const override
	{
		return std::ranges::any_of(items, [](const auto& i) { return i.second == item_status::fail; });
	}

	void abort(std::string_view) override {}

	void complete(const std::string_view message) override
	{
		if (completed) return;
		completion = message;
		completed = true;
	}

	void show_errors() override {}

	void message(std::string_view, int64_t, int64_t) override {}

	void show_message(std::string_view) override {}

	bool is_canceled() const override { return canceled; }

	void wait_for_complete() const override {}

	item_status status_of(const std::string_view name) const
	{
		const auto found = std::ranges::find_if(items, [name](const auto& i) { return i.first == name; });
		return found == items.end() ? item_status::cancel : found->second;
	}
};

// Run holds every reviewed row to the file that was reviewed, one row at a time. A row whose file
// moved on is refused and reported; the rows that still match are still run.
static void should_revalidate_sync_rows()
{
	const auto root = test_temps.next_folder("sync-revalidate");
	const auto local = root.combine("local");
	const auto remote = root.combine("remote");
	platform::create_folder(local);
	platform::create_folder(remote);

	write_test_file(local.combine_file("keep.txt"), "keep");
	write_test_file(local.combine_file("changed.txt"), "changed");
	write_test_file(local.combine_file("claimed.txt"), "claimed");

	core::index_roots roots;
	roots.folders.emplace(local);
	const auto analysis = sync_analysis(roots, remote, true, false, false, false, test_token);
	assert_equal(true, analysis.valid, "sync analysis is valid");
	assert_equal(3, static_cast<int>(count_sync_actions(analysis)), "three files to copy out");

	// The source grows after review, so copying it would send content nobody approved.
	write_test_file(local.combine_file("changed.txt"), "changed again");
	// A destination the review found free is claimed by something else before the run reaches it.
	write_test_file(remote.combine_file("claimed.txt"), "not yours");

	const auto status = std::make_shared<recording_status>();
	const auto run = sync_copy(status, analysis, test_token);

	assert_equal(true, status->status_of("keep.txt") == item_status::success, "unchanged row runs");
	assert_equal(true, status->status_of("changed.txt") == item_status::fail, "changed source is refused");
	assert_equal(true, status->status_of("claimed.txt") == item_status::fail, "claimed destination is refused");
	assert_equal(2, static_cast<int>(run.refused), "both refusals are reported so the run can say why");

	assert_equal(true, platform::exists(remote.combine_file("keep.txt")), "unchanged row was copied");
	assert_equal(false, platform::exists(remote.combine_file("changed.txt")), "refused row wrote nothing");
	assert_equal("not yours"s, read_test_file(remote.combine_file("claimed.txt")),
	             "claimed destination was not overwritten");
}

// A delete was reviewed against the file's content, so that is what it is held to at run time.
static void should_revalidate_sync_deletes()
{
	const auto root = test_temps.next_folder("sync-delete-revalidate");
	const auto local = root.combine("local");
	const auto remote = root.combine("remote");
	platform::create_folder(local);
	platform::create_folder(remote);

	write_test_file(remote.combine_file("stale.txt"), "stale");
	write_test_file(remote.combine_file("touched.txt"), "touched");

	core::index_roots roots;
	roots.folders.emplace(local);
	const auto analysis = sync_analysis(roots, remote, false, false, false, true, test_token);
	assert_equal(true, analysis.valid, "sync analysis is valid");
	assert_equal(2, static_cast<int>(count_sync_actions(analysis, sync_action::delete_remote)),
	             "two remote files to delete");

	write_test_file(remote.combine_file("touched.txt"), "touched after review");

	const auto status = std::make_shared<recording_status>();
	sync_copy(status, analysis, test_token);

	assert_equal(true, status->status_of("stale.txt") == item_status::success, "unchanged file is deleted");
	assert_equal(true, status->status_of("touched.txt") == item_status::fail, "changed file is not deleted");
	assert_equal(false, platform::exists(remote.combine_file("stale.txt")), "unchanged file is gone");
	assert_equal(true, platform::exists(remote.combine_file("touched.txt")), "changed file survives");
}

// A full-path exclusion is expressed in local collection coordinates. Sync's remote tree has to
// project each relative remote folder back into that same local scope before deciding whether it is
// in or out; otherwise reverse sync can copy excluded files back and mirror delete can remove them.
static void should_apply_sync_full_path_exclusions_to_remote_counterparts()
{
	const auto root = test_temps.next_folder("sync-full-path-exclude");
	const auto local = root.combine("local");
	const auto remote = root.combine("remote");
	const auto remote_private = remote.combine("Private");
	const auto remote_nested = remote_private.combine("Nested");
	const auto remote_cache = remote.combine("Cache.tmp");
	platform::create_folder(local);
	platform::create_folder(remote_nested);
	platform::create_folder(remote_cache);

	write_test_file(remote_private.combine_file("hidden.txt"), "hidden");
	write_test_file(remote_nested.combine_file("nested.txt"), "nested");
	write_test_file(remote_cache.combine_file("wildcard.txt"), "wildcard");
	write_test_file(remote.combine_file("ordinary.txt"), "ordinary");

	core::index_roots roots;
	roots.folders.emplace(local);
	roots.excludes.emplace(local.combine("Private"));
	roots.exclude_wildcards.emplace(core::cache("*.tmp"));

	const auto reverse_copy = sync_analysis(roots, remote, false, true, false, false, test_token);
	assert_equal(true, reverse_copy.valid, "reverse sync remains valid");
	assert_equal(1, static_cast<int>(count_sync_actions(reverse_copy, sync_action::copy_local)),
	             "only the ordinary sibling is copied back");

	const auto remote_delete = sync_analysis(roots, remote, false, false, false, true, test_token);
	assert_equal(true, remote_delete.valid, "remote delete sync remains valid");
	assert_equal(1, static_cast<int>(count_sync_actions(remote_delete, sync_action::delete_remote)),
	             "only the ordinary sibling is proposed for remote deletion");
}

static void should_resolve_sync_exclusions_across_multiple_roots()
{
	const auto root = test_temps.next_folder("sync-multi-root-exclude");
	const auto local1 = root.combine("local1");
	const auto local2 = root.combine("local2");
	const auto remote = root.combine("remote");
	platform::create_folder(local1.combine("Shots"));
	platform::create_folder(local2.combine("Shots"));
	platform::create_folder(remote.combine("Shots"));
	write_test_file(local2.combine("Shots").combine_file("photo.txt"), "local2");
	write_test_file(remote.combine("Shots").combine_file("photo.txt"), "remote");

	core::index_roots roots;
	roots.folders.emplace(local1);
	roots.folders.emplace(local2);
	roots.excludes.emplace(local1.combine("Shots"));

	const auto disagreeing = sync_analysis(roots, remote, true, true, true, true, test_token);
	assert_equal(false, disagreeing.valid, "excluded and included roots disagreeing is ambiguous");
	assert_equal(true, disagreeing.reason == sync_invalid_reason::ambiguous_local_root,
	             "the disagreement keeps the ambiguous-root refusal");

	roots.excludes.emplace(local2.combine("Shots"));
	const auto all_excluded = sync_analysis(roots, remote, true, true, true, true, test_token);
	assert_equal(true, all_excluded.valid, "matching exclusions are not ambiguous");
	assert_equal(0, static_cast<int>(count_sync_actions(all_excluded)), "the excluded remote folder is ignored");
}

static void should_detect_duplicate_import_destinations()
{
	const auto src1 = test_temps.folder().combine("import-dup-1");
	const auto src2 = test_temps.folder().combine("import-dup-2");
	const auto dest = test_temps.folder().combine("import-dup-dest");
	platform::create_folder(src1);
	platform::create_folder(src2);
	platform::create_folder(dest);

	const auto source = core::file_path(test_files_folder(), "Test.jpg");
	platform::copy_file(source, src1.combine_file("same.jpg"), false, false);
	platform::copy_file(source, src2.combine_file("same.jpg"), false, false);

	const auto make_item = [](const core::folder_path folder)
	{
		const auto path = folder.combine_file("same.jpg");
		const auto fi = platform::file_attributes(path);
		folder_scan_item item;
		item.folder = folder;
		item.item.name = path.name();
		item.item.file_modified = core::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.ft = files::file_type_from_name(path);
		return item;
	};

	const std::vector<folder_scan_item> items{make_item(src1), make_item(src2)};
	const item_import_set no_previous;

	import_options options;
	options.dest_folder = dest;
	options.dest_structure = std::string(default_custom_folder_structure);
	options.collision = collision_policy::block_run;

	const auto blocked = import_analysis(items, options, no_previous, test_token);
	assert_equal(1, static_cast<int>(count_import_collisions(blocked)),
	             "second source claiming the same destination is a collision");
	assert_equal(1, static_cast<int>(count_imports(blocked)), "only the first source imports");

	options.collision = collision_policy::auto_rename;
	const auto renamed = import_analysis(items, options, no_previous, test_token);
	core::unique_paths destinations;

	for (const auto& folder : renamed)
	{
		for (const auto& i : folder.second) destinations.emplace(i.destination);
	}

	assert_equal(2, static_cast<int>(count_imports(renamed)), "auto-rename imports both sources");
	assert_equal(2, static_cast<int>(destinations.size()), "auto-rename gives each source its own destination");
}

// A destination folder that cannot be created stops the run, but everything already imported into
// earlier folders still has to be recorded and rescanned. Returning from the middle of the walk lost
// both: a later run imported those files again as duplicates, and the index never saw them arrive.
static void should_keep_import_bookkeeping_when_a_folder_fails(shared_test_context& stc)
{
	const auto root = test_temps.next_folder("import-folder-fail");
	const auto src = root.combine("source");
	const auto dest_ok = root.combine("a-ok");
	const auto dest_blocked = root.combine("b-blocked");
	platform::create_folder(src);

	write_test_file(src.combine_file("first.txt"), "first");
	write_test_file(src.combine_file("second.txt"), "second");

	// A file where the second destination folder needs to be, so creating it cannot succeed.
	write_test_file(core::file_path(root, "b-blocked"), "in the way");

	const auto make_row = [&src](const std::string_view name, const core::folder_path destination)
	{
		const auto path = src.combine_file(name);
		const auto fi = platform::file_attributes(path);

		import_analysis_item row;
		row.source = path;
		row.destination = destination.combine_file(name);
		row.action = import_action::import;
		row.source_fi = fi;
		row.import_rec.name = path.name();
		return row;
	};

	import_analysis_result analysis;
	analysis[dest_ok].emplace_back(make_row("first.txt", dest_ok));
	analysis[dest_blocked].emplace_back(make_row("second.txt", dest_blocked));

	import_options options;
	options.dest_folder = root;

	const auto status = std::make_shared<recording_status>();
	const auto run = import_copy(stc.empty_index, status, analysis, options, test_token);

	assert_equal(true, platform::exists(dest_ok.combine_file("first.txt")), "the first folder was imported into");
	assert_equal(true, status->status_of("first.txt") == item_status::success, "and its row says so");

	// The whole point: the run stopped, but what it wrote is still accounted for.
	assert_equal(1u, static_cast<uint32_t>(run.imports.size()), "the file that did arrive is recorded as imported");
	assert_equal(true, run.folder == dest_ok, "and the run names the folder it reached");
}

// A photo's date is its camera's clock, local time, and the file system keeps UTC: written unconverted, Explorer
// showed the imported file created hours from when the photo was taken, by the zone's offset that day.
static void should_set_imported_file_created_to_when_it_was_taken(shared_test_context& stc)
{
	const auto root = test_temps.next_folder("import-created");
	const auto src = root.combine("source");
	const auto dest = root.combine("dest");
	platform::create_folder(src);
	write_test_file(src.combine_file("photo.txt"), "photo");

	const auto path = src.combine_file("photo.txt");
	import_analysis_item row;
	row.source = path;
	row.destination = dest.combine_file("photo.txt");
	row.action = import_action::import;
	row.source_fi = platform::file_attributes(path);
	row.import_rec.name = path.name();
	// Summer, where a zone with daylight saving is furthest from UTC.
	row.created_date = core::date_t(2007, 9, 15, 7, 20, 2);

	import_analysis_result analysis;
	analysis[dest].emplace_back(row);

	import_options options;
	options.dest_folder = root;
	options.set_created_date = true;

	const auto status = std::make_shared<recording_status>();
	const auto run = import_copy(stc.empty_index, status, analysis, options, test_token);

#ifdef _WIN32
	assert_equal(0u, run.dates_unset, "the created date was set");
	assert_equal(platform::local_to_utc(row.created_date).to_int64(),
	             core::date_t(platform::file_attributes(row.destination).created).to_int64(),
	             "the file was created when the photo was taken, in local time");
#else
	// Linux keeps no creation time a program can set, so the run counts the date it was asked for and could not write.
	assert_equal(1u, run.dates_unset, "a created date the file system refuses is counted, not reported as set");
	assert_equal(true, platform::exists(row.destination), "while the file itself arrives");
#endif
}

// Import holds each reviewed row to the file that was reviewed, and lets the file system prove a
// destination is free rather than asking and then writing.
static void should_revalidate_import_rows(shared_test_context& stc)
{
	const auto root = test_temps.next_folder("import-revalidate");
	const auto src = root.combine("source");
	const auto dest = root.combine("dest");
	platform::create_folder(src);
	platform::create_folder(dest);

	write_test_file(src.combine_file("keep.txt"), "keep");
	write_test_file(src.combine_file("changed.txt"), "changed");
	write_test_file(src.combine_file("claimed.txt"), "claimed");

	const auto make_item = [&src](const std::string_view name)
	{
		const auto path = src.combine_file(name);
		const auto fi = platform::file_attributes(path);
		folder_scan_item item;
		item.folder = src;
		item.item.name = path.name();
		item.item.file_modified = core::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.size = core::file_size(fi.size);
		item.item.ft = files::file_type_from_name(path);
		return item;
	};

	const std::vector<folder_scan_item> items{make_item("keep.txt"), make_item("changed.txt"),
	                                          make_item("claimed.txt")};

	import_options options;
	options.dest_folder = dest;
	options.dest_structure = {};
	options.collision = collision_policy::skip;

	const auto analysis = import_analysis(items, options, {}, test_token);
	assert_equal(3, static_cast<int>(count_imports(analysis)), "three files to import");

	write_test_file(src.combine_file("changed.txt"), "changed after review");
	write_test_file(dest.combine_file("claimed.txt"), "not yours");

	const auto status = std::make_shared<recording_status>();
	const auto run = import_copy(stc.empty_index, status, analysis, options, test_token);

	assert_equal(true, status->status_of("keep.txt") == item_status::success, "unchanged row imports");
	assert_equal(true, status->status_of("changed.txt") == item_status::fail, "changed source is refused");
	assert_equal(true, status->status_of("claimed.txt") == item_status::fail, "claimed destination is refused");
	assert_equal(2, static_cast<int>(run.refused), "both refusals are reported so the run can say why");
	// And it does say why, in the completion the view shows - which is the first one reported, so
	// an explanation added by the caller after the copy had completed never reached the user.
	assert_equal(true, core::contains(status->completion, tt.sync_analysis_changed.sv()),
	             "the run explains that files changed since the review");

	assert_equal(true, platform::exists(dest.combine_file("keep.txt")), "unchanged row was imported");
	assert_equal(false, platform::exists(dest.combine_file("changed.txt")), "refused row wrote nothing");
	assert_equal("not yours"s, read_test_file(dest.combine_file("claimed.txt")),
	             "claimed destination was not overwritten");
}

// A sidecar is part of the group the user reviewed, not a detail of it. The primary was held to the
// file that was reviewed and its sidecars were not, so a sidecar edited since the review travelled
// with an approved file as content nobody approved.
static void should_revalidate_import_sidecars(shared_test_context& stc)
{
	const auto root = test_temps.next_folder("import-sidecar-revalidate");
	const auto src = root.combine("source");
	const auto dest = root.combine("dest");
	platform::create_folder(src);
	platform::create_folder(dest);

	const auto make_item = [&src](const std::string_view name)
	{
		const auto path = src.combine_file(name);
		const auto sidecar = path.extension(".xmp");
		write_test_file(path, "media");
		write_test_file(sidecar, "sidecar");

		const auto fi = platform::file_attributes(path);
		folder_scan_item item;
		item.folder = src;
		item.item.name = path.name();
		item.item.file_modified = core::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.size = core::file_size(fi.size);
		item.item.ft = files::file_type_from_name(path);

		const auto metadata = std::make_shared<prop::item_metadata>();
		metadata->sidecars = sidecar.name();
		item.item.metadata.store(metadata);
		return item;
	};

	const std::vector<folder_scan_item> items{make_item("keep.txt"), make_item("racing.txt")};

	import_options options;
	options.dest_folder = dest;
	options.dest_structure = {};
	options.collision = collision_policy::skip;

	const auto analysis = import_analysis(items, options, {}, test_token);
	assert_equal(2, static_cast<int>(count_imports(analysis)), "both groups are planned");

	// Only the sidecar changes. The file it describes is exactly what was reviewed, so nothing the
	// primary is held to can see this.
	write_test_file(src.combine_file("racing.xmp"), "edited since review");

	const auto status = std::make_shared<recording_status>();
	const auto run = import_copy(stc.empty_index, status, analysis, options, test_token);

	assert_equal(true, status->status_of("keep.txt") == item_status::success, "an unchanged group imports");
	assert_equal(true, platform::exists(dest.combine_file("keep.xmp")), "and brings its sidecar");

	assert_equal(true, status->status_of("racing.txt") == item_status::fail,
	             "a group whose sidecar changed is refused");
	assert_equal(1, static_cast<int>(run.refused), "and is reported as a refusal so the run can say why");
	assert_equal(false, platform::exists(dest.combine_file("racing.txt")), "the refused group wrote nothing");
	assert_equal(false, platform::exists(dest.combine_file("racing.xmp")), "not even the sidecar that changed");
}

// The RAW and the JPEG of one shot share an XMP by base name, so both rows name it. The group travels
// once: a second row carrying the sidecar again collided with the copy the first had just written.
// Under Replace that refused the JPEG outright, and under Skip the JPEG was blocked by its own twin.
static void should_import_a_shared_sidecar_once(shared_test_context& stc)
{
	for (const auto& [policy, move] : {std::pair{collision_policy::replace, false},
	                                   {collision_policy::replace, true},
	                                   {collision_policy::skip, false},
	                                   {collision_policy::skip, true},
	                                   {collision_policy::auto_rename, false},
	                                   {collision_policy::auto_rename, true}})
	{
		const auto root = test_temps.next_folder("import-shared-sidecar");
		const auto src = root.combine("source");
		const auto dest = root.combine("dest");
		platform::create_folder(src);
		platform::create_folder(dest);

		const auto sidecar = src.combine_file("shot.xmp");
		write_test_file(sidecar, "shared sidecar");
		write_test_file(src.combine_file("shot.dop"), "shared adjustments");

		const auto make_item = [&src, &sidecar](const std::string_view name)
		{
			const auto path = src.combine_file(name);
			write_test_file(path, name);

			const auto fi = platform::file_attributes(path);
			folder_scan_item item;
			item.folder = src;
			item.item.name = path.name();
			item.item.file_modified = core::date_t(fi.modified);
			item.item.file_created = fi.created;
			item.item.size = core::file_size(fi.size);
			item.item.ft = files::file_type_from_name(path);

			const auto metadata = std::make_shared<prop::item_metadata>();
			metadata->sidecars = core::cache("shot.xmp;shot.dop");
			item.item.metadata.store(metadata);
			return item;
		};

		const std::vector<folder_scan_item> items{make_item("shot.cr2"), make_item("shot.jpg")};

		import_options options;
		options.dest_folder = dest;
		options.dest_structure = {};
		options.collision = policy;
		options.is_move = move;

		const auto analysis = import_analysis(items, options, {}, test_token);
		assert_equal(2, static_cast<int>(count_imports(analysis)), "both files of the shot are planned");

		const auto status = std::make_shared<recording_status>();
		import_copy(stc.empty_index, status, analysis, options, test_token);

		assert_equal(true, status->status_of("shot.cr2") == item_status::success, "the raw imports");
		assert_equal(true, status->status_of("shot.jpg") == item_status::success, "and so does the jpeg");
		assert_equal("shared sidecar"s, read_test_file(dest.combine_file("shot.xmp")),
		             "the sidecar they share arrives once");
		assert_equal(!move, platform::exists(sidecar), "Move releases the shared source after both primary writes");
		assert_equal("shared adjustments"s, read_test_file(dest.combine_file("shot.dop")),
		             "each dependency is published once per row, not once per new companion");
		assert_equal(!move, platform::exists(src.combine_file("shot.dop")),
		             "the second shared source has the same lifetime");
	}
}

static folder_scan_item import_test_item(const core::file_path path, const std::string_view sidecars = {})
{
	const auto fi = platform::file_attributes(path);
	folder_scan_item item;
	item.folder = path.folder();
	item.item.name = path.name();
	item.item.file_modified = core::date_t(fi.modified);
	item.item.file_created = fi.created;
	item.item.size = core::file_size(fi.size);
	item.item.ft = files::file_type_from_name(path);
	if (!sidecars.empty())
	{
		const auto metadata = std::make_shared<prop::item_metadata>();
		metadata->sidecars = core::cache(sidecars);
		item.item.metadata.store(metadata);
	}
	return item;
}

// docs/faces.md#62-marks: a picture an import moves takes the marks the user put on its faces with it; one it copies
// leaves them with the original.
static void should_take_face_marks_with_a_picture_an_import_moves(shared_test_context& stc)
{
	for (const auto move : {false, true})
	{
		const auto root = test_temps.next_folder("import-face-marks");
		const auto src = root.combine("source");
		const auto dest = root.combine("dest");
		platform::create_folder(src);
		platform::create_folder(dest);
		const auto source = src.combine_file("photo.jpg");
		const auto destination = dest.combine_file("photo.jpg");
		write_test_file(source, "media");

		auto& book = stc.empty_index.faces().book();
		const std::array batch{faces::placement_batch{0, {}, {{source, rectd{0.1, 0.1, 0.2, 0.2}, 0}}}};
		book.place(batch, faces::face_action::hide, book.generation());

		import_options options;
		options.dest_folder = dest;
		options.dest_structure = {};
		options.is_move = move;
		const auto analysis = import_analysis(std::vector{import_test_item(source)}, options, {}, test_token);
		import_copy(stc.empty_index, std::make_shared<recording_status>(), analysis, options, test_token);

		assert_equal(true, platform::exists(destination), "the picture is imported");
		assert_equal(move ? 0 : 1, static_cast<int>(book.marks(source).size()),
		             move ? "moved, its marks leave the source" : "copied, the original keeps its marks");
		assert_equal(move ? 1 : 0, static_cast<int>(book.marks(destination).size()),
		             move ? "and arrive with it" : "and the copy starts without");
	}
}

static void should_preserve_shared_import_sidecars_after_auto_rename(shared_test_context& stc)
{
	for (const auto move : {false, true})
	{
		const auto root = test_temps.next_folder("import-shared-renamed");
		const auto src = root.combine("source");
		const auto dest = root.combine("dest");
		platform::create_folder(src);
		platform::create_folder(dest);
		write_test_file(src.combine_file("shot.jpg"), "jpeg");
		write_test_file(src.combine_file("shot.nef"), "raw");
		write_test_file(src.combine_file("shot.xmp"), "shared metadata");
		write_test_file(dest.combine_file("shot.nef"), "existing raw");
		write_test_file(dest.combine_file("shot (2).nef"), "another raw");

		const std::vector items{import_test_item(src.combine_file("shot.jpg"), "shot.xmp"),
		                        import_test_item(src.combine_file("shot.nef"), "shot.xmp"),
		                        import_test_item(src.combine_file("shot.xmp"))};
		import_options options;
		options.dest_folder = dest;
		options.is_move = move;
		options.collision = collision_policy::auto_rename;
		const auto analysis = import_analysis(items, options, {}, test_token);
		const auto status = std::make_shared<recording_status>();
		const auto imported = import_copy(stc.empty_index, status, analysis, options, test_token);

		assert_equal("raw"s, read_test_file(dest.combine_file("shot (3).nef")), "the RAW follows its reviewed rename");
		assert_equal("shared metadata"s, read_test_file(dest.combine_file("shot (3).xmp")),
		             "the renamed RAW receives its matching metadata");
		assert_equal("jpeg"s, read_test_file(dest.combine_file("shot.jpg")), "the JPEG keeps its free name");
		assert_equal("shared metadata"s, read_test_file(dest.combine_file("shot.xmp")),
		             "both destinations receive the shared metadata");
		assert_equal("existing raw"s, read_test_file(dest.combine_file("shot.nef")), "the first collision survives");
		assert_equal("another raw"s, read_test_file(dest.combine_file("shot (2).nef")),
		             "the second collision survives");
		assert_equal(false, platform::exists(dest.combine_file("shot (2).xmp")),
		             "no standalone companion is auto-renamed");
		assert_equal(2_z, count_imports(analysis), "automatic companions do not become independent rows");
		assert_equal(2_z, imported.imports.size(), "both primary imports are recorded");
		assert_equal(true, status->status_of("shot.jpg") == item_status::success, "the JPEG reports success");
		assert_equal(true, status->status_of("shot.nef") == item_status::success, "the RAW reports success");
		for (const auto name : {"shot.jpg", "shot.nef", "shot.xmp"})
			assert_equal(!move, platform::exists(src.combine_file(name)), "only a completed Move removes its sources");
	}
}

static void should_keep_shared_import_sidecars_for_unfinished_sources(shared_test_context& stc)
{
	for (const auto outcome : {item_status::ignore, item_status::fail, item_status::cancel})
	{
		const auto root = test_temps.next_folder("import-shared-unfinished");
		const auto src = root.combine("source");
		const auto dest = root.combine("dest");
		platform::create_folder(src);
		platform::create_folder(dest);
		write_test_file(src.combine_file("shot.jpg"), "jpeg");
		write_test_file(src.combine_file("shot.nef"), "raw");
		write_test_file(src.combine_file("shot.xmp"), "shared metadata");
		if (outcome == item_status::ignore) write_test_file(dest.combine_file("shot.nef"), "existing raw");

		const std::vector items{import_test_item(src.combine_file("shot.jpg"), "shot.xmp"),
		                        import_test_item(src.combine_file("shot.nef"), "shot.xmp"),
		                        import_test_item(src.combine_file("shot.xmp"))};
		import_options options;
		options.dest_folder = dest;
		options.is_move = true;
		options.collision = collision_policy::skip;
		const auto analysis = import_analysis(items, options, {}, test_token);
		const auto status = std::make_shared<recording_status>();
		auto first_write = true;
		test_import_sidecar_write_override = [outcome, dest, status,
		                                      &first_write](core::file_path) -> std::optional<platform::file_op_result>
		{
			if (first_write)
			{
				first_write = false;
				if (outcome == item_status::fail) write_test_file(dest.combine_file("shot.jpg"), "new occupant");
				if (outcome == item_status::cancel) status->canceled = true;
			}
			return {};
		};
		const core::scope_exit restore_hook([] { test_import_sidecar_write_override = {}; });
		import_copy(stc.empty_index, status, analysis, options, test_token);

		assert_equal("shared metadata"s, read_test_file(src.combine_file("shot.xmp")),
		             "the unfinished source retains its authored sidecar");
		assert_equal("shared metadata"s, read_test_file(dest.combine_file("shot.xmp")),
		             "the completed source also has its sidecar, even if an earlier row rolled back");
		if (outcome == item_status::fail)
		{
			assert_equal(true, status->status_of("shot.jpg") == item_status::fail, "the raced JPEG is refused");
			assert_equal(true, status->status_of("shot.nef") == item_status::success, "the RAW still arrives whole");
			assert_equal("jpeg"s, read_test_file(src.combine_file("shot.jpg")), "the refused primary stays");
		}
		else
		{
			assert_equal(true, status->status_of("shot.jpg") == item_status::success, "the JPEG completed");
			assert_equal(true, status->status_of("shot.nef") == outcome, "the RAW has the expected unfinished result");
			assert_equal("raw"s, read_test_file(src.combine_file("shot.nef")), "the unfinished primary stays");
		}
	}
}

// Replace is the one policy that writes over a file instead of proving the name is free, so it is
// the one that has to prove the file is still the one that was reviewed.
static void should_revalidate_replaced_import_destinations(shared_test_context& stc)
{
	const auto root = test_temps.next_folder("import-replace-revalidate");
	const auto src = root.combine("source");
	const auto dest = root.combine("dest");
	platform::create_folder(src);
	platform::create_folder(dest);

	write_test_file(src.combine_file("stable.txt"), "new stable");
	write_test_file(src.combine_file("racing.txt"), "new racing");
	write_test_file(dest.combine_file("stable.txt"), "old stable");
	write_test_file(dest.combine_file("racing.txt"), "old racing");

	const auto make_item = [&src](const std::string_view name)
	{
		const auto path = src.combine_file(name);
		const auto fi = platform::file_attributes(path);
		folder_scan_item item;
		item.folder = src;
		item.item.name = path.name();
		item.item.file_modified = core::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.size = core::file_size(fi.size);
		item.item.ft = files::file_type_from_name(path);
		return item;
	};

	const std::vector<folder_scan_item> items{make_item("stable.txt"), make_item("racing.txt")};

	import_options options;
	options.dest_folder = dest;
	options.dest_structure = {};
	options.collision = collision_policy::replace;

	const auto analysis = import_analysis(items, options, {}, test_token);
	assert_equal(2, static_cast<int>(count_imports(analysis)), "replace imports over both destinations");

	write_test_file(dest.combine_file("racing.txt"), "edited since review");

	const auto status = std::make_shared<recording_status>();
	import_copy(stc.empty_index, status, analysis, options, test_token);

	assert_equal(true, status->status_of("stable.txt") == item_status::success, "reviewed destination is replaced");
	assert_equal(true, status->status_of("racing.txt") == item_status::fail, "changed destination is refused");

	assert_equal("new stable"s, read_test_file(dest.combine_file("stable.txt")),
	             "reviewed destination took the new file");
	assert_equal("edited since review"s, read_test_file(dest.combine_file("racing.txt")),
	             "changed destination was left alone");
}

#ifdef _WIN32
static void should_restore_replaced_import_sidecars_when_primary_write_fails(shared_test_context& stc)
{
	for (const auto is_move : {false, true})
	{
		const auto root = test_temps.next_folder(is_move ? "import-move-sidecar-rollback"
		                                                 : "import-copy-sidecar-rollback");
		const auto src = root.combine("source");
		const auto dest = root.combine("dest");
		platform::create_folder(src);
		platform::create_folder(dest);

		const auto source = src.combine_file("photo.jpg");
		const auto source_sidecar = src.combine_file("photo.xmp");
		const auto destination = dest.combine_file("photo.jpg");
		const auto destination_sidecar = dest.combine_file("photo.xmp");
		write_test_file(source, "new media");
		write_test_file(source_sidecar, "new xmp");
		write_test_file(destination, "old media");
		write_test_file(destination_sidecar, "old xmp");

		const auto fi = platform::file_attributes(source);
		folder_scan_item item;
		item.folder = src;
		item.item.name = source.name();
		item.item.file_modified = core::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.size = core::file_size(fi.size);
		item.item.ft = files::file_type_from_name(source);

		const auto metadata = std::make_shared<prop::item_metadata>();
		metadata->sidecars = source_sidecar.name();
		item.item.metadata.store(metadata);

		import_options options;
		options.dest_folder = dest;
		options.collision = collision_policy::replace;
		options.is_move = is_move;

		const auto analysis = import_analysis(std::vector{item}, options, {}, test_token);
		auto locked = platform::open_file(destination, platform::file_open_mode::read_write);
		assert_equal(true, locked != nullptr, "the destination media is held open");

		const auto status = std::make_shared<recording_status>();
		import_copy(stc.empty_index, status, analysis, options, test_token);
		locked.reset();

		assert_equal(true, status->status_of("photo.jpg") == item_status::fail, "the row reports failure");
		assert_equal("old media"s, read_test_file(destination), "the destination media keeps its old bytes");
		assert_equal("old xmp"s, read_test_file(destination_sidecar), "the destination sidecar is restored");
		assert_equal("new media"s, read_test_file(source), "the source media remains recoverable");
		assert_equal("new xmp"s, read_test_file(source_sidecar), "the source sidecar remains with its media");
	}
}
#endif

static void should_delete_free_import_sidecar_copy_when_primary_write_fails(shared_test_context& stc)
{
	const auto root = test_temps.next_folder("import-copy-free-sidecar-undo");
	const auto src = root.combine("source");
	const auto dest = root.combine("dest");
	platform::create_folder(src);
	platform::create_folder(dest);

	const auto source = src.combine_file("photo.jpg");
	const auto source_sidecar = src.combine_file("photo.xmp");
	const auto destination = dest.combine_file("photo.jpg");
	const auto destination_sidecar = dest.combine_file("photo.xmp");
	write_test_file(source, "new media");
	write_test_file(source_sidecar, "source xmp");
	write_test_file(destination, "old media");

	std::filesystem::permissions(platform::to_stream_path(source_sidecar), std::filesystem::perms::owner_write,
	                             std::filesystem::perm_options::remove);
	const core::scope_exit restore_readonly([source_sidecar] { platform::make_file_writable(source_sidecar); });
	auto locked_source_sidecar = platform::open_file(source_sidecar, platform::file_open_mode::read);
	assert_equal(true, locked_source_sidecar != nullptr, "the source sidecar is held open");

	const auto fi = platform::file_attributes(source);
	folder_scan_item item;
	item.folder = src;
	item.item.name = source.name();
	item.item.file_modified = core::date_t(fi.modified);
	item.item.file_created = fi.created;
	item.item.size = core::file_size(fi.size);
	item.item.ft = files::file_type_from_name(source);

	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = source_sidecar.name();
	item.item.metadata.store(metadata);

	import_options options;
	options.dest_folder = dest;
	options.collision = collision_policy::replace;

	const auto analysis = import_analysis(std::vector{item}, options, {}, test_token);
#ifdef _WIN32
	auto locked = platform::open_file(destination, platform::file_open_mode::read_write);
	assert_equal(true, locked != nullptr, "the destination media is held open");
#else
	std::filesystem::permissions(platform::to_stream_path(destination), std::filesystem::perms::owner_write,
	                             std::filesystem::perm_options::remove);
	const core::scope_exit restore_destination_writable([destination] { platform::make_file_writable(destination); });
#endif

	const auto status = std::make_shared<recording_status>();
	import_copy(stc.empty_index, status, analysis, options, test_token);
#ifdef _WIN32
	locked.reset();
#endif
	locked_source_sidecar.reset();

	assert_equal(true, status->status_of("photo.jpg") == item_status::fail, "the row reports the primary failure");
	assert_equal("source xmp"s, read_test_file(source_sidecar), "the source sidecar remains byte-identical");
	assert_equal(false, platform::exists(destination_sidecar), "the copied destination sidecar is removed");
}

static void should_delete_readonly_import_sidecar_rollback_copy(shared_test_context& stc)
{
	const auto root = test_temps.next_folder("import-rollback-cleanup-failure");
	const auto src = root.combine("source");
	const auto dest = root.combine("dest");
	platform::create_folder(src);
	platform::create_folder(dest);

	const auto source = src.combine_file("photo.jpg");
	const auto source_sidecar = src.combine_file("photo.xmp");
	const auto destination = dest.combine_file("photo.jpg");
	const auto destination_sidecar = dest.combine_file("photo.xmp");
	write_test_file(source, "new media");
	write_test_file(source_sidecar, "new xmp");
	write_test_file(destination, "old media");
	write_test_file(destination_sidecar, "old xmp");

	const auto fi = platform::file_attributes(source);
	folder_scan_item item;
	item.folder = src;
	item.item.name = source.name();
	item.item.file_modified = core::date_t(fi.modified);
	item.item.file_created = fi.created;
	item.item.size = core::file_size(fi.size);
	item.item.ft = files::file_type_from_name(source);

	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = source_sidecar.name();
	item.item.metadata.store(metadata);

	import_options options;
	options.dest_folder = dest;
	options.collision = collision_policy::replace;

	platform::file_ptr rollback_lock;
	test_before_import_rollback_cleanup = [&rollback_lock](const core::file_path path)
	{
		rollback_lock = platform::open_file(path, platform::file_open_mode::read_write);
	};
	const core::scope_exit restore_cleanup_hook([&rollback_lock]
	{
		rollback_lock.reset();
		test_before_import_rollback_cleanup = {};
	});

	const auto analysis = import_analysis(std::vector{item}, options, {}, test_token);
	const auto status = std::make_shared<recording_status>();
	import_copy(stc.empty_index, status, analysis, options, test_token);

	assert_equal(true, status->status_of("photo.jpg") == item_status::success,
	             "cleanup failure does not change the import result");
	assert_equal("new media"s, read_test_file(destination), "the destination media was imported");
	assert_equal("new xmp"s, read_test_file(destination_sidecar), "the destination sidecar was imported");
}

static void should_restore_destination_sidecar_after_partial_overwrite_failure(shared_test_context& stc)
{
	const auto root = test_temps.next_folder("import-sidecar-partial-restore");
	const auto src = root.combine("source");
	const auto dest = root.combine("dest");
	platform::create_folder(src);
	platform::create_folder(dest);

	const auto source = src.combine_file("photo.jpg");
	const auto source_sidecar = src.combine_file("photo.xmp");
	const auto destination = dest.combine_file("photo.jpg");
	const auto destination_sidecar = dest.combine_file("photo.xmp");
	write_test_file(source, "new media");
	write_test_file(source_sidecar, "new xmp");
	write_test_file(destination, "old media");
	write_test_file(destination_sidecar, "old xmp");

	const auto fi = platform::file_attributes(source);
	folder_scan_item item;
	item.folder = src;
	item.item.name = source.name();
	item.item.file_modified = core::date_t(fi.modified);
	item.item.file_created = fi.created;
	item.item.size = core::file_size(fi.size);
	item.item.ft = files::file_type_from_name(source);

	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = source_sidecar.name();
	item.item.metadata.store(metadata);

	import_options options;
	options.dest_folder = dest;
	options.collision = collision_policy::replace;

	test_import_sidecar_write_override =
		[destination_sidecar](const core::file_path path) -> std::optional<platform::file_op_result>
	{
		if (path != destination_sidecar) return {};
		write_test_file(destination_sidecar, "partial xmp");
		return platform::file_op_result{platform::file_op_result_code::FAILED, "simulated sidecar overwrite failure"};
	};
	const core::scope_exit restore_hook([] { test_import_sidecar_write_override = {}; });

	const auto analysis = import_analysis(std::vector{item}, options, {}, test_token);
	const auto status = std::make_shared<recording_status>();
	import_copy(stc.empty_index, status, analysis, options, test_token);

	assert_equal(true, status->status_of("photo.jpg") == item_status::fail, "the row reports failure");
	assert_equal("old media"s, read_test_file(destination), "the destination media is unchanged");
	assert_equal("old xmp"s, read_test_file(destination_sidecar), "the destination sidecar is restored");
}

static void should_prepare_import_from_the_selection_snapshot()
{
	null_state_strategy ss;
	null_async_strategy as;
	view_host_base_ptr view;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	state.view_mode(view_type::items);
	state.open(view, df::search_t().add_selector(test_files_folder()), {});
	state.update_item_groups();
	state.update_selection();

	const auto first = state.find_displayed_item_by_name("Test.jpg");
	const auto second = state.find_displayed_item_by_name("Test90.jpg");
	assert_equal(true, first != nullptr, "first source exists");
	assert_equal(true, second != nullptr, "second source exists");

	state.select(view, first, false, false, false);
	state.update_selection();
	auto selected_source = calc_import_selected_source(state);
	assert_equal(true, selected_source.has_value(), "the selected source is captured");

	state.select(view, second, false, false, false);
	state.update_selection();
	const auto sources = calc_import_sources(std::move(selected_source));

	assert_equal(true, !sources.empty(), "import sources are prepared");
	assert_equal(1_z, sources.front().items.size(), "the captured source has one item");
	assert_equal(first->path().pack(), sources.front().items.items().front()->path().pack(),
	             "the prepared source is the selection captured before discovery");
}

static void should_offer_every_removable_drive_with_media_for_import()
{
	const auto drive = [](const std::string_view name, const platform::drive_type type, const uint64_t capacity)
	{
		platform::drive_t d;
		d.type = type;
		d.name = name;
		d.capacity = core::file_size(capacity);
		return d;
	};

	constexpr uint64_t card = 64'000'000'000ull;
	constexpr auto removable = platform::drive_type::removable;
	constexpr auto fixed = platform::drive_type::fixed;

	// Two fixed drives and a four-slot card reader with one card, in its last slot.
	const platform::drives drives = {
		drive("C:\\", fixed, card),  drive("D:\\", fixed, card),  drive("E:\\", removable, 0),
		drive("F:\\", removable, 0), drive("G:\\", removable, 0), drive("H:\\", removable, card),
	};

	const auto offered = import_source_drives(drives, 5);
	assert_equal(1_z, offered.size(), "only the slot holding a card is offered");
	assert_equal("H:\\"sv, offered.front().name, "a card past the fifth drive letter is still offered");

	platform::drives many;
	for (const auto letter : "IJKLMNO"sv) many.emplace_back(drive(std::format("{}:\\", letter), removable, card));
	const auto limited = import_source_drives(many, 5);
	assert_equal(5_z, limited.size(), "the list is still bounded");
	assert_equal("I:\\"sv, limited.front().name, "in drive order");
}

// design.md#guided-operations: a phone's files are staged under the folders it keeps them in, with their names and
// times, as a name the file system takes. What an import already took stays on the phone, and its staged copy goes;
// what an earlier Analyze staged is not copied again; a file the phone will not give up is named and passed over, the
// rest staged regardless; and what the phone no longer holds goes from staging.
static void should_stage_a_devices_files_to_import()
{
	const auto staging = test_temps.next_folder("device-staging");
	const core::file_path picture(test_files_folder(), "Test.jpg");
	const auto size = core::file_size(platform::file_attributes(picture).size);
	constexpr uint64_t taken = 133'000'000'000'000'000ull;
	const auto camera = std::format("Internal storage{}DCIM{}Camera", core::preferred_path_sep,
	                                core::preferred_path_sep);
	const auto odd = std::format("Internal storage{}Pictures{}Odd:Name", core::preferred_path_sep,
	                             core::preferred_path_sep);

	const std::vector<platform::portable_file_t> files = {
		{"first", camera, "PXL_1.jpg", size, taken, taken},
		{"second", camera, "PXL_2.jpg", size, taken + 10'000'000, taken + 10'000'000},
		{"cloud", camera, "PXL_3.jpg", size, taken + 30'000'000, taken + 30'000'000},
		{"odd", odd, "a?b.jpg", size, taken + 20'000'000, taken + 20'000'000},
	};

	std::vector<std::string> asked;
	const device_copier copier = [&](const std::vector<platform::portable_copy_t>& copies)
	{
		platform::portable_copy_result result;
		for (size_t i = 0; i < copies.size(); ++i)
		{
			const auto& c = copies[i];
			asked.emplace_back(c.id);

			// Kept only in the cloud: the phone lists it but will not give it up.
			if (c.id == "cloud")
			{
				result.unreadable.emplace_back(i, "not on the phone");
				continue;
			}

			(void)platform::create_folder(c.destination.folder());
			(void)platform::copy_file(picture, c.destination, true, true);
			(void)platform::set_file_attributes(c.destination, c.modified);
			result.copied.emplace_back(c.destination);
		}
		return result;
	};

	const auto in_camera = staging.combine("Internal storage").combine("DCIM").combine("Camera");
	item_import_set imported;
	imported.emplace(item_import{core::cache("PXL_2.jpg"), core::date_t(taken + 10'000'000), size, {}});

	const auto first = stage_device_files(files, imported, staging, copier);
	assert_equal(2_z, first.staged, "what no import took is staged");
	assert_equal(1_z, first.previously_imported, "and what one did stays on the phone");
	assert_equal(true, asked == std::vector<std::string>{"first", "cloud", "odd"}, "only those are asked for");
	assert_equal(true, first.unreadable == std::vector<std::string>{"PXL_3.jpg"},
	             "one the phone would not give up is named, and the rest staged regardless");
	assert_equal(true, first.stopped.success(), "which stops nothing");
	assert_equal(taken, platform::file_attributes(in_camera.combine_file("PXL_1.jpg")).modified,
	             "under the folders the phone keeps it in, with its time");
	assert_equal(
		true,
		platform::exists(
			staging.combine("Internal storage").combine("Pictures").combine("Odd_Name").combine_file("a_b.jpg")),
		"a name the file system refuses is made one it takes");

	asked.clear();
	const auto again = stage_device_files(files, imported, staging, copier);
	assert_equal(2_z, again.staged, "an Analyze again stages the same");
	assert_equal(true, asked == std::vector<std::string>{"cloud"}, "asking again only for what it did not have");

	imported.emplace(item_import{core::cache("PXL_1.jpg"), core::date_t(taken), size, {}});
	const auto after_import = stage_device_files(files, imported, staging, copier);
	assert_equal(1_z, after_import.staged, "once imported, a file is no longer staged");
	assert_equal(false, platform::exists(in_camera.combine_file("PXL_1.jpg")), "and its staged copy goes");

	const auto odd_staged =
		staging.combine("Internal storage").combine("Pictures").combine("Odd_Name").combine_file("a_b.jpg");
	const auto still_held = stage_device_files({files[2], files[3]}, imported, staging, copier);
	assert_equal(1_z, still_held.staged, "what the phone still holds stays staged");
	assert_equal(true, platform::exists(odd_staged), "where it was");
	(void)stage_device_files({files[2]}, imported, staging, copier);
	assert_equal(false, platform::exists(odd_staged), "and what it no longer holds goes");
}

// design.md#guided-operations: opening Import clears what this instance and instances that have ended left staged,
// and leaves what a running instance holds alone.
static void should_clear_only_abandoned_device_staging()
{
	const auto root = test_temps.next_folder("device-staging-root");
	const auto own = root.combine(core::to_string(platform::process_id()));
	const auto running = root.combine("4000000001");
	const auto ended = root.combine("4000000002");
	const auto stray = root.combine("Pixel 7");

	for (const auto& folder : {own, running, ended, stray})
	{
		(void)platform::create_folder(folder);
		(void)platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), folder.combine_file("staged.jpg"),
		                          true, true);
	}

	const auto held = platform::try_lock_instance(device_staging_lock_name(4000000001));
	assert_equal(true, held.lock != nullptr, "a running instance holds its lock");

	clear_abandoned_device_staging(root);
	assert_equal(true, platform::exists(running.combine_file("staged.jpg")), "and keeps what it staged");
	assert_equal(false, platform::exists(ended), "an ended instance's staging is cleared");
	assert_equal(false, platform::exists(own), "as is this one's, which holds nothing as Import opens");
	assert_equal(false, platform::exists(stray), "and anything else there");
}

static void should_reject_missing_sync_folder()
{
	core::index_roots roots;
	roots.folders.emplace(test_temps.folder().combine("missing-local"));

	const auto result = sync_analysis(roots, test_temps.folder(), true, false, false, true, test_token);
	assert_equal(false, result.valid, "missing local folder invalidates sync analysis");
	assert_equal(true, result.empty(), "invalid sync analysis has no actions");
	assert_equal(true, result.reason != sync_invalid_reason::none, "records why the analysis is invalid");
	assert_equal(false, sync_invalid_message(result) == tt.platform_text.error_cannot_continue.sv(),
	             "a missing folder is not reported as an internal fault");
}

static void should_reject_overlapping_sync_folders()
{
	const auto local = test_temps.folder().combine("sync-overlap-local");
	platform::create_folder(local);
	core::index_roots roots;
	roots.folders.emplace(local);

	const auto result = sync_analysis(roots, local.combine("remote"), true, false, false, false, test_token);
	assert_equal(false, result.valid, "nested remote folder invalidates sync analysis");
	assert_equal(true, result.reason == sync_invalid_reason::overlapping_paths, "reports overlapping folders");
	assert_equal(false, sync_invalid_message(result) == tt.platform_text.error_cannot_continue.sv(),
	             "overlapping folders are not reported as an internal fault");

	const auto accented_local = test_temps.folder().combine("sync-overlap-é");
	const auto accented_remote = test_temps.folder().combine("sync-overlap-è").combine("remote");
	platform::create_folder(accented_local);
	platform::create_folder(accented_remote.parent());
	platform::create_folder(accented_remote);
	core::index_roots accented_roots;
	accented_roots.folders.emplace(accented_local);

	const auto accented_result = sync_analysis(accented_roots, accented_remote, true, false, false, false, test_token);
	assert_equal(true, accented_result.valid, "accented sibling sync roots do not overlap");
}

static void should_reject_ambiguous_sync_roots()
{
	const auto local1 = test_temps.folder().combine("sync-root-1");
	const auto local2 = test_temps.folder().combine("sync-root-2");
	const auto remote = test_temps.folder().combine("sync-remote");
	platform::create_folder(local1);
	platform::create_folder(local2);
	platform::create_folder(remote);
	const auto source = core::file_path(test_files_folder(), "Test.jpg");
	platform::copy_file(source, local1.combine_file("same.jpg"), false, false);
	platform::copy_file(source, local2.combine_file("same.jpg"), false, false);

	core::index_roots roots;
	roots.folders.emplace(local1);
	roots.folders.emplace(local2);
	const auto result = sync_analysis(roots, remote, true, false, false, false, test_token);
	assert_equal(false, result.valid, "duplicate relative paths across roots invalidate sync analysis");
	assert_equal(true, result.empty(), "ambiguous sync analysis has no actions");
	assert_equal(true, result.reason == sync_invalid_reason::ambiguous_local_root, "reports ambiguous roots");
	assert_equal(false, sync_invalid_message(result) == tt.platform_text.error_cannot_continue.sv(),
	             "ambiguous roots are not reported as an internal fault");
}

static void should_ignore_unclaimed_remote_files()
{
	// A collection with more than one root cannot name a local destination for a remote-only file in
	// an unknown folder. That must only fail the run when the file would actually be copied local.
	const auto local1 = test_temps.folder().combine("unclaimed-root-1");
	const auto local2 = test_temps.folder().combine("unclaimed-root-2");
	const auto remote = test_temps.folder().combine("unclaimed-remote");
	platform::create_folder(local1);
	platform::create_folder(local2);
	platform::create_folder(remote);
	const auto source = core::file_path(test_files_folder(), "Test.jpg");
	platform::copy_file(source, remote.combine_file("remote-only.jpg"), false, false);

	core::index_roots roots;
	roots.folders.emplace(local1);
	roots.folders.emplace(local2);

	const auto ignored = sync_analysis(roots, remote, true, false, false, false, test_token);
	assert_equal(true, ignored.valid, "remote-only file does not invalidate a multi-root sync analysis");
	assert_equal(1, static_cast<int>(ignored.size()), "the remote-only file is still reported");

	for (const auto& folder : ignored)
	{
		for (const auto& file : folder.second)
		{
			assert_equal(true, file.second.action == sync_action::none, "the remote-only file is ignored");
		}
	}

	const auto copied = sync_analysis(roots, remote, false, true, false, false, test_token);
	assert_equal(false, copied.valid, "copying a remote-only file needs an unambiguous local root");
	assert_equal(true, copied.reason == sync_invalid_reason::ambiguous_local_root, "reports ambiguous roots");
}

static void should_select_sync_actions()
{
	// Modified times are FILETIME ticks; a second is ten million of them.
	constexpr uint64_t s = 10'000'000ull;

	assert_equal(true,
	             calc_sync_action(true, false, 10 * s, 0, 100, 0, false, false, true, false) ==
	                 sync_action::delete_local,
	             "delete local is independent");
	assert_equal(true,
	             calc_sync_action(false, true, 0, 10 * s, 0, 100, false, false, false, true) ==
	                 sync_action::delete_remote,
	             "delete remote is independent");
	assert_equal(true,
	             calc_sync_action(true, false, 10 * s, 0, 100, 0, true, false, true, false) == sync_action::copy_remote,
	             "copy local to remote takes precedence over delete local");
	assert_equal(true,
	             calc_sync_action(false, true, 0, 10 * s, 0, 100, false, true, false, true) == sync_action::copy_local,
	             "copy remote to local takes precedence over delete remote");
	assert_equal(true,
	             calc_sync_action(true, true, 20 * s, 10 * s, 100, 100, true, true, true, true) ==
	                 sync_action::copy_remote,
	             "newer local file copies to remote");
	assert_equal(true,
	             calc_sync_action(true, true, 10 * s, 20 * s, 100, 100, true, true, true, true) ==
	                 sync_action::copy_local,
	             "newer remote file copies to local");
	assert_equal(true,
	             calc_sync_action(true, true, 10 * s, 10 * s, 100, 100, true, true, true, true) == sync_action::none,
	             "matching timestamps need no action");
	assert_equal(true,
	             calc_sync_action(true, true, 10 * s, 10 * s, 101, 100, true, false, false, false) ==
	                 sync_action::copy_remote,
	             "one-way sync copies unequal sizes with matching timestamps");
	assert_equal(true,
	             calc_sync_action(true, true, 10 * s, 10 * s, 100, 101, false, true, false, false) ==
	                 sync_action::copy_local,
	             "reverse one-way sync copies unequal sizes with matching timestamps");

	// FAT32 rounds a copy's modified time to two seconds and exFAT to ten milliseconds, in either direction.
	assert_equal(true,
	             calc_sync_action(true, true, 10 * s + s / 2, 10 * s, 100, 100, true, true, false, false) ==
	                 sync_action::none,
	             "a copy whose time the drive rounded down is not copied again");
	assert_equal(true,
	             calc_sync_action(true, true, 10 * s, 10 * s + 2 * s, 100, 100, true, true, false, false) ==
	                 sync_action::none,
	             "a copy whose time the drive rounded up by two seconds is not copied back");
	assert_equal(true,
	             calc_sync_action(true, true, 13 * s, 10 * s, 100, 100, true, true, false, false) ==
	                 sync_action::copy_remote,
	             "three seconds newer is a change");
	assert_equal(true,
	             calc_sync_action(true, true, 10 * s + s, 10 * s, 101, 100, true, false, false, false) ==
	                 sync_action::copy_remote,
	             "a size change within the rounding still copies one way");
	assert_equal(true,
	             calc_sync_action(true, true, 10 * s + s, 10 * s, 101, 100, true, true, false, false) ==
	                 sync_action::none,
	             "a size change within the rounding has no direction when both are enabled");
}

static void should_show_the_latest_running_work_on_the_taskbar()
{
	work_progress_reports reports;
	const int view = 0;
	const int dialog = 0;

	assert_equal(false, reports.latest().has_value(), "nothing running shows nothing");
	assert_equal(false, reports.finish(&view), "ending work that never ran changes nothing");

	reports.update(&view, 2, 10);
	reports.update(&dialog, 0, 0);
	assert_equal(true, reports.latest()->source == &dialog, "the button follows whichever reported last");

	reports.update(&view, 3, 10);
	assert_equal(true, reports.latest()->source == &view, "and goes back to the view when it reports again");
	assert_equal(3, static_cast<int>(reports.latest()->done), "with its latest position");

	assert_equal(true, reports.finish(&view), "the view's run ends");
	assert_equal(true, reports.latest()->source == &dialog, "the dialog still running takes the button back");
	assert_equal(false, reports.finish(&view), "a view reporting its end again changes nothing");

	assert_equal(true, reports.finish(&dialog), "the dialog ends");
	assert_equal(false, reports.latest().has_value(), "nothing left running clears the button");
}

static void should_offer_every_matching_tool()
{
	const auto make_tool = [](const std::string_view exe, const std::string_view group)
	{
		auto tool = std::make_shared<file_tool>();
		tool->exe = core::cache(exe);
		tool->text = core::cache(exe);
		tool->group = core::cache(group);
		tool->invoke_text = core::cache("{exe-path} {item-path}");
		return tool;
	};

	const auto first = make_tool("first", "photo");
	const auto second = make_tool("second", "photo");
	const auto player = make_tool("player", "video");

	file_tools_result result;
	result.by_extension[core::cache("jpg")] = {first, second};
	result.by_group[core::cache("photo")] = {second};
	result.by_group[core::cache("video")] = {player};
	apply_tools(std::move(result));
	// The tool table is process-global; a failing assertion below must not leave it replaced.
	const core::scope_exit restore_tools([] { apply_tools({}); });

	const auto jpeg = files::file_type_from_name("test.jpg");
	const auto tools = jpeg->all_tools();
	assert_equal(2_z, tools.size(), "every tool declaring the extension is offered");
	assert_equal_strict("first", tools[0]->text.sv(), "config order is preserved");
	assert_equal_strict("second", tools[1]->text.sv(), "a tool matched by extension and group is listed once");

	const auto mp4 = files::file_type_from_name("test.mp4");
	assert_equal(1_z, mp4->all_tools().size(), "group membership offers a tool with no extension match");

	assert_equal(false, first->invoke(core::file_path{}), "an unresolved executable is never launched");

	apply_tools({});
	assert_equal(0_z, jpeg->all_tools().size(), "reapplying replaces rather than accumulates");
}

static void should_offer_configured_tools_only_for_one_selected_file()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto path1 = test_temps.next_path(".jpg");
	const auto path2 = test_temps.next_path(".jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), path1, false, false);
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), path2, false, false);

	const auto first = load_item(index, path1, false);
	const auto second = load_item(index, path2, false);

	df::item_set single;
	single.add(first);
	assert_equal(path1.pack(), selected_external_tool_target(single).pack(),
	             "a singular selected file is the configured-tool target");

	df::item_set multiple;
	multiple.add(first);
	multiple.add(second);
	assert_equal(true, selected_external_tool_target(multiple).is_empty(),
	             "configured tools are absent for a multi-file selection");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #175 - Sidebar history chart doesn't show the whole collection
// The date histogram must record files older than 10 years so the (now
// user-configurable) chart can display the full collection span.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_record_history_beyond_ten_years()
{
	const auto this_year = core::utc_now().year();
	const location_cache locations;
	index_histograms h;

	const auto record_created = [&](const int years_ago, const int month)
	{
		df::index_file_item f;
		f.ft = files::file_type_from_name("test.jpg");
		f.file_created = core::date_t(this_year - years_ago, month, 1);
		f.file_modified = core::date_t(this_year - years_ago, month, 1);
		h.record(locations, f);
	};

	record_created(0, 3);  // this year, March
	record_created(9, 6);  // within the old 10-year window
	record_created(15, 6); // BEYOND the old 10-year cap - previously dropped
	record_created(40, 1); // decades back, still within max_history_years

	assert_equal(1, h._dates.dates[0 * 12 + (3 - 1)].created, "this year recorded");
	assert_equal(1, h._dates.dates[9 * 12 + (6 - 1)].created, "9-year-old recorded");
	assert_equal(1, h._dates.dates[15 * 12 + (6 - 1)].created, "15-year-old recorded (beyond old cap)");
	assert_equal(1, h._dates.dates[40 * 12 + (1 - 1)].created, "40-year-old recorded");

	// The storage capacity must exceed the old hard-coded 10-year limit.
	assert_equal(true, df::max_history_years > 10, "history capacity beyond 10 years");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The timeline is a navigation surface: clicking a month runs a search. A chart that buckets an
// item under one date while the search answers with another is "click to open finds nothing" at the
// place a user is most likely to click. Both halves resolve through prop::item_metadata::created(),
// and only the round trip proves they still agree - neither half can be checked alone.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_answer_a_timeline_month_with_its_own_items(shared_test_context& stc)
{
	stc.lazy_load_index();
	stc.test_index.update_summary();

	const auto histograms = stc.test_index.histograms();
	const auto year = histograms->_year;

	auto months_checked = 0;

	for (auto index = 0u; index < histograms->_dates.dates.size(); ++index)
	{
		if (histograms->_dates.dates[index].created == 0) continue;

		// The cell draws this item, so this is the item a user is clicking through to.
		const auto shown = histograms->_dates.representative_paths[index];
		if (shown.is_empty()) continue;

		const auto cell_year = year - static_cast<int>(index) / 12;
		const auto cell_month = static_cast<int>(index) % 12 + 1;

		// Day zero is the whole month, which is what the chart cell stands for. The chart buckets on
		// the capture-first ladder, so the click has to ask for that key rather than the Created one.
		const auto search = df::search_t().day(0, cell_month, cell_year, df::date_parts_prop::original);

		auto found = false;
		auto listed = 0;
		auto cb = [&](const index_state::query_item_results& items, bool)
		{
			listed += static_cast<int>(items.size());
			for (const auto& i : items) found = found || i.path == shown;
		};
		stc.test_index.query_items(search, cb, test_token);

		assert_equal(true, listed > 0, std::format("{}-{:02} lists something", cell_year, cell_month));
		assert_equal(true, found,
		             std::format("{}-{:02} lists the item its cell draws - {}", cell_year, cell_month, shown.name()));
		++months_checked;
	}

	assert_equal(true, months_checked > 0, "the fixture collection populates at least one month");
}

static void should_calculate_history_span_from_start_year()
{
	constexpr auto this_year = 2026;

	const auto typed = df::history_range_from_start_year(2010, this_year);
	assert_equal(2010, typed.first_year, "a typed start year is taken as given");
	assert_equal(this_year, typed.last_year, "and the range still ends today");

	// A start year inside the window would leave the calendar with rows the navigator cannot reach.
	const auto recent = df::history_range_from_start_year(this_year, this_year);
	assert_equal(df::history_window_years, recent.year_count(), "a start year too recent still leaves a window");

	const auto ancient = df::history_range_from_start_year(1800, this_year);
	assert_equal(df::max_history_years, ancient.year_count(), "an old start year clamps to what is stored");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The navigator's range is worked out from what was indexed, and photographs carry wrong dates. A
// scanner that stamped 1900 on four negatives must not set the scale for twenty thousand photos,
// and a camera whose clock reset must not add a decade nobody photographed.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_choose_a_history_range_that_survives_wrong_dates()
{
	constexpr auto this_year = 2026;

	const auto with = [](const std::vector<std::pair<int, int>>& years_and_counts)
	{
		df::date_histogram h;

		for (const auto& [years_ago, count] : years_and_counts)
		{
			h.dates[static_cast<size_t>(years_ago) * 12 + 5].created += count;
		}

		return df::history_auto_range(h, this_year);
	};

	{
		const auto empty = df::history_auto_range({}, this_year);
		assert_equal(this_year, empty.last_year, "an empty collection still ends today");
		assert_equal(df::history_window_years, empty.year_count(), "and offers exactly one window");
	}

	{
		// Fifteen real years, and four negatives a scanner stamped 1900.
		std::vector<std::pair<int, int>> years;
		years.reserve(16);
		for (auto y = 0; y < 15; ++y) years.emplace_back(y, 800);
		years.emplace_back(99, 4);

		const auto range = with(years);
		assert_equal(this_year - 14, range.first_year, "the range stops at the real history");
		assert_equal(this_year, range.last_year, "and ends today");
	}

	{
		// A camera whose clock reset: an island of a few dozen behind a decade of silence.
		std::vector<std::pair<int, int>> years;
		years.reserve(12);
		for (auto y = 0; y < 10; ++y) years.emplace_back(y, 1000);
		years.emplace_back(40, 30);
		years.emplace_back(41, 30);

		const auto range = with(years);
		assert_equal(this_year - 9, range.first_year, "an island behind a long gap is a date bug");
	}

	{
		// Scanned family photographs are a real decade, however long ago: they are too much of the
		// collection to be a clock that reset, and cutting them would hide them from the navigator.
		std::vector<std::pair<int, int>> years;
		years.reserve(20);
		for (auto y = 0; y < 10; ++y) years.emplace_back(y, 1000);
		for (auto y = 40; y < 50; ++y) years.emplace_back(y, 400);

		const auto range = with(years);
		assert_equal(this_year - 49, range.first_year, "a substantial older decade is kept");
	}

	{
		// One busy year is still eight years of navigator, or there is nothing to navigate.
		const auto range = with({{0, 500}});
		assert_equal(df::history_window_years, range.year_count(), "the range is never shorter than a window");
	}

	{
		const auto range = with({{0, 1}, {20, 1}});
		assert_equal(this_year - 20, range.first_year, "an older photo is half of this collection, not an outlier");
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The media-type filter (photo/video/audio) survives a save/restore cycle. The
// filter is serialized to a stable
// comma-separated string of group names and rebuilt from it on load.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_persist_media_filter()
{
	filter_t filter;
	filter.toggle(file_group::photo);
	filter.toggle(file_group::video);

	// Round-trip through the same serialization used to persist the setting.
	const auto restored = media_filter_from_string(media_filter_to_string(filter));

	assert_equal(true, restored.has_group(file_group::photo), "photo filter restored");
	assert_equal(true, restored.has_group(file_group::video), "video filter restored");
	assert_equal(false, restored.has_group(file_group::audio), "audio filter stays off");

	// An empty filter round-trips to an empty filter (no groups selected).
	const auto empty = media_filter_from_string(media_filter_to_string(filter_t{}));
	assert_equal(true, empty.is_empty(), "empty filter round-trips to empty");

	// Unknown group names are ignored rather than producing bogus groups.
	const auto bogus = media_filter_from_string("photo,not_a_group");
	assert_equal(true, bogus.has_group(file_group::photo), "known group parsed");
	assert_equal(1_z, bogus.groups().size(), "unknown group ignored");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #228 - the tag view's action field appeared blank.
// The field is the action to apply ("what to add or remove"), not a view of the current tags,
// so it starts empty by design. What it must get right is the round-trip: a '-' prefix means
// remove, a tag containing white space is quoted, a repeated tag keeps only the last modifier,
// and matching is case-insensitive so "Beach" and "beach" are one action.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_round_trip_tag_actions()
{
	// The field is an action list, so no text means no actions - not "all current tags".
	assert_equal(0_z, parse_tag_actions({}).size(), "an empty field is no actions");
	assert_equal(0_z, parse_tag_actions("   ").size(), "white space alone is no actions");
	assert_equal_strict("", serialize_tag_actions({}), "no actions serializes to an empty field");

	const auto actions = parse_tag_actions("beach -winter \"new york\" -\"old town\"");
	assert_equal(4_z, actions.size(), "action count");

	assert_equal_strict("beach", actions[0].first, "first tag");
	assert_equal(true, actions[0].second, "unprefixed tag is an add");
	assert_equal_strict("winter", actions[1].first, "second tag");
	assert_equal(false, actions[1].second, "'-' prefixed tag is a remove");
	assert_equal_strict("new york", actions[2].first, "quoted tag keeps its space");
	assert_equal(true, actions[2].second, "quoted tag is an add");
	assert_equal_strict("old town", actions[3].first, "quoted removal keeps its space");
	assert_equal(false, actions[3].second, "quoted '-' prefixed tag is a remove");

	// Serializing re-quotes and re-prefixes, so the field survives an edit/apply/edit cycle.
	const auto text = serialize_tag_actions(actions);
	const auto reparsed = parse_tag_actions(text);
	assert_equal(actions.size(), reparsed.size(), "round-trip action count");

	for (auto i = 0_z; i < actions.size(); ++i)
	{
		assert_equal_strict(actions[i].first, reparsed[i].first, "round-trip tag");
		assert_equal(actions[i].second, reparsed[i].second, "round-trip modifier");
	}

	// A tag typed twice is one action, matched case-insensitively, and the last modifier wins.
	const auto deduped = parse_tag_actions("Beach -beach");
	assert_equal(1_z, deduped.size(), "a repeated tag is one action");
	assert_equal_strict("Beach", deduped[0].first, "the first spelling is kept");
	assert_equal(false, deduped[0].second, "the last modifier wins");

	const auto readded = parse_tag_actions("-beach Beach");
	assert_equal(1_z, readded.size(), "a repeated tag is one action either way round");
	assert_equal(true, readded[0].second, "the last modifier wins the other way round");

	const auto assert_two_tags =
		[](const std::string_view text, const std::string_view first, const std::string_view second)
	{
		const auto parsed = parse_tag_actions(text);
		assert_equal(2_z, parsed.size(), text);
		assert_equal_strict(first, parsed[0].first, text);
		assert_equal_strict(second, parsed[1].first, text);
	};

	assert_two_tags("beach, sunset", "beach", "sunset");
	assert_two_tags("beach,sunset", "beach", "sunset");
	assert_two_tags("\"New York\", beach", "New York", "beach");
	assert_two_tags("beach\r\nsunset", "beach", "sunset");

	const auto hash_tag = parse_tag_actions("#beach");
	assert_equal(1_z, hash_tag.size(), "hash tag count");
	assert_equal_strict("beach", hash_tag[0].first, "leading hash is syntax");

	const auto quoted_hash = parse_tag_actions("#\"New York\" -#\"old town\" #'New Town' \"#1\"");
	assert_equal(4_z, quoted_hash.size(), "quoted hash action count");
	assert_equal_strict("New York", quoted_hash[0].first, "quoted double hash tag");
	assert_equal(true, quoted_hash[0].second, "quoted double hash add");
	assert_equal_strict("old town", quoted_hash[1].first, "quoted hash removal tag");
	assert_equal(false, quoted_hash[1].second, "quoted hash removal");
	assert_equal_strict("New Town", quoted_hash[2].first, "quoted single hash tag");
	assert_equal(true, quoted_hash[2].second, "quoted single hash add");
	assert_equal_strict("#1", quoted_hash[3].first, "quoted literal hash is kept");
	assert_equal(true, quoted_hash[3].second, "quoted literal hash add");

	const std::vector<tag_action> literals = {
		{"or", true},
		{"and", true},
		{"-literal", true},
		{"name,with;delimiters|and(parens)", false},
		{"line\r\nbreak", true},
		{"tab\tname", true},
		{"quoted \"tag\"", true},
	};
	const auto literal_text = serialize_tag_actions(literals);
	const auto literal_reparsed = parse_tag_actions(literal_text);
	assert_equal(literals.size(), literal_reparsed.size(), "literal action count");

	for (auto i = 0_z; i < literals.size(); ++i)
	{
		assert_equal_strict(literals[i].first, literal_reparsed[i].first, "literal tag");
		assert_equal(literals[i].second, literal_reparsed[i].second, "literal modifier");
	}

	assert_equal_strict("or", parse_tag_actions(serialize_tag_actions({{"or", true}})).front().first,
	                    "reserved word survives a recommendation toggle");
	assert_equal_strict("-literal", parse_tag_actions(serialize_tag_actions({{"-literal", true}})).front().first,
	                    "leading marker survives a recommendation toggle");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #229 - switching a group between thumbnails and details did not stick: any search that
// recreated the groups reverted them. The choice is held per media type as a bitmask over
// group_key_type in setting.detail_items, which is what both the group toggle and the group
// construction read, and what the settings store persists.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_persist_detail_display_per_media_type()
{
	const auto saved = setting.detail_items;
	const core::scope_exit restore([saved] { setting.detail_items = saved; });

	setting.detail_items = 0;

	setting.set_detail_display(group_key_type::photo, true);
	assert_equal(true, setting.is_detail_display(group_key_type::photo), "photo set to detail");

	// Each media type is an independent bit: setting one must not change the others.
	assert_equal(false, setting.is_detail_display(group_key_type::video), "video unaffected");
	assert_equal(false, setting.is_detail_display(group_key_type::audio), "audio unaffected");
	assert_equal(false, setting.is_detail_display(group_key_type::folder), "folder unaffected");

	setting.set_detail_display(group_key_type::video, true);
	setting.set_detail_display(group_key_type::photo, false);
	assert_equal(false, setting.is_detail_display(group_key_type::photo), "photo back to thumbnails");
	assert_equal(true, setting.is_detail_display(group_key_type::video), "video still detail");

	// Clearing a type that was never set must not disturb the ones that were.
	setting.set_detail_display(group_key_type::audio, false);
	assert_equal(true, setting.is_detail_display(group_key_type::video), "video survives an unrelated clear");

	// Every media type has its own bit, so no two types can alias each other.
	constexpr group_key_type all[] = {
		// clang-format off
		group_key_type::folder, group_key_type::photo, group_key_type::video, group_key_type::audio,
		group_key_type::grouped_value, group_key_type::grouped_no_value, group_key_type::archive,
		group_key_type::retro, group_key_type::other,
		// clang-format on
	};

	for (const auto type : all)
	{
		setting.detail_items = 0;
		setting.set_detail_display(type, true);
		assert_equal(true, setting.is_detail_display(type), "media type set to detail");

		auto others_unset = true;

		for (const auto other : all)
		{
			if (other != type && setting.is_detail_display(other)) others_unset = false;
		}

		assert_equal(true, others_unset, "no other media type shares this bit");
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #135 - Rating/labeling via NumPad not working
// The rating (0-5) and label (6-9) shortcuts are bound to the top-row digit keys
// '0'..'9'. With NumLock on, the numeric keypad sends the distinct VK_NUMPAD0..9
// virtual-key codes (0x60..0x69), which never matched the '0'..'9' bindings, so the
// keypad could not rate or label. platform::keys::normalize_numpad maps the keypad digits onto
// the equivalent top-row digit while leaving every other key untouched.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_map_numpad_digits_to_rating_keys()
{
	// Every keypad digit VK_NUMPAD0..VK_NUMPAD9 maps to the matching top-row digit,
	// so the rating (0-5) and label (6-9) accelerators fire from the keypad.
	for (char32_t d = 0; d <= 9; ++d)
	{
		const auto numpad_key = platform::keys::numpad0 + d;
		assert_equal(static_cast<char32_t>(U'0' + d), platform::keys::normalize_numpad(numpad_key),
		             "keypad digit maps to top-row digit");
	}

	// Top-row digits are already correct and must pass through unchanged.
	for (char32_t c = U'0'; c <= U'9'; ++c)
	{
		assert_equal(c, platform::keys::normalize_numpad(c), "top-row digit unchanged");
	}

	// Non-digit keys (letters, and the keypad codes just outside the digit range) are
	// left untouched so unrelated shortcuts are not disturbed.
	assert_equal(U'A', platform::keys::normalize_numpad(U'A'), "letter unchanged");
	assert_equal(platform::keys::numpad0 - 1, platform::keys::normalize_numpad(platform::keys::numpad0 - 1),
	             "below range unchanged");
	assert_equal(platform::keys::numpad9 + 1, platform::keys::normalize_numpad(platform::keys::numpad9 + 1),
	             "above range unchanged");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #227 - Removed default sidebar tags reappear after restart
// The default favorite (sidebar) tags must be seeded only on the very first run. Because
// favorite_tags is persisted as one string, an empty saved value is otherwise treated as
// "never configured" and the defaults are re-injected on every launch, resurrecting the
// tags the user deliberately removed. The persisted favorite_tags_initialized flag records
// that favorites have been configured; after that an empty list is respected.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_only_seed_favorite_tags_on_first_run()
{
	// First run: nothing configured yet (flag unset) and the list is empty -> seed defaults.
	assert_equal(true, should_seed_default_favorite_tags(false, true, true), "first run seeds defaults");

	// After the user configured and then cleared favorites (flag set, list empty) the empty
	// list must be respected -- the removed defaults must NOT come back.
	assert_equal(false, should_seed_default_favorite_tags(true, true, false), "cleared list stays cleared");

	// A configured, non-empty list is never overwritten.
	assert_equal(false, should_seed_default_favorite_tags(true, false, false), "configured list not seeded");

	// Upgrade case: an existing user has favorites (non-empty) but no flag yet -> do NOT
	// re-seed over their existing tags.
	assert_equal(false, should_seed_default_favorite_tags(false, false, false), "existing tags preserved on upgrade");

	// Upgrade from a version without the initialization flag: an existing empty saved list
	// means the user removed every favorite, not that this is a first run.
	assert_equal(false, should_seed_default_favorite_tags(false, true, false), "legacy empty list stays empty");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// 1.27.0 - the app crashed during startup with the sidebar hidden, before the user could act.
// A start that never reaches idle is counted; once enough consecutive starts have failed that
// way the next one reverts presentation instead of repeating the crash. The threshold has to
// tolerate a single unsettled start, which is also what a kill or power loss during launch
// looks like, without needing a third crash before it helps.
///////////////////////////////////////////////////////////////////////////////////////////////////

// A command that rewrites pixels - Rotate, the wallpaper, a shared copy - encodes with the user's save
// settings, as the editor does. Rotate re-encoded a lossless WebP original at quality 70 whatever was set.
static void should_encode_commands_with_the_save_settings()
{
	const auto saved = std::tuple(setting.jpeg_save_quality, setting.webp_quality, setting.webp_lossless,
	                              setting.jxl_quality, setting.jxl_lossless, setting.avif_quality,
	                              setting.avif_lossless);
	setting.jpeg_save_quality = 77;
	setting.webp_quality = 33;
	setting.webp_lossless = true;
	setting.jxl_quality = 55;
	setting.jxl_lossless = true;
	setting.avif_quality = 44;
	setting.avif_lossless = true;

	const auto params = make_file_encode_params();
	std::tie(setting.jpeg_save_quality, setting.webp_quality, setting.webp_lossless, setting.jxl_quality,
	         setting.jxl_lossless, setting.avif_quality, setting.avif_lossless) = saved;

	assert_equal(77, params.jpeg_save_quality, "JPEG quality");
	assert_equal(33, params.webp_quality, "WebP quality");
	assert_equal(true, params.webp_lossless, "WebP lossless");
	assert_equal(55, params.jxl_quality, "JPEG XL quality");
	assert_equal(true, params.jxl_lossless, "JPEG XL lossless");
	assert_equal(44, params.avif_quality, "AVIF quality");
	assert_equal(true, params.avif_lossless, "AVIF lossless");
}

// Edit description opens the metadata task with the description alone ticked. A field the task last had
// ticked - a capture date, a rating - would otherwise be written by the same run: the date as today, the
// rating as none.
static void should_select_only_the_description_for_edit_description()
{
	settings_t s;
	s.set_title = s.set_comment = s.set_synopsis = s.set_rating = s.set_year = true;
	s.set_original = s.set_created = true;
	s.set_episode = s.set_season = s.set_track = s.set_disk = true;
	s.set_artist = s.set_album = s.set_album_artist = s.set_genre = s.set_tv_show = true;
	s.set_copyright_notice = s.set_copyright_creator = s.set_copyright_source = s.set_copyright_credit =
		s.set_copyright_url = true;
	s.set_caption = false;

	s.select_only_description();

	const bool others[] = {
		// clang-format off
		s.set_title, s.set_comment, s.set_synopsis, s.set_rating, s.set_year, s.set_original, s.set_created,
		s.set_episode, s.set_season, s.set_track, s.set_disk, s.set_artist, s.set_album, s.set_album_artist,
		s.set_genre, s.set_tv_show, s.set_copyright_notice, s.set_copyright_creator, s.set_copyright_source,
		s.set_copyright_credit, s.set_copyright_url,
		// clang-format on
	};

	assert_equal(true, s.set_caption, "the description is ticked");
	assert_equal(0, static_cast<int>(std::ranges::count(others, true)), "no other field is");
}

static void should_start_safe_only_after_repeated_failures()
{
	assert_equal(false, should_start_safe(0), "a clean history starts normally");
	assert_equal(false, should_start_safe(1), "one unsettled start could be a kill or power loss");
	assert_equal(true, should_start_safe(2), "two in a row is a reproducible startup crash");
	assert_equal(true, should_start_safe(7), "and it stays safe until one start settles");

	// The reset covers what the window puts on screen and turns the graphics path off, and must
	// leave everything the user cannot re-reach from a working window alone.
	settings_t s;
	s.show_sidebar = false;
	s.large_font = true;
	s.show_debug_info = true;
	s.sidebar.show_world_map = false;
	s.sidebar.width = 900;
	s.use_gpu = true;
	s.use_d3d11va = true;
	s.use_yuv = true;
	s.write_folder = "c:\\keep-me";
	s.favorite_tags = "keep";
	s.language = "de";

	s.reset_presentation();

	assert_equal(true, s.show_sidebar, "the sidebar comes back");
	assert_equal(false, s.large_font, "font scale returns to default");
	assert_equal(false, s.show_debug_info, "debug overlay off");
	assert_equal(true, s.sidebar.show_world_map, "sidebar contents return to default");
	assert_equal(0, s.sidebar.width, "sidebar width returns to auto");
	assert_equal(false, s.use_gpu, "hardware acceleration off, not merely defaulted");
	assert_equal(false, s.use_d3d11va, "hardware video decode off");
	// A latch only a driver fault clears, with no option to set it again: clearing it on a safe start
	// switched YUV textures off for good. The GPU being off already keeps them out of this session.
	assert_equal(true, s.use_yuv, "the yuv fault latch is not the safe start's to clear");
	assert_equal("c:\\keep-me"s, s.write_folder, "user paths survive");
	assert_equal("keep"s, s.favorite_tags, "favorite tags survive");
	assert_equal("de"s, s.language, "language survives");
}

// The renderer no longer reads or writes the settings: it reads the YUV latch the app hands it, and
// tells the app when a driver fault clears it. The hook is not called here - it writes the store.
static void should_hand_the_renderer_its_yuv_latch()
{
	const auto saved = setting.use_yuv;
	const auto saved_allowed = render::yuv_textures_allowed;
	const auto saved_hook = render::on_yuv_fault;
	const core::scope_exit restore([saved, saved_allowed, saved_hook]
	{
		setting.use_yuv = saved;
		render::yuv_textures_allowed = saved_allowed;
		render::on_yuv_fault = saved_hook;
	});

	render::on_yuv_fault = nullptr;
	setting.use_yuv = false;
	connect_yuv_latch();
	assert_equal(false, render::yuv_textures_allowed, "a driver that faulted in an earlier run keeps YUV off");
	assert_equal(true, render::on_yuv_fault != nullptr, "and a fault this run reaches the setting");

	setting.use_yuv = true;
	connect_yuv_latch();
	assert_equal(true, render::yuv_textures_allowed, "a driver that never faulted may sample YUV");
}

// Selecting several files in Explorer starts one process each, and they all reach the shared count
// before any of them can settle. Concurrency is not a crash history.
static void should_not_count_concurrent_starts()
{
	const auto owner = decide_startup(true, 1);
	assert_equal(true, owner.record, "the launch holding the startup scope records its attempt");
	assert_equal(2u, owner.next_unsettled, "and raises the count");
	assert_equal(false, owner.safe_start, "one unsettled start is not yet a crash loop");

	assert_equal(true, decide_startup(true, 2).safe_start, "two unsettled starts still escalate");

	const auto concurrent = decide_startup(false, 5);
	assert_equal(false, concurrent.record, "a concurrent launch must not raise the count");
	assert_equal(5u, concurrent.next_unsettled, "so the count is left where it was");
	assert_equal(false, concurrent.safe_start, "and it must not throw away the user's settings");
}

static void should_restore_history_selection()
{
	history_state history;
	const auto search_a = df::search_t::parse("one");
	const auto search_b = df::search_t::parse("two");
	const auto unresolved_area = df::search_t::parse("area:Munich");
	auto resolved_area = unresolved_area;
	resolved_area.resolve_area(map_location_area{.name = "Munich", .cell = {136, 29}, .cell_span = 1});
	const auto selected_a = core::file_path("c:\\one.jpg");
	const auto selected_b = core::file_path("c:\\two.jpg");

	history.history_add(search_a, {});
	history.history_add(search_b, core::paths{{selected_a}, {}});

	history_state::history_entry entry;
	assert_equal(true, history.move_history_pos(-1, core::paths{{selected_b}, {}}, entry), "browse back succeeds");
	assert_equal(true, entry.search == search_a, "back restores first search");
	assert_equal(true, entry.selected.files.size() == 1 && entry.selected.files.front() == selected_a,
	             "back restores first selection");
	history.history_add(entry.search, core::paths{{selected_b}, {}});

	assert_equal(true, history.move_history_pos(1, entry.selected, entry), "browse forward succeeds");
	assert_equal(true, entry.search == search_b, "forward restores second search");
	assert_equal(true, entry.selected.files.size() == 1 && entry.selected.files.front() == selected_b,
	             "forward restores second selection");
	history.history_add(entry.search, core::paths{{selected_a}, {}});

	assert_equal(true, history.move_history_pos(-1, entry.selected, entry), "second browse back succeeds");
	assert_equal(true, entry.selected.files.size() == 1 && entry.selected.files.front() == selected_a,
	             "opening history entries does not overwrite their selection");

	history_state area_history;
	area_history.history_add(unresolved_area, {});
	area_history.replace_current_search(unresolved_area, resolved_area);
	assert_equal(true, area_history._history.back().search == resolved_area,
	             "deferred area resolution updates the current history entry");
	assert_equal(1_z, area_history._history.size(), "deferred area resolution does not add a history entry");
}

class navigation_gate_strategy final : public null_state_strategy
{
public:
	df::search_t vetoed_search;

	bool can_open_search(const df::search_t& path) override
	{
		return vetoed_search.is_empty() || path != vetoed_search;
	}
};

static void drain_query_work(deferred_async_strategy& as)
{
	while (as.run_next(async_queue::query)) as.drain_ui();
}

static void should_commit_history_navigation_only_after_open_is_accepted()
{
	navigation_gate_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const view_host_base_ptr view;
	state.view_mode(view_type::items);

	const auto search_a = df::search_t::parse("history-a");
	const auto search_b = df::search_t::parse("history-b");
	const auto search_c = df::search_t::parse("history-c");

	assert_equal(true, state.open(view, search_a, {}), "first search opens");
	drain_query_work(as);
	assert_equal(true, state.open(view, search_b, {}), "second search opens");
	drain_query_work(as);
	assert_equal(true, state.open(view, search_c, {}), "third search opens");
	drain_query_work(as);
	assert_equal(true, state.search() == search_c, "the fixture starts at C");

	ss.vetoed_search = search_b;
	state.browse_back(view);
	assert_equal(true, state.search() == search_c, "a vetoed Back leaves C displayed");

	history_state::history_entry next_back;
	assert_equal(true, state.history.history_entry_at(-1, next_back), "Back is still available");
	assert_equal(true, next_back.search == search_b, "and still targets the rejected entry");
	assert_equal(false, state.history.can_browse_forward(), "Forward is not invented by a rejected Back");

	ss.vetoed_search = {};
	state.browse_back(view);
	drain_query_work(as);
	assert_equal(true, state.search() == search_b, "accepted Back reaches B");
	assert_equal(true, state.history.can_browse_forward(), "Forward becomes available after accepted Back");
}

// Explore and Faces, where searches are opened from, are steps of the history: Back from the items one opens returns
// to it as it was left, and Close from it leaves the history standing on the listing Items shows.
static void should_step_back_into_the_view_a_search_was_opened_from()
{
	navigation_gate_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const view_host_base_ptr view;
	state.view_mode(view_type::items);

	const auto listing = df::search_t::parse("history-listing");
	const auto week = df::search_t::parse("history-week");

	assert_equal(true, state.open(view, listing, {}), "Items lists a search");
	drain_query_work(as);

	state.open_in_history(view_type::explore);
	assert_equal(true, state.view_mode() == view_type::explore, "Explore opens over it");
	assert_equal(true, state.can_browse_history() && state.history.can_browse_back(), "as a step Back takes back");

	// A part of Explore opens its search in Items.
	assert_equal(true, state.open(view, week, {}), "a week opens");
	state.view_mode(view_type::items);
	drain_query_work(as);

	state.browse_back(view);
	assert_equal(true, state.view_mode() == view_type::explore, "Back from its items returns to Explore");
	assert_equal(true, state.search() == week, "over the listing, which Back into a view leaves alone");
	assert_equal(true, state.history.can_browse_forward(), "and Forward can return to the week");

	state.browse_forward(view);
	assert_equal(true, state.view_mode() == view_type::items && state.search() == week, "which it does");

	state.browse_back(view);
	state.browse_back(view);
	drain_query_work(as);
	assert_equal(true, state.view_mode() == view_type::items && state.search() == listing,
	             "Back again reaches the listing Explore was opened from");

	state.browse_forward(view);
	assert_equal(true, state.view_mode() == view_type::explore, "Forward opens Explore again");
	state.view_mode(view_type::items);
	assert_equal(true, state.history.current_view() == view_type::items,
	             "closed, the history stands on the listing Items shows");
	history_state::history_entry next;
	assert_equal(true, state.history.history_entry_at(1, next) && next.view == view_type::explore,
	             "with Explore after it for Forward");

	state.open_in_history(view_type::explore);
	state.open_in_history(view_type::faces);
	history_state::history_entry before;
	assert_equal(true, state.history.history_entry_at(-1, before) && before.view == view_type::explore,
	             "Faces opened from Explore follows it");

	// A task view is no step of the history, so Back there leaves the task's target as it is.
	state.view_mode(view_type::items);
	state.view_mode(view_type::tags);
	const auto at = state.history._pos;
	state.browse_back(view);
	assert_equal(true, state.view_mode() == view_type::tags && state.history._pos == at,
	             "Back does nothing in a task view");
}

static void should_open_an_area_found_later_without_asking_the_view_to_give_way()
{
	navigation_gate_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const view_host_base_ptr view;
	state.view_mode(view_type::items);

	const auto unresolved = df::search_t::parse("area:Munich");
	const map_location_area munich{.name = "Munich", .cell = {136, 29}, .cell_span = 1};
	auto resolved = unresolved;
	resolved.resolve_area(munich);

	assert_equal(true, state.open(view, unresolved, {}), "the area search opens before its area is known");
	drain_query_work(as);
	assert_equal(true, state.search() == unresolved, "unresolved");

	// A view with unsaved changes refuses every new scope; the open search, with its area found, is not one.
	ss.vetoed_search = resolved;
	state.map_locations({munich});
	assert_equal(true, state.open(view, resolved, {}), "the search opens again with its area found");
	drain_query_work(as);
	assert_equal(true, state.search() == resolved, "and shows the area");

	const auto elsewhere = df::search_t::parse("elsewhere");
	ss.vetoed_search = elsewhere;
	assert_equal(false, state.open(view, elsewhere, {}), "another scope still asks");

	// An area searched without matches everything until it is found, and finding it takes those items away - perhaps
	// the one being edited - so the view is asked first.
	const auto without = df::search_t::parse("-area:Munich");
	assert_equal(false, view_state::finding_areas_only_adds(without), "finding an area searched without takes away");
	ss.vetoed_search = {};
	state.map_locations({});
	assert_equal(true, state.open(view, without, {}), "the search opens before its area is known");
	drain_query_work(as);
	auto found_without = without;
	found_without.resolve_area(munich);
	ss.vetoed_search = found_without;
	state.map_locations({munich});
	assert_equal(false, state.open(view, found_without, {}), "found, it asks the view as any new scope does");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Selection and command enablement
///////////////////////////////////////////////////////////////////////////////////////////////////

static df::item_element_ptr find_item_n(const view_state& s, const int n)
{
	auto count = 0;

	for (const auto& b : s.groups())
	{
		for (const auto& i : b->items())
		{
			if (count == n) return i;
			count++;
		}
	}

	return nullptr;
}

static void should_reveal_an_external_file_without_bypassing_navigation_guards()
{
	navigation_gate_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const view_host_base_ptr view;
	const auto file = test_files_folder().combine_file("Test.jpg");
	const auto scope = df::search_t().add_selector(file.folder());
	state.view_mode(view_type::items);
	state.open(view, scope, {});
	state.filter().wildcard("no-file-could-match-this");
	state.update_item_groups();
	state.update_selection();
	state.view_mode(view_type::media);
	ss.vetoed_search = scope;

	assert_equal(false, state.open_external_file(view, file), "busy or unsaved work can refuse the open");
	assert_equal(false, state.filter().is_empty(), "refusal leaves the user's filter intact");
	assert_equal(true, state.view_mode() == view_type::media, "refusal leaves the current view intact");
	assert_equal(0_z, state.selected_count(), "refusal does not select a hidden target");

	ss.vetoed_search = {};
	assert_equal(true, state.open_external_file(view, file), "an accepted external open succeeds");
	assert_equal(true, state.filter().is_empty(), "a listing filter cannot hide an explicit file request");
	assert_equal(true, state.view_mode() == view_type::items, "the preview and listing are shown");
	assert_equal(true, state.search() == scope, "the target's containing folder is opened");
	assert_equal(1_z, state.selected_count(), "the target is selected even within the same folder");
	assert_equal(true, state.selected_items().items().front()->path() == file, "the requested file is the selection");
	const auto other = test_files_folder().combine_file("Cube.png");
	const auto held = state.selected_items().items().front();
	state._pin_item = held;
	ss.vetoed_search = scope;
	assert_equal(false, state.open_external_file(view, other), "a held comparison cannot bypass the navigation guard");
	assert_equal(true, state._pin_item == held, "a refused external open keeps the held item");
	ss.vetoed_search = {};
	assert_equal(true, state.open_external_file(view, other), "a later request replaces the first");
	assert_equal(true, state._pin_item == nullptr, "an explicit file request releases the held comparison");
	assert_equal(1_z, state.selected_count(), "the two requests are not combined");
	assert_equal(true, state.selected_items().items().front()->path() == other, "the latest request wins");
}

static void should_select_items()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(test_files_folder()), {});
	s.update_item_groups();
	s.update_selection();

	assert_equal(expected_cached_item_count + 1_z, s.search_items().file_paths(false).size(),
	             "items loaded into state");
	assert_equal(expected_cached_item_count + 2_z, s.search_items().file_paths().size(),
	             "items loaded into state with xmp");
	assert_equal(4_z, s.search_items().folder_paths().size(), "folders loaded into state");

	assert_equal(0_z, s.selected_count(), "invalid selection");

	s.select(view, find_item_n(s, 3), false, false, false);
	s.update_selection();
	assert_equal(1_z, s.selected_count(), "invalid selection");

	s.select(view, find_item_n(s, 1), false, false, false);
	s.update_selection();
	assert_equal(1_z, s.selected_count(), "invalid selection");

	s.select(view, find_item_n(s, 8), true, false, false);
	s.update_selection();
	assert_equal(2_z, s.selected_count(), "invalid control selection");

	s.select(view, find_item_n(s, 1), false, false, false);
	s.update_selection();
	assert_equal(1_z, s.selected_count(), "invalid selection");

	s.select(view, find_item_n(s, 4), false, true, false);
	s.update_selection();
	assert_equal(4_z, s.selected_count(), "invalid selection");

	s.select(view, find_item_n(s, 2), true, false, false);
	s.update_selection();
	assert_equal(3_z, s.selected_count(), "invalid selection");

	s.select(view, find_item_n(s, 6), true, false, false);
	s.update_selection();
	assert_equal(4_z, s.selected_count(), "invalid selection");

	const auto outside = find_item_n(s, 0);
	const auto inside = find_item_n(s, 1);
	s.select(view, df::item_elements{outside}, false);
	s.select(view, df::item_elements{inside}, true);
	s.update_selection();
	assert_equal(2_z, s.selected_count(), "control rectangle preserves outside selection");
	s.select(view, df::item_elements{inside}, true);
	s.update_selection();
	assert_equal(1_z, s.selected_count(), "control rectangle toggles inside selection");
	assert_equal(true, s.selected_items().contains(outside), "outside selection remains selected");

	s.select(view, df::item_elements{find_item_n(s, 5)}, false);
	s.select(view, find_item_n(s, 7), false, true, false);
	s.update_selection();
	assert_equal(3_z, s.selected_count(), "shift extends from the focus a rectangle set");

	s.select(view, df::item_elements{find_item_n(s, 4), find_item_n(s, 5)}, false);
	s.unselect(view, find_item_n(s, 4));
	s.select(view, find_item_n(s, 7), false, true, false);
	s.update_selection();
	assert_equal(3_z, s.selected_count(), "unselecting the anchor moves it to the focus");

	df::item_element_ptr photo;
	df::item_element_ptr other;
	for (const auto& item : s.search_items().items())
	{
		if (!item->is_folder() && item->file_type()->group == file_group::photo && !photo) photo = item;
		if (!item->is_folder() && item->file_type()->group != file_group::photo && !other) other = item;
	}
	assert_equal(true, photo != nullptr && other != nullptr, "test has photo and non-photo items");
	s.select(view, df::item_elements{photo, other}, false);
	s.update_selection();
	assert_equal(2_z, s.selected_count(), "mixed media selection");

	s.filter().add_group(file_group::photo);
	s.update_item_groups();
	s.update_selection();
	assert_equal(1_z, s.selected_count(), "filtered item removed from selection");
	assert_equal(true, s.selected_items().contains(photo), "visible selected item preserved");

	s.filter().clear();
	s.update_item_groups();
	s.update_selection();
	assert_equal(1_z, s.selected_count(), "filtered item does not reappear in selection");

	// A dropped focus keeps its last layout bounds, so item_from_location would keep answering with
	// an item select() then rejects as not displayed, silently ignoring clicks over that area.
	s.select(view, other, false, false, false);
	assert_equal(true, s.focus_item() == other, "focus follows selection");

	s.filter().add_group(file_group::photo);
	s.update_item_groups();
	s.update_selection();
	assert_equal(true, s.focus_item() == nullptr, "filtered focus is dropped");

	s.filter().clear();
	s.update_item_groups();
	s.select(view, photo, false, false, false);
	s.update_selection();
	assert_equal(1_z, s.selected_count(), "selection still works after the focus is dropped");
}

static void should_toggle_rating()
{
	const core::file_path src_path1(test_files_folder(), "Test.jpg");
	const core::file_path src_path2(test_files_folder(), "Gherkin.CR2");

	// A private folder: opening the shared suite temp folder would scan every file every other
	// test has left there, making this test's cost and results depend on test order.
	const auto temp_folder = test_temps.next_folder("rating");
	const auto save_path_1 = test_temps.next_path_in(temp_folder, ".jpg");
	const auto save_path_2 = test_temps.next_path_in(temp_folder, ".cr2");

	platform::copy_file(src_path1, save_path_1, false, false);
	platform::copy_file(src_path2, save_path_2, false, false);

	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;
	const auto results = std::make_shared<null_item_results_ui>();

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(temp_folder), {});
	s.item_index.scan_items(s.search_items(), false, false, false, false, test_token);
	s.update_item_groups();

	s.select(view, save_path_1.name(), false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "invalid selection");
	const auto jpeg = s.selected_items().items().front();
	s.toggle_rating(results, {jpeg}, 3, view);
	s.item_index.scan_items(s.search_items(), false, false, false, false, test_token);
	assert_equal(3, s.displayed_rating(), "invalid jpeg rating");

	s.toggle_rating(results, {jpeg}, 0, view);
	s.item_index.scan_items(s.search_items(), false, false, false, false, test_token);
	assert_equal(0, s.displayed_rating(), "invalid remove jpeg rating");

	s.select_nothing(view);
	s.update_selection();
	s.toggle_rating(results, {jpeg}, 2, view);
	s.item_index.scan_items(s.search_items(), false, false, false, false, test_token);
	assert_equal(2, jpeg->rating(), "inline rating target is independent of selection");

	s.select(view, save_path_2.name(), false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "invalid selection");
	s.toggle_rating(results, s.selected_items().items(), 4, view);
	s.item_index.scan_items(s.search_items(), false, false, false, false, test_token);
	assert_equal(4, s.displayed_rating(), "invalid cr2 rating");

	s.item_index.scan_items(s.search_items(), true, false, false, false, test_token);
	assert_equal(4, s.displayed_rating(), "invalid cr2 rating after thumbnail load");

	s.toggle_rating(results, s.selected_items().items(), 0, view);
	s.item_index.scan_items(s.search_items(), false, false, false, false, test_token);
	assert_equal(0, s.displayed_rating(), "invalid remove cr2 rating");
}

// design.md#recent-changes: a write is kept, newest first, under what it did, and the items it changed stay marked.
// The same change made twice is one change, the list keeps only the latest, a renamed item takes its mark along, and
// a change opens again with its items selected where they are now.
static void should_keep_the_session_changes_in_sight()
{
	const auto temp_folder = test_temps.next_folder("recent-changes");
	const auto first = test_temps.next_path_in(temp_folder, ".jpg");
	const auto second = test_temps.next_path_in(temp_folder, ".jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), first, false, false);
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), second, false, false);

	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;
	const auto results = std::make_shared<null_item_results_ui>();
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(temp_folder), {});
	s.item_index.scan_items(s.search_items(), false, false, false, false, test_token);
	s.update_item_groups();

	assert_equal(true, s.session_changes().empty(), "nothing is changed yet");

	s.select(view, first.name(), false);
	s.update_selection();
	s.toggle_rating(results, s.selected_items().items(), 3, view);

	assert_equal(1_z, s.session_changes().size(), "the rating is a change");
	const auto rated = s.session_changes().front();
	assert_equal(std::format("{}: {}", tt.prop_name_rating, prop::format_rating(3)), rated.what,
	             "named by what it set");
	assert_equal(true, rated.paths == std::vector{first}, "on the item it was set on");
	assert_equal(true, s.changed_this_session(first), "which is marked as changed");
	assert_equal(false, s.changed_this_session(second), "and the other is not");

	s.record_change(rated.icon, rated.what, {first});
	assert_equal(1_z, s.session_changes().size(), "the same change made again is one change");

	const auto renamed = temp_folder.combine_file("renamed.jpg");
	s.move_changed(std::vector<std::pair<core::file_path, core::file_path>>{{first, renamed}});
	assert_equal(true, s.changed_this_session(renamed), "a renamed item keeps its mark");
	assert_equal(false, s.changed_this_session(first), "which leaves its old name");
	assert_equal(true, s.session_changes().front().paths == std::vector{renamed}, "and its change follows it");

	// A rename that hands one item's name to another - renumbering, or a swap - moves each change once, to where its
	// own item went.
	s.move_changed(std::vector<std::pair<core::file_path, core::file_path>>{{renamed, second}, {second, first}});
	assert_equal(true, s.session_changes().front().paths == std::vector{second},
	             "an item taking a name another gave up keeps its own change");
	assert_equal(true, s.changed_this_session(second), "and its mark");
	assert_equal(false, s.changed_this_session(first), "which does not follow the other item on");

	// A folder moved takes the changes of everything in it along.
	const auto moved_folder = temp_folder.parent().combine("recent-changes-moved");
	s.move_changed(std::vector<std::pair<core::folder_path, core::folder_path>>{{temp_folder, moved_folder}});
	assert_equal(true, s.changed_this_session(moved_folder.combine_file(second.name())), "a moved folder's item");
	s.move_changed(std::vector<std::pair<core::folder_path, core::folder_path>>{{moved_folder, temp_folder}});

	for (auto i = 0_z; i < view_state::max_session_changes + 2; ++i)
		s.record_change(icon_index::tag, std::format("change {}", i), {second});
	assert_equal(view_state::max_session_changes, s.session_changes().size(), "only the latest changes are kept");
	assert_equal(std::format("change {}", view_state::max_session_changes + 1), s.session_changes().front().what,
	             "newest first");

	s.select_nothing(view);
	s.update_selection();
	assert_equal(true, s.open_changed(view, {second}), "a change opens again");
	s.update_selection();
	assert_equal(1_z, s.selected_count(), "with its item selected");
	assert_equal(true, s.selected_items().items().front()->path() == second, "the one it changed");
}

// design.md#recent-changes: a copy counts the items it made, and a sidecar that went along is its item's.
static void should_count_copied_items_without_their_sidecars()
{
	const auto root = test_temps.next_folder("recent-changes-sidecars");
	const auto movie = root.combine_file("Movie.mkv");
	const auto subtitle = root.combine_file("Movie.en.srt");
	write_test_file(movie, "video");
	write_test_file(subtitle, "1");

	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	core::index_roots roots;
	roots.folders.emplace(root);
	index.index_roots(roots);
	index.index_folders(test_token);
	view_state s(ss, as, index, make_test_player());

	const auto copied = root.parent().combine("recent-changes-sidecars-copied");
	core::paths made;
	made.files = {copied.combine_file(movie.name()), copied.combine_file(subtitle.name())};
	record_dropped_files(s, icon_index::copy_to_folder, tt.command_copy.sv(), {movie, subtitle}, {false, false}, made,
	                     false);

	assert_equal(1_z, s.session_changes().size(), "the copy is one change");
	assert_equal(true, s.session_changes().front().paths == std::vector{made.files.front()},
	             "of the item it made, not the sidecar that went with it");

	// A sidecar copied on its own, as a related search lists one, is an item of its own.
	core::paths alone;
	alone.files = {copied.combine_file(subtitle.name())};
	record_dropped_files(s, icon_index::copy_to_folder, tt.command_copy.sv(), {subtitle}, {false}, alone, false);
	assert_equal(2_z, s.session_changes().size(), "a sidecar copied alone is a change");
	assert_equal(true, s.session_changes().front().paths == alone.files, "of the sidecar");
}

static void assert_can_process(const view_state& s, const bool photos_only, const bool can_save_pixels,
                               const bool can_save_metadata, const bool local_file, const bool local_file_or_folder,
                               const std::string_view message)
{
	const view_host_base_ptr view;
	assert_equal(photos_only, s.can_process_selection(view, df::process_items_type::photos_only), message);
	assert_equal(can_save_pixels, s.can_process_selection(view, df::process_items_type::can_save_pixels), message);
	assert_equal(can_save_metadata, s.can_process_selection(view, df::process_items_type::can_save_metadata), message);
	assert_equal(local_file, s.can_process_selection(view, df::process_items_type::local_file), message);
	assert_equal(local_file_or_folder, s.can_process_selection(view, df::process_items_type::local_file_or_folder),
	             message);
}

static void should_enable_based_on_selection()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(test_files_folder()), {});
	s.update_item_groups();
	s.update_selection();

	assert_equal(0_z, s.selected_count(), "by default nothing selected");
	assert_can_process(s, false, false, false, false, false, "nothing selected");

	s.select(view, "test.jpg", false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "select test.jpg");
	assert_can_process(s, true, true, true, true, true, "photo selected");

	s.select(view, "gizmo.mp4", false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "select gizmo.mp4");
	assert_can_process(s, false, false, true, true, true, "mp4 selected");

	s.select(view, "Gherkin.xmp", false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "Gherkin.xmp");
	assert_can_process(s, false, false, true, true, true, "xmp selected");

	s.select(view, "test.jpg", false);
	s.select(view, "gizmo.mp4", true);
	s.update_selection();

	assert_equal(2_z, s.selected_count(), "test.jpg and gizmo.mp4");
	assert_can_process(s, false, false, true, true, true, "jpg and mp4 selected");

	s.select(view, "raw", false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "folder");
	assert_can_process(s, false, false, false, false, true, "folder selected");

	// A type that carries no edit trait must refuse metadata and say which format refused.
	// can_process must ask file_type::has_trait so a future group-level edit trait cannot
	// silently disagree with the per-extension table.
	s.select(view, "benchmarks.zip", false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "benchmarks.zip");
	assert_can_process(s, false, false, false, true, true, "zip selected");

	const auto zip_result = s.selection_process_result(df::process_items_type::can_save_metadata);
	assert_equal(true, zip_result.fail(), "zip cannot save metadata");
	assert_equal(".zip", std::string(zip_result.first_file_extension), "reason names the extension");

	// A JPEG XL is turned by the orientation its codestream keeps, and edited as any photo is. A HEIF is turned by
	// its image's rotation and mirror, and edited too: Diffractor encodes no HEIF, so an edit but a turn is saved as a
	// JPEG of its own.
	s.select(view, "zoltan-tasi.jxl", false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "zoltan-tasi.jxl");
	assert_can_process(s, true, true, true, true, true, "jxl selected");
	assert_equal(true, s.selected_items().items().front()->file_type()->can_edit_photo(), "a JPEG XL is edited");

	s.select(view, "melnik.heic", false);
	s.update_selection();

	assert_equal(1_z, s.selected_count(), "melnik.heic");
	assert_can_process(s, true, true, true, true, true, "heic selected");
	assert_equal(true, s.selected_items().items().front()->file_type()->can_edit_photo(), "a HEIF is edited");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Rename execution
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_rename()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const core::file_path src_path(test_files_folder(), "Test.jpg");
	const auto save_path_1 = test_temps.next_path(".jpg");
	const auto save_path_2 = test_temps.next_path(".jpg");

	platform::copy_file(src_path, save_path_1, false, false);

	const auto test_item = load_item(index, save_path_1, false);
	// docs/faces.md#62-marks: the marks the user put on its faces go with it.
	const std::array batch{faces::placement_batch{0, {}, {{save_path_1, rectd{0.1, 0.1, 0.2, 0.2}, 0}}}};
	auto& book = index.faces().book();
	book.place(batch, faces::face_action::hide, book.generation());

	assert_equal(true, test_item->rename(index, save_path_2.file_name_without_extension()).success(), "can rename");
	assert_equal(save_path_2.name(), test_item->name(), "renamed");
	assert_equal(true, platform::exists(save_path_2), "renamed exists");
	assert_equal(true,
	             index.faces().book().marks(save_path_1).empty() && index.faces().book().marks(save_path_2).size() == 1,
	             "its face marks go with it");
}

static void should_not_overwrite_during_rename()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const core::file_path src_path(test_files_folder(), "Test.jpg");
	const auto save_path_1 = test_temps.next_path(".jpg");
	const auto save_path_2 = test_temps.next_path(".jpg");

	platform::copy_file(src_path, save_path_1, false, false);
	platform::copy_file(src_path, save_path_2, false, false);

	const auto test_item = load_item(index, save_path_1, false);

	assert_equal(false, test_item->rename(index, save_path_2.file_name_without_extension()).success(),
	             "should not rename");
	assert_equal(save_path_1.name(), test_item->name(), "not renamed");
	assert_equal(true, platform::exists(test_item->path()), "exists");
}

static void should_rename_file_case()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto source = test_temps.next_path(".jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);
	const auto item = load_item(index, source, false);
	auto upper_name = core::to_upper(std::string(source.file_name_without_extension()));
	if (upper_name == source.file_name_without_extension()) upper_name = core::to_lower(upper_name);
	const auto destination = core::file_path(source.folder(), upper_name, source.extension());

	assert_equal(true, item->rename(index, upper_name).success(), "case-only rename succeeds");
	assert_equal_strict(destination.name(), item->name(), "item keeps renamed case");
	assert_equal(true, platform::exists(destination), "case-only destination exists");
}

static void should_rollback_rename_when_sidecar_fails()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto primary = test_temps.next_path(".jpg");
	const auto sidecar = primary.extension(".xmp");
	const auto target_base = std::string(primary.file_name_without_extension()) + "-renamed";
	const auto target_sidecar = core::file_path(primary.folder(), target_base, ".xmp");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), primary, false, false);
	platform::copy_file(core::file_path(test_files_folder(), "IMG_0604.xmp"), sidecar, false, false);
	platform::copy_file(core::file_path(test_files_folder(), "IMG_0604.xmp"), target_sidecar, false, false);

	df::index_file_item info;
	info.name = primary.name();
	info.ft = files::file_type_from_name(primary);
	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = sidecar.name();
	info.metadata.store(metadata);
	const auto item = std::make_shared<df::item_element>(primary, info);

	assert_equal(false, item->rename(index, target_base).success(), "sidecar collision fails rename");
	assert_equal(primary.name(), item->name(), "item keeps original name");
	assert_equal(true, platform::exists(primary), "primary remains at original path");
	assert_equal(true, platform::exists(sidecar), "sidecar remains at original path");
}

static void should_detect_original_path()
{
	const core::file_path path("c:\\temp\\test.original.jpg");
	assert_equal(true, path.is_original(), "detect original");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Collection membership and crash recording
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_toggle_collection_entry(shared_test_context& /*stc*/)
{
	const auto local_folders = platform::local_folders();

	settings_t::index_t settings;
	toggle_collection_entry(settings, local_folders.pictures, false);
	assert_equal(true, settings.pictures, "pictures add");

	toggle_collection_entry(settings, local_folders.pictures, true);
	toggle_collection_entry(settings, test_files_folder(), false);
	assert_equal(false, settings.pictures, "pictures remove");
	assert_equal(test_files_folder().text(), settings.more_folders);

	toggle_collection_entry(settings, local_folders.video, false);
	toggle_collection_entry(settings, test_files_folder(), true);
	assert_equal(true, settings.video, "pictures remove");
	assert_equal({}, settings.more_folders);
}

// Options keeps the collection's other folders one per line, and an INI line cannot hold a line break:
// after a restart only the first folder was left. On 1.27.2 an apostrophe also opened a quote, which
// cost the reported folder its last letter.
static void should_keep_collection_folders_across_a_restart()
{
	const auto settings_folder = test_temps.next_folder("ini-collection-folders");

	// As Options writes them, each quoted where it needs it, and then a line
	// toggle_collection_entry appends: unquoted, after \r\n.
	const auto more_folders = "\"C:\\Photos\"\n\"C:\\Foto's\"\r\nD:\\Raw"s;
	assert_equal(true, platform::create_ini_file_settings(settings_folder)->write("index", "more", more_folders),
	             "the folders are written");

	std::string read;
	assert_equal(true, platform::create_ini_file_settings(settings_folder)->read("index", "more", read),
	             "and read after a restart");

	const auto folders = split_collection_folders(read);
	assert_equal(3_z, folders.size(), "every folder survives");
	assert_equal("C:\\Photos", folders[0], "the first folder is unquoted");
	assert_equal("C:\\Foto's", folders[1], "an apostrophe keeps the letter after it");
	assert_equal("D:\\Raw", folders[2], "and the folder the collection command added is kept");
}

static void should_record_crashes()
{
	const auto db_path = test_temps.next_path();
	const auto build = "test-build-1"sv;
	const auto paths = {
		test_files_folder().combine_file("Test.jpg"),
		test_files_folder().combine_file("Test90.jpg"),
		test_files_folder().combine_file("Small.jpg"),
		test_files_folder().combine_file("Lossless0.jpg"),
	};

	for (auto path : paths)
	{
		{
			crash_files_db test_crash_files(db_path, build);

			assert_equal(test_crash_files.is_known_crash_file(path), false, "is_known_crash_file");

			const auto id = test_crash_files.add_open(path, core::utf8_cast(__FUNCTION__));
			test_crash_files.remove_open(id);
			test_crash_files.flush_open_files();
		}

		{
			crash_files_db test_crash_files(db_path, build);

			assert_equal(test_crash_files.is_known_crash_file(path), false, "is_known_crash_file");

			test_crash_files.add_open(path, core::utf8_cast(__FUNCTION__));
			test_crash_files.flush_open_files();
		}

		{
			crash_files_db test_crash_files(db_path, build);

			assert_equal(test_crash_files.is_known_crash_file(path), true, "is_known_crash_file");
		}
	}

	{
		crash_files_db test_crash_files(db_path, build);
		assert_equal(test_crash_files.skipped_file_count(), paths.size(), "every crash is recorded once");
	}

	// A repeating crash must not append the same file again, or the list would grow with every run.
	{
		crash_files_db test_crash_files(db_path, build);

		for (auto path : paths) test_crash_files.add_open(path, core::utf8_cast(__FUNCTION__));
		test_crash_files.flush_open_files();
	}

	{
		crash_files_db test_crash_files(db_path, build);
		assert_equal(test_crash_files.skipped_file_count(), paths.size(), "a repeated crash adds no duplicates");
	}

	// A decoder fix ships in an update, so entries earned by an earlier release must not skip files
	// forever - the next release line retries them.
	{
		crash_files_db next_build(db_path, "test-build-2"sv);
		assert_equal(next_build.skipped_file_count(), 0_z, "a new release retries files an older one recorded");
	}

	// A crash is far more costly than a missing thumbnail, so the tag is coarse: neither a rebuild nor
	// a point release may discard what the list has earned.
	assert_equal(crash_files_db::release_tag("127.1"), crash_files_db::release_tag("127.9"),
	             "a point release keeps the skip list");
	assert_equal(crash_files_db::release_tag(df::format_version(true)), crash_files_db::release_tag(s_app_version),
	             "the tag carries no build number");
	assert_equal(crash_files_db::release_tag("127.1") != crash_files_db::release_tag("128.0"), true,
	             "the next release line retries");

	// SRC-007 - overlapping registrations for the same path are independent; finishing one worker
	// must not erase the other worker's still-live crash claim.
	const auto overlap_path = test_files_folder().combine_file("Test.jpg");

	for (const auto remove_first : {true, false})
	{
		const auto overlap_db_path = test_temps.next_path();
		{
			crash_files_db test_crash_files(overlap_db_path, build);
			const auto first = test_crash_files.add_open(overlap_path, "first");
			const auto second = test_crash_files.add_open(overlap_path, "second");

			test_crash_files.remove_open(remove_first ? first : second);
			test_crash_files.flush_open_files();
		}

		{
			crash_files_db test_crash_files(overlap_db_path, build);
			assert_equal(true, test_crash_files.is_known_crash_file(overlap_path),
			             remove_first ? "second overlapping registration survives"
			                          : "first overlapping registration survives");
			assert_equal(1_z, test_crash_files.skipped_file_count(), "overlap is persisted once by path");
		}
	}
}

static duplicate_report_test_photo duplicate_report_photo(const std::string_view name, const uint64_t created_key,
                                                          const uint64_t hash)
{
	duplicate_report_test_photo result;
	result.path = core::file_path(core::folder_path("c:\\duplicate-report"), name);
	result.created_key = created_key;
	result.phash = {hash, 0, 0, 0};
	return result;
}

static void should_anchor_duplicate_report_sets()
{
	{
		const std::vector photos = {
			duplicate_report_photo("z.jpg", 1, 0x100),
			duplicate_report_photo("other.jpg", 2, 0x100),
			duplicate_report_photo("a.jpg", 1, 0x100),
		};

		const auto stats = calc_duplicate_report_anchor_stats_for_tests(photos);
		assert_equal(1_z, stats.sets, "global index equal to bucket size is a valid anchor");
		assert_equal(2_z, stats.grouped_photos, "the anchored set includes both matching photos");
	}

	{
		const std::vector photos = {
			duplicate_report_photo("outside-0.jpg", 1, 0),
			duplicate_report_photo("outside-1.jpg", 2, 0),
			duplicate_report_photo("z.jpg", 3, 0x200),
			duplicate_report_photo("a.jpg", 3, 0x200),
		};

		const auto stats = calc_duplicate_report_anchor_stats_for_tests(photos);
		assert_equal(1_z, stats.sets, "a later smaller path can replace an anchor whose index equals the bucket size");
	}

	{
		const std::vector photos = {
			duplicate_report_photo("a.jpg", 1, 0),
			duplicate_report_photo("b.jpg", 1, 0),
		};

		const auto stats = calc_duplicate_report_anchor_stats_for_tests(photos);
		assert_equal(0_z, stats.sets, "a bucket with no usable hash has no eligible anchor");
	}
}

static void should_fail_duplicate_report_command_output_errors()
{
	const auto quiet = [](std::string_view) {};
	const auto input = test_temps.folder().combine("dup-report-empty");
	assert_equal(true, platform::create_folder(input).success(), "empty input folder is created");

	const auto missing_parent_output = input.combine("missing").combine_file("report.txt");
	assert_equal(1, run_duplicate_report(input.text(), missing_parent_output.str(), quiet),
	             "a requested report under a missing parent fails");
	assert_equal(false, platform::exists(missing_parent_output), "failed report output is not created");

	const auto directory_output = input.combine("report-directory");
	assert_equal(true, platform::create_folder(directory_output).success(), "directory output target is created");
	assert_equal(1, run_duplicate_report(input.text(), directory_output.text(), quiet),
	             "a requested report that names a directory fails");

	const auto relative_name = std::string(test_temps.next_path(".txt").name());
	const auto relative_report = core::file_path(core::folder_path("."), relative_name);
	if (platform::exists(relative_report)) platform::delete_file(relative_report);
	assert_equal(false, platform::exists(relative_report), "relative report starts absent");
	assert_equal(0, run_duplicate_report(input.text(), relative_name, quiet),
	             "a bare relative report name writes to the current folder");
	assert_equal(true, platform::exists(relative_report), "relative report output is created");
	platform::delete_file(relative_report);

	assert_equal(0, run_duplicate_report(input.text(), {}, quiet), "stdout-only duplicate report remains successful");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The ping store keeps daily aggregate totals, so whatever the ingest decodes on the day is the
// only reading of that day that will ever exist: a field misread for a month is a month gone. The
// decoder therefore lives here beside the encoder, and this covers every field at its extremes.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_round_trip_the_environment_mask()
{
	const auto round_trip = [](const platform::environment_facts& facts, const std::string_view what)
	{
		const auto packed = platform::pack_environment(facts);
		const auto read_back = platform::unpack_environment(packed);

		assert_equal(true, read_back.has_value(), std::format("{} decodes", what));
		assert_equal(true, read_back.value() == facts, std::format("{} reads back as itself", what));
	};

	// Nothing established: every field answers unknown, which is a reading rather than an absence.
	round_trip({}, "an unidentified machine");

	// Every field at the top of its range at once. This is what a field running out of room looks
	// like, and it has to survive being written down.
	round_trip({platform::os_family::other, platform::os_release::other, platform::machine_arch::other,
	            platform::machine_arch::other, platform::package_kind::other},
	           "every field at its widest");

	// The cross-tabs the fields exist to answer, each one a real shape.
	round_trip({platform::os_family::windows, platform::os_release::windows_11, platform::machine_arch::x86,
	            platform::machine_arch::x64, platform::package_kind::installer},
	           "a 32-bit process on 64-bit Windows 11");
	round_trip({platform::os_family::windows, platform::os_release::windows_10, platform::machine_arch::x64,
	            platform::machine_arch::arm64, platform::package_kind::microsoft_store},
	           "an x64 build on arm64 hardware under translation");
	round_trip({platform::os_family::linux_, platform::os_release::unknown, platform::machine_arch::riscv64,
	            platform::machine_arch::riscv64, platform::package_kind::flatpak},
	           "a Linux package");

	// Each field on its own, so a shift or a width that is wrong cannot hide behind a neighbour.
	for (const auto family : {platform::os_family::unknown, platform::os_family::windows, platform::os_family::other})
		round_trip({.family = family}, "an OS family alone");
	for (const auto release :
	     {platform::os_release::unknown, platform::os_release::windows_7, platform::os_release::other})
		round_trip({.release = release}, "an OS version alone");
	for (const auto arch :
	     {platform::machine_arch::unknown, platform::machine_arch::x86, platform::machine_arch::other})
	{
		round_trip({.process = arch}, "a process architecture alone");
		round_trip({.machine = arch}, "an OS architecture alone");
	}
	for (const auto package :
	     {platform::package_kind::unknown, platform::package_kind::appimage, platform::package_kind::other})
		round_trip({.package = package}, "a package alone");

	// The layout version is read first and anything else fails closed. Guessing at an unrecognised
	// layout is the one mistake a daily aggregate cannot be talked out of.
	const auto packed = platform::pack_environment({platform::os_family::windows, platform::os_release::windows_11});
	assert_equal(platform::environment_layout_version, platform::environment_layout::version.unpack(packed),
	             "the version is the first field");
	assert_equal(false, platform::unpack_environment((packed & ~0xfull) | 0xf).has_value(),
	             "an unrecognised layout is left undecoded");

	// A reserved bit means the writer knew something this reader does not, so the same rule applies.
	assert_equal(0ull, packed & platform::environment_layout::reserved, "nothing is written above the last field");
	assert_equal(false, platform::unpack_environment(packed | (1ull << 27)).has_value(),
	             "a value using a reserved bit is left undecoded");

	// The value travels as hex, and the identity carries no measurement, so it stays small enough to
	// read at a glance rather than becoming an opaque 16 digits.
	assert_equal(true,
	             core::to_hex(
					 platform::pack_environment({platform::os_family::windows, platform::os_release::windows_11,
	                                             platform::machine_arch::x64, platform::machine_arch::x64,
	                                             platform::package_kind::installer}))
	                     .size() <= 8,
	             "an ordinary machine packs into a short value");
}

// A view_state opened over the fixture collection, with no window. Every test below drives the same
// entry points the commands do, so what they pin is the behavior a user sees rather than a helper.
static void should_retest_the_sidebar_globe_button_when_pointed_at()
{
	struct record_explore final : null_state_strategy
	{
		int opened = 0;
		void invoke(const commands id) override
		{
			if (id == commands::explore) ++opened;
		}
	} ss;
	null_async_strategy as;
	index_state index(as, as.locations);
	view_state state(ss, as, index, make_test_player());
	const auto host = std::make_shared<sidebar_host>(state);
	const core::scope_exit reset_controller([&host] { host->_active_controller.reset(); });
	host->_elements = {host->_map};
	ui::test::fake_measure_context mc({8, 16});
	host->layout_embedded(mc, recti(0, 0, 320, 600));
	const auto& globe = *host->_map;
	const auto button = globe.expand_bounds({});
	const auto on_globe = globe.bounds.center();
	const auto on_button = button.center();

	ui::test::fake_draw_context rest;
	globe.render(rest, {});
	assert_equal(true, rest.ellipses.empty(), "the globe button has no background at rest");
	assert_equal(true,
	             std::ranges::any_of(rest.texts, [](const auto& t)
	{ return t.font == render::font_face::icons && t.text == icon_to_utf8(icon_index::world); }),
	             "the globe button draws the world icon");

	host->on_mouse_move(on_globe, false);
	host->on_mouse_move(on_button, false);
	assert_equal(true, host->_active_controller && host->_active_controller->bounds() == button,
	             "moving directly from the globe picks up the button");
	ui::test::fake_draw_context hovered;
	globe.render(hovered, {});
	assert_equal(true, std::ranges::any_of(hovered.ellipses, [button](const auto& e) { return e.bounds == button; }),
	             "the globe button lights while the pointer is over it");
	host->on_mouse_left_button_down(on_button, {});
	host->on_mouse_left_button_up(on_button, {});
	assert_equal(1, ss.opened, "the globe button opens Explore");
	host->on_mouse_move(on_button, false);
	host->on_mouse_left_button_down(on_button, {});
	host->on_mouse_move(on_globe, true);
	ui::test::fake_draw_context outside;
	globe.render(outside, {});
	assert_equal(true, outside.ellipses.empty(), "dragging off clears the button highlight");
	host->on_mouse_left_button_up(on_globe, {});
	assert_equal(1, ss.opened, "releasing off the globe button does not open Explore");
}

static void should_show_the_sidebar_faces_button_before_previews()
{
	browsing_fixture fixture;
	const auto host = std::make_shared<sidebar_host>(fixture.state);
	ui::test::fake_measure_context mc({8, 16});
	const auto check_button = [&]
	{
		host->layout_embedded(mc, recti(0, 0, 320, 800));
		assert_equal(true, std::ranges::any_of(host->_elements, [&](const auto& e) { return e == host->_face_row; }),
		             "the Faces row belongs to the sidebar before previews arrive");
		const auto parts = host->_face_row->keyboard_parts();
		assert_equal(1_z, parts.size(), "the empty row has just one button");
		assert_equal(false, parts.front().is_empty(), "the Faces button has a usable target");
	};

	check_button();
	fixture.state.item_index.faces().configure(true, true, false);
	host->faces_changed();
	check_button();
}

static void should_pan_year_columns_in_the_sidebar_calendar()
{
	browsing_fixture fixture;
	for (const auto scale : {1, 2})
	{
		const auto host = std::make_shared<sidebar_host>(fixture.state);
		const core::scope_exit reset_controller([&host] { host->_active_controller.reset(); });
		auto& calendar = *host->_history_chart;
		calendar._offered = {calendar._current_year - 40, calendar._current_year};
		ui::test::fake_measure_context mc({8 * scale, 16 * scale});
		mc.scale_factor = scale;
		const auto measured = calendar.measure(mc, 320 * scale);
		assert_equal(16 * scale, calendar._year_width, "year columns remain compact rather than following label width");
		assert_equal(8 * scale, calendar.row_height, "month cells do not reserve room for month-name labels");
		assert_equal(true, measured.cy <= 128 * scale, "the twelve-month chart stays compact");
		calendar.layout(mc, recti(10, 20, 10 + measured.cx, 20 + measured.cy));
		for (auto year = calendar._range.first_year; year <= calendar._range.last_year; ++year)
			assert_equal(year % 5 == 0, !calendar.year_label_bounds(calendar.bounds, year).is_empty(),
			             "only five-year landmarks have a label");
		const auto latest = calendar.month_bounds(calendar.bounds, calendar._range.last_year, 11);
		const auto january = calendar.month_bounds(calendar.bounds, calendar._range.last_year, 0);
		assert_equal(true, january.top > latest.bottom, "January is at the front and December at the back");
		assert_equal(true, january.left < latest.left, "the month rows follow the same depth direction as the blocks");
		assert_equal(true, std::abs(latest.right - (calendar.bounds.right - calendar._skew_pad)) <= 1,
		             "the initial viewport ends at the newest year");
		assert_equal(measured.cy, calendar._top_pad + 12 * calendar.row_height + 1,
		             "there is no separate navigator or row of years underneath");
		const auto preceding = calendar.month_bounds(calendar.bounds, calendar._range.last_year - 1, 11);
		assert_equal(calendar._year_width, latest.left - preceding.left, "time advances in columns from left to right");

		const auto newest_pan = calendar._pan_years;
		const auto point = calendar.bounds.center();
		assert_equal(true, calendar.wheel(point, {}, 120), "the calendar takes the wheel at its bounds");
		assert_equal(newest_pan - 1.0, calendar._pan_years, "one wheel step exposes the preceding year");
		calendar.pan_to(-100);
		assert_equal(0.0, calendar._pan_years, "panning stops at the oldest year");
		calendar.pan_to(1000);
		assert_equal(newest_pan, calendar._pan_years, "panning stops at the newest year");

		calendar._window_is_user_set = false;
		calendar.begin_pan();
		calendar.pan_to(0);
		calendar.cancel_pan();
		assert_equal(newest_pan, calendar._pan_years, "Escape restores the pan origin");
		assert_equal(false, calendar._window_is_user_set, "cancelling preserves the initial follow-recent policy");

		calendar.pan_to(3);
		calendar.layout(mc, calendar.bounds);
		assert_equal(3.0, calendar._pan_years, "relayout does not yank a user-panned calendar to the recent end");
		const auto search = df::search_t().day(0, 6, calendar._range.first_year);
		assert_equal(true, calendar.set_current_search(search),
		             "a newly selected offscreen month is brought into view");
		calendar.pan_to(5);
		calendar.set_current_search(search);
		assert_equal(5.0, calendar._pan_years, "refreshing the same search does not undo a later pan");

		calendar.pan_to(newest_pan);
		const auto year = calendar._current_year - 2;
		const auto index = (calendar._current_year - year) * 12 + 5;
		calendar._counts[index].created = 17;
		assert_equal(true, calendar._chart.prepare(calendar.bounds.extent()), "the compact hit map is allocated");
		std::vector<calendar_chart_cell> cells;
		calendar.build_month_cells(cells, recti(calendar.bounds.extent()), calendar.bounds.extent(), 0xFFFFFF);
		render_calendar_chart(calendar._chart, cells, calendar._month_style);
		calendar._chart_invalid = false;
		const auto find_hit = [&calendar](const int month)
		{
			pointi hit{-1, -1};
			for (auto y = 0; y < calendar.bounds.height() && hit.x < 0; ++y)
				for (auto x = 0; x < calendar.bounds.width(); ++x)
					if (calendar._chart.id_at({x, y}) == calendar.month_id_base + month)
					{
						hit = {x, y};
						break;
					}
			return hit;
		};
		const auto hit = find_hit(index);
		assert_equal(true, hit.x >= 0, "a narrow date cell retains a real hover target");
		host->_extent = {calendar.bounds.right + 100, calendar.bounds.bottom + 100};
		host->_scroller._offset = {0, 7 * scale};
		const auto offset = -host->_scroller.scroll_offset();
		const auto check_arrow = [&](const view_hover_element& popup, const int month)
		{
			cells.clear();
			calendar.build_month_cells(cells, recti(calendar.bounds.extent()), calendar.bounds.extent(), 0xFFFFFF);
			render_calendar_chart(calendar._chart, cells, calendar._month_style);
			assert_equal(true, popup.window_bounds.width() <= calendar._year_width,
			             "the bubble anchors to one month rather than the whole calendar");
			for (const auto left : {0, -2000 * scale})
			{
				const auto placement = ui::place_bubble({600 * scale, 180 * scale}, popup.window_bounds, popup.x_focus,
				                                        popup.horizontal, {left, 0, 2000 * scale, 2000 * scale});
				const auto pixel = placement.focus - calendar.bounds.top_left() - offset;
				assert_equal(static_cast<uint16_t>(calendar.month_id_base + month), calendar._chart.id_at(pixel),
				             "the bubble arrow connects to the actual highlighted cell on either side");
			}
		};
		const auto loc = calendar.bounds.top_left() + hit + offset;
		host->on_mouse_move(loc, false);
		assert_equal(true, host->_active_controller != nullptr,
		             "the real sidebar routes the hover to its calendar controller");
		assert_equal(index, calendar._hover_month, "the hit map still identifies the exact month and year");
		view_hover_element popup;
		popup.elements = std::make_shared<view_elements>();
		host->_active_controller->popup_from_location(popup);
		assert_equal(true, popup.elements->size() >= 5, "hover supplies the date and counts in a popup bubble");
		assert_equal(true, popup.active_bounds == calendar.bounds.offset(offset),
		             "the bubble follows the scrolled sidebar");
		assert_equal(true, host->_active_controller->_last_loc == loc,
		             "the controller retains the actual hover position");
		check_arrow(popup, index);
		const auto* controller = host->_active_controller.get();
		const auto next = find_hit(index + 1);
		assert_equal(true, next.x >= 0, "another month has a hover target");
		host->on_mouse_move(calendar.bounds.top_left() + next + offset, false);
		assert_equal(true, host->_active_controller.get() == controller,
		             "moving between cells retains the pan controller");
		assert_equal(index + 1, calendar._hover_month, "the retained controller follows the next month");
		popup.elements->clear();
		host->_active_controller->popup_from_location(popup);
		assert_equal(true, popup.elements->size() >= 5, "the popup remains available when the hovered month changes");
		check_arrow(popup, index + 1);
		host->on_mouse_leave({-1, -1});
		assert_equal(false, calendar.hover_month_is_valid(), "leaving the calendar clears its hover");
	}
}

// The pie and the globe keep one size; the calendar is a timeline, so it takes the sidebar's width and fills
// it with years whatever the collection offers, ending at this year.
static void should_fill_the_sidebar_calendar_with_years()
{
	browsing_fixture fixture;
	const auto host = std::make_shared<sidebar_host>(fixture.state);
	auto& calendar = *host->_history_chart;
	const auto this_year = calendar._current_year;
	ui::test::fake_measure_context mc({8, 16});

	const auto measured = calendar.measure(mc, 600);
	assert_equal(600, measured.cx, "the calendar is as wide as the sidebar");

	const auto earlier = this_year - 20;
	const auto earlier_june = (this_year - earlier) * 12 + 5;
	calendar._counts[earlier_june].created = 9;

	for (const auto typed : {false, true})
	{
		calendar._offered = {this_year - df::history_window_years + 1, this_year};
		calendar._start_is_typed = typed;
		calendar._window_is_user_set = false;
		calendar.layout(mc, recti(0, 0, measured.cx, measured.cy));

		assert_equal(true, calendar._range.first_year <= earlier,
		             "earlier years fill the width the offered ones leave");
		const auto newest = calendar.month_bounds(calendar.bounds, this_year, 11);
		assert_equal(true, std::abs(newest.right - (calendar.bounds.right - calendar._skew_pad)) <= 1,
		             "the calendar still ends at this year");
		const auto oldest = calendar.month_bounds(calendar.bounds, calendar._range.first_year, 0);
		assert_equal(true, oldest.left <= calendar.bounds.left + calendar._skew_pad, "no width is left empty");

		std::vector<calendar_chart_cell> cells;
		calendar.build_month_cells(cells, recti(calendar.bounds.extent()), calendar.bounds.extent(), 0xFFFFFF);
		const auto target = std::ranges::any_of(cells, [&calendar, earlier_june](const calendar_chart_cell& c)
		{ return c.id == calendar.month_id_base + earlier_june; });
		assert_equal(!typed, target,
		             typed ? "the years before a typed start are empty and cannot be opened"
		                   : "the years before a worked-out start are offered like any other");
	}
}

// The keyboard reaches the calendar's months, earliest first: those the collection has reached, in the years the
// window shows, and none still to come. Pressed, a month opens as a click on it does.
static void should_open_a_calendar_month_from_the_keyboard()
{
	browsing_fixture fixture;
	const auto host = std::make_shared<sidebar_host>(fixture.state);
	auto& calendar = *host->_history_chart;
	ui::test::fake_measure_context mc({8, 16});
	calendar._offered = {calendar._current_year - 2, calendar._current_year};
	const auto measured = calendar.measure(mc, 600);
	calendar.layout(mc, recti(0, 0, measured.cx, measured.cy));

	const auto months = calendar.keyboard_months();
	const auto parts = calendar.keyboard_parts();
	assert_equal(false, months.empty(), "the calendar's months are parts of the keyboard's");
	assert_equal(months.size(), parts.size(), "a part for each");
	assert_equal(true, std::ranges::is_sorted(months), "earliest first");
	assert_equal(true,
	             std::ranges::none_of(months, [&calendar](const std::pair<int, int>& m)
	{ return calendar.is_future(m.first, m.second); }),
	             "none still to come");
	assert_equal(true,
	             std::ranges::all_of(parts,
	                                 [&calendar](const recti r) { return calendar.bounds.contains(r.center()); }),
	             "each in the window");

	const auto [year, month] = months.back();
	assert_equal(true, calendar.press_keyboard_part(static_cast<int>(months.size()) - 1, host), "a month opens itself");
	const auto opened = fixture.state.search().find_date_parts();
	assert_equal(year, opened.year, "the year pressed");
	assert_equal(month + 1, opened.month, "and its month");
}

namespace
{
	struct tooltip_test_bubble final : ui::bubble_frame
	{
		recti anchor;
		bool shown = false;
		int shows = 0;
		void show(const ui::element_ptr&, const recti target, int, int, bool) override
		{
			anchor = target;
			shown = true;
			++shows;
		}
		void hide() override { shown = false; }
		bool is_visible() const override { return shown; }
	};
}

static void should_hint_the_shortcut_for_the_action_under_the_pointer()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	app->_state.view_mode(view_type::items);
	const auto hint_for = [](const commands id, const app_frame& app)
	{
		return std::format("{} {}", tt.keyboard_accelerator_press, app.find_command(id)->keyboard_accelerator_text);
	};
	view_hover_element hover;
	const auto last_hint = [&hover]
	{
		std::string text;
		hover.elements->visit_children([&text](const view_element_ptr& child)
		{
			const auto hint = std::dynamic_pointer_cast<action_element>(child);
			text = hint ? hint->text() : std::string{};
		});
		return text;
	};
	sidebar_map_element globe(app->_state, {});
	globe.bounds = {0, 0, 200, 200};
	hover.clear();
	globe.tooltip(hover, globe.expand_bounds({}).center(), {});
	assert_equal(hint_for(commands::explore, *app), last_hint(), "the globe's Expand ends with the Explore shortcut");
	hover.clear();
	globe.tooltip(hover, globe.bounds.center(), {});
	assert_equal(std::string{}, last_hint(), "the globe away from Expand does not advertise Explore");

	sidebar_face_row row(app->_state);
	row.bounds = {0, 0, 240, 40};
	hover.clear();
	row.tooltip(hover, {220, 20}, {});
	assert_equal(hint_for(commands::faces, *app), last_hint(), "the Faces button shows the same key as the toolbar");
	hover.clear();
	row.tooltip(hover, {110, 20}, {});
	assert_equal(true, hover.is_empty(), "the gap between faces and their button does not act");

	const auto exit_hint = [&]
	{
		hover.clear();
		app->tooltip(hover, commands::exit);
		return last_hint();
	};
	assert_equal(std::format("{} {}", tt.keyboard_accelerator_press,
	                         platform::format_keyboard_accelerator({close_view_shortcut()})),
	             exit_hint(), "in normal Items the close shortcut exits the application");
	app->_state.is_full_screen = true;
	assert_equal(std::string{}, exit_hint(), "in fullscreen the same key is not Exit");
}

static void should_place_sidebar_and_bar_bubbles_in_screen_coordinates()
{
	struct child_frame final : ui::null_frame
	{
		recti parent_bounds;
		pointi screen_origin;
		recti window_bounds() const override { return parent_bounds; }
		pointi client_to_screen(const pointi loc) const override { return loc + screen_origin; }
	};
	struct popup_controller final : view_controller
	{
		popup_controller(const view_host_ptr& host, const recti anchor) : view_controller(host, anchor) {}
		void popup_from_location(view_hover_element& hover) override
		{
			hover.elements->add(std::make_shared<text_element>("Sidebar tooltip"));
			hover.active_bounds = hover.window_bounds = _bounds;
			hover.horizontal = true;
		}
	};
	const auto help = setting.show_help_tooltips;
	const auto debug_info = setting.show_debug_info;
	const auto active = ui::command_active.load();
	const core::scope_exit restore([=]
	{
		setting.show_help_tooltips = help;
		setting.show_debug_info = debug_info;
		ui::command_active = active;
	});
	setting.show_help_tooltips = true;
	setting.show_debug_info = false;
	ui::command_active = 0;
	const auto app = std::make_shared<app_frame>();
	const auto sidebar = app->_view_items->sidebar();
	const core::scope_exit clear_controllers([&]
	{
		app->_view_frame->_active_controller.reset();
		sidebar->_active_controller.reset();
	});
	const auto frame = std::make_shared<child_frame>();
	app->_view_frame->_frame = frame;
	sidebar->attach_embedded(frame, ui::no_frame());
	app->_title_bar->_frame = frame;
	app->_status_bar->_frame = frame;
	const auto bubble = std::make_shared<tooltip_test_bubble>();
	app->_bubble = bubble;
	bool first_hover = true;
	uint64_t now = 0;
	const auto finish_dwell = [&](const pointi position, const int scale)
	{
		app->update_tooltip_visibility(now, position, scale);
		app->update_tooltip_visibility(now + 1000, position, scale);
		now += 2000;
	};
	for (const auto scale : {1, 2})
	{
		frame->parent_bounds = {20 * scale, 70 * scale, 1020 * scale, 770 * scale};
		for (const auto origin : {pointi{300 * scale, 180 * scale}, pointi{-1800 * scale, 40 * scale}})
		{
			frame->screen_origin = origin + frame->parent_bounds.top_left();
			for (const auto anchor :
			     {recti(12, 150, 28, 158), recti(12, 210, 280, 238), recti(12, 20, 280, 100), recti(12, 300, 280, 450)})
			{
				const recti local{anchor.left * scale, anchor.top * scale - 37 * scale, anchor.right * scale,
				                  anchor.bottom * scale - 37 * scale};
				const auto expected = local.offset(frame->screen_origin);
				for (const auto fallback : {false, true})
				{
					const auto controller = std::make_shared<popup_controller>(sidebar, local);
					app->_view_frame->_active_controller = fallback ? nullptr : controller;
					sidebar->_active_controller = fallback ? controller : nullptr;
					app->update_tooltip();
					if (first_hover)
					{
						assert_equal(false, bubble->shown,
						             "a bubble waits for half a second of near-stillness before appearing");
						first_hover = false;
					}
					assert_equal(expected.left, app->_hover.window_bounds.left,
					             "sidebar bubbles use the client-to-screen origin, not parent-relative window bounds");
					assert_equal(expected.top, app->_hover.window_bounds.top,
					             "sidebar bubble vertical coordinates are screen-relative");
					finish_dwell(expected.center(), scale);
					assert_equal(true, bubble->shown && bubble->anchor == expected,
					             "the actual popup receives the screen anchor");
					const auto& host = fallback ? std::static_pointer_cast<view_host>(sidebar)
					                            : std::static_pointer_cast<view_host>(app->_view_frame);
					assert_equal(true, host->_tooltip_bounds == local,
					             "hover tracking remains in the host's client coordinates");
				}
			}
			app->_view_frame->_active_controller.reset();
			sidebar->_active_controller.reset();
			app->_app_logo->bounds = {10, 4, 110, 30};
			app->_app_logo_hover = true;
			app->update_tooltip();
			finish_dwell(app->_app_logo->bounds.offset(frame->screen_origin).center(), scale);
			assert_equal(true, bubble->anchor == app->_app_logo->bounds.offset(frame->screen_origin),
			             "title-bar bubbles use their frame's client-to-screen conversion too");
			app->_app_logo_hover = false;
			app->_status_hover = app_frame::status_part::totals;
			app->_totals_bounds = {600, 4, 760, 30};
			app->_totals_text = "Total";
			app->update_tooltip();
			finish_dwell(app->_totals_bounds.offset(frame->screen_origin).center(), scale);
			assert_equal(true, bubble->anchor == app->_totals_bounds.offset(frame->screen_origin),
			             "status-bar bubbles use their frame's client-to-screen conversion too");
			app->_status_hover = app_frame::status_part::none;
		}
	}
}

static void should_wait_for_a_still_pointer_before_showing_bubbles()
{
	const auto help = setting.show_help_tooltips;
	const auto debug_info = setting.show_debug_info;
	const auto active = ui::command_active.load();
	const auto animations = render::animations_enabled;
	const core::scope_exit restore([=]
	{
		setting.show_help_tooltips = help;
		setting.show_debug_info = debug_info;
		ui::command_active = active;
		render::animations_enabled = animations;
	});
	setting.show_help_tooltips = true;
	setting.show_debug_info = false;
	ui::command_active = 0;
	const auto app = std::make_shared<app_frame>();
	const auto bubble = std::make_shared<tooltip_test_bubble>();
	app->_bubble = bubble;
	app->_app_logo->bounds = {10, 10, 110, 40};
	app->_app_logo_hover = true;
	app->update_tooltip();
	assert_equal(false, bubble->shown, "a new hover does not show a bubble immediately");
	render::animations_enabled = false;
	const auto sample = [&](const uint64_t now, const pointi position, const bool visible, const double scale = 1.0)
	{
		app->update_tooltip_visibility(now, position, scale);
		assert_equal(visible, bubble->shown, std::format("bubble visibility at {}ms", now));
	};
	sample(0, {40, 30}, false);
	sample(499, {43, 27}, false);
	sample(500, {42, 30}, true);
	const auto shows = bubble->shows;
	sample(501, {42, 30}, true);
	assert_equal(shows, bubble->shows, "idle checks do not repeatedly lay out a visible bubble");
	app->update_tooltip();
	assert_equal(true, bubble->shown, "a thumbnail or content refresh does not restart the hover delay");
	sample(502, {42, 30}, true);
	assert_equal(shows + 1, bubble->shows, "fresh content is shown without a second dwell");

	sample(600, {47, 30}, false);
	sample(1099, {49, 30}, false);
	sample(1100, {50, 30}, true);
	app->_app_logo->bounds = app->_app_logo->bounds.offset(0, 20);
	app->update_tooltip();
	assert_equal(false, bubble->shown, "moving to another target hides the old bubble");
	sample(3000, {100, 100}, false);
	sample(3200, {103, 100}, false);
	sample(3350, {106, 100}, false);
	sample(3650, {109, 100}, false);
	sample(3849, {109, 100}, false);
	sample(3850, {109, 100}, true);

	app->_app_logo->bounds = app->_app_logo->bounds.offset(0, 20);
	app->update_tooltip();
	sample(5000, {109, 100}, false);
	sample(5499, {109, 100}, false);
	sample(5500, {109, 100}, true);
	app->_app_logo_hover = false;
	app->update_tooltip();
	sample(7000, {109, 100}, false);
	app->_app_logo_hover = true;
	app->update_tooltip();
	sample(8000, {109, 100}, false);
	sample(8499, {109, 100}, false);
	sample(8500, {109, 100}, true);
	ui::command_active = 1;
	sample(9500, {109, 100}, false);
	ui::command_active = 0;
	app->update_tooltip();
	sample(10000, {109, 100}, false);
	sample(10499, {109, 100}, false);
	sample(10500, {109, 100}, true);

	app->_app_logo->bounds = app->_app_logo->bounds.offset(0, 20);
	app->update_tooltip();
	sample(12000, {0, 0}, false, 2.0);
	sample(12499, {8, -8}, false, 2.0);
	sample(12500, {8, -8}, true, 2.0);
	sample(13100, {9, 0}, false, 2.0);
	sample(13600, {9, 0}, true, 2.0);

	struct pointer_frame final : ui::null_frame
	{
		pointi pointer{50, 20};
		bool is_visible() const override { return true; }
		pointi cursor_location() override { return pointer; }
		pointi client_to_screen(const pointi loc) const override { return loc + pointi{600, 400}; }
		double scale_factor() const override { return 2.0; }
	};
	const auto frame = std::make_shared<pointer_frame>();
	app->_app_frame = frame;
	app->_startup_settled = true;
	app->_invalids = view_invalid::none;
	app->_app_logo->bounds = app->_app_logo->bounds.offset(0, 20);
	app->update_tooltip();
	app->idle();
	assert_equal(true, app->_tooltip_dwell.armed, "the real idle path advances a pending bubble without mouse events");
	assert_equal(true, app->_tooltip_dwell.origin == frame->client_to_screen(frame->pointer),
	             "idle samples the pointer in screen coordinates");
	assert_equal(false, bubble->shown, "the real idle path also observes the initial delay");
	frame->pointer.x += 9;
	app->idle();
	assert_equal(true, app->_tooltip_dwell.origin == frame->client_to_screen(frame->pointer),
	             "the real idle path detects movement outside the scaled tolerance");
}

static void should_follow_pointer_with_fast_hover_bubbles()
{
	struct preview_controller final : view_controller
	{
		recti anchor{10, 10, 30, 30};
		uint32_t delay = 0;

		explicit preview_controller(const view_host_ptr& host) : view_controller(host, {}) {}

		void popup_from_location(view_hover_element& hover) override
		{
			hover.elements->add(std::make_shared<text_element>("Preview"));
			hover.active_bounds = hover.window_bounds = anchor;
			hover.active_delay = delay;
		}
	};

	const auto help = std::exchange(setting.show_help_tooltips, true);
	const core::scope_exit restore([help] { setting.show_help_tooltips = help; });
	const auto app = std::make_shared<app_frame>();
	const auto bubble = std::make_shared<tooltip_test_bubble>();
	app->_bubble = bubble;
	const auto preview = std::make_shared<preview_controller>(app->_view_frame);
	const core::scope_exit clear_controller([&] { app->_view_frame->_active_controller.reset(); });
	const auto sample = [&](const uint64_t now, const bool visible)
	{
		app->update_tooltip_visibility(now, preview->anchor.center(), 1.0);
		assert_equal(visible, bubble->shown, std::format("bubble visibility at {} ms", now));
		if (visible) assert_equal(true, preview->anchor == bubble->anchor, "the bubble follows the current target");
	};

	app->_view_frame->_active_controller = preview;
	app->update_tooltip();
	sample(0, true);
	preview->anchor = preview->anchor.offset(0, 40);
	app->update_tooltip();
	sample(1, true);

	preview->delay = bubble_delay::sidebar_entry;
	app->update_tooltip();
	sample(100, false);
	preview->anchor = preview->anchor.offset(0, 40);
	app->update_tooltip();
	sample(249, false);
	sample(250, true);
	preview->anchor = preview->anchor.offset(0, 40);
	app->update_tooltip();
	assert_equal(true, bubble->shown, "the bubble stays visible between sidebar rows");
	sample(251, true);

	app->_view_frame->_active_controller.reset();
	app->update_tooltip();
	sample(252, false);
	app->_view_frame->_active_controller = preview;
	app->update_tooltip();
	sample(300, false);
	sample(450, true);

	preview->delay = bubble_delay::scrollbar_entry;
	app->update_tooltip();
	sample(500, false);
	sample(549, false);
	sample(550, true);
	preview->anchor = preview->anchor.offset(0, 40);
	app->update_tooltip();
	sample(551, true);

	preview->delay = bubble_delay::face_preview;
	app->update_tooltip();
	sample(600, false);
	preview->anchor = preview->anchor.offset(0, 40);
	app->update_tooltip();
	sample(799, false);
	sample(998, false);
	sample(999, true);

	app->_view_frame->_active_controller.reset();
	app->_app_logo->bounds = preview->anchor;
	app->_app_logo_hover = true;
	app->update_tooltip();
	sample(1000, false);
	sample(1499, false);
	sample(1500, true);
}

// The plan clears a destination held by a file the same run renames away, so the run has to free it
// rather than fail on it. Every row here wants the next row's current name.
static void should_run_a_rename_onto_vacated_names()
{
	const auto root = test_temps.next_folder("rename-run-chain");
	std::map<std::string, std::string> before;
	for (const auto& name : {"Item 001.jpg"s, "Item 002.jpg"s, "Item 003.jpg"s})
	{
		const auto path = root.combine_file(name);
		platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), path, false, false);
		{
			std::ofstream fs(platform::to_stream_path(path), std::ios::binary | std::ios::app);
			fs << name;
		}
		before.emplace(name, read_test_file(path));
	}

	browsing_fixture f(root);
	f.state.select_all(f.view);
	f.state.update_selection();
	assert_equal(3_z, f.state.selected_items().size(), "the fixture folder is selected");

	const auto saved = setting.rename;
	const core::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "Item ###";
	setting.rename.start_seq = "2";
	setting.rename.collision = collision_policy::block_run;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "a chained rename can run");
	view->run();

	// The finished run replaces the plan's rows and status with what it did.
	assert_equal(true, view->showing_results(), "the rows show the run, not the plan");
	assert_equal(false, view->status().empty() || view->status() == tt.processing.sv(),
	             "the status states the run's outcome");

	// Every reviewed row stays as it was reviewed, its old name beside the new one, and a Status column after them
	// says what the run did to it.
	assert_equal(4, view->column_count(), "a Status column follows the review's three");
	assert_equal(tt.status.sv(), view->column_titles()[3].sv(), "under its own heading");
	assert_equal(3_z, view->_rows.size(), "every reviewed row stays");
	for (const auto& row : view->_rows)
	{
		assert_equal(true, row->_text[0].starts_with("Item 00") && row->_text[2].starts_with("Item 00"),
		             "each row keeps its old and new names");
		assert_equal(tt.result_success.sv(), row->_text[3], row->_text[0], "and says the rename succeeded");
	}

	const auto completion_status = std::string(view->status());
	view->refresh_from_source();
	assert_equal(completion_status, std::string(view->status()), "the file notification retains the real run summary");
	assert_equal(true, view->showing_results(), "completed rows survive their own file notifications");

	assert_equal(false, platform::exists(root.combine_file("Item 001.jpg")), "the vacated name is gone");
	for (const auto& name : {"Item 002.jpg"s, "Item 003.jpg"s, "Item 004.jpg"s})
		assert_equal(true, platform::exists(root.combine_file(name)), std::format("{} was written", name));
	assert_equal(before["Item 001.jpg"], read_test_file(root.combine_file("Item 002.jpg")),
	             "the first source moved to its reviewed destination");
	assert_equal(before["Item 002.jpg"], read_test_file(root.combine_file("Item 003.jpg")),
	             "the second source moved to its reviewed destination");
	assert_equal(before["Item 003.jpg"], read_test_file(root.combine_file("Item 004.jpg")),
	             "the third source moved to its reviewed destination");

	// Every file the run leaves behind carries a name the review showed. The parked-file cleanup
	// has a last-resort " (n)" variant for when a file can reach neither its own name nor its
	// reviewed destination; nothing in a chain that completes should ever reach it, and a file
	// under a generated name is one the user was never shown and cannot have expected.
	const auto contents = platform::iterate_file_items(root, false);
	assert_equal(3_z, contents.files.size(), "the folder holds exactly the three renamed files");

	for (const auto& file : contents.files)
	{
		assert_equal(true, std::string_view(file.name).find(" (") == std::string_view::npos,
		             std::format("{} is a reviewed name, not a generated variant", file.name));
	}
}

static void should_recover_canceled_rename_sidecars_as_a_group()
{
	for (const auto& [block_reviewed, block_numbered, first_has_sidecar] :
	     {std::tuple{true, 0, true}, {true, 1, true}, {false, 0, true}, {true, 0, false}, {true, 2, true}})
	{
		const auto root = test_temps.next_folder("rename-cancel-group");
		const auto first = root.combine_file("1.jpg");
		const auto second = root.combine_file("2.jpg");
		const auto third = root.combine_file("3.jpg");
		const auto source_bytes = read_test_file(test_files_folder().combine_file("Test.jpg"));
		write_test_file(first, source_bytes + "first");
		write_test_file(second, source_bytes + "second");
		if (first_has_sidecar) write_test_file(first.extension(".xmp"), "first metadata");
		write_test_file(second.extension(".xmp"), "second metadata");
		if (block_reviewed) write_test_file(third, source_bytes + "unrelated");
		if (block_numbered == 1) write_test_file(root.combine_file("2 (2).xmp"), "unrelated sidecar");

		browsing_fixture f(root);
		f.state.select(f.view, f.find("1.jpg"), false, false, false);
		f.state.select(f.view, f.find("2.jpg"), true, false, false);
		f.state.update_selection();
		assert_equal(2_z, f.state.selected_count(), "only the two source media are selected");
		assert_equal(true, core::contains(f.find("2.jpg")->sidecars(), "2.xmp"),
		             "the unfinished row carries its automatic companion");

		const auto saved = setting.rename;
		const core::scope_exit restore([saved] { setting.rename = saved; });
		setting.rename.name_template = "#";
		setting.rename.start_seq = "2";
		setting.rename.collision = collision_policy::replace;

		const auto view = std::make_shared<rename_view>(f.state, nullptr);
		view->activate({100, 100});
		assert_equal(true, view->can_run(), "the reviewed chain can run");
		df::rename_after_move_for_test =
			[first, second, root, block_numbered, &view](const core::file_path from, const core::file_path to)
		{
			if (from == first && to == second) view->cancel_operation();
			if (block_numbered == 2 && to == root.combine_file("2 (2).jpg"))
				write_test_file(root.combine_file("2 (2).xmp"), "unrelated sidecar");
		};
		const core::scope_exit restore_hook([] { df::rename_after_move_for_test = {}; });
		view->run();

		const auto recovered = !block_reviewed ? third : root.combine_file(block_numbered ? "2 (3).jpg" : "2 (2).jpg");
		assert_equal(source_bytes + "first", read_test_file(second), "the completed rename stays completed");
		if (first_has_sidecar)
			assert_equal("first metadata"s, read_test_file(second.extension(".xmp")),
			             "the completed media keeps its own metadata");
		else
			assert_equal(false, platform::exists(second.extension(".xmp")),
			             "an unparked companion must not stay beside the completed primary");
		assert_equal(source_bytes + "second", read_test_file(recovered), "the parked primary is recovered");
		assert_equal("second metadata"s, read_test_file(recovered.extension(".xmp")),
		             "its parked sidecar uses the same recovered basename");
		if (block_reviewed)
		{
			assert_equal(source_bytes + "unrelated", read_test_file(third), "the unrelated primary is untouched");
			assert_equal(false, platform::exists(third.extension(".xmp")), "it acquires no recovered metadata");
		}
		if (block_numbered)
		{
			assert_equal("unrelated sidecar"s, read_test_file(root.combine_file("2 (2).xmp")),
			             "a numbered sidecar collision advances the whole group");
			assert_equal(false, platform::exists(root.combine_file("2 (2).jpg")),
			             "a partially recovered primary is rolled back before trying the next basename");
		}
	}
}

// The plan records each file's identity (see should_record_rename_identity); this is the run holding
// itself to it. A source rewritten between the review and the run still exists, which is all the run
// used to ask, but it is not the file that was reviewed - so nothing is renamed.
static void should_refuse_a_rename_whose_source_changed_since_review()
{
	const auto root = test_temps.next_folder("rename-identity-run");
	const auto source = root.combine_file("alpha.jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);

	browsing_fixture f(root);
	const auto item = f.find("alpha.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved = setting.rename;
	const core::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "renamed";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::block_run;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "the rename can run");

	write_test_file(source, "rewritten since the review");
	view->run();

	assert_equal(true, platform::exists(source), "the changed source keeps its name");
	assert_equal(false, platform::exists(root.combine_file("renamed.jpg")), "and nothing was renamed");
	// A run that did nothing says why, rather than leaving the processing status behind.
	assert_equal(std::string(tt.sync_analysis_changed.sv()), std::string(view->status()),
	             "the status names the stale review");
}

// Releasing the displayed file's handles pumps messages, so a Close can be handled while Run is still
// setting out. Run then went ahead with nothing on screen to show it, renaming files after the user had
// left the view.
static void should_not_run_a_rename_closed_while_releasing_handles()
{
	const auto root = test_temps.next_folder("rename-closed-during-detach");
	const auto source = root.combine_file("alpha.jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);

	browsing_fixture f(root);
	const auto item = f.find("alpha.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved = setting.rename;
	const core::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "renamed";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::block_run;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "the rename can run");

	const core::scope_exit clear_hook([] { test_during_detach_file_handles = {}; });
	test_during_detach_file_handles = [&view]
	{
		view->deactivate();
	};
	view->run();

	assert_equal(true, platform::exists(source), "the source keeps its name");
	assert_equal(false, platform::exists(root.combine_file("renamed.jpg")),
	             "nothing is renamed once the view has closed");
	assert_equal(false, view->progress().active, "and no run is left showing progress");
}

static void should_keep_completed_run_results_on_source_refresh()
{
	browsing_fixture fixture;
	const std::vector<std::shared_ptr<review_list>> views{std::make_shared<rename_view>(fixture.state, nullptr),
	                                                      std::make_shared<batch_tool_view>(fixture.state, nullptr)};

	for (const auto& view : views)
	{
		view->show_results({{"completed.jpg", item_status::success}});
		const auto status = std::string(view->status());
		view->refresh_from_source();
		assert_equal(true, view->showing_results(), "source changes retain completed results");
		assert_equal(status, std::string(view->status()), "source changes preserve the completion summary");
		// F5 reaches the view as reload() and then the same invalidation a file notification raises.
		view->reload();
		view->refresh_from_source();
		assert_equal(false, view->showing_results(), "an explicit Refresh re-plans after a completed run");
		view->show_results({{"completed.jpg", item_status::success}});
		view->refresh_from_source();
		assert_equal(true, view->showing_results(), "a Refresh is honoured once, not remembered");
		view->refresh();
		assert_equal(false, view->showing_results(), "explicit refresh leaves results for a fresh review");
	}
}

// Convert and Rename work their review out on a worker, off the progress a run reports. An agent's task_set
// answered while it was still to come, with Analyzing on the status line and the run refused, and
// wait_until_idle had nothing to wait for: each asks the view whether its review is still being worked out.
static void should_say_while_a_review_is_still_being_worked_out()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	state.view_mode(view_type::items);

	const auto convert = std::make_shared<batch_tool_view>(state, nullptr);
	convert->mode(batch_tool_mode::convert);
	const auto rename = std::make_shared<rename_view>(state, nullptr);

	const std::vector<std::shared_ptr<review_list>> views{convert, rename};

	for (const auto& view : views)
	{
		const auto name = std::string(view->operation_name());
		view->activate({100, 100});
		assert_equal(true, view->reviewing(), name + "'s review is worked out once it is shown");

		while (as.run_next(async_queue::work)) as.drain_ui();
		assert_equal(false, view->reviewing(), name + "'s review has been worked out");

		view->refresh();
		assert_equal(true, view->reviewing(), "a change sets " + name + "'s working out again");
		view->deactivate();
		assert_equal(false, view->reviewing(), "and one closed works out nothing");
		as.drain_ui();
	}
}

// Every task view ends a run the same way: what it did, then why some rows did nothing. Rename used to
// drop the counts whenever there was a reason, and Tags kept "Processing" when there was neither.
static void should_conclude_a_run_with_what_it_did_and_why()
{
	assert_equal("2 renamed"s, review_list::conclude_run("2 renamed", {}, "review"), "the counts alone");
	assert_equal("stale"s, review_list::conclude_run({}, "stale", "review"), "the reason alone");
	assert_equal("2 renamed  stale"s, review_list::conclude_run("2 renamed", "stale", "review"),
	             "a partial run states both");
	assert_equal("review"s, review_list::conclude_run({}, {}, "review"), "nothing to report keeps the review");
}

// Convert under Replace wrote over a colliding destination whatever had happened to it since the
// review. Replace was agreed to for the file the review showed, and one edited since is not that
// file - the same test Import, Sync and Rename hold a replaced destination to.
static void should_not_convert_over_a_destination_changed_since_review()
{
	const auto root = test_temps.next_folder("convert-replace-identity");
	const auto sources = root.combine("sources");
	const auto output = root.combine("output");
	platform::create_folder(sources);
	platform::create_folder(output);

	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), sources.combine_file("photo.jpg"), false,
	                    false);
	const auto destination = output.combine_file("photo.jpg");
	write_test_file(destination, "reviewed");

	browsing_fixture f(sources);
	const auto item = f.find("photo.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved_convert = setting.convert;
	const auto saved_folder = setting.write_folder;
	const core::scope_exit restore([saved_convert, saved_folder]
	{
		setting.convert = saved_convert;
		setting.write_folder = saved_folder;
	});

	setting.convert = {};
	setting.convert.to_jpeg = true;
	setting.convert.jpeg_quality = 90;
	setting.convert.collision = collision_policy::replace;
	setting.write_folder = std::string(output.text());

	const auto view = std::make_shared<batch_tool_view>(f.state, nullptr);
	view->mode(batch_tool_mode::convert);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "a replacing convert can run");

	write_test_file(destination, "edited since the review");
	view->run();

	assert_equal("edited since the review"s, read_test_file(destination), "the edited destination is left alone");
}

// Convert writes an AVIF as it writes the other formats: under its own extension, with the AVIF settings it shows -
// lossless here, which an AVIF holds as RGB itself.
static void should_convert_to_avif_with_its_settings()
{
	const auto root = test_temps.next_folder("convert-avif");
	const auto sources = root.combine("sources");
	const auto output = root.combine("output");
	platform::create_folder(sources);
	platform::create_folder(output);
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), sources.combine_file("photo.jpg"), false,
	                    false);

	browsing_fixture f(sources);
	const auto item = f.find("photo.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved_convert = setting.convert;
	const auto saved_folder = setting.write_folder;
	const core::scope_exit restore([saved_convert, saved_folder]
	{
		setting.convert = saved_convert;
		setting.write_folder = saved_folder;
	});

	setting.convert = {};
	setting.convert.to_avif = true;
	setting.convert.avif_quality = 75;
	setting.convert.avif_lossless = true;
	setting.write_folder = std::string(output.text());

	const auto view = std::make_shared<batch_tool_view>(f.state, nullptr);
	view->mode(batch_tool_mode::convert);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "the convert can run");
	view->run();

	const auto destination = output.combine_file("photo.avif");
	assert_equal(true, platform::exists(destination), "written under .avif");

	files ff;
	const auto converted = ff_scan_file(ff, destination);
	assert_equal(true, converted.format == libfiles::image_format::heif, "as an AVIF");
	assert_equal(true, converted.picture.chroma == libfiles::chroma_format::rgb, "lossless, as the settings ask");
}

// Edit metadata and Adjust Date write each row through one runner - the edits every row shares, or each row's own
// date - and each run is one recent change.
static void should_write_metadata_and_dates_in_a_batch()
{
	const auto root = test_temps.next_folder("batch-updates");
	const auto photo = root.combine_file("photo.jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), photo, false, false);

	browsing_fixture f(root);
	const auto item = f.find("photo.jpg");
	assert_equal(true, item != nullptr, "the photo is listed");
	if (!item) return;
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved_set_caption = setting.set_caption;
	const auto saved_caption = setting.caption;
	const core::scope_exit restore([saved_set_caption, saved_caption]
	{
		setting.set_caption = saved_set_caption;
		setting.caption = saved_caption;
	});
	const std::string caption = "A walk by the sea";
	setting.set_caption = true;
	setting.caption = caption;

	const auto metadata = std::make_shared<batch_tool_view>(f.state, nullptr);
	metadata->mode(batch_tool_mode::metadata);
	metadata->activate({100, 100});
	assert_equal(true, metadata->can_run(), "a caption can be written");
	metadata->run();
	assert_equal(caption, extract_properties(photo)->description, "the caption is written");
	assert_equal(std::string(tt.command_edit_metadata.sv()), f.state.session_changes().front().what, "as a change");

	setting.set_caption = false;
	const core::date_t moved_to(1985, 7, 14, 10, 30, 0);
	const auto dates = std::make_shared<batch_tool_view>(f.state, nullptr);
	dates->mode(batch_tool_mode::adjust_date);
	dates->activate({100, 100});
	dates->new_start_for_tests(moved_to);
	dates->refresh();
	assert_equal(true, dates->can_run(), "the date can be moved");
	dates->run();
	assert_equal(true, extract_properties(photo)->created() == moved_to, "the date is moved");
	assert_equal(std::string(tt.command_adjust_date.sv()), f.state.session_changes().front().what, "as another change");
	assert_equal(true, f.state.session_changes().front().paths == std::vector{photo}, "of the photo");
}

static void should_refuse_convert_destination_claimed_during_staged_publication()
{
	const auto root = test_temps.next_folder("convert-publish-claimed");
	const auto source = root.combine_file("photo.jpg");
	const auto destination = root.combine_file("converted.jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);

	test_before_file_update_publish = [destination](const core::file_path path)
	{
		if (path == destination) write_test_file(destination, "claimed during publication");
	};
	const core::scope_exit restore_hook([] { test_before_file_update_publish = {}; });

	files ff;
	file_encode_params params;
	params.jpeg_save_quality = 90;
	file_update_publish publish;
	publish.fail_if_exists = true;
	const auto result = ff.update(source, destination, {}, image_edits(16), params, false, {}, {}, {}, publish);

	assert_equal(true, result.code == platform::file_op_result_code::ALREADY_EXISTS,
	             "the final publication refuses a claimed destination");
	assert_equal("claimed during publication"s, read_test_file(destination), "the claimed file survives");
	assert_equal(true, platform::exists(source), "the source survives the refused convert");
}

static void should_refuse_convert_replacement_changed_during_staged_publication()
{
	const auto root = test_temps.next_folder("convert-publish-replace-changed");
	const auto source = root.combine_file("photo.jpg");
	const auto destination = root.combine_file("converted.jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);
	write_test_file(destination, "reviewed destination");
	const auto reviewed = platform::file_attributes(destination);

	test_before_file_update_publish = [destination](const core::file_path path)
	{
		if (path == destination) write_test_file(destination, "changed during publication");
	};
	const core::scope_exit restore_hook([] { test_before_file_update_publish = {}; });

	files ff;
	file_encode_params params;
	params.jpeg_save_quality = 90;
	file_update_publish publish;
	publish.require_unchanged_destination = true;
	publish.destination_attributes = reviewed;
	const auto result = ff.update(source, destination, {}, image_edits(16), params, false, {}, {}, {}, publish);

	assert_equal(true, result.code == platform::file_op_result_code::ALREADY_EXISTS,
	             "the final publication refuses a changed reviewed replacement");
	assert_equal("changed during publication"s, read_test_file(destination), "the changed destination survives");
	assert_equal(true, platform::exists(source), "the source survives the refused replacement");
}

#ifdef _WIN32
static void should_refuse_convert_replacement_locked_during_staged_publication()
{
	const auto root = test_temps.next_folder("convert-publish-replace-locked");
	const auto source = root.combine_file("photo.jpg");
	const auto destination = root.combine_file("converted.jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);
	write_test_file(destination, "reviewed destination");
	const auto reviewed = platform::file_attributes(destination);

	platform::file_ptr locked;
	test_before_file_update_publish = [destination, &locked](const core::file_path path)
	{
		if (path == destination) locked = platform::open_file(destination, platform::file_open_mode::read_write);
	};
	const core::scope_exit restore_hook([&locked]
	{
		locked.reset();
		test_before_file_update_publish = {};
	});

	files ff;
	file_encode_params params;
	params.jpeg_save_quality = 90;
	file_update_publish publish;
	publish.require_unchanged_destination = true;
	publish.destination_attributes = reviewed;
	const auto result = ff.update(source, destination, {}, image_edits(16), params, false, {}, {}, {}, publish);

	assert_equal(true, result.code == platform::file_op_result_code::ALREADY_EXISTS,
	             "the platform replace refuses a locked reviewed replacement");
	assert_equal(false, result.error_message.empty(), "the refusal is actionable");
	locked.reset();
	assert_equal("reviewed destination"s, read_test_file(destination), "the locked destination survives");
}

// A rename is a move, so once a sidecar has landed on a Replace destination that destination's
// former content is gone whether or not the row completes. Leaving the sidecar there after the
// primary fails strands it away from the file it describes and empties the source of a group the
// run promised to leave whole; putting it back destroys nothing further. The failure is staged by
// holding the destination open, which denies the move only where locking is mandatory.
static void should_restore_a_replaced_sidecar_when_the_rename_fails()
{
	const auto root = test_temps.next_folder("rename-replace-rollback");
	const auto source = root.combine_file("a.jpg");
	const auto source_sidecar = root.combine_file("a.xmp");
	const auto destination = root.combine_file("b.jpg");
	const auto destination_sidecar = root.combine_file("b.xmp");

	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);
	write_test_file(source_sidecar, "source sidecar");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), destination, false, false);
	write_test_file(destination_sidecar, "destination sidecar");

	browsing_fixture f(root);
	const auto item = f.find("a.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();
	assert_equal(1_z, f.state.selected_count(), "only the source is selected");

	const auto saved = setting.rename;
	const core::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "b";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::replace;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "a replacing rename can run");

	// Denies the primary move while leaving the sidecar's destination writable, so the row fails
	// after its sidecar has already been replaced.
	auto locked = platform::open_file(destination, platform::file_open_mode::read_write);
	assert_equal(true, locked != nullptr, "the destination is held open");

	view->run();
	locked.reset();

	assert_equal(true, platform::exists(source), "the primary is left at its source");
	assert_equal(true, platform::exists(source_sidecar), "and so is the sidecar that had already moved");
	assert_equal("source sidecar"s, read_test_file(source_sidecar), "the recovered source sidecar keeps its own bytes");
}

static void should_report_batch_rename_sidecar_rollback_survivor()
{
	const auto root = test_temps.next_folder("rename-sidecar-rollback-survivor");
	const auto source = root.combine_file("a.jpg");
	const auto source_sidecar = root.combine_file("a.xmp");
	const auto destination = root.combine_file("b.jpg");
	const auto destination_sidecar = root.combine_file("b.xmp");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);
	write_test_file(source_sidecar, "source sidecar");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), destination, false, false);
	write_test_file(destination_sidecar, "destination sidecar");

	df::rename_after_move_for_test =
		[source_sidecar, destination_sidecar](const core::file_path moved_from, const core::file_path moved_to)
	{
		if (moved_from == source_sidecar && moved_to == destination_sidecar)
		{
			write_test_file(source_sidecar, "rollback blocker");
		}
	};
	const core::scope_exit restore_hook([] { df::rename_after_move_for_test = {}; });

	browsing_fixture f(root);
	const auto item = f.find("a.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved = setting.rename;
	const core::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "b";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::replace;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	auto locked = platform::open_file(destination, platform::file_open_mode::read_write);
	assert_equal(true, locked != nullptr, "the destination is held open");
	view->run();
	locked.reset();

	assert_equal(true, platform::exists(destination_sidecar), "the moved sidecar remains at its surviving path");
	assert_equal(true, core::contains(view->status(), destination_sidecar.str()),
	             "the status names the sidecar survivor");
}

static void should_report_batch_rename_case_only_recovery_failure()
{
	const auto root = test_temps.next_folder("rename-case-recovery-fail");
	const auto source = root.combine_file("photo.jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);

	core::file_path surviving;
	df::rename_after_move_for_test =
		[source, &surviving](const core::file_path moved_from, const core::file_path moved_to)
	{
		if (moved_from == source && moved_to != source)
		{
			surviving = moved_to;
			write_test_file(source, "rollback blocker");
		}
	};
	const core::scope_exit restore_hook([] { df::rename_after_move_for_test = {}; });

	browsing_fixture f(root);
	f.state.select_all(f.view);
	f.state.update_selection();

	const auto saved = setting.rename;
	const core::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "PHOTO";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::block_run;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "a case-only rename can run");
	view->run();

	assert_equal(true, platform::exists(surviving), "the temporary surviving path is kept");
	assert_equal("rollback blocker"s, read_test_file(source), "the blocker remains at the reviewed path");
	assert_equal(true, core::contains(view->status(), surviving.str()), "the status names the surviving path");
}

static void should_report_singleton_rename_case_only_rollback_failure()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto source = test_temps.next_path(".jpg");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), source, false, false);
	const auto item = load_item(index, source, false);
	auto upper_name = core::to_upper(std::string(source.file_name_without_extension()));
	if (upper_name == source.file_name_without_extension()) upper_name = core::to_lower(upper_name);

	core::file_path surviving;
	df::rename_after_move_for_test =
		[source, &surviving](const core::file_path moved_from, const core::file_path moved_to)
	{
		if (moved_from == source && moved_to != source)
		{
			surviving = moved_to;
			write_test_file(source, "singleton rollback blocker");
		}
	};
	const core::scope_exit restore_hook([] { df::rename_after_move_for_test = {}; });

	const auto result = item->rename(index, upper_name);
	assert_equal(true, result.failed(), "the blocked rollback reports failure");
	assert_equal(true, platform::exists(surviving), "the temporary surviving path is kept");
	assert_equal("singleton rollback blocker"s, read_test_file(source), "the blocker remains at the reviewed path");
	assert_equal(true, core::contains(result.format_error(), surviving.str()), "the error names the surviving path");
}
#endif

static void should_continue_singleton_rename_sidecar_rollbacks_after_failure()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto primary = test_temps.next_path(".jpg");
	const auto sidecar_xmp = primary.extension(".xmp");
	const auto sidecar_dop = primary.extension(".dop");
	const auto sidecar_on1 = primary.extension(".on1");
	const auto target_base = std::string(primary.file_name_without_extension()) + "-renamed";
	const auto target_primary = core::file_path(primary.folder(), target_base, ".jpg");
	const auto target_xmp = core::file_path(primary.folder(), target_base, ".xmp");
	const auto target_dop = core::file_path(primary.folder(), target_base, ".dop");
	const auto target_on1 = core::file_path(primary.folder(), target_base, ".on1");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), primary, false, false);
	write_test_file(sidecar_xmp, "xmp");
	write_test_file(sidecar_dop, "dop");
	write_test_file(sidecar_on1, "on1");
	write_test_file(target_primary, "blocked primary");

	std::vector<std::string> reports;
	df::rename_after_move_for_test = [sidecar_dop, target_dop, sidecar_on1,
	                                  target_on1](const core::file_path moved_from, const core::file_path moved_to)
	{
		if (moved_from == sidecar_dop && moved_to == target_dop)
		{
			write_test_file(sidecar_dop, "dop rollback blocker");
		}
		if (moved_from == sidecar_on1 && moved_to == target_on1)
		{
			write_test_file(sidecar_on1, "on1 rollback blocker");
		}
	};
	df::rename_recovery_report_for_test = [&reports](const std::string_view message)
	{
		reports.emplace_back(message);
	};
	const core::scope_exit restore_hook([]
	{
		df::rename_after_move_for_test = {};
		df::rename_recovery_report_for_test = {};
	});

	df::index_file_item info;
	info.name = primary.name();
	info.ft = files::file_type_from_name(primary);
	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = core::cache(
		std::format("{};{};{}", sidecar_xmp.name(), sidecar_dop.name(), sidecar_on1.name()));
	info.metadata.store(metadata);
	const auto item = std::make_shared<df::item_element>(primary, info);

	const auto result = item->rename(index, target_base);
	assert_equal(true, result.failed(), "the rename reports the rollback failure");
	assert_equal(true, core::contains(result.format_error(), target_on1.str()), "the error names the first survivor");
	assert_equal(true, platform::exists(target_dop), "the second failed rollback sidecar remains recoverable");
	assert_equal(true, platform::exists(target_on1), "the first failed rollback sidecar remains recoverable");
	assert_equal(true,
	             std::ranges::any_of(reports, [&](const std::string& message)
	{ return core::contains(message, target_dop.str()); }),
	             "the second survivor is logged");
	assert_equal(true,
	             std::ranges::any_of(reports, [&](const std::string& message)
	{ return core::contains(message, target_on1.str()); }),
	             "the first survivor is logged");
	assert_equal(false, platform::exists(target_xmp),
	             "the successfully rolled-back sidecar leaves no destination copy");
	assert_equal(true, platform::exists(target_dop), "the failed rollback sidecar remains recoverable");
}

// A case-only rename frees nothing, so the run must not park the file it is renaming within.
static void should_run_a_case_only_rename()
{
	const auto root = test_temps.next_folder("rename-run-case");
	platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), root.combine_file("photo.jpg"), false, false);

	browsing_fixture f(root);
	f.state.select_all(f.view);
	f.state.update_selection();

	const auto saved = setting.rename;
	const core::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "PHOTO";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::block_run;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "a case-only rename can run");
	view->run();

	const auto contents = platform::iterate_file_items(root, false);
	assert_equal(1_z, contents.files.size(), "the folder still holds one file");
	assert_equal_strict("PHOTO.jpg", std::string(contents.files.front().name), "the file carries the new case");
}

static void should_run_a_singleton_folder_case_only_rename()
{
	const auto root = test_temps.next_folder("rename-single-folder-case");
	const auto original = root.combine("Photos");
	const auto occupied = root.combine("Taken");
	platform::create_folder(original);
	platform::create_folder(occupied);
	write_test_file(original.combine_file("child.txt"), "child");

	browsing_fixture f(root);
	const auto item = f.find("Photos");
	assert_equal(true, item != nullptr, "the folder is listed");
	assert_equal(true, item->is_folder(), "the listed item is a folder");

	auto result = item->rename(f.index, "photos");
	assert_equal(true, result.success(), "a case-only folder rename succeeds");
	assert_equal(true, platform::exists(root.combine("photos")), "the folder has the requested case");
	assert_equal("child"s, read_test_file(root.combine("photos").combine_file("child.txt")),
	             "children remain under the renamed folder");
	assert_equal("photos", std::string(item->base_name()), "the item name is updated after success");

	result = item->rename(f.index, "photos");
	assert_equal(true, result.success(), "a byte-identical folder rename is still a no-op");

	result = item->rename(f.index, "Taken");
	assert_equal(true, result.failed(), "an occupied distinct destination is still refused");
	assert_equal(true, platform::exists(root.combine("photos")), "the source folder survives the refused rename");
	assert_equal("child"s, read_test_file(root.combine("photos").combine_file("child.txt")),
	             "child content survives the refused rename");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #250 - after a move or a delete the cursor reset to the start of the listing instead of
// settling on what followed the set that left, which turns sorting a folder into a re-navigation
// after every action. Delete and move ask the same question, so they must reuse one answer rather
// than each deriving its own.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_land_on_what_followed_a_removed_set()
{
	browsing_fixture f;

	const auto& groups = f.state.groups();
	assert_equal(true, !groups.empty(), "the fixture folder lists something");

	std::vector<df::item_element_ptr> listed;
	for (const auto& g : groups)
		for (const auto& i : g->items()) listed.emplace_back(i);

	assert_equal(true, listed.size() > 2, "and it lists enough to have a successor");

	// A set in the middle: the cursor settles on the first item after it, not on the first item.
	f.state.select(f.view, listed[1], false, false, false);
	f.state.update_selection();
	assert_equal(true, f.state.next_unselected_item() == listed[2], "the item after the selection follows it");

	// The last item: nothing follows it, so the cursor settles on what came BEFORE the set rather
	// than on the first item of the listing. Asserting only "not null and not the item that left"
	// is satisfied by listed[0], which is the reset this exists to catch.
	f.state.select(f.view, listed.back(), false, false, false);
	f.state.update_selection();

	const auto after_last = f.state.next_unselected_item();
	assert_equal(true, after_last == listed[listed.size() - 2], "a set running to the end settles on what preceded it");

	// Everything selected: nothing survives it, and the caller has to cope with that rather than
	// being handed an item that is about to disappear.
	f.state.select_all(f.view);
	f.state.update_selection();
	assert_equal(true, f.state.next_unselected_item() == nullptr, "a whole listing leaving has no successor");
}

// Issue #265 - in a listing of copies, a copy whose others are all in the set leaves with them: with them gone it has
// no copy left. Settling the cursor on it followed it to the top, where an item without copies sorts, before the
// listing dropped it - so deleting one of a pair lost the user's place. The cursor settles on a copy that stays.
static void should_land_on_a_copy_that_stays_after_deleting_copies()
{
	null_state_strategy ss;
	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const view_host_base_ptr view;
	state.view_mode(view_type::items);
	state.open(view, df::search_t::parse("@duplicates"), {});
	assert_equal(true, state.search().is_duplicates(), "a listing of copies");

	const auto copy = [](const std::string_view name, const uint32_t group, const uint32_t count)
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(indexed.name);
		const auto item = std::make_shared<df::item_element>(core::file_path(std::format("c:\\copies\\{}", name)),
		                                                     indexed);
		item->duplicates(df::duplicate_info{.group = group, .count = count});
		return item;
	};

	// Two pairs either side of a set of three, as a listing of copies orders them: by set, then by name.
	const auto a1 = copy("a1.jpg", 1, 2);
	const auto a2 = copy("a2.jpg", 1, 2);
	const auto b1 = copy("b1.jpg", 2, 3);
	const auto b2 = copy("b2.jpg", 2, 3);
	const auto b3 = copy("b3.jpg", 2, 3);
	const auto c1 = copy("c1.jpg", 3, 2);
	const auto c2 = copy("c2.jpg", 3, 2);
	state.append_items(view, df::item_set({a1, a2, b1, b2, b3, c1, c2}), {}, true, true);

	const auto after_deleting = [&](const df::item_element_ptr& deleted)
	{
		state.select(view, deleted, false, false, false);
		state.update_selection();
		return state.next_unselected_item();
	};

	assert_equal(true, after_deleting(a1) == b1, "one of a pair passes over its partner, which leaves with it");
	assert_equal(true, after_deleting(a2) == b1, "whichever of the two goes");
	assert_equal(true, after_deleting(b1) == b2, "one of three leaves two copies, and the next of them stays");
	assert_equal(true, after_deleting(c1) == b3, "and at the end the cursor settles on the copy before that stays");
}

// What a command deletes or moves away leaves the listing where it stood and the cursor lands on the item after it,
// without the search running again: a search the index answers would still list it, and the listing would not stay
// where the user had it.
static void should_remove_deleted_items_in_place()
{
	browsing_fixture f;
	auto& s = f.state;
	const auto files = f.files();
	assert_equal(true, files.size() > 6, "test folder has enough files");

	const auto listed = s.search_items().size();
	s.select(f.view, files[2], false, false, false);
	s.update_selection();
	s.remove_items(f.view, df::item_set({files[2]}), files[3]);

	assert_equal(listed - 1, s.search_items().size(), "the deleted item leaves the listing");
	assert_equal(false, s.display_items().contains(files[2]), "and is not shown");
	assert_equal(true, s.display_items().contains(files[1]) && s.display_items().contains(files[3]),
	             "while those either side of it stay");
	assert_equal(true, s.focus_item() == files[3], "the cursor lands on the item after it");
	assert_equal(true, s.selected_items().size() == 1 && s.selected_items().contains(files[3]),
	             "which is what is selected");

	// One item deleted from its own row, not the selection: the selection stays where it is.
	s.remove_items(f.view, df::item_set({files[5]}), nullptr);
	assert_equal(listed - 2, s.search_items().size(), "a second deletion leaves too");
	assert_equal(true, s.focus_item() == files[3], "and the cursor stays put");

	// The selection deleted with nothing after it: the cursor goes with it rather than staying on what has gone.
	s.select(f.view, files[4], false, false, false);
	s.update_selection();
	s.remove_items(f.view, df::item_set({files[4]}), nullptr);
	assert_equal(true, s.focus_item() == nullptr && s.selected_items().empty(), "nothing is left selected");

	s.remove_items(f.view, df::item_set({files[4]}), files[0]);
	assert_equal(listed - 3, s.search_items().size(), "removing what is not listed changes nothing");
	assert_equal(true, s.focus_item() == nullptr, "not even the cursor");
}

// An open naming the items to land on - a paste, a move that shows where it went - is followed straight away by the
// refresh that validating the folders it changed asks for: an open of the same search with nothing to select. The
// refresh supersedes the first pass before it publishes, and a superseded pass is dropped, so unless the refresh
// takes over what that pass was opened for the cursor lands nowhere at all.
static void should_keep_what_a_superseded_open_asked_for()
{
	const auto root = test_temps.next_folder("superseded-open");
	for (const auto& name : {"a.jpg"s, "b.jpg"s, "c.jpg"s})
		platform::copy_file(core::file_path(test_files_folder(), "Test.jpg"), root.combine_file(name), false, false);

	null_state_strategy ss;
	deferred_async_strategy as;
	const view_host_base_ptr view;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);

	const auto settle = [&as]
	{
		drain_query_work(as);
	};

	const auto search = df::search_t().add_selector(root);
	s.open(view, search, {});
	settle();
	assert_equal(true, s.find_displayed_item_by_name("b.jpg") != nullptr, "the folder is listed");
	assert_equal(0_z, s.selected_count(), "and nothing is selected yet");

	// Both opens are queued before either pass runs, so the first is superseded without publishing.
	const auto landing = root.combine_file("b.jpg");
	s.open(view, search, core::unique_paths{landing});
	s.open(view, search, {});
	settle();

	assert_equal(1_z, s.selected_count(), "the refresh selects what the superseded open named");
	assert_equal(true, s.selected_items().items().front()->path() == landing, "which is the item it named");
}

// design.md: "Only photos, video, and audio take part, so a folder, document, or archive is stepped
// over rather than stalling the sequence". The fixture folder holds all three kinds of passenger.
static void should_step_over_items_that_cannot_play()
{
	browsing_fixture f;

	const auto archive = f.find("benchmarks.zip");
	const auto document = f.find("place.json");
	assert_equal(true, archive != nullptr, "the archive fixture is present");
	assert_equal(true, document != nullptr, "the document fixture is present");
	assert_equal(false, archive->is_media(), "an archive is not media");
	assert_equal(false, document->is_media(), "a document is not media");

	auto folders = 0;

	for (const auto& g : f.state.groups())
	{
		for (const auto& i : g->items())
		{
			if (i->is_folder()) ++folders;
		}
	}

	assert_equal(true, folders > 0, "the fixture collection contains folders to step over");

	// Walk the whole collection from every starting point; the sequence must never offer a
	// passenger it cannot play or hold.
	auto visited = 0;

	for (const auto& g : f.state.groups())
	{
		for (const auto& i : g->items())
		{
			f.state.select(f.view, i, false, false, false);
			const auto next = f.state.next_media_item(true, true);

			if (next)
			{
				assert_equal(true, next->is_media(), "forward step lands on media");
				++visited;
			}

			const auto previous = f.state.next_media_item(false, true);
			if (previous) assert_equal(true, previous->is_media(), "backward step lands on media");
		}
	}

	assert_equal(true, visited > 0, "the walk actually stepped");

	// Without wrap the ends are ends, which is what stops a sequence looping when repeat is off.
	f.state.select(f.view, f.state.next_media_item(true, true), false, false, false);
	auto steps = 0;
	while (const auto next = f.state.next_media_item(true, false))
	{
		f.state.select(f.view, next, false, false, false);
		if (++steps > 1000) break;
	}

	assert_equal(true, steps > 0, "stepping forward without wrap advances");
	assert_equal(true, f.state.next_media_item(true, false) == nullptr, "the last media item has no next");
	assert_equal(true, f.state.next_media_item(true, true) != nullptr, "wrapping from the last item finds one");
}

// design.md: "a slideshow that can no longer reach a playable item stops instead of appearing to
// keep playing".
static void should_offer_a_slideshow_only_when_something_can_play()
{
	browsing_fixture f;
	assert_equal(true, f.state.can_slideshow(), "a collection holding media can run a slideshow");

	// A scope holding nothing playable cannot, however it was reached.
	browsing_fixture empty(test_temps.next_folder("no-media"));
	assert_equal(false, empty.state.can_slideshow(), "an empty scope cannot run a slideshow");
}

// design.md: "Next and Previous folder move between siblings of the current folder. At the first or
// last sibling they do nothing and are unavailable; they never change level."
static void should_move_between_sibling_folders()
{
	const auto root = test_temps.next_folder("siblings");
	for (const auto* const name : {"b-middle", "a-first", "c-last"})
	{
		platform::create_folder(root.combine(name));
	}

	browsing_fixture f(root.combine("b-middle"));

	assert_equal(true, f.state.has_next_path(true), "a middle sibling has a next");
	assert_equal(true, f.state.has_next_path(false), "a middle sibling has a previous");

	f.state.open_next_path(f.view, true);
	assert_equal("c-last", f.state.search().selectors().front().folder().name(), "next moves to the later sibling");
	assert_equal(false, f.state.has_next_path(true), "the last sibling has no next");
	assert_equal(true, f.state.has_next_path(false), "the last sibling still has a previous");

	// At the end the command does nothing rather than changing level.
	const auto before = f.state.search().text();
	f.state.open_next_path(f.view, true);
	assert_equal(before, f.state.search().text(), "the last sibling does not move");

	f.state.open_next_path(f.view, false);
	f.state.open_next_path(f.view, false);
	assert_equal("a-first", f.state.search().selectors().front().folder().name(), "previous walks back");
	assert_equal(false, f.state.has_next_path(false), "the first sibling has no previous");
}

// design.md: "Empty space is inert: a click that hits no item, and a drag whose rectangle covers no
// item, leave the selection unchanged rather than clearing it."
static void should_leave_the_selection_alone_over_empty_space()
{
	browsing_fixture f;

	const auto first = find_item_n(f.state, 0);
	const auto second = find_item_n(f.state, 1);
	assert_equal(true, first != nullptr && second != nullptr, "the fixture has items to select");

	// Item bounds come from layout, which no test runs, so they are placed here instead.
	first->bounds = {0, 0, 100, 100};
	second->bounds = {100, 0, 200, 100};

	for (const auto& g : f.state.groups())
	{
		g->bounds = {0, 0, 1000, 1000};
	}

	f.state.select(f.view, df::item_elements{first, second}, false);
	f.state.update_selection();
	assert_equal(2_z, f.state.selected_count(), "two items selected to start");

	// A rectangle over nothing must not clear what is selected.
	f.state.select(f.view, recti{500, 500, 600, 600}, false);
	f.state.update_selection();
	assert_equal(2_z, f.state.selected_count(), "an empty rectangle leaves the selection alone");

	// A rectangle that does cover an item still selects normally, so the guard above is not simply
	// refusing every rectangle.
	f.state.select(f.view, recti{0, 0, 50, 50}, false);
	f.state.update_selection();
	assert_equal(1_z, f.state.selected_count(), "a rectangle over an item replaces the selection");
}

// design.md: "Filters change what is visible, not what exists ... every command still targets only
// visible items".
static void should_target_only_visible_items_when_filtered()
{
	browsing_fixture f;

	f.state.select_all(f.view);
	f.state.update_selection();
	const auto unfiltered = f.state.selected_count();
	assert_equal(true, unfiltered > 0, "select all selects something");

	f.state.filter().toggle(file_group::photo);
	f.state.update_item_groups();
	f.state.update_selection();

	const auto filtered = f.state.selected_count();
	assert_equal(true, filtered > 0, "photos remain selected through the filter");
	assert_equal(true, filtered < unfiltered, "a filter removes hidden items from the command target");

	for (const auto& i : f.state.selected_items()._items)
	{
		assert_equal(true, i->file_type()->group == file_group::photo, "only visible items are targeted");
	}

	// Select All targets what is visible, not what exists.
	f.state.select_all(f.view);
	f.state.update_selection();
	assert_equal(filtered, f.state.selected_count(), "select all targets the visible items");

	f.state.clear_filters();
	f.state.update_item_groups();
	f.state.update_selection();
	assert_equal(true, f.state.selected_count() >= filtered, "clearing the filter shows the items again");
}

// design.md: "Shuffle is visibly exclusive with deterministic sorting."
static void should_keep_shuffle_exclusive_with_sorting()
{
	browsing_fixture f;

	f.state.group_order(group_by::shuffle, {});
	assert_equal(true, f.state.group_order() == group_by::shuffle, "shuffle can be chosen");

	// Choosing a deterministic sort while shuffled must leave shuffle behind rather than producing a
	// grouping that claims to be both.
	f.state.group_order({}, sort_by::name);
	assert_equal(true, f.state.sort_order() == sort_by::name, "the sort order is taken");
	assert_equal(false, f.state.group_order() == group_by::shuffle, "sorting turns shuffle off");

	f.state.group_order(group_by::shuffle, {});
	assert_equal(true, f.state.group_order() == group_by::shuffle, "shuffle can be chosen again");

	// Grouping alone is a different axis and does not need to disturb the sort.
	f.state.group_order(group_by::camera, {});
	assert_equal(true, f.state.group_order() == group_by::camera, "grouping can be changed");

	// A related search presents as related regardless of the chosen grouping.
	f.state.group_order(group_by::file_type, sort_by::def);
	assert_equal(true, f.state.effective_group_order() == group_by::file_type, "a plain search groups as chosen");
}

static void should_honor_no_indexing_for_collection_updates()
{
	const auto saved = command_line.no_indexing;
	const core::scope_exit restore([saved] { command_line.no_indexing = saved; });
	const auto app = std::make_shared<app_frame>();

	command_line.no_indexing = true;
	app->update_index();
	assert_equal(0_z, app->index_task_queue.dequeue_all().size(), "no-indexing suppresses collection refresh");
	app->rebuild_index();
	assert_equal(0_z, app->index_task_queue.dequeue_all().size(), "no-indexing does not queue cache invalidation");

	command_line.no_indexing = false;
	app->update_index();
	assert_equal(1_z, app->index_task_queue.dequeue_all().size(), "normal startup still queues collection refresh");
}

static void should_bind_every_advertised_command()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	assert_equal(false, app->_commands.empty(), "the command table is populated");

	std::vector<std::string> unbound;
	for (const auto& [id, command] : app->_commands)
	{
		if (!command->invoke && !command->menu) unbound.emplace_back(command->text);
	}
	std::ranges::sort(unbound);
	assert_equal(std::string{}, core::combine(unbound, ", "), "commands without an action or submenu");
}

// The Options page was reached through three commands, one per section, and the sections are now headers on
// the one page: one command opens it, with the cog and F6, and the Options menu lists it once. The sidebar's
// link to its own section is in no menu and has no key.
static void should_offer_options_as_one_command()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();

	const auto options = app->find_command(commands::options);
	assert_equal(true, options != nullptr, "Options is a command");
	assert_equal(true, options->icon == icon_index::settings, "with the cog");
	assert_equal(tt.options_title.sv(), options->text, "named Options");

	const auto menu = app->find_command(commands::menu_options)->menu();
	assert_equal(1, static_cast<int>(std::ranges::count(menu, options)), "the Options menu lists it once");
	assert_equal(true, std::ranges::find(menu, app->find_command(commands::options_sidebar)) == menu.end(),
	             "but not the sidebar's link to its own section");

	std::vector<platform::keyboard_accelerator_t> bound;
	for (const auto& [id, key] : default_keyboard_accelerators())
	{
		if (id == commands::options || id == commands::options_sidebar) bound.emplace_back(key);
	}

	assert_equal(1, static_cast<int>(bound.size()), "one key opens the page");
	assert_equal(true, bound.front().key == platform::keys::F6 && bound.front().key_state == 0, "and it is F6");
}

// The items and media views' context menus open with a row of the commonest commands on what they act on
// as buttons - cut, copy, rename and delete, and paste among items - which are not listed again below.
static void should_open_the_context_menus_with_a_row_of_buttons()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	app->_view_items->_regions.items = {0, 0, 100, 100};

	// An item under the pointer, laid out as the listing lays one out.
	df::item_set listed;
	df::index_file_item indexed;
	indexed.name = core::cache("menu-photo.jpg"sv);
	indexed.ft = files::file_type_from_name("menu-photo.jpg"sv);
	const auto item = std::make_shared<df::item_element>(core::file_path(core::folder_path("c:\\menus"),
	                                                                     "menu-photo.jpg"),
	                                                     indexed);
	item->add_to(listed);
	app->_state.append_items(app->_view_frame, listed, {}, true, true);
	app->_state.update_item_groups();
	app->_state.select(app->_view_frame, item, false, false, false);
	ui::test::fake_measure_context mc({8, 16});
	item->layout(mc, {10, 10, 40, 40});

	const auto items = app->context_menu(app->_view_items->context_menu({20, 20}));
	const auto media = app->context_menu(app->_view_media->context_menu({50, 50}));

	for (const auto& menu : {items, media})
	{
		assert_equal(true, menu.size() > 2 && menu[0] && !menu[0]->buttons.empty() && menu[1] == nullptr,
		             "the menu opens with a row of buttons and a separator");

		for (const auto id : {commands::edit_cut, commands::edit_copy, commands::tool_rename, commands::tool_delete})
		{
			const auto command = app->find_command(id);
			assert_equal(1, static_cast<int>(std::ranges::count(menu[0]->buttons, command)), "each is a button");
			assert_equal(0, static_cast<int>(std::ranges::count(menu, command)), "and is not listed again");
		}
	}

	const auto paste = app->find_command(commands::edit_paste);
	assert_equal(1, static_cast<int>(std::ranges::count(items[0]->buttons, paste)),
	             "paste is among the items' buttons");

	const auto movie = app->context_menu(app->_view_movie->context_menu({50, 50}));
	assert_equal(true, !movie.empty() && movie[0] && movie[0]->buttons.empty(), "the movie's menu has no row");
}

// Each part of Items offers a menu about what is under the pointer. Between the listing's items it is about the
// listing, not a selection the pointer is not on; beside the media pane's picture, about the item's details; on a
// sidebar row, what can be done with that row, before the sidebar's own commands.
static void should_offer_a_menu_about_what_is_under_the_pointer()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	app->_view_items->_regions.items = {0, 0, 100, 100};
	app->_view_items->_regions.media = {100, 0, 300, 100};
	app->_view_items->_regions.sidebar = {300, 0, 400, 100};

	const auto count = [&app](const std::vector<ui::command_ptr>& menu, const commands id)
	{
		const auto command = app->find_command(id);
		auto n = static_cast<int>(std::ranges::count(menu, command));
		for (const auto& entry : menu)
			if (entry) n += static_cast<int>(std::ranges::count(entry->buttons, command));
		return n;
	};

	const auto space = app->context_menu(app->_view_items->context_menu({80, 80}));
	assert_equal(true, space.size() > 2 && space[0] && !space[0]->buttons.empty() && space[1] == nullptr,
	             "the listing's own menu opens with a row of buttons");
	assert_equal(1, count(space, commands::edit_paste), "Paste is among them");
	assert_equal(1, count(space, commands::tool_new_folder), "and New folder");
	for (const auto id : {commands::edit_cut, commands::edit_copy, commands::tool_rename, commands::tool_delete,
	                      commands::menu_open, commands::menu_tools, commands::menu_rate_or_label,
	                      commands::tool_copy_to_folder, commands::edit_copy_item_path})
	{
		assert_equal(0, count(space, id), "nothing acts on the selection");
	}
	assert_equal(1, count(space, commands::menu_group), "the listing is grouped from it");

	const auto details = app->context_menu(app->_view_items->context_menu({200, 50}));
	assert_equal(1, count(details, commands::verbose_metadata), "the details' menu lists the metadata in full");
	assert_equal(1, count(details, commands::tool_edit_metadata), "and edits it");
	for (const auto id : {commands::play, commands::slideshow, commands::view_fullscreen, commands::menu_zoom})
	{
		assert_equal(0, count(details, id), "and does not play or zoom a picture it is not on");
	}

	const auto sidebar = app->_view_items->sidebar();
	const auto row = std::make_shared<sidebar_element>(app->_state);
	row->search = df::search_t().add_selector(core::folder_path("c:\\menus"));
	row->bounds = {300, 10, 400, 30};
	sidebar->_elements = {row};

	const auto on_row = app->context_menu(app->_view_items->context_menu({350, 20}));
	assert_equal(true, on_row.size() > 5 && on_row[0] && on_row[0]->text == tt.command_open.sv(),
	             "a row's menu opens with what a click on it does");
	assert_equal(std::string(tt.command_show_in_file_browser.sv()), on_row[1] ? on_row[1]->text : std::string{},
	             "then shows its folder in the file browser");
	assert_equal(std::string(tt.command_edit_copy_item_path.sv()), on_row[2] ? on_row[2]->text : std::string{},
	             "copies its path");
	assert_equal(std::string(tt.command_file_properties.sv()), on_row[3] ? on_row[3]->text : std::string{},
	             "and shows its properties");
	assert_equal(true, on_row[4] == nullptr, "before the sidebar's own commands");
	assert_equal(1, count(on_row, commands::options_sidebar), "which customise it");

	const auto off_row = app->context_menu(app->_view_items->context_menu({350, 80}));
	assert_equal(true, !off_row.empty() && off_row[0] == app->find_command(commands::refresh),
	             "away from a row, the sidebar's own commands alone");
	assert_equal(1, count(off_row, commands::options_sidebar), "customise it");
	assert_equal(1, count(off_row, commands::view_show_sidebar), "and hide it");
}

namespace
{
	// A view's surface as Windows makes it: a child window, whose window bounds are its parent's coordinates
	// and not the screen's. Placed on a display left of the primary one, where the two cannot agree by chance.
	struct child_surface final : ui::null_frame
	{
		recti parent_bounds{20, 70, 1020, 770};
		pointi screen_origin = pointi{-2400, 98} + parent_bounds.top_left();
		recti opened;

		recti window_bounds() const override { return parent_bounds; }
		pointi client_to_screen(const pointi loc) const override { return loc + screen_origin; }
		void track_menu(const recti bounds, const std::vector<ui::command_ptr>&) override { opened = bounds; }
	};

	bool offers(const view_menu& menu, const commands id)
	{
		return std::ranges::find(menu.row, id) != menu.row.end() || std::ranges::find(menu.list, id) != menu.list.end();
	}
}

// A right-click arrives as a point on the screen, and reaches the view under it through the view surface's
// own place there. Read through its window bounds instead, a right-click anywhere on a window away from the
// screen's origin offered the frame's menu, and one on the listing picked the item a window's offset away.
static void should_offer_the_menu_of_the_view_under_the_pointer()
{
	const auto app = std::make_shared<app_frame>();
	const auto surface = std::make_shared<child_surface>();
	auto& frame = *app->_view_frame;
	frame._frame = surface;
	frame._extent = surface->parent_bounds.extent();
	frame._view_bounds = {0, 0, 800, 700};
	app->_view = app->_view_items;
	auto& items = *app->_view_items;
	items._regions.sidebar = {0, 0, 200, 700};
	items._regions.items = {200, 0, 500, 700};
	items._regions.media = {500, 0, 800, 700};
	const auto at = [&](const pointi loc)
	{
		return app->menu_at(loc + surface->screen_origin);
	};

	assert_equal(true, offers(at({300, 100}), commands::menu_navigate), "the listing offers its own menu");
	assert_equal(true, offers(at({600, 100}), commands::verbose_metadata),
	             "the media pane offers the menu on what it shows, details without a picture");
	assert_equal(true,
	             offers(at({100, 100}), commands::options_sidebar) && offers(at({100, 100}), commands::tool_eject),
	             "the sidebar offers its own commands, with the window's");
	assert_equal(true, at({900, 100}).empty(), "the panel beside the view offers none");
	assert_equal(true, offers(at({300, -30}), commands::tool_eject), "and beyond the view is the frame's");

	app->_view = app->_view_rename;
	assert_equal(true, at({300, 100}).list == std::vector{commands::tool_run, commands::none, commands::view_close},
	             "a task view offers the commands of its status bar, then Close");
	app->_view = app->_view_movie;
	const auto movie = at({300, 100});
	assert_equal(true, offers(movie, commands::tool_movie_render) && movie.list.back() == commands::view_close,
	             "and Movie away from its timeline offers its own commands");

	// Apps and Shift+F10 open the menu beside what has the keyboard as they are pressed, so the menu the platform
	// asks for afterwards, at no point, is not opened a second time.
	assert_equal(true, app->menu({-1, -1}).empty(), "the keyboard's menu is not opened again");
}

// Tab visits the whole window in its order, whichever frame each stop is in: the title bar's controls, the view's
// own content, then its panel - each control, a radio group as one stop at its checked button, each link, and a
// rating's stars as one - and the status bar's controls. An edit is entered with its text selected. Full screen,
// with no bars, keeps the rest.
static void should_take_tab_round_the_window()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	app->_view = app->_view_rename;
	app->_view_frame->view(app->_view_rename);

	const auto toolbar = []
	{
		const auto command = std::make_shared<ui::command>();
		command->invoke = [] {};
		return ui::make_toolbar({}, {command});
	};

	const auto title = toolbar();
	const auto status = toolbar();
	app->_title_bar->_controls = {title};
	app->_status_bar->_controls = {status};

	auto rating = 0;
	const auto edit = ui::make_edit({}, "Old", [](const std::string&) {});
	const auto one = ui::make_check_button(true, "One", true, [](bool) {}, 7);
	const auto two = ui::make_check_button(false, "Two", true, [](bool) {}, 7);
	const auto link = std::make_shared<link_element>("Link", [] {});
	const auto stars = std::make_shared<rating_edit_control>(rating, nullptr);
	const auto panel = std::make_shared<view_controls_host>(app->_state);
	panel->_controls = {edit, one, two, link, stars};
	app->_view_frame->panel(panel, false);
	app->_view_frame->_view_bounds = {0, 0, 600, 400};
	ui::test::fake_measure_context mc({8, 16});
	panel->layout_panel(mc, {600, 0, 1000, 400});

	const auto stops = app->keyboard_stops();
	assert_equal(7_z, stops.size(), "a stop for each control and link, a radio group and a rating one each");
	assert_equal(true, stops[0].control == title, "the title bar's controls come first");
	assert_equal(true, !stops[1].control && stops[1].parts.empty() && !stops[1].strip, "then the view's own content");
	assert_equal(true, stops[2].control == edit, "then the panel's controls, in its order");
	assert_equal(true, stops[3].control == one, "a radio group at its checked button");
	assert_equal(true, stops[4].parts.size() == 1 && stops[4].parts.front().element == link, "a link of its own");
	assert_equal(true,
	             stops[5].parts.size() == rating_edit_control::cell_count && stops[5].parts.front().element == stars,
	             "a rating's stars one stop, which the arrows move through");
	assert_equal(true, stops[6].control == status, "and the status bar's controls last");

	app->keyboard_to(stops[2]);
	edit->on_text("New");
	assert_equal(std::string("New"), edit->window_text(),
	             "an edit is entered with its text selected, so typing replaces it");

	app->_show_bars = false;
	assert_equal(5_z, app->keyboard_stops().size(), "full screen passes over the bars");
}

// The keyboard on a drawn part: the arrows move it among its group - a rating's stars - Enter presses the part it
// is on as a click there does, and Escape and Tab are left to the task and the window. A rebuild keeps the keyboard
// on the part nearest where it was; a part that has gone has the keyboard no longer.
static void should_work_a_part_from_the_keyboard()
{
	const auto app = std::make_shared<app_frame>();
	auto& frame = *app->_view_frame;
	frame.view(app->_view_rename);

	auto rating = 0;
	const auto stars = std::make_shared<rating_edit_control>(rating, nullptr);
	const auto panel = std::make_shared<view_controls_host>(app->_state);
	panel->_controls = {stars};
	frame.panel(panel, false);
	ui::test::fake_measure_context mc({8, 16});
	panel->layout_panel(mc, {600, 0, 1000, 400});

	const auto parts = frame.keyboard_parts(keyboard_area::panel);
	frame.keyboard_to(keyboard_area::panel, &parts[1]);
	assert_equal(true, frame.keyboard_part_key_down(platform::keys::RIGHT, {}), "Right moves along the stars");
	assert_equal(2, frame._keyboard.index, "to the next");
	assert_equal(true, frame.keyboard_part_key_down(platform::keys::RETURN, {}), "Enter presses it");
	assert_equal(2, rating, "as a click on that star does");
	assert_equal(false, frame.keyboard_part_key_down(platform::keys::ESCAPE, {}),
	             "Escape in a task's panel is the task's");
	assert_equal(false, frame.keyboard_part_key_down(platform::keys::TAB, {}), "and Tab the window's");

	// The stars outlived their press, so a rebuild after it - the panel populated again - is followed as any is.
	assert_equal(true, frame.focused_part().has_value(), "the pressed star is still there");
	panel->_controls = {std::make_shared<rating_edit_control>(rating, nullptr)};
	panel->layout_panel(mc, {600, 0, 1000, 400});
	const auto followed = frame.focused_part();
	assert_equal(true, followed && followed->index == 2, "a later rebuild keeps the keyboard on the part nearest");

	frame.panel(nullptr, false);
	assert_equal(false, frame.focused_part().has_value(), "a part that has gone has the keyboard no longer");
}

// A focused control is worked by its own keys before the view's meanings for them: Enter presses a button or a
// toolbar's button rather than running the task or opening the focused item, and Space works any control rather
// than playing. Enter on a check box still submits the task, as Enter in a dialog does; an edit has had its keys.
static void should_leave_a_focused_control_its_own_keys()
{
	const auto command = std::make_shared<ui::command>();
	command->invoke = [] {};
	const auto toolbar = ui::make_toolbar({}, {command});
	const auto button = ui::make_button("Go", [] {});
	const auto check = ui::make_check_button(false, "Check", false, [](bool) {});
	const auto edit = ui::make_edit({}, {}, [](const std::string&) {});
	const platform::key_state none{};
	platform::key_state control;
	control.control = true;

	assert_equal(true, control_takes_key(*toolbar, platform::keys::RETURN, none), "Enter presses a toolbar's button");
	assert_equal(true, control_takes_key(*button, platform::keys::RETURN, none), "and a button");
	assert_equal(false, control_takes_key(*check, platform::keys::RETURN, none), "but submits a task from a check box");
	assert_equal(true, control_takes_key(*check, platform::keys::SPACE, none), "Space works any control");
	assert_equal(true, control_takes_key(*toolbar, platform::keys::RIGHT, none), "the arrows move along a toolbar");
	assert_equal(false, control_takes_key(*edit, platform::keys::SPACE, none), "an edit has had its keys already");
	assert_equal(false, control_takes_key(*button, U'A', control), "and a shortcut is the window's");
}

// The links among Items' listing - those in a group's title among them - are a stop of the keyboard's of their own.
static void should_reach_the_links_among_the_listing()
{
	const auto app = std::make_shared<app_frame>();
	auto& items = *app->_view_items;
	ui::test::fake_measure_context mc({8, 16});

	const auto link = std::make_shared<link_element>("2024", [] {});
	const auto title = std::make_shared<group_title_control>("Group", std::vector<view_element_ptr>{link});
	title->measure(mc, 400);
	title->layout(mc, {0, 0, 400, 40});
	items._item_elements = {title};

	const auto regions = items.keyboard_regions();
	const auto listing = std::ranges::find(regions, keyboard_area::listing, &keyboard_region::area);
	assert_equal(true, listing != regions.end(), "the listing's links are a group of their own");

	const auto parts = find_keyboard_parts(listing->roots, listing->offset);
	assert_equal(true, parts.size() == 1 && parts.front().element == link, "a group's title offers its link");
}

// Menus a view's controls open are placed by bounds in the view's surface, which open on the screen where
// those bounds are, beside what opened them.
static void should_open_a_view_s_menus_beside_what_opened_them()
{
	const auto app = std::make_shared<app_frame>();
	const auto surface = std::make_shared<child_surface>();
	const recti button{40, 50, 90, 70};
	const auto expected = button.offset(surface->screen_origin);

	app->_view_frame->_frame = surface;
	app->_view_frame->track_menu(button, {});
	assert_equal(true, surface->opened == expected, "the view's own menus open beside what opened them");

	const auto panel = std::make_shared<view_controls_host>(app->_state);
	panel->_frame = surface;
	surface->opened = {};
	panel->track_menu(button, {});
	assert_equal(true, surface->opened == expected, "and so do those of the panel beside it");

	const auto sidebar = app->_view_items->sidebar();
	sidebar->attach_embedded(surface, ui::no_frame());
	surface->opened = {};
	sidebar->track_menu(button, {});
	assert_equal(true, surface->opened == expected, "and the sidebar's");
}

// Items starts its status bar with one toolbar: Options, Tools, Playback options, Recent changes and Open.
// All but Open are icons alone. Open is labelled with what is selected and opens the same menu as the
// panel's Open, which keeps its own name.
static void should_start_the_items_status_bar_with_one_toolbar()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();

	for (const auto id :
	     {commands::options, commands::menu_tools, commands::playback_menu, commands::menu_recent_changes})
	{
		const auto command = app->find_command(id);
		assert_equal(true, command != nullptr && command->icon != icon_index::none, "an icon");
		assert_equal(std::string{}, command->toolbar_text, "shown alone");
	}

	assert_equal(true, static_cast<bool>(app->find_command(commands::menu_tools)->menu), "Tools drops down");
	assert_equal(true, static_cast<bool>(app->find_command(commands::playback_menu)->menu), "so does Playback");
	assert_equal(true, static_cast<bool>(app->find_command(commands::menu_recent_changes)->menu), "and Recent changes");

	const auto open = app->find_command(commands::menu_open);
	const auto open_status = app->find_command(commands::menu_open_status);
	assert_equal(true, open_status->icon == open->icon, "Open in the status bar looks like Open");
	assert_equal(true, open_status->menu && open_status->menu.target_type() == open->menu.target_type(),
	             "and opens the Open menu");
	assert_equal(true, open_status->text_can_change, "its label follows the selection");
	assert_equal(tt.command_open.sv(), open->toolbar_text, "while the panel's Open keeps its name");
}

// docs/faces.md#92-faces: the status bar relabels both review actions while Move to group still opens a chooser.
static void should_relabel_the_faces_commands_in_the_status_bar()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	const auto name = app->find_command(commands::face_name);
	const auto confirm = app->find_command(commands::face_confirm);
	const auto reject = app->find_command(commands::face_reject);
	const auto status = ui::make_toolbar({}, {name, confirm, reject});

	assert_equal(tt.face_search.sv(), app->find_command(commands::face_search)->toolbar_text,
	             "Face search is the button for comparing faces");
	assert_equal(tt.command_face_person_search.sv(), app->find_command(commands::face_person_search)->toolbar_text,
	             "Person search is a distinct command");
	app->update_toolbar_text(commands::face_confirm, str_format(tt.faces_confirm_person_fmt, "Zac Walker"));
	app->update_toolbar_text(commands::face_reject, str_format(tt.faces_not_person_fmt, "Zac Walker"));
	status->update_button_state(false, false);
	const auto shown = status->buttons();
	assert_equal(3_z, shown.size(), "all three buttons");
	assert_equal(tt.faces_name_faces.sv(), shown[0].text, "Name acts on the selected faces");
	assert_equal("Confirm Zac Walker"sv, shown[1].text, "confirm names the person");
	assert_equal("Not Zac Walker"sv, shown[2].text, "reject names the person");
}

static void should_show_one_line_of_faces_status_text()
{
	class status_draw_context final : public ui::test::fake_draw_context
	{
	public:
		render::text_style style = render::text_style::none;
		void draw_text(const std::string_view text, const recti bounds, const render::font_face font,
		               const render::text_style text_style, const core::color foreground,
		               const core::color background) override
		{
			style = text_style;
			fake_draw_context::draw_text(text, bounds, font, text_style, foreground, background);
		}
	};

	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	app->create_toolbars();
	app->_view_faces = std::make_shared<faces_view>(state, nullptr);
	app->_view = app->_view_faces;
	app->_view_faces->model_changed();
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	const auto layout_bar = [&](const int width)
	{
		app->_extent = {width, 800};
		const auto height = app->status_bar_height(mc, width);
		std::vector<ui::control_ptr> shown;
		app->layout_status_bar(mc, {width, height}, shown);
		assert_equal(true, shown.size() > 2 && shown[0] == app->_face_filter_edit && shown[1] == app->_face_size,
		             "the filter and size slider lead, even when the action buttons overflow");
		assert_equal(true, shown[0]->bounds.right < shown[1]->bounds.left,
		             "the size slider sits immediately to the right of the filter");
		assert_equal(true, std::ranges::find(shown, app->_filter_edit) == shown.end(),
		             "the Items filter is not used in Faces");
	};

	layout_bar(520);
	status_draw_context dc;
	app->draw_status_bar(dc);
	assert_equal(true, dc.style == render::text_style::single_line, "one line of face status");
	assert_equal(true, dc.texts.size() == 1 && dc.texts.front().text == app->_view->status(),
	             "the whole status is passed to the renderer");
	const auto text = app->_status_text_bounds;
	assert_equal(true, text.top > 0 && text.width() > 400, "a narrow window gives status a row below the commands");
	assert_equal(mc.text_line_height(render::font_face::dialog) + mc.padding2, text.height(), "no unused second line");

	layout_bar(2400);
	assert_equal(0, app->_status_text_bounds.top, "at a wide width the status stays beside the commands");

	const auto store = std::make_shared<faces::face_store>();
	const auto photo = core::file_path(core::folder_path("C:\\pictures"), "portrait.jpg");
	faces::face_record record;
	record.model = faces::model_id();
	record.faces.emplace_back().box = {0.1, 0.1, 0.2, 0.2};
	store->add(photo, record);
	store->publish();
	auto model = std::make_shared<faces_view_model>();
	model->store = store;
	auto overview = std::make_shared<faces::overview>();
	overview->store = store;
	overview->unknown = {0};
	model->overview = std::move(overview);
	app->_view_faces->model_arrived_for_tests(model);
	app->_view_faces->move_cells_to_name({0}, "Ann");
	assert_equal(true, app->_view_faces->can_undo_placement(), "a placement receipt with Undo");
	for (const int width : {520, 2400})
	{
		layout_bar(width);
		const auto row = app->_status_text_bounds;
		const auto receipt = app->_receipt_prefix_bounds;
		assert_equal(true,
		             !receipt.is_empty() && std::abs((receipt.top - row.top) - (row.bottom - receipt.bottom)) <= 1,
		             "the receipt text is vertically centered in its status row");
		assert_equal(true, app->_receipt_link && receipt.center().y == app->_receipt_link->bounds.center().y,
		             "the destination action shares the receipt's center line");
	}
	app->_view_faces->deactivate();
}

static void should_invoke_link_commands_and_selection_shortcuts()
{
	const auto saved_favorite_tags_only = setting.sidebar.show_favorite_tags_only;
	const core::scope_exit restore([saved_favorite_tags_only]
	{ setting.sidebar.show_favorite_tags_only = saved_favorite_tags_only; });
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	app->_view_has_focus = false;

	const auto click_link = [&app](const commands id, const std::string_view text)
	{
		const auto link = std::make_shared<link_element>(text, id);
		link->bounds = recti(0, 0, 180, 24);
		link->is_visible(true);
		const pointi offset{20, 40};
		const pointi location{60, 50};
		hit_test_context ctx{location, recti(0, 0, 200, 100)};
		const auto controller = link->controller_from_location(app->_view_frame, location, offset, ctx);
		assert_equal(true, controller != nullptr, "a command link can be clicked");
		controller->on_mouse_left_button_down(location, {});
		controller->on_mouse_left_button_up(location, {});
	};

	setting.sidebar.show_favorite_tags_only = true;
	click_link(commands::view_favorite_tags, tt.command_all_tags);
	assert_equal(false, setting.sidebar.show_favorite_tags_only, "Show all tags removes the favorites restriction");
	click_link(commands::view_favorite_tags, tt.command_favorite_tags);
	assert_equal(true, setting.sidebar.show_favorite_tags_only, "Favorite tags restores the restriction");

	df::item_set items;
	for (const auto name : {"command-photo.jpg"sv, "command-video.mp4"sv, "command-audio.mp3"sv})
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(name);
		const auto item = std::make_shared<df::item_element>(core::file_path(core::folder_path("c:\\commands"), name),
		                                                     indexed);
		item->add_to(items);
	}
	app->_state.append_items(app->_view_frame, items, {}, true, true);
	click_link(commands::filter_videos, tt.command_filter_videos);
	assert_equal(true, app->_state.filter().has_group(file_group::video), "the video link enables the filter");
	app->_state.update_item_groups();

	platform::key_state control;
	control.control = true;
	assert_equal(true, app->key_down('A', control), "the keyboard dispatcher handles Ctrl+A");
	for (const auto& item : items.items())
	{
		assert_equal(item->file_type()->group == file_group::video, item->is_selected(),
		             "Ctrl+A selects visible videos, not hidden photos or audio");
	}

	click_link(commands::filter_videos, tt.command_filter_videos);
	assert_equal(false, app->_state.filter().has_group(file_group::video), "the video link clears the filter");
	app->_state.update_item_groups();
	assert_equal(true, app->key_down('A', control), "Ctrl+A still works after clearing the filter");
	for (const auto& item : items.items())
	{
		assert_equal(true, item->is_selected(), "Ctrl+A selects every visible item");
	}
}

// docs/design.md#keyboard: Items takes the keys every list does. The arrows select what they reach, Ctrl moves the
// focus alone, Ctrl+Space puts the focused item in the selection or takes it out, and Ctrl with Shift adds the run
// from the anchor - while the selection stays what the keys leave it.
static void should_move_through_items_by_the_common_keys()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();

	df::item_set items;
	for (const auto name : {"keys-1.jpg"sv, "keys-2.jpg"sv, "keys-3.jpg"sv, "keys-4.jpg"sv})
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(name);
		const auto item = std::make_shared<df::item_element>(core::file_path(core::folder_path("c:\\keys"), name),
		                                                     indexed);
		item->add_to(items);
	}
	app->_state.append_items(app->_view_frame, items, {}, true, true);
	app->_state.update_item_groups();
	app->_state.view_mode(view_type::items);
	app->_view_has_focus = true;

	std::vector<df::item_element_ptr> shown;
	for (const auto& group : app->_state.groups())
		for (const auto& i : group->items()) shown.push_back(i);
	assert_equal(4_z, shown.size(), "four items listed");

	const auto selected = [&shown]
	{
		std::string result;
		for (size_t i = 0; i < shown.size(); ++i)
			if (shown[i]->is_selected()) result += std::to_string(i);
		return result;
	};
	platform::key_state control;
	control.control = true;
	platform::key_state control_shift = control;
	control_shift.shift = true;

	app->_state.select(app->_view_frame, shown[0], false, false, false);
	assert_equal(true, app->key_down(platform::keys::RIGHT, {}), "Items takes the arrows");
	assert_equal("1"s, selected(), "an arrow selects what it reaches");
	app->key_down(platform::keys::RIGHT, control);
	assert_equal("1"s, selected(), "Ctrl moves the focus alone");
	assert_equal(true, app->_state.focus_item() == shown[2], "on to the next item");
	app->key_down(platform::keys::SPACE, control);
	assert_equal("12"s, selected(), "Ctrl+Space puts the focused item in the selection");
	app->key_down(platform::keys::SPACE, control);
	assert_equal("1"s, selected(), "and takes it out again");
	app->key_down(platform::keys::RIGHT, control_shift);
	assert_equal("123"s, selected(), "Ctrl with Shift adds the run from the anchor");
	app->key_down(platform::keys::HOME, control);
	assert_equal(true, selected() == "123" && app->_state.focus_item() == shown[0], "Ctrl+Home moves the focus alone");
}

static void should_route_focused_view_keyboard_commands()
{
	struct keyboard_view final : view_base
	{
		int key_count = 0;
		bool editing_text = false;

		void activate(sizei) override {}
		void deactivate() override {}
		void refresh() override {}

		bool key_down(const char32_t key, const platform::key_state keys) override
		{
			++key_count;
			return key == 'Z' && keys.control;
		}

		ui::focus_mode focus_mode() const override
		{
			return editing_text ? ui::focus_mode::text_edit : ui::focus_mode::view;
		}
	};

	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	const auto view = std::make_shared<keyboard_view>();
	app->_view = view;
	app->_view_has_focus = true;
	platform::key_state control;
	control.control = true;

	assert_equal(true, app->key_down('Z', control), "a focused timeline receives its undo shortcut");
	assert_equal(1, view->key_count, "the view receives the key once");
	assert_equal(true, app->key_down('A', control), "an unhandled view key reaches global accelerators");
	assert_equal(2, view->key_count, "the focused view has first refusal");

	app->_view_has_focus = false;
	assert_equal(false, app->key_down('Z', control), "an unfocused view cannot consume keys");
	assert_equal(2, view->key_count, "native control focus does not dispatch to the view");

	app->_view_has_focus = true;
	view->editing_text = true;
	assert_equal(false, app->key_down('A', control), "text editing does not fall through to global selection");
	assert_equal(3, view->key_count, "text editing receives the key once");
}

// A Commodore 64 running in the view has the keyboard before the view and the application's own keys: the
// arrows are its joystick, and Escape leaves it rather than taking any of Escape's other meanings.
static void should_give_a_running_commodore_64_the_keyboard()
{
	struct counting_view final : view_base
	{
		int key_count = 0;

		void activate(sizei) override {}
		void deactivate() override {}
		void refresh() override {}

		bool key_down(const char32_t, const platform::key_state) override
		{
			++key_count;
			return true;
		}
	};

	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	const auto view = std::make_shared<counting_view>();
	app->_view = view;
	app->_view_has_focus = true;

	// Never started: the keys are handed to it all the same.
	common_display_state_t common;
	const auto display = std::make_shared<display_state_t>(app->_state._async, common);
	display->_c64 = std::make_shared<c64_session>(core::file_path{}, std::vector<uint8_t>{});
	app->_state._display = display;
	const core::scope_exit restore([&app] { app->_state._display.reset(); });

	assert_equal(true, app->key_down(platform::keys::RIGHT, {}), "the arrows go to the machine");
	assert_equal(true, app->key_down(U'A', {}), "and so do the C64's other keys");
	assert_equal(0, view->key_count, "before the view sees them");

	assert_equal(true, app->key_down(platform::keys::ESCAPE, {}), "Escape is taken");
	assert_equal(true, display->_c64 == nullptr, "and leaves the machine");
	assert_equal(0, view->key_count, "without reaching the view");

	assert_equal(true, app->key_down(platform::keys::RIGHT, {}), "the arrows are the view's again");
	assert_equal(1, view->key_count, "once the machine has gone");
}

// Two commands claiming one key means the second is unreachable, and nothing in the running app
// reports it. Dispatch (app_frame::key_down) walks `_commands`, an UNORDERED map, and takes the
// first match whose `enable` is set - so a collision is only safe while the two can never be
// enabled at once, and is otherwise resolved arbitrarily. The pairs below qualify: browse_* is
// enabled in the media and items views, edit_item_save_and_* only in the edit and locate views.
static void should_not_claim_one_key_for_two_commands()
{
	const auto table = default_keyboard_accelerators();
	assert_equal(true, table.size() > 80, "the accelerator table is populated");

	const std::set<std::pair<commands, commands>> allowed_collisions{
		{commands::browse_back, commands::edit_item_save_and_prev},
		{commands::browse_forward, commands::edit_item_save_and_next},
	};

	std::map<std::pair<char32_t, uint32_t>, commands> claimed;

	for (const auto& [id, key] : table)
	{
		assert_equal(true, key.key != 0, "every binding names a key");
		assert_equal(true, id != commands::none, "every binding names a command");

		const auto stroke = std::make_pair(key.key, key.key_state);
		const auto found = claimed.find(stroke);

		if (found == claimed.end())
		{
			claimed.emplace(stroke, id);
			continue;
		}

		const auto pair = std::minmax(found->second, id);
		assert_equal(true, allowed_collisions.contains({pair.first, pair.second}),
		             std::format("key {:#x} state {:#x} is claimed twice", static_cast<uint32_t>(key.key),
		                         key.key_state));
	}

	// A few bindings users rely on, spot-checked so a wholesale edit of the table cannot pass unnoticed.
	const auto bound = [&table](const commands id, const char32_t key, const uint32_t state)
	{
		return std::ranges::any_of(table, [=](const command_accelerator& a)
		{ return a.id == id && a.key.key == key && a.key.key_state == state; });
	};

	assert_equal(true, bound(commands::refresh, platform::keys::F5, 0), "F5 refreshes");
	assert_equal(true, bound(commands::view_fullscreen, platform::keys::F11, 0), "F11 is fullscreen");
	assert_equal(true, bound(commands::explore, platform::keys::F4, 0), "F4 opens Explore");
	assert_equal(true, bound(commands::faces, platform::keys::F8, 0), "F8 opens Faces");
	assert_equal(true, bound(commands::select_all, 'A', platform::keyboard_accelerator_t::control),
	             "Ctrl+A selects all");
	assert_equal(true, bound(commands::tool_delete, platform::keys::DEL, 0), "Delete deletes");
	assert_equal(true, bound(commands::view_close, platform::keys::ESCAPE, 0), "Escape closes");
}

// The keyboard reference and every command tooltip spell a binding through this one function, so a
// key left without a name, or modifiers in another order, would show in both.
static void should_spell_keyboard_accelerators()
{
	constexpr auto alt = platform::keyboard_accelerator_t::alt;
	constexpr auto control = platform::keyboard_accelerator_t::control;
	constexpr auto shift = platform::keyboard_accelerator_t::shift;

	assert_equal(std::string(tt.platform_text.keyboard_f5.sv()),
	             platform::format_keyboard_accelerator({{platform::keys::F5, 0}}), "a named key");
	assert_equal(std::string(tt.platform_text.keyboard_del.sv()),
	             std::string(platform::keys::format(platform::keys::DEL)), "Delete has a name");
	assert_equal(std::format("{}+{}+A", tt.platform_text.keyboard_alt, tt.platform_text.keyboard_shift),
	             platform::format_keyboard_accelerator({{U'A', alt | shift}}), "modifiers lead in a fixed order");
	assert_equal(std::format("{}+'{}'", tt.platform_text.keyboard_control, tt.platform_text.keyboard_oem_plus),
	             platform::format_keyboard_accelerator({{platform::keys::OEM_PLUS, control}}),
	             "plus is quoted so it is not read as the separator");
	assert_equal(std::format("{} {} {}", tt.platform_text.keyboard_left, tt.platform_text.keyboard_or,
	                         tt.platform_text.keyboard_right),
	             platform::format_keyboard_accelerator({{platform::keys::LEFT, 0}, {platform::keys::RIGHT, 0}}),
	             "alternatives are joined");
}

// Feature use records one bit per view, so a view added without a bit is never counted as opened
// - which is how opening the Movie view went unrecorded. Faces alone records none: face search counts nothing
// it is used for (docs/faces.md#11-privacy-and-recovery).
static void should_record_a_bit_for_every_view()
{
	assert_equal(0_z, static_cast<size_t>(features::view_bit(view_type::none)), "no view records nothing");
	assert_equal(0_z, static_cast<size_t>(features::view_bit(view_type::faces)), "nor does Faces");

	uint64_t seen = 0;

	for (auto v = static_cast<int>(view_type::items); v <= static_cast<int>(view_type::options); ++v)
	{
		if (static_cast<view_type>(v) == view_type::faces) continue;
		const auto bit = features::view_bit(static_cast<view_type>(v));
		assert_equal(1, std::popcount(bit), std::format("view {} records one bit", v));
		assert_equal(0_z, static_cast<size_t>(seen & bit), std::format("view {} has its own bit", v));
		seen |= bit;
	}
}

// The button reads as one label or two. design.md: "Shuffle is visibly exclusive with deterministic
// sorting", so a shuffled view must never also advertise a sort order.
static void should_label_grouping_and_sorting()
{
	df::file_group_histogram summary;
	const df::file_group_histogram empty_summary;

	summary.record(files::file_type_from_name("Test.jpg"), core::file_size(4096));

	const auto says = [](const std::string& text, const std::string_view part)
	{
		return text.find(part) != std::string::npos;
	};

	// Nothing indexed yet reads as loading rather than as an empty collection.
	assert_equal(std::string(tt.loading),
	             format_items_summary(group_by::file_type, sort_by::name, empty_summary, false),
	             "an uncounted empty collection is loading");
	assert_equal(std::string(tt.empty), format_items_summary(group_by::file_type, sort_by::name, empty_summary, true),
	             "a counted empty collection is empty");

	// Shuffle names only itself; adding a sort order would claim an order it does not have.
	const auto shuffled = format_items_summary(group_by::shuffle, sort_by::name, summary, true);
	assert_equal(true, says(shuffled, tt.sort_by_shuffle.sv()), "shuffle is named");
	assert_equal(false, says(shuffled, tt.sort_by_name.sv()), "shuffle does not also claim a sort order");

	// The default sort is not a choice worth naming either.
	const auto defaulted = format_items_summary(group_by::file_type, sort_by::def, summary, true);
	assert_equal(false, says(defaulted, tt.sort_by_def.sv()), "the default order is not named");

	// A real pairing names both.
	const auto both = format_items_summary(group_by::camera, sort_by::name, summary, true);
	assert_equal(true, says(both, tt.prop_name_camera.sv()), "the grouping is named");
	assert_equal(true, says(both, tt.sort_by_name.sv()), "the sort order is named");

	// Grouping and sorting by the same thing is said once, not twice.
	const auto same = format_items_summary(group_by::date_created, sort_by::date_created, summary, true);
	assert_equal(true, says(same, tt.prop_name_created.sv()), "the shared name is present");
	assert_equal(true, both.length() > same.length(), "a shared name is not repeated");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Discussion #251 - the filter toolbar scrolled away with the list, so changing a filter over a long
// listing meant scrolling to the top first. The list's controls are declared for the frame's status
// bar instead, which does not scroll: everything the list owns is there, and it carries everything
// the scroll-track menu that stood in for it used to offer.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_keep_the_items_controls_in_the_status_bar()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());

	const items_view items(state, nullptr);
	const auto declared = items.bars();

	assert_equal(true, declared.navigation, "Items keeps the navigation bar as its title bar");
	assert_equal(true, declared.top.empty(), "and adds nothing to it");

	const auto declares = [&declared](const bar_entry& entry)
	{
		return std::ranges::find(declared.bottom, entry) != declared.bottom.end();
	};

	assert_equal(true, declares(bar_control::item_filter), "the filter box is in the status bar");
	assert_equal(true, declares(bar_control::item_size), "so is the thumbnail size");
	assert_equal(true, declares(bar_control::item_totals), "and the totals");
	assert_equal(true, declares(commands::filter_photos), "the media type filter is there");
	assert_equal(true, declares(commands::filter_videos), "every part of it");
	assert_equal(true, declares(commands::filter_audio), "every part of it");
	assert_equal(true, declares(commands::browse_recursive), "and Show items in subfolders");
	assert_equal(true, declares(commands::menu_group_toolbar), "and the grouping and sorting");

	// locations.md: the totals are their own affordance beside the grouping, not part of its button.
	const auto totals = std::ranges::find(declared.bottom, bar_entry{bar_control::item_totals});
	assert_equal(true,
	             totals != declared.bottom.end() && std::next(totals) != declared.bottom.end() &&
	                 *std::next(totals) == bar_entry{commands::menu_group_toolbar},
	             "the totals stand directly beside the grouping");

	// The bar starts under the sidebar with one toolbar: the Options cog, beside the overview of the
	// collection Options opens on; Tools, Playback options and Recent changes; then Open, labelled with what
	// is selected, as a file browser's status bar says it. Commands alone, so the frame builds them as a single
	// toolbar.
	const std::vector<bar_entry> leading = {
		commands::options,          commands::menu_tools, commands::playback_menu, commands::menu_recent_changes,
		commands::menu_open_status,
	};
	assert_equal(true, declared.bottom_leading == leading,
	             "the status bar starts with Options, Tools, Playback, Recent changes, Open");

	for (const auto& entry : leading)
	{
		assert_equal(false, declares(entry), "none is repeated at the trailing end");
	}
}

// A file browser's status bar says what is selected: how many, and how big, which is how Open in Items'
// status bar is labelled. With nothing selected it says nothing, and Open shows its own name instead; a
// count with no size to tell stands alone.
static void should_summarise_the_selection_for_the_status_bar()
{
	assert_equal(std::string{}, format_selection_summary({}), "nothing selected says nothing");

	const auto make_item = [](const std::string_view name, const uint64_t size)
	{
		df::index_file_item indexed;
		indexed.name = core::cache(name);
		indexed.ft = files::file_type_from_name(name);
		indexed.size = core::file_size(size);
		return std::make_shared<df::item_element>(core::file_path(core::folder_path("c:\\selection"), name), indexed);
	};

	df::item_set two;
	make_item("selection-1.jpg", 1500)->add_to(two);
	make_item("selection-2.mp4", 2500)->add_to(two);
	assert_equal(std::format("{} \u00B7 {}", format_plural_text(tt.title_item_count_fmt, 2),
	                         platform::format_size(core::file_size(uint64_t{4000}))),
	             format_selection_summary(two), "two files read as their count and their total size");

	df::item_set sizeless;
	make_item("selection-empty.jpg", 0)->add_to(sizeless);
	assert_equal(format_plural_text(tt.title_item_count_fmt, 1), format_selection_summary(sizeless),
	             "with no size to tell, the count stands alone");
}

// A task view's commands are its own, so they sit at the end of the status bar under the controls
// they act on; the title bar keeps what leaves the view. A Run left at the top would be the one
// command not where the others are.
static void should_put_task_commands_in_the_status_bar()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());

	struct task
	{
		std::shared_ptr<view_base> view;
		commands run;
	};

	const std::vector<task> tasks = {
		{std::make_shared<rename_view>(state, nullptr), commands::tool_run},
		{std::make_shared<batch_tool_view>(state, nullptr), commands::tool_run},
		{std::make_shared<import_view>(state, nullptr), commands::import_run},
		{std::make_shared<sync_view>(state, nullptr), commands::sync_run},
		{std::make_shared<tags_view>(state, nullptr), commands::tags_run},
	};

	const auto contains = [](const std::vector<bar_entry>& entries, const commands id)
	{
		return std::ranges::find(entries, bar_entry{id}) != entries.end();
	};

	for (const auto& [view, run] : tasks)
	{
		const auto declared = view->bars();
		assert_equal(false, declared.navigation, "a task view has no address box");
		assert_equal(true, contains(declared.top, commands::view_close), "Close leaves the view from the title bar");
		assert_equal(false, contains(declared.top, run), "the task's run command is not in the title bar");
		assert_equal(true, contains(declared.bottom, run), "it ends the status bar");
		assert_equal(false, contains(declared.bottom, commands::view_close), "where Close is not");
	}

	// A run in progress replaces the view's commands with the one that stops it, so nothing else can
	// be started under it and no view has to declare Cancel beside commands it would dim.
	const auto& running = app_frame::running_task_bar();
	assert_equal(1_z, running.size(), "a running task's status bar offers one command");
	assert_equal(true, running.front() == bar_entry{commands::view_cancel}, "and that command is Cancel");

	for (const auto& t : tasks)
	{
		assert_equal(false, contains(t.view->bars().bottom, commands::view_cancel),
		             "no view declares Cancel among its own commands");
	}
}

// The Options view is a page of settings, not a task: its status bar offers Help, then Cancel, then Save
// options and close, which ends it as a task's Run does. Close stays in the title bar.
static void should_end_the_options_view_with_save()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());

	const options_view options(state, nullptr, {});
	const auto declared = options.bars();
	const std::vector<bar_entry> expected = {commands::options_help, commands::options_cancel, commands::options_save};

	assert_equal(false, declared.navigation, "Options has no address box");
	assert_equal(true, declared.bottom == expected, "Help, Cancel, then Save options and close");
	assert_equal(true, std::ranges::find(declared.top, bar_entry{commands::view_close}) != declared.top.end(),
	             "Close is in the title bar");
	assert_equal(false, options._show_render_window, "the page fills the window, with no view beside it");
}

static void should_preserve_newer_window_reuse_preferences()
{
	temp_files temps;
	const auto folder = temps.next_folder("window-reuse-preference");
	const core::scope_exit restore([profile = command_line.profile_folder]
	{ use_profile_folder(core::folder_path(profile)); });
	use_profile_folder(folder);

	settings_t first;
	first.read();
	assert_equal(true, first.reuse_existing_window, "a missing stored preference defaults to reuse");
	settings_t newer;
	newer.read();
	newer.reuse_existing_window = false;
	newer.write();
	first.write();

	settings_t observed;
	observed.read();
	assert_equal(false, observed.reuse_existing_window, "an unchanged default does not undo another window's opt-out");

	first.reuse_existing_window = false;
	first.write();
	newer.reuse_existing_window = true;
	newer.write();
	first.show_hidden = !first.show_hidden;
	first.write();
	observed.read();
	assert_equal(true, observed.reuse_existing_window,
	             "an older disabled window saving or closing keeps the newer choice");

	newer.reuse_existing_window = false;
	newer.write();
	observed.read();
	assert_equal(false, observed.reuse_existing_window, "a deliberate later preference change still persists");
}

// Saving the page copies what it edits and nothing else. What the app changed while it was open - the
// thumbnail size, a task's remembered fields, the version on offer - is newer than the page's copy.
static void should_save_only_what_the_options_view_edits()
{
	settings_t edited;
	settings_t current;

	edited.reuse_existing_window = false;
	assert_equal(true, edited_options_differ(edited, current), "turning off window reuse makes Options dirty");
	edited.show_hidden = !current.show_hidden;
	edited.subtitles = subtitle_mode::always;
	edited.subtitle_language = "de";
	edited.language = "fr";
	edited.collection.more_folders = R"(D:\Films)";
	edited.collection.library_types = "films_tv\tD:\\Films";
	edited.sidebar.show_world_map = !current.sidebar.show_world_map;
	edited.search.title[2] = "Holidays";
	edited.favorite_tags = "beach family";
	edited.jpeg_save_quality = 55;
	edited.slideshow_delay = 1000;

	edited.item_scale = 1;
	edited.write_folder = R"(C:\stale)";
	edited.available_version = "stale";
	edited.copyright_notice = "stale";
	current.item_scale = 6;
	current.write_folder = R"(C:\newer)";
	current.available_version = "newer";
	current.copyright_notice = "newer";

	apply_edited_options(edited, current);

	assert_equal(edited.show_hidden, current.show_hidden, "an application option");
	assert_equal(false, current.reuse_existing_window, "saving Options can turn off default window reuse");
	assert_equal(true, current.subtitles == subtitle_mode::always && current.subtitle_language == "de", "subtitles");
	assert_equal("fr", current.language, "the language");
	assert_equal(R"(D:\Films)", current.collection.more_folders, "the collection's folders");
	assert_equal("films_tv\tD:\\Films", current.collection.library_types, "and what they hold");
	assert_equal(edited.sidebar.show_world_map, current.sidebar.show_world_map, "the sidebar");
	assert_equal("Holidays", current.search.title[2], "a favourite search");
	assert_equal("beach family", current.favorite_tags, "the favourite tags");
	assert_equal(55, current.jpeg_save_quality, "a saving option");
	assert_equal(settings_t::max_slideshow_delay, current.slideshow_delay, "a delay kept in range");

	assert_equal(6, current.item_scale, "the thumbnail size is not the page's");
	assert_equal(R"(C:\newer)", current.write_folder, "nor a task's folder");
	assert_equal("newer", current.available_version, "nor what the app learnt while the page was open");
	assert_equal("newer", current.copyright_notice, "nor what a metadata task remembers");
}

// The page pauses what is playing while it shows, as the dialogs it replaced did, and resumes it however it is
// left. Opened again while showing - a key pressed before its command is disabled - it keeps the copy its
// controls edit and the pause it took: a second pause would find nothing playing and lose the first.
static void should_pause_media_while_the_options_view_shows()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());

	struct release_counter
	{
		int& releases;
		explicit release_counter(int& r) : releases(r) {}
		~release_counter() { ++releases; }
		release_counter(const release_counter&) = delete;
		release_counter& operator=(const release_counter&) = delete;
	};

	auto pauses = 0;
	auto resumes = 0;
	options_view_actions actions;
	actions.pause_media = [&]
	{
		++pauses;
		return std::static_pointer_cast<void>(std::make_shared<release_counter>(resumes));
	};

	const auto options = std::make_shared<options_view>(state, nullptr, actions);

	options->open(options_section::top);
	assert_equal(true, state.view_mode() == view_type::options, "the page shows");
	assert_equal(1, pauses, "what is playing pauses as the page opens");

	options->open(options_section::sidebar);
	assert_equal(1, pauses, "opened again, it takes no second pause");
	assert_equal(0, resumes, "and keeps the first");

	options->deactivate();
	assert_equal(1, resumes, "leaving the page, by any route, resumes what it paused");

	options->cancel();
	assert_equal(true, state.view_mode() == view_type::items, "Cancel returns to Items");
	assert_equal(1, resumes, "once");
}

// Leaving the page by Close or Escape, or closing Diffractor, with changes it has not saved asks first: save
// them, close without them, or stay. With nothing changed - a change put back, or stored text the page writes
// back in another form - it closes without asking, and Cancel, which says it discards, is never asked.
static void should_ask_before_leaving_options_with_changes()
{
	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());

	const auto saved_settings = setting;
	const core::scope_exit restore([saved_settings] { setting = saved_settings; });
	const auto original = setting.show_hidden;
	// As toggle_collection_entry and older builds wrote it, which the page writes back another way.
	setting.collection.more_folders = "D:\\Films\r\nE:\\Music\r\n";

	auto asked = 0;
	auto answer = options_leave_choice::stay;
	options_view_actions actions;
	actions.ask_to_save = [&]
	{
		++asked;
		return answer;
	};

	const auto options = std::make_shared<options_view>(state, nullptr, actions);
	const auto change = [&options, original]
	{
		options->editing()->settings.show_hidden = !original;
	};

	options->open(options_section::top);
	assert_equal(false, options->has_changes(), "a page just opened holds no changes");
	options->exit();
	assert_equal(0, asked, "so Close does not ask");
	assert_equal(true, state.view_mode() == view_type::items, "it just closes");

	options->open(options_section::top);
	change();
	assert_equal(true, options->has_changes(), "a changed option is a change");
	options->editing()->settings.show_hidden = original;
	assert_equal(false, options->has_changes(), "put back, it is none");

	change();
	options->exit();
	assert_equal(1, asked, "with a change, Close asks");
	assert_equal(true, state.view_mode() == view_type::options, "staying keeps the page");
	assert_equal(true, options->has_changes(), "and the change");
	assert_equal(true, options->escape(), "Escape is the page's to answer");
	assert_equal(2, asked, "and asks as Close does");

	answer = options_leave_choice::discard;
	options->exit();
	assert_equal(true, state.view_mode() == view_type::items, "closing without saving leaves the page");
	assert_equal(original, setting.show_hidden, "and the option as it was");

	options->open(options_section::top);
	change();
	answer = options_leave_choice::save;
	options->exit();
	assert_equal(true, state.view_mode() == view_type::items, "saving leaves the page");
	assert_equal(!original, setting.show_hidden, "with the change made");
	setting.show_hidden = original;

	options->open(options_section::top);
	change();
	const auto asked_before_cancel = asked;
	options->cancel();
	assert_equal(asked_before_cancel, asked, "Cancel is not asked");
	assert_equal(original, setting.show_hidden, "and discards");

	options->open(options_section::top);
	assert_equal(true, options->confirm_exit(), "with nothing changed Diffractor closes without asking");
	change();
	answer = options_leave_choice::stay;
	assert_equal(false, options->confirm_exit(), "with a change, staying keeps Diffractor open");
	answer = options_leave_choice::discard;
	assert_equal(true, options->confirm_exit(), "closing without saving lets it close");
	assert_equal(original, setting.show_hidden, "without the change");

	// What opens a new scope - a file opened from Explorer among them - asks the same before the page gives way.
	options->open(options_section::top);
	change();
	answer = options_leave_choice::stay;
	assert_equal(false, options->can_exit(), "a new scope asks first, and staying keeps the page");
	assert_equal(true, options->has_changes(), "with its change");
	answer = options_leave_choice::discard;
	assert_equal(true, options->can_exit(), "closing without saving lets it go");
	assert_equal(original, setting.show_hidden, "without the change");

	// A change is in what the page edits, its nested and listed settings too, and in nothing else.
	const settings_t before;
	auto after = before;
	after.item_scale = before.item_scale + 1;
	assert_equal(false, edited_options_differ(before, after), "a setting the page does not edit is no change");
	after.search.title[3] = "Holidays";
	assert_equal(true, edited_options_differ(before, after), "a favourite search is one");
	after = before;
	after.collection.library_types = "films_tv\tD:\\Films";
	assert_equal(true, edited_options_differ(before, after), "and so is what a folder holds");
}

// A library type belongs to a folder of the collection: a ticked local folder, another folder named by its
// path, or one given a type before. Exclusions and device labels name no one folder.
static void should_offer_library_types_for_collection_folders()
{
	// Each platform's own paths: a share is a UNC path on Windows and a mount elsewhere.
	const auto windows = core::windows_path_semantics;
	const auto pictures = windows ? R"(C:\Users\me\Pictures)"s : "/home/me/Pictures"s;
	const auto videos = windows ? R"(C:\Users\me\Videos)"s : "/home/me/Videos"s;
	const auto share = windows ? R"(\\nas\video)"s : "/mnt/nas/video"s;
	const auto family = windows ? R"(\\nas\video\Family)"s : "/mnt/nas/video/Family"s;
	const auto excluded = windows ? R"(-c:\secret)"s : "-/secret"s;

	platform::local_folders_result local;
	local.pictures = core::folder_path(pictures);
	local.video = core::folder_path(videos);
	local.music = core::folder_path(windows ? R"(C:\Users\me\Music)" : "/home/me/Music");

	settings_t::index_t collection;
	collection.pictures = true;
	collection.video = true;
	collection.more_folders = std::format("{}\n{}\n-proxy*\nMyPassport\n{}\n", share, excluded, videos);
	collection.library_types = std::format("personal\t{}\nfilms_tv\t{}", family, share);

	const auto folders = library_type_folders(collection, local);
	const std::vector<core::folder_path> expected = {core::folder_path(pictures), core::folder_path(videos),
	                                                 core::folder_path(share), core::folder_path(family)};

	assert_equal(expected.size(), folders.size(), "ticked, named and typed folders, each once");

	for (auto i = 0_z; i < std::min(expected.size(), folders.size()); ++i)
	{
		assert_equal(expected[i].text().sv(), folders[i].text().sv(), std::format("folder {}", i));
	}
}

// The bars right-align their parts and give what is left to the title or the status. Short of room,
// a bar gives up what is optional, then narrows what can narrow, and then drops whole parts from the
// leading end - the trailing end, where the commands are, is the last to go, and the last part
// standing is clipped rather than lost.
static void should_arrange_a_bar_from_its_trailing_end()
{
	constexpr recti bounds(10, 0, 410, 40);
	constexpr auto gap = 4;
	const std::vector<bar_slot> slots = {
		{{100, 20}, 0, true}, // optional, like the thumbnail size
		{{150, 30}, 80},      // narrows, like the filter box
		{{120, 36}},          // fixed, like a toolbar
	};

	const auto roomy = arrange_bar(bounds, slots, gap);
	assert_equal(true, roomy.slots[2] == recti(290, 2, 410, 38), "the last slot ends the bar, centred in it");
	assert_equal(true, roomy.slots[1] == recti(136, 5, 286, 35), "the one before it sits a gap to its left");
	assert_equal(true, roomy.slots[0] == recti(32, 10, 132, 30), "and so on, in declared order");
	assert_equal(true, roomy.remainder == recti(10, 0, 28, 40), "the leading end keeps what is left");

	const auto tight = arrange_bar(recti(10, 0, 310, 40), slots, gap);
	assert_equal(true, tight.slots[0].is_empty(), "an optional slot is the first given up");
	assert_equal(150, tight.slots[1].width(), "while the others still fit at their size");
	assert_equal(true, tight.slots[2] == recti(190, 2, 310, 38), "and the trailing slot has not moved");

	const auto tighter = arrange_bar(recti(10, 0, 220, 40), slots, gap);
	assert_equal(true, tighter.slots[0].is_empty(), "the optional slot stays given up");
	assert_equal(86, tighter.slots[1].width(), "a narrowing slot gives up width next");
	assert_equal(120, tighter.slots[2].width(), "before the trailing slot loses any");
	assert_equal(0, tighter.remainder.width(), "nothing is left for the status");

	const auto cramped = arrange_bar(recti(10, 0, 150, 40), slots, gap);
	assert_equal(true, cramped.slots[1].is_empty(), "past its minimum a leading slot goes whole");
	assert_equal(true, cramped.slots[2] == recti(30, 2, 150, 38), "so the trailing slot still fits whole");

	const auto clipped = arrange_bar(recti(10, 0, 70, 40), slots, gap);
	assert_equal(60, clipped.slots[2].width(), "a lone slot wider than the bar is cut to the bar");
	assert_equal(true, clipped.slots[2].right == 70, "from its leading end");

	const auto empty = arrange_bar(bounds, {}, gap);
	assert_equal(true, empty.remainder == bounds, "a bar with nothing declared is all remainder");
}

// A run of a view's own commands short of room gives way from its leading end into a More menu, so the command
// that ends it - here Movie's Render - stays on the bar; given the room again, the run shows whole.
static void should_keep_a_short_status_bar_s_last_command()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();
	app->create_toolbars();
	app->_view = app->_view_movie;
	ui::test::fake_measure_context mc({8, 16});

	const auto render = app->find_command(commands::tool_movie_render);
	const auto add = app->find_command(commands::tool_movie_add);
	const auto more = app->find_command(commands::menu_bar_more);
	const auto lay_out = [&](const int width)
	{
		std::vector<ui::control_ptr> shown;
		app->layout_status_bar(mc, {width, 40}, shown);
		assert_equal(false, shown.empty(), "the status bar shows the view's commands");
		const auto toolbar = std::dynamic_pointer_cast<ui::toolbar>(shown.back());
		assert_equal(true, toolbar != nullptr, "as a toolbar");
		return toolbar;
	};

	const auto roomy = lay_out(3000);
	assert_equal(true, app->_bar_overflow.empty(), "with room nothing gives way");
	assert_equal(false, roomy->button_bounds(add).is_empty(), "the run starts with Add");
	assert_equal(true, roomy->button_bounds(more).is_empty(), "and has no More button");

	const auto short_bar = lay_out(300);
	assert_equal(false, short_bar->button_bounds(render).is_empty(), "short of room, Render still ends the bar");
	assert_equal(false, short_bar->button_bounds(more).is_empty(), "after a More button");
	assert_equal(true, short_bar->button_bounds(add).is_empty(), "standing in for the commands before it");
	assert_equal(true, std::ranges::find(app->_bar_overflow, add) != app->_bar_overflow.end(), "which its menu lists");
	assert_equal(true, std::ranges::find(app->_bar_overflow, render) == app->_bar_overflow.end(),
	             "and not the command still shown");
	assert_equal(true, more->menu() == app->_bar_overflow, "More opens on exactly those");

	lay_out(3000);
	assert_equal(true, app->_bar_overflow.empty(), "given the room again, nothing is left over");
}

// The status bar starts from both ends. What a view starts it with is arranged from the leading end,
// in the room its trailing controls leave, by the same rules mirrored: short of room, the part
// furthest from the end it starts from goes first. A part with nothing to show takes no room, not
// even the gap beside it.
static void should_arrange_a_bar_from_its_leading_end()
{
	constexpr recti bounds(10, 0, 410, 40);
	constexpr auto gap = 4;
	const std::vector<bar_slot> slots = {
		{{120, 36}}, // a toolbar, like the one Items starts its status bar with
		{{0, 0}},    // nothing to show
		{{150, 30}}, // text, like the totals
	};

	const auto roomy = arrange_bar(bounds, slots, gap, bar_alignment::leading);
	assert_equal(true, roomy.slots[0] == recti(10, 2, 130, 38), "the first slot starts the bar, centred in it");
	assert_equal(true, roomy.slots[1].is_empty(), "a slot with nothing to show is not placed");
	assert_equal(true, roomy.slots[2] == recti(134, 5, 284, 35), "so the next sits one gap after the first");
	assert_equal(true, roomy.remainder == recti(288, 0, 410, 40), "and the trailing end keeps what is left");

	const auto cramped = arrange_bar(recti(10, 0, 200, 40), slots, gap, bar_alignment::leading);
	assert_equal(true, cramped.slots[2].is_empty(), "short of room the slot furthest from the start goes");
	assert_equal(true, cramped.slots[0] == recti(10, 2, 130, 38), "while the first keeps its place");

	const auto trailing = arrange_bar(bounds, slots, gap);
	assert_equal(true, trailing.slots[1].is_empty(), "nor is one at the trailing end");
	assert_equal(true, trailing.slots[0] == recti(136, 2, 256, 38), "which leaves no gap for it either");
}

// design.md "Navigation and search" specifies the address box as an editing session with a
// committed address, a typed draft and a previewed completion. The two-stage Escape is the part a
// user notices: losing it either throws away what they typed or traps them in the popup.
static void should_run_an_address_editing_session()
{
	search_edit_session session;
	session.begin("c:\\photos");

	assert_equal("c:\\photos", session.original_text(), "focusing snapshots the committed address");
	assert_equal("c:\\photos", session.typed_text(), "the draft starts at the committed address");
	assert_equal(false, session.is_previewing(), "a new session previews nothing");

	// Typing maintains a draft separate from the committed address.
	session.typed("bea");
	assert_equal("bea", session.typed_text(), "typing updates the draft");
	assert_equal("c:\\photos", session.original_text(), "typing does not disturb the committed address");

	// Up and Down preview a completion without changing the draft.
	session.preview("beach");
	assert_equal(true, session.is_previewing(), "a highlighted completion is previewing");
	assert_equal("bea", session.typed_text(), "previewing leaves the draft alone");

	// First Escape: back to the draft, popup stays open, first completion highlighted.
	const auto first_escape = session.escape();
	assert_equal(true, first_escape.handled, "escape is handled");
	assert_equal("bea", first_escape.edit_text, "the first escape restores the typed draft");
	assert_equal(true, first_escape.select_first_completion, "the first escape highlights the first completion");
	assert_equal(false, first_escape.close_popup, "the first escape keeps the popup open");
	assert_equal(false, first_escape.focus_view, "the first escape keeps address focus");
	assert_equal(false, session.is_previewing(), "the first escape ends the preview");

	// Second Escape: back to where editing began, popup closes, focus returns to the content view.
	const auto second_escape = session.escape();
	assert_equal("c:\\photos", second_escape.edit_text, "the second escape restores the committed address");
	assert_equal(true, second_escape.close_popup, "the second escape closes the popup");
	assert_equal(true, second_escape.focus_view, "the second escape returns focus to the content view");
	assert_equal(false, second_escape.select_first_completion, "the second escape highlights nothing");

	// Escape with nothing previewed goes straight to the committed address.
	session.begin("c:\\photos");
	session.typed("bea");
	const auto direct = session.escape();
	assert_equal("c:\\photos", direct.edit_text, "escape without a preview restores the committed address");
	assert_equal(true, direct.close_popup, "escape without a preview closes the popup");
}

// design.md: "Plain Tab accepts the highlighted completion into the draft without running it,
// refreshes completions, keeps the popup open, and retains address focus."
static void should_accept_a_completion_without_running_it()
{
	search_edit_session session;
	session.begin("c:\\photos");
	session.typed("bea");
	session.preview("beach");

	const auto accepted = session.accept(true, "beach ");
	assert_equal(true, accepted.handled, "tab with a highlighted completion is handled");
	assert_equal("beach ", accepted.edit_text, "tab fills in the completion");
	assert_equal(true, accepted.refresh_completions, "tab refreshes completions so the token can extend");
	assert_equal(false, accepted.close_popup, "tab keeps the popup open");
	assert_equal(false, accepted.focus_view, "tab retains address focus");

	// The accepted text becomes the draft, so a later preview-then-Escape returns here, not to "bea".
	assert_equal("beach ", session.typed_text(), "the accepted completion becomes the draft");
	assert_equal(false, session.is_previewing(), "accepting ends the preview");

	session.preview("beach huts");
	assert_equal("beach ", session.escape().edit_text, "escape after tab returns to the accepted draft");

	// With nothing highlighted, Tab is not ours to handle - it must fall through to focus traversal.
	search_edit_session untouched;
	untouched.begin("c:\\photos");
	const auto ignored = untouched.accept(false, {});
	assert_equal(false, ignored.handled, "tab without a completion is not handled");
	assert_equal(false, ignored.set_edit_text, "tab without a completion changes nothing");
}

// A preview is a suggestion the user is looking at, not one they chose. Ending the session left it
// sitting in the address box, where the next Enter - and anything else that reads the box - takes
// it for the address. The draft the user actually typed has to come back with them.
static void should_restore_the_draft_when_a_previewing_session_ends()
{
	search_edit_session session;
	session.begin("c:\\photos");
	session.typed("bea");
	session.preview("beach huts");

	const auto ended = session.end();
	assert_equal(true, ended.set_edit_text, "ending while previewing puts the box back");
	assert_equal("bea", ended.edit_text, "and what it puts back is the draft that was typed");
	assert_equal(false, session.is_previewing(), "the preview is over");

	// Nothing to undo when no preview was showing: the box already holds what the user typed.
	search_edit_session typed_only;
	typed_only.begin("c:\\photos");
	typed_only.typed("bea");
	assert_equal(false, typed_only.end().set_edit_text, "ending without a preview leaves the box alone");

	// And accepting a completion makes it the draft, so ending after that keeps it.
	search_edit_session accepted;
	accepted.begin("c:\\photos");
	accepted.typed("bea");
	accepted.preview("beach");
	accepted.accept(true, "beach ");
	assert_equal(false, accepted.end().set_edit_text, "an accepted completion is the draft and stays");
}

// design.md: "Enter commits the highlighted completion when one is selected, otherwise it commits
// the visible address."
static void should_commit_the_highlighted_completion_or_the_visible_address()
{
	search_edit_session session;
	session.begin("c:\\photos");
	session.typed("bea");

	assert_equal("beach", session.commit(true, "beach", "bea"), "enter commits the highlighted completion");
	assert_equal("bea", session.commit(false, {}, "bea"),
	             "enter commits the visible address when nothing is highlighted");

	// A preview shows in the address box, so committing with no selection still runs what is visible.
	session.preview("beach");
	assert_equal("beach", session.commit(false, {}, "beach"), "enter runs the address the user can see");
}

// design.md separates three named behaviors that all decide what happens when an item ends:
// Slideshow, Continue with the next item, and Repeat. They interact, and the interaction is the
// part a user reports as "it stopped" or "it played two at once".
static void should_decide_what_follows_an_item()
{
	// A photo in a slideshow, with a next item waiting.
	const auto photo_in_slideshow = []
	{
		playback_tick t;
		t.is_slideshow = true;
		t.is_photo = true;
		t.can_next = true;
		t.has_next = true;
		return t;
	};

	// Nothing has ended yet, so nothing happens.
	{
		auto t = photo_in_slideshow();
		assert_equal(true, calc_playback_advance(t) == playback_advance::none, "an unfinished photo is left alone");
	}

	// design.md: "a folder, document, or archive is stepped over rather than stalling the sequence",
	// and a slideshow that reaches one ends instead of appearing to keep playing.
	{
		playback_tick t;
		t.is_slideshow = true;
		t.can_next = true;
		t.has_next = true;
		assert_equal(true, calc_playback_advance(t) == playback_advance::stop,
		             "a slideshow on something that cannot play or time out stops");
	}

	// The delay elapses and the sequence moves on.
	{
		auto t = photo_in_slideshow();
		t.photo_delay_elapsed = true;
		assert_equal(true, calc_playback_advance(t) == playback_advance::advance, "an elapsed photo advances");
	}

	// design.md: "A slideshow always continues, because continuing is what the mode means" - even
	// with Continue with the next item switched off.
	{
		auto t = photo_in_slideshow();
		t.photo_delay_elapsed = true;
		t.auto_advance = false;
		assert_equal(true, calc_playback_advance(t) == playback_advance::advance,
		             "a slideshow continues regardless of the auto advance preference");
	}

	// Browsing normally, the preference decides.
	{
		playback_tick t;
		t.is_av = true;
		t.media_ended = true;
		t.can_next = true;
		t.has_next = true;
		assert_equal(true, calc_playback_advance(t) == playback_advance::stop,
		             "browsing without auto advance stops at the end of an item");

		t.auto_advance = true;
		assert_equal(true, calc_playback_advance(t) == playback_advance::advance,
		             "browsing with auto advance moves to the next item");
	}

	// design.md: "repeat one holds on the current item" - and it outranks everything below it.
	{
		auto t = photo_in_slideshow();
		t.photo_delay_elapsed = true;
		t.repeat = repeat_mode::repeat_one;
		t.has_next = true;
		assert_equal(true, calc_playback_advance(t) == playback_advance::hold, "repeat one holds on the item");

		t.has_next = false;
		assert_equal(true, calc_playback_advance(t) == playback_advance::hold,
		             "repeat one holds even with nothing to advance to");
	}

	// design.md: "repeat all returns to the first item after the last, and no repeat stops there".
	{
		auto t = photo_in_slideshow();
		t.photo_delay_elapsed = true;
		t.has_next = false;
		t.has_wrapped_next = true;

		t.repeat = repeat_mode::repeat_none;
		assert_equal(true, calc_playback_advance(t) == playback_advance::stop, "no repeat stops at the last item");

		t.repeat = repeat_mode::repeat_all;
		assert_equal(true, calc_playback_advance(t) == playback_advance::advance, "repeat all wraps to the first item");

		// Wrapping onto the only item must restart it in place; advancing would be a no-op select
		// that leaves the player paused on the first frame.
		t.wrapped_is_current = true;
		assert_equal(true, calc_playback_advance(t) == playback_advance::hold,
		             "wrapping onto the displayed item restarts it in place");
	}

	// Nothing to continue to at all.
	{
		auto t = photo_in_slideshow();
		t.photo_delay_elapsed = true;
		t.has_next = false;
		t.repeat = repeat_mode::repeat_all;
		t.has_wrapped_next = false;
		assert_equal(true, calc_playback_advance(t) == playback_advance::stop,
		             "repeat all with nothing to wrap to stops");
	}

	// The view cannot play: the sequence ends rather than fighting it.
	{
		auto t = photo_in_slideshow();
		t.photo_delay_elapsed = true;
		t.can_next = false;
		t.repeat = repeat_mode::repeat_one;
		assert_equal(true, calc_playback_advance(t) == playback_advance::stop,
		             "a sequence that cannot advance stops, even under repeat one");
	}
}

// A menu or a command acts on the selection, so the sequence does not move it while one is open: a photo
// that has had its time waits, and the slideshow goes on to the next once the menu has gone.
static void should_hold_a_slideshow_while_a_menu_is_open()
{
	browsing_fixture fixture;
	auto& s = fixture.state;
	const auto active = ui::command_active.load();
	const auto repeat = setting.repeat;
	const core::scope_exit restore([&s, active, repeat]
	{
		s.stop();
		ui::command_active = active;
		setting.repeat = repeat;
	});
	setting.repeat = repeat_mode::repeat_none;

	s.filter().add_group(file_group::photo);
	s.update_item_groups();
	df::item_element_ptr first;
	auto photos = 0;
	for (const auto& group : s.groups())
	{
		for (const auto& item : group->items())
		{
			if (item->is_folder() || !item->file_type()->has_trait(file_traits::bitmap)) continue;
			if (!first) first = item;
			++photos;
		}
	}
	assert_equal(true, photos >= 2, "the fixture shows photos to walk");
	s.select(fixture.view, first, false, false, false);
	s.update_selection();
	s.toggle_slideshow(fixture.view);
	const auto display = s.display_state();
	assert_equal(true, display && display->is_slideshow(), "the slideshow runs");

	display->_next_photo_tick = 1'000'000;
	ui::command_active = 1;
	s.tick(fixture.view, core::now());
	assert_equal(true, s.focus_item() == first, "the photo waits while a menu is open");
	assert_equal(true, display->is_slideshow(), "and the slideshow goes on");

	ui::command_active = 0;
	s.tick(fixture.view, core::now());
	assert_equal(true, s.focus_item() != nullptr && s.focus_item() != first, "it moves on once the menu has gone");
}

// index_state::auto_complete_words asserts it is not on the UI thread, because the real app reads
// the vocabulary on the auto_complete worker. The hop is modelled rather than bypassed, so these
// tests exercise the same threading the shipped code does.
static void run_completion_worker(deferred_async_strategy& as)
{
	auto ran = false;
	std::thread worker([&as, &ran] { ran = as.run_next(async_queue::auto_complete); });
	worker.join();
	assert_equal(true, ran, "a completion pass was queued");
}

// The order of what the address bar offers is the part a user judges. These rules live in
// search_auto_complete::search and had no coverage: it is reached only through the address bar's
// edit, so the strategy is built here through its factory instead.
static void should_rank_address_completions(shared_test_context& stc)
{
	stc.lazy_load_index();
	stc.test_index.update_summary();

	null_state_strategy ss;
	deferred_async_strategy as;
	view_state s(ss, as, stc.test_index, make_test_player());
	s.view_mode(view_type::items);

	const auto completes = make_search_auto_complete(s, [](const std::string&) {});
	completes->initialise([](const ui::auto_complete_results&) {});

	ui::auto_complete_results results;
	auto completions = 0;

	const auto run = [&](const std::string& query)
	{
		results.clear();
		completions = 0;
		completes->search(query, [&](const ui::auto_complete_results& r)
		{
			results = r;
			++completions;
		});
		run_completion_worker(as);
		as.drain_ui();
	};

	run("#key");

	assert_equal(1, completions, "a completion pass reports once");
	assert_equal(true, !results.empty(), "a query produces completions");

	// design.md: "Parsed input retains the user's visible spelling". Whatever else is offered, what
	// the user actually typed stays reachable at the top - Enter must never run something else.
	assert_equal("#key", results.front()->edit_text(), "the typed query is offered first");

	// The list is ranked, so a lower-weight suggestion can never appear above a higher one.
	for (auto i = 1u; i < results.size(); ++i)
	{
		assert_equal(true, results[i - 1]->weight >= results[i]->weight, "completions are ranked by weight");
	}

	// A repeated suggestion wastes a row and reads as a bug.
	core::hash_set<std::string, core::ihash, core::ieq> seen;

	for (const auto& r : results)
	{
		assert_equal(true, seen.emplace(r->edit_text()).second, std::format("'{}' is offered once", r->edit_text()));
		assert_equal(true, !r->edit_text().empty(), "a completion is never blank");
	}

	assert_equal(true, results.size() <= completes->max_predictions, "the list is capped");

	// A scoped token is answered from its own vocabulary. The indexed fixture carries
	// #key1/#key2/#key3, so this cannot pass vacuously.
	const auto tag_suggested = std::ranges::any_of(results, [](const ui::auto_complete_match_ptr& r)
	{ return core::icmp(r->edit_text(), "#key1") == 0; });
	assert_equal(true, tag_suggested, "an indexed tag is offered for a tag prefix");

	// That tag must also outrank the generic matches below it, or it is offered but unreachable.
	const auto tag_pos = std::ranges::find_if(results, [](const ui::auto_complete_match_ptr& r)
	{ return core::icmp(r->edit_text(), "#key1") == 0; });
	assert_equal(true, tag_pos != results.end() && tag_pos - results.begin() <= 3,
	             "a scoped suggestion is ranked near the top");

	// A term the user has typed before AND that the index knows arrives from several sources at
	// once, so this is where a missing de-duplication pass shows up as a repeated row.
	s.recent_tags.add("key1");
	completes->initialise([](const ui::auto_complete_results&) {});
	run("#key1");

	assert_equal(true, !results.empty(), "a known tag produces completions");

	core::hash_set<std::string, core::ihash, core::ieq> seen_known;

	for (const auto& r : results)
	{
		assert_equal(true, seen_known.emplace(r->edit_text()).second,
		             std::format("'{}' is offered once for a query with several sources", r->edit_text()));
	}
}

// design.md: the address box "also accepts folders and offers relevant completions". With nothing
// typed there is no query to complete, so the useful offer is where the user has already been.
static void should_offer_recent_searches_for_an_empty_address()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);

	s.recent_searches.add("@photo beach");
	s.recent_searches.add(std::string(test_files_folder().text()));
	const auto folder_query = std::string(test_files_folder().text()) + " Test";
	s.recent_searches.add(folder_query);

	const auto completes = make_search_auto_complete(s, [](const std::string&) {});
	completes->initialise([](const ui::auto_complete_results&) {});

	ui::auto_complete_results results;
	completes->search({}, [&results](const ui::auto_complete_results& r) { results = r; });
	run_completion_worker(as);
	as.drain_ui();

	const auto offered = [&results](const std::string_view text)
	{
		return std::ranges::any_of(results, [text](const ui::auto_complete_match_ptr& r)
		{ return core::icmp(r->edit_text(), text) == 0; });
	};

	assert_equal(true, offered("@photo beach"), "a recent query is offered");
	assert_equal(true, offered(test_files_folder().text()), "a recent folder is offered");
	assert_equal(true, offered(folder_query), "a recent folder-scoped query keeps its separate search term");
	assert_equal(false, offered(df::quote_path_term(folder_query)), "a recent query is not quoted as one folder");

	// An empty address has nothing typed, so nothing may be presented as the typed query.
	for (const auto& r : results)
	{
		assert_equal(true, !r->edit_text().empty(), "an empty address offers no blank completion");
	}
}

// Completions are computed off the UI thread while the user keeps typing. design.md requires the
// preview to follow what was typed, so an older pass must not deliver over a newer one.
static void should_discard_a_superseded_completion()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);

	const auto completes = make_search_auto_complete(s, [](const std::string&) {});
	completes->initialise([](const ui::auto_complete_results&) {});

	auto first_delivered = 0;
	auto second_delivered = 0;

	completes->search("bea", [&first_delivered](const ui::auto_complete_results&) { ++first_delivered; });
	completes->search("beach", [&second_delivered](const ui::auto_complete_results&) { ++second_delivered; });

	assert_equal(2_z, as.pending_worker_count(async_queue::auto_complete), "both passes were queued");

	run_completion_worker(as);
	run_completion_worker(as);
	as.drain_ui();

	assert_equal(0, first_delivered, "the superseded pass delivers nothing");
	assert_equal(1, second_delivered, "the newest pass delivers once");

	// The same guard must hold when the older pass finishes last, which is the case that actually
	// overwrites the user's typing.
	first_delivered = 0;
	second_delivered = 0;

	completes->search("bea", [&first_delivered](const ui::auto_complete_results&) { ++first_delivered; });
	run_completion_worker(as);

	completes->search("beach", [&second_delivered](const ui::auto_complete_results&) { ++second_delivered; });
	run_completion_worker(as);

	as.drain_ui();

	assert_equal(0, first_delivered, "a late older pass is still discarded");
	assert_equal(1, second_delivered, "the newest pass is the one that lands");
}

static void should_predict_face_book_names_in_the_address_bar()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	state.view_mode(view_type::items);
	index.faces().configure(true, true, false);
	index.faces().book().create("Robin Example");
	index.faces().book().create("Wren");
	const auto completes = make_search_auto_complete(state, [](const std::string&) {});
	completes->initialise([](const ui::auto_complete_results&) {});

	const auto offers = [&](const std::string& query, const std::string_view prediction)
	{
		ui::auto_complete_results results;
		completes->search(query, [&results](const ui::auto_complete_results& r) { results = r; });
		run_completion_worker(as);
		as.drain_ui();
		return std::ranges::any_of(results, [prediction](const auto& p) { return p->edit_text() == prediction; });
	};

	for (const std::string query : {"rob", "Robin Example", "person:rob", "exam", "person:exam"})
	{
		assert_equal(true, offers(query, "person:\"Robin Example\""),
		             std::format("'{}' offers a face-book name before it has been written into files", query));
	}

	// docs/faces.md#8-searching: a name chosen is the whole name, quoted even where it is one word.
	assert_equal(true, offers("wre", "person:\"Wren\""), "a one-word name is offered whole"sv);
	assert_equal(true, offers("rob!", "person:\"Robin Example\"!"), "a completion preserves confirmed-only"sv);
	assert_equal(true, offers("@photo person:\"Robin Ex", "@photo person:\"Robin Example\""),
	             "completing a quoted name keeps the preceding filters"sv);
}

// design.md: each prediction says how many items it finds, and a suggestion that finds more ranks above
// one of its kind that finds fewer. The number shown is the number running the search lists, and one
// drawn from the collection's own words, tags and places that would find nothing is not offered.
static void should_count_address_completions(shared_test_context& stc)
{
	stc.lazy_load_index();
	stc.test_index.update_summary();
	// As every summary pass does, so the snapshot holds what earlier tests changed in the shared index.
	stc.test_index.update_search_index();

	null_state_strategy ss;
	deferred_async_strategy as;
	view_state s(ss, as, stc.test_index, make_test_player());
	s.view_mode(view_type::items);

	const auto completes = make_search_auto_complete(s, [](const std::string&) {});
	completes->initialise([](const ui::auto_complete_results&) {});

	ui::auto_complete_results results;

	const auto run = [&](const std::string& query)
	{
		results.clear();
		completes->search(query, [&results](const ui::auto_complete_results& r) { results = r; });
		run_completion_worker(as);
		as.drain_ui();
	};

	const auto found_by = [&stc](const std::string& text)
	{
		return stc.test_index.count_matches(df::search_t::parse(text), test_token, index_state::search_execution::scan)
		    .total_items()
		    .count;
	};

	run("key");
	assert_equal(true, !results.empty(), "a query produces completions");
	assert_equal(true, results.front()->count.has_value(), "what was typed is counted too");

	auto counted = 0;

	for (const auto& r : results)
	{
		const auto text = r->edit_text();
		assert_equal(false, r->drop_when_empty && r->count == 0u, std::format("'{}' leads somewhere", text));

		if (r->count && !core::is_path(core::trim(text)))
		{
			++counted;
			assert_equal(found_by(text), *r->count, std::format("'{}' says how many it finds", text));
		}
	}

	assert_equal(true, counted > 1, "completions are counted");

	run("#key1 @");
	const auto combined = std::ranges::find_if(results, [](const auto& row)
	{ return core::icmp(row->edit_text(), "#key1 @photo") == 0; });
	assert_equal(true, combined != results.end(), "offers a kind from the matching items");
	if (combined != results.end())
		assert_equal(found_by("#key1 @photo"), (*combined)->count.value_or(0), "count is for both terms");

	run("2012-09-14 cam");
	assert_equal(true,
	             std::ranges::any_of(results, [](const auto& row)
	{ return row->edit_text() == "2012-09-14 camera:\"Canon EOS 7D\""; }),
	             "complete the camera carried by the pictures from that day, quoting its whole value");

	run("#key");

	std::vector<uint64_t> tag_counts;

	for (const auto& r : results)
	{
		if (r->drop_when_empty && core::starts(r->edit_text(), "#") && r->count) tag_counts.emplace_back(*r->count);
	}

	assert_equal(true, !tag_counts.empty(), "tags are suggested and counted");
	assert_equal(true, std::ranges::is_sorted(tag_counts, std::greater<>()), "a tag that finds more ranks higher");

	// Nothing in the collection holds zzzqqq, so nothing completed after it finds anything.
	run("zzzqqq ke");
	assert_equal(true, !results.empty(), "what was typed is still offered");
	assert_equal("zzzqqq ke", results.front()->edit_text(), "and offered first");

	for (const auto& r : results)
	{
		assert_equal(false, r->drop_when_empty, std::format("'{}' would find nothing", r->edit_text()));
	}
}

static void should_deliver_address_portraits_without_replacing_the_prediction()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto temp = test_temps.next_folder("prediction-portrait");
	const auto path = temp.combine_file("offline.jpg");
	database db(index);
	db.open(temp, "index");
	as.db = &db;
	index.save_thumbnail(path, files().load(test_files_folder().combine_file("Test.jpg"), false).i, {},
	                     core::date_t(2020, 1, 1));
	db.perform_writes();

	auto& faces = index.faces();
	faces.configure(true, true, false);
	faces::face_record record;
	record.stamp = core::date_t(2020, 1, 1);
	record.model = faces::model_id();
	record.extent = {128, 128};
	record.faces.emplace_back().box = {0.2, 0.2, 0.4, 0.4};
	const std::array batch{faces::placement_batch{0, "Robin", {{path, record.faces.front().box, 0}}}};
	faces.book().place(batch, faces::face_action::assign, faces.book().generation());
	faces.load(path, faces::pack_record(record));
	faces.loaded();
	as.run_next(async_queue::faces_overview);
	as.drain_ui();

	int repaints = 0;
	const auto completes = make_search_auto_complete(state, [](std::string) {}, [&] { ++repaints; });
	const auto run = [&]
	{
		std::shared_ptr<text_match> row;
		completes->search("rob", [&](const ui::auto_complete_results& results)
		{
			for (const auto& r : results)
				if (r->edit_text() == "person:\"Robin\"") row = std::dynamic_pointer_cast<text_match>(r);
		});
		run_completion_worker(as);
		as.drain_ui();
		return row;
	};

	const auto stale = run();
	assert_equal(true, stale && !stale->portrait, "the name is usable before its portrait is decoded");
	completes->clear();
	as.run_next(async_queue::thumbnail_decode);
	as.drain_ui();
	assert_equal(0, repaints, "closing the popup discards the queued portrait");

	const auto current = run();
	assert_equal(true, as.run_next(async_queue::thumbnail_decode), "a separate worker decodes the cached thumbnail");
	as.drain_ui();
	assert_equal(true, current && is_valid(current->portrait), "the same row receives its face without a source file");
	assert_equal(1, repaints, "the popup repaints without replacing its results");
}

static void should_gate_application_alpha_effects()
{
	const auto restore_animations = render::animations_enabled;
	const auto restore_step = ui::animation_step_factor;
	const core::scope_exit restore_scope([restore_animations, restore_step]
	{
		render::animations_enabled = restore_animations;
		ui::animation_step_factor = restore_step;
	});

	render::animations_enabled = false;
	assert_equal(0.65f, command_progress_alpha(true, 0, 0), "disabled indeterminate progress is stable");
	assert_equal(0.65f, command_progress_alpha(true, 0, 600), "disabled progress ignores time");
	assert_equal(false, command_progress_needs_animation(true, 0), "disabled progress requests no cadence");

	auto logo = core::color(0.2f, 0.3f, 0.4f, 0.8f);
	const auto logo_target = core::color(0.2f, 0.3f, 0.4f, 0.0f);
	assert_equal(true, ui::step_color_alpha(logo, logo_target), "disabled logo step changes once");
	assert_equal(0.0f, logo.a, "disabled logo step snaps to target alpha");

	render::animations_enabled = true;
	ui::animation_step_factor = 0.5f;
	assert_equal(true, command_progress_needs_animation(true, 0), "enabled progress requests cadence");
	assert_equal(true, command_progress_alpha(true, 0, 0) != command_progress_alpha(true, 0, 300),
	             "enabled progress still pulses");

	logo = core::color(0.2f, 0.3f, 0.4f, 0.8f);
	assert_equal(true, ui::step_color_alpha(logo, logo_target), "enabled logo step changes");
	assert_equal(0.4f, logo.a, "enabled logo step advances by the frame factor");
}

static void should_snap_app_logo_hover_when_animations_are_disabled()
{
	const auto restore_animations = render::animations_enabled;
	const core::scope_exit restore_scope([restore_animations] { render::animations_enabled = restore_animations; });

	browsing_fixture f;
	app_logo_element logo(f.state);

	render::animations_enabled = false;
	assert_equal(true, logo.hover(true), "hover enters");
	assert_equal(true, logo.step_background_if_animations_disabled(), "disabled hover snaps background");
	assert_equal(true, logo._bg_color.a > 0.0f, "hover highlight is visible immediately");

	assert_equal(true, logo.hover(false), "hover exits");
	assert_equal(true, logo.step_background_if_animations_disabled(), "disabled leave snaps background");
	assert_equal(0.0f, logo._bg_color.a, "hover highlight is hidden immediately");
}

// Faded, the logo's plasma keeps drifting at an eighth of its busy speed and is redrawn only when that
// moves the field; where drifting is not wanted it stays still, and work starting brings the colour back.
// Steps a quarter of a second apart keep the arithmetic exact.
static void should_keep_the_idle_logo_drifting_slowly()
{
	plasma p;
	auto t = 100.0;
	const auto step = [&p, &t](const bool busy, const bool drift)
	{
		t += 0.25;
		return p.step(t, busy, drift);
	};

	p.step(t, false, true);
	for (auto i = 0; i < 32; ++i) step(false, true);
	assert_equal(false, p.is_active(), "with no work running the plasma fades to the sidebar tone in 8 seconds");

	p._phase = 0.0;
	auto redraws = 0;
	for (auto i = 0; i < 16; ++i) redraws += step(false, true) ? 1 : 0;
	assert_equal(0.5, p._phase, "faded, four seconds drift the field half a second of its busy speed");
	assert_equal(10, redraws, "and it is redrawn only for the ten cosine indices that moved");

	for (auto i = 0; i < 16; ++i) redraws += step(false, false) ? 1 : 0;
	assert_equal(10, redraws, "where drifting is not wanted, a faded step changes nothing");
	assert_equal(0.5, p._phase, "and the field stays where it was");

	assert_equal(true, step(true, false), "work starting redraws it at once");
	assert_equal(true, p.is_active(), "fading back to colour at full speed");
}

static void should_skip_stable_progress_repaints_when_animations_are_disabled()
{
	const auto restore_animations = render::animations_enabled;
	const core::scope_exit restore_scope([restore_animations] { render::animations_enabled = restore_animations; });

	render::animations_enabled = false;
	assert_equal(false, status_repaints(true, 0, false).status_bar,
	             "stable disabled progress does not repaint the status bar");
	assert_equal(false, status_repaints(true, 0, false).view, "nor the view");
	assert_equal(true, status_repaints(true, 0, true).view, "debug overlays keep their live repaint");

	render::animations_enabled = true;
	assert_equal(true, status_repaints(true, 0, false).status_bar, "enabled indeterminate progress still pulses");
	assert_equal(false, status_repaints(true, 0, false).view, "in the status bar, not over the view");
	assert_equal(false, status_repaints(true, 12, false).status_bar, "counted progress repaints only as it counts");
}

static void run_destination_completion_worker(deferred_async_strategy& as)
{
	auto ran = false;
	std::thread worker([&as, &ran] { ran = as.run_next(async_queue::work); });
	worker.join();
	assert_equal(true, ran, "a destination completion pass was queued");
}

static std::shared_ptr<ui::complete_strategy_t> make_test_destination_completer(
	view_state& s, const std::shared_ptr<void>& lifetime, const std::shared_ptr<const core::folder_counts>& folders,
	destination_folder_enumerator enumerate_child_folders = {})
{
	return make_destination_folder_auto_complete_for_test(s, lifetime, folders, std::move(enumerate_child_folders));
}

static void should_clear_destination_completions_by_emptying_the_query()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.recent_folders.add("C:\\Recent");

	auto lifetime = std::make_shared<int>(1);
	auto folders = std::make_shared<core::folder_counts>();
	(*folders)[core::folder_path("C:\\Beta")] = 100;
	const auto completes = make_test_destination_completer(s, lifetime, folders);

	auto stale_delivered = 0;
	auto recents_delivered = 0;
	completes->search("Beta", [&stale_delivered](const ui::auto_complete_results&) { ++stale_delivered; });
	completes->search({}, [&recents_delivered](const ui::auto_complete_results& results)
	{
		++recents_delivered;
		assert_equal(1_z, results.size(), "empty destination query returns recents immediately");
	});

	run_destination_completion_worker(as);
	as.drain_ui();

	assert_equal(0, stale_delivered, "clearing the box supersedes the in-flight completion");
	assert_equal(1, recents_delivered, "the empty-query result remains current");
}

// A folder double-clicked in Copy or Move to folder is the destination chosen, as one typed is when OK is pressed.
static void should_choose_a_destination_folder_with_a_double_click()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.recent_folders.add("C:\\Recent");

	const auto lifetime = std::make_shared<int>(1);
	const auto dialog = std::make_shared<answered_test_frame>();
	const auto completes = make_destination_folder_auto_complete_for_test(s, lifetime,
	                                                                      std::make_shared<core::folder_counts>(), {},
	                                                                      dialog);
	ui::auto_complete_results recents;
	completes->search({}, [&recents](const ui::auto_complete_results& r) { recents = r; });
	assert_equal(1_z, recents.size(), "the recent destination is offered");
	if (recents.empty()) return;

	completes->selected(recents.front(), ui::complete_strategy_t::select_type::click);
	assert_equal(false, dialog->cancelled.has_value(), "a click only chooses it");
	completes->selected(recents.front(), ui::complete_strategy_t::select_type::double_click);
	assert_equal(true, dialog->cancelled == false, "a double-click answers the dialog as OK does");
}

static void should_rank_destination_completions_after_combining_sources()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	auto lifetime = std::make_shared<int>(1);
	auto folders = std::make_shared<core::folder_counts>();
	const auto parent = core::folder_path(core::windows_path_semantics ? "D:\\Photos" : "/photos");
	const auto holiday = parent.combine("Holidays 2019");
	(*folders)[holiday] = 232;

	for (char suffix = 'A'; suffix <= 'L'; ++suffix)
	{
		(*folders)[parent.combine("2019").combine(std::string_view(&suffix, 1))] = 240;
	}

	const auto completes = make_test_destination_completer(s, lifetime, folders, [holiday](const core::folder_path)
	{ return core::folder_paths{holiday}; });

	ui::auto_complete_results results;
	completes->search(std::format("{} 2019", parent.text()),
	                  [&results](const ui::auto_complete_results& r) { results = r; });
	run_destination_completion_worker(as);
	as.drain_ui();

	assert_equal(true, results.size() <= completes->max_predictions, "destination completions remain capped");
	const auto holiday_text = std::string(holiday.text()); // APP-003: destination completions are raw paths
	assert_equal(true,
	             std::ranges::any_of(results, [holiday_text](const ui::auto_complete_match_ptr& r)
	{ return core::icmp(r->edit_text(), holiday_text) == 0; }),
	             "a disk child that is also known combines both weights before the cap");
}

static void should_skip_stale_destination_completion_work()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	auto lifetime = std::make_shared<int>(1);
	auto folders = std::make_shared<core::folder_counts>();
	auto enumerations = 0;
	const auto completes = make_test_destination_completer(s, lifetime, folders,
	                                                       [&enumerations](const core::folder_path)
	{
		++enumerations;
		return core::folder_paths{};
	});

	auto stale_delivered = 0;
	auto empty_delivered = 0;
	completes->search("D:\\Server", [&stale_delivered](const ui::auto_complete_results&) { ++stale_delivered; });
	completes->search({}, [&empty_delivered](const ui::auto_complete_results&) { ++empty_delivered; });

	run_destination_completion_worker(as);
	as.drain_ui();

	assert_equal(0, enumerations, "a stale path completion skips folder enumeration");
	assert_equal(0, stale_delivered, "a stale path completion does not publish");
	assert_equal(1, empty_delivered, "the current empty completion is retained");
}

static void should_release_stale_destination_completion_callbacks_on_the_ui_thread()
{
	struct release_probe
	{
		bool& released_on_ui;
		~release_probe() { released_on_ui = platform::is_ui_thread(); }
	};

	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	auto lifetime = std::make_shared<int>(1);
	auto folders = std::make_shared<core::folder_counts>();
	const auto completes = make_test_destination_completer(s, lifetime, folders, [](const core::folder_path)
	{ return core::folder_paths{}; });

	bool released_on_ui = false;
	std::weak_ptr<release_probe> weak_probe;
	{
		auto probe = std::make_shared<release_probe>(released_on_ui);
		weak_probe = probe;
		completes->search("D:\\Server", [probe](const ui::auto_complete_results&) {});
	}
	completes->search({}, [](const ui::auto_complete_results&) {});

	run_destination_completion_worker(as);
	assert_equal(false, weak_probe.expired(), "the stale callback is handed back to the UI queue");
	as.drain_ui();

	assert_equal(true, weak_probe.expired(), "the stale callback is released");
	assert_equal(true, released_on_ui, "the stale callback capture is released on the UI thread");
}

// What command_status now holds its dialog and controls through. The worker that runs an operation
// can drop the last reference to its status, and a dialog destroyed there destroys windows and moves
// focus from the wrong thread, so a release off the UI thread is handed back to the UI queue.
static void should_hand_the_last_ui_owned_reference_back_to_the_ui_thread()
{
	struct release_probe
	{
		bool& released;
		bool& released_on_ui;
		~release_probe()
		{
			released = true;
			released_on_ui = platform::is_ui_thread();
		}
	};

	deferred_async_strategy as;
	bool released = false;
	bool released_on_ui = false;
	auto owned = ui_owned(as, std::make_shared<release_probe>(released, released_on_ui));

	// Moved in and dropped there, so the worker holds the only reference when it lets go.
	std::thread worker([held = std::move(owned)]() mutable { const auto last = std::move(held); });
	worker.join();

	assert_equal(false, released, "the worker did not run the destructor");
	assert_equal(1_z, as.pending_ui_count(), "the last reference was handed to the UI queue");

	as.drain_ui();

	assert_equal(true, released, "the reference is released once the UI queue runs");
	assert_equal(true, released_on_ui, "and the destructor runs on the UI thread");
}

static void should_keep_sidebar_rebuild_packets_current()
{
	null_state_strategy ss;
	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	const auto restore = core::scope_exit([]
	{
		setting.search = {};
		setting.sidebar = {};
		setting.favorite_tags.clear();
	});

	search_item_factory::sidebar_config_snapshot config;
	config.search.title[0] = "Snapshot search";
	config.search.path[0] = "tag:snapshot";
	config.sidebar.show_favorite_searches = true;
	config.favorite_tags = "snapshot";

	setting.search.title[0] = "Live search";
	setting.search.path[0] = "tag:live";
	setting.favorite_tags = "live";

	constexpr search_item_factory factory;
	const auto items = factory.create_search_items(s, {}, config);

	assert_equal(true,
	             std::ranges::any_of(items, [](const search_item_ptr& item) { return item->key == "s:tag:snapshot"; }),
	             "sidebar workers use the captured search settings");
	assert_equal(false,
	             std::ranges::any_of(items, [](const search_item_ptr& item) { return item->key == "s:tag:live"; }),
	             "sidebar workers do not read replaced live search settings");

	assert_equal(false, should_publish_sidebar_structural_request(2, 1),
	             "an obsolete sidebar structural packet cannot publish");
	assert_equal(true, should_publish_sidebar_structural_request(2, 2),
	             "the current sidebar structural packet can publish");
}

// Every sidebar row is counted by the search index, so its total is what clicking it finds, down to the
// sizes and the picture chosen to show it. A tag or label row that would find nothing is not shown;
// rating rows always are, because together they are one scale.
static void should_count_sidebar_rows_from_the_search_index(shared_test_context& stc)
{
	stc.lazy_load_index();
	stc.test_index.update_summary();
	// As every summary pass does, so the snapshot holds what earlier tests changed in the shared index.
	stc.test_index.update_search_index();

	null_state_strategy ss;
	null_async_strategy as;
	view_state s(ss, as, stc.test_index, make_test_player());

	search_item_factory::sidebar_config_snapshot config;
	config.favorite_tags = "key1 zz_tag_on_nothing";

	constexpr search_item_factory factory;
	search_counter counter(stc.test_index);
	assert_equal(true, counter.is_indexed(), "the sidebar counts from the search index");

	const auto tags = search_item_factory::count_rows(s, counter, factory.create_tags(s, {}, config), true);
	const auto labels = search_item_factory::count_rows(s, counter, factory.create_labels(s, {}), true);
	const auto ratings = search_item_factory::count_rows(s, counter, factory.create_ratings(s, {}), false);

	const auto has_row = [](const std::vector<search_item_ptr>& rows, const std::string_view key)
	{
		return std::ranges::any_of(rows, [key](const search_item_ptr& row) { return row->key == key; });
	};

	assert_equal(true, has_row(tags, "t:key1"), "a tag that finds items is shown");
	assert_equal(false, has_row(tags, "t:zz_tag_on_nothing"), "a favorite tag that finds nothing is not shown");
	assert_equal(6_z, ratings.size(), "every rating row is shown, empty or not");

	for (const auto* const rows : {&tags, &labels, &ratings})
	{
		for (const auto& row : *rows)
		{
			assert_equal(true, row->summary_known, std::format("'{}' arrives counted", row->key));

			const auto expected = stc.test_index.count_matches(row->search, test_token,
			                                                   index_state::search_execution::scan);
			assert_equal(expected.total_items().count, row->summary.total_items().count,
			             std::format("'{}' counts what clicking it finds", row->key));
			assert_equal(true, row->summary == expected, std::format("'{}' counts sizes and picture alike", row->key));
		}
	}

	for (const auto& row : labels)
	{
		assert_equal(true, row->summary.total_items().count > 0, std::format("'{}' finds items", row->key));
	}
}

static void should_consume_cancelled_globe_release()
{
	assert_equal(true, should_invoke_globe_release(true, false, true), "a normal click invokes");
	assert_equal(false, should_invoke_globe_release(false, false, true),
	             "a release whose press Escape let go of, or that never had one, is consumed");
	assert_equal(false, should_invoke_globe_release(true, true, true), "an uncancelled drag is not a click");
	assert_equal(false, should_invoke_globe_release(true, false, false), "release outside the globe does not invoke");
}

static void should_offer_only_native_movie_project_dialogs()
{
	const auto filters = movie_project_filters();
	assert_equal(1_z, filters.size(), "Open and Save offer one project format");
	assert_equal("*.dfv"s, filters.front().extensions, "the plain text project Movie writes");
	assert_equal(std::string(tt.movie_project_files.sv()), filters.front().text, "the filter is localized");
}

static void should_skip_existing_movie_batch_outputs()
{
	for (const auto separate_output : {false, true})
	{
		const auto root = test_temps.next_folder("movie-batch-skip");
		const auto project = root.combine_file("project.dfv");
		const auto output_folder = separate_output ? root.combine("output") : root;
		platform::create_folder(output_folder);
		const auto output = output_folder.combine_file("project.mp4");
		write_test_file(project, "c - missing.mp4\n");
		write_test_file(output, "existing output");

		movie_batch_options options;
		options.mode = movie_batch_mode::render;
		if (separate_output) options.output = output_folder.text();
		assert_equal(true, movie_batch_test_seams::project_is_skipped(project, options),
		             "existing output is skipped before reading the project or its missing media");
		assert_equal("existing output"s, read_test_file(output), "skipping preserves existing bytes");

		options.overwrite = true;
		assert_equal(false, movie_batch_test_seams::project_is_skipped(project, options),
		             "explicit overwrite still validates the project and media");
		options.overwrite = false;
		options.mode = movie_batch_mode::check;
		assert_equal(false, movie_batch_test_seams::project_is_skipped(project, options),
		             "check validates instead of skipping an existing output");
		assert_equal("existing output"s, read_test_file(output), "failed validation writes nothing");
		assert_equal(false, write_movie_project_file(output, "unapproved replacement", true).empty(),
		             "staged publication also refuses an output that appears after the skip check");
		assert_equal("existing output"s, read_test_file(output), "publication preserves the competing file");
	}
}

// A project an agent writes is made only as it was written: a line the reader cannot use stops the
// project rather than rendering a movie without it. Text screens need no source at all.
static void should_render_a_movie_project_only_as_written()
{
	const auto root = test_temps.next_folder("movie-batch-lines");

	const auto screens = root.combine_file("screens.dfv");
	write_test_file(screens, "# Diffractor video\nt 3 'Hiking to\\nEverest'\n\nt 2 'The End'\n");
	assert_equal(true, movie_batch_test_seams::project_is_ready(screens), "a movie of text screens alone is ready");

	const auto mistaken = root.combine_file("mistaken.dfv");
	write_test_file(mistaken, "t 3 'Hiking to Everest'\nx 2 'The End'\n");
	assert_equal(false, movie_batch_test_seams::project_is_ready(mistaken),
	             "a line the reader cannot use stops the project");

	// skip_missing leaves out the clips it cannot use, but a project whose every clip is gone is not its text
	// screens alone.
	const auto partly = root.combine_file("partly.dfv");
	write_test_file(partly, std::format("t 3 'Trip'\nc -2 \"{}\"\nc - missing.mp4\n",
	                                    test_files_folder().combine_file("Small.jpg")));
	assert_equal(false, movie_batch_test_seams::project_is_ready(partly), "a missing clip stops the project");
	assert_equal(true, movie_batch_test_seams::project_is_ready(partly, true), "unless it is left out");

	const auto gone = root.combine_file("gone.dfv");
	write_test_file(gone, "t 3 'Trip'\nc - missing.mp4\n");
	assert_equal(false, movie_batch_test_seams::project_is_ready(gone, true),
	             "and a project whose every clip is missing is not rendered as its titles");

	// A sound file is part of the movie its writer meant, as a source is.
	const auto photo = test_files_folder().combine_file("Small.jpg");
	const auto unheard = root.combine_file("unheard.dfv");
	write_test_file(unheard, std::format("c -2 \"{}\"\nc -2 \"{}\" !missing.wav\n", photo, photo));
	assert_equal(false, movie_batch_test_seams::project_is_ready(unheard), "a missing sound file stops the project");
	assert_equal(true, movie_batch_test_seams::project_is_ready(unheard, true), "unless its clip is left out");

	const auto heard = root.combine_file("heard.dfv");
	write_test_file(heard,
	                std::format("c -2 \"{}\" !\"{}\"\n", photo, test_files_folder().combine_file("Colorblind.mp3")));
	assert_equal(true, movie_batch_test_seams::project_is_ready(heard), "while one that plays is ready");
}

// movie_check and movie_render answer with a report a client reads as well as lines a person does: each project's
// outcome, what it holds, and each line the reader could not use.
static void should_report_movie_projects_as_json()
{
	const auto root = test_temps.next_folder("movie-batch-report");
	write_test_file(root.combine_file("a screens.dfv"), "t 3 'Hiking'\nt 2 'The End'\n");
	write_test_file(root.combine_file("b mistaken.dfv"), "t 3 'Hiking'\nx 2 'The End'\n");

	movie_batch_options options;
	options.projects = root.text();
	std::vector<std::string> printed;
	const auto report = run_movie_batch(options,
	                                    [&printed](const std::string_view line) { printed.emplace_back(line); });
	assert_equal(false, report.succeeded, "a project that is not ready fails the check");
	assert_equal(false, printed.empty(), "what it finds is printed as it goes");

	rapidjson::Document json;
	json.Parse(format_movie_batch_report(report).c_str());
	assert_equal(true, json.IsObject() && json.HasMember("projects") && json["projects"].IsArray(), "a JSON object");
	const auto& projects = json["projects"];
	assert_equal(2_z, static_cast<size_t>(projects.Size()), "an entry a project");
	assert_equal("ready"s, std::string(projects[0]["outcome"].GetString()), "text screens alone are ready");
	assert_equal(2_z, static_cast<size_t>(projects[0]["text_screens"].GetUint64()), "and are counted");
	assert_equal("failed"s, std::string(projects[1]["outcome"].GetString()), "a line it cannot use fails the project");
	assert_equal(1_z, static_cast<size_t>(projects[1]["unreadable_lines"].Size()), "which names the line");
	assert_equal(2, projects[1]["unreadable_lines"][0]["line"].GetInt(), "by its number");
}

// -tool:<name> takes name=value pairs, each read as the tool's schema types it, so a call from the command line
// reaches the tool with the arguments a client would send.
static void should_type_a_tool_call_s_arguments()
{
	constexpr auto schema =
		R"({"type":"object","properties":{"path":{"type":"string"},"count":{"type":"integer"},"scale":{"type":"number"},"overwrite":{"type":"boolean"},"names":{"type":"array","items":{"type":"string"}},"checks":{"type":"object"}}})"sv;

	std::string error;
	const auto arguments = tool_arguments_json(schema,
	                                           {"path=C:\\a b\\c=d.dfv", "count=3", "scale=0.5", "overwrite=yes",
	                                            "names=a.jpg,b.jpg", R"(checks={"Keep":true})", "other=7"},
	                                           error);
	assert_equal(std::string{}, error, "every pair reads");

	rapidjson::Document json;
	json.Parse(arguments.c_str());
	assert_equal(true, json.IsObject(), "the arguments are an object");
	assert_equal("C:\\a b\\c=d.dfv"s, std::string(json["path"].GetString()), "the first = ends the name");
	assert_equal(3, json["count"].GetInt(), "an integer");
	assert_equal(0.5, json["scale"].GetDouble(), "a number");
	assert_equal(true, json["overwrite"].GetBool(), "yes is true");
	assert_equal(2_z, static_cast<size_t>(json["names"].Size()), "a list is split at its commas");
	assert_equal(true, json["checks"]["Keep"].GetBool(), "an object is given as JSON");
	assert_equal("7"s, std::string(json["other"].GetString()), "a name the schema has not is text, which it refuses");

	error.clear();
	tool_arguments_json(schema, {"count=three"}, error);
	assert_equal(false, error.empty(), "a number that is not one is refused");
	error.clear();
	tool_arguments_json(schema, {"overwrite"}, error);
	assert_equal(false, error.empty(), "and a pair without its =");
}

// An agent's Save in the Edit view was answered as soon as the window caught up: the dialog saying the save was under
// way read as a question to answer, and wait_until_idle as idle, while the file was still being written. A dialog
// showing work is waited out, and says what it waits on; one that asks is answered.
static void should_wait_out_a_dialog_that_shows_work_under_way()
{
	struct shown_frame final : ui::null_frame
	{
		bool is_visible() const override { return true; }
		bool is_canceled() const override { return false; }
	};

	const auto work_before = dialogs_showing_work().size();
	const auto asked_before = dialog_awaits_answer();

	{
		const auto saving = make_dlg(ui::no_frame());
		saving->_frame = std::make_shared<shown_frame>();
		saving->show_status(icon_index::save, "Saving photo.avif..."sv);

		const auto work = dialogs_showing_work();
		assert_equal(work_before + 1, work.size(), "a status shows work under way");
		assert_equal("Saving photo.avif..."s, work.back(), "and says what it is");
		assert_equal(asked_before, dialog_awaits_answer(), "with nothing to answer");

		const auto asking = make_dlg(ui::no_frame());
		asking->_frame = std::make_shared<shown_frame>();
		asking->show_controls({std::make_shared<ui::close_control>(asking->_frame)}, {44}, {44});
		asking->list_as_open();
		assert_equal(true, dialog_awaits_answer(), "a dialog that asks is answered");
		assert_equal(work_before + 1, dialogs_showing_work().size(), "and is no work to wait out");
	}

	assert_equal(work_before, dialogs_showing_work().size(), "the work is not waited on once its dialog has gone");
	assert_equal(asked_before, dialog_awaits_answer(), "nor the question");
}

// What -mcp-ui and -mcp-allow ask of a session. What is not understood is reported, not guessed at, and nothing
// beyond reading is allowed unless asked for.
static void should_read_an_agent_session_s_options()
{
	const auto options = parse_agent_options("visible", "write, capture,dev");
	assert_equal(true, options.presence == agent_presence::visible, "the window on the desktop");
	assert_equal(true, options.allow.write && options.allow.capture && options.allow.dev, "each tier asked for");
	assert_equal(false, options.allow.network || options.allow.launch, "and no other");
	assert_equal(true, options.problems.empty(), "spaces round a tier are allowed");
	assert_equal(true, parse_agent_options({}, "launch").allow.launch, "opening other programs is a tier of its own");

	const auto plain = parse_agent_options({}, {});
	assert_equal(true, plain.presence == agent_presence::none, "no window until a tool needs one");
	assert_equal(false,
	             plain.allow.write || plain.allow.capture || plain.allow.network || plain.allow.dev ||
	                 plain.allow.launch,
	             "reading alone");

	const auto wrong = parse_agent_options("hidden", "write,everything");
	assert_equal(2_z, wrong.problems.size(), "what is not understood is reported");
	assert_equal(true, wrong.presence == agent_presence::none && wrong.allow.write, "and what is understood is kept");
}

// What an agent session writes to the settings lasts as long as it does: it reads what is kept, and the user's next
// start finds what was there before.
static void should_keep_an_agent_session_s_settings_to_itself()
{
	const auto folder = test_temps.next_folder("session-settings");
	assert_equal(true, platform::create_ini_file_settings(folder)->write("view", "zoom", static_cast<uint32_t>(3)),
	             "a setting kept before the session");

	const auto session = make_session_settings(platform::create_ini_file_settings(folder));
	uint32_t zoom = 0;
	assert_equal(true, session->read("view", "zoom", zoom) && zoom == 3, "the session reads what was kept");
	assert_equal(true, session->write("view", "zoom", static_cast<uint32_t>(5)), "and writes its own");
	assert_equal(true, session->write("view", "theme", "dark"sv), "of any kind");
	assert_equal(true, session->read("view", "zoom", zoom) && zoom == 5, "which it reads back");

	const auto reopened = platform::create_ini_file_settings(folder);
	assert_equal(true, reopened->read("view", "zoom", zoom) && zoom == 3, "while the store keeps its own");
	std::string theme;
	assert_equal(false, reopened->read("view", "theme", theme), "and nothing the session added");
}

// An agent and a tutorial script call a command by its name: every command the window defines has one, which calls
// it back, and no two share one.
static void should_name_every_command()
{
	const auto app = std::make_shared<app_frame>();
	app->initialise_commands();

	std::vector<std::string> unnamed;

	for (const auto& [id, command] : app->_commands)
	{
		const auto name = command_name(id);
		if (name.empty()) unnamed.emplace_back(command->text);
		else assert_equal(true, command_from_name(name) == id, "a name calls its own command");
	}

	std::ranges::sort(unnamed);
	assert_equal(std::string{}, core::combine(unnamed, ", "), "commands without a name");

	std::set<std::string_view> names;
	for (const auto id : all_commands()) assert_equal(true, names.insert(command_name(id)).second, "names are unique");
	assert_equal(false, command_from_name("no_such_command").has_value(), "a name no command has calls nothing");
}

// A tone loud enough that nothing would call it quiet, for `seconds`: stereo 16-bit at 48 kHz.
static core::file_path write_tone_wave(const double seconds)
{
	constexpr auto rate = 48000;
	const auto frames = static_cast<uint32_t>(seconds * rate);
	const auto data_bytes = frames * 4;
	std::string wave;
	const auto append = [&wave](const uint32_t value, const int bytes)
	{
		for (auto i = 0; i < bytes; ++i) wave.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
	};

	wave += "RIFF";
	append(36 + data_bytes, 4);
	wave += "WAVEfmt ";
	append(16, 4);
	append(1, 2);
	append(2, 2);
	append(rate, 4);
	append(rate * 4, 4);
	append(4, 2);
	append(16, 2);
	wave += "data";
	append(data_bytes, 4);

	for (uint32_t i = 0; i < frames; ++i)
	{
		const auto sample = static_cast<int16_t>(std::lround(10000.0 * std::sin(2.0 * M_PI * 440.0 * i / rate)));
		append(static_cast<uint16_t>(sample), 2);
		append(static_cast<uint16_t>(sample), 2);
	}

	const auto path = test_temps.next_path(".wav");
	write_test_file(path, wave);
	return path;
}

// A photo has no sound of its own; given a sound file, the movie carries that sound while the photo shows. The
// render opens the sound apart from the picture, so asking for the sound first still leaves the photo to draw.
static void should_render_a_clips_sound_file()
{
	movie_clip clip;
	clip.path = test_files_folder().combine_file("Small.jpg");
	clip.is_photo = true;
	clip.is_missing = false;
	clip.is_probed = true;
	clip.end = 3;
	clip.extent = {64, 64};
	clip.sound_file = write_tone_wave(2);
	clip = apply_movie_sound_probe(clip, probe_movie_sound(clip.sound_file));
	assert_equal(true, clip.sound_file_is_probed && !clip.sound_file_is_missing, "the sound file is found");
	assert_near(2.0, clip.sound_file_duration, 0.05, "and measured");

	// A screen before it, so a crossfade brings the photo in and hears its sound before drawing its picture.
	movie_render_request request;
	request.clips = {make_movie_text("Title", 2), clip};
	request.output = derive_movie_output(request.clips);
	request.path = test_temps.next_path(".mp4");
	const auto control = std::make_shared<movie_render_control>();
	const auto result = render_movie_to_file(request, [](double) {}, test_token, control);

	if (result.reason == movie_render_error::no_encoder) return;

	assert_equal(true, result.ok, "a photo with a sound file renders");

	const auto written = probe_movie_sound(request.path);
	assert_equal(true, written.found, "and the movie has a sound track");

	av_format_decoder decoder;
	assert_equal(true, decoder.open(request.path, media_intent::playback), "the movie opens");
	decoder.init_streams(-1, -1, false, false, false);
	// The photo comes in at one second and its tone plays for two, at full level once the crossfade is done.
	const auto pcm = decoder.extract_audio_pcm_range(48000, 2.0, 0.8);
	assert_equal(false, movie_sound_is_quiet(pcm), "carrying the sound file's tone while the photo shows");
}

// Each item of a timeline made a YouTube Short of its own, into one folder: named for what it plays, never over a
// file already there or one the run made before it, and a text screen passed over, being a page between clips.
static void should_render_each_movie_item_as_a_short()
{
	movie_clip clip;
	clip.path = test_files_folder().combine_file("Small.jpg");
	clip.is_photo = true;
	clip.is_missing = false;
	clip.is_probed = true;
	clip.end = 0.2;
	clip.extent = {64, 64};

	const auto folder = test_temps.next_folder("shorts");
	write_test_file(folder.combine_file("Small.mp4"), "a file already there");

	const auto made = render_movie_shorts({make_movie_text("Between", 1), clip, clip}, folder,
	                                      [](size_t, size_t, double) {}, test_token);

	// With no encoder - the Linux build has none - the run stops at its first Short, naming the one it could not make.
	if (made.last.reason == movie_render_error::no_encoder)
	{
		assert_equal(true, made.made.empty() && made.failed_name == "Small (2).mp4",
		             "nothing is made, and the Short it stopped at is named after the file already there");
		return;
	}

	assert_equal(true, made.last.ok && made.made.size() == 2, "each clip is made a Short of its own");
	assert_equal(true,
	             made.made.size() == 2 && made.made[0] == folder.combine_file("Small (2).mp4") &&
	                 made.made[1] == folder.combine_file("Small (3).mp4"),
	             "named for what it plays, after the file already there and the one before it");
	assert_equal("a file already there"s, read_test_file(folder.combine_file("Small.mp4")), "which is left alone");

	for (const auto& path : made.made)
	{
		av_format_decoder decoder;
		assert_equal(true, decoder.open(path, media_intent::metadata), "a Short opens");
		decoder.init_streams(-1, -1, false, true, false);
		assert_equal(true, decoder.info().display_dimensions == movie_short_extent, "standing upright");
	}

	std::atomic_bool stop = true;
	const auto stopped = render_movie_shorts({clip}, test_temps.next_folder("stopped"), [](size_t, size_t, double) {},
	                                         core::cancel_token(stop));
	assert_equal(true, stopped.last.cancelled && stopped.made.empty(), "a run asked to stop makes nothing more");
}

static void should_refuse_a_movie_output_claimed_during_render()
{
	movie_clip clip;
	clip.path = test_files_folder().combine_file("Small.jpg");
	clip.is_photo = true;
	clip.is_missing = false;
	clip.is_probed = true;
	clip.end = 0.1;
	clip.extent = {64, 64};
	movie_render_request request;
	request.clips = {clip};
	request.output = derive_movie_output(request.clips);
	request.path = test_temps.next_path(".mp4");
	request.fail_if_exists = true;
	const auto control = std::make_shared<movie_render_control>();
	auto claimed = false;
	const auto result = render_movie_to_file(request, [&](double)
	{
		if (!claimed)
		{
			claimed = true;
			write_test_file(request.path, "competing movie");
		}
	}, test_token, control);

	assert_equal(false, result.ok, "render never replaces a destination without permission");
	if (claimed)
		assert_equal("competing movie"s, read_test_file(request.path), "the late destination survives publication");
	else
		assert_equal(true, result.reason == movie_render_error::no_encoder,
		             "only an unavailable platform writer can prevent the publication race from running");
}

// The platform's chrome - a menu's check mark, check boxes, radio buttons and the caption buttons -
// draws the app's own glyphs for them, which only the app's icon table names.
static void should_draw_the_chrome_with_the_app_glyphs()
{
	assert_equal(true, ui::chrome_icons().check == icon_index::check, "a menu's check mark");
	assert_equal(true, ui::chrome_icons().checkbox_off == icon_index::checkbox_off, "a clear check box");
	assert_equal(true, ui::chrome_icons().checkbox_on == icon_index::checkbox_on, "a checked one");
	assert_equal(true, ui::chrome_icons().radio_off == icon_index::radio_off, "a clear radio button");
	assert_equal(true, ui::chrome_icons().radio_on == icon_index::radio_on, "a chosen one");
	assert_equal(true, ui::chrome_icons().close == icon_index::close, "the caption's close button");
	assert_equal(true, ui::chrome_icons().maximize == icon_index::maximize, "its maximize button");
	assert_equal(true, ui::chrome_icons().minimize == icon_index::minimize, "its minimize button");
	assert_equal(true, ui::chrome_icons().restore == icon_index::restore, "and its restore button");
}

// The app's assets are linked into the executable, the same bytes on every platform: the index's schema
// and the world map the sidebar draws.
static void should_embed_the_app_assets()
{
	const auto sql = load_app_resource(app_resource::sql);
	const std::string_view schema(reinterpret_cast<const char*>(sql.data()), sql.size());
	assert_equal(true, schema.find("CREATE TABLE") != std::string_view::npos, "the index's schema");

	const auto map = load_app_resource(app_resource::map_png);
	constexpr std::array<uint8_t, 8> png_signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	assert_equal(true,
	             map.size() > png_signature.size() &&
	                 std::equal(png_signature.begin(), png_signature.end(), map.data()),
	             "and the world map");
}

// Guards the drawn mark against silent drift. The same artwork is drawn independently by
// tools/generate_store_assets.py for app.ico and the Store assets.
static void should_draw_the_logo()
{
	for (const auto size : {16, 32, 44, 150, 256})
	{
		const auto s = std::make_shared<core::surface>();
		assert_equal(true, s->alloc(size, size, core::texture_format::ARGB), "logo surface allocated");
		fill_app_logo(*s);

		const auto last = size - 1;
		assert_equal(0u, s->get_pixel(0, 0), "logo corner is transparent");
		assert_equal(0u, s->get_pixel(last, last), "logo opposite corner is transparent");

		// The four squares sit on the vertical and horizontal axes through the centre.
		const auto mid = size / 2;
		const auto near_edge = std::max(1, size / 8);
		const auto top = s->get_pixel(mid, near_edge);
		const auto bottom = s->get_pixel(mid, last - near_edge);
		const auto left = s->get_pixel(near_edge, mid);
		const auto right = s->get_pixel(last - near_edge, mid);

		// Surface pixels are stored blue first, so core::get_r reads the blue channel here.
		const auto red_of = [](const core::color32 c)
		{
			return core::get_b(c);
		};
		const auto green_of = [](const core::color32 c)
		{
			return core::get_g(c);
		};
		const auto blue_of = [](const core::color32 c)
		{
			return core::get_r(c);
		};

		assert_equal(true, green_of(top) > red_of(top) && green_of(top) > blue_of(top), "logo top is green");
		assert_equal(true, red_of(bottom) > green_of(bottom) && red_of(bottom) > blue_of(bottom), "logo bottom is red");
		assert_equal(true, red_of(left) > 0x80 && green_of(left) > 0x60 && blue_of(left) < 0x40, "logo left is yellow");
		assert_equal(true, blue_of(right) > red_of(right) && blue_of(right) > green_of(right), "logo right is blue");

		for (const auto c : {top, bottom, left, right})
		{
			assert_equal(255u, core::get_a(c), "logo square centres are opaque");
		}
	}
}

static void should_read_a_tutorial_script()
{
	const auto script = parse_tutorial_script("# a comment\n"
	                                          "open \"C:\\photos\"\n"
	                                          "say welcome 'The address box {address} opens folders.'\n"
	                                          "  point address\n"
	                                          "cue address\n"
	                                          "point month 2006-07 1.5\n"
	                                          "click ctrl\n"
	                                          "point tile 'it''s.jpg'\n"
	                                          "point tile 'b.jpg'\n"
	                                          "point tile 'v.mp4' 0.2 0.5 2\n"
	                                          "key ctrl+shift+f11\n"
	                                          "type 'sunset'\n"
	                                          "command rate-5\n"
	                                          "point at 0.25 0.75\n"
	                                          "tooltips on\n"
	                                          "say end 'Goodbye.'\n");

	assert_equal(size_t{1}, script.problems.size(), "a quote run into what follows is a problem");
	assert_equal(true, script.problems.front().starts_with("line 8:"), "and names its line");
	assert_equal(size_t{1}, script.scene.size(), "a step before the first say sets the scene");
	assert_equal("C:\\photos"sv, script.scene.front().text, "a double-quoted path keeps its backslash");
	assert_equal(size_t{2}, script.beats.size(), "each say starts a beat");

	const auto& steps = script.beats.front().steps;
	assert_equal(size_t{11}, steps.size(), "the beat's steps, the unreadable line left out");
	assert_equal(true, steps[0].anchor.kind == tutorial_anchor_kind::address, "an anchor");
	assert_equal("address"sv, steps[1].text, "a cue its narration has");
	assert_equal(true, steps[2].anchor.year == 2006 && steps[2].anchor.month == 7, "a month of the calendar");
	assert_equal(1.5, steps[2].seconds, "a glide of its own");
	assert_equal(true, steps[3].keys.control && !steps[3].keys.shift, "a click with ctrl held");
	assert_equal("b.jpg"sv, steps[4].anchor.name, "a tile");
	assert_equal(true, steps[4].anchor.x == 0.5 && steps[4].anchor.y == 0.5, "at its middle");
	assert_equal(true, steps[5].anchor.x == 0.2 && steps[5].anchor.y == 0.5 && steps[5].seconds == 2.0,
	             "or across it, with a glide after");
	assert_equal(true, steps[6].key == platform::keys::F11 && steps[6].keys.control && steps[6].keys.shift,
	             "a key with its modifiers");
	assert_equal(true, steps[8].command == commands::rate_5, "a command by name");
	assert_equal(true, steps[9].anchor.x == 0.25 && steps[9].anchor.y == 0.75, "a point of the window");
	assert_equal(true, steps[10].on, "tooltips on");

	const auto broken = parse_tutorial_script("cue early\n"
	                                          "say a 'Hello {there}.'\n"
	                                          "cue elsewhere\n"
	                                          "key hyper+x\n"
	                                          "command teleport\n"
	                                          "wait soon\n"
	                                          "say a 'Again.'\n"
	                                          "dance\n");
	assert_equal(size_t{7}, broken.problems.size(), "each line that cannot be read is named");

	assert_equal("The address box opens folders."s,
	             tutorial_spoken_text("The address box {address} opens folders {x}."),
	             "cues are not spoken, and the text closes up where they were");

	const auto guessed = estimate_tutorial_narration("Half of this {half}is here.");
	assert_equal(false, guessed.measured, "a guess before there is a voice");
	assert_equal(true,
	             guessed.cue("half").value_or(0.0) > 0.4 * (guessed.seconds - 0.4) &&
	                 guessed.cue("half").value_or(0.0) < 0.6 * (guessed.seconds - 0.4),
	             "a cue falls as far into the narration as it is written");

	const auto timing = parse_tutorial_timing("length\t12.5\r\ncue\taddress\t3.25\n");
	assert_equal(true, timing.has_value() && timing->measured, "a voice's timing is measured");
	assert_equal(12.5, timing ? timing->seconds : 0.0, "its length");
	assert_equal(3.25, timing ? timing->cue("address").value_or(0.0) : 0.0, "and where its cues fall");
	assert_equal(false, parse_tutorial_timing("cue\taddress\t3.25\n").has_value(), "a timing gives a length");
}

// The steps that work the window as a hand does: a right-click, a drag to an anchor, the wheel turned, and text typed
// into whatever has the keyboard.
static void should_read_a_script_s_pointer_and_keyboard_steps()
{
	const auto script = parse_tutorial_script("right-click shift\n"
	                                          "drag at 0.5 0.25 1.5\n"
	                                          "drag tile 'a.jpg'\n"
	                                          "wheel down 3\n"
	                                          "wheel up\n"
	                                          "text 'two\\nlines'\n"
	                                          "key insert\n");

	assert_equal(size_t{0}, script.problems.size(), "each is a step");
	const auto& steps = script.scene;
	assert_equal(size_t{7}, steps.size(), "of the scene");
	assert_equal(true, steps[0].kind == tutorial_step_kind::right_click && steps[0].keys.shift,
	             "a right-click with shift held");
	assert_equal(true,
	             steps[1].kind == tutorial_step_kind::drag && steps[1].anchor.kind == tutorial_anchor_kind::at &&
	                 steps[1].anchor.y == 0.25 && steps[1].seconds == 1.5,
	             "a drag to a point of the window, with a glide of its own");
	assert_equal(true, steps[2].anchor.kind == tutorial_anchor_kind::tile && steps[2].anchor.name == "a.jpg",
	             "or to a tile");
	assert_equal(-3, steps[3].notches, "the wheel turned down three notches");
	assert_equal(1, steps[4].notches, "or up one");
	assert_equal("two\nlines"sv, steps[5].text, "text, a new line read in it");
	assert_equal(true, steps[6].key == platform::keys::INSERT, "a key by its name");

	const auto broken = parse_tutorial_script("wheel sideways\nwheel up 0\ndrag nowhere\ntext unquoted\n");
	assert_equal(size_t{4}, broken.problems.size(), "each line that cannot be read is named");
}

void register_app_tests(view_state& /*state*/, test_registry& tests)
{
	tests.add("Should bind every advertised command"s, should_bind_every_advertised_command);
	tests.add("Should offer Options as one command"s, should_offer_options_as_one_command);
	tests.add("Should open the context menus with a row of buttons"s,
	          should_open_the_context_menus_with_a_row_of_buttons);
	tests.add("Should offer a menu about what is under the pointer"s,
	          should_offer_a_menu_about_what_is_under_the_pointer);
	tests.add("Should offer the menu of the view under the pointer"s,
	          should_offer_the_menu_of_the_view_under_the_pointer);
	tests.add("Should take Tab round the window"s, should_take_tab_round_the_window);
	tests.add("Should work a part from the keyboard"s, should_work_a_part_from_the_keyboard);
	tests.add("Should leave a focused control its own keys"s, should_leave_a_focused_control_its_own_keys);
	tests.add("Should reach the links among the listing"s, should_reach_the_links_among_the_listing);
	tests.add("Should open a view's menus beside what opened them"s,
	          should_open_a_view_s_menus_beside_what_opened_them);
	tests.add("Should start the items status bar with one toolbar"s,
	          should_start_the_items_status_bar_with_one_toolbar);
	tests.add("Should relabel the faces commands in the status bar"s,
	          should_relabel_the_faces_commands_in_the_status_bar);
	tests.add("Should show one line of Faces status text"s, should_show_one_line_of_faces_status_text);
	tests.add("Should draw the chrome with the app's glyphs"s, should_draw_the_chrome_with_the_app_glyphs);
	tests.add("Should embed the app's assets"s, should_embed_the_app_assets);
	tests.add("Should draw the logo"s, should_draw_the_logo);
	tests.add("Should offer only native Movie project dialogs"s, should_offer_only_native_movie_project_dialogs);
	// Release review: an existing batch output needs neither a readable project nor usable sources.
	tests.add("Should skip existing Movie batch outputs"s, should_skip_existing_movie_batch_outputs);
	tests.add("Should render a Movie project only as written"s, should_render_a_movie_project_only_as_written);
	tests.add("Should report Movie projects as JSON"s, should_report_movie_projects_as_json);
	tests.add("Should type a tool call's arguments"s, should_type_a_tool_call_s_arguments);
	tests.add("Should read an agent session's options"s, should_read_an_agent_session_s_options);
	tests.add("Should wait out a dialog that shows work under way"s,
	          should_wait_out_a_dialog_that_shows_work_under_way);
	tests.add("Should keep an agent session's settings to itself"s, should_keep_an_agent_session_s_settings_to_itself);
	tests.add("Should name every command"s, should_name_every_command);
	tests.add("Should refuse a Movie output claimed during render"s,
	          should_refuse_a_movie_output_claimed_during_render);
	tests.add("Should render a Movie clip's sound file"s, should_render_a_clips_sound_file);
	tests.add("Should render each Movie item as a Short"s, should_render_each_movie_item_as_a_short);
	tests.add("Should read a tutorial script"s, should_read_a_tutorial_script);
	tests.add("Should read a script's pointer and keyboard steps"s, should_read_a_script_s_pointer_and_keyboard_steps);
	tests.add("Should invoke link commands and selection shortcuts"s,
	          should_invoke_link_commands_and_selection_shortcuts);
	tests.add("Should move through Items by the common keys"s, should_move_through_items_by_the_common_keys);
	tests.add("Should route focused view keyboard commands"s, should_route_focused_view_keyboard_commands);
	tests.add("Should give a running Commodore 64 the keyboard"s, should_give_a_running_commodore_64_the_keyboard);
	tests.add("Should honor no-indexing for collection updates"s, should_honor_no_indexing_for_collection_updates);
	tests.add("INI file settings should persist values"s, should_persist_to_ini_file);
	tests.add("Should drop damaged recent entries"s, should_drop_damaged_recent_entries);
#ifndef _WIN32
	// PLAT-010 - Linux INI writes published values before the file was durably replaced.
	tests.add("Should commit Linux INI file settings after durable save"s,
	          should_commit_linux_ini_file_settings_after_durable_save);
	// G21 review - unchanged settings values should not restage and fsync the whole file.
	tests.add("Should skip unchanged Linux INI file settings"s, should_skip_unchanged_linux_ini_file_settings);
	// G21 review - each settings save needs its own stage path.
	tests.add("Should commit Linux INI file settings from owned stage"s,
	          should_commit_linux_ini_file_settings_from_owned_stage);
#endif
	tests.add("Should store an order an older build can read"s, should_store_an_order_an_older_build_can_read);
	tests.add("Should Rename with substitutions"s, should_rename_with_substitutions);
	tests.add("Should rename name token without extension"s, should_rename_name_token_without_extension);
	tests.add("Should reject duplicate rename targets"s, should_reject_duplicate_rename_targets);
	tests.add("Should reject unusable rename targets"s, should_reject_unusable_rename_targets);
	tests.add("Should group Rename sidecar collisions"s, should_group_rename_sidecar_collisions);
	tests.add("Should rename subtitles with their language"s, should_rename_subtitles_with_their_language);
	tests.add("Should rename a shared sidecar with the RAW that reads it"s,
	          should_rename_a_shared_sidecar_with_the_raw_that_reads_it);
	tests.add("Should plan the default rename template"s, should_plan_the_default_rename_template);
	tests.add("Should rename a set onto names it is vacating"s, should_rename_a_set_onto_names_it_is_vacating);
	tests.add("Should cascade skipped rename rows"s, should_cascade_skipped_rename_rows);
	tests.add("Should record rename identity"s, should_record_rename_identity);
	tests.add("Should run a rename onto vacated names"s, should_run_a_rename_onto_vacated_names);
	// Release review: cancellation recovery must choose one basename for every companion.
	tests.add("Should recover canceled Rename sidecars as a group"s,
	          should_recover_canceled_rename_sidecars_as_a_group);
	tests.add("Should refuse a rename whose source changed since review"s,
	          should_refuse_a_rename_whose_source_changed_since_review);
	// Concurrency review #26 - a Close handled while handles were released did not stop the run.
	tests.add("Should not run a rename closed while releasing handles"s,
	          should_not_run_a_rename_closed_while_releasing_handles);
	tests.add("Should conclude a run with what it did and why"s, should_conclude_a_run_with_what_it_did_and_why);
	tests.add("Should keep completed run results on source refresh"s,
	          should_keep_completed_run_results_on_source_refresh);
	tests.add("Should say while a review is still being worked out"s,
	          should_say_while_a_review_is_still_being_worked_out);
	// Adjust Date planned the tag each row shifts but drew only three of its four columns.
	tests.add("Should show every column a batch tool names"s, []
	{
		browsing_fixture fixture;
		const auto tool = std::make_shared<batch_tool_view>(fixture.state, nullptr);
		tool->mode(batch_tool_mode::adjust_date);
		assert_equal(4, tool->column_count(), "Adjust Date shows the source of each date");
		assert_equal(false, tool->col_titles()[3].sv().empty(), "under a title of its own");
		tool->mode(batch_tool_mode::convert);
		assert_equal(3, tool->column_count(), "Convert shows three");
		tool->mode(batch_tool_mode::metadata);
		assert_equal(3, tool->column_count(), "and so does Edit metadata");
	});
	// Rename skipped the list's activation, so a playing video ran on behind it and column widths
	// dragged in one use were still there in the next.
	tests.add("Should start each Rename from columns that fit"s, []
	{
		browsing_fixture fixture;
		const auto rename = std::make_shared<rename_view>(fixture.state, nullptr);
		rename->restore_column_sizes({{120.0, 0.0, 0.0, 0.0}});
		assert_equal(true, rename->is_user_sized(0), "a dragged column is user sized");
		rename->deactivate();
		rename->activate({400, 300});
		assert_equal(false, rename->is_user_sized(0), "the next use starts from columns that fit");
	});
	tests.add("Should not convert over a destination changed since review"s,
	          should_not_convert_over_a_destination_changed_since_review);
	tests.add("Should convert to AVIF with its settings"s, should_convert_to_avif_with_its_settings);
	tests.add("Should write metadata and dates in a batch"s, should_write_metadata_and_dates_in_a_batch);
	tests.add("Should refuse Convert destination claimed during staged publication"s,
	          should_refuse_convert_destination_claimed_during_staged_publication);
	tests.add("Should refuse Convert replacement changed during staged publication"s,
	          should_refuse_convert_replacement_changed_during_staged_publication);
#ifdef _WIN32
	tests.add("Should refuse Convert replacement locked during staged publication"s,
	          should_refuse_convert_replacement_locked_during_staged_publication);
#endif
#ifdef _WIN32
	tests.add("Should restore a replaced sidecar when the rename fails"s,
	          should_restore_a_replaced_sidecar_when_the_rename_fails);
	tests.add("Should report batch Rename sidecar rollback survivor"s,
	          should_report_batch_rename_sidecar_rollback_survivor);
	tests.add("Should report batch Rename case-only recovery failure"s,
	          should_report_batch_rename_case_only_recovery_failure);
	tests.add("Should report singleton Rename case-only rollback failure"s,
	          should_report_singleton_rename_case_only_rollback_failure);
#endif
	tests.add("Should continue singleton Rename sidecar rollbacks after failure"s,
	          should_continue_singleton_rename_sidecar_rollbacks_after_failure);
	tests.add("Should run a case only rename"s, should_run_a_case_only_rename);
	tests.add("Should run a singleton folder case only rename"s, should_run_a_singleton_folder_case_only_rename);
	tests.add("Should format rename"s, should_format_rename);
	tests.add("Should rename episodes with padded numbers"s, should_rename_episodes_with_padded_numbers);
	tests.add("Should plan unique convert outputs"s, should_plan_unique_convert_outputs);
	tests.add("Should limit only converted images over the limit"s, should_limit_only_converted_images_over_the_limit);
	tests.add("Should not plan a convert output over a source"s, should_not_plan_a_convert_output_over_a_source);
	tests.add("Should plan no convert output without a folder"s, should_plan_no_convert_output_without_a_folder);
	tests.add("Should prepare shared copies under their own names"s,
	          should_prepare_shared_copies_under_their_own_names);
	tests.add("Should adjust item dates from snapshot"s, should_adjust_item_dates_from_snapshot);
	tests.add("Should round trip the environment mask"s, should_round_trip_the_environment_mask);
#ifdef _WIN32
	tests.add("Should classify MAPI results"s, should_classify_mapi_results);
#endif
	tests.add("Should detect duplicate import destinations"s, should_detect_duplicate_import_destinations);
	tests.add("Should revalidate import rows"s, should_revalidate_import_rows);
	tests.add("Should revalidate import sidecars"s, should_revalidate_import_sidecars);
	tests.add("Should import a shared sidecar once"s, should_import_a_shared_sidecar_once);
	tests.add("Should take face marks with a picture an import moves"s,
	          should_take_face_marks_with_a_picture_an_import_moves);
	// Release review: shared companions follow every resolved destination and every unfinished source.
	tests.add("Should preserve shared Import sidecars after Auto-rename"s,
	          should_preserve_shared_import_sidecars_after_auto_rename);
	tests.add("Should keep shared Import sidecars for unfinished sources"s,
	          should_keep_shared_import_sidecars_for_unfinished_sources);
	tests.add("Should keep import bookkeeping when a folder fails"s,
	          should_keep_import_bookkeeping_when_a_folder_fails);
	tests.add("Should set imported file created to when it was taken"s,
	          should_set_imported_file_created_to_when_it_was_taken);
	tests.add("Should revalidate replaced import destinations"s, should_revalidate_replaced_import_destinations);
#ifdef _WIN32
	tests.add("Should restore replaced Import sidecars when primary write fails"s,
	          should_restore_replaced_import_sidecars_when_primary_write_fails);
#endif
	tests.add("Should delete free Import sidecar copy when primary write fails"s,
	          should_delete_free_import_sidecar_copy_when_primary_write_fails);
	tests.add("Should keep Import success when rollback cleanup fails"s,
	          should_delete_readonly_import_sidecar_rollback_copy);
	tests.add("Should restore Import destination sidecar after partial overwrite failure"s,
	          should_restore_destination_sidecar_after_partial_overwrite_failure);
	tests.add("Should prepare Import from the selection snapshot"s, should_prepare_import_from_the_selection_snapshot);
	tests.add("Should offer every removable drive with media for Import"s,
	          should_offer_every_removable_drive_with_media_for_import);
	tests.add("Should stage a device's files to Import"s, should_stage_a_devices_files_to_import);
	tests.add("Should clear only abandoned device staging"s, should_clear_only_abandoned_device_staging);
	tests.add("Should reject missing sync folder"s, should_reject_missing_sync_folder);
	tests.add("Should reject overlapping sync folders"s, should_reject_overlapping_sync_folders);
	tests.add("Should reject ambiguous sync roots"s, should_reject_ambiguous_sync_roots);
	tests.add("Should ignore unclaimed remote sync files"s, should_ignore_unclaimed_remote_files);
	tests.add("Should select sync actions"s, should_select_sync_actions);
	tests.add("Should show the latest running work on the taskbar"s,
	          should_show_the_latest_running_work_on_the_taskbar);
	tests.add("Should revalidate sync rows"s, should_revalidate_sync_rows);
	tests.add("Should revalidate sync deletes"s, should_revalidate_sync_deletes);
	tests.add("Should apply Sync full-path exclusions to remote counterparts"s,
	          should_apply_sync_full_path_exclusions_to_remote_counterparts);
	tests.add("Should resolve Sync exclusions across multiple roots"s,
	          should_resolve_sync_exclusions_across_multiple_roots);
	tests.add("Should offer every matching external tool"s, should_offer_every_matching_tool);
	// APP-006 - configured external tools declare a singular {item-path} target.
	tests.add("Should offer configured tools only for one selected file"s,
	          should_offer_configured_tools_only_for_one_selected_file);

	// Issue #175 - sidebar history chart span
	tests.add("Should record history beyond ten years"s, should_record_history_beyond_ten_years);
	// Issue #175 - the chart and the search it runs must agree about which month an item is in
	tests.add("Should answer a timeline month with its own items"s, should_answer_a_timeline_month_with_its_own_items);
	tests.add("Should calculate history span from start year"s, should_calculate_history_span_from_start_year);
	tests.add("Should choose a history range that survives wrong dates"s,
	          should_choose_a_history_range_that_survives_wrong_dates);
	tests.add("Should retest the sidebar globe button when pointed at"s,
	          should_retest_the_sidebar_globe_button_when_pointed_at);
	tests.add("Should show the sidebar Faces button before previews"s,
	          should_show_the_sidebar_faces_button_before_previews);
	tests.add("Should pan year columns in the sidebar calendar"s, should_pan_year_columns_in_the_sidebar_calendar);
	tests.add("Should fill the sidebar calendar with years"s, should_fill_the_sidebar_calendar_with_years);
	tests.add("Should open a calendar month from the keyboard"s, should_open_a_calendar_month_from_the_keyboard);
	tests.add("Should place sidebar and bar bubbles in screen coordinates"s,
	          should_place_sidebar_and_bar_bubbles_in_screen_coordinates);
	tests.add("Should hint the shortcut for the action under the pointer"s,
	          should_hint_the_shortcut_for_the_action_under_the_pointer);
	tests.add("Should wait for a still pointer before showing bubbles"s,
	          should_wait_for_a_still_pointer_before_showing_bubbles);
	tests.add("Should follow the pointer with fast hover bubbles"s, should_follow_pointer_with_fast_hover_bubbles);

	tests.add("Should persist media filter"s, should_persist_media_filter);

	// Issue #228 - the tag view's action field
	tests.add("Should round trip tag actions"s, should_round_trip_tag_actions);

	// Issue #229 - detail display per media type
	tests.add("Should persist detail display per media type"s, should_persist_detail_display_per_media_type);

	// Issue #135 - rating/labeling via NumPad
	tests.add("Should map numpad digits to rating keys"s, should_map_numpad_digits_to_rating_keys);

	// Issue #227 - removed default sidebar tags reappear after restart
	tests.add("Should only seed favorite tags on first run"s, should_only_seed_favorite_tags_on_first_run);

	tests.add("Should start safe only after repeated failures"s, should_start_safe_only_after_repeated_failures);
	tests.add("Should select only the description for Edit description"s,
	          should_select_only_the_description_for_edit_description);
	tests.add("Should stop the thumbnail size at either end unless cycling"s, []
	{
		settings_t s;
		s.set_item_scale_position(settings_t::item_scale_position_max);
		s.step_item_scale(1);
		assert_equal(settings_t::item_scale_position_max, s.item_scale_slider_position(),
		             "a wheel step past the largest size keeps it");

		s.set_item_scale_position(0);
		s.step_item_scale(-1);
		assert_equal(0, s.item_scale_slider_position(), "and past the smallest keeps that");

		s.step_item_scale(1);
		assert_equal(true, s.item_scale_slider_position() > 0, "a step between the ends still moves");

		s.set_item_scale_position(settings_t::item_scale_position_max);
		s.step_item_scale(1, true);
		assert_equal(0, s.item_scale_slider_position(), "the toggle button cycles back to the smallest");
	});
	// Today and Yesterday were the UTC day, a day out for hours every day away from Europe.
	tests.add("Should group a day as Today or Yesterday against the local day"s, []
	{
		const auto today = core::date_t(2026, 11, 2).to_days();
		assert_equal(0, date_group_key(core::date_t(2026, 11, 2, 7, 30), today).order1, "this morning is Today");
		assert_equal(1, date_group_key(core::date_t(2026, 11, 1, 22, 0), today).order1, "last night is Yesterday");

		const auto older = date_group_key(core::date_t(2026, 10, 31, 12, 0), today);
		assert_equal(3000 - 2026, older.order1, "an older day groups by its year");
		assert_equal(100 - 10, older.order2, "and its month");

		assert_equal(0, date_group_key(platform::utc_to_local(core::utc_now()), local_today_days()).order1,
		             "the local time now is Today");
	});
	tests.add("Should encode commands with the save settings"s, should_encode_commands_with_the_save_settings);
	tests.add("Should not count concurrent starts"s, should_not_count_concurrent_starts);
	tests.add("Should hand the renderer its YUV latch"s, should_hand_the_renderer_its_yuv_latch);
	tests.add("Should restore history selection"s, should_restore_history_selection);
	tests.add("Should commit history navigation only after open is accepted"s,
	          should_commit_history_navigation_only_after_open_is_accepted);
	tests.add("Should step back into the view a search was opened from"s,
	          should_step_back_into_the_view_a_search_was_opened_from);
	tests.add("Should open an area found later without asking the view to give way"s,
	          should_open_an_area_found_later_without_asking_the_view_to_give_way);
	// Issue #267 - an external file open reuses normal navigation without hiding its target.
	tests.add("Should reveal an external file without bypassing navigation guards"s,
	          should_reveal_an_external_file_without_bypassing_navigation_guards);

	//
	// Selection and command enablement
	//
	tests.add("Should select correctly"s, should_select_items);
	tests.add("Should Enable based on selection"s, should_enable_based_on_selection);
	tests.add("Should toggle rating"s, should_toggle_rating);
	tests.add("Should keep the session changes in sight"s, should_keep_the_session_changes_in_sight);
	tests.add("Should count copied items without their sidecars"s, should_count_copied_items_without_their_sidecars);
	tests.add("Should leave the selection alone over empty space"s, should_leave_the_selection_alone_over_empty_space);
	tests.add("Should target only visible items when filtered"s, should_target_only_visible_items_when_filtered);
	tests.add("Should keep shuffle exclusive with sorting"s, should_keep_shuffle_exclusive_with_sorting);
	tests.add("Should not claim one key for two commands"s, should_not_claim_one_key_for_two_commands);
	tests.add("Should spell keyboard accelerators"s, should_spell_keyboard_accelerators);
	tests.add("Should record a bit for every view"s, should_record_a_bit_for_every_view);
	tests.add("Should label grouping and sorting"s, should_label_grouping_and_sorting);
	// Discussion #251 - the filter toolbar scrolled away
	tests.add("Should keep the items controls in the status bar"s, should_keep_the_items_controls_in_the_status_bar);
	tests.add("Should put task commands in the status bar"s, should_put_task_commands_in_the_status_bar);
	tests.add("Should end the options view with save"s, should_end_the_options_view_with_save);
	tests.add("Should save only what the options view edits"s, should_save_only_what_the_options_view_edits);
	// Issue #267 - closing another window must not undo the current launch-routing preference.
	tests.add("Should preserve newer window reuse preferences"s, should_preserve_newer_window_reuse_preferences);
	tests.add("Should pause media while the options view shows"s, should_pause_media_while_the_options_view_shows);
	tests.add("Should ask before leaving options with changes"s, should_ask_before_leaving_options_with_changes);
	tests.add("Should offer library types for collection folders"s, should_offer_library_types_for_collection_folders);
	tests.add("Should arrange a bar from its trailing end"s, should_arrange_a_bar_from_its_trailing_end);
	tests.add("Should keep a short status bar's last command"s, should_keep_a_short_status_bar_s_last_command);
	tests.add("Should arrange a bar from its leading end"s, should_arrange_a_bar_from_its_leading_end);
	tests.add("Should summarise the selection for the status bar"s, should_summarise_the_selection_for_the_status_bar);

	//
	// Address box editing session
	//
	tests.add("Should run an address editing session"s, should_run_an_address_editing_session);
	tests.add("Should accept a completion without running it"s, should_accept_a_completion_without_running_it);
	tests.add("Should restore the draft when a previewing session ends"s,
	          should_restore_the_draft_when_a_previewing_session_ends);
	tests.add("Should commit the highlighted completion or the visible address"s,
	          should_commit_the_highlighted_completion_or_the_visible_address);
	tests.add("Should rank address completions"s, should_rank_address_completions);
	tests.add("Should offer recent searches for an empty address"s, should_offer_recent_searches_for_an_empty_address);
	tests.add("Should discard a superseded completion"s, should_discard_a_superseded_completion);
	tests.add("Should predict face book names in the address bar"s, should_predict_face_book_names_in_the_address_bar);
	tests.add("Should count address completions"s, should_count_address_completions);
	tests.add("Should deliver address portraits without replacing the prediction"s,
	          should_deliver_address_portraits_without_replacing_the_prediction);
	// APP-007 - application-local fades must honor the shared alpha-animation gate.
	tests.add("Should gate application alpha effects"s, should_gate_application_alpha_effects);
	tests.add("Should snap app logo hover when animations are disabled"s,
	          should_snap_app_logo_hover_when_animations_are_disabled);
	tests.add("Should keep the idle logo drifting slowly"s, should_keep_the_idle_logo_drifting_slowly);
	tests.add("Should skip stable progress repaints when animations are disabled"s,
	          should_skip_stable_progress_repaints_when_animations_are_disabled);
	// APP-005 - destination-folder completion work is asynchronous and latest-wins.
	tests.add("Should clear destination completions by emptying the query"s,
	          should_clear_destination_completions_by_emptying_the_query);
	tests.add("Should choose a destination folder with a double-click"s,
	          should_choose_a_destination_folder_with_a_double_click);
	tests.add("Should rank destination completions after combining sources"s,
	          should_rank_destination_completions_after_combining_sources);
	tests.add("Should skip stale destination completion work"s, should_skip_stale_destination_completion_work);
	tests.add("Should release stale destination completion callbacks on the UI thread"s,
	          should_release_stale_destination_completion_callbacks_on_the_ui_thread);
	// Concurrency review #19 - operation dialogs could be destroyed by the worker holding their status.
	tests.add("Should hand the last ui-owned reference back to the UI thread"s,
	          should_hand_the_last_ui_owned_reference_back_to_the_ui_thread);
	// APP-008 - sidebar structural rebuild packets carry settings snapshots and request identity.
	tests.add("Should keep sidebar rebuild packets current"s, should_keep_sidebar_rebuild_packets_current);
	tests.add("Should count sidebar rows from the search index"s, should_count_sidebar_rows_from_the_search_index);
	// APP-011 - cancelling a globe drag consumes its release.
	tests.add("Should consume cancelled globe release"s, should_consume_cancelled_globe_release);

	//
	// Browsing sequence
	//
	tests.add("Should step over items that cannot play"s, should_step_over_items_that_cannot_play);
	// Issue #250 - the cursor reset instead of landing after the set that left
	tests.add("Should land on what followed a removed set"s, should_land_on_what_followed_a_removed_set);
	tests.add("Issue #265: Should land on a copy that stays after deleting copies"s,
	          should_land_on_a_copy_that_stays_after_deleting_copies);
	// Issue #265 - deleting a copy jumped the listing to the top, and could leave the deleted file listed until F5
	tests.add("Should remove deleted items in place"s, should_remove_deleted_items_in_place);
	tests.add("Should keep what a superseded open asked for"s, should_keep_what_a_superseded_open_asked_for);
	tests.add("Should offer a slideshow only when something can play"s,
	          should_offer_a_slideshow_only_when_something_can_play);
	tests.add("Should move between sibling folders"s, should_move_between_sibling_folders);
	tests.add("Should decide what follows an item"s, should_decide_what_follows_an_item);
	tests.add("Should hold a slideshow while a menu is open"s, should_hold_a_slideshow_while_a_menu_is_open);

	//
	// Rename execution
	//
	tests.add("Should Rename"s, should_rename);
	tests.add("Should not overwrite during rename"s, should_not_overwrite_during_rename);
	tests.add("Should rename file case"s, should_rename_file_case);
	tests.add("Should rollback rename when sidecar fails"s, should_rollback_rename_when_sidecar_fails);
	tests.add("Should detect original path"s, should_detect_original_path);

	//
	// Collection membership and crash recording
	//
	tests.add("Should toggle collection entry"s, should_toggle_collection_entry);
	// Issue #256 - a collection folder lost a letter, and every folder after the first, at a restart.
	tests.add("Should keep collection folders across a restart"s, should_keep_collection_folders_across_a_restart);
	tests.add("Should record crashes"s, should_record_crashes);
	// APP-013 - bucket size is not an unset global photo index.
	tests.add("Should anchor duplicate report sets"s, should_anchor_duplicate_report_sets);
	// APP-014 - requested output errors are command failures, not stdout fallback.
	tests.add("Should fail duplicate report command output errors"s,
	          should_fail_duplicate_report_command_output_errors);
}
