// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Pure image zoom scale, source-space center, anchoring, bounds geometry,
// and navigator visibility timing.

#pragma once

#include "libdesktop/util_geometry.h"

namespace df
{
	enum class zoom_scale_mode
	{
		fit,
		fit_width,
		fill,
		explicit_scale
	};

	enum class zoom_pane
	{
		primary,
		secondary
	};

	struct zoom_geometry
	{
		rectd destination;
		pointd center;
	};

	class zoom_view_state
	{
		zoom_scale_mode _mode = zoom_scale_mode::fit;
		double _scale = 1.0;
		pointd _center{0.5, 0.5};
		pointd _last_explicit_center{0.5, 0.5};
		// What Fit replaced, so toggling back restores it as it was. A Fit width or Fill left for Fit
		// came back as the fixed scale it last computed, which then stopped following the viewport.
		zoom_scale_mode _restore_mode = zoom_scale_mode::explicit_scale;
		sized _source;
		bool _carried_fit = false;
		bool _has_explicit = false;

		static constexpr std::array<double, 19> scale_ladder{
			0.05, 0.07, 0.10, 0.15, 0.20, 0.25, 0.33, 0.50, 0.67, 0.75,
			1.00, 1.50, 2.00, 3.00, 4.00, 6.00, 8.00, 12.00, 16.00
		};

		static pointd clamp_center(const pointd center, const sized source, const sized viewport,
		                           const double scale) noexcept
		{
			const auto clamp_axis = [scale](const double value, const double source_extent,
			                                const double viewport_extent)
			{
				if (source_extent <= 0.0 || source_extent * scale <= viewport_extent) return 0.5;
				const auto half_visible = viewport_extent / (2.0 * source_extent * scale);
				return std::clamp(value, half_visible, 1.0 - half_visible);
			};

			return {
				clamp_axis(center.X, source.Width, viewport.Width),
				clamp_axis(center.Y, source.Height, viewport.Height)
			};
		}

	public:
		zoom_scale_mode mode() const noexcept { return _mode; }
		bool is_fit() const noexcept { return _mode == zoom_scale_mode::fit; }

		bool is_carried_fit() const noexcept { return _carried_fit; }
		double explicit_scale() const noexcept { return _scale; }
		pointd center() const noexcept { return _center; }

		static double fit_scale(const sized source, const sized viewport, const bool enlarge = false) noexcept
		{
			if (source.Width <= 0.0 || source.Height <= 0.0 || viewport.Width <= 0.0 || viewport.Height <= 0.0)
				return 1.0;
			const auto scale = std::min(viewport.Width / source.Width, viewport.Height / source.Height);
			return enlarge ? scale : std::min(1.0, scale);
		}

		static double fit_variant_scale(const zoom_scale_mode mode, const sized source, const sized viewport,
		                                const bool enlarge = false) noexcept
		{
			if (source.Width <= 0.0 || source.Height <= 0.0 || viewport.Width <= 0.0 || viewport.Height <= 0.0)
				return 1.0;
			auto scale = fit_scale(source, viewport, enlarge);
			if (mode == zoom_scale_mode::fit_width) scale = viewport.Width / source.Width;
			else if (mode == zoom_scale_mode::fill)
				scale = std::max(viewport.Width / source.Width, viewport.Height / source.Height);
			return enlarge || mode != zoom_scale_mode::fit ? scale : std::min(1.0, scale);
		}

		static constexpr const std::array<double, 19>& ladder() noexcept { return scale_ladder; }

		static pointd accelerate_pan(const pointd delta, const double ramp) noexcept
		{
			const auto distance = std::hypot(delta.X, delta.Y);
			if (distance <= 0.0 || ramp <= 0.0) return delta;
			return delta * (1.0 + distance / ramp);
		}
		static pointd auto_pan_velocity(const pointd offset, const double dead_zone = 12.0,
		                                const double max_speed = 1200.0) noexcept
		{
			const auto distance = std::hypot(offset.X, offset.Y);
			if (distance <= dead_zone || max_speed <= 0.0) return {};
			const auto speed = std::min(max_speed, (distance - dead_zone) * 8.0);
			return offset * (speed / distance);
		}

