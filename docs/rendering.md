# Rendering Stack

This document owns render-backend parity, device and frame lifetime, resource budgets, text, image
and video presentation, and graphics recovery. [Design](design.md) owns visible behavior;
[Zoom](zoom.md) owns scale and center; [File I/O](file-io.md) owns decoded-image acquisition; and
[Implementation](implementation.md) owns thread publication.

## Backends

Every window draws through `ui::draw_context_device` using one of two backends:

- Direct3D 11 is the primary backend for the main window.
- The CPU software backend draws dialogs and popups and is the fallback when hardware setup fails or
  GPU use is disabled.

The window layer selects the backend and falls back to software after cleaning partial D3D state.
The software path is the fallback renderer, not D3D WARP. The Microsoft Basic Render Driver, which
is WARP offered as the default adapter when no GPU driver is working, therefore counts as no GPU,
and so does Windows 7, which has no flip-model presentation. A hardware window that cannot build its
Direct3D context switches the whole app to software as device loss does, so the animation and YUV
gates never describe a GPU that no window is drawing with.

## Backend Parity

Both backends consume the same scene and must agree on clipping, opacity, flat and gradient fills,
borders, shadows, rounded geometry, image transforms, text placement, and panorama output. A damage
rectangle limits required output but cannot affect pixels inside it. Layered windows preserve
transparency; ordinary targets begin from the same neutral clear color.

The GPU may dither solid output slightly to avoid banding. This is the only intentional visible
backend difference. Alpha animation is disabled through `ui::animations_enabled` in software mode;
callers use `ui::animate_alpha` rather than bypassing that gate.

## Device And Frame Lifetime

Shared D3D and DXGI factories live for the process, and swap chains come from the factory that owns
the device's adapter. Each hardware window owns its swap chain and size-dependent targets. Device
creation enables BGRA and video support and multithread protection: the hardware decoder decodes on
the render device where it can, and the UI renderer copies out of its surfaces from another thread. A
driver that cannot make a device with video support is given one without, and the decoder then gets a
device of its own.

A frame records draw commands, submits the retained scene, and presents. The window's tick runs on a
high-resolution periodic timer at the display's refresh period; a standard timer fires on the 15.6 ms
system tick whatever period it is given, which paced 120 and 144 Hz displays at 64 Hz and dropped a tick
of a 60 Hz display every half second. Resize keeps the old target
until a new one is ready, then replaces size-dependent resources together. A window that cannot be
seen skips drawing and presenting: one whose top-level window is minimised, or whose present was
answered with `DXGI_STATUS_OCCLUDED`, as behind the secure desktop of a UAC prompt or another user's
session. A flip-model swap chain reports neither a minimised nor a covered window, so minimising is
asked of the window itself. Visibility is answered afresh each frame, and a poll repaints the whole
window once it can be seen again, so a skipped frame never leaves stale pixels behind. The app's own
tick is not slowed, so playback goes on taking frames at its clock's pace and the picture is current
the moment the window can be seen again. Device loss releases GPU objects and requests recreation
without dropping logical view, selection, zoom, or playback state.

Resources are keyed by every input that changes their pixels or geometry. DPI, font, theme, device,
and graphics resets invalidate the dependent caches. View-owned graphics objects are released when
their device generation changes, whether or not the view is currently visible.

## Image Budgets

Decoded surfaces and textures are bounded separately from encoded thumbnails and source bytes.
Visible content has priority; off-screen GPU textures and reproducible decoded surfaces are released
before irreproducible or currently displayed data. A budget eviction must leave a cheaper path back,
such as an encoded image or SQLite thumbnail, and must preserve layout dimensions.

A displayed texture may cost a fraction of the memory its GPU allocates textures from, never more
than the shipped ceiling. That memory is a discrete card's own; an integrated part reports only the
carve-out it makes at boot as dedicated and draws on shared system memory, so for it the shared
memory counts too.

Texture upload occurs lazily on the UI thread because the draw context owns device resources. Decode
and scaling remain on workers. Publication checks item identity and generation before installing a
surface or texture.

