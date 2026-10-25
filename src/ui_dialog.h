// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Dialog management and layout. Provides dialog construction helpers,
// standard dialog layouts, and modal dialog handling.

#pragma once

#include <utility>

#include "libdesktop/ui.h"
#include "ui_elements.h"
#include "files.h"
#include "app_text.h"
#include "util_spell.h"

namespace ui
{
	bool browse_for_term(view_state& vs, const control_frame_ptr& parent, std::string& result);

	std::vector<recti> layout_collage(recti draw_bounds, const std::vector<sizei>& dimensions);

	static int default_control_height(measure_context& mc, const int lines = 1)
	{
		return mc.text_line_height(style::font_face::dialog) * lines + mc.padding2;
	}

	struct slider_row_layout
	{
		recti label;
		recti slider;
		recti edit;
		int height = 0;
		bool stacked = false;
	};

	inline slider_row_layout layout_slider_row(measure_context& mc, const std::string_view label, const recti bounds)
	{
		const auto width = std::max(0, bounds.width());
		const auto control_height = default_control_height(mc);
		const auto label_extent = label.empty() ? sizei{} :
			mc.measure_text(label, style::font_face::dialog, style::text_style::single_line, width);
		mc.col_widths.c1 = std::max(mc.col_widths.c1, label_extent.cx);
		const auto number_extent = mc.measure_text("00.00", style::font_face::dialog,
		                                          style::text_style::single_line, width);
		const auto edit_width = std::min(width, number_extent.cx + mc.padding2 * 2);
		const auto split = bounds.right - edit_width;
		const auto track_right = std::max(bounds.left, split - mc.padding1);
		const auto label_limit = std::max(0, track_right - bounds.left - control_height * 2 - mc.padding1);

		slider_row_layout result;
		result.stacked = !label.empty() && label_extent.cx > label_limit;
		auto controls_top = bounds.top;
		auto track_left = bounds.left;
		if (!label.empty())
		{
			if (result.stacked)
			{
				const auto extent = mc.measure_text(label, style::font_face::dialog, style::text_style::multiline, width);
				result.label = {bounds.left, bounds.top, bounds.right, bounds.top + extent.cy};
				controls_top = result.label.bottom + mc.padding1;
			}
			else
			{
				result.label = {bounds.left, bounds.top, bounds.left + std::min(mc.col_widths.c1, label_limit),
				                bounds.top + control_height};
				track_left = result.label.right + mc.padding1;
			}
		}
		result.slider = {track_left, controls_top, track_right, controls_top + control_height};
		result.edit = {std::min(bounds.right, split + mc.padding1), controls_top, bounds.right,
		               controls_top + control_height};
		result.height = controls_top + control_height - bounds.top;
		return result;
	}

	class auto_complete_match : public view_element
	{
	public:
		int weight = 0;

		auto_complete_match() noexcept = default;

		explicit auto_complete_match(const view_element_options& style_in) noexcept : view_element(style_in)
		{
		}

		~auto_complete_match() override = default;
		virtual std::string edit_text() const = 0;
	};

	using auto_complete_match_ptr = std::shared_ptr<auto_complete_match>;
	using auto_complete_results = std::vector<auto_complete_match_ptr>;
	using match_highlights = std::vector<str::part_t>;

	class complete_strategy_t
	{
	public:
		virtual ~complete_strategy_t() = default;
		bool resize_to_show_results = false;
		bool auto_select_first = true;
		bool folder_select_button = false;
		uint32_t max_predictions = 20u;

		enum class select_type
		{
			click,
			double_click,
			arrow,
			init
		};

		virtual std::string no_results_message() = 0;
		virtual void selected(const auto_complete_match_ptr& i, select_type st) = 0;
		virtual auto_complete_match_ptr selected() const = 0;
		virtual void search(const std::string& query, std::function<void(const auto_complete_results&)> complete) = 0;
		virtual void initialise(std::function<void(const auto_complete_results&)> complete) = 0;

		// Drop any cached vocabulary and selection. Not every strategy holds either.
		virtual void clear()
		{
		}
	};

	using complete_strategy_ptr = std::shared_ptr<complete_strategy_t>;

	class slider_control final : public view_element, public std::enable_shared_from_this<slider_control>
	{
		trackbar_ptr _slider;
		edit_ptr _edit;
		std::string _label;
		slider_row_layout _row;

		int& _val;
		int _min;
		int _max;
		std::function<void()> _changed;
		// Once a change is complete: the thumb let go, a key step, a click on the track. The values a
		// drag passes through arrive first through `_changed`, so an owner keeping history can make one
		// gesture one step.
		std::function<void()> _settled;
		bool _showing_bound_value = false;

	public:
		slider_control(const control_frame_ptr& h, const std::string_view label, int& v, const int min, const int max,
		               std::function<void()> changed = {}, std::function<void()> settled = {}) :
			_label(label), _val(v), _min(min), _max(max), _changed(std::move(changed)), _settled(std::move(settled))
		{
			edit_styles style;
			style.number = true;
			style.align_center = true;
			_edit = h->create_edit(style, {}, [this](const std::string_view text) { edit_change(text); });
			_slider = h->create_slider(min, max, [this](const int pos, const bool is_tracking)
			{
				slider_change(pos, is_tracking);
			});

			update_slider();
			update_edit();
		}

		void label(const std::string_view label)
		{
			_label = label;
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_slider);
			handler(_edit);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			return {cx, layout_slider_row(mc, _label, {0, 0, cx, 0}).height};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			_row = layout_slider_row(mc, _label, bounds);
			positions.emplace_back(_slider, _row.slider, is_visible());
			positions.emplace_back(_edit, _row.edit, is_visible());
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			if (!str::is_empty(_label))
			{
				const auto text_color = color(dc.colors.foreground, dc.colors.alpha);
				dc.draw_text(_label, _row.label.offset(element_offset), style::font_face::dialog,
				             _row.stacked ? style::text_style::multiline : style::text_style::single_line, text_color, {});
			}
		}

		void update_edit() const
		{
			const auto actual = _edit->window_text();
			const auto expected = str::to_string(_val);

			if (actual.empty() || str::to_int(actual) != str::to_int(expected))
			{
				_edit->window_text(expected);
			}
		}

		void update_slider() const
		{
			if (_val != _slider->get_pos())
			{
				_slider->SetPos(_val);
			}
		}

		// Shows the bound value again after its owner changed it directly, as an undo or a
		// reloaded document does. The native edit reports text written from code as typed, so the
		// change callback is muted while the value is written: an echo is not an edit.
		void show_bound_value()
		{
			_showing_bound_value = true;
			update_slider();
			update_edit();
			_showing_bound_value = false;
		}

		void edit_change(const std::string_view text)
		{
			if (_showing_bound_value) return;

			_val = str::to_int(text);
			update_slider();
			if (_changed) _changed();
		}

