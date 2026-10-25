// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Photo editing view. Implements pixel adjustments and comparison.

#include "pch.h"
#include "model_index.h"
#include "view_edit.h"
#include "app_text.h"
#include "files.h"
#include "ui_controls.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class log_slider_control final : public view_element, public std::enable_shared_from_this<log_slider_control>
{
	ui::trackbar_ptr _slider;
	ui::edit_ptr _edit;
	std::string _label;
	ui::slider_row_layout _row;

	double& _val;
	double _min;
	double _max;

	bool _tracking;
	std::function<void()> _changed;

	constexpr static int Range = 100;

public:
	log_slider_control(ui::control_frame_ptr& h, const std::string_view text, double& v, double min, double max,
	                   std::function<void()> changed) noexcept :
		_label(text),
		_val(v),
		_min(min),
		_max(max),
		_tracking(false),
		_changed(std::move(changed))
	{
		ui::edit_styles style;
		style.align_center = true;
		_edit = h->create_edit(style, {}, [this](const std::string_view text) { edit_change(text); });
		_slider = h->create_slider(-Range, Range,
		                           [this](const int pos, const bool is_tracking) { slider_change(pos, is_tracking); });
	}

	void label(const std::string_view label)
	{
		_label = label;
	}

	void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler) override
	{
		handler(_slider);
		handler(_edit);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::populate)
		{
			update_slider();
			update_edit();
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, ui::layout_slider_row(mc, _label, {0, 0, width_limit, 0}).height};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		_row = ui::layout_slider_row(mc, _label, bounds);
		positions.emplace_back(_slider, _row.slider, is_visible());
		positions.emplace_back(_edit, _row.edit, is_visible());
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		if (!str::is_empty(_label))
		{
			const auto text_color = ui::color(dc.colors.foreground, dc.colors.alpha);
			dc.draw_text(_label, _row.label.offset(element_offset), ui::style::font_face::dialog,
			             _row.stacked ? ui::style::text_style::multiline : ui::style::text_style::single_line,
			             text_color, {});
		}
	}

	int value_to_slider(const double a) const
	{
		const auto d = a / _max;
		return df::round(sqrt(fabs(d)) * (a < 0 ? -Range : Range));
	}

	double slider_to_value(const double s) const
	{
		const auto d = s / Range;
		return d * d * (s < 0 ? _min : _max);
	}

	void update_edit() const
	{
		const auto actual = _edit->window_text();
		const auto expected = std::format("{:.2f}", _val);

		if (actual.empty() || actual != expected)
		{
			_edit->window_text(expected);
		}
	}

	void update_slider() const
	{
		if (!df::equiv(_val, slider_to_value(_slider->get_pos())))
		{
			_slider->SetPos(value_to_slider(_val));
		}
	}

	void edit_change(const std::string_view text)
	{
		_val = std::clamp(str::to_double(text), _min, _max);
		update_slider();
		_changed();
	}

	void slider_change(const int pos, const bool is_tracking)
	{
		_tracking = is_tracking;
		_val = slider_to_value(pos);
		update_edit();
		_changed();
	}

	bool is_tracking() const
	{
		return _tracking;
	}
};

namespace
{
	constexpr double detection_pi = 3.14159265358979323846;
	constexpr double minimum_document_confidence = 0.55;
	constexpr double clear_document_confidence = 0.80;

	struct document_candidate
	{
		std::array<pointd, 4> corners{};
		double confidence = 0;

		explicit operator bool() const { return confidence > 0; }
	};

	// Corners must be ordered top-left, top-right, bottom-right, bottom-left in screen space.
	double convex_quad_area(const std::array<pointd, 4>& corners)
	{
		double area = 0;

		for (auto index = 0; index < 4; ++index)
		{
			const auto& a = corners[index];
			const auto& b = corners[(index + 1) % 4];
			const auto& c = corners[(index + 2) % 4];
			if ((b.X - a.X) * (c.Y - b.Y) - (b.Y - a.Y) * (c.X - b.X) <= 0) return 0;
			area += a.X * b.Y - a.Y * b.X;
		}

		return std::abs(area) * 0.5;
	}
}

// Segments a bright low-chroma page from a darker surround. Requires tonal separation to succeed.
static document_candidate detect_document_by_region(const ui::const_surface_ptr& surface,
                                                    const std::vector<float>& luminance, const int width,
                                                    const int height)
{
	document_candidate candidate;
	const auto pixel_count = static_cast<size_t>(width) * height;
	std::array<uint32_t, 256> histogram{};

	for (const auto value : luminance)
	{
		++histogram[std::clamp(df::round(value * 255), 0, 255)];
	}

	const auto percentile = [&](const double fraction)
	{
		const auto target = static_cast<size_t>(pixel_count * fraction);
		size_t count = 0;
		for (auto index = 0; index < 256; ++index)
		{
			count += histogram[index];
			if (count >= target) return index / 255.0;
		}
		return 1.0;
	};

	const auto threshold = std::clamp((percentile(0.50) + percentile(0.90)) * 0.5, 0.55, 0.88);
	std::vector<uint8_t> mask(pixel_count);
	for (auto y = 0; y < height; ++y)
	{
		const auto* pixels = std::bit_cast<const ui::color32*>(surface->pixels_line(y));
		for (auto x = 0; x < width; ++x)
		{
			const auto color = pixels[x];
			const auto maximum = std::max({ui::get_r(color), ui::get_g(color), ui::get_b(color)});
			const auto minimum = std::min({ui::get_r(color), ui::get_g(color), ui::get_b(color)});
			const auto chroma = (maximum - minimum) / 255.0;
			const auto index = static_cast<size_t>(y) * width + x;
			mask[index] = luminance[index] >= threshold && chroma <= 0.28 ? 1 : 0;
		}
	}

	std::vector<uint8_t> expanded(pixel_count);
	for (auto y = 0; y < height; ++y)
	{
		for (auto x = 0; x < width; ++x)
		{
			for (auto yy = (std::max)(0, y - 2); yy <= (std::min)(height - 1, y + 2) && !expanded[static_cast<size_t>(y)
				     * width + x]; ++yy)
				for (auto xx = (std::max)(0, x - 2); xx <= (std::min)(width - 1, x + 2); ++xx)
					if (mask[static_cast<size_t>(yy) * width + xx])
					{
						expanded[static_cast<size_t>(y) * width + x] = 1;
						break;
					}
		}
	}
	for (auto y = 2; y < height - 2; ++y)
	{
		for (auto x = 2; x < width - 2; ++x)
		{
			auto solid = true;
			for (auto yy = y - 2; yy <= y + 2 && solid; ++yy)
				for (auto xx = x - 2; xx <= x + 2; ++xx)
					if (!expanded[static_cast<size_t>(yy) * width + xx])
					{
						solid = false;
						break;
					}
			mask[static_cast<size_t>(y) * width + x] = solid ? 1 : 0;
		}
	}

	std::vector<int> labels(pixel_count, -1);
	std::vector<int> queue;
	std::vector<int> best_component;
	for (auto start = 0; start < static_cast<int>(pixel_count); ++start)
	{
		if (!mask[start] || labels[start] != -1) continue;
		queue.clear();
		queue.push_back(start);
		labels[start] = start;
		for (size_t position = 0; position < queue.size(); ++position)
		{
			const auto index = queue[position];
			const auto x = index % width;
			const auto y = index / width;
			const std::array<int, 4> neighbors = {index - 1, index + 1, index - width, index + width};
			for (auto neighbor : neighbors)
			{
				if (neighbor < 0 || neighbor >= static_cast<int>(pixel_count)) continue;
				const auto nx = neighbor % width;
				if (std::abs(nx - x) + std::abs(neighbor / width - y) != 1) continue;
				if (mask[neighbor] && labels[neighbor] == -1)
				{
					labels[neighbor] = start;
					queue.push_back(neighbor);
				}
			}
		}
		if (queue.size() > best_component.size()) best_component = queue;
	}
	if (best_component.empty()) return candidate;

	std::vector<uint8_t> outside(pixel_count);
	queue.clear();
	const auto enqueue_outside = [&](const int index)
	{
		if (!mask[index] && !outside[index])
		{
			outside[index] = 1;
			queue.push_back(index);
		}
	};
	for (auto x = 0; x < width; ++x)
	{
		enqueue_outside(x);
		enqueue_outside((height - 1) * width + x);
	}
	for (auto y = 0; y < height; ++y)
	{
		enqueue_outside(y * width);
		enqueue_outside(y * width + width - 1);
	}
	for (size_t position = 0; position < queue.size(); ++position)
	{
		const auto index = queue[position];
		const auto x = index % width;
		const auto y = index / width;
		const std::array<int, 4> neighbors = {index - 1, index + 1, index - width, index + width};
		for (const auto neighbor : neighbors)
		{
			if (neighbor < 0 || neighbor >= static_cast<int>(pixel_count)) continue;
			if (std::abs(neighbor % width - x) + std::abs(neighbor / width - y) != 1) continue;
			if (!mask[neighbor] && !outside[neighbor])
			{
				outside[neighbor] = 1;
				queue.push_back(neighbor);
			}
		}
	}

	const auto best_label = labels[best_component.front()];
	for (auto index = 0; index < static_cast<int>(pixel_count); ++index)
	{
		if (!outside[index] && labels[index] != best_label)
		{
			labels[index] = best_label;
			best_component.push_back(index);
		}
	}

	const auto area_fraction = best_component.size() / static_cast<double>(pixel_count);
	if (area_fraction < 0.18 || area_fraction > 0.92) return candidate;

	pointd top_left(width, height);
	pointd top_right(0, height);
	pointd bottom_right(0, 0);
	pointd bottom_left(width, 0);
	double min_sum = width + height;
	double max_sum = 0;
	double max_difference = -height;
	double min_difference = width;
	for (const auto index : best_component)
	{
		const auto x = static_cast<double>(index % width) + 0.5;
		const auto y = static_cast<double>(index / width) + 0.5;
		if (x + y < min_sum)
		{
			min_sum = x + y;
			top_left = {x, y};
		}
		if (x + y > max_sum)
		{
			max_sum = x + y;
			bottom_right = {x, y};
		}
		if (x - y > max_difference)
		{
			max_difference = x - y;
			top_right = {x, y};
		}
		if (x - y < min_difference)
		{
			min_difference = x - y;
			bottom_left = {x, y};
		}
	}

	const std::array<pointd, 4> corners = {top_left, top_right, bottom_right, bottom_left};
	const auto polygon_area = convex_quad_area(corners);
	if (polygon_area / pixel_count < 0.16) return candidate;

	double edge_support = 0;
	for (auto edge = 0; edge < 4; ++edge)
	{
		const auto a = corners[edge];
		const auto b = corners[(edge + 1) % 4];
		auto supported = 0;
		constexpr auto samples = 64;
		for (auto sample = 0; sample < samples; ++sample)
		{
			const auto position = (sample + 0.5) / samples;
			const auto x = std::clamp(df::round(a.X + (b.X - a.X) * position), 2, width - 3);
			const auto y = std::clamp(df::round(a.Y + (b.Y - a.Y) * position), 2, height - 3);
			bool found = false;
			for (auto yy = y - 2; yy <= y + 2 && !found; ++yy)
				for (auto xx = x - 2; xx <= x + 2; ++xx)
					if (labels[static_cast<size_t>(yy) * width + xx] == labels[best_component.front()]) found = true;
			supported += found ? 1 : 0;
		}
		edge_support += supported / static_cast<double>(samples);
	}
	edge_support /= 4.0;
	if (edge_support < 0.82) return candidate;

	candidate.corners = corners;
	candidate.confidence = std::clamp(edge_support * 0.55 + area_fraction * 0.45, 0.0, 1.0);
	return candidate;
}

