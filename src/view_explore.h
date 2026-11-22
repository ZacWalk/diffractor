// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Explore view: the collection on a globe beside its timeline, for finding items by where
// and when they were taken. The globe turns and zooms to street level, map tiles draped over it and its
// places clustered at the scale it is seen at; the timeline reads from today back - years, months with
// a picture each, the photos and videos of each day as bars, weeks, and the trips away from home beside
// them. Pointing at a time marks where it was spent, and pointing at a place marks when. Clicking a week,
// a run of weeks, a month, a year, a trip or a place visited in a year opens its items in Items, and
// clicking a place on the globe searches the area around it. Also the timeline's geometry, which is laid
// out apart from the view so it can be tested.

#pragma once

#include "model.h"
#include "model_explore.h"
#include "ui_view.h"
#include "ui_globe.h"
#include "ui_map_common.h"
#include "libdesktop/ui/list_selection.h"

// The timeline's vertical geometry at a scale: from the top, each year with anything in it starts with a
// header, its months with items are drawn a line a day, newest at the top, and runs of months or years
// with nothing fold into one short row.
struct explore_timeline_metrics
{
	int day_height = 3;
	int year_height = 46;
	int gap_height = 24;
	int top = 4;
	int bottom = 24;
};

struct explore_timeline_layout
{
	enum class row_kind : uint8_t
	{
		year,
		days,
		gap,
		empty_years,
	};

	struct row
	{
		row_kind kind = row_kind::days;
		int top = 0;
		int bottom = 0;
		// A year's header, or the year of a gap; for empty years, the newest of them.
		int year = 0;
		// For empty years, the oldest of them.
		int oldest_year = 0;
		// Days drawn, or folded away by a gap, oldest first and newest last. Days rows draw the newest at top.
		uint32_t first_day = 0;
		uint32_t last_day = 0;
	};

	// A month, week or piece of a trip, where it is drawn.
	struct span
	{
		uint32_t index = 0; // into the model's months, weeks or trips
		int top = 0;
		int bottom = 0;
	};

	int day_height = 3;
	// A year's header, and so how much of the top the header held there while its year scrolls covers.
	int year_height = 0;
	int height = 0;
	std::vector<row> rows;
	std::vector<span> months;
	std::vector<span> weeks;
	std::vector<span> trips;

	// The row under a height, the last one above it when it is past them, or none.
	const row* row_at(int y) const;
	// The day drawn at a height, or nothing where no day is.
	std::optional<uint32_t> day_at(int y) const;
	// The day drawn at a height or, at a header or a fold, the newest drawn below it: where the timeline stands.
	std::optional<uint32_t> day_near(int y) const;
	// Where a day's line is drawn, or nothing for a day that is not.
	std::optional<int> top_of(uint32_t day) const;
	// Where a day's line is drawn or, for one not drawn, the newest older day's: where a time left stands now.
	std::optional<int> top_of_or_older(uint32_t day) const;
	// The header of the year a height is in.
	const row* year_row_at(int y) const;
	const row* year_row(int year) const;
	// The span of a list holding a height.
	static const span* span_at(const std::vector<span>& spans, int y);
};

explore_timeline_layout layout_explore_timeline(const df::explore_model& model,
                                                const explore_timeline_metrics& metrics);

// What the pointer can be on in either half.
enum class explore_target_kind : uint8_t
{
	none,
	cluster,
	year,
	year_place,
	month,
	week,
	weeks,
	trip,
};

struct explore_target
{
	explore_target_kind kind = explore_target_kind::none;
	// Into the clusters, model years, year places, months, weeks or trips. For weeks, the newer end.
	uint32_t index = 0;
	// For weeks, the older end.
	uint32_t other = 0;

	bool is_none() const { return kind == explore_target_kind::none; }
	bool operator==(const explore_target&) const = default;
};

class explore_view final : public view_base, public std::enable_shared_from_this<explore_view>
{
public:
	enum class pane : uint8_t
	{
		globe,
		timeline,
	};

private:
	view_state& _state;
	view_host_ptr _host;
	sizei _extent;
	double _scale_factor = 1.0;
	std::string _title;
	std::string _status;

	// What is shown, built off the UI thread and replaced whole. A model that arrives waits until the
	// timeline is laid out for it, so what is drawn and pointed at is always one model and its layout, and while the
	// user turns the globe or drags across weeks, so what they are doing is not taken from under them.
	df::explore_model_ptr _model;
	df::explore_model_ptr _arrived;
	uint32_t _model_generation = 0;
	bool _building = false;
	bool _build_again = false;
	bool _stale = true;
	// The collection changes in runs while the index walks - its histograms every tenth of a second - so a model is
	// made again at most once a second, and the change that ends a run is taken up by a later tick.
	bool _model_due = false;
	int64_t _model_requested_ms = 0;

