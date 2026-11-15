// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Implements the /movie-check and /movie-render command line options. Render makes a movie from
// each .dfv project with the Movie render; check reports what the render would find and writes nothing.
// Both report the project's lines that could not be read, its text screens and captions, which sources
// and sound files are missing or unreadable, and sound files their clips cut short, so an agent that wrote
// the project can see what it made. Never modifies a source or a project. docs/movie.md owns the rules.

#include "pch.h"
#include "app_command_line.h"
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

	// What the render can use, by the rule the Movie view's Render follows: a usable source, and a sound file
	// that plays, where the clip names one.
	bool has_usable_source(const movie_clip& clip)
	{
		if (clip.is_text) return clip.duration() > 0;
		return clip.is_probed && !clip.is_missing && clip.duration() > 0 && clip.extent.cx > 0 && clip.extent.cy > 0;
	}

	bool has_usable_sound(const movie_clip& clip)
	{
		return !clip.has_sound_file() || (clip.sound_file_is_probed && !clip.sound_file_is_missing);
	}

	bool is_usable(const movie_clip& clip)
	{
		return has_usable_source(clip) && has_usable_sound(clip);
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

	// A screen's words as one line of a report: its lines joined, and cut short where they run long.
	std::string report_text(const std::string_view text)
	{
		auto line = core::replace(text, "\n", " / ");
		constexpr size_t longest = 60;

		if (line.size() > longest)
		{
			auto end = longest;
			while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xC0u) == 0x80u) --end;
			line = line.substr(0, end) + "...";
		}

		return line;
	}

	// The text a movie shows: each text screen where it starts, and how many clips carry a caption. And how
	// many clips are marked to stabilize, which a render does not do yet.
	void print_text(const std::vector<movie_clip>& clips, const movie_timing& timing)
	{
		const auto screens = std::ranges::count_if(clips, [](const movie_clip& clip) { return clip.is_text; });
		const auto captions = std::ranges::count_if(clips, [](const movie_clip& clip)
		{ return !clip.is_text && !clip.text.empty(); });

		if (screens > 0 || captions > 0)
		{
			print_line(std::format("  text      {} screen{} and {} caption{}", screens, screens == 1 ? "" : "s",
			                       captions, captions == 1 ? "" : "s"));
		}

		size_t listed = 0;

		for (size_t i = 0; i < clips.size(); ++i)
		{
			if (!clips[i].is_text) continue;

			if (listed++ == max_listed_paths)
			{
				print_line(std::format("  screen    and {} more", screens - static_cast<ptrdiff_t>(max_listed_paths)));
				break;
			}

			print_line(std::format("  screen    {} \"{}\" for {:.1f} s", format_duration(timing.starts[i]),
			                       report_text(clips[i].text), clips[i].duration()));
		}

		if (const auto marked = std::ranges::count_if(clips, [](const movie_clip& clip) { return clip.stabilize; });
		    marked > 0)
		{
			print_line(std::format("  stabilize {} clip{} marked; the render does not stabilize yet", marked,
			                       marked == 1 ? " is" : "s are"));
		}
	}

	struct prepared_movie
	{
		std::vector<movie_clip> clips;
		movie_output output;
		double duration = 0;
		bool ready = false;
	};

	// What a render of these clips would make and whether it can. With `skip_missing`, a clip whose source
	// cannot be used is left out rather than refusing the movie, and the report says how many.
	prepared_movie prepare_movie(std::vector<movie_clip> clips, const core::file_path project_path,
	                             const bool skip_missing)
	{
		const auto is_sourced = [](const movie_clip& clip)
		{
			return !clip.is_text;
		};
		// A source that is there, whose part starts at or past its end.
		const auto is_empty = [](const movie_clip& clip)
		{
			return !clip.is_text && clip.is_probed && !clip.is_missing && clip.extent.cx > 0 && clip.extent.cy > 0 &&
			       clip.duration() <= 0;
		};

		// Of the clips that need a source: a text screen has none to lose.
		const auto sourced = std::ranges::count_if(clips, is_sourced);
		auto no_clip_left = false;

		if (skip_missing)
		{
			const auto before = clips.size();
			std::erase_if(clips, [](const movie_clip& clip) { return !is_usable(clip); });

			if (clips.size() < before)
			{
				print_line(std::format("  skipping  {} of {} clips with no usable source or sound file",
				                       before - clips.size(), sourced));
			}

			// The text screens of a project of clips are not the movie its writer meant.
			no_clip_left = sourced > 0 && std::ranges::none_of(clips, is_sourced);
		}

		movie_project project;
		project.reset(clips, project_path);

		prepared_movie result;
		result.output = project.output();
		const auto timing = project.timing();
		result.duration = timing.duration;
		result.ready = project.is_ready_to_render() && !no_clip_left;

		const auto count = [&clips](const auto& kind)
		{
			return std::ranges::count_if(clips, kind);
		};
		const auto photos = count([](const movie_clip& clip) { return clip.is_photo; });
		const auto audio = count([](const movie_clip& clip) { return clip.is_audio; });
		const auto screens = count([](const movie_clip& clip) { return clip.is_text; });

		print_line(std::format("  movie     {} videos, {} photos, {} audio files and {} text screens, {} long; "
		                       "{}x{} at {} fps, {:.1f} Mbit/s",
		                       std::ssize(clips) - photos - audio - screens, photos, audio, screens,
		                       format_duration(result.duration), result.output.extent.cx, result.output.extent.cy,
		                       result.output.frame_rate, result.output.video_bitrate / 1000000.0));
		print_text(clips, timing);

		if (!result.ready)
		{
			if (no_clip_left)
			{
				print_line("  not ready no clip has a usable source");
			}
			else if (clips.empty())
			{
				print_line("  not ready the project holds nothing to show");
			}
			else
			{
				const auto hint = skip_missing ? "" : "; -movie-skip-missing renders without them";
				const auto empty = count(is_empty);
				const auto unusable = count([](const movie_clip& c) { return !has_usable_source(c); }) - empty;
				const auto unheard = count([](const movie_clip& c)
				{ return has_usable_source(c) && !has_usable_sound(c); });

				if (unusable > 0)
				{
					print_line(
						std::format("  not ready {} of {} clips have no usable source{}", unusable, sourced, hint));
				}

				if (empty > 0)
				{
					print_line(std::format("  not ready {} of {} clips play nothing: their part starts at or past the "
					                       "end of their source{}",
					                       empty, sourced, hint));
				}

				if (unheard > 0)
				{
					print_line(std::format("  not ready {} of {} clips name a sound file that cannot be played{}",
					                       unheard, sourced, hint));
				}
			}
		}

		result.clips = std::move(clips);
		return result;
	}

	enum class project_outcome
	{
		ready,
		rendered,
		skipped,
		failed,
	};

	// Where this run writes `project_path`'s movie, refusing what would replace a file it should not: one
	// written earlier in this run, or one already there that nobody asked to replace.
	std::optional<core::file_path> choose_destination(const core::file_path project_path,
	                                                  const core::folder_path output_folder, const bool overwrite,
	                                                  const std::vector<core::file_path>& written, bool& skipped)
	{
		const auto folder = output_folder.is_empty() ? project_path.folder() : output_folder;
		const auto destination = folder.combine_file_ext(project_path.file_name_without_extension(), ".mp4");

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

	// One project: rendered, or checked as the render would find it.
	project_outcome render_project(const core::file_path project_path, const movie_batch_options& options,
	                               const core::folder_path output_folder, std::vector<core::file_path>& written)
	{
		std::optional<core::file_path> destination;

		if (options.mode != movie_batch_mode::check)
		{
			auto skipped = false;
			destination = choose_destination(project_path, output_folder, options.overwrite, written, skipped);
			if (!destination) return skipped ? project_outcome::skipped : project_outcome::failed;
		}

		const auto text = read_project_text(project_path);

		if (text.empty())
		{
			print_line("  failed    the project file could not be read");
			return project_outcome::failed;
		}

		const auto loaded = read_dfv(text, project_path.folder());
		print_line(std::format("  read      {} items", loaded.clips.size()));

		// A line that could not be read is a line of the movie its writer meant, so the movie is not made
		// without it: each is named, with its number, for whoever wrote it to put right.
		for (size_t i = 0; i < loaded.problems.size() && i < max_listed_paths; ++i)
		{
			const auto& problem = loaded.problems[i];
			print_line(std::format("  line {:<5}{}", problem.line, problem.reason));
		}

		if (loaded.problems.size() > max_listed_paths)
		{
			print_line(std::format("  line      and {} more", loaded.problems.size() - max_listed_paths));
		}

		if (!loaded || !loaded.problems.empty())
		{
			print_line(!loaded ? "  failed    this is not a project Movie can read"
			                   : std::format("  failed    {} line{} of the project could not be read",
			                                 loaded.problems.size(), loaded.problems.size() == 1 ? "" : "s"));
			return project_outcome::failed;
		}

		std::map<std::pair<core::file_path, int>, movie_probe_result> probes;
		std::vector<core::file_path> missing;
		std::vector<core::file_path> unreadable;
		auto clips = loaded.clips;

		for (auto& clip : clips)
		{
			if (clip.is_text) continue;

			// Another tune of a SID file is a source of its own, with a length of its own.
			const std::pair source{clip.path, clip.song};
			auto found = probes.find(source);

			if (found == probes.end())
			{
				const auto probe = probe_movie_source(clip.path, clip.song);

				if (!probe.found && !movie_path_list_contains(missing, clip.path) &&
				    !movie_path_list_contains(unreadable, clip.path))
				{
					(platform::exists(clip.path) ? unreadable : missing).emplace_back(clip.path);
				}

				found = probes.emplace(source, probe).first;

				if (probes.size() % probe_report_interval == 0)
				{
					print_line(std::format("  probing   {} sources", probes.size()));
				}
			}

			clip = apply_movie_probe(clip, found->second);
		}

		// Counted by file, whichever of a SID file's tunes the movie plays, as missing and unreadable are.
		std::set<core::file_path> files;
		for (const auto& [source, probe] : probes) files.insert(source.first);

		print_line(std::format("  sources   {} files: {} usable, {} missing, {} unreadable", files.size(),
		                       files.size() - missing.size() - unreadable.size(), missing.size(), unreadable.size()));
		print_paths("missing", missing);
		print_paths("unreadable", unreadable);

		// Each sound file once, and by the rule the view's probe follows.
		std::map<core::file_path, movie_sound_probe> sounds;
		std::vector<core::file_path> sounds_missing;
		std::vector<core::file_path> sounds_unreadable;

		for (auto& clip : clips)
		{
			if (!clip.has_sound_file()) continue;

			auto found = sounds.find(clip.sound_file);

			if (found == sounds.end())
			{
				const auto probe = probe_movie_sound(clip.sound_file);

				if (!probe.found)
				{
					(platform::exists(clip.sound_file) ? sounds_unreadable : sounds_missing)
						.emplace_back(clip.sound_file);
				}

				found = sounds.emplace(clip.sound_file, probe).first;
			}

			clip = apply_movie_sound_probe(clip, found->second);
		}

		if (!sounds.empty())
		{
			print_line(std::format("  sounds    {} files: {} usable, {} missing, {} unreadable", sounds.size(),
			                       sounds.size() - sounds_missing.size() - sounds_unreadable.size(),
			                       sounds_missing.size(), sounds_unreadable.size()));
			print_paths("missing", sounds_missing);
			print_paths("unreadable", sounds_unreadable);
		}

		// A part that starts at or past the end of its source plays nothing. Named with where it starts,
		// as the project wrote it, so whoever wrote the line can put it right.
		for (size_t i = 0; i < clips.size(); ++i)
		{
			const auto& clip = clips[i];

			if (!clip.is_text && clip.is_probed && !clip.is_missing && clip.duration() <= 0)
			{
				print_line(std::format("  empty     {} from {:.3f} s, but it ends at {:.3f} s", clip.path,
				                       loaded.clips[i].start, clip.source_duration));
			}

			// A sound file plays only while its clip shows, so a longer one is cut where the clip ends. Named,
			// since words cut off are rarely what the writer meant.
			if (has_usable_source(clip) && has_usable_sound(clip) && clip.has_sound_file() &&
			    clip.sound_file_duration > clip.duration() + 0.05)
			{
				print_line(std::format("  cut       {} plays {:.2f} s, but its clip shows {:.2f} s", clip.sound_file,
				                       clip.sound_file_duration, clip.duration()));
			}
		}

		auto movie = prepare_movie(std::move(clips), project_path, options.skip_missing);
		if (!movie.ready) return project_outcome::failed;

		if (options.mode == movie_batch_mode::check)
		{
			print_line("  ready     every clip has a usable source");
			return project_outcome::ready;
		}

		movie_render_request request;
		request.clips = std::move(movie.clips);
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
	const auto folder = options.output.empty() ? core::folder_path{} : core::folder_path(options.output);
	return render_project(project, options, folder, written) == project_outcome::skipped;
}

bool movie_batch_test_seams::project_is_ready(const core::file_path project, const bool skip_missing)
{
	std::vector<core::file_path> written;
	movie_batch_options options;
	options.mode = movie_batch_mode::check;
	options.skip_missing = skip_missing;
	return render_project(project, options, {}, written) == project_outcome::ready;
}

int run_movie_batch(const movie_batch_options& options)
{
	load_file_types();

	const auto mode = options.mode;
	const auto* const tool = mode == movie_batch_mode::render ? "movie-render" : "movie-check";

	if (!core::is_path(options.projects))
	{
		print_line(
			std::format("{}: give the full path of a project or of a folder of projects: {}", tool, options.projects));
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
			if (has_extension(path, ".dfv")) projects.emplace_back(path);
		}

		std::ranges::sort(projects, [](const core::file_path l, const core::file_path r)
		{ return core::icmp(l.name(), r.name()) < 0; });
	}
	else
	{
		const auto file = core::file_path(options.projects);

		if (!platform::exists(file))
		{
			print_line(std::format("{}: no project or folder at {}", tool, options.projects));
			return 1;
		}

		if (!has_extension(file, ".dfv"))
		{
			print_line(std::format("{}: {} is not a .dfv project", tool, file));
			return 1;
		}

		projects.emplace_back(file);
	}

	if (projects.empty())
	{
		print_line(std::format("{}: no .dfv projects in {}", tool, projects_folder));
		return 1;
	}

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

	if (mode != movie_batch_mode::check)
	{
		print_line(std::format("  output    {}{}",
		                       output_folder.is_empty() ? "beside each project"s : std::format("{}", output_folder),
		                       options.overwrite ? ", replacing what is there" : ""));
	}

	if (options.skip_missing) print_line("  skipping  clips whose source cannot be used");

	std::vector<core::file_path> written;
	std::map<project_outcome, size_t> outcomes;

	for (size_t i = 0; i < projects.size(); ++i)
	{
		print_line("");
		print_line(std::format("[{}/{}] {}", i + 1, projects.size(), projects[i]));

		auto outcome = project_outcome::failed;

		try
		{
			outcome = render_project(projects[i], options, output_folder, written);
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

	if (mode == movie_batch_mode::render)
	{
		print_line(std::format("{}: {} rendered, {} skipped, {} failed", summary, outcomes[project_outcome::rendered],
		                       outcomes[project_outcome::skipped], failed));
	}
	else
	{
		print_line(std::format("{}: {} ready to render, {} not", summary, outcomes[project_outcome::ready], failed));
	}

	return failed == 0 ? 0 : 1;
}
