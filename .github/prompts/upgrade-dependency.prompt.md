---
description: Upgrade a fetched dependency without losing an integration patch.
---

# Upgrade a dependency

Owning document: [third-party](../../docs/third-party.md).

Ask which dependency and which target version if not already stated.

1. **Never patch a fetched dependency.** Everything Diffractor owns about one is its pin in
   `cmake/dd-dependencies.json`, `cmake/vendored`, the CMake dependency and policy files, or the
   owned wrapper — the `files_*` decoder, [av_format.cpp](../../src/av_format.cpp),
   [metadata_xmp.cpp](../../src/metadata_xmp.cpp), [util_spell.cpp](../../src/util_spell.cpp) or
   [util_zip.cpp](../../src/util_zip.cpp). If a fix seems to require editing the library's source, it
   belongs in the wrapper or the build configuration, or upstream: Diffractor keeps no fork.

2. **Inventory the integration constraints first.** Read [third-party.md](../../docs/third-party.md)
   for this dependency and list every constraint, why it exists, and the test that catches its loss.
   Several are invisible until one specific test fails — that is the point of the list, and it is the
   thing an upgrade silently drops.

3. **Regenerate, then re-apply.** For FFmpeg, the Windows configuration in
   `cmake/vendored/ffmpeg-windows` is regenerated for the new version and compared with Linux's by
   `tools/compare_ffmpeg_config.py`, which CI runs. The guard is not "no divergence" but "no
   divergence that has not been accounted for": record any new platform switch beside the Windows
   configuration.

4. **Prove it through the wrapper, not the library.** The library's own suite says nothing about
   Diffractor's integration. Run `/test:*metadata*`, `/test:*video*`, `/test:*audio*` and the format
   tests in [test_files.cpp](../../src/test_files.cpp), plus anything the patch list named.

5. **Move the pin and update the document.** Put the new archive's URL and SHA-256 in
   `cmake/dd-dependencies.json`, and record the new version, any constraint that changed shape, and
   any that is no longer needed because upstream took it.

6. **Full validation**, because a dependency change has no narrow blast radius:

   ```powershell
   .\dd.ps1 test
   ```

Report: the version moved from and to, each constraint kept or retired, the tests run, and anything
you could not verify — an ARM64 or Linux path you did not build is worth saying out loud.