// Recovers the page outline from the dominant straight edges. Works where page and surround share a tone,
// because gradient direction and line continuity carry the border even when the tonal step is small.
static document_candidate detect_document_by_edges(const std::vector<float>& luminance, const int width,
                                                   const int height)
{
	document_candidate candidate;
	if (width < 64 || height < 64) return candidate;

	const auto pixel_count = static_cast<size_t>(width) * height;
	std::vector<float> magnitude(pixel_count, 0.0f);
	std::vector<float> orientation(pixel_count, 0.0f);
	const auto at = [&](const int x, const int y)
	{
		return luminance[static_cast<size_t>(y) * width + x];
	};

	float maximum_magnitude = 0;
	for (auto y = 1; y < height - 1; ++y)
	{
		for (auto x = 1; x < width - 1; ++x)
		{
			const auto gx = -at(x - 1, y - 1) - 2.0f * at(x - 1, y) - at(x - 1, y + 1) +
				at(x + 1, y - 1) + 2.0f * at(x + 1, y) + at(x + 1, y + 1);
			const auto gy = -at(x - 1, y - 1) - 2.0f * at(x, y - 1) - at(x + 1, y - 1) +
				at(x - 1, y + 1) + 2.0f * at(x, y + 1) + at(x + 1, y + 1);
			const auto index = static_cast<size_t>(y) * width + x;
			magnitude[index] = std::hypot(gx, gy);
			orientation[index] = std::atan2(gy, gx);
			maximum_magnitude = (std::max)(maximum_magnitude, magnitude[index]);
		}
	}
	if (maximum_magnitude < 0.02f) return candidate;

	// Relative threshold so a faint page border survives; capped so texture cannot flood the accumulator.
	std::array<uint32_t, 256> magnitude_histogram{};
	for (const auto value : magnitude)
	{
		++magnitude_histogram[std::clamp(df::round(value / maximum_magnitude * 255), 0, 255)];
	}
	const auto strong_target = static_cast<size_t>(pixel_count * 0.06);
	size_t strong_count = 0;
	auto strong_bin = 0;
	for (auto bin = 255; bin >= 0; --bin)
	{
		strong_count += magnitude_histogram[bin];
		if (strong_count >= strong_target)
		{
			strong_bin = bin;
			break;
		}
	}
	const auto edge_threshold = (std::max)(maximum_magnitude * 0.06f,
	                                       maximum_magnitude * static_cast<float>(strong_bin) / 255.0f);

	constexpr auto theta_steps = 180;
	constexpr auto theta_window = 4;
	const auto diagonal = static_cast<int>(std::ceil(std::hypot(width, height)));
	const auto rho_bins = diagonal * 2 + 1;
	std::array<double, theta_steps> cos_table{};
	std::array<double, theta_steps> sin_table{};
	for (auto step = 0; step < theta_steps; ++step)
	{
		const auto theta = step * detection_pi / theta_steps;
		cos_table[step] = std::cos(theta);
		sin_table[step] = std::sin(theta);
	}

	std::vector<float> accumulator(static_cast<size_t>(theta_steps) * rho_bins, 0.0f);
	auto edge_count = 0;
	for (auto y = 1; y < height - 1; ++y)
	{
		for (auto x = 1; x < width - 1; ++x)
		{
			const auto index = static_cast<size_t>(y) * width + x;
			const auto weight = magnitude[index];
			if (weight < edge_threshold) continue;
			++edge_count;

			const auto center = static_cast<int>(std::lround(orientation[index] * theta_steps / detection_pi));
			for (auto offset = -theta_window; offset <= theta_window; ++offset)
			{
				const auto step = ((center + offset) % theta_steps + theta_steps) % theta_steps;
				const auto rho = x * cos_table[step] + y * sin_table[step];
				const auto bin = std::clamp(static_cast<int>(std::lround(rho)) + diagonal, 0, rho_bins - 1);
				accumulator[static_cast<size_t>(step) * rho_bins + bin] += weight;
			}
		}
	}
	if (edge_count < width + height) return candidate;

	struct hough_line
	{
		double theta = 0;
		double rho = 0;
		double votes = 0;
	};

	// Page borders sit within 35 degrees of an image axis; anything more oblique is scene clutter.
	const auto collect_lines = [&](const bool vertical)
	{
		const auto in_family = [vertical](const int step)
		{
			return vertical ? step <= 35 || step >= 145 : step >= 55 && step <= 125;
		};

		double family_maximum = 0;
		for (auto step = 0; step < theta_steps; ++step)
		{
			if (!in_family(step)) continue;
			for (auto bin = 0; bin < rho_bins; ++bin)
			{
				family_maximum = (std::max)(family_maximum,
				                            static_cast<double>(accumulator[static_cast<size_t>(step) * rho_bins +
					                            bin]));
			}
		}

		std::vector<hough_line> peaks;
		if (family_maximum <= 0) return peaks;
		const auto minimum_votes = family_maximum * 0.20;

		for (auto step = 0; step < theta_steps; ++step)
		{
			if (!in_family(step)) continue;
			for (auto bin = 3; bin < rho_bins - 3; ++bin)
			{
				const auto votes = static_cast<double>(accumulator[static_cast<size_t>(step) * rho_bins + bin]);
				if (votes < minimum_votes) continue;

				auto is_peak = true;
				for (auto dt = -2; dt <= 2 && is_peak; ++dt)
				{
					const auto neighbor = ((step + dt) % theta_steps + theta_steps) % theta_steps;
					for (auto dr = -3; dr <= 3; ++dr)
					{
						if (dt == 0 && dr == 0) continue;
						if (accumulator[static_cast<size_t>(neighbor) * rho_bins + bin + dr] > votes)
						{
							is_peak = false;
							break;
						}
					}
				}
				if (!is_peak) continue;

				auto theta = step * detection_pi / theta_steps;
				auto rho = static_cast<double>(bin - diagonal);
				if (vertical && step > 90)
				{
					theta -= detection_pi;
					rho = -rho;
				}
				peaks.emplace_back(theta, rho, votes);
			}
		}

		std::ranges::sort(peaks, [](const hough_line& a, const hough_line& b) { return a.votes > b.votes; });

		std::vector<hough_line> selected;
		for (const auto& peak : peaks)
		{
			const auto duplicate = std::ranges::any_of(selected, [&](const hough_line& other)
			{
				return std::abs(peak.theta - other.theta) < 6.0 * detection_pi / 180.0 &&
					std::abs(peak.rho - other.rho) < 12.0;
			});
			if (!duplicate) selected.push_back(peak);
			if (selected.size() >= 10) break;
		}
		return selected;
	};

	const auto vertical_lines = collect_lines(true);
	const auto horizontal_lines = collect_lines(false);
	if (vertical_lines.size() < 2 || horizontal_lines.size() < 2) return candidate;

	const auto vertical_position = [center_y = height * 0.5](const hough_line& line)
	{
		return (line.rho - center_y * std::sin(line.theta)) / std::cos(line.theta);
	};
	const auto horizontal_position = [center_x = width * 0.5](const hough_line& line)
	{
		return (line.rho - center_x * std::cos(line.theta)) / std::sin(line.theta);
	};

	// Prefer well separated near-parallel pairs so an inner rule cannot outvote the page border.
	const auto best_pair = [](const std::vector<hough_line>& lines, auto position, const double span)
	{
		std::pair best{-1, -1};
		double best_score = 0;

		for (auto a = 0; a < static_cast<int>(lines.size()); ++a)
		{
			for (auto b = a + 1; b < static_cast<int>(lines.size()); ++b)
			{
				if (std::abs(lines[a].theta - lines[b].theta) > 15.0 * detection_pi / 180.0) continue;
				const auto separation = std::abs(position(lines[a]) - position(lines[b]));
				if (separation < span * 0.35) continue;
				const auto score = (lines[a].votes + lines[b].votes) * (0.5 + separation / span);
				if (score > best_score)
				{
					best_score = score;
					best = {a, b};
				}
			}
		}
		return best;
	};

	const auto vertical_pair = best_pair(vertical_lines, vertical_position, width);
	const auto horizontal_pair = best_pair(horizontal_lines, horizontal_position, height);
	if (vertical_pair.first < 0 || horizontal_pair.first < 0) return candidate;

	auto left = vertical_lines[vertical_pair.first];
	auto right = vertical_lines[vertical_pair.second];
	if (vertical_position(left) > vertical_position(right)) std::swap(left, right);
	auto top = horizontal_lines[horizontal_pair.first];
	auto bottom = horizontal_lines[horizontal_pair.second];
	if (horizontal_position(top) > horizontal_position(bottom)) std::swap(top, bottom);

	const auto intersect = [](const hough_line& a, const hough_line& b, pointd& point)
	{
		const auto determinant = std::cos(a.theta) * std::sin(b.theta) - std::sin(a.theta) * std::cos(b.theta);
		if (std::abs(determinant) < 1e-6) return false;
		point = {
			(a.rho * std::sin(b.theta) - b.rho * std::sin(a.theta)) / determinant,
			(b.rho * std::cos(a.theta) - a.rho * std::cos(b.theta)) / determinant
		};
		return true;
	};

	std::array<pointd, 4> corners{};
	if (!intersect(left, top, corners[0]) || !intersect(right, top, corners[1]) ||
		!intersect(right, bottom, corners[2]) || !intersect(left, bottom, corners[3]))
	{
		return candidate;
	}

	const auto margin_x = width * 0.04;
	const auto margin_y = height * 0.04;
	for (auto& corner : corners)
	{
		if (corner.X < -margin_x || corner.X > width + margin_x ||
			corner.Y < -margin_y || corner.Y > height + margin_y)
		{
			return candidate;
		}
		corner.X = std::clamp(corner.X, 0.0, static_cast<double>(width));
		corner.Y = std::clamp(corner.Y, 0.0, static_cast<double>(height));
	}

	const auto area_fraction = convex_quad_area(corners) / static_cast<double>(pixel_count);
	if (area_fraction < 0.15) return candidate;

	double edge_support = 0;
	for (auto edge = 0; edge < 4; ++edge)
	{
		const auto a = corners[edge];
		const auto b = corners[(edge + 1) % 4];
		auto supported = 0;
		constexpr auto samples = 64;
		for (auto sample = 0; sample < samples; ++sample)
		{
			const auto position = (sample + 0.5) / samples;
			const auto x = std::clamp(df::round(a.X + (b.X - a.X) * position), 1, width - 2);
			const auto y = std::clamp(df::round(a.Y + (b.Y - a.Y) * position), 1, height - 2);
			auto found = false;
			for (auto yy = (std::max)(1, y - 2); yy <= (std::min)(height - 2, y + 2) && !found; ++yy)
				for (auto xx = (std::max)(1, x - 2); xx <= (std::min)(width - 2, x + 2); ++xx)
					if (magnitude[static_cast<size_t>(yy) * width + xx] >= edge_threshold)
					{
						found = true;
						break;
					}
			supported += found ? 1 : 0;
		}
		edge_support += supported / static_cast<double>(samples);
	}
	edge_support /= 4.0;
	if (edge_support < 0.70) return candidate;

	candidate.corners = corners;
	candidate.confidence = std::clamp(edge_support * 0.70 + (std::min)(area_fraction, 1.0) * 0.30, 0.0, 1.0);
	return candidate;
}

static document_detection_result build_document_result(const std::array<pointd, 4>& pixel_corners, const int width,
                                                       const int height, const sizei source_extent,
                                                       const double confidence)
{
	document_detection_result result;

	// Detection runs on a reduced analysis surface; report the page in source pixels so every later
	// step works in the same coordinates as the crop and perspective controls.
	for (auto index = 0; index < 4; ++index)
	{
		result.corners[index] = {
			pixel_corners[index].X / width * source_extent.cx,
			pixel_corners[index].Y / height * source_extent.cy
		};
	}

	const auto top_width = pixel_corners[0].dist(pixel_corners[1]);
	const auto bottom_width = pixel_corners[3].dist(pixel_corners[2]);
	const auto left_height = pixel_corners[0].dist(pixel_corners[3]);
	const auto right_height = pixel_corners[1].dist(pixel_corners[2]);
	result.extent = {
		(std::max)(32, df::round((top_width + bottom_width) * 0.5 / width * source_extent.cx)),
		(std::max)(32, df::round((left_height + right_height) * 0.5 / height * source_extent.cy))
	};
	result.confidence = std::clamp(confidence, 0.0, 1.0);
	return result;
}

namespace
{
	struct homogeneous
	{
		double x = 0;
		double y = 0;
		double w = 0;
	};

	homogeneous cross_product(const homogeneous& a, const homogeneous& b)
	{
		return {a.y * b.w - a.w * b.y, a.w * b.x - a.x * b.w, a.x * b.y - a.y * b.x};
	}
}

document_correction fit_document_correction(const std::array<pointd, 4>& corners, const sizei extent,
                                            const ui::orientation orientation)
{
	document_correction result;
	if (extent.cx < 2 || extent.cy < 2) return result;

	// Normalized about the image centre, matching image_edits::perspective_bounds.
	std::array<homogeneous, 4> page{};

	for (auto index = 0; index < 4; ++index)
	{
		page[index] = {corners[index].X / extent.cx - 0.5, corners[index].Y / extent.cy - 0.5, 1.0};
	}

	// The perspective warp sends the line where its denominator vanishes to infinity, so making the
	// page's opposite edges parallel is exactly a matter of placing that line through both vanishing
	// points. That recovers the two perspective slider values rather than a baked homography.
	const auto horizon = cross_product(
		cross_product(cross_product(page[0], page[1]), cross_product(page[3], page[2])),
		cross_product(cross_product(page[0], page[3]), cross_product(page[1], page[2])));

	auto horizontal = 0.0;
	auto vertical = 0.0;
	const auto horizon_scale = std::hypot(horizon.x, horizon.y);

	// A vanishing line through the image centre would need an unbounded correction; leave it flat.
	if (horizon_scale > 1e-9 && std::abs(horizon.w) > horizon_scale * 1e-4)
	{
		horizontal = -horizon.x / horizon.w;
		vertical = -horizon.y / horizon.w;
	}

	// Clamp by magnitude so the limit holds in both the display and slider frames. A clamped
	// correction is partial, and the sliders show exactly what was applied so it can be finished.
	constexpr auto maximum_perspective = 0.6;
	const auto strength = std::hypot(horizontal, vertical);

	if (strength > maximum_perspective)
	{
		const auto reduction = maximum_perspective / strength;
		horizontal *= reduction;
		vertical *= reduction;
	}

	quadd warped;

	for (auto index = 0; index < 4; ++index)
	{
		const auto denominator = 1.0 - horizontal * page[index].x - vertical * page[index].y;
		if (denominator < 0.05) return result;
		warped[index] = {
			(0.5 + page[index].x / denominator) * extent.cx,
			(0.5 + page[index].y / denominator) * extent.cy
		};
	}

	// The warped page is a parallelogram, so both horizontal edges share one direction.
	const auto edge_x = (warped[1].X - warped[0].X) + (warped[2].X - warped[3].X);
	const auto edge_y = (warped[1].Y - warped[0].Y) + (warped[2].Y - warped[3].Y);
	const auto rotation = to_degrees(std::atan2(edge_y, edge_x));

	const auto centre = warped.center_point();
	const auto upright = warped.rotate(-rotation, centre);
	const auto left = (std::max)(upright[0].X, upright[3].X);
	const auto right = (std::min)(upright[1].X, upright[2].X);
	const auto top = (std::max)(upright[0].Y, upright[1].Y);
	const auto bottom = (std::min)(upright[2].Y, upright[3].Y);
	if (right - left < 2.0 || bottom - top < 2.0) return result;

	// The inscribed rectangle holds page only. Corner order carries the stored orientation, exactly
	// as the initial crop does, so the page stays upright for rotated captures.
	result.crop = quadd(left, top, right, bottom).rotate(rotation, centre)
	                                             .transform(to_simple_transform_inv(orientation));
	result.straighten = edit_view_state::calc_straighten(result.crop.angle());

	// The renderer rotates the perspective axes by the crop angle, so undo that here.
	const auto crop_angle = to_radian(result.crop.angle());
	const auto angle_sin = sin(crop_angle);
	const auto angle_cos = cos(crop_angle);
	result.perspective_horizontal = horizontal * angle_cos - vertical * angle_sin;
	result.perspective_vertical = horizontal * angle_sin + vertical * angle_cos;
	return result;
}

