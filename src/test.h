// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Assertion helpers and the failure type every test file asserts through.
// Holds no test itself; the taxonomy is documented in docs/testing.md.

#pragma once

#include <source_location>

// assert_equal for images decodes through `files`, so every test translation unit needs it.
#include "files.h"

struct shared_test_context;

class test_assert_exception final : public std::exception
{
public:
	std::string message;

	explicit test_assert_exception(std::string m) : message(std::move(m))
	{
	}
};

struct test_registry
{
	virtual void add(std::string name, std::function<void(shared_test_context& stc)> f) = 0;
	virtual void add(std::string name, std::function<void(void)> f) = 0;
};

// Counted so the runner can fail a test that asserted nothing: a test whose only assertions sit
// behind an early return or a disabled #if would otherwise report PASS on an unsuitable machine.
inline int test_assert_count = 0;

inline std::string test_assert_where(const std::source_location& loc)
{
	const std::string_view path(loc.file_name());
	const auto slash = path.find_last_of("\\/");
	return std::format("{}:{}", slash == std::string_view::npos ? path : path.substr(slash + 1), loc.line());
}

static void assert_equal(const std::string_view expected, const std::string_view actual,
                         const std::string_view name = {}, const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (str::icmp(actual, expected) != 0)
	{
		throw test_assert_exception(
			std::format("{} - {}: expected '{}', got '{}' at {}", message, name, expected, actual,
			            test_assert_where(loc)));
	}
}

static void assert_not_equal(const std::string_view expected, const std::string_view actual,
                             const std::string_view name = {}, const std::string_view message = {},
                             const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (str::icmp(actual, expected) == 0)
	{
		throw test_assert_exception(std::format("{} - {}: expected '{}' not to equal '{}' at {}", message, name,
		                                        expected, actual, test_assert_where(loc)));
	}
}

static void assert_equal_strict(const std::string_view expected, const std::string_view actual,
                                const std::string_view name = {}, const std::string_view message = {},
                                const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (actual != expected)
	{
		throw test_assert_exception(
			std::format("{} - {}: expected '{}', got '{}' at {}", message, name, expected, actual,
			            test_assert_where(loc)));
	}
}

static void assert_equal(const std::wstring_view expected, const std::wstring_view actual,
                         const std::string_view name = {}, const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::utf16_to_utf8(expected), str::utf16_to_utf8(actual), name, message, loc);
}

static void assert_equal(const int expected, const int actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected), str::to_string(actual), name, message, loc);
}

static void assert_equal(const uint32_t expected, const uint32_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected), str::to_string(actual), name, message, loc);
}

static void assert_equal(const uint64_t expected, const uint64_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected), str::to_string(actual), name, message, loc);
}

static void assert_equal(const ui::const_image_ptr& expected, const ui::const_image_ptr& actual,
                         const std::string_view name = {}, const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(static_cast<int>(expected->width()), static_cast<int>(actual->width()), name, message, loc);
	assert_equal(static_cast<int>(expected->height()), static_cast<int>(actual->height()), name, message, loc);

	files ff;
	const auto diff = ff.pixel_difference(expected, actual);
	assert_equal(true, diff == ui::pixel_difference_result::equal, name, message, loc);
}

static void assert_equal(const bool expected, const bool actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected), str::to_string(actual), name, message, loc);
}

// Note: assert_true/assert_false are reserved: util.h already defines assert_true as a runtime
// invariant macro. Use assert_equal(true, ...) in tests.

static void assert_equal(const double expected, const double actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected, 5), str::to_string(actual, 5), name, message, loc);
}

static void assert_near(const double expected, const double actual, const double tolerance,
                        const std::string_view name = {}, const std::string_view message = {},
                        const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (!std::isfinite(expected) || !std::isfinite(actual) || !std::isfinite(tolerance) || tolerance < 0.0 ||
		std::abs(expected - actual) > tolerance)
	{
		throw test_assert_exception(
			std::format("{} - {}: expected '{}' within '{}', got '{}' at {}", message, name, expected, tolerance,
			            actual, test_assert_where(loc)));
	}
}

static void assert_equal(const df::date_t expected, const df::date_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (expected.to_int64() != actual.to_int64())
	{
		throw test_assert_exception(
			std::format("{} - {}: expected '{}' ({}), got '{}' ({}) at {}", message, name,
			            platform::format_date_time(expected), expected.to_int64(),
			            platform::format_date_time(actual), actual.to_int64(), test_assert_where(loc)));
	}
}

static void assert_equal(const gps_coordinate expected, const gps_coordinate actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(gps_coordinate::decimal_to_dms_str(expected.latitude(), true),
	             gps_coordinate::decimal_to_dms_str(actual.latitude(), true), name, message, loc);
	assert_equal(gps_coordinate::decimal_to_dms_str(expected.longitude(), false),
	             gps_coordinate::decimal_to_dms_str(actual.longitude(), false), name, message, loc);
}

static void assert_equal(const df::xy8 expected, const df::xy8 actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(expected.str(), actual.str(), name, message, loc);
}
