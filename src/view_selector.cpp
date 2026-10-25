// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Compact horizontal item selector used by edit and locate workflows.

#include "pch.h"
#include "model.h"
#include "model_index.h"
#include "view_selector.h"

class selector_item_controller final : public view_controller
{
	selector_view& _view;
	selector_view::select_item_fn _select_item;
	df::item_element_ptr _item;
	bool _tracking = false;
	bool _dragging = false;
	pointi _last_loc;

public:
	selector_item_controller(const view_host_ptr& host, selector_view& view, const recti bounds,
	                         selector_view::select_item_fn select_item, df::item_element_ptr item) :
		view_controller(host, bounds), _view(view), _select_item(std::move(select_item)), _item(std::move(item))
	{
	}

	ui::style::cursor cursor() const override
	{
		return _tracking ? ui::style::cursor::hand_up : ui::style::cursor::hand_down;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		view_controller::on_mouse_left_button_down(loc, keys);
		_tracking = true;
		_dragging = false;
		_last_loc = loc;
	}

	void on_mouse_move(const pointi loc) override
	{
		if (!_tracking) return;

		const auto drag_distance = std::abs(loc.x - _start_loc.x);
		if (_dragging || drag_distance > 4)
		{
			_dragging = true;
			_view.scroll_by(_last_loc.x - loc.x);
			_last_loc = loc;
		}
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_tracking && _dragging)
		{
			_view.scroll_by(_last_loc.x - loc.x);
		}
		else if (_tracking && _bounds.contains(loc) && _select_item && _item)
		{
			_select_item(_item, keys);
		}
		_tracking = false;
		_dragging = false;
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		_dragging = false;
		return true;
	}
};

class selector_scroll_controller final : public view_controller
{
	selector_view& _view;
	bool _tracking = false;
	int _grab_offset = 0;

public:
	selector_scroll_controller(const view_host_ptr& host, selector_view& view, const recti bounds) :
		view_controller(host, bounds), _view(view)
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::left_right;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
		const auto thumb = _view.scrollbar_thumb_bounds();
		_grab_offset = thumb.contains(loc) ? loc.x - thumb.left : thumb.width() / 2;
		_view.scrollbar_to(loc.x - _grab_offset);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_tracking) _view.scrollbar_to(loc.x - _grab_offset);
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_tracking)
		{
			_view.scrollbar_to(loc.x - _grab_offset);
			_tracking = false;
		}
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		return true;
	}
};

selector_view::selector_view(view_state& state, view_host_ptr host, select_item_fn select_item) :
	_state(state), _host(std::move(host)), _select_item(std::move(select_item))
{
}

selector_view::rebuild_stats selector_view::rebuild_selector_items(std::vector<selector_item>& existing,
                                                                   const std::vector<df::item_element_ptr>& ordered,
                                                                   std::vector<selector_item>& items)
{
	rebuild_stats stats;
	std::unordered_map<const df::item_element*, size_t> old_by_item;
	old_by_item.reserve(existing.size());

	for (size_t index = 0; index < existing.size(); ++index)
	{
		old_by_item.emplace(existing[index].item.get(), index);
	}

	stats.lookup_entries = old_by_item.size();
	std::vector<bool> reused(existing.size(), false);
	items.reserve(ordered.size());

	for (const auto& item : ordered)
	{
		++stats.probes;
		const auto found = old_by_item.find(item.get());
		if (found != old_by_item.end() && !reused[found->second])
		{
			reused[found->second] = true;
			items.emplace_back(std::move(existing[found->second]));
			++stats.reused;
		}
		else
		{
			items.push_back({item});
		}
	}

	return stats;
}

void selector_view::filter(item_filter_fn item_filter)
{
	// The strip is rebuilt by the activate that follows, so setting the filter does not walk the
	// items a second time.
	_item_filter = std::move(item_filter);
}

