# Diffractor Collections

This document owns the collection: what it is, how membership is decided and changed, what membership earns, and where its edge is reported. Interaction rules live in [design.md](design.md); indexing architecture, threading and data flow in [implementation.md](implementation.md).

Both [primary design drivers](design.md#primary-design-drivers) apply without exception: the collection is the largest thing the application knows, it is built by walking storage the user controls, and nearly every aggregate the user navigates by comes from it.

## 1. What a collection is

**A collection is a declaration, not an accumulation**: a short list of included root folders plus a list of exclusions. It records no per-file decisions, and nothing a user does while browsing adds to it.

## 2. What follows from it

1. **Membership belongs to the folder, not to the file.** A file is in the collection because it sits under an included root that is not excluded; moving it in or out joins or leaves. There is no "add to library" step, and the filesystem stays the user's organizing tool. Import is a file operation, not a membership one: its results join only when their destination folder is already a member.
2. **The declaration is small enough to read and reverse.** A user sees the whole definition at once, can tell why any folder is or is not in it, and changes it by editing a line.
3. **Everything derived is rebuildable.** The index is a cache over files that remain the authority; deleting it costs time, not information. A membership change therefore changes what is *known*, never what *exists*.

## 3. Deciding membership

### 3.1 Includes

Roots come from two places and combine into one set:

- **Well-known folders** offered as switches: Pictures, Videos, Music, Dropbox photos, and OneDrive's Pictures, Video and Music. A switch for a folder the system does not have contributes nothing.
- **Other folders**, one per line.

An include is recursive; naming a folder and its descendant does not double-count. A line that is not an existing path is read as a **device or volume label**, or a **server name**, because the usual reason a path is missing is that its storage is detached: a collection defined by label survives re-plugging a removable or network drive whose letter changes, and one defined by `E:\` does not.

### 3.2 Excludes

A line prefixed with `-` excludes, in three forms:

| Form | Example | Meaning |
|---|---|---|
| Full path | `-c:\photos\secret` | That folder and its subtree. |
| Folder name | `-secret` | Any folder with that name, anywhere in the collection. |
| Wildcard | `-proxy*` | Any folder whose name matches, anywhere in the collection. |

Excludes apply during the walk, so an excluded folder is never entered. Name and wildcard excludes remove recurring by-product folders - caches, proxies, dot-folders - without listing each one. A full-path exclude is recorded **whether or not the path exists**: one that vanished while its drive was detached would let the next walk pull in exactly the folder the user removed.

### 3.3 The resulting rule

A folder is in the collection when it is an included root, or is reached from one without crossing an exclude; a file is in when its folder is. Nothing else confers membership - not viewing, searching, editing or importing. The walk is bounded: a definition that would enumerate an unreasonable number of folders stops rather than growing without limit.

### 3.4 Folders that are known but not members

Browsing an outside folder makes Diffractor aware of it, and that awareness is kept for navigation and completion. **Awareness is not membership**: a visited folder adds nothing to collection aggregates, is never a source of copies for redundancy reporting, and does not make its files report as members. Otherwise the collection would drift into "everywhere I have ever looked".

### 3.5 What a folder holds

A collection folder can also say what it holds, in [Options](design.md#options): films and TV shows, personal recordings, music, or - the default - whatever each file's name says. The setting reaches every folder beneath unless a deeper one has its own. It changes how a video is read - film or episode, and so the date it files under ([metadata.md](metadata.md#films-episodes-and-music)) - and never membership.

## 4. Changing a collection

Changing the declaration changes what is known; it never moves, copies, renames, deletes, or writes a file. A removed folder's files stay where they are and remain reachable by browsing. Because the change is a re-derivation rather than a migration, it cannot lose user data and needs no confirmation on those grounds. It does cost a walk and a scan, so it is progressive rather than modal: browsing stays usable and results improve as it takes effect. A folder named twice by different means stays until both are removed, and re-adding a folder the index still knows is cheap, because what is known is reconciled with the filesystem rather than rebuilt.

## 5. What membership earns

Membership is the precondition for every whole-collection capability. Each is described by its owning document and defined over members and nothing else:

| Capability | What membership provides |
|---|---|
| Search | Terms resolve against the index across every member folder at once. |
| Navigation aggregates | Dates, tags, ratings, labels, locations, media types, folders and drives are counted over members. |
| Duplicate search | Copies are identified over members only ([§7](#7-redundancy-one-relation-three-questions)). |
| [Related items](design.md#related-items) | What else belongs with an item is answered from members only. |
| [Presence](design.md#collection-presence) | An outside file is compared against members: "do I already have this?". |
| Import and Sync | Each names a collection side, so its direction and effect are stated against a defined set. |
| Totals | "The collection contains N items" means something because N has an inspectable definition. |

A scope outside the collection cannot honestly offer these. When the current scope is not in the collection that is reported plainly, and where the report explains an empty or partial result it carries the action that would widen the collection. Silently empty aggregates would read as a broken product rather than a scope distinction.

## 6. The collection's edge

Two reports live at the boundary and must not be confused:

- **Membership** is a fact about a folder: cheap, exact, never probabilistic.
- **[Presence](design.md#collection-presence)** is an assessment of content: for an outside file, are there likely copies inside, newer or older? It is evidence-based and states its confidence as *possible*.

Membership never implies uniqueness; whether a member has copies is [§7](#7-redundancy-one-relation-three-questions)'s question. Presence never implies a recommendation: it supports an import decision, is not a deletion verdict, chooses no keeper, and never enlarges a command's target.

**An incomplete collection may not publish an absence.** While indexing is incomplete the answer is "checking", not "not found": a confident wrong negative is the exact input to a decision to delete or re-import.

## 7. Redundancy: one relation, three questions

A declared collection accumulates copies - by import, by sync, by a camera emptied twice, by folders added years after they were filled - so managing redundancy is part of the collection. Three surfaces answer it, differing only in what is compared:

| Surface | Question | Compares |
|---|---|---|
| [Presence](design.md#collection-presence) | Do I already have this? | One outside file against the collection. |
| [Related items](design.md#related-items) | What else belongs with this one? | One member against the collection. |
| Duplicate search | Where is the collection redundant? | The collection against itself. |

They are one relation at three scales and **may never disagree about a pair**: if two files are copies, every surface able to see both says so at the same strength, including when only one of them is a member.

### 7.1 Evidence grades

A copy claim states how it was reached, in one of three grades, strongest first:

| Grade | Evidence | Claim |
|---|---|---|
| **Identical** | Equal size and equal checksum. | Certain: the files are interchangeable. |
| **Same file** | The same name and the same capture date - for a file that states none, its creation time. For audio and video, the same name and size. | Near certain: one file in two places. |
| **Same picture** | Different bytes whose pictures match at the same capture time, compared at every quarter turn. | Possible: a re-encode, re-save, resize or rotation. |

The grade is reported, not collapsed into "duplicate": *certain* and *possible* support different decisions, and merging them hides what separates a safe deletion from a judgement call. A rotated copy is a copy - a quarter turn is a common grading step - but a crop is a different picture and is not claimed. An image with too little detail to identify - a blank frame, a solid colour, a plain scanned page - is never claimed to match anything.

### 7.2 Groups are proven; similarities are anchored

Identity is transitive, so **the Identical and Same file grades form groups of any size**: a group asserts only what was established.

Similarity is not transitive: A may match B and B match C while A and C are plainly different, and joining findings end to end lets a set grow without limit however strict each comparison is. **Same picture is anchored, not chained**: every item presented with an anchor was compared against that anchor. An anchor may have many possible copies; what is forbidden is an item that joined only by resembling another member. Anchoring bounds how unlike the members are, not how many there are, which is [§7.3](#73-capture-time-is-not-identity)'s subject.

### 7.3 Capture time is not identity

Capture time is recorded to the second and cameras shoot faster, so a shared capture time is **not** evidence of copying: a burst's frames share one timestamp and resemble each other at least as much as a re-encode resembles its original.

- **Frames from one burst are not copies of one another.** A picture matching many others *in the same orientation* under one timestamp is a burst frame: the size of the crowd, not the closeness of any pair, identifies it, and the whole capture time is declined rather than reported in part. A match that needed a quarter turn is never burst evidence, because continuous shooting does not produce a rotated frame.
- **A shared capture time never stands alone.** It narrows what is worth comparing and is never itself a claim. Shape narrows further: a picture whose neighbours under one timestamp are all a different shape is not compared at all, and a transposed neighbour - a quarter turn swaps width and height - counts as the same shape.

Presence judges an outside file by the same crowd rule against the same capture time, so the boundary and the whole answer alike. This is the case most likely to go wrong, because the narrowing that finds re-encodes selects burst frames by construction.

### 7.4 What redundancy reporting will not do

- **Choose a keeper.** Which copy survives carries taste - a trusted folder, a readable name, the edited version - so redundancy puts the candidates side by side and lets the user decide.
- **Enlarge a target.** Being reported as a copy never adds an item to a command's target ([design.md](design.md#user-mental-model)).
- **Publish an absence from an incomplete collection** ([§6](#6-the-collections-edge)), on any of the three surfaces.
- **Persist a verdict.** Redundancy is recomputed from current evidence, so a better comparison changes the answers rather than leaving stale ones, and a disputed grouping is fixed in the comparison, not by migrating data. A duplicate search on screen follows the recount: a file whose last copy was deleted leaves it without a refresh.
- **Resolve in bulk.** There is no "remove all duplicates": it would have to choose keepers, act partly on claims graded *possible*, and could not be previewed item by item at the scale where it would be worth having.

## 8. Truth, staleness, and honesty

The collection is a cache over storage the user can change behind Diffractor's back. Three rules keep it from becoming a second truth:

1. **Files are authoritative; the index is derived.** Where they disagree the file wins and the index is corrected; metadata written to a file or sidecar is the record.
2. **Knowledge is retained when storage is not reachable.** A member folder on a detached or offline drive stays a member, and what is known of it stays searchable, marked offline rather than deleted.
3. **Incompleteness is disclosed, not hidden.** Results improve progressively while browsing stays usable, a surface whose results may be incomplete says so, and completed work replaces provisional results without navigation or restart.

Presence wording and confidence belong to [design.md](design.md#collection-presence), and the relations a related search reports to [design.md](design.md#related-items); the copy relation they share, its grades and their parity are owned here. Place resolution belongs to [locations.md](locations.md), index and scanning architecture to [implementation.md](implementation.md#index-search-and-database), and how a metadata write reaches disk to [file-io.md](file-io.md).

## Where this lives

| Collection subject | Source |
|---|---|
| The declared folders and what each holds | [app_settings.h](../src/app_settings.h)'s `settings_t::index_t` and [app_settings.cpp](../src/app_settings.cpp); [view_options.cpp](../src/view_options.cpp) edits them |
| Roots, excludes and labels read from the declaration | [app_workers.cpp](../src/app_workers.cpp)'s `index_folders`, [app.cpp](../src/app.cpp)'s `parse_more_folders`, [app_util.cpp](../src/app_util.cpp)'s `core::is_excluded` |
| The walk, membership and the collection edge | [model_index.cpp](../src/model_index.cpp), [model_index.h](../src/model_index.h) - `index_state` |
| The indexed record a member earns | [model_items.h](../src/model_items.h) - `df::index_file_item`, `df::index_folder_item` |
| Duplicate grouping and the perceptual stage | [model_index_duplicates.cpp](../src/model_index_duplicates.cpp) - `index_state::update_predictions`; [app_dup_report.cpp](../src/app_dup_report.cpp) measures it over a real library |
| Presence for a file outside the collection | [model_index_duplicates.cpp](../src/model_index_duplicates.cpp) - `index_state::queue_update_presence` |
| Related items | [model_search.cpp](../src/model_search.cpp)'s `search_matcher::evaluate_related`, and [model_related.h](../src/model_related.h)'s bounded nearest-match sets it answers with |
| Persistence of the cached index | [model_db.cpp](../src/model_db.cpp), [model_db_pack.h](../src/model_db_pack.h) |

The three surfaces in [§7](#7-redundancy-one-relation-three-questions) apply the copy rules through three paths - `update_predictions` for duplicate search, presence's pass beside it, and `dup_match` in [model_index.h](../src/model_index.h) for related items - which share `same_picture_shape`, `max_duplicate_phash_distance` and the capture-time crowd limits. A change to the rules is made in all three.
