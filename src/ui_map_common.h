// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Common map tile types, coordinate math, and rendering engine shared
// by map_view (full view) and map_control (dialog widget). Handles tile
// fetching (through the SQLite tile store and OSM tile-usage-policy compliance),
// caching, GPS math, panning, zooming, marker clustering, picked-marker highlight,
// and crosshair rendering.

#pragma once

#include "model_tile_cache.h"
#include "libdesktop/ui.h"
#include "libdesktop/util_kdtree.h"

// For M_PI on some compilers, otherwise define it manually.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// A standard tile size in pixels (OpenStreetMap tiles are 256x256)
constexpr int TILE_SIZE = 256;

// Represents a tile coordinate (slippy map tilenames).
struct map_tile_id
{
	int x;
	int y;
	int z; // Zoom level

	bool operator<(const map_tile_id& other) const
	{
		if (z != other.z) return z < other.z;
		if (x != other.x) return x < other.x;
		return y < other.y;
	}

	bool operator==(const map_tile_id& other) const
	{
		return x == other.x && y == other.y && z == other.z;
	}
};

// Hash specialization for map_tile_id to enable use with std::unordered_set
namespace std
{
	template <>
	struct hash<map_tile_id>
	{
		std::size_t operator()(const map_tile_id& id) const noexcept
		{
			const std::size_t h1 = std::hash<int>{}(id.x);
			const std::size_t h2 = std::hash<int>{}(id.y);
			const std::size_t h3 = std::hash<int>{}(id.z);

			return h1 ^ (h2 << 1) ^ (h3 << 2);
		}
	};
}

// The primary output struct, linking a tile to its drawing position.
struct map_tile
{
	map_tile_id coord;
	pointi screen_pos;
};

// Converts longitude to a tile's X coordinate at a given zoom level.
static double lon_to_tile_x(const double lon, const int zoom)
{
	return (lon + 180.0) / 360.0 * pow(2.0, zoom);
}

// Converts latitude to a tile's Y coordinate at a given zoom level.
static double lat_to_tile_y(const double lat, const int zoom)
{
	constexpr auto max_mercator_latitude = 85.05112878;
	const double lat_rad = std::clamp(lat, -max_mercator_latitude, max_mercator_latitude) * M_PI / 180.0;
	return (1.0 - asinh(tan(lat_rad)) / M_PI) / 2.0 * pow(2.0, zoom);
}

inline pointi map_marker_world_cell(const gps_coordinate coordinate, const int zoom, const int cell_size = 44)
{
	return {
		static_cast<int>(std::floor(lon_to_tile_x(coordinate.longitude(), zoom) * TILE_SIZE / cell_size)),
		static_cast<int>(std::floor(lat_to_tile_y(coordinate.latitude(), zoom) * TILE_SIZE / cell_size))
	};
}

inline std::vector<map_tile> get_tiles_for_view(const recti& bounds, const pointi scroll_offset,
                                                const gps_coordinate& center,
                                                const int zoom)
{
	std::vector<map_tile> tiles_to_draw;

	const double center_tile_x_f = lon_to_tile_x(center.longitude(), zoom);
	const double center_tile_y_f = lat_to_tile_y(center.latitude(), zoom);

	const int center_tile_x = static_cast<int>(floor(center_tile_x_f));
	const int center_tile_y = static_cast<int>(floor(center_tile_y_f));

	const double offset_x = center_tile_x_f - center_tile_x;
	const double offset_y = center_tile_y_f - center_tile_y;

	const int center_tile_screen_x = bounds.left + bounds.width() / 2 - static_cast<int>(offset_x * TILE_SIZE) +
		scroll_offset.x;
	const int center_tile_screen_y = bounds.top + bounds.height() / 2 - static_cast<int>(offset_y * TILE_SIZE) +
		scroll_offset.y;

	int start_tile_x = center_tile_x;
	int start_screen_x = center_tile_screen_x;
	while (start_screen_x > bounds.left)
	{
		start_screen_x -= TILE_SIZE;
		start_tile_x--;
	}

	int start_tile_y = center_tile_y;
	int start_screen_y = center_tile_screen_y;
	while (start_screen_y > bounds.top)
	{
		start_screen_y -= TILE_SIZE;
		start_tile_y--;
	}

	for (int y = start_tile_y, screen_y = start_screen_y; screen_y < bounds.bottom; ++y, screen_y += TILE_SIZE)
	{
		for (int x = start_tile_x, screen_x = start_screen_x; screen_x < bounds.right; ++x, screen_x += TILE_SIZE)
		{
			const int max_coord = static_cast<int>(pow(2.0, zoom));
			if (x >= 0 && x < max_coord && y >= 0 && y < max_coord)
			{
				map_tile tile_info;
				tile_info.coord = {x, y, zoom};
				tile_info.screen_pos = {screen_x, screen_y};
				tiles_to_draw.push_back(tile_info);
			}
		}
	}

	return tiles_to_draw;
}