	// The globe: the coordinate facing the viewer, how far it is zoomed beyond fitting its half, and what
	// was drawn last. The view is the user's once they have moved it. The sidebar's globe is this one smaller:
	// each faces where the user last turned either, and this counts the turns seen.
	recti _globe_bounds;
	globe_renderer _globe;
	map_tile_cache _tiles;
	gps_coordinate _view;
	double _zoom = 1.0;
	bool _view_is_user_set = false;
	uint32_t _globe_turns_seen = 0;
	core::surface_ptr _globe_surface;
	mutable render::texture_ptr _globe_texture;
	bool _globe_invalid = true;
	uint32_t _tile_revision = 0;
	bool _tiles_drawn = false;
	// Tiles arrive one at a time after a turn or a zoom: the globe is drawn again for them at most ten times a second,
	// and the last of a run by a later tick, rather than once for each.
	int64_t _globe_rendered_ms = 0;
	bool _tiles_waiting = false;

	struct globe_key
	{
		gps_coordinate view;
		double radius = 0.0;
		sizei extent;
		uint32_t tile_revision = 0;

		bool operator==(const globe_key&) const = default;
	};

	globe_key _rendered;

	// A drag turns the globe so the point grabbed stays under the pointer.
	bool _turning = false;
	gps_coordinate _grabbed;
	gps_coordinate _drag_start_view;
	bool _drag_start_view_is_user_set = false;

	// A turn the view makes itself, to show where a time pointed at was spent when it is all out of sight.
	gps_coordinate _turn_from;
	gps_coordinate _turn_to;
	int64_t _turn_start_ms = 0;

	// The places, gathered at the level the globe is seen at, and each one's world vector, worked out once
	// so drawing and hit testing project them without trigonometry. Until a new model's clusters arrive the
	// old ones are still drawn, but they mark nothing, as their items are not the new model's.
	std::shared_ptr<const df::explore_clusters> _clusters;
	df::explore_model_ptr _clusters_of;
	std::vector<std::array<double, 3>> _cluster_vectors;
	uint32_t _cluster_generation = 0;
	int _cluster_level_asked = -1;
	// What each cluster is called, as the gazetteer answers on the location worker.
	core::hash_map<uint32_t, map_location_area> _cluster_areas;
	core::hash_set<uint32_t> _cluster_areas_asked;

	// The timeline.
	recti _timeline_bounds;
	std::shared_ptr<class view_info_element> _timeline_info;
	recti _timeline_content;
	explore_timeline_layout _layout;
	view_scroller _scroller;
	int _pad = 10;
	int _gutter_width = 112;
	int _bars_left = 0;
	int _bars_width = 150;
	int _lane_left = 0;
	int _line_height = 16;
	int _thumbnail_height = 72;
	int _rail_width = 0;

	// Where each trip's label is, kept apart from the pieces it labels so labels never overlap.
	struct trip_label
	{
		uint32_t trip = 0;
		recti bounds; // content coordinates
		int piece_y = 0;
	};

	std::vector<trip_label> _trip_labels;

	// Each year's place chips, relative to the top-left of its header.
	struct year_chip
	{
		uint32_t year_place = 0;
		recti bounds;
	};

	core::hash_map<int, std::vector<year_chip>> _year_chips;
	uint32_t _busiest_day = 1;

	// What the pointer is on, the drag that is choosing a run of weeks, and what pointing at one half
	// marks in the other.
	explore_target _hover;
	pointer_dwell _hover_turn_dwell;
	bool _hover_turn_done = false;
	explore_target _pressed;
	bool _choosing_weeks = false;
	uint32_t _weeks_anchor = 0;
	uint32_t _weeks_end = 0;
	// Where the pointer is while it chooses weeks, from which the tick scrolls on while it rests near an end.
	pointi _weeks_loc;
	ui::edge_scroll _weeks_scroll;
	std::vector<uint32_t> _cluster_marks;       // per cluster: items of the time pointed at
	std::vector<uint32_t> _marked_days;         // days holding items of the place pointed at, oldest first
	std::pair<uint32_t, uint32_t> _marked_span; // the first and last of them

	// The week the keyboard is on, when it has been used.
	uint32_t _keyboard_week = 0;
	bool _keyboard_used = false;
	// A day an item was taken, to bring into view and mark once the timeline is laid out for a model.
	std::optional<uint32_t> _show_day;

	// Pictures for the months, weeks, trips and places, by path. Items are made when they come into view,
	// so drawing only reads what they hold.
	struct thumbnail
	{
		df::item_element_ptr item;
		bool requested = false;
		mutable render::texture_ptr texture;
		mutable core::const_surface_ptr uploaded;
	};

