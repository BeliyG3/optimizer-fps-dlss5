<#
.SYNOPSIS
    Checks PowerShell sources for constructs that Windows PowerShell 5.1 cannot run.

.DESCRIPTION
    PeripheralWarp ships scripts that have to run on a stock Windows box, i.e. on
    Windows PowerShell 5.1 - no PowerShell 7 install required. This linter walks
    every *.ps1 / *.psm1 below -Path and reports:

      1. Parse errors (System.Management.Automation.Language.Parser::ParseFile).
         Note: the parser used is the one of the *host* that runs this script, so
         running the linter under powershell.exe (5.1) gives the strictest result.
      2. Grep-style rules with line numbers for APIs and syntax added after 5.1:
           [IO.Path]::GetRelativePath        (.NET Core / netstandard2.1 only)
           [Convert]::ToHexString            (.NET 5+)
           ConvertFrom-Json -AsHashtable     (PowerShell 6+)
           ??  /  ??=                        (null coalescing, PowerShell 7)
           $x?.Member  /  $x?[0]             (null conditional, PowerShell 7)
           Join-Path with 3+ positional args (PowerShell 6+)
           #Requires -Version 7              (locks the script out of 5.1)
         Ternary "a ? b : c" is deliberately NOT checked: no heuristic separates it
         reliably from wildcards and command names, and it is a parse error under
         5.1 anyway, so rule 1 catches it.
      3. PSScriptAnalyzer's PSUseCompatibleSyntax against 5.1, when the module is
         installed. Otherwise a notice is printed and the run continues.

    Exit code is 1 if anything was found, 0 otherwise.

.PARAMETER Path
    One or more files or directories. Directories are searched recursively.

.EXAMPLE
    powershell.exe -NoProfile -File tools\Lint-PowerShell51.ps1 -Path tools

.EXAMPLE
    powershell.exe -NoProfile -File tools\Lint-PowerShell51.ps1 -Path tools\Verify-Dependencies.ps1,tools\Lint-PowerShell51.ps1
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]]$Path
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:Findings = New-Object System.Collections.ArrayList

function Add-Finding {
    param(
        [Parameter(Mandatory = $true)][string]$File,
        [Parameter(Mandatory = $true)][int]$Line,
        [Parameter(Mandatory = $true)][string]$Rule,
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Text = ''
    )

    $null = $script:Findings.Add([pscustomobject]@{
        File    = $File
        Line    = $Line
        Rule    = $Rule
        Message = $Message
        Text    = $Text.Trim()
    })
}

# Blank out comments and string bodies so the regex rules below do not fire on
# documentation or on data. Deliberately a heuristic: a single left-to-right pass
# with quote tracking, plus <# #> block-comment tracking carried across lines.
function Remove-CommentsAndStrings {
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines)

    $result = New-Object System.Collections.ArrayList
    $inBlockComment = $false
    $inHereString = $false

    foreach ($line in $Lines) {
        if ($inHereString) {
            $null = $result.Add('')
            if ($line -match '^\s*[''"]@') { $inHereString = $false }
            continue
        }

        $builder = New-Object System.Text.StringBuilder
        $quote = [char]0
        $i = 0
        while ($i -lt $line.Length) {
            $c = $line[$i]
            $next = if ($i + 1 -lt $line.Length) { $line[$i + 1] } else { [char]0 }

            if ($inBlockComment) {
                if ($c -eq '#' -and $next -eq '>') { $inBlockComment = $false; $i += 2 } else { $i++ }
                $null = $builder.Append(' ')
                continue
            }

            if ($quote -ne [char]0) {
                if ($c -eq '`' -and $quote -eq '"') { $i += 2; $null = $builder.Append('__'); continue }
                if ($c -eq $quote) {
                    if ($next -eq $quote) { $i += 2; $null = $builder.Append('__'); continue }
                    # Keep the delimiters: the Join-Path rule still has to see a
                    # quoted argument, it just must not see what is inside it.
                    $quote = [char]0
                    $null = $builder.Append($c)
                    $i++
                    continue
                }
                $null = $builder.Append('_')
                $i++
                continue
            }

            if ($c -eq '<' -and $next -eq '#') { $inBlockComment = $true; $i += 2; $null = $builder.Append('  '); continue }
            if ($c -eq '#') { break }  # line comment: drop the rest
            if ($c -eq '@' -and ($next -eq '"' -or $next -eq "'")) {
                # start of a here-string: everything up to the terminator is data
                $inHereString = $true
                break
            }
            if ($c -eq '"' -or $c -eq "'") { $quote = $c; $null = $builder.Append($c); $i++; continue }

            $null = $builder.Append($c)
            $i++
        }

        $null = $result.Add($builder.ToString())
    }

    return , $result.ToArray()
}