document_detection_result detect_document(const ui::const_surface_ptr& surface, const sizei source_extent)
{
	if (!is_valid(surface) || surface->width() < 32 || surface->height() < 32) return {};

	const auto width = static_cast<int>(surface->width());
	const auto height = static_cast<int>(surface->height());
	std::vector<float> luminance(static_cast<size_t>(width) * height);

	for (auto y = 0; y < height; ++y)
	{
		const auto* pixels = std::bit_cast<const ui::color32*>(surface->pixels_line(y));
		for (auto x = 0; x < width; ++x)
		{
			const auto color = pixels[x];
			luminance[static_cast<size_t>(y) * width + x] =
				static_cast<float>((0.299 * ui::get_r(color) + 0.587 * ui::get_g(color) +
					0.114 * ui::get_b(color)) / 255.0);
		}
	}

	auto candidate = detect_document_by_region(surface, luminance, width, height);

	// Uneven lighting lets a global tone threshold cut through the page, so a merely adequate region
	// result is re-contested by the line search rather than trusted.
	if (candidate.confidence < clear_document_confidence)
	{
		const auto fallback = detect_document_by_edges(luminance, width, height);
		if (fallback.confidence > candidate.confidence) candidate = fallback;
	}

	if (candidate.confidence < minimum_document_confidence) return {};
	return build_document_result(candidate.corners, width, height, source_extent, candidate.confidence);
}

bool edit_view_controls::is_tracking() const
{
	const log_slider_control* sliders[] = {
		_straighten_slider.get(), _perspective_horizontal_slider.get(), _perspective_vertical_slider.get(),
		_vibrance_slider.get(), _darks_slider.get(), _midtones_slider.get(), _lights_slider.get(),
		_contrast_slider.get(), _brightness_slider.get(), _saturation_slider.get(), _temperature_slider.get(),
		_tint_slider.get()
	};

	return std::ranges::any_of(sliders, [](const log_slider_control* slider)
	{
		return slider && slider->is_tracking();
	});
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class task_toolbar_control final : public view_element, public std::enable_shared_from_this<task_toolbar_control>
{
	ui::toolbar_ptr _tb;

public:
	task_toolbar_control(const ui::control_frame_ptr& h, const std::vector<ui::command_ptr>& buttons)
	{
		_tb = h->create_toolbar({}, buttons);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, default_control_height(mc)};
	}

	void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler) override
	{
		handler(_tb);
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		const auto extent = _tb->measure_toolbar(bounds.width());
		const auto r = center_rect(extent, bounds);
		positions.emplace_back(_tb, r, is_visible());
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


class edit_rating_control final : public std::enable_shared_from_this<edit_rating_control>, public ui::frame_host
{
public:
	bool _hover = false;
	int _rating = -1;
	int _hover_rating = -1;
	ui::edit_ptr _edit;
	ui::frame_ptr _frame;
	sizei _extent;

	int _cell_width = 18;

	edit_rating_control(ui::edit_ptr edit) : _edit(std::move(edit))
	{
	}

	void init(const ui::control_frame_ptr& owner)
	{
		_frame = owner->create_frame(weak_from_this(), {});
	}

	// Never null: the rating can be set before init() has given this control a window.
	const ui::frame_ptr& frame() const
	{
		return _frame ? _frame : ui::no_frame();
	}

	void on_window_layout(ui::measure_context& mc, const sizei extent, const bool is_minimized) override
	{
		_extent = extent;
		_cell_width = mc.icon_cxy;
	}

	void on_mouse_move(const pointi loc, const bool is_tracking) override
	{
		if (!_hover)
		{
			_hover = true;
			frame()->invalidate();
		}

		const auto hr = point_to_stars(loc);

		if (hr != _hover_rating)
		{
			_hover_rating = hr;
			frame()->invalidate();
		}
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		const auto star = point_to_stars(loc);

		_rating = star;

		if (_edit)
		{
			_edit->window_text(_rating < 1 ? std::string_view{} : str::to_string(_rating));
		}

		frame()->invalidate();
	}

	void on_mouse_leave(const pointi loc) override
	{
		if (_hover)
		{
			_hover = false;
			_hover_rating = -1;
			frame()->invalidate();
		}
	}

	void on_mouse_wheel(const pointi loc, const int delta, const ui::key_state keys, bool& was_handled) override
	{
	}

	void tick() override
	{
	}

	void activate(bool is_active) override
	{
	}

	bool key_down(const int c, const ui::key_state keys) override
	{
		return false;
	}

	void on_window_paint(ui::draw_context& dc) override
	{
		dc.clear(ui::color(dc.colors.background, 1.0f));

		const recti logical_bounds = _extent;
		const auto bg = _hover
			                ? view_handle_color(false, _hover, false, dc.frame_has_focus, true).aa(dc.colors.alpha)
			                : ui::color{};
		const auto rating = _hover ? _hover_rating : _rating;
		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		std::string text;

		for (auto i = 0; i < 5; i++)
		{
			text += icon_to_utf8(i < rating ? icon_index::star_solid : icon_index::star);
		}

		dc.draw_text(text, logical_bounds, ui::style::font_face::icons,
		             ui::style::text_style::single_line, clr, bg);
	}

	void rating(const int stars)
	{
		if (_rating != stars)
		{
			_rating = stars;
			frame()->invalidate();
		}
	}

	int point_to_stars(const pointi loc) const
	{
		return std::clamp(1 + loc.x / std::max(1, _cell_width), 0, 5);
	}
};

using edit_rating_control_ptr = std::shared_ptr<edit_rating_control>;

class rating_bar_control final : public view_element, public std::enable_shared_from_this<rating_bar_control>
{
	edit_rating_control_ptr _stars;
	ui::edit_ptr _edit;
	std::string _label;
	int& _val;

public:
	rating_bar_control(const ui::control_frame_ptr& owner, const std::string_view label,
	                   int& v) : _label(label), _val(v)
	{
		ui::edit_styles style;
		style.number = true;
		style.align_center = true;
		_edit = owner->create_edit(style, {}, [this](const std::string_view text) { edit_change(text); });
		_stars = std::make_shared<edit_rating_control>(_edit);
		_stars->init(owner);
	}

	void label(const std::string_view label)
	{
		_label = label;
	}

	void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler) override
	{
		handler(_stars->frame());
		handler(_edit);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		if (!str::is_empty(_label))
		{
			const auto label_extent = mc.measure_text(_label, ui::style::font_face::dialog,
			                                          ui::style::text_style::single_line, width_limit);
			mc.col_widths.c1 = std::max(mc.col_widths.c1, label_extent.cx + mc.padding2);
		}

		return {width_limit, default_control_height(mc)};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		auto rStars = bounds;
		auto rEdit = bounds;

		if (!str::is_empty(_label)) rEdit.left += mc.col_widths.c1;
		rEdit.right = std::min(rEdit.left + df::round(50 * mc.scale_factor), bounds.right);
		rStars.left = std::min(rEdit.right + mc.padding2, bounds.right);

		positions.emplace_back(_edit, rEdit, is_visible());
		positions.emplace_back(_stars->frame(), rStars, is_visible());
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		if (!str::is_empty(_label))
		{
			auto r = bounds.offset(element_offset);
			r.right = r.left + dc.col_widths.c1 - dc.padding2;
			dc.draw_text(_label, r, ui::style::font_face::dialog, ui::style::text_style::single_line,
			             {dc.colors.foreground, dc.colors.alpha}, {});
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::populate)
		{
			_edit->window_text(str::to_string(_val));
			_stars->rating(_val);
		}
	}

	void edit_change(const std::string_view text) const
	{
		_val = std::clamp(str::to_int(text), 0, 5);
		_stars->rating(_val);
	}
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class save_buttons_control final : public view_element, public std::enable_shared_from_this<save_buttons_control>
{
	ui::button_ptr _ok;
	ui::button_ptr _abort;
	ui::button_ptr _cancel;

public:
	save_buttons_control(const ui::control_frame_ptr& h,
	                     std::function<void()> fn_save,
	                     std::function<void()> fn_dont,
	                     std::function<void()> fn_cancel)
	{
		_ok = h->create_button(tt.button_save, std::move(fn_save), true);
		_abort = h->create_button(tt.button_dont_save, std::move(fn_dont));
		_cancel = h->create_button(tt.button_cancel, std::move(fn_cancel));
	}

	void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler) override
	{
		handler(_ok);
		handler(_abort);
		handler(_cancel);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, mc.text_line_height(ui::style::font_face::dialog) + mc.padding2 * 4};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		const auto width = bounds.width() / 3;

		auto rok = bounds;
		auto rab = bounds;
		auto rcan = bounds;

		rab.left = rok.right = rok.left + width;
		rab.right = rcan.left = rcan.right - width;

		positions.emplace_back(_ok, rok.inflate(-mc.padding1), is_visible());
		positions.emplace_back(_abort, rab.inflate(-mc.padding1), is_visible());
		positions.emplace_back(_cancel, rcan.inflate(-mc.padding1), is_visible());
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

edit_view::edit_view(view_state& s, view_host_ptr host, edit_view_state& evs) :
	_state(s),
	_host(std::move(host)),
	_edit_state(evs)
{
}

view_controls_host_ptr edit_view::controls(const ui::control_frame_ptr& owner)
{
	if (!_edit_controls)
	{
		_edit_controls = std::make_shared<edit_view_controls>(_state, _edit_state);
		_edit_controls->_view = shared_from_this();
		_edit_controls->_frame = _edit_controls->_dlg = owner->create_dlg(_edit_controls, false);
	}

	return _edit_controls;
}

ui::const_surface_ptr edit_view::build_preview_surface(const ui::const_surface_ptr& source,
                                                       const sizei loaded_dimensions,
                                                       const image_edits& edits)
{
	if (!is_valid(source) || loaded_dimensions.is_empty()) return nullptr;

	auto preview_edits = edits;
	const auto edit_dimensions = loaded_dimensions;
	const auto preview_edit_dimensions = source->dimensions();

	if (preview_edits.has_crop_bounds())
	{
		const auto crop = edits.effective_crop_bounds(edit_dimensions);
		preview_edits.crop_bounds(crop.mult(static_cast<double>(preview_edit_dimensions.cx) / edit_dimensions.cx,
		                                    static_cast<double>(preview_edit_dimensions.cy) / edit_dimensions.cy));
	}
	preview_edits.scale(128);
	return apply_image_edits(source, preview_edits);
}

ui::const_surface_ptr edit_view::preview_surface() const
{
	return build_preview_surface(_dialog_preview_source, _loaded.dimensions(), _edit_state._edits);
}

void edit_view::activate(const sizei extent)
{
	_extent = extent;

	if (_edit_controls->_controls.empty())
	{
		_edit_controls->create_controls();
	}

	display_changed();

	_state.stop();
	changed();

	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::app_layout);
}

void edit_view::deactivate()
{
	_state.stop();
	_media_display.reset();
	_media_element.reset();
	_play_element.reset();
	_scrubber_element.reset();
	_preview_source.reset();
	_dialog_preview_source.reset();
	++_display_generation;
	++_preview_generation;
	_pending_source_load_request.reset();
	_pending_preview_decode_request.reset();
	_source_loading = false;
	_texture.reset();
	_loaded.clear();
	clear_crop_interaction_bounds();
}

void edit_view::refresh()
{
	_invalid = true;
	_host->frame()->invalidate();
}

void edit_view::layout(ui::measure_context& mc, const sizei extent)
{
	// No reset on a new extent: queue_preview_decode asks again only if the wanted size changed, and
	// the picture on screen stays until its replacement arrives.
	_extent = extent;
	queue_preview_decode();

	if (_media_element && _play_element && _scrubber_element)
	{
		ui::control_layouts positions;
		const auto outer_bounds = recti(extent).inflate(-mc.padding2);
		const auto play_extent = _play_element->measure(mc, outer_bounds.width());
		const auto scrubber_extent = _scrubber_element->measure(mc, outer_bounds.width());
		const auto controls_height = std::max(play_extent.cy, scrubber_extent.cy);
		const auto controls_top = std::max(outer_bounds.top, outer_bounds.bottom - controls_height);
		const auto controls_bounds = recti(outer_bounds.left, controls_top, outer_bounds.right, outer_bounds.bottom);
		const auto play_bounds = recti(controls_bounds.left, controls_bounds.top,
		                               controls_bounds.left + play_extent.cx, controls_bounds.bottom);
		const auto scrubber_bounds = recti(play_bounds.right + mc.padding1, controls_bounds.top,
		                                   controls_bounds.right, controls_bounds.bottom);
		const auto media_bounds = recti(outer_bounds.left, outer_bounds.top, outer_bounds.right,
		                                std::max(outer_bounds.top, controls_bounds.top - mc.padding2));

		_play_element->layout(mc, play_bounds, positions);
		_scrubber_element->layout(mc, scrubber_bounds, positions);
		_media_element->layout(mc, media_bounds, positions);
	}
}

