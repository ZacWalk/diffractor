// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: People and the Unknown and Hidden face inboxes, face review and undoable placements.
// Owned by docs/faces.md#9-the-faces-view.

#pragma once

#include "model.h"
#include "model_faces_search.h"
#include "ui_view.h"
#include "libdesktop/ui/list_selection.h"

class view_controls_host;
namespace ui
{
	class check_control;
}
using view_controls_host_ptr = std::shared_ptr<view_controls_host>;

// What the Faces view shows, made off the UI thread from the index and face search, and replaced whole.
struct faces_view_model
{
	// What a row or a face is cut from: a face face search found, a picture's region, or a whole picture.
	struct shown
	{
		std::optional<uint32_t> face;
		core::file_path path;
		// Fractions of the upright picture; empty for the whole picture.
		rectd area;
		// In a video, the frame the face was found in, in milliseconds: docs/faces.md#33-videos.
		std::optional<uint32_t> frame_ms;

		bool has_area() const { return area.Width > 0.0 && area.Height > 0.0; }
		// A face of the store, where it was found.
		static shown of(const faces::face_store& store, uint32_t face);
	};

	// A person the collection's files name, as their metadata names them.
	struct person
	{
		// As most of their pictures spell them.
		std::string name;
		uint32_t pictures = 0;
		shown face;
		// Each region naming them, and each picture naming them without one.
		std::vector<shown> named;
	};

	faces::face_store_const_ptr store;
	faces::overview_ptr overview;
	// Most pictures first.
	std::vector<person> people;
	const person* find_person(std::string_view name) const;
};

using faces_view_model_ptr = std::shared_ptr<const faces_view_model>;

// The people the collection's pictures name, with where each region places them, and face search's overview when it
// is on. `cancel` answers true once the model is no longer wanted.
faces_view_model_ptr make_faces_view_model(const index_state& index, faces::face_store_const_ptr store,
                                           faces::overview_ptr overview, const std::function<bool()>& cancel);

using face_edits_by_path = core::hash_map<core::file_path, face_name_edits, core::ihash, core::ieq>;

// docs/faces.md#10-saving-names-to-files: what saving names writes - each picture's name edits, from the faces the
// face book has put in a group or taken out of one and the regions its file holds - and how many faces it names and
// how many names it takes out.
struct face_name_plan
{
	face_edits_by_path edits;
	size_t named = 0;
	size_t removed = 0;
};

face_name_plan plan_name_writes(const index_state& index, const faces::face_book& book);

// docs/faces.md#31-turning-it-on-and-off, from the Faces view and from Options: turning face search on for photos,
// videos or both, and deleting what it found, ask first; turning a kind off does not, and turning on neither turns face
// search off. Each saves the setting.
bool turn_on_face_search(view_state& state, const ui::frame_ptr& parent, bool photos = true, bool videos = true);
void turn_off_face_search(view_state& state);
void pause_face_search(view_state& state, bool paused);
bool delete_face_data(view_state& state, const ui::frame_ptr& parent);

// docs/faces.md#65-saving-and-moving-the-face-book: the face book written to a CSV file, and one read into it, on a
// worker; `done` is called on the UI thread with what to tell the user, and whether it went well.
void export_face_book(view_state& state, core::file_path path, std::function<void(bool, std::string)> done);
void import_face_book(view_state& state, core::file_path path, std::function<void(bool, std::string)> done);

// docs/faces.md#93-faces-on-a-picture: how a face on a picture is named - as anyone, a suggestion offered first, or as
// someone other than whom it is suggested as - and whether it was, and the user asked to go on to the next picture.
enum class picture_naming : uint8_t
{
	anyone,
	someone_else,
};

struct picture_named
{
	bool named = false;
	bool next = false;
};

// docs/faces.md#93-faces-on-a-picture: name or rename this face without leaving the picture, in the popup
// docs/faces.md#95-naming-a-face describes; `offer_next` offers going on to the next picture once it is named.
picture_named name_picture_face(view_state& state, const ui::frame_ptr& parent, uint32_t face, picture_naming how,
                                bool offer_next);
