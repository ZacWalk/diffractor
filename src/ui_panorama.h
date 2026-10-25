// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The geometry of looking around inside an equirectangular panorama - the camera the user
// turns, the field-of-view ladder that replaces the zoom ladder while projected, and where a
// direction lands in the file's pixels. What patch of the sphere a file holds is the metadata
// layer's answer (prop::panorama_geometry). The software rasteriser declared here is implemented
// in render_panorama.cpp.

#pragma once

#include "model_property.h"
#include "libdesktop/ui.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//
// A file says which patch of the sphere it holds, in GPano's terms, and prop::panorama_geometry
// records it. Everything here works in that space, so a 360 x 60 degree strip and a complete
// sphere are the same object with different numbers.
//

// True when the pixels close the circle, which is what decides whether sampling wraps in longitude
// or stops at an edge. Within a texel of the full turn counts: writers round these numbers, and a
// file one pixel short of 360 is still seamless - sampled without wrapping it shows a hard line at
// the join.
inline bool panorama_wraps_longitude(const prop::panorama_geometry& g) noexcept
{
	return g.cropped_left <= 1 && g.cropped_width >= g.full_width - 2;
}

// The denominators come straight from the file. A zero one yields an infinity that the longitude
// wrap below spins on forever, so an undeclared extent reads as the whole sphere rather than as a
// hang - the geometry is rejected upstream, and this is what stops a future caller reaching it.
inline double panorama_longitude_at_left(const prop::panorama_geometry& g) noexcept
{
	if (g.full_width <= 0) return -M_PI;
	return g.cropped_left * 2.0 * M_PI / g.full_width - M_PI;
}

inline double panorama_longitude_span(const prop::panorama_geometry& g) noexcept
{
	if (g.full_width <= 0) return 2.0 * M_PI;
	return g.cropped_width * 2.0 * M_PI / g.full_width;
}

inline double panorama_latitude_at_top(const prop::panorama_geometry& g) noexcept
{
	if (g.full_height <= 0) return M_PI / 2.0;
	return M_PI / 2.0 - g.cropped_top * M_PI / g.full_height;
}

inline double panorama_latitude_span(const prop::panorama_geometry& g) noexcept
{
	if (g.full_height <= 0) return M_PI;
	return g.cropped_height * M_PI / g.full_height;
}

// Where the file's own pixels are centred on the sphere, which is where the camera starts: a
// half-sphere panorama must open looking at itself rather than at the empty side.
inline double panorama_center_longitude(const prop::panorama_geometry& g) noexcept
{
	return panorama_longitude_at_left(g) + panorama_longitude_span(g) / 2.0;
}

inline double panorama_center_latitude(const prop::panorama_geometry& g) noexcept
{
	return panorama_latitude_at_top(g) - panorama_latitude_span(g) / 2.0;
}

// The source texel a direction lands on, in the file's own pixels, or false when the direction
// points at sphere the file does not hold. Longitude is wrapped into the crop first, so a full
// circle has no seam to fall through.
inline bool panorama_texel_at(const prop::panorama_geometry& g, double longitude, const double latitude,
                              double& u, double& v) noexcept
{
	const auto left = panorama_longitude_at_left(g);

	while (longitude < left) longitude += 2.0 * M_PI;
	while (longitude >= left + 2.0 * M_PI) longitude -= 2.0 * M_PI;

	u = (longitude - left) * g.full_width / (2.0 * M_PI);
	v = (panorama_latitude_at_top(g) - latitude) * g.full_height / M_PI;

	return u >= 0.0 && u < g.cropped_width && v >= 0.0 && v < g.cropped_height;
}

//
// The camera, and the only three numbers the user changes: which way it faces and how much of the
// sphere it takes in. Scale has no meaning inside a sphere - there is no pixel to be one-to-one
// with - so the ladder is a field of view and the readout says degrees.
//

inline constexpr double panorama_default_fov_deg = 85.0;
inline constexpr double panorama_min_fov_deg = 9.0;
inline constexpr double panorama_max_fov_deg = 120.0;

// Coarse at the wide end where a step has to be large to be felt, fine at the narrow end where it
// does not. The widest stop is the floor: stepping out from it leaves the projection entirely,
// which is the same shape as zoom.md L2 stepping out to Fit.
inline constexpr std::array panorama_fov_ladder_deg{
	120.0, 100.0, 85.0, 70.0, 60.0, 50.0, 40.0, 32.0, 25.0, 20.0, 15.0, 12.0, 9.0
};

class panorama_view
{
	double _yaw = 0.0;
	double _pitch = 0.0;
	double _fov = panorama_default_fov_deg * M_PI / 180.0;

public:
	panorama_view() noexcept = default;

	double yaw() const noexcept { return _yaw; }
	double pitch() const noexcept { return _pitch; }
	double fov() const noexcept { return _fov; }

	double fov_degrees() const noexcept
	{
		return _fov * 180.0 / M_PI;
	}

