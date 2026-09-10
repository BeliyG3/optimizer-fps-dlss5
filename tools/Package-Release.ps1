#Requires -Version 5.1
<#
.SYNOPSIS
    Assembles the shippable "Optimizer FPS for DLSS5" release folder and zips it.

.DESCRIPTION
    Copyright (c) 2026 Yuri Grib (BeliyG3). MIT licence (see LICENSE).

    Takes the two build trees (x64 add-on + forwarder + compiled shaders, x86 remote tab),
    lays them out the way Install-OptimizerFPS.ps1 expects, writes payload\VERSION.txt and
    payload\files.sha256 (sha256, two spaces, forward-slash relative path, LF, sorted), then
    zips the whole folder (forward-slash entry names) and prints the zip's SHA-256.

    The shader list is never hard-coded: every *.dxbc the build produced goes into
    payload\x64\optimizer-fps-dlss5\, and the installer reads the list back out of files.sha256.

.EXAMPLE
    powershell.exe -NoProfile -File tools\Package-Release.ps1
#>
[CmdletBinding()]
param(
    [string] $BuildDirX64,
    [string] $BuildDirX86,
    [string] $Version,
    [string] $Out,
    [switch] $Force
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $BuildDirX64) { $BuildDirX64 = Join-Path $repoRoot 'out\build\x64-mt' }
if (-not $BuildDirX86) { $BuildDirX86 = Join-Path $repoRoot 'out\build\x86-remote' }
if (-not $Out)         { $Out         = Join-Path $repoRoot 'dist-release' }

function Fail { param([string] $Text) Write-Host ('  [FAIL] ' + $Text) -ForegroundColor Red; exit 1 }
function Note { param([string] $Text) Write-Host ('  [ .. ] ' + $Text) -ForegroundColor DarkGray }
function Good { param([string] $Text) Write-Host ('  [ OK ] ' + $Text) -ForegroundColor Green }

