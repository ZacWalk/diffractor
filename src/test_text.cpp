// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for text and localization (app_text, util_spell, po catalogs) -- translated month names, po catalog loading and plural form selection.

#include "pch.h"
#include "test.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "app_text.h"
#include "util_spell.h"

static void should_parse_translated_short_month()
{
	// Shipped catalogs carry short months that are not 3 bytes - ru/uk/ja/ko/zh for all 12, fr for 8.
	const auto* const saved_oct = tt.words.month_short_oct.published();
	const auto* const saved_jan = tt.words.month_short_jan.published();

	// Storage that outlives the reads below, which is the contract text_t::publish states.
	const std::string oct_ru = "\xd0\xbe\xd0\xba\xd1\x82"; // ru, 6 bytes
	const std::string jan_fr = "janv"; // fr, 4 bytes

	tt.words.month_short_oct.publish(&oct_ru);
	tt.words.month_short_jan.publish(&jan_fr);

	const auto oct = str::month("\xd0\xbe\xd0\xba\xd1\x82");
	const auto jan = str::month("JANV");

	tt.words.month_short_oct.publish(saved_oct);
	tt.words.month_short_jan.publish(saved_jan);

	assert_equal(10, oct, "6 byte translated short month");
	assert_equal(1, jan, "4 byte translated short month, case insensitive");
	assert_equal(3, str::month("mar"), "ascii short month");
	assert_equal(5, str::month("May"), "long month");
	assert_equal(0, str::month("nope"), "non month");
}

static void should_format_plural_text()
{
	const plural_text items_fmt("{count} item", "{count} items");

	assert_equal("1 item", format_plural_text(items_fmt, 1), "singular");
	assert_equal("5 items", format_plural_text(items_fmt, 5), "plural");
	assert_equal("0 items", format_plural_text(items_fmt, 0), "zero");

	const plural_text name_fmt("{first-name} will be processed.",
	                           "{count} items including {first-name} will be processed.");

	assert_equal("photo.jpg will be processed.", format_plural_text(name_fmt, "photo.jpg", 1, {}), "named singular");
	assert_equal("3 items including photo.jpg will be processed.", format_plural_text(name_fmt, "photo.jpg", 3, {}),
	             "named plural");
}

static void should_load_po()
{
	const auto app_folder = known_path(platform::known_folder::running_app_folder);
	const auto lang_folder = app_folder.combine("languages");
	const auto lang_path = lang_folder.combine_file("de.po");

	const auto po_entries = load_po(lang_path);

	app_text_t t;
	t.load_lang(lang_path.name(), po_entries);

	assert_equal("Datenbank bereinigen und neu indexieren.\nAlle Daten werden regeneriert.", t.reset_database,
	             "reset_database");
}

static void should_word_platform_messages_for_the_application()
{
	// The platform layer's defaults name no application; Diffractor's wording is set over them, and
	// it is the msgid the catalogs translate.
	const app_text_t t;
	assert_equal("Diffractor requires a processor with sse2.", t.platform_text.error_sse2_needed, "sse2");
	assert_equal("Diffractor requires Windows 7 or newer.", t.platform_text.error_unsupported_os, "os");
	assert_equal("Diffractor has a serious problem and cannot continue.", t.platform_text.error_cannot_continue,
	             "cannot continue");
	assert_equal("Diffractor ERROR", t.platform_text.title_error, "title");
	assert_equal(true, platform::messages{}.title_error.sv().find("Diffractor") == std::string_view::npos,
	             "default names no application");
}