The hardware backend recycles textures rather than creating one per upload. A released texture of the
plain kind - one level, sampled and nothing else - is kept with its views in a per-device pool of an
eighth of the texture budget and handed to the next texture of the same size and format, which
overwrites it in full. Sizes must match exactly, because a texture larger than its picture is sampled
past the picture at its edges. A texture is not handed out while a scene that a redraw can replay
still draws it, the oldest entries make room for new ones, and the pool goes with its device.

FFmpeg still-image fallback constrains stream creation during probing so every probe decoder it opens
inherits the application pixel ceiling. General AV container probing still supports formats that
create streams while reading packets; those streams are checked again before explicit decoder setup,
but their probe decoder may use FFmpeg's default ceiling until the demuxer exposes the stream without
a vendored FFmpeg patch.

## The Software Tile

The CPU backend rasterizes the damaged region in bounded BGRA tiles and hands completed tiles to a
platform present target. The rasterizer is platform-independent; only the final presentation target
names a native window. Tiles clip every primitive and use the same premultiplied-alpha convention as
the GPU path.

Text arrives as positioned glyph coverage, so the software rasterizer does not own font discovery or
shaping. A memory present target supports headless rendering without a window.

## The Sidebar Globe

The globe is rasterized into a software surface and uploaded like any other image. The globe model,
projection, selection, and hit testing are shared across backends, so software and hardware output
use identical geometry and colors. Tile and globe cache identities include projection, dimensions,
theme, and source revision.

## Text And Glyphs

The platform font layer resolves faces, fallback, shaping, metrics, and glyph coverage. Draw
backends consume positioned glyphs. Hardware rendering stores coverage in glyph atlases keyed by
font and device generation; software rendering blends the same coverage masks directly. An atlas row
is as tall as its tallest glyph, since a fallback face can draw taller than the font's own line, and
an atlas full at its size cap starts again empty rather than leaving new glyphs undrawn.

Text measurement and drawing use the same shaping result. A font, DPI, locale, or device change
invalidates both measurement and glyph resources so layout cannot describe different text from the
pixels drawn.
Both text backends report the identity pixel-space transform and one pixel per DIP to DirectWrite.
A layout that could not be created is logged and left undrawn rather than dereferenced.

## Subtitles

A subtitle is drawn from a bitmap made off the UI thread in the pixels of the picture as it is drawn -
turned, for a video recorded turned and shown upright - then scaled with the picture; a picture that
changes shape has its lines laid out again. Text goes through libdesktop's `platform::render_styled_text`,
which lays its styled runs out with DirectWrite and composes them over a dilated outline in straight
alpha; pictures - Blu-ray, DVD and DVB subtitles - are expanded from their palettes, each from the canvas
its stream states as it decodes. A picture stream shows one picture at a time: the next picture, or a
clear, ends the one before it. Sizes follow the script's canvas, so a line takes the
same share of the picture at every window size. The UI thread uploads each bitmap once while it stays up
and draws it over the frame the picture last drew. A line coming, going or finishing its rasterising
rebuilds the scene once, since the lines are recorded into it and a replay would present the old ones.
Two lines up at once are stacked rather than drawn
over each other, and lines in the lower half rise clear of media controls floating over the picture as
those fade in. Linux has no text renderer for subtitles yet, so only picture subtitles draw there.

## Images And Pixel Formats

Surfaces carry explicit color and alpha semantics. Upload maps supported RGB, YUV, and grayscale
formats to backend textures without silently changing range, matrix, orientation, or premultiplication.
Unsupported combinations convert through the shared rendering utilities before upload.
Decoded associated-alpha images are normalized to straight alpha before publication, so drawing
and subsequent encoding do not multiply their colors by alpha a second time.

Color transforms and image edits are applied in the shared render layer. Backends perform sampling,
composition, and presentation; they do not reinterpret metadata.

A destination is shaped by what the item is, not by what has arrived to draw into it: a stand-in
staged before the decode need not match the item's shape, and stretching it would distort the picture
until the decode landed. Video is the exception, because a decoded frame can carry non-square pixels.
There the stored frame is not the shape to fill, so the destination takes the container's declared
display dimensions and the frame is stretched into them.

## Video Decode

