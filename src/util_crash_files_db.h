// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Crash-loop protection. Records which media files were open when the process faulted so
// the next launch does not scan them again, and keeps that list bounded, attributed and recoverable.

#pragma once

class crash_files_db
{
	struct open_file
	{
		df::file_path path;
		std::string_view context;
		uint32_t thread_id = 0;
	};

	using open_file_id = uint64_t;
	using open_file_map = df::hash_map<open_file_id, open_file>;
	using path_set = df::hash_set<df::file_path, df::ihash, df::ieq>;

	// The list only has to survive until the next launch, so a small ceiling is enough to stop a
	// repeating crash from growing the file without bound. Lines left by other releases count toward
	// it, because they are still occupying the file.
	static constexpr size_t max_entries = 512;

	df::file_path _crash_files_path;
	std::string _release_tag;

	path_set _crash_files;
	size_t _lines_on_disk = 0;

	open_file_map _open_files;
	open_file_id _next_open_file_id = 1;
	platform::mutex _mtx;

	// Each line is "<release>\t<path>". Only the running release's entries skip a file. A decoder fix
	// ships in an update, and without the tag a file blacklisted once would never be scanned again on
	// this install - a permanently missing thumbnail with nothing in the product to recover it.
	void load()
	{
		std::ifstream file(platform::to_stream_path(_crash_files_path));
		std::string line;

		while (_lines_on_disk < max_entries && std::getline(file, line))
		{
			++_lines_on_disk;

			const std::string_view entry(line);
			const auto tab = entry.find('\t');

			if (tab == std::string_view::npos) continue;
			if (entry.substr(0, tab) != _release_tag) continue;

			const auto path = entry.substr(tab + 1);
			if (!path.empty()) _crash_files.emplace(path);
		}
	}

public:
	// Deliberately the release line and not the build or the point release. The build number changes
	// on every compile, so tagging with it retried every recorded file after almost any update and
	// handed the user the same crash again. Retrying once per release line is where a decoder fix
	// actually reaches them, and costs at most one repeat crash per file.
	static std::string release_tag(const std::string_view app_version)
	{
		return str::to_string(df::version(app_version).major);
	}

	crash_files_db(const df::file_path path, const std::string_view release_tag) :
		_crash_files_path(path), _release_tag(release_tag)
	{
		try
		{
			load();
		}
		catch (const std::exception& e)
		{
			// Bounded fallback: an unreadable skip list only costs one repeated decoder crash, and
			// this is constructed during startup where a throw would take the whole launch with it.
			df::log(__FUNCTION__, e.what());
			_crash_files.clear();
			_lines_on_disk = 0;
		}
	}

	bool is_known_crash_file(const df::file_path path) const
	{
		return _crash_files.contains(path);
	}

	// A skipped file is silently left without metadata or a thumbnail, so the count is reported at
	// startup rather than left for the user to deduce.
	size_t skipped_file_count() const
	{
		return _crash_files.size();
	}

	bool is_full() const
	{
		return _lines_on_disk >= max_entries;
	}

	open_file_id add_open(const df::file_path path, const std::string_view context)
	{
		platform::exclusive_lock lock(_mtx);
		const auto id = _next_open_file_id++;
		_open_files[id] = {path, context, platform::current_thread_id()};
		return id;
	}

	void remove_open(const open_file_id id)
	{
		platform::exclusive_lock lock(_mtx);
		_open_files.erase(id);
	}

	// The following two readers run from the crash / application-recovery handler
	// (see app_frame::crash and recover_callback). They take _mtx only if it is free: a blocking
	// acquire would deadlock the handler if the crashing thread happened to hold the lock inside
	// add_open/remove_open, and iterating without it is undefined - a concurrent insert rehashes the
	// map and invalidates the iterator, which turns one crash into two and loses the protection this
	// list exists to provide. A refused lock costs one unrecorded crash, which is bounded and stated.
	void flush_open_files() const
	{
		if (is_full()) return;

		platform::try_exclusive_lock lock(_mtx);

		if (!lock.locked())
		{
			df::log(__FUNCTION__, "open files busy - crash list not updated");
			return;
		}

		if (_open_files.empty()) return;

		// Several workers decode at once, so most open files are bystanders. Only the thread running
		// this handler faulted, so record what it had open and leave the rest scannable. The recovery
		// callback runs on its own thread and matches nothing, which falls back to recording them all.
		const auto faulting_thread = platform::current_thread_id();
		const auto attributed = std::ranges::any_of(_open_files, [faulting_thread](const auto& i)
		{
			return i.second.thread_id == faulting_thread;
		});

		auto room = max_entries - _lines_on_disk;
		std::string appended;

		path_set appended_paths;

		for (const auto& [id, open] : _open_files)
		{
			if (room == 0) break;
			if (attributed && open.thread_id != faulting_thread) continue;
			if (_crash_files.contains(open.path) || appended_paths.contains(open.path)) continue;

			df::log(__FUNCTION__, std::format("Add file type to crash list {}", open.path.extension()));
			appended += std::format("{}\t{}\n", _release_tag, open.path.str());
			appended_paths.emplace(open.path);
			--room;
		}

		if (appended.empty()) return;

		// Appended rather than rewritten: a second fault inside this handler must not be able to
		// truncate away the protection already earned by earlier crashes.
		std::ofstream file(platform::to_stream_path(_crash_files_path), std::ios_base::app);
		file << appended;

		// A read-only install folder makes this the difference between one crash and a crash on every
		// launch, so it is stated rather than left as an unexplained repeat.
		if (!file) df::log(__FUNCTION__, std::format("could not write {}", _crash_files_path.str()));
	}

	void log_open_files() const
	{
		platform::try_exclusive_lock lock(_mtx);

		if (!lock.locked())
		{
			df::log(__FUNCTION__, "open files busy - not listed");
			return;
		}

		const auto faulting_thread = platform::current_thread_id();

		for (const auto& [id, open] : _open_files)
		{
			// The report is uploaded, so the file is identified by what diagnoses the fault - its type
			// and the stage that had it open - rather than by name. The full path is recorded locally
			// in the crash-files list, which stays on the machine.
			df::log(__FUNCTION__, std::format("Open file {} in {}{}", open.path.extension(), open.context,
			                                  open.thread_id == faulting_thread ? " (faulting thread)" : ""));
		}
	}
};


struct record_open_path
{
	crash_files_db& files_that_crash_diffractor_;
	uint64_t _id = 0;

	record_open_path(crash_files_db& files_that_crash_diffractor, const df::file_path path,
	                 const std::string_view context) : files_that_crash_diffractor_(files_that_crash_diffractor)
	{
		_id = files_that_crash_diffractor_.add_open(path, context);
	}

	~record_open_path()
	{
		files_that_crash_diffractor_.remove_open(_id);
	}
};

// Constructed on first use rather than as a global: the constructor resolves known folders and reads
// a file, and before WinMain a throw there would end the process with no log and no message box.
crash_files_db& crash_files();

// Called from the crash handler and the shutdown path.
void flush_open_files_to_crash_files_list();
void log_open_files_to_crash_files_list();