		static pointd navigator_center(const pointd location, const sized extent) noexcept
		{
			return {
				std::clamp(location.X / std::max(1.0, extent.Width), 0.0, 1.0),
				std::clamp(location.Y / std::max(1.0, extent.Height), 0.0, 1.0)
			};
		}

		// zoom.md: looking around. The pointer maps onto the source-space centre, so crossing the
		// viewport sweeps the whole extent. An axis is pinned unless the picture is larger than the
		// viewport along it at this scale: without something to see there, following the pointer is only
		// jitter. Both axes are tested, because a tall stitch qualifies as readily as a wide one.
		static pointd look_around_center(const pointd local, const sized source, const sized viewport,
		                                 const double scale) noexcept
		{
			if (viewport.Width <= 0.0 || viewport.Height <= 0.0) return {0.5, 0.5};

			const auto horizontal_has_travel = source.Width * scale > viewport.Width + 1.0;
			const auto vertical_has_travel = source.Height * scale > viewport.Height + 1.0;

			return {
				horizontal_has_travel ? std::clamp(local.X / viewport.Width, 0.0, 1.0) : 0.5,
				vertical_has_travel ? std::clamp(local.Y / viewport.Height, 0.0, 1.0) : 0.5
			};
		}

		static constexpr int navigator_dip = 160;

		// Sized for navigator_dip at the highest display scaling we support, so the overview stays sharp.
		static constexpr sizei navigator_surface_extent{512, 512};

		double effective_scale(const double fit) const noexcept
		{
			return is_fit() || _carried_fit ? fit : _scale;
		}

		bool is_magnified(const double fit) const noexcept
		{
			return effective_scale(fit) > fit + 0.000001;
		}

		void update_source(const sized source, const double fit) noexcept
		{
			// Exact != on doubles would call a source "changed" for a difference no pixel can show.
			const auto source_changed = _source.Width > 0.0 && _source.Height > 0.0 &&
				(!df::equiv(_source.Width, source.Width) || !df::equiv(_source.Height, source.Height));
			_source = source;

			if (_mode == zoom_scale_mode::explicit_scale && (source_changed || _carried_fit))
			{
				_carried_fit = _scale < fit;
			}
		}

		void update_fit_variant(const sized source, const sized viewport, const bool enlarge = false) noexcept
		{
			if (_mode == zoom_scale_mode::fit_width || _mode == zoom_scale_mode::fill)
			{
				// The scale follows the viewport; where the user panned to is theirs to keep. This
				// runs on every layout, so recentring here threw the pan away whenever the window
				// was resized or a panel opened or closed. Choosing Fit width or Fill still
				// recentres - that is what those commands do, and they say so themselves.
				_scale = fit_variant_scale(_mode, source, viewport, enlarge);
				_carried_fit = false;
			}
		}

		void fit() noexcept
		{
			if (!is_fit())
			{
				_last_explicit_center = _center;
				_restore_mode = _mode;
				_has_explicit = true;
			}
			_mode = zoom_scale_mode::fit;
			_center = {0.5, 0.5};
			_carried_fit = false;
		}

		void fit_width(const sized source, const sized viewport) noexcept
		{
			_mode = zoom_scale_mode::fit_width;
			_scale = fit_variant_scale(_mode, source, viewport, true);
			_center = {0.5, 0.5};
			_carried_fit = false;
		}

		void fill(const sized source, const sized viewport) noexcept
		{
			_mode = zoom_scale_mode::fill;
			_scale = fit_variant_scale(_mode, source, viewport, true);
			_center = {0.5, 0.5};
			_carried_fit = false;
		}

		void set_explicit(const double scale, const pointd center = {0.5, 0.5}) noexcept
		{
			_mode = zoom_scale_mode::explicit_scale;
			_scale = scale;
			_center = {std::clamp(center.X, 0.0, 1.0), std::clamp(center.Y, 0.0, 1.0)};
			_last_explicit_center = _center;
			_carried_fit = false;
			_has_explicit = true;
		}