static void should_select_slavic_plural_forms()
{
	// Czech (and Polish, Russian, Ukrainian) declare a third plural form:
	// msgstr[2]. load_po must capture it, and plural_form must select it for
	// "many" counts while keeping the binary behavior for other languages.
	const auto path = _temps.next_path(".po");

	{
		std::ofstream fs(platform::to_stream_path(path));
		fs << "msgid \"one apple\"\n";
		fs << "msgid_plural \"{count} apples\"\n";
		fs << "msgstr[0] \"jedno jablko\"\n";
		fs << "msgstr[1] \"{count} jablka\"\n";
		fs << "msgstr[2] \"{count} jablek\"\n";
	}

	const auto po_entries = load_po(path);

	assert_equal(1, static_cast<int>(po_entries.size()), "entry count");
	assert_equal("jedno jablko", po_entries.front().str, "msgstr[0]");
	assert_equal("{count} jablka", po_entries.front().str_plural, "msgstr[1]");
	assert_equal(1, static_cast<int>(po_entries.front().str_extra.size()), "extra form count");
	assert_equal("{count} jablek", po_entries.front().str_extra.front(), "msgstr[2] captured");

	// Czech uses three forms: one (1), few (2-4), many (0, 5+, ...).
	app_text_t cs;
	cs.load_lang("cs.po", po_entries);
	assert_equal(0, cs.plural_form(1), "cs form for 1");
	assert_equal(1, cs.plural_form(2), "cs form for 2");
	assert_equal(1, cs.plural_form(4), "cs form for 4");
	assert_equal(2, cs.plural_form(5), "cs form for 5");
	assert_equal(2, cs.plural_form(11), "cs form for 11");

	// Russian shares three forms but its form 0 also covers 21, 31, ...; those
	// are clamped to the plural form so the literal-"1" singular is never reused.
	app_text_t ru;
	ru.load_lang("ru.po", po_entries);
	assert_equal(0, ru.plural_form(1), "ru form for 1");
	assert_equal(1, ru.plural_form(2), "ru form for 2");
	assert_equal(2, ru.plural_form(5), "ru form for 5");
	assert_equal(1, ru.plural_form(21), "ru form for 21 clamped");

	// Unlisted languages keep the binary one/plural behavior.
	app_text_t de;
	de.load_lang("de.po", po_entries);
	assert_equal(0, de.plural_form(1), "de form for 1");
	assert_equal(1, de.plural_form(2), "de form for 2");
	assert_equal(1, de.plural_form(5), "de form for 5");

	// Storage that outlives the reads below, which is the contract a published form holds to.
	const std::vector<std::string> czech_forms = {"{count} polozek"};
	cs.title_item_count_fmt.extra_forms.store(&czech_forms);
	assert_equal(false, cs.title_item_count_fmt.extra_form(0).empty(), "an extra plural form is published");
	cs.clear();
	assert_equal(1, cs.plural_form(5), "clear restores binary plural rule");
	assert_equal(true, cs.title_item_count_fmt.extra_form(0).empty(), "clear drops extra plural forms");
}

static void should_clear_duplicate_text_instances()
{
	app_text_t text;
	std::vector<po_entry> entries;

	po_entry print;
	print.id = "Print";
	print.str = "Druck";
	entries.emplace_back(std::move(print));

	po_entry plural;
	plural.id = "{count} item";
	plural.id_plural = "{count} items";
	plural.str = "{count} Ding";
	plural.str_plural = "{count} Dinge";
	plural.str_extra = {"{count} Dingern"};
	entries.emplace_back(std::move(plural));

	text.load_lang("de.po", entries);

	assert_equal("Druck", text.command_print.sv(), "first Print instance is translated");
	assert_equal("Druck", text.print_title.sv(), "second Print instance is translated");
	assert_equal("{count} Ding", text.title_item_count_fmt.one.sv(), "plural singular form is translated");
	assert_equal("{count} Dinge", text.title_item_count_fmt.plural.sv(), "plural primary form is translated");
	assert_equal("{count} Dingern", text.title_item_count_fmt.extra_form(0), "extra plural form is translated");

	text.clear();

	assert_equal("Print", text.command_print.sv(), "first Print instance returns to English");
	assert_equal("Print", text.print_title.sv(), "second Print instance returns to English");
	assert_equal("{count} item", text.title_item_count_fmt.one.sv(), "plural singular form returns to English");
	assert_equal("{count} items", text.title_item_count_fmt.plural.sv(), "plural primary form returns to English");
	assert_equal(true, text.title_item_count_fmt.extra_form(0).empty(), "extra plural forms are cleared");
}

