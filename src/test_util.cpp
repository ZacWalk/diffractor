// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for the shared utility layer (util*, crypto*) -- strings, wildcards, versions, natural compare, interning, cancellation tokens, result scopes, hashes and perceptual hashes, json parsing, file presence and memory-mapped files.

#include "pch.h"
#include "files.h"
#include "test.h"
#include "app_command_line.h"
#include "crypto.h"
#include "libdesktop/util_base64.h"
#include "crypto_sha.h"
#include "libdesktop/util_json.h"
#include "libdesktop/util_kdtree.h"
#include "libdesktop/util_simd.h"
#include "util_top.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "app_util.h"

static void assert_assertion_fails(const std::function<void()>& f, const std::string_view name)
{
	auto failed = false;

	try
	{
		f();
	}
	catch (const test_assert_exception&)
	{
		failed = true;
	}

	assert_equal(true, failed, name);
}

static void should_name_files_after_the_application()
{
	// The foundation and the platform layer derive their file, folder and key names from the
	// application's name. These are the names Diffractor always used, so existing logs, settings
	// and caches are still found.
	assert_equal("diffractor"sv, df::app_file_stem(), "file stem");
	assert_equal("diffractor.log"sv, df::log_path.name(), "log");
	assert_equal("diffractor.previous.log"sv, df::previous_log_path.name(), "previous log");
}

static void should_complete_result_scope()
{
	const auto results = std::make_shared<null_item_results_ui>();
	{
		result_scope scope(results);
	}

	assert_equal(1, results->complete_count, "normal scope exit completes results");
	assert_equal(0, results->abort_count, "normal scope exit does not abort results");
}

static void should_abort_result_scope_during_exception()
{
	const auto results = std::make_shared<null_item_results_ui>();
	try
	{
		result_scope scope(results);
		throw std::runtime_error("test");
	}
	catch (const std::runtime_error&)
	{
	}

	assert_equal(0, results->complete_count, "exception unwinding does not complete results");
	assert_equal(1, results->abort_count, "exception unwinding aborts results");
}

// EXIF text can arrive as UTF-16, and which way round it is decides whether it reads as words or
// as mojibake. Testing only the even bytes for zero recognised big-endian alone - so a
// little-endian value, which is what Windows writes, was read a byte at a time.
static void should_detect_utf16_either_way_round()
{
	const auto is_utf16 = [](const std::initializer_list<uint8_t> bytes)
	{
		const std::vector<uint8_t> v(bytes);
		return str::is_utf16(v.data(), static_cast<int>(v.size()));
	};

	// "Hi" with no mark, each way round.
	assert_equal(true, is_utf16({'H', 0, 'i', 0}), "unmarked little-endian is recognised");
	assert_equal(true, is_utf16({0, 'H', 0, 'i'}), "and so is unmarked big-endian");

	// Marked, each way round.
	assert_equal(true, is_utf16({0xff, 0xfe, 'H', 0}), "a little-endian mark is recognised");
	assert_equal(true, is_utf16({0xfe, 0xff, 0, 'H'}), "and so is a big-endian mark");

	// UTF-32 LE opens with the UTF-16 LE mark and must not be taken for it.
	assert_equal(false, is_utf16({0xff, 0xfe, 0x00, 0x00}), "a utf-32 mark is not utf-16");

	// Plain bytes stay plain.
	assert_equal(false, is_utf16({'H', 'e', 'l', 'l', 'o'}), "ascii is not utf-16");
	assert_equal(false, is_utf16({0xe2, 0x82, 0xac, 'x'}), "utf-8 is not utf-16");
	assert_equal(false, is_utf16({'H'}), "and a single byte cannot be");

	// Recognising either order is only half of it: the text is decoded the same way round it was
	// recognised, and a mark is not part of the value. Copied as it lay, big-endian text came out
	// backwards and a marked value kept an invisible U+FEFF at its start.
	const auto decode = [](const std::initializer_list<uint8_t> bytes)
	{
		const std::vector<uint8_t> v(bytes);
		return str::utf16_from_bytes(v.data(), static_cast<int>(v.size()));
	};

	assert_equal(true, decode({'H', 0, 'i', 0}) == u"Hi", "unmarked little-endian decodes");
	assert_equal(true, decode({0, 'H', 0, 'i'}) == u"Hi", "and so does unmarked big-endian");
	assert_equal(true, decode({0xff, 0xfe, 'H', 0, 'i', 0}) == u"Hi", "a little-endian mark is not part of the text");
	assert_equal(true, decode({0xfe, 0xff, 0, 'H', 0, 'i'}) == u"Hi", "and a big-endian mark sets the order");
}

static void should_icmp_natural()
{
	// Test basic numeric comparison - the key bug fix
	// Files like 43_100 should come after 43_99, not between 43_10 and 43_11
	assert_equal(true, str::icmp_natural("43_09", "43_10") < 0, "43_09 < 43_10");
	assert_equal(true, str::icmp_natural("43_10", "43_11") < 0, "43_10 < 43_11");
	assert_equal(true, str::icmp_natural("43_10", "43_100") < 0, "43_10 < 43_100");
	assert_equal(true, str::icmp_natural("43_99", "43_100") < 0, "43_99 < 43_100");
	assert_equal(true, str::icmp_natural("43_100", "43_101") < 0, "43_100 < 43_101");

	// Verify the order reported in the bug is fixed
	assert_equal(true, str::icmp_natural("43_09", "43_100") < 0, "43_09 < 43_100");
	assert_equal(true, str::icmp_natural("43_11", "43_100") < 0, "43_11 < 43_100");

	// Test equality
	assert_equal(0, str::icmp_natural("file10", "file10"), "equal strings");
	assert_equal(0, str::icmp_natural("", ""), "empty strings");

	// Test case insensitivity
	assert_equal(0, str::icmp_natural("File10", "file10"), "case insensitive");
	assert_equal(0, str::icmp_natural("FILE10", "file10"), "case insensitive upper");

	// Test basic natural ordering
	assert_equal(true, str::icmp_natural("file1", "file2") < 0, "file1 < file2");
	assert_equal(true, str::icmp_natural("file2", "file10") < 0, "file2 < file10");
	assert_equal(true, str::icmp_natural("file9", "file10") < 0, "file9 < file10");
	assert_equal(true, str::icmp_natural("file10", "file11") < 0, "file10 < file11");
	assert_equal(true, str::icmp_natural("file19", "file20") < 0, "file19 < file20");
	// Issue #197: "43_100" sorted between "43_10" and "43_11" (lexicographic instead of numeric).
	assert_equal(true, str::icmp_natural("file99", "file100") < 0, "file99 < file100");
	assert_equal(true, str::icmp_natural("file100", "file1000") < 0, "file100 < file1000");

	// Test reverse ordering
	assert_equal(true, str::icmp_natural("file10", "file9") > 0, "file10 > file9");
	assert_equal(true, str::icmp_natural("file100", "file99") > 0, "file100 > file99");

	// Test with different prefixes
	assert_equal(true, str::icmp_natural("a10", "b1") < 0, "a10 < b1");
	assert_equal(true, str::icmp_natural("img001", "img002") < 0, "img001 < img002");
	assert_equal(true, str::icmp_natural("img009", "img010") < 0, "img009 < img010");

	// Test numbers at the start
	assert_equal(true, str::icmp_natural("1file", "2file") < 0, "1file < 2file");
	assert_equal(true, str::icmp_natural("9file", "10file") < 0, "9file < 10file");
	assert_equal(true, str::icmp_natural("10file", "100file") < 0, "10file < 100file");

	// Test multiple number groups
	assert_equal(true, str::icmp_natural("file1-1", "file1-2") < 0, "file1-1 < file1-2");
	assert_equal(true, str::icmp_natural("file1-9", "file1-10") < 0, "file1-9 < file1-10");
	assert_equal(true, str::icmp_natural("file1-10", "file2-1") < 0, "file1-10 < file2-1");

	// Test leading zeros
	assert_equal(true, str::icmp_natural("file007", "file7") > 0, "file007 > file7 (more leading zeros)");
	assert_equal(true, str::icmp_natural("file07", "file007") < 0,
	             "file07 < file007 (fewer leading zeros)");
	assert_equal(0, str::icmp_natural("file007", "file007"), "same with leading zeros");

	// Test purely numeric strings
	assert_equal(true, str::icmp_natural("1", "2") < 0, "1 < 2");
	assert_equal(true, str::icmp_natural("9", "10") < 0, "9 < 10");
	assert_equal(true, str::icmp_natural("99", "100") < 0, "99 < 100");
	assert_equal(true, str::icmp_natural("999", "1000") < 0, "999 < 1000");

	// Test strings with no numbers
	assert_equal(true, str::icmp_natural("abc", "abd") < 0, "abc < abd");
	assert_equal(true, str::icmp_natural("abc", "abcd") < 0, "abc < abcd");
	assert_equal(0, str::icmp_natural("abc", "ABC"), "abc == ABC (case insensitive)");

	// Test image sequence patterns (common use case)
	assert_equal(true, str::icmp_natural("DSC_0001.jpg", "DSC_0002.jpg") < 0, "DSC sequence");
	assert_equal(true, str::icmp_natural("DSC_0099.jpg", "DSC_0100.jpg") < 0, "DSC sequence 99-100");
	assert_equal(true, str::icmp_natural("IMG_9999.png", "IMG_10000.png") < 0, "IMG sequence overflow");

	// A run longer than nineteen digits does not fit a uint64_t. Accumulating the value wrapped it,
	// and the wrapped result compared as something small - so a long serial number, a hash or a
	// phone-camera timestamp landed at an arbitrary point in the listing. Leading zeros have
	// already been consumed by this point, so the longer run is simply the larger number.
	assert_equal(true, str::icmp_natural("id_99999999999999999999", "id_100000000000000000000") < 0,
	             "twenty digits beats twenty");
	assert_equal(true, str::icmp_natural("id_18446744073709551615", "id_18446744073709551616") < 0,
	             "one past the widest value a uint64 holds still orders after it");
	assert_equal(true, str::icmp_natural("id_9", "id_99999999999999999999999999") < 0,
	             "a short run orders before a very long one");
	assert_equal(0, str::icmp_natural("id_99999999999999999999", "id_99999999999999999999"),
	             "and two of the same are equal");

	// 2^64 wraps to zero, so any comparison that forms the value puts it below a bare 1.
	assert_equal(true, str::icmp_natural("id_1", "id_18446744073709551616") < 0,
	             "two to the sixty-four is larger than one, not smaller");
	assert_equal(true, str::icmp_natural("id_18446744073709551616", "id_1") > 0, "and the reverse holds");
}

static void should_follow_the_filesystem_for_path_identity()
{
	// The subject is identity, not spelling, so the literals use the running platform's root.
	const auto root = df::folder_path(df::windows_path_semantics ? "c:\\photos" : "/photos");
	const auto root_upper = df::folder_path(df::windows_path_semantics ? "C:\\PHOTOS" : "/PHOTOS");
	const auto lower = root.combine_file("holiday.jpg");
	const auto upper = root.combine_file("HOLIDAY.JPG");
	const auto other = root.combine_file("apple.jpg");

	// Case-folded where the filesystem folds, and two distinct files where it does not.
	constexpr auto same = df::case_insensitive_path_identity;
	assert_equal(same, root.compare(root_upper) == 0, "folder identity");
	assert_equal(same, lower.icmp(upper) == 0, "file identity");
	assert_equal(same, lower == upper, "file equality follows the comparison");

	// A hash that disagreed with the comparison would leave one file in two buckets.
	const df::ihash hash;
	assert_equal(same, hash(lower) == hash(upper), "file hash agrees with file identity");
	assert_equal(same, hash(root) == hash(root_upper), "folder hash agrees with folder identity");
	assert_equal(true, hash(lower) == hash(root.combine_file("holiday.jpg")), "one path, one hash");

	df::hash_set<df::file_path, df::ihash, df::ieq> paths;
	paths.emplace(lower);
	paths.emplace(upper);
	assert_equal(same ? 1 : 2, static_cast<int>(paths.size()), "a set holds one entry per file");

	// Ordering still folds case on both platforms, so a case variant sorts beside its twin rather
	// than ahead of every lower-case name. A byte comparison would put "HOLIDAY.JPG" before
	// "apple.jpg", which is what this asserts against.
	assert_equal(true, lower.icmp(other) > 0, "ordering is lexicographic");
	assert_equal(true, upper.icmp(other) > 0, "a case variant orders with its twin, not before it");

	// Type is a different question and never follows the filesystem: an upper-case extension names
	// the same format on every platform.
	assert_equal(0, str::icmp(lower.extension(), upper.extension()), "extension matching stays folded");
	const auto ft_lower = files::file_type_from_name(lower);
	const auto ft_upper = files::file_type_from_name(upper);
	assert_equal(true, ft_lower == ft_upper, "file type does not follow path identity");

	const auto accented_acute = root.parent().combine("album-é");
	const auto accented_grave = root.parent().combine("album-è");
	assert_equal(false, df::folder_contains(accented_acute.text(), accented_grave.combine("child").text()),
	             "accented sibling folders are distinct");
	assert_equal(true, df::folder_contains(accented_acute.text(), accented_acute.combine("child").text()),
	             "a real child is contained");
	assert_equal(false, df::folder_contains(root.text(), root.parent().combine("photoshop").text()),
	             "prefix matches stop at a separator boundary");
	assert_equal(same, df::folder_contains(root_upper.text(), root.combine("child").text()),
	             "case aliases follow filesystem identity only");

	// A key held as a string has to answer the same way, or the rename planner disagrees with the
	// index about whether a destination is occupied.
	assert_equal(same, df::compare_path_key(lower.pack(), upper.pack()) == 0, "packed key identity");
	assert_equal(same, df::path_text_starts(root_upper.text(), root.text()), "prefix match identity");
	assert_equal(true, df::path_text_starts(root.text(), root.text()), "a path contains itself");
}

