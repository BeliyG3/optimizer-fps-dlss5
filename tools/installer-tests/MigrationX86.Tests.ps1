#Requires -Version 5.1
param([string]$RepoRoot,[string]$Root,[string]$Payload,[string]$ReShade64,[string]$ReShade32)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $RepoRoot 'tools\installer\IniMigration.psm1') -Force

if ($ReShade64 -and $ReShade32 -and
    (Test-Path -LiteralPath $ReShade64) -and (Test-Path -LiteralPath $ReShade32)) {
    $g = New-X86Fixture -Name 'migration-x86-host64' `
        -Root $Root -ReShade64 $ReShade64 -ReShade32 $ReShade32
    $hIni = Join-Path $g 'host64\ReShade.ini'
    $gIni = Join-Path $g 'ReShade.ini'
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'tests\fixtures\reshade_ini\legacy_2615.ini') -Destination $hIni -Force
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'tests\fixtures\reshade_ini\legacy_background.ini') -Destination $gIni -Force
    $beforeDisabled = Get-OfpsIniValue ([IO.File]::ReadAllText($gIni)) 'ADDON' 'DisabledAddons'
    $r = Invoke-Installer @('-GameExe',(Join-Path $g 'game.exe'),'-Payload',$Payload,
                            '-Mode','Update','-Yes','-NoPause','-NoVerify')
    Check 'x86 migration Update exits 0/10' ($r.Code -eq 0 -or $r.Code -eq 10) $r.Out
    foreach ($ini in @($hIni,$gIni)) {
        $sections = Get-OfpsIniSections ([IO.File]::ReadAllText($ini))
        Check ('x86 migrated ' + $ini) ($sections.New -and -not $sections.Old)
    }
    Check 'x86 game DisabledAddons preserved' `
        ((Get-OfpsIniValue ([IO.File]::ReadAllText($gIni)) 'ADDON' 'DisabledAddons') -ceq $beforeDisabled)
    Check 'x86 core only in host64' `
        ((Test-Path -LiteralPath (Join-Path $g 'host64\optimizer-fps-dlss5-core.dll')) -and
         -not (Test-Path -LiteralPath (Join-Path $g 'optimizer-fps-dlss5-core.dll')))
    Check 'x86 remote beside game ReShade' `
        (Test-Path -LiteralPath (Join-Path $g 'optimizer-fps-dlss5-remote.addon32'))
    $separate = New-X86Fixture -Name 'migration-x86-addon-path' `
        -Root $Root -ReShade64 $ReShade64 -ReShade32 $ReShade32
    $separateHost = Join-Path $separate 'host64'
    $separateAddon = Join-Path $separateHost 'addons'
    New-Item -ItemType Directory -Path $separateAddon | Out-Null
    [IO.File]::WriteAllText((Join-Path $separateHost 'ReShade.ini'),
        "[ADDON]`r`nAddonPath=addons`r`n[PeripheralWarp]`r`nMode=2`r`n")
    $r = Invoke-Installer @('-GameExe',(Join-Path $separate 'game.exe'),'-Payload',$Payload,
                            '-Mode','Update','-Yes','-NoPause','-NoVerify')
    Check 'x86 separate AddonPath Update exits 0/10' ($r.Code -eq 0 -or $r.Code -eq 10) $r.Out
    Check 'x86 separate AddonPath receives core and addon' `
        ((Test-Path -LiteralPath (Join-Path $separateAddon 'optimizer-fps-dlss5-core.dll')) -and
         (Test-Path -LiteralPath (Join-Path $separateAddon 'optimizer-fps-dlss5.addon64')))
    Check 'x86 separate AddonPath migrates host ini' `
        ((Get-OfpsIniSections ([IO.File]::ReadAllText((Join-Path $separateHost 'ReShade.ini')))).New)
    $hostLog = Join-Path $g 'host64\ReShade.log'
    $remoteLog = Join-Path $g 'ReShade.log'
    [IO.File]::WriteAllText($hostLog, ('Registered add-on "Optimizer FPS for DLSS5"' + "`r`n" +
        ('Optimizer FPS: core loaded; ABI 1; release ' + $testContext.ReleaseVersion)), (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText($remoteLog,
        'Registered add-on "Optimizer FPS for DLSS5 (tab for the 64-bit host)"',
        (New-Object Text.UTF8Encoding($false)))
    (Get-Item -LiteralPath $hostLog).LastWriteTimeUtc = (Get-Date).ToUniversalTime()
    (Get-Item -LiteralPath $remoteLog).LastWriteTimeUtc = (Get-Date).ToUniversalTime()
    $run = Invoke-Verifier @('-GameExe',(Join-Path $g 'game.exe'),'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'x86 host core and remote log checked separately' ($json -and $run.Code -eq 0 -and
        $json.Runtime.Core.Verdict -eq 'loaded' -and $json.Runtime.RemoteTabLoaded) $run.Out
    (Get-Item -LiteralPath $remoteLog).LastWriteTimeUtc = (Get-Date).ToUniversalTime().AddDays(-2)
    $run = Invoke-Verifier @('-GameExe',(Join-Path $g 'game.exe'),'-Payload',$Payload,'-Json')
    $json = $null
    try { $json = $run.Out | ConvertFrom-Json } catch { }
    Check 'x86 remote log older than Update is stale' ($json -and $json.Runtime.RemoteLogStale -and
        -not $json.Runtime.RemoteTabLoaded) $run.Out

    $rollback = New-X86Fixture -Name 'migration-x86-invalid-ini' `
        -Root $Root -ReShade64 $ReShade64 -ReShade32 $ReShade32
    $rollbackHost = Join-Path $rollback 'host64\ReShade.ini'
    $rollbackGame = Join-Path $rollback 'ReShade.ini'
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'tests\fixtures\reshade_ini\legacy_2615.ini') -Destination $rollbackHost -Force
    [IO.File]::WriteAllBytes($rollbackGame, [byte[]]@(0xff,0xfe,0x41,0x00))
    $beforeHost = (Get-FileHash -LiteralPath $rollbackHost -Algorithm SHA256).Hash
    $beforeGame = (Get-FileHash -LiteralPath $rollbackGame -Algorithm SHA256).Hash
    $r = Invoke-Installer @('-GameExe',(Join-Path $rollback 'game.exe'),'-Payload',$Payload,
                            '-Mode','Update','-Yes','-NoPause','-NoVerify')
    Check 'x86 invalid second ini fails before write' ($r.Code -eq 1) $r.Out
    Check 'x86 first ini unchanged on second ini failure' `
        ((Get-FileHash -LiteralPath $rollbackHost -Algorithm SHA256).Hash -eq $beforeHost)
    Check 'x86 second ini unchanged on failure' `
        ((Get-FileHash -LiteralPath $rollbackGame -Algorithm SHA256).Hash -eq $beforeGame)
    Check 'x86 failed migration has no receipt' `
        (-not (Test-Path -LiteralPath (Join-Path $rollback 'host64\_OptimizerFPS\latest-receipt.json')))
}
