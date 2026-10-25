// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The row list the task views share (Rename, Import, Sync, Convert/Metadata/Date, Tags).
// list_view lays out the column header and rows and tracks a run's progress and results;
// view_command_status is the status a run reports through; view_controls_host pairs the list with
// its controls panel; and the CollisionPolicy control is defined once here for every operation
// that writes to a destination.

#pragma once

#include "model.h"
#include "ui_view.h"
#include "ui_controllers.h"
#include "ui_elements.h"
#include "ui_dialog.h"

class list_view;
class view_controls_host;
class view_command_status;
using view_controls_host_ptr = std::shared_ptr<view_controls_host>;

struct view_operation_result
{
	std::string name;
	item_status status = item_status::cancel;
};

// One named CollisionPolicy control (docs/design.md) shared by every destination-writing
// operation so the vocabulary, ordering and behavior are identical everywhere.
// Storage for the radio flags is owned by the callbacks held by the returned element.
// A prompt raised because collisions were already found offers only the choices that resolve one:
// Block run would repeat the Cancel button beside it, and Skip is not always expressible.
inline view_element_ptr create_collision_policy_control(const ui::control_frame_ptr& frame, collision_policy& value,
                                                        std::function<void()> changed,
                                                        const bool allow_auto_rename = true,
                                                        const bool allow_block_and_skip = true)
{
	struct policy_flags
	{
		bool block_run = false;
		bool skip = false;
		bool replace = false;
		bool auto_rename = false;
	};

	const auto flags = std::make_shared<policy_flags>();

	// Auto-rename is not offered by every operation; fall back rather than leave nothing selected.
	if (!allow_auto_rename && value == collision_policy::auto_rename) value = collision_policy::skip;
	if (!allow_block_and_skip && (value == collision_policy::block_run || value == collision_policy::skip))
		value = allow_auto_rename ? collision_policy::auto_rename : collision_policy::replace;

	flags->block_run = value == collision_policy::block_run;
	flags->skip = value == collision_policy::skip;
	flags->replace = value == collision_policy::replace;
	flags->auto_rename = value == collision_policy::auto_rename;

	auto group = std::make_shared<ui::group_control>();
	group->add(std::make_shared<text_element>(tt.collision_policy_label));

	auto apply = [flags, &value, allow_block_and_skip, changed = std::move(changed)](const bool)
	{
		if (flags->skip) value = collision_policy::skip;
		else if (flags->replace) value = collision_policy::replace;
		else if (flags->auto_rename) value = collision_policy::auto_rename;
		// Nothing selected can only mean Block run where it is offered; where it is not, the safe
		// default stands rather than a policy the prompt never showed.
		else value = allow_block_and_skip ? collision_policy::block_run : collision_policy::auto_rename;

		if (changed) changed();
	};

	if (allow_block_and_skip)
	{
		group->add(std::make_shared<ui::check_control>(frame, tt.collision_block, flags->block_run, true, false, apply,
		                                               ui::radio_group_collision));
		group->add(std::make_shared<ui::check_control>(frame, tt.collision_skip, flags->skip, true, false, apply,
		                                               ui::radio_group_collision));
	}

	if (allow_auto_rename)
	{
		group->add(
			std::make_shared<ui::check_control>(frame, tt.collision_rename, flags->auto_rename, true, false, apply,
			                                    ui::radio_group_collision));
	}

	group->add(std::make_shared<ui::check_control>(frame, tt.collision_replace, flags->replace, true, false, apply,
	                                               ui::radio_group_collision));
	return group;
}

class list_view : public view_base
{
protected:
	using this_type = list_view;

	view_state& _state;
	view_host_ptr _host;
	sizei _extent;
	view_scroller _scroller;

	bool _rows_clickable = false;
	progress_state _progress;
	bool _showing_results = false;
	bool _refresh_requested = false;
	size_t _processing_generation = 0;
	std::shared_ptr<std::atomic_int> _processing_cancel;

	// The line under the rows: what the plan would do while it is reviewed, what the run did after.
	std::string _status;

	static constexpr int max_col_count = 4;
	int col_count = 4;
	int coll_offset = 0;
	int col_widths[max_col_count] = {0, 0, 0, 0};


	recti _col_header_bounds[max_col_count];
	bool _header_active = false;
	bool _header_tracking = false;
	int _header_active_num = 0;

	// The column headers are painted over the top of the scroller and the status band over its base,
	// so the rows they cover are inside the scroller but not visible. Text metrics are only available
	// during layout, so the band height is held here for the scroll that has to clear them.
	int _band_height = 0;
	int _band_margin = 0;

