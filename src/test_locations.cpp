// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tests for places and location search (model_location*) -- the gazetteer, place naming,
// distance, map-driven queries, visits and the timeline. Owning document: docs/locations.md.

#include "pch.h"

#include <sqlite3.h>

#include "test_fixtures.h"
#include "test_runner.h"
#include "model_location.h"
#include "model_faces_search.h"
#include "model_tile_cache.h"
#include "model_visits.h"
#include "model_explore.h"
#include "ui_globe.h"
#include "ui_map_common.h"

static void should_group_sidebar_map_by_place()
{
	const location_cache locations;
	index_histograms histograms;

	const auto record = [&]
	{
		df::index_file_item file;
		file.ft = files::file_type_from_name("test.jpg");
		const auto metadata = std::make_shared<prop::item_metadata>();
		metadata->coordinate = gps_coordinate(50.08806, 14.42083);
		metadata->location_place = "Prague"_c;
		metadata->location_state = "Prague"_c;
		metadata->location_country = "Czechia"_c;
		file.metadata.store(metadata);
		histograms.record(locations, file);
	};

	record();
	record();

	const auto map_loc = df::location_heat_map::calc_map_loc({50.08806, 14.42083});
	const auto heat = histograms._locations.coordinates[map_loc.y * df::location_heat_map::map_width + map_loc.x];
	assert_equal(2u, heat, "map heat accumulates repeated coordinates");
	assert_equal(1u, static_cast<uint32_t>(histograms._location_groups.size()), "one place group");
	const auto& group = histograms._location_groups.begin()->second;
	assert_equal(2u, group.count, "place group count");
	assert_equal(true, !core::is_empty(group.country), "place group country");
	assert_equal(true, group.centroid().distance_in_kilometers({50.08806, 14.42083}) < 0.01,
	             "place group uses photo centroid");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Map tiles (OpenStreetMap tile usage policy compliance)
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_build_tile_user_agent()
{
	// The OSM tile usage policy requires a User-Agent that names the app + version
	// and provides a contact URL; a generic/library-default agent is blocked.
	const auto ua = tile_user_agent();
	const auto expected = std::format("Diffractor/{} (+https://diffractor.com)", s_app_version);

	assert_equal(expected, ua, "stable product, version and contact url", "tile user agent");
	assert_equal(std::string::npos, ua.find_first_of("\r\n\t"), "contains no invalid header characters",
	             "tile user agent");
}

static void should_pack_tile_database_keys()
{
	// Zoom caps at 18, so x and y stay below 2^20 and the whole address fits a positive int64 -
	// which is what lets the store key its rows on the rowid.
	assert_equal(true, map_tile_db_key(0, 0, 0) == 0, "origin", "tile key");

	constexpr auto widest = 1 << map_max_zoom;
	assert_equal(true, map_tile_db_key(map_max_zoom, widest - 1, widest - 1) > 0, "widest address stays positive",
	             "tile key");

	std::set<int64_t> seen;

	for (auto z = map_min_zoom; z <= map_max_zoom; ++z)
	{
		const auto span = 1 << z;

		for (const auto x : {0, 1, span / 2, span - 1})
		{
			for (const auto y : {0, 1, span / 2, span - 1})
			{
				assert_equal(true, seen.insert(map_tile_db_key(z, x, y)).second, "address is unique", "tile key");
			}
		}
	}
}

static core::date_t tile_days_ago(const uint32_t days)
{
	return core::date_t(core::utc_now().to_int64() - days * core::date_t::intervals_per_day);
}

static void should_cache_tiles_in_a_database()
{
	tile_cache_db db;
	db.open(test_temps.next_path(".db"));

	assert_equal(true, db.is_open(), "database opened", "tile cache");

	constexpr auto key = map_tile_db_key(3, 1, 2);
	assert_equal(true, db.load(key).empty(), "load absent returns empty", "tile cache");

	const core::blob bytes = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3, 4};
	db.store(key, core::cspan(bytes));
	db.flush();

	const auto loaded = db.load(key);
	assert_equal(static_cast<int>(bytes.size()), static_cast<int>(loaded.size()), "size round-trips", "tile cache");
	assert_equal(true, loaded == bytes, "bytes round-trip", "tile cache");
	assert_equal(1, static_cast<int>(db.count()), "one row stored", "tile cache");

	const auto path = db.path();
	db.close();

	// A fresh connection over the same file still finds it (persists between sessions).
	tile_cache_db reopened;
	reopened.open(path);
	assert_equal(true, reopened.load(key) == bytes, "persists across sessions", "tile cache");
	reopened.close();
}

static void should_prune_unused_tiles()
{
	tile_cache_db db;
	db.open(test_temps.next_path(".db"));

	const core::blob bytes = {1, 2, 3, 4};
	const auto long_ago = tile_days_ago(60);

	for (auto i = 0; i < 3; ++i)
	{
		db.store(map_tile_db_key(5, i, i), core::cspan(bytes), long_ago);
	}

	db.flush();
	assert_equal(3, static_cast<int>(db.count()), "stored before prune", "tile cache");

	db.prune(30, tile_cache_db::max_bytes);
	assert_equal(0, static_cast<int>(db.count()), "tiles unused for longer than the window are dropped", "tile cache");

	db.close();
}

static void should_bound_tile_cache_by_size()
{
	tile_cache_db db;
	db.open(test_temps.next_path(".db"));

	const core::blob bytes(8192, 0x5a);
	const auto long_ago = tile_days_ago(60);

	for (auto i = 0; i < 8; ++i)
	{
		db.store(map_tile_db_key(6, i, i), core::cspan(bytes), long_ago);
	}

	db.flush();
	assert_equal(8, static_cast<int>(db.count()), "stored before prune", "tile cache");

	// A window wide enough that the age pass matches nothing, so only the size pass can evict.
	db.prune(365, 1);
	assert_equal(0, static_cast<int>(db.count()), "size cap evicts least recently used", "tile cache");

	db.close();
}

static void should_budget_expired_tile_pruning()
{
	tile_cache_db db;
	db.open(test_temps.next_path(".db"));

	const core::blob bytes = {1, 2, 3, 4};
	const auto long_ago = tile_days_ago(60);
	const auto recent = tile_days_ago(1);

	for (auto i = 0; i < 4100; ++i)
	{
		db.store(map_tile_db_key(8, i, i), core::cspan(bytes), long_ago);
	}

	for (auto i = 0; i < 3; ++i)
	{
		db.store(map_tile_db_key(9, i, i), core::cspan(bytes), recent);
	}

	assert_equal(4103, static_cast<int>(db.count()), "stored before prune", "tile cache");

	db.prune(30, tile_cache_db::max_bytes);
	assert_equal(7, static_cast<int>(db.count()), "one maintenance pass removes only its row budget", "tile cache");

	db.prune(30, tile_cache_db::max_bytes);
	assert_equal(3, static_cast<int>(db.count()), "a later pass finishes expired cleanup", "tile cache");

	db.close();
}

static void should_classify_tile_cache_open_failures_before_replacement()
{
	assert_equal(false, tile_cache_test_seams::failure_allows_replacement(SQLITE_BUSY),
	             "busy is environmental, not replaceable", "tile cache");
	assert_equal(false, tile_cache_test_seams::failure_allows_replacement(SQLITE_LOCKED),
	             "locked is environmental, not replaceable", "tile cache");
	assert_equal(false, tile_cache_test_seams::failure_allows_replacement(SQLITE_IOERR),
	             "I/O is environmental, not replaceable", "tile cache");
	assert_equal(true, tile_cache_test_seams::failure_allows_replacement(SQLITE_ERROR),
	             "incompatible schema is replaceable", "tile cache");
	assert_equal(true, tile_cache_test_seams::failure_allows_replacement(SQLITE_CORRUPT),
	             "corrupt bytes are replaceable", "tile cache");
	assert_equal(true, tile_cache_test_seams::failure_allows_replacement(SQLITE_NOTADB),
	             "non-database bytes are replaceable", "tile cache");
}

static void should_keep_tile_transaction_bookkeeping_after_commit_failure()
{
	tile_cache_db db;
	db.open(test_temps.next_path(".db"));

	const core::blob bytes = {1, 2, 3, 4};
	for (auto i = 0u; i < tile_cache_db::prune_write_interval; ++i)
	{
		db.store(map_tile_db_key(10, static_cast<int>(i), static_cast<int>(i)), core::cspan(bytes));
	}

	tile_cache_test_seams::fail_next_commit();
	db.flush();

	assert_equal(false, tile_cache_test_seams::wrapper_transaction_active(db),
	             "wrapper state is not left inside a transaction", "tile cache");
	assert_equal(false, tile_cache_test_seams::sqlite_transaction_active(db),
	             "SQLite state agrees after failed commit recovery", "tile cache");
	assert_equal(tile_cache_db::prune_write_interval, tile_cache_test_seams::writes_since_prune(db),
	             "prune is not run after persistence failed", "tile cache");
	assert_equal(0, static_cast<int>(db.count()), "rolled back rows are not reported as stored", "tile cache");

	constexpr auto key = map_tile_db_key(11, 1, 1);
	db.store(key, core::cspan(bytes));
	db.flush();
	assert_equal(true, db.load(key) == bytes, "subsequent writes persist after the fault clears", "tile cache");

	db.close();
}

static void should_keep_tiles_inside_the_retention_window()
{
	// The OSM tile usage policy asks clients to keep what they download for at least a week, so a
	// prune that has run out of anything older must give up rather than evict a fresh tile.
	tile_cache_db db;
	db.open(test_temps.next_path(".db"));

	const core::blob bytes(8192, 0x5a);

	for (auto i = 0; i < 4; ++i)
	{
		db.store(map_tile_db_key(7, i, i), core::cspan(bytes));
	}

	db.flush();
	db.prune(0, 1);

	assert_equal(4, static_cast<int>(db.count()), "recently fetched tiles survive any cap", "tile cache");
	assert_equal(0u, tile_cache_test_seams::writes_since_prune(db),
	             "protected tiles over the cap do not reschedule idle prune passes", "tile cache");

	db.close();
}

static void should_replace_an_unreadable_tile_cache()
{
	const auto path = test_temps.next_path(".db");
	const core::blob junk(4096, 0x7f);
	platform::blob_save_to_file(core::cspan(junk), path);

	tile_cache_db db;
	db.open(path);

	assert_equal(true, db.is_open(), "a file this build cannot read is replaced", "tile cache");

	constexpr auto key = map_tile_db_key(4, 5, 6);
	const core::blob bytes = {9, 8, 7};
	db.store(key, core::cspan(bytes));
	db.flush();

	assert_equal(true, db.load(key) == bytes, "usable after replacement", "tile cache");

	db.close();
}

static void should_resolve_tile_cache_db_beside_index_db()
{
	// The tile database shares the folder the index database (diffractor-cache.db) is opened from.
	const auto db_path = resolve_tile_cache_db_path();
	const auto base = known_path(platform::known_folder::app_cache_data);

	assert_equal(true, db_path == base.combine_file("map-tiles-cache.db"sv), "database beside index database",
	             "tile cache location");
	assert_equal(true, platform::exists(base), "base folder exists", "tile cache location");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Fetching map tiles
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	// A tile's worth of PNG, as the store keeps it and OpenStreetMap sends it.
	core::blob tile_png()
	{
		const auto surface = std::make_shared<core::surface>();
		surface->alloc(256, 256, core::texture_format::ARGB);

		for (auto y = 0; y < 256; ++y)
		{
			auto* const line = std::bit_cast<core::color32*>(surface->pixels_line(y));
			for (auto x = 0; x < 256; ++x) line[x] = core::rgba(x, y, 128);
		}

		return save_png(surface, {})->data().clone();
	}

	// A cache whose store is a test database, whose workers run when the test says, and whose downloads the test
	// answers, so nothing reaches the network.
	struct tile_fetch_fixture
	{
		tile_cache_db db;
		deferred_async_strategy async;
		int invalidations = 0;
		map_tile_cache cache;
		std::vector<map_tile_id> downloaded;
		bool network_was_allowed = df::network_allowed;

		explicit tile_fetch_fixture(const bool downloads_succeed = true) : cache(async, [this] { ++invalidations; })
		{
			df::network_allowed = true;
			db.open(test_temps.next_path(".db"));
			async.tile_db = &db;
			cache.download_with([this, downloads_succeed](const map_tile_id& t)
			{
				downloaded.push_back(t);
				return downloads_succeed ? tile_png() : core::blob{};
			});
		}

		~tile_fetch_fixture()
		{
			df::network_allowed = network_was_allowed;
			db.close();
		}

		tile_fetch_fixture(const tile_fetch_fixture&) = delete;
		tile_fetch_fixture& operator=(const tile_fetch_fixture&) = delete;

		void run_workers()
		{
			while (async.run_next(async_queue::map_tile))
			{
			}
			async.drain_ui();
		}
	};
}

// After a zoom or a turn, a tile the view has moved away from is not downloaded, wherever it was waiting, and the
// one the view shows is - into the store, and onto the screen.
static void should_download_only_the_map_tiles_still_shown()
{
	tile_fetch_fixture f;
	const map_tile_id a{1, 1, 5}, b{2, 1, 5}, c{3, 1, 5};

	f.cache.show(std::vector{a, b, c});
	assert_equal(static_cast<size_t>(map_tile_cache::max_downloads),
	             f.async.pending_worker_count(async_queue::map_tile), "no more downloads run at once than the limit",
	             "tile fetch");

	f.cache.show(std::vector{c});
	f.run_workers();

	assert_equal(1_z, f.downloaded.size(), "only the tile still shown is downloaded", "tile fetch");
	assert_equal(true, f.downloaded.front() == c, "the one the view moved to", "tile fetch");
	assert_equal(true, f.cache.surface(c) != nullptr, "it is decoded", "tile fetch");
	assert_equal(1, f.invalidations, "and the view drawn again for it", "tile fetch");
	assert_equal(false, f.db.load(map_tile_db_key(c.z, c.x, c.y)).empty(), "the store keeps it", "tile fetch");

	// Shown again, the tiles let go are fetched again.
	f.cache.show(std::vector{a, b, c});
	f.run_workers();
	assert_equal(3_z, f.downloaded.size(), "tiles let go can be asked for again", "tile fetch");
}

// Downloads take the most wanted tile first, as the view last ranked them, and a tile the store holds is decoded by
// a worker of its own rather than waiting for a download to finish.
static void should_download_the_most_wanted_map_tile_first()
{
	tile_fetch_fixture f;
	const map_tile_id a{1, 1, 5}, b{2, 1, 5}, c{3, 1, 5}, stored{4, 1, 5};
	f.db.store(map_tile_db_key(stored.z, stored.x, stored.y), core::cspan(tile_png()));

	f.cache.show(std::vector{a, b, c, stored});
	assert_equal(static_cast<size_t>(map_tile_cache::max_downloads) + 1,
	             f.async.pending_worker_count(async_queue::map_tile),
	             "the stored tile's decode is queued beside the downloads, not behind them", "tile fetch");

	f.cache.show(std::vector{c, b, a, stored});
	f.run_workers();

	assert_equal(true, f.downloaded == std::vector{c, b, a}, "the newest ranking decides the order", "tile fetch");
	assert_equal(true, f.cache.surface(stored) != nullptr, "the stored tile is decoded without a download",
	             "tile fetch");
}

