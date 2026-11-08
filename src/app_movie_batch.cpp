// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implements the /movie-check, /movie-import and /movie-render command line options.
// Import turns each Windows Live Movie Maker project into a native OpenTimelineIO one, finding
// sources that are no longer at their stored paths among the collection's files, and reports what
// was read, where each source was found and what the import left out. Render makes a movie from
// each native project with the Movie render. Check is either one without writing anything. Never
// modifies a source or a Movie Maker project. docs/movie.md owns the rules.

#include "pch.h"
#include "app_command_line.h"
#include "app_settings.h"
#include "app_text.h"
#include "files.h"
#include "model_movie.h"
#include "libdesktop/platform.h"
#include "view_movie.h"

#include <chrono>
#include <cstdio>

namespace
{
	// A project file is a document, not a stream. Mirrors the limit the view's Open applies.
	constexpr uint64_t max_project_bytes = 32ull * 1024ull * 1024ull;

	// Past a screenful a list stops being feedback, so longer ones are counted rather than printed.
	constexpr size_t max_listed_paths = 25;

	// Probing a large project takes long enough that silence would read as a hang.
	constexpr size_t probe_report_interval = 250;

	using batch_clock = std::chrono::steady_clock;

	double seconds_since(const batch_clock::time_point started)
	{
		return std::chrono::duration<double>(batch_clock::now() - started).count();
	}

	// Whole lines only, flushed: the usual reader is a pipe into PowerShell, which shows nothing of a
	// line until it ends.
	void print_line(const std::string_view text)
	{
		printf("%.*s\n", static_cast<int>(text.size()), text.data());
		fflush(stdout);
	}

	std::string format_duration(const double seconds)
	{
		const auto total = static_cast<int64_t>(std::llround(std::max(0.0, seconds)));
		const auto hours = total / 3600;
		const auto minutes = total / 60 % 60;
		const auto rest = total % 60;
		return hours > 0 ? std::format("{}:{:02}:{:02}", hours, minutes, rest) : std::format("{}:{:02}", minutes, rest);
	}

	bool has_extension(const core::file_path path, const std::string_view ext)
	{
		return core::icmp(path.extension(), ext) == 0;
	}

	std::string read_project_text(const core::file_path path)
	{
		const auto file = platform::open_file(path, platform::file_open_mode::read);
		if (!file) return {};

		const auto size = file->size();
		if (size == 0 || size > max_project_bytes) return {};

		std::string result(static_cast<size_t>(size), '\0');
		const auto read = file->read(std::bit_cast<uint8_t*>(result.data()), size);
		result.resize(static_cast<size_t>(read));
		return result;
	}

	std::string render_failure(const movie_render_result& result)
	{
		switch (result.reason)
		{
		case movie_render_error::no_length: return std::string(tt.movie_error_no_length.sv());
		case movie_render_error::no_encoder: return std::string(tt.movie_error_no_encoder.sv());
		case movie_render_error::no_memory: return std::string(tt.movie_error_no_memory.sv());
		case movie_render_error::undecodable: return std::string(tt.movie_error_undecodable.sv());
		case movie_render_error::encoder_failed: return std::string(tt.movie_error_encoder_failed.sv());
		case movie_render_error::output_is_source: return std::string(tt.movie_error_output_is_source.sv());
		case movie_render_error::none: break;
		}

		return result.message.empty() ? std::string(tt.platform_text.error_unknown.sv()) : result.message;
	}

	bool is_usable(const movie_clip& clip)
	{
		return clip.is_probed && !clip.is_missing && clip.duration() > 0 && clip.extent.cx > 0 && clip.extent.cy > 0;
	}

	bool is_found_by_name(const movie_source_match match)
	{
		return match == movie_source_match::only_name || match == movie_source_match::same_folders ||
			match == movie_source_match::same_length || match == movie_source_match::siblings;
	}

	void print_paths(const std::string_view label, std::vector<core::file_path> paths)
	{
		std::sort(paths.begin(), paths.end());

		for (size_t i = 0; i < paths.size() && i < max_listed_paths; ++i)
		{
			print_line(std::format("  {:<10}{}", label, paths[i]));
		}

		if (paths.size() > max_listed_paths)
		{
			print_line(std::format("  {:<10}and {} more", label, paths.size() - max_listed_paths));
		}
	}

