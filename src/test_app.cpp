// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for application coordination (app*, view_* task views): command registration
// and input dispatch, settings persistence, headless Movie batches, and rename, import, sync and convert planning.

#include "pch.h"
#include "files.h"
#include "test.h"
#include "model_db.h"
#include "model_index.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "util_crash_files_db.h"
#include "util_zip.h"
#include "app_command_line.h"
#include "app_util.h"
#include "app_settings.h"
#include "app_text.h"
#include "app_commands.h"
#include "app_search.h"
#include "ui_dialog.h"
#include "app.h"
#include "app_sidebar.h"
#include "app_dialogs.h"
#include "view_movie.h"
#include "view_rename.h"
#include "view_batch.h"
#include "view_tags.h"
#include "view_items.h"

extern std::function<void()> test_during_detach_file_handles;

// Rolling a release back is ordinary, and the settings store is shared with the build rolled back
// to. An order it cannot name falls through its switches to a default, so the user silently loses
// the order they chose - which is exactly the user this release's migration just moved onto one.
static void should_store_an_order_an_older_build_can_read()
{
	const auto plain = [](const group_by o) { return static_cast<uint32_t>(group_order_older_builds_understand(o)); };
	const auto ex = [](const group_by o) { return static_cast<uint32_t>(o); };

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

static void should_bound_advanced_search_hover_thumbnail_retries()
{
	auto retries = 2;
	assert_equal(true, should_refresh_advanced_search_hover_thumbnail(false, true, false, retries),
	             "first failed thumbnail hover is retried");
	assert_equal(1, retries, "retry budget decrements");
	assert_equal(true, should_refresh_advanced_search_hover_thumbnail(false, true, true, retries),
	             "in-flight staging keeps the hover refreshing without spending the budget");
	assert_equal(1, retries, "in-flight refresh does not spend retry budget");
	assert_equal(true, should_refresh_advanced_search_hover_thumbnail(false, true, false, retries),
	             "second failed thumbnail hover is retried");
	assert_equal(0, retries, "retry budget reaches zero");
	assert_equal(false, should_refresh_advanced_search_hover_thumbnail(false, true, false, retries),
	             "exhausted thumbnail retries stop refreshing");
	assert_equal(false, should_refresh_advanced_search_hover_thumbnail(true, true, false, retries),
	             "an available surface needs no refresh");
}

static void should_persist_to_ini_file()
{
	const auto settings_folder = _temps.folder().combine("ini-settings");
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
	assert_equal(true, settings->read("test_section", "uint32_value", read_uint32), "read uint32",
	             "INI file settings");
	assert_equal(test_uint32, read_uint32, "uint32 value", "INI file settings");

	// Test uint64_t
	constexpr uint64_t test_uint64 = 0xFFFFFFFFFFFFull;
	settings->write("test_section", "uint64_value", test_uint64);
	uint64_t read_uint64 = 0;
	assert_equal(true, settings->read("test_section", "uint64_value", read_uint64), "read uint64",
	             "INI file settings");
	assert_equal(test_uint64, read_uint64, "uint64 value", "INI file settings");

	// Test string
	const auto test_string = "Hello, World! With special chars: äöü"s;
	settings->write("test_section", "string_value", test_string);
	std::string read_string;
	assert_equal(true, settings->read("test_section", "string_value", read_string), "read string",
	             "INI file settings");
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
	settings->write("test_section", "binary_value", df::cspan{test_binary.data(), test_binary.size()});
	std::vector<uint8_t> read_buffer(test_binary.size());
	size_t read_len = read_buffer.size();
	assert_equal(true, settings->read("test_section", "binary_value", read_buffer.data(), read_len),
	             "read binary", "INI file settings");
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
	const auto root = df::windows_path_semantics ? "C:\\Users\\someone\\"s : "/home/someone/"s;
	const auto damaged = std::format(R"({0}Pictures"' "{0}Videos" "{0}Music" "{0}OneDrive - Work\Desktop" '"{0}Downloads)",
	                                 root);

	assert_equal(true, recent_state::is_damaged_entry(std::format(R"({0}Pictures"' "{0}Videos)", root)),
	             "a quote wedged inside a path is damage");
	for (const auto& healthy : {root + "Pictures", std::format(R"({}photos "new york")", root),
	                            "\"" + root + "My Photos\" beach", "Bob's \"party\""s, "@photo beach"s,
	                            std::format(R"({}Photos loc:"Boulder City")", root),
	                            std::format(R"({}Photos -"draft")", root),
	                            std::format(R"({}Bob's photos tag:"summer")", root)})
	{
		assert_equal(false, recent_state::is_damaged_entry(healthy), "a real entry is kept", healthy);
	}

	{
		// A folder-scoped search with a quoted term is ordinary recent text and must survive a restart.
		const auto search = std::format(R"({}Photos loc:"Boulder City")", root);
		const auto search_folder = _temps.folder().combine("ini-recent-search");
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

	const auto settings_folder = _temps.folder().combine("ini-recent");
	platform::create_folder(settings_folder);
	const auto settings = platform::create_ini_file_settings(settings_folder);
	settings->write("", "recent_searches", damaged + " " + root + "Kept");

	assert_equal(true, recent_state::is_damaged_entry(std::format(R"('"{}Downloads)", root)),
	             "stray quotes leading a path are damage");

	recent_state recents;
	recents.read("", "recent_searches", settings);
	const std::vector<std::string> expected{
		root + "Videos", root + "Music", root + R"(OneDrive - Work\Desktop)", root + "Kept"
	};
	assert_equal(str::combine(expected, "|", false), str::combine(recents.items(), "|", false),
	             "exactly the readable entries load, in order");

	recents.write("", "recent_searches", settings);
	recent_state reread;
	reread.read("", "recent_searches", settings);
	assert_equal(true, recents.items() == reread.items(), "the cleaned list round-trips");
}

#ifndef _WIN32
static void should_commit_linux_ini_file_settings_after_durable_save()
{
	const auto settings_folder = _temps.folder().combine("linux-ini-settings");
	platform::create_folder(settings_folder);
	const auto settings_path = settings_folder.combine_file("diffractor.ini");

	auto settings = platform::create_ini_file_settings(settings_folder);
	assert_equal(true, settings->write("test_section", "string_value", "original"), "initial write");

	for (const auto failure : {
		     platform::linux_settings_test_failure::after_open,
		     platform::linux_settings_test_failure::after_write,
		     platform::linux_settings_test_failure::before_replace
	     })
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
	const auto settings_folder = _temps.folder().combine("linux-ini-unchanged");
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
	const auto settings_folder = _temps.folder().combine("linux-ini-owned-stage");
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

	const df::file_path src_path(test_files_folder, "Test.jpg");
	const auto save_path_1 = _temps.next_path(".jpg");
	const auto save_path_2 = _temps.next_path(".jpg");

	platform::copy_file(src_path, save_path_1, false, false);

	const auto test_item = load_item(index, save_path_1, false);

	assert_equal(true, test_item->rename(index, save_path_2.file_name_without_extension()).success(), "can rename");
	assert_equal(save_path_2.name(), test_item->name(), "renamed");
	assert_equal(true, save_path_2.exists(), "renamed exists");

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
}

static void should_rename_name_token_without_extension()
{
	df::item_set items;
	const auto item = std::make_shared<df::item_element>(
		df::file_path(test_files_folder, "Test.jpg"), df::index_file_item{});
	items.add(item);

	const auto renames = calc_item_renames(items, "{name}-edited", 1, collision_policy::block_run);
	assert_equal(1ULL, static_cast<uint64_t>(renames.size()), "one rename planned");
	assert_equal("Test-edited", renames.front().new_name, "name token excludes extension");
}

static void should_reject_unusable_rename_targets()
{
	df::item_set items;
	items.add(std::make_shared<df::item_element>(df::file_path(test_files_folder, "Test.jpg"), df::index_file_item{}));

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
	items.add(std::make_shared<df::item_element>(df::file_path(test_files_folder, "Test.jpg"), df::index_file_item{}));
	items.add(std::make_shared<df::item_element>(df::file_path(test_files_folder, "Small.jpg"), df::index_file_item{}));

	const auto renames = calc_item_renames(items, "same", 1, collision_policy::block_run);
	assert_equal(false, can_rename_items(renames), "duplicate rename targets rejected");
	assert_equal(false, renames.front().valid, "first duplicate target invalid");
	assert_equal(false, renames.back().valid, "second duplicate target invalid");
}

static std::vector<rename_source> make_rename_sources(const df::folder_path root,
                                                     const std::vector<std::string>& names)
{
	const df::blob contents = {1};
	std::vector<rename_source> sources;
	for (const auto& name : names)
	{
		const auto path = root.combine_file(name + ".jpg");
		df::blob_save_to_file(contents, path);
		rename_source source;
		source.source = path;
		source.original_name = name;
		sources.emplace_back(std::move(source));
	}
	return sources;
}

static void should_plan_the_default_rename_template()
{
	const auto root = _temps.next_folder("rename-default");
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
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), with_sidecar, false, false);
	platform::copy_file(df::file_path(test_files_folder, "IMG_0604.xmp"), with_sidecar.extension(".xmp"), false, false);

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
	const auto root = _temps.next_folder("rename-rotate");
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
	const auto root = _temps.next_folder("rename-identity");
	const auto sources = make_rename_sources(root, {"alpha"});
	const auto occupied = root.combine_file("renamed.jpg");
	df::blob_save_to_file(df::blob{1, 2, 3}, occupied);

	const auto renames = calc_item_renames(sources, "renamed", 1, collision_policy::replace);
	assert_equal(1_z, renames.size(), "one row");

	const auto& row = renames.front();
	assert_equal(true, row.source_attributes.exists(), "the source identity was recorded");
	assert_equal(true, row.destination_attributes.exists(), "so was the destination the row will replace");
	assert_equal(true, unchanged_since_analysis(row.source, row.source_attributes.modified,
	                                            row.source_attributes.size),
	             "an untouched source still matches the review");
	assert_equal(true, unchanged_since_analysis(row.destination, row.destination_attributes.modified,
	                                            row.destination_attributes.size),
	             "and so does an untouched destination");

	// Both paths still exist, which is all the run used to ask. Only the recorded identity says the
	// content behind them is no longer what was reviewed.
	df::blob_save_to_file(df::blob{1, 2, 3, 4, 5}, row.source);
	df::blob_save_to_file(df::blob{9}, row.destination);

	assert_equal(true, row.source.exists(), "the source path is still occupied");
	assert_equal(true, row.destination.exists(), "and so is the destination path");
	assert_equal(false, unchanged_since_analysis(row.source, row.source_attributes.modified,
	                                             row.source_attributes.size),
	             "a rewritten source no longer matches the review");
	assert_equal(false, unchanged_since_analysis(row.destination, row.destination_attributes.modified,
	                                             row.destination_attributes.size),
	             "nor does a destination that became another file");
}

static void should_cascade_skipped_rename_rows()
{
	const auto root = _temps.next_folder("rename-cascade");
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
	const auto root = _temps.folder().combine(std::format("rename-sidecar-{}", platform::tick_count()));
	platform::create_folder(root);
	const auto source = root.combine_file("photo.jpg");
	const auto sidecar = root.combine_file("photo.xmp");
	const df::blob contents = {1};
	df::blob_save_to_file(contents, source);
	df::blob_save_to_file(contents, sidecar);
	df::blob_save_to_file(contents, root.combine_file("renamed.xmp"));

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

static void should_plan_unique_convert_outputs()
{
	std::vector<convert_source> sources;
	sources.emplace_back(df::file_path("c:\\one\\photo.jpg"), sizei{}, str::cached{});
	sources.emplace_back(df::file_path("c:\\two\\photo.png"), sizei{}, str::cached{});

	const auto plan = plan_convert_outputs(df::folder_path("c:\\destination"), sources, ".webp",
	                                       collision_policy::block_run);
	assert_equal(2_z, plan.size(), "all conversion sources planned");
	assert_equal("photo.webp", plan[0].destination.name(), "first conversion keeps basename");
	assert_equal("photo (2).webp", plan[1].destination.name(), "duplicate conversion basename is suffixed");
	assert_equal(false, plan[0].destination == plan[1].destination, "conversion outputs are unique");
}

// A destination that names another row's source is a file this run reads. Writing it destroyed an
// input the review listed as a source, and that row then converted whatever the earlier one had
// just written over it. Ordering matters: the clobbering row is planned first, so the source it
// lands on has not yet claimed its own destination.
static void should_not_plan_a_convert_output_over_a_source()
{
	std::vector<convert_source> sources;
	sources.emplace_back(df::file_path("c:\\a\\photo.png"), sizei{}, str::cached{});
	sources.emplace_back(df::file_path("c:\\destination\\photo.jpg"), sizei{}, str::cached{});

	const auto plan = plan_convert_outputs(df::folder_path("c:\\destination"), sources, ".jpg",
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

// A recipient sees a shared copy under its staged name, so two photos named alike in different
// folders must not land on one name, and a file the options leave alone goes as itself. Breaking
// the free-name step fails the "(2)" row; skipping the clear fails the stale row.
static void should_prepare_shared_copies_under_their_own_names()
{
	const auto root = _temps.next_folder("share");
	const auto staging = root.combine("staging");
	const auto one = root.combine("one");
	const auto two = root.combine("two");
	platform::create_folder(staging);
	platform::create_folder(one);
	platform::create_folder(two);

	const auto png = test_files_folder.combine("excluded1").combine_file("gamma.png");
	const auto jpeg = root.combine_file("Progressive.jpg");
	const std::vector sources = {one.combine_file("photo.png"), two.combine_file("photo.png"), jpeg};
	platform::copy_file(png, sources[0], false, false);
	platform::copy_file(png, sources[1], false, false);
	platform::copy_file(test_files_folder.combine("excluded1").combine_file("Progressive.jpg"), jpeg, false, false);
	const auto source_bytes = df::blob_from_file(sources[0]);

	const auto stale = staging.combine_file("stale.jpg");
	df::blob_save_to_file(df::blob{1}, stale);

	null_item_results_ui status;
	outgoing_options convert;
	convert.convert_to_jpeg = true;

	const auto converted = prepare_outgoing_files(sources, convert, staging, status);
	assert_equal(true, converted.success, "the conversion is prepared");
	assert_equal(3_z, converted.paths.size(), "one outgoing file per source");
	assert_equal(staging.combine_file("photo.jpg").str(), converted.paths[0].str(), "the copy keeps its name");
	assert_equal(staging.combine_file("photo (2).jpg").str(), converted.paths[1].str(),
	             "a second photo of that name does not overwrite the first");
	assert_equal(jpeg.str(), converted.paths[2].str(), "a JPEG with nothing to convert goes as itself");
	assert_equal(false, stale.exists(), "what the previous share staged is cleared");
	assert_equal(true, df::blob_from_file(sources[0]) == source_bytes, "the source is unchanged");

	const auto copy_bytes = df::blob_from_file(converted.paths[1]);
	assert_equal(true, copy_bytes.size() > 2 && copy_bytes[0] == 0xFF && copy_bytes[1] == 0xD8,
	             "the copy is a JPEG");

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
	assert_equal(false, converted.paths[0].exists(), "the earlier share's copies are cleared");

	const auto unchanged = prepare_outgoing_files(sources, {}, staging, status);
	assert_equal(true, unchanged.success, "nothing to prepare still succeeds");
	assert_equal(sources.size(), unchanged.paths.size(), "every source goes");
	assert_equal(sources[1].str(), unchanged.paths[1].str(), "as itself");
	assert_equal(true, zipped.paths.front().exists(), "and nothing staged is cleared when nothing is written");

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
	const auto limited_size = codecs.scan_file(limited.paths.front(), false,
	                                           files::file_type_from_name(limited.paths.front())).dimensions();
	assert_equal(128, std::max(limited_size.cx, limited_size.cy), "its longest side is the limit");
}

static void should_adjust_item_dates_from_snapshot()
{
	const df::date_t original_start(2025, 8, 16, 18, 11, 56);
	const df::date_t new_start(2025, 8, 17, 9, 30, 0);
	constexpr auto offset = 10 * df::date_t::intervals_per_second;
	assert_equal(new_start + offset, adjusted_item_date(original_start + offset, new_start, original_start),
	             "dated item preserves offset");
	assert_equal(new_start, adjusted_item_date({}, new_start, original_start),
	             "undated item uses new start");

	// The row says which tag it is about to shift, so a selection whose files are dated by different
	// things is visible before the run rather than after it.
	const auto with_shutter = std::make_shared<prop::item_metadata>();
	with_shutter->dates.add(prop::date_source::exif_original, df::date_t(2025, 8, 16, 18, 11, 56));
	with_shutter->dates.add(prop::date_source::file_modified, df::date_t(2026, 8, 19, 21, 30, 0));
	assert_equal("EXIF DateTimeOriginal", adjust_date_source_name(with_shutter), "the shutter answers first");

	const auto scanned = std::make_shared<prop::item_metadata>();
	scanned->dates.add(prop::date_source::xmp_create, df::date_t(2019, 5, 4, 9, 0, 0));
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

	void start_item(std::string_view) override
	{
	}

	void end_item(const std::string_view name, const item_status status) override
	{
		items.emplace_back(name, status);
	}

	bool has_failures() const override
	{
		return std::ranges::any_of(items, [](const auto& i) { return i.second == item_status::fail; });
	}

	void abort(std::string_view) override
	{
	}

	void complete(const std::string_view message) override
	{
		if (completed) return;
		completion = message;
		completed = true;
	}

	void show_errors() override
	{
	}

	void message(std::string_view, int64_t, int64_t) override
	{
	}

	void show_message(std::string_view) override
	{
	}

	bool is_canceled() const override { return canceled; }

	void wait_for_complete() const override
	{
	}

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
	const auto root = _temps.next_folder("sync-revalidate");
	const auto local = root.combine("local");
	const auto remote = root.combine("remote");
	platform::create_folder(local);
	platform::create_folder(remote);

	write_test_file(local.combine_file("keep.txt"), "keep");
	write_test_file(local.combine_file("changed.txt"), "changed");
	write_test_file(local.combine_file("claimed.txt"), "claimed");

	df::index_roots roots;
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

	assert_equal(true, remote.combine_file("keep.txt").exists(), "unchanged row was copied");
	assert_equal(false, remote.combine_file("changed.txt").exists(), "refused row wrote nothing");
	assert_equal("not yours"s, read_test_file(remote.combine_file("claimed.txt")),
	             "claimed destination was not overwritten");
}

// A delete was reviewed against the file's content, so that is what it is held to at run time.
static void should_revalidate_sync_deletes()
{
	const auto root = _temps.next_folder("sync-delete-revalidate");
	const auto local = root.combine("local");
	const auto remote = root.combine("remote");
	platform::create_folder(local);
	platform::create_folder(remote);

	write_test_file(remote.combine_file("stale.txt"), "stale");
	write_test_file(remote.combine_file("touched.txt"), "touched");

	df::index_roots roots;
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
	assert_equal(false, remote.combine_file("stale.txt").exists(), "unchanged file is gone");
	assert_equal(true, remote.combine_file("touched.txt").exists(), "changed file survives");
}

// A full-path exclusion is expressed in local collection coordinates. Sync's remote tree has to
// project each relative remote folder back into that same local scope before deciding whether it is
// in or out; otherwise reverse sync can copy excluded files back and mirror delete can remove them.
static void should_apply_sync_full_path_exclusions_to_remote_counterparts()
{
	const auto root = _temps.next_folder("sync-full-path-exclude");
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

	df::index_roots roots;
	roots.folders.emplace(local);
	roots.excludes.emplace(local.combine("Private"));
	roots.exclude_wildcards.emplace(str::cache("*.tmp"));

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
	const auto root = _temps.next_folder("sync-multi-root-exclude");
	const auto local1 = root.combine("local1");
	const auto local2 = root.combine("local2");
	const auto remote = root.combine("remote");
	platform::create_folder(local1.combine("Shots"));
	platform::create_folder(local2.combine("Shots"));
	platform::create_folder(remote.combine("Shots"));
	write_test_file(local2.combine("Shots").combine_file("photo.txt"), "local2");
	write_test_file(remote.combine("Shots").combine_file("photo.txt"), "remote");

	df::index_roots roots;
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
	const auto src1 = _temps.folder().combine("import-dup-1");
	const auto src2 = _temps.folder().combine("import-dup-2");
	const auto dest = _temps.folder().combine("import-dup-dest");
	platform::create_folder(src1);
	platform::create_folder(src2);
	platform::create_folder(dest);

	const auto source = df::file_path(test_files_folder, "Test.jpg");
	platform::copy_file(source, src1.combine_file("same.jpg"), false, false);
	platform::copy_file(source, src2.combine_file("same.jpg"), false, false);

	const auto make_item = [](const df::folder_path folder)
	{
		const auto path = folder.combine_file("same.jpg");
		const auto fi = platform::file_attributes(path);
		folder_scan_item item;
		item.folder = folder;
		item.item.name = path.name();
		item.item.file_modified = df::date_t(fi.modified);
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
	df::unique_paths destinations;

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
	const auto root = _temps.next_folder("import-folder-fail");
	const auto src = root.combine("source");
	const auto dest_ok = root.combine("a-ok");
	const auto dest_blocked = root.combine("b-blocked");
	platform::create_folder(src);

	write_test_file(src.combine_file("first.txt"), "first");
	write_test_file(src.combine_file("second.txt"), "second");

	// A file where the second destination folder needs to be, so creating it cannot succeed.
	write_test_file(df::file_path(root, "b-blocked"), "in the way");

	const auto make_row = [&src](const std::string_view name, const df::folder_path destination)
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

	assert_equal(true, dest_ok.combine_file("first.txt").exists(), "the first folder was imported into");
	assert_equal(true, status->status_of("first.txt") == item_status::success, "and its row says so");

	// The whole point: the run stopped, but what it wrote is still accounted for.
	assert_equal(1u, static_cast<uint32_t>(run.imports.size()),
	             "the file that did arrive is recorded as imported");
	assert_equal(true, run.folder == dest_ok, "and the run names the folder it reached");
}

// Import holds each reviewed row to the file that was reviewed, and lets the file system prove a
// destination is free rather than asking and then writing.
static void should_revalidate_import_rows(shared_test_context& stc)
{
	const auto root = _temps.next_folder("import-revalidate");
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
		item.item.file_modified = df::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.size = df::file_size(fi.size);
		item.item.ft = files::file_type_from_name(path);
		return item;
	};

	const std::vector<folder_scan_item> items{
		make_item("keep.txt"), make_item("changed.txt"), make_item("claimed.txt")
	};

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
	assert_equal(true, str::contains(status->completion, tt.sync_analysis_changed.sv()),
	             "the run explains that files changed since the review");

	assert_equal(true, dest.combine_file("keep.txt").exists(), "unchanged row was imported");
	assert_equal(false, dest.combine_file("changed.txt").exists(), "refused row wrote nothing");
	assert_equal("not yours"s, read_test_file(dest.combine_file("claimed.txt")),
	             "claimed destination was not overwritten");
}

// A sidecar is part of the group the user reviewed, not a detail of it. The primary was held to the
// file that was reviewed and its sidecars were not, so a sidecar edited since the review travelled
// with an approved file as content nobody approved.
static void should_revalidate_import_sidecars(shared_test_context& stc)
{
	const auto root = _temps.next_folder("import-sidecar-revalidate");
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
		item.item.file_modified = df::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.size = df::file_size(fi.size);
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
	assert_equal(true, dest.combine_file("keep.xmp").exists(), "and brings its sidecar");

	assert_equal(true, status->status_of("racing.txt") == item_status::fail,
	             "a group whose sidecar changed is refused");
	assert_equal(1, static_cast<int>(run.refused), "and is reported as a refusal so the run can say why");
	assert_equal(false, dest.combine_file("racing.txt").exists(), "the refused group wrote nothing");
	assert_equal(false, dest.combine_file("racing.xmp").exists(), "not even the sidecar that changed");
}

// The RAW and the JPEG of one shot share an XMP by base name, so both rows name it. The group travels
// once: a second row carrying the sidecar again collided with the copy the first had just written.
// Under Replace that refused the JPEG outright, and under Skip the JPEG was blocked by its own twin.
static void should_import_a_shared_sidecar_once(shared_test_context& stc)
{
	for (const auto [policy, move] : {
		     std::pair{collision_policy::replace, false}, {collision_policy::replace, true},
		     {collision_policy::skip, false}, {collision_policy::skip, true},
		     {collision_policy::auto_rename, false}, {collision_policy::auto_rename, true}})
	{
		const auto root = _temps.next_folder("import-shared-sidecar");
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
			item.item.file_modified = df::date_t(fi.modified);
			item.item.file_created = fi.created;
			item.item.size = df::file_size(fi.size);
			item.item.ft = files::file_type_from_name(path);

			const auto metadata = std::make_shared<prop::item_metadata>();
			metadata->sidecars = str::cache("shot.xmp;shot.dop");
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
		assert_equal(!move, sidecar.exists(), "Move releases the shared source after both primary writes");
		assert_equal("shared adjustments"s, read_test_file(dest.combine_file("shot.dop")),
		             "each dependency is published once per row, not once per new companion");
		assert_equal(!move, src.combine_file("shot.dop").exists(), "the second shared source has the same lifetime");
	}
}

static folder_scan_item import_test_item(const df::file_path path, const std::string_view sidecars = {})
{
	const auto fi = platform::file_attributes(path);
	folder_scan_item item;
	item.folder = path.folder();
	item.item.name = path.name();
	item.item.file_modified = df::date_t(fi.modified);
	item.item.file_created = fi.created;
	item.item.size = df::file_size(fi.size);
	item.item.ft = files::file_type_from_name(path);
	if (!sidecars.empty())
	{
		const auto metadata = std::make_shared<prop::item_metadata>();
		metadata->sidecars = str::cache(sidecars);
		item.item.metadata.store(metadata);
	}
	return item;
}

static void should_preserve_shared_import_sidecars_after_auto_rename(shared_test_context& stc)
{
	for (const auto move : {false, true})
	{
		const auto root = _temps.next_folder("import-shared-renamed");
		const auto src = root.combine("source");
		const auto dest = root.combine("dest");
		platform::create_folder(src);
		platform::create_folder(dest);
		write_test_file(src.combine_file("shot.jpg"), "jpeg");
		write_test_file(src.combine_file("shot.nef"), "raw");
		write_test_file(src.combine_file("shot.xmp"), "shared metadata");
		write_test_file(dest.combine_file("shot.nef"), "existing raw");
		write_test_file(dest.combine_file("shot (2).nef"), "another raw");

		const std::vector items{
			import_test_item(src.combine_file("shot.jpg"), "shot.xmp"),
			import_test_item(src.combine_file("shot.nef"), "shot.xmp"),
			import_test_item(src.combine_file("shot.xmp"))
		};
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
		assert_equal("another raw"s, read_test_file(dest.combine_file("shot (2).nef")), "the second collision survives");
		assert_equal(false, dest.combine_file("shot (2).xmp").exists(), "no standalone companion is auto-renamed");
		assert_equal(2_z, count_imports(analysis), "automatic companions do not become independent rows");
		assert_equal(2_z, imported.imports.size(), "both primary imports are recorded");
		assert_equal(true, status->status_of("shot.jpg") == item_status::success, "the JPEG reports success");
		assert_equal(true, status->status_of("shot.nef") == item_status::success, "the RAW reports success");
		for (const auto name : {"shot.jpg", "shot.nef", "shot.xmp"})
			assert_equal(!move, src.combine_file(name).exists(), "only a completed Move removes its sources");
	}
}

static void should_keep_shared_import_sidecars_for_unfinished_sources(shared_test_context& stc)
{
	for (const auto outcome : {item_status::ignore, item_status::fail, item_status::cancel})
	{
		const auto root = _temps.next_folder("import-shared-unfinished");
		const auto src = root.combine("source");
		const auto dest = root.combine("dest");
		platform::create_folder(src);
		platform::create_folder(dest);
		write_test_file(src.combine_file("shot.jpg"), "jpeg");
		write_test_file(src.combine_file("shot.nef"), "raw");
		write_test_file(src.combine_file("shot.xmp"), "shared metadata");
		if (outcome == item_status::ignore) write_test_file(dest.combine_file("shot.nef"), "existing raw");

		const std::vector items{
			import_test_item(src.combine_file("shot.jpg"), "shot.xmp"),
			import_test_item(src.combine_file("shot.nef"), "shot.xmp"),
			import_test_item(src.combine_file("shot.xmp"))
		};
		import_options options;
		options.dest_folder = dest;
		options.is_move = true;
		options.collision = collision_policy::skip;
		const auto analysis = import_analysis(items, options, {}, test_token);
		const auto status = std::make_shared<recording_status>();
		auto first_write = true;
		test_import_sidecar_write_override = [outcome, dest, status, &first_write](df::file_path)
			-> std::optional<platform::file_op_result>
		{
			if (first_write)
			{
				first_write = false;
				if (outcome == item_status::fail) write_test_file(dest.combine_file("shot.jpg"), "new occupant");
				if (outcome == item_status::cancel) status->canceled = true;
			}
			return {};
		};
		const df::scope_exit restore_hook([] { test_import_sidecar_write_override = {}; });
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
	const auto root = _temps.next_folder("import-replace-revalidate");
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
		item.item.file_modified = df::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.size = df::file_size(fi.size);
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
		const auto root = _temps.next_folder(is_move ? "import-move-sidecar-rollback" : "import-copy-sidecar-rollback");
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
		item.item.file_modified = df::date_t(fi.modified);
		item.item.file_created = fi.created;
		item.item.size = df::file_size(fi.size);
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
	const auto root = _temps.next_folder("import-copy-free-sidecar-undo");
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
	const df::scope_exit restore_readonly([source_sidecar] { platform::make_file_writable(source_sidecar); });
	auto locked_source_sidecar = platform::open_file(source_sidecar, platform::file_open_mode::read);
	assert_equal(true, locked_source_sidecar != nullptr, "the source sidecar is held open");

	const auto fi = platform::file_attributes(source);
	folder_scan_item item;
	item.folder = src;
	item.item.name = source.name();
	item.item.file_modified = df::date_t(fi.modified);
	item.item.file_created = fi.created;
	item.item.size = df::file_size(fi.size);
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
	const df::scope_exit restore_destination_writable([destination] { platform::make_file_writable(destination); });
#endif

	const auto status = std::make_shared<recording_status>();
	import_copy(stc.empty_index, status, analysis, options, test_token);
#ifdef _WIN32
	locked.reset();
#endif
	locked_source_sidecar.reset();

	assert_equal(true, status->status_of("photo.jpg") == item_status::fail, "the row reports the primary failure");
	assert_equal("source xmp"s, read_test_file(source_sidecar), "the source sidecar remains byte-identical");
	assert_equal(false, destination_sidecar.exists(), "the copied destination sidecar is removed");
}

static void should_delete_readonly_import_sidecar_rollback_copy(shared_test_context& stc)
{
	const auto root = _temps.next_folder("import-rollback-cleanup-failure");
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
	item.item.file_modified = df::date_t(fi.modified);
	item.item.file_created = fi.created;
	item.item.size = df::file_size(fi.size);
	item.item.ft = files::file_type_from_name(source);

	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = source_sidecar.name();
	item.item.metadata.store(metadata);

	import_options options;
	options.dest_folder = dest;
	options.collision = collision_policy::replace;

	platform::file_ptr rollback_lock;
	test_before_import_rollback_cleanup = [&rollback_lock](const df::file_path path)
	{
		rollback_lock = platform::open_file(path, platform::file_open_mode::read_write);
	};
	const df::scope_exit restore_cleanup_hook([&rollback_lock]
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
	const auto root = _temps.next_folder("import-sidecar-partial-restore");
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
	item.item.file_modified = df::date_t(fi.modified);
	item.item.file_created = fi.created;
	item.item.size = df::file_size(fi.size);
	item.item.ft = files::file_type_from_name(source);

	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = source_sidecar.name();
	item.item.metadata.store(metadata);

	import_options options;
	options.dest_folder = dest;
	options.collision = collision_policy::replace;

	test_import_sidecar_write_override = [destination_sidecar](const df::file_path path)
		-> std::optional<platform::file_op_result>
	{
		if (path != destination_sidecar) return {};
		write_test_file(destination_sidecar, "partial xmp");
		return platform::file_op_result{platform::file_op_result_code::FAILED, "simulated sidecar overwrite failure"};
	};
	const df::scope_exit restore_hook([] { test_import_sidecar_write_override = {}; });

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
	state.open(view, df::search_t().add_selector(test_files_folder), {});
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
		d.capacity = df::file_size(capacity);
		return d;
	};

	constexpr uint64_t card = 64'000'000'000ull;
	constexpr auto removable = platform::drive_type::removable;
	constexpr auto fixed = platform::drive_type::fixed;

	// Two fixed drives and a four-slot card reader with one card, in its last slot.
	const platform::drives drives = {
		drive("C:\\", fixed, card), drive("D:\\", fixed, card), drive("E:\\", removable, 0),
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

static void should_reject_missing_sync_folder()
{
	df::index_roots roots;
	roots.folders.emplace(_temps.folder().combine("missing-local"));

	const auto result = sync_analysis(roots, _temps.folder(), true, false, false, true, test_token);
	assert_equal(false, result.valid, "missing local folder invalidates sync analysis");
	assert_equal(true, result.empty(), "invalid sync analysis has no actions");
	assert_equal(true, result.reason != sync_invalid_reason::none, "records why the analysis is invalid");
	assert_equal(false, sync_invalid_message(result) == tt.platform_text.error_cannot_continue.sv(),
	             "a missing folder is not reported as an internal fault");
}

static void should_reject_overlapping_sync_folders()
{
	const auto local = _temps.folder().combine("sync-overlap-local");
	platform::create_folder(local);
	df::index_roots roots;
	roots.folders.emplace(local);

	const auto result = sync_analysis(roots, local.combine("remote"), true, false, false, false, test_token);
	assert_equal(false, result.valid, "nested remote folder invalidates sync analysis");
	assert_equal(true, result.reason == sync_invalid_reason::overlapping_paths, "reports overlapping folders");
	assert_equal(false, sync_invalid_message(result) == tt.platform_text.error_cannot_continue.sv(),
	             "overlapping folders are not reported as an internal fault");

	const auto accented_local = _temps.folder().combine("sync-overlap-é");
	const auto accented_remote = _temps.folder().combine("sync-overlap-è").combine("remote");
	platform::create_folder(accented_local);
	platform::create_folder(accented_remote.parent());
	platform::create_folder(accented_remote);
	df::index_roots accented_roots;
	accented_roots.folders.emplace(accented_local);

	const auto accented_result = sync_analysis(accented_roots, accented_remote, true, false, false, false, test_token);
	assert_equal(true, accented_result.valid, "accented sibling sync roots do not overlap");
}

static void should_reject_ambiguous_sync_roots()
{
	const auto local1 = _temps.folder().combine("sync-root-1");
	const auto local2 = _temps.folder().combine("sync-root-2");
	const auto remote = _temps.folder().combine("sync-remote");
	platform::create_folder(local1);
	platform::create_folder(local2);
	platform::create_folder(remote);
	const auto source = df::file_path(test_files_folder, "Test.jpg");
	platform::copy_file(source, local1.combine_file("same.jpg"), false, false);
	platform::copy_file(source, local2.combine_file("same.jpg"), false, false);

	df::index_roots roots;
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
	const auto local1 = _temps.folder().combine("unclaimed-root-1");
	const auto local2 = _temps.folder().combine("unclaimed-root-2");
	const auto remote = _temps.folder().combine("unclaimed-remote");
	platform::create_folder(local1);
	platform::create_folder(local2);
	platform::create_folder(remote);
	const auto source = df::file_path(test_files_folder, "Test.jpg");
	platform::copy_file(source, remote.combine_file("remote-only.jpg"), false, false);

	df::index_roots roots;
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

	assert_equal(true, calc_sync_action(true, false, 10 * s, 0, 100, 0, false, false, true, false) ==
	             sync_action::delete_local, "delete local is independent");
	assert_equal(true, calc_sync_action(false, true, 0, 10 * s, 0, 100, false, false, false, true) ==
	             sync_action::delete_remote, "delete remote is independent");
	assert_equal(true, calc_sync_action(true, false, 10 * s, 0, 100, 0, true, false, true, false) ==
	             sync_action::copy_remote, "copy local to remote takes precedence over delete local");
	assert_equal(true, calc_sync_action(false, true, 0, 10 * s, 0, 100, false, true, false, true) ==
	             sync_action::copy_local, "copy remote to local takes precedence over delete remote");
	assert_equal(true, calc_sync_action(true, true, 20 * s, 10 * s, 100, 100, true, true, true, true) ==
	             sync_action::copy_remote, "newer local file copies to remote");
	assert_equal(true, calc_sync_action(true, true, 10 * s, 20 * s, 100, 100, true, true, true, true) ==
	             sync_action::copy_local, "newer remote file copies to local");
	assert_equal(true, calc_sync_action(true, true, 10 * s, 10 * s, 100, 100, true, true, true, true) ==
	             sync_action::none, "matching timestamps need no action");
	assert_equal(true, calc_sync_action(true, true, 10 * s, 10 * s, 101, 100, true, false, false, false) ==
	             sync_action::copy_remote, "one-way sync copies unequal sizes with matching timestamps");
	assert_equal(true, calc_sync_action(true, true, 10 * s, 10 * s, 100, 101, false, true, false, false) ==
	             sync_action::copy_local, "reverse one-way sync copies unequal sizes with matching timestamps");

	// FAT32 rounds a copy's modified time to two seconds and exFAT to ten milliseconds, in either direction.
	assert_equal(true, calc_sync_action(true, true, 10 * s + s / 2, 10 * s, 100, 100, true, true, false, false) ==
	             sync_action::none, "a copy whose time the drive rounded down is not copied again");
	assert_equal(true, calc_sync_action(true, true, 10 * s, 10 * s + 2 * s, 100, 100, true, true, false, false) ==
	             sync_action::none, "a copy whose time the drive rounded up by two seconds is not copied back");
	assert_equal(true, calc_sync_action(true, true, 13 * s, 10 * s, 100, 100, true, true, false, false) ==
	             sync_action::copy_remote, "three seconds newer is a change");
	assert_equal(true, calc_sync_action(true, true, 10 * s + s, 10 * s, 101, 100, true, false, false, false) ==
	             sync_action::copy_remote, "a size change within the rounding still copies one way");
	assert_equal(true, calc_sync_action(true, true, 10 * s + s, 10 * s, 101, 100, true, true, false, false) ==
	             sync_action::none, "a size change within the rounding has no direction when both are enabled");
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
		tool->exe = str::cache(exe);
		tool->text = str::cache(exe);
		tool->group = str::cache(group);
		tool->invoke_text = str::cache("{exe-path} {item-path}");
		return tool;
	};

	const auto first = make_tool("first", "photo");
	const auto second = make_tool("second", "photo");
	const auto player = make_tool("player", "video");

	file_tools_result result;
	result.by_extension[str::cache("jpg")] = {first, second};
	result.by_group[str::cache("photo")] = {second};
	result.by_group[str::cache("video")] = {player};
	apply_tools(std::move(result));
	// The tool table is process-global; a failing assertion below must not leave it replaced.
	const df::scope_exit restore_tools([] { apply_tools({}); });

	const auto jpeg = files::file_type_from_name("test.jpg");
	const auto tools = jpeg->all_tools();
	assert_equal(2_z, tools.size(), "every tool declaring the extension is offered");
	assert_equal_strict("first", tools[0]->text.sv(), "config order is preserved");
	assert_equal_strict("second", tools[1]->text.sv(), "a tool matched by extension and group is listed once");

	const auto mp4 = files::file_type_from_name("test.mp4");
	assert_equal(1_z, mp4->all_tools().size(), "group membership offers a tool with no extension match");

	assert_equal(false, first->invoke(df::file_path{}), "an unresolved executable is never launched");

	apply_tools({});
	assert_equal(0_z, jpeg->all_tools().size(), "reapplying replaces rather than accumulates");
}

static void should_offer_configured_tools_only_for_one_selected_file()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto path1 = _temps.next_path(".jpg");
	const auto path2 = _temps.next_path(".jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), path1, false, false);
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), path2, false, false);

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
	const auto this_year = platform::now().year();
	const location_cache locations;
	index_histograms h;

	const auto record_created = [&](const int years_ago, const int month)
	{
		df::index_file_item f;
		f.ft = files::file_type_from_name("test.jpg");
		f.file_created = df::date_t(this_year - years_ago, month, 1);
		f.file_modified = df::date_t(this_year - years_ago, month, 1);
		h.record(locations, f);
	};

	record_created(0, 3); // this year, March
	record_created(9, 6); // within the old 10-year window
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
		for (auto y = 0; y < 15; ++y) years.emplace_back(y, 800);
		years.emplace_back(99, 4);

		const auto range = with(years);
		assert_equal(this_year - 14, range.first_year, "the range stops at the real history");
		assert_equal(this_year, range.last_year, "and ends today");
	}

	{
		// A camera whose clock reset: an island of a few dozen behind a decade of silence.
		std::vector<std::pair<int, int>> years;
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

	const auto assert_two_tags = [](const std::string_view text, const std::string_view first,
	                                const std::string_view second)
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
		{"quoted \"tag\"", true}
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
	const df::scope_exit restore([saved] { setting.detail_items = saved; });

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
		group_key_type::folder, group_key_type::photo, group_key_type::video, group_key_type::audio,
		group_key_type::grouped_value, group_key_type::grouped_no_value, group_key_type::archive,
		group_key_type::retro, group_key_type::other,
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
// keypad could not rate or label. keys::normalize_numpad maps the keypad digits onto
// the equivalent top-row digit while leaving every other key untouched.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_map_numpad_digits_to_rating_keys()
{
	// Every keypad digit VK_NUMPAD0..VK_NUMPAD9 maps to the matching top-row digit,
	// so the rating (0-5) and label (6-9) accelerators fire from the keypad.
	for (char32_t d = 0; d <= 9; ++d)
	{
		const auto numpad_key = keys::numpad0 + d;
		assert_equal(static_cast<char32_t>(U'0' + d), keys::normalize_numpad(numpad_key),
		             "keypad digit maps to top-row digit");
	}

	// Top-row digits are already correct and must pass through unchanged.
	for (char32_t c = U'0'; c <= U'9'; ++c)
	{
		assert_equal(c, keys::normalize_numpad(c), "top-row digit unchanged");
	}

	// Non-digit keys (letters, and the keypad codes just outside the digit range) are
	// left untouched so unrelated shortcuts are not disturbed.
	assert_equal(U'A', keys::normalize_numpad(U'A'), "letter unchanged");
	assert_equal(keys::numpad0 - 1, keys::normalize_numpad(keys::numpad0 - 1), "below range unchanged");
	assert_equal(keys::numpad9 + 1, keys::normalize_numpad(keys::numpad9 + 1), "above range unchanged");
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
	const auto saved_allowed = ui::yuv_textures_allowed;
	const auto saved_hook = ui::on_yuv_fault;
	const df::scope_exit restore([saved, saved_allowed, saved_hook]
	{
		setting.use_yuv = saved;
		ui::yuv_textures_allowed = saved_allowed;
		ui::on_yuv_fault = saved_hook;
	});

	ui::on_yuv_fault = nullptr;
	setting.use_yuv = false;
	connect_yuv_latch();
	assert_equal(false, ui::yuv_textures_allowed, "a driver that faulted in an earlier run keeps YUV off");
	assert_equal(true, ui::on_yuv_fault != nullptr, "and a fault this run reaches the setting");

	setting.use_yuv = true;
	connect_yuv_latch();
	assert_equal(true, ui::yuv_textures_allowed, "a driver that never faulted may sample YUV");
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
	const auto selected_a = df::file_path("c:\\one.jpg");
	const auto selected_b = df::file_path("c:\\two.jpg");

	history.history_add(search_a, {});
	history.history_add(search_b, df::paths{{selected_a}, {}});

	history_state::history_entry entry;
	assert_equal(true, history.move_history_pos(-1, df::paths{{selected_b}, {}}, entry), "browse back succeeds");
	assert_equal(true, entry.search == search_a, "back restores first search");
	assert_equal(true, entry.selected.files.size() == 1 && entry.selected.files.front() == selected_a,
	             "back restores first selection");
	history.history_add(entry.search, df::paths{{selected_b}, {}});

	assert_equal(true, history.move_history_pos(1, entry.selected, entry), "browse forward succeeds");
	assert_equal(true, entry.search == search_b, "forward restores second search");
	assert_equal(true, entry.selected.files.size() == 1 && entry.selected.files.front() == selected_b,
	             "forward restores second selection");
	history.history_add(entry.search, df::paths{{selected_a}, {}});

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

static void should_select_items()
{
	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;

	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, df::search_t().add_selector(test_files_folder), {});
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
	const df::file_path src_path1(test_files_folder, "Test.jpg");
	const df::file_path src_path2(test_files_folder, "Gherkin.CR2");

	// A private folder: opening the shared suite temp folder would scan every file every other
	// test has left there, making this test's cost and results depend on test order.
	const auto temp_folder = _temps.next_folder("rating");
	const auto save_path_1 = _temps.next_path_in(temp_folder, ".jpg");
	const auto save_path_2 = _temps.next_path_in(temp_folder, ".cr2");

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

static void assert_can_process(const view_state& s, const bool photos_only, const bool can_save_pixels,
                               const bool can_save_metadata,
                               const bool local_file, const bool local_file_or_folder, const std::string_view message)
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
	s.open(view, df::search_t().add_selector(test_files_folder), {});
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
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Rename execution
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_rename()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const df::file_path src_path(test_files_folder, "Test.jpg");
	const auto save_path_1 = _temps.next_path(".jpg");
	const auto save_path_2 = _temps.next_path(".jpg");

	platform::copy_file(src_path, save_path_1, false, false);

	const auto test_item = load_item(index, save_path_1, false);

	assert_equal(true, test_item->rename(index, save_path_2.file_name_without_extension()).success(), "can rename");
	assert_equal(save_path_2.name(), test_item->name(), "renamed");
	assert_equal(true, save_path_2.exists(), "renamed exists");
}

static void should_not_overwrite_during_rename()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const df::file_path src_path(test_files_folder, "Test.jpg");
	const auto save_path_1 = _temps.next_path(".jpg");
	const auto save_path_2 = _temps.next_path(".jpg");

	platform::copy_file(src_path, save_path_1, false, false);
	platform::copy_file(src_path, save_path_2, false, false);

	const auto test_item = load_item(index, save_path_1, false);

	assert_equal(false, test_item->rename(index, save_path_2.file_name_without_extension()).success(),
	             "should not rename");
	assert_equal(save_path_1.name(), test_item->name(), "not renamed");
	assert_equal(true, test_item->path().exists(), "exists");
}

static void should_rename_file_case()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto source = _temps.next_path(".jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);
	const auto item = load_item(index, source, false);
	auto upper_name = str::to_upper(std::string(source.file_name_without_extension()));
	if (upper_name == source.file_name_without_extension()) upper_name = str::to_lower(upper_name);
	const auto destination = df::file_path(source.folder(), upper_name, source.extension());

	assert_equal(true, item->rename(index, upper_name).success(), "case-only rename succeeds");
	assert_equal_strict(destination.name(), item->name(), "item keeps renamed case");
	assert_equal(true, destination.exists(), "case-only destination exists");
}

static void should_rollback_rename_when_sidecar_fails()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto primary = _temps.next_path(".jpg");
	const auto sidecar = primary.extension(".xmp");
	const auto target_base = std::string(primary.file_name_without_extension()) + "-renamed";
	const auto target_sidecar = df::file_path(primary.folder(), target_base, ".xmp");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), primary, false, false);
	platform::copy_file(df::file_path(test_files_folder, "IMG_0604.xmp"), sidecar, false, false);
	platform::copy_file(df::file_path(test_files_folder, "IMG_0604.xmp"), target_sidecar, false, false);

	df::index_file_item info;
	info.name = primary.name();
	info.ft = files::file_type_from_name(primary);
	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = sidecar.name();
	info.metadata.store(metadata);
	const auto item = std::make_shared<df::item_element>(primary, info);

	assert_equal(false, item->rename(index, target_base).success(), "sidecar collision fails rename");
	assert_equal(primary.name(), item->name(), "item keeps original name");
	assert_equal(true, primary.exists(), "primary remains at original path");
	assert_equal(true, sidecar.exists(), "sidecar remains at original path");
}

static void should_detect_original_path()
{
	const df::file_path path("c:\\temp\\test.original.jpg");
	assert_equal(true, path.is_original(), "detect original");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Collection membership and crash recording
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_toggle_collection_entry(shared_test_context& stc)
{
	const auto local_folders = platform::local_folders();

	settings_t::index_t settings;
	toggle_collection_entry(settings, local_folders.pictures, false);
	assert_equal(true, settings.pictures, "pictures add");

	toggle_collection_entry(settings, local_folders.pictures, true);
	toggle_collection_entry(settings, test_files_folder, false);
	assert_equal(false, settings.pictures, "pictures remove");
	assert_equal(test_files_folder.text(), settings.more_folders);

	toggle_collection_entry(settings, local_folders.video, false);
	toggle_collection_entry(settings, test_files_folder, true);
	assert_equal(true, settings.video, "pictures remove");
	assert_equal({}, settings.more_folders);
}

static void should_record_crashes()
{
	const auto db_path = _temps.next_path();
	const auto build = "test-build-1"sv;
	const auto paths = {
		test_files_folder.combine_file("Test.jpg"),
		test_files_folder.combine_file("Test90.jpg"),
		test_files_folder.combine_file("Small.jpg"),
		test_files_folder.combine_file("Lossless0.jpg")
	};

	for (auto path : paths)
	{
		{
			crash_files_db test_crash_files(db_path, build);

			assert_equal(test_crash_files.is_known_crash_file(path), false, "is_known_crash_file");

			const auto id = test_crash_files.add_open(path, str::utf8_cast(__FUNCTION__));
			test_crash_files.remove_open(id);
			test_crash_files.flush_open_files();
		}

		{
			crash_files_db test_crash_files(db_path, build);

			assert_equal(test_crash_files.is_known_crash_file(path), false, "is_known_crash_file");

			test_crash_files.add_open(path, str::utf8_cast(__FUNCTION__));
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

		for (auto path : paths) test_crash_files.add_open(path, str::utf8_cast(__FUNCTION__));
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
	const auto overlap_path = test_files_folder.combine_file("Test.jpg");

	for (const auto remove_first : {true, false})
	{
		const auto overlap_db_path = _temps.next_path();
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
			             remove_first ? "second overlapping registration survives" : "first overlapping registration survives");
			assert_equal(1_z, test_crash_files.skipped_file_count(), "overlap is persisted once by path");
		}
	}
}

static duplicate_report_test_photo duplicate_report_photo(const std::string_view name, const uint64_t created_key,
                                                          const uint64_t hash)
{
	duplicate_report_test_photo result;
	result.path = df::file_path(df::folder_path("c:\\duplicate-report"), name);
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
	const auto input = _temps.folder().combine("dup-report-empty");
	assert_equal(true, platform::create_folder(input).success(), "empty input folder is created");

	const auto missing_parent_output = input.combine("missing").combine_file("report.txt");
	assert_equal(1, run_duplicate_report(input.text(), missing_parent_output.str()),
	             "a requested report under a missing parent fails");
	assert_equal(false, missing_parent_output.exists(), "failed report output is not created");

	const auto directory_output = input.combine("report-directory");
	assert_equal(true, platform::create_folder(directory_output).success(), "directory output target is created");
	assert_equal(1, run_duplicate_report(input.text(), directory_output.text()),
	             "a requested report that names a directory fails");

	const auto relative_name = std::string(_temps.next_path(".txt").name());
	const auto relative_report = df::file_path(df::folder_path("."), relative_name);
	if (relative_report.exists()) platform::delete_file(relative_report);
	assert_equal(false, relative_report.exists(), "relative report starts absent");
	assert_equal(0, run_duplicate_report(input.text(), relative_name),
	             "a bare relative report name writes to the current folder");
	assert_equal(true, relative_report.exists(), "relative report output is created");
	platform::delete_file(relative_report);

	assert_equal(0, run_duplicate_report(input.text(), {}),
	             "stdout-only duplicate report remains successful");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The ping store keeps daily aggregate totals, so whatever the ingest decodes on the day is the
// only reading of that day that will ever exist: a field misread for a month is a month gone. The
// decoder therefore lives here beside the encoder, and this covers every field at its extremes.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_round_trip_the_environment_mask()
{
	const auto round_trip = [](const df::environment_facts& facts, const std::string_view what)
	{
		const auto packed = df::pack_environment(facts);
		const auto read_back = df::unpack_environment(packed);

		assert_equal(true, read_back.has_value(), std::format("{} decodes", what));
		assert_equal(true, read_back.value() == facts, std::format("{} reads back as itself", what));
	};

	// Nothing established: every field answers unknown, which is a reading rather than an absence.
	round_trip({}, "an unidentified machine");

	// Every field at the top of its range at once. This is what a field running out of room looks
	// like, and it has to survive being written down.
	round_trip({
		           df::os_family::other, df::os_release::other, df::machine_arch::other,
		           df::machine_arch::other, df::package_kind::other
	           }, "every field at its widest");

	// The cross-tabs the fields exist to answer, each one a real shape.
	round_trip({
		           df::os_family::windows, df::os_release::windows_11, df::machine_arch::x86,
		           df::machine_arch::x64, df::package_kind::installer
	           }, "a 32-bit process on 64-bit Windows 11");
	round_trip({
		           df::os_family::windows, df::os_release::windows_10, df::machine_arch::x64,
		           df::machine_arch::arm64, df::package_kind::microsoft_store
	           }, "an x64 build on arm64 hardware under translation");
	round_trip({
		           df::os_family::linux_, df::os_release::unknown, df::machine_arch::riscv64,
		           df::machine_arch::riscv64, df::package_kind::flatpak
	           }, "a Linux package");

	// Each field on its own, so a shift or a width that is wrong cannot hide behind a neighbour.
	for (const auto family : {df::os_family::unknown, df::os_family::windows, df::os_family::other})
		round_trip({.family = family}, "an OS family alone");
	for (const auto release : {df::os_release::unknown, df::os_release::windows_7, df::os_release::other})
		round_trip({.release = release}, "an OS version alone");
	for (const auto arch : {df::machine_arch::unknown, df::machine_arch::x86, df::machine_arch::other})
	{
		round_trip({.process = arch}, "a process architecture alone");
		round_trip({.machine = arch}, "an OS architecture alone");
	}
	for (const auto package : {df::package_kind::unknown, df::package_kind::appimage, df::package_kind::other})
		round_trip({.package = package}, "a package alone");

	// The layout version is read first and anything else fails closed. Guessing at an unrecognised
	// layout is the one mistake a daily aggregate cannot be talked out of.
	const auto packed = df::pack_environment({df::os_family::windows, df::os_release::windows_11});
	assert_equal(df::environment_layout_version, df::environment_layout::version.unpack(packed),
	             "the version is the first field");
	assert_equal(false, df::unpack_environment((packed & ~0xfull) | 0xf).has_value(),
	             "an unrecognised layout is left undecoded");

	// A reserved bit means the writer knew something this reader does not, so the same rule applies.
	assert_equal(0ull, packed & df::environment_layout::reserved, "nothing is written above the last field");
	assert_equal(false, df::unpack_environment(packed | (1ull << 27)).has_value(),
	             "a value using a reserved bit is left undecoded");

	// The value travels as hex, and the identity carries no measurement, so it stays small enough to
	// read at a glance rather than becoming an opaque 16 digits.
	assert_equal(true, str::to_hex(df::pack_environment({
		             df::os_family::windows, df::os_release::windows_11, df::machine_arch::x64,
		             df::machine_arch::x64, df::package_kind::installer
	             })).size() <= 8, "an ordinary machine packs into a short value");
}

// A view_state opened over the fixture collection, with no window. Every test below drives the same
// entry points the commands do, so what they pin is the behavior a user sees rather than a helper.
struct browsing_fixture
{
	null_state_strategy ss;
	null_async_strategy as;
	view_host_base_ptr view;
	location_cache locations;
	index_state index;
	view_state state;

	explicit browsing_fixture(const df::folder_path folder = test_files_folder)
		: index(as, locations), state(ss, as, index, make_test_player())
	{
		state.view_mode(view_type::items);
		state.open(view, df::search_t().add_selector(folder), {});
		state.update_item_groups();
		state.update_selection();
	}

	df::item_element_ptr find(const std::string_view name) const
	{
		return state.find_displayed_item_by_name(name);
	}
};

// The plan clears a destination held by a file the same run renames away, so the run has to free it
// rather than fail on it. Every row here wants the next row's current name.
static void should_run_a_rename_onto_vacated_names()
{
	const auto root = _temps.next_folder("rename-run-chain");
	std::map<std::string, std::string> before;
	for (const auto& name : {"Item 001.jpg"s, "Item 002.jpg"s, "Item 003.jpg"s})
	{
		const auto path = root.combine_file(name);
		platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), path, false, false);
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
	const df::scope_exit restore([saved] { setting.rename = saved; });
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
	const auto completion_status = std::string(view->status());
	view->refresh_from_source();
	assert_equal(completion_status, std::string(view->status()), "the file notification retains the real run summary");
	assert_equal(true, view->showing_results(), "completed rows survive their own file notifications");

	assert_equal(false, root.combine_file("Item 001.jpg").exists(), "the vacated name is gone");
	for (const auto& name : {"Item 002.jpg"s, "Item 003.jpg"s, "Item 004.jpg"s})
		assert_equal(true, root.combine_file(name).exists(), std::format("{} was written", name));
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
	for (const auto [block_reviewed, block_numbered, first_has_sidecar] :
	     {std::tuple{true, 0, true}, {true, 1, true}, {false, 0, true}, {true, 0, false}, {true, 2, true}})
	{
		const auto root = _temps.next_folder("rename-cancel-group");
		const auto first = root.combine_file("1.jpg");
		const auto second = root.combine_file("2.jpg");
		const auto third = root.combine_file("3.jpg");
		const auto source_bytes = read_test_file(test_files_folder.combine_file("Test.jpg"));
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
		assert_equal(true, str::contains(f.find("2.jpg")->sidecars(), "2.xmp"),
		             "the unfinished row carries its automatic companion");

		const auto saved = setting.rename;
		const df::scope_exit restore([saved] { setting.rename = saved; });
		setting.rename.name_template = "#";
		setting.rename.start_seq = "2";
		setting.rename.collision = collision_policy::replace;

		const auto view = std::make_shared<rename_view>(f.state, nullptr);
		view->activate({100, 100});
		assert_equal(true, view->can_run(), "the reviewed chain can run");
		df::rename_after_move_for_test = [first, second, root, block_numbered, &view](const df::file_path from,
		                                                                           const df::file_path to)
		{
			if (from == first && to == second) view->cancel_operation();
			if (block_numbered == 2 && to == root.combine_file("2 (2).jpg"))
				write_test_file(root.combine_file("2 (2).xmp"), "unrelated sidecar");
		};
		const df::scope_exit restore_hook([] { df::rename_after_move_for_test = {}; });
		view->run();

		const auto recovered = !block_reviewed ? third :
			root.combine_file(block_numbered ? "2 (3).jpg" : "2 (2).jpg");
		assert_equal(source_bytes + "first", read_test_file(second), "the completed rename stays completed");
		if (first_has_sidecar)
			assert_equal("first metadata"s, read_test_file(second.extension(".xmp")),
			             "the completed media keeps its own metadata");
		else
			assert_equal(false, second.extension(".xmp").exists(),
			             "an unparked companion must not stay beside the completed primary");
		assert_equal(source_bytes + "second", read_test_file(recovered), "the parked primary is recovered");
		assert_equal("second metadata"s, read_test_file(recovered.extension(".xmp")),
		             "its parked sidecar uses the same recovered basename");
		if (block_reviewed)
		{
			assert_equal(source_bytes + "unrelated", read_test_file(third), "the unrelated primary is untouched");
			assert_equal(false, third.extension(".xmp").exists(), "it acquires no recovered metadata");
		}
		if (block_numbered)
		{
			assert_equal("unrelated sidecar"s, read_test_file(root.combine_file("2 (2).xmp")),
			             "a numbered sidecar collision advances the whole group");
			assert_equal(false, root.combine_file("2 (2).jpg").exists(),
			             "a partially recovered primary is rolled back before trying the next basename");
		}
	}
}

// The plan records each file's identity (see should_record_rename_identity); this is the run holding
// itself to it. A source rewritten between the review and the run still exists, which is all the run
// used to ask, but it is not the file that was reviewed - so nothing is renamed.
static void should_refuse_a_rename_whose_source_changed_since_review()
{
	const auto root = _temps.next_folder("rename-identity-run");
	const auto source = root.combine_file("alpha.jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);

	browsing_fixture f(root);
	const auto item = f.find("alpha.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved = setting.rename;
	const df::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "renamed";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::block_run;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "the rename can run");

	write_test_file(source, "rewritten since the review");
	view->run();

	assert_equal(true, source.exists(), "the changed source keeps its name");
	assert_equal(false, root.combine_file("renamed.jpg").exists(), "and nothing was renamed");
	// A run that did nothing says why, rather than leaving the processing status behind.
	assert_equal(std::string(tt.sync_analysis_changed.sv()), std::string(view->status()),
	             "the status names the stale review");
}

// Releasing the displayed file's handles pumps messages, so a Close can be handled while Run is still
// setting out. Run then went ahead with nothing on screen to show it, renaming files after the user had
// left the view.
static void should_not_run_a_rename_closed_while_releasing_handles()
{
	const auto root = _temps.next_folder("rename-closed-during-detach");
	const auto source = root.combine_file("alpha.jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);

	browsing_fixture f(root);
	const auto item = f.find("alpha.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved = setting.rename;
	const df::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "renamed";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::block_run;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "the rename can run");

	const df::scope_exit clear_hook([] { test_during_detach_file_handles = {}; });
	test_during_detach_file_handles = [&view] { view->deactivate(); };
	view->run();

	assert_equal(true, source.exists(), "the source keeps its name");
	assert_equal(false, root.combine_file("renamed.jpg").exists(), "nothing is renamed once the view has closed");
	assert_equal(false, view->progress().active, "and no run is left showing progress");
}

static void should_keep_completed_run_results_on_source_refresh()
{
	browsing_fixture fixture;
	const std::vector<std::shared_ptr<list_view>> views{
		std::make_shared<rename_view>(fixture.state, nullptr),
		std::make_shared<batch_tool_view>(fixture.state, nullptr)
	};

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

// Every task view ends a run the same way: what it did, then why some rows did nothing. Rename used to
// drop the counts whenever there was a reason, and Tags kept "Processing" when there was neither.
static void should_conclude_a_run_with_what_it_did_and_why()
{
	assert_equal("2 renamed"s, list_view::conclude_run("2 renamed", {}, "review"), "the counts alone");
	assert_equal("stale"s, list_view::conclude_run({}, "stale", "review"), "the reason alone");
	assert_equal("2 renamed  stale"s, list_view::conclude_run("2 renamed", "stale", "review"),
	             "a partial run states both");
	assert_equal("review"s, list_view::conclude_run({}, {}, "review"), "nothing to report keeps the review");
}

// Convert under Replace wrote over a colliding destination whatever had happened to it since the
// review. Replace was agreed to for the file the review showed, and one edited since is not that
// file - the same test Import, Sync and Rename hold a replaced destination to.
static void should_not_convert_over_a_destination_changed_since_review()
{
	const auto root = _temps.next_folder("convert-replace-identity");
	const auto sources = root.combine("sources");
	const auto output = root.combine("output");
	platform::create_folder(sources);
	platform::create_folder(output);

	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), sources.combine_file("photo.jpg"), false, false);
	const auto destination = output.combine_file("photo.jpg");
	write_test_file(destination, "reviewed");

	browsing_fixture f(sources);
	const auto item = f.find("photo.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved_convert = setting.convert;
	const auto saved_folder = setting.write_folder;
	const df::scope_exit restore([saved_convert, saved_folder]
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

static void should_refuse_convert_destination_claimed_during_staged_publication()
{
	const auto root = _temps.next_folder("convert-publish-claimed");
	const auto source = root.combine_file("photo.jpg");
	const auto destination = root.combine_file("converted.jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);

	test_before_file_update_publish = [destination](const df::file_path path)
	{
		if (path == destination) write_test_file(destination, "claimed during publication");
	};
	const df::scope_exit restore_hook([] { test_before_file_update_publish = {}; });

	files ff;
	file_encode_params params;
	params.jpeg_save_quality = 90;
	file_update_publish publish;
	publish.fail_if_exists = true;
	const auto result = ff.update(source, destination, {}, image_edits(16), params, false, {}, {}, {}, publish);

	assert_equal(true, result.code == platform::file_op_result_code::ALREADY_EXISTS,
	             "the final publication refuses a claimed destination");
	assert_equal("claimed during publication"s, read_test_file(destination), "the claimed file survives");
	assert_equal(true, source.exists(), "the source survives the refused convert");
}

static void should_refuse_convert_replacement_changed_during_staged_publication()
{
	const auto root = _temps.next_folder("convert-publish-replace-changed");
	const auto source = root.combine_file("photo.jpg");
	const auto destination = root.combine_file("converted.jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);
	write_test_file(destination, "reviewed destination");
	const auto reviewed = platform::file_attributes(destination);

	test_before_file_update_publish = [destination](const df::file_path path)
	{
		if (path == destination) write_test_file(destination, "changed during publication");
	};
	const df::scope_exit restore_hook([] { test_before_file_update_publish = {}; });

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
	assert_equal(true, source.exists(), "the source survives the refused replacement");
}

static void should_refuse_convert_replacement_locked_during_staged_publication()
{
	const auto root = _temps.next_folder("convert-publish-replace-locked");
	const auto source = root.combine_file("photo.jpg");
	const auto destination = root.combine_file("converted.jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);
	write_test_file(destination, "reviewed destination");
	const auto reviewed = platform::file_attributes(destination);

	platform::file_ptr locked;
	test_before_file_update_publish = [destination, &locked](const df::file_path path)
	{
		if (path == destination) locked = platform::open_file(destination, platform::file_open_mode::read_write);
	};
	const df::scope_exit restore_hook([&locked]
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

#ifdef _WIN32
// A rename is a move, so once a sidecar has landed on a Replace destination that destination's
// former content is gone whether or not the row completes. Leaving the sidecar there after the
// primary fails strands it away from the file it describes and empties the source of a group the
// run promised to leave whole; putting it back destroys nothing further. The failure is staged by
// holding the destination open, which denies the move only where locking is mandatory.
static void should_restore_a_replaced_sidecar_when_the_rename_fails()
{
	const auto root = _temps.next_folder("rename-replace-rollback");
	const auto source = root.combine_file("a.jpg");
	const auto source_sidecar = root.combine_file("a.xmp");
	const auto destination = root.combine_file("b.jpg");
	const auto destination_sidecar = root.combine_file("b.xmp");

	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);
	write_test_file(source_sidecar, "source sidecar");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), destination, false, false);
	write_test_file(destination_sidecar, "destination sidecar");

	browsing_fixture f(root);
	const auto item = f.find("a.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();
	assert_equal(1_z, f.state.selected_count(), "only the source is selected");

	const auto saved = setting.rename;
	const df::scope_exit restore([saved] { setting.rename = saved; });
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

	assert_equal(true, source.exists(), "the primary is left at its source");
	assert_equal(true, source_sidecar.exists(), "and so is the sidecar that had already moved");
	assert_equal("source sidecar"s, read_test_file(source_sidecar),
	             "the recovered source sidecar keeps its own bytes");
}
#endif

static void should_report_batch_rename_sidecar_rollback_survivor()
{
	const auto root = _temps.next_folder("rename-sidecar-rollback-survivor");
	const auto source = root.combine_file("a.jpg");
	const auto source_sidecar = root.combine_file("a.xmp");
	const auto destination = root.combine_file("b.jpg");
	const auto destination_sidecar = root.combine_file("b.xmp");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);
	write_test_file(source_sidecar, "source sidecar");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), destination, false, false);
	write_test_file(destination_sidecar, "destination sidecar");

	df::rename_after_move_for_test = [source_sidecar, destination_sidecar](const df::file_path moved_from,
	                                                                       const df::file_path moved_to)
	{
		if (moved_from == source_sidecar && moved_to == destination_sidecar)
		{
			write_test_file(source_sidecar, "rollback blocker");
		}
	};
	const df::scope_exit restore_hook([] { df::rename_after_move_for_test = {}; });

	browsing_fixture f(root);
	const auto item = f.find("a.jpg");
	assert_equal(true, item != nullptr, "the source is listed");
	f.state.select(f.view, item, false, false, false);
	f.state.update_selection();

	const auto saved = setting.rename;
	const df::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "b";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::replace;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	auto locked = platform::open_file(destination, platform::file_open_mode::read_write);
	assert_equal(true, locked != nullptr, "the destination is held open");
	view->run();
	locked.reset();

	assert_equal(true, destination_sidecar.exists(), "the moved sidecar remains at its surviving path");
	assert_equal(true, str::contains(view->status(), destination_sidecar.str()), "the status names the sidecar survivor");
}

static void should_report_batch_rename_case_only_recovery_failure()
{
	const auto root = _temps.next_folder("rename-case-recovery-fail");
	const auto source = root.combine_file("photo.jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);

	df::file_path surviving;
	df::rename_after_move_for_test = [source, &surviving](const df::file_path moved_from,
	                                                      const df::file_path moved_to)
	{
		if (moved_from == source && moved_to != source)
		{
			surviving = moved_to;
			write_test_file(source, "rollback blocker");
		}
	};
	const df::scope_exit restore_hook([] { df::rename_after_move_for_test = {}; });

	browsing_fixture f(root);
	f.state.select_all(f.view);
	f.state.update_selection();

	const auto saved = setting.rename;
	const df::scope_exit restore([saved] { setting.rename = saved; });
	setting.rename.name_template = "PHOTO";
	setting.rename.start_seq = "1";
	setting.rename.collision = collision_policy::block_run;

	const auto view = std::make_shared<rename_view>(f.state, nullptr);
	view->activate({100, 100});
	assert_equal(true, view->can_run(), "a case-only rename can run");
	view->run();

	assert_equal(true, surviving.exists(), "the temporary surviving path is kept");
	assert_equal("rollback blocker"s, read_test_file(source), "the blocker remains at the reviewed path");
	assert_equal(true, str::contains(view->status(), surviving.str()), "the status names the surviving path");
}

static void should_report_singleton_rename_case_only_rollback_failure()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto source = _temps.next_path(".jpg");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), source, false, false);
	const auto item = load_item(index, source, false);
	auto upper_name = str::to_upper(std::string(source.file_name_without_extension()));
	if (upper_name == source.file_name_without_extension()) upper_name = str::to_lower(upper_name);

	df::file_path surviving;
	df::rename_after_move_for_test = [source, &surviving](const df::file_path moved_from,
	                                                      const df::file_path moved_to)
	{
		if (moved_from == source && moved_to != source)
		{
			surviving = moved_to;
			write_test_file(source, "singleton rollback blocker");
		}
	};
	const df::scope_exit restore_hook([] { df::rename_after_move_for_test = {}; });

	const auto result = item->rename(index, upper_name);
	assert_equal(true, result.failed(), "the blocked rollback reports failure");
	assert_equal(true, surviving.exists(), "the temporary surviving path is kept");
	assert_equal("singleton rollback blocker"s, read_test_file(source), "the blocker remains at the reviewed path");
	assert_equal(true, str::contains(result.format_error(), surviving.str()), "the error names the surviving path");
}

static void should_continue_singleton_rename_sidecar_rollbacks_after_failure()
{
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	const auto primary = _temps.next_path(".jpg");
	const auto sidecar_xmp = primary.extension(".xmp");
	const auto sidecar_dop = primary.extension(".dop");
	const auto sidecar_on1 = primary.extension(".on1");
	const auto target_base = std::string(primary.file_name_without_extension()) + "-renamed";
	const auto target_primary = df::file_path(primary.folder(), target_base, ".jpg");
	const auto target_xmp = df::file_path(primary.folder(), target_base, ".xmp");
	const auto target_dop = df::file_path(primary.folder(), target_base, ".dop");
	const auto target_on1 = df::file_path(primary.folder(), target_base, ".on1");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), primary, false, false);
	write_test_file(sidecar_xmp, "xmp");
	write_test_file(sidecar_dop, "dop");
	write_test_file(sidecar_on1, "on1");
	write_test_file(target_primary, "blocked primary");

	std::vector<std::string> reports;
	df::rename_after_move_for_test = [sidecar_dop, target_dop, sidecar_on1, target_on1](
		const df::file_path moved_from, const df::file_path moved_to)
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
	const df::scope_exit restore_hook([]
	{
		df::rename_after_move_for_test = {};
		df::rename_recovery_report_for_test = {};
	});

	df::index_file_item info;
	info.name = primary.name();
	info.ft = files::file_type_from_name(primary);
	const auto metadata = std::make_shared<prop::item_metadata>();
	metadata->sidecars = str::cache(std::format("{};{};{}", sidecar_xmp.name(), sidecar_dop.name(), sidecar_on1.name()));
	info.metadata.store(metadata);
	const auto item = std::make_shared<df::item_element>(primary, info);

	const auto result = item->rename(index, target_base);
	assert_equal(true, result.failed(), "the rename reports the rollback failure");
	assert_equal(true, str::contains(result.format_error(), target_on1.str()), "the error names the first survivor");
	assert_equal(true, target_dop.exists(), "the second failed rollback sidecar remains recoverable");
	assert_equal(true, target_on1.exists(), "the first failed rollback sidecar remains recoverable");
	assert_equal(true, std::ranges::any_of(reports, [&](const std::string& message)
	{
		return str::contains(message, target_dop.str());
	}), "the second survivor is logged");
	assert_equal(true, std::ranges::any_of(reports, [&](const std::string& message)
	{
		return str::contains(message, target_on1.str());
	}), "the first survivor is logged");
	assert_equal(false, target_xmp.exists(), "the successfully rolled-back sidecar leaves no destination copy");
	assert_equal(true, target_dop.exists(), "the failed rollback sidecar remains recoverable");
}

// A case-only rename frees nothing, so the run must not park the file it is renaming within.
static void should_run_a_case_only_rename()
{
	const auto root = _temps.next_folder("rename-run-case");
	platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), root.combine_file("photo.jpg"), false, false);

	browsing_fixture f(root);
	f.state.select_all(f.view);
	f.state.update_selection();

	const auto saved = setting.rename;
	const df::scope_exit restore([saved] { setting.rename = saved; });
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
	const auto root = _temps.next_folder("rename-single-folder-case");
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
	assert_equal(true, root.combine("photos").exists(), "the folder has the requested case");
	assert_equal("child"s, read_test_file(root.combine("photos").combine_file("child.txt")),
	             "children remain under the renamed folder");
	assert_equal("photos", std::string(item->base_name()), "the item name is updated after success");

	result = item->rename(f.index, "photos");
	assert_equal(true, result.success(), "a byte-identical folder rename is still a no-op");

	result = item->rename(f.index, "Taken");
	assert_equal(true, result.failed(), "an occupied distinct destination is still refused");
	assert_equal(true, root.combine("photos").exists(), "the source folder survives the refused rename");
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
		for (const auto& i : g->items())
			listed.emplace_back(i);

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
	assert_equal(true, after_last == listed[listed.size() - 2],
	             "a set running to the end settles on what preceded it");

	// Everything selected: nothing survives it, and the caller has to cope with that rather than
	// being handed an item that is about to disappear.
	f.state.select_all(f.view);
	f.state.update_selection();
	assert_equal(true, f.state.next_unselected_item() == nullptr, "a whole listing leaving has no successor");
}

// Delete re-opens the listing naming the item to land on, and validating the deleted files refreshes
// the same listing straight away - an open of the same search with nothing to select. The refresh
// supersedes the first pass before it publishes, and a superseded pass is dropped, so unless the
// refresh takes over what that pass was opened for the cursor lands nowhere at all.
static void should_keep_what_a_superseded_open_asked_for()
{
	const auto root = _temps.next_folder("superseded-open");
	for (const auto& name : {"a.jpg"s, "b.jpg"s, "c.jpg"s})
		platform::copy_file(df::file_path(test_files_folder, "Test.jpg"), root.combine_file(name), false, false);

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
	s.open(view, search, df::unique_paths{landing});
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
	browsing_fixture empty(_temps.next_folder("no-media"));
	assert_equal(false, empty.state.can_slideshow(), "an empty scope cannot run a slideshow");
}

// design.md: "Next and Previous folder move between siblings of the current folder. At the first or
// last sibling they do nothing and are unavailable; they never change level."
static void should_move_between_sibling_folders()
{
	const auto root = _temps.next_folder("siblings");
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
	const df::scope_exit restore([saved] { command_line.no_indexing = saved; });
	const auto app = std::make_shared<app_frame>(nullptr);

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
	const auto app = std::make_shared<app_frame>(nullptr);
	app->initialise_commands();
	assert_equal(false, app->_commands.empty(), "the command table is populated");

	std::vector<std::string> unbound;
	for (const auto& [id, command] : app->_commands)
	{
		if (!command->invoke && !command->menu) unbound.emplace_back(command->text);
	}
	std::ranges::sort(unbound);
	assert_equal(std::string{}, str::combine(unbound, ", "), "commands without an action or submenu");
}

static void should_invoke_link_commands_and_selection_shortcuts()
{
	const auto saved_favorite_tags_only = setting.sidebar.show_favorite_tags_only;
	const df::scope_exit restore([saved_favorite_tags_only]
	{
		setting.sidebar.show_favorite_tags_only = saved_favorite_tags_only;
	});
	const auto app = std::make_shared<app_frame>(nullptr);
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
		indexed.name = str::cache(name);
		indexed.ft = files::file_type_from_name(name);
		const auto item = std::make_shared<df::item_element>(df::file_path(df::folder_path("c:\\commands"), name), indexed);
		item->add_to(items);
	}
	app->_state.append_items(app->_view_frame, items, {}, true, true);
	click_link(commands::filter_videos, tt.command_filter_videos);
	assert_equal(true, app->_state.filter().has_group(file_group::video), "the video link enables the filter");
	app->_state.update_item_groups();

	ui::key_state control;
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

static void should_route_focused_view_keyboard_commands()
{
	struct keyboard_view final : view_base
	{
		int key_count = 0;
		bool editing_text = false;

		void activate(sizei) override {}
		void deactivate() override {}
		void refresh() override {}

		bool key_down(const char32_t key, const ui::key_state keys) override
		{
			++key_count;
			return key == 'Z' && keys.control;
		}

		ui::focus_mode focus_mode() const override
		{
			return editing_text ? ui::focus_mode::text_edit : ui::focus_mode::view;
		}
	};

	const auto app = std::make_shared<app_frame>(nullptr);
	app->initialise_commands();
	const auto view = std::make_shared<keyboard_view>();
	app->_view = view;
	app->_view_has_focus = true;
	ui::key_state control;
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
		{
			return a.id == id && a.key.key == key && a.key.key_state == state;
		});
	};

	assert_equal(true, bound(commands::refresh, keys::F5, 0), "F5 refreshes");
	assert_equal(true, bound(commands::view_fullscreen, keys::F11, 0), "F11 is fullscreen");
	assert_equal(true, bound(commands::select_all, 'A', keyboard_accelerator_t::control), "Ctrl+A selects all");
	assert_equal(true, bound(commands::tool_delete, keys::DEL, 0), "Delete deletes");
	assert_equal(true, bound(commands::view_close, keys::ESCAPE, 0), "Escape closes");
}

