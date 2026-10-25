// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implements the /validate-po command line option. Validates the
// translation .po files in the languages folder against the strings registered
// in app_text (see app_text.cpp), reporting entries that are needed but missing,
// present but no longer needed, untranslated, duplicated, or have a mismatched
// plural form. Invoked headless via /validate-po.

#include "pch.h"
#include "app_text.h"
#include "libdesktop/platform.h"

#include <cstdio>

namespace
{
	struct po_report
	{
		std::string file;
		std::vector<std::string> missing; // registered in app_text.cpp but absent from the .po file
		std::vector<std::string> not_needed; // present in the .po file but not registered in app_text.cpp
		std::vector<std::string> untranslated; // present but with an empty translation
		std::vector<std::string> plural_mismatch; // msgid_plural differs from the registered plural form
		std::vector<std::string> duplicates; // msgid appears more than once
		std::vector<std::string> parse_errors; // malformed or unsupported PO syntax

		// Structural problems make the .po file inconsistent with app_text.cpp.
		bool has_structural_issues() const
		{
			return !missing.empty() || !not_needed.empty() || !plural_mismatch.empty() || !duplicates.empty() ||
				!parse_errors.empty();
		}
	};

	po_report validate_one(const df::file_path lang_path, const app_text_t& texts)
	{
		po_report report;
		report.file = std::string(lang_path.name().sv());

		const auto loaded = load_po_report(lang_path);
		const auto& entries = loaded.entries;

		for (const auto& error : loaded.errors)
		{
			report.parse_errors.emplace_back(std::format("line {}: {}", error.line, error.message));
		}

		// Index the .po entries by msgid, detecting duplicates. The first entry
		// (with an empty msgid) is the header and is skipped.
		df::hash_map<std::string_view, const po_entry*> po_by_id;

		for (const auto& e : entries)
		{
			if (e.id.empty()) continue;

			const auto inserted = po_by_id.try_emplace(e.id, &e);

			if (!inserted.second)
			{
				report.duplicates.emplace_back(e.id);
			}
		}

		// Build the set of msgids that app_text.cpp needs, and check each one is
		// present (and translated) in the .po file.
		df::hash_set<std::string_view> needed;

		for (const auto& t : texts._all_texts)
		{
			const std::string_view id = t.get().text;
			if (id.empty()) continue;

			needed.insert(id);

			const auto found = po_by_id.find(id);

			if (found == po_by_id.end())
			{
				report.missing.emplace_back(id);
			}
			else if (found->second->str.empty())
			{
				report.untranslated.emplace_back(id);
			}
		}

		for (const auto& p : texts._all_plurals)
		{
			const std::string_view id = p.get().one.text;
			const std::string_view id_plural = p.get().plural.text;
			if (id.empty()) continue;

			needed.insert(id);

			const auto found = po_by_id.find(id);

			if (found == po_by_id.end())
			{
				report.missing.emplace_back(id);
			}
			else
			{
				const auto* e = found->second;

				if (e->id_plural != id_plural)
				{
					report.plural_mismatch.emplace_back(id);
				}

				if (e->str.empty() || e->str_plural.empty())
				{
					report.untranslated.emplace_back(id);
				}
			}
		}

		// Report any .po entries that app_text.cpp no longer needs.
		for (const auto& e : entries)
		{
			if (e.id.empty()) continue;
			if (!needed.contains(e.id))
			{
				report.not_needed.emplace_back(e.id);
			}
		}

		return report;
	}

	void print_list(const std::string_view label, const std::vector<std::string>& items)
	{
		if (items.empty()) return;

		printf("  %s: %zu\n", std::string(label).c_str(), items.size());

		for (const auto& s : items)
		{
			printf("    msgid \"%s\"\n", s.c_str());
		}
	}
}

int validate_po_files()
{
	const auto lang_folder = platform::known_path(platform::known_folder::running_app_folder).combine("languages");

	if (!platform::exists(lang_folder))
	{
		printf("ERROR: languages folder not found: %s\n", std::string(lang_folder.text().sv()).c_str());
		return 1;
	}

	const app_text_t texts;
	const auto contents = platform::iterate_file_items(lang_folder, false);

	printf("Validating translation files in %s\n\n", std::string(lang_folder.text().sv()).c_str());

	int files_checked = 0;
	int files_with_issues = 0;

	for (const auto& f : contents.files)
	{
		const auto lang_path = lang_folder.combine_file(f.name);
		if (str::icmp(lang_path.extension(), ".po") != 0) continue;

		files_checked += 1;

		const auto report = validate_one(lang_path, texts);

		printf("==== %s ====\n", report.file.c_str());

		print_list("Missing (needed by app_text.cpp but absent)", report.missing);
		print_list("Not needed (present but not in app_text.cpp)", report.not_needed);
		print_list("Duplicate msgid", report.duplicates);
		print_list("Parse errors", report.parse_errors);
		print_list("Plural mismatch (msgid_plural differs from app_text.cpp)", report.plural_mismatch);
		print_list("Untranslated (empty translation)", report.untranslated);

		if (report.has_structural_issues())
		{
			files_with_issues += 1;
		}
		else if (report.untranslated.empty())
		{
			printf("  OK\n");
		}

		printf("\n");
	}

	if (files_checked == 0)
	{
		printf("No .po files found.\n");
		return 1;
	}

	printf("Checked %d file(s); %d with structural issues.\n", files_checked, files_with_issues);

	return files_with_issues == 0 ? 0 : 1;
}