// A tile that could not be had is not asked for again each time the view is drawn.
static void should_not_ask_again_at_once_for_a_map_tile_that_failed()
{
	tile_fetch_fixture f(false);
	const map_tile_id a{1, 1, 5};

	f.cache.show(std::vector{a});
	f.run_workers();
	assert_equal(1_z, f.downloaded.size(), "the tile is asked for", "tile fetch");
	assert_equal(true, f.cache.surface(a) == nullptr, "and is not there", "tile fetch");

	f.cache.show(std::vector{a});
	f.run_workers();
	assert_equal(1_z, f.downloaded.size(), "drawn again a moment later, it is not asked for again", "tile fetch");
	assert_equal(0, f.invalidations, "and nothing is drawn again for it", "tile fetch");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Map geometry
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_anchor_map_marker_cells_to_world()
{
	const gps_coordinate first(50.0755, 14.4378);
	const gps_coordinate nearby(50.07551, 14.43781);

	const auto first_cell = map_marker_world_cell(first, 16);
	const auto nearby_cell = map_marker_world_cell(nearby, 16);

	assert_equal(true, first_cell == map_marker_world_cell(first, 16), "cell is independent of viewport center",
	             "map markers");
	assert_equal(true, first_cell == nearby_cell, "nearby markers aggregate in the same world cell", "map markers");
	assert_equal(false, first_cell == map_marker_world_cell(first, 17), "zoom recalculates world cells", "map markers");
}

static void should_publish_prepared_map_marker_snapshots()
{
	null_async_strategy async;
	auto invalidated = false;
	map_engine engine(async, [&invalidated] { invalidated = true; });
	constexpr sizei extent(800, 600);
	constexpr recti bounds(0, 0, extent.cx, extent.cy);
	const gps_coordinate prague(50.0755, 14.4378);

	std::vector<map_engine::marker> markers;
	markers.reserve(100);
	for (auto i = 0; i < 100; ++i) markers.push_back({prague, 1});

	auto snapshot = map_engine::prepare_marker_snapshot(markers, 16);
	assert_equal(true, snapshot.has_markers, "snapshot has markers", "map markers");
	assert_equal(1u, static_cast<uint32_t>(map_engine::prepared_marker_count(snapshot)),
	             "snapshot work is bounded by prepared cells", "map markers");

	engine.set_location(prague, bounds);
	invalidated = false;
	engine.set_marker_snapshot(std::move(snapshot));
	assert_equal(true, invalidated, "publishing a snapshot invalidates once", "map markers");

	const auto visible = engine.visible_cluster_coordinates(extent);
	assert_equal(1u, static_cast<uint32_t>(visible.size()), "colocated markers form one bubble", "map markers");
	assert_equal(1u, static_cast<uint32_t>(engine.last_cluster_work()),
	             "paint clustering visits prepared cells, not source markers", "map markers");

	pointi anchor;
	auto count = 0;
	const auto marker = engine.hit_test_marker({extent.cx / 2, extent.cy / 2}, extent, anchor, count);
	assert_equal(true, marker >= 0, "the prepared bubble is hit-testable", "map markers");
	assert_equal(100, count, "hit testing and paint use the same aggregate count", "map markers");
}

static void should_cull_prepared_map_marker_cells_to_view()
{
	null_async_strategy async;
	map_engine engine(async, [] {});
	constexpr sizei extent(800, 600);
	constexpr recti bounds(0, 0, extent.cx, extent.cy);
	const gps_coordinate prague(50.0755, 14.4378);
	std::vector<map_engine::marker> markers;

	for (auto lat = -60; lat <= 60; lat += 5)
	{
		for (auto lon = -175; lon <= 175; lon += 5)
		{
			markers.push_back({gps_coordinate(static_cast<double>(lat), static_cast<double>(lon)), 1});
		}
	}
	markers.push_back({prague, 1});

	auto snapshot = map_engine::prepare_marker_snapshot(markers, 8);
	const auto prepared_count = map_engine::prepared_marker_count(snapshot);
	assert_equal(true, prepared_count > 100u, "test prepares many cells", "map markers");

	engine.set_location(prague, bounds);
	engine.set_marker_snapshot(std::move(snapshot));
	const auto visible = engine.visible_cluster_coordinates(extent);

	assert_equal(true, !visible.empty(), "the viewport still has visible cells", "map markers");
	assert_equal(true, engine.last_cluster_work() < prepared_count / 4,
	             "viewport clustering visits nearby prepared cells, not every prepared cell", "map markers");
}

static void should_clear_map_marker_hits_when_snapshot_changes()
{
	null_async_strategy async;
	map_engine engine(async, [] {});
	constexpr sizei extent(800, 600);
	constexpr recti bounds(0, 0, extent.cx, extent.cy);
	const gps_coordinate prague(50.0755, 14.4378);

	engine.set_location(prague, bounds);
	engine.set_marker_snapshot(map_engine::prepare_marker_snapshot({{prague, 1}}, 16));
	(void)engine.visible_cluster_coordinates(extent);

	pointi anchor;
	auto count = 0;
	assert_equal(true, engine.hit_test_marker({extent.cx / 2, extent.cy / 2}, extent, anchor, count) >= 0,
	             "initial snapshot is hit-testable", "map markers");

	engine.set_marker_snapshot({});
	assert_equal(-1, engine.hit_test_marker({extent.cx / 2, extent.cy / 2}, extent, anchor, count),
	             "a replaced snapshot cannot return stale hit-test indices", "map markers");
}

static void should_measure_distance_to_map_cells()
{
	constexpr recti cell(10, 20, 30, 40);
	assert_equal(0, static_cast<int>(distance_squared({20, 30}, cell)), "cursor inside cell has zero distance",
	             "map cells");
	assert_equal(25, static_cast<int>(distance_squared({35, 30}, cell)), "horizontal gap is measured", "map cells");
	assert_equal(50, static_cast<int>(distance_squared({35, 45}, cell)), "diagonal gap is measured", "map cells");
	const auto left_distance = distance_squared({50, 30}, cell);
	constexpr auto right_distance = distance_squared({50, 30}, recti(60, 20, 80, 40));
	assert_equal(true, right_distance < left_distance, "nearest occupied cell wins between cells", "map cells");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The sidebar globe
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_project_globe_coordinates()
{
	constexpr pointi center(160, 160);
	constexpr double radius = 150.0;

	const globe_projection equator({0.0, 0.0}, center, radius);
	const auto middle = equator.project({0.0, 0.0});
	assert_equal(true, middle.has_value(), "the view coordinate faces the viewer", "globe");
	assert_equal(true, middle.value() == center, "and lands dead centre", "globe");

	const auto east = equator.project({0.0, 90.0});
	assert_equal(false, east.has_value(), "a quarter turn away sits exactly on the limb", "globe");
	assert_equal(false, equator.project({0.0, 180.0}).has_value(), "the far side is not shown", "globe");

	const auto right = equator.project({0.0, 30.0});
	assert_equal(true, right.has_value(), "an eastward place stays visible", "globe");
	assert_equal(true, right->x > center.x, "and is drawn to the east", "globe");
	assert_equal(center.y, right->y, "with no northward drift", "globe");

	const auto north = equator.project({30.0, 0.0});
	assert_equal(true, north.has_value(), "a northward place stays visible", "globe");
	assert_equal(true, north->y < center.y, "and is drawn above the centre", "globe");

	// Full pan: with the view over Australia the antipode is hidden and Australia is centred.
	const gps_coordinate sydney(-33.8688, 151.2093);
	const globe_projection tilted(sydney, center, radius);
	const auto framed = tilted.project(sydney);
	assert_equal(true, framed.has_value(), "the framed place faces the viewer", "globe");
	assert_equal(true, framed.value() == center, "and holds the centre under a tilted view", "globe");
	assert_equal(false, tilted.project({33.8688, -28.7907}).has_value(), "its antipode is hidden", "globe");

	const auto pole = globe_projection({90.0, 0.0}, center, radius).project({90.0, 137.0});
	assert_equal(true, pole.has_value(), "a polar view can hold the pole itself", "globe");
	assert_equal(true, pole.value() == center, "wherever the longitude claims to be", "globe");
}

static void should_round_trip_globe_pixels()
{
	constexpr pointi center(160, 160);
	constexpr double radius = 150.0;

	const gps_coordinate views[] = {{0.0, 0.0}, {-33.8688, 151.2093}, {39.5, -98.35}, {89.0, 0.0}};
	const gps_coordinate places[] = {{0.0, 0.0}, {51.5, -0.1}, {-33.8688, 151.2093}, {35.7, 139.7}, {-60.0, -170.0}};

	for (const auto& view : views)
	{
		const globe_projection projection(view, center, radius);

		for (const auto& place : places)
		{
			const auto pixel = projection.project(place);
			if (!pixel) continue;

			const auto shown = projection.unproject(pixel.value());
			assert_equal(true, shown.has_value(), "a pixel on the disc shows a coordinate", "globe");

			// Rounding to the pixel costs about a fifth of a degree at this radius, and more as
			// the limb compresses; a degree of slack keeps the check honest without being noise.
			assert_near(place.latitude(), shown->latitude(), 1.0, "round trip latitude", "globe");
			assert_near(0.0, globe_wrap_longitude(place.longitude() - shown->longitude()), 1.0, "round trip longitude",
			            "globe");
		}
	}

	const globe_projection projection({0.0, 0.0}, center, radius);
	assert_equal(false, projection.unproject({center.x + 200, center.y}).has_value(), "a pixel off the disc shows none",
	             "globe");
	assert_equal(true, projection.unproject({center.x + 149, center.y}).has_value(), "a pixel inside the limb does",
	             "globe");
}

static void should_rotate_globe_by_drag()
{
	constexpr double radius = 150.0;
	const gps_coordinate start(0.0, 0.0);

	const auto west = globe_view_from_drag(start, {static_cast<int>(radius * 2), 0}, radius);
	assert_near(-180.0, west.longitude(), 0.0001, "a drag across the diameter turns half way round", "globe");
	assert_near(0.0, west.latitude(), 0.0001, "and leaves the latitude alone", "globe");

	const auto east = globe_view_from_drag(start, {-static_cast<int>(radius), 0}, radius);
	assert_near(90.0, east.longitude(), 0.0001, "dragging left turns the globe east", "globe");

	// Dragging down brings the north to the centre - the ball rolls under the finger.
	const auto north = globe_view_from_drag(start, {0, static_cast<int>(radius / 2)}, radius);
	assert_near(45.0, north.latitude(), 0.0001, "dragging down looks north", "globe");

	const auto clamped = globe_view_from_drag({80.0, 0.0}, {0, static_cast<int>(radius)}, radius);
	assert_near(90.0, clamped.latitude(), 0.0001, "the pole is as far as the view tilts", "globe");
	assert_near(-90.0, globe_view_from_drag({-80.0, 0.0}, {0, -static_cast<int>(radius)}, radius).latitude(), 0.0001,
	            "in both directions", "globe");

	// Turning past the date line has to stay continuous: a jump here reads as the globe flipping.
	const auto crossed = globe_view_from_drag({0.0, -170.0}, {static_cast<int>(radius / 3), 0}, radius);
	assert_near(160.0, crossed.longitude(), 0.0001, "the view wraps through the date line", "globe");
	assert_near(-179.0, globe_wrap_longitude(181.0), 0.0001, "wrapping is symmetric", "globe");

	// gps_coordinate spends exactly +-180 as its "no coordinate" sentinel, so a view that turns onto
	// the date line must still be a view rather than a hole.
	assert_near(-180.0, globe_wrap_longitude(-180.0), 0.001, "the date line stays the date line", "globe");
	assert_equal(true, gps_coordinate(0.0, globe_wrap_longitude(-180.0)).is_valid(),
	             "and is a coordinate, not the invalid sentinel", "globe");
	assert_equal(true, gps_coordinate(0.0, globe_wrap_longitude(540.0)).is_valid(), "from either direction", "globe");

	assert_equal(true, globe_view_from_drag(start, {50, 50}, 0.0).longitude() == start.longitude(),
	             "an unlaid-out globe cannot turn", "globe");
}

static void should_frame_globe_on_the_collection()
{
	// An Australian collection opens on Australia, a US one on the US: the default view is where
	// the photos are, not a fixed prime meridian.
	globe_framer australian;
	australian.add(gps_coordinate(-33.8688, 151.2093), 400);
	australian.add(gps_coordinate(-37.8136, 144.9631), 100);
	const auto australia = australian.view();
	assert_equal(true, australia.is_valid(), "a collection has a view", "globe framing");
	assert_near(-34.7, australia.latitude(), 1.5, "framed on the southern hemisphere", "globe framing");
	assert_near(149.9, australia.longitude(), 1.5, "and weighted toward the larger place", "globe framing");

	globe_framer american;
	american.add(gps_coordinate(37.7749, -122.4194), 300);
	american.add(gps_coordinate(40.7128, -74.0060), 300);
	const auto usa = american.view();
	assert_near(-98.0, usa.longitude(), 3.0, "a coast-to-coast collection frames on the middle", "globe framing");
	assert_equal(true, usa.latitude() > 30.0, "in the northern hemisphere", "globe framing");

	// The antimeridian trap: averaging the numbers would face the Atlantic instead.
	globe_framer date_line;
	date_line.add(gps_coordinate(0.0, 170.0), 1);
	date_line.add(gps_coordinate(0.0, -170.0), 1);
	const auto crossing = date_line.view();
	assert_near(180.0, std::abs(crossing.longitude()), 0.001, "places either side face the date line", "globe framing");
	assert_equal(true, crossing.is_valid(), "and the view is usable there", "globe framing");

	globe_framer empty;
	assert_equal(false, empty.view().is_valid(), "an empty collection frames nothing", "globe framing");

	globe_framer cancelling;
	cancelling.add(gps_coordinate(90.0, 0.0), 1);
	cancelling.add(gps_coordinate(-90.0, 0.0), 1);
	assert_equal(false, cancelling.view().is_valid(), "and neither does one with no deserved direction",
	             "globe framing");

	globe_framer ignored;
	ignored.add(gps_coordinate(), 10);
	ignored.add(gps_coordinate(10.0, 20.0), 0);
	assert_equal(false, ignored.view().is_valid(), "places without a coordinate or a count are not framed on",
	             "globe framing");
}

// A drag keeps the point grabbed under the pointer, and a zoom the point under the pointer, by solving for the
// view that puts a coordinate at a point of the disc. Either is wrong as soon as the coordinate drifts from it.
static void should_place_a_coordinate_under_the_pointer()
{
	constexpr pointi center(400, 400);
	constexpr double radius = 300.0;

	const gps_coordinate views[] = {{0.0, 0.0}, {-33.8688, 151.2093}, {48.0, 16.0}, {-70.0, -120.0}};
	const gps_coordinate places[] = {{0.0, 0.0}, {43.5, 16.4}, {-33.8688, 151.2093}, {35.7, 139.7}, {64.1, -21.9}};
	const std::pair<double, double> offsets[] = {{0.0, 0.0}, {0.3, -0.2}, {-0.55, 0.4}, {0.1, 0.8}, {-0.7, -0.6}};

	for (const auto& view : views)
	{
		for (const auto& place : places)
		{
			for (const auto& [unit_x, unit_y] : offsets)
			{
				const auto placed = globe_view_placing(place, unit_x, unit_y, view);
				if (!placed) continue;

				const auto at = globe_projection(*placed, center, radius).project(place);
				if (!at)
				{
					assert_equal(true, std::abs(placed->latitude()) > 89.9,
					             "only a view held at a pole may leave the point out of sight", "globe placing");
					continue;
				}

				// An upright view cannot always put the point exactly there: what it misses by is how far past a
				// pole it would have had to tip. Short of that it is exact to the pixel.
				if (std::abs(placed->latitude()) < 89.9)
				{
					assert_near(center.x + unit_x * radius, at->x, 1.0, "the point lands where it was put, across",
					            "globe placing");
					assert_near(center.y - unit_y * radius, at->y, 1.0, "and down", "globe placing");
				}
			}
		}
	}

	const auto centred = globe_view_placing({43.5, 16.4}, 0.0, 0.0, {0.0, 0.0});
	assert_equal(true, centred.has_value(), "a coordinate can be put at the centre", "globe placing");
	assert_near(43.5, centred->latitude(), 0.0001, "by facing it", "globe placing");
	assert_near(16.4, centred->longitude(), 0.0001, "squarely", "globe placing");

	assert_equal(false, globe_view_placing({10.0, 10.0}, 0.9, 0.9, {0.0, 0.0}).has_value(),
	             "a point off the disc has no view", "globe placing");
	assert_equal(false, globe_view_placing({85.0, 10.0}, 0.5, 0.0, {0.0, 0.0}).has_value(),
	             "nor one a parallel that small cannot reach", "globe placing");
	assert_equal(false, globe_view_placing(gps_coordinate(), 0.0, 0.0, {0.0, 0.0}).has_value(),
	             "nor a coordinate that is not one", "globe placing");
}

namespace
{
	// An item of the collection as the index holds it, taken on a day, with the place its file names.
	struct explore_test_item
	{
		core::file_path path;
		df::index_file_item file;
	};

	void add_explore_item(std::vector<explore_test_item>& items, const std::string_view extension, const uint32_t day,
	                      const std::string_view country, const std::string_view state = {},
	                      const gps_coordinate where = {})
	{
		explore_test_item item;
		item.path = core::file_path(std::format("c:\\explore\\{}.{}", items.size(), extension));
		item.file.ft = files::file_type_from_name(item.path.name().sv());
		const auto md = item.file.safe_ps();
		md->dates.add(prop::date_source::exif_original,
		              core::date_t::from_days(day) + static_cast<int64_t>(core::date_t::intervals_per_day / 2));
		if (!country.empty()) md->location_country = core::cache(country);
		if (!state.empty()) md->location_state = core::cache(state);
		md->coordinate = where;
		items.push_back(std::move(item));
	}

	df::explore_model build_test_explore_model(const std::vector<explore_test_item>& items, const uint32_t today)
	{
		df::explore_request request;
		request.today = today;

		for (const auto& item : items)
		{
			if (auto sample = df::explore_sample_of(item.file, item.path)) request.samples.push_back(*sample);
		}

		return df::build_explore_model(std::move(request), nullptr);
	}

	uint32_t count_listed(const std::vector<explore_test_item>& items, const df::search_t& search)
	{
		const df::search_matcher matcher(search);
		return static_cast<uint32_t>(std::ranges::count_if(items, [&matcher](const explore_test_item& item)
		{ return matcher.match_item(item.path, item.file).is_match(); }));
	}
}

// Every part of Explore states a count and opens a search, so the two are the same question asked twice: a
// week, a month, a year, a trip and a place in a year each list exactly the items they counted.
static void should_count_explore_parts_as_their_searches_list()
{
	std::vector<explore_test_item> items;
	using df::explore_day;

	// Home: New South Wales, a photo every third day for two years, and a video now and then.
	for (auto day = explore_day(2024, 1, 2); day <= explore_day(2025, 12, 28); day += 3)
	{
		add_explore_item(items, day % 5 == 0 ? "mp4" : "jpg", day, "Australia", "New South Wales");
	}

	// Abroad: a week in Croatia, four a day, one of them by a camera that recorded no place at all.
	for (auto day = explore_day(2025, 7, 5); day <= explore_day(2025, 7, 12); ++day)
	{
		for (auto i = 0; i < 4; ++i) add_explore_item(items, "jpg", day, i == 3 && day % 2 == 0 ? "" : "Croatia");
	}

	// A stray photo from Croatia a fortnight later is not a trip of its own, though it is of the year's places.
	add_explore_item(items, "jpg", explore_day(2025, 7, 26), "Croatia");

	// A long weekend in another state of home's country.
	for (auto day = explore_day(2025, 4, 18); day <= explore_day(2025, 4, 20); ++day)
	{
		for (auto i = 0; i < 3; ++i) add_explore_item(items, "jpg", day, "Australia", "Victoria");
	}

	// An older year, with years of nothing after it.
	add_explore_item(items, "jpg", explore_day(2021, 5, 1), "Australia", "New South Wales");

	const auto model = build_test_explore_model(items, explore_day(2025, 12, 31));

	assert_equal(2021, model.first_year, "the timeline starts with the first year holding anything", "explore");
	assert_equal(static_cast<uint32_t>(items.size()), model.total.total(), "and counts every item", "explore");
	assert_equal(2u, static_cast<uint32_t>(model.trips.size()), "two trips away from home", "explore");

	const auto& croatia = model.trips[0];
	assert_equal("Croatia", croatia.place.name, "the newest first, abroad", "explore");
	assert_equal(true, croatia.place.level == df::location_level::country, "labelled by its country", "explore");
	assert_equal(explore_day(2025, 7, 5), croatia.first_day, "from its first day", "explore");
	assert_equal(explore_day(2025, 7, 12), croatia.last_day, "to its last, not the stray photo after", "explore");

	const auto& victoria = model.trips[1];
	assert_equal("Victoria", victoria.place.name, "a state of home's country is away too", "explore");
	assert_equal(true, victoria.place.level == df::location_level::state, "labelled by the state", "explore");

	for (const auto& trip : model.trips)
	{
		assert_equal(trip.count, count_listed(items, df::explore_trip_search(trip)),
		             std::format("{}'s search lists what it counted", trip.place.name), "explore");
	}

	auto croatia_2025 = false;

	for (const auto& place : model.year_places)
	{
		croatia_2025 |= place.year == 2025 && place.place.name == "Croatia";
		assert_equal(place.count, count_listed(items, df::explore_year_place_search(place)),
		             std::format("{} in {} lists what it counted", place.place.name, place.year), "explore");
	}

	assert_equal(true, croatia_2025, "Croatia 2025 is one of the year's places", "explore");

	for (const auto& year : model.years)
	{
		assert_equal(year.counts.total(), count_listed(items, df::explore_year_search(year.year)),
		             std::format("{} lists what it counted", year.year), "explore");
	}

	for (const auto& month : model.months)
	{
		assert_equal(month.counts.total(), count_listed(items, df::explore_month_search(month)),
		             std::format("{}-{} lists what it counted", month.year, month.month), "explore");
	}

	auto weeks_with_items = 0;

	for (const auto& week : model.weeks)
	{
		assert_equal(true, week.first_day <= week.last_day && week.last_day - week.first_day < 7, "a week is a week",
		             "explore");
		assert_equal(df::explore_date(week.first_day).year, df::explore_date(week.last_day).year,
		             "cut to the year it falls in", "explore");
		assert_equal(week.counts.total(), count_listed(items, df::explore_days_search(week.first_day, week.last_day)),
		             "a week lists what it counted", "explore");
		if (!week.counts.is_empty()) ++weeks_with_items;
	}

	assert_equal(true, weeks_with_items > 100, "two years of weeks were counted", "explore");

	// 1 January 2025 was a Wednesday, so its week starts with the year rather than on the Monday before.
	const auto new_year = std::ranges::find(model.weeks, explore_day(2025, 1, 1), &df::explore_week::first_day);
	assert_equal(true, new_year != model.weeks.end(), "a week starts with the new year", "explore");
	assert_equal(explore_day(2025, 1, 5), new_year->last_day, "and ends on its Sunday", "explore");

	const auto videos = std::ranges::count_if(items, [](const explore_test_item& item)
	{ return item.file.ft->group == &file_group::video; });
	assert_equal(static_cast<uint32_t>(videos), model.total.videos, "videos are counted as videos", "explore");
}

// The globe gathers places into cells about a marker across at the scale it is seen at: zoomed out, a city and
// the next one share a marker; zoomed in, they part.
static void should_cluster_explore_places_by_scale()
{
	df::explore_request request;
	request.today = df::explore_day(2025, 12, 31);

	const auto add = [&request](const gps_coordinate where, const int n)
	{
		for (auto i = 0; i < n; ++i)
		{
			df::explore_sample sample;
			sample.day = df::explore_day(2025, 3, 1) + i;
			sample.kind = df::explore_kind::photo;
			sample.rank = 1;
			sample.coordinate = gps_coordinate(where.latitude() + i * 0.001, where.longitude());
			sample.path = core::file_path(std::format("c:\\places\\{}-{}.jpg", where.latitude(), i));
			request.samples.push_back(sample);
		}
	};

	add({-33.87, 151.21}, 5); // Sydney
	add({-34.42, 150.89}, 3); // Wollongong, 70 km south
	add({45.81, 15.98}, 2);   // Zagreb

	const auto model = df::build_explore_model(std::move(request), nullptr);
	assert_equal(10u, static_cast<uint32_t>(model.located.size()), "every located item is placed", "explore clusters");

	const auto far = df::cluster_explore_model(model, df::explore_cluster_level(300.0));
	const auto near = df::cluster_explore_model(model, df::explore_cluster_level(300.0 * 1024.0));

	assert_equal(true, near.level > far.level, "zooming in asks for finer cells", "explore clusters");
	assert_equal(2u, static_cast<uint32_t>(far.clusters.size()), "seen whole, Sydney and Wollongong are one place",
	             "explore clusters");
	assert_equal(3u, static_cast<uint32_t>(near.clusters.size()), "zoomed in, they part", "explore clusters");

	for (const auto* clusters : {&far, &near})
	{
		uint32_t total = 0;
		for (const auto& c : clusters->clusters) total += c.count;
		assert_equal(10u, total, "every item is in exactly one cluster", "explore clusters");

		std::vector<uint32_t> counted(clusters->clusters.size());
		for (const auto c : clusters->of_located) ++counted[c];
		for (size_t i = 0; i < counted.size(); ++i)
		{
			assert_equal(clusters->clusters[i].count, counted[i], "and each item says which", "explore clusters");
		}
	}
}

static void should_build_aggregate_location_matrix()
{
	location_matrix matrix({16, 44, -60.0, -30.0, 60.0, 170.0});
	const gps_coordinate prague1(50.0755, 14.4378);
	const gps_coordinate prague2(50.0756, 14.4379);
	const gps_coordinate sydney(-33.8688, 151.2093);
	const gps_coordinate excluded_by_bounds(64.1466, -21.9426);
	matrix.add(core::file_path("c:\\b.jpg"), prague2, true, 5);
	matrix.add(core::file_path("c:\\sydney.jpg"), sydney, true, 0);
	matrix.add(core::file_path("c:\\a.jpg"), prague1, true, 3);
	matrix.add(core::file_path("c:\\reykjavik.jpg"), excluded_by_bounds, true, 5);
	matrix.finalize();

	assert_equal(2_z, matrix.cells.size(), "only occupied in-bounds cells are stored", "location matrix");
	const auto prague = std::ranges::find(matrix.cells, matrix.params.cell(prague1), &location_matrix::cell::index);
	assert_equal(true, prague != matrix.cells.end(), "nearby items share a cell", "location matrix");
	assert_equal(2, static_cast<int>(prague->count), "cell retains the item count", "location matrix");
	assert_equal("b.jpg", prague->representative_path.name(), "high-rated media is representative", "location matrix");
	assert_equal(50.07555, prague->centroid.latitude(), "cell coordinate is the centroid", "location matrix");
	assert_equal(50.0755, prague->min_latitude, "cell retains minimum latitude", "location matrix");
	assert_equal(14.4379, prague->max_longitude, "cell retains maximum longitude", "location matrix");

	location_matrix ranked;
	ranked.add(core::file_path("c:\\0.txt"), prague1, false, 5);
	ranked.add(core::file_path("c:\\a.jpg"), prague1, true, 3);
	ranked.add(core::file_path("c:\\b.jpg"), prague1, true, 5);
	ranked.finalize();
	assert_equal("b.jpg", ranked.cells.front().representative_path.name(),
	             "rated visual media outranks non-visual and lower-rated items", "location matrix");

	location_matrix_params sidebar_params;
	sidebar_params.projection = location_matrix_projection::location_heat_map;
	sidebar_params.area_cell_span = 4;
	const auto heat_cell = df::location_heat_map::calc_map_loc(prague1);
	const auto sidebar_cell = sidebar_params.cell(prague1);
	assert_equal(heat_cell.x / 4 * 4, sidebar_cell.x, "fixed projection aligns horizontally", "location matrix");
	assert_equal(heat_cell.y / 4 * 4, sidebar_cell.y, "fixed projection aligns vertically", "location matrix");
}

static void should_select_thumbnail_representatives_while_counting()
{
	auto make_file = [](const std::string_view name, const int rating)
	{
		df::index_file_item result;
		result.name = core::cache(name);
		result.ft = files::file_type_from_name(name);
		const auto metadata = std::make_shared<prop::item_metadata>();
		metadata->rating = static_cast<int16_t>(rating);
		result.metadata.store(metadata);
		return result;
	};

	const auto text = make_file("0.txt", 5);
	const auto ordinary_photo = make_file("a.jpg", 3);
	const auto rated_photo = make_file("b.jpg", 5);
	df::file_group_histogram first;
	first.record(text, core::file_path("c:\\0.txt"));
	first.record(ordinary_photo, core::file_path("c:\\a.jpg"));
	assert_equal("a.jpg", first.representative_path.name(), "visual media outranks non-thumbnail files",
	             "summary thumbnail");

	df::file_group_histogram second;
	second.record(rated_photo, core::file_path("c:\\b.jpg"));
	first.add(second);
	assert_equal("b.jpg", first.representative_path.name(), "rated media survives histogram merge",
	             "summary thumbnail");

	df::date_histogram dates;
	dates.record_representative(0, ordinary_photo, core::file_path("c:\\a.jpg"));
	dates.record_representative(0, rated_photo, core::file_path("c:\\b.jpg"));
	assert_equal("b.jpg", dates.representative_paths[0].name(), "date bucket prioritises rated media",
	             "summary thumbnail");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Gazetteer lookup
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_find_location()
{
	auto& locations = test_locations();
	// The display language is shared state; every early return below would otherwise leak it.
	const core::scope_exit restore_language([&locations] { locations.set_display_language({}); });

	const auto default_location = gps_coordinate(51.5255317687988, -0.116743430495262); // London

	assert_equal("City of London", locations.find_by_id(2643741).place, "City");
	assert_equal(true, locations.find_by_id(2643741).population > 0.0, "City population");
	assert_equal("King of Prussia", locations.find_by_id(5196220).place, "City");
	assert_equal("London", locations.find_largest(51.3, -0.5, 51.7, 0.3).place,
	             "largest population center inside photo bounds");
	const auto boulder_city_coord = gps_coordinate(35.9786, -114.8325);
	assert_equal("Boulder City",
	             locations.find_closest(boulder_city_coord.latitude(), boulder_city_coord.longitude()).place,
	             "Boulder City reverse geocode");

	df::index_file_item boulder_city_file;
	boulder_city_file.ft = files::file_type_from_name("test.jpg");
	auto boulder_city_metadata = boulder_city_file.safe_ps();
	boulder_city_metadata->coordinate = boulder_city_coord;
	boulder_city_metadata->location_state = "Nevada"_c;
	boulder_city_metadata->location_country = "United States"_c;
	const auto boulder_city_search = df::search_t::parse("loc:\"Boulder City\"");
	const df::search_matcher boulder_city_matcher(boulder_city_search, core::utc_now().to_days(), &locations);
	assert_equal(true, boulder_city_matcher.match_item({}, boulder_city_file).is_match(),
	             "loc place uses map reverse geocode when stored place is empty");

	// docs/locations.md#location-search: a prediction is labelled at its qualification level, not always fully.
	assert_equal("London, United Kingdom", locations.auto_complete("london", 8, default_location)[0].location.str(),
	             "City");
	assert_equal("London, Canada", locations.auto_complete("london canada", 8, default_location)[0].location.str(),
	             "City");
	assert_equal("Armidale", locations.auto_complete("armid aust", 8, default_location)[0].location.str(), "City");
	assert_equal("Birmingham, United Kingdom",
	             locations.auto_complete("birm gb", 8, default_location)[0].location.str(), "City");
	assert_equal("King of Prussia", locations.auto_complete("king pru usa", 8, default_location)[0].location.str(),
	             "City");
	assert_equal("King of Prussia",
	             locations.auto_complete("king of prussia pennsylvania", 8, default_location)[0].location.str(),
	             "State");

	const auto czech_matches = locations.auto_complete("cz", 8, default_location);
	assert_equal(true, !czech_matches.empty(), "country code produces location predictions");
	assert_equal("Czechia"s, std::string(czech_matches.front().location.country), "CZ predicts Czechia");

	const auto short_local = locations.auto_complete("bi", 4, default_location);
	assert_equal(true,
	             std::ranges::any_of(short_local,
	                                 [](const location_match& match)
	{
		return core::icmp(match.location.place, "Birmingham") == 0 &&
		       core::icmp(match.location.country, "United Kingdom") == 0;
	}),
	             "short prefixes include local places by canonical country code");
	assert_equal(true, short_local.size() <= 4, "short completion honours the result cap");

	const auto short_nonlocal = locations.auto_complete("ar", 8, default_location);
	assert_equal(false,
	             std::ranges::any_of(short_nonlocal, [](const location_match& match)
	{ return core::icmp(match.location.place, "Armidale") == 0; }),
	             "short prefixes still suppress non-local place matches");

	// Issue #119: the displayed place and country names follow the selected UI language.
	locations.set_display_language("de");
	const auto munich_de = locations.find_by_id(2867714);
	assert_equal("München", munich_de.place, "German place");
	assert_equal("Deutschland", munich_de.country, "German country");
	const auto local_de = locations.auto_complete("bi", 4, default_location);
	assert_equal(true,
	             std::ranges::any_of(local_de,
	                                 [](const location_match& match)
	{
		return core::icmp(match.location.place, "Birmingham") == 0 &&
		       core::icmp(match.location.country, "Vereinigtes Königreich") == 0;
	}),
	             "localized display names do not break short local completion");
	locations.set_display_language("es");
	const auto munich_es = locations.find_by_id(2867714);
	assert_equal("Múnich", munich_es.place, "Spanish place");
	assert_equal("Alemania", munich_es.country, "Spanish country");
	const auto local_es = locations.auto_complete("bi", 4, default_location);
	assert_equal(true,
	             std::ranges::any_of(local_es,
	                                 [](const location_match& match)
	{
		return core::icmp(match.location.place, "Birmingham") == 0 &&
		       core::icmp(match.location.country, "Reino Unido") == 0;
	}),
	             "a second localized display language still completes local short prefixes");
	locations.set_display_language("zh");
	const auto munich_zh = locations.find_by_id(2867714);
	assert_equal("慕尼黑", munich_zh.place, "Chinese place");
	assert_equal("德国", munich_zh.country, "Chinese country");

	// Issue #119 regression guard: find_country() feeds the map/heat-map country grouping whose
	// label doubles as a SEARCH term (sidebar .with(name)). It must stay canonical (English)
	// regardless of the selected display language so a map click still matches the country name
	// stored in the index -- localizing it here previously broke map-click search in zh/de/etc.
	const auto grouping = locations.find_country(48.137, 11.575); // Munich, Germany
	assert_equal("Germany", grouping.name, "Country grouping key stays canonical while display is localized");
	country_loc combined_country;
	const auto combined_location = locations.find_closest(48.137, 11.575, &combined_country);
	assert_equal("慕尼黑", combined_location.place, "Combined lookup keeps localized place");
	assert_equal("Germany", combined_country.name, "Combined lookup keeps canonical country grouping key");

	locations.set_display_language("xx"); // unknown language falls back to the default names
	const auto munich_default = locations.find_by_id(2867714);
	assert_equal("Munich", munich_default.place, "Unknown language falls back");
	assert_equal("Germany", munich_default.country, "Default country");
	locations.set_display_language("en"); // English exonym equals the default name -> fallback
	assert_equal("Munich", locations.find_by_id(2867714).place, "English name");
}

static void should_return_no_place_for_empty_location_lookup()
{
	const location_cache locations;

	const auto closest = locations.find_closest(0.0, 0.0);
	assert_equal(true, core::is_empty(closest.place), "empty gazetteer has no closest place");
	assert_equal(0u, closest.id, "empty gazetteer has no closest id");

	const auto invalid = locations.find_closest(std::numeric_limits<double>::quiet_NaN(), 0.0);
	assert_equal(true, core::is_empty(invalid.place), "invalid lookup has no closest place");

	const auto country = locations.find_country(std::numeric_limits<double>::quiet_NaN(), 0.0);
	assert_equal(0u, country.code, "invalid country lookup has no country code");
}

static void should_find_places_across_the_antimeridian_and_poles()
{
	auto& locations = test_locations();

	const auto seam = locations.find_closest(66.323, 179.9);
	assert_equal("Egvekinot"s, std::string(seam.place.sv()), "nearest lookup wraps across the antimeridian");

	const auto attributed = locations.find_attributed(66.323, 179.9);
	assert_equal("Egvekinot"s, std::string(attributed.nearest.place.sv()),
	             "attribution starts from the wrapped nearest place");

	assert_equal(180.0, location_longitude_span_degrees(89.9, 100.0),
	             "a polar attribution radius conservatively spans every longitude");
	assert_equal(true, location_longitude_span_degrees(0.0, 100.0) < 1.0, "ordinary midlatitude bounds stay narrow");
}

// Records are read back through a mapping that load_index drops and rebuilds, and the index is
// appended to rather than assigned, so a reload is the case where both a stale mapping and a
// doubled index would first show up.
static void should_reload_location_index()
{
	auto& locations = test_locations();
	const core::scope_exit restore_language([&locations] { locations.set_display_language({}); });
	const auto default_location = gps_coordinate(51.5255317687988, -0.116743430495262); // London

	const auto before = locations.auto_complete("london", 8, default_location);
	assert_equal(true, !before.empty(), "predictions before reload");

	locations.load_index();

	assert_equal(true, locations.is_index_loaded(), "index is loaded again after a reload");

	// A record read after the reload proves the read-back mapping was re-established, not just
	// that the offsets survived.
	assert_equal("City of London", locations.find_by_id(2643741).place, "id lookup survives a reload");
	assert_equal("London", locations.find_largest(51.3, -0.5, 51.7, 0.3).place, "bounds lookup survives a reload");
	assert_equal("Boulder City", locations.find_closest(35.9786, -114.8325).place, "reverse geocode survives a reload");

	const auto after = locations.auto_complete("london", 8, default_location);
	assert_equal(before.size(), after.size(), "reload does not change the number of predictions");
	assert_equal("London, United Kingdom", after[0].location.str(), "reload keeps the top prediction");
}

#ifndef _DEBUG
static void should_find_closest_location()
{
	const auto& locations = test_locations();

	assert_equal("Bread Street", locations.find_closest(51.5142, -000.0985).place, "City");
	assert_equal("Armidale", locations.find_closest(-30.515, 151.665).place, "City");
	assert_equal("Johannesburg", locations.find_closest(-26.204444, 28.045556).place, "City");
	assert_equal("Santiago", locations.find_closest(-33.45, -70.666667).place, "City");
	assert_equal("Eastern Parkway", locations.find_closest(40.664167, -73.938611).place, "City");
	assert_equal("Beijing", locations.find_closest(39.913889, 116.391667).place, "City");
}
#endif

static void should_offset_localized_name()
{
	// Issue #119: bit unset (or no language selected) resolves to the default name (offset 0).
	assert_equal(0, location_localized_name_offset(0u, 2), "no bits set");
	assert_equal(0, location_localized_name_offset(0b0010u, 2), "requested bit unset");
	assert_equal(0, location_localized_name_offset(0xFFFFFFFFu, -1), "no language selected");
	assert_equal(0, location_localized_name_offset(0xFFFFFFFFu, 32), "bit out of range");

	// Only the requested bit set -> first localized name (offset 1).
	assert_equal(1, location_localized_name_offset(0b0001u, 0), "first bit");
	assert_equal(1, location_localized_name_offset(0b0100u, 2), "single higher bit");

	// Lower set bits push the localized name further along, one column per earlier name.
	assert_equal(2, location_localized_name_offset(0b0101u, 2), "one lower bit");
	assert_equal(3, location_localized_name_offset(0b0111u, 2), "two lower bits");

	// Highest bit works and counts all lower set bits.
	assert_equal(1, location_localized_name_offset(1u << 31, 31), "top bit alone");
	assert_equal(2, location_localized_name_offset((1u << 31) | 1u, 31), "top bit with one lower");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Place naming and attribution
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_read_place_qualification_level()
{
	// docs/locations.md#place-data: the flags column carries the smallest name form that identifies
	// the place. Levels are assigned by tools/generate_locations.py over the emitted records.
	auto& locations = test_locations();

	const auto level = [&locations](const std::string_view query)
	{
		return static_cast<int>(locations.find_by_name(query).qualification());
	};

	// A name held by exactly one record needs no qualifier.
	assert_equal(0, level("Reykjavík"), "unique name is level 0");
	assert_equal(0, level("Ouagadougou"), "unique name is level 0");

	// London collides across countries but is unique within each, so the country is enough.
	assert_equal(1, level("London"), "name unique within its country is level 1");
	assert_equal(1, level("London, Canada"), "the qualifier does not change the level");

	// Springfield and Toronto collide inside one country, so the region is also needed.
	assert_equal(2, level("Springfield"), "name repeated within a country is level 2");
	assert_equal(2, level("Toronto"), "name repeated within a country is level 2");

	// Populated places are never extent features; bits 3-31 are reserved and read as zero.
	assert_equal(false, locations.find_by_name("London").is_extent(), "a place is not an extent feature");
	assert_equal(false, locations.find_by_name("Reykjavík").is_extent(), "a place is not an extent feature");

	// The reserved level encoding 3 degrades to level 2, so a newer file over-qualifies
	// rather than mislabels.
	location_t reserved;
	reserved.flags = 3u;
	assert_equal(2, static_cast<int>(reserved.qualification()), "reserved level reads as level 2");

	location_t extent;
	extent.flags = location_flag_extent;
	assert_equal(0, static_cast<int>(extent.qualification()), "the extent bit does not disturb the level");
	assert_equal(true, extent.is_extent(), "the extent bit is readable");
}

static void should_compose_qualified_place_name()
{
	// docs/locations.md#place-data: one composer, driven by the qualification level, using the country's
	// short common form.
	auto& locations = test_locations();

	const auto name = [&locations](const std::string_view query)
	{
		return qualified_name(locations.find_by_name(query));
	};

	// Level 0 carries no qualifier at all.
	assert_equal("Reykjavík"s, name("Reykjavík"), "level 0 is the bare name");
	assert_equal("King of Prussia"s, name("King of Prussia"), "level 0 is the bare name");

	// Level 1 adds the country and nothing else, so the region is omitted even though it is known.
	assert_equal("London, United Kingdom"s, name("London"), "level 1 is name and country");
	assert_equal("London, Canada"s, name("London, Canada"), "level 1 is name and country");
	assert_equal("Birmingham, United Kingdom"s, name("Birmingham, United Kingdom"), "level 1 is name and country");

	// The short common form, not the long official one.
	assert_equal("United Kingdom"s, std::string(locations.find_by_name("London").country.sv()),
	             "the country is the short common form");

	// Level 2 needs the region to separate same-country namesakes.
	assert_equal("Reno, Nevada, United States"s, name("Reno, Nevada"), "level 2 is name, region and country");

	// A repeated part is dropped and the level is treated as satisfied.
	location_t city_state;
	city_state.place = "Singapore"_c;
	city_state.state = "Singapore"_c;
	city_state.country = "Singapore"_c;
	city_state.flags = 2u;
	assert_equal("Singapore"s, qualified_name(city_state), "a city-state does not repeat itself");

	location_t place_is_region;
	place_is_region.place = "Luxembourg"_c;
	place_is_region.state = "Luxembourg"_c;
	place_is_region.country = "Luxembourg"_c;
	place_is_region.flags = 2u;
	assert_equal("Luxembourg"s, qualified_name(place_is_region), "a place equal to its region does not repeat it");

	// A record with no place name still labels itself.
	location_t country_only;
	country_only.country = "Portugal"_c;
	assert_equal("Portugal"s, qualified_name(country_only), "a country hit labels itself");

	assert_equal(""s, qualified_name(location_t{}), "an empty location has no label");
}

static void should_bound_place_attribution()
{
	// docs/locations.md#item-locations: a place may label an item only within a radius scaled to its
	// significance, so a photo taken far from anywhere is not confidently mislabelled.
	assert_equal(100.0, location_attribution_radius_km(8961989.0), "a megacity reaches 100km");
	assert_equal(100.0, location_attribution_radius_km(1000000.0), "the 1M boundary is inclusive");
	assert_equal(50.0, location_attribution_radius_km(999999.0), "just below 1M reaches 50km");
	assert_equal(25.0, location_attribution_radius_km(10000.0), "the 10k boundary is inclusive");
	assert_equal(15.0, location_attribution_radius_km(1000.0), "the 1k boundary is inclusive");
	assert_equal(10.0, location_attribution_radius_km(0.0), "an unknown population reaches 10km");
	assert_equal(300.0, location_max_attribution_km, "no place ever reaches beyond three max radii");

	auto& locations = test_locations();

	// Step 2: the nearest place is close enough to stand for the item.
	const auto london = locations.find_attributed(51.5142, -0.0985);
	assert_equal(static_cast<int>(location_attribution::at), static_cast<int>(london.attribution),
	             "a city centre is At its place");
	assert_equal("Bread Street", london.place.place, "At keeps the nearest place");

	const auto singapore = locations.find_attributed(1.291985, 103.866511);
	assert_equal(static_cast<int>(location_attribution::at), static_cast<int>(singapore.attribution),
	             "a Singapore photo is At its nearest feature");
	assert_equal("Marina Bay", singapore.place.place, "item attribution keeps the nearest feature");
	assert_equal("Singapore"s, qualified_name(locations.find_largest_attributed(1.291985, 103.866511)),
	             "a map cluster prefers the recognizable city");

	// Step 3: outside its radius but within three, on land. It is still located, and it still
	// carries the place identity so rural photography groups instead of shattering.
	const auto outback = locations.find_attributed(-25.153, 131.75);
	assert_equal(static_cast<int>(location_attribution::near), static_cast<int>(outback.attribution),
	             "20km from a hamlet is Near it");
	assert_equal("Curtin Springs", outback.place.place, "Near keeps the place identity");
	assert_equal(true, outback.is_located(), "Near is located");

	// Step 5: land, nothing within three radii. The country is still a true answer.
	const auto sahara = locations.find_attributed(23.0, 15.0);
	assert_equal(static_cast<int>(location_attribution::remote), static_cast<int>(sahara.attribution),
	             "220km from a small town is Remote");
	assert_equal(true, core::is_empty(sahara.place.place), "Remote names no place");
	assert_equal("Libya", sahara.place.country, "Remote still names a country when it can");
	assert_equal("Libya"s, qualified_name(sahara.place), "a Remote label is the country alone");

	const auto iceland = locations.find_attributed(64.85, -18.6);
	assert_equal(static_cast<int>(location_attribution::remote), static_cast<int>(iceland.attribution),
	             "96km from a 19k town is Remote");
	assert_equal("Iceland", iceland.place.country, "Remote still names a country when it can");

	// Issue #119: the out-param country stays canonical because it doubles as a search term.
	{
		locations.set_display_language("de");
		const core::scope_exit restore_language([&locations] { locations.set_display_language({}); });
		country_loc canonical;
		const auto munich_area = locations.find_attributed(48.137, 11.575, &canonical);
		assert_equal("Deutschland", munich_area.place.country, "the displayed country is localized");
		assert_equal("Germany", canonical.name, "the search country stays canonical");
	}

	// find_closest stays unbounded because the bearing descriptor needs the nearest place at any distance,
	// but it must no longer be what labels an item.
	const auto atlantic = locations.find_attributed(35.0, -45.0);
	assert_equal(static_cast<int>(location_attribution::remote), static_cast<int>(atlantic.attribution),
	             "mid-ocean is Remote");
	assert_equal(true, core::is_empty(atlantic.place.country), "mid-ocean claims no country");
	assert_equal(""s, qualified_name(atlantic.place), "mid-ocean has no place label");
	assert_equal(false, core::is_empty(locations.find_closest(35.0, -45.0).place),
	             "find_closest stays unbounded for the bearing descriptor");

	// An item with no coordinates is not located at all.
	assert_equal(static_cast<int>(location_attribution::none),
	             static_cast<int>(locations.find_attributed(gps_coordinate{}).attribution),
	             "no coordinate is not located");
}

static void should_describe_bearing()
{
	// docs/locations.md#item-locations: every item resolved Near a place or Remote also gets a secondary descriptor
	// computed from the nearest place regardless of radius. It is never a group key and never a
	// search term, so it exists only to answer "where was that?".
	const auto point = [](const double degrees)
	{
		return static_cast<int>(location_bearing_from_degrees(degrees));
	};

	assert_equal(static_cast<int>(location_bearing::north), point(0.0), "0 degrees is north");
	assert_equal(static_cast<int>(location_bearing::north), point(22.4), "just under 22.5 is still north");
	assert_equal(static_cast<int>(location_bearing::north_east), point(22.5), "22.5 rounds up to north-east");
	assert_equal(static_cast<int>(location_bearing::east), point(90.0), "90 degrees is east");
	assert_equal(static_cast<int>(location_bearing::south_east), point(135.0), "135 degrees is south-east");
	assert_equal(static_cast<int>(location_bearing::south), point(180.0), "180 degrees is south");
	assert_equal(static_cast<int>(location_bearing::south_west), point(225.0), "225 degrees is south-west");
	assert_equal(static_cast<int>(location_bearing::west), point(270.0), "270 degrees is west");
	assert_equal(static_cast<int>(location_bearing::north_west), point(315.0), "315 degrees is north-west");
	assert_equal(static_cast<int>(location_bearing::north), point(350.0), "350 degrees wraps back to north");
	assert_equal(static_cast<int>(location_bearing::north_west), point(-45.0), "a negative bearing wraps");
	assert_equal(static_cast<int>(location_bearing::north), point(720.0), "a bearing beyond one turn wraps");

	// The angle runs from the named place towards the item, because the descriptor reads as a
	// direction a user would give: the item is north-west *of* the place.
	const gps_coordinate origin(10.0, 10.0);
	const auto from_origin = [origin](const gps_coordinate to)
	{
		return static_cast<int>(location_bearing_from_degrees(location_bearing_degrees(origin, to)));
	};

	assert_equal(static_cast<int>(location_bearing::north), from_origin(gps_coordinate(11.0, 10.0)), "due north");
	assert_equal(static_cast<int>(location_bearing::south), from_origin(gps_coordinate(9.0, 10.0)), "due south");
	assert_equal(static_cast<int>(location_bearing::east), from_origin(gps_coordinate(10.0, 11.0)), "due east");
	assert_equal(static_cast<int>(location_bearing::west), from_origin(gps_coordinate(10.0, 9.0)), "due west");
	assert_equal(static_cast<int>(location_bearing::north_west), from_origin(gps_coordinate(11.0, 9.0)), "north-west");
	assert_equal(static_cast<int>(location_bearing::south_east), from_origin(gps_coordinate(9.0, 11.0)), "south-east");

	// The composed string, pinned without depending on which gazetteer record wins.
	located_place composed;
	composed.attribution = location_attribution::remote;
	composed.nearest.place = "Lisbon"_c;
	composed.nearest.country = "Portugal"_c;
	composed.nearest.flags = 1u;
	composed.nearest_km = 410.0;
	composed.nearest_bearing = location_bearing::north_west;
	assert_equal("410 km NW of Lisbon, Portugal"s, bearing_descriptor(composed),
	             "the descriptor is distance, compass point and qualified place");

	composed.nearest_bearing = location_bearing::south;
	composed.nearest_km = 0.4;
	assert_equal("400 m S of Lisbon, Portugal"s, bearing_descriptor(composed),
	             "the descriptor shares the distance format the user types");

	// An item that is At its place is already answered by the place name.
	composed.attribution = location_attribution::at;
	assert_equal(""s, bearing_descriptor(composed), "an item At its place carries no bearing");
	assert_equal(""s, bearing_descriptor({}), "an unlocated item carries no bearing");

	auto& locations = test_locations();

	// find_closest stays unbounded precisely so a Remote item can still say where it was.
	const auto atlantic = locations.find_attributed(35.0, -45.0);
	const auto atlantic_text = bearing_descriptor(atlantic);
	assert_equal(false, core::is_empty(atlantic.nearest.place), "Remote still resolves a nearest place");
	assert_equal(true, atlantic.nearest_km > location_max_attribution_km,
	             "the nearest place is beyond every attribution radius");
	assert_equal(true, atlantic_text.find(qualified_name(atlantic.nearest)) != std::string::npos,
	             "the descriptor names the nearest place regardless of radius");

	const auto outback = locations.find_attributed(-25.153, 131.75);
	assert_equal(false, bearing_descriptor(outback).empty(), "a Near item carries a bearing");

	// Step 2 keeps the nearest record too, so nothing downstream has to resolve it a second time.
	const auto london = locations.find_attributed(51.5142, -0.0985);
	assert_equal("Bread Street", london.nearest.place, "At records the nearest place as well");
	assert_equal(""s, bearing_descriptor(london), "an item At its place carries no bearing");
}

// Attribution starts from the nearest place, and it has to be the same nearest place find_closest
// names. Searching raw degrees there while find_closest weighed longitude by latitude meant that far
// from the equator an item was labelled after somewhere half as close as a place it was actually
// nearer to. The grid spans high latitudes, where a degree of longitude is least like a degree of
// latitude; against the unweighted search it finds disagreements.
static void should_attribute_from_the_place_find_closest_names()
{
	const auto& locations = test_locations();
	auto compared = 0;
	auto disagreements = 0;

	for (auto latitude = 56.0; latitude <= 70.0; latitude += 0.5)
	{
		for (auto longitude = 5.0; longitude <= 60.0; longitude += 1.5)
		{
			const auto attributed = locations.find_attributed(latitude, longitude);
			const auto closest = locations.find_closest(latitude, longitude);

			if (attributed.nearest.id != closest.id) ++disagreements;
			++compared;
		}
	}

	assert_equal(true, compared > 1000, "the grid covers a wide band of high latitudes");
	assert_equal(0, disagreements, "attribution starts from the place find_closest names");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Location search
///////////////////////////////////////////////////////////////////////////////////////////////////

// docs/design.md#system-states-and-consistency: a location query that finds nothing must explain itself and
// offer the next action. Each action is a normal search, so Back reverses it like any other navigation.
static void should_recover_from_an_empty_location_search()
{
	const auto search = df::search_t::parse("loc:\"London, Canada, 2km\"");
	const auto* const term = search.single_place_term();

	assert_equal(true, term != nullptr, "the empty state only speaks for a single place term");
	assert_equal("London, Canada", term->text, "the resolved centre is what the message names");
	assert_equal(2.0, term->float_val, "and the radius it searched");

	// Widen to the next detent -- the single most likely fix, one click.
	const auto wider = location_distance_at_detent(location_nearest_distance_detent(term->float_val) + 1);
	assert_equal(5.0, wider, "2 km widens to 5 km");

	auto widened = search;
	widened.set_place_distance(wider);
	assert_equal("loc:London, Canada, 5km", widened.format_terms(), "widening keeps the place");
	assert_equal(location_distance_at_detent(std::size(location_distance_detents_km) - 1),
	             location_distance_at_detent(std::size(location_distance_detents_km)),
	             "the widest detent has nothing above it to offer");

	// Drop the qualifier when the name resolved to the wrong namesake.
	auto all_named = search;
	all_named.set_place_name("London");
	assert_equal("loc:London, 2km", all_named.format_terms(), "dropping the qualifier keeps the radius");

	// Never let a user mistake "not placed" for "not present".
	auto unplaced = search;
	unplaced.clear_terms();
	unplaced.without(df::search_term_type::has_location);
	assert_equal(true, unplaced.single_place_term() == nullptr, "the location term is gone");

	const auto has_no_location = [&unplaced](const gps_coordinate coord, const core::cached stored_place)
	{
		df::index_file_item file;
		file.ft = files::file_type_from_name("test.jpg");
		const auto md = file.safe_ps();
		md->coordinate = coord;
		md->location_place = stored_place;
		file.calc_search_presence();

		const df::search_matcher matcher(unplaced);
		return matcher.match_item({}, file).is_match();
	};

	assert_equal(true, has_no_location({}, {}), "neither text nor coordinates");
	assert_equal(false, has_no_location({51.5142, -0.0985}, {}), "coordinates are a location");
	assert_equal(false, has_no_location({}, "London"_c), "stored text is a location");
}

// docs/locations.md#location-search: a location group header must reproduce its own group.
static void should_reproduce_a_location_group_from_its_header()
{
	// The link the header builds for `London` inside `England, United Kingdom`.
	auto search =
		df::search_t().location("London", df::location_level::place).location("England", df::location_level::state);

	const auto matches = [&search](const core::cached place, const core::cached state)
	{
		df::index_file_item file;
		file.ft = files::file_type_from_name("test.jpg");
		const auto md = file.safe_ps();
		md->location_place = place;
		md->location_state = state;
		file.calc_search_presence();

		const df::search_matcher matcher(search);
		return matcher.match_item({}, file).is_match();
	};

	assert_equal(true, matches("London"_c, "England"_c), "the group's own items come back");
	assert_equal(false, matches("London"_c, "Ontario"_c), "the other London does not");
	assert_equal(false, matches("Bristol"_c, "England"_c), "nor the rest of the region");
}

// Height classes are not built, so their words name nothing yet: each is searched for as typed, as any `@` word that
// names nothing is, rather than dropped and widening the search to everything.
static void should_search_unbuilt_height_classes_as_typed()
{
	assert_equal("\"@flying\""s, df::search_t::parse("@flying").format_terms(), "flying is searched as typed");
	assert_equal("\"@altitude\""s, df::search_t::parse("@altitude").format_terms(), "altitude is searched as typed");
	assert_equal("\"@underwater\""s, df::search_t::parse("@underwater").format_terms(),
	             "underwater is searched as typed");
}

// docs/locations.md#location-search: the distance slider rewrites the radius of the one location term it controls.
// The detent ladder and the term it applies to are both pure, so both are pinned here.
static void should_step_search_distance()
{
	assert_equal(0.1, location_distance_at_detent(0), "the smallest detent");
	assert_equal(100.0, location_distance_at_detent(location_distance_detent_count - 1), "the largest detent");
	assert_equal(0.1, location_distance_at_detent(-5), "below the ladder clamps");
	assert_equal(100.0, location_distance_at_detent(99), "above the ladder clamps");

	assert_equal(0, location_nearest_distance_detent(0.0), "no radius sits at the smallest detent");
	assert_equal(3, location_nearest_distance_detent(1.0), "an exact detent is itself");
	assert_equal(4, location_nearest_distance_detent(2.0), "an exact detent is itself");
	assert_equal(3, location_nearest_distance_detent(1.4), "1.4 km is nearer 1 km on a ratio scale");
	assert_equal(4, location_nearest_distance_detent(1.5), "1.5 km is nearer 2 km on a ratio scale");
	assert_equal(4, location_nearest_distance_detent(3.0), "3 km is nearer 2 km than 5 km");
	assert_equal(5, location_nearest_distance_detent(3.5), "3.5 km is nearer 5 km");
	assert_equal(9, location_nearest_distance_detent(1000.0), "beyond the ladder clamps");

	assert_equal(3, location_distance_detent_at_least(1.0), "an exact detent needs no widening");
	assert_equal(5, location_distance_detent_at_least(3.0), "3 km rounds up to 5 km, never down");
	assert_equal(9, location_distance_detent_at_least(1000.0), "beyond the ladder clamps");

	assert_equal("100 m", format_distance_km(0.1), "below a kilometre reads in metres");
	assert_equal("250 m", format_distance_km(0.25), "below a kilometre reads in metres");
	assert_equal("1 km", format_distance_km(1.0), "a whole kilometre drops the fraction");
	assert_equal("2.5 km", format_distance_km(2.5), "a small fraction is kept");
	assert_equal("100 km", format_distance_km(100.0), "the widest detent");

	const auto place = df::search_t::parse("loc:\"London, 2km\"");
	assert_equal(true, place.single_place_term() != nullptr, "one named place offers a radius");
	assert_equal(2.0, place.single_place_term()->float_val, "the parsed radius in km");

	assert_equal(true, df::search_t::parse("loc:London").single_place_term() != nullptr,
	             "a place with no radius still offers one");
	assert_equal(true, df::search_t::parse("country:France").single_place_term() != nullptr,
	             "a level-constrained place is still a place");
	assert_equal(false, df::search_t::parse("loc:+51.5-0.09+10").single_place_term() != nullptr,
	             "a coordinate the user typed is not widened for them");
	assert_equal(false, df::search_t::parse("loc:London loc:Paris").single_place_term() != nullptr,
	             "two places have no single radius");
	assert_equal(false, df::search_t::parse("@remote").single_place_term() != nullptr,
	             "a built-in class takes no distance");
	assert_equal(false, df::search_t::parse("cat").single_place_term() != nullptr,
	             "a search with no location has no slider");

	auto widened = df::search_t::parse("loc:\"London, 2km\"");
	widened.set_place_distance(10.0);
	assert_equal("loc:London, 10km", widened.format_terms(), "widening rewrites the canonical term");

	auto added = df::search_t::parse("loc:London");
	added.set_place_distance(0.5);
	assert_equal("loc:London, 500m", added.format_terms(), "a radius can be added to a bare place");
}

// A coordinate is signed numbers from the start, as ISO 6709 writes one. A place with two hyphens in its
// name was read as two numbers - a coordinate of 0, 0 - and found nothing but photos at that point.
static void should_read_only_signed_numbers_as_a_coordinate()
{
	const auto iso = split_location("+47.6062-122.3321+056.000/");
	assert_equal(true, iso.success, "ISO 6709 with an altitude");
	assert_equal(true, std::abs(iso.x - 47.6062) < 1e-9 && std::abs(iso.y + 122.3321) < 1e-9, "its position");
	assert_equal(true, split_location("+37.3317-122.0306/").success, "ISO 6709 without one");
	assert_equal(true, split_location("-30.515+151.66+5").success, "a coordinate typed into a search");
	assert_equal(false, split_location("Stratford-upon-Avon").success, "a place with two hyphens");
	assert_equal(false, split_location("Aix-en-Provence, France").success, "named with its country");
	assert_equal(false, split_location("-upon-Avon").success, "words after a sign");

	assert_equal(true, df::search_t::parse("loc:Stratford-upon-Avon").single_place_term() != nullptr,
	             "searched as a place");
	assert_equal(true, df::search_t::parse("near:Aix-en-Provence").single_place_term() != nullptr, "near: too");
}

// docs/locations.md#location-search: every location scope spelling is one entry in df::location_scopes, and
// completing, parsing and writing a query all read it - so a spelling added there works everywhere at its level.
static void should_read_every_location_scope_from_one_table()
{
	for (const auto& scope : df::location_scopes)
	{
		const auto query = std::format("{}:Paris", scope.name);

		const auto completion = df::classify_search_scope(query);
		assert_equal(true, completion.kind == df::search_scope_kind::location,
		             std::format("{} completes as a location", scope.name));
		assert_equal(true, completion.level == scope.level, std::format("{} completes at its level", scope.name));

		const auto search = df::search_t::parse(query);
		assert_equal(1_z, search.terms().size(), std::format("{} parses to one term", scope.name));
		assert_equal(true, search.terms().front().type == df::search_term_type::location,
		             std::format("{} parses to a location term", scope.name));
		assert_equal(true, search.terms().front().level == scope.level,
		             std::format("{} parses at its level", scope.name));
		assert_equal(std::format("{}:Paris", df::location_scope_name(scope.level)), search.format_terms(),
		             std::format("{} is written back in its level's spelling", scope.name));
	}
}

static void should_search_gps_faces_and_metadata_as_independent_words()
{
	const auto& locations = test_locations();
	const auto folder = test_temps.next_folder("gps-word-search");
	df::index_item_infos items(4);
	for (size_t i = 0; i < items.size(); ++i)
	{
		items[i].name = core::cache(std::format("{}.jpg", i));
		items[i].ft = files::file_type_from_name(items[i].name);
	}
	// A GPS-only London photo with a suggested face, a text-only match, and each word alone.
	items[0].safe_ps()->coordinate = gps_coordinate(51.5142, -0.0985);
	items[1].safe_ps()->title = "London trip"_c;
	items[1].safe_ps()->tags = "zac"_c;
	items[2].safe_ps()->coordinate = gps_coordinate(51.5142, -0.0985);
	items[3].safe_ps()->title = "Zac's trip"_c;
	const auto node = std::make_shared<df::index_folder_item>(std::move(items));
	node->is_in_collection = true;
	const df::search_index::folder_list folders{{folder, node}};
	const auto index = df::search_index::build(folders, locations, 1, [] { return false; });
	df::search_index_evaluator evaluator(index, folders);

	const auto overview = std::make_shared<faces::overview>();
	overview->people = {{1, "Zac Walker"}};
	overview->picture_people[folder.combine_file("0.jpg")] = {{0, false}};
	const auto previous = faces::active_overview();
	const core::scope_exit restore([previous] { faces::set_active_overview(previous); });
	faces::set_active_overview(overview);

	const auto check = [&](const std::string_view query, const std::array<bool, 4> expected)
	{
		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search, core::utc_now().to_days(), &locations);
		const auto docs = evaluator.evaluate(matcher, test_token);
		for (uint32_t i = 0; i < expected.size(); ++i)
		{
			assert_equal(expected[i], matcher.match_all_terms(folder.text(), node->files[i]).is_match(),
			             std::format("scan: {}, item {}", query, i));
			assert_equal(expected[i], docs.test(i), std::format("index: {}, item {}", query, i));
		}
	};
	check("london zac", {true, true, false, false});
	check("zac -london", {false, false, false, true});
	check("london zac!", {false, false, false, false});
}

static void should_resolve_location_vocabulary()
{
	auto& locations = test_locations();

	const auto boulder_city_coord = gps_coordinate(35.9786, -114.8325);

	// A GPS-only item: no stored place, region or country field at all.
	df::index_file_item gps_only;
	gps_only.ft = files::file_type_from_name("test.jpg");
	gps_only.safe_ps()->coordinate = boulder_city_coord;

	// An item with stored text but no coordinates.
	df::index_file_item text_only;
	text_only.ft = files::file_type_from_name("test.jpg");
	const auto text_only_metadata = text_only.safe_ps();
	text_only_metadata->location_place = "Boulder City"_c;
	text_only_metadata->location_state = "Nevada"_c;
	text_only_metadata->location_country = "United States"_c;

	const auto matches = [&locations](const std::string_view query, const df::index_file_item& file)
	{
		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search, core::utc_now().to_days(), &locations);
		return matcher.match_item({}, file).is_match();
	};

	// The defect this fixes: the most guessable spelling used to read the raw field only.
	assert_equal(true, matches("place:\"Boulder City\"", gps_only), "place resolves a location for a GPS-only item");
	assert_equal(true, matches("city:\"Boulder City\"", gps_only), "city is a synonym of place");
	assert_equal(true, matches("state:Nevada", gps_only), "state resolves for a GPS-only item");
	assert_equal(true, matches("country:\"United States\"", gps_only), "country resolves for a GPS-only item");
	assert_equal(true, matches("countries:\"United States\"", gps_only), "countries is a synonym of country");
	assert_equal(true, matches("near:\"Boulder City\"", gps_only), "near is a synonym of loc");

	// Level constraints: the point of keeping the narrower spellings.
	assert_equal(false, matches("state:\"Boulder City\"", gps_only), "a place does not match at region level");
	assert_equal(false, matches("place:Nevada", gps_only), "a region does not match at place level");
	assert_equal(false, matches("place:\"United States\"", gps_only), "a country does not match at place level");
	assert_equal(true, matches("loc:Nevada", gps_only), "loc matches at any level");

	// Stored text is authoritative and needs no coordinates.
	assert_equal(true, matches("place:\"Boulder City\"", text_only), "stored place matches without coordinates");
	assert_equal(true, matches("country:\"United States\"", text_only), "stored country matches without coordinates");
	assert_equal(false, matches("place:Reno", text_only), "a different place does not match");

	// Negation still composes.
	assert_equal(false, matches("-place:\"Boulder City\"", gps_only), "negated location term excludes the item");
	assert_equal(true, matches("-place:Reno", gps_only), "negated non-matching location term keeps the item");

	// docs/locations.md#location-search: a completion commits the gazetteer's qualified name, which is labelled at
	// the record's own level. London omits its region; the index still knows the item as England.
	df::index_file_item london;
	london.ft = files::file_type_from_name("test.jpg");
	london.safe_ps()->coordinate = gps_coordinate(51.5142, -0.0985);

	assert_equal("loc:\"London, United Kingdom\"",
	             df::search_t().location("London, United Kingdom", df::location_level::any).format_terms(),
	             "the term the address bar commits for a London completion");
	assert_equal(true, matches("loc:\"London, United Kingdom\"", london),
	             "a name qualified to its country matches an item qualified to its region");
	assert_equal(true, matches("loc:London", london), "the bare name still matches");
	assert_equal(false, matches("loc:\"London, Ontario\"", london), "and the other London still does not");
	assert_equal(false, matches("loc:\"Reno, United States\"", london), "nor a qualifier that holds without the place");

	// The reach stops at the record that named the item, so a town near a metropolis keeps its own
	// identity rather than being swallowed by it.
	df::index_file_item windsor;
	windsor.ft = files::file_type_from_name("test.jpg");
	windsor.safe_ps()->coordinate = gps_coordinate(51.483, -0.6);

	assert_equal(true, matches("loc:Windsor", windsor), "the town answers for itself");
	assert_equal(false, matches("loc:\"London, United Kingdom\"", windsor), "35 km away is not in London");
}

static void should_search_suburbs_by_their_enclosing_city()
{
	const auto& locations = test_locations();
	const auto folder = test_temps.next_folder("search-city-parent");
	df::index_item_infos items(5);
	for (size_t i = 0; i < items.size(); ++i)
	{
		items[i].name = core::cache(std::format("{}.jpg", i));
		items[i].ft = files::file_type_from_name(items[i].name);
		items[i].safe_ps()->people = "Zac"_c;
	}
	items[0].safe_ps()->coordinate = {-27.480, 153.020}; // South Brisbane, GPS only.
	items[1].safe_ps()->location_place = "Indooroopilly"_c; // No GPS and no "Brisbane" in the name.
	items[1].safe_ps()->location_country = "Australia"_c;
	items[2].safe_ps()->coordinate = {-27.623, 152.760}; // Ipswich is an independent town.
	items[3].safe_ps()->location_place = "Gold Coast"_c;
	items[4].safe_ps()->location_place = "Greenwich"_c; // Ambiguous without a region.
	const auto node = std::make_shared<df::index_folder_item>(std::move(items));
	node->is_in_collection = true;
	const df::search_index::folder_list folders{{folder, node}};
	const auto index = df::search_index::build(folders, locations, 1, [] { return false; });
	df::search_index_evaluator evaluator(index, folders);
	for (const std::string_view query : {"Zac Brisbane", "zac loc:\"Brisbane, Australia\""})
	{
		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search, core::utc_now().to_days(), &locations);
		const auto docs = evaluator.evaluate(matcher, test_token);
		for (uint32_t i = 0; i < node->files.size(); ++i)
		{
			assert_equal(i < 2, matcher.match_all_terms(folder.text(), node->files[i]).is_match(),
			             std::format("scan: {}, item {}", query, i));
			assert_equal(i < 2, docs.test(i), std::format("index: {}, item {}", query, i));
		}
	}
	assert_equal(0u, locations.find_enclosing_city("Greenwich"), "ambiguous names do not gain a city");
}

// docs/locations.md#location-search: typing a bare place name must offer the canonical location term. Before
// this, the prediction committed the plain qualified name, which ran a text search over stored
// place fields and found nothing for the (typical) GPS-only photo.
static void should_complete_locations_as_search_terms()
{
	null_async_strategy as;
	const auto& locations = test_locations();
	const index_state index(as, locations);

	const auto london = index.auto_complete_locations("london", 5);
	assert_equal(true, !london.empty(), "london predicts a location");
	assert_equal("loc:\"London, United Kingdom\""s, london.front().text, "bare name predicts the canonical term");
	assert_equal(true, !london.front().highlights.empty(), "typed fragment is highlighted");

	// The prediction must round-trip: committing it has to produce a location term, not text.
	const auto parsed = df::search_t::parse(london.front().text);
	assert_equal(1, static_cast<int>(parsed.terms().size()), "prediction is a single term");
	assert_equal(true, parsed.terms().front().type == df::search_term_type::location,
	             "prediction commits a location term");

	// A level-qualified scope labels the place at that level and keeps the scope prefix.
	const auto states = index.auto_complete_locations("cali", 5, df::location_level::state);
	assert_equal(true, !states.empty(), "state scope predicts states");
	assert_equal(true, core::starts(states.front().text, "state:"), "state scope keeps its prefix");

	const auto countries = index.auto_complete_locations("franc", 5, df::location_level::country);
	assert_equal(true, !countries.empty(), "country scope predicts countries");
	assert_equal("country:France"s, countries.front().text, "country scope labels at country level");

	// Predictions are distinct; the gazetteer holds many rows per named place.
	const auto many = index.auto_complete_locations("lond", 8);
	core::hash_set<std::string, core::ihash, core::ieq> seen;
	for (const auto& m : many) assert_equal(true, seen.emplace(m.text).second, "predictions are distinct");

	assert_equal(true, index.auto_complete_locations("", 5).empty(), "empty query predicts nothing");
}

static void should_match_location_radius_and_presence()
{
	auto& locations = test_locations();

	const auto boulder_city_coord = gps_coordinate(35.9786, -114.8325);

	df::index_file_item gps_only;
	gps_only.ft = files::file_type_from_name("test.jpg");
	gps_only.safe_ps()->coordinate = boulder_city_coord;

	df::index_file_item text_only;
	text_only.ft = files::file_type_from_name("test.jpg");
	const auto text_only_metadata = text_only.safe_ps();
	text_only_metadata->location_place = "Boulder City"_c;
	text_only_metadata->location_state = "Nevada"_c;
	text_only_metadata->location_country = "United States"_c;

	df::index_file_item unlocated;
	unlocated.ft = files::file_type_from_name("test.jpg");
	unlocated.safe_ps()->title = "no location here"_c;

	const auto matches = [&locations](const std::string_view query, const df::index_file_item& file)
	{
		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search, core::utc_now().to_days(), &locations);
		return matcher.match_item({}, file).is_match();
	};

	// A trailing distance component is a radius, not part of the name.
	const auto is_km = [](const double actual, const double expected)
	{
		return std::fabs(actual - expected) < 0.0001;
	};

	const auto radius_search = df::search_t::parse("loc:\"Boulder City, 10km\"");
	assert_equal(1, static_cast<int>(radius_search.terms().size()), "radius query is one term");
	assert_equal("Boulder City", std::string(radius_search.terms()[0].text), "distance is stripped from the name");
	assert_equal(true, is_km(radius_search.terms()[0].float_val, 10.0), "distance parsed as kilometres");

	assert_equal(true, is_km(df::search_t::parse("loc:\"Boulder City, 1000m\"").terms()[0].float_val, 1.0),
	             "metres convert to kilometres");
	assert_equal(true, is_km(df::search_t::parse("loc:\"Boulder City, 1mi\"").terms()[0].float_val, 1.609344),
	             "miles convert to kilometres");
	assert_equal(true, is_km(df::search_t::parse("loc:\"Boulder City, 1 miles\"").terms()[0].float_val, 1.609344),
	             "the plural unit is accepted");

	// A component that does not parse completely stays part of the name.
	assert_equal("Boulder City, 10 furlongs",
	             std::string(df::search_t::parse("loc:\"Boulder City, 10 furlongs\"").terms()[0].text),
	             "an unrecognised unit is not a distance");
	assert_equal("Nevada, United States",
	             std::string(df::search_t::parse("loc:\"Nevada, United States\"").terms()[0].text),
	             "an ordinary qualifier is not a distance");

	// Radius matching needs coordinates; stored text alone can never satisfy one.
	assert_equal(true, matches("loc:\"Boulder City, 10km\"", gps_only), "an item inside the radius matches");
	assert_equal(false, matches("loc:\"Boulder City, 1km\"", text_only),
	             "stored text without coordinates never satisfies a radius");
	assert_equal(true, matches("loc:\"Boulder City\"", text_only), "the same query without a radius still matches");
	assert_equal(false, matches("loc:\"Reno, 10km\"", gps_only), "an item outside the radius does not match");

	// A bare name resolves to its canonical record: exact name, largest population
	// (docs/locations.md#location-search).
	const auto reno = locations.find_by_name("Reno");
	assert_equal("Reno/United States", std::string(reno.place.sv()) + "/" + std::string(reno.country.sv()),
	             "a bare name resolves to its canonical record");
	assert_equal("Reno/United States",
	             std::string(locations.find_by_name("Reno, Nevada").place.sv()) + "/" +
	                 std::string(locations.find_by_name("Reno, Nevada").country.sv()),
	             "a region qualifier selects the same record");
	assert_equal(true, core::is_empty(locations.find_by_name("Reno, France").place),
	             "a qualifier that does not hold resolves nothing");
	assert_equal(true, matches("loc:\"Reno, 800km\"", gps_only), "a wide enough radius reaches the item");

	// A region or country has extent, not a centre, so it takes no radius.
	assert_equal("Nevada, 10km", std::string(df::search_t::parse("state:\"Nevada, 10km\"").terms()[0].text),
	             "state does not take a distance");

	// without:location is about having no location at all, not about the stored field.
	assert_equal(true, matches("without:location", unlocated), "an item with neither text nor coordinates matches");
	assert_equal(false, matches("without:location", gps_only), "coordinates alone count as a location");
	assert_equal(false, matches("without:location", text_only), "stored text alone counts as a location");
	assert_equal(true, matches("with:location", gps_only), "with:location is the complement");
	assert_equal(false, matches("with:location", unlocated), "with:location excludes the unlocated item");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Map areas
///////////////////////////////////////////////////////////////////////////////////////////////////

// docs/locations.md#areas-become-places-with-a-radius: a map area resolves to a place plus a radius. `area:` was
// an internal heat-map concept leaking into the user's address bar; "within 25 km of London" is not.
static void should_open_a_map_area_as_a_place_and_radius()
{
	map_location_area area;
	area.name = "London, United Kingdom";
	area.position = gps_coordinate(51.5, -0.1);
	area.place_position = gps_coordinate(51.5142, -0.0985);
	area.min_latitude = 51.4;
	area.max_latitude = 51.6;
	area.min_longitude = -0.2;
	area.max_longitude = 0.0;

	const auto km = area.search_radius_km();
	const auto furthest = area.place_position.distance_in_kilometers(gps_coordinate(51.4, 0.0));

	assert_equal(true, km >= furthest, "the radius never drops an item the area contained");
	assert_equal(true, location_distance_at_detent(location_distance_detent_at_least(furthest) - 1) < furthest,
	             "and is the smallest detent that covers it");

	// A degenerate area still gets a usable radius rather than a zero one that matches nothing.
	map_location_area point;
	point.position = point.place_position = gps_coordinate(51.5, -0.1);
	point.min_latitude = point.max_latitude = 51.5;
	point.min_longitude = point.max_longitude = -0.1;
	assert_equal(location_distance_at_detent(0), point.search_radius_km(),
	             "a degenerate area uses the smallest detent");

	// A zoomed-out area is wider than the widest detent. Rounding it down to the top of the ladder
	// dropped every item the area showed beyond 100 km, which is the one thing the radius promises
	// not to do. The slider clamps at its widest; the search covers what was displayed.
	map_location_area wide;
	wide.position = wide.place_position = gps_coordinate(0.0, 0.0);
	wide.min_latitude = -10.0;
	wide.max_latitude = 10.0;
	wide.min_longitude = -10.0;
	wide.max_longitude = 10.0;

	const auto wide_furthest = wide.place_position.distance_in_kilometers(gps_coordinate(10.0, 10.0));
	assert_equal(true, wide_furthest > location_distance_at_detent(location_distance_detent_count - 1),
	             "the fixture is wider than the ladder");
	assert_equal(true, wide.search_radius_km() >= wide_furthest,
	             "a radius past the widest detent still covers the area");

	// What the click opens: a resolved place term the distance slider can then widen.
	auto named = df::search_t();
	named.location(area.name, df::location_level::any).set_place_distance(km);

	const auto* const term = named.single_place_term();
	assert_equal(true, term != nullptr, "the map produces a place term the slider can drive");
	assert_equal("London, United Kingdom", term->text, "qualified, so the right London is searched");
	assert_equal(km, term->float_val, "carrying the area's radius");
	assert_equal(25.0, km, "these bounds round up to the 25 km detent");
	assert_equal("loc:\"London, United Kingdom, 25km\"", named.format_terms(),
	             "and canonicalizes to a loc: term, never an area: term");

	// Nothing near enough to name still opens exactly what was clicked, by coordinate.
	auto unnamed = df::search_t();
	unnamed.location(area.position, km);
	assert_equal(true, unnamed.single_place_term() == nullptr, "a coordinate term names no place");
	assert_equal(true, unnamed.has_term_type(df::search_term_type::location), "but is still a location search");
}

static void should_contain_map_location_cells()
{
	const map_location_area area{.cell = {32, 48}, .cell_span = 4};
	assert_equal(true, area.contains({32, 48}));
	assert_equal(true, area.contains({35, 51}));
	assert_equal(false, area.contains({36, 48}));
	assert_equal(false, area.contains({32, 52}));
}

static void should_scale_map_location_cells()
{
	assert_equal(16, map_location_cell_span(0, 220), "default span before map layout");
	assert_equal(8, map_location_cell_span(256, 220), "world map uses regional areas");
	assert_equal(4, map_location_cell_span(85, 220), "regional crop uses local areas");
	assert_equal(2, map_location_cell_span(45, 220), "tight crop exposes finer areas");
	assert_equal(1, map_location_cell_span(20, 220), "closest crop uses individual cells");
}

static void should_average_map_location_coordinates()
{
	index_histograms histograms;
	constexpr auto map_width = static_cast<int>(df::location_heat_map::map_width);
	constexpr auto first = 48 * map_width + 32;
	constexpr auto second = 49 * map_width + 33;
	histograms._locations.coordinates[first] = 2;
	histograms._location_latitude_sums[first] = 20.0;
	histograms._location_longitude_sums[first] = 40.0;
	histograms._location_min_latitudes[first] = 8.0;
	histograms._location_min_longitudes[first] = 18.0;
	histograms._location_max_latitudes[first] = 12.0;
	histograms._location_max_longitudes[first] = 22.0;
	histograms._locations.coordinates[second] = 1;
	histograms._location_latitude_sums[second] = 40.0;
	histograms._location_longitude_sums[second] = 80.0;
	histograms._location_min_latitudes[second] = 40.0;
	histograms._location_min_longitudes[second] = 80.0;
	histograms._location_max_latitudes[second] = 40.0;
	histograms._location_max_longitudes[second] = 80.0;

	const auto areas = histograms.map_locations(4);
	assert_equal(1_z, areas.size(), "cells fold into one map area");
	assert_equal(3u, areas.front().count, "area contains all photos");
	assert_equal(20.0, areas.front().position.latitude(), "latitude is weighted by photo count");
	assert_equal(40.0, areas.front().position.longitude(), "longitude is weighted by photo count");
	assert_equal(8.0, areas.front().min_latitude, "area tracks southern photo bound");
	assert_equal(18.0, areas.front().min_longitude, "area tracks western photo bound");
	assert_equal(40.0, areas.front().max_latitude, "area tracks northern photo bound");
	assert_equal(80.0, areas.front().max_longitude, "area tracks eastern photo bound");
}

static void should_resolve_named_map_area_on_demand()
{
	const auto& locations = test_locations();
	index_histograms histograms;
	const gps_coordinate munich(48.137, 11.575);
	const auto cell = df::location_heat_map::calc_map_loc(munich);
	const auto map_index = cell.y * df::location_heat_map::map_width + cell.x;
	histograms._locations.coordinates[map_index] = 1;
	histograms._location_latitude_sums[map_index] = munich.latitude();
	histograms._location_longitude_sums[map_index] = munich.longitude();
	histograms._location_min_latitudes[map_index] = 48.0;
	histograms._location_min_longitudes[map_index] = 11.4;
	histograms._location_max_latitudes[map_index] = 48.3;
	histograms._location_max_longitudes[map_index] = 11.8;

	const auto area = histograms.find_map_location("Munich", locations, munich);
	assert_equal(true, area.has_value(), "name-only area is reconstructed from histogram");
	assert_equal("Munich", area->name, "reconstructed area keeps its population-center name");
	assert_equal(1, area->cell_span, "on-demand resolution selects the finest matching bucket");
	assert_equal(true, area->contains(cell), "reconstructed area contains the named center");
	auto search = df::search_t::parse("area:Munich");
	search.resolve_area(*area);
	assert_equal(1, search.terms().front().location_cell_span, "saved area address receives geometry");
	assert_equal(true, cell == search.terms().front().location_cell, "saved area resolves to the photo bucket");
}

static void should_parse_map_area()
{
	const map_location_area area{.name = "Brisbane", .cell = {32, 48}, .cell_span = 4};
	auto search = df::search_t();
	search.area(area);
	assert_equal("area:Brisbane", search.format_terms(), "formatted map area");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Visits and timeline
///////////////////////////////////////////////////////////////////////////////////////////////////

static df::visit_sample visit_sample_at(const int y, const int m, const int d, const double lat, const double lon,
                                        const std::string_view place)
{
	df::visit_sample s;
	s.days = core::date_t(y, m, d).to_days();
	s.coordinate = gps_coordinate(lat, lon);
	if (!place.empty()) s.place = core::cache(place);
	return s;
}

static void should_derive_visits_from_a_result_set()
{
	const location_cache locations;
	df::visit_request request;

	// Two separated trips plus a run at home, all far enough apart in space to cluster apart.
	for (auto d = 1; d <= 10; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2019, 6, d, 35.68, 139.69, "Tokyo"));
	}

	for (auto d = 1; d <= 8; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2021, 3, d, -33.87, 151.21, "Sydney"));
	}

	for (auto d = 1; d <= 9; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2023, 9, d, 51.51, -0.13, "London"));
	}

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(27u, timeline.located_count, "located count");
	assert_equal(0u, timeline.undated_count, "undated count");
	assert_equal(3u, static_cast<uint32_t>(timeline.nodes.size()), "node count");
	assert_equal(true, timeline.publish, "publishes a timeline");

	// docs/locations.md#visits: chronological order after selection, never score order.
	assert_equal("Tokyo", timeline.nodes[0].name, "first node");
	assert_equal("Sydney", timeline.nodes[1].name, "second node");
	assert_equal("London", timeline.nodes[2].name, "third node");
	assert_equal(true, timeline.nodes[0].first < timeline.nodes[1].first, "nodes ordered by date");
	assert_equal(10u, timeline.nodes[0].count, "first node count");

	// docs/locations.md#the-places-row: the same clustering counted by place.
	assert_equal(3u, static_cast<uint32_t>(timeline.places.size()), "place tallies");
	assert_equal("Tokyo", timeline.places[0].name, "largest place first");
	assert_equal(10u, timeline.places[0].count, "largest place count");
}