df::item_elements selector_view::selection_range(const df::item_element_ptr& item) const
{
	const auto item_pos = std::ranges::find(_items, item, &selector_item::item);
	if (item_pos == _items.end()) return {};

	// The anchor stays where the last plain click left it, so repeated Shift clicks re-measure one
	// range rather than pivoting on the previous click.
	auto anchor_pos = std::ranges::find(_items, _selection_anchor, &selector_item::item);
	if (anchor_pos == _items.end()) anchor_pos = std::ranges::find(_items, _state.focus_item(), &selector_item::item);
	if (anchor_pos == _items.end()) anchor_pos = item_pos;

	const auto first = std::min(anchor_pos, item_pos);
	const auto last = std::max(anchor_pos, item_pos);
	df::item_elements result;
	result.reserve(static_cast<size_t>(std::distance(first, last)) + 1);

	// view_state::select focuses the first entry, so the clicked item leads: focus follows the
	// pointer instead of jumping to whichever end of the range came first.
	result.emplace_back(item);
	for (auto pos = first; pos != std::next(last); ++pos)
	{
		if (pos != item_pos) result.emplace_back(pos->item);
	}

	return result;
}

quadd selector_view::thumbnail_destination(sizei texture_dimensions, const recti image_bounds,
                                           const ui::orientation orientation, const bool show_rotated)
{
	if (show_rotated && flips_xy(orientation))
	{
		std::swap(texture_dimensions.cx, texture_dimensions.cy);
	}

	const auto destination = ui::scale_dimensions(texture_dimensions, image_bounds);
	return show_rotated ? quadd(destination).transform(to_simple_transform(orientation)) : quadd(destination);
}

void selector_view::activate(const sizei extent)
{
	_active = true;
	_extent = extent;
	rebuild_items();

	// Item bounds are computed in layout, which has not run yet when a task view opens, so the scroll
	// that reveals the focused item is deferred rather than lost.
	_scroll_to_focus = true;
	make_visible(_state.focus_item());
}

void selector_view::deactivate()
{
	_active = false;
	for (const auto& entry : _items) entry.item->is_visible(false);
	_items.clear();
	_selection_anchor.reset();
	_scroll_x = 0;
	_content_width = 0;
	_scroll_to_focus = false;
}

void selector_view::refresh()
{
	rebuild_items();
}

void selector_view::items_changed(const bool path_changed)
{
	if (path_changed) _scroll_x = 0;
	rebuild_items();
}

void selector_view::display_changed()
{
	_host->frame()->invalidate();
}

void selector_view::rebuild_items()
{
	// An inactive strip is not on screen: it holds no items, so it neither claims item visibility nor
	// queues thumbnails on behalf of the view that is.
	if (!_active) return;

	std::vector<df::item_element_ptr> ordered;
	ordered.reserve(_state.display_items().size());

	for (const auto& group : _state.groups())
	{
		for (const auto& item : group->items())
		{
			if (!_item_filter || _item_filter(item)) ordered.emplace_back(item);
		}
	}

	const auto same_items = _items.size() == ordered.size() &&
		std::ranges::equal(_items, ordered, {}, &selector_item::item, std::identity{});

	if (!same_items)
	{
		for (const auto& entry : _items) entry.item->is_visible(false);

		std::vector<selector_item> items;
		rebuild_selector_items(_items, ordered, items);
		_items = std::move(items);
	}

	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller);
}

void selector_view::clamp_scroll()
{
	_scroll_x = std::clamp(_scroll_x, 0, std::max(0, _content_width - _extent.cx));
}

void selector_view::update_visible_items()
{
	if (!_active || _extent.is_empty()) return;

	df::item_elements visible;
	const auto logical_bounds = resource_logical_bounds();

	for (const auto& entry : _items)
	{
		const auto is_visible = entry.bounds.intersects(logical_bounds);
		entry.item->is_visible(is_visible);
		if (is_visible) visible.emplace_back(entry.item);
	}

	if (!visible.empty()) _state.item_index.queue_load_visible_thumbnails(std::move(visible));
	retire_off_band_resources();
}

recti selector_view::resource_logical_bounds() const
{
	return recti(_extent).offset(_scroll_x, 0).inflate(_extent.cx / 2, 0);
}

