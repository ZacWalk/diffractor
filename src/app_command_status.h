// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Progress dialog and status tracking for long-running operations. Displays progress,
// handles cancellation, and shows success/failure results for batch processing commands.

#pragma once
#include "ui_elements.h"
#include "ui_dialog.h"

struct folder_scan_item;

class command_status final : public std::enable_shared_from_this<command_status>, public df::status_i
{
	async_strategy& _async;
	std::atomic_int _cancel_version = 0;
	int _cancel_initial_value = 0;

	// UI-owned, though the status itself is not: the worker that runs the operation can hold its last
	// reference - a task's captures live until its whole batch has run - and a dialog destroyed there
	// destroys windows and moves focus from the wrong thread. The final release is handed back to the
	// UI thread instead.
	ui_owned_ptr<dialog> _dlg;
	icon_index _icon;
	std::string _title;

	ui_owned_ptr<ui::progress_control> _progress;
	ui_owned_ptr<ui::close_control> _cancel;

	bool _closed = false;
	std::atomic<bool> _completed = false;
	core::mutex _publication_mutex;

	enum class completion_type
	{
		none,
		show_errors,
		abort,
		complete
	};

	struct pending_publication
	{
		bool has_progress = false;
		std::string progress_message;
		int64_t progress_pos = 0;
		int64_t progress_total = 0;
		completion_type completion = completion_type::none;
		std::string completion_message;
	};

	_Guarded_by_(_publication_mutex) pending_publication _pending_publication;
	_Guarded_by_(_publication_mutex) bool _ui_callback_pending = false;
	int64_t _pos = 0;
	int64_t _total = 0;

	int64_t _processed_count = 0;
	int64_t _failed_count = 0;
	int64_t _ignore_count = 0;
	int64_t _canceled_count = 0;

	std::string _processed_first_name;
	std::string _failed_first_name;
	std::string _ignore_first_name;
	std::string _canceled_first_name;

	std::string _error_message;
	std::string _message;

public:
	command_status(async_strategy& as, const dialog_ptr& dlg, const icon_index& icon, const std::string_view title,
	               const size_t total, const std::string_view preparing = {}) :
		_async(as), _dlg(as, dlg), _icon(icon), _title(title),
		_progress(as, std::make_shared<ui::progress_control>(dlg->_frame, preparing.empty() ? title : preparing)),
		_cancel(as, std::make_shared<ui::close_control>(dlg->_frame,
	                                                    [this, h = dlg->_frame]
	{
		++_cancel_version;
		h->close(true);
	}, tt.button_cancel)),
		_total(static_cast<int>(total))
	{
		const std::vector<view_element_ptr> controls{set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon,
		                                                                                             title,
		                                                                                             std::string{})),
		                                             std::make_shared<divider_element>(), _progress.shared(),
		                                             _cancel.shared()};

		_dlg->show_controls(controls, {44}, {44});
	}

	~command_status() override = default;

	void total(const size_t t)
	{
		core::exclusive_lock lock(_publication_mutex);
		_total = t;
	}

	void show_message(const std::string_view message) override
	{
		if (!_closed)
		{
			const std::vector<view_element_ptr> controls = {
				set_margin(std::make_shared<ui::title_control2>(_dlg->_frame, _icon, _title, message)),
				std::make_shared<divider_element>(),
				std::make_shared<ui::close_control>(_dlg->_frame),
			};

			_dlg->show_modal(controls);
		}
	}

	void message(const std::string_view message)
	{
		publish([this, message](pending_publication& pending)
		{
			pending.has_progress = true;
			pending.progress_message = message;
			pending.progress_pos = _pos;
			pending.progress_total = _total;
		});
	}

	void message(const std::string_view message, int64_t pos, int64_t total) override
	{
		publish([message, pos, total](pending_publication& pending)
		{
			pending.has_progress = true;
			pending.progress_message = message;
			pending.progress_pos = pos;
			pending.progress_total = total;
		});
	}

	bool is_canceled() const override { return _dlg->is_canceled() || _cancel_version.load() != _cancel_initial_value; }

	bool has_failures() const override
	{
		core::shared_lock lock(_publication_mutex);
		return _failed_count > 0;
	}

	void start_item(const std::string_view name) override
	{
		publish([this, name](pending_publication& pending)
		{
			pending.has_progress = true;
			pending.progress_message = name;
			pending.progress_pos = ++_pos;
			pending.progress_total = _total;
		});
	}

	void end_item(const std::string_view name, const item_status status) override
	{
		core::exclusive_lock lock(_publication_mutex);
		if (status == item_status::success)
		{
			if (_processed_first_name.empty()) _processed_first_name = name;
			++_processed_count;
		}
		else if (status == item_status::fail)
		{
			if (_failed_first_name.empty()) _failed_first_name = name;
			++_failed_count;
		}
		else if (status == item_status::ignore)
		{
			if (_ignore_first_name.empty()) _ignore_first_name = name;
			++_ignore_count;
		}
		else if (status == item_status::cancel)
		{
			if (_canceled_first_name.empty()) _canceled_first_name = name;
			++_canceled_count;
		}
	}

	void show_errors() override
	{
		publish([](pending_publication& pending) { pending.completion = completion_type::show_errors; });
	}

	void wait_for_complete() const override
	{
		// Quitting ends every modal loop at once, and the completion comes back through the loop waited in here, so
		// a quit while the work runs would otherwise leave this waiting on it for ever.
		while (!_completed && !platform::is_closing)
		{
			_dlg->wait_for_close();
		}

		_dlg->unlist_as_open();
	}

	void abort(const std::string_view error_message) override
	{
		publish([error_message](pending_publication& pending)
		{
			pending.completion = completion_type::abort;
			pending.completion_message = error_message;
		});
	}

	void complete(const std::string_view message = {}) override
	{
		publish([message](pending_publication& pending)
		{
			pending.completion = completion_type::complete;
			pending.completion_message = message;
		});
	}

