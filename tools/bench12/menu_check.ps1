#Requires -Version 5.1
# Menu-mode spike checks for reference_dumps.ps1 (dot-sourced). The add-on writes menu_events.log
# (enter/exit/dump/fail/stop/missed/cpu/gpu lines) and menu_dump_<presentIndex>.bmp next to the bench exe.

function Clear-MenuOutput([string] $Run) {
    Remove-Item -LiteralPath (Join-Path $Run 'menu_events.log') -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $Run -Filter 'menu_dump_*.bmp' | Remove-Item
}

# The present indices the listed cases dumped (dump and probe lines of their menu_events.log in $Out), as
# one ascending union. Cases not run in this invocation are skipped; none at all is an error.
function Get-MenuDumpIndices([string] $Out, [string[]] $Cases) {
    $logs = @($Cases | ForEach-Object { Join-Path $Out "$_`_menu_events.log" } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    if ($logs.Count -eq 0) { throw "No menu_events.log of $($Cases -join ', ') in $Out; run one of them earlier in the same invocation" }
    $indices = @($logs | ForEach-Object { [IO.File]::ReadAllLines($_) } | Where-Object { $_ -match '^(dump|probe) \d+' } |
        ForEach-Object { [int](($_ -split ' ')[1]) } | Sort-Object -Unique | ForEach-Object { [string]$_ })
    if ($indices.Count -eq 0) { throw "$($logs -join ', ') have no dump lines" }
    return $indices
}

# Menu runs as half-open present ranges: each enter closes at the next exit/release/resize/drain/stop/left line.
function Get-MenuRuns([string[]] $Lines) {
    $runs = New-Object System.Collections.Generic.List[object]
    $open = -1
    foreach ($line in $Lines) {
        $f = $line -split ' '
        if ($f[0] -eq 'enter') { $open = [int]$f[1] }
        elseif ($open -ge 0 -and @('exit', 'release', 'resize', 'drain', 'stop', 'left') -contains $f[0]) { $runs.Add(@($open, [int]$f[1])); $open = -1 }
    }
    if ($open -ge 0) { $runs.Add(@($open, [int]::MaxValue)) }
    return , $runs.ToArray()
}

# True when the 64x64 marker square at (16,16) is opaque red at its centre (48,48) in a 32-bit BMP.
function Test-MenuMarker([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $offset = [BitConverter]::ToInt32($bytes, 10)
    $width = [BitConverter]::ToInt32($bytes, 18)
    $height = [BitConverter]::ToInt32($bytes, 22)
    $row = if ($height -lt 0) { 48 } else { $height - 1 - 48 }       # top-down (negative height) or bottom-up
    $i = $offset + ($row * $width + 48) * 4                          # BGRA
    return ($bytes[$i + 2] -ge 250) -and ($bytes[$i + 1] -le 5) -and ($bytes[$i] -le 5)
}

function Test-SameFile([string] $A, [string] $B) {
    return (Test-Path -LiteralPath $A) -and (Test-Path -LiteralPath $B) -and
        ((Get-FileHash -LiteralPath $A).Hash -eq (Get-FileHash -LiteralPath $B).Hash)
}

# Task 13 fix round 1 (C5): the run end on the first line matching $EndPattern wrote 'flow off' (the core's motion source
# back to the game's vectors), and the next host evaluate after it reached the core with them ('game <present> vectors').
function Test-FlowRestoredAfter([string[]] $Lines, [string] $EndPattern) {
    $at = -1
    for ($i = 0; $i -lt $Lines.Count; $i++) { if ($Lines[$i] -match $EndPattern) { $at = $i; break } }
    if ($at -lt 0 -or $at + 1 -ge $Lines.Count -or $Lines[$at + 1] -notmatch '^flow off ') { return $false }
    for ($i = $at + 2; $i -lt $Lines.Count; $i++) { if ($Lines[$i] -match '^game \d+ (\S+)') { return $Matches[1] -eq 'vectors' } }
    return $false
}

# Task 13 fix round 1 (Codex I2), DebugMenuNoFlow=2/3: the flow fails once on a carried frame of the first run. $Event
# 'full': that frame ran again as a full model frame ('redo 1'); 'last': the last output was shown again ('redo 0'). The
# first run's in-menu dump is that very frame (its 't' line follows the 'redo' line). $Runs 10 (Mode Off: every run
# ends 'exit', no core frame after the first run: the direct pass) or 1 (a compressed model: the run ends 'left' on the
# next present and no run enters again, the flow blocker in the tab).
function Test-MenuFlowFailure([string[]] $Lines, [string] $Event, [int] $Runs) {
    $redo = @(for ($i = 0; $i -lt $Lines.Count; $i++) { if ($Lines[$i] -match '^redo (\d)') { $i } })
    $want = if ($Event -eq 'full') { '1' } else { '0' }
    $shown = -1
    if ($redo.Count -eq 1) {
        for ($i = $redo[0] + 1; $i -lt $Lines.Count; $i++) { if ($Lines[$i] -match '^t (\d+) ') { $shown = [int]$Matches[1]; break } }
    }
    $firstDump = @($Lines | Where-Object { $_ -match '^dump \d+' } | ForEach-Object { [int](($_ -split ' ')[1]) } | Select-Object -First 1)
    $menuRuns = Get-MenuRuns $Lines # not $runs: PowerShell names ignore case, that is the [int] $Runs parameter
    $firstEnd = if ($menuRuns.Count) { $menuRuns[0][1] } else { -1 }
    $endLine = -1 # the first run's end line; no core frame after it (the cadence is off from the next present on)
    for ($i = 0; $i -lt $Lines.Count; $i++) { if ($Lines[$i] -match '^(exit|release|resize|drain|stop|left) ') { $endLine = $i; break } }
    $coreAfter = $endLine -ge 0 -and @($Lines[($endLine + 1)..($Lines.Count - 1)] | Where-Object { $_ -like 'core *' }).Count -gt 0
    $ended = if ($Runs -eq 1) { @($Lines | Where-Object { $_ -match "^left $firstEnd " }).Count -eq 1 -and $menuRuns.Count -eq 1 } else { $menuRuns.Count -eq $Runs }
    $ok = $redo.Count -eq 1 -and $Lines[$redo[0]] -eq "redo $want" -and $firstDump.Count -eq 1 -and $shown -eq $firstDump[0] -and $ended -and -not $coreAfter
    return [ordered]@{ redo = $(if ($redo.Count) { $Lines[$redo[0]] } else { '' }); redo_count = $redo.Count; shown_at = $shown
                       first_dump = $(if ($firstDump.Count) { $firstDump[0] } else { -1 }); runs = $menuRuns.Count; core_after = $coreAfter; ok = $ok }
}

# Plan correction C7, on a case's output already in $Out ($Lines: its menu_events.log). $Event 'recreate', 'resize',
# 'race', 'setting' or 'teardown' (C5: the swap chain destroyed and created again)
# happened at present $EventAt: the bench logged it at that frame ([nr] ... feature 18 released / [info] ... swap chain
# resized) and the add-on saw it (recreate: the run ends with 'release <EventAt>'; resize: ReShade logged ResizeBuffers).
# spanning: a menu run was under way (entered before $EventAt, ended at or after it). later_passes: runs entered after
# $EventAt that each have their own recorded pass: an in-menu dump inside the run whose present has a 't' line with a
# nonzero evaluate recording (a refused pass also writes a 'fail' line, which fails the case).
function Test-MenuLifetime([string] $Out, [string] $Name, [string[]] $Lines, [int] $EventAt, [string] $Event) {
    $stdout = Join-Path $Out "$Name.txt"
    $benchPattern = @{ recreate = "^\[nr\] frame $EventAt`: feature 18 released"; race = "^\[nr\] frame $EventAt`: feature 18 released on a second thread"
                       resize = "^\[info\] frame $EventAt`: swap chain resized"; setting = "^\[set\] frame $EventAt`: setting .* \(status 0\)"
                       teardown = "^\[info\] frame $EventAt`: swap chain recreated" }[$Event]
    $benchSeen = $EventAt -ge 0 -and $benchPattern -and (Test-Path -LiteralPath $stdout) -and @(Select-String -LiteralPath $stdout -Pattern $benchPattern).Count -gt 0
    $reshadeLog = Join-Path $Out "$Name`_ReShade.log"
    $addonSeen = switch ($Event) {
        'recreate' { @($Lines | Where-Object { $_ -match "^release $EventAt " }).Count -gt 0 }
        # --nr-release-thread: the release lands during present EventAt (logged with the next index) or just before it.
        'race' { @($Lines | Where-Object { $_ -match "^release ($EventAt|$($EventAt + 1)) " }).Count -gt 0 }
        # --set-at: the run ends on the next present with the settings blocker in the tab (fix round 1 of Task 12).
        'setting' { @($Lines | Where-Object { $_ -match "^left ($EventAt|$($EventAt + 1)) " }).Count -gt 0 -and (Test-Path -LiteralPath $reshadeLog) -and
                    @(Select-String -LiteralPath $reshadeLog -Pattern 'a setting changed in this menu' -SimpleMatch).Count -gt 0 }
        'resize' { (Test-Path -LiteralPath $reshadeLog) -and @(Select-String -LiteralPath $reshadeLog -Pattern 'IDXGISwapChain::ResizeBuffers' -SimpleMatch).Count -gt 0 }
        # --recreate-swapchain: destroy_swapchain drains the pipeline; the run under way ends with 'drain <EventAt>', whose
        # end switched the core back to the game's vectors before the game's next evaluate reached the core (fix round 1).
        'teardown' { (Test-FlowRestoredAfter $Lines "^drain $EventAt ") }
        default { $false }
    }
    $runs = Get-MenuRuns $Lines
    $spanning = @($runs | Where-Object { $_[0] -lt $EventAt -and $_[1] -ge $EventAt }).Count -gt 0
    $dumps = @($Lines | Where-Object { $_ -match '^dump \d+' } | ForEach-Object { [int](($_ -split ' ')[1]) })
    $evaluated = @{}
    foreach ($line in $Lines) { if ($line -match '^t (\d+) \S+ (\S+) ' -and [double]$Matches[2] -gt 0) { $evaluated[[int]$Matches[1]] = $true } }
    $later = @($runs | Where-Object { $_[0] -gt $EventAt })
    $passed = 0
    foreach ($r in $later) {
        $own = @($dumps | Where-Object { $_ -ge $r[0] -and $_ -lt $r[1] -and $evaluated.ContainsKey($_) -and
                                         (Test-Path -LiteralPath (Join-Path $Out ("{0}_menu_dump_{1}.bmp" -f $Name, $_))) })
        if ($own.Count -gt 0) { $passed++ }
    }
    return [ordered]@{ event_logged = [bool]($benchSeen -and $addonSeen); spanning = $spanning; later_runs = $later.Count; later_passes = $passed }
}

# Final review C1: menu mode was turned off during gameplay, the watched feature's model re-created meanwhile (a Mode
# round trip), and menu mode turned on again at present $EventAt inside an open menu (the bench's '[set] frame
# $EventAt: setting 45 = 1'). The snapshot of the old model must not be used: no run enters in the rest of that menu
# (30 presents, the bench's pause length); later runs ($Runs or more) enter after a fresh host evaluate, each with its
# own pass (as Test-MenuLifetime counts them).
function Test-MenuReenable([string] $Out, [string] $Name, [string[]] $Lines, [int] $EventAt) {
    $stdout = Join-Path $Out "$Name.txt"
    $benchSeen = (Test-Path -LiteralPath $stdout) -and
        @(Select-String -LiteralPath $stdout -Pattern "^\[set\] frame $EventAt`: setting 45 = 1 \(status 0\)").Count -gt 0
    $runs = Get-MenuRuns $Lines
    $staleRuns = @($runs | Where-Object { $_[0] -ge $EventAt -and $_[0] -lt $EventAt + 30 }).Count
    $dumps = @($Lines | Where-Object { $_ -match '^dump \d+' } | ForEach-Object { [int](($_ -split ' ')[1]) })
    $evaluated = @{}
    foreach ($line in $Lines) { if ($line -match '^t (\d+) \S+ (\S+) ' -and [double]$Matches[2] -gt 0) { $evaluated[[int]$Matches[1]] = $true } }
    $later = @($runs | Where-Object { $_[0] -ge $EventAt + 30 })
    $passed = 0
    foreach ($r in $later) {
        $own = @($dumps | Where-Object { $_ -ge $r[0] -and $_ -lt $r[1] -and $evaluated.ContainsKey($_) -and
                                         (Test-Path -LiteralPath (Join-Path $Out ("{0}_menu_dump_{1}.bmp" -f $Name, $_))) })
        if ($own.Count -gt 0) { $passed++ }
    }
    return [ordered]@{ event_logged = [bool]$benchSeen; stale_runs = $staleRuns; later_runs = $later.Count; later_passes = $passed }
}

# Moves the add-on's menu output into $Out and measures it. $Expect: 'marker' (red square inside every
# menu run, absent after every exit), 'none' (never red; a forced-failure line is required), 'model'
# ($Runs enter/exit pairs, every entry >= $MinEnterMs after the last host evaluate (DebugMenuEntryMs, 150 by
# default); PSNR by menu_psnr.py), 'sigfail' (a stop line; the dump of the stopped present equals the bench's
# own dump: the frame untouched), 'lifetime' (Test-MenuLifetime: $Event at $EventAt inside a run, no stop, failure or
# missed dump, and at least $Runs later runs, every one with its own pass) or 'flowfail' (Test-MenuFlowFailure). Every
# case: no 'flow owed' / 'game <present> withheld' line (the optical flow always switched back at the run's end).
function Get-MenuCheck([string] $Run, [string] $Out, [string] $Name, [string] $Expect, [string] $ProbeFrame, [int] $Runs = 0, [int] $MinEnterMs = 150, [int] $EventAt = -1, [string] $Event = '') {
    $log = Join-Path $Run 'menu_events.log'
    $lines = @()
    if (Test-Path -LiteralPath $log -PathType Leaf) {
        Move-Item -LiteralPath $log -Destination (Join-Path $Out "$Name`_menu_events.log") -Force
        $lines = @([IO.File]::ReadAllLines((Join-Path $Out "$Name`_menu_events.log")))
    }
    foreach ($bmp in Get-ChildItem -LiteralPath $Run -Filter 'menu_dump_*.bmp') {
        Move-Item -LiteralPath $bmp.FullName -Destination (Join-Path $Out ("{0}_{1}" -f $Name, $bmp.Name)) -Force
    }
    $enterLines = @($lines | Where-Object { $_ -like 'enter *' })
    $exits = @($lines | Where-Object { $_ -like 'exit *' }).Count
    $dumped = @($lines | Where-Object { $_ -like 'dump *' } | ForEach-Object { [int](($_ -split ' ')[1]) })
    $stops = @($lines | Where-Object { $_ -like 'stop *' } | ForEach-Object { [int](($_ -split ' ')[1]) })
    $failed = @($lines | Where-Object { $_ -like 'fail *' }).Count -gt 0
    $missedCount = @($lines | Where-Object { $_ -like 'missed *' }).Count
    $minEnterMs = if ($enterLines.Count) { ($enterLines | ForEach-Object { [double](($_ -split ' ')[2]) } | Measure-Object -Minimum).Minimum } else { 0 }
    $menuRuns = Get-MenuRuns $lines
    $inMenu = 0; $inMenuRed = 0; $after = 0; $afterRed = 0; $missing = 0
    foreach ($index in $dumped) {
        $path = Join-Path $Out ("{0}_menu_dump_{1}.bmp" -f $Name, $index)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $missing++; continue }
        $menu = @($menuRuns | Where-Object { $index -ge $_[0] -and $index -lt $_[1] }).Count -gt 0
        $red = Test-MenuMarker $path
        if ($menu) { $inMenu++; if ($red) { $inMenuRed++ } } else { $after++; if ($red) { $afterRed++ } }
    }
    $life = if ($Expect -eq 'lifetime') { Test-MenuLifetime $Out $Name $lines $EventAt $Event }
            elseif ($Expect -eq 'reenable') { Test-MenuReenable $Out $Name $lines $EventAt } else { $null }
    $flowFail = if ($Expect -eq 'flowfail') { Test-MenuFlowFailure $lines $Event $Runs } else { $null }
    # Fix round 1 (C5): no run end left the menu's optical flow owed, no game evaluate was withheld for it.
    $flowOwed = @($lines | Where-Object { $_ -like 'flow owed *' -or $_ -match '^game \d+ withheld' }).Count
    $probeSame = Test-SameFile (Join-Path $Out ("{0}_menu_dump_probe_{1}.bmp" -f $Name, $ProbeFrame)) (Join-Path $Out ("{0}_dump_{1}.bmp" -f $Name, $ProbeFrame))
    $stopUntouched = $stops.Count -gt 0 -and
        (Test-SameFile (Join-Path $Out ("{0}_menu_dump_{1}.bmp" -f $Name, $stops[0])) (Join-Path $Out ("{0}_dump_{1}.bmp" -f $Name, $stops[0])))
    $ok = $enterLines.Count -gt 0 -and $dumped.Count -gt 0 -and $missing -eq 0 -and $probeSame -and $flowOwed -eq 0
    switch ($Expect) {
        'flowfail' { $ok = $ok -and $stops.Count -eq 0 -and -not $failed -and $missedCount -eq 0 -and $flowFail.ok }
        'marker' { $ok = $ok -and $inMenuRed -eq $inMenu -and $afterRed -eq 0 }
        'none' { $ok = $ok -and $inMenuRed -eq 0 -and $afterRed -eq 0 -and $failed }
        'model' { $ok = $ok -and $stops.Count -eq 0 -and -not $failed -and $missedCount -eq 0 -and $minEnterMs -ge $MinEnterMs -and ($Runs -eq 0 -or ($enterLines.Count -eq $Runs -and $exits -eq $Runs)) }
        'sigfail' { $ok = $ok -and $stops.Count -eq 1 -and $stopUntouched -and @($enterLines | Where-Object { [int](($_ -split ' ')[1]) -gt $stops[0] }).Count -eq 0 }
        'lifetime' { $ok = $ok -and $stops.Count -eq 0 -and -not $failed -and $missedCount -eq 0 -and $life.event_logged -and $life.spanning -and
                           $life.later_runs -ge $Runs -and $life.later_passes -eq $life.later_runs }
        'reenable' { $ok = $ok -and $stops.Count -eq 0 -and -not $failed -and $missedCount -eq 0 -and $life.event_logged -and $life.stale_runs -eq 0 -and
                           $life.later_runs -ge $Runs -and $life.later_passes -eq $life.later_runs }
    }
    return [ordered]@{ enter = $enterLines.Count; exit = $exits; dumps = $dumped.Count; missing = $missing
                       in_menu = $inMenu; in_menu_red = $inMenuRed; after_exit = $after; after_exit_red = $afterRed
                       forced_failure = $failed; probe_identical = $probeSame; min_enter_ms = $minEnterMs
                       stop = $(if ($stops.Count) { $stops[0] } else { -1 }); stop_untouched = $stopUntouched; life = $life
                       flow_owed = $flowOwed; flow_failure = $flowFail; ok = $ok }
}
