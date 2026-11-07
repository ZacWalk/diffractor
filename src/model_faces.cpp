// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The face pipeline's rules - input reduction, alignment, similarity, anchored grouping, and the stored
// form a face row takes. Owned by docs/faces.md. No file, database or window is touched here.

#include "pch.h"

#include "app_text.h"
#include "model_faces.h"

#ifndef DIFFRACTOR_HAS_FACE_ENGINE
namespace faces
{
	// Neither vendored library resolved, so this build has no detector and no model. Face search is
	// then unavailable rather than merely switched off, and the sidebar never offers it.
	face_engine_ptr create_face_engine()
	{
		return {};
	}

	bool face_engine_available()
	{
		return false;
	}
}
#endif

namespace faces
{
	const tuning& defaults()
	{
		static const tuning t;
		return t;
	}

	// The detector reads only luma, including odd output extents that an NV12 surface cannot hold.
	// Match the shared scaler's Q8 weights and rounding without processing or allocating chroma.
	static bool reduce_luma_plane(const ui::const_surface_ptr& surface, const sizei destination,
	                              std::vector<uint8_t>& result)
	{
		const auto source = surface->dimensions();
		if (destination.cx > source.cx || destination.cy > source.cy) return false;

		struct span
		{
			int first;
			std::vector<uint16_t> weights;
		};
		const auto axis = [](const int source_count, const int destination_count)
		{
			std::vector<span> spans;
			spans.reserve(destination_count);
			const auto scale = static_cast<double>(source_count) / destination_count;
			for (auto i = 0; i < destination_count; ++i)
			{
				const auto start = i * scale;
				const auto end = (i + 1) * scale;
				const auto first = std::clamp(static_cast<int>(start), 0, source_count - 1);
				const auto last = std::clamp(static_cast<int>(std::ceil(end)) - 1, first, source_count - 1);
				span sample{first, {}};
				auto previous = 0;
				for (auto j = first; j <= last; ++j)
				{
					const auto edge = std::min(end, static_cast<double>(j + 1));
					const auto covered = std::clamp((edge - start) / (end - start), 0.0, 1.0);
					const auto cumulative = j == last ? 256 :
						std::clamp(static_cast<int>(std::lround(covered * 256.0)), previous, 256);
					sample.weights.emplace_back(static_cast<uint16_t>(cumulative - previous));
					previous = cumulative;
				}
				spans.emplace_back(std::move(sample));
			}
			return spans;
		};
		const auto horizontal = axis(source.cx, destination.cx);
		const auto vertical = axis(source.cy, destination.cy);
		std::vector<uint16_t> accumulated(source.cx);
		std::vector<uint8_t> reduced(source.cx);
		for (auto y = 0; y < destination.cy; ++y)
		{
			std::ranges::fill(accumulated, uint16_t{0});
			const auto& rows = vertical[y];
			for (auto k = 0_z; k < rows.weights.size(); ++k)
			{
				const auto* pixels = surface->pixels() + (rows.first + k) * surface->stride();
				for (auto x = 0; x < source.cx; ++x)
					accumulated[x] = static_cast<uint16_t>(accumulated[x] + pixels[x] * rows.weights[k]);
			}
			for (auto x = 0; x < source.cx; ++x)
				reduced[x] = static_cast<uint8_t>((accumulated[x] + 128) >> 8);
			auto* output = result.data() + static_cast<size_t>(y) * destination.cx;
			for (auto x = 0; x < destination.cx; ++x)
			{
				const auto& columns = horizontal[x];
				uint32_t accumulated_pixel = 0;
				for (auto k = 0_z; k < columns.weights.size(); ++k)
					accumulated_pixel += reduced[columns.first + k] * columns.weights[k];
				output[x] = static_cast<uint8_t>((accumulated_pixel + 128) >> 8);
			}
		}
		return true;
	}

	bool reduce_to_luma(const ui::const_surface_ptr& surface, const sizei destination,
	                    ui::surface_ptr& scratch, std::vector<uint8_t>& result)
	{
		if (!ui::is_valid(surface) || destination.cx <= 0 || destination.cy <= 0) return false;
		const auto packed = ui::is_packed(surface->format());
		if (!packed && surface->format() != ui::texture_format::NV12) return false;
		result.resize(static_cast<size_t>(destination.cx) * destination.cy);
		if (packed)
		{
			auto source = surface;
			if (source->dimensions() != destination)
			{
				if (!ui::area_downscale(source, scratch, destination)) return false;
				source = scratch;
			}
			for (auto row = 0; row < destination.cy; ++row)
			{
				const auto* pixels = source->pixels_line(row);
				auto* output = result.data() + static_cast<size_t>(row) * destination.cx;
				for (auto column = 0; column < destination.cx; ++column)
				{
					const auto* pixel = pixels + static_cast<size_t>(column) * 4;
					output[column] = static_cast<uint8_t>((pixel[0] * 29 + pixel[1] * 150 + pixel[2] * 77 + 128) >> 8);
				}
			}
			return true;
		}
		if (!reduce_luma_plane(surface, destination, result)) return false;
		const auto color_space = surface->color_space();
		if (color_space == ui::color_space::rec601_limited || color_space == ui::color_space::rec709_limited ||
			color_space == ui::color_space::rec2020_limited)
		{
			for (auto& sample : result)
				sample = static_cast<uint8_t>(std::clamp(((static_cast<int>(sample) - 16) * 255 + 109) / 219, 0, 255));
		}
		return true;
	}

	// The five-point template every published MobileFaceNet weight set is trained against, for a
	// 112x112 crop. Changing a number here changes the meaning of every stored vector, which is why
	// faces::model_version sits beside it.
	const landmarks& reference_template()
	{
		static const landmarks t{
			pointd{38.2946, 51.6963}, // left eye
			pointd{73.5318, 51.5014}, // right eye
			pointd{56.0252, 71.7366}, // nose tip
			pointd{41.5493, 92.3655}, // left mouth corner
			pointd{70.7299, 92.2041}, // right mouth corner
		};
		return t;
	}

