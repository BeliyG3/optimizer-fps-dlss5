#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Payload,
    [string]$ReShade64,
    [string]$ReShade32
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $RepoRoot 'tools\installer\IniMigration.psm1') -Force

function Get-SectionPairs([string]$Text, [string]$Section) {
    $active = $false
    $pairs = New-Object System.Collections.ArrayList
    foreach ($line in ($Text -split "`r?`n")) {
        $trimmed = $line.Trim().TrimStart([char]0xFEFF)
        if ($trimmed -match '^\[([^\]]+)\]$') {
            $active = $Matches[1] -ieq $Section
        }
        elseif ($active -and $trimmed -match '^([^=]+?)\s*=\s*(.*)$') {
            $null = $pairs.Add($Matches[1].Trim() + '=' + $Matches[2].Trim())
        }
    }
    return @($pairs.ToArray())
}
$fixtureDir = Join-Path $RepoRoot 'tests\fixtures\reshade_ini'
$names = @('legacy_2615.ini', 'legacy_2628.ini',
           'legacy_background.ini', 'both_sections.ini',
           'new_wins.ini', 'empty_new.ini')
$known = @(Get-OfpsKnownIniKeys)
$unique = @($known | Sort-Object -Unique)
Check 'migration key table has no duplicates' ($unique.Count -eq $known.Count)
Check 'migration key table includes shell and DebugWarpPath' `
    (($known -contains 'CrashGuard') -and ($known -contains 'DebugWarpPath'))
Check 'migration excludes old bridge/FG keys' `
    (-not ($known -contains 'ForceBridgeWarpOff') -and
     -not ($known -contains 'OptiScalerTakeover'))

# In repository tests the source table must still contain 45 setting IDs.
$schemaPath = Join-Path $RepoRoot 'core\settings\schema.cpp'
$schemaText = [IO.File]::ReadAllText($schemaPath)
$rows = [regex]::Matches($schemaText,
    'Row\(OFPS_SET_[A-Z0-9_]+,\s*"([^"]+)"')
$schemaKeys = @($rows | ForEach-Object { $_.Groups[1].Value })
Check 'schema source still contains 45 iniKey rows' ($schemaKeys.Count -eq 45)
foreach ($key in $schemaKeys) {
    Check ('schema migration parity: ' + $key) ($known -contains $key)
}
$shellKeys = @('Passive','FloatingWindow','TraceExit','DebugLayer','CrashGuard')
foreach ($key in $shellKeys) {
    Check ('shell migration parity: ' + $key) ($known -contains $key)
}

foreach ($name in $names) {
    $source = Join-Path $fixtureDir $name
    $copy = Join-Path $Root $name
    Copy-Item -LiteralPath $source -Destination $copy -Force
    $beforeHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $oldText = [IO.File]::ReadAllText($copy)
    $sections = Get-OfpsIniSections -Text $oldText
    $change = Convert-OfpsIniForUpdate -Text $oldText
    $after = Get-OfpsIniSections -Text $change.Text
    $again = Convert-OfpsIniForUpdate -Text $change.Text
    Check ($name + ' removes old section') (-not $after.Old)
    Check ($name + ' Update is idempotent') `
        (-not $again.Changed -and $again.Text -ceq $change.Text)
    Check ($name + ' fixture source is untouched') `
        ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -eq $beforeHash)
    if ($sections.Old -and -not $sections.New) {
        Check ($name + ' moves old-only keys') `
            ($change.Outcome -eq 'old-only' -and $after.New)
        foreach ($key in $known) {
            $prior = Get-OfpsIniValue $oldText 'PeripheralWarp' $key
            if ($null -eq $prior) { continue }
            Check ($name + ' preserves ' + $key) `
                ((Get-OfpsIniValue $change.Text 'OptimizerFPS' $key) -ceq $prior)
        }
    }
    elseif ($sections.New) {
        $expectedOutcome = if ($sections.Old) { 'new-wins' } else { 'none' }
        Check ($name + ' new section wins') ($change.Outcome -eq $expectedOutcome)
        foreach ($key in $known) {
            $prior = Get-OfpsIniValue $oldText 'OptimizerFPS' $key
            $next = Get-OfpsIniValue $change.Text 'OptimizerFPS' $key
            Check ($name + ' keeps new ' + $key) ($prior -ceq $next)
        }
    }
    $disabledBefore = Get-OfpsIniValue $oldText 'ADDON' 'DisabledAddons'
    $disabledAfter = Get-OfpsIniValue $change.Text 'ADDON' 'DisabledAddons'
    Check ($name + ' preserves DisabledAddons') `
        ($disabledBefore -ceq $disabledAfter)
    foreach ($section in @('ADDON','PeripheralWarpDLSS','RenoDX.DLSS5','GENERAL')) {
        $prior = @(Get-SectionPairs $oldText $section)
        $next = @(Get-SectionPairs $change.Text $section)
        Check ($name + ' keeps all ' + $section + ' key/value pairs') `
            ([string]::Join([char]0, $prior) -ceq [string]::Join([char]0, $next))
    }
}

