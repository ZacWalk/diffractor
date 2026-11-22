#!/usr/bin/env python3
# Purpose: Drives an agent session (-mcp) end to end over its pipes, as an MCP client does: the handshake each era of
# client uses, the tool list and what the session withholds, a Movie project checked without a window, the window made
# out of sight by the first tool that needs it, the index searched, the window's controls listed and a menu opened, read
# and cancelled, a screenshot, task panels filled in and reviewed - fields a check box shows or enables among them - a
# place suggested from the photo taken nearest in time and proposed by Locate, a tool's argument refused, and the end of
# input ending the session. A second session, which may write, types a search into the address box as a user does,
# waits while a large photo is read and decoded, reads back a file Convert has just written, and meets the prompt
# Windows shows when Delete finds a file held open: listed with the dialogs, ending a wait, and cancelled. A third, over
# libfacedetection's sample photographs where a build has fetched them, turns face search on for photos and then
# videos, names faces, confirms one suggestion and rejects another, hides a face and shows it again, joins two people
# and separates them, saves the face book to a file, and reads the saved book into a fourth with a profile of its own,
# over the same photos renamed and copied. dd.ps1 test runs it against the program it built; give the program's path
# to run it by hand.

import base64
import glob
import json
import os
import queue
import shutil
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


def face_samples():
    """libfacedetection's sample photographs, from the archive a build fetched: this repository keeps none of people."""
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets = os.path.join("_deps", "facedetection-src", "mobile", "Android", "FaceDetection", "app", "src", "main",
                          "assets")
    for found in sorted(glob.glob(os.path.join(repo, "tmp", "*", assets))):
        portrait, crowd = os.path.join(found, "test.jpg"), os.path.join(found, "test2.jpg")
        if os.path.isfile(portrait) and os.path.isfile(crowd):
            return portrait, crowd
    return None