static void should_reject_unsupported_plural_indices()
{
	const auto write_catalog = [](const df::file_path path, const std::string_view indexed_line)
	{
		std::ofstream fs(platform::to_stream_path(path));
		fs << "msgid \"one apple\"\n";
		fs << "msgid_plural \"{count} apples\"\n";
		fs << "msgstr[0] \"one apple\"\n";
		fs << "msgstr[1] \"{count} apples\"\n";
		fs << indexed_line << "\n";
	};

	const auto accepted = _temps.next_path(".po");
	write_catalog(accepted, "msgstr[2] \"{count} apples many\"");
	const auto accepted_report = load_po_report(accepted);
	assert_equal(true, accepted_report.errors.empty(), "highest supported plural index is accepted");
	assert_equal(1_z, accepted_report.entries.front().str_extra.size(), "supported extra form is retained");

	const auto fourth = _temps.next_path(".po");
	write_catalog(fourth, "msgstr[3] \"{count} apples fourth\"");
	const auto fourth_report = load_po_report(fourth);
	assert_equal(true, fourth_report.errors.empty(), "shipped fourth plural index is accepted");
	assert_equal(2_z, fourth_report.entries.front().str_extra.size(), "supported fourth form is retained");

	const auto unsupported = _temps.next_path(".po");
	write_catalog(unsupported, "msgstr[4] \"{count} apples unsupported\"");
	const auto unsupported_report = load_po_report(unsupported);
	assert_equal(false, unsupported_report.errors.empty(), "one beyond supported plural index is rejected");
	assert_equal(0_z, unsupported_report.entries.front().str_extra.size(), "unsupported form is not allocated");

	const auto large = _temps.next_path(".po");
	write_catalog(large, "msgstr[2147483647] \"{count} apples huge\"");
	const auto large_report = load_po_report(large);
	assert_equal(false, large_report.errors.empty(), "large plural index is rejected before allocation");
	assert_equal(0_z, large_report.entries.front().str_extra.size(), "large plural index is not allocated");

	const auto negative = _temps.next_path(".po");
	write_catalog(negative, "msgstr[-1] \"{count} apples negative\"");
	assert_equal(false, load_po_report(negative).errors.empty(), "negative plural index is rejected");

	const auto malformed = _temps.next_path(".po");
	write_catalog(malformed, "msgstr[2x] \"{count} apples malformed\"");
	assert_equal(false, load_po_report(malformed).errors.empty(), "malformed plural index is rejected");
}

// The spell checker only reaches the user through metadata field editing, and it fails soft: a
// missing dictionary must leave every word "valid" rather than underlining the whole caption. Both
// halves are pinned here because a broken load looks exactly like a clean one from the caller.
static void should_check_spelling()
{
	auto& checker = spell();
	checker.lazy_load();

	const auto dictionary_present = known_path(platform::known_folder::running_app_folder)
	                                .combine("dictionaries").combine_file("en_US.dic").exists();

	assert_equal(true, checker.is_word_valid("photograph"), "a dictionary word is valid");

	if (!dictionary_present)
	{
		// Without a dictionary nothing may be reported wrong; that is the fail-soft contract.
		assert_equal(true, checker.is_word_valid("qwertyuiopasdfgh"), "no dictionary means no misspelling");
		return;
	}

	assert_equal(false, checker.is_word_valid("qwertyuiopasdfgh"), "a nonsense word is not valid");

	const auto suggestions = checker.suggest("photograpg");
	assert_equal(true, !suggestions.empty(), "a near miss produces suggestions");

	// Case and punctuation reach the checker straight from a caption field.
	assert_equal(true, checker.is_word_valid("Photograph"), "capitalisation is accepted");
	assert_equal(true, checker.is_word_valid("photographs"), "an inflected form is accepted");

	// Passing at all proves the read falls back to the shipped folder: en_US is only ever installed
	// beside the executable, never in the per-user folder writes go to.
	// add_word is deliberately NOT exercised: it appends to a real dictionary the user owns.
}

// The custom dictionary has to land somewhere the user can write. It used to be placed beside the
// executable whenever that folder existed - which it always does, because en_US ships there - so on
// a Store install "Add to dictionary" and any dictionary download failed silently and the word was
// lost at restart.
static void should_keep_the_custom_dictionary_where_it_can_be_written()
{
	const auto custom = spell().custom_dictionary_path();
	const auto install_folder = known_path(platform::known_folder::running_app_folder).combine("dictionaries");
	const auto user_folder = known_path(platform::known_folder::app_data).combine("dictionaries");

	assert_equal("custom.dic", custom.name(), "the custom dictionary keeps its name");
	assert_equal(user_folder.text(), custom.folder().text(), "the custom dictionary lives in the per-user folder");
	assert_not_equal(install_folder.text(), custom.folder().text(),
	                 "the custom dictionary is not written into the install folder");

	// The shipped dictionary is still readable from where it actually is.
	assert_equal(true, install_folder.combine_file("en_US.aff").exists(),
	             "the shipped dictionary is where the read fallback looks");
}

