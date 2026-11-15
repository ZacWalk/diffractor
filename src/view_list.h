// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The row list the task views share (Rename, Import, Sync, Convert/Metadata/Date, Tags).
// list_view lays out the column header and rows, whose columns fit their content and are resized by
// dragging the divider after them, scrolls by the wheel and the keyboard, and tracks a run's progress
// and then, in a Status column after the review's own, what it did to each row; view_command_status is
// the status a run reports through; view_controls_host pairs the list with its controls panel; and
// the CollisionPolicy control is defined once here for every operation that writes to a destination.

#pragma once

#include "model.h"
#include "ui_view.h"
#include "ui_controllers.h"
#include "ui_elements.h"
#include "ui_dialog.h"
#include "libdesktop/ui/text/table_layout.h"

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
inline view_element_ptr create_collision_policy_control(const ui::frame_ptr& frame, collision_policy& value,
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
		                                               radio_group_collision));
		group->add(std::make_shared<ui::check_control>(frame, tt.collision_skip, flags->skip, true, false, apply,
		                                               radio_group_collision));
	}

	if (allow_auto_rename)
	{
		group->add(std::make_shared<ui::check_control>(frame, tt.collision_rename, flags->auto_rename, true, false,
		                                               apply, radio_group_collision));
	}

	group->add(std::make_shared<ui::check_control>(frame, tt.collision_replace, flags->replace, true, false, apply,
	                                               radio_group_collision));
	return group;
}

