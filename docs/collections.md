# Diffractor Collections

This document owns the collection: what it is, how membership is decided and changed, what membership earns, and where the collection's edge is reported to the user. It states durable behavior, not class structure. Cross-cutting interaction rules live in [design.md](design.md); indexing architecture, threading, and data flow live in [implementation.md](implementation.md).

Both [primary design drivers](design.md#primary-design-drivers) apply without exception. The collection is the feature most able to violate them: it is the largest thing the application knows, it is built by walking storage the user controls, and it is the source of nearly every aggregate the user navigates by.

## What A Collection Is

**A collection is a declaration, not an accumulation.** It is a short list of included root folders plus a list of exclusions. It is not a record of per-file decisions, and nothing a user does while browsing quietly adds to it.

Three consequences follow, and they are the whole theory:

1. **Membership belongs to the folder, not to the file.** A file is in the collection because it sits under an included root that is not excluded. Moving a file into such a folder joins the collection; moving it out leaves. There is no "add to library" step and no per-file membership state that could disagree with where the file actually is. The filesystem remains the user's organizing tool.

   Import is not an exception. Import is a file operation — it copies or moves files into a folder structure built from metadata — and never a membership operation. Its results join the collection when, and only when, the destination folder is already a member, and importing to a folder outside the collection adds nothing to the collection. Membership still follows the folder; Import merely decides which folder a file ends up in.
2. **The declaration is small enough to read and reverse.** A user can see the whole collection definition at once, understand why any folder is or is not in it, and change it by editing a line. A definition that could only be inspected by querying it is not a mental model.
3. **Everything derived is rebuildable.** The index is a cache over files that remain the authority. Deleting it costs time, not information. This is what makes membership changes safe: they change what is *known*, never what *exists*.

## 3. Deciding membership

### 3.1 Includes

Roots come from two places, and they combine into one set:

- **Well-known folders** offered as switches: Pictures, Videos, Music, Dropbox photos, and the OneDrive Pictures, Video, and Music folders. A switch for a folder the system does not have contributes nothing rather than failing.
- **Other folders**, one entry per line, for anything else.

An include is recursive: naming a folder names its subtree. Naming both a folder and its descendant is not an error and does not double-count.

A "More folders" line that is not an existing path is not discarded, because the most common reason a path does not exist is that the storage is not attached right now. Such a line is resolved as a **device or volume label**, or as a **server name**. Labels matter for exactly the case where paths are worst: removable and network storage whose drive letter changes between sessions. A collection defined by label survives re-plugging; one defined by `E:\` does not.

### 3.2 Excludes

An entry prefixed with `-` excludes rather than includes, in three forms:

| Form | Example | Meaning |
|---|---|---|
| Full path | `-c:\photos\secret` | That folder and its subtree. |
| Folder name | `-secret` | Any folder with that name, anywhere in the collection. |
| Wildcard | `-proxy*` | Any folder whose name matches, anywhere in the collection. |

Excludes are evaluated during the walk, so an excluded folder is never entered and its subtree never joins. Name and wildcard excludes are how a user removes a recurring by-product folder — caches, proxies, hidden dot-folders — without listing every instance.

A full-path exclude is recorded **whether or not the path currently exists**. An exclude that silently disappeared while a drive was detached would let the next indexing pass pull in exactly the folder the user removed, so an exclude is a standing instruction rather than an observation.

### 3.3 The resulting rule

A folder is in the collection when it is an included root, or is reached from one by recursive walk without crossing an exclude. A file is in the collection when its folder is. Nothing else confers membership, and membership is never inferred from having viewed, searched, edited, or imported something.

The walk is bounded. A collection definition that would enumerate an unreasonable number of folders stops rather than growing without limit, because an unbounded walk trades the first driver for nothing the user asked for.

### 3.4 Folders that are known but not members

Browsing an external folder makes Diffractor aware of it, and that awareness is kept so navigation and completion stay useful. **Awareness is not membership.** A folder that has merely been visited contributes nothing to collection aggregates, is never a source of copies for redundancy reporting, and does not make its files report as being in the collection. The only way into the collection is the declaration.

This separation is what keeps the collection meaningful. If browsing joined, the collection would drift into "everywhere I have ever looked", which answers no question at all.

## 4. Changing a collection

Changing the declaration changes what is known. It never moves, copies, renames, deletes, or writes to a file. Adding a folder makes its contents findable; removing one makes them unfindable from collection scope while leaving them exactly where they are and still reachable by browsing.

Because the change is a re-derivation rather than a migration, it has no failure mode that loses user data and needs no confirmation on those grounds. It does have a cost — a new subtree must be walked and scanned — so it is progressive rather than modal: browsing stays usable while the change takes effect, and results improve as it does.

Removing a root does not delete a folder that was named twice by different means, and re-adding a previously indexed folder is cheap, because cached knowledge for it is reconciled against the filesystem rather than rebuilt from nothing.

## 5. What membership earns

Membership is not a label; it is the precondition for every whole-collection capability. Each of these is described in full by its owning document; what matters here is that each is defined over collection members and nothing else.

| Capability | What membership provides |
|---|---|
| Search | Terms resolve against the index across every member folder at once, instead of against one folder's directory listing. |
| Navigation aggregates | Dates, tags, ratings, labels, locations, media types, folders, and drives are counted over members, and the sidebar navigates by those counts. |
| Duplicate search | Copies are identified over members only, so a duplicate claim always has a defined population. See [§7](#7-redundancy-one-relation-three-questions). |
| [Related items](design.md#related-items) | An item's copies, album, series, capture time, and capture place are answered from members only, so "what else belongs with this?" has a defined population too. |
| [Presence](design.md#collection-presence) | An outside file can be compared against members to answer "do I already have this?". |
| Import and Sync | Both name a collection side, so their direction and effect are stated against a defined set rather than against an ad-hoc folder pair. |
| [Movie](movie.md#6-project-files) import | An old Movie Maker project's moved sources are found again among members, by name and by the length the project recorded, so the search has a defined population instead of the whole disk. |
| Totals | "The collection contains N items" is meaningful because N has a definition the user can inspect. |

The corollary is that a scope outside the collection cannot honestly offer these. When the current scope is not in the collection, that fact is reported plainly — the items shown are not from the collection. Where the report explains an empty or partial result, it also carries the action that would widen the collection, so the explanation and its remedy are the same surface. Silently returning empty or partial aggregates for a non-member scope would read as a broken product rather than as a scope distinction.

## 6. The collection's edge

The collection has an inside and an outside, and the interesting behavior is at the boundary. Two separate reports live there and must not be confused:

- **Membership** is a fact about a folder: this file is in the collection, or it is not. It is cheap, exact, and never probabilistic.
- **[Presence](design.md#collection-presence)** is an assessment about content: for a file outside, are there likely copies inside, and are they newer or older? It is evidence-based and states its confidence as *possible*.

Membership never implies uniqueness — a file being in the collection says nothing about whether three copies of it are also in the collection. That question belongs to [§7](#7-redundancy-one-relation-three-questions). Presence never implies a recommendation — it supports an import decision and is not a deletion verdict, does not choose a keeper, and never enlarges a command's target.

The honesty rule at this edge is absolute: **an incomplete collection may not publish an absence.** While indexing is incomplete the answer is "checking", not "not found". A confident wrong negative here is worse than a slow answer, because it is the exact input to a decision to delete or re-import.

## 7. Redundancy: one relation, three questions

A declared collection accumulates copies. Files arrive by import, by sync, by a camera emptied into a member folder twice, and by folders being added to the declaration years after they were filled. Redundancy is the normal outcome of owning a collection in place, so managing it is part of the collection rather than a feature beside it.

Three surfaces answer redundancy questions. They differ in exactly one thing — which population is compared against:

| Surface | Question | Compares |
|---|---|---|
| [Presence](design.md#collection-presence) | Do I already have this? | One file outside, against the whole collection. |
| [Related items](design.md#related-items) | What else belongs with this one? | One member, against the whole collection. |
| Duplicate search | Where is the collection redundant? | The collection, against itself. |

Boundary, item, whole. They are one relation asked at three scales, and the binding consequence is that **they may never disagree about a pair.** If two files are copies, every surface able to see both says so, at the same strength. A pair reported by one and denied by another is a defect, not a difference of feature — including when only one of the two files is a member, which is the case presence exists to cover.

### 7.1 Evidence grades

A copy claim states how it was reached. There are exactly three grades, strongest first:

| Grade | Evidence | Claim |
|---|---|---|
| **Identical** | The same bytes: equal size and equal checksum. | Certain. The files are interchangeable. |
| **Same file** | The same name and the same capture time. For audio and video, the same name and size. | Near certain. One file in two places. |
| **Same picture** | Different bytes, but the images match when compared as pictures, at the same capture time. Every quarter turn is compared, so a rotated copy is recognised. | Possible. A re-encode, re-save, resize, or rotation. |

The grade is reported rather than collapsed into the single word "duplicate". *Certain* and *possible* support different decisions, and merging them costs the user the only information that separates a safe deletion from a judgement call. A rotated copy is claimed as a copy: a quarter turn is a common grading step, and the picture is the one the user already has. A cropped version is a different picture and is not claimed.

Underneath the grades sits an explicit refusal: an image with too little detail to identify — a blank frame, a solid colour, a plain scanned page — is never claimed to match anything, because at that point every such image looks alike.

### 7.2 Groups are proven; similarities are anchored

This rule is what keeps redundancy honest, and it follows from what the grades are rather than from any threshold.

Identity is transitive. If A and B are identical and B and C are identical, then A and C are identical, and presenting all three as one group asserts only what was established. **The Identical and Same file grades form groups of any size.**

Similarity is not transitive. A may match B, and B may match C, while A and C are plainly different pictures. Joining those two findings end to end asserts something no comparison performed, and the set can then grow without limit while every individual comparison stayed comfortably inside its tolerance. How strict the comparison is does not control the outcome.

The Same picture grade is therefore **anchored, not chained**. Every item presented alongside an anchor was compared against that anchor. This does not confine a finding to two items: an anchor may have many possible copies, and a set of them is a legitimate answer. That the user can only compare two at a time is a property of viewing, not a statement about how many copies exist. What is forbidden is the chain — an item that reached the set only by resembling another member rather than the anchor.

Anchoring bounds how unlike one another the members of a set can be. It does not bound how many of them there are; that is [§7.3](#73-capture-time-is-not-identity)'s subject, and neither rule substitutes for the other.

### 7.3 Capture time is not identity

Capture time is recorded to the second, and cameras shoot faster than a second. Photographs sharing one exact capture time are therefore **not** evidence of copying: a burst produces many frames under one timestamp, and consecutive frames of one scene resemble each other at least as much as a re-encode resembles its original.

Two consequences bind:

- **Frames from one burst are not copies of one another.** A picture that matches many others *in the same orientation* under a single timestamp is a burst frame, not a re-save: continuous shooting produces frames that resemble each other however strict the comparison is, so the size of the crowd — not the closeness of any pair — is what identifies it. A capture time that answers with a crowd is declined in full rather than reported in part, because reporting part of it would mean choosing which frames count. A match that needed a quarter turn is never burst evidence, because continuous shooting does not produce a rotated frame.
- **A shared capture time never stands alone.** It narrows what is worth comparing. It is never itself a claim. Shape narrows it further: a picture whose neighbours under one timestamp are all a different shape cannot be a copy of any of them, and is not compared as a picture at all. A quarter turn transposes the stored extent, so a transposed neighbour still counts as the same shape.

The refusal is symmetric: an outside file being assessed for [presence](design.md#collection-presence) is judged by the same crowd rule against the same capture time, so the boundary and the whole answer alike.

This is the case redundancy reporting is most likely to get wrong, because the same narrowing that finds genuine re-encodes selects burst frames by construction.

### 7.4 What redundancy reporting will not do

- **It does not choose a keeper.** Which copy survives carries taste — a folder the user trusts, a filename they can read, the version they edited. Redundancy supplies evidence and stops. Its job is to put the candidates in front of the user, next to one another, and let them decide.
- **It does not enlarge a target.** Being reported as a copy never adds an item to the set a command acts on. Targeting follows [design.md](design.md#user-mental-model) without exception.
- **It does not publish an absence from an incomplete collection.** "No copy found" derived from a half-built index is the exact input to a wrong deletion, so while the answer is incomplete the answer is that it is incomplete. This is the same rule as [§6](#6-the-collections-edge), applied to all three surfaces.
- **It does not persist a verdict.** Redundancy is recomputed from current evidence, never stored as a judgement that could outlive the files that justified it. Improving the comparison therefore changes the answers instead of leaving a stale one behind, and a grouping the user disagrees with is corrected by fixing the comparison, not by migrating data.
- **It offers no bulk automatic resolution.** There is no "remove all duplicates" command. Such a command would have to choose keepers, would act partly on claims graded *possible*, and could not be previewed item by item at the scale where it would be worth having.

## 8. Truth, staleness, and honesty

The collection is a cache over storage the user can change behind Diffractor's back. Three rules keep the cache from becoming a second, competing truth:

1. **Files are authoritative; the index is derived.** Where they disagree, the file wins and the index is corrected. Metadata written to a file or sidecar is the record; the index merely remembers it.
2. **Knowledge is retained when storage is not reachable.** A member folder on a detached or offline drive stays a member, and what is known about it stays known and searchable, marked as offline rather than deleted. Forgetting a collection because a drive was unplugged would make the collection less trustworthy than the folder tree it describes.
3. **Incompleteness is disclosed, not hidden.** Indexing improves results progressively while browsing remains usable, and a surface whose results may be incomplete says so. Completed work replaces provisional results without requiring navigation or restart.

Presence wording and confidence values belong to [design.md](design.md#collection-presence), and the relations a related search reports belong to [design.md](design.md#related-items); the copy relation those surfaces share, its evidence grades, and the rules that keep them in parity are owned here in [§7](#7-redundancy-one-relation-three-questions). Place resolution over collection items belongs to [locations.md](locations.md). Index, summary, and scanning architecture belongs to [implementation.md](implementation.md#index-search-and-database). How a metadata write reaches disk belongs to [file-io.md](file-io.md).

## Where this lives

| Collection subject | Source |
|---|---|
| The declared collection folders, includes and excludes | [app_settings.h](../src/app_settings.h), [app_settings.cpp](../src/app_settings.cpp) |
| Membership resolution, folder roots, the collection edge | [model_index.cpp](../src/model_index.cpp), [model_index.h](../src/model_index.h) — `index_state` |
| The indexed record a member earns | [model_index.h](../src/model_index.h) — `df::index_file_item`, `df::index_folder_item` |
| Duplicate grouping and the perceptual-hash stage | [model_index_duplicates.cpp](../src/model_index_duplicates.cpp) — `index_state::update_predictions`; [app_dup_report.cpp](../src/app_dup_report.cpp) measures it over a real library |
| The bounded nearest-match sets a related search answers with | [model_related.h](../src/model_related.h) |
| Presence evaluation for a file outside the collection | [model_index_duplicates.cpp](../src/model_index_duplicates.cpp) — `index_state::queue_update_presence`, [model_search.cpp](../src/model_search.cpp) |
| Finding a Movie Maker project's moved sources among members | [view_movie.cpp](../src/view_movie.cpp) — `import_movie_maker_project`; [model_movie.cpp](../src/model_movie.cpp) — `choose_movie_sources`; [app_movie_batch.cpp](../src/app_movie_batch.cpp) walks the member folders for the command line |
| Persistence of the cached index | [model_db.cpp](../src/model_db.cpp), [model_db_pack.h](../src/model_db_pack.h) |

The three redundancy surfaces in [§7](#7-redundancy-one-relation-three-questions) read one relation,
so a change to the copy rules in `update_predictions` moves duplicate search, presence, and related
items together.