		void slider_change(const int pos, const bool is_tracking)
		{
			_val = pos;
			update_edit();
			if (_changed) _changed();
			if (!is_tracking && _settled) _settled();
		}
	};

	// A dialog row that reserves column 1 for a label and gives the rest to its control.
	class labelled_element : public view_element
	{
	protected:
		std::string _label;

		labelled_element() noexcept = default;

		explicit labelled_element(const std::string_view label) : _label(label)
		{
		}

		recti control_bounds(const measure_context& mc, const recti row) const
		{
			auto r = row;
			if (!str::is_empty(_label)) r.left += mc.col_widths.c1;
			return r;
		}

	public:
		void label(const std::string_view label)
		{
			_label = label;
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			if (!str::is_empty(_label))
			{
				const auto label_extent = mc.measure_text(_label, style::font_face::dialog,
				                                          style::text_style::single_line, cx);
				mc.col_widths.c1 = std::max(mc.col_widths.c1, label_extent.cx + mc.padding2);
			}

			return {cx, default_control_height(mc)};
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			if (!str::is_empty(_label))
			{
				auto r = bounds.offset(element_offset);
				r.right = r.left + dc.col_widths.c1 - dc.padding2;
				dc.draw_text(_label, r, style::font_face::dialog, style::text_style::single_line,
				             color(dc.colors.foreground, dc.colors.alpha), {});
			}
		}
	};

	// A labelled row whose control area is a single edit box.
	class labelled_edit_element : public labelled_element
	{
	protected:
		edit_ptr _edit;

		using labelled_element::labelled_element;

		// Splits the control area between the edit and a trailing toolbar button.
		void layout_edit_and_toolbar(const measure_context& mc, const toolbar_ptr& tb,
		                             control_layouts& positions) const
		{
			auto edit_bounds = control_bounds(mc, bounds);
			auto tb_bounds = bounds;
			const auto tb_extent = tb->measure_toolbar(50);
			const auto split = edit_bounds.right - tb_extent.cx - mc.padding2;
			tb_bounds.left = split + mc.padding2 / 2;
			tb_bounds.top += (tb_bounds.height() - tb_extent.cy) / 2;
			edit_bounds.right = split;

			positions.emplace_back(_edit, edit_bounds, is_visible());
			positions.emplace_back(tb, tb_bounds, is_visible());
		}

	public:
		void focus() const
		{
			if (_edit) _edit->focus();
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_edit);
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			positions.emplace_back(_edit, control_bounds(mc, bounds), is_visible());
		}
	};

	class edit_control final : public labelled_edit_element, public std::enable_shared_from_this<edit_control>
	{
		std::string& _val;

	public:
		edit_control(const control_frame_ptr& h,
		             const std::string_view label,
		             std::string& v,
		             std::function<void(std::string_view)> changed = {}) : labelled_edit_element(label), _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			_edit = h->create_edit(style, v, [this, changed](const std::string_view text)
			{
				_val = text;
				if (changed) changed(text);
			});
		}

		edit_control(const control_frame_ptr& h, std::string& v) : _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			_edit = h->create_edit(style, v, [this](const std::string_view text) { _val = text; });
		}

		edit_control(const control_frame_ptr& h, std::string& v,
		             const std::vector<std::string>& auto_completes) : _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.auto_complete_list = auto_completes;
			_edit = h->create_edit(style, v, [this](const std::string_view text) { _val = text; });
		}

		edit_control(const control_frame_ptr& h, const std::string_view label, std::string& v,
		             const std::vector<std::string>& auto_completes) : labelled_edit_element(label), _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.auto_complete_list = auto_completes;
			_edit = h->create_edit(style, v, [this](const std::string_view text) { _val = text; });
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				_edit->window_text(_val);
			}
		}

		void auto_completes(const std::vector<std::string>& texts) const
		{
			_edit->auto_completes(texts);
		}
	};

	class edit_picker_control final : public labelled_edit_element,
	                                  public std::enable_shared_from_this<edit_picker_control>
	{
		std::string& _val;
		toolbar_ptr _tb;

		std::vector<std::string> _auto_completes;
		control_frame_ptr _parent;

	public:
		edit_picker_control(control_frame_ptr parent, std::string& v,
		                    std::vector<std::string> auto_completes,
		                    std::function<void(std::string_view)> changed = {}) : _val(v),
			_auto_completes(std::move(auto_completes)), _parent(std::move(parent))
		{
			edit_styles style;
			style.horizontal_scroll = true;
			_edit = _parent->create_edit(style, v, [this, changed](const std::string_view text)
			{
				_val = text;
				if (changed) changed(text);
			});

			const auto c = std::make_shared<command>();
			c->icon = icon_index::add;
			c->invoke = [this] { show_menu(); };

			toolbar_styles styles;
			styles.xTBSTYLE_LIST = true;
			_tb = _parent->create_toolbar(styles, {c});
		}

		edit_picker_control(control_frame_ptr parent, const std::string_view label, std::string& v,
		                    std::vector<std::string> auto_completes) : labelled_edit_element(label), _val(v),
		                                                               _auto_completes(std::move(auto_completes)),
		                                                               _parent(std::move(parent))
		{
			edit_styles style;
			style.horizontal_scroll = true;
			_edit = _parent->create_edit(style, v, [this](const std::string_view text) { _val = text; });
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_edit);
			handler(_tb);
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				_edit->window_text(_val);
			}
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			layout_edit_and_toolbar(mc, _tb, positions);
		}

		void show_menu() const
		{
			std::vector<command_ptr> commands;

			for (const auto& s : _auto_completes)
			{
				auto c = std::make_shared<command>();
				c->text = s;
				c->invoke = [e = _edit, s]
				{
					e->window_text(s);
				};

				commands.emplace_back(c);
			}

			_parent->track_menu(_tb->window_bounds(), commands);
			_parent->invalidate();
		}
	};

	class password_control final : public labelled_edit_element, public std::enable_shared_from_this<password_control>
	{
		std::string& _val;

	public:
		password_control(const control_frame_ptr& h, const std::string_view label,
		                 std::string& v) : labelled_edit_element(label), _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.password = true;
			_edit = h->create_edit(style, v, [this](const std::string_view text) { _val = text; });
		}

		password_control(const control_frame_ptr& h, std::string& v) : _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.password = true;
			_edit = h->create_edit(style, v, [this](const std::string_view text) { _val = text; });
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				_edit->window_text(_val);
			}
		}
	};

	class multi_line_edit_control final : public view_element,
	                                      public std::enable_shared_from_this<multi_line_edit_control>
	{
		std::string& _text;
		edit_ptr _edit;
		const int _edit_height;
		std::function<void(std::string_view)> _changed;

	public:
		multi_line_edit_control(const control_frame_ptr& h, std::string& v, const int height = 6,
		                        const bool wants_return = false,
		                        std::function<void(std::string_view)> changed = {}) :
			_text(v), _edit_height(height), _changed(std::move(changed))
		{
			edit_styles style;
			style.vertical_scroll = true;
			style.multi_line = true;
			style.want_return = wants_return;
			style.spelling = &spell_dictionary();
			_edit = h->create_edit(style, v, [this](const std::string_view text)
			{
				_text = text;
				if (_changed) _changed(text);
			});
		}

		void add_word(const std::string_view s) const
		{
			_edit->replace_sel(s, true);
		}

		void focus() const
		{
			_edit->focus();
		}

		void text(const std::string_view text) const
		{
			_text = text;
			_edit->window_text(text);
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				_edit->window_text(_text);
			}
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_edit);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			return {cx, _edit_height * mc.text_line_height(style::font_face::dialog)};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			auto rEdit = bounds;
			positions.emplace_back(_edit, rEdit, is_visible());
		}
	};

	class recommended_words_control final : public view_element,
	                                        public std::enable_shared_from_this<recommended_words_control>
	{
		toolbar_ptr _tb;
		const std::function<void(std::string_view)> _f;

	public:
		using refresh_group = std::shared_ptr<std::vector<std::function<void()>>>;

		recommended_words_control(const control_frame_ptr& h, const std::vector<std::string_view>& words,
		                          const std::function<void(std::string_view)>& f,
		                          const std::function<bool(std::string_view)>& is_checked = {},
		                          refresh_group shared_refresh = {})
		{
			toolbar_styles styles;
			styles.xTBSTYLE_WRAPABLE = true;
			styles.xTBSTYLE_LIST = true;
			std::vector<command_ptr> commands;
			const auto toolbar = std::make_shared<std::weak_ptr<ui::toolbar>>();
			const auto refresh = shared_refresh
				                     ? std::move(shared_refresh)
				                     : std::make_shared<refresh_group::element_type>();
			std::vector<std::pair<std::string_view, std::weak_ptr<command>>> command_states;

			for (auto word : words)
			{
				const auto c = std::make_shared<command>();
				c->text = word;
				c->toolbar_text = word;
				c->checkable = static_cast<bool>(is_checked);
				c->checked = is_checked && is_checked(word);
				c->invoke = [f, is_checked, word, refresh]
				{
					f(word);
					if (is_checked)
					{
						for (const auto& update : *refresh) update();
					}
				};
				command_states.emplace_back(word, c);
				commands.emplace_back(c);
			}

			_tb = h->create_toolbar(styles, commands);
			*toolbar = _tb;
			refresh->emplace_back([is_checked, command_states = std::move(command_states), toolbar]
			{
				for (const auto& [word, weak_command] : command_states)
				{
					if (const auto command = weak_command.lock()) command->checked = is_checked && is_checked(word);
				}
				if (const auto control = toolbar->lock()) control->update_button_state(false, false);
			});
			refresh->back()();
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_tb);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			auto cy = 0;

			if (_tb)
			{
				const auto toolbar_extent = _tb->measure_toolbar(cx);
				cy += toolbar_extent.cy;
			}

			return {cx, cy};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			if (_tb)
			{
				positions.emplace_back(_tb, bounds, is_visible());
			}
		}
	};

	class num_control final : public labelled_edit_element, public std::enable_shared_from_this<num_control>
	{
		int& _val;
		bool _empty_zero = false;
		std::function<void(int)> _changed;

	public:
		num_control(const control_frame_ptr& h, const std::string_view label, int& v, const bool empty_zero = false,
		            std::function<void(int)> changed = {}) :
			labelled_edit_element(label), _val(v), _empty_zero(empty_zero), _changed(std::move(changed))
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.number = true;
			_edit = h->create_edit(style, empty_zero && v == 0 ? std::string{} : str::to_string(v),
			                       [this](const std::string_view text)
			                       {
				                       _val = str::to_int(text);
				                       if (_changed) _changed(_val);
			                       });
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				_edit->window_text(_empty_zero && _val == 0 ? std::string{} : str::to_string(_val));
			}
		}
	};

	class float_control final : public labelled_edit_element, public std::enable_shared_from_this<float_control>
	{
		double& _val;

	public:
		float_control(const control_frame_ptr& h, const std::string_view label, double& v) :
			labelled_edit_element(label), _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.number = true;
			_edit = h->create_edit(style, str::to_string(v, 5), [this](const std::string_view text)
			{
				_val = str::to_double(text);
			});
		}

		float_control(const control_frame_ptr& h, double& v) : _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.number = true;
			_edit = h->create_edit(style, str::to_string(v, 5), [this](const std::string_view text)
			{
				_val = str::to_double(text);
			});
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				_edit->window_text(str::to_string(_val, 5));
			}
		}
	};

	class num_pair_control final : public labelled_element, public std::enable_shared_from_this<num_pair_control>
	{
		df::xy8& _val;
		edit_ptr _edit1;
		edit_ptr _edit2;
		mutable recti _of_bounds;

	public:
		num_pair_control(const control_frame_ptr& h, const std::string_view label,
		                 df::xy8& v) : labelled_element(label), _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.number = true;
			_edit1 = h->create_edit(style, str::to_string(_val.x), [this](const std::string_view text)
			{
				_val.x = str::to_int(text);
			});
			_edit2 = h->create_edit(style, str::to_string(_val.y), [this](const std::string_view text)
			{
				_val.y = str::to_int(text);
			});
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_edit1);
			handler(_edit2);
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				_edit1->window_text(str::to_string(_val.x));
				const auto y_val = _val.y == 0 ? std::string{} : str::to_string(_val.y);
				_edit2->window_text(y_val);
			}
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			if (!str::is_empty(_label))
			{
				labelled_element::render(dc, element_offset);
				dc.draw_text(tt.num_of, _of_bounds.offset(element_offset), style::font_face::dialog,
				             style::text_style::single_line,
				             color(dc.colors.foreground, dc.colors.alpha), {});
			}
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			const auto controls_left = bounds.left + (str::is_empty(_label) ? 0 : mc.col_widths.c1);
			const auto of_extent = mc.measure_text(tt.num_of, style::font_face::dialog,
			                                       style::text_style::single_line, bounds.width());
			const auto desired_edit_width = df::round(60 * mc.scale_factor);
			const auto available_width = std::max(0, bounds.right - controls_left - of_extent.cx - mc.padding2 * 2);
			const auto edit_width = std::min(desired_edit_width, available_width / 2);

			const recti edit1_bounds{controls_left, bounds.top, controls_left + edit_width, bounds.bottom};
			_of_bounds = {
				edit1_bounds.right + mc.padding2, bounds.top,
				edit1_bounds.right + mc.padding2 + of_extent.cx, bounds.bottom
			};
			const recti edit2_bounds{
				_of_bounds.right + mc.padding2, bounds.top,
				_of_bounds.right + mc.padding2 + edit_width, bounds.bottom
			};

			positions.emplace_back(_edit1, edit1_bounds, is_visible());
			positions.emplace_back(_edit2, edit2_bounds, is_visible());
		}
	};

	class two_col_table_control final : public view_element, public std::enable_shared_from_this<two_col_table_control>
	{
		std::string* _val1;
		std::string* _val2;

		int _row_count = 0;
		mutable int _control_height = 0;

		std::vector<edit_ptr> _edit1;
		std::vector<edit_ptr> _edit2;

	public:
		two_col_table_control(const control_frame_ptr& h, std::string* v1, std::string* v2, const int row_count) :
			_val1(v1), _val2(v2), _row_count(row_count), _edit1(row_count), _edit2(row_count)
		{
			edit_styles style;
			style.horizontal_scroll = true;

			for (auto i = 0; i < row_count; i++)
			{
				_edit1[i] = h->create_edit(style, _val1[i], [this, i](const std::string_view text)
				{
					_val1[i] = text;
				});
				_edit2[i] = h->create_edit(style, _val2[i], [this, i](const std::string_view text)
				{
					_val2[i] = text;
				});
			}
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				for (auto i = 0; i < _row_count; i++)
				{
					_edit1[i]->window_text(_val1[i]);
					_edit2[i]->window_text(_val2[i]);
				}
			}
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			for (auto i = 0; i < _row_count; i++)
			{
				handler(_edit1[i]);
				handler(_edit2[i]);
			}
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			_control_height = default_control_height(mc);
			return {cx, (_control_height + mc.padding1) * (_row_count + 1)};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			for (auto i = 0; i < _row_count; i++)
			{
				const auto height = default_control_height(mc);
				const auto y = bounds.top + (height + mc.padding1) * (i + 1);
				const auto x = bounds.left + df::mul_div(bounds.width(), 1, 3);

				recti r1(bounds.left, y, x, y + height);
				recti r2(x + mc.padding1, y, bounds.right, y + height);

				positions.emplace_back(_edit1[i], r1, is_visible());
				positions.emplace_back(_edit2[i], r2, is_visible());
			}
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			const auto r = bounds.offset(element_offset);
			const auto y = r.top;
			const auto x = r.left + df::mul_div(r.width(), 1, 3);

			const recti r1(r.left, y, x, y + _control_height);
			const recti r2(x + dc.padding1, y, r.right, y + _control_height);

			dc.draw_text(tt.prop_name_title, r1, style::font_face::dialog, style::text_style::single_line_center,
			             color(dc.colors.foreground, dc.colors.alpha), {});
			dc.draw_text(tt.search_or_folder, r2, style::font_face::dialog, style::text_style::single_line_center,
			             color(dc.colors.foreground, dc.colors.alpha), {});
		}
	};


	class folder_picker_control final : public view_element, public std::enable_shared_from_this<folder_picker_control>
	{
		std::string& _text;
		edit_ptr _edit;
		toolbar_ptr _tb;
		bool _multiline = false;
		mutable int _edit_height = 0;
		std::function<void(std::string_view)> _changed;

	public:
		folder_picker_control(const control_frame_ptr& h, std::string& path, const bool multiline = false,
		                      std::function<void(std::string_view)> changed = {}) :
			_text(path),
			_multiline(multiline),
			_changed(std::move(changed))
		{
			edit_styles style;
			style.horizontal_scroll = true;
			style.file_system_auto_complete = !_multiline;
			style.multi_line = _multiline;
			style.want_return = _multiline;
			_edit = h->create_edit(style, path, [this](const std::string_view text)
			{
				_text = text;
				if (_changed) _changed(text);
			});

			const auto c = std::make_shared<command>();
			c->icon = icon_index::folder;
			c->text = _multiline ? tt.add_folder.sv() : std::string_view{};
			if (_multiline) c->toolbar_text = c->text;
			c->invoke = [this] { browse_for_folder(); };

			toolbar_styles styles;
			styles.xTBSTYLE_LIST = true;
			_tb = h->create_toolbar(styles, {c});
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_edit);
			handler(_tb);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			_edit_height = default_control_height(mc, _multiline ? 10 : 1);

			if (_multiline)
			{
				const auto tb_extent = _tb->measure_toolbar(50);
				return {cx, _edit_height + tb_extent.cy + mc.padding2};
			}

			return {cx, _edit_height};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			auto edit_bounds = bounds;
			auto tb_bounds = edit_bounds;

			if (_multiline)
			{
				edit_bounds.bottom = edit_bounds.top + _edit_height;

				const auto tb_extent = _tb->measure_toolbar(50);
				tb_bounds.left = tb_bounds.right - tb_extent.cx;
				tb_bounds.top = tb_bounds.bottom - tb_extent.cy;
			}
			else
			{
				const auto tb_extent = _tb->measure_toolbar(50);
				const auto split = edit_bounds.right - tb_extent.cx - mc.padding2;
				tb_bounds.left = split + mc.padding2 / 2;
				tb_bounds.top += (tb_bounds.height() - tb_extent.cy) / 2;
				edit_bounds.right = split;
			}

			positions.emplace_back(_edit, edit_bounds, is_visible());
			positions.emplace_back(_tb, tb_bounds, is_visible());
		}

		void browse_for_folder() const
		{
			auto existing_last_path = std::string(_text);

			if (_multiline)
			{
				const auto lines = str::split(existing_last_path, true,
				                              [](const char c) { return c == '\n' || c == '\r'; });

				if (!lines.empty())
				{
					existing_last_path = lines.back();
				}
			}

			const auto is_exclude = str::is_exclude(existing_last_path);
			auto browse_path = df::folder_path(
				str::trim(is_exclude ? existing_last_path.substr(1) : existing_last_path));

			if (platform::browse_for_folder(browse_path))
			{
				if (_multiline)
				{
					const auto text = std::string(browse_path.text());
					const auto original_len = static_cast<int>(_text.size());
					if (!_text.empty()) _text += "\r\n";
					_text += text;
					_edit->window_text(_text);
					_edit->select(original_len, -1);
				}
				else
				{
					auto text = std::string(browse_path.text());
					if (is_exclude) text.insert(text.begin(), '-');
					_text = text;
					_edit->window_text(_text);
					_edit->select_all();
				}

				if (_changed) _changed(_text);
				_edit->focus();
			}
		}
	};

	class term_picker_control final : public labelled_edit_element,
	                                  public std::enable_shared_from_this<term_picker_control>
	{
		const control_frame_ptr& _parent;
		view_state& _state;

		std::string& _val;
		toolbar_ptr _tb;

	public:
		term_picker_control(view_state& state, const control_frame_ptr& h, const std::string_view label,
		                    std::string& v) : labelled_edit_element(label), _parent(h), _state(state), _val(v)
		{
			edit_styles style;
			style.horizontal_scroll = true;
			_edit = h->create_edit(style, v, [this](const std::string_view text) { _val = text; });

			const auto c = std::make_shared<command>();
			c->icon = icon_index::add;
			c->invoke = [this] { browse_for_term(); };

			toolbar_styles styles;
			styles.xTBSTYLE_LIST = true;
			_tb = h->create_toolbar(styles, {c});
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::populate)
			{
				_edit->window_text(_val);
			}
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			layout_edit_and_toolbar(mc, _tb, positions);
		}

		void auto_completes(const std::vector<std::string>& texts) const
		{
			_edit->auto_completes(texts);
		}

		void browse_for_term() const
		{
			std::string term;

			if (ui::browse_for_term(_state, _parent, term))
			{
				const auto original_len = _val.size();
				if (!_val.empty()) _val += " ";
				_val += term;
				_edit->window_text(_val);
				_edit->select(static_cast<int>(original_len), -1);
				_edit->focus();
			}
		}
	};

	class select_control final : public labelled_element, public std::enable_shared_from_this<select_control>
	{
		std::string& _text;
		toolbar_ptr _tb;

		mutable recti _label_bounds;
		mutable recti _text_bounds;

		const control_frame_ptr& _parent;
		std::function<std::vector<command_ptr>(const std::shared_ptr<select_control>&)> _commands;

	public:
		select_control(const control_frame_ptr& h, const std::string_view label, std::string& text,
		               std::function<std::vector<command_ptr>(const std::shared_ptr<select_control>&)> commands) :
			labelled_element(label), _text(text), _parent(h), _commands(std::move(commands))
		{
			const auto c = std::make_shared<command>();
			c->icon = icon_index::add;
			c->invoke = [this] { show_menu(); };

			toolbar_styles styles;
			styles.xTBSTYLE_LIST = true;
			_tb = h->create_toolbar(styles, {c});
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_tb);
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			const auto tb_extent = _tb->measure_toolbar(50);
			const auto split = bounds.right - tb_extent.cx - mc.padding2;

			auto tb_bounds = bounds;
			tb_bounds.left = split + mc.padding2 / 2;
			tb_bounds.top += (tb_bounds.height() - tb_extent.cy) / 2;

			_label_bounds = bounds;
			_label_bounds.right = bounds.left + mc.col_widths.c1;

			_text_bounds = bounds;
			if (!str::is_empty(_label)) _text_bounds.left += mc.col_widths.c1 + mc.padding2;
			_text_bounds.right = tb_bounds.left - mc.padding2;

			positions.emplace_back(_tb, tb_bounds, is_visible());
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			const auto text_clr = color(dc.colors.foreground, dc.colors.alpha);

			if (!str::is_empty(_label))
			{
				const auto r = _label_bounds.offset(element_offset);
				dc.draw_text(_label, r, style::font_face::dialog, style::text_style::single_line, text_clr, {});
			}

			if (!str::is_empty(_text))
			{
				const auto r = _text_bounds.offset(element_offset);
				dc.draw_text(_text, r, style::font_face::dialog, style::text_style::single_line, text_clr, {});
			}
		}

		void update_text(const std::string_view text) const
		{
			_text = text;
			_parent->invalidate();
		}

		void show_menu()
		{
			_parent->track_menu(_tb->window_bounds(), _commands(shared_from_this()));
			_parent->invalidate();
		}
	};


	class date_control final : public labelled_element, public std::enable_shared_from_this<date_control>
	{
		df::date_t& _val;
		date_time_control_ptr _control;
		std::function<void()> _changed;

	public:
		date_control(const control_frame_ptr& h, const std::string_view label, df::date_t& val,
		             const bool include_time = true, std::function<void()> changed = {}) :
			labelled_element(label), _val(val), _changed(std::move(changed))
		{
			_control = h->
				create_date_time_control(val, [this](const df::date_t val) { on_changed(val); }, include_time);
		}

		date_control(const control_frame_ptr& h, df::date_t& val, const bool include_time = true,
		             std::function<void()> changed = {}) : _val(val), _changed(std::move(changed))
		{
			_control = h->
				create_date_time_control(val, [this](const df::date_t val) { on_changed(val); }, include_time);
		}

		void on_changed(const df::date_t val) const
		{
			_val = val;
			if (_changed) _changed();
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_control);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			if (!str::is_empty(_label))
			{
				const auto label_extent = mc.measure_text(_label, style::font_face::dialog,
				                                          style::text_style::single_line, cx);
				mc.col_widths.c1 = std::max(mc.col_widths.c1, label_extent.cx + mc.padding2);
				const auto control_extent = _control->measure(cx - mc.col_widths.c1);
				return {mc.col_widths.c1 + control_extent.cx, default_control_height(mc)};
			}
			return {_control->measure(cx).cx, default_control_height(mc)};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			positions.emplace_back(_control, control_bounds(mc, bounds), is_visible());
		}
	};

	class image_control final : public view_element, public std::enable_shared_from_this<image_control>
	{
		surface_ptr _image;
		mutable texture_ptr _tex;

	public:
		image_control(const control_frame_ptr& h, const app_resource i)
		{
			files ff;
			_image = ff.image_to_surface(load_app_resource(i));
		}

		~image_control() override
		{
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			return scale_dimensions(_image->dimensions(), std::clamp(cx, 300, 500), true);
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			if (!_tex)
			{
				const auto t = dc.create_texture();

				if (t && t->update(_image) != texture_update_result::failed)
				{
					_tex = t;
				}
			}

			if (_tex)
			{
				dc.draw_texture(_tex, bounds.offset(-element_offset));
			}
		}
	};

	class before_after_control final : public view_element, public std::enable_shared_from_this<before_after_control>
	{
		const_surface_ptr _before;
		const_surface_ptr _after;
		simple_transform _after_transform = simple_transform::none;

		mutable texture_ptr _tex_before;
		mutable texture_ptr _tex_after;

		mutable int _text_height = 0;

	public:
		before_after_control(const_surface_ptr before, const_surface_ptr after) : _before(std::move(before)),
			_after(std::move(after))
		{
		}

		before_after_control(const_surface_ptr before, const simple_transform after_transform) : _before(before),
			_after(std::move(before)), _after_transform(after_transform)
		{
		}

		static sizei transform_dimensions(sizei dims, const simple_transform t)
		{
			if (t == simple_transform::rot_90 || t == simple_transform::rot_270) std::swap(dims.cx, dims.cy);
			return dims;
		}

		static simple_transform preview_destination_transform(const simple_transform t)
		{
			if (t == simple_transform::rot_90) return simple_transform::rot_270;
			if (t == simple_transform::rot_270) return simple_transform::rot_90;
			return t;
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			_text_height = mc.text_line_height(style::font_face::dialog);

			const auto s1 = scale_dimensions(_before->dimensions(), cx / 2);
			const auto s2 = scale_dimensions(transform_dimensions(_after->dimensions(), _after_transform), cx / 2);
			return {cx, std::max(s1.cy, s2.cy) + _text_height};
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			const auto device_bounds = bounds.offset(element_offset);

			if (!_tex_before)
			{
				const auto t = dc.create_texture();

				if (t && t->update(_before) != texture_update_result::failed)
				{
					_tex_before = t;
				}
			}

			if (!_tex_after)
			{
				const auto t = dc.create_texture();

				if (t && t->update(_after) != texture_update_result::failed)
				{
					_tex_after = t;
				}
			}

			const auto center = device_bounds.center();
			constexpr auto font = style::font_face::dialog;
			const auto text_color = color(dc.colors.foreground, dc.colors.alpha);

			auto rImage = device_bounds;
			rImage.top += _text_height + dc.padding2;
			rImage.bottom -= dc.padding2;

			auto r1 = rImage;
			r1.right = center.x - 2;
			if (_tex_before) dc.draw_texture(_tex_before, scale_dimensions(_tex_before->dimensions(), r1));

			auto r2 = rImage;
			r2.left = center.x + 2;
			if (_tex_after)
			{
				const auto after_bounds = scale_dimensions(transform_dimensions(_tex_after->dimensions(),
				                                                               _after_transform), r2);
				const auto destination_transform = preview_destination_transform(_after_transform);
				const auto after_destination = destination_transform == simple_transform::none
					                               ? quadd(after_bounds)
					                               : quadd(after_bounds).transform(destination_transform);
				dc.draw_texture(_tex_after, after_destination, recti({}, _tex_after->dimensions()), dc.colors.alpha,
				                texture_sampler::bilinear);
			}

			r1.top -= _text_height;
			r2.top -= _text_height;

			r1.bottom = r1.top + _text_height;
			r2.bottom = r2.top + _text_height;

			dc.draw_text(tt.before, r1, font, style::text_style::single_line_center, text_color, {});
			dc.draw_text(tt.after, r2, font, style::text_style::single_line_center, text_color, {});
		}
	};

	class table_element final : public view_element, public std::enable_shared_from_this<table_element>
	{
		struct cell
		{
			view_element_ptr element;
			pointi offset;
			sizei extent;
		};

		struct row
		{
			std::vector<cell> cells;
			bool alt = false;
			int y = 0;
			int cy = 0;
		};

		mutable std::vector<row> _rows;
		view_scroller _scroller;
		mutable int _total_height = 0;

	public:
		static constexpr int max_cols = 8;
		bool no_shrink_col[max_cols] = {false, false, false, false, false, false};
		bool can_scroll = false;
		bool frame_when_empty = false;

		table_element(const view_element_options& s) : view_element(
			s | view_element_style::has_tooltip | view_element_style::can_invoke)
		{
		}

		table_element() : view_element(view_element_style::has_tooltip | view_element_style::can_invoke)
		{
		}

		void clear() const
		{
			_rows.clear();
		}

		void add(const std::vector<view_element_ptr>& elements) const
		{
			row r;
			for (auto e : elements)
			{
				r.cells.emplace_back(e);
			}
			_rows.emplace_back(r);
		}

		void add(const view_element_ptr& e1, const view_element_ptr& e2, const view_element_ptr& e3) const
		{
			row r;
			r.cells.emplace_back(e1);
			r.cells.emplace_back(e2);
			r.cells.emplace_back(e3);
			_rows.emplace_back(r);
		}

		void add(const view_element_ptr& e1, const view_element_ptr& e2) const
		{
			row r;
			r.cells.emplace_back(e1);
			r.cells.emplace_back(e2);
			_rows.emplace_back(r);
		}

		void add(const std::string_view label, const std::string_view text) const
		{
			row r;
			r.cells.emplace_back(std::make_shared<text_element>(label));
			r.cells.emplace_back(std::make_shared<text_element>(text));
			_rows.emplace_back(r);
		}

		void add(const icon_index i, const std::string_view label, const std::string_view text) const
		{
			row r;
			r.cells.emplace_back(std::make_shared<action_element>(label));
			r.cells.emplace_back(std::make_shared<text_element>(text));
			_rows.emplace_back(r);
		}

		void add(const std::string_view label, const std::string_view text1, const std::string_view text2) const
		{
			row r;
			r.cells.emplace_back(std::make_shared<text_element>(label));
			r.cells.emplace_back(std::make_shared<text_element>(text1));
			r.cells.emplace_back(std::make_shared<text_element>(text2));
			_rows.emplace_back(r);
		}

		void add(const std::string_view label, const color32 clr, const std::string_view text1,
		         const std::string_view text2) const
		{
			row r;
			auto label_element = std::make_shared<text_element>(label);
			if (clr) label_element->foreground_color(clr);
			r.cells.emplace_back(label_element);
			r.cells.emplace_back(std::make_shared<text_element>(text1));
			r.cells.emplace_back(std::make_shared<text_element>(text2));
			_rows.emplace_back(r);
		}

		void add(const std::string_view c1, const std::string_view c2, const std::string_view c3,
		         const std::string_view c4) const
		{
			row r;
			r.cells.emplace_back(std::make_shared<text_element>(c1));
			r.cells.emplace_back(std::make_shared<text_element>(c2));
			r.cells.emplace_back(std::make_shared<text_element>(c3));
			r.cells.emplace_back(std::make_shared<text_element>(c4));
			_rows.emplace_back(r);
		}

		void add(const std::string_view label, view_element_ptr e) const
		{
			row r;
			r.cells.emplace_back(std::make_shared<text_element>(label));
			r.cells.emplace_back(e);
			_rows.emplace_back(r);
		}

		void add(const std::string_view label, view_element_ptr e1, view_element_ptr e2) const
		{
			row r;
			r.cells.emplace_back(std::make_shared<text_element>(label));
			r.cells.emplace_back(e1);
			r.cells.emplace_back(e2);
			_rows.emplace_back(r);
		}

		void add(view_element_ptr e0, view_element_ptr e1, view_element_ptr e2, view_element_ptr e3) const
		{
			row r;
			r.cells.emplace_back(e0);
			r.cells.emplace_back(e1);
			r.cells.emplace_back(e2);
			r.cells.emplace_back(e3);
			_rows.emplace_back(r);
		}

		void add(view_element_ptr e0, view_element_ptr e1, view_element_ptr e2, view_element_ptr e3,
		         view_element_ptr e4) const
		{
			row r;
			r.cells.emplace_back(e0);
			r.cells.emplace_back(e1);
			r.cells.emplace_back(e2);
			r.cells.emplace_back(e3);
			r.cells.emplace_back(e4);
			_rows.emplace_back(r);
		}

		void add(const std::string_view label, view_element_ptr e1, view_element_ptr e2, view_element_ptr e3) const
		{
			row r;
			r.cells.emplace_back(std::make_shared<text_element>(label));
			r.cells.emplace_back(e1);
			r.cells.emplace_back(e2);
			r.cells.emplace_back(e3);
			_rows.emplace_back(r);
		}

		void add(const std::string_view label, view_element_ptr e1, view_element_ptr e2, view_element_ptr e3,
		         view_element_ptr e4) const
		{
			row r;
			r.cells.emplace_back(std::make_shared<text_element>(label));
			r.cells.emplace_back(e1);
			r.cells.emplace_back(e2);
			r.cells.emplace_back(e3);
			r.cells.emplace_back(e4);
			_rows.emplace_back(r);
		}

		bool is_control_area(const pointi loc, const pointi element_offset) const override
		{
			if (can_scroll && _scroller.can_scroll() && _scroller.scroll_bounds().contains(loc))
			{
				return true;
			}

			for (const auto& r : _rows)
			{
				for (const auto& c : r.cells)
				{
					if (c.element->is_control_area(loc, element_offset))
					{
						return true;
					}
				}
			}

			return false;
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			std::vector<int> col_widths;
			auto max_row_width = 0;


			const int scroll_padding = can_scroll && _scroller.can_scroll() ? mc.scroll_width : 0;
			const int cx_avail = cx - scroll_padding;

			for (const auto& r : _rows)
			{
				auto row_width = 0;
				if (col_widths.size() < r.cells.size())
				{
					col_widths.resize(r.cells.size(), 0);
				}

				for (auto i = 0u; i < r.cells.size(); ++i)
				{
					const auto padding = r.cells[i].element->porch() * mc.scale_factor;
					const auto extent = r.cells[i].element->measure(mc, cx_avail);
					row_width += col_widths[i] = std::max(col_widths[i], extent.cx + padding.cx * 2);
				}

				if (row_width > max_row_width)
				{
					max_row_width = row_width;
				}
			}

			if (max_row_width > cx_avail)
			{
				const auto target_width = std::max(0, cx_avail);
				auto fixed_width = 0;
				auto shrink_width = 0;

				for (auto i = 0u; i < col_widths.size(); ++i)
				{
					const auto no_shrink = i < max_cols && no_shrink_col[i];
					(no_shrink ? fixed_width : shrink_width) += col_widths[i];
				}

				if (fixed_width < target_width && shrink_width > 0)
				{
					const auto shrink_avail = target_width - fixed_width;
					for (auto i = 0u; i < col_widths.size(); ++i)
					{
						const auto no_shrink = i < max_cols && no_shrink_col[i];
						if (!no_shrink) col_widths[i] = df::mul_div(shrink_avail, col_widths[i], shrink_width);
					}
				}
				else if (max_row_width > 0)
				{
					for (auto& width : col_widths)
					{
						width = df::mul_div(target_width, width, max_row_width);
					}
				}

				max_row_width = target_width;
			}
			else if (max_row_width < cx_avail && !col_widths.empty() && flex.grow > 0.0f)
			{
				// Expand the last column to fill available width
				col_widths.back() += cx_avail - max_row_width;
				max_row_width = cx_avail;
			}

			_total_height = 0;

			for (auto&& r : _rows)
			{
				auto row_height = 0;
				auto x = 0;

				for (auto i = 0u; i < r.cells.size(); ++i)
				{
					const auto xx = col_widths[i];
					const auto padding = r.cells[i].element->porch() * mc.scale_factor;
					const auto extent = r.cells[i].element->measure(mc, xx - padding.cx * 2);
					const auto cell_height = extent.cy + padding.cy * 2;
					if (row_height < cell_height) row_height = cell_height;
					r.cells[i].extent = {xx - padding.cx * 2, extent.cy};
					r.cells[i].offset = {x + padding.cx, _total_height + padding.cy};
					x += xx;
				}

				r.y = _total_height;
				r.cy = row_height;
				_total_height += row_height;

				// vertical center
				for (auto&& c : r.cells)
				{
					const auto padding = c.element->porch() * mc.scale_factor;
					const auto yy = (row_height - c.extent.cy - padding.cy * 2) / 2;
					c.offset.y += yy;
				}
			}

			return {max_row_width + scroll_padding, _total_height};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			measure(mc, bounds_in.width());

			const auto needs_scroll = _total_height > bounds_in.height();
			const int scroll_cx = needs_scroll ? mc.scroll_width : 0;

			bounds = bounds_in;
			bool alt_row = false;

			for (auto&& r : _rows)
			{
				for (const auto& c : r.cells)
				{
					c.element->layout(mc, {bounds_in.top_left() + c.offset, c.extent}, positions);
				}

				r.alt = alt_row;
				alt_row = !alt_row;
			}

			if (can_scroll)
			{
				const recti scroll_bounds{
					bounds_in.right - scroll_cx, bounds_in.top, bounds_in.right, bounds_in.bottom
				};
				const recti client_bounds{bounds_in.left, bounds_in.top, bounds_in.right - scroll_cx, bounds_in.bottom};

				_scroller.layout({client_bounds.width(), _total_height}, client_bounds, scroll_bounds);
			}
			else
			{
				_scroller.clear();
			}
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			const auto scroll_offset = element_offset - _scroller.scroll_offset();
			const auto show_scroll = can_scroll && _scroller.can_scroll();

			if (show_scroll)
			{
				dc.clip_bounds(bounds);
			}

			if (_rows.empty())
			{
				if (frame_when_empty)
				{
					const auto clr = color(average(dc.colors.background, 0u), dc.colors.alpha);
					dc.draw_border(bounds.inflate(-8), bounds.inflate(-7), clr, clr);
				}
			}
			else
			{
				for (const auto& r : _rows)
				{
					auto row_bounds = bounds;
					row_bounds.top = bounds.top + r.y;
					row_bounds.bottom = bounds.top + r.y + r.cy;
					row_bounds = row_bounds.offset(scroll_offset);

					//if (row_bounds.intersects(clip_bounds))
					{
						if (r.alt)
						{
							auto alt_bounds = row_bounds;
							if (show_scroll) alt_bounds.right = _scroller.scroll_bounds().left;
							dc.draw_rounded_rect(alt_bounds, color(0x000000, dc.colors.alpha / 11.11f), dc.padding1);
						}

						for (const auto& e : r.cells)
						{
							e.element->render(dc, scroll_offset);
						}
					}
				}
			}

			if (show_scroll)
			{
				dc.restore_clip();
				_scroller.draw_scroll(dc);
			}
		}

		void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
		{
			for (const auto& r : _rows)
			{
				for (const auto& e : r.cells)
				{
					if (e.element->bounds.contains(loc - element_offset))
					{
						e.element->tooltip(hover, loc, element_offset);
						return;
					}
				}
			}
		}

		void hover(interaction_context& ic) override
		{
			for (const auto& r : _rows)
			{
				for (const auto& e : r.cells)
				{
					e.element->hover(ic);
				}
			}
		}

		void dispatch_event(const view_element_event& event) override
		{
			for (const auto& r : _rows)
			{
				for (const auto& e : r.cells)
				{
					e.element->dispatch_event(event);
				}
			}
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			for (const auto& r : _rows)
			{
				for (const auto& e : r.cells)
				{
					e.element->visit_controls(handler);
				}
			}
		}

		view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
		                                             const pointi element_offset,
		                                             hit_test_context& ctx) override
		{
			view_controller_ptr result;

			if (bounds.contains(loc - element_offset))
			{
				if (_scroller.can_scroll() && _scroller.scroll_bounds().contains(loc))
				{
					return std::make_shared<scroll_controller>(host, _scroller, _scroller.scroll_bounds());
				}

				const auto scroll_offset = element_offset - _scroller.scroll_offset();

				for (const auto& r : _rows)
				{
					for (const auto& e : r.cells)
					{
						if (e.element->bounds.contains(loc - scroll_offset))
						{
							result = e.element->controller_from_location(host, loc, scroll_offset, ctx);
							if (result)
							{
								return result;
							}
						}
					}
				}
			}

			return result;
		}

		void reverse() const
		{
			std::ranges::reverse(_rows);
		}
	};

	class settings_control final : public view_element, public std::enable_shared_from_this<settings_control>
	{
		button_ptr _button;
		std::string _text;
		mutable sizei _extent;
		control_frame_ptr _parent;

		const int button_width = 100;

	public:
		settings_control(const control_frame_ptr& h, const std::string_view text,
		                 std::function<void(void)> f) : _text(text), _parent(h)
		{
			_button = h->create_button(tt.button_change, std::move(f));
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_button);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			_extent = mc.measure_text(_text, style::font_face::dialog, style::text_style::multiline,
			                          cx - (button_width + 8));
			_extent.cx += 8;
			return {cx, std::max(_extent.cy, default_control_height(mc) + mc.padding2 * 2)};
		}

		void text(const std::string_view a)
		{
			_text = a;
			_parent->invalidate({});
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			auto r = bounds.offset(element_offset);
			r.right -= button_width + 8;
			r = center_rect(_extent, r);

			dc.draw_text(_text, r, style::font_face::dialog, style::text_style::multiline,
			             color(dc.colors.foreground, dc.colors.alpha), {});
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			auto r = bounds;
			r.right -= button_width + 8;
			r = center_rect(_extent, r);

			auto rr = bounds;
			rr.left = r.right + 8;
			rr.right = rr.left + button_width;
			rr = center_rect(sizei(button_width, 32), rr);

			positions.emplace_back(_button, rr, is_visible());
		}
	};

	class progress_control final : public view_element, public std::enable_shared_from_this<progress_control>
	{
		std::string _text;
		recti _rect_progress;
		recti _rect_text;
		mutable sizei _text_extent;
		int64_t _pos = 0;
		control_frame_ptr _parent;

	public:
		progress_control(control_frame_ptr h, const std::string_view a) : _text(a), _parent(std::move(h))
		{
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::tick && _pos == 0)
			{
				_parent->invalidate({}, false);
			}
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			measure_text(mc);
			constexpr auto prog_height = 16;
			return {cx, std::max(100, _text_extent.cy + prog_height)};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			layout(mc);
		}

		void measure_text(measure_context& mc) const
		{
			_text_extent = mc.measure_text(_text, style::font_face::dialog, style::text_style::multiline,
			                               bounds.width() - 32);
			_text_extent.cx += 4;
		}

		void layout(measure_context& mc)
		{
			layout();
		}

		void layout()
		{
			auto limit = bounds;
			if (_pos > 0) limit.bottom -= 16;
			const auto r = center_rect(_text_extent, limit);

			_rect_text.set(bounds.left, r.top, bounds.right, r.top + _text_extent.cy);
			int y_off = 4;
			_rect_progress.set(bounds.left, r.bottom + y_off, bounds.right, r.bottom + y_off + 8);
			if (_pos > 0) y_off += 16;
			_parent->invalidate();
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			render_progress(dc, _rect_progress.offset(-element_offset), _pos);
			dc.draw_text(_text, _rect_text.offset(-element_offset), style::font_face::dialog,
			             style::text_style::multiline_center, color(dc.colors.foreground, dc.colors.alpha), {});
		}

		static void render_progress(draw_context& dc, const recti progress_rect, const int64_t pos)
		{
			if (!progress_rect.is_empty())
			{
				const auto cc = color(dc.colors.background, dc.colors.alpha).scale(1.22f);

				if (pos > 0)
				{
					dc.draw_rect(progress_rect, cc);

					auto r = progress_rect.inflate(-2);
					r.right = r.left + static_cast<int>(df::mul_div(std::min<int64_t>(pos, 1000),
					                                                static_cast<int64_t>(r.width()), 1000ll));
					dc.draw_rect(r, color(0xFFFFFF, dc.colors.alpha * 0.22f));
				}
				else
				{
					dc.draw_rect(progress_rect, cc);

					const auto inner = progress_rect.inflate(-2);
					const auto segment_width = std::max(1, inner.width() / 5);
					const auto travel_width = inner.width() + segment_width;
					// Unsigned throughout: tick_count passes INT_MAX after ~24.8 days of uptime.
					const auto offset = static_cast<int>(platform::tick_count() / 20u %
						static_cast<uint32_t>(std::max(1, travel_width)));
					auto segment = inner;
					segment.left += offset - segment_width;
					segment.right = segment.left + segment_width;
					dc.draw_rect(segment.intersection(inner),
					             color(style::color::important_background, dc.colors.alpha));
				}
			}
		}

		void text(const std::string_view a)
		{
			_text = a;
			layout();
		}

		void message(const std::string_view m, const int64_t nn = 0, const int64_t dd = 0)
		{
			df::assert_true(is_ui_thread());

			const auto text_changed = _text != m;
			const auto pos = dd > 0ll ? df::mul_div(1000ll, nn, dd) : 0ll;

			if (_pos != pos || text_changed)
			{
				_text = m;
				_pos = pos;

				if (text_changed) _parent->layout();
				layout();
			}
		}
	};

	class title_control final : public view_element, public std::enable_shared_from_this<title_control>
	{
		std::string _text;
		icon_index _icon = icon_index::none;

	public:
		title_control(const std::string_view text) : _text(text)
		{
		}

		title_control(const icon_index& icon, const std::string_view text) : _text(text), _icon(icon)
		{
		}

		const std::string& text() const
		{
			return _text;
		}

		void text(const std::string_view a)
		{
			_text = a;
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			const auto icon_width = _icon == icon_index::none ? 0 : mc.icon_cxy + mc.padding2;
			const auto text_extent = mc.measure_text(_text, style::font_face::title, style::text_style::multiline,
			                                         cx - icon_width);

			return {text_extent.cx + icon_width, text_extent.cy};
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			auto r = bounds.offset(element_offset);
			const auto clr = color(dc.colors.foreground, dc.colors.alpha);

			if (_icon != icon_index::none)
			{
				auto icon_bounds = r;
				icon_bounds.right = icon_bounds.left + dc.icon_cxy;
				xdraw_icon(dc, _icon, icon_bounds, clr, {});
				r.left += dc.icon_cxy + dc.padding2;
			}

			dc.draw_text(_text, r, style::font_face::title, style::text_style::single_line, clr, {});
		}
	};

	class title_control2 final : public view_element, public std::enable_shared_from_this<title_control2>
	{
		std::string _text1;
		std::string _text2;
		mutable sizei _text_extent1;
		mutable sizei _text_extent2;
		mutable sizei _image_extent;
		icon_index _icon = icon_index::none;
		control_frame_ptr _parent;

		std::vector<const_surface_ptr> _surfaces;
		mutable std::vector<texture_ptr> _textures;
		mutable std::vector<sizei> _surface_extents;
		std::vector<recti> _surface_bounds;
		size_t _selection_overflow_count = 0;
		uint64_t _surface_generation = 0;

		const size_t max_surfaces = 7;

	public:
		title_control2(control_frame_ptr h, const std::string_view text,
		               const std::string_view text2) : _text1(text), _text2(text2), _parent(std::move(h))
		{
		}

		title_control2(control_frame_ptr h, const icon_index& icon, const std::string_view text,
		               const std::string_view text2) : _text1(text), _text2(text2), _icon(icon), _parent(std::move(h))
		{
		}

		title_control2(control_frame_ptr h, const std::string_view text, const std::string_view text2,
		               const std::vector<const_image_ptr>& images) : _text1(text), _text2(text2), _parent(std::move(h))
		{
			init(images);
		}

		title_control2(control_frame_ptr h, const icon_index& icon, const std::string_view text,
		               const std::string_view text2, const std::vector<const_image_ptr>& images,
		               const size_t selection_count) : _text1(text),
		                                               _text2(text2), _icon(icon), _parent(std::move(h))
		{
			init(images);
			_selection_overflow_count = selection_count - std::min(selection_count, _surfaces.size());
		}

		title_control2(control_frame_ptr h, const icon_index& icon, const std::string_view text,
		               const std::string_view text2, const std::vector<const_surface_ptr>& surfaces) : _text1(text),
			_text2(text2), _icon(icon), _parent(std::move(h))
		{
			init(surfaces);
		}

		title_control2(control_frame_ptr h, const icon_index& icon, const std::string_view text,
		               const std::string_view text2, const std::vector<const_surface_ptr>& surfaces,
		               const size_t selection_count) : _text1(text),
		                                               _text2(text2), _icon(icon), _parent(std::move(h))
		{
			init(surfaces);
			_selection_overflow_count = selection_count - std::min(selection_count, _surfaces.size());
		}

		void init(const std::vector<const_image_ptr>& images)
		{
			files ff;
			constexpr sizei max_dims(128, 128);

			for (const auto& image : images)
			{
				if (is_valid(image))
				{
					auto surface = ff.image_to_surface(image, scale_dimensions(image->dimensions(), max_dims));
					if (is_valid(surface)) _surfaces.emplace_back(std::move(surface));
					if (_surfaces.size() >= max_surfaces) break;
				}
			}
		}

		void init(const std::vector<const_surface_ptr>& surfaces)
		{
			files ff;
			constexpr sizei max_dims(128, 128);

			for (const auto& surface : surfaces)
			{
				if (is_valid(surface))
				{
					auto fitted = ff.fit_within(surface, max_dims);
					if (is_valid(fitted)) _surfaces.emplace_back(std::move(fitted));
					if (_surfaces.size() >= max_surfaces) break;
				}
			}
		}

		const std::string& text() const
		{
			return _text1;
		}

		void text(const std::string_view a)
		{
			_text1 = a;
		}

		void selection(const std::string_view text, const std::vector<const_surface_ptr>& surfaces,
		               const size_t selection_count)
		{
			++_surface_generation;
			_text2 = text;
			_surfaces.clear();
			_textures.clear();
			_surface_extents.clear();
			_surface_bounds.clear();
			init(surfaces);
			_selection_overflow_count = selection_count - std::min(selection_count, _surfaces.size());
		}

		void selection_async(const std::vector<const_image_ptr>& images, const size_t selection_count,
		                     df::async_i& async)
		{
			const auto generation = ++_surface_generation;
			const auto weak = weak_from_this();
			const auto max = max_surfaces;
			auto staged_images = images;

			async.queue_async(async_queue::render, [weak, generation, max, selection_count,
			                                        staged_images = std::move(staged_images), &async]() mutable
			{
				files ff;
				constexpr sizei max_dims(128, 128);
				std::vector<const_surface_ptr> surfaces;
				surfaces.reserve(std::min(max, staged_images.size()));

				for (const auto& image : staged_images)
				{
					if (is_valid(image))
					{
						auto surface = ff.image_to_surface(image, scale_dimensions(image->dimensions(), max_dims));
						if (is_valid(surface)) surfaces.emplace_back(std::move(surface));
						if (surfaces.size() >= max) break;
					}
				}

				async.queue_ui([weak, generation, selection_count, surfaces = std::move(surfaces)]() mutable
				{
					const auto self = weak.lock();
					if (!self || self->_surface_generation != generation) return;

					self->selection(self->_text2, surfaces, selection_count);
					self->_parent->layout();
					self->_parent->invalidate();
				});
			});
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			const auto icon_width = _icon == icon_index::none ? 0 : mc.icon_cxy * 2;

			_text_extent1 = mc.measure_text(_text1, style::font_face::title, style::text_style::multiline,
			                                cx - icon_width);
			_text_extent2 = mc.measure_text(_text2, style::font_face::dialog, style::text_style::multiline,
			                                cx - icon_width);

			for (int i = 0; i < 2; ++i)
			{
				const int text_height = _text_extent1.cy + _text_extent2.cy;
				const auto image_extent = measure_images(mc, std::min(cx - icon_width, cx / 2),
				                                         std::min(text_height, 100));
				const auto max_text_width = cx - (image_extent.cx + mc.padding2 + icon_width);

				_text_extent1 = mc.measure_text(_text1, style::font_face::title, style::text_style::multiline,
				                                max_text_width);
				_text_extent2 = mc.measure_text(_text2, style::font_face::dialog, style::text_style::multiline,
				                                max_text_width);

				_image_extent = image_extent;
			}

			return {cx, std::max(_text_extent1.cy + _text_extent2.cy + mc.padding2, _image_extent.cy)};
		}

		sizei measure_images(const measure_context& mc, const int avail_width, const int preferred_height) const
		{
			auto dims = surface_dims();

			const auto cx_slot = df::mul_div(avail_width, 2, static_cast<int>(dims.size()));

			for (auto&& d : dims)
			{
				d = scale_dimensions(d, sizei{cx_slot, preferred_height}, true);
			}

			auto total_width = 0;

			for (const auto& d : dims)
			{
				total_width += d.cx + mc.padding1;
			}

			if (total_width > avail_width)
			{
				for (auto&& d : dims)
				{
					d = scale_dimensions(d, sizei{df::mul_div(d.cx, avail_width, total_width), preferred_height}, true);
				}
			}

			total_width = 0;
			auto max_height = 0;

			for (const auto& d : dims)
			{
				total_width += d.cx + mc.padding1;
				max_height = std::max(max_height, d.cy);
			}

			_surface_extents = dims;

			return {total_width, max_height};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			auto total_width = 0;

			for (const auto& d : _surface_extents)
			{
				total_width += d.cx + mc.padding1;
			}

			_surface_bounds.clear();
			_surface_bounds.reserve(_surface_extents.size());

			int x = bounds.right - (total_width + mc.padding2);

			for (const auto& d : _surface_extents)
			{
				const auto y = bounds.top + (bounds.height() - d.cy) / 2;
				_surface_bounds.emplace_back(x, y, x + d.cx, y + d.cy);
				x += d.cx + mc.padding1;
			}
		}

		std::vector<sizei> surface_dims() const
		{
			std::vector<sizei> results;
			results.reserve(_surfaces.size() + (_selection_overflow_count > 0 ? 1 : 0));

			for (const auto& s : _surfaces)
			{
				auto dimensions = s->dimensions();
				if (setting.show_rotated && flips_xy(s->orientation()))
				{
					std::swap(dimensions.cx, dimensions.cy);
				}
				results.emplace_back(dimensions);
			}

			if (_selection_overflow_count > 0)
			{
				results.emplace_back(results.empty() ? sizei{128, 128} : results.back());
			}

			return results;
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			auto r = bounds.offset(element_offset);

			if (_icon != icon_index::none)
			{
				auto icon_bounds = r;
				r.left = icon_bounds.right = icon_bounds.left + dc.icon_cxy * 2;

				if (!_text2.empty())
				{
					icon_bounds.bottom = icon_bounds.top + _text_extent1.cy;
				}

				xdraw_icon(dc, _icon, icon_bounds, color(dc.colors.foreground, dc.colors.alpha), {});
			}

			if (_text2.empty())
			{
				auto text_rect1 = r;
				text_rect1.right = text_rect1.left + _text_extent1.cx;
				dc.draw_text(_text1, text_rect1, style::font_face::title, style::text_style::multiline,
				             color(dc.colors.foreground, dc.colors.alpha), {});
			}
			else
			{
				auto text_rect1 = r;
				text_rect1.bottom = text_rect1.top + _text_extent1.cy;
				text_rect1.right = text_rect1.left + _text_extent1.cx;

				auto text_rect2 = r;
				text_rect2.bottom -= dc.padding2;
				text_rect2.top = text_rect2.bottom - _text_extent2.cy;
				text_rect2.right = text_rect2.left + _text_extent2.cx;

				dc.draw_text(_text1, text_rect1, style::font_face::title, style::text_style::multiline,
				             color(dc.colors.foreground, dc.colors.alpha), {});
				dc.draw_text(_text2, text_rect2, style::font_face::dialog, style::text_style::multiline,
				             color(dc.colors.foreground, dc.colors.alpha), {});
			}

			if (_textures.size() != _surfaces.size()) _textures.resize(_surfaces.size());

			for (auto index = 0u; index < _surfaces.size(); ++index)
			{
				if (_textures[index]) continue;

				auto t = dc.create_texture();

				if (t && t->update(_surfaces[index]) != texture_update_result::failed) _textures[index] = std::move(t);
			}

			for (auto i = 0u; i < std::min(_surfaces.size(), _surface_bounds.size()); ++i)
			{
				const auto& texture = _textures[i];
				if (!texture) continue;
				const auto orientation = _surfaces[i]->orientation();
				const auto draw_bounds = _surface_bounds[i].offset(element_offset);
				const auto destination = setting.show_rotated
					                         ? quadd(draw_bounds).transform(to_simple_transform(orientation))
					                         : quadd(draw_bounds);
				const auto source_bounds = recti({}, texture->dimensions());
				const auto sampler = draw_bounds.width() < source_bounds.width()
					                     ? texture_sampler::bicubic
					                     : texture_sampler::bilinear;
				dc.draw_texture(texture, destination, source_bounds, dc.colors.alpha, sampler);
			}

			if (_selection_overflow_count > 0 && _surface_bounds.size() > _surfaces.size())
			{
				const auto overflow_bounds = _surface_bounds[_surfaces.size()].offset(element_offset);
				const auto text = std::format("+{}", _selection_overflow_count);
				dc.draw_rect(overflow_bounds, color(style::color::group_background, dc.colors.alpha));
				dc.draw_text(text, overflow_bounds, style::font_face::dialog, style::text_style::single_line_center,
				             color(dc.colors.foreground, dc.colors.alpha), {});
			}
		}
	};

	class selection_thumbnails_control final : public view_element,
	                                         public std::enable_shared_from_this<selection_thumbnails_control>
	{
		// Weak, because the frame's own child controls hold this strip in their callbacks. A strong
		// reference closes a cycle whose last link is released while the frame is clearing those
		// children, destroying the frame inside its own teardown.
		control_frame_weak_ptr _parent;
		std::vector<const_surface_ptr> _surfaces;
		mutable std::vector<texture_ptr> _textures;
		mutable std::vector<sizei> _surface_extents;
		std::vector<recti> _surface_bounds;
		size_t _overflow_count = 0;
		uint64_t _surface_generation = 0;
		static constexpr size_t max_surfaces = 7;

	public:
		explicit selection_thumbnails_control(control_frame_ptr parent) : _parent(std::move(parent))
		{
		}

		void selection(const std::vector<const_surface_ptr>& surfaces, const size_t selection_count)
		{
			++_surface_generation;
			_surfaces.clear();
			_textures.clear();
			_surface_extents.clear();
			_surface_bounds.clear();

			constexpr sizei max_dims(128, 128);
			files ff;
			for (const auto& surface : surfaces)
			{
				if (is_valid(surface))
				{
					auto fitted = ff.fit_within(surface, max_dims);
					if (is_valid(fitted)) _surfaces.emplace_back(std::move(fitted));
					if (_surfaces.size() >= max_surfaces) break;
				}
			}
			_overflow_count = _surfaces.empty()
				                  ? 0
				                  : selection_count - std::min(selection_count, _surfaces.size());
			is_visible(!_surfaces.empty());
		}

		void selection_async(const std::vector<const_image_ptr>& images, const size_t selection_count,
		                     df::async_i& async)
		{
			const auto generation = ++_surface_generation;
			const auto weak = weak_from_this();
			auto staged_images = images;

			async.queue_async(async_queue::render, [weak, generation, selection_count,
			                                        staged_images = std::move(staged_images), &async]() mutable
			{
				files ff;
				constexpr sizei max_dims(128, 128);
				std::vector<const_surface_ptr> surfaces;
				surfaces.reserve(std::min(max_surfaces, staged_images.size()));

				for (const auto& image : staged_images)
				{
					if (is_valid(image))
					{
						auto surface = ff.image_to_surface(image, scale_dimensions(image->dimensions(), max_dims));
						if (is_valid(surface)) surfaces.emplace_back(std::move(surface));
						if (surfaces.size() >= max_surfaces) break;
					}
				}

				async.queue_ui([weak, generation, selection_count, surfaces = std::move(surfaces)]() mutable
				{
					const auto self = weak.lock();
					if (!self || self->_surface_generation != generation) return;

					self->selection(surfaces, selection_count);

					if (const auto parent = self->_parent.lock())
					{
						parent->layout();
						parent->invalidate();
					}
				});
			});
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			auto dimensions = surface_dims();
			const auto slot_width = dimensions.empty() ? cx : df::mul_div(cx, 2, static_cast<int>(dimensions.size()));
			for (auto& size : dimensions) size = scale_dimensions(size, sizei{slot_width, 72}, true);

			auto total_width = 0;
			for (const auto size : dimensions) total_width += size.cx + mc.padding1;
			if (total_width > cx)
			{
				for (auto& size : dimensions)
					size = scale_dimensions(size, sizei{df::mul_div(size.cx, cx, total_width), 72}, true);
			}

			_surface_extents = dimensions;
			auto max_height = 0;
			for (const auto size : dimensions) max_height = std::max(max_height, size.cy);
			return {cx, max_height};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts&) override
		{
			bounds = bounds_in;
			_surface_bounds.clear();
			auto strip_width = 0;
			for (const auto size : _surface_extents) strip_width += size.cx;
			if (!_surface_extents.empty()) strip_width += mc.padding1 * (static_cast<int>(_surface_extents.size()) - 1);

			auto x = bounds.left + std::max(0, (bounds.width() - strip_width) / 2);
			for (const auto size : _surface_extents)
			{
				_surface_bounds.emplace_back(x, bounds.top, x + size.cx, bounds.top + size.cy);
				x += size.cx + mc.padding1;
			}
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			if (_textures.size() != _surfaces.size()) _textures.resize(_surfaces.size());

			for (auto index = 0u; index < _surfaces.size(); ++index)
			{
				if (_textures[index]) continue;
				auto texture = dc.create_texture();
				if (texture && texture->update(_surfaces[index]) != texture_update_result::failed)
					_textures[index] = std::move(texture);
			}

			for (auto index = 0u; index < std::min(_surfaces.size(), _surface_bounds.size()); ++index)
			{
				if (!_textures[index]) continue;
				const auto draw_bounds = _surface_bounds[index].offset(element_offset);
				const auto orientation = _surfaces[index]->orientation();
				const auto destination = setting.show_rotated
					                         ? quadd(draw_bounds).transform(to_simple_transform(orientation))
					                         : quadd(draw_bounds);
				const auto source = recti({}, _textures[index]->dimensions());
				dc.draw_texture(_textures[index], destination, source, dc.colors.alpha,
				                draw_bounds.width() < source.width()
					                ? texture_sampler::bicubic
					                : texture_sampler::bilinear);
			}

			if (_overflow_count > 0 && _surface_bounds.size() > _surfaces.size())
			{
				const auto overflow_bounds = _surface_bounds[_surfaces.size()].offset(element_offset);
				dc.draw_rect(overflow_bounds, color(style::color::group_background, dc.colors.alpha));
				dc.draw_text(std::format("+{}", _overflow_count), overflow_bounds, style::font_face::dialog,
				             style::text_style::single_line_center, color(dc.colors.foreground, dc.colors.alpha), {});
			}
		}

	private:
		std::vector<sizei> surface_dims() const
		{
			std::vector<sizei> result;
			result.reserve(_surfaces.size() + (_overflow_count > 0 ? 1 : 0));
			for (const auto& surface : _surfaces)
			{
				auto dimensions = surface->dimensions();
				if (setting.show_rotated && flips_xy(surface->orientation())) std::swap(dimensions.cx, dimensions.cy);
				result.emplace_back(dimensions);
			}
			if (_overflow_count > 0) result.emplace_back(result.empty() ? sizei{128, 72} : result.back());
			return result;
		}
	};

	// A stack of dialog controls. The surrounding flex container already reserves this element's
	// porch, so the group adds no padding of its own.
	class group_control final : public view_elements
	{
	public:
		group_control()
		{
			init();
		}

		explicit group_control(const view_element_options& style_in) : view_elements(style_in)
		{
			init();
		}

	private:
		void init()
		{
			flex_container.direction = flex_direction::column;
			flex_container.wrap = flex_wrap::no_wrap;
			flex_container.align_items = flex_align::start;
		}
	};

	// Side-by-side dialog columns. A column with a preferred width keeps it and gives up space
	// under pressure; the rest share what is left. In compact mode the sharing columns are
	// capped at the widest content so a short list of check boxes is not stretched apart.
	class col_control final : public view_elements
	{
		std::vector<screen_units> _preferred_widths;

	public:
		bool compact = false;

		col_control()
		{
			init();
		}

		explicit col_control(const std::vector<view_element_ptr>& controls)
		{
			init();
			for (const auto& c : controls) add(c);
		}

		void add(const view_element_ptr& p, const screen_units width = {0})
		{
			view_elements::add(p);
			_preferred_widths.emplace_back(width);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			if (_children.empty()) return {cx, 0};
			apply_column_flex(mc, cx);
			return view_elements::measure(mc, cx);
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			apply_column_flex(mc, bounds_in.width());
			view_elements::layout(mc, bounds_in, positions);
		}

	private:
		void init()
		{
			flex_container.direction = flex_direction::row;
			flex_container.wrap = flex_wrap::no_wrap;
			flex_container.justify = flex_justify::center;
			flex_container.align_items = flex_align::stretch;
		}

		flex_container_layout container_layout(const measure_context& mc) const override
		{
			auto result = flex_container;
			result.gap.cx = df::round(mc.padding2 / mc.scale_factor);
			return result;
		}

		void apply_column_flex(measure_context& mc, const int cx) const
		{
			for (auto i = 0u; i < _children.size(); ++i)
			{
				auto& flex = _children[i]->flex;

				if (_preferred_widths[i].n > 0)
				{
					flex.basis = _preferred_widths[i].n * 10;
					flex.grow = 0.0f;
					flex.shrink = 1.0f;
				}
				else
				{
					flex.basis = 0;
					flex.grow = 1.0f;
					flex.shrink = 0.0f;
				}

				flex.min_size.cx = 0;
				flex.max_size.cx = INT_MAX;
			}

			if (!compact) return;

			const auto shared = calc_flex_layout(_children, mc, {cx, -1}, container_layout(mc));
			auto max_width = 0;

			for (auto i = 0u; i < _children.size(); ++i)
			{
				if (_children[i]->is_visible() && _preferred_widths[i].n == 0)
				{
					max_width = std::max(max_width, _children[i]->measure(mc, shared.layout_bounds[i].width()).cx);
				}
			}

			for (auto i = 0u; i < _children.size(); ++i)
			{
				if (_children[i]->is_visible() && _preferred_widths[i].n == 0)
				{
					_children[i]->flex.max_size.cx = df::round(max_width / mc.scale_factor);
				}
			}
		}
	};

	class busy_control final : public view_element, public std::enable_shared_from_this<busy_control>
	{
		std::string _text;
		mutable sizei _extent;
		icon_index _icon = icon_index::none;
		control_frame_ptr _parent;

	public:
		busy_control(control_frame_ptr h, const icon_index& icon, const std::string_view text) : _text(text),
			_icon(icon), _parent(std::move(h))
		{
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::tick)
			{
				_parent->invalidate({}, false);
			}
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			_extent = mc.measure_text(_text, style::font_face::dialog, style::text_style::multiline, cx - 32);
			return {cx, std::max(64, _extent.cy + 8)};
		}

		void text(const std::string_view a)
		{
			_text = a;
		}

		static void draw_animation(draw_context& dc, const recti bounds_in)
		{
			auto bounds = bounds_in;
			bounds.top = bounds.bottom - 8;

			dc.draw_rect(bounds, color(lighten(dc.colors.background, 0.11f), dc.colors.alpha));

			const auto inner = bounds.inflate(-2);
			const auto cx = inner.width() / 5;
			// Unsigned throughout: tick_count passes INT_MAX after ~24.8 days of uptime.
			const auto xx = static_cast<int>(platform::tick_count() / 20u %
				static_cast<uint32_t>(std::max(1, inner.width())));

			auto draw_bounds = inner;
			draw_bounds.left = draw_bounds.left + xx - cx;
			draw_bounds.right = draw_bounds.left + xx + cx;
			dc.draw_rect(draw_bounds.intersection(inner),
			             color(style::color::dialog_selected_background, dc.colors.alpha));
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			const auto icon_width = _icon != icon_index::none ? dc.icon_cxy : 0;
			auto r = bounds.offset(element_offset);
			draw_animation(dc, r);

			r.bottom -= 16;
			r.top += (r.height() - _extent.cy) / 2;
			r.left += (r.width() - (_extent.cx + icon_width)) / 2;

			if (_icon != icon_index::none)
			{
				auto icon_bounds = r;
				icon_bounds.right = icon_bounds.left + dc.icon_cxy;
				xdraw_icon(dc, _icon, icon_bounds, color(dc.colors.foreground, dc.colors.alpha), {});
				r.left += dc.icon_cxy + 8;
			}

			dc.draw_text(_text, r, style::font_face::dialog, style::text_style::multiline,
			             color(dc.colors.foreground, dc.colors.alpha), {});
		}
	};

	static void enable_element(const view_element_ptr& e, bool enable)
	{
		const auto enabler = [enable](const control_base_ptr& c) { c->enable(enable); };
		e->visit_controls(enabler);
	}

	// Showing is left to the next layout pass, which knows where the controls belong and shows them
	// as it positions them; only hiding has to reach the windows directly.
	static void show_element(const view_element_ptr& e, const bool show)
	{
		e->is_visible(show);

		if (!show)
		{
			const auto hider = [](const control_base_ptr& c) { c->show(false); };
			e->visit_controls(hider);
		}
	}

	class check_control final : public view_element, public std::enable_shared_from_this<check_control>
	{
		button_ptr _check;
		bool& _val;
		bool _is_wide_format;
		bool _collapse_child = false;
		mutable sizei _check_extent;
		mutable sizei _child_extent;
		view_element_ptr _child;
		icon_index _icon = icon_index::none;
		std::string _details;

		bool child_shown() const
		{
			return _child && (!_collapse_child || _val);
		}

	public:
		check_control(const control_frame_ptr& h, const std::string_view text, bool& val, const bool is_radio = false,
		              const bool is_wide_format = false,
		              std::function<void(bool checked)> f = {},
		              const int radio_group = radio_group_default) : _val(val), _is_wide_format(is_wide_format)
		{
			_check = h->create_check_button(_val, text, is_radio, [this, f = std::move(f)](const bool checked)
			{
				on_check(checked);
				if (f) f(checked);
			}, radio_group);
		}

		check_control(const control_frame_ptr& h, const icon_index icon, const std::string_view text, bool& val,
		              const bool is_radio = false, const bool is_wide_format = false,
		              std::function<void(bool checked)> f = nullptr,
		              const int radio_group = radio_group_default) : _val(val), _is_wide_format(is_wide_format),
		                                                             _icon(icon)
		{
			_check = h->create_check_button(_val, text, is_radio, [this, f = std::move(f)](const bool checked)
			{
				on_check(checked);
				if (f) f(checked);
			}, radio_group);
		}

		void details(const std::string_view details)
		{
			_details = details;
		}

		void child(const view_element_ptr& child)
		{
			_child = child;
		}

		// Opt in to collapsing: an unchecked box then shows only its label, so a long form of
		// optional fields reads as a list of the fields actually in play.
		void collapse_child_when_unchecked()
		{
			_collapse_child = true;
			if (_child) show_element(_child, _val);
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			if (_child) _child->visit_controls(handler);
			handler(_check);
		}

		bool child_interactive() const
		{
			return _val && child_shown() && _child && _child->is_visible();
		}

		void on_check(const bool checked) const
		{
			_val = checked;
			if (_child) enable_element(_child, checked);
			if (_child && _collapse_child) show_element(_child, checked);
		}

		// Ticks the box from code, for example when another control makes the option
		// meaningful. The bound value and any child follow, but the change callback does
		// not run: the caller is the one making the decision.
		void checked(const bool checked)
		{
			if (_val == checked) return;
			on_check(checked);
			_check->set_checked(checked);
		}

		// Shows the bound value again after its owner changed it directly, as an undo or a
		// reloaded document does. checked() cannot: it compares against the bound value, which
		// already holds the new answer. The change callback does not run.
		void show_bound_value() const
		{
			on_check(_val);
			_check->set_checked(_val);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			if (_is_wide_format)
			{
				_check_extent = _check->measure(cx / 2);

				if (child_shown())
				{
					_child_extent = _child->measure(mc, cx - (_check_extent.cx + mc.padding2));
				}
				else
				{
					_child_extent = sizei(0, 0);
				}

				return {cx, std::max(_child_extent.cy, _check_extent.cy)};
			}


			_check_extent = _check->measure(cx);
			auto cy_result = _check_extent.cy;
			const auto indent = mc.padding2 * 3;

			if (child_shown())
			{
				_child_extent = _child->measure(mc, cx - (indent + mc.padding2));
				cy_result += _child_extent.cy;
			}
			else
			{
				_child_extent = sizei(0, 0);
			}

			return {std::max(_check_extent.cx, _child_extent.cx + indent), cy_result};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			positions.emplace_back(_check, recti(bounds.top_left(), _check_extent), is_visible());

			// A collapsed child is left out of the layout entirely: windows absent from the applied
			// positions keep the hidden state on_check gave them.
			if (!child_shown()) return;

			if (_is_wide_format)
			{
				auto child_bounds = bounds;
				child_bounds.left += _check_extent.cx + mc.padding2;
				_child->layout(mc, child_bounds, positions);
			}
			else
			{
				const auto indent = mc.padding2 * 3;
				auto child_bounds = bounds;
				child_bounds.left += indent;
				child_bounds.top += _check_extent.cy;
				_child->layout(mc, child_bounds, positions);
			}
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
			if (child_shown() && _child->is_visible())
			{
				_child->render(dc, element_offset);
			}
		}

		bool is_control_area(const pointi loc, const pointi element_offset) const override
		{
			return child_interactive() && _child->is_control_area(loc, element_offset);
		}

		view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
		                                             const pointi element_offset,
		                                             hit_test_context& ctx) override
		{
			return child_interactive() ? _child->controller_from_location(host, loc, element_offset, ctx) : nullptr;
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::initialise)
			{
				if (_child) enable_element(_child, _val);
				if (_child && _collapse_child) show_element(_child, _val);
			}
		}
	};

	class button_control final : public view_element, public std::enable_shared_from_this<button_control>
	{
		button_ptr _button;
		icon_index _icon = icon_index::none;
		std::string _details;

	public:
		button_control(const control_frame_ptr& h, const icon_index icon, const std::string_view title,
		               const std::string_view details, std::function<void(void)> f) : _icon(icon), _details(details)
		{
			_button = h->create_button(icon, title, details, std::move(f));
		}

		button_control(const control_frame_ptr& h, const std::string_view title, std::function<void(void)> f)
		{
			_button = h->create_button(_icon, title, {}, std::move(f));
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_button);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			const auto avail = cx;
			auto extent = _button->measure(avail);
			if (!str::is_empty(_details)) extent.cx = std::max(avail, extent.cx);
			return extent;
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			positions.emplace_back(_button, bounds, is_visible());
		}
	};

	class ok_cancel_control final : public view_element, public std::enable_shared_from_this<ok_cancel_control>
	{
		button_ptr _ok;
		button_ptr _cancel;

		mutable int _ok_width = 100;
		mutable int _cancel_width = 100;

		std::string _ok_text;
		std::string _cancel_text;

	public:
		ok_cancel_control(const control_frame_ptr& h, const std::string_view text = tt.button_ok,
		                  const std::string_view cancel_text = tt.button_cancel) : _ok_text(text),
			_cancel_text(cancel_text)
		{
			_ok = h->create_button(_ok_text, [h] { h->close(false); }, true);
			_cancel = h->create_button(_cancel_text, [h] { h->close(true); });
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_ok);
			handler(_cancel);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			const auto ok_extent = _ok->measure(cx);
			const auto cancel_extent = _cancel->measure(cx);

			_ok_width = std::max(cx / 5, ok_extent.cx + mc.padding2);
			_cancel_width = std::max(cx / 5, cancel_extent.cx + mc.padding2);

			return {cx, std::max(ok_extent.cy, cancel_extent.cy) + mc.padding2};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			const auto total_button_width = _ok_width + _cancel_width + mc.padding2;
			const auto button_rect = center_rect(sizei{total_button_width, bounds.height()}, bounds);

			auto rok = button_rect;
			auto rcan = button_rect;

			rok.right = rok.left + _ok_width;
			rcan.left = rcan.right - _cancel_width;

			positions.emplace_back(_ok, rok, is_visible());
			positions.emplace_back(_cancel, rcan, is_visible());
		}
	};

	class ok_cancel_control_analyze final : public view_element,
	                                        public std::enable_shared_from_this<ok_cancel_control_analyze>
	{
		button_ptr _ok;
		button_ptr _cancel;
		button_ptr _analyze;

		mutable int _ok_width = 100;
		mutable int _cancel_width = 100;
		mutable int _analyze_width = 100;

		std::string _ok_text;
		std::string _cancel_text;
		std::string _analyze_text;

	public:
		ok_cancel_control_analyze(const control_frame_ptr& h, const std::string_view text,
		                          std::function<void()> invoke) : _ok_text(text), _cancel_text(tt.button_cancel),
		                                                          _analyze_text(tt.button_analyze)
		{
			_analyze = h->create_button(_analyze_text, std::move(invoke), true);
			_ok = h->create_button(_ok_text, [h] { h->close(false); });
			_cancel = h->create_button(_cancel_text, [h] { h->close(true); });
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_ok);
			handler(_cancel);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			const auto ok_extent = _ok->measure(cx);
			const auto cancel_extent = _cancel->measure(cx);
			const auto analyze_extent = _analyze->measure(cx);

			_ok_width = std::max(cx / 5, ok_extent.cx + mc.padding2);
			_cancel_width = std::max(cx / 5, cancel_extent.cx + mc.padding2);
			_analyze_width = std::max(cx / 5, analyze_extent.cx + mc.padding2);

			return {
				cx,
				std::max(std::max(ok_extent.cy, analyze_extent.cy), cancel_extent.cy) + mc.padding2
			};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			const auto total_button_width = _ok_width + _cancel_width + _analyze_width + 2 * mc.padding2;
			const auto button_rect = center_rect(sizei{total_button_width, bounds.height()}, bounds);

			auto ranalyze = button_rect;
			auto rok = button_rect;
			auto rcan = button_rect;

			ranalyze.right = rok.left + _analyze_width;
			rcan.left = rcan.right - _cancel_width;
			rok.left = (rok.left + rok.right - _ok_width) / 2;
			rok.right = rok.left + _ok_width;

			positions.emplace_back(_analyze, ranalyze, is_visible());
			positions.emplace_back(_ok, rok, is_visible());
			positions.emplace_back(_cancel, rcan, is_visible());
		}
	};

	class close_control final : public view_element, public std::enable_shared_from_this<close_control>
	{
		button_ptr _button;

	public:
		close_control(const control_frame_ptr& h, const bool is_cancel = false,
		              const std::string_view text = tt.button_close)
		{
			_button = h->create_button(text, [h, is_cancel] { h->close(is_cancel); }, true);
		}

		close_control(const control_frame_ptr& h, std::function<void()> invoke,
		              const std::string_view text = tt.button_close)
		{
			_button = h->create_button(text, std::move(invoke), true);
		}

		void text(const std::string_view text) const
		{
			_button->window_text(text);
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_button);
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			const auto button_extent = _button->measure(cx);

			return {cx, button_extent.cy + mc.padding2};
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;
			const auto button_extent = _button->measure(bounds_in.width());
			const auto width = std::max(bounds_in.width() / 5, button_extent.cx + mc.padding2);

			positions.emplace_back(_button, center_rect(sizei{width, bounds_in.height()}, bounds), is_visible());
		}
	};

	class list_frame final : public std::enable_shared_from_this<list_frame>, public view_host
	{
	public:
		frame_ptr _frame;
		control_frame_ptr _owner;
		auto_complete_match_ptr _hover_item;
		auto_complete_results _results;
		complete_strategy_ptr _completes;
		view_scroller _scroller;
		int _height = 0;
		bool _is_popup = false;
		color_style _colors;

		void init(const control_frame_ptr& owner, const complete_strategy_ptr& completes, const frame_style& style)
		{
			df::assert_true(!_frame);

			_completes = completes;
			_frame = owner->create_frame(weak_from_this(), style);
			_owner = owner;
			_colors = style.colors;
			_is_popup = !style.child;
		}

		int height() const
		{
			return _height;
		}

		void search(const std::string& text)
		{
			if (_completes)
			{
				_completes->search(text, [t = shared_from_this()](const auto_complete_results& results)
				{
					t->show_results(results);
				});
			}
		}

		void show_results(const auto_complete_results& results)
		{
			df::assert_true(is_ui_thread());

			_results = results;
			_scroller._offset = {0, 0};

			if (_completes->auto_select_first && !_results.empty())
			{
				selected(_results.front(), complete_strategy_t::select_type::init);
			}
			else
			{
				selected(nullptr, complete_strategy_t::select_type::init);
			}

			frame()->invalidate();
			frame()->layout();
		}

		void step_selection(const int i)
		{
			if (!_results.empty())
			{
				bool found = false;

				if (i > 0)
				{
					for (const auto& r : _results)
					{
						if (found)
						{
							selected(r, complete_strategy_t::select_type::arrow);
							return;
						}

						found = _completes->selected() == r;
					}

					selected(_results.front(), complete_strategy_t::select_type::arrow);
				}
				else
				{
					for (auto r = _results.rbegin(); r != _results.rend(); ++r)
					{
						if (found)
						{
							selected(*r, complete_strategy_t::select_type::arrow);
							return;
						}

						found = _completes->selected() == *r;
					}

					selected(_results.back(), complete_strategy_t::select_type::arrow);
				}
			}
		}

		void selected(const auto_complete_match_ptr& i, const complete_strategy_t::select_type st)
		{
			if (_completes->selected() != i)
			{
				if (_completes->selected())
				{
					_completes->selected()->set_style_bit(view_element_style::selected, false);
				}

				frame()->invalidate();
				_completes->selected(i, st);

				if (i)
				{
					i->set_style_bit(view_element_style::selected, true);
					make_visible(i);
				}
			}
		}

		void make_visible(const auto_complete_match_ptr& i)
		{
			if (i)
			{
				const auto item_bounds = i->bounds;
				const auto offset = _scroller.scroll_offset();

				if (item_bounds.top < offset.y)
				{
					_scroller.scroll_offset(shared_from_this(), 0, item_bounds.top);
				}
				else if (item_bounds.bottom > offset.y + _extent.cy)
				{
					_scroller.scroll_offset(shared_from_this(), 0, item_bounds.bottom - _extent.cy);
				}
			}
		}

		void on_window_layout(measure_context& mc, const sizei extent, const bool is_minimized) override
		{
			if (!is_minimized)
			{
				_extent = extent;
				layout(mc);
			}
		}

		void layout(measure_context& mc)
		{
			control_layouts positions;
			const auto text_height = mc.text_line_height(style::font_face::dialog);
			auto y = mc.padding2;

			for (const auto& i : _results)
			{
				const recti bounds(0 + mc.padding2, y, _extent.cx - mc.padding2,
				                   y + text_height + mc.padding1);
				i->layout(mc, bounds, positions);
				y = bounds.bottom;
			}

			const auto y_max = y + mc.padding2;
			const recti scroll_bounds{_extent.cx - mc.scroll_width, 0, _extent.cx, _extent.cy};
			const recti client_bounds{0, 0, _extent.cx - mc.scroll_width, _extent.cy};
			_scroller.layout({client_bounds.width(), y_max}, client_bounds, scroll_bounds);

			if (_completes->resize_to_show_results)
			{
				if (_height != y_max)
				{
					const auto r = frame()->window_bounds();
					_height = y_max;
					frame()->window_bounds(recti(r.left, r.top, r.right, r.top + y_max), !_results.empty());
				}
			}
			else
			{
				_height = static_cast<int>(_completes->max_predictions) * (text_height + mc.padding1) + mc.padding2 *
					2;
			}

			frame()->invalidate();
		}

		void on_window_paint(draw_context& dc) override
		{
			const auto r = recti(_extent);
			dc.frame_has_focus = true;

			if (_is_popup)
			{
				const auto border_clr = color(emphasize(_colors.background, 0.123f), dc.colors.alpha);
				dc.draw_border(recti(_extent).inflate(-3), recti(_extent), border_clr, border_clr);
			}

			if (_results.empty())
			{
				const auto text = _completes->no_results_message();

				if (!str::is_empty(text))
				{
					dc.draw_text(text, r.inflate(-32), style::font_face::title, style::text_style::multiline_center,
					             color(dc.colors.foreground, dc.colors.alpha), {});
				}
			}
			else
			{
				const auto offset = _scroller.scroll_offset();

				for (const auto& i : _results)
				{
					i->render(dc, -offset);
				}
			}

			if (_active_controller)
			{
				_active_controller->draw(dc);
			}

			if (setting.show_debug_info && _active_controller)
			{
				const auto c = color(1.0f, 0.0f, 0.0f, dc.colors.alpha);
				const auto pad = df::round(2 * dc.scale_factor);
				dc.draw_border(_controller_bounds, _controller_bounds.inflate(pad), c, c);
			}
		}

		void on_mouse_wheel(const pointi loc, const int delta, const key_state keys, bool& was_handled) override
		{
			_scroller.offset(shared_from_this(), 0, -(delta / 2));
			was_handled = _scroller.can_scroll();
			update_controller(loc);
		}

		const frame_ptr frame() const override
		{
			return _frame ? _frame : no_frame();
		}

		const control_frame_ptr owner() override
		{
			return _owner;
		}

		view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
		{
			if (_scroller.can_scroll() && ctx.occluded(_scroller.scroll_bounds()))
			{
				return std::make_shared<scroll_controller>(shared_from_this(), _scroller, _scroller.scroll_bounds());
			}

			const auto offset = -_scroller.scroll_offset();

			for (const auto& r : _results)
			{
				auto controller = r->controller_from_location(shared_from_this(), loc, offset, ctx);
				if (controller) return controller;
			}

			return nullptr;
		}

		void controller_changed() override
		{
		}

		void invoke(const commands cmd) override
		{
		}

		bool is_command_checked(const commands cmd) override
		{
			return false;
		}

		void track_menu(const recti recti, const std::vector<command_ptr>& commands) override
		{
		}

		void invalidate_view(const view_invalid invalid) override
		{
		}

		void invalidate_element(const view_element_ptr& e) override
		{
			frame()->invalidate();
		}
	};

	using list_window_ptr = std::shared_ptr<list_frame>;

	class search_control final : public view_element, public std::enable_shared_from_this<search_control>
	{
		std::string& _val;
		complete_strategy_ptr _completes;

		std::atomic_int _pin;
		edit_ptr _edit;
		toolbar_ptr _tb;
		list_window_ptr _list;
		mutable int _edit_line_height;

	public:
		search_control(const control_frame_ptr& h, std::string& v,
		               const complete_strategy_ptr& s) : _val(v), _completes(s)
		{
			edit_styles e_style;
			e_style.horizontal_scroll = true;
			e_style.font = style::font_face::title;
			e_style.capture_key_down = [this](const int c, const key_state keys) { return key_down(c, keys); };
			_edit = h->create_edit(e_style, v, [this](const std::string_view text) { edit_change(text); });

			if (s->folder_select_button)
			{
				constexpr toolbar_styles styles;
				const auto c = std::make_shared<command>();
				c->icon = icon_index::folder;
				c->invoke = [this] { browse_for_folder(); };
				_tb = h->create_toolbar(styles, {c});
			}

			frame_style f_style;
			f_style.can_focus = false;
			_list = std::make_shared<list_frame>();
			_list->init(h, s, f_style);
		}

		~search_control() override
		{
			_completes.reset();
			_list.reset();
			_edit.reset();
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::initialise)
			{
				_completes->initialise([list = _list](const auto_complete_results& results)
				{
					list->show_results(results);
				});
			}
		}

		void browse_for_folder() const
		{
			df::folder_path path;

			if (df::is_path(_val))
			{
				path = df::folder_path(_val);
			}

			if (platform::browse_for_folder(path))
			{
				_edit->window_text(path.text());
				_edit->select_all();
				_edit->focus();
			}
		}

		void update_edit_text(const std::string_view text)
		{
			df::scope_locked_inc l(_pin);
			_edit->window_text(text);
		}

		void edit_change(const std::string_view text) const
		{
			_val = text;

			if (_pin == 0)
			{
				_list->search(_val);
			}
		}

		void visit_controls(const std::function<void(const control_base_ptr&)>& handler) override
		{
			handler(_edit);
			handler(_list->frame());
		}

		sizei measure(measure_context& mc, const int cx) const override
		{
			_edit_line_height = mc.text_line_height(style::font_face::title) + 16;
			const auto line_height = mc.text_line_height(style::font_face::dialog);
			return {cx, line_height * 16 + _edit_line_height + mc.padding2};
		}

		void render(draw_context& dc, const pointi element_offset) const override
		{
		}

		void layout(measure_context& mc, const recti bounds_in, control_layouts& positions) override
		{
			bounds = bounds_in;

			auto edit_bounds = bounds;
			edit_bounds.bottom = edit_bounds.top + _edit_line_height;

			if (_completes->folder_select_button)
			{
				const auto tb_extent = _tb->measure_toolbar(50);
				const auto split = edit_bounds.right - tb_extent.cx - mc.padding2;
				auto tb_bounds = edit_bounds;
				tb_bounds.left = split + mc.padding2 / 2;
				tb_bounds.top += (tb_bounds.height() - tb_extent.cy) / 2;
				edit_bounds.right = split;

				positions.emplace_back(_tb, tb_bounds, is_visible());
			}

			positions.emplace_back(_edit, edit_bounds, is_visible());

			auto search_bounds = bounds;
			search_bounds.top = search_bounds.top + _edit_line_height + mc.padding2;
			positions.emplace_back(_list->frame(), search_bounds, is_visible());
		}

		bool key_down(const int key_code, const key_state keys) const
		{
			const auto key = static_cast<char32_t>(key_code);

			if (key == keys::UP)
			{
				_list->step_selection(-1);
				return true;
			}

			if (key == keys::DOWN)
			{
				_list->step_selection(1);
				return true;
			}

			return false;
		}

		void focus() const
		{
			if (_edit)
			{
				_edit->focus();
			}
		}
	};
}

