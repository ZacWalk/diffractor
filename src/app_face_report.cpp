// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implements the /face-report command line option. Runs the experimental detect-align-embed
// pipeline over a named list of photographs and reports every cross-file similarity, under several
// decode variants, so a false match or a missed match can be attributed to the colour path, the
// decode resolution or the model itself rather than guessed at. Takes a list file so the pictures
// being measured never enter the repository. Read-only: it opens no database, writes no file except
// the report, and changes no application behaviour.

#include "pch.h"
#include "app_command_line.h"
#include "files.h"
#include "model_faces.h"
#include "libdesktop/platform.h"

#include <cstdio>

namespace
{
	// Mirrors model_index.cpp, so the first variant measures the shipped decode rather than a
	// restatement of it.
	constexpr sizei shipped_decode_bound{1600, 1600};

	struct decode_variant
	{
		std::string_view name;
		sizei bound;
		bool can_use_yuv;
	};

	// The shipped decode first. The same bound without YUV isolates the colour path on its own; the
	// two other bounds then move resolution with the colour path held still.
	constexpr decode_variant variants[]{
		{"shipped"sv, shipped_decode_bound, true},
		{"packed"sv, shipped_decode_bound, false},
		{"packed-max"sv, sizei{8192, 8192}, false},
		{"packed-small"sv, sizei{640, 640}, false},
	};

	struct measured_face
	{
		int file = 0;
		int index = 0;
		// Kept whole because grouping ranks a face from its box and its landmarks, not from its vector.
		faces::detection d;
		faces::face_vector vec{};
	};

	struct measured_file
	{
		df::file_path path;
		sizei extent;
		std::string_view format = "none"sv;
		std::vector<measured_face> faces;
	};

	struct report_writer
	{
		FILE* file = nullptr;

		~report_writer()
		{
			if (file) fclose(file);
		}

		void line(const std::string_view text) const
		{
			printf("%.*s\n", static_cast<int>(text.size()), text.data());
			if (file) fprintf(file, "%.*s\n", static_cast<int>(text.size()), text.data());
		}
	};

	std::vector<df::file_path> read_list(const df::file_path path)
	{
		std::vector<df::file_path> result;

		df::blob owner;
		file_read_stream stream;

		if (!stream.open(path)) return result;

		const auto data = stream.view_all(owner);
		const std::string_view text(std::bit_cast<const char*>(data.data), data.size);

		for (const auto raw : str::split(text, false, [](const char c) { return c == '\n' || c == '\r'; }))
		{
			const auto line = str::trim(raw);
			if (line.empty() || line.front() == '#') continue;
			result.emplace_back(line);
		}

		return result;
	}

	// Mirrors index_state::detect_faces_in: the same clipping refusal and the same embed, so what the
	// report measures is what the collection pass would have stored.
	void measure(faces::face_engine& engine, const ui::const_surface_ptr& surface, const int file_index,
	             measured_file& result)
	{
		if (!ui::is_valid(surface)) return;

		result.extent = surface->dimensions();
		result.format = ui::to_string(surface->format());

		const auto& t = faces::defaults();
		auto index = 0;

		for (const auto& d : engine.detect(surface, t))
		{
			const auto transform = faces::align_transform(d.marks);
			if (faces::alignment_is_clipped(transform, result.extent, t)) continue;

			measured_face f;
			f.file = file_index;
			f.index = index++;
			f.d = d;

			if (engine.embed(surface, transform, f.vec)) result.faces.emplace_back(std::move(f));
		}
	}

	measured_file measure_file(faces::face_engine& engine, files& ff, const df::file_path path,
	                           const decode_variant& v, const int file_index)
	{
		measured_file m;
		m.path = path;

		df::blob owner;
		file_read_stream stream;

		if (stream.open(path))
		{
			const auto surface = ff.image_to_surface(stream.view_all(owner), v.bound, v.can_use_yuv,
			                                         decode_intent::analysis);
			measure(engine, surface, file_index, m);
		}

		return m;
	}

	void collect_photos(const df::folder_path folder, std::vector<df::file_path>& result)
	{
		auto contents = platform::iterate_file_items(folder, false);

		for (const auto& f : contents.files)
		{
			const auto ft = files::file_type_from_name(f.name);
			if (ft && ft->has_trait(file_traits::bitmap)) result.emplace_back(folder, f.name);
		}

		for (const auto& sub : contents.folders) collect_photos(folder.combine(sub.name.sv()), result);
	}

	float best_between(const measured_file& a, const measured_file& b)
	{
		auto best = -2.0f;

		for (const auto& l : a.faces)
		{
			for (const auto& r : b.faces) best = std::max(best, faces::similarity(l.vec, r.vec));
		}

		return best;
	}

