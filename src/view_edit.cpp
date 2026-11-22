// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Edit view. Implements a photo's pixel adjustments - drawn with a live histogram and compared with the
// original by a before-and-after handle or side by side - and a video's or a sound file's lossless trim, sound removal
// and turn, previewed on the player before a save copies - or repairs - the file.

#include "pch.h"
#include "model_index.h"
#include "view_edit.h"
#include "app_text.h"
#include "files.h"
#include "ui_controls.h"

#include "libfiles/identify.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

std::string format_edit_adjustment(const double value)
{
	auto text = std::format("{:.2f}", value);
	if (text == "-0.00") text.erase(0, 1);
	return text;
}

class log_slider_control final :
	public view_element,
	public ui::labelled_row,
	public std::enable_shared_from_this<log_slider_control>
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
	log_slider_control(ui::frame_ptr& /*h*/, const std::string_view text, double& v, double min, double max,
	                   std::function<void()> changed) noexcept :
		_label(text), _val(v), _min(min), _max(max), _tracking(false), _changed(std::move(changed))
	{
		ui::edit_styles edit_style;
		edit_style.align_center = true;
		_edit = ui::make_edit(edit_style, {}, [this](const std::string_view text) { edit_change(text); });
		_slider = ui::make_slider(-Range, Range,
		                          [this](const int pos, const bool is_tracking) { slider_change(pos, is_tracking); });
	}

	void label(const std::string_view label) { _label = label; }
	std::string_view row_label() const override { return _label; }

	void visit_children(const std::function<void(const view_element_ptr&)>& visit) const override
	{
		visit(_slider);
		visit(_edit);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::populate)
		{
			update_slider();
			update_edit();
		}
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, ui::layout_slider_row(mc, _label, {0, 0, width_limit, 0}).height};
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;
		_row = ui::layout_slider_row(mc, _label, bounds);
		_slider->layout(mc, _row.slider);
		_edit->layout(mc, _row.edit);
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		if (!core::is_empty(_label))
		{
			const auto text_color = core::color(dc.colors.foreground, dc.colors.alpha);
			dc.draw_text(_label, _row.label.offset(element_offset), render::font_face::dialog,
			             _row.stacked ? render::text_style::multiline : render::text_style::single_line, text_color,
			             {});
		}

		render_children(dc, element_offset);
	}

	int value_to_slider(const double a) const
	{
		const auto d = a / _max;
		return core::round(sqrt(fabs(d)) * (a < 0 ? -Range : Range));
	}

	double slider_to_value(const double s) const
	{
		const auto d = s / Range;
		return d * d * (s < 0 ? _min : _max);
	}

	void update_edit() const
	{
		const auto actual = _edit->window_text();
		const auto expected = format_edit_adjustment(_val);

		if (actual.empty() || actual != expected)
		{
			_edit->window_text(expected);
		}
	}

	void update_slider() const
	{
		if (!core::equiv(_val, slider_to_value(_slider->get_pos())))
		{
			_slider->set_pos(value_to_slider(_val));
		}
	}

	void edit_change(const std::string_view text)
	{
		_val = std::clamp(core::to_double(text), _min, _max);
		update_slider();
		_changed();
	}

	void slider_change(const int pos, const bool is_tracking)
	{
		_tracking = is_tracking;
		// The value is the one the box shows, to the hundredth, wherever the thumb lands between.
		_val = std::clamp(core::to_double(format_edit_adjustment(slider_to_value(pos))), _min, _max);
		update_edit();
		update_slider();
		_changed();
	}

	bool is_tracking() const { return _tracking; }
};

namespace
{
	constexpr double detection_pi = 3.14159265358979323846;
	constexpr double minimum_document_confidence = 0.55;
	constexpr double clear_document_confidence = 0.80;