$blank = Convert-OfpsIniForUpdate -Text "[ADDON]`r`nDisabledAddons=x`r`n"
Check 'no-section is unchanged' `
    (-not $blank.Changed -and $blank.Outcome -eq 'none')
$newOnlyText = "[OptimizerFPS]`r`nMode=3`r`n[ADDON]`r`nDisabledAddons=a,b`r`n"
$newOnly = Convert-OfpsIniForUpdate -Text $newOnlyText
Check 'new-only section is byte-identical' `
    (-not $newOnly.Changed -and $newOnly.Outcome -eq 'none' -and $newOnly.Text -ceq $newOnlyText)
$duplicate = Convert-OfpsIniForUpdate -Text `
    "[pErIpHeRaLwArP]`r`nmode=1`r`nMODE=2`r`nUnknown=private`r`n[OTHER]`r`nValue=kept`r`n"
Check 'case-insensitive duplicate uses last known value' `
    ((Get-OfpsIniValue $duplicate.Text 'OptimizerFPS' 'Mode') -ceq '2')
Check 'case-insensitive duplicate is written once' `
    (@([regex]::Matches($duplicate.Text, '(?im)^Mode=')).Count -eq 1)
Check 'unknown key and other section remain independent' `
    ($null -eq (Get-OfpsIniValue $duplicate.Text 'OptimizerFPS' 'Unknown') -and
     (Get-OfpsIniValue $duplicate.Text 'OTHER' 'Value') -ceq 'kept')
$emptyNew = Convert-OfpsIniForUpdate -Text `
    "[PeripheralWarp]`r`nMode=2`r`n[OptimizerFPS]`r`n"
Check 'empty new suppresses legacy fallback' `
    ($emptyNew.Outcome -eq 'new-wins' -and
     $null -eq (Get-OfpsIniValue $emptyNew.Text 'OptimizerFPS' 'Mode'))
$unknown = Convert-OfpsIniForUpdate -Text `
    "[PeripheralWarp]`r`nMode=2`r`nUnknown=secret`r`n"
Check 'unknown key is not copied' `
    ($null -eq (Get-OfpsIniValue $unknown.Text 'OptimizerFPS' 'Unknown'))
$bom = [char]0xFEFF + "[PeripheralWarp]`r`nMode=2`r`n[ADDON]`r`nDisabledAddons=a,b`r`n"
$bomResult = Convert-OfpsIniForUpdate -Text $bom
Check 'BOM survives outside old block' `
    ($bomResult.Text[0] -eq [char]0xFEFF)
Check 'CRLF survives' `
    ($bomResult.Text -match "`r`n" -and $bomResult.Text -notmatch "(?<!`r)`n")

$path = Join-Path $Root 'ownership.ini'
$old = "[PeripheralWarp]`r`nMode=2`r`nCenterX=80`r`n"
$converted = Convert-OfpsIniForUpdate -Text $old
$priorReceipt = @(
    [pscustomobject]@{Path=$path;Section='PeripheralWarp';Key='Mode';Value='2'},
    [pscustomobject]@{Path=$path;Section='PeripheralWarp';Key='CenterX';Value='79'}
)
$owned = @(Convert-OfpsIniOwnership -Previous $priorReceipt -Path $path `
    -OldText $old -NewText $converted.Text -Outcome $converted.Outcome)
Check 'only unchanged installer-owned old key transfers' `
    ($owned.Count -eq 1 -and $owned[0].Section -eq 'OptimizerFPS' -and
     $owned[0].Key -eq 'Mode')
$newWins = Convert-OfpsIniForUpdate -Text `
    ($old + "[OptimizerFPS]`r`nMode=1`r`n")
$notOwned = @(Convert-OfpsIniOwnership -Previous $priorReceipt -Path $path `
    -OldText $old -NewText $newWins.Text -Outcome $newWins.Outcome)
