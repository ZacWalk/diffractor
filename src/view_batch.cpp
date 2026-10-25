// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shared preview and processing view for batch conversion, metadata, and date tools.

#include "pch.h"
#include "model.h"
#include "model_index.h"
#include "view_batch.h"
#include "ui_controls.h"

static std::string convert_extension()
{
	if (setting.convert.to_png) return ".png";
	if (setting.convert.to_webp) return ".webp";
	return ".jpg";
}

struct agreed_date_row
{
	std::string_view label;
	std::string value;
	std::string_view source;
};

// A read-only report of the date an item holds now, and the tag it came from. Stated only where
// every selected item agrees on both: a set carrying two answers has no single one to state, and
// inventing one would be worse than saying nothing.
static std::optional<agreed_date_row> agreed_date(const df::item_set& items, const std::string_view label,
                                                  const prop::date_concept kind)
{
	if (items.is_empty()) return {};

	df::date_t value;
	auto source = prop::date_source::none;
	auto first = true;

	for (const auto& item : items.items())
	{
		const auto md = item->metadata();
		const auto item_value = md ? md->dates.resolve(kind) : df::date_t{};
		const auto item_source = md ? md->dates.resolved_source(kind) : prop::date_source::none;

		if (first)
		{
			value = item_value;
			source = item_source;
			first = false;
		}
		else if (value != item_value || source != item_source)
		{
			return {};
		}
	}

	if (!value.is_valid()) return {};
	return agreed_date_row{label, platform::format_date_time(value), prop::date_source_name(source)};
}

// Offer the built-in genres that suit the selected media, merged with the genres already present
// in the collection so an established vocabulary is never hidden behind the built-in list.
static std::vector<std::string> genre_suggestions(const view_state& s, const df::item_set& items)
{
	auto kinds = genre_kind::none;

	for (const auto& i : items.items())
	{
		const auto ft = i->file_type();
		if (ft->has_trait(file_traits::music_metadata)) kinds |= genre_kind::audio;
		if (ft->has_trait(file_traits::video_metadata)) kinds |= genre_kind::video;
		if (ft->has_trait(file_traits::photo_metadata)) kinds |= genre_kind::photo;
	}

	if (kinds == genre_kind::none) kinds = genre_kind::any;

	auto result = tt.known_genres(kinds);

	for (auto&& genre : tt.add_translate_text(s.item_index.distinct_genres(), "genre"))
	{
		result.emplace_back(std::move(genre));
	}

	std::ranges::sort(result, str::iless());
	result.erase(std::ranges::unique(result, df::ieq()).begin(), result.end());
	return result;
}

std::array<text_t, list_view::max_col_count> batch_tool_view::col_titles()
{
	// The last column answers a different question once the run is over: not what will change, but
	// what happened. The columns that identify the row are the same either way.
	if (_mode == batch_tool_mode::convert)
		return {tt.source, tt.destination, showing_results() ? tt.status : tt.changes, {}};
	if (_mode == batch_tool_mode::metadata) return {tt.file, tt.changes, tt.status, {}};
	// Adjust dates changes Original, and the source column makes mixed provenance visible before the
	// shift: a set where one file resolves from DateTimeOriginal and another from the file stamp is
	// not a set the same shift means the same thing to.
	return {
		tt.file, tt_prep(tt.prop_name_original.sv()), showing_results() ? tt.status : tt.after,
		tt_prep(tt.source.sv())
	};
}

std::string_view batch_tool_view::title()
{
	const auto text = _mode == batch_tool_mode::convert
		                  ? tt.command_convert_or_resize.sv()
		                  : _mode == batch_tool_mode::metadata
		                  ? tt.command_edit_metadata.sv()
		                  : tt.command_adjust_date.sv();
	_title = std::format("{}: {}", s_app_name, text);
	return _title;
}

// Toolbar text, so it names the operation without the dialog-button accelerator marker.
text_t batch_tool_view::run_text() const
{
	if (_mode == batch_tool_mode::convert) return tt.command_convert;
	if (_mode == batch_tool_mode::metadata) return tt.command_update_metadata;
	return tt.command_adjust_date;
}

std::string_view batch_tool_view::operation_name() const
{
	// View into the persistent text table, not into a text_t copy.
	if (_mode == batch_tool_mode::convert) return tt.command_convert.sv();
	if (_mode == batch_tool_mode::metadata) return tt.command_update_metadata.sv();
	return tt.command_adjust_date.sv();
}

void batch_tool_view::activate(const sizei extent)
{
	list_view::activate(extent);
	refresh();
}

void batch_tool_view::deactivate()
{
	abandon_processing();
	_rows.clear();
	_convert_plan.clear();
	_plan_valid = false;
	++_plan_generation;
	_row_names.clear();
	_status.clear();
	_showing_results = false;
	// The next use starts from its own selection, so a date chosen for the previous selection must
	// not survive as the starting date for a different set of items.
	_original_start = {};
	_new_start = {};
	_new_start_seeded = false;
}