// A cell's text cut to its column's width, ending in an ellipsis, where it is wider, as a file browser shows a name
// too long for its column: drawn whole it was clipped mid-character with nothing to say so. Text that fits is as it
// is, and a character is never split.
inline std::string fit_cell_text(render::measure_context& mc, const std::string_view text, const int width)
{
	constexpr auto unbounded = std::numeric_limits<int>::max() / 4;
	const auto measure = [&mc](const std::string_view s)
	{
		return mc.measure_text(s, render::font_face::dialog, render::text_style::single_line, unbounded).cx;
	};

	if (text.empty() || measure(text) <= width) return std::string(text);

	constexpr std::string_view ellipsis = "\xE2\x80\xA6";
	const auto available = width - measure(ellipsis);
	// The most characters that fit before the ellipsis: as many as `fits` always do, as many as `unfit` never.
	auto fits = 0;
	auto unfit = core::utf8_codepoint_count(text);

	while (fits + 1 < unfit)
	{
		const auto mid = fits + (unfit - fits) / 2;
		if (measure(text.substr(0, core::utf8_truncate(text, mid))) <= available) fits = mid;
		else unfit = mid;
	}

	auto result = std::string(text.substr(0, core::utf8_truncate(text, fits)));
	result += ellipsis;
	return result;
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

	// The status line the frame's status bar shows: what the plan would do while it is reviewed, what
	// the run did after.
	std::string _status;

	// The most columns a review shows, and the Status column a finished run adds after them.
	static constexpr int max_col_count = 5;
	// The review's columns; a finished run shows one more (column_count).
	int col_count = 4;
	int coll_offset = 0;
	int col_widths[max_col_count] = {};

	recti _col_header_bounds[max_col_count];
	bool _header_active = false;
	bool _header_tracking = false;
	int _header_active_num = 0;

	// A divider resizes the column before it, as in a file browser's details view. A column dragged
	// to a width keeps it, in unscaled units so a change of DPI does too, until the view is opened
	// again; zero is a column that sizes to its content. The last column has no divider after it: it
	// takes whatever width the others leave.
	ui::table_layout::columns _table_columns;
	// What each column's content asks for before any shrinking to fit, which is what a double-click
	// on the divider after it fits the column to.
	int _content_col_widths[max_col_count] = {};
	// Where the divider after each column but the last can be grabbed: across the header band.
	recti _col_divider_bounds[max_col_count - 1];
	int _min_col_width = 0;
	int _cx_col_avail = 0;
	int _divider_active = -1;

	// The column headers are painted over the top of the scroller, so the rows they cover are inside
	// the scroller but not visible. Text metrics are only available during layout, so the header
	// height is held here for the scroll that has to clear it.
	int _band_height = 0;
	int _band_margin = 0;
	// From one row's top to the next one's, which the arrow keys scroll by.
	int _row_step = 0;

	friend class ui::scroll_controller;
	friend class ui::clickable_controller;
	friend class header_controller;
	friend class column_divider_controller;

public:
	struct row_element final : std::enable_shared_from_this<row_element>, view_element
	{
		list_view& _view;
		std::string _text[max_col_count];
		icon_index _icons[max_col_count] = {};
		core::color _bg_row;
		core::color32 _text_color[max_col_count] = {};
		int _order = 0;
		// This row's index into whatever vector the run reports against - the plan for the batch views,
		// the dense work list for the others - or -1 when the run never reports this row at all. The
		// header sort reorders _rows while the run stays in plan order, so display position is not an
		// identity and nothing may index the run by it.
		int _work_index = -1;
		// Where the run's report of this row comes among the rest: every task builds its rows in the
		// order its run reports them, so review_rows numbers them as they are built.
		int _result_index = -1;

		// Each cell's text as last drawn, fitted to its column, with the text and width it was fitted to: a row
		// redrawn as the list scrolls measures nothing until one of them changes.
		struct fitted_cell
		{
			std::string source;
			int width = -1;
			std::string text;
		};

		mutable fitted_cell _fitted[max_col_count];

		row_element(list_view& view) noexcept : view_element(view_element_style::can_invoke), _view(view) {}

		std::string_view fitted_text(render::measure_context& mc, const int col, const int width) const
		{
			auto& cell = _fitted[col];

			if (cell.width != width || cell.source != _text[col])
			{
				cell.source = _text[col];
				cell.width = width;
				cell.text = fit_cell_text(mc, _text[col], width);
			}

			return cell.text;
		}

		void render(render::draw_context& dc, const pointi element_offset) const override
		{
			const auto bg_alpha = dc.colors.alpha * dc.colors.bg_alpha;
			const auto logical_bounds = bounds.offset(element_offset);

			if (dc.clip_bounds().intersects(logical_bounds))
			{
				dc.draw_rect(logical_bounds, _bg_row.a_min(bg_alpha));
				dc.draw_rect(logical_bounds, _bg_color);

				auto x = logical_bounds.left + _view.coll_offset;

				for (int i = 0; i < _view.column_count(); i++)
				{
					const auto text_color = core::color(_text_color[i] != 0 ? _text_color[i] : dc.colors.foreground,
					                                    dc.colors.alpha);
					const recti cell_bounds(x, logical_bounds.top, x + _view.col_widths[i], logical_bounds.bottom);
					if (_icons[i] != icon_index::none)
					{
						xdraw_icon(dc, _icons[i], cell_bounds, text_color, {});
					}
					else
					{
						dc.draw_text(fitted_text(dc, i, cell_bounds.width()), cell_bounds, render::font_face::dialog,
						             render::text_style::single_line, text_color, {});
					}
					x += _view.col_widths[i] + dc.padding2;
				}
			}
		}

		sizei measure(render::measure_context& mc, int width_limit) const override
		{
			const auto row_height = mc.text_line_height(render::font_face::dialog) + mc.padding2;
			return {width_limit, row_height};
		}

		void dispatch_event(const view_element_event& /*event*/) override {}

		view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
		                                             const pointi element_offset, hit_test_context& ctx) override
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

	list_view(view_state& state, view_host_ptr host) : _state(state), _host(std::move(host)) {}

	~list_view() override { _rows.clear(); }

	progress_state progress() const override { return _progress; }

	std::string_view status() override { return _status; }

	// A completed run keeps its results through the file notifications its own writes raise. Refresh
	// is the user asking to re-plan, so it is honoured even then; both arrive as the same invalidation,
	// which is why the request is remembered here rather than read from the flag.
	void reload() override { _refresh_requested = true; }

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
		row->_bg_color = core::color(ui::style::colors.important_background, 1.0f);

		const auto client_height = _scroller.client_bounds().height();
		auto reserve_top = _band_height + _band_margin;

		// A view too short to hold the row below the headers has no range left to place it in, so it
		// falls back to the whole client rather than refusing to move.
		if (reserve_top + row->bounds.height() > client_height)
		{
			reserve_top = 0;
		}

		const auto visible_top = _scroller.scroll_offset().y + reserve_top;
		const auto visible_bottom = _scroller.scroll_offset().y + client_height;

		if (row->bounds.top < visible_top)
		{
			_scroller.scroll_offset(_host, 0, row->bounds.top - reserve_top);
		}
		else if (row->bounds.bottom > visible_bottom)
		{
			_scroller.scroll_offset(_host, 0, row->bounds.bottom - client_height);
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

	void processing_work_item(const size_t work_index) { processing_work_item(work_index, work_index + 1); }

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
		const auto append = [&summary](const std::string& text)
		{
			if (!summary.empty()) summary += " ";
			summary += text;
		};
		if (succeeded > 0) append(format_plural_text(tt.processed_fmt, first_succeeded, succeeded, {}, total));
		if (failed > 0) append(format_plural_text(tt.failed_items_fmt, first_failed, failed, {}, total));
		if (skipped > 0) append(format_plural_text(tt.ignored_fmt, first_skipped, skipped, {}, total));
		if (canceled > 0) append(format_plural_text(tt.canceled_items_fmt, first_canceled, canceled, {}, total));
		return summary;
	}

	// True while the rows describe a finished run rather than a plan waiting to be run.
	bool showing_results() const { return _showing_results; }

	// The reviewed rows, given in the order the run will report them - every task builds its rows in the
	// order its run takes them - so what the run reports lands on the row it describes however the list is
	// sorted meanwhile.
	void review_rows(std::vector<row_element_ptr> rows)
	{
		for (size_t i = 0; i < rows.size(); ++i) rows[i]->_result_index = static_cast<int>(i);
		_rows = std::move(rows);
	}

	// A finished run keeps every row and column of the review it ran, and adds a Status column saying what
	// it did to each row: statuses[i] for the row reported i-th. A row the run never reached was not run,
	// and a row that did nothing is dimmed, so what changed stands out.
	void show_results(const std::vector<item_status>& statuses)
	{
		const auto success_color = core::lighten(ui::style::colors.success_background, 0.55f);
		const auto fail_color = core::lighten(ui::style::colors.warning_background, 0.55f);
		const auto muted_color = core::darken(ui::style::colors.view_text, 0.22f);
		const auto status_col = col_count;

		for (const auto& row : _rows)
		{
			const auto index = row->_result_index;
			const auto status = index >= 0 && static_cast<size_t>(index) < statuses.size()
			                        ? statuses[static_cast<size_t>(index)]
			                        : item_status::cancel;
			auto& text = row->_text[status_col];
			auto& color = row->_text_color[status_col];
			row->_icons[status_col] = icon_index::none;

			switch (status)
			{
			case item_status::success:
				text = std::string(tt.result_success.sv());
				color = success_color;
				break;
			case item_status::fail:
				text = std::string(tt.result_failed.sv());
				color = fail_color;
				break;
			case item_status::ignore:
				text = std::string(tt.result_skipped.sv());
				color = muted_color;
				break;
			case item_status::cancel:
				text = std::string(tt.result_not_run.sv());
				color = muted_color;
				break;
			}

			if (status == item_status::ignore || status == item_status::cancel)
			{
				for (int i = 0; i < col_count; ++i) row->_text_color[i] = muted_color;
			}
		}

		_showing_results = true;
		_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
		                       view_invalid::command_state);
	}

	// The results a run reports through view_command_status, one per row in the order it built them. Answers
	// the summary that concludes the run.
	std::string show_results(const std::vector<view_operation_result>& results)
	{
		std::vector<item_status> statuses;
		statuses.reserve(results.size());
		for (const auto& result : results) statuses.emplace_back(result.status);
		show_results(statuses);
		return format_operation_summary(results);
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

	void cancel_operation() override { cancel_processing(); }

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
			std::make_shared<ui::ok_cancel_control>(dlg->_frame, tt.button_cancel_operation, tt.button_keep_running),
		};

		if (dlg->show_modal(controls, {44}, {33}) != ui::close_result::ok) return false;

		cancel_processing();
		return true;
	}

	bool can_exit() override { return !_progress.active; }

	bool confirm_exit() override { return confirm_exit_while_processing(operation_name()); }

	void activate(const sizei extent) override
	{
		_extent = extent;
		_state.stop();

		// Each use of the task starts from columns that fit what it lists.
		_table_columns.reset();
	}

	// The first heading puts the rows back in the order the review built them, and any other sorts by what its
	// column says.
	void sort(int col_num)
	{
		if (col_num > 0 && col_num < column_count())
		{
			std::ranges::stable_sort(_rows, [col_num](const auto& left, const auto& right)
			{ return core::icmp(left->_text[col_num], right->_text[col_num]) < 0; });
		}
		else
		{
			std::ranges::sort(_rows, [](const auto& left, const auto& right) { return left->_order < right->_order; });
		}

		_state.invalidate_view(view_invalid::controller | view_invalid::view_layout);
	}

	void layout(render::measure_context& mc, const sizei extent) override
	{
		_extent = extent;

		const auto columns = column_count();
		const auto titles = column_titles();
		const recti scroll_bounds{_extent.cx - mc.scroll_width, 0, _extent.cx, _extent.cy};
		const recti client_bounds{0, 0, std::max(0, _extent.cx - mc.scroll_width), _extent.cy};

		const auto line_height = mc.text_line_height(render::font_face::dialog);
		_band_margin = line_height;
		_band_height = line_height + mc.padding2 * 2;

		auto y_max = 0;
		std::string_view longest_text[max_col_count];
		bool has_icon[max_col_count] = {};

		bool odd_row = false;

		for (int i = 0; i < columns; i++)
		{
			longest_text[i] = titles[i].sv();
		}

		if (!_rows.empty())
		{
			const auto row_height = mc.text_line_height(render::font_face::dialog) + mc.padding2;
			auto y = row_height + mc.padding2 * 2;
			_row_step = row_height + mc.padding1;

			for (const auto& r : _rows)
			{
				r->bounds.set(client_bounds.left, y, client_bounds.right, y + row_height);
				y += _row_step;

				for (int i = 0; i < columns; i++)
				{
					const std::string_view text = r->_text[i];
					if (text.size() > longest_text[i].size()) longest_text[i] = text;
					has_icon[i] = has_icon[i] || r->_icons[i] != icon_index::none;
				}

				r->_bg_row.a = odd_row ? 0.11f : 0.0f;
				odd_row = !odd_row;
			}

			y_max = y + mc.padding2;
		}

		_min_col_width = std::max(mc.icon_cxy + mc.padding2, core::round(32 * mc.scale_factor));
		// The table starts a margin in from the view's left edge however little it holds - never centred in
		// the view - so its text does not run up against the window's frame, and runs to the right edge half a
		// gap in, with a gap between each column and the next.
		coll_offset = mc.padding2 * 2;
		_cx_col_avail = std::max(0,
		                         client_bounds.width() - coll_offset - (columns - 1) * mc.padding2 - mc.padding2 / 2);

		for (int i = 0; i < columns; i++)
		{
			auto content_width = mc.measure_text(longest_text[i], render::font_face::dialog,
			                                     render::text_style::single_line, client_bounds.width())
			                         .cx +
			                     mc.padding2;
			if (has_icon[i]) content_width = std::max(content_width, mc.icon_cxy + mc.padding2);
			_content_col_widths[i] = content_width;
		}
		const std::array<uint8_t, max_col_count> fixed{1};
		_table_columns.layout(std::span(_content_col_widths, columns), _cx_col_avail, _min_col_width, mc.scale_factor,
		                      true, std::span(fixed).first(columns));
		std::ranges::copy(_table_columns.widths(), col_widths);

		// Each divider sits in the middle of the gap between two columns, a grip either side of it.
		const auto grip = std::max(mc.padding2 / 2, core::round(4 * mc.scale_factor));
		auto column_x = coll_offset;

		for (int i = 0; i < max_col_count - 1; i++)
		{
			_col_divider_bounds[i] = {};
			if (i >= columns - 1) continue;

			column_x += col_widths[i] + mc.padding2;
			const auto divider_x = column_x - mc.padding2 / 2;
			_col_divider_bounds[i] = recti(divider_x - grip, 0, divider_x + grip, _band_height);
		}

		_scroller.layout({client_bounds.width(), y_max}, client_bounds, scroll_bounds);
		_host->frame()->invalidate();
	}

	bool is_user_sized(const int col) const { return _table_columns.user_sized(col); }

	// How many of the titled columns the list lays out and draws: the review's, and the Status a finished
	// run adds after them.
	int column_count() const { return col_count + (_showing_results ? 1 : 0); }

	// The headings over the columns column_count counts.
	std::array<text_t, max_col_count> column_titles()
	{
		auto titles = col_titles();
		if (_showing_results) titles[col_count] = tt.status;
		return titles;
	}

	// What a resize can be taken back to: every column's width.
	struct column_sizes
	{
		ui::table_layout::columns::sizes widths;
	};

	column_sizes current_column_sizes() const { return {_table_columns.user_sizes()}; }

	void restore_column_sizes(const column_sizes& sizes)
	{
		_table_columns.restore(sizes.widths);
		_state.invalidate_view(view_invalid::view_layout);
	}

	// The widest a column can be: the room the columns have, less the columns before it and those
	// after it the user sized, less the narrowest the others after it shrink to.
	int max_col_width(const int col) const { return _table_columns.maximum(col); }

	// Sizes one column, as a drag on the divider after it asks. Every column before this one is held as
	// it is on screen, shrunk to fit or not, so the divider stays under the pointer, and this one goes no
	// narrower than a column can and no wider than the columns after it leave room for. The last column
	// takes up what it gives back, and makes the room it takes.
	void resize_column(const int col, const int width)
	{
		if (_table_columns.resize(col, width)) _state.invalidate_view(view_invalid::view_layout);
	}

	// Fits a column to its content, as far as the columns after it can make room.
	void fit_column(const int col)
	{
		if (_table_columns.fit(col)) _state.invalidate_view(view_invalid::view_layout);
	}

	void deactivate() override {}

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

	void render_headers(render::draw_context& dc)
	{
		const auto columns = column_count();
		const auto titles = column_titles();
		const auto cy = dc.text_line_height(render::font_face::dialog) + dc.padding2 * 2;
		const auto bg_alpha = dc.colors.alpha * 0.77f;
		const auto text_color = core::color(dc.colors.foreground, dc.colors.alpha);

		constexpr int y = 0;
		auto x = coll_offset;

		const core::color header_bg(core::darken(dc.colors.background, 0.33f), bg_alpha);

		if (!_header_active)
		{
			const auto header_bounds = recti(0, y, _extent.cx, y + cy);
			dc.draw_rect(header_bounds, header_bg);
		}
		else
		{
			auto xx = x;

			for (int i = 0; i < columns; ++i)
			{
				xx += col_widths[i] + dc.padding2;
			}

			const auto header_bounds_left = recti(0, y, x - dc.padding2, y + cy);
			const auto header_bounds_right = recti(xx - dc.padding2, y, _extent.cx, y + cy);

			dc.draw_rect(header_bounds_left, header_bg);
			dc.draw_rect(header_bounds_right, header_bg);
		}

		for (int i = 0; i < columns; ++i)
		{
			const auto cx = col_widths[i];
			const recti col_header_bounds(x - dc.padding2, y, x + cx, y + cy);
			const recti col_header_text_bounds(x, y, x + cx, y + cy);

			if (_header_active)
			{
				const core::color bg(core::darken(_header_active_num == i ? ui::style::colors.dialog_selected_background
				                                                          : dc.colors.background,
				                                  0.33f),
				                     bg_alpha);
				dc.draw_rect(col_header_bounds, bg);
			}

			dc.draw_text(titles[i], col_header_text_bounds, render::font_face::dialog, render::text_style::single_line,
			             text_color, {});

			_col_header_bounds[i] = col_header_bounds;
			x += cx + dc.padding2;
		}

		// The dividers are drawn so the columns read as resizable: faintly, and plainly under the pointer.
		const auto line_width = std::max(1, core::round(dc.scale_factor));

		for (int i = 0; i < columns - 1; ++i)
		{
			const auto& divider = _col_divider_bounds[i];
			if (divider.is_empty()) continue;

			const auto active = _divider_active == i;
			const auto width = active ? line_width * 2 : line_width;
			const auto mid = (divider.left + divider.right) / 2;
			const recti line(mid - width / 2, y + dc.padding1, mid - width / 2 + width, y + cy - dc.padding1);
			dc.draw_rect(line, core::color(dc.colors.foreground, dc.colors.alpha * (active ? 0.9f : 0.3f)));
		}
	}

	void render(render::draw_context& rc, view_controller_ptr /*controller*/) override
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
			// An open-ended run has no rows to show yet, so the work area says what it is doing rather
			// than standing blank or claiming to be empty; the status bar says it as well.
			const auto running = _progress.active && _progress.total == 0;
			const std::string_view message = running ? status() : empty_message().sv();

			if (!message.empty())
			{
				const auto text_color = core::color(rc.colors.foreground, rc.colors.alpha);
				rc.draw_text(message, recti(_extent), render::font_face::dialog, render::text_style::single_line_center,
				             text_color, {});
			}
		}
	}

	bool mouse_wheel(const pointi /*loc*/, const platform::wheel_notch notch) override
	{
		if (!notch.is_vertical()) return false;
		_scroller.offset(_host, 0, -notch.delta);
		_state.invalidate_view(view_invalid::controller);
		return _scroller.can_scroll();
	}

	// The keys that scroll the review while it has the keyboard: the arrows by a row, Page Up and Page Down by what
	// shows under the headings, Home and End to either end.
	bool key_down(const char32_t key, const platform::key_state keys) override
	{
		if (keys.control || keys.alt) return false;

		const auto page = std::max(_row_step, _scroller.client_bounds().height() - _band_height - _row_step);
		const auto y = _scroller.scroll_offset().y;
		int to;

		if (key == platform::keys::UP) to = y - _row_step;
		else if (key == platform::keys::DOWN) to = y + _row_step;
		else if (key == platform::keys::PRIOR) to = y - page;
		else if (key == platform::keys::NEXT) to = y + page;
		else if (key == platform::keys::HOME) to = 0;
		else if (key == platform::keys::END) to = std::numeric_limits<int>::max();
		else return false;

		_scroller.scroll_offset(_host, 0, to);
		_state.invalidate_view(view_invalid::controller);
		return true;
	}

	virtual text_t empty_message() { return {}; }

	virtual std::array<text_t, max_col_count> col_titles()
	{
		return std::array<text_t, max_col_count>{text_t{}, text_t{}, text_t{}, text_t{}};
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
	core::mutex _progress_mutex;
	_Guarded_by_(_progress_mutex) size_t _pending_position = 0;
	_Guarded_by_(_progress_mutex) bool _progress_callback_pending = false;
	_Guarded_by_(_progress_mutex) std::vector<view_operation_result> _results;

public:
	view_command_status(async_strategy& async, std::shared_ptr<std::atomic_int> cancel_source,
	                    std::function<void(size_t)> started,
	                    std::function<void(std::string, std::vector<view_operation_result>)> completed) :
		_async(async), _cancel_source(std::move(cancel_source)),
		_cancel_version(_cancel_source ? _cancel_source->load() : 0), _started(std::move(started)),
		_completed(std::move(completed))
	{
	}

	void start_item(std::string_view) override
	{
		const auto position = _position.fetch_add(1);
		auto queue_callback = false;
		{
			core::exclusive_lock lock(_progress_mutex);
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
		core::exclusive_lock lock(_progress_mutex);
		_results.emplace_back(std::string(name), status);
	}

	bool has_failures() const override { return _has_failures; }

	bool is_canceled() const override { return _cancel_source && _cancel_source->load() != _cancel_version; }

	void abort(const std::string_view message) override { finish(std::string(message)); }
	void complete(const std::string_view message = {}) override { finish(std::string(message)); }
	void show_errors() override { finish({}); }
	void show_message(const std::string_view message) override { finish(std::string(message)); }

	void message(std::string_view, int64_t, int64_t) override {}

	void wait_for_complete() const override {}

private:
	void publish_progress()
	{
		size_t position;
		{
			core::exclusive_lock lock(_progress_mutex);
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
			core::exclusive_lock lock(_progress_mutex);
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
	return std::make_shared<view_command_status>(view->_state._async, std::move(cancel_source),
	                                             [view, generation](const size_t index)
	{
		if (view->is_processing_generation(generation)) view->processing_work_item(index);
	},
	                                             [view, generation, review_status = std::move(review_status),
	                                              hold = std::move(
													  hold)](std::string message,
	                                                         const std::vector<view_operation_result>& results)
	{
		if (!view->is_processing_generation(generation)) return;
		view->end_processing();
		view->_status = conclude_run(view->show_results(results), std::move(message), review_status);
		view->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
	});
}

// The panel of a task view's controls: a column of elements beside the view, drawn in the view's own frame
// and scrolled on its own. It is the host its elements answer to - their controllers and their events -
// while the view's frame lays it out, draws it and hands it the pointer over it.
class view_controls_host : public view_host, public std::enable_shared_from_this<view_controls_host>
{
public:
	view_state& _state;
	view_scroller _scroller;
	// The view's frame, which the panel is drawn in.
	ui::frame_ptr _frame;
	// Where the panel is in that frame. Its elements are laid out in the frame's coordinates, and move
	// up by its scroll.
	recti _bounds;
	std::vector<view_element_ptr> _controls;
	core::color _clr = core::color(ui::style::colors.dialog_text);
	int _layout_height = 0;
	int _layout_width = 0;
	render::column_widths _label_width;
	// Set by a view whose panel exists to be typed into. Applied after the first layout, which is when
	// the panel's frame first finds its controls.
	std::function<void()> initial_focus;

	view_controls_host(view_state& s) : _state(s)
	{
		// Drawn among the view's frame's other content, so a scroll draws the panel again.
		_scroller._moves_pixels = false;
	}

	~view_controls_host() override = default;

	pointi elements_offset() const { return -_scroller.scroll_offset(); }

	virtual void layout_controls(render::measure_context& mc)
	{
		if (_controls.empty()) return;

		const auto layout_padding = core::round(mc.padding1 / mc.scale_factor);
		auto avail_bounds = _bounds;
		avail_bounds.right -= mc.scroll_width + layout_padding;

		mc.col_widths = {};
		flex_container_layout column;
		column.direction = flex_direction::column;
		column.wrap = flex_wrap::no_wrap;
		column.align_items = flex_align::start;
		column.padding = {layout_padding, layout_padding};
		const auto height = layout_flex_elements(_controls, mc, avail_bounds, column).cy;

		_layout_height = height;
		_layout_width = avail_bounds.width();
		_label_width = mc.col_widths;

		const recti scroll_bounds{_bounds.right - mc.scroll_width, _bounds.top, _bounds.right, _bounds.bottom};
		const recti client_bounds{_bounds.left, _bounds.top, _bounds.right - mc.scroll_width, _bounds.bottom};
		_scroller.layout({_layout_width, _layout_height}, client_bounds, scroll_bounds);
	}

	// Laid out by the view's frame, in the part of it bounds says.
	void layout_panel(render::measure_context& mc, const recti bounds)
	{
		_bounds = bounds;
		layout_controls(mc);

		if (initial_focus && !_controls.empty() && !bounds.is_empty())
		{
			const auto claim = std::move(initial_focus);
			initial_focus = {};
			claim();
		}
	}

	void render_panel(render::draw_context& dc) const
	{
		if (_bounds.is_empty()) return;

		const auto colors = dc.colors;
		dc.colors.background = ui::style::colors.dialog_background;
		dc.colors.foreground = ui::style::colors.dialog_text;
		dc.colors.selected = ui::style::colors.dialog_selected_background;
		dc.col_widths = _label_width;

		{
			render::scoped_clip clip(dc, _bounds);
			dc.draw_rect(_bounds, core::color(dc.colors.background, 1.0f));

			const auto offset = elements_offset();

			for (const auto& c : _controls)
			{
				if (c->is_visible()) c->render(dc, offset);
			}

			_scroller.draw_scroll(dc);
		}

		dc.colors = colors;
	}

	void visit_elements(const std::function<void(const view_element_ptr&, pointi)>& visit) override
	{
		const auto offset = elements_offset();
		const auto controls = _controls;
		for (const auto& c : controls) visit(c, offset);
	}

	void populate()
	{
		const view_element_event e{view_element_event_type::populate, shared_from_this()};

		for (const auto& c : _controls)
		{
			c->dispatch_event(e);
		}
	}

	// Every control in the panel takes input, or none does: while the view's task runs.
	void enable(const bool enable)
	{
		auto changed = false;

		for (const auto& c : _controls)
		{
			if (c->is_enabled() == enable) continue;
			c->enable(enable);
			changed = true;
		}

		if (changed) frame()->invalidate(_bounds);
	}

	// Drawn by the view's frame, never laid out or painted as a frame of its own.
	void on_window_layout(render::measure_context& /*mc*/, const sizei /*extent*/, bool /*is_minimized*/) override {}

	void on_window_paint(render::draw_context& /*dc*/) override {}

	void tick() override {}

	void activate(bool /*is_active*/) override {}

	bool key_down(const char32_t /*c*/, const platform::key_state /*keys*/) override { return false; }

	const ui::frame_ptr frame() const override { return _frame ? _frame : ui::no_frame(); }

	const ui::frame_ptr owner() override { return frame(); }

	void invoke(const commands cmd) override { _state.invoke(cmd); }

	bool is_command_checked(const commands cmd) override { return _state.is_command_checked(cmd); }

	void controller_changed() override { _state.invalidate_view(view_invalid::tooltip); }

	void invalidate_element(const view_element_ptr& /*e*/) override { frame()->invalidate(_bounds); }

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
	{
		if (_scroller.can_scroll() && ctx.occluded(_scroller.scroll_bounds()))
		{
			return std::make_shared<scroll_controller>(shared_from_this(), _scroller, _scroller.scroll_bounds());
		}

		const auto offset = elements_offset();

		for (const auto& e : _controls)
		{
			if (!e->is_visible()) continue;
			auto controller = e->controller_from_location(shared_from_this(), loc, offset, ctx);
			if (controller) return controller;
		}

		return nullptr;
	}

	// A wheel over something in the panel that scrolls - a list of results - turns that; elsewhere it
	// scrolls the panel.
	void on_mouse_wheel(const pointi loc, const int delta, const platform::key_state /*keys*/,
	                    bool& was_handled) override
	{
		const auto offset = elements_offset();

		for (const auto& c : _controls)
		{
			if (c->is_visible() && c->wheel(loc, offset, delta))
			{
				was_handled = true;
				return;
			}
		}

		_scroller.offset(shared_from_this(), 0, -(delta / 2));
		was_handled = _scroller.can_scroll();
	}

	virtual void options_changed()
	{
		for (const auto& c : _controls)
		{
			ui::walk_tree(c, {}, true, [](ui::element& e, pointi, bool, bool)
			{
				if (e.kind == ui::element_kind::control) static_cast<ui::control&>(e).options_changed();
			});
		}
	}

	// A control taking the focus is scrolled into view.
	void focus_changed(bool has_focus, const ui::control_ptr& child) override
	{
		if (has_focus && child) scroll_into_view(child->bounds);
	}

	// Scrolls the panel so what has the focus, at bounds in the panel's content, is in view.
	void scroll_into_view(const recti bounds)
	{
		const auto client = _scroller.client_bounds();
		auto y = _scroller.scroll_offset().y;

		if (bounds.top - y < client.top)
		{
			y = bounds.top - client.top;
		}
		else if (bounds.bottom - y > client.bottom)
		{
			y = bounds.bottom - client.bottom;
		}

		_scroller.scroll_offset(shared_from_this(), 0, y);
	}

	void command_hover(const ui::command_ptr& c, const recti window_bounds) override
	{
		_state.command_hover(c, window_bounds);
	}

	// bounds are this frame's; the menu opens where they are on the screen, as the view frame's do.
	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override
	{
		const auto f = frame();
		_state.track_menu(f, bounds.offset(f->client_to_screen({})), commands);
	}

	void invalidate_view(const view_invalid invalid) override { _state.invalidate_view(invalid); }

	void on_window_destroy() override
	{
		_active_controller.reset();
		_controls.clear();
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

	void draw(render::draw_context& /*rc*/) override {}

	platform::cursor cursor() const override { return platform::cursor::link; }

	void on_mouse_left_button_down(const pointi loc, const platform::key_state /*keys*/) override
	{
		_last_loc = loc;
		_parent._header_tracking = true;
		_host->frame()->invalidate();
	}

	void on_mouse_move(const pointi loc) override { _last_loc = loc; }

	void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
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

	void popup_from_location(view_hover_element& /*hover*/) override {}
};

// Drags a column divider, which resizes the column before it as in a file browser's details view: the
// columns before it stay put and the divider stays under the pointer, while the last column takes up the
// room it gives back or makes the room it takes. Only a drag that moves sizes anything, Escape takes the
// drag back, and a double-click fits the column to its content.
class column_divider_controller final : public view_controller
{
	list_view& _parent;
	int _col = 0;
	ui::table_layout::resize_gesture _drag;

public:
	column_divider_controller(const view_host_ptr& host, list_view& parent, const recti bounds, const int col) :
		view_controller(host, bounds), _parent(parent), _col(col)
	{
		_parent._divider_active = _col;
		_host->frame()->invalidate();
	}

	~column_divider_controller() override
	{
		if (_drag.active()) escape();
		_parent._divider_active = -1;
		_host->frame()->invalidate();
	}

	platform::cursor cursor() const override { return platform::cursor::left_right; }

	void on_mouse_left_button_down(const pointi loc, const platform::key_state /*keys*/) override
	{
		_drag.begin(_parent._table_columns, _col, loc.x);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_drag.update(_parent._table_columns, loc.x)) _parent._state.invalidate_view(view_invalid::view_layout);
	}

	void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
	{
		on_mouse_move(loc);
		_drag.finish();
	}

	void on_mouse_left_button_double_click(const pointi /*loc*/, const platform::key_state /*keys*/) override
	{
		_drag.finish();
		_parent.fit_column(_col);
	}

	bool escape() override
	{
		if (!_drag.cancel(_parent._table_columns)) return false;
		_parent._state.invalidate_view(view_invalid::view_layout);
		return true;
	}
};

inline view_controller_ptr list_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                               hit_test_context& ctx)
{
	if (_progress.active) return nullptr;

	// A divider sits over the edges of the headers either side of it, so it is found first.
	for (int i = 0; i < column_count() - 1 && !_rows.empty(); ++i)
	{
		if (ctx.occluded(_col_divider_bounds[i]))
		{
			return std::make_shared<column_divider_controller>(host, *this, _col_divider_bounds[i], i);
		}
	}

	for (int i = 0; i < column_count(); ++i)
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