# Splits the arguments that follow a command name, honouring quotes, and stops at
# a pipeline / statement separator. Used by the Join-Path rule only.
function Split-CommandArguments {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)

    $arguments = New-Object System.Collections.ArrayList
    $index = 0
    while ($index -lt $Text.Length) {
        while ($index -lt $Text.Length -and [char]::IsWhiteSpace($Text[$index])) { $index++ }
        if ($index -ge $Text.Length) { break }

        # An argument runs until the next whitespace that sits outside any (), [] or
        # quotes; "(Join-Path $a $b).Trim()" is therefore one argument, not three.
        $start = $index
        $round = 0
        $square = 0
        $quote = [char]0
        $stopped = $false
        while ($index -lt $Text.Length) {
            $c = $Text[$index]
            if ($quote -ne [char]0) {
                if ($c -eq $quote) { $quote = [char]0 }
                $index++
                continue
            }
            if ($c -eq '"' -or $c -eq "'") { $quote = $c; $index++; continue }
            if ($c -eq '(') { $round++; $index++; continue }
            if ($c -eq '[') { $square++; $index++; continue }
            if ($c -eq ')') {
                if ($round -eq 0) { $stopped = $true; break }   # end of the enclosing call
                $round--; $index++; continue
            }
            if ($c -eq ']') {
                if ($square -eq 0) { $stopped = $true; break }
                $square--; $index++; continue
            }
            if ($round -eq 0 -and $square -eq 0) {
                if ([char]::IsWhiteSpace($c)) { break }
                if ($c -eq '|' -or $c -eq ';' -or $c -eq '}' -or $c -eq ',') { $stopped = $true; break }
            }
            $index++
        }

        if ($index -gt $start) { $null = $arguments.Add($Text.Substring($start, $index - $start)) }
        if ($stopped) { break }
    }
    return , $arguments.ToArray()
}