metadata_edits batch_tool_view::metadata_changes() const
{
	metadata_edits edits;
	if (setting.set_title) edits.title = _metadata_title;
	if (setting.set_comment) edits.comment = _metadata_comment;
	if (setting.set_synopsis) edits.synopsis = _metadata_synopsis;
	if (setting.set_rating) edits.rating = _metadata_rating;
	if (setting.set_year) edits.year = _metadata_year;
	if (setting.set_original) edits.date_original = _metadata_date_original;
	if (setting.set_created) edits.date_created = _metadata_date_created;
	if (setting.set_episode) edits.episode = _metadata_episode;
	if (setting.set_season) edits.season = _metadata_season;
	if (setting.set_track) edits.track_num = _metadata_track;
	if (setting.set_disk) edits.disk_num = _metadata_disk;
	if (setting.set_artist) edits.artist = setting.artist;
	if (setting.set_caption) edits.description = setting.caption;
	if (setting.set_album) edits.album = setting.album;
	if (setting.set_album_artist) edits.album_artist = setting.album_artist;
	if (setting.set_genre) edits.genre = setting.genre;
	if (setting.set_tv_show) edits.show = setting.tv_show;
	if (setting.set_copyright_notice) edits.copyright_notice = setting.copyright_notice;
	if (setting.set_copyright_creator) edits.copyright_creator = setting.copyright_creator;
	if (setting.set_copyright_source) edits.copyright_source = setting.copyright_source;
	if (setting.set_copyright_credit) edits.copyright_credit = setting.copyright_credit;
	if (setting.set_copyright_url) edits.copyright_url = setting.copyright_url;
	return edits;
}

void batch_tool_view::refresh_convert()
{
	// Deciding a destination means asking the filesystem whether it is already taken, once per
	// selected item. That is a worker's job: on a network or sleeping destination it is the
	// difference between a responsive panel and a frozen one.
	const auto sources = snapshot_convert_sources(_state.selected_items());
	const auto write_folder = df::folder_path(setting.write_folder);
	const auto extension = convert_extension();
	const auto policy = setting.convert.collision;
	const auto generation = ++_plan_generation;
	_plan_valid = false;
	_convert_plan.clear();
	_status = std::string(tt.analyzing.sv());

	const std::weak_ptr<batch_tool_view> weak_view = shared_from_this();
	_state.queue_async(async_queue::work,
	                   [&s = _state, weak_view, sources, write_folder, extension, policy, generation]
	                   {
		                   auto plan = plan_convert_outputs(write_folder, sources, extension, policy);

		                   s.queue_ui([weak_view, plan = std::move(plan), generation]() mutable
		                   {
			                   const auto view = weak_view.lock();
			                   if (!view || generation != view->_plan_generation) return;
			                   view->_convert_plan = std::move(plan);
			                   view->_plan_valid = true;
			                   view->describe_convert_plan();
		                   });
	                   });
}

void batch_tool_view::describe_convert_plan()
{
	const auto& items = _state.selected_items();
	std::vector<row_element_ptr> rows;
	rows.reserve(_convert_plan.size());
	_row_names.clear();
	_row_names.reserve(_convert_plan.size());
	const auto error_color = ui::lighten(ui::style::color::warning_background, 0.55f);
	const auto resolved_color = ui::lighten(ui::style::color::important_background, 0.55f);
	int order = 0;
	int collisions = 0;
	for (const auto& entry : _convert_plan)
	{
		auto row = std::make_shared<row_element>(*this);
		row->_text[0] = entry.source.path.pack();
		row->_text[1] = entry.destination.pack();
		const auto dimensions = entry.source.dimensions;
		const auto max_side = setting.convert.limit_dimension ? setting.convert.max_side : 0;
		const auto scale = max_side > 0 && std::max(dimensions.cx, dimensions.cy) > max_side
			                   ? ui::scale_dimensions(dimensions, max_side, true)
			                   : dimensions;

		if (entry.collides) ++collisions;

		if (entry.skipped)
		{
			row->_text[2] = std::string(tt.collision_skip.sv());
			row->_text_color[2] = resolved_color;
		}
		else if (entry.renamed_to_avoid_collision)
		{
			row->_text[2] = std::string(tt.collision_rename.sv());
			row->_text_color[2] = resolved_color;
		}
		else if (entry.collides)
		{
			// The row states the outcome, never a question: Replace names the overwrite, and an
			// unresolved collision names only the fact so the policy control remains the one place
			// the user chooses what to do about it.
			const auto replacing = setting.convert.collision == collision_policy::replace;
			row->_text[2] = std::string(replacing ? tt.collision_replace.sv() : tt.collision_exists.sv());
			row->_text_color[2] = replacing ? resolved_color : error_color;
		}
		else
		{
			row->_text[2] = std::format("{}x{} {}", scale.cx, scale.cy, convert_extension());
		}

		row->_order = order++;
		row->_work_index = row->_order;
		rows.emplace_back(row);
		_row_names.emplace_back(entry.destination.name());
	}
	_rows = std::move(rows);
	_status = collisions > 0
		          ? std::format("{}   {}", format_plural_text(tt.convert_info_fmt, items),
		                        format_collision_summary(setting.convert.collision, collisions))
		          : format_plural_text(tt.convert_info_fmt, items);

	append_blocked_reason();
	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
		view_invalid::command_state);
}