	// Closed-form least-squares Procrustes fit with uniform scale. Four degrees of freedom - one
	// rotation, one scale, two translations - so the result cannot shear however badly the landmarks
	// are placed. A general affine fit would have six, and would happily squash an off-axis face flat
	// against the template.
	affined align_transform(const landmarks& marks)
	{
		const auto& target = reference_template();

		pointd mean_src{0, 0};
		pointd mean_dst{0, 0};

		for (int i = 0; i < landmark_count; ++i)
		{
			mean_src.X += marks[i].X;
			mean_src.Y += marks[i].Y;
			mean_dst.X += target[i].X;
			mean_dst.Y += target[i].Y;
		}

		mean_src.X /= landmark_count;
		mean_src.Y /= landmark_count;
		mean_dst.X /= landmark_count;
		mean_dst.Y /= landmark_count;

		double var_src = 0;
		double covar_dot = 0;
		double covar_cross = 0;

		for (int i = 0; i < landmark_count; ++i)
		{
			const auto ax = marks[i].X - mean_src.X;
			const auto ay = marks[i].Y - mean_src.Y;
			const auto bx = target[i].X - mean_dst.X;
			const auto by = target[i].Y - mean_dst.Y;

			var_src += ax * ax + ay * ay;
			covar_dot += ax * bx + ay * by;
			covar_cross += ax * by - ay * bx;
		}

		// Five coincident landmarks. There is no orientation to recover, so the honest answer is the
		// translation that at least puts the point where the template expects it.
		if (var_src < 1e-9)
		{
			return affined{}.translate(mean_dst.X - mean_src.X, mean_dst.Y - mean_src.Y);
		}

		// scale * cos and scale * sin of the fitted rotation.
		const auto a = covar_dot / var_src;
		const auto b = covar_cross / var_src;

		// affined applies X = x*t0 + y*t2 + t4, so the rotation matrix rows are (t0, t2) and (t1, t3).
		const auto tx = mean_dst.X - (a * mean_src.X - b * mean_src.Y);
		const auto ty = mean_dst.Y - (b * mean_src.X + a * mean_src.Y);

		return {a, b, -b, a, tx, ty};
	}

	bool alignment_is_clipped(const affined& transform, const sizei source_extent, const tuning& t)
	{
		if (source_extent.is_empty()) return true;

		const auto to_source = transform.invert();
		constexpr double edge = aligned_extent;

		const pointd corners[4]{
			to_source.transform({0, 0}),
			to_source.transform({edge, 0}),
			to_source.transform({edge, edge}),
			to_source.transform({0, edge}),
		};

		auto outside = 0;

		for (const auto& c : corners)
		{
			if (c.X < 0 || c.Y < 0 || c.X > source_extent.cx || c.Y > source_extent.cy)
			{
				++outside;
			}
		}

		if (outside == 0) return false;

		const auto area = [](const std::vector<pointd>& poly)
		{
			auto sum = 0.0;
			for (size_t i = 0; i < poly.size(); ++i)
			{
				const auto& a = poly[i];
				const auto& b = poly[(i + 1) % poly.size()];
				sum += a.X * b.Y - b.X * a.Y;
			}
			return std::abs(sum) * 0.5;
		};
		const auto clip = [](const std::vector<pointd>& input, const auto inside, const auto intersect)
		{
			std::vector<pointd> output;
			if (input.empty()) return output;
			auto prev = input.back();
			auto prev_inside = inside(prev);
			for (const auto& curr : input)
			{
				const auto curr_inside = inside(curr);
				if (curr_inside != prev_inside) output.emplace_back(intersect(prev, curr));
				if (curr_inside) output.emplace_back(curr);
				prev = curr;
				prev_inside = curr_inside;
			}
			return output;
		};

		std::vector<pointd> poly(std::begin(corners), std::end(corners));
		const auto full = area(poly);
		if (full <= 0) return true;

		poly = clip(poly, [](const pointd p) { return p.X >= 0; },
		            [](const pointd a, const pointd b) { return pointd{0, a.Y + (b.Y - a.Y) * (-a.X / (b.X - a.X))}; });
		poly = clip(poly, [source_extent](const pointd p) { return p.X <= source_extent.cx; },
		            [source_extent](const pointd a, const pointd b)
		            {
			            const auto t = (source_extent.cx - a.X) / (b.X - a.X);
			            return pointd{static_cast<double>(source_extent.cx), a.Y + (b.Y - a.Y) * t};
		            });
		poly = clip(poly, [](const pointd p) { return p.Y >= 0; },
		            [](const pointd a, const pointd b) { return pointd{a.X + (b.X - a.X) * (-a.Y / (b.Y - a.Y)), 0}; });
		poly = clip(poly, [source_extent](const pointd p) { return p.Y <= source_extent.cy; },
		            [source_extent](const pointd a, const pointd b)
		            {
			            const auto t = (source_extent.cy - a.Y) / (b.Y - a.Y);
			            return pointd{a.X + (b.X - a.X) * t, static_cast<double>(source_extent.cy)};
		            });

		return (1.0 - area(poly) / full) > t.max_outside_fraction;
	}

	void normalise(face_vector& v)
	{
		double sum = 0;
		for (const auto f : v)
		{
			if (!std::isfinite(f))
			{
				v.fill(0.0f);
				return;
			}

			sum += static_cast<double>(f) * f;
		}

		// Nothing to normalise, and dividing would produce a vector that compares as similar to
		// everything. Left as it is so that similarity() answers zero against every other face.
		if (sum < 1e-12) return;

		const auto inv = static_cast<float>(1.0 / std::sqrt(sum));
		for (auto& f : v) f *= inv;
	}

