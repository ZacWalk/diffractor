# Diffractor Locations

This document owns place attribution, location search, distance, maps, visit derivation, and map-tile
storage. [Design](design.md) owns navigation and presentation rules; [Metadata](metadata.md) owns GPS
tag mapping; [Implementation](implementation.md) owns threading and database boundaries.

## Place Data

Diffractor ships a compact GeoNames-derived gazetteer with place, region, country, population,
coordinates, alternate names, and localized names. Country and region tables supply canonical and
localized labels. The generator removes duplicate records while retaining useful alternate names.

`location_cache` loads the data once, indexes coordinates with a KD-tree, and indexes names for
lookup and autocomplete. It can resolve a record by identity, find nearby or significant places in
an area, and localize display names. Geographic lookup keeps seam and pole handling in the location
layer: KD-tree candidates are only a planar acceleration structure, and final nearest/attribution
decisions use geodesic distance with conservative wrapped bounds. Gazetteer access runs on the
location worker.

## Item Locations

An item may carry stored place, state, country, coordinate, altitude, and GPS date metadata. Stored
text remains the item's authored metadata. When useful text is absent and a coordinate exists,
Diffractor derives a place for grouping, search, and display without writing it back to the file.

Derived values are cache data. Editing a location is an explicit operation that writes the selected
coordinate through the metadata pipeline; browsing, indexing, grouping, and searching do not modify
the media file.

## Location Search

Location-aware terms are distinct from raw metadata properties:

- `loc:` and `near:` resolve a place name or coordinate and may carry a distance.
- `place:`/`city:`, `state:`, and `country:` constrain the location level.
- `latitude:`/`x:` and `longitude:`/`y:` compare stored coordinate properties.
- `area:` remains readable for saved map-cell searches but new map actions produce `loc:` terms.

Distance accepts metres, kilometres, and miles and is normalized for display. Text matching uses
gazetteer identity and the item's stored or derived location; coordinate matching uses geodesic
distance. Search generations and location-result generations prevent an older lookup replacing a
newer query.

Autocomplete displays localized place and country names but compares canonical country identifiers
when it limits very short place prefixes to the default country. Explicit country-code completion
continues to use the code itself.

Location grouping uses the same effective place identity as location search. A group-header or
breakdown action must therefore reproduce the items it counted, including coordinate-only items.

## Map And Globe

The sidebar globe displays the collection heat map on an orthographic sphere. Dragging rotates it;
movement beyond the click threshold never invokes a marker. Only the visible hemisphere is drawn or
hit-tested. The initial view faces the collection's weighted location center and remains where the
user leaves it after manual rotation.

### Areas Become Places With A Radius

A map area resolves to a named place and a radius covering its item bounds. If no suitable name is
available, its coordinate and radius are used. Invoking it opens a `loc:` search. It changes the
query only and preserves grouping, sorting, focus, and selection.

Map bubbles show the resolved name, count, and representative image when available. Marker cells are
aggregated at the current zoom off the UI thread and published as complete snapshots; paint and hit
testing consume the same ready geometry. Resolution may arrive asynchronously; an unresolved bubble
states only facts already known and remains actionable through its coordinate.

The Locate task uses a different map contract: the fixed center crosshair is the coordinate to be
written. Dragging pans, clicking a cluster centers it, and no file changes until the user runs Add
location against the reviewed selection.

## Visits

`compute_visits` derives visit candidates from a detached snapshot of the current results. It filters
items without usable date/location data, groups nearby items, segments each group by time gaps,
classifies long residence-like runs separately, ranks bounded candidates, and publishes only if the
request generation is still current.

Visit derivation feeds location summaries; it does not make the database authoritative for a trip or
write an album into media. The presentation uses only nodes the model has produced and may omit the
strip when the result set does not support a useful summary.

## The Items Control Bar

The totals control reports item count and total size; grouping and sorting remain a separate user
choice. For a location search or drill-down that is not already grouped by location, Items may show
a breakdown row of the strongest effective places in the current results. Each entry includes a
count and opens the same level-scoped search used to compute that count. Partial place identities
that would overlap the same items are merged into a deterministic non-overlapping predicate before
the row is ranked, so the chip count and the query it runs remain the same promise. When no common
predicate can cover an overlap component, the row keeps the largest exact predicates that do not
overlap one another and leaves smaller overlapping partial identities out of the row rather than
showing a chip whose click would return a different count.

The row is hidden when location grouping already supplies equivalent group headers or when there are
no located results. It is navigation only: it does not change grouping, sorting, focus, or selection.

## Tile Cache

Downloaded map tiles live in `map-tiles-cache.db`, separate from the media index database and owned
by the tile-database queue. The store is rebuildable and bounded by age and least-recent use while
retaining recently fetched tiles. An unreadable store can be replaced without affecting media data;
if it cannot be opened, maps continue without persistent tile caching.

## Threading

Gazetteer loading, attribution, marker building, visit derivation, and tile database access stay off
the UI thread and outside paint. Workers consume immutable requests and publish detached results;
the UI applies them after lifetime, query, language, and generation checks.

## Where this lives

- [model_location.h](../src/model_location.h): coordinates, effective locations, and attribution.
- [model_locations.cpp](../src/model_locations.cpp) and
  [model_locations.h](../src/model_locations.h): gazetteer and autocomplete.
- [model_visits.cpp](../src/model_visits.cpp) and [model_visits.h](../src/model_visits.h): visit
  derivation.
- [model_search.h](../src/model_search.h): `location_scopes`, the one list of location scope spellings
  and the level each constrains to.
- [model_search.cpp](../src/model_search.cpp): location query parsing and matching.
- [app_sidebar.h](../src/app_sidebar.h), [ui_globe.h](../src/ui_globe.h), and
  [render_globe.cpp](../src/render_globe.cpp): sidebar globe and map actions.
- [ui_map_common.h](../src/ui_map_common.h), [ui_map.h](../src/ui_map.h), and
  [view_locate.cpp](../src/view_locate.cpp): shared map behavior and location editing.
- [view_items.cpp](../src/view_items.cpp): location breakdown row.
- [model_tile_cache.cpp](../src/model_tile_cache.cpp): persistent tile cache.
- `exe/location-places.txt`, `exe/location-countries.txt`, and `exe/location-states.txt`: generated
  location data; `tools/generate_locations.py` builds them.