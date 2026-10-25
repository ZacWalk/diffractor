// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Search query tests. Verifies search parsing, term matching, date matching,
// selectors, file selection, related results, search navigation and prediction.

#include "pch.h"
#include "test_fixtures.h"
#include "model_postings.h"
#include "model_tokenizer.h"
#include "app_match.h"

// Path syntax is the vehicle in most of these tests, not the subject: what they constrain - where
// the folder ends, what makes a browse recursive, which part is a wildcard, what a parent is - is
// the same wherever Diffractor runs. This spells the same path for the platform under test, so a
// drive root becomes the filesystem root and separators follow suit.
static std::string as_platform_path(std::string_view windows_form)
{
	if constexpr (df::windows_path_semantics) return std::string(windows_form);

	std::string result;

	// A drive letter names nothing here, so the filesystem root stands in for the drive root.
	if (windows_form.size() >= 2 && windows_form[1] == ':' &&
		(windows_form[0] | 0x20) >= 'a' && (windows_form[0] | 0x20) <= 'z')
	{
		result = "/";
		windows_form.remove_prefix(2);

		if (!windows_form.empty() && (windows_form.front() == '\\' || windows_form.front() == '/'))
		{
			windows_form.remove_prefix(1);
		}
	}

	for (const auto c : windows_form) result += c == '\\' ? '/' : c;

	return result;
}

static void should_parse_searches()
{
	const auto date_filter = df::search_t::parse("2012-09-14");
	const df::search_matcher matcher(date_filter);

	df::index_file_item info;
	info.ft = files::file_type_from_name("test.jpg");
	info.safe_ps()->dates.add(prop::date_source::exif_original, df::date_t(2012, 9, 14));
	assert_equal(true, matcher.match_item({}, info).is_match(), "date");

	info.safe_ps()->dates.clear();
	info.safe_ps()->dates.add(prop::date_source::exif_original, df::date_t(2012, 9, 13));
	assert_equal(false, matcher.match_item({}, info).is_match(), "date");
}

// Group by Date Original keys on the capture-first ladder and Group by Date Created keys on the
// Created concept, so each header has to be answered by its own key and by nothing else: answered by
// the wrong one, a header lists items the cell never counted; answered by neither, it opens empty.
// That is #184 on whichever order loses the rung.
static void should_answer_a_date_group_header_with_its_own_items()
{
	df::index_file_item info;
	info.ft = files::file_type_from_name("test.jpg");

	// A photograph taken in 2019 and written to this disk by an export in 2026 - the ordinary shape
	// of a file whose two dates disagree, and the whole reason Created is an order of its own.
	info.safe_ps()->dates.add(prop::date_source::exif_original, df::date_t(2019, 5, 4));
	info.safe_ps()->dates.add(prop::date_source::xmp_create, df::date_t(2026, 8, 19));

	assert_equal(df::date_t(2019, 5, 4), info.safe_ps()->dates.original(), "the shutter is Original");
	assert_equal(df::date_t(2026, 8, 19), info.safe_ps()->dates.created(), "and the export is Created");

	// Day zero is the whole month, which is what a group header stands for.
	const auto header = [](const int month, const int year, const df::date_parts_prop target)
	{
		return df::search_t().day(0, month, year, target);
	};

	assert_equal(true, df::search_matcher(header(5, 2019, df::date_parts_prop::original))
	             .match_item({}, info).is_match(), "the Original header lists it");
	assert_equal(true, df::search_matcher(header(8, 2026, df::date_parts_prop::created))
	             .match_item({}, info).is_match(), "the Created header lists it");

	// The two probes that pin the split. While the targets were one word both of these matched, so
	// each header listed the other's items and the two orders were the same order.
	assert_equal(false, df::search_matcher(header(8, 2026, df::date_parts_prop::original))
	             .match_item({}, info).is_match(), "the Original header does not list the export month");
	assert_equal(false, df::search_matcher(header(5, 2019, df::date_parts_prop::created))
	             .match_item({}, info).is_match(), "the Created header does not list the shutter month");

	// Still a date test, not a date-shaped wildcard.
	assert_equal(false, df::search_matcher(header(7, 2022, df::date_parts_prop::original))
	             .match_item({}, info).is_match(), "a month neither date names lists nothing");

	// Where the concept has no tag the header is drawn from the filesystem stamp, so the stamp has
	// to answer that order - and only that order, or Created reaches back into the shutter time.
	df::index_file_item stamped;
	stamped.ft = files::file_type_from_name("test.jpg");
	stamped.safe_ps()->dates.add(prop::date_source::exif_original, df::date_t(2019, 5, 4));
	stamped.file_created = df::date_t(2021, 3, 9);

	assert_equal(false, stamped.safe_ps()->dates.created().is_valid(), "no tag names a creation date");

	// Derived rather than written out: the group key converts the stamp to local, so the month the
	// header carries depends on the machine and hard-coding one would pin nothing off UTC.
	const auto stamp_parts = stamped.file_created.system_to_local().date();

	assert_equal(true, df::search_matcher(header(stamp_parts.month, stamp_parts.year,
	                                             df::date_parts_prop::created))
	             .match_item({}, stamped).is_match(), "the Created header drawn from the stamp lists it");
	assert_equal(true, df::search_matcher(header(5, 2019, df::date_parts_prop::original))
	             .match_item({}, stamped).is_match(), "and its Original header lists it too");
	assert_equal(false, df::search_matcher(header(5, 2019, df::date_parts_prop::created))
	             .match_item({}, stamped).is_match(), "while Created still does not reach the shutter time");
}

static void should_calculate_safe_presence_requirements()
{
	const auto photo_bit = file_group::photo.search_presence_bit();
	const auto video_bit = file_group::video.search_presence_bit();

	assert_equal(photo_bit, df::search_t::parse("@photo").calc_required_presence().types,
	             "positive media type is required");
	assert_equal(0u, df::search_t::parse("-@photo").calc_required_presence().types,
	             "negated media type cannot be required");
	assert_equal(0u, df::search_t::parse("@photo or @video").calc_required_presence().types,
	             "OR branches have no individually required bit");
	assert_equal(photo_bit | video_bit, df::search_t::parse("@photo @video").calc_required_presence().types,
	             "positive conjunction requires every media bit");
}

static void should_update_duplicate_search_presence()
{
	df::index_item_infos files(1);
	files.front().ft = files::file_type_from_name("test.jpg");
	files.front().calc_search_presence();

	const auto folder = std::make_shared<df::index_folder_item>(std::move(files));
	folder->reset_search_presence();
	const auto duplicate_search = df::search_t::parse("@duplicates");
	const df::search_matcher matcher(duplicate_search);
	const auto& file = folder->files.front();

	assert_equal(false, matcher.can_contain(file.search_presence), "initial item presence");
	assert_equal(false, matcher.can_contain(folder->search_presence_summary), "initial folder presence");

	file.update_duplicates(folder, df::duplicate_info{.group = 7, .count = 2});
	assert_equal(true, matcher.can_contain(file.search_presence), "added item presence");
	assert_equal(true, matcher.can_contain(folder->search_presence_summary), "added folder presence");
	assert_equal(true, matcher.match_item({}, file).is_match(), "added exact duplicate state");

	file.update_duplicates(folder, df::duplicate_info{});
	assert_equal(false, matcher.can_contain(file.search_presence), "removed item presence");
	assert_equal(true, matcher.can_contain(folder->search_presence_summary), "safe stale folder presence");
	assert_equal(false, matcher.match_item({}, file).is_match(), "removed exact duplicate state");
}

static void should_search_boolean_presence_terms(shared_test_context& stc)
{
	stc.lazy_load_index();

	const auto photos = count_search_results(stc.test_index, "@photo");
	const auto videos = count_search_results(stc.test_index, "@video");

	// The positive media term keeps these comparisons file-only (a bare negative term
	// can also match folders). These identities verify that the bitmap prefilter does
	// not discard files before the exact Boolean matcher sees them.
	assert_equal(photos, count_search_results(stc.test_index, "@photo -@video"),
	             "negated media search");
	assert_equal(photos + videos, count_search_results(stc.test_index, "@photo or @video"),
	             "OR media search");

	const auto duplicate_photos = count_search_results(stc.test_index, "@photo @duplicates");
	assert_equal(photos - duplicate_photos, count_search_results(stc.test_index, "@photo -@duplicates"),
	             "negated duplicate search");
}

static void should_match_terms()
{
	prop_test().tag("aaa")
	           .is_match("#aaa")
	           .is_not_match("#bbb")
	           .is_match("-#bbb")
	           .is_match("!#bbb")
	           .is_not_match("-#aaa")
	           .is_not_match("!#aaa")
	           .is_match("#aaa or #bbb")
	           .is_not_match("#aaa and #bbb")
	           .is_not_match("#aaa #bbb");

	prop_test().tag("bbb")
	           .is_not_match("#aaa")
	           .is_match("#bbb")
	           .is_match("#aaa or #bbb");

	prop_test().tag("'aa bb'")
	           .is_not_match("#aaa")
	           .is_not_match("#bb")
	           .is_match("#'aa bb'");

	prop_test()
		.tag("aaa bbb")
		.is_match("#aaa")
		.is_match("#bbb")
		.is_match("#aaa or #bbb")
		.is_match("#aaa and #bbb")
		.is_match("#aaa #bbb")
		.is_not_match("#aaa !#bbb");

	prop_test().rate(4)
	           .is_match("rate:4")
	           .is_match("4")
	           .is_match(">= 4")
	           .is_not_match("> 4")
	           .is_match(">3")
	           .is_match("3 | 4")
	           .is_match("3 or 4")
	           .is_not_match("3 and 4");

	prop_test().desc("one two three")
	           .is_match("two")
	           .is_match("one two")
	           .is_match("two three")
	           .is_match("'two three'")
	           .is_not_match("'one three'");

	prop_test().date(1999, 12, 27)
	           .is_match("age:5")
	           .is_match("age:10")
	           .is_not_match("age:1")
	           .is_not_match("age:2");

	prop_test().date(2000, 1, 1)
	           .is_match("age:1")
	           .is_match("age:5")
	           .is_not_match("-age:5")
	           .is_not_match("!age:5");

	// original: is the capture-first ladder the tile shows and Group by Date Original keys on;
	// created: is the Created concept alone. A file carrying only a shutter time answers the first
	// and not the second, which is what stops the two words being synonyms.
	prop_test().tag("aaa").date(2000, 1, 1)
	           .is_match("#aaa age:1")
	           .is_match("#aaa original:2000-jan")
	           .is_not_match("original:2000-feb")
	           .is_not_match("#bbb original:2000-jan")
	           .is_not_match("created:2000-jan");

	// The reverse asymmetry: the shutter time answers original: and the filesystem stamp behind it
	// answers created:, so neither word reaches the other's date.
	prop_test().file_created_date(2000, 1, 1).date(1999, 5, 25)
	           .is_match("original:1999-may")
	           .is_not_match("original:1998-mar")
	           .is_not_match("created:1999-may");

	// With no metadata date at all both ladders fall back to the same stamp, which is what keeps a
	// header drawn from it clickable under either order.
	prop_test().file_created_date(2000, 1, 1)
	           .is_match("age:10")
	           .is_match("created:2000-jan")
	           .is_match("original:2000-jan");
}

// A query the user is midway through typing has an unbalanced '(' on almost every
// keystroke. The Boolean evaluator must fold that open group back into the outermost
// level, otherwise the outermost level keeps its initial 'true' and the query silently
// matches every item in scope.
static void should_match_unbalanced_groups()
{
	prop_test().tag("aaa")
	           .is_match("(#aaa")
	           .is_match("(#aaa)")
	           .is_not_match("(#bbb")
	           .is_not_match("(#bbb)")
	           .is_match("#aaa (#bbb or #aaa")
	           .is_not_match("#aaa (#bbb")
	           .is_match("((#aaa")
	           .is_not_match("((#bbb");

	prop_test().tag("bbb")
	           .is_not_match("(#aaa")
	           .is_match("(#aaa or #bbb")
	           .is_not_match("(#aaa #bbb");
}

// An unknown property name after with:/without: used to build a null-key term, which
// inverted into "match everything" for without: and "match nothing" for with:.
static void should_ignore_unknown_property_scopes()
{
	prop_test().tag("aaa")
	           .is_match("without:rating")
	           .is_not_match("with:rating")
	           .is_not_match("without:not-a-property #bbb")
	           .is_match("with:tag");

	prop_test().rate(4)
	           .is_match("with:rating")
	           .is_not_match("without:rating");
}

// Property kinds that the parser used to drop entirely, leaving an empty term list and
// therefore an unfiltered result set.
static void should_match_parsed_property_values()
{
	// int_pair: "N" matches that number of any total, "N/M" must match both parts.
	prop_test().track(3, 12)
	           .is_match("track:3")
	           .is_match("track:3/12")
	           .is_not_match("track:4")
	           .is_not_match("track:3/11");
	prop_test().track(4, 12)
	           .is_match(">track:3")
	           .is_match(">=track:3")
	           .is_not_match("<track:3");
	prop_test().track(2, 12)
	           .is_match("<track:3")
	           .is_match("<=track:3")
	           .is_not_match(">track:3");
	prop_test().track(3, 13)
	           .is_match("track:3")
	           .is_match(">track:3/12")
	           .is_not_match("=track:3/12");
	prop_test().disk(4, 12)
	           .is_match(">disk:3")
	           .is_not_match("<disk:3");
	prop_test().disk(3, 13)
	           .is_match("disk:3")
	           .is_match(">disk:3/12");
	prop_test().episode(4, 12)
	           .is_match(">episode:3")
	           .is_not_match("<episode:3");
	prop_test().episode(3, 13)
	           .is_match("episode:3")
	           .is_match(">episode:3/12");
	prop_test().dimensions(4000, 3000)
	           .is_match(">dimensions:3000x2000")
	           .is_not_match("<dimensions:3000x2000")
	           .is_match("dimensions:4000");
	prop_test().dimensions(3000, 2200)
	           .is_match(">dimensions:3000x2000")
	           .is_not_match("=dimensions:3000x2000");

	// a date property other than created/modified reached a poisoned comparison
	prop_test().digitized(2000, 1, 1)
	           .is_match("digitized:2000-jan")
	           .is_not_match("digitized:2000-feb");

	// "ISO400" parsed the number but then matched against zero
	prop_test().iso(400)
	           .is_match("iso:ISO400")
	           .is_match("iso:400")
	           .is_not_match("iso:800");

	// "%d:%d" also matches the prefix of "1:02:03", so H:MM:SS must be tried first
	prop_test().duration(3723)
	           .is_match("duration:1:02:03")
	           .is_not_match("duration:1:02");
}

