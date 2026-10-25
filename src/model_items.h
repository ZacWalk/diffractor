// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Item representation and selection. Defines item_element for files/folders,
// item_set for collections, item_group for grouping, and thumbnail management.

#pragma once

#include "files.h"
#include "app_text.h"
#include "model_search.h"
#include "ui_elements.h"

class group_title_control;
class view_state;
class index_state;
class async_strategy;

enum class group_by
{
	file_type,
	shuffle,
	size,
	extension,
	location,
	rating_label,
	date_created,
	date_modified,
	camera,
	resolution,
	album_show,
	presence,
	folder,
	aspect_ratio,
	// Not a user choice: a related search always groups by how each item is related.
	related,
	// Appended rather than placed beside date_created: the order is persisted as an integer, so
	// inserting here would silently change what an existing setting means.
	date_original
};

// The three orders whose headers are days. Anything offered for one of them - the date direction,
// the reversal, the timeline - has to be offered for all three, or the order the upgrade migrates
// users onto is the one order missing the affordance.
constexpr bool is_date_group_order(const group_by order)
{
	return order == group_by::date_created || order == group_by::date_modified || order ==
		group_by::date_original;
}

enum class sort_by
{
	def,
	name,
	size,
	date_modified,
	date_created,
	date_original,
};

// Rolling a release back is ordinary, and the settings store is shared with whatever build the user
// goes back to. An order added after that build is a number it cannot name - its group and sort
// switches fall through to a default - so the long-standing key keeps a value every build
// understands and the true order is stored beside it. Original is the capture time, which is what an
// older build's "date created" resolved to, so it is the honest stand-in rather than a placeholder.
constexpr group_by group_order_older_builds_understand(const group_by order)
{
	return order == group_by::date_original ? group_by::date_created : order;
}

constexpr sort_by sort_order_older_builds_understand(const sort_by order)
{
	return order == sort_by::date_original ? sort_by::date_created : order;
}

// Which of the two stored values to believe. The plain key is the only one an older build writes, so
// when it no longer matches what our own stored order projects to, that build changed it while we
// were not running and it is the one to believe.
template <typename T, typename projection_t>
constexpr T resolve_stored_order(const uint32_t plain, const uint32_t extended, const bool has_extended,
                                 projection_t projection)
{
	if (!has_extended) return static_cast<T>(plain);

	const auto stored = static_cast<T>(extended);
	return static_cast<uint32_t>(projection(stored)) == plain ? stored : static_cast<T>(plain);
}

enum class aspect_ratio_bucket
{
	square,
	five_four,
	four_three,
	three_two,
	sixteen_ten,
	sixteen_nine,
	twenty_one_nine,
	other
};

struct aspect_ratio_group
{
	aspect_ratio_bucket bucket = aspect_ratio_bucket::other;
	bool is_portrait = false;
};

aspect_ratio_group calc_aspect_ratio_group(sizei dimensions);

enum class item_presence
{
	this_in = 1,
	similar_in = 2,
	newer_in = 3,
	older_in = 4,
	not_in = 5,
	unknown = 100
};

inline double calc_thumb_scale(const sized dimensions, const sized limit, const bool zoom = true)
{
	const double sx = limit.Width / dimensions.Width;
	const double sy = limit.Height / dimensions.Height;

	return zoom ? std::max(sx, sy) : std::min(sx, sy);
}

inline double calc_thumb_scale(const sizei dimensions, const sizei limit, const bool zoom = true)
{
	const double sx = limit.cx / static_cast<double>(dimensions.cx);
	const double sy = limit.cy / static_cast<double>(dimensions.cy);

	return zoom ? std::max(sx, sy) : std::min(sx, sy);
}


std::string format_invalid_name_message(std::string_view name);
std::string_view item_presence_text(item_presence v, bool long_text);
void parse_more_folders(df::index_roots& result, std::string_view more_folders);
void parse_more_folders(df::index_roots& result, std::string_view more_folders, const platform::drives& drives);

namespace df
{
	struct index_file_item;
	class file_group_histogram;
	struct index_folder_item;
	class item_element;
	class item_group;
	class item_set;

	struct item_row_draw_info;

	extern std::function<void(file_path source, file_path destination)> rename_after_move_for_test;
	extern std::function<void(std::string_view message)> rename_recovery_report_for_test;

	using item_element_ptr = std::shared_ptr<item_element>;
	using const_item_element_ptr = std::shared_ptr<const item_element>;
	using item_summary_ptr = std::shared_ptr<file_group_histogram>;
	using item_group_ptr = std::shared_ptr<item_group>;
	using weak_item_group_ptr = std::weak_ptr<item_group>;

	using item_groups = std::vector<item_group_ptr>;
	using item_elements = std::vector<item_element_ptr>;

	using unique_item_elements = hash_map<file_path, item_element_ptr, ihash, ieq>;

	using index_folder_item_ptr = std::shared_ptr<index_folder_item>;
	using index_folder_info_const_ptr = std::shared_ptr<const index_folder_item>;
	using index_item_info_map = hash_map<str::cached, index_file_item, ihash, ieq>;
	using index_item_infos = std::vector<index_file_item>;
	using index_folder_info_map = hash_map<folder_path, index_folder_item_ptr, ihash, ieq>;
	using index_folder_infos = std::vector<index_folder_item_ptr>;

	enum class item_group_display
	{
		icons,
		detail
	};

	enum class item_online_status
	{
		disk,
		offline
	};

	// Thumbnail progress for one item, UI-thread owned. See docs/implementation.md "Thumbnail
	// pipeline" for the stages, queues and hops these flags gate.
	//
	// These are one word rather than separate bools because they interlock: load_blocked is read as
	// a set, and a single flag left stuck strands the item on its file-type placeholder for the rest
	// of the session with no error anywhere - the historical failure mode in this area.
	//
	//   db_query_pending     Set at construction, cleared by begin_db_thumbnail_query when the item
	//                        first becomes visible. Blocks loading until SQLite has had its chance,
	//                        so a scan never regenerates a thumbnail the database already holds.
	//   loading              Claimed on the UI thread by make_scan_request BEFORE the scan batch is
	//                        queued, released by that batch's completion hop. De-duplicates
	//                        concurrent batches. A batch that is dropped instead of run leaks this
	//                        claim permanently, so the scan_displayed_items queue must never discard
	//                        a pending task; cancelled batches still run and still release.
	//   load_failed          The scan produced nothing; stops the item retrying every pass. Cleared
	//                        when a cloud placeholder is hydrated to local disk.
	//   shell_pending        A cloud (offline) thumbnail fetch is in flight. Cleared on every exit
	//                        path of the fetch, and by thumbnail() when any thumbnail arrives.
	//   shell_retry_pending  The provider returned a generic icon; items_view::retry_visible_
	//                        thumbnails re-arms it on a 1.5s cadence.
	//   staging_surface      An encoded image is being decoded to a surface on the render queue.
	//   staging_requested    A restage was asked for while one was running; coalesces to exactly one
	//                        follow-up, which re-reads the then-current image.
	//   invalidate_on_stage  A caller asked to be told when staging lands. Latched rather than held
	//                        as a callback so a request arriving mid-stage survives coalescing; every
	//                        path that stops staging must clear it and invalidate exactly once.
	//   surface_cached       The held surfaces are current for the held images. NOT "a surface
	//                        exists": a superseded surface is deliberately retained and drawn while
	//                        its replacement decodes, otherwise a scrubbed video blanks per frame.
	//   texture_is_cover_art Which of the two surfaces the cached GPU texture was built from.
	//   fade_pending         The next render should start the fade-in animation.
	//
	// Invariant: any code that replaces _thumbnail or _cover_art must also request staging in the
	// same UI hop. thumbnail() advances _thumbnail_surface_generation, so an in-flight stage will
	// discard its result, and it only reschedules itself when staging_requested was set.
	//
	// Measured on a 50k+ collection under sustained scrolling. df::thumbnail_perf aggregates this
	// pipeline and app_frame::final_exit writes one summary line at exit; re-measure before changing
	// anything below, because two of these findings contradict what the code shape suggests:
	//
	//   - 96% of visible-scan batches are superseded and 93% of requested scans are abandoned, with
	//     the queue reaching 121 batches deep. This is the design working, not a fault. A cancelled
	//     batch breaks on its first token check, so abandoning costs a queue hop and one UI
	//     completion pass - never a decode. Do NOT trade the loading claim for a self-expiring batch
	//     id on the strength of that 93%: it removes cheap work and reintroduces duplicate scans.
	//   - staging_requested coalesces roughly once per decode (12.4k vs 12.0k in one session), so the
	//     "already staging" branch is a hot path, not an edge case. It silently discarded its
	//     caller's redraw request until invalidate_on_stage replaced the callback parameter.
	//   - ~25% of surface decodes are discarded on generation mismatch. That is the largest real
	//     waste left here and the next thing worth attacking.
	enum class thumbnail_state : uint32_t
	{
		none = 0,
		db_query_pending = 1 << 0,
		loading = 1 << 1,
		load_failed = 1 << 2,
		shell_pending = 1 << 3,
		shell_retry_pending = 1 << 4,
		staging_surface = 1 << 5,
		staging_requested = 1 << 6,
		surface_cached = 1 << 7,
		texture_is_cover_art = 1 << 8,
		fade_pending = 1 << 9,
		invalidate_on_stage = 1 << 10,
		tooltip_on_stage = 1 << 11,

