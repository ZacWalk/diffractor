// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Common map tile types, coordinate math, the tile cache, and the flat map's rendering
// engine. The tile cache fetches tiles through the SQLite tile store, following the OSM tile usage
// policy, for the Locate map (map_view) and the Explore globe; the engine handles the flat map's
// panning, zooming, marker clustering, picked-marker highlight, and crosshair rendering.

#pragma once

#include "model_tile_cache.h"
#include "libdesktop/ui.h"
#include "libdesktop/core/kdtree.h"

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

	bool operator==(const map_tile_id& other) const { return x == other.x && y == other.y && z == other.z; }
};

// Hash specialization for map_tile_id to enable use with std::unordered_set
namespace std
{
	template <> struct hash<map_tile_id>
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
	return {static_cast<int>(std::floor(lon_to_tile_x(coordinate.longitude(), zoom) * TILE_SIZE / cell_size)),
	        static_cast<int>(std::floor(lat_to_tile_y(coordinate.latitude(), zoom) * TILE_SIZE / cell_size))};
}

// Tiles in the order a map fills in, as map viewers do: nearest the point the view centres on first. Measured
// across the world rather than in tiles, so the coarse tiles round a globe's rim wait behind the fine ones in
// its middle; and the shorter way round, so the date line is no wall.
inline void order_tiles_from(std::vector<map_tile_id>& tiles, const gps_coordinate centre)
{
	const auto cx = lon_to_tile_x(centre.longitude(), 0);
	const auto cy = lat_to_tile_y(centre.latitude(), 0);

	std::vector<std::pair<double, map_tile_id>> keyed;
	keyed.reserve(tiles.size());

	for (const auto& t : tiles)
	{
		const auto size = 1.0 / static_cast<double>(1 << t.z);
		auto dx = std::abs((t.x + 0.5) * size - cx);
		dx = std::min(dx, 1.0 - dx);
		const auto dy = (t.y + 0.5) * size - cy;
		keyed.emplace_back(dx * dx + dy * dy, t);
	}

	std::ranges::stable_sort(keyed, {}, &std::pair<double, map_tile_id>::first);
	for (size_t i = 0; i < keyed.size(); ++i) tiles[i] = keyed[i].second;
}

inline std::vector<map_tile> get_tiles_for_view(const recti& bounds, const pointi scroll_offset,
                                                const gps_coordinate& center, const int zoom)
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

// The OSM tile usage policy requires visible licence attribution wherever its tiles are shown:
// "© OpenStreetMap contributors" in the bottom-right corner, over a translucent backing so it stays
// legible over any tile.
inline void draw_map_attribution(render::draw_context& dc, const recti bounds)
{
	constexpr std::string_view attribution = "© OpenStreetMap contributors";
	constexpr int pad = 4;

	const auto text_extent = dc.measure_text(attribution, render::font_face::dialog, render::text_style::single_line,
	                                         bounds.width());

	const int w = text_extent.cx + pad * 2;
	const int h = text_extent.cy + pad;

	const recti bg_rect(std::max(bounds.left, bounds.right - w), std::max(bounds.top, bounds.bottom - h), bounds.right,
	                    bounds.bottom);
	dc.draw_rect(bg_rect, core::color(0.0f, 0.0f, 0.0f, 0.4f));

	const recti text_rect(bg_rect.left + pad, bg_rect.top, bg_rect.right - pad, bg_rect.bottom);
	dc.draw_text(attribution, text_rect, render::font_face::dialog, render::text_style::single_line,
	             core::color(1.0f, 1.0f, 1.0f, 0.9f), {});
}