inline std::string generate_tile_path(const map_tile_id& coord)
{
	std::ostringstream url;
	url << "/" << coord.z << "/" << coord.x << "/" << coord.y << ".png";
	return url.str();
}

// Canonical OpenStreetMap tile host. The OSM tile usage policy requires the
// exact hostname tile.openstreetmap.org; the legacy a/b/c subdomains "may be
// slower or withdrawn without notice".
constexpr std::string_view osm_tile_host = "tile.openstreetmap.org";

// Builds the User-Agent required by the OSM tile usage policy: it must clearly
// identify the application (name + version) and provide a contact URL. A generic
// or library-default User-Agent is blocked by the tile servers.
inline std::string tile_user_agent()
{
	return std::format("Diffractor/{} (+https://diffractor.com)", s_app_version);
}

// Tile levels the shared tile source actually serves. Anything outside is a blank map.
inline constexpr int map_min_zoom = 3;
inline constexpr int map_max_zoom = 18;

// A latitude/longitude box, used to frame a map on the region that actually holds items
// instead of an arbitrary default coordinate. Invalid until at least one point is added.
struct map_box
{
	double min_latitude = 0.0;
	double min_longitude = 0.0;
	double max_latitude = 0.0;
	double max_longitude = 0.0;
	bool valid = false;

	void add(const gps_coordinate coordinate)
	{
		if (!coordinate.is_valid()) return;

		if (!valid)
		{
			min_latitude = max_latitude = coordinate.latitude();
			min_longitude = max_longitude = coordinate.longitude();
			valid = true;
			return;
		}

		min_latitude = std::min(min_latitude, coordinate.latitude());
		max_latitude = std::max(max_latitude, coordinate.latitude());
		min_longitude = std::min(min_longitude, coordinate.longitude());
		max_longitude = std::max(max_longitude, coordinate.longitude());
	}

	gps_coordinate centre() const
	{
		return valid
			       ? gps_coordinate((min_latitude + max_latitude) / 2.0, (min_longitude + max_longitude) / 2.0)
			       : gps_coordinate{};
	}
};

// The closest zoom at which the whole box still fits in `extent`. A single point has no span,
// so it fits everywhere and yields the closest zoom; a box wider than the world yields the
// widest. Half a tile of slack on each side keeps the outermost markers clear of the edge,
// while never demanding more room than a small map actually has.
inline int map_fit_zoom(const map_box& box, const sizei extent)
{
	if (!box.valid || extent.cx <= 0 || extent.cy <= 0) return map_min_zoom;

	const auto fit_cx = std::max(TILE_SIZE / 2, extent.cx - TILE_SIZE);
	const auto fit_cy = std::max(TILE_SIZE / 2, extent.cy - TILE_SIZE);

	for (auto z = map_max_zoom; z > map_min_zoom; --z)
	{
		const auto cx = (lon_to_tile_x(box.max_longitude, z) - lon_to_tile_x(box.min_longitude, z)) * TILE_SIZE;
		const auto cy = (lat_to_tile_y(box.min_latitude, z) - lat_to_tile_y(box.max_latitude, z)) * TILE_SIZE;

		if (cx <= fit_cx && cy <= fit_cy) return z;
	}

	return map_min_zoom;
}

class map_engine
{
public:
	struct cache_entry
	{
		// Guards surface, in_view and requested — these are touched by both the UI
		// thread (render/fetch_tiles/cleanup) and the map worker thread (fetch_tile).
		platform::mutex mutex;
		ui::surface_ptr surface;
		bool in_view = false;
		bool requested = false; // a fetch task is queued/in-flight for this tile
	};

	using cache_entry_ptr = std::shared_ptr<cache_entry>;

	// A single aggregated marker (cluster of nearby item locations) in screen space.
	struct map_cluster
	{
		pointi screen_pos;
		int count = 0;
		uint32_t rep_index = 0; // index into the caller's marker array (see set_markers)
	};

	struct marker
	{
		gps_coordinate coordinate;
		uint32_t count = 1;
	};

	struct marker_snapshot
	{
		struct cell
		{
			gps_coordinate coordinate;
			int count = 0;
			uint32_t rep_index = UINT32_MAX;
		};