	float similarity(const face_vector& a, const face_vector& b)
	{
		double sum = 0;
		for (int i = 0; i < vector_size; ++i)
		{
			if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return 0.0f;
			sum += static_cast<double>(a[i]) * b[i];
		}
		return static_cast<float>(std::clamp(sum, -1.0, 1.0));
	}

	face_vector mean_vector(const std::span<const face_vector> vectors)
	{
		face_vector result{};
		if (vectors.empty()) return result;

		std::array<double, vector_size> sums{};

		for (const auto& v : vectors)
		{
			for (int i = 0; i < vector_size; ++i) sums[i] += v[i];
		}

		for (int i = 0; i < vector_size; ++i)
		{
			result[i] = static_cast<float>(sums[i] / static_cast<double>(vectors.size()));
		}

		normalise(result);
		return result;
	}

	double frontality(const landmarks& marks)
	{
		const auto& left_eye = marks[0];
		const auto& right_eye = marks[1];
		const auto& nose = marks[2];

		const auto eye_span = left_eye.dist(right_eye);
		if (eye_span < 1e-6) return 0.0;

		// Yaw reads as the nose sitting nearer one eye than the other; at zero yaw it is halfway.
		const auto to_left = nose.dist(left_eye);
		const auto to_right = nose.dist(right_eye);
		const auto balance = 1.0 - std::abs(to_left - to_right) / (to_left + to_right);

		// Roll reads as the eye line tilting away from horizontal. A quarter turn scores nothing.
		const auto tilt = std::abs(std::atan2(right_eye.Y - left_eye.Y, right_eye.X - left_eye.X));
		const auto level = 1.0 - std::min(1.0, tilt / (geom_pi / 2.0));

		return std::clamp(balance, 0.0, 1.0) * std::clamp(level, 0.0, 1.0);
	}

	sizei upright_extent(const sizei stored_extent, const ui::orientation orientation)
	{
		return ui::flips_xy(orientation) ? sizei{stored_extent.cy, stored_extent.cx} : stored_extent;
	}

	pointd to_upright(const pointd p, const sizei stored_extent, const ui::orientation orientation)
	{
		const auto w = static_cast<double>(stored_extent.cx);
		const auto h = static_cast<double>(stored_extent.cy);

		switch (orientation)
		{
		case ui::orientation::top_right: return {w - p.X, p.Y};
		case ui::orientation::bottom_right: return {w - p.X, h - p.Y};
		case ui::orientation::bottom_left: return {p.X, h - p.Y};
		case ui::orientation::left_top: return {p.Y, p.X};
		case ui::orientation::right_top: return {h - p.Y, p.X};
		case ui::orientation::right_bottom: return {h - p.Y, w - p.X};
		case ui::orientation::left_bottom: return {p.Y, w - p.X};
		default: return p;
		}
	}

	rectd to_upright(const rectd r, const sizei stored_extent, const ui::orientation orientation)
	{
		if (orientation == ui::orientation::top_left || orientation == ui::orientation::none) return r;

		// Two opposite corners are enough for an axis-aligned box under a right-angle turn, but which
		// corner is which changes, so the result is rebuilt from the extremes rather than reordered.
		const auto a = to_upright({r.left(), r.top()}, stored_extent, orientation);
		const auto b = to_upright({r.right(), r.bottom()}, stored_extent, orientation);

		return {std::min(a.X, b.X), std::min(a.Y, b.Y), std::abs(b.X - a.X), std::abs(b.Y - a.Y)};
	}

	double candidate_rank(const detection& d, const sizei source_extent)
	{
		const auto frame = std::max(1, std::min(source_extent.cx, source_extent.cy));
		const auto size_fraction = std::clamp(d.bounds.Width / frame, 0.0, 1.0);

		// Confidence first, because a low-confidence detection is the one most likely not to be a
		// face at all; size and pose only order the ones that are.
		return (d.confidence / 100.0) * 0.5 + size_fraction * 0.2 + frontality(d.marks) * 0.3;
	}

	namespace
	{
		double anchor_distance(const face_vector& left, const face_vector& right)
		{
			return std::sqrt(std::transform_reduce(left.begin(), left.end(), right.begin(), 0.0f,
				std::plus{}, [](const float first, const float second)
				{
					const auto delta = first - second;
					return delta * delta;
				}));
		}

		struct nearest_anchors
		{
			double first_distance = std::numeric_limits<double>::max();
			size_t first = 0;
			bool has_first = false;

			void consider(const double distance, const size_t index)
			{
				if (!has_first || distance < first_distance || (distance == first_distance && index < first))
				{
					first_distance = distance;
					first = index;
					has_first = true;
				}
			}
		};

		class anchor_tree
		{
			struct node
			{
				size_t anchor = 0;
				double radius = 0;
				int near_child = -1;
				int far_child = -1;
			};

			std::vector<size_t> _anchors;
			std::vector<node> _nodes;

			int build_node(std::vector<size_t> anchors, const std::vector<group>& groups)
			{
				const auto vantage = anchors.back();
				anchors.pop_back();
				const auto node_index = static_cast<int>(_nodes.size());
				_nodes.emplace_back(node{.anchor = vantage});
				if (anchors.empty()) return node_index;

				std::vector<std::pair<double, size_t>> distances;
				distances.reserve(anchors.size());
				for (const auto anchor : anchors)
				{
					distances.emplace_back(anchor_distance(groups[vantage].anchor, groups[anchor].anchor), anchor);
				}

				const auto middle = distances.begin() + distances.size() / 2;
				std::nth_element(distances.begin(), middle, distances.end());
				_nodes[node_index].radius = middle->first;

				std::vector<size_t> near;
				std::vector<size_t> far;
				near.reserve(static_cast<size_t>(middle - distances.begin()));
				far.reserve(static_cast<size_t>(distances.end() - middle));
				for (auto current = distances.begin(); current != middle; ++current) near.emplace_back(current->second);
				for (auto current = middle; current != distances.end(); ++current) far.emplace_back(current->second);

				if (!near.empty()) _nodes[node_index].near_child = build_node(std::move(near), groups);
				if (!far.empty()) _nodes[node_index].far_child = build_node(std::move(far), groups);
				return node_index;
			}

