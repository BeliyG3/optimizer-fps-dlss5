#Requires -Version 5.1
param([hashtable] $Context)
$root = $Context.Root
$Payload = $Context.Payload
$OldAddon64 = $Context.OldAddon64
$ReShade64 = $Context.ReShade64
$shaderNames = $Context.ShaderNames

# --- 15. Uninstall never overwrites a file the user changed ------------------------------

Say ''
Say '== 15. Uninstall and a file changed after the install' 'Cyan'

# -Force backs the foreign file up as an original, so this covers both halves of the bug:
# a changed file must be kept, and the backed-up original must not land on top of it.
$g11    = New-X64Fixture -Root $root -ReShade64 $ReShade64 -Name 'x64-changed'
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

$g12   = New-X64Fixture -Root $root -ReShade64 $ReShade64 -Name 'x64-unchanged'
$exe12 = Join-Path $g12 'pwgame.exe'
$null = Invoke-Installer @('-GameExe', $exe12, '-Payload', $Payload, '-Yes', '-NoPause', '-NoVerify')
$r = Invoke-Installer @('-GameExe', $exe12, '-Mode', 'Uninstall', '-Yes', '-NoPause')
Check 'an untouched install is still uninstalled completely' `
    ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $g12 'optimizer-fps-dlss5.addon64')) -and
     -not (Test-Path -LiteralPath (Join-Path $g12 'optimizer-fps-dlss5'))) ('exit ' + $r.Code + "`n" + $r.Out)

# --- 16. An install left over from a release before the rename ---------------------------

Say ''
Say '== 16. Files from a pre-26.26 release are replaced by the new names' 'Cyan'

if (-not $OldAddon64 -or -not (Test-Path -LiteralPath $OldAddon64)) {
    Skip 'files from a pre-26.26 release' ('older add-on build not found: ' + $OldAddon64)
}
else {
    $g13   = New-X64Fixture -Root $root -ReShade64 $ReShade64 -Name 'x64-legacy'
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

# --- 17. The version the receipt reports -------------------------------------------------

Say ''
Say '== 17. payload\VERSION.txt reaches the receipt' 'Cyan'

# Schema 3 cannot claim a working core when VERSION.txt disagrees with its PE version.

$gv1   = New-X64Fixture -Root $root -ReShade64 $ReShade64 -Name 'x64-version'
$exev1 = Join-Path $gv1 'pwgame.exe'
$pv1   = New-PayloadWithVersion -Root $root -Payload $Payload -Name 'payload-version-plain' -Bytes ([Text.Encoding]::UTF8.GetBytes("26.99.1`n"))
$r = Invoke-Installer @('-GameExe', $exev1, '-Payload', $pv1, '-Yes', '-NoPause')
Check 'mismatched VERSION.txt fails (exit 1)' ($r.Code -eq 1) ('exit ' + $r.Code + "`n" + $r.Out)
Check 'mismatched VERSION.txt leaves no receipt' `
    (-not (Test-Path -LiteralPath (Join-Path $gv1 '_OptimizerFPS\latest-receipt.json')))

$gv2   = New-X64Fixture -Root $root -ReShade64 $ReShade64 -Name 'x64-version-missing'
$exev2 = Join-Path $gv2 'pwgame.exe'
$pv2   = New-PayloadWithVersion -Root $root -Payload $Payload -Name 'payload-version-missing' -Drop
$r = Invoke-Installer @('-GameExe', $exev2, '-Payload', $pv2, '-Yes', '-NoPause')
Check 'missing VERSION.txt fails (exit 1)' ($r.Code -eq 1) ('exit ' + $r.Code + "`n" + $r.Out)
Check 'missing VERSION.txt leaves no receipt' `
    (-not (Test-Path -LiteralPath (Join-Path $gv2 '_OptimizerFPS\latest-receipt.json')))

# Package-Release.ps1 writes VERSION.txt as UTF-8 without a BOM and LF-terminated, but a
# version corrected by hand comes back out of Notepad with a BOM and CRLF, and neither of
# those bytes is part of the version.
$gv3   = New-X64Fixture -Root $root -ReShade64 $ReShade64 -Name 'x64-version-bom'
$exev3 = Join-Path $gv3 'pwgame.exe'
$pv3   = New-PayloadWithVersion -Root $root -Payload $Payload -Name 'payload-version-bom' `
             -Bytes ([byte[]] (@(0xEF, 0xBB, 0xBF) + [Text.Encoding]::UTF8.GetBytes($testContext.ReleaseVersion + "`r`n")))
$r = Invoke-Installer @('-GameExe', $exev3, '-Payload', $pv3, '-Yes', '-NoPause')
Check 'install from a BOM-prefixed VERSION.txt exits 10' ($r.Code -eq 10) ('exit ' + $r.Code + "`n" + $r.Out)
$rv3 = (Get-Content -LiteralPath (Join-Path $gv3 '_OptimizerFPS\latest-receipt.json') -Raw) | ConvertFrom-Json
Check 'neither the BOM nor the CRLF reaches the receipt' ([string]::Equals([string]$rv3.Version, $testContext.ReleaseVersion, [StringComparison]::Ordinal)) (Format-VersionDetail ([string]$rv3.Version))

# Verify has a reader of its own for the case where the receipt carries no version, and that
# one has to strip the same bytes: with no receipt there is nothing left to correct it.
Remove-Item -LiteralPath (Join-Path $gv3 '_OptimizerFPS\latest-receipt.json') -Force
$r = Invoke-Verifier @('-GameExe', $exev3, '-Payload', $pv3, '-Json')
$vjv3 = $null
try { $vjv3 = $r.Out | ConvertFrom-Json } catch { }
Check 'verify falls back to payload\VERSION.txt without carrying the BOM over' `
    ($null -ne $vjv3 -and [string]::Equals([string]$vjv3.Version, $testContext.ReleaseVersion, [StringComparison]::Ordinal)) `
    ($(if ($null -eq $vjv3) { $r.Out } else { Format-VersionDetail ([string]$vjv3.Version) }))