// A visit centre used to be the arithmetic mean of latitude and longitude, which has a seam: a trip
// straddling the antimeridian averaged +179 and -179 to 0 and put the visit off West Africa, from
// where its covering radius reached half the planet.
static void should_centre_a_visit_across_the_antimeridian()
{
	const location_cache locations;
	df::visit_request request;

	for (auto d = 1; d <= 6; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2022, 5, d, -16.9, 179.9, "Taveuni"));
		request.samples.emplace_back(visit_sample_at(2022, 5, d + 6, -16.9, -179.9, "Taveuni"));
	}

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(1u, static_cast<uint32_t>(timeline.nodes.size()),
	             std::format("one visit either side of the line (located {}, nodes {})", timeline.located_count,
	                         timeline.nodes.size()));

	const auto centre = timeline.nodes.front().centre;
	assert_equal(true, centre.is_valid(),
	             std::format("the visit has a centre ({}, {})", centre.latitude(), centre.longitude()));
	assert_equal(true, std::abs(centre.longitude()) > 179.0,
	             std::format("the centre stays on the meridian the samples straddle ({}, {})", centre.latitude(),
	                         centre.longitude()));
	assert_equal(true, std::abs(centre.latitude() + 16.9) < 0.5, "and keeps the latitude it was given");

	// The radius covers the members, so a centre on the wrong side of the planet would show up as a
	// radius of thousands of kilometres rather than a few.
	assert_equal(true, timeline.nodes.front().radius_km < 100.0, "the covering radius is local, not half the planet");
}