static void should_match_multi_value_genre()
{
	// A single ';'-separated genre field should match a query for any one of its values.
	prop_test().genre("Rock; Pop; Hip Hop")
	           .is_match("genre:Rock")
	           .is_match("genre:Pop")
	           .is_match("genre:'Hip Hop'")
	           .is_not_match("genre:Jazz")
	           .is_match("genre:Rock or genre:Jazz")
	           .is_not_match("-genre:Rock");

	// A single-value genre still matches exactly.
	prop_test().genre("Jazz")
	           .is_match("genre:Jazz")
	           .is_not_match("genre:Rock");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #139 / #178 - Quoted terms containing spaces.
// A quoted value with spaces (e.g. a path or multi-word tag) must be parsed as a
// single term, not split on the space, and must survive round-tripping through
// the parser. This is the search-engine capability behind #139 (auto-quoting
// paths with spaces) and #178 (quoted/edited terms must not be silently dropped);
// the search-bar UI behaviour in those reports is tracked separately.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_match_quoted_terms()
{
	// A multi-word tag is kept whole when quoted and does not match its parts.
	prop_test().tag("'holiday photos'")
	           .is_match("#'holiday photos'")
	           .is_not_match("#holiday")
	           .is_not_match("#photos");

	// A quoted phrase matches a contiguous substring only.
	prop_test().desc("new york city")
	           .is_match("'new york'")
	           .is_not_match("'york new'");

	// A path-like value with spaces round-trips through parse + match.
	prop_test().desc("C:/My Photos/trip")
	           .is_match("'My Photos'")
	           .is_not_match("'Your Photos'");
}

// Issue #178 - the search bar must echo exactly what the user typed instead of
// silently reverting it to the normalized form. Issue #139 - a bare path that
// contains spaces is auto-quoted so it reads as a single term.
static void should_preserve_and_auto_quote_search_input()
{
	const df::search_t base;

	// #178: user text is preserved, not reverted to a normalized reconstruction.
	assert_equal("#a #b #a"s, base.parse_from_input("#a #b #a").text(),
	             "duplicate tags preserved in search bar text");
	assert_equal("\"aaa\""s, base.parse_from_input("\"aaa\"").text(),
	             "quotes preserved in search bar text");

	// #139: a bare path containing spaces is auto-quoted.
	const auto my_photos = as_platform_path("C:\\My Photos");
	const auto photos = as_platform_path("C:\\Photos");

	assert_equal("\"" + my_photos + "\"", base.parse_from_input(my_photos).text(),
	             "path with spaces auto-quoted");
	assert_equal(photos, base.parse_from_input(photos).text(),
	             "path without spaces left unquoted");
	assert_equal("\"" + my_photos + "\"", base.parse_from_input("\"" + my_photos + "\"").text(),
	             "already-quoted path not double-quoted");

	const auto folder_query = std::string(test_files_folder.text()) + " Test";
	const auto scoped = base.parse_from_input(folder_query);
	assert_equal(1_z, scoped.selectors().size(), "folder query has one selector");
	assert_equal(1_z, scoped.terms().size(), "the filename term is separate from the folder");
	assert_equal(folder_query, scoped.text(), "the whole query is not quoted as one folder");
	const auto resubmitted = base.parse_from_input(scoped.text());
	assert_equal(scoped.format_terms(), resubmitted.format_terms(), "resubmission preserves the query");
	assert_equal(scoped.selectors().front().folder().text(), resubmitted.selectors().front().folder().text(),
	             "resubmission preserves the folder");
}

// Issue #157 - scope-aware search auto-completion. The active (last) token of a query
// is classified so the value part of a scoped term can be completed from the matching
// vocabulary, and matches are formatted back into the text that will be committed.
static void should_classify_search_scope()
{
	using k = df::search_scope_kind;

	// A plain word / empty query is not scoped.
	assert_equal(true, df::classify_search_scope("hello").kind == k::none, "plain word not scoped");
	assert_equal(true, df::classify_search_scope("").kind == k::none, "empty query not scoped");

	// '#' tag scope: completes from "#tag" vocabulary and keeps the '#' form.
	{
		const auto s = df::classify_search_scope("#do");
		assert_equal(true, s.kind == k::tag, "# is a tag scope");
		assert_equal(false, s.tag_as_scope, "# is not the tag: form");
		assert_equal("do"s, s.value, "# value fragment");
		assert_equal("#do"s, s.vocab_query(), "# vocabulary query");
		assert_equal("#dog"s, s.format("#dog"), "# keeps the hash form");
	}

	// 'tag:' scope: completes from the same vocabulary but rewrites to the tag: form.
	{
		const auto s = df::classify_search_scope("tag:do");
		assert_equal(true, s.kind == k::tag, "tag: is a tag scope");
		assert_equal(true, s.tag_as_scope, "tag: form recorded");
		assert_equal("#do"s, s.vocab_query(), "tag: uses the # vocabulary");
		assert_equal("tag:dog"s, s.format("#dog"), "tag: rewrites #dog -> tag:dog");
	}

	// '@' media group scope.
	{
		const auto s = df::classify_search_scope("@vi");
		assert_equal(true, s.kind == k::group, "@ is a group scope");
		assert_equal("@vi"s, s.vocab_query(), "@ vocabulary query");
		assert_equal("@video"s, s.format("@video"), "@ keeps the group form");
	}

	// with: / without: complete from property scope names (no vocabulary query).
	{
		const auto sw = df::classify_search_scope("with:ex");
		assert_equal(true, sw.kind == k::with, "with: scope");
		assert_equal(""s, sw.vocab_query(), "with: has no word vocabulary");
		assert_equal("with:exposure"s, sw.format("exposure"), "with: formats the scope name");

		const auto swo = df::classify_search_scope("without:ta");
		assert_equal(true, swo.kind == k::without, "without: scope");
		assert_equal("without:tag"s, swo.format("tag"), "without: formats the scope name");
	}

	// Only the last token is scoped; the lead text is preserved verbatim.
	{
		const auto s = df::classify_search_scope("beach tag:do");
		assert_equal(true, s.kind == k::tag, "last token classified");
		assert_equal("beach "s, s.lead, "lead text preserved");
		assert_equal("do"s, s.value, "value taken from last token");
		assert_equal("beach tag:dog"s, str::combine2(s.lead, s.format("#dog")), "lead + formatted value");
	}

	// Scope prefixes are case-insensitive.
	assert_equal(true, df::classify_search_scope("TAG:do").kind == k::tag, "tag: is case-insensitive");
	assert_equal(true, df::classify_search_scope("WITH:x").kind == k::with, "with: is case-insensitive");

	// An empty value (just the prefix) still classifies and yields a broad vocabulary query.
	{
		const auto s = df::classify_search_scope("tag:");
		assert_equal(true, s.kind == k::tag, "bare tag: is a tag scope");
		assert_equal("#"s, s.vocab_query(), "bare tag: matches all tags");
	}

	// locations.md 3.4/3.5: every location scope completes from the gazetteer, differing only in
	// the level it constrains to, and the candidate arrives as a finished canonical term.
	{
		using l = df::location_level;

		const auto loc = df::classify_search_scope("loc:lond");
		assert_equal(true, loc.kind == k::location, "loc: is a location scope");
		assert_equal(true, loc.level == l::any, "loc: is unqualified");
		assert_equal("lond"s, loc.value, "loc: value fragment");
		assert_equal(""s, loc.vocab_query(), "loc: has no word vocabulary");
		assert_equal("loc:\"London, United Kingdom\""s, loc.format("loc:\"London, United Kingdom\""),
		             "location candidates commit verbatim");

		assert_equal(true, df::classify_search_scope("near:lond").kind == k::location, "near: is a location scope");
		assert_equal(true, df::classify_search_scope("place:lond").level == l::place, "place: is place level");
		assert_equal(true, df::classify_search_scope("city:lond").level == l::place, "city: is place level");
		assert_equal(true, df::classify_search_scope("state:cali").level == l::state, "state: is state level");
		assert_equal(true, df::classify_search_scope("country:fra").level == l::country, "country: is country level");
		assert_equal(true, df::classify_search_scope("countries:fra").level == l::country,
		             "countries: is country level");
		assert_equal(true, df::classify_search_scope("LOC:x").kind == k::location, "loc: is case-insensitive");

		// Only the last token is scoped, and a bare prefix still classifies.
		const auto lead = df::classify_search_scope("beach loc:lond");
		assert_equal(true, lead.kind == k::location, "last token classified");
		assert_equal("beach "s, lead.lead, "lead text preserved");
		assert_equal(""s, df::classify_search_scope("loc:").value, "bare loc: matches all places");

		// A word that merely begins with a scope name is not a scope.
		assert_equal(true, df::classify_search_scope("location").kind == k::none, "'location' is a plain word");
	}
}

// A completion match needs a strategy to report back to. Nothing here selects, so the callbacks
// are never reached.
class null_complete_strategy final : public ui::complete_strategy_t
{
public:
	std::string no_results_message() override { return {}; }
	void selected(const ui::auto_complete_match_ptr& i, select_type st) override {}
	ui::auto_complete_match_ptr selected() const override { return nullptr; }

	void search(const std::string& query, std::function<void(const ui::auto_complete_results&)> complete) override
	{
	}

	void initialise(std::function<void(const ui::auto_complete_results&)> complete) override {}
};

static void should_format_search_predictions()
{
	assert_equal("#holiday #beach"s, str::combine2(auto_complete_lead("#holiday"), "#beach"),
	             "missing separator is inserted");
	assert_equal("#holiday #beach"s, str::combine2(auto_complete_lead("#holiday "), "#beach"),
	             "existing separator is preserved");

	// Issue #139 - a completed path containing spaces reads as one term. Committing it after a lead
	// term is the case the whole-input auto-quote cannot reach.
	null_complete_strategy strategy;

	const auto my_photos = as_platform_path("C:\\My Photos");
	const auto photos = as_platform_path("C:\\Photos");

	assert_equal("\"" + my_photos + "\"",
	             folder_match(strategy, df::folder_path(my_photos)).edit_text(),
	             "completed path with spaces is quoted");
	assert_equal(photos, folder_match(strategy, df::folder_path(photos)).edit_text(),
	             "completed path without spaces is left alone");
	assert_equal("#holiday \"" + my_photos + "\"",
	             folder_match(strategy, df::folder_path(my_photos), "#holiday"s).edit_text(),
	             "completed path after a lead term is quoted");
	assert_equal(my_photos,
	             folder_match(strategy, df::folder_path(my_photos), ui::match_highlights{}, 1, true).edit_text(),
	             "folder picker completions keep raw paths");

	assert_equal(true, search_icon("holiday") == icon_index::search, "plain text uses search icon");
	assert_equal(true, search_icon(as_platform_path("C:\\Photos")) == icon_index::folder, "path uses folder icon");
	assert_equal(true, search_icon("#holiday") == icon_index::tag, "tag uses tag icon");
	assert_equal(true, search_icon("rating:4") == prop::rating.icon, "property uses property icon");
	assert_equal(true, search_icon("2012-09-14") == icon_index::time, "date uses time icon");
	assert_equal(true, search_icon("@photo") == icon_index::photo, "media group uses media icon");
}

// A bare byte count went through an int, so anything above two billion wrapped and matched small
// files, and TB was not a unit the parser knew at all - the formatter already wrote it, so a
// terabyte query did not survive a round trip through the address box.
static void should_parse_file_size_terms()
{
	const auto size_of = [](const std::string_view query) -> uint64_t
	{
		const auto search = df::search_t::parse(query);
		const auto& terms = search.terms();
		return terms.empty() ? 0ull : terms.front().int64_val;
	};

	constexpr uint64_t kb = 1024ull;
	constexpr uint64_t mb = kb * 1024ull;
	constexpr uint64_t gb = mb * 1024ull;
	constexpr uint64_t tb = gb * 1024ull;

	assert_equal(14ull * kb, size_of("size:14kb"), "kilobytes");
	assert_equal(mb, size_of("size:1mb"), "megabytes");
	assert_equal(2ull * gb, size_of("size:2gb"), "gigabytes");
	assert_equal(tb, size_of("size:1tb"), "terabytes");
	assert_equal(3ull * tb / 2ull, size_of("size:1.5tb"), "fractional terabytes");
	assert_equal(512ull, size_of("size:512"), "a bare byte count");

	// Larger than a signed 32-bit count, which is where the int overflowed.
	assert_equal(3'000'000'000ull, size_of("size:3000000000"), "a bare count past the int range");
	assert_equal(tb, size_of("size:1099511627776"), "a bare count of a terabyte");

	// The formatter writes a terabyte as "size: 1 TB", the unit apart from its number, so the unit
	// has to be recognised on its own as well or the query reads back as one byte and a text term.
	const auto formatted = df::search_t::parse("size:1tb").format_terms();
	assert_equal(1_z, df::search_t::parse(formatted).terms().size(), "a formatted terabyte is still one term");
	assert_equal(tb, size_of(formatted), "and still a terabyte");
}

static void should_not_match_folder_without()
{
	const auto now_days = df::date_t(2000, 1, 1).to_days();
	const auto search = df::search_t::parse("without:tag");
	const df::search_matcher matcher(search, now_days);
	assert_equal(false, matcher.match_folder(test_files_folder.text(), "test"_c).is_match(), "folder name test");
}

// A level used to start at `true`, the identity for AND but the annihilator for OR. A group opened
// by an OR term folded into a parent seeded with `true`, so the parent stayed true whatever it
// received and the whole expression matched every item.
static void should_group_search_terms()
{
	const auto matches = [](const std::string_view query, const std::string_view name)
	{
		df::index_file_item file;
		file.name = str::cache(name);
		file.ft = files::file_type_from_name(name);
		file.safe_ps();

		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search, df::date_t(2000, 1, 1).to_days());
		return matcher.match_all_terms(test_files_folder.text(), file).is_match();
	};

	assert_equal(true, matches("alpha or (beta gamma)", "alpha.jpg"), "the left side of an or still matches");
	assert_equal(false, matches("alpha or (beta gamma)", "beta.jpg"), "half of an and-group does not");
	assert_equal(false, matches("alpha or (beta gamma)", "delta.jpg"), "neither side matches nothing");

	// The regression: the same expression with the group nested one deeper.
	assert_equal(false, matches("alpha or ((beta gamma))", "delta.jpg"),
	             "a nested group after or does not match everything");
	assert_equal(false, matches("alpha or ((beta gamma))", "beta.jpg"),
	             "and still binds inside the nested group");
	assert_equal(true, matches("alpha or ((beta gamma))", "alpha.jpg"),
	             "and the or still matches its left side");

	assert_equal(false, matches("(alpha) (beta)", "alpha.jpg"), "two and-groups both have to match");
	assert_equal(true, matches("(alpha) or (beta)", "beta.jpg"), "either or-group may match");
	assert_equal(false, matches("(alpha) or (beta)", "delta.jpg"), "an or of groups is not unconditional");
}

// Four ways a query came back as a different question from the one that was asked. Each is a silent
// wrong answer: the search runs, returns a plausible set, and never says the question changed.
static void should_read_a_query_back_as_it_was_asked()
{
	// Dimensions are written "WxH", but the pair was split on '/' alone - so the height fell off,
	// and compare_term reads a zero second component as "any". The query then matched every image
	// of that width whatever its height, which is broader than what was asked and than what the
	// list had shown.
	{
		const auto parsed = df::search_t::parse("dimensions:1920x1080");
		assert_equal(1_z, parsed.terms().size(), "a dimension is one term");
		assert_equal(1920, static_cast<int>(parsed.terms().front().xy_val.x), "the width is read");
		assert_equal(1080, static_cast<int>(parsed.terms().front().xy_val.y), "and so is the height");

		const auto track = df::search_t::parse("track:3/12");
		assert_equal(3, static_cast<int>(track.terms().front().xy_val.x), "a track still reads its number");
		assert_equal(12, static_cast<int>(track.terms().front().xy_val.y), "and its total");
	}

	// Every delimiter the tokenizer would otherwise split is quoted when it is data, not grammar.
	for (const auto value : {"Part(1)"sv, "a,b"sv, "a;b"sv, "a|b"sv, "a#b"sv, "<3"sv, ">x"sv, "=x"sv,
		     "&ME"sv})
	{
		auto query = df::search_t().with(prop::title, value);
		const auto text = query.format_terms();
		const auto reparsed = df::search_t::parse(text);
		assert_equal(1_z, reparsed.terms().size(), std::format("{} stays one term", value));
		assert_equal(value, reparsed.terms().front().text, std::format("{} operand survives", value));
		assert_equal(text, reparsed.format_terms(), std::format("{} canonical form is stable", value));
	}

	// Numeric query operands keep the exact predicate value rather than the rounded display label.
	{
		const auto query = df::search_t().with(prop::focal_length, 8.8);
		const auto text = query.format_terms();
		assert_equal("focal.length: 8.8mm", text, "focal length query text");
		const auto reparsed = df::search_t::parse(text);
		assert_equal(8.8, reparsed.terms().front().float_val, "focal length operand survives");

		prop_test().focal_length(8.8).is_match(text);
		prop_test().focal_length(9.0).is_not_match(text);

		const auto sub_mm = df::search_t().with(prop::focal_length, 0.8).format_terms();
		assert_equal("focal.length: 0.8mm", sub_mm, "sub-millimetre focal length text");
		prop_test().focal_length(0.8).is_match(sub_mm);

		for (const auto exposure : {0.3, 1.6, 1.0 / 250.0})
		{
			const auto exposure_text = df::search_t().with(prop::exposure_time, exposure).format_terms();
			prop_test().exposure_time(exposure).is_match(exposure_text);
		}

		for (const auto sample_rate : {uint16_t{22050}, uint16_t{44100}})
		{
			const auto sample_rate_text = df::search_t().with(prop::audio_sample_rate, sample_rate).format_terms();
			prop_test().audio_sample_rate(sample_rate).is_match(sample_rate_text);
		}
	}

	// A quoted word is the word. Rewriting it into an operator changed the query's own logic
	// instead of matching the text between the quotes.
	{
		const auto quoted = df::search_t::parse("alpha \"and\" beta");
		assert_equal(3_z, quoted.terms().size(), "a quoted and is a term of its own");

		const auto bare = df::search_t::parse("alpha and beta");
		assert_equal(2_z, bare.terms().size(), "an unquoted one still joins the two either side");

		// Written back it has to stay quoted: format_terms is what the address box shows once the
		// typed text is gone, and a bare "and" there reads back as the operator again.
		const auto reread = df::search_t::parse(quoted.format_terms());
		assert_equal(3_z, reread.terms().size(), "a quoted and survives being written back");
	}

	// "-f/2.8" expands to a group matching either reading of the value: the text of a lens
	// description, or the aperture. Negated, the arms have to be joined the other way round -
	// "not either" is "neither". Left as an OR the pair read as "not the text or not the
	// aperture", which is true whenever the two readings disagree, so the exclusion let through
	// exactly the photographs it was asked to remove.
	{
		// A zoom whose description names f/3.5, on a frame actually taken at f/5.6. That is the one
		// shape where the two arms disagree, so it is the only one where the join between them can
		// be observed at all.
		const auto matches = [](const std::string_view query)
		{
			df::index_file_item file;
			file.name = "shot.jpg"_c;
			file.ft = files::file_type_from_name("shot.jpg");
			const auto md = file.safe_ps();
			md->lens = "EF 24-105mm f/3.5-5.6 IS"_c;
			md->f_number = 5.6f;

			const auto search = df::search_t::parse(query);
			const df::search_matcher matcher(search, df::date_t(2000, 1, 1).to_days());
			return matcher.match_all_terms(test_files_folder.text(), file).is_match();
		};

		assert_equal(true, matches("f/3.5"), "the lens description is one of the two readings");
		assert_equal(false, matches("-f/3.5"), "so excluding that value excludes this photograph");
		assert_equal(true, matches("f/5.6"), "the aperture it was taken at is the other reading");
		assert_equal(false, matches("-f/5.6"), "and excluding that excludes it too");
	}
}

// A cancelled walk stops where the token was raised, so what it holds is a prefix of the answer.
// Reporting that as complete let a superseded pass settle the list showing fewer items than exist
// and announce that it had finished - and because re-opening the same search leaves the search
// identity unchanged, the identity test at the publication point could not tell the two apart.
static void should_not_call_a_cancelled_query_complete(shared_test_context& stc)
{
	const auto search = df::search_t().add_selector(test_files_folder);

	auto complete_runs = 0;
	auto partial_runs = 0;

	auto cb = [&complete_runs, &partial_runs](const index_state::query_item_results& items, const bool is_complete)
	{
		if (is_complete) ++complete_runs;
		else ++partial_runs;
	};

	stc.test_index.query_items(search, cb, test_token);
	assert_equal(1, complete_runs, "a walk that ran to the end reports completion");
	assert_equal(0, partial_runs, "and reports it once");

	complete_runs = 0;
	partial_runs = 0;

	std::atomic_bool cancelled = true;
	const df::cancel_token stopped(cancelled);
	stc.test_index.query_items(search, cb, stopped);

	assert_equal(0, complete_runs, "a cancelled walk does not claim to be complete");
	assert_equal(1, partial_runs, "it still answers, but as the partial set it holds");
}

static void should_match_volume_label()
{
	// Map a drive letter to a volume label so the volume: term can be matched
	// deterministically without depending on the drives present on the machine.
	df::hash_map<char, str::cached> labels;
	labels['X'] = str::cache("Backup");

	const auto exact = df::search_t::parse("volume:Backup");
	const auto& exact_term = exact.terms().front();

	assert_equal(true, df::match_volume_label("X:\\photos\\2024", labels, exact_term), "match on drive X");
	assert_equal(true, df::match_volume_label("x:\\photos", labels, exact_term), "drive letter case-insensitive");
	assert_equal(false, df::match_volume_label("Y:\\photos", labels, exact_term), "different drive");
	assert_equal(false, df::match_volume_label("\\\\server\\share", labels, exact_term),
	             "unc path has no drive letter");

	assert_equal(true, df::match_volume_label("X:\\x", labels, df::search_t::parse("volume:BACKUP").terms().front()),
	             "label case-insensitive");
	assert_equal(true, df::match_volume_label("X:\\x", labels, df::search_t::parse("volume:Back*").terms().front()),
	             "label wildcard");
	assert_equal(false, df::match_volume_label("X:\\x", labels, df::search_t::parse("volume:Other").terms().front()),
	             "non-matching label");
}

static void should_match_date(const std::string_view query, const df::date_t d)
{
	df::index_file_item props_without_val;
	props_without_val.ft = files::file_type_from_name("test.jpg");
	props_without_val.file_modified = df::date_t(1972, 5, 25);
	props_without_val.safe_ps();

	df::index_file_item props_with_val;
	props_with_val.ft = files::file_type_from_name("test.jpg");
	props_with_val.safe_ps()->dates.add(prop::date_source::exif_original, d);

	const auto search = df::search_t::parse(query);
	df::search_matcher matcher(search, df::date_t(2000, 1, 1).to_days());

	assert_equal(true, matcher.match_all_terms(test_files_folder.text(), props_with_val).is_match(), query);
	assert_equal(false, matcher.match_all_terms(test_files_folder.text(), props_without_val).is_match(), query);
}

static void assert_parse(const std::string_view selector, const std::string_view display,
                         const std::string_view folder,
                         const bool is_recursive, const std::string_view wildcard)
{
	const df::item_selector sel(selector);

	assert_equal(display, sel.str(), "item_selector.str");
	assert_equal(folder, sel.folder().text(), "item_selector.folder");
	assert_equal(is_recursive, sel.is_recursive(), "item_selector.is_recursive");
	assert_equal(wildcard, sel.wildcard(), "item_selector.wildcard");
}

static void assert_parse(const std::string_view selector, const std::string_view folder,
                         const bool is_recursive, const std::string_view wildcard)
{
	assert_parse(selector, selector, folder, is_recursive, wildcard);
}

static void should_parse_selector()
{
	const auto p = as_platform_path;

	assert_parse(p("c:\\"), p("c:\\"), false, "*.*");
	assert_parse(p("c:\\**"), p("c:\\"), true, "*.*");
	assert_parse(p("c:\\**\\"), p("c:\\**"), p("c:\\"), true, "*.*");
	assert_parse(p("c:/**/"), p("c:\\**"), p("c:\\"), true, "*.*");
	assert_parse(p("c:\\*.jpg"), p("c:\\"), false, "*.jpg");
	assert_parse(p("c:\\temp\\*.jpg"), p("c:\\temp"), false, "*.jpg");
	assert_parse(p(R"(c:\temp\**\*.jpg)"), p("c:\\temp"), true, "*.jpg");
	assert_parse(p("c:\\temp\\***.jpg"), p("c:\\temp"), false, "***.jpg");
	assert_parse(p("c:\\temp\\?x.jpg"), p("c:\\temp"), false, "?x.jpg");
}

static void should_detect_folder_browse()
{
	// A plain folder path (with no search terms) is a folder browse. This drives the
	// "Empty Folder" vs "Nothing found" message when the view has no items.
	//
	// parse_path only accepts a folder that exists, so this uses the fixtures folder rather than a
	// system one that only happens to be there on one platform.
	const std::string folder(test_files_folder.text());

#ifdef _WIN32
	// A UNC path is a Windows spelling; elsewhere a leading backslash is an ordinary name character.
	assert_equal(true, df::search_t::parse("\\\\").is_empty(), "incomplete UNC root");
	assert_equal(true, df::search_t::parse("\\\\server").is_empty(), "incomplete UNC server");
	assert_equal(true, df::search_t::parse("\\\\server\\").is_empty(), "incomplete UNC server root");
#endif
	assert_equal(true, df::search_t::parse(folder).is_folder(), "plain folder path");
	assert_equal(true, df::search_t::parse(folder + df::preferred_path_sep + "**").is_folder(),
	             "recursive folder browse");
	assert_equal(true, df::search_t::parse(folder + df::preferred_path_sep + "*.jpg").is_folder(),
	             "folder with wildcard, no terms");

	// A search - even one scoped to a folder - is not a folder browse.
	assert_equal(false, df::search_t::parse(folder + " with:tag").is_folder(), "folder scoped search");
	assert_equal(false, df::search_t::parse("dog").is_folder(), "term with no selector");
	assert_equal(false, df::search_t::parse("2014").is_folder(), "date term");
	assert_equal(false, df::search_t().is_folder(), "empty search");
}

static bool contains(const std::vector<platform::file_info>& files, const std::string_view find)
{
	for (const auto& f : files)
	{
		if (icmp(f.name, find) == 0)
		{
			return true;
		}
	}

	return false;
}

static bool contains(const std::vector<platform::folder_info>& files, const std::string_view find)
{
	for (const auto& f : files)
	{
		if (icmp(f.name, find) == 0)
		{
			return true;
		}
	}

	return false;
}

static void should_select_files()
{
	const auto recursive = df::item_selector(test_files_folder, true);
	assert_equal(true, contains(platform::select_files(recursive, true), "Screws.CR2"),
	             "files from sub folders");
	assert_equal(true, contains(platform::select_files(recursive, true), "Cube.png"), "files from sub folders");
	assert_equal(true, contains(platform::select_files(recursive, true), "Test.jpg"), "files from sub folders");
	assert_equal(false, contains(platform::select_folders(recursive, true), "raw"), "no folders");

	const auto not_recursive = df::item_selector(test_files_folder, false);
	assert_equal(false, contains(platform::select_files(not_recursive, true), "Screws.CR2"),
	             "no files from sub folder");
	assert_equal(true, contains(platform::select_files(not_recursive, true), "Cube.png"),
	             "files from sub folders");
	assert_equal(true, contains(platform::select_files(not_recursive, true), "Test.jpg"),
	             "files from sub folders");
	assert_equal(true, contains(platform::select_folders(not_recursive, true), "raw"), "include folders");

	const auto recursive_png = df::item_selector(test_files_folder, true, "*.png");
	assert_equal(false, contains(platform::select_files(recursive_png, true), "Screws.CR2"),
	             "no files from sub folder");
	assert_equal(true, contains(platform::select_files(recursive_png, true), "Cube.png"),
	             "files from sub folders");
	assert_equal(false, contains(platform::select_files(recursive_png, true), "Test.jpg"),
	             "files from sub folders");
	assert_equal(false, contains(platform::select_folders(recursive_png, true), "raw"), "include folders");

	const auto not_recursive_cr2 = df::item_selector(test_files_folder, false, "*.cr2");
	assert_equal(false, contains(platform::select_files(not_recursive_cr2, true), "Screws.CR2"),
	             "no files from sub folder");
	assert_equal(false, contains(platform::select_files(not_recursive_cr2, true), "Cube.png"),
	             "files from sub folders");
	assert_equal(false, contains(platform::select_files(not_recursive_cr2, true), "Test.jpg"),
	             "files from sub folders");
	assert_equal(true, contains(platform::select_folders(not_recursive_cr2, true), "raw"), "include folders");

	const auto recursive_cr2 = df::item_selector(test_files_folder, true, "*.cr2");
	assert_equal(true, contains(platform::select_files(recursive_cr2, true), "Screws.CR2"),
	             "no files from sub folder");
	assert_equal(false, contains(platform::select_files(recursive_cr2, true), "Cube.png"),
	             "files from sub folders");
	assert_equal(false, contains(platform::select_files(recursive_cr2, true), "Test.jpg"),
	             "files from sub folders");
	assert_equal(false, contains(platform::select_folders(recursive_cr2, true), "raw"), "include folders");
}

static void should_preserve_query_when_toggling_recursive_selectors()
{
	auto search = df::search_t::parse("#family @photo created:2020");
	search.add_selector(df::item_selector(test_files_folder, false));
	search.add_selector(df::item_selector(test_files_folder, false, "*.jpg"));

	auto recursive = search;
	recursive.set_recursive(true);
	assert_equal(true, recursive.has_recursive_selector(), "selectors become recursive");
	assert_equal(search.terms().size(), recursive.terms().size(), "terms are preserved");
	assert_equal(search.selectors().size(), recursive.selectors().size(), "selector count is preserved");
	assert_equal("*.jpg", recursive.selectors().back().wildcard(), "selector wildcard is preserved");
	assert_equal(true, recursive.has_media_type(), "media type is preserved");
	assert_equal(2020, recursive.find_date_parts().year, "date is preserved");

	recursive.remove_recursive();
	assert_equal(search.format_terms(), recursive.format_terms(), "recursive toggle round-trips");

	auto simple = df::search_t().add_selector(test_files_folder);
	simple.set_recursive(true);
	assert_equal(true, simple.has_recursive_selector(), "simple folder can still become recursive");
}

// A recursive selector names a folder, not a string. Normalisation drops the trailing separator, so
// a raw prefix compare claims every sibling whose name the root is a prefix of - `C:\Pics` swallowing
// `C:\Pictures` is a scope that silently holds files the user never pointed at.
static void should_bound_a_recursive_selector_at_a_folder_boundary()
{
	const df::folder_path root("c:\\Pics");
	const df::item_selector pics(root, true);

	assert_equal(true, pics.match(df::file_path(root, "a.jpg")), "the root itself is in scope");
	assert_equal(true, pics.match(df::file_path(df::folder_path("c:\\Pics\\holiday"), "b.jpg")),
	             "and so is a folder below it");
	assert_equal(false, pics.match(df::file_path(df::folder_path("c:\\Pictures"), "c.jpg")),
	             "a sibling the root merely prefixes is not");
	assert_equal(false, pics.match(df::file_path(df::folder_path("c:\\PicsOld\\holiday"), "d.jpg")),
	             "and neither is anything under one");

	// Non-recursive was already an equality test; pinned so the two branches cannot drift apart.
	const df::item_selector one_folder(root, false);
	assert_equal(true, one_folder.match(df::file_path(root, "a.jpg")), "one folder holds its own files");
	assert_equal(false, one_folder.match(df::file_path(df::folder_path("c:\\Pictures"), "c.jpg")),
	             "and claims no sibling");
}

// A related search answers with the closest matches on each axis, so the collector has to keep the
// best `limit` and drop the rest whatever order the index happened to hand them over in.
static void should_bound_related_results_by_closeness()
{
	df::bounded_best<int> slots;
	slots.limit(3);

	const df::folder_path folder("c:\\test");

	for (const auto distance : {50, 10, 40, 20, 30})
	{
		slots.offer(distance, df::file_path(folder, std::format("{}.jpg", distance)), distance);
	}

	const auto best = slots.take();

	assert_equal(3u, static_cast<uint32_t>(best.size()), "cap holds");
	assert_equal(10, best[0].value, "closest first");
	assert_equal(20, best[1].value, "then next closest");
	assert_equal(30, best[2].value, "furthest kept is the third closest");
	assert_equal(true, slots.empty(), "taking drains the collector");
}

// Which items survive a full axis must not depend on the order folders were walked in, or the same
// search would answer differently each time it ran.
static void should_break_related_ties_on_path()
{
	const df::folder_path folder("c:\\test");
	const df::file_path a(folder, "a.jpg");
	const df::file_path b(folder, "b.jpg");
	const df::file_path c(folder, "c.jpg");

	const auto survivors = [&](const df::file_paths& order)
	{
		df::bounded_best<int> slots;
		slots.limit(2);
		for (const auto& path : order) slots.offer(7, path, 0);

		std::string result;
		for (const auto& e : slots.take()) result += e.path.name();
		return result;
	};

	assert_equal("a.jpgb.jpg", survivors({a, b, c}), "equal distances keep the first paths");
	assert_equal("a.jpgb.jpg", survivors({c, b, a}), "and do not depend on offer order");
	assert_equal("a.jpgb.jpg", survivors({b, c, a}), "or on which arrived first");
}

static void should_group_related_results_by_axis()
{
	df::related_collector<int> collector;
	const df::folder_path folder("c:\\test");

	collector.offer({df::related_axis::location, 5}, df::file_path(folder, "place.jpg"), 4);
	collector.offer({df::related_axis::duplicate, 0}, df::file_path(folder, "copy.jpg"), 1);
	collector.offer({df::related_axis::time, 5}, df::file_path(folder, "when.jpg"), 3);
	collector.offer({df::related_axis::album, 5}, df::file_path(folder, "album.jpg"), 2);

	assert_equal(4u, static_cast<uint32_t>(collector.size()), "every axis holds its own matches");
	assert_equal(1u, static_cast<uint32_t>(collector.size(df::related_axis::time)), "per axis count");

	std::string order;
	collector.drain([&order](const df::file_path, int&& value) { order += std::to_string(value); });

	assert_equal("1234", order, "axes drain in priority order");
}

static df::index_file_item make_related_candidate(const std::string_view name)
{
	df::index_file_item result;
	result.ft = files::file_type_from_name(name);
	result.name = str::cache(name);
	return result;
}

static std::string_view related_axis_name(const df::related_axis axis)
{
	switch (axis)
	{
	case df::related_axis::duplicate: return "duplicate";
	case df::related_axis::album: return "album";
	case df::related_axis::series: return "series";
	case df::related_axis::time: return "time";
	default: return "location";
	}
}

static std::string_view matched_axis(const df::search_result& match)
{
	return related_axis_name(df::related_axis_of(match.type));
}

// Names each result with the relation that earned it, so a related search is asserted on what it
// found and why rather than on a bare count.
static std::string related_result_summary(index_state& index, const df::search_t& search)
{
	std::vector<std::string> found;

	auto cb = [&found](const index_state::query_item_results& items, const bool)
	{
		for (const auto& i : items)
		{
			found.emplace_back(std::format("{}:{}", matched_axis(i.match), i.path.name()));
		}
	};

	index.query_items(search, cb, test_token);
	std::ranges::sort(found);

	std::string result;
	for (const auto& text : found)
	{
		if (!result.empty()) result += ' ';
		result += text;
	}
	return result;
}

static df::related_info make_related_anchor()
{
	df::related_info anchor;
	anchor.path = df::file_path(test_files_folder, "anchor.jpg");
	anchor.name = str::cache("anchor.jpg");
	anchor.ft = files::file_type_from_name("anchor.jpg");
	anchor.size = df::file_size(1000);
	anchor.is_loaded = true;
	return anchor;
}

static df::search_result match_related(const df::related_info& anchor, const std::string_view name,
                                       const df::index_file_item& candidate)
{
	const auto search = df::search_t().related(anchor);
	const df::search_matcher matcher(search);
	return matcher.match_item(df::file_path(test_files_folder, name), candidate);
}

static void should_match_related_by_album()
{
	auto anchor = make_related_anchor();
	anchor.album = str::cache("Album");
	anchor.disk = df::xy8::make(1, 1);
	anchor.track = df::xy8::make(3, 12);

	auto candidate = make_related_candidate("track5.mp3");
	candidate.safe_ps()->album = str::cache("Album");
	candidate.safe_ps()->disk = df::xy8::make(1, 1);
	candidate.safe_ps()->track = df::xy8::make(5, 12);

	const auto match = match_related(anchor, "track5.mp3", candidate);
	assert_equal(true, match.is_match(), "same album is related");
	assert_equal("album", matched_axis(match), "album axis");
	assert_equal(2, static_cast<int>(match.distance), "distance is the gap in track order");

	// Two artists can both have a "Greatest Hits", so a named artist on each side has to agree.
	anchor.album_artist = str::cache("One");
	candidate.safe_ps()->album_artist = str::cache("Another");
	assert_equal(false, match_related(anchor, "track5.mp3", candidate).is_match(),
	             "a different album artist is not the same album");
}

static void should_match_related_by_series()
{
	auto anchor = make_related_anchor();
	anchor.show = str::cache("Show");
	anchor.season = 2;
	anchor.episode = df::xy8::make(5, 10);

	auto candidate = make_related_candidate("episode7.mkv");
	candidate.safe_ps()->show = str::cache("Show");
	candidate.safe_ps()->season = 2;
	candidate.safe_ps()->episode = df::xy8::make(7, 10);

	const auto match = match_related(anchor, "episode7.mkv", candidate);
	assert_equal(true, match.is_match(), "same series is related");
	assert_equal("series", matched_axis(match), "series axis");
	assert_equal(2, static_cast<int>(match.distance), "distance is the gap in episode order");

	candidate.safe_ps()->show = str::cache("Other show");
	assert_equal(false, match_related(anchor, "episode7.mkv", candidate).is_match(), "a different show is unrelated");
}

static void should_match_related_by_capture_time()
{
	auto anchor = make_related_anchor();
	anchor.metadata_created = df::date_t(2020, 5, 1, 12, 0, 0);

	auto candidate = make_related_candidate("burst.jpg");
	candidate.safe_ps()->dates.add(prop::date_source::exif_original, df::date_t(2020, 5, 1, 12, 0, 30));

	const auto match = match_related(anchor, "burst.jpg", candidate);
	assert_equal(true, match.is_match(), "a near capture time is related");
	assert_equal("time", matched_axis(match), "time axis");
	assert_equal(30, static_cast<int>(match.distance), "distance is the gap in seconds");

	candidate.safe_ps()->dates.clear();
	candidate.safe_ps()->dates.add(prop::date_source::exif_original, df::date_t(2020, 5, 3, 12, 0, 0));
	assert_equal(false, match_related(anchor, "burst.jpg", candidate).is_match(),
	             "outside the window is not the same time");

	// A whole collection copied in one pass shares a file time, which would otherwise make every
	// item in it equally and meaninglessly close.
	auto file_time_only = make_related_candidate("copied.jpg");
	file_time_only.file_created = anchor.metadata_created;
	assert_equal(false, match_related(anchor, "copied.jpg", file_time_only).is_match(),
	             "file time alone is not capture time");
}

static void should_match_related_by_capture_place()
{
	auto anchor = make_related_anchor();
	anchor.gps = gps_coordinate(51.5007, -0.1246);

	auto candidate = make_related_candidate("nearby.jpg");
	candidate.safe_ps()->coordinate = gps_coordinate(51.5033, -0.1195);

	const auto match = match_related(anchor, "nearby.jpg", candidate);
	assert_equal(true, match.is_match(), "a near capture place is related");
	assert_equal("location", matched_axis(match), "location axis");
	assert_equal(true, match.distance > 0 && match.distance < 1000, "distance is metres apart");

	candidate.safe_ps()->coordinate = gps_coordinate(48.8584, 2.2945);
	assert_equal(false, match_related(anchor, "nearby.jpg", candidate).is_match(),
	             "outside the window is not the same place");
}

// An item can be near in time and in place at once. It belongs to one group, so the strongest
// relation decides which, and it is never listed twice.
static void should_report_the_strongest_related_axis()
{
	auto anchor = make_related_anchor();
	anchor.metadata_created = df::date_t(2020, 5, 1, 12, 0, 0);
	anchor.gps = gps_coordinate(51.5007, -0.1246);
	anchor.album = str::cache("Album");

	auto candidate = make_related_candidate("everything.jpg");
	candidate.safe_ps()->dates.add(prop::date_source::exif_original, anchor.metadata_created);
	candidate.safe_ps()->coordinate = anchor.gps;
	candidate.safe_ps()->album = str::cache("Album");

	assert_equal("album", matched_axis(match_related(anchor, "everything.jpg", candidate)),
	             "album outranks time and place");

	candidate.safe_ps()->album.clear();
	assert_equal("time", matched_axis(match_related(anchor, "everything.jpg", candidate)),
	             "time outranks place");

	candidate.safe_ps()->dates.clear();
	assert_equal("location", matched_axis(match_related(anchor, "everything.jpg", candidate)),
	             "place is the weakest relation");
}

// A file matched by two overlapping selectors is still one file. `folders_scanned` only stops a
// folder being walked twice within one selector, so the result set is what has to hold the rule.
static void should_list_an_item_once(shared_test_context& stc)
{
	stc.lazy_load_index();

	const df::item_selector whole_tree(test_files_folder, true);
	const df::item_selector sub_folder(test_files_folder.combine("raw"), true);

	const auto tree_only = count_search_results(stc.test_index, df::search_t().add_selector(whole_tree));
	const auto overlapping = count_search_results(stc.test_index,
	                                              df::search_t().add_selector(whole_tree).add_selector(sub_folder));

	assert_equal(tree_only, overlapping, "an overlapping selector adds no items");
}

static void should_match_related(shared_test_context& stc)
{
	stc.lazy_load_index();

	const df::file_path path(test_files_folder, "Test.jpg");
	const auto i = std::make_shared<df::item_element>(path, stc.test_index.find_item(path));
	stc.test_index.scan_item(i, true, false);

	df::related_info r;
	r.load(i);

	// The item the search started at, then the perceptual copies that duplicate search can also see.
	assert_equal("duplicate:Small.jpg duplicate:Test.jpg duplicate:Test180.jpg duplicate:Test270.jpg duplicate:Test90.jpg",
	             related_result_summary(stc.test_index, df::search_t().related(r)), "Related");

	r.group = 0;
	df::index_file_item different_size = stc.test_index.find_item(path);
	different_size.name = str::cache("different-name.jpg");
	different_size.size = df::file_size(different_size.size.to_int64() + 1);
	const auto related_search = df::search_t().related(r);
	const df::search_matcher matcher(related_search);

	// Equal CRC is evidence of a copy only when the sizes agree, so a bumped size has to fall through
	// the duplicate rules. It is still the same photo, so capture time catches it.
	assert_equal("time", matched_axis(matcher.match_item({}, different_size)),
	             "Related CRC requires equal size");
}

static uint64_t phash_bits(const std::initializer_list<int> bits)
{
	uint64_t result = 0;

	for (const auto bit : bits)
	{
		result |= 1ull << bit;
	}

	return result;
}

static void should_not_treat_anchored_perceptual_group_as_pairwise_duplicate()
{
	const auto base = phash_bits({
		1, 2, 3, 4, 5, 6, 7, 8,
		9, 10, 11, 12, 13, 14, 15, 16,
		17, 18, 19, 20, 21, 22, 23, 24,
		25, 26, 27, 28, 29, 30, 31
	});
	const auto left = (base & ~(1ull << 1) & ~(1ull << 2) & ~(1ull << 3)) |
		(1ull << 32) | (1ull << 33) | (1ull << 34);
	const auto right = (base & ~(1ull << 4) & ~(1ull << 5) & ~(1ull << 6)) |
		(1ull << 35) | (1ull << 36) | (1ull << 37);

	df::related_info related;
	related.path = df::file_path("c:\\photos\\left.jpg");
	related.name = "left.jpg"_c;
	related.ft = files::file_type_from_name("left.jpg");
	related.group = 7;
	related.duplicate_grade = df::copy_grade::same_picture;
	related.metadata_created = df::date_t(2026, 1, 1, 12, 0, 0);
	related.phash = {left, 0, 0, 0};
	related.dimensions = {100, 100};
	related.is_loaded = true;

	df::index_file_item candidate;
	candidate.name = "right.jpg"_c;
	candidate.ft = files::file_type_from_name(candidate.name);
	auto candidate_metadata = std::make_shared<prop::item_metadata>();
	candidate_metadata->dates.add(prop::date_source::exif_original, related.metadata_created);
	candidate_metadata->width = 100;
	candidate_metadata->height = 100;
	candidate.metadata = candidate_metadata;
	candidate.phash = df::make_picture_hashes({right, 0, 0, 0});
	candidate.duplicates = df::duplicate_info{.group = 7, .count = 3, .grade = df::copy_grade::same_picture};

	assert_equal(12, crypto::phash_distance(left, right), "non-anchor pair distance");
	assert_equal(false, dup_match(related, candidate).matched(),
	             "sharing an anchored perceptual group is not pairwise duplicate evidence");

	candidate.phash = df::make_picture_hashes({base, 0, 0, 0});
	assert_equal(static_cast<int>(df::copy_grade::same_picture),
	             static_cast<int>(dup_match(related, candidate).grade),
	             "direct perceptual evidence against the queried item still matches");

	related.duplicate_crowded = true;
	assert_equal(false, dup_match(related, candidate).matched(),
	             "a crowd-declined burst frame is not a related duplicate");
	related.duplicate_crowded = false;
	candidate.duplicates = df::duplicate_info{
		.group = 7, .count = 3, .same_picture_crowded = 1, .grade = df::copy_grade::same_picture
	};
	assert_equal(false, dup_match(related, candidate).matched(),
	             "a candidate from a crowd-declined burst is not a related duplicate");
}

static void should_use_only_exact_groups_for_related_duplicate_shortcut()
{
	const auto base = phash_bits({
		1, 2, 3, 4, 5, 6, 7, 8,
		9, 10, 11, 12, 13, 14, 15, 16,
		17, 18, 19, 20, 21, 22, 23, 24,
		25, 26, 27, 28, 29, 30, 31
	});
	const auto turned = (base & ~(1ull << 1) & ~(1ull << 2) & ~(1ull << 3)) |
		(1ull << 32) | (1ull << 33) | (1ull << 34);

	df::related_info related;
	related.path = df::file_path("c:\\photos\\original.jpg");
	related.name = "original.jpg"_c;
	related.ft = files::file_type_from_name("original.jpg");
	related.group = 11;
	related.exact_group = 1;
	related.duplicate_grade = df::copy_grade::identical;
	related.metadata_created = df::date_t(2026, 1, 1, 12, 0, 0);
	related.phash = {base, 0, 0, 0};
	related.dimensions = {100, 100};
	related.is_loaded = true;

	auto candidate_metadata = std::make_shared<prop::item_metadata>();
	candidate_metadata->dates.add(prop::date_source::exif_original, related.metadata_created);
	candidate_metadata->width = 100;
	candidate_metadata->height = 100;

	df::index_file_item candidate;
	candidate.name = "turned.jpg"_c;
	candidate.ft = files::file_type_from_name(candidate.name);
	candidate.metadata = candidate_metadata;
	candidate.phash = df::make_picture_hashes({turned, 0, 0, 0});
	candidate.duplicates = df::duplicate_info{.group = 11, .count = 4, .grade = df::copy_grade::identical};
	candidate.exact_duplicate_group = 2;

	assert_equal(static_cast<int>(df::copy_grade::same_picture),
	             static_cast<int>(dup_match(related, candidate).grade),
	             "a mixed group does not reuse another exact pair's grade");

	candidate.exact_duplicate_group = related.exact_group;
	assert_equal(static_cast<int>(df::copy_grade::identical),
	             static_cast<int>(dup_match(related, candidate).grade),
	             "the exact shortcut still applies within the same exact subgroup");
}

static void should_clear_exact_duplicate_shortcut_outside_collection()
{
	const auto first_root = _temps.next_folder("outside-exact-first");
	const auto second_root = _temps.next_folder("outside-exact-second");
	const auto first_a = first_root.combine_file("first-a.jpg");
	const auto first_b = first_root.combine_file("first-b.jpg");
	const auto second_a = second_root.combine_file("second-a.jpg");
	const auto second_b = second_root.combine_file("second-b.jpg");
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), first_a, false, false);
	platform::copy_file(test_files_folder.combine_file("Test.jpg"), first_b, false, false);
	platform::copy_file(test_files_folder.combine_file("Small.jpg"), second_a, false, false);
	platform::copy_file(test_files_folder.combine_file("Small.jpg"), second_b, false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);

	df::index_roots first_roots;
	first_roots.folders.emplace(first_root);
	index.index_roots(first_roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);
	index.update_predictions();
	assert_equal(true, index.find_item(first_a).exact_duplicate_group.load() != 0,
	             "first collection earned an exact group");

	df::index_roots second_roots;
	second_roots.folders.emplace(second_root);
	index.index_roots(second_roots);
	index.index_folders(test_token);
	index.scan_uncached(test_token);
	index.update_predictions();

	df::related_info related;
	related.path = first_a;
	related.ft = files::file_type_from_name(first_a);
	related.size = index.find_item(first_a).size;
	related.crc32c = index.find_item(first_a).crc32c.load();
	related.exact_group = index.find_item(first_a).exact_duplicate_group.load();
	related.is_loaded = true;

	assert_equal(0u, related.exact_group, "outside collection rows do not keep stale exact group ids");
	assert_equal(false, dup_match(related, index.find_item(second_a)).matched(),
	             "a stale outside exact id cannot match an unrelated current exact group");
}

