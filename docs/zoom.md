# Zoom

This document owns image magnification: the zoom model and its laws, panning, comparison linking, panorama
projection and looking around, input, rendering tiers, and the navigator. [Rendering](rendering.md) owns image
budgets, the draw backends are libdesktop's, and [File I/O](file-io.md) owns the decoded representations zoom
consumes.

## Model

Each presented image has a scale and a source-space center. Scale is Fit, Fit width, Fill, or an explicit scale;
stepping moves between the values of a fixed zoom ladder. Fit variants recalculate from source and viewport
dimensions; an explicit scale and center survive layout changes and navigation, and an explicit scale below a new
image's Fit shows that image fitted and resumes on the next image it magnifies.

The center is normalized in source coordinates. Destination geometry clamps it only when an image edge would
enter the viewport, and recenters an axis whose scaled image fits. A scale change at the pointer keeps the source
point under the pointer; keyboard and menu changes anchor the viewport center.

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
8. Flat images are point-sampled at exactly 1:1 and above 3x and sampled bicubic otherwise; interactive input
   samples bilinear where bicubic would be. A stand-in stays smooth above 3x until source pixels arrive. A video
   frame is never pre-scaled, so a picture drawn below three quarters of its decoded size on both axes is sampled
   bilinearly, judged from the fitted picture rather than the pane it sits in. A picture the GPU draws at less
   than half its size is averaged over each pixel's footprint
   ([render.md](https://github.com/ZacWalk/libdesktop/blob/main/docs/render.md#textures-video-and-vertices)), and
   one the decode thread reduces by an area filter.

## Rendering Tiers

GPU and software backends consume the same zoom geometry and sampler choice. The GPU scales, translates and fades
a retained texture; the software backend samples only the clipped viewport, a bounded tile at a time. Interactive
input uses the affordable path for its backend; settled output may use higher-quality downsampling. Alpha
transitions run only when the rendering animation gate is open, and disabling them changes no anchoring, final
geometry or image quality.

## Scale And Pan

The ladder steps between fixed adjacent values, from small percentages through 100% to high magnification, with
Fit inserted as the floor where its scale lies between ladder values. Toggle Fit restores the scale and center Fit
replaced when one exists: an explicit scale, or a Fit width or Fill that goes on following the viewport.

Pointer drag, keyboard pan, auto-pan, and navigator repositioning all update the same center. Stored pan is
clamped to the same visible interval used for drawing, so reversing at an edge moves the picture immediately
rather than consuming hidden overscroll. Pan acceleration derives from displacement rather than event frequency.
Wheel fractions accumulate before producing a step, so precision devices do not lose motion.

Ctrl and drag draws a region on the picture, held in source space; its Zoom button sets the explicit scale that
fits that rectangle to the viewport, centred on it - not a ladder value, but kept within the ladder's range, or
Fit where Fit lies beyond it. A drag under the drag threshold remains a click and keeps any earlier region. While
projected, a region is hidden rather than drawn, and kept for the flat picture.

## Comparison

Each comparison pane has its own source geometry but shares an effective scale and source-space
center. The active pane supplies the anchor. Pane flip changes which item occupies each pane without
changing the compared region. Different source dimensions are reconciled in normalized source
space, not by copying scroll offsets.

## Panorama

### Standing Inside An Equirectangular Panorama

An equirectangular panorama can be viewed as flat pixels or as a camera inside the sphere. Projected mode
replaces percentage scale with a bounded field-of-view ladder, and stores yaw and pitch instead of a planar
center. Zooming in from Fit enters it; drag turns the camera, inspect zoom aims it by the pointer's position
across the viewport, stepping adjusts field of view, and stepping out past the widest leaves zoom, as stepping out
to Fit does. The navigator marks the camera's covered source region. A control in the zoom chrome shows and
switches flat or projected; leaving the projection, or asking for 100%, shows the flat picture at 100%.

Partial panoramas use GPano full dimensions and crop offsets to map stored texels onto the sphere
([metadata](metadata.md#panorama)). Longitude wraps only when the declared panorama spans the full width. A
same-path source whose dimensions change rereads declaration geometry without resetting the user's camera or
flat/projected choice; unchanged-source refreshes keep both. GPU projection uses a shader; software rendering uses
the shared panorama rasterizer and publishes generation-checked surfaces.

### Looking Around A Wide Picture

The display treats as a panorama a picture that declares one or, failing that, whose long edge is 4000 pixels or
more and at least twice its short edge. Zooming in on one from a Fit below 100% goes straight to 100%, where the
strip is legible. In zoom mode, shown flat, its center follows the resting pointer with no button held: crossing
the viewport sweeps the whole picture along each axis on which it is larger than the viewport, and the other axis
stays centred. Inspect zoom and dragging are unchanged.

## Navigator

The navigator appears when magnified and shows the visible source region. Clicking or dragging it moves the
center. The setting offers Auto-hide or Off; activity reveals the auto-hidden navigator for a bounded interval.
Panorama mode draws the field-of-view patch in the same surface.

## Input

### The Wheel

The pointer chooses the receiving surface. Over magnified media, horizontal tilt pans horizontally. During inspect
zoom, vertical stepping adjusts magnification. In durable zoom mode, Control plus the wheel steps scale while the
unmodified vertical wheel retains sequence navigation. Over a fitted picture, Control plus the wheel is consumed
rather than entering zoom mode. Elsewhere the wheel keeps the behavior of the view under the pointer.

### Pinch

Pinch on magnifiable media anchors at the gesture center and steps the same ladder. Touch pan updates the same
source-space center in Items and Fullscreen when the gesture starts on magnified media, and in Fullscreen a
double-tap toggles between Fit and 100% at the tapped point. Mouse, touch, keyboard, and menu paths do not create
separate zoom state.

## Quality And Memory

Zoom asks the [display phase ladder](file-io.md#reading-the-display-phase-ladder) for a display-sized decode; a
thumbnail or embedded preview stands in meanwhile, and a better representation is never replaced by a worse one.
Decoded images remain subject to the [rendering budgets](rendering.md#image-budgets): eviction keeps enough
encoded or cached data to rebuild without changing layout or zoom state, and GPU resource loss drops textures, not
the model's scale and center.

## Where this lives

- [model_zoom.h](../src/model_zoom.h): scale modes, ladder, source-space center, anchoring, bounds, panning,
  region fitting, and navigator timing.
- [view_media.h](../src/view_media.h), [view_items.cpp](../src/view_items.cpp)'s `media_wheel` and
  [ui_elements.cpp](../src/ui_elements.cpp): media input and presentation, the wheel, inspect zoom, panning,
  drawing a region and looking around.
- [model_display.h](../src/model_display.h): display state, inspect zoom, zoom mode, the panorama session and its
  flat/projected choice, looking around, and comparison coordination; `displays_as_panorama` in
  [model_property.h](../src/model_property.h): what is shown as a panorama.
- [model.cpp](../src/model.cpp)'s `calc_sampler` and `calc_video_sampler`: the sampler law 8 chooses.
- [ui_panorama.h](../src/ui_panorama.h) and [render_panorama.cpp](../src/render_panorama.cpp): panorama camera and
  software projection.
- libdesktop's [surface.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/core/surface.cpp): shared
  resampling.
- [app_commands.cpp](../src/app_commands.cpp): zoom commands and navigator setting.