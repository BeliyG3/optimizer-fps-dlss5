#Requires -Version 5.1
function Set-IniKey
{
    param([string] $Text, [string] $Section, [string] $Key, [string] $Value)

    if ($null -eq $Text) { $Text = '' }
    $nl = "`r`n"
    $lines = New-Object System.Collections.ArrayList
    foreach ($l in ($Text -split "`r?`n")) { $null = $lines.Add($l) }
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

    $cur = ''
    $secStart = -1
    $secEnd = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\[(.+)\]$') {
            if ($cur -ieq $Section -and $secStart -ge 0 -and $secEnd -lt 0) { $secEnd = $i }
            $cur = $Matches[1]
            if ($cur -ieq $Section) { $secStart = $i + 1 }
            continue
        }
        if ($cur -ieq $Section -and $t -match ('(?i)^' + [regex]::Escape($Key) + '\s*=')) {
            $lines[$i] = $Key + '=' + $Value
            return (($lines -join $nl) + $nl)
        }
    }
    if ($Section -eq '' -and $secStart -lt 0) {
        $secStart = 0; $secEnd = 0
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -match '^\[') { break }; $secEnd = $i + 1 }
    }
    if ($secStart -ge 0) {
        if ($secEnd -lt 0) { $secEnd = $lines.Count }
        $at = $secEnd
        while ($at -gt $secStart -and $lines[$at - 1].Trim() -eq '') { $at-- }
        $lines.Insert($at, ($Key + '=' + $Value))
    }
    else {
        if ($lines.Count -gt 0) { $null = $lines.Add('') }
        $null = $lines.Add('[' + $Section + ']')
        $null = $lines.Add($Key + '=' + $Value)
    }
    return (($lines -join $nl) + $nl)
}

function Get-IniKey
{
    param([string] $Text, [string] $Section, [string] $Key)
    if ($null -eq $Text) { return $null }
    $cur = ''
    foreach ($l in ($Text -split "`r?`n")) {
        $t = $l.Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
        if ($cur -ieq $Section -and $t -match ('(?i)^' + [regex]::Escape($Key) + '\s*=\s*(.*)$')) { return $Matches[1] }
    }
    return $null
}

function Remove-IniKey
{
    param([string] $Text, [string] $Section, [string] $Key)
    if ($null -eq $Text) { return $null }
    $nl = "`r`n"
    $lines = New-Object System.Collections.ArrayList
    foreach ($l in ($Text -split "`r?`n")) { $null = $lines.Add($l) }
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

    $cur = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
        if ($cur -ieq $Section -and $t -match ('(?i)^' + [regex]::Escape($Key) + '\s*=')) {
            $lines.RemoveAt($i)
            return (($lines -join $nl) + $nl)
        }
    }
    return (($lines -join $nl) + $nl)
}

# Drops "[Section]" when nothing but blank lines follows it.
function Remove-EmptyIniSection
{
    param([string] $Text, [string] $Section)
    if ($null -eq $Text) { return $null }
    $nl = "`r`n"
    $lines = New-Object System.Collections.ArrayList
    foreach ($l in ($Text -split "`r?`n")) { $null = $lines.Add($l) }
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\[(.+)\]$' -and $Matches[1] -ieq $Section) { $start = $i; break }
    }
    if ($start -lt 0) { return (($lines -join $nl) + $nl) }

    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -match '^\[.+\]$') { $end = $i; break }
    }
    for ($i = $start + 1; $i -lt $end; $i++) {
        if ($lines[$i].Trim() -ne '') { return (($lines -join $nl) + $nl) }
    }
    # Take the blank lines that were inserted in front of the header with it, so removing a
    # section this installer added leaves the file exactly as it was before.
    $from = $start
    while ($from -gt 0 -and $lines[$from - 1].Trim() -eq '') { $from-- }
    for ($i = $end - 1; $i -ge $from; $i--) { $lines.RemoveAt($i) }
    return (($lines -join $nl) + $nl)
}


Export-ModuleMember -Function Set-IniKey, Get-IniKey, Remove-IniKey, Remove-EmptyIniSection