void batch_tool_view::refresh_metadata()
{
	const auto& items = _state.selected_items();

	// The date picker cannot show an empty date, so an untouched control displays today while an
	// unset value would be written. Seed the value that the control already shows so the preview,
	// the control and the written date agree.
	if (!_metadata_date_original.is_valid()) _metadata_date_original = platform::now();
	if (!_metadata_date_created.is_valid()) _metadata_date_created = platform::now();

	std::vector<std::string_view> fields;
	if (setting.set_title) fields.emplace_back(tt.prop_name_title.sv());
	if (setting.set_comment) fields.emplace_back(tt.prop_name_comment.sv());
	if (setting.set_synopsis) fields.emplace_back(tt.prop_name_synopsis.sv());
	if (setting.set_rating) fields.emplace_back(tt.prop_name_rating.sv());
	if (setting.set_year) fields.emplace_back(tt.prop_name_year.sv());
	if (setting.set_original) fields.emplace_back(tt.prop_name_original.sv());
	if (setting.set_created) fields.emplace_back(tt.prop_name_created.sv());
	if (setting.set_episode) fields.emplace_back(tt.prop_name_episode.sv());
	if (setting.set_season) fields.emplace_back(tt.prop_name_season.sv());
	if (setting.set_track) fields.emplace_back(tt.prop_name_track.sv());
	if (setting.set_disk) fields.emplace_back(tt.prop_name_disk.sv());
	if (setting.set_artist) fields.emplace_back(tt.prop_name_artist.sv());
	if (setting.set_caption) fields.emplace_back(tt.prop_name_description.sv());
	if (setting.set_album) fields.emplace_back(tt.prop_name_album.sv());
	if (setting.set_album_artist) fields.emplace_back(tt.album_artist.sv());
	if (setting.set_genre) fields.emplace_back(tt.prop_name_genre.sv());
	if (setting.set_tv_show) fields.emplace_back(tt.prop_name_show.sv());
	if (setting.set_copyright_notice) fields.emplace_back(tt.copyright_notice.sv());
	if (setting.set_copyright_creator) fields.emplace_back(tt.copyright_creator.sv());
	if (setting.set_copyright_source) fields.emplace_back(tt.copyright_source.sv());
	if (setting.set_copyright_credit) fields.emplace_back(tt.copyright_credit.sv());
	if (setting.set_copyright_url) fields.emplace_back(tt.copyright_url.sv());
	const auto field_text = str::combine(fields, ", ", false);
	std::vector<row_element_ptr> rows;
	_row_names.clear();
	_row_names.reserve(items.size());
	const auto warning_color = ui::lighten(ui::style::color::important_background, 0.55f);
	int order = 0;
	for (const auto& item : items.items())
	{
		auto row = std::make_shared<row_element>(*this);
		row->_text[0] = item->path().pack();
		row->_text[1] = field_text;
		const auto md = item->metadata();
		const auto overwrites = md && ((setting.set_title && !prop::is_null(md->title)) ||
			(setting.set_comment && !prop::is_null(md->comment)) ||
			(setting.set_synopsis && !prop::is_null(md->synopsis)) || (setting.set_rating && md->rating != 0) ||
			(setting.set_year && md->year != 0) ||
			(setting.set_original && md->dates.original().is_valid()) ||
			(setting.set_created && md->dates.created().is_valid()) ||
			(setting.set_episode && (md->episode.x != 0 || md->episode.y != 0)) ||
			(setting.set_season && md->season != 0) ||
			(setting.set_track && (md->track.x != 0 || md->track.y != 0)) ||
			(setting.set_disk && (md->disk.x != 0 || md->disk.y != 0)) ||
			(setting.set_artist && !prop::is_null(md->artist)) ||
			(setting.set_caption && !prop::is_null(md->description)) || (setting.set_album && !prop::is_null(md->album))
			||
			(setting.set_album_artist && !prop::is_null(md->album_artist)) ||
			(setting.set_genre && !prop::is_null(md->genre)) || (setting.set_tv_show && !prop::is_null(md->show)) ||
			(setting.set_copyright_notice && !prop::is_null(md->copyright_notice)) ||
			(setting.set_copyright_creator && !prop::is_null(md->copyright_creator)) ||
			(setting.set_copyright_source && !prop::is_null(md->copyright_source)) ||
			(setting.set_copyright_credit && !prop::is_null(md->copyright_credit)) ||
			(setting.set_copyright_url && !prop::is_null(md->copyright_url)));
		row->_text[2] = overwrites
			                ? std::string(tt.collision_replace.sv())
			                : std::string(tt.update.sv());
		if (overwrites) row->_text_color[2] = warning_color;
		row->_order = order++;
		row->_work_index = row->_order;
		rows.emplace_back(row);
		_row_names.emplace_back(item->path().name());
	}
	_rows = std::move(rows);
	_status = format_plural_text(tt.edit_metadata_fmt, items);
}

void batch_tool_view::update_date_start()
{
	const auto& items = _state.selected_items();
	_original_start = {};
	for (const auto& item : items.items())
	{
		const auto date = item->media_created();
		if (date.is_valid() && (!_original_start.is_valid() || date < _original_start)) _original_start = date;
	}

	// Seed once. The picker cannot show an empty date, so an unseeded control would display today
	// while a different date was written. Re-seeding on every refresh would instead discard a date
	// the user chose but that is not a valid capture date, silently running something else.
	if (!_new_start_seeded)
	{
		_new_start = _original_start.is_valid() ? _original_start : platform::now();
		_new_start_seeded = true;
	}
}

void batch_tool_view::refresh_dates()
{
	const auto& items = _state.selected_items();
	update_date_start();
	std::vector<row_element_ptr> rows;
	_row_names.clear();
	_row_names.reserve(items.size());
	int order = 0;
	for (const auto& item : items.items())
	{
		auto row = std::make_shared<row_element>(*this);
		const auto original = item->media_created();
		row->_text[0] = item->path().pack();
		row->_text[1] = platform::format_date_time(original);
		row->_text[2] = platform::format_date_time(adjusted_item_date(original, _new_start, _original_start));
		row->_text[3] = std::string(adjust_date_source_name(item->metadata()));
		row->_order = order++;
		row->_work_index = row->_order;
		rows.emplace_back(row);
		_row_names.emplace_back(item->path().name());
	}
	_rows = std::move(rows);
	_status = format_plural_text(tt.adjust_date_info_fmt, items);
}

void batch_tool_view::append_blocked_reason()
{
	// A dimmed Run is only honest if the view says what would make it work. One answer, appended
	// after whatever the mode already stated about scope.
	if (const auto blocked = run_blocked_reason(); !blocked.empty() && _status.find(blocked) == std::string::npos)
	{
		if (!_status.empty()) _status += "   ";
		_status += blocked;
	}
}