class dialog final : public view_host, public std::enable_shared_from_this<dialog>
{
public:
	view_scroller _scroller;
	std::vector<view_element_ptr> _controls;
	ui::control_frame_ptr _frame;
	ui::control_frame_ptr _parent;
	bool _parent_disabled = false;
	bool _center = false;
	bool _resize = false;
	std::any _parent_focus = false;
	int _layout_height = 0;
	int _layout_width = 0;
	ui::screen_units _max_height = {0};
	ui::screen_units _max_width = {0};
	ui::color _background = ui::color(ui::style::color::dialog_background);
	ui::color _clr = ui::color(ui::style::color::dialog_text);
	ui::coll_widths _label_width;
	std::function<void()> _close_cb;
	// Controls held outside the scrolling body so a title and actions such as Close stay in place.
	size_t _pinned_header = 0;
	size_t _pinned_footer = 0;

	dialog(const dialog& other) = delete;
	dialog& operator=(const dialog& other) = delete;

	dialog(ui::control_frame_ptr parent) : _parent(std::move(parent))
	{
		_parent_focus = ui::focus();
		disable_parent();
	}

	void create_frame()
	{
		_frame = _parent->create_dlg(shared_from_this(), true);

		// Creating the native dialog can fail - desktop heap exhaustion, or a request made while the
		// session is ending. There is no stand-in for a control_frame the way ui::no_frame() stands in
		// for a window, so one is used here: every call site opening a dialog would otherwise have to
		// test for null, and none of the dozens of them do.
		if (!_frame)
		{
			df::log(__FUNCTION__, "dialog window could not be created");
			_frame = ui::no_control_frame();
		}

		_scroller._scroll_child_controls = true;
		_scroller.changed_func = [this]
		{
			// Child controls are repositioned by the layout instead of being dragged by the scroll.
			if (is_split() && _frame) _frame->layout();
		};
	}

