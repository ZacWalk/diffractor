#!/usr/bin/env python3
# Purpose: Drives an agent session (-mcp) end to end over its pipes, as an MCP client does: the handshake each era of
# client uses, the tool list and what the session withholds, a Movie project checked without a window, the window made
# out of sight by the first tool that needs it, the index searched, the window's controls listed and a menu opened, read
# and cancelled, a screenshot, task panels filled in and reviewed - fields a check box shows or enables among them - a
# place suggested from the photo taken nearest in time and proposed by Locate, a tool's argument refused, and the end of
# input ending the session. A second session, which may write, types a search into the address box as a user does,
# waits while a large photo is read and decoded and reads back a file Convert has just written. dd.ps1 test runs it
# against the program it built; give the program's path to run it by hand.

import base64
import json
import os
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

MODERN = "2026-07-28"
LEGACY = "2025-06-18"


def png_chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path, width=32, height=24, xmp=None):
    """A small picture for the collection to hold, made without an imaging library, with an XMP packet if given."""
    rows = b"".join(b"\x00" + b"".join(bytes((x * 8 % 256, y * 10 % 256, 128)) for x in range(width))
                    for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    metadata = png_chunk(b"iTXt", b"XML:com.adobe.xmp\x00\x00\x00\x00\x00" + xmp.encode("utf-8")) if xmp else b""
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + metadata +
                png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b""))