Check 'new-wins does not adopt old ownership' ($notOwned.Count -eq 0)
$sameValueNewWins = Convert-OfpsIniForUpdate -Text ($old + "[OptimizerFPS]`r`nMode=2`r`n")
$sameValueNotOwned = @(Convert-OfpsIniOwnership -Previous $priorReceipt -Path $path `
    -OldText $old -NewText $sameValueNewWins.Text -Outcome $sameValueNewWins.Outcome)
Check 'new-wins same value is still user-owned' ($sameValueNotOwned.Count -eq 0)

Import-Module (Join-Path $RepoRoot 'tools\installer\Common.psm1') -Force
Import-Module (Join-Path $RepoRoot 'tools\installer\IniFile.psm1') -Force
$first = Join-Path $Root 'transaction-first.ini'
$backup = Join-Path $Root 'transaction-first.backup'
[IO.File]::WriteAllText($first, $old, (New-Object Text.UTF8Encoding($false)))
Copy-Item -LiteralPath $first -Destination $backup
$firstDoc = Read-OfpsIniDocument -Path $first
$backupChange = [pscustomobject]@{Path=$first; Document=$firstDoc; BackupPath=$null}
$missingBackupCaught = $false
try { Test-OfpsIniBackups -Changes @($backupChange) -BackupDir $Root -PrimaryPath $first }
catch { $missingBackupCaught = $true }
Check 'missing ini backup is rejected' $missingBackupCaught
$backupFolder = Join-Path $Root 'corrupt-backup'
New-Item -ItemType Directory -Path $backupFolder | Out-Null
[IO.File]::WriteAllText((Join-Path $backupFolder 'ReShade.ini.original'), 'damaged')
$corruptBackupCaught = $false
try { Test-OfpsIniBackups -Changes @($backupChange) -BackupDir $backupFolder -PrimaryPath $first }
catch { $corruptBackupCaught = $true }
Check 'corrupt ini backup is rejected' $corruptBackupCaught
$badDoc = [pscustomobject]@{ Exists=$false; Text=''; Encoding=(New-Object Text.UTF8Encoding($false)); Hash=$null }
$txChanges = @(
    [pscustomobject]@{Path=$first; Document=$firstDoc; NewText=$converted.Text; BackupPath=$backup},
    [pscustomobject]@{Path='?:\invalid\ReShade.ini'; Document=$badDoc; NewText='[OptimizerFPS]'; BackupPath=$null}
)
$rollbackCaught = $false
try { Write-OfpsIniTransaction -Changes $txChanges -Context @{Changed=(New-Object System.Collections.ArrayList)} }
catch { $rollbackCaught = $true }
Check 'second ini write failure triggers rollback' $rollbackCaught
Check 'first ini restored byte-identically after second write failure' `
    ((Get-FileHash -LiteralPath $first -Algorithm SHA256).Hash -eq
     (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash)
$bomPath = Join-Path $Root 'migration-bom.ini'
$bomBytes = [byte[]](@(0xef,0xbb,0xbf) + [Text.Encoding]::UTF8.GetBytes($bom.Substring(1)))
[IO.File]::WriteAllBytes($bomPath, $bomBytes)
$bomDoc = Read-OfpsIniDocument -Path $bomPath
$bomChange = Convert-OfpsIniForUpdate -Text $bomDoc.Text
Write-OfpsIniTransaction -Changes @([pscustomobject]@{
    Path=$bomPath; Document=$bomDoc; NewText=$bomChange.Text; BackupPath=$null
}) -Context @{Changed=(New-Object System.Collections.ArrayList)}
$writtenBom = [IO.File]::ReadAllBytes($bomPath)
Check 'UTF-8 BOM survives migration write' `
    ($writtenBom.Length -ge 3 -and $writtenBom[0] -eq 0xef -and
     $writtenBom[1] -eq 0xbb -and $writtenBom[2] -eq 0xbf)
Check 'UTF-8 BOM is not duplicated' `
    ($writtenBom.Length -lt 6 -or -not ($writtenBom[3] -eq 0xef -and
     $writtenBom[4] -eq 0xbb -and $writtenBom[5] -eq 0xbf))

# Full integration needs real PE files; the main runner enforces this in the
# acceptance profile. Pure tests above always run.
if ($ReShade64 -and (Test-Path -LiteralPath $ReShade64) -and
    $Payload -and (Test-Path -LiteralPath (Join-Path $Payload 'files.sha256'))) {
    foreach ($name in $names) {
        $text = [IO.File]::ReadAllText((Join-Path $fixtureDir $name))
        $dir = New-X64Fixture -Name ('migration-' + $name) -IniText $text `
            -Root $Root -ReShade64 $ReShade64
        $exe = Join-Path $dir 'pwgame.exe'
        $result = Invoke-Installer @('-GameExe',$exe,'-Payload',$Payload,
                                     '-Mode','Update','-Yes','-NoPause','-NoVerify')
        Check ($name + ' integrated Update exits 0/10') `
            ($result.Code -eq 0 -or $result.Code -eq 10) $result.Out
        $receiptPath = Join-Path $dir '_OptimizerFPS\latest-receipt.json'
        Check ($name + ' receipt exists') (Test-Path -LiteralPath $receiptPath)
        if (Test-Path -LiteralPath $receiptPath) {
            $receipt = [IO.File]::ReadAllText($receiptPath) | ConvertFrom-Json
            Check ($name + ' receipt Schema 3') ([int]$receipt.Schema -eq 3)
            Check ($name + ' receipt has Core') `
                ($null -ne $receipt.Core -and [int]$receipt.Core.Abi -eq 1)
            Check ($name + ' ownership uses new section') `
                (@($receipt.IniKeysWritten | Where-Object {
                    $_.Section -ne 'OptimizerFPS'
                }).Count -eq 0)
        }
    }

    $ownedDir = New-X64Fixture -Name 'migration-schema2-ownership' `
        -IniText $old -Root $Root -ReShade64 $ReShade64
    $ownedIni = Join-Path $ownedDir 'ReShade.ini'
    $stateDir = Join-Path $ownedDir '_OptimizerFPS'
    $null = New-Item -ItemType Directory -Path $stateDir -Force
    $oldReceipt = @{
        Schema=2
        IniKeysWritten=@(
            @{Path=$ownedIni;Section='PeripheralWarp';Key='Mode';Value='2'},
            @{Path=$ownedIni;Section='PeripheralWarp';Key='CenterX';Value='79'}
        )
    }
    [IO.File]::WriteAllText((Join-Path $stateDir 'latest-receipt.json'),
        ($oldReceipt | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
    $r = Invoke-Installer @('-GameExe',(Join-Path $ownedDir 'pwgame.exe'),'-Payload',$Payload,
                            '-Mode','Update','-Yes','-NoPause','-NoVerify')
    Check 'Schema 2 ownership Update exits 0/10' ($r.Code -eq 0 -or $r.Code -eq 10) $r.Out
    $receipt = [IO.File]::ReadAllText((Join-Path $stateDir 'latest-receipt.json')) | ConvertFrom-Json
    Check 'Schema 2 transfers only unchanged installer key' `
        (@($receipt.IniKeysWritten).Count -eq 1 -and
         $receipt.IniKeysWritten[0].Section -eq 'OptimizerFPS' -and
         $receipt.IniKeysWritten[0].Key -eq 'Mode')
    $updated = [IO.File]::ReadAllText($ownedIni).Replace('Mode=2','Mode=9')
    [IO.File]::WriteAllText($ownedIni, $updated, (New-Object Text.UTF8Encoding($false)))
    $r = Invoke-Installer @('-GameExe',(Join-Path $ownedDir 'pwgame.exe'),
                            '-Mode','Uninstall','-Yes','-NoPause')
    Check 'Uninstall after user edit exits 0' ($r.Code -eq 0) $r.Out
    Check 'Uninstall preserves changed migrated key' `
        ((Get-OfpsIniValue ([IO.File]::ReadAllText($ownedIni)) 'OptimizerFPS' 'Mode') -ceq '9')
    Check 'Uninstall does not restore old section' `
        (-not (Get-OfpsIniSections ([IO.File]::ReadAllText($ownedIni))).Old)

    $partialDir = New-X64Fixture -Name 'migration-partial-receipt' `
        -IniText $old -Root $Root -ReShade64 $ReShade64
    $partialState = Join-Path $partialDir '_OptimizerFPS'
    New-Item -ItemType Directory -Path $partialState | Out-Null
    [IO.File]::WriteAllText((Join-Path $partialState 'latest-receipt.json'),
        '{"Schema":2,"IniKeysWritten":[]}', (New-Object Text.UTF8Encoding($false)))
    $partialExe = Join-Path $partialDir 'pwgame.exe'
    $r = Invoke-Installer @('-GameExe',$partialExe,'-Payload',$Payload,
                            '-Mode','Update','-Yes','-NoPause','-NoVerify')
    Check 'partial receipt Update exits 0/10' ($r.Code -eq 0 -or $r.Code -eq 10) $r.Out
    $partialReceipt = [IO.File]::ReadAllText((Join-Path $partialState 'latest-receipt.json')) | ConvertFrom-Json
    Check 'partial receipt becomes Schema 3 with core' `
        ([int]$partialReceipt.Schema -eq 3 -and [int]$partialReceipt.Core.Abi -eq 1)
    $firstHash = (Get-FileHash -LiteralPath (Join-Path $partialDir 'ReShade.ini')).Hash
    $r = Invoke-Installer @('-GameExe',$partialExe,'-Payload',$Payload,
                            '-Mode','Update','-Yes','-NoPause','-NoVerify')
    Check 'repeated integrated Update exits 0/10' ($r.Code -eq 0 -or $r.Code -eq 10) $r.Out
    Check 'repeated integrated Update keeps ini bytes' `
        ((Get-FileHash -LiteralPath (Join-Path $partialDir 'ReShade.ini')).Hash -eq $firstHash)
}
else {
    Skip 'migration integrated Update fixtures' `
        'real ReShade64 DLL or packaged payload absent; required acceptance run must provide them'
}