// Decoded map tiles, keyed by address and fetched as a view shows them: from the tile store when it holds
// them, otherwise from OpenStreetMap, which the store then keeps. The flat map and the Explore globe each
// hold one. A tile task that completes after its cache is gone reaches nothing, because every task holds
// the cache's liveness flag rather than the cache.
//
// Only what the view shows now is fetched, the most wanted first. A tile the view has moved away from is let
// go wherever it waits - for the store, for the network or for its decode - so after a zoom or a turn the tiles
// on screen never wait behind ones that were. Two tiles download at once, and a tile the store holds is decoded
// meanwhile by another map worker rather than after them.
class map_tile_cache
{
public:
	// Fetches a tile the store does not hold: OpenStreetMap's, unless a test stands in for it. Empty when it
	// could not be had.
	using downloader = std::function<core::blob(const map_tile_id&)>;

	static constexpr int max_downloads = map_tile_downloads;

	// A tile that could not be had is not asked for again until this has passed, so a server that refuses, or a
	// network that is down, is not asked again each time the view is drawn.
	static constexpr int64_t retry_after_ms = 10'000;

private:
	struct entry
	{
		// Guards everything below: the UI thread shows and evicts, the map workers publish.
		mutex mutex;
		core::surface_ptr surface;
		bool in_view = false;
		bool requested = false; // a fetch is queued or in flight for this tile
		// Its place among the tiles the view last showed, the most wanted first.
		uint32_t rank = 0;
		// When it may be asked for again, after a fetch that failed (core::now_ms).
		int64_t retry_ms = 0;
	};

	using entry_ptr = std::shared_ptr<entry>;

	// The tiles waiting for the network, and how many workers are downloading them. A worker that is free takes
	// the most wanted tile still shown, and lets go of any the view moved away from.
	struct download_queue
	{
		mutex mutex;
		std::vector<std::pair<map_tile_id, entry_ptr>> waiting;
		int running = 0;
		downloader download = download_from_osm;
	};

	using download_queue_ptr = std::shared_ptr<download_queue>;

	async_strategy& _async;
	std::function<void()> _invalidate;
	size_t _capacity;

	// Set false in the destructor, which runs on the UI thread, the thread every publish lands on.
	std::shared_ptr<std::atomic_bool> _alive = std::make_shared<std::atomic_bool>(true);
	download_queue_ptr _downloads = std::make_shared<download_queue>();
	std::map<map_tile_id, entry_ptr> _entries;

public:
	map_tile_cache(async_strategy& async, std::function<void()> invalidate, const size_t capacity = 1000) :
		_async(async), _invalidate(std::move(invalidate)), _capacity(capacity)
	{
	}

	~map_tile_cache()
	{
		_alive->store(false);
		clear();
	}

	map_tile_cache(const map_tile_cache&) = delete;
	map_tile_cache& operator=(const map_tile_cache&) = delete;

	// For tests, which must not reach the network.
	void download_with(downloader download)
	{
		core::exclusive_lock lock(_downloads->mutex);
		_downloads->download = std::move(download);
	}

	// The tiles shown now, the most wanted first. Each is fetched unless it is loaded, already on its way, or
	// failed a moment ago; every other tile is no longer fetched, and is free to go once the cache holds more
	// than its capacity.
	void show(const std::span<const map_tile_id> tiles)
	{
		// A tile listed twice keeps its first place.
		std::unordered_map<map_tile_id, uint32_t> ranks;
		ranks.reserve(tiles.size());
		for (const auto& coord : tiles) ranks.try_emplace(coord, static_cast<uint32_t>(ranks.size()));

		for (auto& [coord, e] : _entries)
		{
			const auto found = ranks.find(coord);
			core::exclusive_lock lock(e->mutex);
			e->in_view = found != ranks.end();
			if (e->in_view) e->rank = found->second;
		}

		const auto now = core::now_ms();

		// In order, so the store is asked for the most wanted first.
		for (const auto& coord : tiles)
		{
			auto found = _entries.find(coord);
			if (found == _entries.end()) found = _entries.emplace(coord, std::make_shared<entry>()).first;

			const auto& e = found->second;
			bool need_fetch = false;
			{
				core::exclusive_lock lock(e->mutex);
				e->in_view = true;
				e->rank = ranks.find(coord)->second;

				// Not while it is on its way: rapid pans would otherwise pile duplicate requests onto the queues.
				if (!e->surface && !e->requested && now >= e->retry_ms)
				{
					e->requested = true;
					need_fetch = true;
				}
			}

			if (need_fetch) fetch(e, coord);
		}

		evict();
	}