		// Moves what is centred without answering the question of scale, so a sweep across a picture
		// cannot turn a chosen Fit width or Fill into an explicit scale that then stops re-fitting.
		void set_center(const pointd center) noexcept
		{
			_center = {std::clamp(center.X, 0.0, 1.0), std::clamp(center.Y, 0.0, 1.0)};
			if (_mode == zoom_scale_mode::explicit_scale) _last_explicit_center = _center;
		}

		void set_anchored(const double scale, const double old_scale, const sized source, const sized viewport,
		                  const pointd anchor) noexcept
		{
			if (source.Width <= 0.0 || source.Height <= 0.0 || old_scale <= 0.0 || scale <= 0.0) return;

			const pointd viewport_center{viewport.Width / 2.0, viewport.Height / 2.0};

			// The anchor has to be resolved against what is on screen, and geometry() draws from
			// the clamped centre. Pivoting about the raw one turned every zoom taken while the
			// centre sat outside its clamped range into a pivot about a point the user is not
			// looking at, and the picture slid out from under the pointer.
			const auto center = clamp_center(_center, source, viewport, old_scale);
			const auto source_anchor = pointd{
				center.X * source.Width + (anchor.X - viewport_center.X) / old_scale,
				center.Y * source.Height + (anchor.Y - viewport_center.Y) / old_scale
			};
			const auto new_center = pointd{
				(source_anchor.X - (anchor.X - viewport_center.X) / scale) / source.Width,
				(source_anchor.Y - (anchor.Y - viewport_center.Y) / scale) / source.Height
			};

			_mode = zoom_scale_mode::explicit_scale;
			_scale = scale;
			_center = {std::clamp(new_center.X, 0.0, 1.0), std::clamp(new_center.Y, 0.0, 1.0)};
			_last_explicit_center = _center;
			_carried_fit = false;
			_has_explicit = true;
		}

		pointd source_point_at(const sized source, const sized viewport, const double fit,
		                       const pointd anchor) const noexcept
		{
			const auto scale = effective_scale(fit);
			if (source.Width <= 0.0 || source.Height <= 0.0 || scale <= 0.0) return {};
			const pointd viewport_center{viewport.Width / 2.0, viewport.Height / 2.0};

			// Reads from the clamped centre for the same reason set_anchored writes to it: this
			// names the source point under a screen point, and what is under a screen point is
			// decided by what geometry() drew.
			const auto center = clamp_center(_center, source, viewport, scale);
			return {
				center.X * source.Width + (anchor.X - viewport_center.X) / scale,
				center.Y * source.Height + (anchor.Y - viewport_center.Y) / scale
			};
		}

		void center_source_point_at(const pointd source_point, const sized source, const sized viewport,
		                            const double fit, const pointd anchor) noexcept
		{
			const auto scale = effective_scale(fit);
			if (is_fit() || source.Width <= 0.0 || source.Height <= 0.0 || scale <= 0.0) return;
			const pointd viewport_center{viewport.Width / 2.0, viewport.Height / 2.0};
			_center = {
				std::clamp((source_point.X - (anchor.X - viewport_center.X) / scale) / source.Width, 0.0, 1.0),
				std::clamp((source_point.Y - (anchor.Y - viewport_center.Y) / scale) / source.Height, 0.0, 1.0)
			};
			_last_explicit_center = _center;
		}

		void toggle_fit() noexcept
		{
			if (is_fit() && _has_explicit)
			{
				// A Fit width or Fill comes back as itself; the next layout recomputes its scale.
				_mode = _restore_mode;
				_center = _last_explicit_center;
				_carried_fit = false;
			}
			else
			{
				fit();
			}
		}

		void pan_source(const pointd delta, const sized source, const sized viewport, const double fit) noexcept
		{
			const auto scale = effective_scale(fit);
			if (source.Width <= 0.0 || source.Height <= 0.0 || scale <= 0.0) return;
			const auto center = clamp_center(_center, source, viewport, scale);
			_center = clamp_center({center.X + delta.X / source.Width, center.Y + delta.Y / source.Height},
			                       source, viewport, scale);
			if (_mode == zoom_scale_mode::explicit_scale) _last_explicit_center = _center;
		}