	void report_variant(const report_writer& out, const decode_variant& v,
	                    const std::vector<measured_file>& files)
	{
		const auto& t = faces::defaults();

		out.line("");
		out.line(std::format("=== {} (bound {}x{}, {}) ===", v.name, v.bound.cx, v.bound.cy,
		                     v.can_use_yuv ? "yuv allowed"sv : "packed"sv));
		out.line("");

		for (auto i = 0u; i < files.size(); ++i)
		{
			const auto& f = files[i];
			out.line(std::format("  [{}] {}", i, f.path));
			out.line(std::format("      surface : {}x{} {}", f.extent.cx, f.extent.cy, f.format));

			if (f.faces.empty())
			{
				out.line("      no faces");
				continue;
			}

			for (const auto& face : f.faces)
			{
				// The crop edge against 112 is the question the resolution variants exist to answer:
				// below it the aligned crop is an upsample and the vector is measuring blur.
				const auto edge = std::min(face.d.bounds.Width, face.d.bounds.Height);
				out.line(std::format(
					"      face {} : conf {:>3}  at {:.0f},{:.0f} {:.0f}x{:.0f}  crop {:.0f}px{}  frontality {:.2f}",
					face.index, face.d.confidence, face.d.bounds.X, face.d.bounds.Y, face.d.bounds.Width,
					face.d.bounds.Height, edge, edge < faces::aligned_extent ? " (upsampled)" : "",
					faces::frontality(face.d.marks)));
			}
		}

		out.line("");
		out.line(std::format("  cross-file best similarity, * at or above the {:.3f} grouping threshold",
		                     t.similar));
		out.line("");

		std::string header = "        ";
		for (auto i = 0u; i < files.size(); ++i) header += std::format("{:>8}", i);
		out.line(header);

		for (auto i = 1u; i < files.size(); ++i)
		{
			auto row = std::format("     {:>2} ", i);

			for (auto j = 0u; j < i; ++j)
			{
				const auto s = best_between(files[i], files[j]);
				row += s < -1.0f ? std::format("{:>8}", "-") : std::format("{:>7.3f}{}", s, s >= t.similar ? '*' : ' ');
			}

			out.line(row);
		}

		// Which two faces actually matched, which a file-level maximum cannot say and which decides
		// whether a false pair is one bad face or the whole picture.
		std::vector<std::tuple<float, const measured_face*, const measured_face*>> pairs;

		for (auto i = 1u; i < files.size(); ++i)
		{
			for (auto j = 0u; j < i; ++j)
			{
				for (const auto& l : files[i].faces)
				{
					for (const auto& r : files[j].faces)
					{
						if (const auto s = faces::similarity(l.vec, r.vec); s >= t.similar)
						{
							pairs.emplace_back(s, &l, &r);
						}
					}
				}
			}
		}

		std::ranges::sort(pairs, std::greater{}, [](const auto& p) { return std::get<0>(p); });

		out.line("");
		out.line(std::format("  cross-file face pairs at or above {:.3f} : {}", t.similar, pairs.size()));

		for (const auto& [s, l, r] : pairs)
		{
			out.line(std::format("      {:.3f}  [{}] face {}  <->  [{}] face {}", s, l->file, l->index, r->file,
			                     r->index));
		}
	}

	// One embed, many groupings. The threshold is the only knob that moves both failure modes at
	// once - too low merges strangers, too high strands faces in the ambiguity margin - so the curve
	// is worth more than any single point, exactly as the duplicate report sweeps its own.
	void report_threshold_sweep(const report_writer& out, const std::vector<faces::candidate>& candidates,
	                            const std::vector<measured_file>& corpus, const size_t of_interest)
	{
		out.line("");
		out.line("  threshold sweep");
		out.line("");
		out.line("    similar  groups  ungrouped  largest  >50  listed pairs sharing a group");

		for (auto similar = 0.40f; similar <= 0.7051f; similar += 0.025f)
		{
			auto t = faces::defaults();
			t.similar = similar;

			const auto grouped = faces::build_groups(candidates, t);

			df::hash_map<faces::face_ref, size_t, faces::face_ref_hash> group_of;

			for (auto g = 0u; g < grouped.groups.size(); ++g)
			{
				for (const auto& m : grouped.groups[g].members) group_of[m] = g;
			}

			size_t largest = 0;
			auto over_fifty = 0;

			for (const auto& g : grouped.groups)
			{
				largest = std::max(largest, g.members.size());
				if (g.members.size() > 50) ++over_fifty;
			}

			std::vector<size_t> placement;

			for (auto i = 0u; i < of_interest && i < corpus.size(); ++i)
			{
				auto found_group = SIZE_MAX;

				for (const auto& face : corpus[i].faces)
				{
					faces::face_ref ref;
					ref.path = str::cache(corpus[i].path.pack());
					ref.index = face.index;

					if (const auto found = group_of.find(ref); found != group_of.end())
					{
						found_group = found->second;
						break;
					}
				}

				placement.emplace_back(found_group);
			}

			std::string pairs;

			for (auto a = 0u; a < placement.size(); ++a)
			{
				for (auto b = a + 1; b < placement.size(); ++b)
				{
					if (placement[a] == SIZE_MAX || placement[a] != placement[b]) continue;
					pairs += std::format("{}-{} ", a, b);
				}
			}

			const auto faces_total = candidates.size();
			const auto ungrouped_pct = faces_total > 0
				                           ? (100.0 * grouped.ungrouped.size() / faces_total)
				                           : 0.0;

			out.line(std::format("      {:.3f}  {:>6}  {:>8.1f}%  {:>7}  {:>3}  {}", similar,
			                     grouped.groups.size(), ungrouped_pct, largest, over_fifty,
			                     pairs.empty() ? "none"s : pairs));
		}
	}