// A text-only visit is keyed by the same identity the place tallies use. Keying on the place name
// alone merged namesakes into one trip: two Springfields in different states are not one visit, and
// a timeline that says they are cannot be reproduced by the search the node promises.
static void should_tell_text_only_namesakes_apart()
{
	const location_cache locations;
	df::visit_request request;

	const auto sample_at =
		[](const int year, const int month, const int day, const std::string_view place, const std::string_view state)
	{
		df::visit_sample s;
		s.days = core::date_t(year, month, day, 12, 0, 0).to_days();
		s.place = core::cache(place);
		s.state = core::cache(state);
		s.country = core::cache("United States");
		return s;
	};

	for (auto d = 1; d <= 8; ++d)
	{
		request.samples.emplace_back(sample_at(2021, 4, d, "Springfield", "Illinois"));
	}

	for (auto d = 1; d <= 8; ++d)
	{
		request.samples.emplace_back(sample_at(2023, 9, d, "Springfield", "Massachusetts"));
	}

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(2u, static_cast<uint32_t>(timeline.nodes.size()), "two Springfields are two visits");
	assert_equal(2u, static_cast<uint32_t>(timeline.places.size()), "and two places");
	assert_equal(8u, timeline.nodes.front().count, "neither absorbed the other");
	assert_equal(8u, timeline.nodes.back().count, "and both kept their own items");

	// The node count alone does not discriminate: one cluster spanning both years would split at the
	// gap into two nodes regardless. It is the cluster identity that says the two places were told
	// apart in the first place.
	assert_equal(false, timeline.nodes.front().cluster == timeline.nodes.back().cluster,
	             "and they are two clusters, not one split at the gap");
}

