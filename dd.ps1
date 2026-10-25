<#
.SYNOPSIS
    Diffractor Build Script

.DESCRIPTION
    Build script for Diffractor photo and video organizer.

.PARAMETER Command
    The build command to execute:
    - desktop    : Build desktop versions (diffractor32.exe, diffractor64.exe), create ZIP and NSIS installer
    - store      : Build Windows Store version (MSIX package)
    - deploy     : Deploy build artifacts to Google Cloud Storage
    - release    : Create GitHub release with tag and upload installer/zip assets
    - run        : Run the recently built diffractor64.exe
    - run32      : Run the recently built diffractor32.exe (32-bit)
    - cpu        : Run diffractor64.exe using CPU software rendering
    - build      : Bump the build number and build Release x64 (diffractor64.exe)
    - test       : Lint the repository, run unit tests and validate translation (.po) files
    - bean       : Run unit tests with the temp folder on the bean NAS (\\bean.local\home\tmp)
    - bump-build : Increment the build number (e.g., 1187 -> 1188)
    - bump-ver   : Increment the minor version (e.g., 1.26.2 -> 1.26.3)

.EXAMPLE
    .\dd.ps1 desktop
    Build desktop binaries and installer

.EXAMPLE
    .\dd.ps1 store
    Build Windows Store MSIX package

.EXAMPLE
    .\dd.ps1 release
    Create GitHub release, tag code, and upload diffractor-setup.exe and diffractor.zip

.EXAMPLE
    .\dd.ps1 run
    Run diffractor64.exe

.EXAMPLE
    .\dd.ps1 run32
    Run diffractor32.exe

.EXAMPLE
    .\dd.ps1 cpu
    Run diffractor64.exe using CPU software rendering

.EXAMPLE
    .\dd.ps1 test
    Lint the repository, run unit tests and validate translation (.po) files

.EXAMPLE
    .\dd.ps1 bump-build
    Increment build number in all version files

.EXAMPLE
    .\dd.ps1 bump-ver
    Increment minor version in all version files
#>

param(
    [Parameter(Position = 0)]
    [ValidateSet("desktop", "store", "run", "run32", "cpu", "test", "bean", "build", "bump-build", "bump-ver", "deploy", "release", "loc", "code", "clear-cache", "setup", "configure", "clean", "info", "help", "")]
    [string]$Command = "",

    # Package the version already in the tree, for rebuilding a submission that was never accepted.
    [switch]$NoBump,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest = @()
)

$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 has no $IsLinux, so the absence of the variable means Windows.
$IsLinuxHost = [bool](Get-Variable -Name IsLinux -ErrorAction SilentlyContinue) -and $IsLinux

# Configuration
$ScriptDir = $PSScriptRoot
$ToolsDir = Join-Path $ScriptDir "tools"
$SourceFilesDir = Join-Path $ScriptDir "exe"
$PackageRoot = Join-Path $ScriptDir "dist"
$InstallerDir = Join-Path $ScriptDir "installer"

# Newest installed SDK that actually has the packaging tools. Windows-only, and the probe is
# skipped on Linux so the gateway can still reach the cross-platform commands.
$SdkRoot = "C:\Program Files (x86)\Windows Kits\10\bin"
$SdkBinDir = $null
if (-not $IsLinuxHost) {
    $SdkBinDir = Get-ChildItem $SdkRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10(\.\d+){3}$' -and (Test-Path (Join-Path $_.FullName "x64\MakeAppx.exe")) } |
        Sort-Object { [version]$_.Name } |
        Select-Object -Last 1 -ExpandProperty FullName
    if ($SdkBinDir) { $SdkBinDir = Join-Path $SdkBinDir "x64" } else { $SdkBinDir = Join-Path $SdkRoot "10.0.26100.0\x64" }
}

# The Store rejects a MaxVersionTested above the newest publicly released Windows, so an
# Insider dev machine must not raise it. Bump this when a new release ships.
$MaxVersionTestedCap = "10.0.26200.0"

# Signing certificates. The desktop cert is a real CA-issued one, so SmartScreen trusts the
# installer. The Store cert is self-signed on CN=<publisher id> because the Store re-signs what it
# distributes; only the Identity/Publisher match matters, so regenerating it costs nothing.
$DesktopSignThumbprint = "B3B4EA219B9BCB79749D5E84066DDCAC61E5C4C3"
$StoreSignThumbprint = "F9B6105B47FA98932E21D265ACD3936A5F626EB6"

# Version file paths. Forward slashes so the same literals resolve on both hosts.
$NsiFile = Join-Path $InstallerDir "diff.nsi"
$AppxManifestFile = Join-Path $SourceFilesDir "AppxManifest.xml"
$ResourceFile = Join-Path $ScriptDir "src/platform_win_res.rc"
$AppManifestFile = Join-Path $ScriptDir "src/platform_win.manifest"
$AppCppFile = Join-Path $ScriptDir "src/app.cpp"

# ============================================================================
# Version Management Functions
# ============================================================================

function Get-CurrentVersion {
    # Read version from app.cpp as the source of truth
    $content = Get-Content $AppCppFile -Raw
    
    # Extract version: const std::string_view s_app_version = "126.2";
    if ($content -match 'const std::string_view s_app_version = "(\d+)\.(\d+)";') {
        $majorMinor = $Matches[1]  # e.g., "126"
        $patch = $Matches[2]       # e.g., "2"
        
        # Parse majorMinor: first digit is major, rest is minor
        $major = [int]$majorMinor.Substring(0, 1)
        $minor = [int]$majorMinor.Substring(1)
        $patchNum = [int]$patch
    }
    else {
        Write-Host "Error: Could not parse version from app.cpp" -ForegroundColor Red
        exit 1
    }
    
    # Extract build: const std::string_view g_app_build = "1206";
    if ($content -match 'const std::string_view g_app_build = "(\d+)";') {
        $build = [int]$Matches[1]
    }
    else {
        Write-Host "Error: Could not parse build number from app.cpp" -ForegroundColor Red
        exit 1
    }
    
    return @{
        Major = $major
        Minor = $minor
        Patch = $patchNum
        Build = $build
        VersionString = "$major.$minor.$patchNum"           # e.g., "1.26.2"
        AppVersion = "$major$minor.$patchNum"               # e.g., "126.2"
        FileVersion = "$major.$minor.$patchNum.$build"      # e.g., "1.26.2.1187"
        CommaVersion = "$major, $minor, $patchNum, $build"  # e.g., "1, 26, 2, 1187"
    }
}

