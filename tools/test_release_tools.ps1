# Purpose: Check SDK SymStore discovery and symbol-publishing failures without touching a real store.

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $repo 'dd.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw "dd.ps1 has syntax errors: $errors" }
foreach ($name in 'Get-SymStore', 'Add-ToSymbolStore') {
    $definition = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    if ($definition.Count -ne 1) { throw "Expected one definition of $name." }
    . ([ScriptBlock]::Create($definition[0].Extent.Text))
}

function Assert-Failure([scriptblock]$Action, [string]$Message) {
    try { & $Action }
    catch {
        if ($_.Exception.Message -notlike "*$Message*") { throw }
        return
    }
    throw "Expected failure containing '$Message'."
}

$fixture = Join-Path (Join-Path $repo 'tmp') ('release-tools-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$previousLocalAppData = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = Join-Path $fixture 'local'
    $SdkRoot = Join-Path $fixture 'Windows Kits\10\bin'
    $ToolsDir = Join-Path $fixture 'tools'
    [IO.Directory]::CreateDirectory($ToolsDir) | Out-Null
    [IO.File]::WriteAllText((Join-Path $ToolsDir 'symstore.exe'), 'Not an SDK tool.')
    Assert-Failure { Get-SymStore } 'Install the Windows SDK'

    $expected = Join-Path $fixture 'Windows Kits\10\Debuggers\x64\symstore.exe'
    [IO.Directory]::CreateDirectory((Split-Path -Parent $expected)) | Out-Null
    [IO.File]::WriteAllText($expected, 'SDK lookup fixture; never executed.')
    if ((Get-SymStore) -cne $expected) { throw 'SymStore was not taken from the SDK.' }

    $images = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsSDK\Debuggers'
    foreach ($version in '10.0.26100.7705', '10.0.26100.900') {
        $userTool = Join-Path $images "$version\Windows Kits\10\Debuggers\x64\symstore.exe"
        [IO.Directory]::CreateDirectory((Split-Path -Parent $userTool)) | Out-Null
        [IO.File]::WriteAllText($userTool, 'Per-user SDK fixture; never executed.')
    }
    if ((Get-SymStore) -cne $expected) { throw 'The installed machine SDK must take precedence.' }
    Remove-Item -LiteralPath $expected
    $newest = Join-Path $images '10.0.26100.7705\Windows Kits\10\Debuggers\x64\symstore.exe'
    if ((Get-SymStore) -cne $newest) { throw 'Select the newest SDK image by numeric version, not by text order.' }

    function Get-SymStore { 'Invoke-FakeSymStore' }
    function Invoke-FakeSymStore {
        $script:Forwarded = @($args)
        $global:LASTEXITCODE = $script:FakeExitCode
    }
    $script:Forwarded = @()
    $script:FakeExitCode = 0
    Assert-Failure { Add-ToSymbolStore -File (Join-Path $fixture 'absent.pdb') } 'Symbol input not found'
    if ($script:Forwarded.Count) { throw 'A missing input reached SymStore.' }

    $inputFile = Join-Path $fixture 'file with spaces.pdb'
    [IO.File]::WriteAllText($inputFile, 'Symbol argument fixture.')
    Add-ToSymbolStore -File $inputFile
    if (($script:Forwarded -join '|') -cne "add|/r|/f|$inputFile|/s|c:\code\symbols|/t|Diffractor|/compress") {
        throw 'Symbol-store arguments changed or a path was split.'
    }
    $script:FakeExitCode = 7
    Assert-Failure { Add-ToSymbolStore -File $inputFile } 'SymStore exit code 7'

    foreach ($name in 'Build-Desktop', 'Build-Store') {
        $definition = $ast.Find({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }, $false)
        $first = $definition.Body.EndBlock.Statements[0].Extent.Text
        if ($first -ne '$null = Get-SymStore') { throw "$name must check SymStore before changing release state." }
    }
}
finally {
    $env:LOCALAPPDATA = $previousLocalAppData
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
Write-Host 'PASS: machine/per-user SDK SymStore, version order, missing SDK/input, quoted paths, failed publish and packaging preflights.'
exit 0