function Get-Sha256Lower
{
    param([string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Copy-Into
{
    param([string] $Source, [string] $Destination)
    $parent = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) { $null = New-Item -ItemType Directory -Path $parent -Force }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

# --- version ---------------------------------------------------------------------------

if (-not $Version) {
    $versionCmake = Join-Path $repoRoot 'cmake\Version.cmake'
    if (-not (Test-Path -LiteralPath $versionCmake -PathType Leaf)) { Fail ('cmake\Version.cmake not found: ' + $versionCmake) }
    $m = [regex]::Match([IO.File]::ReadAllText($versionCmake), '(?im)^\s*set\s*\(\s*PW_RELEASE_VERSION\s+"([^"]+)"')
    if (-not $m.Success) { Fail 'PW_RELEASE_VERSION not found in cmake\Version.cmake.' }
    $Version = $m.Groups[1].Value
}
Note ('Version: ' + $Version)

# --- the inputs ------------------------------------------------------------------------

$addon64   = Join-Path $BuildDirX64 'adapters\reshade\Release\optimizer-fps-dlss5.addon64'
$forwarder = Join-Path $BuildDirX64 'adapters\reshade\ngx_forwarder\Release\nvngx.dll_optimizerfps.dll'
$shaderSrc = Join-Path $BuildDirX64 'shaders'
$remote32  = Join-Path $BuildDirX86 'adapters\reshade\Release\optimizer-fps-dlss5-remote.addon32'

foreach ($p in @($addon64, $forwarder, $remote32)) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { Fail ('Build output missing: ' + $p) }
}
if (-not (Test-Path -LiteralPath $shaderSrc -PathType Container)) { Fail ('Compiled shaders missing: ' + $shaderSrc) }
$shaders = @(Get-ChildItem -LiteralPath $shaderSrc -File -Filter '*.dxbc' | Sort-Object Name)
if ($shaders.Count -eq 0) { Fail ('No .dxbc in ' + $shaderSrc) }
Note ('' + $shaders.Count + ' compiled shader(s).')

# --- the output folder -----------------------------------------------------------------

$name    = 'Optimizer-FPS-for-DLSS5-' + $Version
$stage   = Join-Path $Out $name
$zipPath = Join-Path $Out ($name + '.zip')

if (Test-Path -LiteralPath $stage) {
    if (-not $Force -and -not (Test-Path -LiteralPath (Join-Path $stage 'payload'))) {
        Fail ('The output folder exists and does not look like a previous release: ' + $stage)
    }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
$null = New-Item -ItemType Directory -Path $stage -Force

# scripts + legal
foreach ($f in @('Install-OptimizerFPS.cmd', 'Install-OptimizerFPS.ps1', 'Verify-OptimizerFPS.ps1')) {
    $src = Join-Path $PSScriptRoot $f
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { Fail ('Missing: ' + $src) }
    Copy-Into -Source $src -Destination (Join-Path $stage $f)
}
foreach ($f in @('LICENSE', 'NOTICE')) {
    $src = Join-Path $repoRoot $f
    if (Test-Path -LiteralPath $src -PathType Leaf) { Copy-Into -Source $src -Destination (Join-Path $stage $f) }
    else { Write-Host ('  [WARN] ' + $f + ' not found in the repo root.') -ForegroundColor Yellow }
}

# THIRD-PARTY-LICENSES.txt = every file in third_party\licenses, concatenated.
$licDir = Join-Path $repoRoot 'third_party\licenses'
if (Test-Path -LiteralPath $licDir -PathType Container) {
    $sb = New-Object Text.StringBuilder
    $null = $sb.AppendLine('Third-party licences bundled with Optimizer FPS for DLSS5 ' + $Version)
    $null = $sb.AppendLine()
    foreach ($lf in @(Get-ChildItem -LiteralPath $licDir -File | Sort-Object Name)) {
        $null = $sb.AppendLine('=' * 78)
        $null = $sb.AppendLine($lf.Name)
        $null = $sb.AppendLine('=' * 78)
        $null = $sb.AppendLine()
        $null = $sb.AppendLine([IO.File]::ReadAllText($lf.FullName))
        $null = $sb.AppendLine()
    }
    [IO.File]::WriteAllText((Join-Path $stage 'THIRD-PARTY-LICENSES.txt'), $sb.ToString(), (New-Object Text.UTF8Encoding($false)))
    Good 'THIRD-PARTY-LICENSES.txt written.'
}
else { Write-Host '  [WARN] third_party\licenses not found.' -ForegroundColor Yellow }

# README.txt: the short half of docs\INSTALL.md, with the markdown taken back out.
$installMd = Join-Path $repoRoot 'docs\INSTALL.md'
if (Test-Path -LiteralPath $installMd -PathType Leaf) {
    $md = [IO.File]::ReadAllText($installMd)
    $cut = $md.IndexOf('<!-- README-END -->')
    if ($cut -gt 0) { $md = $md.Substring(0, $cut) }
    $lines = New-Object System.Collections.ArrayList
    $null = $lines.Add('Optimizer FPS for DLSS5 ' + $Version)
    $null = $lines.Add('')
    foreach ($l in ($md -split "`r?`n")) {
        if ($l -match '^\s*<!--') { continue }
        $t = $l
        $t = $t -replace '^#{1,6}\s*', ''
        $t = $t -replace '\*\*([^*]+)\*\*', '$1'
        $t = $t -replace '`([^`]+)`', '$1'
        $t = $t -replace '^\s*```.*$', ''
        $null = $lines.Add($t)
    }
    $null = $lines.Add('')
    $null = $lines.Add('Full guide: docs/INSTALL.md in the repository.')
    [IO.File]::WriteAllText((Join-Path $stage 'README.txt'), (($lines.ToArray()) -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    Good 'README.txt written.'
}
else { Write-Host '  [WARN] docs\INSTALL.md not found; no README.txt.' -ForegroundColor Yellow }

# --- payload ---------------------------------------------------------------------------

$payload = Join-Path $stage 'payload'
$null = New-Item -ItemType Directory -Path $payload -Force

Copy-Into -Source $addon64   -Destination (Join-Path $payload 'x64\optimizer-fps-dlss5.addon64')
Copy-Into -Source $forwarder -Destination (Join-Path $payload 'x64\nvngx.dll_optimizerfps.dll')
foreach ($s in $shaders) {
    Copy-Into -Source $s.FullName -Destination (Join-Path $payload ('x64\optimizer-fps-dlss5\' + $s.Name))
}
Copy-Into -Source $remote32 -Destination (Join-Path $payload 'x86\optimizer-fps-dlss5-remote.addon32')

[IO.File]::WriteAllText((Join-Path $payload 'VERSION.txt'), ($Version + "`n"), (New-Object Text.UTF8Encoding($false)))

# files.sha256: "<sha256>  <forward/slash/relative/path>", LF, sorted, no BOM.
$manifestLines = New-Object System.Collections.ArrayList
$payloadFull = (Resolve-Path -LiteralPath $payload).ProviderPath
$prefix = $payloadFull
if (-not $prefix.EndsWith('\')) { $prefix += '\' }
foreach ($f in @(Get-ChildItem -LiteralPath $payload -File -Recurse)) {
    if ($f.Name -ieq 'files.sha256') { continue }
    $rel = $f.FullName.Substring($prefix.Length).Replace('\', '/')
    $null = $manifestLines.Add((Get-Sha256Lower $f.FullName) + '  ' + $rel)
}
$sorted = @($manifestLines.ToArray() | Sort-Object { ($_ -split '  ', 2)[1] })
[IO.File]::WriteAllText((Join-Path $payload 'files.sha256'), (($sorted -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
Good ('payload\files.sha256: ' + $sorted.Count + ' entries.')

# --- zip -------------------------------------------------------------------------------

if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
# Compress-Archive (PowerShell 5.1) writes entry names with backslashes; Explorer copes, 7-Zip and unzip
# complain. Write the entries ourselves with forward slashes, as the zip format requires.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$stageParent = Split-Path -Parent ((Get-Item -LiteralPath $stage).FullName)
$zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entryName = $_.FullName.Substring($stageParent.Length).TrimStart([char]92, [char]47).Replace([string][char]92, '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $zip.Dispose() }
$zipHash = Get-Sha256Lower $zipPath

Write-Host ''
Good ('Release: ' + $zipPath)
Write-Host ('         SHA-256: ' + $zipHash)
Write-Host ('         Folder:  ' + $stage)
exit 0