	// Controls split into [0, header_end) pinned above, [header_end, body_end) scrolling, then pinned below.
	size_t header_end() const
	{
		return std::min(_pinned_header, _controls.size());
	}

	size_t body_end() const
	{
		return std::max(header_end(), _controls.size() - std::min(_pinned_footer, _controls.size()));
	}

	bool is_split() const
	{
		return header_end() > 0 || body_end() < _controls.size();
	}

	~dialog() override
	{
		enable_parent();
		_controls.clear();
		if (_frame) _frame->destroy();
		ui::focus(_parent_focus);
	}

	void disable_parent()
	{
		if (_parent_disabled) return;
		_parent->enable(false);
		_parent_disabled = true;
	}

	void enable_parent()
	{
		if (!_parent_disabled) return;
		_parent->enable(true);
		_parent_disabled = false;
	}


	void layout() const
	{
		if (_frame)
		{
			_frame->layout();
		}
	}

	operator const ui::control_frame_ptr() const
	{
		return _frame;
	}

	void close(const bool is_cancel) const
	{
		if (_frame)
		{
			_frame->close(is_cancel);
		}
	}

	bool is_canceled() const
	{
		return _frame && _frame->is_canceled();
	}

	void wait_for_close() const
	{
		if (_frame)
		{
			_frame->wait_for_close(0);
		}
	}