	// Mirrors view_frame::draw_view_status: an indeterminate run draws a centred pill rather than the
	// band across the base of the view. Asked at the moment it matters, because a run raises the status
	// without resizing the view, so a layout need not have followed it.
	bool shows_status_band()
	{
		return !status().empty() && !(_progress.active && _progress.total == 0);
	}

	friend class scroll_controller;
	friend class clickable_controller;
	friend class header_controller;

public:
	struct row_element final : std::enable_shared_from_this<row_element>, view_element
	{
		list_view& _view;
		std::string _text[max_col_count];
		icon_index _icons[max_col_count] = {};
		ui::color _bg_row;
		ui::color32 _text_color[max_col_count] = {0, 0, 0, 0};
		int _order = 0;
		// This row's index into whatever vector the run reports against - the plan for the batch views,
		// the dense work list for the others - or -1 when the run never reports this row at all. The
		// header sort reorders _rows while the run stays in plan order, so display position is not an
		// identity and nothing may index the run by it.
		int _work_index = -1;

		row_element(list_view& view) noexcept
			: view_element(view_element_style::can_invoke), _view(view)
		{
		}

		void render(ui::draw_context& dc, const pointi element_offset) const override
		{
			const auto bg_alpha = dc.colors.alpha * dc.colors.bg_alpha;
			const auto logical_bounds = bounds.offset(element_offset);

			if (dc.clip_bounds().intersects(logical_bounds))
			{
				dc.draw_rect(logical_bounds, _bg_row.a_min(bg_alpha));
				dc.draw_rect(logical_bounds, _bg_color);

				auto x = logical_bounds.left + _view.coll_offset;

				for (int i = 0; i < _view.col_count; i++)
				{
					const auto text_color = ui::color(_text_color[i] != 0 ? _text_color[i] : dc.colors.foreground,
					                                  dc.colors.alpha);
					const std::string_view text = _text[i];
					const recti bounds(x, logical_bounds.top, x + _view.col_widths[i], logical_bounds.bottom);
					if (_icons[i] != icon_index::none)
					{
						xdraw_icon(dc, _icons[i], bounds, text_color, {});
					}
					else
					{
						dc.draw_text(text, bounds, ui::style::font_face::dialog, ui::style::text_style::single_line,
						             text_color, {});
					}
					x += _view.col_widths[i] + dc.padding2;
				}
			}
		}

		sizei measure(ui::measure_context& mc, int width_limit) const override
		{
			const auto row_height = mc.text_line_height(ui::style::font_face::dialog) + mc.padding2;
			return {width_limit, row_height};
		}

		void dispatch_event(const view_element_event& event) override
		{
		}