void edit_view::update_media_elements()
{
	_media_display = _state.display_state();
	_media_element.reset();
	_play_element.reset();
	_scrubber_element.reset();

	if (!_media_display || !_media_display->is_one()) return;

	const auto* const file_type = _media_display->_item1->file_type();
	if (file_type->has_trait(file_traits::visualize_audio))
	{
		_media_element = std::make_shared<audio_control>(_state, _media_display, _host);
	}
	else if (file_type->has_trait(file_traits::av))
	{
		_media_element = std::make_shared<video_control>(_state, _media_display, _host);
	}
	else
	{
		return;
	}

	_play_element = std::make_shared<play_control>(_state, view_element_style::none);
	_scrubber_element = std::make_shared<scrubber_element>(_state._player, _media_display);
}

bool edit_view::is_photo() const
{
	return can_edit_pixels() && _mt->has_trait(file_traits::bitmap);
}

bool edit_load_has_source_pixels(const file_load_result& loaded)
{
	return loaded.success && loaded.reason == file_load_result::failure::none && !loaded.is_empty();
}

bool edit_view::can_edit_pixels() const
{
	return edit_load_has_source_pixels(_loaded);
}

std::string_view edit_load_status_text(const file_load_result& loaded, const bool source_loading)
{
	if (source_loading) return tt.loading.sv();
	if (loaded.reason == file_load_result::failure::too_large) return tt.image_too_large.sv();
	if (!loaded.success) return tt.image_display_failed.sv();
	return {};
}

bool should_accept_edit_async_result(const size_t current_display_generation, const size_t result_display_generation)
{
	return current_display_generation == result_display_generation;
}

bool should_accept_edit_preview_result(const size_t current_display_generation, const size_t result_display_generation,
                                       const size_t current_preview_generation, const size_t result_preview_generation,
                                       const sizei current_request_dimensions, const sizei result_dimensions)
{
	return should_accept_edit_async_result(current_display_generation, result_display_generation) &&
		current_preview_generation == result_preview_generation &&
		current_request_dimensions == result_dimensions;
}

bool should_accept_edit_analysis(const size_t current_display_generation, const size_t result_display_generation,
                                 const size_t current_edit_generation, const size_t result_edit_generation)
{
	return should_accept_edit_async_result(current_display_generation, result_display_generation) &&
		current_edit_generation == result_edit_generation;
}

bool edit_preview_is_current(const ui::const_surface_ptr& preview, const sizei answered_request, const sizei wanted)
{
	return is_valid(preview) && answered_request == wanted;
}

file_load_result::failure edit_preview_failure(const ui::const_surface_ptr& preview, const file_load_result& loaded,
                                               const sizei request)
{
	if (is_valid(preview)) return file_load_result::failure::none;
	return files::exceeds_decode_budget(loaded.i, request)
		       ? file_load_result::failure::too_large
		       : file_load_result::failure::unreadable;
}

file_load_result edit_load_without_preview(const file_load_result& loaded, const file_load_result::failure failure)
{
	file_load_result result;
	result.reason = failure;
	result.source_dimensions = loaded.dimensions();
	return result;
}

std::optional<quadd> edit_pending_crop_for_loaded_photo(std::optional<rectd> pending_crop,
                                                        const file_load_result& loaded, const bool show_rotated)
{
	if (!pending_crop || pending_crop->is_empty() || !edit_load_has_source_pixels(loaded)) return {};
	return edit_view_state::crop_from_displayed_rect(*pending_crop, loaded.dimensions(), loaded.orientation(),
	                                                 show_rotated);
}

bool edit_pixel_controls_visible(const bool is_bitmap, const bool can_edit_pixels)
{
	return is_bitmap && can_edit_pixels;
}

edit_preview_render_decision decide_edit_preview_render(const bool has_texture, const bool has_current_preview,
                                                        const bool has_loading_status)
{
	return {has_texture || has_current_preview, !has_texture && !has_current_preview && has_loading_status,
		has_texture || has_current_preview};
}

edit_request_coalescing_decision decide_edit_request_coalescing(const bool request_in_flight)
{
	return request_in_flight ? edit_request_coalescing_decision{false, true} : edit_request_coalescing_decision{true, false};
}

bool should_start_edit_follow_up_request(const bool has_latest_request)
{
	return has_latest_request;
}

static prop::item_metadata_const_ptr safe_metadata(const df::item_element_ptr& i);

void edit_view::queue_source_load(source_load_request request)
{
	const auto decision = decide_edit_request_coalescing(_source_load_in_flight);
	if (decision.remember_latest)
	{
		_pending_source_load_request = std::move(request);
		return;
	}

	start_source_load(std::move(request));
}

void edit_view::start_source_load(source_load_request request)
{
	_source_load_in_flight = true;
	const auto weak = weak_from_this();
	_state.queue_async(async_queue::load, [weak, request = std::move(request), &s = _state]
	{
		df::scope_locked_inc l(df::loading_media);
		files loader;
		auto loaded = loader.load(request.path, false);
		s.queue_ui([weak, request, loaded = std::move(loaded)]() mutable
		{
			if (const auto self = weak.lock()) self->complete_source_load(request, std::move(loaded));
		});
	});
}

void edit_view::complete_source_load(const source_load_request& request, file_load_result loaded)
{
	_source_load_in_flight = false;

	if (should_accept_edit_async_result(_display_generation, request.display_generation) && _path == request.path)
	{
		_source_loading = false;
		_loaded = std::move(loaded);
		_preview_source.reset();
		_dialog_preview_source.reset();
		_preview_request_dimensions = {};
		_preview_failed_request = {};
		++_preview_generation;
		_invalid = true;

		_edit_state.reset(safe_metadata(_state._edit_item), _loaded.dimensions(), _loaded.orientation());

		if (const auto crop = edit_pending_crop_for_loaded_photo(request.pending_crop, _loaded, setting.show_rotated))
		{
			_edit_state._edits.crop_bounds(*crop);
		}

		queue_preview_decode();

		if (is_photo())
		{
			const auto loaded_copy = _loaded;
			const auto preview_dimensions = ui::scale_dimensions(loaded_copy.dimensions(), 192);
			const auto weak_dialog = weak_from_this();
			const auto dialog_generation = _display_generation;
			_state.queue_async(async_queue::render,
			                   [weak_dialog, loaded_copy, preview_dimensions, dialog_generation, &s = _state]
			                   {
				                   const auto source = loaded_copy.to_surface(preview_dimensions);
				                   s.queue_ui([weak_dialog, source, dialog_generation]
				                   {
					                   if (const auto current = weak_dialog.lock();
						                   current && current->_display_generation == dialog_generation)
					                   {
						                   current->_dialog_preview_source = source;
					                   }
				                   });
			                   });
		}

		if (_edit_controls->_dlg)
		{
			_edit_controls->populate();
			_edit_controls->_dlg->layout();
		}
		_state.invalidate_view(view_invalid::command_state | view_invalid::view_redraw | view_invalid::controller);
		_host->frame()->invalidate();
	}

	if (should_start_edit_follow_up_request(_pending_source_load_request.has_value()))
	{
		auto next = std::move(*_pending_source_load_request);
		_pending_source_load_request.reset();
		start_source_load(std::move(next));
	}
}

void edit_view::queue_preview_decode()
{
	if (!can_edit_pixels() || _extent.is_empty()) return;

	const auto preview_dimensions = ui::scale_dimensions(_loaded.dimensions(), _extent);
	if (preview_dimensions.is_empty()) return;
	if (edit_preview_is_current(_preview_source, _preview_source_request, preview_dimensions)) return;
	if (_preview_request_dimensions == preview_dimensions) return;
	if (_preview_failed_request == preview_dimensions) return;

	preview_decode_request request{_loaded, preview_dimensions, _display_generation, ++_preview_generation};
	const auto decision = decide_edit_request_coalescing(_preview_decode_in_flight);
	if (decision.remember_latest)
	{
		_pending_preview_decode_request = std::move(request);
		return;
	}

	start_preview_decode(std::move(request));
}

void edit_view::start_preview_decode(preview_decode_request request)
{
	_preview_decode_in_flight = true;
	_preview_request_dimensions = request.dimensions;
	const auto weak = weak_from_this();
	_state.queue_async(async_queue::render,
	                   [weak, request, &s = _state]
	                   {
		                   const auto source = request.loaded.to_surface(request.dimensions);
		                   const auto failure = edit_preview_failure(source, request.loaded, request.dimensions);
		                   s.queue_ui([weak, request, source, failure]
		                   {
			                   if (const auto self = weak.lock())
			                   {
				                   self->complete_preview_decode(request, source, failure);
			                   }
		                   });
	                   });
}

void edit_view::complete_preview_decode(const preview_decode_request& request, ui::const_surface_ptr source,
                                        const file_load_result::failure failure)
{
	_preview_decode_in_flight = false;

	if (should_accept_edit_preview_result(_display_generation, request.display_generation, _preview_generation,
	                                      request.preview_generation, _preview_request_dimensions, request.dimensions))
	{
		_preview_request_dimensions = {};
		if (is_valid(source))
		{
			_preview_source = source;
			_preview_source_request = request.dimensions;
			_invalid = true;
			_host->frame()->invalidate();
		}
		else if (is_valid(_preview_source))
		{
			// An earlier size is on screen and stays there. Asking again for this one would fail the
			// same way on every repaint.
			_preview_failed_request = request.dimensions;
		}
		else
		{
			// Nothing of this file has been shown, and its load read only the header, so it cannot be
			// edited: say why, as the media view does, and withdraw the pixel controls with it.
			_loaded = edit_load_without_preview(_loaded, failure);
			_invalid = true;

			if (_edit_controls->_dlg)
			{
				_edit_controls->populate();
				_edit_controls->_dlg->layout();
			}
			_state.invalidate_view(view_invalid::command_state | view_invalid::view_redraw | view_invalid::controller);
			_host->frame()->invalidate();
		}
	}

	if (should_start_edit_follow_up_request(_pending_preview_decode_request.has_value()))
	{
		auto next = std::move(*_pending_preview_decode_request);
		_pending_preview_decode_request.reset();
		start_preview_decode(std::move(next));
	}
}

void edit_view::clear_crop_interaction_bounds()
{
	_crop_bounds = {};
	_crop_handle_tl = {};
	_crop_handle_tr = {};
	_crop_handle_bl = {};
	_crop_handle_br = {};
}

void edit_view::draw_loading_status(ui::draw_context& dc, const std::string_view status) const
{
	if (status.empty()) return;

	const recti status_bounds(0, 0, _extent.cx, _extent.cy);
	dc.draw_text(status, status_bounds, ui::style::font_face::title,
	             ui::style::text_style::single_line_center, ui::color(dc.colors.foreground, dc.colors.alpha), {});
}

df::item_element_ptr edit_view::next_editable_item(const bool forward) const
{
	// The selector strip only offers editable photos, so save-and-next walks the same set rather
	// than stranding the user on a video, folder or unsupported file with no editing controls.
	df::item_elements editable;

	for (const auto& g : _state.groups())
	{
		for (const auto& i : g->items())
		{
			if (i && i->file_type()->can_edit_photo()) editable.emplace_back(i);
		}
	}

	if (editable.size() < 2) return {};

	const auto focus = _state.focus_item();
	const auto found = std::ranges::find(editable, focus);
	if (found == editable.end()) return forward ? editable.front() : editable.back();

	const auto index = std::distance(editable.begin(), found);
	const auto count = static_cast<ptrdiff_t>(editable.size());
	return editable[(index + (forward ? 1 : count - 1)) % count];
}

view_controller_ptr edit_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                       hit_test_context& ctx)
{
	if (is_photo() && !_edit_state._preview_mode)
	{
		if (ctx.occluded(_crop_handle_tl.round().inflate(8)))
		{
			return std::make_shared<handle_move_controller<edit_view>>(host, *this, _crop_handle_tl, true, true, false,
			                                                           false);
		}

		if (ctx.occluded(_crop_handle_tr.round().inflate(8)))
		{
			return std::make_shared<handle_move_controller<edit_view>>(host, *this, _crop_handle_tr, false, true, true,
			                                                           false);
		}

		if (ctx.occluded(_crop_handle_bl.round().inflate(8)))
		{
			return std::make_shared<handle_move_controller<edit_view>>(host, *this, _crop_handle_bl, true, false, false,
			                                                           true);
		}

		if (ctx.occluded(_crop_handle_br.round().inflate(8)))
		{
			return std::make_shared<handle_move_controller<edit_view>>(host, *this, _crop_handle_br, false, false, true,
			                                                           true);
		}

		if (_crop_bounds.contains(loc))
		{
			return std::make_shared<selection_move_controller<edit_view>>(host, *this, _crop_bounds.round());
		}
	}
	else if (_media_element)
	{
		constexpr pointi element_offset{};

		if (const auto controller = _play_element->controller_from_location(host, loc, element_offset, ctx))
			return controller;
		ctx.passed_over(_play_element->bounds);

		if (const auto controller = _scrubber_element->controller_from_location(host, loc, element_offset, ctx))
			return controller;
		ctx.passed_over(_scrubber_element->bounds);

		return _media_element->controller_from_location(host, loc, element_offset, ctx);
	}

	return nullptr;
}