	void show_status(icon_index icon, const std::string_view text, const ui::screen_units cx = {33},
	                 const ui::screen_units cy = {66})
	{
		disable_parent();
		const auto scale_factor = _frame->scale_factor();
		const auto extent = sizei{cx * scale_factor, cy * scale_factor};
		// Same anchor as show_modal, so a flow that alternates between busy and question phases
		// keeps its window in one place instead of hopping.
		_frame->position(center_rect(extent, ui::desktop_bounds(true)));

		const std::vector<view_element_ptr> controls = {std::make_shared<ui::busy_control>(_frame, icon, text)};
		show_controls(controls, cx, cy);
		layout();
		_frame->show(true);
	}

	void show_cancel_status(icon_index icon, const std::string_view text, const std::function<void()>& cb,
	                        const ui::screen_units cx = {33}, const ui::screen_units cy = {66})
	{
		disable_parent();
		_close_cb = [f = _frame, cb]
		{
			if (cb) { cb(); }
			f->close(true);
		};

		const auto scale_factor = _frame->scale_factor();
		const auto extent = sizei{cx * scale_factor, cy * scale_factor};
		_frame->position(center_rect(extent, ui::desktop_bounds(true)));

		const std::vector<view_element_ptr> controls = {
			std::make_shared<ui::busy_control>(_frame, icon, text),
			std::make_shared<ui::close_control>(_frame, _close_cb, tt.button_cancel)
		};
		show_controls(controls, cx, cy);
		layout();
		_frame->show(true);
	}