Video decoding runs off the UI thread, and so does preparing each picture for display: the decode
thread converts it into the texture the renderer uploads - NV12 or P010 planes where the renderer
samples YUV and the picture is 4:2:0 at an even size, with P010 only where the device samples it too,
and packed BGRA otherwise - so presenting a frame is an upload and nothing more. A prepared picture is
written over once the queue, the presenter and the renderer have all let it go, rather than allocated
again for every frame. A decoder reads only
the streams it decodes; the others are discarded
in the demuxer. Closing a session, or abandoning an audio walk, interrupts a read blocked inside the
demuxer rather than waiting it out. Software decode uses frame and slice threads, bounded by the
core count and by the memory each frame thread holds.

The decode thread decodes every packet already queued while its frame queue wants frames, asking the
reader for more as soon as there is room rather than waiting on it, and the presenter taking a frame
wakes it again: with nothing else to wake it, a stream with no audio ran its queue dry waiting out a
poll. The playing session and Movie's frame sources share the thread in turns of a few packets. The
presenter takes every frame its clock has reached and shows the nearest, so content faster than the
tick, or a tick that arrives late, drops frames rather than falling behind its audio. Of several seeks
waiting for the read thread, only the latest is made.

Movie's bounded PCM reads place converted audio by its source timestamp minus the resampler's
buffered delay. Continuous source audio stays continuous when its sample rate changes; genuine
timestamp gaps remain gaps, and the resampler's delayed tail is drained at the end of a read.
Rate, sample-format, and channel-layout changes are taken from decoded frames; pending audio is
drained before changing conversion parameters. A seek instead resets filter history. Waveform
extraction drains delayed decoder frames at end of stream as playback does. A packet the decoder
rejects costs only its own samples; waveform and PCM extraction continue with the packets after it.

AV1 has no FFmpeg software decoder of its own, so software AV1 decodes through libdav1d; FFmpeg's
own `av1` decoder exists only to drive hardware acceleration.

## Hardware Video

Hardware decode is chosen per stream, before the decoder is opened, and only where FFmpeg's D3D11
acceleration covers the stream and the driver reports a decoder for its profile and format. A stream
the driver still refuses decodes in software from the same context rather than producing no
pictures. One D3D11 decode device serves every video in the process, and is replaced only once it is
no longer usable. Where the render device carries the video interfaces it is the decode device, so a
picture reaches the screen in one copy on one device. Otherwise the decode device is made on the
adapter the renderer draws with, found by that adapter's LUID, because a shared texture cannot cross
adapters. Left to choose, FFmpeg would take whichever adapter
was the default when the first video opened, and docking or a new primary display can move that.
Where the renderer has no adapter, or it is gone, the decode device falls back to that default.

The decoder publishes an immutable frame description, and the UI copies its picture into a
render-owned texture under the decoder's lock. A picture decoded on the render device is copied
straight out of the decoder's surface, ordered after the decode by the device's own command stream. A
decode device of its own shares completed textures through keyed synchronization: the picture is
copied into a texture shared between the devices and from there into the render-owned texture, which
frees the shared texture for the next picture at once. The session keeps the frame on show, and so its
decoder surface, until the next replaces it, and the decoder's surface pool allows for that. Session
generation is verified before display. A renderer that cannot open the shared texture, or cannot
sample its format because the device has no plane views or YUV presentation is closed, asks for CPU
frames of that surface layout, and so does the CPU backend when a hardware picture reaches it. From
then on hardware pictures of that layout are downloaded and prepared on the decode thread; other
layouts are still shared.

The renderers see a frame only through `ui::video_frame`: the picture the decode thread prepared,
or else the decoder's texture with the lock its device takes around it, which a renderer copies only
while holding it. Any other picture is converted by the application's scaler, made by
`ui::make_surface_scaler`, so no renderer includes FFmpeg.

When hardware decoding, shared textures, or YUV presentation are unavailable, playback falls back to
software-decoded surfaces. The fallback preserves timing, orientation, color intent, and playback
state. Closing or superseding a session releases decoder resources on their owning context before
the render-side reference is discarded.

## HDR Video