bool select_path(df::file_path& path)
{
	if (!files::can_save(path))
	{
		path = path.extension(".jpg");
	}

	return platform::prompt_for_save_path(path);
}

bool edit_view::check_path(df::file_path& path, const ui::control_frame_ptr& owner) const
{
	if (!path.is_save_valid())
	{
		const auto dlg = make_dlg(owner);
		const auto message = format_invalid_name_message(path.name());
		dlg->show_message(icon_index::error, s_app_name, message);
		return false;
	}

	if (is_photo())
	{
		const auto dlg = make_dlg(owner);

		if (_edit_state.has_pixel_changes() && !files::can_save(path))
		{
			const std::vector<view_element_ptr> controls{
				set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::save, tt.save_changes,
				                                                str_format(tt.save_as_jpeg_fmt.sv(), path.extension()),
				                                                std::vector<ui::const_surface_ptr>{preview_surface()})),
				std::make_shared<divider_element>(),
				std::make_shared<ui::ok_cancel_control>(dlg->_frame, tt.button_save_as_jpeg)
			};

			const auto result = dlg->show_modal(controls);

			if (result != ui::close_result::ok)
			{
				return false;
			}

			// Changing the extension writes a different file, so the destination is chosen and its
			// overwrite confirmed rather than silently landing on a sibling that already exists.
			if (!select_path(path)) return false;

			if (!path.is_save_valid())
			{
				make_dlg(owner)->show_message(icon_index::error, s_app_name,
				                              format_invalid_name_message(path.name()));
				return false;
			}
		}
	}


	return true;
}

bool edit_view::can_exit()
{
	if (has_changes())
	{
		auto dlg = make_dlg(_host->owner());

		auto save_fn = [dlg, this]
		{
			auto path = _path;
			const auto xmp_name = _xmp_name;
			if (check_path(path, dlg->_frame))
			{
				save(_path, path, xmp_name, dlg->_frame, [dlg, weak = weak_from_this()](const bool success)
				{
					if (const auto self = weak.lock(); success && self)
					{
						// The edits are on disk now, so whatever the user was doing when they were asked to save
						// must be allowed to continue rather than being refused for unsaved changes.
						self->_edit_state._original_edits = self->_edit_state._edits;
						dlg->close(false);
						self->_state.view_mode(view_type::items);
					}
				});
			}
		};
		auto cancel_fn = [dlg, this]
		{
			dlg->close(false);
			cancel();
		};

		const std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::save, tt.changes,
			                                                str_format(tt.has_changes.sv(), _path.name()),
			                                                std::vector<ui::const_surface_ptr>{preview_surface()})),
			std::make_shared<divider_element>(),
			std::make_shared<save_buttons_control>(dlg->_frame,
			                                       save_fn,
			                                       cancel_fn,
			                                       [dlg] { dlg->close(true); })
		};

		dlg->show_modal(controls);
	}

	return !has_changes();
}

void edit_view::select_item(const df::item_element_ptr& item)
{
	if (!item || item == _state.focus_item()) return;

	auto select = [weak = weak_from_this(), item]
	{
		if (const auto self = weak.lock()) self->_state.select(self->_host, item, false, false, false);
	};

	if (!has_changes())
	{
		select();
		return;
	}

	auto dlg = make_dlg(_host->owner());
	auto save_fn = [dlg, select, this]
	{
		auto path = _path;
		const auto xmp_name = _xmp_name;
		if (check_path(path, dlg->_frame))
		{
			save(_path, path, xmp_name, dlg->_frame, [dlg, select](const bool success)
			{
				if (success)
				{
					dlg->close(false);
					select();
				}
			});
		}
	};
	auto discard_fn = [dlg, select]
	{
		dlg->close(false);
		select();
	};

	const std::vector<view_element_ptr> controls = {
		set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::save, tt.changes,
		                                                str_format(tt.has_changes.sv(), _path.name()),
		                                                std::vector<ui::const_surface_ptr>{preview_surface()})),
		std::make_shared<divider_element>(),
		std::make_shared<save_buttons_control>(dlg->_frame, save_fn, discard_fn, [dlg] { dlg->close(true); })
	};

	dlg->show_modal(controls);
}

void edit_view::save_current()
{
	if (!can_edit_pixels()) return;

	auto path = _path;
	const auto xmp_name = _xmp_name;
	const auto owner = _host->owner();

	if (check_path(path, owner))
	{
		save(_path, path, xmp_name, owner, [weak = weak_from_this()](const bool success)
		{
			if (const auto self = weak.lock(); success && self) self->display_changed();
		});
	}
}

static prop::item_metadata_const_ptr safe_metadata(const df::item_element_ptr& i)
{
	if (i)
	{
		const auto md = i->metadata();
		if (md) return md;
	}

	return std::make_shared<prop::item_metadata>();
}

bool edit_view::has_changes() const
{
	return is_photo() && _edit_state.has_pixel_changes();
}

void edit_view::save(const df::file_path src_path, const df::file_path dst_path, const std::string_view xmp_name,
                     const ui::control_frame_ptr& owner, std::function<void(bool)> complete) const
{
	auto update_result = std::make_shared<file_update_result>();
	const auto dlg = make_dlg(owner);
	dlg->show_status(icon_index::save, str_format(tt.saving_file_name.sv(), dst_path.name()));

	auto detach = std::make_shared<detach_file_handles>(_state);
	const auto has_pixel_changes = _edit_state.has_pixel_changes();
	const auto me = edit_view_state::save_metadata_edits(has_pixel_changes, _loaded.orientation());
	const auto pe = has_pixel_changes ? _edit_state._edits : image_edits{};
	const auto dimensions = _loaded.dimensions();
	const auto photo_metadata = _mt->has_trait(file_traits::photo_metadata);
	const auto video_metadata = _mt->has_trait(file_traits::video_metadata);
	const auto music_metadata = _mt->has_trait(file_traits::music_metadata);
	const auto scan_request = _state.item_index.make_scan_request(_state._edit_item, true, false);

	_state.queue_async(async_queue::work,
	                   [update_result, detach, dst_path, me, pe, &s = _state, src_path, scan_request,
		                   dimensions, xmp_name = std::string(xmp_name), photo_metadata, video_metadata,
		                   music_metadata, has_pixel_changes, dlg, complete = std::move(complete)]() mutable
	                   {
		                   try
		                   {
			                   files ff;
			                   file_encode_params encode_params;
			                   encode_params.jpeg_save_quality = setting.jpeg_save_quality;
			                   encode_params.webp_lossless = setting.webp_lossless;
			                   encode_params.webp_quality = setting.webp_quality;

			                   const auto is_in_place = src_path == dst_path;
			                   const auto create_original = is_in_place && setting.create_originals &&
				                   pe.has_changes(dimensions);
			                   *update_result = ff.update(src_path, dst_path, me, pe, encode_params, create_original,
			                                              xmp_name, {},
			                                              is_in_place
				                                              ? index_state::make_rescan_spec(scan_request, xmp_name,
					                                              true)
				                                              : rescan_spec{});

			                   if (update_result->success())
			                   {
				                   if (is_in_place)
				                   {
					                   const auto force = s.item_index.apply_write_scan(scan_request, *update_result);
					                   s._async.queue_ui([&s, scan_request, force, update_result]
					                   {
						                   // The writer's own bytes, so the display never reads back
						                   // what it just saved.
						                   s.publish_written_image(scan_request.path,
						                                           std::move(update_result->loaded),
						                                           df::date_t(update_result->modified));

						                   const auto item = scan_request.lifetime.lock();
						                   if (!item || item->path() != scan_request.path) return;

						                   df::item_set items;
						                   items.add(item);
						                   s.item_index.queue_scan_modified_items(std::move(items), force);
					                   });
				                   }
				                   else
				                   {
					                   s.item_index.queue_scan_folder(dst_path.folder());
				                   }
			                   }
		                   }
		                   catch (const std::exception& e)
		                   {
			                   df::log(__FUNCTION__, e.what());
			                   update_result->code = platform::file_op_result_code::FAILED;
			                   update_result->error_message = str::utf8_cast(e.what());
		                   }

		                   s.queue_ui([update_result, detach, dst_path, me, photo_metadata, video_metadata,
				                   music_metadata, has_pixel_changes, dlg,
				                   complete = std::move(complete)]() mutable
			                   {
				                   detach.reset();

				                   if (update_result->success())
				                   {
					                   if (photo_metadata)
					                   {
						                   if (me.has_changes()) record_feature_use(features::edit_photo_metadata);
						                   if (has_pixel_changes) record_feature_use(features::edit_photo_bitmap);
					                   }
					                   else if (video_metadata)
					                   {
						                   record_feature_use(features::edit_video_metadata);
					                   }
					                   else if (music_metadata)
					                   {
						                   record_feature_use(features::edit_audio_metadata);
					                   }
				                   }
				                   else
				                   {
					                   dlg->show_message(icon_index::error, s_app_name,
					                                     update_result->format_error(str_format(
						                                     tt.error_create_file_failed_fmt.sv(), dst_path)));
				                   }

				                   complete(update_result->success());
			                   });
	                   });
}


void edit_view::save_and_next(const bool forward)
{
	auto path = _path;
	const auto xmp_name = _xmp_name;
	auto continue_next = [weak = weak_from_this(), forward]
	{
		const auto self = weak.lock();
		if (!self) return;

		const auto item = self->next_editable_item(forward);
		if (item)
		{
			self->_state.select(self->_host, item, false, false, false);
			self->_state.item_index.queue_scan_modified_items(self->_state.selected_items());
		}
	};

	if (has_changes())
	{
		const auto owner = _host->owner();

		if (!check_path(path, owner)) return;
		save(_path, path, xmp_name, owner, [continue_next](const bool success)
		{
			if (success) continue_next();
		});
		return;
	}

	continue_next();
}

void edit_view::save_as()
{
	if (!can_edit_pixels()) return;

	auto path = _path;
	const auto xmp_name = _xmp_name;

	const auto name = std::string(path.file_name_without_extension()) + "-edit"s;
	path = df::file_path(path.folder(), name, path.extension());

	// Number repeats rather than stacking suffixes, so a third edit is "photo-edit 3", not "photo-edit-edit-edit".
	for (auto n = 2; path.exists(); ++n)
	{
		path = df::file_path(path.folder(), name + " "s + std::to_string(n), path.extension());
	}

	const auto owner = _host->owner();

	if (select_path(path) && check_path(path, owner))
	{
		save(_path, path, xmp_name, owner, [weak = weak_from_this(), path](const bool success)
		{
			if (const auto self = weak.lock(); success && self)
			{
				self->_state.view_mode(view_type::items);
				self->_state.open(self->_host, path);
				self->_state.item_index.queue_scan_modified_items(self->_state.selected_items());
			}
		});
	}
}


void edit_view::cancel() const
{
	_edit_state._edits.clear();
	_state.view_mode(view_type::items);
}

void edit_view::exit()
{
	if (_state.view_mode() == view_type::edit)
	{
		if (can_exit())
		{
			if (_state.view_mode() == view_type::edit)
			{
				cancel();
			}
		}
	}
}

void edit_view::rotate_anticlockwise()
{
	if (!can_edit_pixels()) return;
	const auto selection = _edit_state._edits.crop_bounds();
	_edit_state._edits.crop_bounds(selection.transform(simple_transform::rot_270));
	const auto horizontal = _edit_state._perspective_horizontal;
	_edit_state._perspective_horizontal = _edit_state._perspective_vertical;
	_edit_state._perspective_vertical = -horizontal;
	_edit_controls->populate();
	changed();
}

void edit_view::rotate_clockwise()
{
	if (!can_edit_pixels()) return;
	const auto selection = _edit_state._edits.crop_bounds();
	_edit_state._edits.crop_bounds(selection.transform(simple_transform::rot_90));
	const auto horizontal = _edit_state._perspective_horizontal;
	_edit_state._perspective_horizontal = -_edit_state._perspective_vertical;
	_edit_state._perspective_vertical = horizontal;
	_edit_controls->populate();
	changed();
}

void edit_view::rotate_reset()
{
	if (!can_edit_pixels()) return;
	_edit_state._edits.crop_bounds(edit_view_state::initial_crop(_loaded.dimensions(), _loaded.orientation()));
	_edit_state._straighten = 0;
	_edit_state._perspective_horizontal = 0;
	_edit_state._perspective_vertical = 0;
	_edit_controls->populate();
	changed();
}

void edit_view::color_reset()
{
	if (!can_edit_pixels()) return;
	_edit_state.color_reset();
	_edit_controls->populate();
	changed();
}

void edit_view::report_no_result(const std::string_view title) const
{
	make_dlg(_host->owner())->show_message(icon_index::question, title, tt.nothing_found1);
}

