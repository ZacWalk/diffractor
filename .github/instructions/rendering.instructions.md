---
description: Backend parity, device lifetime, and the animation gate for the rendering and media layers.
applyTo: 'src/render_*.cpp, src/av_*.cpp, src/av_*.h'
---

# Rendering and media

Owning document: [rendering](../../docs/rendering.md). Zoom's rendering tiers are owned by
[zoom](../../docs/zoom.md).

## Two backends, one result

libdesktop-render's `platform_win_d3d11.cpp` (hardware) and `platform_win_software.cpp` (CPU) must
produce the same picture - a change to either is made in libdesktop, under its AGENTS.md. A change to one is incomplete until the other agrees or the difference is stated as
deliberate. The software backend rasterises through one fixed 512-square system-memory BGRA DIB
walked across the damaged region, so it does **not** track window size — code that assumes a
full-window buffer works on D3D and silently breaks here.

Software rendering is not a rare path: it is the fallback after a GPU or hardware-decode crash,
and it draws dialogs and bubble popups in every session.

## Animation

Alpha fades are decoration and are dropped when they would cost more than they convey. Gate on
`ui::animations_enabled` and animate alpha only through
`ui::animate_alpha`. Never bypass the gate. Under CPU rendering every transition completes
immediately — same result, sooner — and no state, target, or availability may differ between the
animated and immediate presentation.

## Device lifetime

Device loss is expected, not exceptional. Anything cached against the device must be rebuildable.
Read `df::gpu_perf` creation totals when reasoning about caching: views, targets, textures and
buffers should flatten early, and any of them keeping pace with the frame count means something is
being rebuilt per frame.

## Classification

A rendering change is almost never `Internal`. If the pixels, the timing, or a command's
availability differ, the AGENTS.md gate wants `User-visible behavior`. "It is only the renderer"
is the most common misclassification in this repository.

## Checks

`/test:*surface*`, `/test:*colour*`, `/test:*video*`, `/test:*audio*`. The suite has no
`ui::draw_context`, so anything whose only output is pixels on a real device is verified by eye —
run `.\dd.ps1 cpu` as well as a normal run when a change touches both backends.
