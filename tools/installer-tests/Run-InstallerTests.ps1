#Requires -Version 5.1
<#
.SYNOPSIS
    Self-tests for Install-OptimizerFPS.ps1 / Verify-OptimizerFPS.ps1.

.DESCRIPTION
    Copyright (c) 2026 Yuri Grib (BeliyG3). MIT licence (see LICENSE).

    Builds throw-away game folders under %TEMP% out of real binaries (a real ReShade 6.8
    add-on build, a real older optimizer-fps-dlss5.addon64, copies of cmd.exe standing in for
    the game and for dlss5-feed-host64.exe) and drives the installer through every path
    that has an exit code of its own.

    Nothing outside the temp folders is touched. Pass -Keep to leave them behind.

    The real ReShade DLL and the older add-on build come from a local bench folder that is not
    part of this repository: pass -BenchDir, or set %PW_BENCH_RUN%. Without it the scenarios
    that need them are skipped, not failed.

.EXAMPLE
    powershell.exe -NoProfile -File tools\installer-tests\Run-InstallerTests.ps1
#>
[CmdletBinding()]
param(
    [string] $PsExe,
    [string] $Payload,
    [string] $BenchDir = $env:PW_BENCH_RUN,
    [string] $ReShade64,
    [string] $ReShade32,
    [string] $OldAddon64,
    [switch] $Keep
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repoRoot  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$installer = Join-Path $repoRoot 'tools\Install-OptimizerFPS.ps1'
$verifier  = Join-Path $repoRoot 'tools\Verify-OptimizerFPS.ps1'

if (-not $PsExe) { $PsExe = 'powershell.exe' }
if (-not $Payload) {
    $hit = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'dist-release') -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1)
    if ($hit.Count -gt 0) { $Payload = Join-Path $hit[0].FullName 'payload' }
}

if ($BenchDir) {
    if (-not $ReShade64)  { $ReShade64  = Join-Path $BenchDir 'dxgi.dll' }
    if (-not $OldAddon64) { $OldAddon64 = Join-Path $BenchDir 'peripheral-warp.addon64.pre2625' }
}
if (-not $ReShade32)  {
    $hit = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'out') -Filter 'ReShade32.dll' -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($hit.Count -gt 0) { $ReShade32 = $hit[0].FullName }
}

$cmd64 = Join-Path $env:WINDIR 'System32\cmd.exe'
$cmd32 = Join-Path $env:WINDIR 'SysWOW64\cmd.exe'

$root = Join-Path ([IO.Path]::GetTempPath()) ('optimizerfps-tests-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
$null = New-Item -ItemType Directory -Path $root -Force

$script:Pass = 0
$script:Fail = 0
$script:Skip = 0
$script:Rows = New-Object System.Collections.ArrayList

function Say { param([string] $T, [string] $C = 'Gray') Write-Host $T -ForegroundColor $C }

function Check
{
    param([string] $Name, [bool] $Ok, [string] $Detail)
    if ($Ok) { $script:Pass++; Say ('  [PASS] ' + $Name) 'Green' }
    else     { $script:Fail++; Say ('  [FAIL] ' + $Name) 'Red'; if ($Detail) { Say ('         ' + $Detail) 'DarkGray' } }
    $null = $script:Rows.Add([pscustomobject]@{ Test = $Name; Result = $(if ($Ok) { 'PASS' } else { 'FAIL' }); Detail = $Detail })
}

function Skip
{
    param([string] $Name, [string] $Why)
    $script:Skip++
    Say ('  [SKIP] ' + $Name + ' -- ' + $Why) 'Yellow'
    $null = $script:Rows.Add([pscustomobject]@{ Test = $Name; Result = 'SKIP'; Detail = $Why })
}

function Invoke-Installer
{
    param([string[]] $Arguments)
    $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installer) + $Arguments
    $out = & $PsExe @all 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($null -eq $code) { $code = 0 }
    return [pscustomobject]@{ Code = $code; Out = $out }
}

function Invoke-Verifier
{
    param([string[]] $Arguments)
    $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $verifier) + $Arguments
    $out = & $PsExe @all 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($null -eq $code) { $code = 0 }
    return [pscustomobject]@{ Code = $code; Out = $out }
}