static void should_split_a_cluster_at_long_gaps()
{
	const location_cache locations;
	df::visit_request request;

	// The same place visited in two summers three years apart is two visits, not one long one.
	for (auto d = 1; d <= 12; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2018, 7, d, 45.44, 12.34, "Venice"));
		request.samples.emplace_back(visit_sample_at(2021, 7, d, 45.44, 12.34, "Venice"));
	}

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(2u, static_cast<uint32_t>(timeline.nodes.size()), "one node per summer");
	assert_equal(2018, timeline.nodes[0].first.year(), "first visit year");
	assert_equal(2021, timeline.nodes[1].first.year(), "second visit year");
	assert_equal(1u, static_cast<uint32_t>(timeline.places.size()), "one place");
	assert_equal(24u, timeline.places[0].count, "place holds both visits");
}

static void should_exclude_items_that_cannot_sit_on_a_timeline()
{
	const location_cache locations;
	df::visit_request request;

	for (auto d = 1; d <= 8; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2020, 5, d, 48.85, 2.35, "Paris"));
		request.samples.emplace_back(visit_sample_at(2022, 1, d, 40.71, -74.01, "New York"));
	}

	// A date with no location, and a location with no date, are counted rather than invented
	// into a node (docs/locations.md#visits).
	auto dated_only = visit_sample_at(2020, 5, 20, 0.0, 0.0, {});
	dated_only.coordinate = {};
	request.samples.emplace_back(dated_only);

	auto located_only = visit_sample_at(2020, 5, 21, 48.85, 2.35, "Paris");
	located_only.days = 0;
	request.samples.emplace_back(located_only);

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(16u, timeline.located_count, "located count excludes both");
	assert_equal(1u, timeline.undated_count, "undated count");
	assert_equal(1u, timeline.unlocated_count, "unlocated count");
	assert_equal(2u, static_cast<uint32_t>(timeline.nodes.size()), "node count");
}