		int zoom = 0;
		std::vector<cell> cells;
		kd_points coords; // x=longitude, y=latitude, offset=cell index
		kd_tree tree;
		bool has_markers = false;
	};

private:
	async_strategy& _async;
	std::function<void()> _invalidate;

	// Shared with queued tile tasks so a task completing after this engine (and its
	// owning frame) is destroyed does not invoke the now-dangling invalidate callback.
	// Set false in the destructor, which runs on the UI thread (same thread as queue_ui).
	std::shared_ptr<std::atomic_bool> _alive = std::make_shared<std::atomic_bool>(true);

	int _zoom = 16;
	gps_coordinate _location;
	pointi _scroll_offset = {0, 0};
	pointi _temp_drag_offset = {0, 0};

	std::map<map_tile_id, cache_entry_ptr> _tile_cache;
	// A derived GPU cache rather than logical state, so it is shed on device loss from the const
	// broadcast the same way an element's texture is.
	mutable std::map<map_tile_id, ui::texture_ptr> _texture_cache;

	// Item-location markers, indexed spatially by worker-prepared snapshots so publication is a
	// bounded move on the UI thread.
	marker_snapshot _markers;
	std::vector<map_cluster> _clusters;
	bool _clusters_dirty = false;
	sizei _cluster_extent;
	size_t _last_cluster_work = 0;

	// The cluster the user picked, kept as a coordinate so it survives zooming and panning.
	gps_coordinate _selected;
	int _selected_count = 0;

	// The centre marker only means something where the centre is the answer.
	bool _show_crosshair = true;

public:
	map_engine(async_strategy& async, std::function<void()> invalidate)
		: _async(async), _invalidate(std::move(invalidate))
	{
	}

	static constexpr int min_zoom = map_min_zoom;
	static constexpr int max_zoom = map_max_zoom;

	~map_engine()
	{
		_alive->store(false);
		_tile_cache.clear();
		_texture_cache.clear();
	}

	const gps_coordinate& location() const { return _location; }
	int zoom_level() const { return _zoom; }

	void set_location(const gps_coordinate& loc, const recti& bounds)
	{
		if (_location != loc)
		{
			_location = loc;
			_scroll_offset = {};
			_texture_cache.clear();
			_clusters_dirty = true;
			fetch_tiles(bounds, _scroll_offset);
			_invalidate();
		}
	}