	// Looking straight up and straight down are both reachable, and neither rolls past. Yaw is left
	// unwrapped in the setter and normalised here, so a long drag does not accumulate a value that
	// loses precision.
	void look_at(const double yaw, const double pitch) noexcept
	{
		_yaw = std::remainder(yaw, 2.0 * M_PI);
		_pitch = std::clamp(pitch, -M_PI / 2.0, M_PI / 2.0);
	}

	void fov(const double radians) noexcept
	{
		_fov = std::clamp(radians, panorama_min_fov_deg * M_PI / 180.0, panorama_max_fov_deg * M_PI / 180.0);
	}

	void reset(const prop::panorama_geometry& geometry) noexcept
	{
		look_at(panorama_center_longitude(geometry), panorama_center_latitude(geometry));
		fov(panorama_default_fov_deg * M_PI / 180.0);
	}

	// The next stop in or out. Returns false at the wide end, where there is no wider stop and the
	// caller leaves the projection rather than pretending the step happened.
	bool step_fov(const int direction) noexcept
	{
		if (direction == 0) return true;

		const auto current = fov_degrees();

		// The ladder descends, so stepping in takes the first stop below the current one and stepping
		// out walks it backwards to take the first stop above.
		if (direction > 0)
		{
			for (const auto stop : panorama_fov_ladder_deg)
			{
				if (stop < current - 0.01)
				{
					fov(stop * M_PI / 180.0);
					return true;
				}
			}

			return true; // Already at the narrowest stop: held there rather than stepped past.
		}

		for (auto i = panorama_fov_ladder_deg.size(); i-- > 0;)
		{
			if (panorama_fov_ladder_deg[i] > current + 0.01)
			{
				fov(panorama_fov_ladder_deg[i] * M_PI / 180.0);
				return true;
			}
		}

		return false;
	}

	// A drag turns the world under the pointer: crossing the viewport turns by one field of view,
	// whatever the field of view is, so the gesture keeps the same feel at every stop.
	void drag(const pointd client_delta, const sized viewport, const panorama_view& from) noexcept
	{
		if (viewport.Width <= 0.0 || viewport.Height <= 0.0) return;

		const auto radians_per_pixel = from._fov / viewport.Width;
		_fov = from._fov;
		look_at(from._yaw - client_delta.X * radians_per_pixel,
		        from._pitch + client_delta.Y * radians_per_pixel);
	}

	// Positional traversal, for a held button: the viewport is a map of the whole file and the
	// pointer picks the direction, which is zoom.md's inspect traversal applied to a sphere rather
	// than to a rectangle.
	void aim(const pointd local, const sized viewport, const prop::panorama_geometry& geometry) noexcept
	{
		if (viewport.Width <= 0.0 || viewport.Height <= 0.0) return;

		const auto x = std::clamp(local.X / viewport.Width, 0.0, 1.0);
		const auto y = std::clamp(local.Y / viewport.Height, 0.0, 1.0);

		look_at(panorama_longitude_at_left(geometry) + x * panorama_longitude_span(geometry),
		        panorama_latitude_at_top(geometry) - y * panorama_latitude_span(geometry));
	}

	// The camera's own trigonometry, which cannot change while one frame is drawn. The exact inverse
	// below is evaluated at least three times per segment, so recomputing these inside it would cost
	// more than the per-pixel inverse the segment scheme exists to avoid.
	struct camera_basis_t
	{
		double tan_half_fov = 0.0;
		double sin_pitch = 0.0;
		double cos_pitch = 1.0;
		double sin_yaw = 0.0;
		double cos_yaw = 1.0;
	};

	camera_basis_t camera_basis() const noexcept
	{
		return {std::tan(_fov / 2.0), std::sin(_pitch), std::cos(_pitch), std::sin(_yaw), std::cos(_yaw)};
	}

	// The direction a viewport pixel looks along, as a unit vector. Square pixels: both axes are
	// scaled by the same half-width, so the vertical field follows the viewport's shape instead of
	// stretching to fill it.
	void direction(const double x, const double y, const sized viewport, const camera_basis_t& camera,
	               double& out_x, double& out_y, double& out_z) const noexcept
	{
		const auto half = viewport.Width / 2.0;
		const auto cam_x = (x - half) / half * camera.tan_half_fov;
		const auto cam_y = -(y - viewport.Height / 2.0) / half * camera.tan_half_fov;

		// Pitch about X first, then yaw about Y: rolling the horizon is not something a viewer may
		// do to a picture the stitcher levelled.
		const auto pitched_y = cam_y * camera.cos_pitch + camera.sin_pitch;
		const auto pitched_z = -cam_y * camera.sin_pitch + camera.cos_pitch;

		const auto wx = cam_x * camera.cos_yaw + pitched_z * camera.sin_yaw;
		const auto wy = pitched_y;
		const auto wz = -cam_x * camera.sin_yaw + pitched_z * camera.cos_yaw;

		const auto length = std::sqrt(wx * wx + wy * wy + wz * wz);
		const auto scale = length > 1e-12 ? 1.0 / length : 0.0;

		out_x = wx * scale;
		out_y = wy * scale;
		out_z = wz * scale;
	}

