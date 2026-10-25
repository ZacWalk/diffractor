# Zoom

This document owns current image magnification, panning, comparison linking, panorama projection,
input, and navigator behavior. [Rendering](rendering.md) owns backend implementation and
[File I/O](file-io.md) owns the decoded representations zoom consumes.

## Model

Each presented image has a scale and a source-space center. Scale is one of Fit, Fit width, Fill, or
an explicit value from the fixed zoom ladder. Fit variants recalculate from source and viewport
dimensions; explicit scale and center survive layout changes and navigation where meaningful.

The center is normalized in source coordinates. Destination geometry clamps it only when an image
edge would enter the viewport, and recenters an axis whose scaled image fits. Scale changes with a
pointer location preserve the source point under that pointer; keyboard and menu changes anchor the
viewport center.

## Magnified States

- **Inspect zoom** is temporary magnification begun by pressing and holding on an image. Releasing or
  losing capture restores the previous state. It can be committed in place to durable zoom mode.
- **Zoom mode** is durable explicit magnification. It uses the full media surface and remains until
  Fit or Escape is chosen, or navigation reaches content that cannot be magnified.

Both states use the same scale ladder, source-space center, pan geometry, rendering, comparison
linking, and navigator. Zoom mode admits zoom, navigation, grading, pane flip, playback, and
fullscreen commands; commands whose target or effect is not visible are suppressed.

## The Laws

1. Every scale change has an anchor.
2. Stepping outward reaches Fit and stops rather than creating an accidental undersized state.
3. Scale and center are visible through the zoom readout and navigator.
4. Input responds from pixels already held; decode and file access never block it.
5. A newer item, request, or generation cannot be replaced by stale decode work.
6. Navigation preserves source-space subject position when source geometry permits it.
7. Escape removes region selection before zoom mode and never skips directly to closing the view.
8. Flat-image pixels use point sampling at exactly 1:1, bicubic above 1:1 through 3x, and point
   sampling above 3x. Interactive magnification uses bilinear sampling inside that 1:1-to-3x
   interval; above 3x it also uses point sampling. Magnified stand-ins stay smooth until source
   pixels arrive.

## Rendering Tiers

GPU and software backends consume the same zoom geometry and sampler choice. The GPU applies scale,
translation, and opacity while drawing a retained texture. The software backend samples only the
clipped viewport into bounded tiles. Interactive input uses the affordable path for its backend;
settled output may use higher-quality downsampling.

Alpha transitions run only when the rendering animation gate is enabled. Disabling animation must
not alter anchoring, final geometry, or image quality.

## Scale And Pan

The explicit ladder ranges from small percentages through 100% to high magnification and always
steps between fixed adjacent values. Fit is inserted as the floor when its calculated scale lies
between ladder values. Toggle Fit restores the scale and center Fit replaced when one exists: an
explicit scale, or a Fit width or Fill that goes on following the viewport.

Pointer drag, keyboard pan, auto-pan, and navigator repositioning all update the same center. Stored
pan is clamped to the same visible interval used for drawing, so reversing at an edge moves the
picture immediately rather than consuming hidden overscroll. Pan acceleration derives from
displacement rather than event frequency. Wheel fractions accumulate before producing a step so
precision devices do not lose motion.

Region zoom selects a source rectangle and chooses the largest ladder value that fits it, centered
on the selected region. A gesture below the drag threshold remains a click.

## Comparison

Each comparison pane has its own source geometry but shares an effective scale and source-space
center. The active pane supplies the anchor. Pane flip changes which item occupies each pane without
changing the compared region. Different source dimensions are reconciled in normalized source
space, not by copying scroll offsets.

## Panorama

### Standing Inside An Equirectangular Panorama

An equirectangular panorama can be viewed as flat pixels or as a camera inside the sphere. Projected
mode replaces percentage scale with a bounded field-of-view ladder, and stores yaw and pitch instead
of a planar center. Drag turns the camera; stepping adjusts field of view; the navigator marks the
camera's covered source region.

Partial panoramas use GPano full dimensions and crop offsets to map stored texels onto the sphere.
Longitude wraps only when the declared panorama spans the full width. A same-path source whose
dimensions change rereads declaration geometry without resetting the user's camera or flat/projected
choice; unchanged-source refreshes keep both. GPU projection uses a shader; software rendering uses
the shared panorama rasterizer and publishes generation-checked surfaces.

## Navigator

The navigator appears when magnified and shows the visible source region. Clicking or dragging it
moves the center. The setting offers Auto-hide or Off; activity reveals the auto-hidden navigator
for a bounded interval. Panorama mode draws the field-of-view patch in the same surface.

## Input

### The Wheel

The pointer chooses the receiving surface. Over magnified media, horizontal tilt pans horizontally.
During inspect zoom, vertical stepping adjusts magnification. In durable zoom mode, Control plus the
wheel steps scale while the unmodified vertical wheel retains sequence navigation. Outside zoom,
the wheel keeps the behavior of the view under the pointer.

### Pinch

Pinch on magnifiable media anchors at the gesture center and steps the same ladder. Touch pan updates
the same source-space center in Items and Fullscreen when the gesture starts on magnified media, and
double-tap toggles between Fit and 100% at the tapped point. Mouse, touch, keyboard, and menu paths
do not create separate zoom state.

## Quality And Memory

Zoom requests display-sized decode through the progressive image ladder. A thumbnail or embedded
preview can stand in immediately while a better representation is decoded. Higher-resolution
publication never replaces a better current representation with a worse one.

Decoded images remain subject to the rendering budgets. Eviction preserves enough encoded or cached
data to rebuild without changing layout or zoom state. GPU resource loss drops textures, not the
model's scale and center.

## Where this lives

- [model_zoom.h](../src/model_zoom.h): scale modes, ladder, source-space center, anchoring, bounds,
  panning, and navigator timing.
- [view_media.h](../src/view_media.h) and [ui_elements.cpp](../src/ui_elements.cpp): media input and presentation.
- [model_display.h](../src/model_display.h): display state, inspect zoom, zoom mode, and comparison
  coordination.
- [ui_panorama.h](../src/ui_panorama.h) and
  [render_panorama.cpp](../src/render_panorama.cpp): panorama camera and software projection.
- libdesktop's [render_surface.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/render_surface.cpp): shared resampling.
- [app_commands.cpp](../src/app_commands.cpp): zoom commands and navigator setting.