	// The decoded tile, or null while it is not loaded. The pointer is copied under the entry's lock,
	// because a worker may be publishing it at the same moment. Safe from several threads at once while
	// the UI thread, the only one that changes which tiles are held, waits for them.
	core::surface_ptr surface(const map_tile_id& coord) const
	{
		const auto found = _entries.find(coord);
		if (found == _entries.end()) return {};

		core::exclusive_lock lock(found->second->mutex);
		return found->second->surface;
	}

	// Lets every tile go, with whatever is still on its way for them, wherever it waits.
	void clear()
	{
		for (auto& [coord, e] : _entries)
		{
			core::exclusive_lock lock(e->mutex);
			e->in_view = false;
		}

		_entries.clear();
	}

private:
	void evict()
	{
		for (auto it = _entries.begin(); it != _entries.end() && _entries.size() > _capacity;)
		{
			bool in_view;
			{
				core::exclusive_lock lock(it->second->mutex);
				in_view = it->second->in_view;
			}

			if (in_view) ++it;
			else it = _entries.erase(it);
		}
	}

	// Whether a tile on its way is worth its next step. One that is not - out of view, or loaded meanwhile -
	// has its claim released, so it can be asked for again.
	static bool still_wanted(entry& e)
	{
		core::exclusive_lock lock(e.mutex);
		if (e.in_view && !e.surface) return true;
		e.requested = false;
		return false;
	}

	// Releases the claim on a tile that could not be had, and keeps it from being asked for again until then.
	static void failed(entry& e, const int64_t retry_ms)
	{
		core::exclusive_lock lock(e.mutex);
		e.requested = false;
		e.retry_ms = retry_ms;
	}

	static uint32_t rank_of(entry& e)
	{
		core::exclusive_lock lock(e.mutex);
		return e.rank;
	}

	// Decodes and hands the result to the UI, releasing the request claim. Every fetch ends here, in
	// still_wanted or in failed exactly once, so a tile can always be asked for again.
	static void publish(async_strategy& async, const entry_ptr& e, const std::function<void()>& invalidate,
	                    const std::shared_ptr<std::atomic_bool>& alive, const core::blob& data)
	{
		// Not decoded for a view that has moved on: the store keeps it for one that comes back.
		if (!still_wanted(*e)) return;

		files ff;
		auto surface = ff.image_to_surface(core::cspan(data.data(), data.size()));

		if (!surface)
		{
			failed(*e, core::now_ms() + retry_after_ms);
			return;
		}

		bool published = false;
		{
			core::exclusive_lock lock(e->mutex);
			e->requested = false;

			if (!e->surface)
			{
				e->surface = std::move(surface);
				published = e->in_view;
			}
		}

		if (published)
		{
			async.queue_ui([invalidate, alive]
			{
				if (alive->load())
				{
					invalidate();
				}
			});
		}
	}

	// One request on this map worker's own connection to the canonical OSM host, with the User-Agent the
	// usage policy requires.
	static core::blob download_from_osm(const map_tile_id& coord)
	{
		platform::web_request req;
		req.path = generate_tile_path(coord);
		req.headers.emplace_back("User-Agent", tile_user_agent());

		thread_local platform::web_host_ptr s_osm_con;
		if (!s_osm_con)
		{
			s_osm_con = platform::connect_to_host(osm_tile_host);
		}

		const auto response = send_request(s_osm_con, req);
		if (response.status_code == 200 && !response.body.empty())
			return core::blob(response.body.begin(), response.body.end());

		// No response at all: drop the connection, so the next request reconnects rather than reusing a
		// broken handle forever.
		if (response.status_code == 0) s_osm_con.reset();
		return {};
	}

