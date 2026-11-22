// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Checks and renders .dfv Movie projects without the window (app_movie_batch.h). Render makes a movie from
// each project with the Movie render; check reports what the render would find and writes nothing. Both report the
// project's lines that could not be read, its text screens and captions, which sources and sound files are missing
// or unreadable, and sound files their clips cut short - as lines, and as a report a tool returns - so an agent that
// wrote the project can see what it made. Never modifies a source or a project. docs/movie.md owns the rules.

#include "pch.h"
#include "app_movie_batch.h"
#include "app_text.h"
#include "files.h"
#include "model_movie.h"
#include "libdesktop/platform.h"
#include "view_movie.h"

#include <chrono>
#include <cstdio>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace
{
	// A project file is a document, not a stream. Mirrors the limit the view's Open applies.
	constexpr uint64_t max_project_bytes = 32ull * 1024ull * 1024ull;

	// Past a screenful a list stops being feedback, so longer ones are counted rather than printed.
	constexpr size_t max_listed_paths = 25;

	// Probing a large project takes long enough that silence would read as a hang.
	constexpr size_t probe_report_interval = 250;

	using batch_clock = std::chrono::steady_clock;

	// Where this thread's batch reports, and the project it is reporting on. A batch runs on one thread.
	thread_local const std::function<void(std::string_view)>* t_print = nullptr;
	thread_local movie_project_report* t_report = nullptr;

	double seconds_since(const batch_clock::time_point started)
	{
		return std::chrono::duration<double>(batch_clock::now() - started).count();
	}

	// Whole lines only, flushed: the usual reader is a pipe into PowerShell, which shows nothing of a
	// line until it ends.
	void print_line(const std::string_view text)
	{
		if (t_print && *t_print)
		{
			(*t_print)(text);
			return;
		}

		printf("%.*s\n", static_cast<int>(text.size()), text.data());
		fflush(stdout);
	}

	// The project being reported on, or one nobody reads.
	movie_project_report& report()
	{
		thread_local movie_project_report unread;
		return t_report ? *t_report : unread;
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
		report().captions = static_cast<size_t>(captions);

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
			report().stabilize = static_cast<size_t>(marked);
			print_line(std::format("  stabilize {} clip{} marked; the render does not stabilize yet", marked,
			                       marked == 1 ? " is" : "s are"));
		}
	}

	struct prepared_movie
	{
		std::vector<movie_clip> clips;
		movie_output output;
		movie_kind kind = movie_kind::movie;
		double duration = 0;
		bool ready = false;
	};

	// What a render of these clips would make and whether it can. With `skip_missing`, a clip whose source
	// cannot be used is left out rather than refusing the movie, and the report says how many.
	prepared_movie prepare_movie(std::vector<movie_clip> clips, const core::file_path project_path,
	                             const movie_kind kind, const bool skip_missing)
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
				const auto skipped = std::format("skipping {} of {} clips with no usable source or sound file",
				                                 before - clips.size(), sourced);
				report().warnings.push_back(skipped);
				print_line(std::format("  {}", skipped));
			}

			// The text screens of a project of clips are not the movie its writer meant.
			no_clip_left = sourced > 0 && std::ranges::none_of(clips, is_sourced);
		}

		movie_project project;
		project.reset(clips, project_path, kind);

		prepared_movie result;
		result.output = project.output();
		result.kind = kind;
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

		auto& r = report();
		r.videos = static_cast<size_t>(std::ssize(clips) - photos - audio - screens);
		r.photos = static_cast<size_t>(photos);
		r.audio = static_cast<size_t>(audio);
		r.text_screens = static_cast<size_t>(screens);
		r.seconds = result.duration;
		r.size = result.output.extent;
		r.frame_rate = result.output.frame_rate;
		r.youtube_short = kind == movie_kind::youtube_short;

		print_line(std::format("  movie     {} videos, {} photos, {} audio files and {} text screens, {} long; "
		                       "{}x{} at {} fps, {:.1f} Mbit/s{}",
		                       std::ssize(clips) - photos - audio - screens, photos, audio, screens,
		                       format_duration(result.duration), result.output.extent.cx, result.output.extent.cy,
		                       result.output.frame_rate, result.output.video_bitrate / 1000000.0,
		                       r.youtube_short ? "; a YouTube Short" : ""));

		// A Short ends by three minutes: one whose items run longer is cut there, and the report says so.
		if (timing.cut)
		{
			const auto cut = std::format("its items run {}, so the Short fades out at {}",
			                             format_duration(timing.whole), format_duration(timing.duration));
			r.warnings.push_back("cut short: " + cut);
			print_line(std::format("  cut       {}", cut));
		}

		print_text(clips, timing);

		// Each reason the movie is not ready, as a line of the report and as a reason the tool gives.
		const auto not_ready = [&r](const std::string& reason)
		{
			r.reasons.push_back(reason);
			print_line(std::format("  not ready {}", reason));
		};

		if (!result.ready)
		{
			if (no_clip_left)
			{
				not_ready("no clip has a usable source");
			}
			else if (clips.empty())
			{
				not_ready("the project holds nothing to show");
			}
			else
			{
				const auto hint = skip_missing ? "" : "; skip_missing renders without them";
				const auto empty = count(is_empty);
				const auto unusable = count([](const movie_clip& c) { return !has_usable_source(c); }) - empty;
				const auto unheard = count([](const movie_clip& c)
				{ return has_usable_source(c) && !has_usable_sound(c); });

				if (unusable > 0)
				{
					not_ready(std::format("{} of {} clips have no usable source{}", unusable, sourced, hint));
				}

				if (empty > 0)
				{
					not_ready(std::format("{} of {} clips play nothing: their part starts at or past the end of their "
					                      "source{}",
					                      empty, sourced, hint));
				}

				if (unheard > 0)
				{
					not_ready(std::format("{} of {} clips name a sound file that cannot be played{}", unheard, sourced,
					                      hint));
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
		report().movie = destination;

		if (std::ranges::any_of(written, [destination](const core::file_path path) { return path == destination; }))
		{
			report().reasons.push_back(std::format("{} was written by an earlier project in this run", destination));
			print_line(std::format("  skipped   {} was written by an earlier project in this run", destination));
			return std::nullopt;
		}

		if (platform::exists(destination) && !overwrite)
		{
			report().reasons.push_back(std::format("{} exists; overwrite replaces it", destination));
			print_line(std::format("  skipped   {} exists; overwrite replaces it", destination));
			skipped = true;
			return std::nullopt;
		}

		return destination;
	}

	// One project: rendered, or checked as the render would find it.
	project_outcome render_project(const core::file_path project_path, const movie_batch_options& options,
	                               const core::folder_path output_folder, std::vector<core::file_path>& written,
	                               const core::cancel_token& cancel = {},
	                               const std::function<void(double)>& progress = {})
	{
		std::optional<core::file_path> destination;

		if (options.mode != movie_batch_mode::check)
		{
			auto skipped = false;
			destination = choose_destination(project_path, output_folder, options.overwrite, written, skipped);
			if (!destination) return skipped ? project_outcome::skipped : project_outcome::failed;
		}

		const auto failed = [](const std::string& reason)
		{
			report().reasons.push_back(reason);
			print_line(std::format("  failed    {}", reason));
			return project_outcome::failed;
		};

		const auto text = read_project_text(project_path);
		if (text.empty()) return failed("the project file could not be read");

		const auto loaded = read_dfv(text, project_path.folder());
		print_line(std::format("  read      {} items", loaded.clips.size()));

		// A line that could not be read is a line of the movie its writer meant, so the movie is not made
		// without it: each is named, with its number, for whoever wrote it to put right.
		for (size_t i = 0; i < loaded.problems.size(); ++i)
		{
			const auto& problem = loaded.problems[i];
			report().unreadable_lines.push_back({static_cast<int>(problem.line), std::string(problem.reason)});
			if (i < max_listed_paths) print_line(std::format("  line {:<5}{}", problem.line, problem.reason));
		}

		if (loaded.problems.size() > max_listed_paths)
		{
			print_line(std::format("  line      and {} more", loaded.problems.size() - max_listed_paths));
		}

		if (!loaded || !loaded.problems.empty())
		{
			return failed(!loaded ? "this is not a project Movie can read"s
			                      : std::format("{} line{} of the project could not be read", loaded.problems.size(),
			                                    loaded.problems.size() == 1 ? "" : "s"));
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
		report().missing = missing;
		report().unreadable = unreadable;

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
			report().sounds_missing = sounds_missing;
			report().sounds_unreadable = sounds_unreadable;
		}

		// A part that starts at or past the end of its source plays nothing. Named with where it starts,
		// as the project wrote it, so whoever wrote the line can put it right.
		for (size_t i = 0; i < clips.size(); ++i)
		{
			const auto& clip = clips[i];

			if (!clip.is_text && clip.is_probed && !clip.is_missing && clip.duration() <= 0)
			{
				const auto empty = std::format("{} from {:.3f} s, but it ends at {:.3f} s", clip.path,
				                               loaded.clips[i].start, clip.source_duration);
				report().warnings.push_back("plays nothing: " + empty);
				print_line(std::format("  empty     {}", empty));
			}

			// A sound file plays only while its clip shows, so a longer one is cut where the clip ends. Named,
			// since words cut off are rarely what the writer meant.
			if (has_usable_source(clip) && has_usable_sound(clip) && clip.has_sound_file() &&
			    clip.sound_file_duration > clip.duration() + 0.05)
			{
				const auto cut = std::format("{} plays {:.2f} s, but its clip shows {:.2f} s", clip.sound_file,
				                             clip.sound_file_duration, clip.duration());
				report().warnings.push_back("cut short: " + cut);
				print_line(std::format("  cut       {}", cut));
			}
		}

		auto movie = prepare_movie(std::move(clips), project_path, loaded.kind, options.skip_missing);
		if (!movie.ready) return project_outcome::failed;

		if (options.mode == movie_batch_mode::check)
		{
			print_line("  ready     every clip has a usable source");
			return project_outcome::ready;
		}

		movie_render_request request;
		request.clips = std::move(movie.clips);
		request.output = movie.output;
		request.kind = movie.kind;
		request.path = *destination;
		request.fail_if_exists = !options.overwrite;

		const auto control = std::make_shared<movie_render_control>();
		const auto token = core::cancel_token(control->cancelled);
		const auto started = batch_clock::now();
		auto reported = 0;

		print_line(std::format("  render    {}", *destination));

		const auto on_progress = [&reported, started, &cancel, &progress, control](const double fraction)
		{
			// Asked to stop from outside: the render abandons its writer, and publishes nothing.
			if (cancel.is_cancelled()) control->cancel();
			if (progress) progress(fraction);

			const auto tenth = static_cast<int>(fraction * 10.0);
			if (tenth <= reported || tenth >= 10) return;

			reported = tenth;
			print_line(std::format("            {}% after {}", tenth * 10, format_duration(seconds_since(started))));
		};

		const auto result = render_movie_to_file(request, on_progress, token, control);

		if (!result.ok)
		{
			const auto reason = cancel.is_cancelled() ? "the render was cancelled"s : render_failure(result);
			failed(reason);

			if (!result.source.is_empty())
			{
				const auto frame = std::format("no frame from {} at {:.2f} s", result.source, result.source_time);
				report().reasons.push_back(frame);
				print_line(std::format("            {}", frame));
			}

			return project_outcome::failed;
		}

		written.emplace_back(*destination);
		report().movie = *destination;

		const auto took = seconds_since(started);
		print_line(std::format("  rendered  {} of movie in {} ({:.1f}x real time)", format_duration(movie.duration),
		                       format_duration(took), took > 0 ? movie.duration / took : 0.0));
		return project_outcome::rendered;
	}

	void write_paths(rapidjson::Writer<rapidjson::StringBuffer>& w, const char* name,
	                 const std::vector<core::file_path>& paths)
	{
		w.Key(name);
		w.StartArray();

		for (const auto& path : paths)
		{
			const auto text = path.str();
			w.String(text.c_str(), static_cast<rapidjson::SizeType>(text.size()));
		}

		w.EndArray();
	}

	void write_text(rapidjson::Writer<rapidjson::StringBuffer>& w, const char* name, const std::string_view text)
	{
		w.Key(name);
		w.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
	}
}

std::string format_movie_batch_report(const movie_batch_report& report)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
	w.StartObject();
	w.Key("succeeded");
	w.Bool(report.succeeded);
	if (!report.failure.empty()) write_text(w, "failure", report.failure);
	w.Key("projects");
	w.StartArray();

	for (const auto& p : report.projects)
	{
		w.StartObject();
		write_text(w, "project", p.project.str());
		write_text(w, "outcome", p.outcome);
		w.Key("reasons");
		w.StartArray();
		for (const auto& r : p.reasons) w.String(r.c_str(), static_cast<rapidjson::SizeType>(r.size()));
		w.EndArray();
		w.Key("unreadable_lines");
		w.StartArray();

		for (const auto& l : p.unreadable_lines)
		{
			w.StartObject();
			w.Key("line");
			w.Int(l.line);
			write_text(w, "reason", l.reason);
			w.EndObject();
		}

		w.EndArray();
		w.Key("videos");
		w.Uint64(p.videos);
		w.Key("photos");
		w.Uint64(p.photos);
		w.Key("audio");
		w.Uint64(p.audio);
		w.Key("text_screens");
		w.Uint64(p.text_screens);
		w.Key("captions");
		w.Uint64(p.captions);
		w.Key("stabilize");
		w.Uint64(p.stabilize);
		w.Key("youtube_short");
		w.Bool(p.youtube_short);
		w.Key("seconds");
		w.Double(p.seconds);
		w.Key("width");
		w.Int(p.size.cx);
		w.Key("height");
		w.Int(p.size.cy);
		w.Key("frame_rate");
		w.Int(p.frame_rate);
		write_paths(w, "missing", p.missing);
		write_paths(w, "unreadable", p.unreadable);
		write_paths(w, "sounds_missing", p.sounds_missing);
		write_paths(w, "sounds_unreadable", p.sounds_unreadable);
		w.Key("warnings");
		w.StartArray();
		for (const auto& r : p.warnings) w.String(r.c_str(), static_cast<rapidjson::SizeType>(r.size()));
		w.EndArray();
		if (!p.movie.is_empty()) write_text(w, "movie", p.movie.str());
		w.EndObject();
	}

	w.EndArray();
	w.EndObject();
	return {buffer.GetString(), buffer.GetSize()};
}

