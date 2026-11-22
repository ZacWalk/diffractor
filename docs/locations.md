# Diffractor Locations

This document owns place attribution, location search, distance, the maps and globes, Explore's trips,
visit derivation, and map-tile storage. [Design](design.md) owns navigation and presentation, including
[Explore](design.md#explore)'s timeline; [Metadata](metadata.md) owns GPS tag mapping;
[Implementation](implementation.md) owns threading and database boundaries.

## Place Data

Diffractor ships a compact GeoNames-derived gazetteer - places, regions and countries, with population,
coordinates, alternate and localized names - from which the generator removes duplicate records while
keeping useful alternate names.

`location_cache` loads it once, on the location worker, indexing coordinates in a KD-tree and names for
lookup and autocomplete; it resolves records, finds nearby or significant places in an area, and
localizes names. The KD-tree only accelerates candidates: nearest-place and attribution decisions use
geodesic distance with wrapped bounds, so seams and poles stay correct.

## Item Locations

An item may store place, state, country, coordinate, altitude and GPS date; stored text is its authored
metadata. Where text is absent and a coordinate exists, Diffractor derives a place for grouping, search
and display without writing it back. Only an explicit edit writes a location: Locate writes the reviewed
coordinate alone through the metadata pipeline, leaving place, state and country to be derived from it.
Browsing, indexing, grouping and searching never modify a file.

## Location Search

- `loc:` and `near:` resolve a place name or coordinate and may carry a radius.
- `place:`/`city:`, `state:` and `country:` constrain the location level.
- `latitude:` (or `x:`) and `longitude:` compare stored coordinates; `y:` is the year.
- `area:` is read for saved map-cell searches; map actions produce `loc:` terms.

Plain words also search the place, region, country and nearby city an item's GPS names, alongside
filenames, other metadata and faces. `london zac` requires **both** words on one item: a London geotag
and a Zac face qualify, as do a London caption and a Zac tag. `loc:` or `person:` restricts a word to
that field.

A city search includes known subdivisions without changing the place shown on a photo. The gazetteer
gives a GeoNames populated section (`PPLX`) a parent city when one in the same country, region and
second-level area lies within 50 km, has at least 100,000 people, is larger than the section, and has at
least twice the population of any rival there; a separate nearby town gets none. For stored place
names, the region and country narrow the lookup, and records that disagree on the parent infer none. An
explicit radius still measures from the requested centre.

Distance accepts metres, kilometres and miles and is normalized for display. Text matching uses
gazetteer identity and the item's stored or derived location; coordinate matching uses geodesic
distance. Generations stop an older lookup replacing a newer query's answer.

Autocomplete shows localized place and country names, but limits very short place prefixes to the
default country by its canonical identifier; country-code completion uses the code itself.

Location grouping uses the same effective place identity as location search, so a group header or a
place chip reproduces the items it counted, coordinate-only items included.

## Map And Globe

The sidebar globe shows the collection's heat map on an orthographic sphere. It first faces the
collection's count-weighted centre and stays wherever the user turns it; a drag turns it and never
invokes a marker, and only the near hemisphere is drawn or hit-tested. A button in the corner the
sphere leaves clear, its background shown only under the pointer, expands it into Explore's globe. The
keyboard passes over the sphere, whose clicks are placed as it is drawn, and reaches that button.

### Explore's Globe

[Explore](design.md#explore)'s globe is the same sphere, larger, zooming from the whole world to
streets: the wheel zooms about the pointer, the status-bar buttons about the centre, and a drag turns it
keeping the grabbed point under the pointer, Escape putting it back. The two globes face wherever the
user last turned either; Explore taking up a turn made on the sidebar's shows its whole sphere.

Zoomed in, the sphere is draped with the Locate map's tiles, at the level whose pixels are nearest the
screen's at each part of it. A tile not yet arrived shows the finer tiles still held from before a zoom
out, each over its quarter, and its nearest arrived ancestor over the rest, else the built-in map, which
also shows beyond the tiles' 85° of latitude, so drawing never waits for the network. Tiles are fetched
and kept as the [tile cache](#tile-cache) says; a session that may not reach the network shows only
cached ones; and the tiles' attribution shows while any are drawn.

Places gather into clusters, one per cell about 64 pixels across at the scale the globe is seen at, so
zooming in splits clusters and zooming out merges them. Cells are square on the sphere, in bands of
latitude, so each covers about the same ground anywhere, and grow with the display's scale as markers
do; where a cell edge falls between places a marker apart, the larger cluster takes in its neighbour. A
marker grows with its count. Clicking a cluster opens its area, as below; a drag never does.

### Areas Become Places With A Radius

A map area resolves to a named place and a radius covering its items, or to its coordinate and that
radius when nothing near enough names it. Invoking it opens a `loc:` search, changing the query only:
grouping, sorting, focus and selection stay.

Map bubbles show the resolved name, count, and a representative image when available. Marker cells are
aggregated for the current zoom off the UI thread and published as complete snapshots that paint and
hit testing share. A name may arrive later; until then a bubble states only what is known and stays
actionable through its coordinate.

Locate's map is different: its fixed centre crosshair is the coordinate to be written. A drag pans,
clicking a cluster centres it, and no file changes until Add location runs on the reviewed selection.

## Visits

`compute_visits` derives visits on the location worker from a detached snapshot of the current results:
it drops items without a usable date and location, clusters nearby items, splits each cluster at gaps
in time, sets long residence-like runs apart as eras, ranks a bounded set of candidates, and publishes
only if its request is still current. It also tallies the results' places; of what it derives, Items
presents only that tally, as [the places row](#the-places-row). Visits are derived, never stored as
albums or written into files.

## Trips And A Year's Places

Explore labels its timeline with trips, and each year with the places it was spent away from home. A
place here is a country, or a state with its country, matched as `country:` and `state:` match it: the
file's own text, else the gazetteer's answer for its coordinate. A city is never the place, because a
place search also reaches items near it and would list more than was counted.

Each day with located items is where most of them were taken. Home, for a year, is the country and the
state most such days fell in over that year and the years either side, and any other holding a quarter
of them; a year with none borrows the nearest year's. A day is away in a country that is not home, or
in a state of home's country that is not home's. A trip is a run of days away in one place - broken by
a day anywhere else, or by more than four days with nothing located - holding at least three items. Its
count, and its search, are the items of its place taken within its days.

A year's places are those its trips went to, up to four, by the most items of the place taken that year.
Each opens that place's search together with the year, and counts what that lists.

## The Places Row

For a location search or drill-down not already grouped by location, Items heads its results with a
row of the strongest effective places in them, each chip a count that opens the level-scoped search
which produced it. The row is hidden when location group headers already say this or the results hold
fewer than two places, collapses to a one-line header rather than bury the items, and, like every
optional row ([design](design.md#application-structure)), is navigation only. Partial place identities
that would overlap the same items are first merged into one non-overlapping predicate, so a chip's
count and its search stay one promise; where no common predicate covers an overlap, the largest exact
predicates that do not overlap are kept and the smaller overlapping ones left out.

## Tile Cache

Downloaded map tiles live in `map-tiles-cache.db`, apart from the media index and owned by the
tile-database queue. The store is rebuildable, bounded by age and least-recent use while keeping
recently fetched tiles; an unreadable store is replaced without touching media data, and if it cannot
be opened maps work without persistent caching.

Only tiles a map or globe shows now are fetched, nearest the middle of the view first. A tile the view
has left is let go wherever it waits - for the store, the network or its decode - so the tiles on screen
never wait behind ones that were, and nothing is downloaded for a view already left, as the
OpenStreetMap usage policy asks. Two tiles download at once, each worker taking the most wanted tile
still shown, while another worker decodes tiles the store holds. A tile that could not be had is not
asked for again for ten seconds.

## Threading

Gazetteer loading, attribution, marker building, Explore's model and clusters, visit derivation and
tile-store access stay off the UI thread and outside paint. Workers publish detached results, which the
UI applies after lifetime, query, language and generation checks; [Implementation](implementation.md#synchronized-types)
owns how the tile cache and the visit timeline are shared.

## Where this lives

- [model_location.h](../src/model_location.h): coordinates, effective locations, and attribution.
- [model_locations.cpp](../src/model_locations.cpp) and [model_locations.h](../src/model_locations.h):
  `location_cache`, the gazetteer and autocomplete.
- [model_visits.cpp](../src/model_visits.cpp) and [model_visits.h](../src/model_visits.h):
  `compute_visits` and the place tally.
- [model_search.h](../src/model_search.h): `location_scopes`, the one list of location scope spellings
  and the level each constrains to.
- [model_search.cpp](../src/model_search.cpp): location query parsing and matching.
- [app_sidebar.h](../src/app_sidebar.h), [ui_globe.h](../src/ui_globe.h), and
  [render_globe.cpp](../src/render_globe.cpp): the globes, their tile draping, and the sidebar's map
  actions.
- [model_explore.cpp](../src/model_explore.cpp) and [model_explore.h](../src/model_explore.h): trips, a
  year's places, Explore's clusters, and a map area's name and search, which both globes share.
- [view_explore.cpp](../src/view_explore.cpp): Explore's globe, its zoom, and its links to the timeline.
- [ui_map_common.h](../src/ui_map_common.h) and [view_locate.cpp](../src/view_locate.cpp): the tile
  cache maps and globe share, the Locate map, and location editing.
- [view_items.cpp](../src/view_items.cpp): `build_location_breakdown_row`, the places row.
- [model_tile_cache.cpp](../src/model_tile_cache.cpp): the persistent tile store.
- `exe/location-places.txt`, `exe/location-countries.txt` and `exe/location-states.txt`: the generated
  gazetteer, which `tools/generate_locations.py` builds.