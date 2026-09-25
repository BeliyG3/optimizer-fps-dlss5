param(
    [string[]] $Only,
    [int] $Frames = 900,
    [string] $Dumps = '600,650,700',
    [string] $CapturesRoot = ''
)
# Runs pw_bench9 once per configuration and keeps the captured frames in run\captures\<label>.
# A configuration sets the Neural Rendering switch (renodx), the Optimizer FPS warp and temporal modes
# and the optical flow grid of the 64-bit host, then restores nothing: the next one sets all of them.
$ErrorActionPreference = 'Stop'
$run = Join-Path $PSScriptRoot 'run'
$capturesRoot = if ($CapturesRoot) { [IO.Path]::GetFullPath($CapturesRoot) } else { Join-Path $run 'captures' }
$hostIni = Join-Path $run 'host64\ReShade.ini'
$hostCfg = Join-Path $run 'host64\dlss5-feed-host64.cfg'

$configs = @(
    # 1080p window, Witcher 2 settings (centre 80 %, bilinear, temporal every 5 frames)
    @{ Label = 'nr_off';  Nr = 0; Mode = 2; Temporal = 1; Grid = 4; Perf = 10 },
    @{ Label = 'game';    Nr = 1; Mode = 2; Temporal = 1; Grid = 4; Perf = 10 },
    @{ Label = 'grid1';   Nr = 1; Mode = 2; Temporal = 1; Grid = 1; Perf = 5 },
    @{ Label = 'notemp';  Nr = 1; Mode = 2; Temporal = 0; Grid = 4; Perf = 10 },
    @{ Label = 'nowarp';  Nr = 1; Mode = 0; Temporal = 1; Grid = 4; Perf = 10 },
    @{ Label = 'plain';   Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 10 },
    # 4K on the second monitor, Dragon Age settings (centre 65.6 x 65.9 %, soft filter, no temporal)
    @{ Label = 'da_off';     Nr = 0; Mode = 2; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_game';    Nr = 1; Mode = 2; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_nowarp';  Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_grid1';   Nr = 1; Mode = 2; Temporal = 0; Grid = 1; Perf = 5;  Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_bilinear'; Nr = 1; Mode = 2; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 0; K = 1 },
    # separating the grid from the quality, both without the warp
    @{ Label = 'da_g1_fast'; Nr = 1; Mode = 0; Temporal = 0; Grid = 1; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_g4_slow'; Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 5;  Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_g2_fast'; Nr = 1; Mode = 0; Temporal = 0; Grid = 2; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    # the depth-guided smoothing of the optical flow cells (motion_smooth.hlsl), on and off
    @{ Label = 'da_smooth';      Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_nosmooth';    Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1; NoSmooth = 1 },
    @{ Label = 'da_game_smooth'; Nr = 1; Mode = 2; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_smooth_t08';  Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1; Tol = '0.08' },
    @{ Label = 'da_smooth_t15';  Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1; Tol = '0.15' },
    @{ Label = 'da_smooth_t30';  Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1; Tol = '0.30' },
    # repeats, for the run-to-run noise of the model's history
    @{ Label = 'da_nosmooth_r2'; Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1; NoSmooth = 1 },
    @{ Label = 'da_smooth_r2';   Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_g1_fast_r2';  Nr = 1; Mode = 0; Temporal = 0; Grid = 1; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_smooth_r3';   Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_smooth_r4';   Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    @{ Label = 'da_smooth_r5';   Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; Center = '65.6,65.9'; Filter = 1; K = 1 },
    # cost of the optical flow quality: 1080p, long enough for the host's 600-frame timing line
    @{ Label = 'cost_g4_fast'; Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 20; NoSmooth = 1 },
    @{ Label = 'cost_g4_slow'; Nr = 1; Mode = 0; Temporal = 0; Grid = 4; Perf = 5;  NoSmooth = 1 },
    @{ Label = 'cost_g2_fast'; Nr = 1; Mode = 0; Temporal = 0; Grid = 2; Perf = 20; NoSmooth = 1 },
    @{ Label = 'cost_g1_fast'; Nr = 1; Mode = 0; Temporal = 0; Grid = 1; Perf = 20; NoSmooth = 1 },
    # The Witcher EE settings (2026-09-21): 4K, centre 80 %, soft filter, temporal every 4 frames, fast
    # grid-4 optical flow, and the model passes that compound its square blotches. NoHist = the extra
    # passes keep no history of their own (DebugPassNoHistory); Spread = one pass per frame.
    @{ Label = 'w_off';       Nr = 0; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 1 },
    @{ Label = 'w_ref';       Nr = 1; Mode = 0; Temporal = 0; Grid = 1; Perf = 5;  Center = '80,80'; Filter = 1; K = 1; Passes = 1 },
    @{ Label = 'w_1p';        Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 1 },
    @{ Label = 'w_2p';        Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'w_2p_nh';     Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2; NoHist = 1 },
    @{ Label = 'w_2p_sp';     Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2; Spread = 1 },
    @{ Label = 'w_2p_sp_nh';  Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2; Spread = 1; NoHist = 1 },
    @{ Label = 'w_3p';        Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 3 },
    @{ Label = 'w_3p_nh';     Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 3; NoHist = 1 },
    @{ Label = 'w_1p_t0';     Nr = 1; Mode = 2; Temporal = 0; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 1 },
    @{ Label = 'w_2p_t0';     Nr = 1; Mode = 2; Temporal = 0; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'w_2p_t0_nh';  Nr = 1; Mode = 2; Temporal = 0; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2; NoHist = 1 },
    @{ Label = 'w_2p_r2';     Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'w_2p_nh_r2';  Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2; NoHist = 1 },
    @{ Label = 'w_1p_r2';     Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 1 },
    # Does the optical flow's quality cost anything once two model passes load the GPU? The flow runs on
    # its own engine; with the NR passes this long it may finish inside the previous frame's work.
    @{ Label = 'q_2p_g4f';    Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'q_2p_g4s';    Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 5;  Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'q_2p_g4m';    Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 10; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'q_2p_g2f';    Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 2; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'q_2p_g2s';    Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 2; Perf = 5;  Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'q_1p_g4f';    Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 1 },
    @{ Label = 'q_1p_g4s';    Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 5;  Center = '80,80'; Filter = 1; K = 1; Passes = 1 },
    @{ Label = 'q_2p_g4f_r2'; Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 'q_2p_g4s_r2'; Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 5;  Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    # Timing only, alternated so a drift of the GPU's clocks cannot favour one side: long runs, the
    # game side's 600-frame lines after the first one are the steady state.
    @{ Label = 't_f1'; Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 't_s1'; Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 5;  Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 't_f2'; Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 't_s2'; Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 5;  Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 't_f3'; Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 20; Center = '80,80'; Filter = 1; K = 1; Passes = 2 },
    @{ Label = 't_s3'; Nr = 1; Mode = 2; Temporal = 1; Every = 4; Grid = 4; Perf = 5;  Center = '80,80'; Filter = 1; K = 1; Passes = 2 }
)
# -File hands a comma list over as one string; split it either way.
if ($Only) { $want = @($Only | ForEach-Object { $_ -split ',' }); $configs = $configs | Where-Object { $want -contains $_.Label } }

function Set-IniKey([string] $Path, [string] $Section, [string] $Key, [string] $Value)
{
    $lines = [System.Collections.Generic.List[string]]([IO.File]::ReadAllLines($Path))
    $inSection = $false; $foundSection = $false; $done = $false; $sectionEnd = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match '^\[(.+)\]\s*$') {
            if ($inSection -and -not $done) { $sectionEnd = $i; break }
            $inSection = ($Matches[1] -eq $Section)
            if ($inSection) { $foundSection = $true }
            continue
        }
        if ($inSection -and $lines[$i] -match ('^' + [regex]::Escape($Key) + '=')) { $lines[$i] = $Key + '=' + $Value; $done = $true; break }
    }
    if (-not $done) {
        if (-not $foundSection) { $lines.Add(''); $lines.Add("[$Section]") }
        if ($sectionEnd -lt 0) { $sectionEnd = $lines.Count }
        $lines.Insert($sectionEnd, $Key + '=' + $Value)
    }
    [IO.File]::WriteAllLines($Path, $lines)
}

function Set-CfgKey([string] $Path, [string] $Key, [string] $Value)
{
    $lines = [System.Collections.Generic.List[string]]([IO.File]::ReadAllLines($Path))
    $found = $false
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match ('^' + [regex]::Escape($Key) + '=')) { $lines[$i] = $Key + '=' + $Value; $found = $true }
    }
    if (-not $found) { $lines.Add($Key + '=' + $Value) }
    [IO.File]::WriteAllLines($Path, $lines)
}

foreach ($c in $configs) {
    Set-IniKey $hostIni 'RenoDX.DLSS5' 'NeuralUplift' ([string]$c.Nr)
    Set-IniKey $hostIni 'OptimizerFPS' 'Mode' ([string]$c.Mode)
    Set-IniKey $hostIni 'OptimizerFPS' 'Flags' '0'
    Set-IniKey $hostIni 'OptimizerFPS' 'TemporalMode' ([string]$c.Temporal)
    Set-IniKey $hostIni 'OptimizerFPS' 'CrashGuard' '0'
    Set-IniKey $hostIni 'OptimizerFPS' 'DebugMotionSmoothTolerance' ([string]$(if ($c.ContainsKey('Tol')) { $c.Tol } else { '-1' }))
    Set-IniKey $hostIni 'OptimizerFPS' 'DebugNoMotionSmooth' ([string]$(if ($c.ContainsKey('NoSmooth')) { $c.NoSmooth } else { 0 }))
    Set-IniKey $hostIni 'OptimizerFPS' 'ModelPasses' ([string]$(if ($c.ContainsKey('Passes')) { $c.Passes } else { 1 }))
    Set-IniKey $hostIni 'OptimizerFPS' 'SpreadPasses' ([string]$(if ($c.ContainsKey('Spread')) { $c.Spread } else { 0 }))
    Set-IniKey $hostIni 'OptimizerFPS' 'DebugPassNoHistory' ([string]$(if ($c.ContainsKey('NoHist')) { $c.NoHist } else { 0 }))
    Set-IniKey $hostIni 'OptimizerFPS' 'DebugTiming' '1'
    if ($c.ContainsKey('Every')) { Set-IniKey $hostIni 'OptimizerFPS' 'TemporalEvery' ([string]$c.Every) }
    Set-CfgKey $hostCfg 'ofa_grid' ([string]$c.Grid)
    Set-CfgKey $hostCfg 'ofa_perf' ([string]$c.Perf)
    $center = if ($c.ContainsKey('Center')) { $c.Center -split ',' } else { @('80.5', '80.5') }
    Set-IniKey $hostIni 'OptimizerFPS' 'CenterX' $center[0]
    Set-IniKey $hostIni 'OptimizerFPS' 'CenterY' $center[1]
    Set-IniKey $hostIni 'OptimizerFPS' 'ColorFilter' ([string]$(if ($c.ContainsKey('Filter')) { $c.Filter } else { 0 }))
    # K configurations run in 4K, borderless on the second monitor, with a walking-pace camera.
    $benchArgs = if ($c.ContainsKey('K')) { @('--size', '3840x2160', '--popup', '--pos', '3840,0', '--speed', '0.4') } else { @() }
    Remove-Item -LiteralPath (Join-Path $run 'host64\optimizer-fps-dlss5.session') -ErrorAction SilentlyContinue
    $out = Join-Path $capturesRoot $c.Label
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Push-Location $run
    try {
        $p = Start-Process -FilePath (Join-Path $run 'pw_bench9.exe') -ArgumentList (@('--frames', $Frames, '--dump', $Dumps, '--dump-dir', $out) + $benchArgs) -PassThru -Wait -WindowStyle Hidden
    } finally { Pop-Location }
    Copy-Item -LiteralPath (Join-Path $run 'host64\ReShade.log') -Destination (Join-Path $out 'host_ReShade.log') -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath (Join-Path $run 'host64\dlss5-feed-host.log') -Destination (Join-Path $out 'feed_host.log') -ErrorAction SilentlyContinue
    # The game side's frame interval ("600 frames: ... frame interval") - the cost the player sees.
    Copy-Item -LiteralPath (Join-Path $run 'dlss5-feed.log') -Destination (Join-Path $out 'feed32.log') -ErrorAction SilentlyContinue
    Write-Host ('{0,-8} exit {1}' -f $c.Label, $p.ExitCode)
    Start-Sleep -Seconds 2 # let the host process exit before the next run
}
