#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Payload,
    [Parameter(Mandatory = $true)][string]$ReShade64
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $RepoRoot 'tools\installer\Common.psm1') -Force
Import-Module (Join-Path $RepoRoot 'tools\installer\Pe.psm1') -Force
Import-Module (Join-Path $RepoRoot 'tools\installer\VerifyCore.psm1') -Force

$game = $testContext.VerifyGame
$exe = $testContext.VerifyExe
$core = Join-Path $game 'optimizer-fps-dlss5-core.dll'
$saved = Join-Path $Root 'core-static-original.dll'
Copy-Item -LiteralPath $core -Destination $saved -Force
$expected = (Get-FileHash -LiteralPath $saved -Algorithm SHA256).Hash

$versionEvidence = Get-OfpsCoreFileEvidence -CorePath $saved `
    -ExpectedVersion '2026.9.1' -ExpectedHash $expected `
    -ReadPeInfo { param($path) Get-PeInfo $path } `
    -ReadExports { param($path) Get-PeExportNames $path } `
    -ReadVersion { param($path) '2026.9.0' }
Check 'core version mismatch is invalid' `
    ($versionEvidence.StaticVerdict -eq 'invalid' -and -not $versionEvidence.VersionMatches)

try {
    [IO.File]::WriteAllBytes($core, ([IO.File]::ReadAllBytes($saved) + [byte[]]@(42)))
    $run = Invoke-Verifier @('-GameExe',$exe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'changed core hash fails Verify' `
        ($json -and $run.Code -eq 1 -and $json.Static.Core.StaticVerdict -eq 'invalid' -and
         -not $json.Static.Core.HashMatches) $run.Out

    [IO.File]::WriteAllBytes($core, [byte[]]@(1,2,3,4))
    $run = Invoke-Verifier @('-GameExe',$exe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'invalid core PE fails Verify' `
        ($json -and $run.Code -eq 1 -and $json.Static.Core.StaticVerdict -eq 'invalid' -and
         -not $json.Static.Core.HasCreateExport) $run.Out

    Copy-Item -LiteralPath $ReShade64 -Destination $core -Force
    $run = Invoke-Verifier @('-GameExe',$exe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'foreign same-named core fails Verify' `
        ($json -and $run.Code -eq 1 -and $json.Static.Core.StaticVerdict -eq 'invalid' -and
         -not $json.Static.Core.HasVersionExport) $run.Out
}
finally {
    Copy-Item -LiteralPath $saved -Destination $core -Force
}

if ($testContext.VerifyGame) {
    $verifyGame = $testContext.VerifyGame
    $verifyExe = $testContext.VerifyExe
    $verifyLog = Join-Path $verifyGame 'ReShade.log'
    $prefix = 'Registered add-on "Optimizer FPS for DLSS5"' + "`r`n"
    foreach ($case in @(
        @{Name='core success'; Line='Optimizer FPS: core loaded; ABI 1; release 2026.9.1'; Code=0; Verdict='loaded'},
        @{Name='core missing'; Line='Optimizer FPS: core load failed: core DLL could not be loaded: 126'; Code=1; Verdict='missing'},
        @{Name='core version'; Line='Optimizer FPS: core loaded; ABI 1; release 2026.9.0'; Code=1; Verdict='version-mismatch'},
        @{Name='core ABI'; Line='Optimizer FPS: core loaded; ABI 2; release 2026.9.1'; Code=1; Verdict='abi-mismatch'},
        @{Name='no core line'; Line=''; Code=10; Verdict='unknown'}
    )) {
        [IO.File]::WriteAllText($verifyLog, ($prefix + $case.Line), (New-Object Text.UTF8Encoding($false)))
        (Get-Item -LiteralPath $verifyLog).LastWriteTimeUtc = (Get-Date).ToUniversalTime()
        $run = Invoke-Verifier @('-GameExe',$verifyExe,'-Payload',$Payload,'-Json')
        $json = $null
        try { $json = $run.Out | ConvertFrom-Json } catch { }
        Check ($case.Name + ' runtime verdict') ($json -and $run.Code -eq $case.Code -and
            $json.Runtime.Core.Verdict -eq $case.Verdict) $run.Out
    }
    (Get-Item -LiteralPath $verifyLog).LastWriteTimeUtc = (Get-Date).ToUniversalTime().AddDays(-2)
    $run = Invoke-Verifier @('-GameExe',$verifyExe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'Update rejects stale core log' ($json -and $run.Code -eq 10 -and
        $json.Runtime.Core.Verdict -eq 'stale-log') $run.Out
    $coreFile = Join-Path $verifyGame 'optimizer-fps-dlss5-core.dll'
    $coreBackup = Join-Path $Root 'verify-core-backup.dll'
    Copy-Item -LiteralPath $coreFile -Destination $coreBackup -Force
    Remove-Item -LiteralPath $coreFile -Force
    $run = Invoke-Verifier @('-GameExe',$verifyExe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'missing core fails static check' ($json -and $run.Code -eq 1 -and
        $json.Static.Core.StaticVerdict -eq 'missing') $run.Out
    Copy-Item -LiteralPath $coreBackup -Destination $coreFile -Force
    $shaderFile = Join-Path $verifyGame ('optimizer-fps-dlss5\' + $testContext.ShaderNames[0])
    $shaderBackup = Join-Path $Root 'verify-shader-backup.dxbc'
    Copy-Item -LiteralPath $shaderFile -Destination $shaderBackup -Force
    [IO.File]::WriteAllBytes($shaderFile, [byte[]]@(0,1,2,3))
    $run = Invoke-Verifier @('-GameExe',$verifyExe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'changed manifest shader fails' ($json -and $run.Code -eq 1 -and
        [int]$json.Static.ChangedFiles -gt 0) $run.Out
    Copy-Item -LiteralPath $shaderBackup -Destination $shaderFile -Force
    $extraShader = Join-Path $verifyGame 'optimizer-fps-dlss5\unexpected.dxbc'
    [IO.File]::WriteAllBytes($extraShader, [byte[]]@(0,1,2,3))
    $run = Invoke-Verifier @('-GameExe',$verifyExe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'extra shader is named in warning' ($json -and
        @($json.Static.ExtraShaders) -contains 'unexpected.dxbc') $run.Out
    Remove-Item -LiteralPath $extraShader -Force
    $iniFile = Join-Path $verifyGame 'ReShade.ini'
    $iniBackup = Join-Path $Root 'verify-ini-backup.ini'
    Copy-Item -LiteralPath $iniFile -Destination $iniBackup -Force
    [IO.File]::WriteAllText($iniFile, "[OptimizerFPS]`r`n", (New-Object Text.UTF8Encoding($false)))
    $run = Invoke-Verifier @('-GameExe',$verifyExe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'empty new settings section is valid' ($json -and $json.Static.OptimizerFPSSection -and
        -not $json.Static.LegacyPeripheralWarpSection) $run.Out
    [IO.File]::WriteAllText($iniFile, "[PeripheralWarp]`r`nMode=2`r`n", (New-Object Text.UTF8Encoding($false)))
    $run = Invoke-Verifier @('-GameExe',$verifyExe,'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'old-only settings section warns' ($json -and -not $json.Static.OptimizerFPSSection -and
        $json.Static.LegacyPeripheralWarpSection -and @($json.Warnings | Where-Object { $_ -match 'Legacy' }).Count -gt 0) $run.Out
    Copy-Item -LiteralPath $iniBackup -Destination $iniFile -Force
}