static void write_minimal_dictionary(df::folder_path folder, bool include_dic);

static void should_load_spelling_dictionaries_asynchronously()
{
	auto& checker = spell();
	deferred_async_strategy async;
	const auto shipped_folder = _temps.next_folder("spell-shipped");
	const auto user_folder = _temps.next_folder("spell-user");
	auto load_started = false;

	write_minimal_dictionary(shipped_folder, true);

	checker.configure_paths_for_tests(shipped_folder, user_folder);
	checker.clear_test_hooks();
	checker.set_load_gate_for_tests([&load_started] { load_started = true; });
	checker.configure_async(&async);
	const df::scope_exit restore([&checker]
	{
		checker.configure_async(nullptr);
		checker.clear_test_hooks();
		checker.reset_paths_for_tests();
	});

	checker.queue_load(async);

	assert_equal(1_z, async.pending_worker_count(async_queue::work), "dictionary load is queued to a worker");
	assert_equal(true, checker.is_word_valid("qwertyuiopasdfgh"),
	             "spelling remains fail-soft while the worker has not loaded a dictionary");
	assert_equal(false, load_started, "the UI path did not run the loader");

	checker.configure_paths_for_tests(shipped_folder, user_folder);
	assert_equal(true, async.run_next(async_queue::work), "the stale worker can finish");
	async.drain_ui();

	assert_equal(false, checker.is_ready(), "a stale dictionary load completion is ignored");
}

static void write_minimal_dictionary(const df::folder_path folder, const bool include_dic)
{
	write_test_file(folder.combine_file("en_US.aff"), "SET UTF-8\nTRY abcdefghijklmnopqrstuvwxyz\n");
	if (include_dic)
	{
		write_test_file(folder.combine_file("en_US.dic"), "1\nphotograph\n");
	}
}

static void should_reload_spelling_after_dictionary_download()
{
	auto& checker = spell();
	deferred_async_strategy async;
	const auto shipped_folder = _temps.next_folder("spell-download-shipped");
	const auto user_folder = _temps.next_folder("spell-download-user");

	write_minimal_dictionary(shipped_folder, false);

	checker.configure_paths_for_tests(shipped_folder, user_folder);
	checker.clear_test_hooks();
	checker.configure_async(&async);
	const df::scope_exit restore([&checker]
	{
		checker.configure_async(nullptr);
		checker.clear_test_hooks();
		checker.reset_paths_for_tests();
	});

	checker.queue_load(async);
	assert_equal(true, async.run_next(async_queue::work), "the incomplete dictionary load runs");
	async.drain_ui();
	assert_equal(false, checker.is_ready(), "an aff without its dic fails soft");

	write_minimal_dictionary(shipped_folder, true);
	checker.dictionary_downloaded(async);

	assert_equal(1_z, async.pending_worker_count(async_queue::work),
	             "download completion re-queues dictionary loading");
	assert_equal(true, async.run_next(async_queue::work), "the completed dictionary load runs");
	async.drain_ui();
	assert_equal(true, checker.is_ready(), "the completed dictionary is published in-session");
	assert_equal(true, checker.is_word_valid("photograph"), "the reloaded dictionary answers words");
}

