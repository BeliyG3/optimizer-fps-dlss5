#Requires -Version 5.1
Import-Module -Name (Join-Path $PSScriptRoot 'Common.psm1') -Force -ErrorAction Stop
Import-Module -Name (Join-Path $PSScriptRoot 'VerifyCore.psm1') -Force -ErrorAction Stop

function Invoke-VerifyRuntime
{
    param([hashtable] $Context)
    $targetDir = $Context.TargetDir
    $remoteDir = $Context.RemoteDir
    $arch = $Context.Arch
    $receipt = $Context.Receipt
& $Context.WriteSection 'Last run'

$logPath = Join-Safe $targetDir 'ReShade.log'
$logText = $null
$logStale = $false

if (-not (Test-FileHere $logPath)) {
    & $Context.Report -Status 'Info' -Text 'No ReShade.log yet: the game has not run since the install.'
}
else {
    $logTime = (Get-Item -LiteralPath $logPath).LastWriteTimeUtc
    if ($receipt -and $receipt.PSObject.Properties['InstalledAt']) {
        $installedAt = [datetime]::MinValue
        try { $installedAt = ([datetime]::Parse([string]$receipt.InstalledAt)).ToUniversalTime() } catch { }
        if ($logTime -lt $installedAt) { $logStale = $true }
    }
    if ($logStale) {
        & $Context.Report -Status 'Info' -Text 'ReShade.log is older than the install (stale log); start the game once.'
    }
    else { $logText = Read-TextSafe $logPath }
}

$rt = $Context.Result.Runtime
$rt['LogPath']  = $logPath
$rt['LogStale'] = $logStale
$rt['Loaded']   = $false
$rt['Hooked']   = $false
$rt['Feature']  = $null
$rt['Warped']   = $false
$rt['Temporal'] = $null
$coreEvidence = Get-OfpsCoreLogEvidence -LogText $logText -ExpectedVersion $Context.Result.Version -Stale $logStale
$rt['Core'] = $coreEvidence
switch ($coreEvidence.Verdict) {
    'loaded' { & $Context.Report -Status 'Ok' -Text ('Core loaded: ABI ' + $coreEvidence.Abi + ', release ' + $coreEvidence.Release) }
    'stale-log' { & $Context.Report -Status 'Info' -Text 'Core load evidence is stale after Update.' }
    'unknown' { & $Context.Report -Status 'Warn' -Text ('Core load is unverified: ' + $coreEvidence.Reason) }
    default { & $Context.Report -Status 'Fail' -Text ('Core load failed: ' + $coreEvidence.Verdict + ' ' + $coreEvidence.Reason) }
}

$anyRuntime = $false

if ($logText) {
    $anyRuntime = $true

    if ($logText -match ('(?i)Registered add-on "' + [regex]::Escape($Context.AddonName) + '"')) {
        $rt['Loaded'] = $true
        & $Context.Report -Status 'Ok' -Text 'ReShade registered the add-on.'
    }
    else {
        & $Context.Report -Status 'Fail' -Text 'ReShade never registered "Optimizer FPS for DLSS5".' `
               -Detail 'Either the add-on is disabled, or the ReShade build has no add-on support, or the file is blocked (mark of the web).'
    }

    if ($logText -match '(?i)nvngx_dlssnr\.dll feature 18 create/evaluate/release hooked') {
        $rt['Hooked'] = $true
        & $Context.Report -Status 'Ok' -Text 'The NGX hook is in: nvngx_dlssnr.dll feature 18.'
    }
    else {
        & $Context.Report -Status 'Warn' -Text 'The add-on never hooked nvngx_dlssnr.dll.' `
               -Detail 'Nothing loaded the neural-rendering runtime in that session: check the consumer and that DLSS 5 NR was on.'
    }

    $nativeW = 0; $nativeH = 0; $modelW = 0; $modelH = 0
    $m = [regex]::Match($logText, '(?i)feature 18 created, native (\d+)x(\d+), model (\d+)x(\d+) \(([^)]*)\)')
    if ($m.Success) {
        $nativeW = [int]$m.Groups[1].Value; $nativeH = [int]$m.Groups[2].Value
        $modelW  = [int]$m.Groups[3].Value; $modelH  = [int]$m.Groups[4].Value
        $why = $m.Groups[5].Value
        $rt['Feature'] = ('created ' + $nativeW + 'x' + $nativeH + ' -> ' + $modelW + 'x' + $modelH + ' (' + $why + ')')
        if ($why -match '(?i)pass-?through') {
            & $Context.Report -Status 'Warn' -Text ('Feature 18 created at native ' + $nativeW + 'x' + $nativeH + ', but pass-through: ' + $why)
        }
        else {
            & $Context.Report -Status 'Ok' -Text ('Feature 18: native ' + $nativeW + 'x' + $nativeH + ' -> model ' + $modelW + 'x' + $modelH + ' (' + $why + ')')
        }
    }
    else {
        $m = [regex]::Match($logText, '(?i)feature 18 adopted \(created before the hooks were installed\), native (\d+)x(\d+)')
        if ($m.Success) {
            $nativeW = [int]$m.Groups[1].Value; $nativeH = [int]$m.Groups[2].Value
            $rt['Feature'] = ('adopted ' + $nativeW + 'x' + $nativeH)
            & $Context.Report -Status 'Ok' -Text ('Feature 18 adopted at native ' + $nativeW + 'x' + $nativeH + ' (it existed before the hooks went in).')
        }
        else {
            & $Context.Report -Status 'Warn' -Text 'No feature 18 in the log: the consumer never asked for neural rendering.'
        }
    }

    $m = [regex]::Match($logText, '(?i)first warped evaluate completed \(model (\d+)x(\d+)\)')
    if (-not $m.Success) { $m = [regex]::Match($logText, '(?i)first warped evaluate completed \((?:D3D1[12]): \d+x\d+ -> packed (\d+)x(\d+)') }
    if ($m.Success) {
        $rt['Warped'] = $true
        $modelW = [int]$m.Groups[1].Value; $modelH = [int]$m.Groups[2].Value
        $rt['ModelWidth'] = $modelW
        $rt['ModelHeight'] = $modelH
        if ($nativeW -gt 0 -and $nativeH -gt 0) {
            $pct = 100.0 * ($modelW * $modelH) / ($nativeW * $nativeH)
            $rt['ModelPixelsPercent'] = [math]::Round($pct, 1)
            & $Context.Report -Status 'Ok' -Text ('Frames are being warped: the model runs on ' + $modelW + 'x' + $modelH + ', ' + ('{0:N1}' -f $pct) + '% of the native pixels.')
        }
        else {
            & $Context.Report -Status 'Ok' -Text ('Frames are being warped (model ' + $modelW + 'x' + $modelH + ').')
        }
    }
    else {
        & $Context.Report -Status 'Warn' -Text 'No warped frame in the log yet.'
    }

    $m = [regex]::Match($logText, '(?i)temporal mode (\d+) running \(full pass every (\d+) frames')
    if ($m.Success) {
        $rt['Temporal'] = ('mode ' + $m.Groups[1].Value + ', full pass every ' + $m.Groups[2].Value + ' frames')
        & $Context.Report -Status 'Ok' -Text ('Temporal mode ' + $m.Groups[1].Value + ' ran (full pass every ' + $m.Groups[2].Value + ' frames).')
    }
    $bg = [regex]::Matches($logText, '(?i)background pass (\d+) adopted').Count
    if ($bg -gt 0) {
        $rt['BackgroundPasses'] = $bg
        & $Context.Report -Status 'Ok' -Text ('' + $bg + ' background model pass(es) adopted.')
    }

    if ($logText -match '(?i)shaders were not found in optimizer-fps-dlss5') {
        & $Context.Report -Status 'Fail' -Text 'The add-on could not find its shaders in optimizer-fps-dlss5\ beside the add-on.'
    }
    $m = [regex]::Match($logText, '(?i)nvngx\.dll_optimizerfps\.dll (is missing beside the add-on|lacks its exports)')
    if ($m.Success) {
        & $Context.Report -Status 'Fail' -Text ('The NGX forwarder ' + $m.Groups[1].Value + '.')
    }
    if ($logText -match '(?i)crash guard - the previous session of this game ended without unloading') {
        & $Context.Report -Status 'Warn' -Text 'The crash guard tripped: that session forwarded everything untouched.' `
               -Detail 'Press Retry in the add-on tab, or delete optimizer-fps-dlss5.session, and start the game again.'
    }
}

# x86: the remote tab lives in the game's own ReShade.
if ($arch -eq 'x86' -and $remoteDir) {
    $remoteLog = Join-Safe $remoteDir 'ReShade.log'
    $rt['RemoteLogPath'] = $remoteLog
    $rt['RemoteLogStale'] = $false
    $rt['RemoteTabLoaded'] = $false
    if (Test-FileHere $remoteLog) {
        if ($receipt -and $receipt.PSObject.Properties['InstalledAt']) {
            $installedAt = [datetime]::MinValue
            try { $installedAt = ([datetime]::Parse([string]$receipt.InstalledAt)).ToUniversalTime() } catch { }
            $rt['RemoteLogStale'] = (Get-Item -LiteralPath $remoteLog).LastWriteTimeUtc -lt $installedAt
        }
        $rlt = $null
        if (-not $rt['RemoteLogStale']) { $rlt = Read-TextSafe $remoteLog }
        if ($rlt -and $rlt -match ('(?i)Registered add-on "' + [regex]::Escape($Context.RemoteAddonName) + '"')) {
            $rt['RemoteTabLoaded'] = $true
            & $Context.Report -Status 'Ok' -Text 'The 32-bit game loaded the remote tab.'
        }
        elseif ($rt['RemoteLogStale']) { & $Context.Report -Status 'Info' -Text 'The 32-bit remote ReShade.log is older than the install.' }
        else {
            $rt['RemoteTabLoaded'] = $false
            & $Context.Report -Status 'Warn' -Text 'The game''s own ReShade never registered the remote tab.'
        }
    }
    else { & $Context.Report -Status 'Info' -Text 'No ReShade.log beside the 32-bit ReShade yet.' }
}

$Context.AnyRuntime = $anyRuntime
}

Export-ModuleMember -Function Invoke-VerifyRuntime
