param(
    [Parameter(Mandatory = $true)][string] $Runtime,
    [Parameter(Mandatory = $true)][string] $Out,
    [Parameter(Mandatory = $true)][string] $Build,
    [ValidateSet('auto','compute','pixel')][string] $WarpPath = 'auto',
    [string[]] $Case,
    [int] $Frames = 240,
    [int[]] $DumpFrames = @(120, 239)
)
$ErrorActionPreference = 'Stop'
$Runtime = [IO.Path]::GetFullPath($Runtime)
$Out = [IO.Path]::GetFullPath($Out)
. "$PSScriptRoot\..\Runtime-Payload.ps1"
$configs = [ordered]@{
    core_warp = @{ Mode = '2'; Temporal = '0'; Reference = 'warp_t0' }
    core_off = @{ Mode = '0'; Temporal = '0'; Reference = 'off_t0' }
    core_warp_t1 = @{ Mode = '2'; Temporal = '1'; Reference = 'warp_t1' }
}
foreach ($name in $Case) { if (-not $configs.Contains($name)) { throw "Unknown core case: $name" } }
if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) {
    throw "Output is not empty: $Out"
}
New-Item -ItemType Directory -Path $Out -Force | Out-Null
# A fresh allowlist directory keeps ReShade and addon modules out of the direct process.
$run = Join-Path $Out 'core-runtime'
New-Item -ItemType Directory -Path "$run\shaders\bin", "$run\optimizer-fps-dlss5" -Force | Out-Null
foreach ($name in @('pw_bench12.exe', 'nvngx.dll_pwbench12.dll')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $run
}
foreach ($name in @('nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssnr.dll')) {
    Copy-Item -LiteralPath (Join-Path $Runtime $name) -Destination $run
}
Copy-CoreShaders "$Build\core\Release\optimizer-fps-dlss5-core.dll" "$Build\shaders" $run
Copy-Item -Path "$PSScriptRoot\shaders\bin\*.cso" -Destination "$run\shaders\bin"
$scene = (Resolve-Path "$PSScriptRoot\..\bench\assets\lab_scene.glb").Path
$binaries = [ordered]@{}
Get-ChildItem -LiteralPath $run -File -Recurse | ForEach-Object {
    $binaries[$_.FullName.Substring($run.Length + 1)] = (Get-FileHash -LiteralPath $_.FullName).Hash
}
$runs = New-Object System.Collections.Generic.List[object]
foreach ($name in $Case) {
    $cfg = $configs[$name]
    $warpId = @{ auto='0'; compute='1'; pixel='2' }[$WarpPath]
    $argv = @("$Frames", '--host', 'core', '--core-mode', $cfg.Mode, '--core-temporal', $cfg.Temporal,
        '--core-warp-path', $warpId,
        '--gltf', ('"' + $scene + '"'), '--sun-dir', '0.45,-0.77,0.45', '--sun-strength', '1500',
        '--exposure', '0.22', '--haze', '0.008', '--fov', '62', '--dump', ($DumpFrames -join ','),
        '--upscaler', 'sr', '--nr', 'native', '--mv-format', 'rgba16f', '--nr-colour-pad', '64,32', '--nr-output-pad', '64,32')
    $process = Start-Process -FilePath "$run\pw_bench12.exe" -ArgumentList $argv -WorkingDirectory $run `
        -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput "$Out\$name.txt" -RedirectStandardError "$Out\$name.err.txt"
    Copy-Item -LiteralPath "$run\ngx_final_audit.log" -Destination "$Out\$name`_ngx_audit.log"
    $stdout = [IO.File]::ReadAllText("$Out\$name.txt")
    $effectiveWarpPath = if ($stdout.Contains('[core eval] warpPath=Compute')) { 'compute' } `
        elseif ($stdout.Contains('[core eval] warpPath=Pixel')) { 'pixel' } else { 'none' }
    $referenceIni = Join-Path $Runtime ("reference_before_2026.9.1/" + $cfg.Reference + '_ReShade.ini')
    & python "$PSScriptRoot\check_core_settings.py" --log "$Out\$name.txt" --ini $referenceIni --warp-path $warpId
    if ($LASTEXITCODE -ne 0) { throw "Core effective settings differ: $name" }
    $pathMatches = $WarpPath -eq 'auto' -or $cfg.Mode -eq '0' -or $effectiveWarpPath -eq $WarpPath
    $ok = $process.ExitCode -eq 0 -and $stdout.Contains('device ok') -and $pathMatches -and
        $stdout.Contains('one core, zero addons, system DXGI') -and $stdout.Contains('drain completed') -and
        ($cfg.Mode -eq '0' -or $stdout.Contains('first warped evaluate completed')) -and
        ($cfg.Temporal -eq '0' -or $stdout.Contains('temporal machine ready'))
    foreach ($frame in $DumpFrames) {
        $dump = "$run\dump_$frame.bmp"
        if (Test-Path -LiteralPath $dump) { Move-Item -LiteralPath $dump -Destination "$Out\$name`_dump_$frame.bmp" }
        else { $ok = $false }
    }
    $runs.Add([ordered]@{ name = $name; argv = @($argv | ForEach-Object { $_.Trim('"') }); working_directory = $run; binaries = $binaries; reference = $cfg.Reference; command = ($argv -join ' ')
        mode = $cfg.Mode; temporal = $cfg.Temporal; warp_path = $WarpPath; effective_warp_path = $effectiveWarpPath; exit = $process.ExitCode; ok = $ok })
    Write-Output "$name exit $($process.ExitCode) ok $ok"
    if (-not $ok) { throw "Core case failed: $name" }
}
$repo = [IO.Path]::GetFullPath("$PSScriptRoot\..\..")
$sourceHashes = [ordered]@{}
foreach ($relative in (& git -C $repo ls-files --cached --others --exclude-standard | Sort-Object -Unique)) {
    $file = Join-Path $repo $relative
    if (Test-Path -LiteralPath $file -PathType Leaf) { $sourceHashes[$relative] = (Get-FileHash -LiteralPath $file).Hash }
}
[ordered]@{ source_sha = (& git -C $repo rev-parse HEAD); binaries = $binaries; source_sha256 = $sourceHashes
    runs = $runs.ToArray() } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$Out\manifest.json" -Encoding UTF8
