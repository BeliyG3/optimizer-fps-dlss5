param(
    [string]$Source = '',
    [string]$Runtime = '',
    # SDK build to take the Optimizer FPS add-on and its shaders from instead of $Source.
    [string]$AddonBuild = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Source) { $Source = Join-Path $PSScriptRoot 'run_addon' }
if (-not $Runtime) { $Runtime = Join-Path $PSScriptRoot 'run_nrhost' }
. "$PSScriptRoot\..\Runtime-Payload.ps1"
# run_nrhost: the bench as the only DLSS Neural Rendering host, next to ReShade and the Optimizer FPS
# add-on. Files come from run_addon (renodx-dlss5.addon64 deliberately excluded) and from this build.
# Local files only; an existing ReShade.ini in the runtime is kept. Nothing is downloaded.
$fromSource = @('dxgi.dll', 'nvngx.dll_optimizerfps.dll', 'nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssnr.dll',
    'optimizer-fps-dlss5.addon64', 'optimizer-fps-dlss5-core.dll', 'settings.json')
$fromBuild = @('pw_bench12.exe', 'nvngx.dll_pwbench12.dll')
if ($AddonBuild) {
    $fromSource = @($fromSource | Where-Object { $_ -notin @('optimizer-fps-dlss5.addon64', 'optimizer-fps-dlss5-core.dll', 'nvngx.dll_optimizerfps.dll') })
    $null = Get-CorePayload $AddonBuild
}
foreach ($name in $fromSource) {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $name) -PathType Leaf)) { throw "Missing $Source\$name" }
}
foreach ($name in $fromBuild) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $name) -PathType Leaf)) { throw "Missing $PSScriptRoot\$name; run build.cmd" }
}
New-Item -ItemType Directory -Force -Path "$Runtime\shaders\bin", "$Runtime\optimizer-fps-dlss5" | Out-Null
foreach ($name in $fromSource) { Copy-Item -LiteralPath (Join-Path $Source $name) -Destination $Runtime }
foreach ($name in $fromBuild) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $Runtime }
Copy-Item -Path "$PSScriptRoot\shaders\bin\*.cso" -Destination "$Runtime\shaders\bin"
if ($AddonBuild) { Copy-CorePayload $AddonBuild $Runtime }
else { Copy-CoreShaders "$Source\optimizer-fps-dlss5-core.dll" "$Source\optimizer-fps-dlss5" $Runtime }
if (-not (Test-Path -LiteralPath "$Runtime\ReShade.ini")) { Copy-Item -LiteralPath "$Source\ReShade.ini" -Destination $Runtime }
# Control runs without ReShade use the build directory itself, which also needs the NR runtime.
if (-not (Test-Path -LiteralPath "$PSScriptRoot\nvngx_dlssnr.dll")) { Copy-Item -LiteralPath "$Source\nvngx_dlssnr.dll" -Destination $PSScriptRoot }
if (Test-Path -LiteralPath "$Runtime\renodx-dlss5.addon64") { Write-Warning "$Runtime\renodx-dlss5.addon64 exists; remove it so the bench is the only NR host" }
Write-Output "NR host runtime: $Runtime (add-on: $(if ($AddonBuild) { $AddonBuild } else { $Source }))"
