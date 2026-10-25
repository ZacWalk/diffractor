#!/usr/bin/env python3
# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Map Diffractor's icon_index names onto Fluent UI System Icons, and generate the code
# points in src/app_icons.h from the font's own mapping file.
#
# Segoe MDL2 Assets cannot ship off Windows -- it is a Windows system font and its licence does not
# permit redistribution -- so the icon set becomes a bundled one, the same on every platform. See
# docs/linux.md and docs/design.md.
#
# The mapping below is the reviewable artifact: it says which Fluent icon means what, in names. The
# code points are generated, because they are assigned sequentially by the font build and change
# between releases. Never hand-edit a code point; re-run this against the vendored JSON instead.
#
#   python tools/fluent_icons.py --json <FluentSystemIcons-Resizable.json> --preview
#   python tools/fluent_icons.py --json <FluentSystemIcons-Resizable.json> --write
#
# Fluent ships each icon as a regular (outline) and a filled pair. Diffractor's icons are outline,
# matching what Segoe MDL2 gave us, so regular is the default and filled is stated only where the
# solid form carries meaning -- a set rating star against an unset one.

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ICONS_HEADER = REPO_ROOT / "src" / "app_icons.h"

# icon_index name -> (fluent name without the ic_fluent_ prefix and size/variant suffix, filled)
#
# Where a row carries a comment, the substitution is not a like-for-like match and is the judgement
# call worth reviewing. Everything else is the same idea drawn in a different hand.
#
# facebook, flickr and twitter are absent deliberately: they were in the old enum, had no call site
# anywhere, and Fluent carries no third-party brand marks. They were deleted rather than mapped.
# tape and move_to are absent for the same reason - no call site - and both were colliding with a
# used icon under the new font.
ICONS: dict[str, tuple[str, bool]] = {
    "add": ("add", False),
    "remove": ("subtract", False),
    "add2": ("add_circle", False),
    "remove2": ("subtract_circle", False),
    # Not a volume level: this marks an audio stream, so it must stay distinct from volume3.
    "audio": ("sound_wave_circle", False),
    "music_note": ("music_note_2", False),
    "usb": ("usb_stick", False),
    "hard_drive": ("hard_drive", False),
    "back": ("arrow_left", False),
    "back_image": ("image_arrow_back", False),
    "album_artist": ("people", False),  # MDL2 used a group-of-people glyph here
    "tools": ("wrench_screwdriver", False),
    "camera": ("camera", False),
    "cancel": ("dismiss", False),
    "check": ("checkmark", False),
    "preview": ("eye", False),
    "color": ("color", False),
    "convert": ("arrow_swap", False),
    "swap": ("arrow_swap", False),
    "crop": ("crop", False),
    "del": ("delete", False),
    "sdcard": ("sim", False),  # Fluent has no SD card; sim is the closest removable-card shape
    "disk": ("storage", False),
    "document": ("document", False),
    "edit": ("edit", False),
    "eject": ("arrow_eject", False),
    "flag": ("flag", False),
    "folder": ("folder", False),
    "fullscreen": ("full_screen_maximize", False),
    "fullscreen_exit": ("full_screen_minimize", False),
    "import": ("arrow_import", False),
    "items": ("grid", False),
    "details": ("apps_list_detail", False),
    "link": ("link", False),
    "location": ("location", False),
    "map_pin": ("map_pin", False),
    "world": ("globe", False),
    "mail": ("mail", False),
    "share": ("share", False),
    "navigation": ("line_horizontal_3", False),
    "next": ("arrow_right", False),
    "next_image": ("image_arrow_forward", False),
    "network": ("server", False),
    "open_items": ("open_folder", False),
    "open_one": ("open", False),
    "more": ("more_horizontal", False),
    "new_folder": ("folder_add", False),
    "settings": ("settings", False),
    "parent": ("arrow_up", False),
    "pause": ("pause", False),
    "photo": ("image", False),
    "play": ("play", False),
    "print": ("print", False),
    "repeat_all": ("arrow_repeat_all", False),
    "repeat_one": ("arrow_repeat_1", False),
    "repeat_none": ("arrow_repeat_all_off", False),
    "refresh": ("arrow_clockwise", False),
    "rotate_clockwise": ("arrow_rotate_clockwise", False),
    # MDL2 had no anticlockwise glyph, so this was the clockwise one with a flag bit meaning
    # "draw it mirrored". Fluent has both, so the flag hack goes.
    "rotate_anticlockwise": ("arrow_rotate_counterclockwise", False),
    "save": ("save", False),
    "save_copy": ("save_copy", False),
    "scan": ("scan", False),
    "scan_text": ("scan_text", False),
    # Fluent UI has no flatbed scanner; fax is its closest physical scanner silhouette.
    "scanner": ("fax", False),
    "fit": ("arrow_fit", False),
    "one_to_one": ("ratio_one_to_one", False),
    "search": ("search", False),
    "shuffle": ("arrow_shuffle", False),
    "slideshow": ("slide_play", False),
    "sort": ("arrow_sort", False),
    "group": ("group", False),
    # MDL2 gave recursive and data the same code point, which made them indistinguishable.
    "recursive": ("arrow_expand_all", False),
    "mute": ("speaker_mute", False),
    # Segoe had four volume levels and Fluent has three, so one pair has to collapse. It collapses
    # at the loud end: telling "silent" from "quiet" carries more than telling "loud" from "louder".
    "volume0": ("speaker_0", False),
    "volume1": ("speaker_1", False),
    "volume2": ("speaker_2", False),
    "volume3": ("speaker_2", False),
    "repair": ("wrench", False),
    "star": ("star", False),
    "star_solid": ("star", True),
    "stop": ("stop", False),
    "tag": ("tag", False),
    "rename": ("rename", False),
    "time": ("clock", False),
    "video": ("video", False),
    "movies": ("movies_and_tv", False),
    "zoom_in": ("zoom_in", False),
    "zoom_out": ("zoom_out", False),
    "wallpaper": ("wallpaper", False),
    "lightbulb": ("lightbulb", False),
    "overview": ("glance", False),
    "buy": ("cart", False),
    "compare": ("item_compare", False),
    "undo": ("arrow_undo", False),
    "screen": ("desktop", False),
    "question": ("question_circle", False),
    "block": ("prohibited", False),
    "dock_bottom": ("panel_bottom", False),
    "cloud": ("cloud", False),
    "down": ("chevron_down", False),
    "up": ("chevron_up", False),
    # Step-through navigation: previous/next item, and previous/next folder or search. A chevron
    # says "one more of the same, that way", which is what stepping is; the image_arrow_* and
    # folder_arrow_* glyphs read as moving the file somewhere instead.
    "left": ("chevron_left", False),
    "right": ("chevron_right", False),
    "small_down": ("caret_down", True),
    "small_up": ("caret_up", True),
    "small_left": ("caret_left", True),
    "small_right": ("caret_right", True),
    "error": ("error_circle", False),
    "bullet": ("circle_small", True),
    "minimize": ("line_horizontal_1", False),
    "maximize": ("maximize", False),
    "restore": ("square_multiple", False),
    "close": ("dismiss", False),
    # folder_arrow_right would read as navigation rather than as moving files.
    "move_to_folder": ("folder_swap", False),
    "copy_to_folder": ("copy_arrow_right", False),
    "person": ("person", False),
    "download": ("arrow_download", False),
    "pin": ("pin", False),
    "pinned": ("pin", True),
    "keyboard": ("keyboard", False),
    "orientation": ("orientation", False),
    "list": ("list", False),
    "attach": ("attach", False),
    "archive": ("folder_zip", False),
    "app": ("app_generic", False),
    "spreadsheet": ("table", False),
    "image_vector": ("draw_shape", False),
    "presentation": ("slide_layout", False),
    "model3d": ("cube", False),
    "code": ("code", False),
    "data": ("database", False),
    "retro": ("games", False),
    "language": ("local_language", False),
    "edit_metadata": ("document_edit", False),
    "verbose_metadata": ("text_description", False),
    "document_information": ("info", False),
    "edit_cut": ("cut", False),
    # MDL2 gave cut and copy the same code point, so they drew identically.
    "edit_copy": ("copy", False),
    "edit_paste": ("clipboard_paste", False),
    "documentation": ("book_open", False),
    "support": ("person_support", False),
    "verify": ("checkmark_circle", False),
    "sync": ("arrow_sync", False),
    "checkbox_off": ("checkbox_unchecked", False),
    "checkbox_on": ("checkbox_checked", True),
    "radio_off": ("circle", False),
    "radio_on": ("radio_button", True),
    "media_options": ("options", False),
    "unknown": ("question", False),
    "today": ("calendar_today", False),
    "yesterday": ("history", False),  # MDL2 used a "back in time" calendar; history reads the same
    "set": ("layer_diagonal", False),
    "set_solid": ("layer_diagonal", True),
}

