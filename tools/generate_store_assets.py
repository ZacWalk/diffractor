import os
import sys
import argparse
from PIL import Image, ImageDraw

class DiffractorLogoGenerator:
    def __init__(self):
        # Official Colors (Approximate based on "Diffractor" visual identity)
        # You can tweak these hex codes to match your exact brand colors
        self.colors = {
            'green':  {'fill': '#00b050', 'border': '#008a3e'}, # Top
            'blue':   {'fill': '#0070c0', 'border': '#005490'}, # Right
            'red':    {'fill': '#c00000', 'border': '#900000'}, # Bottom
            'yellow': {'fill': '#ffc000', 'border': '#bf9000'}, # Left
        }
        self.bg_color = (255, 255, 255, 0) # Transparent background

    def draw_square(self, draw, center_x, center_y, size, color_key, crop_mask=None):
        """
        Draws a single square with a border.
        """
        half = size / 2
        x0, y0 = center_x - half, center_y - half
        x1, y1 = center_x + half, center_y + half
        
        fill = self.colors[color_key]['fill']
        border = self.colors[color_key]['border']
        border_width = max(1, int(size * 0.08)) # 8% border thickness

        # Draw the main square
        draw.rectangle([x0, y0, x1, y1], fill=fill, outline=border, width=border_width)

    def draw_partial_square(self, draw, center_x, center_y, size, color_key, region):
        """
        Draws only a specific quadrant of a square (used to create the interlock effect).
        region: 'bottom-right', 'bottom-left', etc.
        """
        half = size / 2
        x0, y0 = center_x - half, center_y - half
        x1, y1 = center_x + half, center_y + half
        
        fill = self.colors[color_key]['fill']
        border = self.colors[color_key]['border']
        border_width = max(1, int(size * 0.08))
        
        # Calculate the crop box for the specific quadrant
        # We draw the full square onto a temp image then crop it
        # This is easier than calculating path coordinates for the border
        temp_img = Image.new('RGBA', (int(size*2), int(size*2)), (0,0,0,0))
        temp_draw = ImageDraw.Draw(temp_img)
        
        # Draw normally on temp (centered at size, size)
        tx, ty = size, size
        temp_draw.rectangle([tx-half, ty-half, tx+half, ty+half], fill=fill, outline=border, width=border_width)
        
        # Define crop box based on region relative to the temp center
        # We want to keep the intersection part.
        # Interlock logic:
        # Green (Top) overlaps Blue (Right) -> Intersection is Green's Bottom-Right
        
        crop_box = None
        paste_pos = None
        if region == 'bottom-right':
            # Keep x > center, y > center
            crop_box = (tx, ty, tx+half+border_width, ty+half+border_width)
            paste_pos = (int(center_x), int(center_y))
        elif region == 'bottom-left':
            # Keep x < center, y > center
            crop_box = (tx-half-border_width, ty, tx, ty+half+border_width)
            paste_pos = (int(center_x - half - border_width), int(center_y))
            
        cropped = temp_img.crop(crop_box)
        return cropped, paste_pos

    def create_logo(self, canvas_size):
        """
        Generates the Diffractor logo at the specific canvas_size.
        """
        # High resolution canvas for anti-aliasing (draw 4x larger then resize)
        scale_factor = 4 
        img_size = canvas_size * scale_factor
        img = Image.new('RGBA', (img_size, img_size), self.bg_color)
        draw = ImageDraw.Draw(img)

        # Geometry Settings
        # The logo is a cross shape. 
        # Square size should be roughly 45% of the total canvas to leave room but fill space.
        sq_size = img_size * 0.45
        
        # The Offset: How far the squares are shifted from the center.
        # Push squares outward to the edges of the bounding area:
        offset = sq_size * 1.11
        
        center = img_size / 2
        
        # Positions
        # Green (Top)
        pos_green = (center, center - offset/2)
        # Blue (Right)
        pos_blue = (center + offset/2, center)
        # Red (Bottom)
        pos_red = (center, center + offset/2)
        # Yellow (Left)
        pos_yellow = (center - offset/2, center)

        # ---------------------------------------------------------
        # THE INTERLOCK ALGORITHM (Painter's Algorithm with a twist)
        # Cycle: Yellow > Green > Blue > Red > Yellow
        # ---------------------------------------------------------
        
        # 1. Base Layer: Green
        self.draw_square(draw, *pos_green, sq_size, 'green')
        
        # Blue 
        self.draw_square(draw, *pos_blue, sq_size, 'blue')
        
        # Red 
        self.draw_square(draw, *pos_red, sq_size, 'red')
        
        # Yellow 
        self.draw_square(draw, *pos_yellow, sq_size, 'yellow')

        
        # THE FIX: Green must cover Blue.
        # Currently, Green was drawn first (step 1), so Blue (step 4) covers it.
        # We need to redraw the part of Green that overlaps Blue.
        # Intersection of Green (Top) and Blue (Right) is the Bottom-Left of Green.
        
        patch, pos = self.draw_partial_square(draw, *pos_green, sq_size, 'green', region='bottom-left')
        
        # Paste the patch onto the main image
        img.paste(patch, pos, patch)

        # Resize down to actual size with high-quality resampling (Anti-aliasing)
        img = img.resize((canvas_size, canvas_size), resample=Image.LANCZOS)
        return img

    def create_wide_logo(self, width, height, content_ratio=0.9):
        """
        Creates the Wide310x150 asset. 
        Places the square logo in the center of a wide transparent canvas.
        """
        img = Image.new('RGBA', (width, height), self.bg_color)
        
        # Determine logo size (fit within height with padding)
        logo_size = int(height * content_ratio)
        logo = self.create_logo(logo_size)
        
        # Center the logo
        x_pos = (width - logo_size) // 2
        y_pos = (height - logo_size) // 2
        
        img.paste(logo, (x_pos, y_pos), logo)
        return img

    def create_tile_logo(self, canvas_size, content_ratio):
        """
        Creates a tile asset: the logo centered on a transparent canvas with a margin.
        """
        img = Image.new('RGBA', (canvas_size, canvas_size), self.bg_color)
        logo_size = max(1, int(canvas_size * content_ratio))
        logo = self.create_logo(logo_size)
        offset = (canvas_size - logo_size) // 2
        img.paste(logo, (offset, offset), logo)
        return img

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
        if not os.path.exists(output_dir):
            os.makedirs(output_dir)

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

        print(f"Generating assets in '{output_dir}'...")

        for filename, w, h, method in assets:
            img = self.render_asset(filename, w, h, method)
            save_path = os.path.join(output_dir, filename)
            img.save(save_path, "PNG")
            print(f"Generated: {filename}")

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
            img.save(save_path, "PNG")
            print(f"Generated: {filename}")

        print("Done! All assets generated.")

    # Windows picks the nearest frame and scales down, so 256 is included to avoid ever scaling up.
    APP_ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
    APP_LOGOS = (("logo.png", 60), ("logo30.png", 30), ("logo15.png", 15))

    def generate_desktop_assets(self, res_dir):
        if not os.path.exists(res_dir):
            os.makedirs(res_dir)

        # Each frame is drawn at its target size rather than resampled from one large bitmap,
        # so the small frames keep the supersampled edges instead of a second round of scaling.
        frames = [self.create_logo(size) for size in self.APP_ICON_SIZES]
        ico_path = os.path.join(res_dir, "app.ico")
        frames[-1].save(
            ico_path,
            format="ICO",
            sizes=[(size, size) for size in self.APP_ICON_SIZES],
            append_images=frames[:-1],
        )
        print(f"Generated: app.ico ({', '.join(str(s) for s in self.APP_ICON_SIZES)})")

        for filename, size in self.APP_LOGOS:
            self.create_logo(size).save(os.path.join(res_dir, filename), "PNG")
            print(f"Generated: {filename}")

        print("Done! Desktop artwork generated.")


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
    args = parser.parse_args()

    generator = DiffractorLogoGenerator()
    if args.res:
        generator.generate_desktop_assets(args.res)
    else:
        generator.generate_windows_assets(args.output)


if __name__ == "__main__":
    main()