// A related search written as text - a favorite, a saved search, or typed into the address
// box - carries only the path, so the query must resolve the rest instead of matching
// against unloaded fields.
static void should_match_related_from_text(shared_test_context& stc)
{
	stc.lazy_load_index();

	const df::file_path path(test_files_folder, "Test.jpg");
	const auto i = std::make_shared<df::item_element>(path, stc.test_index.find_item(path));
	stc.test_index.scan_item(i, true, false);

	df::related_info r;
	r.load(i);

	const auto reparsed = df::search_t::parse(df::search_t().related(r).format_terms());

	assert_equal(true, reparsed.has_related(), "related survives a text round trip");
	assert_equal(path.str(), reparsed.related().path.str(), "related path");

	// Every field the axes compare has to be recovered from the index, so a favorite answers with the
	// same relations as the command that created it.
	assert_equal(related_result_summary(stc.test_index, df::search_t().related(r)),
	             related_result_summary(stc.test_index, reparsed), "related search from text");
}

static void should_parse_search()
{
	constexpr auto message = "Should tokenize search";
	search_tokenizer t;
	const auto parts = t.parse(
		"  (#tree or house)    -nega-tive aperture:f/2.0   's\"om:e#inv-lid~s' -description:\"hello world\" tag : spaces c:\\test 12:00 or -#tree|ant");

	assert_equal(parts[0].modifier.positive, true, message);
	assert_equal(parts[0].modifier.logical_op, df::search_term_modifier_bool::none, message);
	assert_equal(parts[0].modifier.begin_group, 1, message);
	assert_equal(parts[0].modifier.end_group, 0, message);
	assert_equal(parts[0].scope, "tag", message);
	assert_equal(parts[0].term, "tree", message);

	assert_equal(parts[1].modifier.positive, true, message);
	assert_equal(parts[1].modifier.logical_op, df::search_term_modifier_bool::m_or, message);
	assert_equal(parts[1].modifier.begin_group, 0, message);
	assert_equal(parts[1].modifier.end_group, 1, message);
	assert_equal(parts[1].scope.empty(), true, message);
	assert_equal(parts[1].term, "house", message);

	assert_equal(parts[2].modifier.positive, false, message);
	assert_equal(parts[2].modifier.logical_op, df::search_term_modifier_bool::none, message);
	assert_equal(parts[2].modifier.begin_group, 0, message);
	assert_equal(parts[2].modifier.end_group, 0, message);
	assert_equal(parts[2].scope.empty(), true, message);
	assert_equal(parts[2].term, "nega-tive", message);

	assert_equal(parts[3].modifier.positive, true, message);
	assert_equal(parts[3].modifier.logical_op, df::search_term_modifier_bool::none, message);
	assert_equal(parts[3].modifier.begin_group, 0, message);
	assert_equal(parts[3].modifier.end_group, 0, message);
	assert_equal(parts[3].scope, "aperture", message);
	assert_equal(parts[3].term, "f/2.0", message);

	assert_equal(parts[4].modifier.positive, true, message);
	assert_equal(parts[4].scope.empty(), true, message);
	assert_equal(parts[4].term, "s\"om:e#inv-lid~s", message);

	assert_equal(parts[5].modifier.positive, false, message);
	assert_equal(parts[5].scope, "description", message);
	assert_equal(parts[5].term, "hello world", message);

	assert_equal(parts[6].scope, "tag", message);
	assert_equal(parts[6].term, "spaces", message);

	assert_equal(parts[7].modifier.positive, true, message);
	assert_equal(parts[7].scope.empty(), true, message);
	assert_equal(parts[7].term, "c:\\test", message);

	assert_equal(parts[8].modifier.positive, true, message);
	assert_equal(parts[8].scope.empty(), true, message);
	assert_equal(parts[8].term, "12:00", message);

	assert_equal(parts[9].modifier.positive, false, message);
	assert_equal(parts[9].modifier.logical_op, df::search_term_modifier_bool::m_or, message);
	assert_equal(parts[9].scope, "tag", message);
	assert_equal(parts[9].term, "tree", message);

	assert_equal(parts[10].modifier.positive, true, message);
	assert_equal(parts[10].modifier.logical_op, df::search_term_modifier_bool::m_or, message);
	assert_equal(parts[10].scope.empty(), true, message);
	assert_equal(parts[10].term, "ant", message);

	const auto location_parts = t.parse("loc: London, UK 5km");
	assert_equal(3, static_cast<int>(location_parts.size()), "a comma separates without creating a term");
	assert_equal("loc", location_parts[0].scope, "location scope is retained");
	assert_equal("London", location_parts[0].term, "location term");
	assert_equal(true, location_parts[1].after_comma, "a comma binds the following token");
	assert_equal("UK", location_parts[1].term, "comma bound token");
	assert_equal(false, location_parts[2].after_comma, "a space does not bind");

	// A comma-bound '-' is a sign only where a latitude precedes it. Everywhere else the comma
	// separates terms and the '-' negates the one after it, as it did before coordinates had signs.
	const auto coordinate_parts = t.parse("loc:51.5142,-0.0985");
	assert_equal(2, static_cast<int>(coordinate_parts.size()), "a coordinate tokenizes as two components");
	assert_equal(true, coordinate_parts[1].modifier.positive, "a longitude's sign is not a negation");
	assert_equal("-0.0985", coordinate_parts[1].term, "the sign stays with the longitude");

	const auto excluded_year = t.parse("sunset, -2019");
	assert_equal(2, static_cast<int>(excluded_year.size()), "a comma separates two terms");
	assert_equal(false, excluded_year[1].modifier.positive, "a '-' after an ordinary term still negates");
	assert_equal("2019", excluded_year[1].term, "and the number is the negated term");

	const auto excluded_from_place = t.parse("loc:London,-2019");
	assert_equal(false, excluded_from_place.back().modifier.positive, "a place name is not a latitude");
	assert_equal("2019", excluded_from_place.back().term, "so the '-' after it negates");
}