static void should_parse_files_directly_under_the_root()
{
	const auto root = df::folder_path(df::windows_path_semantics ? "c:\\" : "/");
	const auto direct_text = df::windows_path_semantics ? "c:\\photo.jpg" : "/photo.jpg";
	const auto direct = df::file_path(direct_text);
	const auto combined = root.combine_file("photo.jpg");

	assert_equal(root.text(), direct.folder().text(), "root folder is preserved");
	assert_equal("photo.jpg", direct.name(), "root child keeps its name");
	assert_equal(".jpg", direct.extension(), "root child keeps its extension");
	assert_equal(direct_text, direct.pack(), "root child packs without a trailing folder separator");
	assert_equal(true, direct == combined, "direct and combined root children are equal");
	const df::ihash hash;
	assert_equal(hash(combined), hash(direct), "direct and combined root children hash alike");

	auto reused = root.combine_file("old-name.txt");
	reused.un_pack(direct_text);
	assert_equal(root.text(), reused.folder().text(), "reuse refreshes the folder");
	assert_equal("photo.jpg", reused.name(), "reuse refreshes the name");

	reused.un_pack(std::string(root.text()));
	assert_equal(root.text(), reused.folder().text(), "folder-only unpack keeps the folder");
	assert_equal(true, reused.name().is_empty(), "folder-only unpack clears a stale name");
}

static void should_group_elements_by_folder()
{
	struct element
	{
		df::folder_path folder;
		int id = 0;
		bool skip = false;
	};

	// The subject is grouping, not path spelling, so the literals use the running platform's root.
	constexpr auto a = df::windows_path_semantics ? "c:\\a" : "/a";
	constexpr auto b = df::windows_path_semantics ? "c:\\b" : "/b";
	constexpr auto c = df::windows_path_semantics ? "c:\\c" : "/c";
	constexpr auto a_upper = df::windows_path_semantics ? "C:\\A" : "/A";

	const std::vector<element> elements{
		{df::folder_path(a), 1},
		{df::folder_path(b), 2},
		{df::folder_path(a), 3},
		{df::folder_path(b), 4, true},
		{df::folder_path(a_upper), 5},
		{df::folder_path(c), 6},
	};

	// Path identity follows the filesystem, so the case variant is a fourth folder where two
	// spellings are two places and a member of the first group where they are one.
	constexpr auto expected_groups = df::case_insensitive_path_identity ? 3 : 4;

	df::folder_groups groups;
	groups.build(elements,
	             [](const element& e) { return e.folder; },
	             [](const element& e) { return !e.skip; });

	assert_equal(expected_groups, static_cast<int>(groups.groups().size()), "one group per distinct folder");

	// Groups are in first-seen order and each keeps its elements in input order.
	assert_equal(a, groups.groups()[0].folder.text().str(), "first group");
	assert_equal(b, groups.groups()[1].folder.text().str(), "second group");
	assert_equal(c, groups.groups()[expected_groups - 1].folder.text().str(), "last group");

	const auto ids = [&](const size_t g)
	{
		std::string result;
		for (const auto i : groups.elements(groups.groups()[g])) result += std::to_string(elements[i].id);
		return result;
	};

	assert_equal(df::case_insensitive_path_identity ? "135" : "13", ids(0), "case variant follows path identity");
	assert_equal("2", ids(1), "excluded element is dropped");
	assert_equal("6", ids(expected_groups - 1), "single element group");

	groups.build(elements, [](const element& e) { return e.folder; });
	assert_equal(expected_groups, static_cast<int>(groups.groups().size()),
	             "rebuild replaces the previous grouping");
	assert_equal("24", ids(1), "no predicate includes every element");

	// Enough distinct folders to force the lookup tables to grow and rehash.
	std::vector<element> many;
	const auto numbered = [](const int i)
	{
		return df::folder_path(df::windows_path_semantics ? std::format("c:\\f{}", i) : std::format("/f{}", i));
	};
	for (auto i = 0; i < 500; ++i) many.emplace_back(numbered(i), i);
	for (auto i = 0; i < 500; ++i) many.emplace_back(numbered(i), i);

	groups.build(many, [](const element& e) { return e.folder; });
	assert_equal(500, static_cast<int>(groups.groups().size()), "grown tables keep folders distinct");

	for (const auto& g : groups.groups())
	{
		assert_equal(2, static_cast<int>(groups.elements(g).size()), "each folder collected both elements");
	}

	groups.clear();
	assert_equal(true, groups.empty(), "cleared grouping has no groups");
}

static void should_cancel_superseded_tokens()
{
	std::atomic_int version = 0;
	const df::cancel_token first(version);
	const auto first_copy = first;

	assert_equal(false, first.is_cancelled(), "current token is active");
	assert_equal(false, first_copy.is_cancelled(), "copied current token is active");

	const df::cancel_token second(version);
	assert_equal(true, first.is_cancelled(), "new generation cancels previous token");
	assert_equal(true, first_copy.is_cancelled(), "new generation cancels copies of previous token");
	assert_equal(false, second.is_cancelled(), "new generation remains active");
}

static void should_calc_HMACSHA1()
{
	const auto signature = crypto::hmac_sha1("Jefe", "what do ya want for nothing?");
	assert_equal_strict("7/zfauXrL6LSdBbV8YTfnCWafHk=", signature, "Signature");
}

static void should_calc_hashes()
{
	assert_equal("A9993E364706816ABA3E25717850C26C9CD0D89D", crypto::to_sha1("abc"), "SHA1");
	assert_equal("187797D630ECAA0FC1B920CD9F809C2BBFFCBF4C", crypto::to_sha1(long_text), "SHA1");
	assert_equal("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", crypto::to_sha256("abc"),
	             "SHA256");
	assert_equal("1660F10AEC042D762CF8B1C53E976F890C8E797BEF74807F505EDCE20308FC2F", crypto::to_sha256(long_text),
	             "SHA256");

	const auto crc_data = "hello world"s;
	const auto crc_result = df::crc32c(crc_data.data(), crc_data.size());
	assert_equal(0xc99465aa, crc_result, "crc32");
	assert_equal(0xe3069283, df::crc32c("123456789"sv), "crc32 standard vector");

	const auto crc_c = ~calc_crc32c_c(df::crc32c_init, crc_data.data(), crc_data.size());
	assert_equal(0xc99465aa, crc_c, "crc32 c");
	assert_equal(0xe3069283, ~calc_crc32c_c(df::crc32c_init, "123456789", 9), "crc32 c standard vector");

	if (platform::crc32_supported)
	{
		const auto crc_x86 = ~calc_crc32c_x86(df::crc32c_init, crc_data.data(), crc_data.size());
		assert_equal(0xc99465aa, crc_x86, "crc32 x86");
		assert_equal(0xe3069283, ~calc_crc32c_x86(df::crc32c_init, "123456789", 9), "crc32 x86 standard vector");
	}

	if (platform::arm_crc32_supported)
	{
		const auto crc_neon = ~calc_crc32c_arm(df::crc32c_init, crc_data.data(), crc_data.size());
		assert_equal(0xc99465aa, crc_neon, "crc32 neon");
		assert_equal(0xe3069283, ~calc_crc32c_arm(df::crc32c_init, "123456789", 9), "crc32 arm standard vector");
	}

	alignas(16) std::array<uint8_t, 96> boundary_data;
	for (auto i = 0u; i < boundary_data.size(); ++i)
	{
		boundary_data[i] = static_cast<uint8_t>(i * 37u + 11u);
	}

	for (auto offset = 0u; offset < 16u; ++offset)
	{
		for (auto len = 0u; len <= 80u; ++len)
		{
			const auto* const data = boundary_data.data() + offset;
			const auto expected = calc_crc32c_c(df::crc32c_init, data, len);
			assert_equal(~expected, df::crc32c(data, len), "crc32 dispatched boundary");

			if (platform::crc32_supported)
			{
				assert_equal(expected, calc_crc32c_x86(df::crc32c_init, data, len), "crc32 x86 boundary");
				const auto split = len / 2;
				const auto first = calc_crc32c_x86(df::crc32c_init, data, split);
				assert_equal(expected, calc_crc32c_x86(first, data + split, len - split), "crc32 x86 continuation");
			}

			if (platform::arm_crc32_supported)
			{
				assert_equal(expected, calc_crc32c_arm(df::crc32c_init, data, len), "crc32 arm boundary");
				const auto split = len / 2;
				const auto first = calc_crc32c_arm(df::crc32c_init, data, split);
				assert_equal(expected, calc_crc32c_arm(first, data + split, len - split), "crc32 arm continuation");
			}
		}
	}
}

// A synthetic 32x32 field, so the hash is tested on its own terms without a decoder in the way.
static std::array<uint8_t, crypto::phash_pixels> make_phash_field(const int seed)
{
	std::array<uint8_t, crypto::phash_pixels> result{};

	for (auto y = 0u; y < crypto::phash_extent; ++y)
	{
		for (auto x = 0u; x < crypto::phash_extent; ++x)
		{
			const auto v = (x * 7 + y * 13 + seed * 29) % 251;
			result[y * crypto::phash_extent + x] = static_cast<uint8_t>((v * v) % 256);
		}
	}

	return result;
}

static void should_calc_perceptual_hashes()
{
	const auto field = make_phash_field(1);
	const auto hash = crypto::perceptual_hash(field.data(), field.size());

	assert_equal(true, crypto::phash_is_usable(hash), "a detailed field hashes");
	assert_equal(hash, crypto::perceptual_hash(field.data(), field.size()), "the same pixels hash the same");
	assert_equal(0, crypto::phash_distance(hash, hash), "distance to itself");

	// Bit 0 comes from the DC coefficient, which is excluded, so it is free to mark a declined hash.
	// Typed rather than 0ull: under LP64 unsigned long long is distinct from uint64_t, so the
	// assert_equal overload set has no exact match.
	assert_equal(uint64_t{0}, hash & uint64_t{1}, "a real hash never sets the reserved bit");
	assert_equal(false, crypto::phash_is_usable(crypto::phash_declined), "the declined marker is not a hash");
	assert_equal(false, crypto::phash_is_usable(0), "not computed is not a hash");

	// Brightness and contrast move every pixel but not the picture, which is what a re-encode does.
	auto brightened = field;
	for (auto& v : brightened) v = static_cast<uint8_t>(std::min(255, v + 20));
	assert_equal(true, crypto::phash_distance(hash, crypto::perceptual_hash(brightened.data(), brightened.size())) <= 6,
	             "brightness does not change the picture");

	// A different picture has to land far away, or the threshold means nothing.
	const auto other = make_phash_field(9);
	assert_equal(true, crypto::phash_distance(hash, crypto::perceptual_hash(other.data(), other.size())) > 6,
	             "a different picture is far away");

	// Flat fields are where a 64-bit hash quietly starts matching everything.
	std::array<uint8_t, crypto::phash_pixels> blank{};
	blank.fill(128);
	assert_equal(false, crypto::phash_is_usable(crypto::perceptual_hash(blank.data(), blank.size())),
	             "a blank image has no opinion");

	std::array<uint8_t, crypto::phash_pixels> almost_blank{};
	almost_blank.fill(128);
	almost_blank[0] = 129;
	assert_equal(false, crypto::phash_is_usable(crypto::perceptual_hash(almost_blank.data(), almost_blank.size())),
	             "one different pixel is not detail");

	assert_equal(0ull, crypto::perceptual_hash(nullptr, crypto::phash_pixels), "no pixels");
	assert_equal(0ull, crypto::perceptual_hash(field.data(), crypto::phash_pixels - 1), "short buffer");
}

