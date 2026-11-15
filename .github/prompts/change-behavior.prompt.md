---
description: Add or change a user-visible behavior without breaking the product's design rules.
---

# Change a user-visible behavior

Use this when the change alters what a user can observe: scope, contents, target, effect,
presentation, timing, or recovery. [AGENTS.md](../../AGENTS.md#behavior-changes) says when a
rendering or performance change counts.

1. **Route before reading.** Find the row in the routing table in [AGENTS.md](../../AGENTS.md) that
   matches the behavior. Read that document's named section and open the listed source. Do not read
   the rest of the set to build context.

2. **State the behavior** in [design.md](../../docs/design.md#user-mental-model)'s terms: its scope,
   contents, target and effect, including the affected count, destinations and collisions, retained
   originals, and recovery. Where design.md does not decide one of them, **HALT and ask** the owner
   rather than inventing a state, an alias or a target.

3. **Check it against the design invariants** the product promises:
   - Does the user know the target before the command runs? Consequential commands name the action,
     count, source, destination, collisions, retained originals, and recovery.
   - Is there a preview stage, and does Run execute only the validated snapshot?
   - Is every collision resolved by one explicit stated policy? Silently resolving one is not
     permitted even when the resolution is safe.
   - Is a command the platform cannot perform **absent** rather than dimmed?
   - Does converged state refresh without unrelated navigation?

4. **Confirm the implementation boundaries** before editing: the UI thread stays free of I/O,
   database, decoding and network work, and no paint path reaches SQLite or scans files.

5. **Update the owning document in the same change.** A behavior change that leaves the document
   describing the old behavior has created a disagreement an agent cannot detect. Keep the
   **Where this lives** anchors accurate.

6. **Validate narrowest first**, then broaden:

   ```powershell
   python tools\dd.py build
   .\exe\diffractor64-d.exe /test:*subject* | Out-Host
   pwsh -File tools/lint_repo.ps1
   .\dd.ps1 test
   ```

Report: the behavior as stated, the document updated, the checks run with their outcomes, and any
check you could not run.
