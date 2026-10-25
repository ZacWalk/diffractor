#!/usr/bin/env python3
# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: write the fixture images for the formats Diffractor decodes but cannot encode, so the
# decode path has something to be held to. Run from the repository root with the tools venv:
#   tools\.venv\Scripts\python.exe tools\make_test_images.py
#
# The content is deliberately not flat colour: a gradient plus a marked corner catches a decoder
# that transposes, mirrors or drops a channel, which a solid block would pass.

import pathlib
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("PIL is missing - use tools\\.venv\\Scripts\\python.exe")

WIDTH = 32
HEIGHT = 24
# Deliberately beside the test folder rather than inside it: that folder's item count is asserted by
# the index tests, so anything added to it has to be accounted for in expected_cached_item_count.
DESTINATION = pathlib.Path(__file__).resolve().parent.parent / "exe" / "test-formats"

# name -> (PIL format, mode). Each is a format the reader supports and the writer does not.
TARGETS = {
    "gradient.bmp": ("BMP", "RGB"),
    "gradient.tga": ("TGA", "RGB"),
    "gradient.sgi": ("SGI", "RGB"),
    "gradient.pcx": ("PCX", "RGB"),
    "gradient.ppm": ("PPM", "RGB"),
    "gradient.pgm": ("PPM", "L"),
}


def build() -> Image.Image:
    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()

    for y in range(HEIGHT):
        for x in range(WIDTH):
            pixels[x, y] = (x * 255 // (WIDTH - 1), y * 255 // (HEIGHT - 1), 128)

    # Top-left is pure red, so an upside-down or mirrored decode is visible rather than plausible.
    for y in range(4):
        for x in range(4):
            pixels[x, y] = (255, 0, 0)

    return image


def main() -> None:
    DESTINATION.mkdir(parents=True, exist_ok=True)
    source = build()

    for name, (fmt, mode) in TARGETS.items():
        path = DESTINATION / name
        source.convert(mode).save(path, fmt)
        print(f"{path.relative_to(DESTINATION.parent.parent.parent)}  {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