	gps_coordinate calc_center_gps() const
	{
		const auto total_offset = _scroll_offset + _temp_drag_offset;

		const double center_tile_x_f = lon_to_tile_x(_location.longitude(), _zoom);
		const double center_tile_y_f = lat_to_tile_y(_location.latitude(), _zoom);

		const double tile_offset_x = -static_cast<double>(total_offset.x) / TILE_SIZE;
		const double tile_offset_y = -static_cast<double>(total_offset.y) / TILE_SIZE;

		const double new_center_tile_x = center_tile_x_f + tile_offset_x;
		const double new_center_tile_y = center_tile_y_f + tile_offset_y;

		const double new_longitude = std::clamp(
			new_center_tile_x / pow(2.0, _zoom) * 360.0 - 180.0, -180.0, 180.0);

		const double n = M_PI - 2.0 * M_PI * new_center_tile_y / pow(2.0, _zoom);
		const double new_latitude = std::clamp(
			180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n))), -85.0, 85.0);

		return gps_coordinate(new_latitude, new_longitude);
	}

	void render(ui::draw_context& dc, const sizei& extent)
	{
		dc.clear(ui::color(dc.colors.background, 1.0f));

		const auto map_bounds = recti(extent);
		const auto total_offset = _scroll_offset + _temp_drag_offset;
		const auto tiles = get_tiles_for_view(map_bounds, total_offset, _location, _zoom);

		for (const auto& tile : tiles)
		{
			const auto tile_rect = recti(tile.screen_pos, sizei(TILE_SIZE, TILE_SIZE));
			auto found_texture = _texture_cache.find(tile.coord);

			if (found_texture != _texture_cache.end())
			{
				dc.draw_texture(found_texture->second, tile_rect);
				continue;
			}

			// Copy the surface shared_ptr under the entry lock; the worker thread may be
			// publishing it concurrently (a torn shared_ptr read would corrupt refcounts).
			ui::surface_ptr surf;
			const auto found_surface = _tile_cache.find(tile.coord);

			if (found_surface != _tile_cache.end())
			{
				platform::exclusive_lock lock(found_surface->second->mutex);
				surf = found_surface->second->surface;
			}

			bool drawn = false;

			if (surf)
			{
				const auto texture = dc.create_texture();

				if (texture && texture->update(surf) != ui::texture_update_result::failed)
				{
					_texture_cache[tile.coord] = texture;
					dc.draw_texture(texture, tile_rect);
					drawn = true;
				}
			}

			if (!drawn)
			{
				dc.draw_rect(tile_rect.inflate(-1), ui::color(0.5, 0.5, 0.5, 0.5));
			}
		}

		update_clusters_if_needed(extent);
		render_selection(dc, extent);
		render_markers(dc, extent);
		if (_show_crosshair) render_crosshair(dc, extent);
		render_attribution(dc, extent);
	}

	bool zoom(const int delta, const recti& bounds)
	{
		const int zoom_change = delta > 0 ? 1 : -1;
		const int new_zoom = _zoom + zoom_change;

		if (new_zoom >= min_zoom && new_zoom <= max_zoom)
		{
			_zoom = new_zoom;
			_texture_cache.clear();
			_scroll_offset = {0, 0};
			_clusters_dirty = true;
			fetch_tiles(bounds, _scroll_offset);
			_invalidate();
			return true;
		}

		return false;
	}

	// Frames the view on a latitude/longitude box: the box centre at the closest zoom that
	// still shows the whole box. A map that opens on the items is the difference between a
	// hot spot the user can click and an arbitrary street they have to zoom out of.
	bool fit_box(const map_box& box, const recti& bounds)
	{
		const auto extent = bounds.extent();
		const auto centre = box.centre();

		if (!box.valid || !centre.is_valid() || extent.cx <= 0 || extent.cy <= 0) return false;

		_zoom = map_fit_zoom(box, extent);
		_location = centre;
		_scroll_offset = {0, 0};
		_temp_drag_offset = {0, 0};
		_texture_cache.clear();
		_clusters_dirty = true;
		fetch_tiles(bounds, _scroll_offset);
		_invalidate();
		return true;
	}

	void pan_start()
	{
		_temp_drag_offset = {0, 0};
	}

	gps_coordinate pan(const pointi start_loc, const pointi current_loc, const recti& bounds)
	{
		_temp_drag_offset = current_loc - start_loc;
		_clusters_dirty = true;
		fetch_tiles(bounds.inflate(TILE_SIZE), _scroll_offset + _temp_drag_offset);
		_invalidate();
		return calc_center_gps();
	}

	gps_coordinate pan_end(const pointi start_loc, const pointi final_loc, const recti& bounds)
	{
		_scroll_offset = _scroll_offset + _temp_drag_offset;
		_temp_drag_offset = {0, 0};

		const auto gps = calc_center_gps();
		_location = gps;
		_scroll_offset = {};
		_texture_cache.clear();
		_clusters_dirty = true;
		fetch_tiles(bounds, _scroll_offset);
		_invalidate();
		return gps;
	}

	void clear_caches()
	{
		_tile_cache.clear();
		_texture_cache.clear();
	}

	// The tile textures belong to the lost device. Keeping the decoded tiles means the map redraws
	// from what is already in memory; keeping the textures would draw nothing at all, because the
	// cache hit returns before the placeholder the miss path draws.
	void free_graphics_resources() const
	{
		_texture_cache.clear();
	}

	// Provide the item-location markers to aggregate on the map. `markers` is indexed
	// by the caller; hit_test_marker returns the representative coordinate's index so
	// the caller can map a hovered cluster back to its own item array.
	static marker_snapshot prepare_marker_snapshot(const std::vector<marker>& markers, const int zoom)
	{
		marker_snapshot result;
		result.zoom = zoom;

		struct accum
		{
			double latitude = 0.0;
			double longitude = 0.0;
			int count = 0;
			uint32_t rep = UINT32_MAX;
		};

		std::map<std::pair<int, int>, accum> cells;

		for (uint32_t i = 0; i < markers.size(); ++i)
		{
			if (markers[i].coordinate.is_valid() && markers[i].count > 0)
			{
				const auto world_cell = map_marker_world_cell(markers[i].coordinate, zoom, cluster_cell_px);
				auto& cell = cells[std::make_pair(world_cell.x, world_cell.y)];
				cell.latitude += markers[i].coordinate.latitude() * markers[i].count;
				cell.longitude += markers[i].coordinate.longitude() * markers[i].count;
				cell.count += markers[i].count;
				cell.rep = std::min(cell.rep, i);
			}
		}

		result.cells.reserve(cells.size());

		for (const auto& [key, cell] : cells)
		{
			result.cells.push_back({
				gps_coordinate(cell.latitude / cell.count, cell.longitude / cell.count),
				cell.count, cell.rep
			});
		}

		result.has_markers = !result.cells.empty();

		result.coords.reserve(result.cells.size());
		for (auto i = 0u; i < result.cells.size(); ++i)
		{
			const auto& cell = result.cells[i];
			result.coords.emplace_back(static_cast<float>(cell.coordinate.longitude()),
			                           static_cast<float>(cell.coordinate.latitude()),
			                           i, 0, 0, 0.0f);
		}

		if (result.has_markers)
		{
			result.tree.build(result.coords);
		}

		return result;
	}

	static size_t prepared_marker_count(const marker_snapshot& markers)
	{
		return markers.cells.size();
	}

	size_t last_cluster_work() const
	{
		return _last_cluster_work;
	}

	void set_marker_snapshot(marker_snapshot markers)
	{
		_markers = std::move(markers);
		_clusters.clear();
		_clusters_dirty = true;
		_invalidate();
	}

	void set_markers(const std::vector<marker>& markers)
	{
		set_marker_snapshot(prepare_marker_snapshot(markers, _zoom));
	}

	// Marks the cluster the user picked. Purely presentational: it shows which bubble the
	// answer came from after the map has moved on.
	void set_selected(const gps_coordinate& loc, const int count)
	{
		_selected = loc;
		_selected_count = count;
		_invalidate();
	}

	void set_show_crosshair(const bool show)
	{
		if (_show_crosshair != show)
		{
			_show_crosshair = show;
			_invalidate();
		}
	}

	// The ground radius a cluster bubble covers at the current zoom, in kilometres. Web
	// Mercator resolves 156543.03392 m/px at the equator for zoom 0, scaled by cos(latitude).
	//
	// The whole cell diagonal, not half a cell width: a bubble sits at the weighted average of its
	// members rather than at the centre of its cell, so a member in the opposite corner is up to a
	// full diagonal away. Half a width left those members out of the search the bubble opened,
	// which is the one thing a cluster's radius promises not to do.
	double cluster_radius_km(const gps_coordinate& at) const
	{
		constexpr auto cell_diagonal_px = cluster_cell_px * 1.4142135623730951;
		const auto meters_per_pixel = 156543.03392 * std::cos(at.latitude() * M_PI / 180.0) / std::pow(2.0, _zoom);
		return meters_per_pixel * cell_diagonal_px / 1000.0;
	}

	std::vector<gps_coordinate> visible_cluster_coordinates(const sizei& extent)
	{
		update_clusters_if_needed(extent);

		std::vector<gps_coordinate> result;
		result.reserve(_clusters.size());

		for (const auto& cluster : _clusters)
		{
			if (cluster.screen_pos.x >= 0 && cluster.screen_pos.y >= 0 &&
				cluster.screen_pos.x < extent.cx && cluster.screen_pos.y < extent.cy)
			{
				result.emplace_back(gps_from_screen(cluster.screen_pos, extent));
			}
		}

		return result;
	}

	// The GPS coordinate at a screen point, so callers can recentre on a hit-tested cluster.
	gps_coordinate gps_at_screen(const pointi p, const sizei& extent) const
	{
		return gps_from_screen(p, extent);
	}

	// Returns the representative marker index of the cluster under `loc`, or -1 if
	// none. Fills the on-screen anchor and aggregated photo count.
	int hit_test_marker(const pointi loc, const sizei& extent, pointi& anchor_out, int& count_out) const
	{
		// Reverse order: render_markers draws them forwards, so the last one is on top and is the
		// one the user sees where two bubbles overlap.
		for (auto i = _clusters.rbegin(); i != _clusters.rend(); ++i)
		{
			const auto& c = *i;
			const int radius = cluster_radius(c.count);
			const auto dx = loc.x - c.screen_pos.x;
			const auto dy = loc.y - c.screen_pos.y;

			if (dx * dx + dy * dy <= radius * radius)
			{
				anchor_out = c.screen_pos;
				count_out = c.count;
				return static_cast<int>(c.rep_index);
			}
		}

		return -1;
	}

	void fetch_tiles_for_bounds(const recti& bounds)
	{
		fetch_tiles(bounds, _scroll_offset);
	}

