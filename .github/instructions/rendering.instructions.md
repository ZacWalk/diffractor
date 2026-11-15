---
description: Backend parity, the animation gate and device lifetime for Diffractor's rasterisers and media layer.
applyTo: 'src/render_*.cpp, src/av_*.cpp, src/av_*.h'
---

# Rendering and media

Owning document: [rendering](../../docs/rendering.md). Zoom's rendering tiers are owned by
[zoom](../../docs/zoom.md). The draw backends are libdesktop's, and a change to one is made there,
under its AGENTS.md.

## Two backends, one result

libdesktop's Direct3D 11 renderer
([d3d11.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/win/d3d11.cpp)) and software
renderer ([software_context.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/software_context.cpp))
must produce the same picture from what these files hand them. A change whose result differs between
them is incomplete until they agree or [rendering](../../docs/rendering.md) states the difference.
The software renderer rasterises through one fixed 512-square system-memory BGRA buffer walked across
the damaged region, so it does **not** track window size: code that assumes a full-window buffer
works on D3D and silently breaks there.

Software rendering is not a rare path: it draws every window after a GPU crash or device loss, and
dialogs and bubble popups in every session.

## Animation

Alpha fades are decoration and are dropped when they would cost more than they convey. Gate on
`render::animations_enabled` and animate alpha only through `ui::animate_alpha`. Never bypass the
gate. Under CPU rendering every transition completes immediately — same result, sooner — and no
state, target, or availability may differ between the animated and immediate presentation.

## Device lifetime

Device loss is expected, not exceptional. Anything cached against the device must be rebuildable.
Read `render::gpu_perf` creation totals when reasoning about caching: views, targets, textures and
buffers should flatten early, and any of them keeping pace with the frame count means something is
being rebuilt per frame.

## Classification

A rendering change is almost never internal. If the pixels, the timing, or a command's availability
differ, it is a [behavior change](../../AGENTS.md#behavior-changes) and its owning document must say
so. "It is only the renderer" is the most common misjudgement in this repository.

## Checks

`/test:*surface*`, `/test:*colour*`, `/test:*video*`, `/test:*audio*`, after a Debug build with
`python tools\dd.py build`. The suite has no `render::draw_context_device`, so anything whose only
output is pixels on a real device is verified by eye — run `.\dd.ps1 cpu` as well as a normal run
when a change touches both backends.