function Update-AllVersionFiles {
    param(
        [int]$Major,
        [int]$Minor,
        [int]$Patch,
        [int]$Build
    )
    
    $appVersion = "$Major$Minor.$Patch"                    # e.g., "126.2"
    $fileVersion = "$Major.$Minor.$Patch.$Build"           # e.g., "1.26.2.1187"
    $commaVersion = "$Major, $Minor, $Patch, $Build"       # e.g., "1, 26, 2, 1187"
    $productVersion = "$Major$Minor.$Patch"                # e.g., "126.2"
    
    Write-Host ""
    Write-Host "Updating version to: $fileVersion" -ForegroundColor Cyan
    Write-Host "  App version: $appVersion"
    Write-Host "  Build: $Build"
    Write-Host ""
    
    # Update app.cpp
    Write-Host "Updating $AppCppFile..." -ForegroundColor Yellow
    $content = Get-Content $AppCppFile -Raw
    $content = $content -replace 'const std::string_view s_app_version = "\d+\.\d+";', "const std::string_view s_app_version = `"$appVersion`";"
    $content = $content -replace 'const std::string_view g_app_build = "\d+";', "const std::string_view g_app_build = `"$Build`";"
    Set-Content $AppCppFile $content -NoNewline
    
    # Update diff.nsi
    Write-Host "Updating $NsiFile..." -ForegroundColor Yellow
    $content = Get-Content $NsiFile -Raw
    $content = $content -replace '!define BUILD_NUM "\d+"', "!define BUILD_NUM `"$Build`""
    $content = $content -replace '!define PRODUCT_VERSION "\d+\.\d+"', "!define PRODUCT_VERSION `"$productVersion`""
    $content = $content -replace '!define FILE_VERSION "\d+\.\d+\.\d+\.\$\{BUILD_NUM\}"', "!define FILE_VERSION `"$Major.$Minor.$Patch.`${BUILD_NUM}`""
    Set-Content $NsiFile $content -NoNewline
    
    # Update AppxManifest.xml (Store requires revision/4th part to be 0)
    Write-Host "Updating $AppxManifestFile..." -ForegroundColor Yellow
    $storeVersion = "$Major.$Minor.$Patch.0"
    $localWinVersion = [version]"10.0.$([System.Environment]::OSVersion.Version.Build).0"
    $maxVersionTested = if ($localWinVersion -gt [version]$MaxVersionTestedCap) { $MaxVersionTestedCap } else { $localWinVersion.ToString() }
    $content = Get-Content $AppxManifestFile -Raw
    # Use specific attribute prefixes to avoid matching MinVersion
    $content = $content -replace '(<Identity[^>]*\s)Version="\d+\.\d+\.\d+\.\d+"', "`$1Version=`"$storeVersion`""
    $content = $content -replace 'MaxVersionTested="\d+\.\d+\.\d+\.\d+"', "MaxVersionTested=`"$maxVersionTested`""
    Set-Content $AppxManifestFile $content -NoNewline
    
    # Update platform_win_res.rc
    Write-Host "Updating $ResourceFile..." -ForegroundColor Yellow
    $content = Get-Content $ResourceFile -Raw
    $content = $content -replace 'FILEVERSION \d+, \d+, \d+, \d+', "FILEVERSION $commaVersion"
    $content = $content -replace 'PRODUCTVERSION \d+, \d+, \d+, \d+', "PRODUCTVERSION $commaVersion"
    $content = $content -replace 'VALUE "FileVersion", "\d+\.\d+\.\d+\.\d+"', "VALUE `"FileVersion`", `"$fileVersion`""
    $content = $content -replace 'VALUE "ProductVersion", "\d+\.\d+\.\d+\.\d+"', "VALUE `"ProductVersion`", `"$fileVersion`""
    Set-Content $ResourceFile $content -NoNewline
    
    # Update platform_win.manifest assembly identity
    # Scoped to the diffractor.exe identity so the Common-Controls dependency version is left alone.
    Write-Host "Updating $AppManifestFile..." -ForegroundColor Yellow
    $content = Get-Content $AppManifestFile -Raw
    $content = $content -replace '(name="diffractor\.exe"\s+)version="\d+\.\d+\.\d+\.\d+"', "`$1version=`"$fileVersion`""
    Set-Content $AppManifestFile $content -NoNewline
    
    # $StorePackageName is computed from the exe's own FileVersion further down, so there is no
    # literal here to rewrite - the version bump reaches it for free.

    Write-Host ""
    Write-Host "Version updated successfully!" -ForegroundColor Green
    Write-Host ""
}

function Invoke-BumpBuild {
    $version = Get-CurrentVersion
    $newBuild = $version.Build + 1
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Incrementing Build Number" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Current: $($version.FileVersion)"
    Write-Host "New:     $($version.Major).$($version.Minor).$($version.Patch).$newBuild"
    
    Update-AllVersionFiles -Major $version.Major -Minor $version.Minor -Patch $version.Patch -Build $newBuild
}

function Invoke-BumpVersion {
    $version = Get-CurrentVersion
    $newPatch = $version.Patch + 1
    $newBuild = $version.Build + 1
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Incrementing Version Number" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Current: $($version.FileVersion)"
    Write-Host "New:     $($version.Major).$($version.Minor).$newPatch.$newBuild"
    
    Update-AllVersionFiles -Major $version.Major -Minor $version.Minor -Patch $newPatch -Build $newBuild
}

# ============================================================================
# Build Functions
# ============================================================================

# Dynamic store package name based on current version
$currentVer = Get-CurrentVersion
$StorePackageName = "Diffractor_$($currentVer.FileVersion)_x64"

function Show-Usage {
    $version = Get-CurrentVersion
    
    Write-Host ""
    Write-Host "Diffractor Build Script" -ForegroundColor Cyan
    Write-Host "========================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Current Version: $($version.FileVersion)" -ForegroundColor Green
    Write-Host ""
    Write-Host "Usage: .\dd.ps1 <command>"
    Write-Host ""
    Write-Host "Commands:"
    Write-Host "  desktop      Build desktop versions (Win32 + x64), runs tests, auto-increments build number"
    Write-Host "  store        Build Windows Store version (MSIX), runs tests, auto-increments build number"
    Write-Host "  deploy       Deploy desktop build artifacts to Google Cloud Storage"
    Write-Host "  release      Create GitHub release with tag and upload installers"
    Write-Host "  run          Run the recently built diffractor64.exe"
    Write-Host "  run32        Run the recently built diffractor32.exe (32-bit)"
    Write-Host "  cpu          Run diffractor64.exe using CPU software rendering"
    Write-Host "  build        Bump the build number and build Release x64 (diffractor64.exe)"
    Write-Host "  test         Lint, run unit tests and validate translation (.po) files"
    Write-Host "  bean         Run unit tests with the temp folder on the bean NAS (\\bean.local\home\tmp)"
    Write-Host "  code         Open VS Code with Developer Command Prompt environment"
    Write-Host "  loc          Regenerate location database files from geonames"
    Write-Host "  bump-build   Manually increment build number (e.g., $($version.Build) -> $($version.Build + 1))"
    Write-Host "  bump-ver     Increment version (e.g., $($version.VersionString) -> $($version.Major).$($version.Minor).$($version.Patch + 1))"
    Write-Host "  clear-cache  Clear Windows icon and thumbnail cache (requires restart)"
    Write-Host ""
    Write-Host "Cross-platform (tools/dd.py):"
    Write-Host "  setup        Install the build toolchain and system packages (Linux)"
    Write-Host "  configure    Configure the CMake/Ninja build"
    Write-Host "  clean        Remove the CMake build directory"
    Write-Host "  info         Report host, build directory and detected tooling"
    Write-Host ""
    Write-Host "  build / test / run use CMake and Ninja on both hosts."
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\dd.ps1 desktop      Build desktop release (auto-increments build)"
    Write-Host "  .\dd.ps1 deploy       Upload installers to GCS"
    Write-Host "  .\dd.ps1 release      Create GitHub release and tag code"
    Write-Host "  .\dd.ps1 store        Build Windows Store package (run bump-ver first)"
    Write-Host "  .\dd.ps1 bump-ver     Increment version before a major release"
    Write-Host "  .\dd.ps1 run          Run diffractor64.exe"
    Write-Host "  .\dd.ps1 run32        Run diffractor32.exe"
    Write-Host "  .\dd.ps1 cpu          Run diffractor64.exe using CPU software rendering"
    Write-Host "  .\dd.ps1 test         Lint, run unit tests and validate .po files"
    Write-Host ""
}

function Get-VisualStudioPath {
    if ($env:VSINSTALLDIR) {
        return $env:VSINSTALLDIR
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath |
            Select-Object -First 1

        if ($vsPath) {
            return $vsPath
        }
    }

    return $null
}

function Test-VisualStudioEnvironment {
    # Only the install location is needed. tools/dd.py runs vcvarsall itself for the compiler.
    $vsPath = Get-VisualStudioPath

    if (-not $vsPath) {
        Write-Host "Error: Visual Studio not found." -ForegroundColor Red
        Write-Host "Install Visual Studio with the C++ workload, or run from a Developer PowerShell." -ForegroundColor Red
        exit 1
    }

    return $vsPath
}

# The build. tools/dd.py owns CMake on both hosts, so this states what to build and nothing about
# how. See docs/linux.md#retiring-msbuild.
function Invoke-CMakeBuild {
    param(
        [ValidateSet("Debug", "Release")]
        [string]$Configuration = "Release",
        [ValidateSet("x64", "x86", "arm64")]
        [string]$Arch = "x64",
        [switch]$WinStore
    )

    Write-Host ""
    Write-Host "Building $Configuration | $Arch$(if ($WinStore) { ' | Store' })..." -ForegroundColor Yellow

    $arguments = @("--config", $Configuration, "--arch", $Arch)
    if ($WinStore) { $arguments += "--winstore" }

    Invoke-DdPython -Action "build" -Arguments $arguments
}

# Where that build puts the binary. Every consumer below names the file rather than asking the
# build, because the installer, the package and the symbol store all name it too.
function Get-DiffractorExe {
    param(
        [ValidateSet("Debug", "Release")]
        [string]$Configuration = "Release",
        [ValidateSet("x64", "x86", "arm64")]
        [string]$Arch = "x64",
        [switch]$WinStore
    )

    $stem = switch ($Arch) {
        "x64" { "diffractor64" }
        "x86" { "diffractor32" }
        default { "diffractor-arm64" }
    }

    if ($WinStore -and $Arch -ne "arm64") { $stem = "diffractor" }
    elseif ($Configuration -eq "Debug") { $stem = "$stem-d" }

    return Join-Path $SourceFilesDir "$stem.exe"
}

function Invoke-SignTool {
    param(
        [string]$Description,
        [string[]]$Files,
        [switch]$UseThumbprint
    )
    
    $signTool = Join-Path $SdkBinDir "SignTool.exe"
    if (-not (Test-Path $signTool)) {
        Write-Host "Error: SignTool not found at $signTool" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "Signing: $Description..." -ForegroundColor Yellow
    
    if ($UseThumbprint) {
        & $signTool sign /fd SHA256 /sha1 $StoreSignThumbprint /d Diffractor /tr "http://timestamp.sectigo.com?td=sha256" /td sha256 @Files
    }
    else {
        & $signTool sign /fd SHA256 /sha1 $DesktopSignThumbprint /d Diffractor /tr "http://timestamp.sectigo.com/?td=sha256" /td sha256 @Files
    }
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Signing failed" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    
    Start-Sleep -Seconds 1
}

# A file fetched at the release cmake/dd-dependencies.json pins, checked by SHA-256, into tmp/tools/.
# Kept there between runs, and fetched again only when what is there does not match its pin.
function Get-PinnedFile {
    param(
        [string]$Name,
        [string]$Destination
    )

    $pin = (Get-Content (Join-Path $ScriptDir "cmake\dd-dependencies.json") -Raw | ConvertFrom-Json).dependencies.$Name
    if (-not $pin) {
        Write-Host "Error: $Name has no pin in cmake/dd-dependencies.json" -ForegroundColor Red
        exit 1
    }

    if ((Test-Path $Destination) -and (Get-FileHash $Destination -Algorithm SHA256).Hash -eq $pin.sha256) {
        return $Destination
    }

    New-Item -ItemType Directory -Force -Path (Split-Path $Destination) | Out-Null
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest -Uri $pin.url -OutFile $Destination -UseBasicParsing
    $hash = (Get-FileHash $Destination -Algorithm SHA256).Hash

    if ($hash -ne $pin.sha256) {
        Remove-Item $Destination -Force
        Write-Host "Error: $Name from $($pin.url) has SHA-256 $hash, not the one pinned" -ForegroundColor Red
        exit 1
    }

    return $Destination
}

# 7-Zip's standalone console from the release the pins name: 7zr.exe, which reads only .7z, unpacks
# the release's extra archive, whose x86 7za.exe and its two DLLs are what tools/ once carried.
function Get-SevenZip {
    $dir = Join-Path $ScriptDir "tmp\tools\7zip"
    $extra = Get-PinnedFile -Name "7zip-extra" -Destination (Join-Path $dir "7z-extra.7z")
    $unpacked = Join-Path $dir "extra"
    $stamp = Join-Path $unpacked "pinned.sha256"
    $hash = (Get-FileHash $extra -Algorithm SHA256).Hash

    if (-not (Test-Path $stamp) -or (Get-Content $stamp -Raw).Trim() -ne $hash) {
        $sevenZr = Get-PinnedFile -Name "7zr" -Destination (Join-Path $dir "7zr.exe")
        if (Test-Path $unpacked) { Remove-Item $unpacked -Recurse -Force }
        & $sevenZr x -y "-o$unpacked" $extra | Out-Null

        if ($LASTEXITCODE -ne 0) {
            Write-Host "Error: 7zr could not unpack $extra" -ForegroundColor Red
            exit $LASTEXITCODE
        }

        Set-Content -Path $stamp -Value $hash -NoNewline
    }

    return (Join-Path $unpacked "7za.exe")
}

function Get-SymStore {
    $symstore = Join-Path (Split-Path -Parent $SdkRoot) "Debuggers\x64\symstore.exe"
    if (Test-Path -LiteralPath $symstore -PathType Leaf) { return $symstore }

    if ($env:LOCALAPPDATA) {
        $images = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsSDK\Debuggers"
        if (Test-Path -LiteralPath $images -PathType Container) {
            $versions = Get-ChildItem -LiteralPath $images -Directory |
                Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
                Sort-Object { [version]$_.Name } -Descending
            foreach ($version in $versions) {
                $tool = Join-Path $version.FullName "Windows Kits\10\Debuggers\x64\symstore.exe"
                if (Test-Path -LiteralPath $tool -PathType Leaf) { return $tool }
            }
        }
    }
    throw "SymStore not found at $symstore or in a per-user SDK image. Install the Windows SDK's Debugging Tools for Windows before packaging a release."
}

function Add-ToSymbolStore {
    param(
        [string]$File
    )
    
    $symstore = Get-SymStore
    if (-not (Test-Path -LiteralPath $File -PathType Leaf)) {
        throw "Symbol input not found: $File"
    }
    & $symstore add /r /f $File /s "c:\code\symbols" /t Diffractor /compress
    
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to add $File to symbol store (SymStore exit code $LASTEXITCODE)."
    }
}

function Clear-IncrementalLink {
    # Release builds must ship a full link, so drop the incremental LTCG state (.iobj/.ipdb)
    # and the previous binaries that let the linker patch instead of relink. /LTCG:incremental
    # writes that state beside the output, which is exe/.
    param(
        [string[]]$Targets
    )

    Write-Host ""
    Write-Host "Removing incremental link artifacts for a full link..." -ForegroundColor Yellow

    foreach ($target in $Targets) {
        # The .pdb is left alone: the compiler and linker share it, so deleting it would
        # strip debug info from objects that are not recompiled.
        foreach ($ext in ".exe", ".ilk", ".iobj", ".ipdb") {
            $path = Join-Path $SourceFilesDir "$target$ext"
            if (Test-Path $path) {
                Remove-Item $path -Force
            }
        }
    }
}

function Copy-LicenceTexts {
    # The released binary is LGPL-3.0-or-later, which must travel with every copy alongside the
    # GPL-3.0 it incorporates by reference - so the installer, the zip and the Store package each
    # carry both, renamed .txt so they open from Explorer.
    param(
        [string]$Destination
    )

    Copy-Item (Join-Path $ScriptDir "LICENSE") (Join-Path $Destination "LICENSE.txt") -Force
    Copy-Item (Join-Path $ScriptDir "COPYING.GPLv3") (Join-Path $Destination "COPYING.GPLv3.txt") -Force
}

function Build-Desktop {
    $null = Get-SymStore

    # Auto-increment build number before building
    Invoke-BumpBuild
    
    $version = Get-CurrentVersion
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Building Diffractor Desktop Release v$($version.FileVersion)" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan
    
    # Clean previous builds
    Write-Host ""
    Write-Host "Cleaning previous release artifacts..." -ForegroundColor Yellow
    $filesToClean = @(
        "diffractor-setup.exe",
        "diffractor.zip"
    )
    foreach ($file in $filesToClean) {
        $path = Join-Path $ScriptDir $file
        if (Test-Path $path) {
            Remove-Item $path -Force
        }
    }

    Clear-IncrementalLink -Targets @("diffractor32", "diffractor64")
    
    # Build Win32 and x64
    Invoke-CMakeBuild -Configuration "Release" -Arch "x86"
    Invoke-CMakeBuild -Configuration "Release" -Arch "x64"
    
    # Sign executables
    Write-Host ""
    $exe32 = Get-DiffractorExe -Configuration "Release" -Arch "x86"
    $exe64 = Get-DiffractorExe -Configuration "Release" -Arch "x64"

    # Gate on the exact binary being shipped, before it is signed and packaged
    Invoke-Tests -Exe $exe64

    Write-Host ""
    Invoke-SignTool -Description "desktop executables" -Files @($exe32, $exe64)

    # Staged into exe\ before either package is made: the installer and the zip both take them from there.
    Copy-LicenceTexts -Destination $SourceFilesDir
    
    # Build NSIS installer
    Write-Host ""
    Write-Host "Building NSIS installer..." -ForegroundColor Yellow
    $nsis = "C:\Program Files (x86)\NSIS\makensis.exe"
    $nsiScript = Join-Path $InstallerDir "diff.nsi"
    
    if (-not (Test-Path $nsis)) {
        Write-Host "Error: NSIS not found at $nsis" -ForegroundColor Red
        exit 1
    }
    
    & $nsis /INPUTCHARSET UTF8 $nsiScript
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: NSIS installer build failed" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    
    # Sign installer
    Write-Host ""
    $installer = Join-Path $ScriptDir "diffractor-setup.exe"
    Invoke-SignTool -Description "installer" -Files @($installer)
    
    # Add symbols to symbol store
    Write-Host ""
    Write-Host "Adding symbols to symbol store..." -ForegroundColor Yellow
    Add-ToSymbolStore -File (Join-Path $SourceFilesDir "diffractor32.exe")
    Add-ToSymbolStore -File (Join-Path $SourceFilesDir "diffractor32.pdb")
    Add-ToSymbolStore -File (Join-Path $SourceFilesDir "diffractor64.exe")
    Add-ToSymbolStore -File (Join-Path $SourceFilesDir "diffractor64.pdb")
    
    # Create portable ZIP
    Write-Host ""
    Write-Host "Creating portable ZIP..." -ForegroundColor Yellow
    $sevenZip = Get-SevenZip
    Push-Location $SourceFilesDir
    
    $zipFiles = @(
        "diffractor32.exe",
        "diffractor64.exe",
        "diffractor-tools.json",
        "LICENSE.txt",
        "COPYING.GPLv3.txt",
        "location-countries.txt",
        "location-places.txt",
        "location-states.txt",
        "languages\*.po",
        "dictionaries\en_US.aff",
        "dictionaries\en_US.dic"
    )
    
    & $sevenZip a -tzip -mx9 "..\diffractor.zip" @zipFiles
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: ZIP creation failed" -ForegroundColor Red
        Pop-Location
        exit $LASTEXITCODE
    }
    
    Pop-Location
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host "Desktop build completed successfully!" -ForegroundColor Green
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Version: $($version.FileVersion)"
    Write-Host ""
    Write-Host "Output files:"
    Write-Host "  diffractor-setup.exe    Desktop installer"
    Write-Host "  diffractor.zip          Portable distribution"
    Write-Host "  exe\diffractor32.exe    32-bit executable"
    Write-Host "  exe\diffractor64.exe    64-bit executable"
    Write-Host ""
}

function Get-PythonExe {
    # The repo venv carries the tooling dependencies (Pillow, polib); a bare `python` often does not.
    if ($IsLinuxHost) {
        $venvPython = Join-Path $ScriptDir ".venv/bin/python3"
        if (Test-Path $venvPython) { return $venvPython }
        return "python3"
    }

    $venvPython = Join-Path $ScriptDir ".venv\Scripts\python.exe"
    if (Test-Path $venvPython) { return $venvPython }
    return "python"
}

# The CMake/Ninja build lives in tools/dd.py so one implementation serves both hosts. This stays
# the gateway; it does not duplicate what the backend knows.
function Invoke-DdPython {
    param(
        [Parameter(Mandatory = $true)][string]$Action,
        [string[]]$Arguments = @()
    )

    $python = Get-PythonExe
    $script = Join-Path $ToolsDir "dd.py"

    # Out-Host, not the pipeline: a function returns everything it does not consume, so without
    # this the backend's output becomes part of Build-App's return value and the caller gets an
    # array of log lines with the path at the end.
    & $python $script $Action @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Assert-StoreVersionUnused {
    param(
        [hashtable]$Version
    )

    # The Store pins the package revision to 0, so Major.Minor.Patch alone identifies a
    # submission. Re-submitting the same triple is rejected after the upload.
    $packageVersion = "$($Version.Major).$($Version.Minor).$($Version.Patch).0"
    $existing = Get-ChildItem $ScriptDir -Filter "Diffractor_$($Version.Major).$($Version.Minor).$($Version.Patch).*_x64.msix" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.BaseName -ne "Diffractor_$($Version.FileVersion)_x64" }

    if ($existing) {
        Write-Host ""
        Write-Host "Error: package version $packageVersion has already been packaged." -ForegroundColor Red
        $existing | ForEach-Object { Write-Host "  $($_.Name)" -ForegroundColor Red }
        Write-Host ""
        Write-Host "Run '.\dd.ps1 bump-ver' before building a new Store submission," -ForegroundColor Yellow
        Write-Host "or delete the stale .msix if you are rebuilding this version." -ForegroundColor Yellow
        exit 1
    }
}

function Build-Store {
    param(
        [switch]$NoBump
    )

    $null = Get-SymStore

    # Auto-increment build number before building
    if ($NoBump) { Write-Host "Skipping the build-number bump (-NoBump)." -ForegroundColor Yellow }
    else { Invoke-BumpBuild }

    $version = Get-CurrentVersion
    $storePackage = "Diffractor_$($version.FileVersion)_x64"

    Assert-StoreVersionUnused -Version $version

    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Building Diffractor Windows Store Package v$($version.FileVersion)" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan
    
    # Clean previous builds
    Write-Host ""
    Write-Host "Cleaning previous release artifacts..." -ForegroundColor Yellow
    $msixPath = Join-Path $ScriptDir "$storePackage.msix"
    $symPath = Join-Path $ScriptDir "$storePackage.appxsym"
    $uploadPath = Join-Path $ScriptDir "$storePackage.msixupload"
    $uploadStaging = Join-Path $ScriptDir "dist-upload"
    foreach ($stale in $msixPath, $symPath, $uploadPath) {
        if (Test-Path $stale) {
            Remove-Item $stale -Force
        }
    }
    if (Test-Path $uploadStaging) {
        Remove-Item $uploadStaging -Recurse -Force
    }
    if (Test-Path $PackageRoot) {
        Remove-Item $PackageRoot -Recurse -Force
    }

    Clear-IncrementalLink -Targets @("diffractor")
    
    # Build the Store variant: Release plus the WINSTORE define
    Invoke-CMakeBuild -Configuration "Release" -Arch "x64" -WinStore
    
    # Gate on the exact binary being shipped, before it is signed and packaged
    $storeExe = Get-DiffractorExe -Arch "x64" -WinStore
    Invoke-Tests -Exe $storeExe

    # Sign executable
    Write-Host ""
    Invoke-SignTool -Description "store executable" -Files @($storeExe) -UseThumbprint
    
    # Prepare MSIX package directory
    Write-Host ""
    Write-Host "Preparing MSIX package..." -ForegroundColor Yellow
    
    New-Item -ItemType Directory -Path $PackageRoot -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $PackageRoot "Assets") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $PackageRoot "languages") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $PackageRoot "dictionaries") -Force | Out-Null
    
    # Generate store assets
    Write-Host ""
    Write-Host "Generating store assets..." -ForegroundColor Yellow
    $assetsDir = Join-Path $PackageRoot "Assets"
    $generateScript = Join-Path $ToolsDir "generate_store_assets.py"
    
    & (Get-PythonExe) $generateScript -o $assetsDir
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Failed to generate store assets" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    
    # MakeAppx only checks manifest-referenced images when no resources.pri is present, and
    # this pipeline always builds one, so a partial generation would slip through to the Store.
    foreach ($asset in "StoreLogo.png", "Square150x150Logo.png", "Square44x44Logo.png", "Wide310x150Logo.png", "SmallTile.png", "LargeTile.png", "SplashScreen.png", "DiffractorFile.png") {
        if (-not (Test-Path (Join-Path $assetsDir $asset))) {
            Write-Host "Error: store asset '$asset' was not generated" -ForegroundColor Red
            exit 1
        }
    }
    
    # Copy files
    Copy-Item (Join-Path $SourceFilesDir "diffractor.exe") $PackageRoot -Force
    Copy-Item (Join-Path $SourceFilesDir "location-countries.txt") $PackageRoot -Force
    Copy-Item (Join-Path $SourceFilesDir "location-places.txt") $PackageRoot -Force
    Copy-Item (Join-Path $SourceFilesDir "location-states.txt") $PackageRoot -Force
    Copy-Item (Join-Path $SourceFilesDir "diffractor-tools.json") $PackageRoot -Force
    Copy-Item (Join-Path $SourceFilesDir "languages\*.po") (Join-Path $PackageRoot "languages") -Force
    Copy-Item (Join-Path $SourceFilesDir "dictionaries\*.aff") (Join-Path $PackageRoot "dictionaries") -Force
    Copy-Item (Join-Path $SourceFilesDir "dictionaries\*.dic") (Join-Path $PackageRoot "dictionaries") -Force
    Copy-Item (Join-Path $SourceFilesDir "AppxManifest.xml") $PackageRoot -Force
    Copy-LicenceTexts -Destination $PackageRoot
    
    # Generate resources.pri using MakePri (required for asset qualifiers like _altform-unplated)
    Write-Host ""
    Write-Host "Generating resources.pri..." -ForegroundColor Yellow
    $makePri = Join-Path $SdkBinDir "MakePri.exe"
    
    if (-not (Test-Path $makePri)) {
        Write-Host "Error: MakePri not found at $makePri" -ForegroundColor Red
        exit 1
    }
    
    # Create priconfig.xml
    $priConfig = Join-Path $PackageRoot "priconfig.xml"
    Push-Location $PackageRoot
    & $makePri createconfig /cf $priConfig /dq en-US /o
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Failed to create priconfig.xml" -ForegroundColor Red
        Pop-Location
        exit $LASTEXITCODE
    }

    # The generated config auto-splits Scale/Language/DXFeatureLevel candidates into separate
    # resource PRIs, which only a bundle can load. In a single package that leaves the main index
    # with scale-100 only, so every tile, splash and logo renders upscaled on the 125%/150%/200%
    # displays most users have. Drop the packaging node so one index carries all candidates.
    [xml]$priXml = Get-Content $priConfig
    $packagingNode = $priXml.resources.SelectSingleNode('packaging')
    if ($packagingNode) { $priXml.resources.RemoveChild($packagingNode) | Out-Null }
    $priXml.Save($priConfig)
    
    # Generate resources.pri
    & $makePri new /pr $PackageRoot /cf $priConfig /o
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Failed to generate resources.pri" -ForegroundColor Red
        Pop-Location
        exit $LASTEXITCODE
    }
    
    # Remove priconfig.xml (not needed in package)
    Remove-Item $priConfig -Force -ErrorAction SilentlyContinue
    Pop-Location

    if (-not (Test-Path (Join-Path $PackageRoot "resources.pri"))) {
        Write-Host "Error: resources.pri was not generated" -ForegroundColor Red
        exit 1
    }

    # A split index means the packaging node came back and the high-DPI assets are unreachable.
    $splitPri = Get-ChildItem $PackageRoot -Filter "resources.*.pri" -File -ErrorAction SilentlyContinue
    if ($splitPri) {
        Write-Host "Error: MakePri split the resource index; high-DPI assets would not resolve." -ForegroundColor Red
        $splitPri | ForEach-Object { Write-Host "  $($_.Name)" -ForegroundColor Red }
        exit 1
    }
    
    # Build MSIX package
    Write-Host ""
    Write-Host "Building MSIX package..." -ForegroundColor Yellow
    $makeAppx = Join-Path $SdkBinDir "MakeAppx.exe"
    
    if (-not (Test-Path $makeAppx)) {
        Write-Host "Error: MakeAppx not found at $makeAppx" -ForegroundColor Red
        exit 1
    }
    
    & $makeAppx pack /d $PackageRoot /p $msixPath /o
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: MSIX package build failed" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    
    # Sign MSIX package
    Write-Host ""
    Invoke-SignTool -Description "MSIX package" -Files @($msixPath) -UseThumbprint

    # Build the Store upload file. The WinStore build compiles out the app's own crash reporting,
    # so the .appxsym symbols are the only way Store crashes reach Partner Center health reports.
    Write-Host ""
    Write-Host "Building Store upload file..." -ForegroundColor Yellow
    Compress-Archive -Path (Join-Path $SourceFilesDir "diffractor.pdb") -DestinationPath "$symPath.zip" -Force
    Move-Item "$symPath.zip" $symPath -Force

    New-Item -ItemType Directory -Path $uploadStaging -Force | Out-Null
    Copy-Item $msixPath, $symPath $uploadStaging -Force
    Compress-Archive -Path (Join-Path $uploadStaging "*") -DestinationPath "$uploadPath.zip" -Force
    Move-Item "$uploadPath.zip" $uploadPath -Force
    Remove-Item $uploadStaging -Recurse -Force

    if (-not (Test-Path $uploadPath)) {
        Write-Host "Error: failed to build $storePackage.msixupload" -ForegroundColor Red
        exit 1
    }
    
    # Add symbols to symbol store
    Write-Host ""
    Write-Host "Adding symbols to symbol store..." -ForegroundColor Yellow
    Add-ToSymbolStore -File (Join-Path $SourceFilesDir "diffractor.exe")
    Add-ToSymbolStore -File (Join-Path $SourceFilesDir "diffractor.pdb")
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host "Windows Store build completed successfully!" -ForegroundColor Green
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Version: $($version.FileVersion)"
    Write-Host ""
    Write-Host "Output files:"
    Write-Host "  $storePackage.msixupload  Upload this to Partner Center"
    Write-Host "  $storePackage.msix        Windows Store package"
    Write-Host "  $storePackage.appxsym     Public symbols for crash analytics"
    Write-Host "  exe\diffractor.exe        Store executable"
    Write-Host ""
}

function Start-Diffractor {
    param(
        [switch]$SoftwareRendering,
        [ValidateSet("x64", "Win32")]
        [string]$Platform = "x64"
    )

    # Build the executable if it is missing or out of date (no build-number bump)
    $exe = Build-App -Platform $Platform
    $exeName = Split-Path $exe -Leaf

    if ($SoftwareRendering) {
        Write-Host "Starting $exeName using CPU software rendering..." -ForegroundColor Yellow
        Start-Process $exe -ArgumentList "-no-gpu" -WorkingDirectory $SourceFilesDir
    }
    else {
        Write-Host "Starting $exeName..." -ForegroundColor Yellow
        Start-Process $exe -WorkingDirectory $SourceFilesDir
    }
}

function Build-App {
    param(
        [ValidateSet("x64", "Win32")]
        [string]$Platform = "x64"
    )

    # Build the Release executable if it is missing or out of date relative to the
    # source files. Returns the path to the executable for $Platform.
    $arch = if ($Platform -eq "Win32") { "x86" } else { "x64" }
    $exe = Get-DiffractorExe -Configuration "Release" -Arch $arch
    $exeName = Split-Path $exe -Leaf

    $needBuild = $false
    if (-not (Test-Path $exe)) {
        Write-Host "$exeName not found - building..." -ForegroundColor Yellow
        $needBuild = $true
    }
    else {
        $exeTime = (Get-Item $exe).LastWriteTimeUtc
        $srcDir = Join-Path $ScriptDir "src"
        $newestSrc = Get-ChildItem -Path $srcDir -Recurse -File -Include *.cpp, *.h, *.rc, *.manifest -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($newestSrc -and $newestSrc.LastWriteTimeUtc -gt $exeTime) {
            Write-Host "Source changes detected since last build - rebuilding..." -ForegroundColor Yellow
            $needBuild = $true
        }
    }

    if ($needBuild) {
        Invoke-CMakeBuild -Configuration "Release" -Arch $arch
    }
    else {
        Write-Host "$exeName is up to date." -ForegroundColor Green
    }

    if (-not (Test-Path $exe)) {
        Write-Host "Error: $exeName not found at $exe after build." -ForegroundColor Red
        exit 1
    }

    return $exe
}

function Invoke-Tests {
    param(
        [string]$TempPath,
        [string]$Exe
    )

    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Running Lint, Tests and Validating Translation Files" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan

    # Lint before the build: it is the cheapest gate in the repository, and it catches
    # boundary violations (platform code outside platform*, raw threads, SQLite outside its
    # owner, stale doc anchors) that a passing unit test cannot see. Running it first means a
    # boundary failure costs seconds rather than a full Release build.
    Write-Host ""
    Write-Host "Linting repository (AGENTS.md boundaries and doc integrity)..." -ForegroundColor Yellow
    Write-Host ""
    & pwsh -NoProfile -File (Join-Path $ToolsDir "lint_repo.ps1") | Out-Host
    $lintResult = $LASTEXITCODE

    & pwsh -NoProfile -File (Join-Path $ToolsDir "test_release_tools.ps1") | Out-Host
    $toolsResult = $LASTEXITCODE

    # Release builds gate on the exe they are about to ship; plain `dd test` builds one.
    $testExe = if ($Exe) { $Exe } else { Build-App }

    # Build the test arguments. When a temp path is supplied, the file-I/O tests do their
    # scratch work there (e.g. \\bean.local\home\tmp to exercise the SMB read-after-write paths).
    $testArgs = @("/test")
    if ($TempPath) {
        $testArgs = @("/test-temp:$TempPath", "/test")
        Write-Host ""
        Write-Host "Test temp folder: $TempPath" -ForegroundColor Yellow
    }

    # Run unit tests
    Write-Host ""
    Write-Host "Running unit tests ($([System.IO.Path]::GetFileName($testExe)))..." -ForegroundColor Yellow
    Write-Host ""
    & $testExe @testArgs | Out-Host
    $testResult = $LASTEXITCODE

    # Validate translation (.po) files
    Write-Host ""
    Write-Host "Validating translation (.po) files..." -ForegroundColor Yellow
    Write-Host ""
    & $testExe /validate-po | Out-Host
    $poResult = $LASTEXITCODE

    # Validate translation content (registration integrity + placeholder/quality
    # checks) via the combined Python validator.
    Write-Host ""
    Write-Host "Validating translation content (check_translations.py)..." -ForegroundColor Yellow
    Write-Host ""
    $python = Get-PythonExe
    $checkScript = Join-Path $ToolsDir "check_translations.py"
    & $python -m unittest discover -s $ToolsDir -p "test_check_translations.py" | Out-Host
    $translationChecksResult = $LASTEXITCODE
    & $python $checkScript | Out-Host
    $transResult = $LASTEXITCODE

    Write-Host ""
    if ($lintResult -eq 0 -and $toolsResult -eq 0 -and $testResult -eq 0 -and $poResult -eq 0 -and
        $translationChecksResult -eq 0 -and $transResult -eq 0) {
        Write-Host "============================================================================" -ForegroundColor Green
        Write-Host "Lint is clean, all tests passed and .po files are valid!" -ForegroundColor Green
        Write-Host "============================================================================" -ForegroundColor Green
        Write-Host ""
    }
    else {
        Write-Host "============================================================================" -ForegroundColor Red
        if ($lintResult -ne 0) {
            Write-Host "Repository lint FAILED (exit code $lintResult)." -ForegroundColor Red
        }
        if ($toolsResult -ne 0) {
            Write-Host "Release tool checks FAILED (exit code $toolsResult)." -ForegroundColor Red
        }
        if ($testResult -ne 0) {
            Write-Host "Unit tests FAILED (exit code $testResult)." -ForegroundColor Red
        }
        if ($poResult -ne 0) {
            Write-Host "Translation (.po) validation FAILED (exit code $poResult)." -ForegroundColor Red
        }
        if ($transResult -ne 0) {
            Write-Host "Translation content validation FAILED (exit code $transResult)." -ForegroundColor Red
        }
        if ($translationChecksResult -ne 0) {
            Write-Host "Translation checker tests FAILED (exit code $translationChecksResult)." -ForegroundColor Red
        }
        Write-Host "============================================================================" -ForegroundColor Red
        Write-Host ""
        exit 1
    }
}

function New-GitHubRelease {
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Creating GitHub Release" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan
    
    $version = Get-CurrentVersion
    $tagName = "v$($version.VersionString)"
    $releaseName = "Diffractor $($version.VersionString)"
    
    # Check that build artifacts exist
    $installer = Join-Path $ScriptDir "diffractor-setup.exe"
    $zipFile = Join-Path $ScriptDir "diffractor.zip"
    
    $missingFiles = @()
    if (-not (Test-Path $installer)) { $missingFiles += "diffractor-setup.exe" }
    if (-not (Test-Path $zipFile)) { $missingFiles += "diffractor.zip" }
    
    if ($missingFiles.Count -gt 0) {
        Write-Host "Error: Missing build artifacts:" -ForegroundColor Red
        foreach ($file in $missingFiles) {
            Write-Host "  - $file" -ForegroundColor Red
        }
        Write-Host ""
        Write-Host "Run '.\dd.ps1 desktop' to build first." -ForegroundColor Yellow
        exit 1
    }
    
    # Check that gh CLI is available
    $gh = Get-Command "gh" -ErrorAction SilentlyContinue
    if (-not $gh) {
        Write-Host "Error: GitHub CLI (gh) not found." -ForegroundColor Red
        Write-Host "Install from: https://cli.github.com/" -ForegroundColor Yellow
        Write-Host "Then run: gh auth login" -ForegroundColor Yellow
        exit 1
    }
    
    # Check if authenticated
    $authStatus = & gh auth status 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Not authenticated with GitHub CLI." -ForegroundColor Red
        Write-Host "Run: gh auth login" -ForegroundColor Yellow
        exit 1
    }
    
    # Check if tag already exists
    $existingTag = & git tag -l $tagName 2>&1
    if ($existingTag -eq $tagName) {
        Write-Host "Warning: Tag $tagName already exists locally." -ForegroundColor Yellow
        $response = Read-Host "Do you want to delete the existing tag and create a new release? (y/N)"
        if ($response -ne "y" -and $response -ne "Y") {
            Write-Host "Aborted." -ForegroundColor Yellow
            exit 0
        }
        # Delete local and remote tag
        Write-Host "Deleting existing tag..." -ForegroundColor Yellow
        & git tag -d $tagName 2>&1 | Out-Null
        & git push origin --delete $tagName 2>&1 | Out-Null
        # Also delete the release if it exists
        & gh release delete $tagName --yes 2>&1 | Out-Null
    }
    
    Write-Host ""
    Write-Host "Creating release: $releaseName" -ForegroundColor Yellow
    Write-Host "Tag: $tagName" -ForegroundColor Yellow
    Write-Host ""
    
    # Create release with assets, generate notes automatically
    # Note: gh release create will create the tag automatically
    # --target pins the tag to the commit that produced these artifacts. Without it gh tags the
    # repository default branch, so releasing from a branch not yet merged to master would publish
    # a tag and source archives for a tree the uploaded installer was never built from.
    $releaseTarget = & git rev-parse HEAD
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Could not resolve HEAD for the release tag" -ForegroundColor Red
        exit 1
    }

    & gh release create $tagName `
        --title $releaseName `
        --target $releaseTarget `
        --generate-notes `
        $installer `
        $zipFile
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Failed to create GitHub release" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host "GitHub Release created successfully!" -ForegroundColor Green
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Release: $releaseName"
    Write-Host "Tag: $tagName"
    Write-Host "URL: https://github.com/ZacWalk/diffractor/releases/tag/$tagName"
    Write-Host ""
    Write-Host "Uploaded assets:"
    Write-Host "  - diffractor-setup.exe"
    Write-Host "  - diffractor.zip"
    Write-Host ""
}

