param(
    [string]$GameSource = 'W:\Game\Steam\steamapps\common\Baldurs Gate 3\bin',
    [string]$Build = '',
    [string]$Runtime = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Build) { $Build = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\out\build\x64')) }
if (-not $Runtime) { $Runtime = Join-Path $PSScriptRoot 'run_addon' }
. "$PSScriptRoot\..\Runtime-Payload.ps1"
# Read installed files only. Nothing is downloaded or written into the game directory.
$reshade = Join-Path $GameSource 'Reshade64.dll'
if ((Get-Item -LiteralPath $reshade).VersionInfo.FileVersion -notlike '6.8.*') {
    throw 'Expected the installed ReShade 6.8 add-on build in Reshade64.dll.'
}
$files = @{
    'optimizer-fps-dlss5-core.dll' = (Join-Path $Build 'core\Release\optimizer-fps-dlss5-core.dll')
    'dxgi.dll' = $reshade
    'renodx-dlss5.addon64' = (Join-Path $GameSource 'renodx-dlss5.addon64')
    'nvngx.dll_optimizerfps.dll' = (Join-Path $Build 'hosts\reshade\ngx_forwarder\Release\nvngx.dll_optimizerfps.dll')
    'optimizer-fps-dlss5.addon64' = (Join-Path $Build 'hosts\reshade\Release\optimizer-fps-dlss5.addon64')
}
foreach ($name in @('pw_bench12.exe', 'nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssnr.dll', 'settings.json')) {
    $files[$name] = Join-Path $PSScriptRoot "run_fork\$name"
}
foreach ($source in $files.Values) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing local file: $source" }
}
New-Item -ItemType Directory -Force -Path $Runtime | Out-Null
foreach ($name in $files.Keys) { Copy-Item -LiteralPath $files[$name] -Destination (Join-Path $Runtime $name) }
Copy-Item -LiteralPath "$PSScriptRoot\run_fork\shaders" -Destination $Runtime -Recurse -Force
Copy-CorePayload $Build $Runtime
$ini = Join-Path $Runtime 'ReShade.ini'
if (-not (Test-Path -LiteralPath $ini)) {
    @'
[GENERAL]
EffectSearchPaths=
TextureSearchPaths=

[OptimizerFPS]
Mode=2
Flags=0
CrashGuard=0
TemporalMode=1
TemporalEvery=4
TemporalDebugLog=1
TemporalShowZone=0
TemporalSeparateZone=0
TemporalResidualBlend=0.6
TemporalToneSmoothing=24

[RenoDX.DLSS5]
EnableHooks=2
NeuralUplift=1
NRAutoMask=0
NREnableUpscaling=0
NRGlobalTone=2
NRIntensity=2
NRLocalStructure=2
NRLocalTone=2
NRPaperWhiteScale=1
NRPreset=0
NRSkinStructure=2
NRStyle=0
NRTransferStrength=1
NRUICorrection=0
'@ | Set-Content -LiteralPath $ini -Encoding UTF8
}
Write-Output "Add-on lab runtime: $Runtime"