	// The BT.709 luma of a packed pixel, 0 to 1 - the luma the colour grade's tone curve reads. A packed
	// surface stores blue in the first byte, which core::get_r names, so red is get_b: reading get_r as red
	// weighted blue as red and reversed Auto colour's white balance.
	double packed_luma(const core::color32 c)
	{
		return (0.2126 * core::get_b(c) + 0.7152 * core::get_g(c) + 0.0722 * core::get_r(c)) / 255.0;
	}

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
static document_candidate detect_document_by_region(const core::const_surface_ptr& surface,
                                                    const std::vector<float>& luminance, const int width,
                                                    const int height)
{
	document_candidate candidate;
	const auto pixel_count = static_cast<size_t>(width) * height;
	std::array<uint32_t, 256> histogram{};

	for (const auto value : luminance)
	{
		++histogram[std::clamp(core::round(value * 255), 0, 255)];
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
		const auto* pixels = std::bit_cast<const core::color32*>(surface->pixels_line(y));
		for (auto x = 0; x < width; ++x)
		{
			const auto color = pixels[x];
			const auto maximum = std::max({core::get_r(color), core::get_g(color), core::get_b(color)});
			const auto minimum = std::min({core::get_r(color), core::get_g(color), core::get_b(color)});
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
			for (auto yy = (std::max)(0, y - 2);
			     yy <= (std::min)(height - 1, y + 2) && !expanded[static_cast<size_t>(y) * width + x]; ++yy)
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
			const auto x = std::clamp(core::round(a.X + (b.X - a.X) * position), 2, width - 3);
			const auto y = std::clamp(core::round(a.Y + (b.Y - a.Y) * position), 2, height - 3);
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
			const auto gx = -at(x - 1, y - 1) - 2.0f * at(x - 1, y) - at(x - 1, y + 1) + at(x + 1, y - 1) +
			                2.0f * at(x + 1, y) + at(x + 1, y + 1);
			const auto gy = -at(x - 1, y - 1) - 2.0f * at(x, y - 1) - at(x + 1, y - 1) + at(x - 1, y + 1) +
			                2.0f * at(x, y + 1) + at(x + 1, y + 1);
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
		++magnitude_histogram[std::clamp(core::round(value / maximum_magnitude * 255), 0, 255)];
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
				                            static_cast<double>(
												accumulator[static_cast<size_t>(step) * rho_bins + bin]));
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
			const auto duplicate = std::ranges::any_of(selected, [&](const hough_line& other) {
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
		point = {(a.rho * std::sin(b.theta) - b.rho * std::sin(a.theta)) / determinant,
		         (b.rho * std::cos(a.theta) - a.rho * std::cos(b.theta)) / determinant};
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
		if (corner.X < -margin_x || corner.X > width + margin_x || corner.Y < -margin_y || corner.Y > height + margin_y)
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
			const auto x = std::clamp(core::round(a.X + (b.X - a.X) * position), 1, width - 2);
			const auto y = std::clamp(core::round(a.Y + (b.Y - a.Y) * position), 1, height - 2);
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
		result.corners[index] = {pixel_corners[index].X / width * source_extent.cx,
		                         pixel_corners[index].Y / height * source_extent.cy};
	}

	const auto top_width = pixel_corners[0].dist(pixel_corners[1]);
	const auto bottom_width = pixel_corners[3].dist(pixel_corners[2]);
	const auto left_height = pixel_corners[0].dist(pixel_corners[3]);
	const auto right_height = pixel_corners[1].dist(pixel_corners[2]);
	result.extent = {(std::max)(32, core::round((top_width + bottom_width) * 0.5 / width * source_extent.cx)),
	                 (std::max)(32, core::round((left_height + right_height) * 0.5 / height * source_extent.cy))};
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
                                            const core::orientation orientation)
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
	const auto horizon = cross_product(cross_product(cross_product(page[0], page[1]), cross_product(page[3], page[2])),
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
		warped[index] = {(0.5 + page[index].x / denominator) * extent.cx,
		                 (0.5 + page[index].y / denominator) * extent.cy};
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
	result.crop =
		quadd(left, top, right, bottom).rotate(rotation, centre).transform(to_simple_transform_inv(orientation));
	result.straighten = edit_view_state::calc_straighten(result.crop.angle());

	// The renderer rotates the perspective axes by the crop angle, so undo that here.
	const auto crop_angle = to_radian(result.crop.angle());
	const auto angle_sin = sin(crop_angle);
	const auto angle_cos = cos(crop_angle);
	result.perspective_horizontal = horizontal * angle_cos - vertical * angle_sin;
	result.perspective_vertical = horizontal * angle_sin + vertical * angle_cos;
	return result;
}

document_detection_result detect_document(const core::const_surface_ptr& picture, const sizei source_extent)
{
	// Read a byte a channel, whatever precision the picture was loaded at.
	const auto surface = core::to_8_bit(picture);
	if (!is_valid(surface) || surface->width() < 32 || surface->height() < 32) return {};

	const auto width = static_cast<int>(surface->width());
	const auto height = static_cast<int>(surface->height());
	std::vector<float> luminance(static_cast<size_t>(width) * height);

	for (auto y = 0; y < height; ++y)
	{
		const auto* pixels = std::bit_cast<const core::color32*>(surface->pixels_line(y));
		for (auto x = 0; x < width; ++x)
		{
			luminance[static_cast<size_t>(y) * width + x] = static_cast<float>(packed_luma(pixels[x]));
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
	const log_slider_control* sliders[] = {_straighten_slider.get(),
	                                       _perspective_horizontal_slider.get(),
	                                       _perspective_vertical_slider.get(),
	                                       _vibrance_slider.get(),
	                                       _darks_slider.get(),
	                                       _midtones_slider.get(),
	                                       _lights_slider.get(),
	                                       _contrast_slider.get(),
	                                       _brightness_slider.get(),
	                                       _saturation_slider.get(),
	                                       _temperature_slider.get(),
	                                       _tint_slider.get()};

	return std::ranges::any_of(sliders,
	                           [](const log_slider_control* slider) { return slider && slider->is_tracking(); });
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class task_toolbar_control final : public view_element, public std::enable_shared_from_this<task_toolbar_control>
{
	ui::toolbar_ptr _tb;

public:
	task_toolbar_control(const ui::frame_ptr& /*h*/, const std::vector<ui::command_ptr>& buttons)
	{
		_tb = ui::make_toolbar({}, buttons);
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, ui::default_control_height(mc)};
	}

	void visit_children(const std::function<void(const view_element_ptr&)>& visit) const override { visit(_tb); }

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;
		const auto extent = _tb->measure_toolbar(mc, bounds.width());
		const auto r = center_rect(extent, bounds);
		_tb->layout(mc, r);
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Five stars, as many filled as the rating; pointing at one shows the rating choosing it would give, and
// choosing it gives it.
class star_rating final :
	public view_element,
	public std::enable_shared_from_this<star_rating>,
	public keyboard_parts_source
{
public:
	int _rating = -1;
	int _hover_rating = -1;
	int _cell_width = 18;
	std::function<void(int)> _chosen;

	explicit star_rating(std::function<void(int)> chosen) : _chosen(std::move(chosen)) {}

	int point_to_stars(const pointi loc) const
	{
		return std::clamp(1 + (loc.x - bounds.left) / std::max(1, _cell_width), 0, 5);
	}

	// A star each, where point_to_stars finds it.
	std::vector<recti> keyboard_parts() const override
	{
		std::vector<recti> parts;
		parts.reserve(5);
		const auto cell = std::max(1, _cell_width);
		for (auto i = 0; i < 5; ++i)
			parts.emplace_back(bounds.left + i * cell, bounds.top, bounds.left + (i + 1) * cell, bounds.bottom);
		return parts;
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;
		_cell_width = mc.icon_cxy;
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto hover = _hover_rating >= 0;
		const auto bg = hover ? view_handle_color(false, true, false, dc.frame_has_focus, true).aa(dc.colors.alpha)
		                      : core::color{};
		const auto rating = hover ? _hover_rating : _rating;
		const auto clr = core::color(dc.colors.foreground, dc.colors.alpha);

		std::string text;

		for (auto i = 0; i < 5; i++)
		{
			text += icon_to_utf8(i < rating ? icon_index::star_solid : icon_index::star);
		}

		dc.draw_text(text, bounds.offset(element_offset), render::font_face::icons, render::text_style::single_line,
		             clr, bg);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
};

class star_rating_controller final : public view_controller
{
	std::shared_ptr<star_rating> _stars;
	pointi _origin;
	bool _pressed = false;

	void hover_at(const int stars) const
	{
		if (_stars->_hover_rating == stars) return;
		_stars->_hover_rating = stars;
		_host->invalidate_element(_stars);
	}

public:
	star_rating_controller(const view_host_ptr& host, std::shared_ptr<star_rating> stars, const pointi origin) :
		view_controller(host, stars->bounds.offset(origin)), _stars(std::move(stars)), _origin(origin)
	{
	}

	~star_rating_controller() override
	{
		_stars->_hover_rating = -1;
		_host->invalidate_element(_stars);
	}

	platform::cursor cursor() const override { return platform::cursor::link; }

	void on_mouse_move(const pointi loc) override { hover_at(_stars->point_to_stars(loc - _origin)); }

	void on_mouse_left_button_down(const pointi /*loc*/, const platform::key_state /*keys*/) override
	{
		_pressed = true;
	}

	// The rating is the click's, so a release whose press went elsewhere - the one that closed a menu - rates nothing.
	void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
	{
		if (!std::exchange(_pressed, false)) return;
		_stars->_rating = _stars->point_to_stars(loc - _origin);
		_host->invalidate_element(_stars);
		if (_stars->_chosen) _stars->_chosen(_stars->_rating);
	}
};

view_controller_ptr star_rating::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                          const pointi element_offset, hit_test_context& /*ctx*/)
{
	if (!bounds.offset(element_offset).contains(loc)) return nullptr;
	return std::make_shared<star_rating_controller>(host, shared_from_this(), element_offset);
}

using star_rating_ptr = std::shared_ptr<star_rating>;

class rating_bar_control final : public view_element, public std::enable_shared_from_this<rating_bar_control>
{
	star_rating_ptr _stars;
	ui::edit_ptr _edit;
	ui::frame_ptr _owner;
	std::string _label;
	int& _val;

public:
	rating_bar_control(ui::frame_ptr owner, const std::string_view label, int& v) :
		_owner(std::move(owner)), _label(label), _val(v)
	{
		ui::edit_styles edit_style;
		edit_style.number = true;
		edit_style.align_center = true;
		_edit = ui::make_edit(edit_style, {}, [this](const std::string_view text) { edit_change(text); });
		_stars = std::make_shared<star_rating>([this](const int stars)
		{
			_val = std::clamp(stars, 0, 5);
			_edit->window_text(_val < 1 ? std::string{} : core::to_string(_val));
		});
	}

	void label(const std::string_view label) { _label = label; }

	void visit_children(const std::function<void(const view_element_ptr&)>& visit) const override
	{
		visit(_edit);
		visit(_stars);
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		if (!core::is_empty(_label))
		{
			const auto label_extent = mc.measure_text(_label, render::font_face::dialog,
			                                          render::text_style::single_line, width_limit);
			mc.col_widths.c1 = std::max(mc.col_widths.c1, label_extent.cx + mc.padding2);
		}

		return {width_limit, ui::default_control_height(mc)};
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;
		auto rStars = bounds;
		auto rEdit = bounds;

		if (!core::is_empty(_label)) rEdit.left += mc.col_widths.c1;
		rEdit.right = std::min(rEdit.left + core::round(50 * mc.scale_factor), bounds.right);
		rStars.left = std::min(rEdit.right + mc.padding2, bounds.right);

		_edit->layout(mc, rEdit);
		_stars->layout(mc, rStars);
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		if (!core::is_empty(_label))
		{
			auto r = bounds.offset(element_offset);
			r.right = r.left + dc.col_widths.c1 - dc.padding2;
			dc.draw_text(_label, r, render::font_face::dialog, render::text_style::single_line,
			             {dc.colors.foreground, dc.colors.alpha}, {});
		}

		render_children(dc, element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return _stars->controller_from_location(host, loc, element_offset, ctx);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::populate)
		{
			_edit->window_text(core::to_string(_val));
			_stars->_rating = _val;
		}
	}

	void edit_change(const std::string_view text) const
	{
		_val = std::clamp(core::to_int(text), 0, 5);

		if (_stars->_rating != _val)
		{
			_stars->_rating = _val;
			if (_owner) _owner->invalidate();
		}
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// The colour histogram above the colour sliders: the photo's red, green and blue as the edit leaves them. Under it a
// handle each for the darks, the midtones and the lights stands where its slider has moved that point of the tone
// curve, and dragging one - or the histogram above it - moves the slider as dragging the slider does.
class edit_histogram_control final : public view_element, public std::enable_shared_from_this<edit_histogram_control>
{
public:
	struct tone_handle
	{
		double& value;
		const core::text_t& label;
	};

	std::function<const edit_histogram&()> _histogram;
	std::array<tone_handle, 3> _handles;
	std::function<void()> _changed;
	int _hover = -1;
	int _held = -1;
	recti _plot;
	recti _track;

	edit_histogram_control(std::function<const edit_histogram&()> histogram, std::array<tone_handle, 3> handles,
	                       std::function<void()> changed) :
		_histogram(std::move(histogram)), _handles(handles), _changed(std::move(changed))
	{
	}

	static int plot_height(const render::measure_context& mc) { return core::round(56 * mc.scale_factor); }

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, mc.padding1 + plot_height(mc) + mc.handle_cxy};
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;
		// Half a handle in from each side, so a handle at either end of its reach stays in the panel.
		const auto inset = mc.handle_cxy / 2 + mc.padding1;
		const auto top = bounds.top + mc.padding1;
		_plot = recti(bounds.left + inset, top, std::max(bounds.left + inset + 1, bounds.right - inset),
		              top + plot_height(mc));
		_track = recti(_plot.left, _plot.bottom, _plot.right, _plot.bottom + mc.handle_cxy);
	}

	int handle_x(const int handle) const
	{
		return _plot.left + core::round(edit_tone_handle_position(handle, _handles[handle].value) * _plot.width());
	}

	// The handle at loc, in the element's coordinates: the nearest within a handle's width of it, on the track or the
	// histogram above it.
	int handle_at(const pointi loc, const int reach) const
	{
		if (loc.y < _plot.top || loc.y >= _track.bottom) return -1;

		auto found = -1;
		auto nearest = reach + 1;

		for (auto i = 0; i < static_cast<int>(_handles.size()); ++i)
		{
			const auto distance = std::abs(loc.x - handle_x(i));

			if (distance < nearest)
			{
				nearest = distance;
				found = i;
			}
		}

		return found;
	}

	recti handle_bounds(const int handle, const int reach) const
	{
		const auto x = handle_x(handle);
		return {x - reach, _plot.top, x + reach + 1, _track.bottom};
	}

	// The handle dragged to x, in the element's coordinates: its slider's value to the hundredth, as the slider sets it.
	void drag(const int handle, const int x)
	{
		const auto position = (x - _plot.left) / static_cast<double>(std::max(1, _plot.width()));
		set(handle, edit_tone_handle_value(handle, position));
	}

	void set(const int handle, const double value)
	{
		const auto rounded = core::to_double(format_edit_adjustment(value));
		if (core::equiv(rounded, _handles[handle].value)) return;
		_handles[handle].value = rounded;
		if (_changed) _changed();
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto alpha = dc.colors.alpha;
		const auto plot = _plot.offset(element_offset);
		const auto track = _track.offset(element_offset);
		dc.draw_rect(plot, core::color(0, alpha * 0.25f));

		static const edit_histogram none;
		const auto& histogram = _histogram ? _histogram() : none;

		if (histogram.samples > 0)
		{
			const auto peak = static_cast<double>(std::max(1u, histogram.peak()));
			const auto width = static_cast<double>(plot.width());
			const auto height = static_cast<double>(plot.height());
			const auto bottom = static_cast<double>(plot.bottom);
			constexpr auto levels = edit_histogram::levels;
			const auto x_at = [&](const int level)
			{
				return plot.left + width * (level + 0.5) / levels;
			};
			const auto channel_at = [&](const int c, const int level)
			{
				return height * std::min(1.0, histogram.counts[c][level] / peak);
			};
			const auto common_at = [&](const int level)
			{
				return std::min({channel_at(0, level), channel_at(1, level), channel_at(2, level)});
			};

			// Each curve joined through the middles of its levels, out to the ends at the end levels' heights.
			const auto add_curve = [&](std::vector<pointd>& outline, const auto& height_at, const bool backwards)
			{
				const auto add = [&](const double x, const int level)
				{
					outline.emplace_back(x, bottom - height_at(level));
				};
				if (!backwards)
				{
					add(plot.left, 0);
					for (auto level = 0; level < levels; ++level) add(x_at(level), level);
					add(plot.right, levels - 1);
				}
				else
				{
					add(plot.right, levels - 1);
					for (auto level = levels - 1; level >= 0; --level) add(x_at(level), level);
					add(plot.left, 0);
				}
			};

			std::vector<pointd> outline;
			outline.reserve(levels * 2 + 8);
			render::polygon_style area_style;

			// Grey where all three channels reach, as light mixed of all three is.
			outline.emplace_back(plot.left, bottom);
			add_curve(outline, common_at, false);
			outline.emplace_back(plot.right, bottom);
			area_style.fill = render::paint(core::color(0.8f, 0.8f, 0.8f, alpha * 0.55f));
			dc.draw_polygon(outline, area_style);

			// Above that each channel in its own colour, two mixed where they both reach.
			const std::array fills{core::color(0.95f, 0.25f, 0.25f, alpha * 0.6f),
			                       core::color(0.3f, 0.85f, 0.3f, alpha * 0.6f),
			                       core::color(0.3f, 0.5f, 1.0f, alpha * 0.6f)};

			for (auto c = 0; c < 3; ++c)
			{
				outline.clear();
				add_curve(outline, [&](const int level) { return channel_at(c, level); }, false);
				add_curve(outline, common_at, true);
				area_style.fill = render::paint(fills[c]);
				dc.draw_polygon(outline, area_style);
			}
		}

		const auto half = std::max(2, dc.handle_cxy / 2);
		const auto scale1 = std::max(1, core::round(dc.scale_factor));

		for (auto i = 0; i < static_cast<int>(_handles.size()); ++i)
		{
			const auto x = handle_x(i) + element_offset.x;
			const auto lit = i == _hover || i == _held;

			if (lit)
			{
				dc.draw_rect(recti(x, plot.top, x + scale1, plot.bottom),
				             core::color(dc.colors.foreground, alpha * 0.6f));
			}

			// Darker for the darks, lighter for the lights, as a levels control's are.
			const auto grey = 0.15f + 0.35f * static_cast<float>(i);
			const std::array<pointd, 3> triangle{pointd(x, track.top), pointd(x + half, track.bottom),
			                                     pointd(x - half, track.bottom)};
			render::polygon_style handle_style;
			handle_style.fill = render::paint(core::color(grey, grey, grey, alpha));
			handle_style.border = lit ? core::color(dc.colors.selected, alpha)
			                          : core::color(dc.colors.foreground, alpha * 0.7f);
			handle_style.border_width = static_cast<double>(scale1);
			dc.draw_polygon(triangle, handle_style);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
};

class edit_tone_handle_controller final : public view_controller
{
	std::shared_ptr<edit_histogram_control> _parent;
	int _handle;
	pointi _origin;
	// How far right of the handle it was grabbed, so a press alone moves nothing.
	int _grab = 0;
	double _start = 0.0;
	bool _tracking = false;

	void drag_to(const pointi loc) const { _parent->drag(_handle, loc.x - _origin.x - _grab); }

public:
	edit_tone_handle_controller(const view_host_ptr& host, std::shared_ptr<edit_histogram_control> parent,
	                            const int handle, const pointi origin, const int reach) :
		view_controller(host, parent->handle_bounds(handle, reach).offset(origin)), _parent(std::move(parent)),
		_handle(handle), _origin(origin)
	{
		_parent->_hover = handle;
		_host->invalidate_element(_parent);
	}

	~edit_tone_handle_controller() override
	{
		_parent->_hover = -1;
		_parent->_held = -1;
		_host->invalidate_element(_parent);
	}

	platform::cursor cursor() const override { return platform::cursor::left_right; }

	void on_mouse_left_button_down(const pointi loc, const platform::key_state /*keys*/) override
	{
		_tracking = true;
		_start = _parent->_handles[_handle].value;
		_grab = loc.x - _origin.x - _parent->handle_x(_handle);
		_parent->_held = _handle;
		_host->invalidate_element(_parent);
		_host->invalidate(ui::host_invalid::tooltip);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_tracking) drag_to(loc);
	}

	void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
	{
		if (!std::exchange(_tracking, false)) return;
		drag_to(loc);
		_parent->_held = -1;
		_host->invalidate_element(_parent);
	}

	// A double click puts the handle back where its slider is at 0.
	void on_mouse_left_button_double_click(const pointi /*loc*/, const platform::key_state /*keys*/) override
	{
		_tracking = false;
		_parent->_held = -1;
		_parent->set(_handle, 0.0);
	}

	bool escape() override
	{
		if (!std::exchange(_tracking, false)) return false;
		_parent->_held = -1;
		_parent->set(_handle, _start);
		return true;
	}

	void popup_from_location(view_hover_element& hover) override
	{
		if (_tracking) return;
		const auto& handle = _parent->_handles[_handle];
		hover.elements->add(std::make_shared<text_element>(
			std::format("{}: {}", handle.label.sv(), format_edit_adjustment(handle.value))));
		hover.active_bounds = hover.window_bounds = _bounds;
	}
};

view_controller_ptr edit_histogram_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                     const pointi element_offset,
                                                                     hit_test_context& /*ctx*/)
{
	const auto reach = std::max(4, _track.height() * 2 / 3);
	const auto handle = handle_at(loc - element_offset, reach);
	if (handle < 0) return nullptr;
	return std::make_shared<edit_tone_handle_controller>(host, shared_from_this(), handle, element_offset, reach);
}

// The before-and-after handle beside the photo's left edge. Held, its line follows the pointer across the view: the
// original as the file shows it left of it, the photo as the edit will save it right. Let go - or Escape - and it
// slides back to where it rests.
class edit_compare_controller final : public view_controller
{
	edit_view& _view;
	// How far right of the line the knob was grabbed, so a press alone moves nothing.
	int _grab = 0;
	bool _tracking = false;

public:
	edit_compare_controller(const view_host_ptr& host, edit_view& view, const recti bounds) :
		view_controller(host, bounds), _view(view)
	{
	}

	~edit_compare_controller() override
	{
		if (_tracking) _view.compare_release();
	}

	platform::cursor cursor() const override { return platform::cursor::left_right; }

	void draw(render::draw_context& dc) override
	{
		if (!_tracking)
		{
			const auto ring = std::max(1, core::round(2 * dc.scale_factor));
			dc.draw_ellipse(_view._compare_handle.inflate(ring), core::color(ui::style::colors.view_text, _alpha),
			                static_cast<double>(ring));
		}
	}

	void on_mouse_left_button_down(const pointi loc, const platform::key_state /*keys*/) override
	{
		_tracking = true;
		_grab = loc.x - _view.compare_line_x();
		_view.compare_at(loc.x - _grab);
		_host->invalidate(ui::host_invalid::tooltip);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_tracking) _view.compare_at(loc.x - _grab);
	}

	void on_mouse_left_button_up(const pointi /*loc*/, const platform::key_state /*keys*/) override
	{
		if (std::exchange(_tracking, false)) _view.compare_release();
	}

	bool escape() override
	{
		if (!std::exchange(_tracking, false)) return false;
		_view.compare_release();
		return true;
	}

	void popup_from_location(view_hover_element& hover) override
	{
		if (_tracking) return;
		hover.elements->add(make_icon_element(icon_index::compare, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(tt.compare));
		hover.elements->add(std::make_shared<text_element>(tt.edit_compare_tooltip));
		hover.preferred_size = core::mul_div(hover.preferred_size, 5, 8);
		hover.active_bounds = hover.window_bounds = _bounds;
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

edit_view::edit_view(view_state& s, view_host_ptr host, edit_view_state& evs) :
	_state(s), _host(std::move(host)), _edit_state(evs)
{
}

view_controls_host_ptr edit_view::controls(const ui::frame_ptr& owner)
{
	if (!_edit_controls)
	{
		_edit_controls = std::make_shared<edit_view_controls>(_state, _edit_state);
		_edit_controls->_view = shared_from_this();
		_edit_controls->_frame = owner;
	}

	return _edit_controls;
}

core::const_surface_ptr edit_view::build_preview_surface(const core::const_surface_ptr& source,
                                                         const sizei loaded_dimensions, const image_edits& edits)
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

core::const_surface_ptr edit_view::preview_surface() const
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
	clear_av_preview();
	_av_plan = {};
	_av_mp4_plan = {};
	_is_av = false;
	_av_plan_pending = false;
	_status.clear();
	_media_display.reset();
	_media_element.reset();
	_play_element.reset();
	_scrubber_element.reset();
	_preview_source.reset();
	_dialog_preview_source.reset();
	_histogram_samples.clear();
	_histogram_counted_at.reset();
	_compare.reset(0.0f);
	_comparing = false;
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

void edit_view::layout(render::measure_context& mc, const sizei extent)
{
	// No reset on a new extent: queue_preview_decode asks again only if the wanted size changed, and
	// the picture on screen stays until its replacement arrives.
	_extent = extent;
	queue_preview_decode();

	if (_media_element && _play_element && _scrubber_element)
	{
		const auto outer_bounds = recti(extent).inflate(-mc.padding2);
		const auto play_extent = _play_element->measure(mc, outer_bounds.width());
		const auto scrubber_extent = _scrubber_element->measure(mc, outer_bounds.width());
		const auto controls_height = std::max(play_extent.cy, scrubber_extent.cy);
		const auto controls_top = std::max(outer_bounds.top, outer_bounds.bottom - controls_height);
		const auto controls_bounds = recti(outer_bounds.left, controls_top, outer_bounds.right, outer_bounds.bottom);
		const auto play_bounds = recti(controls_bounds.left, controls_bounds.top, controls_bounds.left + play_extent.cx,
		                               controls_bounds.bottom);
		const auto scrubber_bounds = recti(play_bounds.right + mc.padding1, controls_bounds.top, controls_bounds.right,
		                                   controls_bounds.bottom);
		const auto media_bounds = recti(outer_bounds.left, outer_bounds.top, outer_bounds.right,
		                                std::max(outer_bounds.top, controls_bounds.top - mc.padding2));

		_play_element->layout(mc, play_bounds);
		_scrubber_element->layout(mc, scrubber_bounds);
		_media_element->layout(mc, media_bounds);
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

	// The scrubber's handles set the trim, while the display says a video's or a sound file's copy can be made.
	auto scrubber = std::make_shared<scrubber_element>(_state._player, _media_display);
	scrubber->trim_changed([weak = weak_from_this()](const double start, const double end)
	{
		if (const auto self = weak.lock()) self->trim(start, end);
	});
	_scrubber_element = std::move(scrubber);
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

av_edits edit_view::av_draft() const
{
	av_edits result;
	result.start = _edit_state._trim_start;
	// The frame shown where the trim ends is kept: the copy stops at the first one after it.
	result.end = _edit_state._trim_end > 0.0 ? _edit_state._trim_end + 0.001 : 0.0;
	result.remove_audio = _edit_state._remove_sound && _av_plan.has_video && _av_plan.has_audio;
	result.quarter_turns = _av_plan.can_rotate ? (_edit_state._quarter_turns % 4 + 4) % 4 : 0;
	return result;
}

void edit_view::queue_av_plan()
{
	const auto path = _path;
	const auto display_generation = _display_generation;
	_av_plan_pending = true;

	_state.queue_async(async_queue::load, [weak = weak_from_this(), path, display_generation, &s = _state,
	                                       loading = df::hold_loading_media()]
	{
		auto plan = av_plan_edits(path);
		// Save As offers MP4 beside a file's own format, which may keep less of it.
		const auto own = av_edit_muxer(path.extension());
		auto mp4_plan = own.empty() || own == av_edit_muxer(av_mp4_extension) ? plan
		                                                                      : av_plan_edits(path, av_mp4_extension);

		s.queue_ui(
			[weak, path, display_generation, plan = std::move(plan), mp4_plan = std::move(mp4_plan), loading]() mutable
		{
			const auto self = weak.lock();
			if (!self || !should_accept_edit_async_result(self->_display_generation, display_generation) ||
			    self->_path != path)
			{
				return;
			}

			self->_av_plan = std::move(plan);
			self->_av_mp4_plan = std::move(mp4_plan);
			self->_av_plan_pending = false;
			self->av_changed();
		});
	});
}

const av_edit_plan& edit_view::av_plan_for(const core::file_path path) const
{
	const auto own = av_edit_muxer(_path.extension());
	return !own.empty() && av_edit_muxer(path.extension()) == own ? _av_plan : _av_mp4_plan;
}

bool edit_view::saves_av_as(const core::file_path path) const
{
	const auto muxer = av_edit_muxer(path.extension());
	if (muxer.empty()) return false;
	if (muxer == av_edit_muxer(_path.extension())) return _av_plan.can_edit;
	return muxer == av_edit_muxer(av_mp4_extension) && _av_mp4_plan.can_edit;
}

void edit_view::trim(const double start, const double end)
{
	if (!edits_av()) return;

	_edit_state._trim_start = std::max(0.0, start);
	_edit_state._trim_end = std::max(0.0, end);
	av_changed(av_change::dragged_trim);
}

void edit_view::trim_start_here()
{
	const auto d = _state.display_state();
	if (!edits_av() || !d) return;

	const auto pos = std::max(0.0, d->media_pos());
	// The latest choice wins: a start at or after the end keeps the rest instead.
	if (_edit_state._trim_end > 0.0 && pos >= _edit_state._trim_end) _edit_state._trim_end = 0.0;
	_edit_state._trim_start = pos;
	av_changed(av_change::trim);
}

void edit_view::trim_end_here()
{
	const auto d = _state.display_state();
	if (!edits_av() || !d) return;

	const auto pos = d->media_pos();
	if (pos <= 0.0) return;

	// The latest choice wins: an end at or before the start keeps from the beginning instead.
	if (pos <= _edit_state._trim_start) _edit_state._trim_start = 0.0;

	// An end on the last frame keeps the rest, which is no trim at all.
	const auto media_end = d->media_end() > 0.0 ? d->media_end() : _av_plan.duration;
	_edit_state._trim_end = media_end > 0.0 && pos >= media_end - 0.05 ? 0.0 : pos;
	av_changed(av_change::trim);
}

void edit_view::trim_clear()
{
	_edit_state._trim_start = 0.0;
	_edit_state._trim_end = 0.0;
	av_changed(av_change::trim);
}

void edit_view::av_changed(const av_change change)
{
	++_edit_generation;

	// The player shows the draft as it will be saved: the trim on the scrubber, playback pausing at its end,
	// silence for a video saved without its sound, and the picture turned.
	const auto shows = edits_av();
	const auto draft = av_draft();

	if (const auto d = _state.display_state())
	{
		d->_trim_start = shows ? _edit_state._trim_start : 0.0;
		d->_trim_end = shows ? _edit_state._trim_end : 0.0;
		d->_trim_handles = shows && _av_plan.duration > 0.0;
		d->_mute_preview = shows && draft.remove_audio;
		if (d->_selected_texture1) d->_selected_texture1->_preview_quarter_turns = shows ? draft.quarter_turns : 0;
	}

	_state._playback_end = shows ? _edit_state._trim_end : 0.0;
	update_status();

	const auto relayout = change == av_change::layout;

	if (_edit_controls && _edit_controls->_remove_sound_check)
	{
		_edit_controls->_remove_sound_check->show_bound_value();
		const auto panel_changed = _edit_controls->update_notes();
		if ((relayout || panel_changed) && _edit_controls->_frame) _edit_controls->_frame->layout();
	}

	// The picture's turn changes its shape; a trim only what is drawn on the scrubber. A dragged handle moves with the
	// pointer, which keeps its controller.
	_state.invalidate_view(view_invalid::command_state | view_invalid::view_redraw |
	                       (relayout ? view_invalid::view_layout : view_invalid::none) |
	                       (change != av_change::dragged_trim ? view_invalid::controller : view_invalid::none));
}

// The changes a save would make, for the status bar: the range a trim keeps, the sound taken out, the turn. With
// none, Save As still writes a re-muxed copy - the way to repair a file other players refuse.
void edit_view::update_status()
{
	std::string status;

	if (edits_av())
	{
		std::vector<std::string> changes;

		if (_edit_state._trim_start > 0.0 || _edit_state._trim_end > 0.0)
		{
			const auto d = _state.display_state();
			const auto media_end = d && d->media_end() > 0.0 ? d->media_end() : _av_plan.duration;
			const auto start = _edit_state._trim_start;
			const auto end = _edit_state._trim_end > 0.0 ? _edit_state._trim_end : media_end;
			changes.emplace_back(str_format(tt.edit_status_trim_fmt.sv(), format_trim_time(start),
			                                format_trim_time(end), format_trim_time(end - start)));
		}

		const auto draft = av_draft();
		if (draft.remove_audio) changes.emplace_back(tt.edit_status_no_sound.sv());
		if (draft.quarter_turns == 1) changes.emplace_back(tt.edit_status_rotated_right.sv());
		if (draft.quarter_turns == 2) changes.emplace_back(tt.edit_status_upside_down.sv());
		if (draft.quarter_turns == 3) changes.emplace_back(tt.edit_status_rotated_left.sv());

		status = changes.empty() ? std::string(tt.edit_status_unchanged.sv())
		                         : core::combine(changes, " \u00B7 ", false);
	}

	if (status == _status) return;
	_status = std::move(status);
	_state.invalidate_view(view_invalid::status);
}

bool edit_view::key_down(const char32_t key, const platform::key_state keys)
{
	if (!edits_av() || keys.control || keys.alt || keys.shift) return false;

	// A trim set at the frame on screen, as precisely as the picture can be stopped on it.
	if (key == 'I')
	{
		trim_start_here();
		return true;
	}

	if (key == 'O')
	{
		trim_end_here();
		return true;
	}

	return false;
}

void edit_view::clear_av_preview() const
{
	const auto clear = [](const display_state_ptr& d)
	{
		if (!d) return;
		d->_trim_start = 0.0;
		d->_trim_end = 0.0;
		d->_trim_handles = false;
		d->_mute_preview = false;
		if (d->_selected_texture1) d->_selected_texture1->_preview_quarter_turns = 0;
	};

	clear(_media_display);
	clear(_state.display_state());
	_state._playback_end = 0.0;
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
	       current_preview_generation == result_preview_generation && current_request_dimensions == result_dimensions;
}

bool should_accept_edit_analysis(const size_t current_display_generation, const size_t result_display_generation,
                                 const size_t current_edit_generation, const size_t result_edit_generation)
{
	return should_accept_edit_async_result(current_display_generation, result_display_generation) &&
	       current_edit_generation == result_edit_generation;
}

bool edit_preview_is_current(const core::const_surface_ptr& preview, const sizei answered_request, const sizei wanted)
{
	return is_valid(preview) && answered_request == wanted;
}

file_load_result::failure edit_preview_failure(const core::const_surface_ptr& preview, const file_load_result& loaded,
                                               const sizei request)
{
	if (is_valid(preview)) return file_load_result::failure::none;
	return files::exceeds_decode_budget(loaded.i, request) ? file_load_result::failure::too_large
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
	return request_in_flight ? edit_request_coalescing_decision{false, true}
	                         : edit_request_coalescing_decision{true, false};
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
		_pending_source_load_request = request;
		return;
	}

	start_source_load(request);
}

void edit_view::start_source_load(source_load_request request)
{
	_source_load_in_flight = true;
	const auto weak = weak_from_this();
	_state.queue_async(async_queue::load, [weak, request, &s = _state, loading = df::hold_loading_media()]
	{
		files loader;
		// At the precision the source holds, as the save will load it, so the preview shows what is saved.
		auto loaded = loader.load(request.path, false, decode_intent::edit);
		s.queue_ui([weak, request, loaded = std::move(loaded), loading]() mutable
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
		_histogram_samples.clear();
		_histogram_counted_at.reset();
		_compare.reset(0.0f);
		_comparing = false;
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
			const auto preview_dimensions = core::scale_dimensions(loaded_copy.dimensions(), 192);
			const auto weak_dialog = weak_from_this();
			const auto dialog_generation = _display_generation;
			_state.queue_async(async_queue::render,
			                   [weak_dialog, loaded_copy, preview_dimensions, dialog_generation, &s = _state]
			{
				// A dialog's thumbnail, for which a byte a channel is plenty.
				const auto source = core::to_8_bit(loaded_copy.to_surface(preview_dimensions));
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

		if (_edit_controls->_frame)
		{
			_edit_controls->populate();
			_edit_controls->_frame->layout();
		}
		_state.invalidate_view(view_invalid::command_state | view_invalid::view_redraw | view_invalid::controller);
		_host->frame()->invalidate();
	}

	if (should_start_edit_follow_up_request(_pending_source_load_request.has_value()))
	{
		auto next = *_pending_source_load_request;
		_pending_source_load_request.reset();
		start_source_load(next);
	}
}

void edit_view::queue_preview_decode()
{
	if (!can_edit_pixels() || _extent.is_empty()) return;

	const auto preview_dimensions = core::scale_dimensions(_loaded.dimensions(), _extent);
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

	start_preview_decode(request);
}

void edit_view::start_preview_decode(const preview_decode_request& request)
{
	_preview_decode_in_flight = true;
	_preview_request_dimensions = request.dimensions;
	const auto weak = weak_from_this();
	_state.queue_async(async_queue::render, [weak, request, &s = _state]
	{
		auto source = request.loaded.to_surface(request.dimensions, false, {}, decode_intent::edit);
		// An HDR picture's signal is shown as its SDR rendition, which its colour is edited on; colours a profile
		// describes, as their sRGB rendition.
		if (const auto& hdr = request.loaded.signal.hdr; hdr && is_valid(source))
			source = sdr_rendition(source, *hdr, true);
		else if (!request.loaded.signal.profile.empty())
			source = profile_rendition(source, request.loaded.signal.profile, true);
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

void edit_view::complete_preview_decode(const preview_decode_request& request, const core::const_surface_ptr& source,
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
			_histogram_samples = edit_histogram_samples(source);
			_histogram_counted_at.reset();
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

			if (_edit_controls->_frame)
			{
				_edit_controls->populate();
				_edit_controls->_frame->layout();
			}
			_state.invalidate_view(view_invalid::command_state | view_invalid::view_redraw | view_invalid::controller);
			_host->frame()->invalidate();
		}
	}

	if (should_start_edit_follow_up_request(_pending_preview_decode_request.has_value()))
	{
		auto next = std::move(*_pending_preview_decode_request);
		_pending_preview_decode_request.reset();
		start_preview_decode(next);
	}
}

void edit_view::clear_crop_interaction_bounds()
{
	_crop_bounds = {};
	_crop_handle_tl = {};
	_crop_handle_tr = {};
	_crop_handle_bl = {};
	_crop_handle_br = {};
	_compare_handle = {};
}

void edit_view::draw_loading_status(render::draw_context& dc, const std::string_view status) const
{
	if (status.empty()) return;

	const recti status_bounds(0, 0, _extent.cx, _extent.cy);
	dc.draw_text(status, status_bounds, render::font_face::title, render::text_style::single_line_center,
	             core::color(dc.colors.foreground, dc.colors.alpha), {});
}

df::item_element_ptr edit_view::next_editable_item(const bool forward) const
{
	// The selector strip only offers what Edit opens, so save-and-next walks the same set rather than
	// stranding the user on a folder or unsupported file with no editing controls.
	const auto next = _state.next_item_where(forward, true, can_edit_in_edit_view);
	return next == _state.focus_item() ? nullptr : next;
}

view_controller_ptr edit_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                        hit_test_context& ctx)
{
	if (is_photo() && !_edit_state._preview_mode)
	{
		if (ctx.occluded(_compare_handle))
		{
			return std::make_shared<edit_compare_controller>(host, *this, _compare_handle);
		}

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

core::file_path edit_save_suggestion(const core::file_path source, const std::function<bool(core::file_path)>& exists,
                                     const bool as_avif)
{
	// A video or a sound file in a format Edit does not write is copied as an MP4, and a photo in one Diffractor does
	// not as a JPEG - or as an AVIF, which keeps what a JPEG would lose.
	const auto is_av = files::file_type_from_name(source)->has_trait(file_traits::av);
	const auto keeps_format = is_av ? !av_edit_muxer(source.extension()).empty() : files::can_save(source);
	const auto extension = keeps_format ? std::string(source.extension())
	                       : is_av      ? std::format(".{}", av_mp4_extension)
	                       : as_avif    ? ".avif"s
	                                    : ".jpg"s;
	const auto name = std::string(source.file_name_without_extension()) + "-edit"s;
	auto path = core::file_path(source.folder(), name, extension);

	// Number repeats rather than stacking suffixes, so a third edit is "photo-edit 3", not "photo-edit-edit-edit".
	for (auto n = 2; exists(path); ++n)
	{
		path = core::file_path(source.folder(), name + " "s + std::to_string(n), extension);
	}

	return path;
}

bool edit_offers_avif(const core::file_path source, const file_load_result& loaded)
{
	if (files::can_save(source) || libfiles::file_type_from_extension(source.extension()) != libfiles::file_type::heif)
		return false;

	const auto& signal = loaded.signal;
	return signal.hdr || signal.gain_map || (is_valid(loaded.s) && core::is_wide(loaded.s->format()));
}

std::string edit_save_loss(const picture_signal& signal, const core::file_path destination, const bool repacks)
{
	// A file of a format Diffractor does not encode is only ever turned in place or copied whole, which keeps all it
	// holds.
	if (!files::can_save(destination)) return {};

	const auto extension = destination.extension();
	const auto is = [extension](const std::string_view e)
	{
		return core::icmp(extension, e) == 0;
	};
	const auto jpeg = files::is_jpeg(destination);

	// A JPEG holds an HDR picture as its SDR rendition with a gain map made for it.
	if (signal.hdr && !(jpeg || is(".avif") || is(".jxl") || is(".png")))
		return str_format(tt.edit_hdr_lost_fmt.sv(), extension);

	if (signal.gain_map && !signal.hdr && !(jpeg || is(".avif") || (repacks && is(".jxl"))))
		return str_format(tt.edit_gain_map_lost_fmt.sv(), extension);

	return {};
}

std::string_view edit_photo_note(const picture_signal& signal)
{
	if (signal.tone_mapped) return tt.edit_hdr_too_large.sv();
	if (signal.hdr) return tt.edit_hdr.sv();
	if (signal.narrowed) return tt.edit_too_large_for_depth.sv();
	return {};
}

uint32_t edit_histogram::peak() const
{
	uint32_t result = 0;

	for (const auto& channel : counts)
	{
		for (auto level = 1; level < levels - 1; ++level) result = std::max(result, channel[level]);
	}

	return result;
}

std::vector<std::array<float, 3>> edit_histogram_samples(const core::const_surface_ptr& surface, const int most)
{
	std::vector<std::array<float, 3>> samples;
	if (!is_valid(surface) || most <= 0) return samples;

	const auto format = surface->format();
	const auto wide = core::is_wide(format);
	if (!wide && !core::is_packed(format)) return samples;

	const auto has_alpha = format == core::texture_format::ARGB || format == core::texture_format::RGBA16;
	const auto dims = surface->dimensions();
	// Every step-th pixel each way, from the middle of the first step, so the sample spreads over the whole picture.
	const auto step = std::max(1, static_cast<int>(std::ceil(
									  std::sqrt(static_cast<double>(dims.cx) * dims.cy / static_cast<double>(most)))));
	samples.reserve(static_cast<size_t>((dims.cx + step - 1) / step) *
	                static_cast<size_t>((dims.cy + step - 1) / step));

	for (auto y = step / 2; y < dims.cy; y += step)
	{
		const auto* const row = surface->pixels() + static_cast<size_t>(y) * surface->stride();

		for (auto x = step / 2; x < dims.cx; x += step)
		{
			if (wide)
			{
				// Red first, sixteen bits a channel.
				const auto* const p = std::bit_cast<const uint16_t*>(row) + static_cast<size_t>(x) * 4;
				if (has_alpha && p[3] == 0) continue;
				samples.push_back({p[0] / 65535.0f, p[1] / 65535.0f, p[2] / 65535.0f});
			}
			else
			{
				// Blue first, a byte a channel.
				const auto* const p = row + static_cast<size_t>(x) * 4;
				if (has_alpha && p[3] == 0) continue;
				samples.push_back({p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f});
			}
		}
	}

	return samples;
}

edit_histogram count_edit_histogram(const std::span<const std::array<float, 3>> samples,
                                    const render::texture_transform* const grade)
{
	edit_histogram result;
	const auto level_of = [](const float v)
	{
		return std::clamp(static_cast<int>(v * edit_histogram::levels), 0, edit_histogram::levels - 1);
	};
	const auto grades = grade && grade->has_color_changes;

	for (auto [r, g, b] : samples)
	{
		if (grades)
		{
			render::grade_color(r, g, b, grade->curve.data(), render::texture_transform::curve_len, grade->saturation,
			                    grade->vibrance, grade->red_gain, grade->green_gain, grade->blue_gain);
		}

		++result.counts[0][level_of(r)];
		++result.counts[1][level_of(g)];
		++result.counts[2][level_of(b)];
	}

	result.samples = static_cast<uint32_t>(samples.size());
	return result;
}

namespace
{
	// Where the darks, midtones and lights move the tone curve, and how far at most: render::color_adjust's knots.
	constexpr std::array tone_handle_points{0.2, 0.5, 0.8};
	constexpr double tone_handle_reach = 0.1;
}

double edit_tone_handle_position(const int handle, const double value)
{
	return tone_handle_points[std::clamp(handle, 0, 2)] + tone_handle_reach * std::clamp(value, -1.0, 1.0);
}

double edit_tone_handle_value(const int handle, const double position)
{
	return std::clamp((position - tone_handle_points[std::clamp(handle, 0, 2)]) / tone_handle_reach, -1.0, 1.0);
}

std::string format_trim_time(const double seconds)
{
	const auto tenths = std::llround(std::max(0.0, seconds) * 10.0);
	const auto hours = tenths / 36000;
	const auto minutes = tenths / 600 % 60;
	const auto rest = tenths % 600;

	return hours > 0 ? std::format("{}:{:02}:{:02}.{}", hours, minutes, rest / 10, rest % 10)
	                 : std::format("{}:{:02}.{}", minutes, rest / 10, rest % 10);
}

std::string edit_repair_report(const int skipped, const double ended_at)
{
	std::vector<std::string> lines{std::string(tt.edit_repaired.sv())};
	if (skipped > 0) lines.emplace_back(str_format(tt.edit_repair_skipped_fmt.sv(), skipped));
	if (ended_at >= 0.0) lines.emplace_back(str_format(tt.edit_repair_ended_fmt.sv(), format_trim_time(ended_at)));
	return core::combine(lines, "\n\n", false);
}

std::vector<platform::file_dialog_filter> edit_save_filters(const core::file_path source, const bool offers_mp4)
{
	std::vector<platform::file_dialog_filter> result;

	const auto offer = [&result](const av_edit_format& format)
	{
		std::string patterns;

		for (const auto extension : format.extensions)
		{
			if (!patterns.empty()) patterns += ';';
			patterns += "*.";
			patterns += extension;
		}

		result.push_back({std::string(format.name), std::move(patterns)});
	};

	if (const auto own = av_edit_format_of(source.extension()); !own.extensions.empty()) offer(own);

	if (offers_mp4 && av_edit_muxer(source.extension()) != av_edit_muxer(av_mp4_extension))
	{
		offer(av_edit_format_of(av_mp4_extension));
	}

	return result;
}

bool edit_view::select_path(core::file_path& path) const
{
	if (edits_av())
	{
		const auto filters = edit_save_filters(_path, _av_mp4_plan.can_edit);
		return !filters.empty() && platform::prompt_for_save_path(path, filters);
	}

	// A format Diffractor cannot write is saved as JPEG - or AVIF, where a JPEG would lose what the edit holds - under a
	// name of its own: the same name with .jpg is, beside a RAW, the camera's own JPEG.
	if (!files::can_save(path))
	{
		path = edit_save_suggestion(path, [](const core::file_path p) { return platform::exists(p); },
		                            edit_offers_avif(_path, _loaded));
	}

	return platform::prompt_for_save_path(path);
}

void edit_view::open_saved_copy(const core::file_path path)
{
	_state.view_mode(view_type::items);
	_state.open(_host, path);
	_state.item_index.queue_scan_modified_items(_state.selected_items());
}

bool edit_view::check_path(core::file_path& path, const ui::frame_ptr& owner) const
{
	if (!path.is_save_valid())
	{
		const auto dlg = make_dlg(owner);
		const auto message = format_invalid_name_message(path.name());
		dlg->show_message(icon_index::error, core::app_name(), message);
		return false;
	}

	if (edits_av())
	{
		// Saved over itself, a video or a sound file in a format Edit does not write is copied as an MP4 beside it,
		// under a name of its own, chosen as Save As chooses one.
		if (av_edit_muxer(_path.extension()).empty() && path == _path)
		{
			path = edit_save_suggestion(_path, [](const core::file_path p) { return platform::exists(p); });
			if (!select_path(path)) return false;

			if (!path.is_save_valid())
			{
				make_dlg(owner)->show_message(icon_index::error, core::app_name(),
				                              format_invalid_name_message(path.name()));
				return false;
			}
		}

		// A copy is written by its own format's muxer or as an MP4, so a name in another format would hold it under
		// the wrong extension.
		if (!saves_av_as(path))
		{
			make_dlg(owner)->show_message(icon_index::error, core::app_name(), tt.edit_av_same_format);
			return false;
		}
	}

	if (is_photo())
	{
		const auto dlg = make_dlg(owner);
		const auto over_itself = path == _path;
		// Over itself, a file Diffractor cannot write still takes a turn its own structure keeps.
		const auto writes = over_itself ? files::saves_in_place(path, _edit_state._edits, _loaded.dimensions())
		                                : files::can_save(path);
		// An HDR photo too large to edit in HDR is edited as its SDR rendition, which never replaces it: a turn its own
		// structure keeps is saved over it, and anything else as a copy beside it.
		const auto keeps_original = over_itself && _loaded.signal.tone_mapped && _edit_state.has_pixel_changes() &&
		                            !files::turns_in_place(path, _edit_state._edits, _loaded.dimensions());

		if (_edit_state.has_pixel_changes() && (!writes || keeps_original))
		{
			const auto as_avif = edit_offers_avif(_path, _loaded);
			const auto message = keeps_original ? std::string(tt.edit_hdr_copy.sv())
			                     : as_avif      ? str_format(tt.save_as_avif_fmt.sv(), path.extension())
			                                    : str_format(tt.save_as_jpeg_fmt.sv(), path.extension());
			const std::string_view button = keeps_original ? tt.button_save_copy.sv()
			                                : as_avif      ? tt.button_save_as_avif.sv()
			                                               : tt.button_save_as_jpeg.sv();
			const std::vector<core::const_surface_ptr> previews{preview_surface()};
			const auto title = std::make_shared<ui::title_control2>(dlg->_frame, icon_index::save, tt.save_changes,
			                                                        message, previews);
			const std::vector<view_element_ptr> controls{set_margin(title), std::make_shared<divider_element>(),
			                                             std::make_shared<ui::ok_cancel_control>(dlg->_frame, button)};

			const auto result = dlg->show_modal(controls);

			if (result != ui::close_result::ok)
			{
				return false;
			}

			// Changing the extension writes a different file, so the destination is chosen and its
			// overwrite confirmed rather than silently landing on a sibling that already exists.
			if (keeps_original)
				path = edit_save_suggestion(_path, [](const core::file_path p) { return platform::exists(p); },
				                            as_avif);
			if (!select_path(path)) return false;

			if (!path.is_save_valid())
			{
				make_dlg(owner)->show_message(icon_index::error, core::app_name(),
				                              format_invalid_name_message(path.name()));
				return false;
			}
		}

		// A copy in a format that cannot hold the photo's HDR or its gain map says so before it is written. A JPEG
		// repacked into a JPEG XL whole keeps its gain map.
		const auto repacks = core::is_jpeg(_loaded.i) && setting.jxl_lossless &&
		                     _edit_state._edits.is_no_loss(_loaded.dimensions());

		if (const auto loss = edit_save_loss(_loaded.signal, path, repacks); !loss.empty())
		{
			const auto confirm = make_dlg(owner);
			const std::vector<core::const_surface_ptr> previews{preview_surface()};
			const auto title = std::make_shared<ui::title_control2>(confirm->_frame, icon_index::save, tt.save_changes,
			                                                        loss, previews);
			const auto buttons = std::make_shared<ui::ok_cancel_control>(confirm->_frame, tt.button_save);
			const std::vector<view_element_ptr> controls{set_margin(title), std::make_shared<divider_element>(),
			                                             buttons};

			if (confirm->show_modal(controls) != ui::close_result::ok) return false;
		}
	}

	return true;
}

view_bars edit_view::bars() const
{
	return task_view_bars({commands::edit_item_preview, commands::edit_item_save_and_prev,
	                       commands::edit_item_save_and_next, commands::edit_item_save_as, commands::edit_item_save});
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
		                                                    dialog_previews())),
			std::make_shared<divider_element>(),
			ui::save_discard_cancel(save_fn, cancel_fn, [dlg] { dlg->close(true); }, true),
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
	                                                    dialog_previews())),
		std::make_shared<divider_element>(),
		ui::save_discard_cancel(save_fn, discard_fn, [dlg] { dlg->close(true); }, true),
	};

	dlg->show_modal(controls);
}

void edit_view::save_current()
{
	if (!can_edit_pixels() && !edits_av()) return;

	auto path = _path;
	const auto xmp_name = _xmp_name;
	const auto owner = _host->owner();

	if (check_path(path, owner))
	{
		// A video or a sound file whose format Edit does not write was saved as a new file, shown as Save As shows one.
		const auto saved_as_copy = edits_av() && path != _path;
		save(_path, path, xmp_name, owner, [weak = weak_from_this(), path, saved_as_copy](const bool success)
		{
			const auto self = weak.lock();
			if (!success || !self) return;
			if (saved_as_copy) self->open_saved_copy(path);
			else self->display_changed();
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
	if (is_photo()) return _edit_state.has_pixel_changes();
	return edits_av() && _edit_state.has_av_changes();
}

std::vector<core::const_surface_ptr> edit_view::dialog_previews() const
{
	const auto preview = preview_surface();
	return is_valid(preview) ? std::vector{preview} : std::vector<core::const_surface_ptr>{};
}

void edit_view::save(const core::file_path src_path, const core::file_path dst_path, const std::string_view xmp_name,
                     const ui::frame_ptr& owner, std::function<void(bool)> complete)
{
	if (edits_av())
	{
		save_av(src_path, dst_path, xmp_name, owner, std::move(complete));
		return;
	}

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
	                   [update_result, detach, dst_path, me, pe, &s = _state, src_path, scan_request, dimensions,
	                    xmp_name = std::string(xmp_name), photo_metadata, video_metadata, music_metadata,
	                    has_pixel_changes, dlg, complete = std::move(complete), function = __FUNCTION__]() mutable
	{
		try
		{
			files ff;
			file_encode_params encode_params;
			encode_params.jpeg_save_quality = setting.jpeg_save_quality;
			encode_params.webp_lossless = setting.webp_lossless;
			encode_params.webp_quality = setting.webp_quality;
			encode_params.jxl_lossless = setting.jxl_lossless;
			encode_params.jxl_quality = setting.jxl_quality;
			encode_params.avif_lossless = setting.avif_lossless;
			encode_params.avif_quality = setting.avif_quality;

			const auto is_in_place = src_path == dst_path;
			// Whatever file the save replaces gets its backup: the source in place, or
			// one whose overwrite was confirmed - beside a RAW, the camera's own JPEG.
			const auto create_original = setting.create_originals && pe.has_changes(dimensions);
			*update_result = ff.update(src_path, dst_path, me, pe, encode_params, create_original, xmp_name, {},
			                           is_in_place ? index_state::make_rescan_spec(scan_request, xmp_name, true)
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
						s.publish_written_image(scan_request.path, std::move(update_result->loaded),
						                        core::date_t(update_result->modified));

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
			core::log(function, e.what());
			update_result->code = platform::file_op_result_code::FAILED;
			update_result->error_message = core::utf8_cast(e.what());
		}

		s.queue_ui([&s, update_result, detach, dst_path, me, photo_metadata, video_metadata, music_metadata,
		            has_pixel_changes, dlg, complete = std::move(complete)]() mutable
		{
			detach.reset();

			if (update_result->success())
			{
				s.record_change(icon_index::edit, tt.command_edit.sv(), {dst_path});

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
				dlg->show_message(icon_index::error, core::app_name(),
				                  update_result->format_error(
									  str_format(tt.error_create_file_failed_fmt.sv(), dst_path)));
			}

			complete(update_result->success());
		});
	});
}

void edit_view::save_av(const core::file_path src_path, const core::file_path dst_path, const std::string_view xmp_name,
                        const ui::frame_ptr& owner, std::function<void(bool)> complete)
{
	auto update_result = std::make_shared<file_update_result>();
	const auto cancelled = std::make_shared<std::atomic_bool>(false);
	const auto dlg = make_dlg(owner);
	dlg->show_cancel_status(icon_index::save, str_format(tt.saving_file_name.sv(), dst_path.name()),
	                        [cancelled] { *cancelled = true; });

	// The player holds the file open, and the copy is swapped in over it; playing again afterwards from the
	// handle that wrote it.
	auto detach = std::make_shared<detach_file_handles>(_state);
	const auto detached_av_path = _state.detached_display_av_path();
	auto edits = av_draft();
	edits.index_first = setting.media_index_first && av_plan_for(dst_path).can_index_first;
	edits.repair = setting.media_repair;
	const auto is_video = _av_plan.has_video;
	const auto scan_request = _state.item_index.make_scan_request(_state._edit_item, true, false);

	// The rating and tags the item shows, which a format keeps besides its packet - where Windows reads them -
	// and a muxer does not write back.
	const auto md = safe_metadata(_state._edit_item);
	metadata_edits carried;
	if (md->rating != 0) carried.rating = md->rating;
	if (!core::is_empty(md->tags)) carried.add_tags = tag_set(md->tags.sv());

	_state.queue_async(async_queue::work,
	                   [update_result, cancelled, detach, detached_av_path, dst_path, src_path, edits, carried,
	                    scan_request, xmp_name = std::string(xmp_name), &s = _state, is_video, dlg,
	                    complete = std::move(complete), weak = weak_from_this(), function = __FUNCTION__]() mutable
	{
		try
		{
			files ff;
			const auto is_in_place = src_path == dst_path;
			const core::cancel_token cancel(*cancelled);
			const auto rescan = is_in_place ? index_state::make_rescan_spec(scan_request, xmp_name, false,
			                                                                dst_path == detached_av_path)
			                                : rescan_spec{};
			*update_result = ff.update_av(src_path, dst_path, edits, carried, setting.create_originals, xmp_name, {},
			                              rescan, cancel);

			if (update_result->success())
			{
				if (is_in_place)
				{
					const auto force = s.item_index.apply_write_scan(scan_request, *update_result);
					s._async.queue_ui([&s, scan_request, force]
					{
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
			core::log(function, e.what());
			update_result->code = platform::file_op_result_code::FAILED;
			update_result->error_message = core::utf8_cast(e.what());
		}

		s.queue_ui([update_result = std::move(update_result), detach = std::move(detach), dst_path, &s, is_video,
		            dlg = std::move(dlg), weak, complete = std::move(complete)]() mutable
		{
			const auto success = update_result->success();

			// Handed to the reopen before the guard lets the display go, so playing again reads the bytes just
			// written rather than opening the path once more.
			if (success) s.publish_written_handle(dst_path, std::move(update_result->display_handle));
			detach.reset();

			if (success)
			{
				record_feature_use(is_video ? features::edit_video : features::edit_audio);
				s.record_change(icon_index::edit, tt.command_edit.sv(), {dst_path});

				// What was saved is no longer a draft, and the file now holds something else to plan from.
				if (const auto self = weak.lock(); self && self->_path == dst_path)
				{
					self->_edit_state.clear_av();
					self->av_changed();
					self->queue_av_plan();
				}

				// A repaired copy says what it left out of the file it was made from.
				if (update_result->repair_skipped > 0 || update_result->repair_ended_at >= 0.0)
				{
					dlg->show_message(icon_index::repair, core::app_name(),
					                  edit_repair_report(update_result->repair_skipped,
					                                     update_result->repair_ended_at));
				}
			}
			else if (update_result->repair_needs_backup)
			{
				dlg->show_message(icon_index::error, core::app_name(), tt.edit_repair_needs_backup);
			}
			else if (update_result->code != platform::file_op_result_code::CANCELLED)
			{
				dlg->show_message(icon_index::error, core::app_name(),
				                  update_result->format_error(
									  str_format(tt.error_create_file_failed_fmt.sv(), dst_path)));
			}

			complete(success);
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
	if (!can_edit_pixels() && !edits_av()) return;

	auto path = edit_save_suggestion(_path, [](const core::file_path p) { return platform::exists(p); },
	                                 edit_offers_avif(_path, _loaded));
	const auto xmp_name = _xmp_name;

	const auto owner = _host->owner();

	if (select_path(path) && check_path(path, owner))
	{
		save(_path, path, xmp_name, owner, [weak = weak_from_this(), path](const bool success)
		{
			if (const auto self = weak.lock(); success && self) self->open_saved_copy(path);
		});
	}
}

void edit_view::cancel() const
{
	_edit_state._edits.clear();
	_edit_state.clear_av();
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
	if (edits_av())
	{
		if (!_av_plan.can_rotate) return;
		_edit_state._quarter_turns = (_edit_state._quarter_turns + 3) % 4;
		av_changed();
		return;
	}

	if (!can_edit_pixels()) return;
	const auto selection = _edit_state._edits.crop_bounds();
	_edit_state._edits.crop_bounds(selection.transform(core::simple_transform::rot_270));
	const auto horizontal = _edit_state._perspective_horizontal;
	_edit_state._perspective_horizontal = _edit_state._perspective_vertical;
	_edit_state._perspective_vertical = -horizontal;
	_edit_controls->populate();
	changed();
}

void edit_view::rotate_clockwise()
{
	if (edits_av())
	{
		if (!_av_plan.can_rotate) return;
		_edit_state._quarter_turns = (_edit_state._quarter_turns + 1) % 4;
		av_changed();
		return;
	}

	if (!can_edit_pixels()) return;
	const auto selection = _edit_state._edits.crop_bounds();
	_edit_state._edits.crop_bounds(selection.transform(core::simple_transform::rot_90));
	const auto horizontal = _edit_state._perspective_horizontal;
	_edit_state._perspective_horizontal = -_edit_state._perspective_vertical;
	_edit_state._perspective_vertical = horizontal;
	_edit_controls->populate();
	changed();
}

void edit_view::rotate_reset()
{
	if (edits_av())
	{
		_edit_state._quarter_turns = 0;
		av_changed();
		return;
	}

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
void edit_view::queue_auto_adjust(
	const int max_dimension, std::string title,
	std::function<std::function<void(edit_view_state&)>(const core::const_surface_ptr&)> analyze)
{
	if (!can_edit_pixels()) return;

	const auto loaded = _loaded;
	const auto scale_hint = core::scale_dimensions(loaded.dimensions(), max_dimension, true);
	const auto generation = _display_generation;
	const auto edit_generation = _edit_generation;
	const auto weak = weak_from_this();

	_state.queue_async(async_queue::render,
	                   [weak, loaded, scale_hint, generation, edit_generation, title = std::move(title),
	                    analyze = std::move(analyze), &s = _state]() mutable
	{
		auto apply = analyze(loaded.to_surface(scale_hint));

		s.queue_ui([weak, generation, edit_generation, title = std::move(title), apply = std::move(apply)]
		{
			const auto self = weak.lock();
			if (!self || !should_accept_edit_analysis(self->_display_generation, generation, self->_edit_generation,
			                                          edit_generation))
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

std::optional<auto_color_settings> analyze_auto_color(const core::const_surface_ptr& picture)
{
	// Read a byte a channel, whatever precision the picture was loaded at.
	const auto surface = core::to_8_bit(picture);
	if (!is_valid(surface)) return {};

	const auto has_alpha = core::has_alpha(surface->format());
	std::array<uint32_t, 256> luminance_histogram{};
	double neutral_red = 0;
	double neutral_green = 0;
	double neutral_blue = 0;
	size_t neutral_count = 0;
	size_t sample_count = 0;
	double chroma_sum = 0;

	for (auto y = 0u; y < surface->height(); ++y)
	{
		const auto* pixel = surface->pixels_line(static_cast<int>(y));

		// Blue, green, red, then alpha or padding.
		for (auto x = 0u; x < surface->width(); ++x, pixel += 4)
		{
			if (has_alpha && pixel[3] < 128) continue;

			const auto blue = pixel[0] / 255.0;
			const auto green = pixel[1] / 255.0;
			const auto red = pixel[2] / 255.0;
			const auto luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
			const auto maximum = std::max({red, green, blue});
			const auto minimum = std::min({red, green, blue});
			const auto chroma = maximum - minimum;

			++luminance_histogram[std::clamp(core::round(luminance * 255), 0, 255)];
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

	auto_color_settings result;
	result.brightness = std::clamp((0.48 - median) * 2.0, -0.45, 0.45);
	result.contrast = std::clamp((0.82 / range - 1.0) * 0.75, -0.35, 0.35);
	result.darks = std::clamp((0.08 - percentile(0.20)) * 2.5, -0.3, 0.3);
	result.lights = std::clamp((0.82 - percentile(0.80)) * 2.5, -0.3, 0.3);
	result.midtones = std::clamp((0.5 - median) * 1.5, -0.25, 0.25);
	result.vibrance = chroma_sum / sample_count < 0.18 ? 0.08 : 0.0;

	// A cast is read from the near-neutral pixels: a warm one, red above blue, asks for a cooler temperature,
	// and a green one for tint towards magenta.
	if (neutral_count > sample_count / 50)
	{
		const auto red = neutral_red / neutral_count;
		const auto green = neutral_green / neutral_count;
		const auto blue = neutral_blue / neutral_count;
		const auto mean = std::max(0.05, (red + green + blue) / 3.0);
		result.temperature = std::clamp((blue - red) / (0.4 * mean), -0.4, 0.4);
		result.tint = std::clamp((green - (red + blue) * 0.5) / (0.3 * mean), -0.35, 0.35);
	}

	return result;
}

void edit_view::auto_color()
{
	queue_auto_adjust(512, std::string(tt.command_auto_color.sv()),
	                  [](const core::const_surface_ptr& surface) -> std::function<void(edit_view_state&)>
	{
		const auto settings = analyze_auto_color(surface);
		if (!settings) return {};

		return [s = *settings](edit_view_state& state)
		{
			state._brightness = s.brightness;
			state._contrast = s.contrast;
			state._darks = s.darks;
			state._lights = s.lights;
			state._midtones = s.midtones;
			state._vibrance = s.vibrance;
			state._saturation = 0;
			state._temperature = s.temperature;
			state._tint = s.tint;
		};
	});
}

void edit_view::auto_straighten()
{
	const auto current_straighten = edit_view_state::calc_straighten(_edit_state._edits.crop_bounds().angle());
	const auto rotation_angle = _edit_state._edits.rotation_angle();

	queue_auto_adjust(768, std::string(tt.command_auto_straighten.sv()),
	                  [current_straighten,
	                   rotation_angle](const core::const_surface_ptr& picture) -> std::function<void(edit_view_state&)>
	{
		const auto surface = core::to_8_bit(picture);
		if (!is_valid(surface) || surface->width() < 3 || surface->height() < 3) return {};

		const auto width = static_cast<int>(surface->width());
		const auto height = static_cast<int>(surface->height());
		std::vector<float> grayscale(static_cast<size_t>(width) * height);

		for (auto y = 0; y < height; ++y)
		{
			const auto* pixels = std::bit_cast<const core::color32*>(surface->pixels_line(y));
			for (auto x = 0; x < width; ++x)
			{
				grayscale[static_cast<size_t>(y) * width + x] = static_cast<float>(packed_luma(pixels[x]));
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
			const auto gx = -at(x - 1, y - 1) - 2.0f * at(x - 1, y) - at(x - 1, y + 1) + at(x + 1, y - 1) +
			                2.0f * at(x + 1, y) + at(x + 1, y + 1);
			const auto gy = -at(x - 1, y - 1) - 2.0f * at(x, y - 1) - at(x + 1, y - 1) + at(x - 1, y + 1) +
			                2.0f * at(x, y + 1) + at(x + 1, y + 1);
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

				const auto position = horizontal ? (y + 0.5) / height - 0.5 : (x + 0.5) / width - 0.5;
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
				if (sample.horizontal == horizontal && std::abs(sample.angle - initial) <= inlier_angle)
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

		const auto detected_angle = horizontal_estimate.second >= vertical_estimate.second ? horizontal_estimate.first
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
				if (sample.horizontal == horizontal && std::abs(sample.angle - baseline_angle) <= inlier_angle)
				{
					const auto bin = std::clamp(static_cast<int>((sample.position + 0.5) * bin_count), 0,
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
			return populated_bins >= 5 && last_bin - first_bin >= 6 && denominator > 0.1 ? numerator / denominator
			                                                                             : 0.0;
		};

		const auto source_horizontal = std::clamp(-perspective_slope(true, horizontal_estimate.first), -0.4, 0.4);
		const auto source_vertical = std::clamp(perspective_slope(false, vertical_estimate.first), -0.4, 0.4);
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
	                  [dimensions,
	                   orientation](const core::const_surface_ptr& surface) -> std::function<void(edit_view_state&)>
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

// The colour the preview is drawn with: the edit's grade, where it changes the colour.
static render::texture_transform edit_color_transform(const image_edits& edits)
{
	render::texture_transform transform;

	if (edits.has_color_changes())
	{
		render::color_adjust adjust;
		adjust.color_params(edits.vibrance(), edits.saturation(), edits.darks(), edits.midtones(), edits.lights(),
		                    edits.contrast(), edits.brightness(), edits.temperature(), edits.tint());
		adjust.populate_texture_transform(transform);
	}

	return transform;
}

const edit_histogram& edit_view::histogram()
{
	if (_histogram_counted_at != _edit_generation)
	{
		const auto grade = edit_color_transform(_edit_state._edits);
		_histogram_counts = count_edit_histogram(_histogram_samples, &grade);
		_histogram_counted_at = _edit_generation;
	}

	return _histogram_counts;
}

// The before-and-after handle's knob, large enough to grab beside a photo of any size.
static int compare_knob_diameter(const render::measure_context& mc)
{
	return core::round(mc.icon_cxy * 1.5);
}

void edit_view::compare_at(const int x)
{
	if (_compare_track.width() <= 0) return;
	_comparing = true;
	_compare.reset(static_cast<float>(
		std::clamp((x - _compare_track.left) / static_cast<double>(_compare_track.width()), 0.0, 1.0)));
	_host->frame()->invalidate();
}

int edit_view::compare_rest_x(const int picture_left, const int knob_diameter, const int margin, const int edge_gap)
{
	const auto radius = knob_diameter / 2;
	return std::max(radius + edge_gap, picture_left - radius - margin);
}

void edit_view::compare_release()
{
	_comparing = false;
	_compare.target(0.0f);
	animate();
	_host->frame()->invalidate();
}

void edit_view::animate()
{
	const auto weak = weak_from_this();
	ui::animations[&_compare] = [weak]
	{
		const auto self = weak.lock();
		if (!self) return false;
		// Both stepped every time: a short-circuit would hold the grid's fade until the line was back.
		const auto line_moved = self->_compare.step();
		const auto grid_faded = self->_edit_state.grid_alpha_animation.step();
		if (line_moved || grid_faded) self->_host->frame()->invalidate();
		return line_moved || grid_faded;
	};
	_host->invalidate(ui::host_invalid::animations);
}

void edit_view::draw_compare(render::draw_context& dc, const recti shown, const int line_x, const bool engaged,
                             const float alpha)
{
	const auto scale1 = std::max(1, core::round(dc.scale_factor));
	const auto accent = core::color(ui::style::colors.dialog_selected_background, alpha * (engaged ? 1.0f : 0.8f));

	if (engaged)
	{
		dc.draw_rect(recti(line_x - scale1, shown.top, line_x + scale1, shown.bottom), accent);

		// Each side named beside the line, where there is room for its name.
		const auto label = [&](const std::string_view text, const bool left)
		{
			const auto extent = dc.measure_text(text, render::font_face::dialog, render::text_style::single_line,
			                                    shown.width())
			                        .inflate(dc.padding1);
			const auto room = left ? line_x - shown.left : shown.right - line_x;
			if (room < extent.cx + dc.padding2 * 2) return;
			const auto x = left ? line_x - dc.padding2 - extent.cx : line_x + dc.padding2;
			const recti bounds(x, shown.top + dc.padding2, x + extent.cx, shown.top + dc.padding2 + extent.cy);
			dc.draw_rounded_rect(bounds, core::color(0, alpha * 0.6f), dc.padding1);
			dc.draw_text(text, bounds, render::font_face::dialog, render::text_style::single_line_center,
			             core::color(ui::style::colors.view_text, alpha), {});
		};

		label(tt.edit_original, true);
		label(tt.command_edit_preview, false);
	}

	// A round knob on the line - at rest, beside the picture's left edge - with an arrow each way.
	const auto diameter = compare_knob_diameter(dc);
	const auto center = pointi(line_x, (shown.top + shown.bottom) / 2);
	_compare_handle = recti(center.x - diameter / 2, center.y - diameter / 2, center.x - diameter / 2 + diameter,
	                        center.y - diameter / 2 + diameter);
	dc.fill_ellipse(_compare_handle, accent);
	dc.draw_ellipse(_compare_handle, core::color(ui::style::colors.view_text, alpha * 0.8f), dc.scale_factor);

	const auto arrow = diameter * 0.16;
	const auto gap = diameter * 0.1;
	const auto cx = static_cast<double>(center.x);
	const auto cy = static_cast<double>(center.y);
	render::polygon_style arrows;
	arrows.fill = render::paint(core::color(ui::style::colors.view_text, alpha));
	const std::array<pointd, 3> left_arrow{pointd(cx - gap - arrow, cy), pointd(cx - gap, cy - arrow),
	                                       pointd(cx - gap, cy + arrow)};
	const std::array<pointd, 3> right_arrow{pointd(cx + gap + arrow, cy), pointd(cx + gap, cy + arrow),
	                                        pointd(cx + gap, cy - arrow)};
	dc.draw_polygon(left_arrow, arrows);
	dc.draw_polygon(right_arrow, arrows);
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
		// The grid shows while the picture is straightened, and fades once it is let go.
		if (_edit_state.grid_alpha_animation.val() != _edit_state.grid_alpha_animation.target()) animate();

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

void edit_view_controls::layout_controls(render::measure_context& mc)
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
		_histogram->is_visible(show_pixel_controls);
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

		// A video or a sound file has its own controls, and its copy no quality to choose.
		const auto is_av = _view && _view->is_av();
		const auto edits_av = _view && _view->edits_av();
		const av_edit_plan no_plan;
		const auto& plan = _view ? _view->av_plan() : no_plan;
		const auto can_trim = edits_av && plan.duration > 0.0;
		_info->text(is_av ? tt.edit_av_info : tt.edit_info);
		_trim_title->is_visible(can_trim);
		_trim_hint->is_visible(can_trim);
		_trim_buttons->is_visible(can_trim);
		_remove_sound_check->is_visible(edits_av && plan.has_video && plan.has_audio);
		_turn_title->is_visible(edits_av && plan.can_rotate);
		_turn_buttons->is_visible(edits_av && plan.can_rotate);
		_index_first_check->is_visible(edits_av && plan.can_index_first);
		_repair_check->is_visible(edits_av);
		update_notes();
		_jpeg_quality_slider->is_visible(!is_av);
		_webp_quality_slider->is_visible(!is_av);
		_webp_lossless_check->is_visible(!is_av);
		_jxl_quality_slider->is_visible(!is_av);
		_jxl_lossless_check->is_visible(!is_av);
		_avif_quality_slider->is_visible(!is_av);
		_avif_lossless_check->is_visible(!is_av);

		view_controls_host::layout_controls(mc);
	}
}

bool edit_view_controls::update_notes()
{
	const auto edits_av = _view && _view->edits_av();
	std::vector<std::string> notes;

	if (edits_av)
	{
		const auto& plan = _view->av_plan();
		if (plan.has_video && !plan.exact_start && _edit_state._trim_start > 0.0)
			notes.emplace_back(tt.edit_trim_key_frame.sv());
		if (!plan.dropped.empty())
			notes.emplace_back(str_format(tt.edit_not_kept_fmt.sv(), core::combine(plan.dropped, ", ", false)));

		// Save As offers MP4 beside the file's own format, which may hold less of it.
		const auto& mp4 = _view->av_mp4_plan();
		if (mp4.can_edit && !mp4.dropped.empty() && mp4.dropped != plan.dropped)
			notes.emplace_back(str_format(tt.edit_not_kept_mp4_fmt.sv(), core::combine(mp4.dropped, ", ", false)));
	}
	else if (_view && _view->is_av() && !_view->av_plan_pending())
	{
		notes.emplace_back(_view->av_plan().readable ? tt.edit_av_unavailable.sv() : tt.edit_av_unreadable.sv());
	}
	else if (_view && _view->is_photo())
	{
		if (const auto note = edit_photo_note(_view->signal()); !note.empty()) notes.emplace_back(note);
	}

	// Each reset is there while there is something for it to undo.
	_trim_reset->enable(_edit_state._trim_start > 0.0 || _edit_state._trim_end > 0.0);
	_turn_reset->enable(_edit_state._quarter_turns % 4 != 0);

	const auto note = core::combine(notes, "\n\n", false);
	const auto changed = note != _note_text || _note->is_visible() == notes.empty();
	_note_text = note;
	_note->text(note);
	_note->is_visible(!notes.empty());
	return changed;
}

void edit_view_controls::options_changed()
{
	view_controls_host::options_changed();

	if (!_controls.empty())
	{
		_info->text(_view && _view->is_av() ? tt.edit_av_info : tt.edit_info);
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
		_trim_title->text(tt.edit_trim_title);
		_trim_hint->text(tt.edit_trim_hint);
		_trim_reset->window_text(tt.edit_trim_clear);
		_turn_title->text(tt.edit_turn_title);
		_turn_left->window_text(tt.edit_rotate_left);
		_turn_right->window_text(tt.edit_rotate_right);
		_turn_reset->window_text(tt.command_rotate_reset);
		update_notes();
		_save_title->text(tt.options_save_options);
		_jpeg_quality_slider->label(tt.options_jpeg_quality);
		_webp_quality_slider->label(tt.options_webp_quality);
		_jxl_quality_slider->label(tt.options_jxl_quality);
		_avif_quality_slider->label(tt.options_avif_quality);
	}
}

void edit_view_controls::create_controls()
{
	const std::vector<ui::command_ptr> rotate_buttons = {
		std::make_shared<ui::command>(icon_index::rotate_anticlockwise, commands::tool_rotate_anticlockwise,
	                                  [this] { _view->rotate_anticlockwise(); }),
		std::make_shared<ui::command>(icon_index::rotate_clockwise, commands::tool_rotate_clockwise,
	                                  [this] { _view->rotate_clockwise(); }),
		std::make_shared<ui::command>(icon_index::lightbulb, commands::edit_item_auto_straighten,
	                                  [this] { _view->auto_straighten(); }),
		std::make_shared<ui::command>(icon_index::scan_text, commands::edit_item_auto_document,
	                                  [this] { _view->auto_document(); }),
		std::make_shared<ui::command>(icon_index::undo, commands::tool_rotate_reset, [this] { _view->rotate_reset(); }),
	};

	const std::vector<ui::command_ptr> color_buttons = {
		std::make_shared<ui::command>(icon_index::lightbulb, commands::edit_item_auto_color,
	                                  [this] { _view->auto_color(); }),
		std::make_shared<ui::command>(icon_index::undo, commands::edit_item_color_reset,
	                                  [this] { _view->color_reset(); }),
	};

	auto changed_func = [this]
	{
		_view->changed();
	};

	_info = std::make_shared<view_info_element>(icon_index::edit, tt.command_edit, tt.edit_info);
	_straighten_title = std::make_shared<ui::title_control>(icon_index::rotate_clockwise, tt.straighten);
	_straighten_slider = std::make_shared<log_slider_control>(_frame, std::string_view{}, _edit_state._straighten,
	                                                          -44.9, 44.9, changed_func);
	_perspective_horizontal_slider = std::make_shared<log_slider_control>(_frame, tt.perspective_horizontal,
	                                                                      _edit_state._perspective_horizontal, -0.6,
	                                                                      0.6, changed_func);
	_perspective_vertical_slider = std::make_shared<log_slider_control>(_frame, tt.perspective_vertical,
	                                                                    _edit_state._perspective_vertical, -0.6, 0.6,
	                                                                    changed_func);
	_rotate_toolbar = std::make_shared<task_toolbar_control>(_frame, rotate_buttons);
	_color_divider = std::make_shared<divider_element>();
	_color_title = std::make_shared<ui::title_control>(icon_index::color, tt.color);
	const std::array<edit_histogram_control::tone_handle, 3> tone_handles{
		{{_edit_state._darks, tt.darks}, {_edit_state._midtones, tt.midtones}, {_edit_state._lights, tt.lights}}};
	auto counted = [this]() -> const edit_histogram&
	{
		return _view->histogram();
	};
	auto tone_moved = [this]
	{
		populate();
		_view->changed();
	};
	_histogram = std::make_shared<edit_histogram_control>(std::move(counted), tone_handles, std::move(tone_moved));
	_vibrance_slider = std::make_shared<log_slider_control>(_frame, tt.vibrance, _edit_state._vibrance, -1, 1,
	                                                        changed_func);
	_darks_slider = std::make_shared<log_slider_control>(_frame, tt.darks, _edit_state._darks, -1, 1, changed_func);
	_midtones_slider = std::make_shared<log_slider_control>(_frame, tt.midtones, _edit_state._midtones, -1, 1,
	                                                        changed_func);
	_lights_slider = std::make_shared<log_slider_control>(_frame, tt.lights, _edit_state._lights, -1, 1, changed_func);
	_contrast_slider = std::make_shared<log_slider_control>(_frame, tt.contrast, _edit_state._contrast, -1, 1,
	                                                        changed_func);
	_brightness_slider = std::make_shared<log_slider_control>(_frame, tt.brightness, _edit_state._brightness, -1, 1,
	                                                          changed_func);
	_saturation_slider = std::make_shared<log_slider_control>(_frame, tt.saturation, _edit_state._saturation, -1, 1,
	                                                          changed_func);
	_temperature_slider = std::make_shared<log_slider_control>(_frame, tt.temperature, _edit_state._temperature, -1, 1,
	                                                           changed_func);
	_tint_slider = std::make_shared<log_slider_control>(_frame, tt.tint, _edit_state._tint, -1, 1, changed_func);
	_color_toolbar = std::make_shared<task_toolbar_control>(_frame, color_buttons);
	_trim_title = std::make_shared<ui::title_control>(icon_index::edit_cut, tt.edit_trim_title);
	_trim_hint = std::make_shared<text_element>(tt.edit_trim_hint);
	_trim_reset = ui::make_button(icon_index::undo, tt.edit_trim_clear, {}, [this] { _view->trim_clear(); });
	_trim_buttons = std::make_shared<ui::button_row>(std::vector<ui::control_ptr>{_trim_reset},
	                                                 ui::button_row_fit::wrap);
	_remove_sound_check = std::make_shared<ui::check_control>(_frame, tt.edit_remove_sound, _edit_state._remove_sound,
	                                                          false, false, [this](bool) { _view->av_changed(); });
	_turn_title = std::make_shared<ui::title_control>(icon_index::rotate_clockwise, tt.edit_turn_title);
	_turn_left = ui::make_button(icon_index::rotate_anticlockwise, tt.edit_rotate_left, {},
	                             [this] { _view->rotate_anticlockwise(); });
	_turn_right = ui::make_button(icon_index::rotate_clockwise, tt.edit_rotate_right, {},
	                              [this] { _view->rotate_clockwise(); });
	_turn_reset = ui::make_button(icon_index::undo, tt.command_rotate_reset, {}, [this] { _view->rotate_reset(); });
	_turn_buttons = std::make_shared<ui::button_row>(std::vector<ui::control_ptr>{_turn_left, _turn_right, _turn_reset},
	                                                 ui::button_row_fit::wrap);
	_note = std::make_shared<text_element>(std::string_view{});
	_save_divider = std::make_shared<divider_element>();
	_save_title = std::make_shared<ui::title_control>(icon_index::save, tt.options_save_options);
	_backup_check = std::make_shared<ui::check_control>(_frame, tt.options_backup_copy, setting.create_originals);
	_index_first_check = std::make_shared<ui::check_control>(_frame, tt.edit_index_first, setting.media_index_first);
	_repair_check = std::make_shared<ui::check_control>(_frame, tt.edit_repair, setting.media_repair);
	_jpeg_quality_slider = std::make_shared<ui::slider_control>(_frame, tt.options_jpeg_quality,
	                                                            setting.jpeg_save_quality, 0, 100);
	_webp_quality_slider = std::make_shared<ui::slider_control>(_frame, tt.options_webp_quality, setting.webp_quality,
	                                                            1, 100);
	_webp_lossless_check = std::make_shared<ui::check_control>(_frame, tt.lossless_compression, setting.webp_lossless);
	_jxl_quality_slider = std::make_shared<ui::slider_control>(_frame, tt.options_jxl_quality, setting.jxl_quality, 1,
	                                                           100);
	_jxl_lossless_check = std::make_shared<ui::check_control>(_frame, tt.lossless_compression, setting.jxl_lossless);
	_avif_quality_slider = std::make_shared<ui::slider_control>(_frame, tt.options_avif_quality, setting.avif_quality,
	                                                            1, 100);
	_avif_lossless_check = std::make_shared<ui::check_control>(_frame, tt.lossless_compression, setting.avif_lossless);
	_controls = {
		_info,
		_straighten_title,
		_straighten_slider,
		_perspective_horizontal_slider,
		_perspective_vertical_slider,
		_rotate_toolbar,
		_color_divider,
		_color_title,
		_histogram,
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
		_trim_title,
		_trim_hint,
		_trim_buttons,
		_remove_sound_check,
		_turn_title,
		_turn_buttons,
		_note,
		_save_divider,
		_save_title,
		_backup_check,
		_index_first_check,
		_repair_check,
		_jpeg_quality_slider,
		_webp_quality_slider,
		_webp_lossless_check,
		_jxl_quality_slider,
		_jxl_lossless_check,
		_avif_quality_slider,
		_avif_lossless_check,
	};

	_clr = core::color(ui::style::colors.dialog_text);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void edit_view_state::reset(const prop::item_metadata_const_ptr& /*md*/, const sizei dimensions,
                            const core::orientation orientation)
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
	clear_av();

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

	// What the last item's draft showed on the player is not this one's.
	clear_av_preview();

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
	_av_plan = {};
	_av_mp4_plan = {};
	_is_av = false;
	_av_plan_pending = false;
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
		else if (can_edit_in_edit_view(item))
		{
			// The player shows a video or a sound file, so there are no pixels to load: a worker reads what a
			// copy of it can do instead.
			_is_av = true;
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

	if (_is_av) queue_av_plan();

	if (_edit_controls->_frame)
	{
		_edit_controls->populate();
		_edit_controls->_frame->layout();
	}

	changed();
	av_changed();
}

void edit_view::draw_handle(render::draw_context& dc, const recti handle_bounds2, const float alpha)
{
	const auto handle_clr = core::color(ui::style::colors.dialog_selected_background, alpha * dc.colors.bg_alpha);
	const auto handle_bg_clr = core::color(ui::style::colors.dialog_selected_background, alpha * dc.colors.bg_alpha);

	dc.draw_rect(handle_bounds2, handle_clr);
	dc.draw_border(handle_bounds2.inflate(core::round(-2 * dc.scale_factor)), handle_bounds2, handle_bg_clr,
	               handle_bg_clr);
}

void edit_view::render(render::draw_context& dc, view_controller_ptr /*controller*/)
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
		const auto selection_rotated_bounds = preview_bounds.transform(affined().rotate(selection_angle))
		                                          .bounding_rect();
		const auto view_bounds = rectd(0, 0, _extent.cx, _extent.cy);
		const auto view_scale = std::min((view_bounds.Width - pad30) / selection_rotated_bounds.Width,
		                                 (view_bounds.Height - pad30) / selection_rotated_bounds.Height);

		_image_transform = affined()
		                       .translate(-image_bounds.center_point())
		                       .rotate(selection_angle)
		                       .scale(view_scale)
		                       .translate(view_bounds.center());
		if (_edit_state._preview_mode)
		{
			const auto transformed_crop = selection_bounds.transform(_image_transform).bounding_rect();
			_image_transform = _image_transform.translate(view_bounds.center() - transformed_crop.center());
		}

		const auto image_view_bounds = image_bounds.transform(_image_transform);

		if (_invalid || !_texture)
		{
			const auto preview_dimensions = core::scale_dimensions(_loaded.dimensions(), _extent);
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
					    t && t->update(_preview_source) != render::texture_update_result::failed)
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
				if (t && t->update(_preview_source) != render::texture_update_result::failed)
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
		                                  core::orientation::top_left);

		// The edit's colour on its perspective, which turns the picture's texture as the save will.
		auto texture_transform = edit_color_transform(_edit_state._edits);
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
		// The original as the file shows it - whole, upright and in its own colour - and the photo as the edit will
		// save it - cropped, turned, straightened and coloured - each fitted to bounds: Preview's two columns, and
		// the two sides of the before-and-after line.
		const quadd original_image_bounds(_loaded.dimensions());
		const auto original_selection_bounds = _edit_state._original_edits.effective_crop_bounds(_loaded.dimensions());
		const auto original_angle = -original_selection_bounds.angle();
		const auto crop_rotated_bounds = selection_bounds.transform(affined().rotate(selection_angle)).bounding_rect();

		const auto comparison_transform = [&](const recti bounds)
		{
			const rectd target_bounds(bounds);
			const auto scale = std::min(target_bounds.Width / crop_rotated_bounds.Width,
			                            target_bounds.Height / crop_rotated_bounds.Height);
			const auto transform = affined()
			                           .translate(-image_bounds.center_point())
			                           .rotate(selection_angle)
			                           .scale(scale)
			                           .translate(target_bounds.center());
			const auto transformed_crop = selection_bounds.transform(transform).bounding_rect();
			return transform.translate(target_bounds.center() - transformed_crop.center());
		};
		const auto original_transform = [&](const recti bounds)
		{
			const rectd target_bounds(bounds);
			const auto original_rotated_bounds = original_selection_bounds.transform(affined().rotate(original_angle))
			                                         .bounding_rect();
			const auto scale = std::min(target_bounds.Width / original_rotated_bounds.Width,
			                            target_bounds.Height / original_rotated_bounds.Height);
			const auto transform = affined()
			                           .translate(-original_image_bounds.center_point())
			                           .rotate(original_angle)
			                           .scale(scale)
			                           .translate(target_bounds.center());
			const auto transformed_crop = original_selection_bounds.transform(transform).bounding_rect();
			return transform.translate(target_bounds.center() - transformed_crop.center());
		};

		// Each picture is clipped to its crop and to bounds, which is a column, or a side of the line.
		const auto draw_comparison_image = [&](const recti bounds, const affined& transform, const bool edited)
		{
			const auto& comparison_image_bounds = edited ? image_bounds : original_image_bounds;
			const auto transformed_image_bounds = comparison_image_bounds.transform(transform);
			const auto crop_rect =
				(edited ? selection_bounds : original_selection_bounds).transform(transform).bounding_rect();
			constexpr auto rounding_tolerance = 1e-6;
			const recti crop_clip(static_cast<int>(std::ceil(crop_rect.left() - rounding_tolerance)),
			                      static_cast<int>(std::ceil(crop_rect.top() - rounding_tolerance)),
			                      static_cast<int>(std::floor(crop_rect.right() + rounding_tolerance)),
			                      static_cast<int>(std::floor(crop_rect.bottom() + rounding_tolerance)));
			const auto comparison_clip = crop_clip.intersection(bounds).intersection(clip_rect);
			const auto comparison_sampler = calc_sampler(transformed_image_bounds.extent().round(),
			                                             _texture->dimensions(), core::orientation::top_left);
			dc.clip_bounds(comparison_clip);
			if (edited && texture_transform.has_changes())
				dc.draw_texture(_texture, transformed_image_bounds, _texture->dimensions(), alpha, comparison_sampler,
				                texture_transform);
			else dc.draw_texture(_texture, transformed_image_bounds, _texture->dimensions(), alpha, comparison_sampler);
			dc.restore_clip();
			return crop_clip;
		};

		if (_edit_state._preview_mode)
		{
			const auto outer_padding = dc.padding2 * 2;
			const auto column_gap = dc.padding2 * 2;
			const auto label_height = dc.text_line_height(render::font_face::dialog) + dc.padding1;
			const auto column_width = std::max(1, (_extent.cx - outer_padding * 2 - column_gap) / 2);
			const auto image_top = outer_padding + label_height;
			const recti original_bounds(outer_padding, image_top, outer_padding + column_width,
			                            std::max(image_top + 1, _extent.cy - outer_padding));
			const recti edited_bounds(original_bounds.right + column_gap, image_top,
			                          std::max(original_bounds.right + column_gap + 1, _extent.cx - outer_padding),
			                          original_bounds.bottom);

			const auto original_transform_value = original_transform(original_bounds);
			const auto edited_transform = comparison_transform(edited_bounds);
			draw_comparison_image(original_bounds, original_transform_value, false);
			draw_comparison_image(edited_bounds, edited_transform, true);
			_image_transform = edited_transform;

			const auto text_color = core::color(dc.colors.foreground, alpha);
			const recti original_label(original_bounds.left, outer_padding, original_bounds.right, image_top);
			const recti edited_label(edited_bounds.left, outer_padding, edited_bounds.right, image_top);
			dc.draw_text(tt.edit_original, original_label, render::font_face::dialog,
			             render::text_style::single_line_center, text_color, {});
			dc.draw_text(tt.command_edit_preview, edited_label, render::font_face::dialog,
			             render::text_style::single_line_center, text_color, {});
			return;
		}

		const auto draw_picture = [&](const render::texture_transform& transform)
		{
			if (transform.has_changes())
				dc.draw_texture(_texture, image_view_bounds, _texture->dimensions(), alpha, sampler, transform);
			else dc.draw_texture(_texture, image_view_bounds, _texture->dimensions(), alpha, sampler);
		};

		// The before-and-after handle rests clear of the picture's left edge where the view has room, so it covers
		// none of the picture, and its line travels from there to the view's right.
		const auto picture = image_view_bounds.bounding_rect().round().intersection(recti(_extent));
		const auto view_margin = pad30 / 2;
		const auto rest_x = compare_rest_x(picture.left, compare_knob_diameter(dc), view_margin, dc.padding1);
		_compare_track = recti(rest_x, picture.top, std::max(rest_x + 1, _extent.cx - view_margin), picture.bottom);
		const auto line_x = compare_line_x();

		if (line_x > _compare_track.left)
		{
			// A true before and after: left of the line the original as the file shows it, right of it the photo as
			// the edit will save it, each fitted to the view as Preview fits its columns. Its crop is the picture
			// rather than a frame drawn over it, so the frame and its handles wait until the line is back.
			const recti area(view_margin, view_margin, std::max(view_margin + 1, _extent.cx - view_margin),
			                 std::max(view_margin + 1, _extent.cy - view_margin));
			const auto before = draw_comparison_image(recti(clip_rect.left, clip_rect.top, line_x, clip_rect.bottom),
			                                          original_transform(area), false);
			const auto after = draw_comparison_image(recti(line_x, clip_rect.top, clip_rect.right, clip_rect.bottom),
			                                         comparison_transform(area), true);
			draw_compare(dc, before.make_union(after).intersection(recti(_extent)), line_x, true, alpha);
			return;
		}

		draw_picture(texture_transform);

		const auto draw_crop = selection_bounds.crop(rectd(0, 0, image_extent.cx, image_extent.cy));
		const auto actual_size = draw_crop.actual_extent();
		const auto crop_bounding = draw_crop.transform(_image_transform).bounding_rect();
		const auto border = recti(0, 0, _extent.cx, _extent.cy);

		_crop_bounds = crop_bounding;

		const auto pad1 = core::round(1 * dc.scale_factor);
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
		auto c1 = core::color(0, alpha / 2.0f);
		dc.draw_border(bounding, border, c1, c1);
		auto c2 = core::color(ui::style::colors.dialog_selected_background, alpha);
		dc.draw_border(bounding, bounding.inflate(pad1), c2, c2);

		const auto grid_alpha = _edit_state.grid_alpha_animation.val();

		if (grid_alpha > 0)
		{
			const auto c = core::color(0, grid_alpha);

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

		const auto text_dims = std::format("{}x{}", core::round(actual_size.Width), core::round(actual_size.Height));
		const sizei text_size = dc.measure_text(text_dims, render::font_face::dialog,
		                                        render::text_style::single_line_center, bounding.width())
		                            .inflate(dc.padding2);
		const auto text_x = (bounding.left + bounding.right - text_size.cx) / 2;
		const recti draw_text_rect(text_x, bounding.top - text_size.cy, text_x + text_size.cx, bounding.top);

		dc.draw_text(text_dims, draw_text_rect, render::font_face::dialog, render::text_style::single_line_center,
		             core::color(dc.colors.foreground, alpha), {});

		if (setting.show_debug_info)
		{
			const auto text_degs = core::print("%3.3f degrees", selection_angle);
			dc.draw_text(text_degs, crop_bounding.round().inflate(core::round(100 * dc.scale_factor)),
			             render::font_face::title, render::text_style::single_line_center,
			             core::color(ui::style::colors.warning_background, alpha), {});
		}

		draw_compare(dc, picture, line_x, false, alpha);
	}
	else if (_media_element)
	{
		// A video or a sound file plays here as it does anywhere, with the draft shown on it.
		_media_element->render(dc, {0, 0});
		_play_element->render(dc, {0, 0});
		_scrubber_element->render(dc, {0, 0});
	}
	else if (!_loaded.is_empty())
	{
		const auto alpha = dc.colors.alpha;
		const auto dims = core::scale_dimensions(_loaded.dimensions(), _extent, true);
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
				if (t && t->update(_preview_source) != render::texture_update_result::failed)
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

		const auto sampler = calc_sampler(bounds.extent(), _texture->dimensions(), core::orientation::top_left);
		dc.draw_texture(_texture, bounds, alpha, sampler);

		auto draw_text_rect = recti(_extent);
		draw_text_rect.top = bounds.bottom + dc.padding2;
		draw_text_rect.bottom = draw_text_rect.top + dc.text_line_height(render::font_face::title) + dc.padding2 * 2;

		dc.draw_text(_path.name(), draw_text_rect, render::font_face::title, render::text_style::single_line_center,
		             core::color(dc.colors.foreground, alpha), {});
	}
	else
	{
		const auto status = edit_load_status_text(_loaded, _source_loading);
		draw_loading_status(dc, status);
	}
}
