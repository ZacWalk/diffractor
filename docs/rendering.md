# Rendering

This document owns how Diffractor's pictures reach the screen: image budgets, presentation, video
decode and hardware decode, HDR, subtitles, the software rasterisers Diffractor owns, and graphics
resilience. The draw backends are libdesktop's - Direct3D 11 and the software renderer, device and
frame lifetime, text, textures and the texture pool - and its
[render.md](https://github.com/ZacWalk/libdesktop/blob/main/docs/render.md) describes them.
[Design](design.md) owns visible behavior; [Zoom](zoom.md) owns scale, center and sampling;
[File I/O](file-io.md) owns decoded-image acquisition; and [Implementation](implementation.md) owns
thread publication.

## Backends

The view draws with Direct3D 11 where it works. Dialogs and popups draw with the software renderer,
and so does every window when GPU use is off (`-no-gpu`, which `.\dd.ps1 cpu` passes) or once the
device is lost. Both must show the same picture: a change that differs between them is incomplete
until they agree or this document states the difference. The GPU's slight dither against banding - of
solid fills, and of a picture's colours where they fall between the eight-bit target's levels - is one
intended difference; the other is the audio visualizer's bars, which
Direct3D draws as rounded blocks of coloured glass - flat-faced and bevelled, lit from the upper left,
each casting a soft shadow tinted with its colour down and to the right - where the software renderer
draws the classic bars, each raised over a soft shadow. A picture with a look of its own - the sidebar's charts, the cartridge's
shadow - is rasterised on the CPU into a surface and drawn as a texture, so it cannot differ.

Alpha animation runs only while `render::animations_enabled` allows it, and only through
`ui::animate_alpha`. Under the software renderer every transition completes at once, with the same
result.

## Image Budgets

Decoded surfaces and textures are bounded separately from encoded thumbnails and source bytes. A
source whose decode would pass the decode budget is refused before allocation
([File I/O](file-io.md#when-an-image-cannot-be-shown)); an edit's sixteen bits a channel cost twice the
bytes, and where only a byte a channel fits the edit is decoded at that, an HDR picture as its SDR rendition. A
texture's extent is fitted, aspect kept,
inside the budget the renderer publishes - the pixels a texture may cost, which libdesktop takes from
the GPU's memory, and the largest edge the device accepts - so an over-wide panorama is shown scaled
down rather than failing its upload.

Visible content has priority. Off the display, a decoded surface is kept only where it has no
compressed form to return to - HEIF, JPEG XL, PSD and a developed RAW - and those are released oldest
first beyond about the displayed image's own budget. An eviction leaves a cheaper path back, such as
an encoded image or SQLite thumbnail, and keeps layout dimensions.

Texture upload happens lazily on the UI thread, which owns the draw context; decode and scaling stay
on workers, and publication checks item identity and generation before installing a surface or
texture.

FFmpeg's still-image fallback constrains stream creation while probing, so every probe decoder it
opens inherits the application's pixel ceiling. Containers that create streams while reading packets
are checked again before decoder setup, but their probe decoder uses FFmpeg's default ceiling.

## Presentation

Decoded associated-alpha images are normalized to straight alpha before publication, so drawing and
later encoding do not multiply their colors by alpha a second time. Color transforms and image edits
are applied in the shared render layer; backends sample, compose and present, and do not reinterpret
metadata. Resources are keyed by every input that changes their pixels or geometry, and DPI, font,
theme, device and graphics resets invalidate the caches that depend on them.

A picture an edit loads at sixteen bits a channel ([File I/O](file-io.md#writing)) is uploaded at that,
as R16G16B16A16, so the Edit view previews the precision the save works at; an HDR picture's signal is previewed
as its SDR rendition, at sixteen bits, which is what its colour is edited on. The edit's colour -
libdesktop's `render::color_adjust`, whose model its
[render.md](https://github.com/ZacWalk/libdesktop/blob/main/docs/render.md#textures-video-and-vertices)
describes - is one piece of arithmetic in the shader, the software renderer and the save.

A destination is shaped by what the item is, not by what has arrived to draw into it: a stand-in
staged before the decode need not match the item's shape, and stretching it would distort the picture
until the decode landed. Video is the exception, because a decoded frame can carry non-square pixels.
There the stored frame is not the shape to fill, so the destination takes the container's declared
display dimensions and the frame is stretched into them.

## Diffractor's Rasterisers

The sidebar globe, the sidebar's file-type pie and history calendar, and a projected panorama on the
software renderer are drawn by Diffractor's own software rasterisers into surfaces uploaded like any
other image, so their pixels are the same under either backend. The globe's model, projection,
selection and hit testing are shared, so both backends show identical geometry and colors, and the
charts match the pointer against the pixels they drew. Tile and globe cache identities include
projection, dimensions, theme and source revision.

Where the backend projects panoramas, the GPU projects the frame with a shader; elsewhere the
panorama rasteriser draws the same frame from the same camera, derived once for both.
[Zoom](zoom.md#panorama) owns what the camera does.

## Subtitles

A subtitle is drawn from a bitmap made off the UI thread in the pixels of the picture as it is drawn -
turned, for a video recorded turned and shown upright - then scaled with the picture; a picture that
changes shape has its lines laid out again. Text goes through libdesktop's `render::render_styled_text`,
which lays its styled runs out with DirectWrite on Windows or Pango on Linux and composes them over a
dilated outline in straight alpha. Pictures - Blu-ray, DVD and DVB subtitles - are expanded from their
palettes, each from the canvas its stream states as it decodes, and a picture stream shows one picture
at a time: the next picture, or a clear, ends the one before it.

Sizes follow the script's canvas, so a line takes the same share of the picture at every window size.
The UI thread uploads each bitmap once while it stays up and draws it over the frame the picture last
drew. A line coming, going or finishing its rasterising rebuilds the scene once, since the lines are
recorded into it and a replay would present the old ones. Two lines up at once are stacked rather
than drawn over each other, and lines in the lower half rise clear of media controls floating over
the picture as those fade in.

## Video Decode

Video decoding runs off the UI thread, and so does preparing each picture for display: the decode
thread converts it into the texture the renderer uploads - NV12 or P010 planes where the renderer
samples YUV and the picture is 4:2:0 at an even size, with P010 only where the device samples it too,
and packed BGRA otherwise - so presenting a frame is an upload and nothing more. A prepared picture is
written over once the queue, the presenter and the renderer have all let it go, rather than allocated
again for every frame. A decoder reads only the streams it decodes; the others are discarded in the
demuxer. Closing a session, or abandoning an audio walk, interrupts a read blocked inside the demuxer
rather than waiting it out. Software decode uses frame and slice threads, bounded by the core count
and by the memory each frame thread holds.

The decode thread decodes every packet already queued while its frame queue wants frames, asking the
reader for more as soon as there is room rather than waiting on it, and the presenter taking a frame
wakes it again, so a stream with no audio never waits out a poll. The playing session and Movie's
frame sources share the thread in turns of a few packets. The presenter takes every frame its clock
has reached and shows the nearest, so content faster than the tick, or a tick that arrives late, drops
frames rather than falling behind its audio. Of several seeks waiting for the read thread, only the
latest is made. A window that cannot be seen skips drawing, but playback goes on taking frames at its
clock's pace, so the picture is current the moment the window can be seen again.

A file's timeline starts where it starts to be shown. An MP4 whose edit list starts it after a key frame,
as a copy Edit trimmed does, holds frames before that start, which are decoded and never shown, so a
seek reaches back past the start for the key frame it needs.

A picture is converted by the matrix and range its stream states, and an untagged one by the matrix
players assume (`av_video_matrix`): BT.2020's only where its primaries or transfer are BT.2020's or
HDR's, BT.709's for anything larger than SD - UHD included, since untagged 4K is ordinary SDR video far
more often than BT.2020 - and BT.601's for SD. Its chroma is read where the stream sites it - with every
other luma column for MPEG-2, H.264 and HEVC's 4:2:0 and for a stream that does not say, centred for
JPEG's - by both renderers and every swscale conversion. The info pane's stream table names the matrix,
marked assumed where the stream states none ([Selection controls](selection-controls.md#verbose-metadata-blocks)).

Interlaced video is shown a field at a time, a field period apart - fifty pictures a second from 25
frames - each made whole by a motion-adaptive deinterlacer from its own field and the fields either side
of it (`av_deinterlace_field`), so movement shows neither the comb of two instants woven together nor the
judder of one picture a frame.

Movie's bounded PCM reads place converted audio by its source timestamp minus the resampler's
buffered delay. Continuous source audio stays continuous when its sample rate changes; genuine
timestamp gaps remain gaps, and the resampler's delayed tail is drained at the end of a read. Rate,
sample-format and channel-layout changes are taken from decoded frames, and pending audio is drained
before the conversion parameters change. A seek instead resets filter history. Waveform extraction
drains delayed decoder frames at end of stream as playback does. A packet the decoder rejects costs
only its own samples; waveform and PCM extraction continue with the packets after it.

Software AV1 decodes through libdav1d, since FFmpeg has no software AV1 decoder of its own; FFmpeg's
`av1` decoder only drives hardware acceleration.

## Hardware Video

Hardware decode is chosen per stream, before the decoder is opened, and only where FFmpeg's D3D11
acceleration covers the stream and the driver reports a decoder for its profile and format. A stream
the driver still refuses decodes in software from the same context rather than producing no
pictures.

One D3D11 decode device serves every video in the process, and is replaced only once it is no longer
usable. Where the render device carries the video interfaces it is the decode device, so a picture
reaches the screen in one copy on one device. Otherwise the decode device is made on the adapter the
renderer draws with, found by that adapter's LUID, because a shared texture cannot cross adapters and
the default adapter FFmpeg would choose can change with docking or a new primary display. Where the
renderer has no adapter, or it is gone, the decode device falls back to that default.

The decoder publishes an immutable frame description, and the renderer copies its picture into a
render-owned texture under the decoder's lock - straight out of the decoder's surface on the render
device, or through a texture the two devices share - as libdesktop's render.md describes. The session
keeps the frame on show, and so its decoder surface, until the next replaces it, and the decoder's
surface pool allows for that. Session generation is verified before display.

A renderer that cannot open the shared texture, or cannot sample its format because the device has no
plane views or YUV presentation is off, asks for CPU frames of that surface layout, and so does the
software renderer when a hardware picture reaches it. From then on hardware pictures of that layout
are downloaded and prepared on the decode thread; other layouts are still shared. The renderers see a
frame only through `render::video_frame`, and convert any other picture with the application's
scaler, made by `render::make_surface_scaler`, so no renderer includes FFmpeg.

When hardware decoding, shared textures or YUV presentation are unavailable, playback falls back to
software-decoded surfaces, keeping timing, orientation, color intent and playback state. Closing or
superseding a session releases decoder resources on their owning context before the render-side
reference is discarded.

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
variants, so SDR video pays nothing. The software renderer and every CPU conversion - thumbnails,
packed frames, captures - apply the same cube with the same trilinear arithmetic, so both backends
and every thumbnail show the same picture. HDR planes are uploaded only as P010 with their cube; an
8-bit HDR stream, or any HDR on a device that samples no P010, is tone mapped into BGRA on the decode
thread instead.

## HDR Stills

A PQ or HLG still - a HEIF or AVIF its colour box marks so, a JPEG XL by its colour encoding, a PNG by
its cICP chunk - is shown as SDR by the same policy as HDR video: 203-nit white, PQ rolled off by the
BT.2390 EETF from its content light level, else its mastering display's peak, else 1000 nits, HLG at a
system gamma of 1.0 for a display whose peak is SDR white, wide gamut taken into BT.709, and a channel
past white desaturated towards its luminance. libfiles maps it as it decodes, a pixel at a time through
tables rather than a cube, which a picture's saturated highlights would show the cube's error in; and
encodes it with the sRGB curve, as stills are shown, where video takes BT.1886's.

A JPEG, HEIF or AVIF with a gain map - Ultra HDR's, Adobe's, Apple's or ISO 21496-1's - is shown as its SDR photo,
since the view presents SDR. An edit keeps the gain map for the HDR displays that show it, and keeps a PQ or HLG
still HDR: it is loaded as its signal and saved with its description, or as an SDR JPEG with a gain map made for
it ([File I/O](file-io.md#writing)).

## Colour Profiles

A still whose ICC profile describes colours other than sRGB's - a phone's Display P3, a camera's Adobe RGB
(1998), ProPhoto RGB, a grey of another gamma - is shown and thumbnailed in sRGB's: libfiles maps each pixel
as a JPEG, PNG, WebP, HEIF, AVIF or PSD decodes, through the profile's tone curves and colorants, white to
white, and clips what sRGB cannot show, as colour-managed viewers do; such a JPEG or WebP never reaches the
GPU as YUV planes. A profile libfiles cannot apply - a press's CMYK, one of lookup tables alone - leaves the
picture as it was, an HDR still is shown by its own description instead, and a TIFF's profile, which FFmpeg
decodes, is not read. The info pane's ICC block says which. An edit works in the file's own colours and
shows, and adjusts, their sRGB rendition ([File I/O](file-io.md#writing)).

## Resilience

Risky graphics capabilities are guarded by durable crash markers, and a crash is attributed to the
narrowest one. A crash during hardware video decode turns hardware decode off and keeps GPU
rendering. A crash with the GPU active and no decode in progress draws the next session in software;
the user's GPU preference is kept, and tried again once that session exits cleanly. A driver fault on
planar textures turns YUV presentation off (`render::on_yuv_fault`). A clean exit clears the markers.
Repeated unsettled starts belong to [Implementation](implementation.md#crash-loop-protection).

Device loss is expected. libdesktop rebuilds every window on the software renderer for the rest of
the session; Diffractor releases what it holds on the GPU, and view-owned graphics objects are
released when their device generation changes, whether or not the view is visible. Logical view,
selection, zoom and playback state are independent of the device and survive it, and repeated failure
never loops device creation.

## Where this lives

- [model.cpp](../src/model.cpp) and [model.h](../src/model.h): `texture_state`, the texture-budget
  fit and the retained textures it releases, and `display_state_t::draw_subtitles`, which draws
  subtitles over the picture.
- [av_format.cpp](../src/av_format.cpp): video decode, hardware decode selection, decode-thread
  picture preparation, the matrix an untagged picture is assumed to have and where its chroma lies,
  the HDR mapping, and hardware-frame publication.
- [av_deinterlace.cpp](../src/av_deinterlace.cpp): the deinterlacer that makes each field of an
  interlaced frame a whole picture.
- [platform_win_av.cpp](../src/platform_win_av.cpp): the D3D11VA decoder the renderer presents from,
  the driver's answer on whether it decodes a stream, the decode device - the render device where it
  carries the video interfaces, else one on the renderer's GPU - and the lock a hardware picture lends
  the renderer.
- [av_player.h](../src/av_player.h): the playback threads, the presenter that takes the frames its
  clock has reached, and the queue that makes only the latest waiting seek.
- [av_subtitle.cpp](../src/av_subtitle.cpp): subtitle placement, stacking and rasterising.
- [render_globe.cpp](../src/render_globe.cpp) and [ui_globe.h](../src/ui_globe.h),
  [render_charts.cpp](../src/render_charts.cpp) and [ui_charts.h](../src/ui_charts.h), and
  [render_panorama.cpp](../src/render_panorama.cpp) and [ui_panorama.h](../src/ui_panorama.h): the
  globe, the sidebar's charts, and the panorama projection.
- [app.cpp](../src/app.cpp)'s `apply_gpu_crash_guard` and [app_settings.cpp](../src/app_settings.cpp)'s
  `connect_yuv_latch`: the graphics fallbacks.
- libdesktop's [render.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/render.h):
  the draw interfaces, and the video frame and scaler interfaces the application hands the renderers;
  its [yuv_common.hlsli](https://github.com/ZacWalk/libdesktop/blob/main/src/render/shaders/yuv_common.hlsli)
  and [surface.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/core/surface.cpp) sample the
  HDR cube on the GPU and on the CPU, and its
  [common.hlsli](https://github.com/ZacWalk/libdesktop/blob/main/src/render/shaders/common.hlsli) averages a
  picture drawn small over each pixel's footprint and dithers what an eight-bit target cannot hold.
- libfiles' [hdr.cpp](https://github.com/ZacWalk/libfiles/blob/main/src/hdr.cpp): the HDR still
  mapping, and the carrying of an edit of the SDR rendition onto the signal; its
  [icc.cpp](https://github.com/ZacWalk/libfiles/blob/main/src/icc.cpp): the mapping of a profile's
  colours to sRGB, and the carrying of an edit of that rendition back into them; and
  [media_edit.cpp](../src/media_edit.cpp): an edit's resampling, at a byte a channel or sixteen bits.
