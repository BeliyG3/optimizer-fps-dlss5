#Requires -Version 5.1
# Menu-mode spike 0c: the menu pipeline over the D3D11 <-> D3D12 bridge on the D3D11 bench.
# Runtime: a folder under tools\bench\run with pw_bench.exe's chain (ReShade 6.8 as dxgi.dll, dlss5-dx11-bridge,
# renodx-dlss5, nvngx_dlss/nvngx_dlssnr, ReShade.ini); the add-on payload is deployed from -Build, pw_bench.exe
# from tools\bench\run (build.cmd). Each case: ini keys, the bench with --nr-pause and --dump-back, then the
# add-on's menu output and the bench's pre-present dumps moved into -Out as <case>_*.
#   .\menu_bridge.ps1 -Runtime run\menu_bridge -Out $env:TEMP\mb -Case mb_off,mb_marker
# Checks: ..\bench12\menu_check.ps1 (counts, marker centre, probe == bench dump), menu_bridge_compare.py (pixels).
param(
    [Parameter(Mandatory = $true)] [string] $Runtime,
    [Parameter(Mandatory = $true)] [string] $Out,
    [string[]] $Case = @(),
    [string] $Build = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Build) { $Build = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\out\build\x64')) }
$Case = @($Case | ForEach-Object { $_ -split ',' })
$run = if (Test-Path -LiteralPath $Runtime -PathType Container) { (Resolve-Path -LiteralPath $Runtime).Path } else { Join-Path $PSScriptRoot $Runtime }
$ini = Join-Path $run 'ReShade.ini'
if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) { throw "No ReShade.ini in $run" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
. "$PSScriptRoot\..\Runtime-Payload.ps1"
. "$PSScriptRoot\..\bench12\menu_check.ps1"
Copy-CorePayload $Build $run
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'run\pw_bench.exe') -Destination $run -Force

function Set-IniKey([string] $Section, [string] $Key, [string] $Value) {
    $lines = [System.Collections.Generic.List[string]]([IO.File]::ReadAllLines($ini))
    $start = $lines.IndexOf("[$Section]")
    if ($start -lt 0) { $lines.Add(''); $lines.Add("[$Section]"); $start = $lines.Count - 1 }
    $i = $start + 1
    while ($i -lt $lines.Count -and -not $lines[$i].StartsWith('[')) {
        if ($lines[$i] -like "$Key=*") { $lines[$i] = "$Key=$Value"; [IO.File]::WriteAllLines($ini, $lines); return }
        $i++
    }
    $lines.Insert($start + 1, "$Key=$Value")
    [IO.File]::WriteAllLines($ini, $lines)
}