bool movie_batch_test_seams::project_is_skipped(const core::file_path project, const movie_batch_options& options)
{
	movie_project_report r;
	t_report = &r;
	const core::scope_exit unset([] { t_report = nullptr; });
	std::vector<core::file_path> written;
	const auto folder = options.output.empty() ? core::folder_path{} : core::folder_path(options.output);
	return render_project(project, options, folder, written) == project_outcome::skipped;
}

bool movie_batch_test_seams::project_is_ready(const core::file_path project, const bool skip_missing)
{
	movie_project_report r;
	t_report = &r;
	const core::scope_exit unset([] { t_report = nullptr; });
	std::vector<core::file_path> written;
	movie_batch_options options;
	options.mode = movie_batch_mode::check;
	options.skip_missing = skip_missing;
	return render_project(project, options, {}, written) == project_outcome::ready;
}

movie_batch_report run_movie_batch(const movie_batch_options& options,
                                   const std::function<void(std::string_view)>& print, const core::cancel_token& cancel,
                                   const std::function<void(double)>& progress)
{
	t_print = &print;
	const core::scope_exit unset([] { t_print = nullptr; });

	load_file_types();

	movie_batch_report result;
	const auto mode = options.mode;
	const auto* const tool = mode == movie_batch_mode::render ? "movie_render" : "movie_check";

	const auto refuse = [&result, tool](const std::string& failure)
	{
		result.failure = failure;
		print_line(std::format("{}: {}", tool, failure));
		return result;
	};

	if (!core::is_path(options.projects))
	{
		return refuse(std::format("give the full path of a project or of a folder of projects: {}", options.projects));
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

		if (!platform::exists(file)) return refuse(std::format("no project or folder at {}", options.projects));
		if (!has_extension(file, ".dfv")) return refuse(std::format("{} is not a .dfv project", file));

		projects.emplace_back(file);
	}

	if (projects.empty()) return refuse(std::format("no .dfv projects in {}", projects_folder));

	core::folder_path output_folder;

	if (!options.output.empty())
	{
		if (!core::is_path(options.output))
		{
			return refuse(std::format("give the full path of the output folder: {}", options.output));
		}

		output_folder = core::folder_path(options.output);

		if (mode != movie_batch_mode::check && !platform::exists(output_folder))
		{
			const auto created = platform::create_folder(output_folder);

			if (created.failed())
			{
				return refuse(std::format("could not create {}: {}", output_folder, created.format_error()));
			}
		}
	}

	// Absent rather than failing per project: no project could render, so none is attempted.
	if (mode == movie_batch_mode::render && !platform::can_write_movies())
	{
		return refuse("this system has no movie encoder, so nothing can be rendered");
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
	const auto count = static_cast<double>(projects.size());

	for (size_t i = 0; i < projects.size(); ++i)
	{
		print_line("");
		print_line(std::format("[{}/{}] {}", i + 1, projects.size(), projects[i]));

		auto& project_report = result.projects.emplace_back();
		project_report.project = projects[i];
		t_report = &project_report;
		const core::scope_exit unreport([] { t_report = nullptr; });

		auto outcome = project_outcome::failed;

		if (cancel.is_cancelled())
		{
			project_report.reasons.emplace_back("the batch was cancelled before this project");
		}
		else
		{
			try
			{
				const auto step = [&progress, i, count](const double fraction)
				{
					if (progress) progress((static_cast<double>(i) + std::clamp(fraction, 0.0, 1.0)) / count);
				};

				outcome = render_project(projects[i], options, output_folder, written, cancel, step);
			}
			catch (const std::exception& e)
			{
				// One broken project does not stop the batch: the reason is reported with the project
				// and counted in the exit code.
				core::log(__FUNCTION__, e.what());
				project_report.reasons.emplace_back(e.what());
				print_line(std::format("  failed    {}", e.what()));
			}
			catch (...)
			{
				core::log(__FUNCTION__, "movie batch project failed");
				project_report.reasons.emplace_back("an unexpected error stopped this project");
				print_line("  failed    an unexpected error stopped this project");
			}
		}

		project_report.outcome = outcome == project_outcome::ready      ? "ready"
		                         : outcome == project_outcome::rendered ? "rendered"
		                         : outcome == project_outcome::skipped  ? "skipped"
		                                                                : "failed";
		++outcomes[outcome];
		if (progress) progress(static_cast<double>(i + 1) / count);
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

	result.succeeded = failed == 0;
	return result;
}