			void find_nearest(const int node_index, const face_vector& query, const std::vector<group>& groups,
			                  nearest_anchors& nearest) const
			{
				if (node_index < 0) return;
				const auto& current = _nodes[node_index];
				const auto distance = anchor_distance(query, groups[current.anchor].anchor);
				nearest.consider(distance, current.anchor);

				const auto search_near = [&]
				{
					const auto limit = nearest.has_first ? nearest.first_distance : std::numeric_limits<double>::max();
					if (distance - limit <= current.radius)
						find_nearest(current.near_child, query, groups, nearest);
				};
				const auto search_far = [&]
				{
					const auto limit = nearest.has_first ? nearest.first_distance : std::numeric_limits<double>::max();
					if (distance + limit >= current.radius)
						find_nearest(current.far_child, query, groups, nearest);
				};

				if (distance < current.radius)
				{
					search_near();
					search_far();
				}
				else
				{
					search_far();
					search_near();
				}
			}

			void find_matches(const int index, const candidate& query, std::vector<group>& groups,
			                  const double radius, const float threshold) const
			{
				if (index < 0) return;
				const auto& current = _nodes[index];
				auto& group = groups[current.anchor];
				const auto distance = anchor_distance(query.vec, group.anchor);
				if (distance <= radius && similarity(query.vec, group.anchor) >= threshold)
					group.members.emplace_back(query.ref);
				if (distance - radius <= current.radius)
					find_matches(current.near_child, query, groups, radius, threshold);
				if (distance + radius >= current.radius)
					find_matches(current.far_child, query, groups, radius, threshold);
			}

		public:
			anchor_tree(std::vector<size_t> anchors, const std::vector<group>& groups) : _anchors(std::move(anchors))
			{
				_nodes.reserve(_anchors.size());
				if (!_anchors.empty()) build_node(_anchors, groups);
			}

			void find_matches(const candidate& query, std::vector<group>& groups, const float threshold) const
			{
				if (!_nodes.empty()) find_matches(0, query, groups,
					std::sqrt(std::max(0.0, 2.0 - 2.0 * threshold)) + 1e-5, threshold);
			}

			const std::vector<size_t>& anchors() const { return _anchors; }
			void find_nearest(const face_vector& query, const std::vector<group>& groups,
			                  nearest_anchors& nearest) const
			{
				if (!_nodes.empty()) find_nearest(0, query, groups, nearest);
			}
		};

		class anchor_forest
		{
			std::array<std::unique_ptr<anchor_tree>, 64> _levels;

		public:
			void insert(const size_t anchor, const std::vector<group>& groups)
			{
				std::vector<size_t> merged{anchor};
				for (auto& level : _levels)
				{
					if (!level)
					{
						level = std::make_unique<anchor_tree>(std::move(merged), groups);
						return;
					}
					merged.insert(merged.end(), level->anchors().begin(), level->anchors().end());
					level.reset();
				}
			}

			nearest_anchors find_nearest(const face_vector& query, const std::vector<group>& groups) const
			{
				nearest_anchors result;
				for (const auto& level : _levels)
				{
					if (level) level->find_nearest(query, groups, result);
				}
				return result;
			}
		};
	}

	void refine_groups(grouping_result& result, const df::hash_map<face_ref, face_vector, face_ref_hash>& vec_of,
	                   const tuning& t, const std::function<bool()>& cancel);

	grouping_result build_groups(std::vector<candidate> candidates, const tuning& t,
	                             const std::function<bool()>& cancel)
	{
		grouping_result result;
		anchor_forest anchors;
		if (cancel && cancel()) return {.cancelled = true};

		// Copied rather than pointed at: the candidates are moved from below, and refinement needs a
		// member's vector after that has happened.
		df::hash_map<face_ref, face_vector, face_ref_hash> vec_of;
		vec_of.reserve(candidates.size());
		for (const auto& c : candidates) vec_of[c.ref] = c.vec;

		std::vector<candidate> unplaced;
		unplaced.reserve(candidates.size());

		for (auto& c : candidates)
		{
			unplaced.emplace_back(std::move(c));
		}

		// The order decides which face becomes a leader, so it is total and derived only from the
		// faces themselves - never from enumeration order.
		std::ranges::sort(unplaced, [](const candidate& a, const candidate& b)
		{
			if (a.rank != b.rank) return a.rank > b.rank;
			const auto d = icmp(a.ref.path, b.ref.path);
			if (d != 0) return d < 0;
			return a.ref.index < b.ref.index;
		});

		for (const auto& c : unplaced)
		{
			if (cancel && cancel()) return {.cancelled = true};
			const auto nearest = anchors.find_nearest(c.vec, result.groups);
			const auto best = nearest.has_first ? similarity(c.vec, result.groups[nearest.first].anchor) : -2.0f;

			if (!result.groups.empty() && best >= t.similar)
			{
				result.groups[nearest.first].members.emplace_back(c.ref);
				continue;
			}

			// Similar to nothing that exists yet, so it anchors a group of its own. Its members are
			// then measured against it and never against each other, which is what stops one
			// ambiguous face bridging two groups.
			group g;
			g.anchor = c.vec;
			g.sample = c.ref;
			g.members.emplace_back(c.ref);
			result.groups.emplace_back(std::move(g));
			anchors.insert(result.groups.size() - 1, result.groups);
		}

		refine_groups(result, vec_of, t, cancel);
		if (result.cancelled) return {.cancelled = true};
		return result;
	}