	void print_omitted(const movie_load_result& loaded)
	{
		if (loaded.omitted.empty()) return;

		std::string parts;

		for (const auto& [name, count] : loaded.omitted)
		{
			if (!parts.empty()) parts += ", ";
			parts += std::format("{} {}", name, count);
		}

		print_line(std::format("  left out  {}: {}", loaded.ignored_elements, parts));
	}

	// Every file in the collection by name, and in the -movie-media folder when one is given. Walked
	// once per run, because walking a large collection is the slowest thing an import does. Folders
	// are entered as the index enters them -- excluded ones and ones that cannot be recursed into are
	// left out -- so the search covers the collection's members and nothing else. The app answers the
	// same question from its index.
	class source_search
	{
	public:
		source_search(core::index_roots roots, const bool show_hidden) : _roots(std::move(roots)), _show_hidden(show_hidden)
		{
		}

		const core::index_roots& roots() const
		{
			return _roots;
		}

		std::vector<std::vector<core::file_path>> find(const std::vector<core::file_path>& missing)
		{
			walk();

			std::vector<std::vector<core::file_path>> result(missing.size());

			for (size_t i = 0; i < missing.size(); ++i)
			{
				const auto found = _by_name.find(std::string(missing[i].name()));
				if (found != _by_name.end()) result[i] = found->second;
			}

			return result;
		}

		void walk()
		{
			if (_walked) return;
			_walked = true;

			const auto started = batch_clock::now();
			size_t files = 0;

			for (const auto& file : _roots.files)
			{
				_by_name[std::string(file.name())].emplace_back(file);
				++files;
			}

			// Roots can nest -- a well-known folder inside a "More folders" line -- and a folder walked
			// twice would offer every file in it twice, which the choice would read as two candidates.
			core::unique_folders walked;
			std::vector<core::folder_path> pending(_roots.folders.begin(), _roots.folders.end());

			while (!pending.empty())
			{
				const auto folder = pending.back();
				pending.pop_back();

				if (core::is_excluded(_roots, folder) || !walked.emplace(folder).second) continue;

				const auto contents = platform::iterate_file_items(folder, _show_hidden);

				for (const auto& file : contents.files)
				{
					_by_name[std::string(file.name)].emplace_back(folder, file.name);
					++files;
				}

				for (const auto& sub : contents.folders)
				{
					if (sub.can_recurse) pending.emplace_back(folder.combine(sub.name.sv()));
				}
			}

			print_line(std::format("            {} files in {:.1f} s", core::format_count(files), seconds_since(started)));
		}

	private:
		core::index_roots _roots;
		bool _show_hidden = false;
		bool _walked = false;
		core::hash_map<std::string, std::vector<core::file_path>, core::ihash, core::ieq> _by_name;
	};

	// Where each stored folder's sources were found, in the order the timeline first meets them. This
	// is the answer to "how are the paths set up": one line per folder rather than one per file.
	void print_folder_moves(const std::vector<movie_import_source>& sources)
	{
		struct folder_move
		{
			core::folder_path from;
			std::vector<std::pair<core::folder_path, int>> to;
		};

		std::vector<folder_move> moves;

		for (const auto& source : sources)
		{
			if (!is_found_by_name(source.match)) continue;

			const auto from = source.stored.folder();
			const auto to = source.path.folder();

			auto move = std::ranges::find_if(moves, [from](const folder_move& m) { return m.from == from; });
			if (move == moves.end()) move = moves.insert(moves.end(), folder_move{from, {}});

			auto target = std::ranges::find_if(move->to, [to](const auto& t) { return t.first == to; });
			if (target == move->to.end()) move->to.emplace_back(to, 1);
			else ++target->second;
		}

		for (auto& move : moves)
		{
			std::ranges::stable_sort(move.to, [](const auto& l, const auto& r) { return l.second > r.second; });

			print_line(std::format("    {}", move.from));

			for (const auto& [to, count] : move.to)
			{
				print_line(std::format("      -> {} ({})", to, count));
			}
		}
	}

