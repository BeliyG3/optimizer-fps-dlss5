param(
    [string] $GameBin = 'D:\Game\Steam\steamapps\common\the witcher 2\bin',
    [string] $Build = '',
    [string] $RemoteBuild = '',
    [string] $Runtime = ''
)
# Lays out the runtime of pw_bench9 from a 32-bit game that already runs the chain (by default the
# Witcher 2 install): dgVoodoo and ReShade x86 with feed32 and the remote tab beside the bench, the
# 64-bit Neural Rendering host in host64\. Reads the game's files only; nothing is downloaded and
# nothing is written into the game. An existing runtime keeps its ini files, so settings tuned for
# the bench survive a redeploy.
$ErrorActionPreference = 'Stop'
if (-not $Build) { $Build = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\out\build\x64')) }
if (-not $RemoteBuild) { $RemoteBuild = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\out\build\x86-remote')) }
if (-not $Runtime) { $Runtime = Join-Path $PSScriptRoot 'run' }
. "$PSScriptRoot\..\Runtime-Payload.ps1"
$null = Get-CorePayload $Build
$remoteAddon = Join-Path $RemoteBuild 'hosts\remote32\Release\optimizer-fps-dlss5-remote.addon32'
if (-not (Test-Path -LiteralPath $remoteAddon -PathType Leaf)) { throw "Missing remote add-on: $remoteAddon" }
$host64 = Join-Path $GameBin 'host64'
$top = @('d3d9.dll', 'dgVoodoo.conf', 'dxgi.dll', 'dlss5-feed.addon32')
$topIni = @('ReShade.ini', 'ReShadePreset.ini', 'dlss5-feed.cfg')
$hostFiles = @('dlss5-feed-host64.exe', 'dxgi.dll', 'nvngx_dlss.dll', 'nvngx_dlssnr.dll',
               'renodx-dlss5.addon64', 'THIRD-PARTY-LICENSES.txt')
$hostIni = @('ReShade.ini', 'dlss5-feed-host64.cfg')

New-Item -ItemType Directory -Force -Path (Join-Path $Runtime 'host64') | Out-Null
foreach ($f in $top) { Copy-Item -LiteralPath (Join-Path $GameBin $f) -Destination $Runtime -Force }
Copy-Item -LiteralPath $remoteAddon -Destination $Runtime -Force
foreach ($f in $topIni) {
    $dst = Join-Path $Runtime $f
    if (-not (Test-Path -LiteralPath $dst)) { Copy-Item -LiteralPath (Join-Path $GameBin $f) -Destination $dst }
}
Copy-Item -LiteralPath (Join-Path $GameBin 'reshade-shaders') -Destination $Runtime -Recurse -Force
foreach ($f in $hostFiles) { Copy-Item -LiteralPath (Join-Path $host64 $f) -Destination (Join-Path $Runtime 'host64') -Force }
foreach ($f in $hostIni) {
    $dst = Join-Path $Runtime ('host64\' + $f)
    if (-not (Test-Path -LiteralPath $dst)) { Copy-Item -LiteralPath (Join-Path $host64 $f) -Destination $dst }
}
Copy-CorePayload $Build (Join-Path $Runtime 'host64')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'pw_bench9.exe') -Destination $Runtime -Force
Write-Host ('pw_bench9 runtime: ' + $Runtime)