// Decoding and scanning the image must not stall the window, so the analysis runs on a worker and
// returns a detached applier. An empty applier means the adjustment found nothing to improve.
void edit_view::queue_auto_adjust(const int max_dimension, std::string title,
                                  std::function<std::function<void(edit_view_state&)>(const ui::const_surface_ptr&)>
                                  analyze)
{
	if (!can_edit_pixels()) return;

	const auto loaded = _loaded;
	const auto scale_hint = ui::scale_dimensions(loaded.dimensions(), max_dimension, true);
	const auto generation = _display_generation;
	const auto edit_generation = _edit_generation;
	const auto weak = weak_from_this();

	_state.queue_async(async_queue::render,
	                   [weak, loaded, scale_hint, generation, edit_generation, title = std::move(title),
		                   analyze = std::move(analyze), &s = _state]() mutable
	                   {
		                   auto apply = analyze(loaded.to_surface(scale_hint));

		                   s.queue_ui([weak, generation, edit_generation, title = std::move(title),
			                   apply = std::move(apply)]
		                   {
			                   const auto self = weak.lock();
			                   if (!self || !should_accept_edit_analysis(self->_display_generation, generation,
				                   self->_edit_generation, edit_generation))
			                   {
				                   return;
			                   }

			                   // The analysis describes the picture as it stood when it was asked
			                   // for. Anything adjusted since is newer than its answer, and applying
			                   // it now would undo the user's own work with a reply to a question
			                   // they have already moved past.

			                   if (!apply)
			                   {
				                   self->report_no_result(title);
				                   return;
			                   }

			                   apply(self->_edit_state);
			                   self->_edit_controls->populate();
			                   self->changed();
		                   });
	                   });
}

void edit_view::auto_color()
{
	queue_auto_adjust(512, std::string(tt.command_auto_color.sv()),
	                  [](const ui::const_surface_ptr& surface) -> std::function<void(edit_view_state&)>
	                  {
		                  if (!is_valid(surface)) return {};

		                  std::array<uint32_t, 256> luminance_histogram{};
		                  double neutral_red = 0;
		                  double neutral_green = 0;
		                  double neutral_blue = 0;
		                  size_t neutral_count = 0;
		                  size_t sample_count = 0;
		                  double chroma_sum = 0;

		                  for (auto y = 0u; y < surface->height(); ++y)
		                  {
			                  const auto* pixels = std::bit_cast<const ui::color32*>(surface->pixels_line(y));

			                  for (auto x = 0u; x < surface->width(); ++x)
			                  {
				                  const auto color = pixels[x];
				                  if (ui::get_a(color) < 128) continue;

				                  const auto red = ui::get_r(color) / 255.0;
				                  const auto green = ui::get_g(color) / 255.0;
				                  const auto blue = ui::get_b(color) / 255.0;
				                  const auto luminance = 0.299 * red + 0.587 * green + 0.114 * blue;
				                  const auto maximum = std::max({red, green, blue});
				                  const auto minimum = std::min({red, green, blue});
				                  const auto chroma = maximum - minimum;

				                  ++luminance_histogram[std::clamp(df::round(luminance * 255), 0, 255)];
				                  chroma_sum += chroma;
				                  ++sample_count;

				                  if (luminance > 0.15 && luminance < 0.85 && chroma < 0.12)
				                  {
					                  neutral_red += red;
					                  neutral_green += green;
					                  neutral_blue += blue;
					                  ++neutral_count;
				                  }
			                  }
		                  }

		                  if (sample_count == 0) return {};

		                  const auto percentile = [&](const double fraction)
		                  {
			                  const auto target = static_cast<size_t>(sample_count * fraction);
			                  size_t accumulated = 0;

			                  for (size_t index = 0; index < luminance_histogram.size(); ++index)
			                  {
				                  accumulated += luminance_histogram[index];
				                  if (accumulated >= target) return index / 255.0;
			                  }

			                  return 1.0;
		                  };

		                  const auto low = percentile(0.02);
		                  const auto median = percentile(0.50);
		                  const auto high = percentile(0.98);
		                  const auto range = std::max(0.05, high - low);

		                  const auto brightness = std::clamp((0.48 - median) * 2.0, -0.45, 0.45);
		                  const auto contrast = std::clamp((0.82 / range - 1.0) * 0.75, -0.35, 0.35);
		                  const auto darks = std::clamp((0.08 - percentile(0.20)) * 2.5, -0.3, 0.3);
		                  const auto lights = std::clamp((0.82 - percentile(0.80)) * 2.5, -0.3, 0.3);
		                  const auto midtones = std::clamp((0.5 - median) * 1.5, -0.25, 0.25);
		                  const auto vibrance = chroma_sum / sample_count < 0.18 ? 0.08 : 0.0;

		                  auto temperature = 0.0;
		                  auto tint = 0.0;

		                  if (neutral_count > sample_count / 50)
		                  {
			                  const auto red = neutral_red / neutral_count;
			                  const auto green = neutral_green / neutral_count;
			                  const auto blue = neutral_blue / neutral_count;
			                  const auto mean = std::max(0.05, (red + green + blue) / 3.0);
			                  temperature = std::clamp((blue - red) / (0.4 * mean), -0.4, 0.4);
			                  tint = std::clamp((green - (red + blue) * 0.5) / (0.3 * mean), -0.35, 0.35);
		                  }

		                  return [brightness, contrast, darks, lights, midtones, vibrance, temperature, tint](
			                  edit_view_state& state)
		                  {
			                  state._brightness = brightness;
			                  state._contrast = contrast;
			                  state._darks = darks;
			                  state._lights = lights;
			                  state._midtones = midtones;
			                  state._vibrance = vibrance;
			                  state._saturation = 0;
			                  state._temperature = temperature;
			                  state._tint = tint;
		                  };
	                  });
}

void edit_view::auto_straighten()
{
	const auto current_straighten = edit_view_state::calc_straighten(_edit_state._edits.crop_bounds().angle());
	const auto rotation_angle = _edit_state._edits.rotation_angle();

	queue_auto_adjust(768, std::string(tt.command_auto_straighten.sv()),
	                  [current_straighten, rotation_angle](const ui::const_surface_ptr& surface) ->
	                  std::function<void(edit_view_state&)>
	                  {
		                  if (!is_valid(surface) || surface->width() < 3 || surface->height() < 3) return {};

		                  const auto width = static_cast<int>(surface->width());
		                  const auto height = static_cast<int>(surface->height());
		                  std::vector<float> grayscale(static_cast<size_t>(width) * height);

		                  for (auto y = 0; y < height; ++y)
		                  {
			                  const auto* pixels = std::bit_cast<const ui::color32*>(surface->pixels_line(y));
			                  for (auto x = 0; x < width; ++x)
			                  {
				                  grayscale[static_cast<size_t>(y) * width + x] =
					                  static_cast<float>((0.299 * ui::get_r(pixels[x]) + 0.587 * ui::get_g(pixels[x]) +
						                  0.114 * ui::get_b(pixels[x])) / 255.0);
			                  }
		                  }

		                  struct edge_sample
		                  {
			                  double angle = 0;
			                  double position = 0;
			                  double weight = 0;
			                  bool horizontal = false;
		                  };

		                  const auto gradient = [&](const int x, const int y)
		                  {
			                  const auto at = [&](const int xx, const int yy)
			                  {
				                  return grayscale[static_cast<size_t>(yy) * width + xx];
			                  };
			                  const auto gx = -at(x - 1, y - 1) - 2.0f * at(x - 1, y) - at(x - 1, y + 1) +
				                  at(x + 1, y - 1) + 2.0f * at(x + 1, y) + at(x + 1, y + 1);
			                  const auto gy = -at(x - 1, y - 1) - 2.0f * at(x, y - 1) - at(x + 1, y - 1) +
				                  at(x - 1, y + 1) + 2.0f * at(x, y + 1) + at(x + 1, y + 1);
			                  return std::pair{gx, gy};
		                  };

		                  float maximum_gradient = 0;
		                  for (auto y = 1; y < height - 1; ++y)
		                  {
			                  for (auto x = 1; x < width - 1; ++x)
			                  {
				                  const auto [gx, gy] = gradient(x, y);
				                  maximum_gradient = (std::max)(maximum_gradient, std::hypot(gx, gy));
			                  }
		                  }
		                  if (maximum_gradient < 0.02f) return {};

		                  constexpr auto pi = 3.14159265358979323846;
		                  constexpr auto half_pi = pi / 2.0;
		                  constexpr auto maximum_axis_angle = 25.0 * pi / 180.0;
		                  const auto gradient_threshold = (std::max)(0.08f, maximum_gradient * 0.10f);
		                  std::vector<edge_sample> samples;
		                  samples.reserve(static_cast<size_t>(width) * height / 8);

		                  for (auto y = 1; y < height - 1; ++y)
		                  {
			                  for (auto x = 1; x < width - 1; ++x)
			                  {
				                  const auto [gx, gy] = gradient(x, y);
				                  const auto magnitude = std::hypot(gx, gy);
				                  if (magnitude < gradient_threshold) continue;

				                  const auto tangent = std::atan2(gy, gx) + half_pi;
				                  const auto axis_angle = std::fmod(tangent + pi, pi);
				                  const auto horizontal = axis_angle < pi / 4.0 || axis_angle > 3.0 * pi / 4.0;
				                  const auto residual = std::remainder(tangent, half_pi);
				                  if (std::abs(residual) > maximum_axis_angle) continue;

				                  const auto position = horizontal
					                                        ? (y + 0.5) / height - 0.5
					                                        : (x + 0.5) / width - 0.5;
				                  samples.emplace_back(residual, position, magnitude, horizontal);
			                  }
		                  }
		                  if (samples.size() < 100) return {};

		                  constexpr auto inlier_angle = 12.0 * pi / 180.0;
		                  const auto estimate_angle = [&](const bool horizontal)
		                  {
			                  double initial_sum = 0;
			                  double initial_weight = 0;
			                  for (const auto& sample : samples)
			                  {
				                  if (sample.horizontal == horizontal)
				                  {
					                  initial_sum += sample.angle * sample.weight;
					                  initial_weight += sample.weight;
				                  }
			                  }
			                  if (initial_weight <= 0) return std::pair{0.0, 0.0};
			                  const auto initial = initial_sum / initial_weight;

			                  double angle_sum = 0;
			                  double weight_sum = 0;
			                  for (const auto& sample : samples)
			                  {
				                  if (sample.horizontal == horizontal && std::abs(sample.angle - initial) <=
					                  inlier_angle)
				                  {
					                  angle_sum += sample.angle * sample.weight;
					                  weight_sum += sample.weight;
				                  }
			                  }
			                  return std::pair{weight_sum > 0 ? angle_sum / weight_sum : 0.0, weight_sum};
		                  };

		                  const auto horizontal_estimate = estimate_angle(true);
		                  const auto vertical_estimate = estimate_angle(false);
		                  if (horizontal_estimate.second <= 0 && vertical_estimate.second <= 0) return {};

		                  const auto detected_angle = horizontal_estimate.second >= vertical_estimate.second
			                                              ? horizontal_estimate.first
			                                              : vertical_estimate.first;
		                  const auto straighten_correction = std::clamp(detected_angle * 180.0 / pi, -15.0, 15.0);
		                  const auto detected_straighten = std::clamp(straighten_correction, -45.0, 45.0);

		                  const auto perspective_slope = [&](const bool horizontal, const double baseline_angle)
		                  {
			                  constexpr auto bin_count = 10;
			                  std::array<double, bin_count> bin_angle_sum{};
			                  std::array<double, bin_count> bin_weight{};

			                  for (const auto& sample : samples)
			                  {
				                  if (sample.horizontal == horizontal && std::abs(sample.angle - baseline_angle) <=
					                  inlier_angle)
				                  {
					                  const auto bin = std::clamp(static_cast<int>((sample.position + 0.5) * bin_count),
					                                              0,
					                                              bin_count - 1);
					                  bin_angle_sum[bin] += sample.angle * sample.weight;
					                  bin_weight[bin] += sample.weight;
				                  }
			                  }

			                  double numerator = 0;
			                  double denominator = 0;
			                  auto populated_bins = 0;
			                  auto first_bin = bin_count;
			                  auto last_bin = -1;
			                  for (auto bin = 0; bin < bin_count; ++bin)
			                  {
				                  if (bin_weight[bin] > 0)
				                  {
					                  const auto position = (bin + 0.5) / bin_count - 0.5;
					                  const auto angle = bin_angle_sum[bin] / bin_weight[bin];
					                  numerator += position * (angle - baseline_angle);
					                  denominator += position * position;
					                  ++populated_bins;
					                  first_bin = (std::min)(first_bin, bin);
					                  last_bin = (std::max)(last_bin, bin);
				                  }
			                  }
			                  return populated_bins >= 5 && last_bin - first_bin >= 6 && denominator > 0.1
				                         ? numerator / denominator
				                         : 0.0;
		                  };

		                  const auto source_horizontal = std::clamp(-perspective_slope(true, horizontal_estimate.first),
		                                                            -0.4, 0.4);
		                  const auto source_vertical = std::clamp(perspective_slope(false, vertical_estimate.first),
		                                                          -0.4,
		                                                          0.4);
		                  const auto target_rotation = rotation_angle - current_straighten + detected_straighten;
		                  const auto display_angle = to_radian(-target_rotation);
		                  const auto angle_sin = sin(display_angle);
		                  const auto angle_cos = cos(display_angle);
		                  const auto horizontal = source_horizontal * angle_cos + source_vertical * angle_sin;
		                  const auto vertical = -source_horizontal * angle_sin + source_vertical * angle_cos;

		                  return [detected_straighten, horizontal, vertical](edit_view_state& state)
		                  {
			                  state._straighten = detected_straighten;
			                  state._perspective_horizontal = horizontal;
			                  state._perspective_vertical = vertical;
		                  };
	                  });
}