static void should_report_custom_dictionary_persistence_results()
{
	auto& checker = spell();
	deferred_async_strategy async;
	const auto shipped_folder = _temps.next_folder("custom-dic-shipped");
	const auto user_folder = _temps.next_folder("custom-dic-user");

	checker.configure_paths_for_tests(shipped_folder, user_folder);
	checker.clear_test_hooks();
	checker.configure_async(&async);
	const df::scope_exit restore([&checker]
	{
		checker.configure_async(nullptr);
		checker.clear_test_hooks();
		checker.reset_paths_for_tests();
	});

	std::vector<custom_dictionary_add_result> results;
	checker.add_word("persisted", [&results](const custom_dictionary_add_result result)
	{
		results.emplace_back(result);
	});

	assert_equal(1_z, async.pending_worker_count(async_queue::work), "custom dictionary write is queued");
	assert_equal(0_z, results.size(), "persistence is not reported before storage completes");
	assert_equal(true, async.run_next(async_queue::work), "the custom word is written on a worker");
	async.drain_ui();

	assert_equal(1_z, results.size(), "the persisted result is published");
	assert_equal(static_cast<int>(custom_dictionary_add_status::persisted), static_cast<int>(results.back().status),
	             "append and flush succeeded");
	assert_equal(true, checker.custom_dictionary_path().exists(), "the test custom dictionary was written");

	checker.set_custom_dictionary_writer_for_tests([](const df::file_path&, std::string_view)
	{
		return custom_dictionary_add_result{custom_dictionary_add_status::failed, "flush failed"};
	});

	checker.add_word("failed", [&results](const custom_dictionary_add_result result)
	{
		results.emplace_back(result);
	});
	assert_equal(true, async.run_next(async_queue::work), "the failing writer runs on a worker");
	async.drain_ui();

	assert_equal(static_cast<int>(custom_dictionary_add_status::failed), static_cast<int>(results.back().status),
	             "flush failure is reported");
	assert_equal("flush failed", results.back().message, "the bounded failure message is preserved");

	checker.add_word("stale", [&results](const custom_dictionary_add_result result)
	{
		results.emplace_back(result);
	});
	checker.configure_paths_for_tests(shipped_folder, user_folder);
	assert_equal(true, async.run_next(async_queue::work), "the stale writer runs");
	async.drain_ui();

	assert_equal(static_cast<int>(custom_dictionary_add_status::stale), static_cast<int>(results.back().status),
	             "stale persistence completion is ignored");
}

static void should_flush_pending_custom_dictionary_words_on_shutdown()
{
	auto& checker = spell();
	deferred_async_strategy async;
	const auto shipped_folder = _temps.next_folder("custom-dic-flush-shipped");
	const auto user_folder = _temps.next_folder("custom-dic-flush-user");

	checker.configure_paths_for_tests(shipped_folder, user_folder);
	checker.clear_test_hooks();
	checker.configure_async(&async);
	const df::scope_exit restore([&checker]
	{
		checker.configure_async(nullptr);
		checker.clear_test_hooks();
		checker.reset_paths_for_tests();
	});

	std::vector<custom_dictionary_add_result> results;
	checker.add_word("queued-before-close", [&results](const custom_dictionary_add_result result)
	{
		results.emplace_back(result);
	});

	assert_equal(1_z, async.pending_worker_count(async_queue::work), "the custom dictionary write is queued");
	checker.flush_pending_custom_words();

	assert_equal(true, checker.custom_dictionary_path().exists(),
	             "shutdown flush writes a word that the worker queue has not reached");
	assert_equal(0_z, results.size(), "worker completion is not faked by the shutdown flush");
}

void register_text_tests(view_state& state, test_registry& tests)
{
	//
	// Formatting
	//
	tests.add("Should parse translated short month"s, should_parse_translated_short_month);
	tests.add("Should format plural text"s, should_format_plural_text);

	//
	// Catalogs
	//
	tests.add("Should load po"s, should_load_po);
	tests.add("Should word platform messages for the application"s, should_word_platform_messages_for_the_application);
	tests.add("Should select Slavic plural forms"s, should_select_slavic_plural_forms);
	// APP-010 - returning to English must clear every registered instance, not only the lookup map.
	tests.add("Should clear duplicate text instances"s, should_clear_duplicate_text_instances);
	// APP-012 - malformed catalogs must fail validation without allocating unsupported plural forms.
	tests.add("Should reject unsupported plural indices"s, should_reject_unsupported_plural_indices);

	//
	// Spell checking
	//
	tests.add("Should check spelling"s, should_check_spelling);
	tests.add("Should keep the custom dictionary where it can be written"s,
	          should_keep_the_custom_dictionary_where_it_can_be_written);
	// SRC-008 - asynchronous dictionary loading.
	tests.add("Should load spelling dictionaries asynchronously"s, should_load_spelling_dictionaries_asynchronously);
	tests.add("Should reload spelling after dictionary download"s, should_reload_spelling_after_dictionary_download);
	// SRC-009 - custom dictionary persistence results.
	tests.add("Should report custom dictionary persistence results"s,
	          should_report_custom_dictionary_persistence_results);
	tests.add("Should flush pending custom dictionary words on shutdown"s,
	          should_flush_pending_custom_dictionary_words_on_shutdown);
}
