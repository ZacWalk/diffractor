"""Headless checks for the HTML logo translation and shared Store/desktop artwork."""

import sys
import unittest

sys.dont_write_bytecode = True
from generate_icon import draw
from generate_store_assets import DiffractorLogoGenerator
from iconlib import Canvas


class LogoTests(unittest.TestCase):
    def test_saturated_square_colours(self) -> None:
        canvas = Canvas(90)
        draw(canvas)
        pixels = canvas.rgba()
        for x, y in ((30, 30), (60, 30), (60, 60), (30, 60)):
            with self.subTest(x=x, y=y):
                offset = (y * 90 + x) * 4
                colour = pixels[offset:offset + 3]
                maximum = max(colour)
                self.assertGreater(maximum, 0)
                self.assertGreaterEqual(1 - min(colour) / maximum, 0.95)

    def test_html_overlap_cycle(self) -> None:
        canvas = Canvas(90)
        draw(canvas)
        pixels = canvas.rgba()

        def colour(x: int, y: int) -> tuple[int, ...]:
            return tuple(pixels[(y * 90 + x) * 4:(y * 90 + x) * 4 + 3])

        r, g, b = colour(30, 30)
        self.assertGreater(g, max(r, b))
        r, g, b = colour(60, 30)
        self.assertGreater(b, max(r, g))
        r, g, b = colour(60, 60)
        self.assertGreater(r, max(g, b))
        r, g, b = colour(30, 60)
        self.assertGreater(min(r, g), b * 3)

    def test_html_border_and_transparency(self) -> None:
        canvas = Canvas(90)
        draw(canvas)
        pixels = canvas.rgba()
        offset = (10 * 90 + 24) * 4
        self.assertEqual(bytes((0, 127, 68, 255)), pixels[offset:offset + 4])
        self.assertEqual(0, pixels[3])
        self.assertEqual(0, pixels[(45 * 90 + 45) * 4 + 3])

    def test_store_and_desktop_use_identical_vectors(self) -> None:
        canvas = Canvas(64)
        draw(canvas)
        self.assertEqual((64, 64, canvas.rgba()), DiffractorLogoGenerator().create_logo(64))

    def test_start_tiles_keep_sixteen_percent_margins(self) -> None:
        generator = DiffractorLogoGenerator()
        width, height, pixels = generator.render_asset("Square150x150Logo.png", 100, 100, "square")
        self.assertEqual((100, 100), (width, height))
        self.assertEqual(bytes(16 * 100 * 4), pixels[:16 * 100 * 4])
        self.assertEqual(bytes(16 * 100 * 4), pixels[84 * 100 * 4:])
        self.assertTrue(any(pixels[3::4]))

    def test_app_list_icons_have_no_tile_margin(self) -> None:
        generator = DiffractorLogoGenerator()
        self.assertEqual(generator.create_logo(32),
                         generator.render_asset("AppList.targetsize-32.png", 32, 32, "square"))

    def test_wide_artwork_is_centred_without_changing_alpha(self) -> None:
        generator = DiffractorLogoGenerator()
        width, height, pixels = generator.create_wide_logo(120, 60, 1)
        self.assertEqual((120, 60), (width, height))
        _, _, logo = generator.create_logo(60)
        for row in range(60):
            offset = (row * 120 + 30) * 4
            self.assertEqual(logo[row * 60 * 4:(row + 1) * 60 * 4], pixels[offset:offset + 60 * 4])


if __name__ == "__main__":
    unittest.main()
