<#
.SYNOPSIS
    Verifies the local ReShade / Dear ImGui / Microsoft Detours trees against DEPENDENCIES.lock.json.

.DESCRIPTION
    With no parameters the script checks the trees that cmake/Dependencies.cmake fetches into
    external\ (external\reshade-src, external\imgui-src, external\detours-src). Pass the -*Root
    parameters to check a hand-provided checkout instead (the same paths you would give to
    PW_RESHADE_SDK_ROOT / PW_IMGUI_ROOT / PW_DETOURS_ROOT... note that PW_DETOURS_ROOT points at a
    *prebuilt* tree, whose layout differs from the source tree checked here).

    The dependency source files are text. Their hashes are recorded over the content normalized to
    CRLF line endings, so a git checkout (core.autocrlf) and a GitHub source tarball verify alike.
    The license files shipped in third_party\licenses are hashed byte-exactly.

.EXAMPLE
    .\tools\Verify-Dependencies.ps1

.EXAMPLE
    .\tools\Verify-Dependencies.ps1 -ReShadeRoot C:\src\reshade -ImGuiRoot C:\src\imgui
#>
[CmdletBinding()]
param(
    [string]$ReShadeRoot,

    [string]$ImGuiRoot,

    [string]$DetoursRoot,

    [string]$LockFile = (Join-Path $PSScriptRoot '..\DEPENDENCIES.lock.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-UpperSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Get-NormalizedSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    # Hash the CRLF-normalized bytes: LF-only tarballs and CRLF checkouts must agree.
    # The normalization is done on raw bytes so that any non-UTF-8 byte survives untouched.
    $bytes = [IO.File]::ReadAllBytes($Path)
    $stream = [IO.MemoryStream]::new($bytes.Length + 64)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $byte = $bytes[$i]
        if ($byte -eq 13 -and ($i + 1) -lt $bytes.Length -and $bytes[$i + 1] -eq 10) {
            continue  # drop the CR of an existing CRLF; the LF below re-adds it
        }
        if ($byte -eq 10) {
            $stream.WriteByte(13)
        }
        $stream.WriteByte($byte)
    }
    $normalized = $stream.ToArray()
    $stream.Dispose()
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($normalized))).Replace('-', '')
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-TreeSha256 {
    param([Parameter(Mandatory = $true)][string[]]$Lines)

    $payload = ($Lines -join "`n") + "`n"
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($payload)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '')
    }
    finally {
        $algorithm.Dispose()
    }
}

$resolvedLock = (Resolve-Path -LiteralPath $LockFile).Path
$sourceRoot = Split-Path -Parent $resolvedLock

# Defaults: whatever cmake/Dependencies.cmake fetched into external\.
$externalRoot = Join-Path $sourceRoot 'external'
if (-not $ReShadeRoot) { $ReShadeRoot = Join-Path $externalRoot 'reshade-src' }
if (-not $ImGuiRoot)   { $ImGuiRoot   = Join-Path $externalRoot 'imgui-src' }
if (-not $DetoursRoot) { $DetoursRoot = Join-Path $externalRoot 'detours-src' }

$rootArguments = @{
    'ReShade Add-on SDK' = $ReShadeRoot
    'Dear ImGui' = $ImGuiRoot
    'Microsoft Detours' = $DetoursRoot
}
$roots = @{}
foreach ($entry in $rootArguments.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Container)) {
        throw ("Dependency root for '{0}' does not exist: {1}. Configure a preset once so " +
               "FetchContent populates external\, or pass the matching -*Root parameter." -f $entry.Key, $entry.Value)
    }
    $roots[$entry.Key] = (Resolve-Path -LiteralPath $entry.Value).Path
}

$lock = Get-Content -LiteralPath $resolvedLock -Raw | ConvertFrom-Json

foreach ($dependency in $lock.dependencies) {
    if (-not $roots.ContainsKey([string]$dependency.name)) {
        throw "No local root mapping exists for dependency '$($dependency.name)'."
    }

    $root = $roots[[string]$dependency.name]

    $licensePath = Join-Path $sourceRoot ([string]$dependency.licenseFile).Replace('/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) {
        throw "Locked license file is missing: $licensePath"
    }
    $actualLicenseHash = Get-UpperSha256 -Path $licensePath
    $expectedLicenseHash = ([string]$dependency.licenseSha256).ToUpperInvariant()
    if ($actualLicenseHash -ne $expectedLicenseHash) {
        throw "License SHA-256 mismatch for '$($dependency.name)': expected $expectedLicenseHash, got $actualLicenseHash."
    }

    $treeLines = [Collections.Generic.List[string]]::new()
    $properties = @($dependency.files.PSObject.Properties)

    if ($properties.Count -ne [int]$dependency.fileCount) {
        throw "File count mismatch in lock data for '$($dependency.name)'."
    }

    [string[]]$relativePaths = @($properties | ForEach-Object { [string]$_.Name })
    [Array]::Sort($relativePaths, [StringComparer]::Ordinal)

    foreach ($relativePath in $relativePaths) {
        $entry = $dependency.files.PSObject.Properties[$relativePath]
        $nativeRelativePath = $relativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)
        $path = Join-Path $root $nativeRelativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Locked dependency file is missing: $path"
        }

        $actualHash = Get-NormalizedSha256 -Path $path
        $expectedHash = ([string]$entry.Value).ToUpperInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "SHA-256 mismatch for '$relativePath' in '$($dependency.name)': expected $expectedHash, got $actualHash."
        }

        $treeLines.Add("$actualHash  $($relativePath.Replace('\', '/'))")
    }

    $actualTreeHash = Get-TreeSha256 -Lines $treeLines.ToArray()
    $expectedTreeHash = ([string]$dependency.treeSha256).ToUpperInvariant()
    if ($actualTreeHash -ne $expectedTreeHash) {
        throw "Tree SHA-256 mismatch for '$($dependency.name)': expected $expectedTreeHash, got $actualTreeHash."
    }

    Write-Host "Verified $($dependency.name) $($dependency.version) in ${root}: $actualTreeHash"
}

Write-Host 'All locked build dependencies match.'
