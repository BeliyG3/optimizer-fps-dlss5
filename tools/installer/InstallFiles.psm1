#Requires -Version 5.1
function Test-IsOurFile
{
    param([string] $Path, [string] $Kind, [hashtable] $Context)
    if (-not (Test-FileHere $Path)) { return $false }
    switch ($Kind) {
        'addon64' {
            $n = Get-PeExportNames $Path
            if ($null -eq $n) { return $false }
            return [bool](($n -contains 'OptimizerFpsSetSettingV1') -or
                (($n -contains 'PeripheralWarpSetLayoutV1') -or ($n -contains 'PeripheralWarpSetTemporalV1')))
        }
        'core' {
            if ([IO.Path]::GetFileName($Path) -ine 'optimizer-fps-dlss5-core.dll') { return $false }
            $pe = Get-PeInfo $Path
            $n = Get-PeExportNames $Path
            if ($null -eq $pe -or $pe.Arch -ne 'x64' -or $null -eq $n -or
                $n -notcontains 'OfpsCoreVersion' -or $n -notcontains 'OfpsCreateCore') { return $false }
            $version = (Get-Item -LiteralPath $Path).VersionInfo
            return [bool]($version.FileVersion -and
                $version.FileDescription -eq 'Optimizer FPS core' -and
                $version.OriginalFilename -eq 'optimizer-fps-dlss5-core.dll')
        }
        'forwarder' {
            $leaf = [IO.Path]::GetFileName($Path)
            if ($leaf -ine $Context.ForwarderFileName -and $leaf -ine $Context.LegacyForwarderFileName) { return $false }
            $n = Get-PeExportNames $Path
            if ($null -eq $n) { return $false }
            return [bool](($n -contains 'pw_ngx_call_create') -and ($n -contains 'pw_ngx_call_evaluate'))
        }
        'addon32' {
            $n = Get-PeExportNames $Path
            if ($null -ne $n -and ($n -contains 'PeripheralWarpSetLayoutV1')) { return $true }
            $m = Get-BinaryMarker $Path '(?i)(Optimizer FPS for DLSS5|PeripheralWarp)'
            return [bool]$m
        }
        'dxbc' { return $true }   # ours by location: it sits in the add-on's optimizer-fps-dlss5\ folder
    }
    return $false
}