	// Refine automatic anchors once, then keep them fixed while assigning their search contents.
	void refine_groups(grouping_result& result, const df::hash_map<face_ref, face_vector, face_ref_hash>& vec_of,
	                   const tuning& t, const std::function<bool()>& cancel)
	{
		if (result.groups.size() < 2) return;

		const auto recompute_anchors = [&result, &vec_of]
		{
			for (auto& g : result.groups)
			{
				if (g.members.empty()) continue;

				std::vector<face_vector> vectors;
				vectors.reserve(g.members.size());

				for (const auto& m : g.members)
				{
					if (const auto found = vec_of.find(m); found != vec_of.end())
					{
						vectors.emplace_back(found->second);
					}
				}

				if (!vectors.empty()) g.anchor = mean_vector(vectors);
			}
		};

		recompute_anchors();

		anchor_forest anchors;
		for (auto index = 0u; index < result.groups.size(); ++index)
		{
			if (cancel && cancel())
			{
				result.cancelled = true;
				return;
			}
			anchors.insert(index, result.groups);
		}

		std::vector<std::vector<face_ref>> moved(result.groups.size());
		std::vector<group> unmatched;

		for (auto i = 0u; i < result.groups.size(); ++i)
		{
			if (cancel && cancel())
			{
				result.cancelled = true;
				return;
			}
			for (const auto& m : result.groups[i].members)
			{
				if (cancel && cancel())
				{
					result.cancelled = true;
					return;
				}
				const auto found = vec_of.find(m);

				if (found == vec_of.end())
				{
					moved[i].emplace_back(m);
					continue;
				}

				const auto nearest = anchors.find_nearest(found->second, result.groups);
				const auto target = nearest.has_first ? nearest.first : i;
				const auto best = nearest.has_first
					                  ? similarity(found->second, result.groups[target].anchor)
					                  : -2.0f;

				// Only a decisive improvement moves a face. Without the threshold test a face would
				// chase whichever anchor drifted closest, and membership would depend on the order the
				// groups happened to be visited.
				if (best >= t.similar) moved[target].emplace_back(m);
				else
				{
					group separate;
					separate.anchor = found->second;
					separate.sample = m;
					separate.members.emplace_back(m);
					unmatched.emplace_back(std::move(separate));
				}
			}
		}

		for (auto i = 0u; i < result.groups.size(); ++i) result.groups[i].members = std::move(moved[i]);

		for (auto& separate : unmatched) result.groups.emplace_back(std::move(separate));

		std::erase_if(result.groups, [](const group& g) { return g.members.empty(); });
	}

	bool match_group_members(std::vector<group>& groups, const std::span<const candidate> candidates,
	                         const tuning& t, const std::function<bool()>& cancel)
	{
		if (cancel && cancel()) return false;
		std::vector<size_t> indices(groups.size());
		std::iota(indices.begin(), indices.end(), size_t{0});
		const anchor_tree anchors(std::move(indices), groups);
		for (auto& group : groups) group.members.clear();
		for (const auto& candidate : candidates)
		{
			if (cancel && cancel()) return false;
			anchors.find_matches(candidate, groups, t.similar);
		}
		return true;
	}

	double sample_score(const candidate& c, const face_vector& anchor)
	{
		// Typicality first: the sample is a visual index AND the group's reference, so it must look
		// like the group rather than like its most photogenic outlier.
		const auto typical = std::clamp((similarity(c.vec, anchor) + 1.0) * 0.5, 0.0, 1.0);

		// Absolute pixels, not a fraction of the frame. candidate_rank measures the frame fraction,
		// which barely varies because a face is rarely much of a photograph, so a tiny blurred face
		// can out-rank a large sharp one. Below the aligned extent the crop is an upsample; past twice
		// it there is more detail than a tile can show, so the term saturates.
		const auto crop_edge = std::min(c.crop.Width, c.crop.Height);
		const auto detail = std::clamp(crop_edge / (aligned_extent * 2.0), 0.0, 1.0);

		// Pose carries the most weight of the three: among faces that are all typical of the group and
		// all big enough, the square-on one is the one a user recognises and the one the embedding of
		// every other member was measured most reliably against.
		return typical * 0.35 + c.frontality * 0.45 + detail * 0.2;
	}

	const candidate* sample_face(const group& g, const candidate_lookup& lookup, const tuning& t)
	{
		// A profile makes a poor tile and a worse reference, so a group passes over one entirely while
		// any member is facing the camera. Only a group of nothing but profiles settles for one.
		const auto pick = [&g, &lookup](const double frontality_floor, const bool require_unique_name)
			-> const candidate*
		{
			const candidate* best = nullptr;
			auto best_score = 0.0;

			for (const auto& member : g.members)
			{
				const auto* const found = lookup(member);
				if (!found) continue;
				if (found->frontality < frontality_floor) continue;
				if (require_unique_name && !found->unique_name) continue;

				const auto score = sample_score(*found, g.anchor);

				// Ties break on the path and then the index, so the tile a user learns to recognise
				// does not change because the index enumerated the folder differently.
				if (!best || score > best_score ||
					(score == best_score && (icmp(member.path, best->ref.path) < 0 ||
						(icmp(member.path, best->ref.path) == 0 && member.index < best->ref.index))))
				{
					best = found;
					best_score = score;
				}
			}

			return best;
		};

		// A face term spells a file name, so a sample whose name is shared with another file asks for
		// two faces. Preferring an unambiguous one keeps the count on the tile and the results of
		// clicking it the same number; it is a preference and not a requirement, because a group every
		// one of whose files repeats a name still has to be shown.
		if (const auto* const found = pick(t.min_sample_frontality, true)) return found;
		if (const auto* const found = pick(t.min_sample_frontality, false)) return found;
		if (const auto* const found = pick(0.0, true)) return found;
		return pick(0.0, false);
	}