	void show_message(icon_index icon, const std::string_view title, const std::string_view text,
	                  const ui::screen_units cx = {33})
	{
		const std::vector<view_element_ptr> controls = {
			std::make_shared<ui::title_control2>(_frame, icon, title, text),
			std::make_shared<divider_element>(),
			std::make_shared<ui::close_control>(_frame),
		};
		show_modal(controls, cx);
	}

	ui::close_result show_modal(const std::vector<view_element_ptr>& controls, const ui::screen_units cx = {44},
	                            const ui::screen_units cy = {88})
	{
		disable_parent();
		const auto scale_factor = _frame->scale_factor();
		const auto extent = sizei{cx * scale_factor, cy * scale_factor};
		_frame->position(center_rect(extent, ui::desktop_bounds(true)));
		show_controls(controls, cx, cy);
		_frame->show(true);

		const auto result = _frame->wait_for_close(0);
		const auto destroyer = [](const ui::control_base_ptr& c) { c->destroy(); };

		for (const auto& c : _controls)
		{
			c->visit_controls(destroyer);
		}

		_controls.clear();

		// The active controller holds this dialog as its host, so the pair keeps itself alive.
		// on_window_destroy breaks that when the native window finally goes, which has not
		// necessarily happened by the time the caller lets go of the dialog - and a dialog that
		// outlives its modal loop takes a hidden popup window with it. The close callback holds
		// the frame and whatever the caller gave it, so it goes here too.
		_active_controller.reset();
		_controller_invalid = true;
		_close_cb = {};

		enable_parent();
		ui::focus(_parent_focus);
		return result;
	}