// docs/faces.md#93-faces-on-a-picture: confirm the person a face on a picture is suggested as, or rule them out for it.
bool review_picture_face(view_state& state, uint32_t face, bool confirm);

// docs/faces.md#93-faces-on-a-picture: the face Ctrl+F2 names on a picture - its only one - or why there is none.
struct picture_face_target
{
	std::optional<uint32_t> face;
	std::string_view refusal;
};

picture_face_target only_face_on_picture(view_state& state, core::file_path path);

namespace ui
{
	class complete_strategy_t;
}

// docs/faces.md#95-naming-a-face: a name the naming popup can offer, with where it matches what was typed and how many
// pictures it is in; and what gives them for what is typed - the collection, or a test.
struct person_word
{
	std::string text;
	std::vector<core::part_t> highlights;
	int occurrences = 0;
};

using person_vocabulary = std::function<std::vector<person_word>(std::string_view query, size_t limit)>;

// The naming popup's list, for tests: `dialog` is what a double-click on a row answers.
std::shared_ptr<ui::complete_strategy_t> make_person_picker_for_test(view_state& state, ui::frame_ptr dialog,
                                                                     std::vector<std::string> likely,
                                                                     std::vector<std::string> recent,
                                                                     std::vector<std::string> leave_out, bool preselect,
                                                                     person_vocabulary people);

class face_crop_queue;

class faces_view final : public view_base, public std::enable_shared_from_this<faces_view>
{
public:
	// A row of the People pane.
	struct row
	{
		enum class kind : uint8_t
		{
			unknown,
			hidden_faces,
			surname,
			person,
			file_person
		};
		kind what = kind::person;
		std::string key, title, detail, surname;
		faces_view_model::shown face;
		std::vector<faces_view_model::shown> previews;
		// Normally one entry each. Immediately after Combine the last completed scan can
		// still have two entries for the same person; keep both sets of confirmed faces.
		std::vector<int> overview_people;
		std::vector<int> file_people;
		uint32_t book_person = 0;
		bool named() const { return what == kind::person || what == kind::file_person; }
		// In the People pane's content.
		recti bounds;
		recti title_bounds;
		// Where the pane is too narrow for them, the name and the count end in an ellipsis.
		bool title_clipped = false;
		bool detail_clipped = false;
	};

	// A face, or a named region, in the faces pane.
	struct cell
	{
		faces_view_model::shown shown;
		std::string caption;
		std::string detail;
		faces::member_kind member = faces::member_kind::unknown;
		float score = 0.0f;
		bool hidden = false;
		// In the faces pane's content.
		recti bounds;
	};

	// docs/faces.md#9-the-faces-view: what a button at the end of a heading does to every face listed under it - in
	// Suggested, confirm them, once the user agrees; in Unknown, name them as one person or hide them.
	enum class heading_action : uint8_t
	{
		confirm_all,
		name,
		hide,
	};

	// docs/faces.md#92-faces: the shortcuts on a hovered suggestion, each acting on that face alone - confirm it, rule
	// the person out for it, or name it as someone else.
	enum class suggestion_action : uint8_t
	{
		confirm,
		reject,
		name,
	};

	struct heading_button
	{
		heading_action action = heading_action::confirm_all;
		// In the faces pane's content; empty until laid out.
		recti bounds;
	};

	struct heading
	{
		std::string text;
		// What the scroll bar names its faces by, where that is shorter than the heading: a group's name rather than
		// its name and how many faces it holds. Empty, the heading's text.
		std::string label;
		// The cells laid out before it.
		size_t before = 0;
		// Its buttons, in the order they read, at the end of its line.
		std::vector<heading_button> buttons;
		recti bounds;
		// Which heading it is, from one build to the next, so the page can be kept where it was: a section's name, or for
		// a run of lookalikes the first face of the run it was first shown as.
		std::string key;