	// What an import found, the same whether it writes the project or only checks it.
	void report_import(const movie_import& imported)
	{
		const auto& sources = imported.sources;

		const auto count = [&sources](const movie_source_match match)
		{
			return std::ranges::count_if(sources, [match](const movie_import_source& s) { return s.match == match; });
		};

		const auto by_name = count(movie_source_match::only_name);
		const auto by_folders = count(movie_source_match::same_folders);
		const auto by_length = count(movie_source_match::same_length);
		const auto by_siblings = count(movie_source_match::siblings);
		const auto found = by_name + by_folders + by_length + by_siblings;
		const auto ambiguous = count(movie_source_match::ambiguous);
		const auto other_length = count(movie_source_match::other_length);
		const auto missing = count(movie_source_match::missing) + other_length;

		print_line(std::format("  sources   {} files: {} at their stored path, {} found by name, {} ambiguous, {} missing",
		                  sources.size(), count(movie_source_match::stored), found, ambiguous, missing));

		if (found > 0)
		{
			print_line(std::format("            {} by a unique name, {} by matching folder names, {} by length, "
			                  "{} beside sources from the same folder", by_name, by_folders, by_length, by_siblings));
			print_folder_moves(sources);
		}

		size_t listed = 0;

		for (const auto& source : sources)
		{
			if (source.match != movie_source_match::ambiguous) continue;

			if (listed++ == max_listed_paths)
			{
				print_line(std::format("  ambiguous and {} more", ambiguous - static_cast<ptrdiff_t>(max_listed_paths)));
				break;
			}

			print_line(std::format("  ambiguous {}", source.stored));

			auto candidates = source.candidates;
			std::ranges::sort(candidates, [](const auto& l, const auto& r) { return l.first < r.first; });

			for (const auto& candidate : candidates)
			{
				print_line(std::format("              could be {}", candidate.first));
			}
		}

		// The namesakes a recorded length ruled out, with both lengths, so the report says why a file
		// that is plainly there was not used.
		listed = 0;

		for (const auto& source : sources)
		{
			if (source.match != movie_source_match::other_length) continue;

			if (listed++ == max_listed_paths)
			{
				print_line(std::format("  not found and {} more", other_length - static_cast<ptrdiff_t>(max_listed_paths)));
				break;
			}

			print_line(std::format("  not found {}, {:.1f} s long", source.stored, source.recorded_seconds));

			auto candidates = source.candidates;
			std::ranges::sort(candidates, [](const auto& l, const auto& r) { return l.first < r.first; });

			for (const auto& [path, seconds] : candidates)
			{
				print_line(std::format("              {} has its name but is {:.1f} s", path, seconds));
			}
		}

		std::vector<core::file_path> lost;
		std::vector<core::file_path> unreadable;
		std::vector<std::string> differing;
		size_t checked = 0;

		for (const auto& source : sources)
		{
			if (source.match == movie_source_match::missing) lost.emplace_back(source.stored);
			if (source.match != movie_source_match::stored && !is_found_by_name(source.match)) continue;

			if (!source.probe.found)
			{
				unreadable.emplace_back(source.path);
				continue;
			}

			// The one check that looks at the file rather than its name: a different clip that happens
			// to share a camera's file number almost never shares its length too.
			if (!source.probe.is_photo && source.recorded_seconds > 0)
			{
				++checked;

				if (std::abs(source.probe.duration - source.recorded_seconds) > movie_source_length_tolerance)
				{
					differing.emplace_back(std::format("{} is {:.1f} s long; the project recorded {:.1f} s",
					                                   source.path, source.probe.duration, source.recorded_seconds));
				}
			}
		}

		print_paths("missing", lost);
		print_paths("unreadable", unreadable);

		if (checked > 0)
		{
			print_line(std::format("  lengths   {} of {} videos are as long as the project recorded", checked - differing.size(),
			                  checked));

			for (size_t i = 0; i < differing.size() && i < max_listed_paths; ++i)
			{
				print_line(std::format("  differs   {}", differing[i]));
			}

			if (differing.size() > max_listed_paths)
			{
				print_line(std::format("  differs   and {} more", differing.size() - max_listed_paths));
			}
		}
	}

