# This file is part of libdesktop, a C++20 platform layer for desktop applications
# Copyright 2026 Zac Walker
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Small, standard-library-only vector rasteriser and desktop icon writer.

The application generators vendor this file so regeneration needs no sibling
checkout, downloaded code, font, graphics library or display server.
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path
from typing import Callable, Sequence, Union

Colour = tuple[int, int, int]
Point = tuple[float, float]
Paint = Union[Colour, Callable[[float, float], Colour]]
ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


class Canvas:
    """Opaque vector primitives on transparent RGBA, with 4x area antialiasing."""

    def __init__(self, size: int, view: float = 100.0) -> None:
        if not 1 <= size <= 2048 or not math.isfinite(view) or view <= 0:
            raise ValueError("size must be 1..2048 and view must be positive")
        self.size = size
        self.view = view
        self.width = size * 4
        self.scale = self.width / view
        self.pixels = bytearray(self.width * self.width * 4)

    def _span(self, y: int, left: float, right: float, paint: Paint) -> None:
        start = max(0, math.ceil(left - 0.5))
        end = min(self.width, math.ceil(right - 0.5))
        offset = (y * self.width + start) * 4
        if start >= end:
            return
        if callable(paint):
            for x in range(start, end):
                colour = paint((x + 0.5) / self.scale, (y + 0.5) / self.scale)
                self.pixels[offset:offset + 4] = bytes((*colour, 255))
                offset += 4
        else:
            self.pixels[offset:offset + (end - start) * 4] = bytes((*paint, 255)) * (end - start)

    def polygon(self, points: Sequence[Point], paint: Paint) -> None:
        if len(points) < 3:
            raise ValueError("a polygon needs at least three points")
        vertices = [(x * self.scale, y * self.scale) for x, y in points]
        first = max(0, math.ceil(min(y for _, y in vertices) - 0.5))
        last = min(self.width, math.ceil(max(y for _, y in vertices) - 0.5))
        edges = list(zip(vertices, vertices[1:] + vertices[:1]))
        for y in range(first, last):
            sample = y + 0.5
            crossings = sorted(
                x1 + (sample - y1) * (x2 - x1) / (y2 - y1)
                for (x1, y1), (x2, y2) in edges
                if min(y1, y2) <= sample < max(y1, y2)
            )
            for i in range(0, len(crossings), 2):
                self._span(y, crossings[i], crossings[i + 1], paint)

    def rect(self, x: float, y: float, width: float, height: float, paint: Paint) -> None:
        if width > 0 and height > 0:
            self.polygon(((x, y), (x + width, y), (x + width, y + height), (x, y + height)), paint)

    def circle(self, x: float, y: float, radius: float, paint: Paint) -> None:
        if radius <= 0:
            return
        cx, cy, r = x * self.scale, y * self.scale, radius * self.scale
        first = max(0, math.ceil(cy - r - 0.5))
        last = min(self.width, math.ceil(cy + r - 0.5))
        for row in range(first, last):
            half = math.sqrt(max(0.0, r * r - (row + 0.5 - cy) ** 2))
            self._span(row, cx - half, cx + half, paint)

    def line(self, points: Sequence[Point], width: float, paint: Paint) -> None:
        if width <= 0:
            return
        for (x1, y1), (x2, y2) in zip(points, points[1:]):
            length = math.hypot(x2 - x1, y2 - y1)
            if length:
                dx, dy = (y2 - y1) * width / (2 * length), (x2 - x1) * width / (2 * length)
                self.polygon(((x1 - dx, y1 + dy), (x2 - dx, y2 + dy),
                              (x2 + dx, y2 - dy), (x1 + dx, y1 - dy)), paint)
        for x, y in points:
            self.circle(x, y, width / 2, paint)

    def rgba(self) -> bytes:
        result = bytearray(self.size * self.size * 4)
        for y in range(self.size):
            for x in range(self.size):
                red = green = blue = covered = 0
                for sy in range(4):
                    offset = ((y * 4 + sy) * self.width + x * 4) * 4
                    for sx in range(4):
                        index = offset + sx * 4
                        if self.pixels[index + 3]:
                            red += self.pixels[index]
                            green += self.pixels[index + 1]
                            blue += self.pixels[index + 2]
                            covered += 1
                if covered:
                    offset = (y * self.size + x) * 4
                    result[offset:offset + 4] = bytes((
                        (red + covered // 2) // covered, (green + covered // 2) // covered,
                        (blue + covered // 2) // covered, (covered * 255 + 8) // 16))
        return bytes(result)


def png(size: int, rgba: bytes, height: int | None = None) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    height = size if height is None else height
    rows = b"".join(b"\0" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", size, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def bgra(rgba: bytes) -> bytes:
    result = bytearray(rgba)
    result[0::4], result[2::4] = rgba[2::4], rgba[0::4]
    return bytes(result)


def bmp(size: int, rgba: bytes) -> bytes:
    # Explicit V4 alpha masks let SDL read transparency without a PNG decoder.
    pixels = bgra(rgba)
    header = struct.pack("<IiiHHIIiiII", 108, size, -size, 1, 32, 3, len(pixels), 0, 0, 0, 0)
    header += struct.pack("<5I", 0xFF0000, 0xFF00, 0xFF, 0xFF000000, 0x73524742) + bytes(48)
    return struct.pack("<2sIHHI", b"BM", 14 + len(header) + len(pixels), 0, 0, 122) + header + pixels


def ico(images: dict[int, bytes]) -> bytes:
    entries, payloads = [], []
    offset = 6 + 16 * len(images)
    for size, rgba in sorted(images.items()):
        if not 1 <= size <= 256:
            raise ValueError("ICO sizes must be 1..256")
        if size == 256:
            payload = png(size, rgba)
        else:
            stride = ((size + 31) // 32) * 4
            mask = bytearray(stride * size)
            for y in range(size):
                for x in range(size):
                    if rgba[(y * size + x) * 4 + 3] == 0:
                        mask[(size - 1 - y) * stride + x // 8] |= 0x80 >> (x % 8)
            pixels = bgra(rgba)
            rows = b"".join(pixels[y * size * 4:(y + 1) * size * 4] for y in reversed(range(size)))
            payload = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                                  len(rows) + len(mask), 0, 0, 0, 0) + rows + mask
        entries.append(struct.pack("<4B2H2I", size % 256, size % 256, 0, 0, 1, 32, len(payload), offset))
        payloads.append(payload)
        offset += len(payload)
    return struct.pack("<3H", 0, 1, len(entries)) + b"".join(entries + payloads)


def generate(draw: Callable[[Canvas], None], default: Path, extra: dict[Path, int] | None = None,
             variants: dict[str, Callable[[Canvas], None]] | None = None) -> int:
    parser = argparse.ArgumentParser(description=draw.__doc__)
    parser.add_argument("--check", action="store_true", help="compare regenerated bytes without writing files")
    parser.add_argument("--output", type=Path, help="write one .ico, .png or .bmp instead of the default resources")
    parser.add_argument("--size", type=int, default=256, help="PNG/BMP extent, 1..2048 (default: 256)")
    parser.add_argument("--sizes", type=int, nargs="+", default=ICON_SIZES, help="ICO extents, 1..256")
    parser.add_argument("--variant", choices=("app", *(variants or {})), default="app")
    args = parser.parse_args()
    if not 1 <= args.size <= 2048 or not args.sizes or any(not 1 <= n <= 256 for n in args.sizes):
        parser.error("PNG/BMP size must be 1..2048; ICO sizes must be 1..256")
    outputs = {args.output: args.size} if args.output else {default: args.size, **(extra or {})}
    if any(path.suffix.lower() not in (".ico", ".png", ".bmp") for path in outputs):
        parser.error("output extension must be .ico, .png or .bmp")
    if args.variant != "app":
        if not args.output:
            parser.error("--variant requires --output so application resources are not replaced")
        draw = (variants or {})[args.variant]
    cache: dict[int, bytes] = {}

    def render(size: int) -> bytes:
        if size not in cache:
            canvas = Canvas(size)
            draw(canvas)
            cache[size] = canvas.rgba()
        return cache[size]

    failures = 0
    for path, size in outputs.items():
        suffix = path.suffix.lower()
        content = ico({n: render(n) for n in sorted(set(args.sizes))}) if suffix == ".ico" else (
            png(size, render(size)) if suffix == ".png" else bmp(size, render(size)))
        current = path.read_bytes() if path.exists() else None
        if args.check:
            if current != content:
                print(f"STALE: {path}")
                failures += 1
        elif current != content:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    print(f"{'Checked' if args.check else 'Generated'} {len(outputs)} icon resource(s), "
          f"{len(set(args.sizes))} ICO sizes; {failures} stale")
    return int(failures != 0)