		view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
		                                             const pointi element_offset,
		                                             hit_test_context& ctx) override
		{
			return default_controller_from_location(*this, host, loc, element_offset, ctx);
		}
	};

	using row_element_ptr = std::shared_ptr<row_element>;
	std::vector<row_element_ptr> _rows;

	// The row currently being processed, held rather than indexed: sort() reorders _rows and a
	// refresh replaces them, either of which would leave a stored position pointing at another row
	// and strand the highlight on the one it left behind.
	row_element_ptr _active_row;


	list_view(view_state& state, view_host_ptr host) : _state(state), _host(std::move(host))
	{
	}

	~list_view() override
	{
		_rows.clear();
	}

	progress_state progress() const override
	{
		return _progress;
	}

	std::string_view status() override
	{
		return _status;
	}

	// A completed run keeps its results through the file notifications its own writes raise. Refresh
	// is the user asking to re-plan, so it is honoured even then; both arrive as the same invalidation,
	// which is why the request is remembered here rather than read from the flag.
	void reload() override
	{
		_refresh_requested = true;
	}

	void refresh_from_source() override
	{
		if (std::exchange(_refresh_requested, false) || !showing_results()) refresh();
	}

	void begin_processing(const size_t total)
	{
		// A run still going belongs to whatever this one replaces - an Analyze restarted by Refresh,
		// say. Its worker reads the source it was given, so bumping that is what stops it; swapping
		// in a new source alone let it scan on to an answer nobody would publish.
		if (_processing_cancel) ++(*_processing_cancel);
		++_processing_generation;
		_processing_cancel = std::make_shared<std::atomic_int>();
		_progress = {true, 0, static_cast<int64_t>(total)};
		_active_row.reset();
		_showing_results = false;
		_state.invalidate_view(view_invalid::view_redraw | view_invalid::status | view_invalid::command_state |
			view_invalid::app_layout);
	}

	size_t processing_generation() const { return _processing_generation; }
	bool is_processing_generation(const size_t generation) const { return generation == _processing_generation; }
	std::shared_ptr<std::atomic_int> processing_cancel_source() const { return _processing_cancel; }

	// The status line a finished run leaves: what it did (the result counts), then why some rows did
	// nothing (the run's own message) - a partial run needs both. A run that reports neither leaves
	// the status the review showed.
	static std::string conclude_run(std::string summary, std::string message, std::string review_status)
	{
		if (summary.empty()) return message.empty() ? std::move(review_status) : std::move(message);
		if (message.empty()) return summary;
		return summary + "  " + message;
	}

	// The status a run reports through. Progress highlights the row being worked on; completion
	// swaps the reviewed rows for the results and concludes the status line from them. Both are
	// ignored once the view has moved past `generation`. The caller reads the generation and the
	// cancel source straight after begin_processing: anything run in between can pump messages, and
	// a view abandoned there must not have its next state mistaken for this run's. `hold` lives
	// until the run reports back - the file handles a run detaches stay detached until then.
	static std::shared_ptr<view_command_status> make_run_status(const std::shared_ptr<list_view>& view,
	                                                            size_t generation,
	                                                            std::shared_ptr<std::atomic_int> cancel_source,
	                                                            std::string review_status,
	                                                            std::shared_ptr<void> hold = {});

	void processing_item(const size_t row_index, const size_t work_position)
	{
		if (!_progress.active || row_index >= _rows.size()) return;

		if (_active_row) _active_row->_bg_color = {};

		const auto& row = _rows[row_index];
		_active_row = row;
		_progress.position = static_cast<int64_t>(work_position);
		row->_bg_color = ui::color(ui::style::color::important_background, 1.0f);

		const auto client_height = _scroller.client_bounds().height();
		auto reserve_top = _band_height + _band_margin;
		auto reserve_bottom = shows_status_band() ? _band_height + _band_margin : 0;

		// A view too short to hold the row and both reserves has no range left to place it in, so it
		// falls back to the whole client rather than refusing to move.
		if (reserve_top + reserve_bottom + row->bounds.height() > client_height)
		{
			reserve_top = 0;
			reserve_bottom = 0;
		}

		const auto visible_top = _scroller.scroll_offset().y + reserve_top;
		const auto visible_bottom = _scroller.scroll_offset().y + client_height - reserve_bottom;

		if (row->bounds.top < visible_top)
		{
			_scroller.scroll_offset(_host, 0, row->bounds.top - reserve_top);
		}
		else if (row->bounds.bottom > visible_bottom)
		{
			_scroller.scroll_offset(_host, 0, row->bounds.bottom + reserve_bottom - client_height);
		}

		_state.invalidate_view(view_invalid::view_redraw | view_invalid::status);
	}

	// Resolving a work index against the rows rather than indexing them: the header sort is offered
	// throughout the review, so by the time the run reports progress the display order and the plan
	// order need not agree. Takes a projection so the rows are scanned in place.
	template <typename work_index_at>
	static int row_for_work_index(const size_t row_count, const size_t work_index, work_index_at&& at)
	{
		if (work_index > static_cast<size_t>(std::numeric_limits<int>::max())) return -1;
		const auto wanted = static_cast<int>(work_index);

		// Every view builds its rows in work order, so until the user sorts, the row is where the
		// index says. Without this the batch views scan the whole list once per item, and they queue
		// a progress callback per item rather than coalescing - quadratic on a large selection.
		if (work_index < row_count && at(work_index) == wanted) return static_cast<int>(work_index);

		for (size_t row_index = 0; row_index < row_count; ++row_index)
		{
			if (at(row_index) == wanted) return static_cast<int>(row_index);
		}

		return -1;
	}

	void processing_work_item(const size_t work_index, const size_t work_position)
	{
		const auto row_index = row_for_work_index(_rows.size(), work_index,
		                                          [this](const size_t i) { return _rows[i]->_work_index; });
		if (row_index < 0) return;

		processing_item(static_cast<size_t>(row_index), work_position);
	}

	void processing_work_item(const size_t work_index)
	{
		processing_work_item(work_index, work_index + 1);
	}

	void end_processing()
	{
		if (_active_row) _active_row->_bg_color = {};

		_active_row.reset();
		_progress = {};
		_state.invalidate_view(view_invalid::view_redraw | view_invalid::status | view_invalid::command_state |
			view_invalid::app_layout);
	}

	// One sentence for a finished run, whatever view drew the rows: counts first, then the classes
	// of row that did nothing, so a partial run is never read as a whole one.
	static std::string format_operation_summary(const std::vector<view_operation_result>& results)
	{
		size_t succeeded = 0;
		size_t failed = 0;
		size_t skipped = 0;
		size_t canceled = 0;
		std::string first_succeeded;
		std::string first_failed;
		std::string first_skipped;
		std::string first_canceled;

		for (const auto& result : results)
		{
			switch (result.status)
			{
			case item_status::success:
				if (first_succeeded.empty()) first_succeeded = result.name;
				++succeeded;
				break;
			case item_status::fail:
				if (first_failed.empty()) first_failed = result.name;
				++failed;
				break;
			case item_status::ignore:
				if (first_skipped.empty()) first_skipped = result.name;
				++skipped;
				break;
			case item_status::cancel:
				if (first_canceled.empty()) first_canceled = result.name;
				++canceled;
				break;
			}
		}

		const auto total = static_cast<int64_t>(results.size());
		std::string summary;
		const auto append = [&summary](std::string text)
		{
			if (!summary.empty()) summary += " ";
			summary += std::move(text);
		};
		if (succeeded > 0) append(format_plural_text(tt.processed_fmt, first_succeeded, succeeded, {}, total));
		if (failed > 0) append(format_plural_text(tt.failed_items_fmt, first_failed, failed, {}, total));
		if (skipped > 0) append(format_plural_text(tt.ignored_fmt, first_skipped, skipped, {}, total));
		if (canceled > 0) append(format_plural_text(tt.canceled_items_fmt, first_canceled, canceled, {}, total));
		return summary;
	}

	// True while the rows describe a finished run rather than a plan waiting to be run.
	bool showing_results() const { return _showing_results; }

	std::string show_results(const std::vector<view_operation_result>& results)
	{
		std::vector<row_element_ptr> rows;
		rows.reserve(results.size());

		for (const auto& result : results)
		{
			auto row = std::make_shared<row_element>(*this);
			row->_text[1] = result.name;
			row->_order = static_cast<int>(rows.size());

			switch (result.status)
			{
			case item_status::success:
				row->_icons[0] = icon_index::check;
				break;
			case item_status::fail:
				row->_icons[0] = icon_index::error;
				break;
			case item_status::ignore:
				row->_icons[0] = icon_index::none;
				break;
			case item_status::cancel:
				row->_icons[0] = icon_index::cancel;
				break;
			}

			rows.emplace_back(std::move(row));
		}

		auto summary = format_operation_summary(results);

		_rows = std::move(rows);
		_showing_results = true;
		_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
			view_invalid::command_state);
		return summary;
	}

	bool cancel_processing()
	{
		if (!_progress.active) return false;
		if (_processing_cancel) ++(*_processing_cancel);
		return true;
	}

	// Everything queued before this point belongs to a view that is no longer on screen. Bumping the
	// generation is what makes their completion callbacks no-ops: without it, an analysis still
	// running when the user switched away published on its return and restored a runnable plan built
	// from a selection the view had already let go of.
	void abandon_processing()
	{
		++_processing_generation;
		if (_processing_cancel) ++(*_processing_cancel);
		_progress = {};
		_active_row.reset();
	}

	void cancel_operation() override
	{
		cancel_processing();
	}

	// Close always exits. When a task is running, state what will stop and let the user choose.
	// Returns true when the view should exit.
	bool confirm_exit_while_processing(const std::string_view operation_title)
	{
		if (!_progress.active) return true;

		const auto dlg = make_dlg(_host->owner());

		const std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::question,
			                                                tt.cancel_operation_title,
			                                                str_format(tt.cancel_operation_fmt.sv(), operation_title))),
			std::make_shared<divider_element>(),
			std::make_shared<ui::ok_cancel_control>(dlg->_frame, tt.button_cancel_operation, tt.button_keep_running)
		};

		if (dlg->show_modal(controls, {44}, {33}) != ui::close_result::ok) return false;

		cancel_processing();
		return true;
	}

	bool can_exit() override
	{
		return !_progress.active;
	}

	bool confirm_exit() override
	{
		return confirm_exit_while_processing(operation_name());
	}

	void activate(const sizei extent) override
	{
		_extent = extent;
		_state.stop();
	}

	void sort(int col_num)
	{
		if (col_num == 1 || col_num == 2)
		{
			std::ranges::sort(_rows, [col_num](const auto& left, const auto& right)
			{
				return str::icmp(left->_text[col_num], right->_text[col_num]) < 0;
			});
		}
		else
		{
			std::ranges::sort(_rows, [](const auto& left, const auto& right) { return left->_order < right->_order; });
		}

		_state.invalidate_view(view_invalid::controller | view_invalid::view_layout);
	}

	void layout(ui::measure_context& mc, const sizei extent) override
	{
		_extent = extent;

		const auto titles = col_titles();
		const recti scroll_bounds{_extent.cx - mc.scroll_width, 0, _extent.cx, _extent.cy};
		const recti client_bounds{0, 0, std::max(0, _extent.cx - mc.scroll_width), _extent.cy};

		const auto line_height = mc.text_line_height(ui::style::font_face::dialog);
		_band_margin = line_height;
		_band_height = line_height + mc.padding2 * 2;

		auto y_max = 0;
		std::string_view longest_text[max_col_count];
		bool has_icon[max_col_count] = {};

		bool odd_row = false;

		for (int i = 0; i < col_count; i++)
		{
			longest_text[i] = titles[i].sv();
		}

		if (!_rows.empty())
		{
			const auto row_height = mc.text_line_height(ui::style::font_face::dialog) + mc.padding2;
			auto y = row_height + mc.padding2 * 2;

			for (const auto& r : _rows)
			{
				r->bounds.set(client_bounds.left, y, client_bounds.right, y + row_height);
				y += row_height + mc.padding1;

				for (int i = 0; i < col_count; i++)
				{
					const std::string_view text = r->_text[i];
					if (text.size() > longest_text[i].size()) longest_text[i] = text;
					has_icon[i] = has_icon[i] || r->_icons[i] != icon_index::none;
				}

				r->_bg_row.a = odd_row ? 0.11f : 0.0f;
				odd_row = !odd_row;
			}

			// The last row has to be able to clear the status band and the margin held below it, so the
			// scrollable height carries both rather than ending at the final row.
			y_max = y + mc.padding2;
			if (shows_status_band()) y_max += _band_height + _band_margin;
		}

		auto x_max = 0;
		auto shrink_col_total = 0;

		for (int i = 0; i < col_count; i++)
		{
			col_widths[i] = mc.measure_text(longest_text[i], ui::style::font_face::dialog,
			                                ui::style::text_style::single_line, client_bounds.width()).cx + mc.padding2;
			if (has_icon[i]) col_widths[i] = std::max(col_widths[i], mc.icon_cxy + mc.padding2);
			x_max += col_widths[i] + mc.padding2;
			if (i > 0) shrink_col_total += col_widths[i];
		}

		// Shrink if too large
		if (x_max > client_bounds.width())
		{
			const auto cx_avail = std::max(0, client_bounds.width() - (col_count + 1) * mc.padding2);
			const auto fixed_width = col_widths[0];
			if (fixed_width < cx_avail && shrink_col_total > 0)
			{
				const auto shrink_avail = cx_avail - fixed_width;
				for (int i = 1; i < col_count; i++)
				{
					col_widths[i] = df::mul_div(shrink_avail, col_widths[i], shrink_col_total);
				}
			}
			else if (x_max > 0)
			{
				const auto total_width = x_max - col_count * mc.padding2;
				for (int i = 0; i < col_count; i++)
				{
					col_widths[i] = total_width > 0 ? df::mul_div(cx_avail, col_widths[i], total_width) : 0;
				}
			}

			x_max = 0;

			for (int i = 0; i < col_count; i++)
			{
				x_max += col_widths[i] + mc.padding2;
			}
		}

		coll_offset = std::max(0, client_bounds.width() - x_max) / 2;

		_scroller.layout({client_bounds.width(), y_max}, client_bounds, scroll_bounds);
		_host->frame()->invalidate();
	}

	void deactivate() override
	{
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, hit_test_context& ctx) override;

	view_element_ptr element_from_location(const int y) const
	{
		view_element_ptr result;
		const auto offset = _scroller.scroll_offset();
		int distance = 10000;

		for (const auto& i : _rows)
		{
			const auto yy = (i->bounds.top + i->bounds.bottom) / 2;
			const auto d = abs(yy - y - offset.y);

			if (d < distance)
			{
				distance = d;
				result = i;
			}
		}

		return result;
	}


	void render_headers(ui::draw_context& dc)
	{
		const auto titles = col_titles();
		const auto cy = dc.text_line_height(ui::style::font_face::dialog) + dc.padding2 * 2;
		const auto bg_alpha = dc.colors.alpha * 0.77f;
		const auto text_color = ui::color(dc.colors.foreground, dc.colors.alpha);

		constexpr int y = 0;
		auto x = coll_offset;

		const ui::color header_bg(ui::darken(dc.colors.background, 0.33f), bg_alpha);

		if (!_header_active)
		{
			const auto header_bounds = recti(0, y, _extent.cx, y + cy);
			dc.draw_rect(header_bounds, header_bg);
		}
		else
		{
			auto xx = x;

			for (int i = 0; i < col_count; ++i)
			{
				xx += col_widths[i] + dc.padding2;
			}

			const auto header_bounds_left = recti(0, y, x - dc.padding2, y + cy);
			const auto header_bounds_right = recti(xx - dc.padding2, y, _extent.cx, y + cy);

			dc.draw_rect(header_bounds_left, header_bg);
			dc.draw_rect(header_bounds_right, header_bg);
		}

		for (int i = 0; i < col_count; ++i)
		{
			const auto cx = col_widths[i];
			const recti col_header_bounds(x - dc.padding2, y, x + cx, y + cy);
			const recti col_header_text_bounds(x, y, x + cx, y + cy);

			if (_header_active)
			{
				const ui::color bg(ui::darken(_header_active_num == i
					                              ? ui::style::color::dialog_selected_background
					                              : dc.colors.background, 0.33f), bg_alpha);
				dc.draw_rect(col_header_bounds, bg);
			}

			dc.draw_text(titles[i], col_header_text_bounds, ui::style::font_face::dialog,
			             ui::style::text_style::single_line, text_color, {});

			_col_header_bounds[i] = col_header_bounds;
			x += cx + dc.padding2;
		}
	}

	void render(ui::draw_context& rc, view_controller_ptr controller) override
	{
		const auto offset = -_scroller.scroll_offset();

		if (!_rows.empty())
		{
			for (const auto& i : _rows)
			{
				i->render(rc, offset);
			}

			render_headers(rc);
			_scroller.draw_scroll(rc);
		}
		else
		{
			if (!(_progress.active && _progress.total == 0))
			{
				const auto message = empty_message();

				if (!message.sv().empty())
				{
					const auto text_color = ui::color(rc.colors.foreground, rc.colors.alpha);
					rc.draw_text(message.sv(), recti(_extent), ui::style::font_face::dialog,
					             ui::style::text_style::single_line_center, text_color, {});
				}
			}
		}
	}

	bool mouse_wheel(const pointi loc, const ui::wheel_notch notch) override
	{
		if (!notch.is_vertical()) return false;
		_scroller.offset(_host, 0, -notch.delta);
		_state.invalidate_view(view_invalid::controller);
		return _scroller.can_scroll();
	}

	virtual text_t empty_message() { return {}; }

	virtual std::array<text_t, max_col_count> col_titles()
	{
		return std::array<text_t, max_col_count>{
			text_t{},
			text_t{},
			text_t{},
			text_t{}
		};
	};
};