	struct prepared_movie
	{
		std::vector<movie_clip> clips;
		movie_output output;
		double duration = 0;
		bool ready = false;
	};

	// What a render of these clips would make and whether it can, by the rule the Movie view's Render
	// follows.
	prepared_movie prepare_movie(std::vector<movie_clip> clips, const movie_settings& settings,
	                             const core::file_path project_path, const bool skip_missing)
	{
		if (skip_missing)
		{
			const auto before = clips.size();
			std::erase_if(clips, [](const movie_clip& clip) { return !is_usable(clip); });

			if (clips.size() < before)
			{
				print_line(std::format("  skipping  {} of {} clips with no usable source", before - clips.size(), before));
			}
		}

		movie_project project;
		project.reset(clips, settings, project_path);

		prepared_movie result;
		result.output = project.output();
		result.duration = calc_movie_timing(clips, settings).duration;
		result.ready = project.is_ready_to_render();

		const auto photos = std::ranges::count_if(clips, [](const movie_clip& clip) { return clip.is_photo; });

		print_line(std::format("  movie     {} videos and {} photos, {} long; {}x{} at {} fps, {:.1f} Mbit/s",
		                  std::ssize(clips) - photos, photos, format_duration(result.duration),
		                  result.output.extent.cx, result.output.extent.cy, result.output.frame_rate,
		                  result.output.video_bitrate / 1000000.0));
		print_line(std::format("  settings  {}, fade in {}, fade out {}, photos held {:.1f} s",
		                  settings.transition == movie_transition::crossfade
			                  ? std::format("crossfade {:.1f} s", settings.transition_seconds)
			                  : "cuts"s,
		                  settings.fade_in ? "on" : "off", settings.fade_out ? "on" : "off", settings.photo_seconds));

		if (!result.ready)
		{
			if (clips.empty())
			{
				print_line("  not ready no clip has a usable source");
			}
			else
			{
				const auto unusable = std::ranges::count_if(clips, [](const movie_clip& c) { return !is_usable(c); });
				print_line(std::format("  not ready {} of {} clips have no usable source{}", unusable, clips.size(),
				                  skip_missing ? "" : "; -movie-skip-missing renders without them"));
			}
		}

		result.clips = std::move(clips);
		return result;
	}

	enum class project_outcome
	{
		ready,
		imported,
		imported_incomplete,
		rendered,
		skipped,
		failed,
	};

	// Where this run writes `project_path`'s output, refusing what would replace a file it should
	// not: one written earlier in this run, or one already there that nobody asked to replace.
	std::optional<core::file_path> choose_destination(const core::file_path project_path, const std::string_view ext,
	                                                const core::folder_path output_folder, const bool overwrite,
	                                                const std::vector<core::file_path>& written, bool& skipped)
	{
		const auto folder = output_folder.is_empty() ? project_path.folder() : output_folder;
		const auto destination = folder.combine_file_ext(project_path.file_name_without_extension(), ext);

		if (std::ranges::any_of(written, [destination](const core::file_path path) { return path == destination; }))
		{
			print_line(std::format("  skipped   {} was written by an earlier project in this run", destination));
			return std::nullopt;
		}

		if (platform::exists(destination) && !overwrite)
		{
			print_line(std::format("  skipped   {} exists; pass -movie-overwrite to replace it", destination));
			skipped = true;
			return std::nullopt;
		}

		return destination;
	}

