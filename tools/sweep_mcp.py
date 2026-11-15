#!/usr/bin/env python3
# Purpose: Presses every button. Drives an agent session (-mcp) out of sight, on a collection and profile it makes and
# throws away, and in each state it reaches - Items with nothing, one photo and all selected, a photo, a tune and a
# document shown, and each task - runs each command, opens each menu and presses each of its rows, and works each
# control, link and part ui_controls lists, putting the window back between. After each it checks that the process
# lives, its answer is no error, the window settles, the log shows no new error, and a dialog or menu that opened is
# described, then cancelled; the window must still draw. Ends with what coverage says was exercised and what was not,
# and why. docs/testing.md describes it; give it the program's path.

import argparse
import json
import math
import os
import queue
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_mcp_session import Session, taken, text_of, write_png  # noqa: E402

# What acts outside the window - on the user's clipboard, desktop, drives or other programs, or the network - is not
# pressed, and the report says so.
OUTSIDE = {
    "browse_open_containingfolder": "opens the system's file browser",
    "browse_open_in_file_browser": "opens the system's file browser",
    "browse_open_googlemap": "opens a web browser",
    "view_help": "opens a web browser",
    "options_help": "opens a web browser",
    "print": "prints through the system",
    "tool_burn": "burns a disc through the system",
    "tool_desktop_background": "sets the user's desktop background",
    "tool_eject": "ejects a drive",
    "tool_file_properties": "shows the system's properties sheet",
    "tool_open_with": "opens another program",
    "tool_share": "opens the system's share sheet",
    "tool_scan": "works a scanner through the system",
    "edit_copy": "writes the user's clipboard",
    "edit_cut": "writes the user's clipboard",
    "edit_paste": "reads the user's clipboard",
    "edit_copy_item_path": "writes the user's clipboard",
    "tool_delete": "sends files to the user's Recycle Bin",
    "info_check_for_updates": "reaches the network",
    "info_new_version": "installs another version",
    "exit": "ends the session",
}

# Pressed once, at the end, as they change the window itself: its size, or the language every label is in.
WINDOW = ["view_minimize", "view_restore", "view_maximize", "view_restore"]
LANGUAGE_MENU = "menu_language"
# The Open menus list the programs that open the file as well as the window's own commands: a row with no command of
# the window's own opens another program, and is not pressed.
OPEN_MENUS = {"menu_open", "menu_open_status"}

# The window's own options - its filters, grouping, sorting, display and playback, and an item held to compare with
# the next - which persist from one state to the next: each is put back as it was when the sweep began, as each state
# is entered again.
OPTIONS = ("filter_", "group_", "sort_", "option_", "playback_", "view_zoom_", "view_show_sidebar", "large_font",
           "show_raw_preview", "verbose_metadata", "repeat_toggle", "view_favorite_tags", "pin_item")

# Log lines that name an error, less what is known to be no fault: the external tools' scan meeting folders it may
# not read.
ERROR_LINE = re.compile(r"\b(error|exception|assert(ion)?|failed|crash(ed)?)\b", re.IGNORECASE)
BENIGN_LINE = re.compile(r"access is denied|scan_tools|cannot open folder", re.IGNORECASE)

PHOTO = "beach.png"
# The status bar's filter box: what it holds filters every state after, as an option does, so it is emptied.
FILTER_BOX = "status/edit/filter"
# A view holding work not yet saved - a movie - asks before it closes; the sweep leaves it without saving.
LEAVE_UNSAVED = re.compile(r"without saving|don.t save|discard", re.IGNORECASE)


def write_wav(path, seconds=2.0, rate=22050):
    """A tune for the collection: a falling tone, made with the standard library alone."""
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(seconds * rate)):
            frequency = 660 - 220 * i / (seconds * rate)
            frames += struct.pack("<h", int(8000 * math.sin(2 * math.pi * frequency * i / rate)))
        w.writeframes(bytes(frames))