class view_command_status final : public df::status_i, public std::enable_shared_from_this<view_command_status>
{
	async_strategy& _async;
	std::shared_ptr<std::atomic_int> _cancel_source;
	int _cancel_version = 0;
	std::function<void(size_t)> _started;
	std::function<void(std::string, std::vector<view_operation_result>)> _completed;
	std::atomic_size_t _position = 0;
	std::atomic_bool _has_failures = false;
	std::atomic_bool _is_complete = false;
	platform::mutex _progress_mutex;
	_Guarded_by_(_progress_mutex) size_t _pending_position = 0;
	_Guarded_by_(_progress_mutex) bool _progress_callback_pending = false;
	_Guarded_by_(_progress_mutex) std::vector<view_operation_result> _results;

public:
	view_command_status(async_strategy& async, std::shared_ptr<std::atomic_int> cancel_source,
	                    std::function<void(size_t)> started,
	                    std::function<void(std::string, std::vector<view_operation_result>)> completed) :
		_async(async), _cancel_source(std::move(cancel_source)),
		_cancel_version(_cancel_source ? _cancel_source->load() : 0),
		_started(std::move(started)), _completed(std::move(completed))
	{
	}

	void start_item(std::string_view) override
	{
		const auto position = _position.fetch_add(1);
		auto queue_callback = false;
		{
			platform::exclusive_lock lock(_progress_mutex);
			_pending_position = position;
			if (!_progress_callback_pending)
			{
				_progress_callback_pending = true;
				queue_callback = true;
			}
		}

		if (queue_callback)
		{
			_async.queue_ui([self = shared_from_this()] { self->publish_progress(); });
		}
	}

