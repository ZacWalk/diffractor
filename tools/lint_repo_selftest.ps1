<#
.SYNOPSIS
    Self-test for tools/lint_repo.ps1: proves each rule can actually fail.

.DESCRIPTION
    docs/testing.md: "A test that cannot fail is worse than no test." That applies to the lint
    itself -- a rule whose regex silently stopped matching would report PASS forever and quietly
    stop guarding its boundary.

    Each case appends a deliberate violation to a real file, runs the lint, asserts the matching
    rule reported FAIL, then restores the file byte for byte. Restoring by bytes matters: the
    source files carry a UTF-8 BOM, and a text-mode rewrite drops it.

.EXAMPLE
    pwsh -File tools/lint_repo_selftest.ps1
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo

# Each case must break exactly one rule, in a file that rule actually covers.
$cases = @(
    @{ Rule = 'no-app-threads'; File = 'src/model_search.cpp'; Text = "`nstd::thread lint_selftest_thread;`n" }
    @{ Rule = 'platform-containment'; File = 'src/model_search.cpp'; Text = "`n#include <windows.h>`n" }
    @{ Rule = 'sqlite-containment'; File = 'src/model_search.cpp'; Text = "`n// sqlite3_step`n" }
    @{ Rule = 'no-const-pointer-cast'; File = 'src/model_search.cpp'; Text = "`n// const_pointer_cast`n" }
    # The lambda copies s and the same call moves it; MSVC evaluates the move first.
    @{ Rule = 'no-capture-and-move'; File = 'src/model_search.cpp'; Text = "`nstatic void lint_selftest_capture(std::string s) { consume([s] { return s; }, std::move(s)); }`n" }
    # An init-capture that copies is the same defect as a plain one; only the moving form is exempt.
    @{ Rule = 'no-capture-and-move'; File = 'src/model_search.cpp'; Text = "`nstatic void lint_selftest_init_capture(std::string s) { consume([t = s] { return t; }, std::move(s)); }`n" }
    # A digit separator is not a character literal: read as one, it hid the rest of the file.
    @{ Rule = 'no-capture-and-move'; File = 'src/model_search.cpp'; Text = "`nconstexpr auto lint_selftest_count = 100'000;`nstatic void lint_selftest_separated(std::string s) { consume([s] { return s; }, std::move(s)); }`n" }
    @{ Rule = 'frame-accessor'; File = 'src/ui_map.h'; Text = "`n// _frame->invalidate();`n" }
    @{ Rule = 'doc-links'; File = 'docs/testing.md'; Text = "`n[nope](does-not-exist.md)`n" }
    @{ Rule = 'doc-anchors'; File = 'docs/testing.md'; Text = "`n[nope](design.md#no-such-heading)`n" }
    @{ Rule = 'doc-code-anchors'; File = 'docs/testing.md'; Text = "`nsrc/model_does_not_exist.cpp`n" }
    # Key is not named Remove: Hashtable exposes Remove as a method, and dot access would
    # return the method object -- always truthy -- silently sending every case down this branch.
    @{ Rule = 'test-taxonomy'; File = 'docs/testing.md'; RenameToken = 'test_search.cpp' }
    @{ Rule = 'doc-where-this-lives'; File = 'docs/zoom.md'; RenameToken = '## Where this lives' }
)

$lint = Join-Path $PSScriptRoot 'lint_repo.ps1'
$missed = 0

# The child lint process holds read handles briefly after it exits, so both the patch and the
# restore retry. A restore that gives up would leave a deliberate violation in the tree.
function Set-FileBytes {
    param([string]$Path, [byte[]]$Bytes)

    for ($attempt = 1; ; $attempt++) {
        try {
            [System.IO.File]::WriteAllBytes($Path, $Bytes)
            return
        }
        catch [System.IO.IOException] {
            if ($attempt -ge 10) { throw }
            Start-Sleep -Milliseconds (50 * $attempt)
        }
    }
}

try {
    foreach ($c in $cases) {
        $path = Join-Path $repo $c.File
        $original = [System.IO.File]::ReadAllBytes($path)

        try {
            if ($c.ContainsKey('RenameToken')) {
                $text = [System.Text.Encoding]::UTF8.GetString($original).Replace($c['RenameToken'], 'test_renamed_by_selftest.cpp')
                Set-FileBytes $path ([System.Text.Encoding]::UTF8.GetBytes($text))
            }
            else {
                [byte[]]$patched = $original + [System.Text.Encoding]::UTF8.GetBytes($c['Text'])
                Set-FileBytes $path $patched
            }

            $output = & pwsh -NoProfile -File $lint -Quiet 2>&1 | Out-String

            if ($output -match ('FAIL\s+' + [regex]::Escape($c.Rule))) {
                Write-Host ("  CAUGHT  {0}" -f $c.Rule) -ForegroundColor DarkGray
            }
            else {
                $missed++
                Write-Host ("  MISSED  {0} -- the rule did not fire on a real violation" -f $c.Rule) -ForegroundColor Red
            }
        }
        finally {
            Set-FileBytes $path $original
        }
    }

    # The restores must leave the tree exactly as it was found.
    $residue = & pwsh -NoProfile -File $lint -Quiet 2>&1 | Out-String
    if ($residue -notmatch 'Lint passed') {
        Write-Host '  ERROR   lint is not clean after restore' -ForegroundColor Red
        Write-Host $residue
        exit 1
    }

    Write-Host ''
    if ($missed -eq 0) {
        Write-Host ("Lint self-test passed: {0} violations caught by the rules they break." -f $cases.Count) -ForegroundColor Green
        exit 0
    }

    Write-Host ("Lint self-test FAILED: {0} violations were missed." -f $missed) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