// A state or province places a photograph as surely as a town does. Leaving it out of the test for
// "has a location" counted an item described only by its state as unlocated, so the summary
// understated how much of the result is placed - while the place breakdown, which keys on the same
// fields, still listed it.
static void should_count_a_state_only_sample_as_located()
{
	const location_cache locations;
	df::visit_request request;

	for (auto d = 1; d <= 8; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2020, 5, d, 48.85, 2.35, "Paris"));
	}

	// Described by its state alone: no coordinate, no town, no country.
	auto state_only = visit_sample_at(2020, 5, 20, 0.0, 0.0, {});
	state_only.coordinate = {};
	state_only.state = "Bavaria"_c;
	request.samples.emplace_back(state_only);

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(0u, timeline.unlocated_count, "a state-only sample is not unlocated");
	assert_equal(9u, timeline.located_count, "it is counted among the located");
}

static df::visit_sample text_place_sample(const int day, const char* place, const char* state, const char* country)
{
	auto s = visit_sample_at(2022, 4, day, 0.0, 0.0, place);
	s.coordinate = {};
	s.state = core::cache(state);
	s.country = core::cache(country);
	return s;
}

static uint32_t count_matching_place_chip(const location_cache& locations, const df::visit_request& request,
                                          const df::visit_place_tally& place)
{
	const auto search = df::visit_place_search(df::search_t(), place);
	const df::search_matcher matcher(search, core::utc_now().to_days(), &locations);
	auto count = 0u;

	for (const auto& s : request.samples)
	{
		df::index_file_item file;
		file.ft = files::file_type_from_name("test.jpg");
		const auto md = file.safe_ps();
		md->coordinate = s.coordinate;
		md->location_place = s.place;
		md->location_state = s.state;
		md->location_country = s.country;
		file.calc_search_presence();

		if (matcher.match_item({}, file).is_match()) ++count;
	}

	return count;
}