# Not glyphs from the icon font, and deliberately left alone.
LITERALS = {
    "none": "0",
    "copyright": "0xA9",  # the actual COPYRIGHT SIGN, not an icon
}


def load_font_map(path: Path) -> dict[str, int]:
    if not path.exists():
        sys.exit(
            f"{path} does not exist.\n"
            "It is the mapping file shipped beside the font, e.g.\n"
            "  FluentSystemIcons-Resizable.json from microsoft/fluentui-system-icons"
        )

    return json.loads(path.read_text(encoding="utf-8"))


def resolve(font: dict[str, int]) -> tuple[dict[str, tuple[int, str]], list[str]]:
    """Answer the resolved code point and full Fluent name for each icon, plus anything missing."""
    resolved: dict[str, tuple[int, str]] = {}
    missing: list[str] = []

    for name, (fluent, filled) in ICONS.items():
        variant = "filled" if filled else "regular"
        # The resizable font is built from the 20px designs, so that is the size in every key.
        key = f"ic_fluent_{fluent}_20_{variant}"

        if key in font:
            resolved[name] = (font[key], key)
        else:
            missing.append(f"{name}: {key}")

    return resolved, missing


def render(resolved: dict[str, tuple[int, str]]) -> str:
    lines = ["enum class icon_index", "{"]

    for name in ["none"] + list(ICONS):
        if name in LITERALS:
            lines.append(f"\t{name} = {LITERALS[name]},")
            continue

        code, key = resolved[name]
        lines.append(f"\t{name} = 0x{code:04X}, // {key}")

    # copyright is not in ICONS, so it is emitted here rather than in the loop above.
    lines.append(f"\tcopyright = {LITERALS['copyright']},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate icon code points from Fluent's mapping")
    parser.add_argument("--json", type=Path, required=True, help="the font's mapping file")
    parser.add_argument("--preview", action="store_true", help="print the enum, change nothing")
    parser.add_argument("--write", action="store_true", help="rewrite the enum in app_icons.h")
    args = parser.parse_args()

    font = load_font_map(args.json)
    resolved, missing = resolve(font)

    if missing:
        print(f"{len(missing)} icon(s) have no match in this font release:")
        for line in missing:
            print(f"  {line}")
        print()
        print("Either the name changed or the icon was withdrawn. Pick a replacement above.")
        return 1

    rendered = render(resolved)

    if args.preview or not args.write:
        print(rendered)
        return 0

    source = ICONS_HEADER.read_text(encoding="utf-8")
    replaced, count = re.subn(r"enum class icon_index\s*\{.*?\};", rendered, source, flags=re.S)

    if count != 1:
        sys.exit(f"Expected exactly one icon_index enum in {ICONS_HEADER}, found {count}")

    ICONS_HEADER.write_text(replaced, encoding="utf-8")
    print(f"Wrote {len(resolved)} icons to {ICONS_HEADER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