// The keyboard reference and every command tooltip spell a binding through this one function, so a
// key left without a name, or modifiers in another order, would show in both.
static void should_spell_keyboard_accelerators()
{
	constexpr auto alt = keyboard_accelerator_t::alt;
	constexpr auto control = keyboard_accelerator_t::control;
	constexpr auto shift = keyboard_accelerator_t::shift;

	assert_equal(std::string(tt.keyboard_f5.sv()), format_keyboard_accelerator({{keys::F5, 0}}), "a named key");
	assert_equal(std::string(tt.keyboard_del.sv()), std::string(keys::format(keys::DEL)), "Delete has a name");
	assert_equal(std::format("{}+{}+A", tt.keyboard_alt, tt.keyboard_shift),
	             format_keyboard_accelerator({{U'A', alt | shift}}), "modifiers lead in a fixed order");
	assert_equal(std::format("{}+'{}'", tt.keyboard_control, tt.keyboard_oem_plus),
	             format_keyboard_accelerator({{keys::OEM_PLUS, control}}),
	             "plus is quoted so it is not read as the separator");
	assert_equal(std::format("{} {} {}", tt.keyboard_left, tt.keyboard_or, tt.keyboard_right),
	             format_keyboard_accelerator({{keys::LEFT, 0}, {keys::RIGHT, 0}}), "alternatives are joined");
}