void batch_tool_view::refresh()
{
	// The worker holds indexes into the reviewed plan, so re-planning under it would repoint the
	// rows a running job is reporting against.
	if (progress().active) return;
	// Every settings control routes here, so a change to the plan inputs is also what leaves results
	// mode. The finished run described the old settings and would be read as describing the new ones.
	_showing_results = false;

	// Convert plans on a worker and publishes its own rows, status and invalidation when it lands.
	if (_mode == batch_tool_mode::convert)
	{
		refresh_convert();
		_state.invalidate_view(view_invalid::status | view_invalid::command_state);
		return;
	}

	if (_mode == batch_tool_mode::metadata) refresh_metadata();
	else refresh_dates();

	append_blocked_reason();
	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
		view_invalid::command_state);
}

void batch_tool_view::refresh_encode_settings()
{
	if (progress().active) return;

	// Results describe destinations that now exist, so returning to review has to re-plan against
	// what is on disk rather than redraw a plan the run has already invalidated.
	if (_mode != batch_tool_mode::convert || showing_results() || !_plan_valid)
	{
		refresh();
		return;
	}

	describe_convert_plan();
}

// Each row is read against the plan entry it was built from, not against its position: the header
// sort is offered throughout the review, so a sorted list would otherwise report every row's
// outcome against a different file.
void batch_tool_view::show_run_results(const std::vector<item_status>& statuses, const std::string_view error)
{
	const auto success_color = ui::lighten(ui::style::color::success_background, 0.55f);
	const auto fail_color = ui::lighten(ui::style::color::warning_background, 0.55f);
	const auto muted_color = ui::darken(ui::style::color::view_text, 0.22f);

	std::vector<view_operation_result> outcomes;
	outcomes.reserve(_rows.size());

	for (const auto& row : _rows)
	{
		const auto work_index = row->_work_index >= 0 ? static_cast<size_t>(row->_work_index) : statuses.size();
		const auto status = work_index < statuses.size() ? statuses[work_index] : item_status::cancel;
		row->_text_color[1] = 0;

		switch (status)
		{
		case item_status::success:
			row->_text[2] = std::string(tt.result_success.sv());
			row->_text_color[2] = success_color;
			break;
		case item_status::fail:
			row->_text[2] = std::string(tt.result_failed.sv());
			row->_text_color[2] = fail_color;
			break;
		case item_status::ignore:
			row->_text[2] = std::string(tt.result_skipped.sv());
			row->_text_color[2] = muted_color;
			row->_text_color[1] = muted_color;
			break;
		case item_status::cancel:
			row->_text[2] = std::string(tt.result_not_run.sv());
			row->_text_color[2] = muted_color;
			row->_text_color[1] = muted_color;
			break;
		}

		outcomes.emplace_back(work_index < _row_names.size() ? _row_names[work_index] : row->_text[0], status);
	}

	_showing_results = true;
	_status = format_operation_summary(outcomes);

	// The counts state what the run did; the first error states why a row failed, which a red row
	// alone cannot.
	if (!error.empty())
	{
		if (!_status.empty()) _status += "   ";
		_status += error;
	}

	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
		view_invalid::command_state);
}

void batch_tool_view::queue_run_results(const view_state& s, const std::shared_ptr<batch_tool_view>& view,
                                        const size_t generation, std::vector<item_status> statuses, std::string fatal,
                                        std::string first_error, std::shared_ptr<detach_file_handles> detach,
                                        std::string title)
{
	s.queue_ui([view, generation, statuses = std::move(statuses), detach = std::move(detach), title = std::move(title),
		fatal = std::move(fatal), first_error = std::move(first_error)]() mutable
	{
		if (!view->is_processing_generation(generation)) return;
		view->end_processing();

		// A fault that stopped everything before any row was attempted has no per-row story to
		// tell, so the review is left as it was rather than filled with statuses nothing earned.
		if (!fatal.empty())
		{
			view->_status = fatal;
			view->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
			const auto dlg = make_dlg(view->_host->owner());
			dlg->show_message(icon_index::error, title, fatal);
			return;
		}

		view->show_run_results(statuses, first_error);
	});
}

std::string batch_tool_view::run_blocked_reason() const
{
	// While a run is in flight, or before there is anything to review, the toolbar already states
	// the situation; naming a settings problem on top of that would be noise.
	if (progress().active || _rows.empty()) return {};
	if (showing_results()) return std::string(tt.run_needs_refresh.sv());

	if (_mode == batch_tool_mode::convert)
	{
		// The plan is still being computed, so nothing is wrong yet and there is nothing to name.
		if (!_plan_valid) return {};
		if (df::folder_path(setting.write_folder).is_empty()) return std::string(tt.convert_folder_required.sv());
		if (setting.convert.limit_dimension && setting.convert.max_side < 1)
			return std::string(tt.convert_dimension_required.sv());

		// Only Block Run refuses the run; the other policies resolve the collision.
		const auto collisions = static_cast<int>(std::ranges::count_if(
			_convert_plan, [](const auto& item) { return item.collides; }));
		if (setting.convert.collision == collision_policy::block_run && collisions > 0)
			return format_collision_summary(collision_policy::block_run, collisions);

		// Every row resolved to Skip, so running would write nothing at all.
		const auto skipped = static_cast<int>(std::ranges::count_if(
			_convert_plan, [](const auto& item) { return item.skipped; }));
		if (skipped == static_cast<int>(_convert_plan.size()))
			return format_collision_summary(collision_policy::skip, skipped);

		return {};
	}

	if (_mode == batch_tool_mode::metadata)
		return metadata_changes().has_changes() ? std::string{} : std::string(tt.metadata_fields_required.sv());

	return _new_start.is_valid() ? std::string{} : std::string(tt.adjust_date_required.sv());
}