void selector_view::retire_off_band_resources()
{
	if (_extent.is_empty()) return;

	retire_off_band_resources(_items, resource_logical_bounds());
}

size_t selector_view::retire_off_band_resources(std::vector<selector_item>& items, const recti logical_bounds)
{
	size_t retired = 0;

	for (auto& entry : items)
	{
		if (!entry.bounds.intersects(logical_bounds) && entry.has_retained_resource())
		{
			entry.retire_resources();
			++retired;
		}
	}

	return retired;
}

bool selector_view::publish_decoded_surface(const df::item_element_ptr& item, const ui::const_image_ptr& image,
                                            const uint64_t request, ui::surface_ptr surface)
{
	return publish_decoded_surface(_items, resource_logical_bounds(), item, image, request, std::move(surface));
}

bool selector_view::publish_decoded_surface(std::vector<selector_item>& items, const recti logical_bounds,
                                            const df::item_element_ptr& item, const ui::const_image_ptr& image,
                                            const uint64_t request, ui::surface_ptr surface)
{
	const auto found = std::ranges::find(items, item, &selector_item::item);
	if (found == items.end() || found->image != image || found->decode_request != request) return false;

	found->decode_pending = false;

	if (!found->bounds.intersects(logical_bounds))
	{
		found->retire_resources();
		return false;
	}

	if (!ui::is_valid(surface))
	{
		found->decode_failed = true;
		return false;
	}

	found->surface = std::move(surface);
	return true;
}

void selector_view::layout(ui::measure_context& mc, const sizei extent)
{
	_extent = extent;
	_gap = std::max(mc.padding1, df::round(6 * mc.scale_factor));
	const auto outer_padding = std::max(mc.padding1, df::round(8 * mc.scale_factor));
	const auto text_height = mc.text_line_height(ui::style::font_face::dialog);
	_scrollbar_height = 0;

	for (auto pass = 0; pass < 2; ++pass)
	{
		const auto tile_height = std::max(1, extent.cy - outer_padding * 2 - _scrollbar_height);
		const auto tile_width = std::max(df::round(72 * mc.scale_factor), tile_height - text_height / 2);
		const auto image_height = std::max(1, tile_height - text_height - mc.padding1);
		const auto image_width = std::min(tile_width, image_height * 4 / 3);
		const auto item_width = std::max(tile_width, image_width);

		auto x = outer_padding;
		for (auto& entry : _items)
		{
			entry.bounds = {x, outer_padding, x + item_width, outer_padding + tile_height};
			x += item_width + _gap;
		}

		_content_width = std::max(0, x - _gap + outer_padding);
		const auto required_height = _content_width > extent.cx
			                             ? std::max(df::round(10 * mc.scale_factor), mc.padding1)
			                             : 0;
		if (required_height == _scrollbar_height) break;
		_scrollbar_height = required_height;
	}

	clamp_scroll();

	if (_scroll_to_focus)
	{
		_scroll_to_focus = false;
		make_visible(_state.focus_item());
	}

	update_visible_items();
	_host->frame()->invalidate();
	_state.invalidate_view(view_invalid::controller);
}

selector_view::selector_item* selector_view::item_from_location(const pointi loc)
{
	const auto logical = loc + pointi(_scroll_x, 0);
	const auto found = std::ranges::find_if(_items, [logical](const selector_item& entry)
	{
		return entry.bounds.contains(logical);
	});
	return found == _items.end() ? nullptr : &*found;
}

const selector_view::selector_item* selector_view::item_from_location(const pointi loc) const
{
	return const_cast<selector_view*>(this)->item_from_location(loc);
}

