// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The sidebar globe's geometry - the orthographic projection the globe is drawn and
// targeted with, how a drag moves the view, and where a collection deserves to be framed.
// The software rasteriser declared here is implemented in render_globe.cpp.

#pragma once

#include "model_location.h"
#include "libdesktop/ui.h"

// For M_PI on some compilers, otherwise define it manually.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//
// The same equirectangular world map the flat sidebar map used, wrapped around a sphere and drawn
// orthographically. The view is one coordinate - the lat/lon facing the viewer - and the user drags
// it in both axes, so nothing here assumes rotation about the polar axis alone.
//

// How far inside the limb a point must sit to count as facing the viewer, as the view-space z of
// its unit vector. Anything closer to the edge is a sliver the user cannot aim at.
inline constexpr double globe_min_visible_z = 0.10;

// Degrees the view turns per pixel of drag, at radius R: a drag across the full diameter turns
// the globe half way round.
inline constexpr double globe_degrees_per_radius = 90.0;

// The view rotation, with its sines and cosines taken once. `project` answers where a coordinate
// lands and whether it faces the viewer; `unproject` answers which coordinate a pixel shows.
class globe_projection
{
	pointi _center;
	double _radius = 0.0;
	double _sin_lat = 0.0;
	double _cos_lat = 1.0;
	double _sin_lon = 0.0;
	double _cos_lon = 1.0;

public:
	globe_projection() = default;

	// An unset view faces (0, 0). Note that gps_coordinate spends exactly +-180 longitude as its
	// "no coordinate" sentinel, so a caller that computes a view must keep it off that value with
	// globe_safe_longitude or it silently gets the meridian instead.
	globe_projection(const gps_coordinate view, const pointi center, const double radius) noexcept :
		_center(center), _radius(radius)
	{
		const auto lat = gps_coordinate::deg2rad(view.is_valid() ? view.latitude() : 0.0);
		const auto lon = gps_coordinate::deg2rad(view.is_valid() ? view.longitude() : 0.0);
		_sin_lat = std::sin(lat);
		_cos_lat = std::cos(lat);
		_sin_lon = std::sin(lon);
		_cos_lon = std::cos(lon);
	}

	pointi center() const { return _center; }
	double radius() const { return _radius; }

	// View-space unit vector of a coordinate. z is positive toward the viewer, y up, x right.
	void view_vector(const double latitude_rad, const double longitude_rad, double& x, double& y, double& z) const
	{
		const auto cos_p = std::cos(latitude_rad);
		const auto wx = cos_p * std::sin(longitude_rad);
		const auto wy = std::sin(latitude_rad);
		const auto wz = cos_p * std::cos(longitude_rad);

		const auto rx = wx * _cos_lon - wz * _sin_lon;
		const auto rz = wx * _sin_lon + wz * _cos_lon;

		x = rx;
		y = wy * _cos_lat - rz * _sin_lat;
		z = wy * _sin_lat + rz * _cos_lat;
	}

	// The pixel a coordinate lands on, or nothing when it faces away. `depth` is the view-space z,
	// which the caller uses to foreshorten what it draws there.
	std::optional<pointi> project(const gps_coordinate coordinate, double* depth = nullptr) const
	{
		if (!coordinate.is_valid()) return {};

		double x, y, z;
		view_vector(gps_coordinate::deg2rad(coordinate.latitude()), gps_coordinate::deg2rad(coordinate.longitude()),
		            x, y, z);

		if (z <= globe_min_visible_z) return {};
		if (depth) *depth = z;

		return pointi{
			_center.x + df::round(x * _radius),
			_center.y - df::round(y * _radius)
		};
	}

	// The rasteriser's inner call: pixel offset from the centre, in units of the radius, to
	// lat/lon in radians. False outside the disc.
	bool unproject_unit(const double x, const double y, double& latitude_rad, double& longitude_rad) const
	{
		const auto r2 = x * x + y * y;
		if (r2 > 1.0) return false;

		const auto z = std::sqrt(std::max(0.0, 1.0 - r2));

		// Undo the latitude rotation, then the longitude one; x is untouched by the first.
		const auto wy = y * _cos_lat + z * _sin_lat;
		const auto rz = z * _cos_lat - y * _sin_lat;
		const auto wx = x * _cos_lon + rz * _sin_lon;
		const auto wz = rz * _cos_lon - x * _sin_lon;

		latitude_rad = std::asin(std::clamp(wy, -1.0, 1.0));
		longitude_rad = std::atan2(wx, wz);
		return true;
	}

