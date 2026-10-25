// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shared test infrastructure definitions, utility tests, test registration,
// and console test runner.

#include "pch.h"

#include "metadata_xmp.h"
#include "test_fixtures.h"
#include "test_runner.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared global definitions
///////////////////////////////////////////////////////////////////////////////////////////////////

temp_files _temps;
std::atomic_int test_version = 0;
df::cancel_token test_token(test_version);


///////////////////////////////////////////////////////////////////////////////////////////////////
// Console test runner
///////////////////////////////////////////////////////////////////////////////////////////////////

struct console_test_entry
{
	std::string name;
	std::function<void(shared_test_context& stc)> func;
};

struct console_test_registry final : test_registry
{
	std::vector<console_test_entry> entries;

	void add(std::string name, std::function<void(shared_test_context& stc)> f) override
	{
		entries.push_back({std::move(name), std::move(f)});
	}

	void add(std::string name, std::function<void(void)> f) override
	{
		entries.push_back({std::move(name), [f = std::move(f)](shared_test_context&) { f(); }});
	}
};

int run_console_tests(const std::string_view test_filter)
{
	load_file_types();
	metadata_xmp::initialise();

	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, nullptr);

	console_test_registry registry;
	register_tests(state, registry);

	// Filter tests by wildcard pattern
	std::vector<console_test_entry> filtered;
	for (auto& entry : registry.entries)
	{
		if (str::wildcard_icmp(entry.name, test_filter))
		{
			filtered.push_back(std::move(entry));
		}
	}

	const auto total = static_cast<int>(filtered.size());
	int passed = 0;
	int failed = 0;
	int structural_failures = 0;

	// A filter that matches nothing prints nothing and used to exit 0, so a mistyped /test:<name>
	// was indistinguishable from a clean run.
	if (total == 0)
	{
		printf("  FAIL  no test matched the filter '%.*s'\n", static_cast<int>(test_filter.size()),
		       test_filter.data());
		return 1;
	}

	// Two registrations under one name make /test:<name> ambiguous and a failure report unattributable.
	{
		df::hash_map<std::string_view, int, df::ihash, df::ieq> seen;
		for (const auto& entry : filtered) ++seen[entry.name];

		for (const auto& [name, count] : seen)
		{
			if (count > 1)
			{
				printf("  FAIL  duplicate test name '%.*s' registered %d times\n",
				       static_cast<int>(name.size()), name.data(), count);
				++structural_failures;
			}
		}
	}

	printf("Running %d tests...\n\n", total);

	shared_test_context stc;

	for (const auto& entry : filtered)
	{
		const auto t = df::now();
		const auto name_utf8 = std::string(entry.name.data(), entry.name.size());
		const auto asserts_before = test_assert_count;

		try
		{
			entry.func(stc);
			const auto elapsed_ms = df::round((df::now() - t) * 1000);

			// A test that asserted nothing proves nothing: it has usually been disabled by an early
			// return or an #if, and would otherwise report PASS forever.
			if (test_assert_count == asserts_before)
			{
				printf("  FAIL  %s (%dms)\n        No assertions were made\n", name_utf8.c_str(), elapsed_ms);
				++failed;
			}
			else
			{
				printf("  PASS  %s (%dms)\n", name_utf8.c_str(), elapsed_ms);
				++passed;
			}
		}
		catch (const test_assert_exception& e)
		{
			const auto elapsed_ms = df::round((df::now() - t) * 1000);
			const auto msg = std::string(e.message.data(), e.message.size());
			printf("  FAIL  %s (%dms)\n        %s\n", name_utf8.c_str(), elapsed_ms, msg.c_str());
			++failed;
		}
		catch (const std::exception& e)
		{
			const auto elapsed_ms = df::round((df::now() - t) * 1000);
			printf("  FAIL  %s (%dms)\n        Exception: %s\n", name_utf8.c_str(), elapsed_ms, e.what());
			++failed;
		}
		catch (...)
		{
			const auto elapsed_ms = df::round((df::now() - t) * 1000);
			printf("  FAIL  %s (%dms)\n        Unknown exception\n", name_utf8.c_str(), elapsed_ms);
			++failed;
		}
	}

	const auto cleanup_errors = _temps.delete_temps();
	const auto cleanup_failures = static_cast<int>(cleanup_errors.size());
	for (const auto& error : cleanup_errors)
	{
		printf("  FAIL  temporary fixture cleanup\n        %s\n", error.c_str());
	}

	printf("\n========================================\n");
	printf("Results: %d passed, %d failed, %d total\n", passed, failed + cleanup_failures,
	       total + cleanup_failures);
	printf("========================================\n");

	return failed > 0 || cleanup_failures > 0 || structural_failures > 0 ? 1 : 0;
}