	// Defined below, and called by adoption: a face that no sample reaches anchors a group of its own.
	bool rescue_unmatched_faces(std::vector<group>& groups, std::span<const candidate> candidates,
	                            const tuning& t, const std::function<bool()>& cancel);

	bool adopt_sample_faces(std::vector<group>& groups, const std::span<const candidate> candidates,
	                        const candidate_lookup& lookup, const tuning& t,
	                        const std::function<bool()>& cancel)
	{
		if (cancel && cancel()) return false;

		std::vector<group> adopted;
		adopted.reserve(groups.size());
		df::hash_set<face_ref, face_ref_hash> samples;

		for (const auto& g : groups)
		{
			if (cancel && cancel()) return false;

			const auto* const best = sample_face(g, lookup, t);
			if (!best) continue;

			// Two clusters that settle on one face are one group: they would draw the same tile, spell
			// the same term and return the same files, so the second is a duplicate of the first.
			if (!samples.emplace(best->ref).second) continue;

			group settled;
			settled.sample = best->ref;
			settled.anchor = best->vec;
			adopted.emplace_back(std::move(settled));
		}

		groups = std::move(adopted);

		// Membership is re-measured against the sample's own vector, so what the palette counts is
		// exactly what a search for that sample returns.
		if (!match_group_members(groups, candidates, t, cancel)) return false;

		// Re-anchoring can leave a face behind. Refinement only promises every member is within the
		// threshold of its group's MEAN, and two members either side of that mean can be twice as far
		// apart as the threshold allows; a sample is a real face on one side of its cluster, so a face
		// on the far side can fall outside every sample there is. A face in no group has no tile, no
		// caption and no term - nothing in the product can reach it - so it anchors a group of its
		// own, exactly as an unmatched face does during refinement.
		return rescue_unmatched_faces(groups, candidates, t, cancel);
	}

	// Deterministic leader order, the same one build_groups uses, so which of several stranded faces
	// leads a rescued group does not depend on enumeration.
	bool rescue_unmatched_faces(std::vector<group>& groups, const std::span<const candidate> candidates,
	                            const tuning& t, const std::function<bool()>& cancel)
	{
		if (cancel && cancel()) return false;

		df::hash_set<face_ref, face_ref_hash> covered;
		for (const auto& g : groups)
		{
			for (const auto& m : g.members) covered.emplace(m);
		}

		std::vector<const candidate*> stranded;
		for (const auto& c : candidates)
		{
			if (!covered.contains(c.ref)) stranded.emplace_back(&c);
		}

		if (stranded.empty()) return true;

		std::ranges::sort(stranded, [](const candidate* a, const candidate* b)
		{
			if (a->rank != b->rank) return a->rank > b->rank;
			const auto d = icmp(a->ref.path, b->ref.path);
			if (d != 0) return d < 0;
			return a->ref.index < b->ref.index;
		});

		std::vector<group> rescued;

		for (const auto* const c : stranded)
		{
			if (cancel && cancel()) return false;

			// A stranded face that the previous rescue already reaches is a member of it, not a group
			// of its own. Two faces that resemble each other do not need two tiles.
			const auto reached = std::ranges::any_of(rescued, [c, &t](const group& g)
			{
				return similarity(c->vec, g.anchor) >= t.similar;
			});
			if (reached) continue;

			group separate;
			separate.sample = c->ref;
			separate.anchor = c->vec;
			rescued.emplace_back(std::move(separate));
		}

		// Membership through the same predicate as every other group, so a rescued group counts what a
		// search for its sample returns.
		if (!match_group_members(rescued, candidates, t, cancel)) return false;

		groups.insert(groups.end(), std::make_move_iterator(rescued.begin()),
		              std::make_move_iterator(rescued.end()));
		return true;
	}

	std::string format_face_token(const std::string_view file_name, const int index)
	{
		if (file_name.empty()) return {};
		// `:1` is left off: most pictures hold one face, so the file's name is the whole of what there
		// is to say about them, and a term nobody has to explain is worth more than a uniform one.
		return index <= 0 ? std::string(file_name) : std::format("{}:{}", file_name, index + 1);
	}

	bool parse_face_token(const std::string_view text, face_token& result)
	{
		const auto trimmed = str::trim(text);
		if (trimmed.empty()) return false;

		// Read from the right: a file name may hold dots and, on some file systems, colons, but a
		// position is the last colon followed by nothing but digits.
		const auto colon = trimmed.rfind(':');

		if (colon != std::string_view::npos && colon + 1 < trimmed.size())
		{
			const auto digits = trimmed.substr(colon + 1);

			if (std::ranges::all_of(digits, [](const char c) { return c >= '0' && c <= '9'; }))
			{
				const auto position = str::to_int(digits);
				const auto name = str::trim(trimmed.substr(0, colon));
				if (name.empty() || position < 1) return false;
				result.name = std::string(name);
				result.index = position - 1;
				return true;
			}
		}

		result.name = std::string(trimmed);
		result.index = 0;
		return true;
	}