	// Runs the shipped grouper over the whole corpus, then answers the question a pairwise number
	// cannot: which anchor each face of interest actually reached, and what it lost to. An unnamed
	// group's anchor is its first member, so the leader is nameable and a merge through a third face
	// is visible rather than inferred.
	void report_grouping(const report_writer& out, const std::vector<measured_file>& corpus,
	                     const size_t of_interest)
	{
		const auto& t = faces::defaults();

		std::vector<faces::candidate> candidates;

		for (const auto& f : corpus)
		{
			const auto path = str::cache(f.path.pack());

			for (const auto& face : f.faces)
			{
				faces::candidate c;
				c.ref.path = path;
				c.ref.index = face.index;
				c.vec = face.vec;
				c.crop = face.d.bounds;
				c.crop_space = f.extent;
				c.rank = faces::candidate_rank(face.d, f.extent);
				c.frontality = faces::frontality(face.d.marks);
				candidates.emplace_back(std::move(c));
			}
		}

		// Clusters only: the report measures what the grouper proposes, not what a sample face would
		// then re-anchor it to.
		const auto grouped = faces::build_groups(candidates, t);

		df::hash_map<faces::face_ref, size_t, faces::face_ref_hash> group_of;

		for (auto g = 0u; g < grouped.groups.size(); ++g)
		{
			for (const auto& m : grouped.groups[g].members) group_of[m] = g;
		}

		out.line("");
		out.line("=== grouping over the whole corpus ===");
		out.line("");
		out.line(std::format("  files    : {}", corpus.size()));
		out.line(std::format("  faces    : {}", candidates.size()));
		out.line(std::format("  groups   : {}", grouped.groups.size()));
		out.line(std::format("  ungrouped: {}", grouped.ungrouped.size()));
		out.line("");

		const auto label_of = [&corpus](const faces::face_ref& r) -> std::string
		{
			for (auto i = 0u; i < corpus.size(); ++i)
			{
				if (icmp(str::cache(corpus[i].path.pack()), r.path) == 0)
				{
					return std::format("{} face {}", corpus[i].path, r.index);
				}
			}

			return std::format("{} face {}", r.path, r.index);
		};

		std::vector<std::pair<int, size_t>> placement;

		for (auto i = 0u; i < of_interest && i < corpus.size(); ++i)
		{
			const auto& f = corpus[i];
			out.line(std::format("  [{}] {}", i, f.path));

			for (const auto& face : f.faces)
			{
				faces::face_ref ref;
				ref.path = str::cache(f.path.pack());
				ref.index = face.index;

				const auto found = group_of.find(ref);

				if (found == group_of.end())
				{
					out.line(std::format("      face {} : UNGROUPED", face.index));
					placement.emplace_back(static_cast<int>(i), SIZE_MAX);
					continue;
				}

				const auto& g = grouped.groups[found->second];
				placement.emplace_back(static_cast<int>(i), found->second);

				out.line(std::format("      face {} : group {} of {} members, similarity to its anchor {:.3f}",
				                     face.index, found->second, g.members.size(),
				                     faces::similarity(face.vec, g.anchor)));

				// The leader is what a merge runs through, so name it whenever it is not this face.
				if (!g.members.empty() && !(g.members.front() == ref))
				{
					out.line(std::format("               anchored by {}", label_of(g.members.front())));
				}

				auto best = -2.0f;
				auto best_group = SIZE_MAX;
				auto second = -2.0f;
				auto second_group = SIZE_MAX;

				for (auto k = 0u; k < grouped.groups.size(); ++k)
				{
					const auto s = faces::similarity(face.vec, grouped.groups[k].anchor);

					if (s > best)
					{
						second = best;
						second_group = best_group;
						best = s;
						best_group = k;
					}
					else if (s > second)
					{
						second = s;
						second_group = k;
					}
				}

				out.line(std::format("               nearest anchor group {} at {:.3f}, runner-up group {} at {:.3f}",
				                     best_group, best, second_group, second));
			}
		}

		out.line("");
		out.line("  which listed files share a group");
		out.line("");

		for (auto a = 0u; a < placement.size(); ++a)
		{
			for (auto b = a + 1; b < placement.size(); ++b)
			{
				if (placement[a].second == SIZE_MAX || placement[a].second != placement[b].second) continue;

				out.line(std::format("      [{}] and [{}] are both in group {}", placement[a].first,
				                     placement[b].first, placement[a].second));
			}
		}

		report_threshold_sweep(out, candidates, corpus, of_interest);
	}
}