static void assert_date_shift(df::search_t d, const std::string_view expected_prev,
                              const std::string_view expected_next)
{
	d.next_date(false);
	assert_equal(expected_prev, d.text(), "prev date");
	d.next_date(true);
	assert_equal(expected_next, d.text(), "next date");
}

static void should_next_date_search()
{
	assert_date_shift(df::search_t().day(25, 5, 1972), "1972-may-24", "1972-may-25");
	assert_date_shift(df::search_t().day(1, 1, 2020), "2019-dec-31", "2020-jan-1");
	assert_date_shift(df::search_t().day(1, 1, 0), "dec-31", "jan-1");
	assert_date_shift(df::search_t().year(2010), "2009", "2010");
	assert_date_shift(df::search_t().month(1), "December", "January");
	assert_date_shift(df::search_t().month(1).year(2010), "2009-dec", "2010-jan");

	assert_date_shift(df::search_t().day(1, 1, 2020, df::date_parts_prop::modified), "modified:2019-dec-31",
	                  "modified:2020-jan-1");
	assert_date_shift(df::search_t().day(1, 1, 0, df::date_parts_prop::modified), "modified:dec-31",
	                  "modified:jan-1");
	assert_date_shift(df::search_t().year(2010, df::date_parts_prop::created), "created:2009", "created:2010");
	assert_date_shift(df::search_t().month(1, df::date_parts_prop::created).year(2010, df::date_parts_prop::created),
	                  "created:2009-dec", "created:2010-jan");
	assert_date_shift(df::search_t().year(2010, df::date_parts_prop::original), "original:2009", "original:2010");

	// A leap February has 29 days, and the step read the month without its year - so navigating
	// across the end of February 2024 jumped straight over the 29th in both directions.
	assert_date_shift(df::search_t().day(29, 2, 2024), "2024-feb-28", "2024-feb-29");
	assert_date_shift(df::search_t().day(1, 3, 2024), "2024-feb-29", "2024-mar-1");

	// 1900 is not a leap year, and a year-less February keeps the 28-day reading it always had.
	assert_date_shift(df::search_t().day(1, 3, 1900), "1900-feb-28", "1900-mar-1");
	assert_date_shift(df::search_t().day(1, 3, 0), "feb-28", "mar-1");
}