static bool place_chip_matches_sample(const df::visit_place_tally& place, const df::visit_sample& sample)
{
	const auto matches = [](const core::cached chip, const core::cached value)
	{
		return is_empty(chip) || (!is_empty(value) && core::icmp(chip, value) == 0);
	};

	return matches(place.place, sample.place) && matches(place.state, sample.state) &&
	       matches(place.country, sample.country);
}

static uint32_t count_matching_place_chip_direct(const df::visit_request& request, const df::visit_place_tally& place)
{
	auto count = 0u;

	for (const auto& sample : request.samples)
	{
		if (place_chip_matches_sample(place, sample)) ++count;
	}

	return count;
}

static void assert_place_chips_match_direct_predicates(const df::visit_request& request,
                                                       const df::visit_timeline& timeline,
                                                       const std::string_view context)
{
	for (const auto& place : timeline.places)
	{
		assert_equal(place.count, count_matching_place_chip_direct(request, place),
		             std::format("{}: chip predicate returns its count", context));
	}
}

static void should_fold_place_breakdown_identity_case()
{
	const location_cache locations;
	df::visit_request request;

	request.samples.emplace_back(text_place_sample(1, "Paris", "Ile-de-France", "France"));
	request.samples.emplace_back(text_place_sample(2, "paris", "ile-de-france", "france"));
	request.samples.emplace_back(text_place_sample(3, "PARIS", "ILE-DE-FRANCE", "FRANCE"));

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(1u, static_cast<uint32_t>(timeline.places.size()), "case variants form one identity");
	assert_equal(3u, timeline.places[0].count, "case variants aggregate");
	assert_equal(3u, count_matching_place_chip(locations, request, timeline.places[0]),
	             "the case-folded chip search returns its count");
}

static void should_merge_overlapping_partial_place_breakdowns()
{
	const location_cache locations;

	const auto run_permutation = [&locations](const std::array<int, 3>& order)
	{
		const std::array samples = {
			text_place_sample(1, "Paris", "", "France"),
			text_place_sample(2, "", "Ile-de-France", "France"),
			text_place_sample(3, "Paris", "Ile-de-France", "France"),
		};

		df::visit_request request;
		for (const auto i : order) request.samples.emplace_back(samples[i]);

		const auto timeline = df::compute_visits(request, locations);
		assert_equal(1u, static_cast<uint32_t>(timeline.places.size()),
		             "overlapping partial identities merge to one predicate");
		assert_equal("France"s, timeline.places[0].name, "the common predicate is deterministic");
		assert_equal(3u, timeline.places[0].count, "the merged chip counts all overlapping items");
		assert_equal(3u, count_matching_place_chip(locations, request, timeline.places[0]),
		             "the merged chip search returns its count");
	};

	run_permutation({0, 1, 2});
	run_permutation({0, 2, 1});
	run_permutation({1, 0, 2});
	run_permutation({1, 2, 0});
	run_permutation({2, 0, 1});
	run_permutation({2, 1, 0});
}

static void should_drop_unbounded_partial_place_breakdowns()
{
	const location_cache locations;

	struct place_group
	{
		const char* place = "";
		const char* state = "";
		const char* country = "";
		uint32_t count = 0;
	};

	const std::array groups = {
		place_group{"Paris", "", "", 1},
		place_group{"", "", "France", 1},
		place_group{"Paris", "Ile-de-France", "France", 1},
		place_group{"Lyon", "Auvergne-Rhone-Alpes", "France", 5},
		place_group{"Nice", "Provence-Alpes-Cote d'Azur", "France", 7},
	};

	std::array<int, groups.size()> order = {0, 1, 2, 3, 4};
	auto permutations = 0u;

	do
	{
		df::visit_request request;
		auto day = 1;

		for (const auto group_index : order)
		{
			const auto& group = groups[group_index];

			for (auto i = 0u; i < group.count; ++i)
			{
				request.samples.emplace_back(text_place_sample(day++, group.place, group.state, group.country));
			}
		}

		const auto timeline = df::compute_visits(request, locations);
		assert_place_chips_match_direct_predicates(request, timeline, "permuted partial place identities");
		++permutations;
	} while (std::next_permutation(order.begin(), order.end()));

	assert_equal(120u, permutations, "all group-order permutations were checked");
}

static void should_keep_the_largest_partial_place_candidate()
{
	const location_cache locations;
	df::visit_request request;
	auto day = 1;

	for (auto i = 0; i < 1000; ++i)
	{
		request.samples.emplace_back(text_place_sample(day++, "", "", "France"));
	}

	request.samples.emplace_back(text_place_sample(day++, "Paris", "", ""));
	request.samples.emplace_back(text_place_sample(day++, "Paris", "Ile-de-France", "France"));

	for (auto i = 0; i < 5; ++i)
	{
		request.samples.emplace_back(text_place_sample(day++, "Lyon", "Auvergne-Rhone-Alpes", "France"));
	}

	const auto timeline = df::compute_visits(request, locations);
	assert_equal(true, !timeline.places.empty(), "the row keeps a visible place chip");
	assert_place_chips_match_direct_predicates(request, timeline, "largest partial place candidate");
	assert_equal("France"s, timeline.places.front().name, "the largest valid predicate survives");
	assert_equal(1006u, timeline.places.front().count, "the largest predicate keeps its covered items");
}

static void should_keep_random_place_breakdown_predicates_authoritative()
{
	const location_cache locations;
	const std::array<const char*, 4> places = {"Paris", "Lyon", "Nice", "Rouen"};
	const std::array<const char*, 4> states = {"Ile-de-France", "Auvergne-Rhone-Alpes", "Provence-Alpes-Cote d'Azur",
	                                           "Normandy"};
	const std::array<const char*, 2> countries = {"France", "Germany"};
	auto rng = 0x6008u;
	const auto next = [&rng]
	{
		rng = rng * 1664525u + 1013904223u;
		return rng;
	};

	for (auto iteration = 0; iteration < 200; ++iteration)
	{
		df::visit_request request;

		for (auto i = 0; i < 18; ++i)
		{
			const auto place_index = static_cast<size_t>(next() % places.size());
			const auto country_index = static_cast<size_t>(next() % countries.size());
			auto mask = next() % 7u + 1u;
			if ((mask & 4u) == 0 && (next() % 3u) == 0) mask |= 4u;

			request.samples.emplace_back(text_place_sample(i + 1, (mask & 1u) != 0 ? places[place_index] : "",
			                                               (mask & 2u) != 0 ? states[place_index] : "",
			                                               (mask & 4u) != 0 ? countries[country_index] : ""));
		}

		const auto timeline = df::compute_visits(request, locations);
		assert_place_chips_match_direct_predicates(request, timeline,
		                                           std::format("random partial identity set {}", iteration));
	}
}

static void should_bound_place_breakdown_collapse_work()
{
	const location_cache locations;

	const auto run = [&locations](const int distinct_places)
	{
		df::visit_request request;
		df::visit_work_counters counters;
		request.counters = &counters;
		request.samples.emplace_back(text_place_sample(1, "", "", "France"));

		for (auto i = 0; i < distinct_places; ++i)
		{
			const auto place = std::format("Place{:04}", i);
			const auto state = std::format("State{:04}", i);
			request.samples.emplace_back(text_place_sample(i + 2, place.c_str(), state.c_str(), "France"));
		}

		const auto timeline = df::compute_visits(request, locations);
		assert_place_chips_match_direct_predicates(request, timeline, "bounded collapse work");
		return counters.place_predicate_unions;
	};

	const auto small = run(64);
	const auto medium = run(128);
	const auto large = run(256);

	assert_equal(true, medium <= small * 3, "doubling identities keeps bounded work near linear");
	assert_equal(true, large <= medium * 3, "doubling again keeps bounded work near linear");
}

static void should_cancel_place_breakdown_derivation()
{
	const location_cache locations;
	std::atomic_bool cancelled = true;
	df::visit_request request{core::cancel_token(cancelled)};

	for (auto i = 0; i < 512; ++i)
	{
		request.samples.emplace_back(text_place_sample(i + 1, "Paris", "Ile-de-France", "France"));
	}

	const auto timeline = df::compute_visits(request, locations);
	assert_equal(0u, timeline.located_count, "cancelled visit derivation stops before tallying places");
	assert_equal(0u, static_cast<uint32_t>(timeline.places.size()), "cancelled visit derivation publishes no chips");
}