private:
	// World-pixel size of the cell markers are aggregated into, and so the ground one bubble
	// stands for.
	static constexpr int cluster_cell_px = 44;

	// Marker radius grows gently with the number of aggregated photos.
	static int cluster_radius(const int count)
	{
		const int r = 10 + static_cast<int>(3.0 * std::log2(static_cast<double>(std::max(1, count)) + 1.0));
		return std::min(r, 28);
	}

	// Project a GPS coordinate to a screen point using the current view transform.
	pointi screen_from_gps(const gps_coordinate& g, const sizei& extent) const
	{
		const auto total_offset = _scroll_offset + _temp_drag_offset;
		const double dx = (lon_to_tile_x(g.longitude(), _zoom) - lon_to_tile_x(_location.longitude(), _zoom)) *
			TILE_SIZE;
		const double dy = (lat_to_tile_y(g.latitude(), _zoom) - lat_to_tile_y(_location.latitude(), _zoom)) *
			TILE_SIZE;

		return pointi(
			static_cast<int>(std::lround(extent.cx / 2.0 + dx + total_offset.x)),
			static_cast<int>(std::lround(extent.cy / 2.0 + dy + total_offset.y)));
	}

	// Inverse of screen_from_gps: the GPS coordinate at a screen point.
	gps_coordinate gps_from_screen(const pointi p, const sizei& extent) const
	{
		const auto total_offset = _scroll_offset + _temp_drag_offset;
		const double tile_x = lon_to_tile_x(_location.longitude(), _zoom) +
			(p.x - extent.cx / 2.0 - total_offset.x) / TILE_SIZE;
		const double tile_y = lat_to_tile_y(_location.latitude(), _zoom) +
			(p.y - extent.cy / 2.0 - total_offset.y) / TILE_SIZE;

		const double lon = tile_x / pow(2.0, _zoom) * 360.0 - 180.0;
		const double n = M_PI - 2.0 * M_PI * tile_y / pow(2.0, _zoom);
		const double lat = 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));

		return gps_coordinate(lat, lon);
	}

	void update_clusters_if_needed(const sizei& extent)
	{
		if (!_clusters_dirty && _cluster_extent == extent)
		{
			return;
		}

		_clusters_dirty = false;
		_cluster_extent = extent;
		recompute_clusters(extent);
	}

	// Cull and project the worker-prepared marker cells. Cell membership is already anchored to
	// the map at the snapshot zoom, so paint and hit testing do no source-marker aggregation.
	void recompute_clusters(const sizei& extent)
	{
		_clusters.clear();
		_last_cluster_work = 0;

		if (!_markers.has_markers || extent.is_empty() || _markers.tree.is_empty())
		{
			return;
		}

		constexpr int cell = cluster_cell_px;
		const auto tl = gps_from_screen({-cell, -cell}, extent);
		const auto br = gps_from_screen({extent.cx + cell, extent.cy + cell}, extent);
		const auto xmin = static_cast<float>(std::min(tl.longitude(), br.longitude()));
		const auto xmax = static_cast<float>(std::max(tl.longitude(), br.longitude()));
		const auto ymin = static_cast<float>(std::min(tl.latitude(), br.latitude()));
		const auto ymax = static_cast<float>(std::max(tl.latitude(), br.latitude()));

		std::vector<kd_coordinates_t> found;
		_markers.tree.find_in_bounds(_markers.coords, xmin, ymin, xmax, ymax, found);
		_last_cluster_work = found.size();
		_clusters.reserve(found.size());

		for (const auto& found_cell : found)
		{
			const auto& marker_cell = _markers.cells[found_cell.offset];
			const auto screen_pos = screen_from_gps(marker_cell.coordinate, extent);
			if (screen_pos.x < -cell || screen_pos.y < -cell ||
				screen_pos.x > extent.cx + cell || screen_pos.y > extent.cy + cell)
			{
				continue;
			}

			map_cluster mc;
			mc.screen_pos = screen_pos;
			mc.count = marker_cell.count;
			mc.rep_index = marker_cell.rep_index;
			_clusters.push_back(mc);
		}
	}

	// A halo behind the bubble the user picked. Drawn under the markers so the bubble keeps
	// its own shape, and anchored to the picked coordinate so it stays put as the map moves.
	void render_selection(ui::draw_context& dc, const sizei& extent) const
	{
		if (!_selected.is_valid()) return;

		const auto p = screen_from_gps(_selected, extent);
		const auto r = cluster_radius(_selected_count) + 7;

		if (p.x < -r || p.y < -r || p.x > extent.cx + r || p.y > extent.cy + r) return;

		const recti glow(p - pointi(r, r), sizei(r * 2, r * 2));
		dc.draw_rounded_rect(glow, ui::color(1.0f, 1.0f, 1.0f, 0.8f), r);
	}

	void render_markers(ui::draw_context& dc, const sizei& extent) const
	{
		for (const auto& c : _clusters)
		{
			const int radius = cluster_radius(c.count);
			const recti r(c.screen_pos - pointi(radius, radius), sizei(radius * 2, radius * 2));

			// Semi-transparent so the underlying map stays visible: a soft white halo
			// plus a translucent accent fill, with the aggregated count on top.
			const recti halo = r.inflate(1);
			dc.draw_rounded_rect(halo, ui::color(1.0f, 1.0f, 1.0f, 0.35f), radius + 1);
			dc.draw_rounded_rect(r, ui::color(ui::style::color::dialog_selected_background, 0.5f), radius);

			if (c.count > 1)
			{
				dc.draw_text(std::to_string(c.count), r, ui::style::font_face::dialog,
				             ui::style::text_style::single_line_center, ui::color(1.0f, 1.0f, 1.0f, 0.9f), {});
			}
		}
	}

	// The OSM tile usage policy requires visible licence attribution on the map.
	// Draw "© OpenStreetMap contributors" in the bottom-right corner with a
	// translucent backing so it stays legible over any tile.
	void render_attribution(ui::draw_context& dc, const sizei& extent) const
	{
		constexpr std::string_view attribution = "© OpenStreetMap contributors";
		constexpr int pad = 4;

		const auto text_extent = dc.measure_text(attribution, ui::style::font_face::dialog,
		                                         ui::style::text_style::single_line, extent.cx);

		const int w = text_extent.cx + pad * 2;
		const int h = text_extent.cy + pad;

		const recti bg_rect(std::max(0, extent.cx - w), std::max(0, extent.cy - h), extent.cx, extent.cy);
		dc.draw_rect(bg_rect, ui::color(0.0f, 0.0f, 0.0f, 0.4f));

		const recti text_rect(bg_rect.left + pad, bg_rect.top, bg_rect.right - pad, bg_rect.bottom);
		dc.draw_text(attribution, text_rect, ui::style::font_face::dialog, ui::style::text_style::single_line,
		             ui::color(1.0f, 1.0f, 1.0f, 0.9f), {});
	}

	// A plain cross, semi-transparent: two unbroken lines whose intersection is the coordinate
	// that will be written. No ring and no centre disc - either would read as one more cluster
	// bubble, and either would hide the bubble the user has just centred on.
	void render_crosshair(ui::draw_context& dc, const sizei& extent) const
	{
		const int center_x = extent.cx / 2;
		const int center_y = extent.cy / 2;

		constexpr int arm = 48;
		constexpr int line_width = 3;
		constexpr int half = line_width / 2;

		const auto crosshair_color = ui::color(ui::style::color::important_background, 0.6f);
		// A thin lighter underlay keeps the cross readable over both dark and pale tiles.
		const auto outline_color = ui::color(1.0f, 1.0f, 1.0f, 0.35f);

		const recti horizontal(center_x - arm, center_y - half, center_x + arm, center_y - half + line_width);
		dc.draw_rect(horizontal.inflate(0, 1), outline_color);
		dc.draw_rect(horizontal, crosshair_color);

		const recti vertical(center_x - half, center_y - arm, center_x - half + line_width, center_y + arm);
		dc.draw_rect(vertical.inflate(1, 0), outline_color);
		dc.draw_rect(vertical, crosshair_color);
	}

	// Decodes and hands the result to the UI, then releases the request claim. Every path through
	// the fetch below ends here exactly once, so a tile can always be asked for again.
	static void publish_tile(async_strategy& async, const cache_entry_ptr& e,
	                         const std::function<void()>& invalidate,
	                         const std::shared_ptr<std::atomic_bool>& alive, const df::blob& data)
	{
		ui::surface_ptr surface;

		if (!data.empty())
		{
			files ff;
			surface = ff.image_to_surface(df::cspan(data.data(), data.size()));
		}

		bool publish = false;
		{
			platform::exclusive_lock lock(e->mutex);
			e->requested = false;

			if (surface && e->in_view && !e->surface)
			{
				e->surface = std::move(surface);
				publish = true;
			}
		}

		if (publish)
		{
			async.queue_ui(
				[invalidate, alive]
				{
					if (alive->load())
					{
						invalidate();
					}
				});
		}
	}

	static void download_tile(async_strategy& async, const cache_entry_ptr& e,
	                          const std::function<void()>& invalidate,
	                          const std::shared_ptr<std::atomic_bool>& alive, const map_tile_id& coord,
	                          const int64_t key)
	{
		// Fetch from the canonical OSM host with a compliant User-Agent.
		platform::web_request req;
		req.path = generate_tile_path(coord);
		req.headers.emplace_back("User-Agent", tile_user_agent());

		thread_local platform::web_host_ptr s_osm_con;
		if (!s_osm_con)
		{
			s_osm_con = platform::connect_to_host(osm_tile_host);
		}

		const auto response = send_request(s_osm_con, req);
		const auto data = std::make_shared<df::blob>();

		if (response.status_code == 200 && !response.body.empty())
		{
			data->assign(response.body.begin(), response.body.end());

			async.queue_tile_db([key, data](tile_cache_db& db)
			{
				db.store(key, df::cspan(data->data(), data->size()));
			});
		}
		else
		{
			// Hard failure: drop the shared connection so the next task reconnects rather than
			// reusing a broken handle forever.
			s_osm_con.reset();
		}

		publish_tile(async, e, invalidate, alive, *data);
	}

	void fetch_tile(const cache_entry_ptr& e, const map_tile_id& coord)
	{
		// A shared liveness flag keeps every hop safe if this engine is destroyed while the request
		// is still in the pipeline.
		const auto key = map_tile_db_key(coord.z, coord.x, coord.y);

		_async.queue_tile_db(
			[&async = _async, coord, key, e, invalidate = _invalidate, alive = _alive](tile_cache_db& db)
			{
				{
					platform::exclusive_lock lock(e->mutex);
					if (!e->in_view || e->surface)
					{
						e->requested = false;
						return;
					}
				}

				// Decoding a PNG on this thread would stall every following lookup, and downloading on
				// it would stall them for a network round trip, so both hand back to the map workers.
				auto data = std::make_shared<df::blob>(db.load(key));

				if (!data->empty())
				{
					async.queue_async(async_queue::map_tile,
					                  [&async, e, invalidate, alive, data]
					                  {
						                  publish_tile(async, e, invalidate, alive, *data);
					                  });
					return;
				}

				async.queue_async(async_queue::map_tile,
				                  [&async, coord, key, e, invalidate, alive]
				                  {
					                  download_tile(async, e, invalidate, alive, coord, key);
				                  });
			});
	}

	void fetch_tiles(const recti& bounds, const pointi scroll_offset)
	{
		const auto tiles = get_tiles_for_view(bounds, scroll_offset, _location, _zoom);
		std::unordered_set<map_tile_id> visible_tiles;
		visible_tiles.reserve(tiles.size());
		for (const auto& tile : tiles)
		{
			visible_tiles.insert(tile.coord);
		}

		// Update visibility and eagerly release GPU textures for tiles that scrolled
		// out of view so the texture cache does not balloon while panning.
		for (auto& [coord, entry] : _tile_cache)
		{
			const bool vis = visible_tiles.contains(coord);
			{
				platform::exclusive_lock lock(entry->mutex);
				entry->in_view = vis;
			}
			if (!vis)
			{
				_texture_cache.erase(coord);
			}
		}

		for (const auto& tile : tiles)
		{
			auto found = _tile_cache.find(tile.coord);

			if (found == _tile_cache.end())
			{
				auto e = std::make_shared<cache_entry>();
				e->in_view = true;
				found = _tile_cache.emplace(tile.coord, e).first;
			}

			const auto& entry = found->second;
			bool need_fetch = false;
			{
				platform::exclusive_lock lock(entry->mutex);
				entry->in_view = true;

				// Only enqueue when there is no surface and no task already in flight,
				// otherwise rapid mouse-move pans pile up duplicate requests on the
				// single serial map queue.
				if (!entry->surface && !entry->requested)
				{
					entry->requested = true;
					need_fetch = true;
				}
			}

			if (need_fetch)
			{
				fetch_tile(entry, tile.coord);
			}
		}

		cleanup_cache();
	}

	void cleanup_cache()
	{
		constexpr size_t max_cache_size = 1000;

		if (_tile_cache.size() > max_cache_size)
		{
			auto it = _tile_cache.begin();
			while (it != _tile_cache.end() && _tile_cache.size() > max_cache_size)
			{
				bool vis;
				{
					platform::exclusive_lock lock(it->second->mutex);
					vis = it->second->in_view;
				}

				if (!vis)
				{
					_texture_cache.erase(it->first);
					it = _tile_cache.erase(it);
				}
				else
				{
					++it;
				}
			}
		}
	}
};