$rules = @(
    @{ Rule = 'net-core-api'; Pattern = '\[\s*(System\.)?IO\.Path\s*\]\s*::\s*GetRelativePath'
       Message = '[IO.Path]::GetRelativePath does not exist on .NET Framework 4.x (5.1). Compute the relative path manually or use Resolve-Path.' }
    @{ Rule = 'net5-api'; Pattern = '\[\s*(System\.)?Convert\s*\]\s*::\s*ToHexString'
       Message = '[Convert]::ToHexString is .NET 5+. Use [BitConverter]::ToString($bytes).Replace(''-'','''').' }
    @{ Rule = 'pwsh-parameter'; Pattern = '(?i)ConvertFrom-Json\b[^|;]*?\s-AsHashtable\b'
       Message = 'ConvertFrom-Json -AsHashtable is PowerShell 6+. Walk the PSCustomObject with .PSObject.Properties instead.' }
    @{ Rule = 'pwsh-syntax'; Pattern = '\?\?=?(?![\w])'
       Message = 'Null-coalescing (?? / ??=) is PowerShell 7. Use an if/else or a default assignment.' }
    @{ Rule = 'pwsh-syntax'; Pattern = '\$(\{[^}]+\}|[\w:.\[\]]+)\?(\.|\[)'
       Message = 'Null-conditional access ($x?.Member / $x?[0]) is PowerShell 7. Test for $null explicitly.' }
)

function Test-File {
    param([Parameter(Mandatory = $true)][string]$FilePath)

    $display = $FilePath
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($FilePath, [ref]$tokens, [ref]$errors)
    if ($errors -and $errors.Count -gt 0) {
        foreach ($parseError in $errors) {
            Add-Finding -File $display -Line $parseError.Extent.StartLineNumber -Rule 'parse-error' `
                -Message $parseError.Message -Text $parseError.Extent.Text
        }
    }

    $rawLines = @(Get-Content -LiteralPath $FilePath)
    if ($rawLines.Count -eq 0) { return }
    $codeLines = Remove-CommentsAndStrings -Lines $rawLines

    # #Requires is a comment to the tokenizer, so let the parser report it: that also
    # keeps a "#Requires" written inside a <# #> help block from being flagged.
    if ($ast -and $ast.ScriptRequirements -and $ast.ScriptRequirements.RequiredPSVersion -and
        $ast.ScriptRequirements.RequiredPSVersion.Major -ge 6) {
        $requiresLine = 1
        for ($r = 0; $r -lt $rawLines.Count; $r++) {
            if ($rawLines[$r] -match '(?i)^\s*#requires\b') { $requiresLine = $r + 1; break }
        }
        Add-Finding -File $display -Line $requiresLine -Rule 'requires-version' `
            -Message ("#Requires -Version {0} prevents the script from running on Windows PowerShell 5.1." -f $ast.ScriptRequirements.RequiredPSVersion) `
            -Text $rawLines[$requiresLine - 1]
    }

    for ($i = 0; $i -lt $rawLines.Count; $i++) {
        $lineNumber = $i + 1
        $raw = $rawLines[$i]
        $code = $codeLines[$i]

        if ([string]::IsNullOrWhiteSpace($code)) { continue }

        foreach ($rule in $rules) {
            if ($code -match $rule.Pattern) {
                Add-Finding -File $display -Line $lineNumber -Rule $rule.Rule -Message $rule.Message -Text $raw
            }
        }

        # Join-Path took a single -ChildPath until PowerShell 6; three or more
        # positional arguments only work on pwsh.
        foreach ($joinMatch in [regex]::Matches($code, '(?i)(^|[\s|;({])Join-Path\b')) {
            $tail = $code.Substring($joinMatch.Index + $joinMatch.Length)
            $arguments = Split-CommandArguments -Text $tail
            $positional = 0
            foreach ($argument in $arguments) {
                if ($argument.StartsWith('-')) { break }
                $positional++
            }
            if ($positional -ge 3) {
                Add-Finding -File $display -Line $lineNumber -Rule 'pwsh-parameter' `
                    -Message 'Join-Path with 3+ positional arguments is PowerShell 6+. Nest two-argument Join-Path calls.' -Text $raw
            }
        }
    }
}

# --- collect the files -----------------------------------------------------
$files = New-Object System.Collections.ArrayList
foreach ($entry in $Path) {
    if (-not (Test-Path -LiteralPath $entry)) {
        throw "Path does not exist: $entry"
    }
    $item = Get-Item -LiteralPath $entry
    if ($item.PSIsContainer) {
        $found = @(Get-ChildItem -LiteralPath $item.FullName -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -eq '.ps1' -or $_.Extension -eq '.psm1' })
        foreach ($file in $found) { $null = $files.Add($file.FullName) }
    }
    elseif ($item.Extension -in @('.ps1', '.psm1')) {
        $null = $files.Add($item.FullName)
    }
    else {
        Write-Warning "Skipping (not a .ps1/.psm1 file): $($item.FullName)"
    }
}

$unique = @($files | Sort-Object -Unique)
if ($unique.Count -eq 0) {
    Write-Host 'Lint-PowerShell51: no PowerShell files to check.'
    exit 0
}

Write-Host "Lint-PowerShell51: checking $($unique.Count) file(s) with PowerShell $($PSVersionTable.PSVersion)."
foreach ($file in $unique) { Test-File -FilePath $file }

# --- PSScriptAnalyzer ------------------------------------------------------
$analyzer = @(Get-Module -ListAvailable -Name PSScriptAnalyzer -ErrorAction SilentlyContinue)
if ($analyzer.Count -gt 0) {
    Import-Module PSScriptAnalyzer -ErrorAction Stop
    $settings = @{
        IncludeRules = @('PSUseCompatibleSyntax')   # compatibility only; style rules are not this linter's job
        Rules = @{
            PSUseCompatibleSyntax = @{
                Enable         = $true
                TargetVersions = @('5.1')
            }
        }
    }
    foreach ($file in $unique) {
        $results = @(Invoke-ScriptAnalyzer -Path $file -Settings $settings -ErrorAction Stop)
        foreach ($result in $results) {
            Add-Finding -File $file -Line $result.Line -Rule "PSScriptAnalyzer/$($result.RuleName)" `
                -Message $result.Message -Text ([string]$result.Extent.Text)
        }
    }
}
else {
    Write-Host 'Lint-PowerShell51: PSScriptAnalyzer is not installed; PSUseCompatibleSyntax was skipped.'
    Write-Host '                   Install it with: Install-Module PSScriptAnalyzer -Scope CurrentUser'
}

# --- report ----------------------------------------------------------------
if ($script:Findings.Count -eq 0) {
    Write-Host 'Lint-PowerShell51: OK, nothing incompatible with Windows PowerShell 5.1 found.'
    exit 0
}

foreach ($finding in ($script:Findings | Sort-Object File, Line)) {
    Write-Host ("{0}({1}): [{2}] {3}" -f $finding.File, $finding.Line, $finding.Rule, $finding.Message)
    if ($finding.Text) {
        Write-Host ("    {0}" -f $finding.Text)
    }
}
Write-Host ''
Write-Host ("Lint-PowerShell51: {0} finding(s)." -f $script:Findings.Count)
exit 1
