param([switch]$Apply)

# Purpose: Report, and with -Apply remove, the ncnn layer sources this build does not compile. The
# enabled set is the one cmake/vendored/ncnn.cmake names; this script exists so the vendored tree
# and that list can be re-synchronised after an ncnn upgrade rather than drifting apart.

$ErrorActionPreference = 'Stop'
$ncnn = Join-Path $PSScriptRoot '..\third-party\ncnn' | Resolve-Path

$keepLine = ([regex]::Match(
        (Get-Content (Join-Path $PSScriptRoot '..\cmake\vendored\ncnn.cmake') -Raw),
        '(?s)set\(DIFFRACTOR_NCNN_LAYERS(.*?)\)')).Groups[1].Value
$keep = $keepLine -split '\s+' | Where-Object { $_ }

$all = Select-String -Path "$ncnn\src\CMakeLists.txt" -Pattern '^ncnn_add_layer\(([A-Za-z0-9_]+)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value.ToLower() }

$drop = $all | Where-Object { $_ -notin $keep }
$missing = $keep | Where-Object { $_ -notin $all }

Write-Output ("ncnn layers: {0} total, {1} kept, {2} dropped" -f $all.Count, ($all.Count - $drop.Count), $drop.Count)
if ($missing) { Write-Output ("NOT AN NCNN LAYER: {0}" -f ($missing -join ', ')) }

$dirs = @("$ncnn\src\layer", "$ncnn\src\layer\x86", "$ncnn\src\layer\arm") | Where-Object { Test-Path $_ }

$victims = foreach ($d in $dirs) {
    Get-ChildItem $d -File | Where-Object {
        $n = $_.BaseName
        $drop | Where-Object { $n -eq $_ -or $n -like "$_`_*" }
    }
}

$sum = $victims | Measure-Object Length -Sum
Write-Output ("{0} files, {1:N1} MB" -f $sum.Count, ($sum.Sum / 1MB))

if ($Apply) {
    $victims | Remove-Item -Force
    Write-Output 'removed'
}
else {
    Write-Output 'dry run; pass -Apply to remove'
}
