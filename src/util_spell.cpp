// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Spell checking integration. Uses Hunspell library for spell checking,
// suggestions, and custom dictionary management.

#include "pch.h"
#include "util_spell.h"

#define HUNSPELL_STATIC
#include <hunspell.hxx>

spell_check& spell()
{
	static spell_check instance;
	return instance;
}

namespace
{
	class spell_check_dictionary final : public ui::spelling_dictionary
	{
	public:
		void queue_load() override { spell().queue_load(); }

		bool is_word_valid(const std::string_view word) const override { return spell().is_word_valid(word); }

		std::vector<std::string> suggest(const std::string_view word) const override
		{
			return spell().suggest(word);
		}

		void add_word(const std::string_view word, std::function<void()> done) override
		{
			spell().add_word(word, [done = std::move(done)](const custom_dictionary_add_result& result)
			{
				if (result.status == custom_dictionary_add_status::failed)
				{
					df::log(__FUNCTION__, result.message);
				}

				if (done) done();
			});
		}
	};
}

ui::spelling_dictionary& spell_dictionary()
{
	static spell_check_dictionary instance;
	return instance;
}

void spell_check::set_default_paths()
{
	_shipped_folder = known_path(platform::known_folder::running_app_folder).combine("dictionaries");
	_user_folder = known_path(platform::known_folder::app_data).combine("dictionaries");
	_custom_dic_path = _user_folder.combine_file("custom.dic");
}

spell_check::spell_check()
{
	set_default_paths();
}

// The per-user copy wins, so a downloaded dictionary supersedes a shipped one of the same name.
df::file_path spell_check::find_dictionary(const std::string_view name, const std::string_view ext) const
{
	const auto user_path = _user_folder.combine_file_ext(name, ext);
	return user_path.exists() ? user_path : _shipped_folder.combine_file_ext(name, ext);
}

// Created on demand rather than at construction, so a user who never adds a word or downloads a
// dictionary gets no empty folder.
bool spell_check::ensure_user_folder() const
{
	return _user_folder.exists() || platform::create_folder(_user_folder).success();
}

spell_check::~spell_check()
{
	platform::exclusive_lock lock(_rw);
	_hunspell.reset();
}

static void download_dic(df::async_i& async, const df::file_path path, spell_check& checker)
{
	// example https://diffractor.com/static/dictionaries/en_GB.aff

	const auto temp_path = platform::temp_file(path.extension());

	platform::web_request req;
	req.path = std::format("/static/dictionaries/{}", path.name());
	req.download_file_path = temp_path;

	async.queue_async(async_queue::web, [req, path, temp_path, &async, &checker]
	{
		const auto con = platform::connect_to_host("diffractor.com");
		const auto response = send_request(con, req);

		if (response.status_code == 200)
		{
			platform::move_file(temp_path, path, true);
			checker.dictionary_downloaded(async);
		}
	});
}

void spell_check::lazy_download(df::async_i& async)
{
	const std::unordered_set<std::string_view, df::ihash, df::ieq> known_dics =
	{
		"bg_BG",
		"ca_ES",
		"cs_CZ",
		"cy_GB",
		"da_DK",
		"de_DE",
		"el_GR",
		"en_AU",
		"en_CA",
		"en_GB",
		"en_US",
		"es_ES",
		"et_EE",
		"fa_IR",
		"fr_FR",
		"he_IL",
		"hi_IN",
		"hr_HR",
		"hu_HU",
		"id_ID",
		"it_IT",
		"ja_JP",
		"lt_LT",
		"lv_LV",
		"nb_NO",
		"nl_NL",
		"pl_PL",
		"pt_BR",
		"pt_PT",
		"ro_RO",
		"ru_RU",
		"sk_SK",
		"sl_SI",
		"sv_SE",
		"ta_IN",
		"tg_TJ",
		"uk_UA",
		"vi_VN",
	};

	const auto language = platform::user_language();

	if (known_dics.contains(language))
	{
		const auto aff = find_dictionary(language, ".aff");
		const auto dic = find_dictionary(language, ".dic");

		if ((!aff.exists() || !dic.exists()) && ensure_user_folder())
		{
			if (!aff.exists()) download_dic(async, _user_folder.combine_file_ext(language, ".aff"s), *this);
			if (!dic.exists()) download_dic(async, _user_folder.combine_file_ext(language, ".dic"s), *this);
		}
	}
}

namespace
{
	struct spell_paths
	{
		df::folder_path shipped_folder;
		df::folder_path user_folder;
		df::file_path custom_dic_path;
		std::function<void()> load_gate;
	};

	df::file_path find_dictionary(const spell_paths& paths, const std::string_view name, const std::string_view ext)
	{
		const auto user_path = paths.user_folder.combine_file_ext(name, ext);
		return user_path.exists() ? user_path : paths.shipped_folder.combine_file_ext(name, ext);
	}