	static void queue_download(async_strategy& async, const download_queue_ptr& downloads, const map_tile_id coord,
	                           entry_ptr e, const std::function<void()>& invalidate,
	                           const std::shared_ptr<std::atomic_bool>& alive)
	{
		bool start = false;
		{
			core::exclusive_lock lock(downloads->mutex);
			downloads->waiting.emplace_back(coord, std::move(e));

			if (downloads->running < max_downloads)
			{
				++downloads->running;
				start = true;
			}
		}

		if (start)
		{
			async.queue_async(async_queue::map_tile, [&async, downloads, invalidate, alive]
			{ download_waiting(async, downloads, invalidate, alive); });
		}
	}

	// A downloading worker: the most wanted tile still shown, then the next, until none waits. Whatever the
	// view moved away from is let go here, before anything is spent on it.
	static void download_waiting(async_strategy& async, const download_queue_ptr& downloads,
	                             const std::function<void()>& invalidate,
	                             const std::shared_ptr<std::atomic_bool>& alive)
	{
		for (;;)
		{
			map_tile_id coord{};
			entry_ptr e;
			downloader download;

			{
				core::exclusive_lock lock(downloads->mutex);
				auto& waiting = downloads->waiting;
				std::erase_if(waiting, [](const auto& w) { return !still_wanted(*w.second); });

				const auto next = std::ranges::min_element(waiting, {},
				                                           [](const auto& w) { return rank_of(*w.second); });

				if (next == waiting.end())
				{
					--downloads->running;
					return;
				}

				coord = next->first;
				e = std::move(next->second);
				waiting.erase(next);
				download = downloads->download;
			}

			const auto data = std::make_shared<const core::blob>(download(coord));

			if (data->empty())
			{
				failed(*e, core::now_ms() + retry_after_ms);
				continue;
			}

			async.queue_tile_db([key = map_tile_db_key(coord.z, coord.x, coord.y), data](tile_cache_db& db)
			{ db.store(key, core::cspan(data->data(), data->size())); });

			publish(async, e, invalidate, alive, *data);
		}
	}

	void fetch(const entry_ptr& e, const map_tile_id& coord)
	{
		_async.queue_tile_db([&async = _async, coord, e, downloads = _downloads, invalidate = _invalidate,
		                      alive = _alive](tile_cache_db& db)
		{
			if (!still_wanted(*e)) return;

			// Decoding a PNG on this thread would stall every following lookup, and downloading on it would stall
			// them for a network round trip, so both hand on to the map workers.
			auto data = std::make_shared<core::blob>(db.load(map_tile_db_key(coord.z, coord.x, coord.y)));

			if (!data->empty())
			{
				async.queue_async(async_queue::map_tile, [&async, e, invalidate, alive, data]
				{ publish(async, e, invalidate, alive, *data); });
			}
			else if (!df::network_allowed)
			{
				// A session that may not reach the network draws the tiles it has cached, and the world map
				// where it has none.
				failed(*e, std::numeric_limits<int64_t>::max());
			}
			else
			{
				queue_download(async, downloads, coord, e, invalidate, alive);
			}
		});
	}
};

class map_engine
{
public:
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

	int _zoom = 16;
	gps_coordinate _location;
	pointi _scroll_offset = {0, 0};
	pointi _temp_drag_offset = {0, 0};

	map_tile_cache _tiles;
	// A derived GPU cache rather than logical state, so it is shed on device loss from the const
	// broadcast the same way an element's texture is.
	mutable std::map<map_tile_id, render::texture_ptr> _texture_cache;

	// Item-location markers, indexed spatially by worker-prepared snapshots so publication is a
	// bounded move on the UI thread.
	marker_snapshot _markers;
	std::vector<map_cluster> _clusters;
	bool _clusters_dirty = false;
	sizei _cluster_extent;
	size_t _last_cluster_work = 0;

public:
	map_engine(async_strategy& async, std::function<void()> invalidate) :
		_async(async), _invalidate(std::move(invalidate)), _tiles(async, _invalidate)
	{
	}

	static constexpr int min_zoom = map_min_zoom;
	static constexpr int max_zoom = map_max_zoom;