function Get-PayloadKind
{
    param([string] $Relative, [hashtable] $Context)
    $r = $Relative.Replace('\', '/')
    $leaf = [IO.Path]::GetFileName($r)
    if ($leaf -ieq 'optimizer-fps-dlss5-core.dll') { return 'core' }
    if ($leaf -ieq $Context.ForwarderFileName -or $leaf -ieq $Context.LegacyForwarderFileName) { return 'forwarder' }
    if ($leaf -ieq $Context.RemoteFileName -or $leaf -ieq $Context.LegacyRemoteFileName)       { return 'addon32' }
    if ($r -match '(?i)\.dxbc$')              { return 'dxbc' }
    if ($r -match '(?i)\.addon64$')           { return 'addon64' }
    return 'other'
}

# Files from a release older than 26.26, when the shipped names still read "peripheral-warp".
# Anything recognisably ours is backed up and deleted; anything else keeps its name and is
# only reported. Returns the paths that were removed.
function Remove-LegacyInstall
{
    param([string] $AddonDir, [string] $RemoteDir, [string] $BackupDir, [string] $TargetDir, [hashtable] $Context)

    $removed = New-Object System.Collections.ArrayList
    $candidates = New-Object System.Collections.ArrayList

    foreach ($n in @($Context.LegacyAddonFileName, $Context.LegacyForwarderFileName)) {
        $p = Join-Safe $AddonDir $n
        $kind = 'addon64'
        if ($n -ieq $Context.LegacyForwarderFileName) { $kind = 'forwarder' }
        if (Test-FileHere $p) { $null = $candidates.Add(@{ Path = $p; Kind = $kind }) }
    }
    $legacyShaderDir = Join-Safe $AddonDir $Context.LegacyShaderFolderName
    if (Test-DirHere $legacyShaderDir) {
        foreach ($h in @(Get-ChildItem -LiteralPath $legacyShaderDir -File -Filter '*.dxbc' -ErrorAction SilentlyContinue)) {
            $null = $candidates.Add(@{ Path = $h.FullName; Kind = 'dxbc' })
        }
    }
    if ($RemoteDir) {
        $p = Join-Safe $RemoteDir $Context.LegacyRemoteFileName
        if (Test-FileHere $p) { $null = $candidates.Add(@{ Path = $p; Kind = 'addon32' }) }
    }

    foreach ($c in $candidates) {
        if (-not (Test-IsOurFile $c.Path $c.Kind -Context $Context)) {
            & $Context.Report -Status 'Warn' -Text ('A file with a pre-26.26 name is there but is not ours; left alone: ' + $c.Path)
            continue
        }
        $rel = Get-RelativePathCompat $TargetDir $c.Path
        $safeRel = $rel -replace '^[A-Za-z]:\\', '' -replace '^\\\\', ''
        try {
            Copy-FileAtomic -Source $c.Path -Destination (Join-Safe $BackupDir ('legacy\' + $safeRel)) -Context $Context
            Remove-Item -LiteralPath $c.Path -Force
            $null = $removed.Add($c.Path)
        }
        catch { & $Context.Report -Status 'Warn' -Text ('Could not remove the old-name file ' + $c.Path) -Detail $_.Exception.Message }
    }

    # The old shader folder goes with its last file.
    if (Test-DirHere $legacyShaderDir) {
        $left = @(Get-ChildItem -LiteralPath $legacyShaderDir -Force -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { try { Remove-Item -LiteralPath $legacyShaderDir -Force } catch { } }
    }

    # ... and so does a crash-guard marker under the old name.
    $legacyMarker = Join-Safe $AddonDir $Context.LegacySessionMarker
    if (Test-FileHere $legacyMarker) {
        try { Remove-Item -LiteralPath $legacyMarker -Force; $null = $removed.Add($legacyMarker) } catch { }
    }

    return @($removed.ToArray())
}


function Invoke-InstallerUninstall
{
    param([string] $LatestPath, [string] $TargetDir, [string] $AddonDir, [string] $RemoteDir, [string] $StateDir, [hashtable] $Context)
    & $Context.WriteSection 'Uninstall'

    $receipt = $null
    if (Test-FileHere $latestPath) {
        try { $receipt = (Read-TextSafe $latestPath) | ConvertFrom-Json } catch { $receipt = $null }
    }

    $removed = 0
    $kept    = 0
    $restored = 0

    if ($receipt) {
        foreach ($f in @($receipt.Files)) {
            $p = [string]$f.Path
            if (-not (Test-FileHere $p)) { continue }
            # Only a file that is still byte-for-byte the one this installer wrote may go --
            # an adopted file is no different there, and one the user has changed since is
            # theirs now. Restoring a backed-up original therefore happens only over the gap
            # our own deletion just left, never on top of a file we decided to keep.
            $now = Get-Sha256 $p
            $deleted = $false
            if ([string]::Equals($now, [string]$f.InstalledHash, [StringComparison]::OrdinalIgnoreCase)) {
                try { Remove-Item -LiteralPath $p -Force; $removed++; $deleted = $true }
                catch { & $Context.Report -Status 'Warn' -Text ('Could not remove ' + $p) -Detail $_.Exception.Message }
            }
            else {
                $kept++
                & $Context.Report -Status 'Warn' -Text ('Changed since the install, kept: ' + (Get-RelativePathCompat $targetDir $p))
            }
            $hadOriginal = $false
            if ($f.PSObject.Properties['HadOriginal']) { $hadOriginal = [bool]$f.HadOriginal }
            if ($deleted -and -not (Test-FileHere $p) -and $hadOriginal -and
                $f.PSObject.Properties['BackupPath'] -and (Test-FileHere ([string]$f.BackupPath))) {
                try { Copy-FileAtomic -Source ([string]$f.BackupPath) -Destination $p -Context $Context; $restored++ }
                catch { & $Context.Report -Status 'Warn' -Text ('Could not restore the original of ' + $p) -Detail $_.Exception.Message }
            }
        }
    }
    else {
        # No receipt: remove only what is unmistakably ours.
        $candidates = @()
        foreach ($d in @($addonDir)) {
            foreach ($n in @($Context.AddonFileName, 'optimizer-fps-dlss5-core.dll', $Context.ForwarderFileName,
                             $Context.LegacyAddonFileName, $Context.LegacyForwarderFileName)) {
                $p = Join-Safe $d $n
                if (Test-FileHere $p) { $candidates += @{ Path = $p; Kind = (Get-PayloadKind $n -Context $Context) } }
            }
            foreach ($sn in @($Context.ShaderFolderName, $Context.LegacyShaderFolderName)) {
                $sd = Join-Safe $d $sn
                if (-not (Test-DirHere $sd)) { continue }
                foreach ($h in @(Get-ChildItem -LiteralPath $sd -File -Filter '*.dxbc' -ErrorAction SilentlyContinue)) {
                    $candidates += @{ Path = $h.FullName; Kind = 'dxbc' }
                }
            }
        }
        if ($remoteDir) {
            foreach ($n in @($Context.RemoteFileName, $Context.LegacyRemoteFileName)) {
                $p = Join-Safe $remoteDir $n
                if (Test-FileHere $p) { $candidates += @{ Path = $p; Kind = 'addon32' } }
            }
        }
        if ($candidates.Count -eq 0) {
            & $Context.Report -Status 'Fail' -Text 'Nothing to uninstall: no receipt and no Optimizer FPS files found.' -Detail $targetDir
            & $Context.Exit $Context.ExitNothingToDo
        }
        & $Context.Report -Status 'Warn' -Text 'No receipt found; removing the files that are recognisably ours.'
        foreach ($c in $candidates) {
            if (-not (Test-IsOurFile $c.Path $c.Kind -Context $Context)) { $kept++; continue }
            try { Remove-Item -LiteralPath $c.Path -Force; $removed++ } catch { $kept++ }
        }
    }

    # The empty shader folder goes too -- under either name.
    foreach ($sn in @($Context.ShaderFolderName, $Context.LegacyShaderFolderName)) {
        $sd = Join-Safe $addonDir $sn
        if (-not (Test-DirHere $sd)) { continue }
        $left = @(Get-ChildItem -LiteralPath $sd -Force -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { try { Remove-Item -LiteralPath $sd -Force } catch { } }
    }

    foreach ($mn in @($Context.SessionMarker, $Context.LegacySessionMarker)) {
        $marker = Join-Safe $addonDir $mn
        if (Test-FileHere $marker) { try { Remove-Item -LiteralPath $marker -Force } catch { } }
    }

    Remove-InstallIniKeys -Receipt $receipt -TargetDir $targetDir -Context $Context

    # Whatever this installer added to Defender's exclusion list goes with it.
    if ($receipt -and $receipt.PSObject.Properties['DefenderExclusions']) {
        $ex = @($receipt.DefenderExclusions)
        if ($ex.Count -gt 0) { Remove-DefenderExclusion -Paths $ex -Context $Context }
    }

    if (Test-FileHere $latestPath) { try { Remove-Item -LiteralPath $latestPath -Force } catch { } }

    & $Context.Report -Status 'Done' -Text ('Uninstalled: ' + $removed + ' file(s) removed, ' + $restored + ' original(s) restored, ' + $kept + ' left alone.') `
           -Detail ('The backup folders under ' + $stateDir + ' are kept.')
}

function Read-InstallPayload
{
    param([string] $Payload, [string] $AddonDir, [string] $RemoteDir, [string] $Arch, [hashtable] $Context)
    & $Context.WriteSection 'Payload'

    if (-not (Test-DirHere $Payload)) {
        & $Context.Report -Status 'Fail' -Text ('Payload folder not found: ' + $Payload) `
            -Manual 'Run this script from the unpacked release folder, or pass -Payload <dir>.'
        & $Context.Exit $Context.ExitPayloadBad
    }

    $manifestPath = Join-Safe $Payload 'files.sha256'
    if (-not (Test-FileHere $manifestPath)) {
        & $Context.Report -Status 'Fail' -Text ('payload\files.sha256 is missing: ' + $manifestPath)
        & $Context.Exit $Context.ExitPayloadBad
    }

    $payloadVersion = 'unknown'
    $versionFile = Join-Safe $Payload 'VERSION.txt'
    if (Test-FileHere $versionFile) {
        $vt = Read-TextSafe $versionFile
        if ($vt) { $payloadVersion = $vt.Trim() }
    }
    $Context.Summary.Version = $payloadVersion

    $manifest = New-Object System.Collections.ArrayList
    foreach ($line in ((Read-TextSafe $manifestPath) -split "`r?`n")) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $m = [regex]::Match($t, '^([0-9a-fA-F]{64})\s+(.+)$')
        if (-not $m.Success) {
            & $Context.Report -Status 'Fail' -Text ('Malformed line in files.sha256: ' + $t)
            & $Context.Exit $Context.ExitPayloadBad
        }
        $null = $manifest.Add(@{ Hash = $m.Groups[1].Value.ToUpperInvariant(); Rel = $m.Groups[2].Value.Trim() })
    }
    if ($manifest.Count -eq 0) {
        & $Context.Report -Status 'Fail' -Text 'files.sha256 lists nothing.'
        & $Context.Exit $Context.ExitPayloadBad
    }
    $relativeNames = @($manifest | ForEach-Object { $_.Rel.Replace('\', '/').ToLowerInvariant() })
    foreach ($required in @('x64/optimizer-fps-dlss5.addon64',
                            'x64/optimizer-fps-dlss5-core.dll',
                            'x64/nvngx.dll_optimizerfps.dll',
                            'x86/optimizer-fps-dlss5-remote.addon32')) {
        if ($relativeNames -notcontains $required) {
            & $Context.Report -Status 'Fail' -Text ('Required payload entry missing: ' + $required)
            & $Context.Exit $Context.ExitPayloadBad
        }
    }
    if (@($relativeNames | Where-Object { $_ -match '^x64/optimizer-fps-dlss5/[^/]+\.dxbc$' }).Count -eq 0) {
        & $Context.Report -Status 'Fail' -Text 'Payload manifest contains no compiled shaders.'
        & $Context.Exit $Context.ExitPayloadBad
    }

    $bad = @()
    foreach ($e in $manifest) {
        $src = Join-Safe $Payload ($e.Rel -replace '/', '\')
        if (-not (Test-FileHere $src)) { $bad += ($e.Rel + ' (missing)'); continue }
        $got = Get-Sha256 $src
        if (-not [string]::Equals($got, $e.Hash, [StringComparison]::OrdinalIgnoreCase)) { $bad += ($e.Rel + ' (hash mismatch)') }
    }
    if ($bad.Count -gt 0) {
        $detail = ($bad | Select-Object -First 12) -join "`n"
        & $Context.Report -Status 'Fail' -Text ('The payload is corrupt: ' + $bad.Count + ' file(s) do not match files.sha256.') -Detail $detail `
            -Manual 'Unpack the release zip again (Windows sometimes truncates files unpacked from a blocked archive).'
        & $Context.Exit $Context.ExitPayloadBad
    }

    & $Context.Report -Status 'Ok' -Text ('Payload ' + $payloadVersion + ': ' + $manifest.Count + ' file(s) verified.') -Detail $Payload

    # Payload entry -> destination.
    $plan = New-Object System.Collections.ArrayList
    foreach ($e in $manifest) {
        $rel = $e.Rel.Replace('\', '/')
        $dest = $null
        if ($rel -match '(?i)^x64/(.+)$') {
            $dest = Join-Safe $addonDir ($Matches[1] -replace '/', '\')
        }
        elseif ($rel -match '(?i)^x86/(.+)$') {
            if ($arch -ne 'x86') { continue }
            $dest = Join-Safe $remoteDir ($Matches[1] -replace '/', '\')
        }
        else { continue }   # VERSION.txt, files.sha256 and anything else stay in the payload
        $null = $plan.Add(@{ Source = (Join-Safe $Payload ($e.Rel -replace '/', '\')); Dest = $dest; Hash = $e.Hash; Rel = $rel; Kind = (Get-PayloadKind $rel -Context $Context) })
    }

    if ($plan.Count -eq 0) {
        & $Context.Report -Status 'Fail' -Text 'The payload has nothing to install for this architecture.'
        & $Context.Exit $Context.ExitPayloadBad
    }

    $dxbcCount = @($plan | Where-Object { $_.Kind -eq 'dxbc' }).Count
    & $Context.Report -Status 'Info' -Text ('' + $plan.Count + ' file(s) to install, of which ' + $dxbcCount + ' compiled shader(s).')


    $Context.PayloadVersion = $payloadVersion
    return [pscustomobject]@{ Plan = $plan; Version = $payloadVersion }
}

function Get-InstallAdoption
{
    param([System.Collections.ArrayList] $Plan, [string] $AddonDir, [hashtable] $Context)
    & $Context.WriteSection 'Existing files'

    $foreign = @()
    foreach ($p in $plan) {
        $p['State'] = 'new'
        if (-not (Test-FileHere $p.Dest)) { continue }
        $now = Get-Sha256 $p.Dest
        if ([string]::Equals($now, $p.Hash, [StringComparison]::OrdinalIgnoreCase)) { $p['State'] = 'uptodate'; continue }
        if (Test-IsOurFile $p.Dest $p.Kind -Context $Context) { $p['State'] = 'adopt'; continue }
        $p['State'] = 'foreign'
        $foreign += $p.Dest
    }

    if ($foreign.Count -gt 0 -and -not $Context.Force) {
        & $Context.Stop -Text ('Files with our names are already there and are NOT ours: ' + $foreign.Count) `
            -Detail (($foreign | Select-Object -First 8) -join "`n") `
            -Manual 'Move them aside yourself, or re-run with -Force (the installer then backs them up as originals).'
    }
    if ($foreign.Count -gt 0) {
        & $Context.Report -Status 'Warn' -Text ('-Force: ' + $foreign.Count + ' foreign file(s) will be backed up and replaced.')
    }

    $adopted = @($plan | Where-Object { $_.State -eq 'adopt' })
    $upToDate = @($plan | Where-Object { $_.State -eq 'uptodate' })
    if ($adopted.Count -gt 0) {
        & $Context.Report -Status 'Info' -Text ('Adopting ' + $adopted.Count + ' file(s) from an earlier (manual) install.')
    }
    if ($upToDate.Count -gt 0) {
        & $Context.Report -Status 'Ok' -Text ('' + $upToDate.Count + ' file(s) already up to date.')
    }

    # Shaders that this release no longer has must go, or the add-on would keep loading them.
    $staleShaders = @()
    $shaderDir = Join-Safe $addonDir $Context.ShaderFolderName
    if (Test-DirHere $shaderDir) {
        $wanted = @{}
        foreach ($p in $plan) { if ($p.Kind -eq 'dxbc') { $wanted[[IO.Path]::GetFileName($p.Dest).ToLowerInvariant()] = $true } }
        foreach ($h in @(Get-ChildItem -LiteralPath $shaderDir -File -Filter '*.dxbc' -ErrorAction SilentlyContinue)) {
            if (-not $wanted.ContainsKey($h.Name.ToLowerInvariant())) { $staleShaders += $h.FullName }
        }
    }
    if ($staleShaders.Count -gt 0) {
        & $Context.Report -Status 'Info' -Text ('' + $staleShaders.Count + ' shader(s) from an older release will be deleted.') `
               -Detail (($staleShaders | ForEach-Object { [IO.Path]::GetFileName($_) }) -join "`n")
    }

    return [pscustomobject]@{ StaleShaders = @($staleShaders) }
}

function Install-FilePayload
{
    param([System.Collections.ArrayList] $Plan, [string[]] $StaleShaders, [string] $StateDir, [string] $TargetDir, [string] $AddonDir, [string] $RemoteDir, [string] $IniPath, [string[]] $IniPaths, [hashtable] $Context)
    & $Context.WriteSection 'Install'

    $stamp     = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $backupDir = Join-Safe $stateDir ('backup-' + $stamp)
    New-DirSafe $backupDir
    $Context.Summary.BackupDir = $backupDir

    foreach ($ip in $iniPaths) {
        if (-not (Test-FileHere $ip)) { continue }
        $tag = 'ReShade.ini.original'
        if ($ip -ne $iniPath) { $tag = 'ReShade.ini.remote.original' }
        Copy-FileAtomic -Source $ip -Destination (Join-Safe $backupDir $tag) -Context $Context
    }

    $receiptFiles = New-Object System.Collections.ArrayList
    $installed = 0
    $defenderAsked = $false

    foreach ($p in $plan) {
        $rel = Get-RelativePathCompat $targetDir $p.Dest
        if ($p.State -eq 'uptodate') {
            $null = $receiptFiles.Add([ordered]@{
                Path = $p.Dest; InstalledHash = $p.Hash; HadOriginal = $false; Adopted = $false; BackupPath = $null
            })
            continue
    }

        $hadOriginal = $false
        $backupPath  = $null
        if ($p.State -eq 'foreign') {
            # $rel is an absolute path when the file lives outside TargetDir (the x86 remote
            # tab); strip the drive so it still lands inside the backup folder.
            $safeRel = $rel -replace '^[A-Za-z]:\\', '' -replace '^\\\\', ''
            $backupPath = Join-Safe $backupDir ('originals\' + $safeRel)
            Copy-FileAtomic -Source $p.Dest -Destination $backupPath -Context $Context
            $hadOriginal = $true
    }

        try { Copy-FileAtomic -Source $p.Source -Destination $p.Dest -ExpectedSha256 $p.Hash -Context $Context }
        catch {
            if (-not (Test-FileHere $p.Dest) -and -not $defenderAsked) {
                $defenderAsked = $true
                $det = Get-DefenderDetection $p.Dest
                if ($det) { $null = Request-DefenderExclusion -Paths @($p.Dest) -File ([IO.Path]::GetFileName($p.Dest)) -Context $Context }
        }
            & $Context.Stop -Text ('Could not install ' + $rel) -Detail $_.Exception.Message
    }

        $installed++
        $null = $receiptFiles.Add([ordered]@{
            Path        = $p.Dest
            InstalledHash = $p.Hash
            HadOriginal = $hadOriginal
            Adopted     = ($p.State -eq 'adopt')
            BackupPath  = $backupPath
        })
    }

    # The folder is ours, but a file in it that this release does not ship is still the user's copy:
    # it goes to the backup folder first, and the receipt names it, so an uninstall can put it back.
    $removedStaleShaders = New-Object System.Collections.ArrayList
    foreach ($s in $staleShaders) {
        $leaf = [IO.Path]::GetFileName($s)
        $staleBackup = Join-Safe $backupDir (Join-Path 'stale-shaders' $leaf)
        try { Copy-FileAtomic -Source $s -Destination $staleBackup -Context $Context }
        catch {
            & $Context.Report -Status 'Warn' -Text ('Could not back up the stale shader ' + $leaf + '; it was kept.') -Detail $_.Exception.Message
            continue
    }
        try {
            Remove-Item -LiteralPath $s -Force
            $null = $removedStaleShaders.Add([ordered]@{ Path = $s; BackupPath = $staleBackup })
    }
        catch { & $Context.Report -Status 'Warn' -Text ('Could not delete the stale shader ' + $leaf) }
    }

    $legacyRemoved = @(Remove-LegacyInstall -AddonDir $addonDir -RemoteDir $remoteDir -BackupDir $backupDir -TargetDir $targetDir -Context $Context)
    if ($legacyRemoved.Count -gt 0) {
        & $Context.Report -Status 'Done' -Text ('' + $legacyRemoved.Count + ' file(s) from a pre-26.26 release removed (backed up first).') `
               -Detail (($legacyRemoved | ForEach-Object { [IO.Path]::GetFileName($_) }) -join "`n")
    }

    & $Context.Report -Status 'Done' -Text ('' + $installed + ' file(s) installed into ' + $addonDir + '.')
    if ($remoteDir) { & $Context.Report -Status 'Done' -Text ('The remote tab went to ' + $remoteDir + '.') }

    # A marker left behind means the previous session died right after its first warped frame,
    # and the add-on would start in pass-through. A fresh install clears it.
    $marker = Join-Safe $addonDir $Context.SessionMarker
    if (Test-FileHere $marker) {
        try { Remove-Item -LiteralPath $marker -Force; & $Context.Report -Status 'Done' -Text ('Removed the stale crash-guard marker ' + $Context.SessionMarker + '.') }
        catch { & $Context.Report -Status 'Warn' -Text ('Could not remove ' + $marker) }
    }

    $Context.BackupDir = $backupDir
    $Context.ReceiptFiles = $receiptFiles
    $Context.RemovedStaleShaders = $removedStaleShaders
    $Context.LegacyRemoved = $legacyRemoved
    return [pscustomobject]@{
        BackupDir = $backupDir
        ReceiptFiles = $receiptFiles
        RemovedStaleShaders = $removedStaleShaders
        LegacyRemoved = $legacyRemoved
    }
}

Export-ModuleMember -Function Install-FilePayload, Get-InstallAdoption, Read-InstallPayload, Invoke-InstallerUninstall, Test-IsOurFile, Get-PayloadKind, Remove-LegacyInstall