	std::unique_ptr<Hunspell> load_dictionary_from_paths(const spell_paths& paths)
	{
		if (paths.load_gate) paths.load_gate();

		const auto language = platform::user_language();
		auto aff_path = find_dictionary(paths, language, ".aff");
		auto dic_path = find_dictionary(paths, language, ".dic");

		if (!aff_path.exists())
		{
			aff_path = find_dictionary(paths, "en_US", ".aff");
			dic_path = find_dictionary(paths, "en_US", ".dic");
		}

		if (!aff_path.exists() || !dic_path.exists()) return {};

		auto hunspell = std::make_unique<Hunspell>(str::utf8_to_a(aff_path.str()).c_str(),
		                                           str::utf8_to_a(dic_path.str()).c_str());

		std::ifstream f(platform::to_stream_path(paths.custom_dic_path));

		if (f.is_open())
		{
			std::string line;

			while (std::getline(f, line))
			{
				if (!line.empty())
				{
					hunspell->add(str::utf8_cast2(line));
				}
			}
		}

		return hunspell;
	}

	struct spell_load_result
	{
		std::unique_ptr<Hunspell> hunspell;
	};
}

void spell_check::lazy_load()
{
	uint64_t generation = 0;
	spell_paths paths;

	{
		platform::exclusive_lock lock(_rw);
		if (_hunspell || _loading) return;

		_loading = true;
		generation = ++_generation;
		paths = {_shipped_folder, _user_folder, _custom_dic_path, _load_gate};
	}

	std::unique_ptr<Hunspell> loaded;

	try
	{
		loaded = load_dictionary_from_paths(paths);
	}
	catch (const std::exception& e)
	{
		df::log(__FUNCTION__, std::format("failed to load dictionary: {}", e.what()));
	}

	platform::exclusive_lock lock(_rw);
	if (generation == _generation)
	{
		_hunspell = std::move(loaded);
		_loading = false;
	}
}

void spell_check::queue_load(df::async_i& async)
{
	uint64_t generation = 0;
	spell_paths paths;

	{
		platform::exclusive_lock lock(_rw);
		_async = &async;

		if (_hunspell || _loading) return;

		_loading = true;
		generation = ++_generation;
		paths = {_shipped_folder, _user_folder, _custom_dic_path, _load_gate};
	}

	async.queue_async(async_queue::work, [this, generation, paths, &async]
	{
		auto result = std::make_shared<spell_load_result>();

		try
		{
			result->hunspell = load_dictionary_from_paths(paths);
		}
		catch (const std::exception& e)
		{
			df::log(__FUNCTION__, std::format("failed to load dictionary: {}", e.what()));
		}

		async.queue_ui([this, generation, result]
		{
			platform::exclusive_lock lock(_rw);
			if (generation == _generation)
			{
				_hunspell = std::move(result->hunspell);
				_loading = false;
			}
		});
	});
}

void spell_check::queue_load()
{
	df::async_i* async = nullptr;

	{
		platform::exclusive_lock lock(_rw);
		async = _async;
	}

	if (async) queue_load(*async);
}

void spell_check::dictionary_downloaded(df::async_i& async)
{
	{
		platform::exclusive_lock lock(_rw);
		_hunspell.reset();
		_loading = false;
		++_generation;
	}

	queue_load(async);
}

void spell_check::configure_async(df::async_i* async)
{
	platform::exclusive_lock lock(_rw);
	_async = async;
	++_generation;
	_loading = false;
}

bool spell_check::is_word_valid(const std::string_view word) const
{
	// Add input validation
	if (word.empty())
		return true;

	platform::shared_lock lock(_rw);
	if (!_hunspell) return true;
	return _hunspell->spell(str::utf8_cast2(word));
}

std::vector<std::string> spell_check::suggest(const std::string_view word) const
{
	// Add input validation
	if (word.empty())
		return {};

	platform::shared_lock lock(_rw);
	if (!_hunspell) return {};

	const auto suggestions = _hunspell->suggest(str::utf8_cast2(word));

	std::vector<std::string> result;
	result.reserve(suggestions.size()); // Reserve space for better performance
	std::transform(suggestions.begin(),
	               suggestions.end(),
	               std::back_inserter(result),
	               [](const std::string& s) { return str::utf8_cast2(s); });

	return result;
}

namespace
{
	custom_dictionary_add_result persist_custom_dictionary_word(const df::file_path path,
	                                                           const df::folder_path user_folder,
	                                                           const std::string_view word)
	{
		if (!user_folder.exists() && !platform::create_folder(user_folder).success())
		{
			return {custom_dictionary_add_status::failed, std::format("could not create {}", user_folder)};
		}

		std::ofstream f(platform::to_stream_path(path), std::ios::out | std::ios::app);
		if (!f.is_open())
		{
			return {custom_dictionary_add_status::failed, std::format("could not open {}", path)};
		}

		f << word << '\n';
		if (!f)
		{
			return {custom_dictionary_add_status::failed, std::format("could not append {}", path)};
		}

		f.flush();
		if (!f)
		{
			return {custom_dictionary_add_status::failed, std::format("could not flush {}", path)};
		}

		f.close();
		if (!f)
		{
			return {custom_dictionary_add_status::failed, std::format("could not close {}", path)};
		}

		return {custom_dictionary_add_status::persisted, {}};
	}
}