private:
	template <typename T> void publish(T update)
	{
		auto queue_callback = false;
		{
			core::exclusive_lock lock(_publication_mutex);
			update(_pending_publication);
			if (!_ui_callback_pending)
			{
				_ui_callback_pending = true;
				queue_callback = true;
			}
		}

		if (queue_callback)
		{
			_async.queue_ui([t = shared_from_this()] { t->drain_publication(); });
		}
	}

	void drain_publication()
	{
		core::assert_true(platform::is_ui_thread());

		pending_publication pending;
		{
			core::exclusive_lock lock(_publication_mutex);
			pending = std::move(_pending_publication);
			_pending_publication = {};
			_ui_callback_pending = false;
		}

		if (!_closed && pending.has_progress)
		{
			_progress->message(pending.progress_message, pending.progress_pos, pending.progress_total);
			_async.work_progress(this, pending.progress_pos, pending.progress_total);
		}

		if (pending.completion != completion_type::none)
		{
			_completed = true;
			_async.work_finished(this);
			if (pending.completion == completion_type::abort) _error_message = std::move(pending.completion_message);
			if (pending.completion == completion_type::complete) _message = std::move(pending.completion_message);
			show_results_or_close();
		}
	}

	std::string format_processed_message() const
	{
		int64_t total;
		int64_t processed_count;
		int64_t failed_count;
		int64_t ignore_count;
		int64_t canceled_count;
		std::string processed_first_name;
		std::string failed_first_name;
		std::string ignore_first_name;
		std::string canceled_first_name;
		{
			core::shared_lock lock(_publication_mutex);
			total = _total;
			processed_count = _processed_count;
			failed_count = _failed_count;
			ignore_count = _ignore_count;
			canceled_count = _canceled_count;
			processed_first_name = _processed_first_name;
			failed_first_name = _failed_first_name;
			ignore_first_name = _ignore_first_name;
			canceled_first_name = _canceled_first_name;
		}

		std::string result;
		const auto processed_status = total ? format_plural_text(tt.processed_x_of_x_fmt, processed_first_name,
		                                                         processed_count, core::file_size{}, total)
		                                    : format_plural_text(tt.processed_fmt, processed_first_name,
		                                                         processed_count, {}, total);

		if (_dlg->is_canceled())
		{
			result = core::replace_tokens(tt.cancel_was_pressed_after,
			                              [&](std::ostringstream& result, const std::string_view token)
			{
				if (token.empty()) result << processed_status;
			});
		}
		else
		{
			result = processed_status;
		}

		if (failed_count > 0)
		{
			result += " ";
			result += format_plural_text(tt.failed_items_fmt, failed_first_name, failed_count, {}, total);
		}

		if (ignore_count > 0)
		{
			result += " ";
			result += format_plural_text(tt.ignored_fmt, ignore_first_name, ignore_count, {}, total);
		}

		if (canceled_count > 0)
		{
			result += " ";
			result += format_plural_text(tt.canceled_items_fmt, canceled_first_name, canceled_count, {}, total);
		}

		return result;
	}

	void show_results_or_close()
	{
		core::assert_true(platform::is_ui_thread());
		bool has_item_failures;
		{
			core::shared_lock lock(_publication_mutex);
			has_item_failures = _failed_count > 0 || _canceled_count > 0;
		}

		if (!_closed)
		{
			_cancel->text(tt.button_close);

			// Progress is waited out, but results waiting on Close are something to answer, so from here the dialog
			// is among open_dialogs() for the agent tools (app_mcp_ui.cpp) until wait_for_complete is done with it.
			if (_dlg->is_canceled() || has_item_failures || !_error_message.empty())
			{
				std::vector<view_element_ptr> controls;

				controls.emplace_back(set_margin(std::make_shared<ui::title_control>(_icon, _title)));
				controls.emplace_back(set_margin(std::make_shared<text_element>(format_processed_message())));

				if (!_message.empty())
				{
					controls.emplace_back(set_margin(std::make_shared<text_element>(_message)));
				}

				if (!_error_message.empty())
				{
					controls.emplace_back(set_margin(std::make_shared<text_element>(_error_message)));
				}

				controls.emplace_back(_cancel.shared());

				_dlg->show_controls(controls, {44}, {44});
				_dlg->list_as_open();
			}
			else if (!_message.empty())
			{
				const std::vector<view_element_ptr> controls = {
					set_margin(std::make_shared<ui::title_control2>(_dlg->_frame, _icon, _title, _message)),
					std::make_shared<divider_element>(),
					_cancel.shared(),
				};

				_dlg->show_controls(controls, {44}, {44});
				_dlg->list_as_open();
			}
			else
			{
				_dlg->close(false);
				_closed = true;
			}
		}
	}
};

using item_results_ptr = std::shared_ptr<command_status>;

inline item_status to_status(const platform::file_op_result_code code)
{
	switch (code)
	{
	case platform::file_op_result_code::OK: return item_status::success;
	case platform::file_op_result_code::CANCELLED: return item_status::cancel;
	case platform::file_op_result_code::FAILED: return item_status::fail;
	case platform::file_op_result_code::ALREADY_EXISTS: return item_status::fail;
	default:;
	}

	return item_status::success;
}