		void zoom_region(const rectd region, const sized source, const sized viewport, const double fit) noexcept
		{
			if (region.Width <= 0.0 || region.Height <= 0.0 || source.Width <= 0.0 || source.Height <= 0.0)
				return;
			const auto first = source_point_at(source, viewport, fit, {region.X, region.Y});
			const auto last = source_point_at(source, viewport, fit, region.bottom_right());
			const auto left = std::clamp(std::min(first.X, last.X), 0.0, source.Width);
			const auto top = std::clamp(std::min(first.Y, last.Y), 0.0, source.Height);
			const auto right = std::clamp(std::max(first.X, last.X), 0.0, source.Width);
			const auto bottom = std::clamp(std::max(first.Y, last.Y), 0.0, source.Height);
			const auto width = right - left;
			const auto height = bottom - top;
			if (width <= 0.0 || height <= 0.0) return;

			const auto scale = std::clamp(std::min(viewport.Width / width, viewport.Height / height),
			                              std::min(0.05, fit), std::max(16.0, fit));
			set_explicit(scale, {(left + right) / (2.0 * source.Width), (top + bottom) / (2.0 * source.Height)});
		}

		void step(const int direction, const double fit, const sized source, const sized viewport,
		          const pointd anchor) noexcept
		{
			if (direction == 0) return;
			const auto current = effective_scale(fit);
			constexpr auto tolerance = 0.000001;

			if (direction < 0 && current <= fit + tolerance)
			{
				this->fit();
				return;
			}

			// The ladder is a sorted compile-time constant and fit is one extra stop, so the next
			// stop is a scan. Building, sorting and deduplicating a vector on every wheel notch
			// allocated on a path the user drives continuously.
			auto next = current;
			auto found = false;

			const auto consider = [&](const double value)
			{
				if (direction > 0)
				{
					if (value > current + tolerance && (!found || value < next))
					{
						next = value;
						found = true;
					}
				}
				else if (value < current - tolerance && (!found || value > next))
				{
					next = value;
					found = true;
				}
			};

			for (const auto value : scale_ladder) consider(value);
			consider(fit);

			if (!found) next = current;

			if (direction < 0 && current > fit && next <= fit + tolerance)
			{
				this->fit();
				return;
			}

			set_anchored(next, current, source, viewport, anchor);
		}

		zoom_geometry geometry(const sized source, const sized viewport, const double fit) const noexcept
		{
			const auto scale = effective_scale(fit);
			const auto center = clamp_center(_center, source, viewport, scale);
			const auto extent = source * scale;
			const pointd viewport_center{viewport.Width / 2.0, viewport.Height / 2.0};
			return {
				{viewport_center.X - center.X * extent.Width, viewport_center.Y - center.Y * extent.Height,
				 extent.Width, extent.Height},
				center
			};
		}
	};

	// A rectangle a user drew on the picture belongs to the picture, not to the window, so it is held
	// in source space for the same reason the zoom model keeps its centre there: it then survives a
	// resize, a layout change and a zoom without sliding off the subject. `image_bounds` is where the
	// whole picture lands on screen at the current scale, which is what both directions map through.
	inline rectd client_rect_to_source(const rectd client, const rectd image_bounds, const sized source) noexcept
	{
		if (image_bounds.Width <= 0.0 || image_bounds.Height <= 0.0) return {};

		const auto scale_x = source.Width / image_bounds.Width;
		const auto scale_y = source.Height / image_bounds.Height;

		// A drag runs in whichever direction the pointer went, so the corners are sorted before the
		// rectangle means anything.
		const auto client_left = std::min(client.left(), client.right());
		const auto client_top = std::min(client.top(), client.bottom());
		const auto client_right = std::max(client.left(), client.right());
		const auto client_bottom = std::max(client.top(), client.bottom());

		// Clamped to the picture: a rectangle reaching past the edge would name pixels that are not
		// there, and both the zoom and the crop it hands off to would have to clamp it again.
		const auto left = std::clamp((client_left - image_bounds.X) * scale_x, 0.0, source.Width);
		const auto top = std::clamp((client_top - image_bounds.Y) * scale_y, 0.0, source.Height);
		const auto right = std::clamp((client_right - image_bounds.X) * scale_x, 0.0, source.Width);
		const auto bottom = std::clamp((client_bottom - image_bounds.Y) * scale_y, 0.0, source.Height);

		return {left, top, right - left, bottom - top};
	}