		bool has(const heading_action action) const
		{
			return std::ranges::any_of(buttons, [action](const heading_button& b) { return b.action == action; });
		}
	};

	enum class pane : uint8_t
	{
		groups,
		faces,
	};

	// What a drag is over, and what dropping there would do.
	struct drop_target
	{
		int row = -1;
		// Empty space rather than a drop destination.
		bool list = false;
		bool valid = false;
	};

private:
	view_state& _state;
	view_host_ptr _host;
	sizei _extent;
	double _scale_factor = 1.0;
	std::string _title;
	std::string _status;
	bool _active = false;

	faces_view_model_ptr _model;
	uint64_t _model_request = 0;
	bool _building = false;
	bool _build_again = false;
	bool _holding_rows = false;
	bool _rows_need_build = false;
	faces_view_model_ptr _pending_model;

	// The People pane.
	std::vector<row> _rows;
	// The selected people, the row the keyboard is on and where a Shift selection extends from, each kept by its
	// row's key.
	ui::list_selection<std::string> _people_selection;
	int _row_hover = -1;
	core::hash_set<std::string, core::ihash, core::ieq> _collapsed_surnames, _filtered_collapsed;
	view_scroller _groups_scroller;
	int _groups_content_height = 0;
	std::string _filter;
	recti _no_matches_bounds;
	std::shared_ptr<link_element> _clear_filter_link;

	// One selected row lists its faces; several list a representative preview of each instead.
	struct person_preview
	{
		int row;
		std::string detail;
		recti bounds;
	};
	std::vector<person_preview> _people_review;
	std::vector<cell> _cells;
	// Each face by what it shows, its picture and where in it: what the selection, the cursor and the anchor a Shift
	// selection extends from are kept by, so they survive the faces being listed again.
	std::vector<std::string> _cell_keys;
	std::vector<heading> _headings;
	ui::list_selection<std::string> _face_selection;
	std::string _faces_title;
	std::string _faces_detail;
	int _hover = -1;
	view_scroller _faces_scroller;
	int _faces_content_height = 0;
	struct face_scroll_anchor
	{
		struct portrait
		{
			std::string key;
			int y;
			bool suggested;
		};
		// A heading or a face that was in view, by its key.
		struct shown
		{
			std::string key;
			int y;
			bool heading;
		};
		std::optional<std::pair<std::string, int>> heading;
		std::vector<portrait> portraits;
		// Unknown and Hidden only lose faces while they are shown, so what was in view, top to bottom: the first still
		// listed stays where it was, or takes the place of the first, and the list closes up below it.
		std::vector<shown> in_view;
		int offset = 0;
		int range = 0;
	};
	std::optional<face_scroll_anchor> _pending_faces_anchor;
	bool _reveal_face = false;
	// One crop for a search or scrollbar bubble, even for a face outside the visible rows.
	faces_view_model::shown _preview_face;

	pane _pane = pane::groups;
	bool _keyboard_used = false;
	// What the view was asked to open on, applied once there are groups to show it among.
	std::string _open_person;
	std::optional<uint32_t> _open_face;
	// Generation of the face book used to build the displayed rows.
	uint64_t _book_generation = 0;

	// Layout, in the view's coordinates.
	int _groups_width = 0;
	std::optional<int> _drag_groups_width;
	int _title_height = 0;
	int _row_height = 0;
	int _tile = 104;
	int _line_height = 16;
	int _scroll_width = 16;
	// The faces pane's scroll bar: as wide as a plain one, or while it names the pane's sections as wide as their
	// names need, within limits.
	int _faces_rail_width = 16;
	recti _faces_title_bounds;
	// With face search off: what it does, and the button that turns it on, in the faces pane's content.
	recti _turn_on_text;
	recti _turn_on_bounds;