	void order_faces(std::vector<df::stored_face>& faces)
	{
		std::ranges::sort(faces, [](const df::stored_face& a, const df::stored_face& b)
		{
			// Reading order over the upright picture, which is the order a person would count the
			// faces in it. Rounded to whole pixels first so that two boxes differing by a fraction do
			// not swap when the same file is read again at a different bound.
			const auto ax = df::round(a.bounds.X);
			const auto bx = df::round(b.bounds.X);
			if (ax != bx) return ax < bx;

			const auto ay = df::round(a.bounds.Y);
			const auto by = df::round(b.bounds.Y);
			if (ay != by) return ay < by;

			const auto aw = df::round(a.bounds.Width);
			const auto bw = df::round(b.bounds.Width);
			if (aw != bw) return aw > bw;

			const auto ah = df::round(a.bounds.Height);
			const auto bh = df::round(b.bounds.Height);
			return ah > bh;
		});
	}


	rectd normalized_face_locator(const rectd bounds, const sizei source_extent)
	{
		if (source_extent.cx <= 0 || source_extent.cy <= 0) return {};
		return {bounds.X / source_extent.cx, bounds.Y / source_extent.cy,
		        bounds.Width / source_extent.cx, bounds.Height / source_extent.cy};
	}

	void order_palette(std::vector<palette_entry>& entries)
	{
		std::ranges::sort(entries, [](const palette_entry& a, const palette_entry& b)
		{
			if (a.file_count != b.file_count) return a.file_count > b.file_count;

			// Everything below is a tiebreak, so two entries of the same size do not swap places
			// between sessions. The sample face is what a group IS, so it is what breaks them.
			const auto d = icmp(a.sample.path, b.sample.path);
			if (d != 0) return d < 0;
			return a.sample.index < b.sample.index;
		});
	}

	void order_by_similarity(std::vector<palette_entry>& entries)
	{
		if (entries.size() < 2) return;

		// Ordering is the same problem one level up, so it runs the same anchored construction over the
		// groups' own anchors rather than inventing a second clusterer to disagree with the first.
		//
		// Deliberately looser than `similar`: this decides what to put NEXT TO what, not what anything
		// is, and a wrong neighbour costs a glance where a wrong group costs a search.
		auto t = defaults();
		t.similar = 0.45f;

		std::vector<candidate> anchors;
		anchors.reserve(entries.size());
		df::hash_map<face_ref, size_t, face_ref_hash> entry_of;

		for (auto i = 0u; i < entries.size(); ++i)
		{
			candidate c;
			// The sample identifies the entry: it is already unique per group and it is what a group
			// is resolved through everywhere else.
			c.ref = entries[i].sample;
			c.vec = entries[i].anchor;
			// Rank decides which group leads a neighbourhood, and the biggest is the one a user is
			// looking for.
			c.rank = entries[i].file_count;
			entry_of[c.ref] = i;
			anchors.emplace_back(std::move(c));
		}

		const auto grouped = build_groups(std::move(anchors), t);

		struct neighbourhood
		{
			std::vector<size_t> members;
			int total = 0;
		};

		std::vector<neighbourhood> neighbourhoods;
		neighbourhoods.reserve(grouped.groups.size());
		df::hash_set<size_t> placed;

		for (const auto& g : grouped.groups)
		{
			neighbourhood n;

			for (const auto& m : g.members)
			{
				const auto found = entry_of.find(m);
				if (found == entry_of.end() || !placed.emplace(found->second).second) continue;
				n.members.emplace_back(found->second);
				n.total += entries[found->second].file_count;
			}

			if (!n.members.empty()) neighbourhoods.emplace_back(std::move(n));
		}

		// Anything the grouper did not place still has to appear: a face the view does not list is a
		// face nobody can search for.
		for (auto i = 0u; i < entries.size(); ++i)
		{
			if (!placed.emplace(i).second) continue;
			neighbourhoods.emplace_back(neighbourhood{{i}, entries[i].file_count});
		}

		std::ranges::stable_sort(neighbourhoods, [](const neighbourhood& a, const neighbourhood& b)
		{
			return a.total > b.total;
		});

		std::vector<palette_entry> ordered;
		ordered.reserve(entries.size());

		for (const auto& n : neighbourhoods)
		{
			for (const auto i : n.members) ordered.emplace_back(std::move(entries[i]));
		}

		entries = std::move(ordered);
	}

	recti crop_in(const rectd crop, const sizei crop_space, const sizei dims)
	{
		if (crop_space.is_empty() || dims.is_empty()) return {};

		const auto scale_x = static_cast<double>(dims.cx) / crop_space.cx;
		const auto scale_y = static_cast<double>(dims.cy) / crop_space.cy;

		const auto to_int = [](const double v) { return static_cast<int>(std::round(v)); };
		const auto left = to_int(std::clamp(crop.X * scale_x, 0.0, static_cast<double>(dims.cx - 1)));
		const auto top = to_int(std::clamp(crop.Y * scale_y, 0.0, static_cast<double>(dims.cy - 1)));
		const auto right = to_int(std::clamp((crop.X + crop.Width) * scale_x, static_cast<double>(left + 1),
		                                     static_cast<double>(dims.cx)));
		const auto bottom = to_int(std::clamp((crop.Y + crop.Height) * scale_y, static_cast<double>(top + 1),
		                                      static_cast<double>(dims.cy)));

		return {left, top, right, bottom};
	}

	std::vector<palette_entry> select_sidebar_palette(const std::span<const palette_entry> entries)
	{
		std::vector<palette_entry> result;
		result.reserve(std::min(entries.size(), static_cast<size_t>(max_sidebar_faces)));
		const auto displayed_vector = [](const palette_entry& entry) -> const face_vector&
		{
			const auto index = entry.sample.index;
			if (entry.source && index >= 0 && static_cast<size_t>(index) < entry.source->faces.size())
				return entry.source->faces[index].vec;
			return entry.anchor;
		};

		for (const auto& entry : entries)
		{
			if (entry.file_count <= 0) continue;
			const auto duplicate = std::ranges::any_of(result, [&entry, &displayed_vector](const palette_entry& kept)
			{
				return (!str::is_empty(entry.sample.path) && entry.sample == kept.sample) ||
					similarity(entry.anchor, kept.anchor) >= 0.45f ||
					similarity(displayed_vector(entry), displayed_vector(kept)) >= 0.45f;
			});
			if (duplicate) continue;
			result.emplace_back(entry);
			if (result.size() == max_sidebar_faces) break;
		}
		return result;
	}

}