	inline rectd source_rect_to_client(const rectd source_rect, const rectd image_bounds, const sized source) noexcept
	{
		if (source.Width <= 0.0 || source.Height <= 0.0) return {};

		const auto scale_x = image_bounds.Width / source.Width;
		const auto scale_y = image_bounds.Height / source.Height;

		return {
			image_bounds.X + source_rect.X * scale_x, image_bounds.Y + source_rect.Y * scale_y,
			source_rect.Width * scale_x, source_rect.Height * scale_y
		};
	}

	// Dragging inside the rectangle moves it, and it stops at the edge of the picture rather than
	// being carried off it.
	inline rectd offset_source_rect(const rectd source_rect, const pointd delta, const sized source) noexcept
	{
		const auto x = std::clamp(source_rect.X + delta.X, 0.0, std::max(0.0, source.Width - source_rect.Width));
		const auto y = std::clamp(source_rect.Y + delta.Y, 0.0, std::max(0.0, source.Height - source_rect.Height));
		return {x, y, source_rect.Width, source_rect.Height};
	}

	// A destination is laid out for the shape the item is, but the surface drawn into it is whatever
	// has arrived so far. A stand-in staged before the real decode need not be that shape: an
	// embedded thumbnail is stored verbatim, and cameras routinely write a padded 160x120 for a 3:2
	// or 16:9 frame. Stretching it into the destination distorts the picture until the decode lands.
	// Fitting it keeps the subject's shape. An aspect that already agrees is returned untouched, so
	// nothing letterboxes itself over a rounding difference.
	inline rectd fit_preserving_aspect(const rectd destination, const sized source) noexcept
	{
		if (source.Width <= 0.0 || source.Height <= 0.0 || destination.Width <= 0.0 || destination.Height <= 0.0)
		{
			return destination;
		}

		const auto scale = std::min(destination.Width / source.Width, destination.Height / source.Height);
		const auto width = source.Width * scale;
		const auto height = source.Height * scale;

		if (destination.Width - width < 1.0 && destination.Height - height < 1.0) return destination;

		return {
			destination.X + (destination.Width - width) / 2.0, destination.Y + (destination.Height - height) / 2.0,
			width, height
		};
	}

	// The two panes always show the same scale and center, so switching between them is a blink comparison.
	class comparison_zoom_state
	{
		zoom_view_state _primary;
		zoom_view_state _secondary;
		zoom_pane _active = zoom_pane::primary;

	public:
		zoom_pane active() const noexcept { return _active; }

		static zoom_pane other(const zoom_pane pane) noexcept
		{
			return pane == zoom_pane::primary ? zoom_pane::secondary : zoom_pane::primary;
		}

		void active(const zoom_pane pane) noexcept
		{
			if (pane == _active) return;
			inactive_state() = active_state();
			_active = pane;
		}

		zoom_view_state& state(const zoom_pane pane) noexcept
		{
			return pane == zoom_pane::primary ? _primary : _secondary;
		}

		const zoom_view_state& state(const zoom_pane pane) const noexcept
		{
			return pane == zoom_pane::primary ? _primary : _secondary;
		}

		zoom_view_state& active_state() noexcept { return state(_active); }
		const zoom_view_state& active_state() const noexcept { return state(_active); }

		template <class Mutator>
		void mutate(Mutator&& mutator)
		{
			mutator(active_state());
			inactive_state() = active_state();
		}

	private:
		zoom_view_state& inactive_state() noexcept
		{
			return state(other(_active));
		}
	};
}