	void end_item(const std::string_view name, const item_status status) override
	{
		if (status == item_status::fail) _has_failures = true;
		platform::exclusive_lock lock(_progress_mutex);
		_results.emplace_back(std::string(name), status);
	}

	bool has_failures() const override { return _has_failures; }

	bool is_canceled() const override
	{
		return _cancel_source && _cancel_source->load() != _cancel_version;
	}

	void abort(const std::string_view message) override { finish(std::string(message)); }
	void complete(const std::string_view message = {}) override { finish(std::string(message)); }
	void show_errors() override { finish({}); }
	void show_message(const std::string_view message) override { finish(std::string(message)); }

	void message(std::string_view, int64_t, int64_t) override
	{
	}

	void wait_for_complete() const override
	{
	}

private:
	void publish_progress()
	{
		size_t position;
		{
			platform::exclusive_lock lock(_progress_mutex);
			position = _pending_position;
			_progress_callback_pending = false;
		}

		if (_started) _started(position);
	}

	void finish(std::string message)
	{
		if (_is_complete.exchange(true)) return;
		std::vector<view_operation_result> results;
		{
			platform::exclusive_lock lock(_progress_mutex);
			results = std::move(_results);
		}
		_async.queue_ui(
			[self = shared_from_this(), message = std::move(message), results = std::move(results)]() mutable
			{
				if (self->_completed) self->_completed(std::move(message), std::move(results));
			});
	}
};