void selector_view::render(ui::draw_context& dc, const view_controller_ptr controller)
{
	const auto client = recti(_extent);
	const auto text_height = dc.text_line_height(ui::style::font_face::dialog);
	const auto focus = _state.focus_item();

	for (auto& entry : _items)
	{
		const auto bounds = entry.bounds.offset(-_scroll_x, 0);
		if (!bounds.intersects(client)) continue;

		const auto is_focus = entry.item == focus;
		const auto is_selected = entry.item->is_selected();
		const auto is_hover = controller && controller->bounds() == bounds;
		const auto bg = view_handle_color(is_selected, is_hover ? 1 : 0, false, _view_has_focus, false);
		if (is_focus || is_selected || is_hover) dc.draw_rounded_rect(bounds, bg, dc.padding1);

		auto image_bounds = bounds.inflate(-dc.padding1);
		image_bounds.bottom -= text_height + dc.padding1;
		const auto image = entry.item->thumbnail();

		if (image != entry.image)
		{
			entry.image = image;
			entry.texture.reset();
			entry.surface.reset();
			entry.decode_pending = false;
			entry.decode_failed = false;
			++entry.decode_request;
		}

		if (entry.surface)
		{
			auto texture = dc.create_texture();

			if (texture && texture->update(entry.surface) != ui::texture_update_result::failed)
			{
				entry.texture = std::move(texture);
				entry.surface.reset();
			}
			// Otherwise the surface is kept: creating a device resource fails while the device is lost, and
			// dropping it here would send the decode round again instead of retrying the upload.
		}

		if (is_valid(image) && !entry.texture && !entry.surface && !entry.decode_pending && !entry.decode_failed)
		{
			// Paint asks for the decode instead of performing it. The item type icon below stands in
			// until the surface lands, which is what an item without a thumbnail already draws.
			entry.decode_pending = true;
			const auto request = ++entry.decode_request;

			_state._async.queue_async(async_queue::load,
			                          [weak = weak_from_this(), &async = _state._async, item = entry.item, image,
				                          extent = image_bounds.extent(), can_use_yuv = ui::yuv_textures_enabled,
				                          request]
			                          {
				                          files ff;
				                          auto surface = ff.image_to_surface(image, extent, can_use_yuv);

				                          async.queue_ui([weak, item, image, request, surface = std::move(surface)]() mutable
				                          {
					                          const auto view = weak.lock();
					                          if (!view) return;

					                          if (view->publish_decoded_surface(item, image, request, std::move(surface)))
					                          {
						                          view->_host->frame()->invalidate();
					                          }
				                          });
			                          });
		}

		if (entry.texture)
		{
			const auto orientation = entry.item->layout_orientation();
			const auto destination = thumbnail_destination(entry.texture->dimensions(), image_bounds, orientation,
			                                               setting.show_rotated);
			const auto sampler = calc_sampler(destination.bounding_rect().extent().round(),
			                                  entry.texture->dimensions(), orientation);
			dc.draw_texture(entry.texture, destination, entry.texture->dimensions(), dc.colors.alpha, sampler);
		}
		else
		{
			const auto icon = entry.item->is_folder() ? icon_index::folder : entry.item->file_type()->icon;
			xdraw_icon(dc, icon, image_bounds,
			           ui::color(dc.colors.foreground, dc.colors.alpha * 0.5f), {});
		}

		auto text_bounds = bounds;
		text_bounds.top = text_bounds.bottom - text_height - dc.padding1;
		dc.draw_text(entry.item->name(), text_bounds.inflate(-dc.padding1, 0), ui::style::font_face::dialog,
		             ui::style::text_style::single_line_center, ui::color(dc.colors.foreground, dc.colors.alpha), {});

		if (is_focus)
		{
			const auto border = bounds.inflate(-df::round(2 * dc.scale_factor));
			const auto border_clr = ui::color(ui::style::color::view_selected_background, dc.colors.alpha);
			dc.draw_border(border, border.inflate(df::round(2 * dc.scale_factor)),
			               border_clr, border_clr);
		}
	}

	if (can_scroll())
	{
		const auto track = scrollbar_bounds();
		const auto thumb = scrollbar_thumb_bounds();
		dc.draw_rounded_rect(track, ui::color(dc.colors.foreground, dc.colors.alpha * 0.15f), track.height() / 2);
		dc.draw_rounded_rect(thumb, ui::color(dc.colors.foreground, dc.colors.alpha * 0.55f), thumb.height() / 2);
	}
}