// Feature use records one bit per view, so a view added without a bit is never counted as opened
// - which is how opening the Movie view went unrecorded.
static void should_record_a_bit_for_every_view()
{
	assert_equal(0_z, static_cast<size_t>(features::view_bit(view_type::none)), "no view records nothing");

	uint64_t seen = 0;

	for (auto v = static_cast<int>(view_type::items); v <= static_cast<int>(view_type::tags); ++v)
	{
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

	summary.record(files::file_type_from_name("Test.jpg"), df::file_size(4096));

	const auto says = [](const std::string& text, const std::string_view part)
	{
		return text.find(part) != std::string::npos;
	};

	// Nothing indexed yet reads as loading rather than as an empty collection.
	assert_equal(std::string(tt.loading), format_items_summary(group_by::file_type, sort_by::name, empty_summary, false),
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

// Hands out real command objects so the menu built from them can be compared by identity. A menu
// that invented its own copies would look right and drift the moment either changed.
class command_state_strategy final : public null_state_strategy
{
public:
	mutable df::hash_map<commands, ui::command_ptr> commands_by_id;

	ui::command_ptr find_command(const commands cmd) const override
	{
		auto& found = commands_by_id[cmd];
		if (!found) found = std::make_shared<ui::command>();
		return found;
	}
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// Discussion #251 - the filter toolbar scrolls away, so changing a filter over a long listing meant
// scrolling to the top first. The control in the scroller track is always on screen and now carries
// those settings. Its click opens the menu in every scroll position: opening a menu when at the top
// and scrolling when not would be a context-dependent surprise.
///////////////////////////////////////////////////////////////////////////////////////////////////
static void should_offer_the_items_menu_at_every_scroll_position()
{
	command_state_strategy ss;
	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());

	// The toolbar's grouping button carries the Group by / Sort by list, so the menu must take it
	// from there rather than list the same commands again.
	const auto group = ss.find_command(commands::menu_group_toolbar);
	group->menu = [&ss] { return std::vector{ss.find_command(commands::group_folder)}; };

	auto scrolled = 0;
	const auto scroll = [&scrolled] { ++scrolled; };

	for (const auto at_top : {true, false})
	{
		const auto menu = items_scroll_menu(state, at_top, scroll);

		assert_equal(true, menu.size() > 1, "the menu is offered whatever the scroll position");
		assert_equal(true, menu.back() != nullptr, "and it closes with an entry of its own");
		assert_equal(std::string(tt.tooltip_scroll_to_top.sv()), std::string(menu.back()->text),
		             "scroll to top comes last, below the settings the menu exists for");
		assert_equal(true, menu[menu.size() - 2] != nullptr, "directly below them, with no separator between");
		assert_equal(!at_top, menu.back()->enable, "and it says whether it has anything to do");

		const auto offers = [&menu](const ui::command_ptr& c)
		{
			return std::ranges::find(menu, c) != menu.end();
		};

		assert_equal(true, offers(ss.find_command(commands::group_folder)), "the toolbar's grouping list is carried");
		assert_equal(true, offers(ss.find_command(commands::filter_photos)), "so is the media type filter");
		assert_equal(true, offers(ss.find_command(commands::filter_videos)), "every part of it");
		assert_equal(true, offers(ss.find_command(commands::filter_audio)), "every part of it");
		assert_equal(true, offers(ss.find_command(commands::browse_recursive)),
		             "and Show items in subfolders, which was on no toolbar at all");
	}

	// Invoking the last entry is what scrolls. A menu entry performs its command whether or not it
	// is enabled - dimming is what the user is shown, not a second gate - so the scroll runs here
	// even though this menu was built at the top.
	const auto at_top_menu = items_scroll_menu(state, true, scroll);
	at_top_menu.back()->invoke();
	assert_equal(1, scrolled, "the entry performs the scroll the click used to");

	// A separator is only ever between two entries: a double rule, or a rule under the last entry,
	// is what an unresolved command silently turns into.
	assert_equal(false, at_top_menu.front() == nullptr, "the menu does not open on a separator");
	assert_equal(false, at_top_menu.back() == nullptr, "and does not end on one");

	for (auto i = 1u; i < at_top_menu.size(); ++i)
	{
		assert_equal(false, at_top_menu[i] == nullptr && at_top_menu[i - 1] == nullptr,
		             "no two separators are adjacent");
	}
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
	assert_equal("bea", session.commit(false, {}, "bea"), "enter commits the visible address when nothing is highlighted");

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

	// A command is running, or the view cannot play: the sequence ends rather than fighting it.
	{
		auto t = photo_in_slideshow();
		t.photo_delay_elapsed = true;
		t.can_next = false;
		t.repeat = repeat_mode::repeat_one;
		assert_equal(true, calc_playback_advance(t) == playback_advance::stop,
		             "a sequence that cannot advance stops, even under repeat one");
	}
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
// search_auto_complete::search and had no coverage: it is reached only through a native edit
// control, so the strategy is built here through its factory instead.
static void should_rank_address_completions(shared_test_context& stc)
{
	stc.lazy_load_index();
	stc.test_index.update_summary();

	null_state_strategy ss;
	deferred_async_strategy as;
	view_state s(ss, as, stc.test_index, make_test_player());
	s.view_mode(view_type::items);

	const auto completes = make_search_auto_complete(s, [](std::string) {});
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
	df::hash_set<std::string, df::ihash, df::ieq> seen;

	for (const auto& r : results)
	{
		assert_equal(true, seen.emplace(r->edit_text()).second,
		             std::format("'{}' is offered once", r->edit_text()));
		assert_equal(true, !r->edit_text().empty(), "a completion is never blank");
	}

	assert_equal(true, results.size() <= completes->max_predictions, "the list is capped");

	// A scoped token is answered from its own vocabulary. The indexed fixture carries
	// #key1/#key2/#key3, so this cannot pass vacuously.
	const auto tag_suggested = std::ranges::any_of(results, [](const ui::auto_complete_match_ptr& r)
	{
		return str::icmp(r->edit_text(), "#key1") == 0;
	});
	assert_equal(true, tag_suggested, "an indexed tag is offered for a tag prefix");

	// That tag must also outrank the generic matches below it, or it is offered but unreachable.
	const auto tag_pos = std::ranges::find_if(results, [](const ui::auto_complete_match_ptr& r)
	{
		return str::icmp(r->edit_text(), "#key1") == 0;
	});
	assert_equal(true, tag_pos != results.end() && tag_pos - results.begin() <= 3,
	             "a scoped suggestion is ranked near the top");

	// A term the user has typed before AND that the index knows arrives from several sources at
	// once, so this is where a missing de-duplication pass shows up as a repeated row.
	s.recent_tags.add("key1");
	completes->initialise([](const ui::auto_complete_results&) {});
	run("#key1");

	assert_equal(true, !results.empty(), "a known tag produces completions");

	df::hash_set<std::string, df::ihash, df::ieq> seen_known;

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
	s.recent_searches.add(std::string(test_files_folder.text()));
	const auto folder_query = std::string(test_files_folder.text()) + " Test";
	s.recent_searches.add(folder_query);

	const auto completes = make_search_auto_complete(s, [](std::string) {});
	completes->initialise([](const ui::auto_complete_results&) {});

	ui::auto_complete_results results;
	completes->search({}, [&results](const ui::auto_complete_results& r) { results = r; });
	run_completion_worker(as);
	as.drain_ui();

	const auto offered = [&results](const std::string_view text)
	{
		return std::ranges::any_of(results, [text](const ui::auto_complete_match_ptr& r)
		{
			return str::icmp(r->edit_text(), text) == 0;
		});
	};

	assert_equal(true, offered("@photo beach"), "a recent query is offered");
	assert_equal(true, offered(test_files_folder.text()), "a recent folder is offered");
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

	const auto completes = make_search_auto_complete(s, [](std::string) {});
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

static void should_gate_application_alpha_effects()
{
	const auto restore_animations = ui::animations_enabled;
	const auto restore_step = ui::animation_step_factor;
	const df::scope_exit restore_scope([restore_animations, restore_step]
	{
		ui::animations_enabled = restore_animations;
		ui::animation_step_factor = restore_step;
	});

	ui::animations_enabled = false;
	assert_equal(0.65f, command_progress_alpha(true, 0, 0), "disabled indeterminate progress is stable");
	assert_equal(0.65f, command_progress_alpha(true, 0, 600), "disabled progress ignores time");
	assert_equal(false, command_progress_needs_animation(true, 0), "disabled progress requests no cadence");

	auto logo = ui::color(0.2f, 0.3f, 0.4f, 0.8f);
	const auto logo_target = ui::color(0.2f, 0.3f, 0.4f, 0.0f);
	assert_equal(true, ui::step_color_alpha(logo, logo_target), "disabled logo step changes once");
	assert_equal(0.0f, logo.a, "disabled logo step snaps to target alpha");

	ui::animations_enabled = true;
	ui::animation_step_factor = 0.5f;
	assert_equal(true, command_progress_needs_animation(true, 0), "enabled progress requests cadence");
	assert_equal(true, command_progress_alpha(true, 0, 0) != command_progress_alpha(true, 0, 300),
	             "enabled progress still pulses");

	logo = ui::color(0.2f, 0.3f, 0.4f, 0.8f);
	assert_equal(true, ui::step_color_alpha(logo, logo_target), "enabled logo step changes");
	assert_equal(0.4f, logo.a, "enabled logo step advances by the frame factor");
}

static void should_snap_app_logo_hover_when_animations_are_disabled()
{
	const auto restore_animations = ui::animations_enabled;
	const df::scope_exit restore_scope([restore_animations] { ui::animations_enabled = restore_animations; });

	browsing_fixture f;
	app_logo_element logo(f.state);

	ui::animations_enabled = false;
	assert_equal(true, logo.hover(true), "hover enters");
	assert_equal(true, logo.step_background_if_animations_disabled(), "disabled hover snaps background");
	assert_equal(true, logo._bg_color.a > 0.0f, "hover highlight is visible immediately");

	assert_equal(true, logo.hover(false), "hover exits");
	assert_equal(true, logo.step_background_if_animations_disabled(), "disabled leave snaps background");
	assert_equal(0.0f, logo._bg_color.a, "hover highlight is hidden immediately");
}

static void should_skip_stable_progress_repaints_when_animations_are_disabled()
{
	const auto restore_animations = ui::animations_enabled;
	const df::scope_exit restore_scope([restore_animations] { ui::animations_enabled = restore_animations; });

	ui::animations_enabled = false;
	assert_equal(false, status_needs_animation(true, 0, false), "stable disabled progress does not animate status");
	assert_equal(true, status_needs_animation(true, 0, true), "debug status keeps its live repaint");

	ui::animations_enabled = true;
	assert_equal(true, status_needs_animation(true, 0, false), "enabled indeterminate progress still animates");
}

static void run_destination_completion_worker(deferred_async_strategy& as)
{
	auto ran = false;
	std::thread worker([&as, &ran] { ran = as.run_next(async_queue::work); });
	worker.join();
	assert_equal(true, ran, "a destination completion pass was queued");
}

static std::shared_ptr<ui::complete_strategy_t> make_test_destination_completer(
	view_state& s, const std::shared_ptr<void>& lifetime, const std::shared_ptr<const df::folder_counts>& folders,
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
	auto folders = std::make_shared<df::folder_counts>();
	(*folders)[df::folder_path("C:\\Beta")] = 100;
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

static void should_rank_destination_completions_after_combining_sources()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	auto lifetime = std::make_shared<int>(1);
	auto folders = std::make_shared<df::folder_counts>();
	const auto parent = df::folder_path(df::windows_path_semantics ? "D:\\Photos" : "/photos");
	const auto holiday = parent.combine("Holidays 2019");
	(*folders)[holiday] = 232;

	for (char suffix = 'A'; suffix <= 'L'; ++suffix)
	{
		(*folders)[parent.combine("2019").combine(std::string_view(&suffix, 1))] = 240;
	}

	const auto completes = make_test_destination_completer(s, lifetime, folders, [holiday](const df::folder_path)
	{
		return df::folder_paths{holiday};
	});

	ui::auto_complete_results results;
	completes->search(std::format("{} 2019", parent.text()),
	                  [&results](const ui::auto_complete_results& r) { results = r; });
	run_destination_completion_worker(as);
	as.drain_ui();

	assert_equal(true, results.size() <= completes->max_predictions, "destination completions remain capped");
	const auto holiday_text = std::string(holiday.text()); // APP-003: destination completions are raw paths
	assert_equal(true, std::ranges::any_of(results, [holiday_text](const ui::auto_complete_match_ptr& r)
	{
		return str::icmp(r->edit_text(), holiday_text) == 0;
	}), "a disk child that is also known combines both weights before the cap");
}

static void should_skip_stale_destination_completion_work()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	auto lifetime = std::make_shared<int>(1);
	auto folders = std::make_shared<df::folder_counts>();
	auto enumerations = 0;
	const auto completes = make_test_destination_completer(s, lifetime, folders,
	                                                       [&enumerations](const df::folder_path)
	                                                       {
		                                                       ++enumerations;
		                                                       return df::folder_paths{};
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
		~release_probe()
		{
			released_on_ui = ui::is_ui_thread();
		}
	};

	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	auto lifetime = std::make_shared<int>(1);
	auto folders = std::make_shared<df::folder_counts>();
	const auto completes = make_test_destination_completer(s, lifetime, folders,
	                                                       [](const df::folder_path)
	                                                       {
		                                                       return df::folder_paths{};
	                                                       });

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
			released_on_ui = ui::is_ui_thread();
		}
	};

	deferred_async_strategy as;
	bool released = false;
	bool released_on_ui = false;
	auto owned = ui_owned(as, std::make_shared<release_probe>(released, released_on_ui));

	// Moved in and dropped there, so the worker holds the only reference when it lets go.
	std::thread worker([held = std::move(owned)]() mutable
	{
		const auto last = std::move(held);
	});
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

	const auto restore = df::scope_exit([] { setting.search = {}; setting.sidebar = {}; setting.favorite_tags.clear(); });

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

	assert_equal(true, std::ranges::any_of(items, [](const search_item_ptr& item)
	{
		return item->key == "s:tag:snapshot";
	}), "sidebar workers use the captured search settings");
	assert_equal(false, std::ranges::any_of(items, [](const search_item_ptr& item)
	{
		return item->key == "s:tag:live";
	}), "sidebar workers do not read replaced live search settings");

	assert_equal(false, should_publish_sidebar_structural_request(2, 1),
	             "an obsolete sidebar structural packet cannot publish");
	assert_equal(true, should_publish_sidebar_structural_request(2, 2),
	             "the current sidebar structural packet can publish");
}

static void should_consume_cancelled_globe_release()
{
	assert_equal(true, should_invoke_globe_release(false, false, true), "a normal click invokes");
	assert_equal(false, should_invoke_globe_release(true, false, true),
	             "release after an Escape-cancelled drag is consumed");
	assert_equal(false, should_invoke_globe_release(false, true, true), "an uncancelled drag is not a click");
	assert_equal(false, should_invoke_globe_release(false, false, false), "release outside the globe does not invoke");
}

static void should_offer_only_native_movie_project_dialogs()
{
	const auto filters = movie_project_filters();
	assert_equal(1_z, filters.size(), "Open and Save offer one native project format");
	assert_equal("*.otio"s, filters.front().extensions, "Movie Maker projects are not an Open or Save choice");
	assert_equal(std::string(tt.movie_project_files.sv()), filters.front().text, "the native filter is localized");
}

static void should_skip_existing_movie_batch_outputs()
{
	constexpr auto missing_media_project = R"({
		"OTIO_SCHEMA": "Timeline.1",
		"tracks": {"OTIO_SCHEMA": "Stack.1", "children": [{
			"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [{
				"OTIO_SCHEMA": "Clip.1",
				"source_range": {
					"start_time": {"value": 0, "rate": 30},
					"duration": {"value": 30, "rate": 30}
				},
				"media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "target_url": "missing.mp4"}
			}]
		}]}
	})";
	for (const auto import : {false, true})
		for (const auto separate_output : {false, true})
		{
			const auto root = _temps.next_folder("movie-batch-skip");
			const auto project = root.combine_file(import ? "project.wlmp" : "project.otio");
			const auto output_folder = separate_output ? root.combine("output") : root;
			platform::create_folder(output_folder);
			const auto output = output_folder.combine_file(import ? "project.otio" : "project.mp4");
			write_test_file(project, import ? "unreadable project" : missing_media_project);
			write_test_file(output, "existing output");

			movie_batch_options options;
			options.mode = import ? movie_batch_mode::import : movie_batch_mode::render;
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

static void should_refuse_a_movie_output_claimed_during_render()
{
	movie_clip clip;
	clip.path = test_files_folder.combine_file("Small.jpg");
	clip.is_photo = true;
	clip.is_missing = false;
	clip.is_probed = true;
	clip.end = 0.1;
	clip.extent = {64, 64};
	movie_render_request request;
	request.clips = {clip};
	request.output = derive_movie_output(request.clips);
	request.path = _temps.next_path(".mp4");
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
	assert_equal(true, ui::chrome_icons.check == icon_index::check, "a menu's check mark");
	assert_equal(true, ui::chrome_icons.checkbox_off == icon_index::checkbox_off, "a clear check box");
	assert_equal(true, ui::chrome_icons.checkbox_on == icon_index::checkbox_on, "a checked one");
	assert_equal(true, ui::chrome_icons.radio_off == icon_index::radio_off, "a clear radio button");
	assert_equal(true, ui::chrome_icons.radio_on == icon_index::radio_on, "a chosen one");
	assert_equal(true, ui::chrome_icons.close == icon_index::close, "the caption's close button");
	assert_equal(true, ui::chrome_icons.maximize == icon_index::maximize, "its maximize button");
	assert_equal(true, ui::chrome_icons.minimize == icon_index::minimize, "its minimize button");
	assert_equal(true, ui::chrome_icons.restore == icon_index::restore, "and its restore button");
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
	assert_equal(true, map.size() > png_signature.size() &&
	             std::equal(png_signature.begin(), png_signature.end(), map.data()), "and the world map");
}

// Guards the drawn mark against silent drift. The same artwork is drawn independently by
// tools/generate_store_assets.py for app.ico and the Store assets.
static void should_draw_the_logo()
{
	for (const auto size : {16, 32, 44, 150, 256})
	{
		const auto s = std::make_shared<ui::surface>();
		assert_equal(true, s->alloc(size, size, ui::texture_format::ARGB), "logo surface allocated");
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

		// Surface pixels are stored blue first, so ui::get_r reads the blue channel here.
		const auto red_of = [](const ui::color32 c) { return ui::get_b(c); };
		const auto green_of = [](const ui::color32 c) { return ui::get_g(c); };
		const auto blue_of = [](const ui::color32 c) { return ui::get_r(c); };

		assert_equal(true, green_of(top) > red_of(top) && green_of(top) > blue_of(top), "logo top is green");
		assert_equal(true, red_of(bottom) > green_of(bottom) && red_of(bottom) > blue_of(bottom),
		             "logo bottom is red");
		assert_equal(true, red_of(left) > 0x80 && green_of(left) > 0x60 && blue_of(left) < 0x40,
		             "logo left is yellow");
		assert_equal(true, blue_of(right) > red_of(right) && blue_of(right) > green_of(right),
		             "logo right is blue");

		for (const auto c : {top, bottom, left, right})
		{
			assert_equal(255u, ui::get_a(c), "logo square centres are opaque");
		}
	}
}

void register_app_tests(view_state& state, test_registry& tests)
{
	tests.add("Should bind every advertised command"s, should_bind_every_advertised_command);
	tests.add("Should draw the chrome with the app's glyphs"s, should_draw_the_chrome_with_the_app_glyphs);
	tests.add("Should embed the app's assets"s, should_embed_the_app_assets);
	tests.add("Should draw the logo"s, should_draw_the_logo);
	tests.add("Should offer only native Movie project dialogs"s, should_offer_only_native_movie_project_dialogs);
	// Release review: an existing batch output needs neither a readable project nor usable sources.
	tests.add("Should skip existing Movie batch outputs"s, should_skip_existing_movie_batch_outputs);
	tests.add("Should refuse a Movie output claimed during render"s,
	          should_refuse_a_movie_output_claimed_during_render);
	tests.add("Should invoke link commands and selection shortcuts"s, should_invoke_link_commands_and_selection_shortcuts);
	tests.add("Should route focused view keyboard commands"s, should_route_focused_view_keyboard_commands);
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
	tests.add("Should bound advanced search hover thumbnail retries"s,
	          should_bound_advanced_search_hover_thumbnail_retries);
	tests.add("Should Rename with substitutions"s, should_rename_with_substitutions);
	tests.add("Should rename name token without extension"s, should_rename_name_token_without_extension);
	tests.add("Should reject duplicate rename targets"s, should_reject_duplicate_rename_targets);
	tests.add("Should reject unusable rename targets"s, should_reject_unusable_rename_targets);
	tests.add("Should group Rename sidecar collisions"s, should_group_rename_sidecar_collisions);
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
	tests.add("Should keep completed run results on source refresh"s, should_keep_completed_run_results_on_source_refresh);
	tests.add("Should not convert over a destination changed since review"s,
	          should_not_convert_over_a_destination_changed_since_review);
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
	tests.add("Should plan unique convert outputs"s, should_plan_unique_convert_outputs);
	tests.add("Should not plan a convert output over a source"s, should_not_plan_a_convert_output_over_a_source);
	tests.add("Should prepare shared copies under their own names"s, should_prepare_shared_copies_under_their_own_names);
	tests.add("Should adjust item dates from snapshot"s, should_adjust_item_dates_from_snapshot);
	tests.add("Should round trip the environment mask"s, should_round_trip_the_environment_mask);
#ifdef _WIN32
	tests.add("Should classify MAPI results"s, should_classify_mapi_results);
#endif
	tests.add("Should detect duplicate import destinations"s, should_detect_duplicate_import_destinations);
	tests.add("Should revalidate import rows"s, should_revalidate_import_rows);
	tests.add("Should revalidate import sidecars"s, should_revalidate_import_sidecars);
	tests.add("Should import a shared sidecar once"s, should_import_a_shared_sidecar_once);
	// Release review: shared companions follow every resolved destination and every unfinished source.
	tests.add("Should preserve shared Import sidecars after Auto-rename"s,
	          should_preserve_shared_import_sidecars_after_auto_rename);
	tests.add("Should keep shared Import sidecars for unfinished sources"s,
	          should_keep_shared_import_sidecars_for_unfinished_sources);
	tests.add("Should keep import bookkeeping when a folder fails"s,
	          should_keep_import_bookkeeping_when_a_folder_fails);
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
	tests.add("Should prepare Import from the selection snapshot"s,
	          should_prepare_import_from_the_selection_snapshot);
	tests.add("Should offer every removable drive with media for Import"s,
	          should_offer_every_removable_drive_with_media_for_import);
	tests.add("Should reject missing sync folder"s, should_reject_missing_sync_folder);
	tests.add("Should reject overlapping sync folders"s, should_reject_overlapping_sync_folders);
	tests.add("Should reject ambiguous sync roots"s, should_reject_ambiguous_sync_roots);
	tests.add("Should ignore unclaimed remote sync files"s, should_ignore_unclaimed_remote_files);
	tests.add("Should select sync actions"s, should_select_sync_actions);
	tests.add("Should show the latest running work on the taskbar"s, should_show_the_latest_running_work_on_the_taskbar);
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
	tests.add("Should not count concurrent starts"s, should_not_count_concurrent_starts);
	tests.add("Should hand the renderer its YUV latch"s, should_hand_the_renderer_its_yuv_latch);
	tests.add("Should restore history selection"s, should_restore_history_selection);
	tests.add("Should commit history navigation only after open is accepted"s,
	          should_commit_history_navigation_only_after_open_is_accepted);

	//
	// Selection and command enablement
	//
	tests.add("Should select correctly"s, should_select_items);
	tests.add("Should Enable based on selection"s, should_enable_based_on_selection);
	tests.add("Should toggle rating"s, should_toggle_rating);
	tests.add("Should leave the selection alone over empty space"s,
	          should_leave_the_selection_alone_over_empty_space);
	tests.add("Should target only visible items when filtered"s, should_target_only_visible_items_when_filtered);
	tests.add("Should keep shuffle exclusive with sorting"s, should_keep_shuffle_exclusive_with_sorting);
	tests.add("Should not claim one key for two commands"s, should_not_claim_one_key_for_two_commands);
	tests.add("Should spell keyboard accelerators"s, should_spell_keyboard_accelerators);
	tests.add("Should record a bit for every view"s, should_record_a_bit_for_every_view);
	tests.add("Should label grouping and sorting"s, should_label_grouping_and_sorting);
	// Discussion #251 - the filter toolbar scrolls away
	tests.add("Should offer the items menu at every scroll position"s,
	          should_offer_the_items_menu_at_every_scroll_position);

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
	tests.add("Should offer recent searches for an empty address"s,
	          should_offer_recent_searches_for_an_empty_address);
	tests.add("Should discard a superseded completion"s, should_discard_a_superseded_completion);
	// APP-007 - application-local fades must honor the shared alpha-animation gate.
	tests.add("Should gate application alpha effects"s, should_gate_application_alpha_effects);
	tests.add("Should snap app logo hover when animations are disabled"s,
	          should_snap_app_logo_hover_when_animations_are_disabled);
	tests.add("Should skip stable progress repaints when animations are disabled"s,
	          should_skip_stable_progress_repaints_when_animations_are_disabled);
	// APP-005 - destination-folder completion work is asynchronous and latest-wins.
	tests.add("Should clear destination completions by emptying the query"s,
	          should_clear_destination_completions_by_emptying_the_query);
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
	// APP-011 - cancelling a globe drag consumes its release.
	tests.add("Should consume cancelled globe release"s, should_consume_cancelled_globe_release);

	//
	// Browsing sequence
	//
	tests.add("Should step over items that cannot play"s, should_step_over_items_that_cannot_play);
	// Issue #250 - the cursor reset instead of landing after the set that left
	tests.add("Should land on what followed a removed set"s, should_land_on_what_followed_a_removed_set);
	tests.add("Should keep what a superseded open asked for"s, should_keep_what_a_superseded_open_asked_for);
	tests.add("Should offer a slideshow only when something can play"s,
	          should_offer_a_slideshow_only_when_something_can_play);
	tests.add("Should move between sibling folders"s, should_move_between_sibling_folders);
	tests.add("Should decide what follows an item"s, should_decide_what_follows_an_item);

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
	tests.add("Should record crashes"s, should_record_crashes);
	// APP-013 - bucket size is not an unset global photo index.
	tests.add("Should anchor duplicate report sets"s, should_anchor_duplicate_report_sets);
	// APP-014 - requested output errors are command failures, not stdout fallback.
	tests.add("Should fail duplicate report command output errors"s, should_fail_duplicate_report_command_output_errors);
}