def faces_session(exe, root, check, failures):
    """Name individual faces, review suggestions, hide/unhide, and transfer the book to a fresh profile."""
    samples = face_samples()
    if not samples:
        print("  SKIP  faces: libfacedetection's sample photographs are in no build tree under tmp")
        return []

    collection = os.path.join(root, "faces")
    os.makedirs(collection)
    # Distinct renderings of the public portrait: lookalikes, not five copies of one exact signature.
    with open(samples[0], "rb") as f:
        portrait = f.read()
    table = portrait.index(b"\xff\xdb") + 4
    assert portrait[table] == 0  # First 8-bit luminance quantization table.
    for i in range(5):
        variant = bytearray(portrait)
        for k in range(table + 1, table + 65):
            variant[k] = min(255, max(1, round(portrait[k] * (1 + i / 8))))
        with open(os.path.join(collection, f"ann{i + 1}.jpg"), "wb") as f:
            f.write(variant)
    shutil.copy(samples[1], os.path.join(collection, "crowd.jpg"))
    exported = os.path.join(root, "faces.csv")
    stderr_paths = []

    def state(session, wait_s=30):
        return session.call("faces_state", {"wait_s": wait_s}, timeout=wait_s + 30).get("structuredContent", {})

    def rows(st, title):
        return [g for g in st.get("people", []) if g.get("title") == title]

    def turn_on(session):
        session.call("navigate", {"to": collection})
        session.call("wait_until_idle", {"timeout_s": 120}, timeout=150)
        session.call("command_run", {"command": "faces"})
        session.call("ui_act", {"id": "panel/check/Scan for faces in photos", "action": "press"})
        session.call("dialog_respond", {"button": "Scan for faces"})
        state(session, 180)
        # docs/faces.md#31-turning-it-on-and-off: videos are turned on apart, asking again, and what the photos gave is
        # loaded again with them rather than read again.
        session.call("ui_act", {"id": "panel/check/Scan for faces in videos", "action": "press"})
        session.call("dialog_respond", {"button": "Scan for faces"})
        return state(session, 180)

    def select(session, page, cells=None):
        args = {"people": [page]}
        if cells is not None:
            args["cells"] = cells
        return session.call("faces_select", args).get("structuredContent", {})

    def named_file(items, file):
        return next(i for i in items if os.path.basename(i["file"]) == file)

    def assign(session, name):
        # docs/faces.md#95-naming-a-face: the popup's search box is called by its title, as Copy to folder's is.
        session.call("command_run", {"command": "face_name"})
        session.call("dialog_respond", {"fields": {"Name faces": name}, "button": "Name faces"})
        return state(session)

    for profile in ("faces-profile", "faces-profile2"):
        if profile == "faces-profile2":
            original = collection
            collection = os.path.join(root, "different-library")
            os.makedirs(collection)
            for old, new in (("ann1.jpg", "renamed.jpg"), ("ann2.jpg", "another.jpg"),
                             ("ann1.jpg", "copy.jpg"), ("ann3.jpg", "unrelated-name.jpg"),
                             ("crowd.jpg", "party.jpg")):
                shutil.copy(os.path.join(original, old), os.path.join(collection, new))
        stderr_paths.append(os.path.join(root, f"{profile}-stderr.txt"))
        session = Session([exe, "-mcp", f"-profile:{os.path.join(root, profile)}", f"-collection:{collection}",
                           "-mcp-allow:write"], stderr_paths[-1])
        try:
            st = turn_on(session)
            search = st.get("face_search", {})
            check(search.get("enabled") is True and search.get("settled") is True and search.get("faces", 0) >= 4 and
                  search.get("photos") is True and search.get("videos") is True,
                  f"face search, turned on for photos then videos from the Faces view's checks, reads every picture ({search})")

            if profile == "faces-profile2":
                session.call("faces_data", {"import": exported})
                st = state(session)
                check(bool(rows(st, "Ann")) and bool(rows(st, "Alex")), "a fresh profile imports both people")
                ann = select(session, "Ann")["faces"]["items"]
                check({os.path.basename(i["file"]) for i in ann if i["member"] == "confirmed"} ==
                      {"renamed.jpg", "another.jpg", "copy.jpg"}, "exact faces under new paths, including copies, stay confirmed")
                check(all(os.path.basename(i["file"]) != "unrelated-name.jpg" for i in ann),
                      "the rejected face is not offered again after export/import")
                continue

            check(bool(rows(st, "Unknown")) and bool(rows(st, "Hidden")) and not rows(st, "Unnamed"),
                  "an unlabelled collection starts with Unknown and Hidden, not inferred people")
            unknown = select(session, "Unknown")["faces"]["items"]
            first = named_file(unknown, "ann1.jpg")
            select(session, "Unknown", [first["cell"]])
            assign(session, "Ann")
            person = select(session, "Ann")["faces"]
            check(person["members"].get("confirmed") == 1 and person["members"].get("suggested", 0) >= 4,
                  "naming a face confirms only that face; similar photos are suggestions")
            # docs/faces.md#92-faces: a suggestion's third shortcut asks for the name of that face alone, offering at a
            # press nobody it is already shown as.
            suggestion = next(i for i in person["items"] if i["member"] == "suggested")
            shortcuts = suggestion.get("shortcuts", [])
            labels = [s.get("label") for s in shortcuts]
            check(labels == ["Confirm Ann", "Not Ann", "Name as someone else"],
                  f"a suggestion has three shortcuts, each with where it is ({labels})")
            someone_else = next((s for s in shortcuts if s.get("label") == "Name as someone else" and s.get("at")), None)
            check(someone_else is not None, "and the third is on screen to press")
            if someone_else:
                x, y = someone_else["at"]
                pressed = session.call("ui_input", {"steps": f"point at {x} {y}\nclick"}).get("structuredContent", {})
                asked = (session.call("ui_state").get("structuredContent", {}).get("dialogs") or [{}])[-1]
                offered = [r.get("text") for r in asked.get("rows", [])]
                check(asked.get("title") == "Name face" and "Ann" not in offered,
                      f"Name as someone else asks for one face's name, not offering Ann ({asked.get('title')}, {offered})")
                if asked.get("title"):
                    session.call("dialog_respond", {"cancel": True})
                # The click's steps finish once the dialog they opened is answered.
                if pressed.get("job"):
                    session.call("job_wait", {"job": pressed["job"], "wait_s": 30}, timeout=60)
            second = named_file(person["items"], "ann2.jpg")
            pending = select(session, "Unknown")["faces"]["items"]
            check(not any(os.path.basename(i["file"]) == "ann2.jpg" for i in pending),
                  "naming one face clears its suggested matches from Unknown without confirming them")
            select(session, "Ann", [second["cell"]])
            session.call("ui_act", {"id": "status/button/Confirm Ann", "action": "press"})
            state(session)
            person = select(session, "Ann")["faces"]
            check(person["members"].get("confirmed") == 2, "Confirm changes only the selected suggestion")
            third = named_file(person["items"], "ann3.jpg")
            select(session, "Ann", [third["cell"]])
            session.call("ui_act", {"id": "status/button/Not Ann", "action": "press"})
            state(session)
            unknown = select(session, "Unknown")["faces"]["items"]
            select(session, "Unknown", [named_file(unknown, "ann3.jpg")["cell"]])
            session.call("command_run", {"command": "face_hide"})
            state(session)
            hidden = select(session, "Hidden")["faces"]["items"]
            check(len(hidden) == 1 and os.path.basename(hidden[0]["file"]) == "ann3.jpg",
                  "hiding a rejected face hides only it")
            # A key given to the view takes the keyboard there first, away from the bar button pressed last.
            select(session, "Hidden", [hidden[0]["cell"]])
            session.call("ui_act", {"id": "view/content/faces", "action": "key", "key": "h"})
            state(session)
            unknown = select(session, "Unknown")["faces"]["items"]
            check(not select(session, "Hidden")["faces"]["items"] and
                  any(os.path.basename(i["file"]) == "ann3.jpg" for i in unknown),
                  "H, given to the Faces view, shows the hidden face again")
            select(session, "Unknown", [named_file(unknown, "ann3.jpg")["cell"]])
            session.call("ui_act", {"id": "view/content/faces", "action": "key", "key": "h"})
            state(session)
            hidden = select(session, "Hidden")["faces"]["items"]
            check(len(hidden) == 1 and os.path.basename(hidden[0]["file"]) == "ann3.jpg", "and H hides it again")
            select(session, "Hidden", [hidden[0]["cell"]])
            assign(session, "Alex")
            check(not select(session, "Hidden")["faces"]["items"], "naming a hidden face reveals it")
            check(select(session, "Alex")["faces"]["members"].get("confirmed") == 1, "Alex has one confirmed face")
            session.call("ui_act", {"id": "status/button/Undo", "action": "press"})
            state(session)
            hidden = select(session, "Hidden")["faces"]["items"]
            check(len(hidden) == 1, "undoing the assignment restores the hidden state")
            select(session, "Hidden", [hidden[0]["cell"]])
            session.call("command_run", {"command": "face_show_again"})
            state(session)
            check(all(os.path.basename(i["file"]) != "ann3.jpg" for i in select(session, "Ann")["faces"]["items"]),
                  "showing a face again does not clear Not Ann")
            unknown = select(session, "Unknown")["faces"]["items"]
            select(session, "Unknown", [named_file(unknown, "ann3.jpg")["cell"]])
            assign(session, "Alex")
            session.call("ui_act", {"id": "status/button/Alex", "action": "press"})
            check(state(session)["faces"]["members"].get("confirmed") == 1, "the receipt opens Alex")
            # docs/faces.md#95-naming-a-face: a face as like Alex as Ann stays in Unknown, and naming it lists the person it
            # is most like, chosen; choosing a row names it and closes the popup at once.
            unknown = select(session, "Unknown")["faces"]["items"]
            alike = next((i for i in unknown if os.path.basename(i["file"]) in ("ann4.jpg", "ann5.jpg")), None)
            check(alike is not None, "a face as like Alex as Ann stays in Unknown")
            if alike:
                select(session, "Unknown", [alike["cell"]])
                session.call("command_run", {"command": "face_name"})
                asked = (session.call("ui_state").get("structuredContent", {}).get("dialogs") or [{}])[-1]
                listed = asked.get("rows", [])
                offered = [r.get("text") for r in listed]
                check("Alex" in offered and bool(listed) and listed[0].get("selected") is True,
                      f"naming it lists Alex, the most likely first and chosen ({listed})")
                if "Alex" in offered:
                    session.call("dialog_respond", {"choose": "Alex"})
                    state(session)
                    named = select(session, "Alex")["faces"]["members"].get("confirmed")
                    left = session.call("ui_state").get("structuredContent", {}).get("dialogs")
                    check(named == 2 and not left, f"choosing Alex names it and closes the popup ({named}, {left})")
                    session.call("ui_act", {"id": "status/button/Undo", "action": "press"})
                    state(session)
                else:
                    session.call("dialog_respond", {"cancel": True})
            for button in ("Cancel", "OK"):
                select(session, "Alex")
                session.call("command_run", {"command": "face_rename_person"})
                prompt = session.call("dialog_respond", {"fields": {"Name": " ann "}, "button": "OK"})
                check("Combine Alex with Ann?" in text_of(prompt), "Rename asks before merging into the existing spelling")
                session.call("dialog_respond", {"button": button})
                current = state(session)
                if button == "Cancel":
                    check(bool(rows(current, "Alex")) and select(session, "Ann")["faces"]["members"].get("confirmed") == 2,
                          "Cancel keeps both people and their confirmations")
                else:
                    check(not rows(current, "Alex") and select(session, "Ann")["faces"]["members"].get("confirmed") == 3,
                          "OK combines into Ann; Alex's confirmed face overrides its earlier Not Ann")
            session.call("ui_act", {"id": "status/button/Undo", "action": "press"})
            check(bool(rows(state(session), "Alex")), "Undo restores the separate identities before export")
            # A panel button scrolled out of sight is scrolled to and pressed in one call, as a user scrolls to it.
            session.call("window", {"width": 1100, "height": 640})
            panel = session.call("ui_controls", {"scope": "panel"}).get("structuredContent", {}).get("controls", [])
            delete = next(c for c in panel if c["id"] == "panel/button/Delete face data")
            pressed = session.call("ui_act", {"id": delete["id"], "action": "press"})
            asked = session.call("ui_state").get("structuredContent", {}).get("dialogs") or []
            check(delete.get("scrolled_away") is True and any(d.get("title") == "Delete face data" for d in asked),
                  f"a panel button scrolled away is pressed once scrolled into view ({text_of(pressed)[:80]})")
            if asked:
                session.call("dialog_respond", {"cancel": True})
            session.call("faces_data", {"export": exported})
            with open(exported, encoding="utf-8-sig") as f:
                data = f.read()
            check(data.startswith("Diffractor faces,4") and ".jpg" not in data and collection not in data,
                  "the exported face signatures include no photo filenames or paths")
            session.call("command_run", {"command": "view_items"})
            session.call("select", {"names": ["ann1.jpg"]})
            session.call("wait_until_idle", {"timeout_s": 60}, timeout=90)
            shown = state(session).get("picture", {}).get("faces", [])
            check(len(shown) == 1 and shown[0].get("name") == "Ann" and not shown[0].get("offered"),
                  "the picture overlay uses the confirmed name")
            check(not session.stray, f"nothing but the protocol on stdout ({session.stray[:3]})")
        except (AssertionError, queue.Empty, KeyError, IndexError, TypeError) as e:
            failures.append(str(e) or type(e).__name__)
            print(f"  FAIL  faces: {e!r}")
        finally:
            try:
                code = session.close()
                check(code == 0, f"the end of input ends the faces session ({code})")
            except subprocess.TimeoutExpired:
                session.process.kill()
                check(False, "the end of input ends the faces session")

    return stderr_paths


