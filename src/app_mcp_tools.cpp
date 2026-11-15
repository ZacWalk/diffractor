// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The agent tools over the engine and the library (app_mcp.h): Movie projects checked and rendered, the
// developer's tools - the test suite, the translations, the documentation and the duplicate report - and the
// library's search, its items, and places for those taken without one from those taken nearest them in time. Long work
// runs on the agent's own thread in a session, or here and now when -tool: calls it; the test suite and the
// translations run in a copy of the program, whose output they report.

#include "pch.h"

#include "app_mcp.h"
#include "app_mcp_json.h"
#include "app_movie_batch.h"
#include "app_dup_report.h"
#include "model_db.h"
#include "model_index.h"
#include "model_locations.h"
#include "model.h"
#include "ui_dialog.h"
#include "app_command_line.h"
#include "app.h"

#include <cstdio>

int generate_wiki_docs(std::string_view output_folder, const std::function<void(std::string_view)>& print);

namespace
{
	using print_fn = std::function<void(std::string_view)>;
	using progress_fn = std::function<void(double)>;
	using work_fn = std::function<core::mcp::tool_result(const print_fn& print, const progress_fn& progress,
	                                                     const core::cancel_token& cancel)>;

	void print_to_stderr(const std::string_view line)
	{
		std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
		std::fflush(stderr);
	}

