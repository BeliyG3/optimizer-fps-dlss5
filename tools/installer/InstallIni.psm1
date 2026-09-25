#Requires -Version 5.1
function Convert-AddonNameToCurrent
{
    param([string] $Entry, [hashtable] $Context)
    $e = $Entry.Trim()
    if ($e -match '(?i)^Optimizer FPS for DLSS5\s+\d+(\.\d+)+\s*\(tab for the 64-bit host\)$') { return $Context.RemoteAddonName }
    if ($e -match '(?i)^Optimizer FPS for DLSS5\s+\d+(\.\d+)+$')                               { return $Context.AddonName }
    if ($e -match '(?i)^PeripheralWarp Producer\b.*\(tab for the 64-bit host\)$')              { return $Context.RemoteAddonName }
    if ($e -match '(?i)^PeripheralWarp Producer\b')                                            { return $Context.AddonName }
    return $e
}

function Add-OfpsIniDefaults
{
    param([string] $Text, [object[]] $Keys)
    $newline = if ($Text -notmatch "`r`n" -and $Text -match "`n") { "`n" } else { "`r`n" }
    if ($Text.Length -gt 0 -and -not $Text.EndsWith("`n")) { $Text += $newline }
    if ($Text.Length -gt 0) { $Text += $newline }
    $Text += '[OptimizerFPS]' + $newline
    foreach ($key in $Keys) { $Text += $key.Key + '=' + $key.Value + $newline }
    return $Text
}