function Update-Locations {
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Regenerating Location Database Files" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host ""
    
    $pythonScript = Join-Path $ToolsDir "generate_locations.py"
    
    if (-not (Test-Path $pythonScript)) {
        Write-Host "Error: Python script not found at $pythonScript" -ForegroundColor Red
        exit 1
    }
    
    # Check for Python
    $python = Get-Command "python" -ErrorAction SilentlyContinue
    if (-not $python) {
        Write-Host "Error: Python not found. Please install Python 3." -ForegroundColor Red
        exit 1
    }
    
    Write-Host "Running generate_locations.py..." -ForegroundColor Yellow
    Write-Host ""
    
    Push-Location $ScriptDir
    try {
        & python $pythonScript
        
        if ($LASTEXITCODE -ne 0) {
            Write-Host ""
            Write-Host "Error: Location generation failed" -ForegroundColor Red
            exit $LASTEXITCODE
        }
    }
    finally {
        Pop-Location
    }
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host "Location files updated successfully!" -ForegroundColor Green
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host ""
}

function Open-VSCode {
    Write-Host ""
    Write-Host "Opening VS Code with Developer environment..." -ForegroundColor Cyan
    Write-Host ""
    
    # Find VS Code
    $codePath = Get-Command "code" -ErrorAction SilentlyContinue
    if (-not $codePath) {
        Write-Host "Error: VS Code 'code' command not found in PATH." -ForegroundColor Red
        Write-Host "Make sure VS Code is installed and added to PATH." -ForegroundColor Yellow
        exit 1
    }
    
    # Check if already in a dev environment
    if ($env:VSINSTALLDIR) {
        Write-Host "Developer environment already active." -ForegroundColor Gray
    }
    else {
        # Find Visual Studio installation using vswhere
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (-not (Test-Path $vswhere)) {
            Write-Host "Error: vswhere.exe not found. Is Visual Studio installed?" -ForegroundColor Red
            exit 1
        }
        
        $vsPath = & $vswhere -latest -property installationPath
        if (-not $vsPath) {
            Write-Host "Error: Could not find Visual Studio installation." -ForegroundColor Red
            exit 1
        }
        
        Write-Host "Using Visual Studio at: $vsPath" -ForegroundColor Gray
        
        # Use the DevShell module to set up environment in current session
        $devShellModule = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
        if (Test-Path $devShellModule) {
            Import-Module $devShellModule
            Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -no_logo"
        }
        else {
            Write-Host "Error: DevShell module not found at $devShellModule" -ForegroundColor Red
            exit 1
        }
    }
    
    Write-Host ""
    
    # Launch VS Code from current environment
    & code $ScriptDir
    
    Write-Host "VS Code launched with Developer environment." -ForegroundColor Green
    Write-Host ""
}