	// In a session, work runs on the agent's thread as the call's job, which keeps what it prints; called once from
	// the command line, it runs here, printing to stderr.
	void run_work(mcp_host& host, const core::mcp::call_ptr& call, const std::string_view tool, work_fn work)
	{
		if (host.is_console())
		{
			call->finish(work(print_to_stderr, {}, call->cancel_token()));
			return;
		}

		const auto job = host.start_job(tool);
		const auto cancelled = std::make_shared<std::atomic_bool>(false);
		job->on_cancel([cancelled] { *cancelled = true; });
		job->progress(-1.0, "waiting for the agent's thread");

		host.app().queue_async(async_queue::agent, [job, cancelled, work = std::move(work)]
		{
			const print_fn print = [&job](const std::string_view line)
			{
				job->output(line);
			};
			const progress_fn progress = [&job](const double fraction)
			{
				job->progress(fraction, {});
			};
			job->progress(-1.0, job->tool());
			job->complete(work(print, progress, core::cancel_token(*cancelled)));
		});

		host.wait(call, job, mcp_host::wait_seconds(*call), false);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Movie
	///////////////////////////////////////////////////////////////////////////////////////////////////

	std::string movie_summary(const std::string_view tool, const movie_batch_report& report)
	{
		if (!report.failure.empty()) return std::format("{}: {}", tool, report.failure);

		std::map<std::string, int> outcomes;
		for (const auto& project : report.projects) ++outcomes[project.outcome];

		std::string counts;

		for (const auto& [outcome, count] : outcomes)
		{
			if (!counts.empty()) counts += ", ";
			counts += std::format("{} {}", count, outcome);
		}

		return std::format("{}: {} project{} - {}", tool, report.projects.size(),
		                   report.projects.size() == 1 ? "" : "s", counts);
	}

	void run_movie(mcp_host& host, const core::mcp::call_ptr& call, const std::string_view tool,
	               movie_batch_options options)
	{
		run_work(host, call, tool,
		         [tool = std::string(tool), options = std::move(options)](const print_fn& print,
		                                                                  const progress_fn& progress,
		                                                                  const core::cancel_token& cancel)
		{
			std::string printed;
			const auto report = run_movie_batch(options, [&print, &printed](const std::string_view line)
			{
				print(line);
				printed += line;
				printed += '\n';
			}, cancel, progress);

			return structured_result(std::format("{}\n\n{}", movie_summary(tool, report), printed),
			                         format_movie_batch_report(report), !report.succeeded);
		});
	}

	constexpr std::string_view wait_property =
		R"("wait_s":{"type":"number","minimum":0,"maximum":600,"description":"How long to wait for the answer before replying with a job to wait on; 50 by default."})";

	void add_movie_check(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "movie_check";
		t.title = "Check Movie projects";
		t.description = "Checks Movie projects (.dfv) without rendering them: each line that cannot be read, each "
						"source that is missing or unreadable, what the movie would hold and how long it would run, "
						"and whether it is ready to render. projects is a .dfv file or a folder of them. help with "
						"topic dfv explains the format.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"projects":{{"type":"string","description":"The full path of a .dfv project, or of a folder of them"}},"skip_missing":{{"type":"boolean","description":"Leave out clips whose source cannot be used, rather than refusing the project"}},{}}},"required":["projects"],"additionalProperties":false}})",
			wait_property);
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			movie_batch_options options;
			options.projects = call->text("projects");
			options.skip_missing = call->flag("skip_missing", false);
			options.mode = movie_batch_mode::check;
			run_movie(host, call, "movie_check", std::move(options));
		};
		host.add(std::move(t), tool_tier::read, false);
	}

	void add_movie_render(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "movie_render";
		t.title = "Render Movie projects";
		t.description =
			"Renders Movie projects (.dfv) to MP4, as the Movie view's Render does: each project is checked "
			"first, and one that is not ready is not rendered. Each movie goes beside its project unless "
			"output names a folder; one already there is kept unless overwrite. A long render answers with "
			"a job: job_wait follows it, job_cancel stops it without leaving a movie behind.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"projects":{{"type":"string","description":"The full path of a .dfv project, or of a folder of them"}},"output":{{"type":"string","description":"The full path of the folder the movies go to"}},"overwrite":{{"type":"boolean"}},"skip_missing":{{"type":"boolean","description":"Leave out clips whose source cannot be used, rather than refusing the project"}},{}}},"required":["projects"],"additionalProperties":false}})",
			wait_property);
		t.annotations.destructive = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			movie_batch_options options;
			options.projects = call->text("projects");
			options.output = call->text("output");
			options.overwrite = call->flag("overwrite", false);
			options.skip_missing = call->flag("skip_missing", false);
			options.mode = movie_batch_mode::render;
			run_movie(host, call, "movie_render", std::move(options));
		};
		host.add(std::move(t), tool_tier::write, false);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// The developer's tools
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// The test runner's report: a line a test, "  PASS  <name> (<n>ms)" or "  FAIL  <name> (<n>ms)" with why on the
	// lines indented beneath, then "Results: <p> passed, <f> failed, <t> total".
	core::mcp::tool_result test_report(const int exit_code, const std::string& out, const agent_job& job)
	{
		struct failure
		{
			std::string test;
			std::string message;
		};

		std::vector<failure> failures;
		auto passed = 0;
		auto failed = 0;
		auto total = 0;
		auto results_seen = false;

		for (const auto line : core::split(out, false, [](const char c) { return c == '\n'; }))
		{
			const auto text = core::trim(line);

			if (line.starts_with("  PASS"))
			{
				++passed;
			}
			else if (line.starts_with("  FAIL"))
			{
				++failed;
				auto name = core::trim(text.substr(4));
				if (const auto timing = name.rfind(" ("); timing != std::string_view::npos)
					name = name.substr(0, timing);
				failures.push_back({std::string(name), {}});
			}
			else if (line.starts_with("        ") && !failures.empty() && !text.empty())
			{
				auto& message = failures.back().message;
				if (!message.empty()) message += '\n';
				message += text;
			}
			else if (text.starts_with("Results: "))
			{
				results_seen = true;
				const auto parts = core::split(text.substr(9), false, [](const char c) { return c == ','; });
				if (parts.size() >= 3)
				{
					passed = parse_int(core::trim(parts[0]));
					failed = parse_int(core::trim(parts[1]));
					total = parse_int(core::trim(parts[2]));
				}
			}
		}

		if (!results_seen)
		{
			return core::mcp::tool_result::error(
				std::format("The test run ended with code {} before it reported its results:\n{}", exit_code,
			                job.recent_output(40) + out.substr(out.size() > 4000 ? out.size() - 4000 : 0)));
		}

		json_writer json;
		json.object().integer("passed", passed).integer("failed", failed).integer("total", total);
		json.integer("exit_code", exit_code).number("seconds", job.seconds());
		json.key("failures").array();
		for (const auto& f : failures) json.object().text("test", f.test).text("message", f.message).end_object();
		json.end_array().end_object();

		auto text = std::format("{} passed, {} failed, {} total, in {:.0f} s", passed, failed, total, job.seconds());
		for (const auto& f : failures) text += std::format("\n\nFAIL {}\n{}", f.test, f.message);
		return structured_result(std::move(text), json.str(), failed > 0 || exit_code != 0);
	}

	void add_run_tests(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "run_tests";
		t.title = "Run the test suite";
		t.description = "Runs Diffractor's own tests in a copy of the program - all of them, or those whose names "
						"match filter, a wildcard such as movie* - and reports how many passed, and why each failure "
						"failed. The whole suite takes minutes: it answers with a job to wait on.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"filter":{{"type":"string","description":"Which tests: a name, or a wildcard; all of them by default"}},{}}},"additionalProperties":false}})",
			wait_property);
		t.annotations.read_only = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			const auto filter = call->text("filter", "*");
			host.run_child(call, "run_tests", {std::format("-test:{}", filter)}, test_report);
		};
		host.add(std::move(t), tool_tier::dev, false);
	}

	void add_validate_translations(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "validate_translations";
		t.title = "Check the translations";
		t.description = "Checks every translation (languages/*.po) against the strings Diffractor shows: entries "
						"missing, left over, or with placeholders that do not match.";
		t.input_schema = std::format(R"({{"type":"object","properties":{{{}}},"additionalProperties":false}})",
		                             wait_property);
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			host.run_child(call, "validate_translations", {"-validate-po"},
			               [](const int exit_code, const std::string& out, const agent_job&)
			{
				json_writer json;
				json.object().boolean("ok", exit_code == 0).integer("exit_code", exit_code).end_object();
				return structured_result(out.empty() ? std::format("Ended with code {}", exit_code) : out, json.str(),
				                         exit_code != 0);
			});
		};
		host.add(std::move(t), tool_tier::dev, false);
	}

	void add_generate_docs(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "generate_docs";
		t.title = "Generate the wiki pages";
		t.description = "Writes the wiki pages generated from Diffractor's own tables - the formats and codecs it "
						"reads - into folder, the program's folder by default.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"folder":{{"type":"string","description":"The full path of the folder to write to"}},{}}},"additionalProperties":false}})",
			wait_property);
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			run_work(host, call, "generate_docs",
			         [folder = call->text("folder")](const print_fn& print, const progress_fn&,
			                                         const core::cancel_token&)
			{
				std::string printed;
				const auto code = generate_wiki_docs(folder, [&print, &printed](const std::string_view line)
				{
					print(line);
					printed += line;
					printed += '\n';
				});
				return code == 0 ? core::mcp::tool_result::text(printed) : core::mcp::tool_result::error(printed);
			});
		};
		host.add(std::move(t), tool_tier::dev, false);
	}

	void add_duplicate_report(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "duplicate_report";
		t.title = "Measure duplicate grouping";
		t.description = "Measures what grouping photos by their perceptual hash finds in a real library folder - "
						"the sets it makes and the capture times it declines - without changing anything. Writes the "
						"report to output as well, where a file is named.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"folder":{{"type":"string","description":"The full path of the library folder"}},"output":{{"type":"string","description":"The full path of a file to write the report to"}},{}}},"required":["folder"],"additionalProperties":false}})",
			wait_property);
		t.annotations.read_only = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			run_work(host, call, "duplicate_report",
			         [folder = call->text("folder"), output = call->text("output")](const print_fn& print,
			                                                                        const progress_fn&,
			                                                                        const core::cancel_token&)
			{
				std::string printed;
				const auto code = run_duplicate_report(folder, output, [&print, &printed](const std::string_view line)
				{
					print(line);
					printed += line;
					printed += '\n';
				});
				return code == 0 ? core::mcp::tool_result::text(printed) : core::mcp::tool_result::error(printed);
			});
		};
		host.add(std::move(t), tool_tier::dev, false);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// The library
	///////////////////////////////////////////////////////////////////////////////////////////////////

	std::string size_text(const uint64_t bytes)
	{
		const auto r = core::round_size(bytes);
		return r.dec ? std::format("{}.{} {}", r.n, r.dec, r.short_unit) : std::format("{} {}", r.n, r.short_unit);
	}

	// One line of what a file is: its kind, dimensions, length, size and date.
	std::string file_summary(const core::file_path path, const file_type_ref ft, const uint64_t bytes,
	                         const prop::item_metadata* md)
	{
		auto text = std::format("{}: {}", path, ft && ft->group ? std::string(ft->group->name) : "file"s);
		if (md && md->width != 0 && md->height != 0) text += std::format(", {}x{}", md->width, md->height);
		if (md && md->duration != 0) text += std::format(", {} s", md->duration);
		text += ", " + size_text(bytes);
		if (md && md->created().is_valid()) text += ", created " + md->created().to_xmp_date();
		return text;
	}

	void write_date(json_writer& json, const std::string_view key, const core::date_t date)
	{
		if (date.is_valid()) json.text(key, date.to_xmp_date());
	}

	void write_text(json_writer& json, const std::string_view key, const core::cached value)
	{
		if (!value.is_empty()) json.text(key, value.sv());
	}

	// What the metadata says, leaving out what it does not.
	void write_metadata(json_writer& json, const prop::item_metadata& md)
	{
		json.key("metadata").object();
		write_text(json, "title", md.title);
		write_text(json, "description", md.description);
		write_text(json, "comment", md.comment);
		write_text(json, "tags", md.tags);
		write_text(json, "label", md.label);
		if (md.rating != 0) json.integer("rating", md.rating);
		write_date(json, "created", md.created());
		if (md.width != 0 && md.height != 0) json.integer("width", md.width).integer("height", md.height);
		if (md.duration != 0) json.integer("duration_s", md.duration);
		if (md.orientation != core::orientation::top_left)
			json.integer("orientation", static_cast<int>(md.orientation));
		if (md.is_panorama()) json.boolean("panorama", true);
		write_text(json, "camera_manufacturer", md.camera_manufacturer);
		write_text(json, "camera_model", md.camera_model);
		write_text(json, "lens", md.lens);
		if (md.iso_speed != 0) json.integer("iso", md.iso_speed);
		if (md.f_number > 0.0f) json.number("f_number", md.f_number);
		if (md.exposure_time > 0.0f) json.number("exposure_time_s", md.exposure_time);
		if (md.focal_length > 0.0f) json.number("focal_length_mm", md.focal_length);
		write_text(json, "artist", md.artist);
		write_text(json, "album", md.album);
		write_text(json, "album_artist", md.album_artist);
		write_text(json, "genre", md.genre);
		write_text(json, "composer", md.composer);
		write_text(json, "show", md.show);
		if (md.year != 0) json.integer("year", md.year);
		write_text(json, "location_place", md.location_place);
		write_text(json, "location_state", md.location_state);
		write_text(json, "location_country", md.location_country);

		if (md.has_gps())
		{
			json.key("gps").object().number("latitude", md.coordinate.latitude());
			json.number("longitude", md.coordinate.longitude());
			if (md.altitude != 0.0f) json.number("altitude_m", md.altitude);
			json.end_object();
		}

		write_text(json, "copyright_creator", md.copyright_creator);
		write_text(json, "copyright_notice", md.copyright_notice);
		write_text(json, "video_codec", md.video_codec);
		write_text(json, "audio_codec", md.audio_codec);
		if (md.audio_sample_rate != 0) json.integer("audio_sample_rate", md.audio_sample_rate);
		if (md.audio_channels != 0) json.integer("audio_channels", md.audio_channels);
		if (md.frame_rate > 0.0f) json.number("frame_rate", md.frame_rate);
		if (md.bit_depth != 0) json.integer("bit_depth", md.bit_depth);
		write_text(json, "color_profile", md.color_profile);
		if (md.is_hdr()) json.text("hdr", prop::format_hdr(md.hdr));
		if (md.is_animated()) json.boolean("animated", true);
		if (md.has_alpha()) json.boolean("transparent", true);
		write_text(json, "game", md.game);
		write_text(json, "system", md.system);
		write_text(json, "contents", md.contents);
		json.end_object();
	}

	std::string_view match_name(const df::copy_grade grade)
	{
		switch (grade)
		{
		case df::copy_grade::identical: return "identical";
		case df::copy_grade::same_file: return "same_file";
		case df::copy_grade::same_picture: return "same_picture";
		case df::copy_grade::none: break;
		}

		return {};
	}

	// The set of copies the collection holds of a file, where it holds more than one.
	std::optional<df::duplicate_info> copies_of(const df::index_file_item& file)
	{
		const auto dup = file.duplicates.load();
		if (dup.group == 0 || dup.count < 2) return std::nullopt;
		return dup;
	}

	// One file the index holds: its kind, size and dates, the copies of it, and its metadata where asked.
	void write_file(json_writer& json, const core::file_path path, const df::index_file_item& file,
	                const bool with_metadata)
	{
		json.text("path", path.str()).text("kind", "file");
		if (file.ft && file.ft->group) json.text("type", file.ft->group->name);
		json.integer("size", static_cast<int64_t>(file.size.to_int64()));
		write_date(json, "modified", file.file_modified.load());
		write_date(json, "file_created", file.file_created);

		if (const auto copies = copies_of(file))
		{
			json.key("duplicates").object().integer("group", copies->group).integer("count", copies->count);
			if (const auto how = match_name(copies->grade); !how.empty()) json.text("match", how);
			json.end_object();
		}

		if (const auto md = file.metadata.load())
		{
			write_date(json, "created", md->created());
			if (md->width != 0 && md->height != 0) json.integer("width", md->width).integer("height", md->height);
			if (md->duration != 0) json.integer("duration_s", md->duration);
			if (md->rating != 0) json.integer("rating", md->rating);
			write_text(json, "label", md->label);
			if (with_metadata) write_metadata(json, *md);
		}
	}

	// The order a search answers in: by path, which keeps a folder's files together; by when each was taken, which
	// puts those taken at the same time together; or by the set of copies each belongs to.
	enum class result_order
	{
		path,
		created,
		duplicates,
	};

	core::mcp::tool_result search_answer(const std::string& query, const df::search_t& search,
	                                     index_state::query_item_results results, const bool complete,
	                                     const size_t offset, const size_t limit, const bool with_metadata,
	                                     const result_order order, const bool index_busy)
	{
		const auto path_of = [](const index_state::query_item& item)
		{
			return item.kind == index_state::query_item_kind::folder ? item.folder_path.text().str() : item.path.str();
		};

		const auto created_of = [](const index_state::query_item& item)
		{
			const auto md = item.kind == index_state::query_item_kind::file ? item.file.metadata.load() : nullptr;
			return md ? md->created() : core::date_t{};
		};

		const auto group_of = [](const index_state::query_item& item) -> uint32_t
		{
			if (item.kind != index_state::query_item_kind::file) return 0;
			const auto copies = copies_of(item.file);
			return copies ? copies->group : 0;
		};

		std::ranges::sort(results, [&](const auto& l, const auto& r)
		{
			if (order == result_order::created)
			{
				const auto lc = created_of(l);
				const auto rc = created_of(r);
				if (lc.is_valid() != rc.is_valid()) return lc.is_valid();
				if (lc != rc) return lc < rc;
			}
			else if (order == result_order::duplicates)
			{
				const auto lg = group_of(l);
				const auto rg = group_of(r);
				if ((lg != 0) != (rg != 0)) return lg != 0;
				if (lg != rg) return lg < rg;
			}

			return core::icmp(path_of(l), path_of(r)) < 0;
		});

		const auto total = results.size();
		const auto first = std::min(offset, total);
		const auto last = std::min(total, first + limit);

		json_writer json;
		json.object().text("query", query).text("search", search.text());
		json.integer("total", static_cast<int64_t>(total)).integer("offset", static_cast<int64_t>(first));
		json.boolean("complete", complete).boolean("more", last < total).boolean("index_busy", index_busy);
		json.key("items").array();

		std::string text = std::format("{} item{} match {}{}", total, total == 1 ? "" : "s", query,
		                               complete ? "" : " (the search was stopped before it finished)");
		if (index_busy)
		{
			text += ". The index was still scanning folders or reading files, so this can change: wait_until_idle "
					"waits for it";
		}
		if (first < last) text += std::format("; {} to {}:", first + 1, last);

		for (auto i = first; i < last; ++i)
		{
			const auto& item = results[i];
			json.object();

			if (item.kind == index_state::query_item_kind::folder)
			{
				json.text("path", path_of(item)).text("kind", "folder");
			}
			else if (item.kind == index_state::query_item_kind::file)
			{
				write_file(json, item.path, item.file, with_metadata);
			}
			else
			{
				json.text("path", path_of(item)).text("kind", "file");
			}

			json.end_object();
			text += "\n" + path_of(item);

			if (order == result_order::created)
			{
				if (const auto created = created_of(item); created.is_valid()) text += "  " + created.to_xmp_date();
			}
			else if (const auto group = group_of(item); group != 0)
			{
				text += std::format("  (copies {}: {})", group, static_cast<uint32_t>(copies_of(item.file)->count));
			}
		}

		json.end_array().end_object();
		if (last < total) text += std::format("\n... {} more: offset {} reads on", total - last, last);
		return structured_result(std::move(text), json.str());
	}

	void add_search(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "search";
		t.title = "Search the library";
		t.description = "Searches the library as the address box does, without changing the window: words, "
						"scope:value, @photo, @duplicates, with:location and without:location, dates and ratings "
						"compared, folders - help with topic search has the language. Answers with how many items "
						"match and a page of them: each one's kind, size, dates, dimensions and rating, the set of "
						"copies it belongs to where the collection holds more than one, and with metadata everything "
						"the index knows of it. In order of path, of when each was taken (created), or of the sets "
						"of copies (duplicates). Waits for the index to load first.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"query":{{"type":"string","description":"What the address box would be given"}},"limit":{{"type":"integer","minimum":1,"maximum":1000,"description":"How many items to list; 100 by default"}},"offset":{{"type":"integer","minimum":0,"description":"How many to pass over first, to read on"}},"metadata":{{"type":"boolean","description":"Each item's metadata too"}},"sort":{{"type":"string","enum":["path","created","duplicates"],"description":"The order: path by default"}},{}}},"required":["query"],"additionalProperties":false}})",
			wait_property);
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto query = call->text("query");
			const auto limit = static_cast<size_t>(call->number("limit").value_or(100));
			const auto offset = static_cast<size_t>(call->number("offset").value_or(0));
			const auto with_metadata = call->flag("metadata", false);
			const auto sort = call->text("sort");
			const auto order = sort == "created"      ? result_order::created
			                   : sort == "duplicates" ? result_order::duplicates
			                                          : result_order::path;
			const auto wait_s = mcp_host::wait_seconds(*call);

			const auto job = host.start_job("search");
			const auto cancelled = std::make_shared<std::atomic_bool>(false);
			job->on_cancel([cancelled] { *cancelled = true; });
			job->progress(-1.0, "waiting for the index to load");

			host.when([&app] { return app._item_index.is_init_complete(); }, 0.0, wait_s,
			          [&app, job, cancelled, query, limit, offset, with_metadata, order](const bool ready)
			{
				if (job->is_done()) return;

				if (!ready)
				{
					job->complete(core::mcp::tool_result::error(
						"The index is still loading: wait_until_idle waits for it, then search again"));
					return;
				}

				job->progress(-1.0, "searching");
				const auto index_busy = app._item_index.scanning_items != 0 || app._item_index.indexing != 0;

				app.queue_async(async_queue::query,
				                [&app, job, cancelled, query, limit, offset, with_metadata, order, index_busy]
				{
					const auto search = df::search_t::parse(query);

					app._item_index.query_items(search,
					                            [&](index_state::query_item_results results, const bool complete)
					{
						job->complete(search_answer(query, search, std::move(results), complete, offset, limit,
						                            with_metadata, order, index_busy));
					}, core::cancel_token(*cancelled));
				});
			});

			host.wait(call, job, wait_s + 10.0, false);
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	void add_item_info(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "item_info";
		t.title = "What Diffractor knows of a file";
		t.description = "Everything Diffractor reads from one file: its kind, size and dates, and its metadata - "
						"title, description, tags, rating and label, camera and exposure, place and coordinates, "
						"dimensions, duration and codecs. From the index where it has read the file since it last "
						"changed, otherwise read from the file itself.";
		t.input_schema =
			R"({"type":"object","properties":{"path":{"type":"string","description":"The file's full path"}},"required":["path"],"additionalProperties":false})";
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			const auto text = call->text("path");

			if (!core::is_path(text))
			{
				call->finish(core::mcp::tool_result::error(std::format("Give the file's full path, not {}", text)));
				return;
			}

			const auto path = core::file_path(text);
			// A file the index has listed but not yet read - one a task has just written, say - is read here instead:
			// the index's record of it holds no metadata until then.
			const auto indexed = app._item_index.find_scanned_item(path);

			if (indexed.ft)
			{
				json_writer json;
				json.object().boolean("in_index", true);
				write_file(json, path, indexed, true);
				json.end_object();
				const auto md = indexed.metadata.load();
				call->finish(structured_result(file_summary(path, indexed.ft, indexed.size.to_int64(), md.get()) +
				                                   "; in the index",
				                               json.str()));
				return;
			}

			const auto job = host.start_job("item_info");

			app.queue_async(async_queue::work, [job, path]
			{
				if (!platform::exists(path))
				{
					job->complete(core::mcp::tool_result::error(std::format("There is no file {}", path)));
					return;
				}

				const auto ft = files::file_type_from_name(path);
				const auto info = platform::file_attributes(path);
				files reader;
				const auto scanned = reader.scan_file(path, false, ft);

				json_writer json;
				json.object().boolean("in_index", false).text("path", path.str()).text("kind", "file");
				if (ft && ft->group) json.text("type", ft->group->name);
				json.integer("size", static_cast<int64_t>(info.size));
				write_date(json, "modified", core::date_t(info.modified));
				write_date(json, "file_created", core::date_t(info.created));
				const auto md = scanned.success ? scanned.to_props() : nullptr;
				if (md) write_metadata(json, *md);
				json.end_object();

				job->complete(
					structured_result(file_summary(path, ft, info.size, md.get()) +
				                          (md ? "; read from the file"s : "; Diffractor reads no metadata from it"s),
				                      json.str()));
			});

			host.wait(call, job, mcp_host::wait_seconds(*call), false);
		};
		host.add(std::move(t), tool_tier::read, true);
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Places for items taken without one
	///////////////////////////////////////////////////////////////////////////////////////////////////

	std::string day_text(const core::date_t date)
	{
		const auto d = date.date();
		return std::format("{:04}-{:02}-{:02}", d.year, d.month, d.day);
	}

	std::string minute_text(const core::date_t date)
	{
		const auto d = date.date();
		return std::format("{:04}-{:02}-{:02} {:02}:{:02}", d.year, d.month, d.day, d.hour, d.minute);
	}

	std::string gap_text(const int64_t seconds)
	{
		const auto s = std::abs(seconds);
		if (s < 60) return std::format("{} s", s);
		const auto minutes = (s + 30) / 60;
		if (minutes < 60) return std::format("{} min", minutes);
		return std::format("{} h {} min", minutes / 60, minutes % 60);
	}

	// The days a group and the item it takes its place from were taken on, as a search that shows them together.
	std::string group_search(const location_suggestion& group)
	{
		auto first = group.located.created.to_days();
		auto last = first;

		for (const auto& item : group.items)
		{
			first = std::min(first, item.created.to_days());
			last = std::max(last, item.created.to_days());
		}

		std::string search;

		for (auto day = first; day <= last; ++day)
		{
			if (!search.empty()) search += " or ";
			search += day_text(core::date_t::from_days(day));
		}

		return search;
	}

	// The place a coordinate is at or near, or how far it is from the nearest.
	std::string place_text(const location_cache& locations, const gps_coordinate coordinate)
	{
		const auto attributed = locations.find_attributed(coordinate);

		switch (attributed.attribution)
		{
		case location_attribution::at: return qualified_name(attributed.place);
		case location_attribution::near: return "near " + qualified_name(attributed.place);
		case location_attribution::remote: return bearing_descriptor(attributed);
		case location_attribution::none: break;
		}

		return {};
	}

	struct location_suggest_answer
	{
		std::string query;
		int64_t max_gap_minutes = 0;
		size_t placed = 0;
		size_t undated = 0;
		size_t unplaced = 0;
		size_t other = 0;
		std::vector<location_suggestion> groups;
		// Where the page's groups take their places from, in words.
		std::vector<std::string> places;
		size_t first = 0;
		bool complete = true;
		bool index_busy = false;
	};

	std::string counted(const size_t n, const std::string_view one, const std::string_view many)
	{
		return std::format("{} {}", n, n == 1 ? one : many);
	}

	core::mcp::tool_result location_suggest_result(const location_suggest_answer& a)
	{
		const auto total = a.groups.size();
		const auto last = a.first + a.places.size();
		size_t suggested = 0;
		for (const auto& group : a.groups) suggested += group.items.size();

		json_writer json;
		json.object().text("query", a.query).integer("max_gap_minutes", a.max_gap_minutes);
		json.integer("unplaced", static_cast<int64_t>(a.unplaced))
			.integer("suggested", static_cast<int64_t>(suggested));
		json.integer("placed", static_cast<int64_t>(a.placed)).integer("undated", static_cast<int64_t>(a.undated));
		json.integer("other", static_cast<int64_t>(a.other)).integer("groups_total", static_cast<int64_t>(total));
		json.integer("offset", static_cast<int64_t>(a.first)).boolean("more", last < total);
		json.boolean("complete", a.complete).boolean("index_busy", a.index_busy);
		json.key("groups").array();

		auto text = std::format("Of the items {} finds{}, {} when but not where they were taken", a.query,
		                        a.complete ? "" : " (the search was stopped before it finished)",
		                        counted(a.unplaced, "says", "say"));
		if (a.placed != 0) text += ", " + counted(a.placed, "has a place already", "have a place already");
		if (a.undated != 0) text += ", " + counted(a.undated, "says neither", "say neither");
		if (a.other != 0) text += ", " + counted(a.other, "is not a photo or video", "are not photos or videos");
		text += suggested == 0
		            ? std::format(". None was taken within {} min of an item with a place", a.max_gap_minutes)
		            : std::format(". {} of them {} taken within {} min of an item with a place: {} by the "
		                          "item with a place taken nearest each",
		                          suggested, suggested == 1 ? "was" : "were", a.max_gap_minutes,
		                          counted(total, "group", "groups"));
		if (a.index_busy)
		{
			text += ". The index was still scanning folders or reading files, so this can change: wait_until_idle "
					"waits for it";
		}
		if (a.first < last) text += std::format(". Groups {} to {}:", a.first + 1, last);

		for (auto i = a.first; i < last; ++i)
		{
			const auto& group = a.groups[i];
			const auto& place = a.places[i - a.first];
			const auto& located = group.located;
			const auto search = group_search(group);
			int64_t widest = 0;
			for (const auto& item : group.items) widest = std::max(widest, std::abs(item.gap_s));

			json.object()
				.integer("group", static_cast<int64_t>(i + 1))
				.integer("count", static_cast<int64_t>(group.items.size()));
			json.text("first", group.items.front().created.to_xmp_date());
			json.text("last", group.items.back().created.to_xmp_date());
			json.integer("max_gap_s", widest).text("search", search);
			json.key("located")
				.object()
				.text("path", located.path.str())
				.text("created", located.created.to_xmp_date());
			json.number("latitude", located.coordinate.latitude()).number("longitude", located.coordinate.longitude());
			if (!place.empty()) json.text("place", place);
			json.end_object().key("items").array();

			const auto first_taken = minute_text(group.items.front().created);
			const auto last_taken = minute_text(group.items.back().created);
			text += std::format("\n\nGroup {}: {} taken {}, within {} of {}, taken {} at {} {}{}. Search: {}", i + 1,
			                    counted(group.items.size(), "item", "items"),
			                    first_taken == last_taken ? first_taken : first_taken + " to " + last_taken,
			                    gap_text(widest), located.path, minute_text(located.created),
			                    core::to_string(located.coordinate.latitude(), 5),
			                    core::to_string(located.coordinate.longitude(), 5), place.empty() ? "" : ", " + place,
			                    search);

			for (const auto& item : group.items)
			{
				json.object().text("path", item.path.str()).text("created", item.created.to_xmp_date());
				json.integer("gap_s", item.gap_s).end_object();
				text += std::format("\n  {}  {}", item.path, minute_text(item.created));
			}

			json.end_array().end_object();
		}

		json.end_array().end_object();
		if (last < total) text += std::format("\n\n... {} more groups: offset {} reads on", total - last, last);
		if (a.first < last)
		{
			text += "\n\nTo place a group: navigate to its search, select its items by path and command_run "
					"tool_locate. Locate proposes the place of the item with one taken nearest in time to the last "
					"item selected; task_state shows it. command_run locate_run, which needs -mcp-allow:write, "
					"writes it.";
		}

		return structured_result(std::move(text), json.str());
	}

	void add_location_suggest(mcp_host& host)
	{
		core::mcp::tool t;
		t.name = "location_suggest";
		t.title = "Places for items taken without one";
		t.description = "For the photos and videos a search finds that say when they were taken but not where, the "
						"item with a place taken nearest each in time, where one was taken within max_gap_minutes: in "
						"groups that share one, each with that item's path, coordinates and place, the items and when "
						"each was taken, and a search that shows them together. Locate proposes that place for the "
						"items when the window shows them with it, so a group is placed by navigating to its search, "
						"selecting its items by path and running tool_locate, then locate_run. Waits for the index to "
						"load first.";
		t.input_schema = std::format(
			R"({{"type":"object","properties":{{"query":{{"type":"string","description":"The items to find places for, as the address box would be given: a folder, a date, without:location"}},"max_gap_minutes":{{"type":"integer","minimum":1,"maximum":1440,"description":"How long before or after an item the one with a place may have been taken; 15 by default"}},"limit":{{"type":"integer","minimum":1,"maximum":500,"description":"How many groups to list; 20 by default"}},"offset":{{"type":"integer","minimum":0,"description":"How many groups to pass over first, to read on"}},{}}},"required":["query"],"additionalProperties":false}})",
			wait_property);
		t.annotations.read_only = true;
		t.annotations.idempotent = true;
		t.handler = [&host](const core::mcp::call_ptr& call)
		{
			auto& app = host.app();
			location_suggest_answer answer;
			answer.query = call->text("query");
			answer.max_gap_minutes = std::clamp(static_cast<int64_t>(call->number("max_gap_minutes").value_or(15)),
			                                    int64_t{1}, int64_t{1440});
			const auto limit = static_cast<size_t>(call->number("limit").value_or(20));
			const auto offset = static_cast<size_t>(call->number("offset").value_or(0));
			const auto wait_s = mcp_host::wait_seconds(*call);

			const auto job = host.start_job("location_suggest");
			const auto cancelled = std::make_shared<std::atomic_bool>(false);
			job->on_cancel([cancelled] { *cancelled = true; });
			job->progress(-1.0, "waiting for the index to load");

			host.when([&app] { return app._item_index.is_init_complete(); }, 0.0, wait_s,
			          [&app, job, cancelled, answer, limit, offset](const bool ready) mutable
			{
				if (job->is_done()) return;

				if (!ready)
				{
					job->complete(core::mcp::tool_result::error(
						"The index is still loading: wait_until_idle waits for it, then ask again"));
					return;
				}

				job->progress(-1.0, "finding the items taken nearest in time");
				answer.index_busy = app._item_index.scanning_items != 0 || app._item_index.indexing != 0;

				app.queue_async(async_queue::query, [&app, job, cancelled, answer, limit, offset]() mutable
				{
					const auto search = df::search_t::parse(answer.query);

					app._item_index.query_items(search,
					                            [&](const index_state::query_item_results& results, const bool complete)
					{
						std::vector<unplaced_item> unplaced;

						for (const auto& item : results)
						{
							if (item.kind != index_state::query_item_kind::file) continue;

							const auto md = item.file.metadata.load();
							if (md && md->coordinate.is_valid()) ++answer.placed;
							else if (!item.file.ft || !item.file.ft->has_trait(file_traits::edit)) ++answer.other;
							else if (!md || !md->created().is_valid()) ++answer.undated;
							else unplaced.push_back({item.path, md->created()});
						}

						answer.complete = complete;
						answer.unplaced = unplaced.size();
						answer.groups = suggest_locations(std::move(unplaced), app._item_index.dated_locations(),
						                                  answer.max_gap_minutes * 60);
						answer.first = std::min(offset, answer.groups.size());
					}, core::cancel_token(*cancelled));

					// The gazetteer is the location worker's, which names the places the page's groups take theirs from.
					app.queue_location([job, answer = std::move(answer), limit](const location_cache& locations) mutable
					{
						const auto last = std::min(answer.groups.size(), answer.first + limit);

						for (auto i = answer.first; i < last; ++i)
						{
							answer.places.push_back(place_text(locations, answer.groups[i].located.coordinate));
						}

						job->complete(location_suggest_result(answer));
					});
				});
			});

			host.wait(call, job, wait_s + 10.0, false);
		};
		host.add(std::move(t), tool_tier::read, true);
	}
}

void add_engine_tools(mcp_host& host)
{
	add_movie_check(host);
	add_movie_render(host);
	add_run_tests(host);
	add_validate_translations(host);
	add_generate_docs(host);
	add_duplicate_report(host);
}

void add_library_tools(mcp_host& host)
{
	add_search(host);
	add_item_info(host);
	add_location_suggest(host);
}
