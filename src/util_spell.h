// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Spell checking integration. Wraps Hunspell library for
// spell checking user input in metadata fields.

#pragma once

class Hunspell;

enum class custom_dictionary_add_status
{
	ignored,
	persisted,
	session_only,
	failed,
	stale,
};

struct custom_dictionary_add_result
{
	custom_dictionary_add_status status = custom_dictionary_add_status::ignored;
	std::string message;

	bool persisted() const { return status == custom_dictionary_add_status::persisted; }
};

class spell_check
{
	mutable platform::mutex _rw;

	struct pending_custom_word
	{
		uint64_t id = 0;
		std::string word;
		df::file_path path;
		df::folder_path user_folder;
		std::function<custom_dictionary_add_result(const df::file_path&, std::string_view)> writer;
	};

	_Guarded_by_(_rw) std::unique_ptr<Hunspell> _hunspell;
	_Guarded_by_(_rw) uint64_t _generation = 0;
	_Guarded_by_(_rw) bool _loading = false;
	_Guarded_by_(_rw) mutable uint64_t _next_custom_word_id = 0;
	_Guarded_by_(_rw) mutable std::vector<pending_custom_word> _pending_custom_words;
	_Guarded_by_(_rw) df::async_i* _async = nullptr;
	_Guarded_by_(_rw) std::function<void()> _load_gate;
	_Guarded_by_(_rw) std::function<custom_dictionary_add_result(const df::file_path&, std::string_view)>
		_custom_dictionary_writer;

	df::file_path _custom_dic_path;

	// Dictionaries ship beside the executable, but that folder is read-only in a Store install and
	// in any per-machine install, so reads come from either folder and every write goes to the
	// per-user one.
	df::folder_path _shipped_folder;
	df::folder_path _user_folder;

	df::file_path find_dictionary(std::string_view name, std::string_view ext) const;
	bool ensure_user_folder() const;
	void set_default_paths();

public:
	spell_check();
	~spell_check();

	// Delete copy constructor and assignment operator for safety
	spell_check(const spell_check&) = delete;
	spell_check& operator=(const spell_check&) = delete;

	void lazy_download(df::async_i& async);
	void lazy_load();
	void queue_load();
	void queue_load(df::async_i& async);
	void dictionary_downloaded(df::async_i& async);
	void configure_async(df::async_i* async);
	bool is_word_valid(std::string_view word) const;
	std::vector<std::string> suggest(std::string_view word) const;
	void add_word(std::string_view word, std::function<void(custom_dictionary_add_result)> complete = {}) const;
	void flush_pending_custom_words() const;
	bool is_ready() const;

	// The file "Add to dictionary" appends to. Exposed so a test can prove it is somewhere the user
	// can write, without writing there.
	df::file_path custom_dictionary_path() const { return _custom_dic_path; }
	void configure_paths_for_tests(df::folder_path shipped_folder, df::folder_path user_folder);
	void reset_paths_for_tests();
	void set_load_gate_for_tests(std::function<void()> gate);
	void set_custom_dictionary_writer_for_tests(
		std::function<custom_dictionary_add_result(const df::file_path&, std::string_view)> writer);
	void clear_test_hooks();
};

// Its constructor resolves known folders, probes the file system and may create the dictionaries
// directory. As a global that would run before WinMain, where a throw ends the process with nothing
// logged and no message box; constructed on first use instead, inside the app's error handling.
spell_check& spell();

// spell() as the edit controls see it: they check words against it, and a word added through one is
// logged here if writing it fails.
ui::spelling_dictionary& spell_dictionary();
