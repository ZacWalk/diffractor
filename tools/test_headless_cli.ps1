# Purpose: Verify headless label selection without building or starting the application.
$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path (Split-Path $PSScriptRoot) 'dd.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw "dd.ps1 has syntax errors: $errors" }
$definition = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Assert-HeadlessTestSelection'
}, $false))
if ($definition.Count -ne 1) { throw 'Expected one headless label validator.' }
. ([ScriptBlock]::Create($definition[0].Extent.Text))
$checks = 0
foreach ($case in @(
    @{ arguments = @(); accepted = $true },
    @{ arguments = @('--label', 'headless'); accepted = $true },
    @{ arguments = @('--label', '^headless$'); accepted = $true },
    @{ arguments = @('--label', 'never'); accepted = $false },
    @{ arguments = @('--label', 'HEADLESS'); accepted = $false },
    @{ arguments = @('--label', '['); accepted = $false },
    @{ arguments = @('--label'); accepted = $false },
    @{ arguments = @('--verbose'); accepted = $false },
    @{ arguments = @('--label', 'headless', '--filter', '*'); accepted = $false }
)) {
    ++$checks
    $accepted = $true
    try { Assert-HeadlessTestSelection -Arguments $case.arguments }
    catch { $accepted = $false }
    if ($accepted -ne $case.accepted) { throw "Wrong selection result for '$($case.arguments -join ' ')'." }
}
$binding = [ScriptBlock]::Create($ast.ParamBlock.Extent.Text + @'

Assert-HeadlessTestSelection -Arguments $Rest -VerboseRequested:($PSBoundParameters.ContainsKey('Verbose'))
'@)
++$checks
$rejected = $false
try { & $binding test --verbose }
catch { $rejected = $true }
if (-not $rejected) { throw 'The PowerShell common Verbose parameter must not bypass test-option validation.' }
Write-Host "Headless CLI: $checks checks, 0 failed"
