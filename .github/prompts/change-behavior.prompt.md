---
description: Add or change a user-visible behavior against the closed design ontology.
---

# Change a user-visible behavior

Use this when the change alters what a user can observe: scope, contents, target, effect,
presentation, or recovery. For work that changes no observable behavior, the `Internal` class in
[AGENTS.md](../../AGENTS.md) applies instead — and reclassifying to avoid a question is a gate
violation.

1. **Route before reading.** Find the row in the routing table in [AGENTS.md](../../AGENTS.md) that
   matches the behavior. Read that document's named section and open the listed source. Do not read
   the rest of the set to build context.

2. **Fill the gate.** Output the `User-visible behavior` schema with all four fields populated from
   the closed ontology in [design.md](../../docs/design.md#user-mental-model). Every value is drawn
   from the enums; none is invented, aliased, or inferred. `All scopes` is earned — use it only when
   the behavior is provably identical in every concrete scope, and say why in `Contents`.

   If the change cannot map unambiguously to the schema, **HALT and ask**. That is the signal that
   product clarification is needed, not that the schema is inadequate.

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
   .\exe\diffractor64-d.exe /test:*subject* | Out-Host
   pwsh -File tools/lint_repo.ps1
   .\dd.ps1 test
   ```

Report: the filled gate schema, the document updated, the checks run with their outcomes, and any
check you could not run.
