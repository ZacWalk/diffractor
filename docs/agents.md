# Agent sessions

An AI agent works with Diffractor through the Model Context Protocol (MCP): Diffractor started with `-mcp`
serves its tools on stdin and stdout, one JSON-RPC message a line, to the client that started it. The tools
search the library, read what a file holds, check and render Movie projects, drive the window - its commands,
tasks, dialogs, selection and input - take screenshots, record the window and play tutorial scripts, and run the
developer's checks. The same tools can be called once from the command line with `-tool:`.

This document owns the session: how it is started, what it may do, its window, how long work and dialogs reach
the agent, the tools, and the tasks they work. The command line's other switches are the
[README](../README.md#command-line)'s; the `.dfv` project format is [movie](movie.md#6-project-files)'s; the
tutorial script grammar is in [app_tutorial.h](../src/app_tutorial.h).

## 1. Starting a session

A client starts Diffractor as a process and talks to it over its pipes. The installer puts Diffractor in
`%LOCALAPPDATA%\Diffractor`, and the Microsoft Store app answers to `diffractor.exe` on the path. GitHub
Copilot - in VS Code and the Copilot CLI - reads its servers from `%USERPROFILE%\.copilot\mcp-config.json`:

```json
{
  "mcpServers": {
    "diffractor": {
      "type": "stdio",
      "command": "C:\\Users\\<you>\\AppData\\Local\\Diffractor\\diffractor64.exe",
      "args": ["-mcp", "-mcp-allow:capture"]
    }
  }
}
```

A workspace's `.vscode/mcp.json` takes the same entry under `servers`; Claude Desktop's
`claude_desktop_config.json` and Cursor's `mcp.json` take its `command` and `args` under `mcpServers`. Grant
tiers (section 2) in `args` only as the work needs them.

| Switch | Effect |
|--------|--------|
| `-mcp` | Serves the tools on stdin and stdout. The end of input ends the session |
| `-mcp-ui:none`, `offscreen` or `visible` | Where the window is: made by the first tool that needs it, out of sight (the default); made at once, out of sight; or made at once, on the desktop |
| `-mcp-allow:<tiers>` | What beyond reading the session may do, comma-separated: `write`, `capture`, `network`, `dev` (section 2) |
| `-profile:<folder>` | Keeps settings, the index and map tiles in that folder, apart from the user's |
| `-collection:<folder>` | Makes one folder the whole collection; without `-profile`, its settings are kept in `<folder>-profile` |

Both revisions of the protocol in use are spoken: 2026-07-28, whose requests carry their revision in `_meta`
and which `server/discover` describes, and the `initialize` handshake of 2025-11-25, 2025-06-18 and 2025-03-26.
The server offers tools only. A call that asks for progress hears it; a call the client cancels is stopped and
answered with nothing. Nothing but the protocol is written to stdout: Diffractor's log goes to its log file, and
what it says to the client is on stderr.

Start with `status`, which says what the session was started with, and `help`, which explains the search
language, the `.dfv` and `.dft` formats and the tiers.

## 2. What a session may do

A session reads by default. The client's configuration, which is the user's, grants more:

| Tier | Allows |
|------|--------|
| (none) | The session's own tools, searching, reading files, checking Movie projects, the window's state, commands that change nothing outside the window, navigation and selection, and cancelling a dialog |
| `write` | Changing files: `movie_render`, `ui_input`, `ui_act`, and the commands, dialog buttons, check boxes, fields and menu rows that can change files, the clipboard or the system |
| `capture` | Pictures of the screen and of the user's media: `screenshot`, the `record_*` tools and `tutorial_run`, which needs `write` as well, as does saving a picture or a movie over a file already there |
| `network` | Reaching the network: update checks, crash reports, map tiles, dictionaries |
| `dev` | The developer's tools: `run_tests`, `validate_translations`, `generate_docs`, `duplicate_report` |
| `launch` | Opening another program, a folder or a web page on the user's desktop: the help, a file's folder, a map, Open with, and the links that open a web page |

A tool the session may not use is not offered; `status` names each one withheld and the tier that would allow
it. `command_list` marks each command that needs a tier, and `command_run` refuses those the session may not
run, `exit` - closing the input ends the session - and those that work through the system's own windows. A
command that opens a menu opens it, and what is chosen from it needs what its own command needs. The tiers hold
however a command is reached - a key, a toolbar's button, a menu row or a step of `ui_input` or `tutorial_run` -
and the log says what was not run; steps that would run a refused command are not played at all. The window's own
Close is left to whoever watches a visible session.

The session uses the user's own profile unless started with `-profile`, and it changes nothing the user's next
start finds: settings it changes last as long as it runs, and are never saved. It shares the user's index, so a
session refuses to make its window while another Diffractor has the same profile open; `-profile` gives it one
of its own. It takes no file opened from Explorer, so the user's next open never lands in its window. It shows no
system dialog - nobody is there to answer one - so a file picker answers as if cancelled and the properties sheet,
the disc burner and the share sheet are not shown, and a tile dragged in it is not handed to the system's drag,
which follows the user's own mouse. Without `launch` it opens no other program, folder or web page either,
whatever asks - a command, a menu's Open with, a link - and the log says what was not opened. Without `network`
it sends no crash report, even if its Options page asks for them.

## 3. The window

Most of the library's and the window's tools need the window, and the first of them to be called makes it and
loads the index; `status`, `help`, the Movie tools and the developer's tools do not. A window out of sight is
never shown on the desktop or the taskbar - on Windows it is cloaked - yet it is drawn, laid out and captured as
one on the desktop is, by the software renderer, and plays nothing aloud. `window` sizes it; screenshots and
recordings are made at its size.

A recording films the window as the screen shows it, so it needs `-mcp-ui:visible`. While it records, the
window is held above the others; keep the screen unlocked.

## 4. Long work, waiting and dialogs

Each tool answers within `wait_s` seconds, 50 unless the call says otherwise. Work that takes longer - a render,
the test suite, a tutorial - goes on as a job: the answer names the job and how far it has got, `job_wait` waits
for it again and answers with its result once it has finished, and `job_cancel` stops it. A render stopped leaves
no movie behind.

A dialog that asks - a confirmation, a question, a report waiting on Close - or a menu that opens stops the input
behind it, and a call waiting on work it interrupts is answered at once with the dialog: its title, text, buttons,
check boxes and fields. Work that holds the window while it runs is waited out, whether nothing shows or a dialog
shows it under way - a rating written, a batch's progress, an edit being saved - and a command that starts it is
answered once it has finished; but one that ends with files it could not change leaves its report of them waiting
on Close, which is a dialog like any other. `ui_state` shows the dialog in front, or the open menu's rows, as well.
`dialog_respond` answers it - fills fields by their label, a date as `yyyy-mm-dd hh:mm:ss`, sets check boxes and
radio buttons by their text, then presses a button by its text, or cancels it as Escape does - and answers once
the dialog has closed or another has opened. A session without `write` can only cancel. `menu_respond` chooses
from a menu - a row by its text, or by its path from the first level, `Sort by > Name` - or cancels it.
`wait_until_idle` waits
until the index has loaded and nothing is scanning, searching, indexing, making thumbnails, loading media - reading
or decoding what the window shows, from when it is asked for until the window has it - acting out steps, running a
task, working out a task's review or showing work under way, before a screenshot or after starting work whose
effects are to be read.

## 5. The tools

| Area | Tools |
|------|-------|
| The session | `help`, `status`, `wait_until_idle`, `diagnostics` (the version, system, graphics, memory, the index's counts and the session's performance counters), `log_read` (the latest log lines, or those since a mark), `job_wait`, `job_cancel`, `window` |
| The library | `search` (the address box's language, without changing the window: each file's dates and rating, the set of copies it belongs to, and its metadata where asked, in order of path, of when each was taken or of its set of copies), `item_info` (what Diffractor reads from one file: from the index once it has read the file, otherwise from the file itself), `location_suggest` (section 6) |
| The window | `ui_state`, `command_list`, `command_run`, `navigate` (a folder or a search, as the address box opens it), `select` (by file name, or by full path where a search across folders shows two of a name), `dialog_respond`, `menu_respond`, `ui_input`, `ui_controls`, `ui_act`, `coverage` |
| Tasks | `task_state`, `task_set` (section 6) |
| Pictures | `screenshot`, `record_start`, `record_mark`, `record_stop`, `tutorial_run` |
| Movie | `movie_check`, `movie_render` |
| The developer's | `run_tests`, `validate_translations`, `generate_docs`, `duplicate_report`, and in a Debug build `debug_action` |

Each tool's description and input schema, from `tools/list`, are its reference. Paths are full paths. A result
is text for the model and, where it has structure, the same as JSON in `structuredContent`.

`ui_input` acts out steps in the window as a user would, in a tutorial script's scene language: `point tile
'IMG_1.jpg'`, `click`, `double-click`, `right-click`, `drag at 0.5 0.5`, `wheel down 3`, `type 'beach'`,
`text 'a caption'`, `key escape`, `command view_fullscreen`. The pointer and keys are given to the window as its
own input, so they reach what a user's would - a control, a toolbar's button, a link, the view - and the pointer
it draws shows where, in a recording too; the user's own pointer and keyboard are not used. Each call starts with
the pointer hidden, so a click follows a point in the same call. A dialog or menu that opens ends the steps.

`ui_controls` lists everything the window offers now, each with an id: the bars' buttons, boxes and slider, the
view's links and parts, its panel's controls, the view itself and the strip, the dialog in front and the open
menu's rows - `status/button/Rotate clockwise`, `panel/edit/Folder`, `menu/item/Sort by > Name`. `ui_act` works
one by its id with a press, a double or right click, a hover, a drag, the wheel, typed text or a key, given to its
window as the platform's own input - so through whatever is in front, as a user's - and answers once the window
has caught up, or as soon as a dialog or menu opens. `coverage` counts each command run and menu opened, from
wherever, and which controls listed have been used, so a sweep that presses every button knows what it has
missed; [sweep_mcp.py](../tools/sweep_mcp.py) is one ([testing](testing.md#the-sweep)).

## 6. Tasks

The task views - Sync, Import, Locate, Tags, Rename, Convert, Metadata and Adjust Date, and Edit, Movie and
Options - each show a panel of settings beside their work. `command_run` opens one by its command:
`tool_sync`, `tool_import`, `tool_locate`, `tool_tag`, `tool_rename`, `tool_convert`, `tool_edit_metadata`,
`tool_adjust_date`, `tool_edit`, `tool_movie` or `options`; a task that works on the selection works on what
`select` chose.

`task_state` reads the task shown: its commands and whether each can run now, its status line and progress, and
its panel's text, check boxes and fields. A field is named by its own label, or else by the heading or check box
just before it; a date field holds `yyyy-mm-dd hh:mm:ss`. `task_set` fills the panel in as a user would - check
boxes and radio buttons by their text first, which can show or enable the fields beneath them, then, once the panel
shows them, fields by their name - and changes no files. Convert and Rename work their review out again as their
settings or the selection change; `task_set`, `select` and `command_run` answer once it is worked out - or, if that
takes more than a few seconds, with it still to come, which `wait_until_idle` waits for.

A task that is reviewed before it runs plans with its Analyze command and shows the plan as rows - for Sync, the
action, local and remote file of each - which `task_state` names a page at a time, narrowed to the rows with a
cell holding `contains`, or to those holding it in one `column`. Its Run command, which needs `write`, does what
the plan says; after a run, the rows say what it did to each. A session without `write` can open, fill in,
analyze and review every task, and run none.

Locate proposes, for the items selected, the place of the item with one that the window shows taken nearest in
time to the last of them. `location_suggest` finds where that applies across a search: of the photos and videos
it finds that say when they were taken but not where, those taken within `max_gap_minutes` (15 by default) of an
item in the collection with a place, in groups that share the one taken nearest each - with its path,
coordinates and place, and a search for the days they were taken on, which shows them together. A group is
placed by navigating to its search, selecting its items by path and running `tool_locate`, whose proposal
`task_state` shows in the status line, then `locate_run`.

## 7. Tutorial videos

A tutorial script (`.dft`) sets a scene, then plays beats: a line of narration each, and the steps shown while it
is spoken, timed by the narration's `.timing` files beside the script where they are and guessed from the text
where not. `tutorial_run` plays a script given by its path or its text. With `record`, it films the window to
that MP4 and writes where each beat starts beside it, one `<seconds><TAB><id>` a line in `<movie>.markers.txt`,
for the narration to be laid under the film; without, it rehearses, naming each beat as it starts - as progress
too, to a call that asks for it. A step that finds nothing to act on is counted and named in the answer. A dialog
or menu that opens is waited out, for a script cannot answer one, and the player says so.

`record_start`, `record_mark` and `record_stop` film the window for any other purpose, at 30 frames a second, the
rate Movie renders at; the names `record_mark` gives are written to the markers file in the same way.

A tutorial made with a collection of its own is the same on every machine: `-collection:<folder>` with the
pictures it shows, and `-profile:<folder>` for its settings.

## 8. Screenshots

`screenshot` takes a picture of the window, or of the dialog in front, as it is drawn - on the desktop or out of
sight - once the window has settled for up to `settle_s` seconds. It is saved where `path` says, as PNG, JPEG,
WebP, JPEG XL or AVIF by the path's extension, and returned as a PNG unless it was saved and `image` is false.
`max_size` bounds its longer side: 1600 pixels for a picture returned, none for one only saved. For pictures
for the documentation, a session with `-mcp-allow:capture`, a collection and profile of its own, `window` to set
the size, then `navigate`, `select`, `command_run` and `ui_input` to set the scene.

## 9. Testing and diagnostics

`run_tests` runs the test suite, or the tests a filter names, in a copy of the program, and answers with how
many passed and why each failure failed; `validate_translations` checks the catalogs. `diagnostics` and
`log_read` say what Diffractor is doing and what it costs: ask before and after an action to see what changed.
The suite and its taxonomy are [testing](testing.md)'s.

`.\dd.ps1 test` drives a session end to end with
[test_mcp_session.py](../tools/test_mcp_session.py): both handshakes, the tools withheld, a Movie project
checked without a window, the window made out of sight by a search, its controls listed and a menu opened, read
and cancelled, a screenshot, Sync's panel filled in, analyzed and refused a run, a date set in Adjust Date, fields
a check box shows or enables filled in by the same call, and the end of input ending the session. A second
session, which may write, types a search into the address box as a user does, finds the window busy while a large
photo is read and decoded, and reads back from the file what Convert has just written.

## 10. A single call from the command line

`-tool:<name> name=value ...` calls one tool once, without a session, and exits. Each value is read as the
tool's input schema types it: a number, `true` or `false`, a list split at commas, an object as JSON, or text.
Quote a pair whose value has a space. What the tool reports as it works goes to stderr, its result to stdout -
its structured content as JSON where it has some, otherwise its text - and the exit code is 0 for a result, 1
for an error the tool reports and 2 for a call that could not be made. A call from the command line may use every
tier, as it does only what it names; the window's tools need a session.

```powershell
& .\exe\diffractor64.exe -tool:movie_check projects=D:\Projects 2>&1 | Out-Host
& .\exe\diffractor64.exe -tool:movie_render "projects=D:\My Projects\trip.dfv" output=D:\Movies 2>&1 | Out-Host
```

Piping into `Out-Host` makes PowerShell wait for this GUI-subsystem executable and show what it writes.

## Where this lives

- [app_mcp.h](../src/app_mcp.h) and [app_mcp.cpp](../src/app_mcp.cpp): the session's host - its options and
  tiers, jobs and the calls waiting on them, the window made when a tool needs it, the log lines it keeps, the
  session's own tools, and `-tool:`. [app_mcp_json.h](../src/app_mcp_json.h): the JSON results are written with.
- [app_mcp_tools.cpp](../src/app_mcp_tools.cpp): the Movie, developer's and library tools, and the `agent` queue
  long work runs on; `location_suggest` groups what `index_state::dated_locations` and `suggest_locations` in
  [model_index_summary.cpp](../src/model_index_summary.cpp) answer. [app_mcp_ui.cpp](../src/app_mcp_ui.cpp): the
  window's tools, dialogs, task views, screenshots and recordings. [app_mcp_controls.cpp](../src/app_mcp_controls.cpp):
  `ui_controls`, `ui_act`, `menu_respond` and `coverage`, the open menu described, and the counts of what has run;
  libdesktop's `ui::send_pointer` and its kin give the input, as the platform's own.
- [app.cpp](../src/app.cpp): the switches, read into [app_command_line.h](../src/app_command_line.h); the
  session made in `parse_command_line`, its window in `start_window`, and the run settings a session applies.
  [app_settings.cpp](../src/app_settings.cpp): the settings a session keeps to itself, and `-profile`.
- [app_tutorial.h](../src/app_tutorial.h) and [app_tutorial.cpp](../src/app_tutorial.cpp): tutorial scripts and
  the player that acts them and `ui_input`'s steps out; [app_record.h](../src/app_record.h) and
  [platform_win_record.cpp](../src/platform_win_record.cpp): the recorder.
- [app_movie_batch.cpp](../src/app_movie_batch.cpp), [app_dup_report.cpp](../src/app_dup_report.cpp) and
  [app_gen_docs.cpp](../src/app_gen_docs.cpp): the work the Movie and developer's tools run.
  [app_commands.cpp](../src/app_commands.cpp): every command's name. [ui_dialog.h](../src/ui_dialog.h): the
  dialogs showing, which the tools describe and answer.
- libdesktop's [core/mcp.h](https://github.com/ZacWalk/libdesktop/blob/main/include/libdesktop/core/mcp.h): the
  protocol, the tools' schemas and results; its [agent session](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/agent.cpp)
  serves it on stdin and stdout from `platform::run`, and keeps system dialogs from being shown.
- Tests: [test_app.cpp](../src/test_app.cpp) for the tools' arguments, the session's options and settings, the
  commands' names and the Movie report; [test_util.cpp](../src/test_util.cpp) for the switches; and
  [test_mcp_session.py](../tools/test_mcp_session.py) for a session end to end.
