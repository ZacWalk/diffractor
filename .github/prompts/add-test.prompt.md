---
description: Add a test to the Diffractor suite, in the right file, proven able to fail.
---

# Add a test

Owning document: [testing](../../docs/testing.md).

Ask the user which behavior to constrain if it is not already clear from the conversation, then:

1. **Place it by subject, not by provenance.** Read the taxonomy table in
   [testing.md](../../docs/testing.md) and pick the file that owns the code the test constrains.
   A regression goes in that file with the issue as a comment above its registration
   (`// Issue #177 - search term negation`). There is no regressions file.

2. **Check whether the behavior is reachable headless.** `view_state` runs with
   `null_state_strategy`, `null_async_strategy` and a null `view_host_base_ptr` — see
   `browsing_fixture` in [test_app.cpp](../../src/test_app.cpp). If the decision is trapped inside a
   method that also performs it, **extract the decision into a pure function** rather than building
   a window. That is how zoom, playback advance, the address-box session and the accelerator table
   became testable, and each extraction was worth more than the code it replaced.

3. **Write it.** Assert the property, not a bound a systematic error could still satisfy. Keep it
   free of ordering, wall-clock timing, machine fonts and DPI. Do not write to any real dictionary,
   settings key or collection — use `_temps`. Do not add files to `exe/test`; its file count is
   asserted by `expected_cached_item_count` and every search-count test, so fixtures loaded by path
   go in `exe/test/excluded1`.

4. **Prove it can fail.** Break the behavior it targets, run the test, confirm it fails, restore.
   Report that you did this. A test that cannot fail is worse than no test, because it reports
   coverage that does not exist. Where the negative case is not obvious, record it in a comment.

5. **Run the narrowest check**, then the lint:

   ```powershell
   .\exe\diffractor64-d.exe /test:*subject* | Out-Host
   pwsh -File tools/lint_repo.ps1
   ```

   A filter matching nothing is a failure, not a silent pass.

Report: which file the test went in and why, the assertion it makes, the negative case you
confirmed, and the check output.