# Ten pauses of 30 frames. With DebugMenuEntryMs=0 a run enters at the 5th quiet present (A+4), its 10th menu
# present is A+13, and the five after the exit are B..B+4: the bench dumps the same frames before Present, and
# A+12 (a menu frame differs from the one before it, so a stale write-back would show).
$pairs = @(60, 90, 120, 150, 180, 210, 240, 270, 300, 330, 360, 390, 420, 450, 480, 510, 540, 570, 600, 630)
$pause = @('--nr-pause', ($pairs -join ','))
$dumps = @('30')
for ($k = 0; $k -lt $pairs.Count; $k += 2) {
    $dumps += @([string]($pairs[$k] + 12), [string]($pairs[$k] + 13)); $dumps += @(0..4 | ForEach-Object { [string]($pairs[$k + 1] + $_) })
}
$base = @{ MenuMode = '1'; DebugMenuPass = '0'; DebugMenuDump = '1'; DebugMenuEntryMs = '0'; DebugMenuOwnBlock = '0'; DebugMenuNoFlow = '0' }
function Join-Ini([hashtable] $Over) { $all = $base.Clone(); foreach ($k in $Over.Keys) { $all[$k] = $Over[$k] }; $all }
$dbg = @('--debug-layer')
$configs = [ordered]@{
    'mb_off'          = @{ Ini = (Join-Ini @{ MenuMode = '0' }); Extra = $pause; Menu = '' }
    'mb_off_dbg'      = @{ Ini = (Join-Ini @{ MenuMode = '0' }); Extra = $pause + @('--debug-layer'); Menu = '' } # the chain's own debug messages
    'mb_nr_ref'       = @{ Ini = (Join-Ini @{ MenuMode = '0' }); Extra = @(); Menu = '' }      # no pause: the in-game NR frames
    'mb_marker'       = @{ Ini = (Join-Ini @{ DebugMenuPass = '1' }); Extra = $pause; Menu = 'marker' }
    'mb_marker150'    = @{ Ini = (Join-Ini @{ DebugMenuPass = '1'; DebugMenuEntryMs = '150' }); Extra = $pause; Menu = 'marker' }
    'mb_marker_dbg'   = @{ Ini = (Join-Ini @{ DebugMenuPass = '1' }); Extra = $pause + $dbg; Menu = 'marker' }
    'mb_fail'         = @{ Ini = (Join-Ini @{ DebugMenuPass = '2' }); Extra = $pause; Menu = 'none' }
    'mb_fail_dbg'     = @{ Ini = (Join-Ini @{ DebugMenuPass = '2' }); Extra = $pause + $dbg; Menu = 'none' }
    'mb_model'        = @{ Ini = (Join-Ini @{}); Extra = $pause; Menu = 'model' }
    'mb_model_dbg'    = @{ Ini = (Join-Ini @{}); Extra = $pause + $dbg; Menu = 'model' }
    # Stage 3 on D3D11 (Task 10): Mode Peripheral, the menu frame through the core on the bridge's private queue.
    'mb_model_per'     = @{ Ini = (Join-Ini @{ Mode = '2' }); Extra = $pause; Menu = 'model' }
    'mb_model_per_dbg' = @{ Ini = (Join-Ini @{ Mode = '2' }); Extra = $pause + $dbg; Menu = 'model' }
    'mb_off_per'       = @{ Ini = (Join-Ini @{ MenuMode = '0'; Mode = '2' }); Extra = $pause; Menu = '' } # the pause without menu mode
    'mb_nr_ref_per'    = @{ Ini = (Join-Ini @{ MenuMode = '0'; Mode = '2' }); Extra = @(); Menu = '' }   # no pause: in-game NR, Peripheral
    # Stage 3 (Task 13): the sync temporal cadence (every 4th frame) in menus, carried along the optical flow; Mode Off and
    # Peripheral, each with its in-game NR reference.
    'mb_model_t1'       = @{ Ini = (Join-Ini @{ TemporalMode = '1'; TemporalEvery = '4' }); Extra = $pause; Menu = 'model' }
    'mb_nr_ref_t1'      = @{ Ini = (Join-Ini @{ MenuMode = '0'; TemporalMode = '1'; TemporalEvery = '4' }); Extra = @(); Menu = '' }
    'mb_model_per_t1'   = @{ Ini = (Join-Ini @{ Mode = '2'; TemporalMode = '1'; TemporalEvery = '4' }); Extra = $pause; Menu = 'model' }
    'mb_nr_ref_per_t1'  = @{ Ini = (Join-Ini @{ MenuMode = '0'; Mode = '2'; TemporalMode = '1'; TemporalEvery = '4' }); Extra = @(); Menu = '' }
    # The process ends inside a menu (TerminateProcess with model passes in flight; the bench never releases its feature).
    'mb_exit'         = @{ Ini = (Join-Ini @{}); Extra = @('--nr-pause', '380,420'); Frames = 400; Dump = @('30', '393'); Menu = 'exit' }
    'mb_exit_off'     = @{ Ini = (Join-Ini @{ MenuMode = '0' }); Extra = @('--nr-pause', '380,420'); Frames = 400; Dump = @('30'); Menu = '' }
}
if (-not $Case) { $Case = @($configs.Keys) }
foreach ($name in $Case) {
    if (-not $configs.Contains($name)) { throw "Unknown case $name" }
    $cfg = $configs[$name]
    Remove-Item -LiteralPath (Join-Path $run 'optimizer-fps-dlss5.session') -ErrorAction SilentlyContinue # TerminateProcess: no crash guard
    Set-IniKey 'OptimizerFPS' 'CrashGuard' '0'
    Set-IniKey 'OptimizerFPS' 'Mode' '0'
    Set-IniKey 'OptimizerFPS' 'TemporalMode' '0'
    Set-IniKey 'RenoDX.DLSS5' 'NeuralUplift' '1' # renodx must create feature 18
    foreach ($k in $cfg.Ini.Keys) { Set-IniKey 'OptimizerFPS' $k $cfg.Ini[$k] }
    # The bridge's debug-layer messages (and the canary) in ReShade.log only with the debug layer (a bench-only key).
    Set-IniKey 'OptimizerFPS' 'DebugMenuBridgeCanary' $(if ($cfg.Extra -contains '--debug-layer') { '1' } else { '0' })
    Clear-MenuOutput $run
    Get-ChildItem -LiteralPath $run -Filter 'dump_*.bmp' | Remove-Item
    $frames = if ($cfg.Frames) { $cfg.Frames } else { 660 }
    $dumpList = if ($cfg.Dump) { $cfg.Dump } else { $dumps }
    $argv = @([string]$frames, '--fps-cap', '60', '--dump-back', '--dump', ($dumpList -join ',')) + $cfg.Extra
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath (Join-Path $run 'pw_bench.exe') -ArgumentList $argv -WorkingDirectory $run -Wait -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $Out "$name`_stdout.log") -RedirectStandardError (Join-Path $Out "$name`_stderr.log")
    $exit = $p.ExitCode
    $wall = $watch.Elapsed.TotalSeconds
    Copy-Item -LiteralPath (Join-Path $run 'ReShade.log') -Destination (Join-Path $Out "$name`_ReShade.log") -Force
    Copy-Item -LiteralPath (Join-Path $run 'dlss5-dx11-bridge.log') -Destination (Join-Path $Out "$name`_bridge.log") -Force -ErrorAction SilentlyContinue
    foreach ($bmp in Get-ChildItem -LiteralPath $run -Filter 'dump_*.bmp') { Move-Item -LiteralPath $bmp.FullName -Destination (Join-Path $Out "$name`_$($bmp.Name)") -Force }
    $line = "{0}: exit {1}, {2:N1} s" -f $name, $exit, $wall
    if ($cfg.Menu -and $cfg.Menu -ne 'exit') {
        $expect = if ($cfg.Menu -eq 'model') { 'model' } else { $cfg.Menu }
        $check = Get-MenuCheck $run $Out $name $expect '30' 10 0
        $line += '; ' + (($check.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')
    } elseif ($cfg.Menu -eq 'exit') {
        $null = Get-MenuCheck $run $Out $name 'model' '30' 0 0
    } else { Clear-MenuOutput $run }
    Write-Output $line
}
Set-IniKey 'OptimizerFPS' 'MenuMode' '0'
Set-IniKey 'OptimizerFPS' 'DebugMenuPass' '0'
Set-IniKey 'OptimizerFPS' 'DebugMenuDump' '0'
Set-IniKey 'OptimizerFPS' 'DebugMenuBridgeCanary' '0'