bool selector_view::mouse_wheel(const pointi loc, const ui::wheel_notch notch)
{
	// A strip has one axis, so both wheels drive it.
	scroll_by(notch.is_vertical() ? -notch.delta : notch.delta);
	return can_scroll();
}

view_controller_ptr selector_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                           hit_test_context& ctx)
{
	if (can_scroll() && ctx.occluded(scrollbar_bounds()))
	{
		return std::make_shared<selector_scroll_controller>(host, *this, scrollbar_bounds());
	}

	if (const auto entry = item_from_location(loc))
	{
		return std::make_shared<selector_item_controller>(host, *this, entry->bounds.offset(-_scroll_x, 0),
		                                                  _select_item, entry->item);
	}
	return nullptr;
}

void selector_view::make_visible(const df::item_element_ptr& item)
{
	if (!item || _extent.cx <= 0) return;
	const auto found = std::ranges::find(_items, item, &selector_item::item);
	if (found == _items.end()) return;

	const auto previous_scroll = _scroll_x;
	if (found->bounds.left < _scroll_x)
	{
		_scroll_x = found->bounds.left - _gap;
	}
	else if (found->bounds.right > _scroll_x + _extent.cx)
	{
		_scroll_x = found->bounds.right - _extent.cx + _gap;
	}
	clamp_scroll();
	update_visible_items();
	if (_scroll_x != previous_scroll) _host->frame()->invalidate();
}

bool selector_view::can_scroll() const
{
	return _content_width > _extent.cx && _extent.cx > 0;
}

recti selector_view::scrollbar_bounds() const
{
	const auto padding = std::max(2, _scrollbar_height / 4);
	return {
		padding, _extent.cy - _scrollbar_height + padding,
		_extent.cx - padding, _extent.cy - padding
	};
}

recti selector_view::scrollbar_thumb_bounds() const
{
	const auto track = scrollbar_bounds();
	const auto thumb_width = std::max(_scrollbar_height * 3,
	                                  df::mul_div(track.width(), _extent.cx, _content_width));
	const auto travel = std::max(0, track.width() - thumb_width);
	const auto max_scroll = std::max(1, _content_width - _extent.cx);
	const auto left = track.left + df::mul_div(_scroll_x, travel, max_scroll);
	return {left, track.top, left + thumb_width, track.bottom};
}

void selector_view::scrollbar_to(const int x)
{
	const auto track = scrollbar_bounds();
	const auto thumb = scrollbar_thumb_bounds();
	const auto travel = std::max(1, track.width() - thumb.width());
	_scroll_x = df::mul_div(std::clamp(x - track.left, 0, travel), _content_width - _extent.cx, travel);
	clamp_scroll();
	update_visible_items();
	_host->frame()->invalidate();
	_state.invalidate_view(view_invalid::controller);
}

void selector_view::scroll_by(const int delta_x)
{
	if (delta_x == 0) return;
	_scroll_x += delta_x;
	clamp_scroll();
	update_visible_items();
	_host->frame()->invalidate();
	_state.invalidate_view(view_invalid::controller);
}

void selector_view::broadcast_event(const view_element_event& event) const
{
	if (event.type == view_element_event_type::free_graphics_resources ||
		event.type == view_element_event_type::dpi_changed)
	{
		// The decode latch is dropped with the textures: a decode that failed under memory pressure or
		// at a previous scale deserves one more attempt once the view is rebuilt.
		for (auto& entry : const_cast<std::vector<selector_item>&>(_items))
		{
			entry.texture.reset();
			entry.decode_pending = false;
			entry.decode_failed = false;
			++entry.decode_request;
		}
	}
}

void selector_view::test_add_pending_decode_for_resource_event()
{
	selector_item entry;
	entry.decode_pending = true;
	entry.decode_failed = true;
	entry.decode_request = 3;
	_items.emplace_back(std::move(entry));
}

bool selector_view::test_resource_event_cleared_pending_decode() const
{
	return !_items.empty() && !_items.front().decode_pending && !_items.front().decode_failed &&
		_items.front().decode_request == 4;
}