bool batch_tool_view::can_run() const
{
	if (progress().active || _rows.empty()) return false;
	// The rows on screen describe a plan the worker has already superseded.
	if (_mode == batch_tool_mode::convert && !_plan_valid) return false;
	return run_blocked_reason().empty();
}

void batch_tool_view::run()
{
	if (!can_run()) return;
	if (_mode == batch_tool_mode::convert) run_convert();
	else if (_mode == batch_tool_mode::metadata) run_metadata();
	else run_dates();
}

void batch_tool_view::run_convert()
{
	struct convert_request
	{
		size_t plan_index = 0;
		df::file_path source;
		df::file_path destination;
		str::cached xmp;
		// The review found this destination occupied and the policy said to replace it. Every other row
		// was reviewed as free, so anything now in its way is a file the user never saw.
		bool replaces_existing = false;
		// What the review saw at that destination. Replace was agreed to for that file, not for one
		// edited or swapped in since.
		platform::file_attributes_t destination_fi;
	};
	std::vector<convert_request> requests;
	requests.reserve(_convert_plan.size());
	std::vector<item_status> statuses(_convert_plan.size(), item_status::ignore);
	for (size_t index = 0; index < _convert_plan.size(); ++index)
	{
		const auto& entry = _convert_plan[index];
		// Skip resolved the collision by leaving the existing file alone; its destination still
		// names that file, so converting the row would overwrite what the review said to keep.
		if (entry.skipped) continue;
		// Nothing has been attempted yet, so a row the run never reaches reports itself as not run.
		statuses[index] = item_status::cancel;
		requests.emplace_back(index, entry.source.path, entry.destination, entry.source.xmp,
		                      entry.collides && !entry.renamed_to_avoid_collision, entry.destination_fi);
	}
	const auto title = std::string(tt.command_convert_or_resize.sv());
	const auto max_side = setting.convert.limit_dimension ? setting.convert.max_side : 0;
	record_feature_use(max_side > 0 ? features::convert | features::resize : features::convert);
	const auto jpeg_quality = setting.convert.jpeg_quality;
	const auto webp_quality = setting.convert.webp_quality;
	const auto webp_lossless = setting.convert.webp_lossless;
	const auto write_folder = df::folder_path(setting.write_folder);
	// Releasing the displayed file's handles can pump messages, and a Close handled in there leaves
	// this view. A run started after that would write files with nothing on screen to show it.
	const auto generation_before_detach = processing_generation();
	const auto detach = std::make_shared<detach_file_handles>(_state);
	if (processing_generation() != generation_before_detach) return;
	begin_processing(requests.size());
	const auto generation = processing_generation();
	const auto cancel_source = processing_cancel_source();
	const auto cancel_version = cancel_source->load();
	_status = std::string(tt.processing.sv());
	_state.queue_async(async_queue::work,
	                   [&s = _state, view = shared_from_this(), requests = std::move(requests),
		                   statuses = std::move(statuses), cancel_source, cancel_version, generation, max_side,
		                   jpeg_quality, webp_quality, webp_lossless, write_folder, detach, title]() mutable
	                   {
		                   std::string fatal;
		                   std::string first_error;
		                   auto wrote_anything = false;
		                   try
		                   {
			                   files ff;

			                   if (const auto folder_result = platform::create_folder(write_folder); folder_result.
				                   failed())
			                   {
				                   fatal = folder_result.format_error(
					                   str_format(tt.failed_to_create_folder_fmt.sv(), write_folder));
			                   }
			                   else
			                   {
				                   for (size_t index = 0; index < requests.size() && cancel_source->load() ==
				                        cancel_version; ++index)
				                   {
					                   const auto& request = requests[index];
					                   const auto plan_index = request.plan_index;
					                   s.queue_ui([view, generation, plan_index, index]
					                   {
						                   if (view->is_processing_generation(generation))
							                   view->processing_work_item(plan_index, index + 1);
					                   });
					                   file_encode_params params;
					                   params.jpeg_save_quality = jpeg_quality;
					                   params.webp_quality = webp_quality;
					                   params.webp_lossless = webp_lossless;

					                   // The review is a snapshot, and the write itself cannot refuse an existing
					                   // file. A destination that filled up in between belongs to something the user
					                   // never reviewed, so the row is left alone rather than written over - and so
					                   // is a reviewed one that has changed since, which is no longer the file the
					                   // user agreed to replace.
					                   const auto destination_exists = request.destination.exists();

					                   if (destination_exists && (!request.replaces_existing ||
						                   !unchanged_since_analysis(request.destination,
						                                             request.destination_fi.modified,
						                                             request.destination_fi.size)))
					                   {
						                   statuses[plan_index] = item_status::ignore;
						                   continue;
					                   }

					                   file_update_publish publish;
					                   publish.fail_if_exists = !request.replaces_existing;
					                   publish.require_unchanged_destination = request.replaces_existing;
					                   publish.destination_attributes = request.destination_fi;

					                   const auto result = ff.update(request.source, request.destination, {},
					                                                 max_side > 0
						                                                 ? image_edits(max_side)
						                                                 : image_edits(), params, false, request.xmp,
					                                                 {}, {}, publish);

					                   // One unreadable or unwritable source must not decide the fate of the rest.
					                   // The row records the failure and the run carries on, so the result list
					                   // says exactly which items landed.
					                   if (result.success())
					                   {
						                   statuses[plan_index] = item_status::success;
						                   wrote_anything = true;
					                   }
					                   else
					                   {
						                   statuses[plan_index] = item_status::fail;
						                   if (first_error.empty()) first_error = result.format_error();
					                   }
				                   }
			                   }

			                   // Convert writes files nobody has told the index about; without this they
			                   // stay invisible until something else happens to rescan that folder.
			                   if (wrote_anything)
			                   {
				                   df::unique_folders written;
				                   written.emplace(write_folder);
				                   s.item_index.queue_validate_changed_folders(std::move(written));
			                   }
		                   }
		                   catch (const std::exception& e)
		                   {
			                   fatal = str::utf8_cast(e.what());
		                   }

		                   queue_run_results(s, view, generation, std::move(statuses), std::move(fatal),
		                                     std::move(first_error), std::move(detach), std::move(title));
	                   });
}