	void show_controls(const std::vector<view_element_ptr>& controls, const ui::screen_units max_width,
	                   const ui::screen_units max_height)
	{
		_max_width = max_width;
		_max_height = max_height;
		_center = true;

		initialise(controls);

		_frame->layout();
		_frame->focus_first();
	}

	void initialise(const std::vector<view_element_ptr>& controls)
	{
		// The controller answers for an element in the outgoing set, and it holds this dialog as its
		// host - so the two keep each other alive, and the controller outlives the control it speaks
		// for. Dropped here, where the set it belongs to is replaced.
		_active_controller.reset();
		_controller_invalid = true;

		const auto destroyer = [](const ui::control_base_ptr& c) { c->destroy(); };
		const std::unordered_set<view_element_ptr> controls_set(controls.begin(), controls.end());

		for (const auto& c : _controls)
		{
			// don't destroy controls that are in the new set
			if (!controls_set.contains(c))
			{
				c->visit_controls(destroyer);
			}
		}

		_controls = controls;
		// SW_SCROLLCHILDREN would drag the pinned controls too, so a split dialog moves its own.
		_scroller._scroll_child_controls = !is_split();

		for (const auto& c : _controls)
		{
			view_element_event e{view_element_event_type::initialise, shared_from_this()};
			c->dispatch_event(e);
		}
	}