	// A drag of faces, from the press that started it.
	struct drag_state
	{
		bool active = false;
		pane from = pane::faces;
		std::vector<int> items;
		pointi loc;
		drop_target target;
		std::string label;
	} _drag;
	// Carries the People pane along while a drag is held near its top or bottom.
	ui::edge_scroll _drag_scroll;

	// A rubber band across the faces pane, from a press between faces, in the pane's content: what it touches is
	// selected when it is let go, or with Ctrl added to the selection or taken out of it.
	struct marquee_state
	{
		bool active = false;
		bool toggle = false;
		pointi start;
		pointi end;

		recti bounds() const { return recti(start, end).normalise(); }
	} _marquee;
	// Carries the faces pane along while a band is held near its top or bottom.
	ui::edge_scroll _band_scroll;

	int _heading_hover = -1;
	// The heading button under the pointer, by its heading and its place in the heading's buttons; -1 for none.
	std::pair<int, int> _heading_button_hover = {-1, -1};

	// Mirror edits until the next overview. An offer has an offer_key and no inbox row_key.
	struct moved_face
	{
		std::string row_key;
		std::string offer_key;
		float score = 0;
		faces::member_kind kind = faces::member_kind::unknown;
		uint64_t generation = 0;
	};
	core::hash_map<uint32_t, moved_face> _moved, _before_placement;
	// docs/faces.md#91-people: Unknown in the order it was shown, while it stays selected - each face's place in it, and
	// the run each was first shown in - so a new overview closes up what left rather than sorting the runs afresh.
	struct unknown_order
	{
		core::hash_map<uint32_t, uint32_t> place;
		core::hash_map<uint32_t, uint32_t> run;
		uint32_t next = 0;

		// Puts the faces shown before back in the order they were shown, those new to the list after them, and answers
		// each run's key.
		std::vector<std::string> arrange(std::vector<std::vector<uint32_t>>& runs, std::vector<uint32_t>& others,
		                                 std::vector<uint32_t>& unclear);
	};
	unknown_order _unknown_order;
	std::string _review_person;
	std::string _confirm_label;
	std::string _remove_label;
	std::string _selection_title;
	std::string _selection_detail;
	std::string _last_placement;
	std::string _undo_detail;
	std::optional<faces::book_edit> _placement_edit;
	bool _combining = false;
	struct placement_target
	{
		std::string label;
		std::string key;
		uint32_t person = 0;
		uint64_t generation = 0;
	} _destination;
	std::vector<std::pair<commands, ui::button_ptr>> _panel_commands;

	// Scan progress updates independently of the selection.
	bool _scan_photos = false;
	bool _scan_videos = false;
	std::weak_ptr<ui::check_control> _photos_check;
	std::weak_ptr<ui::check_control> _videos_check;
	std::shared_ptr<text_element> _progress_text;
	ui::button_ptr _pause_button;
	std::shared_ptr<text_element> _book_text;
	ui::button_ptr _delete_button;

	// Crops cut off the UI thread for the rows and faces in and near view, kept while they are near it.
	struct crop
	{
		core::const_surface_ptr surface;
		mutable render::texture_ptr texture;
		int requested_edge = 0;
	};

	core::hash_map<std::string, crop> _crops;
	uint64_t _crop_generation = 0;
	std::shared_ptr<face_crop_queue> _crop_queue;
	// The pictures hovering a face shows whole.
	core::hash_map<core::file_path, df::item_element_ptr, core::ihash, core::ieq> _pictures;
	core::file_path _bubble_picture;
	std::string _bubble_picture_key;
	int _bubble_picture_edge = 0;
	// In a video, the frame the bubble's face was found in, which the bubble shows rather than the whole video's.
	std::optional<uint32_t> _bubble_frame_ms;