void batch_tool_view::run_metadata()
{
	struct metadata_request
	{
		df::file_path path;
		str::cached xmp;
	};
	const auto items = _state.selected_items().items();
	std::vector<metadata_request> requests;
	requests.reserve(items.size());
	for (const auto& item : items)
	{
		requests.emplace_back(item->path(), item->xmp());
	}
	const auto title = std::string(tt.command_edit_metadata.sv());
	const auto edits = metadata_changes();
	record_feature_use(features::batch_edit);
	// See run_convert: a Close handled while the handles are released leaves this view.
	const auto generation_before_detach = processing_generation();
	const auto detach = std::make_shared<detach_file_handles>(_state);
	if (processing_generation() != generation_before_detach) return;
	begin_processing(items.size());
	const auto generation = processing_generation();
	const auto cancel_source = processing_cancel_source();
	const auto cancel_version = cancel_source->load();
	_status = std::string(tt.processing.sv());
	_state.queue_async(async_queue::work,
	                   [&s = _state, view = shared_from_this(), requests = std::move(requests), edits, cancel_source,
		                   cancel_version, generation, detach, title]() mutable
	                   {
		                   // Nothing has been attempted yet, so a row the run never reaches reports itself
		                   // as not run.
		                   std::vector<item_status> statuses(requests.size(), item_status::cancel);
		                   std::string fatal;
		                   std::string first_error;
		                   df::unique_folders written;
		                   try
		                   {
			                   files ff;
			                   for (size_t index = 0; index < requests.size() && cancel_source->load() == cancel_version
			                        ; ++index)
			                   {
				                   s.queue_ui([view, generation, index]
				                   {
					                   if (view->is_processing_generation(generation))
						                   view->processing_work_item(index, index + 1);
				                   });
				                   const auto& request = requests[index];

				                   // One file that cannot be written must not decide the fate of the rest.
				                   // The row records the failure and the run carries on - including when the
				                   // write throws, which a refused sidecar or a codec error does.
				                   try
				                   {
					                   const auto result = ff.update(request.path, edits, {}, file_encode_params{},
					                                                 false, request.xmp);
					                   statuses[index] = apply_batch_update_result(written, request.path.folder(),
					                                                                first_error, result);
				                   }
				                   catch (const std::exception& e)
				                   {
					                   statuses[index] = item_status::fail;
					                   if (first_error.empty()) first_error = str::utf8_cast(e.what());
				                   }
			                   }
		                   }
		                   catch (const std::exception& e)
		                   {
			                   fatal = str::utf8_cast(e.what());
		                   }

		                   // Outside the try: the write is invisible to the index until the folder is validated,
		                   // and a run that ended early still wrote everything it reported as written.
		                   if (!written.empty())
		                   {
			                   s.item_index.queue_validate_changed_folders(std::move(written));
		                   }

		                   queue_run_results(s, view, generation, std::move(statuses), std::move(fatal),
		                                     std::move(first_error), std::move(detach), std::move(title));
	                   });
}

void batch_tool_view::run_dates()
{
	struct date_update_request
	{
		df::file_path path;
		df::date_t media_created;
		str::cached xmp;
	};

	std::vector<date_update_request> requests;
	for (const auto& item : _state.selected_items().items())
	{
		requests.emplace_back(item->path(), item->media_created(), item->xmp());
	}

	record_feature_use(features::adjust_date);

	const auto title = std::string(tt.command_adjust_date.sv());
	const auto new_start = _new_start;
	const auto original_start = _original_start;
	// See run_convert: a Close handled while the handles are released leaves this view.
	const auto generation_before_detach = processing_generation();
	const auto detach = std::make_shared<detach_file_handles>(_state);
	if (processing_generation() != generation_before_detach) return;
	begin_processing(requests.size());
	const auto generation = processing_generation();
	const auto cancel_source = processing_cancel_source();
	const auto cancel_version = cancel_source->load();
	_status = std::string(tt.processing.sv());
	_state.queue_async(async_queue::work,
	                   [&s = _state, view = shared_from_this(), requests = std::move(requests), new_start,
		                   original_start, cancel_source, cancel_version, generation, detach, title]() mutable
	                   {
		                   // Nothing has been attempted yet, so a row the run never reaches reports itself
		                   // as not run.
		                   std::vector<item_status> statuses(requests.size(), item_status::cancel);
		                   std::string fatal;
		                   std::string first_error;
		                   df::unique_folders written;
		                   try
		                   {
			                   files ff;
			                   for (size_t index = 0; index < requests.size() && cancel_source->load() == cancel_version
			                        ; ++index)
			                   {
				                   s.queue_ui([view, generation, index]
				                   {
					                   if (view->is_processing_generation(generation))
						                   view->processing_work_item(index, index + 1);
				                   });
				                   const auto& request = requests[index];
				                   const auto date = adjusted_item_date(request.media_created, new_start,
				                                                        original_start);
				                   metadata_edits edits;
				                   edits.date_original = date;

				                   // One file that cannot be written must not decide the fate of the rest.
				                   // The row records the failure and the run carries on - including when the
				                   // write throws, which a refused sidecar or a codec error does.
				                   try
				                   {
					                   const auto result = ff.update(request.path, edits, {}, file_encode_params{},
					                                                 false, request.xmp);
					                   statuses[index] = apply_batch_update_result(written, request.path.folder(),
					                                                                first_error, result);
				                   }
				                   catch (const std::exception& e)
				                   {
					                   statuses[index] = item_status::fail;
					                   if (first_error.empty()) first_error = str::utf8_cast(e.what());
				                   }
			                   }
		                   }
		                   catch (const std::exception& e)
		                   {
			                   fatal = str::utf8_cast(e.what());
		                   }

		                   // Outside the try: the write is invisible to the index until the folder is validated,
		                   // and a run that ended early still wrote everything it reported as written.
		                   if (!written.empty())
		                   {
			                   s.item_index.queue_validate_changed_folders(std::move(written));
		                   }

		                   queue_run_results(s, view, generation, std::move(statuses), std::move(fatal),
		                                     std::move(first_error), std::move(detach), std::move(title));
	                   });
}