	void direction(const double x, const double y, const sized viewport, double& out_x, double& out_y,
	               double& out_z) const noexcept
	{
		direction(x, y, viewport, camera_basis(), out_x, out_y, out_z);
	}

	// Which source texel a viewport pixel shows, or false where the file holds no sphere there.
	bool texel_at(const double x, const double y, const sized viewport, const prop::panorama_geometry& geometry,
	              double& u, double& v) const noexcept
	{
		double dx, dy, dz;
		direction(x, y, viewport, dx, dy, dz);

		return panorama_texel_at(geometry, std::atan2(dx, dz), std::asin(std::clamp(dy, -1.0, 1.0)), u, v);
	}

	// The patch of the file the current view covers, as a fraction of the file's own pixels on each
	// axis, for the navigator to draw. It is the field of view as a box, so a wide view whose edges
	// bow on the sphere is reported by its extent rather than by its outline. A view straddling the
	// file's own seam runs off the edge of the box rather than turning inside out, which is what a
	// pair of independently wrapped edges would do.
	rectd covered_source_fraction(const sized viewport, const prop::panorama_geometry& geometry) const noexcept
	{
		// The vertical field follows from the horizontal one through the tangent, not linearly: the
		// camera is a pinhole, so a box computed as fov * height / width is short of what is really on
		// screen, by a quarter at the widest stop.
		const auto vertical_fov = 2.0 * std::atan(std::tan(_fov / 2.0) * viewport.Height /
			std::max(1.0, viewport.Width));
		const auto longitude_span = std::max(1e-9, panorama_longitude_span(geometry));
		const auto latitude_span = std::max(1e-9, panorama_latitude_span(geometry));

		const auto center_x = (std::remainder(_yaw - panorama_center_longitude(geometry), 2.0 * M_PI) +
			longitude_span / 2.0) / longitude_span;
		const auto top = std::clamp(_pitch + vertical_fov / 2.0, -M_PI / 2.0, M_PI / 2.0);
		const auto bottom = std::clamp(_pitch - vertical_fov / 2.0, -M_PI / 2.0, M_PI / 2.0);
		const auto top_y = (panorama_latitude_at_top(geometry) - top) / latitude_span;
		const auto bottom_y = (panorama_latitude_at_top(geometry) - bottom) / latitude_span;

		const auto width = _fov / longitude_span;

		return {center_x - width / 2.0, top_y, width, bottom_y - top_y};
	}
};

// What a draw needs to know to project: whether it is projecting at all, which patch of the sphere
// the file holds, and where the camera is pointing. Passed by value, so nothing in the draw path
// reaches back into the state that produced it.
struct panorama_request
{
	bool active = false;
	prop::panorama_geometry geometry;
	panorama_view view;
};

// What a backend needs to draw the projection, derived in one place so a shader and the software
// rasteriser are never handed different cameras. The scales carry the file's coverage: a direction
// at the left of the file lands at u 0, and one a full turn later at `full width / cropped width`,
// so anything at or past 1 is sphere the file does not hold.
inline ui::panorama_params panorama_shader_params(const prop::panorama_geometry& geometry,
                                                  const panorama_view& view, const sized viewport) noexcept
{
	ui::panorama_params result;

	result.yaw = static_cast<float>(view.yaw());
	result.pitch = static_cast<float>(view.pitch());
	result.tan_half_fov = static_cast<float>(std::tan(view.fov() / 2.0));
	result.aspect = static_cast<float>(viewport.Width / std::max(1.0, viewport.Height));

	result.longitude_left = static_cast<float>(panorama_longitude_at_left(geometry));
	// A file that closes the circle covers all of it. Scaling by its declared crop instead would
	// leave the pixel or two a writer rounded away uncovered, and the join - the one place the
	// tolerance exists to make seamless - would show a hairline of background.
	result.u_scale = panorama_wraps_longitude(geometry)
		                 ? 1.0f
		                 : static_cast<float>(geometry.full_width /
		                 	static_cast<double>(std::max(1, geometry.cropped_width)));
	result.latitude_top = static_cast<float>(panorama_latitude_at_top(geometry));
	result.v_scale = static_cast<float>(geometry.full_height /
		static_cast<double>(std::max(1, geometry.cropped_height)));

	result.wraps = panorama_wraps_longitude(geometry);

	return result;
}

// Resamples an equirectangular source through the camera above, in software, into a surface the
// caller uploads as a texture - so both draw backends show the same pixels. The exact inverse is
// evaluated only at segment endpoints; reduced copies of the source cover a wide field of view,
// where one destination pixel spans many source texels.
class panorama_renderer
{
	std::vector<ui::const_surface_ptr> _levels;

public:
	void set_source(ui::const_surface_ptr source);
	bool is_ready() const { return !_levels.empty(); }
	sizei source_extent() const;
	bool render(ui::surface& destination, const prop::panorama_geometry& geometry, const panorama_view& view) const;
};