	// A Movie Maker project: imported into a native one, or checked as the import would make it.
	project_outcome import_project(const core::file_path project_path, const movie_batch_options& options,
	                               source_search& search, const core::folder_path output_folder,
	                               std::vector<core::file_path>& written)
	{
		std::optional<core::file_path> destination;
		if (options.mode != movie_batch_mode::check)
		{
			auto skipped = false;
			destination = choose_destination(project_path, ".otio", output_folder, options.overwrite, written, skipped);
			if (!destination) return skipped ? project_outcome::skipped : project_outcome::failed;
		}

		const auto text = read_project_text(project_path);

		if (text.empty())
		{
			print_line("  failed    the project file could not be read");
			return project_outcome::failed;
		}

		const auto find = [&search](const std::vector<core::file_path>& missing) { return search.find(missing); };
		auto imported = import_movie_maker_project(text, project_path.folder(), find);
		const auto& loaded = imported.loaded;

		print_line(std::format("  read      {} clips", loaded.clips.size()));
		print_omitted(loaded);

		if (!loaded)
		{
			print_line(loaded.status == movie_load_status::empty
				      ? "  failed    the project holds no clip Movie can use"
				      : "  failed    this is not a project Movie can read");
			return project_outcome::failed;
		}

		report_import(imported);

		// The native project keeps every clip, found or not: a missing one is still the user's to
		// relink in Movie, so only rendering may leave it out.
		const auto movie = prepare_movie(loaded.clips, loaded.settings, project_path, false);

		if (options.mode == movie_batch_mode::check)
		{
			if (movie.ready) print_line("  ready     every clip has a usable source");
			return movie.ready ? project_outcome::ready : project_outcome::failed;
		}

		const auto json = write_otio(loaded.clips, loaded.settings, destination->folder());
		const auto error = write_movie_project_file(*destination, json, !options.overwrite);

		if (!error.empty())
		{
			print_line(std::format("  failed    {}: {}", *destination, error));
			return project_outcome::failed;
		}

		written.emplace_back(*destination);
		print_line(std::format("  wrote     {}", *destination));
		return movie.ready ? project_outcome::imported : project_outcome::imported_incomplete;
	}

	// A native project: rendered, or checked as the render would find it.
	project_outcome render_project(const core::file_path project_path, const movie_batch_options& options,
	                               const core::folder_path output_folder, std::vector<core::file_path>& written)
	{
		std::optional<core::file_path> destination;
		if (options.mode != movie_batch_mode::check)
		{
			auto skipped = false;
			destination = choose_destination(project_path, ".mp4", output_folder, options.overwrite, written, skipped);
			if (!destination) return skipped ? project_outcome::skipped : project_outcome::failed;
		}

		const auto text = read_project_text(project_path);

		if (text.empty())
		{
			print_line("  failed    the project file could not be read");
			return project_outcome::failed;
		}

		const auto loaded = read_otio(text, project_path.folder());

		print_line(std::format("  read      {} clips", loaded.clips.size()));
		print_omitted(loaded);

		if (!loaded)
		{
			print_line(loaded.status == movie_load_status::empty
				      ? "  failed    the project holds no clips"
				      : "  failed    this is not a project Movie can read");
			return project_outcome::failed;
		}

		core::hash_map<core::file_path, movie_probe_result, core::ihash, core::ieq> probes;
		std::vector<core::file_path> missing;
		std::vector<core::file_path> unreadable;

		for (const auto& clip : loaded.clips)
		{
			if (probes.contains(clip.path)) continue;

			const auto probe = probe_movie_source(clip.path);
			if (!probe.found) (platform::exists(clip.path) ? unreadable : missing).emplace_back(clip.path);
			probes.emplace(clip.path, probe);

			if (probes.size() % probe_report_interval == 0)
			{
				print_line(std::format("  probing   {} sources", probes.size()));
			}
		}

		print_line(std::format("  sources   {} files: {} usable, {} missing, {} unreadable", probes.size(),
		                  probes.size() - missing.size() - unreadable.size(), missing.size(), unreadable.size()));
		print_paths("missing", missing);
		print_paths("unreadable", unreadable);

		auto clips = loaded.clips;

		for (auto& clip : clips)
		{
			clip = apply_movie_probe(clip, probes.at(clip.path), loaded.settings.photo_seconds);
		}

		auto movie = prepare_movie(std::move(clips), loaded.settings, project_path, options.skip_missing);
		if (!movie.ready) return project_outcome::failed;

		if (options.mode == movie_batch_mode::check)
		{
			print_line("  ready     every clip has a usable source");
			return project_outcome::ready;
		}

		movie_render_request request;
		request.clips = std::move(movie.clips);
		request.settings = loaded.settings;
		request.output = movie.output;
		request.path = *destination;
		request.fail_if_exists = !options.overwrite;

		const auto control = std::make_shared<movie_render_control>();
		const auto token = core::cancel_token(control->cancelled);
		const auto started = batch_clock::now();
		auto reported = 0;

		print_line(std::format("  render    {}", *destination));

		const auto report = [&reported, started](const double fraction)
		{
			const auto tenth = static_cast<int>(fraction * 10.0);
			if (tenth <= reported || tenth >= 10) return;

			reported = tenth;
			print_line(std::format("            {}% after {}", tenth * 10, format_duration(seconds_since(started))));
		};

		const auto result = render_movie_to_file(request, report, token, control);

		if (!result.ok)
		{
			print_line(std::format("  failed    {}", render_failure(result)));

			if (!result.source.is_empty())
			{
				print_line(std::format("            no frame from {} at {:.2f} s", result.source, result.source_time));
			}

			return project_outcome::failed;
		}

		written.emplace_back(*destination);

		const auto took = seconds_since(started);
		print_line(std::format("  rendered  {} of movie in {} ({:.1f}x real time)", format_duration(movie.duration),
		                  format_duration(took), took > 0 ? movie.duration / took : 0.0));
		return project_outcome::rendered;
	}
}