inline std::shared_ptr<view_command_status> list_view::make_run_status(const std::shared_ptr<list_view>& view,
                                                                       const size_t generation,
                                                                       std::shared_ptr<std::atomic_int> cancel_source,
                                                                       std::string review_status,
                                                                       std::shared_ptr<void> hold)
{
	return std::make_shared<view_command_status>(
		view->_state._async, std::move(cancel_source),
		[view, generation](const size_t index)
		{
			if (view->is_processing_generation(generation)) view->processing_work_item(index);
		},
		[view, generation, review_status = std::move(review_status), hold = std::move(hold)](
		std::string message, const std::vector<view_operation_result>& results)
		{
			if (!view->is_processing_generation(generation)) return;
			view->end_processing();
			view->_status = conclude_run(view->show_results(results), std::move(message), review_status);
			view->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
		});
}

class view_controls_host : public view_host, public std::enable_shared_from_this<view_controls_host>
{
public:
	view_state& _state;
	view_scroller _scroller;
	ui::control_frame_ptr _dlg;
	ui::frame_ptr _frame;
	std::vector<view_element_ptr> _controls;
	ui::color _clr = ui::color(ui::style::color::dialog_text);
	int _layout_height = 0;
	int _layout_width = 0;
	ui::coll_widths _label_width;
	// Set by a view whose panel exists to be typed into. Applied after the first layout because the
	// child controls are not shown, and so cannot take focus, until their positions are applied.
	std::function<void()> initial_focus;