	void invalidate_view(const view_invalid invalid) override
	{
	}

	void invalidate_element(const view_element_ptr& e) override
	{
		if (_frame)
		{
			_frame->invalidate();
		}
	}

	void scroll_controls() override
	{
		layout();
	}

	void layout(ui::measure_context& mc)
	{
		const auto scale_factor = _frame->scale_factor();

		const auto work_area = ui::desktop_bounds(true);
		const auto max_width = std::min(work_area.width(), _max_width * scale_factor);
		const auto max_height = std::min(work_area.height(), _max_height * scale_factor);

		const auto layout_padding = df::round(mc.padding1 / mc.scale_factor);
		auto avail_bounds = recti(0, 0, max_width, max_height);
		avail_bounds.right -= mc.scroll_width;

		mc.col_widths = {};
		ui::control_layouts positions;
		flex_container_layout column;
		column.direction = flex_direction::column;
		column.wrap = flex_wrap::no_wrap;
		column.align_items = flex_align::start;
		column.padding = {layout_padding, layout_padding};

		const auto split_header = header_end();
		const auto split_body = body_end();
		const std::vector<view_element_ptr> header(_controls.begin(), _controls.begin() + split_header);
		const std::vector<view_element_ptr> body(_controls.begin() + split_header, _controls.begin() + split_body);
		const std::vector<view_element_ptr> footer(_controls.begin() + split_body, _controls.end());

		auto header_height = 0;

		if (!header.empty())
		{
			header_height = layout_flex_elements(header, mc, positions, avail_bounds, column).cy;
			for (auto& p : positions) p.offset = false;
		}

		// The footer height has to be known before the body can be given the space that is left.
		auto footer_height = 0;

		if (!footer.empty())
		{
			ui::control_layouts measure_only;
			footer_height = layout_flex_elements(footer, mc, measure_only, avail_bounds, column).cy;
		}

		auto body_bounds = avail_bounds;
		body_bounds.top = header_height;
		body_bounds.bottom = std::max(header_height, max_height - footer_height);

		ui::control_layouts body_positions;
		const auto height = layout_flex_elements(body, mc, body_positions, body_bounds, column).cy;
		const auto show_scroller = height > body_bounds.height();
		const auto padding_right = show_scroller ? mc.scroll_width : 0;

		const auto client_height = std::min(height, body_bounds.height());
		positions.insert(positions.end(), body_positions.begin(), body_positions.end());

		_layout_height = header_height + client_height + footer_height;
		_layout_width = std::min(max_width, avail_bounds.width() + padding_right);

		if (!footer.empty())
		{
			ui::control_layouts footer_positions;
			layout_flex_elements(footer, mc, footer_positions,
			                     {0, header_height + client_height, avail_bounds.right, _layout_height}, column);

			for (auto& p : footer_positions) p.offset = false;
			positions.insert(positions.end(), footer_positions.begin(), footer_positions.end());
		}

		const auto client_bottom = header_height + client_height;
		const auto scroll_inset = mc.padding1;
		// Inset so the bar sits in the gutter rather than against the dialog border.
		const recti scroll_bounds{
			_layout_width - padding_right + scroll_inset, header_height + scroll_inset,
			_layout_width - scroll_inset, client_bottom - scroll_inset
		};
		const recti client_bounds{0, header_height, _layout_width - padding_right, client_bottom};
		_scroller.layout({client_bounds.width(), height}, client_bounds, scroll_bounds);
		_label_width = mc.col_widths;

		_frame->apply_layout(positions, -_scroller.scroll_offset());
		_frame->invalidate();

		if (_center || _resize)
		{
			const auto existing_bounds = _frame->window_bounds();
			const auto work_area = ui::desktop_bounds(true);
			const auto bounds = _center
				                    ? center_rect(sizei{_layout_width, _layout_height}, work_area)
				                    : recti(existing_bounds.left, existing_bounds.top,
				                            existing_bounds.left + _layout_width, existing_bounds.top + _layout_height);
			_frame->position(bounds);
			_center = false;
			_resize = false;
		}
	}

	void scroll_to(const view_element_ptr& focus)
	{
		for (const auto& c : _controls)
		{
			if (c->is_visible() && c == focus)
			{
				_scroller.scroll_offset(shared_from_this(), 0, c->bounds.top - 32);
			}
		}
	}

	void on_window_layout(ui::measure_context& mc, const sizei extent, const bool is_minimized) override
	{
		_extent = extent;
		layout(mc);
	}

	void on_window_paint(ui::draw_context& dc) override
	{
		dc.col_widths = _label_width;

		const auto offset = -_scroller.scroll_offset();
		const auto split_header = header_end();
		const auto split_body = body_end();
		const auto clip_body = is_split();

		for (auto i = 0u; i < split_header; ++i)
		{
			const auto& c = _controls[i];
			if (c->is_visible()) c->render(dc, {0, 0});
		}

		if (clip_body) dc.clip_bounds(_scroller.client_bounds());

		for (auto i = split_header; i < split_body; ++i)
		{
			const auto& c = _controls[i];
			if (c->is_visible()) c->render(dc, offset);
		}

		if (clip_body) dc.restore_clip();

		for (auto i = split_body; i < _controls.size(); ++i)
		{
			const auto& c = _controls[i];
			if (c->is_visible()) c->render(dc, {0, 0});
		}

		_scroller.draw_scroll(dc);

		const auto border_outside = recti(0, 0, _layout_width, _layout_height);
		const auto clr = ui::color(0xffffff, 0.22f);
		dc.draw_border(border_outside.inflate(-2), border_outside, clr, clr);

		if (_active_controller)
		{
			_active_controller->draw(dc);
		}
	}

	void tick() override
	{
		for (const auto& c : _controls)
		{
			view_element_event e{view_element_event_type::tick, shared_from_this()};
			c->dispatch_event(e);
		}
	}

	void populate()
	{
		for (const auto& c : _controls)
		{
			view_element_event e{view_element_event_type::populate, shared_from_this()};
			c->dispatch_event(e);
		}
	}

	bool key_down(const int c, const ui::key_state keys) override
	{
		return false;
	}

	void on_mouse_wheel(const pointi loc, const int delta, const ui::key_state keys, bool& was_handled) override
	{
		_scroller.offset(shared_from_this(), 0, -(delta / 2));
		was_handled = _scroller.can_scroll();
	}

	void focus_changed(const bool has_focus, const ui::control_base_ptr& child) override
	{
	}

	const ui::frame_ptr frame() const override
	{
		return _frame ? _frame : ui::no_frame();
	}

	const ui::control_frame_ptr owner() override
	{
		return _frame;
	}

	void invoke(const commands cmd) override
	{
	}

	bool is_command_checked(const commands cmd) override
	{
		return false;
	}

	void controller_changed() override
	{
	}

	view_controller_ptr controller_from_location(const pointi loc, hit_test_context& ctx) override
	{
		if (_scroller.can_scroll() && ctx.occluded(_scroller.scroll_bounds()))
		{
			return std::make_shared<scroll_controller>(shared_from_this(), _scroller, _scroller.scroll_bounds());
		}

		const auto offset = -_scroller.scroll_offset();
		const auto split_header = header_end();
		const auto split_body = body_end();
		const auto clip_body = is_split();

		for (auto i = 0u; i < _controls.size(); ++i)
		{
			const auto& e = _controls[i];
			if (!e->is_visible()) continue;
			const auto in_body = i >= split_header && i < split_body;
			// Body elements scrolled under the header or footer must not answer for them.
			if (in_body && clip_body && !_scroller.client_bounds().contains(loc)) continue;
			auto controller = e->controller_from_location(shared_from_this(), loc, in_body ? offset : pointi{}, ctx);
			if (controller) return controller;
		}

		return nullptr;
	}

	bool is_control_area(const pointi loc) override
	{
		// Without this the frame treats the scrollbar as caption and drags the whole dialog.
		if (_scroller.can_scroll() && _scroller.scroll_bounds().contains(loc))
		{
			return true;
		}

		const auto offset = -_scroller.scroll_offset();
		const auto split_header = header_end();
		const auto split_body = body_end();
		const auto clip_body = is_split();

		for (auto i = 0u; i < _controls.size(); ++i)
		{
			const auto& e = _controls[i];
			if (!e->is_visible()) continue;
			const auto in_body = i >= split_header && i < split_body;
			if (in_body && clip_body && !_scroller.client_bounds().contains(loc)) continue;
			if (e->is_control_area(loc, in_body ? offset : pointi{}))
			{
				return true;
			}
		}

		return false;
	}

	void track_menu(const recti bounds, const std::vector<ui::command_ptr>& commands) override
	{
	}

	void dpi_changed() override
	{
		for (const auto& c : _controls)
		{
			view_element_event e{view_element_event_type::dpi_changed, shared_from_this()};
			c->dispatch_event(e);
		}

		_frame->invalidate();
		_resize = true;
	}
};

using dialog_ptr = std::shared_ptr<dialog>;

inline dialog_ptr make_dlg(const ui::control_frame_ptr& parent)
{
	auto result = std::make_shared<dialog>(parent);
	result->create_frame();
	return result;
}

class wallpaper_control final : public view_element, public std::enable_shared_from_this<wallpaper_control>
{
	bool& _maximize;
	ui::control_frame_ptr _parent;
	file_load_result _loaded;
	mutable ui::texture_ptr _tex;

public:
	wallpaper_control(ui::control_frame_ptr h, file_load_result loaded, bool& maximize) : _maximize(maximize),
		_parent(std::move(h)), _loaded(std::move(loaded))
	{
	}

	void refresh() const
	{
		_parent->invalidate({}, false);
	}

	sizei measure(ui::measure_context& mc, const int cx) const override
	{
		const auto bounds = ui::desktop_bounds(true);
		const auto cy = df::mul_div(cx, bounds.height(), bounds.width());
		return {cx, cy};
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto max_dim = std::max(bounds.width(), bounds.height());
		const auto max_dims = _maximize ? sizei(max_dim, max_dim) : bounds.extent();
		const auto dimensions = ui::scale_dimensions(_loaded.dimensions(), max_dims);
		const auto bg = ui::style::color::desktop_background;

		if (!_tex)
		{
			const auto t = dc.create_texture();

			if (t && t->update(_loaded.to_surface(dimensions)) != ui::texture_update_result::failed)
			{
				_tex = t;
			}
		}

		dc.draw_rect(bounds, ui::color(bg, dc.colors.alpha));

		if (_tex)
		{
			const auto dst = recti(bounds.center() - sizei(dimensions.cx / 2, dimensions.cy / 2), dimensions).
				intersection(bounds);
			const auto texture_dimensions = _tex->dimensions();

			const double sx = dst.width() / static_cast<double>(texture_dimensions.cx);
			const double sy = dst.height() / static_cast<double>(texture_dimensions.cy);

			const auto texture_scale = std::max(sx, sy);
			const auto cx_tex = dst.width() / texture_scale;
			const auto cy_tex = dst.height() / texture_scale;
			const auto x_tex = (texture_dimensions.cx - cx_tex) / 2;
			const auto y_tex = (texture_dimensions.cy - cy_tex) / 3; // Bias to show top of image

			const rectd rect_tex(x_tex, y_tex, cx_tex, cy_tex);

			dc.draw_texture(_tex, dst, rect_tex.round());
		}
	}
};

inline view_element_ptr set_padding(view_element_ptr e, const int cx = 8, const int cy = 4)
{
	e->padding.cx = cx;
	e->padding.cy = cy;
	return e;
}

inline view_element_ptr set_margin(view_element_ptr e, const int cx = 8, const int cy = 4)
{
	e->margin.cx = cx;
	e->margin.cy = cy;
	return e;
}