# A hash list of a folder, so "the uninstall put it back exactly" is a real assertion.
function Get-TreeSnapshot
{
    param([string] $Root, [string[]] $Exclude = @())
    $prefix = (Resolve-Path -LiteralPath $Root).ProviderPath
    if (-not $prefix.EndsWith('\')) { $prefix += '\' }
    $lines = New-Object System.Collections.ArrayList
    foreach ($f in @(Get-ChildItem -LiteralPath $Root -File -Recurse -Force -ErrorAction SilentlyContinue)) {
        $rel = $f.FullName.Substring($prefix.Length).Replace('\', '/')
        $skip = $false
        foreach ($x in $Exclude) { if ($rel -like $x) { $skip = $true; break } }
        if ($skip) { continue }
        $null = $lines.Add($rel.ToLowerInvariant() + '  ' + (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash)
    }
    return (@($lines.ToArray() | Sort-Object) -join "`n")
}

$MinimalIni = @'
[ADDON]
DisabledAddons=

[GENERAL]
EffectSearchPaths=.\

'@

function New-X64Fixture
{
    param([string] $Name, [string] $IniText)
    $dir = Join-Path $root $Name
    $null = New-Item -ItemType Directory -Path $dir -Force
    Copy-Item -LiteralPath $cmd64 -Destination (Join-Path $dir 'pwgame.exe') -Force
    Copy-Item -LiteralPath $ReShade64 -Destination (Join-Path $dir 'dxgi.dll') -Force
    $t = $IniText
    if (-not $t) { $t = $MinimalIni }
    [IO.File]::WriteAllText((Join-Path $dir 'ReShade.ini'), ($t -replace "`r`n", "`n" -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $dir
}

function New-X86Fixture
{
    param([string] $Name)
    $dir = Join-Path $root $Name
    $null = New-Item -ItemType Directory -Path $dir -Force
    Copy-Item -LiteralPath $cmd32 -Destination (Join-Path $dir 'game.exe') -Force
    Copy-Item -LiteralPath $ReShade32 -Destination (Join-Path $dir 'dxgi.dll') -Force
    [IO.File]::WriteAllText((Join-Path $dir 'ReShade.ini'), ($MinimalIni -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
    # A Feeder install: the 32-bit side has the feeder add-on, the 64-bit host has ReShade.
    [IO.File]::WriteAllText((Join-Path $dir 'dlss5-feed.addon32'), 'stub', (New-Object Text.UTF8Encoding($false)))
    $h = Join-Path $dir 'host64'
    $null = New-Item -ItemType Directory -Path $h -Force
    Copy-Item -LiteralPath $cmd64 -Destination (Join-Path $h 'dlss5-feed-host64.exe') -Force
    Copy-Item -LiteralPath $ReShade64 -Destination (Join-Path $h 'dxgi.dll') -Force
    [IO.File]::WriteAllText((Join-Path $h 'ReShade.ini'), ($MinimalIni -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $dir
}

function Get-PayloadShaderNames
{
    $names = @()
    $mf = Join-Path $Payload 'files.sha256'
    foreach ($l in ([IO.File]::ReadAllText($mf) -split "`r?`n")) {
        $m = [regex]::Match($l.Trim(), '^[0-9a-f]{64}\s+x64/optimizer-fps-dlss5/(.+\.dxbc)$')
        if ($m.Success) { $names += $m.Groups[1].Value }
    }
    return $names
}

# ---------------------------------------------------------------------------------------

Say ''
Say ('Optimizer FPS installer tests -- host: ' + $PsExe + ' (' + $PSVersionTable.PSVersion + ')') 'White'
Say ('Payload:  ' + $Payload) 'DarkGray'
Say ('Sandbox:  ' + $root) 'DarkGray'

$fatal = @()
if (-not (Test-Path -LiteralPath $installer)) { $fatal += 'Install-OptimizerFPS.ps1 not found' }
if (-not $Payload -or -not (Test-Path -LiteralPath (Join-Path $Payload 'files.sha256'))) { $fatal += 'no payload (run tools\Package-Release.ps1 first)' }
if ($fatal.Count -gt 0) {
    foreach ($f in $fatal) { Say ('  [FAIL] ' + $f) 'Red' }
    exit 1
}

# Every scenario builds a game folder around a real ReShade DLL, which is a local file this
# repository does not carry. Without it there is nothing to test, but nothing is broken either.
if (-not $ReShade64 -or -not (Test-Path -LiteralPath $ReShade64)) {
    Say ''
    foreach ($s in @(
        'x64 fresh install', 'Verify', 'update', 'uninstall', 'adoption of a manual install',
        '32-bit install', 'the game is running', 'corrupt payload', 'no ReShade beside the game',
        'a foreign file carrying one of our names', '-NoIni', '-Json', 'nothing to uninstall',
        'Verify reads a real ReShade.log', 'files from a pre-26.26 release')) {
        Skip $s 'no 64-bit ReShade fixture: pass -BenchDir <folder with dxgi.dll>, or set %PW_BENCH_RUN%'
    }
    Say ''
    Say ('Results: ' + $script:Pass + ' passed, ' + $script:Fail + ' failed, ' + $script:Skip + ' skipped.') 'White'
    if (-not $Keep) { try { Remove-Item -LiteralPath $root -Recurse -Force } catch { } }
    exit 0
}

$shaderNames = Get-PayloadShaderNames
Say ('Shaders:  ' + $shaderNames.Count) 'DarkGray'

# --- 1. x64 fresh install ---------------------------------------------------------------

Say ''
Say '== 1. x64 fresh install' 'Cyan'

$g1 = New-X64Fixture -Name 'x64-fresh'
$before1 = Get-TreeSnapshot -Root $g1
$exe1 = Join-Path $g1 'pwgame.exe'

$r = Invoke-Installer @('-GameExe', $exe1, '-Payload', $Payload, '-Yes', '-NoPause')
Check 'install exits 10 (installed, game not run yet)' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
Check 'optimizer-fps-dlss5.addon64 installed' (Test-Path -LiteralPath (Join-Path $g1 'optimizer-fps-dlss5.addon64'))
Check 'nvngx.dll_optimizerfps.dll installed' (Test-Path -LiteralPath (Join-Path $g1 'nvngx.dll_optimizerfps.dll'))
$got = @(Get-ChildItem -LiteralPath (Join-Path $g1 'optimizer-fps-dlss5') -File -Filter '*.dxbc' -ErrorAction SilentlyContinue).Count
Check ('all ' + $shaderNames.Count + ' shaders installed') ($got -eq $shaderNames.Count) ('found ' + $got)
Check 'latest-receipt.json written' (Test-Path -LiteralPath (Join-Path $g1 '_OptimizerFPS\latest-receipt.json'))

$ini1 = [IO.File]::ReadAllText((Join-Path $g1 'ReShade.ini'))
Check '[PeripheralWarp] Mode=2 written' ($ini1 -match '(?m)^\[PeripheralWarp\]' -and $ini1 -match '(?m)^Mode=2\r?$')
Check 'CenterX/CenterY=80, WorkX/WorkY=90 written' ($ini1 -match '(?m)^CenterX=80\r?$' -and $ini1 -match '(?m)^CenterY=80\r?$' -and $ini1 -match '(?m)^WorkX=90\r?$' -and $ini1 -match '(?m)^WorkY=90\r?$')

$receipt1 = (Get-Content -LiteralPath (Join-Path $g1 '_OptimizerFPS\latest-receipt.json') -Raw) | ConvertFrom-Json
Check 'receipt Schema 2' ([int]$receipt1.Schema -eq 2)
Check 'receipt lists every installed file' (@($receipt1.Files).Count -eq ($shaderNames.Count + 2)) ('files: ' + @($receipt1.Files).Count)

# --- 2. Verify --------------------------------------------------------------------------

Say ''
Say '== 2. Verify (static only, no log yet)' 'Cyan'
$r = Invoke-Verifier @('-GameExe', $exe1)
Check 'verify exits 10' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
$r = Invoke-Verifier @('-GameExe', $exe1, '-Json')
$vj = $null
try { $vj = $r.Out | ConvertFrom-Json } catch { }
Check 'verify -Json is parseable JSON' ($null -ne $vj) $r.Out
if ($vj) {
    Check 'verify JSON: no missing files' ([int]$vj.Static.MissingFiles -eq 0)
    Check 'verify JSON: shader count matches' ([int]$vj.Static.ShaderCount -eq $shaderNames.Count)
    Check 'verify JSON: forwarder exports ok' ([bool]$vj.Static.ForwarderExports)
}

# --- 3. Update --------------------------------------------------------------------------

Say ''
Say '== 3. Update over the same install' 'Cyan'
$iniHashBefore = (Get-FileHash -LiteralPath (Join-Path $g1 'ReShade.ini') -Algorithm SHA256).Hash
$r = Invoke-Installer @('-GameExe', $exe1, '-Payload', $Payload, '-Mode', 'Update', '-Yes', '-NoPause')
Check 'update exits 10' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
Check 'update reports the files as up to date' ($r.Out -match 'already up to date') $r.Out
$iniHashAfter = (Get-FileHash -LiteralPath (Join-Path $g1 'ReShade.ini') -Algorithm SHA256).Hash
Check 'update left ReShade.ini byte-identical' ($iniHashBefore -eq $iniHashAfter)

# --- 4. Uninstall -----------------------------------------------------------------------

Say ''
Say '== 4. Uninstall' 'Cyan'
$r = Invoke-Installer @('-GameExe', $exe1, '-Mode', 'Uninstall', '-Yes', '-NoPause')
Check 'uninstall exits 0' ($r.Code -eq 0) ('exit ' + $r.Code + "`n" + $r.Out)
$after1 = Get-TreeSnapshot -Root $g1 -Exclude @('_optimizerfps/*', '_OptimizerFPS/*')
Check 'the game folder is back to what it was' ($after1 -eq $before1) ("before:`n" + $before1 + "`nafter:`n" + $after1)
Check 'the backup folder is kept' ((@(Get-ChildItem -LiteralPath (Join-Path $g1 '_OptimizerFPS') -Directory -Filter 'backup-*' -ErrorAction SilentlyContinue)).Count -ge 1)

# --- 5. Install over a manual install ----------------------------------------------------

Say ''
Say '== 5. Adoption of an existing manual install' 'Cyan'

if (-not (Test-Path -LiteralPath $OldAddon64)) {
    Skip 'adoption of a manual install' ('older add-on build not found: ' + $OldAddon64)
}
else {
    $iniOld = $MinimalIni -replace 'DisabledAddons=', 'DisabledAddons=Optimizer FPS for DLSS5 26.15'
    $g2 = New-X64Fixture -Name 'x64-manual' -IniText $iniOld
    $exe2 = Join-Path $g2 'pwgame.exe'

    Copy-Item -LiteralPath $OldAddon64 -Destination (Join-Path $g2 'optimizer-fps-dlss5.addon64') -Force
    $sd = Join-Path $g2 'optimizer-fps-dlss5'
    $null = New-Item -ItemType Directory -Path $sd -Force
    $stale = 'zz_stale_test_ps.dxbc'
    foreach ($cand in @('temporal_zone_ps.dxbc', 'preview_ps.dxbc', 'zz_stale_test_ps.dxbc')) {
        if (-not ($shaderNames -contains $cand)) { $stale = $cand; break }
    }
    [IO.File]::WriteAllBytes((Join-Path $sd $stale), [byte[]] @(1, 2, 3, 4))
    [IO.File]::WriteAllText((Join-Path $g2 'optimizer-fps-dlss5.session'), 'stale marker', (New-Object Text.UTF8Encoding($false)))

    $r = Invoke-Installer @('-GameExe', $exe2, '-Payload', $Payload, '-Yes', '-NoPause')
    Check 'install over a manual install exits 10' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
    Check 'the older add-on was adopted, not refused' ($r.Out -match 'Adopting \d+ file') $r.Out
    Check ('the stale shader ' + $stale + ' was deleted') (-not (Test-Path -LiteralPath (Join-Path $sd $stale)))
    Check 'the crash-guard marker was removed' (-not (Test-Path -LiteralPath (Join-Path $g2 'optimizer-fps-dlss5.session')))

    $ini2 = [IO.File]::ReadAllText((Join-Path $g2 'ReShade.ini'))
    Check 'DisabledAddons migrated to the version-free name' ($ini2 -match '(?m)^DisabledAddons=Optimizer FPS for DLSS5\s*$') $ini2
    Check 'the user is told the add-on is disabled' ($r.Out -match 'disabled in ReShade') $r.Out

    $receipt2 = (Get-Content -LiteralPath (Join-Path $g2 '_OptimizerFPS\latest-receipt.json') -Raw) | ConvertFrom-Json
    $adoptedCount = @($receipt2.Files | Where-Object { $_.Adopted }).Count
    Check 'the receipt records the adoption' ($adoptedCount -ge 1) ('adopted: ' + $adoptedCount)
    Check 'the receipt records the DisabledAddons migration' (@($receipt2.DisabledAddonsMigrated).Count -ge 1)

    # The stale shader is ours by location but not part of the payload: it must be recoverable.
    $staleRec = @()
    if ($receipt2.PSObject.Properties['RemovedStaleShaders']) {
        $staleRec = @(@($receipt2.RemovedStaleShaders) | Where-Object { $_ -and ([IO.Path]::GetFileName([string]$_.Path) -ieq $stale) })
    }
    Check ('the receipt records the removed stale shader ' + $stale) ($staleRec.Count -eq 1) ('records: ' + $staleRec.Count)
    $staleBackupOk = $false
    $staleBackupDetail = 'no receipt entry'
    if ($staleRec.Count -eq 1) {
        $bp = [string]$staleRec[0].BackupPath
        $staleBackupDetail = $bp
        $staleBackupOk = (Test-Path -LiteralPath $bp) -and
                         ((Split-Path -Path (Split-Path -Path $bp -Parent) -Leaf) -ieq 'stale-shaders') -and
                         (((Get-Content -LiteralPath $bp -Encoding Byte) -join ',') -eq '1,2,3,4')
    }
    Check 'the stale shader was backed up before deletion' $staleBackupOk $staleBackupDetail
}

# --- 6. x86 -----------------------------------------------------------------------------

Say ''
Say '== 6. 32-bit game through host64' 'Cyan'

if (-not $ReShade32 -or -not (Test-Path -LiteralPath $ReShade32)) {
    Skip '32-bit install' 'no 32-bit ReShade fixture found'
}
elseif (-not (Test-Path -LiteralPath $cmd32)) {
    Skip '32-bit install' 'SysWOW64\cmd.exe not found'
}
else {
    $g3 = New-X86Fixture -Name 'x86-host64'
    $exe3 = Join-Path $g3 'game.exe'
    $r = Invoke-Installer @('-GameExe', $exe3, '-Payload', $Payload, '-Yes', '-NoPause')
    Check 'x86 install exits 10' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
    Check 'the add-on went into host64' (Test-Path -LiteralPath (Join-Path $g3 'host64\optimizer-fps-dlss5.addon64'))
    Check 'the forwarder went into host64' (Test-Path -LiteralPath (Join-Path $g3 'host64\nvngx.dll_optimizerfps.dll'))
    $got3 = @(Get-ChildItem -LiteralPath (Join-Path $g3 'host64\optimizer-fps-dlss5') -File -Filter '*.dxbc' -ErrorAction SilentlyContinue).Count
    Check 'the shaders went into host64' ($got3 -eq $shaderNames.Count) ('found ' + $got3)
    Check 'the remote tab went beside the 32-bit ReShade' (Test-Path -LiteralPath (Join-Path $g3 'optimizer-fps-dlss5-remote.addon32'))
    Check 'nothing of the x64 payload landed beside the game' (-not (Test-Path -LiteralPath (Join-Path $g3 'optimizer-fps-dlss5.addon64')))
    $ini3 = [IO.File]::ReadAllText((Join-Path $g3 'host64\ReShade.ini'))
    Check 'host64 ReShade.ini got [PeripheralWarp]' ($ini3 -match '(?m)^\[PeripheralWarp\]')

    # --- 7. the game is running --------------------------------------------------------
    Say ''
    Say '== 7. the game is running' 'Cyan'
    $proc = $null
    try {
        $proc = Start-Process -FilePath $exe3 -ArgumentList '/c', 'ping', '127.0.0.1', '-n', '30' -WindowStyle Hidden -PassThru
        Start-Sleep -Milliseconds 700
        $r = Invoke-Installer @('-GameExe', $exe3, '-Payload', $Payload, '-Yes', '-NoPause')
        Check 'install refuses while the game runs (exit 2)' ($r.Code -eq 2) ('exit ' + $r.Code + "`n" + $r.Out)
    }
    finally {
        if ($proc) { try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { } }
    }
}

# --- 8. corrupt payload -----------------------------------------------------------------

Say ''
Say '== 8. corrupt payload' 'Cyan'
$g4 = New-X64Fixture -Name 'x64-badpayload'
$exe4 = Join-Path $g4 'pwgame.exe'
$badPayload = Join-Path $root 'payload-corrupt'
Copy-Item -LiteralPath $Payload -Destination $badPayload -Recurse -Force
[IO.File]::WriteAllBytes((Join-Path $badPayload 'x64\optimizer-fps-dlss5.addon64'), [byte[]] @(0, 0, 0, 0))
$r = Invoke-Installer @('-GameExe', $exe4, '-Payload', $badPayload, '-Yes', '-NoPause')
Check 'corrupt payload exits 6' ($r.Code -eq 6) ('exit ' + $r.Code + "`n" + $r.Out)
Check 'nothing was installed from a corrupt payload' (-not (Test-Path -LiteralPath (Join-Path $g4 'optimizer-fps-dlss5.addon64')))

# --- 9. no ReShade ----------------------------------------------------------------------

Say ''
Say '== 9. no ReShade beside the game' 'Cyan'
$g5 = Join-Path $root 'x64-noreshade'
$null = New-Item -ItemType Directory -Path $g5 -Force
Copy-Item -LiteralPath $cmd64 -Destination (Join-Path $g5 'pwgame.exe') -Force
$r = Invoke-Installer @('-GameExe', (Join-Path $g5 'pwgame.exe'), '-Payload', $Payload, '-Yes', '-NoPause')
Check 'no ReShade exits 3' ($r.Code -eq 3) ('exit ' + $r.Code + "`n" + $r.Out)

# --- 10. a foreign file with our name ---------------------------------------------------

Say ''
Say '== 10. a foreign file carrying one of our names' 'Cyan'
$g6 = New-X64Fixture -Name 'x64-foreign'
$exe6 = Join-Path $g6 'pwgame.exe'
[IO.File]::WriteAllText((Join-Path $g6 'optimizer-fps-dlss5.addon64'), 'not ours at all', (New-Object Text.UTF8Encoding($false)))
$r = Invoke-Installer @('-GameExe', $exe6, '-Payload', $Payload, '-Yes', '-NoPause')
Check 'a foreign file stops the install (exit 1)' ($r.Code -eq 1) ('exit ' + $r.Code + "`n" + $r.Out)
Check 'the foreign file is left alone' ([IO.File]::ReadAllText((Join-Path $g6 'optimizer-fps-dlss5.addon64')) -eq 'not ours at all')
$r = Invoke-Installer @('-GameExe', $exe6, '-Payload', $Payload, '-Yes', '-NoPause', '-Force')
Check '-Force installs over it (exit 10)' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
$origs = @(Get-ChildItem -LiteralPath (Join-Path $g6 '_OptimizerFPS') -Recurse -File -Filter 'optimizer-fps-dlss5.addon64' -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -match 'originals' })
Check '-Force backed the foreign file up as an original' ($origs.Count -ge 1)

# --- 11. -NoIni -------------------------------------------------------------------------

Say ''
Say '== 11. -NoIni' 'Cyan'
$g7 = New-X64Fixture -Name 'x64-noini'
$exe7 = Join-Path $g7 'pwgame.exe'
$h7 = (Get-FileHash -LiteralPath (Join-Path $g7 'ReShade.ini') -Algorithm SHA256).Hash
$r = Invoke-Installer @('-GameExe', $exe7, '-Payload', $Payload, '-Yes', '-NoPause', '-NoIni')
Check '-NoIni still installs (exit 10)' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
Check '-NoIni left ReShade.ini untouched' ($h7 -eq (Get-FileHash -LiteralPath (Join-Path $g7 'ReShade.ini') -Algorithm SHA256).Hash)

# --- 12. -Json --------------------------------------------------------------------------

Say ''
Say '== 12. -Json' 'Cyan'
$g8 = New-X64Fixture -Name 'x64-json'
$exe8 = Join-Path $g8 'pwgame.exe'
$r = Invoke-Installer @('-GameExe', $exe8, '-Payload', $Payload, '-Yes', '-NoPause', '-Json')
$ij = $null
try { $ij = $r.Out | ConvertFrom-Json } catch { }
Check 'install -Json is parseable JSON' ($null -ne $ij) $r.Out
if ($ij) {
    Check 'install JSON: exit code, arch and file list' ([int]$ij.ExitCode -eq 10 -and [string]$ij.Arch -eq 'x64' -and @($ij.Files).Count -eq ($shaderNames.Count + 2))
    Check 'install JSON: 5 ini keys written' (@($ij.IniKeysWritten).Count -eq 5) ('keys: ' + @($ij.IniKeysWritten).Count)
}

# --- 13. Uninstall with nothing installed -----------------------------------------------

Say ''
Say '== 13. nothing to uninstall' 'Cyan'
$g9 = New-X64Fixture -Name 'x64-empty'
$r = Invoke-Installer @('-GameExe', (Join-Path $g9 'pwgame.exe'), '-Mode', 'Uninstall', '-Yes', '-NoPause')
Check 'nothing to uninstall exits 7' ($r.Code -eq 7) ('exit ' + $r.Code + "`n" + $r.Out)

# --- 14. Verify against a real ReShade.log ----------------------------------------------

Say ''
Say '== 14. Verify reads a real ReShade.log' 'Cyan'
# Captured ReShade.log lines, kept as fixtures: the live bench log is rewritten by every run.
$fixtures = Join-Path $PSScriptRoot 'fixtures'
$g10 = New-X64Fixture -Name 'x64-log'
$exe10 = Join-Path $g10 'pwgame.exe'
$null = Invoke-Installer @('-GameExe', $exe10, '-Payload', $Payload, '-Yes', '-NoPause', '-NoVerify')

foreach ($case in @(
    @{ File = 'ReShade-sample.log';  Label = 'adopted feature'; Feature = 'adopted';  Temporal = 'mode 1'; Background = 0 },
    @{ File = 'ReShade-created.log'; Label = 'created feature'; Feature = 'created';  Temporal = 'mode 3'; Background = 2 }
)) {
    $src = Join-Path $fixtures $case.File
    if (-not (Test-Path -LiteralPath $src)) { Skip ('runtime verdicts (' + $case.Label + ')') ('fixture missing: ' + $src); continue }
    Copy-Item -LiteralPath $src -Destination (Join-Path $g10 'ReShade.log') -Force
    (Get-Item -LiteralPath (Join-Path $g10 'ReShade.log')).LastWriteTimeUtc = (Get-Date).ToUniversalTime()

    $r = Invoke-Verifier @('-GameExe', $exe10, '-Json')
    $vj2 = $null
    try { $vj2 = $r.Out | ConvertFrom-Json } catch { }
    Check ($case.Label + ': verify -Json is parseable') ($null -ne $vj2) $r.Out
    if (-not $vj2) { continue }
    Check ($case.Label + ': the add-on registered') ([bool]$vj2.Runtime.Loaded)
    Check ($case.Label + ': the NGX hook is seen') ([bool]$vj2.Runtime.Hooked)
    Check ($case.Label + ': feature 18 is seen') ([string]$vj2.Runtime.Feature -match $case.Feature) ([string]$vj2.Runtime.Feature)
    Check ($case.Label + ': a warped frame is seen') ([bool]$vj2.Runtime.Warped)
    Check ($case.Label + ': the model-pixel percentage is right') `
        ($vj2.Runtime.PSObject.Properties['ModelPixelsPercent'] -and [math]::Abs([double]$vj2.Runtime.ModelPixelsPercent - 81.0) -lt 0.2) `
        ([string]$vj2.Runtime.ModelPixelsPercent)
    Check ($case.Label + ': the temporal mode is seen') ([string]$vj2.Runtime.Temporal -match $case.Temporal) ([string]$vj2.Runtime.Temporal)
    if ($case.Background -gt 0) {
        Check ($case.Label + ': background passes counted') ($vj2.Runtime.PSObject.Properties['BackgroundPasses'] -and [int]$vj2.Runtime.BackgroundPasses -eq $case.Background)
    }
    Check ($case.Label + ': verify exits 0') ([int]$vj2.ExitCode -eq 0) ('exit ' + $r.Code)
}

# A log older than the install must be called stale, not read.
Copy-Item -LiteralPath (Join-Path $fixtures 'ReShade-sample.log') -Destination (Join-Path $g10 'ReShade.log') -Force
(Get-Item -LiteralPath (Join-Path $g10 'ReShade.log')).LastWriteTimeUtc = (Get-Date).ToUniversalTime().AddDays(-2)
$r = Invoke-Verifier @('-GameExe', $exe10, '-Json')
$vj3 = $null
try { $vj3 = $r.Out | ConvertFrom-Json } catch { }
Check 'a log older than the install is treated as stale' ($null -ne $vj3 -and [bool]$vj3.Runtime.LogStale -and [int]$vj3.ExitCode -eq 10) ('exit ' + $r.Code)

# --- 15. Uninstall never overwrites a file the user changed ------------------------------

Say ''
Say '== 15. Uninstall and a file changed after the install' 'Cyan'

# -Force backs the foreign file up as an original, so this covers both halves of the bug:
# a changed file must be kept, and the backed-up original must not land on top of it.
$g11    = New-X64Fixture -Name 'x64-changed'
$exe11  = Join-Path $g11 'pwgame.exe'
$addon11 = Join-Path $g11 'optimizer-fps-dlss5.addon64'
[IO.File]::WriteAllText($addon11, 'not ours at all', (New-Object Text.UTF8Encoding($false)))
$null = Invoke-Installer @('-GameExe', $exe11, '-Payload', $Payload, '-Yes', '-NoPause', '-NoVerify', '-Force')
$installed11 = [IO.File]::ReadAllBytes($addon11)
[IO.File]::WriteAllBytes($addon11, ($installed11 + [byte[]] @(42)))
$r = Invoke-Installer @('-GameExe', $exe11, '-Mode', 'Uninstall', '-Yes', '-NoPause')
$now11 = $null
if (Test-Path -LiteralPath $addon11) { $now11 = [IO.File]::ReadAllBytes($addon11) }
Check 'a file changed after the install is kept, and no original is restored over it' `
    ($null -ne $now11 -and $now11.Length -eq ($installed11.Length + 1) -and $now11[$now11.Length - 1] -eq 42) `
    ('exit ' + $r.Code + '; size ' + $(if ($null -eq $now11) { 'deleted' } else { $now11.Length }) + ' vs ' + ($installed11.Length + 1) + "`n" + $r.Out)

$g12   = New-X64Fixture -Name 'x64-unchanged'
$exe12 = Join-Path $g12 'pwgame.exe'
$null = Invoke-Installer @('-GameExe', $exe12, '-Payload', $Payload, '-Yes', '-NoPause', '-NoVerify')
$r = Invoke-Installer @('-GameExe', $exe12, '-Mode', 'Uninstall', '-Yes', '-NoPause')
Check 'an untouched install is still uninstalled completely' `
    ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $g12 'optimizer-fps-dlss5.addon64')) -and
     -not (Test-Path -LiteralPath (Join-Path $g12 'optimizer-fps-dlss5'))) ('exit ' + $r.Code + "`n" + $r.Out)

# --- 16. An install left over from a release before the rename ---------------------------

Say ''
Say '== 16. Files from a pre-26.26 release are replaced by the new names' 'Cyan'

if (-not (Test-Path -LiteralPath $OldAddon64)) {
    Skip 'files from a pre-26.26 release' ('older add-on build not found: ' + $OldAddon64)
}
else {
    $g13   = New-X64Fixture -Name 'x64-legacy'
    $exe13 = Join-Path $g13 'pwgame.exe'

    $oldAddon     = Join-Path $g13 'peripheral-warp.addon64'
    $oldForwarder = Join-Path $g13 'nvngx.dll_peripheralwarp.dll'
    $oldShaderDir = Join-Path $g13 'peripheral-warp'
    $oldMarker    = Join-Path $g13 'peripheral-warp.session'

    Copy-Item -LiteralPath $OldAddon64 -Destination $oldAddon -Force
    Copy-Item -LiteralPath (Join-Path $Payload 'x64\nvngx.dll_optimizerfps.dll') -Destination $oldForwarder -Force
    $null = New-Item -ItemType Directory -Path $oldShaderDir -Force
    [IO.File]::WriteAllBytes((Join-Path $oldShaderDir 'pack_ps.dxbc'), [byte[]] @(1, 2, 3, 4))
    [IO.File]::WriteAllText($oldMarker, 'stale marker', (New-Object Text.UTF8Encoding($false)))

    $r = Invoke-Installer @('-GameExe', $exe13, '-Payload', $Payload, '-Mode', 'Update', '-Yes', '-NoPause')
    Check 'update over a pre-26.26 install exits 10' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
    Check 'the new add-on is there' (Test-Path -LiteralPath (Join-Path $g13 'optimizer-fps-dlss5.addon64'))
    Check 'the new forwarder is there' (Test-Path -LiteralPath (Join-Path $g13 'nvngx.dll_optimizerfps.dll'))
    $got13 = @(Get-ChildItem -LiteralPath (Join-Path $g13 'optimizer-fps-dlss5') -File -Filter '*.dxbc' -ErrorAction SilentlyContinue).Count
    Check 'the shaders went into optimizer-fps-dlss5\' ($got13 -eq $shaderNames.Count) ('found ' + $got13)
    Check 'the old add-on is gone' (-not (Test-Path -LiteralPath $oldAddon))
    Check 'the old forwarder is gone' (-not (Test-Path -LiteralPath $oldForwarder))
    Check 'the old shader folder is gone' (-not (Test-Path -LiteralPath $oldShaderDir))
    Check 'the old crash-guard marker is gone' (-not (Test-Path -LiteralPath $oldMarker))

    $receipt13 = (Get-Content -LiteralPath (Join-Path $g13 '_OptimizerFPS\latest-receipt.json') -Raw) | ConvertFrom-Json
    $legacyList = @()
    if ($receipt13.PSObject.Properties['RemovedLegacyFiles']) { $legacyList = @($receipt13.RemovedLegacyFiles) }
    Check 'the receipt lists the removed legacy files' ($legacyList.Count -eq 4) ('listed: ' + $legacyList.Count)
    $backedUp = @(Get-ChildItem -LiteralPath (Join-Path $g13 '_OptimizerFPS') -Recurse -File -Filter 'peripheral-warp.addon64' -ErrorAction SilentlyContinue |
                  Where-Object { $_.FullName -match '(?i)\\legacy\\' })
    Check 'the old add-on was backed up before it was deleted' ($backedUp.Count -ge 1)

    $r = Invoke-Verifier @('-GameExe', $exe13, '-Json')
    $vj13 = $null
    try { $vj13 = $r.Out | ConvertFrom-Json } catch { }
    Check 'verify sees no legacy files left' ($null -ne $vj13 -and @($vj13.Static.LegacyFiles).Count -eq 0) $r.Out

    # ... and it says so when one is put back by hand.
    Copy-Item -LiteralPath $OldAddon64 -Destination $oldAddon -Force
    $r = Invoke-Verifier @('-GameExe', $exe13)
    Check 'verify warns about a legacy file that is still there' ($r.Out -match 'legacy file\(s\) from an earlier release') $r.Out
}

# ---------------------------------------------------------------------------------------

Say ''
Say ('Results: ' + $script:Pass + ' passed, ' + $script:Fail + ' failed, ' + $script:Skip + ' skipped.') 'White'
$script:Rows | Where-Object { $_.Result -ne 'PASS' } | Format-Table -AutoSize | Out-String | Write-Host

if ($Keep) { Say ('Sandbox kept: ' + $root) 'DarkGray' }
else { try { Remove-Item -LiteralPath $root -Recurse -Force } catch { Say ('Could not clean up ' + $root) 'Yellow' } }

if ($script:Fail -gt 0) { exit 1 }
exit 0