	view_controls_host(view_state& s) : _state(s)
	{
		_scroller._scroll_child_controls = true;
	}

	~view_controls_host() override = default;

	void scroll_controls() override
	{
		frame()->layout();
	}


	virtual void layout_controls(ui::measure_context& mc)
	{
		// _dlg is released on destroy, and a panel can still be asked to lay out after that.
		if (!_controls.empty() && _dlg)
		{
			const auto layout_padding = df::round(mc.padding1 / mc.scale_factor);
			auto avail_bounds = recti(_extent);
			avail_bounds.right -= mc.scroll_width + layout_padding;

			mc.col_widths = {};
			ui::control_layouts positions;
			flex_container_layout column;
			column.direction = flex_direction::column;
			column.wrap = flex_wrap::no_wrap;
			column.align_items = flex_align::start;
			column.padding = {layout_padding, layout_padding};
			const auto height = layout_flex_elements(_controls, mc, positions, avail_bounds, column).cy;

			_layout_height = height;
			_layout_width = avail_bounds.width();
			_label_width = mc.col_widths;

			const recti scroll_bounds{_extent.cx - mc.scroll_width, 0, _extent.cx, _extent.cy};
			const recti client_bounds{0, 0, _extent.cx - mc.scroll_width, _extent.cy};
			_scroller.layout({_layout_width, _layout_height}, client_bounds, scroll_bounds);

			_dlg->apply_layout(positions, -_scroller.scroll_offset());
			_dlg->invalidate();
		}
	}

	void populate()
	{
		const view_element_event e{view_element_event_type::populate, shared_from_this()};

		for (const auto& c : _controls)
		{
			c->dispatch_event(e);
		}
	}

	void on_window_layout(ui::measure_context& mc, const sizei extent, bool is_minimized) override
	{
		_extent = extent;
		layout_controls(mc);

		if (initial_focus && !is_minimized && !_controls.empty())
		{
			const auto claim = std::move(initial_focus);
			initial_focus = {};
			claim();
		}
	}