selector_view::rebuild_stats selector_view::test_rebuild_reuse(const size_t count)
{
	std::vector<selector_item> existing;
	std::vector<df::item_element_ptr> ordered;
	existing.reserve(count);
	ordered.reserve(count + 1);

	for (size_t index = 0; index < count; ++index)
	{
		df::index_file_item indexed;
		indexed.name = str::cache(str::print("item%zu.jpg", index));
		indexed.ft = files::file_type_from_name(indexed.name);
		auto item = std::make_shared<df::item_element>(df::file_path(str::print("c:\\item%zu.jpg", index)), indexed);
		existing.push_back({item, recti(static_cast<int>(index), 0, static_cast<int>(index + 1), 1)});
		ordered.emplace_back(std::move(item));
	}

	std::ranges::reverse(ordered);

	df::index_file_item indexed;
	indexed.name = str::cache("appended.jpg");
	indexed.ft = files::file_type_from_name(indexed.name);
	ordered.emplace_back(std::make_shared<df::item_element>(df::file_path("c:\\appended.jpg"), indexed));

	std::vector<selector_item> rebuilt;
	auto stats = rebuild_selector_items(existing, ordered, rebuilt);

	for (size_t index = 0; index < count; ++index)
	{
		if (rebuilt[index].bounds.left == static_cast<int>(count - index - 1)) ++stats.preserved_markers;
	}

	return stats;
}

selector_view::resource_stats selector_view::test_resource_retirement(const size_t count, const sizei extent,
                                                                      const int item_width, const int scroll_x)
{
	std::vector<selector_item> items;
	items.reserve(count);

	const auto image = std::make_shared<ui::image>(df::blob(16), sizei(16, 16), ui::image_format::JPEG,
	                                               ui::orientation::top_left);

	for (size_t index = 0; index < count; ++index)
	{
		df::index_file_item indexed;
		indexed.name = str::cache(str::print("resource%zu.jpg", index));
		indexed.ft = files::file_type_from_name(indexed.name);
		auto item = std::make_shared<df::item_element>(df::file_path(str::print("c:\\resource%zu.jpg", index)),
		                                               indexed);
		auto surface = std::make_shared<ui::surface>();
		surface->alloc({8, 8}, ui::texture_format::ARGB, ui::orientation::top_left);
		items.push_back({std::move(item), recti(static_cast<int>(index) * item_width, 0,
		                                        static_cast<int>(index + 1) * item_width, extent.cy), image, {},
		                 std::move(surface), index == 0, false, static_cast<uint64_t>(index + 1)});
	}

	const auto retained = [&items]
	{
		return std::ranges::count_if(items, [](const selector_item& entry)
		{
			return entry.has_retained_resource();
		});
	};

	resource_stats stats;
	stats.before = retained();
	const auto stale_item = items.front().item;
	const auto stale_request = items.front().decode_request;
	const auto scrolled_bounds = recti(extent).offset(scroll_x, 0).inflate(extent.cx / 2, 0);
	retire_off_band_resources(items, scrolled_bounds);
	stats.after_retire = retained();

	auto stale_surface = std::make_shared<ui::surface>();
	stale_surface->alloc({8, 8}, ui::texture_format::ARGB, ui::orientation::top_left);
	publish_decoded_surface(items, scrolled_bounds, stale_item, image, stale_request, std::move(stale_surface));
	stats.after_stale_publish = retained();

	auto reentry_surface = std::make_shared<ui::surface>();
	reentry_surface->alloc({8, 8}, ui::texture_format::ARGB, ui::orientation::top_left);
	auto& reentry = items.front();
	reentry.image = image;
	const auto reentry_request = ++reentry.decode_request;
	const auto reentered_bounds = recti(extent).inflate(extent.cx / 2, 0);
	retire_off_band_resources(items, reentered_bounds);
	publish_decoded_surface(items, reentered_bounds, reentry.item, image, reentry_request, std::move(reentry_surface));
	stats.after_reentry_publish = retained();

	return stats;
}
