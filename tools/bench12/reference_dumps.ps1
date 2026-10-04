param(
      [Parameter(Mandatory = $true)] [string] $Runtime,
      [ValidateSet('auto','compute','pixel')][string] $WarpPath = 'auto',
      [string[]] $Case = @(),
      [string] $Out = 'reference_before_2026.9.1',
      [string] $Build = '',
      [int] $Frames = 240,
      [string[]] $DumpFrames = @('120', '239')   # strings: -File hands "a,b" over as one ([int[]] would read 240479)
  )
  $ErrorActionPreference = 'Stop'
  if (-not $Build) { $Build = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\out\build\x64')) }
  if ($Case) { $Case = @($Case | ForEach-Object { $_ -split ',' }) }   # -File hands "a,b" over as one string
  $DumpFrames = @($DumpFrames | ForEach-Object { $_ -split ',' } | ForEach-Object { [string][int]$_ })
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
  . "$PSScriptRoot\menu_check.ps1"
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
  # argv = frames + scene + --dump + case extras; a case may override Frames and Dump (menu-mode spike cases).
  $scene = @('--gltf', '..\..\bench\assets\lab_scene.glb', '--sun-dir', '0.45,-0.77,0.45', '--sun-strength', '1500',
             '--exposure', '0.22', '--haze', '0.008', '--fov', '62')
  $rr = @('--upscaler', 'rr')
  $nr = @('--upscaler', 'sr', '--nr', 'native')
  $nrr = @('--upscaler', 'rr', '--nr', 'native')   # Ray Reconstruction, then the model at output size
  $sf = @('--mv-format', 'rgba16f', '--nr-colour-pad', '64,32', '--nr-output-pad', '64,32')
  $forward = @('--camera', 'forward', '--move-speed', '7')
  $face = @('--camera', 'static', '--anim-speed', '0', '--fov', '30', '--cam-pos', '0.35,1.68,-5.45', '--cam-target', '0.35,1.64,-6.07')
  $mpPause = @('--nr-pause', '60,90,120,150,180,210,240,270,300,330,360,390,420,450,480,510,540,570,600,630')
  # Model cases: a paused frame presents its own NR input (a menu drawn without NR), static or moving camera.
  $mpShow = @('--nr-pause-show', 'input')
  $mpStatic = @('--camera', 'static')
  $mpMoving = @('--camera', 'forward')
  $mpMatched = @('--tonemap', 'none', '--bloom', '0')
  $mpCompute = @('--nr-list', 'compute')
  # A reference dumps at the union of the listed cases' logged indices (those run earlier in the invocation).
  $mpStaticModels = @('mp_model', 'mp_model_dbg', 'mp_recreate', 'mp_resize')
  # C7: a lifetime event at frame 80, inside the first pause while its run is under way, the process keeps running; the 9
  # later pauses must enter and get passes. The debug layer is on: an error ends the bench with an exception (no
  # 'device ok'); all its messages go to <case>.err.txt as [d3d12:<severity>] lines (0 corruption, 1 error, 2 warning).
  $mpLife = $nr + $mpStatic + $mpPause + $mpShow + @('--debug-layer')
  function Join-Ini([hashtable] $Base, [hashtable] $Over) { $all = $Base.Clone(); foreach ($k in $Over.Keys) { $all[$k] = $Over[$k] }; $all }
  # Menu mode on (MenuMode=1) with the product pass; DebugMenuPass 1 marker, 2 forced refusal, 3 forced f2 failure.
  $mpModelIni = @{ MenuMode = '1'; DebugMenuDump = '1' }
  $mpOffIni = @{ MenuMode = '0'; DebugMenuDump = '0' }
  # pp_*: the host's own evaluates. MenuMode=1 traces the runtime's reads (frames unchanged); DebugMenuOwnBlock=1 runs
  # them with menu mode's own block. Compare with pair_psnr.py against pp_ref.
  $pp = @{ OptIn = $true; Mode = '0'; Temporal = '0' }
  switch ($kind) {
      'run_addon' { $configs = [ordered]@{
          'off'             = @{ Mode = '0'; Temporal = '0'; Reno = @{}; Extra = $rr }
          'warp'            = @{ Mode = '2'; Temporal = '0'; Reno = @{}; Extra = $rr }
          'warp_repeat'     = @{ Mode = '2'; Temporal = '0'; Reno = @{}; Extra = $rr }
          'uniform'         = @{ Mode = '1'; Temporal = '0'; Reno = @{}; Extra = $rr }
          'warp_forward'    = @{ Mode = '2'; Temporal = '0'; Reno = @{}; Extra = $rr + $forward }
          'warp_t1'         = @{ Mode = '2'; Temporal = '1'; Reno = @{}; Extra = $rr }
          'warp_forward_t1' = @{ Mode = '2'; Temporal = '1'; Reno = @{}; Extra = $rr + $forward }
          'pp_ref'          = Join-Ini $pp @{ Reno = @{}; Extra = $rr; Ini = @{ MenuMode = '0' } }
          'pp_ref_repeat'   = Join-Ini $pp @{ Reno = @{}; Extra = $rr; Ini = @{ MenuMode = '0' } }
          'pp_trace'        = Join-Ini $pp @{ Reno = @{}; Extra = $rr; Ini = @{ MenuMode = '1' } }
          'pp_own'          = Join-Ini $pp @{ Reno = @{}; Extra = $rr; Ini = @{ MenuMode = '1'; DebugMenuOwnBlock = '1' } }
          'pp_own_dbg'      = Join-Ini $pp @{ Reno = @{}; Extra = $rr + @('--debug-layer'); Ini = @{ MenuMode = '1'; DebugMenuOwnBlock = '1' } }
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
          'pp_ref'             = Join-Ini $pp @{ Reno = @{ NRFollowInputRes = '0' }; Extra = $rr; Ini = @{ MenuMode = '0' } }
          'pp_ref_repeat'      = Join-Ini $pp @{ Reno = @{ NRFollowInputRes = '0' }; Extra = $rr; Ini = @{ MenuMode = '0' } }
          'pp_trace'           = Join-Ini $pp @{ Reno = @{ NRFollowInputRes = '0' }; Extra = $rr; Ini = @{ MenuMode = '1' } }
          'pp_own'             = Join-Ini $pp @{ Reno = @{ NRFollowInputRes = '0' }; Extra = $rr; Ini = @{ MenuMode = '1'; DebugMenuOwnBlock = '1' } }
          'pp_own_dbg'         = Join-Ini $pp @{ Reno = @{ NRFollowInputRes = '0' }; Extra = $rr + @('--debug-layer'); Ini = @{ MenuMode = '1'; DebugMenuOwnBlock = '1' } }
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
          # The model on a COMPUTE list on its own queue, as DLSS5-Reshade-AIO calls it.
          'off_t0_cl'         = @{ Mode = '0'; Temporal = '0'; Extra = $nr + $sf + @('--nr-list', 'compute') }
          'warp_t0_cl'        = @{ Mode = '2'; Temporal = '0'; Extra = $nr + $sf + @('--nr-list', 'compute') }
          'warp_t1_cl'        = @{ Mode = '2'; Temporal = '1'; Extra = $nr + $sf + @('--nr-list', 'compute') }
          # Colour and output in different formats (the sRGB proxy in R10G10B10A2, the output RGBA16F), as
          # DLSS5-Reshade-AIO hands them over: the temporal colour snapshot must keep its own view format.
          'warp_t1_r10'       = @{ Mode = '2'; Temporal = '1'; Extra = $nr + @('--mv-format', 'rgba16f', '--nr-proxy-format', 'r10g10b10a2') }
          'warp_t1_r10_cl'    = @{ Mode = '2'; Temporal = '1'; Extra = $nr + @('--mv-format', 'rgba16f', '--nr-proxy-format', 'r10g10b10a2', '--nr-list', 'compute') }
          # Detail transfer (plan 2026-09-27); run only when named with -Case, never by the release gate.
          'depthview'   = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Extra = @('--upscaler', 'sr', '--view', 'depth') + $sf }
          'off_t0_time' = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ DebugTiming = '1' } }
          'uni50_f1'    = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '1'; DebugTiming = '1' } }
          'uni50_f2'    = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '2'; DebugTiming = '1' } }
          'uni50_f3'    = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '3'; DebugTiming = '1' } }
          'uni70_f1'    = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ WorkX = '70'; WorkY = '70'; ColorFilter = '1'; DebugTiming = '1' } }
          'uni70_f3'    = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ WorkX = '70'; WorkY = '70'; ColorFilter = '3'; DebugTiming = '1' } }
          'uni85_f1'    = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ WorkX = '85'; WorkY = '85'; ColorFilter = '1'; DebugTiming = '1' } }
          'uni85_f3'    = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ WorkX = '85'; WorkY = '85'; ColorFilter = '3'; DebugTiming = '1' } }
          'per_f1'      = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ ColorFilter = '1'; DebugTiming = '1' } }
          'per_f3'      = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Extra = $nr + $sf; Ini = @{ ColorFilter = '3'; DebugTiming = '1' } }
          'uni50_f3_cl'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf + @('--nr-list', 'compute'); Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '3' } }
          'uni50_f3_t1'   = @{ OptIn = $true; Mode = '1'; Temporal = '1'; Extra = $nr + $sf; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '3' } }
          'uni50_f3_stdz' = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf + @('--depth', 'standard'); Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '3' } }
          'uni50_f3_dbg'  = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nr + $sf + @('--debug-layer'); Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '3' } }
          'per_f1_r10'    = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rgba16f', '--nr-proxy-format', 'r10g10b10a2'); Ini = @{ ColorFilter = '1' } }
          'per_f3_r10'    = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Extra = $nr + @('--mv-format', 'rgba16f', '--nr-proxy-format', 'r10g10b10a2'); Ini = @{ ColorFilter = '3' } }
          # The same detail-transfer cases behind Ray Reconstruction instead of SR (opt-in). rr_nonr: RR alone.
          'rr_nonr'       = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Extra = $rr + @('--mv-format', 'rgba16f') }
          'rr_depthview'  = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Extra = $rr + @('--view', 'depth') + $sf }
          'rr_off_t0_time' = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ DebugTiming = '1' } }
          'rr_uni50_f1'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '1'; DebugTiming = '1' } }
          'rr_uni50_f2'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '2'; DebugTiming = '1' } }
          'rr_uni50_f3'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '3'; DebugTiming = '1' } }
          'rr_uni70_f1'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ WorkX = '70'; WorkY = '70'; ColorFilter = '1'; DebugTiming = '1' } }
          'rr_uni70_f3'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ WorkX = '70'; WorkY = '70'; ColorFilter = '3'; DebugTiming = '1' } }
          'rr_uni85_f1'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ WorkX = '85'; WorkY = '85'; ColorFilter = '1'; DebugTiming = '1' } }
          'rr_uni85_f3'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ WorkX = '85'; WorkY = '85'; ColorFilter = '3'; DebugTiming = '1' } }
          'rr_per_f1'     = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ ColorFilter = '1'; DebugTiming = '1' } }
          'rr_per_f3'     = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Extra = $nrr + $sf; Ini = @{ ColorFilter = '3'; DebugTiming = '1' } }
          # Menu mode (opt-in): the bench stops calling feature 18 on the --nr-pause frames; the add-on runs the model on
          # the presented frame and writes menu_events.log and menu_dump_*.bmp (Marker: the red 64x64 square is expected
          # inside menus). DumpFrom: the bench dumps at the present indices of that case's menu_events.log (dump/probe
          # lines), so it must run earlier in the same invocation.
          'mp_off'        = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Extra = $nr + $mpPause; Ini = $mpOffIni }
          'mp_marker'     = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'marker'; Extra = $nr + $mpPause; Ini = (Join-Ini $mpModelIni @{ DebugMenuPass = '1' }) }
          'mp_fail'       = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'none'; Extra = $nr + $mpPause; Ini = (Join-Ini $mpModelIni @{ DebugMenuPass = '2' }) }
          'mp_marker_dbg' = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'marker'; Extra = $nr + $mpPause + @('--debug-layer'); Ini = (Join-Ini $mpModelIni @{ DebugMenuPass = '1' }) }
          'mp_exit_off'   = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 400; Dump = @('30'); Extra = $nr + @('--nr-pause', '380,420'); Ini = $mpOffIni }   # mp_exit's usual time
          'mp_exit'       = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 400; Dump = @('30'); Menu = 'marker'; Extra = $nr + @('--nr-pause', '380,420'); Ini = (Join-Ini $mpModelIni @{ DebugMenuPass = '1' }) }
          # C1: the feature is released (bench exit at 400) while model passes run on the private queue, which is
          # registered with the core; the log line at the release gives the private queue's last f2 and its completed value.
          'mp_exit_model' = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 400; Dump = @('30'); Menu = 'model'; Extra = $nr + $mpStatic + @('--nr-pause', '380,420') + $mpShow; Ini = $mpModelIni }
          'mp_model'        = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpModelIni }
          'mp_model_dbg'    = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow + @('--debug-layer'); Ini = $mpModelIni }
          'mp_model_motion' = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpMoving + $mpPause + $mpShow; Ini = $mpModelIni }
          # The watched feature released and created again (a game re-creating NR in its menu); the swap chain resized at
          # the same size (a fullscreen toggle), or to 1600x900 (a resolution change: renderer, DLSS and feature 18 rebuilt).
          'mp_recreate'     = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'lifetime'; Runs = 9; EventAt = 80; Event = 'recreate'; Extra = $mpLife + @('--nr-recreate', '80'); Ini = $mpModelIni }
          'mp_resize'       = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'lifetime'; Runs = 9; EventAt = 80; Event = 'resize'; Extra = $mpLife + @('--resize-at', '80');Ini = $mpModelIni }
          'mp_resize_size'  = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'lifetime'; Runs = 9; EventAt = 80; Event = 'resize'; Extra = $mpLife + @('--resize-at', '80,1600,900'); Ini = $mpModelIni }
          'mp_ref'          = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = $mpStaticModels; Extra = $nr + $mpStatic; Ini = $mpOffIni }
          'mp_off_model'    = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = $mpStaticModels; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpOffIni }
          'mp_frozen'       = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = 'mp_model'; Extra = $nr + $mpStatic + $mpPause; Ini = $mpOffIni }
          'mp_off_motion'   = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_motion'); Extra = $nr + $mpMoving + $mpPause + $mpShow; Ini = $mpOffIni }
          'mp_ref_motion'   = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_motion'); Extra = $nr + $mpMoving; Ini = $mpOffIni }
          # The back buffer in the host's NR colour encoding (no tone curve, no bloom): the menu model sees what
          # the host hands NR, 8-bit quantised.
          'mp_model_matched' = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow + $mpMatched; Ini = $mpModelIni }
          'mp_ref_matched'   = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_matched'); Extra = $nr + $mpStatic + $mpMatched; Ini = $mpOffIni }
          'mp_off_matched'   = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_matched'); Extra = $nr + $mpStatic + $mpPause + $mpShow + $mpMatched; Ini = $mpOffIni }
          # The host evaluates NR on a COMPUTE list on its own queue: the guide snapshots are recorded there.
          'mp_model_cl'      = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow + $mpCompute; Ini = $mpModelIni }
          'mp_ref_cl'        = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_cl'); Extra = $nr + $mpStatic + $mpCompute; Ini = $mpOffIni }
          'mp_off_cl'        = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_cl'); Extra = $nr + $mpStatic + $mpPause + $mpShow + $mpCompute; Ini = $mpOffIni }
          # Stage 3: the menu frame through the core with the user's layout (Uniform / Peripheral, no temporal mode).
          'mp_model_uni' = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpModelIni }
          'mp_ref_uni'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_uni'); Extra = $nr + $mpStatic; Ini = $mpOffIni }
          'mp_off_uni'   = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_uni'); Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpOffIni }
          # Control: the marker in menus (no model), so the exit Reset alone sets the after-exit frames (same indices).
          'mp_marker_uni' = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'marker'; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuPass = '1' }) }
          'mp_model_per' = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpModelIni }
          'mp_ref_per'   = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_per'); Extra = $nr + $mpStatic; Ini = $mpOffIni }
          'mp_off_per'   = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Frames = 660; DumpFrom = @('mp_model_per'); Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpOffIni }
          # C4: the watched feature released on a second thread during present 80 (a Peripheral menu run: the present's
          # menu pass calls into the core's feature), then created again; the 9 later pauses must get their passes.
          # Fix round 1: a setting changed at frame 80 inside a Peripheral menu run (Mode -> Off: a new model at the frame's
          # size; ModelPasses -> 2: an extra pass). The run ends with the settings blocker, no core call meets the change,
          # the game's next evaluate applies it, and the 9 later pauses get their passes.
          'mp_setmode_per'   = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'lifetime'; Runs = 9; EventAt = 80; Event = 'setting'; Extra = $mpLife + @('--set-at', '80,0,0'); Ini = $mpModelIni }
          'mp_setpasses_per' = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'lifetime'; Runs = 9; EventAt = 80; Event = 'setting'; Extra = $mpLife + @('--set-at', '80,20,2'); Ini = $mpModelIni }
          # Stage 3: the user's sync temporal mode in menus (TemporalEvery 4), Mode Off and Peripheral: the model every 4th
          # menu frame, the frames between carried along the optical flow (menu_events.log 'cadence' lines).
          'mp_model_t1'     = @{ OptIn = $true; Mode = '0'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpModelIni }
          'mp_ref_t1'       = @{ OptIn = $true; Mode = '0'; Temporal = '1'; Frames = 660; DumpFrom = @('mp_model_t1'); Extra = $nr + $mpStatic; Ini = $mpOffIni }
          'mp_off_t1'       = @{ OptIn = $true; Mode = '0'; Temporal = '1'; Frames = 660; DumpFrom = @('mp_model_t1'); Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpOffIni }
          'mp_model_per_t1' = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpModelIni }
          'mp_model_per_t1_dbg' = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow + @('--debug-layer'); Ini = $mpModelIni }
          'mp_ref_per_t1'   = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; DumpFrom = @('mp_model_per_t1'); Extra = $nr + $mpStatic; Ini = $mpOffIni }
          'mp_off_per_t1'   = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; DumpFrom = @('mp_model_per_t1'); Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpOffIni }
          # Controls: the marker in sync-cadence menus (no model), so the exit Reset and the cadence's restart alone set the
          # after-exit frames (compare with mp_ref_t1 / mp_ref_per_t1 at the same indices).
          'mp_marker_t1'     = @{ OptIn = $true; Mode = '0'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'marker'; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuPass = '1' }) }
          'mp_marker_per_t1' = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'marker'; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuPass = '1' }) }
          # C5: the swap chain destroyed and created again at frame 75 inside a Peripheral sync-cadence run (ends 'drain',
          # the optical flow switched back there), then the pause goes on and 9 later pauses get their passes.
          'mp_teardown_per'    = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'lifetime'; Runs = 9; EventAt = 75; Event = 'teardown'; Extra = $mpLife + @('--recreate-swapchain', '75'); Ini = $mpModelIni }
          'mp_teardown_per_t1' = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'lifetime'; Runs = 9; EventAt = 75; Event = 'teardown'; Extra = $mpLife + @('--recreate-swapchain', '75'); Ini = $mpModelIni }
          # C6 (DebugMenuNoFlow=1: as if the core had no optical flow): Mode Off runs the model on every menu frame (direct
          # pass, no 'cadence' line); Peripheral shows "unavailable: optical flow is not available" (no menu run).
          'mp_noflow_t1'     = @{ OptIn = $true; Mode = '0'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuNoFlow = '1' }) }
          'mp_noflow_per_t1' = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; Dump = @('30'); Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuNoFlow = '1' }) }
          # Fix round 1 (Codex I2): the flow fails once, on the first run's in-menu dump frame (a carried one); DebugMenuNoFlow=2
          # runs it again as a full model frame, 3 shows the last output again. Then C6: Mode Off runs the direct pass in every
          # later menu frame; Peripheral's run ends on the next present and no run enters again (the flow blocker).
          'mp_flowfail_t1'     = @{ OptIn = $true; Mode = '0'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'flowfail'; Event = 'full'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuNoFlow = '2' }) }
          'mp_flowfail_per_t1' = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'flowfail'; Event = 'full'; Runs = 1; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuNoFlow = '2' }) }
          'mp_flowlast_t1'     = @{ OptIn = $true; Mode = '0'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'flowfail'; Event = 'last'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuNoFlow = '3' }) }
          'mp_flowlast_per_t1' = @{ OptIn = $true; Mode = '2'; Temporal = '1'; Frames = 660; Dump = @('30'); Menu = 'flowfail'; Event = 'last'; Runs = 1; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuNoFlow = '3' }) }
          # The background temporal mode is not used in menus: Peripheral shows the blocker (no menu run), Mode Off runs the
          # direct pass on every menu frame.
          'mp_bg_per'       = @{ OptIn = $true; Mode = '2'; Temporal = '3'; Frames = 660; Dump = @('30'); Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpModelIni }
          'mp_model_t3'     = @{ OptIn = $true; Mode = '0'; Temporal = '3'; Frames = 660; Dump = @('30'); Menu = 'model'; Runs = 10; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = $mpModelIni }
          # Final review C1: menu mode off at 92, the model re-created while off (Mode Off -> Uniform at 94 -> Off at 97), menu
          # mode on again at 130 inside the open menu 120-150: no run may enter there on the old model's snapshot; the 8 later
          # pauses enter after a fresh host evaluate, each with its own pass.
          'mp_reenable'  = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'reenable'; Runs = 8; EventAt = 130; Extra = $mpLife + @('--set-at', '92,45,0,94,0,1,97,0,0,130,45,1'); Ini = $mpModelIni }
          'mp_race_per'  = @{ OptIn = $true; Mode = '2'; Temporal = '0'; Frames = 660; Dump = @('30'); Menu = 'lifetime'; Runs = 9; EventAt = 80; Event = 'race'; Extra = $mpLife + @('--nr-recreate', '80', '--nr-release-thread'); Ini = $mpModelIni }
          # Regression check of a failed f2 signal: forced at the 10th menu present of the first run (~73-75), before anything
          # is submitted (DebugMenuPass=3).
          'mp_sigfail'      = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Frames = 660; Dump = @('30') + @(68..84 | ForEach-Object { "$_" }); Menu = 'sigfail'; Extra = $nr + $mpStatic + $mpPause + $mpShow; Ini = (Join-Ini $mpModelIni @{ DebugMenuPass = '3' }) }
          'pp_ref'        = Join-Ini $pp @{ Extra = $nr + $sf; Ini = @{ MenuMode = '0' } }
          'pp_ref_repeat' = Join-Ini $pp @{ Extra = $nr + $sf; Ini = @{ MenuMode = '0' } }
          'pp_trace'      = Join-Ini $pp @{ Extra = $nr + $sf; Ini = @{ MenuMode = '1' } }
          'pp_own'        = Join-Ini $pp @{ Extra = $nr + $sf; Ini = @{ MenuMode = '1'; DebugMenuOwnBlock = '1' } }
          # Face close-up (hero_girl frozen at time 0, static camera), RR in front of the model (opt-in).
          'face_nonr'     = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Extra = $rr + @('--mv-format', 'rgba16f') + $face }
          'face_off'      = @{ OptIn = $true; Mode = '0'; Temporal = '0'; Extra = $nrr + $sf + $face; Ini = @{ DebugTiming = '1' } }
          'face_uni50_f1' = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf + $face; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '1'; DebugTiming = '1' } }
          'face_uni50_f2' = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf + $face; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '2'; DebugTiming = '1' } }
          'face_uni50_f3' = @{ OptIn = $true; Mode = '1'; Temporal = '0'; Extra = $nrr + $sf + $face; Ini = @{ WorkX = '50'; WorkY = '50'; ColorFilter = '3'; DebugTiming = '1' } }
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
      if ($cfg.ContainsKey('OptIn') -and $cfg.OptIn -and $Case -notcontains $name) { continue } # only when named
      if ($WarpPath -eq 'pixel' -and $name -like '*_cl') { "$name skipped: a COMPUTE list has no pixel path"; continue }
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
      Set-IniKey 'OptimizerFPS' 'DebugTiming' '0'
      Set-IniKey 'OptimizerFPS' 'MenuMode' '0'
      Set-IniKey 'OptimizerFPS' 'DebugMenuPass' '0'
      Set-IniKey 'OptimizerFPS' 'DebugMenuDump' '0'
      Set-IniKey 'OptimizerFPS' 'DebugMenuEntryMs' '150'
      Set-IniKey 'OptimizerFPS' 'DebugMenuOwnBlock' '0'
      Set-IniKey 'OptimizerFPS' 'DebugMenuNoFlow' '0'
      if ($cfg.ContainsKey('Ini')) { foreach ($k in $cfg.Ini.Keys) { Set-IniKey 'OptimizerFPS' $k $cfg.Ini[$k] } }
      if ($cfg.ContainsKey('Reno')) {                    # renodx runtimes; run_addon's 4.70 knows only NeuralUplift
          Set-IniKey 'RenoDX.DLSS5' 'NeuralUplift' '1'
          if ($kind -eq 'run_r521') { Set-IniKey 'RenoDX.DLSS5' 'NRPreUpscale' '0'; Set-IniKey 'RenoDX.DLSS5' 'NRResolutionScale' '100' }
          foreach ($k in $cfg.Reno.Keys) { Set-IniKey 'RenoDX.DLSS5' $k $cfg.Reno[$k] }
      }
      Remove-Item -LiteralPath (Join-Path $run 'optimizer-fps-dlss5.session') -ErrorAction SilentlyContinue
      Remove-Item -LiteralPath (Join-Path $run 'ReShade.log') -ErrorAction SilentlyContinue
      Remove-Item -LiteralPath (Join-Path $run 'ngx_final_audit.log') -ErrorAction SilentlyContinue
      Get-ChildItem -LiteralPath $run -Filter 'dump_*.bmp' | Remove-Item
      Clear-MenuOutput $run
      $iniCopy = Join-Path $out ($name + '_ReShade.ini')
      Copy-Item -LiteralPath $ini -Destination $iniCopy
      $caseFrames = if ($cfg.ContainsKey('Frames')) { $cfg.Frames } else { $Frames }
      $caseDumps = @(if ($cfg.ContainsKey('Dump')) { $cfg.Dump } else { $DumpFrames })   # @(): one frame stays an array
      if ($cfg.ContainsKey('DumpFrom')) { $caseDumps = @(Get-MenuDumpIndices $out $cfg.DumpFrom) }
      $argv = @("$caseFrames") + $scene + @('--dump', ($caseDumps -join ',')) + $cfg.Extra
      Write-Output ((@('pw_bench12.exe') + $argv) -join ' ')
      $started = Get-Date
      $p = Start-Process -FilePath (Join-Path $run 'pw_bench12.exe') -ArgumentList $argv -WorkingDirectory $run -Wait -PassThru -WindowStyle Hidden `
          -RedirectStandardOutput (Join-Path $out "$name.txt") -RedirectStandardError (Join-Path $out "$name.err.txt")
      $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
      foreach ($f in @($caseDumps | ForEach-Object { "dump_$_.bmp" })) {
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
      foreach ($frame in $caseDumps) {
          $dump = Join-Path $out ("{0}_dump_{1}.bmp" -f $name, $frame)
          $dumps[[string]$frame] = Test-Path -LiteralPath $dump -PathType Leaf
      }
      $pathMatches = $WarpPath -eq 'auto' -or $cfg.Mode -eq '0' -or $effectiveWarpPath -eq $WarpPath
      $ok = $dumps.Count -gt 0 -and ($dumps.Values -notcontains $false) -and ($p.ExitCode -eq 0) -and $stdout.Contains('device ok') -and (($cfg.Mode -eq '0') -or $warped) -and (($cfg.Temporal -eq '0') -or $ready) -and $pathMatches
      $menu = $null
      if ($cfg.ContainsKey('Menu')) {
          $entryMs = if ($cfg.ContainsKey('Ini') -and $cfg.Ini.ContainsKey('DebugMenuEntryMs')) { [int]$cfg.Ini.DebugMenuEntryMs } else { 150 }
          $eventAt = if ($cfg.ContainsKey('EventAt')) { $cfg.EventAt } else { -1 }
          $menu = Get-MenuCheck $run $out $name $cfg.Menu $caseDumps[0] $(if ($cfg.ContainsKey('Runs')) { $cfg.Runs } else { 0 }) $entryMs $eventAt $(if ($cfg.ContainsKey('Event')) { $cfg.Event } else { '' })
          $ok = $ok -and $menu.ok
          '{0,-20} menu: enter {1} exit {2} dumps {3} (missing {4}) in-menu red {5}/{6} after-exit red {7}/{8} forced-failure {9} probe identical {10} min enter ms {11} stop {12} stop frame untouched {13}' -f `
              $name, $menu.enter, $menu.exit, $menu.dumps, $menu.missing, $menu.in_menu_red, $menu.in_menu, $menu.after_exit_red, $menu.after_exit, $menu.forced_failure, $menu.probe_identical, $menu.min_enter_ms, $menu.stop, $menu.stop_untouched
          if ($menu.life -and $cfg.Menu -eq 'reenable') { '{0,-20} menu: re-enabled at present {1} logged {2}, runs on the old snapshot {3}; {4} later runs, {5} with their own pass' -f `
              $name, $eventAt, $menu.life.event_logged, $menu.life.stale_runs, $menu.life.later_runs, $menu.life.later_passes }
          elseif ($menu.life) { '{0,-20} menu: {1} at present {2} logged {3}, inside a run {4}; {5} later runs, {6} with their own pass' -f `
              $name, $cfg.Event, $eventAt, $menu.life.event_logged, $menu.life.spanning, $menu.life.later_runs, $menu.life.later_passes }
          if ($menu.flow_failure) { '{0,-20} menu: "{1}" ({2}x) shown at present {3}, first dump {4}; {5} runs, core frames after the first run {6}; flow owed {7}' -f `
              $name, $menu.flow_failure.redo, $menu.flow_failure.redo_count, $menu.flow_failure.shown_at, $menu.flow_failure.first_dump, $menu.flow_failure.runs,
              $menu.flow_failure.core_after, $menu.flow_owed }
      }
      $runs.Add([ordered]@{ name = $name; argv = $argv; working_directory = $run; command = ((@('pw_bench12.exe') + $argv) -join ' '); mode = $cfg.Mode; temporal = $cfg.Temporal; warp_path = $WarpPath; effective_warp_path = $effectiveWarpPath
                            ini = (Split-Path -Leaf $iniCopy); dumps = $dumps; exit = $p.ExitCode; seconds = $seconds; warped = $warped; ready = $ready; menu = $menu; ok = $ok })
      '{0,-20} exit {1} warped {2} ready {3} {4}s {5}' -f $name, $p.ExitCode, $warped, $ready, $seconds, $(if ($ok) { 'ok' } else { 'CHECK' })
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