	void on_window_paint(ui::draw_context& dc) override
	{
		dc.col_widths = _label_width;

		const auto offset = _scroller.scroll_offset();

		for (const auto& c : _controls)
		{
			if (c->is_visible())
			{
				c->render(dc, -offset);
			}
		}

		if (_active_controller)
		{
			_active_controller->draw(dc);
		}

		_scroller.draw_scroll(dc);
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

	const ui::frame_ptr frame() const override
	{
		return _frame ? _frame : ui::no_frame();
	}

	const ui::control_frame_ptr owner() override
	{
		return _dlg;
	}

	void invoke(const commands cmd) override
	{
		_state.invoke(cmd);
	}

	bool is_command_checked(const commands cmd) override
	{
		return _state.is_command_checked(cmd);
	}

	void controller_changed() override
	{
		_state.invalidate_view(view_invalid::tooltip);
	}

	void invalidate_element(const view_element_ptr& e) override
	{
		frame()->invalidate();
	}

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
	{
		if (_scroller.can_scroll() && ctx.occluded(_scroller.scroll_bounds()))
		{
			return std::make_shared<scroll_controller>(shared_from_this(), _scroller, _scroller.scroll_bounds());
		}

		const auto offset = -_scroller.scroll_offset();

		for (const auto& e : _controls)
		{
			auto controller = e->controller_from_location(shared_from_this(), loc, offset, ctx);
			if (controller) return controller;
		}

		return nullptr;
	}

	void on_mouse_wheel(const pointi loc, const int delta, const ui::key_state keys, bool& was_handled) override
	{
		_scroller.offset(shared_from_this(), 0, -(delta / 2));
		update_controller(loc);
	}

	virtual void options_changed()
	{
		if (!_controls.empty())
		{
			for (const auto& c : _controls)
			{
				c->visit_controls([](const ui::control_base_ptr& cc) { cc->options_changed(); });
			}
		}
	}

	void focus_changed(bool has_focus, const ui::control_base_ptr& child) override
	{
		if (child && child->has_focus() && _dlg)
		{
			auto point_offset = _scroller.scroll_offset();
			const auto rc = child->window_bounds().offset(point_offset - _dlg->window_bounds().top_left());

			if (rc.top < point_offset.y)
			{
				point_offset.y = rc.top;
			}
			else if (rc.bottom > point_offset.y + _extent.cy)
			{
				point_offset.y = rc.bottom - _extent.cy;
			}

			_scroller.scroll_offset(shared_from_this(), 0, point_offset.y);
		}
	}

	void command_hover(const ui::command_ptr& c, const recti window_bounds) override
	{
		_state.command_hover(c, window_bounds);
	}

	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override
	{
		if (_dlg) _state.track_menu(_dlg, bounds, commands);
	}

	void invalidate_view(const view_invalid invalid) override
	{
		_state.invalidate_view(invalid);
	}

	void on_window_destroy() override
	{
		_active_controller.reset();
		_controls.clear();
		_dlg.reset();
		_frame.reset();
	}
};

class header_controller final : public view_controller
{
public:
	list_view& _parent;
	int _col_num;

	header_controller(const view_host_ptr& host, list_view& parent, const recti bounds, const int col_num) :
		view_controller(host, bounds), _parent(parent), _col_num(col_num)
	{
		_parent._header_active = true;
		_parent._header_active_num = _col_num;
	}

	~header_controller() override
	{
		if (_parent._header_tracking)
		{
			escape();
		}

		_parent._header_active = false;
	}

	void draw(ui::draw_context& rc) override
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::link;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;
		_parent._header_tracking = true;
		_host->frame()->invalidate();
	}

	void on_mouse_move(const pointi loc) override
	{
		_last_loc = loc;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;

		// A press that escape abandoned, or that was released away from the header it started on,
		// is not a click on that header. Sorting regardless re-ordered the whole list on a gesture
		// the user had already taken back.
		const auto was_tracking = _parent._header_tracking;
		_parent._header_tracking = false;

		if (was_tracking && _bounds.contains(loc))
		{
			_parent.sort(_col_num);
		}

		_host->frame()->invalidate();
	}

	bool escape() override
	{
		if (!_parent._header_tracking) return false;
		_parent._header_tracking = false;
		return true;
	}

	void popup_from_location(view_hover_element& hover) override
	{
	}
};

inline view_controller_ptr list_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                              hit_test_context& ctx)
{
	if (_progress.active) return nullptr;

	for (int i = 0; i < col_count; ++i)
	{
		if (ctx.occluded(_col_header_bounds[i]))
		{
			return std::make_shared<header_controller>(host, *this, _col_header_bounds[i], i);
		}
	}

	if (ctx.occluded(_scroller.scroll_bounds()))
	{
		return std::make_shared<scroll_controller>(host, _scroller, _scroller.scroll_bounds());
	}

	if (_rows_clickable)
	{
		const auto test = element_from_location(loc.y);

		if (test)
		{
			const auto test_offset = -_scroller.scroll_offset();
			return std::make_shared<clickable_controller>(host, test, test_offset);
		}
	}

	return nullptr;
}