	// The coordinate a pixel shows, or nothing when the pixel is off the sphere.
	std::optional<gps_coordinate> unproject(const pointi loc) const
	{
		if (_radius <= 0.0) return {};

		double latitude_rad, longitude_rad;

		if (!unproject_unit((loc.x - _center.x) / _radius, (_center.y - loc.y) / _radius, latitude_rad,
		                    longitude_rad))
		{
			return {};
		}

		return gps_coordinate(gps_coordinate::rad2deg(latitude_rad), gps_coordinate::rad2deg(longitude_rad));
	}
};

// gps_coordinate spends +-180 as its "no coordinate" sentinel, so a view that lands exactly on the
// date line has to be expressed as the nearest longitude that is not that sentinel. Without this a
// drag through the date line would read as an invalid view and snap the globe to the meridian.
inline double globe_safe_longitude(const double degrees)
{
	if (degrees >= gps_coordinate::invalid_coordinate) return std::nextafter(180.0, 0.0);
	if (degrees <= -gps_coordinate::invalid_coordinate) return std::nextafter(-180.0, 0.0);
	return degrees;
}

// Longitude folded back into (-180, 180) so a view that keeps turning never accumulates a jump.
inline double globe_wrap_longitude(double degrees)
{
	degrees = std::fmod(degrees + 180.0, 360.0);
	if (degrees < 0.0) degrees += 360.0;
	return globe_safe_longitude(degrees - 180.0);
}

// Where a drag leaves the view. Dragging right turns the globe west; dragging down brings the
// north toward the centre, which is what grabbing a ball and pulling it downward does.
inline gps_coordinate globe_view_from_drag(const gps_coordinate view, const pointi drag, const double radius)
{
	if (radius <= 0.0) return view;

	const auto degrees_per_pixel = globe_degrees_per_radius / radius;
	const auto latitude = std::clamp(view.latitude() + drag.y * degrees_per_pixel, -90.0, 90.0);
	const auto longitude = globe_wrap_longitude(view.longitude() - drag.x * degrees_per_pixel);
	return {latitude, longitude};
}

// The view a collection deserves on first sight: the count-weighted mean of its places as unit
// vectors. One operation covers both axes and the antimeridian, so a collection either side of
// the date line faces the date line rather than the Atlantic.
class globe_framer
{
	double _x = 0.0;
	double _y = 0.0;
	double _z = 0.0;

public:
	void add(const gps_coordinate coordinate, const double weight)
	{
		if (!coordinate.is_valid() || weight <= 0.0) return;

		const auto lat = gps_coordinate::deg2rad(coordinate.latitude());
		const auto lon = gps_coordinate::deg2rad(coordinate.longitude());
		const auto cos_lat = std::cos(lat);

		_x += weight * cos_lat * std::sin(lon);
		_y += weight * std::sin(lat);
		_z += weight * cos_lat * std::cos(lon);
	}

	// Invalid when nothing was added, or when the places cancel out and no direction is more
	// deserved than any other.
	gps_coordinate view() const
	{
		const auto length = std::sqrt(_x * _x + _y * _y + _z * _z);
		if (length < 1e-9) return {};

		return {
			gps_coordinate::rad2deg(std::asin(std::clamp(_y / length, -1.0, 1.0))),
			globe_safe_longitude(gps_coordinate::rad2deg(std::atan2(_x, _z)))
		};
	}
};

// Wraps an equirectangular world map around the sphere, in software, into a surface the caller
// uploads as a texture - so both draw backends show the same pixels. The source is resampled every
// time the view moves, with the exact inverse evaluated only at segment endpoints; reduced copies
// of the source cover the compression toward the limb.
class globe_renderer
{
	std::vector<ui::const_surface_ptr> _levels;

public:
	void set_source(ui::const_surface_ptr source);
	bool is_ready() const { return !_levels.empty(); }
	bool render(ui::surface& destination, const globe_projection& projection) const;
};
