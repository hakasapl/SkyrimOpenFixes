#Requires -Version 5.1
<#
.SYNOPSIS
    Configures and builds the plugin in RelWithDebInfo, then stages the artifacts into dist/.

.DESCRIPTION
    CommonLibSSE-NG resolves addresses and struct layouts at runtime, so a single
    OpenFixes.dll serves every supported Skyrim runtime - there are no per-flavor
    builds. The built DLL and PDB, the generated OpenFixes.ini and everything else in the
    package/ folder are copied into dist/SKSE/Plugins, and the license files into
    dist/SKSE/Plugins/OpenFixes.

.PARAMETER Config
    CMake build configuration. Defaults to RelWithDebInfo.

.PARAMETER VcpkgRoot
    Path to the vcpkg root (containing scripts/buildsystems/vcpkg.cmake).
    Defaults to $env:VCPKG_ROOT, then the Visual Studio bundled vcpkg.
#>
[CmdletBinding()]
param(
    [string]$Config = "RelWithDebInfo",
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "buildRel"
$DistRoot = Join-Path $RepoRoot "dist"
$PackageDir = Join-Path $RepoRoot "package"
$PluginName = "OpenFixes"

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# --- Set up the MSVC / Ninja developer environment ---------------------------
function Enter-DevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return # already in a developer shell
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Run this script from a Developer PowerShell for VS, or install Visual Studio."
    }

    $vsPath = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsPath) {
        throw "No Visual Studio installation with the C++ toolset was found."
    }

    $devShell = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    if (-not (Test-Path $devShell)) {
        throw "DevShell module not found at $devShell."
    }

    Write-Step "Entering Visual Studio developer environment ($vsPath)"
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
        -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

# --- Resolve the vcpkg toolchain file ----------------------------------------
function Resolve-VcpkgToolchain {
    $candidates = @()
    if ($VcpkgRoot) { $candidates += $VcpkgRoot }
    if ($env:VCPKG_INSTALLATION_ROOT) { $candidates += $env:VCPKG_INSTALLATION_ROOT }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -prerelease -products * -property installationPath
        if ($vsPath) { $candidates += (Join-Path $vsPath "VC\vcpkg") }
    }

    foreach ($root in $candidates) {
        $toolchain = Join-Path $root "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path $toolchain) { return $toolchain }
    }

    throw "Could not locate vcpkg.cmake. Set VCPKG_ROOT or pass -VcpkgRoot."
}

# --- Clean and recreate output folders ---------------------------------------
Write-Step "Cleaning output folders"
foreach ($dir in @($BuildDir, $DistRoot)) {
    if (Test-Path $dir) { Remove-Item -Path $dir -Recurse -Force }
}
New-Item -ItemType Directory -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Path $DistRoot | Out-Null

Enter-DevEnvironment
$Toolchain = Resolve-VcpkgToolchain
Write-Step "Using vcpkg toolchain: $Toolchain"

# --- Configure, build and stage ----------------------------------------------
$distDir = Join-Path $DistRoot "SKSE\plugins"
New-Item -ItemType Directory -Path $distDir -Force | Out-Null

Write-Step "Configuring"
& cmake -S $RepoRoot -B $BuildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Step "Building"
& cmake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Step "Staging artifacts into $distDir"
$dll = Get-ChildItem -Path $BuildDir -Recurse -Filter "$PluginName.dll" | Select-Object -First 1
if (-not $dll) { throw "Built DLL '$PluginName.dll' not found under $BuildDir." }
Copy-Item -Path $dll.FullName -Destination $distDir -Force

$pdb = Get-ChildItem -Path $BuildDir -Recurse -Filter "$PluginName.pdb" | Select-Object -First 1
if ($pdb) {
    Copy-Item -Path $pdb.FullName -Destination $distDir -Force
}
else {
    Write-Warning "PDB '$PluginName.pdb' not found under $BuildDir."
}

Write-Step "Generating $PluginName.ini and the README fix lists from the fixes' metadata"
& python (Join-Path $RepoRoot "tools\gen_ini.py")
if ($LASTEXITCODE -ne 0) { throw "INI generation failed." }
& python (Join-Path $RepoRoot "tools\gen_readme.py")
if ($LASTEXITCODE -ne 0) { throw "README generation failed." }

if (Test-Path $PackageDir) {
    Copy-Item -Path (Join-Path $PackageDir "*") -Destination $distDir -Recurse -Force
}

# GPL-3.0 and the original mods' licenses travel with the DLL
$licenseDir = Join-Path $distDir "$PluginName"
New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
foreach ($file in @("LICENSE", "EXCEPTIONS.md", "README.md")) {
    Copy-Item -Path (Join-Path $RepoRoot $file) -Destination $licenseDir -Force
}
Copy-Item -Path (Join-Path $RepoRoot "licenses") -Destination $licenseDir -Recurse -Force
# Each fix keeps its original mod's license files in its folder under src/Fixes; ship them as licenses/<FixId>/
Get-ChildItem -Path (Join-Path $RepoRoot "src\Fixes") -Recurse -File |
    Where-Object { $_.Extension -notin @(".cpp", ".h") } |
    ForEach-Object {
        $target = Join-Path $licenseDir "licenses\$($_.Directory.Name)"
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Copy-Item -Path $_.FullName -Destination $target -Force
    }

Write-Step "Done. Artifacts staged in $DistRoot"
Get-ChildItem -Path $DistRoot -Recurse -File | ForEach-Object {
    Write-Host "  $($_.FullName.Substring($DistRoot.Length + 1))"
}