function Clear-IconCache {
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Clearing Windows Icon and Thumbnail Cache" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host ""
    
    # Stop Explorer to release file locks on cache files
    Write-Host "Stopping Explorer..." -ForegroundColor Yellow
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    
    $localAppData = $env:LOCALAPPDATA
    $deletedCount = 0
    
    # Delete icon cache files
    Write-Host "Deleting icon cache files..." -ForegroundColor Yellow
    $iconCacheFiles = Get-ChildItem -Path $localAppData -Filter "iconcache*.db" -ErrorAction SilentlyContinue
    foreach ($file in $iconCacheFiles) {
        try {
            Remove-Item $file.FullName -Force
            Write-Host "  Deleted: $($file.Name)" -ForegroundColor Gray
            $deletedCount++
        }
        catch {
            Write-Host "  Failed to delete: $($file.Name)" -ForegroundColor Red
        }
    }
    
    # Delete thumbnail cache files
    Write-Host "Deleting thumbnail cache files..." -ForegroundColor Yellow
    $thumbCacheDir = Join-Path $localAppData "Microsoft\Windows\Explorer"
    if (Test-Path $thumbCacheDir) {
        $thumbCacheFiles = Get-ChildItem -Path $thumbCacheDir -Filter "thumbcache*.db" -ErrorAction SilentlyContinue
        foreach ($file in $thumbCacheFiles) {
            try {
                Remove-Item $file.FullName -Force
                Write-Host "  Deleted: $($file.Name)" -ForegroundColor Gray
                $deletedCount++
            }
            catch {
                Write-Host "  Failed to delete: $($file.Name)" -ForegroundColor Red
            }
        }
        
        # Delete IconCache.db
        $iconCacheDb = Join-Path $thumbCacheDir "IconCache.db"
        if (Test-Path $iconCacheDb) {
            try {
                Remove-Item $iconCacheDb -Force
                Write-Host "  Deleted: IconCache.db" -ForegroundColor Gray
                $deletedCount++
            }
            catch {
                Write-Host "  Failed to delete: IconCache.db" -ForegroundColor Red
            }
        }
        
        # Delete iconcache_*.db files in Explorer folder
        $iconCacheFiles2 = Get-ChildItem -Path $thumbCacheDir -Filter "iconcache_*.db" -ErrorAction SilentlyContinue
        foreach ($file in $iconCacheFiles2) {
            try {
                Remove-Item $file.FullName -Force
                Write-Host "  Deleted: $($file.Name)" -ForegroundColor Gray
                $deletedCount++
            }
            catch {
                Write-Host "  Failed to delete: $($file.Name)" -ForegroundColor Red
            }
        }
    }
    
    # Restart Explorer
    Write-Host ""
    Write-Host "Restarting Explorer..." -ForegroundColor Yellow
    Start-Process explorer
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host "Cache cleared! Deleted $deletedCount file(s)." -ForegroundColor Green
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Note: You may need to log out and back in for all changes to take effect." -ForegroundColor Yellow
    Write-Host ""
}