def write_large_png(path, width, height):
    """A picture large enough that reading and decoding it keeps the window busy for a moment: every row the same
    gradient, which PNG's Up filter turns to zeros, so the file stays small however large the picture."""
    row = bytes(b for x in range(width) for b in (x * 255 // (width - 1), x * 7 % 256, 128))
    same = b"\x02" + bytes(len(row))
    compressor = zlib.compressobj()
    data = [compressor.compress(b"\x00" + row)]
    data += [compressor.compress(same) for _ in range(height - 1)]
    data.append(compressor.flush())
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", b"".join(data)) +
                png_chunk(b"IEND", b""))


def taken(when, latitude=None, longitude=None):
    """An XMP packet saying when a picture was taken, and where if given, as degrees and minutes."""
    where = ""
    if latitude is not None:
        def degrees(value, positive, negative):
            minutes = abs(value) * 60
            return f"{int(minutes // 60)},{minutes % 60:.4f}{positive if value >= 0 else negative}"
        where = (f' exif:GPSLatitude="{degrees(latitude, "N", "S")}"'
                 f' exif:GPSLongitude="{degrees(longitude, "E", "W")}"')
    return ('<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
            f'<rdf:Description rdf:about="" xmlns:exif="http://ns.adobe.com/exif/1.0/" '
            f'exif:DateTimeOriginal="{when}"{where}/></rdf:RDF></x:xmpmeta>')


class Session:
    def __init__(self, arguments, stderr_path):
        self._stderr = open(stderr_path, "w", encoding="utf-8", errors="replace")
        self.process = subprocess.Popen(arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self._stderr)
        self._lines = queue.Queue()
        self.stray = []
        self._next_id = 1
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.process.stdout:
            self._lines.put(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
        self._lines.put(None)

    def send(self, message):
        self.process.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
        self.process.stdin.flush()

    def request(self, method, params=None, modern=True, timeout=120):
        params = dict(params or {})
        if modern:
            meta = dict(params.get("_meta", {}))
            meta["io.modelcontextprotocol/protocolVersion"] = MODERN
            meta["io.modelcontextprotocol/clientCapabilities"] = {}
            params["_meta"] = meta
        request_id = self._next_id
        self._next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        deadline = time.monotonic() + timeout

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"{method}: no answer in {timeout} s")
            line = self._lines.get(timeout=remaining)
            if line is None:
                raise AssertionError(f"{method}: the session ended without answering")
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                # Nothing but the protocol may reach stdout.
                self.stray.append(line)
                continue
            if message.get("id") == request_id:
                return message

    def call(self, name, arguments=None, timeout=120):
        answer = self.request("tools/call", {"name": name, "arguments": arguments or {}}, timeout=timeout)
        if "error" in answer:
            raise AssertionError(f"{name}: {answer['error']}")
        return answer["result"]

    def close(self, timeout=60):
        self.process.stdin.close()
        try:
            return self.process.wait(timeout=timeout)
        finally:
            self._stderr.close()


def text_of(result):
    return "\n".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")


def main(exe):
    failures = []
    checks = 0

    def check(condition, what):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(what)
            print(f"  FAIL  {what}")

    with tempfile.TemporaryDirectory(prefix="df-mcp-") as root:
        collection = os.path.join(root, "collection")
        os.makedirs(collection)
        write_png(os.path.join(collection, "beach.png"))
        write_png(os.path.join(collection, "pompei.png"), xmp=taken("2019-07-03T10:00:00", 40.75, 14.48))
        write_png(os.path.join(collection, "walk.png"), xmp=taken("2019-07-03T10:05:00"))
        project = os.path.join(root, "trip.dfv")
        with open(project, "w", encoding="utf-8") as f:
            f.write("t 3 'Hiking'\nt 2 'The End'\n")

        stderr_path = os.path.join(root, "stderr.txt")
        session = Session([exe, "-mcp", f"-profile:{os.path.join(root, 'profile')}", f"-collection:{collection}",
                           "-mcp-allow:capture"], stderr_path)

        try:
            legacy = session.request("initialize", {"protocolVersion": LEGACY, "capabilities": {},
                                                    "clientInfo": {"name": "smoke", "version": "1"}}, modern=False)
            check(legacy.get("result", {}).get("protocolVersion") == LEGACY, "initialize agrees the version asked for")
            check(legacy.get("result", {}).get("serverInfo", {}).get("name") == "Diffractor", "and names the server")

            discover = session.request("server/discover")
            check(MODERN in discover.get("result", {}).get("supportedVersions", []), "server/discover offers 2026-07-28")

            listed = session.request("tools/list").get("result", {})
            names = {t["name"] for t in listed.get("tools", [])}
            check({"status", "help", "movie_check", "search", "ui_state", "screenshot"} <= names, "the tools offered")
            check("movie_render" not in names and "ui_input" not in names, "and write's withheld without write")

            status = session.call("status").get("structuredContent", {})
            window = status.get("window", {})
            check(window.get("presence") == "none" and window.get("made") is False, "no window until a tool needs one")
            check(any(w.get("tool") == "movie_render" for w in status.get("withheld", [])), "status says what it withholds")

            checked = session.call("movie_check", {"projects": project}).get("structuredContent", {})
            outcomes = [p.get("outcome") for p in checked.get("projects", [])]
            check(outcomes == ["ready"], f"a project of text screens is ready to render ({outcomes})")

            found = session.call("search", {"query": "@photo"}).get("structuredContent", {})
            paths = [i.get("path", "") for i in found.get("items", [])]
            check(found.get("total") == 3 and any(p.endswith("beach.png") for p in paths),
                  f"search makes the window, loads the index and finds the collection's photos ({paths})")

            made = session.call("status").get("structuredContent", {}).get("window", {})
            check(made.get("made") is True, "the window is made, out of sight")

            idle = session.call("wait_until_idle", {"timeout_s": 90}, timeout=120).get("structuredContent", {})
            check(idle.get("idle") is True, f"and settles ({idle.get('busy')})")

            shown = session.call("ui_state").get("structuredContent", {})
            check(shown.get("view") == "items" and not shown.get("dialogs"), "ui_state shows the items view")

            controls = session.call("ui_controls").get("structuredContent", {}).get("controls", [])
            check(any(c["id"] == "title/edit/address" for c in controls) and
                  any(c.get("command") == "menu_main" for c in controls),
                  f"ui_controls lists the bars' boxes and buttons, with the commands they run ({len(controls)})")
            opened = session.call("command_run", {"command": "menu_main"}).get("structuredContent", {})
            rows = [r.get("command") for r in ((opened.get("menu") or {}).get("levels") or [{}])[0].get("rows", [])]
            check(opened.get("state") == "waiting_for_dialog" and "tool_import" in rows,
                  f"command_run opens a command's menu, and answers with its rows ({rows[:4]})")
            listed = session.call("ui_controls", {"scope": "menu"}).get("structuredContent", {}).get("controls", [])
            cancelled = session.call("menu_respond", {"cancel": True}).get("structuredContent", {})
            covered = session.call("coverage").get("structuredContent", {}).get("commands", {}).get("run", [])
            check(any(c["id"] == "menu/item/Import" for c in listed) and cancelled.get("menu") is None and
                  any(c.get("name") == "menu_main" and c.get("menus") == 1 for c in covered),
                  "ui_controls lists the menu's rows, menu_respond cancels it, and coverage counts it opened")

            picture = session.call("screenshot", {"settle_s": 5})
            images = [c for c in picture.get("content", []) if c.get("type") == "image"]
            data = base64.b64decode(images[0]["data"]) if images else b""
            check(images and images[0].get("mimeType") == "image/png" and data.startswith(b"\x89PNG"),
                  "screenshot returns the window as a PNG, though it is out of sight")
            size = picture.get("structuredContent", {})
            check(size.get("width", 0) > 200 and size.get("height", 0) > 200, f"at the window's size ({size})")

            session.call("command_run", {"command": "tool_sync"})
            task = session.call("task_state").get("structuredContent", {})
            fields = {f.get("label"): f for f in task.get("panel", {}).get("fields", [])}
            needs = {c.get("name"): c.get("needs") for c in task.get("commands", [])}
            check(task.get("view") == "sync" and "Remote folder" in fields and needs.get("sync_run") == "write",
                  f"task_state reads Sync's panel, its fields by their headings and its commands ({list(fields)})")

            remote = os.path.join(root, "remote")
            os.makedirs(remote)
            wrong = session.call("task_set", {"fields": {"Nowhere": remote}})
            check(wrong.get("isError") is True and "Nothing was changed" in text_of(wrong),
                  "task_set changes nothing when a name is wrong")

            session.call("task_set", {"fields": {"Remote folder": remote}})
            session.call("command_run", {"command": "sync_analyze"})
            session.call("wait_until_idle", {"timeout_s": 60}, timeout=90)
            review = session.call("task_state", {"contains": "copy", "column": "Action"}).get(
                "structuredContent", {}).get("review", {})
            rows = review.get("items", [])
            expected = os.path.join(remote, "beach.png").lower()
            check(review.get("matching") == 3 and expected in [row[-1].lower() for row in rows],
                  f"task_set fills the panel, and analyzing reviews the copies Sync would make ({rows})")

            run = session.call("command_run", {"command": "sync_run"})
            check(run.get("isError") is True and not os.listdir(remote), "and running it needs write")

            session.call("command_run", {"command": "view_close"})
            session.call("select", {"names": ["beach.png"]})
            session.call("command_run", {"command": "tool_adjust_date"})
            panel = session.call("task_state").get("structuredContent", {}).get("panel", {})
            dates = [f.get("label") for f in panel.get("fields", []) if f.get("kind") == "date"]
            adjusted = session.call("task_set", {"fields": {dates[0]: "1985-07-14 10:30:00"}} if dates else {})
            after = adjusted.get("structuredContent", {}).get("review", {}).get("items", [])
            check(after and "1985" in after[0][2], f"a date field takes a date, and the review shows it ({after})")

            session.call("command_run", {"command": "view_close"})
            session.call("select", {"names": ["beach.png"]})
            session.call("command_run", {"command": "tool_convert"})
            converted = os.path.join(root, "converted")
            planned = session.call("task_set", {"fields": {"Destination folder": converted}}).get(
                "structuredContent", {})
            destinations = [row[1].lower() for row in planned.get("review", {}).get("items", [])]
            runnable = {c.get("name"): c.get("enabled") for c in planned.get("commands", [])}
            check(destinations == [os.path.join(converted, "beach.jpg").lower()] and runnable.get("tool_run") is True,
                  f"task_set answers once Convert has planned for the folder typed in ({destinations}, {runnable})")

            # A check box enables the field beneath it, or shows it, and the one call sets both.
            limited = session.call("task_set", {
                "checks": {"Limit output to a maximum dimension (in pixels).": True},
                "fields": {"Limit output to a maximum dimension (in pixels)": "16"}})
            changes = [row[2] for row in limited.get("structuredContent", {}).get("review", {}).get("items", [])]
            check(not limited.get("isError") and len(changes) == 1 and "16x12" in changes[0],
                  f"task_set fills in a field the check box it sets enables ({changes or text_of(limited)})")

            session.call("command_run", {"command": "view_close"})
            session.call("select", {"names": ["beach.png"]})
            session.call("command_run", {"command": "tool_edit_metadata"})
            titled = session.call("task_set", {"checks": {"Title": True}, "fields": {"Title": "Hiking"}})
            titles = [f.get("text") for f in titled.get("structuredContent", {}).get("panel", {}).get("fields", [])
                      if f.get("label") == "Title"]
            check(not titled.get("isError") and titles == ["Hiking"],
                  f"and a field the check box it sets shows ({titles or text_of(titled)})")

            session.call("command_run", {"command": "view_close"})
            suggested = session.call("location_suggest", {"query": "without:location"}).get("structuredContent", {})
            groups = suggested.get("groups", [])
            lent = groups[0] if len(groups) == 1 else {}
            walks = [i.get("path", "") for i in lent.get("items", [])]
            gaps = [i.get("gap_s") for i in lent.get("items", [])]
            check(lent.get("located", {}).get("path", "").endswith("pompei.png") and
                  [os.path.basename(p) for p in walks] == ["walk.png"] and gaps == [-300],
                  f"location_suggest finds the photo with a place taken five minutes before the walk ({groups})")

            session.call("navigate", {"to": lent.get("search", "")})
            chosen = session.call("select", {"names": walks}).get("structuredContent", {})
            session.call("command_run", {"command": "tool_locate"})
            proposed = session.call("task_state").get("structuredContent", {})
            check(chosen.get("not_shown") == [] and proposed.get("view") == "locate" and
                  proposed.get("status", "").startswith("40.75"),
                  f"selected by path in the group's search, Locate proposes that place ({proposed.get('status')})")

            session.call("command_run", {"command": "view_close"})
            session.call("navigate", {"to": collection})
            session.call("select", {"names": ["beach.png"]})
            session.call("command_run", {"command": "tool_edit"})
            edit_panel = session.call("task_state").get("structuredContent", {}).get("panel", {})
            sliders = {f.get("label") for f in edit_panel.get("fields", [])}
            check({"Straighten", "Vibrance", "Brightness"} <= sliders,
                  f"a slider is named by the label drawn beside it, or else the heading above it ({sorted(sliders)})")

            refused = session.call("help", {"topic": "nonsense"})
            check(refused.get("isError") is True, "an argument the schema refuses is the tool's error to report")

            unknown = session.request("tools/call", {"name": "movie_render", "arguments": {"projects": project}})
            check("error" in unknown, "a withheld tool cannot be called")

            check(not session.stray, f"nothing but the protocol on stdout ({session.stray[:3]})")
        except (AssertionError, queue.Empty) as e:
            failures.append(str(e) or type(e).__name__)
            print(f"  FAIL  {e}")
        finally:
            try:
                code = session.close()
                check(code == 0, f"the end of input ends the session ({code})")
            except subprocess.TimeoutExpired:
                session.process.kill()
                check(False, "the end of input ends the session")

        # A session that may write, in the same collection with a photo large enough that showing it takes a moment:
        # the window is busy until it is shown, and what a task writes is read back from the file.
        write_large_png(os.path.join(collection, "large.png"), 10000, 7500)
        outside = os.path.join(root, "outside")
        os.makedirs(outside)
        writer_stderr_path = os.path.join(root, "writer-stderr.txt")
        writer = Session([exe, "-mcp", f"-profile:{os.path.join(root, 'profile')}", f"-collection:{collection}",
                          "-mcp-allow:write"], writer_stderr_path)

        try:
            writer.call("navigate", {"to": collection})
            writer.call("wait_until_idle", {"timeout_s": 120}, timeout=150)

            # Typed into the address box as a user types: pressed into, its text replaced, then Enter.
            writer.call("ui_act", {"id": "title/edit/address", "action": "type", "text": "beach"})
            writer.call("ui_act", {"id": "title/edit/address", "action": "key", "key": "enter"})
            writer.call("wait_until_idle", {"timeout_s": 60}, timeout=90)
            searched = writer.call("ui_state").get("structuredContent", {})
            used = writer.call("coverage").get("structuredContent", {}).get("controls", {})
            check(searched.get("search") == "beach" and searched.get("shown") == 1 and used.get("used", 0) >= 1,
                  f"ui_act types into the address box, Enter searches, and coverage counts the box used ({searched})")
            helped = writer.call("command_run", {"command": "view_help"})
            check(helped.get("isError") is True and "-mcp-allow:launch" in text_of(helped),
                  "opening a web page needs launch, which write does not include")
            writer.call("navigate", {"to": collection})
            writer.call("wait_until_idle", {"timeout_s": 60}, timeout=90)

            writer.call("select", {"names": ["large.png"]})
            writer.call("command_run", {"command": "view_fullscreen"})
            showing = writer.call("wait_until_idle", {"timeout_s": 0, "quiet_ms": 0}).get("structuredContent", {})
            check("loading media" in showing.get("busy", []),
                  f"a photo still being read or decoded keeps the window busy ({showing})")
            shown = writer.call("wait_until_idle", {"timeout_s": 90}, timeout=120).get("structuredContent", {})
            check(shown.get("idle") is True, f"until it is shown ({shown.get('busy')})")

            writer.call("command_run", {"command": "view_fullscreen"})
            writer.call("select", {"names": ["beach.png"]})
            writer.call("command_run", {"command": "tool_convert"})
            writer.call("task_set", {"fields": {"Destination folder": outside}})
            writer.call("command_run", {"command": "tool_run"})
            writer.call("wait_until_idle", {"timeout_s": 60}, timeout=90)
            written = os.path.join(outside, "beach.jpg")
            info = writer.call("item_info", {"path": written}).get("structuredContent", {})
            size = (info.get("metadata", {}).get("width"), info.get("metadata", {}).get("height"))
            check(os.path.isfile(written) and size == (32, 24),
                  f"item_info reads a file Convert has just written, not the index's unread record of it ({info})")

            check(not writer.stray, f"nothing but the protocol on stdout ({writer.stray[:3]})")
        except (AssertionError, queue.Empty) as e:
            failures.append(str(e) or type(e).__name__)
            print(f"  FAIL  {e}")
        finally:
            try:
                code = writer.close()
                check(code == 0, f"the end of input ends the session that may write ({code})")
            except subprocess.TimeoutExpired:
                writer.process.kill()
                check(False, "the end of input ends the session that may write")

        if failures:
            for name, path in (("session", stderr_path), ("session that may write", writer_stderr_path)):
                with open(path, encoding="utf-8", errors="replace") as f:
                    print(f"\n  The {name}'s stderr, last lines:")
                    for line in f.read().splitlines()[-30:]:
                        print(f"    {line}")

    print(f"Agent session: {checks} checks, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: test_mcp_session.py <path to diffractor>")
    sys.exit(main(sys.argv[1]))
