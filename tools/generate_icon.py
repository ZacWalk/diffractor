"""Diffractor's four interlocking squares, translated from the square canvas in logo.html."""

import math
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from iconlib import Canvas, Colour, Paint, generate

ROOT = Path(__file__).resolve().parents[1]


def draw(canvas: Canvas) -> None:
    """Use logo.html's geometry, clipping and radial gradients with a saturated palette."""
    def square(x: int, y: int, colour: Colour, clips: tuple[tuple[int, int, int, int], ...]) -> None:
        dark = tuple(channel - math.floor(channel * 0.33 + 0.5) for channel in colour)
        border = tuple(channel // 2 for channel in colour)

        def gradient(px: float, py: float) -> Colour:
            dx, dy = px * 9 - (x + 160), py * 9 - (y + 160)
            a, b, c = -80900, -80 * (dx + dy) - 5800, dx * dx + dy * dy - 100
            t = max(0.0, min(1.0, (-b - math.sqrt(max(0.0, b * b - 4 * a * c))) / (2 * a)))
            return tuple(math.floor(start + (end - start) * t + 0.5)
                         for start, end in zip(colour, dark))

        def clipped_rect(left: int, top: int, right: int, bottom: int, paint: Paint) -> None:
            for cx, cy, cr, cb in clips:
                l, t, r, b = max(left, cx), max(top, cy), min(right, cr), min(bottom, cb)
                canvas.rect(l / 9, t / 9, (r - l) / 9, (b - t) / 9, paint)

        clipped_rect(x - 10, y - 10, x + 410, y + 410, border)
        clipped_rect(x + 10, y + 10, x + 390, y + 390, gradient)

    whole = ((0, 0, 900, 900),)
    square(250, 10, (0x00, 0xFF, 0x88), whole)
    square(490, 250, (0x00, 0x88, 0xFF), whole)
    square(250, 490, (0xFF, 0x00, 0x20), whole)
    square(10, 250, (0xFF, 0xDD, 0x00), ((0, 0, 240, 900), (240, 420, 900, 910)))


if __name__ == "__main__":
    raise SystemExit(generate(draw, ROOT / "src/Res/app.ico", {
        ROOT / "installer/Diffractor.ico": 256,
        ROOT / "src/Res/app.png": 512,
        ROOT / "src/Res/app-icon.bmp": 128,
        ROOT / "src/Res/logo.png": 60,
        ROOT / "src/Res/logo30.png": 30,
        ROOT / "src/Res/logo15.png": 15,
    }))