bool movie_batch_test_seams::project_is_skipped(const core::file_path project, const movie_batch_options& options)
{
	std::vector<core::file_path> written;
	source_search search({}, false);
	const auto folder = options.output.empty() ? core::folder_path{} : core::folder_path(options.output);
	const auto outcome = has_extension(project, ".wlmp")
		                     ? import_project(project, options, search, folder, written)
		                     : render_project(project, options, folder, written);
	return outcome == project_outcome::skipped;
}

int run_movie_batch(const movie_batch_options& options)
{
	load_file_types();

	const auto mode = options.mode;
	const auto* const tool = mode == movie_batch_mode::import
		                         ? "movie-import"
		                         : mode == movie_batch_mode::render
		                         ? "movie-render"
		                         : "movie-check";

	// Import reads Movie Maker projects, render reads native ones, and check reads either.
	const auto reads = [mode](const core::file_path path)
	{
		const auto wlmp = has_extension(path, ".wlmp");
		const auto otio = has_extension(path, ".otio");
		return mode == movie_batch_mode::import ? wlmp : mode == movie_batch_mode::render ? otio : wlmp || otio;
	};

	if (!core::is_path(options.projects))
	{
		print_line(std::format("{}: give the full path of a project or of a folder of projects: {}", tool,
		                  options.projects));
		return 1;
	}

	std::vector<core::file_path> projects;
	const auto projects_folder = core::folder_path(options.projects);

	if (platform::exists(projects_folder))
	{
		const auto contents = platform::iterate_file_items(projects_folder, false);

		for (const auto& file : contents.files)
		{
			const auto path = core::file_path(projects_folder, file.name);
			if (reads(path)) projects.emplace_back(path);
		}

		std::ranges::sort(projects, [](const core::file_path l, const core::file_path r)
		{
			return core::icmp(l.name(), r.name()) < 0;
		});
	}
	else
	{
		const auto file = core::file_path(options.projects);

		if (!platform::exists(file))
		{
			print_line(std::format("{}: no project or folder at {}", tool, options.projects));
			return 1;
		}

		if (!reads(file))
		{
			print_line(mode == movie_batch_mode::render && has_extension(file, ".wlmp")
				      ? std::format("{}: {} is a Movie Maker project; import it with -movie-import first", tool, file)
				      : std::format("{}: {} is not a project {} reads", tool, file, tool));
			return 1;
		}

		projects.emplace_back(file);
	}

	if (projects.empty())
	{
		print_line(std::format("{}: no {} projects in {}", tool,
		                  mode == movie_batch_mode::import ? ".wlmp" : mode == movie_batch_mode::render ? ".otio" : ".wlmp or .otio",
		                  projects_folder));
		return 1;
	}

	// The collection is the user's own: its folders are the ones the app's settings name.
	setting.read();
	auto roots = index_folders(setting.collection);

	if (!options.media.empty())
	{
		if (!core::is_path(options.media) || !platform::exists(core::folder_path(options.media)))
		{
			print_line(std::format("{}: the media folder {} does not exist", tool, options.media));
			return 1;
		}

		roots.folders.emplace(core::folder_path(options.media));
	}

	source_search search(std::move(roots), setting.show_hidden);

	core::folder_path output_folder;

	if (!options.output.empty())
	{
		if (!core::is_path(options.output))
		{
			print_line(std::format("{}: give the full path of the output folder: {}", tool, options.output));
			return 1;
		}

		output_folder = core::folder_path(options.output);

		if (mode != movie_batch_mode::check && !platform::exists(output_folder))
		{
			const auto created = platform::create_folder(output_folder);

			if (created.failed())
			{
				print_line(std::format("{}: could not create {}: {}", tool, output_folder, created.format_error()));
				return 1;
			}
		}
	}

	// Absent rather than failing per project: no project could render, so none is attempted.
	if (mode == movie_batch_mode::render && !platform::can_write_movies())
	{
		print_line(std::format("{}: this system has no movie encoder, so nothing can be rendered", tool));
		return 1;
	}

	print_line(std::format("{}: {} project{}", tool, projects.size(), projects.size() == 1 ? "" : "s"));

	if (std::ranges::any_of(projects, [](const core::file_path p) { return has_extension(p, ".wlmp"); }))
	{
		for (const auto& folder : search.roots().folders) print_line(std::format("  search    {}", folder));
		if (search.roots().folders.empty()) print_line("  search    the collection names no folders");

		// Walked now rather than on the first missing source: a Movie Maker project's sources have
		// almost always moved, and the walk reads better here than inside one project's report.
		search.walk();
	}

	if (mode != movie_batch_mode::check)
	{
		print_line(std::format("  output    {}{}", output_folder.is_empty() ? "beside each project"s :
		                                       std::format("{}", output_folder),
		                  options.overwrite ? ", replacing what is there" : ""));
	}

	if (options.skip_missing && mode != movie_batch_mode::import)
	{
		print_line("  skipping  clips whose source cannot be used");
	}

	std::vector<core::file_path> written;
	std::map<project_outcome, size_t> outcomes;

	for (size_t i = 0; i < projects.size(); ++i)
	{
		print_line("");
		print_line(std::format("[{}/{}] {}", i + 1, projects.size(), projects[i]));

		auto outcome = project_outcome::failed;

		try
		{
			outcome = has_extension(projects[i], ".wlmp")
				          ? import_project(projects[i], options, search, output_folder, written)
				          : render_project(projects[i], options, output_folder, written);
		}
		catch (const std::exception& e)
		{
			// One broken project does not stop the batch: the reason is reported with the project
			// and counted in the exit code.
			core::log(__FUNCTION__, e.what());
			print_line(std::format("  failed    {}", e.what()));
		}
		catch (...)
		{
			core::log(__FUNCTION__, "movie batch project failed");
			print_line("  failed    an unexpected error stopped this project");
		}

		++outcomes[outcome];
	}

	print_line("");

	const auto summary = std::format("{} project{}", projects.size(), projects.size() == 1 ? "" : "s");
	const auto failed = outcomes[project_outcome::failed];

	if (mode == movie_batch_mode::import)
	{
		print_line(std::format("{}: {} imported ready to render, {} imported with sources missing, {} skipped, {} failed",
		                  summary, outcomes[project_outcome::imported], outcomes[project_outcome::imported_incomplete],
		                  outcomes[project_outcome::skipped], failed));
	}
	else if (mode == movie_batch_mode::render)
	{
		print_line(std::format("{}: {} rendered, {} skipped, {} failed", summary, outcomes[project_outcome::rendered],
		                  outcomes[project_outcome::skipped], failed));
	}
	else
	{
		print_line(std::format("{}: {} ready to render, {} not", summary, outcomes[project_outcome::ready], failed));
	}

	return failed == 0 && outcomes[project_outcome::imported_incomplete] == 0 ? 0 : 1;
}
