---
description: Investigate a Diffractor crash report or dump and attribute the fault to a cause.
---

# Triage a crash

Owning document: [crash investigation](../../docs/crash.md). Follow its numbered sections; this
prompt is the order to work in, not a replacement for them.

Ask for the dump, report, or log if none was supplied.

1. **Read the report before the dump.** Version, build, backend, and the open-file list are usually
   enough to classify the fault. `util_crash_files_db.h` records which media files were open when
   the process faulted.

2. **Symbolize** using the store setup in [crash.md](../../docs/crash.md). Do not guess at frames
   from names alone.

3. **Classify before fixing.** Which of these is it?
   - A **decode fault** on a specific file — the crash-guard skip list is the containing behavior,
     and the file should present as a failed item, not one still loading.
   - A **graphics fault** — check whether the hardware-acceleration fallback fired. If it did not,
     that is a second defect alongside the first.
   - A **startup fault** before the first window — the degraded-start path owns this, and the user
     has no mental model for it. Two consecutive unsettled launches must open with the default
     layout and acceleration off, saying so.
   - A **lifetime or currency fault** — a worker result applied to an object that moved on. Check
     for a `weak_ptr::lock()` with no accompanying generation, path, or request check; lifetime is
     not currency. Also check for a handle whose lifetime was bound to visibility rather than
     existence: Diffractor 1.27.0 shipped a startup crash that was exactly that.

4. **State the hypothesis as something falsifiable**, then find the discriminating check. Cap at
   three correction attempts on one hypothesis, then stop and report.

5. **Record the invariant.** If the cause is a setting, a driver, or a fallback that did not fire,
   write the invariant into the owning document so the next reader does not re-derive it. Use the
   ownership table in [AGENTS.md](../../AGENTS.md) to find that document.

6. **Clean up.** Expanded images and PDBs are hundreds of megabytes; delete them from `tmp\`. They
   are reproducible from the store.

Report: the fault, the frame it was attributed to, the evidence for the attribution, whether a fix
is proposed or only a diagnosis, and which document you recorded the invariant in.
