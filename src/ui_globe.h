// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The globe's geometry - the orthographic projection the sidebar's and Explore's globes are
// drawn and targeted with, how a drag or a zoom moves the view, and where a collection deserves to be
// framed - and the map tiles Explore drapes over it. The software rasteriser declared here is
// implemented in render_globe.cpp.

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

// The deepest map tile level a globe reads: the tile store's own limit.
inline constexpr int globe_max_tile_level = 18;

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
		view_vector(gps_coordinate::deg2rad(coordinate.latitude()), gps_coordinate::deg2rad(coordinate.longitude()), x,
		            y, z);

		if (z <= globe_min_visible_z) return {};
		if (depth) *depth = z;

		return pointi{_center.x + core::round(x * _radius), _center.y - core::round(y * _radius)};
	}

	// As project, for a point already turned into its world unit vector - x toward 90 degrees east, y
	// north and z toward the meridian - so many points converted once are projected without trigonometry.
	std::optional<pointi> project_world(const std::array<double, 3>& world, double* depth = nullptr) const
	{
		const auto rx = world[0] * _cos_lon - world[2] * _sin_lon;
		const auto rz = world[0] * _sin_lon + world[2] * _cos_lon;
		const auto y = world[1] * _cos_lat - rz * _sin_lat;
		const auto z = world[1] * _sin_lat + rz * _cos_lat;

		if (z <= globe_min_visible_z) return {};
		if (depth) *depth = z;

		return pointi{_center.x + core::round(rx * _radius), _center.y - core::round(y * _radius)};
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

		if (!unproject_unit((loc.x - _center.x) / _radius, (_center.y - loc.y) / _radius, latitude_rad, longitude_rad))
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

// A coordinate as the world unit vector globe_projection::project_world takes.
inline std::array<double, 3> globe_world_vector(const gps_coordinate coordinate)
{
	const auto latitude = gps_coordinate::deg2rad(coordinate.latitude());
	const auto longitude = gps_coordinate::deg2rad(coordinate.longitude());
	const auto cos_latitude = std::cos(latitude);
	return {cos_latitude * std::sin(longitude), std::sin(latitude), cos_latitude * std::cos(longitude)};
}

// The bundled world map ships as flat-toned land over transparent water. Composited once over deep
// water, with the source's alpha as coverage so coastlines stay smooth, it is the map both globes wrap.
core::const_surface_ptr globe_composite_water(const core::const_surface_ptr& map);

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

// The view that puts a coordinate at a point of the disc, given as its offset from the centre in units
// of the radius, y up. It keeps a grabbed point under the pointer through a drag, and the point under
// the pointer where it was through a zoom. Two views can, and the one nearer `current` is answered. A
// view is upright - north is never past the top - so a point that no upright view can put there gets
// the nearest one that comes closest, and an offset off the disc, or a coordinate whose parallel cannot
// reach it, gets nothing.
inline std::optional<gps_coordinate> globe_view_placing(const gps_coordinate coordinate, const double unit_x,
                                                        const double unit_y, const gps_coordinate current)
{
	const auto r2 = unit_x * unit_x + unit_y * unit_y;
	if (!coordinate.is_valid() || r2 > 1.0) return {};

	const auto latitude = gps_coordinate::deg2rad(coordinate.latitude());
	const auto longitude = gps_coordinate::deg2rad(coordinate.longitude());
	const auto cos_latitude = std::cos(latitude);
	const auto world_y = std::sin(latitude);
	const auto unit_z = std::sqrt(std::max(0.0, 1.0 - r2));

	// How far right a point sits is cos(latitude) sin(longitude - view longitude), so the parallel has
	// to be wide enough to reach it.
	if (std::abs(unit_x) > cos_latitude + 1e-12) return {};

	const auto turn = std::asin(std::clamp(unit_x / std::max(cos_latitude, 1e-12), -1.0, 1.0));

	std::optional<gps_coordinate> best;
	auto best_reach = std::numeric_limits<double>::max();
	auto best_distance = std::numeric_limits<double>::max();

	for (const auto delta : {turn, M_PI - turn})
	{
		// Turning by the view latitude carries (world_y, depth) onto (unit_y, unit_z).
		const auto depth = cos_latitude * std::cos(delta);
		auto view_latitude = std::atan2(unit_z, unit_y) - std::atan2(depth, world_y);
		while (view_latitude > M_PI) view_latitude -= 2.0 * M_PI;
		while (view_latitude <= -M_PI) view_latitude += 2.0 * M_PI;

		// How far past a pole the view would have to tip; zero for every upright answer.
		const auto reach = std::max(0.0, std::abs(view_latitude) - M_PI / 2.0);
		const gps_coordinate candidate(gps_coordinate::rad2deg(std::clamp(view_latitude, -M_PI / 2.0, M_PI / 2.0)),
		                               globe_wrap_longitude(gps_coordinate::rad2deg(longitude - delta)));
		const auto distance = current.is_valid() ? current.distance_in_kilometers(candidate) : 0.0;

		if (reach < best_reach - 1e-12 || (std::abs(reach - best_reach) <= 1e-12 && distance < best_distance))
		{
			best = candidate;
			best_reach = reach;
			best_distance = distance;
		}
	}

	return best;
}

// The pixels around the equator at a radius, and the map tile level that holds as many: the level a
// view at that radius reads at its centre.
inline double globe_tile_level(const double radius)
{
	return std::log2(std::max(1.0, 2.0 * M_PI * radius / 256.0));
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

		return {gps_coordinate::rad2deg(std::asin(std::clamp(_y / length, -1.0, 1.0))),
		        globe_safe_longitude(gps_coordinate::rad2deg(std::atan2(_x, _z)))};
	}
};

// Map tiles a globe drapes over its sphere where they are loaded: Web Mercator tiles, addressed as the
// tile store addresses them. The renderer asks for the tile each part of the sphere needs and records
// that it wanted it, whether or not it was there, so its owner knows what to fetch for the next frame.
// It draws bands of rows side by side, so `tile` is asked from several threads at once; `want` is told
// afterwards, on the thread that called render, in the order the rows were drawn.
class globe_tile_source
{
public:
	virtual ~globe_tile_source() = default;

	// The deepest level worth reading; below zero, none.
	virtual int max_level() const = 0;

	// The decoded tile, or null while it is not loaded.
	virtual core::const_surface_ptr tile(int z, int x, int y) const = 0;

	virtual void want(int z, int x, int y) = 0;
};

// Wraps an equirectangular world map around the sphere, in software, into a surface the caller
// uploads as a texture - so both draw backends show the same pixels. The source is resampled every
// time the view moves, with the exact inverse evaluated only at segment endpoints; reduced copies
// of the source cover the compression toward the limb.
class globe_renderer
{
	std::vector<core::const_surface_ptr> _levels;

public:
	void set_source(const core::const_surface_ptr& source);
	bool is_ready() const { return !_levels.empty(); }
	bool render(core::surface& destination, const globe_projection& projection) const
	{
		return render(destination, projection, nullptr);
	}

	// With tiles, each part of the sphere shows the tile of the level its pixels need; while that one is on
	// its way, the tiles a level finer still held from before a zoom out, each over its quarter, and the
	// nearest loaded ancestor over the rest; and the world map where no tile has arrived or none exists -
	// past the Web Mercator latitudes, round the poles.
	bool render(core::surface& destination, const globe_projection& projection, globe_tile_source* tiles) const;
};