void spell_check::add_word(const std::string_view word,
                           std::function<void(custom_dictionary_add_result)> complete) const
{
	// Add input validation
	if (word.empty())
	{
		if (complete) complete({custom_dictionary_add_status::ignored, {}});
		return;
	}

	df::async_i* async = nullptr;
	uint64_t generation = 0;
	df::file_path custom_dic_path;
	df::folder_path user_folder;
	std::function<custom_dictionary_add_result(const df::file_path&, std::string_view)> writer;

	{
		platform::exclusive_lock lock(_rw);
		async = _async;
		generation = _generation;
		custom_dic_path = _custom_dic_path;
		user_folder = _user_folder;
		writer = _custom_dictionary_writer;
	}

	const std::string word_copy(word);

	if (!async)
	{
		platform::exclusive_lock lock(_rw);
		if (_hunspell) _hunspell->add(str::utf8_cast2(word_copy));
		if (complete) complete({custom_dictionary_add_status::session_only, "no async strategy configured"});
		return;
	}

	uint64_t pending_id = 0;

	{
		platform::exclusive_lock lock(_rw);
		pending_id = ++_next_custom_word_id;
		_pending_custom_words.emplace_back(pending_custom_word{pending_id, word_copy, custom_dic_path, user_folder,
		                                                       writer});
	}

	async->queue_async(async_queue::work,
	                   [this, async, generation, custom_dic_path, user_folder, word_copy, writer, pending_id,
		                   complete = std::move(complete)]
	                   {
		                   custom_dictionary_add_result result;

		                   try
		                   {
			                   result = writer
				                            ? writer(custom_dic_path, word_copy)
				                            : persist_custom_dictionary_word(custom_dic_path, user_folder, word_copy);
		                   }
		                   catch (const std::exception& e)
		                   {
			                   result = {custom_dictionary_add_status::failed,
			                             std::format("failed to persist custom word: {}", e.what())};
		                   }

		                   {
			                   platform::exclusive_lock lock(_rw);
			                   std::erase_if(_pending_custom_words, [pending_id](const pending_custom_word& pending)
			                   {
				                   return pending.id == pending_id;
			                   });
		                   }

		                   if (result.status == custom_dictionary_add_status::failed)
		                   {
			                   df::log(__FUNCTION__, result.message);
		                   }

		                   async->queue_ui([this, generation, word_copy, result, complete]() mutable
		                   {
			                   auto final_result = result;

			                   {
				                   platform::exclusive_lock lock(_rw);
				                   if (generation != _generation)
				                   {
					                   final_result = {custom_dictionary_add_status::stale, {}};
				                   }
				                   else if (final_result.persisted() && _hunspell)
				                   {
					                   _hunspell->add(str::utf8_cast2(word_copy));
				                   }
			                   }

			                   if (complete) complete(final_result);
		                   });
	                   });
}

void spell_check::flush_pending_custom_words() const
{
	std::vector<pending_custom_word> pending;

	{
		platform::exclusive_lock lock(_rw);
		pending = std::move(_pending_custom_words);
		_pending_custom_words.clear();
	}

	for (const auto& word : pending)
	{
		custom_dictionary_add_result result;

		try
		{
			result = word.writer
				         ? word.writer(word.path, word.word)
				         : persist_custom_dictionary_word(word.path, word.user_folder, word.word);
		}
		catch (const std::exception& e)
		{
			result = {custom_dictionary_add_status::failed,
			          std::format("failed to persist custom word during shutdown: {}", e.what())};
		}

		if (result.status == custom_dictionary_add_status::failed)
		{
			df::log(__FUNCTION__, result.message);
		}
	}
}

bool spell_check::is_ready() const
{
	platform::shared_lock lock(_rw);
	return !!_hunspell;
}

void spell_check::configure_paths_for_tests(const df::folder_path shipped_folder, const df::folder_path user_folder)
{
	platform::exclusive_lock lock(_rw);
	_shipped_folder = shipped_folder;
	_user_folder = user_folder;
	_custom_dic_path = _user_folder.combine_file("custom.dic");
	_hunspell.reset();
	_loading = false;
	_pending_custom_words.clear();
	++_generation;
}

void spell_check::reset_paths_for_tests()
{
	platform::exclusive_lock lock(_rw);
	set_default_paths();
	_hunspell.reset();
	_loading = false;
	_pending_custom_words.clear();
	++_generation;
}

void spell_check::set_load_gate_for_tests(std::function<void()> gate)
{
	platform::exclusive_lock lock(_rw);
	_load_gate = std::move(gate);
}

void spell_check::set_custom_dictionary_writer_for_tests(
	std::function<custom_dictionary_add_result(const df::file_path&, std::string_view)> writer)
{
	platform::exclusive_lock lock(_rw);
	_custom_dictionary_writer = std::move(writer);
}

void spell_check::clear_test_hooks()
{
	platform::exclusive_lock lock(_rw);
	_load_gate = {};
	_custom_dictionary_writer = {};
}