static uint64_t phash_of_file(const std::string_view name)
{
	files ff;
	file_read_stream stream;
	if (!stream.open(test_files_folder.combine_file(name))) return 0;

	df::blob owner;
	return ff.calc_perceptual_hash(stream.view_all(owner));
}

static crypto::phash_rotations phash_rotations_of_file(const std::string_view name)
{
	files ff;
	file_read_stream stream;
	if (!stream.open(test_files_folder.combine_file(name))) return {};

	df::blob owner;
	return ff.calc_perceptual_hash_rotations(stream.view_all(owner));
}

// The point of the hash is the case a checksum cannot see: the same picture in a different file.
static void should_recognise_the_same_picture()
{
	const auto original = phash_of_file("Test.jpg");
	const auto resized = phash_of_file("Small.jpg");

	assert_equal(true, crypto::phash_is_usable(original), "Test.jpg hashes");
	assert_equal(true, crypto::phash_is_usable(resized), "Small.jpg hashes");

	// Measured separation on these fixtures: 0 for the resize, 30 for a rotation, 32 for an
	// unrelated photo. The threshold sits in that gap rather than near either side of it.
	assert_equal(0, crypto::phash_distance(original, resized), "a resized copy is the same picture");

	// A rotation is a different bitmap, so the single-orientation hash must still land far away: the
	// rotations are what recognise it, and nothing else should quietly start matching.
	assert_equal(true, crypto::phash_distance(original, phash_of_file("Test90.jpg")) > 20,
	             "a rotated copy is not the same bitmap");

	const auto unrelated = phash_of_file("IMG_0096.JPG");
	assert_equal(true, crypto::phash_is_usable(unrelated), "an unrelated photo hashes");
	assert_equal(true, crypto::phash_distance(original, unrelated) > 20, "an unrelated photo is far away");
}

// A quarter turn is a common grading step, so the same picture rotated has to be recognised as a
// copy - while an unrelated photo stays clear in every orientation, not just the one it was saved in.
static void should_recognise_a_rotated_picture()
{
	const auto original = phash_of_file("Test.jpg");

	assert_equal(true, crypto::phash_is_usable(original), "Test.jpg hashes");

	// Every quarter turn of the same photograph, and a losslessly rotated pair from a second source.
	for (const auto name : {"Test90.jpg"sv, "Test180.jpg"sv, "Test270.jpg"sv})
	{
		const auto rotated = phash_rotations_of_file(name);
		assert_equal(true, crypto::phash_is_usable(rotated[0]), "a turned copy hashes");
		assert_equal(true, crypto::phash_distance(original, rotated) <= 6, "a turned copy is the same picture");
	}

	const auto lossless = phash_of_file("Lossless0.jpg");
	assert_equal(true, crypto::phash_is_usable(lossless), "Lossless0.jpg hashes");
	assert_equal(true, crypto::phash_distance(lossless, phash_rotations_of_file("Lossless90.jpg")) <= 6,
	             "a losslessly rotated copy matches");

	// Whichever side carries the turns, the pair has to meet: presence compares the outside file's
	// stored hash against an indexed picture's turns, and duplicate search does the reverse.
	assert_equal(true, crypto::phash_distance(phash_of_file("Test90.jpg"), phash_rotations_of_file("Test.jpg")) <= 6,
	             "the turned file recognises the upright original");
	assert_equal(true, crypto::phash_is_usable(phash_rotations_of_file("Test.jpg")[0]),
	             "the upright original keeps a usable set of turns");

	// The rotations must not become a way for anything to match anything.
	assert_equal(true, crypto::phash_distance(original, phash_rotations_of_file("IMG_0096.JPG")) > 6,
	             "an unrelated photo stays clear in every orientation");
	assert_equal(true, crypto::phash_distance(original, phash_rotations_of_file("Lossless0.jpg")) > 6,
	             "a second unrelated photo stays clear in every orientation");

	// A resize still matches without needing a turn, so the plain comparison is not weakened.
	assert_equal(0, crypto::phash_distance(original, phash_rotations_of_file("Small.jpg")),
	             "a resized copy still matches at zero");
}

static void should_convert_utf8()
{
	// icon font
	wchar_t stars_utf16[6] = {};

	for (auto i = 0; i < 5; i++)
	{
		stars_utf16[i] = static_cast<uint16_t>(i & 0x01 ? icon_index::star_solid : icon_index::star);
	}
	stars_utf16[5] = 0;

	const auto stars = platform::utf16_to_utf8(stars_utf16);

	std::string_view strings[] = {
		"In vollen Zügen genießen",
		"Nældens takvinge",
		"💉💎👦🏻👓⚡",
		"Žižkov",
		"Доброго ранку!",
		"Japanese こんにちは世界",
		"Arabic مرحبا العالم",
		stars
	};

	for (const auto src : strings)
	{
		assert_equal(src, platform::utf16_to_utf8(platform::utf8_to_utf16(src)), "platform conversions");
		assert_equal(platform::utf8_to_utf16(src), str::utf8_to_utf16(src), "to utf16");
		assert_equal(src, str::utf16_to_utf8(platform::utf8_to_utf16(src)), "to utf8");
		assert_equal(src, str::utf16_to_utf8(str::utf8_to_utf16(src)), "internal conversions");
	}

	constexpr wchar_t icon_text[2] = {static_cast<wchar_t>(icon_index::fit), 0};
	const auto icon_text_converted = str::utf8_to_utf16(str::utf16_to_utf8(icon_text));
	assert_equal(icon_text, icon_text_converted, "icon to utf8");

	// Verify icon_to_utf8 matches the old wchar_t + utf16_to_utf8 approach
	const auto icon_new = icon_to_utf8(icon_index::fit);
	constexpr wchar_t icon_old_text[2] = {static_cast<wchar_t>(icon_index::fit), 0};
	const auto icon_old = str::utf16_to_utf8(icon_old_text);
	assert_equal(icon_old, icon_new, "icon_to_utf8 matches old approach");
	assert_equal(true, icon_index::rotate_clockwise != icon_index::rotate_anticlockwise,
	             "the two rotations are different glyphs");

	// Verify char32_to_utf8 round-trips for icon code points
	std::string char32_result;
	str::char32_to_utf8(std::back_inserter(char32_result), static_cast<uint32_t>(icon_index::fit) & 0xFFFF);
	assert_equal(icon_old, char32_result, "char32_to_utf8 for icon");

	// Verify every icon in the icon_index enum round-trips correctly through UTF-8
	constexpr icon_index all_icons[] = {
		icon_index::add, icon_index::remove, icon_index::audio, icon_index::camera,
		icon_index::cancel, icon_index::check, icon_index::del, icon_index::edit,
		icon_index::folder, icon_index::search, icon_index::star, icon_index::star_solid,
		icon_index::play, icon_index::pause, icon_index::stop, icon_index::copyright,
		icon_index::photo, icon_index::video, icon_index::settings, icon_index::save,
		icon_index::rotate_clockwise, icon_index::rotate_anticlockwise,
		icon_index::fit, icon_index::zoom_in, icon_index::zoom_out,
	};

	for (const auto icon : all_icons)
	{
		const auto icon_val = static_cast<uint32_t>(icon) & 0xFFFF;
		const wchar_t expected_utf16[2] = {static_cast<wchar_t>(icon_val), 0};
		const auto expected_utf8 = platform::utf16_to_utf8(expected_utf16);

		// icon_to_utf8 should produce correct UTF-8
		const auto actual_utf8 = icon_to_utf8(icon);
		assert_equal(expected_utf8, actual_utf8,
		             std::format("icon_to_utf8 for 0x{:X}", static_cast<uint32_t>(icon)));

		// UTF-8 should be exactly 3 bytes for BMP icons >= 0x800
		if (icon_val >= 0x800)
		{
			assert_equal(3, static_cast<int>(actual_utf8.size()),
			             std::format("icon UTF-8 byte count for 0x{:X}", icon_val));
		}

		// Round-trip: UTF-8 -> UTF-16 should recover the original code point
		const auto round_tripped_utf16 = str::utf8_to_utf16(actual_utf8);
		assert_equal(1, static_cast<int>(round_tripped_utf16.size()),
		             std::format("icon round-trip UTF-16 length for 0x{:X}", icon_val));
		assert_equal(static_cast<int>(icon_val), round_tripped_utf16[0],
		             std::format("icon round-trip code point for 0x{:X}", icon_val));

		// Also verify str:: matches platform:: conversion
		const auto platform_utf16 = platform::utf8_to_utf16(actual_utf8);
		assert_equal(platform_utf16, round_tripped_utf16,
		             std::format("icon str vs platform utf8_to_utf16 for 0x{:X}", icon_val));
	}
}

