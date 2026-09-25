#Requires -Version 5.1
function Test-Elevated
{
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    catch { return $false }
}

function ConvertTo-PsLiteral
{
    param([string] $S)
    return ("'" + ($S -replace "'", "''") + "'")
}

function Invoke-Elevated
{
    param([string] $Script, [string] $What, [hashtable] $Context)

    $body = "`$ErrorActionPreference = 'Stop'`r`ntry {`r`n" + $Script + "`r`n  exit 0`r`n}`r`ncatch {`r`n  Write-Host `$_.Exception.Message`r`n  Start-Sleep -Seconds 4`r`n  exit 1`r`n}`r`n"
    $tmp = Join-Safe ([IO.Path]::GetTempPath()) ('optimizerfps-elevated-' + [Guid]::NewGuid().ToString('N') + '.ps1')
    [IO.File]::WriteAllText($tmp, $body, (New-Object Text.UTF8Encoding($true)))

    $code = 1
    if (Test-Elevated) {
        try { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $tmp; $code = $LASTEXITCODE }
        catch { $code = 1 }
    }
    else {
        & $Context.WriteChunk ('  [ .. ] Asking for administrator rights: ' + $What) 'Cyan'
        try {
            $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru `
                     -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $tmp + '"'))
            $code = $p.ExitCode
        }
        catch {
            try { Remove-Item -LiteralPath $tmp -Force } catch { }
            return 'declined'
        }
    }
    try { Remove-Item -LiteralPath $tmp -Force } catch { }
    if ($code -eq 0) { return 'ok' }
    return 'failed'
}

# A detection in the last 15 minutes naming $Path -- or $null.
function Get-DefenderDetection
{
    param([string] $Path)
    try {
        $since = (Get-Date).AddMinutes(-15)
        $all = Get-MpThreatDetection -ErrorAction Stop
        foreach ($d in $all) {
            if ($d.InitialDetectionTime -lt $since) { continue }
            $res = @($d.Resources) -join ' '
            if ($res -match [regex]::Escape($Path)) { return $d }
        }
    }
    catch { }
    return $null
}

# An exclusion is never automatic: it weakens the user's antivirus, so it needs its own
# explicit -AllowDefenderExclusion switch. -Yes answers the installer's own questions and
# deliberately does not answer this one. Only the exact file(s) Defender removed are
# excluded -- never the folder they sit in.
function Request-DefenderExclusion
{
    param([string[]] $Paths, [string] $File, [hashtable] $Context)

    $todo = @()
    foreach ($p in $Paths) { if (-not $Context.DefenderExcluded.ContainsKey($p.ToLowerInvariant())) { $todo += $p } }
    if ($todo.Count -eq 0) { return $true }

    $cmd = ($todo | ForEach-Object { 'Add-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join '; '

    & $Context.WriteLine ''
    & $Context.WriteChunk ('  Windows Defender flagged ' + $File + '.') 'Yellow'
    & $Context.WriteChunk '  The add-on uses Microsoft Detours to hook DLSS functions, which some AV heuristics' 'DarkGray'
    & $Context.WriteChunk '  flag; if you trust this download, add an exclusion for the listed file(s) only:' 'DarkGray'
    foreach ($p in $todo) { & $Context.WriteChunk ('      ' + $p) 'White' }

    if (-not $Context.AllowDefenderExclusion) {
        & $Context.Report -Status 'Fail' -Text 'Defender removed a file this install needs; no exclusion was added.' `
               -Manual ('Add it yourself (elevated PowerShell): ' + $cmd + "`n" +
                        'or re-run this installer with -AllowDefenderExclusion, which asks the same question ' +
                        'and adds the exclusion for you.')
        return $false
    }

    if (-not (& $Context.Confirm 'Add the Defender exclusion for the file(s) above now?')) {
        & $Context.Report -Status 'Fail' -Text 'Defender exclusion declined.' `
               -Manual ('Add it yourself (elevated PowerShell): ' + $cmd + "`nThen re-run this installer.")
        return $false
    }

    $lines = ($todo | ForEach-Object { '  Add-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join "`r`n"
    $r = Invoke-Elevated -Script $lines -What 'add a Windows Defender exclusion' -Context $Context
    if ($r -eq 'ok') {
        foreach ($p in $todo) {
            $Context.DefenderExcluded[$p.ToLowerInvariant()] = $true
            $null = $Context.DefenderExclusions.Add($p)
        }
        & $Context.Report -Status 'Done' -Text 'Defender exclusion added for the listed file(s).'
        return $true
    }
    & $Context.Report -Status 'Fail' -Text ('Defender exclusion could not be added (' + $r + ').')
    return $false
}

# Undo of the above, at uninstall time.
function Remove-DefenderExclusion
{
    param([string[]] $Paths, [hashtable] $Context)

    $todo = @()
    foreach ($p in $Paths) { if ($p) { $todo += [string]$p } }
    if ($todo.Count -eq 0) { return }

    $lines = ($todo | ForEach-Object { '  Remove-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join "`r`n"
    $r = Invoke-Elevated -Script $lines -What 'remove the Windows Defender exclusion this installer added' -Context $Context
    if ($r -eq 'ok') { & $Context.Report -Status 'Done' -Text ('Removed ' + $todo.Count + ' Defender exclusion(s) this installer had added.') }
    else {
        $cmd = ($todo | ForEach-Object { 'Remove-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join '; '
        & $Context.Report -Status 'Warn' -Text ('Could not remove the Defender exclusion(s) (' + $r + ').') `
               -Detail ('Remove them yourself (elevated PowerShell): ' + $cmd)
    }
}


Export-ModuleMember -Function Test-Elevated, ConvertTo-PsLiteral, Invoke-Elevated, Get-DefenderDetection, Request-DefenderExclusion, Remove-DefenderExclusion
