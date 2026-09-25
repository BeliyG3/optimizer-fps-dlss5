#Requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $Zip)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
Import-Module (Join-Path $PSScriptRoot 'installer\Common.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'installer\Pe.psm1') -Force

function Read-ZipText($Entry) {
    $reader = New-Object IO.StreamReader($Entry.Open(), (New-Object Text.UTF8Encoding($false)))
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}

function Get-ZipHash($Entry) {
    $stream = $Entry.Open()
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}

$archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path)
$tempRoot = $null
try {
    $all = @{}
    foreach ($entry in $archive.Entries) {
        $path = $entry.FullName
        if (-not $entry.Name -or $path -match '\\|(^/|^[A-Za-z]:|(^|/)\.\.(/|$))') {
            throw "Invalid ZIP entry: $path"
        }
        if ($all.ContainsKey($path)) { throw "Duplicate ZIP entry: $path" }
        $all[$path] = $entry
    }
    $manifests = @($all.Keys | Where-Object { $_ -match '^[^/]+/payload/files\.sha256$' })
    if ($manifests.Count -ne 1) { throw 'Expected one payload manifest' }
    $prefix = $manifests[0].Substring(0, $manifests[0].Length - 'files.sha256'.Length)
    $releasePrefix = $prefix.Substring(0, $prefix.Length - 'payload/'.Length)
    $entries = @{}
    foreach ($path in $all.Keys) {
        if ($path.StartsWith($prefix, [StringComparison]::Ordinal)) {
            $entries[$path.Substring($prefix.Length)] = $all[$path]
        }
    }

    # The zip must carry exactly the installer modules of this tree (Package-Release ships them all).
    $requiredModules = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'installer') -File -Filter '*.psm1' |
        Sort-Object Name | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.Name) })
    $modulePaths = @($all.Keys | Where-Object { $_.StartsWith($releasePrefix + 'installer/',
        [StringComparison]::Ordinal) })
    $expectedModules = @($requiredModules | ForEach-Object { $releasePrefix + 'installer/' + $_ + '.psm1' })
    if ($modulePaths.Count -ne $expectedModules.Count -or
        @(Compare-Object $modulePaths $expectedModules).Count -ne 0) {
        throw 'Installer modules in ZIP do not match the required module list'
    }
    foreach ($entryName in @('Install-OptimizerFPS.cmd', 'Install-OptimizerFPS.ps1',
                             'Verify-OptimizerFPS.ps1')) {
        if (-not $all.ContainsKey($releasePrefix + $entryName)) {
            throw "Missing ZIP script: $entryName"
        }
    }

    $manifestText = Read-ZipText $entries['files.sha256']
    if ($manifestText.Contains("`r") -or -not $manifestText.EndsWith("`n")) {
        throw 'files.sha256 must use LF and end with LF'
    }
    $seen = @{}
    $manifestPaths = New-Object System.Collections.ArrayList
    foreach ($line in ($manifestText.TrimEnd("`n") -split "`n")) {
        if ($line -cnotmatch '^([0-9a-f]{64})  ([^\\]+)$') { throw "Invalid manifest line: $line" }
        $expected = $Matches[1]
        $relative = $Matches[2]
        if ($relative -match '(^/|^[A-Za-z]:|(^|/)\.\.(/|$))' -or
            $relative -eq 'files.sha256' -or $seen.ContainsKey($relative) -or
            -not $entries.ContainsKey($relative)) { throw "Invalid manifest entry: $relative" }
        $seen[$relative] = $true
        $null = $manifestPaths.Add($relative)
        if ((Get-ZipHash $entries[$relative]) -cne $expected) {
            throw "ZIP hash mismatch: $relative"
        }
    }
    $sorted = @($manifestPaths.ToArray() | Sort-Object)
    if (($manifestPaths.ToArray() -join "`n") -cne ($sorted -join "`n")) {
        throw 'Manifest paths are not sorted'
    }
    if ($seen.Count -ne $entries.Count - 1) { throw 'Manifest does not cover the entire payload' }
    foreach ($required in @('x64/optimizer-fps-dlss5-core.dll', 'x64/optimizer-fps-dlss5.addon64',
            'x64/nvngx.dll_optimizerfps.dll', 'x86/optimizer-fps-dlss5-remote.addon32', 'VERSION.txt')) {
        if (-not $seen.ContainsKey($required)) { throw "Missing ZIP payload: $required" }
    }
    $shaders = @($seen.Keys | Where-Object { $_ -match '^x64/optimizer-fps-dlss5/[^/]+\.dxbc$' })
    if ($shaders.Count -eq 0) { throw 'No DXBC in ZIP manifest' }
    if (@($seen.Keys | Where-Object { $_ -match '\.dxbc$' }).Count -ne $shaders.Count) {
        throw 'DXBC outside the expected x64 shader directory'
    }

    $version = (Read-ZipText $entries['VERSION.txt']).Trim()
    $versionCmake = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\cmake\Version.cmake'))
    $versionMatch = [regex]::Match($versionCmake, 'OFPS_RELEASE_VERSION\s+"([^"]+)"')
    if (-not $versionMatch.Success -or $version -ne $versionMatch.Groups[1].Value) {
        throw 'ZIP release version does not match cmake/Version.cmake'
    }
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('ofps-package-verify-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempRoot | Out-Null
    $corePath = Join-Path $tempRoot 'optimizer-fps-dlss5-core.dll'
    $addonPath = Join-Path $tempRoot 'optimizer-fps-dlss5.addon64'
    foreach ($copy in @(@{ Entry = $entries['x64/optimizer-fps-dlss5-core.dll']; Path = $corePath },
                       @{ Entry = $entries['x64/optimizer-fps-dlss5.addon64']; Path = $addonPath })) {
        $inputStream = $copy.Entry.Open()
        $outputStream = [IO.File]::Create($copy.Path)
        try { $inputStream.CopyTo($outputStream) }
        finally { $inputStream.Dispose(); $outputStream.Dispose() }
    }
    $coreInfo = Get-PeInfo $corePath
    $coreExports = Get-PeExportNames $corePath
    if ($null -eq $coreInfo -or $coreInfo.Arch -ne 'x64' -or
        (Get-Item $corePath).VersionInfo.FileVersion -ne $version -or
        $null -eq $coreExports -or $coreExports -notcontains 'OfpsCoreVersion' -or
        $coreExports -notcontains 'OfpsCreateCore') { throw 'ZIP core DLL is invalid' }
    $addonExports = Get-PeExportNames $addonPath
    if ($null -eq $addonExports -or -not (($addonExports -contains 'OptimizerFpsSetSettingV1') -or
        (($addonExports -contains 'PeripheralWarpSetLayoutV1') -and
         ($addonExports -contains 'PeripheralWarpSetTemporalV1')))) { throw 'ZIP add-on exports are invalid' }

    Write-Output "ZIP verified: core, addon, forwarder, remote, $($shaders.Count) DXBC; $($seen.Count) hashes; $($modulePaths.Count) modules"
}
finally {
    $archive.Dispose()
    if ($tempRoot -and (Test-Path -LiteralPath $tempRoot)) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}