view_controls_host_ptr batch_tool_view::controls(const ui::control_frame_ptr& owner)
{
	auto result = std::make_shared<view_controls_host>(_state);
	auto frame = owner->create_dlg(result, false);
	std::vector<view_element_ptr> controls;
	const auto& items = _state.selected_items();
	auto selection_thumbnails = std::make_shared<ui::selection_thumbnails_control>(frame);
	selection_thumbnails->selection_async(items.thumbs(), items.size(), _state._async);

	if (_mode == batch_tool_mode::convert)
	{
		controls.emplace_back(create_view_info_element(tt.convert_info));
		controls.emplace_back(selection_thumbnails);
		controls.emplace_back(std::make_shared<text_element>(format_plural_text(tt.convert_info_fmt, items)));
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<text_element>(tt.destination_folder));
		controls.emplace_back(std::make_shared<ui::folder_picker_control>(frame, setting.write_folder, false,
		                                                                  [this](std::string_view) { refresh(); }));
		auto jpeg = std::make_shared<ui::check_control>(frame, tt.platform_text.jpeg_best, setting.convert.to_jpeg, true, false,
		                                                [this](const bool checked)
		                                                {
			                                                if (checked) setting.convert.to_png = setting.convert.
				                                                to_webp = false;
			                                                refresh();
		                                                },
		                                                ui::radio_group_format);
		const auto jpeg_options = std::make_shared<ui::group_control>();
		jpeg_options->add(std::make_shared<text_element>(tt.options_jpeg_quality));
		jpeg_options->add(std::make_shared<ui::slider_control>(frame, std::string_view{}, setting.convert.jpeg_quality,
		                                                       1,
		                                                       100, [this] { refresh_encode_settings(); }));
		jpeg->child(jpeg_options);
		controls.emplace_back(jpeg);
		controls.emplace_back(std::make_shared<ui::check_control>(frame, tt.platform_text.png_best, setting.convert.to_png, true,
		                                                          false,
		                                                          [this](const bool checked)
		                                                          {
			                                                          if (checked) setting.convert.to_jpeg = setting.
				                                                          convert.to_webp = false;
			                                                          refresh();
		                                                          },
		                                                          ui::radio_group_format));
		auto webp = std::make_shared<ui::check_control>(frame, tt.platform_text.webp_best, setting.convert.to_webp, true, false,
		                                                [this](const bool checked)
		                                                {
			                                                if (checked) setting.convert.to_jpeg = setting.convert.
				                                                to_png = false;
			                                                refresh();
		                                                },
		                                                ui::radio_group_format);
		const auto webp_options = std::make_shared<ui::group_control>();
		webp_options->add(std::make_shared<ui::slider_control>(frame, std::string_view{}, setting.convert.webp_quality,
		                                                       1,
		                                                       100, [this] { refresh_encode_settings(); }));
		webp_options->add(std::make_shared<ui::check_control>(frame, tt.lossless_compression,
		                                                      setting.convert.webp_lossless, false, false, [this](bool)
		                                                      {
			                                                      refresh_encode_settings();
		                                                      }));
		webp->child(webp_options);
		controls.emplace_back(webp);
		controls.emplace_back(std::make_shared<divider_element>());
		auto dimension = std::make_shared<ui::check_control>(frame, tt.limit_output_dimensions,
		                                                     setting.convert.limit_dimension, false, false, [this](bool)
		                                                     {
			                                                     refresh_encode_settings();
		                                                     });
		dimension->child(std::make_shared<ui::num_control>(frame, std::string_view{}, setting.convert.max_side, false,
		                                                   [this](int) { refresh_encode_settings(); }));
		controls.emplace_back(dimension);
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(
			create_collision_policy_control(frame, setting.convert.collision, [this] { refresh(); }));
	}
	else if (_mode == batch_tool_mode::metadata)
	{
		controls.emplace_back(create_view_info_element(tt.edit_metadata_info));
		controls.emplace_back(selection_thumbnails);
		controls.emplace_back(std::make_shared<text_element>(format_plural_text(tt.edit_metadata_fmt, items)));
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<text_element>(tt.metadata_select_field));
		std::weak_ptr<view_controls_host> weak_controls = result;
		// Only a checked field shows its editor, so the panel stays a readable list of field names
		// until the user chooses what this run is about.
		auto make_check = [&](const text_t& label, bool& selected)
		{
			auto check = std::make_shared<ui::check_control>(frame, label, selected, false, false,
			                                                 [this, weak_controls](bool)
			                                                 {
				                                                 refresh();
				                                                 if (const auto host = weak_controls.lock())
					                                                 host->scroll_controls();
			                                                 });
			return check;
		};
		auto add_field = [&](const text_t& label, bool& selected, std::string& value, const bool multiline = false)
		{
			auto check = make_check(label, selected);
			if (multiline)
				check->child(std::make_shared<ui::multi_line_edit_control>(frame, value, 6, true,
				                                                           [this](std::string_view) { refresh(); }));
			else
				check->child(std::make_shared<ui::edit_control>(frame, std::string_view{}, value,
				                                                [this](std::string_view) { refresh(); }));
			check->collapse_child_when_unchecked();
			controls.emplace_back(check);
		};
		auto add_control_field = [&](const text_t& label, bool& selected, const view_element_ptr& child)
		{
			auto check = make_check(label, selected);
			check->child(child);
			check->collapse_child_when_unchecked();
			controls.emplace_back(check);
		};
		add_field(tt.prop_name_title, setting.set_title, _metadata_title);
		add_field(tt.prop_name_comment, setting.set_comment, _metadata_comment, true);
		add_field(tt.prop_name_synopsis, setting.set_synopsis, _metadata_synopsis, true);
		add_field(tt.prop_name_artist, setting.set_artist, setting.artist);
		add_field(tt.prop_name_description, setting.set_caption, setting.caption, true);
		add_field(tt.prop_name_album, setting.set_album, setting.album);
		add_field(tt.album_artist, setting.set_album_artist, setting.album_artist);
		const auto genre_group = std::make_shared<ui::group_control>();
		genre_group->add(std::make_shared<ui::edit_picker_control>(frame, setting.genre,
		                                                           genre_suggestions(_state, items),
		                                                           [this](std::string_view) { refresh(); }));
		genre_group->add(std::make_shared<text_element>(tt.genre_separator_help));
		add_control_field(tt.prop_name_genre, setting.set_genre, genre_group);
		add_field(tt.prop_name_show, setting.set_tv_show, setting.tv_show);
		add_control_field(tt.prop_name_rating, setting.set_rating,
		                  std::make_shared<rating_edit_control>(_metadata_rating, [this](int) { refresh(); }));
		add_control_field(tt.prop_name_year, setting.set_year,
		                  std::make_shared<ui::num_control>(frame, std::string_view{}, _metadata_year, true,
		                                                    [this](int) { refresh(); }));
		// A date field carries what the item holds now, inside the checkbox: the value a run is about
		// to replace is worth seeing beside its replacement, and worth nothing while the field is not
		// in the run.
		auto add_date_field = [&](const text_t& label, bool& selected, df::date_t& value,
		                          const prop::date_concept kind)
		{
			const auto group = std::make_shared<ui::group_control>();
			group->add(std::make_shared<ui::date_control>(frame, value, true, [this] { refresh(); }));

			if (const auto current = agreed_date(items, label.sv(), kind))
			{
				const auto table = std::make_shared<ui::table_element>();
				table->add(current->label, current->value, current->source);
				group->add(table);
			}

			add_control_field(label, selected, group);
		};
		add_date_field(tt_prep(tt.prop_name_original.sv()), setting.set_original, _metadata_date_original,
		               prop::date_concept::original);
		add_date_field(tt_prep(tt.prop_name_created.sv()), setting.set_created, _metadata_date_created,
		               prop::date_concept::created);
		// Modified is a fact about the file rather than a field, so the editor reports it and does not
		// offer it. Named only where the whole selection agrees, for the reason above.
		if (const auto modified = agreed_date(items, tt_prep(tt.prop_name_modified.sv()),
		                                      prop::date_concept::modified))
		{
			const auto table = std::make_shared<ui::table_element>();
			table->add(modified->label, modified->value, modified->source);
			controls.emplace_back(table);
		}
		add_control_field(tt.prop_name_episode, setting.set_episode,
		                  std::make_shared<ui::num_pair_control>(frame, std::string_view{}, _metadata_episode));
		add_control_field(tt.prop_name_season, setting.set_season,
		                  std::make_shared<ui::num_control>(frame, std::string_view{}, _metadata_season, true,
		                                                    [this](int) { refresh(); }));
		add_control_field(tt.prop_name_track, setting.set_track,
		                  std::make_shared<ui::num_pair_control>(frame, std::string_view{}, _metadata_track));
		add_control_field(tt.prop_name_disk, setting.set_disk,
		                  std::make_shared<ui::num_pair_control>(frame, std::string_view{}, _metadata_disk));
		add_field(tt.copyright_notice, setting.set_copyright_notice, setting.copyright_notice, true);
		add_field(tt.copyright_creator, setting.set_copyright_creator, setting.copyright_creator);
		add_field(tt.copyright_source, setting.set_copyright_source, setting.copyright_source);
		add_field(tt.copyright_credit, setting.set_copyright_credit, setting.copyright_credit);
		add_field(tt.copyright_url, setting.set_copyright_url, setting.copyright_url);
	}
	else
	{
		// The controls are built before the view is activated, so the starting date must be known here
		// or the picker would show a date that neither the preview nor the run would use.
		update_date_start();
		controls.emplace_back(create_view_info_element(tt.adjust_date_info));
		controls.emplace_back(selection_thumbnails);
		controls.emplace_back(std::make_shared<text_element>(format_plural_text(tt.adjust_date_info_fmt, items)));
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<text_element>(tt.adjust_date_help1));
		controls.emplace_back(std::make_shared<text_element>(tt.adjust_date_help2));
		controls.emplace_back(std::make_shared<text_element>(tt.adjust_date_help3));
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<text_element>(tt.starting_date_label));
		controls.emplace_back(std::make_shared<ui::date_control>(frame, _new_start, true, [this] { refresh(); }));
	}

	for (const auto& control : controls)
	{
		control->margin.cx = 8;
		control->margin.cy = 4;
	}
	result->_controls = std::move(controls);
	result->_frame = result->_dlg = frame;
	return result;
}