function Install-IniSettings
{
    param([string] $IniPath, [string] $RemoteDir, [string] $GameRoot, [string] $Mode, [string] $LatestPath, [System.Collections.ArrayList] $ReceiptFiles, [object[]] $LegacyRemoved, [System.Collections.ArrayList] $RemovedStaleShaders, [hashtable] $Context)
    $iniKeysWritten = New-Object System.Collections.ArrayList
    $migrated = New-Object System.Collections.ArrayList

    $prevReceipt = $null
    if (Test-FileHere $latestPath) {
        try { $prevReceipt = (Read-TextSafe $latestPath) | ConvertFrom-Json } catch { $prevReceipt = $null }
    }

    if ($Context.NoIni) {
        & $Context.WriteSection 'Settings'
        & $Context.Report -Status 'Skip' -Text '-NoIni: ReShade.ini was not touched.'
        if ($prevReceipt -and $prevReceipt.PSObject.Properties['IniKeysWritten']) {
            foreach ($entry in @($prevReceipt.IniKeysWritten)) { $null = $iniKeysWritten.Add($entry) }
        }
    }
    else {
        & $Context.WriteSection 'Settings'
        $changes = New-Object System.Collections.ArrayList
        $seenPaths = @{}
        $wrote = 0
        foreach ($ip in @($Context.IniPaths)) {
            $id = [IO.Path]::GetFullPath($ip).ToLowerInvariant()
            if ($seenPaths.ContainsKey($id)) { continue }
            $seenPaths[$id] = $true
            $document = Read-OfpsIniDocument -Path $ip
            $text = $document.Text
            $outcome = 'none'
            if ($Mode -eq 'Update') {
                $migration = Convert-OfpsIniForUpdate -Text $text
                $text = $migration.Text
                $outcome = $migration.Outcome
            }
            $sections = Get-OfpsIniSections -Text $text
            if ([string]::Equals($ip, $iniPath, [StringComparison]::OrdinalIgnoreCase) -and
                -not $sections.Old -and -not $sections.New -and $outcome -eq 'none') {
                $text = Add-OfpsIniDefaults -Text $text -Keys $Context.DefaultIniKeys
                foreach ($key in $Context.DefaultIniKeys) {
                    $null = $iniKeysWritten.Add([ordered]@{ Path=$ip; Section='OptimizerFPS'; Key=$key.Key; Value=$key.Value })
                    $wrote++
                }
            }
            $null = $changes.Add([pscustomobject]@{
                Path=$ip; Document=$document; NewText=$text; Outcome=$outcome; BackupPath=$null
            })
        }

        # DisabledAddons migration, on the host ini and (x86) on the game's own ini.
        foreach ($change in $changes) {
            if (-not $change.Document.Exists -and $change.Path -ine $iniPath) { continue }
            $ip = $change.Path
            $text = $change.NewText
            $cur = Get-IniKey $text 'ADDON' 'DisabledAddons'
            if ($null -eq $cur -or $cur.Trim() -eq '') { continue }
            $entries = @($cur -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
            $out = New-Object System.Collections.ArrayList
            $listChanged = $false
            foreach ($e in $entries) {
                $n = Convert-AddonNameToCurrent $e -Context $Context
                if ($n -ne $e) { $listChanged = $true; $null = $migrated.Add($e + ' -> ' + $n) }
                if (-not ($out -contains $n)) { $null = $out.Add($n) }
            }
            if ($listChanged) {
                $text = Set-IniKey -Text $text -Section 'ADDON' -Key 'DisabledAddons' -Value (($out.ToArray()) -join ',')
                $change.NewText = $text
                & $Context.Report -Status 'Done' -Text ('Carried the "disabled" state over to the new add-on name in ' + (Get-RelativePathCompat $gameRoot $ip) + '.')
            }
            if ($out -contains $Context.AddonName -or $out -contains $Context.RemoteAddonName) {
                & $Context.Report -Status 'Warn' -Text 'The add-on is disabled in ReShade; enable it in Home -> Add-ons.' -Detail $ip
            }
        }

        Test-OfpsIniBackups -Changes @($changes.ToArray()) -BackupDir $Context.BackupDir -PrimaryPath $iniPath
        Write-OfpsIniTransaction -Changes @($changes.ToArray()) -Context $Context

        if ($prevReceipt -and $prevReceipt.PSObject.Properties['IniKeysWritten']) {
            foreach ($change in $changes) {
                $owned = @(Convert-OfpsIniOwnership -Previous @($prevReceipt.IniKeysWritten) `
                    -Path $change.Path -OldText $change.Document.Text -NewText $change.NewText -Outcome $change.Outcome)
                foreach ($entry in $owned) {
                    if (@($iniKeysWritten.ToArray() | Where-Object {
                        $_.Path -ieq $entry.Path -and $_.Key -ieq $entry.Key
                    }).Count -eq 0) { $null = $iniKeysWritten.Add($entry) }
                }
            }
        }

        if ($wrote -gt 0) {
            & $Context.Report -Status 'Done' -Text ('' + $wrote + ' key(s) written to [OptimizerFPS] in ReShade.ini.') `
                   -Detail 'Mode=2 (peripheral), CenterX/CenterY=80, WorkX/WorkY=90.'
        }
        else {
            & $Context.Report -Status 'Ok' -Text 'Existing settings kept; no defaults added.'
        }
        if ($Mode -eq 'Update') {
            & $Context.Report -Status 'Info' -Text 'Update mode: existing settings are never rewritten.'
        }
    }

    # File and Defender ownership must survive Update. Ini ownership was handled above:
    # only unchanged installer-created values transfer, except -NoIni, which keeps it intact.
    if ($prevReceipt) {
        if ($prevReceipt.PSObject.Properties['DefenderExclusions']) {
            foreach ($x in @($prevReceipt.DefenderExclusions)) {
                if (-not $x) { continue }
                $k = ([string]$x).ToLowerInvariant()
                if ($Context.DefenderExcluded.ContainsKey($k)) { continue }
                $Context.DefenderExcluded[$k] = $true
                $null = $Context.DefenderExclusions.Add([string]$x)
            }
        }
        if ($prevReceipt.PSObject.Properties['Files']) {
            $prevFiles = @{}
            foreach ($f in @($prevReceipt.Files)) {
                if ($f.PSObject.Properties['HadOriginal'] -and [bool]$f.HadOriginal) { $prevFiles[([string]$f.Path).ToLowerInvariant()] = $f }
            }
            foreach ($e in @($receiptFiles.ToArray())) {
                $id = ([string]$e['Path']).ToLowerInvariant()
                if (-not $e['HadOriginal'] -and $prevFiles.ContainsKey($id)) {
                    $e['HadOriginal'] = $true
                    $e['BackupPath']  = [string]$prevFiles[$id].BackupPath
                }
            }
        }
    }

    $Context.Summary.IniKeysWritten         = @($iniKeysWritten.ToArray())
    $Context.Summary.DisabledAddonsMigrated = @($migrated.ToArray())
    $Context.Summary.DefenderExclusions     = @($Context.DefenderExclusions.ToArray())
    $Context.Summary.Files                  = @($receiptFiles.ToArray())
    $Context.Summary.RemovedLegacyFiles     = $legacyRemoved
    $Context.Summary.RemovedStaleShaders    = @($removedStaleShaders.ToArray())


    $Context.IniKeysWritten = $iniKeysWritten
    $Context.Migrated = $migrated
    return [pscustomobject]@{ IniKeysWritten = $iniKeysWritten; Migrated = $migrated }
}

function Write-InstallReceipt
{
    param([hashtable] $Context)
    $corePath = Join-Safe $Context.AddonDir 'optimizer-fps-dlss5-core.dll'
    if (-not (Test-IsOurFile -Path $corePath -Kind 'core' -Context $Context)) {
        & $Context.Stop -Text 'Installed core DLL has invalid PE, version metadata or exports.' -Detail $corePath
    }
    $coreHash = Get-Sha256 $corePath
    $coreVersion = [string](Get-Item -LiteralPath $corePath).VersionInfo.FileVersion
    $fileEntry = @($Context.ReceiptFiles.ToArray() | Where-Object { $_['Path'] -ieq $corePath })
    if ($fileEntry.Count -ne 1 -or $fileEntry[0]['InstalledHash'] -ine $coreHash -or
        $coreVersion -cne $Context.PayloadVersion) {
        & $Context.Stop -Text 'Installed core does not match payload version or receipt hash.' -Detail $corePath
    }
    $core = [ordered]@{ Path=$corePath; InstalledHash=$coreHash; FileVersion=$coreVersion; Abi=1 }
    $Context.Summary.Core = $core
    $receipt = [ordered]@{
        Schema                 = $Context.ReceiptSchema
        Version                = $Context.PayloadVersion
        InstalledAt            = (Get-Date).ToString('o')
        Arch                   = $Context.Arch
        GameExe                = $Context.GameExePath
        TargetDir              = $Context.TargetDir
        AddonDir               = $Context.AddonDir
        RemoteDir              = $Context.RemoteDir
        IniPaths               = $Context.IniPaths
        Files                  = @($Context.ReceiptFiles.ToArray())
        Core                   = $core
        IniKeysWritten         = @($Context.IniKeysWritten.ToArray())
        DisabledAddonsMigrated = @($Context.Migrated.ToArray())
        DefenderExclusions     = @($Context.DefenderExclusions.ToArray())
        RemovedLegacyFiles     = $Context.LegacyRemoved
        RemovedStaleShaders    = @($Context.RemovedStaleShaders.ToArray())
    }

    $receiptPath = Join-Safe $Context.BackupDir 'receipt.json'
    Write-JsonAtomic -Value $receipt -Path $receiptPath -Context $Context
    Write-JsonAtomic -Value $receipt -Path $Context.LatestPath -Context $Context
    $Context.Summary.ReceiptPath = $receiptPath
    & $Context.Report -Status 'Done' -Text 'Receipt written.' -Detail $receiptPath

    # Mark of the Web: files unpacked from a downloaded zip are blocked and ReShade will not
    # load them.
    $unblocked = 0
    foreach ($p in @($Context.Changed.ToArray())) {
        try { if (Test-FileHere $p) { Unblock-File -LiteralPath $p -ErrorAction SilentlyContinue; $unblocked++ } } catch { }
    }
    if ($unblocked -gt 0) { & $Context.Report -Status 'Ok' -Text ('Unblocked ' + $unblocked + ' file(s) (mark of the web).') }
}

function Remove-InstallIniKeys
{
    param([pscustomobject] $Receipt, [string] $TargetDir, [hashtable] $Context)

    # Ini: only the keys this installer wrote. DisabledAddons keeps the migrated name --
    # reverting it would resurrect a name no build answers to any more.
    if ($Receipt -and -not $Context.NoIni -and $Receipt.PSObject.Properties['IniKeysWritten']) {
        $byPath = @{}
        foreach ($k in @($Receipt.IniKeysWritten)) {
            $kp = [string]$k.Path
            if (-not $byPath.ContainsKey($kp)) { $byPath[$kp] = New-Object System.Collections.ArrayList }
            $null = $byPath[$kp].Add($k)
        }
        foreach ($kp in $byPath.Keys) {
            $t = Read-TextSafe $kp
            if ($null -eq $t) { continue }
            $sections = @()
            foreach ($k in $byPath[$kp]) {
                $current = Get-IniKey $t ([string]$k.Section) ([string]$k.Key)
                if ($current -cne [string]$k.Value) {
                    & $Context.Report -Status 'Warn' -Text ('Kept user-changed ini key ' + $k.Section + '/' + $k.Key) -Detail $kp
                    continue
                }
                $t = Remove-IniKey -Text $t -Section ([string]$k.Section) -Key ([string]$k.Key)
                $sections += [string]$k.Section
            }
            foreach ($s in ($sections | Sort-Object -Unique)) { $t = Remove-EmptyIniSection -Text $t -Section $s }
            if ($sections.Count -gt 0) {
                Write-TextAtomic -Content $t -Path $kp -Context $Context
                & $Context.Report -Status 'Done' -Text ('Removed the keys this installer wrote from ' + (Get-RelativePathCompat $TargetDir $kp))
            }
        }
    }
}

Export-ModuleMember -Function Remove-InstallIniKeys, Write-InstallReceipt, Install-IniSettings, Convert-AddonNameToCurrent