function Deploy-Desktop {
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Cyan
    Write-Host "Deploying Desktop Build to Google Cloud Storage" -ForegroundColor Cyan
    Write-Host "============================================================================" -ForegroundColor Cyan
    
    # Check that build artifacts exist
    $installer = Join-Path $ScriptDir "diffractor-setup.exe"
    $zipFile = Join-Path $ScriptDir "diffractor.zip"
    
    $missingFiles = @()
    if (-not (Test-Path $installer)) { $missingFiles += "diffractor-setup.exe" }
    if (-not (Test-Path $zipFile)) { $missingFiles += "diffractor.zip" }
    
    if ($missingFiles.Count -gt 0) {
        Write-Host "Error: Missing build artifacts:" -ForegroundColor Red
        foreach ($file in $missingFiles) {
            Write-Host "  - $file" -ForegroundColor Red
        }
        Write-Host ""
        Write-Host "Run '.\dd.ps1 desktop' to build first." -ForegroundColor Yellow
        exit 1
    }
    
    # Check that gsutil is available
    $gsutil = Get-Command "gsutil" -ErrorAction SilentlyContinue
    if (-not $gsutil) {
        Write-Host "Error: gsutil not found. Please install Google Cloud SDK." -ForegroundColor Red
        Write-Host "https://cloud.google.com/sdk/docs/install" -ForegroundColor Yellow
        exit 1
    }
    
    Write-Host ""
    Write-Host "Uploading to gs://diffractor/..." -ForegroundColor Yellow
    
    & gsutil -m cp $installer $zipFile gs://diffractor/
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Upload failed" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    
    Write-Host ""
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host "Deploy completed successfully!" -ForegroundColor Green
    Write-Host "============================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Uploaded files:"
    Write-Host "  https://storage.googleapis.com/diffractor/diffractor-setup.exe"
    Write-Host "  https://storage.googleapis.com/diffractor/diffractor.zip"
    Write-Host ""
}