static void should_parse_search_input()
{
	const auto base_folder = std::string(test_files_folder.text());

	null_state_strategy ss;
	null_async_strategy as;
	const view_host_base_ptr view;
	const location_cache locations;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);
	s.open(view, test_files_folder.text());
	s.open(view, "..");

	assert_equal(test_files_folder.parent().text(), s.search().text(), "Parse ..");

	s.open(view, test_files_folder.text());
	s.open(view, "**");

	assert_equal(base_folder + df::preferred_path_sep + "**"s, s.search().text(), "Parse **");

	s.open(view, test_files_folder.text());
	s.open(view, "aaa");

	assert_equal("aaa"s, s.search().text(), "Parse aaa");

	s.open(view, "\"aaa\"");
	assert_equal("\"aaa\""s, s.search().text(), "Parse \"aaa\"");
}

static void should_parent()
{
	const auto folder = df::folder_path(as_platform_path("c:\\windows\\system32"));
	assert_equal(as_platform_path("c:\\windows"), folder.parent().text(), "parent test");
	assert_equal(as_platform_path("c:\\"), folder.parent().parent().text(), "parent test");
	assert_equal(as_platform_path("c:\\"), folder.parent().parent().parent().text(), "parent test");

	const auto base_folder = std::string(test_files_folder.text());
	null_state_strategy ss;
	null_async_strategy as;
	location_cache locations;
	view_host_base_ptr view;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());
	s.view_mode(view_type::items);

	df::index_roots roots;
	roots.folders = {test_files_folder};
	s.item_index.index_roots(roots);
	s.item_index.index_folders(test_token);

	s.open(view, base_folder + " test"s);
	assert_equal(base_folder, s.parent_search().parent.text(), "parent test");

	s.open(view, base_folder + df::preferred_path_sep + "raw"s);
	const auto raw_parent = s.parent_search();
	assert_equal(base_folder, raw_parent.parent.text(), "parent \\raw");
	assert_equal("raw", raw_parent.name, "parent name \\raw");

	s.open(view, base_folder + df::preferred_path_sep + "*.png"s);
	assert_equal(base_folder, s.parent_search().parent.text(), "parent *.png");

	s.open(view, base_folder + " @photo"s);
	assert_equal(base_folder, s.parent_search().parent.text(), "parent @photo");

	s.open(view, "@photo");
	assert_equal({}, s.parent_search().parent.text(), "parent @photo");

	s.open(view, "@photo");
	s.update_item_groups();
	s.select_next(view, true, false, false);
	s.update_selection();
	// Parent broadens the query. It must not switch to a folder taken from the selection -
	// that would be a different scope, not a wider one.
	assert_equal({}, s.parent_search().parent.text(), "parent @photo with selection");

	// DATES
	s.open(view, "1972-may-25");
	assert_equal("1972-may", s.parent_search().parent.text(), "parent");

	s.open(view, "may-25");
	assert_equal("May", s.parent_search().parent.text(), "parent");

	s.open(view, "1972-may");
	assert_equal("1972", s.parent_search().parent.text(), "parent");

	s.open(view, "2009 December");
	assert_equal("2009", s.parent_search().parent.text(), "parent");

	// A drive root has nothing wider to show, so Parent reports no parent and the command is
	// disabled rather than re-running the same query.
	const auto root = as_platform_path("c:\\");
	// A folder scope only survives if the folder is real, so this uses the fixtures folder rather
	// than a system one that only exists on one platform.
	const std::string scope_folder(test_files_folder.text());

	assert_equal(true, find_parent_search(df::search_t::parse(root)).parent.is_empty(), "parent of drive root");

	// A wildcard or recursive scope at a root still broadens - to the plain root folder.
	assert_equal(root, find_parent_search(df::search_t::parse(as_platform_path("c:\\*.png"))).parent.text(),
	             "parent of root *.png");
	assert_equal(root, find_parent_search(df::search_t::parse(as_platform_path("c:\\**"))).parent.text(),
	             "parent of root **");

	// Terms are dropped one at a time, dates first, whether or not a folder selector is present.
	const auto folder_photo_date = find_parent_search(
		df::search_t::parse(scope_folder + " @photo 1972-may")).parent;
	assert_equal(true, folder_photo_date.has_media_type(), "parent keeps the media type");
	assert_equal(true, folder_photo_date.has_selector(), "parent keeps the folder scope");
	assert_equal(1972, folder_photo_date.find_date_parts().year, "parent steps the date first");
	assert_equal(0, folder_photo_date.find_date_parts().month, "parent steps the date first");

	assert_equal(scope_folder, find_parent_search(df::search_t::parse(scope_folder + " @photo")).parent.text(),
	             "parent then drops the media type");

	// A place inside a state inside a country steps straight out to the country. Location terms are
	// written most specific first, so dropping the last term would widen the broadest end instead.
	const auto place = df::search_t()
	                   .location("London", df::location_level::place)
	                   .location("England", df::location_level::state)
	                   .location("United Kingdom", df::location_level::country);
	const auto place_parent = find_parent_search(place).parent;
	assert_equal(1, static_cast<int>(place_parent.terms().size()), "parent keeps one location term");
	assert_equal("United Kingdom", place_parent.terms().front().text, "parent keeps the country");

	// The country is then the whole location, so the next step drops it.
	assert_equal(true, find_parent_search(place_parent).parent.terms().empty(), "parent of a country drops it");
}

static void should_escape()
{
	const auto base_folder = std::string(test_files_folder.text());

	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	const view_host_base_ptr view;
	index_state index(as, locations);
	view_state s(ss, as, index, make_test_player());

	df::index_roots roots;
	roots.folders = {test_files_folder};
	s.item_index.index_roots(roots);
	s.item_index.index_folders(test_token);

	s.view_mode(view_type::items);
	s.open(view, base_folder + df::preferred_path_sep + "**"s);
	s.is_full_screen = true;
	assert_equal(true, s.escape(view), "fullscreen escape handled");
	assert_equal(1, ss.toggle_full_screen_count, "fullscreen escape toggled fullscreen");
	assert_equal(base_folder + df::preferred_path_sep + "**"s, s.search().text(),
	             "fullscreen escape preserves search");
	s.is_full_screen = false;
	s.escape(view);
	assert_equal(base_folder, s.search().text(), "parent escape **");
}