	core::hash_map<core::file_path, thumbnail, core::ihash, core::ieq> _thumbnails;
	// How many pictures off the timeline's window - its bubbles' among them - are held before they are let go.
	static constexpr size_t max_spare_thumbnails = 64;

public:
	explore_view(view_state& state, view_host_ptr host);

	void open();
	// Opens Explore at an item: its globe facing where the item was taken, close enough to see the place among its
	// surroundings, and its timeline at the week it was taken, marked.
	void show_item(core::file_path path);

	// The collection changed; what is shown is worked out again if it is on screen, and on showing if not.
	void collection_changed();
	// Called by the window's tick while Explore is shown: scrolls on a run of weeks held near an end, asks for the model
	// a collection change left due, once a second has passed since the last was asked for, and draws the globe again
	// for tiles held back meanwhile.
	void tick_model();

	const df::explore_model_ptr& model() const { return _model; }
	const explore_timeline_layout& timeline() const { return _layout; }

	// view_base
	void activate(sizei extent) override;
	void deactivate() override;
	void refresh() override;
	void layout(render::measure_context& mc, sizei extent) override;
	void render(render::draw_context& dc, view_controller_ptr controller) override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, hit_test_context& ctx) override;
	bool mouse_wheel(pointi loc, platform::wheel_notch notch) override;
	bool key_down(char32_t key, platform::key_state keys) override;
	recti keyboard_bounds() const override;
	void broadcast_event(const view_element_event& event) const override;
	void display_changed() override;
	void exit() override;
	std::string_view title() override;
	std::string_view status() override;
	view_bars bars() const override;

	// What a pane's controller asks of the view. The view keeps the state; a controller only routes the
	// pointer to it.
	void hover(pane p, pointi loc);
	void press(pane p, pointi loc);
	void drag(pane p, pointi start, pointi loc, bool moved);
	void choose_weeks_at(pointi loc);
	void release(pane p, pointi loc, bool moved);
	bool cancel(pane p);
	void leave(pane p);
	void bubble(pane p, view_hover_element& hover);
	void tick_hover_turn(pointi loc, uint64_t now);
	const explore_target& hovered() const { return _hover; }

	// A model handed over as a finished build hands it over, for tests, which build theirs without an index.
	void model_arrived_for_tests(df::explore_model_ptr model) { model_built(std::move(model), _model_generation); }

	// The radius the globe is drawn at, and where its centre is.
	double globe_radius() const;
	globe_projection projection() const;
	// Where the globe faces and how far it is zoomed, and where the timeline is scrolled to, for tests.
	gps_coordinate globe_view() const { return _view; }
	double globe_zoom() const { return _zoom; }
	void step_globe_zoom(int direction);
	int timeline_scroll() const { return _scroller.scroll_offset().y; }

private:
	void request_model();
	void model_built(df::explore_model_ptr model, uint32_t generation);
	void adopt_arrived_model();
	// Faces where the sidebar's globe was last turned, whole, when it was turned since this one showed; and tells it
	// where the user turned this one.
	void follow_globe_turns();
	void publish_globe_turn();
	void request_clusters();
	void frame_on_collection();
	void zoom_globe(double factor, std::optional<pointi> about);
	void show_marked_places();
	bool step_turn();
	void stop_turn();
	void resolve_cluster(uint32_t cluster);
	map_location_area cluster_area(uint32_t cluster) const;

	void layout_timeline(render::measure_context& mc);
	void prepare_thumbnails();
	void scroll_to_week(uint32_t week);

	explore_target globe_target_at(pointi loc) const;
	explore_target timeline_target_at(pointi loc) const;
	void set_hover(const explore_target& target);
	void mark_from_time(uint32_t first_day, uint32_t last_day, const df::explore_place* place);
	void mark_from_cluster(uint32_t cluster);
	void clear_marks();
	void open_target(const explore_target& target);
	void open_search(const df::search_t& search);
	void update_status();

	pointi timeline_offset() const;
	recti week_bounds(uint32_t week) const;
	std::optional<std::pair<int, int>> weeks_extent(uint32_t newer, uint32_t older) const;
	recti target_bounds(const explore_target& target) const;

	void render_globe(render::draw_context& dc);
	void render_clusters(render::draw_context& dc, const globe_projection& projection) const;
	void render_timeline(render::draw_context& dc);
	void render_year_header(render::draw_context& dc, const explore_timeline_layout::row& row, int top) const;
	void render_days(render::draw_context& dc, const explore_timeline_layout::row& row, pointi offset) const;
	void render_trips(render::draw_context& dc, pointi offset, recti clip) const;
	void render_thumbnail(render::draw_context& dc, const core::file_path& path, recti bounds, float radius) const;

	void bubble_cluster(view_hover_element& hover, uint32_t cluster);
	void bubble_time(view_hover_element& hover, const explore_target& target);
	void add_bubble_thumbnail(view_hover_element& hover, const core::file_path& path);
};