	void request_model();
	void model_built(faces_view_model_ptr model, uint64_t request);
	void build_rows();
	void build_cells(std::optional<uint32_t> focus_face = {});
	void build_people_review(const std::vector<int>& rows);
	void layout_rows(render::measure_context* mc);
	void layout_cells(render::measure_context* mc);
	face_scroll_anchor capture_faces_anchor() const;
	int restore_faces_anchor(const face_scroll_anchor& anchor, int height) const;
	// The faces pane's sections - the faces before its first heading, then each heading's - as the scroll bar names
	// them: each run's name, and the width the bar needs to show them.
	std::vector<ui::scroller_run> faces_runs() const;
	int faces_rail_width(render::measure_context& mc) const;
	// What the faces pane's scroll bar shows where the pointer is over it: the face there and its section's name.
	void faces_scroll_popup(view_hover_element& hover, pointi loc);
	void add_crop_preview(view_hover_element& hover, const faces_view_model::shown& face);
	int crop_edge() const;
	void request_crops();
	void crops_cut(uint64_t generation, int edge, std::vector<std::pair<std::string, core::const_surface_ptr>> cut);
	static std::string crop_key(const faces_view_model::shown& s);
	void update_status();
	void update_selection();
	void update_selection_marks();
	std::string drag_label() const;
	moved_face placement_of(uint32_t face) const;
	void apply_placement(const std::vector<int>& cells, faces::face_action action, std::string_view name = {});
	void ask_to_place_all(int heading);
	// Asks for the name to give faces, offering the people they most likely are at a press: for the selection, or for
	// one suggestion from its shortcut.
	void name_cells(const std::vector<int>& cells, bool one_face);
	void changed(bool layout = true);

	int find_row(std::string_view key) const;
	bool is_row_selected(int row) const;
	// The row the keyboard is on, or -1.
	int focus_row() const;
	// Visible selectable people and inboxes; a surname heading is never a range member.
	std::vector<std::string> selectable_row_keys() const;
	// The selected people have changed: the faces pane lists theirs afresh.
	void show_selected_rows();
	const faces_view_model::shown* single_selected_face() const;
	const faces_view_model::shown* face_search_target() const;
	std::string person_search_target() const;
	void open_items(const df::search_t& search, const core::unique_paths& selection = {});
	void open_picture_in_folder(core::file_path path);
	faces::face_ref ref_of(const faces_view_model::shown& s) const;
	bool ask_name(std::string_view title, std::string_view info, std::string& name) const;
	std::vector<std::string> known_names() const;
	void rename_file_person(int row);
	bool can_rename_row(int row) const;
	// Whether the face book can be changed from this window: docs/faces.md#64-the-face-database.
	bool book_editable() const;
	bool offer_combine(std::string_view from, std::string_view to);
	bool can_drop_cell(int cell, int destination) const;
	bool can_drop(pane from, const std::vector<int>& items, const drop_target& target) const;

	// Holds the rows and faces as they are for as long as it lives. A command that asks the user something acts
	// afterwards on the positions it chose, and the window keeps running while it asks: a model made meanwhile would
	// build them again under it. Where the face book changed since the rows were built, the command is dropped.
	class row_hold final
	{
		faces_view& _view;
		bool _was_holding;
		uint64_t _generation;

	public:
		explicit row_hold(faces_view& view);
		~row_hold();
		row_hold(const row_hold&) = delete;
		row_hold& operator=(const row_hold&) = delete;

		bool book_unchanged(std::string_view command) const;
	};

	void names_planned(face_name_plan plan);
	// Writes names through the batch write, which keeps the change among the session's recent changes as what.
	void write_face_edits(face_edits_by_path by_path, std::string_view title, std::string what);

