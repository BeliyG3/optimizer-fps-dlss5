param(
    [string] $Pixel = 'plan5-task8-pixel',
    [string] $Compute = 'plan5-task8-compute',
    [string] $OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$scene = (Resolve-Path (Join-Path $PSScriptRoot '..\bench\assets\lab_scene.glb')).Path
$cases = @(
    @{ Runtime = 'run_addon'; Name = 'warp'; Upscaler = 'rr'; Rebind = $true },
    @{ Runtime = 'run_r521'; Name = 'native_warp'; Upscaler = 'rr'; Rebind = $true },
    @{ Runtime = 'run_nrhost'; Name = 'warp_t0'; Upscaler = 'sr'; Rebind = $true },
    @{ Runtime = 'run_nrhost'; Name = 'core_warp'; Upscaler = 'sr'; Rebind = $true; Core = $true }
)

foreach ($path in @(@{ Name = $Pixel; Id = 2 }, @{ Name = $Compute; Id = 1 })) {
    foreach ($case in $cases) {
        $runtime = Join-Path $PSScriptRoot $case.Runtime
        $candidate = if ($OutputRoot) {
            Join-Path (Join-Path $OutputRoot $case.Runtime) $path.Name
        } else { Join-Path $runtime $path.Name }
        $work = if ($case.Core) { Join-Path $candidate 'core\core-runtime' } else { $runtime }
        $evidence = Join-Path $candidate $(if ($case.Core) { 'core\state_sentinel' } else { 'state_sentinel' })
        if (!(Test-Path -LiteralPath $work -PathType Container)) { throw "Missing runtime: $work" }
        if (Test-Path -LiteralPath $evidence) {
            $retry = 2
            while (Test-Path -LiteralPath "$evidence-r$retry") { $retry++ }
            $evidence = "$evidence-r$retry"
        }
        New-Item -ItemType Directory -Path $evidence | Out-Null
        $ini = Join-Path $runtime 'ReShade.ini'
        $saved = if ($case.Core) { $null } else { [IO.File]::ReadAllBytes($ini) }
        try {
            if (!$case.Core) {
                $snapshot = Join-Path $candidate ($case.Name + '_ReShade.ini')
                if (!(Test-Path -LiteralPath $snapshot -PathType Leaf)) { throw "Missing $snapshot" }
                Copy-Item -LiteralPath $snapshot -Destination $ini -Force
            }
            foreach ($mode in @('draw', 'compute')) {
                $args = @('12', '--host', $(if ($case.Core) { 'core' } else { 'ngx' }),
                    '--gltf', ('"' + $scene + '"'), '--upscaler', $case.Upscaler,
                    '--sun-dir', '0.45,-0.77,0.45', '--sun-strength', '1500',
                    '--exposure', '0.22', '--haze', '0.008', '--fov', '62',
                    '--core-state-sentinel', $mode, '--debug-layer')
                if ($case.Core) { $args += @('--core-mode', '2', '--core-warp-path', [string]$path.Id) }
                if (($case.Rebind -or ($case.Runtime -eq 'run_r521' -and $mode -eq 'draw')) -and !$case.Core) {
                    $args += '--core-state-rebind'
                }
                if ($case.Upscaler -eq 'sr') {
                    $args += @('--nr', 'native', '--mv-format', 'rgba16f',
                        '--nr-colour-pad', '64,32', '--nr-output-pad', '64,32')
                }
                $stdout = Join-Path $evidence "$mode.stdout.txt"
                $stderr = Join-Path $evidence "$mode.stderr.txt"
                $process = Start-Process -FilePath (Join-Path $work 'pw_bench12.exe') -ArgumentList $args `
                    -WorkingDirectory $work -Wait -PassThru -WindowStyle Hidden `
                    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
                $output = [IO.File]::ReadAllText($stdout)
                if ($process.ExitCode -ne 0 -or !$output.Contains('core state sentinel: PASS') -or
                    !$output.Contains('device ok')) {
                    throw "State sentinel failed: $($case.Runtime) $($path.Name) $mode exit=$($process.ExitCode)"
                }
                Write-Output "PASS $($case.Runtime) $($case.Name) $($path.Name) $mode"
            }
        } finally {
            if ($null -ne $saved) { [IO.File]::WriteAllBytes($ini, $saved) }
        }
    }
}
