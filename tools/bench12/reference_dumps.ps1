param(
      [Parameter(Mandatory = $true)] [string] $Runtime,
      [ValidateSet('auto','compute','pixel')][string] $WarpPath = 'auto',
      [string[]] $Case = @(),
      [string] $Out = 'reference_before_2026.9.1',
      [string] $Build = '',
      [int] $Frames = 240,
      [int[]] $DumpFrames = @(120, 239)
  )
  $ErrorActionPreference = 'Stop'
  if (-not $Build) { $Build = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\out\build\x64')) }
  if ($Case) { $Case = @($Case | ForEach-Object { $_ -split ',' }) }   # -File hands "a,b" over as one string
  $run = if (Test-Path -LiteralPath $Runtime -PathType Container) { (Resolve-Path -LiteralPath $Runtime).Path } else { Join-Path $PSScriptRoot $Runtime }
  if (-not (Test-Path -LiteralPath $run -PathType Container)) { throw "Runtime not found: $run (run deploy_addon.ps1 / deploy_nrhost.ps1 first)" }
  $kind = Split-Path -Leaf $run
  $out = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $run $Out }
  if (@($Case | Where-Object { $_ -like 'core_*' }).Count) {
      if ($kind -ne 'run_nrhost' -or @($Case | Where-Object { $_ -notlike 'core_*' }).Count) {
          throw 'Core cases require run_nrhost and a separate invocation'
      }
      & "$PSScriptRoot\reference_core.ps1" -Runtime $run -Out $out -Build $Build -Case $Case -Frames $Frames -DumpFrames $DumpFrames -WarpPath $WarpPath
      return
  }
  if ((Test-Path -LiteralPath $out) -and @(Get-ChildItem -LiteralPath $out -Force).Count -gt 0) {
      throw "Output is not empty: $out; choose a new -Out to preserve references"
  }
  New-Item -ItemType Directory -Force -Path $out | Out-Null
  $ini = Join-Path $run 'ReShade.ini'
  if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) { throw "No ReShade.ini in $run" }
  . "$PSScriptRoot\..\Runtime-Payload.ps1"
  Copy-CorePayload $Build $run
  $shaderDir = Join-Path $run 'optimizer-fps-dlss5'
  foreach ($name in @('pw_bench12.exe', 'nvngx.dll_pwbench12.dll')) {
      $source = Join-Path $PSScriptRoot $name
      if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing $source; run build.cmd" }
      Copy-Item -LiteralPath $source -Destination $run -Force
  }
  New-Item -ItemType Directory -Force -Path (Join-Path $run 'shaders\bin') | Out-Null
  Copy-Item -Path (Join-Path $PSScriptRoot 'shaders\bin\*.cso') -Destination (Join-Path $run 'shaders\bin') -Force
  function Set-IniKey([string] $Section, [string] $Key, [string] $Value) {
      $lines = [System.Collections.Generic.List[string]]([IO.File]::ReadAllLines($ini))
      $start = $lines.IndexOf("[$Section]")
      if ($start -lt 0) { $lines.Add(''); $lines.Add("[$Section]"); $start = $lines.Count - 1 }
      $i = $start + 1
      while ($i -lt $lines.Count -and -not $lines[$i].StartsWith('[')) {
          if ($lines[$i] -like "$Key=*") { $lines[$i] = "$Key=$Value"; [IO.File]::WriteAllLines($ini, $lines); return }
          $i++
      }
      $lines.Insert($start + 1, "$Key=$Value")
      [IO.File]::WriteAllLines($ini, $lines)
  }
  $common = @("$Frames", '--gltf', '..\..\bench\assets\lab_scene.glb', '--sun-dir', '0.45,-0.77,0.45', '--sun-strength', '1500',
              '--exposure', '0.22', '--haze', '0.008', '--fov', '62', '--dump', ($DumpFrames -join ','))
  $rr = @('--upscaler', 'rr')
  $nr = @('--upscaler', 'sr', '--nr', 'native')
  $sf = @('--mv-format', 'rgba16f', '--nr-colour-pad', '64,32', '--nr-output-pad', '64,32')
  $forward = @('--camera', 'forward', '--move-speed', '7')
  switch ($kind) {
      'run_addon' { $configs = [ordered]@{
          'off'             = @{ Mode = '0'; Temporal = '0'; Reno = @{}; Extra = $rr }
          'warp'            = @{ Mode = '2'; Temporal = '0'; Reno = @{}; Extra = $rr }
          'warp_repeat'     = @{ Mode = '2'; Temporal = '0'; Reno = @{}; Extra = $rr }
          'uniform'         = @{ Mode = '1'; Temporal = '0'; Reno = @{}; Extra = $rr }
          'warp_forward'    = @{ Mode = '2'; Temporal = '0'; Reno = @{}; Extra = $rr + $forward }
          'warp_t1'         = @{ Mode = '2'; Temporal = '1'; Reno = @{}; Extra = $rr }
          'warp_forward_t1' = @{ Mode = '2'; Temporal = '1'; Reno = @{}; Extra = $rr + $forward }
      } }
      'run_r521' { $configs = [ordered]@{
          'native_off'         = @{ Mode = '0'; Temporal = '0'; Reno = @{ NRFollowInputRes = '0' }; Extra = $rr }
          'native_warp'        = @{ Mode = '2'; Temporal = '0'; Reno = @{ NRFollowInputRes = '0' }; Extra = $rr }
          'native_warp_repeat' = @{ Mode = '2'; Temporal = '0'; Reno = @{ NRFollowInputRes = '0' }; Extra = $rr }
          'native_warp_rgba'   = @{ Mode = '2'; Temporal = '0'; Reno = @{ NRFollowInputRes = '0' }; Extra = $rr + @('--mv-format', 'rgba16f') }
          'native_uniform'     = @{ Mode = '1'; Temporal = '0'; Reno = @{ NRFollowInputRes = '0' }; Extra = $rr }
          'follow_warp'        = @{ Mode = '2'; Temporal = '0'; Reno = @{ NRFollowInputRes = '1' }; Extra = $rr + @('--mv-format', 'rgba16f') }
          'scaled_warp'        = @{ Mode = '2'; Temporal = '0'; Reno = @{ NRFollowInputRes = '2'; NRResolutionScale = '60' }; Extra = $rr + @('--mv-format', 'rgba16f') }
          'presr_warp'         = @{ Mode = '2'; Temporal = '0'; Reno = @{ NRFollowInputRes = '0'; NRPreUpscale = '1' }; Extra = $rr + @('--mv-format', 'rgba16f') }
          'native_warp_t1'     = @{ Mode = '2'; Temporal = '1'; Reno = @{ NRFollowInputRes = '0' }; Extra = $rr + @('--mv-format', 'rgba16f') }
      } }
      'run_nrhost' { $configs = [ordered]@{
          'off_t0'            = @{ Mode = '0'; Temporal = '0'; Extra = $nr + $sf }
          'warp_t0'           = @{ Mode = '2'; Temporal = '0'; Extra = $nr + $sf }
          'warp_t0_repeat'    = @{ Mode = '2'; Temporal = '0'; Extra = $nr + $sf }
          'warp_rg_cpad'      = @{ Mode = '2'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rg16f', '--nr-colour-pad', '64,32', '--nr-output-pad', '64,32') }
          'warp_rgba_nopad'   = @{ Mode = '2'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rgba16f') }
          'warp_rg_cpad_edge' = @{ Mode = '2'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rg16f', '--nr-colour-pad', '64,32,1') }
          'warp_rg_nopad'     = @{ Mode = '2'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rg16f') }
          'uni_black'         = @{ Mode = '1'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rg16f', '--nr-colour-pad', '64,32') }
          'uni_edge'          = @{ Mode = '1'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rg16f', '--nr-colour-pad', '64,32,1') }
          'uni_nopad'         = @{ Mode = '1'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rg16f') }
          'off_nopad'         = @{ Mode = '0'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rgba16f') }
          'off_cpad_only'     = @{ Mode = '0'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rgba16f', '--nr-colour-pad', '64,32') }
          'warp_t1'           = @{ Mode = '2'; Temporal = '1'; Extra = $nr + $sf }
      } }
      default { throw "Unknown runtime kind '$kind': expected run_addon, run_r521 or run_nrhost" }
  }
  foreach ($name in $Case) { if (-not $configs.Contains($name)) { throw "Unknown case '$name' for $kind; known: $($configs.Keys -join ', ')" } }
  $repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
  $binaries = [ordered]@{}
  foreach ($name in @('optimizer-fps-dlss5-core.dll', 'optimizer-fps-dlss5.addon64', 'pw_bench12.exe', 'nvngx.dll_pwbench12.dll', 'dxgi.dll', 'renodx-dlss5.addon64',
                      'nvngx.dll_optimizerfps.dll', 'nvngx_dlssnr.dll', 'nvngx_dlssd.dll', 'nvngx_dlss.dll')) {
      $path = Join-Path $run $name
      if (Test-Path -LiteralPath $path -PathType Leaf) { $binaries[$name] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
  }
  foreach ($dxbc in Get-ChildItem -LiteralPath $shaderDir -Filter '*.dxbc') {
      $binaries['optimizer-fps-dlss5/' + $dxbc.Name] = (Get-FileHash -LiteralPath $dxbc.FullName -Algorithm SHA256).Hash
  }
  $sourceSha = (& git --no-optional-locks -C $repo rev-parse HEAD)
  $sourceStatus = @(& git --no-optional-locks -C $repo status --porcelain=v1 --untracked-files=all)
  $sourceHashes = [ordered]@{}
  foreach ($relative in (& git -C $repo -c core.quotepath=false ls-files --cached --others --exclude-standard | Sort-Object -Unique)) {
      $path = Join-Path $repo $relative
      if (Test-Path -LiteralPath $path -PathType Leaf) {
          $sourceHashes[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
      }
  }
  $runs = New-Object System.Collections.Generic.List[object]
  foreach ($name in $configs.Keys) {
      if ($Case.Count -gt 0 -and $Case -notcontains $name) { continue }
      $cfg = $configs[$name]
      Set-IniKey 'OptimizerFPS' 'CrashGuard' '0'      # the bench leaves through TerminateProcess
      Set-IniKey 'OptimizerFPS' 'Mode' $cfg.Mode
      $warpId = @{ auto='0'; compute='1'; pixel='2' }[$WarpPath]
      Set-IniKey 'OptimizerFPS' 'DebugWarpPath' $warpId
      Set-IniKey 'OptimizerFPS' 'TemporalMode' $cfg.Temporal
      Set-IniKey 'OptimizerFPS' 'TemporalEvery' '4'
      Set-IniKey 'OptimizerFPS' 'ModelPasses' '1'
      Set-IniKey 'OptimizerFPS' 'SpreadPasses' '0'
      foreach ($key in @('CenterX', 'CenterY')) { Set-IniKey 'OptimizerFPS' $key '80' }
      foreach ($key in @('WorkX', 'WorkY')) { Set-IniKey 'OptimizerFPS' $key '90' }
      Set-IniKey 'OptimizerFPS' 'GlobalScale' '100'
      Set-IniKey 'OptimizerFPS' 'ColorFilter' '1'
      Set-IniKey 'OptimizerFPS' 'Flags' '0' # same motion boundary policy in all runtimes
      if ($cfg.ContainsKey('Reno')) {                    # renodx runtimes; run_addon's 4.70 knows only NeuralUplift
          Set-IniKey 'RenoDX.DLSS5' 'NeuralUplift' '1'
          if ($kind -eq 'run_r521') { Set-IniKey 'RenoDX.DLSS5' 'NRPreUpscale' '0'; Set-IniKey 'RenoDX.DLSS5' 'NRResolutionScale' '100' }
          foreach ($k in $cfg.Reno.Keys) { Set-IniKey 'RenoDX.DLSS5' $k $cfg.Reno[$k] }
      }
      Remove-Item -LiteralPath (Join-Path $run 'optimizer-fps-dlss5.session') -ErrorAction SilentlyContinue
      Remove-Item -LiteralPath (Join-Path $run 'ReShade.log') -ErrorAction SilentlyContinue
      Remove-Item -LiteralPath (Join-Path $run 'ngx_final_audit.log') -ErrorAction SilentlyContinue
      Get-ChildItem -LiteralPath $run -Filter 'dump_*.bmp' | Remove-Item
      $iniCopy = Join-Path $out ($name + '_ReShade.ini')
      Copy-Item -LiteralPath $ini -Destination $iniCopy
      $argv = $common + $cfg.Extra
      Write-Output ((@('pw_bench12.exe') + $argv) -join ' ')
      $p = Start-Process -FilePath (Join-Path $run 'pw_bench12.exe') -ArgumentList $argv -WorkingDirectory $run -Wait -PassThru -WindowStyle Hidden `
          -RedirectStandardOutput (Join-Path $out "$name.txt") -RedirectStandardError (Join-Path $out "$name.err.txt")
      foreach ($f in @($DumpFrames | ForEach-Object { "dump_$_.bmp" })) {
          $src = Join-Path $run $f
          if (Test-Path -LiteralPath $src) { Move-Item -LiteralPath $src -Destination (Join-Path $out ("{0}_{1}" -f $name, $f)) -Force }
      }
      $log = Join-Path $run 'ReShade.log'
      $logText = ''
      if (Test-Path -LiteralPath $log -PathType Leaf) {
          Copy-Item -LiteralPath $log -Destination (Join-Path $out "$name`_ReShade.log")
          $logText = [IO.File]::ReadAllText($log)
      }
      $audit = Join-Path $run 'ngx_final_audit.log'
      if (Test-Path -LiteralPath $audit) { Copy-Item -LiteralPath $audit -Destination (Join-Path $out "$name`_ngx_audit.log") }
      $stdout = [IO.File]::ReadAllText((Join-Path $out "$name.txt"))
      $warped = $logText.Contains('first warped evaluate completed')
      $effectiveWarpPath = if ($logText.Contains('compute path ready')) { 'compute' } `
          elseif ($logText.Contains('pixel path:')) { 'pixel' } else { 'none' }
      $ready = $logText.Contains('temporal machine ready')
      $dumps = [ordered]@{}
      foreach ($frame in $DumpFrames) {
          $dump = Join-Path $out ("{0}_dump_{1}.bmp" -f $name, $frame)
          $dumps[[string]$frame] = Test-Path -LiteralPath $dump -PathType Leaf
      }
      $pathMatches = $WarpPath -eq 'auto' -or $cfg.Mode -eq '0' -or $effectiveWarpPath -eq $WarpPath
      $ok = $dumps.Count -gt 0 -and ($dumps.Values -notcontains $false) -and ($p.ExitCode -eq 0) -and $stdout.Contains('device ok') -and (($cfg.Mode -eq '0') -or $warped) -and (($cfg.Temporal -eq '0') -or $ready) -and $pathMatches
      $runs.Add([ordered]@{ name = $name; argv = $argv; working_directory = $run; command = ((@('pw_bench12.exe') + $argv) -join ' '); mode = $cfg.Mode; temporal = $cfg.Temporal; warp_path = $WarpPath; effective_warp_path = $effectiveWarpPath
                            ini = (Split-Path -Leaf $iniCopy); dumps = $dumps; exit = $p.ExitCode; warped = $warped; ready = $ready; ok = $ok })
      '{0,-20} exit {1} warped {2} ready {3} {4}' -f $name, $p.ExitCode, $warped, $ready, $(if ($ok) { 'ok' } else { 'CHECK' })
  }
  $manifest = [ordered]@{
      label = (Split-Path -Leaf $out); runtime = $run; created = (Get-Date).ToString('s')
      git = (& git -C $repo describe --tags --always --dirty)
      source_sha = $sourceSha; dirty = ($sourceStatus.Count -gt 0); source_status = $sourceStatus; source_sha256 = $sourceHashes
      binaries = $binaries; runs = $runs.ToArray()
  }
  $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'manifest.json') -Encoding UTF8
  Write-Output "Reference: $out"
if (@($runs | Where-Object { -not $_.ok }).Count -gt 0) { throw 'One or more reference cases failed' }