void edit_view::auto_document()
{
	const auto dimensions = _loaded.dimensions();
	const auto orientation = _loaded.orientation();

	queue_auto_adjust(768, std::string(tt.command_auto_document.sv()),
	                  [dimensions, orientation](const ui::const_surface_ptr& surface) ->
	                  std::function<void(edit_view_state&)>
	                  {
		                  const auto detected = detect_document(surface, dimensions);
		                  if (!detected) return {};

		                  // Propose the correction through the straighten, perspective and crop controls
		                  // so the page is presented upright while every value stays adjustable.
		                  const auto correction = fit_document_correction(detected.corners, dimensions, orientation);
		                  if (!correction) return {};

		                  return [correction](edit_view_state& state)
		                  {
			                  state._edits.crop_bounds(correction.crop);
			                  state._straighten = correction.straighten;
			                  state._perspective_horizontal = correction.perspective_horizontal;
			                  state._perspective_vertical = correction.perspective_vertical;
		                  };
	                  });
}

void edit_view::toggle_preview()
{
	_edit_state._preview_mode = !_edit_state._preview_mode;
	_state.invalidate_view(view_invalid::command_state | view_invalid::controller | view_invalid::view_redraw);
	_host->frame()->invalidate();
}

bool edit_view::escape()
{
	if (!_edit_state._preview_mode) return false;
	toggle_preview();
	return true;
}