def windows_prompt(session, collection, check):
    """A file another process holds open stops Delete at Windows' own File In Use prompt. The session lists it with
    Diffractor's dialogs, marked system, with its buttons; wait_until_idle answers with it at once rather than waiting
    out its time; and dialog_respond cancels it, keeping the file."""
    import ctypes
    import ctypes.wintypes as wintypes

    held = os.path.join(collection, "held.png")
    write_png(held)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.restype = wintypes.HANDLE
    invalid = wintypes.HANDLE(-1).value
    # Held open, shared with nobody - once the session has let go of it, as it reads what arrives in its collection.
    handle = invalid
    for _ in range(100):
        handle = kernel32.CreateFileW(held, 0x80000000, 0, None, 3, 0, None)  # GENERIC_READ, OPEN_EXISTING
        if handle not in (None, invalid):
            break
        time.sleep(0.1)
    check(handle not in (None, invalid), "a file is held open by another program")
    try:
        session.call("command_run", {"command": "view_close"})
        session.call("navigate", {"to": collection})
        session.call("wait_until_idle", {"timeout_s": 60}, timeout=90)
        chosen = session.call("select", {"names": ["held.png"]}).get("structuredContent", {})
        asked = session.call("command_run", {"command": "tool_delete"})
        check(chosen.get("selected") == 1 and "Delete" in text_of(asked),
              f"Delete asks before it deletes the held file ({text_of(asked)[:120]!r})")
        answered = session.call("dialog_respond", {"button": "Delete"}).get("structuredContent", {})
        prompts = [d for d in answered.get("dialogs", []) if d.get("system")]
        check(prompts and prompts[-1].get("buttons"),
              f"a prompt Windows shows during Delete is listed with the dialogs, with its buttons ({prompts})")
        waited = session.call("wait_until_idle", {"timeout_s": 30}, timeout=60)
        content = waited.get("structuredContent", {})
        check(content.get("idle") is False and content.get("waited_s", 30) < 10 and content.get("dialogs"),
              f"wait_until_idle answers with it at once ({text_of(waited)[:120]!r})")
        session.call("dialog_respond", {"cancel": True})
        left = [d for d in session.call("ui_state").get("structuredContent", {}).get("dialogs", []) if d.get("system")]
        check(not left and os.path.isfile(held), f"and dialog_respond cancels it, keeping the file ({left})")
    finally:
        if handle not in (None, invalid):
            kernel32.CloseHandle(handle)


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
            check({"status", "help", "movie_check", "search", "ui_state", "screenshot", "faces_state"} <= names,
                  "the tools offered")
            check("movie_render" not in names and "ui_input" not in names and "faces_data" not in names,
                  "and write's withheld without write")

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

            toolbar = {c.get("command"): c["bounds"] for c in controls if c["scope"] == "title"}
            check("explore" in toolbar and "faces" in toolbar and toolbar["explore"][0] < toolbar["faces"][0],
                  "Faces is on the navigation toolbar beside Explore")
            check("browse_back" in toolbar and "browse_forward" in toolbar, "Items keeps its Back and Forward buttons")
            original_window = session.call("window").get("structuredContent", {})
            session.call("window", {"width": 880, "height": 690})
            narrow = session.call("ui_controls", {"scope": "title"}).get("structuredContent", {}).get("controls", [])
            check(any(c.get("command") == "menu_main" for c in narrow) and
                  any(c.get("command") == "browse_back" for c in narrow) and
                  any(c["id"] == "title/edit/address" for c in narrow),
                  "a narrow Items window keeps Back, the address box and the menu")
            session.call("window", {"width": original_window["width"], "height": original_window["height"]})
            for view in ("explore", "faces"):
                session.call("command_run", {"command": view})
                visible = session.call("ui_controls").get("structuredContent", {}).get("controls", [])
                buttons = sorted((c for c in visible if c["scope"] == "title" and c["kind"] == "button"),
                                 key=lambda c: c["bounds"][0])
                check([c.get("command") for c in buttons] in
                      (["browse_back", "view_maximize", "view_close"], ["browse_back", "view_restore", "view_close"])
                      and buttons[0]["bounds"][0] > size["width"] / 2
                      and buttons[0]["bounds"][0] + buttons[0]["bounds"][2] == buttons[1]["bounds"][0],
                      f"Back starts the right-hand {view} toolbar, with no Forward button")
                if view == "explore":
                    zoom = {c.get("command"): c for c in visible if c["scope"] == "status"}
                    check(all(zoom.get(name, {}).get("enabled") and zoom[name]["bounds"][0] < size["width"] / 2
                              for name in ("view_zoom_in", "view_zoom_out")), "Explore zoom is under the globe")
                    for name in ("view_zoom_in", "view_zoom_out"):
                        check(not session.call("command_run", {"command": name}).get("isError"),
                              f"{name} runs in Explore")
                session.call("command_run", {"command": "view_close"})

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
            pair = ["beach.png", "walk.png"]
            counts = [session.call("select", {"names": pair}).get("structuredContent", {}).get("selected")
                      for _ in range(2)]
            check(counts == [2, 2], f"selecting what is already selected keeps all of it selected ({counts})")
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

            writer.call("select", {"names": ["beach.png"]})
            def sidebar_visible():
                return bool(writer.call("ui_controls", {"scope": "sidebar", "limit": 1})
                            .get("structuredContent", {}).get("controls"))

            before_sidebar = sidebar_visible()
            writer.call("ui_input", {"steps": "key shift+f4"})
            check(sidebar_visible() != before_sidebar, "Shift+F4 toggles the sidebar")
            writer.call("ui_input", {"steps": "key shift+f4"})
            check(sidebar_visible() == before_sidebar, "Shift+F4 restores the sidebar")
            for key, view in (("f4", "explore"), ("f8", "faces"), ("ctrl+f8", "batch"), ("f11", "media")):
                writer.call("ui_input", {"steps": f"key {key}"})
                reached = writer.call("ui_state").get("structuredContent", {}).get("view")
                check(reached == view, f"{key} opens {view} from Items (reached {reached})")
                writer.call("ui_input", {"steps": "key ctrl+f4"})
                after = writer.call("ui_state").get("structuredContent", {})
                check(after.get("view") == "items" and not after.get("full_screen"),
                      f"Ctrl+F4 returns from {view} to windowed Items")

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
            for fmt in ("PNG", "WEBP", "JPEG XL", "AVIF", "JPEG"):
                panel = writer.call("ui_controls", {"scope": "panel"})["structuredContent"]["controls"]
                radio = next(c for c in panel if c["kind"] == "radio" and c["label"].startswith(fmt + " -"))
                writer.call("ui_act", {"id": radio["id"], "action": "press"})
                panel = writer.call("ui_controls", {"scope": "panel"})["structuredContent"]["controls"]
                sliders = [i for i, c in enumerate(panel) if c["kind"] == "slider"]
                conflicts = [i for i, c in enumerate(panel) if c["kind"] == "radio"]
                lossless = [c for c in panel if c["label"] == "Lossless compression"]
                check(len(sliders) == int(fmt != "PNG") and all(i > max(conflicts) for i in sliders) and
                      len(lossless) == int(fmt in ("WEBP", "JPEG XL", "AVIF")),
                      f"only {fmt}'s encoding controls are shown, after conflict handling")
            writer.call("task_set", {"fields": {"Destination folder": outside}})
            writer.call("command_run", {"command": "tool_run"})
            writer.call("wait_until_idle", {"timeout_s": 60}, timeout=90)
            written = os.path.join(outside, "beach.jpg")
            info = writer.call("item_info", {"path": written}).get("structuredContent", {})
            size = (info.get("metadata", {}).get("width"), info.get("metadata", {}).get("height"))
            check(os.path.isfile(written) and size == (32, 24),
                  f"item_info reads a file Convert has just written, not the index's unread record of it ({info})")

            if sys.platform == "win32":
                windows_prompt(writer, collection, check)

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

        faces_stderr_paths = faces_session(exe, root, check, failures)

        if failures:
            logs = [("session", stderr_path), ("session that may write", writer_stderr_path)]
            logs += [(f"faces session {i + 1}", path) for i, path in enumerate(faces_stderr_paths)]
            for name, path in logs:
                if not os.path.isfile(path):
                    continue
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