namespace df
{
	namespace
	{
		constexpr uint32_t face_blob_magic = 0x53464644; // 'DFFS'
		constexpr uint16_t face_blob_version = 1;

		// score, four box edges, ten landmark values, and the vector.
		constexpr size_t face_record_floats = 1 + 4 + (faces::landmark_count * 2) + faces::vector_size;
		constexpr size_t face_record_bytes = face_record_floats * sizeof(float);
		constexpr size_t face_blob_header_bytes = 4 + 2 + 2 + 4 + 8 + 4 + 4 + 4;

		template <typename T>
		void write_pod(blob& b, const T v)
		{
			const auto* const p = std::bit_cast<const uint8_t*>(&v);
			b.insert(b.end(), p, p + sizeof(T));
		}

		template <typename T>
		T read_pod(const uint8_t*& p)
		{
			T v{};
			memcpy(&v, p, sizeof(T));
			p += sizeof(T);
			return v;
		}
	}

	// Self-describing in the same shape as the date pack: the reader takes the prefix it understands
	// and steps by the stride the writer recorded, so a later release can append a per-face field
	// without forcing the whole collection to be detected again.
	blob encode_faces(const face_set& fs)
	{
		blob result;
		result.reserve(face_blob_header_bytes + (fs.faces.size() * face_record_bytes));

		write_pod(result, face_blob_magic);
		write_pod(result, face_blob_version);
		write_pod(result, static_cast<uint16_t>(face_record_bytes));
		write_pod(result, fs.model_version);
		write_pod(result, fs.scanned.to_int64());
		write_pod(result, static_cast<int32_t>(fs.source_extent.cx));
		write_pod(result, static_cast<int32_t>(fs.source_extent.cy));
		write_pod(result, static_cast<uint32_t>(fs.faces.size()));

		for (const auto& f : fs.faces)
		{
			write_pod(result, f.score);
			write_pod(result, static_cast<float>(f.bounds.X));
			write_pod(result, static_cast<float>(f.bounds.Y));
			write_pod(result, static_cast<float>(f.bounds.Width));
			write_pod(result, static_cast<float>(f.bounds.Height));

			for (const auto& m : f.marks)
			{
				write_pod(result, static_cast<float>(m.X));
				write_pod(result, static_cast<float>(m.Y));
			}

			for (const auto v : f.vec) write_pod(result, v);

		}

		return result;
	}

	face_set decode_faces(const cspan data)
	{
		face_set result;

		if (data.size < face_blob_header_bytes) return result;

		const auto* p = data.data;

		if (read_pod<uint32_t>(p) != face_blob_magic) return result;
		if (read_pod<uint16_t>(p) < face_blob_version) return result;

		const size_t stride = read_pod<uint16_t>(p);
		result.model_version = read_pod<uint32_t>(p);
		result.scanned = date_t(static_cast<uint64_t>(read_pod<int64_t>(p)));
		result.source_extent.cx = read_pod<int32_t>(p);
		result.source_extent.cy = read_pod<int32_t>(p);
		const size_t count = read_pod<uint32_t>(p);
		if (result.source_extent.cx < 0 || result.source_extent.cy < 0) return {};
		if (count > 0 && (result.source_extent.cx == 0 || result.source_extent.cy == 0)) return {};

		// A stride shorter than this build's record is a blob written to a narrower layout than the
		// version claims. Refusing it costs a re-detection; reading it would walk off the records.
		if (stride < face_record_bytes) return {};

		const auto available = data.size - face_blob_header_bytes;

		if (count > available / stride)
		{
			// Truncated. The row is treated as unscanned rather than half-read, so the pass fills it
			// again instead of publishing faces that are not all there.
			return {};
		}

		result.faces.reserve(count);

		for (size_t i = 0; i < count; ++i)
		{
			const auto* record = p + (i * stride);

			stored_face f;
			f.score = read_pod<float>(record);
			const auto x = read_pod<float>(record);
			const auto y = read_pod<float>(record);
			const auto w = read_pod<float>(record);
			const auto h = read_pod<float>(record);
			f.bounds = rectd{x, y, w, h};

			for (auto& m : f.marks)
			{
				m.X = read_pod<float>(record);
				m.Y = read_pod<float>(record);
			}

			for (auto& v : f.vec) v = read_pod<float>(record);


			const auto finite_rect = std::isfinite(f.bounds.X) && std::isfinite(f.bounds.Y) &&
				std::isfinite(f.bounds.Width) && std::isfinite(f.bounds.Height) &&
				f.bounds.Width > 0 && f.bounds.Height > 0 &&
				f.bounds.X >= -result.source_extent.cx && f.bounds.Y >= -result.source_extent.cy &&
				f.bounds.X + f.bounds.Width <= result.source_extent.cx * 2.0 &&
				f.bounds.Y + f.bounds.Height <= result.source_extent.cy * 2.0;
			const auto finite_marks = std::ranges::all_of(f.marks, [](const pointd mark)
			{
				return std::isfinite(mark.X) && std::isfinite(mark.Y);
			});
			const auto finite_vector = std::ranges::all_of(f.vec, [](const float value)
			{
				return std::isfinite(value);
			});
			if (!std::isfinite(f.score) || !finite_rect || !finite_marks || !finite_vector) return {};

			result.faces.emplace_back(std::move(f));
		}

		return result;
	}
}