def make_collection(root):
    collection = os.path.join(root, "collection")
    trip = os.path.join(collection, "trip")
    os.makedirs(trip)
    # Large enough to show in a small screenshot at its own size, full screen on a large display.
    write_png(os.path.join(collection, PHOTO), 640, 480)
    write_png(os.path.join(collection, "pompei.png"), xmp=taken("2019-07-03T10:00:00", 40.75, 14.48))
    write_png(os.path.join(collection, "walk.png"), xmp=taken("2019-07-03T10:05:00"))
    write_png(os.path.join(trip, "hill.png"), 48, 32, xmp=taken("2020-05-01T09:00:00"))
    write_png(os.path.join(trip, "lake.png"), 40, 30)
    write_wav(os.path.join(collection, "tune.wav"))
    with open(os.path.join(collection, "notes.md"), "w", encoding="utf-8") as f:
        f.write("# Notes\n\nA *document* with a [link](https://example.com) and a list:\n\n- one\n- two\n")
    with open(os.path.join(collection, "data.csv"), "w", encoding="utf-8") as f:
        f.write("name,count\nbeach,3\nhill,5\n")
    return collection


def png_pixels(data):
    """An 8-bit RGB or RGBA PNG's pixels, decoded without an imaging library: (width, height, channels, rows)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, width, height, kind = 8, b"", 0, 0, 0
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height, depth, kind = struct.unpack(">IIBB", body[:10])
            if depth != 8 or kind not in (2, 6) or body[12] != 0:
                return None
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    channels = 3 if kind == 2 else 4
    raw, stride = zlib.decompress(idat), width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind_of_filter, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if kind_of_filter == 1:
                line[i] = (line[i] + left) & 255
            elif kind_of_filter == 2:
                line[i] = (line[i] + up) & 255
            elif kind_of_filter == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind_of_filter == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else up_left)) & 255
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


def colours_in(data):
    """How many colours a screenshot shows: one or two is a window that draws nothing."""
    decoded = png_pixels(data)
    if not decoded:
        return -1
    _, _, channels, rows = decoded
    seen = set()
    for row in rows:
        for x in range(0, len(row), channels * 3):
            seen.add(row[x:x + 3])
            if len(seen) > 16:
                return len(seen)
    return len(seen)


class Sweep:
    def __init__(self, exe, root, collection, verbose, all_parts=False):
        self.exe, self.root, self.collection, self.verbose = exe, root, collection, verbose
        self.all_parts = all_parts
        self.session = None
        self.mark = 0
        self.findings = []
        self.actions = 0
        self.skipped = {}
        self.pressed = set()
        self.started = time.monotonic()
        self.restarts = 0
        self.state = None
        self.options = None
        # Whether the session opens no other program without launch, which a build that knows the tier does; and what
        # it was asked to open, and did not.
        self.guarded = False
        self.not_opened = []

    # ------------------------------------------------------------------ the session

    def start(self):
        stderr = os.path.join(self.root, f"stderr-{self.restarts}.txt")
        self.session = Session([self.exe, "-mcp", "-mcp-ui:offscreen", "-mcp-allow:write,capture",
                                f"-profile:{os.path.join(self.root, 'profile')}", f"-collection:{self.collection}"],
                               stderr)
        self.session.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                            "clientInfo": {"name": "sweep", "version": "1"}}, modern=False)
        self.call("window", {"width": 1280, "height": 800})
        self.call("wait_until_idle", {"timeout_s": 120}, timeout=150)
        allow = self.call("status").get("structuredContent", {}).get("allow", {})
        self.guarded = "launch" in allow and not allow["launch"]
        self.mark = self.call("log_read", {"lines": 1}).get("structuredContent", {}).get("mark", 0)

    def alive(self):
        return self.session and self.session.process.poll() is None

    def restart(self, why):
        self.restarts += 1
        self.find("session", why)
        try:
            self.session.process.kill()
        except OSError:
            pass
        self.start()
        if self.state:
            self.enter(self.state)

    def call(self, name, arguments=None, timeout=120):
        return self.session.call(name, arguments or {}, timeout=timeout)

    def find(self, kind, what, detail=""):
        finding = {"state": self.state[0] if self.state else "", "kind": kind, "what": what, "detail": detail}
        self.findings.append(finding)
        print(f"  FINDING [{finding['state']}] {kind}: {what}{(' - ' + detail) if detail else ''}", flush=True)

    # ------------------------------------------------------------------ states

    def states(self):
        def view_of(name):
            return [("navigate", {"to": self.collection}), ("select", {"names": [name]}),
                    ("command_run", {"command": "view_fullscreen"})]

        items = [("navigate", {"to": self.collection})]
        tasks = ["tool_sync", "tool_import", "tool_locate", "tool_tag", "tool_rename", "tool_convert",
                 "tool_edit_metadata", "tool_adjust_date", "tool_edit", "tool_movie", "options"]
        return ([("items, nothing selected", items + [("select", {"none": True})]),
                 ("items, a photo selected", items + [("select", {"names": [PHOTO]})]),
                 ("items, all selected", items + [("select", {"all": True})]),
                 ("a photo shown", view_of(PHOTO)),
                 ("a tune shown", view_of("tune.wav")),
                 ("a document shown", view_of("notes.md"))] +
                [(f"the {t} task", items + [("select", {"names": [PHOTO]}), ("command_run", {"command": t})])
                 for t in tasks])

    def close_modal(self):
        """Cancels whatever dialog or menu is open, describing it first."""
        for _ in range(6):
            shown = self.call("ui_state").get("structuredContent", {})
            if shown.get("menu_open"):
                self.call("menu_respond", {"cancel": True})
            elif shown.get("dialogs"):
                self.call("dialog_respond", {"cancel": True})
            else:
                return shown
        return self.call("ui_state").get("structuredContent", {})

    def signature(self, shown):
        return (shown.get("view"), shown.get("search"), shown.get("selected"), shown.get("full_screen"),
                tuple(shown.get("selection") or []))

    def checked_options(self):
        listed = self.call("command_list").get("structuredContent", {}).get("commands", [])
        return {c["name"]: bool(c.get("checked")) for c in listed
                if c["name"].startswith(OPTIONS) and c["name"] != "playback_menu"}

    def restore_options(self):
        """Puts the window's options back as they were when the sweep began: those it had on first, which turns off
        the others of a choice, then any still on that it did not have."""
        if self.options is None:
            self.options = self.checked_options()
            return
        for wanted in (True, False):
            now = self.checked_options()
            for name, was in self.options.items():
                if was == wanted and now.get(name) != was:
                    self.call("command_run", {"command": name})
                    self.close_modal()
        # One no command puts back - a choice none of whose options was on - is taken as it is now.
        now = self.checked_options()
        for name in [n for n, was in self.options.items() if now.get(n) != was]:
            self.options[name] = now.get(name)

    def filtering(self):
        """Whether the filter box holds anything - typed there, or set by a button that filters."""
        box = next((c for c in self.controls("status") if c["id"] == FILTER_BOX), None)
        return bool(box and box.get("value"))

    def empty_filter(self):
        if self.filtering():
            for key in ("ctrl+a", "delete"):
                self.call("ui_act", {"id": FILTER_BOX, "action": "key", "key": key})
            self.close_modal()

    def enter(self, state):
        self.state = state
        self.close_modal()
        # Out of a view first, back to Items, as a user leaves one.
        for _ in range(3):
            shown = self.call("ui_state").get("structuredContent", {})
            if shown.get("full_screen"):
                self.call("command_run", {"command": "view_fullscreen"})
            elif shown.get("view") not in ("items", None):
                self.leave_unsaved(self.call("command_run", {"command": "view_close"}))
            else:
                break
            self.close_modal()
        self.restore_options()
        self.empty_filter()
        for tool, arguments in state[1]:
            answer = self.call(tool, arguments)
            if answer.get("isError"):
                self.find("setup", f"{tool} {json.dumps(arguments)}", text_of(answer)[:300])
            self.close_modal()
        self.call("wait_until_idle", {"timeout_s": 60}, timeout=90)
        shown = self.call("ui_state").get("structuredContent", {})
        # What the state selects must be all that is selected: something left over - an item held to compare - would
        # show something else.
        named = [arguments["names"] for tool, arguments in state[1] if tool == "select" and "names" in arguments]
        selected = sorted(os.path.basename(path) for path in shown.get("selection") or [])
        if named and selected != sorted(named[-1]):
            self.find("setup", "the selection is not what the state selects", ", ".join(selected))
        self.home = self.signature(shown)

    def back_home(self):
        shown = self.close_modal()
        if (self.signature(shown) != self.home or (self.options and self.checked_options() != self.options) or
                self.filtering()):
            self.enter(self.state)

    def leave_unsaved(self, answer):
        """Answers a view's question as it closes with what leaves without saving, where it asks one."""
        for dialog in (answer.get("structuredContent") or {}).get("dialogs") or []:
            for button in dialog.get("buttons", []):
                if LEAVE_UNSAVED.search(button.get("text", "")):
                    self.call("dialog_respond", {"button": button["text"]})
                    return

    # ------------------------------------------------------------------ one action, and what it left

    def act(self, what, tool, arguments, expect_error=False):
        """Does one thing, then checks the window: alive, settled, nothing new logged as an error, a dialog or menu
        described and closed."""
        self.actions += 1
        if self.verbose:
            print(f"    {self.actions:5} {time.monotonic() - self.started:7.1f}s {what}", flush=True)
        try:
            answer = self.call(tool, arguments, timeout=120)
        except (AssertionError, queue.Empty) as e:
            if not self.alive():
                self.restart(f"the process ended after {what}: {e}")
                return None
            self.find("hang", what, str(e))
            self.restart(f"no answer after {what}")
            return None

        if answer.get("isError") and not expect_error:
            self.find("error", what, text_of(answer)[:400])

        structured = answer.get("structuredContent", {}) or {}
        if structured.get("state") == "waiting_for_dialog" or structured.get("opened"):
            dialogs = structured.get("dialogs") or []
            titles = [d.get("title", "") for d in dialogs] if isinstance(dialogs, list) else []
            if titles and self.verbose:
                print(f"          opened {', '.join(titles)}", flush=True)

        if not self.alive():
            self.restart(f"the process ended after {what}")
            return None

        idle = self.call("wait_until_idle", {"timeout_s": 30, "quiet_ms": 200}, timeout=60).get("structuredContent", {})
        if not idle.get("idle"):
            shown = self.call("ui_state").get("structuredContent", {})
            if not shown.get("dialogs") and not shown.get("menu_open"):
                self.find("busy", what, ", ".join(idle.get("busy", [])))

        logged = self.call("log_read", {"since": self.mark, "lines": 400})
        self.mark = logged.get("structuredContent", {}).get("mark", self.mark)
        lines = text_of(logged).splitlines()
        self.not_opened += [f"{what}: {line.split('may not open other programs: ', 1)[-1]}" for line in lines
                            if "may not open other programs" in line]
        errors = [line for line in lines
                  if ERROR_LINE.search(line) and not BENIGN_LINE.search(line) and not line.startswith("mark ")]
        if errors:
            self.find("log", what, " | ".join(errors[:5])[:600])
        return answer

    def check_draws(self):
        picture = self.call("screenshot", {"settle_s": 5, "max_size": 160})
        images = [c for c in picture.get("content", []) if c.get("type") == "image"]
        if not images:
            self.find("screenshot", "no picture", text_of(picture)[:200])
            return
        import base64
        colours = colours_in(base64.b64decode(images[0]["data"]))
        if 0 <= colours <= 2:
            self.find("screenshot", "the window draws nothing", f"{colours} colours")

    # ------------------------------------------------------------------ what is pressed

    def controls(self, scope=None):
        arguments = {"limit": 5000}
        if scope:
            arguments["scope"] = scope
        return self.call("ui_controls", arguments).get("structuredContent", {}).get("controls", [])

    def outside(self, control):
        command = control.get("command", "")
        if command in OUTSIDE:
            self.skipped[command] = OUTSIDE[command]
            return True
        needs = control.get("needs")
        if needs in ("refused", "launch", "network"):
            self.skipped[command or control["id"]] = f"needs {needs}" if needs != "refused" else "refused"
            return True
        return False

    def sampled(self, controls):
        """Of the parts one element shows many of - a calendar's months - the first, the middle and the last."""
        if self.all_parts:
            return controls
        groups = {}
        for c in controls:
            named = re.match(r"^(.*) (\d+)$", c["label"]) if c["kind"] == "part" else None
            if named:
                groups.setdefault((c["scope"], named.group(1)), []).append(c["id"])
        grouped = {i for ids in groups.values() for i in ids}
        kept = {i for ids in groups.values() for i in (ids[0], ids[len(ids) // 2], ids[-1])}
        return [c for c in controls if c["id"] in kept or c["id"] not in grouped]

    def press_controls(self):
        for control in self.sampled(self.controls()):
            key = control["id"]
            if (control["scope"] == "menu" or not control.get("enabled") or key in self.pressed or
                    self.outside(control)):
                continue
            current = {c["id"]: c for c in self.controls()}
            if key not in current:
                self.back_home()
                current = {c["id"]: c for c in self.controls()}
                if key not in current:
                    continue
            control = current[key]
            if not control.get("enabled"):
                continue
            self.pressed.add(key)
            kind = control["kind"]
            if kind == "edit":
                self.act(f"type into {key}", "ui_act", {"id": key, "action": "type", "text": "sweep"})
            elif kind == "slider":
                self.act(f"drag {key}", "ui_act", {"id": key, "action": "drag", "dx": 30})
            elif kind == "content":
                for action, extra in (("press", {}), ("wheel", {"notches": -2}), ("right_click", {}),
                                      ("double_click", {})):
                    self.act(f"{action} {key}", "ui_act", {"id": key, "action": action, **extra})
                    self.back_home()
                continue
            else:
                self.act(f"press {key}", "ui_act", {"id": key})
            self.back_home()

    def pressable(self, row, opens_programs):
        """A menu row to press: enabled, and acting inside the window. An Open menu's rows that open other programs
        are pressed only where the session opens none - a build that skips them without launch."""
        if not row.get("enabled") or self.outside(row):
            return False
        if opens_programs and not row.get("command") and not self.guarded:
            self.skipped["Open with ..."] = "opens another program, and this build would open it"
            return False
        return True

    def menu_leaves(self, name, open_menu):
        """The rows of a menu and its submenus, each by its path, opening each submenu to read it. The language's
        submenu is passed over: its rows change the language every label is in."""
        leaves, submenus = [], []
        open_menu()
        for row in self.controls("menu"):
            path = row.get("path") or [row["label"]]
            if row.get("submenu"):
                if row.get("command") == LANGUAGE_MENU:
                    self.skipped[LANGUAGE_MENU] = "its rows change the language every label is in"
                else:
                    submenus.append((row["id"], row.get("command") in OPEN_MENUS))
            elif self.pressable(row, name in OPEN_MENUS):
                leaves.append(path)
        self.close_modal()
        for submenu, opens_programs in submenus:
            open_menu()
            self.call("ui_act", {"id": submenu})
            for row in self.controls("menu"):
                path = row.get("path") or [row["label"]]
                if len(path) > 1 and not row.get("submenu") and self.pressable(row, opens_programs):
                    leaves.append(path)
            self.close_modal()
        return leaves

    def press_menu(self, name, open_menu):
        for path in self.menu_leaves(name, open_menu):
            key = f"{name}: {' > '.join(path)}"
            if key in self.pressed:
                continue
            self.pressed.add(key)
            open_menu()
            self.act(f"choose {key}", "menu_respond", {"choose": " > ".join(path)})
            self.back_home()

    def run_commands(self):
        listed = self.call("command_list", {"enabled_only": True}).get("structuredContent", {}).get("commands", [])
        for command in listed:
            name = command["name"]
            if name in OUTSIDE:
                self.skipped[name] = OUTSIDE[name]
                continue
            if command.get("needs") in ("launch", "network"):
                self.skipped[name] = f"needs {command['needs']}"
                continue
            if command.get("needs") == "refused" or name in WINDOW or f"run {name}" in self.pressed:
                continue
            if name.startswith("menu_") or name == "playback_menu":
                if name == LANGUAGE_MENU:
                    self.skipped[name] = "its rows change the language every label is in"
                    continue
                self.press_menu(name, lambda n=name: self.call("command_run", {"command": n}))
                continue
            self.pressed.add(f"run {name}")
            self.act(f"run {name}", "command_run", {"command": name})
            # A command that toggles is run again, to put the window back.
            if command.get("checked") is not None:
                self.act(f"run {name} again", "command_run", {"command": name})
            self.back_home()

    def context_menus(self):
        for control in self.controls():
            if control["kind"] == "content" or (control["scope"] == "sidebar" and control["kind"] == "link"):
                key = f"context {control['id']}"
                if key in self.pressed or not control.get("enabled"):
                    continue
                self.pressed.add(key)
                self.press_menu(key, lambda i=control["id"]: self.call("ui_act", {"id": i, "action": "right_click"}))
                if control["kind"] != "content":
                    break

    # ------------------------------------------------------------------ the whole of it

    def run(self, only, quick):
        self.start()
        for state in self.states():
            if only and not any(o.lower() in state[0].lower() for o in only):
                continue
            print(f"State: {state[0]}", flush=True)
            self.enter(state)
            self.check_draws()
            self.run_commands()
            self.context_menus()
            if not quick:
                self.press_controls()
            self.check_draws()

        self.state = ("the window", [])
        for name in WINDOW:
            self.act(f"run {name}", "command_run", {"command": name})
        self.call("window", {"width": 1280, "height": 800})

        coverage = self.call("coverage", {"list": 5000})
        code = self.session.close(timeout=60)
        if code != 0:
            self.find("session", f"the end of input ended the session with {code}")
        return coverage


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Presses every button Diffractor offers, and says what it found.")
    parser.add_argument("exe", help="The program: exe\\diffractor64.exe, or a Debug build")
    parser.add_argument("--state", action="append", help="Only the states whose name has this; repeatable")
    parser.add_argument("--quick", action="store_true", help="Commands and menus only, not every control")
    parser.add_argument("--all-parts", action="store_true",
                        help="Every part of an element that shows many - each of a calendar's months - not a sample")
    parser.add_argument("--report", help="Where to write the report as JSON")
    parser.add_argument("--keep", action="store_true", help="Keep the collection, profile and stderr made")
    parser.add_argument("--verbose", action="store_true", help="Name each action as it is done")
    arguments = parser.parse_args()

    root = tempfile.mkdtemp(prefix="df-sweep-")
    sweep = Sweep(os.path.abspath(arguments.exe), root, make_collection(root), arguments.verbose, arguments.all_parts)
    coverage = {}
    try:
        coverage = sweep.run(arguments.state, arguments.quick)
    except (AssertionError, queue.Empty, OSError, subprocess.SubprocessError) as e:
        sweep.find("sweep", "stopped", str(e))
    finally:
        if sweep.session and sweep.alive():
            sweep.session.process.kill()

    structured = coverage.get("structuredContent", {}) if coverage else {}
    commands = structured.get("commands", {})
    controls = structured.get("controls", {})
    report = {"actions": sweep.actions, "restarts": sweep.restarts, "seconds": round(time.monotonic() - sweep.started),
              "findings": sweep.findings, "not_pressed": sweep.skipped, "not_opened": sweep.not_opened,
              "coverage": structured}
    if arguments.report:
        with open(arguments.report, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)

    print(f"\nSweep: {sweep.actions} actions in {report['seconds']} s, {sweep.restarts} restarts, "
          f"{len(sweep.findings)} findings")
    print(f"Commands: {commands.get('exercised', 0)} of {commands.get('total', 0)} exercised; "
          f"controls: {controls.get('used', 0)} of {controls.get('listed', 0)} listed used")
    not_run = commands.get("not_run", [])
    if not_run:
        print("Not run: " + ", ".join(
            c["name"] + (f" [{c['needs']}]" if c.get("needs") else "") + ("" if c.get("enabled") else " (disabled)")
            for c in not_run))
    if sweep.skipped:
        print("Not pressed, as they act outside the window: " + ", ".join(sorted(sweep.skipped)))
    if sweep.not_opened:
        print(f"Asked to open another program {len(sweep.not_opened)} times, and the session opened none: "
              + "; ".join(sweep.not_opened[:8]) + (" ..." if len(sweep.not_opened) > 8 else ""))
    for finding in sweep.findings:
        print(f"  [{finding['state']}] {finding['kind']}: {finding['what']} {finding['detail']}".rstrip())

    if not arguments.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print(f"Kept {root}")
    return 1 if sweep.findings else 0


if __name__ == "__main__":
    sys.exit(main())