// docs/locations.md#the-places-row: a chip states a count and then runs a search. They are one promise, so the
// search has to return exactly what the chip counted -- whatever placed those items.
static void should_reproduce_a_place_breakdown_from_its_chip()
{
	auto& locations = test_locations();

	df::visit_request request;

	const auto stored = [](df::visit_sample s)
	{
		s.state = "Nevada"_c;
		s.country = "United States"_c;
		return s;
	};

	// A place whose items carry stored text, some of them with no coordinates at all.
	for (auto d = 1; d <= 10; ++d)
	{
		request.samples.emplace_back(
			stored(visit_sample_at(2019, 6, d, 35.9786 + d * 0.002, -114.8325 + d * 0.002, "Boulder City")));
	}

	for (auto d = 1; d <= 5; ++d)
	{
		auto s = stored(visit_sample_at(2019, 6, d, 0.0, 0.0, "Boulder City"));
		s.coordinate = {};
		request.samples.emplace_back(s);
	}

	// An undated item is still one of the results, and the chip's query carries no date.
	auto undated = stored(visit_sample_at(2019, 6, 1, 35.9786, -114.8325, "Boulder City"));
	undated.days = 0;
	request.samples.emplace_back(undated);

	// A place named only by attribution. It sits beside a much larger neighbour, close enough that
	// any circle drawn around it swallows that neighbour whole -- the case that made a chip read 5
	// and then return 560.
	for (auto d = 1; d <= 60; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2021, 3, 1 + d % 28, 35.2325, 139.1069, {}));
	}

	for (auto d = 1; d <= 5; ++d)
	{
		request.samples.emplace_back(visit_sample_at(2021, 4, d, 35.0955, 138.8634, {}));
	}

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(3u, static_cast<uint32_t>(timeline.places.size()), "three places");
	assert_equal(60u, timeline.places[0].count, "the large neighbour");
	assert_equal(16u, timeline.places[1].count, "an undated item is still in the place it was taken");
	assert_equal(5u, timeline.places[2].count, "the small place is not swallowed by the large one");

	// Every item that has a place is in exactly one entry, including the undated one, because the
	// chip's query carries no date either.
	auto tallied = 0u;
	for (const auto& place : timeline.places) tallied += place.count;
	assert_equal(81u, tallied, "the breakdown partitions the items that have a place");

	const auto count_matches = [&locations, &request](const df::search_t& search)
	{
		auto count = 0u;
		const df::search_matcher matcher(search, core::utc_now().to_days(), &locations);

		for (const auto& s : request.samples)
		{
			df::index_file_item file;
			file.ft = files::file_type_from_name("test.jpg");
			const auto md = file.safe_ps();
			md->coordinate = s.coordinate;
			md->location_place = s.place;
			md->location_state = s.state;
			md->location_country = s.country;
			file.calc_search_presence();

			if (matcher.match_item({}, file).is_match()) ++count;
		}

		return count;
	};

	for (const auto& place : timeline.places)
	{
		const auto search = df::visit_place_search(df::search_t(), place);
		assert_equal(place.count, count_matches(search), "the chip's search returns the count it displayed");
	}
}

// docs/locations.md#the-places-row: two chips that read the same are one affordance the user cannot choose from,
// and a chip that omits a field runs a query that already returns the items a sharper chip counted.
static void should_tell_two_place_chips_apart()
{
	const location_cache locations;

	const auto sample = [](const int day, const char* place, const char* state, const char* country)
	{
		auto s = visit_sample_at(2022, 4, day, 0.0, 0.0, place);
		s.coordinate = {};
		s.state = core::cache(state);
		s.country = core::cache(country);
		return s;
	};

	df::visit_request request;

	// One place recorded three ways: the region GeoNames no longer names, the region a user typed,
	// and no region at all. The vaguest chip's query returns all three, so it is the only honest one.
	for (auto d = 1; d <= 9; ++d) request.samples.emplace_back(sample(d, "Singapore", "", "Singapore"));
	for (auto d = 1; d <= 4; ++d) request.samples.emplace_back(sample(d, "Singapore", "Singapore", "Singapore"));
	for (auto d = 1; d <= 2; ++d)
	{
		request.samples.emplace_back(sample(d, "Singapore", "Singapore (general)", "Singapore"));
	}

	// Two genuinely different places that share a name. Neither omits what the other names, so
	// both survive and both have to say which one they are.
	for (auto d = 1; d <= 8; ++d) request.samples.emplace_back(sample(d, "London", "England", "United Kingdom"));
	for (auto d = 1; d <= 3; ++d) request.samples.emplace_back(sample(d, "London", "Ontario", "Canada"));

	const auto timeline = df::compute_visits(request, locations);

	assert_equal(3u, static_cast<uint32_t>(timeline.places.size()), "three distinguishable places");
	assert_equal("Singapore"s, timeline.places[0].name, "one chip for one place");
	assert_equal(15u, timeline.places[0].count, "the vaguest chip absorbs what its query returns");
	assert_equal("London, United Kingdom"s, timeline.places[1].name, "a shared name is qualified");
	assert_equal("London, Canada"s, timeline.places[2].name, "and so is the place it collided with");
	assert_equal(8u, timeline.places[1].count, "the larger London");
	assert_equal(3u, timeline.places[2].count, "the smaller London");
}

static void should_suppress_an_era_until_the_query_names_it()
{
	df::visit_request request;

	// Five years of steady photos from one place is a residence, not a trip.
	for (auto y = 2015; y <= 2020; ++y)
	{
		for (auto m = 1; m <= 12; ++m)
		{
			for (auto d = 1; d <= 6; ++d)
			{
				request.samples.emplace_back(visit_sample_at(y, m, d, 51.51, -0.13, "London"));
			}
		}
	}

	{
		const location_cache locations;
		const auto suppressed = df::compute_visits(request, locations);
		assert_equal(0u, static_cast<uint32_t>(suppressed.nodes.size()), "era suppressed by default");
		assert_equal(false, suppressed.publish, "nothing published for an era alone");
	}

	request.intent_place = "London";

	{
		const location_cache locations;
		const auto revealed = df::compute_visits(request, locations);
		assert_equal(false, revealed.nodes.empty(), "era revealed when named");
		assert_equal(true, revealed.nodes[0].kind == df::visit_kind::era, "revealed node is an era");
	}
}

static void should_run_the_search_a_timeline_node_promises()
{
	df::visit_node node;
	node.name = "Tokyo, Japan";
	node.named = true;
	node.first = core::date_t(2019, 6, 12);
	node.last = core::date_t(2019, 6, 21);
	node.count = 10;
	node.radius_km = 25.0;
	node.centre = gps_coordinate(35.68, 139.69);

	assert_equal("Jun 2019", df::visit_node_dates(node), "node label inside one month");

	// docs/locations.md#visits: refining an existing query replaces its location and date scope rather
	// than intersecting with it, so clicking a second node moves the view instead of emptying it.
	auto current = df::search_t();
	current.location("Paris", df::location_level::any);
	current.year(2011);

	const auto search = df::visit_node_search(current, node);
	const auto formatted = search.format_terms();

	assert_equal(formatted, df::search_t::parse(formatted).format_terms(), "node search round-trips");
	assert_equal(true, df::is_visit_node_selected(search, node), "node latched by its own search");
	assert_equal(false, df::is_visit_node_selected(current, node), "node not latched by another search");

	// docs/locations.md#visits: the date bounds have to include their own end days, or the query a node
	// runs returns fewer items than the node displayed.
	const auto range = df::search_t().date_range(node.first.date(), node.last.date());

	const auto matches = [&range](const int y, const int m, const int d)
	{
		df::index_file_item file;
		file.ft = files::file_type_from_name("test.jpg");
		const auto md = file.safe_ps();
		md->dates.add(prop::date_source::exif_original, core::date_t(y, m, d, 12, 0, 0));
		file.calc_search_presence();

		const df::search_matcher matcher(range, core::utc_now().to_days());
		return matcher.match_item({}, file).is_match();
	};

	assert_equal(true, matches(2019, 6, 15), "item inside the range");
	assert_equal(true, matches(2019, 6, 12), "item on the first day");
	assert_equal(true, matches(2019, 6, 21), "item on the last day");
	assert_equal(false, matches(2019, 6, 11), "item before the range");
	assert_equal(false, matches(2019, 6, 22), "item after the range");

	// A node with no name at all clicks through as coordinates, never as an invented place.
	df::visit_node remote;
	remote.name = "Remote area";
	remote.first = core::date_t(2004, 2, 1);
	remote.last = core::date_t(2006, 9, 4);
	remote.radius_km = 100.0;
	remote.centre = gps_coordinate(-40.5, -110.25);

	assert_equal("2004-2006", df::visit_node_dates(remote), "node label across years");
	assert_equal(true, df::visit_node_search(df::search_t(), remote).format_terms().starts_with("loc:"),
	             "unnamed node searches coordinates");
}

void register_location_tests(view_state& /*state*/, test_registry& tests)
{
	tests.add("Should group sidebar map by place"s, should_group_sidebar_map_by_place);

	//
	// Map tiles
	//
	tests.add("Should build tile user agent"s, should_build_tile_user_agent);
	tests.add("Should pack tile database keys"s, should_pack_tile_database_keys);
	tests.add("Should cache tiles in a database"s, should_cache_tiles_in_a_database);
	tests.add("Should prune unused tiles"s, should_prune_unused_tiles);
	tests.add("Should bound tile cache by size"s, should_bound_tile_cache_by_size);
	// MOD-023 - age pruning and vacuum share bounded maintenance work with size pruning.
	tests.add("Should budget expired tile pruning"s, should_budget_expired_tile_pruning);
	tests.add("Should keep tiles inside the retention window"s, should_keep_tiles_inside_the_retention_window);
	// SRC-037 - environmental tile-cache open failures must not replace healthy bytes.
	tests.add("Should classify tile cache open failures before replacement"s,
	          should_classify_tile_cache_open_failures_before_replacement);
	tests.add("Should replace an unreadable tile cache"s, should_replace_an_unreadable_tile_cache);
	// MOD-022 - failed COMMIT must leave wrapper and SQLite transaction state coherent.
	tests.add("Should keep tile transaction bookkeeping after commit failure"s,
	          should_keep_tile_transaction_bookkeeping_after_commit_failure);
	tests.add("Should resolve tile cache db beside index db"s, should_resolve_tile_cache_db_beside_index_db);
	tests.add("Should download only the map tiles still shown"s, should_download_only_the_map_tiles_still_shown);
	tests.add("Should download the most wanted map tile first"s, should_download_the_most_wanted_map_tile_first);
	tests.add("Should not ask again at once for a map tile that failed"s,
	          should_not_ask_again_at_once_for_a_map_tile_that_failed);

	//
	// Map geometry
	//
	tests.add("Should anchor map marker cells to world"s, should_anchor_map_marker_cells_to_world);
	// UI-008 - marker snapshots are prepared before UI publication
	tests.add("Should publish prepared map marker snapshots"s, should_publish_prepared_map_marker_snapshots);
	tests.add("Should cull prepared map marker cells to view"s, should_cull_prepared_map_marker_cells_to_view);
	tests.add("Should clear map marker hits when snapshot changes"s,
	          should_clear_map_marker_hits_when_snapshot_changes);
	tests.add("Should measure distance to map cells"s, should_measure_distance_to_map_cells);
	tests.add("Should build aggregate location matrix"s, should_build_aggregate_location_matrix);
	tests.add("Should select thumbnail representatives while counting"s,
	          should_select_thumbnail_representatives_while_counting);

	//
	// The sidebar globe
	//
	tests.add("Should project globe coordinates"s, should_project_globe_coordinates);
	tests.add("Should round trip globe pixels"s, should_round_trip_globe_pixels);
	tests.add("Should rotate globe by drag"s, should_rotate_globe_by_drag);
	tests.add("Should frame globe on the collection"s, should_frame_globe_on_the_collection);
	tests.add("Should place a coordinate under the pointer"s, should_place_a_coordinate_under_the_pointer);
	tests.add("Should count explore parts as their searches list"s, should_count_explore_parts_as_their_searches_list);
	tests.add("Should cluster explore places by scale"s, should_cluster_explore_places_by_scale);

	//
	// Gazetteer lookup
	//
	// MOD-003/MOD-004/MOD-005 - canonical country identity and bounded gazetteer lookup
	tests.add("Should find Location"s, should_find_location);
	tests.add("Should return no place for empty location lookup"s, should_return_no_place_for_empty_location_lookup);
	tests.add("Should find places across the antimeridian and poles"s,
	          should_find_places_across_the_antimeridian_and_poles);
	tests.add("Should reload location index"s, should_reload_location_index);
	// Issue #119 - localized place and country names
	tests.add("Should offset localized name"s, should_offset_localized_name);

	//
	// Place naming
	//
	tests.add("Should read place qualification level"s, should_read_place_qualification_level);
	tests.add("Should compose qualified place name"s, should_compose_qualified_place_name);
	tests.add("Should bound place attribution"s, should_bound_place_attribution);
	tests.add("Should describe bearing"s, should_describe_bearing);
	tests.add("Should attribute from the place find closest names"s,
	          should_attribute_from_the_place_find_closest_names);

	//
	// Location search
	//
	tests.add("Should recover from an empty location search"s, should_recover_from_an_empty_location_search);
	tests.add("Should reproduce a location group from its header"s, should_reproduce_a_location_group_from_its_header);
	tests.add("Should search unbuilt height classes as typed"s, should_search_unbuilt_height_classes_as_typed);
	tests.add("Should step search distance"s, should_step_search_distance);
	tests.add("Should read only signed numbers as a coordinate"s, should_read_only_signed_numbers_as_a_coordinate);
	tests.add("Should resolve location vocabulary"s, should_resolve_location_vocabulary);
	tests.add("Should read every location scope from one table"s, should_read_every_location_scope_from_one_table);
	tests.add("Should complete locations as search terms"s, should_complete_locations_as_search_terms);
	tests.add("Should match location radius and presence"s, should_match_location_radius_and_presence);
	tests.add("Should search suburbs by their enclosing city"s, should_search_suburbs_by_their_enclosing_city);
	tests.add("Should search GPS faces and metadata as independent words"s,
	          should_search_gps_faces_and_metadata_as_independent_words);

	//
	// Map areas
	//
	tests.add("Should open a map area as a place and radius"s, should_open_a_map_area_as_a_place_and_radius);
	tests.add("Should contain map location cells"s, should_contain_map_location_cells);
	tests.add("Should scale map location cells"s, should_scale_map_location_cells);
	tests.add("Should average map location coordinates"s, should_average_map_location_coordinates);
	tests.add("Should resolve named map area on demand"s, should_resolve_named_map_area_on_demand);
	tests.add("Should parse map area"s, should_parse_map_area);

	//
	// Visits and timeline
	//
	tests.add("Should derive visits from a result set"s, should_derive_visits_from_a_result_set);
	tests.add("Should split a cluster at long gaps"s, should_split_a_cluster_at_long_gaps);
	tests.add("Should centre a visit across the antimeridian"s, should_centre_a_visit_across_the_antimeridian);
	tests.add("Should tell text only namesakes apart"s, should_tell_text_only_namesakes_apart);
	tests.add("Should exclude items that cannot sit on a timeline"s,
	          should_exclude_items_that_cannot_sit_on_a_timeline);
	tests.add("Should count a state only sample as located"s, should_count_a_state_only_sample_as_located);
	// MOD-006 - place breakdown identity case folding
	tests.add("Should fold place breakdown identity case"s, should_fold_place_breakdown_identity_case);
	tests.add("Should reproduce a place breakdown from its chip"s, should_reproduce_a_place_breakdown_from_its_chip);
	// MOD-007/MOD-008 - deterministic non-overlapping place breakdown predicates
	tests.add("Should merge overlapping partial place breakdowns"s, should_merge_overlapping_partial_place_breakdowns);
	tests.add("Should drop unbounded partial place breakdowns"s, should_drop_unbounded_partial_place_breakdowns);
	tests.add("Should keep the largest partial place candidate"s, should_keep_the_largest_partial_place_candidate);
	tests.add("Should keep random place breakdown predicates authoritative"s,
	          should_keep_random_place_breakdown_predicates_authoritative);
	// MOD-008 - collapse work scales with identities, not identity pairs
	tests.add("Should bound place breakdown collapse work"s, should_bound_place_breakdown_collapse_work);
	tests.add("Should cancel place breakdown derivation"s, should_cancel_place_breakdown_derivation);
	tests.add("Should tell two place chips apart"s, should_tell_two_place_chips_apart);
	tests.add("Should suppress an era until the query names it"s, should_suppress_an_era_until_the_query_names_it);
	tests.add("Should run the search a timeline node promises"s, should_run_the_search_a_timeline_node_promises);

#ifndef _DEBUG
	tests.add("Should Find Closest Location"s, should_find_closest_location);
#endif
}