	~map_engine() { _texture_cache.clear(); }

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

		const double new_longitude = std::clamp(new_center_tile_x / pow(2.0, _zoom) * 360.0 - 180.0, -180.0, 180.0);

		const double n = M_PI - 2.0 * M_PI * new_center_tile_y / pow(2.0, _zoom);
		const double new_latitude = std::clamp(180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n))), -85.0, 85.0);

		return gps_coordinate(new_latitude, new_longitude);
	}

	void render(render::draw_context& dc, const sizei& extent)
	{
		dc.clear(core::color(dc.colors.background, 1.0f));

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

			// The surface pointer is copied under the entry lock, because a map worker may be publishing it
			// concurrently.
			const auto surf = _tiles.surface(tile.coord);

			bool drawn = false;

			if (surf)
			{
				const auto texture = dc.create_texture();

				if (texture && texture->update(surf) != render::texture_update_result::failed)
				{
					_texture_cache[tile.coord] = texture;
					dc.draw_texture(texture, tile_rect);
					drawn = true;
				}
			}

			if (!drawn)
			{
				dc.draw_rect(tile_rect.inflate(-1), core::color(0.5, 0.5, 0.5, 0.5));
			}
		}

		update_clusters_if_needed(extent);
		render_markers(dc, extent);
		render_crosshair(dc, extent);
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

	void pan_start() { _temp_drag_offset = {0, 0}; }

	gps_coordinate pan(const pointi start_loc, const pointi current_loc, const recti& bounds)
	{
		_temp_drag_offset = current_loc - start_loc;
		_clusters_dirty = true;
		fetch_tiles(bounds.inflate(TILE_SIZE), _scroll_offset + _temp_drag_offset);
		_invalidate();
		return calc_center_gps();
	}

	gps_coordinate pan_end(const pointi /*start_loc*/, const pointi /*final_loc*/, const recti& bounds)
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
		_tiles.clear();
		_texture_cache.clear();
	}

	// The tile textures belong to the lost device. Keeping the decoded tiles means the map redraws
	// from what is already in memory; keeping the textures would draw nothing at all, because the
	// cache hit returns before the placeholder the miss path draws.
	void free_graphics_resources() const { _texture_cache.clear(); }

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
			result.cells.push_back(
				{gps_coordinate(cell.latitude / cell.count, cell.longitude / cell.count), cell.count, cell.rep});
		}

		result.has_markers = !result.cells.empty();

		result.coords.reserve(result.cells.size());
		for (auto i = 0u; i < result.cells.size(); ++i)
		{
			const auto& cell = result.cells[i];
			result.coords.emplace_back(static_cast<float>(cell.coordinate.longitude()),
			                           static_cast<float>(cell.coordinate.latitude()), i, 0, 0, 0.0f);
		}

		if (result.has_markers)
		{
			result.tree.build(result.coords);
		}

		return result;
	}

	static size_t prepared_marker_count(const marker_snapshot& markers) { return markers.cells.size(); }

	size_t last_cluster_work() const { return _last_cluster_work; }

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

	std::vector<gps_coordinate> visible_cluster_coordinates(const sizei& extent)
	{
		update_clusters_if_needed(extent);

		std::vector<gps_coordinate> result;
		result.reserve(_clusters.size());

		for (const auto& cluster : _clusters)
		{
			if (cluster.screen_pos.x >= 0 && cluster.screen_pos.y >= 0 && cluster.screen_pos.x < extent.cx &&
			    cluster.screen_pos.y < extent.cy)
			{
				result.emplace_back(gps_from_screen(cluster.screen_pos, extent));
			}
		}

		return result;
	}

	// The GPS coordinate at a screen point, so callers can recentre on a hit-tested cluster.
	gps_coordinate gps_at_screen(const pointi p, const sizei& extent) const { return gps_from_screen(p, extent); }

	// Returns the representative marker index of the cluster under `loc`, or -1 if
	// none. Fills the on-screen anchor and aggregated photo count.
	int hit_test_marker(const pointi loc, const sizei& /*extent*/, pointi& anchor_out, int& count_out) const
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

	void fetch_tiles_for_bounds(const recti& bounds) { fetch_tiles(bounds, _scroll_offset); }

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
		const double dy = (lat_to_tile_y(g.latitude(), _zoom) - lat_to_tile_y(_location.latitude(), _zoom)) * TILE_SIZE;

		return pointi(static_cast<int>(std::lround(extent.cx / 2.0 + dx + total_offset.x)),
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
			if (screen_pos.x < -cell || screen_pos.y < -cell || screen_pos.x > extent.cx + cell ||
			    screen_pos.y > extent.cy + cell)
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

	void render_markers(render::draw_context& dc, const sizei& /*extent*/) const
	{
		for (const auto& c : _clusters)
		{
			const int radius = cluster_radius(c.count);
			const recti r(c.screen_pos - pointi(radius, radius), sizei(radius * 2, radius * 2));

			// Semi-transparent so the underlying map stays visible: a soft white halo
			// plus a translucent accent fill, with the aggregated count on top.
			const recti halo = r.inflate(1);
			dc.draw_rounded_rect(halo, core::color(1.0f, 1.0f, 1.0f, 0.35f), radius + 1);
			dc.draw_rounded_rect(r, core::color(ui::style::colors.dialog_selected_background, 0.5f), radius);

			if (c.count > 1)
			{
				dc.draw_text(std::to_string(c.count), r, render::font_face::dialog,
				             render::text_style::single_line_center, core::color(1.0f, 1.0f, 1.0f, 0.9f), {});
			}
		}
	}

	void render_attribution(render::draw_context& dc, const sizei& extent) const
	{
		draw_map_attribution(dc, recti(extent));
	}

	// A plain cross, semi-transparent: two unbroken lines whose intersection is the coordinate
	// that will be written. No ring and no centre disc - either would read as one more cluster
	// bubble, and either would hide the bubble the user has just centred on.
	void render_crosshair(render::draw_context& dc, const sizei& extent) const
	{
		const int center_x = extent.cx / 2;
		const int center_y = extent.cy / 2;

		constexpr int arm = 48;
		constexpr int line_width = 3;
		constexpr int half = line_width / 2;

		const auto crosshair_color = core::color(ui::style::colors.important_background, 0.6f);
		// A thin lighter underlay keeps the cross readable over both dark and pale tiles.
		const auto outline_color = core::color(1.0f, 1.0f, 1.0f, 0.35f);

		const recti horizontal(center_x - arm, center_y - half, center_x + arm, center_y - half + line_width);
		dc.draw_rect(horizontal.inflate(0, 1), outline_color);
		dc.draw_rect(horizontal, crosshair_color);

		const recti vertical(center_x - half, center_y - arm, center_x - half + line_width, center_y + arm);
		dc.draw_rect(vertical.inflate(1, 0), outline_color);
		dc.draw_rect(vertical, crosshair_color);
	}

	void fetch_tiles(const recti& bounds, const pointi scroll_offset)
	{
		auto tiles = get_tiles_for_view(bounds, scroll_offset, _location, _zoom);

		// The tile under the centre first, as a map fills in.
		const auto centre = bounds.center();
		std::ranges::stable_sort(tiles, {}, [centre](const map_tile& t)
		{
			const auto dx = t.screen_pos.x + TILE_SIZE / 2 - centre.x;
			const auto dy = t.screen_pos.y + TILE_SIZE / 2 - centre.y;
			return dx * dx + dy * dy;
		});

		std::vector<map_tile_id> visible;
		visible.reserve(tiles.size());
		for (const auto& tile : tiles) visible.push_back(tile.coord);

		// Textures for tiles that scrolled out of view are released eagerly, so the texture cache does not
		// balloon while panning.
		std::erase_if(_texture_cache, [&visible](const auto& entry)
		{ return std::ranges::find(visible, entry.first) == visible.end(); });

		_tiles.show(visible);
	}
};