	pointi groups_offset() const;
	pointi faces_offset() const;
	recti groups_pane() const;
	recti faces_pane() const;
	recti groups_rail() const;
	recti splitter_bounds() const;
	int clamp_groups_width(int width) const;
	int row_at(pointi loc) const;
	recti expander_bounds(int index) const;
	int cell_at(pointi loc) const;
	int heading_at(pointi loc) const;
	// The faces listed under a heading, up to the next.
	std::vector<int> cells_under(int heading) const;
	std::vector<int> cells_in(recti content) const;
	// The faces a rubber band would leave selected, were it let go now.
	bool shown_selected(int cell, bool band_has_cells) const;
	drop_target target_at(pointi loc) const;
	void ensure_row_visible(int row);
	void ensure_visible(int cell);
	bool key_down_groups(char32_t key, platform::key_state keys);
	bool key_down_faces(char32_t key, platform::key_state keys);
	void draw_rows(render::draw_context& dc) const;
	void draw_cells(render::draw_context& dc) const;
	void draw_people_review(render::draw_context& dc) const;
	void draw_crop(render::draw_context& dc, const faces_view_model::shown& s, recti square, float alpha,
	               int radius) const;
	void draw_drag(render::draw_context& dc) const;

public:
	faces_view(view_state& state, view_host_ptr host);
	~faces_view() override;

	// Opens the view, on a named person if supplied.
	void open(std::string_view person = {});
	// Opens the face's person or inbox with the face selected.
	void open_at_face(uint32_t face);
	std::string_view filter_text() const { return _filter; }
	void filter_people(std::string_view text);
	std::string_view status_detail() const { return _selection_detail; }

	// The collection's names or face search's findings changed: what is shown is made again while it is on screen.
	void model_changed();
	// Face search or the face book reported: its progress and its switch are shown as they now are, the groups as the
	// book now has them, and what it found shown again once its overview or its store is new.
	void faces_changed();

	// The commands, each on what is selected: the faces where the faces pane was used last and has some selected, and
	// otherwise the groups. That selection is drawn in the selection's colour, the other pane's grey.
	bool faces_targeted() const;
	// The labels of the commands whose effect depends on what they act on.
	std::string_view rename_text() const;
	std::string_view confirm_text() const;
	std::string_view remove_text() const;
	void search_tooltip(view_hover_element& hover, commands id);
	bool can_rename() const;
	std::string placement_destination() const;
	void follow_placement();
	bool can_undo_placement() const;
	std::string_view undo_detail() const { return _undo_detail; }
	void undo_placement();
	bool can_hide() const;
	bool can_show_again() const;
	bool can_reject() const;
	bool can_name_faces() const;
	bool can_confirm() const;
	bool can_search_face() const;
	bool can_search_person() const;
	bool can_open_in_folder() const;
	bool can_save_names() const;
	void rename();
	void hide(bool hidden);
	void reject();
	void name_faces();
	void confirm();
	void search_face();
	void search_person();
	void open_in_folder();
	void save_names();
	void export_data();
	void import_data();
	void select_all();

	void rename_to(int row, std::string_view name);
	void move_cells_to_row(const std::vector<int>& cells, int row);
	// Moves faces to the person kept under a name, a new one where nobody is.
	void move_cells_to_name(const std::vector<int>& cells, std::string_view name);
	// What a heading's Confirm all does to the faces under it: for the button, and for tests.
	void place_all_under(int heading);
	// What a heading's button does when pressed: Confirm all asks first, Name all asks for the name, and Hide all hides
	// every face listed under the heading. For the button, and for tests.
	void press_heading_button(int heading, heading_action action);
	// What a drag dropped at a target does: for the drag, and for tests.
	void drop(pane from, const std::vector<int>& items, const drop_target& target);