		// Any claim that means another path already owns producing this thumbnail.
		load_blocked = loading | load_failed | shell_pending | db_query_pending,
	};

	constexpr thumbnail_state operator|(thumbnail_state a, thumbnail_state b)
	{
		return static_cast<thumbnail_state>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	constexpr thumbnail_state operator&(thumbnail_state a, thumbnail_state b)
	{
		return static_cast<thumbnail_state>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
	}

	constexpr thumbnail_state operator~(thumbnail_state a)
	{
		return static_cast<thumbnail_state>(~static_cast<uint32_t>(a));
	}

	constexpr bool operator&&(thumbnail_state a, thumbnail_state b)
	{
		return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
	}

	// Cloud providers generate a thumbnail some time after upload, so a generic icon is retried on the
	// items_view cadence. Bounded because a provider that never produces one would retry forever.
	constexpr uint32_t max_shell_thumbnail_retries = 8;

	struct item_display_info
	{
		std::string info = {};

		str::cached name = {};
		str::cached title = {};
		str::cached folder = {};
		str::cached label = {};

		icon_index icon = icon_index::document;

		int rating = 0;
		int duplicates = 0;
		int sidecars = 0;
		int disk = 0;
		int track = 0;
		int duration = 0;
		int items = 0;

		str::cached bitrate = {};
		str::cached pixel_format = {};
		sizei dimensions = {};
		uint16_t audio_channels = 0;
		uint32_t audio_sample_rate = 0;
		uint16_t audio_sample_type = 0;

		file_size size = {};
		date_t created = {};
		// The file-creation concept, which is what Group by Created keys on. Kept beside `created`
		// rather than replacing it: a tile that showed one date under a header made from the other
		// is the same mismatch #184 reported.
		date_t file_created = {};
		date_t modified = {};

		item_online_status online_status = item_online_status::disk;
		ui::style::font_face title_font = ui::style::font_face::dialog;
		item_presence presence = item_presence::unknown;
	};

	// Issue #137 - the badge counts the copies the collection holds of this file, this one included,
	// so below two it reports "no other copy" - which the absence of a badge already says, and says
	// more clearly. Presence is still the gate: a count drawn while the check is running would read
	// as an answer it has not reached.
	constexpr bool can_show_duplicates(const item_display_info& info)
	{
		return info.presence != item_presence::unknown && info.duplicates > 1;
	}

	// How a copy claim was reached, strongest first. Two bits, because it is packed into
	// duplicate_info to keep that atomic lock-free (docs/collections.md section 7.1).
	enum class copy_grade : uint32_t
	{
		none = 0,
		identical = 1,
		same_file = 2,
		same_picture = 3,
	};

	// Strongest wins: none is weakest, and among the rest the lowest value is the strongest evidence.
	constexpr copy_grade strongest(const copy_grade left, const copy_grade right)
	{
		if (left == copy_grade::none) return right;
		if (right == copy_grade::none) return left;
		return left < right ? left : right;
	}

	struct duplicate_info
	{
		uint32_t group = 0;
		uint32_t count : 29 = 0;
		uint32_t same_picture_crowded : 1 = 0;
		// How this item joined its set, not how the set as a whole was reached.
		copy_grade grade : 2 = copy_grade::none;

		auto operator<=>(const duplicate_info&) const = default;
	};

	struct duplicate_info2 : duplicate_info
	{
		date_t group_modified;
		bool group_has_modifications = false;

		void record(const date_t modified, const uint32_t dup_group)
		{
			++count;

			if (group == 0)
			{
				group = dup_group;
			}

			if (modified.to_seconds() != group_modified.to_seconds())
			{
				group_has_modifications = group_modified.is_valid();

				if (group_modified < modified)
				{
					group_modified = modified;
				}
			}
		}
	};

	enum class index_item_flags : uint32_t
	{
		none = 0,
		is_read_only = 1 << 0,
		is_offline = 1 << 1,
		is_sidecar = 1 << 2,
	};

	constexpr index_item_flags operator|(const index_item_flags a, const index_item_flags b)
	{
		return static_cast<index_item_flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	constexpr index_item_flags operator&(const index_item_flags a, const index_item_flags b)
	{
		return static_cast<index_item_flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
	}

	constexpr bool operator&&(const index_item_flags a, const index_item_flags b)
	{
		return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
	}

	constexpr index_item_flags& operator|=(index_item_flags& a, const index_item_flags b)
	{
		a = static_cast<index_item_flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
		return a;
	}

	constexpr index_item_flags& operator&=(index_item_flags& a, const index_item_flags b)
	{
		a = static_cast<index_item_flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
		return a;
	}

	constexpr index_item_flags operator~(const index_item_flags a)
	{
		return static_cast<index_item_flags>(~static_cast<uint32_t>(a));
	}

	// The four quarter turns of one picture. A rotation cannot be recovered from a finished hash, so
	// the orientations are hashed together and kept together, and the set is replaced whole rather
	// than edited, which is what lets a reader hold one pointer and see a consistent picture.
	struct picture_hashes
	{
		crypto::phash_rotations rotations{};

		uint64_t stored() const { return rotations[0]; }
		bool is_usable() const { return crypto::phash_is_usable(rotations[0]); }
	};

	using picture_hashes_ptr = std::shared_ptr<const picture_hashes>;
	using picture_hashes_aptr = std::atomic<picture_hashes_ptr>;

	inline picture_hashes_ptr make_picture_hashes(const crypto::phash_rotations& rotations)
	{
		auto result = std::make_shared<picture_hashes>();
		result->rotations = rotations;
		return result;
	}

	struct index_file_item
	{
		// Declared widest-first. MSVC aligns atomic<shared_ptr> to 16 bytes, so leading with the
		// published pointers and trailing with the 32-bit members keeps the record compact.
		//
		// Published exactly like metadata: an immutable set replaced whole, never edited in place, so a
		// reader that holds the pointer holds four orientations of one picture. Null means not computed;
		// a set whose first entry is crypto::phash_declined means hashed and refused.
		mutable prop::item_metadata_aptr metadata;
		mutable df::picture_hashes_aptr phash;

		file_type_ref ft = nullptr;
		file_size size;
		date_t file_created;
		// Mutable and atomic for the same reason as metadata_scanned: a coherent write advances the
		// file's modified time while the folder's file list (index_folder_item::files) is const, so the
		// record has to be corrected in place rather than by rebuilding the node. Written on the work
		// queue by index_state::apply_scan_now and on the scan queue by folder enumeration; read from
		// the UI, database and search contexts.
		mutable std::atomic<date_t> file_modified;
		mutable std::atomic<date_t> metadata_scanned;
		mutable std::atomic<duplicate_info> duplicates;
		mutable std::atomic<uint32_t> exact_duplicate_group = 0;

		index_item_flags flags = index_item_flags::none;
		str::cached name;
		mutable std::atomic<search_presence_mask> search_presence;
		mutable std::atomic<uint32_t> crc32c = 0;

		index_file_item() = default;

		index_file_item(const index_file_item& other)
			: metadata(other.metadata.load()),
			  phash(other.phash.load()),
			  ft(other.ft),
			  size(other.size),
			  file_created(other.file_created),
			  file_modified(other.file_modified.load()),
			  metadata_scanned(other.metadata_scanned.load()),
			  duplicates(other.duplicates.load()),
			  exact_duplicate_group(other.exact_duplicate_group.load()),
			  flags(other.flags),
			  name(other.name),
			  search_presence(other.search_presence.load()),
			  crc32c(other.crc32c.load())
		{
		}

		index_file_item(index_file_item&& other) noexcept
			: metadata(other.metadata.load()),
			  phash(other.phash.load()),
			  ft(other.ft),
			  size(std::move(other.size)),
			  file_created(std::move(other.file_created)),
			  file_modified(other.file_modified.load()),
			  metadata_scanned(other.metadata_scanned.load()),
			  duplicates(other.duplicates.load()),
			  exact_duplicate_group(other.exact_duplicate_group.load()),
			  flags(other.flags),
			  name(std::move(other.name)),
			  search_presence(other.search_presence.load()),
			  crc32c(other.crc32c.load())
		{
			other.metadata.store(nullptr);
			other.phash.store(nullptr);
		}

		index_file_item& operator=(const index_file_item& other)
		{
			if (this == &other)
				return *this;
			flags = other.flags;
			ft = other.ft;
			name = other.name;
			size = other.size;
			file_created = other.file_created;
			file_modified = other.file_modified.load();
			metadata_scanned = other.metadata_scanned.load();
			metadata.store(other.metadata.load());
			search_presence = other.search_presence.load();
			duplicates = other.duplicates.load();
			exact_duplicate_group = other.exact_duplicate_group.load();
			crc32c = other.crc32c.load();
			phash = other.phash.load();
			return *this;
		}

		index_file_item& operator=(index_file_item&& other) noexcept
		{
			if (this == &other)
				return *this;
			flags = other.flags;
			ft = other.ft;
			name = std::move(other.name);
			size = std::move(other.size);
			file_created = std::move(other.file_created);
			file_modified = other.file_modified.load();
			metadata_scanned = other.metadata_scanned.load();
			metadata.store(other.metadata.load());
			other.metadata.store(nullptr);
			search_presence = other.search_presence.load();
			duplicates = other.duplicates.load();
			exact_duplicate_group = other.exact_duplicate_group.load();
			crc32c = other.crc32c.load();
			phash = other.phash.load();
			other.phash.store(nullptr);
			return *this;
		}

		date_t created() const
		{
			date_t d;
			const auto md = metadata.load();

			if (md)
			{
				d = md->created();
			}

			if (d.is_valid()) return d;

			// A metadata date is a wall clock and the index stamp is an instant. This is compared
			// against item_element::media_created(), which converts the same stamp.
			return file_created.is_valid() ? file_created.system_to_local() : date_t{};
		}

		item_online_status calc_online_status() const
		{
			return flags && index_item_flags::is_offline ? item_online_status::offline : item_online_status::disk;
		}

		void update_duplicates(const index_folder_item_ptr& f, duplicate_info dup_info) const;
		void calc_search_presence() const;

		prop::item_metadata_ptr safe_ps() const
		{
			auto result = metadata.load();

			if (!result)
			{
				// slight safety around the not returning an invalid result;
				result = std::make_shared<prop::item_metadata>();
				metadata.store(result);
			}

			return result;
		}

		str::cached xmp() const
		{
			const auto md = metadata.load();
			return md ? md->xmp : str::cached{};
		}

		bool operator<(const str::cached other) const
		{
			return icmp(name, other) < 0;
		}

		bool operator<(const std::string_view other) const
		{
			return icmp(name, other) < 0;
		}

		bool operator<(const index_file_item& other) const
		{
			return icmp(name, other.name) < 0;
		}

		bool operator==(const str::cached other) const
		{
			return icmp(name, other) == 0;
		}

		bool operator==(const std::string_view other) const
		{
			return icmp(name, other) == 0;
		}

		bool operator==(const index_file_item& other) const
		{
			return icmp(name, other.name) == 0;
		}
	};

	struct index_folder_item
	{
		// Declared widest-first so the flags and 32-bit members share one tail word instead of
		// leaving holes between the wide members; see index_file_item.
		const index_item_infos files;
		std::atomic<std::shared_ptr<const index_folder_infos>> child_folders =
			std::make_shared<const index_folder_infos>();

		date_t created = {};
		date_t modified = {};

		str::cached volume = {};
		str::cached name = {};

		// OR-summary of item presence masks. Missing bits reject the whole folder;
		// extra stale bits are safe because item masks and exact matching follow.
		std::atomic<search_presence_mask> search_presence_summary;
		std::atomic_uint64_t content_revision = 0;

		// is_in_collection / is_excluded are flipped by the indexing thread (index_folders)
		// while UI and database threads read them through shared node pointers. They are
		// atomic so those concurrent reads/writes are not a data race. is_read_only is set
		// once before the node is published, so it does not need to be atomic.
		std::atomic<bool> is_in_collection = false;
		std::atomic<bool> is_excluded = false;
		bool is_read_only = false;
		bool can_recurse = true;

		index_folder_item() = default;
		index_folder_item(const index_folder_item&) noexcept = delete;
		index_folder_item& operator=(const index_folder_item&) = delete;
		index_folder_item(index_folder_item&&) noexcept = delete;
		index_folder_item& operator=(index_folder_item&&) = delete;

		explicit index_folder_item(index_item_infos f, index_folder_infos g = {}) noexcept : files(std::move(f)),
			child_folders(std::make_shared<const index_folder_infos>(std::move(g)))
		{
			assert_true(files.size() == files.capacity());
			assert_true(child_folders.load()->size() == child_folders.load()->capacity());
		}

		std::shared_ptr<const index_folder_infos> folders_snapshot() const
		{
			return child_folders.load();
		}

		uint64_t revision_snapshot() const
		{
			return content_revision.load(std::memory_order_acquire);
		}

		void mark_content_changed()
		{
			content_revision.fetch_add(1, std::memory_order_release);
		}

		void set_content_revision(const uint64_t revision)
		{
			content_revision.store(revision, std::memory_order_release);
		}

		void replace_child(const str::cached folder_name, const index_folder_item_ptr& replacement)
		{
			auto existing = child_folders.load();

			for (;;)
			{
				auto updated = std::make_shared<index_folder_infos>(*existing);
				const auto found = std::lower_bound(updated->begin(), updated->end(), folder_name,
				                                    [](const index_folder_item_ptr& left, const std::string_view right)
				                                    {
					                                    return icmp(left->name, right) < 0;
				                                    });

				if (found == updated->end() || icmp((*found)->name, folder_name) != 0) return;
				*found = replacement;

				const std::shared_ptr<const index_folder_infos> published = std::move(updated);
				if (child_folders.compare_exchange_weak(existing, published)) return;
			}
		}

		void reset_search_presence()
		{
			// Folder reconstruction is the point where bits removed from items are cleared.
			search_presence_mask updated;

			for (const auto& f : files)
			{
				updated |= f.search_presence.load();
			}

			search_presence_summary = updated;
		}

		void update_search_presence(const index_file_item& file_node)
		{
			// Incremental updates only add bits. This lock-free monotonic summary can do
			// extra work until reset, but cannot hide an exact match.
			auto existing = search_presence_summary.load();
			search_presence_mask updated;

			do
			{
				updated = existing;
				updated |= file_node.search_presence.load();
			}
			while (!search_presence_summary.compare_exchange_weak(existing, updated));
		}

		void update_search_presence(const index_file_item& file_node, const search_presence_mask previous)
		{
			(void)previous;
			update_search_presence(file_node);
		}
	};


	struct location_heat_map
	{
		static constexpr uint32_t map_width = 256;
		static constexpr uint32_t map_height = 128;

		std::array<uint32_t, map_width * map_height> coordinates{};

		static pointi calc_map_loc(const gps_coordinate coord)
		{
			const auto x = round(coord.longitude() / 180.0 * (map_width / 2.0) + map_width / 2.0);
			const auto y = round(coord.latitude() / 90.0 * (map_height / 2.0) + map_height / 2.0);
			const auto xx = std::clamp(x, 0, static_cast<int>(map_width) - 1);
			const auto yy = static_cast<int>(map_height) - 1 - std::clamp(y, 0, static_cast<int>(map_height) - 1);
			return {xx, yy};
		}
	};

	struct date_counts
	{
		int modified = 0;
		int created = 0;
	};

	inline uint8_t thumbnail_representative_rank(const index_file_item& file)
	{
		const auto metadata = file.metadata.load();
		const auto visual_media = file.ft &&
			(file.ft->has_trait(file_traits::bitmap) || file.ft->has_trait(file_traits::video_metadata));
		if (!visual_media || !file.ft->has_trait(file_traits::thumbnail)) return 0;
		return metadata && metadata->rating >= 4 ? 2 : 1;
	}

	inline bool should_replace_thumbnail_representative(const file_path& candidate, const uint8_t candidate_rank,
	                                                    const file_path& current, const uint8_t current_rank)
	{
		return candidate_rank > 0 && (candidate_rank > current_rank ||
			(candidate_rank == current_rank && (current.is_empty() || candidate.icmp(current) < 0)));
	}

	// Maximum number of years the sidebar history chart can hold. The chart itself shows one window
	// of history_window_years at a time and a navigator to move that window; the storage is always
	// sized to this upper bound so changing what is shown never requires re-indexing. Covers
	// collections back to ~1900s.
	constexpr int max_history_years = 100;

	// Rows in the calendar, and the span a navigator selection covers. More than this and the grid
	// stops being something the eye can take in at once.
	constexpr int history_window_years = 8;

	// The span of years the navigator offers, which is not the span the collection literally covers.
	struct history_range
	{
		int first_year = 0;
		int last_year = 0;

		int year_count() const { return last_year - first_year + 1; }
		bool contains(const int year) const { return year >= first_year && year <= last_year; }
	};

	constexpr history_range history_range_from_start_year(const int start_year, const int current_year)
	{
		return {
			std::clamp(start_year, current_year - max_history_years + 1,
			           current_year - history_window_years + 1),
			current_year
		};
	}

	// Share of the collection the range must still hold once its oldest years are trimmed.
	constexpr int history_coverage_percent = 99;

	// An empty stretch at least this long, with no more than history_island_percent of the
	// collection beyond it, is treated as the edge of the real history.
	constexpr int history_gap_years = 6;
	constexpr int history_island_percent = 5;

	struct date_histogram;

	// The years worth offering, given what has been indexed. Photographs carry wrong dates - a
	// scanner that stamped 1900, a camera whose battery died and reset the clock - and a range
	// drawn to the oldest item would squeeze the decades the collection actually lives in into a
	// few pixels to make room for a handful of items that are not really there.
	history_range history_auto_range(const date_histogram& dates, int current_year);

	struct date_histogram
	{
		std::array<date_counts, 12 * max_history_years> dates{};
		std::array<file_path, 12 * max_history_years> representative_paths{};
		std::array<uint8_t, 12 * max_history_years> representative_ranks{};

		void record_representative(const size_t index, const index_file_item& file, const file_path& path)
		{
			const auto rank = thumbnail_representative_rank(file);
			if (should_replace_thumbnail_representative(
				path, rank, representative_paths[index], representative_ranks[index]))
			{
				representative_paths[index] = path;
				representative_ranks[index] = rank;
			}
		}
	};

	class file_group_histogram
	{
		uint8_t _representative_rank = 0;

	public:
		std::array<count_and_size, file_group::max_count> counts{};
		file_path representative_path;

		count_and_size total_items() const
		{
			count_and_size result;
			for (auto i = 1; i < file_group::max_count; i++)
			{
				result.size += counts[i].size;
				result.count += counts[i].count;
			}
			return result;
		}

		count_and_size total_folders() const
		{
			return counts[file_group::folder.id];
		}

		icon_index icon() const
		{
			auto largest = 0;

			for (auto i = 0; i < file_group::max_count; i++)
			{
				if (counts[i].count > counts[largest].count)
				{
					largest = i;
				}
			}

			return file_group_from_index(largest)->icon;
		}

		void record(const index_folder_info_const_ptr& folder)
		{
			counts[file_group::folder.id].count += 1;
		}

		void record(const index_file_item& info)
		{
			const auto ii = info.ft->group->id;
			counts[ii].count += 1;
			counts[ii].size += info.size;
		}

		void record(const index_file_item& info, const file_path& path)
		{
			record(info);
			const auto rank = thumbnail_representative_rank(info);
			if (should_replace_thumbnail_representative(path, rank, representative_path, _representative_rank))
			{
				representative_path = path;
				_representative_rank = rank;
			}
		}

		void record(const file_type_ref mt, const file_size& size)
		{
			const auto ii = mt->group->id;
			counts[ii].count += 1;
			counts[ii].size += size;
		}

		void add(const file_group_histogram& other)
		{
			for (auto i = 1; i < file_group::max_count; i++)
			{
				counts[i].size += other.counts[i].size;
				counts[i].count += other.counts[i].count;
			}
			if (should_replace_thumbnail_representative(other.representative_path, other._representative_rank,
			                                            representative_path, _representative_rank))
			{
				representative_path = other.representative_path;
				_representative_rank = other._representative_rank;
			}
		}

		friend bool operator==(const file_group_histogram& lhs, const file_group_histogram& rhs)
		{
			return lhs.counts == rhs.counts && lhs.representative_path == rhs.representative_path;
		}

		friend bool operator!=(const file_group_histogram& lhs, const file_group_histogram& rhs)
		{
			return !(lhs == rhs);
		}
	};

	class item_element final : public std::enable_shared_from_this<item_element>, public view_element
	{
	protected:
		// Declared widest-first so the flags and small enums share tail words instead of each
		// stranding padding between the pointer- and 8-byte-sized members.
		prop::item_metadata_const_ptr _metadata;
		index_folder_item_ptr _info;
		// Thumbnail rendering has four progressively more disposable representations:
		// metadata dimensions drive layout; encoded images survive while the item is alive;
		// CPU surfaces are staged only for the visible working set; and the GPU texture is
		// created lazily by render() on the UI thread. Resource cleanup drops the latter two,
		// then items_view restages visible items without rescanning their files or database rows.
		// Workers publish encoded images through queue_ui. Encoded images remain atomic only while
		// scan/database code still reads their status; decoded surfaces and textures are UI-owned.
		ui::const_image_ptr _thumbnail;
		ui::const_image_ptr _cover_art;
		mutable ui::texture_ptr _texture;
		mutable ui::const_surface_ptr _thumbnail_surface;
		mutable ui::const_surface_ptr _cover_art_surface;

		mutable recti _interactive_bounds;
		// Logical, unoffset. Empty unless the last render drew the pin badge for this item.
		mutable recti _pin_badge_bounds;
		search_result _search = {};

		// Invalidates a staging result if thumbnail() or resource cleanup replaced its inputs
		// while image_to_surface() was running on the render worker.
		mutable uint64_t _thumbnail_surface_generation = 0;
		uint64_t _thumbnail_request_generation = 0;
		uint64_t _thumbnail_load_generation = 0;
		uint64_t _total_count = 0;
		double _media_position = 0.0;

		file_type_ref _ft = file_type::other;
		file_size _size = {};
		date_t _thumbnail_timestamp = {};
		date_t _modified = {};
		date_t _created = {};
		date_t _media_created = {};
		duplicate_info _duplicates = {};
		mutable ui::animate_alpha _thumbnail_alpha{1.0f};

		file_path _path = {};
		// The intrinsic media size that decides this item's tile geometry. Seeded from indexed
		// metadata, which is stable for the life of the file, and only falls back to a decoded
		// image's pixel size while no metadata dimensions are known.
		sizei _layout_dims = {};
		str::cached _name = {};
		int _random = 0;
		uint32_t _crc32c = 0;
		uint32_t _shell_retry_count = 0;
		mutable thumbnail_state _thumbnail_state = thumbnail_state::db_query_pending;
		item_presence _presence = item_presence::unknown;
		item_online_status _online_status = item_online_status::offline;

		ui::orientation _layout_orientation = ui::orientation::top_left;
		// Armed before a write that cannot change what is drawn, consumed by the update that publishes
		// the modified time that write produced. UI thread only.
		bool _retain_thumbnail_on_modify = false;
		bool _media_position_changed = false;
		bool _layout_aspect_known = false;
		bool _is_visible = false;
		bool _is_read_only = true;
		bool _is_folder = false;

		void set_thumbnail_state(const thumbnail_state s, const bool on) const
		{
			_thumbnail_state = on ? (_thumbnail_state | s) : (_thumbnail_state & ~s);
		}

	public:
		bool alt_background = false;
		bool row_layout_valid = true;

		item_element(const file_path id, const index_file_item& info) noexcept : view_element(
				view_element_style::can_invoke), _random(rand())
		{
			update(id, info);
		}

		item_element(const folder_path path, index_folder_item_ptr info) noexcept : _info(std::move(info)), _path(path)
		{
			_is_read_only = _info->is_read_only;
			_name = path.name();
			_ft = file_type::folder;
			_modified = _info->modified;
			_created = _info->created;
			_is_folder = true;
		}

		// Returns whether anything the layout depends on changed, so the caller can ask for the layout
		// pass rather than leaving the tile at its previous geometry.
		bool update(file_path path, const index_file_item& info) noexcept;

		// Shapes the tile from the image it actually draws. Cover art wins because that is what is
		// shown; the media's own indexed size is next; a thumbnail is only a guess until one of those
		// arrives, and never earns the justification that a known aspect does.
		void refresh_layout_dims();


		item_element(const item_element& other) = delete;
		item_element(item_element&& other) = delete;
		item_element& operator=(const item_element& other) = delete;
		item_element& operator=(item_element&& other) = delete;

		void duplicates(const duplicate_info d)
		{
			assert_true(ui::is_ui_thread());
			_duplicates = d;
		}

		duplicate_info duplicates() const
		{
			assert_true(ui::is_ui_thread());
			return _duplicates;
		}

		date_t thumbnail_timestamp() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_timestamp;
		}

		// Declares that the pending write leaves the decoded image alone, so the new modified time it
		// produces must not send a thumbnail that is still correct back through a reload.
		void retain_thumbnail_across_next_write()
		{
			assert_true(ui::is_ui_thread());
			_retain_thumbnail_on_modify = true;
		}

		str::cached name() const { return _name; }
		file_type_ref file_type() const { return _ft; }

		std::string_view extension() const
		{
			if (is_folder()) return {};
			const std::string_view sv = _name;
			const auto ext = find_ext(sv);
			return sv.substr(ext);
		}

		bool is_media() const
		{
			return _ft->is_media();
		}

		bool is_link() const
		{
			return ends(_name, ".lnk");
		}

		bool has_title() const
		{
			assert_true(ui::is_ui_thread());
			const auto& m = _metadata;
			return m && !is_empty(m->title);
		}

		str::cached title() const
		{
			assert_true(ui::is_ui_thread());
			const auto& m = _metadata;
			return !m || is_empty(m->title) ? _name : m->title;
		}

		int rating() const
		{
			assert_true(ui::is_ui_thread());
			const auto& md = _metadata;
			return md ? md->rating : 0;
		}

		std::string_view label() const
		{
			assert_true(ui::is_ui_thread());
			const auto& md = _metadata;
			return md ? md->label : std::string_view{};
		}

		bool has_gps() const
		{
			assert_true(ui::is_ui_thread());
			const auto& md = _metadata;
			return md && md->has_gps();
		}

		uint32_t crc32c() const
		{
			assert_true(ui::is_ui_thread());
			return _crc32c;
		}

		void crc32c(const uint32_t crc)
		{
			assert_true(ui::is_ui_thread());
			_crc32c = crc;
		}

		item_display_info populate_info() const;

		bool has_thumb() const
		{
			assert_true(ui::is_ui_thread());
			return is_valid(_thumbnail);
		}

		bool has_cover_art() const
		{
			assert_true(ui::is_ui_thread());
			return is_valid(_cover_art);
		}

		size_t thumbnail_blob_bytes() const
		{
			assert_true(ui::is_ui_thread());
			size_t bytes = 0;
			if (is_valid(_thumbnail)) bytes += _thumbnail->data().size();
			if (is_valid(_cover_art)) bytes += _cover_art->data().size();
			return bytes;
		}

		// Gives up the encoded thumbnail so it can be reloaded from the database when the item returns to
		// view; re-arming the query is what makes that the cheap path rather than a fresh file scan.
		// Layout dimensions are deliberately left as they are: the same thumbnail comes back, so
		// recomputing them here would reflow the row for nothing. Returns the bytes released.
		size_t clear_thumbnail_blob()
		{
			assert_true(ui::is_ui_thread());

			if (_is_visible || is_selected() || _is_folder || is_loading_thumbnail()) return 0;

			const auto bytes = thumbnail_blob_bytes();
			if (bytes == 0) return 0;

			++_thumbnail_surface_generation;
			_thumbnail.reset();
			_cover_art.reset();
			_texture.reset();
			_thumbnail_surface.reset();
			_cover_art_surface.reset();
			set_thumbnail_state(thumbnail_state::surface_cached, false);
			set_thumbnail_state(thumbnail_state::db_query_pending, true);

			return bytes;
		}

		ui::const_image_ptr thumbnail() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail;
		}

		ui::const_image_ptr cover_art() const
		{
			assert_true(ui::is_ui_thread());
			return _cover_art;
		}

		// Decoded by the render worker and published through queue_ui, so a caller that wants pixels
		// reads this rather than decoding the encoded blob again. Empty until it has been staged, and
		// empty again after resource cleanup until the visible-items pass restages it.
		ui::const_surface_ptr cover_art_surface() const
		{
			assert_true(ui::is_ui_thread());
			return _cover_art_surface;
		}

		bool is_selected() const
		{
			return is_style_bit_set(view_element_style::selected);
		}

		void select(const bool s, const view_host_base_ptr& view, const view_element_ptr& e)
		{
			set_style_bit(view_element_style::selected, s, view, e);
			is_error(s && is_error(), view, e);
		}

		bool is_error() const
		{
			return is_style_bit_set(view_element_style::error);
		}

		void is_error(const bool val, const view_host_base_ptr& view, const view_element_ptr& e)
		{
			set_style_bit(view_element_style::error, val, view, e);
		}

		void invert_selection(const view_host_base_ptr& view, const view_element_ptr& e)
		{
			set_style_bit(view_element_style::selected, !is_selected(), view, e);
			is_error(is_selected() && is_error(), view, e);
		}

		search_result search() const
		{
			return _search;
		}

		void search(const search_result& r)
		{
			_search = r;
		}

		index_folder_info_const_ptr info() const
		{
			return _info;
		}

		void info(index_folder_item_ptr info)
		{
			assert_true(ui::is_ui_thread());
			assert_true(is_folder());
			_info = std::move(info);
			_is_read_only = _info->is_read_only;
		}

		void update_folder(index_folder_item_ptr info, const count_and_size total)
		{
			assert_true(ui::is_ui_thread());
			assert_true(is_folder());
			_info = std::move(info);
			_is_read_only = _info->is_read_only;
			_size = total.size;
			_total_count = total.count;
			row_layout_valid = false;
		}

		sizei layout_dims() const
		{
			assert_true(ui::is_ui_thread());
			return _layout_dims;
		}

		ui::orientation layout_orientation() const
		{
			assert_true(ui::is_ui_thread());
			return _layout_orientation;
		}

		// True once the tile's aspect is the one it will keep - taken from the image the tile draws or
		// from the indexed media size, rather than guessed. That is what makes it safe to size the tile
		// from the row's solved height instead of holding a nominal width.
		bool layout_aspect_known() const
		{
			assert_true(ui::is_ui_thread());
			return _layout_aspect_known;
		}

		void thumbnail(ui::const_image_ptr i, ui::const_image_ptr ca, const date_t timestamp = date_t::null,
		               const bool fade_in = false)
		{
			assert_true(ui::is_ui_thread());
			++_thumbnail_request_generation;
			set_thumbnail_state(thumbnail_state::shell_pending, false);
			_thumbnail_timestamp = timestamp;
			const auto had_visual = is_valid(_thumbnail) || is_valid(_cover_art);
			const auto has_visual = is_valid(i) || is_valid(ca);

			_thumbnail = std::move(i);
			_cover_art = std::move(ca);
			refresh_layout_dims();

			set_thumbnail_state(thumbnail_state::fade_pending,
			                    fade_in && !had_visual && has_visual && _is_visible);
			row_layout_valid = false;

			// Invalidate an in-flight staging result, but keep the staged surface on screen until the
			// replacement is staged - discarding it here blanked a scrubbed video between frames.
			++_thumbnail_surface_generation;
			set_thumbnail_state(thumbnail_state::surface_cached, false);
		}

		uint64_t begin_thumbnail_load()
		{
			assert_true(ui::is_ui_thread());
			set_thumbnail_state(thumbnail_state::loading, true);
			_thumbnail_load_generation = ++_thumbnail_request_generation;
			return _thumbnail_load_generation;
		}

		uint64_t begin_thumbnail_request()
		{
			assert_true(ui::is_ui_thread());
			set_thumbnail_state(thumbnail_state::shell_pending, true);
			return ++_thumbnail_request_generation;
		}

		bool is_current_thumbnail_request(const uint64_t generation) const
		{
			assert_true(ui::is_ui_thread());
			return generation == _thumbnail_request_generation;
		}

		bool is_current_thumbnail_load(const uint64_t generation) const
		{
			assert_true(ui::is_ui_thread());
			return generation == _thumbnail_load_generation;
		}

		uint64_t thumbnail_request_generation() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_request_generation;
		}