int run_face_report(const std::string_view list_text, const std::string_view scan_text,
                    const std::string_view scan_variant_text, const std::string_view output_text)
{
	load_file_types();

	const auto list_path = df::file_path(list_text);

	if (!list_path.exists())
	{
		printf("face-report: list not found: %.*s\n", static_cast<int>(list_text.size()), list_text.data());
		return 1;
	}

	const auto paths = read_list(list_path);

	if (paths.empty())
	{
		printf("face-report: no usable paths in the list\n");
		return 1;
	}

	const auto engine = faces::create_face_engine();

	if (!engine)
	{
		printf("face-report: this build has no face engine\n");
		return 1;
	}

	report_writer out;

	if (!output_text.empty())
	{
		const auto output_path = std::string(output_text);
		if (fopen_s(&out.file, output_path.c_str(), "wt") != 0) out.file = nullptr;
	}

	const auto& t = faces::defaults();

	out.line("Face pipeline report");
	out.line(std::format("  list             : {}", list_path));
	out.line(std::format("  files            : {}", paths.size()));
	out.line(std::format("  model version    : {}", faces::model_version));
	out.line(std::format("  detector edge    : {}", t.detector_edge));
	out.line(std::format("  min confidence   : {}", t.detector_confidence));
	out.line(std::format("  similar          : {:.3f}", t.similar));

	for (const auto& v : variants)
	{
		files ff;
		std::vector<measured_file> measured;
		measured.reserve(paths.size());

		for (auto i = 0u; i < paths.size(); ++i)
		{
			printf("  %.*s: %u/%zu\r", static_cast<int>(v.name.size()), v.name.data(), i + 1, paths.size());
			measured.emplace_back(measure_file(*engine, ff, paths[i], v, static_cast<int>(i)));
		}

		printf("%-60s\r", "");
		report_variant(out, v, measured);
	}

	if (scan_text.empty()) return 0;

	// The listed files lead the corpus so their indices match the pairwise report above, and a scanned
	// copy of one of them is dropped rather than embedded twice.
	printf("Enumerating photos...\n");
	std::vector<df::file_path> corpus_paths(paths);
	std::vector<df::file_path> scanned;

	for (const auto part : str::split(scan_text, false, [](const char c) { return c == ';'; }))
	{
		const auto scan_folder = df::folder_path(str::trim(part));

		if (!platform::exists(scan_folder))
		{
			printf("face-report: scan folder not found: %.*s\n", static_cast<int>(part.size()), part.data());
			return 1;
		}

		collect_photos(scan_folder, scanned);
	}

	for (const auto& p : scanned)
	{
		if (std::ranges::none_of(paths, [&p](const df::file_path listed) { return listed == p; }))
		{
			corpus_paths.emplace_back(p);
		}
	}

	printf("  %zu photos to embed\n", corpus_paths.size());

	// Grouping is measured on one decode only. The variants above already showed what each does to a
	// vector - across a 12.8x range similarity moved under 0.02 - so the cheap decode buys an
	// iterable run without changing which anchor a face reaches.
	const auto* scan_variant = &variants[0];

	if (!scan_variant_text.empty())
	{
		const auto found = std::ranges::find(variants, scan_variant_text, &decode_variant::name);

		if (found == std::end(variants))
		{
			printf("face-report: unknown scan variant: %.*s\n", static_cast<int>(scan_variant_text.size()),
			       scan_variant_text.data());
			return 1;
		}

		scan_variant = &*found;
	}

	files ff;
	std::vector<measured_file> corpus;
	corpus.reserve(corpus_paths.size());

	for (auto i = 0u; i < corpus_paths.size(); ++i)
	{
		if ((i % 64) == 0) printf("  embedding: %u/%zu\n", i, corpus_paths.size());
		corpus.emplace_back(measure_file(*engine, ff, corpus_paths[i], *scan_variant, static_cast<int>(i)));
	}

	out.line("");
	out.line(std::format("  corpus decoded with variant: {}", scan_variant->name));
	report_grouping(out, corpus, paths.size());

	return 0;
}