	// What is shown and selected, for the commands, the agent tools and tests.
	const std::vector<row>& rows() const { return _rows; }
	const std::vector<cell>& cells() const { return _cells; }
	const std::vector<heading>& headings() const { return _headings; }
	bool reviewing_rows() const { return !_people_review.empty(); }
	const view_scroller& groups_scroller() const { return _groups_scroller; }
	const view_scroller& faces_scroller() const { return _faces_scroller; }
	bool is_cell_selected(int cell) const;
	// The face the keyboard is on, or -1.
	int focus_cell() const;
	std::vector<int> selected_rows() const;
	std::vector<int> selected_cells() const;
	std::string_view faces_title() const { return _faces_title; }
	bool surname_collapsed(std::string_view surname) const;
	void select_rows(const std::vector<int>& rows);
	void select_cells(const std::vector<int>& cells);
	// Where a row or a face is drawn, in the view's coordinates; empty while it is scrolled out of sight.
	recti row_bounds(int row) const;
	recti cell_bounds(int cell) const;
	// Where a heading's button is drawn, in the view's coordinates; empty while it is scrolled out of sight.
	recti heading_button_bounds(const heading_button& button) const;
	// What a heading's button says, and the icon beside it.
	std::string_view heading_action_text(heading_action action) const;
	static icon_index heading_action_icon(heading_action action);
	// A hovered suggestion's shortcuts: what each says in its bubble, and where it is drawn in the view's coordinates -
	// empty while scrolled out of sight. None for any other face, nor where the face book cannot be changed.
	struct suggestion_button
	{
		suggestion_action action = suggestion_action::confirm;
		recti bounds;
	};
	std::vector<suggestion_button> suggestion_buttons(int cell) const;
	std::string suggestion_action_text(suggestion_action action) const;
	const faces_view_model_ptr& model() const { return _model; }
	// Whether what is shown was made from the store, the overview and the face book face search has now.
	bool is_current() const;
	void model_arrived_for_tests(faces_view_model_ptr model)
	{
		_active = true;
		model_built(std::move(model), _model_request);
	}

	view_controls_host_ptr controls(const ui::frame_ptr& owner);

	// view_base
	void activate(sizei extent) override;
	void deactivate() override;
	void refresh() override;
	void layout(render::measure_context& mc, sizei extent) override;
	void render(render::draw_context& dc, view_controller_ptr controller) override;
	std::vector<keyboard_region> keyboard_regions() override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, hit_test_context& ctx) override;
	// A right-click on a face or a group selects it, where it is not selected already, and offers what can be done to
	// it; anywhere else, the view's own commands.
	view_menu context_menu(pointi loc) override;
	bool mouse_wheel(pointi loc, platform::wheel_notch notch) override;
	bool key_down(char32_t key, platform::key_state keys) override;
	bool can_scroll(pointi loc) const override;
	recti keyboard_bounds() const override;
	bool escape() override;
	void broadcast_event(const view_element_event& event) const override;
	void exit() override;
	std::string_view title() override;
	std::string_view status() override;
	view_bars bars() const override;

	// What the pointer does, from the controllers.
	void hover_row(int index);
	void hover_cell(int index);
	recti suggestion_button_bounds(int index, suggestion_action action) const;
	void review_suggestion(int index, const faces_view_model::shown& target, std::string_view person,
	                       suggestion_action action);
	void hover_heading(int index);
	void hover_heading_button(int heading, int button);
	// What a heading's button says in its bubble.
	void bubble_heading_button(int heading, int button, view_hover_element& hover) const;
	void click_row(int index, platform::key_state keys, bool on_title);
	void double_click_row(int index);
	void click_cell(int index, platform::key_state keys);
	void double_click_cell(int index);
	// A heading clicked selects the faces under it, or with Ctrl adds them.
	void click_heading(int index, platform::key_state keys);
	void click_faces_title();
	void begin_drag(pane from, int index, pointi loc);
	void drag_to(pointi loc);
	void hold_rows();
	void release_rows();
	void toggle_surname(int index);
	void end_drag(pointi loc);
	void cancel_drag();
	bool dragging() const { return _drag.active; }
	// A rubber band, as Items has, from a press between faces to where the pointer is, in the view's coordinates;
	// held near the top or bottom of the pane, it scrolls.
	void begin_marquee(pointi loc, bool toggle);
	void marquee_to(pointi loc);
	void end_marquee(pointi loc);
	void cancel_marquee();
	void marquee_scroll(pointi loc);
	bool marquee_active() const { return _marquee.active; }
	void bubble_row(int index, view_hover_element& hover);
	void bubble_cell(int index, view_hover_element& hover);
	void groups_width(int width);
	void finish_groups_resize(bool keep);
};