		void clear_cached_surface() const
		{
			assert_true(ui::is_ui_thread());

			// Not surface_cached: a surface awaiting replacement is still held but no longer current.
			if (!_texture && !_thumbnail_surface && !_cover_art_surface &&
				!(_thumbnail_state && thumbnail_state::staging_surface))
			{
				return;
			}

			// Advance first so an in-flight decode cannot repopulate a cache being discarded.
			++_thumbnail_surface_generation;
			_texture.reset();
			_thumbnail_surface.reset();
			_cover_art_surface.reset();
			set_thumbnail_state(thumbnail_state::surface_cached, false);
		}

		bool has_cached_surface() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_state && thumbnail_state::surface_cached;
		}

		bool is_staging_thumbnail_surface() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_state && thumbnail_state::staging_surface;
		}

		// The decoded thumbnail, where the browser has already staged one. Handed to the display so it can
		// show something the moment an item is selected instead of decoding the same pixels again.
		const ui::const_surface_ptr& thumbnail_surface() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_surface;
		}

		void stage_thumbnail_surface(async_strategy& async, bool invalidate_on_complete = false,
		                             bool tooltip_on_complete = false) const;
		void start_thumbnail_animation(view_state& state) const;

		void render_bg(ui::draw_context& dc, const item_group& group, pointi element_offset) const;
		void render(ui::draw_context& dc, const item_group& group, pointi element_offset) const;

		recti interactive_bounds() const
		{
			return _interactive_bounds;
		}

		recti pin_badge_bounds() const
		{
			return _pin_badge_bounds;
		}

		sizei measure(ui::measure_context& mc, int width_limit) const override;
		void layout(ui::measure_context& mc, recti bounds_in, ui::control_layouts& positions) override;
		view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
		                                             hit_test_context& ctx) override;

		platform::file_op_result rename(index_state& index, std::string_view name);

		std::string base_name() const
		{
			if (is_folder()) return std::string(_name);
			return std::string(path().file_name_without_extension());
		}

		void add_to(item_set& results);
		void add_to(paths& paths);
		void add_to(unique_paths& paths);
		void open(view_state& s, const view_host_base_ptr& view) const;

		search_t containing() const
		{
			if (is_folder()) return search_t().add_selector(_path.folder().parent());
			return search_t().add_selector(_path.folder());
		}

		bool is_read_only() const
		{
			assert_true(ui::is_ui_thread());
			return _is_read_only;
		}

		bool is_folder() const
		{
			return _is_folder;
		}

		bool is_in_collection() const
		{
			return _info && _info->is_in_collection;
		}

		prop::item_metadata_const_ptr metadata() const
		{
			assert_true(ui::is_ui_thread());
			return _metadata;
		}

		file_size file_size() const
		{
			return _size;
		}

		date_t file_created() const
		{
			assert_true(ui::is_ui_thread());
			return _created;
		}

		date_t file_modified() const
		{
			assert_true(ui::is_ui_thread());
			return _modified;
		}

		date_t media_created() const
		{
			assert_true(ui::is_ui_thread());
			return _media_created;
		}

		date_t calc_media_created() const
		{
			assert_true(ui::is_ui_thread());
			const auto& md = _metadata;

			if (!md)
			{
				return _created.system_to_local();
			}

			auto d = md->created();

			if (!d.is_valid())
			{
				d = _created.system_to_local();
			}

			return d;
		}

		// When this file came to be, as distinct from when its content was made. Metadata answers
		// it when the file carries a creation tag; the filesystem is the fallback, not the source.
		date_t file_or_metadata_created() const
		{
			assert_true(ui::is_ui_thread());
			const auto& md = _metadata;
			const auto d = md ? md->dates.created() : date_t::null;
			return d.is_valid() ? d : _created.system_to_local();
		}

		str::cached sidecars() const
		{
			assert_true(ui::is_ui_thread());
			const auto& md = _metadata;
			return md ? md->sidecars : str::cached{};
		}

		str::cached xmp() const
		{
			assert_true(ui::is_ui_thread());
			const auto& md = _metadata;
			return md ? md->xmp : str::cached{};
		}

		size_t sidecars_count() const
		{
			return split_count(sidecars(), true);
		}

		double media_position() const
		{
			assert_true(ui::is_ui_thread());
			return _media_position;
		}

		void media_position(const double d)
		{
			assert_true(ui::is_ui_thread());
			_media_position = d;
			_media_position_changed = true;
		}

		bool should_load_thumbnail() const
		{
			assert_true(ui::is_ui_thread());
			if (is_folder())
				return false;

			if (!_ft->has_trait(file_traits::bitmap) && !_ft->has_trait(file_traits::av))
				return false;

			// Cloud-only placeholders have no local thumbnail; attempting to load one would hit
			// the shell every session for every visible item. Skip until the file is hydrated
			// (its online status becomes 'disk'), at which point it is scanned normally.
			if (_online_status == item_online_status::offline)
				return false;

			if (_thumbnail_state && thumbnail_state::load_blocked)
				return false;

			if (is_empty(_thumbnail))
				return true;

			if (_thumbnail_timestamp != _modified)
				return true;

			return false;
		}

		void add_if_thumbnail_load_needed(item_elements& items)
		{
			if (should_load_thumbnail())
			{
				items.emplace_back(shared_from_this());
			}
		}

		// A cloud-only placeholder has no local thumbnail from the normal (hydrating) scan, but the
		// shell can supply the cloud provider's thumbnail on-demand WITHOUT downloading the file.
		// This is requested only for items the user is actually viewing (visible in the items view),
		// and only once the db-thumbnail query has run (db_query_pending) so a previously cached
		// shell thumbnail is reused instead of re-fetched from the shell/network.
		bool should_load_shell_thumbnail() const
		{
			assert_true(ui::is_ui_thread());
			if (is_folder())
				return false;

			if (_online_status != item_online_status::offline)
				return false;

			if (!_ft->has_trait(file_traits::bitmap) && !_ft->has_trait(file_traits::av))
				return false;

			if (_thumbnail_state && (thumbnail_state::load_blocked | thumbnail_state::shell_retry_pending))
				return false;

			return is_empty(_thumbnail);
		}

		void add_if_shell_thumbnail_needed(item_elements& items)
		{
			if (should_load_shell_thumbnail())
			{
				items.emplace_back(shared_from_this());
			}
		}

		bool shell_thumbnail_pending() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_state && thumbnail_state::shell_pending;
		}

		void shell_thumbnail_pending(const bool v)
		{
			assert_true(ui::is_ui_thread());
			set_thumbnail_state(thumbnail_state::shell_pending, v);
		}

		// A cloud thumbnail was requested but only a generic icon was available (the provider had not
		// generated the real thumbnail yet). The item is left waiting so view_state::tick can retry it
		// periodically until the real thumbnail becomes available.
		bool shell_thumbnail_retry_pending() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_state && thumbnail_state::shell_retry_pending;
		}

		void shell_thumbnail_retry_pending(const bool v, const bool counted = true)
		{
			assert_true(ui::is_ui_thread());

			// Bounded: a provider that never produces a thumbnail (common for video) returns its generic
			// icon forever, which would otherwise re-fetch over the network every retry tick for the whole
			// session. After the last attempt the item keeps its file-type placeholder instead. Only real
			// provider responses are counted; re-arming an abandoned batch does not spend an attempt.
			if (v && counted && ++_shell_retry_count > max_shell_thumbnail_retries)
			{
				set_thumbnail_state(thumbnail_state::shell_retry_pending, false);
				set_thumbnail_state(thumbnail_state::load_failed, true);
				return;
			}

			set_thumbnail_state(thumbnail_state::shell_retry_pending, v);
		}

		// Maintained by items_view::update_visible_items_list: true while the item is within the
		// (extended) visible viewport. The cloud-thumbnail fetcher reads it to abandon items that have
		// scrolled out of view since a batch was queued, so it tracks the visible set rather than
		// grinding through a stale screenful.
		bool is_visible() const
		{
			assert_true(ui::is_ui_thread());
			return _is_visible;
		}

		void is_visible(const bool v)
		{
			assert_true(ui::is_ui_thread());
			_is_visible = v;
		}

		bool is_loading_thumbnail() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_state && thumbnail_state::loading;
		}

		void is_loading_thumbnail(const bool v)
		{
			assert_true(ui::is_ui_thread());
			set_thumbnail_state(thumbnail_state::loading, v);
		}

		bool failed_loading_thumbnail() const
		{
			assert_true(ui::is_ui_thread());
			return _thumbnail_state && thumbnail_state::load_failed;
		}

		void failed_loading_thumbnail(const bool v)
		{
			assert_true(ui::is_ui_thread());
			set_thumbnail_state(thumbnail_state::load_failed, v);
		}

		item_online_status online_status() const
		{
			assert_true(ui::is_ui_thread());
			return _online_status;
		}

		int random() const
		{
			return _random;
		}

		void random(const int i)
		{
			_random = i;
		}

		void presence(const item_presence presence)
		{
			assert_true(ui::is_ui_thread());
			_presence = presence;
		}

		item_presence presence() const
		{
			assert_true(ui::is_ui_thread());
			return _presence;
		}

		file_path path() const
		{
			return _path;
		}

		folder_path folder() const
		{
			return _path.folder();
		}

		uint64_t begin_db_thumbnail_query()
		{
			assert_true(ui::is_ui_thread());
			if (!(_thumbnail_state && thumbnail_state::db_query_pending)) return 0;
			set_thumbnail_state(thumbnail_state::db_query_pending, false);
			return ++_thumbnail_request_generation;
		}
	};

	struct unique_items
	{
		unique_item_elements _items;

		item_element_ptr find(const folder_path path_in) const
		{
			const file_path search_path(path_in);
			const auto found = _items.find(search_path);
			return found != _items.end() ? found->second : nullptr;
		}

		item_element_ptr find(const file_path id) const
		{
			const auto found = _items.find(id);
			return found != _items.end() ? found->second : nullptr;
		}
	};

	enum class process_items_type
	{
		photos_only,
		can_save_pixels,
		can_save_metadata,
		local_file,
		local_file_or_folder
	};

	enum class process_result_code
	{
		ok,
		nothing_selected,
		cloud_item,
		read_only,
		not_photo,
		cannot_embed_xmp,
		cannot_save_pixels,
		cannot_edit,
		folder
	};

	struct process_result
	{
		process_result_code code = process_result_code::ok;
		size_t items_count = 0;
		str::cached first_file_name;
		std::string first_file_extension;

		std::string to_string() const
		{
			std::string result;

			if (code == process_result_code::nothing_selected)
			{
				result = tt.no_items_are_selected;
			}
			else
			{
				result = format_plural_text(tt.cannot_process_fmt, first_file_name, static_cast<int>(items_count), {});
				result += " ";

				if (code == process_result_code::cloud_item)
				{
					result += tt.not_supported_cloud;
				}
				else if (code == process_result_code::read_only)
				{
					result += tt.not_supported_readonly;
				}
				else if (code == process_result_code::cannot_embed_xmp)
				{
					result += tt.not_supported_readonly_metadata;
				}
				else if (code == process_result_code::not_photo)
				{
					result += tt.not_supported_photo;
				}
				else if (code == process_result_code::cannot_save_pixels)
				{
					result += tt.not_supported_save_format;
				}
				else if (code == process_result_code::cannot_edit)
				{
					result += str_format(tt.cannot_edit_fmt.sv(), first_file_extension);
				}
				else if (code == process_result_code::folder)
				{
					result += tt.not_supported_folder;
				}
			}

			return result;
		}

		void record_error(const item_element_ptr& i, const process_result_code result_code, const bool mark_errors,
		                  const view_host_base_ptr& view)
		{
			if (code == process_result_code::ok || code == result_code)
			{
				code = result_code;
				if (is_empty(first_file_name))
				{
					first_file_name = i->name();
					first_file_extension = i->extension();
				}
				if (mark_errors) i->is_error(true, view, i);
				items_count += 1;
			}
		}

		bool fail() const
		{
			return code != process_result_code::ok;
		}

		bool success() const
		{
			return code == process_result_code::ok;
		}
	};

	// Keeps the encoded thumbnails the user is most likely to scroll back to and releases the rest.
	// Distance from the viewport, not age: a retained blob is a texture upload away from the screen
	// while an evicted one is a database round trip, and scrolling is what decides which is which.
	// Items inside the viewport are never released. Returns the bytes released.
	size_t trim_thumbnail_blobs(const item_elements& items, recti viewport, size_t budget_bytes);

	class item_set_info
	{
	public:
		int duplicates = 0;
		int sidecars = 0;
		int untagged = 0;
		int unlocated = 0;
		int unrated = 0;
		int uncredited = 0;
		int rejected = 0;
	};

	class item_set
	{
	public:
		item_elements _items;

		item_set() noexcept = default;

		item_set(item_elements items) : _items(std::move(items))
		{
		}

		item_set(const item_set&) = default;
		item_set& operator=(const item_set&) = default;
		item_set(item_set&&) noexcept = default;
		item_set& operator=(item_set&&) noexcept = default;

		void clear()
		{
			_items.clear();
		}

		bool is_empty() const
		{
			return _items.empty();
		}

		bool empty() const
		{
			return _items.empty();
		}

		size_t size() const
		{
			return _items.size();
		}

		friend bool operator==(const item_set& lhs, const item_set& rhs)
		{
			return lhs._items == rhs._items;
		}

		friend bool operator!=(const item_set& lhs, const item_set& rhs)
		{
			return !(lhs == rhs);
		}

		void shuffle() const
		{
			for (const auto& i : _items)
			{
				i->random(rand());
			}
		}

		void for_all(const std::function<void(const item_element_ptr&)>& f) const
		{
			for (const auto& i : _items) f(i);
		}

		bool has_errors() const
		{
			for (const auto& i : _items) if (i->is_error()) return true;
			return false;
		}

		void append(const item_set& other)
		{
			_items.insert(_items.end(), other._items.begin(), other._items.end());
		}

		bool single_file_extension() const
		{
			if (has_folders()) return false;
			if (_items.empty()) return false;

			const auto ext = _items[0]->extension();

			for (const auto& i : _items)
			{
				if (str::icmp(ext, i->extension()) != 0)
				{
					return false;
				}
			}

			return true;
		}

		void add(const item_element_ptr& i) { _items.emplace_back(i); }
		void reserve(const size_t count) { _items.reserve(count); }

		file_group_histogram summary() const;
		item_set_info info() const;

		const item_elements& items() const
		{
			return _items;
		}

		paths ids() const
		{
			paths result;
			for (const auto& i : _items)
			{
				if (i->is_folder())
				{
					result.folders.emplace_back(i->folder());
				}
				else
				{
					result.files.emplace_back(i->path());
				}
			}
			return result;
		}

		bool has_folders() const
		{
			for (const auto& i : _items)
			{
				if (i->is_folder())
					return true;
			}
			return false;
		}

		file_type_ref group_file_type() const
		{
			file_type_ref ft = file_type::other;

			for (const auto& i : _items)
			{
				if (i->file_type() != ft)
				{
					if (ft == file_type::other)
					{
						ft = i->file_type();
					}
					else
					{
						return file_type::other;
					}
				}
			}

			return ft;
		}

		std::vector<ui::const_image_ptr> thumbs(size_t max = max_thumbnails_to_display,
		                                        const item_element_ptr& skip_this = nullptr) const;
		std::vector<ui::const_surface_ptr> thumbnail_surfaces(size_t max = max_thumbnails_to_display,
		                                                      const item_element_ptr& skip_this = nullptr) const;
		void stage_thumbnail_surfaces(async_strategy& async, size_t max = max_thumbnails_to_display,
		                              const item_element_ptr& skip_this = nullptr) const;

		process_result can_process(process_items_type file_types, bool mark_errors,
		                           const view_host_base_ptr& view) const;

		item_set selected() const
		{
			item_set result;
			for (const auto& i : _items) if (i->is_selected()) result._items.emplace_back(i);
			return result;
		}

		str::cached first_name() const
		{
			for (const auto& i : _items) return i->name();
			return {};
		}

		item_element_ptr find(const std::string_view s) const
		{
			for (const auto& i : _items) if (icmp(i->name(), s) == 0 || icmp(i->path().name(), s) == 0) return i;
			return nullptr;
		}

		item_element_ptr find(const file_path id) const
		{
			for (const auto& i : _items) if (i->path() == id) return i;
			return nullptr;
		}

		item_element_ptr find(const folder_path id) const
		{
			const file_path search_path(id);
			for (const auto& i : _items) if (i->path() == search_path) return i;
			return nullptr;
		}

		item_element_ptr find(const item_element_ptr& f) const
		{
			for (const auto& i : _items) if (i == f) return i;
			return nullptr;
		}

		item_element_ptr find_file(const item_element_ptr& f) const
		{
			for (const auto& i : _items) if (i == f) return i;
			return nullptr;
		}

		file_size file_size() const
		{
			df::file_size result;
			for (const auto& i : _items) result += i->file_size();
			return result;
		}

		bool contains(const item_element_ptr& ii) const
		{
			for (const auto& i : _items) if (i == ii) return true;
			return false;
		}

		void append_unique(unique_items& results) const
		{
			// Sizing once avoids the repeated forced rehash that dominated this walk for large listings.
			results._items.reserve(results._items.size() + _items.size());
			for (const auto& i : _items) results._items.insert_or_assign(i->path(), i);
		}

		std::vector<folder_path> folder_paths() const
		{
			std::vector<folder_path> result;
			for (const auto& i : _items)
			{
				if (i->is_folder())
				{
					result.emplace_back(i->folder());
				}
			}
			return result;
		}

		std::vector<file_path> file_paths(const bool include_sidecars = true) const
		{
			hash_set<file_path, ihash, ieq> result;

			for (const auto& i : _items)
			{
				if (!i->is_folder())
				{
					auto&& path = i->path();
					auto&& folder = i->folder();

					result.emplace(path);

					if (include_sidecars)
					{
						const auto sidecar_parts = split(i->sidecars(), true);

						for (const auto& s : sidecar_parts)
						{
							result.emplace(folder.combine_file(s));
						}
					}
				}
			}

			return {result.begin(), result.end()};
		}
	};

	struct item_draw_info
	{
		static constexpr int _max_width = 500;
		static constexpr int _text_padding = 4;
		static constexpr int _thumb_padding = 2;

		int extent = 0;
		int width = 0;

		double val_max = static_cast<double>(INT64_MIN);
		double val_min = static_cast<double>(INT64_MAX);

		void clear_for_layout()
		{
			*this = {};
		}

		void update_extent(ui::draw_context& dc, const std::string_view text, const double val)
		{
			const auto max_width = round(_max_width * dc.scale_factor);

			extent = std::max(extent, dc.measure_text(text, ui::style::font_face::dialog,
			                                          ui::style::text_style::single_line, max_width).cx);
			val_max = std::max(val_max, val);
			val_min = std::min(val_min, val);
		}

		void update_extent(ui::draw_context& dc, const std::string_view text)
		{
			const auto max_width = round(_max_width * dc.scale_factor);
			extent = std::max(extent, dc.measure_text(text, ui::style::font_face::dialog,
			                                          ui::style::text_style::single_line, max_width).cx);
		}

		recti calc_bounds(const recti row_bounds, const int text_x, const int text_y, const int text_padding) const
		{
			auto bounds = row_bounds;
			bounds.left = text_x + text_padding / 2;
			bounds.right = bounds.left + width;
			return bounds;
		}

		void draw(ui::draw_context& rc, const std::string_view text, const double val, const recti bounds,
		          const ui::style::font_face text_font, const ui::style::text_style text_style,
		          const ui::color color) const
		{
			ui::color rank_color;

			if (val_min < val_max && setting.highlight_large_items)
			{
				const auto importance_alpha = std::min(
					color.a, static_cast<float>(0.7 * (val - val_min) / (val_max - val_min)));
				rank_color = ui::color(app_colors::rank_background, importance_alpha);
			}

			draw(rc, text, rank_color, bounds, text_font, text_style, color);
		}

		static void draw(ui::draw_context& rc, const std::string_view text, const ui::color bg_color,
		                 const recti bounds,
		                 const ui::style::font_face text_font, const ui::style::text_style text_style,
		                 const ui::color color)
		{
			rc.draw_text(text, bounds, text_font, text_style, color, bg_color);
		}
	};

	struct item_row_draw_info
	{
		item_draw_info icon;
		item_draw_info disk;
		item_draw_info track;
		item_draw_info title;
		item_draw_info flag;
		item_draw_info presence;
		item_draw_info sidecars;
		item_draw_info items;
		item_draw_info info;
		item_draw_info duration;
		item_draw_info file_size;
		item_draw_info bitrate;
		item_draw_info pixel_format;
		item_draw_info dimensions;
		item_draw_info audio_sample_rate;
		item_draw_info created;
		item_draw_info modified;

		void clear_for_layout()
		{
			icon.clear_for_layout();
			disk.clear_for_layout();
			track.clear_for_layout();
			title.clear_for_layout();
			flag.clear_for_layout();
			presence.clear_for_layout();
			sidecars.clear_for_layout();
			items.clear_for_layout();
			info.clear_for_layout();
			duration.clear_for_layout();
			file_size.clear_for_layout();
			bitrate.clear_for_layout();
			pixel_format.clear_for_layout();
			dimensions.clear_for_layout();
			audio_sample_rate.clear_for_layout();
			created.clear_for_layout();
			modified.clear_for_layout();
		}

		int total(const int text_padding) const
		{
			auto result = 0;
			if (icon.width > 0) result += icon.width + text_padding;
			if (disk.width > 0) result += disk.width + text_padding;
			if (track.width > 0) result += track.width + text_padding;
			if (title.width > 0) result += title.width + text_padding;
			if (flag.width > 0) result += flag.width + text_padding;
			if (presence.width > 0) result += presence.width + text_padding;
			if (sidecars.width > 0) result += sidecars.width + text_padding;
			if (items.width > 0) result += items.width + text_padding;
			if (info.width > 0) result += info.width + text_padding;
			if (duration.width > 0) result += duration.width + text_padding;
			if (file_size.width > 0) result += file_size.width + text_padding;
			if (bitrate.width > 0) result += bitrate.width + text_padding;
			if (pixel_format.width > 0) result += pixel_format.width + text_padding;
			if (dimensions.width > 0) result += dimensions.width + text_padding;
			if (audio_sample_rate.width > 0) result += audio_sample_rate.width + text_padding;
			if (created.width > 0) result += created.width + text_padding;
			if (modified.width > 0) result += modified.width + text_padding;
			return result;
		}
	};

	struct group_key
	{
		group_key_type type = group_key_type::grouped_no_value;
		prop::key_ref text1_prop_type = prop::null;
		icon_index icon = icon_index::none;
		file_group_ref group = nullptr;

		int32_t order1 = 0;
		int32_t order2 = 0;
		int32_t order3 = 0;

		str::cached text1 = {};
		str::cached text2 = {};
		str::cached text3 = {};


		group_key() noexcept = default;
		group_key(const group_key&) = default;
		group_key& operator=(const group_key&) = default;
		group_key(group_key&&) noexcept = default;
		group_key& operator=(group_key&&) noexcept = default;

		group_key(const group_key_type t) noexcept : type(t)
		{
		}

		bool operator<(const group_key& other) const
		{
			if (type != other.type) return type < other.type;

			if (order1 != other.order1) return order1 < other.order1;
			if (order2 != other.order2) return order2 < other.order2;

			// Interned storage: same pointer means same text, so the compare can be skipped.
			const auto text_delta1 = text1.id == other.text1.id ? 0 : icmp(text1, other.text1);
			if (text_delta1 != 0) return text_delta1 < 0;

			if (order3 != other.order3) return order3 < other.order3;

			const auto text_delta2 = text2.id == other.text2.id ? 0 : icmp(text2, other.text2);
			if (text_delta2 != 0) return text_delta2 < 0;

			if (text3.id == other.text3.id) return false;
			return icmp(text3, other.text3) < 0;
		}
	};


	class item_group final : public std::enable_shared_from_this<item_group>, public view_element
	{
	public:
		view_state& _state;
		item_elements _items;
		item_group_display _display = item_group_display::icons;
		mutable item_row_draw_info _row_draw_info;

		int _scroll_tooltip_rating = 0;
		std::vector<std::string> _scroll_tooltip_text;
		mutable item_element_ptr _scroll_tooltip_staging_item;

		mutable std::vector<recti> _layout_bounds;
		sort_by _sort_order = sort_by::def;
		bool _show_folder = true;
		group_key _key;

		// Title controls are derived from the key plus the search and grouping, so they survive the
		// group_layout passes that only reshuffle items. Rebuilding them was O(groups) allocations
		// and search_t copies per pass.
		std::shared_ptr<group_title_control> _title;
		uint32_t _title_generation = 0;

		std::string scroll_text;
		icon_index icon = icon_index::none;

		item_group(view_state& s, item_elements items, const item_group_display display, group_key key) noexcept :
			_state(s),
			_items(std::move(items)),
			_display(display),
			_key(std::move(key))
		{
		}

		void sort(group_by group_mode, sort_by sort_order, bool group_by_dups);

		const item_row_draw_info& row_widths() const
		{
			return _row_draw_info;
		}

		item_group_display display() const
		{
			return _display;
		}

		void toggle_display();
		void update_row_layout(const ui::measure_context& mc) const;
		void update_detail_row_layout(ui::draw_context& dc, const item_element_ptr& i, bool has_related) const;
		void display(item_group_display d);

		void items(item_elements items)
		{
			_items = std::move(items);
			_row_draw_info.clear_for_layout();
			for (const auto& item : _items) item->row_layout_valid = false;
		}

		const item_elements& items() const
		{
			return _items;
		}

		void render(ui::draw_context& dc, pointi element_offset) const override;
		sizei measure(ui::measure_context& mc, int width_limit) const override;
		void layout(ui::measure_context& mc, recti bounds_in, ui::control_layouts& positions) override;
		void scroll_tooltip(const item_element_ptr& item, const view_elements_ptr& elements, async_strategy& async) const;
		void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;
		view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
		                                             hit_test_context& ctx) override;

		item_element_ptr drawable_from_layout_location(pointi loc) const;
		void update_scroll_info(group_by gb);


		friend class item_set;
		friend class item_element;
		friend class item_group_header;
	};


	std::shared_ptr<group_title_control> build_group_title(view_state& s, const view_host_base_ptr& view,
	                                                       const item_group_ptr& g);
};

// locations.md 7.1: the totals affordance text. Defined next to the grouping label it was split
// away from, so the two can never drift into saying the same thing twice.
std::string format_items_totals(const df::file_group_histogram& summary, bool is_init_complete);
std::string format_items_summary(group_by grouping, sort_by order, const df::file_group_histogram& summary,
                                 bool is_init_complete);