// locations.md 3.5: `@remote` is a built-in location class -- a closed word, no argument -- that
// selects the items step 5 of the attribution ladder refuses to name.
static void should_select_remote_items()
{
	assert_equal("@remote", df::search_t::parse("@remote").format_terms(), "the canonical spelling");
	assert_equal("@remote", df::search_t::parse("loc:remote").format_terms(),
	             "the guessable spelling canonicalizes to the class");
	assert_equal("-@remote", df::search_t::parse("-@remote").format_terms(), "negation");
	assert_equal(search_presence_mask::location,
	             df::search_t::parse("@remote").calc_required_presence().types,
	             "only a coordinate can be remote");

	auto& locations = test_locations();

	const auto is_remote = [&locations](const gps_coordinate coord, const str::cached stored_place)
	{
		df::index_file_item file;
		file.ft = files::file_type_from_name("test.jpg");
		const auto md = file.safe_ps();
		md->coordinate = coord;
		md->location_place = stored_place;
		file.calc_search_presence();

		const auto search = df::search_t::parse("@remote");
		const df::search_matcher matcher(search, platform::now().to_days(), &locations);
		return matcher.match_item({}, file).is_match();
	};

	assert_equal(true, is_remote({23.0, 15.0}, {}), "the Sahara is remote");
	assert_equal(false, is_remote({51.5142, -0.0985}, {}), "central London is not remote");
	assert_equal(false, is_remote({-25.153, 131.75}, {}), "Near a place is not remote");
	assert_equal(false, is_remote({23.0, 15.0}, "Camp"_c), "a stored place name is the user's own answer");
	assert_equal(false, is_remote({}, {}), "an item with no coordinate is not remote");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #203 - Search broken with Russian letter "Х" (U+0425)
// Cyrillic characters must be properly case-folded for case-insensitive comparison.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_search_cyrillic_text()
{
	// Simulate the actual bug: searching for metadata containing Cyrillic Х (U+0425)
	// "Фото с буквой Х в описании" using \u escapes
	constexpr auto description_with_x =
		"\u0424\u043e\u0442\u043e \u0441 \u0431\u0443\u043a\u0432\u043e\u0439 \u0425 \u0432 \u043e\u043f\u0438\u0441\u0430\u043d\u0438\u0438";

	// The search should find text containing Cyrillic Х
	const auto search = df::search_t::parse("\u0425");
	const df::search_matcher matcher(search);

	df::index_file_item info;
	info.ft = files::file_type_from_name("test.jpg");
	info.safe_ps()->description = str::cache(description_with_x);

	assert_equal(true, matcher.match_item({}, info).is_match(), "Cyrillic X search");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #177 - Excluding terms from search excludes everything
// Negated search terms (e.g., "-term" or "!term") should exclude only matching items.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_negate_search_terms()
{
	// Item with tag "aaa" should NOT match "-#aaa" (negated tag search)
	prop_test().tag("aaa")
	           .is_not_match("-#aaa")
	           .is_not_match("!#aaa");

	// Item with tag "aaa" SHOULD match "-#bbb" (negated search for a different tag)
	prop_test().tag("aaa")
	           .is_match("-#bbb")
	           .is_match("!#bbb");
}

static void should_negate_text_search()
{
	// Item with description "hello world" should NOT match when "hello" is negated
	prop_test().desc("hello world")
	           .is_not_match("-hello");

	// Item with description "hello world" SHOULD match when "goodbye" is negated
	prop_test().desc("hello world")
	           .is_match("-goodbye");
}

static void should_combine_positive_and_negative_terms()
{
	// Positive + negative: find items with "aaa" but NOT "bbb"
	prop_test().tag("aaa bbb")
	           .is_not_match("#aaa !#bbb");

	prop_test().tag("aaa")
	           .is_match("#aaa !#bbb");

	prop_test().tag("bbb")
	           .is_not_match("#aaa !#bbb");
}

static void should_negate_rating_search()
{
	// Item with rating 5 should NOT match "-rate:5"
	prop_test().rate(5)
	           .is_not_match("-rate:5");

	// Item with rating 3 SHOULD match "-rate:5" (it's not rating 5)
	prop_test().rate(3)
	           .is_match("-rate:5");
}

static void should_evaluate_folder_wildcards_inside_boolean_search()
{
	const auto matches = [](const std::string_view query, const std::string_view folder, const std::string_view tag)
	{
		df::index_file_item file;
		file.name = "photo.jpg"_c;
		file.ft = files::file_type_from_name("photo.jpg");
		file.safe_ps()->tags = str::cache(tag);
		file.calc_search_presence();

		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search);
		return matcher.match_item(df::file_path(df::folder_path(folder), "photo.jpg"), file).is_match();
	};

	assert_equal(false, matches("-**Private**", "c:\\photos\\Private", {}),
	             "a negated folder wildcard excludes its folder");
	assert_equal(true, matches("-**Private**", "c:\\photos\\Public", {}),
	             "and leaves unrelated folders alone");
	assert_equal(false, matches("**Private** -#hidden", "c:\\photos\\Private", "hidden"),
	             "a positive folder wildcard does not bypass a later exclusion");
	assert_equal(true, matches("**Private** -#hidden", "c:\\photos\\Private", "shown"),
	             "but still matches when the exclusion does not");
	assert_equal(false, matches("(**Private** or **Public**) -#hidden", "c:\\photos\\Public", "hidden"),
	             "grouped or folder matches still respect exclusions");
	assert_equal(true, matches("(**Private** or **Public**) #shown", "c:\\photos\\Public", "shown"),
	             "grouped folder matches remain positive terms");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #174 - Folders starting with '.' can't be excluded with '-'
// Wildcard patterns like ".*" should match folder names starting with a dot.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_exclude_dot_folders_with_wildcard()
{
	df::index_roots roots;
	roots.exclude_wildcards.emplace(".*"_c); // pattern to match any dot-prefixed folder

	// Folder names starting with '.' should be excluded
	assert_equal(true, df::is_excluded(roots, df::folder_path("c:\\photos\\.git")),
	             ".git excluded");
	assert_equal(true, df::is_excluded(roots, df::folder_path("c:\\photos\\.thumbnails")),
	             ".thumbnails excluded");
	assert_equal(true, df::is_excluded(roots, df::folder_path("c:\\photos\\.hidden")),
	             ".hidden excluded");

	// Folder names NOT starting with '.' should NOT be excluded
	assert_equal(false, df::is_excluded(roots, df::folder_path("c:\\photos\\vacation")),
	             "vacation not excluded");
	assert_equal(false, df::is_excluded(roots, df::folder_path("c:\\photos\\2024")),
	             "2024 not excluded");
}

static void should_exclude_specific_folder_name()
{
	df::index_roots roots;
	roots.exclude_wildcards.emplace("Proxy"_c);

	assert_equal(true, df::is_excluded(roots, df::folder_path("c:\\photos\\Proxy")),
	             "Proxy excluded");
	assert_equal(true, df::is_excluded(roots, df::folder_path("c:\\videos\\Proxy")),
	             "Proxy in different parent excluded");
	assert_equal(false, df::is_excluded(roots, df::folder_path("c:\\photos\\originals")),
	             "originals not excluded");
}

// Reproduces the follow-up report on #174: excluding a dot-folder by NAME
// ("-.dtrash") must work the same as excluding it by full path
// ("-c:\path\.dtrash"). This exercises the real parse path used by the
// collection-folders edit box (parse_more_folders), not just is_excluded.
static void should_parse_exclude_dot_folder_by_name()
{
	// Exclude a dot-folder by bare name.
	df::index_roots by_name;
	parse_more_folders(by_name, "-.dtrash");

	assert_equal(0_z, by_name.excludes.size(), "no full-path excludes");
	assert_equal(1_z, by_name.exclude_wildcards.size(), "dot name stored as wildcard");
	assert_equal(".dtrash", *by_name.exclude_wildcards.begin(), "wildcard text preserved");

	assert_equal(true, df::is_excluded(by_name, df::folder_path("c:\\photos\\.dtrash")),
	             ".dtrash excluded by name");
	assert_equal(true, df::is_excluded(by_name, df::folder_path("d:\\other\\sub\\.dtrash")),
	             ".dtrash excluded by name in any parent");
	assert_equal(false, df::is_excluded(by_name, df::folder_path("c:\\photos\\dtrash")),
	             "non-dot folder not excluded");

	// Exclude the same dot-folder by full path - the case the user confirmed works.
	df::index_roots by_path;
	const auto dtrash = as_platform_path("c:\\photos\\.dtrash");
	parse_more_folders(by_path, "-" + dtrash);

	assert_equal(1_z, by_path.excludes.size(), "full path stored as exclude");
	assert_equal(true, df::is_excluded(by_path, df::folder_path(dtrash)),
	             ".dtrash excluded by full path");
}

// A dot-prefixed wildcard ("-.*") entered in the collection box should also
// round-trip through the parser into an exclude wildcard.
static void should_parse_exclude_dot_wildcard()
{
	df::index_roots roots;
	parse_more_folders(roots, "-.*");

	assert_equal(1_z, roots.exclude_wildcards.size(), "dot wildcard stored");
	assert_equal(".*", *roots.exclude_wildcards.begin(), "wildcard text preserved");

	assert_equal(true, df::is_excluded(roots, df::folder_path("c:\\photos\\.git")),
	             ".git excluded by .* wildcard");
	assert_equal(false, df::is_excluded(roots, df::folder_path("c:\\photos\\normal")),
	             "normal folder not excluded by .* wildcard");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #219 - Korean tags are not working
// Korean (Hangul) tags must round-trip through tag parsing and be searchable.
// Hangul has no letter case, so case-folding must leave it unchanged.
///////////////////////////////////////////////////////////////////////////////////////////////////

// Hangul samples (via \u escapes to avoid source-encoding issues):
//   family = \uAC00\uC871 (가족), travel = \uC5EC\uD589 (여행),
//   photo  = \uC0AC\uC9C4 (사진), Seoul  = \uC11C\uC6B8 (서울)

static void should_search_korean_tags()
{
	constexpr auto family = "\uAC00\uC871"; // 가족
	constexpr auto travel = "\uC5EC\uD589"; // 여행
	constexpr auto photo = "\uC0AC\uC9C4"; // 사진

	// A file tagged with Korean words matches a #tag search for those words.
	prop_test().tag(std::format("{} {}", family, travel))
	           .is_match(std::format("#{}", family))
	           .is_match(std::format("#{}", travel))
	           .is_not_match(std::format("#{}", photo));

	// Bare (non-scoped) text search across metadata finds the Korean tag too.
	prop_test().tag(family)
	           .is_match(std::string(family));
}

static void should_search_korean_description()
{
	// Korean text embedded in a description is found by a substring search,
	// mirroring the Cyrillic #203 scenario for Hangul.
	// "서울에서 찍은 사진" (photo taken in Seoul)
	constexpr auto description = "\uC11C\uC6B8\uC5D0\uC11C \uCC0D\uC740 \uC0AC\uC9C4";

	const auto search = df::search_t::parse("\uC11C\uC6B8"); // 서울 (Seoul)
	const df::search_matcher matcher(search);

	df::index_file_item info;
	info.ft = files::file_type_from_name("test.jpg");
	info.safe_ps()->description = str::cache(description);

	assert_equal(true, matcher.match_item({}, info).is_match(), "Korean description search");
}

// Issue #219 - Korean tags stored in a different Unicode normalization form.
// Hangul can be encoded precomposed (NFC: 가 = U+AC00) or decomposed into
// conjoining jamo (NFD: U+1100 U+1161). macOS/Finder, some cameras and apps emit
// NFD; Windows IMEs emit NFC. The two are canonically equivalent and must compare
// and search as equal, otherwise a Korean tag "cannot be found / deleted".
static void should_match_korean_nfc_nfd()
{
	constexpr auto nfc = "\uAC00"; // 가 precomposed syllable
	constexpr auto nfd = "\u1100\u1161"; // 가 decomposed conjoining jamo

	// Canonical equivalence: normalising both forms to NFC yields identical bytes.
	// (Generic str::icmp intentionally stays byte-exact for path safety, so the
	// normalization happens in the search layer, not in icmp.)
	assert_equal(platform::normalize_nfc(nfc), platform::normalize_nfc(nfd), "NFC(nfc) == NFC(nfd)");

	// A tag stored in either form is found by a search in the other form.
	prop_test().tag(nfd).is_match(std::string(nfc));
	prop_test().tag(nfc).is_match(std::string(nfd));

	// Clicking a tag does not run a bare text search: it builds search_t().with(prop::tag, ...),
	// which reaches the query as the qualified `tag:` form. That is the path the report describes,
	// so it has to normalise too - a bare-text-only fix would look correct and still find nothing.
	prop_test().tag(nfd).is_match(std::format("tag:{}", nfc));
	prop_test().tag(nfc).is_match(std::format("tag:{}", nfd));

	// The same tag alongside others, since a tag list is split before each part is compared.
	constexpr auto travel = "\uC5EC\uD589"; // 여행
	prop_test().tag(std::format("{} {}", travel, nfd)).is_match(std::format("tag:{}", nfc));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #134 - Emojis breaking labels and rating
// The filename half of this issue is covered end-to-end by
// "Issue #134: Should update rating and label for emoji filename" in test_media_edit.cpp.
// What follows is the string half: emoji are non-BMP code points (surrogate pairs in UTF-16,
// 4-byte UTF-8) and must round-trip through tag parsing, case-folding and search like any
// other Unicode text, without corrupting neighbouring values.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_handle_emoji_tags()
{
	constexpr auto party = "\U0001F389"; // 🎉
	constexpr auto cat = "\U0001F431"; // 🐱

	// Space-separated emoji split into individual tags.
	tag_set tags(std::format("{} {}", party, cat));
	assert_equal(2, static_cast<int>(tags.size()), "emoji tag count");

	// Case-folding a non-BMP code point is the identity (no case).
	assert_equal_strict(party, str::to_lower(party), "emoji to_lower identity");
	assert_equal(0, str::icmp(party, party), "emoji icmp equal");
	assert_equal(true, str::icmp(party, cat) != 0, "different emoji differ");

	// Removing one emoji tag leaves the other intact.
	tags.remove(tag_set(cat));
	assert_equal(1, static_cast<int>(tags.size()), "emoji tag count after remove");
	assert_equal(true, tags.to_string().find(cat) == std::string::npos, "cat emoji removed");
	assert_equal(true, tags.to_string().find(party) != std::string::npos, "party emoji remains");

	// An emoji tag is searchable via a #tag query.
	prop_test().tag(party)
	           .is_match(std::format("#{}", party))
	           .is_not_match(std::format("#{}", cat));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #58 - Search "with"/"without" scope returns identical results
// "with:<property>" must match only items that HAVE the property and
// "without:<property>" must match only items that LACK it. Previously both
// scopes produced the same result set.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_distinguish_with_without_scope()
{
	// An item that HAS a genre matches with:genre and must NOT match without:genre.
	prop_test().genre("Rock")
	           .is_match("with:genre")
	           .is_not_match("without:genre");

	// An item that LACKS a genre matches without:genre and must NOT match with:genre.
	// (rating is set only so the item still carries metadata; genre stays empty.)
	prop_test().rate(3)
	           .is_match("without:genre")
	           .is_not_match("with:genre");

	// The same complementary behaviour must hold for another property (rating).
	prop_test().rate(5)
	           .is_match("with:rating")
	           .is_not_match("without:rating");

	prop_test().genre("Jazz")
	           .is_match("without:rating")
	           .is_not_match("with:rating");
}

// The scope spells the polarity, but everything else the tokenizer read belongs to the term. A
// leading `-` inverts, and an `or` joins - both were being discarded because the modifier was
// rebuilt from the polarity alone rather than carried.
static void should_carry_modifiers_through_with_and_without()
{
	// `-with:` is the same question as `without:`, and `-without:` the same as `with:`.
	prop_test().rate(5)
	           .is_not_match("-with:rating")
	           .is_match("-without:rating");

	prop_test().genre("Jazz")
	           .is_match("-with:rating")
	           .is_not_match("-without:rating");

	// `or` must union the two, not intersect them: an item carrying only one of the pair matches.
	prop_test().genre("Jazz")
	           .is_match("with:genre or with:rating");

	prop_test().rate(5)
	           .is_match("with:genre or with:rating");

	// And the same for a location term, which takes its own arm.
	prop_test().rate(5)
	           .is_match("-with:location")
	           .is_not_match("-without:location");
}

// `with:` completes from prop::key_scopes(), so a name on that list is a promise that asking for it
// answers. These are the arms that were missing, and the names that had no stored value behind them.
static void should_answer_every_offered_with_scope()
{
	prop_test().orientation(ui::orientation::left_top)
	           .is_match("with:orientation")
	           .is_not_match("without:orientation");

	// top_left is the default and none means nothing was recorded, so neither is an orientation
	// anyone asked for - the test that answered `!= left_top` had these exactly backwards.
	prop_test().orientation(ui::orientation::top_left).rate(3)
	           .is_not_match("with:orientation")
	           .is_match("without:orientation");

	prop_test().orientation(ui::orientation::none).rate(3)
	           .is_not_match("with:orientation")
	           .is_match("without:orientation");

	prop_test().dimensions(4000, 3000)
	           .is_match("with:megapixels")
	           .is_not_match("without:megapixels");

	prop_test().rate(3)
	           .is_not_match("with:megapixels")
	           .is_match("without:megapixels");

	prop_test().audio_channels(2)
	           .is_match("with:audio.channels")
	           .is_not_match("without:audio.channels");

	const auto scopes = prop::key_scopes();
	const auto offered = [&scopes](const std::string_view name)
	{
		return std::ranges::any_of(scopes, [name](const prop::prop_scope& s) { return s.scope == name; });
	};

	assert_equal(true, offered("orientation"), "an answerable scope stays on the list");
	assert_equal(true, offered("megapixels"), "megapixels answers and stays on the list");
	assert_equal(true, offered("audio.channels"), "audio.channels answers and stays on the list");

	// Each of these is a registered name with nothing stored behind it, so `with:` would answer no
	// for every item and `without:` would answer the whole collection.
	assert_equal(false, offered("null"), "null is not a scope");
	assert_equal(false, offered("encoding.tool"), "encoding.tool is never stored");
	assert_equal(false, offered("media.category"), "media.category is never stored");
	assert_equal(false, offered("streams"), "streams is never stored");
	assert_equal(false, offered("id"), "id is never stored");
}

// format_term writes the media type's display name, which is translated, and the vocabulary offers
// the plural. Anything the address box can show has to be something the parser reads back, because
// a term that fails to re-parse is dropped silently - and the search is persisted as text.
//
// The plural is what makes this discriminating: the suite runs in English, where a display name is
// its own untranslated name, so only the plural exercises the same fallback a translated catalog
// would take.
static void should_read_back_a_media_type_term()
{
	const auto* const photo = &file_group::photo;

	assert_equal(true, parse_file_group("photo") == photo, "the canonical name parses");
	assert_equal(true, parse_file_group("photos") == photo, "the plural the vocabulary offers parses");
	assert_equal(true, parse_file_group(photo->display_name(false)) == photo,
	             "the display name format_term writes parses");
	assert_equal(true, parse_file_group(photo->display_name(true)) == photo,
	             "the plural display name parses");
	assert_equal(true, parse_file_group("PHOTOS") == photo, "the match does not depend on case");
	assert_equal(true, parse_file_group("") == nullptr, "an empty term names no group");
	assert_equal(true, parse_file_group("not-a-group") == nullptr, "an unknown term names no group");

	// The whole round trip: a media type set by a sidebar click, written out as the address box
	// text that is persisted, and read back on the next launch.
	df::search_t search;
	search.add_media_type(photo);
	const auto text = search.text();

	assert_equal(true, df::search_t::parse(text).has_media_type(),
	             std::format("a formatted media type survives a re-parse ('{}')", text));

	prop_test().is_match(text);
	prop_test().media_type("test.mp4").is_not_match(text);
	prop_test().is_match("@photos");
	prop_test().media_type("test.mp4").is_not_match("@photos");
}

// Every part of the query has to be found: the result test counts one hit per part, so a part that
// matched must move on to the next part rather than ending the walk. And the highlights it hands
// back have to be in text order, because both renderers walk the text forwards and drop anything
// behind the position they have reached.
static void should_complete_on_every_word()
{
	ui::match_highlights m;

	const std::vector<std::string_view> two = {"my", "pictures"};
	assert_equal(true, find_auto_complete(two, "c:\\users\\zac\\my pictures", true, m),
	             "both words present is a match");
	assert_equal(static_cast<size_t>(2), m.size(), "one highlight per query part");

	// Typed in the other order, which is what the user does when they remember the distinctive word
	// first. The highlights must still come back ascending.
	const std::vector<std::string_view> reversed = {"pictures", "my"};
	assert_equal(true, find_auto_complete(reversed, "c:\\users\\zac\\my pictures", true, m),
	             "word order does not decide the match");
	assert_equal(static_cast<size_t>(2), m.size(), "still one highlight per query part");
	assert_equal(true, m[0].offset < m[1].offset, "the highlights are in text order");

	const std::vector<std::string_view> missing = {"my", "videos"};
	assert_equal(false, find_auto_complete(missing, "c:\\users\\zac\\my pictures", true, m),
	             "one word absent is not a match");

	const std::vector<std::string_view> one = {"pictures"};
	assert_equal(true, find_auto_complete(one, "c:\\users\\zac\\my pictures", true, m),
	             "a single word still matches");
}

static void should_highlight_the_actual_auto_complete_match_text()
{
	ui::match_highlights m;

	const std::vector<std::string_view> accented = {"cafe"};
	assert_equal(true, find_auto_complete(accented, "caf\xc3\xa9" "X", false, m),
	             "normalized query matches accented text");
	assert_equal(1_z, m.size(), "one accented highlight");
	assert_equal(0u, m[0].offset, "accented match starts at the first byte");
	assert_equal(5u, m[0].length, "accented highlight consumes the matched UTF-8 text, not the query bytes");

	const std::vector<std::string_view> decomposed = {"cafe"};
	assert_equal(true, find_auto_complete(decomposed, "cafe\xcc\x81" "X", false, m),
	             "normalized query matches decomposed text");
	assert_equal(4u, m[0].length, "the complete matched base text is highlighted without the suffix");

	const std::vector<std::string_view> emoji = {"\xf0\x9f\x98\x80"};
	assert_equal(true, find_auto_complete(emoji, "A\xf0\x9f\x98\x80" "B", false, m),
	             "supplementary characters match");
	assert_equal(1u, m[0].offset, "emoji match begins after the ASCII prefix");
	assert_equal(4u, m[0].length, "emoji highlight remains a UTF-8 byte span for the renderer to translate");
}

static void should_measure_search_highlights_as_text_bytes()
{
	const auto cyrillic = str::ifind2("привет", "ве", 0);
	assert_equal(true, cyrillic.found, "cyrillic substring matches");
	assert_equal(1_z, cyrillic.parts.size(), "one cyrillic highlight");
	assert_equal(6u, cyrillic.parts[0].offset, "cyrillic byte offset");
	assert_equal(4u, cyrillic.parts[0].length, "cyrillic highlight covers both two-byte letters");

	assert_equal(8u, str::matched_text_byte_length("M\xc3\xbcnchen", 0, "m\xc3\xbcnchen"),
	             "folded umlaut prefix covers the whole UTF-8 folder name");
	assert_equal(7u, str::matched_text_byte_length("Z\xc3\xbcrich", 0, "zurich"),
	             "folded ASCII query covers the whole UTF-8 location text");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Prediction and completion
///////////////////////////////////////////////////////////////////////////////////////////////////

// Mirrors the vocabulary substring-prediction path in auto_complete_words: for single-token
// queries, trigram candidates verified with str::ifind2 must reproduce a full ifind2 scan of
// the vocabulary exactly (the correctness guarantee behind the per-keystroke acceleration).
static void should_accelerate_substring_prediction()
{
	const std::vector<std::string_view> words = {
		"amsterdam", "index", "indigo", "industry", "reindex", "window", "windmill", "grind", "din"
	};

	df::trigram_index tri;
	for (uint32_t i = 0; i < words.size(); ++i) tri.add(i, words[i]);
	tri.freeze();

	const auto scan = [&words](const std::string_view q)
	{
		std::vector<uint32_t> out;
		for (uint32_t i = 0; i < words.size(); ++i)
			if (str::ifind2(words[i], q, 0).found) out.push_back(i);
		return out;
	};

	for (const auto* const q : {"ind", "win", "dex", "ndu", "zzz"})
	{
		const auto cand = tri.candidates(q);
		assert_equal(true, cand.has_value(), std::format("'{}' has trigram candidates", q));

		std::vector<uint32_t> via_index;
		for (const auto id : *cand)
			if (str::ifind2(words[id], q, 0).found) via_index.push_back(id);
		std::ranges::sort(via_index);

		assert_equal(true, via_index == scan(q), std::format("'{}' trigram+verify == full scan", q));
	}
}

static void should_predict_search_completions(shared_test_context& stc)
{
	stc.lazy_load_index();
	stc.test_index.update_summary();
	auto& index = stc.test_index;

	std::vector<index_state::auto_complete_word> words;
	std::vector<index_state::auto_complete_word> companions;
	std::vector<index_state::auto_complete_word> filtered;
	std::thread prediction_worker([&]
	{
		words = index.auto_complete_words("#key", 10);
		companions = index.auto_complete_tag_companions({"key1"}, {}, 10);
		filtered = index.auto_complete_tag_companions({"key1"}, "key3", 10);
	});
	prediction_worker.join();

	const auto key1 = std::ranges::find(words, "#key1", &index_state::auto_complete_word::text);
	assert_equal(true, key1 != words.end(), "indexed tag is suggested");
	assert_equal(true, key1 != words.end() && key1->occurrences > 0, "tag occurrence count is returned");

	assert_equal(true,
	             std::ranges::find(companions, "#key2", &index_state::auto_complete_word::text) != companions.end(),
	             "co-occurring tag is suggested");
	assert_equal(true,
	             std::ranges::find(companions, "#key1", &index_state::auto_complete_word::text) == companions.end(),
	             "existing tag is not suggested again");

	assert_equal(1_z, filtered.size(), "companion prefix filters suggestions");
	assert_equal("#key3"s, filtered.front().text, "matching companion is retained");
	assert_equal(true, filtered.front().occurrences > 0, "companion co-occurrence count is returned");
}

// design.md: Parent "drops one narrowing at a time - date, then media type, then the remaining
// terms, then a folder level - so the step is the same size whether or not a folder is in the
// query". The order is the whole point: a Parent that dropped the folder first would jump the user
// out of the folder they are standing in while their date and type narrowings survived.
static void should_broaden_one_narrowing_at_a_time()
{
	const auto folder = std::string(test_files_folder.text());
	auto search = df::search_t::parse(folder + " @photo beach 2019-06-15"s);

	assert_equal(true, search.has_date(), "the query starts with a date");
	assert_equal(true, search.has_media_type(), "the query starts with a media type");
	assert_equal(true, search.has_terms(), "the query starts with terms");
	assert_equal(true, search.has_selector(), "the query starts with a folder");

	// 1. The date goes first, and it widens by one level rather than vanishing.
	auto parent = find_parent_search(search).parent;
	assert_equal(true, parent.has_media_type(), "broadening a date keeps the media type");
	assert_equal(true, parent.has_terms(), "broadening a date keeps the terms");
	assert_equal(true, parent.has_selector(), "broadening a date keeps the folder");

	// Walk out the remaining date levels; each step must still be a date step.
	auto guard = 0;
	while (parent.has_date() && ++guard < 8)
	{
		const auto next = find_parent_search(parent).parent;
		assert_equal(true, next.has_selector(), "widening a date never leaves the folder");
		parent = next;
	}

	assert_equal(false, parent.has_date(), "the date is fully broadened away");

	// 2. Then the media type.
	parent = find_parent_search(parent).parent;
	assert_equal(false, parent.has_media_type(), "the media type goes after the date");
	assert_equal(true, parent.has_terms(), "dropping the media type keeps the terms");
	assert_equal(true, parent.has_selector(), "dropping the media type keeps the folder");

	// 3. Then the remaining terms.
	parent = find_parent_search(parent).parent;
	assert_equal(false, parent.has_terms(), "the terms go after the media type");
	assert_equal(true, parent.has_selector(), "dropping the terms keeps the folder");

	// 4. Only then a folder level.
	const auto folder_parent = find_parent_search(parent);
	assert_equal(true, folder_parent.parent.has_selector(), "the last step is one folder level");
	assert_equal(test_files_folder.name(), folder_parent.name, "the departed folder is named");
	assert_equal(true, folder_parent.parent.selectors().front().folder() == test_files_folder.parent(),
	             "the folder step goes up exactly one level");

	// design.md: "A scope with nothing wider to show, such as a drive root, reports no parent".
	auto root = df::search_t().add_selector(df::item_selector(df::folder_path("c:\\")));
	assert_equal(true, find_parent_search(root).parent.is_empty(), "a drive root has no parent");
}

void register_search_tests(view_state& state, test_registry& tests)
{
	//
	// Search parsing
	//
	tests.add("Should parse search"s, should_parse_search);
	tests.add("Should parse search input"s, should_parse_search_input);
	tests.add("Should parse searches"s, should_parse_searches);
	tests.add("Should answer a date group header with its own items"s,
	          should_answer_a_date_group_header_with_its_own_items);
	tests.add("Should bound a recursive selector at a folder boundary"s,
	          should_bound_a_recursive_selector_at_a_folder_boundary);
	tests.add("Should calculate safe presence requirements"s, should_calculate_safe_presence_requirements);
	tests.add("Should update duplicate search presence"s, should_update_duplicate_search_presence);
	tests.add("Should next date search"s, should_next_date_search);
	tests.add("Should parse selector"s, should_parse_selector);
	tests.add("Should detect folder browse"s, should_detect_folder_browse);
	tests.add("Should select files"s, should_select_files);
	// APP-004 - recursive toggles preserve the copied query
	tests.add("Should preserve query when toggling recursive selectors"s,
	          should_preserve_query_when_toggling_recursive_selectors);
	tests.add("Should parent"s, should_parent);
	tests.add("Should broaden one narrowing at a time"s, should_broaden_one_narrowing_at_a_time);
	tests.add("Should escape"s, should_escape);
	tests.add("Should select remote items"s, should_select_remote_items);

	//
	// Search matching
	//
	tests.add("Should match terms"s, should_match_terms);
	tests.add("Should match unbalanced groups"s, should_match_unbalanced_groups);
	tests.add("Should ignore unknown property scopes"s, should_ignore_unknown_property_scopes);
	tests.add("Should match parsed property values"s, should_match_parsed_property_values);
	tests.add("Should search Boolean presence terms"s, should_search_boolean_presence_terms);
	tests.add("Should match related"s, should_match_related);
	tests.add("Should not treat anchored perceptual group as pairwise duplicate"s,
	          should_not_treat_anchored_perceptual_group_as_pairwise_duplicate);
	tests.add("Should use only exact groups for related duplicate shortcut"s,
	          should_use_only_exact_groups_for_related_duplicate_shortcut);
	tests.add("Should clear exact duplicate shortcut outside collection"s,
	          should_clear_exact_duplicate_shortcut_outside_collection);
	tests.add("Should list an item once"s, should_list_an_item_once);
	tests.add("Should match related from text"s, should_match_related_from_text);
	tests.add("Should bound related results by closeness"s, should_bound_related_results_by_closeness);
	tests.add("Should break related ties on path"s, should_break_related_ties_on_path);
	tests.add("Should group related results by axis"s, should_group_related_results_by_axis);
	tests.add("Should match related by album"s, should_match_related_by_album);
	tests.add("Should match related by series"s, should_match_related_by_series);
	tests.add("Should match related by capture time"s, should_match_related_by_capture_time);
	tests.add("Should match related by capture place"s, should_match_related_by_capture_place);
	tests.add("Should report the strongest related axis"s, should_report_the_strongest_related_axis);
	tests.add("Should match volume label"s, should_match_volume_label);
	tests.add("Should match multi-value genre"s, should_match_multi_value_genre);
	// Issue #139/#178 - quoted search terms
	tests.add("Should match quoted terms"s, should_match_quoted_terms);
	// Issue #139/#178 - quoted search terms
	tests.add("Should preserve and auto-quote search input"s,
	          should_preserve_and_auto_quote_search_input);
	// Issue #157 - search scope classification
	tests.add("Should classify search scope"s, should_classify_search_scope);
	tests.add("Should format search predictions"s, should_format_search_predictions);
	tests.add("Should not match folder against without:tag"s, should_not_match_folder_without);
	tests.add("Should group search terms"s, should_group_search_terms);
	tests.add("Should read a query back as it was asked"s, should_read_a_query_back_as_it_was_asked);
	tests.add("Should not call a cancelled query complete"s, should_not_call_a_cancelled_query_complete);
	tests.add("Should parse file size terms"s, should_parse_file_size_terms);

	// Issue #203 - Cyrillic character search
	tests.add("Should search Cyrillic text"s, should_search_cyrillic_text);

	// Issue #177 - search term negation
	tests.add("Should negate search terms"s, should_negate_search_terms);
	tests.add("Should negate text search"s, should_negate_text_search);
	tests.add("Should combine positive and negative terms"s, should_combine_positive_and_negative_terms);
	tests.add("Should negate rating search"s, should_negate_rating_search);
	// MOD-012 - folder wildcard terms must stay inside the Boolean evaluator
	tests.add("Should evaluate folder wildcards inside Boolean search"s,
	          should_evaluate_folder_wildcards_inside_boolean_search);

	// Issue #174 - exclude dot folders
	tests.add("Should exclude dot folders with wildcard"s, should_exclude_dot_folders_with_wildcard);
	tests.add("Should exclude specific folder name"s, should_exclude_specific_folder_name);
	tests.add("Should parse -.dtrash exclude by name"s, should_parse_exclude_dot_folder_by_name);
	tests.add("Should parse -.* exclude wildcard"s, should_parse_exclude_dot_wildcard);

	// Issue #219 - Korean tags
	tests.add("Should search Korean tags"s, should_search_korean_tags);
	tests.add("Should search Korean description"s, should_search_korean_description);
	tests.add("Should match Korean NFC and NFD"s, should_match_korean_nfc_nfd);

	// Issue #134 - emoji tags/labels
	tests.add("Should handle emoji tags"s, should_handle_emoji_tags);

	// Issue #58 - search with/without scope
	tests.add("Should distinguish with/without scope"s, should_distinguish_with_without_scope);
	tests.add("Should carry modifiers through with and without"s, should_carry_modifiers_through_with_and_without);
	tests.add("Should answer every offered with scope"s, should_answer_every_offered_with_scope);
	tests.add("Should read back a media type term"s, should_read_back_a_media_type_term);
	tests.add("Should complete on every word"s, should_complete_on_every_word);
	// UI-006 - auto-complete highlights must describe the matched text, not the query byte count.
	tests.add("Should highlight the actual auto complete match text"s,
	          should_highlight_the_actual_auto_complete_match_text);
	// UI-006 - every completion producer reports highlight lengths in UTF-8 bytes.
	tests.add("Should measure search highlights as text bytes"s, should_measure_search_highlights_as_text_bytes);

	// One row per query/date pair. The name carries both so a failure names the case and
	// /test: can select a single row.
	const auto register_match_date = [&tests](const std::string& query, const df::date_t d)
	{
		tests.add(std::format("Should match date {} on {}", query, d.to_xmp_date()),
		          [query, d] { should_match_date(query, d); });
	};

	register_match_date("2012-09-14", df::date_t(2012, 9, 14));
	register_match_date("2012", df::date_t(2012, 1, 14));
	register_match_date("2012|2013", df::date_t(2012, 1, 14));
	register_match_date("(April or June) (2013 or 2015)", df::date_t(2013, 4, 22));
	register_match_date("(April or June) (2013 or 2015)", df::date_t(2015, 6, 7));
	register_match_date("age:4", df::date_t(1999, 12, 30));

	// A bare month is a month whether or not a date scope names which date it applies to.
	// Scoped, it used to parse as a YEAR, so every one of these returned nothing.
	register_match_date("august", df::date_t(2019, 8, 3));
	register_match_date("original:august", df::date_t(2019, 8, 3));
	register_match_date("taken:august", df::date_t(2019, 8, 3));

	//
	// Search format round-trip
	//
	const auto alfie = "Alfie"_c;
	const auto jana = "Jana"_c;
	const auto app_data_folder = known_path(platform::known_folder::app_data);

	auto register_assert_format = [&tests](const df::search_t& query)
	{
		tests.add(std::format("Should format {}", query.text()), [query](shared_test_context& stc)
		{
			const auto text = query.format_terms();
			const auto parsed = df::search_t::parse(text).format_terms();
			assert_equal(text, parsed, "assert_parse");
		});
	};

	register_assert_format(df::search_t().with(prop::duration));
	register_assert_format(df::search_t().add_selector(app_data_folder));
	register_assert_format(df::search_t().add_selector(app_data_folder).with("*.jpg"));
	register_assert_format(df::search_t().without(prop::tag));
	register_assert_format(df::search_t().without(prop::location_place).with(prop::latitude));
	register_assert_format(df::search_t().year(2000));
	register_assert_format(df::search_t().year(2000).month(5));
	register_assert_format(df::search_t().day(25, 5, 1972));
	register_assert_format(df::search_t().day(25, 5, 0));
	register_assert_format(df::search_t().day(0, 5, 2005));
	register_assert_format(df::search_t().with(prop::tag, alfie));
	register_assert_format(df::search_t().with(prop::duration, 500));
	register_assert_format(df::search_t().with("!-"));
	register_assert_format(df::search_t().without(prop::tag, alfie));
	register_assert_format(df::search_t().with(prop::tag, alfie).with(prop::tag, jana));
	register_assert_format(df::search_t().with(prop::tag, alfie).without(prop::tag, jana));
	register_assert_format(df::search_t().with(prop::tag, "tag with space").without(prop::tag, "space tag"));
	register_assert_format(df::search_t().with(alfie).with(jana));
	register_assert_format(df::search_t().age(7, df::date_parts_prop::any));
	register_assert_format(df::search_t().age(7, df::date_parts_prop::modified));
	register_assert_format(df::search_t().age(7, df::date_parts_prop::created));
	register_assert_format(df::search_t().age(7, df::date_parts_prop::original));
	register_assert_format(df::search_t().day(0, 5, 2019, df::date_parts_prop::original));
	register_assert_format(df::search_t().month(5, df::date_parts_prop::original));
	register_assert_format(df::search_t().month(5, df::date_parts_prop::created));
	register_assert_format(df::search_t().month(5, df::date_parts_prop::modified));
	register_assert_format(df::search_t().fuzzy(prop::duration, 33));
	register_assert_format(df::search_t().location(gps_coordinate(-30.515, 151.665), 5.0));
	// A west longitude carries its sign after the comma, where a leading '-' used to read as a
	// negation and split the component off as a term of its own.
	register_assert_format(df::search_t().location(gps_coordinate(51.5142, -0.0985), 2.0));
	register_assert_format(df::search_t().with_extension("jpg"));

	auto register_assert_parse = [&tests](const std::string& query, const std::string& expected = {})
	{
		tests.add(std::format("Should parse {}", query), [query, expected](shared_test_context& stc)
		{
			const auto formatted = df::search_t::parse(query).format_terms();
			assert_equal(str::is_empty(expected) ? query : expected, formatted, "parse query");
			// The canonical form must survive a second trip, or the address box would rewrite
			// itself every time the user pressed Enter on an unchanged query.
			const auto reparsed = df::search_t::parse(formatted).format_terms();
			assert_equal(formatted, reparsed, "parse formatted");
		});
	};

	register_assert_parse("2014 2015"s);
	register_assert_parse("2014 @photo"s);
	register_assert_parse("2014 -@video"s);
	register_assert_parse("2014 @audio"s);
	register_assert_parse("2014 or 2015"s);
	register_assert_parse("Jana Alfie"s);
	register_assert_parse("Jana or Alfie"s);
	register_assert_parse("Jana -Alfie"s);
	register_assert_parse("genre: Rock -artist: \"Counting Crows\""s);
	register_assert_parse("Created:7"s);
	register_assert_parse("Modified:7"s);
	register_assert_parse("(2014 or 2015) (May or June)"s);
	register_assert_parse("Bertie or ((Jana or Alfie) or Amalka)"s);
	register_assert_parse("(2013 or 2014 or 2015) (May or June or July)"s);
	register_assert_parse("Rating:5"s, "rating: 5"s);
	register_assert_parse("dec-25"s);
	register_assert_parse("2020-aug-16"s);
	register_assert_parse("2020-aug"s);
	register_assert_parse("modified:2020-aug"s);
	register_assert_parse("modified:2020-aug-16"s);
	register_assert_parse("modified:aug-16"s);
	register_assert_parse("@duplicates"s);
	register_assert_parse("@remote"s);
	register_assert_parse("@panorama"s);
	// The short spelling is the same term, so it round-trips to the long one.
	register_assert_parse("@pano"s, "@panorama"s);
	register_assert_parse("loc:-30.515+151.66+5"s, "loc:-30.515,151.66,5km"s);
	register_assert_parse("loc:\"Boulder City\""s);
	register_assert_parse("place:London"s);
	register_assert_parse("state:Nevada"s);
	register_assert_parse("country:\"United Kingdom\""s);
	register_assert_parse("city:London"s, "place:London"s);
	register_assert_parse("countries:France"s, "country:France"s);
	register_assert_parse("near:London"s, "loc:London"s);
	register_assert_parse("loc:\"London, 10km\""s, "loc:London, 10km"s);
	register_assert_parse("loc: 'London UK 5km'"s, "loc:\"London UK, 5km\""s);
	register_assert_parse("loc: London UK 5km"s, "loc:London, UK, 5km"s);
	register_assert_parse("loc: London, UK, 5km"s, "loc:London, UK, 5km"s);
	register_assert_parse("loc: London UK tag:travel"s, "loc:London, UK #travel"s);
	register_assert_parse("loc: London, Ontario, 5km"s, "loc:London, Ontario, 5km"s);
	register_assert_parse("loc: London sunset"s, "loc:London sunset"s);
	register_assert_parse("place:\"London, 2.5km\""s, "place:London, 2.5km"s);
	register_assert_parse("loc:\"London, 500m\""s, "loc:London, 500m"s);
	register_assert_parse("with:location"s, "with: location"s);
	register_assert_parse("without:location"s, "without: location"s);
	register_assert_parse("area:Brisbane"s);
	register_assert_parse("size:1mb"s, "size: 1 MB"s);
	register_assert_parse("> size:1mb"s, "> size: 1 MB"s);
	register_assert_parse("ext:jpg"s);
	register_assert_parse("volume:Backup"s);
	register_assert_parse("with:tag"s, "with: tag"s);
	register_assert_parse("without:tag"s, "without: tag"s);
	register_assert_parse("c:\\windows"s);
	register_assert_parse("c:\\windows with:tag"s, "c:\\windows with: tag"s);
	register_assert_parse("c:\\windows without:tag"s, "c:\\windows without: tag"s);
	register_assert_parse("@ photo"s, "@photo"s);
	register_assert_parse("# key1"s, "#key1"s);
	register_assert_parse("# \"tag with space\""s, "#\"tag with space\""s);
	register_assert_parse("#'tag with space'"s, "#\"tag with space\""s);

	//
	// Search execution
	//
	auto register_should_search = [&tests](const std::string_view query, int expected_index, int expected_recurse,
	                                       int expected_folder)
	{
		tests.add(std::format("Should search {}", query),
		          [query, expected_index, expected_recurse, expected_folder](shared_test_context& stc)
		          {
			          stc.lazy_load_index();
			          assert_equal(expected_index, count_search_results(stc.test_index, query), query);

			          const auto query_recurse_test_folder = std::format(
				          "\"{}\\**\" {} -excluded", test_files_folder, query);
			          assert_equal(expected_recurse, count_search_results(stc.empty_index, query_recurse_test_folder),
			                       std::format("recurse {}", query));

			          const auto query_test_folder = std::format("\"{}\" {}", test_files_folder, query);
			          assert_equal(expected_folder, count_search_results(stc.empty_index, query_test_folder),
			                       std::format("folder {}", query));
		          });
	};

	register_should_search("2012-09-14", 5, 5, 5);
	register_should_search("Created:2012-09-14", 5, 5, 5);
	register_should_search("2010-05-25", 0, 0, 0);
	register_should_search("2010-5-25", 0, 0, 0);
	register_should_search("Created:2010-05-25", 0, 0, 0);
	register_should_search("2009-11-15", 1, 1, 1);

	register_should_search("@video", 14, 14, 14);
	register_should_search("@audio", 6, 6, 5);
	register_should_search("@commodore", 1, 1, 0);
	register_should_search("@archive", 1, 1, 1);

	register_should_search("@photo", 29, 29, 28);
	register_should_search("@ photo", 29, 29, 28);
	register_should_search("@   photo", 29, 29, 28);

	register_should_search("key1", 4, 4, 4);
	register_should_search("Tag:key1", 4, 4, 4);
	register_should_search("Tag :key1", 4, 4, 4);
	register_should_search("Tag: key1", 4, 4, 4);
	register_should_search("#key1", 4, 4, 4);
	register_should_search("# key1", 4, 4, 4);
	register_should_search("Tag:dog Tag:london", 1, 1, 1);
	register_should_search("not_exist", 0, 0, 0);
	register_should_search("Tag:not_exist", 0, 0, 0);
	register_should_search("#ke*", 4, 4, 4);
	register_should_search("ke*", 5, 5, 5);

	register_should_search("Megapixels:1.6 dog", 1, 1, 1);
	register_should_search("Megapixels:1.6", 2, 2, 2);
	register_should_search("(< Megapixels:1.0 > Megapixels:0.5)", 8, 8, 8);
	register_should_search("Megapixels:2", 1, 1, 1);
	register_should_search("pixels:2", 1, 1, 1);
	register_should_search("> pixels:1", 11, 11, 10);
	register_should_search(">pixels:1", 11, 11, 10);
	register_should_search(">pixels :1", 11, 11, 10);
	register_should_search(">pixels: 1", 11, 11, 10);
	register_should_search("> pixels : 1", 11, 11, 10);
	register_should_search("2mp", 1, 1, 1);
	register_should_search("6000x4000", 1, 1, 1);

	register_should_search("Aperture:f/5", 1, 1, 1);
	register_should_search("f/3.5", 6, 6, 6);
	register_should_search("f/1.8", 0, 0, 0);
	register_should_search("f/4.0", 0, 0, 0);
	register_should_search("f/6.3", 5, 5, 5);

	register_should_search("with:Exposure @photo", 17, 17, 16);
	register_should_search("with: Exposure @ photo", 17, 17, 16);
	register_should_search("without:Exposure @photo", 12, 12, 12);
	register_should_search("without:Exposure", 35, 35, 33);
	register_should_search("with:Exposure", 18, 18, 17);
	register_should_search("with: Exposure", 18, 18, 17);
	register_should_search("ExposureTime:1/20s", 1, 1, 1);
	register_should_search("ExposureTime: 1/20s", 1, 1, 1);
	register_should_search("1/20s", 1, 1, 1);
	register_should_search("1/100s", 5, 5, 5);
	register_should_search("1/1000s", 0, 0, 0);

	register_should_search("iso400 @photo", 2, 2, 2);
	register_should_search("iso:400 @photo", 2, 2, 2);
	register_should_search("iso: 400 @photo", 2, 2, 2);
	register_should_search("iso : 400 @photo", 2, 2, 2);
	register_should_search(">= iso:400 @photo", 3, 3, 3);

	register_should_search("1:26", 1, 1, 1);
	register_should_search("0:10", 6, 6, 6);
	register_should_search("7:77", 0, 0, 0);
	register_should_search("10:00", 0, 0, 0);

	register_should_search("size:0.3mb", 5, 5, 5);
	register_should_search("size:14kb", 1, 1, 1);
	register_should_search("size:5.1mb", 1, 1, 1);
	register_should_search(">size:1mb", 11, 11, 10);

	register_should_search("dog london", 1, 1, 1);
	// locations.md 3.6: free text searches the stored field. The GPS-only Prague photo carries no
	// stored place, so the location vocabulary is what finds it.
	register_should_search("prague", 4, 4, 4);
	register_should_search("loc:prague", 5, 5, 5);
	register_should_search("ipad", 1, 1, 1);
	register_should_search("48kHz", 5, 5, 4);
	register_should_search("44.1kHz", 5, 5, 5);
	register_should_search("dog or london", 4, 4, 4);
	register_should_search("Rock", 3, 3, 3);
	register_should_search("canon @photo", 10, 10, 9);
	register_should_search("nikon d100", 1, 1, 1);

	register_should_search("ext:cr2", 2, 2, 1);
	register_should_search("ext:.cr2", 2, 2, 1);

	register_should_search("sony", 1, 1, 1);
	register_should_search("\"sony\"", 1, 1, 1);
	register_should_search("sony*", 1, 1, 1);
	register_should_search("*sony*", 1, 1, 1);

	register_should_search("screws", 1, 1, 0);
	register_should_search("\"screws\"", 1, 1, 0);
	register_should_search("screws*", 1, 1, 0);
	register_should_search("*screws*", 1, 1, 0);

	register_should_search("london", 4, 4, 4);
	register_should_search("\"london\"", 4, 4, 4);
	register_should_search("london*", 4, 4, 4);
	register_should_search("*london*", 4, 4, 4);

	register_should_search("d64", 1, 1, 0);
	register_should_search("ace -retro", 1, 1, 1);
	register_should_search("jpg", 15, 15, 15);
	register_should_search("-jpg", 41, 40, 39);
	register_should_search("-ext:jpg", 41, 40, 39);

	//
	// Prediction and completion
	//
	tests.add("Should accelerate substring prediction"s, should_accelerate_substring_prediction);
	tests.add("Should predict search completions"s, should_predict_search_completions);
}
