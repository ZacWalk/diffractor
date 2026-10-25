<#
.SYNOPSIS
    Diffractor repository lint: the mechanically checkable subset of AGENTS.md.

.DESCRIPTION
    AGENTS.md states architectural rules as prose. Prose is advisory; this script is not.
    Every rule here corresponds to a named rule in AGENTS.md and fails the build when broken,
    so a boundary violation is caught by `.\dd.ps1 test` rather than by review.

    Only rules that can be decided by inspecting text belong here. Rules that need type or
    call-graph knowledge -- "no I/O under an index lock", "no UI-owned object captured by a
    worker" -- stay in AGENTS.md as review rules, because a lint that guesses at them would
    produce false positives and be turned off.

.PARAMETER Fix
    Reserved. No rule is auto-fixable today.

.EXAMPLE
    pwsh -File tools/lint_repo.ps1
#>

[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo

function Get-BlankedSource {
    <#
        Replaces comments and literals with spaces of the same length, so offsets and line numbers
        stay valid while brackets inside them stop counting as structure.
    #>
    param([string]$Text)

    $sb = [System.Text.StringBuilder]::new($Text)
    $n = $Text.Length
    $i = 0

    $blank = {
        param($at)
        if ($Text[$at] -ne "`n" -and $Text[$at] -ne "`r") { $sb[$at] = ' ' }
    }

    while ($i -lt $n) {
        $c = $Text[$i]
        $next = if ($i + 1 -lt $n) { $Text[$i + 1] } else { [char]0 }

        # A C++14 digit separator (100'000) does not open a character literal. The token the quote
        # ends decides it: a number starts with a digit, and a literal prefix (u8'x', L'x') does
        # not. Read as a literal, one separator blanked everything up to the next quote in the file.
        $isSeparator = $false
        if ($c -eq "'" -and $i -gt 0) {
            $s = $i
            while ($s -gt 0 -and "$($Text[$s - 1])" -match '[0-9A-Za-z_.]') { $s-- }
            $isSeparator = $s -lt $i -and [char]::IsDigit($Text[$s])
        }

        if ($c -eq '/' -and $next -eq '/') {
            while ($i -lt $n -and $Text[$i] -ne "`n") { & $blank $i; $i++ }
        }
        elseif ($c -eq '/' -and $next -eq '*') {
            & $blank $i; $i++
            while ($i -lt $n) {
                if ($Text[$i] -eq '*' -and $i + 1 -lt $n -and $Text[$i + 1] -eq '/') {
                    & $blank $i; & $blank ($i + 1); $i += 2
                    break
                }
                & $blank $i; $i++
            }
        }
        elseif ($c -eq 'R' -and $next -eq '"') {
            $open = $Text.IndexOf('(', $i + 2)
            if ($open -lt 0) { $i++; continue }
            $terminator = ')' + $Text.Substring($i + 2, $open - ($i + 2)) + '"'
            $end = $Text.IndexOf($terminator, $open)
            if ($end -lt 0) { $i++; continue }
            for ($k = $i; $k -lt $end + $terminator.Length; $k++) { & $blank $k }
            $i = $end + $terminator.Length
        }
        elseif (($c -eq '"' -or $c -eq "'") -and -not $isSeparator) {
            $quote = $c
            & $blank $i; $i++
            while ($i -lt $n -and $Text[$i] -ne $quote) {
                if ($Text[$i] -eq '\') { & $blank $i; $i++; if ($i -lt $n) { & $blank $i; $i++ }; continue }
                & $blank $i; $i++
            }
            if ($i -lt $n) { & $blank $i; $i++ }
        }
        else { $i++ }
    }

    return $sb.ToString()
}

function Find-CaptureAndMove {
    <#
        One call that both captures a variable by copy, for a lambda, and std::move-s that same
        variable into another argument. See the no-capture-and-move rule below for why that is a
        defect and what is deliberately not reported.
    #>
    param([string]$Path)

    $raw = Get-Content -Raw -LiteralPath $Path
    if (-not $raw) { return }
    $text = Get-BlankedSource $raw
    $n = $text.Length

    # Requiring a body after the bracket keeps attributes and array subscripts out.
    $lambda = [regex]'\[([^\[\]]*)\](?:\s*\([^()]*\))?(?:\s*mutable)?(?:\s*noexcept)?(?:\s*->[^{;]+)?\s*\{'

    $found = @()
    foreach ($m in $lambda.Matches($text)) {
        $copied = @()
        $declared = @()

        foreach ($part in $m.Groups[1].Value.Split(',')) {
            $p = $part.Trim()
            if (-not $p -or $p -eq 'this' -or $p -eq '*this' -or $p -eq '=' -or $p -eq '&') { continue }

            if ($p -match '^&?\s*([A-Za-z_]\w*)') { $declared += $matches[1] }

            # Only a plain by-copy capture of an outer variable can observe a move elsewhere in the
            # call: '&' does not copy, and an init-capture introduces a binding of its own.
            if ($p -notmatch '=' -and -not $p.StartsWith('&') -and $p -match '^([A-Za-z_]\w*)$') {
                $copied += $matches[1]
            }

            # Except an init-capture that copies an outer variable ([t = tree]): it reads the
            # variable when the lambda is built, so it observes a move in the same call exactly as
            # a plain capture does. Only the moving form, [t = std::move(tree)], is exempt.
            if ($p -match '^[A-Za-z_]\w*\s*=\s*([A-Za-z_]\w*)$') {
                $copied += $matches[1]
            }
        }

        $found += [pscustomobject]@{
            CaptureStart = $m.Groups[1].Index
            BraceAt      = $m.Index + $m.Length - 1
            Copied       = $copied
            Declared     = $declared
            BodyEnd      = -1
        }
    }

    if (-not $found.Count) { return }

    # One scan collects parenthesis pairs, brace pairs, and the open parentheses at each lambda.
    $parens = [System.Collections.Generic.List[int]]::new()
    $braces = [System.Collections.Generic.List[int]]::new()
    $parenEnd = @{}
    $braceEnd = @{}
    $enclosingParens = @{}
    $lambdaAt = @{}
    foreach ($f in $found) { $lambdaAt[$f.CaptureStart] = $f }

    for ($i = 0; $i -lt $n; $i++) {
        $ch = $text[$i]

        if ($lambdaAt.ContainsKey($i)) { $enclosingParens[$i] = @($parens.ToArray()) }

        if ($ch -eq '(') { $parens.Add($i) }
        elseif ($ch -eq ')') {
            if ($parens.Count) { $o = $parens[$parens.Count - 1]; $parens.RemoveAt($parens.Count - 1); $parenEnd[$o] = $i }
        }
        elseif ($ch -eq '{') { $braces.Add($i) }
        elseif ($ch -eq '}') {
            if ($braces.Count) { $o = $braces[$braces.Count - 1]; $braces.RemoveAt($braces.Count - 1); $braceEnd[$o] = $i }
        }
    }

    foreach ($f in $found) {
        $f.BodyEnd = if ($braceEnd.ContainsKey($f.BraceAt)) { $braceEnd[$f.BraceAt] } else { $f.BraceAt }
    }

    foreach ($f in $found) {
        if (-not $f.Copied.Count) { continue }

        $opens = $enclosingParens[$f.CaptureStart]
        if (-not $opens) { continue }

        # A lambda nested inside another sees the enclosing lambda's capture, not the function's
        # variable, so a move naming it further out is about a different entity.
        $shadowed = @()
        foreach ($other in $found) {
            if ($other.CaptureStart -lt $f.CaptureStart -and $other.BodyEnd -gt $f.CaptureStart) {
                $shadowed += $other.Declared
            }
        }

        foreach ($name in ($f.Copied | Select-Object -Unique)) {
            if ($shadowed -contains $name) { continue }

            $move = [regex]"std::move\(\s*$([regex]::Escape($name))\s*\)"

            foreach ($open in $opens) {
                if (-not $parenEnd.ContainsKey($open)) { continue }
                $close = $parenEnd[$open]

                # The lambda's own extent is excluded: moving the captured copy inside the body is
                # ordinary, and an init-capture is the correct way to move a value into a lambda.
                $lamStart = [Math]::Max($open, $f.CaptureStart - 1)
                $lamEnd = [Math]::Min($f.BodyEnd + 1, $close)

                $region = $text.Substring($open, $close - $open)
                $cutAt = $lamStart - $open
                $cutLen = [Math]::Max(0, $lamEnd - $lamStart)
                if ($cutAt -ge 0 -and $cutAt + $cutLen -le $region.Length) {
                    $region = $region.Remove($cutAt, $cutLen).Insert($cutAt, ' ' * $cutLen)
                }

                $hit = $move.Match($region)
                if ($hit.Success) {
                    $at = $open + $hit.Index
                    $line = ($text.Substring(0, $at) -split "`n").Count
                    "src/$(Split-Path $Path -Leaf):${line}: '$name' is captured by copy and moved in one call -- sequence the capture first"
                    break
                }
            }
        }
    }
}

try {
    # secrets.h is generated locally and git-ignored; it is not part of the source contract.
    $sourceFiles = Get-ChildItem src -File -Include *.cpp, *.h -Recurse |
        Where-Object { $_.Name -ne 'secrets.h' }

    # The documentation set is what the repository publishes, so it is what git tracks. A private
    # working note left in docs/ -- ignored, excluded, or simply not added yet -- owns nothing and
    # promises nothing, and failing the ownership rule for one only teaches the reader to ignore it.
    $trackedDocs = [System.Collections.Generic.HashSet[string]]::new(
        [string[]]@(git ls-files 'docs/*.md' 2>$null | ForEach-Object { Split-Path $_ -Leaf }),
        [System.StringComparer]::OrdinalIgnoreCase)

    $publishedDocs = { Get-ChildItem docs -Filter *.md | Where-Object { $trackedDocs.Contains($_.Name) } }

    # Instruction and prompt files carry the same routing as the docs and rot the same way.
    $docFiles = @(& $publishedDocs) +
        @(Get-ChildItem .github/instructions -Filter *.md -ErrorAction SilentlyContinue) +
        @(Get-ChildItem .github/prompts -Filter *.md -ErrorAction SilentlyContinue) +
        @(Get-Item AGENTS.md) + @(Get-Item README.md)

    $rules = [ordered]@{}

    # ---------------------------------------------------------------- code boundaries

    $rules['purpose-comment'] = @{
        Why   = 'AGENTS.md "Working rules": every src file states what it is for.'
        Check = {
            $sourceFiles | Where-Object {
                ((Get-Content $_.FullName -TotalCount 25) -join "`n") -notmatch '//\s*Purpose:'
            } | ForEach-Object { "src/$($_.Name): no '// Purpose:' in the first 25 lines" }
        }
    }

    $rules['no-app-threads'] = @{
        Why   = 'AGENTS.md "Strict code anti-patterns": use async_strategy or an existing queue, never a raw thread.'
        Check = {
            $sourceFiles |
                Where-Object { $_.Name -notmatch '^(platform_|test_)' } |
                Select-String -Pattern '\bstd::(thread|jthread)\s+[A-Za-z_]' |
                ForEach-Object { "src/$($_.Filename):$($_.LineNumber): $($_.Line.Trim())" }
        }
    }

    $rules['platform-containment'] = @{
        Why   = 'AGENTS.md "Working rules": platform-specific code exists only in platform* files.'
        Check = {
            $sourceFiles |
                Where-Object { $_.Name -notmatch '^(platform_|platform\.h$|test_platform_)' } |
                Select-String -Pattern '#include\s*[<"](?:[Ww]indows\.h|d3d11|dxgi|dwrite|shlobj|shellapi|wincodec)|\b(?:HWND|LRESULT|WPARAM|LPARAM)\b' |
                ForEach-Object { "src/$($_.Filename):$($_.LineNumber): $($_.Line.Trim())" }
        }
    }

    $rules['sqlite-containment'] = @{
        Why   = 'docs/implementation.md "SQLite connection ownership": database access belongs to its owning module.'
        Check = {
            $sourceFiles |
                Where-Object { $_.Name -notmatch '^(model_db|model_tile_cache|test_)' } |
                Select-String -Pattern '\bsqlite3_' |
                ForEach-Object { "src/$($_.Filename):$($_.LineNumber): $($_.Line.Trim())" }
        }
    }

    $rules['no-const-pointer-cast'] = @{
        Why   = 'AGENTS.md "Thread ownership": never cast away const to publish a worker result.'
        Check = {
            $sourceFiles |
                Select-String -Pattern '\bconst_pointer_cast\b' |
                ForEach-Object { "src/$($_.Filename):$($_.LineNumber): $($_.Line.Trim())" }
        }
    }

    $rules['no-capture-and-move'] = @{
        # A by-copy capture is reported, plain ([tree]) or through an init-capture ([t = tree]). A
        # '[=]' default capture would need to know which names it binds, and guessing there would
        # report the safe cases too.
        Why   = 'AGENTS.md "Argument order": a variable captured by copy and moved in one call is read in unspecified order.'
        Check = {
            $sourceFiles | ForEach-Object { Find-CaptureAndMove $_.FullName }
        }
    }

    $rules['frame-accessor'] = @{
        # ui_dialog.h is excluded because it declares both a ui::frame_ptr and a
        # ui::control_frame_ptr named _frame in different classes, and only the first is
        # covered by the rule. Text alone cannot tell the two apart.
        Why   = 'AGENTS.md "Absent handles": a ui::frame_ptr member is reached only through its no_frame() accessor.'
        Check = {
            $sourceFiles |
                Where-Object { $_.Name -ne 'ui_dialog.h' } |
                Where-Object {
                    $t = Get-Content $_.FullName -Raw
                    $t -match '(?m)^\s*(?:ui::)?frame_ptr\s+_frame\s*;' -and $t -notmatch 'control_frame_ptr\s+_frame'
                } |
                Select-String -Pattern '(?<![A-Za-z0-9_])_frame->' |
                ForEach-Object { "src/$($_.Filename):$($_.LineNumber): $($_.Line.Trim()) -- use frame() instead" }
        }
    }

    $rules['one-build-description'] = @{
        Why   = 'docs/linux.md "Retiring MSBuild": CMake is the only description of the tree, and two descriptions drift.'
        Check = {
            $tracked = @(git ls-files '*.sln' '*.vcxproj' '*.vcxproj.filters' 2>$null) |
                Where-Object { $_ }

            $tracked | ForEach-Object { "$_ -- describe the build in CMakeLists.txt or cmake/vendored/" }
        }
    }

    # ---------------------------------------------------------------- documentation integrity
    # Docs are read as fact by an agent, so a stale one is worse than a missing one.

    $rules['doc-links'] = @{
        Why   = 'A broken link in a doc an agent is told to consult sends it to the wrong place.'
        Check = {
            foreach ($d in $docFiles) {
                $text = Get-Content $d.FullName -Raw
                foreach ($m in [regex]::Matches($text, '\]\(([^)#:]+?)(?:#[^)]*)?\)')) {
                    $target = $m.Groups[1].Value.Trim()
                    if ($target -match '^(https?:|mailto:)') { continue }
                    if (-not (Test-Path (Join-Path $d.DirectoryName $target))) {
                        "$($d.Name): broken link -> $target"
                    }
                }
            }
        }
    }

    $rules['doc-anchors'] = @{
        Why   = 'A link to a section that no longer exists lands the reader at the top of a 1200-line document.'
        Check = {
            # GitHub slug rules: lower-case, punctuation dropped, spaces to hyphens.
            $headings = @{}
            $slugsFor = {
                param($path)
                $set = @{}
                foreach ($line in (Get-Content $path)) {
                    if ($line -match '^#{1,6}\s+(.*)$') {
                        $s = $Matches[1].Trim().ToLowerInvariant()
                        $s = $s -replace '`', '' -replace '\[([^\]]*)\]\([^)]*\)', '$1'
                        $s = $s -replace '[^\p{L}\p{Nd} _-]', ''
                        $set[($s -replace ' ', '-')] = $true
                    }
                }
                $set
            }

            foreach ($d in $docFiles) {
                $text = Get-Content $d.FullName -Raw
                foreach ($m in [regex]::Matches($text, '\]\(([^)\s]*?)#([^)\s]+)\)')) {
                    $target = $m.Groups[1].Value
                    $file = if ($target) { Join-Path $d.DirectoryName $target } else { $d.FullName }
                    if (-not (Test-Path $file)) { continue }   # doc-links already reports this
                    $key = (Resolve-Path $file).Path
                    if (-not $headings.ContainsKey($key)) { $headings[$key] = & $slugsFor $key }
                    $frag = $m.Groups[2].Value.ToLowerInvariant()
                    if (-not $headings[$key].ContainsKey($frag)) {
                        "$($d.Name): #$frag is not a heading in $(Split-Path $file -Leaf)"
                    }
                }
            }
        }
    }

    $rules['doc-code-anchors'] = @{
        # third-party.md is excluded: its "src/..." paths are paths inside vendored packages,
        # not paths in this repository.
        Why   = 'The "Where this lives" anchors are the routing an agent uses; a renamed file must not silently orphan one.'
        Check = {
            foreach ($d in ($docFiles | Where-Object { $_.Name -ne 'third-party.md' })) {
                $text = Get-Content $d.FullName -Raw
                # Longest extension first, and a boundary after it: bare 'h' would otherwise claim
                # the '.h' of a '.hlsl' shader and report the truncation as a missing file.
                foreach ($m in [regex]::Matches($text, '(?<![\w./-])(?:\.\./)*(src/[A-Za-z0-9_./-]+\.(?:hlsli|hlsl|cpp|h))(?![\w])')) {
                    $p = $m.Groups[1].Value
                    if (-not (Test-Path $p)) { "$($d.Name): references missing $p" }
                }
            }
        }
    }

    $rules['doc-ownership'] = @{
        Why   = 'AGENTS.md "Information ownership": every doc has exactly one owner, and the table is that claim.'
        Check = {
            $agents = Get-Content AGENTS.md -Raw
            # v-*.md are archived release notes; only the current one is named in the table.
            & $publishedDocs |
                Where-Object { $_.Name -notmatch '^v-\d' } |
                Where-Object { $agents -notmatch [regex]::Escape("docs/$($_.Name)") } |
                ForEach-Object { "AGENTS.md: docs/$($_.Name) has no owner row in the ownership table" }
        }
    }

    $rules['doc-where-this-lives'] = @{
        # v-*.md are version records -- release notes and post-release context -- not subject
        # documents, and own no code.
        Why   = 'AGENTS.md: each subject document names the source that implements it, so a reader can route without searching.'
        Check = {
            & $publishedDocs |
                Where-Object { $_.Name -notmatch '^v-' } |
                Where-Object { (Get-Content $_.FullName -Raw) -notmatch '(?m)^##+\s+Where this lives\s*$' } |
                ForEach-Object { "docs/$($_.Name): no '## Where this lives' section" }
        }
    }

    $rules['test-taxonomy'] = @{
        Why   = 'docs/testing.md "Taxonomy": a test file with no row is a subject nobody has claimed.'
        Check = {
            $testing = Get-Content docs\testing.md -Raw
            Get-ChildItem src -Filter 'test_*.cpp' |
                Where-Object { $testing -notmatch [regex]::Escape($_.Name) } |
                ForEach-Object { "docs/testing.md: no row for src/$($_.Name)" }
        }
    }

    # ---------------------------------------------------------------- run

    $failed = 0
    $violationCount = 0

    foreach ($name in $rules.Keys) {
        $violations = @(& $rules[$name].Check)

        if ($violations.Count -eq 0) {
            if (-not $Quiet) { Write-Host ("  PASS  {0}" -f $name) -ForegroundColor DarkGray }
        }
        else {
            $failed++
            $violationCount += $violations.Count
            Write-Host ("  FAIL  {0} ({1})" -f $name, $violations.Count) -ForegroundColor Red
            Write-Host ("        {0}" -f $rules[$name].Why) -ForegroundColor Yellow
            $violations | Select-Object -First 20 | ForEach-Object { Write-Host "        $_" }
            if ($violations.Count -gt 20) {
                Write-Host ("        ... and {0} more" -f ($violations.Count - 20))
            }
        }
    }

    Write-Host ""
    if ($failed -eq 0) {
        Write-Host ("Lint passed: {0} rules, no violations." -f $rules.Count) -ForegroundColor Green
        exit 0
    }

    Write-Host ("Lint FAILED: {0} of {1} rules, {2} violations." -f $failed, $rules.Count, $violationCount) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