static void should_split()
{
	constexpr auto to_be_split = "H:\\2-Archief VIDEO privé\\Eigen video's\nF:\\1-Archief FOTOGRAFIE privé";
	const auto parts = str::split(to_be_split, false, [](const char c) { return c == '\n' || c == '\r'; });

	assert_equal("H:\\2-Archief VIDEO privé\\Eigen video's", parts[0], "Split 1");
	assert_equal("F:\\1-Archief FOTOGRAFIE privé", parts[1], "Split 2");

	constexpr auto to_be_split2 = "aaa 'bbb ccc' ddd \"ee ff \"";
	const auto parts2 = str::split(to_be_split2, true);

	constexpr auto to_be_split3 = "Доброго ранку!";
	const auto parts3 = str::split(to_be_split3, true);

	assert_equal("aaa", parts2[0], "Split 1");
	assert_equal("bbb ccc", parts2[1], "Split 2");
	assert_equal("ddd", parts2[2], "Split 3");
	assert_equal("ee ff ", parts2[3], "Split 4");
	assert_equal("ранку!", parts3[1], "Split 5");

	const auto internal_quote = str::split("Jane O'Brien; Alex", true, str::is_artist_separator);
	assert_equal(2_z, internal_quote.size(), "internal apostrophe does not merge names");
	assert_equal("Jane O'Brien", internal_quote[0], "internal apostrophe is preserved");
	assert_equal(" Alex", internal_quote[1], "separator policy still owns surrounding spaces");

	const auto quoted_separators = str::split("'Jane; O\\'Brien';\"Alex, Pat\";Sam", true,
	                                          str::is_artist_separator);
	assert_equal(3_z, quoted_separators.size(), "outer quotes protect separators");
	assert_equal("Jane; O\\'Brien", quoted_separators[0], "single-quoted separator");
	assert_equal("Alex, Pat", quoted_separators[1], "double-quoted separator");
	assert_equal("Sam", quoted_separators[2], "unquoted tail");

	const auto spaced_quote = str::split("Jane; \"Smith; Pat\"", true, str::is_artist_separator);
	assert_equal(2_z, spaced_quote.size(), "quote after separator whitespace wraps");
	assert_equal("Smith; Pat", spaced_quote[1], "separator whitespace is outside the wrapping quote");

	const auto quoted_prefix = str::split("\"Weird Al\" Yankovic; Madonna", true, str::is_artist_separator);
	assert_equal(2_z, quoted_prefix.size(), "quoted prefix does not swallow artist separator");
	assert_equal("\"Weird Al\" Yankovic", quoted_prefix[0], "quoted prefix stays literal");
	assert_equal(" Madonna", quoted_prefix[1], "quoted prefix tail splits");

	const auto quote_with_space_before_separator = str::split("\"Doe, Jane\" ; Alex", true, str::is_artist_separator);
	assert_equal(2_z, quote_with_space_before_separator.size(), "space before separator keeps wrapping quote");
	assert_equal("Doe, Jane", quote_with_space_before_separator[0], "wrapped token before spaced separator");
	assert_equal(" Alex", quote_with_space_before_separator[1], "spaced separator tail splits");

	const auto quote_with_trailing_space = str::split("\"Doe, Jane\" ", true, str::is_artist_separator);
	assert_equal(1_z, quote_with_trailing_space.size(), "trailing whitespace keeps wrapping quote");
	assert_equal("Doe, Jane", quote_with_trailing_space[0], "trailing whitespace outside quote");

	// A tab separates artists and genres while a space does not; skipping whitespace after a closing
	// quote must stop at that separator rather than carry on past it.
	for (const auto& pred : {std::function<bool(char)>(str::is_artist_separator),
	                         std::function<bool(char)>(str::is_genre_separator)})
	{
		const auto quote_space_tab = str::split("\"Doe, Jane\" \tAlex", true, pred);
		assert_equal(2_z, quote_space_tab.size(), "space then tab after a closing quote ends the token");
		assert_equal("Doe, Jane", quote_space_tab[0], "quoted token before a space and tab separator");
		assert_equal("Alex", quote_space_tab[1], "token after a space and tab separator");
	}

	const auto quoted_first_word = str::split("Jane Doe; 'Bud' Abbott; Lou Costello", true,
	                                         str::is_artist_separator);
	assert_equal(3_z, quoted_first_word.size(), "quoted first word does not swallow later separators");
	assert_equal(" 'Bud' Abbott", quoted_first_word[1], "quoted first word stays literal");
	assert_equal(" Lou Costello", quoted_first_word[2], "tail after quoted first word splits");

	const auto unmatched_leading_quote = str::split("Alex; 'Til Tuesday; Bob", true, str::is_artist_separator);
	assert_equal(3_z, unmatched_leading_quote.size(), "unmatched leading quote does not swallow later separators");
	assert_equal(" 'Til Tuesday", unmatched_leading_quote[1], "unmatched leading quote stays literal");
	assert_equal(" Bob", unmatched_leading_quote[2], "tail after unmatched quote splits");

	const auto collection_roots = str::split("\"C:\\Photos\"\n\"D:\\\"", true,
	                                         [](const char c) { return c == '\n' || c == '\r'; });
	assert_equal(2_z, collection_roots.size(), "quoted collection roots split");
	assert_equal("C:\\Photos", collection_roots[0], "quoted collection root");
	assert_equal("D:\\", collection_roots[1], "quoted drive root keeps trailing slash");

	const auto command_line_drive = str::split("\"D:\\\" -no-gpu", true, str::is_white_space);
	assert_equal(2_z, command_line_drive.size(), "quoted drive-root argument splits from switch");
	assert_equal("D:\\", command_line_drive[0], "quoted drive-root argument");
	assert_equal("-no-gpu", command_line_drive[1], "switch after quoted drive-root");

	const auto command_line_trailing_slash = str::split("\"C:\\My Photos\\\" -no-gpu", true, str::is_white_space);
	assert_equal(2_z, command_line_trailing_slash.size(), "quoted trailing-slash path splits from switch");
	assert_equal("C:\\My Photos\\", command_line_trailing_slash[0], "quoted trailing-slash path");
	assert_equal("-no-gpu", command_line_trailing_slash[1], "switch after quoted trailing-slash path");

	std::string long_escaped_artist;
	constexpr auto escaped_artist_count = 50'000;
	for (auto i = 0; i < escaped_artist_count; ++i)
	{
		if (i != 0) long_escaped_artist += '\\';
		long_escaped_artist += "'a";
	}
	const auto long_escaped_artist_parts = str::split(long_escaped_artist, true, str::is_artist_separator);
	assert_equal(static_cast<size_t>(escaped_artist_count), long_escaped_artist_parts.size(),
	             "long escaped-quote artist text splits");
	assert_equal("'a", long_escaped_artist_parts.front(), "long escaped-quote first token");
	assert_equal("'a", long_escaped_artist_parts.back(), "long escaped-quote last token");
	for (const auto part : long_escaped_artist_parts)
	{
		assert_equal("'a", part, "long escaped-quote token");
	}

	// The same chain ending in a quote that cannot close: every token-start quote stops at that final
	// quote, which the scan caches instead of rescanning to it once per token (linearity was verified
	// separately; this asserts the tokens).
	const auto stopped_chain = long_escaped_artist + "'b";
	const auto stopped_chain_parts = str::split(stopped_chain, true, str::is_artist_separator);
	assert_equal(static_cast<size_t>(escaped_artist_count), stopped_chain_parts.size(),
	             "escaped-quote chain ending in an unclosed quote splits");
	assert_equal("'a", stopped_chain_parts.front(), "stopped chain first token");
	assert_equal("'a'b", stopped_chain_parts.back(), "stopped chain keeps the unclosed quote literal");

	std::string long_blank = "A;";
	long_blank.append(200'000, ' ');
	long_blank += 'B';
	const auto long_blank_parts = str::split(long_blank, true, str::is_artist_separator);
	assert_equal(2_z, long_blank_parts.size(), "long whitespace-prefix token splits");
	assert_equal("A", long_blank_parts[0], "long whitespace first token");
	assert_equal(200'001_z, long_blank_parts[1].size(), "long whitespace token length");
	assert_equal(' ', long_blank_parts[1].front(), "long whitespace token starts with space");
	assert_equal('B', long_blank_parts[1].back(), "long whitespace token keeps tail");

	const auto mixed_quotes = str::split("\"Jane 'JJ' Doe\" 'Alex \"Ace\" Roe' \"unmatched", true);
	assert_equal(3_z, mixed_quotes.size(), "mixed quote characters split into tokens");
	assert_equal("Jane 'JJ' Doe", mixed_quotes[0], "inner single quotes preserved");
	assert_equal("Alex \"Ace\" Roe", mixed_quotes[1], "inner double quotes preserved");
	assert_equal("\"unmatched", mixed_quotes[2], "unmatched wrapping quote stays literal");

	// Random data checking for crashes
	std::string_view strings[] = {
		"In vollen Zügen genießen",
		"Nældens takvinge",
		"Žižkov",
		"Доброго ранку!",
		"Japanese こんにちは世界",
		"Arabic مرحبا العالم",
		"Доброго ранку!",
		"\"'",
		"\" \" \"",
		"''''",
		"aaa'bb  bbb'aa",
		"aaa\0\0\'",
		"\r\t\naaaa\" aaa bbb",
		"'\t \n abc",
		"'",
	};

	for (const auto& src : strings)
	{
		str::split_count(src, true);
	}
}

static void should_split_genre()
{
	// Genre values use ';' as the multi-value separator. Multi-word genres and
	// genres containing '&' or '/' must survive splitting intact.
	const auto parts = str::split("Rock; Pop ; Hip Hop", false, str::is_genre_separator);
	assert_equal(size_t{3}, parts.size(), "genre part count");
	assert_equal("Rock", str::trim(parts[0]), "genre 1");
	assert_equal("Pop", str::trim(parts[1]), "genre 2");
	assert_equal("Hip Hop", str::trim(parts[2]), "genre 3");

	const auto parts2 = str::split("Action & Adventure; R&B/Soul", false, str::is_genre_separator);
	assert_equal(size_t{2}, parts2.size(), "genre part count 2");
	assert_equal("Action & Adventure", str::trim(parts2[0]), "genre with ampersand");
	assert_equal("R&B/Soul", str::trim(parts2[1]), "genre with slash");

	const auto parts3 = str::split("Jazz", false, str::is_genre_separator);
	assert_equal(size_t{1}, parts3.size(), "single genre part count");
	assert_equal("Jazz", str::trim(parts3[0]), "single genre");
}

static void should_parse_dates()
{
	const auto parsed = [](const std::string_view text)
	{
		df::date_t d;
		return d.parse(text) ? d.date() : df::day_t{0};
	};

	assert_equal(2006, parsed("2006-01-14 15:51:31").year, "an ISO date parses");
	assert_equal(31, parsed("2006-01-14 15:51:31").second, "including its seconds");
	assert_equal(2006, parsed("2006:01:14 15:51:31").year, "an EXIF date parses");
	assert_equal(13, parsed("2011-10-03T02:59:13.000000Z").second, "fractional seconds truncate to the second");

	// The seconds field is read with %lg, which accepts "inf", "nan" and an overflowing exponent.
	// Converting any of those to int is undefined, so they fail the parse instead of reaching it.
	df::date_t d;
	assert_equal(false, d.parse("2006-01-14 15:51:inf"), "infinite seconds is not a date");
	assert_equal(false, d.parse("2006-01-14 15:51:nan"), "seconds that are not a number is not a date");
	assert_equal(false, d.parse("2006-01-14 15:51:1e400"), "an overflowing seconds exponent is not a date");
	assert_equal(false, d.parse("2006-01-14 15:51:-1"), "negative seconds is not a date");
	assert_equal(false, d.parse("2006-01-14 15:51:99"), "seconds past a leap second is not a date");
	assert_equal(true, d.parse("2006-01-14 15:51:60"), "a leap second still is");
}

#ifndef _WIN32
static void should_convert_local_dates_with_date_specific_rules()
{
	assert_equal(uint64_t{0}, platform::utc_to_local(0), "an absent UTC timestamp remains absent");
	assert_equal(uint64_t{0}, platform::local_to_utc(0), "an absent local timestamp remains absent");
	const std::array days = {
		df::day_t{2024, 1, 15, 12, 0, 0},
		df::day_t{2024, 7, 15, 12, 0, 0},
		df::day_t{2024, 3, 10, 3, 30, 0},
		df::day_t{2024, 11, 3, 3, 30, 0}
	};
	for (const auto& day : days)
	{
		std::tm local{};
		local.tm_year = day.year - 1900;
		local.tm_mon = day.month - 1;
		local.tm_mday = day.day;
		local.tm_hour = day.hour;
		local.tm_min = day.minute;
		local.tm_isdst = -1;
		const auto unix_time = std::mktime(&local);
		constexpr uint64_t fraction = 1234567;
		const auto expected = (static_cast<int64_t>(unix_time) + 11644473600ll) * 10000000ll + fraction;
		const auto wall = df::date_t(day.year, day.month, day.day, day.hour, day.minute, 0).to_int64() + fraction;
		assert_equal(expected, platform::local_to_utc(wall), "local conversion matches the date-specific timezone rule");
	}
}
#endif

static void should_extract_url()
{
	constexpr auto input1 = "Visit my website at https://www.example.com for more info.";
	constexpr auto input2 = "Check out this article: http://anotherexample.org/article";
	constexpr auto input3 = "No URLs here.";
	constexpr auto input4 =
		"Quite nice  <a href=\"http://bighugelabs.com/flickr/onblack.php?id=1397504988\"> On Black</a>";

	assert_equal("https://www.example.com", df::url_extract(input1), "extract url");
	assert_equal("http://anotherexample.org/article", df::url_extract(input2), "extract url");
	assert_equal("", df::url_extract(input3), "extract url");
	assert_equal("", df::url_extract(input3), "extract url");
	assert_equal("http://bighugelabs.com/flickr/onblack.php?id=1397504988", df::url_extract(input4),
	             "extract url");

	// A description panel offering a choice of links needs every distinct one, in reading order.
	const auto all = df::url_extract_all(
		"See https://example.com/a and https://example.com/b then https://example.com/a again.");
	assert_equal(size_t{2}, all.size(), "repeated url listed once");
	assert_equal("https://example.com/a", all[0], "first url in source order");
	assert_equal("https://example.com/b", all[1], "second url in source order");
	assert_equal(size_t{0}, df::url_extract_all(input3).size(), "no urls found");
}

static void should_match_wildcard()
{
	assert_equal(true, str::wildcard_icmp("", ""));
	assert_equal(true, str::wildcard_icmp("", "*"));
	assert_equal(true, str::wildcard_icmp(" ", "*"));
	assert_equal(true, str::wildcard_icmp(" ", " *"));
	assert_equal(false, str::wildcard_icmp(" ", "  *"));

	assert_equal(true, str::wildcard_icmp("hello world", "hello world"));
	assert_equal(true, str::wildcard_icmp("hello ?! world", "hello * world"));
	assert_equal(true, str::wildcard_icmp("hello-xx-world", "hello*world"));
	assert_equal(false, str::wildcard_icmp("hello-xx-world", "hello *world"));
	assert_equal(true, str::wildcard_icmp("hello-xx-world", "*world"));
	assert_equal(true, str::wildcard_icmp("hello-xx-world", "hello*"));

	assert_equal(true, str::wildcard_icmp("HELLO-XX-WORLD", "hello*"));
	assert_equal(true, str::wildcard_icmp("HELLO-XX-WORLD", "hello*world"));


	assert_equal(0, str::icmp("ДОБРОГО РАНКУ", "Доброго ранку"));
	assert_equal(0, str::icmp("ARABIC مرحبا العالم", "Arabic مرحبا العالم"));
	assert_equal(0, str::icmp("JAPANESE こんにちは世界", "Japanese こんにちは世界"));
	assert_equal(0, str::icmp("💉💎👦🏻👓⚡", "💉💎👦🏻👓⚡"));

	assert_equal(true, str::wildcard_icmp("Доброго ранку", "Доброго*"));
	assert_equal(true, str::wildcard_icmp("ДОБРОГО РАНКУ", "Доброго*"));
	assert_equal(true, str::wildcard_icmp("ДОБРОГО РАНКУ", "*ранку"));
	assert_equal(true, str::wildcard_icmp("💉💎👦🏻👓⚡", "*💎*"));
	assert_equal(true, str::wildcard_icmp("💉💎👦🏻👓⚡", "💉*"));
	assert_equal(true, str::wildcard_icmp("Οδός", "οδ*"));
	assert_equal(false, str::wildcard_icmp("Οδός", "sd*"));
}

static void should_detect_wildcard()
{
	assert_equal(false, str::is_wildcard(""));
	assert_equal(false, str::is_wildcard("abcdef"));
	assert_equal(true, str::is_wildcard("abc*"));
	assert_equal(false, str::is_wildcard("abc\\*"));
	assert_equal(false, str::is_wildcard("abc\\*ef"));
}

static void should_parse_command_line()
{
	command_line_t cl1;
	cl1.parse("-no-gpu");

	assert_equal(true, cl1.no_gpu, "no_gpu");
	assert_equal(false, cl1.no_indexing, "no_indexing");

	command_line_t cl2;
	cl2.parse(test_files_folder.text());

	assert_equal(false, cl2.folder_path.is_empty(), "folder_path");
	assert_equal(std::string_view{}, cl2.selection.name(), "selection name");
	assert_equal(test_files_folder.text(), cl2.folder_path.folder().text(), "folder path");
	assert_equal(false, cl2.no_gpu, "no_gpu");
	assert_equal(false, cl2.no_indexing, "no_indexing");


	// The fixture's own spelling: a case-folded name only resolves on a case-insensitive volume.
	const auto path3 = test_files_folder.combine_file("Test.jpg");
	command_line_t cl3;
	cl3.parse(std::format("{} -no-indexing", path3));

	assert_equal(path3.folder().text(), cl3.folder_path.folder().text(), "folder_path");
	assert_equal(path3.name(), cl3.selection.name(), "selection name");
	assert_equal(path3.folder().text(), cl3.selection.folder().text(), "selection folder");
	assert_equal(false, cl3.no_gpu, "no_gpu");
	assert_equal(true, cl3.no_indexing, "no_indexing");

	// The subject is that quotes hold a spaced path together as one part, so the folder is made
	// here rather than borrowed from whatever the platform happens to install.
	temp_files temps;
	const auto spaced = temps.next_folder("program files");

	command_line_t cl4;
	cl4.parse(std::format("--no-gpu \"{}\"", spaced));
	assert_equal(true, cl4.no_gpu, "no_gpu");
	assert_equal(false, cl4.folder_path.is_empty(), "folder_path with a space");

	command_line_t cl5;
	cl5.parse("----- --no-gpu");
	assert_equal(true, cl5.no_gpu, "no_gpu");

	const auto root_path = df::windows_path_semantics
		                       ? std::string(test_files_folder.text().sv().substr(0, 3))
		                       : "/"s;
	command_line_t cl_drive_root;
	cl_drive_root.parse(std::format("\"{}\" -no-gpu", root_path));
	assert_equal(true, cl_drive_root.no_gpu, "quoted drive root no_gpu");
	assert_equal(false, cl_drive_root.folder_path.is_empty(), "quoted drive root folder path");

	const auto trailing_slash = std::string(spaced.text().sv()) + "\\";
	command_line_t cl_trailing_slash;
	cl_trailing_slash.parse(std::format("\"{}\" -no-gpu", trailing_slash));
	assert_equal(true, cl_trailing_slash.no_gpu, "quoted trailing slash no_gpu");
	assert_equal(false, cl_trailing_slash.folder_path.is_empty(), "quoted trailing slash folder path");

	command_line_t cl6;
	cl6.parse("-no-gpu -no-indexing");
	assert_equal(true, cl6.no_gpu, "multiple options no_gpu");
	assert_equal(true, cl6.no_indexing, "multiple options no_indexing");

#ifdef _DEBUG
	command_line_t cl7;
	cl7.parse("-screenshot:edit \"-screenshot-output:C:\\temp\\edit.png\"");
	assert_equal("edit"sv, cl7.screenshot_scene, "screenshot scene");
	assert_equal("C:\\temp\\edit.png"sv, cl7.screenshot_output, "screenshot output");

	command_line_t cl8;
	cl8.parse("-test-reset-graphics");
	assert_equal("reset-graphics"sv, cl8.test_action, "test action");
#endif

	command_line_t cl9;
	cl9.parse("-run-tests");
	assert_equal(true, cl9.console_test, "run-tests alias");

	command_line_t cl_movie_check;
	cl_movie_check.parse(
		"-movie-check:C:\\movies -movie-media:D:\\media \"-movie-out:E:\\rendered movies\" --movie-overwrite");
	assert_equal("C:\\movies"sv, cl_movie_check.movie_batch.projects, "movie projects");
	assert_equal(true, cl_movie_check.movie_batch.mode == movie_batch_mode::check, "a check only reports");
	assert_equal("D:\\media"sv, cl_movie_check.movie_batch.media, "movie media folder");
	assert_equal("E:\\rendered movies"sv, cl_movie_check.movie_batch.output, "a quoted output folder keeps its space");
	assert_equal(true, cl_movie_check.movie_batch.overwrite, "movie overwrite");

	command_line_t cl_movie_import;
	cl_movie_import.parse("-movie-import:C:\\movies\\trip.wlmp");
	assert_equal("C:\\movies\\trip.wlmp"sv, cl_movie_import.movie_batch.projects, "a project to import");
	assert_equal(true, cl_movie_import.movie_batch.mode == movie_batch_mode::import, "import imports");

	command_line_t cl_movie_render;
	cl_movie_render.parse("-MOVIE-RENDER:C:\\movies\\trip.otio -movie-skip-missing");
	assert_equal("C:\\movies\\trip.otio"sv, cl_movie_render.movie_batch.projects, "a single movie project");
	assert_equal(true, cl_movie_render.movie_batch.mode == movie_batch_mode::render, "render renders");
	assert_equal(false, cl_movie_render.movie_batch.overwrite, "and replaces nothing unless asked");
	assert_equal(true, cl_movie_render.movie_batch.skip_missing, "skipping missing sources is asked for");
	assert_equal(false, cl_movie_check.movie_batch.skip_missing, "and never assumed");

	command_line_t cl_no_movie;
	cl_no_movie.parse("-movie-check: -no-gpu");
	assert_equal(true, cl_no_movie.movie_batch.projects.empty(), "a movie switch with no path starts no batch");

	// AutoPlay passes the inserted volume with "**", the address box's spelling for its subfolders.
	const auto recursive_text = std::string(test_files_folder.text()) + df::preferred_path_sep + "**";
	command_line_t cl_recursive;
	cl_recursive.parse(std::format("\"{}\"", recursive_text));
	assert_equal(true, cl_recursive.folder_path.is_recursive(), "a trailing ** opens the subfolders too");
	assert_equal(test_files_folder.text(), cl_recursive.folder_path.folder().text(), "of the named folder");

	command_line_t cl_recursive_root;
	cl_recursive_root.parse(std::format("\"{}**\" -no-gpu", root_path));
	assert_equal(true, cl_recursive_root.folder_path.is_recursive(), "a drive root with ** is recursive");
	assert_equal(true, cl_recursive_root.no_gpu, "and the rest of the line still parses");

	command_line_t cl_missing_recursive;
	cl_missing_recursive.parse(std::format("\"{}\"", std::string(spaced.text()) + "-missing" + df::preferred_path_sep + "**"));
	assert_equal(true, cl_missing_recursive.folder_path.is_empty(), "a folder that does not exist opens nothing");

	command_line_t cl_stars_only;
	cl_stars_only.parse("** -no-gpu");
	assert_equal(true, cl_stars_only.folder_path.is_empty(), "stars alone name no folder");
	assert_equal(true, cl_stars_only.no_gpu, "and do not stop the rest of the line");

	if constexpr (df::windows_path_semantics)
	{
		command_line_t cl_bare_drive;
		cl_bare_drive.parse(root_path.substr(0, 2) + "**");
		assert_equal(true, cl_bare_drive.folder_path.is_recursive(), "a drive named without its separator");
		assert_equal(std::string_view(root_path), cl_bare_drive.folder_path.folder().text().sv(),
		             "is that drive's root");
	}
}

static void should_trim_strings()
{
	assert_equal("xxx", str::trim_and_cache("xxx\n"), "remove cr lf");
	assert_equal("xxx", str::trim_and_cache("\rxxx\r"), "remove lf");
	assert_equal("xxx", str::trim_and_cache("   xxx\t\t "), "remove space");
}

static void should_format_text()
{
	assert_equal_strict("ac-dc", std::format("{2}{0}-{1}{0}", "c", "d", "a"), "order");
	assert_equal_strict("0.00123", std::format("{}", 0.00123), "double");
	assert_equal_strict("0.001", std::format("{:.3f}", 0.00123), "double");
	assert_equal_strict("5.5", std::format("{}", 5.5000), "double");
	assert_equal_strict("123", std::format("{}", 123), "int");
	assert_equal_strict("0123", std::format("{:04}", 123), "int");
	assert_equal_strict(" 123", std::format("{:4}", 123), "int");
	assert_equal_strict("hex=7b", std::format("hex={:x}", 0x7B), "hex");
	assert_equal_strict("-test-", std::format("-{}-", "test"), "char*");
	assert_equal_strict("-test-", std::format("-{}-", std::string("test")), "string");
	assert_equal_strict("-test-", std::format("-{}-", std::string_view("test")), "string_view");
	assert_equal_strict("33 {} {test}", std::format("{} {{}} {{test}}", 33), "string_view");
	assert_equal_strict("22 x 33", std::format("{} x {}", 22, 33), "string_view");
	assert_equal_strict("0", str::to_hex(uint32_t{0}), "zero hex has one digit");
	assert_equal_strict("10", str::to_hex(uint32_t{0x10}), "interior zero nibble preserved");
	assert_equal_strict("1001", str::to_hex(uint32_t{0x1001}), "interior zero byte preserved");
	assert_equal_strict("100000000", str::to_hex(uint64_t{0x100000000ull}), "64-bit scalar width");
	assert_equal_strict("F0", str::to_hex(uint32_t{0xF0}), "trailing zero nibble preserved");
	assert_equal(0x1001u, str::hex_to_num(str::to_hex(uint32_t{0x1001})), "scalar hex round-trips");
	const uint8_t fixed_width[] = {0x00, 0x10};
	assert_equal_strict("0010", str::to_hex(fixed_width, 2, false, false), "fixed-width hex is unchanged");
}

static void should_reject_invalid_assertion_inputs()
{
	assert_assertion_fails([] { assert_near(1.0, std::numeric_limits<double>::quiet_NaN(), 0.1); },
	                       "NaN actual is rejected");
	assert_assertion_fails([] { assert_near(1.0, 1.0, std::numeric_limits<double>::quiet_NaN()); },
	                       "NaN tolerance is rejected");
	assert_assertion_fails([] { assert_equal(df::date_t(100), df::date_t(110)); },
	                       "distinct invalid dates compare by value");
}

// str::to_string(double) used a 128-byte buffer, and _fcvt_s ends the process - through the CRT's
// invalid-parameter fail-fast, which no crash report records - when the fixed form of the value does
// not fit, from about 1e120 up. A RAW file's maker notes can state a value that large. Restoring the
// short buffer makes this test terminate the run instead of failing an assertion.
static void should_format_a_double_of_any_magnitude()
{
	const auto huge = str::to_string(1e300, 3);
	assert_equal(305_z, huge.size(), "301 integer digits, the point and three decimals");
	assert_equal(true, huge.starts_with('1'), "led by the value's first digit");
	assert_equal(true, huge.ends_with(".000"), "and closed by the decimals asked for");

	const auto lowest = str::to_string(std::numeric_limits<double>::lowest(), 2);
	assert_equal(313_z, lowest.size(), "the sign, all 309 integer digits, the point and two decimals");
	assert_equal(true, lowest.starts_with("-17976931348623157"), "the most negative double formats whole");

	assert_equal("2.5", str::to_string(2.5, -1), "an ordinary value is unchanged");
}

static std::string find_and_format_result(const std::string_view text, const std::string_view sub_string)
{
	const auto r = str::ifind2(text, sub_string, 0);
	auto result = std::string(text);

	if (r.found)
	{
		for (auto i = static_cast<int>(r.parts.size()) - 1; i >= 0; --i)
		{
			const auto part = r.parts[i];
			result.insert(part.offset + part.length, 1, '*');
			result.insert(part.offset, 1, '*');
		}
	}

	return result;
}

static void should_find_text()
{
	assert_equal("*white* on blond", find_and_format_result("white on blond", "white"));
	assert_equal("*whi*te on *bl*ond", find_and_format_result("white on blond", "whi bl"));
	assert_equal("*white* on *blond*", find_and_format_result("white on blond", "white blond"));
	assert_equal("*white* bl on *blond*", find_and_format_result("white bl on blond", "white blond"));

	// Offsets are byte positions of a character start - a match following multi-byte characters
	// must not land on a continuation byte, or the renderer drops the highlight.
	const auto cyrillic = str::ifind2("\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "\xd0\xb2\xd0\xb5", 0);
	assert_equal(true, cyrillic.found, "cyrillic match found");
	assert_equal(6, static_cast<int>(cyrillic.parts[0].offset), "cyrillic match byte offset");

	assert_equal(true, str::ifind2("Οδός", "οδ", 0).found, "Greek case folds consistently");
	assert_equal(true, str::ifind2("crème brûlée", "creme brulee", 0).found,
	             "Latin accent folding remains intentional");
	assert_equal(false, str::ifind2("Οδός", "sd", 0).found, "Greek omicron is not ASCII s");
}

static void should_compare_versions()
{
	const df::version current_version(s_app_version);
	assert_equal(s_app_version, current_version.to_string(), "Can parse and to_string current version");

	const df::version test_version1("123.45");
	const df::version test_version1b("123.45");
	const df::version test_version2("456.1");

	assert_equal("123.45", test_version1.to_string(), "Can parse and to_string test version 1");
	assert_equal("456.1", test_version2.to_string(), "Can parse and to_string test version 2");

	assert_equal(true, test_version1 < test_version2, "Less op version");
	assert_equal(false, test_version2 < test_version1, "Less op version");
	assert_equal(false, test_version1 == test_version2, "== op version");
	assert_equal(true, test_version1 == test_version1b, "== op version");

	assert_equal("457.1", (test_version2 + 1).to_string(), "+ op version");
}

// Verifies the append-only string interning table: one shared immutable copy per unique
// string, identity == handle equality, stability across table growth, and correct
// deduplication under concurrent interning from multiple threads.
static void should_intern_strings()
{
	const auto a = str::cache("interned-example");
	const auto b = str::cache(std::string("interned-example"));
	assert_equal(true, a == b, "same content -> same handle");
	assert_equal(true, a.id == b.id, "identity is handle equality");
	assert_equal("interned-example"s, a.str(), "round-trips content");
	assert_equal(false, str::cache("interned-example") == str::cache("interned-different"),
	             "different content -> different handle");
	assert_equal(true, str::cache(std::string_view{}).is_empty(), "empty interns to empty");

	const std::string largest(platform::memory_pool::block_size -
	                          offsetof(str::cached_string_storage_t, sz) - 1, 'x');
	assert_equal(largest.size(), str::cache(largest).size(), "largest pool record is interned");
	const std::string too_large(largest.size() + 1, 'x');
	assert_equal(true, str::cache(too_large).is_empty(), "oversized pool record is rejected");

	// Many distinct strings force table growth / rehash; handles stay valid, unique and stable.
	constexpr int n = 5000;
	std::vector<str::cached> handles;
	handles.reserve(n);
	for (auto i = 0; i < n; ++i) handles.emplace_back(str::cache(std::format("intern-word-{}", i)));

	df::hash_set<uint32_t> seen;
	for (auto i = 0; i < n; ++i)
	{
		const auto w = std::format("intern-word-{}", i);
		assert_equal(w, handles[i].str(), "content preserved after growth");
		assert_equal(true, str::cache(w) == handles[i], "re-intern returns the same handle");
		seen.insert(handles[i].id);
	}
	assert_equal(n, static_cast<int>(seen.size()), "each distinct string interned exactly once");

	// Concurrent interning of an overlapping set must still yield one handle per string.
	constexpr int thread_count = 8;
	constexpr int word_count = 500;
	std::vector<std::vector<str::cached>> per_thread(thread_count);
	std::vector<std::thread> threads;

	for (auto t = 0; t < thread_count; ++t)
	{
		threads.emplace_back([t, &per_thread]
		{
			auto& out = per_thread[t];
			out.reserve(word_count);
			for (auto i = 0; i < word_count; ++i) out.emplace_back(str::cache(std::format("shared-word-{}", i)));
		});
	}

	for (auto& th : threads) th.join();

	for (auto i = 0; i < word_count; ++i)
	{
		const auto handle = per_thread[0][i];
		for (auto t = 1; t < thread_count; ++t)
		{
			assert_equal(true, per_thread[t][i] == handle, "all threads share one interned handle");
		}
	}
}

// A query that fails is not proof the file is gone: a caller that deletes or overwrites on
// "not there" must be able to tell a removed file from one it simply could not read.
static void should_report_file_presence()
{
	const auto scratch = _temps.next_folder("file-presence");
	const auto present = scratch.combine_file("present.txt");
	{
		std::ofstream fs(platform::to_stream_path(present));
		fs << "content";
	}

	const auto found = platform::file_attributes(present);
	assert_equal(true, found.exists(), "existing file is found");
	assert_equal(false, found.confirmed_missing(), "existing file is not missing");

	const auto missing = platform::file_attributes(scratch.combine_file("missing.txt"));
	assert_equal(false, missing.exists(), "removed file does not exist");
	assert_equal(true, missing.confirmed_missing(), "removed file is confirmed missing");

	// A path under a folder that is not there is absent for the same reason, not a failure.
	const auto missing_folder = platform::file_attributes(scratch.combine("gone").combine_file("missing.txt"));
	assert_equal(true, missing_folder.confirmed_missing(), "file under a missing folder is confirmed missing");

	// An empty file must not read as absent just because it has no bytes.
	const auto empty_path = scratch.combine_file("empty.txt");
	{
		std::ofstream fs(platform::to_stream_path(empty_path));
	}
	const auto empty = platform::file_attributes(empty_path);
	assert_equal(true, empty.exists(), "empty file exists");
	assert_equal(0, static_cast<int>(empty.size), "empty file has no bytes");

	assert_equal(true, platform::file_attributes(scratch).exists(), "existing folder is found");
	assert_equal(true, platform::file_attributes(scratch.combine("gone")).confirmed_missing(),
	             "removed folder is confirmed missing");

	// Enumeration only ever reports what it found, so those records are never left unknown.
	const auto contents = platform::iterate_file_items(scratch, false);
	assert_equal(2, static_cast<int>(contents.files.size()), "both files enumerated");
	assert_equal(true, std::ranges::all_of(contents.files, [](const platform::file_info& f)
	             {
		             return f.attributes.exists();
	             }),
	             "enumerated files are found");

	assert_equal(false, platform::file_attributes_t{}.exists(), "unqueried attributes do not exist");
	assert_equal(false, platform::file_attributes_t{}.confirmed_missing(),
	             "unqueried attributes are not confirmed missing");
}

// The write used to open the destination with create, truncating whatever was there before the
// first byte was written: a short write or a full disk left a partial file where a whole one used
// to be. A zero-length write compounded it, reporting success when the file could not be opened at
// all because zero of zero bytes had been written.
static void should_save_a_blob_without_truncating_the_destination()
{
	const auto scratch = _temps.next_folder("blob-save");
	const auto path = scratch.combine_file("payload.bin");

	const df::blob whole = {1, 2, 3, 4};
	assert_equal(true, df::blob_save_to_file(whole, path), "a blob is saved");
	assert_equal(uint64_t{4}, platform::file_attributes(path).size, "and lands whole");

	const df::blob empty;
	assert_equal(true, df::blob_save_to_file(empty, path), "an empty blob replaces it");
	assert_equal(uint64_t{0}, platform::file_attributes(path).size, "with an empty file");

	// Nothing can be written here, so nothing is reported as written.
	const auto missing = scratch.combine("no-such-folder").combine_file("payload.bin");
	assert_equal(false, df::blob_save_to_file(whole, missing), "an unwritable destination fails");
	assert_equal(false, df::blob_save_to_file(empty, missing), "and does so for an empty blob too");

	// The staging file is moved into place, not left beside it.
	const auto contents = platform::iterate_file_items(scratch, true);
	assert_equal(1_z, contents.files.size(), "the save leaves only the destination behind");
}

static std::string replace_file_bytes(const df::file_path path, const std::string_view initial, const int64_t start,
                                      const int64_t replace, const std::string_view replacement)
{
	write_test_file(path, initial);
	{
		df::file file(platform::open_file(path, platform::file_open_mode::read_write));
		assert_equal(true, file.insert(std::bit_cast<const uint8_t*>(replacement.data()),
		                               static_cast<int64_t>(replacement.size()), start, replace),
		             "replacement succeeds");
	}
	return read_test_file(path);
}

static void should_replace_bytes_in_files()
{
	const auto scratch = _temps.next_folder("file-insert");
	const auto path = scratch.combine_file("payload.bin");

	assert_equal("0123X789", replace_file_bytes(path, "0123456789", 4, 3, "X"),
	             "shrinking keeps prefix and tail");
	assert_equal("0123ABCDE789", replace_file_bytes(path, "0123456789", 4, 3, "ABCDE"),
	             "growing keeps prefix and tail");
	assert_equal("0123XYZ789", replace_file_bytes(path, "0123456789", 4, 3, "XYZ"),
	             "equal-size replacement");
	assert_equal("X3456789", replace_file_bytes(path, "0123456789", 0, 3, "X"),
	             "replace at beginning");
	assert_equal("012345X", replace_file_bytes(path, "0123456789", 6, 4, "X"),
	             "replace through end");
	assert_equal("012345X", replace_file_bytes(path, "012345", 6, 0, "X"),
	             "append with empty tail");

	std::string large;
	large.reserve(df::sixty_four_k + 32);
	for (auto i = 0; i < static_cast<int>(df::sixty_four_k) + 32; ++i) large += static_cast<char>('A' + i % 26);
	const auto large_actual = replace_file_bytes(path, large, 8, 3, "x");
	auto large_expected = large;
	large_expected.replace(8, 3, "x");
	assert_equal(large_expected, large_actual, "shrinking tail crosses a 64 KiB block boundary");

	write_test_file(path, "0123456789");
	{
		df::file file(platform::open_file(path, platform::file_open_mode::read_write));
		assert_equal(false, file.insert(reinterpret_cast<const uint8_t*>("X"), 1, 20, 1), "invalid range fails");
	}
	assert_equal("0123456789", read_test_file(path), "failed replacement leaves bytes unchanged");
}

static void should_map_files()
{
	const auto scratch = _temps.next_folder("map-file");
	const auto path = scratch.combine_file("mapped.txt");

	// Larger than one allocation granularity so a window can be placed past the first boundary.
	std::string content;
	content.reserve(200'000);
	for (auto i = 0; content.size() < 200'000; ++i) content += std::format("line {}\n", i);
	write_test_file(path, content);

	const auto whole = platform::map_file(path);
	assert_equal(true, whole != nullptr, "whole file maps");
	assert_equal(content.size(), static_cast<size_t>(whole->file_size()), "mapped size matches the file");
	assert_equal(content.size(), whole->data().size, "whole view covers the file");
	assert_equal(0, memcmp(whole->data().data, content.data(), content.size()), "mapped bytes match the file");

	// Trimmed pages must still read back correctly - the mapping stays valid, it just faults in.
	whole->release_working_set();
	assert_equal(0, memcmp(whole->data().data, content.data(), content.size()), "bytes survive a working-set release");

	const auto windowed = platform::map_file(path, platform::map_mode::windowed);
	assert_equal(true, windowed != nullptr, "windowed file maps");
	assert_equal(true, windowed->data().empty(), "windowed mapping has no view until a window is set");

	// An offset that is not a multiple of the allocation granularity must still return the exact
	// bytes asked for, because the granularity alignment is the mapping's business, not a caller's.
	constexpr uint64_t odd_offset = 70'001;
	constexpr uint64_t window_len = 4'096;
	const auto window = windowed->set_window(odd_offset, window_len);
	assert_equal(static_cast<size_t>(window_len), window.size, "window is the requested length");
	assert_equal(0, memcmp(window.data, content.data() + odd_offset, window_len), "window starts at the offset");

	// A window running past the end is clamped rather than refused.
	const auto tail = windowed->set_window(content.size() - 10, 4'096);
	assert_equal(10_z, tail.size, "window past the end is clamped to the file");
	assert_equal(0, memcmp(tail.data, content.data() + content.size() - 10, 10), "clamped window holds the tail");

	assert_equal(true, windowed->set_window(content.size(), 16).empty(), "window at the end is empty");

	assert_equal(true, platform::map_file(scratch.combine_file("missing.txt")) == nullptr,
	             "a missing file does not map");

	// An empty file cannot be mapped at all, which is an answer rather than a fault.
	const auto empty_path = scratch.combine_file("empty.txt");
	write_test_file(empty_path, {});
	assert_equal(true, platform::map_file(empty_path) == nullptr, "an empty file does not map");
}

static void should_clean_temporary_fixtures()
{
	temp_files temps;
	const auto scratch = temps.next_folder("cleanup");
	const auto fixed = scratch.combine_file("fixed.txt");
	write_test_file(fixed, "fixed");

	const auto nested = scratch.combine("nested");
	platform::create_folder(nested);
	write_test_file(nested.combine_file("derived.xmp"), "derived");

	const auto tracked = temps.next_path(".jpg");
	write_test_file(tracked, "tracked");
	write_test_file(tracked.extension(".xmp"), "sidecar");

	const auto locked = temps.next_path(".bin");
	write_test_file(locked, "locked");
	auto locked_handle = platform::open_file(locked, platform::file_open_mode::read);
	assert_equal(true, locked_handle != nullptr, "locked temp file opened");

#ifdef _WIN32
	const auto blocked = temps.delete_temps();
	assert_equal(true, !blocked.empty(), "cleanup reports denied deletion");
	assert_equal(true, locked.exists(), "denied file remains owned for retry");

	locked_handle.reset();
	const auto cleaned = temps.delete_temps();
	assert_equal(0, static_cast<int>(cleaned.size()), "cleanup retry succeeds");
#else
	const auto cleaned = temps.delete_temps();
	assert_equal(0, static_cast<int>(cleaned.size()), "POSIX cleanup can unlink an open file");
	locked_handle.reset();
#endif
	assert_equal(false, scratch.exists(), "owned subfolder is removed");
	assert_equal(false, tracked.exists(), "tracked file is removed");
	assert_equal(false, tracked.extension(".xmp").exists(), "derived sidecar is removed");

	const auto preexisting = _temps.next_folder("cleanup-preexisting");
	const auto unrelated = preexisting.combine_file("unrelated.txt");
	write_test_file(unrelated, "unrelated");
	const auto unrelated_child = preexisting.combine("unrelated-child");
	platform::create_folder(unrelated_child);
	write_test_file(unrelated_child.combine_file("unrelated.txt"), "unrelated child");

	temp_files unowned(preexisting);
	const auto registered = unowned.next_path(".tmp");
	write_test_file(registered, "registered");

	const auto unowned_cleanup = unowned.delete_temps();
	assert_equal(0, static_cast<int>(unowned_cleanup.size()), "unowned cleanup reports registered-file success");
	assert_equal(false, registered.exists(), "registered file in pre-existing folder is removed");
	assert_equal(true, unrelated.exists(), "pre-existing folder file is preserved");
	assert_equal(true, unrelated_child.exists(), "pre-existing child folder is preserved");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #203 - Search broken with Russian letter "Х" (U+0425)
// Cyrillic characters must be properly case-folded for case-insensitive comparison.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_handle_cyrillic_case_folding()
{
	// Cyrillic uppercase Х (U+0425) should lowercase to х (U+0445)
	constexpr auto upper_ha = "\u0425"; // Х
	constexpr auto lower_ha = "\u0445"; // х

	// to_lower should convert uppercase Cyrillic to lowercase
	assert_equal_strict(lower_ha, str::to_lower(upper_ha), "Cyrillic to_lower");

	// Case-insensitive comparison should treat them as equal
	assert_equal(0, str::icmp(upper_ha, lower_ha), "Cyrillic icmp");

	// Additional Cyrillic pairs
	assert_equal(0, str::icmp("\u0410", "\u0430"), "А == а"); // А/а
	assert_equal(0, str::icmp("\u0411", "\u0431"), "Б == б"); // Б/б
	assert_equal(0, str::icmp("\u042F", "\u044F"), "Я == я"); // Я/я

	// Mixed Cyrillic text comparison (was failing due to towlower() locale dependency)
	// Test individual characters from "Текст" to isolate failures
	// Use \u escapes to avoid source-file encoding issues
	assert_equal(0, str::icmp("\u0422", "\u0442"), "T == t cyrillic"); // Т/т U+0422/U+0442

	// Текст = U+0422 U+0435 U+043A U+0441 U+0442
	// текст = U+0442 U+0435 U+043A U+0441 U+0442
	constexpr auto cyrillic_upper = "\u0422\u0435\u043a\u0441\u0442"; // Текст
	constexpr auto cyrillic_lower = "\u0442\u0435\u043a\u0441\u0442"; // текст
	const auto upper_text = str::to_lower(cyrillic_upper);
	assert_equal_strict(cyrillic_lower, upper_text, "to_lower cyrillic word");

	assert_equal(0, str::icmp(cyrillic_upper, cyrillic_lower), "mixed Cyrillic icmp");
	// МОСКВА = U+041C U+041E U+0421 U+041A U+0412 U+0410
	// москва = U+043C U+043E U+0441 U+043A U+0432 U+0430
	assert_equal(true, str::icmp("\u041c\u041e\u0421\u041a\u0412\u0410",
	                             "\u043c\u043e\u0441\u043a\u0432\u0430") == 0, "MOSKVA icmp");

	// Greek omicron must case-fold as Greek, not as an unrelated ASCII letter.
	assert_equal(0, str::icmp("\u039f", "\u03bf"), "Greek omicron case-folds");
	assert_equal(false, str::icmp("\u039f", "s") == 0, "Greek omicron is not ASCII s");
	assert_equal(false, str::icmp("\u03bf", "y") == 0, "Greek omicron is not ASCII y");
	assert_equal(str::normalize_for_compare(0x03bf), str::normalize_for_compare(0x039f),
	             "Greek omicron normalizes consistently");
	assert_equal(false, str::normalize_for_compare(0x039f) == 's', "Greek omicron does not normalize to ASCII s");
	assert_equal(false, str::normalize_for_compare(0x03bf) == 'y', "Greek omicron does not normalize to ASCII y");

	// Latin Extended pairs
	assert_equal(0, str::icmp("\u00C0", "\u00E0"), "A-grave icmp");
	assert_equal(0, str::icmp("\u00D6", "\u00F6"), "O-umlaut icmp");
	assert_equal(0, str::icmp("\u00D8", "\u00F8"), "O-stroke icmp");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #219 - Korean tags are not working
// Hangul has no letter case, so case-folding must leave it unchanged.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_case_fold_korean()
{
	// Hangul has no case - normalisation/lowercasing must be identity.
	constexpr auto family = "\uAC00\uC871"; // 가족
	assert_equal_strict(family, str::to_lower(family), "Korean to_lower identity");
	assert_equal(0, str::icmp(family, family), "Korean icmp equal");

	// A single Hangul syllable normalises to itself for comparison.
	assert_equal(0xAC00, str::normalize_for_compare(0xAC00), "Hangul normalise identity");

	// Different Korean words must not compare equal.
	assert_equal(true, str::icmp(family, "\uC5EC\uD589") != 0, "different Korean words differ");
}

static void should_parse_facebook_json()
{
	const auto path_status = test_files_folder.combine_file("place.json");
	const auto json = df::util::json::json_from_file(path_status);

	auto& result = json["result"];
	auto& address_components = result["address_components"];
	assert_equal(5u, address_components.Size(), "data");
	assert_equal("WC1X", address_components[0]["long_name"].GetString(), "long_name");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// kd-tree
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_query_kdtree_bounds()
{
	// A 10x10 integer grid of points; offset encodes the original index.
	kd_points pts;
	for (int gx = 0; gx < 10; ++gx)
	{
		for (int gy = 0; gy < 10; ++gy)
		{
			pts.emplace_back(static_cast<float>(gx), static_cast<float>(gy),
			                 static_cast<uint32_t>(gx * 10 + gy), 0, 0, 0.0f);
		}
	}

	kd_tree tree;
	tree.build(pts); // reorders pts, but offset travels with each point

	// x in {3,4,5} and y in {3,4,5} => 9 points.
	std::vector<kd_coordinates_t> found;
	tree.find_in_bounds(pts, 2.5f, 2.5f, 5.5f, 5.5f, found);
	assert_equal(9, static_cast<int>(found.size()), "window count", "kd bounds");

	for (const auto& c : found)
	{
		assert_equal(true, c.x >= 2.5f && c.x <= 5.5f && c.y >= 2.5f && c.y <= 5.5f, "point in range", "kd bounds");
	}

	// A window far from every point returns nothing.
	std::vector<kd_coordinates_t> none;
	tree.find_in_bounds(pts, 100.0f, 100.0f, 200.0f, 200.0f, none);
	assert_equal(0, static_cast<int>(none.size()), "empty window", "kd bounds");

	// A window covering everything returns all points exactly once.
	std::vector<kd_coordinates_t> all;
	tree.find_in_bounds(pts, -1.0f, -1.0f, 100.0f, 100.0f, all);
	assert_equal(100, static_cast<int>(all.size()), "full window", "kd bounds");
}

static void should_rotate_points()
{
	constexpr auto quarter_turn = 3.14159265358979323846 / 2.0;
	const pointd origin(0, 0);
	const pointd point(2, 3);

	const auto zero = point.rotate(0.0, origin);
	assert_equal(true, df::equiv(point.X, zero.X) && df::equiv(point.Y, zero.Y), "zero-angle identity");

	const auto quarter = pointd(2, 0).rotate(quarter_turn, origin);
	assert_equal(true, df::equiv(0.0, quarter.X) && df::equiv(2.0, quarter.Y), "quarter turn");

	const pointd center(10, 10);
	const auto full = point.rotate(quarter_turn * 4.0, center);
	assert_equal(true, std::abs(point.X - full.X) < 1e-9 && std::abs(point.Y - full.Y) < 1e-9,
	             "full turn around center");

	const auto rotated = point.rotate(quarter_turn, center);
	const auto restored = rotated.rotate(-quarter_turn, center);
	assert_equal(true, std::abs(point.X - restored.X) < 1e-9 && std::abs(point.Y - restored.Y) < 1e-9,
	             "inverse rotation");

	const auto dx_before = point.X - center.X;
	const auto dy_before = point.Y - center.Y;
	const auto dx_after = rotated.X - center.X;
	const auto dy_after = rotated.Y - center.Y;
	assert_equal(true, df::equiv(dx_before * dx_before + dy_before * dy_before,
	                             dx_after * dx_after + dy_after * dy_after), "distance preserved");
}

static void should_intersect_rectangle_bounds()
{
	rectd out;
	assert_equal(true, rectd::intersect(out, rectd(0, 0, 2, 2), rectd(1, 1, 2, 2)), "overlap intersects");
	assert_equal(true, df::equiv(1.0, out.X) && df::equiv(1.0, out.Y) &&
	             df::equiv(1.0, out.Width) && df::equiv(1.0, out.Height), "overlap result");

	assert_equal(false, rectd::intersect(out, rectd(0, 0, 1, 1), rectd(2, 2, 1, 1)), "separated");
	assert_equal(true, out.is_empty(), "separated result is empty");

	assert_equal(true, rectd::intersect(out, rectd(0, 0, 4, 4), rectd(1, 1, 1, 1)), "contained");
	assert_equal(true, df::equiv(1.0, out.X) && df::equiv(1.0, out.Y) &&
	             df::equiv(1.0, out.Width) && df::equiv(1.0, out.Height), "contained result");

	assert_equal(false, rectd::intersect(out, rectd(0, 0, 1, 1), rectd(1, 0, 1, 1)), "touching edge");
	assert_equal(false, rectd::intersect(out, rectd(0, 0, 0, 1), rectd(0, 0, 1, 1)), "zero-size input");

	rectd symmetric;
	assert_equal(true, rectd::intersect(out, rectd(1, 1, 3, 2), rectd(2, 0, 1, 4)), "asymmetric overlap");
	assert_equal(true, rectd::intersect(symmetric, rectd(2, 0, 1, 4), rectd(1, 1, 3, 2)), "symmetric overlap");
	assert_equal(true, df::equiv(out.X, symmetric.X) && df::equiv(out.Y, symmetric.Y) &&
	             df::equiv(out.Width, symmetric.Width) && df::equiv(out.Height, symmetric.Height),
	             "intersection is symmetric");

	rectd aliased(0, 0, 2, 2);
	assert_equal(true, aliased.intersect(rectd(1, 1, 2, 2)), "aliased output");
	assert_equal(true, df::equiv(1.0, aliased.X) && df::equiv(1.0, aliased.Y) &&
	             df::equiv(1.0, aliased.Width) && df::equiv(1.0, aliased.Height), "aliased result");
}

static void should_find_the_closest_kdtree_point()
{
	// Deliberately elongated (x spans 4, y spans 400): a node's bounding box prunes such a spread
	// far more tightly than a bounding circle, so a pruning error surfaces here as a wrong answer.
	kd_points pts;
	std::vector<std::pair<float, float>> expected;
	uint32_t seed = 12345;
	const auto next_rand = [&seed]
	{
		seed = seed * 1664525u + 1013904223u;
		return static_cast<float>(seed % 100000u) * 0.00001f;
	};

	for (uint32_t i = 0; i < 400; ++i)
	{
		const auto x = next_rand() * 4.0f;
		const auto y = next_rand() * 400.0f;
		pts.emplace_back(x, y, i * 3 + 1, 7, i, 0.0f);
		expected.emplace_back(x, y);
	}

	kd_tree tree;
	tree.build(pts);

	// Query points inside, at the corners of, and far outside the data extent.
	const std::vector<std::pair<float, float>> queries{
		{0.0f, 0.0f}, {2.0f, 200.0f}, {3.9f, 399.0f}, {0.1f, 12.5f}, {-40.0f, -40.0f},
		{900.0f, 900.0f}, {-1000.0f, 25.0f}, {2.0f, 100000.0f}
	};

	for (const auto& [qx, qy] : queries)
	{
		auto best_d2 = std::numeric_limits<double>::max();

		for (const auto& [px, py] : expected)
		{
			const double dx = px - qx, dy = py - qy;
			best_d2 = std::min(best_d2, dx * dx + dy * dy);
		}

		const auto found = tree.find_closest(pts, qx, qy);
		const double fdx = found.x - qx, fdy = found.y - qy;
		const auto found_d2 = fdx * fdx + fdy * fdy;

		assert_equal(true, std::abs(found_d2 - best_d2) <= 1e-6 * std::max(1.0, best_d2),
		             "matches the brute-force nearest", "kd closest");

		// The build reorders coordinates and record fields in lockstep, so the record that comes
		// back must still be the one that owns the coordinate it came back with.
		assert_equal(true, found.id < expected.size(), "record identifies a point", "kd closest");
		assert_equal(true, df::equiv(found.x, expected[found.id].first) &&
		             df::equiv(found.y, expected[found.id].second), "record travels with its coordinate",
		             "kd closest");
		assert_equal(static_cast<int>(found.id * 3 + 1), static_cast<int>(found.offset),
		             "every record field travels together", "kd closest");
	}

	// An empty set has no tree and therefore no answer.
	kd_points none;
	kd_tree empty;
	empty.build(none);
	assert_equal(true, empty.is_empty(), "an empty point set builds no nodes", "kd closest");
}

// The gazetteer stores degrees, and a degree of longitude is only a degree of latitude's worth of
// ground at the equator. Searching the raw plane therefore ranked somewhere far to the east as
// nearer than somewhere just up the road, and at sixty degrees the error is a factor of two.
static void should_weigh_longitude_by_latitude_when_finding_the_closest()
{
	// x is latitude and y is longitude, as the gazetteer tree stores them.
	kd_points pts;
	pts.emplace_back(60.0f, 1.0f, 0, 0, 0, 0.0f); // one degree east: about 56 km at this latitude
	pts.emplace_back(60.8f, 0.0f, 0, 0, 1, 0.0f); // 0.8 degrees north: about 89 km

	kd_tree tree;
	tree.build(pts);

	// Unweighted, the northern point wins on raw degrees (0.8 against 1.0) despite being half as
	// far again on the ground.
	const auto raw = tree.find_closest(pts, 60.0f, 0.0f);
	assert_equal(1u, raw.id, "raw degrees pick the point that is fewer degrees away");

	// cos(60 degrees) is a half, so the eastern point's degree counts for half a degree and it wins
	// - which is the answer the ground agrees with.
	const auto weighted = tree.find_closest(pts, 60.0f, 0.0f, 0.5f);
	assert_equal(0u, weighted.id, "weighting longitude picks the point that is actually nearer");

	// At the equator the weighting is one and nothing changes.
	kd_points equator;
	equator.emplace_back(0.0f, 1.0f, 0, 0, 0, 0.0f);
	equator.emplace_back(0.8f, 0.0f, 0, 0, 1, 0.0f);

	kd_tree equator_tree;
	equator_tree.build(equator);
	assert_equal(1u, equator_tree.find_closest(equator, 0.0f, 0.0f, 1.0f).id,
	             "on the equator a degree is a degree either way");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Platform queue
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_signal_and_replace_pending_queue_work()
{
	platform::queue<int> values;
	assert_equal(true, values.enqueue(1), "first enqueue signals empty transition");
	assert_equal(false, values.enqueue(2), "additional queued work does not signal again");

	int value = 0;
	assert_equal(true, values.dequeue(value), "first value dequeued");
	assert_equal(false, values.enqueue(3), "queue remains nonempty until the final value is dequeued");
	assert_equal(true, values.dequeue(value), "second value dequeued");
	assert_equal(true, values.dequeue(value), "third value dequeued");
	assert_equal(true, values.enqueue(4), "enqueue signals after queue becomes empty again");

	platform::task_queue tasks;
	auto executed = 0;
	tasks.enqueue([&executed] { executed = 1; });
	tasks.reset_and_enqueue([&executed] { executed = 2; });
	for (const auto& task : tasks.dequeue_all()) task();
	assert_equal(2, executed, "reset_and_enqueue retains only the latest pending task");

	tasks.enqueue_after(1000, [] {});
	const auto delayed = tasks.dequeue_all();
	assert_equal(1, static_cast<int>(delayed.size()), "delayed work remains queued");
	const auto ready_at = delayed.front().ready_at_us();
	assert_equal(1'000'000, static_cast<int>(ready_at - delayed.front().queued_at_us()),
	             "enqueue_after moves readiness by the requested delay");
	assert_equal(0, static_cast<int>(delayed.front().wait_us(ready_at - 1)),
	             "the intentional delay is not queue wait");
	assert_equal(250, static_cast<int>(delayed.front().wait_us(ready_at + 250)),
	             "time after the deadline is queue wait");
}

static void should_bucket_performance_latency()
{
	df::latency_counters counters;
	df::record_latency(counters, 16'666);
	df::record_latency(counters, 16'667);
	df::record_latency(counters, 50'000);
	df::record_latency(counters, 100'000);

	assert_equal(3, static_cast<int>(counters.over_16ms.load()), "the frame-budget boundary is inclusive");
	assert_equal(2, static_cast<int>(counters.over_50ms.load()), "fifty millisecond stalls are counted");
	assert_equal(1, static_cast<int>(counters.over_100ms.load()), "hundred millisecond stalls are counted");
}

// Base64 carries the saved password and the web service payloads, so a padding or alphabet slip
// corrupts stored credentials silently. Every length modulo 3 is covered because the padding rule
// differs per remainder.
static void should_round_trip_base64()
{
	assert_equal_strict("", base64_encode(""sv), "empty encodes to empty");
	assert_equal_strict("TQ==", base64_encode("M"sv), "one byte pads twice");
	assert_equal_strict("TWE=", base64_encode("Ma"sv), "two bytes pad once");
	assert_equal_strict("TWFu", base64_encode("Man"sv), "three bytes need no padding");

	for (auto length = 0u; length < 64u; ++length)
	{
		std::vector<uint8_t> data(length);

		for (auto i = 0u; i < length; ++i)
		{
			data[i] = static_cast<uint8_t>((i * 61u + 7u) & 0xff);
		}

		const auto encoded = base64_encode(data);
		const auto decoded = base64_decode(encoded);

		assert_equal(static_cast<uint32_t>(length), static_cast<uint32_t>(decoded.size()),
		             "decoded length matches");
		assert_equal(true, decoded == data, "base64 round trips every length");
	}

	// Every byte value must survive, not just printable ones.
	std::vector<uint8_t> all(256);
	for (auto i = 0u; i < 256u; ++i) all[i] = static_cast<uint8_t>(i);
	assert_equal(true, base64_decode(base64_encode(all)) == all, "every byte value round trips");
}

// The sidebar's most-common tags and file types are built from this, so a wrong order or an
// off-by-one limit is directly visible to the user.
static void should_rank_the_most_common_values()
{
	df::string_counts counts;
	counts["alpha"] = 3;
	counts["bravo"] = 11;
	counts["charlie"] = 7;
	counts["delta"] = 1;

	const auto top_two = top_map(counts, 2);
	assert_equal(2, static_cast<int>(top_two.size()), "the limit bounds the result");

	// The winners are chosen by count but presented in name order, so the list does not reshuffle
	// as counts drift during indexing.
	assert_equal("bravo", top_two[0], "first by name");
	assert_equal("charlie", top_two[1], "second by name");

	const auto all = top_map(counts, 10);
	assert_equal(4, static_cast<int>(all.size()), "a limit above the size returns everything");
	assert_equal("alpha", all[0], "sorted by name when nothing is dropped");

	assert_equal(0, static_cast<int>(top_map(counts, 0).size()), "a zero limit returns nothing");
	assert_equal(0, static_cast<int>(top_map(counts, -1).size()), "a negative limit returns nothing");
	assert_equal(0, static_cast<int>(top_map({}, 5).size()), "no counts returns nothing");

	df::string_counts tied;
	std::vector<std::string> tied_names;
	tied_names.reserve(203);
	for (auto i = 0; i < 200; ++i)
	{
		tied_names.emplace_back(std::format("value-{:03}", i));
		tied[tied_names.back()] = i;
	}
	tied_names.emplace_back("tie-a");
	tied[tied_names.back()] = 250;
	tied_names.emplace_back("tie-b");
	tied[tied_names.back()] = 250;
	tied_names.emplace_back("tie-c");
	tied[tied_names.back()] = 250;
	const auto top_five = top_map(tied, 5);
	assert_equal(5, static_cast<int>(top_five.size()), "large candidate set is limited");
	assert_equal("tie-a", top_five[0], "ties are retained by name");
	assert_equal("tie-b", top_five[1], "second tie retained by name");
	assert_equal("tie-c", top_five[2], "third tie retained by name");
	assert_equal("value-198", top_five[3], "next non-tie retained");
	assert_equal("value-199", top_five[4], "highest non-tie retained");
}

void register_util_tests(view_state& state, test_registry& tests)
{
	tests.add("Should natural compare"s, should_icmp_natural);
	tests.add("Should name files after the application"s, should_name_files_after_the_application);
	tests.add("Should detect utf16 either way round"s, should_detect_utf16_either_way_round);
	tests.add("Should complete result scope"s, should_complete_result_scope);
	tests.add("Should abort result scope during exception"s, should_abort_result_scope_during_exception);
	tests.add("Should cancel superseded tokens"s, should_cancel_superseded_tokens);
	tests.add("Should group elements by folder"s, should_group_elements_by_folder);
	tests.add("Should follow the filesystem for path identity"s, should_follow_the_filesystem_for_path_identity);
	// SRC-004 - direct children of the Linux root were parsed as folder-only paths.
	tests.add("Should parse root child paths"s, should_parse_files_directly_under_the_root);
	tests.add("Should report file presence"s, should_report_file_presence);
	tests.add("Should map files"s, should_map_files);
	tests.add("Should clean temporary fixtures"s, should_clean_temporary_fixtures);
	tests.add("Should save a blob without truncating the destination"s,
	          should_save_a_blob_without_truncating_the_destination);
	tests.add("Should replace bytes in files"s, should_replace_bytes_in_files);
	tests.add("Should intern strings"s, should_intern_strings);
	tests.add("Should round-trip base64"s, should_round_trip_base64);
	tests.add("Should rank the most common values"s, should_rank_the_most_common_values);
	tests.add("Should calc HMAC SHA1"s, should_calc_HMACSHA1);
	tests.add("Should calc Hashes"s, should_calc_hashes);
	tests.add("Should calc perceptual hashes"s, should_calc_perceptual_hashes);
	tests.add("Should recognise the same picture"s, should_recognise_the_same_picture);
	tests.add("Should recognise a rotated picture"s, should_recognise_a_rotated_picture);
	tests.add("Should rotate points"s, should_rotate_points);
	tests.add("Should convert Utf8"s, should_convert_utf8);
	tests.add("Should split"s, should_split);
	tests.add("Should split genre"s, should_split_genre);
	tests.add("Should extract url"s, should_extract_url);
	tests.add("Should parse dates"s, should_parse_dates);
#ifndef _WIN32
	tests.add("Should convert local dates with date specific rules"s, should_convert_local_dates_with_date_specific_rules);
#endif
	tests.add("Should detect wildcard"s, should_detect_wildcard);
	tests.add("Should match wildcard"s, should_match_wildcard);
	tests.add("Should compare versions"s, should_compare_versions);
	tests.add("Should parse command line"s, should_parse_command_line);
	tests.add("Should trim strings"s, should_trim_strings);
	tests.add("Should format text"s, should_format_text);
	tests.add("Should reject invalid assertion inputs"s, should_reject_invalid_assertion_inputs);
	tests.add("Should format a double of any magnitude"s, should_format_a_double_of_any_magnitude);
	tests.add("Should find text"s, should_find_text);

	//
	// Json
	//
	tests.add("Should parse facebook Json"s, should_parse_facebook_json);

	// Issue #203 - Cyrillic character search
	tests.add("Should handle Cyrillic case folding"s, should_handle_cyrillic_case_folding);

	// Issue #219 - Korean tags
	tests.add("Should case-fold Korean"s, should_case_fold_korean);

	//
	// kd-tree
	//
	tests.add("Should query kd-tree bounds"s, should_query_kdtree_bounds);
	tests.add("Should intersect rectangle bounds"s, should_intersect_rectangle_bounds);
	tests.add("Should find the closest kd-tree point"s, should_find_the_closest_kdtree_point);
	tests.add("Should weigh longitude by latitude when finding the closest"s,
	          should_weigh_longitude_by_latitude_when_finding_the_closest);

	//
	// Platform queue
	//
	tests.add("Should signal and replace pending queue work"s, should_signal_and_replace_pending_queue_work);
	tests.add("Should bucket performance latency"s, should_bucket_performance_latency);
}