# Main entry point
switch ($Command) {
    "desktop" { Build-Desktop }
    "store" { Build-Store -NoBump:$NoBump }
    "deploy" { Deploy-Desktop }
    "release" { New-GitHubRelease }
    "run32" { Start-Diffractor -Platform "Win32" }
    "cpu" { Start-Diffractor -SoftwareRendering }
    "bean" { Invoke-Tests -TempPath "\\bean.local\home\tmp" }
    "code" { Open-VSCode }
    "loc" { Update-Locations }
    "bump-build" { Invoke-BumpBuild }
    "bump-ver" { Invoke-BumpVersion }
    "clear-cache" { Clear-IconCache }

    # Cross-platform, delegated to tools/dd.py.
    "setup" { Invoke-DdPython -Action "setup" -Arguments $Rest }
    "configure" { Invoke-DdPython -Action "configure" -Arguments $Rest }
    "clean" { Invoke-DdPython -Action "clean" -Arguments $Rest }
    "info" { Invoke-DdPython -Action "info" -Arguments $Rest }

    "build" {
        if ($IsLinuxHost) { Invoke-DdPython -Action "build" -Arguments $Rest }
        else { Invoke-BumpBuild; Build-App | Out-Null }
    }
    "test" {
        if ($IsLinuxHost) { Invoke-DdPython -Action "test" -Arguments $Rest }
        else { Invoke-Tests }
    }
    "run" {
        if ($IsLinuxHost) { Invoke-DdPython -Action "run" -Arguments $Rest }
        else { Start-Diffractor }
    }

    "help" { Show-Usage }
    "" { Show-Usage }
    default { Show-Usage }
}