PQ and HLG video is shown as the BT.709 SDR picture an SDR display can show, with 203 nits - where
BT.2408 puts HDR reference white - as SDR white. Each clip's frames share one mapping, held as a
33-sample cube over R'G'B' signal and made once per clip: swscale takes the signal to light and maps
the wide gamut into BT.709 without moving any light level. The two transfers then part:

- PQ is absolute light. The BT.2390 EETF rolls highlights off on luminance from the content's peak
  (its MaxCLL, else its mastering display's, else 1000 nits) into SDR white; midtones pass untouched.
- HLG is relative: it describes the scene, and each display renders it for its own peak. It is
  rendered by FFmpeg's BT.2100 HLG EOTF for a display whose peak is SDR white, at the system gamma of
  1.0 that EOTF uses below 1000 nits, so the signal's peak lands on white, HLG's own log segment rolls
  the highlights off, and reference white shows at about a quarter of white's light. Rendering it as
  1000-nit light and rolling that off as PQ is would squeeze phone HLG, most of which lies above
  reference white, into the top of the range. A file's mastering display is not used for HLG.

A saturated highlight that would carry one channel past white is desaturated towards its own
luminance instead of clipped, which keeps its hue. HDR10+ per-scene metadata is not used, since the
cube holds one static mapping per clip.

The cube is the parity mechanism. The GPU samples it after the YUV matrix in dedicated shader
variants, so SDR video pays nothing. The software backend and every CPU conversion - thumbnails,
packed frames, captures - apply the same cube with the same trilinear arithmetic, so both backends
and every thumbnail show the same picture. HDR planes are uploaded only as P010 with their cube; an
8-bit HDR stream, or any HDR on a device that samples no P010, is tone mapped into BGRA on the decode
thread instead.

## Resilience

Risky graphics capabilities are guarded by durable crash markers. A marker raised at the next launch
disables only the implicated capability, such as GPU rendering, hardware video decode, or direct YUV
presentation. Successful initialization clears its marker.

Runtime device removal tears down dependent resources, falls back to software where possible, and
invalidates affected views. Repeated failure must not loop device creation or leave a partially
initialized backend selected. Logical application state remains independent from device state.

## Where this lives

- libdesktop's [platform_win_d3d11.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_d3d11.cpp): device and adapter selection, image
  budgets, hardware drawing, swap-chain resources, texture upload and recycling, glyph atlases,
  video textures copied from the render device's decoder or shared from another device, and the render
  device and adapter published for the decoder.
- libdesktop's [platform_win_ui.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_ui.cpp): each window's backend choice, swap chain,
  presentation and its hidden-window standby, the frame timer, and the switch to software on device loss.
- libdesktop's [platform_win_software.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_software.cpp): CPU backend and Windows present
  target.
- libdesktop's [platform_win_font.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_font.cpp): font resolution, shaping, and glyph masks.
- libdesktop's [platform_win_wic.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_wic.cpp): text rendered into surfaces, styled
  and outlined for subtitles.
- [av_subtitle.cpp](../src/av_subtitle.cpp): subtitle placement, stacking and rasterising, and
  [model.cpp](../src/model.cpp) `display_state_t::draw_subtitles`, which draws them over the picture.
- libdesktop's [render_surface.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/render_surface.cpp) and [render_software.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render_software.h):
  shared surfaces and software rasterization, and the CPU side of the HDR cube.
- [av_format.cpp](../src/av_format.cpp): video decode, hardware decode selection, decode-thread
  picture preparation, the HDR mapping, and hardware-frame publication.
- [platform_win_av.cpp](../src/platform_win_av.cpp): the D3D11VA decoder the renderer presents
  from, the driver's answer on whether it decodes a stream, the decode device - the render device where
  it carries the video interfaces, else one on the renderer's GPU - and the lock a hardware picture lends
  the renderer.
- [av_player.h](../src/av_player.h): the playback threads, the presenter that takes the frames its clock
  has reached, and the queue that makes only the latest waiting seek.
- libdesktop's [yuv_common.hlsli](https://github.com/ZacWalk/libdesktop/blob/main/src/render/shaders/yuv_common.hlsli): YUV sampling and the GPU side of the HDR cube.
- libdesktop's [ui.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/ui.h): backend-neutral draw interfaces and presentation results, and the video frame
  and scaler interfaces the application hands the renderers.