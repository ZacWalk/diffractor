"""Store artwork uses the same vector source as the desktop icon, without Pillow."""

import os
import sys
import argparse
from pathlib import Path

sys.dont_write_bytecode = True
from generate_icon import draw
from iconlib import Canvas, ICON_SIZES, ico, png

Image = tuple[int, int, bytes]

class DiffractorLogoGenerator:
    def __init__(self, check=False):
        self.check = check
        self.failures = 0
        self._logos = {}

    def create_logo(self, canvas_size) -> Image:
        if canvas_size not in self._logos:
            canvas = Canvas(canvas_size)
            draw(canvas)
            self._logos[canvas_size] = (canvas_size, canvas_size, canvas.rgba())
        return self._logos[canvas_size]

    def _place(self, width, height, content_ratio) -> Image:
        size = max(1, int(min(width, height) * content_ratio))
        _, _, logo = self.create_logo(size)
        pixels = bytearray(width * height * 4)
        x, y = (width - size) // 2, (height - size) // 2
        for row in range(size):
            offset = ((y + row) * width + x) * 4
            pixels[offset:offset + size * 4] = logo[row * size * 4:(row + 1) * size * 4]
        return width, height, bytes(pixels)

    def create_wide_logo(self, width, height, content_ratio=0.9) -> Image:
        return self._place(width, height, content_ratio)

    def create_tile_logo(self, canvas_size, content_ratio) -> Image:
        return self._place(canvas_size, canvas_size, content_ratio)

    def _write(self, path, content):
        path = Path(path)
        previous = path.read_bytes() if path.exists() else None
        if self.check:
            if previous != content:
                print(f"STALE: {path}")
                self.failures += 1
        elif previous != content:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

    def _save(self, image: Image, path):
        width, height, pixels = image
        self._write(path, png(width, pixels, height))

    # Start tiles must not be filled edge to edge; Windows asks for at least a 16% margin per
    # side. Taskbar/app-list/Store/file icons are the opposite - they are meant to be full bleed.
    TILE_ASSETS = ("Square150x150Logo", "SmallTile", "LargeTile", "Wide310x150Logo")
    TILE_CONTENT_RATIO = 0.68

    def render_asset(self, filename, w, h, method):
        is_tile = filename.startswith(self.TILE_ASSETS)
        if method == "square":
            if is_tile:
                return self.create_tile_logo(w, self.TILE_CONTENT_RATIO)
            return self.create_logo(w)
        ratio = self.TILE_CONTENT_RATIO if is_tile else 0.9
        return self.create_wide_logo(w, h, ratio)

    def generate_windows_assets(self, output_dir="DiffractorAssets"):
        # Standard Windows App SDK / UWP Asset List
        # Format: (Filename, Width, Height, Method)
        assets = [
            # Store Logo (required for Microsoft Store)
            ("StoreLogo.scale-100.png", 50, 50, "square"),
            ("StoreLogo.scale-125.png", 63, 63, "square"),
            ("StoreLogo.scale-150.png", 75, 75, "square"),
            ("StoreLogo.scale-200.png", 100, 100, "square"),
            ("StoreLogo.scale-400.png", 200, 200, "square"),

            # App Icon (Square44x44 - used in Start Menu lists, Taskbar)
            # Scale variants
            ("Square44x44Logo.scale-100.png", 44, 44, "square"),
            ("Square44x44Logo.scale-125.png", 55, 55, "square"),
            ("Square44x44Logo.scale-150.png", 66, 66, "square"),
            ("Square44x44Logo.scale-200.png", 88, 88, "square"),
            ("Square44x44Logo.scale-400.png", 176, 176, "square"),
            # Target size variants (required)
            ("Square44x44Logo.targetsize-16.png", 16, 16, "square"),
            ("Square44x44Logo.targetsize-20.png", 20, 20, "square"),
            ("Square44x44Logo.targetsize-24.png", 24, 24, "square"),
            ("Square44x44Logo.targetsize-30.png", 30, 30, "square"),
            ("Square44x44Logo.targetsize-32.png", 32, 32, "square"),
            ("Square44x44Logo.targetsize-36.png", 36, 36, "square"),
            ("Square44x44Logo.targetsize-40.png", 40, 40, "square"),
            ("Square44x44Logo.targetsize-48.png", 48, 48, "square"),
            ("Square44x44Logo.targetsize-60.png", 60, 60, "square"),
            ("Square44x44Logo.targetsize-64.png", 64, 64, "square"),
            ("Square44x44Logo.targetsize-72.png", 72, 72, "square"),
            ("Square44x44Logo.targetsize-80.png", 80, 80, "square"),
            ("Square44x44Logo.targetsize-96.png", 96, 96, "square"),
            ("Square44x44Logo.targetsize-256.png", 256, 256, "square"),
            # Unplated versions for dark theme taskbar (required for transparent background)
            ("Square44x44Logo.targetsize-16_altform-unplated.png", 16, 16, "square"),
            ("Square44x44Logo.targetsize-20_altform-unplated.png", 20, 20, "square"),
            ("Square44x44Logo.targetsize-24_altform-unplated.png", 24, 24, "square"),
            ("Square44x44Logo.targetsize-30_altform-unplated.png", 30, 30, "square"),
            ("Square44x44Logo.targetsize-32_altform-unplated.png", 32, 32, "square"),
            ("Square44x44Logo.targetsize-36_altform-unplated.png", 36, 36, "square"),
            ("Square44x44Logo.targetsize-40_altform-unplated.png", 40, 40, "square"),
            ("Square44x44Logo.targetsize-48_altform-unplated.png", 48, 48, "square"),
            ("Square44x44Logo.targetsize-60_altform-unplated.png", 60, 60, "square"),
            ("Square44x44Logo.targetsize-64_altform-unplated.png", 64, 64, "square"),
            ("Square44x44Logo.targetsize-72_altform-unplated.png", 72, 72, "square"),
            ("Square44x44Logo.targetsize-80_altform-unplated.png", 80, 80, "square"),
            ("Square44x44Logo.targetsize-96_altform-unplated.png", 96, 96, "square"),
            ("Square44x44Logo.targetsize-256_altform-unplated.png", 256, 256, "square"),
            # Light unplated for light theme taskbar (required for transparent background)
            ("Square44x44Logo.targetsize-16_altform-lightunplated.png", 16, 16, "square"),
            ("Square44x44Logo.targetsize-20_altform-lightunplated.png", 20, 20, "square"),
            ("Square44x44Logo.targetsize-24_altform-lightunplated.png", 24, 24, "square"),
            ("Square44x44Logo.targetsize-30_altform-lightunplated.png", 30, 30, "square"),
            ("Square44x44Logo.targetsize-32_altform-lightunplated.png", 32, 32, "square"),
            ("Square44x44Logo.targetsize-36_altform-lightunplated.png", 36, 36, "square"),
            ("Square44x44Logo.targetsize-40_altform-lightunplated.png", 40, 40, "square"),
            ("Square44x44Logo.targetsize-48_altform-lightunplated.png", 48, 48, "square"),
            ("Square44x44Logo.targetsize-60_altform-lightunplated.png", 60, 60, "square"),
            ("Square44x44Logo.targetsize-64_altform-lightunplated.png", 64, 64, "square"),
            ("Square44x44Logo.targetsize-72_altform-lightunplated.png", 72, 72, "square"),
            ("Square44x44Logo.targetsize-80_altform-lightunplated.png", 80, 80, "square"),
            ("Square44x44Logo.targetsize-96_altform-lightunplated.png", 96, 96, "square"),
            ("Square44x44Logo.targetsize-256_altform-lightunplated.png", 256, 256, "square"),

            # AppList icons (Taskbar, Start pins, all-apps list, search results).
            # Windows resolves these against the Square44x44Logo entries above, so strictly
            # these are redundant - but the icon docs list them as required by literal name and
            # shell icon resolution has bitten us before, so keep both spellings.
            ("AppList.targetsize-16.png", 16, 16, "square"),
            ("AppList.targetsize-20.png", 20, 20, "square"),
            ("AppList.targetsize-24.png", 24, 24, "square"),
            ("AppList.targetsize-30.png", 30, 30, "square"),
            ("AppList.targetsize-32.png", 32, 32, "square"),
            ("AppList.targetsize-36.png", 36, 36, "square"),
            ("AppList.targetsize-40.png", 40, 40, "square"),
            ("AppList.targetsize-48.png", 48, 48, "square"),
            ("AppList.targetsize-60.png", 60, 60, "square"),
            ("AppList.targetsize-64.png", 64, 64, "square"),
            ("AppList.targetsize-72.png", 72, 72, "square"),
            ("AppList.targetsize-80.png", 80, 80, "square"),
            ("AppList.targetsize-96.png", 96, 96, "square"),
            ("AppList.targetsize-256.png", 256, 256, "square"),
            # Dark theme unplated (avoids the system icon backplate on the taskbar)
            ("AppList.targetsize-16_altform-unplated.png", 16, 16, "square"),
            ("AppList.targetsize-20_altform-unplated.png", 20, 20, "square"),
            ("AppList.targetsize-24_altform-unplated.png", 24, 24, "square"),
            ("AppList.targetsize-30_altform-unplated.png", 30, 30, "square"),
            ("AppList.targetsize-32_altform-unplated.png", 32, 32, "square"),
            ("AppList.targetsize-36_altform-unplated.png", 36, 36, "square"),
            ("AppList.targetsize-40_altform-unplated.png", 40, 40, "square"),
            ("AppList.targetsize-48_altform-unplated.png", 48, 48, "square"),
            ("AppList.targetsize-60_altform-unplated.png", 60, 60, "square"),
            ("AppList.targetsize-64_altform-unplated.png", 64, 64, "square"),
            ("AppList.targetsize-72_altform-unplated.png", 72, 72, "square"),
            ("AppList.targetsize-80_altform-unplated.png", 80, 80, "square"),
            ("AppList.targetsize-96_altform-unplated.png", 96, 96, "square"),
            ("AppList.targetsize-256_altform-unplated.png", 256, 256, "square"),
            # Light theme unplated
            ("AppList.targetsize-16_altform-lightunplated.png", 16, 16, "square"),
            ("AppList.targetsize-20_altform-lightunplated.png", 20, 20, "square"),
            ("AppList.targetsize-24_altform-lightunplated.png", 24, 24, "square"),
            ("AppList.targetsize-30_altform-lightunplated.png", 30, 30, "square"),
            ("AppList.targetsize-32_altform-lightunplated.png", 32, 32, "square"),
            ("AppList.targetsize-36_altform-lightunplated.png", 36, 36, "square"),
            ("AppList.targetsize-40_altform-lightunplated.png", 40, 40, "square"),
            ("AppList.targetsize-48_altform-lightunplated.png", 48, 48, "square"),
            ("AppList.targetsize-60_altform-lightunplated.png", 60, 60, "square"),
            ("AppList.targetsize-64_altform-lightunplated.png", 64, 64, "square"),
            ("AppList.targetsize-72_altform-lightunplated.png", 72, 72, "square"),
            ("AppList.targetsize-80_altform-lightunplated.png", 80, 80, "square"),
            ("AppList.targetsize-96_altform-lightunplated.png", 96, 96, "square"),
            ("AppList.targetsize-256_altform-lightunplated.png", 256, 256, "square"),
            # Scale variants (Windows 10)
            ("AppList.scale-100.png", 44, 44, "square"),
            ("AppList.scale-125.png", 55, 55, "square"),
            ("AppList.scale-150.png", 66, 66, "square"),
            ("AppList.scale-200.png", 88, 88, "square"),
            ("AppList.scale-400.png", 176, 176, "square"),

            # Medium Tile (Square150x150)
            ("Square150x150Logo.scale-100.png", 150, 150, "square"),
            ("Square150x150Logo.scale-125.png", 188, 188, "square"),
            ("Square150x150Logo.scale-150.png", 225, 225, "square"),
            ("Square150x150Logo.scale-200.png", 300, 300, "square"),
            ("Square150x150Logo.scale-400.png", 600, 600, "square"),

            # Large Tile (Square310x310)
            ("LargeTile.scale-100.png", 310, 310, "square"),
            ("LargeTile.scale-125.png", 388, 388, "square"),
            ("LargeTile.scale-150.png", 465, 465, "square"),
            ("LargeTile.scale-200.png", 620, 620, "square"),
            ("LargeTile.scale-400.png", 1240, 1240, "square"),

            # Small Tile (Square71x71)
            ("SmallTile.scale-100.png", 71, 71, "square"),
            ("SmallTile.scale-125.png", 89, 89, "square"),
            ("SmallTile.scale-150.png", 107, 107, "square"),
            ("SmallTile.scale-200.png", 142, 142, "square"),
            ("SmallTile.scale-400.png", 284, 284, "square"),

            # Wide Tile (Wide310x150)
            ("Wide310x150Logo.scale-100.png", 310, 150, "wide"),
            ("Wide310x150Logo.scale-125.png", 388, 188, "wide"),
            ("Wide310x150Logo.scale-150.png", 465, 225, "wide"),
            ("Wide310x150Logo.scale-200.png", 620, 300, "wide"),
            ("Wide310x150Logo.scale-400.png", 1240, 600, "wide"),

            # Splash Screen
            ("SplashScreen.scale-100.png", 620, 300, "wide"),
            ("SplashScreen.scale-125.png", 775, 375, "wide"),
            ("SplashScreen.scale-150.png", 930, 450, "wide"),
            ("SplashScreen.scale-200.png", 1240, 600, "wide"),
            ("SplashScreen.scale-400.png", 2480, 1200, "wide"),

            # File type association icon
            ("DiffractorFile.scale-100.png", 44, 44, "square"),
            ("DiffractorFile.scale-125.png", 55, 55, "square"),
            ("DiffractorFile.scale-150.png", 66, 66, "square"),
            ("DiffractorFile.scale-200.png", 88, 88, "square"),
            ("DiffractorFile.scale-400.png", 176, 176, "square"),
        ]

        print(f"{'Checking' if self.check else 'Generating'} assets in '{output_dir}'...")

        for filename, w, h, method in assets:
            img = self.render_asset(filename, w, h, method)
            save_path = os.path.join(output_dir, filename)
            self._save(img, save_path)

        # Also generate base-named assets for Windows manifest compatibility
        # Windows looks for these when the manifest references e.g., "Assets\StoreLogo.png"
        base_assets = [
            ("StoreLogo.png", 50, 50, "square"),
            ("Square44x44Logo.png", 44, 44, "square"),
            ("Square150x150Logo.png", 150, 150, "square"),
            ("Wide310x150Logo.png", 310, 150, "wide"),
            ("SmallTile.png", 71, 71, "square"),
            ("LargeTile.png", 310, 310, "square"),
            ("SplashScreen.png", 620, 300, "wide"),
            ("DiffractorFile.png", 44, 44, "square"),
        ]

        for filename, w, h, method in base_assets:
            img = self.render_asset(filename, w, h, method)
            save_path = os.path.join(output_dir, filename)
            self._save(img, save_path)

        print(f"{len(assets) + len(base_assets)} Store assets; {self.failures} stale")

    # Windows picks the nearest frame and scales down, so 256 is included to avoid ever scaling up.
    APP_ICON_SIZES = ICON_SIZES
    APP_LOGOS = (("logo.png", 60), ("logo30.png", 30), ("logo15.png", 15))

    def generate_desktop_assets(self, res_dir):
        # Each frame is drawn at its target size rather than resampled from one large bitmap,
        # so the small frames keep the supersampled edges instead of a second round of scaling.
        frames = {size: self.create_logo(size)[2] for size in self.APP_ICON_SIZES}
        ico_path = os.path.join(res_dir, "app.ico")
        self._write(ico_path, ico(frames))

        for filename, size in self.APP_LOGOS:
            self._save(self.create_logo(size), os.path.join(res_dir, filename))

        print(f"{1 + len(self.APP_LOGOS)} desktop resources; {self.failures} stale")


def main():
    parser = argparse.ArgumentParser(
        description="Generate Windows Store assets for Diffractor"
    )
    parser.add_argument(
        "-o", "--output",
        default="DiffractorAssets",
        help="Output directory for generated assets (default: DiffractorAssets)"
    )
    parser.add_argument(
        "--res",
        help="Instead of Store assets, write the desktop app.ico and logo PNGs to this "
             "resource folder (e.g. src/Res)"
    )
    parser.add_argument("--check", action="store_true", help="compare regenerated bytes without writing files")
    args = parser.parse_args()

    generator = DiffractorLogoGenerator(check=args.check)
    if args.res:
        generator.generate_desktop_assets(args.res)
    else:
        generator.generate_windows_assets(args.output)
    return int(generator.failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())