void edit_view::changed()
{
	++_edit_generation;
	_state.invalidate_view(view_invalid::command_state);

	if (_mt && _edit_controls && _edit_controls->_straighten_slider)
	{
		const auto geometry_tracking = _edit_controls->_straighten_slider->is_tracking() ||
			_edit_controls->_perspective_horizontal_slider->is_tracking() ||
			_edit_controls->_perspective_vertical_slider->is_tracking();
		_edit_state.changed(_extent, geometry_tracking);

		if (_mt->has_trait(file_traits::bitmap))
		{
			_host->frame()->invalidate();
		}
		else
		{
			refresh();
		}
	}
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void edit_view_controls::layout_controls(ui::measure_context& mc)
{
	if (!_controls.empty())
	{
		const auto item = _state._edit_item;
		const auto* const ft = item ? item->file_type() : file_type::other;
		const auto is_bitmap = ft->has_trait(file_traits::bitmap);
		const auto show_pixel_controls = edit_pixel_controls_visible(is_bitmap, _view && _view->can_edit_pixels());
		_straighten_title->is_visible(show_pixel_controls);
		_straighten_slider->is_visible(show_pixel_controls);
		_perspective_horizontal_slider->is_visible(show_pixel_controls);
		_perspective_vertical_slider->is_visible(show_pixel_controls);
		_rotate_toolbar->is_visible(show_pixel_controls);
		_color_divider->is_visible(show_pixel_controls);
		_color_title->is_visible(show_pixel_controls);
		_vibrance_slider->is_visible(show_pixel_controls);
		_darks_slider->is_visible(show_pixel_controls);
		_midtones_slider->is_visible(show_pixel_controls);
		_lights_slider->is_visible(show_pixel_controls);
		_contrast_slider->is_visible(show_pixel_controls);
		_brightness_slider->is_visible(show_pixel_controls);
		_saturation_slider->is_visible(show_pixel_controls);
		_temperature_slider->is_visible(show_pixel_controls);
		_tint_slider->is_visible(show_pixel_controls);
		_color_toolbar->is_visible(show_pixel_controls);

		view_controls_host::layout_controls(mc);
	}
}


void edit_view_controls::options_changed()
{
	view_controls_host::options_changed();

	if (!_controls.empty())
	{
		_info->text(tt.edit_info);
		_straighten_title->text(tt.straighten);
		_perspective_horizontal_slider->label(tt.perspective_horizontal);
		_perspective_vertical_slider->label(tt.perspective_vertical);
		_color_title->text(tt.color);
		_vibrance_slider->label(tt.vibrance);
		_darks_slider->label(tt.darks);
		_midtones_slider->label(tt.midtones);
		_lights_slider->label(tt.lights);
		_contrast_slider->label(tt.contrast);
		_brightness_slider->label(tt.brightness);
		_saturation_slider->label(tt.saturation);
		_temperature_slider->label(tt.temperature);
		_tint_slider->label(tt.tint);
		_save_title->text(tt.options_save_options);
		_jpeg_quality_slider->label(tt.options_jpeg_quality);
		_webp_quality_slider->label(tt.options_webp_quality);
	}
}

void edit_view_controls::create_controls()
{
	const std::vector<ui::command_ptr> rotate_buttons =
	{
		std::make_shared<ui::command>(icon_index::rotate_anticlockwise, commands::tool_rotate_anticlockwise,
		                              [this] { _view->rotate_anticlockwise(); }),
		std::make_shared<ui::command>(icon_index::rotate_clockwise, commands::tool_rotate_clockwise,
		                              [this] { _view->rotate_clockwise(); }),
		std::make_shared<ui::command>(icon_index::lightbulb, commands::edit_item_auto_straighten,
		                              [this] { _view->auto_straighten(); }),
		std::make_shared<ui::command>(icon_index::scan_text, commands::edit_item_auto_document,
		                              [this] { _view->auto_document(); }),
		std::make_shared<ui::command>(icon_index::undo, commands::tool_rotate_reset,
		                              [this] { _view->rotate_reset(); }),
	};

	const std::vector<ui::command_ptr> color_buttons =
	{
		std::make_shared<ui::command>(icon_index::lightbulb, commands::edit_item_auto_color,
		                              [this] { _view->auto_color(); }),
		std::make_shared<ui::command>(icon_index::undo, commands::edit_item_color_reset,
		                              [this] { _view->color_reset(); })
	};


	auto changed_func = [this] { _view->changed(); };

	_info = create_view_info_element(tt.edit_info);
	_straighten_title = std::make_shared<ui::title_control>(icon_index::rotate_clockwise, tt.straighten);
	_straighten_slider = std::make_shared<log_slider_control>(_dlg, std::string_view{}, _edit_state._straighten,
	                                                          -44.9, 44.9, changed_func);
	_perspective_horizontal_slider = std::make_shared<log_slider_control>(
		_dlg, tt.perspective_horizontal, _edit_state._perspective_horizontal, -0.6, 0.6, changed_func);
	_perspective_vertical_slider = std::make_shared<log_slider_control>(
		_dlg, tt.perspective_vertical, _edit_state._perspective_vertical, -0.6, 0.6, changed_func);
	_rotate_toolbar = std::make_shared<task_toolbar_control>(_dlg, rotate_buttons);
	_color_divider = std::make_shared<divider_element>();
	_color_title = std::make_shared<ui::title_control>(icon_index::color, tt.color);
	_vibrance_slider = std::make_shared<log_slider_control>(_dlg, tt.vibrance, _edit_state._vibrance, -1, 1,
	                                                        changed_func);
	_darks_slider = std::make_shared<log_slider_control>(_dlg, tt.darks, _edit_state._darks, -1, 1, changed_func);
	_midtones_slider = std::make_shared<log_slider_control>(_dlg, tt.midtones, _edit_state._midtones, -1, 1,
	                                                        changed_func);
	_lights_slider = std::make_shared<log_slider_control>(_dlg, tt.lights, _edit_state._lights, -1, 1, changed_func);
	_contrast_slider = std::make_shared<log_slider_control>(_dlg, tt.contrast, _edit_state._contrast, -1, 1,
	                                                        changed_func);
	_brightness_slider = std::make_shared<log_slider_control>(_dlg, tt.brightness, _edit_state._brightness, -1, 1,
	                                                          changed_func);
	_saturation_slider = std::make_shared<log_slider_control>(_dlg, tt.saturation, _edit_state._saturation, -1, 1,
	                                                          changed_func);
	_temperature_slider = std::make_shared<log_slider_control>(_dlg, tt.temperature, _edit_state._temperature, -1, 1,
	                                                           changed_func);
	_tint_slider = std::make_shared<log_slider_control>(_dlg, tt.tint, _edit_state._tint, -1, 1, changed_func);
	_color_toolbar = std::make_shared<task_toolbar_control>(_dlg, color_buttons);
	_save_divider = std::make_shared<divider_element>();
	_save_title = std::make_shared<ui::title_control>(icon_index::save, tt.options_save_options);
	_backup_check = std::make_shared<ui::check_control>(_dlg, tt.options_backup_copy, setting.create_originals);
	_jpeg_quality_slider = std::make_shared<ui::slider_control>(_dlg, tt.options_jpeg_quality,
	                                                            setting.jpeg_save_quality, 0, 100);
	_webp_quality_slider = std::make_shared<ui::slider_control>(_dlg, tt.options_webp_quality, setting.webp_quality, 1,
	                                                            100);
	_webp_lossless_check = std::make_shared<ui::check_control>(_dlg, tt.lossless_compression, setting.webp_lossless);
	_controls = {
		_info,
		_straighten_title,
		_straighten_slider,
		_perspective_horizontal_slider,
		_perspective_vertical_slider,
		_rotate_toolbar,
		_color_divider,
		_color_title,
		_vibrance_slider,
		_darks_slider,
		_midtones_slider,
		_lights_slider,
		_contrast_slider,
		_brightness_slider,
		_saturation_slider,
		_temperature_slider,
		_tint_slider,
		_color_toolbar,
		_save_divider,
		_save_title,
		_backup_check,
		_jpeg_quality_slider,
		_webp_quality_slider,
		_webp_lossless_check,
	};

	_clr = ui::color(ui::style::color::dialog_text);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void edit_view_state::reset(const prop::item_metadata_const_ptr& md, const sizei dimensions,
                            const ui::orientation orientation)
{
	_edits.clear();

	_straighten = 0;
	_perspective_horizontal = 0;
	_perspective_vertical = 0;
	_vibrance = 0;
	_darks = 0;
	_midtones = 0;
	_lights = 0;
	_contrast = 0;
	_brightness = 0;
	_saturation = 0;
	_temperature = 0;
	_tint = 0;
	_preview_mode = false;

	_edits.crop_bounds(initial_crop(dimensions, orientation));
	_original_edits = _edits;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void edit_view::display_changed()
{
	const auto display = _state.display_state();
	const auto item = display ? display->_item1 : df::item_element_ptr{};

	_state._edit_item = item;
	_loaded.clear();
	_preview_source.reset();
	_dialog_preview_source.reset();
	_preview_request_dimensions = {};
	_preview_failed_request = {};
	_texture.reset();
	_invalid = true;
	clear_crop_interaction_bounds();
	_source_loading = false;
	_pending_source_load_request.reset();
	_pending_preview_decode_request.reset();
	const auto display_generation = ++_display_generation;
	++_preview_generation;

	if (item)
	{
		_path = item->path();
		_xmp_name = item->xmp();
		_mt = item->file_type();
		auto pending_crop = _state.take_pending_edit_crop(_path);

		if (_mt->has_trait(file_traits::bitmap))
		{
			_source_loading = true;
			queue_source_load({_path, display_generation, pending_crop});
		}
		else
		{
			_loaded = display ? display->_selected_texture1->loaded() : file_load_result{};
		}
	}
	else
	{
		_path.clear();
		_mt = file_type::other;
	}

	update_media_elements();

	_edit_state.reset(safe_metadata(item), _loaded.dimensions(), _loaded.orientation());

	if (_edit_controls->_dlg)
	{
		_edit_controls->populate();
		_edit_controls->_dlg->layout();
	}

	changed();
}

void edit_view::draw_handle(ui::draw_context& dc, const recti handle_bounds2, const float alpha)
{
	const auto handle_clr = ui::color(ui::style::color::dialog_selected_background, alpha * dc.colors.bg_alpha);
	const auto handle_bg_clr = ui::color(ui::style::color::dialog_selected_background, alpha * dc.colors.bg_alpha);

	dc.draw_rect(handle_bounds2, handle_clr);
	dc.draw_border(handle_bounds2.inflate(df::round(-2 * dc.scale_factor)), handle_bounds2, handle_bg_clr,
	               handle_bg_clr);
}

void edit_view::render(ui::draw_context& dc, view_controller_ptr controller)
{
	auto clip_rect = dc.clip_bounds();

	if (is_photo())
	{
		const auto image_extent = _loaded.dimensions();
		const auto alpha = dc.colors.alpha;

		const quadd image_bounds(image_extent);

		const auto pad30 = dc.padding2 * 2;
		const auto selection_bounds = _edit_state._edits.effective_crop_bounds(image_extent);
		const auto selection_angle = -selection_bounds.angle();
		const auto preview_bounds = _edit_state._preview_mode ? selection_bounds : image_bounds;
		const auto selection_rotated_bounds = preview_bounds.transform(affined().rotate(selection_angle)).
		                                                     bounding_rect();
		const auto view_bounds = rectd(0, 0, _extent.cx, _extent.cy);
		const auto view_scale = std::min((view_bounds.Width - pad30) / selection_rotated_bounds.Width,
		                                 (view_bounds.Height - pad30) / selection_rotated_bounds.Height);

		_image_transform = affined().translate(-image_bounds.center_point()).rotate(selection_angle).scale(view_scale).
		                             translate(view_bounds.center());
		if (_edit_state._preview_mode)
		{
			const auto transformed_crop = selection_bounds.transform(_image_transform).bounding_rect();
			_image_transform = _image_transform.translate(view_bounds.center() - transformed_crop.center());
		}

		const auto image_view_bounds = image_bounds.transform(_image_transform);

		if (_invalid || !_texture)
		{
			const auto preview_dimensions = ui::scale_dimensions(_loaded.dimensions(), _extent);
			const auto has_current_preview = edit_preview_is_current(_preview_source, _preview_source_request,
			                                                         preview_dimensions);
			if (!has_current_preview)
			{
				queue_preview_decode();

				// With no texture, the last good preview stands in: it shows the whole picture at
				// another size, which is better than Loading while the current one is decoded - or
				// for good, if this size cannot be.
				if (!(_texture && _texture->is_valid()) && is_valid(_preview_source))
				{
					if (const auto t = dc.create_texture();
						t && t->update(_preview_source) != ui::texture_update_result::failed)
					{
						_texture = t;
					}
				}

				const auto decision = decide_edit_preview_render(_texture && _texture->is_valid(), false, true);
				if (!decision.draw_texture)
				{
					clear_crop_interaction_bounds();
					draw_loading_status(dc, tt.loading.sv());
					return;
				}
			}
			else
			{
				auto t = dc.create_texture();
				if (t && t->update(_preview_source) != ui::texture_update_result::failed)
				{
					_texture = t;
					_invalid = false;
				}
			}
		}

		if (!_texture)
		{
			_invalid = true;
			clear_crop_interaction_bounds();
			draw_loading_status(dc, tt.loading.sv());
			return;
		}

		const auto sampler = calc_sampler(image_view_bounds.extent().round(), _texture->dimensions(),
		                                  ui::orientation::top_left);

		ui::texture_transform texture_transform;
		if (_edit_state._edits.has_perspective())
		{
			const auto display_angle = to_radian(-_edit_state._edits.rotation_angle());
			const auto angle_sin = sin(display_angle);
			const auto angle_cos = cos(display_angle);
			texture_transform.perspective_horizontal = static_cast<float>(
				_edit_state._edits.perspective_horizontal() * angle_cos -
				_edit_state._edits.perspective_vertical() * angle_sin);
			texture_transform.perspective_vertical = static_cast<float>(
				_edit_state._edits.perspective_horizontal() * angle_sin +
				_edit_state._edits.perspective_vertical() * angle_cos);
			texture_transform.has_perspective = true;
		}
		if (_edit_state._edits.has_color_changes())
		{
			ui::color_adjust adjust;
			adjust.color_params(_edit_state._edits.vibrance(), _edit_state._edits.saturation(),
			                    _edit_state._edits.darks(), _edit_state._edits.midtones(), _edit_state._edits.lights(),
			                    _edit_state._edits.contrast(), _edit_state._edits.brightness(),
			                    _edit_state._edits.temperature(), _edit_state._edits.tint());
			adjust.populate_texture_transform(texture_transform);
		}
		if (_edit_state._preview_mode)
		{
			const auto outer_padding = dc.padding2 * 2;
			const auto column_gap = dc.padding2 * 2;
			const auto label_height = dc.text_line_height(ui::style::font_face::dialog) + dc.padding1;
			const quadd original_image_bounds(_loaded.dimensions());
			const auto original_selection_bounds = _edit_state._original_edits.effective_crop_bounds(
				_loaded.dimensions());
			const auto original_angle = -original_selection_bounds.angle();
			const auto column_width = std::max(1, (_extent.cx - outer_padding * 2 - column_gap) / 2);
			const auto image_top = outer_padding + label_height;
			const recti original_bounds(outer_padding, image_top, outer_padding + column_width,
			                            std::max(image_top + 1, _extent.cy - outer_padding));
			const recti edited_bounds(original_bounds.right + column_gap, image_top,
			                          std::max(original_bounds.right + column_gap + 1, _extent.cx - outer_padding),
			                          original_bounds.bottom);

			const auto comparison_transform = [&](const recti bounds)
			{
				const rectd target_bounds(bounds);
				const auto scale = std::min(target_bounds.Width / selection_rotated_bounds.Width,
				                            target_bounds.Height / selection_rotated_bounds.Height);
				const auto transform = affined().
				                       translate(-image_bounds.center_point()).rotate(selection_angle).scale(scale).
				                       translate(target_bounds.center());
				const auto transformed_crop = selection_bounds.transform(transform).bounding_rect();
				return transform.translate(target_bounds.center() - transformed_crop.center());
			};
			const auto original_transform = [&](const recti bounds)
			{
				const rectd target_bounds(bounds);
				const auto original_rotated_bounds = original_selection_bounds.transform(
					affined().rotate(original_angle)).bounding_rect();
				const auto scale = std::min(target_bounds.Width / original_rotated_bounds.Width,
				                            target_bounds.Height / original_rotated_bounds.Height);
				const auto transform = affined().
				                       translate(-original_image_bounds.center_point()).rotate(original_angle).
				                       scale(scale).translate(target_bounds.center());
				const auto transformed_crop = original_selection_bounds.transform(transform).bounding_rect();
				return transform.translate(target_bounds.center() - transformed_crop.center());
			};

			const auto draw_comparison_image = [&](const recti bounds, const affined& transform, const bool edited)
			{
				const auto& comparison_image_bounds = edited ? image_bounds : original_image_bounds;
				const auto transformed_image_bounds = comparison_image_bounds.transform(transform);
				const auto crop_rect = (edited ? selection_bounds : original_selection_bounds).transform(transform).
					bounding_rect();
				constexpr auto rounding_tolerance = 1e-6;
				const recti crop_clip(static_cast<int>(std::ceil(crop_rect.left() - rounding_tolerance)),
				                      static_cast<int>(std::ceil(crop_rect.top() - rounding_tolerance)),
				                      static_cast<int>(std::floor(crop_rect.right() + rounding_tolerance)),
				                      static_cast<int>(std::floor(crop_rect.bottom() + rounding_tolerance)));
				const auto comparison_clip = crop_clip.intersection(bounds).intersection(clip_rect);
				const auto comparison_sampler = calc_sampler(transformed_image_bounds.extent().round(),
				                                             _texture->dimensions(), ui::orientation::top_left);
				dc.clip_bounds(comparison_clip);
				if (edited && texture_transform.has_changes())
					dc.draw_texture(_texture, transformed_image_bounds, _texture->dimensions(), alpha,
					                comparison_sampler, texture_transform);
				else
					dc.draw_texture(_texture, transformed_image_bounds, _texture->dimensions(), alpha,
					                comparison_sampler);
				dc.restore_clip();
			};

			const auto original_transform_value = original_transform(original_bounds);
			const auto edited_transform = comparison_transform(edited_bounds);
			draw_comparison_image(original_bounds, original_transform_value, false);
			draw_comparison_image(edited_bounds, edited_transform, true);
			_image_transform = edited_transform;

			const auto text_color = ui::color(dc.colors.foreground, alpha);
			const recti original_label(original_bounds.left, outer_padding, original_bounds.right, image_top);
			const recti edited_label(edited_bounds.left, outer_padding, edited_bounds.right, image_top);
			dc.draw_text(tt.edit_original, original_label, ui::style::font_face::dialog,
			             ui::style::text_style::single_line_center, text_color, {});
			dc.draw_text(tt.command_edit_preview, edited_label, ui::style::font_face::dialog,
			             ui::style::text_style::single_line_center, text_color, {});
			return;
		}

		if (texture_transform.has_changes())
			dc.draw_texture(_texture, image_view_bounds, _texture->dimensions(), alpha, sampler, texture_transform);
		else
			dc.draw_texture(_texture, image_view_bounds, _texture->dimensions(), alpha, sampler);

		const auto draw_crop = selection_bounds.crop(rectd(0, 0, image_extent.cx, image_extent.cy));
		const auto actual_size = draw_crop.actual_extent();
		const auto crop_bounding = draw_crop.transform(_image_transform).bounding_rect();
		const auto border = recti(0, 0, _extent.cx, _extent.cy);

		_crop_bounds = crop_bounding;

		const auto pad1 = df::round(1 * dc.scale_factor);
		const auto pad8 = dc.padding2;
		const auto pad16 = pad8 * 2;

		_crop_handle_tl.set(crop_bounding.left() - pad16, crop_bounding.top() - pad16, crop_bounding.left() + pad8,
		                    crop_bounding.top() + pad8);
		_crop_handle_tr.set(crop_bounding.right() - pad8, crop_bounding.top() - pad16, crop_bounding.right() + pad16,
		                    crop_bounding.top() + pad8);
		_crop_handle_bl.set(crop_bounding.left() - pad16, crop_bounding.bottom() - pad8, crop_bounding.left() + pad8,
		                    crop_bounding.bottom() + pad16);
		_crop_handle_br.set(crop_bounding.right() - pad8, crop_bounding.bottom() - pad8, crop_bounding.right() + pad16,
		                    crop_bounding.bottom() + pad16);

		const auto bounding = crop_bounding.round();
		auto c1 = ui::color(0, alpha / 2.0f);
		dc.draw_border(bounding, border, c1, c1);
		auto c2 = ui::color(ui::style::color::dialog_selected_background, alpha);
		dc.draw_border(bounding, bounding.inflate(pad1), c2, c2);

		const auto grid_alpha = _edit_state.grid_alpha_animation.val();

		if (grid_alpha > 0)
		{
			const auto c = ui::color(0, grid_alpha);

			// draw grid
			const auto center = bounding.center();
			dc.draw_rect(recti(center.x - pad1, bounding.top, center.x + pad1, bounding.bottom), c);
			dc.draw_rect(recti(bounding.left, center.y - pad1, bounding.right, center.y + pad1), c);

			for (int div = 6; div >= 3; div /= 2)
			{
				const auto l = center.x - bounding.width() / div;
				const auto r = center.x + bounding.width() / div;
				const auto t = center.y - bounding.height() / div;
				const auto b = center.y + bounding.height() / div;

				dc.draw_rect(recti(bounding.left, t - pad1, bounding.right, t + pad1), c);
				dc.draw_rect(recti(bounding.left, b - pad1, bounding.right, b + pad1), c);
				dc.draw_rect(recti(l - pad1, bounding.top, l + pad1, bounding.bottom), c);
				dc.draw_rect(recti(r - pad1, bounding.top, r + pad1, bounding.bottom), c);
			}
		}

		draw_handle(dc, _crop_handle_tl.round(), alpha);
		draw_handle(dc, _crop_handle_tr.round(), alpha);
		draw_handle(dc, _crop_handle_bl.round(), alpha);
		draw_handle(dc, _crop_handle_br.round(), alpha);

		const auto text_dims = std::format("{}x{}", df::round(actual_size.Width), df::round(actual_size.Height));
		const sizei text_size = dc.measure_text(text_dims, ui::style::font_face::dialog,
		                                        ui::style::text_style::single_line_center, bounding.width()).inflate(
			dc.padding2);
		const auto text_x = (bounding.left + bounding.right - text_size.cx) / 2;
		const recti draw_text_rect(text_x, bounding.top - text_size.cy, text_x + text_size.cx, bounding.top);

		dc.draw_text(text_dims, draw_text_rect, ui::style::font_face::dialog, ui::style::text_style::single_line_center,
		             ui::color(dc.colors.foreground, alpha), {});

		if (setting.show_debug_info)
		{
			const auto text_degs = str::print("%3.3f degrees", selection_angle);
			dc.draw_text(text_degs, crop_bounding.round().inflate(df::round(100 * dc.scale_factor)),
			             ui::style::font_face::title,
			             ui::style::text_style::single_line_center,
			             ui::color(ui::style::color::warning_background, alpha), {});
		}
	}
	else if (!_loaded.is_empty())
	{
		if (_media_element)
		{
			_media_element->render(dc, {0, 0});
			_play_element->render(dc, {0, 0});
			_scrubber_element->render(dc, {0, 0});
			return;
		}

		const auto alpha = dc.colors.alpha;
		const auto dims = ui::scale_dimensions(_loaded.dimensions(), _extent, true);
		const auto bounds = center_rect(dims, _extent);

		if (_invalid || !_texture)
		{
			if (!is_valid(_preview_source))
			{
				queue_preview_decode();
				const auto decision = decide_edit_preview_render(_texture && _texture->is_valid(), false, true);
				if (!decision.draw_texture)
				{
					draw_loading_status(dc, tt.loading.sv());
					return;
				}
			}
			else
			{
				auto t = dc.create_texture();
				if (t && t->update(_preview_source) != ui::texture_update_result::failed)
				{
					_texture = t;
					_invalid = false;
				}
			}
		}

		if (!_texture)
		{
			_invalid = true;
			draw_loading_status(dc, tt.loading.sv());
			return;
		}

		const auto sampler = calc_sampler(bounds.extent(), _texture->dimensions(), ui::orientation::top_left);
		dc.draw_texture(_texture, bounds, alpha, sampler);

		auto draw_text_rect = recti(_extent);
		draw_text_rect.top = bounds.bottom + dc.padding2;
		draw_text_rect.bottom = draw_text_rect.top + dc.text_line_height(ui::style::font_face::title) + dc.padding2 *
			2;

		dc.draw_text(_path.name(), draw_text_rect, ui::style::font_face::title,
		             ui::style::text_style::single_line_center, ui::color(dc.colors.foreground, alpha), {});
	}
	else
	{
		const auto status = edit_load_status_text(_loaded, _source_loading);
		draw_loading_status(dc, status);
	}
}
